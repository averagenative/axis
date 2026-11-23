/*
 * test-web-tv-disk.c - the series on disk under Media -> TV: the list with
 * the series identification found (in every view, with its filters, sorts
 * and counts), a series on disk's page with its seasons and episodes and
 * the files on no episode, managing one, the calendar and Wanted, a disk
 * offline, and the list at scale; with the real shell on an ephemeral
 * loopback port, the metadata test provider, a real database and a real
 * volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of identify-series-on-disk: 4.x (the pages), 5.x (managing), 6.x
 * (the calendar and Wanted; looking writes nothing), 8.x (scale).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-identify.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gchar                *series_dir;
	gint64                volume_id;
	gint64                root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisIdentify         *identify;
	AxisWebIdentify      *identify_ui;
	AxisWebHub           *hub;
	AxisWebStorage       *storage_ui;
	AxisTv               *tv;
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
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIdentifyTuning tuning;

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
	fx->series_dir = g_build_filename(fx->disk, "series", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, axis_test_identify_provider, NULL);
	axis_test_identify_posters(fx->fa);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	fx->identify_ui = axis_web_identify_new(fx->identify, fx->quality);
	fx->hub = axis_web_hub_new(NULL);
	fx->storage_ui = axis_web_storage_new(fx->t->storage, fx->hub);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_tv_set_clock(fx->tv, NOW_US);
	axis_tv_set_identify(fx->tv, fx->identify);

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
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), fx->tv, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
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
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 60, "idle-timeout", 60, NULL);
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
	axis_tv_stop(fx->tv);
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
	axis_web_hub_free(fx->hub);
	axis_web_storage_free(fx->storage_ui);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_tv_unref(fx->tv);
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
	axis_web_settings_free(fx->web_settings);
	axis_test_meta_fixture_free(fx->fa);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->series_dir);
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
	const gchar *token_from,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;
	AxisTestReply opened = get_as(fx, cookie, token_from);

	token = axis_test_web_csrf_token(opened.body);
	axis_test_reply_clear(&opened);
	escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "", form != NULL ? form :
		"");
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
	g_autofree gchar *plain = haystack != NULL ? plain_of(haystack) : NULL;

	if (plain != NULL && strstr(plain, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
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
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

static void
put(
	const gchar *dir,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(dir, rel, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autofree gchar *content = g_malloc(MAX(size, 1));
	gsize i;

	g_assert_cmpint(g_mkdir_with_parents(parent, 0755), ==, 0);
	for (i = 0; i < size; i++)
		content[i] = (gchar)('a' + (i * 7 + size) % 26);
	g_assert_true(g_file_set_contents(path, content, (gssize)size, NULL));
}

typedef struct
{
	const gchar *rel;
	gint         minutes;
} Video;

/* Ember Station (2026), airing weekly from 2026-10-05: five episodes on disk
 * (a copy of one), one missing, two not aired yet, a file on no episode;
 * Lantern Row (2015) with one; Copper Valley with no year, held. */
static const Video tree[] = {
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E01 - Chapter 1 [WEBDL-1080p].mkv", 42 },
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E02 - Chapter 2 [WEBDL-1080p].mkv", 42 },
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E02 - Chapter 2 [HDTV-720p].mkv", 42 },
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E03 - Chapter 3 [WEBDL-1080p].mkv", 42 },
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E04 - Chapter 4 [WEBDL-1080p].mkv", 42 },
	{ "Ember Station (2026)/Season 01/Ember Station (2026) - S01E06 - Chapter 6 [WEBDL-1080p].mkv", 42 },
	{ "Ember Station (2026)/Ember Station (2026) - Bonus Reel.mkv", 12 },
	{ "Lantern Row (2015)/Season 01/Lantern Row (2015) - S01E01 - Chapter 1 [HDTV-720p].mkv", 45 },
	{ "Copper Valley/S01/Copper.Valley.S01E01.720p.HDTV.x264-GRP.mkv", 30 },
	{ NULL, 0 }
};

