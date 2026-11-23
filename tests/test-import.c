/*
 * test-import.c - the import pipeline on a real database and a real volume
 * (a directory of the scratch directory): a download and the films' root
 * on the same volume, so every file is hardlinked
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-import-pipeline (the cross-volume half of 6.1, 6.2, 6.8 and
 * 6.10, and 6.12, are in test-import-mounts.c; the names of 6.6 in
 * test-import-name.c):
 *   6.1  a torrent download hardlinked: the same inode, two links, the
 *        download's file untouched, the torrent left with its client; a
 *        Usenet download removed from its client after its import
 *   6.2  a failure at each step of a hardlink leaves the library as it was
 *        (no temporary name, no folder), and the import retries
 *   6.3  an upgrade: the new file placed first, the old one and its subtitle
 *        in the recycle folder, the film never without a file; a failed
 *        placement keeps the old file; the cleanup after the retention, and
 *        what it leaves alone; an upgrade to the same name swaps the names
 *   6.4  a partial file is never visible under a media name, and the scanner
 *        ignores it and the recycle folder
 *   6.5  the reasons: a sample, extras and their folders, several videos, an
 *        archive, an unreadable file, another film's name, the runtime; a
 *        film whose title says "sample"
 *   6.6  the Naming preview names a film as its import does
 *   6.7  the hardlinked source's contents, mode, owner and time unchanged
 *   6.8  nothing written while storage is read-only or the volume offline
 *   6.9  a link in a download, a download that is a link, one inside the
 *        root, one that climbs out: blocked, nothing written
 *   6.10 the actions (import anyway, choose, assign, dismiss, retry), each
 *        recorded with the user; import anyway refused for what it cannot
 *        override (several films, an offline volume)
 *   6.11 a run that dies at any step (of an import, and of an upgrade) and
 *        runs again completes once and records once; what a dead run left
 *        for a dismissed import is cleaned up
 * Also: the follow-up opens imports for completed grabs, and a manual
 * import from a downloads root; and (fix-first-import) the folder SABnzbd
 * keeps of a completed Usenet job, deleted after the import, waiting while
 * storage is read-only or the volume offline, left by each rule, and looked
 * at again for a row the previous release recorded; and (fix-first-torrent)
 * a job reported by its file imported from its folder, a torrent already
 * gone from its client: gone, not failed (also for a row recorded failed
 * before), what it left deleted inside the client's folder for Axis's label,
 * its folder left with no such folder (why and where said), its one file
 * deleted alone while the library has its copy, nothing deleted while
 * read-only or once torrents are left seeding.
 *
 * Every file is written under the scratch directory the test makes; nothing
 * else is touched.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-storage.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "import/axis-import-private.h"
#include "compat/axis-compat.h"
#include "compat/axis-compat-notify.h"
#include "indexer/axis-indexer.h"
#include "library/axis-library-kind.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define TOOL     "tests/fixtures/import/fake-ffprobe.sh"

#define HEARTH_RELEASE "Hearth.1995.1080p.BluRay.x264-GRP"
#define HEARTH_FOLDER  "Hearth (1995) {tmdb-6001}"
#define HEARTH_FILE    "Hearth (1995) {tmdb-6001} [BluRay 1080p][DTS-HD MA 5.1][x264]-GRP.mkv"
#define FILM_LINE    "AXISFAKE duration=10200 width=1920 height=800 vcodec=h264 acodec=dts aprofile=DTS-HD_MA channels=6"
#define SAMPLE_LINE  "AXISFAKE duration=60 width=1920 height=800 vcodec=h264"

typedef struct _Fixture Fixture;

typedef struct
{
	Fixture               *fx;
	const gchar           *at;
	AxisImportHookVerdict  verdict;
	guint                  seen;
	/* When set: at every step the film's folder must hold a video. */
	gchar                 *film_dir;
	guint                  without_film;
	guint                  steps;
	/* When set: at "copied", what the film's folder holds. */
	gchar                 *at_copied;
	/* The occurrence of @at that gets the verdict (0: the first). */
	guint                  nth;
	/* When set: at the first @act_at, @act_unlink is deleted (something else
	 * deleting a download's file while the import runs). */
	const gchar           *act_at;
	gchar                 *act_unlink;
	/* When set: at the first "temp", whether the client still held @watch
	 * (1) or not (0); -1 before. */
	const gchar           *watch;
	gint                   held_at_temp;
	/* When set: @act_fn is called at the @act_fn_nth (0: the first)
	 * @act_fn_at, as something else acting while the import runs. */
	const gchar           *act_fn_at;
	guint                  act_fn_nth;
	guint                  act_fn_seen;
	void                 (*act_fn)(Fixture *fx, gint64 import_id, gint64 file_id);
	/* What an @act_fn saw. */
	gboolean               act_result;
} Hook;

struct _Fixture
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	gint64                downloads_root;
	gchar                *films;
	gchar                *downloads;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisIndexers         *indexers;
	AxisTestDownloadFake *fake;
	AxisDownloads        *dl;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisMovies           *movies;
	AxisProbe            *probe;
	AxisImport           *import;
	gchar                *tool[3];
	Hook                  hook;
	guint                 refs;
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

static AxisImportHookVerdict
hook_fn(
	const gchar *step_name,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
);

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *base = NULL;
	AxisMetaTuning meta_tuning;
	AxisDownloadTuning download_tuning;
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
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->downloads_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "downloads", AXIS_MEDIA_DOWNLOADS);
	fx->films = g_build_filename(fx->disk, "films", NULL);
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
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_MOVIES_MODULE_ID, axis_movies_get_module()->settings,
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IMPORT_MODULE_ID, axis_import_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_COMPAT_MODULE_ID, axis_compat_get_setting_table(), &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_no_error(error);
	/* These tests are of the hardlinks (import.take_files "hardlink"): the
	 * download's files stay where they are.  The moves of the default have
	 * tests of their own below, which say "move". */
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_HARDLINK, &error));
	g_assert_no_error(error);

	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->dl, fx->t->storage,
		fx->quality);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->tool[0] = g_strdup("/bin/sh");
	fx->tool[1] = g_canonicalize_filename(TOOL, NULL);
	fx->tool[2] = NULL;
	memset(&probe_tuning, 0, sizeof(probe_tuning));
	probe_tuning.tool_command = (const gchar *const *)fx->tool;
	axis_probe_set_tuning(fx->probe, &probe_tuning);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->dl, fx->probe);
	axis_movies_set_import(fx->movies, fx->import);
	fx->hook.fx = fx;
	axis_import_set_hook(fx->import, hook_fn, &fx->hook);

	/* The films: no runtime unless the test is about it (a size per minute
	 * would reject these small files). */
	axis_test_meta_fixture_standard(fx->fa);
	set_route(fx, "/title/film/107", "{\"id\":\"107\",\"kind\":\"film\",\"title\":\"Hearth\",\"year\":1995,"
		"\"status\":\"released\",\"release_date\":\"1995-12-15\",\"ids\":{\"imdb\":\"tt0600001\",\"tmdb\":\"6001\"}}");
	set_route(fx, "/title/film/108", "{\"id\":\"108\",\"kind\":\"film\",\"title\":\"Rokan\",\"year\":1998,"
		"\"status\":\"released\",\"release_date\":\"1998-09-25\",\"ids\":{\"tmdb\":\"6002\"}}");
	set_route(fx, "/title/film/109", "{\"id\":\"109\",\"kind\":\"film\",\"title\":\"Colliery\",\"year\":2004,"
		"\"runtime\":120,\"status\":\"released\",\"release_date\":\"2004-08-06\",\"ids\":{\"tmdb\":\"6004\"}}");
	set_route(fx, "/title/film/111", "{\"id\":\"111\",\"kind\":\"film\",\"title\":\"Sample\",\"year\":2001,"
		"\"status\":\"released\",\"release_date\":\"2001-05-01\",\"ids\":{\"tmdb\":\"4321\"}}");
	/* A made-up film, for the Usenet job's leftover folder. */
	set_route(fx, "/title/film/120", "{\"id\":\"120\",\"kind\":\"film\",\"title\":\"Marrowfield Lanterns\","
		"\"year\":2011,\"status\":\"released\",\"release_date\":\"2011-03-04\",\"ids\":{\"tmdb\":\"990120\"}}");

	fx->fake = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(fx->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->dl, 0, &client, &fx->client_id, &error));
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_import_set_hook(fx->import, NULL, NULL);
	axis_movies_unref(fx->movies);
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
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->hook.film_dir);
	g_free(fx->hook.at_copied);
	g_free(fx->hook.act_unlink);
	g_free(fx->tool[0]);
	g_free(fx->tool[1]);
	g_free(fx->disk);
	g_free(fx->films);
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

static gint64
title_of(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 id;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_FILM, provider_id, 5000, &answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

static gint64
add_film(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

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

/* The download folder of @release in the downloads root, with the film,
 * its sample, an English subtitle and an .nfo. */
static gchar *
standard_download(
	Fixture     *fx,
	const gchar *release,
	guint        seed
){
	gchar *dir = g_build_filename(fx->downloads, release, NULL);
	g_autofree gchar *name = g_strconcat(release, ".mkv", NULL);
	g_autofree gchar *film = g_build_filename(dir, name, NULL);
	g_autofree gchar *sample_name = g_strconcat(release, "-sample.mkv", NULL);
	g_autofree gchar *sample = g_build_filename(dir, sample_name, NULL);
	g_autofree gchar *sub_name = g_strconcat(release, ".en.srt", NULL);
	g_autofree gchar *sub = g_build_filename(dir, sub_name, NULL);
	g_autofree gchar *nfo_name = g_strconcat(release, ".nfo", NULL);
	g_autofree gchar *nfo = g_build_filename(dir, nfo_name, NULL);

	write_media(film, FILM_LINE, 256 * 1024, seed);
	write_media(sample, SAMPLE_LINE, 8 * 1024, seed + 1);
	write_text(sub, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n");
	write_text(nfo, "an nfo\n");
	return dir;
}

/* A completed download of @release for film @movie at @rel_path (relative
 * to the volume), with its grab.  Returns the download's id. */
static gint64
add_download(
	Fixture     *fx,
	gint64       movie,
	const gchar *protocol,
	const gchar *release,
	const gchar *rel_path,
	const gchar *ref
){
	g_autofree gchar *title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	gint64 id;

	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', '%s', '%s', 'file', '%s', %s, '%s', 'movies',"
		" 'completed', 1000, 1000, '/data/%s', %" G_GINT64_FORMAT ", '%s', now(), now()) RETURNING id",
		fx->client_id, ref, protocol, release, title, release, rel_path, fx->volume_id, rel_path);
	g_assert_nonnull(download);
	id = g_ascii_strtoll(download, NULL, 10);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '%s', '%s', lower('1:0:%s'), %" G_GINT64_FORMAT ", now(), now())"
		" RETURNING id", movie, release, protocol, release, id);
	g_assert_nonnull(grab);
	return id;
}

static gint64
request(
	Fixture *fx,
	gint64   download
){
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	gint64 id = 0;
	gboolean created = FALSE;

	g_assert_no_error(error);
	g_assert_true(axis_import_request(fx->import, conn, AXIS_MOVIES_MODULE_ID, download, &id, &created, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* One run of the job; its state afterwards. */
static const gchar *
run(
	Fixture          *fx,
	gint64            import_id,
	AxisImportReport *report_out
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	memset(&report, 0, sizeof(report));
	if (!axis_import_run(fx->import, import_id, NULL, &report, &error))
		g_error("the import run failed: %s", error != NULL ? error->message : "?");
	if (report_out != NULL)
		*report_out = report;
	if (g_strcmp0(report.state, "imported") != 0) {
		g_autofree gchar *why = q(fx, "SELECT state || ' ' || coalesce(reason, '') || ': ' || coalesce(reason_text, '')"
			" || ' ' || blockers::text FROM import WHERE id = %" G_GINT64_FORMAT, import_id);

		g_test_message("import %" G_GINT64_FORMAT ": %s (report %s/%s, placed %u; steps %u)", import_id, why,
			report.state, report.reason != NULL ? report.reason : "-", report.placed, fx->hook.steps);
	}
	return report.state;
}

/* A run that fails (an injected failure or crash); the error's message. */
static gchar *
run_failing(
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

static gchar *
state_of(
	Fixture *fx,
	gint64   import_id
){
	return q(fx, "SELECT state || coalesce('/' || reason, '') FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
}

static void
assert_state(
	Fixture     *fx,
	gint64       import_id,
	const gchar *expected
){
	g_autofree gchar *state = state_of(fx, import_id);

	if (g_strcmp0(state, expected) != 0) {
		g_autofree gchar *text = q(fx, "SELECT coalesce(reason_text, '') || ' ' || blockers::text FROM import"
			" WHERE id = %" G_GINT64_FORMAT, import_id);

		g_error("import %" G_GINT64_FORMAT ": expected %s, got %s (%s)", import_id, expected, state, text);
	}
}

/* A listing of a tree: names, kinds, modes, sizes, inodes, links and
 * contents; not the folders' times (a folder made and removed again
 * changes its parent's). */
static void
tree_at(
	const gchar *path,
	const gchar *shown,
	GString     *out
){
	GStatBuf st;

	if (g_lstat(path, &st) != 0) {
		g_string_append_printf(out, "%s gone\n", shown);
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		GDir *dir = g_dir_open(path, 0, NULL);
		g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
		const gchar *name;
		guint i;

		g_string_append_printf(out, "%s/ mode=%o\n", shown, (guint)(st.st_mode & 07777));
		while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
			g_ptr_array_add(names, g_strdup(name));
		if (dir != NULL)
			g_dir_close(dir);
		g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
		for (i = 0; i < names->len; i++) {
			g_autofree gchar *child = g_build_filename(path, g_ptr_array_index(names, i), NULL);
			g_autofree gchar *child_shown = g_strconcat(shown, "/", (const gchar *)g_ptr_array_index(names, i), NULL);

			tree_at(child, child_shown, out);
		}
	} else if (S_ISREG(st.st_mode)) {
		g_autofree gchar *contents = NULL;
		g_autofree gchar *sum = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
		sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
		g_string_append_printf(out, "%s mode=%o size=%" G_GINT64_FORMAT " ino=%" G_GUINT64_FORMAT " links=%u"
			" mtime=%" G_GINT64_FORMAT " sha=%s\n", shown, (guint)(st.st_mode & 07777), (gint64)st.st_size,
			(guint64)st.st_ino, (guint)st.st_nlink, (gint64)st.st_mtime, sum);
	} else if (S_ISLNK(st.st_mode)) {
		g_autofree gchar *target = g_file_read_link(path, NULL);

		g_string_append_printf(out, "%s link=%s\n", shown, target != NULL ? target : "?");
	} else {
		g_string_append_printf(out, "%s other\n", shown);
	}
}

static gchar *
tree(const gchar *path)
{
	GString *out = g_string_new(NULL);

	tree_at(path, ".", out);
	return g_string_free(out, FALSE);
}

/* How many names in the tree begin with @prefix. */
static guint
count_names(
	const gchar *path,
	const gchar *prefix
){
	GDir *dir = g_dir_open(path, 0, NULL);
	const gchar *name;
	guint n = 0;

	if (dir == NULL)
		return 0;
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		if (g_str_has_prefix(name, prefix))
			n++;
		if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
			n += count_names(child, prefix);
	}
	g_dir_close(dir);
	return n;
}

/* How many videos (.mkv) the folder holds, not counting temporary names. */
static guint
count_videos(const gchar *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	const gchar *name;
	guint n = 0;

	if (dir == NULL)
		return 0;
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (g_str_has_suffix(name, ".mkv") && name[0] != '.')
			n++;
	}
	g_dir_close(dir);
	return n;
}

/* Empties the test's film folder (its files only) and removes it, with the
 * film's rows: for the next round of a test. */
static void
clear_film(
	Fixture     *fx,
	const gchar *path
){
	GDir *dir = g_dir_open(path, 0, NULL);
	const gchar *name;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		g_assert_true(g_file_test(child, G_FILE_TEST_IS_REGULAR));
		g_assert_cmpint(g_unlink(child), ==, 0);
	}
	g_dir_close(dir);
	g_assert_cmpint(g_rmdir(path), ==, 0);
	exec(fx, "DELETE FROM movie_file");
	exec(fx, "DELETE FROM library_file");
}

/* Removes a folder of the test's own, with what is in it. */
static void
remove_tree(const gchar *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	const gchar *name;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);

		if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
			remove_tree(child);
		else
			g_assert_cmpint(g_unlink(child), ==, 0);
	}
	g_dir_close(dir);
	g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void
clear_film_tree(
	Fixture     *fx,
	const gchar *path
){
	remove_tree(path);
	exec(fx, "DELETE FROM movie_file");
	exec(fx, "DELETE FROM library_file");
}

static GStatBuf
stat_of(const gchar *path)
{
	GStatBuf st;

	memset(&st, 0, sizeof(st));
	if (g_lstat(path, &st) != 0)
		g_error("%s: %s", path, g_strerror(errno));
	return st;
}

static gchar *
hearth_path(
	Fixture     *fx,
	const gchar *file
){
	return g_build_filename(fx->films, HEARTH_FOLDER, file, NULL);
}

static void
hook_reset(Fixture *fx)
{
	fx->hook.at = NULL;
	fx->hook.verdict = AXIS_IMPORT_HOOK_GO;
	fx->hook.seen = 0;
	fx->hook.steps = 0;
	fx->hook.without_film = 0;
	fx->hook.nth = 0;
	fx->hook.act_at = NULL;
	fx->hook.watch = NULL;
	fx->hook.held_at_temp = -1;
	fx->hook.act_fn_at = NULL;
	fx->hook.act_fn_nth = 0;
	fx->hook.act_fn_seen = 0;
	fx->hook.act_fn = NULL;
	fx->hook.act_result = FALSE;
	g_clear_pointer(&fx->hook.film_dir, g_free);
	g_clear_pointer(&fx->hook.at_copied, g_free);
	g_clear_pointer(&fx->hook.act_unlink, g_free);
}

static void
hook_at(
	Fixture               *fx,
	const gchar           *at,
	AxisImportHookVerdict  verdict
){
	hook_reset(fx);
	fx->hook.at = at;
	fx->hook.verdict = verdict;
}

static AxisImportHookVerdict
hook_fn(
	const gchar *step_name,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
){
	Hook *hook = data;

	hook->steps++;
	if (hook->film_dir != NULL && count_videos(hook->film_dir) == 0)
		hook->without_film++;
	if (strcmp(step_name, "copied") == 0 && hook->at_copied == NULL && hook->fx != NULL) {
		g_autofree gchar *dir = g_build_filename(hook->fx->films, HEARTH_FOLDER, NULL);

		hook->at_copied = tree(dir);
	}
	if (hook->watch != NULL && hook->held_at_temp < 0 && strcmp(step_name, "temp") == 0 && hook->fx != NULL)
		hook->held_at_temp = axis_test_download_fake_has(hook->fx->fake, hook->watch) ? 1 : 0;
	if (hook->act_at != NULL && hook->act_unlink != NULL && strcmp(step_name, hook->act_at) == 0) {
		g_assert_cmpint(g_unlink(hook->act_unlink), ==, 0);
		g_clear_pointer(&hook->act_unlink, g_free);
	}
	if (hook->act_fn != NULL && hook->act_fn_at != NULL && strcmp(step_name, hook->act_fn_at) == 0 &&
	    ++hook->act_fn_seen == MAX(hook->act_fn_nth, 1))
		hook->act_fn(hook->fx, import_id, file_id);
	if (hook->at != NULL && strcmp(step_name, hook->at) == 0 && ++hook->seen == MAX(hook->nth, 1))
		return hook->verdict;
	return AXIS_IMPORT_HOOK_GO;
}

static gint64
movie_state(
	Fixture *fx,
	gint64   movie
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;

	g_assert_true(axis_movies_get(fx->movies, movie, &facts, &error));
	g_assert_no_error(error);
	axis_movies_get_rules(fx->movies, &rules);
	return (gint64)axis_movie_derive(facts, &rules, NULL);
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

static void
set_read_only(
	Fixture  *fx,
	gboolean  on
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, on ? "true" : "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A Hearth download of the standard kind, its import requested. */
static gint64
hearth_import(
	Fixture     *fx,
	gint64       movie,
	const gchar *protocol,
	const gchar *release,
	const gchar *ref,
	gint64      *download_out
){
	g_autofree gchar *dir = standard_download(fx, release, 1);
	g_autofree gchar *rel = g_strconcat("downloads/", release, NULL);
	gint64 download = add_download(fx, movie, protocol, release, rel, ref);

	if (download_out != NULL)
		*download_out = download;
	return request(fx, download);
}

/* ── 6.1 Hardlinked ─────────────────────────────────────────────── */

static void
test_hardlink(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	gint64 download = 0;
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autofree gchar *source_digest = axis_test_storage_digest(dir);
	g_autofree gchar *source_film = g_build_filename(dir, HEARTH_RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(dir, HEARTH_RELEASE ".en.srt", NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
	g_autofree gchar *placed_sub = hearth_path(fx, "Hearth (1995) {tmdb-6001} [BluRay 1080p][DTS-HD MA 5.1][x264]-GRP.en.srt");
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *after = NULL;
	g_autofree gchar *library = NULL;
	GStatBuf before_st = stat_of(source_film);
	GStatBuf film_st;
	GStatBuf source_st;
	GStatBuf folder_st;
	AxisImportReport report;
	gint64 id;

	/* Left seeding by choice: the default removes the torrent (below). */
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "seed");
	download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "downloads/" HEARTH_RELEASE, "torrent-1");
	id = request(fx, download);
	assert_state(fx, id, "waiting");
	/* A job is queued for it, on the downloads queue. */
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'import.run' AND queue = 'downloads'"
		" AND dedupe_key = 'import:%" G_GINT64_FORMAT "'", id);
	g_assert_cmpstr(run(fx, id, &report), ==, "imported");
	g_assert_cmpuint(report.placed, ==, 2);
	g_assert_cmpuint(report.recycled, ==, 0);

	/* The same file, by a second name. */
	film_st = stat_of(placed);
	source_st = stat_of(source_film);
	g_assert_cmpuint((guint64)film_st.st_ino, ==, (guint64)source_st.st_ino);
	g_assert_cmpuint((guint)film_st.st_nlink, ==, 2);
	g_assert_cmpuint((guint64)stat_of(placed_sub).st_ino, ==, (guint64)stat_of(source_sub).st_ino);
	/* The download's file as it was: contents, mode, owner, time. */
	g_assert_cmpuint(source_st.st_mode, ==, before_st.st_mode);
	g_assert_cmpuint(source_st.st_uid, ==, before_st.st_uid);
	g_assert_cmpuint(source_st.st_gid, ==, before_st.st_gid);
	g_assert_cmpint((gint64)source_st.st_mtime, ==, (gint64)before_st.st_mtime);
	after = axis_test_storage_digest(dir);
	g_assert_cmpstr(after, ==, source_digest);
	/* The folder made with import.directory_mode; nothing temporary left;
	 * nothing but the film and its subtitle (no sample, no .nfo). */
	folder_st = stat_of(folder);
	g_assert_cmpuint(folder_st.st_mode & 07777, ==, 0755);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	library = tree(folder);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	g_assert_null(strstr(library, "sample"));
	g_assert_null(strstr(library, ".nfo"));

	/* What was recorded. */
	assert_qf(fx, "hardlink", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_qf(fx, "kept", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_qf(fx, "bluray_1080p", "SELECT quality FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "imported", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
		" WHERE mf.movie_id = %" G_GINT64_FORMAT " AND lf.inode = %" G_GUINT64_FORMAT " AND mf.import_id = %"
		G_GINT64_FORMAT, movie, (guint64)film_st.st_ino, id);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'imported'",
		movie);
	assert_qf(fx, "sample", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%-sample.mkv'", id);
	assert_qf(fx, "other", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE '%%.nfo'", id);
	assert_qf(fx, "published", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND role = 'film'", id);
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_IN_LIBRARY);
	/* A torrent is left to seed: its client is not asked to remove it. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* Run again: nothing more. */
	g_assert_cmpstr(run(fx, id, &report), ==, "imported");
	g_assert_cmpuint(report.placed, ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'imported'",
		movie);
	/* And no second import is opened for the download. */
	g_assert_cmpint(request(fx, download), ==, id);
}

static void
test_usenet(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	gint64 download = 0;
	gint64 id;

	axis_test_download_fake_put(fx->fake, "SABnzbd_nzo_hearth", HEARTH_RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/data/downloads/" HEARTH_RELEASE, NULL);
	id = hearth_import(fx, movie, "usenet", HEARTH_RELEASE, "SABnzbd_nzo_hearth", &download);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	/* Removed from its client, with its data, once it is in the library. */
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "SABnzbd_nzo_hearth"), ==, 1);
	g_assert_false(axis_test_download_fake_has(fx->fake, "SABnzbd_nzo_hearth"));
	assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	{
		g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);

		g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
	}
}

/* A torrent, by default: removed from its client with its data, retried when
 * the client cannot be reached, and the library's file is the same inode
 * with one name fewer to share it. */
static void
test_torrent_removed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autofree gchar *source_film = g_build_filename(dir, HEARTH_RELEASE ".mkv", NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
	g_autofree gchar *text = NULL;
	AxisImportCleanupReport cleanup;
	GStatBuf source_before = stat_of(source_film);
	GStatBuf film;
	gint64 movie = add_film(fx, "107");
	gint64 download = 0;
	gint64 id;

	g_assert_cmpuint((guint)source_before.st_nlink, ==, 1);
	/* The client holds it, and deletes what it holds when asked to. */
	axis_test_download_fake_put(fx->fake, "torrent-rm", HEARTH_RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/data/downloads/" HEARTH_RELEASE, NULL);
	axis_test_download_fake_set_data_dir(fx->fake, fx->downloads);
	/* The client is unreachable when the import ends. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "downloads/" HEARTH_RELEASE, "torrent-rm");
	id = request(fx, download);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");

	/* A server being down never fails an import: the removal waits. */
	assert_qf(fx, "failed", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	film = stat_of(placed);
	g_assert_cmpuint((guint64)film.st_ino, ==, (guint64)source_before.st_ino);
	g_assert_cmpuint((guint)film.st_nlink, ==, 2);
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* The cleanup asks again, and now it goes, with its data. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(cleanup.removals, ==, 1);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "torrent-rm"), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", "torrent-rm"), ==, 1);
	g_assert_false(axis_test_download_fake_has(fx->fake, "torrent-rm"));
	/* The download's names are gone; the library's file is the same inode,
	 * and its link count went from 2 to 1. */
	g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	film = stat_of(placed);
	g_assert_cmpuint((guint64)film.st_ino, ==, (guint64)source_before.st_ino);
	g_assert_cmpuint((guint)film.st_nlink, ==, 1);
	g_assert_cmpint((gint64)film.st_size, ==, (gint64)source_before.st_size);
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_IN_LIBRARY);
	/* The record says what happened. */
	text = q(fx, "SELECT client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "The torrent was removed from its client with its downloaded files");
	assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	/* A torrent's files are its client's business: Axis removes no folder. */
	assert_qf(fx, "-", "SELECT coalesce(folder_removal, '-') FROM import WHERE id = %" G_GINT64_FORMAT, id);
	/* Once only: nothing more is asked of the client. */
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_cmpuint(cleanup.removals, ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 1);
}

/* Chosen to seed: nothing is asked of the client, then or later. */
static void
test_torrent_seed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autofree gchar *source_film = g_build_filename(dir, HEARTH_RELEASE ".mkv", NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
	AxisImportCleanupReport cleanup;
	gint64 movie = add_film(fx, "107");
	gint64 id;

	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "seed");
	axis_test_download_fake_put(fx->fake, "torrent-seed", HEARTH_RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/data/downloads/" HEARTH_RELEASE, NULL);
	axis_test_download_fake_set_data_dir(fx->fake, fx->downloads);
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "torrent-seed", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	assert_qf(fx, "kept", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_cmpuint(cleanup.removals, ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, "torrent-seed"));
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint((guint)stat_of(placed).st_nlink, ==, 2);

	/* A removal waiting when the choice is made is not made. */
	exec(fx, "UPDATE import SET client_removal = 'pending' WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	assert_qf(fx, "kept", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
}

/* ── A Usenet job's leftover folder ─────────────────────────────── */

/* A made-up film, its release, and where SABnzbd puts it: the category's
 * folder /usenet/complete/axis is downloads/axis on the volume (the path
 * map takes /usenet/complete to downloads). */
#define LANTERN_ID       "120"
#define LANTERN_RELEASE  "Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP"
#define LANTERN_FOLDER   "Marrowfield Lanterns (2011) {tmdb-990120}"
#define SAB_COMPLETE     "/usenet/complete"
#define SAB_CATEGORY     SAB_COMPLETE "/axis"
#define SAB_JOB          SAB_CATEGORY "/" LANTERN_RELEASE
#define JOB_REL          "downloads/axis/" LANTERN_RELEASE

typedef struct
{
	gint64  movie;
	gint64  download;
	gint64  id;
	gchar  *dir;		/* the job's folder on the volume */
	gchar  *film;		/* the film's file in it */
	gchar  *placed;		/* the film's file in the library */
} UsenetJob;

static void
usenet_job_clear(UsenetJob *job)
{
	g_free(job->dir);
	g_free(job->film);
	g_free(job->placed);
}

/* A file of another job, straight in the category's folder. */
#define OTHER_JOB_FILE "Quillfeather.Junction.2014-MADEUP.nfo"

/* What SABnzbd leaves of a completed job, its film's file at @film beneath
 * the category's folder: with it in the job's folder (at its top or deeper),
 * a sample, a subtitle, an .nfo, the par2 files and a folder of its own
 * there too; with it straight in the category's folder, a file of another
 * job beside it. */
static void
write_job(
	Fixture     *fx,
	const gchar *film
){
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *film_path = g_build_filename(category, film, NULL);

	write_media(film_path, FILM_LINE, 256 * 1024, 5);
	if (strchr(film, '/') != NULL) {
		g_autofree gchar *dir = g_build_filename(category, LANTERN_RELEASE, NULL);
		g_autofree gchar *sample = g_build_filename(dir, LANTERN_RELEASE "-sample.mkv", NULL);
		g_autofree gchar *sub = g_build_filename(dir, LANTERN_RELEASE ".en.srt", NULL);
		g_autofree gchar *nfo = g_build_filename(dir, LANTERN_RELEASE ".nfo", NULL);
		g_autofree gchar *par2 = g_build_filename(dir, LANTERN_RELEASE ".vol00+01.par2", NULL);
		g_autofree gchar *proof = g_build_filename(dir, "Proof", "proof.txt", NULL);

		write_media(sample, SAMPLE_LINE, 8 * 1024, 6);
		write_text(sub, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n");
		write_text(nfo, "an nfo\n");
		write_text(par2, "par2 leftovers\n");
		write_text(proof, "a proof\n");
	} else {
		g_autofree gchar *other = g_build_filename(category, OTHER_JOB_FILE, NULL);

		write_text(other, "another job's nfo\n");
	}
}

/*
 * A completed SABnzbd job of the made-up film, imported.  The film's file is
 * at @film beneath the category's folder, and SABnzbd reports the job at
 * @reported beneath it (the job's folder, as it is believed to report a job
 * of several files; or the file, as it was seen to report a job of one).
 * The fake client behaves as SABnzbd does: the removal deletes nothing of a
 * completed job, and it keeps Axis's category in /usenet/complete/axis.  With
 * @client_down the client does not answer while the import ends, so the
 * client removal fails and the folder waits for it.
 */
static void
usenet_imported_full(
	Fixture     *fx,
	gboolean     client_down,
	const gchar *reported,
	const gchar *film,
	const gchar *category_dir,
	UsenetJob   *job
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *client_path = g_strconcat(SAB_CATEGORY "/", reported, NULL);
	g_autofree gchar *rel_path = g_strconcat("downloads/axis/", reported, NULL);

	memset(job, 0, sizeof(*job));
	job->movie = add_film(fx, LANTERN_ID);
	job->dir = g_build_filename(fx->downloads, "axis", LANTERN_RELEASE, NULL);
	job->film = g_build_filename(fx->downloads, "axis", film, NULL);
	job->placed = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUP.mkv",
		NULL);
	write_job(fx, film);
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, SAB_COMPLETE, fx->volume_id, "downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_keep_completed(fx->fake, TRUE);
	axis_test_download_fake_set_category_dir(fx->fake, category_dir);
	{
		g_autofree gchar *data = g_build_filename(fx->downloads, "axis", NULL);

		axis_test_download_fake_set_data_dir(fx->fake, data);
	}
	axis_test_download_fake_put(fx->fake, "SABnzbd_nzo_lantern", LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
		client_path, NULL);
	title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, job->movie);
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'SABnzbd_nzo_lantern', 'usenet', 'file', '%s', %s,"
		" 'Marrowfield Lanterns (2011)', 'movies', 'completed', 1000, 1000, '%s', %" G_GINT64_FORMAT ", '%s', now(),"
		" now()) RETURNING id", fx->client_id, LANTERN_RELEASE, title, client_path, fx->volume_id, rel_path);
	job->download = g_ascii_strtoll(download, NULL, 10);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '%s', 'usenet', lower('1:0:%s'), %" G_GINT64_FORMAT ", now(),"
		" now()) RETURNING id", job->movie, LANTERN_RELEASE, LANTERN_RELEASE, job->download);
	g_assert_nonnull(grab);
	job->id = request(fx, job->download);
	if (client_down)
		axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	g_assert_cmpstr(run(fx, job->id, NULL), ==, "imported");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_true(g_file_test(job->placed, G_FILE_TEST_IS_REGULAR));
	/* After it, the client keeps the category where it says now. */
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);
}

static void
usenet_imported_at(
	Fixture     *fx,
	gboolean     client_down,
	const gchar *reported,
	const gchar *film,
	UsenetJob   *job
){
	usenet_imported_full(fx, client_down, reported, film, SAB_CATEGORY, job);
}

/* SABnzbd reporting the job's folder, the film at its top. */
static void
usenet_imported(
	Fixture   *fx,
	gboolean   client_down,
	UsenetJob *job
){
	usenet_imported_at(fx, client_down, LANTERN_RELEASE, LANTERN_RELEASE "/" LANTERN_RELEASE ".mkv", job);
}

static void
cleanup(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisImportCleanupReport report;

	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &report, &error));
	g_assert_no_error(error);
}

