/*
 * test-identify.c - identification on a real database and a real volume:
 * a messy films root built in a scratch directory, scanned, its probes
 * recorded, identified against the metadata test provider (a fake server on
 * loopback answering from a table of films), reviewed, and added in bulk to
 * the films module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-library-identification:
 *   4.1 each kind of evidence gives its confidence on real names; an .nfo
 *       naming another film goes to review
 *   4.2 sure by title, year and runtime; likely without a year, with the
 *       runtime off, with two candidates; nothing likely is linked
 *   4.3 two films in one folder are two units; two copies are one, the
 *       best by quality is the file for upgrades
 *   4.4 a managed film with an identified file is in library and is not
 *       searched for; one added later is in library at once, unsearched
 *   4.5 an identified file of unknown source is not upgraded by default
 *   4.6 the tree is byte- and name-identical after identification, review
 *       and bulk add
 *   4.7 the .nfo: a symbolic link is not followed, a 5 MB file is read only
 *       as far as 64 KiB, hostile names are harmless
 *   4.8 lookups stay within the provider's rate limit; a rescan with
 *       nothing changed makes no lookup, and one with a new folder looks at
 *       that folder only
 *   4.9 (half) the actions are recorded with the user, undo restores
 *       review; an import's link wins over identification's
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-identify.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "quality/axis-quality-store.h"

#define SLOW 1

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gchar                *films_dir;
	gint64                volume_id;
	gint64                root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisIndexers         *indexers;
	AxisDownloads        *downloads;
	AxisQualityStore     *quality;
	AxisMovies           *movies;
	AxisIdentify         *identify;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
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
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->films_dir = g_build_filename(fx->disk, "films", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	/* A slow provider for the pacing test: two requests a second. */
	fx->fa = (flags & SLOW) != 0 ? axis_test_meta_fixture_new("fixture", 2.0, 2) :
		axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, axis_test_identify_provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
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
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_MOVIES_MODULE_ID, axis_movies_get_module()->settings,
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IDENTIFY_MODULE_ID, axis_identify_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	/* The provider's settings table is made once per process: its default
	 * rate is the first fixture's, so the rate is set here. */
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", (flags & SLOW) != 0 ? "2" : "50", &error));
	g_assert_no_error(error);

	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_movies_set_identify(fx->movies, fx->identify);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_identify_unref(fx->identify);
	axis_movies_unref(fx->movies);
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
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->films_dir);
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

/* A complete scan of the root. */
static void
scan(Fixture *fx)
{
	g_autoptr(AxisScanResult) result = axis_test_library_scan_expect(fx->t, fx->root_id, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE);

	g_assert_nonnull(result);
}

/* A file of @size bytes (a pattern, not zeros) at films/@rel. */
static void
put(
	Fixture     *fx,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(fx->films_dir, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *content = g_malloc(MAX(size, 1));
	gsize i;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	for (i = 0; i < size; i++)
		content[i] = (gchar)('a' + (i * 7 + size) % 26);
	g_assert_true(g_file_set_contents(path, content, (gssize)size, NULL));
}

static void
put_text(
	Fixture     *fx,
	const gchar *rel,
	const gchar *text
){
	g_autofree gchar *path = g_build_filename(fx->films_dir, rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* The probe of films/@rel as ffprobe would have stored it. */
static void
probed(
	Fixture     *fx,
	const gchar *rel,
	const gchar *resolution,
	gint         minutes
){
	axis_test_probe_video(fx->t, fx->root_id, rel, "matroska", "h264", resolution, "sdr", "0.1",
		(gint64)minutes * 60000, "aac", FALSE);
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* Every name under @dir with its kind, size, mode, modification time, link
 * target and content hash: equal digests mean nothing changed on disk. */
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
		g_string_append_printf(out, "%s mode=%o size=%" G_GINT64_FORMAT " mtime=%" G_GINT64_FORMAT ".%ld ino=%"
			G_GUINT64_FORMAT, rel, (guint)st.st_mode, (gint64)st.st_size, (gint64)st.st_mtim.tv_sec,
			st.st_mtim.tv_nsec, (guint64)st.st_ino);
		if (S_ISLNK(st.st_mode)) {
			g_autofree gchar *target = g_file_read_link(path, NULL);

			g_string_append_printf(out, " -> %s\n", target);
		} else if (S_ISREG(st.st_mode)) {
			g_autofree gchar *content = NULL;
			gsize length = 0;
			g_autofree gchar *sum = NULL;

			g_assert_true(g_file_get_contents(path, &content, &length, NULL));
			sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)content, length);
			g_string_append_printf(out, " %s\n", sum);
		} else if (S_ISDIR(st.st_mode)) {
			g_string_append_c(out, '\n');
			digest_dir(path, rel, out);
		} else {
			g_string_append(out, " special\n");
		}
	}
}

static gchar *
digest(Fixture *fx)
{
	GString *out = g_string_new(NULL);

	digest_dir(fx->disk, "", out);
	return g_string_free(out, FALSE);
}

