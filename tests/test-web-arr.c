/*
 * test-web-arr.c - "Migrate from Radarr": the connections, their test,
 * the reading (the job arr.read on a real queue) and the preview, with the
 * real shell on an ephemeral loopback port, a stand-in Radarr and a stand-in
 * details provider on loopback, a real database, and a real library folder
 * with its index
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (bring-over): Only GET; A request of another kind; Never
 * shown again; In no log line; Forgotten (the key and the plans); A viewer;
 * An address on the home network; Not Radarr; A redirect elsewhere; Nothing
 * changed; Skipped, with reasons; Disk offline; Another mount; The wrong
 * folder; Not mapped; Found at its path; Not seen; Linked elsewhere; Radarr
 * stops answering; Thousands of movies; Too large; An incomplete title.
 * The scenarios are those of openspec/specs/arr-migration/spec.md.
 *
 * Slice R3 (the tests under /web-arr/run, with the movies module as axisd
 * has it): Monitored movies; Unmonitored, when asked; Changed since; Nothing
 * touched; A thousand missing movies; Started by a person; Radarr is
 * downloading it; Still monitored there; Twice; A disk that came back;
 * Changed in Axis since; Interrupted; and, of the movies and
 * library-identification capabilities, Held through the scheduler; One
 * movie, by a person; A file Axis does not see; Shown; The run reads the
 * folder otherwise.  Task 3.9.
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
#include <sys/stat.h>

#include "arr/axis-arr.h"
#include "arr/axis-arr-private.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-plex.h"
#include "axis-test-radarr.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-http.h"
#include "core/axis-module-state.h"
#include "db/axis-db-migrate.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-arr.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

/* Made up: no Radarr has this key. */
#define KEY       "radarr-key-7f3a9c1e5b2d4f60"
#define OTHER_KEY "radarr-key-0000aaaa1111bbbb"

typedef struct
{
	AxisTestLibrary     *t;
	gchar               *disk;
	gchar               *films_dir;
	gint64               volume_id;
	gint64               root_id;
	AxisDataDir         *data;
	AxisSettings        *settings;
	AxisWebSettings     *web_settings;
	AxisTestMetaFixture *fa;
	AxisMeta            *meta;
	AxisWebMeta         *meta_ui;
	/* The movies module, which a run adds its movies through. */
	AxisIndexers        *indexers;
	AxisWebIndexers     *indexers_ui;
	AxisDownloads       *downloads;
	AxisWebDownloads    *downloads_ui;
	AxisQualityStore    *quality;
	AxisWebQuality      *quality_ui;
	AxisIdentify        *identify;
	AxisMovies          *movies;
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

/* ── The stand-in details provider ──────────────────────────────── */

static const struct
{
	const gchar *tmdb;
	const gchar *title;
	gint         year;
} known[] = {
	{ "6001", "Paper Lanterns", 2011 },
	{ "6002", "The Quiet Orchard", 2004 },
	{ "6005", "Nine Winters", 2022 },
	{ "6006", "Ash and Ember", 1996 },
	{ "6007", "Tin Sparrow", 2001 },
	{ "6008", "Low Tide Radio", 2015 },
	{ "6009", "Copper Kettle", 2003 },
	{ "6010", "Marrow Lane", 1990 },
	{ "6012", "Window Seat", 1979 },
	{ "6014", "Harbour Lights", 1988 },
	{ NULL, NULL, 0 }
};

/* The provider's answers: a film by its TMDb identifier, nothing for one it
 * does not know, and an error for 6011 (a provider that does not answer);
 * for identification, a search by title and a film's full record. */
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

	if (g_str_has_prefix(request->path, "/search/film")) {
		const gchar *q = query != NULL ? g_hash_table_lookup(query, "q") : NULL;
		g_autofree gchar *want = q != NULL ? g_utf8_strdown(q, -1) : g_strdup("");
		guint n = 0;

		for (i = 0; known[i].tmdb != NULL; i++) {
			g_autofree gchar *title = g_utf8_strdown(known[i].title, -1);

			if (want[0] == '\0' || strstr(title, want) == NULL)
				continue;
			g_string_append_printf(json, "%s{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
				"\"ids\":{\"tmdb\":\"%s\"}}", n++ > 0 ? "," : "", known[i].tmdb, known[i].title, known[i].year,
				known[i].tmdb);
		}
		g_string_append(json, "]}");
		answer->status = 200;
		answer->content_type = g_strdup("application/json");
		answer->body = g_bytes_new(json->str, json->len);
		return;
	}
	if (g_str_has_prefix(request->path, "/title/film/")) {
		const gchar *id = request->path + strlen("/title/film/");

		for (i = 0; known[i].tmdb != NULL; i++) {
			if (strcmp(known[i].tmdb, id) == 0) {
				g_string_truncate(json, 0);
				g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
					"\"ids\":{\"tmdb\":\"%s\"},\"runtime\":100,\"status\":\"released\",\"release_date\":\"%d-06-01\"}",
					known[i].tmdb, known[i].title, known[i].year, known[i].tmdb, known[i].year);
				answer->status = 200;
				answer->content_type = g_strdup("application/json");
				answer->body = g_bytes_new(json->str, json->len);
				return;
			}
		}
		answer->status = 404;
		return;
	}
	if (!g_str_has_prefix(request->path, "/find/film") || scheme == NULL || value == NULL) {
		answer->status = 404;
		return;
	}
	if (strcmp(value, "6011") == 0) {
		answer->status = 500;
		return;
	}
	for (i = 0; known[i].tmdb != NULL; i++) {
		if (strcmp(scheme, "tmdb") == 0 && strcmp(known[i].tmdb, value) == 0)
			g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
				"\"ids\":{\"tmdb\":\"%s\"}}", known[i].tmdb, known[i].title, known[i].year, known[i].tmdb);
	}
	g_string_append(json, "]}");
	answer->status = 200;
	answer->content_type = g_strdup("application/json");
	answer->body = g_bytes_new(json->str, json->len);
}

/* ── The fixture ────────────────────────────────────────────────── */

static void
write_file(
	Fixture     *fx,
	const gchar *folder,
	const gchar *name,
	const gchar *content
){
	g_autofree gchar *dir = g_build_filename(fx->films_dir, folder, NULL);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	axis_test_write_named(dir, name, content);
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
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->films_dir = g_build_filename(fx->disk, "films", NULL);
	/* What is on the disk, as Axis sees it below its library folder. */
	write_file(fx, "Paper Lanterns (2011)", "Paper Lanterns (2011).mkv", "lanterns123");
	write_file(fx, "Nine Winters (2022)", "Nine Winters (2022).mkv", "short");
	write_file(fx, "elsewhere", "Copper Kettle (2003).mkv", "kettle123");
	write_file(fx, "Marrow Lane (1990)", "Marrow Lane (1990).mkv", "marrow1");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	while (g_main_context_iteration(NULL, FALSE))
		;
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	/* The movies module as axisd has it, with the layers it joins; nothing
	 * here has an indexer or a download client to ask. */
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_movies_set_identify(fx->movies, fx->identify);
	g_assert_no_error(error);

	fx->radarr = axis_test_radarr_new(KEY);
	fx->arr = axis_arr_new(fx->t->pool, fx->meta);
	axis_arr_set_movies(fx->arr, fx->movies);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.list_timeout_ms = 20000;
	tuning.lookup_wait_ms = 5000;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);
	fx->ui = axis_web_arr_new(fx->arr);

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
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_arr_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IDENTIFY_MODULE_ID, axis_identify_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
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
	fx->session = soup_session_new_with_options("timeout", 120, "idle-timeout", 120, NULL);
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
	g_clear_object(&fx->queue);
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_arr_free(fx->ui);
	axis_arr_unref(fx->arr);
	axis_test_radarr_free(fx->radarr);
	axis_identify_unref(fx->identify);
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
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->films_dir);
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

/* A connection to the stand-in (or to @address), made through the layer. */
static gint64
add_source(
	Fixture     *fx,
	const gchar *name,
	const gchar *address,
	const gchar *key
){
	g_autofree gchar *base = axis_test_radarr_base(fx->radarr);
	g_autoptr(GError) error = NULL;
	AxisArrSourceSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.app = "radarr";
	spec.name = name;
	spec.address = address != NULL ? address : base;
	spec.secret = key;
	spec.user = "root";
	g_assert_true(axis_arr_save_source(fx->arr, 0, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* The queue that runs arr.read, as axisd has it. */
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
	/* The run, too. */
	g_assert_cmpstr(module->job_handlers[2].kind, ==, AXIS_ARR_APPLY_KIND);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[2], fx->ui, &error));
	g_assert_no_error(error);
	fx->queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

/* "Read what Radarr has", from the page; waits until the plan is @state and
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

		g_error("the reading did not become %s: the plan is %s, the job says %s", state, now, why);
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

/* One movie as Radarr lists it, appended to @json (an array being written). */
static void
add_movie(
	GString     *json,
	gint64       id,
	const gchar *title,
	gint         year,
	gint64       tmdb,
	gboolean     monitored,
	gint64       profile,
	const gchar *root,
	gint64       file_size
){
	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"title\":\"%s\",\"year\":%d,\"tmdbId\":%"
		G_GINT64_FORMAT ",\"monitored\":%s,\"qualityProfileId\":%" G_GINT64_FORMAT ",\"minimumAvailability\":"
		"\"released\",\"path\":\"%s/%s (%d)\",\"rootFolderPath\":\"%s\",\"hasFile\":%s", json->len > 1 ? "," : "",
		id, title, year, tmdb, monitored ? "true" : "false", profile, root, title, year, root,
		file_size >= 0 ? "true" : "false");
	if (file_size >= 0)
		g_string_append_printf(json, ",\"movieFile\":{\"path\":\"%s/%s (%d)/%s (%d).mkv\",\"size\":%"
			G_GINT64_FORMAT ",\"quality\":{\"quality\":{\"name\":\"Bluray-1080p\"}}}", root, title, year, title, year,
			file_size);
	g_string_append_c(json, '}');
}

/*
 * A library with one of everything the preview tells apart: thirteen
 * entries, in this order (the one the provider does not answer for last).
 */
static void
odd_library_with(
	Fixture  *fx,
	gboolean  one_more
){
	g_autoptr(GString) json = g_string_new("[");
	static const gint64 queued[] = { 2 };

	add_movie(json, 1, "Paper Lanterns", 2011, 6001, TRUE, 4, "/movies", 11);
	add_movie(json, 2, "The Quiet Orchard", 2004, 6002, FALSE, 4, "/movies", -1);
	add_movie(json, 3, "Salt Meridian", 2019, 0, TRUE, 4, "/movies", -1);
	add_movie(json, 4, "Glass Harbour", 1987, 6004, TRUE, 4, "/movies", -1);
	add_movie(json, 5, "Ash and Ember", 1996, 6006, TRUE, 4, "/movies", -1);
	add_movie(json, 6, "Tin Sparrow", 2001, 6007, TRUE, 6, "/movies", -1);
	add_movie(json, 7, "Low Tide Radio", 2015, 6008, TRUE, 4, "/movies-4k", -1);
	/* No id: Radarr's record of it is incomplete. */
	g_string_append(json, ",{\"title\":\"Hollow Compass\",\"year\":1999,\"tmdbId\":6013,\"monitored\":true,"
		"\"qualityProfileId\":4,\"path\":\"/movies/Hollow Compass (1999)\",\"rootFolderPath\":\"/movies\"}");
	add_movie(json, 9, "Nine Winters", 2022, 6005, TRUE, 4, "/movies", 22);
	add_movie(json, 10, "Copper Kettle", 2003, 6009, TRUE, 4, "/movies", 9);
	add_movie(json, 11, "Marrow Lane", 1990, 6010, TRUE, 4, "/movies", 7);
	/* A path with a ".." segment: never resolved. */
	g_string_append(json, ",{\"id\":12,\"title\":\"Window Seat\",\"year\":1979,\"tmdbId\":6012,\"monitored\":true,"
		"\"qualityProfileId\":4,\"path\":\"/movies/../etc/Window Seat (1979)\",\"rootFolderPath\":\"/movies\","
		"\"hasFile\":false}");
	/* A movie added in Radarr after a first run (before the one the provider
	 * fails for, after which it is not asked for a while). */
	if (one_more)
		add_movie(json, 14, "Harbour Lights", 1988, 6014, TRUE, 4, "/movies", -1);
	add_movie(json, 13, "Tall Grass", 2008, 6011, TRUE, 4, "/movies", -1);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"},{\"id\":6,\"name\":\"Ultra-HD\"}]");
	axis_test_radarr_set_folders(fx->radarr, "[{\"path\":\"/movies\",\"accessible\":true},"
		"{\"path\":\"/movies-4k\",\"accessible\":true}]");
	axis_test_radarr_set_queue(fx->radarr, queued, G_N_ELEMENTS(queued));
	/* The provider's failure for the last one is said in the log. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
}

static void
odd_library(Fixture *fx)
{
	odd_library_with(fx, FALSE);
}

/* Maps Radarr's /movies to the library folder @root, from the page. */
static AxisTestReply
map_movies(
	Fixture     *fx,
	gint64       source,
	gint64       root,
	const gchar *more
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	/* (Radarr's profile 6 has no namesake in Axis: left to itself it would
	 * be set to Axis's default profile, so "none" is chosen for it by hand,
	 * and the movie that uses it stays skipped for its profile.) */
	g_autofree gchar *form = g_strdup_printf("profile_ids=6&profile_6=0&folder_rows=1&folder_prefix_0=%%2Fmovies"
		"&folder_root_0=%" G_GINT64_FORMAT "&folder_below_0=%s", root, more != NULL ? more : "");

	return post_as(fx, fx->admin, page, page, form);
}

/* Everything a reading and a preview must leave as it was. */
static gchar *
library_digest(Fixture *fx)
{
	return axis_test_db_query(fx->t->db,
		"SELECT md5("
		" coalesce((SELECT string_agg(m::text, '|' ORDER BY m.id) FROM movie m), '-') ||"
		" coalesce((SELECT string_agg(f::text, '|' ORDER BY f.file_id) FROM movie_file f), '-') ||"
		" coalesce((SELECT string_agg(b::text, '|') FROM movie_blocklist b), '-') ||"
		" coalesce((SELECT string_agg(g::text, '|') FROM movie_grab g), '-') ||"
		" coalesce((SELECT string_agg(s::text, '|') FROM movie_search s), '-') ||"
		" coalesce((SELECT string_agg(e::text, '|') FROM movie_event e), '-') ||"
		" coalesce((SELECT string_agg(s::text, '|' ORDER BY s.key) FROM setting s), '-') ||"
		" coalesce((SELECT string_agg(f.id::text || f.rel_path || f.size::text || f.state, '|' ORDER BY f.id)"
		"   FROM library_file f), '-') ||"
		" (SELECT count(*) FROM series)::text || (SELECT count(*) FROM series_file)::text)");
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
		" (SELECT count(*) FROM arr_plan_item i WHERE to_jsonb(i)::text LIKE '%%%s%%')", key, key, key);
	g_assert_cmpstr(rows, ==, "0");
	for (l = 0; l < G_N_ELEMENTS(levels); l++)
		g_assert_cmpuint(axis_test_log_count(levels[l], pattern), ==, 0);
}

/* ── The connections (R1) ───────────────────────────────────────── */

/* Spec: A viewer */
static void
test_administrators_only(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	static const gchar *const pages[] = { "/migrate", "/migrate/1", "/migrate/1/preview",
		"/migrate/1/status", "/migrate/1/brought", "/migrate/1/start", NULL };
	static const gchar *const posts[] = { "/migrate", "/migrate/1", "/migrate/1/test", "/migrate/1/read",
		"/migrate/1/forget", "/migrate/1/preview", "/migrate/1/run", "/migrate/1/start", NULL };
	AxisTestReply home_admin = get_as(fx, fx->admin, "/");
	AxisTestReply home_viewer = get_as(fx, fx->viewer, "/");
	AxisTestReply admin = get_as(fx, fx->admin, "/migrate");
	g_autofree gchar *sources = NULL;
	g_autofree gchar *plans = NULL;
	guint i;

	g_assert_cmpint(source, ==, 1);
	for (i = 0; pages[i] != NULL; i++) {
		AxisTestReply viewer = get_as(fx, fx->viewer, pages[i]);
		AxisTestReply nobody = get_as(fx, NULL, pages[i]);

		g_assert_cmpuint(viewer.status, ==, 403);
		assert_lacks(viewer.body, "Radarr");
		g_assert_cmpuint(nobody.status, !=, 200);
		axis_test_reply_clear(&viewer);
		axis_test_reply_clear(&nobody);
	}
	for (i = 0; posts[i] != NULL; i++) {
		/* With the viewer's own token from a page of theirs: still refused. */
		AxisTestReply viewer = post_as(fx, fx->viewer, "/", posts[i], "name=x&confirm=yes&app=radarr"
			"&address=http%3A%2F%2Fradarr.example%3A7878&secret=" OTHER_KEY);

		g_assert_cmpuint(viewer.status, ==, 403);
		axis_test_reply_clear(&viewer);
	}
	/* Nothing was read or changed. */
	sources = axis_test_db_query(fx->t->db, "SELECT count(*) || ':' || min(name) FROM arr_source");
	g_assert_cmpstr(sources, ==, "1:Radarr");
	plans = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan");
	g_assert_cmpstr(plans, ==, "0");
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr)), ==, 0);

	/* In the navigation for administrators only. */
	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "href=\"/migrate\"");
	assert_contains(admin.body, ">Migrate</a>");
	g_assert_cmpuint(home_admin.status, ==, 200);
	assert_lacks(home_viewer.body, "href=\"/migrate\"");
	axis_test_reply_clear(&home_admin);
	axis_test_reply_clear(&home_viewer);
	axis_test_reply_clear(&admin);
}