static void
assert_folder(
	Fixture     *fx,
	gint64       id,
	const gchar *state,
	const gchar *words
){
	g_autofree gchar *now = q(fx, "SELECT coalesce(folder_removal, '-') FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_autofree gchar *text = q(fx, "SELECT folder_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, id);

	if (g_strcmp0(now, state) != 0)
		g_error("import %" G_GINT64_FORMAT ": the folder is %s, not %s (%s)", id, now, state, text != NULL ? text : "");
	if (words != NULL)
		assert_contains(text, words);
}

/* Spec (import): A Usenet download's leftover folder is removed after its
 * import.  SABnzbd keeps a completed job's folder when the job is removed with
 * its files; Axis deletes it, with everything SABnzbd left in it, and the
 * library's file keeps its own name. */
static void
test_usenet_folder_removed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *text = NULL;
	GStatBuf placed;
	UsenetJob job;

	usenet_imported(fx, FALSE, &job);
	placed = stat_of(job.placed);
	/* The client was asked to remove the job with its files, and (as SABnzbd
	 * does for a completed job) kept them. */
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", "SABnzbd_nzo_lantern"), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-kept-files", "SABnzbd_nzo_lantern"), ==, 1);
	g_assert_false(axis_test_download_fake_has(fx->fake, "SABnzbd_nzo_lantern"));
	text = q(fx, "SELECT client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	g_assert_cmpstr(text, ==, "The job was removed from its client.");
	/* Then Axis deleted the folder SABnzbd left, the client having said where
	 * it keeps Axis's category. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "folder", "axis"), ==, 1);
	assert_folder(fx, job.id, "done", "was deleted, with the 7 files and folders the client left in it");
	assert_folder(fx, job.id, "done", SAB_JOB);
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	/* The category's folder and the downloads root stay. */
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(fx->downloads, G_FILE_TEST_IS_DIR));
	/* The library's file: the same inode, with its one name now. */
	{
		GStatBuf after = stat_of(job.placed);

		g_assert_cmpuint((guint64)after.st_ino, ==, (guint64)placed.st_ino);
		g_assert_cmpint((gint64)after.st_size, ==, (gint64)placed.st_size);
		g_assert_cmpuint((guint)after.st_nlink, ==, 1);
	}
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	/* Once: the cleanup has nothing more to do, and asks nothing. */
	{
		g_autoptr(GError) error = NULL;
		AxisImportCleanupReport report;

		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_run_cleanup(fx->import, 0, &report, &error));
		g_assert_cmpuint(report.folders, ==, 0);
		g_assert_cmpuint(report.removals, ==, 0);
	}
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "folder", NULL), ==, 1);
	usenet_job_clear(&job);
}

/* Spec (import): never while storage is read-only or the volume is offline:
 * the folder waits, the page says why, and the hourly cleanup deletes it once
 * it can.  And it waits for the client's removal first. */
static void
test_usenet_folder_waits(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	UsenetJob job;

	usenet_imported(fx, TRUE, &job);
	/* The client did not answer: its removal failed, the folder waits for
	 * it, untouched. */
	assert_qf(fx, "failed", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "pending", NULL);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "folder", NULL), ==, 0);
	before = axis_test_storage_digest(job.dir);

	/* Storage read-only: the client removes the job, the folder waits. */
	set_read_only(fx, TRUE);
	cleanup(fx);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "waiting", "Read-only storage is on");
	assert_folder(fx, job.id, "waiting", "tried again every hour");
	after = axis_test_storage_digest(job.dir);
	g_assert_cmpstr(after, ==, before);
	g_clear_pointer(&after, g_free);
	/* Again, still read-only: still waiting. */
	cleanup(fx);
	assert_folder(fx, job.id, "waiting", "Read-only storage is on");
	set_read_only(fx, FALSE);

	/* The volume offline: waits too, and nothing is written. */
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	cleanup(fx);
	assert_folder(fx, job.id, "waiting", "is offline");
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	after = axis_test_storage_digest(job.dir);
	g_assert_cmpstr(after, ==, before);

	/* Writable and online: deleted. */
	cleanup(fx);
	assert_folder(fx, job.id, "done", "was deleted");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	usenet_job_clear(&job);
}

/* Looks again at a folder that was left: as if it had not been looked at. */
static void
look_again(
	Fixture         *fx,
	const UsenetJob *job
){
	exec(fx, "UPDATE import SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		job->id);
}

/* Spec (import): each rule that leaves the folder where it is, the page
 * saying which.  The folder is never touched. */
static void
test_usenet_folder_rules(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *real = NULL;
	UsenetJob job;

	/* Imported with the client away: the folder waits for its removal. */
	usenet_imported(fx, TRUE, &job);
	before = axis_test_storage_digest(job.dir);

	/* The client keeps Axis's category in no folder of its own. */
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	cleanup(fx);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "left", "no folder Axis may delete in");
	/* Left is final: the cleanup does not ask again. */
	cleanup(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "folder", NULL), ==, 1);

	/* The job is outside the category's folder. */
	look_again(fx, &job);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_COMPLETE "/other");
	cleanup(fx);
	assert_folder(fx, job.id, "left", "not inside the folder of Axis's category");

	/* The job's folder is the category's own (its files put straight in). */
	look_again(fx, &job);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_JOB);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "itself, not a job's own folder");
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);

	/* A path with "..": refused, never resolved. */
	look_again(fx, &job);
	exec(fx, "UPDATE download SET client_path = '" SAB_CATEGORY "/../axis/" LANTERN_RELEASE "' WHERE id = %"
		G_GINT64_FORMAT, job.download);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "'..'");
	exec(fx, "UPDATE download SET client_path = '" SAB_JOB "' WHERE id = %" G_GINT64_FORMAT, job.download);

	/* The only mapping that reaches it begins inside the category's folder. */
	look_again(fx, &job);
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, SAB_JOB, fx->volume_id, JOB_REL, &error));
		g_assert_no_error(error);
		cleanup(fx);
		assert_folder(fx, job.id, "left", "begins inside the folder of Axis's category");
		g_assert_true(axis_downloads_remove_path_map(fx->dl, fx->client_id, SAB_JOB, &error));
		g_assert_no_error(error);
	}

	/* The mapping puts it elsewhere now than it was imported from. */
	look_again(fx, &job);
	exec(fx, "UPDATE download_path_map SET rel_path = 'downloads/moved' WHERE client_id = %" G_GINT64_FORMAT,
		fx->client_id);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "another place than the one its files were imported from");
	exec(fx, "UPDATE download_path_map SET rel_path = 'downloads' WHERE client_id = %" G_GINT64_FORMAT, fx->client_id);

	/* Inside a library root that holds films. */
	look_again(fx, &job);
	exec(fx, "UPDATE library_root SET media_type = 'movies' WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "inside the library root");
	exec(fx, "UPDATE library_root SET media_type = 'downloads' WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);

	/* Another download still at its client is in it. */
	look_again(fx, &job);
	exec(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" volume_id, rel_path) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'SABnzbd_nzo_inside', 'usenet', 'file',"
		" 'Quillfeather.Junction.2014-MADEUP', 'downloading', %" G_GINT64_FORMAT ", '" JOB_REL "/Second')",
		fx->client_id, fx->volume_id);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "another download Axis knows");
	exec(fx, "DELETE FROM download WHERE client_ref = 'SABnzbd_nzo_inside'");

	/* The client never removed the job: the folder is still its. */
	look_again(fx, &job);
	exec(fx, "UPDATE import SET client_removal = 'kept' WHERE id = %" G_GINT64_FORMAT, job.id);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "did not remove the job");
	exec(fx, "UPDATE import SET client_removal = 'done' WHERE id = %" G_GINT64_FORMAT, job.id);

	/* A symbolic link at its place: never followed. */
	look_again(fx, &job);
	real = g_strconcat(job.dir, ".real", NULL);
	g_assert_cmpint(g_rename(job.dir, real), ==, 0);
	g_assert_cmpint(symlink(real, job.dir), ==, 0);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "symbolic link");
	g_assert_cmpint(g_unlink(job.dir), ==, 0);
	g_assert_cmpint(g_rename(real, job.dir), ==, 0);
	{
		g_autofree gchar *now = axis_test_storage_digest(job.dir);

		g_assert_cmpstr(now, ==, before);
	}

	/* What is there is not the folder imported from: the film's file is
	 * another one now. */
	look_again(fx, &job);
	g_assert_cmpint(g_unlink(job.film), ==, 0);
	write_media(job.film, FILM_LINE, 4096, 9);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "not in it any more");

	/* Through all of it the folder and everything in it stayed. */
	g_assert_true(g_file_test(job.dir, G_FILE_TEST_IS_DIR));
	{
		g_autofree gchar *nfo = g_build_filename(job.dir, LANTERN_RELEASE ".nfo", NULL);

		g_assert_true(g_file_test(nfo, G_FILE_TEST_IS_REGULAR));
	}
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	usenet_job_clear(&job);
}

/* Spec (import): Imports recorded before are looked at again.  A row as the
 * previous release left it (the client's removal done, the folder still
 * there, and the migration's "pending"): the cleanup deletes the folder
 * without asking the client again. */
static void
test_usenet_folder_revisited(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	usenet_imported(fx, TRUE, &job);
	/* As the previous release recorded it, with the job gone from SABnzbd. */
	axis_test_download_fake_drop(fx->fake, "SABnzbd_nzo_lantern");
	exec(fx, "UPDATE import SET client_removal = 'done', client_removal_text = NULL, folder_removal = 'pending',"
		" folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	g_assert_true(g_file_test(job.dir, G_FILE_TEST_IS_DIR));
	cleanup(fx);
	assert_folder(fx, job.id, "done", "was deleted");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	/* The client was not asked to remove anything again. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* A row whose folder is gone already is recorded so. */
	exec(fx, "UPDATE import SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		job.id);
	cleanup(fx);
	assert_folder(fx, job.id, "gone", "already gone");
	usenet_job_clear(&job);
}

/* ── A Usenet job reported by its file ──────────────────────────── */

#define JOB_FILE  LANTERN_RELEASE "/" LANTERN_RELEASE ".mkv"

/* Spec (import): SABnzbd reports a job of one file by the file inside the
 * job's folder (seen on a real server): the job's folder goes, with what the
 * client left beside the file. */
static void
test_usenet_file_in_job_folder(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	UsenetJob job;

	usenet_imported_at(fx, FALSE, JOB_FILE, JOB_FILE, &job);
	/* The import took its files from the job's folder around the file the
	 * client reported (fix-first-torrent), as for a job reported by its
	 * folder: the subtitle beside the film went into the library with it. */
	assert_qf(fx, "downloads/axis/" LANTERN_RELEASE, "SELECT source_path FROM import WHERE id = %" G_GINT64_FORMAT,
		job.id);
	assert_qf(fx, "1", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND kind = 'subtitle'"
		" AND accepted AND dest_name IS NOT NULL", job.id);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	/* The film, the sample, the subtitle, the .nfo, the par2 file, Proof and
	 * what is in it. */
	assert_folder(fx, job.id, "done", "Its folder at the client (" SAB_JOB ") was deleted, with the 7 files and "
		"folders the client left in it.");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	usenet_job_clear(&job);
}

/* The file deeper in the job's folder: still the job's folder, the first
 * part beneath the category's. */
static void
test_usenet_file_in_subfolder(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	UsenetJob job;

	usenet_imported_at(fx, FALSE, LANTERN_RELEASE "/Feature/" LANTERN_RELEASE ".mkv",
		LANTERN_RELEASE "/Feature/" LANTERN_RELEASE ".mkv", &job);
	/* Feature and the film in it, the sample, the subtitle, the .nfo, the
	 * par2 file, Proof and what is in it. */
	assert_folder(fx, job.id, "done", "Its folder at the client (" SAB_JOB ") was deleted, with the 8 files and "
		"folders");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	usenet_job_clear(&job);
}

/* The file straight in the category's folder: that file alone goes, and the
 * category's folder and another job's file in it stay. */
static void
test_usenet_file_in_category(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *other = g_build_filename(category, OTHER_JOB_FILE, NULL);
	UsenetJob job;

	usenet_imported_at(fx, FALSE, LANTERN_RELEASE ".mkv", LANTERN_RELEASE ".mkv", &job);
	/* No job's folder around it: the import took the file alone, as before. */
	assert_qf(fx, "downloads/axis/" LANTERN_RELEASE ".mkv", "SELECT source_path FROM import WHERE id = %"
		G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "done", "Its file at the client (" SAB_CATEGORY "/" LANTERN_RELEASE ".mkv), which the "
		"client put straight into the folder of Axis's category, was deleted; that folder stays.");
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(other, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	usenet_job_clear(&job);
}

/* Another file at the reported file's name (another inode): not the one
 * imported, so its folder is left; straight in the category's folder, the
 * file is left. */
static void
test_usenet_file_replaced(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	UsenetJob job;

	usenet_imported_at(fx, TRUE, JOB_FILE, JOB_FILE, &job);
	assert_folder(fx, job.id, "pending", NULL);
	g_assert_cmpint(g_unlink(job.film), ==, 0);
	write_media(job.film, FILM_LINE, 4096, 9);
	before = axis_test_storage_digest(job.dir);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "Its folder was left where it is: the file Axis imported from it is not in it "
		"any more");
	after = axis_test_storage_digest(job.dir);
	g_assert_cmpstr(after, ==, before);
	usenet_job_clear(&job);
}

static void
test_usenet_loose_file_replaced(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	usenet_imported_at(fx, TRUE, LANTERN_RELEASE ".mkv", LANTERN_RELEASE ".mkv", &job);
	g_assert_cmpint(g_unlink(job.film), ==, 0);
	write_media(job.film, FILM_LINE, 4096, 9);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "Its file was left where it is: the file there is not the one Axis imported");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	usenet_job_clear(&job);
}

/* Spec (import): the read-only wait, a job reported by its file: nothing is
 * deleted while storage is read-only, the page says why, and the job's
 * folder goes once it is writable. */
static void
test_usenet_file_waits(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	UsenetJob job;

	usenet_imported_at(fx, TRUE, JOB_FILE, JOB_FILE, &job);
	before = axis_test_storage_digest(job.dir);
	set_read_only(fx, TRUE);
	cleanup(fx);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "waiting", "Read-only storage is on");
	assert_folder(fx, job.id, "waiting", "tried again every hour");
	after = axis_test_storage_digest(job.dir);
	g_assert_cmpstr(after, ==, before);
	set_read_only(fx, FALSE);
	cleanup(fx);
	assert_folder(fx, job.id, "done", "Its folder at the client (" SAB_JOB ") was deleted");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	usenet_job_clear(&job);
}

/* The reported file gone, its job's folder too: gone; the folder still
 * there without the file: left. */
static void
test_usenet_file_gone(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	usenet_imported_at(fx, TRUE, JOB_FILE, JOB_FILE, &job);
	g_assert_cmpint(g_unlink(job.film), ==, 0);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "not in it any more");
	g_assert_true(g_file_test(job.dir, G_FILE_TEST_IS_DIR));
	remove_tree(job.dir);
	exec(fx, "UPDATE import SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		job.id);
	cleanup(fx);
	assert_folder(fx, job.id, "gone", "Its folder at the client was already gone.");
	usenet_job_clear(&job);
}

/* Spec (import): a file reported inside a job's folder whose client cannot
 * say where it keeps the category when the import runs: the job's folder
 * cannot be established, so the file alone is imported, as before. */
static void
test_usenet_file_job_folder_unknown(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	usenet_imported_full(fx, FALSE, JOB_FILE, JOB_FILE, NULL, &job);
	assert_qf(fx, "downloads/axis/" JOB_FILE, "SELECT source_path FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_qf(fx, "0", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND kind = 'subtitle'",
		job.id);
	/* And its folder was left, the client keeping the category in no folder
	 * of its own then. */
	assert_folder(fx, job.id, "left", "no folder Axis may delete in");
	usenet_job_clear(&job);
}

/* ── A torrent gone from its client (fix-first-torrent) ─────────── */

/* The torrent client's folders: /torrents/done is its folder for finished
 * torrents (downloads/done on the volume, the path map taking /torrents to
 * downloads), and /torrents/done/axis the folder of Axis's label when it has
 * one. */
#define TOR_ROOT   "/torrents"
#define TOR_DONE   TOR_ROOT "/done"
#define TOR_LABEL  TOR_DONE "/axis"
#define TOR_REF    "torrent-lantern"

typedef struct
{
	gint64  movie;
	gint64  download;
	gint64  id;
	gchar  *place;		/* the torrent's folder or file on the volume */
	gchar  *film;		/* the film's file there */
	gchar  *placed;		/* the film's file in the library */
} TorrentJob;

static void
torrent_job_clear(TorrentJob *job)
{
	g_free(job->place);
	g_free(job->film);
	g_free(job->placed);
}

/*
 * A torrent of the made-up film, imported, whose client no longer has it
 * when Axis asks for its removal (a plugin of the client removed it and left
 * its files).  It is at @below beneath the client's folder for finished
 * torrents: a folder with the film, a sample, a subtitle and an .nfo
 * (@folder), or the film's one file.  The client keeps Axis's label in
 * @label_dir (%NULL: in no folder of its own).  With @client_down the client
 * does not answer while the import ends, so its removal fails and is asked
 * again by the hourly cleanup.
 */
static void
torrent_imported(
	Fixture     *fx,
	const gchar *label_dir,
	const gchar *below,
	gboolean     folder,
	gboolean     client_down,
	TorrentJob  *job
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *client_path = g_strconcat(TOR_DONE "/", below, NULL);
	g_autofree gchar *rel_path = g_strconcat("downloads/done/", below, NULL);

	memset(job, 0, sizeof(*job));
	job->movie = add_film(fx, LANTERN_ID);
	job->place = g_build_filename(fx->downloads, "done", below, NULL);
	job->placed = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUP.mkv",
		NULL);
	if (folder) {
		g_autofree gchar *sample = g_build_filename(job->place, LANTERN_RELEASE "-sample.mkv", NULL);
		g_autofree gchar *sub = g_build_filename(job->place, LANTERN_RELEASE ".en.srt", NULL);
		g_autofree gchar *nfo = g_build_filename(job->place, LANTERN_RELEASE ".nfo", NULL);

		job->film = g_build_filename(job->place, LANTERN_RELEASE ".mkv", NULL);
		write_media(job->film, FILM_LINE, 256 * 1024, 5);
		write_media(sample, SAMPLE_LINE, 8 * 1024, 6);
		write_text(sub, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n");
		write_text(nfo, "an nfo\n");
	} else {
		job->film = g_strdup(job->place);
		write_media(job->film, FILM_LINE, 256 * 1024, 5);
	}
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, TOR_ROOT, fx->volume_id, "downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(fx->fake, label_dir);
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, job->movie);
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'Torrents', '" TOR_REF "', 'torrent', 'magnet', '%s', %s,"
		" 'Marrowfield Lanterns (2011)', 'movies', 'completed', 1000, 1000, '%s', %" G_GINT64_FORMAT ", '%s', now(),"
		" now()) RETURNING id", fx->client_id, LANTERN_RELEASE, title, client_path, fx->volume_id, rel_path);
	job->download = g_ascii_strtoll(download, NULL, 10);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '%s', 'torrent', lower('1:0:%s'), %" G_GINT64_FORMAT ", now(),"
		" now()) RETURNING id", job->movie, LANTERN_RELEASE, LANTERN_RELEASE, job->download);
	g_assert_nonnull(grab);
	job->id = request(fx, job->download);
	if (client_down)
		axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	g_assert_cmpstr(run(fx, job->id, NULL), ==, "imported");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_true(g_file_test(job->placed, G_FILE_TEST_IS_REGULAR));
}

static void
assert_removal(
	Fixture     *fx,
	gint64       id,
	const gchar *state,
	const gchar *words
){
	g_autofree gchar *now = q(fx, "SELECT coalesce(client_removal, '-') FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_autofree gchar *text = q(fx, "SELECT client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, id);

	if (g_strcmp0(now, state) != 0)
		g_error("import %" G_GINT64_FORMAT ": the client removal is %s, not %s (%s)", id, now, state,
			text != NULL ? text : "");
	if (words != NULL)
		assert_contains(text, words);
}

/* Spec (import): a torrent already gone from its client is "gone", not a
 * removal that failed; inside the client's folder for Axis's label, what it
 * left is deleted under the rules of a Usenet job's folder, and the library's
 * file keeps its own name. */
static void
test_torrent_gone_label_folder(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	torrent_imported(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, TRUE, FALSE, &job);
	assert_removal(fx, job.id, "gone", "The torrent was already gone from its client");
	assert_removal(fx, job.id, "gone", "deleted none of its files now");
	/* Asked once, never again. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", TOR_REF), ==, 1);
	assert_folder(fx, job.id, "done", "Its folder at the client (" TOR_LABEL "/" LANTERN_RELEASE ") was deleted, with "
		"the 4 files and folders the torrent left in it.");
	g_assert_false(g_file_test(job.place, G_FILE_TEST_EXISTS));
	{
		g_autofree gchar *label = g_build_filename(fx->downloads, "done", "axis", NULL);

		g_assert_true(g_file_test(label, G_FILE_TEST_IS_DIR));
	}
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	cleanup(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", TOR_REF), ==, 1);
	torrent_job_clear(&job);
}

/* Spec (import): the client keeps Axis's torrents in no folder of their own:
 * a torrent's folder is left, and the page says why and where it is. */
static void
test_torrent_gone_no_own_folder(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	TorrentJob job;

	torrent_imported(fx, NULL, LANTERN_RELEASE, TRUE, TRUE, &job);
	before = axis_test_storage_digest(job.place);
	cleanup(fx);
	assert_removal(fx, job.id, "gone", "already gone");
	assert_folder(fx, job.id, "left", "Its folder was left where it is: its client keeps Axis's torrents in no folder "
		"of their own");
	assert_folder(fx, job.id, "left", "It is at " TOR_DONE "/" LANTERN_RELEASE " at the client");
	assert_folder(fx, job.id, "left", "cannot be told from another of that name");
	after = axis_test_storage_digest(job.place);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 2);
	/* Left is final. */
	cleanup(fx);
	assert_folder(fx, job.id, "left", NULL);
	torrent_job_clear(&job);
}

/* Spec (import): with no folder of Axis's own, a torrent of one file has that
 * file deleted alone, by its inode, while the library still has its copy. */
static void
test_torrent_gone_one_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *done = NULL;
	g_autofree gchar *neighbour = NULL;
	TorrentJob job;

	done = g_build_filename(fx->downloads, "done", NULL);
	neighbour = g_build_filename(done, "Quillfeather.Junction.2014-MADEUP.mkv", NULL);
	write_text(neighbour, "another program's download\n");
	torrent_imported(fx, NULL, LANTERN_RELEASE ".mkv", FALSE, FALSE, &job);
	assert_removal(fx, job.id, "gone", NULL);
	assert_folder(fx, job.id, "done", "Its file at the client (" TOR_DONE "/" LANTERN_RELEASE ".mkv) was deleted: the "
		"torrent's one file, the very one imported");
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(neighbour, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(done, G_FILE_TEST_IS_DIR));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	torrent_job_clear(&job);
}

/* The same, the library's copy gone meanwhile: the download's file may be the
 * one copy left, and is kept, saying so.  Also a removal recorded as failed
 * (the client then answering as Deluge 2.2.0 did, recorded before this
 * change) is asked again by the hourly cleanup, and is gone, not failed. */
static void
test_torrent_gone_library_copy_missing(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	torrent_imported(fx, NULL, LANTERN_RELEASE ".mkv", FALSE, TRUE, &job);
	assert_removal(fx, job.id, "failed", "tried again every hour");
	/* As the earlier release recorded Deluge's answer. */
	exec(fx, "UPDATE import SET client_removal_text = 'Its client could not remove it yet (tried again every hour):"
		" Deluge refused it: Failure: [Failure instance: Traceback (failure with no frames): <class"
		" ''deluge.error.InvalidTorrentError''>: torrent_id 0123456789abcdef0123456789abcdef01234567 not in session. ]'"
		" WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_qf(fx, "-", "SELECT coalesce(folder_removal, '-') FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	g_assert_cmpint(g_unlink(job.placed), ==, 0);
	cleanup(fx);
	assert_removal(fx, job.id, "gone", "The torrent was already gone from its client");
	assert_folder(fx, job.id, "left", "Its file was left where it is: the file the import placed in the library");
	assert_folder(fx, job.id, "left", "is not there any more");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", TOR_REF), ==, 1);
	torrent_job_clear(&job);
}

/* Spec (import): nothing of a torrent is deleted while storage is read-only
 * (it waits, saying so), nor once the settings leave torrents seeding; and a
 * torrent outside the folder of Axis's label is left. */
static void
test_torrent_gone_waits_and_seed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	TorrentJob job;

	torrent_imported(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, TRUE, TRUE, &job);
	before = axis_test_storage_digest(job.place);
	set_read_only(fx, TRUE);
	cleanup(fx);
	assert_removal(fx, job.id, "gone", NULL);
	assert_folder(fx, job.id, "waiting", "What the torrent left at the client is still there");
	assert_folder(fx, job.id, "waiting", "tried again every hour");
	set_read_only(fx, FALSE);
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "seed");
	cleanup(fx);
	assert_folder(fx, job.id, "left", "leave torrents seeding");
	after = axis_test_storage_digest(job.place);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 2);

	/* Outside the label's folder: left, by the rule of the category. */
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "remove");
	exec(fx, "UPDATE import SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		job.id);
	axis_test_download_fake_set_category_dir(fx->fake, TOR_DONE "/other");
	cleanup(fx);
	assert_folder(fx, job.id, "left", "not inside the folder of Axis's category");
	g_free(after);
	after = axis_test_storage_digest(job.place);
	g_assert_cmpstr(after, ==, before);
	torrent_job_clear(&job);
}

/* ── A download removed with its data outside an import ─────────── */

#define GIVEN_REF "SABnzbd_nzo_given"

/*
 * A Usenet job of the made-up film that completed at the client in its own
 * folder inside the category's (with what SABnzbd leaves beside the film),
 * recorded as tracking records it (with the inode of that folder when
 * @with_inode, as tracking now does), and not imported: the job a person
 * gives up on.  The fake client keeps a completed job's files when it is
 * removed with them, as SABnzbd does.  With @located FALSE the client never
 * said where it was (it was still downloading as far as Axis saw).
 */
static gint64
completed_job(
	Fixture  *fx,
	gboolean  located,
	gboolean  with_inode,
	gchar   **dir_out
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_build_filename(fx->downloads, "axis", LANTERN_RELEASE, NULL);
	g_autofree gchar *inode = NULL;
	g_autofree gchar *download = NULL;

	g_autofree gchar *mapped = q(fx, "SELECT count(*) FROM download_path_map WHERE client_id = %" G_GINT64_FORMAT
		" AND client_prefix = '" SAB_COMPLETE "'", fx->client_id);

	write_job(fx, LANTERN_RELEASE "/" LANTERN_RELEASE ".mkv");
	if (g_strcmp0(mapped, "0") == 0) {
		g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, SAB_COMPLETE, fx->volume_id, "downloads",
			&error));
		g_assert_no_error(error);
	}
	axis_test_download_fake_keep_completed(fx->fake, TRUE);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);
	axis_test_download_fake_put(fx->fake, GIVEN_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000, SAB_JOB,
		NULL);
	inode = with_inode ? g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)stat_of(dir).st_ino) : g_strdup("NULL");
	download = located ?
		q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, title_label,"
			" module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, location_inode, sent_at,"
			" completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', '" GIVEN_REF "', 'usenet', 'file', '%s',"
			" 'Marrowfield Lanterns (2011)', 'movies', 'completed', 1000, 1000, '" SAB_JOB "', %" G_GINT64_FORMAT ","
			" '" JOB_REL "', %s, now(), now()) RETURNING id", fx->client_id, LANTERN_RELEASE, fx->volume_id, inode) :
		q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, title_label,"
			" module, state, size_bytes, done_bytes, sent_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', '" GIVEN_REF "',"
			" 'usenet', 'file', '%s', 'Marrowfield Lanterns (2011)', 'movies', 'downloading', 1000, 650, now())"
			" RETURNING id", fx->client_id, LANTERN_RELEASE);
	if (dir_out != NULL)
		*dir_out = g_steal_pointer(&dir);
	return g_ascii_strtoll(download, NULL, 10);
}

/* As a person giving up on it does: removed from its client with its data. */
static void
give_up(
	Fixture *fx,
	gint64   download
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_remove(fx->dl, download, TRUE, &error));
	g_assert_no_error(error);
	assert_qf(fx, "removed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
}

static void
assert_download_files(
	Fixture     *fx,
	gint64       download,
	const gchar *state,
	const gchar *words
){
	g_autofree gchar *now = q(fx, "SELECT coalesce(folder_removal, '-') FROM download WHERE id = %" G_GINT64_FORMAT,
		download);
	g_autofree gchar *text = q(fx, "SELECT folder_removal_text FROM download WHERE id = %" G_GINT64_FORMAT, download);

	if (g_strcmp0(now, state) != 0)
		g_error("download %" G_GINT64_FORMAT ": its files are %s, not %s (%s)", download, now, state,
			text != NULL ? text : "");
	if (words != NULL)
		assert_contains(text, words);
}

/* Spec (import): a Usenet job given up on after it completed: SABnzbd keeps
 * its folder, and the hourly cleanup deletes it under the folder rules, the
 * folder being the very one the client reported when the job completed. */
static void
test_given_up_job_folder_removed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	gint64 download = completed_job(fx, TRUE, TRUE, &dir);

	give_up(fx, download);
	/* SABnzbd kept it, and its leftover waits for the cleanup. */
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-kept-files", GIVEN_REF), ==, 1);
	g_assert_true(g_file_test(dir, G_FILE_TEST_IS_DIR));
	assert_download_files(fx, download, "pending", NULL);
	cleanup(fx);
	assert_download_files(fx, download, "done", "Its folder at the client (" SAB_JOB ") was deleted, with the 7 files "
		"and folders the client left in it.");
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	/* Once. */
	cleanup(fx);
	assert_download_files(fx, download, "done", NULL);
}

/* Spec (import): nothing of a given-up download is deleted without the place
 * its client reported when it completed and what was there then, without a
 * folder of Axis's own, or when something else is at its place now; the
 * download's line says why and where. */
static void
test_given_up_job_refusals(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *nfo = NULL;
	gint64 download;

	/* Never said where it was. */
	download = completed_job(fx, FALSE, FALSE, &dir);
	nfo = g_build_filename(dir, LANTERN_RELEASE ".nfo", NULL);
	give_up(fx, download);
	cleanup(fx);
	assert_download_files(fx, download, "left", "its client never said where its files were");
	assert_download_files(fx, download, "left", "in the client's folder for Axis's category, under the download's name");
	exec(fx, "DELETE FROM download WHERE id = %" G_GINT64_FORMAT, download);

	/* Located, but nothing recorded of what was there (as recorded before
	 * this change). */
	download = completed_job(fx, TRUE, FALSE, NULL);
	give_up(fx, download);
	cleanup(fx);
	assert_download_files(fx, download, "left", "Axis recorded nothing of what was at the place");
	exec(fx, "DELETE FROM download WHERE id = %" G_GINT64_FORMAT, download);

	/* No folder of Axis's own at the client: left, saying where. */
	download = completed_job(fx, TRUE, TRUE, NULL);
	give_up(fx, download);
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	cleanup(fx);
	assert_download_files(fx, download, "left", "no folder Axis may delete in");
	assert_download_files(fx, download, "left", "it is at " SAB_JOB " at the client");
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);

	/* Another folder of that name now: not what the client reported. */
	exec(fx, "UPDATE download SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		download);
	moved = g_strconcat(dir, ".old", NULL);
	g_assert_cmpint(g_rename(dir, moved), ==, 0);
	g_assert_cmpint(g_mkdir(dir, 0755), ==, 0);
	cleanup(fx);
	assert_download_files(fx, download, "left", "not what the client reported there when the download completed");
	g_assert_true(g_file_test(dir, G_FILE_TEST_IS_DIR));
	g_assert_cmpint(g_rmdir(dir), ==, 0);
	g_assert_cmpint(g_rename(moved, dir), ==, 0);
	/* Through all of it the job's folder and its files stayed. */
	g_assert_true(g_file_test(nfo, G_FILE_TEST_IS_REGULAR));
	exec(fx, "DELETE FROM download WHERE id = %" G_GINT64_FORMAT, download);

	/* Read-only: waits, then goes. */
	download = completed_job(fx, TRUE, TRUE, NULL);
	give_up(fx, download);
	set_read_only(fx, TRUE);
	cleanup(fx);
	assert_download_files(fx, download, "waiting", "Read-only storage is on");
	set_read_only(fx, FALSE);
	cleanup(fx);
	assert_download_files(fx, download, "done", "was deleted");
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
}

