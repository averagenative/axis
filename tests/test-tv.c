/*
 * test-tv.c - the TV module against the metadata test provider, the fake
 * Prowlarr and the fixture download client together, on a real database
 * and a real volume, with real jobs and real imports
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The series is Harbour Lights (tests/fixtures/meta, 202): specials, two
 * seasons, a two-part episode aired on one day, an episode that airs next
 * week, one with no air date.  The module's clock stands at 2026-11-14
 * 12:00 UTC.
 *
 * Tasks of add-tv-module:
 *   6.1 end to end: added, an aired episode found, grabbed, followed,
 *       imported and named; a season pack fills a season
 *   6.2 the monitoring choices on add; a new episode from a refresh takes
 *       its season's monitoring; specials unmonitored; renumbered episodes
 *       keep their rows; episodes known by their place
 *   6.3 a pack only when it fits; the fallback to episodes; a double
 *       episode satisfies both
 *   6.4 numbered beyond the series' data: rejected and counted; absolute
 *       only: rejected; another series (or one of the same name) never
 *       grabbed
 *   6.5 overlapping releases: one grab; two searches at once: one grab
 *   6.6 not searched before air date plus delay; no air date: never
 *       automatically
 *   6.7 the import, file by file: each file to its episodes, an extra
 *       wanted episode imported, unmatched files left, an upgrade of one
 *       episode leaves the others; a failure on one file (partly imported,
 *       retried), a run that dies (each file once); Usenet removed only
 *       when every wanted file is in; a blocked download does not hold up
 *       another of the series
 *   6.8 the names (the name builder's own cases are in test-import-name)
 *   3.4 the scheduler: recent first, the backlog, pacing, the caps; a
 *       failed grab blocklisted and sought again
 *   6.10 (half) every action in the series' history with the user
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-newznab.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "import/axis-import-private.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"

#define KEY      "prowlarr-key-for-series-5e2a9c13"
/* add-indexer-newznab: a native Newznab indexer, and no Prowlarr. */
#define NATIVE_ONLY 1
#define NZB_KEY  "nzb-key-for-series-0d7b2e44"
#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define TOOL     "tests/fixtures/import/fake-ffprobe.sh"

#define EPISODE_LINE "AXISFAKE duration=2700 width=1920 height=1080 vcodec=h264 acodec=eac3 channels=6"
#define SAMPLE_LINE  "AXISFAKE duration=50 width=1920 height=1080 vcodec=h264"

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)
#define HOUR_US ((gint64)3600 * G_USEC_PER_SEC)

#define SERIES_FOLDER "Harbour Lights (2024) {tvdb-360002}"

typedef struct _Fixture Fixture;

/* The import's test hook: @verdict at the @nth (1-based; 0: every) step
 * named @at, of a file whose path holds @path_has (when set). */
typedef struct
{
	Fixture               *fx;
	const gchar           *at;
	const gchar           *path_has;
	AxisImportHookVerdict  verdict;
	guint                  nth;
	guint                  seen;
	guint                  fired;
} Hook;

struct _Fixture
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	gint64                films_root;
	gint64                downloads_root;
	gchar                *shows;
	gchar                *downloads;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisTestProwlarr     *prowlarr;
	AxisTestNewznab      *nzb;
	AxisIndexers         *indexers;
	gint64                source_id;
	AxisTestDownloadFake *fake;
	AxisDownloads        *dl;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisProbe            *probe;
	AxisImport           *import;
	AxisTv               *tv;
	gchar                *tool[3];
	Hook                  hook;
	AxisJobRegistry      *registry;
	AxisJobQueue         *queue;
};

/* ── Setting up ─────────────────────────────────────────────────── */

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, 200, "application/json", body, NULL);
}

static AxisImportHookVerdict hook_fn(const gchar *step, gint64 import_id, gint64 file_id, gpointer data);

#define SEARCH_SERIES "{\"results\":[{\"id\":\"202\",\"kind\":\"series\",\"title\":\"Harbour Lights\",\"year\":2024," \
	"\"ids\":{\"tvdb\":\"360002\"}},{\"id\":\"201\",\"kind\":\"series\",\"title\":\"Fixture Street\",\"year\":2021," \
	"\"ids\":{\"tvdb\":\"350001\"}}]}"

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
	AxisProbeTuning probe_tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	fx->films_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->downloads_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "downloads", AXIS_MEDIA_DOWNLOADS);
	fx->shows = g_build_filename(fx->disk, "series", NULL);
	fx->downloads = g_build_filename(fx->disk, "downloads", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
	/* Writing is allowed in these tests (the default is read-only). */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);

	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &indexer_tuning);

	fx->dl = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->dl, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(fx->dl, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->dl, &download_tuning);

	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_QUALITY_MODULE_ID, axis_quality_store_settings(), &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_TV_MODULE_ID, axis_tv_get_module()->settings, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IMPORT_MODULE_ID, axis_import_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_no_error(error);

	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->tool[0] = g_strdup("/bin/sh");
	fx->tool[1] = g_canonicalize_filename(TOOL, NULL);
	fx->tool[2] = NULL;
	memset(&probe_tuning, 0, sizeof(probe_tuning));
	probe_tuning.tool_command = (const gchar *const *)fx->tool;
	axis_probe_set_tuning(fx->probe, &probe_tuning);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->dl, fx->probe);
	fx->hook.fx = fx;
	axis_import_set_hook(fx->import, hook_fn, &fx->hook);

	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->dl, fx->t->storage, fx->quality);
	axis_tv_set_import(fx->tv, fx->import);
	axis_tv_set_clock(fx->tv, NOW_US);

	/* The standard titles, Harbour Lights, and a search for series that
	 * finds it and Fixture Street (the fake answers with the route added
	 * last). */
	axis_test_meta_fixture_standard(fx->fa);
	axis_test_plex_route_file(fx->fa->api, "/title/series/202", -1, "tests/fixtures/meta/title-series-202.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/0", -1, "tests/fixtures/meta/season-202-0.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/1", -1, "tests/fixtures/meta/season-202-1.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/2", -1, "tests/fixtures/meta/season-202-2.json");
	set_route(fx, "/search/series", SEARCH_SERIES);

	fx->prowlarr = axis_test_prowlarr_new(KEY, "");
	if (GPOINTER_TO_UINT(data) == NATIVE_ONLY) {
		fx->nzb = axis_test_newznab_new(NZB_KEY, FALSE, "");
		base = axis_test_newznab_base(fx->nzb);
		memset(&source, 0, sizeof(source));
		source.name = "NewsOwl";
		source.kind = AXIS_INDEXER_NEWZNAB_NAME;
		source.address = base;
		source.enabled = TRUE;
		source.secret = NZB_KEY;
	} else {
		base = axis_test_prowlarr_base(fx->prowlarr);
		memset(&source, 0, sizeof(source));
		source.name = "Prowlarr";
		source.kind = AXIS_INDEXER_PROWLARR_NAME;
		source.address = base;
		source.enabled = TRUE;
		source.secret = KEY;
	}
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &source, &fx->source_id, &error));
	g_assert_no_error(error);
	g_clear_pointer(&base, g_free);

	fx->fake = axis_test_download_fake_new(PASSWORD);
	/* As Deluge and qBittorrent do: a removal of a torrent the client does
	 * not have says so (a torrent's client lets go of it before its files are
	 * moved, and is asked until it says it no longer has it). */
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	base = axis_test_download_fake_base(fx->fake);
	memset(&client, 0, sizeof(client));
	client.name = "Usenet";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->dl, 0, &client, &fx->client_id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, "/data/downloads", fx->volume_id, "downloads",
		&error));
	g_assert_no_error(error);
}

static void
stop_queue(Fixture *fx)
{
	if (fx->queue == NULL)
		return;
	axis_job_queue_stop(fx->queue, 5000);
	g_clear_object(&fx->queue);
	g_clear_pointer(&fx->registry, axis_job_registry_unref);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	stop_queue(fx);
	axis_tv_set_claim_hook(fx->tv, NULL, NULL);
	axis_import_set_hook(fx->import, NULL, NULL);
	axis_tv_unref(fx->tv);
	axis_import_unref(fx->import);
	axis_probe_unref(fx->probe);
	axis_downloads_stop(fx->dl);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_downloads_unref(fx->dl);
	axis_indexers_unref(fx->indexers);
	axis_meta_unref(fx->meta);
	axis_quality_store_unref(fx->quality);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_prowlarr_free(fx->prowlarr);
	axis_test_newznab_free(fx->nzb);
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->tool[0]);
	g_free(fx->tool[1]);
	g_free(fx->disk);
	g_free(fx->shows);
	g_free(fx->downloads);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* The module's own jobs on a real queue. */
static void
start_queue(Fixture *fx)
{
	const AxisJobHandlerInfo *handler;
	g_autoptr(GError) error = NULL;

	fx->registry = axis_job_registry_new();
	for (handler = axis_tv_get_module()->job_handlers; handler->kind != NULL; handler++)
		g_assert_true(axis_job_registry_add(fx->registry, handler, fx->tv, &error));
	fx->queue = axis_job_queue_new(fx->t->pool, fx->registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

/* ── Helpers ────────────────────────────────────────────────────── */

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

static void
exec(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
exec(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	axis_test_db_exec(fx->t->db, sql);
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

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

static void
set(
	Fixture     *fx,
	const gchar *key,
	const gchar *value
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, key, value, &error));
	g_assert_no_error(error);
}

static gint64
title_of(
	Fixture       *fx,
	AxisMetaKind   kind,
	const gchar   *provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 id;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", kind, provider_id, 5000, &answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

static gint64
add_series_full(
	Fixture       *fx,
	const gchar   *provider_id,
	AxisTvMonitor  monitor,
	gboolean       specials,
	gboolean       search_now
){
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, AXIS_META_KIND_SERIES, provider_id);
	spec.root_id = fx->root_id;
	spec.monitor = monitor;
	spec.specials = specials;
	spec.search_now = search_now;
	spec.user = "root";
	g_assert_true(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

static gint64
add_series(Fixture *fx)
{
	return add_series_full(fx, "202", AXIS_TV_MONITOR_ALL, FALSE, FALSE);
}

static gint64
episode_id(
	Fixture *fx,
	gint64   series,
	gint     season,
	gint     number
){
	return qi(fx, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT " AND season_number = %d"
		" AND number = %d AND gone_at IS NULL", series, season, number);
}

/* The monitored episodes, "S01E01,S01E02", or NULL for none. */
static gchar *
monitored(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT string_agg('S' || lpad(season_number::text, 2, '0') || 'E' || lpad(number::text, 2, '0'), ','"
		" ORDER BY season_number, number) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND monitored AND gone_at IS NULL", series);
}

static AxisTvState
state_of(
	Fixture  *fx,
	gint64    series_id,
	gint      season,
	gint      number,
	gchar   **reason
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GError) error = NULL;
	AxisTvRules rules;
	guint i;

	g_assert_true(axis_tv_get(fx->tv, series_id, &series, &episodes, &error));
	g_assert_no_error(error);
	axis_tv_get_rules(fx->tv, &rules);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (e->season == season && e->number == number && !e->gone)
			return axis_tv_episode_derive(e, series, &rules, reason);
	}
	g_error("no episode S%02dE%02d", season, number);
	return AXIS_TV_WANTED;
}

static void
assert_ep(
	Fixture     *fx,
	gint64       series,
	gint         season,
	gint         number,
	AxisTvState  expected,
	const gchar *in_reason
){
	g_autofree gchar *reason = NULL;
	AxisTvState state = state_of(fx, series, season, number, &reason);

	if (state != expected)
		g_error("S%02dE%02d: expected %s, got %s (%s)", season, number, axis_tv_state_name(expected),
			axis_tv_state_name(state), reason != NULL ? reason : "");
	if (in_reason != NULL)
		assert_contains(reason, in_reason);
}

typedef struct
{
	const gchar *title;
	gint64       size;
} Rel;

/* What the fake Prowlarr answers to every search (of @type: "tvsearch",
 * "search"; NULL: every type) from now on: Usenet releases. */
static void
set_releases_of(
	Fixture     *fx,
	const gchar *type,
	const Rel   *rels,
	guint        n
){
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/%u\",\"title\":\"%s\",\"size\":%"
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"NewsOwl\",\"publishDate\":\"2026-11-13T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=hearth-nzb\",\"magnetUrl\":null,\"protocol\":"
			"\"usenet\",\"tvdbId\":360002,\"seeders\":null,\"leechers\":null,\"categories\":[{\"id\":5040}]}",
			i > 0 ? "," : "", i, rels[i].title, rels[i].size, origin, KEY, i);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, type, body->str);
	/* A new answer: what the layer kept of the old one is forgotten. */
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

static void
set_releases(
	Fixture   *fx,
	const Rel *rels,
	guint      n
){
	set_releases_of(fx, NULL, rels, n);
}

static AxisAcquireSearchReport
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
	return report;
}

static void
poll(Fixture *fx)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->dl, fx->client_id, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_true(report.reached);
}

static AxisAcquireFollowReport
follow(Fixture *fx)
{
	AxisAcquireFollowReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_tv_run_follow(fx->tv, &report, &error));
	g_assert_no_error(error);
	return report;
}

static AxisTvScheduleReport
schedule(Fixture *fx)
{
	AxisTvScheduleReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
	g_assert_no_error(error);
	return report;
}

static guint
n_adds(Fixture *fx)
{
	return axis_test_download_fake_count(fx->fake, "add", NULL);
}

/* Whether the candidate named @name of the latest search has a reason
 * coded @code. */
static void
assert_reason(
	Fixture     *fx,
	const gchar *name,
	const gchar *code
){
	assert_qf(fx, "1", "SELECT count(*) FROM series_candidate c WHERE c.name = '%s' AND c.search_id = (SELECT max(id)"
		" FROM series_search) AND NOT c.accepted AND EXISTS (SELECT 1 FROM jsonb_array_elements(c.reasons) r"
		" WHERE r->>'c' = '%s')", name, code);
}

/* The queued searches, by their keys without the series: "episode:12",
 * "season:1", "series", in the order queued. */
static gchar *
queued_searches(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT string_agg(substr(dedupe_key, length('tv.search:%" G_GINT64_FORMAT ":') + 1), ',' ORDER BY id)"
		" FROM job WHERE kind = 'tv.search' AND state = 'queued'", series);
}

static gchar *
episode_key(
	Fixture *fx,
	gint64   series,
	gint     season,
	gint     number
){
	return g_strdup_printf("episode:%" G_GINT64_FORMAT, episode_id(fx, series, season, number));
}

/* ── Files ──────────────────────────────────────────────────────── */

/* A test file: @line first (what the fake ffprobe reads), then @size bytes
 * in all of a pattern @seed chooses. */
static void
write_media(
	const gchar *path,
	const gchar *line,
	gsize        size,
	guint        seed
){
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree guchar *bytes = g_malloc(MAX(size, strlen(line) + 1));
	gsize n = strlen(line);
	gsize i;
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	memcpy(bytes, line, n);
	bytes[n] = '\n';
	for (i = n + 1; i < size; i++)
		bytes[i] = (guchar)((i * 31 + seed * 7) & 0xff);
	g_assert_true(g_file_set_contents(path, (const gchar *)bytes, (gssize)MAX(size, n + 1), &error));
	g_assert_no_error(error);
}

static void
write_text(
	const gchar *path,
	const gchar *text
){
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* The folder of a download in the downloads root, with a video for each
 * of @videos (a name ending .srt or .nfo is written as text). */
static void
write_download(
	Fixture            *fx,
	const gchar        *release,
	const gchar *const *names
){
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(fx->downloads, release, names[i], NULL);

		if (g_str_has_suffix(names[i], ".srt"))
			write_text(path, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n");
		else if (g_str_has_suffix(names[i], ".nfo"))
			write_text(path, "an nfo\n");
		else if (strstr(names[i], "sample") != NULL)
			write_media(path, SAMPLE_LINE, 8 * 1024, i + 1);
		else
			write_media(path, EPISODE_LINE, 256 * 1024, i + 1);
	}
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The video files of @dir (not hidden ones), sorted, joined by "|". */
static gchar *
videos_in(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) out = g_string_new(NULL);
	const gchar *name;
	guint i;

	if (d == NULL)
		return g_strdup("");
	while ((name = g_dir_read_name(d)) != NULL) {
		if (name[0] != '.' && g_str_has_suffix(name, ".mkv"))
			g_ptr_array_add(names, g_strdup(name));
	}
	g_ptr_array_sort(names, compare_names);
	for (i = 0; i < names->len; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? "|" : "", (const gchar *)g_ptr_array_index(names, i));
	return g_string_free(g_steal_pointer(&out), FALSE);
}

static guint
count_hidden(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint n = 0;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *child = g_build_filename(dir, name, NULL);

		if (g_str_has_prefix(name, ".axis-"))
			n++;
		if (g_file_test(child, G_FILE_TEST_IS_DIR))
			n += count_hidden(child);
	}
	return n;
}

static gchar *
season_dir(
	Fixture *fx,
	gint     season
){
	g_autofree gchar *folder = g_strdup_printf("Season %02d", season);

	return g_build_filename(fx->shows, SERIES_FOLDER, folder, NULL);
}

/* A completed download of @release for the episodes @episodes ("{1,2}"),
 * with its open grab, as a search would have left it; its id. */
static gint64
add_download(
	Fixture     *fx,
	gint64       series,
	const gchar *release,
	const gchar *ref,
	const gchar *episodes
){
	g_autofree gchar *title = q(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	gint64 download;
	gint64 grab;

	download = qi(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'Usenet', '%s', 'usenet', 'file', '%s', %s,"
		" 'Harbour Lights (2024)', 'tv', 'completed', 1000, 1000, '/data/downloads/%s', %" G_GINT64_FORMAT ","
		" 'downloads/%s', now(), now()) RETURNING id", fx->client_id, ref, release, title, release, fx->volume_id,
		release);
	grab = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at, scope, episode_id) VALUES (%" G_GINT64_FORMAT ", '%s', 'usenet', lower('1:0:%s'), %"
		G_GINT64_FORMAT ", now(), now(), 'episode', ('%s'::bigint[])[1]) RETURNING id", series, release, release,
		download, episodes);
	exec(fx, "INSERT INTO series_grab_episode (grab_id, episode_id) SELECT %" G_GINT64_FORMAT ", unnest('%s'::bigint[])",
		grab, episodes);
	/* The client holds it, so that a removal can be seen. */
	{
		g_autofree gchar *path = g_strdup_printf("/data/downloads/%s", release);

		axis_test_download_fake_put(fx->fake, ref, release, "axis", NULL, "completed", 1000, 1000, path, NULL);
	}
	return download;
}

