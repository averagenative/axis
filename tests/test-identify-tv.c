/*
 * test-identify-tv.c - identification of the series already on disk, on a
 * real database and real volumes: roots for TV built in scratch directories
 * (made-up series, Plex-, Sonarr- and release-named folders and files),
 * scanned, their probes recorded, identified against the metadata test
 * provider, each episode file placed on the series' episodes, and the TV
 * module as the owner of what is found
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of identify-series-on-disk: 2.x (series folders matched by the films'
 * rules: identifiers in names decide, a name and year with the episode's
 * runtime is sure, no year is likely and held), 3.x (each video read for the
 * episodes its name holds and placed against the series' data when read:
 * double episodes, specials, a daily show's dates, copies, and every file
 * that cannot be placed kept with its reason), 4.x (a series split across
 * two disks, a disk offline), 5.x (managing a series links its files; bulk
 * add), 6.x (nothing searched, grabbed or written for a series on disk; the
 * scheduler sees none; the tree is unchanged), 7.x (a season is fetched
 * once).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-identify.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-tvdb.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gchar                *disk2;
	gint64                volume_id;
	gint64                volume2;
	gint64                root_id;
	gint64                root2;
	gchar                *series_dir;
	gchar                *more_dir;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisTestTvdb         *tvdb;	/* without its key until a test sets it */
	AxisMeta             *meta;
	AxisIndexers         *indexers;
	AxisDownloads        *downloads;
	AxisQualityStore     *quality;
	AxisTv               *tv;
	AxisIdentify         *identify;
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
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	/* A second disk (the scratch directory is one filesystem, which can be
	 * one volume only: axis_test_library_add_volume_beside(); the first is
	 * away while it is added). */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk, "away-one");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	fx->disk2 = axis_test_library_disk(fx->t, "disk2");
	fx->volume2 = axis_test_library_add_volume_beside(fx->t, fx->disk2, "Disk two");
	fx->root2 = axis_test_library_add_root(fx->t, fx->volume2, fx->disk2, "more", AXIS_MEDIA_TV);
	axis_test_library_plug(fx->t, fx->disk, "away-one");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);
	fx->series_dir = g_build_filename(fx->disk, "series", NULL);
	fx->more_dir = g_build_filename(fx->disk2, "more", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, axis_test_identify_provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_QUALITY_MODULE_ID, axis_quality_store_settings(), &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_TV_MODULE_ID, axis_tv_get_module()->settings, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IDENTIFY_MODULE_ID, axis_identify_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
	g_assert_no_error(error);

	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_tv_set_clock(fx->tv, NOW_US);
	axis_tv_set_identify(fx->tv, fx->identify);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_identify_unref(fx->identify);
	axis_tv_unref(fx->tv);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_downloads_unref(fx->downloads);
	axis_indexers_unref(fx->indexers);
	axis_meta_unref(fx->meta);
	axis_quality_store_unref(fx->quality);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_tvdb_free(fx->tvdb);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->disk2);
	g_free(fx->series_dir);
	g_free(fx->more_dir);
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

/* A file of @size bytes (a pattern, not zeros) at @dir/@rel. */
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

/* Scans @root; then each video (by its path relative to the root) is
 * probed to run @minutes. */
static void
scan(
	Fixture *fx,
	gint64   root
){
	g_autoptr(AxisScanResult) result = axis_test_library_scan_expect(fx->t, root, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE);

	g_assert_nonnull(result);
}

static void
probed(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	gint         minutes
){
	axis_test_probe_video(fx->t, root, rel, "matroska", "h264", "1080p", "sdr", "0.1", (gint64)minutes * 60000, "aac",
		FALSE);
}

/* A video of @root at @rel, written and (after the next scan) probed. */
typedef struct
{
	const gchar *rel;
	gint         minutes;
} Video;

static void
make_tree(
	Fixture     *fx,
	const gchar *dir,
	gint64       root,
	const Video *videos
){
	guint i;

	for (i = 0; videos[i].rel != NULL; i++)
		put(dir, videos[i].rel, 1000 + i * 10);
	scan(fx, root);
	for (i = 0; videos[i].rel != NULL; i++) {
		if (videos[i].minutes > 0)
			probed(fx, root, videos[i].rel, videos[i].minutes);
	}
}