/* A removal that keeps the files, and a torrent's (its client deletes them):
 * nothing is for Axis to look at.  A torrent its client no longer had: its
 * leftover by the torrent rules, never without a folder of Axis's own. */
static void
test_removed_downloads_left_alone(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *torrent_dir = g_build_filename(fx->downloads, "done", "axis", LANTERN_RELEASE, NULL);
	g_autofree gchar *torrent_film = g_build_filename(torrent_dir, LANTERN_RELEASE ".mkv", NULL);
	g_autofree gchar *torrent = NULL;
	g_autofree gchar *inode = NULL;
	gint64 download = completed_job(fx, TRUE, TRUE, &dir);
	gint64 id;

	/* Without its data: the person kept the files. */
	g_assert_true(axis_downloads_remove(fx->dl, download, FALSE, &error));
	g_assert_no_error(error);
	assert_download_files(fx, download, "-", NULL);
	cleanup(fx);
	g_assert_true(g_file_test(dir, G_FILE_TEST_IS_DIR));

	/* A torrent removed with its data: its client deleted them. */
	write_media(torrent_film, FILM_LINE, 4096, 3);
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, TOR_ROOT, fx->volume_id, "downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
		TOR_LABEL "/" LANTERN_RELEASE, NULL);
	inode = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)stat_of(torrent_dir).st_ino);
	torrent = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" client_path, volume_id, rel_path, location_inode) VALUES (%" G_GINT64_FORMAT ", 'Torrents', '" TOR_REF "',"
		" 'torrent', 'magnet', '%s', 'completed', '" TOR_LABEL "/" LANTERN_RELEASE "', %" G_GINT64_FORMAT ","
		" 'downloads/done/axis/" LANTERN_RELEASE "', %s) RETURNING id", fx->client_id, LANTERN_RELEASE, fx->volume_id,
		inode);
	id = g_ascii_strtoll(torrent, NULL, 10);
	give_up(fx, id);
	assert_download_files(fx, id, "-", NULL);

	/* A torrent its client no longer had: gone, and its leftover inside the
	 * client's folder for Axis's label is deleted. */
	exec(fx, "UPDATE download SET state = 'completed' WHERE id = %" G_GINT64_FORMAT, id);
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	g_assert_false(axis_downloads_remove(fx->dl, id, TRUE, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE);
	g_clear_error(&error);
	assert_qf(fx, "gone", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);
	assert_download_files(fx, id, "pending", NULL);
	/* With no folder of Axis's own: left, saying where. */
	cleanup(fx);
	assert_download_files(fx, id, "left", "it is at " TOR_LABEL "/" LANTERN_RELEASE " at the client");
	g_assert_true(g_file_test(torrent_film, G_FILE_TEST_IS_REGULAR));
	/* With the label's folder. */
	exec(fx, "UPDATE download SET folder_removal = 'pending', folder_removal_text = NULL WHERE id = %" G_GINT64_FORMAT,
		id);
	axis_test_download_fake_set_category_dir(fx->fake, TOR_LABEL);
	cleanup(fx);
	assert_download_files(fx, id, "done", "Its folder at the client (" TOR_LABEL "/" LANTERN_RELEASE ") was deleted");
	g_assert_false(g_file_test(torrent_dir, G_FILE_TEST_EXISTS));
}

static void
old_hearth(
	Fixture *fx,
	gint64   movie
);

/* The media servers are told which folder changed: one row for each server
 * that is set up, for the movie's folder, once for a film with its subtitle,
 * and the import goes through with the servers unreachable. */
static void
test_media_servers_are_told_the_folder(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	gint64 movie = add_film(fx, "107");
	gint64 id;

	assert_qf(fx, "0", "SELECT count(*) FROM compat_notify");
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_SETTING_ADDRESS, "http://127.0.0.1:9", NULL));
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_SETTING_TOKEN, "plex-token-0123456789", NULL));
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, "http://127.0.0.1:9", NULL));
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_JELLYFIN_TOKEN, "jf-key-0123456789", NULL));
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-notify", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	/* Two files placed (the film and its subtitle), one row per server. */
	assert_qf(fx, "2", "SELECT count(*) FROM compat_notify WHERE state = 'pending' AND change = 'created'"
		" AND events = 1 AND folder = '%s'", folder);
	assert_qf(fx, "jellyfin,plex", "SELECT string_agg(server_kind, ',' ORDER BY server_kind) FROM compat_notify");
	/* Not before the quiet time. */
	assert_qf(fx, "2", "SELECT count(*) FROM compat_notify WHERE due_at > now() + interval '20 seconds'");

	/* Switched off for one server: it gets no row for the next import. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_SETTING_NOTIFY_PLEX, "false", NULL));
	exec(fx, "DELETE FROM compat_notify");
	g_assert_true(axis_compat_notify_record(fx->t->pool, fx->settings, folder, AXIS_COMPAT_CHANGE_MODIFIED, NULL));
	assert_qf(fx, "jellyfin", "SELECT string_agg(server_kind, ',') FROM compat_notify");
}

/* An upgrade is a change to the folder, not a new one. */
static void
test_upgrade_tells_the_servers_modified(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	gint64 id;

	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_JELLYFIN_ADDRESS, "http://127.0.0.1:9", NULL));
	g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_JELLYFIN_TOKEN, "jf-key-0123456789", NULL));
	old_hearth(fx, movie);
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-notify-up", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	assert_qf(fx, "1", "SELECT count(*) FROM compat_notify WHERE change = 'modified' AND server_kind = 'jellyfin'");
}

/* The recycle period is two days unless an administrator set another. */
static void
test_recycle_default(Fixture *fx, gconstpointer data)
{
	AxisImportSettings settings;

	axis_import_settings_read(fx->import, &settings);
	g_assert_cmpint(settings.retention_s, ==, 2 * 86400);
	g_assert_true(settings.torrents_remove);
	axis_import_settings_clear(&settings);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, AXIS_IMPORT_SETTING_RETENTION), ==, 2 * 86400);

	set(fx, AXIS_IMPORT_SETTING_RETENTION, "7d");
	axis_import_settings_read(fx->import, &settings);
	g_assert_cmpint(settings.retention_s, ==, 7 * 86400);
	axis_import_settings_clear(&settings);
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "seed");
	axis_import_settings_read(fx->import, &settings);
	g_assert_false(settings.torrents_remove);
	axis_import_settings_clear(&settings);
}

/* ── 6.3 A failure at each step ─────────────────────────────────── */

static void
test_faults(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = { "folder", "temp", "copied", NULL };
	guint i;

	for (i = 0; steps[i] != NULL; i++) {
		g_autofree gchar *release = g_strdup_printf("Hearth.1995.1080p.BluRay.x264-GRP%u", i);
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		g_autofree gchar *message = NULL;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, "107");
		id = hearth_import(fx, movie, "torrent", release, release, NULL);
		before = tree(fx->films);
		hook_at(fx, steps[i], AXIS_IMPORT_HOOK_FAIL);
		g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
		assert_state(fx, id, "waiting/place_failed");
		message = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_contains(message, "nothing was left");
		/* The library exactly as it was: no folder, no temporary name. */
		after = tree(fx->films);
		g_assert_cmpstr(after, ==, before);
		g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		assert_qf(fx, "0", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND step IS NOT NULL", id);
		assert_qf(fx, "0", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
		/* It is tried again, and then it goes. */
		hook_reset(fx);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		{
			g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);

			g_assert_cmpuint(count_videos(folder), ==, 1);
			/* For the next round. */
			clear_film(fx, folder);
		}
	}

	/* A failure before anything is placed (the decision recorded, then a
	 * database error, say): the run fails, nothing is written, and the job
	 * runs again. */
	{
		g_autofree gchar *before = tree(fx->films);
		g_autofree gchar *after = NULL;
		g_autofree gchar *message = NULL;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, "107");
		id = hearth_import(fx, movie, "torrent", "Hearth.1995.1080p.BluRay.x264-GRPX", "grpx", NULL);
		hook_at(fx, "decided", AXIS_IMPORT_HOOK_FAIL);
		message = run_failing(fx, id);
		assert_contains(message, "decided");
		after = tree(fx->films);
		g_assert_cmpstr(after, ==, before);
		hook_reset(fx);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	}
}

/* ── 6.4 A run that dies, and the next ──────────────────────────── */

static void
test_crash(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = { "decided", "folder", "temp", "copied", "published", "linked", "recorded",
		NULL };
	guint i;

	for (i = 0; steps[i] != NULL; i++) {
		g_autofree gchar *release = g_strdup_printf("Hearth.1995.1080p.BluRay.x264-GRP%u", i);
		g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
		g_autofree gchar *message = NULL;
		gint64 movie;
		gint64 download = 0;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, "107");
		id = hearth_import(fx, movie, "torrent", release, release, &download);
		hook_at(fx, steps[i], AXIS_IMPORT_HOOK_CRASH);
		message = run_failing(fx, id);
		assert_contains(message, steps[i]);
		/* The next run carries on from what is on disk. */
		hook_reset(fx);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		assert_state(fx, id, "imported");
		/* Once: one file, one link to the film, one event, no temporary name. */
		g_assert_cmpuint(count_videos(folder), ==, 1);
		g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
			" AND kind IN ('imported', 'upgraded')", movie);
		assert_qf(fx, "false", "SELECT upgrade::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
		assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT
			" AND kind = 'video'", fx->root_id);
		/* The film and its subtitle. */
		g_assert_cmpuint(count_names(folder, "Hearth (1995)"), ==, 2);
		clear_film(fx, folder);
	}
}

/* ── 6.5 Upgrades ───────────────────────────────────────────────── */

#define OLD_FILE "Hearth (1995) {tmdb-6001} [WEB-DL 1080p]-OLD.mkv"
#define OLD_SUB  "Hearth (1995) {tmdb-6001} [WEB-DL 1080p]-OLD.en.srt"

/* An older file of Hearth in the library, indexed and linked to the film. */
static void
old_hearth(
	Fixture *fx,
	gint64   movie
){
	g_autofree gchar *film = hearth_path(fx, OLD_FILE);
	g_autofree gchar *sub = hearth_path(fx, OLD_SUB);
	g_autofree gchar *file_id = NULL;

	write_media(film, FILM_LINE, 128 * 1024, 9);
	write_text(sub, "1\n00:00:01,000 --> 00:00:02,000\nOld.\n");
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path LIKE '%%-OLD.mkv'",
		fx->root_id);
	g_assert_nonnull(file_id);
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, release_name) VALUES (%s, %" G_GINT64_FORMAT ","
		" 'webdl_1080p', 'Hearth.1995.1080p.WEB-DL.x264-OLD')", file_id, movie);
}

/* import-from-arr, task 3.8: what an upgrade does with a movie whose file
 * lies in a folder Axis did not name (Radarr named it) and whose link was
 * brought over.  The new file goes into the folder of Axis's own naming, and
 * the Radarr-named file is retired by the ordinary retirement, wherever it
 * lies: the movie is never left with two copies, and nothing is deleted
 * outright.  What else was in Radarr's folder stays there, untouched. */
#define RADARR_FOLDER "Hearth 1995 Remastered"
#define RADARR_FILE   "Hearth.1995.WEB-DL.1080p.mkv"

static void
test_upgrade_of_a_brought_file(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *old_path = g_build_filename(fx->films, RADARR_FOLDER, RADARR_FILE, NULL);
	g_autofree gchar *old_sub = g_build_filename(fx->films, RADARR_FOLDER, "Hearth.1995.WEB-DL.1080p.en.srt", NULL);
	g_autofree gchar *extra = g_build_filename(fx->films, RADARR_FOLDER, "movie.nfo", NULL);
	g_autofree gchar *radarr_folder = g_build_filename(fx->films, RADARR_FOLDER, NULL);
	g_autofree gchar *axis_folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "films", RADARR_FOLDER, RADARR_FILE, NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
	g_autofree gchar *file_id = NULL;
	AxisImportReport report;
	GStatBuf old_st;
	gint64 id;

	write_media(old_path, FILM_LINE, 128 * 1024, 9);
	write_text(old_sub, "1\n00:00:01,000 --> 00:00:02,000\nOld.\n");
	write_text(extra, "<movie/>\n");
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path LIKE '%%/"
		RADARR_FILE "'", fx->root_id);
	g_assert_nonnull(file_id);
	/* The link as bringing over makes it (no import, no identity, the mark
	 * "brought") and as it is once a person let it go with "Let Axis upgrade
	 * these files": not kept.  (Bringing over records it kept, as
	 * decided on 2026-11-21: tests/test-web-arr.c.) */
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, release_name, hand_how, hand_by, hand_at, kept)"
		" VALUES (%s, %" G_GINT64_FORMAT ", 'webdl_1080p', 'Hearth.1995.1080p.WEB-DL.x264-OLD', 'brought', 'root', now(),"
		" false)", file_id, movie);
	old_st = stat_of(old_path);
	/* Let go: it is judged for upgrades as any file. */
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_UPGRADE_WANTED);

	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-brought", NULL);
	g_assert_cmpstr(run(fx, id, &report), ==, "imported");
	assert_qf(fx, "true", "SELECT upgrade::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	/* The new file is in the folder Axis names, not in Radarr's. */
	g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(count_videos(axis_folder), ==, 1);
	/* Radarr's file is retired, the same file, into the recycle folder of
	 * its disk under its own folder's name: not deleted, and not left as a
	 * second copy. */
	g_assert_false(g_file_test(old_path, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(old_sub, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, (guint64)old_st.st_ino);
	g_assert_cmpuint(count_videos(radarr_folder), ==, 0);
	g_assert_cmpuint(report.recycled, ==, 2);
	assert_qf(fx, "2", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'",
		id);
	/* One file is linked to the movie: the import's, with no mark of a
	 * person or of bringing over. */
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT " AND import_id IS NOT NULL"
		" AND hand_how IS NULL AND NOT kept", movie);
	assert_qf(fx, "0", "SELECT count(*) FROM library_file WHERE rel_path LIKE '%%/" RADARR_FILE "'");
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_IN_LIBRARY);
	/* What else Radarr's folder held stays where it was: Axis removes no
	 * folder and nothing it did not link. */
	g_assert_true(g_file_test(extra, G_FILE_TEST_IS_REGULAR));
}

/* Spec (import): an upgrade retires the old file's subtitles and nothing of
 * another video beside it (a suspicion of the stage 2 bug hunt, confirmed by
 * this test before the fix: the old file's subtitles were found by its name
 * and a dot alone, so those of another cut whose name begins with it went to
 * the recycle folder too). */
static void
test_upgrade_keeps_another_cuts_subtitles(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *old_film = hearth_path(fx, "Hearth.1995.1080p.WEB-DL-OLD.mkv");
	g_autofree gchar *old_sub = hearth_path(fx, "Hearth.1995.1080p.WEB-DL-OLD.en.srt");
	g_autofree gchar *other_film = hearth_path(fx, "Hearth.1995.1080p.WEB-DL-OLD.Extended.mkv");
	g_autofree gchar *other_sub = hearth_path(fx, "Hearth.1995.1080p.WEB-DL-OLD.Extended.en.srt");
	g_autofree gchar *file_id = NULL;
	gint64 id;

	write_media(old_film, FILM_LINE, 128 * 1024, 9);
	write_text(old_sub, "1\n00:00:01,000 --> 00:00:02,000\nOld.\n");
	write_media(other_film, FILM_LINE, 128 * 1024, 11);
	write_text(other_sub, "1\n00:00:01,000 --> 00:00:02,000\nAnother cut.\n");
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path LIKE '%%-OLD.mkv'",
		fx->root_id);
	g_assert_nonnull(file_id);
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, release_name) VALUES (%s, %" G_GINT64_FORMAT ","
		" 'webdl_1080p', 'Hearth.1995.1080p.WEB-DL.x264-OLD')", file_id, movie);
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-cut", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	/* The old file and its own subtitle retired; the other cut's subtitle,
	 * and the other cut, where they were. */
	g_assert_false(g_file_test(old_film, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(old_sub, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(other_film, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(other_sub, G_FILE_TEST_IS_REGULAR));
}

static void
test_upgrade(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *old_path = hearth_path(fx, OLD_FILE);
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *recycle = g_build_filename(fx->disk, ".axis-recycle", "films", HEARTH_FOLDER, NULL);
	g_autofree gchar *recycled = g_build_filename(recycle, OLD_FILE, NULL);
	g_autofree gchar *recycled_sub = g_build_filename(recycle, OLD_SUB, NULL);
	g_autofree gchar *old_sum = NULL;
	g_autofree gchar *contents = NULL;
	GStatBuf old_st;
	AxisImportReport report;
	AxisImportCleanupReport cleanup;
	g_autoptr(GError) error = NULL;
	gsize length = 0;
	gint64 id;

	old_hearth(fx, movie);
	old_st = stat_of(old_path);
	g_assert_true(g_file_get_contents(old_path, &contents, &length, NULL));
	old_sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_UPGRADE_WANTED);

	/* A placement that fails leaves the old file where it was, named and
	 * linked, and nothing in the recycle folder. */
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-up", NULL);
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_FAIL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
	g_assert_cmpuint((guint64)stat_of(old_path).st_ino, ==, (guint64)old_st.st_ino);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	g_assert_false(g_file_test(recycled, G_FILE_TEST_EXISTS));
	assert_qf(fx, "0", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT, id);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);

	/* Then it goes: the new file first, and the film never without one. */
	hook_reset(fx);
	fx->hook.film_dir = g_strdup(folder);
	g_assert_cmpstr(run(fx, id, &report), ==, "imported");
	g_assert_cmpuint(fx->hook.steps, >, 3);
	g_assert_cmpuint(fx->hook.without_film, ==, 0);
	g_assert_cmpuint(report.recycled, ==, 2);
	assert_qf(fx, "true", "SELECT upgrade::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	/* The old file and its subtitle in the recycle folder of the volume,
	 * the same files, the folders above them as the library's. */
	g_assert_false(g_file_test(old_path, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, (guint64)old_st.st_ino);
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(recycled, &contents, &length, NULL));
	{
		g_autofree gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);

		g_assert_cmpstr(sum, ==, old_sum);
	}
	g_assert_true(g_file_test(recycled_sub, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(count_videos(folder), ==, 1);
	{
		g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
		g_autofree gchar *placed_sub = hearth_path(fx, "Hearth (1995) {tmdb-6001} [BluRay 1080p][DTS-HD MA 5.1][x264]-GRP.en.srt");

		g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
		g_assert_true(g_file_test(placed_sub, G_FILE_TEST_IS_REGULAR));
	}
	assert_qf(fx, "2", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'"
		" AND how = 'moved' AND remove_after > now() + interval '47 hours' AND remove_after < now() + interval '49 hours'",
		id);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "0", "SELECT count(*) FROM library_file WHERE rel_path LIKE '%%-OLD.%%'");
	assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'upgraded'",
		movie);
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_IN_LIBRARY);

	/* The scanner does not see the recycle folder (a root at the volume's
	 * top would hold it), nor a temporary name. */
	g_assert_cmpint(axis_library_ignore_builtin(".axis-recycle", strlen(".axis-recycle"), TRUE), !=, AXIS_IGNORE_NONE);
	g_assert_cmpint(axis_library_ignore_builtin(".axis-partial-12", strlen(".axis-partial-12"), FALSE), !=,
		AXIS_IGNORE_NONE);

	/* Not yet due: the cleanup leaves them. */
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(cleanup.removed, ==, 0);
	g_assert_true(g_file_test(recycled, G_FILE_TEST_EXISTS));

	/* Due: the subtitle is not what was recorded any more (replaced by
	 * something else), so it is left; the film goes. */
	exec(fx, "UPDATE import_recycle SET remove_after = now() - interval '1 second' WHERE import_id = %" G_GINT64_FORMAT,
		id);
	g_assert_cmpint(g_unlink(recycled_sub), ==, 0);
	write_text(recycled_sub, "somebody put this here\n");
	set_read_only(fx, TRUE);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	/* Nothing while storage is read-only. */
	g_assert_cmpuint(cleanup.removed, ==, 0);
	g_assert_true(g_file_test(recycled, G_FILE_TEST_EXISTS));
	set_read_only(fx, FALSE);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(cleanup.removed, ==, 1);
	g_assert_cmpuint(cleanup.changed, ==, 1);
	g_assert_false(g_file_test(recycled, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(recycled_sub, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "removed", "SELECT state FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT
		" AND original_path LIKE '%%.mkv'", id);
	assert_qf(fx, "changed", "SELECT state FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT
		" AND original_path LIKE '%%.srt'", id);
	/* The new file is untouched by all this. */
	{
		g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);

		g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
	}
}

static void
test_upgrade_no_retention(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "films", HEARTH_FOLDER, OLD_FILE, NULL);
	gint64 id;

	set(fx, AXIS_IMPORT_SETTING_RETENTION, "0");
	old_hearth(fx, movie);
	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-up0", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	/* Removed as soon as the upgrade was recorded. */
	g_assert_false(g_file_test(recycled, G_FILE_TEST_EXISTS));
	assert_qf(fx, "2", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT
		" AND state = 'removed'", id);
}

/* An upgrade whose new file has the old one's name: linked into the
 * recycle folder first, then the names swapped. */
static void
test_same_name(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	const gchar *first = "Hearth.1995.1080p.WEB-DL.x264-GRP";
	const gchar *second = "Hearth.1995.1080p.BluRay.x264-GRP";
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FOLDER "-GRP.mkv");
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "films", HEARTH_FOLDER,
		HEARTH_FOLDER "-GRP.mkv", NULL);
	g_autofree gchar *new_source = NULL;
	guint64 old_ino;
	gint64 id;

	set(fx, AXIS_IMPORT_SETTING_FORMAT, "{-group}");
	id = hearth_import(fx, movie, "torrent", first, "t-same-1", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	old_ino = (guint64)stat_of(placed).st_ino;
	assert_qf(fx, "webdl_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);

	/* A run that dies after the new file's temporary name was made: the
	 * old file has two names then; the next run puts it right. */
	id = hearth_import(fx, movie, "torrent", second, "t-same-2", NULL);
	new_source = g_build_filename(fx->downloads, second, "Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL);
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, old_ino);
	g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, old_ino);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);

	hook_reset(fx);
	fx->hook.film_dir = g_strdup(folder);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint(fx->hook.without_film, ==, 0);
	/* The name leads to the new file; the old one is only in the recycle
	 * folder (under its name, and a numbered one from the first try). */
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, (guint64)stat_of(new_source).st_ino);
	g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, old_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	assert_qf(fx, "bluray_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "true", "SELECT (count(*) >= 1)::text FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT
		" AND how = 'linked' AND state = 'kept' AND remove_after IS NOT NULL", id);
	/* The index has the new file at the name. */
	assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND inode = %"
		G_GUINT64_FORMAT, fx->root_id, (guint64)stat_of(placed).st_ino);
}

/* An upgrade whose run dies after the new file is placed, linked, or the
 * old one retired: the next run retires the old file once (never the new
 * one, nor its subtitle) and records the upgrade. */
static void
test_upgrade_crash(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = { "published", "linked", "retired", NULL };
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);
	g_autofree gchar *placed_sub = hearth_path(fx, "Hearth (1995) {tmdb-6001} [BluRay 1080p][DTS-HD MA 5.1][x264]-GRP.en.srt");
	g_autofree gchar *recycle = g_build_filename(fx->disk, ".axis-recycle", "films", HEARTH_FOLDER, NULL);
	guint i;

	for (i = 0; steps[i] != NULL; i++) {
		g_autofree gchar *release = g_strdup_printf("Hearth.1995.1080p.BluRay.x264-GRP");
		g_autofree gchar *ref = g_strdup_printf("uc-%u", i);
		g_autofree gchar *recycled = g_build_filename(recycle, OLD_FILE, NULL);
		g_autofree gchar *old_path = hearth_path(fx, OLD_FILE);
		guint64 old_ino;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, "107");
		old_hearth(fx, movie);
		old_ino = (guint64)stat_of(old_path).st_ino;
		id = hearth_import(fx, movie, "torrent", release, ref, NULL);
		hook_at(fx, steps[i], AXIS_IMPORT_HOOK_CRASH);
		g_free(run_failing(fx, id));
		hook_reset(fx);
		fx->hook.film_dir = g_strdup(folder);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		g_assert_cmpuint(fx->hook.without_film, ==, 0);
		/* The new film and its subtitle in place; the old ones retired once. */
		g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
		g_assert_true(g_file_test(placed_sub, G_FILE_TEST_IS_REGULAR));
		g_assert_cmpuint(count_videos(folder), ==, 1);
		g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, old_ino);
		assert_qf(fx, "2", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT
			" AND state = 'kept' AND remove_after IS NOT NULL", id);
		assert_qf(fx, "true", "SELECT upgrade::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
			" AND kind = 'upgraded'", movie);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
		g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		/* For the next round: the folders as before the upgrade. */
		clear_film(fx, folder);
		g_assert_cmpint(g_unlink(recycled), ==, 0);
		{
			g_autofree gchar *sub = g_build_filename(recycle, OLD_SUB, NULL);

			g_assert_cmpint(g_unlink(sub), ==, 0);
		}
		exec(fx, "DELETE FROM import_recycle");
	}
}

/* A temporary name left by a run that died, of an import a person then
 * dismissed: the cleanup removes it (another name of its file exists). */
static void
test_leftover(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autoptr(GError) error = NULL;
	AxisImportCleanupReport cleanup;
	gint64 id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-left", NULL);

	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
	g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(cleanup.temporaries, ==, 1);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 0);
	/* The download's file is still there, with its one name. */
	{
		g_autofree gchar *source = g_build_filename(fx->downloads, HEARTH_RELEASE, HEARTH_RELEASE ".mkv", NULL);

		g_assert_cmpuint((guint)stat_of(source).st_nlink, ==, 1);
	}
}

/* ── 6.6 A partial file is never visible ────────────────────────── */

static void
test_partial(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	gint64 id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-part", NULL);

	/* When the file is placed and not named yet, the folder holds only its
	 * temporary dot-name. */
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	assert_contains(fx->hook.at_copied, "/.axis-partial-");
	g_assert_cmpuint(count_videos(folder), ==, 0);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
	/* A scan does not index it. */
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	assert_qf(fx, "0", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND rel_path LIKE '%%.axis-%%'", fx->root_id);
	hook_reset(fx);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
}

/* ── 6.7 The reasons ────────────────────────────────────────────── */

static gint64
odd_download(
	Fixture     *fx,
	gint64       movie,
	const gchar *release,
	const gchar *ref
){
	g_autofree gchar *rel = g_strconcat("downloads/", release, NULL);

	return request(fx, add_download(fx, movie, "torrent", release, rel, ref));
}

static gchar *
in_download(
	Fixture     *fx,
	const gchar *release,
	const gchar *name
){
	return g_build_filename(fx->downloads, release, name, NULL);
}

static void
test_reasons(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 hearth = add_film(fx, "107");
	gint64 colliery = add_film(fx, "109");
	gint64 sample = add_film(fx, "111");
	gint64 id;

	/* Two whole films. */
	{
		const gchar *r = "Hearth.1995.Double.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Hearth.1995.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *b = in_download(fx, r, "Hearth.1995.Directors.Cut.1080p.BluRay.x264-GRP.mkv");

		write_media(a, FILM_LINE, 64 * 1024, 1);
		write_media(b, FILM_LINE, 64 * 1024, 2);
		before = tree(fx->films);
		id = odd_download(fx, hearth, r, "r-several");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/several");
		assert_qf(fx, "2", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND reasons @> '[{\"c\":\"several\"}]'", id);
		assert_qf(fx, "false", "SELECT (blockers->0->>'o')::boolean::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		/* A movie whose import is blocked needs attention. */
		g_assert_cmpint(movie_state(fx, hearth), ==, AXIS_MOVIE_ATTENTION);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
			" AND kind = 'import_blocked'", hearth);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT, hearth);
	}
	/* An archive the client did not unpack. */
	{
		const gchar *r = "Hearth.1995.Packed.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "hearth.rar");
		g_autofree gchar *b = in_download(fx, r, "hearth.r00");

		write_text(a, "Rar!");
		write_text(b, "Rar!");
		id = odd_download(fx, hearth, r, "r-archive");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/archive");
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT " AND closed_at IS NULL", hearth);
	}
	/* Nothing but an .nfo. */
	{
		const gchar *r = "Hearth.1995.Empty.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "hearth.nfo");

		write_text(a, "nfo");
		id = odd_download(fx, hearth, r, "r-novideo");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/no_video");
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT " AND closed_at IS NULL", hearth);
	}
	/* A film in parts. */
	{
		const gchar *r = "Hearth.1995.Parts.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Hearth.1995.cd1.mkv");
		g_autofree gchar *b = in_download(fx, r, "Hearth.1995.cd2.mkv");

		write_media(a, "AXISFAKE duration=5100 width=1920 height=800", 32 * 1024, 1);
		write_media(b, "AXISFAKE duration=5100 width=1920 height=800", 32 * 1024, 2);
		id = odd_download(fx, hearth, r, "r-parts");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/multi_part");
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT " AND closed_at IS NULL", hearth);
	}
	/* A file ffprobe cannot read: tried again, then a person decides (and
	 * may import it anyway). */
	{
		const gchar *r = "Hearth.1995.Broken.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Hearth.1995.1080p.BluRay.x264-GRP.mkv");
		guint k;

		write_media(a, "AXISFAKE fail", 64 * 1024, 3);
		id = odd_download(fx, hearth, r, "r-unreadable");
		for (k = 0; k < 3; k++) {
			g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
			assert_state(fx, id, "waiting/probe_retry");
		}
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/unreadable");
		assert_qf(fx, "true", "SELECT (blockers->0->>'o')::boolean::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT " AND closed_at IS NULL", hearth);
	}
	/* Another film's name. */
	{
		const gchar *r = "Hearth.1995.Other.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Rokan.1998.1080p.BluRay.x264-GRP.mkv");

		write_media(a, FILM_LINE, 64 * 1024, 4);
		id = odd_download(fx, hearth, r, "r-name");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/wrong_name");
		assert_qf(fx, "true", "SELECT (blockers->0->>'o')::boolean::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %"
			G_GINT64_FORMAT " AND closed_at IS NULL", hearth);
	}
	/* Nothing of these was written. */
	after = tree(fx->films);
	g_assert_cmpstr(after, ==, before);

	/* A runtime far from the film's (it runs 120 minutes; this one 60). */
	{
		const gchar *r = "Colliery.2004.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Colliery.2004.1080p.BluRay.x264-GRP.mkv");

		write_media(a, "AXISFAKE duration=3600 width=1920 height=800 vcodec=h264", 64 * 1024, 5);
		id = odd_download(fx, colliery, r, "r-runtime");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "1", "SELECT count(*) FROM jsonb_array_elements((SELECT blockers FROM import WHERE id = %"
			G_GINT64_FORMAT ")) b WHERE b->>'c' = 'runtime' AND (b->>'o')::boolean", id);
	}
	/* A film called Sample: its file is not taken for a sample. */
	{
		const gchar *r = "Sample.2001.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Sample.2001.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *b = in_download(fx, r, "Sample.2001.1080p.BluRay.x264-GRP-sample.mkv");
		g_autofree gchar *placed = g_build_filename(fx->films, "Sample (2001) {tmdb-4321}",
			"Sample (2001) {tmdb-4321} [BluRay 1080p][x264]-GRP.mkv", NULL);

		write_media(a, "AXISFAKE duration=5400 width=1920 height=800 vcodec=h264", 64 * 1024, 6);
		write_media(b, "AXISFAKE duration=30 width=1920 height=800 vcodec=h264", 4 * 1024, 7);
		id = odd_download(fx, sample, r, "r-sample");
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, (guint64)stat_of(a).st_ino);
		assert_qf(fx, "sample", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND path LIKE '%%-sample.mkv'", id);
	}
}

/* add-manual-search: the import of a release a person set the profile
 * aside for (set_aside: Grab this one, Grab it anyway) has the profile set
 * aside (a quality it does not allow is imported); a grab marked only
 * manual (an accepted Grab, any grab before this version) has not.  It
 * replaces a file it is not better than only when the person ticked that
 * very file, at the quality shown. */
