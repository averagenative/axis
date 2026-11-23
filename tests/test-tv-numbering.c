/*
 * test-tv-numbering.c - changing a series' episode numbering between TMDb
 * (here the metadata test provider stands in for it) and TheTVDB's aired
 * order (the stand-in TheTVDB, tests/axis-test-tvdb.h): the mapping, the
 * change in one transaction, the way back, and what searches, release
 * matching, imports and the scheduler make of the series afterwards; on a
 * real database, a real volume, with real imports
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-metadata-tvdb, V3.  The series is Gullwing Ferry (made up): TMDb 7061
 * holds one season of ten half-hour episodes, the tenth with no air date;
 * TheTVDB 81061 numbers the same ten as two seasons of five, and gives the
 * tenth a date.  So TMDb's S01E08 is TheTVDB's S02E03.
 *
 * Spec scenarios (tv): A release numbered as TheTVDB numbers it; A date the
 * other source lacks; An absolute number; Matched by air date and title; A
 * file with no counterpart; Nothing renamed; Recorded; A pack downloading;
 * There and back; Changed since; Nothing before the confirmation; The key
 * removed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "axis-test-tvdb.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"

#define KEY      "prowlarr-key-for-numbering-7a1c0e52"
#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define TOOL     "tests/fixtures/import/fake-ffprobe.sh"

/* A half-hour episode, as the fake ffprobe reads it. */
#define EPISODE_LINE "AXISFAKE duration=1440 width=1920 height=1080 vcodec=h264 acodec=eac3 channels=6"

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

#define TMDB_ID  "7061"
#define TVDB_ID  "81061"
#define EPISODES "/v4/series/" TVDB_ID "/episodes/official/eng"

#define WEB_S01E08    "Gullwing.Ferry.S01E08.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define BLURAY_S02E03 "Gullwing.Ferry.S02E03.1080p.BluRay.x264-DEMAND"
#define ABSOLUTE_08   "[SubGroup] Gullwing Ferry - 08 [1080p]"

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	gchar                *shows;
	gchar                *downloads;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisTestTvdb         *tvdb;
	AxisMeta             *meta;
	AxisTestProwlarr     *prowlarr;
	AxisIndexers         *indexers;
	AxisTestDownloadFake *fake;
	AxisDownloads        *dl;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisProbe            *probe;
	AxisImport           *import;
	AxisTv               *tv;
	gchar                *tool[3];
} Fixture;

/* ── The series ─────────────────────────────────────────────────── */

typedef struct
{
	const gchar *title;
	const gchar *date;	/* TMDb's; NULL: none */
} Episode;

/* The ten, in TMDb's order. */
static const Episode ten[] = {
	{ "Low Water", "2025-01-06" }, { "The Ropes", "2025-01-13" }, { "Gull Count", "2025-01-20" },
	{ "Ticket Office", "2025-01-27" }, { "Fog Horn", "2025-02-03" }, { "Night Crossing", "2025-02-10" },
	{ "The Pier Lamp", "2025-02-17" }, { "Harbour Masters", "2025-02-24" }, { "Rough Passage", "2025-03-03" },
	{ "Last Sailing", NULL }
};

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, 200, "application/json", body, NULL);
}

/*
 * TMDb's record and season (@extra more episodes after the ten, numbered on,
 * dated a week apart from 2025-03-17, "Extra Crossing N"), and TheTVDB's
 * aired order of the ten (@tvdb_extra of them as more episodes of its season
 * 2).  @n is the number of episodes TMDb lists in all when larger (for the
 * series of forty).
 */
static void
set_series(
	Fixture *fx,
	guint    n,
	guint    extra,
	guint    tvdb_extra
){
	g_autoptr(GString) tmdb = g_string_new("{\"number\":1,\"name\":\"Season 1\",\"episodes\":[");
	g_autoptr(GString) tvdb = g_string_new("{\"status\":\"success\",\"data\":{\"series\":{\"id\":" TVDB_ID "},"
		"\"episodes\":[");
	g_autofree gchar *record = NULL;
	guint total = MAX(n, G_N_ELEMENTS(ten)) + extra;
	guint half = MAX(n, G_N_ELEMENTS(ten)) / 2;
	guint i;

	for (i = 0; i < total; i++) {
		g_autofree gchar *title = i < G_N_ELEMENTS(ten) && n == 0 ? g_strdup(ten[i].title) :
			g_strdup_printf("%s Crossing %u", i < MAX(n, G_N_ELEMENTS(ten)) ? "Ferry" : "Extra", i + 1);
		g_autofree gchar *date = NULL;
		GDate day;

		g_date_clear(&day, 1);
		g_date_set_dmy(&day, 6, G_DATE_JANUARY, 2025);
		g_date_add_days(&day, 7 * i);
		date = g_strdup_printf("%04u-%02u-%02u", g_date_get_year(&day), g_date_get_month(&day), g_date_get_day(&day));
		/* TMDb: the tenth of the ten has no date. */
		if (n == 0 && i < G_N_ELEMENTS(ten) && ten[i].date == NULL)
			g_string_append_printf(tmdb, "%s{\"number\":%u,\"id\":\"96%03u\",\"name\":\"%s\",\"runtime\":24}",
				i > 0 ? "," : "", i + 1, i + 1, title);
		else
			g_string_append_printf(tmdb, "%s{\"number\":%u,\"id\":\"96%03u\",\"name\":\"%s\",\"air_date\":\"%s\","
				"\"runtime\":24}", i > 0 ? "," : "", i + 1, i + 1, title, date);
		/* TheTVDB: two seasons of the first ones; one of its titles written
		 * otherwise ("Harbour Masters."). */
		if (i < MAX(n, G_N_ELEMENTS(ten)) + tvdb_extra) {
			gboolean second = i >= half;

			g_string_append_printf(tvdb, "%s{\"id\":81061%03u,\"seriesId\":" TVDB_ID ",\"name\":\"%s%s\",\"aired\":"
				"\"%s\",\"runtime\":24,\"seasonNumber\":%u,\"number\":%u,\"absoluteNumber\":%u}", i > 0 ? "," : "",
				i + 1, title, strcmp(title, "Harbour Masters") == 0 ? "." : "", date, second ? 2 : 1,
				second ? i - half + 1 : i + 1, i + 1);
		}
	}
	g_string_append(tmdb, "]}");
	g_string_append(tvdb, "]},\"links\":{\"prev\":null,\"next\":null}}");
	record = g_strdup_printf("{\"id\":\"" TMDB_ID "\",\"kind\":\"series\",\"title\":\"Gullwing Ferry\",\"year\":2025,"
		"\"status\":\"continuing\",\"first_air_date\":\"2025-01-06\",\"runtime\":24,\"ids\":{\"tvdb\":\"" TVDB_ID "\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"episodes\":%u}]}", total);
	set_route(fx, "/title/series/" TMDB_ID, record);
	set_route(fx, "/title/series/" TMDB_ID "/season/1", tmdb->str);
	axis_test_tvdb_set_answer(fx->tvdb, EPISODES, "page=0", tvdb->str);
}

