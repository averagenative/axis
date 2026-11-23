/*
 * test-web-conversions.c - the Conversions pages: the card on a file's
 * page, the confirmation of a trial and of a replacement with every gate,
 * the request (administrators only, with the CSRF token, a replacement only
 * with its box ticked), a conversion's page with its nine checks (passed,
 * and failed with the later ones not run), the list, removing a trial,
 * cancelling, and proving a revision by hand from its confirmation
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell and the storage, library, probe, workers and conversions
 * modules on an ephemeral loopback port; the worker API of
 * tests/axis-test-worker.h on another, played by the test with ffmpeg's
 * software encoders on a generated clip.  With AXIS_TEST_CONVERT_PAGES set
 * to a directory, the pages are saved there (for screenshots).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "storage/axis-volume-write.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"
#include "web/axis-password.h"
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

static gboolean have_ffmpeg;
static gchar *clips;

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
	AxisTestDaemon     *d;
	gchar              *token;
	gchar              *disk;
	gchar              *uuid;
	gchar              *scratch;
	gint64              volume;
	gint64              movies;
	guint               n_movies;
} Fixture;

#define SKIP_UNLESS_READY(fx) do { \
	if (!have_ffmpeg) { g_test_skip("ffmpeg is not installed"); return; } \
	if ((fx)->db == NULL) return; \
} while (0)

static gboolean
run(const gchar *const *argv)
{
	gint status = 0;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
	                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL))
		return FALSE;
	return g_spawn_check_wait_status(status, NULL);
}

static AxisCompatVerdict
always_safe(
	gpointer     data,
	gint64       root_id,
	const gchar *slug
){
	return AXIS_VERDICT_SAFE;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisLibraryTuning tuning;
	AxisConversionTuning convert;
	AxisSampleTuning sample;
	AxisWorkerApiTiming timing;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "conversions: *");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	if (!have_ffmpeg)
		return;

	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
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
	fx->scratch = g_build_filename(fx->dir, "scratch", NULL);
	axis_test_storage_mkdir(fx->scratch);
	memset(&sample, 0, sizeof(sample));
	sample.scratch_parent = fx->scratch;
	axis_estimator_set_sample_tuning(fx->estimator, &sample);
	fx->conv = axis_conversions_new(fx->pool, fx->settings, fx->storage, fx->probe, fx->estimator);
	axis_conversions_set_verdict(fx->conv, always_safe, NULL);
	memset(&convert, 0, sizeof(convert));
	convert.software_preset = "fast";
	convert.threads = 2;
	convert.tool_timeout_ms = 120000;
	convert.similarity_points = 2;
	convert.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &convert);

	fx->tokens = axis_worker_tokens_new();
	fx->kinds = axis_job_registry_new();
	g_assert_true(axis_worker_api_register_kinds(fx->kinds, NULL, &error));
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->workers_ui = axis_web_workers_new(fx->pool, fx->tokens, fx->estimator, 500);
	axis_web_workers_set_kinds(fx->workers_ui, fx->kinds);
	fx->conversions_ui = axis_web_conversions_new(fx->conv);
	axis_web_probe_set_estimator(fx->probe_ui, fx->estimator);
	axis_web_probe_set_file_card(fx->probe_ui, axis_web_conversions_file_collect, axis_web_conversions_file_render,
		axis_web_conversions_file_free, fx->conversions_ui);

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
	fx->volume = axis_test_library_add_volume(&fx->view, fx->disk, "Disk");
	fx->uuid = axis_test_library_q(&fx->view, "SELECT uuid::text FROM volume WHERE id = %" G_GINT64_FORMAT,
		fx->volume);
	fx->movies = axis_test_library_add_root(&fx->view, fx->volume, fx->disk, "Movies", AXIS_MEDIA_MOVIES);

	memset(&timing, 0, sizeof(timing));
	timing.lease_ms = 60000;
	timing.heartbeat_ms = 500;
	timing.poll_ms = 200;
	timing.reap_ms = 50;
	timing.backoff_base_ms = 50;
	timing.backoff_cap_ms = 100;
	fx->d = axis_test_daemon_new(fx->pool, fx->estimator, &timing, NULL);
	axis_worker_api_set_convert(fx->d->api, axis_web_conversions_worker_hooks(), fx->conv);
	fx->token = axis_test_daemon_add_worker(fx->d, "garage", NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	if (fx->db == NULL) {
		if (fx->dir != NULL)
			axis_test_storage_dir_free(fx->dir);
		axis_test_log_reset();
		return;
	}
	axis_test_daemon_free(fx->d);
	axis_secret_free(fx->token);
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
	g_free(fx->uuid);
	g_free(fx->scratch);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static gint64
n(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_library_n(&fx->view, "%s", sql);
}

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
	AxisTestReply page = get_as(fx, fx->admin, "/conversions");
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

/* Saves a page for the screenshots, when asked to. */
static void
save(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_CONVERT_PAGES");
	g_autofree gchar *file = NULL;
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	file = g_strconcat(name, ".html", NULL);
	path = g_build_filename(dir, file, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

/* A page every administrator page of this module is: 200, an intro,
 * tables that declare their kind and columns. */
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

static gint64
revision_of(
	Fixture     *fx,
	const gchar *slug
){
	return axis_test_library_n(&fx->view, "SELECT current_revision_id FROM conversion_profile WHERE slug = '%s'", slug);
}

static void
scan(Fixture *fx)
{
	axis_scan_result_free(axis_test_library_scan_expect(&fx->view, fx->movies, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
}

/* A new movie from the clip, scanned and probed: its id. */
static gint64
add_movie(
	Fixture     *fx,
	const gchar *title
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *from = g_build_filename(clips, "movie.mkv", NULL);
	g_autofree gchar *rel = g_strdup_printf("%s/%s.mkv", title, title);
	g_autofree gchar *to = g_build_filename(fx->disk, "Movies", rel, NULL);
	g_autofree gchar *content = NULL;
	g_autofree gchar *folder = g_path_get_dirname(to);
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;
	gsize length = 0;
	gint64 id;

	g_assert_true(g_file_get_contents(from, &content, &length, NULL));
	axis_test_storage_mkdir(folder);
	g_assert_true(g_file_set_contents(to, content, (gssize)length, NULL));
	scan(fx);
	id = axis_test_scenario_file(&fx->view, fx->movies, rel);
	g_assert_true(axis_probe_file(fx->probe, id, NULL, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_PROBE_OUTCOME_STORED);
	return id;
}

/* ── Playing the worker ─────────────────────────────────────────── */

static AxisTestReply
api(
	Fixture     *fx,
	const gchar *name,
	const gchar *body
){
	return axis_test_daemon_post(fx->d, "POST", name, fx->token, NULL, body, NULL);
}

static void
do_register(Fixture *fx)
{
	const gchar *volumes[3];
	g_autofree gchar *json = NULL;
	AxisTestReply reply;

	volumes[0] = fx->uuid;
	volumes[1] = fx->disk;
	volumes[2] = NULL;
	json = axis_test_register_json(FALSE, volumes);
	reply = api(fx, "register", json);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
}

static AxisWorkerJob *
claim(Fixture *fx)
{
	static const gchar *const kinds[] = { AXIS_WORKER_KIND_CONVERT, NULL };
	const gchar *volumes[2];
	g_autofree gchar *json = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerJob *job = NULL;
	AxisWorkerClaim request;
	AxisTestReply reply;

	volumes[0] = fx->uuid;
	volumes[1] = NULL;
	memset(&request, 0, sizeof(request));
	request.kinds = (gchar **)kinds;
	request.volumes = (gchar **)volumes;
	json = axis_worker_claim_to_json(&request);
	reply = api(fx, "claim", json);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, &problem));
	axis_test_reply_clear(&reply);
	g_assert_nonnull(job);
	g_assert_nonnull(job->convert);
	return job;
}

/* Encodes as told (with @negate, another picture) and reports it. */
static void
encode(
	Fixture  *fx,
	gboolean  negate
){
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *src = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *json = NULL;
	AxisWorkerJob *job = claim(fx);
	AxisConvertResult result;
	AxisWorkerComplete complete;
	AxisTestReply reply;
	struct stat st;
	gint in;
	gint fd;
	guint i;

	src = g_build_filename(fx->disk, job->convert->root, job->convert->path, NULL);
	out = g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, job->convert->output, NULL);
	in = open(src, O_RDONLY | O_CLOEXEC);
	fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	g_assert_cmpint(in, >=, 0);
	g_assert_cmpint(fd, >=, 0);
	g_subprocess_launcher_take_fd(launcher, in, 3);
	g_subprocess_launcher_take_fd(launcher, fd, 4);
	g_ptr_array_add(argv, g_strdup("ffmpeg"));
	for (i = 0; job->convert->args[i] != NULL; i++) {
		if (negate && i > 0 && strcmp(job->convert->args[i - 1], "-filter:0") == 0)
			g_ptr_array_add(argv, g_strconcat("negate,", job->convert->args[i], NULL));
		else
			g_ptr_array_add(argv, g_strdup(job->convert->args[i]));
	}
	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, NULL);
	g_assert_nonnull(process);
	g_assert_true(g_subprocess_wait_check(process, NULL, NULL));
	g_assert_cmpint(stat(out, &st), ==, 0);

	result.output = job->convert->output;
	result.size = (gint64)st.st_size;
	memset(&complete, 0, sizeof(complete));
	complete.job = job->id;
	complete.lease_token = job->lease_token;
	complete.kind = (gchar *)AXIS_WORKER_KIND_CONVERT;
	complete.convert = &result;
	json = axis_worker_complete_to_json(&complete);
	reply = api(fx, "complete", json);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	axis_worker_job_free(job);
}

/* Asks for a conversion through the page; its id. */
static gint64
ask(
	Fixture     *fx,
	gint64       file,
	gint64       revision,
	const gchar *outcome,
	gboolean     confirm
){
	g_autofree gchar *form = g_strdup_printf("file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT "&outcome=%s%s",
		file, revision, outcome, confirm ? "&confirm=yes" : "");
	AxisTestReply reply = post_as(fx, fx->admin, "/conversions", form);
	const gchar *location;
	gint64 id;

	if (reply.status != 303)
		g_error("the request was answered %u:\n%s", reply.status, reply.body);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_true(g_str_has_prefix(location, "/conversions/"));
	g_assert_true(g_str_has_suffix(location, "?done=requested"));
	id = g_ascii_strtoll(location + strlen("/conversions/"), NULL, 10);
	axis_test_reply_clear(&reply);
	return id;
}

static void
finish(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_conversions_finish(fx->conv, id, NULL, &error));
	g_assert_no_error(error);
}

