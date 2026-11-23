/*
 * test-web-attention.c - what needs a person (add-attention-notices, slice
 * N1): the bell in the header, the page "Needs attention", each module's
 * source, the waits, dismissal for each user, who sees what, what a page
 * view costs and the event told through the hub; with the real shell on an
 * ephemeral loopback port, a real database and a real volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks 6.1 to 6.5 of the change.  The states behind the notices are written
 * into the database as the modules write them (an import's state, a client's
 * state and since when), the disks are unplugged for real, and the clock of
 * the readings is moved (AxisAttentionTuning.clock_offset_us) to pass a wait.
 * A module of the test's own gives a notice the test controls: how many, how
 * serious, whether it fails.
 *
 * Every name here is made up.
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
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-tvdb.h"
#include "axis-test-web.h"
#include "arr/axis-arr.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-arr.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs-private.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"

#define PASSWORD   AXIS_TEST_DOWNLOAD_PASSWORD
#define KEY        "0123456789abcdef0123456789abcdef"

/* Nothing listens here: a contact fails at once. */
#define NOWHERE    "http://127.0.0.1:9"

#define MINUTES    (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

/* Since when a client or an indexer has not been working, in its row. */
#define NOT_WORKING_SINCE "not_working_since"

/* A fixture whose disk was found offline before the pages' objects were made. */
#define OFFLINE_BEFORE_START "offline-before-start"

/* The test's own module: one notice, as the test sets it. */
typedef struct
{
	gint  calls;	/* atomic */
	gint  fail;	/* atomic */
	gint  count;	/* atomic: 0 for no notice */
	gint  level;	/* atomic */
	gint  newest;	/* atomic */
	gint  not_known;	/* atomic: it answers "not known yet" (and gives no item) */
	gint  block;	/* atomic: it runs a statement that takes BLOCK_SECONDS, with no deadline */
	gint  entered;	/* atomic: how many times it began that statement */
	gint  answered;	/* atomic: how many times that statement was answered */
	gint  after;	/* atomic: how many times the source registered after it ran */
	AxisDbPool *pool;	/* for the statement; set before @block */
} Fake;

/* How long the statement of a blocked source takes: far longer than a stop
 * may. */
#define BLOCK_SECONDS 8

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
	AxisTestTvdb         *tvdb;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisMovies           *movies;
	AxisTv               *tv;
	AxisProbe            *probe;
	AxisImport           *import;
	AxisWebImports       *imports_ui;
	AxisArr              *arr;
	AxisWebArr           *arr_ui;
	AxisWebHub           *hub;
	AxisWebJobs          *jobs;
	AxisWebStorage       *storage_ui;
	Fake                  fake;
	AxisModuleRegistry   *registry;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;	/* user 1 */
	gchar                *viewer;	/* user 2, not an administrator */
	gchar                *ops;	/* user 3, a second administrator */
	gint64                clock_offset_us;
} Fixture;

/* ── The test's own module ──────────────────────────────────────── */

static gboolean
fake_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	Fake *fake = user_data;
	gint count = g_atomic_int_get(&fake->count);

	g_atomic_int_inc(&fake->calls);
	if (g_atomic_int_get(&fake->block)) {
		/* As a source whose database has gone silent in the middle of a
		 * statement that has no deadline on this side. */
		g_atomic_int_inc(&fake->entered);
		if (!axis_db_pool_exec(fake->pool, "SELECT pg_sleep(" G_STRINGIFY(BLOCK_SECONDS) ")", 0, NULL, NULL, error))
			return FALSE;
		g_atomic_int_inc(&fake->answered);
		return TRUE;
	}
	if (g_atomic_int_get(&fake->fail)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "made to fail");
		return FALSE;
	}
	if (g_atomic_int_get(&fake->not_known)) {
		axis_attention_not_known(items, "test.thing", NULL);
		return TRUE;
	}
	if (count > 0) {
		AxisAttentionItem *item = axis_attention_item_new("test.thing", "", (AxisAttentionLevel)g_atomic_int_get(
			&fake->level), (guint64)count, g_atomic_int_get(&fake->newest));
		g_autofree gchar *text = g_strdup_printf("%d test things wait at ", count);

		axis_attention_item_say(item, text, "Pier <Nine>", ".");
		axis_attention_item_link(item, "/test-things", "Open Test things");
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/* The source after the test's own: it has nothing to say, and counts. */
static gboolean
fake_source_after(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	Fake *fake = user_data;

	g_atomic_int_inc(&fake->after);
	return TRUE;
}

/* A page that reads nothing: its cost is the header's. */
static HtmxResponse *
fake_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	g_autoptr(HtmxDiv) content = htmx_div_new();

	htmx_node_set_text_content(HTMX_NODE(content), "A page of the test.");
	return axis_web_page_respond(request, "Test things", HTMX_NODE(content));
}

static void
fake_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_attention_source(web, "the test things", fake_source, user_data);
	axis_web_add_attention_source(web, "what comes after the test things", fake_source_after, user_data);
	axis_web_get(web, "/test-things", fake_page, user_data);
}

static const AxisModuleInfo *
fake_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = "attntest";
		info.name = "Attention test";
		info.register_routes = fake_routes;
	}
	return &info;
}

/* ── The fixture ────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;
	AxisDownloadClientSpec client;
	AxisAttentionTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Attic");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	/* The disk went away, and was found away, before this "daemon" (the
	 * pages' objects below) started. */
	if (g_strcmp0(data, OFFLINE_BEFORE_START) == 0) {
		axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
		axis_test_library_unplug(fx->t, fx->disk, "attic-away");
		g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
		g_assert_no_error(error);
		g_usleep(20 * 1000);
	}
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	/* TheTVDB at a stand-in: a provider whose key can be refused. */
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	g_assert_no_error(error);
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 2000;
	indexer_tuning.wait_ms = 2000;
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
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->downloads, fx->probe);
	axis_movies_set_import(fx->movies, fx->import);
	axis_tv_set_import(fx->tv, fx->import);
	fx->imports_ui = axis_web_imports_new(fx->import, fx->settings);
	fx->arr = axis_arr_new(fx->t->pool, fx->meta);
	fx->arr_ui = axis_web_arr_new(fx->arr);
	fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(fx->t->pool, fx->hub);
	fx->storage_ui = axis_web_storage_new(fx->t->storage, fx->hub);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(), fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), fx->tv, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_imports_get_module(), fx->imports_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_arr_get_module(), fx->arr_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, fake_module(), &fx->fake, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_no_error(error);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	/* (The storage module checks every disk when it starts.  The fixture
	 * whose disk went away before the start is of the moment before that
	 * check: its modules are not started, and the test checks when it wants
	 * to.) */
	if (g_strcmp0(data, OFFLINE_BEFORE_START) != 0)
		g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	/* The accounts of the sessions, with the ids the sessions have: a
	 * dismissal refers to its user. */
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (username, password_hash, role) VALUES"
		" ('root', '$argon2id$none', 'admin'), ('viewer', '$argon2id$none', 'user'), ('ops', '$argon2id$none', 'admin')");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT id FROM app_user WHERE username = 'ops'"), ==, 3);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->ops = axis_web_auth_open_test_session(fx->auth, "ops", AXIS_USER_ROLE_ADMIN);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);

	memset(&client, 0, sizeof(client));
	client.name = "Harbor";
	client.kind = "fixturenzb";
	client.address = NOWHERE;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &client, &fx->client_id, &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;

	/* A reading follows an event at once, and never by itself: every
	 * reading here is one the test asked for. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.min_gap_ms = 20;
	tuning.period_ms = 3600 * 1000;
	tuning.deadline_ms = 10000;
	axis_web_attention_set_tuning(fx->web, &tuning);
	fx->fake.level = AXIS_ATTENTION_WAITING;
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
	axis_web_arr_stop(fx->arr_ui);
	axis_web_jobs_stop(fx->jobs);
	axis_movies_stop(fx->movies);
	axis_tv_stop(fx->tv);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_web_imports_stop(fx->imports_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_storage_free(fx->storage_ui);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_arr_free(fx->arr_ui);
	axis_arr_unref(fx->arr);
	axis_web_imports_free(fx->imports_ui);
	axis_movies_unref(fx->movies);
	axis_tv_unref(fx->tv);
	axis_import_unref(fx->import);
	axis_probe_unref(fx->probe);
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
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->admin);
	g_free(fx->viewer);
	g_free(fx->ops);
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

/* A form posted from /attention as it is open for @cookie's user
 * (@with_token %FALSE: without the CSRF token). */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	gboolean     with_token,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	if (with_token) {
		AxisTestReply opened = get_as(fx, cookie, "/test-things");

		token = axis_test_web_csrf_token(opened.body);
		axis_test_reply_clear(&opened);
	}
	escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	body = g_strdup_printf("_csrf=%s&%s", escaped, form);
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
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
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	axis_test_db_exec(fx->t->db, text);
}

#define qi(fx, ...) axis_test_library_n((fx)->t, __VA_ARGS__)

static void
pump_until_reading(
	Fixture *fx,
	guint    before
){
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	while (axis_web_attention_get_readings(fx->web) == before) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_main_context_iteration(NULL, TRUE);
	}
}

/*
 * The board as it is after everything the test did so far: two readings are
 * asked for one after the other, so that the second began after this call
 * whatever was in flight before it.
 */
static void
read_board(Fixture *fx)
{
	guint round;

	for (round = 0; round < 2; round++) {
		guint before = axis_web_attention_get_readings(fx->web);

		axis_web_attention_changed(fx->web);
		pump_until_reading(fx, before);
	}
}

static void
start(Fixture *fx)
{
	axis_web_attention_start(fx->web, fx->t->pool);
	read_board(fx);
}

/* Moves the readings' clock to @minutes from now. */
static void
clock_at(
	Fixture *fx,
	gint     minutes
){
	AxisAttentionTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.clock_offset_us = minutes * MINUTES;
	axis_web_attention_set_tuning(fx->web, &tuning);
}

/* The bell of @cookie's user as its own request gives it (the body; "" for
 * a refusal). */
static gchar *
bell_of(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply reply = get_as(fx, cookie, "/attention/bell");
	gchar *body = g_strdup(reply.status == 200 ? reply.body : "");

	axis_test_reply_clear(&reply);
	return body;
}

static gchar *
page_of(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply reply = get_as(fx, cookie, "/attention");
	gchar *body;

	g_assert_cmpuint(reply.status, ==, 200);
	body = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	return body;
}

static void
set_fake(
	Fixture *fx,
	gint     count,
	gint     level,
	gint     newest
){
	g_atomic_int_set(&fx->fake.count, count);
	g_atomic_int_set(&fx->fake.level, level);
	g_atomic_int_set(&fx->fake.newest, newest);
}

/* A movie Axis manages, with its last search's outcome (%NULL: none) and
 * its destination (or none).  Returns its id. */
static gint64
add_movie(
	Fixture     *fx,
	const gchar *title,
	const gchar *outcome,
	gboolean     with_root,
	const gchar *hold
){
	g_autofree gchar *root = with_root ? g_strdup_printf("%" G_GINT64_FORMAT, fx->root_id) : g_strdup("NULL");
	g_autofree gchar *held = hold != NULL ? g_strdup_printf("'%s'", hold) : g_strdup("NULL");
	gint64 title_id = qi(fx, "INSERT INTO meta_title (kind, title, year, status, release_date, provider, provider_id)"
		" VALUES ('film', '%s', 2001, 'released', DATE '2001-03-04', 'fixture', 'm' || nextval('meta_title_id_seq'))"
		" RETURNING id", title);
	gint64 id = qi(fx, "INSERT INTO movie (title_id, quality_profile_id, root_id, search_hold) VALUES (%" G_GINT64_FORMAT
		", (SELECT id FROM quality_profile WHERE slug = 'balanced'), %s, %s) RETURNING id", title_id, root, held);

	if (outcome != NULL)
		sql(fx, "INSERT INTO movie_search (movie_id, cause, finished_at, outcome, outcome_text) VALUES (%"
			G_GINT64_FORMAT ", 'scheduled', now(), '%s', 'As the test made it.')", id, outcome);
	return id;
}

/* An indexer in @state, in it since @since_minutes ago, last contacted
 * @checked_minutes ago.  Returns its id. */
static gint64
add_indexer(
	Fixture     *fx,
	const gchar *name,
	const gchar *state,
	gint         since_minutes,
	gint         checked_minutes
){
	g_autoptr(GError) error = NULL;
	AxisIndexerSourceSpec source;
	gint64 id = 0;

	memset(&source, 0, sizeof(source));
	source.name = name;
	source.kind = AXIS_INDEXER_PROWLARR_NAME;
	source.address = NOWHERE;
	source.enabled = TRUE;
	source.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &source, &id, &error));
	g_assert_no_error(error);
	sql(fx, "UPDATE indexer_source SET state = '%s', " NOT_WORKING_SINCE " = now() - interval '%d minutes',"
		" last_checked_at = now() - interval '%d minutes' WHERE id = %" G_GINT64_FORMAT, state, since_minutes,
		checked_minutes, id);
	return id;
}

/* ── The bell and the page with nothing waiting ─────────────────── */

/* Spec (web-shell): Nothing waits; The page is in no tab.  (attention):
 * Nothing waits. */