/* ── Setting up ─────────────────────────────────────────────────── */

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
	axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "downloads", AXIS_MEDIA_DOWNLOADS);
	fx->shows = g_build_filename(fx->disk, "series", NULL);
	fx->downloads = g_build_filename(fx->disk, "downloads", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);

	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &indexer_tuning);
	fx->dl = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
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
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);

	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->tool[0] = g_strdup("/bin/sh");
	fx->tool[1] = g_canonicalize_filename(TOOL, NULL);
	fx->tool[2] = NULL;
	memset(&probe_tuning, 0, sizeof(probe_tuning));
	probe_tuning.tool_command = (const gchar *const *)fx->tool;
	axis_probe_set_tuning(fx->probe, &probe_tuning);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->dl, fx->probe);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->dl, fx->t->storage, fx->quality);
	axis_tv_set_import(fx->tv, fx->import);
	axis_tv_set_clock(fx->tv, NOW_US);

	fx->prowlarr = axis_test_prowlarr_new_recorded(KEY, "");
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
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
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
	axis_test_tvdb_free(fx->tvdb);
	axis_test_prowlarr_free(fx->prowlarr);
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
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static gint64
add_series(Fixture *fx)
{
	AxisMetaTitleAnswer answer;
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_SERIES, TMDB_ID, 5000, &answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer.title);
	memset(&spec, 0, sizeof(spec));
	spec.title_id = answer.title->id;
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_ALL;
	spec.user = "root";
	axis_meta_title_answer_clear(&answer);
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
		" AND number = %d AND gone_at IS NULL", series, season, number);
}

/* Every row, "id:SxxEyy:source:live:monitored", by id. */
static gchar *
rows_of(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT string_agg(id || ':S' || lpad(season_number::text, 2, '0') || 'E' || lpad(number::text, 2, '0')"
		" || ':' || source || ':' || (gone_at IS NULL)::text || ':' || monitored::text, ',' ORDER BY id)"
		" FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
}

/* Everything a change of numbering must leave alone: links, grabs,
 * history, blocklist. */
static gchar *
kept_of(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT coalesce((SELECT string_agg(f.file_id || '>' || f.episode_id, ',' ORDER BY f.file_id)"
		" FROM series_file_episode f JOIN series_file sf ON sf.file_id = f.file_id WHERE sf.series_id = %" G_GINT64_FORMAT
		"), '') || '|' || coalesce((SELECT string_agg(g.grab_id || '>' || g.episode_id, ',' ORDER BY g.grab_id, g.episode_id)"
		" FROM series_grab_episode g JOIN series_grab sg ON sg.id = g.grab_id WHERE sg.series_id = %" G_GINT64_FORMAT
		"), '') || '|' || coalesce((SELECT string_agg(e.id || '>' || coalesce(e.episode_id::text, '-'), ',' ORDER BY e.id)"
		" FROM series_event e WHERE e.series_id = %" G_GINT64_FORMAT " AND e.kind <> 'numbering'), '') || '|'"
		" || (SELECT count(*) FROM series_blocklist WHERE series_id = %" G_GINT64_FORMAT ")", series, series, series,
		series);
}

static AxisTvRenumberPlan *
plan_to(
	Fixture     *fx,
	gint64       series,
	const gchar *to
){
	AxisTvRenumberPlan *plan = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_tv_plan_numbering(fx->tv, series, to, TRUE, &plan, &error));
	g_assert_no_error(error);
	g_assert_nonnull(plan);
	return plan;
}

static void
change_to(
	Fixture     *fx,
	gint64       series,
	const gchar *to
){
	g_autoptr(AxisTvRenumberPlan) plan = plan_to(fx, series, to);
	g_autoptr(GError) error = NULL;
	gboolean changed = TRUE;

	g_assert_null(plan->problem);
	g_assert_true(axis_tv_change_numbering(fx->tv, series, to, plan->digest, FALSE, "root", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
}

static const AxisTvRenumbered *
planned(
	const AxisTvRenumberPlan *plan,
	gint64                    episode
){
	guint i;

	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		if (row->episode_id == episode)
			return row;
	}
	g_error("episode %" G_GINT64_FORMAT " is not in the plan", episode);
	return NULL;
}

static void apply_plan(Fixture *fx, AxisTvRenumberPlan *plan);

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
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=hearth-nzb\",\"magnetUrl\":null,\"protocol\":"
			"\"usenet\",\"tvdbId\":" TVDB_ID ",\"seeders\":null,\"leechers\":null,\"categories\":[{\"id\":5040}]}",
			i > 0 ? "," : "", i, rels[i].title, rels[i].size, origin, KEY, i);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
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

/* A video the fake ffprobe reads as a half-hour episode, of a size such an
 * episode has (300 MiB, sparse: nothing is written but its first line). */
static void
write_media(const gchar *path)
{
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = g_strdup_printf("%s\n", EPISODE_LINE);
	gint fd;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, &error));
	g_assert_no_error(error);
	fd = g_open(path, O_WRONLY, 0);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, (off_t)300 * 1024 * 1024), ==, 0);
	g_assert_cmpint(close(fd), ==, 0);
}

/* A completed download of @release holding @episodes ("{1}"), with its open
 * grab, as a search would have left it; its id. */
static gint64
add_download(
	Fixture     *fx,
	gint64       series,
	const gchar *release,
	const gchar *ref,
	const gchar *episodes
){
	g_autofree gchar *video = g_strdup_printf("%s.mkv", release);
	g_autofree gchar *file = g_build_filename(fx->downloads, release, video, NULL);
	g_autofree gchar *title = q(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	g_autofree gchar *path = g_strdup_printf("/data/downloads/%s", release);
	gint64 download;
	gint64 grab;

	write_media(file);
	download = qi(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'Usenet', '%s', 'usenet', 'file', '%s', %s,"
		" 'Gullwing Ferry (2025)', 'tv', 'completed', 1000, 1000, '%s', %" G_GINT64_FORMAT ", 'downloads/%s', now(),"
		" now()) RETURNING id", fx->client_id, ref, release, title, path, fx->volume_id, release);
	grab = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at, scope, episode_id) VALUES (%" G_GINT64_FORMAT ", '%s', 'usenet', lower('1:0:%s'), %"
		G_GINT64_FORMAT ", now(), now(), 'episode', ('%s'::bigint[])[1]) RETURNING id", series, release, release,
		download, episodes);
	exec(fx, "INSERT INTO series_grab_episode (grab_id, episode_id) SELECT %" G_GINT64_FORMAT ", unnest('%s'::bigint[])",
		grab, episodes);
	axis_test_download_fake_put(fx->fake, ref, release, "axis", NULL, "completed", 1000, 1000, path, NULL);
	return download;
}