static void
test_hand_grab(Fixture *fx, gconstpointer data)
{
	const gchar *better = "Marrowfield.Lanterns.2011.1080p.BluRay.x264-GRP";
	const gchar *worse = "Marrowfield.Lanterns.2011.1080p.WEB-DL.DDP5.1.H.264-NTb";
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	gint64 film = add_film(fx, "120");
	gint64 bluray = 0;
	gint64 id;

	/* A profile that allows no 1080p. */
	exec(fx, "UPDATE movie SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug = 'uhd') WHERE id = %"
		G_GINT64_FORMAT, film);
	/* Chosen by a search: the profile objects. */
	a = in_download(fx, "auto", "Marrowfield.Lanterns.2011.1080p.BluRay.x264-GRP.mkv");
	write_media(a, FILM_LINE, 64 * 1024, 1);
	id = odd_download(fx, film, "auto", "r-auto");
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/quality");
	exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
	exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE movie_id = %" G_GINT64_FORMAT,
		film);
	/* Marked manual alone (an administrator's accepted Grab, or a grab made
	 * before this version): the profile is not set aside. */
	b = in_download(fx, better, "Marrowfield.Lanterns.2011.1080p.BluRay.x264-GRP.mkv");
	write_media(b, FILM_LINE, 64 * 1024, 2);
	{
		g_autofree gchar *rel = g_strconcat("downloads/", better, NULL);
		gint64 download = add_download(fx, film, "torrent", better, rel, "r-manual");

		exec(fx, "UPDATE movie_grab SET manual = true WHERE download_id = %" G_GINT64_FORMAT, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/quality");
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE download_id = %"
			G_GINT64_FORMAT, download);
	}
	/* Chosen by hand: imported, the profile set aside. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", better, NULL);
		gint64 download = add_download(fx, film, "torrent", better, rel, "r-hand");

		exec(fx, "UPDATE movie_grab SET manual = true, set_aside = 'hand' WHERE download_id = %" G_GINT64_FORMAT,
			download);
		id = request(fx, download);
	}
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	assert_qf(fx, "bluray_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
	{
		g_autofree gchar *file = q(fx, "SELECT file_id FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);

		bluray = g_ascii_strtoll(file, NULL, 10);
	}
	/* A WEB-DL by hand, over that BluRay: not without the tick. */
	c = in_download(fx, worse, "Marrowfield.Lanterns.2011.1080p.WEB-DL.DDP5.1.H.264-NTb.mkv");
	write_media(c, FILM_LINE, 64 * 1024, 3);
	{
		g_autofree gchar *rel = g_strconcat("downloads/", worse, NULL);
		gint64 download = add_download(fx, film, "torrent", worse, rel, "r-worse");

		exec(fx, "UPDATE movie_grab SET manual = true, set_aside = 'hand' WHERE download_id = %" G_GINT64_FORMAT,
			download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/not_upgrade");
		assert_qf(fx, "bluray_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		/* Ticked for another file, or for this one at another quality than
		 * it has: not this file's tick. */
		exec(fx, "UPDATE movie_grab SET replaces_files = '[{\"f\": %" G_GINT64_FORMAT ", \"q\": \"bluray_1080p\","
			" \"r\": null}]' WHERE download_id = %" G_GINT64_FORMAT, bluray + 1000, download);
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/not_upgrade");
		exec(fx, "UPDATE movie_grab SET replaces_files = '[{\"f\": %" G_GINT64_FORMAT ", \"q\": \"webdl_720p\","
			" \"r\": null}]' WHERE download_id = %" G_GINT64_FORMAT, bluray, download);
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/not_upgrade");
		assert_qf(fx, "bluray_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE download_id = %"
			G_GINT64_FORMAT, download);
	}
	/* Ticked for that very file, at the quality shown: it replaces it. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", worse, NULL);
		gint64 download = add_download(fx, film, "torrent", worse, rel, "r-ticked");

		exec(fx, "UPDATE movie_grab SET manual = true, set_aside = 'hand', replaces_files = '[{\"f\": %"
			G_GINT64_FORMAT ", \"q\": \"bluray_1080p\", \"r\": null}]' WHERE download_id = %" G_GINT64_FORMAT,
			bluray, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	}
	assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_qf(fx, "webdl_1080p", "SELECT mf.quality FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
		" WHERE mf.movie_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", film);
}

/* A grab by force of the download: by root, bound to its movie, with the
 * rule set aside as its confirmation recorded it. */
static void
make_forced(
	Fixture *fx,
	gint64   download
){
	exec(fx, "UPDATE movie_grab SET manual = true, grabbed_by = 'root', set_aside = 'hand', bound = true, forced ="
		" '[{\"k\": \"safety\", \"c\": \"ambiguous\", \"t\": \"it could be more than one title\"}]' WHERE download_id = %"
		G_GINT64_FORMAT, download);
}

static void
put_aside(
	Fixture *fx,
	gint64   import_id,
	gint64   download
){
	exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, import_id);
	exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE download_id = %" G_GINT64_FORMAT
		" AND closed_at IS NULL", download);
}

/*
 * add-force-grab-and-assign, slice B.  Spec (import): "A download's files can
 * be assigned by hand", scenario "A movie's file chosen": a movie's import is
 * blocked because its file's name reads as another movie; an administrator
 * assigns that file as the movie, and it is imported as the movie, the link
 * marked as assigned by that person and kept by hand.  Also: what is not a
 * video of the import cannot be assigned, a second file cannot be the movie
 * too, and the review alone writes nothing.
 */
static void
test_assign_movie(Fixture *fx, gconstpointer data)
{
	const gchar *release = "Marrowfield.Lanterns.1080p.BluRay.x264-NAMED";
	g_autofree gchar *a = in_download(fx, release, "Quillfeather.Harbour.2003.1080p.BluRay.x264-GRP.mkv");
	g_autofree gchar *b = in_download(fx, release, "Quillfeather.Harbour.2003.notes.rar");
	g_autofree gchar *rel = g_strconcat("downloads/", release, NULL);
	g_autoptr(AxisImportAssignAsk) ask = axis_import_assign_ask_new();
	g_autoptr(AxisImportAssignment) review = NULL;
	g_autoptr(AxisImportAssignment) out = NULL;
	g_autoptr(GError) error = NULL;
	gint64 film = add_film(fx, LANTERN_ID);
	gint64 download;
	gint64 id;
	gint video;
	gint archive;
	gboolean applied = FALSE;
	gboolean changed = FALSE;

	write_media(a, FILM_LINE, 64 * 1024, 1);
	write_text(b, "not a video\n");
	download = add_download(fx, film, "torrent", release, rel, "r-named");
	id = request(fx, download);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/wrong_name");
	{
		g_autofree gchar *at = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND kind = 'video'", id);
		g_autofree gchar *other = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND kind = 'archive'", id);

		g_assert_nonnull(at);
		g_assert_nonnull(other);
		video = (gint)g_ascii_strtoll(at, NULL, 10);
		archive = (gint)g_ascii_strtoll(other, NULL, 10);
	}

	/* The archive is not a video: refused, whoever asks. */
	g_array_append_val(ask->positions, archive);
	g_ptr_array_add(ask->said, g_strdup(""));
	g_assert_true(axis_import_assign_preview(fx->import, id, ask, &review, &error));
	g_assert_no_error(error);
	g_assert_nonnull(review->refused);
	g_assert_false(review->ready);
	g_clear_pointer(&review, axis_import_assignment_free);

	/* The video file is the movie: the review says what it becomes, and
	 * writes nothing. */
	g_array_set_size(ask->positions, 0);
	g_ptr_array_set_size(ask->said, 0);
	g_array_append_val(ask->positions, video);
	g_ptr_array_add(ask->said, g_strdup(""));
	g_assert_true(axis_import_assign_preview(fx->import, id, ask, &review, &error));
	g_assert_no_error(error);
	g_assert_null(review->refused);
	g_assert_true(review->ready);
	assert_qf(fx, "0", "SELECT count(*) FROM import_assignment");
	assert_state(fx, id, "blocked/wrong_name");

	/* Applied: recorded with the person, the import queued; its run takes
	 * the file as the movie although its name reads as another. */
	g_assert_true(axis_import_assign(fx->import, id, ask, review->digest, "root", &applied, &changed, &out, &error));
	g_assert_no_error(error);
	g_assert_true(applied);
	assert_qf(fx, "waiting|root", "SELECT i.state || '|' || a.by_user FROM import i JOIN import_assignment a"
		" ON a.import_id = i.id WHERE i.id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	assert_qf(fx, "bluray_1080p|assigned|root|t", "SELECT mf.quality || '|' || mf.hand_how || '|' || mf.hand_by ||"
		" '|' || mf.kept::text::char(1) FROM movie_file mf WHERE mf.movie_id = %" G_GINT64_FORMAT, film);
	/* Nothing of the download was deleted by it. */
	g_assert_true(g_file_test(b, G_FILE_TEST_EXISTS));
}

/* A movie whose file from this download is in place already (published by an
 * earlier run): a different file of the download cannot be chosen. */
static void
test_assign_movie_placed(Fixture *fx, gconstpointer data)
{
	const gchar *release = "Marrowfield.Lanterns.1080p.BluRay.x264-NAMED";
	g_autofree gchar *a = in_download(fx, release, "Quillfeather.Harbour.2003.1080p.BluRay.x264-GRP.mkv");
	g_autofree gchar *b = in_download(fx, release, "Quillfeather.Harbour.2003.other.1080p.BluRay.x264-GRP.mkv");
	g_autofree gchar *rel = g_strconcat("downloads/", release, NULL);
	g_autoptr(AxisImportAssignAsk) ask = axis_import_assign_ask_new();
	g_autoptr(AxisImportAssignment) review = NULL;
	g_autoptr(GError) error = NULL;
	gint64 film = add_film(fx, LANTERN_ID);
	gint64 download;
	gint64 id;
	gint other;

	write_media(a, FILM_LINE, 64 * 1024, 1);
	write_media(b, FILM_LINE, 64 * 1024, 2);
	download = add_download(fx, film, "torrent", release, rel, "r-named");
	id = request(fx, download);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	{
		g_autofree gchar *at = q(fx, "SELECT max(position) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND kind = 'video'", id);

		g_assert_nonnull(at);
		other = (gint)g_ascii_strtoll(at, NULL, 10);
	}
	exec(fx, "UPDATE import_file SET step = 'published' WHERE import_id = %" G_GINT64_FORMAT
		" AND kind = 'video' AND position <> %d", id, other);
	g_array_append_val(ask->positions, other);
	g_ptr_array_add(ask->said, g_strdup(""));
	g_assert_true(axis_import_assign_preview(fx->import, id, ask, &review, &error));
	g_assert_no_error(error);
	g_assert_nonnull(review->refused);
	g_assert_nonnull(strstr(review->refused, "already in place"));
	g_assert_false(review->ready);
}

/*
 * add-force-grab-and-assign, F1.  Spec (import): "The import of a forced
 * grab is bound to what the person named", scenarios "A movie whose file is
 * named otherwise", "A file not ticked", "Far from the runtime"; "The import
 * of a release grabbed by hand sets the profile aside", scenario "Another
 * title's file" (a grab by hand that is not by force still has the name
 * judged), and its last sentence for a movie (a grab by hand whose import
 * can take no file is closed, the movie freed).  Also: a grab Axis made by
 * itself does not replace a file kept by hand.
 */
static void
test_forced_movie(Fixture *fx, gconstpointer data)
{
	/* What the download holds is named for another title than the movie. */
	const gchar *named_otherwise = "Quillfeather.Harbour.2003.1080p.BluRay.x264-GRP.mkv";
	const gchar *by_hand = "Marrowfield.Lanterns.1080p.BluRay.x264-HAND";
	const gchar *forced = "Marrowfield.Lanterns.1080p.BluRay.x264-FORCED";
	const gchar *again = "Marrowfield.Lanterns.2160p.WEB-DL.x265-AGAIN";
	const gchar *by_search = "Marrowfield.Lanterns.2011.2160p.BluRay.x265-SEARCH";
	const gchar *short_one = "Colliery.1080p.BluRay.x264-SHORT";
	const gchar *empty = "Colliery.1080p.BluRay.x264-EMPTY";
	g_autofree gchar *a = in_download(fx, by_hand, named_otherwise);
	g_autofree gchar *b = in_download(fx, forced, named_otherwise);
	g_autofree gchar *c = in_download(fx, again, "Quillfeather.Harbour.2003.2160p.WEB-DL.x265-GRP.mkv");
	g_autofree gchar *d = in_download(fx, by_search, "Marrowfield.Lanterns.2011.2160p.BluRay.x265-SEARCH.mkv");
	g_autofree gchar *e = in_download(fx, short_one, named_otherwise);
	g_autofree gchar *f = in_download(fx, empty, "readme.txt");
	gint64 film = add_film(fx, LANTERN_ID);
	gint64 colliery = add_film(fx, "109");	/* it runs 120 minutes */
	gint64 first = 0;
	gint64 download;
	gint64 id;

	write_media(a, FILM_LINE, 64 * 1024, 1);
	write_media(b, FILM_LINE, 64 * 1024, 2);
	write_media(c, "AXISFAKE duration=10200 width=3840 height=1600 vcodec=hevc", 64 * 1024, 3);
	write_media(d, "AXISFAKE duration=10200 width=3840 height=1600 vcodec=hevc", 64 * 1024, 4);
	write_media(e, "AXISFAKE duration=3600 width=1920 height=800 vcodec=h264", 64 * 1024, 5);
	write_text(f, "nothing to see\n");

	/* Scenario: Another title's file.  Grabbed by hand, not by force: the
	 * file's name is judged as for any import, and it reads as another movie. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", by_hand, NULL);

		download = add_download(fx, film, "torrent", by_hand, rel, "r-by-hand");
		exec(fx, "UPDATE movie_grab SET manual = true, grabbed_by = 'root', set_aside = 'hand' WHERE download_id = %"
			G_GINT64_FORMAT, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/wrong_name");
		assert_qf(fx, "0", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		put_aside(fx, id, download);
	}

	/* Scenario: A movie whose file is named otherwise.  By force, the grab is
	 * bound to the movie: the same name is not judged, the file is imported
	 * as the movie and named as an import names a movie's file, and its link
	 * says it came from a grab by force by that person, kept by hand. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", forced, NULL);
		g_autofree gchar *file = NULL;
		g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);

		download = add_download(fx, film, "torrent", forced, rel, "r-forced");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		assert_qf(fx, "bluray_1080p|forced|root|t|t", "SELECT mf.quality || '|' || mf.hand_how || '|' || mf.hand_by ||"
			" '|' || mf.kept::text::char(1) || '|' || (mf.hand_at IS NOT NULL)::text::char(1) FROM movie_file mf"
			" WHERE mf.movie_id = %" G_GINT64_FORMAT, film);
		assert_qf(fx, "imported", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, download);
		assert_qf(fx, "t", "SELECT lf.rel_path LIKE '" LANTERN_FOLDER "/" LANTERN_FOLDER " %%' FROM movie_file mf"
			" JOIN library_file lf ON lf.id = mf.file_id WHERE mf.movie_id = %" G_GINT64_FORMAT, film);
		g_assert_true(g_file_test(folder, G_FILE_TEST_IS_DIR));
		file = q(fx, "SELECT file_id FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		first = g_ascii_strtoll(file, NULL, 10);
	}

	/* A grab Axis made by itself, in flight when the file was kept: it does
	 * not replace the kept file, though it is better. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", by_search, NULL);

		download = add_download(fx, film, "torrent", by_search, rel, "r-by-search");
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "t", "SELECT blockers::text LIKE '%%kept_by_hand%%' FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "bluray_1080p", "SELECT mf.quality FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
			" WHERE mf.movie_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", film);
		put_aside(fx, id, download);
	}

	/* Scenario: A file not ticked.  Another grab by force, of a better
	 * release: the file in the library was not ticked on its confirmation,
	 * so it is not replaced; ticked for that very file, it is. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", again, NULL);

		download = add_download(fx, film, "torrent", again, rel, "r-again");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/not_upgrade");
		assert_qf(fx, "t", "SELECT blockers::text LIKE '%%was not ticked to be replaced when this release was grabbed by"
			" force%%' FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "bluray_1080p", "SELECT mf.quality FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
			" WHERE mf.movie_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", film);
		exec(fx, "UPDATE movie_grab SET replaces_files = '[{\"f\": %" G_GINT64_FORMAT ", \"q\": \"bluray_1080p\","
			" \"r\": null}]' WHERE download_id = %" G_GINT64_FORMAT, first, download);
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		assert_qf(fx, "t", "SELECT upgrade FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "webdl_2160p|forced", "SELECT mf.quality || '|' || mf.hand_how FROM movie_file mf"
			" JOIN library_file lf ON lf.id = mf.file_id WHERE mf.movie_id = %" G_GINT64_FORMAT
			" AND lf.state = 'present'", film);
	}

	/* Scenario: Far from the runtime.  Bound is about the name alone: a file
	 * that runs half the movie's runtime is blocked, as for any import. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", short_one, NULL);

		download = add_download(fx, colliery, "torrent", short_one, rel, "r-short");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/runtime");
		assert_qf(fx, "0", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, colliery);
		put_aside(fx, id, download);
	}

	/* A grab by force whose download holds no video: nothing can be taken,
	 * so the grab lets go of the movie and its history says why. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", empty, NULL);

		download = add_download(fx, colliery, "torrent", empty, rel, "r-empty");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/no_video");
		assert_qf(fx, "not_imported", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT,
			download);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
			" 'abandoned' AND text LIKE 'The grab by force of %%is let go: its import could take no file of the"
			" download.%%'", colliery);
	}
}

/*
 * Review finding 1 of add-force-grab-and-assign.  A file kept by hand is
 * replaced only with the tick for that very file, whatever the kind of grab
 * ("Grab it anyway" sets the profile aside, and used to skip the guard).
 */
static void
test_kept_anyway(Fixture *fx, gconstpointer data)
{
	const gchar *forced = "Marrowfield.Lanterns.1080p.BluRay.x264-FORCED";
	const gchar *anyway = "Marrowfield.Lanterns.2011.2160p.WEB-DL.x265-ANYWAY";
	g_autofree gchar *a = in_download(fx, forced, "Quillfeather.Harbour.2003.1080p.BluRay.x264-GRP.mkv");
	g_autofree gchar *b = in_download(fx, anyway, "Marrowfield.Lanterns.2011.2160p.WEB-DL.x265-ANYWAY.mkv");
	gint64 film = add_film(fx, LANTERN_ID);
	gint64 first = 0;
	gint64 download;
	gint64 id;

	write_media(a, FILM_LINE, 64 * 1024, 1);
	write_media(b, "AXISFAKE duration=10200 width=3840 height=1600 vcodec=hevc", 64 * 1024, 2);

	/* A kept file: a forced import's. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", forced, NULL);
		g_autofree gchar *file = NULL;

		download = add_download(fx, film, "torrent", forced, rel, "r-forced");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		file = q(fx, "SELECT file_id FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		first = g_ascii_strtoll(file, NULL, 10);
	}
	/* "Grab it anyway" (the profile set aside, no tick), better: blocked,
	 * the kept file stays. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", anyway, NULL);

		download = add_download(fx, film, "torrent", anyway, rel, "r-anyway");
		exec(fx, "UPDATE movie_grab SET manual = true, grabbed_by = 'root', set_aside = 'anyway' WHERE download_id = %"
			G_GINT64_FORMAT, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "t", "SELECT blockers::text LIKE '%%kept_by_hand%%' FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "bluray_1080p", "SELECT mf.quality FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
			" WHERE mf.movie_id = %" G_GINT64_FORMAT " AND lf.state = 'present'", film);
		/* Ticked for that very file, it is replaced (the tick is a person's
		 * leave, whatever the kind of grab). */
		exec(fx, "UPDATE movie_grab SET replaces_files = '[{\"f\": %" G_GINT64_FORMAT ", \"q\": \"bluray_1080p\","
			" \"r\": null}]' WHERE download_id = %" G_GINT64_FORMAT, first, download);
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	}
}

/* Finding 5: with a profile that cannot be used, the kept and the tick checks
 * still come first (not only "the profile cannot be used"). */
static void
test_kept_invalid_profile(Fixture *fx, gconstpointer data)
{
	const gchar *forced = "Marrowfield.Lanterns.1080p.BluRay.x264-FORCED";
	const gchar *anyway = "Marrowfield.Lanterns.2011.2160p.WEB-DL.x265-ANYWAY";
	const gchar *again = "Marrowfield.Lanterns.2160p.WEB-DL.x265-AGAIN";
	g_autofree gchar *a = in_download(fx, forced, "Quillfeather.Harbour.2003.1080p.BluRay.x264-GRP.mkv");
	g_autofree gchar *b = in_download(fx, anyway, "Marrowfield.Lanterns.2011.2160p.WEB-DL.x265-ANYWAY.mkv");
	g_autofree gchar *c = in_download(fx, again, "Quillfeather.Harbour.2003.2160p.WEB-DL.x265-GRP.mkv");
	gint64 film = add_film(fx, LANTERN_ID);
	gint64 download;
	gint64 id;

	write_media(a, FILM_LINE, 64 * 1024, 1);
	write_media(b, "AXISFAKE duration=10200 width=3840 height=1600 vcodec=hevc", 64 * 1024, 2);
	write_media(c, "AXISFAKE duration=10200 width=3840 height=1600 vcodec=hevc", 64 * 1024, 3);
	{
		g_autofree gchar *rel = g_strconcat("downloads/", forced, NULL);

		download = add_download(fx, film, "torrent", forced, rel, "r-forced");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	}
	exec(fx, "UPDATE quality_profile SET format = 99, definition = '{\"format\": 99}' WHERE id = (SELECT quality_profile_id FROM movie WHERE id = %"
		G_GINT64_FORMAT ")", film);
	{
		g_autofree gchar *rel = g_strconcat("downloads/", anyway, NULL);

		download = add_download(fx, film, "torrent", anyway, rel, "r-anyway");
		exec(fx, "UPDATE movie_grab SET manual = true, grabbed_by = 'root', set_aside = 'anyway' WHERE download_id = %"
			G_GINT64_FORMAT, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "t", "SELECT blockers::text LIKE '%%kept_by_hand%%' FROM import WHERE id = %" G_GINT64_FORMAT, id);
		put_aside(fx, id, download);
	}
	/* A grab by force with no tick for the file it would replace. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", again, NULL);

		download = add_download(fx, film, "torrent", again, rel, "r-again");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "t", "SELECT blockers::text LIKE '%%was not ticked to be replaced%%' FROM import WHERE id = %"
			G_GINT64_FORMAT, id);
	}
}

/* Findings 2: a grab by hand (not forced) whose download holds no video stays
 * open, so the movie is not searched for again at once; a forced one is
 * closed, said "forced", and the movie held from automatic search while its
 * import waits for a person. */
static void
test_stuck_hand_and_force(Fixture *fx, gconstpointer data)
{
	const gchar *empty_hand = "Colliery.1080p.BluRay.x264-HANDEMPTY";
	const gchar *empty_force = "Colliery.1080p.BluRay.x264-FORCEEMPTY";
	g_autofree gchar *c = in_download(fx, empty_hand, "readme.txt");
	g_autofree gchar *d = in_download(fx, empty_force, "readme.txt");
	gint64 colliery = add_film(fx, "109");
	gint64 download;
	gint64 id;

	write_text(c, "nothing to see\n");
	write_text(d, "nothing to see\n");
	{
		g_autofree gchar *rel = g_strconcat("downloads/", empty_hand, NULL);

		download = add_download(fx, colliery, "torrent", empty_hand, rel, "r-hand-empty");
		exec(fx, "UPDATE movie_grab SET manual = true, grabbed_by = 'root', set_aside = 'hand' WHERE download_id = %"
			G_GINT64_FORMAT, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "", "SELECT coalesce(close_reason, '') FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT,
			download);
		put_aside(fx, id, download);
	}
	/* Forced: closed, said "forced", and the movie held from automatic search
	 * while its import waits; dismissed, the hold ends. */
	{
		g_autofree gchar *rel = g_strconcat("downloads/", empty_force, NULL);
		g_autoptr(GError) error = NULL;
		AxisMovieSearchReport report;

		download = add_download(fx, colliery, "torrent", empty_force, rel, "r-force-empty");
		make_forced(fx, download);
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_qf(fx, "not_imported", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT,
			download);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
			" 'abandoned' AND text LIKE 'The grab by force of %%'", colliery);
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_movies_run_search(fx->movies, colliery, "scheduled", "root", NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_false(report.searched);
		g_assert_nonnull(report.skipped);
		g_assert_nonnull(strstr(report.skipped, "grabbed by force"));
		/* Dismissed, the hold ends. */
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		memset(&report, 0, sizeof(report));
		(void)axis_movies_run_search(fx->movies, colliery, "scheduled", "root", NULL, &report, NULL);
		g_assert_true(report.skipped == NULL || strstr(report.skipped, "grabbed by force") == NULL);
	}
}

/* Extras: in a standard folder (kept in that folder of the film's), or
 * named with a standard suffix (beside the film); a short video that is
 * neither is not imported; with import.extras off none is. */
static void
test_extras(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	const gchar *r = HEARTH_RELEASE;
	g_autofree gchar *film = in_download(fx, r, HEARTH_RELEASE ".mkv");
	g_autofree gchar *making = in_download(fx, r, "Featurettes/Making Of.mkv");
	g_autofree gchar *trailer = in_download(fx, r, "Hearth-trailer.mkv");
	g_autofree gchar *short_one = in_download(fx, r, "Bonus Clip.mkv");
	g_autofree gchar *folder = g_build_filename(fx->films, HEARTH_FOLDER, NULL);
	g_autofree gchar *placed_making = g_build_filename(folder, "Featurettes", "Making Of.mkv", NULL);
	g_autofree gchar *listing = NULL;
	gint64 id;

	write_media(film, FILM_LINE, 64 * 1024, 1);
	write_media(making, "AXISFAKE duration=1500 width=1920 height=800 vcodec=h264", 16 * 1024, 2);
	write_media(trailer, "AXISFAKE duration=150 width=1920 height=800 vcodec=h264", 8 * 1024, 3);
	write_media(short_one, "AXISFAKE duration=300 width=1920 height=800 vcodec=h264", 8 * 1024, 4);
	id = odd_download(fx, movie, r, "x-extras");
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed_making).st_ino, ==, (guint64)stat_of(making).st_ino);
	listing = tree(folder);
	assert_contains(listing, "-trailer.mkv");
	g_assert_null(strstr(listing, "Bonus Clip"));
	assert_qf(fx, "extra", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE 'Featurettes/%%'", id);
	assert_qf(fx, "short", "SELECT reasons->0->>'c' FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE 'Bonus%%'", id);
	/* The extra is in the index too, and nothing temporary is left. */
	assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE rel_path LIKE '%%/Featurettes/Making Of.mkv'");
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);

	/* With extras off, only the film (and its subtitles) is imported. */
	clear_film_tree(fx, folder);
	exec(fx, "DELETE FROM movie");
	movie = add_film(fx, "107");
	set(fx, AXIS_IMPORT_SETTING_EXTRAS, "false");
	id = odd_download(fx, movie, r, "x-extras-off");
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_false(g_file_test(placed_making, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(count_videos(folder), ==, 1);
}

/* ── 6.8 The preview names a film as its import does ────────────── */

static void
test_preview(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) previews = NULL;
	g_autofree gchar *problem = NULL;
	const AxisImportPreview *hearth;
	const gchar *release = "Hearth.1995.REMASTERED.1080p.BluRay.DTS-HD.MA.5.1.x264-QXR";
	g_autofree gchar *source = in_download(fx, release, "hearth.1995.remastered.1080p.bluray.x264-qxr.mkv");
	g_autofree gchar *placed = NULL;
	gint64 movie = add_film(fx, "107");
	gint64 id;

	g_assert_true(axis_import_preview(TRUE, "tmdb", AXIS_IMPORT_DEFAULT_FORMAT, &previews, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(previews->len, ==, 3);
	hearth = g_ptr_array_index(previews, 0);
	g_assert_cmpstr(hearth->label, ==, "Hearth (1995)");
	g_assert_cmpstr(hearth->folder, ==, HEARTH_FOLDER);

	write_media(source, FILM_LINE, 64 * 1024, 8);
	id = odd_download(fx, movie, release, "p-hearth");
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	placed = g_build_filename(fx->films, hearth->folder, hearth->file, NULL);
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, (guint64)stat_of(source).st_ino);

	/* An invalid format is refused, with why. */
	g_clear_pointer(&previews, g_ptr_array_unref);
	g_assert_false(axis_import_preview(TRUE, "tmdb", "{nope}", &previews, &problem));
	g_assert_nonnull(problem);
}

/* ── 6.9 Read-only, offline ─────────────────────────────────────── */

static void
test_read_only(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	gint64 id;

	id = hearth_import(fx, movie, "torrent", HEARTH_RELEASE, "t-ro", NULL);
	before = tree(fx->films);
	set_read_only(fx, TRUE);
	g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
	assert_state(fx, id, "waiting/read_only");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "storage.read_only");
	after = tree(fx->films);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_TO_IMPORT);
	set_read_only(fx, FALSE);

	/* The volume offline: the import waits, and nothing is written where
	 * the volume was. */
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
	assert_state(fx, id, "waiting/volume_offline");
	/* Nobody can import it anyway while its volume is away. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	}
	{
		GDir *empty = g_dir_open(fx->disk, 0, NULL);

		g_assert_nonnull(empty);
		g_assert_null(g_dir_read_name(empty));
		g_dir_close(empty);
	}
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
}

/* ── 6.10 Links and places outside ──────────────────────────────── */

static void
test_escape(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *outside = g_build_filename(fx->disk, "elsewhere", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL);
	g_autofree gchar *outside_dir = g_path_get_dirname(outside);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 id;

	write_media(outside, FILM_LINE, 64 * 1024, 1);
	before = tree(fx->films);

	/* A link in the download is not followed. */
	{
		const gchar *r = "Hearth.1995.Linked.1080p.BluRay.x264-GRP";
		g_autofree gchar *dir = g_build_filename(fx->downloads, r, NULL);
		g_autofree gchar *link = g_build_filename(dir, "Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL);

		g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
		g_assert_cmpint(symlink(outside, link), ==, 0);
		id = odd_download(fx, movie, r, "e-link");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/no_video");
		assert_qf(fx, "link", "SELECT kind FROM import_file WHERE import_id = %" G_GINT64_FORMAT, id);
		g_assert_cmpuint((guint)stat_of(outside).st_nlink, ==, 1);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
	}
	/* A download that is itself a link to a folder elsewhere. */
	{
		const gchar *r = "Hearth.1995.Pointer.1080p.BluRay.x264-GRP";
		g_autofree gchar *link = g_build_filename(fx->downloads, r, NULL);

		g_assert_cmpint(symlink(outside_dir, link), ==, 0);
		id = odd_download(fx, movie, r, "e-pointer");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		g_assert_cmpuint((guint)stat_of(outside).st_nlink, ==, 1);
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
	}
	/* A download inside the films' root. */
	{
		g_autofree gchar *inside = g_build_filename(fx->films, "Hearth.1995.1080p.BluRay.x264-GRP",
			"Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL);
		gint64 download;

		write_media(inside, FILM_LINE, 64 * 1024, 2);
		g_free(before);
		before = tree(fx->films);
		download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "films/" HEARTH_RELEASE, "e-inside");
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/inside_root");
		exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
	}
	/* A place that climbs out of where it says. */
	{
		gint64 download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "downloads/../elsewhere", "e-climb");

		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		g_assert_cmpuint((guint)stat_of(outside).st_nlink, ==, 1);
	}
	after = tree(fx->films);
	g_assert_cmpstr(after, ==, before);
}

/* ── 6.11 The actions ───────────────────────────────────────────── */

static void
test_actions(Fixture *fx, gconstpointer data)
{
	gint64 hearth = add_film(fx, "107");
	gint64 rokan = add_film(fx, "108");
	g_autoptr(GError) error = NULL;
	gint64 assigned = 0;
	gint64 id;
	gint64 download = 0;

	/* Several films: "import anyway" cannot choose for a person. */
	{
		const gchar *r = "Hearth.1995.Double.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Hearth.1995.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *b = in_download(fx, r, "Hearth.1995.Directors.Cut.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *position = NULL;

		write_media(a, FILM_LINE, 64 * 1024, 1);
		write_media(b, FILM_LINE, 64 * 1024, 2);
		id = odd_download(fx, hearth, r, "a-several");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
		g_clear_error(&error);
		/* A position that is not one of its videos is refused. */
		g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_CHOOSE, 999, 0, "root", NULL, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
		g_clear_error(&error);
		position = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND path LIKE 'Hearth.1995.1080p%%'", id);
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_CHOOSE, (gint)g_ascii_strtoll(position, NULL, 10),
			0, "root", NULL, &error));
		g_assert_no_error(error);
		assert_qf(fx, "choose_file/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
			G_GINT64_FORMAT, id);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
			" AND kind = 'import_action' AND by_user = 'root'", hearth);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		{
			g_autofree gchar *placed = hearth_path(fx, HEARTH_FILE);

			g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, (guint64)stat_of(a).st_ino);
		}
		/* Done: nothing more can be done to it. */
		g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
		g_clear_error(&error);
	}
	/* Another film's name: imported anyway, the override recorded. */
	{
		const gchar *r = "Rokan.1998.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Hearth.1995.1080p.BluRay.x264-GRP.mkv");

		write_media(a, FILM_LINE, 64 * 1024, 3);
		id = odd_download(fx, rokan, r, "a-name");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		assert_state(fx, id, "blocked/wrong_name");
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
		g_assert_no_error(error);
		assert_qf(fx, "{wrong_name}/root", "SELECT override_codes::text || '/' || override_by FROM import WHERE id = %"
			G_GINT64_FORMAT, id);
		assert_qf(fx, "import_anyway/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
			G_GINT64_FORMAT, id);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		assert_qf(fx, "root", "SELECT linked_by FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, rokan);
	}
	/* Dismissed: nothing written, the download left at its client, its
	 * grab closed. */
	{
		const gchar *r = "Rokan.1998.Twice.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Rokan.1998.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *b = in_download(fx, r, "Rokan.1998.Alternate.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *rel = g_strconcat("downloads/", r, NULL);

		write_media(a, FILM_LINE, 64 * 1024, 4);
		write_media(b, FILM_LINE, 64 * 1024, 5);
		download = add_download(fx, rokan, "torrent", r, rel, "a-dismiss");
		id = request(fx, download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
		g_assert_no_error(error);
		assert_state(fx, id, "dismissed/dismissed");
		assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
		assert_qf(fx, "dismissed", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT,
			download);
		assert_qf(fx, "dismiss/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
			G_GINT64_FORMAT, id);
		/* And it stays dismissed: no new import for the download. */
		g_assert_cmpint(request(fx, download), ==, id);
		g_assert_cmpstr(run(fx, id, NULL), ==, "dismissed");
	}
	/* Assigned to another film: a manual import for that one. */
	{
		const gchar *r = "Hearth.1995.Wrong.1080p.BluRay.x264-GRP";
		g_autofree gchar *a = in_download(fx, r, "Thistle.1981.1080p.BluRay.x264-GRP.mkv");
		g_autofree gchar *b = in_download(fx, r, "Thistle.1981.Extended.1080p.BluRay.x264-GRP.mkv");
		gint64 colliery = add_film(fx, "109");

		write_media(a, "AXISFAKE duration=7200 width=1920 height=800", 64 * 1024, 6);
		write_media(b, "AXISFAKE duration=7300 width=1920 height=800", 64 * 1024, 7);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
		id = odd_download(fx, hearth, r, "a-assign");
		g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_ASSIGN, -1, colliery, "root", &assigned,
			&error));
		g_assert_no_error(error);
		g_assert_cmpint(assigned, >, 0);
		assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, id);
		assert_qf(fx, "manual", "SELECT kind FROM import WHERE id = %" G_GINT64_FORMAT, assigned);
		assert_qf(fx, "root", "SELECT requested_by FROM import WHERE id = %" G_GINT64_FORMAT, assigned);
		{
			g_autofree gchar *owner = q(fx, "SELECT owner_id FROM import WHERE id = %" G_GINT64_FORMAT, assigned);

			g_assert_cmpint(g_ascii_strtoll(owner, NULL, 10), ==, colliery);
		}
	}
	/* Retry takes a waiting import too. */
	{
		gint64 thistle_download;

		set_read_only(fx, TRUE);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
		id = hearth_import(fx, rokan, "torrent", "Rokan.1998.Again.1080p.BluRay.x264-GRP", "a-retry", &thistle_download);
		g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
		g_assert_no_error(error);
		assert_qf(fx, "retry/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
			G_GINT64_FORMAT, id);
		set_read_only(fx, FALSE);
	}
}