static AxisIdentifyReport
run(Fixture *fx)
{
	AxisIdentifyReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_identify_run_root(fx->identify, fx->root_id, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

/* Runs until nothing is left (or a run makes no progress). */
static void
run_all(Fixture *fx)
{
	guint i;

	for (i = 0; i < 20; i++) {
		AxisIdentifyReport report = run(fx);

		g_assert_null(report.skipped);
		g_assert_false(report.provider_down);
		if (report.pending == 0)
			return;
	}
	g_error("identification did not finish");
}

/* The unit at @rel: "state confidence title-label how". */
static gchar *
unit(
	Fixture     *fx,
	const gchar *rel
){
	return q(fx, "SELECT i.state || ' ' || coalesce(i.confidence, '-') || ' ' || coalesce(t.title || ' (' || t.year || ')',"
		" '-') || ' ' || coalesce(i.linked_how, '-') FROM library_identity i LEFT JOIN meta_title t ON t.id = i.title_id"
		" WHERE i.root_id = %" G_GINT64_FORMAT " AND i.rel_path = '%s'", fx->root_id, rel);
}

static void
assert_unit(
	Fixture     *fx,
	const gchar *rel,
	const gchar *expected
){
	g_autofree gchar *got = unit(fx, rel);

	if (g_strcmp0(got, expected) != 0) {
		g_autofree gchar *reasons = q(fx, "SELECT reasons::text || ' | ' || coalesce(read_text, '') FROM"
			" library_identity WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", fx->root_id, rel);

		g_error("%s: expected '%s', got '%s' (%s)", rel, expected, got != NULL ? got : "no unit",
			reasons != NULL ? reasons : "");
	}
}

static gint64
unit_id(
	Fixture     *fx,
	const gchar *rel
){
	g_autofree gchar *id = q(fx, "SELECT id FROM library_identity WHERE root_id = %" G_GINT64_FORMAT
		" AND rel_path = '%s'", fx->root_id, rel);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
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
	const gchar *provider_id,
	gboolean     search_now
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.search_now = search_now;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static AxisMovieState
state_of(
	Fixture         *fx,
	gint64           movie_id,
	AxisMovieFacts **facts_out
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;
	AxisMovieState state;

	g_assert_true(axis_movies_get(fx->movies, movie_id, &facts, &error));
	g_assert_no_error(error);
	axis_movies_get_rules(fx->movies, &rules);
	state = axis_movie_derive(facts, &rules, NULL);
	if (facts_out != NULL)
		*facts_out = g_steal_pointer(&facts);
	return state;
}

static void
assert_no_search(Fixture *fx)
{
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind = 'movies.search'");
}

/* The messy root: release-named and Plex-named folders, identifiers in
 * names and in .nfo files, two films in one folder, two copies of one, a
 * film without a year, samples and extras, a symbolic link, a huge .nfo, a
 * name with markup, a file at the top of the root. */
static void
build_tree(Fixture *fx)
{
	g_autofree gchar *outside = g_build_filename(fx->t->dir, "elsewhere.nfo", NULL);
	g_autofree gchar *link = g_build_filename(fx->films_dir, "Blackwick (2015)", "movie.nfo", NULL);
	g_autofree gchar *huge = NULL;
	gsize i;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.x264-GRP.mkv", 4000);
	put_text(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.x264-GRP.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
	put(fx, "Hearth (1995)/Sample/hearth-sample.mkv", 300);
	put(fx, "Hearth (1995)/Featurettes/Making Hearth.mkv", 500);
	put(fx, "Hearth (1995)/Hearth.1995-trailer.mkv", 200);
	put(fx, "Rokan (1998) {tmdb-6002}/Rokan.mkv", 3100);
	put(fx, "The.Mazework.1999.1080p.BluRay.x264-GRP/The.Mazework.1999.1080p.BluRay.x264-GRP.mkv", 3600);
	put(fx, "Rowan Marsh/Colliery.2004.1080p.BluRay.mkv", 2900);
	put(fx, "Rowan Marsh/Thistle.1981.720p.BluRay.mkv", 2800);
	put(fx, "Dunmore (2021)/Dunmore.2021.1080p.WEB-DL.DDP5.1.H.264-GRP.mkv", 3000);
	put(fx, "Dunmore (2021)/Dunmore.2021.2160p.WEB-DL.DDP5.1.HDR.H.265-GRP.mkv", 5000);
	put(fx, "Solvane/Solvane.mkv", 2500);
	put(fx, "Manhollow (1986)/Manhollow.1986.1080p.BluRay.mkv", 2700);
	put_text(fx, "Manhollow (1986)/movie.nfo", "<movie><title>Hearth</title><id>tt0600001</id></movie>");
	put(fx, "Insmere (2002)/insmere.mkv", 2600);
	put_text(fx, "Insmere (2002)/insmere.nfo", "Find it at https://www.themoviedb.org/movie/6011-insmere");
	put(fx, "Blackwick (2015)/Blackwick.2015.1080p.WEB-DL.mkv", 2600);
	g_assert_true(g_file_set_contents(outside, "<id>tt0600001</id>", -1, NULL));
	g_assert_cmpint(symlink(outside, link), ==, 0);
	put(fx, "Public Rivals (2009)/Public.Rivals.2009.1080p.BluRay.mkv", 2600);
	huge = g_malloc(5 * 1024 * 1024 + 32);
	for (i = 0; i < 5 * 1024 * 1024; i++)
		huge[i] = (gchar)('a' + i % 23);
	memcpy(huge + 5 * 1024 * 1024, " tt0600001 \n", 13);
	{
		g_autofree gchar *path = g_build_filename(fx->films_dir, "Public Rivals (2009)",
			"Public.Rivals.2009.1080p.BluRay.nfo", NULL);

		g_assert_true(g_file_set_contents(path, huge, 5 * 1024 * 1024 + 12, NULL));
	}
	put(fx, "<img src=x onerror=alert(1)> & \"q\" (2001)/x<b>.mkv", 1000);
	put(fx, "The Inlander (1999)/The.Inlander.1999.1080p.BluRay.mkv", 2600);
	put(fx, "Aro.2001.1080p.BluRay.x264-GRP.mkv", 2600);
	put(fx, "Some Home Video (2003)/clip.mkv", 900);
	/* An .nfo with nothing in the names to check it against, and a stray
	 * .nfo of another film beside a film whose names give no year. */
	put(fx, "a8f5f167f44f4964e6c998dee827110c/movie.mkv", 2100);
	put_text(fx, "a8f5f167f44f4964e6c998dee827110c/movie.nfo", "<id>tt0600001</id>");
	put(fx, "Thistle/thistle.mkv", 2200);
	put_text(fx, "Thistle/extras.nfo", "https://www.themoviedb.org/movie/6010-manhollow");

	scan(fx);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.x264-GRP.mkv", "1080p", 170);
	probed(fx, "The.Mazework.1999.1080p.BluRay.x264-GRP/The.Mazework.1999.1080p.BluRay.x264-GRP.mkv", "1080p", 136);
	probed(fx, "Rowan Marsh/Colliery.2004.1080p.BluRay.mkv", "1080p", 119);
	probed(fx, "Rowan Marsh/Thistle.1981.720p.BluRay.mkv", "720p", 122);
	probed(fx, "Dunmore (2021)/Dunmore.2021.1080p.WEB-DL.DDP5.1.H.264-GRP.mkv", "1080p", 155);
	probed(fx, "Dunmore (2021)/Dunmore.2021.2160p.WEB-DL.DDP5.1.HDR.H.265-GRP.mkv", "2160p", 155);
	probed(fx, "Solvane/Solvane.mkv", "1080p", 99);
	probed(fx, "Manhollow (1986)/Manhollow.1986.1080p.BluRay.mkv", "1080p", 120);
	probed(fx, "Insmere (2002)/insmere.mkv", "1080p", 118);
	probed(fx, "Blackwick (2015)/Blackwick.2015.1080p.WEB-DL.mkv", "1080p", 133);
	probed(fx, "Public Rivals (2009)/Public.Rivals.2009.1080p.BluRay.mkv", "1080p", 140);
	probed(fx, "The Inlander (1999)/The.Inlander.1999.1080p.BluRay.mkv", "1080p", 60);
	probed(fx, "Aro.2001.1080p.BluRay.x264-GRP.mkv", "1080p", 157);
}

/* ── The tree ───────────────────────────────────────────────────── */

static void
test_tree(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint requests;

	build_tree(fx);
	before = digest(fx);
	run_all(fx);

	/* Sure: names agreeing with a year and the runtime in tolerance. */
	assert_unit(fx, "Hearth (1995)", "linked sure Hearth (1995) auto");
	assert_unit(fx, "The.Mazework.1999.1080p.BluRay.x264-GRP", "linked sure The Mazework (1999) auto");
	assert_unit(fx, "Aro.2001.1080p.BluRay.x264-GRP.mkv", "linked sure Aro (2001) auto");
	/* Sure: an identifier in the folder's name (the file is not even probed). */
	assert_unit(fx, "Rokan (1998) {tmdb-6002}", "linked sure Rokan (1998) auto");
	/* Sure: an identifier in an .nfo the names agree with. */
	assert_unit(fx, "Insmere (2002)", "linked sure Insmere (2002) auto");
	/* Two films in one folder: a unit each. */
	assert_unit(fx, "Rowan Marsh/Colliery.2004.1080p.BluRay.mkv", "linked sure Colliery (2004) auto");
	assert_unit(fx, "Rowan Marsh/Thistle.1981.720p.BluRay.mkv", "linked sure Thistle (1981) auto");
	assert_q(fx, NULL, "SELECT id FROM library_identity WHERE rel_path = 'Rowan Marsh'");
	/* Two copies of one film: one unit, two files, the 2160p one the best. */
	assert_unit(fx, "Dunmore (2021)", "linked sure Dunmore (2021) auto");
	assert_q(fx, "2", "SELECT count(*) FROM library_identity_file lif JOIN library_identity i ON i.id = lif.identity_id"
		" WHERE i.rel_path = 'Dunmore (2021)'");
	assert_q(fx, "webdl_2160p", "SELECT lif.quality FROM library_identity_file lif JOIN library_identity i ON"
		" i.id = lif.identity_id WHERE i.rel_path = 'Dunmore (2021)' AND lif.best");
	/* The symbolic link to an .nfo naming Hearth is not followed; the names
	 * say Blackwick. */
	assert_unit(fx, "Blackwick (2015)", "linked sure Blackwick (2015) auto");
	/* The 5 MB .nfo holds Hearth's identifier past its first 64 KiB: unseen. */
	assert_unit(fx, "Public Rivals (2009)", "linked sure Public Rivals (2009) auto");

	/* Likely: no year (and two films of that title). */
	assert_unit(fx, "Solvane", "review likely - -");
	/* Likely: an .nfo naming another film than the folder, with both. */
	assert_unit(fx, "Manhollow (1986)", "review likely - -");
	assert_q(fx, "2", "SELECT jsonb_array_length(candidates) FROM library_identity WHERE rel_path = 'Manhollow (1986)'");
	/* Likely: the runtime off. */
	assert_unit(fx, "The Inlander (1999)", "review likely - -");
	/* Likely: an .nfo the names give nothing to check against (a hash for a
	 * folder, movie.mkv), its film the best candidate. */
	assert_unit(fx, "a8f5f167f44f4964e6c998dee827110c", "review likely - -");
	assert_q(fx, "1", "SELECT count(*) FROM library_identity i WHERE i.rel_path = 'a8f5f167f44f4964e6c998dee827110c'"
		" AND (i.candidates->0->>'id')::bigint = (SELECT id FROM meta_title WHERE title = 'Hearth')"
		" AND i.reasons::text LIKE '%id_uncorroborated%'");
	/* Likely: a stray .nfo of another film beside a film with no year. */
	assert_unit(fx, "Thistle", "review likely - -");
	assert_q(fx, "1", "SELECT count(*) FROM library_identity WHERE rel_path = 'Thistle' AND reasons::text LIKE"
		" '%id_uncorroborated%' AND jsonb_array_length(candidates) = 2");
	/* Unknown: nothing of that name. */
	assert_unit(fx, "Some Home Video (2003)", "review unknown - -");
	assert_unit(fx, "<img src=x onerror=alert(1)> & \"q\" (2001)", "review unknown - -");
	/* Samples and extras are no units, and no unit's files. */
	assert_q(fx, "0", "SELECT count(*) FROM library_identity_file lif JOIN library_file f ON f.id = lif.file_id"
		" WHERE f.rel_path LIKE '%Sample%' OR f.rel_path LIKE '%Featurettes%' OR f.rel_path LIKE '%trailer%'");
	/* Nothing likely or unknown is linked. */
	assert_q(fx, "0", "SELECT count(*) FROM library_identity WHERE state = 'linked' AND confidence <> 'sure'");

	/* Nothing changed on disk. */
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);

	/* A rescan with nothing changed looks nothing up. */
	scan(fx);
	requests = axis_test_plex_n_requests(fx->fa->api);
	{
		AxisIdentifyReport report = run(fx);

		g_assert_cmpuint(report.examined, ==, 0);
		g_assert_cmpuint(report.pending, ==, 0);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, requests);

	/* A new folder: only it is looked at. */
	put(fx, "The Prestwick (2006)/The.Prestwick.2006.1080p.BluRay.mkv", 2500);
	scan(fx);
	probed(fx, "The Prestwick (2006)/The.Prestwick.2006.1080p.BluRay.mkv", "1080p", 130);
	{
		AxisIdentifyReport report = run(fx);

		g_assert_cmpuint(report.examined, ==, 1);
		g_assert_cmpuint(report.units, ==, 1);
	}
	assert_unit(fx, "The Prestwick (2006)", "linked sure The Prestwick (2006) auto");

	/* A probe arriving is a change: the unit is looked at again. */
	probed(fx, "Rokan (1998) {tmdb-6002}/Rokan.mkv", "1080p", 122);
	{
		AxisIdentifyReport report = run(fx);

		g_assert_cmpuint(report.examined, ==, 1);
	}
	assert_unit(fx, "Rokan (1998) {tmdb-6002}", "linked sure Rokan (1998) auto");
	g_free(after);
	after = digest(fx);
	g_assert_cmpstr(before, !=, after);	/* the new folder */
}

/* ── Pacing ─────────────────────────────────────────────────────── */

static void
test_rate(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(GArray) times = NULL;
	guint i;
	gint64 start;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	put(fx, "Rokan (1998)/Rokan.1998.1080p.BluRay.mkv", 1000);
	put(fx, "Thistle (1981)/Thistle.1981.1080p.BluRay.mkv", 1000);
	put(fx, "Colliery (2004)/Colliery.2004.1080p.BluRay.mkv", 1000);
	scan(fx);
	axis_test_plex_reset_requests(fx->fa->api);
	start = g_get_monotonic_time();
	run_all(fx);
	times = axis_test_plex_request_times(fx->fa->api);
	/* Two lookups each (a search and a full record): eight, at two a second
	 * after a burst of two.  The k-th request cannot have reached the server
	 * before (k - 2) / 2 seconds after the run began (a slow server only
	 * records it later). */
	g_assert_cmpuint(times->len, >=, 8);
	for (i = 0; i < times->len; i++) {
		gint64 elapsed = g_array_index(times, gint64, i) - start;
		gint64 earliest = ((gint64)i + 1 - 2) * 500000 - 50000;

		if (elapsed < earliest)
			g_error("request %u came %" G_GINT64_FORMAT " ms into the run, before %" G_GINT64_FORMAT " ms: over the "
				"rate", i + 1, elapsed / 1000, earliest / 1000);
	}
	/* And the run took the time that implies. */
	g_assert_cmpint(g_get_monotonic_time() - start, >=, ((gint64)times->len - 2) * 500000 - 50000);
}

/* The provider away: try again later, never "unknown". */
static void
test_provider_down(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	scan(fx);
	g_assert_true(axis_settings_clear(fx->settings, "metadata.fixture_key", &error));
	g_assert_no_error(error);
	report = run(fx);
	g_assert_true(report.provider_down);
	g_assert_cmpuint(report.examined, ==, 0);
	g_assert_cmpuint(report.pending, ==, 1);
	assert_q(fx, "0", "SELECT count(*) FROM library_identity");
	assert_q(fx, "t", "SELECT retry_after > now() FROM library_identity_root");
	/* The wait holds until it is over or someone asks. */
	report = run(fx);
	g_assert_cmpstr(report.skipped, ==, "waiting for the details provider");
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_identify_request(fx->identify, fx->root_id, NULL, &error));
	g_assert_no_error(error);
	run_all(fx);
	assert_unit(fx, "Hearth (1995)", "review likely - -");	/* not probed */
}

/* ── Films ──────────────────────────────────────────────────────── */

static void
test_films(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(AxisMovieFacts) hearth_facts = NULL;
	g_autoptr(AxisMovieFacts) dunmore_facts = NULL;
	gint64 hearth;
	gint64 rokan;
	gint64 dunmore;

	build_tree(fx);
	/* Managed before its file was identified. */
	hearth = add_film(fx, "6001", FALSE);
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_WANTED);
	run_all(fx);
	g_assert_cmpint(state_of(fx, hearth, &hearth_facts), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_cmpuint(hearth_facts->n_files, ==, 1);
	g_assert_cmpint(hearth_facts->file_quality, ==, AXIS_QUALITY_BLURAY_1080P);
	assert_q(fx, "1", "SELECT count(*) FROM movie_event WHERE kind = 'identified'");
	/* Added after: in the library at once, and not searched, though asked. */
	rokan = add_film(fx, "6002", TRUE);
	g_assert_cmpint(state_of(fx, rokan, NULL), ==, AXIS_MOVIE_IN_LIBRARY);
	assert_no_search(fx);
	/* Two copies: both the film's, the best judged by. */
	dunmore = add_film(fx, "6006", TRUE);
	g_assert_cmpint(state_of(fx, dunmore, &dunmore_facts), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_cmpuint(dunmore_facts->n_files, ==, 2);
	g_assert_cmpint(dunmore_facts->file_quality, ==, AXIS_QUALITY_WEBDL_2160P);
	assert_no_search(fx);
	/* A likely one is not linked: its film stays wanted. */
	{
		gint64 insider = add_film(fx, "6015", FALSE);

		g_assert_cmpint(state_of(fx, insider, NULL), ==, AXIS_MOVIE_WANTED);
	}
	/* The scheduler searches none of the films in the library. */
	{
		AxisMovieScheduleReport report;
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_movies_run_schedule(fx->movies, &report, &error));
		g_assert_no_error(error);
		assert_q(fx, NULL, "SELECT payload->>'movie' FROM job WHERE kind = 'movies.search' AND"
			" (payload->>'movie')::bigint IN (SELECT id FROM movie WHERE title_id IN (SELECT title_id FROM"
			" library_identity WHERE state = 'linked'))");
	}
}

/* A file whose name states no source: an unknown source at its probed
 * resolution, not upgraded by default. */
static void
test_unknown_source(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autofree gchar *reason = NULL;
	AxisMovieRules rules;
	gint64 hearth;

	put(fx, "Hearth (1995)/Hearth (1995).mkv", 3000);
	scan(fx);
	probed(fx, "Hearth (1995)/Hearth (1995).mkv", "1080p", 170);
	run_all(fx);
	assert_unit(fx, "Hearth (1995)", "linked sure Hearth (1995) auto");
	hearth = add_film(fx, "6001", FALSE);
	assert_q(fx, "|1080p", "SELECT coalesce(quality, '') || '|' || coalesce(resolution, '') FROM movie_file");
	g_assert_cmpint(state_of(fx, hearth, &facts), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_cmpint(facts->file_quality, ==, AXIS_QUALITY_UNKNOWN);
	g_assert_cmpint(facts->file_resolution, ==, AXIS_RELEASE_RESOLUTION_1080P);
	axis_movies_get_rules(fx->movies, &rules);
	axis_movie_derive(facts, &rules, &reason);
	g_assert_nonnull(strstr(reason, "does not upgrade a file of unknown source"));
	{
		AxisMovieScheduleReport report;
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_movies_run_schedule(fx->movies, &report, &error));
		g_assert_no_error(error);
		assert_no_search(fx);
	}
	/* A profile that upgrades an unknown source wants it upgraded. */
	facts->profile_upgrade_unknown = TRUE;
	g_assert_cmpint(axis_movie_derive(facts, &rules, NULL), ==, AXIS_MOVIE_UPGRADE_WANTED);
}

/* ── Review ─────────────────────────────────────────────────────── */

static void
act(
	Fixture            *fx,
	const gchar        *rel,
	AxisIdentifyAction  action,
	gint64              title_id
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_identify_act(fx->identify, unit_id(fx, rel), action, title_id, "alice", &error));
	g_assert_no_error(error);
}

static void
test_review(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	gint64 hearth_film;
	gint64 manhollow;

	build_tree(fx);
	before = digest(fx);
	run_all(fx);
	hearth_film = add_film(fx, "6001", FALSE);
	g_assert_cmpint(state_of(fx, hearth_film, NULL), ==, AXIS_MOVIE_IN_LIBRARY);

	/* Confirm: the best candidate of a likely one. */
	act(fx, "Solvane", AXIS_IDENTIFY_CONFIRM, 0);
	assert_unit(fx, "Solvane", "linked likely Solvane (1972) confirmed");
	/* Choose: another film than the best candidate. */
	manhollow = title_of(fx, "6010");
	act(fx, "Manhollow (1986)", AXIS_IDENTIFY_CHOOSE, manhollow);
	assert_unit(fx, "Manhollow (1986)", "linked likely Manhollow (1986) chosen");
	/* Not a film. */
	act(fx, "Some Home Video (2003)", AXIS_IDENTIFY_NOT_FILM, 0);
	assert_unit(fx, "Some Home Video (2003)", "not_film unknown - -");
	/* Choosing a title that is no film, or confirming twice, is refused. */
	g_assert_false(axis_identify_act(fx->identify, unit_id(fx, "Solvane"), AXIS_IDENTIFY_CHOOSE, 999999, "alice",
		&error));
	g_assert_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_identify_act(fx->identify, unit_id(fx, "Some Home Video (2003)"), AXIS_IDENTIFY_CONFIRM, 0,
		"alice", &error));
	g_assert_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID);
	g_clear_error(&error);

	/* Undo a sure link: back to review; the film no longer counts its file. */
	act(fx, "Hearth (1995)", AXIS_IDENTIFY_UNDO, 0);
	assert_unit(fx, "Hearth (1995)", "review sure - -");
	g_assert_cmpint(state_of(fx, hearth_film, NULL), ==, AXIS_MOVIE_WANTED);
	{
		g_autofree gchar *count = q(fx, "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT,
			hearth_film);

		g_assert_cmpstr(count, ==, "0");
	}
	assert_q(fx, "1", "SELECT count(*) FROM movie_event WHERE kind = 'unidentified'");
	/* ... and a person's decision stands: a new look does not link it again. */
	g_assert_true(axis_identify_request(fx->identify, fx->root_id, NULL, &error));
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = '00000000000000000000000000000000'");
	run_all(fx);
	assert_unit(fx, "Hearth (1995)", "review sure - -");
	assert_unit(fx, "Solvane", "linked likely Solvane (1972) confirmed");
	assert_unit(fx, "Some Home Video (2003)", "not_film unknown - -");
	/* Undo "not a film". */
	act(fx, "Some Home Video (2003)", AXIS_IDENTIFY_UNDO, 0);
	assert_unit(fx, "Some Home Video (2003)", "review unknown - -");
	/* Link it again by hand. */
	act(fx, "Hearth (1995)", AXIS_IDENTIFY_CONFIRM, 0);
	assert_unit(fx, "Hearth (1995)", "linked sure Hearth (1995) confirmed");
	g_assert_cmpint(state_of(fx, hearth_film, NULL), ==, AXIS_MOVIE_IN_LIBRARY);

	/* Every action is recorded with the user. */
	assert_q(fx, "confirm,choose,not_film,undo,undo,confirm", "SELECT string_agg(action, ',' ORDER BY id) FROM"
		" library_identity_action");
	assert_q(fx, "alice", "SELECT string_agg(DISTINCT by_user, ',') FROM library_identity_action");
	assert_q(fx, "alice", "SELECT decided_by FROM library_identity WHERE rel_path = 'Hearth (1995)'");

	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
}

