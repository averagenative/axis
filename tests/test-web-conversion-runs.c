/*
 * test-web-conversion-runs.c - the conversion runs' pages: planning a run,
 * its review before it starts (the list a page at a time, what is not in it
 * and why, a series with its episodes), starting it (trials; replacing only
 * with a proven revision and the box ticked), where it is and why (the
 * polled block), pausing, resuming, cancelling, moving from trials to
 * replacing, taking files out, the limits, the dashboard's piece, a
 * conversion's page naming its run; administrators only, with the CSRF
 * token; and every page at scale costing the same few statements
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell and the storage, library, probe, workers and conversions
 * modules on an ephemeral loopback port, a real database and a scratch
 * disk; probe records and the worker written by SQL (as in
 * tests/test-conversion-runs.c), so no ffmpeg is needed and no encoder
 * runs.  With AXIS_TEST_RUN_PAGES set to a directory, the pages are saved
 * there (for screenshots).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-savings.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "api/axis-worker-store.h"
#include "core/axis-module-state.h"
#include "core/axis-size.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-conversion-run.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-conversions.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-library.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-workers.h"

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisStorage        *storage;
	AxisLibrary        *library;
	AxisProbe          *probe;
	AxisEstimator      *estimator;
	AxisConversions    *conv;
	AxisWorkerTokens   *tokens;
	AxisJobRegistry    *kinds;
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisWebWorkers     *workers_ui;
	AxisWebConversions *conversions_ui;
	AxisConversionRuns *runs;
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
	gchar              *disk;
	gint64              volume;
	gint64              movies;
	gint64              tv;
	gint64              space_saver;
	GPtrArray          *strings;
} Fixture;

static AxisCompatVerdict
always_safe(
	gpointer     data,
	gint64       root_id,
	const gchar *slug
){
	return AXIS_VERDICT_SAFE;
}

static const gchar *
keep(
	Fixture *fx,
	gchar   *text
){
	if (text != NULL)
		g_ptr_array_add(fx->strings, text);
	return text;
}

#define N(fx, ...) axis_test_library_n(&(fx)->view, __VA_ARGS__)
#define Q(fx, ...) keep((fx), axis_test_library_q(&(fx)->view, __VA_ARGS__))

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *statement = NULL;
	va_list args;

	va_start(args, format);
	statement = g_strdup_vprintf(format, args);
	va_end(args);
	if (!axis_db_pool_exec(fx->pool, statement, 0, NULL, NULL, &error))
		g_error("%s: %s", statement, error->message);
}

static void
add_worker(Fixture *fx)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_encoder_capabilities_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *machine = axis_worker_machine("garage");
	const AxisConvertEncoder *table = axis_convert_encoders();
	AxisDbConn *conn;
	guint i;

	sql(fx, "INSERT INTO worker (name, token_hash, registered_at, last_seen_at, max_jobs, worker_version)"
		" VALUES ('garage', md5('a') || md5('b'), now(), now(), 1, '1.0')");
	sql(fx, "INSERT INTO worker_volume (worker_id, volume_id, path) SELECT id, %" G_GINT64_FORMAT ", '/media/disk1'"
		" FROM worker WHERE name = 'garage'", fx->volume);
	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0000000000000000000000000000000000000000000000000000000000000000");
	caps->aac = caps->mov_text = caps->subrip = TRUE;
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		gboolean soft = table[i].family == AXIS_ENCODER_SOFTWARE;

		axis_encoder_capabilities_set(caps, &table[i], soft, soft && table[i].ten_bit, soft && table[i].ten_bit,
			soft ? NULL : "not on this test's worker");
	}
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_nonnull(conn);
	g_assert_true(axis_convert_capabilities_store(conn, machine, caps, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_no_error(error);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisLibraryTuning tuning;

	memset(fx, 0, sizeof(*fx));
	fx->strings = g_ptr_array_new_with_free_func(g_free);
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "conversions: *");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->dir = axis_test_storage_dir_new();
	fx->pool = axis_test_jobs_pool(fx->db, 12);
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
	fx->estimator = axis_estimator_new(fx->pool, fx->storage, fx->settings);
	fx->conv = axis_conversions_new(fx->pool, fx->settings, fx->storage, fx->probe, fx->estimator);
	axis_conversions_set_verdict(fx->conv, always_safe, NULL);

	fx->tokens = axis_worker_tokens_new();
	fx->kinds = axis_job_registry_new();
	g_assert_true(axis_worker_api_register_kinds(fx->kinds, NULL, &error));
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->workers_ui = axis_web_workers_new(fx->pool, fx->tokens, fx->estimator, 500);
	axis_web_workers_set_kinds(fx->workers_ui, fx->kinds);
	fx->conversions_ui = axis_web_conversions_new(fx->conv);
	fx->runs = axis_web_conversions_get_runs(fx->conversions_ui);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(), fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(), fx->library_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_probe_get_module(), fx->probe_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_workers_get_module(), fx->workers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_conversions_get_module(), fx->conversions_ui,
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_MIN_FREE, "1MiB", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT, "0", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_PROVE_TRIALS, "20", &error));
	g_assert_true(axis_profile_store_sync_builtins(fx->pool, NULL, &error));
	g_assert_no_error(error);

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
	fx->session = soup_session_new_with_options("timeout", 60, "idle-timeout", 60, NULL);

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
	fx->disk = axis_test_library_disk(&fx->view, "disk1");
	fx->volume = axis_test_library_add_volume(&fx->view, fx->disk, "Disk one");
	fx->movies = axis_test_library_add_root(&fx->view, fx->volume, fx->disk, "Movies", AXIS_MEDIA_MOVIES);
	fx->tv = axis_test_library_add_root(&fx->view, fx->volume, fx->disk, "TV", AXIS_MEDIA_TV);
	fx->space_saver = N(fx, "SELECT current_revision_id FROM conversion_profile WHERE slug = 'space_saver'");
	add_worker(fx);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	g_ptr_array_unref(fx->strings);
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_conversions_stop(fx->conversions_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_workers_stop(fx->workers_ui);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_conversions_free(fx->conversions_ui);
	axis_web_workers_free(fx->workers_ui);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_job_registry_unref(fx->kinds);
	axis_worker_tokens_free(fx->tokens);
	axis_conversions_unref(fx->conv);
	axis_estimator_unref(fx->estimator);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->view.media);
	g_free(fx->view.stash);
	g_free(fx->admin);
	g_free(fx->viewer);
	g_free(fx->disk);
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

/* A form posted with the CSRF token of the Conversion runs page. */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply page = get_as(fx, fx->admin, "/conversions/runs");
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
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = axis_test_web_unbroken(haystack);

	if (strstr(plain, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
save(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_RUN_PAGES");
	g_autofree gchar *file = NULL;
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	file = g_strconcat(name, ".html", NULL);
	path = g_build_filename(dir, file, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

/* A page of this module: 200, an intro, tables that declare their kind. */
static AxisTestReply
page(
	Fixture     *fx,
	const gchar *path,
	const gchar *name
){
	AxisTestReply reply = get_as(fx, fx->admin, path);

	if (reply.status != 200)
		g_error("%s: %u\n%s", path, reply.status, reply.body);
	assert_contains(reply.body, "page-intro");
	axis_test_web_assert_tables(reply.body);
	if (name != NULL)
		save(name, reply.body);
	return reply;
}

static void
expect_redirect(
	AxisTestReply *reply,
	const gchar   *location
){
	if (reply->status != 303)
		g_error("answered %u, not 303:\n%s", reply->status, reply->body);
	g_assert_cmpstr(axis_test_reply_header(reply, "Location"), ==, location);
	axis_test_reply_clear(reply);
}

static void
put_video(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	gdouble      mbits,
	gint         season,
	gint         number
){
	AxisTestProbeStream streams[3];
	gint64 id;

	memset(streams, 0, sizeof(streams));
	streams[0].kind = "video";
	streams[0].codec = "h264";
	streams[0].bitrate = (gint64)(mbits * 1000000);
	streams[0].width = 1920;
	streams[0].height = 1080;
	streams[0].fps = 24.0;
	streams[0].hdr = "sdr";
	streams[0].field_order = "progressive";
	streams[1].kind = "audio";
	streams[1].codec = "aac";
	streams[1].bitrate = 128000;
	streams[1].channels = 2;
	streams[1].sample_rate = 48000;
	streams[1].lossless = "false";
	streams[1].language = "eng";
	axis_test_scenario_put(fx->disk, root == fx->tv ? "TV" : "Movies", rel, axis_test_savings_size(streams, 1.0));
	axis_scan_result_free(axis_test_library_scan_expect(&fx->view, root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	id = axis_test_scenario_file(&fx->view, root, rel);
	axis_test_savings_probe(&fx->view, id, "matroska,webm", 1.0, streams, "false");
	sql(fx, "UPDATE media_stream SET pixel_format = 'yuv420p' WHERE file_id = %" G_GINT64_FORMAT " AND kind = 'video'",
		id);
	if (season >= 0) {
		g_autofree gchar *show = g_strndup(rel, strcspn(rel, "/"));

		sql(fx, "INSERT INTO library_identity_file (file_id, identity_id, best, quality) SELECT %" G_GINT64_FORMAT ", id,"
			" false, 'webdl_1080p' FROM library_identity WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", id,
			fx->tv, show);
		sql(fx, "INSERT INTO library_identity_episode (file_id, position, season_number, number) VALUES (%"
			G_GINT64_FORMAT ", 0, %d, %d)", id, season, number);
	}
}

static void
series(
	Fixture     *fx,
	const gchar *title,
	gint         year
){
	sql(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id, detail_fetched_at, name_keys)"
		" VALUES ('series', '%s', %d, 'ended', 'fixture', 'test-%s', now(), lower(replace('%s', ' ', '')))", title, year,
		title, title);
	sql(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence, title_id,"
		" linked_how) SELECT %" G_GINT64_FORMAT ", library_path_hash('%s'), 'folder', '%s', 'linked', 'sure', id, 'auto'"
		" FROM meta_title WHERE provider_id = 'test-%s'", fx->tv, title, title, title);
}

static void
episode(
	Fixture     *fx,
	const gchar *show,
	gint         season,
	gint         number,
	gdouble      mbits
){
	g_autofree gchar *rel = g_strdup_printf("%s/Season %02d/%s - S%02dE%02d.mkv", show, season, show, season, number);

	put_video(fx, fx->tv, rel, mbits, season, number);
}

static void
movie(
	Fixture     *fx,
	const gchar *title,
	gdouble      mbits
){
	g_autofree gchar *rel = g_strdup_printf("%s/%s.mkv", title, title);

	put_video(fx, fx->movies, rel, mbits, -1, -1);
}

/* A run made through the page; its list made by the job's work. */
static gint64
plan(
	Fixture     *fx,
	const gchar *form
){
	g_autoptr(GError) error = NULL;
	AxisTestReply reply = post_as(fx, fx->admin, "/conversions/runs", form);
	const gchar *location;
	gint64 id;

	if (reply.status != 303)
		g_error("the run was answered %u:\n%s", reply.status, reply.body);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_true(g_str_has_prefix(location, "/conversions/runs/"));
	g_assert_true(g_str_has_suffix(location, "?done=created"));
	id = g_ascii_strtoll(location + strlen("/conversions/runs/"), NULL, 10);
	axis_test_reply_clear(&reply);
	return id;
}

static void
list(
	Fixture *fx,
	gint64   run
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_conversion_runs_build_list(fx->runs, run, NULL, &error));
	g_assert_no_error(error);
}

static void
step(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_conversion_runs_step(fx->runs, NULL, &error));
	g_assert_no_error(error);
}

/* ── The pages ──────────────────────────────────────────────────── */

/*
 * Spec (conversion-runs): Planning a run; the review; starting; where it
 * is.  Only administrators, with the token.  The review shows the list
 * ranked, what is left out and why, and the start (trials only with a
 * locked revision); a run started shows what it asked for, linked to the
 * ordinary conversion page, which names its run.
 */
static void
test_pages(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	AxisTestReply reply;
	gint64 conversion;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	put_video(fx, fx->movies, "Quiet Field (2005)/Quiet Field (2005).mkv", 1.0, -1, -1);

	/* The list of runs and the plan, an administrator's only. */
	reply = page(fx, "/conversions/runs", "runs-empty");
	assert_contains(reply.body, "Conversion runs");
	assert_contains(reply.body, "There is no conversion run yet. Plan one with Plan a conversion run above.");
	assert_contains(reply.body, "Plan a conversion run");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/conversions/runs");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = page(fx, "/conversions/runs/new", "runs-new");
	assert_contains(reply.body, "Every movie library folder");
	assert_contains(reply.body, "Movies (Movies)");
	assert_contains(reply.body, "Space saver (locked: trials only)");
	assert_contains(reply.body, "No series on disk is identified yet");
	axis_test_reply_clear(&reply);
	/* The Conversions page leads here. */
	reply = page(fx, "/conversions", NULL);
	assert_contains(reply.body, "href=\"/conversions/runs/new\"");
	axis_test_reply_clear(&reply);

	/* Not without the token, nor by a viewer. */
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/conversions/runs", fx->admin, NULL, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, "/conversions/runs", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run"), ==, 0);

	run = plan(fx, form);
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	/* Make the run clicked again while it is not started: that run, nothing
	 * new, said at its controls (add-activity-feedback). */
	{
		g_autofree gchar *again = g_strdup_printf("%s?done=exists&at=run-controls#run-controls", path);
		g_autofree gchar *shown = g_strdup_printf("%s?done=exists&at=run-controls", path);
		guint i;

		for (i = 0; i < 4; i++) {
			reply = post_as(fx, fx->admin, "/conversions/runs", form);
			g_assert_cmpuint(reply.status, ==, 303);
			g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, again);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run"), ==, 1);
		g_assert_cmpint(N(fx, "SELECT count(*) FROM job WHERE kind = '" AXIS_CONVERSION_RUN_LIST_KIND "'"), ==, 1);
		reply = page(fx, shown, NULL);
		assert_contains(reply.body, "Nothing new was made: this run");
		assert_contains(reply.body, "id=\"run-controls\"");
		axis_test_reply_clear(&reply);
	}
	reply = page(fx, path, "run-listing");
	assert_contains(reply.body, "Making the list");
	assert_contains(reply.body, "hx-trigger=\"every 15s\"");
	axis_test_reply_clear(&reply);

	list(fx, run);
	reply = page(fx, path, "run-ready");
	assert_contains(reply.body, "Ready to start");
	assert_contains(reply.body, "Copper Road (2002).mkv");
	assert_contains(reply.body, "Lantern Hill (2001).mkv");
	assert_contains(reply.body, "2 movies and series, 2 files of the 3 looked at");
	assert_contains(reply.body, "Start with trials");
	assert_lacks(reply.body, "Start replacing originals");
	assert_contains(reply.body, "Replacing originals needs the version proven");
	assert_contains(reply.body, "Files left out, and why");
	g_assert_true(strstr(reply.body, "Copper Road") < strstr(reply.body, "Lantern Hill"));
	assert_lacks(reply.body, fx->dir);
	axis_test_reply_clear(&reply);
	/* What is left out, and the file it is. */
	{
		const gchar *reason = Q(fx, "SELECT reason FROM conversion_run_reason WHERE run_id = %" G_GINT64_FORMAT, run);
		g_autofree gchar *left = g_strdup_printf("%s/left-out?reason=%s", path, reason);

		g_assert_nonnull(reason);
		reply = page(fx, left, "run-left-out");
		assert_contains(reply.body, "Quiet Field (2005)/Quiet Field (2005).mkv");
		axis_test_reply_clear(&reply);
	}
	reply = get_as(fx, fx->admin, "/conversions/runs/999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Limits, then the start. */
	action = g_strdup_printf("%s/limits", path);
	reply = post_as(fx, fx->admin, action, "at_once=1&cap_files=&cap_gib=&cap_hours=48&window_from=22:00&window_to=06:00");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=limits&at=run-controls#run-controls", path);

		expect_redirect(&reply, done);
	}
	g_assert_cmpint(N(fx, "SELECT cap_seconds FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 48 * 3600);
	g_assert_cmpint(N(fx, "SELECT window_start FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 22 * 60);
	reply = post_as(fx, fx->admin, action, "at_once=9");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "at_once=1&window_from=25:00&window_to=06:00");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "at_once=1");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/start", path);
	/* Replacing is refused with a locked revision, whatever the form says. */
	reply = post_as(fx, fx->admin, action, "outcome=replace&confirm=yes");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "is locked");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "outcome=trial");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=started&at=run-controls#run-controls", path);

		expect_redirect(&reply, done);
	}
	step(fx);
	conversion = N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'asked'", run);
	g_assert_cmpint(conversion, >, 0);
	reply = page(fx, path, "run-running");
	assert_contains(reply.body, "Running");
	assert_contains(reply.body, "Files being converted or checked now");
	assert_contains(reply.body, ": Waiting for a worker");
	assert_contains(reply.body, "Pause the run");
	assert_contains(reply.body, "0 of 2 files done");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *status = g_strdup_printf("%s/status", path);

		reply = get_as(fx, fx->admin, status);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "id=\"run-status\"");
		assert_lacks(reply.body, "<html");
		axis_test_reply_clear(&reply);
	}
	/* The conversion is an ordinary one, and names its run. */
	{
		g_autofree gchar *one = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, conversion);
		g_autofree gchar *link = g_strdup_printf("href=\"%s\"", path);

		reply = page(fx, one, NULL);
		assert_contains(reply.body, "Part of");
		assert_contains(reply.body, link);
		axis_test_reply_clear(&reply);
	}
	/* The list of runs. */
	reply = page(fx, "/conversions/runs", "runs");
	assert_contains(reply.body, "Run 1: Every movie library folder");
	assert_contains(reply.body, "0 of 2");
	axis_test_reply_clear(&reply);
}