/* ── The pages ──────────────────────────────────────────────────── */

/* Spec: the file page offers a conversion to administrators; the
 * confirmation says what will happen, with every gate, before anything is
 * done; only an administrator with the CSRF token asks; a replacement only
 * with its box ticked. */
static void
test_request(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *file_page = NULL;
	g_autofree gchar *new_trial = NULL;
	g_autofree gchar *new_replace = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 file;
	gint64 space_saver;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	file = add_movie(fx, "First Movie (2001)");
	space_saver = revision_of(fx, "space_saver");

	/* The card on the file's page. */
	file_page = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, file);
	reply = get_as(fx, fx->admin, file_page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Convert this file");
	assert_contains(reply.body, "action=\"/conversions/new\"");
	assert_contains(reply.body, "Space saver (locked: trials only)");
	save("file", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, file_page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Convert this file");
	axis_test_reply_clear(&reply);

	/* The confirmation of a trial. */
	new_trial = g_strdup_printf("/conversions/new?file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT
		"&outcome=trial", file, space_saver);
	reply = page(fx, new_trial, "new-trial");
	assert_contains(reply.body, "Try a conversion");
	assert_contains(reply.body, "First Movie (2001).mkv");
	assert_contains(reply.body, "Locked: trials only");
	assert_contains(reply.body, "Start the trial");
	assert_contains(reply.body, "Storage may be written");
	assert_contains(reply.body, "libx265");
	assert_lacks(reply.body, fx->dir);
	axis_test_reply_clear(&reply);

	/* A replacement with a locked revision: the gate says why, and there
	 * is no form to send. */
	new_replace = g_strdup_printf("/conversions/new?file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT
		"&outcome=replace", file, space_saver);
	reply = page(fx, new_replace, NULL);
	assert_contains(reply.body, "This conversion cannot start now: The profile version is proven");
	assert_lacks(reply.body, "Replace the original</button>");
	axis_test_reply_clear(&reply);

	/* Proven: the confirmation says the original will be deleted. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *statement = g_strdup_printf("INSERT INTO conversion_profile_proof (revision_id, method)"
			" VALUES (%" G_GINT64_FORMAT ", 'canary')", space_saver);

		g_assert_true(axis_db_pool_exec(fx->pool, statement, 0, NULL, NULL, &error));
	}
	reply = page(fx, new_replace, "new-replace");
	assert_contains(reply.body, "Replace a file with its conversion");
	assert_contains(reply.body, "The original will be deleted after the new file has passed every check");
	assert_contains(reply.body, "cannot be recovered");
	assert_contains(reply.body, "I understand: the original will be deleted");
	assert_contains(reply.body, "Replace the original");
	axis_test_reply_clear(&reply);

	/* Not an administrator: nothing shown, nothing asked. */
	reply = get_as(fx, fx->viewer, new_trial);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT "&outcome=trial", file,
		space_saver);
	reply = post_as(fx, fx->viewer, "/conversions", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Without the CSRF token, even an administrator. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/conversions", fx->admin, NULL, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* A replacement without its box ticked. */
	{
		g_autofree gchar *unticked = g_strdup_printf("file=%" G_GINT64_FORMAT "&revision=%" G_GINT64_FORMAT
			"&outcome=replace", file, space_saver);

		reply = post_as(fx, fx->admin, "/conversions", unticked);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "tick the box");
		axis_test_reply_clear(&reply);
	}
	reply = post_as(fx, fx->admin, "/conversions", "file=x&revision=1&outcome=trial");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion"), ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode'"), ==, 0);

	/* Asked: the conversion's page; a second one is refused by a gate. */
	id = ask(fx, file, space_saver, "trial", FALSE);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode'"), ==, 1);
	reply = post_as(fx, fx->admin, "/conversions", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "in flight");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "?done=requested", id);

		reply = page(fx, path, NULL);
		assert_contains(reply.body, "Waiting for a worker");
		axis_test_reply_clear(&reply);
	}

	/* Cancelled from its page. */
	{
		g_autofree gchar *path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/cancel", id);

		reply = post_as(fx, fx->viewer, path, "");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, path, "");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE state = 'cancelled'"), ==, 1);
}