/* The import of @download, run once: its state. */
static gchar *
import_of(
	Fixture *fx,
	gint64   download
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	AxisImportReport report;
	gint64 id = 0;
	gboolean created = FALSE;

	g_assert_no_error(error);
	g_assert_true(axis_import_request(fx->import, conn, AXIS_TV_MODULE_ID, download, &id, &created, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	memset(&report, 0, sizeof(report));
	if (!axis_import_run(fx->import, id, NULL, &report, &error))
		g_error("the import run failed: %s", error != NULL ? error->message : "?");
	return q(fx, "SELECT state || coalesce('/' || reason, '') || coalesce(' ' || reason_text, '') FROM import"
		" WHERE id = %" G_GINT64_FORMAT, id);
}

/* What is on disk of the series' files: "path:size:inode", by path. */
static gchar *
files_on_disk(
	Fixture *fx,
	gint64   series
){
	g_autofree gchar *paths = q(fx, "SELECT string_agg(lf.rel_path, '\n' ORDER BY lf.rel_path) FROM series_file sf"
		" JOIN library_file lf ON lf.id = sf.file_id WHERE sf.series_id = %" G_GINT64_FORMAT, series);
	g_auto(GStrv) each = paths != NULL ? g_strsplit(paths, "\n", -1) : NULL;
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; each != NULL && each[i] != NULL; i++) {
		g_autofree gchar *full = g_build_filename(fx->shows, each[i], NULL);
		GStatBuf st;

		g_assert_cmpint(g_lstat(full, &st), ==, 0);
		g_string_append_printf(out, "%s:%" G_GINT64_FORMAT ":%" G_GUINT64_FORMAT "\n", each[i], (gint64)st.st_size,
			(guint64)st.st_ino);
	}
	return g_string_free(out, FALSE);
}

/* A file of the library (on disk, under the series' folder) linked to
 * @episode as an import would have; its id. */
static gint64
file_on(
	Fixture     *fx,
	gint64       series,
	gint64       episode,
	const gchar *rel_path
){
	g_autofree gchar *full = g_build_filename(fx->shows, rel_path, NULL);
	GStatBuf st;
	gint64 file;

	write_media(full);
	g_assert_cmpint(g_lstat(full, &st), ==, 0);
	file = qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', 'video',"
		" %" G_GINT64_FORMAT ", 1, 0, 1, 0, %" G_GUINT64_FORMAT ", 1, false, false, false, 1, 1, 1) RETURNING id",
		fx->root_id, fx->volume_id, rel_path, (gint64)st.st_size, (guint64)st.st_ino);
	exec(fx, "INSERT INTO series_file (file_id, series_id, quality, release_name) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", 'webdl_1080p', 'Gullwing.Ferry.WEB-DL-NTb')", file, series);
	exec(fx, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")",
		file, episode);
	return file;
}

static AxisTvState
state_of(
	Fixture *fx,
	gint64   series_id,
	gint64   episode
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

		if (e->id == episode)
			return axis_tv_episode_derive(e, series, &rules, NULL);
	}
	g_error("no episode %" G_GINT64_FORMAT, episode);
	return AXIS_TV_WANTED;
}

/* ── The end to end ─────────────────────────────────────────────── */

/*
 * Spec: A release numbered as TheTVDB numbers it; Matched by air date and
 * title; Recorded; A date the other source lacks; An absolute number.  The
 * series follows TMDb: its S01E08 is imported, and a release named S02E03
 * matches nothing.  Changed to TheTVDB's aired order, the same row is
 * S02E03 with its file and its history; the S02E03 release is matched to it,
 * found by a search that asks for S02E03, judged against its half hour,
 * grabbed as an upgrade and imported into Season 02 under the new numbering.
 */