static gint64
request_import(
	Fixture *fx,
	gint64   download
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	gint64 id = 0;
	gboolean created = FALSE;

	g_assert_no_error(error);
	g_assert_true(axis_import_request(fx->import, conn, AXIS_TV_MODULE_ID, download, &id, &created, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* One run of the import; its state and reason afterwards ("imported",
 * "waiting/place_failed", "partial/partly_imported"). */
static gchar *
run_import(
	Fixture *fx,
	gint64   import_id
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	memset(&report, 0, sizeof(report));
	if (!axis_import_run(fx->import, import_id, NULL, &report, &error))
		g_error("the import run failed: %s", error != NULL ? error->message : "?");
	return q(fx, "SELECT state || coalesce('/' || reason, '') FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
}

static void
assert_import(
	Fixture     *fx,
	gint64       import_id,
	const gchar *expected
){
	g_autofree gchar *state = run_import(fx, import_id);

	if (g_strcmp0(state, expected) != 0) {
		g_autofree gchar *text = q(fx, "SELECT coalesce(reason_text, '') || ' ' || blockers::text || ' / ' ||"
			" (SELECT string_agg(path || ': ' || coalesce(role, '-') || ' ' || reasons::text || ' ' ||"
			" coalesce(note, ''), '; ') FROM import_file WHERE import_id = i.id) FROM import i WHERE id = %"
			G_GINT64_FORMAT, import_id);

		g_error("import %" G_GINT64_FORMAT ": expected %s, got %s (%s)", import_id, expected, state, text);
	}
}

/* A run that dies (the hook's crash); the error's message. */
static gchar *
run_import_failing(
	Fixture *fx,
	gint64   import_id
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	memset(&report, 0, sizeof(report));
	g_assert_false(axis_import_run(fx->import, import_id, NULL, &report, &error));
	g_assert_nonnull(error);
	return g_strdup(error->message);
}

static void
hook_set(
	Fixture               *fx,
	const gchar           *at,
	const gchar           *path_has,
	AxisImportHookVerdict  verdict,
	guint                  nth
){
	fx->hook.at = at;
	fx->hook.path_has = path_has;
	fx->hook.verdict = verdict;
	fx->hook.nth = nth;
	fx->hook.seen = 0;
	fx->hook.fired = 0;
}

static AxisImportHookVerdict
hook_fn(
	const gchar *step,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
){
	Hook *hook = data;

	if (hook->at == NULL || strcmp(step, hook->at) != 0)
		return AXIS_IMPORT_HOOK_GO;
	if (hook->path_has != NULL) {
		g_autofree gchar *path = NULL;

		if (file_id <= 0)
			return AXIS_IMPORT_HOOK_GO;
		path = q(hook->fx, "SELECT path FROM import_file WHERE id = %" G_GINT64_FORMAT, file_id);
		if (path == NULL || strstr(path, hook->path_has) == NULL)
			return AXIS_IMPORT_HOOK_GO;
	}
	hook->seen++;
	if (hook->nth == 0 || hook->seen == hook->nth) {
		hook->fired++;
		return hook->verdict;
	}
	return AXIS_IMPORT_HOOK_GO;
}

/* ── 6.2 Adding, and what is monitored ──────────────────────────── */

#define ALL_REGULAR "S01E01,S01E02,S01E03,S01E04,S02E01,S02E02,S02E03,S02E04"

static void
test_add(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		AxisTvMonitor  monitor;
		gboolean       specials;
		const gchar   *expected;
	} choices[] = {
		{ AXIS_TV_MONITOR_ALL, TRUE, "S00E01," ALL_REGULAR },
		/* Not aired yet (S02E02 aired yesterday; S02E04 has no date). */
		{ AXIS_TV_MONITOR_FUTURE, FALSE, "S02E03,S02E04" },
		{ AXIS_TV_MONITOR_LATEST, FALSE, "S02E01,S02E02,S02E03,S02E04" },
		{ AXIS_TV_MONITOR_FIRST, FALSE, "S01E01,S01E02,S01E03,S01E04" },
		{ AXIS_TV_MONITOR_NONE, FALSE, NULL }
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;
	g_autofree gchar *on = NULL;
	AxisTvSpec spec;
	AxisTvSummary *summary;
	gint64 series = add_series(fx);
	gint64 id = 0;
	guint i;

	/* Every season and episode as the metadata has them; the specials
	 * not monitored; each episode with the provider's own id. */
	assert_qf(fx, "0,1,2", "SELECT string_agg(number::text, ',' ORDER BY number) FROM series_season WHERE series_id = %"
		G_GINT64_FORMAT, series);
	assert_qf(fx, "1,2", "SELECT string_agg(number::text, ',' ORDER BY number) FROM series_season WHERE series_id = %"
		G_GINT64_FORMAT " AND monitored", series);
	assert_qf(fx, "9", "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
	assert_qf(fx, "9102", "SELECT provider_id FROM series_episode WHERE id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 2));
	on = monitored(fx, series);
	g_assert_cmpstr(on, ==, ALL_REGULAR);

	/* Their states. */
	assert_ep(fx, series, 1, 1, AXIS_TV_WANTED, "Aired on 2024-03-01");
	assert_ep(fx, series, 2, 2, AXIS_TV_WANTED, "Aired on 2026-11-13");
	assert_ep(fx, series, 2, 3, AXIS_TV_NOT_AIRED, "Airs on 2026-11-20.");
	assert_ep(fx, series, 2, 4, AXIS_TV_NOT_AIRED, "No air date is known");
	assert_ep(fx, series, 0, 1, AXIS_TV_UNMONITORED, NULL);
	g_assert_true(axis_tv_list(fx->tv, &list, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(list->len, ==, 1);
	summary = g_ptr_array_index(list, 0);
	g_assert_cmpuint(summary->episodes, ==, 9);
	g_assert_cmpuint(summary->aired_monitored, ==, 6);
	g_assert_cmpuint(summary->in_library, ==, 0);
	g_assert_cmpuint(summary->counts[AXIS_TV_WANTED], ==, 6);
	g_assert_cmpuint(summary->counts[AXIS_TV_NOT_AIRED], ==, 2);
	g_assert_cmpuint(summary->counts[AXIS_TV_UNMONITORED], ==, 1);
	g_assert_cmpint(summary->state, ==, AXIS_TV_WANTED);
	g_assert_cmpstr(summary->next_air_date, ==, "2026-11-20");
	g_assert_cmpstr(summary->series->tvdb_id, ==, "360002");
	g_assert_cmpuint(summary->series->n_by_place, ==, 0);

	/* The history says who. */
	assert_qf(fx, "added|root", "SELECT kind || '|' || by_user FROM series_event WHERE series_id = %" G_GINT64_FORMAT
		" ORDER BY id LIMIT 1", series);

	/* Twice: refused. */
	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, AXIS_META_KIND_SERIES, "202");
	spec.root_id = fx->root_id;
	spec.user = "root";
	g_assert_false(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_EXISTS);
	g_clear_error(&error);
	/* Not a library root for series: refused. */
	spec.title_id = title_of(fx, AXIS_META_KIND_SERIES, "201");
	spec.root_id = fx->films_root;
	g_assert_false(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID);
	g_clear_error(&error);
	/* Not a series: refused. */
	spec.title_id = title_of(fx, AXIS_META_KIND_FILM, "101");
	spec.root_id = fx->root_id;
	g_assert_false(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID);
	g_clear_error(&error);
	assert_qf(fx, "1", "SELECT count(*) FROM series");

	/* Removed: its rows go, its history stays. */
	g_assert_true(axis_tv_remove(fx->tv, series, "root", &error));
	g_assert_no_error(error);
	assert_qf(fx, "0", "SELECT count(*) FROM series");
	assert_qf(fx, "0", "SELECT count(*) FROM series_episode");
	assert_qf(fx, "removed|root", "SELECT kind || '|' || by_user FROM series_event ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "2", "SELECT count(*) FROM series_event WHERE series_id IS NULL");

	/* Each choice. */
	for (i = 0; i < G_N_ELEMENTS(choices); i++) {
		g_autofree gchar *got = NULL;

		series = add_series_full(fx, "202", choices[i].monitor, choices[i].specials, FALSE);
		got = monitored(fx, series);
		if (g_strcmp0(got, choices[i].expected) != 0)
			g_error("%s: expected %s, got %s", axis_tv_monitor_name(choices[i].monitor),
				choices[i].expected != NULL ? choices[i].expected : "nothing", got != NULL ? got : "nothing");
		g_assert_true(axis_tv_remove(fx->tv, series, "root", &error));
		g_assert_no_error(error);
	}

	/* Monitoring changed by hand: a season with its episodes, and one
	 * episode; each recorded with the user. */
	series = add_series_full(fx, "202", AXIS_TV_MONITOR_NONE, FALSE, FALSE);
	g_assert_true(axis_tv_set_season_monitored(fx->tv, series, 1, TRUE, "root", &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_set_episode_monitored(fx->tv, series, episode_id(fx, series, 1, 2), FALSE, "root", &error));
	g_assert_no_error(error);
	g_clear_pointer(&on, g_free);
	on = monitored(fx, series);
	g_assert_cmpstr(on, ==, "S01E01,S01E03,S01E04");
	assert_qf(fx, "2", "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'monitored'"
		" AND by_user = 'root'", series);
	g_assert_false(axis_tv_set_season_monitored(fx->tv, series, 7, TRUE, "root", &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND);
}

/* ── 6.2 Following the metadata ─────────────────────────────────── */

static void
refetch_season(
	Fixture     *fx,
	gint64       series,
	gint         season,
	const gchar *json
){
	g_autofree gchar *route = g_strdup_printf("/title/series/202/season/%d", season);
	g_autofree gchar *title = q(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	g_autoptr(GError) error = NULL;
	AxisMetaSeasonAnswer answer;

	set_route(fx, route, json);
	exec(fx, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days' WHERE title_id = %s AND"
		" number = %d", title, season);
	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_get_season(fx->meta, g_ascii_strtoll(title, NULL, 10), season, 5000, &answer, &error));
	g_assert_no_error(error);
	axis_meta_season_answer_clear(&answer);
}

static guint
sync_series(
	Fixture *fx,
	gint64   series
){
	g_autoptr(GError) error = NULL;
	guint changes = 0;

	g_assert_true(axis_tv_sync(fx->tv, series, FALSE, &changes, &error));
	g_assert_no_error(error);
	return changes;
}

static void
test_metadata(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *on = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 storm = episode_id(fx, series, 2, 2);
	gint64 tba = episode_id(fx, series, 2, 4);
	gint64 street;

	/* Season 1 not monitored any more. */
	g_assert_true(axis_tv_set_season_monitored(fx->tv, series, 1, FALSE, "root", &error));
	g_assert_no_error(error);
	/* Something that refers to S01E02: its row must stay with it. */
	exec(fx, "INSERT INTO series_event (series_id, title_label, episode_id, kind, text) VALUES (%" G_GINT64_FORMAT ","
		" 'Harbour Lights (2024)', %" G_GINT64_FORMAT ", 'searched', 'A search of it.')", series, low_tide);

	/* Nothing changed: nothing to say. */
	g_assert_cmpuint(sync_series(fx, series), ==, 0);

	/* A refresh brings a new episode to each season, and season 1 is
	 * numbered anew (S01E02 is now S01E04, the two-part shifts up). */
	refetch_season(fx, series, 1, "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9101\",\"name\":\"Arrival\",\"air_date\":\"2024-03-01\"},"
		"{\"number\":2,\"id\":\"9103\",\"name\":\"High Water (1)\",\"air_date\":\"2024-03-15\"},"
		"{\"number\":3,\"id\":\"9104\",\"name\":\"High Water (2)\",\"air_date\":\"2024-03-15\"},"
		"{\"number\":4,\"id\":\"9102\",\"name\":\"Low Tide\",\"air_date\":\"2024-03-08\"},"
		"{\"number\":5,\"id\":\"9105\",\"name\":\"Found Footage\",\"air_date\":\"2024-03-22\"}]}");
	/* Season 2: a new episode; the one with no date is dropped. */
	refetch_season(fx, series, 2, "{\"number\":2,\"name\":\"Season 2\",\"episodes\":["
		"{\"number\":1,\"id\":\"9201\",\"name\":\"Return\",\"air_date\":\"2026-11-06\"},"
		"{\"number\":2,\"id\":\"9202\",\"name\":\"Storm\",\"air_date\":\"2026-11-13\"},"
		"{\"number\":3,\"id\":\"9203\",\"name\":\"Undertow\",\"air_date\":\"2026-11-20\"},"
		"{\"number\":5,\"id\":\"9205\",\"name\":\"Lighthouse\",\"air_date\":\"2026-11-27\"}]}");
	g_assert_cmpuint(sync_series(fx, series), >=, 5);

	/* Renumbered: the same row, at its new place, with what refers to it. */
	assert_qf(fx, "1|4|Low Tide", "SELECT season_number || '|' || number || '|' || title FROM series_episode WHERE id = %"
		G_GINT64_FORMAT, low_tide);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
	assert_qf(fx, "2", "SELECT number FROM series_episode WHERE provider_id = '9103' AND series_id = %" G_GINT64_FORMAT,
		series);
	g_assert_cmpint(episode_id(fx, series, 2, 2), ==, storm);
	/* New: monitored as its season is. */
	g_clear_pointer(&on, g_free);
	on = monitored(fx, series);
	g_assert_cmpstr(on, ==, "S02E01,S02E02,S02E03,S02E05");
	assert_ep(fx, series, 2, 5, AXIS_TV_NOT_AIRED, "Airs on 2026-11-27.");
	assert_ep(fx, series, 1, 5, AXIS_TV_UNMONITORED, NULL);
	/* Dropped: kept, marked gone, said so. */
	assert_qf(fx, "t", "SELECT gone_at IS NOT NULL FROM series_episode WHERE id = %" G_GINT64_FORMAT, tba);
	text = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'episodes'"
		" ORDER BY id DESC LIMIT 1", series);
	assert_contains(text, "2 new episodes");
	assert_contains(text, "S01E02 is now S01E04");
	assert_contains(text, "each keeps its grabs, files and history");
	assert_contains(text, "No longer listed: S02E04");

	/* Listed again: it comes back, the same row. */
	refetch_season(fx, series, 2, "{\"number\":2,\"name\":\"Season 2\",\"episodes\":["
		"{\"number\":1,\"id\":\"9201\",\"name\":\"Return\",\"air_date\":\"2026-11-06\"},"
		"{\"number\":2,\"id\":\"9202\",\"name\":\"Storm\",\"air_date\":\"2026-11-13\"},"
		"{\"number\":3,\"id\":\"9203\",\"name\":\"Undertow\",\"air_date\":\"2026-11-20\"},"
		"{\"number\":4,\"id\":\"9204\",\"name\":\"To Be Announced\"},"
		"{\"number\":5,\"id\":\"9205\",\"name\":\"Lighthouse\",\"air_date\":\"2026-11-27\"}]}");
	g_assert_cmpuint(sync_series(fx, series), >=, 1);
	g_assert_cmpint(episode_id(fx, series, 2, 4), ==, tba);

	/* A provider that gives no episode ids: episodes known by their place,
	 * counted on the series' page. */
	street = add_series_full(fx, "201", AXIS_TV_MONITOR_ALL, FALSE, FALSE);
	{
		g_autoptr(AxisTvSeries) s = NULL;
		g_autoptr(GPtrArray) episodes = NULL;

		g_assert_true(axis_tv_get(fx->tv, street, &s, &episodes, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(episodes->len, ==, 5);
		g_assert_cmpuint(s->n_by_place, ==, 5);
	}
	g_assert_cmpuint(sync_series(fx, street), ==, 0);

	/* The scheduler follows the metadata for a series whose seasons were
	 * fetched since. */
	exec(fx, "UPDATE series SET synced_at = now() - interval '1 hour'");
	exec(fx, "UPDATE meta_season SET episodes_fetched_at = now()");
	g_assert_cmpuint(schedule(fx).synced, ==, 2);
}

/* ── 6.1 End to end, on the job queue ───────────────────────────── */

#define STORM_WEBDL  "Harbour.Lights.S02E02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define STORM_HDTV   "Harbour.Lights.S02E02.720p.HDTV.x264-KILLERS"
#define RETURN_WEBDL "Harbour.Lights.S02E01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define STREET_WEBDL "Fixture.Street.S02E02.1080p.WEB-DL.DDP5.1.H.264-NTb"

static const Rel storm_releases[] = {
	{ STORM_HDTV, 900000000 },
	{ STORM_WEBDL, 1500000000 },
	{ RETURN_WEBDL, 1500000000 },
	{ STREET_WEBDL, 1500000000 }
};

static void
test_end_to_end(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = {
		STORM_WEBDL ".mkv", STORM_WEBDL ".en.srt", "harbour.lights.s02e02.sample.mkv", STORM_WEBDL ".nfo", NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *dir = season_dir(fx, 2);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *kinds = NULL;
	gboolean created = FALSE;
	gint64 series = add_series(fx);
	gint64 storm = episode_id(fx, series, 2, 2);
	gint64 download;
	gint64 import_id;

	set_releases(fx, storm_releases, G_N_ELEMENTS(storm_releases));
	start_queue(fx);
	/* Asked for: the job searches, and grabs. */
	g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, storm, "manual", "root", &created,
		&error));
	g_assert_no_error(error);
	g_assert_true(created);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM series_grab WHERE download_id IS NOT NULL", "1",
		20000));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND state ="
		" 'succeeded'", "1", 10000));
	stop_queue(fx);
	/* The one acceptable for that episode, of this series. */
	assert_qf(fx, STORM_WEBDL "|S02E02", "SELECT release_name || '|' || holds FROM series_grab");
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab_episode WHERE open AND episode_id = %" G_GINT64_FORMAT, storm);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_qf(fx, "tv", "SELECT module FROM download");
	assert_qf(fx, "grabbed|4|1|episode", "SELECT outcome || '|' || n_found || '|' || n_accepted || '|' || scope"
		" FROM series_search");
	assert_reason(fx, STORM_HDTV, "not_allowed");
	assert_reason(fx, RETURN_WEBDL, "other_episode");
	assert_qf(fx, "none", "SELECT match FROM series_candidate WHERE name = '" STREET_WEBDL "'");
	assert_ep(fx, series, 2, 2, AXIS_TV_DOWNLOADING, STORM_WEBDL);
	/* Nothing else of the series is touched. */
	assert_ep(fx, series, 2, 1, AXIS_TV_WANTED, NULL);

	/* Progress, then completed where the downloads root is. */
	ref = q(fx, "SELECT client_ref FROM download");
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	poll(fx);
	assert_ep(fx, series, 2, 2, AXIS_TV_DOWNLOADING, "40%");
	write_download(fx, STORM_WEBDL, files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 1500000000, "/data/downloads/" STORM_WEBDL, NULL);
	poll(fx);
	/* The follow-up records it and asks for its import. */
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	assert_ep(fx, series, 2, 2, AXIS_TV_TO_IMPORT, STORM_WEBDL);
	download = qi(fx, "SELECT id FROM download");
	import_id = qi(fx, "SELECT id FROM import WHERE download_id = %" G_GINT64_FORMAT, download);

	/* Imported, named, linked; the Usenet download removed. */
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S02E02 - Storm [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, storm);
	assert_qf(fx, "webdl_1080p|" STORM_WEBDL, "SELECT quality || '|' || release_name FROM series_file");
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab");
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE open");
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	assert_qf(fx, "sample", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%sample.mkv'", import_id);
	/* Its subtitle beside it. */
	{
		g_autofree gchar *sub = g_build_filename(dir, "Harbour Lights (2024) - S02E02 - Storm [WEB-DL 1080p][EAC3 5.1]"
			"[x264]-NTb.en.srt", NULL);

		g_assert_true(g_file_test(sub, G_FILE_TEST_IS_REGULAR));
	}
	/* In the library, below the profile's cutoff (Balanced: BluRay). */
	assert_ep(fx, series, 2, 2, AXIS_TV_UPGRADE_WANTED, "In the library as");

	/* The history tells it. */
	kinds = q(fx, "SELECT string_agg(DISTINCT kind, ',') FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(kinds, ==, "added,completed,grabbed,imported,searched");
	assert_qf(fx, "root", "SELECT by_user FROM series_event WHERE kind = 'searched' ORDER BY id LIMIT 1");
}

/* add-indexer-newznab task 4.7; spec: Without Prowlarr: a native Usenet
 * indexer alone finds the episode by its TVDB identifier, season and
 * number, and the release is fetched from it and sent */
static void
test_end_to_end_native(Fixture *fx, gconstpointer data)
{
	g_autoptr(GString) items = g_string_new(NULL);
	g_autofree gchar *feed = NULL;
	g_autofree gchar *query = NULL;
	gint64 series = add_series(fx);
	gint64 storm = episode_id(fx, series, 2, 2);
	AxisAcquireSearchReport report;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(storm_releases); i++)
		g_string_append_printf(items, "<item><title>%s</title><guid>@ORIGIN@/details/s%u</guid>"
			"<pubDate>Fri, 13 Nov 2026 10:00:00 +0000</pubDate>"
			"<enclosure url=\"@FILES@@BASE@/api?t=get&amp;id=nzb-s%u&amp;apikey=@KEY@\" length=\"%" G_GINT64_FORMAT
			"\" type=\"application/x-nzb\"/><newznab:attr name=\"category\" value=\"5040\"/>"
			"<newznab:attr name=\"size\" value=\"%" G_GINT64_FORMAT "\"/><newznab:attr name=\"tvdbid\" value=\"360002\"/>"
			"</item>", storm_releases[i].title, i, i, storm_releases[i].size, storm_releases[i].size);
	feed = axis_test_newznab_feed(items->str, 0, 0, 0, 0);
	axis_test_newznab_set_search(fx->nzb, NULL, feed);
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, storm);
	g_assert_true(report.searched);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, STORM_WEBDL "|S02E02", "SELECT release_name || '|' || holds FROM series_grab");
	assert_qf(fx, "grabbed|4|1|episode", "SELECT outcome || '|' || n_found || '|' || n_accepted || '|' || scope"
		" FROM series_search");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	/* By its TVDB identifier, season and number, in its TV categories; the
	 * file from the indexer; Prowlarr asked nothing. */
	query = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(query, ==, "t=tvsearch&tvdbid=360002&season=2&ep=2&cat=5000&extended=1&limit=100&offset=0&apikey="
		NZB_KEY);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->prowlarr), ==, 0);
	assert_qf(fx, "0", "SELECT count(*) FROM series_search WHERE coalesce(how, '') LIKE '%%" NZB_KEY "%%'");
	assert_ep(fx, series, 2, 2, AXIS_TV_DOWNLOADING, STORM_WEBDL);
}

/* ── 6.1, 6.3 A season pack ─────────────────────────────────────── */

#define PACK_S01      "Harbour.Lights.S01.1080p.BluRay.x264-DEMAND"
#define PACK_S01_S02  "Harbour.Lights.S01-S02.1080p.BluRay.x264-DEMAND"
#define PACK_S02      "Harbour.Lights.S02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define ARRIVAL_WEBDL "Harbour.Lights.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb"

static void
test_season_pack(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ ARRIVAL_WEBDL, 1500000000 },
		{ PACK_S01, 12000000000 },
		{ PACK_S01_S02, 20000000000 }
	};
	static const gchar *const files[] = {
		"Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND.mkv", "Harbour.Lights.S01E02.1080p.BluRay.x264-DEMAND.mkv",
		"Harbour.Lights.S01E03.1080p.BluRay.x264-DEMAND.mkv", "Harbour.Lights.S01E04.1080p.BluRay.x264-DEMAND.mkv",
		"Harbour.Lights.S01.Making.Of.1080p.BluRay.x264-DEMAND.mkv", "Harbour.Lights.S01.1080p.BluRay.x264-DEMAND.nfo",
		NULL
	};
	g_autofree gchar *ref = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	gint64 series = add_series(fx);
	gint64 import_id;
	AxisAcquireSearchReport report;
	gint i;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	/* The whole season, and nothing that is not. */
	assert_qf(fx, PACK_S01 "|S01|season|1", "SELECT release_name || '|' || holds || '|' || scope || '|' || season_number"
		" FROM series_grab");
	assert_reason(fx, ARRIVAL_WEBDL, "not_season");
	assert_reason(fx, PACK_S01_S02, "not_season");
	assert_qf(fx, "4", "SELECT count(*) FROM series_grab_episode WHERE open");
	for (i = 1; i <= 4; i++)
		assert_ep(fx, series, 1, i, AXIS_TV_DOWNLOADING, PACK_S01);
	assert_ep(fx, series, 2, 1, AXIS_TV_WANTED, NULL);
	assert_qf(fx, "season", "SELECT scope FROM series_search");
	/* Spec: Overlap: the pack in flight, a single episode of it is not
	 * grabbed (not even searched for). */
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, episode_id(fx, series, 1, 1));
	g_assert_cmpstr(report.skipped, ==, "a grab of it is in flight");
	g_assert_cmpuint(n_adds(fx), ==, 1);

	/* It arrives with the season's four files, an extra and an .nfo. */
	ref = q(fx, "SELECT client_ref FROM download");
	write_download(fx, PACK_S01, files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 12000000000, "/data/downloads/" PACK_S01, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT id FROM import");
	assert_import(fx, import_id, "imported");

	/* Each file to its episode, in the season's folder; the extra left. */
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E03 - High Water (1) [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E04 - High Water (2) [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv");
	for (i = 1; i <= 4; i++) {
		assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
			episode_id(fx, series, 1, i));
		assert_ep(fx, series, 1, i, AXIS_TV_IN_LIBRARY, "In the library as");
	}
	assert_qf(fx, "4", "SELECT count(*) FROM series_file");
	assert_qf(fx, "unmatched", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%Making.Of%%'", import_id);
	assert_qf(fx, "f", "SELECT accepted FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%Making.Of%%'", import_id);
	/* Every wanted file is in: the grab is done, the download removed. */
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	assert_qf(fx, "imported", "SELECT state FROM download");
}

/* ── 6.3 A pack only when it fits; the fallback ─────────────────── */

static void
test_pack_fits(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ RETURN_WEBDL, 1500000000 },
		{ STORM_WEBDL, 1500000000 },
		{ PACK_S02, 6000000000 }
	};
	g_autofree gchar *queued = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *e1 = NULL;
	g_autofree gchar *e2 = NULL;
	gint64 series = add_series(fx);
	AxisAcquireSearchReport report;

	/* Season 2: two aired, wanted; the pack also holds S02E03, which airs
	 * next week (monitored, not wanted): not taken.  The episodes are not
	 * a whole season either.  It runs as the queue runs it, its own job
	 * running meanwhile: that job does not stand for the episodes it falls
	 * back to (it did, and nothing followed). */
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	exec(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, lease_token, lease_expires_at,"
		" started_at) VALUES ('tv.search', 'searches', 'running', 2, 1, 'tv.search:%" G_GINT64_FORMAT ":season:2',"
		" gen_random_uuid(), now() + interval '5 minutes', now())", series);
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 2);
	exec(fx, "DELETE FROM job WHERE state = 'running'");
	g_assert_cmpstr(report.outcome, ==, "none_acceptable");
	assert_reason(fx, PACK_S02, "holds_unwanted");
	assert_reason(fx, RETURN_WEBDL, "not_season");
	assert_reason(fx, STORM_WEBDL, "not_season");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	/* So each wanted episode of the season is sought by itself, the latest
	 * aired first. */
	e1 = episode_key(fx, series, 2, 1);
	e2 = episode_key(fx, series, 2, 2);
	expected = g_strdup_printf("%s,%s", e2, e1);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, expected);
	assert_qf(fx, "manual,manual", "SELECT string_agg(payload->>'cause', ',') FROM job WHERE kind = 'tv.search'");

	/* Asked by a person again, with the series' search queued (it takes
	 * packs only) and the scheduler's search of S02E01 queued behind its
	 * backlog: S02E02 is searched for all the same, and S02E01's search is
	 * raised to a person's priority. */
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	exec(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) VALUES ('tv.search', 'searches', 'queued', 2,"
		" 'tv.search:%" G_GINT64_FORMAT ":series')", series);
	exec(fx, "INSERT INTO job (kind, queue, state, max_attempts, priority, dedupe_key) VALUES ('tv.search', 'searches',"
		" 'queued', 2, 0, 'tv.search:%" G_GINT64_FORMAT ":%s')", series, e1);
	exec(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, lease_token, lease_expires_at,"
		" started_at) VALUES ('tv.search', 'searches', 'running', 2, 1, 'tv.search:%" G_GINT64_FORMAT ":season:2',"
		" gen_random_uuid(), now() + interval '5 minutes', now())", series);
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 2);
	exec(fx, "DELETE FROM job WHERE state = 'running'");
	g_assert_cmpstr(report.outcome, ==, "none_acceptable");
	{
		g_autofree gchar *raised = g_strdup_printf("SELECT priority::text FROM job WHERE dedupe_key = 'tv.search:%"
			G_GINT64_FORMAT ":%s'", series, e1);
		g_autofree gchar *queued_e2 = g_strdup_printf("SELECT count(*)::text FROM job WHERE state = 'queued' AND"
			" dedupe_key = 'tv.search:%" G_GINT64_FORMAT ":%s'", series, e2);

		assert_qf(fx, "10", "%s", raised);
		assert_qf(fx, "1", "%s", queued_e2);
		/* A person's search that meets the scheduler's in the queue's
		 * de-duplication (the scheduler queued it between a look and the
		 * insert) raises it too. */
		exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
		{
			g_autoptr(GError) local = NULL;
			gboolean created = FALSE;

			g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, episode_id(fx, series, 2, 1),
				"scheduled", NULL, &created, &local));
			g_assert_true(created);
			assert_qf(fx, "0", "%s", raised);
			g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, episode_id(fx, series, 2, 1),
				"manual", "root", &created, &local));
			g_assert_no_error(local);
			g_assert_false(created);
			assert_qf(fx, "10", "%s", raised);
		}
	}
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");

	/* Not monitored, S02E03 no longer stands in the way. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_tv_set_episode_monitored(fx->tv, series, episode_id(fx, series, 2, 3), FALSE, "root",
			&error));
		g_assert_true(axis_tv_set_episode_monitored(fx->tv, series, episode_id(fx, series, 2, 4), FALSE, "root",
			&error));
		g_assert_no_error(error);
	}
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 2);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, PACK_S02, "SELECT release_name FROM series_grab");
	/* It holds the two it is not monitored for as well. */
	assert_qf(fx, "4", "SELECT count(*) FROM series_grab_episode WHERE open");
}

/* The scheduler's choice between a season and its episodes. */
static void
test_pack_threshold(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *queued = NULL;
	g_autofree gchar *e1 = NULL;
	g_autofree gchar *e3 = NULL;
	g_autofree gchar *e4 = NULL;
	g_autofree gchar *expected = NULL;
	gint64 series = add_series(fx);
	AxisTvScheduleReport report;

	/* Everything wanted, nothing of it in the library: a search of the whole
	 * series first, when asked for at once. */
	{
		g_autoptr(GError) error = NULL;
		gboolean created = FALSE;

		g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_SERIES, 0, "manual", "root", &created,
			&error));
		g_assert_no_error(error);
		g_assert_true(created);
		queued = queued_searches(fx, series);
		g_assert_cmpstr(queued, ==, "series");
		exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	}

	/* The scheduler: a season with every aired episode wanted is one
	 * search, the latest aired first. */
	report = schedule(fx);
	g_assert_cmpuint(report.considered, ==, 6);
	g_assert_cmpuint(report.due, ==, 6);
	g_assert_cmpuint(report.queued, ==, 2);
	g_assert_cmpuint(report.seasons, ==, 2);
	g_clear_pointer(&queued, g_free);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, "season:2,season:1");
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");

	/* One of season 1 searched a while ago and waiting: three of four is
	 * below a threshold of 100 %, so each episode by itself; at most three
	 * searches of one series a run. */
	set(fx, AXIS_TV_SETTING_PACK_SHARE, "100");
	exec(fx, "UPDATE series_episode SET next_search_at = to_timestamp(%" G_GINT64_FORMAT ") + interval '1 hour',"
		" last_search_at = to_timestamp(%" G_GINT64_FORMAT ") - interval '1 hour' WHERE id = %" G_GINT64_FORMAT,
		NOW_US / G_USEC_PER_SEC, NOW_US / G_USEC_PER_SEC, episode_id(fx, series, 1, 2));
	report = schedule(fx);
	g_assert_cmpuint(report.due, ==, 5);
	g_assert_cmpuint(report.queued, ==, 3);
	g_assert_cmpuint(report.seasons, ==, 1);
	e3 = episode_key(fx, series, 1, 3);
	e4 = episode_key(fx, series, 1, 4);
	e1 = episode_key(fx, series, 1, 1);
	expected = g_strdup_printf("season:2,%s,%s", e3, e4);
	g_clear_pointer(&queued, g_free);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, expected);
	/* Queued already: not twice. */
	report = schedule(fx);
	g_assert_cmpuint(report.queued, ==, 1);
	g_clear_pointer(&queued, g_free);
	queued = q(fx, "SELECT substr(dedupe_key, length('tv.search:%" G_GINT64_FORMAT ":') + 1) FROM job"
		" WHERE kind = 'tv.search' ORDER BY id DESC LIMIT 1", series);
	g_assert_cmpstr(queued, ==, e1);

	/* At most tv.searches_per_run a run. */
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	set(fx, AXIS_TV_SETTING_PER_RUN, "1");
	report = schedule(fx);
	g_assert_cmpuint(report.queued, ==, 1);
	g_clear_pointer(&queued, g_free);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, "season:2");
}

/* ── 6.3, 6.5 A double episode; overlapping releases ────────────── */

#define HIGH_WATER_1   "Harbour.Lights.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define HIGH_WATER_1_2 "Harbour.Lights.S01E03E04.1080p.BluRay.x264-DEMAND"
#define HIGH_WATER_2   "Harbour.Lights.S01E04.1080p.WEB-DL.DDP5.1.H.264-NTb"

static void
test_double_episode(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ HIGH_WATER_1, 1500000000 },
		{ HIGH_WATER_1_2, 6000000000 },
		{ HIGH_WATER_2, 1500000000 }
	};
	static const gchar *const files[] = { HIGH_WATER_1_2 ".mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	gint64 series = add_series(fx);
	gint64 part1 = episode_id(fx, series, 1, 3);
	gint64 part2 = episode_id(fx, series, 1, 4);
	gint64 grab = 0;
	gint64 other;
	gint64 import_id;
	AxisAcquireSearchReport report;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	/* A search for the first part: the better release holds both. */
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, part1);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, HIGH_WATER_1_2 "|S01E03-E04", "SELECT release_name || '|' || holds FROM series_grab");
	assert_reason(fx, HIGH_WATER_2, "other_episode");
	assert_qf(fx, "2", "SELECT count(*) FROM series_grab_episode WHERE open");
	assert_ep(fx, series, 1, 3, AXIS_TV_DOWNLOADING, HIGH_WATER_1_2);
	assert_ep(fx, series, 1, 4, AXIS_TV_DOWNLOADING, HIGH_WATER_1_2);

	/* The second part is in flight with it: not searched, and the indexer
	 * not asked. */
	{
		guint searches = axis_test_prowlarr_n_searches(fx->prowlarr);

		report = search(fx, series, AXIS_TV_SCOPE_EPISODE, part2);
		g_assert_cmpstr(report.skipped, ==, "a grab of it is in flight");
		g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, searches);
	}
	/* The first part's own release, grabbed by hand: it holds an episode in
	 * flight, refused; one grab, one download. */
	other = qi(fx, "SELECT id FROM series_candidate WHERE name = '" HIGH_WATER_1 "'");
	g_assert_false(axis_tv_grab(fx->tv, series, other, "root", &grab, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT);
	g_clear_error(&error);
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	/* A season search does not take a pack that holds them either. */
	{
		static const Rel pack[] = { { PACK_S01, 12000000000 } };

		set_releases(fx, pack, G_N_ELEMENTS(pack));
		report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
		g_assert_cmpstr(report.outcome, ==, "none_acceptable");
		assert_reason(fx, PACK_S01, "in_flight");
	}
	/* The database's own guard: an episode in two open grabs is refused. */
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, series);
		g_autofree gchar *episode = g_strdup_printf("%" G_GINT64_FORMAT, part2);
		const gchar *params[2];
		g_autoptr(AxisDbResult) row = NULL;
		gint64 second = 0;

		params[0] = id;
		g_assert_true(axis_db_pool_exec(fx->t->pool, "INSERT INTO series_grab (series_id, release_name, protocol,"
			" name_key) VALUES ($1::bigint, 'x', 'usenet', 'x') RETURNING id", 1, params, &row, &error));
		g_assert_no_error(error);
		axis_db_result_get_int64(row, 0, 0, &second);
		params[0] = g_strdup_printf("%" G_GINT64_FORMAT, second);
		params[1] = episode;
		g_assert_false(axis_db_pool_exec(fx->t->pool, "INSERT INTO series_grab_episode (grab_id, episode_id) VALUES"
			" ($1::bigint, $2::bigint)", 2, params, NULL, &error));
		g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION);
		g_clear_error(&error);
		g_free((gchar *)params[0]);
		exec(fx, "DELETE FROM series_grab WHERE id = %" G_GINT64_FORMAT, second);
	}

	/* Imported: one file, both episodes. */
	ref = q(fx, "SELECT client_ref FROM download");
	write_download(fx, HIGH_WATER_1_2, files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 6000000000, "/data/downloads/" HIGH_WATER_1_2, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT id FROM import");
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E03-E04 - High Water (1) [BluRay 1080p][EAC3 5.1][x264]"
		"-DEMAND.mkv");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file");
	assert_qf(fx, "2", "SELECT count(*) FROM series_file_episode");
	assert_qf(fx, "1", "SELECT count(DISTINCT file_id) FROM series_file_episode WHERE episode_id IN (%" G_GINT64_FORMAT
		", %" G_GINT64_FORMAT ")", part1, part2);
	assert_ep(fx, series, 1, 3, AXIS_TV_IN_LIBRARY, NULL);
	assert_ep(fx, series, 1, 4, AXIS_TV_IN_LIBRARY, NULL);
}

/* ── 6.5 Two searches at once ───────────────────────────────────── */

typedef struct
{
	GMutex lock;
	GCond  cond;
	guint  arrived;
} Barrier;

/* Both searches wait for each other just before the claim, so that the two
 * claim transactions really race. */
static void
claim_barrier(
	gint64   series_id,
	gpointer data
){
	Barrier *barrier = data;
	gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	g_mutex_lock(&barrier->lock);
	barrier->arrived++;
	g_cond_broadcast(&barrier->cond);
	while (barrier->arrived < 2 && g_cond_wait_until(&barrier->cond, &barrier->lock, end))
		;
	g_mutex_unlock(&barrier->lock);
}