/* Spec: a conversion's page shows each check with what it found: all nine
 * passed for a trial kept aside, the failed one and the ones not run for a
 * rejected one; the list shows both and the revisions tried; proving a
 * revision by hand names its trials and needs the box ticked. */
static void
test_checks_and_proving(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *prove = NULL;
	g_autofree gchar *trials = NULL;
	AxisTestReply reply;
	gint64 space_saver;
	gint64 balanced;
	gint64 passed[3];
	gint64 rejected;
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	space_saver = revision_of(fx, "space_saver");
	balanced = revision_of(fx, "balanced");

	/* Three trials that pass, on three movies. */
	for (i = 0; i < 3; i++) {
		static const gchar *const titles[] = { "First Movie (2001)", "Second Movie (2002)", "Third Movie (2003)" };
		gint64 file = add_movie(fx, titles[i]);

		passed[i] = ask(fx, file, space_saver, "trial", FALSE);
		encode(fx, FALSE);
		finish(fx, passed[i]);
	}
	/* One with another picture, rejected by check 8. */
	{
		gint64 file = add_movie(fx, "Fourth Movie (2004)");

		rejected = ask(fx, file, balanced, "trial", FALSE);
		encode(fx, TRUE);
		finish(fx, rejected);
	}

	path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, passed[0]);
	reply = page(fx, path, "conversion-passed");
	assert_contains(reply.body, "Done");
	assert_contains(reply.body, "The converted file is the one asked for");
	assert_contains(reply.body, "The conditions still hold");
	assert_contains(reply.body, "SSIM");
	{
		g_autofree gchar *trial = g_strdup_printf("trial-%" G_GINT64_FORMAT ".mkv", passed[0]);

		assert_contains(reply.body, trial);
	}
	assert_lacks(reply.body, fx->dir);
	assert_lacks(reply.body, "Not run");
	axis_test_reply_clear(&reply);

	g_free(path);
	path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, rejected);
	reply = page(fx, path, "conversion-failed");
	assert_contains(reply.body, "Rejected by a check");
	assert_contains(reply.body, "The picture matches the original&#39;s");
	assert_contains(reply.body, "below 0.850");
	assert_contains(reply.body, "Not run");
	assert_lacks(reply.body, fx->dir);
	axis_test_reply_clear(&reply);

	reply = page(fx, "/conversions", "conversions");
	assert_contains(reply.body, "First Movie (2001).mkv");
	assert_contains(reply.body, "Fourth Movie (2004).mkv");
	assert_contains(reply.body, "Rejected by a check");
	assert_contains(reply.body, "Space saver");
	axis_test_reply_clear(&reply);

	/* Proving by hand: the confirmation names the trials. */
	prove = g_strdup_printf("/conversions/prove/%" G_GINT64_FORMAT, space_saver);
	reply = page(fx, prove, "prove");
	assert_contains(reply.body, "Prove a conversion profile version by hand");
	assert_contains(reply.body, "I played these 3 trial files");
	assert_contains(reply.body, "Second Movie (2002).mkv");
	axis_test_reply_clear(&reply);
	trials = g_strdup_printf("trials=%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT, passed[2],
		passed[1], passed[0]);
	reply = post_as(fx, fx->admin, prove, trials);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *two = g_strdup_printf("trials=%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "&confirm=yes",
			passed[2], passed[1]);

		reply = post_as(fx, fx->admin, prove, two);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_profile_proof"), ==, 0);
	{
		g_autofree gchar *ticked = g_strconcat(trials, "&confirm=yes", NULL);

		reply = post_as(fx, fx->viewer, prove, ticked);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, prove, ticked);
		g_assert_cmpuint(reply.status, ==, 303);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/conversions?done=proven");
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_profile_proof WHERE method = 'trials'"
		" AND recorded_by = 'root'"), ==, 1);
	/* The revision with a rejected trial is not proven by hand. */
	g_free(prove);
	prove = g_strdup_printf("/conversions/prove/%" G_GINT64_FORMAT, balanced);
	reply = page(fx, prove, NULL);
	assert_contains(reply.body, "rejected by a check of the converted file itself");
	assert_lacks(reply.body, "Prove this version");
	axis_test_reply_clear(&reply);

	/* Spec (conversion-execution, A rejected trial set aside; hunt finding
	 * H6): the rejected trial's page says it counts against proving and
	 * offers to set it aside, with a reason; an administrator does, and from
	 * then on it counts neither for nor against, and the pages say so. */
	g_assert_cmpint(n(fx, "SELECT coalesce(checks_version, -1) FROM conversion WHERE state = 'rejected'"), ==,
		AXIS_CONVERSION_CHECKS_VERSION);
	g_free(path);
	path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, rejected);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Whether this trial counts toward proving");
	assert_contains(reply.body, "Against proving the profile version");
	assert_contains(reply.body, "Set this trial aside");
	axis_test_reply_clear(&reply);

	/* Review item 3, the marks: a rejection by a check found wrongly strict
	 * since counts neither way, and a pass by a rule found too lenient since
	 * not towards proving; the pages say so, with the reason. */
	{
		static const AxisConversionMark marks[] = {
			{ 8, AXIS_CONVERSION_MARK_STRICT, 2, "it compared the wrong frames of clips cut short" },
			{ 5, AXIS_CONVERSION_MARK_LENIENT, 2, "it did not decode the last sound track" }
		};
		g_autofree gchar *passed_page = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, passed[1]);

		axis_conversion_marks_set_for_test(marks, G_N_ELEMENTS(marks));
		reply = page(fx, path, NULL);
		assert_contains(reply.body, "Neither for nor against proving the profile version: check 8 was found wrongly strict");
		assert_contains(reply.body, "it compared the wrong frames of clips cut short");
		assert_lacks(reply.body, "Set this trial aside");
		axis_test_reply_clear(&reply);
		reply = page(fx, passed_page, NULL);
		assert_contains(reply.body, "Not towards proving the profile version: check 5 was found too lenient");
		assert_contains(reply.body, "it did not decode the last sound track");
		axis_test_reply_clear(&reply);
		axis_conversion_marks_set_for_test(NULL, 0);
		reply = page(fx, passed_page, NULL);
		assert_lacks(reply.body, "Not towards proving the profile version");
		axis_test_reply_clear(&reply);
	}
	{
		g_autofree gchar *action = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/set-aside", rejected);

		reply = post_as(fx, fx->admin, action, "reason=");
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "Say why the trial is set aside");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->viewer, action, "reason=check+7+was+wrong");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE set_aside_at IS NOT NULL"), ==, 0);
		reply = post_as(fx, fx->admin, action, "reason=check+7+was+wrong+for+this+clip%2C+fixed+since");
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "done=set-aside");
		axis_test_reply_clear(&reply);
		/* Once only. */
		reply = post_as(fx, fx->admin, action, "reason=again");
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
		/* A trial that passed cannot be. */
		g_free(action);
		action = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/set-aside", passed[1]);
		reply = post_as(fx, fx->admin, action, "reason=no");
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE set_aside_by = 'root'"
		" AND set_aside_reason = 'check 7 was wrong for this clip, fixed since'"), ==, 1);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Set aside");
	assert_contains(reply.body, "By root");
	assert_contains(reply.body, "check 7 was wrong for this clip, fixed since");
	assert_contains(reply.body, "Neither for nor against proving the profile version");
	assert_lacks(reply.body, "Set this trial aside");
	axis_test_reply_clear(&reply);
	reply = page(fx, prove, NULL);
	assert_lacks(reply.body, "rejected by a check of the converted file itself");
	assert_contains(reply.body, "1 rejected trial of this version was set aside by an administrator");
	axis_test_reply_clear(&reply);
	reply = page(fx, "/conversions", NULL);
	assert_contains(reply.body, "1 trial set aside");
	axis_test_reply_clear(&reply);

	/* A trial's file removed from its page. */
	g_free(path);
	path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/remove-trial", passed[0]);
	reply = post_as(fx, fx->admin, path, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE trial_state = 'removed'"), ==, 1);
	{
		g_autofree gchar *kept = g_strdup_printf("trial-%" G_GINT64_FORMAT ".mkv", passed[0]);
		g_autofree gchar *file = g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, kept, NULL);

		g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
	}
	reply = get_as(fx, fx->admin, "/conversions/999999");
	g_assert_cmpuint(reply.status, ==, 404);
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
	g_autofree gchar *rejected_path = NULL;
	g_autofree gchar *trial_path = NULL;
	g_autofree gchar *cancel_path = NULL;
	AxisTestReply reply;
	gint64 space_saver;
	gint64 balanced;
	gint64 passed;
	gint64 rejected;
	gint64 queued;
	gint64 file;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	space_saver = revision_of(fx, "space_saver");
	balanced = revision_of(fx, "balanced");

	/* A trial that passes, and one rejected by check 8. */
	file = add_movie(fx, "First Movie (2001)");
	passed = ask(fx, file, space_saver, "trial", FALSE);
	encode(fx, FALSE);
	finish(fx, passed);
	file = add_movie(fx, "Second Movie (2002)");
	rejected = ask(fx, file, balanced, "trial", FALSE);
	encode(fx, TRUE);
	finish(fx, rejected);

	/* The rejected trial set aside: the revision card. */
	rejected_path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/set-aside", rejected);
	reply = post_as(fx, fx->admin, rejected_path, "reason=check+7+was+wrong");
	assert_back_to_place(fx, &reply, "conversion-aside");

	/* The trial's file removed: the trial card. */
	trial_path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/remove-trial", passed);
	reply = post_as(fx, fx->admin, trial_path, "");
	assert_back_to_place(fx, &reply, "conversion-trial");

	/* One waiting for a worker, cancelled: the card of what was asked. */
	file = add_movie(fx, "Third Movie (2003)");
	queued = ask(fx, file, space_saver, "trial", FALSE);
	cancel_path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/cancel", queued);
	reply = post_as(fx, fx->admin, cancel_path, "");
	assert_back_to_place(fx, &reply, "conversion-asked");

	/* Resumed after a start: the box of what holds conversion work back. */
	axis_conversions_hold_after_start(fx->conv);
	reply = post_as(fx, fx->admin, "/conversions/resume", "");
	assert_back_to_place(fx, &reply, "conversions-state");

	/* The workers' page: a sample measured by a worker, in its own card. */
	reply = post_as(fx, fx->admin, "/workers/sample", "target=space_saver");
	assert_back_to_place(fx, &reply, "worker-sample");
}