/*
 * Spec (conversion-runs): Pause, resume, cancel; trials to replacing; the
 * dashboard.  A paused run is on the dashboard as something to look at;
 * once its revision is proven the page offers replacing originals with the
 * single replacement's own sentence and box, and refuses it without the
 * box; taking files out; cancelling.
 */
static void
test_actions(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	AxisTestReply reply;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);
	run = plan(fx, form);
	list(fx, run);
	/* (add-attention-notices, after the N2 review.)  Ready and not started:
	 * no notice while its list is fresh (the person who planned it is on its
	 * page), one ten minutes after the list was finished. */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "is ready and was not started");
	axis_test_reply_clear(&reply);
	axis_test_web_read_attention_at(fx->web, 11);
	reply = get_as(fx, fx->admin, "/attention");
	assert_contains(reply.body, "id=\"attention-look\"");
	assert_contains(reply.body, "<bdi>Conversion run 1</bdi><span> is ready and was not started.</span>");
	assert_contains(reply.body, "<a href=\"/conversions/runs/1\">Open the conversion run</a>");
	axis_test_reply_clear(&reply);
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	action = g_strdup_printf("%s/start", path);
	reply = post_as(fx, fx->admin, action, "outcome=trial");
	axis_test_reply_clear(&reply);
	step(fx);

	/* Paused: on the dashboard. */
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/pause", path);
	reply = post_as(fx, fx->admin, action, "");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=paused&at=run-controls#run-controls", path);

		expect_redirect(&reply, done);
	}
	reply = page(fx, path, "run-paused");
	assert_contains(reply.body, "Paused by root.");
	assert_contains(reply.body, "Resume the run");
	axis_test_reply_clear(&reply);
	/* (Under what needs attention through the board: add-attention-notices,
	 * slice N2.  Paused by a person, it is "Worth a look".) */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Conversion run 1 (Every movie library folder)");
	assert_contains(reply.body, "attention-level\">Worth a look</span><a href=\"/conversions/runs/1\"><span></span>"
		"<bdi>Conversion run 1</bdi><span> is paused: a person paused it.</span></a>");
	save("dashboard", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/");
	assert_lacks(reply.body, "Conversion run 1");
	axis_test_reply_clear(&reply);

	/* Proven: replacing is offered, with the sentence and its box. */
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Replace originals from now on");
	assert_contains(reply.body, "For each file: The original will be deleted after the new file has passed every check;"
		" it cannot be recovered.");
	assert_contains(reply.body, "I understand: the originals will be deleted");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/replace", path);
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(Q(fx, "SELECT outcome FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "trial");
	reply = post_as(fx, fx->admin, action, "confirm=yes");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=replacing&at=run-controls#run-controls", path);

		expect_redirect(&reply, done);
	}
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || confirmed_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		==, "replace root");

	/* A unit, then a file of one, taken out. */
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/units/3/remove", path);
	reply = post_as(fx, fx->admin, action, "");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=removed&at=run-unit-3#run-unit-3", path);

		expect_redirect(&reply, done);
	}
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND unit_position = 3", run), ==, "removed");
	{
		gint64 entry = N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
			" AND unit_position = 2", run);
		g_autofree gchar *remove = g_strdup_printf("%s/entries/%" G_GINT64_FORMAT "/remove", path, entry);
		g_autofree gchar *done = g_strdup_printf("%s/units/2?done=removed&at=run-entry-%" G_GINT64_FORMAT
			"#run-entry-%" G_GINT64_FORMAT, path, entry, entry);
		g_autofree gchar *unit = g_strdup_printf("%s/units/2", path);

		reply = page(fx, unit, "run-unit");
		assert_contains(reply.body, "Take out");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, remove, "unit=2");
		expect_redirect(&reply, done);
	}

	/* Cancelled: the conversion waiting for a worker is given back. */
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/cancel", path);
	reply = post_as(fx, fx->admin, action, "");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=cancelled&at=run-controls#run-controls", path);

		expect_redirect(&reply, done);
	}
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, "cancelled");
	reply = page(fx, path, "run-cancelled");
	assert_contains(reply.body, "Cancelled by root.");
	assert_lacks(reply.body, "Pause the run");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
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
	AxisTestReply got;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strlen(location) - strlen(tail));
	got = get_as(fx, fx->admin, path);
	g_assert_cmpuint(got.status, ==, 200);
	element = strstr(got.body, id);
	notice = strstr(got.body, "notice-here");
	if (element == NULL || notice == NULL || element > notice)
		g_error("expected the notice inside %s in:\n%s", place, got.body);
	axis_test_reply_clear(&got);
	axis_test_reply_clear(reply);
}