static void
test_nothing_waits(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	g_autofree gchar *page = NULL;
	const gchar *search;
	const gchar *bell;
	const gchar *widget;

	start(fx);
	reply = get_as(fx, fx->admin, "/test-things");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<a id=\"attention-bell\"");
	assert_contains(reply.body, "href=\"/attention\"");
	assert_contains(reply.body, "aria-label=\"Nothing needs attention\"");
	assert_contains(reply.body, "attn-bell-none");
	assert_lacks(reply.body, "attn-bell-badge");
	/* To the right of the search box's place, before the running-jobs
	 * indicator and the user's corner. */
	search = strstr(reply.body, "class=\"search-slot\"");
	bell = strstr(reply.body, "id=\"attention-bell\"");
	widget = strstr(reply.body, "class=\"user-menu\"");
	g_assert_nonnull(search);
	g_assert_nonnull(bell);
	g_assert_nonnull(widget);
	g_assert_true(search < bell && bell < widget);
	/* It works without script: a plain link.  With script, it asks for
	 * itself when the page is told. */
	assert_contains(reply.body, "hx-get=\"/attention/bell\"");
	assert_contains(reply.body, "hx-trigger=\"sse:attention\"");
	assert_contains(reply.body, "topic=attention");
	assert_lacks(reply.body, "style=\"");
	axis_test_reply_clear(&reply);

	page = page_of(fx, fx->admin);
	assert_contains(page, "<h1>Needs attention</h1>");
	assert_contains(page, "Nothing needs attention.");
	assert_lacks(page, "attn-row");
	/* No primary tab is marked; the bell is. */
	assert_contains(page, "aria-current=\"page\"");
	assert_lacks(page, "class=\"current\"");
	g_assert_nonnull(strstr(strstr(page, "id=\"attention-bell\""), "aria-current=\"page\""));
}

/* ── The sources ────────────────────────────────────────────────── */

/* Spec (attention): A disk; A short outage; Mends itself; Dismissed for one
 * user's "It happens again". */
static void
test_disk_offline(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *short_outage = NULL;
	g_autofree gchar *long_outage = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);

	/* Three minutes: Axis is still trying. */
	clock_at(fx, 3);
	read_board(fx);
	short_outage = page_of(fx, fx->admin);
	assert_contains(short_outage, "Nothing needs attention.");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);

	/* Eleven: a notice, under "Not working", naming the disk and not its
	 * path, linking to the disk's page. */
	clock_at(fx, 11);
	read_board(fx);
	long_outage = page_of(fx, fx->admin);
	assert_contains(long_outage, "id=\"attention-broken\"");
	assert_contains(long_outage, "<span>The disk </span><bdi>Attic</bdi><span> has been offline for 11 minutes.");
	assert_contains(long_outage, "the disk is not mounted");
	assert_lacks(long_outage, fx->disk);
	form = g_strdup_printf("<a href=\"/storage/volumes/%" G_GINT64_FORMAT "\">Open the disk</a>", fx->volume_id);
	assert_contains(long_outage, form);
	assert_contains(long_outage, "Since 20");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention, 1 not working\"");
	assert_contains(bell, "attn-bell-broken");
	assert_contains(bell, "attn-bell-badge");
	assert_contains(bell, ">1</span>");
	g_clear_pointer(&bell, g_free);

	/* Dismissed, then the disk comes back: the notice is gone without anyone
	 * touching it; and when it goes again, that is a new occurrence. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("kind=volume.offline&subject=%" G_GINT64_FORMAT "&severity=3&count=1&newest=%"
		G_GINT64_FORMAT, fx->volume_id, qi(fx, "SELECT extract(epoch FROM unusable_since)::bigint FROM volume"
		" WHERE id = %" G_GINT64_FORMAT, fx->volume_id));
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);

	g_usleep(1100 * 1000);
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	read_board(fx);
	back = page_of(fx, fx->admin);
	assert_contains(back, "Nothing needs attention.");
	assert_lacks(back, "Attic");
	assert_lacks(back, "Dismissed (");

	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	read_board(fx);
	again = page_of(fx, fx->admin);
	assert_contains(again, "<bdi>Attic</bdi>");
	assert_contains(again, "id=\"attention-broken\"");
	assert_lacks(again, "Dismissed (");
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
}

/* Spec (attention): Several of a kind; One of a kind; Dealt with on its own
 * page.  And the Imports page's badge of a partly imported import. */
static void
test_imports(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = NULL;
	g_autofree gchar *three = NULL;
	g_autofree gchar *mixed = NULL;
	g_autofree gchar *done = NULL;
	AxisTestReply imports;

	start(fx);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	read_board(fx);
	one = page_of(fx, fx->admin);
	assert_contains(one, "id=\"attention-waiting\"");
	assert_contains(one, "1 import is blocked: it needs a person to decide what to do with its files.");
	assert_contains(one, "<a href=\"/imports\">Open Imports</a>");
	/* A count names no title. */
	assert_lacks(one, "Lantern Bay");

	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Copper Kettle Lane (2019)', 'downloads/copper', 'blocked'),"
		" ('movies', 'Gullwing Ferry (2021)', 'downloads/gullwing', 'blocked')");
	read_board(fx);
	three = page_of(fx, fx->admin);
	assert_contains(three, "3 imports are blocked: each needs a person to decide what to do with its files.");

	sql(fx, "INSERT INTO import (module, title_label, source_path, state, finished_at) VALUES"
		" ('tv', 'The Morning Tide Report', 'downloads/tide', 'partial', now())");
	read_board(fx);
	mixed = page_of(fx, fx->admin);
	assert_contains(mixed, "3 imports are blocked and 1 is partly done: each needs a person");

	/* The Imports page says of the one partly done what it is, not
	 * "Waiting". */
	imports = get_as(fx, fx->admin, "/imports");
	g_assert_cmpuint(imports.status, ==, 200);
	assert_contains(imports.body, "Partly imported");
	assert_contains(imports.body, "The Morning Tide Report");
	axis_test_reply_clear(&imports);

	/* Decided on their own page: the notice is gone by itself. */
	sql(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE state IN ('blocked', 'partial')");
	read_board(fx);
	done = page_of(fx, fx->admin);
	assert_contains(done, "Nothing needs attention.");
	assert_lacks(done, "import");
}

/* Spec (attention): A refused key (a download client's); the wait of a
 * client that does not answer; a client switched off. */
static void
test_download_client(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *early = NULL;
	g_autofree gchar *late = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *off = NULL;
	g_autofree gchar *link = NULL;
	gint64 since;

	start(fx);
	/* The client's state is written with since when, and since when stays
	 * while the state does. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download_client WHERE not_working_since IS NOT NULL"), ==, 0);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download_client WHERE state = 'unreachable'"
		" AND not_working_since IS NOT NULL"), ==, 1);
	since = qi(fx, "SELECT (extract(epoch FROM not_working_since) * 1000000)::bigint FROM download_client");
	g_usleep(20 * 1000);
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, NULL, &error));
	g_assert_cmpint(qi(fx, "SELECT (extract(epoch FROM not_working_since) * 1000000)::bigint FROM download_client"), ==,
		since);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download_client WHERE last_checked_at > not_working_since"), ==, 1);

	/* Not answering for a moment, and for five minutes: Axis is trying. */
	read_board(fx);
	first = page_of(fx, fx->admin);
	assert_contains(first, "Nothing needs attention.");
	clock_at(fx, 5);
	read_board(fx);
	early = page_of(fx, fx->admin);
	assert_contains(early, "Nothing needs attention.");

	/* For half an hour: a notice. */
	clock_at(fx, 31);
	read_board(fx);
	late = page_of(fx, fx->admin);
	assert_contains(late, "id=\"attention-broken\"");
	assert_contains(late, "<span>The download client </span><bdi>Harbor</bdi><span> has not answered for 31 minutes."
		"</span>");
	link = g_strdup_printf("<a href=\"/downloads/clients/%" G_GINT64_FORMAT "\">Open the download client</a>",
		fx->client_id);
	assert_contains(late, link);

	/* Refused: at once, whatever the clock. */
	clock_at(fx, 0);
	sql(fx, "UPDATE download_client SET state = 'refused', not_working_since = now()");
	read_board(fx);
	refused = page_of(fx, fx->admin);
	assert_contains(refused, "<bdi>Harbor</bdi><span> refused the password or key Axis has for it.</span>");

	/* Switched off with nothing in progress: left out. */
	sql(fx, "UPDATE download_client SET enabled = false");
	read_board(fx);
	off = page_of(fx, fx->admin);
	assert_contains(off, "Nothing needs attention.");
}

/* Spec (attention): Order's indexer; A level can rise; An allowance used
 * up; the second contact; more than five as one. */
static void
test_indexers(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *look = NULL;
	g_autofree gchar *once = NULL;
	g_autofree gchar *twice = NULL;
	g_autofree gchar *none_works = NULL;
	g_autofree gchar *many = NULL;
	g_autofree gchar *link = NULL;
	gint64 works;
	gint64 refused;
	gint64 quiet;
	guint i;

	start(fx);
	works = add_indexer(fx, "Driftwood", "ok", 600, 1);
	refused = add_indexer(fx, "Lanternfish", "refused", 0, 0);
	/* Found not answering 40 minutes ago by one contact, and not contacted
	 * since: one contact's bad luck is no notice. */
	quiet = add_indexer(fx, "Saltmarsh", "unreachable", 40, 40);
	/* Asked to wait, its allowance used up: it works, and that ends by
	 * itself. */
	sql(fx, "UPDATE indexer_source SET held_until = now() + interval '1 hour', hold_message = 'Asked to wait.',"
		" api_used = 100, api_max = 100, limits_at = now() WHERE id = %" G_GINT64_FORMAT, works);
	read_board(fx);
	look = page_of(fx, fx->admin);
	assert_contains(look, "id=\"attention-look\"");
	assert_lacks(look, "id=\"attention-broken\"");
	assert_contains(look, "<span>The indexer </span><bdi>Lanternfish</bdi><span> refused its API key.</span>");
	link = g_strdup_printf("<a href=\"/indexers/%" G_GINT64_FORMAT "\">Open the indexer</a>", refused);
	assert_contains(look, link);
	assert_lacks(look, "Saltmarsh");
	assert_lacks(look, "Driftwood");
	once = g_steal_pointer(&look);

	/* A second contact found it so: a notice. */
	sql(fx, "UPDATE indexer_source SET last_checked_at = now() - interval '2 minutes' WHERE id = %" G_GINT64_FORMAT,
		quiet);
	read_board(fx);
	twice = page_of(fx, fx->admin);
	assert_contains(twice, "<bdi>Saltmarsh</bdi><span> has not answered for 40 minutes.</span>");
	assert_contains(twice, "id=\"attention-look\"");

	/* The one that worked fails as well: every indexer notice is "Not
	 * working". */
	sql(fx, "UPDATE indexer_source SET state = 'failing', not_working_since = now() - interval '45 minutes',"
		" last_checked_at = now() WHERE id = %" G_GINT64_FORMAT, works);
	read_board(fx);
	none_works = page_of(fx, fx->admin);
	assert_contains(none_works, "id=\"attention-broken\"");
	assert_lacks(none_works, "id=\"attention-look\"");
	assert_contains(none_works, "<bdi>Driftwood</bdi><span> has been failing for 45 minutes.</span>");

	/* More than five: one notice with their number. */
	for (i = 0; i < 4; i++) {
		g_autofree gchar *name = g_strdup_printf("Tidepool %u", i + 1);

		add_indexer(fx, name, "refused", 0, 0);
	}
	read_board(fx);
	many = page_of(fx, fx->admin);
	assert_contains(many, "7 indexers are not working, and none of the others answers");
	assert_contains(many, "<a href=\"/indexers\">Open Indexers</a>");
	assert_lacks(many, "Tidepool");

	/* The indexer's state is written with since when, and a second failed
	 * contact moves only the time of the last one. */
	sql(fx, "UPDATE indexer_source SET state = 'ok', not_working_since = NULL WHERE id = %" G_GINT64_FORMAT, works);
	axis_indexers_test_source(fx->indexers, works, NULL, &error);
	g_clear_error(&error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM indexer_source WHERE id = %" G_GINT64_FORMAT " AND state <> 'ok'"
		" AND not_working_since IS NOT NULL AND NOT last_checked_at > not_working_since", works), ==, 1);
	g_usleep(20 * 1000);
	axis_indexers_test_source(fx->indexers, works, NULL, &error);
	g_clear_error(&error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM indexer_source WHERE id = %" G_GINT64_FORMAT
		" AND last_checked_at > not_working_since", works), ==, 1);
}

/* Spec (attention): A consequence; the movies' count and its reasons; read
 * only after the module's jobs. */
static void
test_movies(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *none = NULL;
	g_autofree gchar *two = NULL;
	g_autofree gchar *still = NULL;
	g_autofree gchar *three = NULL;

	start(fx);
	/* Ten movies that cannot be searched for because no indexer is set up:
	 * a consequence, whose cause has its own notice. */
	{
		guint i;

		for (i = 0; i < 10; i++) {
			g_autofree gchar *title = g_strdup_printf("Quiet Harbour %u", i + 1);

			add_movie(fx, title, "no_indexer", TRUE, NULL);
		}
	}
	/* (Each count is at least five minutes after the one before.) */
	clock_at(fx, 6);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	none = page_of(fx, fx->admin);
	assert_contains(none, "Nothing needs attention.");

	add_movie(fx, "Lantern Bay", "exhausted", TRUE, NULL);
	add_movie(fx, "Copper Kettle Lane", "exhausted", TRUE, NULL);
	clock_at(fx, 12);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	two = page_of(fx, fx->admin);
	assert_contains(two, "id=\"attention-waiting\"");
	assert_contains(two, "2 movies need attention, each because every acceptable release failed.");
	assert_contains(two, "<a href=\"/films/wanted?state=attention\">Open Movies, Wanted</a>");
	assert_lacks(two, "Lantern Bay");
	assert_lacks(two, "Quiet Harbour");

	/* The count is the heaviest reading: it is not made again while no job
	 * that can change it ended (another module's search does not count)... */
	add_movie(fx, "Gullwing Ferry", NULL, FALSE, NULL);
	clock_at(fx, 18);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	still = page_of(fx, fx->admin);
	assert_contains(still, "2 movies need attention");

	/* ...and is made after one did: an import, here. */
	axis_web_attention_job_ended(fx->web, "import.run");
	read_board(fx);
	three = page_of(fx, fx->admin);
	assert_contains(three, "3 movies need attention, most often because every acceptable release failed.");
}

