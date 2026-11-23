/*
 * test-web-sonarr-run.c - "Bring over from Sonarr", slice S2: one confirmed
 * run that brings series over from the preview, with the real shell on an
 * ephemeral loopback port, a stand-in Sonarr, a stand-in details provider and
 * a stand-in TheTVDB on loopback, a real database, the TV module, real
 * identification, and a real library folder for series with its index
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every title here is made up.  What is proved
 * (openspec/specs/arr-migration/spec.md, and the specs tv and
 * library-identification):
 *   run/brings        a run brings the planned series with the mapped profile
 *                     and folder under TheTVDB's aired order from the start;
 *                     the monitoring of the series, each season and each
 *                     episode copied, and of seasons that come later; files
 *                     linked, a double episode's on both its episodes; an
 *                     episode Sonarr has and the aired order lacks, and one
 *                     whose TheTVDB id disagrees, reported and made into
 *                     nothing, the file of one not linked; each reason a file
 *                     is left alone; the episodes held apart; nothing on disk
 *                     changed (path, inode, size, time, content); no search
 *                     queued; no movie made; Sonarr only read
 *   run/name          a brought file whose name states another episode than
 *                     the one Sonarr had it on is not listed as misplaced,
 *                     identification afterwards leaves every brought link as
 *                     it is, and the links page says what its name states
 *   run/identified    a file identification linked by its name before the run,
 *                     on another episode than Sonarr has it on, is moved to
 *                     Sonarr's episode and stamped brought; identification
 *                     does not move it back; the others are left and counted
 *   run/second        a second run brings only what is new, links a file that
 *                     became visible, leaves what a person changed, and does
 *                     not bring back a series removed in Axis
 *   run/twice         a place Sonarr lists twice matches nothing, in the
 *                     preview as in the run
 *   run/renumbered    a series switched to another numbering since is linked
 *                     nothing by a later run, and the preview says so
 *   run/another-way   an episode held apart that gets a file another way is
 *                     not held any more
 *   run/hold          the hold stops the scheduler through three runs and
 *                     every other way a search is queued; a person's search
 *                     of one episode or one season, and a manual search, pass
 *                     it and leave the series held and the scheduler quiet;
 *                     the search job passes by its cause; Search now for the
 *                     whole series ends the series' hold and no episode's;
 *                     "Start searching" ends it for the connection's series,
 *                     at the scheduler's pace; an episode whose file Axis does
 *                     not see stays held, also one at a place Sonarr lists
 *                     under another TheTVDB id, through three scheduler runs
 *   run/refusals      a stale digest, a viewer, and a Radarr connection that
 *                     cannot reach the series run
 *   run/interrupted   a run that fails half-way is taken up by the queue's
 *                     retry: every series whole, none twice
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
#include "axis-test-download.h"
#include "axis-test-identify.h"
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
#include "db/axis-db-migrate.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/tv/axis-tv.h"
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
	AxisWebQuality      *quality_ui;
	AxisIndexers        *indexers;
	AxisDownloads       *downloads;
	AxisQualityStore    *quality;
	AxisIdentify        *identify;
	AxisTv              *tv;
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
	gchar               *profile;	/* the name of one of Axis's quality profiles */
} Fixture;

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

static void
scan(Fixture *fx)
{
	g_autoptr(AxisScanResult) result = axis_test_library_scan(fx->t, fx->root_id);

	while (g_main_context_iteration(NULL, FALSE))
		;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIdentifyTuning identify_tuning;
	AxisArrTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	/* The stand-ins say in the log what they do not know. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "shows", AXIS_MEDIA_TV);
	fx->shows_dir = g_build_filename(fx->disk, "shows", NULL);
	/* What is on the disk, as Axis sees it below its library folder. */
	write_file(fx, "Lantern Row (2015)/Season 1", "Lantern Row - S01E01 - Low Water.mkv", "lowwater1");
	write_file(fx, "Lantern Row (2015)/Season 1", "Lantern Row - S01E02-E03 - The Ropes.mkv", "ropes12");
	/* Named for episode 4; Sonarr has it as episode 5. */
	write_file(fx, "Lantern Row (2015)/Season 1", "Lantern Row - S01E04 - Scene Numbered.mkv", "scene5x");
	write_file(fx, "Lantern Row (2015)/Season 1", "Lantern Row - S01E09 - Beyond.mkv", "beyond");
	write_file(fx, "Harrow Fields (2010)/Season 1", "Harrow Fields - S01E01.mkv", "elsewhere");
	write_file(fx, "Harrow Fields (2010)/Season 1", "Harrow Fields - S01E02.mkv", "unlinked");
	write_file(fx, "Harrow Fields (2010)/Season 1", "Harrow Fields - S01E03.mkv", "short");
	write_file(fx, "Harrow Fields (2010)/Season 1", "Harrow Fields - S01E04.mkv", "fine4");
	scan(fx);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	/* The details provider (in TMDb's place): the one identification's tests
	 * use, which knows these series by name and by TheTVDB identifier. */
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, axis_test_identify_provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	g_assert_no_error(error);
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	g_assert_no_error(error);

	fx->sonarr = axis_test_sonarr_new(KEY);
	fx->radarr = axis_test_radarr_new(RADARR_KEY);
	fx->arr = axis_arr_new(fx->t->pool, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.list_timeout_ms = 20000;
	tuning.series_timeout_ms = 20000;
	tuning.lookup_wait_ms = 5000;
	tuning.requests_per_second = 100000;
	tuning.provider = "fixture";
	axis_arr_set_tuning(fx->arr, &tuning);
	fx->ui = axis_web_arr_new(fx->arr);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_arr_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_TV_MODULE_ID, axis_tv_get_module()->settings, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IDENTIFY_MODULE_ID, axis_identify_get_setting_table(),
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_RATE, "50", &error));
	g_assert_no_error(error);

	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&identify_tuning, 0, sizeof(identify_tuning));
	identify_tuning.wait_ms = 6000;
	identify_tuning.budget_ms = 120000;
	identify_tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &identify_tuning);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_tv_set_identify(fx->tv, fx->identify);
	/* What a run of a Sonarr brings series through. */
	axis_arr_set_tv(fx->arr, fx->tv);
	fx->profile = axis_test_db_query(fx->t->db, "SELECT name FROM quality_profile ORDER BY id LIMIT 1");
	g_assert_nonnull(fx->profile);

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
	axis_arr_apply_hook = NULL;
	axis_arr_apply_hook_data = NULL;
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
	axis_identify_unref(fx->identify);
	axis_tv_unref(fx->tv);
	axis_test_sonarr_free(fx->sonarr);
	axis_test_radarr_free(fx->radarr);
	axis_downloads_unref(fx->downloads);
	axis_indexers_unref(fx->indexers);
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
	g_free(fx->profile);
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

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *unbroken = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;
	g_autoptr(GString) text = g_string_new(unbroken != NULL ? unbroken : "");

	g_string_replace(text, "&#39;", "'", 0);
	g_string_replace(text, "&apos;", "'", 0);
	g_string_replace(text, "&#x27;", "'", 0);
	g_string_replace(text, "&amp;", "&", 0);
	if (strstr(text->str, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

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
	g_assert_cmpstr(module->job_handlers[2].kind, ==, AXIS_ARR_APPLY_KIND);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[2], fx->ui, &error));
	g_assert_no_error(error);
	fx->queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

static gint64
add_sonarr(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_sonarr_base(fx->sonarr);
	AxisArrSourceSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.app = "sonarr";
	spec.name = "Sonarr";
	spec.address = base;
	spec.secret = KEY;
	spec.user = "root";
	g_assert_true(axis_arr_save_source(fx->arr, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

/* "Read what Sonarr has", from the page, to the end. */
static void
read_from_page(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT, source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/read", source);
	g_autofree gchar *n_before = axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM arr_plan WHERE source_id = %"
		G_GINT64_FORMAT " AND state = 'complete'", source);
	g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM arr_plan p WHERE p.source_id = %" G_GINT64_FORMAT
		" AND p.state = 'complete' AND p.id = (SELECT max(id) FROM arr_plan) AND NOT EXISTS (SELECT 1 FROM job j"
		" WHERE j.kind = 'arr.read' AND j.state IN ('queued', 'running'))", source);
	AxisTestReply posted;

	queue_start(fx);
	posted = post_as(fx, fx->admin, page, action, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	if (!axis_test_wait_query(fx->t->db, sql, "1", 90000)) {
		g_autofree gchar *problem = axis_test_db_query(fx->t->db, "SELECT coalesce(problem, state) FROM arr_plan"
			" ORDER BY id DESC LIMIT 1");

		g_error("the reading did not complete: %s", problem);
	}
}

/* Maps Sonarr's /tv to the library folder, from the page.  (The quality
 * profile needs no choice: Sonarr's has the name of one of Axis's.) */
static void
map_tv(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	g_autofree gchar *form = g_strdup_printf("profile_ids=&folder_rows=1&folder_prefix_0=%%2Ftv&folder_root_0=%"
		G_GINT64_FORMAT "&folder_below_0=", fx->root_id);
	AxisTestReply posted = post_as(fx, fx->admin, page, page, form);

	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
}

static gchar *
digest_now(
	Fixture *fx,
	gint64   source
){
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_no_error(error);
	g_assert_nonnull(preview->digest);
	return g_strdup(preview->digest);
}

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

/* Confirms the preview as it stands and waits for the run; how it ended. */
static gchar *
run_from_preview(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *digest = digest_now(fx, source);
	AxisTestReply posted;

	queue_start(fx);
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=running");
	axis_test_reply_clear(&posted);
	if (!axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'"
	        " AND state IN ('queued', 'running')", "0", 120000))
		g_error("the run did not end");
	return axis_test_db_query(fx->t->db, "SELECT CASE WHEN state = 'succeeded' THEN 'succeeded' ELSE state::text ||"
		" coalesce(': ' || last_error, '') END FROM job WHERE kind = 'arr.apply' ORDER BY id DESC LIMIT 1");
}

static void
run_ok(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *state = run_from_preview(fx, source);

	g_assert_cmpstr(state, ==, "succeeded");
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

	collect_files(fx->shows_dir, "", lines);
	g_ptr_array_sort(lines, compare_lines);
	g_ptr_array_add(lines, NULL);
	return g_strjoinv("\n", (gchar **)lines->pdata);
}

/* ── The library in Sonarr ──────────────────────────────────────── */

static void
add_series(
	GString     *json,
	gint64       id,
	const gchar *title,
	gint         year,
	gint64       tvdb,
	gboolean     monitored,
	const gchar *profile,
	const gchar *new_items,	/* "all", "none", or NULL for a Sonarr 3 */
	const gchar *seasons
){
	g_autofree gchar *later = new_items != NULL ? g_strdup_printf("\"monitorNewItems\":\"%s\",", new_items) :
		g_strdup("");

	(void)profile;
	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"title\":\"%s\",\"year\":%d,\"tvdbId\":%"
		G_GINT64_FORMAT ",\"monitored\":%s,\"seriesType\":\"standard\",\"qualityProfileId\":4,%s"
		"\"path\":\"/tv/%s (%d)\",\"rootFolderPath\":\"/tv\",\"seasons\":%s}", json->len > 1 ? "," : "", id, title,
		year, tvdb, monitored ? "true" : "false", later, title, year, seasons);
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

/* TheTVDB's aired order of series @tvdb: @places is "S:E:ID" entries, space
 * between. */
static void
tvdb_order(
	Fixture     *fx,
	gint64       tvdb,
	const gchar *places
){
	g_auto(GStrv) each = g_strsplit(places, " ", -1);
	g_autofree gchar *path = g_strdup_printf("/v4/series/%" G_GINT64_FORMAT "/episodes/official/eng", tvdb);
	g_autoptr(GString) json = g_string_new(NULL);
	guint i;

	g_string_append_printf(json, "{\"status\":\"success\",\"data\":{\"series\":{\"id\":%" G_GINT64_FORMAT
		"},\"episodes\":[", tvdb);
	for (i = 0; each[i] != NULL; i++) {
		gint season = 0;
		gint number = 0;
		gint64 id = 0;

		g_assert_cmpint(sscanf(each[i], "%d:%d:%" G_GINT64_FORMAT, &season, &number, &id), ==, 3);
		g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"seriesId\":%" G_GINT64_FORMAT ",\"name\":\"A Made"
			" Up Episode %u\",\"aired\":\"2025-01-%02u\",\"runtime\":24,\"seasonNumber\":%d,\"number\":%d,"
			"\"absoluteNumber\":%u}", i > 0 ? "," : "", id, tvdb, i + 1, i % 27 + 1, season, number,
			season > 0 ? i + 1 : 0);
	}
	g_string_append(json, "]},\"links\":{\"prev\":null,\"next\":null}}");
	axis_test_tvdb_set_answer(fx->tvdb, path, "page=0", json->str);
}