/* A bookmark of the pages' old name, Bring over: a permanent redirect to the
 * same path under /migrate, for a page (GET) and not for a post. */
static void
test_old_address_redirects(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *from;
		const gchar *to;
	} pages[] = {
		{ "/bring-over", "/migrate" },
		{ "/bring-over/1", "/migrate/1" },
		{ "/bring-over/1/preview", "/migrate/1/preview" },
		{ "/bring-over/1/brought", "/migrate/1/brought" },
	};
	AxisTestReply posted;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		AxisTestReply reply = get_as(fx, fx->admin, pages[i].from);

		g_assert_cmpuint(reply.status, ==, 301);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, pages[i].to);
		axis_test_reply_clear(&reply);
	}
	posted = post_as(fx, fx->admin, "/migrate", "/bring-over", "name=x&confirm=yes");
	g_assert_cmpuint(posted.status, !=, 301);
	g_assert_cmpuint(posted.status, !=, 303);
	axis_test_reply_clear(&posted);
}

/* Spec: Never shown again; An address on the home network; Forgotten */
static void
test_add_edit_forget(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_radarr_base(fx->radarr);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("app=radarr&name=Living+room+Radarr&address=%s%%2Fradarr%%2F&secret=%s",
		escaped, KEY);
	static const struct
	{
		const gchar *form;
		const gchar *says;
	} refused[] = {
		{ "app=radarr&name=Second&address=ftp%3A%2F%2Fradarr.example&secret=" OTHER_KEY,
		  "The address must start with http:// or https://" },
		{ "app=radarr&name=Second&address=http%3A%2F%2Fuser%3Aword%40radarr.example%3A7878&secret=" OTHER_KEY,
		  "The address must not hold a user name or a password." },
		{ "app=radarr&name=Second&address=http%3A%2F%2Fradarr.example%3A7878%2F%3Fapikey%3D" OTHER_KEY "&secret="
		  OTHER_KEY, "The address must not hold a query" },
		{ "app=radarr&name=Second&address=http%3A%2F%2Fradarr.example%3A7878%2F..%2Fadmin&secret=" OTHER_KEY,
		  "The path after the host may hold only" },
		{ "app=radarr&name=Second&address=http%3A%2F%2Fradarr.example%3A7878&secret=", "The API key is missing." },
		{ "app=radarr&name=Second&address=http%3A%2F%2Fradarr.example%3A7878&secret=two+words",
		  "The API key must be 1 to 1024 characters without spaces." },
		{ "app=lidarr&name=Second&address=http%3A%2F%2Flidarr.example%3A8686&secret=" OTHER_KEY,
		  "Choose the app: Radarr or Sonarr." },
		{ "app=radarr&name=&address=http%3A%2F%2Fradarr.example%3A7878&secret=" OTHER_KEY,
		  "The name must be 1 to 64 characters." },
		{ "app=radarr&name=Living+room+Radarr&address=http%3A%2F%2Fradarr.example%3A7878&secret=" OTHER_KEY,
		  "Another connection already has this name." }
	};
	AxisTestReply empty = get_as(fx, fx->admin, "/migrate");
	AxisTestReply added;
	AxisTestReply page;
	AxisTestReply list;
	AxisTestReply saved;
	AxisTestReply moved;
	AxisTestReply unconfirmed;
	AxisTestReply forgotten;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *left = NULL;
	guint i;

	g_assert_cmpuint(empty.status, ==, 200);
	assert_csp_clean(empty.body);
	assert_contains(empty.body, "Migrate from Radarr");
	assert_contains(empty.body, "No connection has been added yet.");
	assert_contains(empty.body, "Axis only reads from Radarr and Sonarr: it changes nothing there.");
	assert_contains(empty.body, "type=\"password\"");
	g_assert_cmpuint(axis_test_web_assert_tables(empty.body), ==, 0);
	save_page("list-empty.html", empty.body);
	axis_test_reply_clear(&empty);

	/* A loopback address with a URL base: accepted, as any address the
	 * server can reach is. */
	added = post_as(fx, fx->admin, "/migrate", "/migrate", form);
	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/migrate/1?done=added");
	axis_test_reply_clear(&added);
	stored = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_source_secret WHERE secret = '" KEY "'");
	g_assert_cmpstr(stored, ==, "1");
	row = axis_test_db_queryf(fx->t->db, "SELECT app || ' ' || name || ' ' || (origin = '%s') || ' ' || path_prefix"
		" || ' ' || made_by FROM arr_source WHERE id = 1", base);
	g_assert_cmpstr(row, ==, "radarr Living room Radarr true /radarr root");

	/* Refused: the form again with what was typed, never the key, and a
	 * reason in fixed words. */
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		AxisTestReply reply = post_as(fx, fx->admin, "/migrate", "/migrate", refused[i].form);

		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, refused[i].says);
		assert_lacks(reply.body, OTHER_KEY);
		assert_lacks(reply.body, "two words");
		assert_csp_clean(reply.body);
		axis_test_reply_clear(&reply);
	}
	left = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_source");
	g_assert_cmpstr(left, ==, "1");

	/* The connection's page: a key is stored, and no part of it is there. */
	page = get_as(fx, fx->admin, "/migrate/1?done=added");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "The connection has been added.");
	assert_contains(page.body, "API key (stored; type a new one to replace it)");
	assert_contains(page.body, "stored (never shown)");
	assert_contains(page.body, "Not tested");
	assert_contains(page.body, "Test the connection");
	assert_contains(page.body, "Read what Radarr has");
	assert_contains(page.body, "Nothing has been read yet.");
	assert_contains(page.body, "Axis only reads from Radarr. It changes nothing there.");
	/* What forgetting costs, said where it is confirmed. */
	assert_contains(page.body, "Axis also forgets which movies you removed after they were migrated, so a later "
		"connection to the same Radarr can migrate them again.");
	assert_lacks(page.body, KEY);
	assert_lacks(page.body, "radarr-key");
	save_page("source.html", page.body);
	axis_test_reply_clear(&page);
	list = get_as(fx, fx->admin, "/migrate");
	assert_contains(list.body, "Living room Radarr");
	assert_lacks(list.body, KEY);
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	axis_test_reply_clear(&list);

	/* Renamed without typing the key: it is kept. */
	g_free(form);
	form = g_strdup_printf("app=radarr&name=Radarr&address=%s%%2Fradarr&secret=", escaped);
	saved = post_as(fx, fx->admin, "/migrate/1", "/migrate/1", form);
	g_assert_cmpuint(saved.status, ==, 303);
	axis_test_reply_clear(&saved);
	g_free(stored);
	stored = axis_test_db_query(fx->t->db, "SELECT a.name || ' ' || s.secret FROM arr_source a JOIN arr_source_secret s"
		" ON s.source_id = a.id");
	g_assert_cmpstr(stored, ==, "Radarr " KEY);

	/* Another address without the key typed again: refused.  A stored key
	 * is only ever sent to the address it was stored for. */
	moved = post_as(fx, fx->admin, "/migrate/1", "/migrate/1",
		"app=radarr&name=Radarr&address=http%3A%2F%2Fother.example%3A7878&secret=");
	g_assert_cmpuint(moved.status, ==, 400);
	assert_contains(moved.body, "Type the API key again: the address changed");
	assert_lacks(moved.body, KEY);
	axis_test_reply_clear(&moved);
	g_free(row);
	row = axis_test_db_queryf(fx->t->db, "SELECT (origin = '%s') FROM arr_source WHERE id = 1", base);
	g_assert_cmpstr(row, ==, "t");

	/* Forgetting needs its tick; then the connection, its key and its plans
	 * are gone. */
	axis_test_db_exec(fx->t->db, "INSERT INTO arr_plan (source_id, state, finished_at) VALUES (1, 'complete', now())");
	axis_test_db_exec(fx->t->db, "INSERT INTO arr_plan_item (plan_id, seq, title, match) SELECT id, 1, 'Paper Lanterns',"
		" 'no_id' FROM arr_plan");
	unconfirmed = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/forget", NULL);
	g_assert_cmpuint(unconfirmed.status, ==, 400);
	axis_test_reply_clear(&unconfirmed);
	forgotten = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/forget", "confirm=yes");
	g_assert_cmpuint(forgotten.status, ==, 303);
	assert_contains(axis_test_reply_header(&forgotten, "Location"), "/migrate?done=forgotten");
	axis_test_reply_clear(&forgotten);
	g_free(left);
	left = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_source) + (SELECT count(*) FROM"
		" arr_source_secret) + (SELECT count(*) FROM arr_plan) + (SELECT count(*) FROM arr_plan_item)");
	g_assert_cmpstr(left, ==, "0");
	page = get_as(fx, fx->admin, "/migrate/1");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);

	/* Nothing was asked of Radarr by any of this. */
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr)), ==, 0);
	/* The audit lines name the connection, never its key or its address. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*migrate: connection 1 added*") +
		axis_test_log_count(G_LOG_LEVEL_INFO, "*migrate: connection 1 added*"), >=, 1);
	assert_secret_nowhere(fx, KEY);
	assert_secret_nowhere(fx, OTHER_KEY);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*migrate:*/radarr*") +
		axis_test_log_count(G_LOG_LEVEL_INFO, "*migrate:*/radarr*"), ==, 0);
}

/* The test of connection @source from its page; the page afterwards. */
static AxisTestReply
test_from_page(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT, source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/test", source);
	AxisTestReply posted = post_as(fx, fx->admin, page, action, NULL);

	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=tested");
	axis_test_reply_clear(&posted);
	return get_as(fx, fx->admin, page);
}

/* Spec: Not Radarr; A redirect elsewhere; Only GET.  Task 1.6: a refused
 * key, an address that answers as another app, a redirect to another origin
 * not followed. */
static void
test_the_connection(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *server = axis_test_radarr_server(fx->radarr);
	AxisTestPlex *elsewhere = axis_test_plex_new();
	g_autofree gchar *base = axis_test_radarr_base(fx->radarr);
	g_autofree gchar *elsewhere_base = axis_test_plex_base(elsewhere);
	g_autofree gchar *elsewhere_status = g_strconcat(elsewhere_base, "/api/v3/system/status", NULL);
	g_autofree gchar *under = g_strconcat(base, "/radarr", NULL);
	g_autofree gchar *row = NULL;
	gint64 good = add_source(fx, "Good", NULL, KEY);
	gint64 bad_key = add_source(fx, "Bad key", NULL, OTHER_KEY);
	/* Nothing listens on port 1 of this machine. */
	gint64 nowhere = add_source(fx, "Nowhere", "http://127.0.0.1:1", KEY);
	gint64 prefixed = add_source(fx, "Under a base", under, KEY);
	AxisTestReply page;

	/* A good key: the app and its version. */
	page = test_from_page(fx, good);
	assert_contains(page.body, "Radarr 5.2.6.8376 answered.");
	assert_contains(page.body, "Answers");
	assert_lacks(page.body, KEY);
	save_page("source-tested.html", page.body);
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/system/status"), ==, 1);
	/* Nothing else was asked: only the answer that names the app. */
	g_assert_cmpuint(axis_test_plex_n_requests(server), ==, 1);

	/* A refused key. */
	page = test_from_page(fx, bad_key);
	assert_contains(page.body, "Radarr refused the API key.");
	assert_contains(page.body, "Key refused");
	assert_lacks(page.body, OTHER_KEY);
	assert_lacks(page.body, "Unauthorized");
	axis_test_reply_clear(&page);

	/* No answer. */
	page = test_from_page(fx, nowhere);
	assert_contains(page.body, "No answer from this address.");
	axis_test_reply_clear(&page);

	/* Under a URL base: the prefix goes before every path. */
	axis_test_radarr_set_prefix(fx->radarr, "/radarr");
	page = test_from_page(fx, prefixed);
	assert_contains(page.body, "Radarr 5.2.6.8376 answered.");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/system/status"), ==, 1);
	/* The same Radarr asked at its top answers, but not as Radarr's API:
	 * nothing of the answer is shown. */
	page = test_from_page(fx, good);
	assert_contains(page.body, "This address answers, but not as Radarr.");
	assert_contains(page.body, "Not Radarr");
	assert_lacks(page.body, "NotFound");
	assert_lacks(page.body, "5.2.6");
	axis_test_reply_clear(&page);
	axis_test_radarr_set_prefix(fx->radarr, NULL);

	/* Another app answers: said so, its own name and version not shown, and
	 * nothing more asked of it. */
	axis_test_radarr_set_app(fx->radarr, "Sonarr");
	axis_test_plex_reset_requests(server);
	page = test_from_page(fx, good);
	assert_contains(page.body, "This address answers, but not as Radarr.");
	assert_lacks(page.body, "Sonarr");
	assert_lacks(page.body, "5.2.6");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(server), ==, 1);
	axis_test_radarr_set_app(fx->radarr, NULL);

	/* A redirect to another host: not followed, and the key is not sent
	 * there. */
	axis_test_plex_route(server, "/api/v3/system/status", -1, AXIS_TEST_PLEX_REDIRECT, 302, elsewhere_status, 0);
	page = test_from_page(fx, good);
	assert_contains(page.body, "This address answers, but not as Radarr.");
	assert_lacks(page.body, elsewhere_base);
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(elsewhere), ==, 0);
	axis_test_plex_clear_routes(server);

	/* What the last test found is on the connection. */
	row = axis_test_db_query(fx->t->db, "SELECT string_agg(name || '=' || coalesce(last_test, '-'), ' ' ORDER BY id)"
		" FROM arr_source");
	g_assert_cmpstr(row, ==, "Good=not_app Bad key=refused Nowhere=no_answer Under a base=ok");

	/* Every request Radarr ever received was a GET, with the key in its
	 * header only. */
	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
	assert_secret_nowhere(fx, OTHER_KEY);
	axis_test_plex_free(elsewhere);
}

/* Spec: A request of another kind.  The connection's session is asked to
 * POST: it fails, and nothing is sent. */
static void
test_not_a_get_fails_without_sending(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(AxisArrSource) source = NULL;
	g_autoptr(GBytes) body = g_bytes_new_static("{\"monitored\":false}", 19);
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GError) error = NULL;
	AxisHttpResponse response;
	AxisArrClient *client;

	g_assert_true(axis_arr_get_source(fx->arr, id, &source, &error));
	client = axis_arr_client_new(fx->arr, source, KEY, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(client);
	g_assert_cmpint(axis_http_session_get_purpose(axis_arr_client_peek_session(client)), ==, AXIS_HTTP_PURPOSE_READ);
	g_assert_false(axis_http_session_post(axis_arr_client_peek_session(client), "/api/v3/movie/editor",
		"application/json", body, NULL, &answer, &response, &error));
	g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONFIG);
	g_assert_false(response.sent);
	g_assert_null(answer);
	axis_http_response_clear(&response);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr)), ==, 0);
	g_assert_cmpuint(axis_test_radarr_n_non_get(fx->radarr), ==, 0);

	/* A GET through the same client arrives, with the key in its header. */
	g_clear_error(&error);
	g_assert_true(axis_arr_client_get(client, "/api/v3/system/status", AXIS_ARR_GET_SMALL, &answer, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr)), ==, 1);
	axis_test_radarr_assert_read_only(fx->radarr);
	axis_arr_client_free(client);
}

/* ── The reading and the preview (R2) ───────────────────────────── */

/* Spec: Nothing changed; Skipped, with reasons; Another mount; The wrong
 * folder; Not mapped; Found at its path; Not seen; Linked elsewhere; An
 * incomplete title.  Task 2.5: opening and changing the preview leaves
 * every movie, link, blocklist and setting as it was; each skip reason; a
 * folder row mapped to the wrong library folder shows 0 found; a path with
 * ".." refused. */