static AxisIdentifyReport
run(
	Fixture *fx,
	gint64   root
){
	AxisIdentifyReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_identify_run_root(fx->identify, root, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

/* Runs until nothing is left: no folder and no season. */
static void
run_all(
	Fixture *fx,
	gint64   root
){
	guint i;

	for (i = 0; i < 20; i++) {
		AxisIdentifyReport report = run(fx, root);

		g_assert_null(report.skipped);
		g_assert_false(report.provider_down);
		if (report.pending == 0 && report.seasons_pending == 0)
			return;
	}
	g_error("identification did not finish");
}

/* The unit at @rel of @root: "state confidence title-label how". */
static void
assert_unit(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	const gchar *expected
){
	g_autofree gchar *got = q(fx, "SELECT i.state || ' ' || coalesce(i.confidence, '-') || ' ' ||"
		" coalesce(t.title || ' (' || t.year || ')', '-') || ' ' || coalesce(i.linked_how, '-')"
		" FROM library_identity i LEFT JOIN meta_title t ON t.id = i.title_id WHERE i.root_id = %" G_GINT64_FORMAT
		" AND i.rel_path = '%s'", root, rel);

	if (g_strcmp0(got, expected) != 0) {
		g_autofree gchar *reasons = q(fx, "SELECT reasons::text || ' | ' || coalesce(read_text, '') FROM"
			" library_identity WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", root, rel);

		g_error("%s: expected '%s', got '%s' (%s)", rel, expected, got != NULL ? got : "no unit",
			reasons != NULL ? reasons : "");
	}
}

static gint64
series_title(
	Fixture     *fx,
	const gchar *provider_id
){
	g_autofree gchar *id = q(fx, "SELECT id FROM meta_title WHERE kind = 'series' AND provider_id = '%s'",
		provider_id);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/* What the series' page reads, for @provider_id. */
static AxisTvDiskDetail *
detail_of(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisTvDiskDetail *detail = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_tv_get_disk_detail(fx->tv, series_title(fx, provider_id), &detail, &error));
	g_assert_no_error(error);
	g_assert_nonnull(detail);
	return detail;
}

static const AxisTvDiskEpisode *
episode_of(
	const AxisTvDiskDetail *detail,
	gint                    season,
	gint                    number
){
	guint i;

	for (i = 0; i < detail->episodes->len; i++) {
		const AxisTvDiskEpisode *e = g_ptr_array_index(detail->episodes, i);

		if (e->season == season && e->number == number)
			return e;
	}
	g_error("no episode S%02dE%02d", season, number);
	return NULL;
}

/* The reason a file whose path ends in @tail could not be placed. */
static const gchar *
unplaced(
	const AxisTvDiskDetail *detail,
	const gchar            *tail
){
	guint i;

	for (i = 0; i < detail->unplaced->len; i++) {
		const AxisTvDiskFile *f = g_ptr_array_index(detail->unplaced, i);

		if (g_str_has_suffix(f->rel_path, tail))
			return f->reason;
	}
	g_error("%s is not among the files that could not be placed", tail);
	return NULL;
}

static void
assert_has(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected '%s' in '%s'", needle, haystack != NULL ? haystack : "(null)");
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* Every name under @dir with its size, mode and times. */
static void
digest_dir(
	const gchar *dir,
	const gchar *prefix,
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
		const gchar *entry = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(dir, entry, NULL);
		g_autofree gchar *rel = g_strconcat(prefix, "/", entry, NULL);
		struct stat st;

		g_assert_cmpint(lstat(path, &st), ==, 0);
		g_string_append_printf(out, "%s mode=%o size=%" G_GINT64_FORMAT " mtime=%" G_GINT64_FORMAT ".%ld\n", rel,
			(guint)st.st_mode, (gint64)st.st_size, (gint64)st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
		if (S_ISDIR(st.st_mode))
			digest_dir(path, rel, out);
	}
}

static gchar *
digest(Fixture *fx)
{
	GString *out = g_string_new(NULL);

	digest_dir(fx->disk, "", out);
	digest_dir(fx->disk2, "", out);
	return g_string_free(out, FALSE);
}

/* ── The trees ──────────────────────────────────────────────────── */

/* Lantern Row (2015), named as Sonarr names: a copy of one episode, a
 * double episode, a special, and files that cannot be placed; a subtitle
 * and an extra beside them. */
static const Video lantern[] = {
	{ "Lantern Row (2015)/Season 01/Lantern Row (2015) - S01E01 - Chapter 1 [WEBDL-1080p].mkv", 45 },
	{ "Lantern Row (2015)/Season 01/Lantern Row (2015) - S01E02 - Chapter 2 [WEBDL-1080p].mkv", 45 },
	{ "Lantern Row (2015)/Season 01/Lantern Row (2015) - S01E02 - Chapter 2 [HDTV-720p].mkv", 45 },
	{ "Lantern Row (2015)/Season 01/Lantern Row (2015) - S02E01 - Misfiled.mkv", 45 },
	{ "Lantern Row (2015)/Season 01/Quiet Lake - S01E03.mkv", 45 },
	{ "Lantern Row (2015)/Season 02/Lantern Row (2015) - S02E02E03 - Chapters.mkv", 90 },
	{ "Lantern Row (2015)/Season 02/Lantern Row (2015) - S02E09 - Nowhere.mkv", 45 },
	{ "Lantern Row (2015)/Specials/Lantern Row (2015) - S00E01 - Winter Special.mkv", 45 },
	{ "Lantern Row (2015)/Lantern Row (2015) - Pilot Party.mkv", 45 },
	{ "Lantern Row (2015)/Extras/Lantern Row - Behind the Lamps.mkv", 10 },
	{ NULL, 0 }
};

/* The other forms: no year (two series of the name), identifiers in the
 * folders' names, episodes in the series' folder itself, an absolute
 * number, a daily show, and an episode file at the top of the root. */
static const Video others[] = {
	{ "Copper Valley/S01/Copper.Valley.S01E01.720p.HDTV.x264-GRP.mkv", 30 },
	{ "Harrow Fields {tvdb-81005}/Season 1/Harrow Fields - 1x01.mkv", 60 },
	{ "Saltmarsh Signals [imdbid-tt7000007]/S01E01.mkv", 50 },
	{ "Glass Harbor (2018)/Glass Harbor - S01E01 [1080p].mkv", 24 },
	{ "Glass Harbor (2018)/Glass Harbor - 05 [1080p].mkv", 24 },
	{ "The Night Ledger (2024)/The.Night.Ledger.2024.03.11.Guest.Name.720p.WEB.h264-GRP.mkv", 40 },
	{ "The Night Ledger (2024)/The.Night.Ledger.2024.03.16.Two.Guests.720p.WEB.h264-GRP.mkv", 40 },
	{ "Copper.Valley.2019.S01E02.720p.HDTV.x264-GRP.mkv", 30 },
	{ NULL, 0 }
};

/* ── The tests ──────────────────────────────────────────────────── */

/*
 * Spec (library-identification): Series in the library are matched to
 * titles with stated evidence; (tv) Episode files are placed on the
 * series' episodes, never by guessing.
 */
static void
test_series(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTvDiskDetail) lantern_page = NULL;
	g_autoptr(AxisTvDiskDetail) ledger = NULL;
	g_autoptr(AxisTvDiskDetail) glass = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	const AxisTvDiskEpisode *e;
	AxisIdentifyReport report;
	guint seasons;

	{
		Video all[G_N_ELEMENTS(lantern) + G_N_ELEMENTS(others)];
		guint n = 0;
		guint i;

		for (i = 0; lantern[i].rel != NULL; i++)
			all[n++] = lantern[i];
		for (i = 0; others[i].rel != NULL; i++)
			all[n++] = others[i];
		all[n].rel = NULL;
		all[n].minutes = 0;
		put(fx->series_dir, "Lantern Row (2015)/Season 01/Lantern Row (2015) - S01E01 - Chapter 1 [WEBDL-1080p].en.srt",
			40);
		make_tree(fx, fx->series_dir, fx->root_id, all);
	}
	before = digest(fx);
	run_all(fx, fx->root_id);

	/* Folders and a file matched by the films' rules. */
	assert_unit(fx, fx->root_id, "Lantern Row (2015)", "linked sure Lantern Row (2015) auto");
	assert_unit(fx, fx->root_id, "Copper Valley", "review likely - -");
	assert_unit(fx, fx->root_id, "Harrow Fields {tvdb-81005}", "linked sure Harrow Fields (2010) auto");
	assert_unit(fx, fx->root_id, "Saltmarsh Signals [imdbid-tt7000007]", "linked sure Saltmarsh Signals (2022) auto");
	assert_unit(fx, fx->root_id, "Glass Harbor (2018)", "linked sure Glass Harbor (2018) auto");
	assert_unit(fx, fx->root_id, "The Night Ledger (2024)", "linked sure The Night Ledger (2024) auto");
	assert_unit(fx, fx->root_id, "Copper.Valley.2019.S01E02.720p.HDTV.x264-GRP.mkv",
		"linked sure Copper Valley (2019) auto");
	/* The runtime checked is the episode's; with no year and one episode on
	 * disk, both series of the name are candidates and nothing is linked. */
	{
		g_autofree gchar *lantern_reasons = q(fx, "SELECT reasons::text FROM library_identity WHERE rel_path ="
			" 'Lantern Row (2015)'");
		g_autofree gchar *copper = q(fx, "SELECT jsonb_array_length(candidates) || ' ' || reasons::text FROM"
			" library_identity WHERE rel_path = 'Copper Valley'");

		assert_has(lantern_reasons, "for the episode");
		assert_has(copper, "2 series have that name");
		assert_has(copper, "only 1 numbered episode is on disk");
	}
	/* The subtitle and the extra are not episodes. */
	assert_q(fx, "0", "SELECT count(*) FROM library_identity_file lif JOIN library_file f ON f.id = lif.file_id"
		" WHERE f.rel_path LIKE '%.srt' OR f.rel_path LIKE '%Extras%'");

	/* Lantern Row: what is on disk, against its data. */
	lantern_page = detail_of(fx, "7001");
	g_assert_nonnull(lantern_page->facts);
	g_assert_cmpuint(lantern_page->facts->episodes, ==, 6);
	g_assert_cmpuint(lantern_page->facts->on_disk, ==, 4);
	g_assert_cmpuint(lantern_page->facts->specials, ==, 1);
	g_assert_cmpuint(lantern_page->facts->copies, ==, 1);
	g_assert_cmpuint(lantern_page->facts->unplaced, ==, 4);
	g_assert_cmpuint(lantern_page->facts->n_files, ==, 9);
	g_assert_cmpuint(lantern_page->facts->seasons_unknown, ==, 0);
	e = episode_of(lantern_page, 1, 1);
	g_assert_cmpuint(e->files->len, ==, 1);
	e = episode_of(lantern_page, 1, 2);
	g_assert_cmpuint(e->files->len, ==, 2);
	e = episode_of(lantern_page, 1, 3);
	g_assert_cmpuint(e->files->len, ==, 0);
	e = episode_of(lantern_page, 2, 1);
	g_assert_cmpuint(e->files->len, ==, 0);
	g_assert_cmpuint(episode_of(lantern_page, 2, 2)->files->len, ==, 1);
	g_assert_cmpuint(episode_of(lantern_page, 2, 3)->files->len, ==, 1);
	g_assert_true(g_ptr_array_index(episode_of(lantern_page, 2, 2)->files, 0) ==
		g_ptr_array_index(episode_of(lantern_page, 2, 3)->files, 0));
	g_assert_cmpuint(episode_of(lantern_page, 0, 1)->files->len, ==, 1);
	/* Each file that holds no episode, with its reason. */
	assert_has(unplaced(lantern_page, "S02E01 - Misfiled.mkv"), "in the folder of season 1");
	assert_has(unplaced(lantern_page, "Quiet Lake - S01E03.mkv"), "\"Quiet Lake\", which is not one of this series'");
	assert_has(unplaced(lantern_page, "S02E09 - Nowhere.mkv"), "numbered S02E09, and there is no such episode");
	assert_has(unplaced(lantern_page, "Pilot Party.mkv"), "no season and episode");
	g_assert_cmpuint(lantern_page->folders->len, ==, 1);

	/* A daily show: one date is one episode, a day two aired is held. */
	ledger = detail_of(fx, "7004");
	g_assert_cmpuint(episode_of(ledger, 1, 1)->files->len, ==, 1);
	assert_has(unplaced(ledger, "2024.03.16.Two.Guests.720p.WEB.h264-GRP.mkv"), "2 episodes of the series aired on "
		"2024-03-16");
	/* An absolute number alone is held, never mapped. */
	glass = detail_of(fx, "7006");
	g_assert_cmpuint(episode_of(glass, 1, 1)->files->len, ==, 1);
	assert_has(unplaced(glass, "Glass Harbor - 05 [1080p].mkv"), "an absolute episode number (5)");

	/* Each season a linked series needs was fetched, once: a second run
	 * with nothing changed asks the provider for nothing. */
	assert_q(fx, "0", "SELECT count(*) FROM meta_season ms WHERE ms.episodes_fetched_at IS NULL AND ms.title_id IN"
		" (SELECT title_id FROM library_identity WHERE state = 'linked')");
	seasons = axis_test_identify_season_requests();
	report = run(fx, fx->root_id);
	g_assert_cmpuint(report.examined, ==, 0);
	g_assert_cmpuint(report.seasons, ==, 0);
	g_assert_cmpuint(axis_test_identify_season_requests(), ==, seasons);

	/* Nothing on disk changed, and nothing was made for the series. */
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
	assert_q(fx, "0", "SELECT count(*) FROM series");
	assert_q(fx, "0", "SELECT count(*) FROM series_file");
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind LIKE 'tv.%'");
}

/*
 * Spec (tv): A series split across disks is one series; a disk away never
 * takes it out of the list.
 */
static void
test_split_and_offline(Fixture *fx, gconstpointer data)
{
	static const Video one[] = {
		{ "Harrow Fields (2010)/Season 1/Harrow Fields (2010) - S01E01.mkv", 60 },
		{ "Harrow Fields (2010)/Season 1/Harrow Fields (2010) - S01E02.mkv", 60 },
		{ NULL, 0 }
	};
	static const Video two[] = {
		{ "Harrow Fields (2010)/Season 2/Harrow Fields (2010) - S02E01.mkv", 60 },
		{ "Harrow Fields (2010)/Season 2/Harrow Fields (2010) - S02E02.mkv", 60 },
		{ "Harrow Fields (2010)/Season 1/Harrow Fields (2010) - S01E02.mkv", 60 },
		{ NULL, 0 }
	};
	g_autoptr(AxisTvDiskDetail) detail = NULL;
	g_autoptr(AxisTvDiskDetail) away = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) error = NULL;
	gint64 title;

	make_tree(fx, fx->series_dir, fx->root_id, one);
	make_tree(fx, fx->more_dir, fx->root2, two);
	run_all(fx, fx->root_id);
	run_all(fx, fx->root2);
	assert_unit(fx, fx->root_id, "Harrow Fields (2010)", "linked sure Harrow Fields (2010) auto");
	assert_unit(fx, fx->root2, "Harrow Fields (2010)", "linked sure Harrow Fields (2010) auto");

	/* One series, two folders; S01E02 on both disks is two copies. */
	detail = detail_of(fx, "7005");
	g_assert_nonnull(detail->facts);
	g_assert_cmpuint(detail->facts->n_folders, ==, 2);
	g_assert_cmpuint(detail->facts->on_disk, ==, 4);
	g_assert_cmpuint(detail->facts->episodes, ==, 4);
	g_assert_cmpuint(detail->facts->copies, ==, 1);
	g_assert_cmpuint(episode_of(detail, 1, 2)->files->len, ==, 2);
	g_assert_cmpuint(detail->folders->len, ==, 2);

	/* Disk two goes away: the series stays, with everything on it, and says
	 * that disk is offline. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_library_unplug(fx->t, fx->disk2, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume2, NULL, &error));
	g_assert_no_error(error);
	{
		AxisScanResult *offline = axis_test_library_scan_expect(fx->t, fx->root2, AXIS_SCAN_UNAVAILABLE,
			AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);

		axis_scan_result_free(offline);
	}
	away = detail_of(fx, "7005");
	g_assert_nonnull(away->facts);
	g_assert_cmpuint(away->facts->on_disk, ==, 4);
	g_assert_cmpuint(away->facts->n_away, ==, 3);
	g_assert_cmpstr(away->facts->away_volume, ==, "Disk two");
	title = series_title(fx, "7005");
	g_array_append_val(ids, title);
	g_assert_true(axis_tv_get_on_disk_by_titles(fx->tv, ids, &listed, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(listed->len, ==, 1);
	axis_test_library_plug(fx->t, fx->disk2, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume2, NULL, &error));
	g_assert_no_error(error);
}

/* The state of the managed series' episode S@season E@number. */
static AxisTvState
episode_state(
	Fixture *fx,
	gint64   series_id,
	gint     season,
	gint     number
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
			return axis_tv_episode_derive(e, series, &rules, NULL);
	}
	g_error("no episode S%02dE%02d", season, number);
	return AXIS_TV_WANTED;
}

/*
 * Spec (tv): A series on disk can be managed; its episodes on disk count
 * as present, only missing monitored episodes are wanted, nothing is
 * searched for on adding; the bulk action manages every identified series.
 */
static void
test_managed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvDiskDetail) managed = NULL;
	AxisTvSpec spec;
	gint64 series_id = 0;
	guint added = 0;
	guint skipped = 0;

	make_tree(fx, fx->series_dir, fx->root_id, lantern);
	{
		static const Video more[] = {
			{ "Harrow Fields (2010)/Season 1/Harrow Fields (2010) - S01E01.mkv", 60 },
			{ "Saltmarsh Signals (2022)/Season 1/Saltmarsh Signals (2022) - S01E01.mkv", 50 },
			{ NULL, 0 }
		};

		make_tree(fx, fx->series_dir, fx->root_id, more);
	}
	run_all(fx, fx->root_id);

	/* Added through search (all episodes monitored, searching nothing now):
	 * the episodes on disk are in the library at once. */
	memset(&spec, 0, sizeof(spec));
	spec.title_id = series_title(fx, "7001");
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_ALL;
	spec.user = "alice";
	g_assert_true(axis_tv_add(fx->tv, &spec, &series_id, &error));
	g_assert_no_error(error);
	/* In the library (below the default profile's cutoff, an upgrade is
	 * wanted, as for a film: the profile decides), never wanted. */
	g_assert_cmpint(episode_state(fx, series_id, 1, 1), ==, AXIS_TV_UPGRADE_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 1, 2), ==, AXIS_TV_UPGRADE_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 2), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 3), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 0, 1), ==, AXIS_TV_IN_LIBRARY);
	g_assert_cmpint(episode_state(fx, series_id, 1, 3), ==, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 1), ==, AXIS_TV_WANTED);
	assert_q(fx, "5", "SELECT count(*) FROM series_file WHERE identity_id IS NOT NULL");
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	{
		g_autofree gchar *added_text = q(fx, "SELECT text FROM series_event WHERE kind = 'added'");

		assert_has(added_text, "5 episode files were on disk already");
	}
	/* It is no longer a series on disk; its page still lists what could not
	 * be placed. */
	managed = detail_of(fx, "7001");
	g_assert_null(managed->facts);
	g_assert_cmpuint(managed->unplaced->len, ==, 4);

	/* Removed: no file is touched, and it is a series on disk again. */
	g_assert_true(axis_tv_remove(fx->tv, series_id, "alice", &error));
	g_assert_no_error(error);
	assert_q(fx, "0", "SELECT count(*) FROM series_file");
	{
		g_autoptr(AxisTvDiskDetail) again = detail_of(fx, "7001");

		g_assert_nonnull(again->facts);
	}

	/* Every identified series of the root at once, future episodes only:
	 * each is managed with its files, and none is searched for. */
	g_assert_true(axis_identify_add_all(fx->identify, fx->root_id, 0, TRUE, "alice", &added, &skipped, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(added, ==, 3);
	g_assert_cmpuint(skipped, ==, 0);
	assert_q(fx, "3", "SELECT count(*) FROM series WHERE monitor_choice = 'future' AND added_by = 'alice'");
	assert_q(fx, "0", "SELECT count(*) FROM series_episode e JOIN series s ON s.id = e.series_id WHERE e.monitored");
	assert_q(fx, "7", "SELECT count(*) FROM series_file WHERE identity_id IS NOT NULL");
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind = 'tv.search'");
	{
		g_autofree gchar *id = q(fx, "SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT,
			series_title(fx, "7005"));

		g_assert_cmpint(episode_state(fx, g_ascii_strtoll(id, NULL, 10), 1, 1), ==, AXIS_TV_IN_LIBRARY);
		g_assert_cmpint(episode_state(fx, g_ascii_strtoll(id, NULL, 10), 1, 2), ==, AXIS_TV_UNMONITORED);
	}
}

/*
 * Spec (tv): Nothing is asked of a series on disk: the scheduler runs with
 * series on disk and considers none, queues nothing, writes nothing.
 */
static void
test_nothing_asked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisTvScheduleReport report;
	AxisAcquireFollowReport follow;

	make_tree(fx, fx->series_dir, fx->root_id, lantern);
	run_all(fx, fx->root_id);
	before = digest(fx);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_schedule(fx->tv, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.considered, ==, 0);
	g_assert_cmpuint(report.queued, ==, 0);
	g_assert_cmpuint(report.synced, ==, 0);
	memset(&follow, 0, sizeof(follow));
	g_assert_true(axis_tv_run_follow(fx->tv, &follow, &error));
	g_assert_no_error(error);
	assert_q(fx, "0", "SELECT count(*) FROM series");
	assert_q(fx, "0", "SELECT count(*) FROM series_episode");
	assert_q(fx, "0", "SELECT count(*) FROM series_search");
	assert_q(fx, "0", "SELECT count(*) FROM series_grab");
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind LIKE 'tv.%'");
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
}