#define SEASONS_012 "[{\"seasonNumber\":0,\"monitored\":false},{\"seasonNumber\":1,\"monitored\":true}," \
	"{\"seasonNumber\":2,\"monitored\":false}]"
#define SEASON_1 "[{\"seasonNumber\":1,\"monitored\":true}]"

/*
 * What Sonarr has.  1 Lantern Row: one of everything an episode can be.  2
 * Harrow Fields: each reason a file is left alone.  3 Glass Harbor: not
 * monitored (so not brought; a test adds it to Axis by hand).  4 Kestrel
 * Point: two monitored episodes, no file, and a Sonarr 3's silence about new
 * seasons.  With @more, 5 Juniper Hollow as well, and Lantern Row's file of
 * season 2 is on the disk.
 */
static void
library_full(
	Fixture  *fx,
	gboolean  more,
	gboolean  twice
){
	g_autoptr(GString) series = g_string_new("[");
	g_autoptr(GString) episodes = g_string_new("[");
	g_autoptr(GString) files = g_string_new("[");
	g_autofree gchar *profiles = g_strdup_printf("[{\"id\":4,\"name\":\"%s\"}]", fx->profile);

	add_series(series, 1, "Lantern Row", 2015, 81001, TRUE, fx->profile, "none", SEASONS_012);
	add_series(series, 2, "Harrow Fields", 2010, 81005, TRUE, fx->profile, "all", SEASON_1);
	add_series(series, 3, "Glass Harbor", 2018, 81006, FALSE, fx->profile, "all", SEASON_1);
	add_series(series, 4, "Kestrel Point", 2016, 81011, TRUE, fx->profile, NULL, SEASON_1);
	if (more)
		add_series(series, 5, "Juniper Hollow", 2012, 81012, TRUE, fx->profile, "all", SEASON_1);
	g_string_append_c(series, ']');
	axis_test_sonarr_set_series(fx->sonarr, series->str);
	axis_test_sonarr_set_profiles(fx->sonarr, profiles);
	axis_test_sonarr_set_folders(fx->sonarr, "[{\"path\":\"/tv\",\"accessible\":true}]");
	axis_test_sonarr_set_queue(fx->sonarr, NULL, 0);

	/* 1: a single; a double episode in one file; one episode of a monitored
	 * season that Sonarr does not monitor; one whose file is named for another
	 * episode; one of the season Sonarr does not monitor, whose file Axis's
	 * index does not hold; one the aired order does not have, with a file; one
	 * whose TheTVDB id is not the aired order's; a special. */
	add_episode(episodes, 101, 8100101, 1, 1, TRUE, 501);
	add_episode(episodes, 102, 8100102, 1, 2, TRUE, 502);
	add_episode(episodes, 103, 8100103, 1, 3, TRUE, 502);
	add_episode(episodes, 104, 8100104, 1, 4, FALSE, 0);
	add_episode(episodes, 105, 8100105, 1, 5, TRUE, 505);
	add_episode(episodes, 106, 8100106, 2, 1, FALSE, 504);
	add_episode(episodes, 107, 8100199, 1, 9, TRUE, 507);
	add_episode(episodes, 108, 99999999, 2, 2, TRUE, 0);
	add_episode(episodes, 109, 0, 0, 1, FALSE, 0);
	/* One whose TheTVDB id is not the aired order's, with a file (not in
	 * Axis's index): Axis has an episode at its place. */
	add_episode(episodes, 110, 99999998, 1, 6, TRUE, 510);
	/* One whose place can be read and whose monitoring is no yes or no. */
	g_string_append(episodes, ",{\"id\":112,\"tvdbId\":8100111,\"seasonNumber\":1,\"episodeNumber\":7,"
		"\"monitored\":\"yes\",\"hasFile\":true,\"episodeFileId\":512}");
	if (twice)
		/* The place of season 2's first episode a second time, on the same
		 * file: which of the two is meant cannot be told. */
		add_episode(episodes, 111, 0, 2, 1, FALSE, 504);
	g_string_append_c(episodes, ']');
	add_file(files, 501, "/tv/Lantern Row (2015)/Season 1/Lantern Row - S01E01 - Low Water.mkv", 9);
	add_file(files, 502, "/tv/Lantern Row (2015)/Season 1/Lantern Row - S01E02-E03 - The Ropes.mkv", 7);
	add_file(files, 505, "/tv/Lantern Row (2015)/Season 1/Lantern Row - S01E04 - Scene Numbered.mkv", 7);
	add_file(files, 504, "/tv/Lantern Row (2015)/Season 2/Lantern Row - S02E01 - Night Crossing.mkv", 11);
	add_file(files, 507, "/tv/Lantern Row (2015)/Season 1/Lantern Row - S01E09 - Beyond.mkv", 6);
	add_file(files, 510, "/tv/Lantern Row (2015)/Season 1/Lantern Row - S01E06 - Far Shore.mkv", 8);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 1, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 1, files->str);
	tvdb_order(fx, 81001, "0:1:8100100 1:1:8100101 1:2:8100102 1:3:8100103 1:4:8100104 1:5:8100105 1:6:8100110"
		" 1:7:8100111 2:1:8100106 2:2:8100107");

	/* 2: four episodes with a file each: linked in Axis to another series,
	 * unlinked by hand, of another size, and one that is linked. */
	g_string_assign(episodes, "[");
	g_string_assign(files, "[");
	add_episode(episodes, 201, 8100501, 1, 1, TRUE, 601);
	add_episode(episodes, 202, 8100502, 1, 2, TRUE, 602);
	add_episode(episodes, 203, 8100503, 1, 3, TRUE, 603);
	add_episode(episodes, 204, 8100504, 1, 4, TRUE, 604);
	g_string_append_c(episodes, ']');
	add_file(files, 601, "/tv/Harrow Fields (2010)/Season 1/Harrow Fields - S01E01.mkv", 9);
	add_file(files, 602, "/tv/Harrow Fields (2010)/Season 1/Harrow Fields - S01E02.mkv", 8);
	add_file(files, 603, "/tv/Harrow Fields (2010)/Season 1/Harrow Fields - S01E03.mkv", 22);
	add_file(files, 604, "/tv/Harrow Fields (2010)/Season 1/Harrow Fields - S01E04.mkv", 5);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 2, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 2, files->str);
	tvdb_order(fx, 81005, "1:1:8100501 1:2:8100502 1:3:8100503 1:4:8100504");

	g_string_assign(episodes, "[");
	add_episode(episodes, 301, 8100601, 1, 1, FALSE, 0);
	g_string_append_c(episodes, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 3, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 3, "[]");
	tvdb_order(fx, 81006, "1:1:8100601");

	g_string_assign(episodes, "[");
	add_episode(episodes, 401, 8101101, 1, 1, TRUE, 0);
	add_episode(episodes, 402, 8101102, 1, 2, TRUE, 0);
	g_string_append_c(episodes, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 4, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 4, "[]");
	tvdb_order(fx, 81011, "1:1:8101101 1:2:8101102");

	if (more) {
		g_string_assign(episodes, "[");
		add_episode(episodes, 501, 8101201, 1, 1, TRUE, 0);
		g_string_append_c(episodes, ']');
		axis_test_sonarr_set_episodes(fx->sonarr, 5, episodes->str);
		axis_test_sonarr_set_episode_files(fx->sonarr, 5, "[]");
		tvdb_order(fx, 81012, "1:1:8101201");
		write_file(fx, "Lantern Row (2015)/Season 2", "Lantern Row - S02E01 - Night Crossing.mkv", "nightcross1");
		scan(fx);
	}
}

static void
library(
	Fixture  *fx,
	gboolean  more
){
	library_full(fx, more, FALSE);
}

static gchar *
file_of(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *id = axis_test_db_queryf(fx->t->db, "SELECT id FROM library_file WHERE rel_path LIKE '%%%s'",
		name);

	g_assert_nonnull(id);
	return g_steal_pointer(&id);
}

static gint64
series_of(
	Fixture     *fx,
	const gchar *title
){
	g_autofree gchar *id = axis_test_db_queryf(fx->t->db, "SELECT s.id FROM series s JOIN meta_title t ON t.id ="
		" s.title_id WHERE t.title = '%s'", title);

	return id != NULL ? g_ascii_strtoll(id, NULL, 10) : 0;
}

/* The connection read and mapped, with Glass Harbor managed in Axis by hand
 * and holding one of Harrow Fields' files, and another of them unlinked by
 * hand: what a run then finds. */
static gint64
prepared(Fixture *fx)
{
	gint64 source = add_sonarr(fx);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *elsewhere = NULL;
	g_autofree gchar *unlinked = NULL;
	g_autofree gchar *sql = NULL;
	AxisTvSpec spec;
	gint64 other = 0;

	library(fx, FALSE);
	read_from_page(fx, source);
	map_tv(fx, source);
	title = axis_test_db_query(fx->t->db, "SELECT id FROM meta_title WHERE title = 'Glass Harbor'");
	g_assert_nonnull(title);
	memset(&spec, 0, sizeof(spec));
	spec.title_id = g_ascii_strtoll(title, NULL, 10);
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_NONE;
	spec.user = "root";
	g_assert_true(axis_tv_add(fx->tv, &spec, &other, &error));
	g_assert_no_error(error);
	elsewhere = file_of(fx, "Harrow Fields - S01E01.mkv");
	unlinked = file_of(fx, "Harrow Fields - S01E02.mkv");
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id, linked_by, hand_how, hand_by, hand_at, kept)"
		" VALUES (%s, %" G_GINT64_FORMAT ", 'root', 'linked', 'root', now(), true);"
		" INSERT INTO library_hand_unlink (file_id, module, owner_id, by_user) VALUES (%s, 'tv', %" G_GINT64_FORMAT
		", 'root')", elsewhere, other, unlinked, other);
	axis_test_db_exec(fx->t->db, sql);
	return source;
}

#define EP(title, s, e, column) \
	"(SELECT e." column "::text FROM series_episode e JOIN series x ON x.id = e.series_id JOIN meta_title t" \
	" ON t.id = x.title_id WHERE t.title = '" title "' AND e.season_number = " #s " AND e.number = " #e ")"

static void
assert_query(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(got, expected) != 0)
		g_error("expected '%s', got '%s', for: %s", expected, got != NULL ? got : "(null)", sql);
}

/* ── run/brings ─────────────────────────────────────────────────── */