/* Spec (attention): the series' count; A module switched off. */
static void
test_series(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *off = NULL;
	g_autofree gchar *bell = NULL;
	gint64 title;
	gint64 series;

	start(fx);
	title = qi(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) VALUES"
		" ('series', 'The Morning Tide Report', 2019, 'ended', 'fixture', 's1') RETURNING id");
	/* No library folder is its destination: only a person ends that. */
	series = qi(fx, "INSERT INTO series (title_id, quality_profile_id) VALUES (%" G_GINT64_FORMAT ","
		" (SELECT id FROM quality_profile WHERE slug = 'balanced')) RETURNING id", title);
	sql(fx, "INSERT INTO series_season (series_id, number, monitored) VALUES (%" G_GINT64_FORMAT ", 1, true)", series);
	sql(fx, "INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored) VALUES"
		" (%" G_GINT64_FORMAT ", 1, 1, 'Low Water', DATE '2019-03-04', true),"
		" (%" G_GINT64_FORMAT ", 1, 2, 'The Ropes', DATE '2019-03-11', true)", series, series);
	clock_at(fx, 6);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	one = page_of(fx, fx->admin);
	assert_contains(one, "1 series needs attention because no library folder is set as the destination.");
	assert_contains(one, "<a href=\"/series/wanted?state=attention\">Open TV, Wanted</a>");
	assert_lacks(one, "Morning Tide");

	/* The series are gone through fifty at a time: 121 are three batches.
	 * The job that follows the downloads ends every minute and causes no
	 * count; a search does. */
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2019, 'ended', 'fixture', 'g' || i FROM generate_series(1, 120) i"
		" RETURNING id), s AS (INSERT INTO series (title_id, quality_profile_id) SELECT id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced') FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT id, 1, true FROM s RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT id, 1, 1, 'Low Water', DATE '2019-03-04', true FROM s");
	clock_at(fx, 12);
	axis_web_attention_job_ended(fx->web, "tv.follow");
	axis_web_attention_job_ended(fx->web, "tv.schedule");
	read_board(fx);
	g_clear_pointer(&one, g_free);
	one = page_of(fx, fx->admin);
	assert_contains(one, "1 series needs attention because");
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	g_clear_pointer(&one, g_free);
	one = page_of(fx, fx->admin);
	assert_contains(one, "121 series need attention, each because no library folder is set as the destination.");
	sql(fx, "DELETE FROM series WHERE id <> %" G_GINT64_FORMAT, series);
	clock_at(fx, 18);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	assert_contains(bell, "attn-bell-waiting");

	/* The module switched off: no notice about series, shown or counted,
	 * from the very next page view. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "tv", FALSE, &error));
	g_assert_no_error(error);
	off = page_of(fx, fx->admin);
	assert_contains(off, "Nothing needs attention.");
	g_clear_pointer(&bell, g_free);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	read_board(fx);
	g_clear_pointer(&off, g_free);
	off = page_of(fx, fx->admin);
	assert_contains(off, "Nothing needs attention.");
}

/* A file of the library's index, present, as a scan would have left it.
 * Returns its id. */
static gint64
add_library_file(
	Fixture *fx,
	guint    n
){
	return qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'Brought %u/Brought %u.mkv', 'video', 2147483648, 1, 0, 1, 0, %u, 1, false, false, false, 1, 1, 1)"
		" RETURNING id", fx->root_id, fx->volume_id, n, n, 4200000 + n);
}

/* A movie a connection migrated, held as brought.  @with_file: with the
 * file the run linked, kept as a migrated file is.  Returns its id. */
static gint64
add_brought_movie(
	Fixture  *fx,
	gint64    source,
	guint     n,
	gboolean  with_file
){
	g_autofree gchar *title = g_strdup_printf("Brought Film %u", n);
	gint64 movie = add_movie(fx, title, NULL, TRUE, "brought");

	sql(fx, "INSERT INTO arr_brought (source_id, title_id, movie_id, label) SELECT %" G_GINT64_FORMAT
		", title_id, id, 'Brought Film' FROM movie WHERE id = %" G_GINT64_FORMAT, source, movie);
	if (with_file)
		sql(fx, "INSERT INTO movie_file (file_id, movie_id, hand_how, hand_by, hand_at, kept, kept_how) VALUES (%"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'brought', 'root', now(), true, 'brought')",
			add_library_file(fx, n), movie);
	return movie;
}

/* A series a connection migrated, held as brought, with one aired episode.
 * @monitored: the episode is.  @with_file: it has the file the run linked. */
static void
add_brought_series(
	Fixture  *fx,
	gint64    source,
	guint     n,
	gboolean  monitored,
	gboolean  with_file
){
	gint64 title = qi(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) VALUES"
		" ('series', 'Brought Show %u', 2019, 'ended', 'fixture', 'bs%u') RETURNING id", n, n);
	gint64 series = qi(fx, "INSERT INTO series (title_id, quality_profile_id, search_hold) VALUES (%" G_GINT64_FORMAT ","
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), 'brought') RETURNING id", title);
	gint64 episode;

	sql(fx, "INSERT INTO series_season (series_id, number, monitored) VALUES (%" G_GINT64_FORMAT ", 1, true)", series);
	episode = qi(fx, "INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored) VALUES"
		" (%" G_GINT64_FORMAT ", 1, 1, 'Low Water', DATE '2019-03-04', %s) RETURNING id", series,
		monitored ? "true" : "false");
	sql(fx, "INSERT INTO arr_brought (source_id, title_id, series_id, label) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'Brought Show')", source, title, series);
	if (with_file) {
		gint64 file = add_library_file(fx, 500 + n);

		sql(fx, "INSERT INTO series_file (file_id, series_id, hand_how, hand_by, hand_at, kept, kept_how) VALUES (%"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'brought', 'root', now(), true, 'brought')", file, series);
		sql(fx, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
			")", file, episode);
	}
}

/* Spec (attention): A migration that waits; Migrated titles that have their
 * files.  Only a held title that "Start searching" would have Axis search
 * for is counted: a monitored movie without a file, a series with a
 * monitored episode without one. */
static void
test_migration(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *kept = NULL;
	g_autofree gchar *waits = NULL;
	g_autofree gchar *started = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *link_plain = NULL;
	g_autofree gchar *link_kept = NULL;
	g_autofree gchar *link_tv = NULL;
	gint64 source;
	gint64 plain;
	gint64 all_kept;
	gint64 tv;
	gint64 movie;
	guint i;

	start(fx);
	/* A migration whose every movie has its file, kept: nothing waits, so
	 * there is no notice at all. */
	all_kept = qi(fx, "INSERT INTO arr_source (app, name, origin) VALUES ('radarr', 'Kept shelf', 'http://kept.test')"
		" RETURNING id");
	for (i = 0; i < 3; i++)
		add_brought_movie(fx, all_kept, 100 + i, TRUE);
	read_board(fx);
	kept = page_of(fx, fx->admin);
	assert_lacks(kept, "migrated from");
	assert_lacks(kept, "Open the migration");

	source = qi(fx, "INSERT INTO arr_source (app, name, origin) VALUES ('radarr', 'Old shelf', 'http://radarr.test')"
		" RETURNING id");
	for (i = 0; i < 12; i++) {
		/* Three have the file that was migrated, kept: starting searching
		 * changes nothing for them. */
		movie = add_brought_movie(fx, source, i + 1, i >= 1 && i <= 3);
		/* One is held because the other app has a file Axis does not see:
		 * "Start searching" does not end that. */
		if (i == 0)
			sql(fx, "UPDATE movie SET search_hold = 'file_unseen' WHERE id = %" G_GINT64_FORMAT, movie);
		/* One has no file and is not monitored: nothing searches for it,
		 * held or not. */
		if (i == 4)
			sql(fx, "UPDATE movie SET monitored = false WHERE id = %" G_GINT64_FORMAT, movie);
	}
	/* A connection named as its app is: the notice says the app once. */
	plain = qi(fx, "INSERT INTO arr_source (app, name, origin) VALUES ('radarr', 'radarr', 'http://plain.test')"
		" RETURNING id");
	add_brought_movie(fx, plain, 200, FALSE);
	add_brought_movie(fx, plain, 201, TRUE);
	/* Series: one with a monitored episode without a file; one whose episode
	 * has its file; one whose episode without a file is not monitored. */
	tv = qi(fx, "INSERT INTO arr_source (app, name, origin) VALUES ('sonarr', 'Sonarr', 'http://sonarr.test')"
		" RETURNING id");
	add_brought_series(fx, tv, 1, TRUE, FALSE);
	add_brought_series(fx, tv, 2, TRUE, TRUE);
	add_brought_series(fx, tv, 3, FALSE, FALSE);
	read_board(fx);
	waits = page_of(fx, fx->admin);
	assert_contains(waits, "id=\"attention-waiting\"");
	assert_contains(waits, "<span>7 movies migrated from Radarr (</span><bdi>Old shelf</bdi><span>) have no file. Axis "
		"does not search for them until you start searching.</span>");
	link = g_strdup_printf("<a href=\"/migrate/%" G_GINT64_FORMAT "\">Open the migration</a>", source);
	assert_contains(waits, link);
	assert_contains(waits, "<span>1 movie migrated from Radarr has no file. Axis does not search for it until you start "
		"searching.</span>");
	link_plain = g_strdup_printf("<a href=\"/migrate/%" G_GINT64_FORMAT "\">Open the migration</a>", plain);
	assert_contains(waits, link_plain);
	assert_contains(waits, "<span>1 series migrated from Sonarr has monitored episodes without a file. Axis does not "
		"search for them until you start searching.</span>");
	link_tv = g_strdup_printf("<a href=\"/migrate/%" G_GINT64_FORMAT "\">Open the migration</a>", tv);
	assert_contains(waits, link_tv);
	link_kept = g_strdup_printf("<a href=\"/migrate/%" G_GINT64_FORMAT "\">Open the migration</a>", all_kept);
	assert_lacks(waits, link_kept);
	assert_lacks(waits, "wait for you to start searching");
	assert_lacks(waits, "Brought Film");
	assert_lacks(waits, "Brought Show");

	/* "Start searching" was selected. */
	sql(fx, "UPDATE movie SET search_hold = NULL WHERE search_hold = 'brought'");
	sql(fx, "UPDATE series SET search_hold = NULL WHERE search_hold = 'brought'");
	read_board(fx);
	started = page_of(fx, fx->admin);
	assert_lacks(started, "migrated from");
}

/* Spec (attention): a details provider that refused its key. */
static void
test_provider_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *csrf = NULL;
	const gchar *extra[3];
	AxisTestReply opened;
	AxisTestReply reply;

	start(fx);
	before = page_of(fx, fx->admin);
	assert_contains(before, "Nothing needs attention.");

	axis_test_tvdb_refuse(fx->tvdb, TRUE);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	opened = get_as(fx, fx->admin, "/metadata");
	csrf = axis_test_web_csrf_token(opened.body);
	axis_test_reply_clear(&opened);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/test-key", fx->admin, extra,
		"provider=tvdb");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	read_board(fx);
	refused = page_of(fx, fx->admin);
	assert_contains(refused, "id=\"attention-broken\"");
	assert_contains(refused, "TheTVDB refused the key Axis has");
	assert_contains(refused, "<a href=\"/metadata\">Open Movie and TV details</a>");
	assert_lacks(refused, AXIS_TEST_TVDB_KEY);
}

/* ── The levels ─────────────────────────────────────────────────── */

/* Spec (attention): Order.  (web-shell): Something waits; A large number. */
static void
test_levels_and_count(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = NULL;
	g_autofree gchar *bell = NULL;
	const gchar *broken;
	const gchar *waiting;
	const gchar *look;

	start(fx);
	set_fake(fx, 2, AXIS_ATTENTION_BROKEN, 5);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked'),"
		" ('movies', 'Copper Kettle Lane (2019)', 'downloads/copper', 'blocked')");
	add_indexer(fx, "Driftwood", "ok", 600, 1);
	add_indexer(fx, "Lanternfish", "refused", 0, 0);
	read_board(fx);

	page = page_of(fx, fx->admin);
	broken = strstr(page, "id=\"attention-broken\"");
	waiting = strstr(page, "id=\"attention-waiting\"");
	look = strstr(page, "id=\"attention-look\"");
	g_assert_nonnull(broken);
	g_assert_nonnull(waiting);
	g_assert_nonnull(look);
	g_assert_true(broken < waiting && waiting < look);
	assert_contains(page, "<h2>Not working</h2>");
	assert_contains(page, "<h2>Waiting for you</h2>");
	assert_contains(page, "<h2>Worth a look</h2>");
	/* A name is text in its own <bdi>: markup in it is escaped. */
	assert_contains(page, "<bdi>Pier &lt;Nine&gt;</bdi>");
	assert_lacks(page, "<Nine>");

	/* The number is of notices, not of the things they count. */
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"3 things need attention, 1 not working\"");
	assert_contains(bell, ">3</span>");
}