typedef struct
{
	Fixture                 *fx;
	gint64                   series;
	gint64                   episode;
	AxisAcquireSearchReport  report;
	gboolean                 ok;
} Racer;

static gpointer
race(gpointer data)
{
	Racer *racer = data;
	g_autoptr(GError) error = NULL;

	racer->ok = axis_tv_run_search(racer->fx->tv, racer->series, AXIS_TV_SCOPE_EPISODE, racer->episode, "manual",
		"root", NULL, &racer->report, &error);
	if (error != NULL)
		g_printerr("race: %s\n", error->message);
	return NULL;
}

static void
test_race(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = { { HIGH_WATER_1_2, 6000000000 } };
	Barrier barrier;
	Racer a;
	Racer b;
	GThread *ta;
	GThread *tb;
	gint64 series = add_series(fx);

	/* Searches for each part at once; the one release holds both. */
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	memset(&barrier, 0, sizeof(barrier));
	g_mutex_init(&barrier.lock);
	g_cond_init(&barrier.cond);
	axis_tv_set_claim_hook(fx->tv, claim_barrier, &barrier);
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	a.fx = b.fx = fx;
	a.series = b.series = series;
	a.episode = episode_id(fx, series, 1, 3);
	b.episode = episode_id(fx, series, 1, 4);
	ta = g_thread_new("racer-a", race, &a);
	tb = g_thread_new("racer-b", race, &b);
	g_thread_join(ta);
	g_thread_join(tb);
	axis_tv_set_claim_hook(fx->tv, NULL, NULL);
	g_assert_true(a.ok);
	g_assert_true(b.ok);
	g_assert_cmpuint(barrier.arrived, ==, 2);
	/* One grab, one download, one release at the client. */
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab");
	assert_qf(fx, "1", "SELECT count(*) FROM download");
	assert_qf(fx, "2", "SELECT count(*) FROM series_grab_episode WHERE open");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	g_assert_true(g_strcmp0(a.report.outcome, "grabbed") == 0 || g_strcmp0(b.report.outcome, "grabbed") == 0);
	g_assert_true(g_strcmp0(a.report.outcome, "in_flight") == 0 || g_strcmp0(b.report.outcome, "in_flight") == 0);
	g_mutex_clear(&barrier.lock);
	g_cond_clear(&barrier.cond);
}

/* ── 6.4 Never another episode, never another series ────────────── */

#define BEYOND      "Harbour.Lights.S01E09.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define ABSOLUTE    "[SubGroup] Harbour Lights - 102 [1080p]"
#define STREET_LOW  "Fixture.Street.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define NO_EPISODE  "Harbour.Lights.1080p.WEB-DL.DDP5.1.H.264-NTb"

static void
test_mismatch(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ BEYOND, 1500000000 },
		{ ABSOLUTE, 1500000000 },
		{ STREET_LOW, 1500000000 },
		{ NO_EPISODE, 1500000000 }
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvSeries) s = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(AxisTvReleases) releases = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 grab = 0;
	gint64 beyond;
	AxisAcquireSearchReport report;
	guint i;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpstr(report.outcome, ==, "none_acceptable");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab");
	/* Every one kept with why, by a fixed rule (safety): none can be
	 * grabbed by hand. */
	assert_qf(fx, "4", "SELECT count(*) FROM series_candidate WHERE NOT accepted AND safety AND match = 'none'");
	text = q(fx, "SELECT match_text FROM series_candidate WHERE name = '" BEYOND "'");
	assert_contains(text, "numbered S01E09, and there is no such episode in the episode list");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT match_text FROM series_candidate WHERE name = '" ABSOLUTE "'");
	assert_contains(text, "only an absolute episode number (102)");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT match_text FROM series_candidate WHERE name = '" STREET_LOW "'");
	assert_contains(text, "Fixture Street");
	/* The numbering trap is counted: on the search, on the series. */
	assert_qf(fx, "1", "SELECT n_mismatched FROM series_search");
	/* A release numbered for no episode of the data at all is nobody's to
	 * grab with one click: not counted as a person's (add-attention-notices,
	 * N2). */
	assert_qf(fx, "0", "SELECT n_person_only FROM series_search");
	assert_qf(fx, "t", "SELECT mismatched FROM series_candidate WHERE name = '" BEYOND "'");
	g_assert_true(axis_tv_get(fx->tv, series, &s, &episodes, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(s->n_mismatched, ==, 1);
	/* The episode's releases say what each holds, and which are mismatched. */
	g_assert_true(axis_tv_get_releases(fx->tv, series, low_tide, -1, &releases, &error));
	g_assert_no_error(error);
	g_assert_nonnull(releases->search);
	g_assert_cmpuint(releases->candidates->len, ==, 4);
	for (i = 0; i < releases->candidates->len; i++) {
		const AxisAcquireCandidate *c = g_ptr_array_index(releases->candidates, i);

		g_assert_cmpint(g_array_index(releases->mismatched, gboolean, i), ==, strcmp(c->name, BEYOND) == 0);
	}

	/* By hand: refused, a fixed rule. */
	beyond = qi(fx, "SELECT id FROM series_candidate WHERE name = '" BEYOND "'");
	g_assert_false(axis_tv_grab(fx->tv, series, beyond, "root", &grab, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_ep(fx, series, 1, 2, AXIS_TV_ATTENTION, NULL);
}

/* A series of the same name: a release name without a year is not
 * certainly this one; with the other's year, it is the other's. */
static void
test_same_name(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ "Harbour.Lights.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 },
		{ "Harbour.Lights.1998.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 },
		{ "Harbour.Lights.2024.S01E02.720p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 },
		{ "Harbour.Lights.2024.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 }
	};
	gint64 series;
	AxisAcquireSearchReport report;

	set_route(fx, "/search/series", "{\"results\":[{\"id\":\"202\",\"kind\":\"series\",\"title\":\"Harbour Lights\","
		"\"year\":2024},{\"id\":\"203\",\"kind\":\"series\",\"title\":\"Harbour Lights\",\"year\":1998}]}");
	set_route(fx, "/title/series/203", "{\"id\":\"203\",\"kind\":\"series\",\"title\":\"Harbour Lights\",\"year\":1998,"
		"\"status\":\"ended\",\"seasons\":[]}");
	series = add_series(fx);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, episode_id(fx, series, 1, 2));
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, "Harbour.Lights.2024.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb", "SELECT release_name FROM series_grab");
	assert_qf(fx, "ambiguous", "SELECT match FROM series_candidate WHERE name ="
		" 'Harbour.Lights.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb'");
	assert_qf(fx, "none", "SELECT match FROM series_candidate WHERE name ="
		" 'Harbour.Lights.1998.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb'");
	g_assert_cmpuint(n_adds(fx), ==, 1);
}

/* ── 6.6 The air date and its delay ─────────────────────────────── */

static void
test_air_delay(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *queued = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *e1 = NULL;
	gint64 series = add_series(fx);
	gint64 tba = episode_id(fx, series, 2, 4);
	AxisTvScheduleReport report;

	/* 2026-11-13 00:30 UTC: S02E02 airs today, the hour has not passed. */
	axis_tv_set_clock(fx->tv, NOW_US - 35 * HOUR_US - HOUR_US / 2);
	assert_ep(fx, series, 2, 2, AXIS_TV_NOT_AIRED, "searched for from 2026-11-13 01:00 UTC");
	report = schedule(fx);
	g_assert_cmpuint(report.considered, ==, 5);
	/* Season 1 whole; S02E01 alone (one of season 2 has aired). */
	e1 = episode_key(fx, series, 2, 1);
	expected = g_strdup_printf("%s,season:1", e1);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, expected);
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");

	/* An hour later: aired, and season 2 is sought whole. */
	axis_tv_set_clock(fx->tv, NOW_US - 35 * HOUR_US + HOUR_US / 2);
	assert_ep(fx, series, 2, 2, AXIS_TV_WANTED, NULL);
	report = schedule(fx);
	g_assert_cmpuint(report.considered, ==, 6);
	g_clear_pointer(&queued, g_free);
	queued = queued_searches(fx, series);
	g_assert_cmpstr(queued, ==, "season:2,season:1");

	/* No air date: never by the scheduler, however late. */
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	exec(fx, "UPDATE series_episode SET monitored = (id = %" G_GINT64_FORMAT ") WHERE series_id = %" G_GINT64_FORMAT,
		tba, series);
	axis_tv_set_clock(fx->tv, NOW_US + 400 * 24 * HOUR_US);
	report = schedule(fx);
	g_assert_cmpuint(report.considered, ==, 0);
	g_assert_cmpuint(report.queued, ==, 0);
	assert_ep(fx, series, 2, 4, AXIS_TV_NOT_AIRED, "No air date is known");
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	/* A person may still ask for it. */
	{
		g_autoptr(GError) error = NULL;
		gboolean created = FALSE;

		g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, tba, "manual", "root", &created,
			&error));
		g_assert_no_error(error);
		g_assert_true(created);
	}
}

/* ── 3.4 A failed grab ──────────────────────────────────────────── */

static void
test_failure(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ STORM_WEBDL, 1500000000 },
		{ "Harbour.Lights.S02E02.1080p.WEB-DL.DDP5.1.H.264-OTHER", 1400000000 }
	};
	g_autofree gchar *ref = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	gint64 series = add_series(fx);
	gint64 storm = episode_id(fx, series, 2, 2);
	AxisAcquireSearchReport report;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, storm);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	ref = q(fx, "SELECT client_ref FROM download");
	axis_test_download_fake_set(fx->fake, ref, "failed", 0, NULL, "CRC error");
	poll(fx);
	g_assert_cmpuint(follow(fx).failed, ==, 1);
	/* Blocklisted, closed, and the episode sought again. */
	assert_qf(fx, "1", "SELECT count(*) FROM series_blocklist");
	assert_qf(fx, "failed", "SELECT close_reason FROM series_grab");
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE open");
	assert_qf(fx, "retry", "SELECT payload->>'cause' FROM job WHERE kind = 'tv.search' AND state = 'queued'");
	{
		g_autofree gchar *key = episode_key(fx, series, 2, 2);
		g_autofree gchar *queued = queued_searches(fx, series);

		g_assert_cmpstr(queued, ==, key);
	}
	/* The next best goes. */
	first = q(fx, "SELECT release_name FROM series_grab");
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, storm);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	second = q(fx, "SELECT release_name FROM series_grab WHERE closed_at IS NULL");
	g_assert_nonnull(second);
	g_assert_cmpstr(second, !=, first);
	assert_qf(fx, "t", "SELECT blocklisted FROM series_candidate WHERE name = (SELECT release_name FROM series_grab"
		" WHERE closed_at IS NOT NULL) AND search_id = (SELECT max(id) FROM series_search)");
	/* The history has it. */
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'failed' AND text LIKE '%%series'' blocklist"
		" now.%%'");
}

/* ── 6.7 The import, file by file ───────────────────────────────── */

#define ARRIVAL_FILE  "Harbour.Lights.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv"
#define LOW_TIDE_FILE "Harbour.Lights.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv"

/* A grab of S01E01 whose download holds S01E02 too, the special, an
 * episode the data does not have, and another series' episode. */
static void
test_import_extra(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = {
		ARRIVAL_FILE, LOW_TIDE_FILE, "Harbour.Lights.S00E01.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv",
		"Harbour.Lights.S01E09.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv", "Fixture.Street.S01E01.1080p.WEB-DL-NTb.mkv",
		"Harbour.Lights.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb.en.srt", NULL
	};
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *episodes = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 import_id;

	episodes = g_strdup_printf("{%" G_GINT64_FORMAT "}", arrival);
	write_download(fx, "Harbour.Lights.S01E01.Extra", files);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01.Extra", "nzo_extra", episodes));
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	{
		g_autofree gchar *specials = season_dir(fx, 0);
		g_autofree gchar *none = videos_in(specials);

		g_assert_cmpstr(none, ==, "");
	}
	assert_qf(fx, "2", "SELECT count(*) FROM series_file");
	assert_ep(fx, series, 1, 1, AXIS_TV_UPGRADE_WANTED, NULL);
	assert_ep(fx, series, 1, 2, AXIS_TV_UPGRADE_WANTED, NULL);
	assert_ep(fx, series, 0, 1, AXIS_TV_UNMONITORED, NULL);
	/* Each left file says why. */
	text = q(fx, "SELECT reasons->0->>'t' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%S00E01%%'", import_id);
	assert_contains(text, "not wanted");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reasons->0->>'t' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%S01E09%%'", import_id);
	assert_contains(text, "no such episode in the episode list");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reasons->0->>'t' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%Fixture.Street%%'", import_id);
	assert_contains(text, "not Harbour Lights (2024)");
	assert_qf(fx, "3", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND reasons->0->>'c' = 'unmatched'", import_id);
	/* The subtitle beside its episode. */
	{
		g_autofree gchar *sub = g_build_filename(dir, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1]"
			"[x264]-NTb.en.srt", NULL);

		g_assert_true(g_file_test(sub, G_FILE_TEST_IS_REGULAR));
	}
	/* What the grab was for is in: done; the download removed. */
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "nzo_extra"), ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'imported'");
}

/* A download that holds nothing wanted is blocked, for a person; another
 * download of the series is imported meanwhile. */
static void
test_import_blocked(Fixture *fx, gconstpointer data)
{
	static const gchar *const nothing[] = { "Harbour.Lights.Behind.The.Scenes.1080p.WEB-DL-NTb.mkv", NULL };
	static const gchar *const arrival[] = { ARRIVAL_FILE, NULL };
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *other = NULL;
	gint64 series = add_series(fx);
	gint64 blocked;
	gint64 import_id;

	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", episode_id(fx, series, 2, 1));
	write_download(fx, "Harbour.Lights.S02E01.1080p.WEB-DL-NTb", nothing);
	blocked = request_import(fx, add_download(fx, series, "Harbour.Lights.S02E01.1080p.WEB-DL-NTb", "nzo_nothing",
		one));
	assert_import(fx, blocked, "blocked/nothing_wanted");
	assert_ep(fx, series, 2, 1, AXIS_TV_ATTENTION, "needs a person");

	other = g_strdup_printf("{%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, 1));
	write_download(fx, "Harbour.Lights.S01E01.WEB-DL", arrival);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01.WEB-DL", "nzo_arrival", other));
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	/* The blocked one is as it was. */
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, blocked);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "nzo_nothing"), ==, 0);
	/* Two of one series are never placing at once (the database's guard). */
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, import_id);
		g_autoptr(GError) error = NULL;
		const gchar *params[1];

		exec(fx, "UPDATE import SET state = 'running' WHERE id = %" G_GINT64_FORMAT, blocked);
		params[0] = id;
		g_assert_false(axis_db_pool_exec(fx->t->pool, "UPDATE import SET state = 'running', finished_at = NULL"
			" WHERE id = $1::bigint", 1, params, NULL, &error));
		g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION);
	}
}

/* An upgrade of one episode leaves the other's file as it is. */
static void
test_import_upgrade(Fixture *fx, gconstpointer data)
{
	static const gchar *const first[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	static const gchar *const better[] = { "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND.mkv", NULL };
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *pair = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *low_tide_path = NULL;
	GStatBuf before;
	GStatBuf after;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 import_id;
	gint64 low_tide_file;

	pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", first);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "nzo_first", pair));
	assert_import(fx, import_id, "imported");
	low_tide_path = g_build_filename(dir, "Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]"
		"-NTb.mkv", NULL);
	g_assert_cmpint(g_lstat(low_tide_path, &before), ==, 0);
	low_tide_file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);

	/* A BluRay of S01E01: it replaces S01E01's file only. */
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", arrival);
	write_download(fx, "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND", better);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND",
		"nzo_better", one));
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "bluray_1080p", "SELECT sf.quality FROM series_file sf JOIN series_file_episode e ON e.file_id ="
		" sf.file_id JOIN library_file lf ON lf.id = sf.file_id WHERE e.episode_id = %" G_GINT64_FORMAT
		" AND lf.state = 'present'", arrival);
	assert_ep(fx, series, 1, 1, AXIS_TV_IN_LIBRARY, NULL);
	/* The other episode's file: the same file, still linked. */
	g_assert_cmpint(g_lstat(low_tide_path, &after), ==, 0);
	g_assert_cmpuint((guint64)after.st_ino, ==, (guint64)before.st_ino);
	g_assert_cmpint((gint64)after.st_mtime, ==, (gint64)before.st_mtime);
	assert_qf(fx, "present", "SELECT state FROM library_file WHERE id = %" G_GINT64_FORMAT, low_tide_file);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, low_tide, low_tide_file);
	assert_ep(fx, series, 1, 2, AXIS_TV_UPGRADE_WANTED, NULL);
	/* The replaced file is in the recycle folder, not gone. */
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT, import_id);
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
}

/* add-manual-search: a manual search lists, grabs nothing and moves no
 * schedule; a release the profile rejects, grabbed by hand, is imported with
 * the profile set aside (and Grab it anyway's too); a file a hand grab is
 * not better than is replaced only with the tick. */
#define BETTER_ARRIVAL "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND"
#define WORSE_ARRIVAL  "Harbour.Lights.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb"

static void
test_hand_grab_import(Fixture *fx, gconstpointer data)
{
	static const Rel storm[] = { { STORM_WEBDL, 1500000000 } };
	static const Rel back[] = { { RETURN_WEBDL, 1500000000 } };
	static const gchar *const storm_files[] = { STORM_WEBDL ".mkv", NULL };
	static const gchar *const back_files[] = { RETURN_WEBDL ".mkv", NULL };
	static const gchar *const better_files[] = { BETTER_ARRIVAL ".mkv", NULL };
	static const gchar *const worse_files[] = { WORSE_ARRIVAL ".mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	gint64 series = add_series(fx);
	gint64 storm_id = episode_id(fx, series, 2, 2);
	gint64 back_id = episode_id(fx, series, 2, 1);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 candidate;
	gint64 grab = 0;
	gint64 download;
	gint64 import_id;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;

	/* Not better than the file in the library: a BluRay of S01E01 first. */
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", arrival);
	write_download(fx, BETTER_ARRIVAL, better_files);
	import_id = request_import(fx, add_download(fx, series, BETTER_ARRIVAL, "nzo_better", one));
	assert_import(fx, import_id, "imported");
	/* A WEB-DL grabbed by hand without the tick: not imported over it. */
	write_download(fx, WORSE_ARRIVAL, worse_files);
	download = add_download(fx, series, WORSE_ARRIVAL, "nzo_worse", one);
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand' WHERE download_id = %" G_GINT64_FORMAT,
		download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/not_upgrade");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv");
	/* With the tick, for that very file at the quality shown, it replaces
	 * it. */
	exec(fx, "UPDATE series_grab SET replaces_files = jsonb_build_array(jsonb_build_object('f', (SELECT file_id FROM"
		" series_file_episode WHERE episode_id = %" G_GINT64_FORMAT "), 'q', 'bluray_1080p', 'r', null))"
		" WHERE download_id = %" G_GINT64_FORMAT, arrival, download);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&placed, g_free);
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, import_id);

	/* A profile that allows no 1080p. */
	exec(fx, "UPDATE series SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug = 'uhd') WHERE id = %"
		G_GINT64_FORMAT, series);

	/* A manual search: every release kept, nothing grabbed, no schedule
	 * moved, no state changed. */
	set_releases(fx, storm, G_N_ELEMENTS(storm));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, storm_id, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab WHERE release_name = '" STORM_WEBDL "'");
	assert_qf(fx, "t", "SELECT last_search_at IS NULL AND next_search_at IS NULL AND search_floor_s = 21600"
		" FROM series_episode WHERE id = %" G_GINT64_FORMAT, storm_id);
	assert_qf(fx, "list|listed", "SELECT cause || '|' || outcome FROM series_search ORDER BY id DESC LIMIT 1");
	assert_reason(fx, STORM_WEBDL, "not_allowed");
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	assert_ep(fx, series, 2, 2, AXIS_TV_WANTED, NULL);

	/* Grab this one: sent by hand, the profile set aside. */
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" STORM_WEBDL "'");
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	grab = grabbed.grab_id;
	g_assert_cmpint(grab, >, 0);
	assert_qf(fx, "true|hand|[]|false", "SELECT manual::text || '|' || set_aside || '|' || replaces_files::text || '|'"
		" || numbering_ok::text FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "root", "SELECT grabbed_by FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND why LIKE"
		" 'Chosen by root by hand from a manual search, the quality profile 4K set aside%%'", grab);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'grabbed' AND text LIKE 'Grabbed by hand %%'"
		" AND grab_id = %" G_GINT64_FORMAT, grab);
	assert_ep(fx, series, 2, 2, AXIS_TV_DOWNLOADING, STORM_WEBDL);
	/* Its import: the profile rejects the file, and it is imported. */
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	write_download(fx, STORM_WEBDL, storm_files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 1500000000, "/data/downloads/" STORM_WEBDL, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, storm_id);

	/* Grab it anyway, from an automatic search: its import too. */
	set_releases(fx, back, G_N_ELEMENTS(back));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, back_id);
	g_assert_cmpstr(report.outcome, ==, "none_acceptable");
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" RETURN_WEBDL "' ORDER BY id DESC LIMIT 1");
	g_assert_true(axis_tv_grab(fx->tv, series, candidate, "root", &grab, &error));
	g_assert_no_error(error);
	assert_qf(fx, "anyway", "SELECT set_aside FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_clear_pointer(&ref, g_free);
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	write_download(fx, RETURN_WEBDL, back_files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 1500000000, "/data/downloads/" RETURN_WEBDL, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, back_id);
}

/* add-manual-search (review): what a person set aside is for the episodes
 * their grab claimed.  A download grabbed by hand for S01E01 that holds
 * S01E02 too: S01E01's file is imported with the profile set aside, and
 * S01E02's is judged as any import's (the profile allows no 1080p). */
static void
test_hand_grab_claimed(Fixture *fx, gconstpointer data)
{
	static const gchar *const both[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	g_autofree gchar *one = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 download;
	gint64 import_id;

	exec(fx, "UPDATE series SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug = 'uhd') WHERE id = %"
		G_GINT64_FORMAT, series);
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", arrival);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", both);
	download = add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "nzo_claimed", one);
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand' WHERE download_id = %" G_GINT64_FORMAT,
		download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, arrival);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
	assert_qf(fx, "t", "SELECT accepted FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE"
		" '%%S01E01%%'", import_id);
	assert_qf(fx, "f", "SELECT accepted FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE"
		" '%%S01E02%%'", import_id);
}

/* add-manual-search (review): a release that numbers its episodes
 * differently from the series' data (S01E04E05: there is no S01E05) is
 * never taken with one click; Grab this one warns, needs a tick of its own,
 * and its import does what the warning said (the file is placed for S01E04
 * alone, the other number noted).  A grab by hand whose import can take no
 * file lets go of its episodes and says so in the history. */
#define NUMBERED       "Harbour.Lights.S01E04E05.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define NUMBERED_STUCK "Harbour.Lights.S01E03E05.1080p.WEB-DL.DDP5.1.H.264-NTb"

/* add-attention-notices, after the N2 review (S5): a release numbered beyond
 * the series' data in part is counted as one that waits for a person
 * (series_search.n_person_only) only when taking it would give the search
 * what it looked for.  One that holds another episode than the one searched
 * for is left for that reason first; it is stored as numbered beyond the data
 * all the same (no one click takes it), and is not counted. */
static void
test_person_only_beside(Fixture *fx, gconstpointer data)
{
	static const Rel numbered[] = { { NUMBERED, 2900000000 } };
	gint64 series = add_series(fx);
	gint64 slack = episode_id(fx, series, 1, 4);
	gint64 high = episode_id(fx, series, 1, 3);
	g_autofree gchar *text = NULL;
	AxisAcquireSearchReport report;

	set_releases(fx, numbered, G_N_ELEMENTS(numbered));
	/* S01E04E05, searched for S01E04: a person's Grab this one fills it. */
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, slack);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	assert_qf(fx, "1", "SELECT n_person_only FROM series_search WHERE episode_id = %" G_GINT64_FORMAT, slack);

	/* The same release, searched for S01E03: it holds other episodes. */
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, high);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	text = q(fx, "SELECT c.reasons::text FROM series_candidate c JOIN series_search s ON s.id = c.search_id"
		" WHERE s.episode_id = %" G_GINT64_FORMAT, high);
	assert_contains(text, "not S01E03 that was searched for");
	assert_contains(text, "only Grab this one, from a manual search, takes it");
	assert_qf(fx, "0", "SELECT n_person_only FROM series_search WHERE episode_id = %" G_GINT64_FORMAT, high);
}

static void
test_hand_grab_numbering(Fixture *fx, gconstpointer data)
{
	static const Rel numbered[] = { { NUMBERED, 2900000000 } };
	static const gchar *const numbered_files[] = { NUMBERED ".mkv", NULL };
	static const gchar *const stuck_files[] = { NUMBERED_STUCK ".mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisAcquireHandPreview) preview = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *three = NULL;
	gint64 series = add_series(fx);
	gint64 slack = episode_id(fx, series, 1, 4);
	gint64 high = episode_id(fx, series, 1, 3);
	gint64 candidate;
	gint64 grab = 0;
	gint64 download;
	gint64 import_id;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;

	/* An automatic search never takes it, and no one click does. */
	set_releases(fx, numbered, G_N_ELEMENTS(numbered));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, slack);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" NUMBERED "' ORDER BY id DESC LIMIT 1");
	g_assert_false(axis_tv_grab(fx->tv, series, candidate, "root", &grab, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "only Grab this one, from a manual search, takes it"));
	g_clear_error(&error);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab");
	g_assert_cmpuint(n_adds(fx), ==, 0);

	/* A manual search lists it; Grab this one warns, as the confirmation
	 * shows it. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, slack, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" NUMBERED "' ORDER BY id DESC LIMIT 1");
	g_assert_true(axis_tv_hand_preview(fx->tv, series, candidate, &preview, &error));
	g_assert_no_error(error);
	g_assert_null(preview->refused);
	g_assert_cmpstr(preview->missing, ==, "S01E05");
	g_assert_nonnull(preview->numbering);
	g_assert_true(g_str_has_prefix(preview->numbering, "This release numbers its episodes differently from the "
		"series' data (it names S01E05, which the data does not have). Files are placed by number, so they may land "
		"on the wrong episodes."));
	/* Without its tick: nothing sent. */
	g_assert_false(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_REFUSED);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_NEEDS_TICK);
	g_clear_error(&error);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab");
	/* With it: sent, the tick recorded. */
	ticks.numbering = TRUE;
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	g_assert_true(grabbed.numbering);
	grab = grabbed.grab_id;
	assert_qf(fx, "hand|t", "SELECT set_aside || '|' || CASE WHEN numbering_ok THEN 't' ELSE 'f' END FROM series_grab"
		" WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND why LIKE"
		" '%%; numbered differently from the data, as ticked.'", grab);
	/* Its import: the file placed for S01E04 alone, the other number noted. */
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	write_download(fx, NUMBERED, numbered_files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 2900000000, "/data/downloads/" NUMBERED, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, slack);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode");
	assert_qf(fx, "1", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND accepted AND"
		" reasons::text LIKE '%%Its name also says S01E05, which the series'' data does not have: it is placed for"
		" S01E04 alone.%%'", import_id);
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);

	/* A grab by hand whose import can take no file (it was not ticked):
	 * blocked, and its episodes let go, said in the history. */
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", high);
	write_download(fx, NUMBERED_STUCK, stuck_files);
	download = add_download(fx, series, NUMBERED_STUCK, "nzo_stuck", three);
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand' WHERE download_id = %" G_GINT64_FORMAT,
		download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "not_imported", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT,
		download);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE open AND episode_id = %" G_GINT64_FORMAT, high);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'abandoned' AND text LIKE 'The grab by hand of "
		NUMBERED_STUCK " is let go: its import could take no file of the download.%%S01E03 may be searched for"
		" again; the download is left with its client.'");
	/* Not a grab by hand: it stays open, for a person (as before). */
	g_clear_pointer(&three, g_free);
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", high);
	download = add_download(fx, series, NUMBERED_STUCK, "nzo_stuck_auto", three);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "t", "SELECT closed_at IS NULL FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
}

/* add-manual-search (review): an administrator's accepted Grab sets nothing
 * aside (marked manual, its import judged by the profile); a grab by hand
 * that fails is followed by no automatic search (the history says to choose
 * again), where an automatic one's is. */