/* Spec: a form comes back to the card or row it was posted from */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	AxisTestReply reply;
	gint64 entry;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);
	run = plan(fx, form);
	list(fx, run);
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);

	/* "What you can do": limits, start, pause, resume, replace. */
	action = g_strdup_printf("%s/limits", path);
	reply = post_as(fx, fx->admin, action, "at_once=1&cap_files=&cap_gib=&cap_hours=48");
	assert_back_to_place(fx, &reply, "run-controls");
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/start", path);
	reply = post_as(fx, fx->admin, action, "outcome=trial");
	assert_back_to_place(fx, &reply, "run-controls");
	step(fx);
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/pause", path);
	reply = post_as(fx, fx->admin, action, "");
	assert_back_to_place(fx, &reply, "run-controls");
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/resume", path);
	reply = post_as(fx, fx->admin, action, "");
	assert_back_to_place(fx, &reply, "run-controls");
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/replace", path);
	reply = post_as(fx, fx->admin, action, "confirm=yes");
	assert_back_to_place(fx, &reply, "run-controls");

	/* A unit's row in the list: taken out. */
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/units/3/remove", path);
	reply = post_as(fx, fx->admin, action, "");
	assert_back_to_place(fx, &reply, "run-unit-3");

	/* A file's row on the unit page: taken out. */
	entry = N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND unit_position = 2", run);
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/entries/%" G_GINT64_FORMAT "/remove", path, entry);
	reply = post_as(fx, fx->admin, action, "unit=2");
	{
		g_autofree gchar *row = g_strdup_printf("run-entry-%" G_GINT64_FORMAT, entry);

		assert_back_to_place(fx, &reply, row);
	}

	/* Cancelled: the controls go with the run, so the notice takes their
	 * place at the top; the answer still names them. */
	g_clear_pointer(&action, g_free);
	action = g_strdup_printf("%s/cancel", path);
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "at=run-controls");
	axis_test_reply_clear(&reply);
}

/*
 * Spec (conversion-runs): A series with its episodes.  The run's list names
 * the series, its unit page lists its episodes in order with their
 * progress; a run of one series is planned from the series on disk.
 */