static void
test_brings(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *disk_before = disk_fingerprint(fx);
	g_autofree gchar *disk_after = NULL;
	g_autofree gchar *index_before = axis_test_db_query(fx->t->db, "SELECT md5(string_agg(f.id::text || f.rel_path ||"
		" f.size::text || f.state || f.inode::text, '|' ORDER BY f.id)) FROM library_file f");
	g_autofree gchar *expected = NULL;
	AxisTestReply page;

	run_ok(fx, source);

	/* Three series: with the mapped profile and library folder, TheTVDB's
	 * aired order from the start, monitored as in Sonarr, and held. */
	expected = g_strdup_printf("Harrow Fields|%s|%" G_GINT64_FORMAT "|tvdb|brought|brought|true;"
		"Kestrel Point|%s|%" G_GINT64_FORMAT "|tvdb|brought|brought|true;"
		"Lantern Row|%s|%" G_GINT64_FORMAT "|tvdb|brought|brought|false", fx->profile, fx->root_id, fx->profile,
		fx->root_id, fx->profile, fx->root_id);
	assert_query(fx, "SELECT string_agg(t.title || '|' || q.name || '|' || s.root_id || '|' || s.episode_source || '|'"
		" || s.monitor_choice || '|' || s.search_hold || '|' || s.monitor_new, ';' ORDER BY t.title) FROM series s"
		" JOIN meta_title t ON t.id = s.title_id JOIN quality_profile q ON q.id = s.quality_profile_id"
		" WHERE s.search_hold IS NOT NULL", expected);
	/* Never under another numbering first: every episode row is TheTVDB's. */
	assert_query(fx, "SELECT count(*) FROM series_episode e JOIN series s ON s.id = e.series_id"
		" WHERE s.search_hold IS NOT NULL AND e.source <> 'tvdb'", "0");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'numbering'", "0");
	assert_query(fx, EP("Lantern Row", 1, 1, "provider_id"), "8100101");

	/* Each season as Sonarr monitors it; each episode as Sonarr monitors the
	 * episode at its place, one of a monitored season unmonitored; where the
	 * TheTVDB ids disagree (1x6, 2x2) the place decides all the same, 2x2
	 * monitored in a season that is not; the place whose entry could not be
	 * read (1x7) unmonitored. */
	assert_query(fx, "SELECT string_agg(ss.number || ':' || ss.monitored, ' ' ORDER BY ss.number) FROM series_season ss"
		" JOIN series s ON s.id = ss.series_id JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row'",
		"0:false 1:true 2:false");
	assert_query(fx, "SELECT string_agg(e.season_number || 'x' || e.number || ':' || e.monitored, ' '"
		" ORDER BY e.season_number, e.number) FROM series_episode e JOIN series s ON s.id = e.series_id"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row'",
		"0x1:false 1x1:true 1x2:true 1x3:true 1x4:false 1x5:true 1x6:true 1x7:false 2x1:false 2x2:true");

	/* The files: a single, a double episode on both its episodes, the one
	 * named for another episode on the episode Sonarr has it on; stamped as
	 * migrated and kept for that (Axis does not replace them), with no
	 * import and no identity, read under the series' numbering. */
	assert_query(fx, "SELECT string_agg(e.season_number || 'x' || e.number || '=' || regexp_replace(lf.rel_path,"
		" '^.* - (S[0-9E-]+) .*$', '\\1'), ' ' ORDER BY e.season_number, e.number) FROM series_file_episode sfe"
		" JOIN series_episode e ON e.id = sfe.episode_id JOIN library_file lf ON lf.id = sfe.file_id"
		" JOIN series s ON s.id = e.series_id JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row'",
		"1x1=S01E01 1x2=S01E02-E03 1x3=S01E02-E03 1x5=S01E04");
	assert_query(fx, "SELECT count(*) || ' ' || count(*) FILTER (WHERE sf.hand_how = 'brought' AND sf.hand_by = 'root'"
		" AND sf.hand_at IS NOT NULL AND sf.kept AND sf.kept_how = 'brought' AND sf.import_id IS NULL"
		" AND sf.identity_id IS NULL"
		" AND sf.read_source = 'tvdb' AND sf.quality IS NOT NULL) FROM series_file sf JOIN series s"
		" ON s.id = sf.series_id WHERE s.search_hold IS NOT NULL", "4 4");

	/* What Sonarr has that the aired order does not (1x9), and the episodes
	 * whose TheTVDB ids disagree (1x6, 2x2): counted, made into nothing, and
	 * the files of the first two not linked.  The place that could not be
	 * read is no episode of Sonarr's, and is not counted as one. */
	assert_query(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id IN (" EP("Lantern Row", 1, 6, "id")
		"::bigint, " EP("Lantern Row", 1, 7, "id") "::bigint)", "0");
	assert_query(fx, EP("Lantern Row", 1, 9, "id"), NULL);
	assert_query(fx, "SELECT count(*) FROM series_file WHERE file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%S01E09 - Beyond.mkv')", "0");
	assert_query(fx, "SELECT b.file || ' ' || (b.counts ->> 'files') || ' ' || (b.counts ->> 'linked') || ' ' ||"
		" (b.counts ->> 'not_indexed') || ' ' || (b.counts ->> 'unmatched_files') || ' ' ||"
		" (b.counts ->> 'unmatched_episodes') || ' ' || (b.counts ->> 'held_episodes') FROM arr_brought b"
		" WHERE b.label = 'Lantern Row (2015)'", "partly 6 3 2 2 3 3");

	/* Each reason a file is left alone: linked to another series (and left
	 * there), unlinked by hand (and left so), another size; one linked. */
	assert_query(fx, "SELECT b.file || ' ' || (b.counts ->> 'files') || ' ' || (b.counts ->> 'linked') || ' ' ||"
		" (b.counts ->> 'elsewhere') || ' ' || (b.counts ->> 'unlinked') || ' ' || (b.counts ->> 'other_size')"
		" || ' ' || (b.counts ->> 'held_episodes') FROM arr_brought b WHERE b.label = 'Harrow Fields (2010)'",
		"partly 4 1 1 1 1 3");
	assert_query(fx, "SELECT t.title FROM series_file sf JOIN series s ON s.id = sf.series_id JOIN meta_title t"
		" ON t.id = s.title_id WHERE sf.file_id = (SELECT id FROM library_file WHERE rel_path LIKE"
		" '%Harrow Fields - S01E01.mkv')", "Glass Harbor");
	assert_query(fx, "SELECT count(*) FROM series_file WHERE file_id IN (SELECT id FROM library_file WHERE rel_path"
		" LIKE '%Harrow Fields - S01E02.mkv' OR rel_path LIKE '%Harrow Fields - S01E03.mkv')", "0");
	assert_query(fx, "SELECT count(*) FROM library_hand_unlink", "1");
	/* An episode Sonarr has a file for that has none in Axis is held apart:
	 * also the one at a place Sonarr lists under another id (1x6), and the
	 * one at the place that could not be read (1x7). */
	assert_query(fx, "SELECT string_agg(t.title || ' ' || e.season_number || 'x' || e.number, ', ' ORDER BY t.title,"
		" e.season_number, e.number) FROM series_episode e JOIN series s ON s.id = e.series_id JOIN meta_title t"
		" ON t.id = s.title_id WHERE e.search_hold = 'file_unseen'",
		"Harrow Fields 1x1, Harrow Fields 1x2, Harrow Fields 1x3, Lantern Row 1x6, Lantern Row 1x7, Lantern Row 2x1");
	assert_query(fx, "SELECT b.file FROM arr_brought b WHERE b.label = 'Kestrel Point (2016)'", "none");

	/* Nothing on any disk changed, nor Axis's index; nothing is searched for;
	 * no movie was made; Sonarr was only read. */
	disk_after = disk_fingerprint(fx);
	g_assert_cmpstr(disk_after, ==, disk_before);
	assert_query(fx, "SELECT md5(string_agg(f.id::text || f.rel_path || f.size::text || f.state || f.inode::text, '|'"
		" ORDER BY f.id)) FROM library_file f", index_before);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind LIKE 'tv.%'", "0");
	assert_query(fx, "SELECT (SELECT count(*) FROM movie) + (SELECT count(*) FROM movie_file) +"
		" (SELECT count(*) FROM series_grab) + (SELECT count(*) FROM series_search)", "0");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'added' AND text LIKE"
		" 'Migrated from Sonarr (Sonarr) by root%numbered as TheTVDB%'", "3");
	axis_test_sonarr_assert_read_only(fx->sonarr);

	/* The list of what was brought names each reason with its count. */
	page = get_as(fx, fx->admin, "/migrate/1/brought");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "3 of 6 files linked. 2 not in Axis's index at their paths. 2 on episodes Axis's numbering "
		"does not have: not linked. 3 episodes Sonarr has that Axis's numbering does not");
	assert_contains(page.body, "1 of 4 files linked. 1 linked in Axis to another series: left there. 1 unlinked by hand "
		"in Axis: left so. 1 of another size than Sonarr recorded.");
	assert_contains(page.body, "Sonarr has no file for it.");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "3 series migrated from Sonarr, 4 episode files linked.");
	/* The held, as the notice of "Needs attention" counts them. */
	assert_contains(page.body, "1 series has no monitored episode without a file. Episode files that were migrated are "
		"kept, and Axis does not replace them: this series needs nothing from you now.");
	assert_contains(page.body, "2 series have monitored episodes without a file. Axis does not search for them until "
		"you start searching.");
	g_assert_null(strstr(page.body, "series are held:"));
	assert_contains(page.body, "Start searching for the episodes without a file");
	assert_contains(page.body, "6 episodes are held apart");
	axis_test_reply_clear(&page);
}

/* ── run/name ───────────────────────────────────────────────────── */

/* Identification holds the folder of @title for a person (its files are not
 * probed here, so it is not sure by itself): a person confirms it, which
 * links the folder and places its files by their names. */
static void
confirm_unit(
	Fixture     *fx,
	const gchar *title
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *unit = axis_test_db_queryf(fx->t->db, "SELECT i.id FROM library_identity i WHERE i::text LIKE"
		" '%%%s (%%' ORDER BY i.id LIMIT 1", title);

	g_assert_nonnull(unit);
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(unit, NULL, 10), AXIS_IDENTIFY_CONFIRM, 0, "root",
		&error));
	g_assert_no_error(error);
}

static gchar *
brought_links(Fixture *fx)
{
	return axis_test_db_query(fx->t->db, "SELECT md5(coalesce(string_agg(sf::text || '>' || (SELECT string_agg("
		"x.episode_id::text, ',' ORDER BY x.episode_id) FROM series_file_episode x WHERE x.file_id = sf.file_id), '|'"
		" ORDER BY sf.file_id), '-')) FROM series_file sf WHERE sf.hand_how = 'brought'");
}

static void
test_name(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) misplaced = NULL;
	g_autoptr(AxisTvLinks) links = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisIdentifyReport report;
	gboolean said = FALSE;
	gint64 series;
	guint i;

	run_ok(fx, source);
	series = series_of(fx, "Lantern Row");
	before = brought_links(fx);

	/* The file named S01E04 is on episode 5, where Sonarr had it: it is not
	 * listed among the files whose names say another episode. */
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &misplaced, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(misplaced->len, ==, 0);

	/* Identification reads the folders, and their names, afterwards: every
	 * brought link is as it was, and the misnamed file is still on episode
	 * 5, with episode 4 still without a file. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
	g_assert_no_error(error);
	confirm_unit(fx, "Lantern Row");
	assert_query(fx, "SELECT count(*) FROM library_identity WHERE state = 'linked' AND title_id = (SELECT title_id"
		" FROM series s JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row')", "1");
	after = brought_links(fx);
	g_assert_cmpstr(after, ==, before);
	assert_query(fx, "SELECT e.number FROM series_file_episode sfe JOIN series_episode e ON e.id = sfe.episode_id"
		" WHERE sfe.file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%S01E04 - Scene Numbered.mkv')", "5");
	assert_query(fx, "SELECT count(*) FROM series_file_episode sfe WHERE sfe.episode_id = " EP("Lantern Row", 1, 4,
		"id") "::bigint", "0");
	g_clear_pointer(&misplaced, g_ptr_array_unref);
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &misplaced, NULL, NULL, &error));
	g_assert_cmpuint(misplaced->len, ==, 0);

	/* The links page says how the link was made, and what the name states. */
	g_assert_true(axis_tv_links_read(fx->tv, series, NULL, 0, &links, &error));
	g_assert_no_error(error);
	for (i = 0; i < links->files->len; i++) {
		const AxisTvLinkFile *file = g_ptr_array_index(links->files, i);

		if (strstr(file->path, "Scene Numbered") == NULL)
			continue;
		said = TRUE;
		g_assert_true(file->linked);
		g_assert_cmpstr(file->on, ==, "S01E05");
		g_assert_nonnull(strstr(file->how, "Migrated from Sonarr by root"));
		g_assert_nonnull(strstr(file->how, "Its name states S01E04, another episode than the one Sonarr had it on"));
	}
	g_assert_true(said);
}

/* ── run/identified ─────────────────────────────────────────────── */

static gchar *
upgrade_searches(
	Fixture *fx,
	gint64   lantern
);