static void
test_hand_grab_follow(Fixture *fx, gconstpointer data)
{
	static const Rel storm[] = { { STORM_WEBDL, 1500000000 } };
	static const Rel back[] = { { RETURN_WEBDL, 1500000000 } };
	static const gchar *const storm_files[] = { STORM_WEBDL ".mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gint64 series = add_series(fx);
	gint64 storm_id = episode_id(fx, series, 2, 2);
	gint64 candidate;
	gint64 grab = 0;
	gint64 import_id;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;

	/* Grab, on a release the profile accepts: manual, nothing set aside. */
	set_releases(fx, storm, G_N_ELEMENTS(storm));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, storm_id, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" STORM_WEBDL "' AND accepted");
	g_assert_true(axis_tv_grab(fx->tv, series, candidate, "root", &grab, &error));
	g_assert_no_error(error);
	assert_qf(fx, "t", "SELECT manual AND set_aside IS NULL FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	/* Its import is the profile's: with one that allows no 1080p now, it is
	 * not imported. */
	exec(fx, "UPDATE series SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug = 'uhd') WHERE id = %"
		G_GINT64_FORMAT, series);
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	write_download(fx, STORM_WEBDL, storm_files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 1500000000, "/data/downloads/" STORM_WEBDL, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	{
		g_autofree gchar *state = run_import(fx, import_id);

		if (!g_str_has_prefix(state, "blocked"))
			g_error("the import of an accepted grab under a profile that allows no 1080p: %s", state);
	}
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, storm_id);

	/* Grab this one from a manual search of the whole series, then its
	 * download fails: no search follows it, and the history says why. */
	set_releases(fx, back, G_N_ELEMENTS(back));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SERIES, 0, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	candidate = qi(fx, "SELECT c.id FROM series_candidate c JOIN series_search s ON s.id = c.search_id WHERE c.name = '"
		RETURN_WEBDL "' AND s.scope = 'series'");
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	grab = grabbed.grab_id;
	assert_qf(fx, "series", "SELECT scope FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	g_clear_pointer(&ref, g_free);
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	axis_test_download_fake_set(fx->fake, ref, "failed", 0, NULL, "CRC error");
	poll(fx);
	g_assert_cmpuint(follow(fx).failed, ==, 1);
	assert_qf(fx, "failed", "SELECT close_reason FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'skipped' AND grab_id = %" G_GINT64_FORMAT
		" AND text = 'No search follows the failed grab by hand of " RETURN_WEBDL ": it was chosen from a manual"
		" search, so the next release is chosen by hand too. Use Manual search again, or wait for the scheduled"
		" search.'", grab);
}

/* A failure on one file: what was imported stays; the failed episode is
 * left as it was; tried again, then partly imported; retried by hand. */
static void
import_partial(
	Fixture  *fx,
	gboolean  torrent
){
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *pair = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 download;
	gint64 import_id;

	pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", files);
	download = add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "nzo_pair", pair);
	if (torrent)
		exec(fx, "UPDATE download SET protocol = 'torrent' WHERE id = %" G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	/* S01E02's copy fails, every time. */
	hook_set(fx, "temp", "S01E02", AXIS_IMPORT_HOOK_FAIL, 0);
	assert_import(fx, import_id, "waiting/place_failed");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, arrival);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
	text = q(fx, "SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE '%%S01E02%%'"
		" AND role = 'episode'", import_id);
	g_assert_nonnull(text);
	assert_import(fx, import_id, "waiting/place_failed");
	assert_import(fx, import_id, "partial/partly_imported");
	g_assert_cmpuint(fx->hook.fired, ==, 3);
	/* Imported once; the other episode untouched; said so. */
	g_clear_pointer(&placed, g_free);
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file");
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_contains(text, "Partly imported: 1 of 2 files are in the library");
	assert_contains(text, "S01E02");
	assert_ep(fx, series, 1, 2, AXIS_TV_ATTENTION, "Partly imported");
	assert_ep(fx, series, 1, 1, AXIS_TV_UPGRADE_WANTED, NULL);
	/* Not every wanted file is in: the download stays at its client, the
	 * grab stays open. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "t", "SELECT closed_at IS NULL FROM series_grab");
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'import_blocked'");

	/* Put right, and retried: the rest goes, the first is not placed again. */
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&placed, g_free);
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "2", "SELECT count(*) FROM series_file");
	assert_qf(fx, "2", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND kind = 'video'",
		fx->root_id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "nzo_pair"), ==, 1);
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab");
	assert_ep(fx, series, 1, 2, AXIS_TV_UPGRADE_WANTED, NULL);
	if (torrent)
		/* The torrent goes with its data once every wanted file is in. */
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", "nzo_pair"), ==, 1);
}

static void
test_import_partial(Fixture *fx, gconstpointer data)
{
	import_partial(fx, FALSE);
}

/* The same for a torrent hardlinked (import.take_files hardlink), which is
 * removed with its data only when every wanted file of the pack is in. */
static void
test_import_partial_torrent(Fixture *fx, gconstpointer data)
{
	set(fx, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_HARDLINK);
	import_partial(fx, TRUE);
}

/* Spec (import): A partly imported pack whose download is deleted ("Delete
 * this download"): the episodes placed keep their files and their links, the
 * grab is closed, and the other episodes it held are wanted again.  And A
 * file that is also in the library: the confirmation says that it stays, the
 * download's name goes, the library's file keeps its inode. */
static void
test_delete_download_partial_pack(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *folder = g_build_filename(fx->downloads, "Harbour.Lights.S01E01-E02.WEB-DL", NULL);
	g_autofree gchar *placed = g_build_filename(dir,
		"Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv", NULL);
	g_autofree gchar *pair = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_WAITING;
	GStatBuf before;
	GStatBuf now;
	gint64 download;
	gint64 import_id;

	/* A torrent of two episodes, hardlinked; the second cannot be placed. */
	set(fx, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_HARDLINK);
	pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", files);
	download = add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "torrent_pair", pair);
	exec(fx, "UPDATE download SET protocol = 'torrent' WHERE id = %" G_GINT64_FORMAT, download);
	/* Its client deletes a torrent's files when it removes it with them. */
	axis_test_download_fake_set_data_dir(fx->fake, fx->downloads);
	import_id = request_import(fx, download);
	hook_set(fx, "temp", "S01E02", AXIS_IMPORT_HOOK_FAIL, 0);
	assert_import(fx, import_id, "waiting/place_failed");
	assert_import(fx, import_id, "waiting/place_failed");
	assert_import(fx, import_id, "partial/partly_imported");
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	g_assert_cmpint(g_lstat(placed, &before), ==, 0);
	g_assert_cmpuint((guint)before.st_nlink, ==, 2);
	assert_qf(fx, "t", "SELECT closed_at IS NULL FROM series_grab");

	/* The confirmation: the client deletes; what was placed stays, and is
	 * another name of a file of the download. */
	g_assert_true(axis_import_delete_preview(fx->import, import_id, &preview, &error));
	g_assert_no_error(error);
	if (!preview->allowed)
		g_error("the partly imported pack's download may not be deleted: %s", preview->refusal);
	g_assert_cmpstr(preview->state, ==, "partial");
	g_assert_cmpint(preview->by, ==, AXIS_IMPORT_DELETE_BY_CLIENT);
	g_assert_cmpuint(preview->n_files, ==, 2);
	g_assert_cmpuint(preview->placed->len, ==, 1);
	assert_contains(g_ptr_array_index(preview->placed, 0), "S01E01 - Arrival");
	g_assert_cmpuint(preview->n_linked, ==, 1);
	g_assert_true(preview->has_grab);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	g_assert_true(axis_import_delete_download(fx->import, import_id, preview->digest, FALSE, "ada", &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_DELETE_DELETED);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", "torrent_pair"), ==, 1);
	g_assert_false(g_file_test(folder, G_FILE_TEST_EXISTS));
	/* The episode placed keeps its file (the same inode, one name now) and
	 * its link. */
	g_assert_cmpint(g_lstat(placed, &now), ==, 0);
	g_assert_cmpuint((guint64)now.st_ino, ==, (guint64)before.st_ino);
	g_assert_cmpuint((guint)now.st_nlink, ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, arrival);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file");
	assert_ep(fx, series, 1, 1, AXIS_TV_UPGRADE_WANTED, NULL);
	/* The grab closed, the other episode wanted again, no search queued. */
	assert_qf(fx, "deleted/-", "SELECT close_reason || '/' || coalesce(removal, '-') FROM series_grab");
	assert_ep(fx, series, 1, 2, AXIS_TV_WANTED, NULL);
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	assert_qf(fx, "dismissed/download_deleted", "SELECT state || '/' || reason FROM import WHERE id = %"
		G_GINT64_FORMAT, import_id);
	text = q(fx, "SELECT text FROM series_event WHERE kind = 'removal' AND by_user = 'ada'");
	assert_contains(text, "its download was deleted by ada from the import's page");
	assert_qf(fx, "0", "SELECT count(*) FROM series_blocklist");
}

/* Spec (import): a torrent whose files are moved (the default) is let go of
 * by its client, without its files, before the first of them is moved; a
 * file that cannot be placed stays in the torrent's folder, the import ends
 * partly imported, and a retry moves it once the client says again that it
 * does not have the torrent (nothing more is removed). */
static void
test_import_partial_torrent_moved(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *pair = NULL;
	g_autofree gchar *arrival_source = g_build_filename(fx->downloads, "Harbour.Lights.S01E01-E02.WEB-DL", ARRIVAL_FILE,
		NULL);
	g_autofree gchar *low_tide_source = g_build_filename(fx->downloads, "Harbour.Lights.S01E01-E02.WEB-DL",
		LOW_TIDE_FILE, NULL);
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 download;
	gint64 import_id;

	pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", files);
	download = add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "tor_pair", pair);
	exec(fx, "UPDATE download SET protocol = 'torrent' WHERE id = %" G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	hook_set(fx, "temp", "S01E02", AXIS_IMPORT_HOOK_FAIL, 0);
	assert_import(fx, import_id, "waiting/place_failed");
	/* Let go of first, without its files: removed, then asked again until the
	 * client said it no longer has it. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "tor_pair"), ==, 2);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_assert_false(axis_test_download_fake_has(fx->fake, "tor_pair"));
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	/* S01E01 moved out of the download; S01E02 where it was. */
	g_assert_false(g_file_test(arrival_source, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(low_tide_source, G_FILE_TEST_IS_REGULAR));
	assert_import(fx, import_id, "waiting/place_failed");
	assert_import(fx, import_id, "partial/partly_imported");
	g_assert_true(g_file_test(low_tide_source, G_FILE_TEST_IS_REGULAR));
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");

	/* Retried: the client is asked again whether it has the torrent (it says
	 * it does not: nothing more is removed, and what was recorded stands), and
	 * the rest is moved. */
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&placed, g_free);
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	g_assert_false(g_file_test(low_tide_source, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_assert_false(axis_test_download_fake_has(fx->fake, "tor_pair"));
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "move", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "moved|moved", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role = 'episode'", import_id);
	assert_qf(fx, "2", "SELECT count(*) FROM series_file");
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab");
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
}

/* A run that dies at each step, and the next: each file imported once. */
static void
test_import_crash(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *step;
		guint        nth;
	} crashes[] = {
		{ "decided", 1 }, { "folder", 1 }, { "temp", 1 }, { "copied", 1 }, { "published", 1 }, { "linked", 1 },
		/* The second file, the first done. */
		{ "temp", 2 }, { "published", 2 }, { "linked", 2 }, { "recorded", 1 }
	};
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(crashes); i++) {
		g_autofree gchar *release = g_strdup_printf("Harbour.Lights.S01E01-E02.WEB-DL.Run%u", i);
		g_autofree gchar *ref = g_strdup_printf("nzo_run%u", i);
		g_autofree gchar *message = NULL;
		g_autofree gchar *placed = NULL;
		gint64 import_id;

		write_download(fx, release, files);
		import_id = request_import(fx, add_download(fx, series, release, ref, pair));
		hook_set(fx, crashes[i].step, NULL, AXIS_IMPORT_HOOK_CRASH, crashes[i].nth);
		message = run_import_failing(fx, import_id);
		assert_contains(message, crashes[i].step);
		/* The next run carries on from what is on disk. */
		hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
		assert_import(fx, import_id, "imported");
		placed = videos_in(dir);
		g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv|"
			"Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
		g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
		/* Once: one file each, one link each, one event. */
		assert_qf(fx, "2", "SELECT count(*) FROM series_file");
		assert_qf(fx, "2", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND kind = 'video'"
			" AND state = 'present'", fx->root_id);
		assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, arrival);
		assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
		assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'imported' AND grab_id = (SELECT id FROM"
			" series_grab WHERE download_id = (SELECT download_id FROM import WHERE id = %" G_GINT64_FORMAT "))",
			import_id);
		assert_qf(fx, "imported", "SELECT state FROM download WHERE client_ref = '%s'", ref);
		/* For the next round: the library as it was. */
		{
			g_autofree gchar *series_dir = g_build_filename(fx->shows, SERIES_FOLDER, NULL);
			g_autofree gchar *a = g_build_filename(dir, "Harbour Lights (2024) - S01E01 - Arrival [WEB-DL 1080p]"
				"[EAC3 5.1][x264]-NTb.mkv", NULL);
			g_autofree gchar *b = g_build_filename(dir, "Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p]"
				"[EAC3 5.1][x264]-NTb.mkv", NULL);

			g_assert_cmpint(g_unlink(a), ==, 0);
			g_assert_cmpint(g_unlink(b), ==, 0);
			g_assert_cmpint(g_rmdir(dir), ==, 0);
			g_assert_cmpint(g_rmdir(series_dir), ==, 0);
			exec(fx, "DELETE FROM series_file");
			exec(fx, "DELETE FROM library_file WHERE root_id = %" G_GINT64_FORMAT, fx->root_id);
		}
	}
}

/* Second review, item 3: a series' import taken up again after its first
 * episode was moved, given its name and linked, its season's folder renamed
 * meanwhile (a person): the episode already in the library does not block the
 * import; the other is placed. */
static void
test_import_season_folder_renamed(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *renamed = g_strconcat(dir, " (renamed)", NULL);
	g_autofree gchar *pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	g_autofree gchar *message = NULL;
	g_autofree gchar *placed = NULL;
	gint64 import_id;

	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL.Renamed", files);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL.Renamed", "nzo_renamed",
		pair));
	/* Dies with the first episode moved and linked, the second not begun. */
	hook_set(fx, "temp", NULL, AXIS_IMPORT_HOOK_CRASH, 2);
	message = run_import_failing(fx, import_id);
	assert_contains(message, "temp");
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file");
	g_assert_cmpint(g_rename(dir, renamed), ==, 0);
	assert_import(fx, import_id, "imported");
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E02 - Low Tide [WEB-DL 1080p][EAC3 5.1][x264]-NTb.mkv");
	assert_qf(fx, "2", "SELECT count(*) FROM series_file");
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
}

/* ── Grab by force (add-force-grab-and-assign, F2) ──────────────── */

#define FORCED_ONE    "Lamp.Room.Part.Three.1080p.WEB-DL.x264-GRP"
#define FORCED_PACK   "Quay.Stories.Second.Year.1080p.WEB-DL.x264-GRP"
#define FORCED_BLIND  "Quay.Stories.Two.Parts.1080p.WEB-DL.x264-GRP"
#define BETTER_THIRD  "Harbour.Lights.S01E03.1080p.BluRay.x264-DEMAND"
#define BETTER_LOW    "Harbour.Lights.S01E02.1080p.BluRay.x264-DEMAND"

/* The grab of @download as a grab by force by root leaves it: by hand, bound,
 * for a whole season (@season, -1: for the episodes it claims). */
static void
forced_by_root(
	Fixture *fx,
	gint64   download,
	gint     season
){
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand', bound = true, grabbed_by = 'root',"
		" forced = '[{\"k\": \"safety\", \"c\": \"no_match\", \"t\": \"It is not this series.\"}]'::jsonb,"
		" bound_season = nullif(%d, -1) WHERE download_id = %" G_GINT64_FORMAT, season, download);
}

/* "forced|root|t" for the present file of @episode ("" when it has none). */
static gchar *
hand_of(
	Fixture *fx,
	gint64   episode
){
	g_autofree gchar *found = q(fx, "SELECT string_agg(coalesce(sf.hand_how, '-') || '|' || coalesce(sf.hand_by, '-')"
		" || '|' || sf.kept::text, ',') FROM series_file sf JOIN series_file_episode x ON x.file_id = sf.file_id"
		" JOIN library_file lf ON lf.id = sf.file_id WHERE x.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present'",
		episode);

	return g_strdup(found != NULL ? found : "");
}

/*
 * add-force-grab-and-assign, F2: the import of a grab by force.  The import
 * delta's scenarios "One file for the episode named", "A forced pack whose
 * files say their episodes" and "A file that cannot be mapped"; and a file
 * kept by hand is not replaced without the tick for that very file, by a
 * grab by hand or by force.
 */
static void
test_forced_import(Fixture *fx, gconstpointer data)
{
	static const gchar *const one_file[] = { "lamp.room.part.three.mkv", NULL };
	static const gchar *const better_file[] = { BETTER_THIRD ".mkv", NULL };
	static const gchar *const pack_files[] = {
		"Quay.Stories.S07E01.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.S07E02.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.The.Long.One.1080p.WEB-DL.x264-GRP.mkv",
		NULL
	};
	static const gchar *const blind_files[] = {
		"Quay.Stories.First.Half.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.Other.Half.1080p.WEB-DL.x264-GRP.mkv",
		NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *hand = NULL;
	gint64 series = add_series(fx);
	gint64 third = episode_id(fx, series, 1, 3);
	gint64 download;
	gint64 import_id;

	/* Scenario: One file for the episode named.  The download's one video
	 * file is the episode the person named, whatever its name says; it is
	 * linked as forced by that person, and kept by hand. */
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", third);
	write_download(fx, FORCED_ONE, one_file);
	download = add_download(fx, series, FORCED_ONE, "nzo_forced_one", ids);
	forced_by_root(fx, download, -1);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "imported");
	hand = hand_of(fx, third);
	g_assert_cmpstr(hand, ==, "forced|root|true");
	assert_ep(fx, series, 1, 3, AXIS_TV_IN_LIBRARY, "Kept by hand by root: not upgraded by itself.");
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);

	/* A file kept by hand is replaced only with the tick for that very file.
	 * A better release grabbed by hand without it: held. */
	write_download(fx, BETTER_THIRD, better_file);
	download = add_download(fx, series, BETTER_THIRD, "nzo_better_third", ids);
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand', grabbed_by = 'root' WHERE download_id = %"
		G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/kept_by_hand");
	/* The same by force, the file not ticked on its confirmation: held. */
	forced_by_root(fx, download, -1);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "blocked/not_upgrade");
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, third);
	g_assert_cmpstr(hand, ==, "forced|root|true");
	assert_qf(fx, "", "SELECT coalesce(string_agg(sf.release_name, ','), '') FROM series_file sf"
		" JOIN series_file_episode x ON x.file_id = sf.file_id JOIN library_file lf ON lf.id = sf.file_id"
		" WHERE x.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present' AND sf.release_name LIKE '%%BluRay%%'",
		third);
	/* With its tick, at the quality the confirmation showed: replaced. */
	exec(fx, "UPDATE series_grab SET replaces_files = (SELECT jsonb_build_array(jsonb_build_object('f', sf.file_id,"
		" 'q', sf.quality, 'r', null)) FROM series_file sf JOIN series_file_episode x ON x.file_id = sf.file_id"
		" JOIN library_file lf ON lf.id = sf.file_id WHERE x.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present')"
		" WHERE download_id = %" G_GINT64_FORMAT, third, download);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "bluray_1080p", "SELECT sf.quality FROM series_file sf JOIN series_file_episode x ON x.file_id ="
		" sf.file_id JOIN library_file lf ON lf.id = sf.file_id WHERE x.episode_id = %" G_GINT64_FORMAT
		" AND lf.state = 'present'", third);

	/* Scenarios: A forced pack whose files say their episodes, and A file
	 * that cannot be mapped.  Bound to season 2: each file is placed by the
	 * episode number in its own name within season 2 (the series' name in it
	 * is not judged, nor the season it states); the file whose name gives no
	 * episode is left and listed, and nothing is guessed for it. */
	g_free(ids);
	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}",
		episode_id(fx, series, 2, 1), episode_id(fx, series, 2, 2), episode_id(fx, series, 2, 3),
		episode_id(fx, series, 2, 4));
	write_download(fx, FORCED_PACK, pack_files);
	download = add_download(fx, series, FORCED_PACK, "nzo_forced_pack", ids);
	forced_by_root(fx, download, 2);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, episode_id(fx, series, 2, 1));
	g_assert_cmpstr(hand, ==, "forced|root|true");
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, episode_id(fx, series, 2, 2));
	g_assert_cmpstr(hand, ==, "forced|root|true");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 2 AND e.number IN (3, 4)", series);
	assert_qf(fx, "2", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 2", series);
	assert_qf(fx, "1", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE"
		" '%%The.Long.One%%' AND NOT accepted AND reasons::text LIKE '%%assign it by hand%%'", import_id);
	/* What it could place is in the library, so the grab is done with as any
	 * pack's is; the file left stays in the download, listed on the import,
	 * and the episodes that got none are free again. */
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE open");

	/* No file can be placed on what the person named (two files, neither
	 * naming an episode, for two episodes): held with the reason "assign",
	 * the grab open, nothing guessed. */
	g_free(ids);
	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, 1),
		episode_id(fx, series, 1, 2));
	write_download(fx, FORCED_BLIND, blind_files);
	download = add_download(fx, series, FORCED_BLIND, "nzo_forced_blind", ids);
	forced_by_root(fx, download, -1);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/assign");
	assert_qf(fx, "t", "SELECT closed_at IS NULL FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "t", "SELECT reason_text LIKE '%%nothing was guessed%%' FROM import WHERE id = %" G_GINT64_FORMAT,
		import_id);
	assert_qf(fx, "2", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND reasons::text LIKE '%%assign it by hand%%'", import_id);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 1 AND e.number IN (1, 2)", series);
}

/*
 * One video file for two episodes picked one by one (the other file a sample,
 * or gone on a rerun): it is both only when its own name states them or the
 * person ticked that one file holds them; otherwise the import waits.
 */
static void
test_forced_one_file(Fixture *fx, gconstpointer data)
{
	static const gchar *const one_file[] = { "lamp.room.part.three.mkv", NULL };
	g_autofree gchar *ids = NULL;
	gint64 series = add_series(fx);
	gint64 third = episode_id(fx, series, 1, 3);
	gint64 fourth = episode_id(fx, series, 1, 4);
	gint64 download;
	gint64 import_id;

	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", third, fourth);
	write_download(fx, FORCED_ONE, one_file);
	download = add_download(fx, series, FORCED_ONE, "nzo_one_file", ids);
	forced_by_root(fx, download, -1);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/assign");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT, series);
	/* The person ticked that one file holds both. */
	exec(fx, "UPDATE series_grab SET forced = forced || '[{\"k\": \"choice\", \"c\": \"one_file\", \"t\": \"x\"}]'::jsonb"
		" WHERE download_id = %" G_GINT64_FORMAT, download);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "2", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 1 AND e.number IN (3, 4)", series);
}

/*
 * Review item 7: a bound grab whose import waits for assigning keeps the
 * series busy (no numbering switch, no other grab), the download's clean-up
 * leaves the files it left, and dismissing the import closes the grab and
 * frees its episodes.
 */
static void
test_forced_assign_wait(Fixture *fx, gconstpointer data)
{
	static const gchar *const blind_files[] = {
		"Quay.Stories.First.Half.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.Other.Half.1080p.WEB-DL.x264-GRP.mkv",
		NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *first = NULL;
	AxisImportCleanupReport report;
	gint64 series = add_series(fx);
	gint64 download;
	gint64 import_id;

	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, 1),
		episode_id(fx, series, 1, 2));
	write_download(fx, FORCED_BLIND, blind_files);
	download = add_download(fx, series, FORCED_BLIND, "nzo_wait", ids);
	forced_by_root(fx, download, -1);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/assign");
	/* The numbering stamp of any grab is on a bound one. */
	assert_qf(fx, "t", "SELECT source IS NOT NULL AND source = (SELECT episode_source FROM series WHERE id = %"
		G_GINT64_FORMAT ") FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, series, download);
	/* The grab is open and the series is busy: the numbering is not switched. */
	assert_qf(fx, "t", "SELECT closed_at IS NULL FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	/* (A numbering switch is refused for any open grab: test-tv-numbering.) */
	/* The clean-up leaves the files that were left. */
	first = g_build_filename(fx->downloads, FORCED_BLIND, blind_files[0], NULL);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_folder_removals(fx->import, import_id, &report, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_test(first, G_FILE_TEST_EXISTS));
	/* Dismissed: the grab is closed and its episodes are free. */
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_qf(fx, "dismissed", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE open");
	assert_import(fx, import_id, "dismissed/dismissed");
	g_assert_true(g_file_test(first, G_FILE_TEST_EXISTS));
}

#define MULTI_PACK "Quay.Stories.Complete.1080p.WEB-DL.x264-GRP"

static const gchar *const multi_pack_files[] = {
	"Quay.Stories.S01E01.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S01E02.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S01E03.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S01E04.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S01E05.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S02E01.1080p.WEB-DL.x264-GRP.mkv",
	"Quay.Stories.S02E02.1080p.WEB-DL.x264-GRP.mkv",
	NULL
};

/*
 * A multi-season pack forced for a whole season: a file that resolves to real
 * episodes of another season of the series is another season's, left and
 * listed; it is never re-read within the season bound, and the pack's own
 * files of the bound season are placed.
 */
static void
test_forced_multi_season(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *ids = NULL;
	gint64 series = add_series(fx);
	gint64 download;
	gint64 import_id;

	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}",
		episode_id(fx, series, 2, 1), episode_id(fx, series, 2, 2), episode_id(fx, series, 2, 3),
		episode_id(fx, series, 2, 4));
	write_download(fx, MULTI_PACK, multi_pack_files);
	download = add_download(fx, series, MULTI_PACK, "nzo_multi_pack", ids);
	forced_by_root(fx, download, 2);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 1", series);
	assert_qf(fx, "2", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 2 AND e.number IN (1, 2)", series);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.season_number = 2 AND e.number IN (3, 4)", series);
	assert_qf(fx, "5", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE"
		" '%%S01E0%%' AND NOT accepted", import_id);
	/* The download's clean-up leaves the files that were left (item 7). */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *left = g_build_filename(fx->downloads, MULTI_PACK, multi_pack_files[0], NULL);
		AxisImportCleanupReport report;

		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_folder_removals(fx->import, import_id, &report, &error));
		g_assert_no_error(error);
		g_assert_true(g_file_test(left, G_FILE_TEST_EXISTS));
	}
}

/* Several seasons the series lacks (or none stated beside one): the re-reading
 * is decided once for the download, so none is re-read and all are listed. */
static void
test_forced_mixed_foreign(Fixture *fx, gconstpointer data)
{
	static const gchar *const two_foreign[] = {
		"Quay.Stories.S03E01.1080p.WEB-DL.x264-GRP.mkv", "Quay.Stories.S03E02.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.S04E03.1080p.WEB-DL.x264-GRP.mkv", "Quay.Stories.S04E04.1080p.WEB-DL.x264-GRP.mkv", NULL
	};
	static const gchar *const with_zero[] = {
		"Quay.Stories.S05E01.1080p.WEB-DL.x264-GRP.mkv", "Quay.Stories.S03E02.1080p.WEB-DL.x264-GRP.mkv", NULL
	};
	static const gchar *const *const sets[] = { two_foreign, with_zero };
	gint64 series = add_series(fx);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(sets); i++) {
		g_autofree gchar *ids = NULL;
		g_autofree gchar *release = g_strdup_printf("Quay.Stories.Mixed.%u.1080p.WEB-DL.x264-GRP", i);
		g_autofree gchar *ref = g_strdup_printf("nzo_mixed%u", i);
		gint64 download;
		gint64 import_id;

		ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, 1),
			episode_id(fx, series, 1, 2));
		write_download(fx, release, sets[i]);
		download = add_download(fx, series, release, ref, ids);
		forced_by_root(fx, download, 1);
		import_id = request_import(fx, download);
		assert_import(fx, import_id, "blocked/assign");
		assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
			" WHERE e.series_id = %" G_GINT64_FORMAT, series);
		exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
	}
}

/* The same, the grab bound to single episodes of the season. */
static void
test_forced_multi_season_picks(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = {
		"Quay.Stories.S01E03.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.S01E04.1080p.WEB-DL.x264-GRP.mkv",
		"Quay.Stories.S02E01.1080p.WEB-DL.x264-GRP.mkv",
		NULL
	};
	g_autofree gchar *ids = NULL;
	gint64 series = add_series(fx);
	gint64 download;
	gint64 import_id;

	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", episode_id(fx, series, 2, 3),
		episode_id(fx, series, 2, 4));
	write_download(fx, MULTI_PACK, files);
	download = add_download(fx, series, MULTI_PACK, "nzo_multi_picks", ids);
	forced_by_root(fx, download, -1);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/assign");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode x JOIN series_episode e ON e.id = x.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT, series);
}