/* The tree written, scanned, probed and identified to the end. */
static void
make_tree(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	guint i;
	guint k;

	for (i = 0; tree[i].rel != NULL; i++)
		put(fx->series_dir, tree[i].rel, 100000 + i * 1000);
	{
		g_autoptr(AxisScanResult) scan = axis_test_library_scan_expect(fx->t, fx->root_id, AXIS_SCAN_COMPLETE,
			AXIS_SCAN_REASON_NONE);
	}
	for (i = 0; tree[i].rel != NULL; i++)
		axis_test_probe_video(fx->t, fx->root_id, tree[i].rel, "matroska", "h264", "1080p", "sdr", "0.1",
			(gint64)tree[i].minutes * 60000, "aac", FALSE);
	for (k = 0; k < 10; k++) {
		AxisIdentifyReport report;

		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		if (report.pending == 0 && report.seasons_pending == 0)
			break;
	}
}

static gint64
title_of(
	Fixture     *fx,
	const gchar *provider_id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM meta_title WHERE kind = 'series' AND provider_id = '%s'",
		provider_id);

	return qi(fx, sql);
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* Every name under the disk with its size and times. */
static void
digest_dir(
	const gchar *dir,
	GString     *out
){
	g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	const gchar *name;
	guint i;

	g_assert_nonnull(handle);
	while ((name = g_dir_read_name(handle)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, compare_names);
	for (i = 0; i < names->len; i++) {
		g_autofree gchar *path = g_build_filename(dir, g_ptr_array_index(names, i), NULL);
		GStatBuf st;

		g_assert_cmpint(g_lstat(path, &st), ==, 0);
		g_string_append_printf(out, "%s %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n", path, (gint64)st.st_size,
			(gint64)st.st_mtime);
		if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
			digest_dir(path, out);
	}
}

static gchar *
digest(Fixture *fx)
{
	GString *out = g_string_new(NULL);

	digest_dir(fx->disk, out);
	return g_string_free(out, FALSE);
}

/* ── The tests ──────────────────────────────────────────────────── */

/*
 * Spec (tv): Every series on disk is listed; nothing is asked of a series on
 * disk.  The list shows each series identification found, with its poster,
 * year, episodes on disk and size, as not managed, in every view, with its
 * filters, sorts and counts; the folders held for a person are counted with
 * a link; looking writes nothing.
 */
static void
test_list(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisTestReply reply;

	make_tree(fx);
	before = digest(fx);

	reply = get_as(fx, fx->viewer, "/series");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Ember Station");
	assert_contains(reply.body, "Lantern Row");
	assert_contains(reply.body, "5 of 6 aired episodes on disk");
	assert_contains(reply.body, "1 of 6 aired episodes on disk");
	assert_contains(reply.body, "/series/on-disk/");
	/* Two series, both in the library; the held folder is said, with the
	 * way to it. */
	assert_contains(reply.body, "tab-count\">2<");
	assert_contains(reply.body, "1 series folder or file on disk is not listed yet: identification was not sure "
		"which series it is");
	assert_contains(reply.body, "href=\"/identify\"");
	assert_contains(reply.body, "Open one and choose Manage this series to have Axis look after it");
	assert_lacks(reply.body, "not recognise");
	axis_test_reply_clear(&reply);

	/* Every view; the overview's line says what it is. */
	reply = get_as(fx, fx->viewer, "/series?view=overview");
	assert_contains(reply.body, "On disk, not managed by Axis, 5 of 6 aired episodes on disk");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series?view=table");
	assert_contains(reply.body, "5 of 6 aired episodes on disk");
	assert_contains(reply.body, ">In library<");
	axis_test_reply_clear(&reply);

	/* By name; by a state no series on disk is in; by title. */
	reply = get_as(fx, fx->viewer, "/series?view=posters&q=ember");
	assert_contains(reply.body, "Ember Station");
	assert_lacks(reply.body, "Lantern Row");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series?state=wanted");
	assert_lacks(reply.body, "Ember Station");
	assert_contains(reply.body, "No series is in this state.");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series?state=in_library&sort=title");
	{
		g_autofree gchar *plain = plain_of(reply.body);
		const gchar *ember = strstr(plain, "Ember Station");
		const gchar *lantern = strstr(plain, "Lantern Row");

		g_assert_nonnull(ember);
		g_assert_nonnull(lantern);
		g_assert_true(ember < lantern);
	}
	axis_test_reply_clear(&reply);

	/* The search result cards say a series on disk is in the library. */
	reply = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);

	/* The Identify page says why the folder is held and what would settle
	 * it (fix-series-match-without-year). */
	{
		g_autofree gchar *root = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, fx->root_id);
		g_autofree gchar *unit = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, qi(fx, "SELECT id FROM"
			" library_identity WHERE rel_path = 'Copper Valley'"));
		const gchar *paths[2];
		guint i;

		paths[0] = root;
		paths[1] = unit;
		for (i = 0; i < G_N_ELEMENTS(paths); i++) {
			reply = get_as(fx, fx->admin, paths[i]);
			g_assert_cmpuint(reply.status, ==, 200);
			assert_contains(reply.body, "Only 1 numbered episode is on disk");
			assert_contains(reply.body, "To settle it: Confirm the series here if it is the right one. Axis links it "
				"by itself once at least 3 of its numbered episodes are on disk, or once the folder's name gives its "
				"year.");
			axis_test_reply_clear(&reply);
		}
	}

	/* Looking made nothing and changed nothing. */
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind LIKE 'tv.%'"), ==, 0);
}