/*
 * Spec (library-identification): A person's link of a series' folder asks
 * for its root's run, which fetches the seasons the series needs.
 */
static void
test_review(Fixture *fx, gconstpointer data)
{
	static const Video copper[] = {
		{ "Copper Valley/S01/Copper.Valley.S01E03.720p.HDTV.x264-GRP.mkv", 30 },
		{ NULL, 0 }
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvDiskDetail) detail = NULL;
	g_autofree gchar *unit = NULL;
	g_autofree gchar *film = NULL;
	gint64 title;

	make_tree(fx, fx->series_dir, fx->root_id, copper);
	run_all(fx, fx->root_id);
	assert_unit(fx, fx->root_id, "Copper Valley", "review likely - -");
	unit = q(fx, "SELECT id FROM library_identity WHERE rel_path = 'Copper Valley'");

	/* A film is not a series: a person cannot link a series' folder to one. */
	film = q(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Copper Valley',"
		" 2019, 'fixture', '9901') RETURNING id");
	g_assert_false(axis_identify_act(fx->identify, g_ascii_strtoll(unit, NULL, 10), AXIS_IDENTIFY_CHOOSE,
		g_ascii_strtoll(film, NULL, 10), "alice", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	assert_unit(fx, fx->root_id, "Copper Valley", "review likely - -");

	/* The series: linked, and its root's run is asked for. */
	title = series_title(fx, "7002");
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(unit, NULL, 10), AXIS_IDENTIFY_CHOOSE, title, "alice",
		&error));
	g_assert_no_error(error);
	assert_q(fx, "1", "SELECT count(*) FROM job WHERE kind = 'identify.root'");
	run_all(fx, fx->root_id);
	detail = detail_of(fx, "7002");
	g_assert_nonnull(detail->facts);
	g_assert_cmpuint(detail->facts->on_disk, ==, 1);
	g_assert_cmpuint(episode_of(detail, 1, 3)->files->len, ==, 1);
}