static void
test_identified(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;
	AxisTestReply page;

	library(fx, FALSE);
	/* Identification has been through the library before the run: it holds
	 * Lantern Row's folder, and its files by their names. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
	g_assert_no_error(error);
	confirm_unit(fx, "Lantern Row");
	read_from_page(fx, source);
	map_tv(fx, source);
	run_ok(fx, source);

	/* Adding the series linked what identification holds, as a person's Add
	 * does.  The two links on the very episodes Sonarr names are Sonarr's
	 * files all the same: the run stamps them as migrated, and so kept
	 * (left as identification's they would be upgraded by Axis itself after
	 * "Start searching").  The file named for episode 4, which identification
	 * had put on episode 4 by its name, is moved to episode 5, where Sonarr
	 * has it, and stamped so too: no file sits on an episode Sonarr says it
	 * is not, and episode 5 is not held as if it had none. */
	assert_query(fx, "SELECT count(*) FILTER (WHERE sf.identity_id IS NOT NULL AND sf.hand_at IS NULL) || ' ' ||"
		" count(*) FILTER (WHERE sf.hand_how = 'brought' AND sf.hand_by = 'root' AND sf.hand_at IS NOT NULL"
		" AND sf.kept AND sf.kept_how = 'brought' AND sf.import_id IS NULL AND sf.identity_id IS NULL"
		" AND sf.read_source = 'tvdb')"
		" FROM series_file sf JOIN series s ON s.id = sf.series_id"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row'", "0 3");
	assert_query(fx, "SELECT (b.counts ->> 'linked') || ' ' || (b.counts ->> 'already') || ' ' ||"
		" (b.counts ->> 'differs') || ' ' || (b.counts ->> 'moved') FROM arr_brought b"
		" WHERE b.label = 'Lantern Row (2015)'", "2 0 0 1");
	assert_query(fx, "SELECT string_agg(e.number::text, ',' ORDER BY e.number) FROM series_file_episode sfe"
		" JOIN series_episode e ON e.id = sfe.episode_id WHERE sfe.file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%S01E04 - Scene Numbered.mkv')", "5");
	assert_query(fx, EP("Lantern Row", 1, 5, "search_hold"), NULL);
	/* The episode it left is as any other: as Sonarr monitors it, and not
	 * held (Sonarr has no file for it). */
	assert_query(fx, EP("Lantern Row", 1, 4, "monitored::text || ' ' || coalesce(e.search_hold, 'free')"), "false free");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE text LIKE '%moved to the episode Sonarr has it on%'", "1");

	/* Identification reads the folder again: it does not move the file back. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
	g_assert_no_error(error);
	assert_query(fx, "SELECT string_agg(e.number::text, ',' ORDER BY e.number) FROM series_file_episode sfe"
		" JOIN series_episode e ON e.id = sfe.episode_id WHERE sfe.file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%S01E04 - Scene Numbered.mkv')", "5");

	/* The list of what was brought says so. */
	page = get_as(fx, fx->admin, "/migrate/1/brought");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "1 linked by its name to another episode than Sonarr has it on: moved to the episode Sonarr "
		"has it on.");
	axis_test_reply_clear(&page);

	/* The folder identification held first is kept as any brought file: below
	 * the cutoff, after "Start searching", three runs of the scheduler queue
	 * nothing that would replace a file of it. */
	{
		gint64 lantern = series_of(fx, "Lantern Row");
		g_autofree gchar *n = NULL;
		AxisTvScheduleReport ran;
		guint i;

		axis_test_db_exec(fx->t->db, "UPDATE series_file SET quality = 'hdtv_720p'");
		page = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
		g_assert_cmpuint(page.status, ==, 303);
		axis_test_reply_clear(&page);
		assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold IS NOT NULL", "0");
		for (i = 0; i < 3; i++) {
			memset(&ran, 0, sizeof(ran));
			g_assert_true(axis_tv_run_schedule(fx->tv, &ran, &error));
			g_assert_no_error(error);
		}
		n = upgrade_searches(fx, lantern);
		g_assert_cmpstr(n, ==, "0");
	}
}

/* ── run/second ─────────────────────────────────────────────────── */

static void
test_second(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *events = NULL;
	g_autofree gchar *sql = NULL;
	gint64 lantern;
	gint64 kestrel;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	kestrel = series_of(fx, "Kestrel Point");

	/* A person's changes in Axis since: a season monitored that Sonarr does
	 * not monitor, a brought file moved by hand to episode 4, Harrow Fields
	 * let search by itself with the box ticked (also the episodes whose files
	 * Axis does not see), and Kestrel Point removed. */
	g_assert_true(axis_tv_set_season_monitored(fx->tv, lantern, 2, TRUE, "root", &error));
	g_assert_no_error(error);
	sql = g_strdup_printf("UPDATE series_file_episode SET episode_id = " EP("Lantern Row", 1, 4, "id") "::bigint"
		" WHERE file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%%Scene Numbered.mkv')");
	axis_test_db_exec(fx->t->db, sql);
	g_assert_true(axis_tv_start_searching(fx->tv, series_of(fx, "Harrow Fields"), TRUE, "root", NULL, &error));
	g_assert_true(axis_tv_remove(fx->tv, kestrel, "root", &error));
	g_assert_no_error(error);
	events = axis_test_db_query(fx->t->db, "SELECT count(*) FROM series_event");

	/* Sonarr has a new series, and Lantern Row's file of season 2 is on the
	 * disk and in the index now.  Read again, and run again. */
	library(fx, TRUE);
	read_from_page(fx, source);
	{
		g_autoptr(AxisArrPreview) preview = NULL;

		g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NONE], ==, 1);
		g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_REMOVED], ==, 1);
		g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_ALREADY], ==, 3);
		/* One file to link later: season 2's.  Not the file on an episode
		 * Axis's numbering lacks, nor the ones left alone for a reason. */
		g_assert_cmpuint(preview->n_relink, ==, 1);
	}
	run_ok(fx, source);

	/* Only what is new was brought; the series removed is not back. */
	assert_query(fx, "SELECT string_agg(t.title, ', ' ORDER BY t.title) FROM series s JOIN meta_title t"
		" ON t.id = s.title_id", "Glass Harbor, Harrow Fields, Juniper Hollow, Lantern Row");
	assert_query(fx, "SELECT count(*) || ' ' || count(series_id) FROM arr_brought", "4 3");
	/* The file that became visible is linked, and its episode's hold is gone. */
	assert_query(fx, "SELECT sf.hand_how || ' ' || e.season_number || 'x' || e.number || ' ' ||"
		" coalesce(e.search_hold, 'free') FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id = sf.file_id"
		" JOIN series_episode e ON e.id = sfe.episode_id WHERE sf.file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%Night Crossing.mkv')", "brought 2x1 free");
	/* (Still held: the two whose files Axis does not see.) */
	assert_query(fx, "SELECT (b.counts ->> 'linked') || ' ' || (b.counts ->> 'held_episodes') FROM arr_brought b"
		" WHERE b.label = 'Lantern Row (2015)'", "4 2");
	/* What the person changed stands: season 2 is still monitored (Sonarr
	 * says not), the file moved by hand is still on episode 4, Harrow Fields
	 * is still free, and its episodes whose holds were ended are not held
	 * again. */
	assert_query(fx, "SELECT ss.monitored FROM series_season ss WHERE ss.series_id = (SELECT s.id FROM series s"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = 'Lantern Row') AND ss.number = 2", "t");
	assert_query(fx, "SELECT e.number FROM series_file_episode sfe JOIN series_episode e ON e.id = sfe.episode_id"
		" WHERE sfe.file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%Scene Numbered.mkv')", "4");
	assert_query(fx, "SELECT coalesce(s.search_hold, 'free') || ' ' || (SELECT count(*) FROM series_episode e"
		" WHERE e.series_id = s.id AND e.search_hold IS NOT NULL) FROM series s JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title = 'Harrow Fields'", "free 0");
	/* Nothing of the three series already there was written but that link:
	 * one history line for it, one for the new series. */
	{
		g_autofree gchar *more = g_strdup_printf("SELECT count(*) - %s FROM series_event", events);

		assert_query(fx, more, "2");
	}
	assert_query(fx, "SELECT count(*) FROM series_file WHERE hand_how = 'brought'", "5");

	/* A third run has nothing to do. */
	{
		g_autofree gchar *digest = digest_now(fx, source);
		AxisTestReply posted = confirm_run(fx, fx->admin, source, digest);

		g_assert_cmpuint(posted.status, ==, 400);
		assert_contains(posted.body, "There is nothing to migrate");
		axis_test_reply_clear(&posted);
	}
}

/* ── run/twice, run/renumbered, run/another-way ─────────────────── */

static guint
relink_now(
	Fixture *fx,
	gint64   source
){
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_no_error(error);
	return preview->n_relink;
}

/* A place Sonarr lists twice matches nothing, in the preview as in the run:
 * a file on it that became visible is not offered, and not linked. */
static void
test_twice(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);

	run_ok(fx, source);
	library_full(fx, TRUE, TRUE);
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 0);
	run_ok(fx, source);
	assert_query(fx, "SELECT count(*) FROM series_file WHERE file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%Night Crossing.mkv')", "0");
	assert_query(fx, EP("Lantern Row", 2, 1, "search_hold"), "file_unseen");
}

/* A file identification linked by its name after the first run, on another
 * episode than Sonarr has it on: a later run offers it, moves the link to
 * Sonarr's episode, stamps it brought and says so, although it links no file
 * that was unlinked.  One on an episode Axis's numbering lacks, linked by its
 * name all the same, cannot be moved: it is named as on another episode. */
static void
test_named_later(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *beyond = NULL;
	AxisTestReply page;
	gint64 lantern;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	library(fx, TRUE);
	/* As identification leaves a link it made by a name: no person's stamp,
	 * no import.  Season 2's file on 2x2 (Sonarr: 2x1); and the file Sonarr
	 * has on 1x9, which Axis's numbering lacks, on 1x4. */
	file = file_of(fx, "Night Crossing.mkv");
	beyond = file_of(fx, "S01E09 - Beyond.mkv");
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id) VALUES (%s, %" G_GINT64_FORMAT "), (%s, %"
		G_GINT64_FORMAT "); INSERT INTO series_file_episode (file_id, episode_id) VALUES (%s, " EP("Lantern Row", 2, 2,
		"id") "::bigint), (%s, " EP("Lantern Row", 1, 4, "id") "::bigint)", file, lantern, beyond, lantern, file, beyond);
	axis_test_db_exec(fx->t->db, sql);
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 1);
	run_ok(fx, source);
	assert_query(fx, "SELECT sf.hand_how || ' ' || (sf.hand_at IS NOT NULL) || ' ' || string_agg(e.season_number || 'x'"
		" || e.number, ',') FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id = sf.file_id"
		" JOIN series_episode e ON e.id = sfe.episode_id WHERE sf.file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%Night Crossing.mkv') GROUP BY sf.hand_how, sf.hand_at", "brought true 2x1");
	assert_query(fx, EP("Lantern Row", 2, 1, "search_hold"), NULL);
	assert_query(fx, "SELECT (b.counts ->> 'moved') || ' ' || (b.counts ->> 'linked') FROM arr_brought b"
		" WHERE b.label = 'Lantern Row (2015)'", "1 3");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'hand_link' AND text LIKE"
		" '%moved to the episode Sonarr has it on%'", "1");
	/* The other stays on 1x4, where its name put it. */
	assert_query(fx, "SELECT e.number FROM series_file_episode sfe JOIN series_episode e ON e.id = sfe.episode_id"
		" WHERE sfe.file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%S01E09 - Beyond.mkv')", "4");
	/* A third run has nothing to move. */
	g_assert_cmpuint(relink_now(fx, source), ==, 0);
	page = get_as(fx, fx->admin, "/migrate/1/brought");
	assert_contains(page.body, "1 linked by its name to another episode than Sonarr has it on: moved to the episode Sonarr "
		"has it on.");
	axis_test_reply_clear(&page);
}

/* A series a person switched to another numbering after it was brought: a
 * later run links nothing of it (Sonarr's places are TheTVDB's, and the
 * series' rows are not any more), and the preview says so. */
static void
test_renumbered(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *sql = NULL;
	AxisTestReply page;
	gint64 lantern;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	/* As the change of numbering leaves it: the series follows TMDb's, and
	 * its episode rows are that numbering's (here at the same places). */
	sql = g_strdup_printf("UPDATE series_episode SET source = 'tmdb' WHERE series_id = %" G_GINT64_FORMAT ";"
		" UPDATE series SET episode_source = 'tmdb' WHERE id = %" G_GINT64_FORMAT, lantern, lantern);
	axis_test_db_exec(fx->t->db, sql);
	library(fx, TRUE);
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 0);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	/* (Two: season 2's, and the one on an episode the aired order lacks, which
	 * is not matched against a numbering that is not TheTVDB's.) */
	assert_contains(page.body, "Not linked: 2 episode files of series migrated before that no longer follow "
		"TheTVDB's numbering in Axis.");
	axis_test_reply_clear(&page);
	run_ok(fx, source);
	assert_query(fx, "SELECT count(*) FROM series_file WHERE file_id = (SELECT id FROM library_file"
		" WHERE rel_path LIKE '%Night Crossing.mkv')", "0");
	assert_query(fx, "SELECT count(*) FROM series_file WHERE hand_how = 'brought'", "4");
}

/* An episode held because Axis does not see Sonarr's file gets a file another
 * way (here a person links one): it is not held any more, at once for every
 * reader, and its mark is gone after the scheduler's next run. */