/*
 * add-force-grab-and-assign, F2: an episode's file kept by hand.  The tv
 * delta's scenarios "Below the cutoff" (nothing is grabbed for it, alone or
 * under a pack, and no one click replaces it) and "Let go" (it is wanted for
 * an upgrade again); and a person's own grab replaces it only with the tick
 * that names it.
 */
static void
test_kept_by_hand(Fixture *fx, gconstpointer data)
{
	static const gchar *const first[] = { ARRIVAL_FILE, LOW_TIDE_FILE, NULL };
	static const Rel better[] = { { BETTER_LOW, 1500000000 }, { PACK_S01, 6000000000 } };
	static const Rel arrival_better[] = { { BETTER_ARRIVAL, 1500000000 } };
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) named = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *pair = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 arrival_file;
	gint64 low_tide_file;
	gint64 candidate;
	gint64 grab = 0;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;

	/* Two WEB-DL files, below the profile's cutoff: an upgrade is wanted. */
	pair = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", arrival, low_tide);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", first);
	assert_import(fx, request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "nzo_first",
		pair)), "imported");
	assert_ep(fx, series, 1, 2, AXIS_TV_UPGRADE_WANTED, NULL);
	arrival_file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, arrival);
	low_tide_file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
	/* They came from a grab by force by root: kept by hand. */
	exec(fx, "UPDATE series_file SET hand_how = 'forced', hand_by = 'root', hand_at = now(), kept = true"
		" WHERE series_id = %" G_GINT64_FORMAT, series);

	/* Scenario: Below the cutoff.  Not wanted for an upgrade, and said. */
	assert_ep(fx, series, 1, 2, AXIS_TV_IN_LIBRARY, "Kept by hand by root: not upgraded by itself.");
	/* A search of it finds a better release and grabs nothing. */
	set_releases(fx, better, G_N_ELEMENTS(better));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_reason(fx, BETTER_LOW, "kept_by_hand");
	/* add-attention-notices, N2: a release only a person can grab is counted
	 * on its search, for the line "Needs attention" has about them. */
	assert_qf(fx, "t", "SELECT n_person_only >= 1 FROM series_search ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab WHERE closed_at IS NULL AND release_name = '" BETTER_LOW "'");
	/* A pack that would replace it is rejected with that reason. */
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_reason(fx, PACK_S01, "kept_by_hand");
	/* No one click replaces it either. */
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" BETTER_LOW "' ORDER BY id LIMIT 1");
	g_assert_false(axis_tv_grab(fx->tv, series, candidate, "root", &grab, &error));
	g_assert_nonnull(error);
	assert_contains(error->message, "kept by hand");
	g_clear_error(&error);
	g_assert_cmpuint(n_adds(fx), ==, 0);

	/* Scenario: Let go.  Wanted for an upgrade by the ordinary rules again,
	 * said in the history; the link still says how it was made. */
	g_assert_true(axis_tv_set_kept(fx->tv, series, low_tide_file, FALSE, "root", &error));
	g_assert_no_error(error);
	assert_ep(fx, series, 1, 2, AXIS_TV_UPGRADE_WANTED, NULL);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'hand_link' AND by_user = 'root' AND text LIKE 'The file of S01E02 is no longer kept by hand%%'", series);
	assert_qf(fx, "forced|false", "SELECT hand_how || '|' || kept::text FROM series_file WHERE file_id = %" G_GINT64_FORMAT,
		low_tide_file);
	g_assert_false(axis_tv_set_kept(fx->tv, series, 999999999, TRUE, "root", &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND);
	g_clear_error(&error);
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_ep(fx, series, 1, 2, AXIS_TV_DOWNLOADING, BETTER_LOW);

	/* A person's own grab from a manual search, of a better release for the
	 * episode whose file is still kept: only with the tick that names the
	 * file, which is recorded for it. */
	set_releases(fx, arrival_better, G_N_ELEMENTS(arrival_better));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, arrival, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" BETTER_ARRIVAL "' ORDER BY id DESC LIMIT 1");
	g_assert_false(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_nonnull(error);
	assert_contains(error->message, "kept by hand by root: tick the box that names it");
	g_clear_error(&error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_NEEDS_TICK);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	g_array_append_val(named, arrival_file);
	ticks.replace = named;
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	{
		g_autofree gchar *expected = g_strdup_printf("%" G_GINT64_FORMAT, arrival_file);

		assert_qf(fx, expected, "SELECT replaces_files->0->>'f' FROM series_grab WHERE id = %" G_GINT64_FORMAT,
			grabbed.grab_id);
	}
}

/* ── Assign by hand (add-force-grab-and-assign, slice B) ────────── */

#define ASSIGN_PACK   "Harbour.Evenings.Four.Parts.1080p.WEB-DL.x264-GRP"
#define ASSIGN_HAND   "Harbour.Nights.One.Part.1080p.WEB-DL.x264-GRP"
#define ASSIGN_OLD    "Harbour.Mornings.One.Part.1080p.WEB-DL.x264-GRP"

/* The file of @import_id whose path has @part is said to hold @said. */
static void
ask_file(
	Fixture             *fx,
	AxisImportAssignAsk *ask,
	gint64               import_id,
	const gchar         *part,
	const gchar         *said
){
	gint position = (gint)qi(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%%s%%'", import_id, part);

	g_array_append_val(ask->positions, position);
	g_ptr_array_add(ask->said, g_strdup(said));
}

/* The review of @ask, then its apply by root with the review's digest (with
 * @tick, every tick the review asks for is given first): whether it was
 * applied; *@changed and *@out as axis_import_assign() gives them. */
static gboolean
assign_as_reviewed(
	Fixture               *fx,
	gint64                 import_id,
	AxisImportAssignAsk   *ask,
	gboolean               tick,
	gboolean              *changed,
	AxisImportAssignment **out
){
	g_autoptr(AxisImportAssignment) review = NULL;
	g_autoptr(GError) error = NULL;
	gboolean applied = FALSE;
	gboolean stale = FALSE;
	guint i;
	guint k;

	g_assert_true(axis_import_assign_preview(fx->import, import_id, ask, &review, &error));
	g_assert_no_error(error);
	for (i = 0; tick && i < review->files->len; i++) {
		const AxisImportAssignFile *file = g_ptr_array_index(review->files, i);

		if (file->chosen && file->runtime_off)
			g_array_append_val(ask->runtime_ok, file->position);
		for (k = 0; file->chosen && k < file->replaces->len; k++) {
			const AxisImportAssignReplace *r = g_ptr_array_index(file->replaces, k);

			g_array_append_val(ask->replace, r->file_id);
		}
	}
	g_assert_true(axis_import_assign(fx->import, import_id, ask, review->digest != NULL ? review->digest : "none",
		"root", &applied, &stale, out, &error));
	g_assert_no_error(error);
	if (changed != NULL)
		*changed = stale;
	return applied;
}

/* The present files of @episode, each as its inode and the import that
 * linked it ("inode:import"), joined by ",". */
static gchar *
release_files_of(
	Fixture *fx,
	gint64   episode
){
	g_autofree gchar *found = q(fx, "SELECT string_agg(lf.inode::text || ':' || coalesce(sf.import_id::text, '-'), ','"
		" ORDER BY lf.id) FROM series_file sf"
		" JOIN series_file_episode x ON x.file_id = sf.file_id JOIN library_file lf ON lf.id = sf.file_id"
		" WHERE x.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", episode);

	return g_strdup(found != NULL ? found : "");
}

/*
 * add-force-grab-and-assign, slice B: the files of a series' download
 * assigned by hand.  The import delta's scenarios "A pack none of whose
 * files matched", "A grab let go as not imported", "Grabbed under another
 * numbering", "It would replace a file", "Not a video" and "Out of date";
 * and a file that was not ticked is not replaced even when the import is
 * told to import anyway, and an episode another grab claims is refused.
 */
static void
test_assign_import(Fixture *fx, gconstpointer data)
{
	static const gchar *const pack_files[] = {
		"evening.part.one.mkv",
		"evening.part.two.mkv",
		"evening.part.three.mkv",
		"evening.part.four.mkv",
		"evening.parts.rar",
		NULL
	};
	static const gchar *const hand_files[] = { "night.the.only.part.mkv", NULL };
	static const gchar *const old_files[] = { "morning.the.only.part.mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *hand = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series = add_series(fx);
	gint64 first = episode_id(fx, series, 1, 1);
	gint64 second = episode_id(fx, series, 1, 2);
	gint64 third = episode_id(fx, series, 1, 3);
	gint64 download;
	gint64 other;
	gint64 import_id;
	gboolean changed = FALSE;

	/* Scenario: A pack none of whose files matched.  No name gives an
	 * episode of the series: the import is blocked, nothing is guessed. */
	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", first, second);
	write_download(fx, ASSIGN_PACK, pack_files);
	download = add_download(fx, series, ASSIGN_PACK, "nzo_assign_pack", ids);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/nothing_wanted");

	/* Scenario: Not a video.  A post that assigns the archive is refused. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "evening.parts.rar", "S01E01");
	g_assert_false(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_nonnull(out->refused);
	g_assert_nonnull(strstr(out->refused, "not a video file"));
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment");
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* Two files cannot be given the same episode, and an episode the series
	 * does not have is not one. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.one", "S01E01");
	ask_file(fx, ask, import_id, "part.two", "S01E01");
	ask_file(fx, ask, import_id, "part.three", "S09E77");
	g_assert_false(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_false(out->ready);
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment");
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* Scenario: Out of date.  The review is shown, then the import changes
	 * (here: it is no longer the same files' review, as the digest is of
	 * another assignment): nothing is applied. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.one", "S01E01");
	{
		g_autoptr(AxisImportAssignment) review = NULL;
		g_autoptr(AxisImportAssignment) now = NULL;
		gboolean applied = TRUE;

		g_assert_true(axis_import_assign_preview(fx->import, import_id, ask, &review, &error));
		g_assert_no_error(error);
		g_assert_true(review->ready);
		ask_file(fx, ask, import_id, "part.two", "S01E02");
		g_assert_true(axis_import_assign(fx->import, import_id, ask, review->digest, "root", &applied, &changed, &now,
			&error));
		g_assert_no_error(error);
		g_assert_false(applied);
		g_assert_true(changed);
	}
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment");
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	g_clear_pointer(&ask, axis_import_assign_ask_free);

	/* Two of the four are assigned and applied: those two are imported as
	 * S01E01 and S01E02, by that person and kept by hand; the other two are
	 * left in the download, saying so; the import ends imported. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.one", "S01E01");
	ask_file(fx, ask, import_id, "part.two", "s01e02");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, FALSE, &changed, &out));
	assert_qf(fx, "waiting", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "assign_files/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
		G_GINT64_FORMAT " ORDER BY id DESC LIMIT 1", import_id);
	assert_import(fx, import_id, "imported");
	hand = hand_of(fx, first);
	g_assert_cmpstr(hand, ==, "assigned|root|true");
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, second);
	g_assert_cmpstr(hand, ==, "assigned|root|true");
	assert_qf(fx, "2", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND kind = 'video'"
		" AND NOT accepted AND reasons::text LIKE '%%Left alone when the files were assigned by hand%%'", import_id);
	assert_qf(fx, "2", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND library_file_id IS NOT NULL", import_id);
	assert_qf(fx, "imported", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	/* The files left alone are still in the download, and the download is
	 * left with its client: nothing of it is deleted. */
	{
		g_autofree gchar *left = g_build_filename(fx->downloads, ASSIGN_PACK, "evening.part.three.mkv", NULL);
		g_autofree gchar *taken = g_build_filename(fx->downloads, ASSIGN_PACK, "evening.part.four.mkv", NULL);

		g_assert_true(g_file_test(left, G_FILE_TEST_EXISTS));
		g_assert_true(g_file_test(taken, G_FILE_TEST_EXISTS));
	}
	assert_qf(fx, "kept/", "SELECT coalesce(client_removal, '') || '/' || coalesce(folder_removal, '') FROM import"
		" WHERE id = %" G_GINT64_FORMAT, import_id);
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* Scenario: A grab let go as not imported.  A grab by hand whose import
	 * could take no file is closed; a file of that import assigned to an
	 * episode is imported for it. */
	g_free(ids);
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", third);
	write_download(fx, ASSIGN_HAND, hand_files);
	download = add_download(fx, series, ASSIGN_HAND, "nzo_assign_hand", ids);
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand', grabbed_by = 'root' WHERE download_id = %"
		G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "not_imported", "SELECT close_reason FROM series_grab WHERE download_id = %" G_GINT64_FORMAT,
		download);
	/* An episode another grab holds is that grab's. */
	other = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, sent_at, scope, episode_id)"
		" VALUES (%" G_GINT64_FORMAT ", 'Harbour.Lights.S01E03.720p.WEB-DL.x264-OTHER', 'usenet', 'other:third', now(),"
		" 'episode', %" G_GINT64_FORMAT ") RETURNING id", series, third);
	exec(fx, "INSERT INTO series_grab_episode (grab_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")",
		other, third);
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "night.the.only.part", "S01E03");
	g_assert_false(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_nonnull(out->refused);
	g_assert_nonnull(strstr(out->refused, "another grab"));
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment WHERE import_id = %" G_GINT64_FORMAT, import_id);
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE id = %" G_GINT64_FORMAT, other);
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "night.the.only.part", "S01E03");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, third);
	g_assert_cmpstr(hand, ==, "assigned|root|true");
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* Scenarios: Grabbed under another numbering, and It would replace a
	 * file.  A download grabbed while the series followed another numbering
	 * is held; assigned to S01E03 (which has a file now) without the tick,
	 * nothing is applied and the review says which tick is missing. */
	write_download(fx, ASSIGN_OLD, old_files);
	download = add_download(fx, series, ASSIGN_OLD, "nzo_assign_old", ids);
	exec(fx, "UPDATE series_grab SET source = CASE WHEN source = 'tvdb' THEN 'tmdb' ELSE 'tvdb' END"
		" WHERE download_id = %" G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	assert_import(fx, import_id, "blocked/no_title");
	assert_qf(fx, "t", "SELECT reason_text LIKE '%%Assign by hand%%' FROM import WHERE id = %" G_GINT64_FORMAT,
		import_id);
	before = release_files_of(fx, third);
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "morning.the.only.part", "S01E03");
	g_assert_false(assign_as_reviewed(fx, import_id, ask, FALSE, &changed, &out));
	g_assert_false(changed);
	g_assert_nonnull(out->missing);
	g_assert_nonnull(strstr(out->missing, "Tick replacing"));
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment WHERE import_id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* An assignment recorded without the tick (as none can be through the
	 * review) does not replace the file either: the import is blocked, and
	 * "import anyway" does not override it. */
	exec(fx, "INSERT INTO import_assignment (import_id, position, path, episode_ids, by_user) SELECT %" G_GINT64_FORMAT
		", position, path, '{%" G_GINT64_FORMAT "}', 'root' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND kind = 'video'", import_id, third, import_id);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	assert_import(fx, import_id, "blocked/assign");
	g_assert_false(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	g_clear_error(&error);
	after = release_files_of(fx, third);
	g_assert_cmpstr(after, ==, before);
	assert_qf(fx, "0", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT, import_id);

	/* With its tick: imported as S01E03 in the numbering the series follows
	 * now, the old file retired to the recycle folder, not deleted. */
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	assert_import(fx, import_id, "imported");
	g_clear_pointer(&after, g_free);
	after = release_files_of(fx, third);
	g_assert_cmpstr(after, !=, before);
	g_assert_null(strchr(after, ','));
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'",
		import_id);
	g_clear_pointer(&hand, g_free);
	hand = hand_of(fx, third);
	g_assert_cmpstr(hand, ==, "assigned|root|true");
	assert_qf(fx, "t", "SELECT sf.read_source = s.episode_source FROM series_file sf JOIN series s ON s.id = sf.series_id"
		" JOIN series_file_episode x ON x.file_id = sf.file_id JOIN library_file lf ON lf.id = sf.file_id"
		" WHERE x.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", third);
}

static gboolean
episode_has_file(
	Fixture *fx,
	gint64   episode
){
	g_autofree gchar *files = release_files_of(fx, episode);

	return files[0] != '\0';
}

/* An import of two files (S01E01 and S01E02 by name) that ends partly imported:
 * the second file's copy fails.  The first is placed; a third file ("part
 * three") matches nothing.  With @have_third, S01E03 has a file first. */
static gint64
partial_pair_import(
	Fixture  *fx,
	gint64    series,
	gboolean  have_third
){
	static const gchar *const have[] = { "Harbour.Lights.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv", NULL };
	static const gchar *const files[] = { ARRIVAL_FILE, LOW_TIDE_FILE, "evening.part.three.mkv", NULL };
	g_autofree gchar *ids = NULL;
	g_autofree gchar *st = NULL;
	gint64 download;
	gint64 import_id;
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);

	if (have_third) {
		ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", e3);
		write_download(fx, "Harbour.Lights.S01E03.WEB-DL", have);
		download = add_download(fx, series, "Harbour.Lights.S01E03.WEB-DL", "nzo_have3", ids);
		assert_import(fx, request_import(fx, download), "imported");
		g_clear_pointer(&ids, g_free);
	}
	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e1, e2);
	write_download(fx, "Harbour.Lights.S01E01-E02.WEB-DL", files);
	download = add_download(fx, series, "Harbour.Lights.S01E01-E02.WEB-DL", "nzo_pair", ids);
	import_id = request_import(fx, download);
	hook_set(fx, "temp", "S01E02", AXIS_IMPORT_HOOK_FAIL, 0);
	st = run_import(fx, import_id);
	g_free(st);
	st = run_import(fx, import_id);
	g_free(st);
	st = run_import(fx, import_id);
	g_assert_cmpstr(st, ==, "partial/partly_imported");
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	return import_id;
}

/* A file an assigned import placed, then the import assigned again: the
 * episode that has a file of its own and was not part of the assignment keeps
 * it, and the placed file's row stays. */
static void
test_assign_again_keeps_placed(Fixture *fx, gconstpointer data)
{
	static const gchar *const have[] = { LOW_TIDE_FILE, NULL };
	static const gchar *const pack[] = { "Harbour.Lights.S01E02.1080p.WEB-DL.x264-GRP.mkv", "evening.part.four.mkv", NULL };
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *st = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series = add_series(fx);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 e4 = episode_id(fx, series, 1, 4);
	gint64 download;
	gint64 import_id;
	gboolean changed = FALSE;

	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", e2);
	write_download(fx, "Harbour.Lights.S01E02.WEB-DL", have);
	download = add_download(fx, series, "Harbour.Lights.S01E02.WEB-DL", "nzo_have", ids);
	assert_import(fx, request_import(fx, download), "imported");
	before = release_files_of(fx, e2);
	g_assert_cmpstr(before, !=, "");

	/* A pack held under another numbering claims E02 to E04. */
	g_free(ids);
	ids = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e2, e3, e4);
	write_download(fx, "Harbour.Evenings.Pack.1080p.WEB-DL.x264-GRP", pack);
	download = add_download(fx, series, "Harbour.Evenings.Pack.1080p.WEB-DL.x264-GRP", "nzo_k1", ids);
	exec(fx, "UPDATE series_grab SET source = CASE WHEN source = 'tvdb' THEN 'tmdb' ELSE 'tvdb' END"
		" WHERE download_id = %" G_GINT64_FORMAT, download);
	import_id = request_import(fx, download);
	st = run_import(fx, import_id);

	/* The file named S01E02 is S01E03, the other S01E04; the other's copy fails. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "S01E02", "S01E03");
	ask_file(fx, ask, import_id, "part.four", "S01E04");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	hook_set(fx, "temp", "part.four", AXIS_IMPORT_HOOK_FAIL, 0);
	g_free(st);
	st = run_import(fx, import_id);
	g_free(st);
	st = run_import(fx, import_id);
	g_free(st);
	st = run_import(fx, import_id);
	g_assert_cmpstr(st, ==, "partial/partly_imported");
	hook_set(fx, NULL, NULL, AXIS_IMPORT_HOOK_GO, 0);
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);

	/* Assigned again: only the file still to place is chosen. */
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.four", "S01E04");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	/* The placed file's row stands. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_assignment WHERE import_id = %" G_GINT64_FORMAT, import_id), ==, 2);
	g_free(st);
	st = run_import(fx, import_id);
	g_assert_cmpstr(st, ==, "imported");
	after = release_files_of(fx, e2);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT, import_id), ==, 0);
	g_assert_true(episode_has_file(fx, e3));
	g_assert_true(episode_has_file(fx, e4));
}

/* An import partly imported, then assigned: a file it did not say is left
 * alone, not imported by its name. */
static void
test_assign_left_alone_not_imported(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autofree gchar *st = NULL;
	gint64 series = add_series(fx);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 import_id = partial_pair_import(fx, series, FALSE);
	gboolean changed = FALSE;

	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.three", "S01E03");	/* S01E02's file is left alone */
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	st = run_import(fx, import_id);
	g_assert_cmpstr(st, ==, "imported");
	g_assert_false(episode_has_file(fx, e2));
	g_assert_true(episode_has_file(fx, e3));
}

/* A tick holds for the file it was given for: the ticked file changed, so it
 * is not replaced, and the import blocks asking for the tick again. */
static void
test_assign_tick_stale_blocks(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autofree gchar *st = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series = add_series(fx);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 import_id = partial_pair_import(fx, series, TRUE);
	gboolean changed = FALSE;

	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "S01E02", "S01E03");	/* onto E03, which has a file */
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_cmpint(qi(fx, "SELECT cardinality(replaces) FROM import_assignment WHERE import_id = %" G_GINT64_FORMAT
		" AND episode_ids = ARRAY[%" G_GINT64_FORMAT "]::bigint[]", import_id, e3), ==, 1);
	/* The ticked file changes before the run. */
	exec(fx, "UPDATE series_file SET quality = 'webdl_720p' WHERE file_id IN (SELECT file_id FROM series_file_episode"
		" WHERE episode_id = %" G_GINT64_FORMAT ")", e3);
	before = release_files_of(fx, e3);
	st = run_import(fx, import_id);
	after = release_files_of(fx, e3);
	g_assert_cmpstr(after, ==, before);
	/* Blocked, asking for the files to be assigned again (and ticked again). */
	g_assert_cmpstr(st, ==, "blocked/assign");
}

/* A file published and not yet linked counts as placed: assigning again keeps
 * its row. */
static void
test_assign_again_keeps_published(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	gint64 series = add_series(fx);
	gint64 import_id = partial_pair_import(fx, series, FALSE);
	gboolean changed = FALSE;

	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "S01E02", "S01E02");
	ask_file(fx, ask, import_id, "part.three", "S01E03");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_clear_pointer(&ask, axis_import_assign_ask_free);
	g_clear_pointer(&out, axis_import_assignment_free);
	exec(fx, "UPDATE import_file SET step = 'published', library_file_id = NULL WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%S01E02%%'", import_id);
	/* The import stopped there, partly imported. */
	exec(fx, "UPDATE import SET state = 'partial', reason = 'partly_imported', finished_at = now() WHERE id = %" G_GINT64_FORMAT, import_id);
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.three", "S01E03");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_assignment WHERE import_id = %" G_GINT64_FORMAT, import_id), ==, 2);
}

/* An override given earlier does not cover a file assigned by hand. */
static void
test_assign_clears_overrides(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportAssignAsk) ask = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	gint64 series = add_series(fx);
	gint64 import_id = partial_pair_import(fx, series, FALSE);
	gboolean changed = FALSE;

	exec(fx, "UPDATE import SET override_codes = '{not_upgrade,runtime}' WHERE id = %" G_GINT64_FORMAT, import_id);
	ask = axis_import_assign_ask_new();
	ask_file(fx, ask, import_id, "part.three", "S01E03");
	g_assert_true(assign_as_reviewed(fx, import_id, ask, TRUE, &changed, &out));
	g_assert_cmpint(qi(fx, "SELECT cardinality(override_codes) FROM import WHERE id = %" G_GINT64_FORMAT, import_id), ==, 0);
}

/* ── Duplicates (add-force-grab-and-assign, slice D) ────────────── */

/* A video file in season 1's folder, found by a scan and linked to
 * @episodes ("{12,13}"); its library file id. */
static gint64
linked_file(
	Fixture     *fx,
	gint64       series,
	const gchar *name,
	gsize        size,
	guint        seed,
	const gchar *quality,
	const gchar *episodes
){
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	gint64 id;

	write_media(path, EPISODE_LINE, size, seed);
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	id = qi(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%/%s'", name);
	g_assert_cmpint(id, >, 0);
	exec(fx, "INSERT INTO series_file (file_id, series_id, quality, release_name) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", '%s', '%s')", id, series, quality, name);
	exec(fx, "INSERT INTO series_file_episode (file_id, episode_id) SELECT %" G_GINT64_FORMAT ", unnest('%s'::bigint[])",
		id, episodes);
	/* Analysed as it is, 45 minutes: without that the page recommends
	 * nothing, and the file may not be the one an episode is left with. */
	exec(fx, "INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, resolution_class) SELECT f.id, 'ok', 1, f.size, f.mtime_sec,"
		" f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, 'matroska', 45 * 60000, '1080p' FROM library_file f"
		" WHERE f.id = %" G_GINT64_FORMAT " ON CONFLICT (file_id) DO NOTHING", id);
	return id;
}

/* ── Upgrades over files brought over (import-from-arr, slice S2) ─ */

/* The link of @file as bringing a series over from Sonarr makes it (no
 * import, no identity, the mark "brought", read under the series' numbering)
 * and as it is once a person let it go with "Let Axis upgrade these files":
 * not kept.  (Bringing over records it kept, as decided on
 * 2026-11-21: tests/test-web-sonarr-run.c, run/kept.) */
static void
mark_brought(
	Fixture *fx,
	gint64   file
){
	exec(fx, "UPDATE series_file sf SET hand_how = 'brought', hand_by = 'root', hand_at = now(), kept = false,"
		" read_source = (SELECT s.episode_source FROM series s WHERE s.id = sf.series_id) WHERE sf.file_id = %"
		G_GINT64_FORMAT, file);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_how = 'brought'"
		" AND import_id IS NULL AND identity_id IS NULL AND NOT kept", file);
}

#define SONARR_E01 "Harbour Lights - S01E01 - Arrival WEBDL-1080p.mkv"
#define SONARR_E02 "Harbour Lights - S01E02 - Low Tide WEBDL-1080p.mkv"
#define SONARR_E03 "Harbour Lights - S01E03-E04 - High Water WEBDL-1080p.mkv"

/*
 * What an upgrade does with an episode file brought over from Sonarr, named
 * as Sonarr named it, once a person let it go: it is judged for upgrades as
 * any file, and the better release replaces it through the ordinary retirement:
 * the very file goes to the recycle folder of its disk, nothing is deleted
 * outright, the episode is never left with two copies, and the other brought
 * files are not touched.
 */
static void
test_upgrade_of_brought(Fixture *fx, gconstpointer data)
{
	static const gchar *const better[] = { "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND.mkv", NULL };
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *old_path = g_build_filename(dir, SONARR_E01, NULL);
	g_autofree gchar *other_path = g_build_filename(dir, SONARR_E02, NULL);
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "series", SERIES_FOLDER, "Season 01",
		SONARR_E01, NULL);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *two = NULL;
	gint64 series = add_series(fx);
	gint64 arrival = episode_id(fx, series, 1, 1);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	GStatBuf old_st;
	GStatBuf now_st;
	gint64 old_file;
	gint64 other_file;
	gint64 import_id;

	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", arrival);
	two = g_strdup_printf("{%" G_GINT64_FORMAT "}", low_tide);
	old_file = linked_file(fx, series, SONARR_E01, 64 * 1024, 3, "webdl_1080p", one);
	other_file = linked_file(fx, series, SONARR_E02, 64 * 1024, 4, "webdl_1080p", two);
	mark_brought(fx, old_file);
	mark_brought(fx, other_file);
	g_assert_cmpint(g_lstat(old_path, &old_st), ==, 0);
	/* Not kept: wanted for an upgrade by the ordinary rules. */
	assert_ep(fx, series, 1, 1, AXIS_TV_UPGRADE_WANTED, NULL);

	write_download(fx, "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND", better);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND",
		"nzo_over_brought", one));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	/* The new file under Axis's name beside Sonarr's other file; Sonarr's
	 * file of the episode is not there any more, and not a second copy. */
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		SONARR_E02);
	g_assert_false(g_file_test(old_path, G_FILE_TEST_EXISTS));
	/* Retired, the same file, to the recycle folder: not deleted. */
	g_assert_cmpint(g_lstat(recycled, &now_st), ==, 0);
	g_assert_cmpuint((guint64)now_st.st_ino, ==, (guint64)old_st.st_ino);
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'",
		import_id);
	/* One file on the episode: the import's, with no mark of bringing over. */
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode sfe JOIN series_file sf ON sf.file_id = sfe.file_id"
		" JOIN library_file lf ON lf.id = sf.file_id WHERE sfe.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present'"
		" AND sf.import_id IS NOT NULL AND sf.hand_how IS NULL AND NOT sf.kept AND sf.quality = 'bluray_1080p'", arrival);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode sfe JOIN library_file lf ON lf.id = sfe.file_id"
		" WHERE sfe.file_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", old_file);
	assert_ep(fx, series, 1, 1, AXIS_TV_IN_LIBRARY, NULL);
	/* The other brought file: where it was, linked as it was. */
	g_assert_true(g_file_test(other_path, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "brought", "SELECT sf.hand_how FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id ="
		" sf.file_id WHERE sfe.episode_id = %" G_GINT64_FORMAT, low_tide);
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
}