/* ── Adding every identified film ───────────────────────────────── */

static void
test_add_all(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *linked = NULL;
	g_autoptr(GError) error = NULL;
	guint added = 0;
	guint skipped = 0;

	build_tree(fx);
	before = digest(fx);
	run_all(fx);
	add_film(fx, "6001", FALSE);
	linked = q(fx, "SELECT count(DISTINCT title_id) FROM library_identity WHERE state = 'linked'");
	g_assert_true(axis_identify_add_all(fx->identify, fx->root_id, 0, FALSE, "alice", &added, &skipped, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(added + skipped, ==, (guint)g_ascii_strtoull(linked, NULL, 10));
	/* Ten films linked as sure, Hearth among the films already. */
	g_assert_cmpuint(skipped, ==, 1);
	g_assert_cmpuint(added, ==, 9);
	/* Each is managed, unmonitored, in the library; nothing is searched. */
	assert_q(fx, "0", "SELECT count(*) FROM library_identity i WHERE i.state = 'linked' AND NOT EXISTS"
		" (SELECT 1 FROM movie m WHERE m.title_id = i.title_id)");
	assert_q(fx, "0", "SELECT count(*) FROM movie m WHERE m.added_by = 'alice' AND m.monitored");
	assert_q(fx, "0", "SELECT count(*) FROM movie m WHERE NOT EXISTS (SELECT 1 FROM movie_file f WHERE"
		" f.movie_id = m.id)");
	assert_no_search(fx);
	{
		g_autoptr(GPtrArray) all = NULL;
		AxisMovieRules rules;
		guint i;

		g_assert_true(axis_movies_list(fx->movies, &all, &error));
		axis_movies_get_rules(fx->movies, &rules);
		for (i = 0; i < all->len; i++)
			g_assert_cmpint(axis_movie_derive(g_ptr_array_index(all, i), &rules, NULL), ==, AXIS_MOVIE_IN_LIBRARY);
	}
	assert_q(fx, "add_all", "SELECT action FROM library_identity_action WHERE by_user = 'alice'");
	/* A second time adds nothing. */
	g_assert_true(axis_identify_add_all(fx->identify, fx->root_id, 0, TRUE, "alice", &added, &skipped, &error));
	g_assert_cmpuint(added, ==, 0);
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
}

/* ── An import's link wins ──────────────────────────────────────── */

static void
test_import_wins(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *file = NULL;
	gint64 thistle;

	put(fx, "Colliery (2004)/Colliery.2004.1080p.BluRay.mkv", 2900);
	scan(fx);
	probed(fx, "Colliery (2004)/Colliery.2004.1080p.BluRay.mkv", "1080p", 120);
	/* An import placed the file for Thistle (whatever its name says). */
	thistle = add_film(fx, "6005", FALSE);
	file = q(fx, "SELECT id FROM library_file WHERE rel_path = 'Colliery (2004)/Colliery.2004.1080p.BluRay.mkv'");
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, owner_id, title_label, source_path, state, finished_at)"
		" VALUES ('movies', 1, 'Thistle (1981)', 'downloads/x', 'imported', now())");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, import_id)"
			" VALUES (%s, %" G_GINT64_FORMAT ", 'bluray_1080p', (SELECT max(id) FROM import))", file, thistle);

		axis_test_db_exec(fx->t->db, sql);
	}
	run_all(fx);
	assert_unit(fx, "Colliery (2004)", "linked sure Thistle (1981) imported");
	/* The import's row stands as it was: no identity of its own. */
	assert_q(fx, "1", "SELECT count(*) FROM movie_file WHERE import_id IS NOT NULL AND identity_id IS NULL");
	assert_q(fx, "1", "SELECT count(*) FROM movie_file");
	/* Its link is not a person's to undo here. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_identify_act(fx->identify, unit_id(fx, "Colliery (2004)"), AXIS_IDENTIFY_UNDO, 0, "alice",
			&error));
		g_assert_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID);
	}
}

/* Units of a folder that went are forgotten, with the films' links. */
static void
test_gone(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *path = NULL;
	gint64 hearth;
	guint i;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	scan(fx);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", "1080p", 170);
	run_all(fx);
	hearth = add_film(fx, "6001", FALSE);
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_IN_LIBRARY);
	path = g_build_filename(fx->films_dir, "Hearth (1995)", "Hearth.1995.1080p.BluRay.mkv", NULL);
	g_assert_cmpint(g_unlink(path), ==, 0);
	/* Missing only after two complete scans an hour apart. */
	scan(fx);
	run_all(fx);
	assert_unit(fx, "Hearth (1995)", "linked sure Hearth (1995) auto");
	/* As if two hours had passed, by both clocks. */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET suspected_at = suspected_at - interval '2 hours',"
		" suspected_boottime_us = suspected_boottime_us - 7200000000 WHERE state = 'suspected'");
	for (i = 0; i < 2; i++)
		scan(fx);
	assert_q(fx, "missing", "SELECT state FROM library_file WHERE rel_path = 'Hearth (1995)/Hearth.1995.1080p.BluRay.mkv'");
	run(fx);
	assert_q(fx, "0", "SELECT count(*) FROM library_identity");
	assert_q(fx, "0", "SELECT count(*) FROM movie_file");
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_WANTED);
}