static void
test_another_way(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *sql = NULL;
	AxisTvScheduleReport report;
	AxisTestReply page;
	gint64 harrow;
	guint held = 0;
	guint i;

	run_ok(fx, source);
	harrow = series_of(fx, "Harrow Fields");
	assert_query(fx, EP("Harrow Fields", 1, 3, "search_hold"), "file_unseen");
	file = file_of(fx, "Harrow Fields - S01E03.mkv");
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id, linked_by, hand_how, hand_by, hand_at, kept)"
		" VALUES (%s, %" G_GINT64_FORMAT ", 'root', 'linked', 'root', now(), false);"
		" INSERT INTO series_file_episode (file_id, episode_id) VALUES (%s, " EP("Harrow Fields", 1, 3, "id")
		"::bigint)", file, harrow, file);
	axis_test_db_exec(fx->t->db, sql);

	g_assert_true(axis_tv_get(fx->tv, harrow, &series, &episodes, &error));
	g_assert_no_error(error);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (e->season == 1 && e->number == 3) {
			g_assert_cmpuint(e->n_files, ==, 1);
			g_assert_null(e->search_hold);
		} else if (e->search_hold != NULL) {
			held++;
		}
	}
	g_assert_cmpuint(held, ==, 2);
	/* The connection's page counts it no more: five of the six. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "5 episodes are held apart");
	axis_test_reply_clear(&page);

	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
	g_assert_no_error(error);
	assert_query(fx, EP("Harrow Fields", 1, 3, "search_hold"), NULL);
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'", "5");
}

/*
 * A file Sonarr has that Axis did not see when the run was made, which
 * identification links by its name afterwards (not kept, no person's stamp):
 * its episode stays held apart, for every reader and after the scheduler's
 * run, so that Axis does not upgrade Sonarr's file by itself; the
 * connection's page says that the file is not kept yet and to run again.  A
 * later run keeps the file, and the hold goes with it.
 */
static void
test_unkept_stays_held(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *sql = NULL;
	AxisTvScheduleReport report;
	AxisTestReply page;
	gint64 lantern;
	guint i;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	library(fx, TRUE);
	file = file_of(fx, "Night Crossing.mkv");
	/* As identification leaves a link it made by a name, on the episode Sonarr
	 * has the file on (Lantern Row 2x1, held apart because Axis did not see
	 * the file). */
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id) VALUES (%s, %" G_GINT64_FORMAT ");"
		" INSERT INTO series_file_episode (file_id, episode_id) VALUES (%s, " EP("Lantern Row", 2, 1, "id")
		"::bigint)", file, lantern, file);
	axis_test_db_exec(fx->t->db, sql);

	g_assert_true(axis_tv_get(fx->tv, lantern, &series, &episodes, &error));
	g_assert_no_error(error);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (e->season == 2 && e->number == 1) {
			g_assert_cmpuint(e->n_files, ==, 1);
			g_assert_cmpstr(e->search_hold, ==, "file_unseen");
		}
	}
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
	g_assert_no_error(error);
	assert_query(fx, EP("Lantern Row", 2, 1, "search_hold"), "file_unseen");
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'", "6");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "1 migrated file is not kept yet. Run the migration again to keep it.");
	assert_contains(page.body, "/migrate/1/preview");
	axis_test_reply_clear(&page);

	/* A later run keeps it, and the hold goes. */
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 1);
	run_ok(fx, source);
	assert_query(fx, "SELECT hand_how || ':' || kept || ':' || kept_how FROM series_file WHERE file_id = " "(SELECT id FROM"
		" library_file WHERE rel_path LIKE '%Night Crossing.mkv')", "brought:true:brought");
	assert_query(fx, EP("Lantern Row", 2, 1, "search_hold"), NULL);
	page = get_as(fx, fx->admin, "/migrate/1");
	g_assert_null(strstr(page.body, "not kept yet"));
	axis_test_reply_clear(&page);
}

/*
 * The migration of a database made before "kept" was migrated's: it
 * stamps by title the files identification linked first, but not those of a
 * series whose counts say some of Sonarr's files are on other episodes than
 * Axis has them ("differs"): which files those are cannot be known, and a
 * later run stamps the rest file by file.
 */
static void
test_migration_differs(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *again = NULL;

	run_ok(fx, source);
	assert_query(fx, "SELECT bool_and(n > 0) FROM (SELECT count(*) FILTER (WHERE sf.hand_how = 'brought') AS n"
		" FROM series_file sf JOIN series s ON s.id = sf.series_id JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title IN ('Lantern Row', 'Harrow Fields') GROUP BY t.title) x", "t");
	/* The database as it was before: identification's links, and what the run
	 * counted. */
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET hand_how = NULL, hand_by = NULL, hand_at = NULL, kept = false,"
		" kept_how = NULL WHERE hand_how = 'brought'");
	axis_test_db_exec(fx->t->db, "UPDATE arr_brought SET counts = counts || '{\"already\": 2}'::jsonb"
		" WHERE label LIKE 'Lantern Row%'");
	axis_test_db_exec(fx->t->db, "UPDATE arr_brought SET counts = counts || '{\"already\": 2, \"differs\": 1}'::jsonb"
		" WHERE label LIKE 'Harrow Fields%'");
	axis_test_db_exec(fx->t->db, "ALTER TABLE series_file DROP COLUMN kept_how");
	axis_test_db_exec(fx->t->db, "ALTER TABLE movie_file DROP COLUMN kept_how");
	axis_test_db_exec(fx->t->db, "ALTER TABLE arr_source DROP COLUMN run_note");
	axis_test_db_exec(fx->t->db, "ALTER TABLE arr_brought DROP COLUMN run_job");
	/* Its statements again, from its file: the migrator refuses a history
	 * with a gap, and migrations have come after this one. */
	g_assert_true(g_file_get_contents("data/migrations/0073_arr_brought_kept.sql", &again, NULL, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, again);
	assert_query(fx, "SELECT count(*) FILTER (WHERE t.title = 'Lantern Row' AND sf.hand_how = 'brought' AND sf.kept)"
		" > 0 AND count(*) FILTER (WHERE t.title = 'Harrow Fields' AND sf.hand_how IS NOT NULL) = 0"
		" FROM series_file sf JOIN series s ON s.id = sf.series_id JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title IN ('Lantern Row', 'Harrow Fields')", "t");
}

static gchar *
run_ticked(
	Fixture *fx,
	gint64   source
);

/*
 * A run made after the first (its relink moves a link) with the tick: it
 * starts the searches for the title it linked a file of only; what the first
 * run brought stays held, and the note says how many are still held.  Another
 * run, after a person pressed Start searching, has none of its own titles
 * held, and says so instead of saying nothing.
 */
static void
test_ticked_later(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply page;
	gint64 lantern;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	library(fx, TRUE);
	file = file_of(fx, "Night Crossing.mkv");
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id) VALUES (%s, %" G_GINT64_FORMAT ");"
		" INSERT INTO series_file_episode (file_id, episode_id) VALUES (%s, " EP("Lantern Row", 2, 2, "id") "::bigint)",
		file, lantern, file);
	axis_test_db_exec(fx->t->db, sql);
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 1);
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	assert_query(fx, "SELECT string_agg(t.title || '=' || coalesce(s.search_hold, 'free'), ' ' ORDER BY t.title)"
		" FROM series s JOIN meta_title t ON t.id = s.title_id WHERE s.monitor_choice = 'brought'",
		"Harrow Fields=brought Juniper Hollow=free Kestrel Point=brought Lantern Row=free");
	page = get_as(fx, fx->admin, "/migrate/1");
	/* (Juniper Hollow came with the library of this run, Lantern Row had a file
	 * linked by it: the two of its own.) */
	assert_contains(page.body, "Searching was started at the end of the last run, as ticked on its confirmation: for 2 "
		"series of that run.");
	assert_contains(page.body, "2 series migrated by an earlier run are still held; press Start searching for them.");
	axis_test_reply_clear(&page);
}

static void
test_ticked_nothing_held(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply page;
	gint64 lantern;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	/* Everything brought is started by a person. */
	page = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(page.status, ==, 303);
	axis_test_reply_clear(&page);
	library(fx, TRUE);
	/* Sonarr lists no new series now: the run only links a file. */
	{
		g_autoptr(GString) list = g_string_new("[");

		add_series(list, 1, "Lantern Row", 2015, 81001, TRUE, fx->profile, "none", SEASONS_012);
		add_series(list, 2, "Harrow Fields", 2010, 81005, TRUE, fx->profile, "all", SEASON_1);
		add_series(list, 3, "Glass Harbor", 2018, 81006, FALSE, fx->profile, "all", SEASON_1);
		add_series(list, 4, "Kestrel Point", 2016, 81011, TRUE, fx->profile, NULL, SEASON_1);
		g_string_append_c(list, ']');
		axis_test_sonarr_set_series(fx->sonarr, list->str);
	}
	file = file_of(fx, "Night Crossing.mkv");
	sql = g_strdup_printf("INSERT INTO series_file (file_id, series_id) VALUES (%s, %" G_GINT64_FORMAT ");"
		" INSERT INTO series_file_episode (file_id, episode_id) VALUES (%s, " EP("Lantern Row", 2, 2, "id") "::bigint)",
		file, lantern, file);
	axis_test_db_exec(fx->t->db, sql);
	read_from_page(fx, source);
	g_assert_cmpuint(relink_now(fx, source), ==, 1);
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "Nothing of this run was waiting to be searched for.");
	axis_test_reply_clear(&page);
}

/* ── run/kept ───────────────────────────────────────────────────── */

/* How many searches are queued that would replace a file of Lantern Row's
 * first season: the season's own search, or one of an episode with a file. */
static gchar *
upgrade_searches(
	Fixture *fx,
	gint64   lantern
){
	return axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM job j WHERE j.kind = 'tv.search' AND (j.dedupe_key ="
		" 'tv.search:%" G_GINT64_FORMAT ":season:1' OR j.dedupe_key IN (SELECT 'tv.search:' || e.series_id || ':episode:'"
		" || e.id FROM series_episode e WHERE e.series_id = %" G_GINT64_FORMAT " AND EXISTS (SELECT 1 FROM"
		" series_file_episode sfe WHERE sfe.episode_id = e.id)))", lantern, lantern);
}

/*
 * Decided on 2026-11-21 (the proposal's question 2): an episode file migrated
 * is kept.  Below the cutoff, its episode is not wanted for an upgrade and the
 * scheduler searches for nothing that would replace it after "Start
 * searching" (a single file, and the double episode's on both its episodes);
 * "Let Axis upgrade these files" lets the connection's go, and then it is
 * wanted and searched for.  A link a person made by hand is not touched.
 */
static void
test_kept(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisArrRelease) release = NULL;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *form = NULL;
	AxisTvScheduleReport report;
	AxisTvRules rules;
	AxisTestReply page;
	gint64 lantern;
	guint with_file = 0;
	guint i;

	run_ok(fx, source);
	lantern = series_of(fx, "Lantern Row");
	assert_query(fx, "SELECT count(*) FILTER (WHERE kept AND kept_how = 'brought') || ' of ' || count(*) FROM series_file"
		" WHERE hand_how = 'brought'", "4 of 4");
	/* Every brought file is below its profile's cutoff. */
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET quality = 'hdtv_720p' WHERE hand_how = 'brought'");
	page = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(page.status, ==, 303);
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold IS NOT NULL", "0");

	/* Not wanted for an upgrade, and the words name Sonarr and no person. */
	g_assert_true(axis_tv_get(fx->tv, lantern, &series, &episodes, &error));
	g_assert_no_error(error);
	axis_tv_get_rules(fx->tv, &rules);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		g_autofree gchar *reason = NULL;

		if (e->n_files == 0)
			continue;
		with_file++;
		g_assert_false(axis_tv_episode_upgradable(e, series));
		g_assert_cmpint(axis_tv_episode_derive(e, series, &rules, &reason), ==, AXIS_TV_IN_LIBRARY);
		g_assert_nonnull(strstr(reason, "Migrated from Sonarr; Axis does not replace it."));
		g_assert_null(strstr(reason, "ept by hand"));
	}
	/* 1x1, the double episode's two, and 1x5. */
	g_assert_cmpuint(with_file, ==, 4);
	for (i = 0; i < 3; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
	}
	n = upgrade_searches(fx, lantern);
	g_assert_cmpstr(n, ==, "0");

	/* The connection's page offers to let them go; the confirmation writes
	 * nothing; a digest of no page is refused. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "4 files migrated from Sonarr are kept: Axis does not upgrade or replace them");
	assert_contains(page.body, "Let Axis upgrade these files");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/release");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "4 files migrated from Sonarr, of 2 series, are kept now");
	assert_contains(page.body, "is replaced at the scheduler's ordinary pace, and the old file goes to the recycle "
		"folder.");
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT count(*) FROM series_file WHERE hand_how = 'brought' AND kept", "4");
	page = post_as(fx, fx->admin, "/migrate/1/release", "/migrate/1/release",
		"digest=0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(page.status, ==, 409);
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT count(*) FROM series_file WHERE hand_how = 'brought' AND kept", "4");

	/* Confirmed: the four are let go; the link a person made by hand (Glass
	 * Harbor's, kept by that person) is as it was. */
	g_assert_true(axis_arr_release_review(fx->arr, source, &release, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(release->n_files, ==, 4);
	form = g_strdup_printf("digest=%s", release->digest);
	page = post_as(fx, fx->admin, "/migrate/1/release", "/migrate/1/release", form);
	g_assert_cmpuint(page.status, ==, 303);
	assert_contains(axis_test_reply_header(&page, "Location"), "done=released");
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT string_agg(hand_how || ':' || kept || ':' || coalesce(kept_how, '-'), ' ' ORDER BY hand_how,"
		" file_id) FROM series_file", "brought:false:- brought:false:- brought:false:- brought:false:- linked:true:-");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE text LIKE 'The episode files migrated from Sonarr%"
		"were let go by root:%'", "2");

	/* Now they are wanted for an upgrade, and the scheduler searches. */
	g_clear_pointer(&series, axis_tv_series_free);
	g_clear_pointer(&episodes, g_ptr_array_unref);
	g_assert_true(axis_tv_get(fx->tv, lantern, &series, &episodes, &error));
	g_assert_no_error(error);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (e->n_files > 0)
			g_assert_true(axis_tv_episode_upgradable(e, series));
	}
	for (i = 0; i < 3; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
	}
	g_free(n);
	n = upgrade_searches(fx, lantern);
	g_assert_cmpint(g_ascii_strtoll(n, NULL, 10), >=, 1);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* ── run/ticked ─────────────────────────────────────────────────── */