/* A season pack over several brought files, one of them a double episode:
 * each is retired to the recycle folder as its episodes get their new files,
 * none is deleted outright, and no episode is left with two. */
static void
test_pack_over_brought(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = {
		"Harbour.Lights.S01E01.1080p.BluRay.x264-DEMAND.mkv", "Harbour.Lights.S01E02.1080p.BluRay.x264-DEMAND.mkv",
		"Harbour.Lights.S01E03.1080p.BluRay.x264-DEMAND.mkv", "Harbour.Lights.S01E04.1080p.BluRay.x264-DEMAND.mkv", NULL
	};
	static const gchar *const olds[] = { SONARR_E01, SONARR_E02, SONARR_E03 };
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *all = NULL;
	gint64 series = add_series(fx);
	guint64 inodes[G_N_ELEMENTS(olds)];
	gint64 import_id;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(olds); i++) {
		g_autofree gchar *path = g_build_filename(dir, olds[i], NULL);
		g_autofree gchar *on = i < 2 ? g_strdup_printf("{%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, (gint)i + 1)) :
			g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", episode_id(fx, series, 1, 3),
				episode_id(fx, series, 1, 4));
		GStatBuf st;

		mark_brought(fx, linked_file(fx, series, olds[i], 64 * 1024, 10 + i, "webdl_1080p", on));
		g_assert_cmpint(g_lstat(path, &st), ==, 0);
		inodes[i] = (guint64)st.st_ino;
	}
	all = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}",
		episode_id(fx, series, 1, 1), episode_id(fx, series, 1, 2), episode_id(fx, series, 1, 3),
		episode_id(fx, series, 1, 4));
	write_download(fx, "Harbour.Lights.S01.1080p.BluRay.x264-DEMAND", files);
	import_id = request_import(fx, add_download(fx, series, "Harbour.Lights.S01.1080p.BluRay.x264-DEMAND",
		"nzo_pack_over_brought", all));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	/* The season's folder holds the four new files and none of Sonarr's. */
	placed = videos_in(dir);
	g_assert_cmpstr(placed, ==, "Harbour Lights (2024) - S01E01 - Arrival [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E02 - Low Tide [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E03 - High Water (1) [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv|"
		"Harbour Lights (2024) - S01E04 - High Water (2) [BluRay 1080p][EAC3 5.1][x264]-DEMAND.mkv");
	/* Each of Sonarr's three is in the recycle folder, the very file. */
	for (i = 0; i < G_N_ELEMENTS(olds); i++) {
		g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "series", SERIES_FOLDER, "Season 01",
			olds[i], NULL);
		GStatBuf st;

		g_assert_cmpint(g_lstat(recycled, &st), ==, 0);
		g_assert_cmpuint((guint64)st.st_ino, ==, inodes[i]);
	}
	assert_qf(fx, "3", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'",
		import_id);
	/* One present file an episode, the import's; no brought link is left. */
	for (i = 1; i <= 4; i++) {
		assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode sfe JOIN library_file lf ON lf.id = sfe.file_id"
			" WHERE sfe.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", episode_id(fx, series, 1, (gint)i));
		assert_ep(fx, series, 1, (gint)i, AXIS_TV_IN_LIBRARY, NULL);
	}
	assert_qf(fx, "0", "SELECT count(*) FROM series_file sf JOIN library_file lf ON lf.id = sf.file_id"
		" WHERE lf.state = 'present' AND sf.hand_how = 'brought'");
	g_assert_cmpuint(count_hidden(fx->shows), ==, 0);
}

/* The groups as the Duplicates page lists them: "label:n,label:n". */
static gchar *
dup_groups(Fixture *fx)
{
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GError) error = NULL;
	GString *out = g_string_new(NULL);
	guint i;

	g_assert_true(axis_import_duplicates_list(fx->import, &groups, &error));
	g_assert_no_error(error);
	for (i = 0; i < groups->len; i++) {
		const AxisImportDupGroup *g = g_ptr_array_index(groups, i);

		g_string_append_printf(out, "%s%s:%u", i > 0 ? "," : "", g->label, g->files->len);
	}
	return g_string_free(out, FALSE);
}

static const AxisImportDupFile *
dup_file_of(
	const AxisImportDupGroup *group,
	gint64                    id
){
	guint i;

	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);

		if (f->file->library_file_id == id)
			return f;
	}
	g_error("file %" G_GINT64_FORMAT " is not in the group %s", id, group->label);
	return NULL;
}

/* A removal of @files (ended by 0) of @episode's group, made from a review
 * just taken; the outcome, and why it was refused. */
static AxisImportDupOutcome
dup_remove(
	Fixture       *fx,
	gint64         series,
	gint64         episode,
	const gint64  *files,
	gchar        **refusal
){
	g_autoptr(AxisImportDupGroup) review = NULL;
	g_autoptr(AxisImportDupGroup) judged = NULL;
	g_autoptr(GArray) remove = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) error = NULL;
	AxisImportDupOutcome outcome = AXIS_IMPORT_DUP_GONE;
	guint i;

	*refusal = NULL;
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, episode, &review, &error));
	g_assert_no_error(error);
	g_assert_nonnull(review);
	for (i = 0; files[i] != 0; i++)
		g_array_append_val(remove, files[i]);
	g_assert_true(axis_import_duplicates_remove(fx->import, AXIS_TV_MODULE_ID, series, episode, remove, review->digest,
		"root", &outcome, refusal, &judged, &error));
	g_assert_no_error(error);
	return outcome;
}

static guint64
inode_at(const gchar *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return (guint64)st.st_ino;
}

/*
 * add-force-grab-and-assign, slice D, for a series.  The import delta's
 * scenarios "A double episode" (one file on two episodes is no duplicate),
 * "An episode with two files", "The recommended file removed", "Every file
 * ticked" and the grab in flight; and what only a series has: a file that
 * is the only file of another episode is never offered, and no removal
 * leaves an episode without a file.
 */
static void
test_duplicates(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDupGroup) group = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *listed = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *good_path = g_build_filename(dir, "Harbour Lights (2024) - S01E03 [WEB-DL 1080p].mkv", NULL);
	g_autofree gchar *poor_path = g_build_filename(dir, "Harbour Lights (2024) - S01E03 [WEB-DL 720p].mkv", NULL);
	g_autofree gchar *double_path = g_build_filename(dir, "Harbour Lights (2024) - S01E01-E02 [WEB-DL 1080p].mkv", NULL);
	g_autofree gchar *single_path = g_build_filename(dir, "Harbour Lights (2024) - S01E01 [WEB-DL 720p].mkv", NULL);
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "series", SERIES_FOLDER, "Season 01",
		"Harbour Lights (2024) - S01E03 [WEB-DL 720p].mkv", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *one_two = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *three = NULL;
	gint64 series = add_series(fx);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 twin_a, twin_b, single, good, poor;
	gint64 files[3] = { 0, 0, 0 };
	guint64 good_inode;
	gsize before_size = 0;
	gsize after_size = 0;

	one_two = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e1, e2);
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", e1);
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", e3);

	/* Scenario: A double episode.  One file on S01E01 and S01E02, and no
	 * other file on either: nothing is listed. */
	twin_a = linked_file(fx, series, "Harbour Lights (2024) - S01E01-E02 [WEB-DL 1080p].mkv", 128 * 1024, 1,
		"webdl_1080p", one_two);
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "");
	g_clear_pointer(&listed, g_free);

	/* Scenario: An episode with two files. */
	good = linked_file(fx, series, "Harbour Lights (2024) - S01E03 [WEB-DL 1080p].mkv", 128 * 1024, 2, "webdl_1080p",
		three);
	poor = linked_file(fx, series, "Harbour Lights (2024) - S01E03 [WEB-DL 720p].mkv", 64 * 1024, 3, "webdl_720p", three);
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "Harbour Lights (2024) S01E03:2");
	g_clear_pointer(&listed, g_free);
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e3, &group, &error));
	g_assert_no_error(error);
	g_assert_nonnull(group);
	g_assert_null(group->busy);
	g_assert_cmpint(dup_file_of(group, good)->advice, ==, AXIS_IMPORT_DUP_KEEP);
	g_assert_cmpint(dup_file_of(group, poor)->advice, ==, AXIS_IMPORT_DUP_REMOVE);
	g_assert_true(dup_file_of(group, poor)->ticked);
	g_clear_pointer(&group, axis_import_dup_group_free);

	/* A second file on S01E01 alone: S01E01 has two, and the double file is
	 * held, as it is the only file of S01E02. */
	single = linked_file(fx, series, "Harbour Lights (2024) - S01E01 [WEB-DL 720p].mkv", 64 * 1024, 4, "webdl_720p", one);
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "Harbour Lights (2024) S01E01:2,Harbour Lights (2024) S01E03:2");
	g_clear_pointer(&listed, g_free);
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e1, &group, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(dup_file_of(group, twin_a)->held, ==, "it is the only file of S01E02 that is on a disk Axis can "
		"read now");
	g_assert_cmpint(dup_file_of(group, twin_a)->advice, ==, AXIS_IMPORT_DUP_STAYS);
	g_assert_nonnull(strstr(dup_file_of(group, twin_a)->also, "It also holds S01E02"));
	g_assert_cmpint(dup_file_of(group, single)->advice, ==, AXIS_IMPORT_DUP_REMOVE);
	g_clear_pointer(&group, axis_import_dup_group_free);
	files[0] = twin_a;
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "cannot be removed here: it is the only file of S01E02"));
	g_clear_pointer(&refusal, g_free);
	g_assert_true(g_file_test(double_path, G_FILE_TEST_IS_REGULAR));

	/* A second double file: neither is the only file of S01E02 now, so both
	 * are offered; but removing both, keeping the single file of S01E01,
	 * would leave S01E02 without a file. */
	twin_b = linked_file(fx, series, "Harbour Lights (2024) - S01E01-E02 [WEB-DL 1080p] [2].mkv", 96 * 1024, 5,
		"webdl_1080p", one_two);
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "Harbour Lights (2024) S01E01:3,Harbour Lights (2024) S01E02:2,"
		"Harbour Lights (2024) S01E03:2");
	g_clear_pointer(&listed, g_free);
	files[0] = twin_a;
	files[1] = twin_b;
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "would leave S01E02 without a file"));
	g_clear_pointer(&refusal, g_free);
	/* Scenario: Every file ticked. */
	files[0] = good;
	files[1] = poor;
	g_assert_cmpint(dup_remove(fx, series, e3, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "One must stay"));
	g_clear_pointer(&refusal, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file"), ==, 5);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle"), ==, 0);
	g_assert_true(g_file_test(double_path, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(poor_path, G_FILE_TEST_IS_REGULAR));

	/* Scenario: The recommended file removed.  It is in the recycle folder
	 * and off the episode; the other is the very file it was. */
	good_inode = inode_at(good_path);
	g_assert_true(g_file_get_contents(good_path, &before, &before_size, NULL));
	files[0] = poor;
	files[1] = 0;
	g_assert_cmpint(dup_remove(fx, series, e3, files, &refusal), ==, AXIS_IMPORT_DUP_DONE);
	g_assert_null(refusal);
	g_assert_false(g_file_test(poor_path, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(recycled, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(inode_at(good_path), ==, good_inode);
	g_assert_true(g_file_get_contents(good_path, &after, &after_size, NULL));
	g_assert_cmpmem(before, before_size, after, after_size);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3), ==, good);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT, poor), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file"), ==, 4);
	assert_qf(fx, "kept|true", "SELECT state || '|' || (import_id IS NULL AND remove_after IS NOT NULL)::text"
		" FROM import_recycle");
	assert_qf(fx, "root", "SELECT by_user FROM series_event WHERE kind = 'hand_link' AND series_id = %" G_GINT64_FORMAT,
		series);
	assert_qf(fx, "true", "SELECT (text LIKE '%%Harbour Lights (2024) S01E03%%' AND text LIKE '%%[WEB-DL 720p].mkv%%')"
		"::text FROM series_event WHERE kind = 'hand_link'");
	/* Nothing else moved. */
	g_assert_true(g_file_test(double_path, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(single_path, G_FILE_TEST_IS_REGULAR));
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "Harbour Lights (2024) S01E01:3,Harbour Lights (2024) S01E02:2");
	g_clear_pointer(&listed, g_free);

	/* A grab in flight for S01E02, which the double files also hold: nothing
	 * of S01E01 is removed beside it. */
	{
		g_autofree gchar *two = g_strdup_printf("{%" G_GINT64_FORMAT "}", e2);

		add_download(fx, series, "Harbour.Lights.S01E02.1080p.BluRay.x264-GRP", "ref-dup", two);
	}
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e1, &group, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(group->busy, "A download for S01E02 is on the way"));
	g_clear_pointer(&group, axis_import_dup_group_free);
	files[0] = single;
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "is on the way"));
	g_assert_true(g_file_test(single_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle"), ==, 1);
}

/*
 * add-force-grab-and-assign, slice D, the review's blocker B1: an episode
 * must not lose its only real file through a file it shares with another
 * episode.  S01E01 has A (S01E01-E02) and B; S01E02 has A and C.  Ticking A
 * on S01E01 is fine while C is a file S01E02 can be left with; it is refused
 * when C is only a row of the index: gone from its disk, unreadable, or not
 * analysed.  And A is not even offered when C is on a disk that is offline.
 */
static void
test_duplicates_shared_file(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDupGroup) group = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *listed = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *a_path = g_build_filename(dir, "Harbour Lights (2024) - S01E01-E02 [WEB-DL 1080p].mkv", NULL);
	g_autofree gchar *c_path = g_build_filename(dir, "Harbour Lights (2024) - S01E02 [WEB-DL 720p].mkv", NULL);
	g_autofree gchar *one_two = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *two = NULL;
	gint64 series = add_series(fx);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 a, b, c;
	gint64 other_volume;
	gint64 files[2] = { 0, 0 };

	one_two = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e1, e2);
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", e1);
	two = g_strdup_printf("{%" G_GINT64_FORMAT "}", e2);
	a = linked_file(fx, series, "Harbour Lights (2024) - S01E01-E02 [WEB-DL 1080p].mkv", 128 * 1024, 1, "webdl_1080p",
		one_two);
	b = linked_file(fx, series, "Harbour Lights (2024) - S01E01 [WEB-DL 720p].mkv", 64 * 1024, 2, "webdl_720p", one);
	c = linked_file(fx, series, "Harbour Lights (2024) - S01E02 [WEB-DL 720p].mkv", 64 * 1024, 3, "webdl_720p", two);
	listed = dup_groups(fx);
	g_assert_cmpstr(listed, ==, "Harbour Lights (2024) S01E01:2,Harbour Lights (2024) S01E02:2");
	files[0] = a;

	/* C is whole: A is offered on S01E01, and nothing holds it. */
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e1, &group, &error));
	g_assert_no_error(error);
	g_assert_null(dup_file_of(group, a)->held);
	g_assert_null(dup_file_of(group, a)->fixed);
	g_clear_pointer(&group, axis_import_dup_group_free);

	/* C could not be read when it was analysed: S01E02 would be left with a
	 * file that may be cut short. */
	exec(fx, "UPDATE media_info SET status = 'failed', error = 'cut short', duration_ms = NULL, container = NULL,"
		" resolution_class = NULL WHERE file_id = %" G_GINT64_FORMAT, c);
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "would leave S01E02 without a file Axis can see on its disk and read now"));
	g_assert_nonnull(strstr(refusal, "[WEB-DL 720p].mkv could not be read as a video"));
	g_clear_pointer(&refusal, g_free);
	/* C was never analysed. */
	exec(fx, "DELETE FROM media_info WHERE file_id = %" G_GINT64_FORMAT, c);
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "has not been analysed"));
	g_clear_pointer(&refusal, g_free);
	exec(fx, "INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec, mtime_nsec, ctime_sec,"
		" ctime_nsec, inode, container, duration_ms, resolution_class) SELECT f.id, 'ok', 1, f.size, f.mtime_sec,"
		" f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, 'matroska', 45 * 60000, '1080p' FROM library_file f"
		" WHERE f.id = %" G_GINT64_FORMAT, c);

	/* C is recorded on a disk that is offline: A is the only file of S01E02
	 * on a disk Axis can read, so it is held and has no tick at all. */
	exec(fx, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES"
		" ('6f1d2c3e-0000-4000-8000-00000000d152', 'Disk two', '/nowhere/disk2', false, 'offline', now())");
	other_volume = qi(fx, "SELECT id FROM volume WHERE name = 'Disk two'");
	exec(fx, "UPDATE library_file SET volume_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT, other_volume, c);
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e1, &group, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(dup_file_of(group, a)->held, ==, "it is the only file of S01E02 that is on a disk Axis can read "
		"now");
	g_assert_cmpint(dup_file_of(group, a)->advice, ==, AXIS_IMPORT_DUP_STAYS);
	g_clear_pointer(&group, axis_import_dup_group_free);
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "cannot be removed here: it is the only file of S01E02"));
	g_clear_pointer(&refusal, g_free);
	exec(fx, "UPDATE library_file SET volume_id = %" G_GINT64_FORMAT " WHERE id = %" G_GINT64_FORMAT, fx->volume_id, c);

	/* C is gone from its disk, and the library has not been scanned since:
	 * its row still says present.  S01E02 would be left with a file that is
	 * not there. */
	g_assert_cmpint(g_unlink(c_path), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
		c), ==, 1);
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_nonnull(strstr(refusal, "would leave S01E02 without a file Axis can see on its disk and read now"));
	g_assert_nonnull(strstr(refusal, "is not on its disk where the library records it"));
	g_clear_pointer(&refusal, g_free);
	g_assert_true(g_file_test(a_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file"), ==, 3);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle"), ==, 0);

	/* The file that holds S01E01 alone may still go: A stays for both. */
	files[0] = b;
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_DONE);
	g_assert_null(refusal);
	g_assert_true(g_file_test(a_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1), ==, 1);
	assert_qf(fx, "Harbour Lights (2024) S01E01|root", "SELECT label || '|' || by_user FROM import_recycle");
}

/* What another session does while a removal is under way: @sql, run once
 * when the removal reaches @step (on this thread: the removal is called
 * here). */
typedef struct
{
	Fixture     *fx;
	const gchar *step;
	gchar       *sql;
	guint        ran;
} Meanwhile;

static AxisImportHookVerdict
meanwhile_hook(
	const gchar *step,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
){
	Meanwhile *m = data;

	if (g_strcmp0(step, m->step) == 0 && m->ran == 0) {
		m->ran++;
		exec(m->fx, "%s", m->sql);
	}
	return AXIS_IMPORT_HOOK_GO;
}

/*
 * add-force-grab-and-assign, slice D, the re-review's R1 and R2.  A ticked
 * file whose link changes while the removal is under way (identification, or
 * a person on the links page, in another session) is another episode's file
 * by then: it is not moved.  And a file whose recycle row is gone when its
 * move ends (the move took so long that the cleanup forgot the row) gets its
 * row again: it stays listed, and is deleted in time.
 */
static void
test_duplicates_relinked(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDupGroup) review = NULL;
	g_autoptr(AxisImportDupGroup) judged = NULL;
	g_autoptr(GArray) remove = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) error = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *q_path = g_build_filename(dir, "Harbour Lights (2024) - S01E01 [WEB-DL 720p].mkv", NULL);
	g_autofree gchar *one = NULL;
	g_autofree gchar *three = NULL;
	gint64 series = add_series(fx);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 q;
	gint64 files[2] = { 0, 0 };
	AxisImportDupOutcome outcome = AXIS_IMPORT_DUP_GONE;
	Meanwhile meanwhile;

	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", e1);
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", e3);
	linked_file(fx, series, "Harbour Lights (2024) - S01E01 [WEB-DL 1080p].mkv", 128 * 1024, 1, "webdl_1080p", one);
	q = linked_file(fx, series, "Harbour Lights (2024) - S01E01 [WEB-DL 720p].mkv", 64 * 1024, 2, "webdl_720p", one);
	linked_file(fx, series, "Harbour Lights (2024) - S01E03 [WEB-DL 1080p].mkv", 128 * 1024, 3, "webdl_1080p", three);
	files[0] = q;
	g_array_append_val(remove, q);

	/* Between the review and the post: the file is S01E03's by the time of
	 * the post, and S01E01 has one file.  Nothing is removed. */
	g_assert_true(axis_import_duplicates_review(fx->import, AXIS_TV_MODULE_ID, series, e1, &review, &error));
	g_assert_no_error(error);
	g_assert_nonnull(review);
	exec(fx, "UPDATE series_file_episode SET episode_id = %" G_GINT64_FORMAT " WHERE file_id = %" G_GINT64_FORMAT, e3, q);
	g_assert_true(axis_import_duplicates_remove(fx->import, AXIS_TV_MODULE_ID, series, e1, remove, review->digest, "root",
		&outcome, &refusal, &judged, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, !=, AXIS_IMPORT_DUP_DONE);
	g_assert_true(g_file_test(q_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle"), ==, 0);
	g_clear_pointer(&refusal, g_free);
	exec(fx, "UPDATE series_file_episode SET episode_id = %" G_GINT64_FORMAT " WHERE file_id = %" G_GINT64_FORMAT, e1, q);

	/* Between the checks and the move: the group was read, the digest held
	 * and the ticks were accepted, and only then is the file linked to
	 * S01E03, which has another good file.  The owner locks the links and
	 * reads them again: the file is no longer S01E01's, and it is not moved. */
	memset(&meanwhile, 0, sizeof(meanwhile));
	meanwhile.fx = fx;
	meanwhile.step = "duplicate_checked";
	meanwhile.sql = g_strdup_printf("UPDATE series_file_episode SET episode_id = %" G_GINT64_FORMAT " WHERE file_id = %"
		G_GINT64_FORMAT, e3, q);
	axis_import_set_hook(fx->import, meanwhile_hook, &meanwhile);
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_REFUSED);
	g_assert_cmpuint(meanwhile.ran, ==, 1);
	g_assert_nonnull(strstr(refusal, "A ticked file is no longer linked to this episode"));
	g_clear_pointer(&refusal, g_free);
	g_assert_true(g_file_test(q_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import_recycle"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, q, e3), ==, 1);
	g_clear_pointer(&meanwhile.sql, g_free);
	exec(fx, "UPDATE series_file_episode SET episode_id = %" G_GINT64_FORMAT " WHERE file_id = %" G_GINT64_FORMAT, e1, q);

	/* The recycle row is gone when the move ends: it is written again, kept,
	 * with what the file was and when it is deleted. */
	memset(&meanwhile, 0, sizeof(meanwhile));
	meanwhile.fx = fx;
	meanwhile.step = "duplicate_moved";
	meanwhile.sql = g_strdup("DELETE FROM import_recycle WHERE state = 'moving'");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*duplicates: the recycle row of a file that was moved was gone*");
	g_assert_cmpint(dup_remove(fx, series, e1, files, &refusal), ==, AXIS_IMPORT_DUP_DONE);
	g_assert_cmpuint(meanwhile.ran, ==, 1);
	g_assert_null(refusal);
	g_assert_false(g_file_test(q_path, G_FILE_TEST_EXISTS));
	assert_qf(fx, "kept|Harbour Lights (2024) S01E01|root|true", "SELECT state || '|' || label || '|' || by_user || '|'"
		" || (remove_after > now() AND recycle_path LIKE '.axis-recycle/%%[WEB-DL 720p].mkv')::text FROM import_recycle");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT, q), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1), ==, 1);
	g_clear_pointer(&meanwhile.sql, g_free);
	axis_import_set_hook(fx->import, NULL, NULL);
}

/* ── add-anime-support, slice A2: names numbered absolutely ─────── */

#define AN_02        "[KumoSubs] Harbour Lights - 02 [WEB 1080p]"
#define AN_02_BARE   "[KumoSubs] Harbour Lights 02 [WEB 1080p]"
#define AN_S2_05     "[Tsuki] Harbour Lights S2 - 05 [WEB 1080p]"
#define AN_300       "[Tsuki] Harbour Lights - 300 [WEB 1080p]"
#define AN_07        "[Tsuki] Harbour Lights - 07 [WEB 1080p]"
#define AN_TITLE_2   "[Tsuki] Harbour Lights 2 - 05 [WEB 1080p]"
#define AN_BATCH     "[Tsuki] Harbour Lights - 01-03 [WEB 1080p]"
#define AN_03        "[Tsuki] Harbour Lights - 03 [WEB 1080p]"
#define AN_03_V2     "[Tsuki] Harbour Lights - 03v2 [WEB 1080p]"
#define AN_BOTH      "[NekoFansub] Harbour Lights - S01E02 - 003 [WEB 1080p]"
#define AN_OVA       "[Tsuki] Harbour Lights - OVA 1 [WEB 1080p]"
#define AN_04_BARE   "[KumoSubs] Harbour Lights 04 [WEB 1080p]"

/* Of the latest search alone. */
#define LATEST " AND search_id = (SELECT max(id) FROM series_search)"

/* The series marked as anime, following TheTVDB's aired order, its episodes
 * numbered 1 to 8 across the two seasons (the special has no number).  As a
 * change of numbering and a sync would leave it; nothing here asks TheTVDB. */
static void
make_anime(
	Fixture *fx,
	gint64   series
){
	exec(fx, "UPDATE series SET kind = 'anime', episode_source = 'tvdb' WHERE id = %" G_GINT64_FORMAT, series);
	exec(fx, "UPDATE series_episode SET source = 'tvdb', absolute_number = CASE WHEN season_number = 1 THEN number"
		" WHEN season_number = 2 THEN 4 + number END WHERE series_id = %" G_GINT64_FORMAT, series);
}

static gchar *
match_of(
	Fixture     *fx,
	const gchar *name
){
	return q(fx, "SELECT match || ': ' || coalesce(match_text, '') FROM series_candidate WHERE name = '%s'"
		" ORDER BY id DESC LIMIT 1", name);
}

static void
assert_match(
	Fixture     *fx,
	const gchar *name,
	const gchar *match,
	const gchar *says
){
	g_autofree gchar *text = match_of(fx, name);
	g_autofree gchar *prefix = g_strdup_printf("%s: ", match);

	if (text == NULL || !g_str_has_prefix(text, prefix))
		g_error("%s: expected %s, got %s", name, match, text != NULL ? text : "no candidate");
	if (says != NULL)
		assert_contains(text, says);
}

/*
 * A manual search of a series marked as anime lists a release numbered
 * absolutely with what it holds and the number it came from, and each
 * doubtful form with its own reason; the same releases for the same series
 * not marked are refused as they always were.  A person grabs one; its file,
 * named by the absolute number, is imported onto that episode; the history
 * names both numbers at the grab and at the import.  A "v2" is a revision.
 * Spec (tv delta): Mapped; Not an anime series; A season and a bare number;
 * Not aired; Both numberings disagree; Imported.
 */