/*
 * Spec (tv): A series on disk has a page: its seasons and episodes, which
 * are on disk (file, quality, size, copies), which are missing and which
 * have not aired; its files on no episode with their reasons; its folders.
 * An administrator can manage it from there; anyone else is told who can.
 */
static void
test_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = NULL;
	g_autofree gchar *title_page = NULL;
	AxisTestReply reply;
	gint64 ember;

	make_tree(fx);
	ember = title_of(fx, "7008");
	page = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT, ember);
	title_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, ember);

	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Ember Station (2026)");
	assert_contains(reply.body, "On disk, not managed by Axis");
	assert_contains(reply.body, "5 of 6 aired episodes on disk");
	assert_contains(reply.body, "5 of 8 episodes on disk, 2 not aired yet");
	assert_contains(reply.body, "Ember Station (2026) - S01E01 - Chapter 1 [WEBDL-1080p].mkv");
	assert_contains(reply.body, "WEB-DL 1080p");
	assert_contains(reply.body, "2 copies");
	assert_contains(reply.body, ">Missing<");
	assert_contains(reply.body, ">Not aired<");
	assert_contains(reply.body, "Files on disk on no episode");
	assert_contains(reply.body, "Bonus Reel.mkv");
	assert_contains(reply.body, "Its name gives no season and episode number.");
	assert_contains(reply.body, "Folder of this series on disk");
	assert_contains(reply.body, "An administrator can have Axis manage it from this page.");
	assert_lacks(reply.body, "Manage this series");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "Manage this series");
	assert_contains(reply.body, "Future episodes only: new ones are searched for as they air");
	axis_test_reply_clear(&reply);

	/* The title page: what it is, the way to its episodes, the form. */
	reply = get_as(fx, fx->admin, title_page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "On disk, not managed by Axis");
	assert_contains(reply.body, "Episodes on disk");
	assert_contains(reply.body, "Manage this series");
	axis_test_reply_clear(&reply);

	/* Not a series on disk: not found. */
	reply = get_as(fx, fx->viewer, "/series/on-disk/999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series"), ==, 0);
}

/*
 * Spec (tv): Managing a series on disk: its episodes on disk count as in the
 * library, only missing monitored episodes are wanted, nothing is searched
 * for; the calendar and Wanted list no episode on disk and nothing of a
 * series on disk; removing it leaves it on disk, listed again.
 */