static void
test_series_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	gint64 title;
	gint64 run;

	series(fx, "Harbour Lights", 2011);
	episode(fx, "Harbour Lights", 1, 2, 10);
	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Harbour Lights", 1, 3, 10);
	title = N(fx, "SELECT id FROM meta_title WHERE title = 'Harbour Lights'");
	reply = page(fx, "/conversions/runs/new", NULL);
	assert_contains(reply.body, "Harbour Lights (2011)");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/conversions/runs", "scope=series");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("scope=series&series=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT, title,
		fx->space_saver);
	run = plan(fx, form);
	list(fx, run);
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "The series Harbour Lights (2011)");
	assert_contains(reply.body, "0 of 3 episodes done");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/units/1", run);
	reply = page(fx, path, "run-series");
	assert_contains(reply.body, "S01E01");
	g_assert_true(strstr(reply.body, "S01E01") < strstr(reply.body, "S01E02"));
	g_assert_true(strstr(reply.body, "S01E02") < strstr(reply.body, "S01E03"));
	assert_contains(reply.body, "its episodes are converted one after another");
	axis_test_reply_clear(&reply);
}

/* ── At scale ───────────────────────────────────────────────────── */

#define SCALE_MOVIES   5000
#define SCALE_SERIES   1500
#define SCALE_EPISODES 30

/* 5,000 movies and 1,500 series of 30 episodes, made by SQL as the
 * scanner, the probe and identification leave them. */
static gint64
make_scale(Fixture *fx)
{
	sql(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation, last_seen_generation,"
		" changed_generation, changed) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'Shelf Film ' || j || '/Shelf Film '"
		" || j || '.mkv', 'video', 1000000 + j * 37, 1, 0, 1, 0, 9000000 + j, 1, false, false, false, 1, 1, 1, false"
		" FROM generate_series(1, %d) j", fx->movies, fx->volume, SCALE_MOVIES);
	sql(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id, detail_fetched_at, name_keys)"
		" SELECT 'series', 'Shelf Series ' || j, 1960 + j %% 60, 'ended', 'fixture', 'shelf' || j, now(),"
		" 'shelfseries' || j FROM generate_series(1, %d) j", SCALE_SERIES);
	sql(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence, title_id,"
		" linked_how) SELECT %" G_GINT64_FORMAT ", library_path_hash(t.title), 'folder', t.title, 'linked', 'sure', t.id,"
		" 'auto' FROM meta_title t WHERE t.provider_id LIKE 'shelf%%'", fx->tv);
	sql(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation, last_seen_generation,"
		" changed_generation, changed) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", i.rel_path || '/Season 0'"
		" || (1 + e / 16) || '/' || i.rel_path || ' - S0' || (1 + e / 16) || 'E' || lpad((1 + e %% 16)::text, 2, '0')"
		" || '.mkv', 'video', 700000 + i.id * 11 + e, 1, 0, 1, 0, 8000000 + i.id * 100 + e, 1, false, false, false,"
		" 1, 1, 1, false FROM library_identity i, generate_series(0, %d) e WHERE i.root_id = %" G_GINT64_FORMAT, fx->tv,
		fx->volume, SCALE_EPISODES - 1, fx->tv);
	sql(fx, "INSERT INTO library_identity_file (file_id, identity_id, best, quality) SELECT f.id, i.id, false,"
		" 'webdl_1080p' FROM library_file f JOIN library_identity i ON i.root_id = f.root_id"
		" AND i.rel_path = split_part(f.rel_path, '/', 1)");
	sql(fx, "INSERT INTO library_identity_episode (file_id, position, season_number, number) SELECT lif.file_id, 0,"
		" substring(f.rel_path FROM ' - S0([0-9])E')::int, substring(f.rel_path FROM ' - S0[0-9]E([0-9]+)')::int"
		" FROM library_identity_file lif JOIN library_file f ON f.id = lif.file_id");
	/* A tenth of the movies hard-linked (a download client's): left out as
	 * pinned. */
	sql(fx, "UPDATE library_file SET nlink = 2, pinned = true, pinned_links = true WHERE root_id = %" G_GINT64_FORMAT
		" AND id %% 10 = 0", fx->movies);
	/* About 8 Mbit/s for a movie, 5.6 for an episode, over the second the
	 * size holds, as the probe records it. */
	sql(fx, "INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, bitrate, stream_count, video_streams, audio_streams,"
		" subtitle_streams, video_codec, width, height, resolution_class, bit_depth, frame_rate, hdr_format, hdr_uncertain,"
		" bits_per_pixel, interlaced, audio_codec, audio_channels, audio_sample_rate, audio_lossless)"
		" SELECT f.id, 'ok', %d, f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, 'matroska,webm',"
		" 1000, f.size * 8 - 128000, 2, 1, 1, 0, 'h264', 1920, 1080, '1080p', 8, 24, 'sdr', false,"
		" (f.size * 8 - 256000)::float8 / (1920 * 1080 * 24), false, 'aac', 2, 48000, false FROM library_file f",
		AXIS_PROBE_VERSION);
	sql(fx, "INSERT INTO media_stream (file_id, idx, kind, codec, bitrate, width, height, bit_depth, frame_rate,"
		" field_order, hdr_format, pixel_format, duration_ms, is_default) SELECT m.file_id, 0, 'video', m.video_codec,"
		" m.bitrate - 128000, 1920, 1080, 8, 24, 'progressive', 'sdr', 'yuv420p', 1000, true FROM media_info m"
		" UNION ALL SELECT m.file_id, 1, 'audio', 'aac', 128000, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 1000, true"
		" FROM media_info m");
	sql(fx, "UPDATE media_stream SET channels = 2, sample_rate = 48000, lossless = false, language = 'eng'"
		" WHERE kind = 'audio'");
	axis_test_db_exec(fx->db, "ANALYZE");
	return N(fx, "SELECT count(*) FROM library_file");
}

/* GETs @path and the statements the server sent for it. */
static guint
statements_for(
	Fixture     *fx,
	const gchar *path,
	const gchar *must
){
	guint before = axis_db_pool_get_statements_sent(fx->pool);
	gint64 started = g_get_monotonic_time();
	AxisTestReply reply = page(fx, path, NULL);
	guint spent = axis_db_pool_get_statements_sent(fx->pool) - before;

	g_test_message("%s: %u statements, %" G_GINT64_FORMAT " ms", path, spent, (g_get_monotonic_time() - started) / 1000);
	if (must != NULL)
		assert_contains(reply.body, must);
	axis_test_reply_clear(&reply);
	return spent;
}

/*
 * Spec (conversion-runs): The list works for a large library.  50,000 files
 * and 1,500 series: the list is made by its job in a number of statements
 * that grows with batches of 2,000 files, not with files, and every page of
 * it (the run with its first units, a page of units at the end of the list,
 * the last series with its episodes, the files left out) costs the same
 * few statements wherever it is in the list.
 */