/* ── Dismissal ──────────────────────────────────────────────────── */

/* Spec (attention): Dismissed for one user; More of them; Fewer of them;
 * Show again; a level that rises; it happens again. */
static void
test_dismiss(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = NULL;
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	page = page_of(fx, fx->admin);
	/* The form carries what the page shows, and the session's token. */
	assert_contains(page, "action=\"/attention/dismiss\"");
	assert_contains(page, "method=\"post\"");
	assert_contains(page, "name=\"_csrf\"");
	assert_contains(page, "<input type=\"hidden\" name=\"kind\" value=\"test.thing\"");
	assert_contains(page, "<input type=\"hidden\" name=\"count\" value=\"3\"");
	assert_contains(page, "<input type=\"hidden\" name=\"newest\" value=\"10\"");
	g_clear_pointer(&page, g_free);

	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=3&newest=10");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/attention");
	axis_test_reply_clear(&reply);

	/* Out of that user's count and into their dismissed list... */
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);
	page = page_of(fx, fx->admin);
	assert_contains(page, "Nothing needs attention.");
	assert_contains(page, "<summary>Dismissed (1)</summary>");
	assert_contains(page, "action=\"/attention/restore\"");
	assert_contains(page, "Show again");
	assert_contains(page, "3 test things wait at ");
	g_clear_pointer(&page, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal WHERE user_id = 1 AND kind = 'test.thing'"
		" AND subject = '' AND severity = 2 AND count = 3 AND newest = 10"), ==, 1);

	/* ...and the other administrator still sees and counts it. */
	bell = bell_of(fx, fx->ops);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_clear_pointer(&bell, g_free);
	page = page_of(fx, fx->ops);
	assert_contains(page, "id=\"attention-waiting\"");
	assert_lacks(page, "Dismissed (");
	g_clear_pointer(&page, g_free);

	/* It does not come back with time, nor with fewer. */
	clock_at(fx, 60 * 24 * 40);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);
	set_fake(fx, 2, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);

	/* More of them: back. */
	set_fake(fx, 4, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_clear_pointer(&bell, g_free);

	/* As dismissed again; then it happens again (a later "newest"): back. */
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 11);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_clear_pointer(&bell, g_free);

	/* More serious: back. */
	set_fake(fx, 3, AXIS_ATTENTION_BROKEN, 10);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention, 1 not working\"");
	g_clear_pointer(&bell, g_free);

	/* Show again: the row is gone and the notice is back in the list. */
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	reply = post_as(fx, fx->admin, TRUE, "/attention/restore", "kind=test.thing&subject=");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);
}

/* Spec (attention): A forged form.  And a dismissal is of what is there:
 * never of more than the board holds, never of a thing that is not on it. */
static void
test_dismiss_refusals(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad_forms[] = {
		"kind=Test.Thing&subject=&severity=2&count=3&newest=10",
		"kind=test.thing&subject=../../x&severity=2&count=3&newest=10",
		"kind=test.thing&subject=&severity=9&count=3&newest=10",
		"kind=test.thing&subject=&severity=2&count=-1&newest=10",
		"kind=test.thing&subject=&severity=2&count=3&newest=ten",
		"subject=&severity=2&count=3&newest=10"
	};
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;
	guint i;

	start(fx);
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);

	/* Without the session's CSRF token: refused, nothing dismissed. */
	reply = post_as(fx, fx->admin, FALSE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=3&newest=10");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Without a session at all. */
	reply = post_as(fx, NULL, FALSE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=3&newest=10");
	g_assert_cmpuint(reply.status, >=, 300);
	if (reply.status == 303)
		g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);
	for (i = 0; i < G_N_ELEMENTS(bad_forms); i++) {
		reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", bad_forms[i]);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	/* A thing that is not on the board: nothing to dismiss. */
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.other&subject=&severity=3&count=9&newest=9");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_clear_pointer(&bell, g_free);

	/* More than is there: what is stored is what is there, so one more
	 * brings it back. */
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss",
		"kind=test.thing&subject=&severity=3&count=999999&newest=999999");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal WHERE user_id = 1 AND severity = 2 AND count = 3"
		" AND newest = 10"), ==, 1);
	set_fake(fx, 4, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
}

/* What was dismissed is read from the database when the readings start: a
 * dismissal outlasts the daemon. */
static void
test_dismissals_are_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *mine = NULL;
	g_autofree gchar *theirs = NULL;

	sql(fx, "INSERT INTO attention_dismissal (user_id, kind, subject, severity, count, newest) VALUES"
		" (1, 'test.thing', '', 2, 3, 10)");
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	start(fx);
	mine = page_of(fx, fx->admin);
	assert_contains(mine, "Nothing needs attention.");
	assert_contains(mine, "<summary>Dismissed (1)</summary>");
	theirs = bell_of(fx, fx->ops);
	assert_contains(theirs, "aria-label=\"1 thing needs attention\"");

	/* A deleted account's dismissals go with it. */
	sql(fx, "DELETE FROM app_user WHERE id = 1");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);
}

/* ── Who sees what ──────────────────────────────────────────────── */

/* Spec (attention): An ordinary user; An administrator.  Every source has
 * a notice here, and a user who is not an administrator is shown none of
 * it, by any of the four routes or in any header. */
static void
test_ordinary_user(Fixture *fx, gconstpointer data)
{
	static const gchar *const secrets[] = {
		"Attic", "Harbor", "Lanternfish", "Old shelf", "Lantern Bay", "Pier", "test things", "is blocked", "refused",
		"offline", "migrated", "wait for you", "attention-bell", "attn-", "/attention"
	};
	static const gchar *const pages[] = { "/test-things", "/storage", "/imports", "/films", "/downloads" };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *admin_page = NULL;
	g_autofree gchar *admin_bell = NULL;
	AxisTestReply reply;
	gint64 source;
	gint64 movie;
	guint i;
	guint k;

	start(fx);
	set_fake(fx, 2, AXIS_ATTENTION_BROKEN, 5);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	add_indexer(fx, "Lanternfish", "refused", 0, 0);
	sql(fx, "UPDATE download_client SET state = 'refused', not_working_since = now()");
	add_movie(fx, "Lantern Bay", "exhausted", TRUE, NULL);
	source = qi(fx, "INSERT INTO arr_source (app, name, origin) VALUES ('radarr', 'Old shelf', 'http://radarr.test')"
		" RETURNING id");
	movie = add_movie(fx, "Copper Kettle Lane", NULL, TRUE, "brought");
	sql(fx, "INSERT INTO arr_brought (source_id, title_id, movie_id, label) SELECT %" G_GINT64_FORMAT
		", title_id, id, 'Copper Kettle Lane' FROM movie WHERE id = %" G_GINT64_FORMAT, source, movie);
	clock_at(fx, 6);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	admin_page = page_of(fx, fx->admin);
	assert_contains(admin_page, "1 movie needs attention because every acceptable release failed.");
	g_clear_pointer(&admin_page, g_free);

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	clock_at(fx, 17);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);

	/* The administrator sees all six: the movie's destination is on the
	 * disk that is offline now, so the movie is a consequence of what has
	 * its own notice, and is not counted any more. */
	admin_page = page_of(fx, fx->admin);
	admin_bell = bell_of(fx, fx->admin);
	assert_contains(admin_page, "<bdi>Attic</bdi>");
	assert_contains(admin_page, "<bdi>Harbor</bdi>");
	assert_contains(admin_page, "<bdi>Lanternfish</bdi>");
	assert_contains(admin_page, "<bdi>Old shelf</bdi>");
	assert_contains(admin_page, "1 import is blocked");
	assert_contains(admin_page, "2 test things wait at ");
	assert_lacks(admin_page, "needs attention because");
	assert_contains(admin_bell, "aria-label=\"6 things need attention, 4 not working\"");

	/* The page and the posts refuse the other user... */
	reply = get_as(fx, fx->viewer, "/attention");
	g_assert_cmpuint(reply.status, ==, 403);
	for (k = 0; k < G_N_ELEMENTS(secrets) - 3; k++)
		assert_lacks(reply.body, secrets[k]);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/attention?part=list");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=3&count=2&newest=5");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, TRUE, "/attention/restore", "kind=test.thing&subject=");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);

	/* ...the bell's own request answers them 403 with nothing in it (which
	 * is what takes the bell off a page left open)... */
	reply = get_as(fx, fx->viewer, "/attention/bell");
	g_assert_cmpuint(reply.status, ==, 403);
	g_assert_cmpuint(reply.length, ==, 0);
	axis_test_reply_clear(&reply);

	/* ...and no page of theirs has a bell, a count or a word of a notice
	 * in its header. */
	for (i = 0; i < G_N_ELEMENTS(pages); i++) {
		const gchar *header_end;
		g_autofree gchar *header = NULL;

		reply = get_as(fx, fx->viewer, pages[i]);
		g_assert_cmpuint(reply.status, ==, 200);
		header_end = strstr(reply.body, "</header>");
		g_assert_nonnull(header_end);
		header = g_strndup(reply.body, (gsize)(header_end - reply.body));
		assert_lacks(header, "attention-bell");
		assert_lacks(header, "attn-");
		assert_lacks(header, "need attention");
		assert_lacks(header, "needs attention");
		assert_lacks(header, "topic=attention");
		axis_test_reply_clear(&reply);
	}
	reply = get_as(fx, fx->viewer, "/test-things");
	for (k = 0; k < G_N_ELEMENTS(secrets); k++) {
		/* "Test things" is the page's own title. */
		if (strcmp(secrets[k], "test things") != 0)
			assert_lacks(reply.body, secrets[k]);
	}
	axis_test_reply_clear(&reply);
	/* The dashboard has no card of what needs attention for them (slice N2:
	 * the card is the head of the page's list, and they have no list), and
	 * not a word of a notice. */
	reply = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "href=\"/attention\"");
	assert_lacks(reply.body, "dash-attention");
	for (k = 0; k < G_N_ELEMENTS(secrets); k++) {
		/* The disk is on their Disk space card by its name and its state,
		 * and a movie they may open in "Movies added lately" by its title. */
		if (strcmp(secrets[k], "Attic") != 0 && strcmp(secrets[k], "offline") != 0 &&
		    strcmp(secrets[k], "Lantern Bay") != 0)
			assert_lacks(reply.body, secrets[k]);
	}
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "id=\"dash-attention\"");
	assert_contains(reply.body, "<a href=\"/attention\">All 6 on the Needs attention page</a>");
	axis_test_reply_clear(&reply);

	/* Without a session: no page at all. */
	reply = get_as(fx, NULL, "/attention");
	g_assert_cmpuint(reply.status, !=, 200);
	assert_lacks(reply.body, "Attic");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, NULL, "/attention/bell");
	g_assert_cmpuint(reply.status, !=, 200);
	assert_lacks(reply.body, "attention-bell");
	axis_test_reply_clear(&reply);
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
}

/* ── What it costs ──────────────────────────────────────────────── */

/* Spec (attention): Many page views.  No statement and no reading for a
 * hundred pages, a hundred bells and a hundred lists, with notices and a
 * dismissal in memory. */
static void
test_page_views_cost_nothing(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	guint statements;
	guint readings;
	gint calls;
	guint i;

	start(fx);
	set_fake(fx, 3, AXIS_ATTENTION_BROKEN, 10);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	read_board(fx);
	reply = post_as(fx, fx->ops, TRUE, "/attention/dismiss", "kind=import.blocked&subject=&severity=2&count=1&newest=1");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	/* The post asked for a reading: let it pass. */
	read_board(fx);
	while (g_main_context_iteration(NULL, FALSE))
		;

	statements = axis_db_pool_get_statements_sent(fx->t->pool);
	readings = axis_web_attention_get_readings(fx->web);
	calls = g_atomic_int_get(&fx->fake.calls);
	for (i = 0; i < 100; i++) {
		reply = get_as(fx, i % 2 == 0 ? fx->admin : fx->ops, "/test-things");
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, i % 2 == 0 ? "aria-label=\"2 things need attention, 1 not working\"" :
			"aria-label=\"1 thing needs attention, 1 not working\"");
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, "/attention/bell");
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->ops, "/attention");
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->viewer, "/test-things");
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(axis_db_pool_get_statements_sent(fx->t->pool), ==, statements);
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, readings);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.calls), ==, calls);
}

/* Spec (attention): A burst of changes.  Fifty jobs that end within a
 * moment are one reading, and the gap between two readings is kept. */