/* ── The follow-up, and a manual import ─────────────────────────── */

static void
test_follow(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autoptr(GError) error = NULL;
	AxisMovieFollowReport report;
	gint64 download;

	download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "downloads/" HEARTH_RELEASE, "f-1");
	set_read_only(fx, TRUE);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.imports, ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM import WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'import.run'");
	/* Again: the same import, not another. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.imports, ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM import");
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_TO_IMPORT);

	/* An import with no open grab (a manual one) that waits is queued again
	 * when it is due, as is one a job left 'running' and gave up on. */
	exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed'");
	exec(fx, "UPDATE job SET state = 'succeeded', finished_at = now() WHERE kind = 'import.run'");
	exec(fx, "UPDATE import SET kind = 'manual', retry_after = now() - interval '1 second'");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_no_error(error);
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'import.run' AND state = 'queued'");
	exec(fx, "UPDATE job SET state = 'failed', finished_at = now() WHERE kind = 'import.run' AND state = 'queued'");
	exec(fx, "UPDATE import SET state = 'running', updated_at = now() - interval '11 minutes'");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_no_error(error);
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'import.run' AND state = 'queued'");
	set_read_only(fx, FALSE);
}

static void
test_manual(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "107");
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autofree gchar *problem = NULL;
	AxisImportManualSpec spec;
	gint64 id = 0;
	guint i;
	gboolean listed = FALSE;

	/* What a person sees: the folder at the top of the downloads root, and
	 * what is in it. */
	g_assert_true(axis_import_list_sources(fx->import, &sources, &error));
	g_assert_no_error(error);
	for (i = 0; i < sources->len; i++) {
		const AxisImportSource *source = g_ptr_array_index(sources, i);

		if (g_strcmp0(source->label, HEARTH_RELEASE) == 0 && source->root_id == fx->downloads_root)
			listed = TRUE;
	}
	g_assert_true(listed);
	/* A finished download nothing has imported is offered too; a YouTube
	 * video's never is, even one left 'completed': its file is in the
	 * library already, put there by its own kind, and is nothing to import. */
	{
		g_autofree gchar *made = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source,"
			" release_name, module, state, size_bytes, done_bytes, volume_id, rel_path, sent_at, completed_at)"
			" VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'okvideo0001', 'usenet', 'file',"
			" 'Moon Kettle Studio - The Lost Spoon [okvideo0001]', 'movies', 'completed', 1000, 1000, %" G_GINT64_FORMAT
			", 'made-up/The Lost Spoon.mkv', now(), now()) RETURNING id", fx->client_id, fx->volume_id);
		gint64 made_id = g_ascii_strtoll(made, NULL, 10);
		g_autoptr(GPtrArray) as_usenet = NULL;
		g_autoptr(GPtrArray) as_youtube = NULL;
		guint offered = 0;

		g_assert_cmpint(made_id, >, 0);
		g_assert_true(axis_import_list_sources(fx->import, &as_usenet, &error));
		for (i = 0; i < as_usenet->len; i++)
			offered += ((const AxisImportSource *)g_ptr_array_index(as_usenet, i))->download_id == made_id;
		g_assert_cmpuint(offered, ==, 1);
		exec(fx, "UPDATE download SET protocol = 'youtube', source = 'address', module = 'youtube' WHERE id = %"
			G_GINT64_FORMAT, made_id);
		g_assert_true(axis_import_list_sources(fx->import, &as_youtube, &error));
		g_assert_no_error(error);
		for (i = 0; i < as_youtube->len; i++)
			g_assert_cmpint(((const AxisImportSource *)g_ptr_array_index(as_youtube, i))->download_id, !=, made_id);
		exec(fx, "DELETE FROM download WHERE id = %" G_GINT64_FORMAT, made_id);
	}
	g_assert_true(axis_import_peek(fx->import, 0, fx->downloads_root, HEARTH_RELEASE, &files, &problem, &error));
	g_assert_no_error(error);
	g_assert_null(problem);
	g_assert_cmpuint(files->len, ==, 4);

	/* A path out of the root is refused. */
	memset(&spec, 0, sizeof(spec));
	spec.module = AXIS_MOVIES_MODULE_ID;
	spec.owner_id = movie;
	spec.root_id = fx->downloads_root;
	spec.path = "../films";
	spec.position = -1;
	spec.user = "root";
	g_assert_false(axis_import_manual(fx->import, &spec, &id, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_INVALID);
	g_clear_error(&error);

	spec.path = HEARTH_RELEASE;
	g_assert_true(axis_import_manual(fx->import, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	assert_qf(fx, "manual/root", "SELECT kind || '/' || requested_by FROM import WHERE id = %" G_GINT64_FORMAT, id);
	/* Busy: one open import per film. */
	g_assert_false(axis_import_manual(fx->import, &spec, NULL, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_BUSY);
	g_clear_error(&error);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	{
		g_autofree gchar *source = g_build_filename(dir, HEARTH_RELEASE ".mkv", NULL);
		g_autofree gchar *placed = g_build_filename(fx->films, HEARTH_FOLDER, NULL);

		g_assert_cmpuint(count_videos(placed), ==, 1);
		g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	}
	assert_qf(fx, "root", "SELECT linked_by FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* ── Moves (import-by-move) ─────────────────────────────────────── */

/* The default, import.take_files "move" (the fixture says "hardlink"). */
static void
moving(Fixture *fx)
{
	set(fx, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_MOVE);
}

#define LANTERN_PLACED LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUP.mkv"
#define LANTERN_SUB    LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUP.en.srt"

/* The inode the import recorded for its file of @role (the download's file
 * when the walk found it). */
static guint64
recorded_inode(
	Fixture     *fx,
	gint64       id,
	const gchar *role
){
	g_autofree gchar *text = q(fx, "SELECT inode FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = '%s'"
		" ORDER BY position LIMIT 1", id, role);

	g_assert_nonnull(text);
	return g_ascii_strtoull(text, NULL, 10);
}

/* Spec (import): a Usenet download's files are moved into the library by
 * default: one rename each on the library's volume, the library's file the
 * download's own (its inode recorded before the move), with one name; the
 * job is removed from its client, and what the import did not take is
 * deleted with the job's folder. */
static void
test_move_usenet_job(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *sub = NULL;
	g_autofree gchar *source_film = NULL;
	g_autofree gchar *source_sub = NULL;
	GStatBuf placed;
	UsenetJob job;

	moving(fx);
	usenet_imported(fx, FALSE, &job);
	source_film = g_build_filename(job.dir, LANTERN_RELEASE ".mkv", NULL);
	source_sub = g_build_filename(job.dir, LANTERN_RELEASE ".en.srt", NULL);
	placed = stat_of(job.placed);
	g_assert_cmpuint((guint)placed.st_nlink, ==, 1);
	g_assert_cmpuint((guint64)placed.st_ino, ==, recorded_inode(fx, job.id, "film"));
	sub = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_SUB, NULL);
	g_assert_cmpuint((guint64)stat_of(sub).st_ino, ==, recorded_inode(fx, job.id, "subtitle"));
	g_assert_cmpuint((guint)stat_of(sub).st_nlink, ==, 1);
	assert_qf(fx, "move", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_qf(fx, "moved|moved", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", job.id);
	assert_qf(fx, "t", "SELECT bool_and(dest_inode = inode)::text::char FROM import_file WHERE import_id = %"
		G_GINT64_FORMAT " AND role IS NOT NULL AND accepted", job.id);
	text = q(fx, "SELECT mode_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "a move on Disk one");
	/* The job removed from its client; the folder SABnzbd kept deleted, with
	 * the sample, the .nfo, the par2 file, Proof and what is in it. */
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "done", "Its folder at the client (" SAB_JOB ") was deleted, with the 5 files and folders "
		"left in it that the import did not take");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	/* The index knows the file has one name: it is not pinned as a hardlink. */
	assert_qf(fx, "1|f", "SELECT nlink || '|' || pinned_links::text::char FROM library_file WHERE inode = %"
		G_GUINT64_FORMAT, (guint64)placed.st_ino);
	usenet_job_clear(&job);
}

/* Spec (import): SABnzbd reporting a job of one file by that file: the job's
 * folder is the import's source, its film and subtitle are moved, and the
 * folder goes with the rest. */
static void
test_move_usenet_file_reported(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	moving(fx);
	usenet_imported_at(fx, FALSE, JOB_FILE, JOB_FILE, &job);
	assert_qf(fx, "downloads/axis/" LANTERN_RELEASE, "SELECT source_path FROM import WHERE id = %" G_GINT64_FORMAT,
		job.id);
	assert_qf(fx, "move", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_folder(fx, job.id, "done", "Its folder at the client (" SAB_JOB ") was deleted, with the 5 files and folders "
		"left in it that the import did not take");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint64)stat_of(job.placed).st_ino, ==, recorded_inode(fx, job.id, "film"));
	usenet_job_clear(&job);
}

/* A torrent of the made-up film that its client holds, completed in its own
 * folder (the film, a sample, a subtitle, an .nfo) at @below beneath the
 * client's folder for finished torrents; the client keeps Axis's label in
 * @label_dir.  Its import is requested, not run. */
static void
torrent_held(
	Fixture     *fx,
	const gchar *label_dir,
	const gchar *below,
	TorrentJob  *job
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *client_path = g_strconcat(TOR_DONE "/", below, NULL);
	g_autofree gchar *rel_path = g_strconcat("downloads/done/", below, NULL);
	g_autofree gchar *sample = NULL;
	g_autofree gchar *sub = NULL;
	g_autofree gchar *nfo = NULL;

	memset(job, 0, sizeof(*job));
	job->movie = add_film(fx, LANTERN_ID);
	job->place = g_build_filename(fx->downloads, "done", below, NULL);
	job->placed = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_PLACED, NULL);
	job->film = g_build_filename(job->place, LANTERN_RELEASE ".mkv", NULL);
	sample = g_build_filename(job->place, LANTERN_RELEASE "-sample.mkv", NULL);
	sub = g_build_filename(job->place, LANTERN_RELEASE ".en.srt", NULL);
	nfo = g_build_filename(job->place, LANTERN_RELEASE ".nfo", NULL);
	write_media(job->film, FILM_LINE, 256 * 1024, 5);
	write_media(sample, SAMPLE_LINE, 8 * 1024, 6);
	write_text(sub, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n");
	write_text(nfo, "an nfo\n");
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, TOR_ROOT, fx->volume_id, "downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(fx->fake, label_dir);
	/* As Deluge and qBittorrent: a removal of a torrent it does not have says
	 * so. */
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000, client_path,
		NULL);
	title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, job->movie);
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'Torrents', '" TOR_REF "', 'torrent', 'magnet', '%s', %s,"
		" 'Marrowfield Lanterns (2011)', 'movies', 'completed', 1000, 1000, '%s', %" G_GINT64_FORMAT ", '%s', now(),"
		" now()) RETURNING id", fx->client_id, LANTERN_RELEASE, title, client_path, fx->volume_id, rel_path);
	job->download = g_ascii_strtoll(download, NULL, 10);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '%s', 'torrent', lower('1:0:%s'), %" G_GINT64_FORMAT ", now(),"
		" now()) RETURNING id", job->movie, LANTERN_RELEASE, LANTERN_RELEASE, job->download);
	g_assert_nonnull(grab);
	job->id = request(fx, job->download);
}

/* Spec (import): a torrent's files are moved only once its client has let
 * go of it: removed WITHOUT its data, then asked again until it said it no
 * longer has it, all before the first file is moved; then what it left is
 * deleted inside the client's folder for Axis's label. */
static void
test_move_torrent(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *label = g_build_filename(fx->downloads, "done", "axis", NULL);
	guint64 film_ino;
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	film_ino = (guint64)stat_of(job.film).st_ino;
	hook_reset(fx);
	fx->hook.watch = TOR_REF;
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	/* Let go of before anything was placed: the client no longer had it at
	 * the first file's temporary name. */
	g_assert_cmpint(fx->hook.held_at_temp, ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", TOR_REF), ==, 2);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_removal(fx, job.id, "done", "removed from its client without its files");
	assert_qf(fx, "move", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	/* The download's own file, renamed. */
	g_assert_cmpuint((guint64)stat_of(job.placed).st_ino, ==, film_ino);
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	/* What it left (the sample, the .nfo) went with its folder; the label's
	 * folder stays. */
	assert_folder(fx, job.id, "done", "Its folder at the client (" TOR_LABEL "/" LANTERN_RELEASE ") was deleted, with the 2 "
		"files and folders left in it that the import did not take");
	g_assert_false(g_file_test(job.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(label, G_FILE_TEST_IS_DIR));
	g_assert_cmpint(movie_state(fx, job.movie), ==, AXIS_MOVIE_IN_LIBRARY);
	/* Nothing more is asked of the client. */
	cleanup(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", TOR_REF), ==, 2);
	torrent_job_clear(&job);
}

/* Spec (import): a client that cannot be reached, or that still has the
 * torrent, makes the import wait with nothing moved or written; once it lets
 * go, the files are moved. */
static void
test_move_torrent_waits(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	before = axis_test_storage_digest(job.place);

	/* Unreachable: waits, nothing moved, no folder made. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "waiting");
	assert_state(fx, job.id, "waiting/client_release");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "once its client has let go of the torrent");
	assert_contains(text, "nothing was moved");
	after = axis_test_storage_digest(job.place);
	g_assert_cmpstr(after, ==, before);
	g_clear_pointer(&after, g_free);
	g_assert_false(g_file_test(folder, G_FILE_TEST_EXISTS));
	assert_removal(fx, job.id, "-", NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);

	/* A client that never says it no longer has the torrent (it answers a
	 * removal of what it lacks as a refusal): it is not taken to have let
	 * go, and nothing is moved. */
	axis_test_download_fake_say_gone(fx->fake, FALSE);
	exec(fx, "UPDATE import SET retry_after = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "waiting");
	assert_state(fx, job.id, "waiting/client_release");
	after = axis_test_storage_digest(job.place);
	g_assert_cmpstr(after, ==, before);
	g_assert_false(g_file_test(folder, G_FILE_TEST_EXISTS));
	assert_removal(fx, job.id, "-", NULL);

	/* It says so now (it had removed the torrent the last time, without
	 * saying so): moved, and recorded as it said, with what Axis knows: it
	 * had asked before. */
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	assert_removal(fx, job.id, "gone", "Axis had asked its client to let go of it");
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	torrent_job_clear(&job);
}

/* Spec (import): a torrent a person moved to another category at its client
 * before its import is not Axis's any more: it is left at its client, nothing
 * of it removed, its files taken without moving them (hardlinked), and the
 * import says so; nothing of it is deleted afterwards (finding 2 of the stage
 * 2 bug hunt: it was removed from its client and its files moved away). */
static void
test_move_torrent_not_ours(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	g_autofree gchar *client_path = g_strconcat(TOR_DONE "/axis/", LANTERN_RELEASE, NULL);
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	/* A person moved it to a category of their own, to keep it seeding. */
	axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "manual", NULL, "completed", 1000, 1000,
		client_path, NULL);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	assert_qf(fx, "hardlink", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	text = q(fx, "SELECT mode_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "not Axis's any more");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT client_removal || ': ' || client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT,
		job.id);
	assert_contains(text, "kept: The torrent is not Axis's any more");
	assert_qf(fx, NULL, "SELECT folder_removal FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	/* At its client still, nothing removed; its file there, hardlinked. */
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint((guint64)stat_of(job.placed).st_ino, ==, (guint64)stat_of(job.film).st_ino);
	/* And the hourly cleanup deletes nothing of it. */
	cleanup(fx);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	g_assert_true(g_file_test(job.place, G_FILE_TEST_IS_DIR));
	torrent_job_clear(&job);
}

/* Spec (import): with torrents left seeding, a torrent is hardlinked as
 * before, nothing asked of its client, and the page says why it was not
 * moved; a Usenet download is still moved. */
static void
test_move_torrent_seed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	TorrentJob job;

	moving(fx);
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, AXIS_IMPORT_TORRENTS_SEED);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	assert_qf(fx, "hardlink", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	text = q(fx, "SELECT mode_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "the torrent is left seeding");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	g_assert_cmpuint((guint64)stat_of(job.placed).st_ino, ==, (guint64)stat_of(job.film).st_ino);
	g_assert_cmpuint((guint)stat_of(job.film).st_nlink, ==, 2);
	assert_removal(fx, job.id, "kept", NULL);
	torrent_job_clear(&job);
}

/* A Usenet download of the made-up film, its files in downloads/@release
 * (the film, a sample, a subtitle, an .nfo), its client holding it; its import
 * requested. */
static gint64
lantern_usenet(
	Fixture     *fx,
	gint64       movie,
	const gchar *release,
	const gchar *ref,
	gint64      *download_out
){
	g_autofree gchar *dir = standard_download(fx, release, 3);
	g_autofree gchar *rel = g_strconcat("downloads/", release, NULL);
	g_autofree gchar *path = g_strconcat("/data/downloads/", release, NULL);
	gint64 download = add_download(fx, movie, "usenet", release, rel, ref);

	axis_test_download_fake_put(fx->fake, ref, release, "axis", NULL, "completed", 1000, 1000, path, NULL);
	if (download_out != NULL)
		*download_out = download;
	return request(fx, download);
}

/* Spec (import): a run that dies between any two steps of a move (before,
 * with the file under its temporary name, published, linked, the subtitle
 * half done, recorded before the client was told) and runs again: the import
 * converges, once, with each file in one place, never lost or doubled. */
static void
test_move_crash(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *step;
		guint        nth;
	} crashes[] = {
		{ "decided", 1 }, { "folder", 1 }, { "temp", 1 }, { "copied", 1 }, { "published", 1 }, { "linked", 1 },
		/* The subtitle: its temporary name recorded, then the file moved. */
		{ "temp", 2 }, { "copied", 2 }, { "published", 2 }, { "recorded", 1 }
	};
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	guint i;

	moving(fx);
	for (i = 0; i < G_N_ELEMENTS(crashes); i++) {
		g_autofree gchar *release = g_strdup_printf("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP%u", i);
		g_autofree gchar *ref = g_strdup_printf("nzo_lantern_crash_%u", i);
		g_autofree gchar *source = g_build_filename(fx->downloads, release, NULL);
		g_autofree gchar *source_film = g_strconcat(source, "/", release, ".mkv", NULL);
		g_autofree gchar *source_sub = g_strconcat(source, "/", release, ".en.srt", NULL);
		g_autofree gchar *message = NULL;
		guint64 film_ino = 0;
		gint64 download = 0;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, LANTERN_ID);
		id = lantern_usenet(fx, movie, release, ref, &download);
		film_ino = (guint64)stat_of(source_film).st_ino;
		hook_reset(fx);
		fx->hook.at = crashes[i].step;
		fx->hook.nth = crashes[i].nth;
		fx->hook.verdict = AXIS_IMPORT_HOOK_CRASH;
		message = run_failing(fx, id);
		assert_contains(message, crashes[i].step);
		/* The next run carries on from what is on disk and recorded (and the
		 * hourly cleanup tells the client, if the run died before it did). */
		hook_reset(fx);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		assert_state(fx, id, "imported");
		cleanup(fx);
		/* Once: the film and its subtitle, moved (the film the download's own
		 * inode), nothing doubled, nothing under a temporary name. */
		g_assert_cmpuint(count_videos(folder), ==, 1);
		g_assert_cmpuint(count_names(folder, "Marrowfield Lanterns (2011)"), ==, 2);
		g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
		g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
		assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND kind = 'video'"
			" AND inode = %" G_GUINT64_FORMAT, fx->root_id, film_ino);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
			" AND kind IN ('imported', 'upgraded')", movie);
		assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
		assert_qf(fx, "moved|moved", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
			" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
		/* And the client was told, once. */
		assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
		g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_IN_LIBRARY);
		clear_film_tree(fx, folder);
	}
}

/* Spec (import): Dismiss never leaves a file an interrupted run moved under
 * its temporary name with nothing that will move it: it puts the file back in
 * the download first, or, when that cannot be done, it is refused, saying
 * where the file is; Assign likewise (finding 4 of the stage 2 bug hunt: the
 * film stayed hidden in the library as .axis-partial-<row> for good). */
static void
test_dismiss_puts_moved_file_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *release = g_strdup("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPD");
	g_autofree gchar *source = g_build_filename(fx->downloads, release, NULL);
	g_autofree gchar *source_film = g_strconcat(source, "/", release, ".mkv", NULL);
	g_autofree gchar *away = g_strconcat(source, ".away", NULL);
	g_autofree gchar *reason = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, release, "nzo_lantern_dismiss", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);

	/* Its download's folder renamed away: it cannot be put back, so the
	 * dismissal is refused, saying where the file is; nothing changes. */
	g_assert_cmpint(g_rename(source, away), ==, 0);
	g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	assert_contains(error->message, ".axis-partial-");
	assert_contains(error->message, "Retry this import");
	g_clear_error(&error);
	g_assert_false(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_ASSIGN, -1, add_film(fx, "107"), "root", NULL,
		&error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
	assert_qf(fx, "0", "SELECT count(*) FROM import WHERE id = %" G_GINT64_FORMAT " AND state = 'dismissed'", id);

	/* With its folder back: put back first, then dismissed. */
	g_assert_cmpint(g_rename(away, source), ==, 0);
	g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint((guint64)stat_of(source_film).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 0);
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, id);
	reason = q(fx, "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND step IS NOT NULL", id);
	g_assert_cmpstr(reason, ==, "0");
	cleanup(fx);
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
}

/* Spec (import): an import dismissed before this rule, with a file an
 * interrupted run moved still under its temporary name, is repaired by the
 * hourly cleanup: the file put back in the download, or, when it cannot be,
 * the import opened again for a person, blocked, saying where the file is. */
static void
test_dismissed_moved_file_repaired(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	guint round;

	moving(fx);
	for (round = 0; round < 2; round++) {
		g_autofree gchar *release = g_strdup_printf("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPR%u", round);
		g_autofree gchar *ref = g_strdup_printf("nzo_lantern_repair_%u", round);
		g_autofree gchar *source = g_build_filename(fx->downloads, release, NULL);
		g_autofree gchar *source_film = g_strconcat(source, "/", release, ".mkv", NULL);
		g_autofree gchar *text = NULL;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, LANTERN_ID);
		id = lantern_usenet(fx, movie, release, ref, NULL);
		hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
		g_free(run_failing(fx, id));
		hook_reset(fx);
		/* Dismissed as it could be before: no settling. */
		exec(fx, "UPDATE import SET state = 'dismissed', reason = 'dismissed', finished_at = now() WHERE id = %"
			G_GINT64_FORMAT, id);
		if (round == 1)
			remove_tree(source);
		cleanup(fx);
		if (round == 0) {
			/* Put back: the dismissal is true now. */
			g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
			g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 0);
			assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, id);
		} else {
			/* Its download gone: opened again for a person, the file kept. */
			g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
			assert_state(fx, id, "blocked/moved_file_kept");
			text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
			assert_contains(text, "It was dismissed, but");
			assert_contains(text, ".axis-partial-");
			/* A retry gives it its name, once. */
			g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, NULL));
			g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
			g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 0);
			g_assert_cmpuint(count_videos(folder), ==, 1);
		}
		clear_film_tree(fx, folder);
	}
}

/* Review of the fixes, items 3 and 8: the hourly repair opens a dismissed
 * import again only on what is proven on disk now.  While storage is
 * read-only it writes nothing at all; a library folder or a download's folder
 * that cannot be opened now (not gone: refused) leaves the import dismissed,
 * for the next time; and one that cannot be settled yet does not hold back
 * those after it (it looked at the same first ones every hour before). */
static void
test_dismissed_repair_waits(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *source_a = NULL;
	g_autofree gchar *film_a = NULL;
	g_autofree gchar *film_b = NULL;
	gboolean root = geteuid() == 0;
	gint64 movie;
	gint64 a;
	gint64 b;
	guint round;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	for (round = 0; round < 2; round++) {
		g_autofree gchar *release = g_strdup_printf("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPW%u", round);
		g_autofree gchar *ref = g_strdup_printf("nzo_lantern_wait_%u", round);
		g_autofree gchar *source = g_build_filename(fx->downloads, release, NULL);
		gint64 id = lantern_usenet(fx, movie, release, ref, NULL);

		hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
		g_free(run_failing(fx, id));
		hook_reset(fx);
		/* Dismissed as it could be before: no settling; its grab closed, so
		 * that the film may have another. */
		exec(fx, "UPDATE import SET state = 'dismissed', reason = 'dismissed', finished_at = now() WHERE id = %"
			G_GINT64_FORMAT, id);
		exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed' WHERE closed_at IS NULL");
		if (round == 0) {
			a = id;
			source_a = g_strdup(source);
			film_a = g_strconcat(source, "/", release, ".mkv", NULL);
		} else {
			b = id;
			film_b = g_strconcat(source, "/", release, ".mkv", NULL);
		}
	}
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 2);
	g_assert_false(g_file_test(film_a, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(film_b, G_FILE_TEST_EXISTS));

	/* Storage read-only: nothing is put back, nothing opened again. */
	set_read_only(fx, TRUE);
	cleanup(fx);
	set_read_only(fx, FALSE);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 2);
	g_assert_false(g_file_test(film_a, G_FILE_TEST_EXISTS));
	assert_qf(fx, "2", "SELECT count(*) FROM import WHERE id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")"
		" AND state = 'dismissed'", a, b);
	if (root) {
		g_test_message("as root a folder's permissions refuse nothing: the rest is not tried");
		return;
	}

	/* The library folder cannot be opened now: neither is opened again. */
	g_assert_cmpint(g_chmod(folder, 0), ==, 0);
	cleanup(fx);
	g_assert_cmpint(g_chmod(folder, 0755), ==, 0);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 2);
	assert_qf(fx, "2", "SELECT count(*) FROM import WHERE id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")"
		" AND state = 'dismissed'", a, b);

	/* One at a time: the first's download folder cannot be opened now, so it
	 * waits; the second, after it, is put back the next time, not held. */
	fx->import->settle_batch = 1;
	g_assert_cmpint(g_chmod(source_a, 0), ==, 0);
	cleanup(fx);
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, a);
	g_assert_false(g_file_test(film_b, G_FILE_TEST_EXISTS));
	cleanup(fx);
	g_assert_true(g_file_test(film_b, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, b);
	g_assert_cmpint(g_chmod(source_a, 0755), ==, 0);
	g_assert_false(g_file_test(film_a, G_FILE_TEST_EXISTS));
	/* Its folder readable again: put back in its turn. */
	cleanup(fx);
	cleanup(fx);
	g_assert_true(g_file_test(film_a, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, a);
	g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 0);
	fx->import->settle_batch = 0;
	clear_film_tree(fx, folder);
}

/* Review of the fixes, item 8: Dismiss settles on the connection it holds
 * (its own transaction): with one connection in the pool it does not wait for
 * a second one. */
static void
test_dismiss_one_connection(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *release = g_strdup("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPO");
	g_autofree gchar *source_film = NULL;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, release, "nzo_lantern_one", NULL);
	source_film = g_strconcat(fx->downloads, "/", release, "/", release, ".mkv", NULL);
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
	{
		g_autoptr(AxisDbPool) one = axis_db_pool_new(axis_test_db_get_url(fx->t->db), 1, &error);
		AxisDbPool *before = fx->import->pool;

		g_assert_no_error(error);
		g_assert_nonnull(one);
		fx->import->pool = one;
		g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
		fx->import->pool = before;
		g_assert_no_error(error);
	}
	g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, id);
	clear_film_tree(fx, folder);
}

/* A run that died with the film moved under its temporary name, and then:
 * its folder in the download deleted by something else (forward to its final
 * name); its download name taken by a new file (forward, and that file is
 * left where it is); both its download name and its final name taken (kept
 * under its temporary name, the import blocked, nothing lost; a person makes
 * room and retries). */
static void
test_move_crash_settled(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUPS.mkv",
		NULL);
	guint round;

	moving(fx);
	for (round = 0; round < 3; round++) {
		g_autofree gchar *release = g_strdup_printf("Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPS");
		g_autofree gchar *ref = g_strdup_printf("nzo_lantern_settle_%u", round);
		g_autofree gchar *source = g_build_filename(fx->downloads, release, NULL);
		g_autofree gchar *source_film = g_strconcat(source, "/", release, ".mkv", NULL);
		g_autofree gchar *text = NULL;
		guint64 film_ino;
		gint64 movie;
		gint64 id;

		exec(fx, "DELETE FROM movie");
		movie = add_film(fx, LANTERN_ID);
		id = lantern_usenet(fx, movie, release, ref, NULL);
		film_ino = (guint64)stat_of(source_film).st_ino;
		hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
		g_free(run_failing(fx, id));
		hook_reset(fx);
		/* Moved: under its temporary name, its one name. */
		g_assert_false(g_file_test(source_film, G_FILE_TEST_EXISTS));
		g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
		if (round == 0) {
			/* The download's folder deleted by a cleanup of the downloads. */
			remove_tree(source);
		} else {
			/* A new file at the film's download name. */
			write_media(source_film, FILM_LINE, 200 * 1024, 7);
			if (round == 2)
				write_text(placed, "a file a person put at the film's name\n");
		}
		if (round < 2) {
			g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
			g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
			g_assert_cmpuint((guint)stat_of(placed).st_nlink, ==, 1);
			g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
			if (round == 1) {
				/* The new file is not Axis's: left, and the folder with it. */
				g_assert_true(g_file_test(source_film, G_FILE_TEST_IS_REGULAR));
				g_assert_cmpuint((guint64)stat_of(source_film).st_ino, !=, film_ino);
			}
		} else {
			g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
			assert_state(fx, id, "blocked/moved_file_kept");
			text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
			assert_contains(text, "kept in the library's folder under the temporary name");
			assert_contains(text, "Nothing of it was lost");
			/* Kept: the same file, its one name, recorded. */
			g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
			assert_qf(fx, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
				id);
			/* The cleanup does not remove a temporary name that is a file's
			 * only name. */
			cleanup(fx);
			g_assert_cmpuint(count_names(folder, ".axis-partial-"), ==, 1);
			/* A person makes room, and retries. */
			g_assert_cmpint(g_unlink(placed), ==, 0);
			{
				g_autoptr(GError) error = NULL;

				g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
				g_assert_no_error(error);
			}
			g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
			g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
			g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
		}
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
		clear_film_tree(fx, folder);
		if (g_file_test(source, G_FILE_TEST_IS_DIR))
			remove_tree(source);
	}
}

/* Spec (import): an upgrade with moves: the new file moved in first, the
 * film never without a file, the old one in the recycle folder for the
 * retention and then removed; an upgrade to the same name swaps the names,
 * also after a run that died with the new file moved under its temporary
 * name. */