static void
test_read_and_preview(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	gint64 other_root;
	g_autoptr(AxisScanResult) scan = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *items = NULL;
	g_autofree gchar *job = NULL;
	g_autofree gchar *matches = NULL;
	g_autofree gchar *mappings = NULL;
	g_autofree gchar *managed_href = NULL;
	g_autofree gchar *wrong = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	AxisTestReply status;
	guint finds;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	items = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item");
	g_assert_cmpstr(items, ==, "13");
	job = axis_test_db_query(fx->t->db, "SELECT state || ' ' || attempts || ' ' || (payload::text NOT LIKE '%" KEY "%')"
		" FROM job WHERE kind = 'arr.read'");
	g_assert_cmpstr(job, ==, "succeeded 1 true");
	matches = axis_test_db_query(fx->t->db, "SELECT string_agg(match, ' ' ORDER BY seq) FROM arr_plan_item");
	g_assert_cmpstr(matches, ==, "matched matched no_id unknown matched matched matched incomplete matched matched"
		" matched matched not_looked_up");
	/* What Radarr was downloading when it was read. */
	g_free(items);
	items = axis_test_db_query(fx->t->db, "SELECT string_agg(title, ',') FROM arr_plan_item WHERE in_queue");
	g_assert_cmpstr(items, ==, "The Quiet Orchard");
	/* One request for each list, and the queue; nothing but GET. */
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/system/status"), ==, 1);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/movie"), ==, 1);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/qualityprofile"), ==, 1);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/rootfolder"), ==, 1);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/queue"), ==, 1);
	axis_test_radarr_assert_read_only(fx->radarr);

	/* Axis manages one of the movies already, and one of Radarr's files is
	 * that movie's in Axis. */
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT e.title_id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT " FROM meta_external_id e"
		" WHERE e.kind = 'film' AND e.scheme = 'tmdb' AND e.value = '6006' RETURNING id", fx->root_id));
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_file (file_id, movie_id) SELECT f.id, m.id FROM library_file f,"
		" movie m WHERE f.rel_path = 'Marrow Lane (1990)/Marrow Lane (1990).mkv'");
	g_free(items);
	items = axis_test_db_query(fx->t->db, "SELECT count(*) FROM movie_file");
	g_assert_cmpstr(items, ==, "1");
	before = library_digest(fx);

	/* The connection's page says what was read, and leads to the preview. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "by root: 13 movies.");
	assert_contains(page.body, "href=\"/migrate/1/preview\"");
	assert_contains(page.body, "Read again");
	assert_lacks(page.body, KEY);
	axis_test_reply_clear(&page);
	/* The polled fragment of a reading that is over asks for the page again. */
	status = get_as(fx, fx->admin, "/migrate/1/status");
	g_assert_cmpuint(status.status, ==, 200);
	assert_contains(status.body, "id=\"arr-reading\"");
	assert_lacks(status.body, "<html");
	g_assert_cmpstr(axis_test_reply_header(&status, "HX-Refresh"), ==, "true");
	axis_test_reply_clear(&status);

	/* The preview, with nothing mapped yet: the profile of the same name is
	 * chosen by itself, and no folder is. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 3);
	assert_contains(page.body, "Radarr has 13 movies: 11 monitored, 2 not. 4 have a file. 1 is already in Axis.");
	assert_contains(page.body, "Would migrate 0 movies and link 0 files. 13 skipped (see below). Nothing would be "
		"searched for.");
	assert_contains(page.body, "Radarr was downloading 1 movie when it was read.");
	/* The profile with no namesake is set to Axis's default profile, and
	 * said to be a suggestion; so no movie is skipped for its profile. */
	assert_contains(page.body, "Folder not mapped (7)");
	assert_lacks(page.body, "Quality profile not mapped (");
	assert_contains(page.body, "Axis filled these in from what it already knows. Change any that are wrong.");
	assert_contains(page.body, "Axis has a profile of the same name");
	assert_contains(page.body, "Axis's default profile");
	/* A poor match suggests no folder: of Radarr's four files under /movies
	 * the index holds two below the one library folder (a file of the same
	 * name and size in another folder is not one of them); and Radarr has no
	 * file under /movies-4k. */
	assert_contains(page.body, "Not mapped. Nothing suggested: of 4 files looked for, no library folder holds more "
		"than 2 at the same paths.");
	assert_contains(page.body, "Not mapped. Nothing suggested: Radarr has no files under it.");
	assert_contains(page.body, "Would be migrated: 0 movies, 0 monitored and 0 unmonitored, as in Radarr.");
	assert_contains(page.body, "Ultra-HD");
	assert_contains(page.body, "Balanced");
	assert_contains(page.body, "Not mapped.");
	assert_lacks(page.body, KEY);
	save_page("preview-unmapped.html", page.body);
	axis_test_reply_clear(&page);

	/* Another mount: Radarr's /movies is the library folder Axis sees at
	 * media/disk1/films, and each file is looked for below it. */
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1/preview?done=saved");
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview?done=saved");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "The choices have been saved.");
	/* What the page is for, first: the monitoring that would come. */
	assert_contains(page.body, "Would be migrated: 4 movies, 4 monitored and 0 unmonitored, as in Radarr. 1 movie that "
		"Radarr does not monitor is left out: tick the box under Options to have it migrated unmonitored.");
	g_assert_true(strstr(page.body, "id=\"arr-monitoring\"") < strstr(page.body, "Radarr has 13 movies"));
	assert_contains(page.body, "Would migrate 4 movies and link 1 file. 9 skipped (see below). Nothing would be "
		"searched for.");
	assert_contains(page.body, "3 files of these movies would not be linked");
	assert_contains(page.body, "2 of 4 files found at these paths in Axis. Last scanned ");
	/* Each reason, with its title. */
	assert_contains(page.body, "Already in Axis (1)");
	assert_contains(page.body, "Not monitored in Radarr (1)");
	assert_contains(page.body, "No match: Radarr has no TMDb identifier for it (1)");
	assert_contains(page.body, "No match: TMDb no longer has the entry Radarr points to. It was probably removed there; nothing can be migrated for it. (1)");
	assert_contains(page.body, "Not looked up yet: TMDb did not answer (1)");
	assert_contains(page.body, "Quality profile not mapped (1)");
	assert_contains(page.body, "Folder not mapped (2)");
	assert_contains(page.body, "Radarr's answer for it is incomplete (1)");
	assert_contains(page.body, "Ash and Ember (1996)");
	assert_contains(page.body, "The Quiet Orchard (2004)");
	assert_contains(page.body, "Salt Meridian (2019)");
	assert_contains(page.body, "Glass Harbour (1987)");
	assert_contains(page.body, "Tall Grass (2008)");
	assert_contains(page.body, "Tin Sparrow (2001)");
	assert_contains(page.body, "Low Tide Radio (2015)");
	assert_contains(page.body, "Window Seat (1979)");
	assert_contains(page.body, "Hollow Compass (1999)");
	/* The managed one links to its page. */
	managed_href = axis_test_db_query(fx->t->db, "SELECT 'href=\"/title/' || title_id || '\"' FROM movie");
	assert_contains(page.body, managed_href);
	/* The files that would not be linked, each with its reason and both
	 * paths; the one of the same name in another folder is not taken. */
	assert_contains(page.body, "Another size than Radarr recorded");
	assert_contains(page.body, "/movies/Nine Winters (2022)/Nine Winters (2022).mkv");
	assert_contains(page.body, ": Nine Winters (2022)/Nine Winters (2022).mkv");
	assert_contains(page.body, "Not in Axis's index at that path");
	assert_contains(page.body, ": Copper Kettle (2003)/Copper Kettle (2003).mkv");
	assert_lacks(page.body, "elsewhere");
	assert_contains(page.body, "Linked in Axis to another movie: Ash and Ember (1996)");
	/* The one that would be brought and linked is in neither list. */
	assert_lacks(page.body, "Paper Lanterns");
	assert_lacks(page.body, KEY);
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 4);
	save_page("preview.html", page.body);
	axis_test_reply_clear(&page);

	/* The reasons as a filter. */
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=folder");
	assert_contains(page.body, "Low Tide Radio (2015)");
	assert_contains(page.body, "Window Seat (1979)");
	assert_lacks(page.body, "Salt Meridian");
	assert_lacks(page.body, "Hollow Compass");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=no-such-reason&sp=99999999&fp=-3");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Salt Meridian");
	axis_test_reply_clear(&page);

	/* The option: unmonitored movies too. */
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 5 movies and link 1 file. 8 skipped");
	assert_contains(page.body, "Would be migrated: 5 movies, 4 monitored and 1 unmonitored, as in Radarr.");
	assert_lacks(page.body, "is left out");
	assert_lacks(page.body, "Not monitored in Radarr (");
	axis_test_reply_clear(&page);

	/* One profile for every row maps the one that had no choice. */
	g_free(mappings);
	mappings = axis_test_db_query(fx->t->db, "SELECT id::text FROM quality_profile WHERE slug = 'space_saver'");
	{
		g_autofree gchar *form = g_strdup_printf("profile_ids=4,6&profile_4=0&profile_6=0&do=fill&fill_profile=%s"
			"&folder_rows=1&folder_prefix_0=%%2Fmovies&folder_root_0=%" G_GINT64_FORMAT, mappings, fx->root_id);

		posted = post_as(fx, fx->admin, "/migrate/1/preview", "/migrate/1/preview", form);
		g_assert_cmpuint(posted.status, ==, 303);
		axis_test_reply_clear(&posted);
	}
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 5 movies and link 1 file. 8 skipped");
	assert_lacks(page.body, "Quality profile not mapped (");
	axis_test_reply_clear(&page);

	/* A path with "..": refused, in fixed words, and nothing stored. */
	g_free(mappings);
	mappings = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");
	posted = post_as(fx, fx->admin, "/migrate/1/preview", "/migrate/1/preview",
		"profile_ids=&folder_rows=2&folder_prefix_0=%2Fmovies%2F..%2Fetc&folder_root_0=1"
		"&folder_prefix_1=%2Fmovies&folder_root_1=1&folder_below_1=..%2F..%2Fsecret-place");
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "A folder row was refused.");
	assert_lacks(posted.body, "secret-place");
	axis_test_reply_clear(&posted);
	posted = post_as(fx, fx->admin, "/migrate/1/preview", "/migrate/1/preview",
		"profile_ids=&folder_rows=1&folder_prefix_0=%2Fmovies&folder_root_0=1&folder_below_0=..%2F..%2Fsecret-place");
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "A folder row was refused.");
	axis_test_reply_clear(&posted);
	{
		g_autofree gchar *now = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");

		g_assert_cmpstr(now, ==, mappings);
	}

	/* The wrong folder: a library folder that holds none of the files (and,
	 * first, one that was never scanned). */
	other_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "other", AXIS_MEDIA_MOVIES);
	posted = map_movies(fx, source, other_root, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "0 of 4 files found at these paths in Axis. This library folder has never been "
		"scanned");
	axis_test_reply_clear(&page);
	scan = axis_test_library_scan(fx->t, other_root);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	wrong = g_strdup("0 of 4 files found at these paths in Axis. None of Radarr's files are at these paths: this is "
		"probably not the same folder.");
	assert_contains(page.body, wrong);
	assert_contains(page.body, "link 0 files");
	save_page("preview-wrong-folder.html", page.body);
	axis_test_reply_clear(&page);

	/* Nothing of the library changed: no movie, link, blocklist entry,
	 * grab, search, history line or setting; the index as the scans left it
	 * (the second folder is empty). */
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);

	/* A second reading takes the titles from the store: the provider is
	 * asked again only for what it did not answer and does not know. */
	finds = axis_test_plex_count(fx->fa->api, "/find/film");
	read_from_page(fx, source, "complete", 60000);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/find/film") - finds, <=, 2);
	g_free(items);
	items = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || count(DISTINCT plan_id) FROM arr_plan_item");
	g_assert_cmpstr(items, ==, "13 1");
	g_free(after);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);

	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
}

/* Spec: Disk offline */
static void
test_disk_offline(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	AxisTestReply posted;
	AxisTestReply page;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 5 movies and link 2 files.");
	axis_test_reply_clear(&page);

	/* The disk goes: its titles are skipped for that, by its name, and
	 * nothing of their files is said. */
	axis_test_db_exec(fx->t->db, "UPDATE volume SET state = 'offline', state_reason = 'path_missing'");
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 0 movies and link 0 files.");
	assert_contains(page.body, "Disk offline (5)");
	assert_contains(page.body, "Disk offline: the library folder is on Disk one, which Axis cannot see now");
	assert_contains(page.body, "The library folder is on Disk one, which Axis cannot see now.");
	axis_test_reply_clear(&page);

	/* It comes back: they would be brought again. */
	axis_test_db_exec(fx->t->db, "UPDATE volume SET state = 'online', state_reason = 'none'");
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 5 movies and link 2 files.");
	axis_test_reply_clear(&page);
}

/* Paged lists: Radarr's queue page by page up to its count; the preview's
 * lists a hundred rows at a time. */
static void
test_paging(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *queued = NULL;
	gint64 ids[7];
	AxisArrTuning tuning;
	AxisTestReply page;
	guint i;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.page_size = 3;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);
	/* 250 movies Radarr has no identifier for: none is looked up. */
	for (i = 1; i <= 250; i++) {
		g_autofree gchar *title = g_strdup_printf("Made Up Film %03u", i);

		add_movie(json, i, title, 2001, 0, TRUE, 4, "/movies", -1);
	}
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	for (i = 0; i < G_N_ELEMENTS(ids); i++)
		ids[i] = 10 + i;
	axis_test_radarr_set_queue(fx->radarr, ids, G_N_ELEMENTS(ids));

	read_from_page(fx, source, "complete", 60000);
	/* Seven records, three a page: three pages, and no fourth. */
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/queue"), ==, 3);
	queued = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || min(arr_id) || ' ' || max(arr_id) FROM"
		" arr_plan_item WHERE in_queue");
	g_assert_cmpstr(queued, ==, "7 10 16");

	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Radarr has 250 movies");
	assert_contains(page.body, "Radarr was downloading 7 movies when it was read.");
	assert_contains(page.body, "1 to 100 of 250.");
	assert_contains(page.body, "Made Up Film 001 (2001)");
	assert_contains(page.body, "Made Up Film 100 (2001)");
	assert_lacks(page.body, "Made Up Film 101");
	assert_contains(page.body, "/migrate/1/preview?sp=1#arr-skipped");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?sp=2");
	assert_contains(page.body, "201 to 250 of 250.");
	assert_contains(page.body, "Made Up Film 250 (2001)");
	assert_lacks(page.body, "Made Up Film 200 ");
	assert_lacks(page.body, "Next page");
	assert_contains(page.body, "First page");
	axis_test_reply_clear(&page);
	axis_test_radarr_assert_read_only(fx->radarr);
}

/* Spec: In no log line; Radarr stops answering; Too large; Not Radarr.  A
 * reading that fails leaves a plan that cannot be previewed, with its reason
 * in fixed words. */
static void
test_reading_fails(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *server = axis_test_radarr_server(fx->radarr);
	gint64 bad = add_source(fx, "Bad key", NULL, OTHER_KEY);
	gint64 good = add_source(fx, "Radarr", NULL, KEY);
	g_autofree gchar *problem = NULL;
	g_autofree gchar *job = NULL;
	g_autofree gchar *items = NULL;
	AxisArrTuning tuning;
	AxisTestReply page;

	/* A failed job is said in the log. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");

	/* The key refused. */
	read_from_page(fx, bad, "failed", 30000);
	problem = plan_problem(fx, bad);
	g_assert_cmpstr(problem, ==, "Radarr refused the API key.");
	job = axis_test_db_query(fx->t->db, "SELECT state || ': ' || last_error FROM job WHERE kind = 'arr.read'"
		" ORDER BY id DESC LIMIT 1");
	g_assert_cmpstr(job, ==, "failed: Radarr refused the API key.");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "The last reading failed");
	assert_contains(page.body, "Radarr refused the API key.");
	assert_contains(page.body, "Key refused");
	assert_contains(page.body, "Read again");
	assert_lacks(page.body, OTHER_KEY);
	assert_lacks(page.body, "Open the preview");
	save_page("source-failed.html", page.body);
	axis_test_reply_clear(&page);
	/* No run can be made from it: there is nothing to preview. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "There is nothing to preview");
	assert_lacks(page.body, "Would migrate");
	axis_test_reply_clear(&page);
	/* Only the answer that names the app was asked for. */
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/movie"), ==, 0);
	assert_secret_nowhere(fx, OTHER_KEY);

	/* Another app at the address: nothing but its status is asked. */
	axis_test_radarr_set_app(fx->radarr, "Sonarr");
	axis_test_plex_reset_requests(server);
	read_from_page(fx, good, "failed", 30000);
	g_free(problem);
	problem = plan_problem(fx, good);
	g_assert_cmpstr(problem, ==, "This address answers, but not as Radarr.");
	g_assert_cmpuint(axis_test_plex_n_requests(server), ==, 1);
	axis_test_radarr_set_app(fx->radarr, NULL);

	/* Radarr stops answering in the middle: the list of movies never comes. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.list_timeout_ms = 700;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_plex_route(server, "/api/v3/movie", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	read_from_page(fx, good, "failed", 30000);
	g_free(problem);
	problem = plan_problem(fx, good);
	g_assert_cmpstr(problem, ==, "Radarr did not answer.");
	axis_test_plex_clear_routes(server);

	/* An answer larger than Axis reads. */
	tuning.list_timeout_ms = 20000;
	tuning.list_max_bytes = 4096;
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_plex_route(server, "/api/v3/movie", -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL, 200000);
	read_from_page(fx, good, "failed", 30000);
	g_free(problem);
	problem = plan_problem(fx, good);
	g_assert_true(g_str_has_prefix(problem, "Radarr's library is too large for this version of Axis to migrate ("));
	axis_test_plex_clear_routes(server);

	/* An answer that is no list. */
	tuning.list_max_bytes = 0;
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_radarr_set_movies(fx->radarr, "{\"message\":\"<script>alert(1)</script>\"}");
	read_from_page(fx, good, "failed", 30000);
	g_free(problem);
	problem = plan_problem(fx, good);
	g_assert_cmpstr(problem, ==, "Radarr's list of movies is not a list.");
	page = get_as(fx, fx->admin, "/migrate/2");
	assert_contains(page.body, "Radarr's list of movies is not a list.");
	assert_lacks(page.body, "alert(1)");
	axis_test_reply_clear(&page);

	/* None of the failed readings kept a title, and none can be previewed. */
	items = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan_item) || ' ' || (SELECT count(*)"
		" FROM arr_plan WHERE state = 'complete') || ' ' || (SELECT count(*) FROM movie)");
	g_assert_cmpstr(items, ==, "0 0 0");
	page = get_as(fx, fx->admin, "/migrate/2/preview");
	assert_contains(page.body, "There is nothing to preview");
	axis_test_reply_clear(&page);

	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
	assert_secret_nowhere(fx, OTHER_KEY);
}