static void
test_burst_is_one_reading(Fixture *fx, gconstpointer data)
{
	AxisAttentionTuning tuning;
	gint64 until;
	guint readings;
	gint calls;
	guint i;

	start(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.min_gap_ms = 400;
	axis_web_attention_set_tuning(fx->web, &tuning);
	/* Past the gap since the last reading. */
	until = g_get_monotonic_time() + 500 * 1000;
	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, FALSE);

	readings = axis_web_attention_get_readings(fx->web);
	calls = g_atomic_int_get(&fx->fake.calls);
	/* As the queue tells the Jobs pages, which tell the board. */
	for (i = 0; i < 50; i++) {
		AxisJobEvent event;

		memset(&event, 0, sizeof(event));
		event.type = AXIS_JOB_EVENT_STATE;
		event.id = 1000 + i;
		event.kind = (gchar *)"import.run";
		event.queue = (gchar *)"downloads";
		event.state = AXIS_JOB_STATE_SUCCEEDED;
		event.progress = -1.0;
		axis_web_jobs_bridge_event(fx->jobs, &event);
	}
	pump_until_reading(fx, readings);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.calls), ==, calls + 1);
	/* Nothing more follows by itself. */
	until = g_get_monotonic_time() + 900 * 1000;
	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, FALSE);
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, readings + 1);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.calls), ==, calls + 1);

	/* A job that only started, or wrote its progress, changes nothing. */
	for (i = 0; i < 5; i++) {
		AxisJobEvent event;

		memset(&event, 0, sizeof(event));
		event.type = i % 2 == 0 ? AXIS_JOB_EVENT_STATE : AXIS_JOB_EVENT_PROGRESS;
		event.id = 2000 + i;
		event.kind = (gchar *)"import.run";
		event.queue = (gchar *)"downloads";
		event.state = AXIS_JOB_STATE_RUNNING;
		event.progress = 0.5;
		axis_web_jobs_bridge_event(fx->jobs, &event);
	}
	until = g_get_monotonic_time() + 900 * 1000;
	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, FALSE);
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, readings + 1);
}

/* Spec (attention): A source that fails. */
static void
test_failing_source(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = NULL;
	g_autofree gchar *mended = NULL;

	start(fx);
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 10);
	read_board(fx);

	g_atomic_int_set(&fx->fake.fail, 1);
	set_fake(fx, 9, AXIS_ATTENTION_WAITING, 10);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	read_board(fx);
	page = page_of(fx, fx->admin);
	/* Its last notice is still shown, and the page says since when; the
	 * other sources' are fresh. */
	assert_contains(page, "3 test things wait at ");
	assert_contains(page, "Axis could not read the test things at its last look. What this page says about them is "
		"unchanged since 20");
	assert_contains(page, "1 import is blocked");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*the test things could not be read*"), ==, 1);

	g_atomic_int_set(&fx->fake.fail, 0);
	read_board(fx);
	mended = page_of(fx, fx->admin);
	assert_contains(mended, "9 test things wait at ");
	assert_lacks(mended, "could not read");
}

/*
 * A stop while a reading waits on the database (spec attention: A stop while
 * the database does not answer).  The reading is in a statement that takes
 * %BLOCK_SECONDS and has no deadline on this side, as a source's is when the
 * database goes silent under it; the stop ends within a second all the same,
 * the statement is given up, no further source is run, and what the reading
 * gave is thrown away.  (The stop waited for the reading: before, for as long
 * as the statement took, and at a daemon whose database had gone silent for
 * the sum of every source's deadline, which ended tests/test-daemon.c's
 * /daemon/health/does-not-block.)
 */
static void
test_stop_while_blocked(Fixture *fx, gconstpointer data)
{
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	guint readings;
	gint after;
	gint64 began;
	gint64 took;

	start(fx);
	readings = axis_web_attention_get_readings(fx->web);
	after = g_atomic_int_get(&fx->fake.after);
	g_assert_cmpint(after, >, 0);

	fx->fake.pool = fx->t->pool;
	g_atomic_int_set(&fx->fake.block, 1);
	axis_web_attention_changed(fx->web);
	while (g_atomic_int_get(&fx->fake.entered) == 0) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_main_context_iteration(NULL, FALSE);
		g_usleep(5 * 1000);
	}
	/* The statement is with the server by now. */
	g_usleep(300 * 1000);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.answered), ==, 0);

	began = g_get_monotonic_time();
	axis_web_stop(fx->web);
	took = g_get_monotonic_time() - began;
	g_test_message("the stop took %" G_GINT64_FORMAT " ms", took / 1000);
	g_assert_cmpint(took, <, 1 * G_USEC_PER_SEC);

	/* The statement was given up, the source after it was not run, and the
	 * reading does not count. */
	g_assert_cmpint(g_atomic_int_get(&fx->fake.entered), ==, 1);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.answered), ==, 0);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.after), ==, after);
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, readings);
	/* Nothing is read after a stop, whatever happens. */
	g_atomic_int_set(&fx->fake.block, 0);
	axis_web_attention_changed(fx->web);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, readings);
	/* The connection of the statement that was given up went back to the
	 * pool, which closed it. */
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->t->pool), ==, 0);
}

/* ── Live ───────────────────────────────────────────────────────── */

/* Spec (web-live): A new notice; Nothing changed; Nothing in the event;
 * Another tab. */
static void
test_live(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) admin = NULL;
	g_autoptr(AxisTestStream) viewer = NULL;
	g_autofree gchar *list = NULL;
	const gchar *const htmx[] = { "HX-Request", "true", NULL };
	AxisTestReply reply;
	guint changes;

	start(fx);
	admin = axis_test_stream_open(fx->port, "/events?topic=attention:admin", fx->admin, 0);
	g_assert_true(axis_test_stream_wait_for(admin, "\r\n\r\n", 5000));
	g_assert_cmpuint(axis_test_stream_get_status(admin), ==, 200);
	/* The topic is administrators': anyone else is refused, and hears
	 * nothing, not even that something changed. */
	reply = get_as(fx, fx->viewer, "/events?topic=attention:admin");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	viewer = axis_test_stream_open(fx->port, "/events?topic=attention", fx->viewer, 0);
	g_assert_true(axis_test_stream_wait_for(viewer, "\r\n\r\n", 5000));

	/* A notice comes: the event, with nothing of it. */
	set_fake(fx, 3, AXIS_ATTENTION_BROKEN, 10);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	changes = axis_web_attention_get_changes(fx->web);
	read_board(fx);
	g_assert_true(axis_test_stream_wait_for(admin, "event: attention", 5000));
	g_assert_cmpuint(axis_web_attention_get_changes(fx->web), ==, changes + 1);
	g_assert_cmpuint(axis_test_stream_count(admin, "event: attention"), ==, 1);
	/* The event says only that something changed. */
	g_assert_null(strstr(axis_test_stream_get_text(admin), "Pier"));
	g_assert_null(strstr(axis_test_stream_get_text(admin), "import"));
	g_assert_null(strstr(axis_test_stream_get_text(admin), "Lantern"));
	g_assert_null(strstr(axis_test_stream_get_text(admin), "test"));
	g_assert_nonnull(strstr(axis_test_stream_get_text(admin), "data: changed"));
	/* Someone following the name the topic once had (open to everyone)
	 * hears nothing. */
	g_assert_cmpuint(axis_test_stream_count(viewer, "event: attention"), ==, 0);
	g_assert_null(strstr(axis_test_stream_get_text(viewer), "data: changed"));

	/* The same notices again: nothing is published. */
	read_board(fx);
	read_board(fx);
	g_assert_cmpuint(axis_web_attention_get_changes(fx->web), ==, changes + 1);
	g_assert_cmpuint(axis_test_stream_count(admin, "event: attention"), ==, 1);

	/* A dismissal tells the user's other pages. */
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=3&count=3&newest=10");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	{
		gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

		while (axis_test_stream_count(admin, "event: attention") < 2 && g_get_monotonic_time() < until)
			axis_test_stream_wait_for(admin, "never sent", 50);
		g_assert_cmpuint(axis_test_stream_count(admin, "event: attention"), >=, 2);
	}

	/* What a page asks for when told: its own list, and its own bell. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/attention?part=list", fx->admin, htmx, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<div "));
	assert_contains(reply.body, "id=\"attention-list\"");
	assert_contains(reply.body, "hx-get=\"/attention?part=list\"");
	assert_contains(reply.body, "hx-trigger=\"sse:attention\"");
	assert_contains(reply.body, "1 import is blocked");
	assert_contains(reply.body, "<summary>Dismissed (1)</summary>");
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/attention/bell");
	g_assert_true(g_str_has_prefix(reply.body, "<a "));
	assert_contains(reply.body, "id=\"attention-bell\"");
	assert_contains(reply.body, "aria-label=\"1 thing needs attention\"");
	assert_contains(reply.body, "hx-swap=\"outerHTML\"");
	axis_test_reply_clear(&reply);
}

/* A shell whose readings were not started (every other test program's) has a
 * quiet bell for administrators, follows no topic for it and reads nothing. */
static void
test_not_started(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	guint statements;

	set_fake(fx, 3, AXIS_ATTENTION_BROKEN, 10);
	axis_web_attention_changed(fx->web);
	axis_web_attention_job_ended(fx->web, "import.run");
	while (g_main_context_iteration(NULL, FALSE))
		;
	statements = axis_db_pool_get_statements_sent(fx->t->pool);
	reply = get_as(fx, fx->admin, "/test-things");
	assert_contains(reply.body, "aria-label=\"Nothing needs attention\"");
	assert_lacks(reply.body, "topic=attention");
	assert_lacks(reply.body, "hx-get=\"/attention/bell\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Nothing needs attention.");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_web_attention_get_readings(fx->web), ==, 0);
	g_assert_cmpint(g_atomic_int_get(&fx->fake.calls), ==, 0);
	g_assert_cmpuint(axis_db_pool_get_statements_sent(fx->t->pool), ==, statements);
}

/* ── The review's findings ──────────────────────────────────────── */

/* "Since 2026-11-22 19:34:37 UTC" of the first row of @page that has one. */
static gchar *
since_of(const gchar *page)
{
	const gchar *from = strstr(page, "Since ");
	const gchar *to = from != NULL ? strstr(from, " UTC") : NULL;

	g_assert_nonnull(to);
	return g_strndup(from, (gsize)(to - from));
}

/* Finding 1: the movies are counted after a search or an import ended, not
 * after the jobs that only follow or schedule (which run every minute); no
 * sooner than five minutes after the count before, whatever ended; and after
 * ten minutes whatever happened. */
static void
test_titles_are_counted_seldom(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = NULL;
	g_autofree gchar *follow = NULL;
	g_autofree gchar *search = NULL;
	g_autofree gchar *floor = NULL;
	g_autofree gchar *past_floor = NULL;
	g_autofree gchar *by_time = NULL;

	start(fx);
	add_movie(fx, "Lantern Bay", "exhausted", TRUE, NULL);
	clock_at(fx, 20);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	one = page_of(fx, fx->admin);
	assert_contains(one, "1 movie needs attention");

	/* The jobs that follow the downloads and plan the searches end every
	 * minute: they cause no count, even past the five minutes. */
	add_movie(fx, "Copper Kettle Lane", "exhausted", TRUE, NULL);
	clock_at(fx, 26);
	axis_web_attention_job_ended(fx->web, "movies.follow");
	axis_web_attention_job_ended(fx->web, "movies.schedule");
	axis_web_attention_job_ended(fx->web, "download.track");
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	follow = page_of(fx, fx->admin);
	assert_contains(follow, "1 movie needs attention");

	/* A search that ended does. */
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	search = page_of(fx, fx->admin);
	assert_contains(search, "2 movies need attention");

	/* Another one a minute later does not: five minutes between two counts. */
	add_movie(fx, "Gullwing Ferry", "exhausted", TRUE, NULL);
	clock_at(fx, 27);
	axis_web_attention_job_ended(fx->web, "movies.search");
	read_board(fx);
	floor = page_of(fx, fx->admin);
	assert_contains(floor, "2 movies need attention");
	/* It is remembered, and counted once the five minutes are over. */
	clock_at(fx, 32);
	read_board(fx);
	past_floor = page_of(fx, fx->admin);
	assert_contains(past_floor, "3 movies need attention");

	/* With nothing ended: after ten minutes, not before. */
	add_movie(fx, "The Morning Tide Report", "exhausted", TRUE, NULL);
	clock_at(fx, 38);
	read_board(fx);
	g_clear_pointer(&past_floor, g_free);
	past_floor = page_of(fx, fx->admin);
	assert_contains(past_floor, "3 movies need attention");
	clock_at(fx, 43);
	read_board(fx);
	by_time = page_of(fx, fx->admin);
	assert_contains(by_time, "4 movies need attention");
}

/* Finding 2: a disk this daemon has not checked is not judged by the age its
 * row gives. */
static void
test_disk_not_checked_yet(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	/* Hours later, as its row would say of a disk that went offline while
	 * the daemon was stopped. */
	clock_at(fx, 180);
	start(fx);
	before = page_of(fx, fx->admin);
	assert_contains(before, "Nothing needs attention.");
	assert_lacks(before, "Attic");

	/* This daemon's own check finds it away: now it is said, with its age. */
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	read_board(fx);
	after = page_of(fx, fx->admin);
	assert_contains(after, "<bdi>Attic</bdi><span> has been offline for 3 hours.");
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
}

/* Finding 3: the wait and "since" count from when the thing stopped working,
 * not from the last change of why. */