static void
test_scale(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *order = NULL;
	guint first;
	guint other;
	guint before;
	guint spent;
	gint64 started;
	gint64 files;
	gint64 run;
	gint last;

	files = make_scale(fx);
	g_test_message("%" G_GINT64_FORMAT " files, %d series", files, SCALE_SERIES);
	g_assert_cmpint(files, >=, 50000);
	form = g_strdup_printf("scope=roots&root_%" G_GINT64_FORMAT "=yes&root_%" G_GINT64_FORMAT "=yes&revision=%"
		G_GINT64_FORMAT, fx->movies, fx->tv, fx->space_saver);
	run = plan(fx, form);

	/* The list, by its job's work. */
	before = axis_db_pool_get_statements_sent(fx->pool);
	started = g_get_monotonic_time();
	list(fx, run);
	spent = axis_db_pool_get_statements_sent(fx->pool) - before;
	g_test_message("the list of %" G_GINT64_FORMAT " files: %u statements, %" G_GINT64_FORMAT " ms", files, spent,
		(g_get_monotonic_time() - started) / 1000);
	/* Each batch of 2,000: a fetch, its streams, its entries and the files
	 * left out; then the units and the totals. */
	g_assert_cmpuint(spent, <=, 4 * (files / 2000 + 1) + 20);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
		" AND kind = 'series'", run), ==, SCALE_SERIES);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run) +
		N(fx, "SELECT coalesce(sum(files), 0) FROM conversion_run_reason WHERE run_id = %" G_GINT64_FORMAT, run), ==, files);
	/* A series' episodes one after another, in order. */
	order = axis_test_library_q(&fx->view, "SELECT string_agg(regexp_replace(e.rel_path, '^.* - ', ''), ','"
		" ORDER BY e.position) FROM (SELECT * FROM conversion_run_entry x WHERE x.run_id = %" G_GINT64_FORMAT
		" AND x.unit_position = (SELECT position FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
		" AND kind = 'series' AND files = %d ORDER BY position LIMIT 1) ORDER BY x.position LIMIT 3) e", run, run,
		SCALE_EPISODES);
	g_assert_cmpstr(order, ==, "S01E01.mkv,S01E02.mkv,S01E03.mkv");

	/* Every page, the same few statements wherever it is. */
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	first = statements_for(fx, path, "Ready to start");
	last = (gint)N(fx, "SELECT max(position) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT, run);
	{
		g_autofree gchar *end = g_strdup_printf("%s?after=%d", path, last - 20);

		other = statements_for(fx, end, "Ready to start");
		g_assert_cmpuint(other, ==, first);
	}
	g_assert_cmpuint(first, <=, 16);
	{
		gint series_last = (gint)N(fx, "SELECT max(position) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
			" AND kind = 'series'", run);
		gint series_first = (gint)N(fx, "SELECT min(position) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
			" AND kind = 'series'", run);
		g_autofree gchar *a = g_strdup_printf("%s/units/%d", path, series_first);
		g_autofree gchar *b = g_strdup_printf("%s/units/%d", path, series_last);

		g_assert_cmpuint(statements_for(fx, a, "episodes done"), ==, statements_for(fx, b, "episodes done"));
		g_assert_cmpuint(statements_for(fx, b, NULL), <=, 12);
	}
	{
		const gchar *reason = Q(fx, "SELECT reason FROM conversion_run_reason WHERE run_id = %" G_GINT64_FORMAT
			" ORDER BY files DESC LIMIT 1", run);
		g_autofree gchar *left = g_strdup_printf("%s/left-out?reason=%s", path, reason);

		g_assert_nonnull(reason);
		g_assert_cmpuint(statements_for(fx, left, "Shelf"), <=, 6);
	}
	g_assert_cmpuint(statements_for(fx, "/conversions/runs", "Run 1:"), <=, 6);

	/* Paging reaches every unit once, two statements a page. */
	{
		guint pages = 0;
		gint64 seen = 0;
		gint after = 0;

		for (;;) {
			g_autoptr(GPtrArray) units = NULL;
			gboolean more = FALSE;

			before = axis_db_pool_get_statements_sent(fx->pool);
			g_assert_true(axis_conversion_runs_units(fx->runs, run, after, AXIS_CONVERSION_RUN_PAGE, &units, &more,
				&error));
			g_assert_no_error(error);
			g_assert_cmpuint(axis_db_pool_get_statements_sent(fx->pool) - before, <=, 2);
			pages++;
			seen += units->len;
			if (!more)
				break;
			after = ((AxisConversionRunUnit *)g_ptr_array_index(units, units->len - 1))->position;
		}
		g_assert_cmpint(seen, ==, N(fx, "SELECT count(*) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT, run));
		g_assert_cmpuint(pages, ==, (seen + AXIS_CONVERSION_RUN_PAGE - 1) / AXIS_CONVERSION_RUN_PAGE);
	}
}

/* ── Kept files, by choice (convert-kept-files) ─────────────────── */

/* Marks the movie file at @title kept by hand (a made-up movie of its own). */
static gint64
keep_movie(
	Fixture     *fx,
	const gchar *title
){
	g_autofree gchar *rel = g_strdup_printf("%s/%s.mkv", title, title);
	gint64 file = axis_test_scenario_file(&fx->view, fx->movies, rel);

	sql(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Kept %" G_GINT64_FORMAT
		"', 2001, 'fixture', 'kept-%" G_GINT64_FORMAT "')", file, file);
	sql(fx, "INSERT INTO movie (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file);
	sql(fx, "INSERT INTO movie_file (file_id, movie_id, quality, kept) SELECT %" G_GINT64_FORMAT
		", m.id, 'webdl_1080p', true FROM movie m JOIN meta_title t ON t.id = m.title_id"
		" WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file, file);
	return file;
}

/*
 * Spec (conversion-runs, Kept files are converted only by choice; Kept
 * files added to a run): the plan offers the choice, off, with its sentence;
 * a run made without it says that kept files are left out and offers, on the
 * row of those files, to add them; only an administrator with the token may;
 * the page then says who chose it; a run made with the choice lists them
 * from the start; and the action is refused for a run that is running or has
 * a file in flight, whatever the form says.
 */
static void
test_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *chosen_form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *done = NULL;
	AxisTestReply reply;
	gint64 run;
	gint64 chosen;
	gint64 busy;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	keep_movie(fx, "Lantern Hill (2001)");
	keep_movie(fx, "Glass Orchard (2003)");

	/* The plan: the choice, not ticked, with what it means. */
	reply = page(fx, "/conversions/runs/new", "runs-new-kept");
	assert_contains(reply.body, "Also convert files Axis keeps");
	assert_contains(reply.body, "A kept file is re-encoded in place after every check passes. It stays kept, so Axis "
		"still does not replace it with another release.");
	assert_contains(reply.body, "name=\"convert_kept\"");
	assert_lacks(reply.body, "name=\"convert_kept\" checked");
	axis_test_reply_clear(&reply);

	/* A viewer cannot make a run with it, nor anyone without the token. */
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);
	chosen_form = g_strdup_printf("%s&convert_kept=yes", form);
	reply = post_as(fx, fx->viewer, "/conversions/runs", chosen_form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/conversions/runs", fx->admin, NULL, chosen_form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run"), ==, 0);

	/* Made without it: kept files are left out, and can be added. */
	run = plan(fx, form);
	list(fx, run);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "false");
	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	action = g_strdup_printf("%s/include-kept", path);
	reply = page(fx, path, "run-kept-left-out");
	assert_contains(reply.body, "Files Axis keeps");
	assert_contains(reply.body, "Left out: the run converts no file that Axis keeps");
	assert_contains(reply.body, "Kept: Axis does not replace this file");
	assert_contains(reply.body, "Add these 2 files to the run");
	assert_contains(reply.body, "/include-kept\"");
	assert_contains(reply.body, "1 movies and series, 1 files of the 3 looked at");
	/* A run not started: nothing is converted until it is, and how is
	 * chosen then; no box to tick here. */
	assert_contains(reply.body, "They join the list. Nothing is converted until you start the run: with trials, or "
		"replacing originals once you confirm that for every file of the list.");
	assert_lacks(reply.body, "This run replaces originals");
	assert_lacks(reply.body, "name=\"confirm\"");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, action, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", action, fx->admin, NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT,
		run), ==, "ready false");

	/* An administrator adds them: back at the row, the list being added to. */
	reply = post_as(fx, fx->admin, action, "");
	done = g_strdup_printf("%s?done=kept&at=run-left-out#run-left-out", path);
	expect_redirect(&reply, done);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || relist_state || ' ' || convert_kept_by FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, run), ==, "listing ready root");
	{
		g_autofree gchar *shown = g_strdup_printf("%s?done=kept&at=run-left-out", path);

		reply = page(fx, shown, "run-kept-adding");
		assert_contains(reply.body, "The files Axis keeps are being added to the list");
		assert_contains(reply.body, "Adding the files Axis keeps");
		assert_contains(reply.body, "id=\"run-left-out\"");
		assert_lacks(reply.body, "Add these 2 files to the run");
		axis_test_reply_clear(&reply);
	}
	list(fx, run);
	reply = page(fx, path, "run-kept-added");
	assert_contains(reply.body, "Ready to start");
	assert_contains(reply.body, "Included: a kept file is converted in place after every check passes, and stays kept. "
		"Chosen by root, ");
	assert_contains(reply.body, "3 movies and series, 3 files of the 3 looked at");
	assert_contains(reply.body, "Every file looked at is in the list.");
	assert_lacks(reply.body, "Add these");
	axis_test_reply_clear(&reply);
	/* Not twice. */
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "converts the files Axis keeps already");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N(fx, "SELECT (SELECT count(*) FROM movie_file WHERE kept)"), ==, 2);

	/* The run has the choice now: the same selection asked for with it, not
	 * started yet, is this run. */
	reply = post_as(fx, fx->admin, "/conversions/runs", chosen_form);
	{
		g_autofree gchar *again = g_strdup_printf("%s?done=exists&at=run-controls#run-controls", path);

		expect_redirect(&reply, again);
	}
	/* The same selection without the choice is another run; and anything
	 * but the tick is no choice. */
	{
		g_autofree gchar *odd = g_strdup_printf("%s&convert_kept=1", form);

		busy = plan(fx, odd);
		g_assert_cmpint(busy, !=, run);
		g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, busy), ==,
			"false");
		list(fx, busy);
	}
	/* Made with the choice: the kept files are listed from the start. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_conversion_runs_cancel(fx->runs, run, "root", &error));
		g_assert_no_error(error);
	}
	chosen = plan(fx, chosen_form);
	g_assert_cmpint(chosen, !=, run);
	list(fx, chosen);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text || ' ' || convert_kept_by FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, chosen), ==, "true root");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, chosen), ==, 3);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_reason WHERE run_id = %" G_GINT64_FORMAT, chosen), ==, 0);

	/* Running, then paused with a file in flight: refused, and the row says
	 * what to do instead. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *busy_path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, busy);
		g_autofree gchar *busy_action = g_strdup_printf("%s/include-kept", busy_path);

		g_assert_true(axis_conversion_runs_start(fx->runs, busy, AXIS_CONVERSION_TRIAL, FALSE, "root", &error));
		g_assert_no_error(error);
		reply = page(fx, busy_path, NULL);
		assert_contains(reply.body, "Pause the run to add these files.");
		assert_lacks(reply.body, "Add these 2 files to the run");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, busy_action, "");
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
		step(fx);
		g_assert_true(axis_conversion_runs_pause(fx->runs, busy, "root", &error));
		g_assert_no_error(error);
		reply = page(fx, busy_path, NULL);
		assert_contains(reply.body, "You can add these files once nothing of the run is in flight.");
		assert_lacks(reply.body, "Add these 2 files to the run");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, busy_action, "");
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "in flight");
		axis_test_reply_clear(&reply);
		g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT,
			busy), ==, "paused false");
		reply = post_as(fx, fx->admin, "/conversions/runs/999999/include-kept", "");
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
	}
}

/*
 * Spec (conversion-execution, A kept file is converted only by choice): the
 * confirmation of a replacement of a kept file that nothing else holds back
 * offers the choice as a tick of its own, not ticked; without the tick the
 * request is refused by the condition, with it the conversion is asked for
 * and carries the choice; only an administrator may.
 */