static void
test_manage(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *series_page = NULL;
	g_autofree gchar *remove = NULL;
	AxisTestReply reply;
	gint64 ember;
	gint64 series;

	make_tree(fx);
	ember = title_of(fx, "7008");
	page = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT, ember);

	/* Nothing of a series on disk is on the calendar or Wanted. */
	reply = get_as(fx, fx->viewer, "/series/calendar");
	assert_lacks(reply.body, "Ember Station");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series/wanted");
	assert_lacks(reply.body, "Ember Station");
	axis_test_reply_clear(&reply);

	/* A viewer may not; an administrator manages it, every episode. */
	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&monitor=all&manage=yes", ember,
		fx->root_id);
	reply = post_as(fx, fx->viewer, page, "/series", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, "/series", form);
	g_assert_cmpuint(reply.status, ==, 303);
	series = qi(fx, "SELECT id FROM series");
	series_page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	location = g_strdup_printf("%s?done=managed", series_page);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE identity_id IS NOT NULL"), ==, 6);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	reply = get_as(fx, fx->admin, location);
	assert_contains(reply.body, "Axis manages this series now. Its episodes on disk are in the library");
	assert_contains(reply.body, "WEB-DL 1080p");
	assert_contains(reply.body, "2 copies");
	assert_contains(reply.body, "Files on disk on no episode");
	axis_test_reply_clear(&reply);

	/* Wanted: the missing aired episode, never one on disk; one below its
	 * profile's cutoff (every episode is monitored) under Upgrades. */
	reply = get_as(fx, fx->viewer, "/series/wanted");
	assert_contains(reply.body, "S01E05");
	assert_lacks(reply.body, "S01E01");
	assert_lacks(reply.body, "S01E06");
	assert_contains(reply.body, "/series/wanted?state=upgrade_wanted");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series/wanted?state=upgrade_wanted");
	assert_contains(reply.body, "S01E01");
	assert_lacks(reply.body, "S01E05");
	axis_test_reply_clear(&reply);
	/* The calendar (a week back to two ahead): what is to come, not the
	 * episode on disk. */
	reply = get_as(fx, fx->viewer, "/series/calendar");
	assert_contains(reply.body, "S01E07");
	assert_contains(reply.body, "S01E08");
	assert_lacks(reply.body, "S01E06");
	axis_test_reply_clear(&reply);
	/* The page on disk is the managed one now. */
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, series_page);
	axis_test_reply_clear(&reply);

	/* Removed: no file touched, a series on disk again. */
	remove = g_strdup_printf("%s/remove", series_page);
	reply = post_as(fx, fx->admin, series_page, remove, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	{
		g_autofree gchar *back = g_strdup_printf("%s?done=unmanaged", page);

		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, back);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, back);
		assert_contains(reply.body, "Axis no longer manages this series. No file was touched");
		assert_contains(reply.body, "5 of 6 aired episodes on disk");
		axis_test_reply_clear(&reply);
	}
}

/*
 * Spec (tv): A disk that is away never takes a series out of the list: it
 * stays, and says its disk is offline.
 */
static void
test_offline(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	AxisTestReply reply;

	make_tree(fx);
	page = g_strdup_printf("/series/on-disk/%" G_GINT64_FORMAT, title_of(fx, "7008"));
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->viewer, "/series?view=overview");
	assert_contains(reply.body, "Ember Station");
	assert_contains(reply.body, "a disk offline");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_contains(reply.body, "Its disk, Disk one, is offline: it stays listed, since a disk that is away never "
		"means a series is gone.");
	assert_contains(reply.body, ">Disk offline<");
	axis_test_reply_clear(&reply);
	axis_test_library_plug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);
}

/* ── At scale ───────────────────────────────────────────────────── */

#define SCALE_SERIES  1500
#define SCALE_SEASONS 4
#define SCALE_EPISODES 10
#define SCALE_HELD    300

/*
 * SCALE_SERIES made-up series on disk, made by SQL as identification leaves
 * them: series j ("Shelf Series j") is the folder "Shelf Series j" with
 * SCALE_SEASONS seasons of SCALE_EPISODES episodes, every one on disk (one
 * file each, j KiB), every tenth with a second copy of its first episode,
 * every twenty-fifth with a file on no episode; SCALE_HELD folders held for
 * a person.  Every season's episodes are fetched (aired in the 2000s).
 */