static void
test_reason_changes_keep_the_clock(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *since_first = NULL;
	g_autofree gchar *since_second = NULL;
	g_autofree gchar *clients = NULL;
	gint64 indexer;

	start(fx);
	/* A disk away: its directory empty, then gone altogether. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	clock_at(fx, 11);
	read_board(fx);
	first = page_of(fx, fx->admin);
	assert_contains(first, "The directory is empty and has no marker");
	since_first = since_of(first);
	g_usleep(1200 * 1000);
	g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	read_board(fx);
	second = page_of(fx, fx->admin);
	assert_contains(second, "There is no directory at the disk");
	since_second = since_of(second);
	g_assert_cmpstr(since_first, ==, since_second);
	g_assert_cmpint(g_mkdir(fx->disk, 0700), ==, 0);
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));

	/* A client failing for 40 minutes that now does not answer at all, and
	 * an indexer the same: one outage, 40 minutes old. */
	clock_at(fx, 0);
	sql(fx, "UPDATE download_client SET state = 'failing', " NOT_WORKING_SINCE " = now() - interval '40 minutes',"
		" last_checked_at = now() - interval '2 minutes'");
	g_assert_true(axis_downloads_test_client(fx->downloads, fx->client_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download_client WHERE state = 'unreachable'"), ==, 1);
	add_indexer(fx, "Driftwood", "ok", 600, 1);
	indexer = add_indexer(fx, "Saltmarsh", "failing", 40, 2);
	axis_indexers_test_source(fx->indexers, indexer, NULL, &error);
	g_clear_error(&error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM indexer_source WHERE id = %" G_GINT64_FORMAT
		" AND state = 'unreachable'", indexer), ==, 1);
	read_board(fx);
	clients = page_of(fx, fx->admin);
	assert_contains(clients, "<bdi>Harbor</bdi><span> has not answered for 40 minutes.</span>");
	assert_contains(clients, "<bdi>Saltmarsh</bdi><span> has not answered for 40 minutes.</span>");
}

/* Finding 4: a dismissal lasts while its notice lasts.  When the cause is
 * gone, the dismissal goes for everyone, so that other things of the same
 * kind and number are shown later. */
static void
test_dismissal_ends_with_its_notice(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	set_fake(fx, 12, AXIS_ATTENTION_WAITING, 40);
	read_board(fx);
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=12&newest=40");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->ops, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=12&newest=40");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 2);

	/* Fewer, not none: the dismissal stays. */
	set_fake(fx, 5, AXIS_ATTENTION_WAITING, 40);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_clear_pointer(&bell, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 2);

	/* All mended: the notice is gone, and the dismissals with it. */
	set_fake(fx, 0, AXIS_ATTENTION_WAITING, 0);
	read_board(fx);
	read_board(fx);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);

	/* Twelve others fail later, older ones: no more of them, nothing newer,
	 * nothing more serious; and they are shown, to both. */
	set_fake(fx, 12, AXIS_ATTENTION_WAITING, 30);
	read_board(fx);
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
	g_clear_pointer(&bell, g_free);
	bell = bell_of(fx, fx->ops);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");
}

/* Finding 5: only an administrator may follow the notices' topic. */
static void
test_topic_is_administrators(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	start(fx);
	reply = get_as(fx, fx->admin, "/test-things");
	assert_contains(reply.body, "topic=attention:admin");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/events?topic=attention:admin");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/test-things");
	assert_lacks(reply.body, "topic=attention");
	axis_test_reply_clear(&reply);
}

/* Finding 6: a client or an indexer switched off and on again is not said
 * to have been failing all the while. */
static void
test_switched_on_again(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *later = NULL;
	AxisDownloadClientSpec client;
	AxisIndexerSourceSpec source;
	gint64 indexer;
	guint on;

	start(fx);
	add_indexer(fx, "Driftwood", "ok", 600, 1);
	indexer = add_indexer(fx, "Saltmarsh", "unreachable", 120, 60);
	sql(fx, "UPDATE download_client SET state = 'unreachable', " NOT_WORKING_SINCE " = now() - interval '120 minutes',"
		" last_checked_at = now() - interval '60 minutes'");
	read_board(fx);
	before = page_of(fx, fx->admin);
	assert_contains(before, "<bdi>Harbor</bdi>");
	assert_contains(before, "<bdi>Saltmarsh</bdi>");

	for (on = 0; on < 2; on++) {
		memset(&client, 0, sizeof(client));
		client.name = "Harbor";
		client.kind = "fixturenzb";
		client.address = NOWHERE;
		client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
		client.enabled = on == 1;
		g_assert_true(axis_downloads_save_client(fx->downloads, fx->client_id, &client, NULL, &error));
		g_assert_no_error(error);
		memset(&source, 0, sizeof(source));
		source.name = "Saltmarsh";
		source.kind = AXIS_INDEXER_PROWLARR_NAME;
		source.address = NOWHERE;
		source.enabled = on == 1;
		g_assert_true(axis_indexers_save_source(fx->indexers, indexer, &source, NULL, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM download_client WHERE enabled AND state = 'unreachable'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM indexer_source WHERE enabled AND state = 'unreachable'"), ==, 1);
	read_board(fx);
	after = page_of(fx, fx->admin);
	assert_contains(after, "Nothing needs attention.");

	/* Still not answering half an hour on (and, the indexer, contacted
	 * again): said, with the time since it was switched on. */
	sql(fx, "UPDATE indexer_source SET last_checked_at = now() + interval '20 minutes' WHERE id = %" G_GINT64_FORMAT,
		indexer);
	clock_at(fx, 31);
	read_board(fx);
	later = page_of(fx, fx->admin);
	assert_contains(later, "<bdi>Harbor</bdi><span> has not answered for 31 minutes.</span>");
	assert_contains(later, "<bdi>Saltmarsh</bdi><span> has not answered for 31 minutes.</span>");
}

/* ── Slice N2 ───────────────────────────────────────────────────── */

/* The dashboard's card "Needs attention" as @cookie's user is sent it: the
 * section alone ("" when the page has none). */
static gchar *
card_of(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply reply = get_as(fx, cookie, "/");
	const gchar *begin;
	const gchar *end;
	gchar *card;

	g_assert_cmpuint(reply.status, ==, 200);
	begin = strstr(reply.body, "id=\"dash-attention\"");
	end = begin != NULL ? strstr(begin, "</section>") : NULL;
	card = begin != NULL && end != NULL ? g_strndup(begin, (gsize)(end - begin)) : g_strdup("");
	axis_test_reply_clear(&reply);
	return card;
}

/* Spec (web-shell): The dashboard's card is the head of the same list.  The
 * same waits, levels and dismissals as the page: no second computation. */
static void
test_dashboard_card(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *early = NULL;
	g_autofree gchar *some = NULL;
	g_autofree gchar *mine = NULL;
	g_autofree gchar *theirs = NULL;
	g_autofree gchar *many = NULL;
	const gchar *client;
	const gchar *imports;
	AxisTestReply reply;
	gint i;

	start(fx);
	none = card_of(fx, fx->admin);
	assert_contains(none, "<a href=\"/attention\">Needs attention</a>");
	assert_contains(none, "Nothing needs attention.");
	assert_lacks(none, "attention-list");

	/* A disk away for three minutes is not on the page yet, so it is not on
	 * the card either (the Disk space card says it at once). */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	clock_at(fx, 3);
	read_board(fx);
	early = card_of(fx, fx->admin);
	assert_contains(early, "Nothing needs attention.");
	assert_lacks(early, "Attic");
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	clock_at(fx, 0);

	set_fake(fx, 2, AXIS_ATTENTION_LOOK, 5);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'blocked')");
	sql(fx, "UPDATE download_client SET state = 'refused', not_working_since = now()");
	read_board(fx);
	some = card_of(fx, fx->admin);
	/* The most serious first, its level in words, the sentence as the link
	 * to where it is dealt with. */
	client = strstr(some, "<bdi>Harbor</bdi>");
	imports = strstr(some, "1 import is blocked");
	g_assert_true(client != NULL && imports != NULL && client < imports);
	assert_contains(some, "attention-level\">Not working</span><a href=\"/downloads/clients/");
	assert_contains(some, "attention-level\">Waiting for you</span><a href=\"/imports\">");
	assert_contains(some, "attention-level\">Worth a look</span><a href=\"/test-things\">");
	assert_contains(some, "<bdi>Pier &lt;Nine&gt;</bdi>");
	assert_contains(some, "<a href=\"/attention\">Open the Needs attention page</a>");
	assert_lacks(some, "Nothing needs attention.");
	/* A title is not named: the card holds what the page holds. */
	assert_lacks(some, "Lantern Bay");
	assert_lacks(some, "Dismiss");

	/* What one administrator dismissed is off their card, not the other's. */
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=1&count=2&newest=5");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	mine = card_of(fx, fx->admin);
	theirs = card_of(fx, fx->ops);
	assert_lacks(mine, "test things");
	assert_contains(mine, "1 import is blocked");
	assert_contains(theirs, "2 test things wait at ");

	/* More than the card shows: the first five, and how many there are. */
	for (i = 0; i < 4; i++) {
		g_autofree gchar *name = g_strdup_printf("Lanternfish %d", i);

		add_indexer(fx, name, "refused", 0, 0);
	}
	read_board(fx);
	many = card_of(fx, fx->ops);
	assert_contains(many, "<a href=\"/attention\">All 7 on the Needs attention page</a>");
	assert_contains(many, "<bdi>Harbor</bdi>");
	assert_contains(many, "<bdi>Lanternfish 0</bdi>");
	/* The least serious are left for the page. */
	assert_lacks(many, "test things");
	assert_lacks(many, "1 import is blocked");
}

/* Spec (attention): A disk that is almost full.  The setting, its notice
 * under "Worth a look", and "never". */
static void
test_low_space(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *off = NULL;
	g_autofree gchar *low = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *link = NULL;

	start(fx);
	off = page_of(fx, fx->admin);
	assert_contains(off, "Nothing needs attention.");

	/* Less free than the whole of it: any disk that holds a file. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_LOW_SPACE, "100", &error));
	g_assert_no_error(error);
	read_board(fx);
	low = page_of(fx, fx->admin);
	assert_contains(low, "id=\"attention-look\"");
	assert_contains(low, "<span>The disk </span><bdi>Attic</bdi><span> is almost full: ");
	assert_contains(low, " free of ");
	assert_contains(low, ", less than 100 percent.</span>");
	link = g_strdup_printf("<a href=\"/storage/volumes/%" G_GINT64_FORMAT "\">Open the disk</a>", fx->volume_id);
	assert_contains(low, link);
	assert_lacks(low, fx->disk);

	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_LOW_SPACE, "0", &error));
	g_assert_no_error(error);
	read_board(fx);
	again = page_of(fx, fx->admin);
	assert_contains(again, "Nothing needs attention.");
}

/* Spec (attention): Imports that wait for Read-only storage.  One thing
 * however many wait: dismissed once, it stays dismissed while more arrive. */
static void
test_imports_read_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *more = NULL;
	g_autofree gchar *off = NULL;
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	/* The storage's own setting is on to begin with. */
	g_assert_true(axis_storage_is_read_only_mode(fx->t->storage));
	sql(fx, "INSERT INTO import (module, title_label, source_path, state, reason, reason_text) VALUES"
		" ('movies', 'Lantern Bay (2025)', 'downloads/lantern.bay', 'waiting', 'read_only', 'As the test made it.'),"
		/* One that waits for something else is not counted. */
		" ('movies', 'Gullwing Ferry (2021)', 'downloads/gullwing', 'waiting', 'offline', 'As the test made it.')");
	read_board(fx);
	one = page_of(fx, fx->admin);
	assert_contains(one, "id=\"attention-waiting\"");
	assert_contains(one, "1 import waits because Read-only storage is on: nothing is imported until you switch it off.");
	assert_contains(one, "<a href=\"/admin/settings#module-storage\">Open Settings, Storage</a>");
	assert_lacks(one, "Lantern Bay");

	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=import.read_only&subject=&severity=2&count=1&newest=0");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	sql(fx, "INSERT INTO import (module, title_label, source_path, state, reason, reason_text) VALUES"
		" ('movies', 'Copper Kettle Lane (2019)', 'downloads/copper', 'waiting', 'read_only', 'As the test made it.')");
	read_board(fx);
	more = page_of(fx, fx->admin);
	assert_contains(more, "Dismissed (1)");
	assert_contains(more, "2 imports wait because Read-only storage is on");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");

	/* Switched off: no notice, and no statement asks. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	read_board(fx);
	off = page_of(fx, fx->admin);
	assert_contains(off, "Nothing needs attention.");
	assert_lacks(off, "Read-only storage");
	assert_lacks(off, "Dismissed (");
}

/* Spec (attention): Finished downloads that cannot be imported; Leftovers. */
static void
test_downloads_stuck(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *moving = NULL;
	g_autofree gchar *stuck = NULL;
	g_autofree gchar *away = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	start(fx);
	sql(fx, "UPDATE download_client SET state = 'ok'");
	sql(fx, "INSERT INTO download (client_id, client_name, source, module, title_label, release_name, protocol, state,"
		" client_ref, client_path, problem, progress_at) SELECT id, name, 'file', 'movies', 'Lantern Bay (2025)',"
		" 'Lantern.Bay.2025.1080p', 'usenet', 'downloading', 'ref-1', '/elsewhere/lantern',"
		" 'Finished at the download client, but cannot be imported yet: nothing is at the mapped place on Attic (check"
		" the mapping).', now() FROM download_client");
	/* (N2 review, S2.)  Finished three minutes ago: the client may still be
	 * moving it to its place, and nothing is said. */
	clock_at(fx, 3);
	read_board(fx);
	moving = page_of(fx, fx->admin);
	assert_contains(moving, "Nothing needs attention.");

	/* Eleven minutes: a notice, which names no cause (the row has one of a
	 * dozen) and sends the person to where each download's reason is shown. */
	clock_at(fx, 11);
	read_board(fx);
	stuck = page_of(fx, fx->admin);
	assert_contains(stuck, "id=\"attention-waiting\"");
	assert_contains(stuck, "<span>1 finished download at the download client </span><bdi>Harbor</bdi>"
		"<span> cannot be imported yet. The Downloads page says why.</span>");
	assert_contains(stuck, "<a href=\"/downloads\">Open Downloads</a>");
	assert_lacks(stuck, "maps to no disk");
	assert_lacks(stuck, "Lantern");
	assert_lacks(stuck, "/elsewhere");

	/* (N2 review, S1.)  Dismissed; then a disk goes away.  That may be why a
	 * download cannot be imported, so nothing is known of them meanwhile:
	 * the notice stays as it was, dismissed, and is still dismissed when the
	 * disk is back.  (It ended for the outage, and came back undismissed.) */
	form = g_strdup_printf("kind=download.stuck&subject=%" G_GINT64_FORMAT "&severity=2&count=1&newest=%" G_GINT64_FORMAT,
		fx->client_id, qi(fx, "SELECT max(id) FROM download"));
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	clock_at(fx, 13);
	read_board(fx);
	read_board(fx);
	away = page_of(fx, fx->admin);
	assert_contains(away, "Dismissed (1)");
	assert_contains(away, "cannot be imported yet");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal WHERE kind = 'download.stuck'"), ==, 1);
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	read_board(fx);
	back = page_of(fx, fx->admin);
	assert_contains(back, "Dismissed (1)");
	assert_lacks(back, "id=\"attention-waiting\"");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal WHERE kind = 'download.stuck'"), ==, 1);
	sql(fx, "DELETE FROM download");

	/* What the hourly cleanup counted in the download folders. */
	sql(fx, "UPDATE download_client SET leftover_count = 3, leftover_checked_at = now()");
	read_board(fx);
	left = page_of(fx, fx->admin);
	assert_contains(left, "id=\"attention-look\"");
	assert_contains(left, "3 files and folders are left in the download folders and were never imported.");
	assert_contains(left, "<a href=\"/downloads/leftovers\">Open Leftovers in the download folders</a>");
	assert_lacks(left, "cannot be imported yet");
}

/* Spec (attention): An indexer Axis holds.  At once, with no second contact,
 * and not also as "failing". */
static void
test_indexer_held(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *alone = NULL;
	g_autofree gchar *held = NULL;
	g_autofree gchar *waits = NULL;
	gint64 definition;
	gint64 other;
	gint64 id;

	start(fx);
	definition = qi(fx, "SELECT coalesce((SELECT min(id) FROM indexer_definition), 0)");
	if (definition == 0) {
		g_test_skip("no site definition is installed in this fixture");
		return;
	}
	id = add_indexer(fx, "Lanternfish", "ok", 0, 0);
	sql(fx, "UPDATE indexer_source SET kind = 'cardigann', definition_id = %" G_GINT64_FORMAT ", state = 'failing',"
		" state_message = 'Lanternfish''s pages no longer look as expected', held_until = now() + interval '1 day',"
		" hold_message = 'Lanternfish''s pages no longer look as expected' WHERE id = %" G_GINT64_FORMAT, definition, id);
	/* (N2 review, S3.)  The only switched-on indexer, or the only one that
	 * is not failing: nothing can be searched for, which is "Not working",
	 * and the sentence says so. */
	other = add_indexer(fx, "Saltmarsh", "unreachable", 5, 5);
	read_board(fx);
	alone = page_of(fx, fx->admin);
	assert_contains(alone, "id=\"attention-broken\"");
	assert_lacks(alone, "id=\"attention-look\"");
	assert_contains(alone, "<span>Axis does not ask the indexer </span><bdi>Lanternfish</bdi><span>: its pages no longer "
		"look as its site definition expects. Test it, or install a newer definition. No other indexer can be searched "
		"either: nothing can be searched for.</span>");

	/* Another indexer answers: "Worth a look", the rest still works. */
	sql(fx, "UPDATE indexer_source SET state = 'ok', " NOT_WORKING_SINCE " = NULL WHERE id = %" G_GINT64_FORMAT, other);
	read_board(fx);
	held = page_of(fx, fx->admin);
	assert_contains(held, "id=\"attention-look\"");
	assert_lacks(held, "id=\"attention-broken\"");
	assert_contains(held, "<span>Axis does not ask the indexer </span><bdi>Lanternfish</bdi><span>: its pages no longer "
		"look as its site definition expects. Test it, or install a newer definition.</span>");
	assert_lacks(held, "has been failing");

	/* An indexer that asked Axis to wait ends by itself: no notice. */
	sql(fx, "UPDATE indexer_source SET state = 'ok', state_message = NULL, hold_message = 'asked to wait' WHERE id = %"
		G_GINT64_FORMAT, id);
	read_board(fx);
	waits = page_of(fx, fx->admin);
	assert_contains(waits, "Nothing needs attention.");
}

/* (N2 review, S1; the reviewer's second probe.)  An indexer held for a
 * browser challenge is held until a contact works, not until the hold's hour
 * has passed: the next search only holds it again.  A notice that ended in
 * between came back undismissed every hour. */
static void
test_indexer_held_past_its_hour(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *held = NULL;
	g_autofree gchar *gap = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *works = NULL;
	g_autofree gchar *bell = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 definition;
	gint64 id;

	start(fx);
	definition = qi(fx, "SELECT coalesce((SELECT min(id) FROM indexer_definition), 0)");
	if (definition == 0) {
		g_test_skip("no site definition is installed in this fixture");
		return;
	}
	add_indexer(fx, "Driftwood", "ok", 0, 0);
	id = add_indexer(fx, "Lanternfish", "ok", 0, 0);
	sql(fx, "UPDATE indexer_source SET kind = 'cardigann', definition_id = %" G_GINT64_FORMAT ", state = 'failing',"
		" state_message = 'Lanternfish answered 403 with a browser challenge in front of its pages',"
		" held_until = now() + interval '50 minutes',"
		" hold_message = 'Lanternfish answers with a browser challenge Axis cannot pass' WHERE id = %" G_GINT64_FORMAT,
		definition, id);
	read_board(fx);
	held = page_of(fx, fx->admin);
	assert_contains(held, "<bdi>Lanternfish</bdi><span>: it answers with a browser challenge Axis cannot pass.");
	form = g_strdup_printf("kind=indexer.held&subject=%" G_GINT64_FORMAT "&severity=1&count=1&newest=0", id);
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	/* The hour passed and nothing contacted it yet: still held, still
	 * dismissed. */
	sql(fx, "UPDATE indexer_source SET held_until = now() - interval '1 minute' WHERE id = %" G_GINT64_FORMAT, id);
	read_board(fx);
	read_board(fx);
	gap = page_of(fx, fx->admin);
	assert_contains(gap, "Dismissed (1)");
	assert_contains(gap, "it answers with a browser challenge");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);

	/* The next search contacts it and holds it again: nothing new. */
	sql(fx, "UPDATE indexer_source SET held_until = now() + interval '1 hour' WHERE id = %" G_GINT64_FORMAT, id);
	read_board(fx);
	again = page_of(fx, fx->admin);
	assert_contains(again, "Dismissed (1)");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");

	/* A contact that works forgets the hold (as the store writes it): the
	 * notice ends, and the dismissal with it. */
	sql(fx, "UPDATE indexer_source SET state = 'ok', state_message = NULL, held_until = NULL, hold_message = NULL"
		" WHERE id = %" G_GINT64_FORMAT, id);
	read_board(fx);
	read_board(fx);
	works = page_of(fx, fx->admin);
	assert_contains(works, "Nothing needs attention.");
	assert_lacks(works, "Dismissed (");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);
}