static void
test_kept_single(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *new_replace = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *ticked = NULL;
	AxisTestReply reply;
	gint64 file;
	gint64 other;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	file = keep_movie(fx, "Lantern Hill (2001)");
	other = axis_test_scenario_file(&fx->view, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv");
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);

	new_replace = g_strdup_printf("/conversions/new?file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT
		"&outcome=replace", file, fx->space_saver);
	reply = page(fx, new_replace, "new-replace-kept");
	assert_contains(reply.body, "Kept: Axis does not replace this file.");
	assert_contains(reply.body, "Also convert files Axis keeps");
	assert_contains(reply.body, "A kept file is re-encoded in place after every check passes. It stays kept");
	assert_contains(reply.body, "name=\"convert_kept\"");
	assert_lacks(reply.body, "name=\"convert_kept\" checked");
	assert_contains(reply.body, "I understand: the original will be deleted");
	assert_lacks(reply.body, "This conversion cannot start now");
	axis_test_reply_clear(&reply);
	/* A file that is not kept is not offered it. */
	{
		g_autofree gchar *plain = g_strdup_printf("/conversions/new?file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT
			"&outcome=replace", other, fx->space_saver);

		reply = page(fx, plain, NULL);
		assert_lacks(reply.body, "convert_kept");
		axis_test_reply_clear(&reply);
	}

	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT "&outcome=replace&confirm=yes", file,
		fx->space_saver);
	ticked = g_strdup_printf("%s&convert_kept=yes", form);
	/* Without the tick: refused by the condition. */
	reply = post_as(fx, fx->admin, "/conversions", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Kept: Axis does not replace this file.");
	axis_test_reply_clear(&reply);
	/* Not a viewer's, nor without the token. */
	reply = post_as(fx, fx->viewer, "/conversions", ticked);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/conversions", fx->admin, NULL, ticked);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion"), ==, 0);

	reply = post_as(fx, fx->admin, "/conversions", ticked);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || requested_by FROM conversion"
		" WHERE file_id = %" G_GINT64_FORMAT, file), ==, "replace true root");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM movie_file WHERE file_id = %" G_GINT64_FORMAT " AND kept", file), ==, 1);
	reply = page(fx, "/conversions/1", NULL);
	assert_contains(reply.body, "Asked for with the choice to convert kept files: if the file is kept, it is converted "
		"in place and stays kept.");
	axis_test_reply_clear(&reply);
}

/* ── Progress bars ──────────────────────────────────────────────── */

/*
 * The figures of a conversion in flight and their words, at their edges: a
 * fraction reported past the end is the end; one that is not a number is
 * nothing known; a speed below once real time has two decimals, and one
 * too small to say is not said; the time left of an enormous figure is a
 * year at most.  A bar of a value that is not a number is at 0, and of a
 * max that is not above 0 is of 1.
 */
static void
test_progress_figures(void)
{
	AxisConversionProgress p;
	HtmxNode *bar;
	gchar *text;

	axis_conversion_progress("encoding", "trial", "running", 1.2, 120, NULL, -1.0, -1, 600000, &p);
	g_assert_cmpstr(p.stage, ==, "encoding");
	g_assert_cmpfloat(p.fraction, ==, 1.0);
	g_assert_cmpint(p.left_s, ==, 0);
	g_assert_cmpfloat_with_epsilon(p.speed, 5.0, 1e-9);
	text = axis_web_conversions_progress_text(&p);
	g_assert_cmpstr(text, ==, "Encoding: 100%, 5.0 times real time");
	g_free(text);

	axis_conversion_progress("encoding", "trial", "running", NAN, 120, NULL, -1.0, -1, 600000, &p);
	g_assert_cmpfloat(p.fraction, ==, -1.0);
	g_assert_cmpfloat(p.speed, ==, -1.0);
	text = axis_web_conversions_progress_text(&p);
	g_assert_cmpstr(text, ==, "Encoding: no progress reported yet");
	g_free(text);

	/* 4 percent of a ten-minute source in ten minutes. */
	axis_conversion_progress("encoding", "trial", "running", 0.04, 600, NULL, -1.0, -1, 600000, &p);
	text = axis_web_conversions_progress_text(&p);
	g_assert_cmpstr(text, ==, "Encoding: 4%, 0.04 times real time, about 4 hours left");
	g_free(text);

	/* 1 percent of a one-second source in an hour: too slow to say. */
	axis_conversion_progress("encoding", "trial", "running", 0.01, 3600, NULL, -1.0, -1, 1000, &p);
	g_assert_cmpfloat(p.speed, >, 0.0);
	text = axis_web_conversions_progress_text(&p);
	g_assert_cmpstr(text, ==, "Encoding: 1%, about 99 hours left");
	g_free(text);

	/* Checks running for an age: a year left at most, and no speed. */
	axis_conversion_progress("verifying", "replace", NULL, -1.0, -1, "running", 0.01, G_MAXINT64 / 2, 600000, &p);
	g_assert_cmpstr(p.stage, ==, "checking");
	g_assert_cmpint(p.left_s, ==, G_GINT64_CONSTANT(31622400));
	g_assert_cmpfloat(p.speed, ==, -1.0);
	text = axis_web_conversions_progress_text(&p);
	g_assert_cmpstr(text, ==, "Checking: 1%, about 8784 hours left");
	g_free(text);

	bar = axis_web_conversions_bar(NAN, 0.0, "Nothing known");
	text = htmx_element_render(HTMX_ELEMENT(bar));
	assert_contains(text, " max=\"1\"");
	assert_contains(text, " value=\"0\"");
	g_free(text);
	g_object_unref(bar);
}

/*
 * The <progress> element of @body named @label (its accessible name) has
 * @max and @value as its attributes (in whatever order they are written), or
 * neither when @max is NULL: a bar without a value is indeterminate.  The
 * same words stand beside it as text.
 */