static void
test_end_to_end(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ BLURAY_S02E03, 1200000000 },
		{ ABSOLUTE_08, 1200000000 }
	};
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *kept = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *query = NULL;
	gint64 series;
	gint64 masters;
	gint64 sailing;
	gint64 file;
	gint64 events;
	AxisAcquireSearchReport report;
	AxisTvScheduleReport scheduled;
	guint asked;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series), ==, 10);
	masters = episode_id(fx, series, 1, 8);
	sailing = episode_id(fx, series, 1, 10);
	/* Its S01E08, imported under TMDb's numbering. */
	{
		g_autofree gchar *one = g_strdup_printf("{%" G_GINT64_FORMAT "}", masters);

		state = import_of(fx, add_download(fx, series, WEB_S01E08, "nzo_masters", one));
	}
	g_assert_cmpstr(state, ==, "imported");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, masters);
	text = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, file);
	assert_contains(text, "Season 01/");
	assert_contains(text, "S01E08");
	g_clear_pointer(&text, g_free);
	/* TMDb has no date for the tenth: it is never searched by itself. */
	g_assert_cmpint(state_of(fx, series, sailing), ==, AXIS_TV_NOT_AIRED);

	/* Under TMDb's numbering, S02E03 is no episode of the series. */
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, masters);
	g_assert_cmpstr(report.outcome, !=, "grabbed");
	text = q(fx, "SELECT match || ' ' || coalesce(match_text, '') FROM series_candidate WHERE name = '" BLURAY_S02E03 "'"
		" AND search_id = (SELECT max(id) FROM series_search)");
	assert_contains(text, "none");
	assert_contains(text, "S02E03");
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);
	kept = kept_of(fx, series);
	events = qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series);

	/* The plan: nine by air date and title, the undated one by its title
	 * alone (flagged), nothing lost, nothing new; the file's episode moves. */
	plan = plan_to(fx, series, "tvdb");
	g_assert_null(plan->problem);
	g_assert_cmpuint(plan->waits->len, ==, 0);
	g_assert_cmpuint(plan->n_by[AXIS_TV_MAPPED_DATE_TITLE], ==, 9);
	g_assert_cmpuint(plan->n_by[AXIS_TV_MAPPED_TITLE], ==, 1);
	g_assert_cmpuint(plan->n_by[AXIS_TV_MAPPED_NONE], ==, 0);
	g_assert_cmpuint(plan->added->len, ==, 0);
	g_assert_cmpint(planned(plan, masters)->to_season, ==, 2);
	g_assert_cmpint(planned(plan, masters)->to_number, ==, 3);
	g_assert_cmpuint(planned(plan, masters)->n_files, ==, 1);
	g_assert_cmpuint(planned(plan, masters)->n_files_after, ==, 1);
	g_assert_true(axis_tv_mapped_flagged(planned(plan, sailing)->how));
	g_assert_cmpstr(planned(plan, sailing)->to_air_date, ==, "2025-03-10");
	/* Planning wrote nothing of the series. */
	assert_q(fx, "fixture", "SELECT episode_source FROM series");

	/* The change. */
	{
		gboolean changed = TRUE;

		g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
			&error));
		g_assert_no_error(error);
		g_assert_false(changed);
	}
	assert_q(fx, "tvdb", "SELECT episode_source FROM series");
	/* The same row is S02E03, with its file, its grabs and its history. */
	g_assert_cmpint(episode_id(fx, series, 2, 3), ==, masters);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND gone_at IS NULL", series), ==, 10);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series), ==, 10);
	after = kept_of(fx, series);
	g_assert_cmpstr(after, ==, kept);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT
		" AND kind = 'imported'", series), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE episode_id = %" G_GINT64_FORMAT
		" AND kind = 'searched'", masters), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab g JOIN series_grab_episode ge ON ge.grab_id = g.id"
		" WHERE g.release_name = '" WEB_S01E08 "' AND ge.episode_id = %" G_GINT64_FORMAT, masters), ==, 1);
	g_assert_cmpint(state_of(fx, series, masters), ==, AXIS_TV_UPGRADE_WANTED);
	/* Recorded: who, from what to what, and the counts. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series), ==,
		events + 1);
	text = q(fx, "SELECT by_user || ': ' || text FROM series_event WHERE kind = 'numbering'");
	assert_contains(text, "root: Episode numbering changed from fixture to TheTVDB's aired order by root: 10 episodes "
		"matched (9 by air date and title, 1 by title only); 0 kept without a counterpart");
	assert_contains(text, "0 new");
	assert_contains(text, "No file was moved, renamed or deleted.");
	g_clear_pointer(&text, g_free);
	/* A date the other source lacks: the tenth is dated now, so wanted, and
	 * the scheduler searches for it by itself. */
	g_assert_cmpint(state_of(fx, series, sailing), ==, AXIS_TV_WANTED);
	g_assert_true(axis_tv_run_schedule(fx->tv, &scheduled, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND state = 'queued' AND (dedupe_key ="
		" 'tv.search:%" G_GINT64_FORMAT ":episode:%" G_GINT64_FORMAT "' OR dedupe_key = 'tv.search:%" G_GINT64_FORMAT
		":season:2' OR dedupe_key = 'tv.search:%" G_GINT64_FORMAT ":series')", series, sailing, series, series), >=, 1);
	exec(fx, "DELETE FROM job WHERE kind = 'tv.search'");

	/* The S02E03 release now holds that episode: searched for as S02E03,
	 * judged against its half hour, grabbed as an upgrade; the absolute
	 * number alone still refused. */
	asked = axis_test_prowlarr_n_searches(fx->prowlarr);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, series, AXIS_TV_SCOPE_EPISODE, masters);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), >, asked);
	{
		g_autofree gchar *raw = axis_test_prowlarr_search_query(fx->prowlarr, asked);

		query = raw != NULL ? g_uri_unescape_string(raw, NULL) : NULL;
	}
	g_test_message("the search asked: %s", query != NULL ? query : "(none)");
	assert_contains(query, "81061");
	assert_contains(query, "{Season:2}{Episode:3}");
	assert_q(fx, BLURAY_S02E03 "|S02E03", "SELECT release_name || '|' || holds FROM series_grab WHERE closed_at IS NULL");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab_episode WHERE open AND episode_id = %" G_GINT64_FORMAT,
		masters), ==, 1);
	text = q(fx, "SELECT match_text FROM series_candidate WHERE name = '" ABSOLUTE_08 "' AND search_id ="
		" (SELECT max(id) FROM series_search)");
	assert_contains(text, "only an absolute episode number");
	g_clear_pointer(&text, g_free);

	/* Downloaded and imported: into the same row, named by the new
	 * numbering. */
	ref = q(fx, "SELECT d.client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.closed_at IS NULL");
	{
		g_autofree gchar *video = g_build_filename(fx->downloads, BLURAY_S02E03, BLURAY_S02E03 ".mkv", NULL);
		AxisDownloadPollReport polled;
		AxisAcquireFollowReport followed;
		gint64 download;

		write_media(video);
		axis_test_download_fake_set(fx->fake, ref, "completed", 1200000000, "/data/downloads/" BLURAY_S02E03, NULL);
		g_assert_true(axis_downloads_poll_client(fx->dl, fx->client_id, NULL, &polled, &error));
		g_assert_no_error(error);
		g_assert_true(axis_tv_run_follow(fx->tv, &followed, &error));
		g_assert_no_error(error);
		download = qi(fx, "SELECT d.id FROM download d WHERE d.release_name = '" BLURAY_S02E03 "'");
		g_clear_pointer(&state, g_free);
		state = import_of(fx, download);
	}
	g_assert_cmpstr(state, ==, "imported");
	text = q(fx, "SELECT lf.rel_path FROM series_file_episode sfe JOIN library_file lf ON lf.id = sfe.file_id"
		" WHERE sfe.episode_id = %" G_GINT64_FORMAT " AND lf.state = 'present' AND lf.id <> %" G_GINT64_FORMAT, masters,
		file);
	assert_contains(text, "Season 02/");
	assert_contains(text, "S02E03");
	assert_contains(text, "Harbour Masters");
	g_assert_cmpint(state_of(fx, series, masters), ==, AXIS_TV_IN_LIBRARY);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT
		" AND kind = 'imported'", series), ==, 2);
	/* Imported after the change, named and read under the numbering
	 * followed: never among the files whose names say another episode. */
	assert_q(fx, "tvdb", "SELECT string_agg(DISTINCT sf.read_source, ',') FROM series_file sf JOIN library_file lf"
		" ON lf.id = sf.file_id WHERE lf.rel_path LIKE '%S02E03%'");
	{
		g_autoptr(GPtrArray) misplaced = NULL;
		guint i;

		g_assert_true(axis_tv_list_misplaced(fx->tv, series, 0, &misplaced, NULL, NULL, &error));
		g_assert_no_error(error);
		for (i = 0; i < misplaced->len; i++)
			g_assert_null(strstr(((AxisTvMisplaced *)g_ptr_array_index(misplaced, i))->path, "S02E03"));
	}
}

/* ── The rows that do not map ───────────────────────────────────── */

/*
 * Spec: A file with no counterpart.  TMDb lists an eleventh episode TheTVDB
 * does not, with a file whose name (S01E11) gives no episode TheTVDB has:
 * kept, marked gone, its file linked and on disk where it was; never
 * searched for.
 */
static void
test_no_counterpart(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *disk_after = NULL;
	g_autofree gchar *reason = NULL;
	g_autoptr(AxisTvSeries) s = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GError) error = NULL;
	gint64 series;
	gint64 extra;
	gint64 file;
	guint i;

	set_series(fx, 0, 1, 0);
	series = add_series(fx);
	extra = episode_id(fx, series, 1, 11);
	file = file_on(fx, series, extra, "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E11 - Extra.mkv");
	disk = files_on_disk(fx, series);
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->n_by[AXIS_TV_MAPPED_NONE], ==, 1);
	g_assert_cmpuint(plan->n_unmapped_files, ==, 1);
	g_assert_cmpint(planned(plan, extra)->to_season, ==, -1);
	g_assert_cmpuint(planned(plan, extra)->n_files, ==, 1);
	g_assert_cmpint(planned(plan, extra)->name_season, ==, 1);
	g_assert_cmpint(planned(plan, extra)->name_number, ==, 11);
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	change_to(fx, series, "tvdb");

	/* Kept as gone, numbered as TMDb numbers it, the file linked and where it
	 * was. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT " AND gone_at IS NOT NULL"
		" AND source = 'fixture' AND season_number = 1 AND number = 11", extra), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, file, extra), ==, 1);
	disk_after = files_on_disk(fx, series);
	g_assert_cmpstr(disk_after, ==, disk);
	g_assert_true(axis_tv_get(fx->tv, series, &s, &episodes, &error));
	g_assert_no_error(error);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		AxisTvRules rules;

		if (e->id != extra)
			continue;
		axis_tv_get_rules(fx->tv, &rules);
		g_assert_true(e->gone);
		g_assert_cmpuint(e->n_files, ==, 1);
		g_assert_cmpint(axis_tv_episode_derive(e, s, &rules, &reason), ==, AXIS_TV_UNMONITORED);
	}
	assert_contains(reason, "on an episode TheTVDB's aired order lacks");
	/* The scheduler leaves it alone. */
	{
		AxisTvScheduleReport scheduled;

		g_assert_true(axis_tv_run_schedule(fx->tv, &scheduled, &error));
		g_assert_no_error(error);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND dedupe_key = 'tv.search:%"
			G_GINT64_FORMAT ":episode:%" G_GINT64_FORMAT "'", series, extra), ==, 0);
	}
}