static guint
make_scale(Fixture *fx)
{
	g_free(q(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, poster_ref, provider, provider_id,"
		" detail_fetched_at, name_keys) SELECT 'series', 'Shelf Series ' || j, 1960 + j %% 60, 'ended',"
		" '/gen' || (j %% 6) || '.png', 'fixture', 'shelf' || j, now(), 'shelfseries' || j"
		" FROM generate_series(1, %d) j RETURNING id)"
		" INSERT INTO meta_season (title_id, number, episode_count, episodes_fetched_at)"
		" SELECT t.id, s, %d, now() FROM t, generate_series(1, %d) s RETURNING 1", SCALE_SERIES, SCALE_EPISODES,
		SCALE_SEASONS));
	g_free(q(fx, "INSERT INTO meta_episode (title_id, season_number, number, name, air_date, provider_id)"
		" SELECT ms.title_id, ms.number, e, 'Chapter ' || e, DATE '2001-01-01' + (ms.number * 100 + e * 7),"
		" 'x' || ms.title_id || 'n' || ms.number || 'e' || e FROM meta_season ms, generate_series(1, %d) e"
		" RETURNING 1", SCALE_EPISODES));
	g_free(q(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence, title_id,"
		" linked_how, created_at) SELECT %" G_GINT64_FORMAT ", library_path_hash(t.title), 'folder', t.title,"
		" 'linked', 'sure', t.id, 'auto', now() - make_interval(secs => substr(t.provider_id, 6)::int + 0.5)"
		" FROM meta_title t WHERE t.provider_id LIKE 'shelf%%' RETURNING 1", fx->root_id));
	/* One file for each episode; a copy of S01E01 of every tenth; a file on
	 * no episode of every twenty-fifth. */
	g_free(q(fx, "WITH want AS (SELECT i.id AS identity_id, i.rel_path AS dir, substr(t.provider_id, 6)::int AS j,"
		"   me.season_number AS s, me.number AS e, '' AS tag FROM library_identity i JOIN meta_title t ON t.id = i.title_id"
		"   JOIN meta_episode me ON me.title_id = t.id"
		"  UNION ALL SELECT i.id, i.rel_path, substr(t.provider_id, 6)::int, 1, 1, ' [HDTV-720p]' FROM library_identity i"
		"   JOIN meta_title t ON t.id = i.title_id WHERE substr(t.provider_id, 6)::int %% 10 = 0"
		"  UNION ALL SELECT i.id, i.rel_path, substr(t.provider_id, 6)::int, 0, 0, ' Bonus' FROM library_identity i"
		"   JOIN meta_title t ON t.id = i.title_id WHERE substr(t.provider_id, 6)::int %% 25 = 0),"
		" f AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec, ctime_sec,"
		"   ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation, last_seen_generation,"
		"   changed_generation) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		"   w.dir || '/Season 0' || w.s || '/' || w.dir || ' - S0' || w.s || 'E' || lpad(w.e::text, 2, '0') || w.tag"
		"   || '.mkv', 'video', w.j * 1024, 1, 0, 1, 0, 5000000 + row_number() OVER (), 1, false, false, false, 1, 1, 1"
		"   FROM want w RETURNING id, rel_path),"
		" lif AS (INSERT INTO library_identity_file (file_id, identity_id, best, quality, episode_code, episode_text)"
		"   SELECT f.id, i.id, false, 'webdl_1080p', CASE WHEN f.rel_path LIKE '%% Bonus.mkv' THEN 'no_episode' END,"
		"   CASE WHEN f.rel_path LIKE '%% Bonus.mkv' THEN 'its name gives no season and episode number' END"
		"   FROM f JOIN library_identity i ON f.rel_path LIKE i.rel_path || '/%%' RETURNING file_id)"
		" SELECT count(*) FROM lif", fx->root_id, fx->volume_id));
	g_free(q(fx, "INSERT INTO library_identity_episode (file_id, position, season_number, number)"
		" SELECT lif.file_id, 0, substring(f.rel_path FROM ' - S0([0-9])E')::int,"
		" substring(f.rel_path FROM ' - S0[0-9]E([0-9]+)')::int FROM library_identity_file lif"
		" JOIN library_file f ON f.id = lif.file_id WHERE lif.episode_code IS NULL RETURNING 1"));
	axis_test_db_exec(fx->t->db, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state,"
		" confidence) SELECT (SELECT id FROM library_root LIMIT 1), library_path_hash('held/' || k), 'folder',"
		" 'held/' || k, 'review', 'unknown' FROM generate_series(1, " G_STRINGIFY(SCALE_HELD) ") k");
	axis_test_db_exec(fx->t->db, "ANALYZE");
	return (guint)qi(fx, "SELECT count(*) FROM library_identity_file");
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

static gdouble
plan_rows(
	Fixture     *fx,
	const gchar *titles,
	const gchar *relation
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *plan = NULL;

	g_assert_true(axis_tv_explain_on_disk(fx->tv, titles, &plan, &error));
	g_assert_no_error(error);
	g_assert_true(json_parser_load_from_data(parser, plan, -1, NULL));
	return rows_read(json_object_get_object_member(json_array_get_object_element(json_node_get_array(
		json_parser_get_root(parser)), 0), "Plan"), relation);
}

/*
 * Spec (library-identification): A library where no series' folder has a
 * year links by itself.  What identification made of the scale library by
 * SQL goes; the index stays, its videos probed at 40 minutes; then runs, as
 * the minute's schedule would queue them, until nothing is left: every
 * folder is linked as sure, none waits for a person, and the series are
 * listed again.  The provider is the test's (Shelf Series j: four seasons of
 * ten, no runtime), at 50 requests a second (the most its setting allows),
 * with 500 units a run.
 */
static void
identify_scale(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	guint runs = 0;
	guint examined = 0;

	axis_test_db_exec(fx->t->db, "DELETE FROM library_identity");
	axis_test_db_exec(fx->t->db, "DELETE FROM library_identity_scope");
	axis_test_db_exec(fx->t->db, "INSERT INTO library_root_state (root_id, generation, complete_generation)"
		" SELECT id, 1, 1 FROM library_root ON CONFLICT (root_id) DO UPDATE SET generation = greatest("
		"library_root_state.generation, 1), complete_generation = greatest(library_root_state.complete_generation, 1)");
	axis_test_db_exec(fx->t->db, "INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, container, duration_ms, resolution_class) SELECT f.id, 'ok', 1,"
		" f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, 'matroska', 40 * 60000, '1080p'"
		" FROM library_file f ON CONFLICT (file_id) DO NOTHING");
	g_assert_true(axis_settings_set(fx->settings, AXIS_IDENTIFY_SETTING_PER_RUN, "500", &error));
	g_assert_no_error(error);

	/* (Until no folder is left; the seasons the series need are fetched by
	 * the runs after, 200 at a time, as /identify/tv/series shows.) */
	for (;;) {
		AxisIdentifyReport report;

		g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_null(report.skipped);
		g_assert_false(report.provider_down);
		runs++;
		examined += report.examined;
		g_test_message("run %u: %u looked at (%u linked, %u to review), %u left; %u seasons fetched, %u left", runs,
			report.examined, report.linked, report.review, report.pending, report.seasons, report.seasons_pending);
		if (report.pending == 0)
			break;
		g_assert_cmpuint(runs, <, 10);
	}
	g_test_message("identification of %u folders with no year: %u runs, %" G_GINT64_FORMAT " ms", examined, runs,
		(g_get_monotonic_time() - start) / 1000);
	g_assert_cmpuint(examined, ==, SCALE_SERIES);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_identity WHERE state = 'linked' AND confidence = 'sure'"
		" AND linked_how = 'auto'"), ==, SCALE_SERIES);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_identity WHERE state <> 'linked'"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_identity WHERE reasons @> '[{\"c\": \"series_fits\"}]'"), ==,
		SCALE_SERIES);
	{
		AxisTestReply reply;

		/* (The test's own connections, kept alive while the main thread ran
		 * identification for minutes, are closed first: a request on one the
		 * server dropped meanwhile would wait for ever.) */
		soup_session_abort(fx->session);
		reply = get_as(fx, fx->viewer, "/series?state=in_library");
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "tab-count\">1,500<");
		assert_lacks(reply.body, "not listed yet");
		axis_test_reply_clear(&reply);
	}
}