/* A connection forgotten while its reading waits for Radarr stops the job,
 * and nothing of it is left. */
static void
test_forget_during_reading(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *server = axis_test_radarr_server(fx->radarr);
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *left = NULL;
	AxisArrTuning tuning;
	AxisTestReply posted;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.list_timeout_ms = 120000;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_plex_route(server, "/api/v3/movie", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	queue_start(fx);
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/read", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	for (i = 0; i < 200 && axis_test_radarr_count(fx->radarr, "/api/v3/movie") == 0; i++)
		g_usleep(50000);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/movie"), >, 0);

	g_assert_true(axis_arr_delete_source(fx->arr, source, &error));
	g_assert_no_error(error);
	/* Not after the two minutes of the request: at once. */
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.read'"
		" AND state IN ('queued', 'running')", "0", 30000));
	state = axis_test_db_query(fx->t->db, "SELECT state FROM job WHERE kind = 'arr.read' ORDER BY id DESC LIMIT 1");
	g_assert_cmpstr(state, ==, "cancelled");
	left = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan) || ' ' ||"
		" (SELECT count(*) FROM arr_plan_item) || ' ' || (SELECT count(*) FROM arr_source_secret)");
	g_assert_cmpstr(left, ==, "0 0 0");
	axis_test_plex_clear_routes(server);
}

/* One reading asks the provider for so many titles and no more, says how
 * many it left, and reading again goes on; the download queue is read within
 * one deadline. */
static void
test_reading_limits(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *counts = NULL;
	g_autofree gchar *problem = NULL;
	static const gint64 tmdb[] = { 6001, 6002, 6005, 6006, 6007, 6008 };
	static const gint64 queued[] = { 1, 2, 3 };
	AxisArrTuning tuning;
	AxisTestReply page;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	for (i = 0; i < G_N_ELEMENTS(tmdb); i++) {
		g_autofree gchar *title = g_strdup_printf("Made Up Film %u", i + 1);

		add_movie(json, i + 1, title, 2001, tmdb[i], TRUE, 4, "/movies", -1);
	}
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.lookup_wait_ms = 5000;
	tuning.lookup_max = 3;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);

	/* Six that the store does not have, three asked. */
	read_from_page(fx, source, "complete", 60000);
	counts = axis_test_db_query(fx->t->db, "SELECT count(*) FILTER (WHERE match = 'matched') || ' ' ||"
		" count(*) FILTER (WHERE match = 'not_looked_up') FROM arr_plan_item");
	g_assert_cmpstr(counts, ==, "3 3");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 3);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "3 movies were not looked up yet");
	assert_contains(page.body, "Read again to continue");
	axis_test_reply_clear(&page);

	/* Again: the first three are in the store, the next three are asked. */
	read_from_page(fx, source, "complete", 60000);
	g_free(counts);
	counts = axis_test_db_query(fx->t->db, "SELECT count(*) FILTER (WHERE match = 'matched') || ' ' ||"
		" count(*) FILTER (WHERE match = 'not_looked_up') FROM arr_plan_item");
	g_assert_cmpstr(counts, ==, "6 0");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 6);

	/* The queue, a record a page, within a deadline that has gone by. */
	tuning.lookup_max = 0;
	tuning.page_size = 1;
	tuning.queue_deadline_ms = 1;
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_radarr_set_queue(fx->radarr, queued, G_N_ELEMENTS(queued));
	read_from_page(fx, source, "failed", 60000);
	problem = plan_problem(fx, source);
	g_assert_cmpstr(problem, ==, "Radarr took too long to list what it is downloading.");
	axis_test_radarr_assert_read_only(fx->radarr);
}

/* A reading that failed keeps no title; a new reading clears what an
 * earlier one that did not finish left. */
static void
test_failed_plans_keep_nothing(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *items = NULL;
	gint64 plan = 0;

	axis_test_db_exec(fx->t->db, "INSERT INTO arr_plan (source_id, state, finished_at, problem)"
		" VALUES (1, 'failed', now(), 'The reading did not finish.')");
	axis_test_db_exec(fx->t->db, "INSERT INTO arr_plan_item (plan_id, seq, title) SELECT id, 1, 'Made Up Film'"
		" FROM arr_plan");
	items = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item");
	g_assert_cmpstr(items, ==, "1");
	g_assert_true(axis_arr_request_read(fx->arr, source, "root", &plan, &error));
	g_assert_no_error(error);
	g_assert_cmpint(plan, >, 0);
	g_free(items);
	items = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item");
	g_assert_cmpstr(items, ==, "0");
}

/* What a test or a reading found is of the address it was made at. */
static void
test_result_of_another_address(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(AxisArrSource) current = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *found = NULL;

	g_assert_true(axis_arr_get_source(fx->arr, source, &current, &error));
	g_assert_no_error(error);
	/* The address was changed meanwhile: the result is not written onto it. */
	g_assert_true(axis_arr_store_test(fx->arr, source, "http://moved.example:7878", "", AXIS_ARR_TEST_OK, "5.0", &error));
	found = axis_test_db_query(fx->t->db, "SELECT coalesce(last_test, 'none') || ' ' || coalesce(version, 'none')"
		" FROM arr_source");
	g_assert_cmpstr(found, ==, "none none");
	g_assert_true(axis_arr_store_test(fx->arr, source, current->origin, current->path_prefix, AXIS_ARR_TEST_OK, "5.0",
		&error));
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT coalesce(last_test, 'none') || ' ' || coalesce(version, 'none')"
		" FROM arr_source");
	g_assert_cmpstr(found, ==, "ok 5.0");
}

/* The choices of a page whose plan is gone are not saved over what is
 * stored. */
static void
test_save_without_a_plan(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autofree gchar *stored = NULL;
	g_autofree gchar *after = NULL;
	AxisTestReply posted;

	odd_library(fx);
	read_from_page(fx, source, "complete", 30000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	stored = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");
	assert_contains(stored, "/movies");

	/* The plan is a day and more old: nothing to preview. */
	axis_test_db_exec(fx->t->db, "UPDATE arr_plan SET started_at = now() - interval '2 days'");
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/preview", "profile_ids=&folder_rows=0");
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "The choices were not saved");
	axis_test_reply_clear(&posted);
	after = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");
	g_assert_cmpstr(after, ==, stored);
}

/* A movie Radarr lists twice is counted once. */
static void
test_listed_twice(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	AxisTestReply posted;
	AxisTestReply page;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	add_movie(json, 1, "Paper Lanterns", 2011, 6001, TRUE, 4, "/movies", -1);
	/* The same TMDb identifier under another id, and another film under
	 * the first's id. */
	add_movie(json, 2, "Paper Lanterns Again", 2011, 6001, TRUE, 4, "/movies", -1);
	add_movie(json, 1, "Tin Sparrow", 2001, 6007, TRUE, 4, "/movies", -1);
	add_movie(json, 4, "Low Tide Radio", 2015, 6008, TRUE, 4, "/movies", -1);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	read_from_page(fx, source, "complete", 30000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Radarr lists it twice (2)");
	assert_contains(page.body, "Would migrate 2 movies and link 0 files. 2 skipped");
	axis_test_reply_clear(&page);
}

/* Spec: Thousands of movies.  Task 2.5: 8,000 generated movies read, matched
 * from the store and paged within a stated time: the reading within two
 * minutes and a page of the preview within thirty seconds, under the
 * sanitizers. */
static void
test_eight_thousand(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *counts = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	gint64 read_ms;
	gint64 start;
	gint64 page_ms;
	guint n = 8000;
	guint i;

	/* The titles are in Axis's store already, as after identification. */
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO meta_title (kind, title, year, provider, provider_id) SELECT"
		" 'film', 'Made Up Film ' || g, 2001, 'fixture', (100000 + g)::text FROM generate_series(1, %u) g RETURNING 1",
		n));
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'film',"
		" 'tmdb', provider_id FROM meta_title WHERE provider = 'fixture'");
	for (i = 1; i <= n; i++) {
		g_autofree gchar *title = g_strdup_printf("Made Up Film %u", i);

		add_movie(json, i, title, 2001, 100000 + i, i % 10 != 0, 4, i % 2 == 0 ? "/movies" : "/movies-4k",
			i % 4 == 0 ? (gint64)1000 + i : (gint64)-1);
	}
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");

	read_ms = read_from_page(fx, source, "complete", 180000);
	counts = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || count(*) FILTER (WHERE match = 'matched') FROM"
		" arr_plan_item");
	g_assert_cmpstr(counts, ==, "8000 8000");
	/* All from the store: the provider was not asked once. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 0);

	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	start = g_get_monotonic_time();
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	page_ms = (g_get_monotonic_time() - start) / 1000;
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Radarr has 8,000 movies: 7,200 monitored, 800 not. 2,000 have a file.");
	/* Half are under the mapped folder; of those a fifth are not monitored. */
	assert_contains(page.body, "Would migrate 3,200 movies and link 0 files. 4,800 skipped");
	assert_contains(page.body, "0 of 2,000 files found at these paths in Axis.");
	assert_contains(page.body, "1 to 100 of 4,800.");
	/* The files of the movies that would come: every fourth has one, and a
	 * fifth of those is not monitored. */
	assert_contains(page.body, "1 to 100 of 1,600.");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 4);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=folder&sp=39&fp=15");
	assert_contains(page.body, "3,901 to 4,000 of 4,000.");
	assert_contains(page.body, "1,501 to 1,600 of 1,600.");
	axis_test_reply_clear(&page);

	g_test_message("8,000 movies: read in %" G_GINT64_FORMAT " ms, a page of the preview in %" G_GINT64_FORMAT " ms",
		read_ms, page_ms);
	g_assert_cmpint(read_ms, <, 120000);
	g_assert_cmpint(page_ms, <, 30000);
	axis_test_radarr_assert_read_only(fx->radarr);
}

/* ── Bringing over (R3) ─────────────────────────────────────────── */

/* The digest the confirmation at the end of the preview carries. */
static gchar *
page_digest(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("name=\"digest\"[^>]*value=\"([0-9a-f]{64})\"|value=\"([0-9a-f]{64})\"[^>]*"
		"name=\"digest\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	gchar *found;

	if (!g_regex_match(regex, body, 0, &match))
		g_error("the page carries no digest:\n%s", body);
	found = g_match_info_fetch(match, 1);
	if (found == NULL || found[0] == '\0') {
		g_free(found);
		found = g_match_info_fetch(match, 2);
	}
	return found;
}

/* The tick box of the run's confirmation, as the page draws it. */
static gchar *
start_box(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("<input[^>]*name=\"start\"[^>]*>", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (!g_regex_match(regex, body, 0, &match))
		g_error("the page has no tick to start searching:\n%s", body);
	return g_match_info_fetch(match, 0);
}

/* Its value: the digest of the run with the tick. */
static gchar *
start_digest(const gchar *body)
{
	g_autofree gchar *box = start_box(body);
	g_autoptr(GRegex) regex = g_regex_new("value=\"([0-9a-f]{64})\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (!g_regex_match(regex, box, 0, &match))
		g_error("the tick carries no digest: %s", box);
	return g_match_info_fetch(match, 1);
}

/* The one post that starts a run, as @cookie, with @digest. */
static AxisTestReply
confirm_run(
	Fixture     *fx,
	const gchar *cookie,
	gint64       source,
	const gchar *digest
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/run", source);
	g_autofree gchar *form = g_strdup_printf("digest=%s", digest);

	return post_as(fx, cookie, cookie == fx->admin ? page : "/", action, form);
}

/* Waits for the run to be over; how its job ended. */
static gchar *
wait_run(Fixture *fx)
{
	if (!axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'"
	        " AND state IN ('queued', 'running')", "0", 120000))
		g_error("the run did not end");
	return axis_test_db_query(fx->t->db, "SELECT state::text FROM job WHERE kind = 'arr.apply' ORDER BY id DESC LIMIT 1");
}

/* Confirms the preview as it stands and waits for the run. */
static void
run_from_preview(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *path = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	AxisTestReply page = get_as(fx, fx->admin, path);
	g_autofree gchar *digest = page_digest(page.body);
	g_autofree gchar *state = NULL;
	AxisTestReply posted;

	axis_test_reply_clear(&page);
	queue_start(fx);
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=running");
	axis_test_reply_clear(&posted);
	state = wait_run(fx);
	if (g_strcmp0(state, "succeeded") != 0) {
		g_autofree gchar *why = axis_test_db_query(fx->t->db, "SELECT last_error FROM job WHERE kind = 'arr.apply'"
			" ORDER BY id DESC LIMIT 1");

		g_error("the run ended %s: %s", state, why);
	}
}

static gint
compare_lines(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void
collect_files(
	const gchar *dir,
	const gchar *below,
	GPtrArray   *lines
){
	g_autoptr(GDir) open = g_dir_open(dir, 0, NULL);
	const gchar *name;

	g_assert_nonnull(open);
	while ((name = g_dir_read_name(open)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		g_autofree gchar *rel = below[0] != '\0' ? g_strconcat(below, "/", name, NULL) : g_strdup(name);
		GStatBuf st;

		g_assert_cmpint(g_lstat(path, &st), ==, 0);
		if (S_ISDIR(st.st_mode)) {
			g_ptr_array_add(lines, g_strdup_printf("%s/ %" G_GUINT64_FORMAT, rel, (guint64)st.st_ino));
			collect_files(path, rel, lines);
		} else {
			g_autofree gchar *content = NULL;
			g_autofree gchar *sum = NULL;
			gsize length = 0;

			g_assert_true(g_file_get_contents(path, &content, &length, NULL));
			sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)content, length);
			g_ptr_array_add(lines, g_strdup_printf("%s %" G_GUINT64_FORMAT " %" G_GSIZE_FORMAT " %" G_GINT64_FORMAT
				" %s", rel, (guint64)st.st_ino, length, (gint64)st.st_mtime, sum));
		}
	}
}

/* Every file and folder below the library folder: its path, its inode, its
 * size, its modification time and its content. */
static gchar *
disk_fingerprint(Fixture *fx)
{
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);

	collect_files(fx->films_dir, "", lines);
	g_ptr_array_sort(lines, compare_lines);
	g_ptr_array_add(lines, NULL);
	return g_strjoinv("\n", (gchar **)lines->pdata);
}

static gchar *
index_digest(Fixture *fx)
{
	return axis_test_db_query(fx->t->db, "SELECT md5(coalesce(string_agg(f.id::text || f.rel_path || f.size::text ||"
		" f.state || f.inode::text, '|' ORDER BY f.id), '-')) FROM library_file f");
}

static gint64
movie_of(
	Fixture     *fx,
	const gchar *title
){
	g_autofree gchar *id = axis_test_db_queryf(fx->t->db, "SELECT m.id FROM movie m JOIN meta_title t ON t.id ="
		" m.title_id WHERE t.title = '%s'", title);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static gchar *
hold_of(
	Fixture     *fx,
	const gchar *title
){
	return axis_test_db_queryf(fx->t->db, "SELECT coalesce(m.search_hold, 'none') FROM movie m JOIN meta_title t ON"
		" t.id = m.title_id WHERE t.title = '%s'", title);
}

/* Every movie brought, by title: its hold, monitoring, availability, folder
 * and profile. */
static gchar *
brought_movies(Fixture *fx)
{
	return axis_test_db_queryf(fx->t->db, "SELECT string_agg(t.title || '=' || coalesce(m.search_hold, 'none') || '/'"
		" || m.monitored || '/' || m.min_availability || '/' || (m.root_id = %" G_GINT64_FORMAT ") || '/' || qp.slug,"
		" ' ' ORDER BY t.title) FROM movie m JOIN meta_title t ON t.id = m.title_id JOIN quality_profile qp ON qp.id ="
		" m.quality_profile_id WHERE m.added_by = 'root'", fx->root_id);
}

/* Every link of a file to a movie, by title: how it was made. */
static gchar *
links(Fixture *fx)
{
	return axis_test_db_query(fx->t->db, "SELECT string_agg(t.title || ':' || f.rel_path || ':' ||"
		" coalesce(mf.hand_how, '-') || ':' || coalesce(mf.hand_by, '-') || ':' || mf.kept || ':' ||"
		" (mf.import_id IS NULL) || ':' || (mf.identity_id IS NULL) || ':' || coalesce(mf.quality, '-'), ' | '"
		" ORDER BY t.title, f.rel_path) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON"
		" t.id = m.title_id JOIN library_file f ON f.id = mf.file_id");
}

/* Axis manages Tin Sparrow already, and a person linked Marrow Lane's file
 * to it by hand and keeps it. */
static void
tin_sparrow_holds_marrow_lane(Fixture *fx)
{
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT e.title_id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT " FROM meta_external_id e"
		" WHERE e.kind = 'film' AND e.scheme = 'tmdb' AND e.value = '6007' RETURNING id", fx->root_id));
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_file (file_id, movie_id, hand_how, hand_by, hand_at, kept)"
		" SELECT f.id, m.id, 'linked', 'root', now(), true FROM library_file f, movie m"
		" WHERE f.rel_path = 'Marrow Lane (1990)/Marrow Lane (1990).mkv'");
}