/* TheTVDB numbers an episode TMDb lacks: added, monitored as its season is,
 * wanted; with the box, left unmonitored. */
static void
test_new_episodes(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	gboolean changed = TRUE;
	gint64 series;
	gint64 added;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	/* TheTVDB's order has an eleventh (its S02E06); TMDb's is not asked
	 * again, so the series keeps its ten. */
	set_series(fx, 0, 1, 1);
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->added->len, ==, 1);
	g_assert_cmpint(((AxisTvRenumberNew *)g_ptr_array_index(plan->added, 0))->season, ==, 2);
	g_assert_cmpint(((AxisTvRenumberNew *)g_ptr_array_index(plan->added, 0))->number, ==, 6);
	g_assert_true(((AxisTvRenumberNew *)g_ptr_array_index(plan->added, 0))->monitored);
	g_assert_cmpuint(plan->n_new_wanted, ==, 1);
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, TRUE, "root", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_false(changed);
	added = episode_id(fx, series, 2, 6);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT " AND NOT monitored"
		" AND source = 'tvdb'", added), ==, 1);
	g_assert_cmpint(state_of(fx, series, added), ==, AXIS_TV_UNMONITORED);
}

/* ── There and back ─────────────────────────────────────────────── */

/*
 * Spec: There and back; Nothing renamed.  A series of forty episodes, each
 * with a file: changed to TheTVDB's aired order and back, every row has its
 * first number, files, grabs and history again, and every file the same
 * path, size and inode.  An unmapped row gone by the first change comes back
 * with its file.
 */
static void
test_there_and_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) back = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *kept = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *middle = NULL;
	g_autofree gchar *again = NULL;
	gint64 series;
	gint64 masters;
	guint i;

	set_series(fx, 40, 1, 0);
	series = add_series(fx);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series), ==, 41);
	for (i = 1; i <= 41; i++) {
		g_autofree gchar *path = g_strdup_printf("Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E%02u.mkv",
			i);

		file_on(fx, series, episode_id(fx, series, 1, (gint)i), path);
	}
	masters = episode_id(fx, series, 1, 23);
	exec(fx, "UPDATE series_episode SET monitored = false WHERE id = %" G_GINT64_FORMAT, masters);
	rows = rows_of(fx, series);
	kept = kept_of(fx, series);
	disk = files_on_disk(fx, series);

	{
		g_autoptr(AxisTvRenumberPlan) there = plan_to(fx, series, "tvdb");

		apply_plan(fx, there);
	}
	/* Twenty and twenty; the 41st kept as gone with its file; S01E23 is
	 * S02E03, still unmonitored. */
	g_assert_cmpint(episode_id(fx, series, 2, 3), ==, masters);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND gone_at IS NULL AND source = 'tvdb'", series), ==, 40);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT " AND NOT monitored",
		masters), ==, 1);
	middle = files_on_disk(fx, series);
	g_assert_cmpstr(middle, ==, disk);
	g_clear_pointer(&middle, g_free);
	middle = kept_of(fx, series);
	g_assert_cmpstr(middle, ==, kept);

	/* Back: every one as it was before, by the identity each held. */
	back = plan_to(fx, series, "fixture");
	g_assert_cmpuint(back->n_by[AXIS_TV_MAPPED_HELD], ==, 40);
	g_assert_cmpuint(back->n_by[AXIS_TV_MAPPED_RETURNS], ==, 1);
	g_assert_cmpuint(back->added->len, ==, 0);
	apply_plan(fx, back);
	again = rows_of(fx, series);
	g_assert_cmpstr(again, ==, rows);
	g_clear_pointer(&again, g_free);
	again = kept_of(fx, series);
	g_assert_cmpstr(again, ==, kept);
	g_clear_pointer(&again, g_free);
	again = files_on_disk(fx, series);
	g_assert_cmpstr(again, ==, disk);
	/* Two events: there, and back. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'numbering' AND series_id = %"
		G_GINT64_FORMAT, series), ==, 2);
}

/* ── Refused ────────────────────────────────────────────────────── */

/* Spec: A pack downloading: refused, naming it; nothing changes.  Spec:
 * Changed since: a file imported after the page was made: nothing changes,
 * and the plan is given again.  Spec: Nothing before the confirmation. */