/* Series' folders named without a year, as series' folders usually are. */
static const Video no_year[] = {
	/* One series of the name, and the episodes fit it. */
	{ "Harrow Fields/Season 1/Harrow Fields - S01E01.mkv", 60 },
	{ "Harrow Fields/Season 1/Harrow Fields - S01E02.mkv", 60 },
	{ "Harrow Fields/Season 2/Harrow Fields - S02E01.mkv", 60 },
	{ "Harrow Fields/Season 2/Harrow Fields - S02E02.mkv", 60 },
	/* A season the series does not have. */
	{ "Saltmarsh Signals/Season 1/Saltmarsh Signals - S01E01.mkv", 50 },
	{ "Saltmarsh Signals/Season 1/Saltmarsh Signals - S01E02.mkv", 50 },
	{ "Saltmarsh Signals/Season 1/Saltmarsh Signals - S01E03.mkv", 50 },
	{ "Saltmarsh Signals/Season 3/Saltmarsh Signals - S03E01.mkv", 50 },
	/* Two of one name; the episodes fit one (the other has two). */
	{ "Copper Valley/Season 1/Copper Valley - S01E01.mkv", 30 },
	{ "Copper Valley/Season 1/Copper Valley - S01E02.mkv", 30 },
	{ "Copper Valley/Season 1/Copper Valley - S01E03.mkv", 30 },
	{ "Copper Valley/Season 1/Copper Valley - S01E04.mkv", 30 },
	/* Two of one name that both fit. */
	{ "Brass Meridian/Season 1/Brass Meridian - S01E01.mkv", 45 },
	{ "Brass Meridian/Season 1/Brass Meridian - S01E02.mkv", 45 },
	{ "Brass Meridian/Season 1/Brass Meridian - S01E03.mkv", 45 },
	{ "Brass Meridian/Season 1/Brass Meridian - S01E04.mkv", 45 },
	{ "Brass Meridian/Season 1/Brass Meridian - S01E05.mkv", 45 },
	/* The runtime contradicts (the series' episodes run 24 minutes). */
	{ "Glass Harbor/Glass Harbor - S01E01.mkv", 60 },
	{ "Glass Harbor/Glass Harbor - S01E02.mkv", 60 },
	{ "Glass Harbor/Glass Harbor - S01E03.mkv", 60 },
	{ "Glass Harbor/Glass Harbor - S01E04.mkv", 60 },
	/* No file with numbers. */
	{ "Lantern Row/Lantern Row - Pilot Party.mkv", 45 },
	{ "Lantern Row/Lantern Row - Holiday.mkv", 45 },
	{ "Lantern Row/Lantern Row - Reunion.mkv", 45 },
	/* One of the series' other names. */
	{ "Kestrel Bay/Season 1/Kestrel Bay - S01E01.mkv", 50 },
	{ "Kestrel Bay/Season 1/Kestrel Bay - S01E02.mkv", 50 },
	{ "Kestrel Bay/Season 1/Kestrel Bay - S01E03.mkv", 50 },
	/* A name that matches a series' only loosely. */
	{ "Lantern/Season 1/Lantern - S01E01.mkv", 45 },
	{ "Lantern/Season 1/Lantern - S01E02.mkv", 45 },
	{ "Lantern/Season 1/Lantern - S01E03.mkv", 45 },
	/* No runtime given by the provider. */
	{ "Juniper Hollow/Season 1/Juniper Hollow - S01E01.mkv", 40 },
	{ "Juniper Hollow/Season 1/Juniper Hollow - S01E02.mkv", 40 },
	{ "Juniper Hollow/Season 1/Juniper Hollow - S01E03.mkv", 40 },
	{ "Juniper Hollow/Season 1/Juniper Hollow - S01E04.mkv", 40 },
	{ NULL, 0 }
};

static gchar *
reasons_of(
	Fixture     *fx,
	const gchar *rel
){
	return q(fx, "SELECT reasons::text FROM library_identity WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'",
		fx->root_id, rel);
}

/*
 * Spec (library-identification): A series' folder without a year is linked
 * by its episodes.  One series of the name whose seasons the episodes fit
 * is linked by itself, under its title or one of its other names, with or
 * without a runtime to check; of two of one name the one they fit is; a
 * season the series lacks, two that both fit, a runtime that contradicts,
 * no numbered file, or a name that only looks like a series' hold the
 * folder, each with its reason.
 */