/* The odd library read, mapped (unmonitored movies too) and migrated. */
static gint64
bring_odd_library(Fixture *fx)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	AxisTestReply posted;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	tin_sparrow_holds_marrow_lane(fx);
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	run_from_preview(fx, source);
	return source;
}

/*
 * The preview arrives filled in, and can be confirmed as it stands: Radarr's
 * folder is set to the folder of the library folder under which Axis's index
 * holds its files, the profile with no namesake to Axis's default.  Viewing
 * writes nothing.  The digest is of the choices in force: when the index
 * changes so that the suggestion is another place (the counts being the
 * same), the page confirmed before is refused; the page as it stands then
 * runs, with nothing saved.
 */
static void
test_run_suggested(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	g_autoptr(AxisScanResult) scan = NULL;
	g_autofree gchar *untouched = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *fresh = NULL;
	g_autofree gchar *text = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	write_file(fx, "shelf/Ash and Ember (1996)", "Ash and Ember (1996).mkv", "ember-0001");
	write_file(fx, "shelf/Tin Sparrow (2001)", "Tin Sparrow (2001).mkv", "sparrow-001");
	write_file(fx, "shelf/Low Tide Radio (2015)", "Low Tide Radio (2015).mkv", "radio-000001");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	while (g_main_context_iteration(NULL, FALSE))
		;
	add_movie(json, 1, "Ash and Ember", 1996, 6006, TRUE, 4, "/films-a", 10);
	add_movie(json, 2, "Tin Sparrow", 2001, 6007, TRUE, 6, "/films-a", 11);
	add_movie(json, 3, "Low Tide Radio", 2015, 6008, FALSE, 4, "/films-a", 12);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"balanced\"},{\"id\":6,\"name\":\"Ultra-HD\"}]");
	axis_test_radarr_set_folders(fx->radarr, "[{\"path\":\"/films-a\",\"accessible\":true}]");
	read_from_page(fx, source, "complete", 60000);
	untouched = library_digest(fx);

	/* The first view: filled in, counted, and offered. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Axis filled these in from what it already knows. Change any that are wrong.");
	assert_contains(page.body, "Would be migrated: 2 movies, 2 monitored and 0 unmonitored, as in Radarr. 1 movie that "
		"Radarr does not monitor is left out: tick the box under Options to have it migrated unmonitored.");
	assert_contains(page.body, "value=\"shelf\"");
	assert_contains(page.body, "3 of 3 files found here. Last scanned ");
	assert_contains(page.body, "Axis has a profile of the same name");
	assert_contains(page.body, "Axis's default profile");
	assert_contains(page.body, "Would migrate 2 movies and link 2 files.");
	assert_lacks(page.body, "Folder not mapped (");
	assert_lacks(page.body, "Quality profile not mapped (");
	digest = page_digest(page.body);
	save_page("preview-suggested.html", page.body);
	axis_test_reply_clear(&page);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, untouched);
	text = axis_test_db_query(fx->t->db, "SELECT mappings::text || ' ' || (SELECT count(*) FROM movie) FROM arr_source");
	g_assert_cmpstr(text, ==, "{} 0");

	/* The index now holds the files in another folder: the suggestion is
	 * that folder, the counts are the same, and the digest is another. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET rel_path = 'ledge/' || substr(rel_path, 7)"
		" WHERE rel_path LIKE 'shelf/%'");
	queue_start(fx);
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "The preview changed after this page was made.");
	assert_contains(posted.body, "value=\"ledge\"");
	assert_contains(posted.body, "Would migrate 2 movies and link 2 files.");
	fresh = page_digest(posted.body);
	g_assert_cmpstr(fresh, !=, digest);
	axis_test_reply_clear(&posted);
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) + (SELECT count(*) FROM job WHERE kind ="
		" 'arr.apply')");
	g_assert_cmpstr(text, ==, "0");

	/* Confirmed as it stands: what was shown is what is brought, each movie
	 * with the suggested profile and folder and its file linked; and no
	 * choice was saved for it. */
	run_from_preview(fx, source);
	g_free(text);
	text = brought_movies(fx);
	g_assert_cmpstr(text, ==, "Ash and Ember=brought/true/released/true/balanced "
		"Tin Sparrow=brought/true/released/true/balanced");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(f.rel_path, ' | ' ORDER BY f.rel_path) FROM movie_file mf"
		" JOIN library_file f ON f.id = mf.file_id");
	g_assert_cmpstr(text, ==, "ledge/Ash and Ember (1996)/Ash and Ember (1996).mkv | "
		"ledge/Tin Sparrow (2001)/Tin Sparrow (2001).mkv");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");
	g_assert_cmpstr(text, ==, "{}");
	axis_test_radarr_assert_read_only(fx->radarr);
}

#define BROUGHT_ODD \
	"Ash and Ember=brought/true/released/true/balanced " \
	"Copper Kettle=file_unseen/true/released/true/balanced " \
	"Marrow Lane=file_unseen/true/released/true/balanced " \
	"Nine Winters=file_unseen/true/released/true/balanced " \
	"Paper Lanterns=brought/true/released/true/balanced " \
	"The Quiet Orchard=brought/false/released/true/balanced"

#define LINKS_ODD \
	"Paper Lanterns:Paper Lanterns (2011)/Paper Lanterns (2011).mkv:brought:root:true:true:true:bluray_1080p | " \
	"Tin Sparrow:Marrow Lane (1990)/Marrow Lane (1990).mkv:linked:root:true:true:true:-"

/* Spec (bring-over): Monitored movies; Unmonitored, when asked; Changed
 * since; Found at its path; Not seen; Nothing touched; Linked elsewhere; A
 * thousand missing movies (nothing searched).  Spec (movies): Shown.  Task
 * 3.9: a stale digest refused; path, name, size and inode of the files
 * unchanged.  A viewer cannot start a run. */
static void
test_run_brings(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autofree gchar *disk_before = NULL;
	g_autofree gchar *disk_after = NULL;
	g_autofree gchar *index_before = NULL;
	g_autofree gchar *index_after = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *fresh = NULL;
	g_autofree gchar *found = NULL;
	g_autofree gchar *state_path = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	guint asked;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	tin_sparrow_holds_marrow_lane(fx);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	disk_before = disk_fingerprint(fx);
	index_before = index_digest(fx);
	asked = axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr));

	/* The confirmation says what a run will do, with one button. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Migrates 5 movies into Axis");
	assert_contains(page.body, "links 1 file that Axis's index holds at the mapped paths");
	assert_contains(page.body, "Radarr's /movies is the library folder films.");
	assert_contains(page.body, "Nothing is searched for unless you tick the box below: every movie migrated is held");
	assert_contains(page.body, "The migrated files are kept: Axis does not upgrade or replace them by itself");
	assert_contains(page.body, "No file is moved, renamed or deleted");
	assert_contains(page.body, "Migrate 5 movies");
	/* The tick to start searching with the run is offered, and not ticked;
	 * no blocklist is offered (the decision on the proposal's question 5). */
	assert_contains(page.body, "Start searching for these as soon as they are migrated");
	{
		g_autofree gchar *box = start_box(page.body);

		g_assert_null(strstr(box, "checked"));
	}
	assert_lacks(page.body, "locklist");
	digest = page_digest(page.body);
	save_page("preview-confirm.html", page.body);
	axis_test_reply_clear(&page);
	queue_start(fx);

	/* A viewer cannot start it, with whatever digest. */
	posted = confirm_run(fx, fx->viewer, source, digest);
	g_assert_cmpuint(posted.status, ==, 403);
	axis_test_reply_clear(&posted);
	/* A digest of no page: refused, the preview shown again. */
	posted = confirm_run(fx, fx->admin, source, "0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "The preview changed after this page was made. Nothing was migrated");
	assert_contains(posted.body, "Migrate 5 movies");
	axis_test_reply_clear(&posted);
	/* The choices change in another window: the first window's confirmation
	 * is refused, and nothing is brought. */
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "The preview changed after this page was made.");
	assert_contains(posted.body, "Migrate 6 movies");
	fresh = page_digest(posted.body);
	g_assert_cmpstr(fresh, !=, digest);
	axis_test_reply_clear(&posted);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) || ' ' || (SELECT count(*) FROM job"
		" WHERE kind = 'arr.apply') || ' ' || (SELECT count(*) FROM arr_brought)");
	g_assert_cmpstr(found, ==, "1 0 0");

	/* Confirmed on the page as it stands: the run, as a job. */
	posted = confirm_run(fx, fx->admin, source, fresh);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1?done=running");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = wait_run(fx);
	g_assert_cmpstr(found, ==, "succeeded");
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (payload::text NOT LIKE '%" KEY "%') || ' ' || (payload::text NOT LIKE"
		" '%Lanterns%') || ' ' || (payload::text NOT LIKE '%/movies%') FROM job WHERE kind = 'arr.apply'");
	g_assert_cmpstr(found, ==, "true true true");
	/* Not ticked: the job holds that, and the run starts no search (as
	 * before the tick existed: everything is held, below). */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT payload ->> 'start' FROM job WHERE kind = 'arr.apply'");
	g_assert_cmpstr(found, ==, "false");

	/* Each movie with its mapped profile and library folder, monitored as in
	 * Radarr, and held: apart where Radarr has a file Axis does not see. */
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==, BROUGHT_ODD);
	/* The file at its mapped path, of its size, is linked and marked as
	 * migrated: no import, no identity, and kept (Axis does not replace
	 * it), because it was migrated.  The file a person linked to another
	 * movie stays there, kept by that person. */
	g_free(found);
	found = links(fx);
	g_assert_cmpstr(found, ==, LINKS_ODD);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(mf.hand_how || '=' || coalesce(mf.kept_how, '-'), ' '"
		" ORDER BY mf.hand_how) FROM movie_file mf");
	g_assert_cmpstr(found, ==, "brought=brought linked=-");
	/* Each "left alone" reason is recorded with its title. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(label || '=' || file || coalesce(':' || file_detail, ''),"
		" '; ' ORDER BY label) FROM arr_brought");
	g_assert_cmpstr(found, ==, "Ash and Ember (1996)=none; Copper Kettle (2003)=not_indexed; Marrow Lane (1990)="
		"elsewhere:Tin Sparrow (2001); Nine Winters (2022)=other_size; Paper Lanterns (2011)=linked; The Quiet Orchard"
		" (2004)=none");
	/* One history line a movie, and it says where the movie came from. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || count(DISTINCT e.movie_id) || ' ' ||"
		" count(*) FILTER (WHERE e.kind = 'added' AND e.text LIKE 'Migrated from Radarr (Radarr) by root, with the"
		" quality profile Balanced%') FROM movie_event e JOIN movie m ON m.id = e.movie_id WHERE m.added_by = 'root'");
	g_assert_cmpstr(found, ==, "6 6 6");
	/* Nothing was searched for or grabbed. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie_search) + (SELECT count(*) FROM"
		" movie_grab) + (SELECT count(*) FROM job WHERE kind LIKE 'movies.%')");
	g_assert_cmpstr(found, ==, "0");
	/* Nothing on disk changed, nor Axis's index of it; and the run asked
	 * Radarr nothing. */
	disk_after = disk_fingerprint(fx);
	g_assert_cmpstr(disk_after, ==, disk_before);
	index_after = index_digest(fx);
	g_assert_cmpstr(index_after, ==, index_before);
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_radarr_server(fx->radarr)), ==, asked);
	axis_test_radarr_assert_read_only(fx->radarr);

	/* The report. */
	page = get_as(fx, fx->admin, "/migrate/1");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "6 movies migrated from Radarr, 1 file linked.");
	/* The held, as the notice of "Needs attention" counts them: the one
	 * with its file needs nothing, the monitored one without a file waits,
	 * and the unmonitored one without a file is searched for by nothing. */
	assert_contains(page.body, "1 movie has its file. The file is kept, and Axis does not replace it: this movie needs "
		"nothing from you.");
	assert_contains(page.body, "1 movie is monitored and has no file. Axis does not search for it until you start "
		"searching.");
	assert_contains(page.body, "1 movie has no file and is not monitored: Axis does not search for it, before or after "
		"you start.");
	assert_lacks(page.body, "movies are held:");
	assert_contains(page.body, "3 movies are held apart: Radarr has a file for them that Axis does not see.");
	assert_contains(page.body, "Start searching for the movies without a file");
	assert_contains(page.body, "Starting searching affects only the movies without a file: the files that were migrated "
		"are kept and are not replaced.");
	save_page("source-brought.html", page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/brought");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Linked in Axis to another movie: Tin Sparrow (2001). Left there");
	assert_contains(page.body, "Another size than Radarr recorded");
	assert_contains(page.body, "Not in Axis's index at that path");
	assert_contains(page.body, "Radarr has no file for it");
	assert_contains(page.body, "Held: its file is not seen");
	assert_contains(page.body, "1 to 6 of 6.");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 1);
	save_page("brought.html", page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/brought?unlinked=yes");
	assert_contains(page.body, "Nine Winters (2022)");
	assert_lacks(page.body, "Paper Lanterns");
	assert_contains(page.body, "1 to 3 of 3.");
	axis_test_reply_clear(&page);

	/* The hold on the list and in a movie's state. */
	page = get_as(fx, fx->admin, "/films");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Held");
	assert_contains(page.body, "film-mark-held");
	axis_test_reply_clear(&page);
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", movie_of(fx, "Ash and Ember"));
	page = get_as(fx, fx->admin, state_path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Migrated from Radarr: Axis does not search for it until you start searching.");
	axis_test_reply_clear(&page);
	g_free(state_path);
	/* A held movie whose file was migrated and is kept says that, and not
	 * that something waits: starting searching changes nothing for it. */
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", movie_of(fx, "Paper Lanterns"));
	page = get_as(fx, fx->admin, state_path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Migrated from Radarr; Axis does not replace it.");
	assert_lacks(page.body, "until you start searching");
	axis_test_reply_clear(&page);
	g_free(state_path);
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", movie_of(fx, "Nine Winters"));
	page = get_as(fx, fx->admin, state_path);
	assert_contains(page.body, "Migrated from Radarr, which has a file for it that Axis does not see");
	axis_test_reply_clear(&page);
	assert_secret_nowhere(fx, KEY);
}

/* Spec (bring-over): Twice; A disk that came back; Changed in Axis since.
 * Task 3.6 and 3.0: a second run brings what is new, links a file that has
 * become visible, and leaves what a person changed in Axis: a profile and
 * monitoring, a link removed by hand (its mark in library_hand_unlink), a
 * movie removed; no second movie, link or history line. */
static void
test_second_run(Fixture *fx, gconstpointer data)
{
	gint64 source = bring_odd_library(fx);
	g_autoptr(AxisScanResult) scan = NULL;
	g_autofree gchar *found = NULL;
	g_autofree gchar *digest = NULL;
	AxisTestReply page;
	AxisTestReply posted;

	found = brought_movies(fx);
	g_assert_cmpstr(found, ==, BROUGHT_ODD);

	/* Since then, in Axis: a person gave Paper Lanterns another profile and
	 * stopped monitoring it, unlinked its file by hand, and removed Ash and
	 * Ember.  On disk: Copper Kettle's file is where Radarr says it is. */
	axis_test_db_exec(fx->t->db, "UPDATE movie m SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug ="
		" 'space_saver'), monitored = false FROM meta_title t WHERE t.id = m.title_id AND t.title = 'Paper Lanterns'");
	axis_test_db_exec(fx->t->db, "INSERT INTO library_hand_unlink (file_id, module, owner_id, by_user) SELECT mf.file_id,"
		" 'movies', mf.movie_id, 'root' FROM movie_file mf WHERE mf.hand_how = 'brought'");
	axis_test_db_exec(fx->t->db, "DELETE FROM movie_file WHERE hand_how = 'brought'");
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_movies_remove(fx->movies, movie_of(fx, "Ash and Ember"), "root", &error));
		g_assert_no_error(error);
	}
	write_file(fx, "Copper Kettle (2003)", "Copper Kettle (2003).mkv", "kettle123");
	scan = axis_test_library_scan(fx->t, fx->root_id);

	/* Radarr has one movie more; it is read again. */
	odd_library_with(fx, TRUE);
	read_from_page(fx, source, "complete", 60000);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 1 movie and link 0 files.");
	assert_contains(page.body, "Removed in Axis after it was migrated (1)");
	assert_contains(page.body, "1 file of movies migrated before is in Axis's index now, and would be linked.");
	assert_contains(page.body, "Migrate 1 movie");
	axis_test_reply_clear(&page);
	run_from_preview(fx, source);

	/* The new movie came; the removed one did not come back; the profile and
	 * the monitoring a person set stand; the file that became visible is
	 * linked, and its movie is held as the others are (searching was not
	 * started). */
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==,
		"Copper Kettle=brought/true/released/true/balanced "
		"Harbour Lights=brought/true/released/true/balanced "
		"Marrow Lane=file_unseen/true/released/true/balanced "
		"Nine Winters=file_unseen/true/released/true/balanced "
		"Paper Lanterns=brought/false/released/true/space_saver "
		"The Quiet Orchard=brought/false/released/true/balanced");
	/* The file unlinked by hand stays unlinked, with its mark. */
	g_free(found);
	found = links(fx);
	g_assert_cmpstr(found, ==,
		"Copper Kettle:Copper Kettle (2003)/Copper Kettle (2003).mkv:brought:root:true:true:true:bluray_1080p | "
		"Tin Sparrow:Marrow Lane (1990)/Marrow Lane (1990).mkv:linked:root:true:true:true:-");
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM library_hand_unlink");
	g_assert_cmpstr(found, ==, "1");
	/* No second movie, and one "Brought over" line a movie; the file linked
	 * later has a line of its own. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) || ' ' || (SELECT count(DISTINCT"
		" title_id) FROM movie) || ' ' || (SELECT count(*) FROM movie_event WHERE kind = 'added' AND movie_id IS NOT"
		" NULL) || ' ' || (SELECT count(*) FROM movie_event WHERE kind = 'hand_link' AND text LIKE 'Migrated from"
		" Radarr (Radarr) by root: its file Copper Kettle (2003)/Copper Kettle (2003).mkv%')");
	g_assert_cmpstr(found, ==, "7 7 6 1");

	/* A run with nothing changed brings nothing: there is nothing to
	 * confirm, and a post made anyway is refused. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "There is nothing to migrate now");
	assert_lacks(page.body, "name=\"digest\"");
	axis_test_reply_clear(&page);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'");
	posted = confirm_run(fx, fx->admin, source, "0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(posted.status, ==, 409);
	axis_test_reply_clear(&posted);
	digest = axis_test_db_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'");
	g_assert_cmpstr(digest, ==, found);
	axis_test_radarr_assert_read_only(fx->radarr);
}

static guint
search_jobs(
	Fixture *fx,
	gint64   movie
){
	g_autofree gchar *n = axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM job WHERE dedupe_key LIKE '"
		AXIS_MOVIES_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT "%%'", movie);

	return (guint)g_ascii_strtoull(n, NULL, 10);
}

static void
schedule(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisMovieScheduleReport report;

	g_assert_true(axis_movies_run_schedule(fx->movies, &report, &error));
	g_assert_no_error(error);
}

/* Spec (bring-over): A thousand missing movies; Started by a person; Radarr
 * is downloading it; Still monitored there.  Spec (movies): Held through the
 * scheduler; One movie, by a person; A file Axis does not see.  Task 3.9: a
 * held movie searched by nothing for three scheduler runs, then searched
 * after "Start searching"; a movie in Radarr's queue left out. */