/* Spec (worker-protocol, A volume without a marker is not offered to
 * workers: The pages say why): the Conversions page names a volume that has
 * no marker yet and says that a worker recognises a disk only by its marker;
 * once the marker is there, it says nothing of it. */
static void
test_unmarked_volume_named(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	AxisTestReply page;

	if (fx->db == NULL)
		return;
	sql = g_strdup_printf("UPDATE volume SET identity = 'filesystem', marker_created_at = NULL, ident_magic = 16914836,"
		" ident_fsid = '03350eabfcdf1e9a', ident_ino = 2, ident_bytes = 2040373248 WHERE id = %" G_GINT64_FORMAT,
		fx->volume);
	axis_test_db_exec(fx->db, sql);
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/conversions");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Disk &#39;Disk&#39; has no marker yet");
	assert_contains(page.body, "A worker recognises a disk only by its marker");
	axis_test_reply_clear(&page);

	g_free(sql);
	sql = g_strdup_printf("UPDATE volume SET identity = 'marker', marker_created_at = now() WHERE id = %" G_GINT64_FORMAT,
		fx->volume);
	axis_test_db_exec(fx->db, sql);
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/conversions");
	g_assert_null(strstr(page.body, "has no marker yet"));
	axis_test_reply_clear(&page);
}