static void
test_no_year(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *salt = NULL;
	g_autofree gchar *brass = NULL;
	g_autofree gchar *glass = NULL;
	g_autofree gchar *unnumbered = NULL;
	g_autofree gchar *kestrel = NULL;
	g_autofree gchar *copper = NULL;
	g_autofree gchar *juniper = NULL;
	g_autoptr(AxisTvDiskDetail) detail = NULL;

	make_tree(fx, fx->series_dir, fx->root_id, no_year);
	run_all(fx, fx->root_id);

	assert_unit(fx, fx->root_id, "Harrow Fields", "linked sure Harrow Fields (2010) auto");
	assert_unit(fx, fx->root_id, "Copper Valley", "linked sure Copper Valley (2019) auto");
	assert_unit(fx, fx->root_id, "Kestrel Bay", "linked sure Kestrel Point (2016) auto");
	assert_unit(fx, fx->root_id, "Juniper Hollow", "linked sure Juniper Hollow (2012) auto");
	assert_unit(fx, fx->root_id, "Saltmarsh Signals", "review likely - -");
	assert_unit(fx, fx->root_id, "Brass Meridian", "review likely - -");
	assert_unit(fx, fx->root_id, "Glass Harbor", "review likely - -");
	assert_unit(fx, fx->root_id, "Lantern Row", "review likely - -");
	assert_unit(fx, fx->root_id, "Lantern", "review unknown - -");

	/* Each with what was compared, in words. */
	copper = reasons_of(fx, "Copper Valley");
	assert_has(copper, "Copper Valley (2019) fits: 4 of the 4 numbered episodes on disk are its episodes");
	assert_has(copper, "2 of the 4 numbered episodes on disk are not episodes of Copper Valley (2004)");
	assert_has(copper, "Copper Valley (2019) is the only series of that name that the episodes on disk fit");
	kestrel = reasons_of(fx, "Kestrel Bay");
	assert_has(kestrel, "it is one of its other names");
	juniper = reasons_of(fx, "Juniper Hollow");
	assert_has(juniper, "the details provider gives no runtime for its episodes");
	salt = reasons_of(fx, "Saltmarsh Signals");
	assert_has(salt, "Saltmarsh Signals (2022) has no season 3");
	brass = reasons_of(fx, "Brass Meridian");
	assert_has(brass, "2 series of that name fit the episodes on disk (Brass Meridian (2008) and Brass Meridian (2021))");
	assert_q(fx, "2", "SELECT jsonb_array_length(candidates) FROM library_identity WHERE rel_path = 'Brass Meridian'");
	glass = reasons_of(fx, "Glass Harbor");
	assert_has(glass, "its episode runs 24 minutes and the main video 60: 36 minutes off");
	unnumbered = reasons_of(fx, "Lantern Row");
	assert_has(unnumbered, "no file on disk is a numbered episode");
	assert_has(unnumbered, "its files hold no numbered episode; 3 files hold no episode");

	/* Linked by itself, a series' files are placed as any other's. */
	detail = detail_of(fx, "7005");
	g_assert_nonnull(detail->facts);
	g_assert_cmpuint(detail->facts->on_disk, ==, 4);
}

/*
 * Spec (library-identification): Folders held by the earlier rule are looked
 * at again.  A series' folder held for its missing year by an earlier
 * release is queued and linked by itself with nothing changed on disk; one a
 * person decided stays as the person left it; each is looked at once.
 */
static void
test_reconsider(Fixture *fx, gconstpointer data)
{
	static const Video tree[] = {
		{ "Harrow Fields/Season 1/Harrow Fields - S01E01.mkv", 60 },
		{ "Harrow Fields/Season 1/Harrow Fields - S01E02.mkv", 60 },
		{ "Harrow Fields/Season 2/Harrow Fields - S02E01.mkv", 60 },
		{ "Juniper Hollow/Season 1/Juniper Hollow - S01E01.mkv", 40 },
		{ "Juniper Hollow/Season 1/Juniper Hollow - S01E02.mkv", 40 },
		{ "Juniper Hollow/Season 1/Juniper Hollow - S01E03.mkv", 40 },
		{ "Copper Valley/S01/Copper.Valley.S01E01.720p.HDTV.x264-GRP.mkv", 30 },
		{ NULL, 0 }
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *juniper = NULL;
	g_autofree gchar *copper = NULL;
	AxisIdentifyReport report;
	guint queued = 99;

	make_tree(fx, fx->series_dir, fx->root_id, tree);
	run_all(fx, fx->root_id);
	assert_unit(fx, fx->root_id, "Harrow Fields", "linked sure Harrow Fields (2010) auto");
	/* A person chose for Copper Valley, and undid Juniper Hollow's link. */
	copper = q(fx, "SELECT id FROM library_identity WHERE rel_path = 'Copper Valley'");
	juniper = q(fx, "SELECT id FROM library_identity WHERE rel_path = 'Juniper Hollow'");
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(copper, NULL, 10), AXIS_IDENTIFY_CHOOSE,
		series_title(fx, "7003"), "alice", &error));
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(juniper, NULL, 10), AXIS_IDENTIFY_UNDO, 0, "alice",
		&error));
	g_assert_no_error(error);
	run_all(fx, fx->root_id);

	/* As an earlier release left them (rules of version 1): Harrow Fields
	 * held for its missing year, and no verdict saying what the files
	 * hold. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'review', confidence = 'likely', title_id = NULL,"
		" linked_how = NULL, reasons = '[{\"c\": \"names\", \"t\": \"title from the folder and the file, which agree: "
		"Harrow Fields (2010)\"}, {\"c\": \"no_year\", \"t\": \"the names give no year\"}]', rules = 1 WHERE rel_path ="
		" 'Harrow Fields'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET reasons = '[{\"c\": \"no_year\", \"t\": \"the names give"
		" no year\"}]', rules = 1 WHERE rel_path IN ('Copper Valley', 'Juniper Hollow')");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");

	/* Nothing changed on disk, and the scheduler queues the root. */
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 1);
	report = run(fx, fx->root_id);
	g_assert_cmpuint(report.examined, ==, 1);
	g_assert_cmpuint(report.linked, ==, 1);
	assert_unit(fx, fx->root_id, "Harrow Fields", "linked sure Harrow Fields (2010) auto");
	/* The person's decisions stand. */
	assert_q(fx, "linked chosen Copper Valley 2004", "SELECT i.state || ' ' || i.linked_how || ' ' || t.title || ' ' ||"
		" t.year FROM library_identity i JOIN meta_title t ON t.id = i.title_id WHERE i.rel_path = 'Copper Valley'");
	assert_q(fx, "review true", "SELECT state || ' ' || held FROM library_identity WHERE rel_path = 'Juniper Hollow'");

	/* Once: nothing more to look at, nothing queued. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 0);
	report = run(fx, fx->root_id);
	g_assert_cmpuint(report.examined, ==, 0);
}

/*
 * Spec (library-identification): Identification is paced.  A root whose
 * last run left folders is queued again by the minute's schedule, not ten
 * minutes later; a root with nothing left, or waiting for the provider, is
 * not.
 */