/* ── The scheduler ──────────────────────────────────────────────── */

/* Runs the scheduler; how many runs of the root it queued.  The queued runs
 * are then forgotten, as if they had been taken. */
static guint
schedule(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	guint queued = 0;

	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	return queued;
}

static void
test_schedule(
	Fixture       *fx,
	gconstpointer  data
){
	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	put(fx, "Rokan (1998)/Rokan.1998.1080p.BluRay.mkv", 1000);
	/* No complete scan yet: nothing to look at. */
	g_assert_cmpuint(schedule(fx), ==, 0);
	scan(fx);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", "1080p", 170);
	/* Never run: queued. */
	g_assert_cmpuint(schedule(fx), ==, 1);
	run_all(fx);
	/* Quiet: not queued, however often the scheduler looks. */
	g_assert_cmpuint(schedule(fx), ==, 0);
	g_assert_cmpuint(schedule(fx), ==, 0);

	/* A newer complete scan. */
	scan(fx);
	g_assert_cmpuint(schedule(fx), ==, 1);
	run_all(fx);
	g_assert_cmpuint(schedule(fx), ==, 0);

	/* A probe of one of its videos. */
	probed(fx, "Rokan (1998)/Rokan.1998.1080p.BluRay.mkv", "1080p", 122);
	g_assert_cmpuint(schedule(fx), ==, 1);
	run_all(fx);
	assert_unit(fx, "Rokan (1998)", "linked sure Rokan (1998) auto");
	g_assert_cmpuint(schedule(fx), ==, 0);

	/* Units left over by the last run. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET pending = 3");
	g_assert_cmpuint(schedule(fx), ==, 1);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET pending = 0");
	g_assert_cmpuint(schedule(fx), ==, 0);

	/* A provider wait: not while it lasts, at once when it is over. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET retry_after = now() + interval '1 hour', pending = 2");
	g_assert_cmpuint(schedule(fx), ==, 0);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET retry_after = now() - interval '1 second',"
		" pending = 0");
	g_assert_cmpuint(schedule(fx), ==, 1);
	run_all(fx);
	g_assert_cmpuint(schedule(fx), ==, 0);
}

/* ── Links by hand (add-force-grab-and-assign, slice C2) ─────────── */

/* One file's choice on a movie's links page, reviewed and applied as @user. */
static void
hand_change(
	Fixture     *fx,
	gint64       movie_id,
	gint64       file_id,
	gboolean     unlink
){
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(AxisMovieLinks) review = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieLinkAsk *ask = g_new0(AxisMovieLinkAsk, 1);
	gboolean changed = TRUE;

	ask->file_id = file_id;
	ask->unlink = unlink;
	g_ptr_array_add(asks, ask);
	g_assert_true(axis_movies_links_review(fx->movies, movie_id, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_null(review->refused);
	g_assert_true(axis_movies_links_apply(fx->movies, movie_id, asks, NULL, review->digest, "alice", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_false(changed);
}

/* Every folder of the root is looked at again by the next run. */
static void
look_again(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_identify_request(fx->identify, fx->root_id, NULL, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = '00000000000000000000000000000000'");
	run_all(fx);
}

/* Where @file is linked: "<movie>:<hand_how>:<kept>"; "" for no link. */
static gchar *
hand_link(
	Fixture *fx,
	gint64   file
){
	g_autofree gchar *found = q(fx, "SELECT movie_id || ':' || coalesce(hand_how, '-') || ':' || kept::text"
		" FROM movie_file WHERE file_id = %" G_GINT64_FORMAT, file);

	return g_strdup(found != NULL ? found : "");
}

static void
assert_hand_link(
	Fixture     *fx,
	gint64       file,
	const gchar *expected
){
	g_autofree gchar *link = hand_link(fx, file);

	g_assert_cmpstr(link, ==, expected);
}

/*
 * Slice C2.  Spec (library-identification): A folder linked to another title
 * afterwards; Unlinked by hand (a movie).  And: a file unlinked by hand is
 * taken up again once Axis manages it; removing the movie clears its marks.
 *
 * Hearth's file is linked by hand to Rokan; Rokan's own file is unlinked by
 * hand.  The folders are looked at again, and a person then chooses another
 * title for Hearth's folder: the first file is still Rokan's, by hand; the
 * second has no link.
 */
static void
test_hand_link_kept(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(AxisMovieLinkManage) handed = NULL;
	g_autoptr(AxisMovieLinkManage) nothing = NULL;
	g_autoptr(AxisMovieLinks) listed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *on_rokan = NULL;
	g_autofree gchar *by_run = NULL;
	gboolean changed = TRUE;
	gint64 hearth;
	gint64 rokan;
	gint64 manhollow;
	gint64 hearth_file;
	gint64 rokan_file;
	guint i;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	put(fx, "Rokan (1998)/Rokan.1998.1080p.BluRay.mkv", 1100);
	scan(fx);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", "1080p", 170);
	probed(fx, "Rokan (1998)/Rokan.1998.1080p.BluRay.mkv", "1080p", 122);
	before = digest(fx);
	run_all(fx);
	assert_unit(fx, "Hearth (1995)", "linked sure Hearth (1995) auto");
	assert_unit(fx, "Rokan (1998)", "linked sure Rokan (1998) auto");
	hearth = add_film(fx, "6001", FALSE);
	rokan = add_film(fx, "6002", FALSE);
	manhollow = add_film(fx, "6010", FALSE);
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_IN_LIBRARY);
	g_assert_cmpint(state_of(fx, rokan, NULL), ==, AXIS_MOVIE_IN_LIBRARY);
	{
		g_autofree gchar *one = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE 'Hearth (1995)/%%'");
		g_autofree gchar *two = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE 'Rokan (1998)/%%'");

		hearth_file = g_ascii_strtoll(one, NULL, 10);
		rokan_file = g_ascii_strtoll(two, NULL, 10);
	}

	/* By hand: Hearth's file is Rokan's, and Rokan's own file is no one's. */
	hand_change(fx, rokan, hearth_file, FALSE);
	hand_change(fx, rokan, rokan_file, TRUE);
	on_rokan = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", rokan);
	assert_hand_link(fx, hearth_file, on_rokan);
	assert_hand_link(fx, rokan_file, "");
	assert_q(fx, "alice", "SELECT by_user FROM library_hand_unlink WHERE module = 'movies'");
	assert_q(fx, "alice", "SELECT hand_by FROM movie_file WHERE hand_at IS NOT NULL AND identity_id IS NULL");
	/* Hearth has no file now: it is wanted; Rokan has one, kept by hand. */
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_WANTED);
	g_assert_cmpint(state_of(fx, rokan, NULL), ==, AXIS_MOVIE_IN_LIBRARY);

	/* Scenario: Unlinked by hand.  A run links both folders as sure again:
	 * neither file's link changes. */
	look_again(fx);
	assert_unit(fx, "Rokan (1998)", "linked sure Rokan (1998) auto");
	assert_hand_link(fx, hearth_file, on_rokan);
	assert_hand_link(fx, rokan_file, "");
	g_assert_cmpint(state_of(fx, hearth, NULL), ==, AXIS_MOVIE_WANTED);

	/* Scenario: A folder linked to another title afterwards.  A person
	 * chooses Manhollow for Hearth's folder: the hand link stands. */
	act(fx, "Hearth (1995)", AXIS_IDENTIFY_CHOOSE, title_of(fx, "6010"));
	assert_hand_link(fx, hearth_file, on_rokan);
	g_assert_cmpint(state_of(fx, manhollow, NULL), ==, AXIS_MOVIE_WANTED);

	/* The links page lists the one as linked by hand, the other as unlinked
	 * by hand. */
	g_assert_true(axis_movies_links_read(fx->movies, rokan, NULL, 0, &listed, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(listed->files->len, ==, 2);
	for (i = 0; i < listed->files->len; i++) {
		const AxisMovieLinkFile *file = g_ptr_array_index(listed->files, i);

		g_assert_cmpint(file->linked, ==, file->file_id == hearth_file);
		g_assert_cmpint(file->unlinked_by_hand, ==, file->file_id == rokan_file);
	}

	/* A hand link identification reads otherwise cannot be handed back. */
	g_assert_true(axis_movies_links_manage_review(fx->movies, rokan, hearth_file, &nothing, &error));
	g_assert_no_error(error);
	g_assert_cmpint(nothing->kind, ==, AXIS_MOVIES_MANAGE_NOTHING);
	g_assert_false(axis_movies_links_manage(fx->movies, rokan, hearth_file, nothing->digest, "alice", NULL, &changed,
		&error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT);
	g_clear_error(&error);
	assert_hand_link(fx, hearth_file, on_rokan);

	/* Let Axis manage the unlinked file again: the next run takes it up. */
	g_assert_true(axis_movies_links_manage_review(fx->movies, rokan, rokan_file, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, ==, AXIS_MOVIES_MANAGE_UNLINKED);
	g_assert_nonnull(strstr(handed->outcome, "links it to that movie at its next run"));
	assert_q(fx, "1", "SELECT count(*) FROM library_hand_unlink");
	g_assert_true(axis_movies_links_manage(fx->movies, rokan, rokan_file, handed->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	assert_q(fx, "0", "SELECT count(*) FROM library_hand_unlink");
	run_all(fx);
	by_run = g_strdup_printf("%" G_GINT64_FORMAT ":-:false", rokan);
	assert_hand_link(fx, rokan_file, by_run);
	assert_hand_link(fx, hearth_file, on_rokan);

	/* Removing a movie clears the marks made for it. */
	hand_change(fx, rokan, rokan_file, TRUE);
	assert_q(fx, "1", "SELECT count(*) FROM library_hand_unlink");
	g_assert_true(axis_movies_remove(fx->movies, rokan, "alice", &error));
	g_assert_no_error(error);
	assert_q(fx, "0", "SELECT count(*) FROM library_hand_unlink");

	/* No file was touched by any of it. */
	after = digest(fx);
	g_assert_cmpstr(before, ==, after);
}

/*
 * A file taken by hand from the movie an import linked it to no longer has
 * that import: the folder is not read as the new movie's imported title, and
 * the folder's other file stays with the movie identification linked it to.
 */
static void
test_hand_link_leaves_import(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(AxisMovieLinkManage) handed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *on_thistle = NULL;
	g_autofree gchar *on_rokan = NULL;
	gint64 thistle;
	gint64 rokan;
	gint64 file_x;
	gint64 file_y;

	put(fx, "Thistle (1981)/Thistle.1981.1080p.BluRay.mkv", 1000);
	put(fx, "Thistle (1981)/Thistle.1981.Extended.mkv", 1100);
	scan(fx);
	probed(fx, "Thistle (1981)/Thistle.1981.1080p.BluRay.mkv", "1080p", 110);
	probed(fx, "Thistle (1981)/Thistle.1981.Extended.mkv", "1080p", 120);
	thistle = add_film(fx, "6005", FALSE);
	rokan = add_film(fx, "6002", FALSE);
	{
		g_autofree gchar *one = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%Extended.mkv'");
		g_autofree gchar *two = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%BluRay.mkv'");

		file_x = g_ascii_strtoll(one, NULL, 10);
		file_y = g_ascii_strtoll(two, NULL, 10);
	}
	/* X was placed by an import for Thistle; Y is Thistle's by identification. */
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, owner_id, title_label, source_path, state, finished_at)"
		" VALUES ('movies', 1, 'Thistle (1981)', 'downloads/x', 'imported', now())");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, import_id)"
			" VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'bluray_1080p', (SELECT max(id) FROM import))", file_x,
			thistle);

		axis_test_db_exec(fx->t->db, sql);
	}
	run_all(fx);
	on_thistle = g_strdup_printf("%" G_GINT64_FORMAT ":-:false", thistle);
	assert_hand_link(fx, file_x, on_thistle);
	assert_hand_link(fx, file_y, on_thistle);

	/* By hand, X is Rokan's.  Its import is Thistle's, not Rokan's. */
	hand_change(fx, rokan, file_x, FALSE);
	on_rokan = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", rokan);
	assert_hand_link(fx, file_x, on_rokan);
	assert_q(fx, "0", "SELECT count(*) FROM movie_file WHERE import_id IS NOT NULL");
	look_again(fx);
	assert_hand_link(fx, file_x, on_rokan);
	assert_hand_link(fx, file_y, on_thistle);
	/* Handing X back is judged by what identification reads, not by an import. */
	g_assert_true(axis_movies_links_manage_review(fx->movies, rokan, file_x, &handed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(handed->kind, !=, AXIS_MOVIES_MANAGE_RELEASE);
	g_assert_cmpint(handed->kind, ==, AXIS_MOVIES_MANAGE_NOTHING);
}

/*
 * A file only kept (linked by identification, no hand mark) follows its
 * folder to another title that is a managed movie, with its mark, and is
 * unlinked when the title is not a managed movie.
 */
static void
test_kept_follows_choice(
	Fixture       *fx,
	gconstpointer  data
){
	g_autofree gchar *on_manhollow = NULL;
	gint64 hearth;
	gint64 manhollow;
	gint64 file;

	put(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", 1000);
	scan(fx);
	probed(fx, "Hearth (1995)/Hearth.1995.1080p.BluRay.mkv", "1080p", 170);
	run_all(fx);
	hearth = add_film(fx, "6001", FALSE);
	manhollow = add_film(fx, "6010", FALSE);
	{
		g_autofree gchar *one = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE 'Hearth (1995)/%%'");

		file = g_ascii_strtoll(one, NULL, 10);
	}
	assert_q(fx, "1", "SELECT count(*) FROM movie_file WHERE identity_id IS NOT NULL");
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET kept = true");
	{
		g_autofree gchar *on_hearth = g_strdup_printf("%" G_GINT64_FORMAT ":-:true", hearth);

		assert_hand_link(fx, file, on_hearth);
	}
	/* Chosen as a managed movie: it follows, still kept. */
	act(fx, "Hearth (1995)", AXIS_IDENTIFY_CHOOSE, title_of(fx, "6010"));
	on_manhollow = g_strdup_printf("%" G_GINT64_FORMAT ":-:true", manhollow);
	assert_hand_link(fx, file, on_manhollow);
	/* Chosen as a title no movie is managed for: no link stays on the old movie. */
	act(fx, "Hearth (1995)", AXIS_IDENTIFY_CHOOSE, title_of(fx, "6015"));
	assert_hand_link(fx, file, "");
}

/*
 * A file taken from a movie another change holds is refused with a reason
 * (nothing waits, nothing is changed), and goes through once the other change
 * is over.
 */
static void
test_links_other_locked(
	Fixture       *fx,
	gconstpointer  data
){
	g_autoptr(AxisMovieLinks) review = NULL;
	g_autoptr(AxisMovieLinks) refused = NULL;
	g_autoptr(GPtrArray) asks = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *lock_sql = NULL;
	g_autofree gchar *on_thistle = NULL;
	g_autofree gchar *on_rokan = NULL;
	AxisMovieLinkAsk *ask = g_new0(AxisMovieLinkAsk, 1);
	AxisDbConn *conn;
	gboolean changed = TRUE;
	gint64 thistle;
	gint64 rokan;
	gint64 file;

	put(fx, "Thistle (1981)/Thistle.1981.1080p.BluRay.mkv", 1000);
	put(fx, "Thistle (1981)/Thistle.1981.Extended.mkv", 1100);
	scan(fx);
	thistle = add_film(fx, "6005", FALSE);
	rokan = add_film(fx, "6002", FALSE);
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality) SELECT id, %"
			G_GINT64_FORMAT ", 'webdl_720p' FROM library_file", thistle);
		g_autofree gchar *one = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%Extended.mkv'");

		axis_test_db_exec(fx->t->db, sql);
		file = g_ascii_strtoll(one, NULL, 10);
	}
	ask->file_id = file;
	g_ptr_array_add(asks, ask);
	g_assert_true(axis_movies_links_review(fx->movies, rokan, asks, NULL, &review, &error));
	g_assert_no_error(error);
	g_assert_null(review->refused);

	/* Another change holds Thistle. */
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_begin(conn, &error));
	lock_sql = g_strdup_printf("SELECT id FROM movie WHERE id = %" G_GINT64_FORMAT " FOR UPDATE", thistle);
	g_assert_true(axis_db_conn_exec(conn, lock_sql, 0, NULL, NULL, &error));
	g_assert_false(axis_movies_links_apply(fx->movies, rokan, asks, NULL, review->digest, "alice", &refused, &changed,
		&error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT);
	g_assert_nonnull(refused);
	g_assert_nonnull(refused->refused);
	g_assert_nonnull(strstr(refused->refused, "Axis or another person is changing"));
	g_clear_error(&error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(fx->t->pool, conn);
	on_thistle = g_strdup_printf("%" G_GINT64_FORMAT ":-:false", thistle);
	assert_hand_link(fx, file, on_thistle);

	/* Over: the same review applies. */
	g_assert_true(axis_movies_links_apply(fx->movies, rokan, asks, NULL, review->digest, "alice", NULL, &changed, &error));
	g_assert_no_error(error);
	g_assert_false(changed);
	on_rokan = g_strdup_printf("%" G_GINT64_FORMAT ":linked:true", rokan);
	assert_hand_link(fx, file, on_rokan);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/identify/links-other-locked", Fixture, NULL, test_links_other_locked);

	AXIS_TEST_ADD_DB_CASE("/identify/hand-link-leaves-import", Fixture, NULL, test_hand_link_leaves_import);
	AXIS_TEST_ADD_DB_CASE("/identify/kept-follows-choice", Fixture, NULL, test_kept_follows_choice);

	AXIS_TEST_ADD_DB_CASE("/identify/hand-link-kept", Fixture, NULL, test_hand_link_kept);

	AXIS_TEST_ADD_DB_CASE("/identify/tree", Fixture, NULL, test_tree);
	AXIS_TEST_ADD_DB_CASE("/identify/rate", Fixture, GUINT_TO_POINTER(SLOW), test_rate);
	AXIS_TEST_ADD_DB_CASE("/identify/provider-down", Fixture, NULL, test_provider_down);
	AXIS_TEST_ADD_DB_CASE("/identify/films", Fixture, NULL, test_films);
	AXIS_TEST_ADD_DB_CASE("/identify/unknown-source", Fixture, NULL, test_unknown_source);
	AXIS_TEST_ADD_DB_CASE("/identify/review", Fixture, NULL, test_review);
	AXIS_TEST_ADD_DB_CASE("/identify/add-all", Fixture, NULL, test_add_all);
	AXIS_TEST_ADD_DB_CASE("/identify/import-wins", Fixture, NULL, test_import_wins);
	AXIS_TEST_ADD_DB_CASE("/identify/gone", Fixture, NULL, test_gone);
	AXIS_TEST_ADD_DB_CASE("/identify/schedule", Fixture, NULL, test_schedule);
	return g_test_run();
}