static void
test_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(AxisTvRenumberPlan) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *now = NULL;
	gboolean changed = FALSE;
	gint64 series;
	gint64 pack;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	rows = rows_of(fx, series);
	/* Looking changes nothing. */
	plan = plan_to(fx, series, "tvdb");
	now = rows_of(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_clear_pointer(&now, g_free);

	/* A season pack downloading. */
	pack = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, scope, season_number)"
		" VALUES (%" G_GINT64_FORMAT ", 'Gullwing.Ferry.S01.1080p.WEB-DL.DDP5.1.H.264-NTb', 'usenet', 'gullwing ferry s01',"
		" 'season', 1) RETURNING id", series);
	exec(fx, "INSERT INTO series_grab_episode (grab_id, episode_id) SELECT %" G_GINT64_FORMAT ", id FROM series_episode"
		" WHERE series_id = %" G_GINT64_FORMAT, pack, series);
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->waits->len, ==, 1);
	g_assert_false(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
		&error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT);
	assert_contains(error->message, "Gullwing.Ferry.S01.1080p.WEB-DL.DDP5.1.H.264-NTb (downloading)");
	assert_contains(error->message, "once each is imported or dismissed");
	g_clear_error(&error);
	/* The same grab made by force (bound): still waited for. */
	exec(fx, "UPDATE series_grab SET manual = true, set_aside = 'hand', bound = true, grabbed_by = 'root',"
		" forced = '[{\"k\": \"safety\", \"c\": \"no_match\", \"t\": \"It is not this series.\"}]'::jsonb WHERE id = %"
		G_GINT64_FORMAT, pack);
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->waits->len, ==, 1);
	g_assert_false(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
		&error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT);
	g_clear_error(&error);
	now = rows_of(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_clear_pointer(&now, g_free);
	assert_q(fx, "fixture", "SELECT episode_source FROM series");
	exec(fx, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed' WHERE id = %" G_GINT64_FORMAT, pack);

	/* Changed since: a file arrives after the page was made. */
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	plan = plan_to(fx, series, "tvdb");
	file_on(fx, series, episode_id(fx, series, 1, 2), "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E02.mkv");
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", &again, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_true(changed);
	g_assert_nonnull(again);
	g_assert_cmpstr(again->digest, !=, plan->digest);
	g_assert_cmpuint(planned(again, episode_id(fx, series, 1, 2))->n_files, ==, 1);
	now = rows_of(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'numbering'"), ==, 0);

	/* A source it cannot follow. */
	g_clear_pointer(&again, axis_tv_renumber_plan_free);
	g_assert_false(axis_tv_plan_numbering(fx->tv, series, "tvdb-dvd", FALSE, &again, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID);
}

/* ── Without TheTVDB ────────────────────────────────────────────── */

/*
 * Spec: The key removed.  A series following TheTVDB's aired order keeps its
 * episodes, wanted state and files when the key is cleared, refreshes or
 * not; nothing is asked of TheTVDB; it says why its numbering is not
 * refreshed, and can still change back to TMDb's.  TheTVDB's aired order is
 * offered no more to another series.  A provider that fails midway leaves a
 * series as it was.
 */
static void
test_key_removed(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvSeries) s = NULL;
	g_autoptr(GPtrArray) choices = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *why = NULL;
	gint64 series;
	guint gets;
	guint changes = 0;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	file_on(fx, series, episode_id(fx, series, 1, 8), "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E08.mkv");
	change_to(fx, series, "tvdb");
	rows = rows_of(fx, series);

	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TVDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	gets = axis_test_tvdb_gets(fx->tvdb);
	exec(fx, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'");
	g_assert_true(axis_tv_sync(fx->tv, series, TRUE, &changes, &error));
	g_assert_no_error(error);
	now = rows_of(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_assert_cmpuint(axis_test_tvdb_gets(fx->tvdb), ==, gets);
	g_assert_cmpint(state_of(fx, series, episode_id(fx, series, 2, 3)), ==, AXIS_TV_UPGRADE_WANTED);
	g_assert_true(axis_tv_get(fx->tv, series, &s, NULL, &error));
	g_assert_no_error(error);
	why = axis_tv_numbering_not_refreshed(fx->tv, s);
	assert_contains(why, "Its numbering is not being refreshed: TheTVDB has no key");
	choices = axis_tv_numbering_choices(fx->tv, s);
	g_assert_cmpuint(choices->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(choices, 1), ==, "fixture");

	/* Back to TMDb's numbering, from what is stored of it. */
	change_to(fx, series, "fixture");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND gone_at IS NULL AND source = 'fixture'", series), ==, 10);
	g_clear_pointer(&choices, g_ptr_array_unref);
	g_clear_pointer(&s, axis_tv_series_free);
	g_assert_true(axis_tv_get(fx->tv, series, &s, NULL, &error));
	g_assert_no_error(error);
	choices = axis_tv_numbering_choices(fx->tv, s);
	g_assert_cmpuint(choices->len, ==, 1);
	g_assert_false(axis_tv_plan_numbering(fx->tv, series, "tvdb", FALSE, &plan, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID);
}

/* ── The review of V3 ───────────────────────────────────────────── */

/* The plan drawn is what was applied (review item 8): each mapped row at its
 * number after, under the source followed; each unmapped one kept, gone,
 * as it was; each one kept from before back; each added one there; nothing
 * else live. */
static void
assert_applied(
	Fixture                  *fx,
	const AxisTvRenumberPlan *plan,
	gint64                    max_before
){
	guint live = 0;
	guint i;

	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		if (row->to_season >= 0) {
			g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT
				" AND season_number = %d AND number = %d AND source = '%s' AND gone_at IS NULL", row->episode_id,
				row->to_season, row->to_number, plan->to), ==, 1);
			g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
				row->episode_id), ==, row->n_files_after);
			live++;
		} else {
			g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT
				" AND source = '%s' AND gone_at IS NOT NULL", row->episode_id, plan->from), ==, 1);
			g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
				row->episode_id), ==, row->n_files);
		}
	}
	for (i = 0; i < plan->added->len; i++) {
		const AxisTvRenumberNew *a = g_ptr_array_index(plan->added, i);

		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
			" AND id > %" G_GINT64_FORMAT " AND season_number = %d AND number = %d AND gone_at IS NULL", plan->series_id,
			max_before, a->season, a->number), ==, 1);
		live++;
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND gone_at IS NULL", plan->series_id), ==, live);
}