static void
test_continue(Fixture *fx, gconstpointer data)
{
	static const Video tree[] = {
		{ "Harrow Fields/Season 1/Harrow Fields - S01E01.mkv", 60 },
		{ "Juniper Hollow/Season 1/Juniper Hollow - S01E01.mkv", 40 },
		{ "Kestrel Bay/Season 1/Kestrel Bay - S01E01.mkv", 50 },
		{ NULL, 0 }
	};
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;
	guint queued = 99;

	g_assert_true(axis_settings_set(fx->settings, AXIS_IDENTIFY_SETTING_PER_RUN, "2", &error));
	g_assert_no_error(error);
	make_tree(fx, fx->series_dir, fx->root_id, tree);
	report = run(fx, fx->root_id);
	g_assert_cmpuint(report.examined, ==, 2);
	g_assert_cmpuint(report.pending, ==, 1);

	g_assert_true(axis_identify_run_continue(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 1);
	/* Queued already: not twice. */
	g_assert_true(axis_identify_run_continue(fx->identify, &queued, &error));
	g_assert_cmpuint(queued, ==, 0);

	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	run_all(fx, fx->root_id);
	g_assert_true(axis_identify_run_continue(fx->identify, &queued, &error));
	g_assert_cmpuint(queued, ==, 0);

	/* Waiting for the provider: the ten minutes' schedule takes it again. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET pending = 3, retry_after = now() + interval '15"
		" minutes'");
	g_assert_true(axis_identify_run_continue(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 0);
}

/* ── A managed series on disk changes its numbering (add-metadata-tvdb) ── */

/* Glass Harbor (7006, TheTVDB 81006): TMDb (the fixture) numbers its twelve
 * chapters as one season; TheTVDB's aired order (made up) as two of six, its
 * season 1 chapters 1, 2, 7, 8, 9 and 10, its season 2 chapters 3, 4, 5, 6,
 * 11 and 12.  So a file named S01E03 (chapter 3) is TheTVDB's S02E01, and
 * TheTVDB's S01E03 is chapter 7. */
static const gint glass_order[12][2] = {
	{ 1, 1 }, { 1, 2 }, { 2, 1 }, { 2, 2 }, { 2, 3 }, { 2, 4 }, { 1, 3 }, { 1, 4 }, { 1, 5 }, { 1, 6 }, { 2, 5 },
	{ 2, 6 }
};

static const Video glass[] = {
	{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E01.mkv", 24 },
	{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E03.mkv", 24 },
	{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E05-E06.mkv", 48 },
	{ NULL, 0 }
};

/* Glass Harbor identified, managed and following TMDb; TheTVDB keyed, its
 * order answered from TMDb's chapters.  The series' id. */
static gint64
glass_managed(Fixture *fx)
{
	g_autoptr(GString) order = g_string_new("{\"status\":\"success\",\"data\":{\"series\":{\"id\":81006},\"episodes\":[");
	g_autoptr(GError) error = NULL;
	AxisTvSpec spec;
	gint64 series_id = 0;
	guint i;

	make_tree(fx, fx->series_dir, fx->root_id, glass);
	run_all(fx, fx->root_id);
	memset(&spec, 0, sizeof(spec));
	spec.title_id = series_title(fx, "7006");
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_ALL;
	spec.user = "alice";
	g_assert_true(axis_tv_add(fx->tv, &spec, &series_id, &error));
	g_assert_no_error(error);
	assert_q(fx, "3", "SELECT count(*) FROM series_file WHERE identity_id IS NOT NULL");
	for (i = 0; i < 12; i++) {
		g_autofree gchar *name = q(fx, "SELECT name FROM meta_episode WHERE title_id = %" G_GINT64_FORMAT " AND source ="
			" 'fixture' AND season_number = 1 AND number = %u", spec.title_id, i + 1);
		g_autofree gchar *aired = q(fx, "SELECT air_date::text FROM meta_episode WHERE title_id = %" G_GINT64_FORMAT
			" AND source = 'fixture' AND season_number = 1 AND number = %u", spec.title_id, i + 1);

		g_assert_nonnull(name);
		g_string_append_printf(order, "%s{\"id\":810060%02u,\"seriesId\":81006,\"name\":\"%s\",\"aired\":\"%s\","
			"\"runtime\":24,\"seasonNumber\":%d,\"number\":%d}", i > 0 ? "," : "", i + 1, name, aired,
			glass_order[i][0], glass_order[i][1]);
	}
	g_string_append(order, "]},\"links\":{\"prev\":null,\"next\":null}}");
	axis_test_tvdb_set_answer(fx->tvdb, "/v4/series/81006/episodes/official/eng", "page=0", order->str);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	return series_id;
}

/* Each file's links, "file>episode", and each linked row's numbering. */
static gchar *
glass_links(
	Fixture *fx,
	gint64   series_id
){
	return q(fx, "SELECT string_agg(f.file_id || '>' || f.episode_id, ',' ORDER BY f.file_id, f.episode_id)"
		" FROM series_file_episode f JOIN series_file sf ON sf.file_id = f.file_id WHERE sf.series_id = %"
		G_GINT64_FORMAT, series_id);
}

static gint64
glass_row(
	Fixture *fx,
	gint64   series_id,
	gint     season,
	gint     number
){
	g_autofree gchar *id = q(fx, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT " AND season_number ="
		" %d AND number = %d AND gone_at IS NULL", series_id, season, number);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static void
glass_change(
	Fixture *fx,
	gint64   series_id
){
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	gboolean changed = TRUE;
	guint i;

	g_assert_true(axis_tv_plan_numbering(fx->tv, series_id, "tvdb", TRUE, &plan, &error));
	g_assert_no_error(error);
	g_assert_null(plan->problem);
	/* The page says each file stays with its episode: so it must. */
	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		g_assert_cmpuint(row->n_files_after, ==, row->n_files);
	}
	g_assert_true(axis_tv_change_numbering(fx->tv, series_id, "tvdb", plan->digest, FALSE, "alice", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_false(changed);
}

/*
 * Review of V3, blocker 1: a file identification placed follows the row it
 * is on (the episode it holds), never its name read again under the new
 * numbering: a single file named S01E03 and a double named S01E05-E06 stay
 * on their episodes, now TheTVDB's S02E01 and S02E03 and S02E04; none of
 * those is wanted, and TheTVDB's S01E03 (chapter 7) gets no file.
 */
static void
test_numbering_files(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 three = glass_row(fx, series_id, 1, 3);
	gint64 five = glass_row(fx, series_id, 1, 5);
	gint64 six = glass_row(fx, series_id, 1, 6);
	gint64 seven = glass_row(fx, series_id, 1, 7);

	before = glass_links(fx, series_id);
	glass_change(fx, series_id);
	after = glass_links(fx, series_id);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(glass_row(fx, series_id, 2, 1), ==, three);
	g_assert_cmpint(glass_row(fx, series_id, 2, 3), ==, five);
	g_assert_cmpint(glass_row(fx, series_id, 2, 4), ==, six);
	g_assert_cmpint(glass_row(fx, series_id, 1, 3), ==, seven);
	g_assert_cmpint(episode_state(fx, series_id, 2, 1), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 3), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 4), !=, AXIS_TV_WANTED);
	assert_q(fx, "0", "SELECT count(*) FROM series_file_episode WHERE episode_id = (SELECT id FROM series_episode"
		" WHERE season_number = 1 AND number = 3 AND gone_at IS NULL)");
}

/*
 * Review of V3, blocker 2: identification looks at the folder again (a new
 * file in it changes its fingerprint): the links of files whose names were
 * read under TMDb's numbering are left as they are, nothing becomes wanted,
 * and only the new file, named under TheTVDB's, is placed by its name.
 */
static void
test_numbering_reidentified(Fixture *fx, gconstpointer data)
{
	static const Video more[] = {
		{ "Glass Harbor (2018)/Season 02/Glass Harbor (2018) - S02E05.mkv", 24 },
		{ NULL, 0 }
	};
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 eleven = glass_row(fx, series_id, 1, 11);

	glass_change(fx, series_id);
	before = glass_links(fx, series_id);
	make_tree(fx, fx->series_dir, fx->root_id, more);
	run_all(fx, fx->root_id);
	after = q(fx, "SELECT string_agg(f.file_id || '>' || f.episode_id, ',' ORDER BY f.file_id, f.episode_id)"
		" FROM series_file_episode f JOIN series_file sf ON sf.file_id = f.file_id JOIN library_file lf ON lf.id ="
		" f.file_id WHERE sf.series_id = %" G_GINT64_FORMAT " AND lf.rel_path NOT LIKE '%%S02E05%%'", series_id);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(episode_state(fx, series_id, 2, 1), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 3), !=, AXIS_TV_WANTED);
	g_assert_cmpint(episode_state(fx, series_id, 2, 4), !=, AXIS_TV_WANTED);
	g_assert_cmpint(glass_row(fx, series_id, 2, 5), ==, eleven);
	assert_q(fx, "1", "SELECT count(*) FROM series_file_episode f JOIN library_file lf ON lf.id = f.file_id"
		" WHERE lf.rel_path LIKE '%S02E05%' AND f.episode_id = (SELECT id FROM series_episode WHERE season_number = 2"
		" AND number = 5 AND gone_at IS NULL)");
}

/* With the disk of its files offline while the numbering changes, every file
 * keeps its link (an offline disk never means its files are gone). */
static void
test_numbering_offline(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 series_id = glass_managed(fx);

	before = glass_links(fx, series_id);
	axis_test_library_unplug(fx->t, fx->disk, "away-glass");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);
	glass_change(fx, series_id);
	after = glass_links(fx, series_id);
	g_assert_cmpstr(after, ==, before);
	axis_test_library_plug(fx->t, fx->disk, "away-glass");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&after, g_free);
	after = glass_links(fx, series_id);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(episode_state(fx, series_id, 2, 1), !=, AXIS_TV_WANTED);
}

/*
 * Re-review, item 3: a file identification found that TMDb's numbering
 * places nowhere (named S02E05) and TheTVDB's places on a mapped row
 * (chapter 11): the plan says that row gets the file and is not wanted, and
 * that is what happens.
 */
static void
test_numbering_unlinked(Fixture *fx, gconstpointer data)
{
	static const Video more[] = {
		{ "Glass Harbor (2018)/Season 02/Glass Harbor (2018) - S02E05.mkv", 24 },
		{ NULL, 0 }
	};
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autoptr(GError) error = NULL;
	gboolean changed = TRUE;
	gint64 series_id;
	gint64 eleven;
	guint i;

	make_tree(fx, fx->series_dir, fx->root_id, more);
	series_id = glass_managed(fx);
	eleven = glass_row(fx, series_id, 1, 11);
	assert_q(fx, "0", "SELECT count(*) FROM series_file sf JOIN library_file lf ON lf.id = sf.file_id"
		" WHERE lf.rel_path LIKE '%S02E05%'");
	g_assert_true(axis_tv_plan_numbering(fx->tv, series_id, "tvdb", TRUE, &plan, &error));
	g_assert_no_error(error);
	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		if (row->episode_id != eleven)
			continue;
		g_assert_cmpuint(row->n_files_after, ==, 1);
		g_assert_false(row->wanted_after);
	}
	g_assert_true(axis_tv_change_numbering(fx->tv, series_id, "tvdb", plan->digest, FALSE, "alice", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_cmpint(glass_row(fx, series_id, 2, 5), ==, eleven);
	assert_q(fx, "1", "SELECT count(*) FROM series_file_episode f JOIN library_file lf ON lf.id = f.file_id"
		" WHERE lf.rel_path LIKE '%S02E05%' AND f.episode_id = (SELECT id FROM series_episode WHERE season_number = 2"
		" AND number = 5 AND gone_at IS NULL)");
}

/* ── What a person linked or unlinked by hand (add-force-grab-and-assign) ── */

static gint64
glass_file(
	Fixture     *fx,
	const gchar *tail
){
	g_autofree gchar *id = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%%s'", tail);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static gint64
qint(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
qint(
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
	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

/* Where @file is linked: "<episode row>:<hand_how>:<kept>", by row; "" for no
 * link. */
static gchar *
glass_link(
	Fixture *fx,
	gint64   file
){
	g_autofree gchar *found = q(fx, "SELECT string_agg(f.episode_id || ':' || coalesce(sf.hand_how, '-') || ':' ||"
		" sf.kept::text, ',' ORDER BY f.episode_id) FROM series_file sf JOIN series_file_episode f ON f.file_id ="
		" sf.file_id WHERE sf.file_id = %" G_GINT64_FORMAT, file);

	return g_strdup(found != NULL ? found : "");
}

/*
 * Slice C1.  Spec (library-identification): The run reads the name otherwise;
 * Unlinked by hand.  And a case met in use: a file unlinked by hand is linked
 * back nowhere by a later run or by a change of numbering, until a person
 * lets Axis manage it again.
 *
 * Glass Harbor's file named S01E03 is linked by hand to S01E04, and its file
 * named S01E01 is unlinked by hand.  The folder is looked at again (a new
 * file changes its fingerprint): the first is still on S01E04, by hand; the
 * second has no link; the new file is linked by its name as ever.
 */
static void
test_hand_link_kept(Fixture *fx, gconstpointer data)
{
	static const Video more[] = {
		{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E09.mkv", 24 },
		{ NULL, 0 }
	};
	static const Video again[] = {
		{ "Glass Harbor (2018)/Season 02/Glass Harbor (2018) - S02E06.mkv", 24 },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinks) listed = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *disk_after = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 one = glass_file(fx, "S01E01.mkv");
	gint64 three = glass_file(fx, "S01E03.mkv");
	gint64 four = glass_row(fx, series_id, 1, 4);
	gboolean changed = TRUE;
	guint i;

	disk = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state || ':' || inode, ',' ORDER BY id)"
		" FROM library_file WHERE kind = 'video'");
	g_ptr_array_add(asks, axis_tv_link_ask_new(three, FALSE, "S01E04"));
	g_ptr_array_add(asks, axis_tv_link_ask_new(one, TRUE, NULL));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_null(review->refused);
	g_assert_cmpuint(review->changes->len, ==, 2);
	/* Three episodes touched: S01E01 and S01E03 lose their file, S01E04 gets one. */
	g_assert_cmpuint(review->touched->len, ==, 3);
	g_assert_true(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", four);
	link = glass_link(fx, three);
	g_assert_cmpstr(link, ==, expected);
	g_clear_pointer(&link, g_free);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, "");
	g_clear_pointer(&link, g_free);
	assert_q(fx, "alice", "SELECT by_user FROM library_hand_unlink WHERE module = 'tv'");
	g_assert_cmpint(episode_state(fx, series_id, 1, 4), !=, AXIS_TV_WANTED);

	/* The run looks at the folder again. */
	make_tree(fx, fx->series_dir, fx->root_id, more);
	run_all(fx, fx->root_id);
	link = glass_link(fx, three);
	g_assert_cmpstr(link, ==, expected);
	g_clear_pointer(&link, g_free);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, "");
	g_clear_pointer(&link, g_free);
	assert_q(fx, "1", "SELECT count(*) FROM series_file_episode f JOIN library_file lf ON lf.id = f.file_id"
		" WHERE lf.rel_path LIKE '%S01E09.mkv' AND f.episode_id = (SELECT id FROM series_episode WHERE season_number = 1"
		" AND number = 9 AND gone_at IS NULL)");
	/* The links page lists it as unlinked by hand. */
	g_assert_true(axis_tv_links_read(fx->tv, series_id, NULL, 0, &listed, &error));
	g_assert_no_error(error);
	for (i = 0; i < listed->files->len; i++) {
		const AxisTvLinkFile *file = g_ptr_array_index(listed->files, i);

		g_assert_cmpint(file->unlinked_by_hand, ==, file->file_id == one);
		g_assert_cmpint(file->linked, ==, file->file_id != one);
	}
	g_assert_cmpuint(listed->files->len, ==, 4);

	/* A change of numbering: the hand link follows its row, and the file
	 * unlinked by hand is not among the files no link holds that it places. */
	glass_change(fx, series_id);
	link = glass_link(fx, three);
	g_assert_cmpstr(link, ==, expected);
	g_clear_pointer(&link, g_free);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, "");
	g_clear_pointer(&link, g_free);

	/* Let Axis manage it again: the next run that looks at the folder takes
	 * it up by its name; the hand link still stands. */
	g_assert_true(axis_tv_links_manage_review(fx->tv, series_id, one, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_UNLINKED);
	g_assert_true(axis_tv_links_manage(fx->tv, series_id, one, handed->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	assert_q(fx, "0", "SELECT count(*) FROM library_hand_unlink");
	make_tree(fx, fx->series_dir, fx->root_id, again);
	run_all(fx, fx->root_id);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, !=, "");
	g_clear_pointer(&link, g_free);
	link = glass_link(fx, three);
	g_assert_cmpstr(link, ==, expected);

	/* No file was touched by any of it. */
	disk_after = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state || ':' || inode, ',' ORDER BY id)"
		" FROM library_file WHERE kind = 'video' AND rel_path NOT LIKE '%%S01E09.mkv' AND rel_path NOT LIKE '%%S02E06.mkv'");
	g_assert_cmpstr(disk_after, ==, disk);
}

/*
 * "Let Axis manage this file again" hands the file back to identification:
 * the file named S01E01, moved by hand to S01E04, is on S01E01 alone after it
 * (its links replaced by what its name gives, never added to), at once and
 * after the next run.
 */
static void
test_manage_replaces(Fixture *fx, gconstpointer data)
{
	static const Video more[] = {
		{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E09.mkv", 24 },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *expected = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 one = glass_file(fx, "S01E01.mkv");
	gint64 first = glass_row(fx, series_id, 1, 1);
	gint64 four = glass_row(fx, series_id, 1, 4);
	gboolean changed = TRUE;

	g_ptr_array_add(asks, axis_tv_link_ask_new(one, FALSE, "S01E04"));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", four);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, expected);
	g_clear_pointer(&link, g_free);

	/* The review says what it does: the file is on S01E01 alone afterwards,
	 * S01E04 is left without a file (and is wanted, being monitored and aired). */
	g_assert_true(axis_tv_links_manage_review(fx->tv, series_id, one, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_PLACE);
	g_assert_cmpstr(handed->before, ==, "S01E04");
	g_assert_cmpstr(handed->after, ==, "S01E01");
	g_assert_cmpuint(handed->touched->len, ==, 2);
	g_assert_nonnull(strstr(((const AxisTvLinkTouched *)g_ptr_array_index(handed->touched, 1))->what, "Left without a file"));
	g_assert_null(handed->refused);
	g_assert_true(axis_tv_links_manage(fx->tv, series_id, one, handed->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	g_clear_pointer(&expected, g_free);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ":-:false", first);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, expected);
	g_clear_pointer(&link, g_free);
	/* And at the next run, which adds nothing to it. */
	make_tree(fx, fx->series_dir, fx->root_id, more);
	run_all(fx, fx->root_id);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, expected);
}

/*
 * "Let Axis manage this file again" on a file brought over: it is placed by
 * its name and no longer kept, and it keeps the stamp of the bringing over, as
 * "Let Axis upgrade it again" leaves it, so that a later run of the bringing
 * over, which keeps every identification link it finds, does not keep it
 * again (the person's choice sticks).
 */
static void
test_manage_brought(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *expected = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 one = glass_file(fx, "S01E01.mkv");
	gint64 first = glass_row(fx, series_id, 1, 1);
	gboolean changed = TRUE;

	g_ptr_array_add(asks, axis_tv_link_ask_new(one, FALSE, "S01E04"));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	/* As bringing over leaves a link: stamped, and kept because it was brought. */
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE series_file SET hand_how = 'brought', kept_how = 'brought'"
			" WHERE file_id = %" G_GINT64_FORMAT, one);

		axis_test_db_exec(fx->t->db, sql);
	}
	g_assert_true(axis_tv_links_manage_review(fx->tv, series_id, one, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_PLACE);
	g_assert_true(axis_tv_links_manage(fx->tv, series_id, one, handed->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ":brought:false", first);
	link = glass_link(fx, one);
	g_assert_cmpstr(link, ==, expected);
	assert_q(fx, "brought:false:-:true", "SELECT hand_how || ':' || kept || ':' || coalesce(kept_how, '-') || ':' || (hand_at"
		" IS NOT NULL) FROM series_file WHERE file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%S01E01.mkv')");
}

/*
 * A file whose name gives no episode of the series (S01E40) that a person
 * linked by hand: letting Axis manage it again says so and changes nothing, so
 * its link stays on S01E04, still marked by hand.
 */
static void
test_manage_nothing(Fixture *fx, gconstpointer data)
{
	static const Video odd[] = {
		{ "Glass Harbor (2018)/Season 01/Glass Harbor (2018) - S01E40.mkv", 24 },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *link = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 odd_file;
	gint64 four = glass_row(fx, series_id, 1, 4);
	gboolean changed = TRUE;

	make_tree(fx, fx->series_dir, fx->root_id, odd);
	run_all(fx, fx->root_id);
	odd_file = glass_file(fx, "S01E40.mkv");
	g_ptr_array_add(asks, axis_tv_link_ask_new(odd_file, FALSE, "S01E04"));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", four);
	link = glass_link(fx, odd_file);
	g_assert_cmpstr(link, ==, expected);
	g_assert_true(axis_tv_links_manage_review(fx->tv, series_id, odd_file, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_NOTHING);
	g_assert_null(handed->after);
	g_assert_nonnull(handed->refused);
	g_assert_false(axis_tv_links_manage(fx->tv, series_id, odd_file, handed->digest, "alice", NULL, &changed, &error));
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT);
	g_clear_pointer(&link, g_free);
	link = glass_link(fx, odd_file);
	g_assert_cmpstr(link, ==, expected);
}

/* The race: between the reading and the write, identification links the file
 * to another series (here, the hook does).  Nothing of this series' plan is
 * written, and the answer is the page again. */
static void
steal_file(gpointer data)
{
	Fixture *fx = data;

	axis_test_db_exec(fx->t->db, "UPDATE series_file SET series_id = (SELECT id FROM series ORDER BY id DESC LIMIT 1)"
		" WHERE file_id = (SELECT id FROM library_file WHERE rel_path LIKE '%S01E01.mkv')");
}

static void
test_apply_race(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinks) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) later = NULL;
	AxisTvSpec spec;
	gint64 series_id;
	gint64 other = 0;
	gint64 one;
	gboolean changed = FALSE;

	make_tree(fx, fx->series_dir, fx->root_id, lantern);
	run_all(fx, fx->root_id);
	series_id = glass_managed(fx);
	one = glass_file(fx, "S01E01.mkv");
	memset(&spec, 0, sizeof(spec));
	spec.title_id = series_title(fx, "7001");
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_ALL;
	spec.user = "alice";
	g_assert_true(axis_tv_add(fx->tv, &spec, &other, &later));
	g_assert_no_error(later);
	g_ptr_array_add(asks, axis_tv_link_ask_new(one, FALSE, "S01E04"));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	axis_tv_links_apply_hook = steal_file;
	axis_tv_links_apply_hook_data = fx;
	g_assert_false(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", &again, &changed, &error));
	axis_tv_links_apply_hook = NULL;
	g_assert_error(error, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT);
	g_assert_nonnull(strstr(error->message, "linked to"));
	/* The file is the other series' now, and no episode of this series was
	 * given to it. */
	g_assert_cmpint(qint(fx, "SELECT series_id FROM series_file WHERE file_id = %" G_GINT64_FORMAT, one), ==, other);
	g_assert_cmpint(qint(fx, "SELECT count(*) FROM series_file_episode f JOIN series_episode e ON e.id = f.episode_id"
		" WHERE f.file_id = %" G_GINT64_FORMAT " AND e.number = 4", one), ==, 0);
}

/*
 * The mark of an unlink is the file's, not the series': one made for a series
 * that was removed (and added again, so under a new id) can be cleared on the
 * new series' page, and removing a series clears the marks made for it.
 */
static void
test_unlink_mark(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	g_autoptr(AxisTvLinks) review = NULL;
	g_autoptr(AxisTvLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	gint64 series_id = glass_managed(fx);
	gint64 one = glass_file(fx, "S01E01.mkv");
	gboolean changed = TRUE;

	g_ptr_array_add(asks, axis_tv_link_ask_new(one, TRUE, NULL));
	g_assert_true(axis_tv_links_review(fx->tv, series_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_links_apply(fx->tv, series_id, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qint(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, one), ==, 1);
	/* Made for another (removed) series. */
	axis_test_db_exec(fx->t->db, "UPDATE library_hand_unlink SET owner_id = owner_id + 500");
	g_assert_true(axis_tv_links_manage_review(fx->tv, series_id, one, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_TV_MANAGE_UNLINKED);
	g_assert_true(axis_tv_links_manage(fx->tv, series_id, one, handed->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qint(fx, "SELECT count(*) FROM library_hand_unlink"), ==, 0);
	/* Removing the series clears the marks made for it. */
	axis_test_db_exec(fx->t->db, "INSERT INTO library_hand_unlink (file_id, module, owner_id, by_user)"
		" SELECT file_id, 'tv', series_id, 'alice' FROM series_file LIMIT 1");
	g_assert_cmpint(qint(fx, "SELECT count(*) FROM library_hand_unlink"), ==, 1);
	g_assert_true(axis_tv_remove(fx->tv, series_id, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpint(qint(fx, "SELECT count(*) FROM library_hand_unlink"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/identify/tv/manage-replaces", Fixture, NULL, test_manage_replaces);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/manage-brought", Fixture, NULL, test_manage_brought);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/manage-nothing", Fixture, NULL, test_manage_nothing);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/apply-race", Fixture, NULL, test_apply_race);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/unlink-mark", Fixture, NULL, test_unlink_mark);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/hand-link-kept", Fixture, NULL, test_hand_link_kept);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/numbering/unlinked", Fixture, NULL, test_numbering_unlinked);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/numbering/files", Fixture, NULL, test_numbering_files);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/numbering/reidentified", Fixture, NULL, test_numbering_reidentified);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/numbering/offline", Fixture, NULL, test_numbering_offline);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/series", Fixture, NULL, test_series);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/split-and-offline", Fixture, NULL, test_split_and_offline);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/managed", Fixture, NULL, test_managed);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/nothing-asked", Fixture, NULL, test_nothing_asked);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/review", Fixture, NULL, test_review);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/no-year", Fixture, NULL, test_no_year);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/reconsider", Fixture, NULL, test_reconsider);
	AXIS_TEST_ADD_DB_CASE("/identify/tv/continue", Fixture, NULL, test_continue);
	return g_test_run();
}