static void
test_anime_search(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ AN_02, 1500000000 }, { AN_02_BARE, 1500000000 }, { AN_S2_05, 1500000000 }, { AN_300, 1500000000 },
		{ AN_07, 1500000000 }, { AN_TITLE_2, 1500000000 }, { AN_BATCH, 1500000000 }, { AN_03, 1500000000 },
		{ AN_BOTH, 1500000000 }, { AN_OVA, 1500000000 }
	};
	static const Rel revisions[] = { { AN_03, 1500000000 }, { AN_03_V2, 1500000000 } };
	static const Rel bare_only[] = { { AN_04_BARE, 1500000000 } };
	static const gchar *const files[] = { AN_02 ".mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 third = episode_id(fx, series, 1, 3);
	gint64 fourth = episode_id(fx, series, 1, 4);
	gint64 candidate;
	gint64 import_id;
	guint adds;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;
	guint i;

	/* Not marked: every one refused, as before this slice. */
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, low_tide, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	/* (Two state a season and an episode, and are matched by them as for any
	 * series: "S01E02 - 003", whose other number nothing reads, and "OVA 1",
	 * which reading a name by itself gives as S00E01.) */
	assert_qf(fx, AN_BOTH "|" AN_OVA, "SELECT string_agg(name, '|' ORDER BY name) FROM series_candidate"
		" WHERE match = 'match'" LATEST);
	text = match_of(fx, AN_02);
	g_assert_cmpstr(text, ==, "none: it states only an absolute episode number (2), which is not matched to a season and "
		"an episode");
	g_clear_pointer(&text, g_free);
	text = match_of(fx, AN_02_BARE);
	g_assert_cmpstr(text, ==, "none: no title of that name");
	g_clear_pointer(&text, g_free);
	text = match_of(fx, AN_S2_05);
	g_assert_cmpstr(text, ==, "none: it states a season and an absolute episode number (5), which is not matched");
	g_clear_pointer(&text, g_free);
	before = q(fx, "SELECT string_agg(name || '=' || match || ':' || coalesce(match_text, ''), '|' ORDER BY name)"
		" FROM series_candidate WHERE true" LATEST);
	assert_qf(fx, "0", "SELECT count(*) FROM series_event WHERE text LIKE '%%absolute number%%'");

	/* Marked as anime, and its numbers not usable (it follows the numbering
	 * of any series): nothing is matched through them, and each says why. */
	exec(fx, "UPDATE series SET kind = 'anime' WHERE id = %" G_GINT64_FORMAT, series);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, low_tide, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	/* (The name with both numberings is matched by its season and episode:
	 * there are no numbers to hold its other number against.  The special
	 * named by a word is not read as S00E01 for an anime series.) */
	assert_qf(fx, AN_BOTH, "SELECT string_agg(name, '|' ORDER BY name) FROM series_candidate WHERE match = 'match'"
		LATEST);
	assert_match(fx, AN_OVA, "none", "it names a special by a word");
	assert_match(fx, AN_02, "none", "TheTVDB's absolute numbers for this series cannot be used: the series does not "
		"follow TheTVDB's aired order");
	assert_match(fx, AN_02_BARE, "none", "no title of that name");

	/* Marked, and usable. */
	make_anime(fx, series);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, low_tide, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	assert_match(fx, AN_02, "match", "it holds S01E02 (absolute 2 is S01E02 by TheTVDB)");
	assert_match(fx, AN_02_BARE, "match", "it begins with the series' name and goes on with a number, and the series is "
		"marked as anime; it holds S01E02 (absolute 2 is S01E02 by TheTVDB)");
	assert_match(fx, AN_S2_05, "none", "it states a season and an absolute episode number (season 2, 5), which is not "
		"matched");
	assert_match(fx, AN_300, "none", "no episode of the series has the absolute number 300 (the highest TheTVDB gives is "
		"8)");
	assert_match(fx, AN_07, "none", "absolute 7 is S02E03, which has not aired");
	assert_match(fx, AN_TITLE_2, "none", "it begins with the series' name, but two numbers follow the series' name");
	/* (Since slice A3 a batch is the episodes it maps to: /tv/anime/batch.) */
	assert_match(fx, AN_BATCH, "match", "it holds S01E01 to S01E03 (absolute 1 to 3 are S01E01 to S01E03 by TheTVDB): a "
		"batch of 3 episodes");
	assert_match(fx, AN_BOTH, "none", "it says S01E02 and absolute 3, which is S01E03 by TheTVDB");
	assert_match(fx, AN_OVA, "none", "it names a special by a word");
	/* Another episode than the one searched for: matched, and not asked for. */
	assert_match(fx, AN_03, "match", "it holds S01E03 (absolute 3 is S01E03 by TheTVDB)");
	assert_reason(fx, AN_03, "other_episode");
	/* What it claims is the episode, by id. */
	assert_qf(fx, "t", "SELECT episode_ids = ARRAY[%" G_GINT64_FORMAT "]::bigint[] FROM series_candidate WHERE name = '"
		AN_02 "'" LATEST, low_tide);
	assert_qf(fx, "t", "SELECT accepted FROM series_candidate WHERE name = '" AN_02 "'" LATEST);
	/* Read only by the number after the series' name: listed with what it
	 * holds, and a person's to grab, never a search's. */
	assert_qf(fx, "f", "SELECT accepted FROM series_candidate WHERE name = '" AN_02_BARE "'" LATEST);
	assert_reason(fx, AN_02_BARE, "bare_number");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	/* add-attention-notices, N2: counted on its search as a person's to grab. */
	assert_qf(fx, "t", "SELECT n_person_only >= 1 FROM series_search ORDER BY id DESC LIMIT 1");

	/* A person grabs it. */
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" AN_02 "'" LATEST);
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	assert_qf(fx, "t", "SELECT array_agg(episode_id) = ARRAY[%" G_GINT64_FORMAT "]::bigint[] FROM series_grab_episode"
		" WHERE grab_id = %" G_GINT64_FORMAT, low_tide, grabbed.grab_id);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND by_user = 'root' AND text LIKE"
		" 'The release " AN_02 " was matched by its absolute number: 2 is S01E02 by TheTVDB (TheTVDB''s numbering as of"
		" 2026-11-14). It is grabbed for S01E02.'");
	assert_ep(fx, series, 1, 2, AXIS_TV_DOWNLOADING, AN_02);

	/* Its file, named by the absolute number, lands on S01E02. */
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grabbed.grab_id);
	write_download(fx, AN_02, files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 1500000000, "/data/downloads/" AN_02, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grabbed.grab_id);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low_tide);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode");
	placed = videos_in(dir);
	assert_contains(placed, "Harbour Lights (2024) - S01E02 - Low Tide");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reasons->0->>'t' FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND kind = 'video'",
		import_id);
	assert_contains(text, "Read by its absolute number: 2 is S01E02 by TheTVDB (TheTVDB's numbering as of 2026-11-14).");
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND episode_id = %" G_GINT64_FORMAT
		" AND text LIKE '%%was placed on S01E02. Read by its absolute number: 2 is S01E02 by TheTVDB%%'", low_tide);

	/* A search that grabs by itself takes one too; of a "v2" and the same
	 * release without it, the revision. */
	set_releases(fx, revisions, G_N_ELEMENTS(revisions));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, third);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, AN_03_V2, "SELECT release_name FROM series_grab ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text LIKE"
		" 'The release " AN_03_V2 " was matched by its absolute number: 3 is S01E03 by TheTVDB%%'");

	/* The review's blocker B2.  A release whose only reading is the number
	 * that follows the series' name ("Harbour Lights 04": by itself, a title
	 * "Harbour Lights 04") is the one wanted episode's only release, the
	 * profile takes it, and a search that grabs by itself does not: another
	 * title's name can be the very same.  It is listed with what it holds. */
	set_releases(fx, bare_only, G_N_ELEMENTS(bare_only));
	adds = n_adds(fx);
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, fourth);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, adds);
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab WHERE release_name = '" AN_04_BARE "'");
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE episode_id = %" G_GINT64_FORMAT, fourth);
	assert_match(fx, AN_04_BARE, "match", "it begins with the series' name and goes on with a number, and the series is "
		"marked as anime; it holds S01E04 (absolute 4 is S01E04 by TheTVDB)");
	assert_reason(fx, AN_04_BARE, "bare_number");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT r->>'t' FROM series_candidate c, jsonb_array_elements(c.reasons) r WHERE c.name = '" AN_04_BARE
		"' AND r->>'c' = 'bare_number' AND c.search_id = (SELECT max(id) FROM series_search)");
	g_assert_cmpstr(text, ==, "Its name was read as the series' name followed by an episode number, a form another "
		"title's name can have too: no search takes it. Only Grab this one, from a manual search, takes it for S01E04.");
	/* The same release with a hyphen states its number by itself, and is a
	 * search's to take as before (seen above for S01E03). */

	/* A person takes it.  The history line of the match is a report on the
	 * claim and cannot fail it (the review's L4): with the line refused by
	 * the database, the grab is made all the same. */
	exec(fx, "CREATE FUNCTION refuse_absolute_line() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN"
		" IF NEW.text LIKE '%%was matched by its absolute number%%' THEN RAISE EXCEPTION 'no line today'; END IF;"
		" RETURN NEW; END $$");
	exec(fx, "CREATE TRIGGER refuse_absolute_line BEFORE INSERT ON series_event FOR EACH ROW"
		" EXECUTE FUNCTION refuse_absolute_line()");
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" AN_04_BARE "'" LATEST);
	result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	g_assert_true(axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	g_assert_cmpuint(n_adds(fx), ==, adds + 1);
	assert_qf(fx, "t", "SELECT array_agg(episode_id) = ARRAY[%" G_GINT64_FORMAT "]::bigint[] FROM series_grab_episode"
		" WHERE grab_id = %" G_GINT64_FORMAT, fourth, grabbed.grab_id);
	assert_qf(fx, "0", "SELECT count(*) FROM series_event WHERE text LIKE 'The release " AN_04_BARE " was matched%%'");
	exec(fx, "DROP TRIGGER refuse_absolute_line ON series_event");
	exec(fx, "DROP FUNCTION refuse_absolute_line()");

	/* Unmarked again: the very same answers as before it was ever marked. */
	exec(fx, "UPDATE series SET kind = 'standard' WHERE id = %" G_GINT64_FORMAT, series);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, low_tide, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	again = q(fx, "SELECT string_agg(name || '=' || match || ':' || coalesce(match_text, ''), '|' ORDER BY name)"
		" FROM series_candidate WHERE true" LATEST);
	g_assert_cmpstr(again, ==, before);
	for (i = 0; i < G_N_ELEMENTS(rels); i++) {
		if (strcmp(rels[i].title, AN_BOTH) != 0 && strcmp(rels[i].title, AN_OVA) != 0)
			assert_match(fx, rels[i].title, "none", NULL);
	}
}

static gchar *
file_reason(
	Fixture     *fx,
	gint64       import_id,
	const gchar *path_has
){
	return q(fx, "SELECT reasons->0->>'t' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%%s%%' AND kind = 'video'", import_id, path_has);
}

/*
 * Imports of files named by absolute number, for an anime series: several
 * files each onto its episode; a download with one file that cannot be placed
 * (an episode the grab did not claim, a number no episode has) places none of
 * its files read by an absolute number; numbers that are not usable place
 * nothing; a file that maps to an episode that has a file follows the
 * ordinary rules; and for a series not marked the file is left as before.
 * Spec (tv delta): Imported; Outside the claim.
 */
static void
test_anime_import(Fixture *fx, gconstpointer data)
{
	static const gchar *const standard_files[] = { "[KumoSubs] Harbour Lights - 05 [WEB 1080p].mkv", NULL };
	static const gchar *const two_files[] = {
		"[KumoSubs] Harbour Lights - 01 [WEB 1080p].mkv", "[KumoSubs] Harbour Lights 02 [WEB 1080p].mkv",
		"[KumoSubs] Harbour Lights - NCOP 01 [WEB 1080p].mkv", NULL
	};
	static const gchar *const outside_files[] = {
		"[KumoSubs] Harbour Lights - 03 [WEB 1080p].mkv", "[KumoSubs] Harbour Lights - 04 [WEB 1080p].mkv", NULL
	};
	static const gchar *const unknown_files[] = {
		"[Tsuki] Harbour Lights - 03 [WEB 1080p].mkv", "[Tsuki] Harbour Lights - 300 [WEB 1080p].mkv", NULL
	};
	static const gchar *const two_number_files[] = {
		"[Hikari-Raws] Harbour Lights - 03 [WEB 1080p].mkv", "[Hikari-Raws] Harbour Lights 2 - 04 [WEB 1080p].mkv", NULL
	};
	static const gchar *const again_files[] = { "[Hikari-Raws] Harbour Lights - 01 [WEB 1080p].mkv", NULL };
	static const gchar *const unusable_files[] = { "[NekoFansub] Harbour Lights - 03 [WEB 1080p].mkv", NULL };
	g_autofree gchar *one = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *three = NULL;
	g_autofree gchar *three_four = NULL;
	g_autofree gchar *five = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *state = NULL;
	gint64 series = add_series(fx);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 e4 = episode_id(fx, series, 1, 4);
	gint64 e5 = episode_id(fx, series, 2, 1);
	gint64 file_one;
	gint64 import_id;

	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", e1);
	both = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e1, e2);
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", e3);
	three_four = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e3, e4);
	five = g_strdup_printf("{%" G_GINT64_FORMAT "}", e5);

	/* Not marked: left, with the reason it always had. */
	write_download(fx, "Standard.Five", standard_files);
	import_id = request_import(fx, add_download(fx, series, "Standard.Five", "nzo_standard", five));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	text = file_reason(fx, import_id, "- 05");
	g_assert_cmpstr(text, ==, "Not imported: it states only an absolute episode number (5), which is not matched to a "
		"season and an episode.");
	g_clear_pointer(&text, g_free);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file");

	make_anime(fx, series);

	/* Two files, each onto its own episode (one by a bare number); an extra
	 * named by a word is left and stands in nobody's way. */
	write_download(fx, "Anime.Two", two_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Two", "nzo_two", both));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2);
	assert_qf(fx, "2", "SELECT count(*) FROM series_file");
	assert_qf(fx, "2", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text LIKE"
		" '%%Read by its absolute number: %% by TheTVDB (TheTVDB''s numbering as of 2026-11-14).'");
	text = file_reason(fx, import_id, "NCOP");
	assert_contains(text, "it names a special, an extra or a film by a word");
	g_clear_pointer(&text, g_free);
	/* An import's own link: not made by hand, not kept. */
	assert_qf(fx, "0", "SELECT count(*) FROM series_file WHERE kept OR hand_how IS NOT NULL");
	file_one = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1);

	/* One file maps to an episode the grab did not claim: none is placed. */
	write_download(fx, "Anime.Outside", outside_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Outside", "nzo_outside", three));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id IN (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")", e3, e4);
	text = file_reason(fx, import_id, "- 04");
	assert_contains(text, "its name gives S01E04 by its absolute number (4 is S01E04 by TheTVDB), and this download was "
		"not grabbed for that episode");
	assert_contains(text, "Assign it by hand");
	g_clear_pointer(&text, g_free);
	text = file_reason(fx, import_id, "- 03");
	assert_contains(text, "its name gives S01E03 by its absolute number (3 is S01E03 by TheTVDB), but another file of "
		"this download");
	assert_contains(text, "placed whole or not at all");
	g_clear_pointer(&text, g_free);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* One file's number is on no episode: none is placed. */
	write_download(fx, "Anime.Unknown", unknown_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Unknown", "nzo_unknown", three_four));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id IN (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")", e3, e4);
	text = file_reason(fx, import_id, "- 300");
	assert_contains(text, "no episode of the series has the absolute number 300");
	g_clear_pointer(&text, g_free);
	text = file_reason(fx, import_id, "- 03");
	assert_contains(text, "is numbered absolutely and cannot be placed: no episode of the series has the absolute number "
		"300");
	g_clear_pointer(&text, g_free);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* One file begins with the series' name and is not read (two numbers
	 * follow it: this series' fourth, or another title's?): it is left, and
	 * so is the other, as for any file of the lot that cannot be placed (the
	 * review's L3: the lot was judged by the matcher alone, which reads "- 04"
	 * as the claimed S01E04, and the other file was placed). */
	write_download(fx, "Anime.TwoNumbers", two_number_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.TwoNumbers", "nzo_twonumbers", three_four));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id IN (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")", e3, e4);
	text = file_reason(fx, import_id, "2 - 04");
	assert_contains(text, "its name begins with the series' name, but two numbers follow the series' name");
	g_clear_pointer(&text, g_free);
	text = file_reason(fx, import_id, "- 03");
	assert_contains(text, "begins with the series' name and is not read: two numbers follow the series' name");
	assert_contains(text, "placed whole or not at all");
	g_clear_pointer(&text, g_free);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* An episode that has a file: the ordinary rules (no better, so not
	 * imported over it); the file there stays. */
	write_download(fx, "Anime.Again", again_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Again", "nzo_again", one));
	state = run_import(fx, import_id);
	g_assert_cmpstr(state, !=, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, e1, file_one);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* TheTVDB gives two episodes one number: nothing is matched through the
	 * numbers until that is put right. */
	exec(fx, "UPDATE series_episode SET absolute_number = 3 WHERE id = %" G_GINT64_FORMAT, e4);
	write_download(fx, "Anime.Unusable", unusable_files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Unusable", "nzo_unusable", three));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	text = file_reason(fx, import_id, "- 03");
	assert_contains(text, "TheTVDB's absolute numbers for this series cannot be used: two episodes share a number");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3);
}

/*
 * Place by its name reads an anime series' files through TheTVDB's numbers as
 * they are now: a file named by absolute number 3 that an import put on
 * S01E03 is listed once TheTVDB gives 3 to S01E04, and nothing moves until a
 * person places it.  For a series not marked it is never listed.
 * Spec (tv delta): TheTVDB moves a number.
 */
static void
test_anime_place_by_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const files[] = { "[KumoSubs] Harbour Lights - 03 [WEB 1080p].mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;
	g_autofree gchar *three = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *dir = season_dir(fx, 1);
	g_autofree gchar *placed_files = NULL;
	gint64 series = add_series(fx);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 e4 = episode_id(fx, series, 1, 4);
	gint64 import_id;
	gint64 file;
	const AxisTvMisplaced *m;
	guint placed = 0;
	gboolean changed = FALSE;

	make_anime(fx, series);
	/* The import keeps the file's own name. */
	set(fx, AXIS_IMPORT_SETTING_RENAME, "false");
	three = g_strdup_printf("{%" G_GINT64_FORMAT "}", e3);
	write_download(fx, "Anime.Three", files);
	import_id = request_import(fx, add_download(fx, series, "Anime.Three", "nzo_three", three));
	assert_import(fx, import_id, "imported");
	placed_files = videos_in(dir);
	g_assert_cmpstr(placed_files, ==, "[KumoSubs] Harbour Lights - 03 [WEB 1080p].mkv");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3);

	/* Its name says the episode it is on. */
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &list, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(list->len, ==, 0);
	g_clear_pointer(&list, g_ptr_array_unref);

	/* TheTVDB moves the number 3 to S01E04. */
	exec(fx, "UPDATE series_episode SET absolute_number = NULL WHERE id = %" G_GINT64_FORMAT, e3);
	exec(fx, "UPDATE series_episode SET absolute_number = 3 WHERE id = %" G_GINT64_FORMAT, e4);

	/* Not marked: its name is not read by its number, and it is not listed. */
	exec(fx, "UPDATE series SET kind = 'standard' WHERE id = %" G_GINT64_FORMAT, series);
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &list, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(list->len, ==, 0);
	g_clear_pointer(&list, g_ptr_array_unref);
	exec(fx, "UPDATE series SET kind = 'anime' WHERE id = %" G_GINT64_FORMAT, series);

	/* Listed, apart (an import placed it), and not moved by itself. */
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &list, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(list->len, ==, 1);
	m = g_ptr_array_index(list, 0);
	g_assert_cmpint(m->file_id, ==, file);
	g_assert_cmpint(m->season, ==, 1);
	g_assert_cmpint(m->number, ==, 3);
	g_assert_cmpint(m->name_season, ==, 1);
	g_assert_cmpint(m->name_number, ==, 4);
	g_assert_cmpint(m->to_episode_id, ==, e4);
	g_assert_true(m->earlier);
	g_assert_true(m->by_import);
	g_clear_pointer(&list, g_ptr_array_unref);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, e3, file);
	/* "Place all" never takes it. */
	g_assert_true(axis_tv_place_by_name(fx->tv, series, 0, NULL, "root", &placed, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(placed, ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, e3, file);

	/* A person places that one file: only the link moves. */
	g_assert_true(axis_tv_list_misplaced(fx->tv, series, file, &list, NULL, &digest, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(list->len, ==, 1);
	g_assert_true(axis_tv_place_by_name(fx->tv, series, file, digest, "root", &placed, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	g_assert_cmpuint(placed, ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, e4, file);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3);
	g_clear_pointer(&placed_files, g_free);
	placed_files = videos_in(dir);
	g_assert_cmpstr(placed_files, ==, "[KumoSubs] Harbour Lights - 03 [WEB 1080p].mkv");
}

/* ── add-anime-support, slice A3: what a search asks, and batches ── */

#define AN_B_03_06   "[Tsuki] Harbour Lights - 03-06 [BluRay 1080p]"
#define AN_B_02_03   "[Tsuki] Harbour Lights - 02-03 [WEB 1080p]"
#define AN_B_BARE    "[Tsuki] Harbour Lights 01-02 [WEB 1080p]"
#define AN_B_05_07   "[Tsuki] Harbour Lights - 05-07 [WEB 1080p]"
#define AN_B_S2      "[Tsuki] Harbour Lights S2 - 01-02 [WEB 1080p]"
#define AN_05        "[Tsuki] Harbour Lights - 05 [WEB 1080p]"
#define AN_SECOND_02 "[Tsuki] Minato no Akari - 02 [WEB 1080p]"
#define ANIME_CATS   "&categories=5000&categories=5070"

/* The queries of the fake Prowlarr's searches from the @from-th on, one to a
 * line. */
static gchar *
prowlarr_queries(
	Fixture *fx,
	guint    from
){
	GString *all = g_string_new(NULL);
	guint n = axis_test_prowlarr_n_searches(fx->prowlarr);
	guint i;

	for (i = from; i < n; i++) {
		g_autofree gchar *query = axis_test_prowlarr_search_query(fx->prowlarr, i);

		g_string_append_printf(all, "%s%s", i > from ? "\n" : "", query != NULL ? query : "");
	}
	return g_string_free(all, FALSE);
}

static gchar *
latest_how(Fixture *fx)
{
	return q(fx, "SELECT coalesce(how, '') FROM series_search ORDER BY id DESC LIMIT 1");
}

/*
 * What a search asks an indexer (through Prowlarr).  A series that is not
 * marked as anime: the one request it always made, byte for byte, in TV alone,
 * and the search's record as it was.  Marked, its numbers usable: the same
 * request in TV and Anime, the words of its title and the absolute number, and
 * the same words under one more name when its details give a romanised one:
 * three requests at most.  An episode whose only release comes back for the
 * words is grabbed; a release two requests return is listed once; the
 * search's record names the requests.  Numbers that cannot be used: by season
 * and episode alone, and the record says why.  A season is also asked for by
 * the series' name, the very request a search of the whole series makes: the
 * second of the two is answered from what the layer kept.
 * Spec (indexers delta): The words; Found twice; No absolute number; A
 * standard series.  Spec (tv delta): Found by its number.
 */
static void
test_anime_asks(Fixture *fx, gconstpointer data)
{
	static const Rel none[] = { { "Nothing.Of.This.Series.S09E09.1080p.WEB.H264-GRP", 1500000000 } };
	static const Rel by_number[] = { { AN_02, 1500000000 } };
	static const Rel by_second[] = { { AN_SECOND_02, 1500000000 }, { AN_05, 1500000000 } };
	g_autofree gchar *asked = NULL;
	g_autofree gchar *how = NULL;
	g_autofree gchar *standard_how = NULL;
	g_autofree gchar *expected = NULL;
	gint64 series = add_series(fx);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	gint64 arrival = episode_id(fx, series, 1, 1);
	AxisAcquireSearchReport report;
	guint from;

	/* Not marked: the one request of any series, in TV, and nothing said of
	 * how it was asked beyond what was always said.  (Prowlarr is asked
	 * twice for that one request, as it always was: by the identifier, and
	 * by the title for its indexer that takes no identifier.) */
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_true(report.searched);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A1%7D%7BEpisode%3A2%7D&type=tvsearch&categories=5000"
		"&limit=1000\n"
		"query=Harbour%20Lights%20S01E02&type=search&categories=5000&indexerIds=5&limit=1000");
	g_clear_pointer(&asked, g_free);
	how = latest_how(fx);
	g_assert_true(g_str_has_prefix(how, "Prowlarr by "));
	g_assert_null(strstr(how, "Asked"));
	g_assert_null(strstr(how, "Anime"));
	standard_how = g_steal_pointer(&how);
	/* Its season and the whole series, as they were asked before. */
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	search(fx, series, AXIS_TV_SCOPE_SERIES, 0);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A1%7D&type=tvsearch&categories=5000&limit=1000\n"
		"query=Harbour%20Lights%20S01&type=search&categories=5000&indexerIds=5&limit=1000\n"
		"query=Harbour%20Lights&type=search&limit=1000");
	g_clear_pointer(&asked, g_free);

	/* What the indexer layer keeps of an answer is kept apart for the two:
	 * an episode asked for as an anime series' is not answered from what was
	 * kept of the same episode asked for as any series', and asked for as any
	 * series' again it is answered from that, with no request to the indexer
	 * (the key of a request that is not an anime series' is what it was). */
	set_releases(fx, none, G_N_ELEMENTS(none));
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	exec(fx, "UPDATE series SET kind = 'anime' WHERE id = %" G_GINT64_FORMAT, series);
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, from + 2);
	exec(fx, "UPDATE series SET kind = 'standard' WHERE id = %" G_GINT64_FORMAT, series);
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, from);

	/* Marked as anime, its numbers not usable (it does not follow TheTVDB's
	 * aired order): by season and episode alone, in TV and Anime, and the
	 * record says why no absolute number was asked for. */
	exec(fx, "UPDATE series SET kind = 'anime' WHERE id = %" G_GINT64_FORMAT, series);
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A1%7D%7BEpisode%3A2%7D&type=tvsearch" ANIME_CATS
		"&limit=1000\n"
		"query=Harbour%20Lights%20S01E02&type=search" ANIME_CATS "&indexerIds=5&limit=1000");
	g_clear_pointer(&asked, g_free);
	how = latest_how(fx);
	expected = g_strdup_printf("Asked by season 1 episode 2, in TV and Anime (not by an absolute number: the series does "
		"not follow TheTVDB's aired order, which is where they come from): %s", standard_how);
	g_assert_cmpstr(how, ==, expected);
	g_clear_pointer(&how, g_free);
	g_clear_pointer(&expected, g_free);

	/* Usable: the request of any series, and the words. */
	make_anime(fx, series);
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A1%7D%7BEpisode%3A2%7D&type=tvsearch" ANIME_CATS
		"&limit=1000\n"
		"query=Harbour%20Lights%20S01E02&type=search" ANIME_CATS "&indexerIds=5&limit=1000\n"
		"query=Harbour%20Lights%2002&type=search" ANIME_CATS "&limit=1000");
	g_clear_pointer(&asked, g_free);
	how = latest_how(fx);
	expected = g_strdup_printf("Asked by season 1 episode 2, in TV and Anime: %s. Asked by the words \"Harbour Lights "
		"02\", in TV and Anime: Prowlarr by the title and year: 1 release", standard_how);
	g_assert_cmpstr(how, ==, expected);
	g_clear_pointer(&how, g_free);
	g_clear_pointer(&expected, g_free);
	/* (Both requests returned the one release: it is listed once.) */
	assert_qf(fx, "1", "SELECT count(*) FROM series_candidate WHERE true" LATEST);
	assert_qf(fx, "1", "SELECT n_found FROM series_search ORDER BY id DESC LIMIT 1");

	/* A series whose numbers reach 100 is asked for with three digits. */
	exec(fx, "UPDATE series_episode SET absolute_number = 104 WHERE series_id = %" G_GINT64_FORMAT
		" AND season_number = 2 AND number = 4", series);
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	asked = prowlarr_queries(fx, from);
	assert_contains(asked, "\nquery=Harbour%20Lights%20002&type=search" ANIME_CATS "&limit=1000");
	g_clear_pointer(&asked, g_free);
	make_anime(fx, series);

	/* Found by its words: the request by the identifier returns nothing of
	 * the episode, and the search grabs what the words returned.  (Found by
	 * the words alone, with nothing else asked by words: /tv/anime/asks-native.)
	 * Spec: Found by its number. */
	set_releases_of(fx, "tvsearch", none, G_N_ELEMENTS(none));
	set_releases_of(fx, "search", by_number, G_N_ELEMENTS(by_number));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, AN_02 "|S01E02", "SELECT release_name || '|' || holds FROM series_grab ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "t", "SELECT array_agg(episode_id) = ARRAY[%" G_GINT64_FORMAT "]::bigint[] FROM series_grab_episode"
		" WHERE open", low_tide);
	assert_qf(fx, "2", "SELECT n_found FROM series_search ORDER BY id DESC LIMIT 1");

	/* One more name, when the details give a romanised one: three requests
	 * of Axis's, and no more (the first is two at Prowlarr, as for any
	 * series).  A release under that name is this series' (the name is its
	 * original title), and one for another episode is not what was asked for,
	 * however it was found. */
	exec(fx, "UPDATE meta_title SET original_title = 'Minato no Akari' WHERE id = (SELECT title_id FROM series WHERE id = %"
		G_GINT64_FORMAT ")", series);
	set_releases_of(fx, "tvsearch", none, G_N_ELEMENTS(none));
	set_releases_of(fx, "search", by_second, G_N_ELEMENTS(by_second));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, arrival, "list", "root", NULL, &report, NULL));
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, from + 4);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A1%7D%7BEpisode%3A1%7D&type=tvsearch" ANIME_CATS
		"&limit=1000\n"
		"query=Harbour%20Lights%20S01E01&type=search" ANIME_CATS "&indexerIds=5&limit=1000\n"
		"query=Harbour%20Lights%2001&type=search" ANIME_CATS "&limit=1000\n"
		"query=Minato%20no%20Akari%2001&type=search" ANIME_CATS "&limit=1000");
	g_clear_pointer(&asked, g_free);
	how = latest_how(fx);
	assert_contains(how, "Asked by the words \"Minato no Akari 01\", in TV and Anime: Prowlarr by the title and year");
	g_clear_pointer(&how, g_free);
	/* (The two word requests returned the same two releases: each once.) */
	assert_qf(fx, "3", "SELECT count(*) FROM series_candidate WHERE true" LATEST);
	assert_match(fx, AN_SECOND_02, "match", "it holds S01E02 (absolute 2 is S01E02 by TheTVDB)");
	assert_reason(fx, AN_SECOND_02, "other_episode");
	assert_match(fx, AN_05, "match", "it holds S02E01 (absolute 5 is S02E01 by TheTVDB)");
	assert_reason(fx, AN_05, "other_episode");

	/* A season: its own request, and the series' name in TV and Anime, which
	 * is how a batch is found.  A search of the whole series asks that very
	 * request, and is answered from what the layer kept: nothing more goes
	 * to the indexer. */
	set_releases(fx, none, G_N_ELEMENTS(none));
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_SEASON, 2);
	asked = prowlarr_queries(fx, from);
	g_assert_cmpstr(asked, ==, "query=%7BTvdbId%3A360002%7D%7BSeason%3A2%7D&type=tvsearch" ANIME_CATS "&limit=1000\n"
		"query=Harbour%20Lights%20S02&type=search" ANIME_CATS "&indexerIds=5&limit=1000\n"
		"query=Harbour%20Lights&type=search" ANIME_CATS "&limit=1000");
	g_clear_pointer(&asked, g_free);
	how = latest_how(fx);
	assert_contains(how, "Asked by season 2, in TV and Anime: Prowlarr by ");
	assert_contains(how, ". Asked by the series' name, in TV and Anime, for batches: Prowlarr by the title and year");
	g_clear_pointer(&how, g_free);
	from = axis_test_prowlarr_n_searches(fx->prowlarr);
	search(fx, series, AXIS_TV_SCOPE_SERIES, 0);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, from);
	how = latest_how(fx);
	assert_contains(how, "Asked by the series' name, in TV and Anime: Prowlarr by the title and year, from a search a "
		"moment ago");
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->prowlarr), ==, 0);
}