/* The run of a Sonarr confirmed with the tick; how its job ended. */
static gchar *
run_ticked(
	Fixture *fx,
	gint64   source
){
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/run", source);
	g_autofree gchar *form = NULL;
	g_autofree gchar *stale = NULL;
	/* (Runs before this one: a later run is made after a first.) */
	g_autofree gchar *before = axis_test_db_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'");
	AxisTestReply posted;

	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_no_error(error);
	g_assert_nonnull(preview->digest_start);
	form = g_strdup_printf("digest=%s&start=%s", preview->digest, preview->digest_start);
	stale = g_strdup_printf("digest=%s&start=%s", preview->digest, preview->digest);
	queue_start(fx);
	/* A tick of no page: refused whole. */
	posted = post_as(fx, fx->admin, page, action, stale);
	g_assert_cmpuint(posted.status, ==, 409);
	axis_test_reply_clear(&posted);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'arr.apply'", before);
	posted = post_as(fx, fx->admin, page, action, form);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=running-start");
	axis_test_reply_clear(&posted);
	if (!axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'"
	        " AND state IN ('queued', 'running')", "0", 120000))
		g_error("the run did not end");
	return axis_test_db_query(fx->t->db, "SELECT CASE WHEN state = 'succeeded' THEN 'succeeded' ELSE state::text ||"
		" coalesce(': ' || last_error, '') END FROM job WHERE kind = 'arr.apply' ORDER BY id DESC LIMIT 1");
}

/*
 * Decided on 2026-11-21 (the proposal's question 3), for a Sonarr: with the tick
 * the run does at its end what "Start searching" does.  The series Sonarr is
 * downloading for stays held for that, the episodes whose files Axis does not
 * see stay held, the others are started; the page says how many.
 */
static void
test_ticked(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	static const gint64 kestrel[] = { 4 };
	g_autofree gchar *state = NULL;
	AxisTestReply page;

	axis_test_sonarr_set_queue(fx->sonarr, kestrel, G_N_ELEMENTS(kestrel));
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	assert_query(fx, "SELECT payload ->> 'start' FROM job WHERE kind = 'arr.apply'", "true");
	assert_query(fx, "SELECT string_agg(t.title || '=' || coalesce(s.search_hold, 'free'), ' ' ORDER BY t.title)"
		" FROM series s JOIN meta_title t ON t.id = s.title_id WHERE s.monitor_choice = 'brought'",
		"Harrow Fields=free Kestrel Point=arr_downloading Lantern Row=free");
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'", "6");
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "0");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "Searching was started at the end of the last run, as ticked on its confirmation: for 2 "
		"series of that run. 1 was left out and stays held: Sonarr is downloading for it.");
	axis_test_reply_clear(&page);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* The tick, and Sonarr does not answer when the run ends: nothing is started,
 * every series is held, and the page says so. */
static void
test_ticked_no_answer(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autofree gchar *state = NULL;
	AxisTestReply page;

	axis_test_plex_route(axis_test_sonarr_server(fx->sonarr), "/api/v3/system/status", -1, AXIS_TEST_PLEX_HANG, 0, NULL,
		0);
	state = run_ticked(fx, source);
	g_assert_cmpstr(state, ==, "succeeded");
	axis_test_plex_clear_routes(axis_test_sonarr_server(fx->sonarr));
	assert_query(fx, "SELECT count(*) FILTER (WHERE s.search_hold = 'brought') || ' of ' || count(*) FROM series s"
		" WHERE s.monitor_choice = 'brought'", "3 of 3");
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'", "6");
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "Searching was not started at the end of the last run, though its confirmation was "
		"ticked: Sonarr did not answer");
	axis_test_reply_clear(&page);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* ── run/hold ───────────────────────────────────────────────────── */

static void
test_hold(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	AxisTvScheduleReport report;
	AxisTvAsked asked = AXIS_TV_ASKED_NOTHING;
	AxisTvRules rules;
	AxisTestReply page;
	gint64 kestrel;
	gint64 harrow;
	gboolean created = TRUE;
	guint queued = 0;
	guint i;

	run_ok(fx, source);
	kestrel = series_of(fx, "Kestrel Point");
	harrow = series_of(fx, "Harrow Fields");

	/* Kestrel Point has two aired, monitored episodes and no file: wanted, by
	 * every rule but the hold.  Its state says so, and that nothing searches. */
	g_assert_true(axis_tv_get(fx->tv, kestrel, &series, &episodes, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(series->search_hold, ==, AXIS_TV_HOLD_BROUGHT);
	axis_tv_get_rules(fx->tv, &rules);
	for (i = 0; i < episodes->len; i++) {
		g_autofree gchar *reason = NULL;

		g_assert_cmpint(axis_tv_episode_derive(g_ptr_array_index(episodes, i), series, &rules, &reason), ==,
			AXIS_TV_WANTED);
		g_assert_nonnull(strstr(reason, "Migrated from Sonarr: Axis does not search for it until you start "
			"searching."));
	}

	/* Three runs of the scheduler queue nothing, and count nothing as due. */
	for (i = 0; i < 3; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.queued, ==, 0);
		g_assert_cmpuint(report.considered, ==, 0);
	}
	/* Nor does any other way a search is queued without a person: one asked
	 * for as the scheduler, a retry or a new profile would. */
	g_assert_true(axis_tv_request_search(fx->tv, kestrel, AXIS_TV_SCOPE_SERIES, 0, "scheduled", NULL, &created, &error));
	g_assert_false(created);
	created = TRUE;
	g_assert_true(axis_tv_request_search(fx->tv, kestrel, AXIS_TV_SCOPE_SEASON, 1, "retry", NULL, &created, &error));
	g_assert_false(created);
	g_assert_no_error(error);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "0");
	/* The retry that is refused is said in the series' history: after a
	 * person's own search of a held series, nothing looks again by itself. */
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'skipped' AND text LIKE '%not searched for again:"
		" the series is held until you start searching%'", "1");

	/* A person's Search now on one episode of Harrow Fields is that search and
	 * no more: it is queued, that episode's own hold ends (the person asked
	 * for the episode Sonarr has a file for), the series stays held with its
	 * other episodes, and the scheduler stays quiet. */
	{
		g_autofree gchar *episode = axis_test_db_query(fx->t->db, EP("Harrow Fields", 1, 1, "id"));

		g_assert_true(axis_tv_ask_search(fx->tv, harrow, AXIS_TV_SCOPE_EPISODE, g_ascii_strtoll(episode, NULL, 10),
			FALSE, "root", &asked, &queued, &error));
		g_assert_no_error(error);
		g_assert_cmpint(asked, ==, AXIS_TV_ASKED_QUEUED);
	}
	assert_query(fx, "SELECT coalesce(s.search_hold, 'free') || ' ' || (SELECT count(*) FROM series_episode e"
		" WHERE e.series_id = s.id AND e.search_hold IS NOT NULL) FROM series s JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title = 'Harrow Fields'", "brought 2");
	assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold = 'brought'", "3");
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "1");

	/* A manual search of one episode of Kestrel Point only lists releases:
	 * it is queued, and nothing of the series' hold ends. */
	{
		g_autofree gchar *episode = axis_test_db_query(fx->t->db, EP("Kestrel Point", 1, 1, "id"));

		g_assert_true(axis_tv_ask_search(fx->tv, kestrel, AXIS_TV_SCOPE_EPISODE, g_ascii_strtoll(episode, NULL, 10),
			TRUE, "root", &asked, &queued, &error));
		g_assert_no_error(error);
		g_assert_cmpint(asked, ==, AXIS_TV_ASKED_QUEUED);
	}
	/* A manual search of a held episode lists too, and leaves it held. */
	{
		g_autofree gchar *episode = axis_test_db_query(fx->t->db, EP("Harrow Fields", 1, 2, "id"));

		g_assert_true(axis_tv_ask_search(fx->tv, harrow, AXIS_TV_SCOPE_EPISODE, g_ascii_strtoll(episode, NULL, 10),
			TRUE, "root", &asked, &queued, &error));
		g_assert_no_error(error);
		g_assert_cmpint(asked, ==, AXIS_TV_ASKED_QUEUED);
		assert_query(fx, EP("Harrow Fields", 1, 2, "search_hold"), "file_unseen");
	}
	/* Search now on a season of Kestrel Point: that season's search, and the
	 * series stays held. */
	g_assert_true(axis_tv_ask_search(fx->tv, kestrel, AXIS_TV_SCOPE_SEASON, 1, FALSE, "root", &asked, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpint(asked, ==, AXIS_TV_ASKED_QUEUED);
	assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold = 'brought'", "3");
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "4");
	for (i = 0; i < 3; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.queued, ==, 0);
	}
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "4");

	/* The search job itself: one the scheduler queued is skipped while the
	 * series is held, whenever it runs; a person's own passes the hold. */
	{
		g_autofree gchar *episode = axis_test_db_query(fx->t->db, EP("Harrow Fields", 1, 4, "id"));
		AxisAcquireSearchReport ran;

		memset(&ran, 0, sizeof(ran));
		g_assert_true(axis_tv_run_search(fx->tv, harrow, AXIS_TV_SCOPE_EPISODE, g_ascii_strtoll(episode, NULL, 10),
			"scheduled", NULL, NULL, &ran, &error));
		g_assert_no_error(error);
		g_assert_nonnull(ran.skipped);
		g_assert_nonnull(strstr(ran.skipped, "held"));
		memset(&ran, 0, sizeof(ran));
		g_assert_true(axis_tv_run_search(fx->tv, harrow, AXIS_TV_SCOPE_EPISODE, g_ascii_strtoll(episode, NULL, 10),
			"manual", "root", NULL, &ran, &error));
		g_assert_no_error(error);
		g_assert_true(ran.skipped == NULL || strstr(ran.skipped, "held") == NULL);
	}

	/* Search now on the whole of Harrow Fields: the person means the series,
	 * so its hold ends.  The episodes Sonarr has files for that Axis does not
	 * see stay held, and nothing is queued for them. */
	g_assert_true(axis_tv_ask_search(fx->tv, harrow, AXIS_TV_SCOPE_SERIES, 0, FALSE, "root", &asked, &queued, &error));
	g_assert_no_error(error);
	assert_query(fx, "SELECT coalesce(s.search_hold, 'free') || ' ' || (SELECT count(*) FROM series_episode e"
		" WHERE e.series_id = s.id AND e.search_hold = 'file_unseen') FROM series s JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title = 'Harrow Fields'", "free 2");
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM job j WHERE j.kind = 'tv.search' AND j.dedupe_key"
			" IN (SELECT 'tv.search:%" G_GINT64_FORMAT ":episode:' || e.id FROM series_episode e WHERE e.series_id = %"
			G_GINT64_FORMAT " AND e.search_hold IS NOT NULL)", harrow, harrow);

		assert_query(fx, sql, "0");
	}
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "4");

	/* "Start searching": the confirmation reads Sonarr's queue (GET) and
	 * changes nothing; the post ends the series' holds and queues nothing. */
	page = get_as(fx, fx->admin, "/migrate/1/start");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Start searching for the migrated series");
	assert_contains(page.body, "Starting searching affects only the episodes without a file. Of 2 series migrated from "
		"Sonarr that are still held,");
	assert_contains(page.body, "that file is kept and is not replaced.");
	assert_contains(page.body, "Sonarr is downloading for none of these series now.");
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold = 'brought'", "2");
	page = post_as(fx, fx->admin, "/migrate/1/start", "/migrate/1/start", NULL);
	g_assert_cmpuint(page.status, ==, 303);
	axis_test_reply_clear(&page);
	assert_query(fx, "SELECT count(*) FROM series s WHERE s.search_hold IS NOT NULL", "0");
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'", "4");
	/* The episodes whose files Axis does not see stay held: two of Harrow
	 * Fields, and of Lantern Row the one whose file is not in the index, the
	 * one at a place Sonarr lists under another id, and the one at the place
	 * that could not be read. */
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'", "5");

	/* The scheduler then goes at its ordinary pace: Lantern Row's one wanted
	 * episode without a file in Sonarr (2x2).  Kestrel Point's season is in
	 * the person's search already.  Through three runs nothing is queued for
	 * an episode held apart: not for the one at a place Sonarr lists under
	 * another id, whose file is on the disk Sonarr sees. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.queued, ==, 1);
	g_assert_cmpuint(report.seasons, ==, 0);
	for (i = 0; i < 2; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.queued, ==, 0);
	}
	assert_query(fx, "SELECT e.season_number || 'x' || e.number FROM job j JOIN series_episode e ON j.dedupe_key ="
		" 'tv.search:' || e.series_id || ':episode:' || e.id JOIN series s ON s.id = e.series_id JOIN meta_title t"
		" ON t.id = s.title_id WHERE j.kind = 'tv.search' AND t.title = 'Lantern Row'", "2x2");
	assert_query(fx, "SELECT count(*) FROM job j JOIN series_episode e ON j.dedupe_key LIKE 'tv.search:' || e.series_id"
		" || ':%episode:' || e.id WHERE e.search_hold IS NOT NULL AND j.dedupe_key NOT LIKE '%:list:%'", "0");
	assert_query(fx, EP("Lantern Row", 1, 6, "monitored::text || ' ' || e.search_hold"), "true file_unseen");
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM job WHERE kind = 'tv.search' AND dedupe_key ="
			" 'tv.search:%" G_GINT64_FORMAT ":season:1'", kestrel);

		assert_query(fx, sql, "1");
	}
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* ── run/refusals ───────────────────────────────────────────────── */