static void
test_hold_and_start(Fixture *fx, gconstpointer data)
{
	gint64 source = bring_odd_library(fx);
	AxisTestPlex *server = axis_test_radarr_server(fx->radarr);
	static const gchar unclear_json[] = "{\"page\":1,\"pageSize\":250,\"totalRecords\":2,\"records\":"
		"[{\"id\":71},{\"id\":72}]}";
	g_autoptr(GBytes) unclear = g_bytes_new_static(unclear_json, sizeof(unclear_json) - 1);
	static const gint64 ash[] = { 5 };
	gint64 ash_id = movie_of(fx, "Ash and Ember");
	gint64 nine_id = movie_of(fx, "Nine Winters");
	gint64 copper_id = movie_of(fx, "Copper Kettle");
	g_autofree gchar *found = NULL;
	g_autofree gchar *action = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	guint i;

	g_assert_cmpint(source, ==, 1);
	/* Every movie is out: only the hold stands between it and a search. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	for (i = 0; i < 3; i++)
		schedule(fx);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM job WHERE kind LIKE 'movies.%') + (SELECT"
		" count(*) FROM movie_search)");
	g_assert_cmpstr(found, ==, "0");

	/* Radarr's queue cannot be read as assumed (its records name no movie):
	 * said in words, and starting needs no tick for it. */
	axis_test_plex_route_full(server, "/api/v3/queue", 200, "application/json", unclear, NULL);
	page = get_as(fx, fx->admin, "/migrate/1/start");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Radarr answered and lists 2 downloads, but Axis could not tell which movies they are "
		"for.");
	assert_lacks(page.body, "arr-start-stopped");
	/* The button says what it does: the one movie without a file. */
	assert_contains(page.body, "Start searching for 1 movie without a file");
	assert_contains(page.body, "Starting searching affects only the movies without a file. Of 3 movies migrated from "
		"Radarr that are still held, 1 movie is monitored and has no file: Axis would search for it. 1 movie has a "
		"file: nothing changes for it, because a file that was migrated is kept and is not replaced.");
	/* The pace, in plain numbers. */
	assert_contains(page.body, "every 15 minutes, so about 15 minutes for the 1 movie that is missing.");
	axis_test_reply_clear(&page);
	axis_test_plex_clear_routes(server);

	/* Radarr is downloading Ash and Ember. */
	axis_test_radarr_set_queue(fx->radarr, ash, G_N_ELEMENTS(ash));
	page = get_as(fx, fx->admin, "/migrate/1/start");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Of 2 movies migrated from Radarr that are still held,");
	assert_contains(page.body, "Radarr is downloading this 1: left out, and held until you start searching again. Ash "
		"and Ember (1996).");
	assert_contains(page.body, "Radarr still monitors 1 of these movies. Both would download them.");
	assert_contains(page.body, "3 movies stay held and are not started: Radarr has a file for them that Axis does not "
		"see");
	/* The truth about the files migrated: kept until released here. */
	assert_contains(page.body, "The files that were migrated from Radarr are kept: Axis does not upgrade or replace them, "
		"before or after you start, until you release them with Let Axis upgrade these files");
	assert_lacks(page.body, "is upgraded at the ordinary pace");
	save_page("start.html", page.body);
	axis_test_reply_clear(&page);
	/* Looking changed nothing. */
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==, BROUGHT_ODD);

	/* A viewer cannot start them. */
	posted = post_as(fx, fx->viewer, "/", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 403);
	axis_test_reply_clear(&posted);
	posted = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1?done=started");
	axis_test_reply_clear(&posted);
	/* The one Radarr is downloading stays held, for that; the ones whose
	 * file Axis does not see stay held; the others are free. */
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==,
		"Ash and Ember=arr_downloading/true/released/true/balanced "
		"Copper Kettle=file_unseen/true/released/true/balanced "
		"Marrow Lane=file_unseen/true/released/true/balanced "
		"Nine Winters=file_unseen/true/released/true/balanced "
		"Paper Lanterns=none/true/released/true/balanced "
		"The Quiet Orchard=none/false/released/true/balanced");
	/* Nothing was searched at once, and the scheduler leaves the held ones. */
	schedule(fx);
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 0);
	g_assert_cmpuint(search_jobs(fx, nine_id), ==, 0);
	page = get_as(fx, fx->admin, "/migrate/1?done=started");
	assert_contains(page.body, "Searching has been started for the movies migrated.");
	assert_contains(page.body, "1 movie was left out when searching was started: Radarr was downloading it.");
	axis_test_reply_clear(&page);

	/* Radarr does not answer: starting needs the person's statement. */
	axis_test_plex_route(server, "/api/v3/system/status", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	page = get_as(fx, fx->admin, "/migrate/1/start");
	assert_contains(page.body, "Radarr did not answer, so Axis could not check what it is downloading.");
	assert_contains(page.body, "arr-start-stopped");
	axis_test_reply_clear(&page);
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "Tick the box to start anyway.");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Ash and Ember");
	g_assert_cmpstr(found, ==, "arr_downloading");
	axis_test_plex_clear_routes(server);

	/* Radarr has finished: started again, the movie is free, and the
	 * scheduler's next run searches for it. */
	axis_test_radarr_set_queue(fx->radarr, ash, 0);
	posted = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Ash and Ember");
	g_assert_cmpstr(found, ==, "none");
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 0);
	schedule(fx);
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 1);
	g_assert_cmpuint(search_jobs(fx, nine_id), ==, 0);

	/* One movie, by a person: "Let Axis search for it" ends its hold and
	 * searches nothing at once; Search now ends it and searches. */
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/start-searching", copper_id);
	posted = post_as(fx, fx->viewer, "/", action, NULL);
	g_assert_cmpuint(posted.status, ==, 403);
	axis_test_reply_clear(&posted);
	posted = post_as(fx, fx->admin, "/films", action, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=started");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Copper Kettle");
	g_assert_cmpstr(found, ==, "none");
	g_assert_cmpuint(search_jobs(fx, copper_id), ==, 0);
	g_free(action);
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/search", nine_id);
	posted = post_as(fx, fx->admin, "/films", action, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Nine Winters");
	g_assert_cmpstr(found, ==, "none");
	g_assert_cmpuint(search_jobs(fx, nine_id), ==, 1);
	/* Marrow Lane, whose file Axis does not see, is held still. */
	g_free(found);
	found = hold_of(fx, "Marrow Lane");
	g_assert_cmpstr(found, ==, "file_unseen");
	/* Every request Radarr received was a GET. */
	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
}

/* Review fix 1: a movie held because its file was not seen, whose file a
 * person (or a scan and identification) linked to it since, is released from
 * that hold by a later run; the link stays as it is. */
static void
test_hold_lifted_when_linked_meanwhile(Fixture *fx, gconstpointer data)
{
	gint64 source = bring_odd_library(fx);
	g_autoptr(AxisScanResult) scan = NULL;
	g_autofree gchar *found = NULL;
	AxisTestReply page;

	found = hold_of(fx, "Copper Kettle");
	g_assert_cmpstr(found, ==, "file_unseen");
	write_file(fx, "Copper Kettle (2003)", "Copper Kettle (2003).mkv", "kettle123");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_file (file_id, movie_id, hand_how, hand_by, hand_at, kept)"
		" SELECT f.id, m.id, 'linked', 'root', now(), true FROM library_file f, movie m JOIN meta_title t ON t.id ="
		" m.title_id WHERE t.title = 'Copper Kettle' AND f.rel_path = 'Copper Kettle (2003)/Copper Kettle (2003).mkv'");
	read_from_page(fx, source, "complete", 60000);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "1 file of movies migrated before is in Axis's index now, and would be linked.");
	axis_test_reply_clear(&page);
	run_from_preview(fx, source);
	g_free(found);
	found = hold_of(fx, "Copper Kettle");
	g_assert_cmpstr(found, ==, "brought");
	/* The person's link stands, and no second one was made. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || min(mf.hand_how) || ' ' || min(mf.kept::text) FROM"
		" movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id = m.title_id WHERE t.title ="
		" 'Copper Kettle'");
	g_assert_cmpstr(found, ==, "1 linked true");
}

/* Review fix 2: a brought movie a person removed and added again is not
 * counted as one a run would link a file to. */
static void
test_removed_and_added_again(Fixture *fx, gconstpointer data)
{
	gint64 source = bring_odd_library(fx);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *found = NULL;
	AxisTestReply page;

	g_assert_true(axis_movies_remove(fx->movies, movie_of(fx, "Paper Lanterns"), "root", &error));
	g_assert_no_error(error);
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT"
		" e.title_id, (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT " FROM"
		" meta_external_id e WHERE e.kind = 'film' AND e.scheme = 'tmdb' AND e.value = '6001' RETURNING id",
		fx->root_id));
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN"
		" meta_title t ON t.id = m.title_id WHERE t.title = 'Paper Lanterns'");
	g_assert_cmpstr(found, ==, "0");
	read_from_page(fx, source, "complete", 60000);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_lacks(page.body, "would be linked");
	assert_contains(page.body, "There is nothing to migrate now");
	axis_test_reply_clear(&page);
}

/* Review fix 5: two entries of Radarr that are one title in Axis: a later
 * run links the file of the entry that was brought, not the other's. */
static void
test_two_entries_one_title(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(AxisScanResult) scan = NULL;
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *found = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	add_movie(json, 1, "Paper Lanterns", 2011, 6001, TRUE, 4, "/movies", 11);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	read_from_page(fx, source, "complete", 30000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	run_from_preview(fx, source);
	found = links(fx);
	g_assert_cmpstr(found, ==,
		"Paper Lanterns:Paper Lanterns (2011)/Paper Lanterns (2011).mkv:brought:root:true:true:true:bluray_1080p");

	/* Radarr lists the film again, under another TMDb number Axis knows as
	 * the same title, with a file of its own that is in the index now. */
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT e.title_id, 'film',"
		" 'tmdb', '6099' FROM meta_external_id e WHERE e.kind = 'film' AND e.scheme = 'tmdb' AND e.value = '6001'");
	write_file(fx, "Paper Lanterns Redux (2011)", "Paper Lanterns Redux (2011).mkv", "redux");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	g_string_truncate(json, json->len - 1);
	add_movie(json, 20, "Paper Lanterns Redux", 2011, 6099, TRUE, 4, "/movies", 5);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	read_from_page(fx, source, "complete", 30000);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_lacks(page.body, "would be linked");
	assert_contains(page.body, "There is nothing to migrate now");
	axis_test_reply_clear(&page);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM movie_file");
	g_assert_cmpstr(found, ==, "1");
}

/* Review fix 7: a brought file of no known quality is not searched for by
 * the scheduler after "Start searching" (it is in the library, not wanted
 * for an upgrade), while a movie without a file is. */
static void
test_unknown_quality_not_searched(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	g_autofree gchar *state_path = NULL;
	gint64 ash_id;
	gint64 paper_id;
	guint i;

	bring_odd_library(fx);
	ash_id = movie_of(fx, "Ash and Ember");
	paper_id = movie_of(fx, "Paper Lanterns");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET quality = NULL WHERE hand_how = 'brought'");
	posted = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Paper Lanterns");
	g_assert_cmpstr(found, ==, "none");
	for (i = 0; i < 3; i++)
		schedule(fx);
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 1);
	g_assert_cmpuint(search_jobs(fx, paper_id), ==, 0);
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", paper_id);
	page = get_as(fx, fx->admin, state_path);
	assert_contains(page.body, "In the library");
	assert_lacks(page.body, "below the cutoff");
	axis_test_reply_clear(&page);
}

static gchar *
kept_of_brought(Fixture *fx)
{
	return axis_test_db_query(fx->t->db, "SELECT string_agg(mf.kept || '/' || coalesce(mf.kept_how, '-'), ' ') FROM"
		" movie_file mf WHERE mf.hand_how = 'brought'");
}

/*
 * Decided on 2026-11-21 (the proposal's question 2), no automatic
 * upgrades: a file migrated is kept.  Below the cutoff of its profile, it
 * is searched for by nothing after "Start searching"; "Let Axis upgrade these
 * files" lets the connection's go (a confirmation that writes nothing, one
 * post with its digest), and then the scheduler searches for an upgrade.  A
 * link a person linked by hand, or kept by hand, is never let go by it.
 */