static gint64
max_episode(
	Fixture *fx,
	gint64   series
){
	return qi(fx, "SELECT coalesce(max(id), 0) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
}

/* Applies @plan as drawn; asserts it was what happened. */
static void
apply_plan(
	Fixture            *fx,
	AxisTvRenumberPlan *plan
){
	g_autoptr(GError) error = NULL;
	gint64 max_before = max_episode(fx, plan->series_id);
	gboolean changed = TRUE;

	g_assert_null(plan->problem);
	g_assert_true(axis_tv_change_numbering(fx->tv, plan->series_id, plan->to, plan->digest, FALSE, "root", NULL,
		&changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	assert_applied(fx, plan, max_before);
}

/* Review blocker 3, a person's grab: a manual search's listing made under
 * TMDb's numbering is not grabbed once the series follows TheTVDB's. */
static void
test_listing_before(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = { { WEB_S01E08, 1200000000 } };
	g_autoptr(GError) error = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireSearchReport report;
	AxisAcquireHandTicks ticks = { FALSE, FALSE, NULL, NULL };
	AxisAcquireHandGrabbed grabbed;
	gint64 series;
	gint64 masters;
	gint64 candidate;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	masters = episode_id(fx, series, 1, 8);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, masters, "list", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	candidate = qi(fx, "SELECT id FROM series_candidate WHERE name = '" WEB_S01E08 "'");
	change_to(fx, series, "tvdb");
	memset(&grabbed, 0, sizeof(grabbed));
	axis_tv_grab_by_hand(fx->tv, series, candidate, &ticks, "root", &grabbed, &result, &error);
	g_clear_error(&error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);
	/* A stored release of an automatic search is not grabbed either. */
	axis_tv_grab(fx->tv, series, candidate, "root", NULL, &error);
	g_clear_error(&error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);
}

typedef struct
{
	Fixture *fx;
	gint64   series;
	guint    fired;
} Race;

static void
switch_now(
	gint64   owner_id,
	gpointer user_data
){
	Race *race = user_data;

	if (race->fired++ > 0)
		return;
	change_to(race->fx, race->series, "tvdb");
}

/* Review blocker 3, a search that runs while the numbering changes: what it
 * found under TMDb's numbering is not claimed under TheTVDB's. */
static void
test_search_during(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = { { WEB_S01E08, 1200000000 } };
	Race race;
	gint64 masters;

	set_series(fx, 0, 0, 0);
	race.fx = fx;
	race.series = add_series(fx);
	race.fired = 0;
	masters = episode_id(fx, race.series, 1, 8);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	axis_tv_set_claim_hook(fx->tv, switch_now, &race);
	{
		AxisAcquireSearchReport report;
		g_autoptr(GError) error = NULL;

		axis_tv_run_search(fx->tv, race.series, AXIS_TV_SCOPE_EPISODE, masters, "scheduled", NULL, NULL, &report,
			&error);
	}
	axis_tv_set_claim_hook(fx->tv, NULL, NULL);
	g_assert_cmpuint(race.fired, >=, 1);
	assert_q(fx, "tvdb", "SELECT episode_source FROM series");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);

	/* A grab claimed under the old numbering all the same (stamped so): its
	 * download is held for a person with the reason, nothing imported. */
	{
		g_autofree gchar *one = g_strdup_printf("{%" G_GINT64_FORMAT "}", masters);
		g_autofree gchar *state = NULL;
		gint64 download = add_download(fx, race.series, WEB_S01E08, "nzo_late", one);

		exec(fx, "UPDATE series_grab SET source = 'fixture' WHERE download_id = %" G_GINT64_FORMAT, download);
		state = import_of(fx, download);
		assert_contains(state, "blocked");
		assert_contains(state, "grabbed while the series was numbered as fixture numbers it");
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file"), ==, 0);
	}
}

/* Review item 4: a refresh of the series' own rows between the page and the
 * confirmation changes the mapping: refused as changed. */
static void
test_digest_rows(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	gboolean changed = FALSE;
	gint64 series;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	plan = plan_to(fx, series, "tvdb");
	exec(fx, "UPDATE series_episode SET title = 'Something Else Entirely', runtime_minutes = NULL WHERE id = %"
		G_GINT64_FORMAT, episode_id(fx, series, 1, 9));
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_true(changed);
	assert_q(fx, "fixture", "SELECT episode_source FROM series");
}

/* Review item 5: an episode TMDb no longer lists, kept with its file, that
 * TheTVDB lists: matched (it comes back), not added again and wanted. */
static void
test_gone_with_file(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	gint64 series;
	gint64 lamp;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	lamp = episode_id(fx, series, 1, 7);
	file_on(fx, series, lamp, "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E07.mkv");
	exec(fx, "UPDATE series_episode SET gone_at = now() WHERE id = %" G_GINT64_FORMAT, lamp);
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->added->len, ==, 0);
	g_assert_cmpint(planned(plan, lamp)->to_season, ==, 2);
	g_assert_cmpint(planned(plan, lamp)->to_number, ==, 2);
	apply_plan(fx, plan);
	g_assert_cmpint(state_of(fx, series, lamp), !=, AXIS_TV_WANTED);
}

/* Review item 6: there and back while TMDb gives its episodes new ids: back,
 * each row is the episode it was, by the place it held (its air date and
 * title agreeing: re-review item 1). */
static void
test_back_after_refresh(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) back = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *again = NULL;
	gint64 series;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	file_on(fx, series, episode_id(fx, series, 1, 8), "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E08.mkv");
	rows = rows_of(fx, series);
	change_to(fx, series, "tvdb");
	exec(fx, "UPDATE meta_episode SET provider_id = 'n' || provider_id WHERE source = 'fixture'");
	back = plan_to(fx, series, "fixture");
	g_assert_cmpuint(back->n_by[AXIS_TV_MAPPED_HELD], ==, 10);
	apply_plan(fx, back);
	again = rows_of(fx, series);
	g_assert_cmpstr(again, ==, rows);
}

/* Review items 7 and 11: the other source's stored episodes hold one id at
 * two places (TheTVDB's parser keeps an id once within an answer; the store
 * can still hold one twice, as a source fetched season by season does while
 * an episode moves): neither is matched, the plan says so plainly, and the
 * change goes through instead of failing on the rows' unique key. */
static void
test_doubled_id(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	gint64 series;
	gint64 masters;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	masters = episode_id(fx, series, 1, 8);
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	plan = plan_to(fx, series, "tvdb");
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	exec(fx, "UPDATE meta_episode SET provider_id = '81061008' WHERE source = 'tvdb' AND season_number = 2"
		" AND number = 4");
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpint(planned(plan, masters)->to_season, ==, -1);
	g_assert_cmpint(planned(plan, episode_id(fx, series, 1, 9))->to_season, ==, -1);
	g_assert_nonnull(plan->notes);
	assert_contains(plan->notes, "lists one episode at two places (S02E03 and S02E04)");
	apply_plan(fx, plan);
}

/* Re-review, item 1: back to TMDb after it gave its episodes new ids, one of
 * them deleted and another listed at its place: that row is not "as before"
 * at the place; it is matched by the ordinary rules (here: none). */
static void
test_held_place_differs(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) back = NULL;
	gint64 series;
	gint64 masters;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	masters = episode_id(fx, series, 1, 8);
	change_to(fx, series, "tvdb");
	exec(fx, "UPDATE meta_episode SET provider_id = 'n' || provider_id WHERE source = 'fixture'");
	exec(fx, "UPDATE meta_episode SET name = 'A Different Story', air_date = '2025-06-01' WHERE source = 'fixture'"
		" AND season_number = 1 AND number = 8");
	g_assert_true(axis_tv_plan_numbering(fx->tv, series, "fixture", FALSE, &back, NULL));
	g_assert_cmpint(planned(back, masters)->how, !=, AXIS_TV_MAPPED_HELD);
	g_assert_cmpuint(back->n_by[AXIS_TV_MAPPED_HELD], ==, 9);
	g_assert_cmpint(planned(back, episode_id(fx, series, 2, 2))->how, ==, AXIS_TV_MAPPED_HELD);
}

/* Re-review, item 4: an id stored at two places in seasons fetched at other
 * times: the plan adds it where the sync keeps it (the season fetched
 * last). */
static void
test_doubled_across_seasons(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	gint64 series;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	plan = plan_to(fx, series, "tvdb");
	g_clear_pointer(&plan, axis_tv_renumber_plan_free);
	exec(fx, "UPDATE meta_episode SET provider_id = '81061005' WHERE source = 'tvdb' AND season_number = 2"
		" AND number = 4");
	exec(fx, "UPDATE meta_season SET episodes_fetched_at = episodes_fetched_at - interval '1 hour' WHERE source = 'tvdb'"
		" AND number = 1");
	g_assert_true(axis_tv_plan_numbering(fx->tv, series, "tvdb", FALSE, &plan, &error));
	g_assert_no_error(error);
	assert_contains(plan->notes, "added once, as S02E04");
	apply_plan(fx, plan);
}

/* Re-review, item 5: a gone duplicate of a live row (same air date and
 * title), kept for its file: the live row is matched, the duplicate stays
 * kept and gone with its file, and nothing is added to be downloaded again. */
static void
test_gone_duplicate(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	gint64 series;
	gint64 lamp;
	gint64 duplicate;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	lamp = episode_id(fx, series, 1, 7);
	duplicate = qi(fx, "INSERT INTO series_episode (series_id, provider_id, season_number, number, title, air_date,"
		" runtime_minutes, monitored, gone_at, source) SELECT series_id, 'old7', season_number, number, title, air_date,"
		" runtime_minutes, true, now(), source FROM series_episode WHERE id = %" G_GINT64_FORMAT " RETURNING id", lamp);
	file_on(fx, series, duplicate, "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E07.mkv");
	plan = plan_to(fx, series, "tvdb");
	g_assert_cmpuint(plan->added->len, ==, 0);
	g_assert_cmpint(planned(plan, lamp)->to_season, ==, 2);
	g_assert_cmpint(planned(plan, duplicate)->to_season, ==, -1);
	apply_plan(fx, plan);
}