static void
assert_bar(
	const gchar *body,
	const gchar *label,
	const gchar *max,
	const gchar *value
){
	g_autofree gchar *plain = axis_test_web_unbroken(body);
	g_autofree gchar *name = g_strdup_printf("aria-label=\"%s\"", label);
	g_autofree gchar *words = g_strdup_printf("<span class=\"conversion-progress-text\">%s</span>", label);
	g_autofree gchar *tag = NULL;
	const gchar *at = strstr(plain, name);
	const gchar *open;
	const gchar *close;

	if (at == NULL)
		g_error("expected a bar named '%s' in:\n%s", label, body);
	for (open = at; open > plain && *open != '<'; open--)
		;
	close = strchr(at, '>');
	g_assert_nonnull(close);
	tag = g_strndup(open, (gsize)(close - open + 1));
	if (!g_str_has_prefix(tag, "<progress "))
		g_error("'%s' names %s, not a progress element", label, tag);
	if (max == NULL) {
		if (strstr(tag, "value=") != NULL || strstr(tag, "max=") != NULL)
			g_error("the bar '%s' should have no value (indeterminate): %s", label, tag);
	} else {
		g_autofree gchar *max_attr = g_strdup_printf(" max=\"%s\"", max);
		g_autofree gchar *value_attr = g_strdup_printf(" value=\"%s\"", value);

		if (strstr(tag, max_attr) == NULL || strstr(tag, value_attr) == NULL)
			g_error("the bar '%s' should be %s of %s: %s", label, value, max, tag);
	}
	if (strstr(plain, words) == NULL)
		g_error("expected the words '%s' beside the bar in:\n%s", label, body);
}

/* Half way after two minutes of a ten-minute source. */
#define HALF "Encoding: 50%, 2.5 times real time, about 2 minutes left"

/* The encode job of @conversion as a worker has it now: running since
 * @seconds ago, having reported @progress ("NULL" for nothing yet). */
static void
job_running(
	Fixture     *fx,
	gint64       conversion,
	gint         seconds,
	const gchar *progress
){
	sql(fx, "UPDATE conversion SET state = 'encoding' WHERE id = %" G_GINT64_FORMAT, conversion);
	sql(fx, "UPDATE job SET state = 'running', attempts = 1, lease_token = coalesce(lease_token, gen_random_uuid()),"
		" lease_expires_at = now() + interval '1 hour', started_at = now() - interval '%d seconds', progress = %s,"
		" progress_message = CASE WHEN %s IS NULL THEN NULL ELSE 'converting: ' || round(%s * 100) || '%% of the source"
		" read' END WHERE id = (SELECT job_id FROM conversion WHERE id = %" G_GINT64_FORMAT ")", seconds, progress,
		progress, progress, conversion);
}

/*
 * Spec (conversion-execution, Progress that can be seen; conversion-runs, A
 * run's progress as bars): a conversion in flight has a progress bar with
 * its words beside it and as its name: no value (indeterminate) while
 * nothing is known, the percent of 100 at 0, in the middle and at the end,
 * the speed and the time left once they can be worked out, the stage in
 * words; the block asks for itself again while the conversion is in flight
 * and not after; the Latest conversions table and a run's page carry the
 * same bar, and the run's page a bar of its files and one of what it saved.
 */
static void
test_progress(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *run_path = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *progress = NULL;
	AxisTestReply reply;
	gint64 conversion;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);
	run = plan(fx, form);
	list(fx, run);
	run_path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	status = g_strdup_printf("%s/status", run_path);

	/* A run that has not started: its files as a bar at nothing of two. */
	reply = get_as(fx, fx->admin, status);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_bar(reply.body, "0 of 2 files settled (0%)", "2", "0");
	axis_test_reply_clear(&reply);

	g_assert_true(axis_conversion_runs_start(fx->runs, run, AXIS_CONVERSION_TRIAL, FALSE, "root", &error));
	g_assert_no_error(error);
	step(fx);
	conversion = N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'asked'", run);
	g_assert_cmpint(conversion, >, 0);
	/* Ten minutes of source from here on, so that a speed can be worked out
	 * (the files were listed and asked for as the second they are). */
	sql(fx, "UPDATE media_info SET duration_ms = 600000");
	one = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, conversion);
	progress = g_strdup_printf("%s/progress", one);

	/* Only an administrator's. */
	reply = get_as(fx, fx->viewer, progress);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* A conversion that is not there: a block that asks for nothing more
	 * (htmx swaps no error, so an error would be asked for again). */
	reply = get_as(fx, fx->admin, "/conversions/999999/progress");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"conversion-progress\"");
	assert_contains(reply.body, "There is no such conversion any more.");
	assert_lacks(reply.body, "hx-get");
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/conversions/0/progress");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "hx-get");
	axis_test_reply_clear(&reply);

	/* Waiting for a worker: nothing is known, the bar has no value. */
	reply = get_as(fx, fx->admin, progress);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<html");
	assert_contains(reply.body, "id=\"conversion-progress\"");
	assert_contains(reply.body, "hx-trigger=\"every 5s\"");
	assert_contains(reply.body, "hx-swap=\"outerHTML\"");
	assert_bar(reply.body, "Waiting for a worker", NULL, NULL);
	axis_test_reply_clear(&reply);

	/* Taken by a worker that has reported nothing yet: indeterminate. */
	job_running(fx, conversion, 120, "NULL");
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, "Encoding: no progress reported yet", NULL, NULL);
	assert_lacks(reply.body, "value=");
	axis_test_reply_clear(&reply);

	/* 0 percent: a value, of 100. */
	job_running(fx, conversion, 120, "0");
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, "Encoding: 0%", "100", "0");
	axis_test_reply_clear(&reply);

	/* Half way after two minutes: 2.5 times real time, about two minutes left. */
	job_running(fx, conversion, 120, "0.5");
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, HALF, "100", "50");
	assert_contains(reply.body, "converting: 50% of the source read");
	axis_test_reply_clear(&reply);
	/* The same on the conversion's page, in the Latest conversions table and
	 * its fragment, and on the run's page for the file in flight. */
	reply = page(fx, one, "conversion-encoding");
	assert_contains(reply.body, "id=\"conversion-progress\"");
	assert_bar(reply.body, HALF, "100", "50");
	axis_test_reply_clear(&reply);
	reply = page(fx, "/conversions", "conversions-encoding");
	assert_contains(reply.body, "id=\"conversions-latest\"");
	assert_contains(reply.body, "hx-get=\"/conversions/latest\"");
	assert_contains(reply.body, "hx-trigger=\"every 15s\"");
	assert_bar(reply.body, HALF, "100", "50");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/conversions/latest");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<html");
	assert_contains(reply.body, "id=\"conversions-latest\"");
	assert_bar(reply.body, HALF, "100", "50");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/conversions/latest");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, status);
	assert_contains(reply.body, "Files being converted or checked now");
	assert_bar(reply.body, HALF, "100", "50");
	assert_bar(reply.body, "0 of 2 files settled (0%)", "2", "0");
	axis_test_reply_clear(&reply);

	/* All of the source read. */
	job_running(fx, conversion, 120, "1");
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, "Encoding: 100%, 5.0 times real time", "100", "100");
	assert_lacks(reply.body, " left");
	axis_test_reply_clear(&reply);

	/* Being checked, then being put aside: the stage in words. */
	sql(fx, "UPDATE job SET state = 'succeeded', lease_token = NULL, lease_expires_at = NULL, finished_at = now()"
		" WHERE id = (SELECT job_id FROM conversion WHERE id = %" G_GINT64_FORMAT ")", conversion);
	sql(fx, "UPDATE conversion SET state = 'verifying' WHERE id = %" G_GINT64_FORMAT, conversion);
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, "Checking: no progress reported yet", NULL, NULL);
	assert_contains(reply.body, "hx-trigger=\"every 5s\"");
	axis_test_reply_clear(&reply);
	sql(fx, "UPDATE conversion SET state = 'placing' WHERE id = %" G_GINT64_FORMAT, conversion);
	reply = get_as(fx, fx->admin, progress);
	assert_bar(reply.body, "Keeping the trial aside", NULL, NULL);
	axis_test_reply_clear(&reply);

	/* Ended: the block asks for nothing more, and points at the page. */
	sql(fx, "INSERT INTO conversion_check (conversion_id, number, result, detail) SELECT %" G_GINT64_FORMAT ", n,"
		" 'passed', 'as planned' FROM generate_series(1, 10) n", conversion);
	sql(fx, "UPDATE conversion SET state = 'done', finished_at = now(), output_size = src_size / 2,"
		" output_reported = src_size / 2, trial_name = 'trial-1.mkv', trial_state = 'kept' WHERE id = %" G_GINT64_FORMAT,
		conversion);
	reply = get_as(fx, fx->admin, progress);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"conversion-progress\"");
	assert_contains(reply.body, "The conversion is no longer in flight");
	assert_lacks(reply.body, "hx-get");
	assert_lacks(reply.body, "<progress");
	axis_test_reply_clear(&reply);
	/* Its page has no bar, and the table asks for nothing while no other
	 * conversion is in flight. */
	reply = page(fx, one, NULL);
	assert_lacks(reply.body, "<progress");
	assert_lacks(reply.body, "no longer in flight");
	axis_test_reply_clear(&reply);

	/* The run: one file settled of two, and what it saved against the
	 * estimate, as bars with their words. */
	sql(fx, "UPDATE conversion_run SET state = 'paused', pause_code = 'person', pause_text = 'Paused in a test.',"
		" paused_at = now(), paused_by = 'root' WHERE id = %" G_GINT64_FORMAT, run);
	step(fx);
	reply = get_as(fx, fx->admin, "/conversions/latest");
	assert_lacks(reply.body, "hx-get");
	assert_lacks(reply.body, "<progress");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, status);
	assert_bar(reply.body, "1 of 2 files settled (50%)", "2", "1");
	/* What it saved, of what was estimated for the whole list, as a percent. */
	{
		g_autofree gchar *saved = axis_size_format((guint64)N(fx, "SELECT sum(size - result_size) FROM"
			" conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'done'", run));
		g_autofree gchar *estimate = axis_size_format((guint64)N(fx, "SELECT sum(size - mid) FROM conversion_run_entry"
			" WHERE run_id = %" G_GINT64_FORMAT, run));
		g_autofree gchar *label = g_strdup_printf("%s saved of about %s estimated (as trials, nothing replaced)", saved,
			estimate);
		g_autofree gchar *percent = g_strdup_printf("%" G_GINT64_FORMAT, N(fx, "SELECT least((SELECT sum(size -"
			" result_size) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'done'),"
			" sum(size - mid)) * 100 / sum(size - mid) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run,
			run));

		assert_bar(reply.body, label, "100", percent);
	}
	axis_test_reply_clear(&reply);
	/* A cap of one file: the bar is of the files within it. */
	{
		AxisConversionRunLimits limits;

		memset(&limits, 0, sizeof(limits));
		limits.at_once = 1;
		limits.cap_files = 1;
		limits.window_start = limits.window_end = -1;
		g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "root", &error));
		g_assert_no_error(error);
	}
	reply = get_as(fx, fx->admin, status);
	assert_bar(reply.body, "1 of 1 files within its caps settled (100%)", "1", "1");
	axis_test_reply_clear(&reply);
	/* The dashboard's piece of the run carries the same bar. */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_bar(reply.body, "1 of 1 files within its caps settled (100%)", "1", "1");
	axis_test_reply_clear(&reply);
}