static void
test_move_upgrade(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *old_path = g_build_filename(folder, LANTERN_FOLDER " [WEB-DL 1080p]-OLD.mkv", NULL);
	g_autofree gchar *recycled = g_build_filename(fx->disk, ".axis-recycle", "films", LANTERN_FOLDER,
		LANTERN_FOLDER " [WEB-DL 1080p]-OLD.mkv", NULL);
	g_autofree gchar *same = g_build_filename(folder, LANTERN_FOLDER "-MADEUP.mkv", NULL);
	g_autofree gchar *same_recycled = g_build_filename(fx->disk, ".axis-recycle", "films", LANTERN_FOLDER,
		LANTERN_FOLDER "-MADEUP.mkv", NULL);
	g_autofree gchar *file_id = NULL;
	AxisImportCleanupReport report;
	guint64 old_ino;
	guint64 new_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	write_media(old_path, FILM_LINE, 128 * 1024, 9);
	old_ino = (guint64)stat_of(old_path).st_ino;
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path LIKE '%%-OLD.mkv'",
		fx->root_id);
	g_assert_nonnull(file_id);
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, release_name) VALUES (%s, %" G_GINT64_FORMAT ","
		" 'webdl_1080p', 'Marrowfield.Lanterns.2011.1080p.WEB-DL.x264-OLD')", file_id, movie);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_up", NULL);
	{
		g_autofree gchar *source = g_build_filename(fx->downloads, LANTERN_RELEASE, LANTERN_RELEASE ".mkv", NULL);

		new_ino = (guint64)stat_of(source).st_ino;
	}
	hook_reset(fx);
	fx->hook.film_dir = g_strdup(folder);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint(fx->hook.without_film, ==, 0);
	assert_qf(fx, "true", "SELECT upgrade::text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	{
		g_autofree gchar *placed = g_build_filename(folder, LANTERN_PLACED, NULL);

		g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, new_ino);
	}
	g_assert_false(g_file_test(old_path, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint64)stat_of(recycled).st_ino, ==, old_ino);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'"
		" AND how = 'moved' AND remove_after > now() + interval '47 hours'", id);
	/* Due: removed by the cleanup, and only it. */
	exec(fx, "UPDATE import_recycle SET remove_after = now() - interval '1 second' WHERE import_id = %" G_GINT64_FORMAT,
		id);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.removed, ==, 1);
	g_assert_false(g_file_test(recycled, G_FILE_TEST_EXISTS));
	clear_film_tree(fx, folder);
	exec(fx, "DELETE FROM import_recycle");

	/* The same name, the run dying with the new file under its temporary
	 * name (the old one linked into the recycle folder already). */
	exec(fx, "DELETE FROM movie");
	set(fx, AXIS_IMPORT_SETTING_FORMAT, "{-group}");
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, "Marrowfield.Lanterns.2011.1080p.WEB-DL.x264-MADEUP", "nzo_lantern_same_1", NULL);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	old_ino = (guint64)stat_of(same).st_ino;
	id = lantern_usenet(fx, movie, "Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP", "nzo_lantern_same_2", NULL);
	{
		g_autofree gchar *source = g_build_filename(fx->downloads, "Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP",
			"Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP.mkv", NULL);

		new_ino = (guint64)stat_of(source).st_ino;
		hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
		g_free(run_failing(fx, id));
		g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
		g_assert_cmpuint((guint64)stat_of(same).st_ino, ==, old_ino);
		g_assert_cmpuint((guint64)stat_of(same_recycled).st_ino, ==, old_ino);
		hook_reset(fx);
		fx->hook.film_dir = g_strdup(folder);
		g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
		g_assert_cmpuint(fx->hook.without_film, ==, 0);
		g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	}
	g_assert_cmpuint((guint64)stat_of(same).st_ino, ==, new_ino);
	g_assert_cmpuint((guint)stat_of(same).st_nlink, ==, 1);
	g_assert_cmpuint((guint64)stat_of(same_recycled).st_ino, ==, old_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	assert_qf(fx, "bluray_1080p", "SELECT quality FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND inode = %"
		G_GUINT64_FORMAT, fx->root_id, new_ino);
}

/* Spec (import): after a move, the folder is deleted only when it is still
 * the folder the import walked (its inode recorded then), every video moved is
 * in the library as placed, and nothing in it is a video of feature size Axis
 * does not account for; each refusal leaves it, saying why; a folder already
 * gone is "gone". */
static void
test_move_folder_identity(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *stranger = NULL;
	g_autofree gchar *aside = NULL;
	g_autofree gchar *kept = NULL;
	UsenetJob job;

	moving(fx);
	/* Imported with the client away: the folder waits for its removal. */
	usenet_imported(fx, TRUE, &job);
	assert_folder(fx, job.id, "pending", NULL);
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));

	/* A video the import did not take and knows nothing of, of feature size:
	 * left, named. */
	stranger = g_build_filename(job.dir, "Another.Feature.2013.1080p.mkv", NULL);
	write_media(stranger, FILM_LINE, 256 * 1024, 8);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "it holds a video the import did not take and Axis knows nothing of");
	assert_folder(fx, job.id, "left", "Another.Feature.2013.1080p.mkv");
	g_assert_true(g_file_test(stranger, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(g_unlink(stranger), ==, 0);

	/* A small one (another sample) is no feature: it would go with the folder;
	 * but first the library's file is not there as placed: left. */
	aside = g_strconcat(job.placed, ".aside", NULL);
	g_assert_cmpint(g_rename(job.placed, aside), ==, 0);
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "is not there as it was placed");
	g_assert_cmpint(g_rename(aside, job.placed), ==, 0);

	/* Another folder of the same name (the first one gone, a new one made):
	 * not the folder the import walked. */
	kept = g_strconcat(job.dir, ".kept", NULL);
	g_assert_cmpint(g_rename(job.dir, kept), ==, 0);
	axis_test_storage_mkdir(job.dir);
	{
		g_autofree gchar *nfo = g_build_filename(job.dir, "new.nfo", NULL);

		write_text(nfo, "a new folder's nfo\n");
	}
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "not the folder the import walked");
	g_assert_true(g_file_test(job.dir, G_FILE_TEST_IS_DIR));
	remove_tree(job.dir);
	g_assert_cmpint(g_rename(kept, job.dir), ==, 0);

	/* All in order: deleted, with what the import did not take. */
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "done", "left in it that the import did not take");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));

	/* Gone already (a cleanup of the downloads got there first): gone, not
	 * an error. */
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "gone", "already gone");
	usenet_job_clear(&job);
}

/* Spec (import): a completed download whose files something else deleted
 * before its import, while it waited (storage read-only): the hourly cleanup
 * ends the import, saying where they were and when Axis last saw them, and it
 * is never tried again; the follow-up closes the grab (no blocklist entry),
 * removes the download from its client without its data, and the film is
 * wanted again, its next best looked for as it is monitored. */
static void
test_files_gone_waiting(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source = g_build_filename(fx->downloads, LANTERN_RELEASE, NULL);
	g_autofree gchar *text = NULL;
	AxisMovieFollowReport follow;
	gint64 download = 0;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_gone", &download);
	set_read_only(fx, TRUE);
	g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
	assert_state(fx, id, "waiting/read_only");
	/* A cleanup of the download folder deletes it, by age. */
	remove_tree(source);
	cleanup(fx);
	set_read_only(fx, FALSE);
	assert_state(fx, id, "dismissed/files_gone");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "no longer at Disk one: downloads/" LANTERN_RELEASE);
	assert_contains(text, "Axis last saw them there on ");
	assert_contains(text, "nothing is tried again");
	/* Never again: the job, the follow-up, the requeue. */
	g_assert_cmpstr(run(fx, id, NULL), ==, "dismissed");
	g_assert_cmpint(request(fx, download), ==, id);
	assert_state(fx, id, "dismissed/files_gone");

	/* The follow-up: the grab closed so, no blocklist entry, the download
	 * removed from its client without its data, one search for the next best. */
	memset(&follow, 0, sizeof(follow));
	g_assert_true(axis_movies_run_follow(fx->movies, &follow, &error));
	g_assert_no_error(error);
	assert_qf(fx, "files_gone", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, movie);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "nzo_lantern_gone"), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_qf(fx, "removed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'movies.search' AND payload->>'cause' = 'retry'");
	assert_qf(fx, "1", "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'removal'"
		" AND text LIKE '%%disappeared from the download folder before they were imported%%'", movie);
	g_assert_cmpint(movie_state(fx, movie), !=, AXIS_MOVIE_TO_IMPORT);
	/* Once. */
	memset(&follow, 0, sizeof(follow));
	g_assert_true(axis_movies_run_follow(fx->movies, &follow, &error));
	assert_qf(fx, "1", "SELECT count(*) FROM job WHERE kind = 'movies.search' AND payload->>'cause' = 'retry'");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "nzo_lantern_gone"), ==, 1);
}

/* Spec (import): the same for an import blocked for a person (two files that
 * could each be the film), and for one whose run finds nothing there; a film
 * that is not monitored is wanted again, and no search is queued for it. */
static void
test_files_gone_blocked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source = g_build_filename(fx->downloads, LANTERN_RELEASE, NULL);
	g_autofree gchar *second = g_build_filename(source, "Marrowfield.Lanterns.2011.Alternate.Cut.mkv", NULL);
	AxisMovieFollowReport follow;
	gint64 download = 0;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	exec(fx, "UPDATE movie SET monitored = false WHERE id = %" G_GINT64_FORMAT, movie);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_blocked", &download);
	write_media(second, FILM_LINE, 256 * 1024, 11);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/several");
	remove_tree(source);
	cleanup(fx);
	assert_state(fx, id, "dismissed/files_gone");
	memset(&follow, 0, sizeof(follow));
	g_assert_true(axis_movies_run_follow(fx->movies, &follow, &error));
	g_assert_no_error(error);
	assert_qf(fx, "files_gone", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	/* Not monitored: wanted again, looked for when a person asks. */
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'movies.search'");
	g_assert_cmpint(movie_state(fx, movie), ==, AXIS_MOVIE_WANTED);

	/* Its run finds nothing at the place: the same at once. */
	exec(fx, "DELETE FROM movie");
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_walk", &download);
	remove_tree(source);
	g_assert_cmpstr(run(fx, id, NULL), ==, "dismissed");
	assert_state(fx, id, "dismissed/files_gone");
	/* A volume that is offline tells nothing: an import of it is not ended. */
	exec(fx, "DELETE FROM movie");
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_offline", &download);
	set_read_only(fx, TRUE);
	g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
	set_read_only(fx, FALSE);
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	cleanup(fx);
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	assert_state(fx, id, "waiting/read_only");
}

/* Spec (import): something else deletes a companion file while the film is
 * moved: the import completes with what it has, the missing file noted; and
 * a seeding torrent's files deleted after its import (a cleanup by age) are
 * said to be gone while the library's file is in place. */
static void
test_files_gone_during_and_after(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *sub = g_build_filename(fx->downloads, LANTERN_RELEASE, LANTERN_RELEASE ".en.srt", NULL);
	g_autofree gchar *text = NULL;
	TorrentJob job;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_during", NULL);
	hook_reset(fx);
	fx->hook.act_at = "linked";
	fx->hook.act_unlink = g_strdup(sub);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint(count_videos(folder), ==, 1);
	g_assert_cmpuint(count_names(folder, "Marrowfield Lanterns (2011)"), ==, 1);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, LANTERN_RELEASE ".en.srt was not placed");
	clear_film_tree(fx, folder);
	exec(fx, "DELETE FROM movie");

	/* Left seeding (hardlinked), then its files deleted by a cleanup. */
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, AXIS_IMPORT_TORRENTS_SEED);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 2);
	cleanup(fx);
	assert_qf(fx, "-", "SELECT coalesce(folder_removal, '-') FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	remove_tree(job.place);
	cleanup(fx);
	assert_folder(fx, job.id, "gone", "deleted by something else after the import");
	assert_folder(fx, job.id, "gone", "The library's files are separate names of them and are in place");
	assert_removal(fx, job.id, "kept", NULL);
	g_assert_cmpuint((guint)stat_of(job.placed).st_nlink, ==, 1);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	torrent_job_clear(&job);
}

/* ── The review of import-by-move ───────────────────────────────── */

#define SETTLE_RELEASE "Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUPS"
#define SETTLE_PLACED  LANTERN_FOLDER " [BluRay 1080p][DTS-HD MA 5.1][x264]-MADEUPS.mkv"

/* The temporary name of the import's file of @role. */
static gchar *
temp_of(
	Fixture     *fx,
	gint64       id,
	const gchar *role
){
	g_autofree gchar *row = q(fx, "SELECT id FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = '%s'"
		" ORDER BY position LIMIT 1", id, role);

	g_assert_nonnull(row);
	return g_strdup_printf(".axis-partial-%s", row);
}