static void
test_brought_kept_until_released(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *state_path = NULL;
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *film_path = NULL;
	g_autofree gchar *keep = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *file = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	gint64 ash_id;
	gint64 paper_id;
	guint i;

	bring_odd_library(fx);
	ash_id = movie_of(fx, "Ash and Ember");
	paper_id = movie_of(fx, "Paper Lanterns");
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/brought");
	/* Every movie is out, and Paper Lanterns' file is below its profile's
	 * cutoff: only its being kept stands between it and an upgrade. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET quality = 'hdtv_720p' WHERE hand_how = 'brought'");
	posted = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Paper Lanterns");
	g_assert_cmpstr(found, ==, "none");
	for (i = 0; i < 3; i++)
		schedule(fx);
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 1);
	g_assert_cmpuint(search_jobs(fx, paper_id), ==, 0);
	/* Its state and its pages say why, and name no person as keeping it. */
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", paper_id);
	page = get_as(fx, fx->admin, state_path);
	assert_contains(page.body, "Migrated from Radarr; Axis does not replace it.");
	assert_lacks(page.body, "below the cutoff");
	assert_lacks(page.body, "kept by hand");
	axis_test_reply_clear(&page);
	links_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", paper_id);
	page = get_as(fx, fx->admin, links_path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Migrated from Radarr by root on ");
	assert_contains(page.body, "; Axis does not replace it");
	assert_lacks(page.body, "; kept by hand");
	axis_test_reply_clear(&page);
	{
		g_autofree gchar *title = axis_test_db_queryf(fx->t->db, "SELECT title_id FROM movie WHERE id = %"
			G_GINT64_FORMAT, paper_id);

		film_path = g_strdup_printf("/title/%s", title);
	}
	page = get_as(fx, fx->admin, film_path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Migrated from Radarr by root.");
	assert_contains(page.body, "Axis does not replace it: no automatic search and no Search now does");
	assert_contains(page.body, "Let Axis upgrade it again");
	axis_test_reply_clear(&page);

	/* The connection's page offers to let them go; the confirmation says how
	 * many and what happens then, and writes nothing. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "1 file migrated from Radarr is kept: Axis does not upgrade or replace it");
	assert_contains(page.body, "Let Axis upgrade these files");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/migrate/1/release");
	g_assert_cmpuint(page.status, ==, 403);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/release");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "1 file migrated from Radarr, of 1 movie, is kept now");
	assert_contains(page.body, "a file that is below the cutoff of its quality profile is replaced at the scheduler's "
		"ordinary pace, and the old file goes to the recycle folder.");
	assert_contains(page.body, "Let Axis upgrade 1 file");
	digest = page_digest(page.body);
	save_page("release.html", page.body);
	axis_test_reply_clear(&page);
	g_free(found);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/brought");
	/* A viewer cannot; a digest of no page is refused, and nothing changes. */
	form = g_strdup_printf("digest=%s", digest);
	posted = post_as(fx, fx->viewer, "/", "/migrate/1/release", form);
	g_assert_cmpuint(posted.status, ==, 403);
	axis_test_reply_clear(&posted);
	posted = post_as(fx, fx->admin, "/migrate/1/release", "/migrate/1/release",
		"digest=0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "Nothing was changed");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/brought");
	schedule(fx);
	g_assert_cmpuint(search_jobs(fx, paper_id), ==, 0);

	/* A person links a file by hand to Copper Kettle, a movie this connection
	 * brought, and keeps it: theirs, and not counted. */
	write_file(fx, "Copper Kettle (2003)", "Copper Kettle (2003).mkv", "kettle123");
	{
		g_autoptr(AxisScanResult) scan = axis_test_library_scan(fx->t, fx->root_id);

		g_assert_nonnull(scan);
	}
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_file (file_id, movie_id, hand_how, hand_by, hand_at, kept)"
		" SELECT f.id, m.id, 'linked', 'root', now(), true FROM library_file f, movie m JOIN meta_title t ON t.id ="
		" m.title_id WHERE t.title = 'Copper Kettle' AND f.rel_path = 'Copper Kettle (2003)/Copper Kettle (2003).mkv'");
	page = get_as(fx, fx->admin, "/migrate/1/release");
	assert_contains(page.body, "1 file migrated from Radarr, of 1 movie, is kept now");
	axis_test_reply_clear(&page);

	/* Confirmed: the brought link is let go, with a history line; the links a
	 * person made by hand (Copper Kettle's, a movie of this connection, and
	 * Tin Sparrow's), kept by that person, are as they were. */
	posted = post_as(fx, fx->admin, "/migrate/1/release", "/migrate/1/release", form);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1?done=released");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(t.title || ':' || mf.hand_how || ':' || mf.kept || ':' ||"
		" coalesce(mf.kept_how, '-'), ' | ' ORDER BY t.title) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id"
		" JOIN meta_title t ON t.id = m.title_id");
	g_assert_cmpstr(found, ==, "Copper Kettle:linked:true:- | Paper Lanterns:brought:false:- | Tin Sparrow:linked:true:-");
	g_free(found);
	found = axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
		" AND text LIKE 'The files migrated from Radarr (Radarr) were let go by root:%%'", paper_id);
	g_assert_cmpstr(found, ==, "1");
	/* Now it is wanted for an upgrade, and the scheduler searches for it. */
	page = get_as(fx, fx->admin, state_path);
	assert_contains(page.body, "below the cutoff of its quality profile");
	axis_test_reply_clear(&page);
	schedule(fx);
	g_assert_cmpuint(search_jobs(fx, paper_id), ==, 1);

	/* A person keeps that file by hand, on the movie's page: the keeping is
	 * theirs, and "Let Axis upgrade these files" has nothing to let go and
	 * never touches it, whatever is posted. */
	file = axis_test_db_query(fx->t->db, "SELECT file_id::text FROM movie_file WHERE hand_how = 'brought'");
	keep = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/keep", paper_id);
	g_free(form);
	form = g_strdup_printf("file=%s&keep=yes", file);
	posted = post_as(fx, fx->admin, film_path, keep, form);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/-");
	page = get_as(fx, fx->admin, "/migrate/1/release");
	assert_contains(page.body, "there is nothing to let go");
	axis_test_reply_clear(&page);
	g_free(digest);
	digest = NULL;
	g_free(form);
	form = g_strdup_printf("digest=%s", "0000000000000000000000000000000000000000000000000000000000000000");
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/release", form);
	g_assert_cmpuint(posted.status, ==, 409);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/-");
	/* The card offers it no more; forgetting the connection says what then
	 * lets a file go. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_lacks(page.body, "migrated from Radarr is kept: Axis does not upgrade or replace it");
	assert_lacks(page.body, "/migrate/1/release");
	assert_contains(page.body, "The files this connection migrated stay kept");
	assert_contains(page.body, "only the control of each file, on its title's page, lets a file go");
	axis_test_reply_clear(&page);
	axis_test_radarr_assert_read_only(fx->radarr);
}

/* A person's keep of a brought file is not the bringing over's: the digest of
 * the page that says so lets nothing go. */
static void
test_release_leaves_a_persons_keep(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *film_path = NULL;
	g_autofree gchar *keep = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *file = NULL;
	AxisTestReply posted;
	gint64 paper_id;

	bring_odd_library(fx);
	paper_id = movie_of(fx, "Paper Lanterns");
	film_path = g_strdup("/films");
	keep =g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/keep", paper_id);
	file = axis_test_db_query(fx->t->db, "SELECT file_id::text FROM movie_file WHERE hand_how = 'brought'");
	/* The person lets it go, then keeps it: kept, and theirs. */
	form = g_strdup_printf("file=%s&keep=no", file);
	posted = post_as(fx, fx->admin, film_path, keep, form);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "false/-");
	g_free(form);
	form = g_strdup_printf("file=%s&keep=yes", file);
	posted = post_as(fx, fx->admin, film_path, keep, form);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = kept_of_brought(fx);
	g_assert_cmpstr(found, ==, "true/-");
	/* The confirmation as it stands now offers nothing; a post made up with
	 * the digest of that very page lets nothing go either. */
	{
		AxisTestReply page = get_as(fx, fx->admin, "/migrate/1/release");

		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "there is nothing to let go");
		assert_lacks(page.body, "name=\"digest\"");
		axis_test_reply_clear(&page);
	}
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "release|1|0|0", -1);
	g_free(form);
	form = g_strdup_printf("digest=%s", digest);
	posted = post_as(fx, fx->admin, "/migrate/1/release", "/migrate/1/release", form);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=nothing-kept");
	axis_test_reply_clear(&posted);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(t.title || ':' || mf.hand_how || ':' || mf.kept, ' | '"
		" ORDER BY t.title) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id = m.title_id");
	g_assert_cmpstr(found, ==, "Paper Lanterns:brought:true | Tin Sparrow:linked:true");
}

/* The schema as it was before 0073_arr_brought_kept.sql, so that the
 * migration runs again. */
static void
before_0073(Fixture *fx)
{
	axis_test_db_exec(fx->t->db, "ALTER TABLE movie_file DROP COLUMN kept_how");
	axis_test_db_exec(fx->t->db, "ALTER TABLE series_file DROP COLUMN kept_how");
	axis_test_db_exec(fx->t->db, "ALTER TABLE arr_source DROP COLUMN run_note");
	axis_test_db_exec(fx->t->db, "ALTER TABLE arr_brought DROP COLUMN run_job");
}

/* 0073_arr_brought_kept.sql again: its statements, from its file.  (Not by
 * the migrator, which refuses a history with a gap: migrations have come
 * after this one, and they stay as they are.) */
static void
again_0073(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;

	g_assert_true(g_file_get_contents("data/migrations/0073_arr_brought_kept.sql", &sql, NULL, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, sql);
}

/* Identification goes through the library folder and a person confirms what
 * it read of Paper Lanterns' folder; then the odd library is read, mapped and
 * migrated.  So Add links Paper Lanterns' file as identification's before
 * the run looks at it: the state of a server whose library Axis had indexed
 * and identified before the move. */
static gint64
bring_identified_first(Fixture *fx)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *found = NULL;
	AxisIdentifyTuning tuning;
	AxisIdentifyReport report;
	AxisTestReply posted;
	guint i;

	odd_library(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	for (i = 0; i < 4; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		if (report.pending == 0 && report.waiting == 0)
			break;
	}
	unit = axis_test_db_query(fx->t->db, "SELECT id FROM library_identity WHERE rel_path LIKE 'Paper Lanterns%'"
		" ORDER BY id LIMIT 1");
	g_assert_nonnull(unit);
	found = axis_test_db_queryf(fx->t->db, "SELECT state FROM library_identity WHERE id = %s", unit);
	if (g_strcmp0(found, "linked") != 0) {
		g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(unit, NULL, 10), AXIS_IDENTIFY_CONFIRM, 0, "root",
			&error));
		g_assert_no_error(error);
	}
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM library_identity i JOIN meta_title t ON t.id = i.title_id"
		" WHERE t.title = 'Paper Lanterns' AND i.state = 'linked'");
	g_assert_cmpstr(found, ==, "1");
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	run_from_preview(fx, source);
	return source;
}

static gchar *
paper_link(Fixture *fx)
{
	return axis_test_db_query(fx->t->db, "SELECT coalesce(mf.hand_how, '-') || ':' || mf.kept || ':' ||"
		" coalesce(mf.kept_how, '-') || ':' || (mf.identity_id IS NULL) || ':' || (mf.import_id IS NULL) FROM movie_file mf"
		" JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id = m.title_id WHERE t.title = 'Paper Lanterns'");
}

/*
 * The review's blocker: a file Radarr has that identification linked first.
 * Add links it as identification's (not kept, no hand stamp); the run then
 * stamps it as migrated, and so kept, and counts it as linked.  Below the
 * cutoff, after "Start searching", three scheduler runs queue no upgrade.
 */
static void
test_identified_first_is_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;
	g_autofree gchar *state_path = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	gint64 paper_id;
	guint i;

	bring_identified_first(fx);
	paper_id = movie_of(fx, "Paper Lanterns");
	found = paper_link(fx);
	g_assert_cmpstr(found, ==, "brought:true:brought:true:true");
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT file FROM arr_brought WHERE label = 'Paper Lanterns (2011)'");
	g_assert_cmpstr(found, ==, "linked");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET quality = 'hdtv_720p'");
	posted = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(found);
	found = hold_of(fx, "Paper Lanterns");
	g_assert_cmpstr(found, ==, "none");
	for (i = 0; i < 3; i++)
		schedule(fx);
	g_assert_cmpuint(search_jobs(fx, paper_id), ==, 0);
	g_assert_cmpuint(search_jobs(fx, movie_of(fx, "Ash and Ember")), ==, 1);
	state_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", paper_id);
	page = get_as(fx, fx->admin, state_path);
	assert_contains(page.body, "Migrated from Radarr; Axis does not replace it.");
	assert_lacks(page.body, "below the cutoff");
	axis_test_reply_clear(&page);
}

/* 0073 for what identification had linked first: the database as R3 left
 * such a movie (its file identification's link, its row of arr_brought
 * saying 'already') is migrated, and the file is migrated and kept. */
static void
test_migration_keeps_identified(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;

	bring_identified_first(fx);
	axis_test_db_exec(fx->t->db, "UPDATE movie_file mf SET hand_how = NULL, hand_by = NULL, hand_at = NULL, kept = false,"
		" kept_how = NULL FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE m.id = mf.movie_id AND t.title ="
		" 'Paper Lanterns'");
	axis_test_db_exec(fx->t->db, "UPDATE arr_brought SET file = 'already' WHERE label = 'Paper Lanterns (2011)'");
	before_0073(fx);
	again_0073(fx);
	found = axis_test_db_query(fx->t->db, "SELECT coalesce(mf.hand_how, '-') || ':' || coalesce(mf.hand_by, '-') || ':' ||"
		" mf.kept || ':' || coalesce(mf.kept_how, '-') FROM movie_file mf JOIN movie m ON m.id = mf.movie_id"
		" JOIN meta_title t ON t.id = m.title_id WHERE t.title = 'Paper Lanterns'");
	g_assert_cmpstr(found, ==, "brought:root:true:brought");
}

/*
 * 0073_arr_brought_kept.sql protects what was brought before it: a database
 * as it was (a brought link with kept false, no kept_how column) is migrated,
 * and the brought link is kept because it was migrated, while a link a
 * person keeps is as it was.
 */
static void
test_migration_keeps_brought(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *found = NULL;

	bring_odd_library(fx);
	/* The database as slice R3 left it. */
	before_0073(fx);
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET kept = false WHERE hand_how = 'brought'");
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(mf.hand_how || ':' || mf.kept, ' ' ORDER BY mf.hand_how)"
		" FROM movie_file mf");
	g_assert_cmpstr(found, ==, "brought:false linked:true");
	again_0073(fx);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(mf.hand_how || ':' || mf.kept || ':' ||"
		" coalesce(mf.kept_how, '-'), ' ' ORDER BY mf.hand_how) FROM movie_file mf");
	g_assert_cmpstr(found, ==, "brought:true:brought linked:true:-");
	/* (The migrator's own record of 73 is covered by tests/test-db-migrate.c.) */
}

/* The run confirmed with the tick, and waited for; how its job ended. */
static gchar *
run_ticked(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *path = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/run", source);
	AxisTestReply page = get_as(fx, fx->admin, path);
	g_autofree gchar *digest = page_digest(page.body);
	g_autofree gchar *start = start_digest(page.body);
	g_autofree gchar *form = g_strdup_printf("digest=%s&start=%s", digest, start);
	g_autofree gchar *stale = g_strdup_printf("digest=%s&start=%s", digest, digest);
	g_autofree gchar *found = NULL;
	g_autofree gchar *before = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM job WHERE kind = 'arr.apply')"
		" + (SELECT count(*) FROM arr_brought)");
	AxisTestReply posted;

	axis_test_reply_clear(&page);
	g_assert_cmpstr(start, !=, digest);
	queue_start(fx);
	/* A tick that is not this run's (a made-up value): refused whole, and
	 * nothing is brought or started. */
	posted = post_as(fx, fx->admin, path, action, stale);
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "The preview changed after this page was made. Nothing was migrated and no search "
		"was started");
	axis_test_reply_clear(&posted);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM job WHERE kind = 'arr.apply') + (SELECT count(*)"
		" FROM arr_brought)");
	g_assert_cmpstr(found, ==, before);
	posted = post_as(fx, fx->admin, path, action, form);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=running-start");
	axis_test_reply_clear(&posted);
	return wait_run(fx);
}

/*
 * Decided on 2026-11-21 (the proposal's question 3), a tick on the run: with the
 * tick the run does at its end what "Start searching" does.  A movie Radarr
 * is downloading stays held for that, a movie whose file Axis does not see
 * stays held, the others are free; the page says how many.  The tick is in
 * the job, and a tick of another page is refused.
 */
static void
test_run_ticked_starts(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	static const gint64 ash[] = { 5 };
	g_autofree gchar *found = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	gint64 ash_id;
	gint64 quiet_id;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	tin_sparrow_holds_marrow_lane(fx);
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	/* Radarr is downloading Ash and Ember when the run ends. */
	axis_test_radarr_set_queue(fx->radarr, ash, G_N_ELEMENTS(ash));
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	found = axis_test_db_query(fx->t->db, "SELECT payload ->> 'start' FROM job WHERE kind = 'arr.apply'");
	g_assert_cmpstr(found, ==, "true");
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==,
		"Ash and Ember=arr_downloading/true/released/true/balanced "
		"Copper Kettle=file_unseen/true/released/true/balanced "
		"Marrow Lane=file_unseen/true/released/true/balanced "
		"Nine Winters=file_unseen/true/released/true/balanced "
		"Paper Lanterns=none/true/released/true/balanced "
		"The Quiet Orchard=none/false/released/true/balanced");
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT search_started_at IS NOT NULL FROM arr_source WHERE id = 1");
	g_assert_cmpstr(found, ==, "t");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "Searching was started at the end of the last run, as ticked on its confirmation: for 2 "
		"movies of that run. 1 was left out and stays held: Radarr is downloading it.");
	assert_contains(page.body, "3 movies are held apart: Radarr has a file for them that Axis does not see.");
	axis_test_reply_clear(&page);
	/* Nothing was searched at once; the scheduler leaves the held one, and
	 * the file migrated, which is kept. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie_search) + (SELECT count(*) FROM job WHERE"
		" kind LIKE 'movies.%')");
	g_assert_cmpstr(found, ==, "0");
	ash_id = movie_of(fx, "Ash and Ember");
	quiet_id = movie_of(fx, "The Quiet Orchard");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	axis_test_db_exec(fx->t->db, "UPDATE movie SET monitored = true");
	schedule(fx);
	g_assert_cmpuint(search_jobs(fx, ash_id), ==, 0);
	g_assert_cmpuint(search_jobs(fx, movie_of(fx, "Nine Winters")), ==, 0);
	g_assert_cmpuint(search_jobs(fx, movie_of(fx, "Paper Lanterns")), ==, 0);
	g_assert_cmpuint(search_jobs(fx, quiet_id), ==, 1);
	/* Every request Radarr received was a GET. */
	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
}

/* The tick, and Radarr does not answer when the run ends: nobody is there to
 * say it is stopped, so nothing is started, everything is held, and the page
 * says so. */