/*
 * Spec (conversion-runs, Kept files added to a run): on a run that replaces
 * originals, "Add these N files to the run" says that their N originals are
 * replaced by their converted files once every check passes, with the
 * sentence and the box of the switch to replacing; a form without the tick
 * is refused and nothing changes; with it the files are added, each with
 * who confirmed it, and the run waits, paused, its Resume saying how many
 * originals it will replace.  On a run of trials the row says that they are
 * added as trials, with no box.
 */
static void
test_kept_replacing(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *trials_path = NULL;
	AxisTestReply reply;
	gint64 run;
	gint64 trials;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	keep_movie(fx, "Lantern Hill (2001)");
	keep_movie(fx, "Glass Orchard (2003)");
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	form = g_strdup_printf("scope=movies&revision=%" G_GINT64_FORMAT, fx->space_saver);

	/* A run that replaces originals, paused after its one file. */
	run = plan(fx, form);
	list(fx, run);
	g_assert_true(axis_conversion_runs_start(fx->runs, run, AXIS_CONVERSION_REPLACE, TRUE, "root", &error));
	g_assert_no_error(error);
	step(fx);
	g_assert_true(axis_conversion_runs_pause(fx->runs, run, "root", &error));
	g_assert_no_error(error);
	sql(fx, "INSERT INTO conversion_check (conversion_id, number, result, detail) SELECT c.id, n, 'passed', 'as planned'"
		" FROM conversion c, generate_series(1, 10) n WHERE c.run_id = %" G_GINT64_FORMAT, run);
	sql(fx, "UPDATE conversion SET state = 'done', finished_at = now(), output_size = src_size / 2,"
		" output_reported = src_size / 2 WHERE run_id = %" G_GINT64_FORMAT, run);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || outcome FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"paused replace");

	path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run);
	action = g_strdup_printf("%s/include-kept", path);
	reply = page(fx, path, "run-kept-replacing");
	assert_contains(reply.body, "This run replaces originals. If you add these 2 files, their 2 originals are replaced "
		"by their converted files once every check passes. They stay kept, so Axis still does not replace them with "
		"another release. The run waits, paused, until you resume it.");
	assert_contains(reply.body, "For each file: The original will be deleted after the new file has passed every check");
	assert_contains(reply.body, "I understand: the originals will be deleted");
	assert_contains(reply.body, "name=\"confirm\"");
	assert_contains(reply.body, "Add these 2 files to the run");
	assert_contains(reply.body, "Resume the run, replacing up to 0 originals");
	assert_lacks(reply.body, "They are added as trials");
	axis_test_reply_clear(&reply);

	/* Without the tick (a form forged without it, or anything but the
	 * tick): refused, and nothing changes. */
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "This run replaces originals");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "confirm=on");
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text || ' ' || coalesce(relist_state, '-')"
		" FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "paused false -");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);

	/* With it: added, each with who confirmed it; the run waits. */
	reply = post_as(fx, fx->admin, action, "confirm=yes");
	done = g_strdup_printf("%s?done=kept&at=run-left-out#run-left-out", path);
	expect_redirect(&reply, done);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || relist_state || ' ' || convert_kept_by FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, run), ==, "listing paused root");
	list(fx, run);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND confirmed_by = 'root' AND confirmed_at IS NOT NULL AND state = 'waiting'", run), ==, 2);
	reply = page(fx, path, "run-kept-replacing-added");
	assert_contains(reply.body, "Resume the run, replacing up to 2 originals");
	assert_contains(reply.body, "The run replaces originals: resuming it replaces up to 2 originals, the files of the "
		"list that wait within its caps, each once every check passes.");
	assert_contains(reply.body, "Paused by root.");
	assert_lacks(reply.body, "Add these");
	axis_test_reply_clear(&reply);
	step(fx);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);

	/* A run of trials, paused with nothing in flight (its version is proven
	 * already, so it pauses itself for the switch to replacing): added as
	 * trials. */
	sql(fx, "DELETE FROM conversion_memory");
	trials = plan(fx, form);
	g_assert_cmpint(trials, !=, run);
	list(fx, trials);
	g_assert_true(axis_conversion_runs_start(fx->runs, trials, AXIS_CONVERSION_TRIAL, FALSE, "root", &error));
	g_assert_no_error(error);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || outcome FROM conversion_run WHERE id = %" G_GINT64_FORMAT, trials), ==,
		"paused trial");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'asked'", trials), ==, 0);
	trials_path = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, trials);
	reply = page(fx, trials_path, "run-kept-trials");
	assert_contains(reply.body, "They are added as trials: each is converted and checked, and the converted file is kept "
		"aside to be played. The library is not touched. The run waits, paused, until you resume it.");
	assert_contains(reply.body, "Add these 2 files to the run");
	assert_lacks(reply.body, "This run replaces originals");
	assert_lacks(reply.body, "replacing up to");
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/kept", Fixture, NULL, test_kept);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/kept-single", Fixture, NULL, test_kept_single);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/kept-replacing", Fixture, NULL, test_kept_replacing);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/progress", Fixture, NULL, test_progress);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/pages", Fixture, NULL, test_pages);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/actions", Fixture, NULL, test_actions);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/back-to-place", Fixture, NULL, test_back_to_place);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/series", Fixture, NULL, test_series_page);
	AXIS_TEST_ADD_DB_CASE("/web/conversion-runs/scale", Fixture, NULL, test_scale);
	g_test_add_func("/web/conversion-runs/progress-figures", test_progress_figures);
	return g_test_run();
}