/*
 * Spec (tv): The TV list works for a large library.  1,500 series on disk
 * and 60,000 episode files: every page costs the same few statements in
 * every view, whatever the number of series or posters; paging reaches
 * every series once; the series on disk are read in one pass, and a page's
 * episodes for its own series alone.
 */
static void
test_scale(Fixture *fx, gconstpointer data)
{
	static const gchar *const views[] = { "/series?view=posters", "/series?view=table", "/series?view=posters",
		"/series?view=table", "/series?view=overview" };
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GRegex) entry = g_regex_new("/series/on-disk/([0-9]+)\"", 0, 0, NULL);
	g_autoptr(GRegex) next = g_regex_new("after=(-?[0-9]+)\"[^>]*>Next page<", 0, 0, NULL);
	g_autoptr(GString) page_ids = g_string_new("{");
	g_autofree gchar *cursor = NULL;
	guint spent[G_N_ELEMENTS(views)];
	guint files;
	guint pages = 0;
	gint64 start;
	gdouble read;
	guint v;

	files = make_scale(fx);
	g_test_message("%u series on disk, %u episode files", SCALE_SERIES, files);
	g_assert_cmpuint(files, >=, SCALE_SERIES * SCALE_SEASONS * SCALE_EPISODES);
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (id, username, password_hash, role) OVERRIDING SYSTEM VALUE"
		" VALUES (1, 'root', '$argon2id$test', 'admin'), (2, 'viewer', '$argon2id$test', 'user')");

	/* The same statements for a page of 60 posters as for a table of 100. */
	for (v = 0; v < G_N_ELEMENTS(views); v++) {
		guint before = axis_db_pool_get_statements_sent(fx->t->pool);
		AxisTestReply reply;

		start = g_get_monotonic_time();
		reply = get_as(fx, fx->admin, views[v]);
		g_assert_cmpuint(reply.status, ==, 200);
		spent[v] = axis_db_pool_get_statements_sent(fx->t->pool) - before;
		g_test_message("%s: %u statements, %" G_GINT64_FORMAT " ms", views[v], spent[v],
			(g_get_monotonic_time() - start) / 1000);
		assert_contains(reply.body, "40 of 40 aired episodes on disk");
		assert_contains(reply.body, "300 series folders and files on disk are not listed yet");
		axis_test_reply_clear(&reply);
	}
	/* (The first of each view also keeps the account's choice.) */
	g_assert_cmpuint(spent[2], ==, spent[3]);
	g_assert_cmpuint(spent[4], <=, spent[2] + 1);
	g_assert_cmpuint(spent[2], <=, 14);

	/* Every series once, a page of 100 at a time, by title. */
	for (;;) {
		g_autofree gchar *path = cursor != NULL ? g_strdup_printf("/series?view=table&sort=title&after=%s", cursor) :
			g_strdup("/series?view=table&sort=title");
		g_autoptr(GMatchInfo) match = NULL;
		g_autoptr(GMatchInfo) more = NULL;
		AxisTestReply reply = get_as(fx, fx->viewer, path);
		guint on_page = 0;

		g_assert_cmpuint(reply.status, ==, 200);
		g_regex_match(entry, reply.body, 0, &match);
		while (g_match_info_matches(match)) {
			g_autofree gchar *id = g_match_info_fetch(match, 1);

			if (!g_hash_table_contains(seen, id)) {
				if (pages == 0 && on_page < 60)
					g_string_append_printf(page_ids, "%s%s", on_page > 0 ? "," : "", id);
				g_hash_table_add(seen, g_steal_pointer(&id));
				on_page++;
			}
			g_match_info_next(match, NULL);
		}
		pages++;
		g_clear_pointer(&cursor, g_free);
		/* The pager's Next. */
		if (g_regex_match(next, reply.body, 0, &more))
			cursor = g_match_info_fetch(more, 1);
		axis_test_reply_clear(&reply);
		if (cursor == NULL || pages > 40)
			break;
	}
	g_assert_cmpuint(g_hash_table_size(seen), ==, SCALE_SERIES);
	g_assert_cmpuint(pages, ==, (SCALE_SERIES + 99) / 100);

	/* The plans: the list reads the linked units' files once (not once per
	 * series); a page's counts read its own series' episode files alone. */
	read = plan_rows(fx, NULL, "library_identity_file");
	g_test_message("the list's pass read %.0f of %u file rows", read, files);
	g_assert_cmpfloat(read, <=, files);
	g_string_append_c(page_ids, '}');
	/* (Each of the page's files is looked at four times: by its numbers and
	 * by its date, for the episodes on disk and for the files on none.) */
	read = plan_rows(fx, page_ids->str, "library_identity_episode");
	g_test_message("a page's counts read %.0f episode rows (60 series of %u)", read, SCALE_SERIES);
	g_assert_cmpfloat(read, <=, 60.0 * (SCALE_SEASONS * SCALE_EPISODES + 2) * 4);
	g_assert_cmpfloat(read, <, files / 4);

	/* The same library identified for real: no folder's name has a year
	 * ("Shelf Series j", as series' folders are named), and every one is
	 * linked by itself, by its episodes (fix-series-match-without-year). */
	identify_scale(fx);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/tv/disk/list", Fixture, NULL, test_list);
	AXIS_TEST_ADD_DB_CASE("/web/tv/disk/page", Fixture, NULL, test_page);
	AXIS_TEST_ADD_DB_CASE("/web/tv/disk/manage", Fixture, NULL, test_manage);
	AXIS_TEST_ADD_DB_CASE("/web/tv/disk/offline", Fixture, NULL, test_offline);
	AXIS_TEST_ADD_DB_CASE("/web/tv/disk/scale", Fixture, NULL, test_scale);
	return g_test_run();
}