/* A series with a destination and one aired, monitored episode whose last
 * search ended @outcome with @person_only releases only a person can grab.
 * Returns the series' id. */
static gint64
add_series_searched(
	Fixture     *fx,
	const gchar *title,
	const gchar *outcome,
	gint         person_only
){
	gint64 root = qi(fx, "SELECT coalesce((SELECT min(id) FROM library_root WHERE media_type = 'tv'), 0)");
	gint64 title_id;
	gint64 series;
	gint64 episode;

	if (root == 0)
		root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	title_id = qi(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) VALUES"
		" ('series', '%s', 2019, 'ended', 'fixture', 's' || nextval('meta_title_id_seq')) RETURNING id", title);
	series = qi(fx, "INSERT INTO series (title_id, quality_profile_id, root_id) VALUES (%" G_GINT64_FORMAT ","
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT ") RETURNING id", title_id, root);
	sql(fx, "INSERT INTO series_season (series_id, number, monitored) VALUES (%" G_GINT64_FORMAT ", 1, true)", series);
	episode = qi(fx, "INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored) VALUES"
		" (%" G_GINT64_FORMAT ", 1, 1, 'Low Water', DATE '2019-03-04', true) RETURNING id", series);
	sql(fx, "INSERT INTO series_search (series_id, cause, finished_at, outcome, outcome_text, scope, episode_id,"
		" n_found, n_person_only) VALUES (%" G_GINT64_FORMAT ", 'scheduled', now(), '%s', 'As the test made it.',"
		" 'episode', %" G_GINT64_FORMAT ", 1, %d)", series, outcome, episode, person_only);
	return series;
}

/* Spec (attention): Releases that wait for Grab this one.  Their own line,
 * apart from the series that need attention for another reason. */
static void
test_series_held(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *bell = NULL;
	g_autofree gchar *gone = NULL;
	AxisTestReply reply;
	gint64 held;

	start(fx);
	held = add_series_searched(fx, "The Morning Tide Report", "none_acceptable", 1);
	clock_at(fx, 6);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	one = page_of(fx, fx->admin);
	assert_contains(one, "id=\"attention-waiting\"");
	assert_contains(one, "1 series has an episode that only a person can take: Axis found a release for it and no "
		"search grabs it by itself. Open the series&#39; manual search and select Grab this one.");
	/* (N2 review, S5.)  Its link lists exactly these episodes. */
	assert_contains(one, "<a href=\"/series/wanted?state=held\">Open TV, Wanted</a>");
	/* One cause, one notice: it is not also a series that needs attention. */
	assert_lacks(one, "needs attention because");
	assert_lacks(one, "Morning Tide");

	/* A series whose releases the profile rejected, none of them a
	 * person's: the general line, and both are counted. */
	add_series_searched(fx, "Gullwing Ferry", "none_acceptable", 0);
	clock_at(fx, 12);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	both = page_of(fx, fx->admin);
	assert_contains(both, "1 series has an episode that only a person can take");
	assert_contains(both, "1 series needs attention because the profile rejected what was found.");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "aria-label=\"2 things need attention\"");

	/* The held line's link shows the held episode and not the other series';
	 * the general list shows both. */
	reply = get_as(fx, fx->admin, "/series/wanted?state=held");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Episodes with a release only you can grab");
	assert_contains(reply.body, "The Morning Tide Report");
	assert_lacks(reply.body, "Gullwing Ferry");
	assert_contains(reply.body, "Waiting for Grab this one</span><span class=\"tab-count\">1</span>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/wanted?state=attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The Morning Tide Report");
	assert_contains(reply.body, "Gullwing Ferry");
	assert_lacks(reply.body, "Waiting for Grab this one");
	axis_test_reply_clear(&reply);

	/* The person grabbed it (a later search of the episode grabbed): gone. */
	sql(fx, "INSERT INTO series_search (series_id, cause, finished_at, outcome, outcome_text, scope, episode_id)"
		" SELECT series_id, 'manual', now(), 'grabbed', 'As the test made it.', 'episode', episode_id FROM series_search"
		" WHERE series_id = %" G_GINT64_FORMAT, held);
	clock_at(fx, 18);
	axis_web_attention_job_ended(fx->web, "tv.search");
	read_board(fx);
	gone = page_of(fx, fx->admin);
	assert_lacks(gone, "only a person can take");
	assert_contains(gone, "1 series needs attention because the profile rejected what was found.");
}

/* Spec (attention): Jobs that failed; Duplicates.  Work that failed for good,
 * never upkeep, as many as the list its link opens; and the number the hourly
 * cleanup left. */
static void
test_jobs_and_duplicates(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *failed = NULL;
	g_autofree gchar *dup = NULL;
	g_autofree gchar *none = NULL;
	AxisTestReply reply;
	const gchar *at;
	guint listed = 0;

	start(fx);
	sql(fx, "INSERT INTO job (kind, queue, state, attempts, max_attempts, finished_at, last_error) VALUES"
		" ('attntest.work', 'default', 'failed', 3, 3, now() - interval '1 hour', 'As the test made it.'),"
		" ('attntest.work', 'default', 'failed', 3, 3, now() - interval '2 days', 'As the test made it.'),"
		/* However old: it is listed, so it is counted. */
		" ('attntest.work', 'default', 'failed', 3, 3, now() - interval '9 days', 'As the test made it.'),"
		/* One that did not fail is neither. */
		" ('attntest.work', 'default', 'succeeded', 1, 3, now() - interval '1 hour', NULL)");
	read_board(fx);
	failed = page_of(fx, fx->admin);
	assert_contains(failed, "id=\"attention-look\"");
	assert_contains(failed, "3 jobs failed for good and were not retried. Retry them, or leave them.");
	assert_contains(failed, "<a href=\"/jobs?state=failed&amp;class=work\">Open Jobs</a>");
	/* (N2 review.)  The two figures agree: the link lists as many jobs as
	 * the notice counts. */
	reply = get_as(fx, fx->admin, "/jobs?state=failed&class=work");
	g_assert_cmpuint(reply.status, ==, 200);
	for (at = strstr(reply.body, "href=\"/jobs/"); at != NULL; at = strstr(at + 1, "href=\"/jobs/"))
		listed += g_ascii_isdigit(at[strlen("href=\"/jobs/")]) ? 1 : 0;
	g_assert_cmpuint(listed, ==, 3);
	axis_test_reply_clear(&reply);
	sql(fx, "DELETE FROM job WHERE kind = 'attntest.work'");

	sql(fx, "INSERT INTO attention_count (kind, count) VALUES ('duplicates', 4)");
	read_board(fx);
	dup = page_of(fx, fx->admin);
	assert_contains(dup, "4 movies or episodes have more than one file, at the last hourly count.");
	assert_contains(dup, "<a href=\"/duplicates\">Open Duplicates</a>");
	assert_lacks(dup, "failed for good");

	sql(fx, "UPDATE attention_count SET count = 0");
	read_board(fx);
	none = page_of(fx, fx->admin);
	assert_contains(none, "Nothing needs attention.");
}