/*
 * add-force-grab-and-assign, C1: a link made by hand is kept through an
 * import with an upgrade.  The file of S01E07 was linked there by hand (it
 * was on S01E08), so it is kept by hand; a better release of S01E07 that Axis
 * grabbed by itself is downloaded: its import is held (kept_by_hand), the
 * link stands, and no file of the library is touched.  Once a person lets
 * Axis manage the file again, the same import replaces it.
 */
static void
test_hand_link_import(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *state = NULL;
	gboolean changed = TRUE;
	gint64 series;
	gint64 lamp;
	gint64 file;
	gint64 download;
	gint64 import_id;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	lamp = episode_id(fx, series, 1, 7);
	file = file_on(fx, series, episode_id(fx, series, 1, 8),
		"Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E08.mkv");
	/* (The file was linked by an import: letting Axis manage it again keeps its
	 * link where it is, and only the person's marks go.) */
	exec(fx, "INSERT INTO import (module, title_id, title_label, source_path, state, kind, finished_at)"
		" SELECT 'tv', title_id, 'Gullwing Ferry (2025)', 'downloads/earlier', 'imported', 'manual', now() FROM series"
		" WHERE id = %" G_GINT64_FORMAT, series);
	exec(fx, "UPDATE series_file SET import_id = (SELECT max(id) FROM import) WHERE file_id = %" G_GINT64_FORMAT, file);
	g_ptr_array_add(asks, axis_tv_link_ask_new(file, FALSE, "S01E07"));
	g_assert_true(axis_tv_links_review(fx->tv, series, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_nonnull(((const AxisTvLinkChange *)g_ptr_array_index(review->changes, 0))->note);
	g_assert_true(axis_tv_links_apply(fx->tv, series, asks, NULL, review->digest, "root", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file sf JOIN series_file_episode f ON f.file_id = sf.file_id"
		" WHERE sf.file_id = %" G_GINT64_FORMAT " AND f.episode_id = %" G_GINT64_FORMAT " AND sf.hand_how = 'linked'"
		" AND sf.kept", file, lamp), ==, 1);
	g_assert_cmpint(state_of(fx, series, lamp), ==, AXIS_TV_IN_LIBRARY);
	disk = files_on_disk(fx, series);

	/* A better release of it, grabbed by Axis itself, arrives. */
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", lamp);
	download = add_download(fx, series, "Gullwing.Ferry.S01E07.1080p.BluRay.x264-DEMAND", "nzo_lamp_bluray", ids);
	state = import_of(fx, download);
	assert_contains(state, "blocked/kept_by_hand");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file sf JOIN series_file_episode f ON f.file_id = sf.file_id"
		" WHERE sf.file_id = %" G_GINT64_FORMAT " AND f.episode_id = %" G_GINT64_FORMAT " AND sf.hand_how = 'linked'"
		" AND sf.kept", file, lamp), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp), ==, 1);
	after = files_on_disk(fx, series);
	g_assert_cmpstr(after, ==, disk);

	/* Let go by a person: the import, retried, replaces it. */
	g_assert_true(axis_tv_links_manage_review(fx->tv, series, file, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_RELEASE);
	g_assert_true(axis_tv_links_manage(fx->tv, series, file, handed->digest, "root", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	import_id = qi(fx, "SELECT id FROM import WHERE download_id = %" G_GINT64_FORMAT, download);
	g_assert_true(axis_import_act(fx->import, import_id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	{
		AxisImportReport report;

		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_run(fx->import, import_id, NULL, &report, &error));
		g_assert_no_error(error);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import WHERE id = %" G_GINT64_FORMAT " AND state = 'imported'",
		import_id), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file sf JOIN series_file_episode f ON f.file_id = sf.file_id"
		" WHERE f.episode_id = %" G_GINT64_FORMAT " AND sf.release_name LIKE '%%BluRay%%'", lamp), ==, 1);
}

/*
 * add-force-grab-and-assign, C1: a file a person unlinked by hand stays
 * unlinked, but an import by Axis of a new file at its path is a new file: the
 * mark is cleared by it.
 */
static void
test_import_over_unlinked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *again = NULL;
	gboolean changed = TRUE;
	gint64 series;
	gint64 lamp;
	gint64 download;
	gint64 file;

	set_series(fx, 0, 0, 0);
	series = add_series(fx);
	lamp = episode_id(fx, series, 1, 7);
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", lamp);
	download = add_download(fx, series, "Gullwing.Ferry.S01E07.1080p.BluRay.x264-DEMAND", "nzo_lamp_first", ids);
	state = import_of(fx, download);
	assert_contains(state, "imported");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp);
	g_ptr_array_add(asks, axis_tv_link_ask_new(file, TRUE, NULL));
	g_assert_true(axis_tv_links_review(fx->tv, series, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_links_apply(fx->tv, series, asks, NULL, review->digest, "root", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, file), ==, 1);
	/* The person deletes the file; the index has not noticed yet.  The same
	 * release is imported again to the same path: the index row is reused. */
	{
		g_autofree gchar *rel = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, file);
		g_autofree gchar *full = g_build_filename(fx->shows, rel, NULL);

		g_assert_cmpint(g_unlink(full), ==, 0);
	}
	download = add_download(fx, series, "Gullwing.Ferry.S01E07.1080p.BluRay.x264-DEMAND", "nzo_lamp_second", ids);
	again = import_of(fx, download);
	g_assert_cmpstr(again, ==, "imported");
	g_assert_cmpint(qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp), ==, file);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/tv/numbering/hand-link-import", test_hand_link_import);
	ADD("/tv/numbering/import-over-unlinked", test_import_over_unlinked);
	ADD("/tv/numbering/held-place-differs", test_held_place_differs);
	ADD("/tv/numbering/doubled-across-seasons", test_doubled_across_seasons);
	ADD("/tv/numbering/gone-duplicate", test_gone_duplicate);
	ADD("/tv/numbering/listing-before", test_listing_before);
	ADD("/tv/numbering/search-during", test_search_during);
	ADD("/tv/numbering/digest-rows", test_digest_rows);
	ADD("/tv/numbering/gone-with-file", test_gone_with_file);
	ADD("/tv/numbering/back-after-refresh", test_back_after_refresh);
	ADD("/tv/numbering/doubled-id", test_doubled_id);
	ADD("/tv/numbering/end-to-end", test_end_to_end);
	ADD("/tv/numbering/no-counterpart", test_no_counterpart);
	ADD("/tv/numbering/new-episodes", test_new_episodes);
	ADD("/tv/numbering/there-and-back", test_there_and_back);
	ADD("/tv/numbering/refused", test_refused);
	ADD("/tv/numbering/key-removed", test_key_removed);
#undef ADD
	return g_test_run();
}