static void
test_refusals(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *digest = digest_now(fx, source);
	g_autofree gchar *base = axis_test_radarr_base(fx->radarr);
	AxisArrSourceSpec spec;
	AxisTestReply posted;
	gint64 radarr = 0;

	queue_start(fx);
	/* A viewer: refused, with the very digest. */
	posted = confirm_run(fx, fx->viewer, source, digest);
	g_assert_cmpuint(posted.status, ==, 403);
	axis_test_reply_clear(&posted);
	/* A digest of another page: refused, the preview shown again. */
	posted = confirm_run(fx, fx->admin, source, "0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "The preview changed after this page was made. Nothing was migrated");
	axis_test_reply_clear(&posted);
	/* The library changed after the page was made (the index holds a file at
	 * another size now): refused. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET size = size + 1 WHERE rel_path LIKE '%S01E01 - Low Water.mkv'");
	posted = confirm_run(fx, fx->admin, source, digest);
	g_assert_cmpuint(posted.status, ==, 409);
	axis_test_reply_clear(&posted);
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET size = size - 1 WHERE rel_path LIKE '%S01E01 - Low Water.mkv'");

	/* A Radarr connection cannot reach the series run: with the Sonarr's own
	 * digest its post is refused (its preview is its own), by the layer too,
	 * and nothing is queued or made. */
	memset(&spec, 0, sizeof(spec));
	spec.app = "radarr";
	spec.name = "Radarr";
	spec.address = base;
	spec.secret = RADARR_KEY;
	spec.user = "root";
	g_assert_true(axis_arr_save_source(fx->arr, 0, &spec, &radarr, &error));
	g_assert_no_error(error);
	posted = confirm_run(fx, fx->admin, radarr, digest);
	g_assert_cmpuint(posted.status, >=, 400);
	axis_test_reply_clear(&posted);
	g_assert_false(axis_arr_request_apply(fx->arr, radarr, digest, NULL, "root", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'arr.apply'", "0");
	assert_query(fx, "SELECT (SELECT count(*) FROM series WHERE search_hold IS NOT NULL) +"
		" (SELECT count(*) FROM arr_brought) + (SELECT count(*) FROM movie)", "0");
	/* And a Sonarr's run, with the right digest, makes series and no movie
	 * (run/brings counts them). */
	run_ok(fx, source);
	assert_query(fx, "SELECT (SELECT count(*) FROM arr_brought WHERE series_id IS NOT NULL) || ' ' ||"
		" (SELECT count(*) FROM arr_brought WHERE movie_id IS NOT NULL) || ' ' || (SELECT count(*) FROM movie)",
		"3 0 0");
}

/* ── run/interrupted ────────────────────────────────────────────── */

static gboolean
fail_once(
	guint     index,
	gpointer  data,
	GError  **error
){
	guint *calls = data;

	/* The first time the run reaches its second series: as a database that
	 * went away would end it. */
	if (index == 1 && (*calls)++ == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "interrupted by the test");
		return FALSE;
	}
	return TRUE;
}

static void
test_interrupted(Fixture *fx, gconstpointer data)
{
	gint64 source = prepared(fx);
	guint calls = 0;

	axis_arr_apply_hook = fail_once;
	axis_arr_apply_hook_data = &calls;
	run_ok(fx, source);
	axis_arr_apply_hook = NULL;

	/* It failed once, half-way, and the queue's retry went on: every series
	 * is whole (its hold, its row of what was brought, its links), none
	 * twice. */
	/* (Its second series was reached twice: by the try that failed there,
	 * and by the retry, whose second series is another.) */
	g_assert_cmpuint(calls, ==, 2);
	assert_query(fx, "SELECT attempts FROM job WHERE kind = 'arr.apply'", "2");
	assert_query(fx, "SELECT count(*) || ' ' || count(DISTINCT s.title_id) || ' ' || count(b.id) FROM series s"
		" LEFT JOIN arr_brought b ON b.series_id = s.id WHERE s.search_hold = 'brought'", "3 3 3");
	assert_query(fx, "SELECT count(*) FROM series_file WHERE hand_how = 'brought'", "4");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'added' AND text LIKE 'Migrated from%'", "3");
}

/* ── run/anime (add-anime-support, A1) ──────────────────────────── */

static void
add_anime_episode(
	GString  *json,
	gint64    id,
	gint64    tvdb,
	gint      season,
	gint      number,
	gboolean  monitored,
	gint64    file,
	gint      absolute	/* Sonarr's own; 0: the member is left out */
){
	g_autofree gchar *numbered = absolute > 0 ? g_strdup_printf("\"absoluteEpisodeNumber\":%d,", absolute) :
		g_strdup("");

	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"tvdbId\":%" G_GINT64_FORMAT ",\"seasonNumber\":%d,"
		"\"episodeNumber\":%d,%s\"airDate\":\"2022-02-%02d\",\"title\":\"A Made Up Episode\",\"monitored\":%s,"
		"\"hasFile\":%s,\"episodeFileId\":%" G_GINT64_FORMAT "}", json->len > 1 ? "," : "", id, tvdb, season, number,
		numbered, number + 6, monitored ? "true" : "false", file > 0 ? "true" : "false", file);
}

#define ANIME "Saltmarsh Signals"

/*
 * What Sonarr has: 7 Saltmarsh Signals, as anime, two seasons of three,
 * complete on disk (two files of season 2 named by absolute number, as a
 * fansub release names them), season 1 monitored but for one episode, season
 * 2 not.  Its own absolute numbers agree with TheTVDB's for four episodes,
 * differ for S02E02 (Sonarr 9, TheTVDB 5) and are missing for S02E03.  4
 * Kestrel Point, a standard series.  5 Juniper Hollow, of a kind Axis does
 * not know.
 */
static void
library_anime(Fixture *fx)
{
	g_autoptr(GString) episodes = g_string_new("[");
	g_autoptr(GString) files = g_string_new("[");
	g_autofree gchar *profiles = g_strdup_printf("[{\"id\":4,\"name\":\"%s\"}]", fx->profile);
	g_autofree gchar *series = g_strdup_printf("[{\"id\":7,\"title\":\"" ANIME "\",\"year\":2022,\"tvdbId\":81007,"
		"\"monitored\":true,\"seriesType\":\"anime\",\"qualityProfileId\":4,\"monitorNewItems\":\"all\","
		"\"path\":\"/tv/" ANIME " (2022)\",\"rootFolderPath\":\"/tv\",\"seasons\":[{\"seasonNumber\":1,"
		"\"monitored\":true},{\"seasonNumber\":2,\"monitored\":false}]},"
		"{\"id\":4,\"title\":\"Kestrel Point\",\"year\":2016,\"tvdbId\":81011,\"monitored\":true,"
		"\"seriesType\":\"standard\",\"qualityProfileId\":4,\"monitorNewItems\":\"all\","
		"\"path\":\"/tv/Kestrel Point (2016)\",\"rootFolderPath\":\"/tv\",\"seasons\":" SEASON_1 "},"
		"{\"id\":5,\"title\":\"Juniper Hollow\",\"year\":2012,\"tvdbId\":81012,\"monitored\":true,"
		"\"seriesType\":\"ghost\",\"qualityProfileId\":4,\"monitorNewItems\":\"all\","
		"\"path\":\"/tv/Juniper Hollow (2012)\",\"rootFolderPath\":\"/tv\",\"seasons\":" SEASON_1 "}]");

	axis_test_sonarr_set_series(fx->sonarr, series);
	axis_test_sonarr_set_profiles(fx->sonarr, profiles);
	axis_test_sonarr_set_folders(fx->sonarr, "[{\"path\":\"/tv\",\"accessible\":true}]");
	axis_test_sonarr_set_queue(fx->sonarr, NULL, 0);

	add_anime_episode(episodes, 701, 8100701, 1, 1, TRUE, 801, 1);
	add_anime_episode(episodes, 702, 8100702, 1, 2, TRUE, 802, 2);
	add_anime_episode(episodes, 703, 8100703, 1, 3, FALSE, 803, 3);
	add_anime_episode(episodes, 704, 8100704, 2, 1, FALSE, 804, 4);
	/* Sonarr says 9 where TheTVDB says 5; and gives the last one no number. */
	add_anime_episode(episodes, 705, 8100705, 2, 2, FALSE, 805, 9);
	add_anime_episode(episodes, 706, 8100706, 2, 3, FALSE, 806, 0);
	g_string_append_c(episodes, ']');
	add_file(files, 801, "/tv/" ANIME " (2022)/Season 1/" ANIME " - S01E01.mkv", 4);
	add_file(files, 802, "/tv/" ANIME " (2022)/Season 1/" ANIME " - S01E02.mkv", 5);
	add_file(files, 803, "/tv/" ANIME " (2022)/Season 1/" ANIME " - S01E03.mkv", 6);
	add_file(files, 804, "/tv/" ANIME " (2022)/Season 2/[KumoSubs] " ANIME " - 04 [1080p].mkv", 7);
	add_file(files, 805, "/tv/" ANIME " (2022)/Season 2/[KumoSubs] " ANIME " - 05 [1080p].mkv", 8);
	add_file(files, 806, "/tv/" ANIME " (2022)/Season 2/" ANIME " - S02E03.mkv", 9);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 7, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 7, files->str);
	/* TheTVDB's aired order numbers them 1 to 6 absolutely. */
	tvdb_order(fx, 81007, "1:1:8100701 1:2:8100702 1:3:8100703 2:1:8100704 2:2:8100705 2:3:8100706");

	g_string_assign(episodes, "[");
	add_episode(episodes, 401, 8101101, 1, 1, TRUE, 0);
	add_episode(episodes, 402, 8101102, 1, 2, TRUE, 0);
	g_string_append_c(episodes, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 4, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 4, "[]");
	tvdb_order(fx, 81011, "1:1:8101101 1:2:8101102");

	write_file(fx, ANIME " (2022)/Season 1", ANIME " - S01E01.mkv", "ss11");
	write_file(fx, ANIME " (2022)/Season 1", ANIME " - S01E02.mkv", "ss012");
	write_file(fx, ANIME " (2022)/Season 1", ANIME " - S01E03.mkv", "ss0013");
	write_file(fx, ANIME " (2022)/Season 2", "[KumoSubs] " ANIME " - 04 [1080p].mkv", "ss00021");
	write_file(fx, ANIME " (2022)/Season 2", "[KumoSubs] " ANIME " - 05 [1080p].mkv", "ss000022");
	write_file(fx, ANIME " (2022)/Season 2", ANIME " - S02E03.mkv", "ss0000023");
	scan(fx);
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

/*
 * Spec (tv): "An anime series is brought from Sonarr like any other":
 * Brought with its files; A file named by its absolute number; The numbers
 * differ; No longer skipped.  And: a series of a kind Axis does not know is
 * still skipped; a standard series beside it is as it always was.
 */
static void
test_anime(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autofree gchar *path = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	g_autofree gchar *disk_before = NULL;
	g_autofree gchar *disk_after = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *digest_agreeing = NULL;
	AxisTvScheduleReport report;
	AxisTestReply page;
	guint i;

	library_anime(fx);
	disk_before = disk_fingerprint(fx);
	read_from_page(fx, source);
	map_tv(fx, source);

	/* The reading: the anime series was looked up and its two lists were read,
	 * as any series'; Sonarr's own absolute numbers are kept beside its
	 * episodes; the series of an unknown kind was asked about nowhere. */
	assert_query(fx, "SELECT string_agg(title || ':' || match || ':' || coalesce(n_episodes::text, '-') || ':' ||"
		" coalesce(n_files::text, '-'), ' | ' ORDER BY seq) FROM arr_plan_item",
		ANIME ":matched:6:6 | Kestrel Point:matched:2:0 | Juniper Hollow:not_looked_up:-:-");
	assert_query(fx, "SELECT string_agg(season || 'x' || episode || '=' || coalesce(absolute_number::text, '-'), ' '"
		" ORDER BY season, episode) FROM arr_plan_episode pe JOIN arr_plan_item i ON i.plan_id = pe.plan_id"
		" AND i.seq = pe.seq WHERE i.title = '" ANIME "'", "1x1=1 1x2=2 1x3=3 2x1=4 2x2=9 2x3=-");
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), ==, 2);

	/* The preview: the anime series is listed to be migrated, with what the
	 * comparison found; the reason "Anime, ..." is nowhere; the unknown kind
	 * is still a skip. */
	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NONE], ==, 2);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_UNKNOWN_KIND], ==, 1);
	g_assert_cmpuint(preview->bring_anime, ==, 1);
	digest = g_strdup(preview->digest);
	g_clear_pointer(&preview, axis_arr_preview_free);
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "1 of these is anime in Sonarr. An anime series is migrated as any series");
	assert_contains(page.body, "Once you start searching, Axis recognises a release named by an absolute episode number "
		"for it, and searches by that number too, when TheTVDB's absolute numbers for it can be used.");
	assert_lacks(page.body, "comes later");
	assert_contains(page.body, "Anime: migrated as any series, numbered by season, and marked as anime in Axis. Absolute "
		"numbers: Sonarr and TheTVDB differ for 1 of 6 episodes, the first S02E02 (Sonarr 9, TheTVDB 5), and agree for 4. "
		"For 1 only one of the two gives a number.");
	assert_contains(page.body, "Sonarr gives a kind of series Axis does not know");
	assert_lacks(page.body, "not supported yet");
	assert_lacks(page.body, "numbered absolutely");
	axis_test_reply_clear(&page);

	/* The comparison is a report: with Sonarr's number agreeing instead, the
	 * preview counts, decides and confirms exactly the same. */
	axis_test_db_exec(fx->t->db, "UPDATE arr_plan_episode SET absolute_number = 5 WHERE absolute_number = 9");
	digest_agreeing = digest_now(fx, source);
	g_assert_cmpstr(digest_agreeing, ==, digest);
	page = get_as(fx, fx->admin, path);
	assert_contains(page.body, "Absolute numbers: Sonarr and TheTVDB agree for 5 of 6 episodes. For 1 only one of the two "
		"gives a number.");
	axis_test_reply_clear(&page);
	axis_test_db_exec(fx->t->db, "UPDATE arr_plan_episode SET absolute_number = 9 WHERE season = 2 AND episode = 2"
		" AND absolute_number = 5");

	/* A plan as a reading before this slice left it (the anime series set
	 * aside, not looked up): not migrated from that plan, and listed to be
	 * migrated once the connection is read again. */
	axis_test_db_exec(fx->t->db, "UPDATE arr_plan_item SET match = 'not_looked_up', n_episodes = NULL"
		" WHERE series_type = 'anime'");
	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NONE], ==, 1);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP], ==, 1);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_UNKNOWN_KIND], ==, 1);
	g_clear_pointer(&preview, axis_arr_preview_free);
	read_from_page(fx, source);
	g_assert_true(axis_arr_preview(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NONE], ==, 2);
	g_assert_cmpuint(preview->bring_anime, ==, 1);
	g_clear_pointer(&preview, axis_arr_preview_free);

	run_ok(fx, source);

	/* In Axis as anime, under TheTVDB's aired order from the start, monitored
	 * as in Sonarr, and held; the standard series beside it is standard. */
	assert_query(fx, "SELECT string_agg(t.title || '|' || s.kind || '|' || s.episode_source || '|' || s.monitor_choice"
		" || '|' || s.search_hold || '|' || (s.brought_absolute IS NOT NULL), ';' ORDER BY t.title) FROM series s"
		" JOIN meta_title t ON t.id = s.title_id",
		"Kestrel Point|standard|tvdb|brought|brought|false;" ANIME "|anime|tvdb|brought|brought|true");
	assert_query(fx, "SELECT count(*) FROM series s JOIN meta_title t ON t.id = s.title_id"
		" WHERE t.title = 'Juniper Hollow'", "0");
	/* Its episodes carry TheTVDB's absolute numbers, never Sonarr's. */
	assert_query(fx, "SELECT string_agg(e.season_number || 'x' || e.number || '=' || coalesce(e.absolute_number::text,"
		" '-'), ' ' ORDER BY e.season_number, e.number) FROM series_episode e JOIN series s ON s.id = e.series_id"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'", "1x1=1 1x2=2 1x3=3 2x1=4 2x2=5 2x3=6");
	/* Monitoring copied: season 1 but for its third episode, season 2 not. */
	assert_query(fx, "SELECT string_agg(e.season_number || 'x' || e.number || ':' || e.monitored, ' '"
		" ORDER BY e.season_number, e.number) FROM series_episode e JOIN series s ON s.id = e.series_id"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'",
		"1x1:true 1x2:true 1x3:false 2x1:false 2x2:false 2x3:false");
	assert_query(fx, "SELECT string_agg(ss.number || ':' || ss.monitored, ' ' ORDER BY ss.number) FROM series_season ss"
		" JOIN series s ON s.id = ss.series_id JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'",
		"1:true 2:false");

	/* Every file linked on the episode Sonarr has it on, whatever number its
	 * name states (the two named by absolute number among them, and the one
	 * whose numbers disagree), stamped as migrated and kept. */
	assert_query(fx, "SELECT string_agg(e.season_number || 'x' || e.number || '=' || regexp_replace(lf.rel_path,"
		" '^.* - ([0-9SE]+)[ .].*$', '\\1'), ' ' ORDER BY e.season_number, e.number) FROM series_file_episode sfe"
		" JOIN series_episode e ON e.id = sfe.episode_id JOIN library_file lf ON lf.id = sfe.file_id"
		" JOIN series s ON s.id = e.series_id JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'",
		"1x1=S01E01 1x2=S01E02 1x3=S01E03 2x1=04 2x2=05 2x3=S02E03");
	assert_query(fx, "SELECT count(*) || ' ' || count(*) FILTER (WHERE sf.hand_how = 'brought' AND sf.hand_by = 'root'"
		" AND sf.hand_at IS NOT NULL AND sf.kept AND sf.kept_how = 'brought' AND sf.import_id IS NULL"
		" AND sf.identity_id IS NULL AND sf.read_source = 'tvdb') FROM series_file sf JOIN series s"
		" ON s.id = sf.series_id JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'", "6 6");
	assert_query(fx, "SELECT count(*) FROM series_episode WHERE search_hold IS NOT NULL", "0");
	assert_query(fx, "SELECT b.file || ' ' || (b.counts ->> 'files') || ' ' || (b.counts ->> 'linked') || ' ' ||"
		" (b.counts ->> 'unmatched_episodes') || ' ' || (b.counts ->> 'held_episodes') FROM arr_brought b"
		" WHERE b.label = '" ANIME " (2022)'", "linked 6 6 0 0");

	/* The comparison: three counts and the episode that differs with both
	 * numbers, on the series and in what the connection brought; in the
	 * series' history; and on the list of what was migrated. */
	assert_query(fx, "SELECT (s.brought_absolute ->> 'agree') || ' ' || (s.brought_absolute ->> 'differ') || ' ' ||"
		" (s.brought_absolute ->> 'one_side') || ' ' || (s.brought_absolute -> 'list')::text FROM series s"
		" JOIN meta_title t ON t.id = s.title_id WHERE t.title = '" ANIME "'", "4 1 1 [[2, 2, 9, 5]]");
	assert_query(fx, "SELECT (b.counts -> 'absolute' ->> 'differ') || ' ' || (b.counts -> 'absolute' ->> 'agree')"
		" FROM arr_brought b WHERE b.label = '" ANIME " (2022)'", "1 4");
	assert_query(fx, "SELECT b.counts ? 'absolute' FROM arr_brought b WHERE b.label = 'Kestrel Point (2016)'", "f");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text LIKE 'Migrated from Sonarr (Sonarr)"
		" as anime. Absolute numbers: Sonarr and TheTVDB differ for 1 of 6 episodes, the first S02E02 (Sonarr 9, TheTVDB"
		" 5), and agree for 4.%'", "1");
	assert_query(fx, "SELECT count(*) FROM series_event WHERE kind = 'added' AND text LIKE 'Migrated from Sonarr (Sonarr)"
		" by root%Marked as anime, as it is there.%'", "1");
	page = get_as(fx, fx->admin, "/migrate/1/brought");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "6 of 6 files linked. Migrated as anime. Absolute numbers: Sonarr and TheTVDB differ for 1 "
		"of 6 episodes, the first S02E02 (Sonarr 9, TheTVDB 5), and agree for 4.");
	axis_test_reply_clear(&page);

	/* Nothing is searched for: not by the run, and not by three runs of the
	 * scheduler afterwards (the series is held, and its files are kept). */
	for (i = 0; i < 3; i++) {
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.queued, ==, 0);
	}
	assert_query(fx, "SELECT count(*) FROM job WHERE kind LIKE 'tv.search%'", "0");
	assert_query(fx, "SELECT (SELECT count(*) FROM series_grab) + (SELECT count(*) FROM series_search)", "0");
	/* No file on any disk was touched, and Sonarr was only read. */
	disk_after = disk_fingerprint(fx);
	g_assert_cmpstr(disk_after, ==, disk_before);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web-sonarr-run/" path, Fixture, NULL, func)
	ADD("run/anime", test_anime);
	ADD("run/brings", test_brings);
	ADD("run/name", test_name);
	ADD("run/identified", test_identified);
	ADD("run/second", test_second);
	ADD("run/twice", test_twice);
	ADD("run/named-later", test_named_later);
	ADD("run/renumbered", test_renumbered);
	ADD("run/another-way", test_another_way);
	ADD("run/migration-differs", test_migration_differs);
	ADD("run/unkept-stays-held", test_unkept_stays_held);
	ADD("run/ticked-later", test_ticked_later);
	ADD("run/ticked-nothing-held", test_ticked_nothing_held);
	ADD("run/kept", test_kept);
	ADD("run/ticked", test_ticked);
	ADD("run/ticked-no-answer", test_ticked_no_answer);
	ADD("run/hold", test_hold);
	ADD("run/refusals", test_refusals);
	ADD("run/interrupted", test_interrupted);
#undef ADD
	return g_test_run();
}