/*
 * The same against a native Newznab indexer: a series that is not marked is
 * asked for by its identifier, season and number in the indexer's TV
 * categories, as it always was; marked as anime it is asked for in those and
 * in 5070, and by the words in the same categories.
 * Spec (indexers delta): A Newznab indexer; A standard series.
 */
static void
test_anime_asks_native(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *feed = NULL;
	g_autofree gchar *empty = NULL;
	g_autofree gchar *how = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *item = g_strdup("<item><title>" AN_02 "</title><guid>@ORIGIN@/details/a2</guid>"
		"<pubDate>Fri, 13 Nov 2026 10:00:00 +0000</pubDate>"
		"<enclosure url=\"@FILES@@BASE@/api?t=get&amp;id=nzb-a2&amp;apikey=@KEY@\" length=\"1500000000\""
		" type=\"application/x-nzb\"/><newznab:attr name=\"category\" value=\"5070\"/>"
		"<newznab:attr name=\"size\" value=\"1500000000\"/></item>");
	gint64 series = add_series(fx);
	gint64 low_tide = episode_id(fx, series, 1, 2);
	AxisAcquireSearchReport report;
	guint searches;
	guint i;

	/* The indexer has the episode's release under its absolute number, and
	 * gives it for words only: by the identifier, season and number it has
	 * nothing. */
	feed = axis_test_newznab_feed(item, 0, 0, 0, 0);
	empty = axis_test_newznab_feed("", 0, 0, 0, 0);
	axis_test_newznab_set_search(fx->nzb, "search", feed);
	axis_test_newznab_set_search(fx->nzb, "tvsearch", empty);
	/* Not marked: one request, as it always was, and so nothing found. */
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	g_assert_cmpstr(report.outcome, ==, "nothing_found");
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 1);
	first = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(first, ==, "t=tvsearch&tvdbid=360002&season=1&ep=2&cat=5000&extended=1&limit=100&offset=0&apikey="
		NZB_KEY);
	g_clear_pointer(&first, g_free);
	g_assert_cmpuint(n_adds(fx), ==, 0);

	make_anime(fx, series);
	exec(fx, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
	searches = axis_test_newznab_n_searches(fx->nzb);
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, low_tide);
	/* Two requests where a series makes one. */
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, searches + 2);
	/* (Nothing before this asked by words, or in 5070.) */
	for (i = 0; i < axis_test_newznab_n_requests(fx->nzb); i++) {
		gchar *query = axis_test_newznab_query(fx->nzb, i);

		if (query != NULL && g_str_has_prefix(query, "t=tvsearch") && strstr(query, "5070") != NULL) {
			g_free(first);
			first = query;
		} else if (query != NULL && g_str_has_prefix(query, "t=search")) {
			g_free(second);
			second = query;
		} else {
			g_free(query);
		}
	}
	g_assert_cmpstr(first, ==, "t=tvsearch&tvdbid=360002&season=1&ep=2&cat=5000%2C5070&extended=1&limit=100&offset=0"
		"&apikey=" NZB_KEY);
	g_assert_cmpstr(second, ==, "t=search&q=Harbour%20Lights%2002&cat=5000%2C5070&extended=1&limit=100&offset=0&apikey="
		NZB_KEY);
	/* Found by its words alone, and grabbed for the episode.
	 * Spec (tv delta): Found by its number. */
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	assert_qf(fx, "1", "SELECT count(*) FROM series_candidate WHERE true" LATEST);
	assert_qf(fx, AN_02 "|S01E02", "SELECT release_name || '|' || holds FROM series_grab");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	how = latest_how(fx);
	assert_contains(how, "Asked by season 1 episode 2, in TV and Anime: NewsOwl by ");
	assert_contains(how, ": 0 releases. Asked by the words \"Harbour Lights 02\", in TV and Anime: NewsOwl by the title and "
		"year: 1 release");
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);
	assert_qf(fx, "0", "SELECT count(*) FROM series_search WHERE coalesce(how, '') LIKE '%%" NZB_KEY "%%'");
}

/*
 * A batch: a release that holds a run of absolute numbers which is not whole
 * seasons is judged as exactly the episodes its numbers map to.  A season
 * search takes one that holds a wanted episode of the season, across two
 * seasons, and its grab claims those episodes by id; one that holds an episode
 * in flight, or one in the library that is not wanted, is not taken, and the
 * reason names the episode; one with a number on an episode that has not
 * aired is refused; what A2 left to a person (a range read only because it
 * follows the series' name) is listed and not grabbed; a season with a range
 * is refused.  At the import each file is placed by its own name on an
 * episode the grab claimed, in that episode's own season folder, by an
 * ordinary link (not by hand, not kept); a file whose name says nothing is
 * left.  The same releases for the same series not marked are refused as
 * they always were.
 * Spec (tv delta): A batch across seasons; A batch that holds an episode not
 * wanted; A number that does not map.
 */
static void
test_anime_batch(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ AN_B_03_06, 6000000000 }, { AN_B_BARE, 3000000000 }, { AN_B_05_07, 4500000000 }, { AN_B_S2, 3000000000 }
	};
	static const Rel after[] = { { AN_B_02_03, 3000000000 } };
	static const gchar *const files[] = {
		"[Tsuki] Harbour Lights - 03 [BluRay 1080p].mkv", "[Tsuki] Harbour Lights - 04 [BluRay 1080p].mkv",
		"[Tsuki] Harbour Lights - 06 [BluRay 1080p].mkv", NULL
	};
	g_autofree gchar *before = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *first_dir = season_dir(fx, 1);
	g_autofree gchar *second_dir = season_dir(fx, 2);
	g_autofree gchar *placed = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 e4 = episode_id(fx, series, 1, 4);
	gint64 e5 = episode_id(fx, series, 2, 1);
	gint64 e6 = episode_id(fx, series, 2, 2);
	gint64 import_id;
	gint64 grab;
	AxisAcquireSearchReport report;

	/* Not marked: none is this series' episodes, as before this slice. */
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_qf(fx, "0", "SELECT count(*) FROM series_candidate WHERE match = 'match'" LATEST);
	before = q(fx, "SELECT string_agg(name || '=' || match || ':' || coalesce(match_text, ''), '|' ORDER BY name)"
		" FROM series_candidate WHERE true" LATEST);
	assert_contains(before, AN_B_03_06 "=none:it states only an absolute episode number (3), which is not matched to a "
		"season and an episode");

	/* Marked, its numbers usable. */
	make_anime(fx, series);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_match(fx, AN_B_03_06, "match", "it holds S01E03 to S02E02 (absolute 3 to 6 are S01E03 to S02E02 by TheTVDB): a "
		"batch of 4 episodes");
	assert_qf(fx, AN_B_03_06 "|S01E03-S02E02|season|1", "SELECT release_name || '|' || holds || '|' || scope || '|' ||"
		" season_number FROM series_grab");
	grab = qi(fx, "SELECT id FROM series_grab");
	/* Bound to exactly those four, by id; an ordinary grab, not one by force. */
	assert_qf(fx, "t", "SELECT array_agg(episode_id ORDER BY episode_id) = (SELECT array_agg(x ORDER BY x) FROM unnest("
		"ARRAY[%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT "]::bigint[]) x)"
		" FROM series_grab_episode WHERE grab_id = %" G_GINT64_FORMAT, e3, e4, e5, e6, grab);
	assert_qf(fx, "f", "SELECT bound FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text = 'The release " AN_B_03_06
		" was matched by its absolute number: 3 to 6 are S01E03 to S02E02 by TheTVDB (TheTVDB''s numbering as of"
		" 2026-11-14). It is grabbed for S01E03-S02E02. It is a batch: each of its files is placed only on one of these"
		" episodes, by the number in the file''s own name.'");
	/* A range read only because it follows the series' name: listed with
	 * what it holds, a person's to grab (A2's rule, for a range as for one
	 * number). */
	assert_match(fx, AN_B_BARE, "match", "it holds S01E01 to S01E02 (absolute 1 to 2 are S01E01 to S01E02 by TheTVDB): a "
		"batch of 2 episodes");
	assert_reason(fx, AN_B_BARE, "bare_number");
	/* One number on an episode that has not aired refuses the whole batch. */
	assert_match(fx, AN_B_05_07, "none", "absolute 7 is S02E03, which has not aired");
	/* A season with a range is a group's season, as with one number. */
	assert_match(fx, AN_B_S2, "none", NULL);
	assert_ep(fx, series, 1, 3, AXIS_TV_DOWNLOADING, AN_B_03_06);
	assert_ep(fx, series, 2, 2, AXIS_TV_DOWNLOADING, AN_B_03_06);
	/* Nothing it does not hold is claimed. */
	assert_qf(fx, "4", "SELECT count(*) FROM series_grab_episode");
	assert_qf(fx, "0", "SELECT count(*) FROM series_grab_episode WHERE episode_id IN (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")", episode_id(fx, series, 1, 1), episode_id(fx, series, 1, 2));

	/* While it is in flight, another batch that holds one of its episodes is
	 * not taken, and says which. */
	set_releases(fx, after, G_N_ELEMENTS(after));
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_reason(fx, AN_B_02_03, "in_flight");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT r->>'t' FROM series_candidate c, jsonb_array_elements(c.reasons) r WHERE c.name = '" AN_B_02_03
		"' AND r->>'c' = 'in_flight'" LATEST);
	assert_contains(text, "It holds S01E03, whose grab is in flight.");

	/* It arrives with three files named by their numbers, and none for the
	 * fifth.  Each goes onto its own episode, in that episode's season folder.
	 * (With a file that does not say which episode it is, nothing is placed:
	 * /tv/anime/batch-files.) */
	ref = q(fx, "SELECT client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	write_download(fx, AN_B_03_06, files);
	axis_test_download_fake_set(fx->fake, ref, "completed", 6000000000, "/data/downloads/" AN_B_03_06, NULL);
	poll(fx);
	g_assert_cmpuint(follow(fx).imports, ==, 1);
	import_id = qi(fx, "SELECT i.id FROM import i JOIN series_grab g ON g.download_id = i.download_id WHERE g.id = %"
		G_GINT64_FORMAT, grab);
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e4);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e5);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e6);
	assert_qf(fx, "3", "SELECT count(*) FROM series_file_episode");
	placed = videos_in(first_dir);
	assert_contains(placed, "Harbour Lights (2024) - S01E03 - ");
	assert_contains(placed, "Harbour Lights (2024) - S01E04 - ");
	g_assert_null(strstr(placed, "S02E"));
	g_clear_pointer(&placed, g_free);
	placed = videos_in(second_dir);
	assert_contains(placed, "Harbour Lights (2024) - S02E02 - ");
	g_assert_null(strstr(placed, "S01E"));
	g_assert_null(strstr(placed, "S02E01"));
	/* An import's own links: not made by hand, not kept, so each is upgraded
	 * by the ordinary rules. */
	assert_qf(fx, "0", "SELECT count(*) FROM series_file WHERE kept OR hand_how IS NOT NULL");
	assert_qf(fx, "3", "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text LIKE"
		" '%%Read by its absolute number: %% by TheTVDB (TheTVDB''s numbering as of 2026-11-14).'");
	g_clear_pointer(&text, g_free);
	/* The episode it was grabbed for and got no file for is not left looking
	 * as if it had one: the grab's record and the history say so, and no
	 * search grabs the same release again for it. */
	g_assert_cmpint(state_of(fx, series, 2, 1, NULL), !=, AXIS_TV_IN_LIBRARY);
	assert_qf(fx, "imported|Imported, with no file for: S02E01", "SELECT close_reason || '|' || close_text FROM"
		" series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE text LIKE 'The download of " AN_B_03_06 " was imported,"
		" and S02E01, which it was grabbed for, got no file from it%%'");
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, e5);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_reason(fx, AN_B_03_06, "had_before");
	/* add-attention-notices, N2: counted on its search as a person's to grab. */
	assert_qf(fx, "t", "SELECT n_person_only >= 1 FROM series_search ORDER BY id DESC LIMIT 1");

	/* A batch that holds an episode in the library that is not wanted (S01E03
	 * has its file now, at the profile's cutoff) is not taken, though it also
	 * holds one that is wanted, and the reason names the episode.
	 * Spec: A batch that holds an episode not wanted. */
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	assert_ep(fx, series, 1, 3, AXIS_TV_IN_LIBRARY, NULL);
	assert_ep(fx, series, 1, 2, AXIS_TV_WANTED, NULL);
	set_releases(fx, after, G_N_ELEMENTS(after));
	report = search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_match(fx, AN_B_02_03, "match", "it holds S01E02 to S01E03 (absolute 2 to 3 are S01E02 to S01E03 by TheTVDB): a "
		"batch of 2 episodes");
	assert_reason(fx, AN_B_02_03, "holds_unwanted");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT r->>'t' FROM series_candidate c, jsonb_array_elements(c.reasons) r WHERE c.name = '" AN_B_02_03
		"' AND r->>'c' = 'holds_unwanted'" LATEST);
	assert_contains(text, "It holds S01E03, which is monitored and not wanted");

	/* Unmarked again: the answers it had before it was ever marked. */
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
	exec(fx, "UPDATE series SET kind = 'standard' WHERE id = %" G_GINT64_FORMAT, series);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, NULL));
	again = q(fx, "SELECT string_agg(name || '=' || match || ':' || coalesce(match_text, ''), '|' ORDER BY name)"
		" FROM series_candidate WHERE true" LATEST);
	g_assert_cmpstr(again, ==, before);
}

#define BF_UNCLAIMED "[Tsuki] Harbour Lights - 03-04 [WEB 1080p]"
#define BF_MIXED     "[KumoSubs] Harbour Lights - 03-04 [WEB 1080p]"
#define BF_WITHIN    "[NekoFansub] Harbour Lights - 03-04 [WEB 1080p]"
#define BF_ORDINARY  "Harbour.Lights.S01E01.1080p.WEB.H264-GRP"
#define BF_STRAY     "[Hikari-Raws] Harbour Lights - 05-06 [WEB 1080p]"
#define BF_SHORT     "[Tsuki] Harbour Lights - 05-06 [BluRay 1080p]"
#define BF_FIRST     "[Tsuki] Harbour Lights - 01-02 [WEB 1080p]"

/*
 * The review of slice A3: the files of a download that was grabbed by
 * absolute numbers.
 *
 * The blocker: such a grab claims the episodes its numbers map to, and a file
 * of it that states a season and an episode the grab never claimed was placed
 * on that episode all the same (the claim was checked only for a file read by
 * an absolute number).  Now a file outside the claim is not placed, however
 * its name states its episode, and none of the download's files is: the lot
 * waits for a person.  A file that states a season and an episode the grab
 * did claim is placed.  A grab that was not made by absolute numbers imports
 * as it always did.
 *
 * And a download that cannot account for every episode it was grabbed for
 * while one of its files does not say which episode it is places nothing (so
 * that it is not "imported" with a hole, removed, and the same release grabbed
 * again); one that simply lacks a file is imported, its history says which
 * episode got nothing, and no search grabs that release again for the hole.
 */
static void
test_anime_batch_files(Fixture *fx, gconstpointer data)
{
	static const gchar *const unclaimed_files[] = {
		"Harbour.Lights.S02E01.1080p.WEB.H264-GRP.mkv", "Harbour.Lights.S02E02.1080p.WEB.H264-GRP.mkv", NULL
	};
	static const gchar *const mixed_files[] = {
		"[KumoSubs] Harbour Lights - 03 [WEB 1080p].mkv", "Harbour.Lights.S02E01.1080p.WEB.H264-GRP.mkv", NULL
	};
	static const gchar *const within_files[] = {
		"[NekoFansub] Harbour Lights - 03 [WEB 1080p].mkv", "Harbour.Lights.S01E04.1080p.WEB.H264-GRP.mkv", NULL
	};
	static const gchar *const ordinary_files[] = {
		"Harbour.Lights.S01E01.1080p.WEB.H264-GRP.mkv", "Harbour.Lights.S01E02.1080p.WEB.H264-GRP.mkv", NULL
	};
	static const gchar *const stray_files[] = { "[Hikari-Raws] Harbour Lights - 05 [WEB 1080p].mkv", "06.mkv", NULL };
	static const gchar *const short_files[] = { "[Tsuki] Harbour Lights - 05 [BluRay 1080p].mkv", NULL };
	static const Rel again[] = { { BF_SHORT, 3000000000 } };
	static const Rel of_first[] = { { BF_FIRST, 3000000000 } };
	g_autofree gchar *three_four = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *five_six = NULL;
	g_autofree gchar *text = NULL;
	gint64 series = add_series(fx);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	gint64 e3 = episode_id(fx, series, 1, 3);
	gint64 e4 = episode_id(fx, series, 1, 4);
	gint64 e5 = episode_id(fx, series, 2, 1);
	gint64 e6 = episode_id(fx, series, 2, 2);
	gint64 import_id;
	AxisAcquireSearchReport report;

	three_four = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e3, e4);
	one = g_strdup_printf("{%" G_GINT64_FORMAT "}", e1);
	five_six = g_strdup_printf("{%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT "}", e5, e6);
	make_anime(fx, series);

	/* A batch fits a season search only when it holds a wanted episode of
	 * that season: one of S01E01 and S01E02 is no answer to a search of
	 * season 2, and is one to a search of season 1. */
	set_releases(fx, of_first, G_N_ELEMENTS(of_first));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 2, "list", "root", NULL, &report, NULL));
	assert_match(fx, BF_FIRST, "match", "it holds S01E01 to S01E02");
	assert_reason(fx, BF_FIRST, "not_season");
	text = q(fx, "SELECT r->>'t' FROM series_candidate c, jsonb_array_elements(c.reasons) r WHERE c.name = '" BF_FIRST
		"' AND r->>'c' = 'not_season'" LATEST);
	assert_contains(text, "nothing it holds of season 2 is wanted");
	g_clear_pointer(&text, g_free);
	set_releases(fx, of_first, G_N_ELEMENTS(of_first));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, NULL));
	assert_qf(fx, "t", "SELECT accepted FROM series_candidate WHERE name = '" BF_FIRST "'" LATEST);

	/* Grabbed for S01E03 and S01E04 by their absolute numbers; its files say
	 * S02E01 and S02E02, which are wanted and were never claimed. */
	write_download(fx, BF_UNCLAIMED, unclaimed_files);
	import_id = request_import(fx, add_download(fx, series, BF_UNCLAIMED, "nzo_unclaimed", three_four));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file");
	text = file_reason(fx, import_id, "S02E01");
	assert_contains(text, "it holds S02E01, and this download was grabbed by absolute numbers for other episodes");
	assert_contains(text, "Assign it by hand");
	g_clear_pointer(&text, g_free);
	/* The grab keeps its episodes: nothing lets them be grabbed again. */
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab WHERE closed_at IS NULL");
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* One file inside the claim and one outside: neither is placed. */
	write_download(fx, BF_MIXED, mixed_files);
	import_id = request_import(fx, add_download(fx, series, BF_MIXED, "nzo_mixed", three_four));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode");
	text = file_reason(fx, import_id, "- 03");
	assert_contains(text, "its name gives S01E03 by its absolute number (3 is S01E03 by TheTVDB), but another file of "
		"this download");
	assert_contains(text, "gives S02E01, which this download was not grabbed for");
	g_clear_pointer(&text, g_free);
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* A file that states a season and an episode the grab did claim is placed
	 * with the one named by its number. */
	write_download(fx, BF_WITHIN, within_files);
	import_id = request_import(fx, add_download(fx, series, BF_WITHIN, "nzo_within", three_four));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e3);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e4);
	assert_qf(fx, "2", "SELECT count(*) FROM series_file_episode");
	assert_qf(fx, "0", "SELECT count(*) FROM series_event WHERE text LIKE '%%got no file%%'");

	/* A grab that was not made by absolute numbers, of the same series: a
	 * second file that holds another wanted episode is placed, as it always
	 * was. */
	write_download(fx, BF_ORDINARY, ordinary_files);
	import_id = request_import(fx, add_download(fx, series, BF_ORDINARY, "nzo_ordinary", one));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e1);
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2);

	/* Grabbed for S02E01 and S02E02; one file says which it is, and one says
	 * nothing: the names do not account for both episodes, so nothing is
	 * placed and the import waits for a person, with the grab still open. */
	write_download(fx, BF_STRAY, stray_files);
	import_id = request_import(fx, add_download(fx, series, BF_STRAY, "nzo_stray", five_six));
	assert_import(fx, import_id, "blocked/nothing_wanted");
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id IN (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ")", e5, e6);
	text = file_reason(fx, import_id, "- 05");
	assert_contains(text, "another file of this download, 06.mkv, does not say which episode it is");
	assert_contains(text, "1 of the 2 episodes");
	g_clear_pointer(&text, g_free);
	assert_qf(fx, "1", "SELECT count(*) FROM series_grab WHERE closed_at IS NULL");
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");

	/* Grabbed for both, and it holds one file: that one is placed, the
	 * history says which episode got nothing, and no search grabs the same
	 * release again for it. */
	write_download(fx, BF_SHORT, short_files);
	import_id = request_import(fx, add_download(fx, series, BF_SHORT, "nzo_short", five_six));
	assert_import(fx, import_id, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e5);
	assert_qf(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e6);
	assert_qf(fx, "1", "SELECT count(*) FROM series_event WHERE text LIKE 'The download of " BF_SHORT " was imported, and"
		" S02E02, which it was grabbed for, got no file from it%%'");
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");
	set_releases(fx, again, G_N_ELEMENTS(again));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, e6);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_match(fx, BF_SHORT, "match", NULL);
	assert_reason(fx, BF_SHORT, "had_before");
	text = q(fx, "SELECT r->>'t' FROM series_candidate c, jsonb_array_elements(c.reasons) r WHERE c.name = '" BF_SHORT
		"' AND r->>'c' = 'had_before'" LATEST);
	assert_contains(text, "was downloaded before and S02E02 got no file from it");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/tv/anime/batch-files", test_anime_batch_files);
	ADD("/tv/anime/asks", test_anime_asks);
	AXIS_TEST_ADD_DB_CASE("/tv/anime/asks-native", Fixture, GUINT_TO_POINTER(NATIVE_ONLY), test_anime_asks_native);
	ADD("/tv/anime/batch", test_anime_batch);
	ADD("/tv/anime/search", test_anime_search);
	ADD("/tv/anime/import", test_anime_import);
	ADD("/tv/anime/place-by-name", test_anime_place_by_name);
	ADD("/tv/duplicates", test_duplicates);
	ADD("/tv/duplicates-shared-file", test_duplicates_shared_file);
	ADD("/tv/duplicates-relinked", test_duplicates_relinked);
	ADD("/tv/assign-import", test_assign_import);
	ADD("/tv/assign-again-keeps-placed", test_assign_again_keeps_placed);
	ADD("/tv/assign-left-alone-not-imported", test_assign_left_alone_not_imported);
	ADD("/tv/assign-tick-stale-blocks", test_assign_tick_stale_blocks);
	ADD("/tv/assign-clears-overrides", test_assign_clears_overrides);
	ADD("/tv/assign-again-keeps-published", test_assign_again_keeps_published);
	ADD("/tv/add", test_add);
	ADD("/tv/metadata", test_metadata);
	ADD("/tv/end-to-end", test_end_to_end);
	AXIS_TEST_ADD_DB_CASE("/tv/end-to-end-native", Fixture, GUINT_TO_POINTER(NATIVE_ONLY), test_end_to_end_native);
	ADD("/tv/season-pack", test_season_pack);
	ADD("/tv/pack-fits", test_pack_fits);
	ADD("/tv/pack-threshold", test_pack_threshold);
	ADD("/tv/double-episode", test_double_episode);
	ADD("/tv/race", test_race);
	ADD("/tv/mismatch", test_mismatch);
	ADD("/tv/same-name", test_same_name);
	ADD("/tv/air-delay", test_air_delay);
	ADD("/tv/failure", test_failure);
	ADD("/tv/import/extra", test_import_extra);
	ADD("/tv/import/blocked", test_import_blocked);
	ADD("/tv/import/upgrade", test_import_upgrade);
	ADD("/tv/import/upgrade-of-a-brought-file", test_upgrade_of_brought);
	ADD("/tv/import/pack-over-brought-files", test_pack_over_brought);
	ADD("/tv/hand-grab-import", test_hand_grab_import);
	ADD("/tv/hand-grab-claimed", test_hand_grab_claimed);
	ADD("/tv/hand-grab-numbering", test_hand_grab_numbering);
	ADD("/tv/person-only-beside", test_person_only_beside);
	ADD("/tv/hand-grab-follow", test_hand_grab_follow);
	ADD("/tv/import/partial", test_import_partial);
	ADD("/tv/import/partial-torrent", test_import_partial_torrent);
	ADD("/tv/import/partial-torrent-moved", test_import_partial_torrent_moved);
	ADD("/tv/delete-download", test_delete_download_partial_pack);
	ADD("/tv/import/season-folder-renamed", test_import_season_folder_renamed);
	ADD("/tv/import/crash", test_import_crash);
	ADD("/tv/forced-import", test_forced_import);
	ADD("/tv/forced-one-file", test_forced_one_file);
	ADD("/tv/forced-mixed-foreign", test_forced_mixed_foreign);
	ADD("/tv/forced-assign-wait", test_forced_assign_wait);
	ADD("/tv/forced-multi-season", test_forced_multi_season);
	ADD("/tv/forced-multi-season-picks", test_forced_multi_season_picks);
	ADD("/tv/kept-by-hand", test_kept_by_hand);
#undef ADD
	return g_test_run();
}