/*
 * Task 3.3a: the series' count where the series are many.  The test tree has
 * no fixture of 1,500 series Axis manages (its 1,500-series fixtures are of
 * files on disk and their identities), so the smallest one is made here, by
 * one statement: 1,500 series of two seasons of twelve aired, monitored
 * episodes each (36,000 episodes), none with a destination, so every series
 * needs attention.  The count is made as the daemon makes it: fifty series a
 * batch, for as long as a reading's own deadline (500 ms), over as many
 * readings as it takes.  What is told: how many readings and how long.  What
 * is held: the count is right, and no reading held the board for longer than
 * the deadline and one batch's two statements (each cancelled by the server
 * after four deadlines).
 */
#define SCALE_SERIES 1500

static void
test_series_count_at_scale(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *want = g_strdup_printf("%d series need attention, each because no library folder is set as the "
		"destination.", SCALE_SERIES);
	AxisAttentionTuning tuning;
	gint64 began;
	gint64 slowest = 0;
	guint readings = 0;
	gboolean counted = FALSE;

	start(fx);
	began = g_get_monotonic_time();
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2019, 'ended', 'fixture', 'z' || i FROM generate_series(1, %d) i"
		" RETURNING id), s AS (INSERT INTO series (title_id, quality_profile_id) SELECT id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced') FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, g, true FROM s,"
		" generate_series(1, 2) g RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT s.id, g, e, 'Part ' || e, DATE '2019-03-04' + e, true FROM s, generate_series(1, 2) g,"
		" generate_series(1, 12) e", SCALE_SERIES);
	g_test_message("the fixture (%d series, %d episodes) took %" G_GINT64_FORMAT " ms to make", SCALE_SERIES,
		SCALE_SERIES * 24, (g_get_monotonic_time() - began) / 1000);

	/* As the daemon reads: a reading gives the series 500 ms. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.deadline_ms = AXIS_ATTENTION_DEADLINE_MS;
	tuning.clock_offset_us = 6 * MINUTES;
	axis_web_attention_set_tuning(fx->web, &tuning);
	axis_web_attention_job_ended(fx->web, "tv.search");
	began = g_get_monotonic_time();
	while (!counted) {
		guint before = axis_web_attention_get_readings(fx->web);
		gint64 reading_began = g_get_monotonic_time();
		g_autofree gchar *page = NULL;

		g_assert_cmpuint(readings, <, 400);
		axis_web_attention_changed(fx->web);
		pump_until_reading(fx, before);
		slowest = MAX(slowest, g_get_monotonic_time() - reading_began);
		readings++;
		page = page_of(fx, fx->admin);
		counted = strstr(page, want) != NULL;
	}
	g_test_message("the count of %d series (3.3a) took %u readings and %" G_GINT64_FORMAT " ms in all; the longest "
		"reading took %" G_GINT64_FORMAT " ms", SCALE_SERIES, readings, (g_get_monotonic_time() - began) / 1000,
		slowest / 1000);
	/* The deadline, and the batch in flight when it passed (two statements
	 * of four deadlines each at the very most; far less here). */
	g_assert_cmpint(slowest / 1000, <, AXIS_ATTENTION_DEADLINE_MS * 9);
}

/* ── After the N2 review: "not known yet" ───────────────────────── */

/* (S1.)  A source that answers "not known yet" for a kind keeps the notice it
 * last gave and every dismissal of it; one that then knows and has none ends
 * both; and "not known yet" with nothing before is no notice. */
static void
test_not_known_keeps(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *kept = NULL;
	g_autofree gchar *ended = NULL;
	g_autofree gchar *nothing = NULL;
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	set_fake(fx, 3, AXIS_ATTENTION_WAITING, 7);
	read_board(fx);
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", "kind=test.thing&subject=&severity=2&count=3&newest=7");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	/* Not known, for several readings: as it was, for both administrators. */
	g_atomic_int_set(&fx->fake.not_known, 1);
	read_board(fx);
	read_board(fx);
	kept = page_of(fx, fx->admin);
	assert_contains(kept, "Dismissed (1)");
	assert_contains(kept, "3 test things wait at ");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);
	bell = bell_of(fx, fx->ops);
	assert_contains(bell, "aria-label=\"1 thing needs attention\"");

	/* Known, and none: the notice has ended, and the dismissal with it. */
	set_fake(fx, 0, AXIS_ATTENTION_WAITING, 0);
	g_atomic_int_set(&fx->fake.not_known, 0);
	read_board(fx);
	read_board(fx);
	ended = page_of(fx, fx->admin);
	assert_contains(ended, "Nothing needs attention.");
	assert_lacks(ended, "Dismissed (");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 0);

	/* Not known with nothing before: nothing. */
	g_atomic_int_set(&fx->fake.not_known, 1);
	read_board(fx);
	nothing = page_of(fx, fx->admin);
	assert_contains(nothing, "Nothing needs attention.");
}

/* (S1; the reviewer's first probe.)  A notice of a disk that was dismissed
 * before the daemon was stopped is still dismissed after it starts: until
 * this daemon has checked the disk, nothing is known of it, and that is not
 * "the notice ended". */
static void
test_dismissed_disk_across_a_start(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *bell = NULL;

	clock_at(fx, 180);
	sql(fx, "INSERT INTO attention_dismissal (user_id, kind, subject, severity, count, newest)"
		" SELECT id, 'volume.offline', '%" G_GINT64_FORMAT "', 3, 1, 99999999999 FROM app_user WHERE role = 'admin'",
		fx->volume_id);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 2);
	start(fx);
	read_board(fx);
	before = page_of(fx, fx->admin);
	assert_lacks(before, "Attic");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 2);

	/* The first check finds it away, as it was left: the notice is there,
	 * and dismissed as it was. */
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	read_board(fx);
	after = page_of(fx, fx->admin);
	assert_contains(after, "Dismissed (1)");
	assert_contains(after, "<bdi>Attic</bdi><span> has been offline for 3 hours.");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 2);
	axis_test_library_plug(fx->t, fx->disk, "attic-away");
}

/* (S1.)  While the series' first count is not finished (it takes several
 * readings), neither of the series' lines is known: what was dismissed
 * before the start is kept, and is in force when the count is done. */
static void
test_series_first_count_keeps_dismissals(Fixture *fx, gconstpointer data)
{
	AxisAttentionTuning tuning;
	g_autofree gchar *early = NULL;
	g_autofree gchar *bell = NULL;
	gboolean counted = FALSE;
	guint readings = 0;

	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2019, 'ended', 'fixture', 'z' || i FROM generate_series(1, 3000) i"
		" RETURNING id), s AS (INSERT INTO series (title_id, quality_profile_id) SELECT id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced') FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, 1, true FROM s RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT s.id, 1, 1, 'Part 1', DATE '2019-03-04', true FROM s");
	sql(fx, "INSERT INTO attention_dismissal (user_id, kind, subject, severity, count, newest)"
		" SELECT id, 'tv.attention', '', 2, 3000, 99999999999 FROM app_user WHERE username = 'root'");
	/* A reading gives the series a tenth of a second: a few batches of fifty
	 * (the other sources' statements take a few milliseconds). */
	memset(&tuning, 0, sizeof(tuning));
	tuning.deadline_ms = 100;
	axis_web_attention_set_tuning(fx->web, &tuning);
	start(fx);
	early = page_of(fx, fx->admin);
	assert_lacks(early, "series need attention");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);

	while (!counted) {
		g_autofree gchar *page = NULL;

		g_assert_cmpuint(readings++, <, 200);
		read_board(fx);
		page = page_of(fx, fx->admin);
		counted = strstr(page, "3000 series need attention") != NULL;
		if (counted)
			assert_contains(page, "Dismissed (1)");
	}
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);
}

/* (S1.)  A disk that is almost full and goes away for a moment: whether it
 * is almost full is not known while it is away, so the notice and its
 * dismissal are as they were when it is back. */
static void
test_low_space_while_away(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *away = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *bell = NULL;
	AxisTestReply reply;

	start(fx);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_LOW_SPACE, "100", &error));
	g_assert_no_error(error);
	read_board(fx);
	form = g_strdup_printf("kind=volume.low_space&subject=%" G_GINT64_FORMAT "&severity=1&count=1&newest=0",
		fx->volume_id);
	reply = post_as(fx, fx->admin, TRUE, "/attention/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*volume 'Attic'*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	read_board(fx);
	read_board(fx);
	away = page_of(fx, fx->admin);
	assert_contains(away, "Dismissed (1)");
	assert_contains(away, "is almost full");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);

	axis_test_library_plug(fx->t, fx->disk, "attic-away");
	g_assert_true(axis_storage_check_all(fx->t->storage, NULL, &error));
	g_assert_no_error(error);
	read_board(fx);
	back = page_of(fx, fx->admin);
	assert_contains(back, "Dismissed (1)");
	bell = bell_of(fx, fx->admin);
	assert_contains(bell, "Nothing needs attention");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM attention_dismissal"), ==, 1);
}

/* ── Words ──────────────────────────────────────────────────────── */

static void
test_duration(void)
{
	static const struct
	{
		gint64       minutes;
		const gchar *words;
	} cases[] = {
		{ 0, "less than a minute" }, { 1, "1 minute" }, { 25, "25 minutes" }, { 119, "119 minutes" },
		{ 120, "2 hours" }, { 47 * 60, "47 hours" }, { 48 * 60, "2 days" }, { 40 * 24 * 60, "40 days" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *words = axis_attention_duration(cases[i].minutes * MINUTES);

		g_assert_cmpstr(words, ==, cases[i].words);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)

	g_test_add_func("/web-attention/duration", test_duration);
	ADD("/web-attention/nothing-waits", test_nothing_waits);
	ADD("/web-attention/not-started", test_not_started);
	ADD("/web-attention/source/disk-offline", test_disk_offline);
	ADD("/web-attention/source/imports", test_imports);
	ADD("/web-attention/source/download-client", test_download_client);
	ADD("/web-attention/source/indexers", test_indexers);
	ADD("/web-attention/source/movies", test_movies);
	ADD("/web-attention/source/series", test_series);
	ADD("/web-attention/source/migration", test_migration);
	ADD("/web-attention/source/provider-refused", test_provider_refused);
	ADD("/web-attention/levels-and-count", test_levels_and_count);
	ADD("/web-attention/dismiss", test_dismiss);
	ADD("/web-attention/dismiss/refusals", test_dismiss_refusals);
	ADD("/web-attention/dismiss/kept", test_dismissals_are_kept);
	ADD("/web-attention/ordinary-user", test_ordinary_user);
	ADD("/web-attention/cost/page-views", test_page_views_cost_nothing);
	ADD("/web-attention/cost/burst", test_burst_is_one_reading);
	ADD("/web-attention/cost/failing-source", test_failing_source);
	ADD("/web-attention/stop/while-blocked", test_stop_while_blocked);
	ADD("/web-attention/live", test_live);
	ADD("/web-attention/review/titles-counted-seldom", test_titles_are_counted_seldom);
	AXIS_TEST_ADD_DB_CASE("/web-attention/review/disk-not-checked-yet", Fixture, OFFLINE_BEFORE_START,
		test_disk_not_checked_yet);
	ADD("/web-attention/review/reason-changes", test_reason_changes_keep_the_clock);
	ADD("/web-attention/review/dismissal-ends", test_dismissal_ends_with_its_notice);
	ADD("/web-attention/review/topic", test_topic_is_administrators);
	ADD("/web-attention/review/switched-on-again", test_switched_on_again);
	ADD("/web-attention/n2/dashboard-card", test_dashboard_card);
	ADD("/web-attention/n2/low-space", test_low_space);
	ADD("/web-attention/n2/imports-read-only", test_imports_read_only);
	ADD("/web-attention/n2/downloads-stuck", test_downloads_stuck);
	ADD("/web-attention/n2/indexer-held", test_indexer_held);
	ADD("/web-attention/n2review/indexer-held-past-its-hour", test_indexer_held_past_its_hour);
	ADD("/web-attention/n2review/not-known-keeps", test_not_known_keeps);
	AXIS_TEST_ADD_DB_CASE("/web-attention/n2review/dismissed-disk-across-a-start", Fixture, OFFLINE_BEFORE_START,
		test_dismissed_disk_across_a_start);
	ADD("/web-attention/n2review/series-first-count", test_series_first_count_keeps_dismissals);
	ADD("/web-attention/n2review/low-space-while-away", test_low_space_while_away);
	ADD("/web-attention/n2/series-held", test_series_held);
	ADD("/web-attention/n2/jobs-and-duplicates", test_jobs_and_duplicates);
	ADD("/web-attention/n2/series-count-at-scale", test_series_count_at_scale);

	return g_test_run();
}