static void
retry(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_import_act(fx->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
}

/* Finding 1: a moved file kept under its temporary name (neither its download
 * name nor its final name free) that has another name outside the library
 * and the download (a backup's hardlink, a cross-seed): the hourly cleanup
 * keeps the temporary name, which is the file's one name in the library and
 * the download; it is removed only beside a name of its own there. */
static void
test_review_stranded_outside_link(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *source = g_build_filename(fx->downloads, SETTLE_RELEASE, NULL);
	g_autofree gchar *source_film = g_build_filename(source, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *outside = g_build_filename(fx->disk, "backup-of-a-film.mkv", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *temp = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_outside", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	temp_name = temp_of(fx, id, "film");
	temp = g_build_filename(folder, temp_name, NULL);
	g_assert_cmpuint((guint64)stat_of(temp).st_ino, ==, film_ino);
	/* A second name of it elsewhere on the disk, not Axis's. */
	g_assert_cmpint(link(temp, outside), ==, 0);
	/* Its download name and its final name both taken. */
	write_media(source_film, FILM_LINE, 200 * 1024, 7);
	write_text(placed, "a file a person put at the film's name\n");
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/moved_file_kept");
	/* The hourly cleanup: the temporary name stays, recorded. */
	cleanup(fx);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint((guint64)stat_of(temp).st_ino, ==, film_ino);
	assert_qf(fx, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	/* A person makes room and retries: the film gets its name, once. */
	g_assert_cmpint(g_unlink(placed), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	g_assert_cmpint(g_unlink(outside), ==, 0);
}

/* Finding 1, the other way: a moved file under its temporary name whose
 * download name leads to it again (a filesystem without RENAME_NOREPLACE left
 * both): the cleanup removes the temporary name beside it. */
static void
test_review_temp_beside_download(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *source = g_build_filename(fx->downloads, SETTLE_RELEASE, NULL);
	g_autofree gchar *source_film = g_build_filename(source, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *temp = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_beside", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	temp_name = temp_of(fx, id, "film");
	temp = g_build_filename(folder, temp_name, NULL);
	g_assert_cmpint(link(temp, source_film), ==, 0);
	exec(fx, "UPDATE import SET state = 'dismissed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, id);
	cleanup(fx);
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
	g_assert_cmpuint((guint64)stat_of(source_film).st_ino, ==, film_ino);
	assert_qf(fx, "-", "SELECT coalesce(step, '-') FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND role = 'film'", id);
}

/* Finding 4: a run died with the film under its temporary name, and the
 * library's folder cannot be opened at the next run (a person renamed it):
 * the row is kept, the import blocked saying where the file is, never ended
 * as files gone; the cleanup does not forget it either; once the folder is
 * back, a retry completes the import once. */
static void
test_review_folder_unreachable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *renamed = g_strconcat(folder, " (renamed)", NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *source = g_build_filename(fx->downloads, SETTLE_RELEASE, NULL);
	g_autofree gchar *source_film = g_build_filename(source, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *away = NULL;
	g_autofree gchar *text = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_away", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	temp_name = temp_of(fx, id, "film");
	g_assert_cmpint(g_rename(folder, renamed), ==, 0);
	away = g_build_filename(renamed, temp_name, NULL);
	g_assert_cmpuint((guint64)stat_of(away).st_ino, ==, film_ino);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/moved_file_kept");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, temp_name);
	assert_contains(text, LANTERN_FOLDER);
	assert_qf(fx, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_grab WHERE close_reason = 'files_gone'");
	/* The hourly cleanup does not forget it. */
	cleanup(fx);
	assert_qf(fx, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	g_assert_cmpuint((guint64)stat_of(away).st_ino, ==, film_ino);
	/* The folder back, a retry: imported once, the download's own file. */
	g_assert_cmpint(g_rename(renamed, folder), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* What the hourly sweep does last for an import it found waiting: the files'
 * absence seen, the import ended (axis_import_files_gone()). */
static void
sweep_now(
	Fixture *fx,
	gint64   import_id,
	gint64   file_id
){
	g_autoptr(GError) error = NULL;
	gboolean done = TRUE;

	g_assert_true(axis_import_files_gone(fx->import, import_id, TRUE, &done, &error));
	g_assert_no_error(error);
	fx->hook.act_result = done;
}

static void
dismiss_now(
	Fixture *fx,
	gint64   import_id,
	gint64   file_id
){
	exec(fx, "UPDATE import SET state = 'dismissed', reason = 'files_gone', finished_at = now() WHERE id = %"
		G_GINT64_FORMAT, import_id);
}

/* Finding 5: the hourly sweep found a download of one file waiting, and
 * ends it only after the import began and moved the file: the sweep's own
 * statement refuses (the import is running and has a file placed), and the
 * import completes; and an import whose state something changed while it ran
 * is not recorded as imported silently. */
static void
test_review_sweep_during_move(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *file = g_build_filename(fx->downloads, LANTERN_RELEASE ".mkv", NULL);
	g_autofree gchar *placed = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_PLACED, NULL);
	g_autofree gchar *message = NULL;
	guint64 film_ino;
	gint64 download;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	write_media(file, FILM_LINE, 256 * 1024, 3);
	film_ino = (guint64)stat_of(file).st_ino;
	download = add_download(fx, movie, "usenet", LANTERN_RELEASE, "downloads/" LANTERN_RELEASE ".mkv",
		"nzo_lantern_single");
	id = request(fx, download);
	hook_reset(fx);
	fx->hook.act_fn_at = "copied";
	fx->hook.act_fn = sweep_now;
	fx->hook.act_result = TRUE;
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_false(fx->hook.act_result);
	assert_state(fx, id, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_grab WHERE close_reason = 'files_gone'");
	{
		g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);

		clear_film_tree(fx, folder);
	}

	/* Its state changed under the run (by hand): the run fails, saying so. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*it is no longer running*");
	exec(fx, "DELETE FROM movie");
	movie = add_film(fx, LANTERN_ID);
	write_media(file, FILM_LINE, 256 * 1024, 4);
	download = add_download(fx, movie, "usenet", LANTERN_RELEASE, "downloads/" LANTERN_RELEASE ".mkv",
		"nzo_lantern_single_2");
	id = request(fx, download);
	hook_reset(fx);
	fx->hook.act_fn_at = "linked";
	fx->hook.act_fn = dismiss_now;
	message = run_failing(fx, id);
	assert_contains(message, "no longer running");
	hook_reset(fx);
}

static void
write_download_sub(
	Fixture *fx,
	gint64   import_id,
	gint64   file_id
){
	g_autofree gchar *sub = g_build_filename(fx->downloads, LANTERN_RELEASE, LANTERN_RELEASE ".en.srt", NULL);

	write_text(sub, "a new file at the subtitle's download name\n");
}

/* Finding 6: a subtitle moved under its temporary name could be neither
 * given its name (a person's file has it) nor put back (a new file has its
 * download name): the import is not recorded as imported, it is blocked
 * naming the file and where it is; once there is room, a retry places it. */
static void
test_review_stranded_sidecar(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, LANTERN_PLACED, NULL);
	g_autofree gchar *sub_final = g_build_filename(folder, LANTERN_SUB, NULL);
	g_autofree gchar *source_sub = g_build_filename(fx->downloads, LANTERN_RELEASE, LANTERN_RELEASE ".en.srt", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *text = NULL;
	guint64 sub_ino;
	gint64 movie;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*a moved file could not be put back in the download*");
	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, LANTERN_RELEASE, "nzo_lantern_sidecar", NULL);
	sub_ino = (guint64)stat_of(source_sub).st_ino;
	write_text(sub_final, "a person's own subtitle\n");
	hook_reset(fx);
	fx->hook.act_fn_at = "copied";
	fx->hook.act_fn_nth = 2;
	fx->hook.act_fn = write_download_sub;
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/moved_file_kept");
	hook_reset(fx);
	temp_name = temp_of(fx, id, "subtitle");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, LANTERN_RELEASE ".en.srt");
	assert_contains(text, temp_name);
	{
		g_autofree gchar *temp = g_build_filename(folder, temp_name, NULL);

		g_assert_cmpuint((guint64)stat_of(temp).st_ino, ==, sub_ino);
	}
	/* The film is in the library meanwhile. */
	g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
	/* Room is made; a retry places the subtitle, once. */
	g_assert_cmpint(g_unlink(sub_final), ==, 0);
	g_assert_cmpint(g_unlink(source_sub), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(sub_final).st_ino, ==, sub_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	g_assert_cmpuint(count_videos(folder), ==, 1);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

static void
re_add_torrent(
	Fixture *fx,
	gint64   import_id,
	gint64   file_id
){
	axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
		TOR_DONE "/axis/" LANTERN_RELEASE, NULL);
}

/* Finding 9: a torrent let go of by an earlier run, then added to its client
 * again before the run that moves its files (a person, an automation): it is
 * asked again, and let go of again, before anything is moved. */
static void
test_review_release_rechecked(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	/* Let go of, then the run dies before it moves anything. */
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	g_assert_false(axis_test_download_fake_has(fx->fake, TOR_REF));
	assert_removal(fx, job.id, "done", NULL);
	/* Added again at the client. */
	re_add_torrent(fx, job.id, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	hook_reset(fx);
	fx->hook.watch = TOR_REF;
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_cmpint(fx->hook.held_at_temp, ==, 0);
	g_assert_false(axis_test_download_fake_has(fx->fake, TOR_REF));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_removal(fx, job.id, "done", "removed from its client without its files");
	torrent_job_clear(&job);
}

/* Finding 9, the text: a run that died after Axis asked the client to let go
 * of the torrent, the client having removed it, before the answer was
 * recorded: the next run does not say that something else removed it. */
static void
test_review_release_text(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "release", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	axis_test_download_fake_drop(fx->fake, TOR_REF);
	hook_reset(fx);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	assert_removal(fx, job.id, "gone", "Axis had asked its client to let go of it");
	{
		g_autofree gchar *text = q(fx, "SELECT client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);

		g_assert_null(strstr(text, "something else removed it"));
	}
	torrent_job_clear(&job);
}

/* Finding 10: after a move the folder goes only with what may go with it: a
 * video of feature size the import looked at and did not take (too short to
 * be the film, and not named as an extra) leaves it, named; so does a large
 * file that is not media (a disc image); the sample, the .nfo, the par2 file
 * go with it. */
static void
test_review_folder_survey(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", LANTERN_RELEASE, NULL);
	g_autofree gchar *other = g_build_filename(category, "Marrowfield.Lanterns.2011.Footage.mkv", NULL);
	g_autofree gchar *iso = NULL;
	UsenetJob job;

	moving(fx);
	write_media(other, "AXISFAKE duration=600 width=1920 height=800 vcodec=h264", 200 * 1024, 9);
	usenet_imported(fx, TRUE, &job);
	assert_qf(fx, "f|t", "SELECT accepted::text::char || '|' || (reasons @> '[{\"c\":\"short\"}]')::text::char"
		" FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND path LIKE '%%Footage.mkv'", job.id);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "Marrowfield.Lanterns.2011.Footage.mkv");
	assert_folder(fx, job.id, "left", "the import looked at and did not take");
	g_assert_true(g_file_test(other, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(g_unlink(other), ==, 0);

	/* A disc image (not media), large: left too. */
	iso = g_build_filename(job.dir, "Marrowfield.Lanterns.2011.iso", NULL);
	write_media(iso, "not media", 200 * 1024, 4);
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "Marrowfield.Lanterns.2011.iso");
	g_assert_true(g_file_test(iso, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(g_unlink(iso), ==, 0);

	/* What is left is small, or the sample: it goes with the folder. */
	look_again(fx, &job);
	cleanup(fx);
	assert_folder(fx, job.id, "done", "left in it that the import did not take");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	usenet_job_clear(&job);
}

/* Finding 3 (the folder): a download Axis follows that is still at its client,
 * not completed, whose files the client says are inside the job's folder of an
 * import: the folder is left. */
static void
test_review_folder_live_download(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *inner = NULL;
	UsenetJob job;

	moving(fx);
	usenet_imported(fx, TRUE, &job);
	inner = g_build_filename(job.dir, "Gorse.Meadow.2015.1080p-MADEUP", NULL);
	write_text(inner, "a download still in progress\n");
	g_free(q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, module,"
		" state) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'nzo_gorse', 'usenet', 'file',"
		" 'Gorse.Meadow.2015.1080p-MADEUP', 'movies', 'downloading') RETURNING id", fx->client_id));
	axis_test_download_fake_put(fx->fake, "nzo_gorse", "Gorse.Meadow.2015.1080p-MADEUP", "axis", NULL, "downloading",
		1000, 10, SAB_JOB "/Gorse.Meadow.2015.1080p-MADEUP", NULL);
	cleanup(fx);
	assert_folder(fx, job.id, "left", "still at its client");
	g_assert_true(g_file_test(inner, G_FILE_TEST_IS_REGULAR));
	usenet_job_clear(&job);
}

/* ── The second review ──────────────────────────────────────────── */

/* Second review, item 2: a torrent let go of by an earlier run whose record
 * a person then removed on the Downloads page (its client gone too): the
 * import does not wait for ever to ask a client that cannot be asked; it
 * moves the files. */
static void
test_review2_release_record_removed(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	hook_reset(fx);
	assert_removal(fx, job.id, "done", NULL);
	exec(fx, "UPDATE download SET state = 'removed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, job.download);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	torrent_job_clear(&job);
}

/* Second review, item 2: the same, with a client that exists and cannot be
 * reached: the import waits a few times, saying so, then is blocked for a
 * person, who may import anyway. */
static void
test_review2_release_unreachable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	TorrentJob job;
	guint i;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	hook_reset(fx);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	for (i = 0; i + 1 < AXIS_IMPORT_PLACE_ATTEMPTS; i++) {
		g_assert_cmpstr(run(fx, job.id, NULL), ==, "waiting");
		assert_state(fx, job.id, "waiting/client_release");
		exec(fx, "UPDATE import SET retry_after = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	}
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "import anyway");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	/* A person says it is not at its client: imported. */
	g_assert_true(axis_import_act(fx->import, job.id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	torrent_job_clear(&job);
}

/* Second review, item 5: a run died after recording the film's step, before
 * the rename, and then a cleanup of the downloads deleted the download: the
 * import ends as files gone, never "a file moved out of the download". */
static void
test_review2_died_before_move(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *source = g_build_filename(fx->downloads, SETTLE_RELEASE, NULL);
	g_autofree gchar *text = NULL;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_before_move", NULL);
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	assert_qf(fx, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	remove_tree(source);
	g_assert_cmpstr(run(fx, id, NULL), ==, "dismissed");
	assert_state(fx, id, "dismissed/files_gone");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_null(strstr(text, "moved out of the download"));
}

/* Second review, item 6: a download its client keeps loose in its folder for
 * Axis (a torrent without a folder of its own reports that folder as its
 * place) does not keep an import's own job folder for ever. */
static void
test_review2_folder_loose_torrent(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	moving(fx);
	usenet_imported(fx, TRUE, &job);
	axis_test_download_fake_put(fx->fake, "hash_loose", "Loose.Pack.2019.1080p-MADEUP", "axis", NULL, "downloading",
		1000, 10, SAB_CATEGORY, NULL);
	cleanup(fx);
	assert_folder(fx, job.id, "done", "left in it that the import did not take");
	g_assert_false(g_file_test(job.dir, G_FILE_TEST_EXISTS));
	usenet_job_clear(&job);
}

/* Second review, item 6: a job's file straight in the category's folder,
 * moved by its import: "gone" comes before what the client has (a download
 * kept loose there). */
static void
test_review2_folder_loose_gone(Fixture *fx, gconstpointer data)
{
	UsenetJob job;

	moving(fx);
	usenet_imported_at(fx, TRUE, LANTERN_RELEASE ".mkv", LANTERN_RELEASE ".mkv", &job);
	axis_test_download_fake_put(fx->fake, "hash_loose", "Loose.Pack.2019.1080p-MADEUP", "axis", NULL, "downloading",
		1000, 10, SAB_CATEGORY, NULL);
	cleanup(fx);
	assert_folder(fx, job.id, "gone", NULL);
	usenet_job_clear(&job);
}

/* Second review, item 8: a run taken up again after the film was moved and
 * given its name, the torrent added to its client again meanwhile: it is let
 * go of again before the subtitle is moved. */
static void
test_review2_release_sidecars(Fixture *fx, gconstpointer data)
{
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "published", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	re_add_torrent(fx, job.id, 0);
	hook_reset(fx);
	fx->hook.watch = TOR_REF;
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_cmpint(fx->hook.held_at_temp, ==, 0);
	g_assert_false(axis_test_download_fake_has(fx->fake, TOR_REF));
	torrent_job_clear(&job);
}

/* ── The third review ───────────────────────────────────────────── */

/* At the film's "copied": a person puts a file at the film's final name. */
static void
take_final_name(
	Fixture *fx,
	gint64   import_id,
	gint64   file_id
){
	g_autofree gchar *placed = g_build_filename(fx->films, LANTERN_FOLDER, SETTLE_PLACED, NULL);

	write_text(placed, "a file a person put at the film's name\n");
}

static void
assert_film_step(
	Fixture     *fx,
	gint64       id,
	const gchar *step
){
	assert_qf(fx, step, "SELECT coalesce(step, '-') FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND role = 'film'", id);
}

/* Third review, item 1: a hardlink whose download's name something else
 * deletes after the link was made, and whose final name is taken: the
 * temporary name is the file's only one now; it keeps its record (the step,
 * the inode), the import stops for a person, naming it, and the cleanup keeps
 * it; once the name is free, a retry gives it its name. */
static void
test_review3_link_only_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *source_film = g_build_filename(fx->downloads, SETTLE_RELEASE, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *temp = NULL;
	g_autofree gchar *text = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*a temporary file could not be removed*");
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_link_only", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_reset(fx);
	fx->hook.act_at = "copied";
	fx->hook.act_unlink = g_strdup(source_film);
	fx->hook.act_fn_at = "copied";
	fx->hook.act_fn = take_final_name;
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	hook_reset(fx);
	assert_state(fx, id, "blocked/moved_file_kept");
	temp_name = temp_of(fx, id, "film");
	temp = g_build_filename(folder, temp_name, NULL);
	g_assert_cmpuint((guint64)stat_of(temp).st_ino, ==, film_ino);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, temp_name);
	assert_contains(text, LANTERN_FOLDER);
	assert_contains(text, "may be the only copy");
	assert_qf(fx, "temp|t", "SELECT step || '|' || (temp_inode = %" G_GUINT64_FORMAT ")::text::char FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", film_ino, id);
	/* The hourly cleanup keeps it, recorded. */
	cleanup(fx);
	g_assert_cmpuint((guint64)stat_of(temp).st_ino, ==, film_ino);
	assert_film_step(fx, id, "temp");
	/* Room made, a retry: the file gets its name, once. */
	g_assert_cmpint(g_unlink(placed), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* Third review, item 3: a run died between the two renames of a move (the
 * film under its temporary name in the movie's folder), and a person then
 * renamed that folder and made a new one with its name: the run does not take
 * the new folder for the one the file was put in; it stops for a person,
 * saying the folder was replaced and where the file is, never ending as files
 * gone; with the folder back, a retry imports it once. */
static void
test_review3_folder_replaced(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *renamed = g_strconcat(folder, " (renamed)", NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *source_film = g_build_filename(fx->downloads, SETTLE_RELEASE, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *temp_name = NULL;
	g_autofree gchar *away = NULL;
	g_autofree gchar *text = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_replaced", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	temp_name = temp_of(fx, id, "film");
	g_assert_cmpint(g_rename(folder, renamed), ==, 0);
	g_assert_cmpint(g_mkdir(folder, 0755), ==, 0);
	away = g_build_filename(renamed, temp_name, NULL);
	g_assert_cmpuint((guint64)stat_of(away).st_ino, ==, film_ino);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/folder_replaced");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "renamed or moved away, and another was made with its name");
	assert_contains(text, temp_name);
	assert_contains(text, LANTERN_FOLDER);
	assert_film_step(fx, id, "temp");
	assert_qf(fx, "0", "SELECT count(*) FROM movie_grab WHERE close_reason = 'files_gone'");
	g_assert_cmpuint((guint64)stat_of(away).st_ino, ==, film_ino);
	/* The folder back, a retry: imported once, the download's own file. */
	g_assert_cmpint(g_rmdir(folder), ==, 0);
	g_assert_cmpint(g_rename(renamed, folder), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* Third review, item 4: a run died after the film was moved and given its
 * name, and a person then renamed the film's file in the library: the next run
 * does not fail for ever, the import left running; it stops for a person,
 * saying what it found at the film's name; with the file back, a retry
 * completes the import. */
static void
test_review3_film_not_at_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *elsewhere = g_build_filename(folder, "renamed by a person.mkv", NULL);
	g_autofree gchar *source_film = g_build_filename(fx->downloads, SETTLE_RELEASE, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *text = NULL;
	guint64 film_ino;
	gint64 movie;
	gint64 id;

	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_not_at_name", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "published", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpint(g_rename(placed, elsewhere), ==, 0);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/film_file_changed");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "nothing has that name now");
	assert_contains(text, LANTERN_FOLDER);
	assert_film_step(fx, id, "published");
	/* Another file at its name: said so. */
	write_text(placed, "another file\n");
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "that name is another file's now");
	/* The film's file back under its name: imported. */
	g_assert_cmpint(g_unlink(placed), ==, 0);
	g_assert_cmpint(g_rename(elsewhere, placed), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* Third review, item 2: a torrent never let go of, whose client cannot be
 * reached: the import waits a few times, counted, then a person is asked;
 * "import anyway" moves the files, and the page says Axis went on without
 * the client. */
static void
test_review3_release_unreachable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	TorrentJob job;
	guint i;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	for (i = 0; i + 1 < AXIS_IMPORT_PLACE_ATTEMPTS; i++) {
		g_assert_cmpstr(run(fx, job.id, NULL), ==, "waiting");
		assert_state(fx, job.id, "waiting/client_release");
		exec(fx, "UPDATE import SET retry_after = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	}
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "its client could not be asked");
	assert_contains(text, "import anyway");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	g_assert_true(axis_import_act(fx->import, job.id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "A person chose to import anyway");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	torrent_job_clear(&job);
}

/* Third review, item 2: a torrent never let go of whose download a person
 * removed on the Downloads page (keeping its files) before its import: time
 * will not change that, so a person is asked at once, the cause said; and a
 * client that refuses the removal is asked a few times, counted, then a
 * person is. */
static void
test_review3_release_lasting(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	TorrentJob job;
	guint i;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	exec(fx, "UPDATE download SET state = 'removed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, job.download);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "removed on the Downloads page");
	assert_contains(text, "import anyway");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));

	/* Completed again, at a client that refuses what it lacks (it let go,
	 * without saying so): counted, then a person. */
	exec(fx, "UPDATE download SET state = 'completed' WHERE id = %" G_GINT64_FORMAT, job.download);
	axis_test_download_fake_say_gone(fx->fake, FALSE);
	retry(fx, job.id);
	for (i = 0; i + 1 < AXIS_IMPORT_PLACE_ATTEMPTS; i++) {
		g_assert_cmpstr(run(fx, job.id, NULL), ==, "waiting");
		assert_state(fx, job.id, "waiting/client_release");
		exec(fx, "UPDATE import SET retry_after = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	}
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "did not confirm that it let go of the torrent");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	torrent_job_clear(&job);
}

/* Third review, item 6: a torrent let go of by an earlier run, then its
 * download removed on the Downloads page, and the torrent added to its client
 * again: its client, asked by the torrent's hash, has it, so nothing is moved
 * and a person is asked; once it is gone from the client, the files are
 * moved, the page saying Axis went on without asking the client to let go of
 * it again, and why. */
static void
test_review3_released_asked_by_hash(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *text = NULL;
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	hook_reset(fx);
	assert_removal(fx, job.id, "done", NULL);
	assert_qf(fx, TOR_REF, "SELECT release_ref FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	exec(fx, "UPDATE download SET state = 'removed', finished_at = now() WHERE id = %" G_GINT64_FORMAT, job.download);
	re_add_torrent(fx, job.id, 0);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	text = q(fx, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, job.id);
	assert_contains(text, "asked by the torrent's hash, it lists it");
	g_assert_true(g_file_test(job.film, G_FILE_TEST_IS_REGULAR));
	/* Gone from its client: moved, and said. */
	axis_test_download_fake_drop(fx->fake, TOR_REF);
	retry(fx, job.id);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(job.film, G_FILE_TEST_EXISTS));
	assert_removal(fx, job.id, "done", "without asking its client to let go of it again");
	assert_removal(fx, job.id, "done", "asked by the torrent's hash, does not have it");
	torrent_job_clear(&job);
}

/* Third review, item 6: the same with the download's record gone from Axis
 * altogether: the client recorded with the first release is asked by the
 * torrent's hash; and with that client removed from Axis, the files are moved,
 * the page saying so. */
static void
test_review3_released_record_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	TorrentJob job;

	moving(fx);
	torrent_held(fx, TOR_LABEL, "axis/" LANTERN_RELEASE, &job);
	hook_at(fx, "temp", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, job.id));
	hook_reset(fx);
	exec(fx, "UPDATE import SET download_id = NULL WHERE id = %" G_GINT64_FORMAT, job.id);
	exec(fx, "UPDATE movie_grab SET download_id = NULL WHERE download_id = %" G_GINT64_FORMAT, job.download);
	exec(fx, "DELETE FROM download WHERE id = %" G_GINT64_FORMAT, job.download);
	re_add_torrent(fx, job.id, 0);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "blocked");
	assert_state(fx, job.id, "blocked/client_release");
	/* The client removed from Axis: nothing to ask; moved, and said. */
	g_assert_true(axis_downloads_delete_client(fx->dl, fx->client_id, &error));
	g_assert_no_error(error);
	retry(fx, job.id);
	g_assert_cmpstr(run(fx, job.id, NULL), ==, "imported");
	g_assert_true(g_file_test(job.placed, G_FILE_TEST_IS_REGULAR));
	assert_removal(fx, job.id, "done", "that client is no longer set up in Axis");
	torrent_job_clear(&job);
}

/* Design note of the third review: a moved film's folder that cannot be read
 * now (access refused) is looked at again a few times, counted, before a
 * person is asked; once it can be, the import completes. */
static void
test_review3_folder_unreadable_waits(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, SETTLE_PLACED, NULL);
	g_autofree gchar *source_film = g_build_filename(fx->downloads, SETTLE_RELEASE, SETTLE_RELEASE ".mkv", NULL);
	guint64 film_ino;
	gint64 movie;
	gint64 id;
	guint i;

	if (geteuid() == 0) {
		g_test_skip("access to a folder is never refused to root");
		return;
	}
	moving(fx);
	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_unreadable", NULL);
	film_ino = (guint64)stat_of(source_film).st_ino;
	hook_at(fx, "copied", AXIS_IMPORT_HOOK_CRASH);
	g_free(run_failing(fx, id));
	hook_reset(fx);
	g_assert_cmpint(g_chmod(folder, 0), ==, 0);
	for (i = 0; i + 1 < AXIS_IMPORT_PLACE_ATTEMPTS; i++) {
		g_assert_cmpstr(run(fx, id, NULL), ==, "waiting");
		assert_state(fx, id, "waiting/file_unreadable");
		exec(fx, "UPDATE import SET retry_after = NULL WHERE id = %" G_GINT64_FORMAT, id);
	}
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/moved_file_kept");
	assert_film_step(fx, id, "temp");
	g_assert_cmpint(g_chmod(folder, 0755), ==, 0);
	retry(fx, id);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, film_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
}

/* A file that is not among the files an import may replace (another movie's
 * by hand, or no movie's) lies at the very name the import would give the
 * film: the import does not replace it, and does not place beside it; it
 * stops for a person, and the file is as it was. */
static void
test_foreign_file_at_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *placed = g_build_filename(fx->films, LANTERN_FOLDER, SETTLE_PLACED, NULL);
	g_autofree gchar *source_film = g_build_filename(fx->downloads, SETTLE_RELEASE, SETTLE_RELEASE ".mkv", NULL);
	g_autofree gchar *text = NULL;
	g_autofree gchar *content = NULL;
	guint64 foreign_ino;
	gint64 movie;
	gint64 id;

	movie = add_film(fx, LANTERN_ID);
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_foreign", NULL);
	write_text(placed, "a file of another movie, at this movie's name\n");
	foreign_ino = (guint64)stat_of(placed).st_ino;
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/name_taken");
	text = q(fx, "SELECT state || coalesce('/' || reason, '') || ' ' || coalesce(reason_text, '') FROM import WHERE id = %"
		G_GINT64_FORMAT, id);
	g_test_message("foreign file at the name: %s", text);
	g_assert_true(g_file_get_contents(placed, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "a file of another movie, at this movie's name\n");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, foreign_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	g_assert_cmpint(g_file_test(source_film, G_FILE_TEST_EXISTS) ? 1 : 0, ==, 1);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* The same, with the file in the way an indexed library file that a person
 * linked by hand to another movie: it is blocked the same way, and the file
 * and its link are as they were. */
static void
test_linked_file_at_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *placed = g_build_filename(fx->films, LANTERN_FOLDER, SETTLE_PLACED, NULL);
	g_autofree gchar *content = NULL;
	g_autofree gchar *file_id = NULL;
	guint64 foreign_ino;
	gint64 movie;
	gint64 other;
	gint64 id;

	movie = add_film(fx, LANTERN_ID);
	other = add_film(fx, "108");
	id = lantern_usenet(fx, movie, SETTLE_RELEASE, "nzo_lantern_linked", NULL);
	write_text(placed, "a file linked by hand to another movie\n");
	foreign_ino = (guint64)stat_of(placed).st_ino;
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE root_id = %" G_GINT64_FORMAT " AND rel_path LIKE '%%%s'",
		fx->root_id, SETTLE_PLACED);
	g_assert_nonnull(file_id);
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, hand_how, hand_by, hand_at, kept) VALUES (%s, %"
		G_GINT64_FORMAT ", 'webdl_1080p', 'linked', 'root', now(), true)", file_id, other);
	g_assert_cmpstr(run(fx, id, NULL), ==, "blocked");
	assert_state(fx, id, "blocked/name_taken");
	g_assert_true(g_file_get_contents(placed, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "a file linked by hand to another movie\n");
	g_assert_cmpuint((guint64)stat_of(placed).st_ino, ==, foreign_ino);
	g_assert_cmpuint(count_names(fx->films, ".axis-"), ==, 0);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE file_id = %s AND movie_id = %" G_GINT64_FORMAT
		" AND hand_how = 'linked'", file_id, other);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* ── Delete this download (add-replace-grab-and-delete-download, B) ── */

#define UNWANTED_REF "SABnzbd_nzo_unwanted"
#define ADA          "ada"

/*
 * A download of the made-up film that completed at its client and whose
 * import waits for a person (blocked): the thing "Delete this download" is
 * for.  Its place and the inode of what was there are recorded as tracking
 * records them.
 */
typedef struct
{
	gint64  movie;
	gint64  download;
	gint64  id;
	gchar  *place;		/* the download's folder or file on the volume */
	gchar  *category;	/* the client's folder for Axis on the volume */
	gchar  *beside;		/* another download's file in that folder */
	gchar  *ref;
} Unwanted;

static void
unwanted_clear(Unwanted *u)
{
	g_free(u->place);
	g_free(u->category);
	g_free(u->beside);
	g_free(u->ref);
}

static void
unwanted_record(
	Fixture     *fx,
	Unwanted    *u,
	const gchar *protocol,
	const gchar *ref,
	const gchar *client_path,
	const gchar *rel_path,
	gboolean     with_inode
){
	g_autofree gchar *title = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *inode = with_inode ? g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)stat_of(u->place).st_ino) :
		g_strdup("NULL");

	u->ref = g_strdup(ref);
	title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, u->movie);
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path,"
		" location_inode, sent_at, completed_at) VALUES (%" G_GINT64_FORMAT ", 'Client', '%s', '%s', 'file', '%s', %s,"
		" 'Marrowfield Lanterns (2011)', 'movies', 'completed', 1000, 1000, '%s', %" G_GINT64_FORMAT ", '%s', %s,"
		" now(), now()) RETURNING id", fx->client_id, ref, protocol, LANTERN_RELEASE, title, client_path, fx->volume_id,
		rel_path, inode);
	u->download = g_ascii_strtoll(download, NULL, 10);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '%s', '%s', lower('1:0:%s'), %" G_GINT64_FORMAT ", now(), now())"
		" RETURNING id", u->movie, LANTERN_RELEASE, protocol, LANTERN_RELEASE, u->download);
	g_assert_nonnull(grab);
	u->id = request(fx, u->download);
	/* A person must decide: the import is blocked (as a run that read the
	 * wrong film's name leaves it). */
	exec(fx, "UPDATE import SET state = 'blocked', reason = 'wrong_name', reason_text = 'The file names another"
		" film.' WHERE id = %" G_GINT64_FORMAT, u->id);
}

/* The client's path mapping from @prefix to @rel on the volume, once. */
static void
ensure_map(
	Fixture     *fx,
	const gchar *prefix,
	const gchar *rel
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *mapped = q(fx, "SELECT count(*) FROM download_path_map WHERE client_id = %" G_GINT64_FORMAT
		" AND client_prefix = '%s'", fx->client_id, prefix);

	if (g_strcmp0(mapped, "0") != 0)
		return;
	g_assert_true(axis_downloads_add_path_map(fx->dl, fx->client_id, prefix, fx->volume_id, rel, &error));
	g_assert_no_error(error);
}

/* A Usenet job in its own folder inside the category's, which SABnzbd keeps
 * when the job is removed with its files. */
static void
unwanted_usenet(
	Fixture  *fx,
	Unwanted *u
){
	memset(u, 0, sizeof(*u));
	u->movie = add_film(fx, LANTERN_ID);
	u->place = g_build_filename(fx->downloads, "axis", LANTERN_RELEASE, NULL);
	u->category = g_build_filename(fx->downloads, "axis", NULL);
	u->beside = g_build_filename(u->category, OTHER_JOB_FILE, NULL);
	write_job(fx, LANTERN_RELEASE "/" LANTERN_RELEASE ".mkv");
	write_text(u->beside, "another job's nfo\n");
	ensure_map(fx, SAB_COMPLETE, "downloads");
	axis_test_download_fake_keep_completed(fx->fake, TRUE);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);
	axis_test_download_fake_put(fx->fake, UNWANTED_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000, SAB_JOB,
		NULL);
	unwanted_record(fx, u, "usenet", UNWANTED_REF, SAB_JOB, JOB_REL, TRUE);
}

/* A torrent in its own folder inside the folder of Axis's label (@label_dir;
 * %NULL: the client keeps Axis's torrents in no folder of their own), which
 * its client still has (@at_client: it deletes the files itself when asked,
 * as a torrent client does) or no longer has. */
static void
unwanted_torrent(
	Fixture     *fx,
	const gchar *label_dir,
	gboolean     at_client,
	Unwanted    *u
){
	g_autofree gchar *film = NULL;
	g_autofree gchar *nfo = NULL;

	memset(u, 0, sizeof(*u));
	u->movie = add_film(fx, LANTERN_ID);
	u->category = g_build_filename(fx->downloads, "done", "axis", NULL);
	u->place = g_build_filename(u->category, LANTERN_RELEASE, NULL);
	u->beside = g_build_filename(u->category, OTHER_JOB_FILE, NULL);
	film = g_build_filename(u->place, LANTERN_RELEASE ".mkv", NULL);
	nfo = g_build_filename(u->place, LANTERN_RELEASE ".nfo", NULL);
	write_media(film, FILM_LINE, 256 * 1024, 5);
	write_text(nfo, "an nfo\n");
	write_text(u->beside, "another torrent's nfo\n");
	ensure_map(fx, TOR_ROOT, "downloads");
	axis_test_download_fake_set_category_dir(fx->fake, label_dir);
	axis_test_download_fake_keep_completed(fx->fake, FALSE);
	axis_test_download_fake_set_data_dir(fx->fake, u->category);
	if (at_client)
		axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
			TOR_LABEL "/" LANTERN_RELEASE, NULL);
	else
		axis_test_download_fake_say_gone(fx->fake, TRUE);
	unwanted_record(fx, u, "torrent", TOR_REF, TOR_LABEL "/" LANTERN_RELEASE, "downloads/done/axis/" LANTERN_RELEASE,
		TRUE);
}

static AxisImportDeletePreview *
delete_look(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;
	AxisImportDeletePreview *preview = NULL;

	g_assert_true(axis_import_delete_preview(fx->import, id, &preview, &error));
	g_assert_no_error(error);
	g_assert_nonnull(preview);
	return preview;
}

/* The confirmation, then its post, as the page makes them. */
static AxisImportDeleteOutcome
delete_now(
	Fixture  *fx,
	gint64    id,
	gboolean  block
){
	g_autoptr(AxisImportDeletePreview) preview = delete_look(fx, id);
	g_autoptr(GError) error = NULL;
	AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_WAITING;

	if (!preview->allowed)
		g_error("import %" G_GINT64_FORMAT ": its download may not be deleted: %s", id, preview->refusal);
	g_assert_true(axis_import_delete_download(fx->import, id, preview->digest, block, ADA, &outcome, &error));
	g_assert_no_error(error);
	return outcome;
}

/*
 * A rule refuses: the confirmation says so in @words and offers nothing, and
 * a post (forged: with whatever digest the page had) changes nothing: the
 * import is still blocked, the grab open, the download recorded as it was,
 * the client not asked to remove anything, and the place untouched.
 */
static void
assert_delete_refused(
	Fixture     *fx,
	Unwanted    *u,
	const gchar *watch,
	const gchar *words,
	gboolean     waits
){
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = axis_test_storage_digest(watch);
	g_autofree gchar *after = NULL;
	guint removals = axis_test_download_fake_count(fx->fake, "remove", NULL);
	AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_DELETED;

	preview = delete_look(fx, u->id);
	if (preview->allowed)
		g_error("import %" G_GINT64_FORMAT ": its download may be deleted, and a rule should refuse (%s)", u->id, words);
	assert_contains(preview->refusal, words);
	g_assert_cmpint(preview->waits, ==, waits);
	g_assert_false(axis_import_delete_download(fx->import, u->id, preview->digest, FALSE, ADA, &outcome, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	assert_contains(error->message, words);
	assert_state(fx, u->id, "blocked/wrong_name");
	assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, u->download);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		u->download);
	assert_qf(fx, "0", "SELECT count(*) FROM import_action WHERE import_id = %" G_GINT64_FORMAT
		" AND action = 'delete_download'", u->id);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removals);
	after = axis_test_storage_digest(watch);
	g_assert_cmpstr(after, ==, before);
}

/* Spec (import): A Usenet download.  The import is dismissed with the
 * person's name, the client has removed the job, the folder is gone before
 * the call answers, the grab is closed, the movie is wanted again and no
 * search was queued; the confirmation wrote nothing; recorded with the
 * person's name; the release is not blocked without the tick. */
static void
test_delete_download_usenet(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *jobs = NULL;
	g_autofree gchar *text = NULL;
	Unwanted u;

	unwanted_usenet(fx, &u);
	before = axis_test_storage_digest(fx->disk);
	jobs = q(fx, "SELECT count(*) FROM job");

	/* The confirmation: what goes, who deletes, and nothing written. */
	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	g_assert_false(preview->gone);
	g_assert_false(preview->file);
	g_assert_cmpint(preview->by, ==, AXIS_IMPORT_DELETE_BY_AXIS_KEPT);
	g_assert_cmpstr(preview->path, ==, JOB_REL);
	g_assert_cmpstr(preview->client_path, ==, SAB_JOB);
	g_assert_cmpstr(preview->volume_name, ==, "Disk one");
	/* The film, its sample, the subtitle, the .nfo, the par2 file and the
	 * proof in its folder. */
	g_assert_cmpuint(preview->n_files, ==, 6);
	g_assert_cmpuint(preview->bytes, >, 256 * 1024);
	g_assert_true(preview->has_grab);
	g_assert_cmpuint(preview->placed->len, ==, 0);
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(after, ==, before);
	assert_state(fx, u.id, "blocked/wrong_name");
	assert_qf(fx, "completed/-", "SELECT state || '/' || coalesce(folder_removal, '-') FROM download WHERE id = %"
		G_GINT64_FORMAT, u.download);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		u.download);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	assert_qf(fx, "0", "SELECT count(*) FROM import_action WHERE import_id = %" G_GINT64_FORMAT, u.id);

	/* Confirmed. */
	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	assert_state(fx, u.id, "dismissed/download_deleted");
	assert_qf(fx, "The download was deleted by " ADA ".", "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT,
		u.id);
	assert_removal(fx, u.id, "done", "The job was removed from its client.");
	g_assert_false(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-kept-files", UNWANTED_REF), ==, 1);
	/* The folder, with everything in it; the category's folder and what is
	 * beside it stay. */
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	assert_download_files(fx, u.download, "done", "Its folder at the client (" SAB_JOB ") was deleted");
	assert_qf(fx, "removed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, u.download);
	/* The grab closed, with no removal of its own; the movie wanted again;
	 * no search queued, and nothing on the blocklist. */
	assert_qf(fx, "deleted/-", "SELECT close_reason || '/' || coalesce(removal, '-') FROM movie_grab WHERE download_id = %"
		G_GINT64_FORMAT, u.download);
	g_assert_cmpint(movie_state(fx, u.movie), ==, AXIS_MOVIE_WANTED);
	{
		g_autofree gchar *jobs_after = q(fx, "SELECT count(*) FROM job");

		g_assert_cmpstr(jobs_after, ==, jobs);
	}
	assert_qf(fx, "0", "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, u.movie);
	/* Recorded with the person's name, on the import and in the movie's
	 * history. */
	assert_qf(fx, ADA, "SELECT by_user FROM import_action WHERE import_id = %" G_GINT64_FORMAT
		" AND action = 'delete_download' AND created_at IS NOT NULL", u.id);
	text = q(fx, "SELECT text FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'removal'"
		" AND by_user = '" ADA "' AND created_at IS NOT NULL", u.movie);
	assert_contains(text, LANTERN_RELEASE ": its download was deleted by " ADA " from the import's page.");
	assert_contains(text, "wanted again; no search was started");

	/* Once: the hourly cleanup finds nothing more to do. */
	cleanup(fx);
	assert_removal(fx, u.id, "done", NULL);
	unwanted_clear(&u);
}

/* Spec (import): A torrent its client still has, at the place recorded when
 * it completed: the client is asked to remove it with its data, and Axis
 * deletes nothing itself. */
static void
test_delete_download_torrent_at_client(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	Unwanted u;

	/* With no folder of Axis's own at the client: the client deletes, so
	 * that folder is not needed. */
	unwanted_torrent(fx, NULL, TRUE, &u);
	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	g_assert_cmpint(preview->by, ==, AXIS_IMPORT_DELETE_BY_CLIENT);
	g_assert_cmpuint(preview->n_files, ==, 2);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	assert_state(fx, u.id, "dismissed/download_deleted");
	assert_removal(fx, u.id, "done", "The torrent was removed from its client with its downloaded files.");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", TOR_REF), ==, 1);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	/* Axis looked at nothing to delete itself. */
	assert_download_files(fx, u.download, "-", NULL);
	assert_qf(fx, "deleted", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, u.download);
	unwanted_clear(&u);
}

/* Spec (import): A torrent its client no longer has (the client dropped the
 * torrent and left its folder): Axis deletes the folder under the rules of
 * a leftover, whatever the setting for torrents after an import says.  Also
 * when tracking had found it gone before the person asked. */
static void
test_delete_download_torrent_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	Unwanted u;

	unwanted_torrent(fx, TOR_LABEL, FALSE, &u);
	/* The setting leaves torrents seeding: it is about what Axis does by
	 * itself after an import, and this was not imported. */
	set(fx, AXIS_IMPORT_SETTING_TORRENTS, "seed");
	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	g_assert_cmpint(preview->by, ==, AXIS_IMPORT_DELETE_BY_AXIS_GONE);
	g_assert_cmpuint(preview->n_files, ==, 2);

	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	assert_state(fx, u.id, "dismissed/download_deleted");
	assert_removal(fx, u.id, "gone", "It was already gone from its client");
	assert_download_files(fx, u.download, "done", "Its folder at the client (" TOR_LABEL "/" LANTERN_RELEASE
		") was deleted");
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);

	/* Recorded gone already (tracking saw the client drop it): the same. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_torrent(fx, TOR_LABEL, FALSE, &u);
	exec(fx, "UPDATE download SET state = 'gone' WHERE id = %" G_GINT64_FORMAT, u.download);
	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	assert_removal(fx, u.id, "gone", "It was already gone from its client");
	assert_download_files(fx, u.download, "done", "was deleted");
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);

	/* With no folder of Axis's own, a torrent's leftover folder is never
	 * deleted by Axis: refused, saying where it is. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_torrent(fx, NULL, FALSE, &u);
	assert_delete_refused(fx, &u, fx->disk, "no folder Axis may delete in", FALSE);
	assert_delete_refused(fx, &u, fx->disk, "The files are at " TOR_LABEL "/" LANTERN_RELEASE " at the client.", FALSE);
	unwanted_clear(&u);
}

/* A torrent that was imported, and that a person then moved in its client
 * into the films' library folder before the client could be asked to remove
 * it: the removal after its import asks the client where it is (the downloads
 * layer's place rule), removes it without its data, deletes nothing in the
 * library, and the import says so. */
static void
test_torrent_removed_in_library(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = standard_download(fx, HEARTH_RELEASE, 1);
	g_autofree gchar *moved_dir = g_build_filename(fx->films, HEARTH_RELEASE, NULL);
	g_autofree gchar *moved = g_build_filename(fx->films, HEARTH_RELEASE, "moved-by-hand.mkv", NULL);
	g_autofree gchar *text = NULL;
	AxisImportCleanupReport cleanup;
	gint64 movie = add_film(fx, "107");
	gint64 download = 0;
	gint64 id;

	axis_test_download_fake_put(fx->fake, "torrent-lib", HEARTH_RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/data/downloads/" HEARTH_RELEASE, NULL);
	/* The client is unreachable when the import ends: the removal waits. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	download = add_download(fx, movie, "torrent", HEARTH_RELEASE, "downloads/" HEARTH_RELEASE, "torrent-lib");
	id = request(fx, download);
	g_assert_cmpstr(run(fx, id, NULL), ==, "imported");
	assert_qf(fx, "failed", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);

	/* Moved in the client into the films' root, which the client sees as
	 * /library; asked to remove it with its data, the client deletes there. */
	ensure_map(fx, "/library", "films");
	axis_test_storage_mkdir(moved_dir);
	axis_test_storage_write(moved, "the torrent, in the library now");
	axis_test_download_fake_set_data_dir(fx->fake, fx->films);
	axis_test_download_fake_set(fx->fake, "torrent-lib", "completed", 1000, "/library/" HEARTH_RELEASE, NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	memset(&cleanup, 0, sizeof(cleanup));
	g_assert_true(axis_import_run_cleanup(fx->import, 0, &cleanup, &error));
	g_assert_no_error(error);
	/* (First: what is in the library is still there.) */
	g_assert_true(g_file_test(moved, G_FILE_TEST_EXISTS));
	g_assert_false(axis_test_download_fake_has(fx->fake, "torrent-lib"));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", "torrent-lib"), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_qf(fx, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	text = q(fx, "SELECT client_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_contains(text, "Removed from its client without its data. Its files were left where they are: its client "
		"says the torrent's files are at /library/" HEARTH_RELEASE ", which is inside a library folder.");
	assert_qf(fx, "imported", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	(void)dir;
}

/* Spec (import): A torrent pointed into a library folder: the client is not
 * asked to remove it with its data, and the page says why.  Also a torrent
 * the client says is somewhere else now, and one reported at a folder that
 * is not the torrent's own. */
static void
test_delete_download_torrent_in_library(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *film = NULL;
	Unwanted u;

	/* The client reports the torrent inside the films' root. */
	memset(&u, 0, sizeof(u));
	u.movie = add_film(fx, LANTERN_ID);
	u.category = g_strdup(fx->films);
	u.place = g_build_filename(fx->films, LANTERN_RELEASE, NULL);
	film = g_build_filename(u.place, LANTERN_RELEASE ".mkv", NULL);
	write_media(film, FILM_LINE, 256 * 1024, 5);
	ensure_map(fx, "/library", "films");
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	axis_test_download_fake_keep_completed(fx->fake, FALSE);
	axis_test_download_fake_set_data_dir(fx->fake, fx->films);
	axis_test_download_fake_put(fx->fake, TOR_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/library/" LANTERN_RELEASE, NULL);
	unwanted_record(fx, &u, "torrent", TOR_REF, "/library/" LANTERN_RELEASE, "films/" LANTERN_RELEASE, TRUE);
	assert_delete_refused(fx, &u, fx->disk, "it is inside the library root films", FALSE);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);

	/* In its own folder in the downloads, but the client says it is
	 * somewhere else now (a person moved it). */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_torrent(fx, NULL, TRUE, &u);
	axis_test_download_fake_set(fx->fake, TOR_REF, "completed", 1000, "/library/" LANTERN_RELEASE, NULL);
	assert_delete_refused(fx, &u, fx->disk, "which is not the place recorded when it completed", FALSE);
	/* Reported at a folder that is not named as the torrent is (a torrent
	 * with no folder of its own is reported at the client's download folder
	 * itself): never. */
	axis_test_download_fake_put(fx->fake, TOR_REF, "Another.Name.2011-MADEUP", "axis", NULL, "completed", 1000, 1000,
		TOR_LABEL "/" LANTERN_RELEASE, NULL);
	assert_delete_refused(fx, &u, fx->disk, "is not named as the torrent is", FALSE);
	unwanted_clear(&u);
}

/* The torrent of @u, files and all, moved in its client into the films' root
 * after it completed; the client (as a torrent client does) deletes it where
 * it is now when asked to remove it with its data.  With @client_says, the
 * client reports the new place; without, it still reports the old one.
 * Returns the film's file in the library (owned). */
static gchar *
move_into_library(
	Fixture  *fx,
	Unwanted *u,
	gboolean  client_says
){
	g_autofree gchar *moved = g_build_filename(fx->films, LANTERN_RELEASE, NULL);
	gchar *film = g_build_filename(moved, LANTERN_RELEASE ".mkv", NULL);

	g_assert_cmpint(g_rename(u->place, moved), ==, 0);
	ensure_map(fx, "/library", "films");
	axis_test_download_fake_set_data_dir(fx->fake, fx->films);
	if (client_says)
		axis_test_download_fake_set(fx->fake, TOR_REF, "completed", 1000, "/library/" LANTERN_RELEASE, NULL);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	return film;
}

/* Spec (import): A torrent moved in its client after it completed (the
 * independent review's blocker): nothing is at the recorded place, the
 * client still lists the torrent, and asked to remove it with its data it
 * would delete it where it is now, in the library.  Refused on the page; a
 * post changes nothing; the client is never asked; the library's file stays.
 * When the client no longer lists it either, nothing is left to delete: the
 * import is closed and the client is asked for nothing with data. */
static void
test_delete_download_torrent_moved(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *film = NULL;
	g_autofree gchar *moved = g_build_filename(fx->films, LANTERN_RELEASE, NULL);
	AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_WAITING;
	gboolean posted;
	gboolean allowed;
	Unwanted u;

	/* Moved, and the client says where it is now. */
	unwanted_torrent(fx, NULL, TRUE, &u);
	film = move_into_library(fx, &u, TRUE);
	preview = delete_look(fx, u.id);
	allowed = preview->allowed;
	posted = axis_import_delete_download(fx->import, u.id, preview->digest, FALSE, ADA, &outcome, &error);
	/* First of all: the library's file, and what the client was asked. */
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_false(allowed);
	g_assert_false(posted);
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	g_clear_error(&error);
	assert_contains(preview->refusal, "the torrent is no longer where Axis recorded it");
	assert_contains(preview->refusal, "probably moved in the client");
	assert_contains(preview->refusal, "Axis will not delete it");
	assert_contains(preview->refusal, "remove it in the client, or dismiss the import");
	/* Where it is now, not where it was. */
	assert_contains(preview->refusal, "/library/" LANTERN_RELEASE);
	g_assert_null(strstr(preview->refusal, "The files are at " TOR_LABEL));
	assert_state(fx, u.id, "blocked/wrong_name");
	assert_delete_refused(fx, &u, fx->disk, "the torrent is no longer where Axis recorded it", FALSE);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	g_clear_pointer(&film, g_free);
	g_clear_pointer(&preview, axis_import_delete_preview_free);
	unwanted_clear(&u);

	/* Moved, and the client still reports the place it completed at. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	remove_tree(moved);
	unwanted_torrent(fx, NULL, TRUE, &u);
	film = move_into_library(fx, &u, FALSE);
	assert_delete_refused(fx, &u, fx->disk, "the torrent is no longer where Axis recorded it", FALSE);
	assert_delete_refused(fx, &u, fx->disk, "its client still lists the torrent", FALSE);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_clear_pointer(&film, g_free);
	unwanted_clear(&u);

	/* Moved, and the client does not list the torrent any more: nothing is
	 * left to delete.  The import is closed; nothing is asked with data. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	remove_tree(moved);
	axis_test_download_fake_drop(fx->fake, TOR_REF);
	unwanted_torrent(fx, TOR_LABEL, FALSE, &u);
	film = move_into_library(fx, &u, FALSE);
	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	g_assert_true(preview->gone);
	g_assert_cmpint(preview->by, ==, AXIS_IMPORT_DELETE_BY_AXIS_GONE);
	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_GONE);
	assert_state(fx, u.id, "dismissed/download_deleted");
	assert_removal(fx, u.id, "gone", "already gone from its client");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);
}

/* Spec (import): The same for the hourly step.  The request was recorded
 * while the client was away; the torrent was moved in the client before the
 * hour; the step refuses as the page would, asks the client nothing, and the
 * library's file stays. */
static void
test_delete_download_torrent_moved_retry(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *film = NULL;
	Unwanted u;

	unwanted_torrent(fx, NULL, TRUE, &u);
	g_assert_true(axis_import_close_deleted(fx->import, u.id, "blocked", ADA, FALSE, NULL, &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	g_assert_true(axis_import_deleted_downloads(fx->import, u.id, NULL, &error));
	g_assert_no_error(error);
	assert_removal(fx, u.id, "failed", "Not deleted yet");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);

	film = move_into_library(fx, &u, TRUE);
	cleanup(fx);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	assert_removal(fx, u.id, "kept", "the client was not asked");
	assert_removal(fx, u.id, "kept", "the torrent is no longer where Axis recorded it");
	assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, u.download);
	/* And at the hour after: the same, once. */
	cleanup(fx);
	g_assert_true(g_file_test(film, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	unwanted_clear(&u);
}

/* Spec (import): The one rule for asking a client to remove with its data,
 * as the step reads it from a look: listed by its client, something at the
 * recorded place, no rule refusing, and a torrent's client saying it is
 * there.  Anything less is asked without its data. */
static void
test_delete_download_with_data_rule(Fixture *fx, gconstpointer data)
{
	AxisImportFolderLook look = AXIS_IMPORT_FOLDER_LOOK_INIT;
	gchar gone[] = "gone";
	gchar left[] = "left";
	gchar waiting[] = "waiting";

	g_assert_false(axis_import_delete_with_data(NULL));
	/* A torrent at its client, at the recorded place, which passes. */
	look.torrent = TRUE;
	look.at_client = TRUE;
	look.client_deletes = TRUE;
	look.client_agrees = TRUE;
	look.inode = 7;
	g_assert_true(axis_import_delete_with_data(&look));
	/* Each condition alone withdraws it. */
	look.at_client = FALSE;
	g_assert_false(axis_import_delete_with_data(&look));
	look.at_client = TRUE;
	look.client_deletes = FALSE;
	g_assert_false(axis_import_delete_with_data(&look));
	look.client_deletes = TRUE;
	look.client_agrees = FALSE;
	g_assert_false(axis_import_delete_with_data(&look));
	look.client_agrees = TRUE;
	look.inode = 0;
	g_assert_false(axis_import_delete_with_data(&look));
	look.inode = 7;
	look.state = gone;
	g_assert_false(axis_import_delete_with_data(&look));
	look.state = left;
	g_assert_false(axis_import_delete_with_data(&look));
	look.state = waiting;
	g_assert_false(axis_import_delete_with_data(&look));
	look.state = NULL;
	g_assert_true(axis_import_delete_with_data(&look));
	/* A Usenet job: listed, there, passing; never one its client does not
	 * list, or whose place is empty. */
	look.torrent = FALSE;
	look.client_deletes = FALSE;
	look.client_agrees = FALSE;
	g_assert_true(axis_import_delete_with_data(&look));
	look.at_client = FALSE;
	g_assert_false(axis_import_delete_with_data(&look));
	look.at_client = TRUE;
	look.state = gone;
	g_assert_false(axis_import_delete_with_data(&look));
}

/* Spec (import): One step for an import at a time.  While another step holds
 * the import's claim (the post's, when the hourly cleanup comes), the step
 * asks the client nothing and deletes nothing; once it is let go, it runs. */
static void
test_delete_download_one_step(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) got = NULL;
	g_autofree gchar *id = NULL;
	const gchar *params[1];
	AxisDbConn *other;
	Unwanted u;

	unwanted_usenet(fx, &u);
	g_assert_true(axis_import_close_deleted(fx->import, u.id, "blocked", ADA, FALSE, NULL, &error));
	g_assert_no_error(error);
	id = g_strdup_printf("%" G_GINT64_FORMAT, u.id);
	params[0] = id;
	other = axis_db_pool_acquire(fx->import->pool, &error);
	g_assert_no_error(error);
	g_assert_nonnull(other);
	g_assert_true(axis_db_conn_exec(other, "SELECT pg_try_advisory_lock(hashtext('" AXIS_IMPORT_DELETE_CLAIM "'),"
		" ($1::bigint % 2147483647)::integer)", 1, params, &got, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_db_result_get_text(got, 0, 0), ==, "t");

	/* Claimed elsewhere: passed over, by the step and by the cleanup. */
	g_assert_true(axis_import_deleted_downloads(fx->import, u.id, NULL, &error));
	g_assert_no_error(error);
	cleanup(fx);
	assert_removal(fx, u.id, "pending", NULL);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_true(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));

	/* Let go: the next step makes it, once. */
	g_assert_true(axis_db_conn_exec(other, "SELECT pg_advisory_unlock(hashtext('" AXIS_IMPORT_DELETE_CLAIM "'),"
		" ($1::bigint % 2147483647)::integer)", 1, params, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->import->pool, other);
	cleanup(fx);
	assert_removal(fx, u.id, "done", "The job was removed from its client.");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", UNWANTED_REF), ==, 1);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	/* And the claim was let go by the step itself: a second import's step is
	 * not held up by a lock left on a pooled connection. */
	cleanup(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", UNWANTED_REF), ==, 1);
	assert_qf(fx, "0", "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND classid::integer ="
		" hashtext('" AXIS_IMPORT_DELETE_CLAIM "')");
	unwanted_clear(&u);
}

/* Spec (import): Deleting a download never touches a library folder or
 * anything that is not the download's own.  Each rule on the confirmation
 * and again on the post, with nothing deleted and the client not asked. */
static void
test_delete_download_rules(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *kind = NULL;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *inode = NULL;
	Unwanted u;

	unwanted_usenet(fx, &u);
	kind = q(fx, "SELECT media_type FROM library_root WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);

	/* Inside a library folder: the root it is in holds the library. */
	exec(fx, "UPDATE library_root SET media_type = (SELECT media_type FROM library_root WHERE id = %" G_GINT64_FORMAT
		") WHERE id = %" G_GINT64_FORMAT, fx->root_id, fx->downloads_root);
	assert_delete_refused(fx, &u, fx->disk, "it is inside the library root downloads", FALSE);
	exec(fx, "UPDATE library_root SET media_type = '%s' WHERE id = %" G_GINT64_FORMAT, kind, fx->downloads_root);

	/* A library folder itself, or one that holds a library folder. */
	exec(fx, "UPDATE library_root SET rel_path = '" JOB_REL "' WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);
	assert_delete_refused(fx, &u, fx->disk, "it is the library root downloads itself", FALSE);
	exec(fx, "UPDATE library_root SET rel_path = '" JOB_REL "/Proof' WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);
	assert_delete_refused(fx, &u, fx->disk, "it holds the library root downloads", FALSE);
	exec(fx, "UPDATE library_root SET rel_path = 'downloads' WHERE id = %" G_GINT64_FORMAT, fx->downloads_root);

	/* Outside the client's own location, or that location itself. */
	axis_test_download_fake_set_category_dir(fx->fake, SAB_COMPLETE "/other");
	assert_delete_refused(fx, &u, fx->disk, "which is not inside the folder of Axis's category at the client", FALSE);
	assert_delete_refused(fx, &u, fx->disk, "The files are at " SAB_JOB " at the client.", FALSE);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_JOB);
	assert_delete_refused(fx, &u, fx->disk, "it is the folder of Axis's category at the client", FALSE);
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	assert_delete_refused(fx, &u, fx->disk, "no folder Axis may delete in", FALSE);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);

	/* A path with a ".." part is refused, never resolved. */
	exec(fx, "UPDATE download SET client_path = '" SAB_CATEGORY "/other/../" LANTERN_RELEASE "' WHERE id = %"
		G_GINT64_FORMAT, u.download);
	assert_delete_refused(fx, &u, fx->disk, "no longer maps onto a volume", FALSE);
	exec(fx, "UPDATE download SET client_path = '" SAB_JOB "' WHERE id = %" G_GINT64_FORMAT, u.download);

	/* Not on a known volume: the client's path maps nowhere now, or onto
	 * another place than the one recorded. */
	exec(fx, "UPDATE download_path_map SET client_prefix = '/elsewhere' WHERE client_id = %" G_GINT64_FORMAT,
		fx->client_id);
	assert_delete_refused(fx, &u, fx->disk, "no longer maps onto a volume", FALSE);
	exec(fx, "UPDATE download_path_map SET client_prefix = '" SAB_COMPLETE "', rel_path = 'films' WHERE client_id = %"
		G_GINT64_FORMAT, fx->client_id);
	assert_delete_refused(fx, &u, fx->disk, "now another place than the one its files were imported from", FALSE);
	exec(fx, "UPDATE download_path_map SET rel_path = 'downloads' WHERE client_id = %" G_GINT64_FORMAT, fx->client_id);

	/* Something else is there now: another folder has the download's name. */
	moved = g_strconcat(u.place, ".old", NULL);
	g_assert_cmpint(g_rename(u.place, moved), ==, 0);
	g_assert_cmpint(g_mkdir(u.place, 0755), ==, 0);
	assert_delete_refused(fx, &u, fx->disk, "not what the client reported there when the download completed", FALSE);
	g_assert_cmpint(g_rmdir(u.place), ==, 0);
	g_assert_cmpint(g_rename(moved, u.place), ==, 0);

	/* Nothing recorded of it. */
	inode = q(fx, "SELECT location_inode FROM download WHERE id = %" G_GINT64_FORMAT, u.download);
	exec(fx, "UPDATE download SET location_inode = NULL WHERE id = %" G_GINT64_FORMAT, u.download);
	assert_delete_refused(fx, &u, fx->disk, "Axis recorded nothing of what was at the place", FALSE);
	exec(fx, "UPDATE download SET location_inode = %s WHERE id = %" G_GINT64_FORMAT, inode, u.download);

	/* Another download that is still at its client is in the place, or
	 * around it; another open import is in it. */
	exec(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" volume_id, rel_path) VALUES (%" G_GINT64_FORMAT ", 'Client', 'nzo_other', 'usenet', 'file',"
		" 'Quillfeather.Junction.2014-MADEUP', 'downloading', %" G_GINT64_FORMAT ", '" JOB_REL "/Proof')",
		fx->client_id, fx->volume_id);
	assert_delete_refused(fx, &u, fx->disk, "another download Axis knows that is still at its client", FALSE);
	exec(fx, "UPDATE download SET rel_path = 'downloads/axis' WHERE client_ref = 'nzo_other'");
	assert_delete_refused(fx, &u, fx->disk, "another download Axis knows that is still at its client", FALSE);
	exec(fx, "DELETE FROM download WHERE client_ref = 'nzo_other'");

	/* The client lists a download it is recorded to have lost: never under
	 * a download its client has. */
	exec(fx, "UPDATE download SET state = 'gone' WHERE id = %" G_GINT64_FORMAT, u.download);
	{
		g_autoptr(AxisImportDeletePreview) preview = delete_look(fx, u.id);

		g_assert_false(preview->allowed);
		assert_contains(preview->refusal, "the client lists it now");
	}
	exec(fx, "UPDATE download SET state = 'completed' WHERE id = %" G_GINT64_FORMAT, u.download);

	/* Through all of it nothing went; and with every rule satisfied again,
	 * it goes. */
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));
	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);
}

/* Spec (import): A symbolic link on the way (nothing is deleted, and the
 * page says Axis never follows one), and a symbolic link inside the folder
 * (it goes with the folder, and the file it points to is untouched). */
static void
test_delete_download_symlink(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *real = NULL;
	g_autofree gchar *outside = g_build_filename(fx->disk, "outside.txt", NULL);
	g_autofree gchar *outside_dir = g_build_filename(fx->disk, "outside", "kept.txt", NULL);
	g_autofree gchar *link = NULL;
	g_autofree gchar *dir_link = NULL;
	g_autofree gchar *contents = NULL;
	GStatBuf before;
	GStatBuf now;
	Unwanted u;

	unwanted_usenet(fx, &u);
	write_text(outside, "a person's file\n");
	write_text(outside_dir, "a person's other file\n");

	/* On the way: the category's folder is a link to a folder elsewhere. */
	real = g_strconcat(u.category, "-real", NULL);
	g_assert_cmpint(g_rename(u.category, real), ==, 0);
	g_assert_cmpint(symlink("axis-real", u.category), ==, 0);
	assert_delete_refused(fx, &u, fx->disk, "symbolic link", FALSE);
	assert_delete_refused(fx, &u, fx->disk, "which Axis never follows", FALSE);
	g_assert_cmpint(g_unlink(u.category), ==, 0);
	g_assert_cmpint(g_rename(real, u.category), ==, 0);

	/* The place itself is a link. */
	real = (g_free(real), g_strconcat(u.place, "-real", NULL));
	g_assert_cmpint(g_rename(u.place, real), ==, 0);
	g_assert_cmpint(symlink(LANTERN_RELEASE "-real", u.place), ==, 0);
	assert_delete_refused(fx, &u, fx->disk, "symbolic link", FALSE);
	g_assert_cmpint(g_unlink(u.place), ==, 0);
	g_assert_cmpint(g_rename(real, u.place), ==, 0);

	/* Inside the folder: a link to a file outside it, and one to a folder. */
	link = g_build_filename(u.place, "linked.txt", NULL);
	dir_link = g_build_filename(u.place, "linked-folder", NULL);
	g_assert_cmpint(symlink(outside, link), ==, 0);
	g_assert_cmpint(symlink("../../../outside", dir_link), ==, 0);
	before = stat_of(outside);
	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	now = stat_of(outside);
	g_assert_cmpuint((guint64)now.st_ino, ==, (guint64)before.st_ino);
	g_assert_true(g_file_get_contents(outside, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, "a person's file\n");
	g_assert_true(g_file_test(outside_dir, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);
}

/* Spec (import): A file that is also in the library (a hard link): the
 * download's name is deleted, the library's file has the same inode and
 * content as before, and its link count drops by one. */
static void
test_delete_download_hardlink(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *film = NULL;
	g_autofree gchar *placed = g_build_filename(fx->films, LANTERN_FOLDER, LANTERN_PLACED, NULL);
	g_autofree gchar *folder = g_build_filename(fx->films, LANTERN_FOLDER, NULL);
	g_autofree gchar *library = NULL;
	g_autofree gchar *after = NULL;
	GStatBuf before;
	GStatBuf now;
	Unwanted u;

	unwanted_usenet(fx, &u);
	film = g_build_filename(u.place, LANTERN_RELEASE ".mkv", NULL);
	g_assert_cmpint(g_mkdir_with_parents(folder, 0755), ==, 0);
	g_assert_cmpint(link(film, placed), ==, 0);
	before = stat_of(placed);
	g_assert_cmpuint((guint)before.st_nlink, ==, 2);
	library = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "", 0);
	g_clear_pointer(&library, g_free);
	{
		g_autofree gchar *bytes = NULL;
		gsize n = 0;

		g_assert_true(g_file_get_contents(placed, &bytes, &n, NULL));
		library = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, n);
	}

	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	g_assert_false(g_file_test(film, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	now = stat_of(placed);
	g_assert_cmpuint((guint64)now.st_ino, ==, (guint64)before.st_ino);
	g_assert_cmpuint((guint)now.st_nlink, ==, 1);
	{
		g_autofree gchar *bytes = NULL;
		gsize n = 0;

		g_assert_true(g_file_get_contents(placed, &bytes, &n, NULL));
		after = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, n);
	}
	g_assert_cmpstr(after, ==, library);
	unwanted_clear(&u);
}

/* Spec (import): a single-file download (no folder of its own) deletes only
 * that file: the category's folder and everything else in it stay. */
static void
test_delete_download_single_file(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	Unwanted u;

	memset(&u, 0, sizeof(u));
	u.movie = add_film(fx, LANTERN_ID);
	u.category = g_build_filename(fx->downloads, "axis", NULL);
	u.place = g_build_filename(u.category, LANTERN_RELEASE ".mkv", NULL);
	u.beside = g_build_filename(u.category, OTHER_JOB_FILE, NULL);
	/* The film straight in the category's folder, another job's file beside
	 * it. */
	write_job(fx, LANTERN_RELEASE ".mkv");
	ensure_map(fx, SAB_COMPLETE, "downloads");
	axis_test_download_fake_keep_completed(fx->fake, TRUE);
	axis_test_download_fake_set_category_dir(fx->fake, SAB_CATEGORY);
	axis_test_download_fake_put(fx->fake, UNWANTED_REF, LANTERN_RELEASE, "axis", NULL, "completed", 1000, 1000,
		SAB_CATEGORY "/" LANTERN_RELEASE ".mkv", NULL);
	unwanted_record(fx, &u, "usenet", UNWANTED_REF, SAB_CATEGORY "/" LANTERN_RELEASE ".mkv",
		"downloads/axis/" LANTERN_RELEASE ".mkv", TRUE);

	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	g_assert_true(preview->file);
	g_assert_cmpuint(preview->n_files, ==, 1);
	g_assert_cmpuint(preview->bytes, ==, 256 * 1024);
	g_assert_cmpstr(preview->path, ==, "downloads/axis/" LANTERN_RELEASE ".mkv");

	g_assert_cmpint(delete_now(fx, u.id, FALSE), ==, AXIS_IMPORT_DELETE_DELETED);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	assert_download_files(fx, u.download, "done", "was deleted; that folder stays.");
	unwanted_clear(&u);
}

/* Spec (import): Storage is read-only; The disk is offline: the confirmation
 * says so and offers nothing, the client is not asked, and nothing is
 * deleted.  For a torrent its client would delete, too. */
static void
test_delete_download_not_writable(Fixture *fx, gconstpointer data)
{
	Unwanted u;

	unwanted_usenet(fx, &u);
	set_read_only(fx, TRUE);
	assert_delete_refused(fx, &u, fx->disk, "Read-only storage is on", TRUE);
	set_read_only(fx, FALSE);
	{
		g_autoptr(AxisImportDeletePreview) preview = NULL;
		g_autoptr(GError) error = NULL;
		guint removals = axis_test_download_fake_count(fx->fake, "remove", NULL);
		AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_DELETED;
		g_autofree gchar *digest = NULL;

		/* A page made while the disk was there, posted while it is away. */
		preview = delete_look(fx, u.id);
		g_assert_true(preview->allowed);
		digest = g_strdup(preview->digest);
		g_clear_pointer(&preview, axis_import_delete_preview_free);
		axis_test_library_unplug(fx->t, fx->disk, "disk1");
		preview = delete_look(fx, u.id);
		g_assert_false(preview->allowed);
		g_assert_true(preview->waits);
		assert_contains(preview->refusal, "is offline");
		g_assert_false(axis_import_delete_download(fx->import, u.id, digest, FALSE, ADA, &outcome, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removals);
		assert_state(fx, u.id, "blocked/wrong_name");
		axis_test_library_plug(fx->t, fx->disk, "disk1");
	}
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));
	g_assert_true(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	unwanted_clear(&u);

	/* A torrent its client still has: read-only storage refuses it too, and
	 * the client is not asked. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_torrent(fx, TOR_LABEL, TRUE, &u);
	set_read_only(fx, TRUE);
	assert_delete_refused(fx, &u, fx->disk, "Read-only storage is on", TRUE);
	set_read_only(fx, FALSE);
	g_assert_true(axis_test_download_fake_has(fx->fake, TOR_REF));
	unwanted_clear(&u);
}

/* Spec (import): The page is out of date: a post made from a page that no
 * longer matches what is so changes nothing.  And a post with no digest. */
static void
test_delete_download_stale(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *added = NULL;
	AxisImportDeleteOutcome outcome = AXIS_IMPORT_DELETE_DELETED;
	Unwanted u;

	unwanted_usenet(fx, &u);
	preview = delete_look(fx, u.id);
	g_assert_true(preview->allowed);
	/* Something was added to the folder after the page was made. */
	added = g_build_filename(u.place, "arrived-later.mkv", NULL);
	write_media(added, FILM_LINE, 4096, 9);
	g_assert_false(axis_import_delete_download(fx->import, u.id, preview->digest, FALSE, ADA, &outcome, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	g_clear_error(&error);
	g_assert_false(axis_import_delete_download(fx->import, u.id, NULL, FALSE, ADA, &outcome, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	g_clear_error(&error);
	g_assert_false(axis_import_delete_download(fx->import, u.id, "", FALSE, ADA, &outcome, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	g_clear_error(&error);
	assert_state(fx, u.id, "blocked/wrong_name");
	g_assert_true(g_file_test(added, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* The import's state changed after the page was made (it waits now). */
	g_clear_pointer(&preview, axis_import_delete_preview_free);
	preview = delete_look(fx, u.id);
	exec(fx, "UPDATE import SET state = 'waiting', reason = NULL WHERE id = %" G_GINT64_FORMAT, u.id);
	g_assert_false(axis_import_delete_download(fx->import, u.id, preview->digest, FALSE, ADA, &outcome, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED);
	g_clear_error(&error);
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));

	/* An import that is imported or dismissed already has nothing to
	 * delete; one with no download neither. */
	exec(fx, "UPDATE import SET state = 'dismissed', reason = 'dismissed', finished_at = now() WHERE id = %"
		G_GINT64_FORMAT, u.id);
	g_clear_pointer(&preview, axis_import_delete_preview_free);
	g_assert_false(axis_import_delete_preview(fx->import, u.id, &preview, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	g_clear_error(&error);
	exec(fx, "UPDATE import SET state = 'blocked', reason = 'wrong_name', finished_at = NULL, download_id = NULL"
		" WHERE id = %" G_GINT64_FORMAT, u.id);
	g_assert_false(axis_import_delete_preview(fx->import, u.id, &preview, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	assert_contains(error->message, "Leftovers page");
	g_clear_error(&error);
	g_assert_false(axis_import_delete_preview(fx->import, 999999, &preview, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND);
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));
	unwanted_clear(&u);
}

/* Spec (import): The client does not answer.  Asked while the client is
 * away, nothing is changed at all.  When it stops answering after the
 * request was recorded, the import says the removal is still to be made,
 * nothing is deleted on a guess, and the hourly cleanup makes it. */
static void
test_delete_download_client_away(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 download = 0;
	Unwanted u;

	unwanted_usenet(fx, &u);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	assert_delete_refused(fx, &u, fx->disk, "could not be asked", TRUE);

	/* The request recorded, and then the client away. */
	g_assert_true(axis_import_close_deleted(fx->import, u.id, "blocked", ADA, FALSE, &download, &error));
	g_assert_no_error(error);
	g_assert_cmpint(download, ==, u.download);
	assert_removal(fx, u.id, "pending", NULL);
	g_assert_true(axis_import_deleted_downloads(fx->import, u.id, NULL, &error));
	g_assert_no_error(error);
	assert_state(fx, u.id, "dismissed/download_deleted");
	assert_removal(fx, u.id, "failed", "Not deleted yet");
	assert_removal(fx, u.id, "failed", "Axis looks again every hour");
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));
	assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, u.download);
	/* Still away at the next hour. */
	cleanup(fx);
	assert_removal(fx, u.id, "failed", NULL);
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));

	/* It answers again: the hourly cleanup finishes what was asked. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	cleanup(fx);
	assert_removal(fx, u.id, "done", "The job was removed from its client.");
	assert_download_files(fx, u.download, "done", "was deleted");
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(u.beside, G_FILE_TEST_IS_REGULAR));
	unwanted_clear(&u);

	/* The disk read-only after the request was recorded: the client is not
	 * asked either, and the cleanup finishes it once it can be written. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_usenet(fx, &u);
	g_assert_true(axis_import_close_deleted(fx->import, u.id, "blocked", ADA, FALSE, NULL, &error));
	g_assert_no_error(error);
	set_read_only(fx, TRUE);
	cleanup(fx);
	assert_removal(fx, u.id, "failed", "Read-only storage is on");
	g_assert_true(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	g_assert_true(g_file_test(u.place, G_FILE_TEST_IS_DIR));
	set_read_only(fx, FALSE);
	cleanup(fx);
	assert_removal(fx, u.id, "done", NULL);
	g_assert_false(g_file_test(u.place, G_FILE_TEST_EXISTS));
	unwanted_clear(&u);
}

/* Spec (import): The blocklist tick: with it the release is on the title's
 * blocklist as blocked by that person; and An ordinary dismissal: its
 * download is still at its client and on the disk after the hourly cleanup
 * has run. */
static void
test_delete_download_block_and_dismiss(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	guint removals;
	Unwanted u;

	unwanted_usenet(fx, &u);
	g_assert_cmpint(delete_now(fx, u.id, TRUE), ==, AXIS_IMPORT_DELETE_DELETED);
	assert_qf(fx, "blocked/" ADA, "SELECT reason || '/' || created_by FROM movie_blocklist WHERE movie_id = %"
		G_GINT64_FORMAT " AND release_name = '" LANTERN_RELEASE "'", u.movie);
	text = q(fx, "SELECT text FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'removal'", u.movie);
	assert_contains(text, "blocklist now");
	assert_qf(fx, "deleted", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, u.download);
	unwanted_clear(&u);

	/* Dismissed, not deleted: the download stays, whatever the cleanup does. */
	exec(fx, "DELETE FROM import");
	exec(fx, "DELETE FROM download");
	exec(fx, "DELETE FROM movie");
	unwanted_usenet(fx, &u);
	before = axis_test_storage_digest(fx->disk);
	removals = axis_test_download_fake_count(fx->fake, "remove", NULL);
	g_assert_true(axis_import_act(fx->import, u.id, AXIS_IMPORT_ACT_DISMISS, -1, 0, ADA, NULL, &error));
	g_assert_no_error(error);
	assert_state(fx, u.id, "dismissed/dismissed");
	g_assert_true(axis_import_deleted_downloads(fx->import, 0, NULL, &error));
	g_assert_no_error(error);
	cleanup(fx);
	cleanup(fx);
	g_assert_true(axis_test_download_fake_has(fx->fake, UNWANTED_REF));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removals);
	assert_qf(fx, "completed/-", "SELECT state || '/' || coalesce(folder_removal, '-') FROM download WHERE id = %"
		G_GINT64_FORMAT, u.download);
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(after, ==, before);
	unwanted_clear(&u);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/import/delete-download/usenet", test_delete_download_usenet);
	ADD("/import/delete-download/torrent-at-client", test_delete_download_torrent_at_client);
	ADD("/import/delete-download/torrent-gone", test_delete_download_torrent_gone);
	ADD("/import/delete-download/torrent-in-library", test_delete_download_torrent_in_library);
	ADD("/import/delete-download/torrent-moved", test_delete_download_torrent_moved);
	ADD("/import/delete-download/torrent-moved-retry", test_delete_download_torrent_moved_retry);
	ADD("/import/delete-download/with-data-rule", test_delete_download_with_data_rule);
	ADD("/import/delete-download/one-step", test_delete_download_one_step);
	ADD("/import/delete-download/rules", test_delete_download_rules);
	ADD("/import/delete-download/symlink", test_delete_download_symlink);
	ADD("/import/delete-download/hardlink", test_delete_download_hardlink);
	ADD("/import/delete-download/single-file", test_delete_download_single_file);
	ADD("/import/delete-download/not-writable", test_delete_download_not_writable);
	ADD("/import/delete-download/stale", test_delete_download_stale);
	ADD("/import/delete-download/client-away", test_delete_download_client_away);
	ADD("/import/delete-download/block-and-dismiss", test_delete_download_block_and_dismiss);
	ADD("/import/linked-file-at-name", test_linked_file_at_name);
	ADD("/import/foreign-file-at-name", test_foreign_file_at_name);
	ADD("/import/hardlink", test_hardlink);
	ADD("/import/usenet", test_usenet);
	ADD("/import/torrent-removed", test_torrent_removed);
	ADD("/import/torrent-removed-in-library", test_torrent_removed_in_library);
	ADD("/import/torrent-seed", test_torrent_seed);
	ADD("/import/usenet-folder/removed", test_usenet_folder_removed);
	ADD("/import/usenet-folder/waits", test_usenet_folder_waits);
	ADD("/import/usenet-folder/rules", test_usenet_folder_rules);
	ADD("/import/usenet-folder/revisited", test_usenet_folder_revisited);
	ADD("/import/usenet-file/in-job-folder", test_usenet_file_in_job_folder);
	ADD("/import/usenet-file/in-subfolder", test_usenet_file_in_subfolder);
	ADD("/import/usenet-file/in-category", test_usenet_file_in_category);
	ADD("/import/usenet-file/replaced", test_usenet_file_replaced);
	ADD("/import/usenet-file/loose-replaced", test_usenet_loose_file_replaced);
	ADD("/import/usenet-file/waits", test_usenet_file_waits);
	ADD("/import/usenet-file/gone", test_usenet_file_gone);
	ADD("/import/usenet-file/job-folder-unknown", test_usenet_file_job_folder_unknown);
	ADD("/import/torrent-gone/label-folder", test_torrent_gone_label_folder);
	ADD("/import/torrent-gone/no-own-folder", test_torrent_gone_no_own_folder);
	ADD("/import/torrent-gone/one-file", test_torrent_gone_one_file);
	ADD("/import/torrent-gone/library-copy-missing", test_torrent_gone_library_copy_missing);
	ADD("/import/torrent-gone/waits-and-seed", test_torrent_gone_waits_and_seed);
	ADD("/import/given-up/folder-removed", test_given_up_job_folder_removed);
	ADD("/import/given-up/refusals", test_given_up_job_refusals);
	ADD("/import/given-up/left-alone", test_removed_downloads_left_alone);
	ADD("/import/recycle-default", test_recycle_default);
	ADD("/import/notify", test_media_servers_are_told_the_folder);
	ADD("/import/notify-upgrade", test_upgrade_tells_the_servers_modified);
	ADD("/import/faults", test_faults);
	ADD("/import/crash", test_crash);
	ADD("/import/upgrade", test_upgrade);
	ADD("/import/upgrade-no-retention", test_upgrade_no_retention);
	ADD("/import/upgrade-of-a-brought-file", test_upgrade_of_a_brought_file);
	ADD("/import/same-name", test_same_name);
	ADD("/import/upgrade-crash", test_upgrade_crash);
	ADD("/import/leftover", test_leftover);
	ADD("/import/partial", test_partial);
	ADD("/import/reasons", test_reasons);
	ADD("/import/hand-grab", test_hand_grab);
	ADD("/import/forced-movie", test_forced_movie);
	ADD("/import/assign-movie", test_assign_movie);
	ADD("/import/assign-movie-placed", test_assign_movie_placed);
	ADD("/import/kept-anyway", test_kept_anyway);
	ADD("/import/kept-invalid-profile", test_kept_invalid_profile);
	ADD("/import/stuck-hand-and-force", test_stuck_hand_and_force);
	ADD("/import/extras", test_extras);
	ADD("/import/preview", test_preview);
	ADD("/import/read-only", test_read_only);
	ADD("/import/escape", test_escape);
	ADD("/import/actions", test_actions);
	ADD("/import/follow", test_follow);
	ADD("/import/manual", test_manual);
	ADD("/import/move/usenet-job", test_move_usenet_job);
	ADD("/import/move/usenet-file-reported", test_move_usenet_file_reported);
	ADD("/import/move/torrent", test_move_torrent);
	ADD("/import/move/torrent-waits", test_move_torrent_waits);
	ADD("/import/move/torrent-seed", test_move_torrent_seed);
	ADD("/import/move/crash", test_move_crash);
	ADD("/import/upgrade-another-cut", test_upgrade_keeps_another_cuts_subtitles);
	ADD("/import/move/torrent-not-ours", test_move_torrent_not_ours);
	ADD("/import/move/dismiss-puts-back", test_dismiss_puts_moved_file_back);
	ADD("/import/move/dismissed-repaired", test_dismissed_moved_file_repaired);
	ADD("/import/move/dismissed-repair-waits", test_dismissed_repair_waits);
	ADD("/import/move/dismiss-one-connection", test_dismiss_one_connection);
	ADD("/import/move/crash-settled", test_move_crash_settled);
	ADD("/import/move/upgrade", test_move_upgrade);
	ADD("/import/move/folder-identity", test_move_folder_identity);
	ADD("/import/files-gone/waiting", test_files_gone_waiting);
	ADD("/import/files-gone/blocked", test_files_gone_blocked);
	ADD("/import/files-gone/during-and-after", test_files_gone_during_and_after);
	ADD("/import/review/stranded-outside-link", test_review_stranded_outside_link);
	ADD("/import/review/temp-beside-download", test_review_temp_beside_download);
	ADD("/import/review/folder-unreachable", test_review_folder_unreachable);
	ADD("/import/review/sweep-during-move", test_review_sweep_during_move);
	ADD("/import/review/stranded-sidecar", test_review_stranded_sidecar);
	ADD("/import/review/release-rechecked", test_review_release_rechecked);
	ADD("/import/review/release-text", test_review_release_text);
	ADD("/import/review/folder-survey", test_review_folder_survey);
	ADD("/import/review/folder-live-download", test_review_folder_live_download);
	ADD("/import/review2/release-record-removed", test_review2_release_record_removed);
	ADD("/import/review2/release-unreachable", test_review2_release_unreachable);
	ADD("/import/review2/died-before-move", test_review2_died_before_move);
	ADD("/import/review2/folder-loose-torrent", test_review2_folder_loose_torrent);
	ADD("/import/review2/folder-loose-gone", test_review2_folder_loose_gone);
	ADD("/import/review3/link-only-name", test_review3_link_only_name);
	ADD("/import/review3/folder-replaced", test_review3_folder_replaced);
	ADD("/import/review3/film-not-at-name", test_review3_film_not_at_name);
	ADD("/import/review3/release-unreachable", test_review3_release_unreachable);
	ADD("/import/review3/release-lasting", test_review3_release_lasting);
	ADD("/import/review3/released-asked-by-hash", test_review3_released_asked_by_hash);
	ADD("/import/review3/released-record-gone", test_review3_released_record_gone);
	ADD("/import/review3/folder-unreadable-waits", test_review3_folder_unreadable_waits);
	ADD("/import/review2/release-sidecars", test_review2_release_sidecars);
#undef ADD
	return g_test_run();
}