/* Spec (conversion-execution): the pages say where checks run, who
 * converted and who checked, and that conversion work is paused since the
 * daemon started, with one button that resumes it (administrators only). */
static void
test_paused_and_where(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *statement = NULL;
	AxisTestReply reply;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	file = add_movie(fx, "Fifth Movie (2005)");
	id = ask(fx, file, revision_of(fx, "space_saver"), "trial", FALSE);
	encode(fx, FALSE);
	path = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, id);

	reply = page(fx, "/conversions", NULL);
	assert_contains(reply.body, "Converted files are checked: On this server");
	assert_lacks(reply.body, "Resume conversion work");
	axis_test_reply_clear(&reply);

	/* After a start with convert.wait_after_start on (the default). */
	axis_conversions_hold_after_start(fx->conv);
	reply = page(fx, "/conversions", "conversions-paused");
	assert_contains(reply.body, "Conversion work has been paused since Axis started");
	assert_contains(reply.body, "Resume conversion work");
	axis_test_reply_clear(&reply);
	finish(fx, id);
	reply = page(fx, path, "conversion-paused");
	assert_contains(reply.body, "Being checked");
	assert_contains(reply.body, "Waiting: conversion work has been paused since Axis started");
	assert_contains(reply.body, "Resume conversion work");
	axis_test_reply_clear(&reply);

	/* Resumed: by an administrator only. */
	reply = post_as(fx, fx->viewer, "/conversions/resume", "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_conversions_is_held(fx->conv, NULL));
	reply = post_as(fx, fx->admin, "/conversions/resume", "");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==,
		"/conversions?done=resumed&at=conversions-state#conversions-state");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_conversions_is_held(fx->conv, NULL));
	reply = page(fx, "/conversions?done=resumed", NULL);
	assert_contains(reply.body, "Conversion work is resumed");
	assert_lacks(reply.body, "Conversion work is paused");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/conversions/resume", "");
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==,
		"/conversions?done=not-held&at=conversions-state#conversions-state");
	axis_test_reply_clear(&reply);

	/* Checked here, then the page says so. */
	finish(fx, id);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Done");
	assert_contains(reply.body, "Checked on");
	assert_contains(reply.body, "This server");
	assert_contains(reply.body, "Where checks run now");
	axis_test_reply_clear(&reply);

	/* As the record of one checked on a worker says it. */
	statement = g_strdup_printf("UPDATE conversion SET checked_where = 'worker', verifier = 'shed' WHERE id = %"
		G_GINT64_FORMAT, id);
	axis_test_db_exec(fx->db, statement);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Worker shed (it was converted on garage)");
	axis_test_reply_clear(&reply);
	g_free(statement);
	statement = g_strdup_printf("UPDATE conversion SET verifier = 'garage' WHERE id = %" G_GINT64_FORMAT, id);
	axis_test_db_exec(fx->db, statement);
	reply = page(fx, path, NULL);
	assert_contains(reply.body, "Worker garage, the worker that converted it");
	axis_test_reply_clear(&reply);

	/* The mode in plain words. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker_only", &error));
	g_assert_no_error(error);
	reply = page(fx, "/conversions", NULL);
	assert_contains(reply.body, "Only on a worker: this server does no heavy work");
	axis_test_reply_clear(&reply);
}

#define ADD(name, func) g_test_add("/web/conversions/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = axis_test_worker_seed_caps(FALSE);
	if (have_ffmpeg) {
		g_autofree gchar *movie = NULL;

		clips = axis_test_storage_dir_new();
		movie = g_build_filename(clips, "movie.mkv", NULL);
		{
			const gchar *argv_movie[] = {
				"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
				"testsrc2=size=320x180:rate=24:duration=4", "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
				"-vf", "setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv",
				"-c:v", "libx264", "-b:v", "4M", "-pix_fmt", "yuv420p", "-x264-params",
				"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv", "-c:a", "aac", "-b:a", "96k",
				"-metadata:s:a:0", "language=eng", movie, NULL
			};

			if (!run(argv_movie))
				g_error("the clip could not be made");
		}
	}
	ADD("request", test_request);
	ADD("checks-and-proving", test_checks_and_proving);
	ADD("paused-and-where", test_paused_and_where);
	ADD("back-to-place", test_back_to_place);
	ADD("unmarked-volume-named", test_unmarked_volume_named);
	status = g_test_run();
	axis_test_worker_cleanup();
	if (clips != NULL)
		axis_test_storage_dir_free(clips);
	return status;
}