static void
test_run_ticked_queue_unreadable(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	AxisTestPlex *server = axis_test_radarr_server(fx->radarr);
	g_autofree gchar *found = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	tin_sparrow_holds_marrow_lane(fx);
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	axis_test_plex_route(server, "/api/v3/system/status", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	axis_test_plex_clear_routes(server);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==, BROUGHT_ODD);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT search_started_at IS NULL FROM arr_source WHERE id = 1");
	g_assert_cmpstr(found, ==, "t");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "Searching was not started at the end of the last run, though its confirmation was "
		"ticked: Radarr did not answer, so Axis could not check what it is downloading. Everything migrated is "
		"held.");
	assert_contains(page.body, "1 movie is monitored and has no file. Axis does not search for it until you start "
		"searching.");
	assert_contains(page.body, "3 movies are held apart: Radarr has a file for them that Axis does not see.");
	axis_test_reply_clear(&page);
	axis_test_radarr_assert_read_only(fx->radarr);
}

/* "These": a run confirmed with the tick starts the movies it brought itself.
 * What an earlier run brought stays held until "Start searching". */
static void
test_run_ticked_starts_only_its_own(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	static const gint64 none[] = { 0 };
	g_autofree gchar *found = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	odd_library(fx);
	axis_test_radarr_set_queue(fx->radarr, none, 0);
	read_from_page(fx, source, "complete", 60000);
	tin_sparrow_holds_marrow_lane(fx);
	/* A first run, not ticked: the five monitored movies, all held. */
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	run_from_preview(fx, source);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || count(*) FILTER (WHERE search_hold IS NOT NULL)"
		" FROM movie WHERE added_by = 'root'");
	g_assert_cmpstr(found, ==, "5 5");
	/* A second run, ticked, brings the one movie Radarr does not monitor. */
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	/* The preview says what the run does not touch: what is in Axis already. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "in Axis already. This run leaves them exactly as they are: Axis's ordinary rules go on "
		"applying to their files, which this run does not migrate and so does not keep.");
	axis_test_reply_clear(&page);
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	g_free(found);
	found = brought_movies(fx);
	g_assert_cmpstr(found, ==,
		"Ash and Ember=brought/true/released/true/balanced "
		"Copper Kettle=file_unseen/true/released/true/balanced "
		"Marrow Lane=file_unseen/true/released/true/balanced "
		"Nine Winters=file_unseen/true/released/true/balanced "
		"Paper Lanterns=brought/true/released/true/balanced "
		"The Quiet Orchard=none/false/released/true/balanced");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "as ticked on its confirmation: for 1 movie of that run.");
	assert_contains(page.body, "Start searching for the movies without a file");
	axis_test_reply_clear(&page);
}

/* A seam of the run: from its second movie on, the library folder is no
 * folder for movies, so the module refuses the rest. */
static gboolean
refuse_from_the_second(
	guint     index,
	gpointer  data,
	GError  **error
){
	Fixture *fx = data;

	/* (On the pool: this is the job's thread.) */
	if (index == 1)
		return axis_db_pool_exec(fx->t->pool, "UPDATE library_root SET media_type = 'tv'", 0, NULL, NULL, error);
	return TRUE;
}

/* A ticked run that could not bring every movie: the searches of what it did
 * bring are started, and the note says so and what was refused. */
static void
test_run_ticked_with_refusals(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	static const gint64 none[] = { 0 };
	g_autofree gchar *found = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	odd_library(fx);
	axis_test_radarr_set_queue(fx->radarr, none, 0);
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	axis_arr_apply_hook = refuse_from_the_second;
	axis_arr_apply_hook_data = fx;
	state = run_ticked(fx, source);
	axis_arr_apply_hook = NULL;
	axis_arr_apply_hook_data = NULL;
	axis_test_db_exec(fx->t->db, "UPDATE library_root SET media_type = 'movies'");
	g_assert_cmpstr(state, ==, "failed");
	found = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || count(*) FILTER (WHERE m.search_hold IS NULL)"
		" FROM arr_brought b JOIN movie m ON m.id = b.movie_id");
	g_assert_cmpstr(found, ==, "1 1");
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT run_note FROM arr_source WHERE id = 1");
	g_assert_nonnull(found);
	assert_contains(found, "Searching was started at the end of the last run, as ticked on its confirmation: for 1 movie "
		"of that run.");
	assert_contains(found, "5 movies were not migrated: That is not a library folder for movies.");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "for 1 movie of that run.");
	assert_contains(page.body, "5 movies were not migrated");
	axis_test_reply_clear(&page);
}

/* Spec (bring-over): Interrupted.  Task 3.9: a run that a database error
 * ends in the middle leaves whole movies, and going on brings the rest and
 * none twice. */
static void
test_run_interrupted(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GString) json = g_string_new("[");
	g_autofree gchar *found = NULL;
	g_autofree gchar *digest = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	guint n = 60;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO meta_title (kind, title, year, provider, provider_id) SELECT"
		" 'film', 'Made Up Film ' || g, 2001, 'fixture', (100000 + g)::text FROM generate_series(1, %u) g RETURNING 1",
		n));
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'film',"
		" 'tmdb', provider_id FROM meta_title WHERE provider = 'fixture'");
	for (i = 1; i <= n; i++) {
		g_autofree gchar *title = g_strdup_printf("Made Up Film %u", i);

		add_movie(json, i, title, 2001, 100000 + i, TRUE, 4, "/movies", -1);
	}
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);

	/* The database fails at the twenty-first movie, each time it is tried. */
	axis_test_db_exec(fx->t->db, "CREATE FUNCTION test_arr_stall() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN"
		" IF (SELECT count(*) FROM arr_brought) >= 20 THEN RAISE EXCEPTION 'the database went away'; END IF;"
		" RETURN NEW; END $$");
	axis_test_db_exec(fx->t->db, "CREATE TRIGGER test_arr_stall BEFORE INSERT ON arr_brought FOR EACH ROW EXECUTE"
		" FUNCTION test_arr_stall()");
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Migrate 60 movies");
	digest = page_digest(page.body);
	axis_test_reply_clear(&page);
	queue_start(fx);
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply' AND attempts >= 1"
		" AND state IN ('queued', 'failed')", "1", 60000));
	/* Whole movies, never half of one: twenty, each with its record and its
	 * one history line, and no twenty-first. */
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) || ' ' || (SELECT count(*) FROM"
		" arr_brought) || ' ' || (SELECT count(*) FROM movie_event WHERE kind = 'added') || ' ' || (SELECT count(*)"
		" FROM movie WHERE search_hold IS NULL)");
	g_assert_cmpstr(found, ==, "20 20 20 0");

	/* The database is back: the queue's retry goes on, or (when its tries
	 * are spent) the person confirms again. */
	axis_test_db_exec(fx->t->db, "DROP TRIGGER test_arr_stall ON arr_brought");
	axis_test_db_exec(fx->t->db, "DROP FUNCTION test_arr_stall()");
	axis_test_db_exec(fx->t->db, "UPDATE job SET run_at = now() WHERE kind = 'arr.apply' AND state = 'queued'");
	g_free(found);
	found = wait_run(fx);
	if (g_strcmp0(found, "succeeded") != 0) {
		page = get_as(fx, fx->admin, "/migrate/1");
		assert_contains(page.body, "The last run did not finish. What it migrated stays.");
		axis_test_reply_clear(&page);
		run_from_preview(fx, source);
	}
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) || ' ' || (SELECT count(DISTINCT title_id)"
		" FROM movie) || ' ' || (SELECT count(*) FROM arr_brought) || ' ' || (SELECT count(*) FROM movie_event WHERE"
		" kind = 'added') || ' ' || (SELECT count(*) FROM movie WHERE search_hold = 'brought')");
	g_assert_cmpstr(found, ==, "60 60 60 60 60");
}

/* Spec (library-identification): The run reads the folder otherwise.  A run
 * of identification after bringing over leaves every brought link as it is,
 * where it reads the folder as the same movie and where it reads it as
 * another movie Axis manages; and it does not take the folder's other file
 * to the movie the brought file is linked to. */
static void
test_identification_leaves_brought(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(AxisScanResult) scan = NULL;
	g_autoptr(GString) json = g_string_new("[");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *found = NULL;
	AxisIdentifyTuning tuning;
	AxisIdentifyReport report;
	AxisTestReply posted;
	guint linked = 0;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	/* A folder named for Tin Sparrow holds two files; Radarr says the first
	 * is Marrow Lane's (years of the person's use say so). */
	write_file(fx, "Tin Sparrow (2001)", "Tin Sparrow (2001).mkv", "sparrow1");
	write_file(fx, "Tin Sparrow (2001)", "Tin Sparrow (2001) 720p.mkv", "sparrow-2");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	add_movie(json, 1, "Paper Lanterns", 2011, 6001, TRUE, 4, "/movies", 11);
	g_string_append(json, ",{\"id\":2,\"title\":\"Marrow Lane\",\"year\":1990,\"tmdbId\":6010,\"monitored\":true,"
		"\"qualityProfileId\":4,\"minimumAvailability\":\"released\",\"path\":\"/movies/Tin Sparrow (2001)\","
		"\"rootFolderPath\":\"/movies\",\"hasFile\":true,\"movieFile\":{\"path\":\"/movies/Tin Sparrow (2001)/"
		"Tin Sparrow (2001).mkv\",\"size\":8,\"quality\":{\"quality\":{\"name\":\"DVD\"}}}}");
	add_movie(json, 3, "Tin Sparrow", 2001, 6007, TRUE, 4, "/movies", -1);
	g_string_append_c(json, ']');
	axis_test_radarr_set_movies(fx->radarr, json->str);
	axis_test_radarr_set_profiles(fx->radarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	run_from_preview(fx, source);
	before = links(fx);
	g_assert_cmpstr(before, ==,
		"Marrow Lane:Tin Sparrow (2001)/Tin Sparrow (2001).mkv:brought:root:true:true:true:dvd | "
		"Paper Lanterns:Paper Lanterns (2011)/Paper Lanterns (2011).mkv:brought:root:true:true:true:bluray_1080p");

	/* Identification runs over the whole library folder. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	for (i = 0; i < 4; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		linked += report.linked;
		if (report.pending == 0 && report.waiting == 0)
			break;
	}
	/* It did decide: the folders it is sure of are linked to their titles,
	 * and it reads Tin Sparrow's folder as Tin Sparrow, which Axis manages. */
	/* The run read every folder by its names (the files are not probed, so
	 * it is not sure and leaves them for a person), and the brought links are
	 * as they were. */
	g_assert_cmpuint(report.examined + linked, >=, 1);
	after = axis_test_db_query(fx->t->db, "SELECT count(*) FROM library_identity WHERE state = 'review'");
	g_assert_cmpint(g_ascii_strtoll(after, NULL, 10), >=, 4);
	g_clear_pointer(&after, g_free);
	after = links(fx);
	g_assert_cmpstr(after, ==, before);
	g_clear_pointer(&after, g_free);
	/* A person confirms what it read: each folder is linked to the title of
	 * its name, Tin Sparrow's folder to Tin Sparrow, which Axis manages. */
	{
		g_autofree gchar *ids = axis_test_db_query(fx->t->db, "SELECT string_agg(id::text, ',' ORDER BY id) FROM"
			" library_identity WHERE state = 'review' AND rel_path <> 'elsewhere'");
		g_auto(GStrv) each = g_strsplit(ids != NULL ? ids : "", ",", -1);

		g_assert_cmpuint(g_strv_length(each), ==, 4);
		for (i = 0; each[i] != NULL; i++) {
			g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(each[i], NULL, 10), AXIS_IDENTIFY_CONFIRM, 0,
				"root", &error));
			g_assert_no_error(error);
		}
	}
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(DISTINCT t.title, ',' ORDER BY t.title) FROM"
		" library_identity i JOIN meta_title t ON t.id = i.title_id WHERE i.state = 'linked' AND i.rel_path LIKE"
		" 'Tin Sparrow%'");
	g_assert_cmpstr(found, ==, "Tin Sparrow");

	/* Every brought link is as it was: the same file, the same movie, the
	 * same mark, no identity put on it. */
	after = axis_test_db_query(fx->t->db, "SELECT string_agg(t.title || ':' || f.rel_path || ':' ||"
		" coalesce(mf.hand_how, '-') || ':' || coalesce(mf.hand_by, '-') || ':' || mf.kept || ':' ||"
		" (mf.import_id IS NULL) || ':' || (mf.identity_id IS NULL) || ':' || coalesce(mf.quality, '-'), ' | '"
		" ORDER BY t.title, f.rel_path) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON"
		" t.id = m.title_id JOIN library_file f ON f.id = mf.file_id WHERE mf.hand_how = 'brought'");
	g_assert_cmpstr(after, ==, before);
	/* The folder's other file went where identification read it (Tin
	 * Sparrow), not to the movie the brought file is linked to. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(t.title, ',' ORDER BY t.title) FROM movie_file mf JOIN"
		" movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id = m.title_id JOIN library_file f ON f.id = mf.file_id"
		" WHERE f.rel_path = 'Tin Sparrow (2001)/Tin Sparrow (2001) 720p.mkv'");
	g_assert_cmpstr(found, ==, "Tin Sparrow");
	/* Marrow Lane has the file that was migrated and the file of its own
	 * folder, which identification linked: nothing else. */
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT string_agg(f.rel_path || ':' || coalesce(mf.hand_how, 'identified'),"
		" ' | ' ORDER BY f.rel_path) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id ="
		" m.title_id JOIN library_file f ON f.id = mf.file_id WHERE t.title = 'Marrow Lane'");
	g_assert_cmpstr(found, ==, "Marrow Lane (1990)/Marrow Lane (1990).mkv:identified | Tin Sparrow (2001)/Tin Sparrow"
		" (2001).mkv:brought");
}

/* Task 3.9: bringing over works while storage is read-only (it writes to
 * the database only); and a disk gone offline between the preview and the
 * run refuses the confirmation made before. */
static void
test_run_read_only_and_offline(Fixture *fx, gconstpointer data)
{
	gint64 source = add_source(fx, "Radarr", NULL, KEY);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *found = NULL;
	AxisTestReply posted;
	AxisTestReply page;

	odd_library(fx);
	read_from_page(fx, source, "complete", 60000);
	posted = map_movies(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Migrate 5 movies");
	digest = page_digest(page.body);
	axis_test_reply_clear(&page);
	queue_start(fx);

	/* The disk goes after the page was made: its confirmation is refused. */
	axis_test_db_exec(fx->t->db, "UPDATE volume SET state = 'offline', state_reason = 'path_missing'");
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "Disk offline (5)");
	axis_test_reply_clear(&posted);
	found = axis_test_db_query(fx->t->db, "SELECT count(*) FROM movie");
	g_assert_cmpstr(found, ==, "0");

	/* It is back, mounted read-only, and storage is read-only: the run
	 * brings the movies all the same. */
	axis_test_db_exec(fx->t->db, "UPDATE volume SET state = 'read_only', state_reason = 'none'");
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	g_assert_no_error(error);
	run_from_preview(fx, source);
	g_free(found);
	found = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM movie) || ' ' || (SELECT count(*) FROM"
		" movie_file WHERE hand_how = 'brought')");
	g_assert_cmpstr(found, ==, "5 2");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web-arr/" path, Fixture, NULL, func)
	ADD("connections/administrators-only", test_administrators_only);
	ADD("connections/old-address-redirects", test_old_address_redirects);
	ADD("connections/add-edit-forget", test_add_edit_forget);
	ADD("connections/test", test_the_connection);
	ADD("connections/not-a-get-fails", test_not_a_get_fails_without_sending);
	ADD("reading/read-and-preview", test_read_and_preview);
	ADD("reading/disk-offline", test_disk_offline);
	ADD("reading/paging", test_paging);
	ADD("reading/fails", test_reading_fails);
	ADD("reading/forget-during-reading", test_forget_during_reading);
	ADD("reading/limits", test_reading_limits);
	ADD("reading/failed-plans-keep-nothing", test_failed_plans_keep_nothing);
	ADD("reading/result-of-another-address", test_result_of_another_address);
	ADD("reading/save-without-a-plan", test_save_without_a_plan);
	ADD("reading/listed-twice", test_listed_twice);
	ADD("reading/eight-thousand", test_eight_thousand);
	ADD("run/suggested", test_run_suggested);
	ADD("run/brings", test_run_brings);
	ADD("run/second-run", test_second_run);
	ADD("run/hold-and-start", test_hold_and_start);
	ADD("run/hold-lifted-when-linked-meanwhile", test_hold_lifted_when_linked_meanwhile);
	ADD("run/removed-and-added-again", test_removed_and_added_again);
	ADD("run/two-entries-one-title", test_two_entries_one_title);
	ADD("run/unknown-quality-not-searched", test_unknown_quality_not_searched);
	ADD("run/brought-kept-until-released", test_brought_kept_until_released);
	ADD("run/release-leaves-a-persons-keep", test_release_leaves_a_persons_keep);
	ADD("run/migration-keeps-brought", test_migration_keeps_brought);
	ADD("run/ticked-starts", test_run_ticked_starts);
	ADD("run/ticked-queue-unreadable", test_run_ticked_queue_unreadable);
	ADD("run/ticked-starts-only-its-own", test_run_ticked_starts_only_its_own);
	ADD("run/ticked-with-refusals", test_run_ticked_with_refusals);
	ADD("run/identified-first-is-kept", test_identified_first_is_kept);
	ADD("run/migration-keeps-identified", test_migration_keeps_identified);
	ADD("run/interrupted", test_run_interrupted);
	ADD("run/identification-leaves-brought", test_identification_leaves_brought);
	ADD("run/read-only-and-offline", test_run_read_only_and_offline);
#undef ADD
	return g_test_run();
}
