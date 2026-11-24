/*
 * test-conversion-runs.c - conversion runs against a database: the list
 * (ranked by estimated saving, a series kept together and its episodes in
 * order, every file left out with its one reason), the scopes, the step
 * asking for one file at a time through the single conversion's request,
 * each automatic pause and wait, trials then replacing originals without a
 * new list, caps, hours, cancelling, a restart mid-run, an offline volume
 * (the list and its pages at scale: tests/test-web-conversion-runs.c)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Files are written to a scratch disk at the sizes their probe records
 * imply and scanned by the real scanner; the probe records are written by
 * SQL (tests/axis-test-savings.h), and the worker is rows of the database
 * with measured software encoders.  No encoder runs: a conversion the run
 * asked for is ended by the test, the way its finish job would end it, so
 * that what the run makes of each ending can be shown.  Where the claim,
 * check 10 and the placement themselves are shown (kept files), the test
 * claims the work as a worker would, writes an output of its own to the
 * disk's work area, and lets the real finish job check it again (checks 1,
 * 2, 9 and 10, which need no encoder) and place it.  Without a database
 * every test skips; none needs ffmpeg.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-savings.h"
#include "axis-test-storage.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-analysis.h"
#include "probe/axis-probe.h"
#include "storage/axis-file-check.h"
#include "storage/axis-volume-write.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-conversion-run.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"

typedef struct
{
	gint64               root;
	gchar               *rel;
	AxisTestProbeStream  streams[3];
	gint                 season;
	gint                 episode;
} Pending;

typedef struct
{
	AxisTestLibrary    *t;
	AxisSettings       *settings;
	AxisEstimator      *estimator;
	AxisProbe          *probe;
	AxisConversions    *conv;
	AxisConversionRuns *runs;
	gchar              *disk;
	gint64              volume;
	gint64              movies;
	gint64              tv;
	gint64              space_saver;
	gint64              worker;
	AxisCompatVerdict   verdict;
	GPtrArray          *pending;
	GPtrArray          *strings;	/* what Q() read, freed at the end */
} Fixture;

static AxisCompatVerdict
verdict_of(
	gpointer     data,
	gint64       root_id,
	const gchar *slug
){
	Fixture *fx = data;

	return fx->verdict;
}

static void
pending_free(gpointer data)
{
	Pending *p = data;

	g_free(p->rel);
	g_free(p);
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
	if (!axis_db_pool_exec(fx->t->pool, statement, 0, NULL, NULL, &error))
		g_error("%s: %s", statement, error->message);
}

/* @text, freed with the fixture. */
static const gchar *
keep(
	Fixture *fx,
	gchar   *text
){
	if (text != NULL)
		g_ptr_array_add(fx->strings, text);
	return text;
}

#define N(fx, ...) axis_test_library_n((fx)->t, __VA_ARGS__)
#define Q(fx, ...) keep((fx), axis_test_library_q((fx)->t, __VA_ARGS__))
/* The same, for the caller to free. */
#define QA(fx, ...) axis_test_library_q((fx)->t, __VA_ARGS__)

/* A worker as its register leaves it: connected now, the disk proved, its
 * software encoders measured (the hardware ones not usable here). */
static gint64
add_worker(
	Fixture     *fx,
	const gchar *name,
	gint         max_jobs
){
	g_autoptr(AxisEncoderCapabilities) caps = axis_encoder_capabilities_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *machine = axis_worker_machine(name);
	const AxisConvertEncoder *table = axis_convert_encoders();
	AxisDbConn *conn;
	gint64 id;
	guint i;

	sql(fx, "INSERT INTO worker (name, token_hash, registered_at, last_seen_at, max_jobs, worker_version)"
		" VALUES ('%s', md5('%s') || md5('%s'), now(), now(), %d, '1.0')", name, name, name, max_jobs);
	id = N(fx, "SELECT id FROM worker WHERE name = '%s'", name);
	sql(fx, "INSERT INTO worker_volume (worker_id, volume_id, path) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", '/media/disk1')", id, fx->volume);
	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0000000000000000000000000000000000000000000000000000000000000000");
	caps->aac = caps->mov_text = caps->subrip = TRUE;
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		gboolean soft = table[i].family == AXIS_ENCODER_SOFTWARE;

		axis_encoder_capabilities_set(caps, &table[i], soft, soft && table[i].ten_bit, soft && table[i].ten_bit,
			soft ? NULL : "not on this test's worker");
	}
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_nonnull(conn);
	g_assert_true(axis_convert_capabilities_store(conn, machine, caps, &error));
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_no_error(error);
	return id;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	fx->strings = g_ptr_array_new_with_free_func(g_free);
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "conversions: *");
	fx->t = axis_test_library_new(12);
	if (fx->t == NULL)
		return;
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ESTIMATE_MODULE_ID, axis_estimator_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ANALYSIS_MODULE_ID, axis_analysis_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_CONVERSION_MODULE_ID,
		axis_conversions_get_setting_table(), &error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE, "1MiB");
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT, "0");

	fx->estimator = axis_estimator_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	g_assert_true(axis_profile_store_sync_builtins(fx->t->pool, NULL, &error));
	g_assert_no_error(error);
	fx->conv = axis_conversions_new(fx->t->pool, fx->settings, fx->t->storage, fx->probe, fx->estimator);
	fx->verdict = AXIS_VERDICT_SAFE;
	axis_conversions_set_verdict(fx->conv, verdict_of, fx);
	fx->runs = axis_conversion_runs_new(fx->conv);

	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->movies = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Movies", AXIS_MEDIA_MOVIES);
	fx->tv = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "TV", AXIS_MEDIA_TV);
	fx->space_saver = N(fx, "SELECT current_revision_id FROM conversion_profile WHERE slug = 'space_saver'");
	fx->worker = add_worker(fx, "garage", 1);
	fx->pending = g_ptr_array_new_with_free_func(pending_free);
	/* A run of trials pauses once enough have passed to prove its revision:
	 * the tests of that set this lower. */
	set(fx, AXIS_CONVERSION_SETTING_PROVE_TRIALS, "20");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	g_ptr_array_unref(fx->strings);
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	g_ptr_array_unref(fx->pending);
	axis_conversion_runs_unref(fx->runs);
	axis_conversions_unref(fx->conv);
	axis_probe_unref(fx->probe);
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── The library ────────────────────────────────────────────────── */

static const gchar *
root_dir(
	Fixture *fx,
	gint64   root
){
	return root == fx->tv ? "TV" : "Movies";
}

/* A video of @mbits Mbit/s (H.264 1080p, AAC stereo), one second long,
 * written now and probed by settle(); @codec and @uncertain for the files
 * the estimator leaves out. */
static void
put_video(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	const gchar *codec,
	gdouble      mbits,
	gboolean     uncertain,
	gint         season,
	gint         episode
){
	Pending *p = g_new0(Pending, 1);

	p->root = root;
	p->rel = g_strdup(rel);
	p->season = season;
	p->episode = episode;
	p->streams[0].kind = "video";
	p->streams[0].codec = codec;
	p->streams[0].bitrate = (gint64)(mbits * 1000000);
	p->streams[0].width = 1920;
	p->streams[0].height = 1080;
	p->streams[0].fps = 24.0;
	p->streams[0].hdr = "sdr";
	p->streams[0].hdr_uncertain = uncertain;
	p->streams[0].field_order = "progressive";
	p->streams[1].kind = "audio";
	p->streams[1].codec = "aac";
	p->streams[1].bitrate = 128000;
	p->streams[1].channels = 2;
	p->streams[1].sample_rate = 48000;
	p->streams[1].lossless = "false";
	p->streams[1].language = "eng";
	axis_test_scenario_put(fx->disk, root_dir(fx, root), rel, axis_test_savings_size(p->streams, 1.0));
	g_ptr_array_add(fx->pending, p);
}

static void
movie(
	Fixture     *fx,
	const gchar *title,
	gdouble      mbits
){
	g_autofree gchar *rel = g_strdup_printf("%s/%s.mkv", title, title);

	put_video(fx, fx->movies, rel, "h264", mbits, FALSE, -1, -1);
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

	put_video(fx, fx->tv, rel, "h264", mbits, FALSE, season, number);
}

static gint64
file_of(
	Fixture     *fx,
	gint64       root,
	const gchar *rel
){
	return axis_test_scenario_file(fx->t, root, rel);
}

/* Scans both roots and writes the probe records of what was put. */
static void
settle(Fixture *fx)
{
	guint i;

	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->movies, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->tv, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	for (i = 0; i < fx->pending->len; i++) {
		Pending *p = g_ptr_array_index(fx->pending, i);
		gint64 id = file_of(fx, p->root, p->rel);

		if (N(fx, "SELECT count(*) FROM media_info WHERE file_id = %" G_GINT64_FORMAT, id) > 0)
			continue;
		axis_test_savings_probe(fx->t, id, "matroska,webm", 1.0, p->streams, "false");
		sql(fx, "UPDATE media_stream SET pixel_format = 'yuv420p' WHERE file_id = %" G_GINT64_FORMAT " AND kind = 'video'",
			id);
	}
}

/* The series at @show's folder, identified (linked) as the made-up series
 * @title, with its files' episodes as their names say. */
static gint64
identify_series(
	Fixture     *fx,
	const gchar *show,
	const gchar *title,
	gint         year
){
	gint64 title_id;
	gint64 identity;
	guint i;

	sql(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id, detail_fetched_at, name_keys)"
		" VALUES ('series', '%s', %d, 'ended', 'fixture', 'test-%s', now(), lower(replace('%s', ' ', '')))", title, year,
		show, title);
	title_id = N(fx, "SELECT id FROM meta_title WHERE provider_id = 'test-%s'", show);
	sql(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence, title_id,"
		" linked_how) VALUES (%" G_GINT64_FORMAT ", library_path_hash('%s'), 'folder', '%s', 'linked', 'sure', %"
		G_GINT64_FORMAT ", 'auto')", fx->tv, show, show, title_id);
	identity = N(fx, "SELECT id FROM library_identity WHERE root_id = %" G_GINT64_FORMAT " AND rel_path = '%s'", fx->tv,
		show);
	for (i = 0; i < fx->pending->len; i++) {
		Pending *p = g_ptr_array_index(fx->pending, i);
		gint64 file;

		if (p->root != fx->tv || !g_str_has_prefix(p->rel, show) || p->rel[strlen(show)] != '/')
			continue;
		file = file_of(fx, p->root, p->rel);
		sql(fx, "INSERT INTO library_identity_file (file_id, identity_id, best, quality) VALUES (%" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ", false, 'webdl_1080p')", file, identity);
		if (p->season >= 0)
			sql(fx, "INSERT INTO library_identity_episode (file_id, position, season_number, number) VALUES (%"
				G_GINT64_FORMAT ", 0, %d, %d)", file, p->season, p->episode);
	}
	return title_id;
}

/* ── Runs ───────────────────────────────────────────────────────── */

static gint64
make_run(
	Fixture                *fx,
	AxisConversionRunScope  scope,
	const gint64           *roots,
	guint                   n_roots,
	gint64                  title,
	gint64                  revision
){
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_conversion_runs_create(fx->runs, scope, roots, n_roots, title, revision, "alice", &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run WHERE id = %" G_GINT64_FORMAT, id), ==, "listing");
	/* The job's work, done here. */
	g_assert_true(axis_conversion_runs_build_list(fx->runs, id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run WHERE id = %" G_GINT64_FORMAT, id), ==, "ready");
	return id;
}

static void
start(
	Fixture                *fx,
	gint64                  run,
	AxisConversionOutcome   outcome
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_conversion_runs_start(fx->runs, run, outcome, outcome == AXIS_CONVERSION_REPLACE, "alice",
		&error));
	g_assert_no_error(error);
}

static AxisConversionRunStepReport
step(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunStepReport report;

	g_assert_true(axis_conversion_runs_step(fx->runs, &report, &error));
	g_assert_no_error(error);
	return report;
}

static gchar *
run_state(
	Fixture *fx,
	gint64   run
){
	return QA(fx, "SELECT state || coalesce('/' || pause_code, '') || coalesce('+' || wait_code, '') FROM conversion_run"
		" WHERE id = %" G_GINT64_FORMAT, run);
}

static void
assert_state(
	Fixture     *fx,
	gint64       run,
	const gchar *expected
){
	g_autofree gchar *state = run_state(fx, run);

	if (g_strcmp0(state, expected) != 0) {
		g_autofree gchar *why = QA(fx, "SELECT coalesce(pause_text, '') || ' | ' || coalesce(wait_text, '') FROM"
			" conversion_run WHERE id = %" G_GINT64_FORMAT, run);

		g_error("run %" G_GINT64_FORMAT " is %s, not %s (%s)", run, state, expected, why);
	}
}

/* The files of the run in flight, in the run's order, as "name" of their
 * paths' last part. */
static const gchar *
asked_names(
	Fixture *fx,
	gint64   run
){
	return Q(fx, "SELECT coalesce(string_agg(regexp_replace(rel_path, '^.*/', ''), ',' ORDER BY position), '')"
		" FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'asked'", run);
}

/* The conversion of the entry in flight named @name. */
static gint64
asked_conversion(
	Fixture     *fx,
	gint64       run,
	const gchar *name
){
	return N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'asked'"
		" AND rel_path LIKE '%%%s'", run, name);
}

/* Ends a conversion the way its finish job would have: done (with its ten
 * checks passed and the converted file's size), rejected by @check, or
 * failed. */
static void
end_conversion(
	Fixture     *fx,
	gint64       conversion,
	const gchar *state,
	gint         check,
	gdouble      ratio
){
	if (g_strcmp0(state, "done") == 0) {
		sql(fx, "INSERT INTO conversion_check (conversion_id, number, result, detail) SELECT %" G_GINT64_FORMAT ", n,"
			" 'passed', 'as planned' FROM generate_series(1, 10) n", conversion);
		sql(fx, "UPDATE conversion SET state = 'done', finished_at = now(), output_size = (src_size * %f)::bigint,"
			" output_reported = (src_size * %f)::bigint, trial_name = CASE WHEN outcome = 'trial' THEN 'trial-' || id || '.mkv'"
			" END, trial_state = CASE WHEN outcome = 'trial' THEN 'kept' END WHERE id = %" G_GINT64_FORMAT, ratio, ratio,
			conversion);
	} else if (g_strcmp0(state, "rejected") == 0) {
		sql(fx, "UPDATE conversion SET state = 'rejected', failed_check = %d, finished_at = now(),"
			" result_text = 'Rejected by check %d in this test.' WHERE id = %" G_GINT64_FORMAT, check, check, conversion);
	} else {
		sql(fx, "UPDATE conversion SET state = '%s', finished_at = now(), result_text = 'It %s in this test.' WHERE id = %"
			G_GINT64_FORMAT, state, state, conversion);
	}
}

/* Ends the one conversion in flight of @run, done. */
static void
end_asked(
	Fixture     *fx,
	gint64       run,
	const gchar *state,
	gint         check
){
	gint64 conversion = N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'asked' ORDER BY position LIMIT 1", run);

	g_assert_cmpint(conversion, >, 0);
	end_conversion(fx, conversion, state, check, 0.4);
}

static gchar *
unit_order(
	Fixture *fx,
	gint64   run
){
	return QA(fx, "SELECT string_agg(u.kind || ':' || coalesce(t.title, regexp_replace(u.rel_path, '^.*/', '')), ','"
		" ORDER BY u.position) FROM conversion_run_unit u LEFT JOIN meta_title t ON t.id = u.title_id WHERE u.run_id = %"
		G_GINT64_FORMAT, run);
}

static gchar *
entry_order(
	Fixture *fx,
	gint64   run
){
	return QA(fx, "SELECT string_agg(regexp_replace(rel_path, '^.*/', ''), ',' ORDER BY position)"
		" FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run);
}

static gint64
reason_count(
	Fixture     *fx,
	gint64       run,
	const gchar *reason
){
	return N(fx, "SELECT coalesce((SELECT files FROM conversion_run_reason WHERE run_id = %" G_GINT64_FORMAT
		" AND reason = '%s'), 0)", run, reason);
}

/* Four movies to convert, scanned and probed: by saving, Copper Road,
 * Glass Orchard, Lantern Hill, Paper Tide. */
static void
four_movies(Fixture *fx)
{
	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	movie(fx, "Paper Tide (2004)", 9);
	settle(fx);
}

/* ── The list ───────────────────────────────────────────────────── */

/*
 * Spec (conversion-runs): A suggested list ranked by saving; Files left out
 * keep their reason.  The list holds what the estimator would convert,
 * largest estimated saving first, with the estimator's own figures; every
 * other file of the scope is counted under the one reason that keeps it
 * out (the conversions' memory and a conversion in flight before the
 * estimate), and can be looked at.
 */
static void
test_list_ranked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) estimates = NULL;
	g_autoptr(GPtrArray) left = NULL;
	g_autoptr(AxisConversionRunInfo) info = NULL;
	g_autofree gchar *order = NULL;
	gint64 copper;
	gint64 lantern;
	gint64 run;
	gint64 conversion = 0;
	gboolean more = FALSE;
	guint i;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	movie(fx, "Paper Tide (2004)", 9);
	put_video(fx, fx->movies, "Quiet Field (2005)/Quiet Field (2005).mkv", "hevc", 1.5, FALSE, -1, -1);
	put_video(fx, fx->movies, "Faint Signal (2006)/Faint Signal (2006).mkv", "h264", 12, TRUE, -1, -1);
	movie(fx, "Worn Map (2007)", 12);
	movie(fx, "Second Look (2008)", 12);
	movie(fx, "Busy Line (2009)", 12);
	axis_test_scenario_put(fx->disk, "Movies", "Unread (2010)/Unread (2010).mkv", 5000);
	settle(fx);
	/* A hard link makes a file pinned: both its names are. */
	{
		g_autofree gchar *from = g_build_filename(fx->disk, "Movies", "Worn Map (2007)", "Worn Map (2007).mkv", NULL);
		g_autofree gchar *to = g_build_filename(fx->disk, "Movies", "Worn Map (2007)", "Worn Map copy.mkv", NULL);

		g_assert_cmpint(link(from, to), ==, 0);
		axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->movies, AXIS_SCAN_COMPLETE,
			AXIS_SCAN_REASON_NONE));
	}
	/* Remembered as not worth it with this revision; one in flight. */
	sql(fx, "INSERT INTO conversion_memory (file_id, revision_id, reason) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", 'not_worth_it')", file_of(fx, fx->movies, "Second Look (2008)/Second Look (2008).mkv"), fx->space_saver);
	g_assert_true(axis_conversions_request(fx->conv, file_of(fx, fx->movies, "Busy Line (2009)/Busy Line (2009).mkv"),
		fx->space_saver, AXIS_CONVERSION_TRIAL, FALSE, "bob", &conversion, &error));
	g_assert_no_error(error);

	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Copper Road (2002).mkv,Glass Orchard (2003).mkv,Lantern Hill (2001).mkv,"
		"Paper Tide (2004).mkv");
	/* Every movie is a unit of its own, ranked by its saving. */
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
		" AND kind = 'file' AND files = 1", run), ==, 4);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM (SELECT position, bytes - mid AS saving, lag(bytes - mid) OVER (ORDER BY"
		" position) AS before FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT ") t WHERE t.saving > t.before",
		run), ==, 0);
	/* The estimator's own figures. */
	copper = file_of(fx, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv");
	g_assert_true(axis_estimator_estimate_file(fx->estimator, copper, &estimates, &error));
	g_assert_no_error(error);
	for (i = 0; i < estimates->len; i++) {
		const AxisFileEstimate *e = &g_array_index(estimates, AxisFileEstimate, i);

		if (g_strcmp0(e->target.id, "space_saver") != 0)
			continue;
		g_assert_cmpint(N(fx, "SELECT mid FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND file_id = %"
			G_GINT64_FORMAT, run, copper), ==, e->result.mid);
		g_assert_cmpint(N(fx, "SELECT low FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND file_id = %"
			G_GINT64_FORMAT, run, copper), ==, e->result.low);
	}
	/* What is left out, and why. */
	g_assert_cmpint(reason_count(fx, run, "already_efficient") + reason_count(fx, run, "better_codec"), ==, 1);
	g_assert_cmpint(reason_count(fx, run, "hdr_uncertain"), ==, 1);
	g_assert_cmpint(reason_count(fx, run, "pinned"), ==, 2);
	g_assert_cmpint(reason_count(fx, run, "not_probed"), ==, 1);
	g_assert_cmpint(reason_count(fx, run, "not_worth_it"), ==, 1);
	g_assert_cmpint(reason_count(fx, run, "in_flight"), ==, 1);
	g_assert_cmpint(N(fx, "SELECT scope_files FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 11);
	g_assert_true(axis_conversion_runs_excluded(fx->runs, run, "pinned", 0, 0, 1, &left, &more, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(left->len, ==, 1);
	g_assert_true(more);
	g_assert_cmpstr(((AxisConversionRunExcluded *)g_ptr_array_index(left, 0))->rel_path, !=, NULL);
	g_clear_pointer(&left, g_ptr_array_unref);
	{
		const AxisConversionRunExcluded *first = NULL;
		g_autoptr(GPtrArray) all = NULL;
		g_autoptr(GPtrArray) rest = NULL;

		g_assert_true(axis_conversion_runs_excluded(fx->runs, run, "pinned", 0, 0, 10, &all, &more, &error));
		g_assert_cmpuint(all->len, ==, 2);
		first = g_ptr_array_index(all, 0);
		g_assert_true(axis_conversion_runs_excluded(fx->runs, run, "pinned", first->size, first->file_id, 10, &rest,
			&more, &error));
		g_assert_cmpuint(rest->len, ==, 1);
		g_assert_false(more);
	}

	/* The summary: the list's totals and the scope. */
	if (!axis_conversion_runs_get(fx->runs, run, &info, &error))
		g_error("the run's summary: %s", error->message);
	g_assert_no_error(error);
	g_assert_cmpint(info->units, ==, 4);
	g_assert_cmpint(info->files, ==, 4);
	g_assert_cmpint(info->waiting, ==, 4);
	g_assert_cmpstr(info->scope_text, ==, "Every movie library folder");
	g_assert_cmpint(info->bytes, ==, N(fx, "SELECT sum(size) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT,
		run));
	g_assert_cmpint(info->mid, <, info->bytes);
	lantern = file_of(fx, fx->movies, "Lantern Hill (2001)/Lantern Hill (2001).mkv");
	g_assert_cmpint(lantern, >, 0);
	/* Nothing is converted by making a list. */
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 0);
}

/*
 * A file whose link is kept (by hand, or brought over) is left out of the
 * list with the reason "kept", for a movie's file and an episode's alike, and
 * counted under it; letting it go lists it again in the next run.
 */
static void
test_list_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *order = NULL;
	gint64 film;
	gint64 show;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	settle(fx);
	film = file_of(fx, fx->movies, "Lantern Hill (2001)/Lantern Hill (2001).mkv");
	show = file_of(fx, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv");
	sql(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Kept Film', 2001,"
		" 'fixture', 'kept-film'), ('series', 'Kept Show', 2002, 'fixture', 'kept-show')");
	sql(fx, "INSERT INTO movie (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider_id = 'kept-film'");
	sql(fx, "INSERT INTO series (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider_id = 'kept-show'");
	sql(fx, "INSERT INTO movie_file (file_id, movie_id, quality, kept) SELECT %" G_GINT64_FORMAT ", id, 'webdl_1080p', true"
		" FROM movie", film);
	sql(fx, "INSERT INTO series_file (file_id, series_id, quality, kept, kept_how) SELECT %" G_GINT64_FORMAT
		", id, 'webdl_1080p', true, 'brought' FROM series", show);

	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Glass Orchard (2003).mkv");
	g_assert_cmpint(reason_count(fx, run, "kept"), ==, 2);
	g_assert_cmpint(N(fx, "SELECT scope_files FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 3);
	g_assert_cmpstr(axis_conversion_run_reason_label("kept"), ==, "Kept: Axis does not replace this file");
	g_assert_cmpstr(axis_conversion_run_reason_help("kept"), ==,
		"Axis does not replace these files with another release. A run with \"Also convert files Axis keeps\" "
		"converts them in place, and they stay kept. Or let Axis upgrade a file again on its links page.");
	/* The default is off, and nothing of the choice is recorded. */
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text || coalesce(convert_kept_by, '-') FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, run), ==, "false-");

	/* A person lets one go: the next run lists it. */
	sql(fx, "UPDATE movie_file SET kept = false, kept_how = NULL WHERE file_id = %" G_GINT64_FORMAT, film);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	g_assert_cmpint(reason_count(fx, run, "kept"), ==, 1);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run), ==, 2);
}

/*
 * Spec (conversion-runs): A series is one unit.  Its episode files, in
 * whichever folder, are ranked together by their total saving and listed in
 * season and episode order; a TV folder that no identified series is is
 * taken as one; a file at a TV root's top is a unit of its own.
 */
static void
test_list_series(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) units = NULL;
	g_autoptr(AxisConversionRunUnit) harbour = NULL;
	g_autofree gchar *order = NULL;
	gboolean more = FALSE;
	gint64 run;

	/* Made in no order: the list puts them in order. */
	episode(fx, "Harbour Lights", 1, 3, 10);
	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Harbour Lights", 2, 1, 10);
	episode(fx, "Harbour Lights", 1, 2, 10);
	episode(fx, "Quiet Meadow", 1, 2, 30);
	episode(fx, "Quiet Meadow", 1, 1, 30);
	episode(fx, "Loose Folder", 1, 1, 14);
	put_video(fx, fx->tv, "Pilot.mkv", "h264", 11, FALSE, -1, -1);
	settle(fx);
	identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);

	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);
	order = unit_order(fx, run);
	g_assert_cmpstr(order, ==, "series:Quiet Meadow,series:Harbour Lights,folder:Loose Folder,file:Pilot.mkv");
	g_clear_pointer(&order, g_free);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Quiet Meadow - S01E01.mkv,Quiet Meadow - S01E02.mkv,Harbour Lights - S01E01.mkv,"
		"Harbour Lights - S01E02.mkv,Harbour Lights - S01E03.mkv,Harbour Lights - S02E01.mkv,"
		"Loose Folder - S01E01.mkv,Pilot.mkv");
	/* Per unit, its progress from its files. */
	g_assert_true(axis_conversion_runs_units(fx->runs, run, 0, 50, &units, &more, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(units->len, ==, 4);
	g_assert_false(more);
	g_assert_cmpstr(((AxisConversionRunUnit *)g_ptr_array_index(units, 1))->title, ==, "Harbour Lights");
	g_assert_cmpint(((AxisConversionRunUnit *)g_ptr_array_index(units, 1))->year, ==, 2011);
	g_assert_cmpint(((AxisConversionRunUnit *)g_ptr_array_index(units, 1))->files, ==, 4);
	g_assert_cmpint(((AxisConversionRunUnit *)g_ptr_array_index(units, 1))->waiting, ==, 4);
	g_assert_true(axis_conversion_runs_unit(fx->runs, run, 2, &harbour, &error));
	g_assert_cmpint(harbour->files, ==, 4);
	/* A page of one: the pager says there is more. */
	g_clear_pointer(&units, g_ptr_array_unref);
	g_assert_true(axis_conversion_runs_units(fx->runs, run, 1, 1, &units, &more, &error));
	g_assert_cmpuint(units->len, ==, 1);
	g_assert_cmpint(((AxisConversionRunUnit *)g_ptr_array_index(units, 0))->position, ==, 2);
	g_assert_true(more);
}

/* Spec (conversion-runs): Scopes.  Libraries chosen, and one series. */
static void
test_list_scopes(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	gint64 title;
	gint64 run;
	gint64 roots[1];
	gint64 none = 0;

	movie(fx, "Lantern Hill (2001)", 12);
	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Harbour Lights", 1, 2, 10);
	episode(fx, "Quiet Meadow", 1, 1, 30);
	settle(fx);
	title = identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);

	roots[0] = fx->movies;
	run = make_run(fx, AXIS_CONVERSION_RUN_ROOTS, roots, 1, 0, fx->space_saver);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Lantern Hill (2001).mkv");
	g_assert_cmpstr(Q(fx, "SELECT scope_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"The library folder Movies");
	g_clear_pointer(&order, g_free);

	run = make_run(fx, AXIS_CONVERSION_RUN_SERIES, NULL, 0, title, fx->space_saver);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Harbour Lights - S01E01.mkv,Harbour Lights - S01E02.mkv");
	g_assert_cmpstr(Q(fx, "SELECT scope_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"The series Harbour Lights (2011)");

	/* A person's click on the same roots in another order, its run not
	 * started: that run, not another (add-activity-feedback). */
	{
		gint64 both[2];
		gint64 first = 0;
		gint64 again = 0;
		gboolean created = FALSE;

		both[0] = fx->movies;
		both[1] = fx->tv;
		g_assert_true(axis_conversion_runs_create_full(fx->runs, AXIS_CONVERSION_RUN_ROOTS, both, 2, 0, fx->space_saver,
			FALSE, "alice", &first, &created, &error));
		g_assert_no_error(error);
		g_assert_true(created);
		both[0] = fx->tv;
		both[1] = fx->movies;
		g_assert_true(axis_conversion_runs_create_full(fx->runs, AXIS_CONVERSION_RUN_ROOTS, both, 2, 0, fx->space_saver,
			FALSE, "alice", &again, &created, &error));
		g_assert_no_error(error);
		g_assert_false(created);
		g_assert_cmpint(again, ==, first);
		/* The same selection with the other choice about kept files is
		 * another run (convert-kept-files). */
		g_assert_true(axis_conversion_runs_create_full(fx->runs, AXIS_CONVERSION_RUN_ROOTS, both, 2, 0, fx->space_saver,
			TRUE, "alice", &again, &created, &error));
		g_assert_no_error(error);
		g_assert_true(created);
		g_assert_cmpint(again, !=, first);
	}

	/* What a run cannot be made of. */
	g_assert_false(axis_conversion_runs_create(fx->runs, AXIS_CONVERSION_RUN_SERIES, NULL, 0, title + 1000,
		fx->space_saver, "alice", &none, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(axis_conversion_runs_create(fx->runs, AXIS_CONVERSION_RUN_ROOTS, NULL, 0, 0, fx->space_saver,
		"alice", &none, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(axis_conversion_runs_create(fx->runs, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver, NULL,
		&none, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
}

/* ── Asking ─────────────────────────────────────────────────────── */

/*
 * Spec (conversion-runs): One file at a time, through the single request.
 * The run asks for its first file, an ordinary conversion of its own
 * (queued, its encode job for a worker, the run and the person recorded),
 * and for nothing more until it has ended; then the next, in the list's
 * order.  At once is a setting, never more than the workers can take.
 */
static void
test_one_at_a_time(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunLimits limits;
	AxisConversionRunStepReport report;
	const gchar *names = NULL;
	gint64 conversion;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);

	/* Not started: nothing is asked for. */
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 0);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 1);
	names = asked_names(fx, run);
	g_assert_cmpstr(names, ==, "Copper Road (2002).mkv");
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	/* An ordinary conversion, the run's. */
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || outcome || ' ' || requested_by || ' ' || source_hdr || ' ' || run_id"
		" FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==, keep(fx, g_strdup_printf("queued trial alice "
		"sdr %" G_GINT64_FORMAT, run)));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM job j JOIN conversion c ON c.job_id = j.id WHERE c.id = %"
		G_GINT64_FORMAT " AND j.kind = 'convert.encode' AND j.state = 'queued'", conversion), ==, 1);
	g_assert_cmpint(N(fx, "SELECT run_entry_id FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==,
		N(fx, "SELECT id FROM conversion_run_entry WHERE conversion_id = %" G_GINT64_FORMAT, conversion));

	/* One at a time. */
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 0);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);
	/* It ends: the next. */
	end_conversion(fx, conversion, "done", 0, 0.4);
	report = step(fx);
	g_assert_cmpuint(report.settled, ==, 1);
	g_assert_cmpuint(report.asked, ==, 1);
	names = asked_names(fx, run);
	g_assert_cmpstr(names, ==, "Glass Orchard (2003).mkv");
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || result_size FROM conversion_run_entry WHERE conversion_id = %"
		G_GINT64_FORMAT, conversion), ==, Q(fx, "SELECT 'done ' || output_size FROM conversion WHERE id = %"
		G_GINT64_FORMAT, conversion));

	/* Two at once, but the one worker takes one job at a time. */
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 2;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_no_error(error);
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 0);
	sql(fx, "UPDATE worker SET max_jobs = 4 WHERE id = %" G_GINT64_FORMAT, fx->worker);
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 1);
	names = asked_names(fx, run);
	g_assert_cmpstr(names, ==, "Glass Orchard (2003).mkv,Lantern Hill (2001).mkv");
	/* The rest, and the run finishes when its list is. */
	end_conversion(fx, asked_conversion(fx, run, "Glass Orchard (2003).mkv"), "done", 0, 0.4);
	end_conversion(fx, asked_conversion(fx, run, "Lantern Hill (2001).mkv"), "done", 0, 0.4);
	report = step(fx);
	g_assert_cmpuint(report.asked, ==, 1);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'done'", run), ==, 4);
}

/*
 * Spec (conversion-runs): A series is finished before the next.  With one
 * file at a time the run asks for a series' episodes one after another, in
 * order, before the next unit's; its progress reads "N of M".
 */
static void
test_series_together(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) asked = g_string_new(NULL);
	gint64 run;
	guint i;

	episode(fx, "Harbour Lights", 1, 2, 10);
	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Quiet Meadow", 1, 1, 30);
	episode(fx, "Quiet Meadow", 1, 2, 30);
	episode(fx, "Quiet Meadow", 1, 3, 30);
	settle(fx);
	identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);
	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	for (i = 0; i < 5; i++) {
		step(fx);
		g_string_append_printf(asked, "%s%s", i > 0 ? "," : "", asked_names(fx, run));
		if (i == 1) {
			g_autoptr(AxisConversionRunUnit) meadow = NULL;

			g_assert_true(axis_conversion_runs_unit(fx->runs, run, 1, &meadow, &error));
			g_assert_cmpint(meadow->done, ==, 1);
			g_assert_cmpint(meadow->asked, ==, 1);
			g_assert_cmpint(meadow->files, ==, 3);
		}
		end_asked(fx, run, "done", 0);
	}
	g_assert_cmpstr(asked->str, ==, "Quiet Meadow - S01E01.mkv,Quiet Meadow - S01E02.mkv,Quiet Meadow - S01E03.mkv,"
		"Harbour Lights - S01E01.mkv,Harbour Lights - S01E02.mkv");
	step(fx);
	assert_state(fx, run, "finished");
}

/*
 * Spec (conversion-runs): A series started is finished first.  The run went
 * on with the second series while the first one's files waited (its disk
 * away): when the first one's files can be asked for again, the run first
 * finishes the series it is working through, then goes back.
 */
static void
test_series_started_first(Fixture *fx, gconstpointer data)
{
	gint64 run;

	episode(fx, "Quiet Meadow", 1, 1, 30);
	episode(fx, "Quiet Meadow", 1, 2, 30);
	episode(fx, "Quiet Meadow", 1, 3, 30);
	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Harbour Lights", 1, 2, 10);
	settle(fx);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);
	identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	g_assert_cmpstr(asked_names(fx, run), ==, "Quiet Meadow - S01E01.mkv");
	end_asked(fx, run, "done", 0);
	/* As the run left it, having gone on with Harbour Lights. */
	sql(fx, "UPDATE conversion_run SET current_unit = 2 WHERE id = %" G_GINT64_FORMAT, run);
	step(fx);
	g_assert_cmpstr(asked_names(fx, run), ==, "Harbour Lights - S01E01.mkv");
	end_asked(fx, run, "done", 0);
	step(fx);
	g_assert_cmpstr(asked_names(fx, run), ==, "Harbour Lights - S01E02.mkv");
	end_asked(fx, run, "done", 0);
	step(fx);
	g_assert_cmpstr(asked_names(fx, run), ==, "Quiet Meadow - S01E02.mkv");
	g_assert_cmpint(N(fx, "SELECT current_unit FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 1);
}

/* ── Pauses and waits ───────────────────────────────────────────── */

/*
 * Spec (conversion-runs): Failures in a row pause the run.  Three files in
 * a row rejected by a check of the converted file (or failed) pause it,
 * saying why; one done between starts the count again; a file not worth
 * it, or changed, is skipped with its reason and does not count; a resume
 * starts the count again.
 */
static void
test_failures_pause(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 run;

	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	movie(fx, "Paper Tide (2004)", 9);
	movie(fx, "Amber Coast (2005)", 10);
	movie(fx, "Night Ferry (2006)", 14);
	movie(fx, "Rain Hollow (2007)", 16);
	settle(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);

	step(fx);
	end_asked(fx, run, "rejected", 9);	/* not worth it: skipped */
	step(fx);
	end_asked(fx, run, "failed", 0);	/* 1 */
	step(fx);
	end_asked(fx, run, "done", 0);	/* the count starts again */
	step(fx);
	end_asked(fx, run, "rejected", 1);	/* 1 */
	step(fx);
	end_asked(fx, run, "failed", 0);	/* 2 */
	step(fx);
	assert_state(fx, run, "running");
	g_assert_cmpint(N(fx, "SELECT failed_in_row FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 2);
	end_asked(fx, run, "failed", 0);	/* 3 */
	step(fx);
	assert_state(fx, run, "paused/failures");
	g_assert_cmpstr(asked_names(fx, run), ==, "");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"3 files in a row failed or were rejected"));
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || reason_code FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" ORDER BY position LIMIT 1", run), ==, "skipped check_9");

	/* A person resumes: the count starts again, the next file is asked. */
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpint(N(fx, "SELECT failed_in_row FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 0);
	g_assert_cmpuint(step(fx).asked, ==, 1);
}

/*
 * Spec (conversion-runs): An encoder crash pauses the run.  The worker
 * paused itself after its encoder died on the run's file: the run pauses
 * too, naming the worker and the file.
 */
static void
test_crash_pause(Fixture *fx, gconstpointer data)
{
	gint64 conversion;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	sql(fx, "UPDATE conversion SET state = 'encoding' WHERE id = %" G_GINT64_FORMAT, conversion);
	sql(fx, "UPDATE worker SET paused_at = now(), paused_job = (SELECT job_id FROM conversion WHERE id = %"
		G_GINT64_FORMAT "), paused_reason = 'ffmpeg died of signal 11' WHERE id = %" G_GINT64_FORMAT, conversion,
		fx->worker);
	step(fx);
	assert_state(fx, run, "paused/crash");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"The encoder crashed on worker garage while it converted Copper Road (2002).mkv (ffmpeg died of signal 11)"));
	/* Nothing more is asked for, and the file stays in flight. */
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
}

/*
 * Spec (conversion-runs): No worker, no request.  With no suitable worker
 * connected the run waits and says so, and asks for nothing (this server
 * never converts in a worker's place); it goes on by itself when one is.
 */
static void
test_no_worker_waits(Fixture *fx, gconstpointer data)
{
	gint64 run;

	four_movies(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker_only");
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	sql(fx, "UPDATE worker SET last_seen_at = now() - interval '10 minutes' WHERE id = %" G_GINT64_FORMAT, fx->worker);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+no_worker");
	g_assert_nonnull(strstr(Q(fx, "SELECT wait_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"this server neither converts nor checks in a worker's place"));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 0);
	/* A paused worker is no help either. */
	sql(fx, "UPDATE worker SET last_seen_at = now(), paused_at = now(), paused_job = 1, paused_reason = 'x' WHERE id = %"
		G_GINT64_FORMAT, fx->worker);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+no_worker");
	sql(fx, "UPDATE worker SET paused_at = NULL, paused_job = NULL, paused_reason = NULL WHERE id = %" G_GINT64_FORMAT,
		fx->worker);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
}

/* Spec (conversion-runs): Read-only storage holds the run, by itself. */
static void
test_read_only_waits(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+read_only");
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
}

/*
 * Spec (conversion-runs): An offline volume freezes its files.  The disk
 * goes away: the run asks for nothing on it, marks nothing failed or
 * skipped, and says which volume its files wait for; the disk comes back
 * and the run goes on by itself.  A file in flight stays in flight.
 */
static void
test_offline_volume(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	end_asked(fx, run, "done", 0);

	axis_test_library_unplug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+volumes");
	g_assert_cmpstr(Q(fx, "SELECT code FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT " AND volume_id = %"
		G_GINT64_FORMAT, run, fx->volume), ==, "offline");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state IN ('skipped', 'failed')", run), ==, 0);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'waiting'", run), ==, 3);
	/* A list made while it is away still has its files (a trial leaves the
	 * library as it was): they wait for the volume. */
	{
		gint64 later = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);

		g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, later), ==, 4);
	}

	axis_test_library_plug(fx->t, fx->disk, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT, run), ==, 0);
}

/*
 * Spec (conversion-runs): A volume below its floor freezes its files.  The
 * request is refused by the free-space gate: the file stays in the list,
 * the volume's files wait, and the run says so.
 */
static void
test_full_volume(Fixture *fx, gconstpointer data)
{
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE, "1000000TiB");
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+volumes");
	g_assert_cmpstr(Q(fx, "SELECT code FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT, run), ==, "full");
	g_assert_nonnull(strstr(Q(fx, "SELECT text FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT, run),
		"Enough free space stays on the disk"));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'waiting'", run), ==, 4);
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE, "1MiB");
	g_assert_cmpuint(step(fx).asked, ==, 1);
}

/*
 * Spec (conversion-runs): A restart holds the run.  The daemon starts again
 * with a file of the run in flight: while conversion work is held
 * (convert.wait_after_start) the run asks for nothing and says why, the file
 * in flight is still followed and recorded when it ends; after the resume
 * the run goes on by itself.
 */
static void
test_restart(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversions) conv = NULL;
	g_autoptr(AxisConversionRuns) runs = NULL;
	AxisConversionRunStepReport report;
	gboolean was_held = FALSE;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");

	/* The daemon again: new objects on the same database, held. */
	conv = axis_conversions_new(fx->t->pool, fx->settings, fx->t->storage, fx->probe, fx->estimator);
	axis_conversions_set_verdict(conv, verdict_of, fx);
	axis_conversions_hold_after_start(conv);
	runs = axis_conversion_runs_new(conv);
	g_assert_true(axis_conversion_runs_step(runs, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.asked, ==, 0);
	assert_state(fx, run, "running+held");
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	/* The file in flight ends while held: it is recorded; nothing new. */
	end_asked(fx, run, "done", 0);
	g_assert_true(axis_conversion_runs_step(runs, &report, &error));
	g_assert_cmpuint(report.settled, ==, 1);
	g_assert_cmpuint(report.asked, ==, 0);
	assert_state(fx, run, "running+held");
	/* A person resumes conversion work: the run goes on. */
	g_assert_true(axis_conversions_resume(conv, "alice", &was_held, &error));
	g_assert_true(was_held);
	g_assert_true(axis_conversion_runs_step(runs, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.asked, ==, 1);
	assert_state(fx, run, "running");
}

/*
 * Spec (conversion-runs): The player check stops saying safe.  A run
 * replacing originals asks for replacements while the gate holds; when the
 * verdict for the library is not safe any more, the request is refused by
 * that gate and the run pauses, the file left in the list.
 */
static void
test_verdict_pause(Fixture *fx, gconstpointer data)
{
	gint64 conversion;
	gint64 run;

	four_movies(fx);
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_REPLACE);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || verdict || ' ' || requested_by FROM conversion WHERE id = %"
		G_GINT64_FORMAT, conversion), ==, "replace safe alice");
	end_conversion(fx, conversion, "done", 0, 0.4);
	fx->verdict = AXIS_VERDICT_UNKNOWN;
	step(fx);
	assert_state(fx, run, "paused/verdict");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"The verdict is unknown; a replacement needs safe."));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'waiting'", run), ==, 3);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);
}

/*
 * Spec (conversion-runs): Trials, then replacing, with the same list.  A run
 * with a locked revision makes trials only, and pauses once enough have
 * passed to prove the revision; it cannot replace until a person proved it;
 * then it replaces, asking again for the files it converted as trials, and
 * the requests are replacements by the person who confirmed.
 */
static void
test_trials_then_replace(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	g_autoptr(GArray) trials = g_array_new(FALSE, FALSE, sizeof(gint64));
	gint64 run;
	guint i;

	four_movies(fx);
	set(fx, AXIS_CONVERSION_SETTING_PROVE_TRIALS, "2");
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	/* A locked revision cannot start replacing. */
	g_assert_false(axis_conversion_runs_start(fx->runs, run, AXIS_CONVERSION_REPLACE, TRUE, "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	end_asked(fx, run, "done", 0);
	step(fx);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "paused/prove");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"2 trials of this version passed every check"));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE run_id = %" G_GINT64_FORMAT, run), ==, 2);

	/* Not without the revision proven. */
	g_assert_false(axis_conversion_runs_replace(fx->runs, run, TRUE, -1, "carol", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	/* Nor without the box. */
	g_assert_false(axis_conversion_runs_replace(fx->runs, run, FALSE, -1, "carol", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);

	/* A person proves it by hand from the run's trials. */
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_no_error(error);
	g_assert_true(proving->allowed);
	for (i = 0; i < proving->trials->len; i++)
		g_array_append_val(trials, ((AxisConversionRecord *)g_ptr_array_index(proving->trials, i))->id);
	g_assert_true(axis_conversions_prove(fx->conv, fx->space_saver, (const gint64 *)trials->data, trials->len, "carol",
		&error));
	g_assert_no_error(error);

	/* The same run, replacing: the trial files are asked for again. */
	g_assert_true(axis_conversion_runs_replace(fx->runs, run, TRUE, -1, "carol", &error));
	g_assert_no_error(error);
	assert_state(fx, run, "running");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'waiting' AND trial_id IS NOT NULL", run), ==, 2);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || requested_by FROM conversion WHERE id = %" G_GINT64_FORMAT,
		asked_conversion(fx, run, "Copper Road (2002).mkv")), ==, "replace carol");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || confirmed_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		==, "replace carol");
}

/*
 * Spec (conversion-runs): A trial rejected by a check of the converted file
 * pauses a run of trials: the revision cannot be proven by hand any more.
 */
static void
test_bad_trial(Fixture *fx, gconstpointer data)
{
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	end_asked(fx, run, "rejected", 4);
	step(fx);
	assert_state(fx, run, "paused/bad_trial");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"was rejected by check 4, \"The converted picture has the planned properties\""));
}

/*
 * Spec (conversion-runs): Caps keep a series whole; a time budget.  A cap of
 * files or of saving ends the list after the unit in which it is reached;
 * the run finishes with what is within it.  Its time budget used up, the
 * run asks for nothing more and finishes when nothing is in flight.
 */
static void
test_caps(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionRunInfo) info = NULL;
	AxisConversionRunLimits limits;
	gint64 run;

	episode(fx, "Quiet Meadow", 1, 1, 30);
	episode(fx, "Quiet Meadow", 1, 2, 30);
	episode(fx, "Quiet Meadow", 1, 3, 30);
	episode(fx, "Harbour Lights", 1, 1, 10);
	put_video(fx, fx->tv, "Pilot.mkv", "h264", 12, FALSE, -1, -1);
	settle(fx);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);
	identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);

	/* Two files: the first series is kept whole (three). */
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 1;
	limits.cap_files = 2;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpint(N(fx, "SELECT cap_unit FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 1);
	if (!axis_conversion_runs_get(fx->runs, run, &info, &error))
		g_error("the run's summary: %s", error->message);
	g_assert_cmpint(info->cap_files_in, ==, 3);
	g_assert_cmpint(info->cap_units_in, ==, 1);
	g_clear_pointer(&info, axis_conversion_run_info_free);
	/* A saving just over the first series': the second is in too. */
	limits.cap_files = 0;
	limits.cap_bytes = N(fx, "SELECT bytes - mid + 1 FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT
		" AND position = 1", run);
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_cmpint(N(fx, "SELECT cap_unit FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 2);

	start(fx, run, AXIS_CONVERSION_TRIAL);
	while (N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'waiting'"
	       " AND unit_position <= 2", run) > 0) {
		g_assert_cmpuint(step(fx).asked, ==, 1);
		end_asked(fx, run, "done", 0);
	}
	step(fx);
	assert_state(fx, run, "finished");
	g_assert_cmpstr(Q(fx, "SELECT finish_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"Every file within its caps is done or was skipped.");
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND unit_position = 3",
		run), ==, "waiting");

	/* A time budget of an hour, an hour and a minute later. */
	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 1;
	limits.cap_seconds = 3600;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	axis_conversion_runs_set_now(fx->runs, g_get_real_time() + G_GINT64_CONSTANT(3660) * G_USEC_PER_SEC);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+budget");
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");
	g_assert_cmpstr(Q(fx, "SELECT finish_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"Its time budget of 1 hour is used up.");
}

/* Spec (conversion-runs): Hours.  Outside its hours the run waits. */
static void
test_window(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunLimits limits;
	gint64 now = g_get_real_time();
	g_autoptr(GDateTime) local = g_date_time_new_from_unix_local(now / G_USEC_PER_SEC);
	gint minute = g_date_time_get_hour(local) * 60 + g_date_time_get_minute(local);
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	axis_conversion_runs_set_now(fx->runs, now);
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 1;
	/* Two hours from now until three hours from now (past midnight as it
	 * may be). */
	limits.window_start = (minute + 120) % 1440;
	limits.window_end = (minute + 180) % 1440;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_no_error(error);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "running+window");
	/* From an hour ago until in an hour. */
	limits.window_start = (minute + 1380) % 1440;
	limits.window_end = (minute + 60) % 1440;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
}

/*
 * Spec (conversion-runs): Stopping is safe.  A person pauses: nothing more
 * is asked for, what is in flight finishes.  Cancelling gives back what
 * waits for a worker (an ordinary cancelled conversion) and leaves what is
 * being checked to finish; nothing is half placed.
 */
static void
test_pause_and_cancel(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunLimits limits;
	gint64 first;
	gint64 second;
	gint64 run;

	four_movies(fx);
	sql(fx, "UPDATE worker SET max_jobs = 4 WHERE id = %" G_GINT64_FORMAT, fx->worker);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 2;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 2);

	g_assert_true(axis_conversion_runs_pause(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	assert_state(fx, run, "paused/person");
	first = asked_conversion(fx, run, "Copper Road (2002).mkv");
	second = asked_conversion(fx, run, "Glass Orchard (2003).mkv");
	end_conversion(fx, first, "done", 0, 0.4);
	g_assert_cmpuint(step(fx).asked, ==, 0);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE conversion_id = %" G_GINT64_FORMAT, first), ==,
		"done");
	/* A second pause is not one. */
	g_assert_false(axis_conversion_runs_pause(fx->runs, run, "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_cmpuint(step(fx).asked, ==, 1);
	first = asked_conversion(fx, run, "Lantern Hill (2001).mkv");

	/* One being checked, one waiting for a worker: cancel. */
	sql(fx, "UPDATE conversion SET state = 'verifying' WHERE id = %" G_GINT64_FORMAT, second);
	g_assert_true(axis_conversion_runs_cancel(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	assert_state(fx, run, "cancelled");
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, first), ==, "cancelled");
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, second), ==, "verifying");
	g_assert_cmpstr(Q(fx, "SELECT j.state::text FROM job j JOIN conversion c ON c.job_id = j.id WHERE c.id = %"
		G_GINT64_FORMAT, first), ==, "cancelled");
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state || ': ' || reason_text FROM conversion_run_entry WHERE conversion_id = %"
		G_GINT64_FORMAT, first), ==, "skipped: Cancelled with the run.");
	/* The one being checked finishes, and is recorded. */
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE conversion_id = %" G_GINT64_FORMAT, second), ==,
		"asked");
	end_conversion(fx, second, "done", 0, 0.4);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE conversion_id = %" G_GINT64_FORMAT, second), ==,
		"done");
	g_assert_false(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
}

/*
 * Spec (conversion-runs): A file's own matter is skipped.  A file not
 * eligible any more, changed since the list, or gone from the index when
 * the run reaches it is refused by the gates of the request: it is skipped
 * with the gate's words and the run goes on with the next.  Taking files
 * out of the list; one run at a time.
 */
static void
test_skips(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunStepReport report;
	gint64 run;
	gint64 other;

	movie(fx, "Lantern Hill (2001)", 14);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	movie(fx, "Paper Tide (2004)", 9);
	movie(fx, "Amber Coast (2005)", 11);
	settle(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	/* Not eligible any more (its probe says HDR uncertain), remembered,
	 * gone from the index; and one taken out by a person. */
	sql(fx, "UPDATE media_info SET hdr_uncertain = true WHERE file_id = %" G_GINT64_FORMAT,
		file_of(fx, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv"));
	sql(fx, "INSERT INTO conversion_memory (file_id, revision_id, reason) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", 'not_worth_it')", file_of(fx, fx->movies, "Glass Orchard (2003)/Glass Orchard (2003).mkv"), fx->space_saver);
	g_assert_true(axis_conversion_runs_remove_unit(fx->runs, run, 3, "alice", &error));
	g_assert_no_error(error);
	sql(fx, "DELETE FROM library_file WHERE id = %" G_GINT64_FORMAT,
		file_of(fx, fx->movies, "Amber Coast (2005)/Amber Coast (2005).mkv"));
	start(fx, run, AXIS_CONVERSION_TRIAL);
	report = step(fx);
	g_assert_cmpuint(report.skipped, ==, 3);
	g_assert_cmpuint(report.asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Paper Tide (2004).mkv");
	g_assert_cmpstr(Q(fx, "SELECT string_agg(reason_code, ',' ORDER BY position) FROM conversion_run_entry"
		" WHERE run_id = %" G_GINT64_FORMAT " AND state IN ('skipped', 'removed')", run), ==,
		"gate_eligible,gate_memory,removed,gone");
	g_assert_nonnull(strstr(Q(fx, "SELECT reason_text FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND reason_code = 'gate_eligible'", run), "The file is eligible for the profile now"));
	g_assert_cmpint(N(fx, "SELECT refused_in_row FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 0);

	/* Another run cannot start while this one runs. */
	other = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	g_assert_false(axis_conversion_runs_start(fx->runs, other, AXIS_CONVERSION_TRIAL, FALSE, "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	/* A file asked for cannot be taken out. */
	g_assert_false(axis_conversion_runs_remove_entry(fx->runs, run, N(fx, "SELECT id FROM conversion_run_entry WHERE"
		" run_id = %" G_GINT64_FORMAT " AND state = 'asked'", run), "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
}

/*
 * Spec (conversion-runs): Refusals in a row pause the run.  A condition
 * every file shares (here, every file made not eligible) does not mark the
 * whole list skipped: after AXIS_CONVERSION_RUN_REFUSALS refusals in a row
 * the run pauses.
 */
static void
test_refusals_pause(Fixture *fx, gconstpointer data)
{
	gint64 run;
	guint i;

	for (i = 0; i < AXIS_CONVERSION_RUN_REFUSALS + 3; i++) {
		g_autofree gchar *title = g_strdup_printf("Shelf Film %02u (2001)", i);

		movie(fx, title, 10 + i);
	}
	settle(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	sql(fx, "UPDATE media_info SET hdr_uncertain = true");
	start(fx, run, AXIS_CONVERSION_TRIAL);
	step(fx);
	assert_state(fx, run, "paused/refusals");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'skipped'", run), ==, AXIS_CONVERSION_RUN_REFUSALS);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'waiting'", run), ==, 3);
}

/*
 * Spec (conversion-runs): Real savings beside the estimates.  The files
 * done record what they came to; the run's summary puts it beside the
 * estimate; per revision the ratio is kept (conversion_estimate_seen) and
 * nothing of it is read back into the estimator.
 */
static void
test_estimates_and_reality(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionRunInfo) info = NULL;
	g_autoptr(GPtrArray) recent = NULL;
	gint64 samples;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	samples = N(fx, "SELECT count(*) FROM estimate_sample");
	step(fx);
	end_asked(fx, run, "done", 0);
	step(fx);
	end_asked(fx, run, "done", 0);
	step(fx);
	if (!axis_conversion_runs_get(fx->runs, run, &info, &error))
		g_error("the run's summary: %s", error->message);
	g_assert_no_error(error);
	g_assert_cmpint(info->done, ==, 2);
	g_assert_cmpint(info->done_after, ==, N(fx, "SELECT sum(output_size) FROM conversion WHERE run_id = %"
		G_GINT64_FORMAT " AND state = 'done'", run));
	g_assert_cmpint(info->done_before, ==, N(fx, "SELECT sum(src_size) FROM conversion WHERE run_id = %"
		G_GINT64_FORMAT " AND state = 'done'", run));
	g_assert_cmpint(info->done_estimate, ==, N(fx, "SELECT sum(mid) FROM conversion_run_entry WHERE run_id = %"
		G_GINT64_FORMAT " AND state = 'done'", run));
	g_assert_cmpint(info->seen_files, ==, 2);
	g_assert_cmpint(info->seen_actual, ==, info->done_after);
	g_assert_cmpint(info->seen_estimated, ==, N(fx, "SELECT sum(estimate_bytes) FROM conversion WHERE run_id = %"
		G_GINT64_FORMAT " AND state = 'done'", run));
	/* Nothing went into the estimator's calibration from the run. */
	g_assert_cmpint(N(fx, "SELECT count(*) FROM estimate_sample"), ==, samples);
	g_assert_true(axis_conversion_runs_recent(fx->runs, run, 10, &recent, &error));
	g_assert_cmpuint(recent->len, ==, 2);
	g_assert_cmpint(((AxisConversionRunEntry *)g_ptr_array_index(recent, 0))->result_size, >, 0);
}

/* A list job that ended without a list leaves no run being listed for ever. */
static void
test_lost_list(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 run = 0;

	g_assert_true(axis_conversion_runs_create(fx->runs, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver, "alice",
		&run, &error));
	sql(fx, "UPDATE job SET state = 'failed', finished_at = now(), last_error = 'stopped in a test' WHERE id ="
		" (SELECT list_job_id FROM conversion_run WHERE id = %" G_GINT64_FORMAT ")", run);
	sql(fx, "UPDATE conversion_run SET created_at = now() - interval '1 minute' WHERE id = %" G_GINT64_FORMAT, run);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "failed");
	g_assert_nonnull(strstr(Q(fx, "SELECT list_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"stopped in a test"));
}

/* A movie of @mbits Mbit/s on another disk's movies root (@disk, @root), for
 * settle() to probe once that root was scanned. */
static void
movie_on(
	Fixture     *fx,
	const gchar *disk,
	gint64       root,
	const gchar *title,
	gdouble      mbits
){
	Pending *p = g_new0(Pending, 1);

	p->root = root;
	p->rel = g_strdup_printf("%s/%s.mkv", title, title);
	p->season = -1;
	p->episode = -1;
	p->streams[0].kind = "video";
	p->streams[0].codec = "h264";
	p->streams[0].bitrate = (gint64)(mbits * 1000000);
	p->streams[0].width = 1920;
	p->streams[0].height = 1080;
	p->streams[0].fps = 24.0;
	p->streams[0].hdr = "sdr";
	p->streams[0].field_order = "progressive";
	p->streams[1].kind = "audio";
	p->streams[1].codec = "aac";
	p->streams[1].bitrate = 128000;
	p->streams[1].channels = 2;
	p->streams[1].sample_rate = 48000;
	p->streams[1].lossless = "false";
	p->streams[1].language = "eng";
	axis_test_scenario_put(disk, "Movies", p->rel, axis_test_savings_size(p->streams, 1.0));
	g_ptr_array_add(fx->pending, p);
}

/*
 * Spec (conversion-runs, A file in flight that no worker can take; hunt
 * finding H4): a run with one file at a time asked for a file of disk one;
 * then the only worker stops proving disk one (its mount hung and it
 * registered again without it).  The run gives that conversion back (no
 * worker took it: nothing was made of it), the file waits in the list
 * again, and the run goes on with disk two's file, naming disk one as the
 * volume whose files wait and why; once the worker proves disk one again,
 * the run comes back to it.
 */
static void
test_in_flight_volume_without_worker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk2 = NULL;
	g_autofree gchar *state = NULL;
	gint64 volume2;
	gint64 movies2;
	gint64 copper;
	gint64 run;

	/* Two "disks" are directories of one filesystem, which storage only lets
	 * be two volumes while the first is away. */
	{
		g_autoptr(GError) error = NULL;

		axis_test_library_unplug(fx->t, fx->disk, "away");
		g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
		disk2 = axis_test_library_disk(fx->t, "disk2");
		volume2 = axis_test_library_add_volume_beside(fx->t, disk2, "Disk two");
		axis_test_library_plug(fx->t, fx->disk, "away");
		g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume, NULL, &error));
		g_assert_no_error(error);
	}
	movies2 = axis_test_library_add_root(fx->t, volume2, disk2, "Movies", AXIS_MEDIA_MOVIES);
	sql(fx, "INSERT INTO worker_volume (worker_id, volume_id, path) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", '/media/disk2')", fx->worker, volume2);
	movie(fx, "Copper Road (2002)", 24);
	movie_on(fx, disk2, movies2, "Glass Orchard (2003)", 18);
	movie_on(fx, disk2, movies2, "Paper Tide (2004)", 9);
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, movies2, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	settle(fx);

	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");

	copper = asked_conversion(fx, run, "Copper Road (2002).mkv");

	/* The worker no longer proves disk one; it is still connected, with disk
	 * two proved. */
	sql(fx, "DELETE FROM worker_volume WHERE worker_id = %" G_GINT64_FORMAT " AND volume_id = %" G_GINT64_FORMAT,
		fx->worker, fx->volume);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	state = run_state(fx, run);
	g_assert_cmpstr(state, ==, "running");
	g_assert_cmpstr(asked_names(fx, run), ==, "Glass Orchard (2003).mkv");
	/* Given back, not skipped: it waits in the list. */
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, copper), ==, "cancelled");
	g_assert_nonnull(strstr(Q(fx, "SELECT result_text FROM conversion WHERE id = %" G_GINT64_FORMAT, copper),
		"No connected worker can reach Disk one"));
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND rel_path LIKE '%%Copper Road (2002).mkv'", run), ==, "waiting");
	g_assert_cmpstr(Q(fx, "SELECT code FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT " AND volume_id = %"
		G_GINT64_FORMAT, run, fx->volume), ==, "no_worker");
	g_assert_nonnull(strstr(Q(fx, "SELECT text FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT, run),
		"Disk one"));
	/* The cancelled conversion is never taken for the entry's again. */
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND rel_path LIKE '%%Copper Road (2002).mkv'", run), ==, "waiting");

	/* Disk two's file done, and disk one proved again: back to it. */
	end_asked(fx, run, "done", 0);
	sql(fx, "INSERT INTO worker_volume (worker_id, volume_id, path) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", '/media/disk1')", fx->worker, fx->volume);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	g_assert_cmpint(asked_conversion(fx, run, "Copper Road (2002).mkv"), >, copper);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_wait WHERE run_id = %" G_GINT64_FORMAT, run), ==, 0);
}

/*
 * Spec (conversion-runs, A file not judged is asked for again; hunt finding
 * H5): a conversion of a run that ended not judged (the plan could not be
 * made again as the worker was given it) is not a failure in a row: the file
 * waits in the list and is asked for again.
 */
static void
test_not_judged_asked_again(Fixture *fx, gconstpointer data)
{
	gint64 first;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	first = asked_conversion(fx, run, "Copper Road (2002).mkv");
	sql(fx, "UPDATE conversion SET state = 'failed', retryable = true, finished_at = now(), result_text = 'Not judged:"
		" in this test.' WHERE id = %" G_GINT64_FORMAT, first);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	g_assert_cmpint(asked_conversion(fx, run, "Copper Road (2002).mkv"), >, first);
	g_assert_cmpint(N(fx, "SELECT failed_in_row FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 0);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND state = 'failed'", run), ==, 0);
}

/*
 * Spec (conversion-runs, One file at a time, through the single request): a
 * request whose conversion committed but whose entry was not linked yet (the
 * step stopped in between) is linked to that conversion at the next step.
 * After a run moved from trials to replacing, the entry has two conversions
 * (its trial, done, and the replacement asked for now): the link is the
 * replacement's, the one asked for since the entry was marked.
 */
static void
test_link_after_stop(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *gate = NULL;
	gint64 file;
	gint64 entry;
	gint64 trial;
	gint64 replacement = 0;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	entry = N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND state = 'asked'", run);
	trial = N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT, entry);
	file = N(fx, "SELECT file_id FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT, entry);
	end_conversion(fx, trial, "done", 0, 0.4);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT, entry), ==, "done");

	/* Proven; the run replaces from now on, with the same list. */
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	g_assert_true(axis_conversion_runs_replace(fx->runs, run, TRUE, -1, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT, entry), ==, "waiting");

	/* The step marks the entry asked and its request commits; the step stops
	 * before it links the entry. */
	sql(fx, "UPDATE conversion_run_entry SET state = 'asked', outcome = 'replace', asked_at = now() WHERE id = %"
		G_GINT64_FORMAT, entry);
	g_assert_true(axis_conversions_request_for_run(fx->conv, file, fx->space_saver, AXIS_CONVERSION_REPLACE, TRUE,
		NULL, "alice", run, entry, &replacement, &gate, &error));
	g_assert_no_error(error);
	g_assert_cmpint(replacement, >, trial);

	/* The next step links the entry to the replacement, which is in flight. */
	step(fx);
	g_assert_cmpint(N(fx, "SELECT coalesce(conversion_id, 0) FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT,
		entry), ==, replacement);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE id = %" G_GINT64_FORMAT, entry), ==, "asked");
}

/*
 * Spec (conversion-execution, A rejected trial set aside; conversion-runs, A
 * run paused for a bad trial goes on once nothing counts; hunt finding H6): a
 * run of trials paused because a trial was rejected by a check of the
 * converted file cannot be resumed while that trial counts against its
 * revision; an administrator sets it aside, saying why (audited), and the run
 * resumes and asks for its next file.  (Rejections cleared by a mark:
 * /conversion-runs/pause/marks.)
 */
static void
test_bad_trial_set_aside(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	gint64 copper;
	gint64 glass;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	copper = asked_conversion(fx, run, "Copper Road (2002).mkv");
	/* Rejected by check 4, as a build that judged before versions were
	 * recorded did. */
	end_conversion(fx, copper, "rejected", 4, 0.4);
	step(fx);
	assert_state(fx, run, "paused/bad_trial");
	g_assert_nonnull(strstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"set the trial aside on its page, then resume this run"));

	/* It still counts: resuming is refused, saying why. */
	g_assert_false(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "1 trial of this version still counts against proving it"));
	g_clear_error(&error);
	assert_state(fx, run, "paused/bad_trial");

	/* What cannot be set aside. */
	g_assert_false(axis_conversions_set_aside(fx->conv, copper, "alice", "  ", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(axis_conversions_set_aside(fx->conv, copper, NULL, "check 4 was wrong", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);

	/* Set aside by a person, with the reason: counted neither way. */
	g_assert_true(axis_conversions_set_aside(fx->conv, copper, "alice", "check 4 was wrong for untagged sources; fixed",
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: alice set trial*aside*check 4 was wrong for "
		"untagged sources; fixed*"), ==, 1);
	g_assert_false(axis_conversions_set_aside(fx->conv, copper, "alice", "again", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(proving->bad_trials, ==, 0);
	g_assert_cmpuint(proving->set_aside, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);

	/* The run goes on with its next file. */
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
	g_assert_cmpstr(asked_names(fx, run), ==, "Glass Orchard (2003).mkv");

	/* Rejected by check 5 by this build's rules: it counts, and the run
	 * pauses. */
	glass = asked_conversion(fx, run, "Glass Orchard (2003).mkv");
	end_conversion(fx, glass, "rejected", 5, 0.4);
	sql(fx, "UPDATE conversion SET checks_version = %d WHERE id = %" G_GINT64_FORMAT, AXIS_CONVERSION_CHECKS_VERSION,
		glass);
	step(fx);
	assert_state(fx, run, "paused/bad_trial");
}

/*
 * Spec (conversion-execution, Rejections and passes by a rule found wrong
 * since; review of fix-conversion-hunt-findings, item 3): the marks.  A
 * version recorded later than the rules of now changes nothing by itself.  A
 * mark that check 5 was wrongly strict before version 2 clears the
 * rejections by check 5 judged before it (one recorded before versions were
 * counts as earlier than any), and only those: one judged by version 2, or by
 * another check, still counts.  A mark that a check was too lenient before
 * version 2 takes the passes judged before it out of proving; a pass judged
 * by version 2 still counts.
 */
static void
test_marks(Fixture *fx, gconstpointer data)
{
	static const AxisConversionMark strict[] = {
		{ 5, AXIS_CONVERSION_MARK_STRICT, 2, "it refused files whose last frame was short" }
	};
	static const AxisConversionMark lenient[] = {
		{ 6, AXIS_CONVERSION_MARK_LENIENT, 2, "it did not compare the last sound track" }
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	gint64 copper;
	gint64 glass;
	gint64 run;

	four_movies(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	copper = asked_conversion(fx, run, "Copper Road (2002).mkv");
	end_conversion(fx, copper, "rejected", 5, 0.4);
	sql(fx, "UPDATE conversion SET checks_version = 2 WHERE id = %" G_GINT64_FORMAT, copper);
	step(fx);
	assert_state(fx, run, "paused/bad_trial");

	/* Wrongly strict before version 2: judged by version 2, it counts. */
	axis_conversion_marks_set_for_test(strict, G_N_ELEMENTS(strict));
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(proving->bad_trials, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	g_assert_false(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_clear_error(&error);
	/* Judged by version 1, before the mark: it counts no more, and the run
	 * goes on. */
	sql(fx, "UPDATE conversion SET checks_version = 1 WHERE id = %" G_GINT64_FORMAT, copper);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->bad_trials, ==, 0);
	g_assert_cmpuint(proving->set_aside, ==, 0);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	assert_state(fx, run, "running");
	/* Recorded before versions: earlier than any, cleared too. */
	sql(fx, "UPDATE conversion SET checks_version = NULL WHERE id = %" G_GINT64_FORMAT, copper);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->bad_trials, ==, 0);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	/* A rejection by another check is not cleared by check 5's mark. */
	sql(fx, "UPDATE conversion SET failed_check = 4 WHERE id = %" G_GINT64_FORMAT, copper);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->bad_trials, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	/* Without the mark, the build's rules: it counts. */
	axis_conversion_marks_set_for_test(NULL, 0);
	sql(fx, "UPDATE conversion SET failed_check = 5 WHERE id = %" G_GINT64_FORMAT, copper);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->bad_trials, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	sql(fx, "UPDATE conversion SET state = 'failed', failed_check = NULL WHERE id = %" G_GINT64_FORMAT, copper);

	/* Passes: too lenient before version 2.  One judged before (here
	 * recorded before versions) is no trial towards proving; one judged by
	 * version 2 is. */
	glass = asked_conversion(fx, run, "Glass Orchard (2003).mkv");
	end_conversion(fx, glass, "done", 0, 0.4);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->trials->len, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	axis_conversion_marks_set_for_test(lenient, G_N_ELEMENTS(lenient));
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->trials->len, ==, 0);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	sql(fx, "UPDATE conversion SET checks_version = 2 WHERE id = %" G_GINT64_FORMAT, glass);
	g_assert_true(axis_conversions_proving(fx->conv, fx->space_saver, &proving, &error));
	g_assert_cmpuint(proving->trials->len, ==, 1);
	g_clear_pointer(&proving, axis_conversion_proving_free);
	axis_conversion_marks_set_for_test(NULL, 0);
}

/* ── Kept files, by choice (convert-kept-files) ─────────────────── */

/* Marks @file kept: by hand, as a movie's file, or brought over, as an
 * episode's (of a made-up movie or series of its own). */
static void
keep_file(
	Fixture  *fx,
	gint64    file,
	gboolean  brought
){
	sql(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('%s', 'Kept %" G_GINT64_FORMAT
		"', 2001, 'fixture', 'kept-%" G_GINT64_FORMAT "')", brought ? "series" : "film", file, file);
	if (brought) {
		sql(fx, "INSERT INTO series (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
			" FROM meta_title t WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file);
		sql(fx, "INSERT INTO series_file (file_id, series_id, quality, kept, kept_how) SELECT %" G_GINT64_FORMAT
			", s.id, 'webdl_1080p', true, 'brought' FROM series s JOIN meta_title t ON t.id = s.title_id"
			" WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file, file);
	} else {
		sql(fx, "INSERT INTO movie (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
			" FROM meta_title t WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file);
		sql(fx, "INSERT INTO movie_file (file_id, movie_id, quality, kept) SELECT %" G_GINT64_FORMAT
			", m.id, 'webdl_1080p', true FROM movie m JOIN meta_title t ON t.id = m.title_id"
			" WHERE t.provider_id = 'kept-%" G_GINT64_FORMAT "'", file, file);
	}
}

/* How many links of files are kept: the mark, as everything else reads it. */
static gint64
kept_marks(Fixture *fx)
{
	return N(fx, "SELECT (SELECT count(*) FROM movie_file WHERE kept) + (SELECT count(*) FROM series_file WHERE kept)");
}

/* The words every reason with files of @run, "reason:files", by reason. */
static const gchar *
reasons_of(
	Fixture *fx,
	gint64   run
){
	return Q(fx, "SELECT coalesce(string_agg(reason || ':' || files, ',' ORDER BY reason), '') FROM conversion_run_reason"
		" WHERE run_id = %" G_GINT64_FORMAT, run);
}

/* A run made with the choice to convert kept files (by "alice"), listed. */
static gint64
make_run_kept(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_conversion_runs_create_full(fx->runs, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver,
		TRUE, "alice", &id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_conversion_runs_build_list(fx->runs, id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run WHERE id = %" G_GINT64_FORMAT, id), ==, "ready");
	return id;
}

/* Lantern Hill kept by hand, Copper Road brought over, Glass Orchard not
 * kept; and two kept files that have another reason of their own to be left
 * out: Quiet Field (efficient already) and Faint Signal (HDR uncertain). */
static void
kept_library(Fixture *fx)
{
	movie(fx, "Lantern Hill (2001)", 12);
	movie(fx, "Copper Road (2002)", 24);
	movie(fx, "Glass Orchard (2003)", 18);
	put_video(fx, fx->movies, "Quiet Field (2005)/Quiet Field (2005).mkv", "hevc", 1.5, FALSE, -1, -1);
	put_video(fx, fx->movies, "Faint Signal (2006)/Faint Signal (2006).mkv", "h264", 12, TRUE, -1, -1);
	settle(fx);
	keep_file(fx, file_of(fx, fx->movies, "Lantern Hill (2001)/Lantern Hill (2001).mkv"), FALSE);
	keep_file(fx, file_of(fx, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv"), TRUE);
	keep_file(fx, file_of(fx, fx->movies, "Quiet Field (2005)/Quiet Field (2005).mkv"), FALSE);
	keep_file(fx, file_of(fx, fx->movies, "Faint Signal (2006)/Faint Signal (2006).mkv"), TRUE);
	g_assert_cmpint(kept_marks(fx), ==, 4);
}

/* "dora" adds the kept files to @run (@confirmed: she ticked that their
 * originals are replaced), and the list job does its work, as the job
 * would; the run is then @expected_state. */
static void
add_kept_confirmed(
	Fixture     *fx,
	gint64       run,
	gboolean     confirmed,
	const gchar *expected_state
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_conversion_runs_include_kept(fx->runs, run, confirmed, "dora", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text || ' ' || convert_kept_by FROM conversion_run"
		" WHERE id = %" G_GINT64_FORMAT, run), ==, "listing true dora");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM job j JOIN conversion_run r ON r.list_job_id = j.id WHERE r.id = %"
		G_GINT64_FORMAT " AND j.kind = 'convert.run.list' AND j.state = 'queued'", run), ==, 1);
	g_assert_true(axis_conversion_runs_build_list(fx->runs, run, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state || coalesce(' ' || relist_state, '') FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, run), ==, expected_state);
}

/* The same for a run of trials, or one not started: no tick. */
static void
add_kept(
	Fixture     *fx,
	gint64       run,
	const gchar *expected_state
){
	add_kept_confirmed(fx, run, FALSE, expected_state);
}

/*
 * Spec (conversion-runs, Kept files are converted only by choice): off by
 * default, a run leaves every kept file out as today.  With the choice the
 * kept files are listed like any other; a kept file that has another reason
 * of its own stays out, under that reason; the request of every file the
 * choice covers (kept when listed) carries it, so the gate "kept" holds for
 * it, and a file that is not kept is asked for without it; and the mark
 * itself is not changed by listing or asking.  (The claim, check 10 and the
 * placement with the choice: /conversion-runs/kept/claim-check-place.)
 */
static void
test_kept_by_choice(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionRunInfo) info = NULL;
	g_autofree gchar *order = NULL;
	g_autofree gchar *gate = NULL;
	gint64 plain;
	gint64 chosen;
	gint64 copper;
	gint64 conversion;
	gint64 refused = 0;

	kept_library(fx);
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	copper = file_of(fx, fx->movies, "Copper Road (2002)/Copper Road (2002).mkv");

	/* Off by default: exactly as before. */
	plain = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	order = entry_order(fx, plain);
	g_assert_cmpstr(order, ==, "Glass Orchard (2003).mkv");
	g_assert_cmpstr(reasons_of(fx, plain), ==, "kept:4");
	g_clear_pointer(&order, g_free);

	/* Chosen: the kept files that would be converted are listed, by saving;
	 * the two with a reason of their own stay out, under it. */
	chosen = make_run_kept(fx);
	order = entry_order(fx, chosen);
	g_assert_cmpstr(order, ==, "Copper Road (2002).mkv,Glass Orchard (2003).mkv,Lantern Hill (2001).mkv");
	g_assert_cmpint(reason_count(fx, chosen, "kept"), ==, 0);
	g_assert_cmpint(reason_count(fx, chosen, "hdr_uncertain"), ==, 1);
	g_assert_cmpint(reason_count(fx, chosen, "already_efficient") + reason_count(fx, chosen, "better_codec"), ==, 1);
	g_assert_cmpint(N(fx, "SELECT scope_files FROM conversion_run WHERE id = %" G_GINT64_FORMAT, chosen), ==, 5);
	g_assert_true(axis_conversion_runs_get(fx->runs, chosen, &info, &error));
	g_assert_no_error(error);
	g_assert_true(info->convert_kept);
	g_assert_cmpstr(info->convert_kept_by, ==, "alice");
	g_assert_cmpint(info->convert_kept_us, >, 0);
	g_assert_cmpint(kept_marks(fx), ==, 4);
	/* The entries the choice covers: the two kept files, not Glass Orchard. */
	g_assert_cmpstr(Q(fx, "SELECT string_agg(regexp_replace(rel_path, '^.*/', '') || '=' || convert_kept::text, ','"
		" ORDER BY position) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, chosen), ==,
		"Copper Road (2002).mkv=true,Glass Orchard (2003).mkv=false,Lantern Hill (2001).mkv=true");

	/* Without the choice the request of a kept file is refused by its gate,
	 * for a run as for a person. */
	g_assert_false(axis_conversions_request_for_run(fx->conv, copper, fx->space_saver, AXIS_CONVERSION_REPLACE, TRUE,
		NULL, "alice", chosen, N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, chosen, copper), &refused, &gate, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_assert_cmpstr(gate, ==, "kept");
	g_assert_nonnull(strstr(error->message, "Kept: Axis does not replace this file."));
	g_clear_error(&error);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion"), ==, 0);

	/* The run asks, replacing: its requests carry the choice. */
	start(fx, chosen, AXIS_CONVERSION_REPLACE);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, chosen), ==, "Copper Road (2002).mkv");
	conversion = asked_conversion(fx, chosen, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || state || ' ' || convert_kept::text || ' ' || requested_by"
		" FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==, "replace queued true alice");
	end_conversion(fx, conversion, "done", 0, 0.4);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion_run_entry WHERE conversion_id = %" G_GINT64_FORMAT, conversion),
		==, "done");
	/* A file that is not kept is asked for without the choice: it does not
	 * cover it, so a mark set on it later refuses it as without the choice. */
	g_assert_cmpstr(asked_names(fx, chosen), ==, "Glass Orchard (2003).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || requested_by FROM conversion"
		" WHERE id = %" G_GINT64_FORMAT, asked_conversion(fx, chosen, "Glass Orchard (2003).mkv")), ==,
		"replace false alice");
	/* Nothing of it changed a mark. */
	g_assert_cmpint(kept_marks(fx), ==, 4);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND kept"
		" AND kept_how = 'brought'", copper), ==, 1);
}

/*
 * Spec (conversion-runs, The choice stays with the run): a run of trials made
 * with the choice keeps it when it is switched to replacing originals: the
 * kept file it tried is asked for again as a replacement, with the choice.
 */
static void
test_kept_trials_then_replace(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 run;
	gint64 trial;
	gint64 replacement;

	kept_library(fx);
	run = make_run_kept(fx);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	trial = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT,
		trial), ==, "trial true");
	end_conversion(fx, trial, "done", 0, 0.4);
	step(fx);
	g_assert_true(axis_conversion_runs_pause(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	/* What is in flight finishes before the switch. */
	end_asked(fx, run, "done", 0);
	step(fx);

	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	g_assert_true(axis_conversion_runs_replace(fx->runs, run, TRUE, -1, "carol", &error));
	g_assert_no_error(error);
	/* The choice, and who made it, are as they were: not the switch's. */
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || convert_kept_by || ' ' || confirmed_by"
		" FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "replace true alice carol");
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "carol", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	replacement = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpint(replacement, >, trial);
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || requested_by FROM conversion WHERE id = %"
		G_GINT64_FORMAT, replacement), ==, "replace true carol");
	g_assert_cmpint(kept_marks(fx), ==, 4);
}

/*
 * Spec (conversion-runs, Kept files added to a run): "Add these N files to
 * the run" turns the choice on for a run that is ready and adds the files it
 * left out as kept to its list: each is listed after what was there (ranked
 * among themselves), or left out for its other reason; none stays left out as
 * kept, and what was listed is untouched.  It is refused for a run that has
 * the choice, that is running, cancelled, being listed, or has a file in
 * flight; and a job that ends without adding them leaves the run as it was.
 */
static void
test_kept_added_to_ready(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	gint64 run;
	gint64 glass_entry;
	gint64 other;

	kept_library(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	glass_entry = N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run);
	g_assert_cmpstr(reasons_of(fx, run), ==, "kept:4");

	/* A job that ended without adding them: the run is ready as it was, the
	 * choice off, and it says so. */
	g_assert_true(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || relist_state || ' ' || convert_kept::text || ' ' || convert_kept_by"
		" FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "listing ready true dora");
	/* Being listed: not again, and nothing else of it. */
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	sql(fx, "UPDATE job SET state = 'failed', finished_at = now(), last_error = 'stopped in a test' WHERE id ="
		" (SELECT list_job_id FROM conversion_run WHERE id = %" G_GINT64_FORMAT ")", run);
	sql(fx, "UPDATE conversion_run SET updated_at = now() - interval '1 minute' WHERE id = %" G_GINT64_FORMAT, run);
	step(fx);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || coalesce(relist_state, '-') || ' ' || convert_kept::text || ' ' ||"
		" coalesce(convert_kept_by, '-') FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"ready - false -");
	g_assert_nonnull(strstr(Q(fx, "SELECT list_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"The files Axis keeps were not added, and the run is as it was"));
	g_assert_cmpstr(reasons_of(fx, run), ==, "kept:4");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);

	/* Added. */
	add_kept(fx, run, "ready");
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Glass Orchard (2003).mkv,Copper Road (2002).mkv,Lantern Hill (2001).mkv");
	g_assert_cmpint(N(fx, "SELECT id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND position = 1", run),
		==, glass_entry);
	g_assert_cmpstr(Q(fx, "SELECT string_agg(position::text || ':' || files, ',' ORDER BY position) FROM"
		" conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT, run), ==, "1:1,2:1,3:1");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND (position < 1 OR unit_position < 1 OR state <> 'waiting')", run), ==, 0);
	g_assert_cmpint(reason_count(fx, run, "kept"), ==, 0);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_excluded WHERE run_id = %" G_GINT64_FORMAT
		" AND reason = 'kept'", run), ==, 0);
	g_assert_cmpint(reason_count(fx, run, "hdr_uncertain"), ==, 1);
	g_assert_cmpint(reason_count(fx, run, "already_efficient") + reason_count(fx, run, "better_codec"), ==, 1);
	g_assert_cmpint(N(fx, "SELECT scope_files FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 5);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text || ' ' || convert_kept_by || ' ' || created_by || coalesce(list_text, '')"
		" FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "true dora alice");
	g_assert_cmpint(kept_marks(fx), ==, 4);
	/* It has the choice now: not twice. */
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);

	/* Not for a run that is running, has a file in flight, or was cancelled;
	 * not without a person; not for a run that is not there. */
	other = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, other, FALSE, "", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, other + 1000, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND);
	g_clear_error(&error);
	start(fx, other, AXIS_CONVERSION_TRIAL);
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, other, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_true(axis_conversion_runs_pause(fx->runs, other, "alice", &error));
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, other, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_assert_nonnull(strstr(error->message, "in flight"));
	g_clear_error(&error);
	g_assert_true(axis_conversion_runs_cancel(fx->runs, other, "alice", &error));
	g_assert_no_error(error);
	step(fx);
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, other, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT,
		other), ==, "cancelled false");
	g_assert_cmpstr(reasons_of(fx, other), ==, "kept:4");

	/* Cancelled while the files are being added: the run is cancelled, and
	 * the job that comes afterwards adds nothing. */
	other = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	g_assert_true(axis_conversion_runs_include_kept(fx->runs, other, FALSE, "dora", &error));
	g_assert_no_error(error);
	g_assert_true(axis_conversion_runs_cancel(fx->runs, other, "alice", &error));
	g_assert_no_error(error);
	g_assert_true(axis_conversion_runs_build_list(fx->runs, other, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || coalesce(relist_state, '-') FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, other), ==, "cancelled -");
	g_assert_cmpstr(reasons_of(fx, other), ==, "kept:4");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, other), ==, 1);
	g_assert_cmpint(kept_marks(fx), ==, 4);
}

/*
 * Spec (conversion-runs, Kept files added to a run): a paused run with files
 * done keeps everything it had: the entries done, their conversions, its
 * pause and why; the kept files come after.  A finished run to which files
 * were added waits, paused, for a person to resume it; then it asks for them
 * with the choice.
 */
static void
test_kept_added_later(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	gint64 run;
	gint64 done;
	gint64 conversion;

	kept_library(fx);
	movie(fx, "Paper Tide (2004)", 9);
	settle(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	done = asked_conversion(fx, run, "Glass Orchard (2003).mkv");
	end_conversion(fx, done, "done", 0, 0.4);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_true(axis_conversion_runs_pause(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	/* Paper Tide is in flight still. */
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "false");
	end_asked(fx, run, "done", 0);
	step(fx);

	add_kept(fx, run, "paused");
	assert_state(fx, run, "paused/person");
	g_assert_cmpstr(Q(fx, "SELECT pause_text || ' ' || paused_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		==, "Paused by alice. alice");
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Glass Orchard (2003).mkv,Paper Tide (2004).mkv,Copper Road (2002).mkv,"
		"Lantern Hill (2001).mkv");
	g_assert_cmpstr(Q(fx, "SELECT string_agg(state, ',' ORDER BY position) FROM conversion_run_entry WHERE run_id = %"
		G_GINT64_FORMAT, run), ==, "done,done,waiting,waiting");
	g_assert_cmpint(N(fx, "SELECT conversion_id FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND position = 1", run), ==, done);
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(asked_names(fx, run), ==, "Copper Road (2002).mkv");
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==, "true");
	/* The trial made before the choice does not carry it. */
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT, done), ==, "false");
	end_conversion(fx, conversion, "done", 0, 0.4);
	step(fx);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");
	g_assert_cmpint(kept_marks(fx), ==, 4);

	/* A finished run. */
	{
		gint64 finished;

		sql(fx, "DELETE FROM conversion_memory");
		finished = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
		g_assert_cmpstr(reasons_of(fx, finished), ==, "kept:4");
		start(fx, finished, AXIS_CONVERSION_TRIAL);
		step(fx);
		end_asked(fx, finished, "done", 0);
		step(fx);
		end_asked(fx, finished, "done", 0);
		step(fx);
		assert_state(fx, finished, "finished");
		add_kept(fx, finished, "paused");
		assert_state(fx, finished, "paused/person");
		g_assert_cmpstr(Q(fx, "SELECT pause_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, finished), ==,
			"dora added 2 files Axis keeps to the list of this run, which had finished. Resume the run to convert them "
			"as trials.");
		g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run WHERE id = %" G_GINT64_FORMAT
			" AND finished_at IS NULL AND finish_text IS NULL AND paused_by = 'dora'", finished), ==, 1);
		g_assert_true(axis_conversion_runs_resume(fx->runs, finished, "dora", &error));
		g_assert_no_error(error);
		g_assert_cmpuint(step(fx).asked, ==, 1);
		g_assert_cmpstr(asked_names(fx, finished), ==, "Copper Road (2002).mkv");
	}
}

/*
 * Spec (conversion-runs, Kept files added to a run): an episode that was
 * left out as kept joins its series' unit, after the episodes that were
 * listed; the unit's totals count it; a unit a person took out of the list
 * stays out, its new file with it; a cap is worked out again.
 */
static void
test_kept_added_to_series(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	AxisConversionRunLimits limits;
	gint64 run;

	episode(fx, "Harbour Lights", 1, 1, 10);
	episode(fx, "Harbour Lights", 1, 2, 10);
	episode(fx, "Harbour Lights", 1, 3, 10);
	episode(fx, "Quiet Meadow", 1, 1, 30);
	episode(fx, "Quiet Meadow", 1, 2, 30);
	episode(fx, "Tin Lantern", 1, 1, 20);
	settle(fx);
	identify_series(fx, "Harbour Lights", "Harbour Lights", 2011);
	identify_series(fx, "Quiet Meadow", "Quiet Meadow", 2015);
	identify_series(fx, "Tin Lantern", "Tin Lantern", 2018);
	keep_file(fx, file_of(fx, fx->tv, "Harbour Lights/Season 01/Harbour Lights - S01E02.mkv"), TRUE);
	keep_file(fx, file_of(fx, fx->tv, "Quiet Meadow/Season 01/Quiet Meadow - S01E01.mkv"), TRUE);
	keep_file(fx, file_of(fx, fx->tv, "Tin Lantern/Season 01/Tin Lantern - S01E01.mkv"), TRUE);

	run = make_run(fx, AXIS_CONVERSION_RUN_TV, NULL, 0, 0, fx->space_saver);
	order = unit_order(fx, run);
	g_assert_cmpstr(order, ==, "series:Quiet Meadow,series:Harbour Lights");
	g_clear_pointer(&order, g_free);
	/* Quiet Meadow is taken out by a person; a cap of three files. */
	g_assert_true(axis_conversion_runs_remove_unit(fx->runs, run, 1, "alice", &error));
	g_assert_no_error(error);
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 1;
	limits.cap_files = 3;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_no_error(error);
	g_assert_cmpint(N(fx, "SELECT coalesce(cap_unit, -1) FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, -1);

	add_kept(fx, run, "ready");
	order = unit_order(fx, run);
	g_assert_cmpstr(order, ==, "series:Quiet Meadow,series:Harbour Lights,series:Tin Lantern");
	g_clear_pointer(&order, g_free);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Quiet Meadow - S01E02.mkv,Harbour Lights - S01E01.mkv,Harbour Lights - S01E03.mkv,"
		"Quiet Meadow - S01E01.mkv,Harbour Lights - S01E02.mkv,Tin Lantern - S01E01.mkv");
	g_assert_cmpstr(Q(fx, "SELECT string_agg(unit_position::text || state, ',' ORDER BY position) FROM"
		" conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run), ==,
		"1removed,2waiting,2waiting,1removed,2waiting,3waiting");
	g_assert_cmpstr(Q(fx, "SELECT string_agg(position::text || ':' || files || ':' || removed::text, ',' ORDER BY position)"
		" FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT, run), ==, "1:2:true,2:3:false,3:1:false");
	g_assert_cmpint(N(fx, "SELECT bytes FROM conversion_run_unit WHERE run_id = %" G_GINT64_FORMAT " AND position = 2",
		run), ==, N(fx, "SELECT sum(size) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND unit_position = 2", run));
	/* The cap of three files is reached in Harbour Lights now. */
	g_assert_cmpint(N(fx, "SELECT coalesce(cap_unit, -1) FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, 2);
	g_assert_cmpint(reason_count(fx, run, "kept"), ==, 0);
	g_assert_cmpint(kept_marks(fx), ==, 3);
}

/* ── The claim, check 10 and the placement, with no encoder ──────── */

/* The path of @conversion's output in the disk's work area, by the name its
 * claim gave it. */
static gchar *
output_path(
	Fixture *fx,
	gint64   conversion
){
	g_autofree gchar *name = QA(fx, "SELECT output_name FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion);

	g_assert_nonnull(name);
	return g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, name, NULL);
}

/* The path of @conversion's file in the Movies folder now, by the name the
 * conversion gives it. */
static gchar *
movie_path(
	Fixture *fx,
	gint64   conversion
){
	g_autofree gchar *rel = QA(fx, "SELECT new_rel_path FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion);

	g_assert_nonnull(rel);
	return g_build_filename(fx->disk, "Movies", rel, NULL);
}

/*
 * The worker "garage" takes @conversion: the claim (the gates again, with
 * what the conversion recorded, and the plan for its encoders); then, when
 * the claim gave it the work, it writes an output of @ratio of the
 * original's size to the disk's work area and reports it, and the
 * conversion waits for its checks.  NULL when the claim gave the work, else
 * why it did not (for the caller to free).
 */
static gchar *
claim_and_encode(
	Fixture *fx,
	gint64   conversion,
	gdouble  ratio
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConvertWork) work = NULL;
	g_autofree gchar *machine = axis_worker_machine("garage");
	g_autofree gchar *path = NULL;
	g_autofree gchar *bytes = NULL;
	gchar *why_not = NULL;
	AxisJobClaim claim;
	AxisConvertResult result;
	AxisDbConn *conn;
	gboolean permanent = FALSE;
	gsize size;
	gsize i;

	memset(&claim, 0, sizeof(claim));
	claim.id = N(fx, "SELECT job_id FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion);
	claim.attempt = 1;
	g_assert_cmpint(claim.id, >, 0);
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	if (!axis_conversions_describe(fx->conv, conn, &claim, "garage", machine, &work, &why_not, &permanent, &error))
		g_error("the claim of conversion %" G_GINT64_FORMAT ": %s", conversion, error->message);
	if (work == NULL) {
		axis_db_pool_release(fx->t->pool, conn);
		g_assert_nonnull(why_not);
		return why_not;
	}
	g_assert_null(why_not);
	/* What it made: smaller than the original by @ratio, bytes of no format
	 * (checks 3 to 8, which read it, passed in an earlier turn). */
	size = (gsize)((gdouble)work->size * ratio);
	bytes = g_malloc(size);
	for (i = 0; i < size; i++)
		bytes[i] = (gchar)((i * 31 + 7) & 0x7f);
	path = g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, work->output, NULL);
	g_assert_true(g_file_set_contents(path, bytes, (gssize)size, &error));
	g_assert_no_error(error);
	memset(&result, 0, sizeof(result));
	result.output = work->output;
	result.size = (gint64)size;
	g_assert_true(axis_db_conn_begin(conn, &error));
	if (!axis_conversions_encoded(fx->conv, conn, claim.id, &result, "garage", &error) ||
	    !axis_db_conn_commit(conn, &error))
		g_error("the encode of conversion %" G_GINT64_FORMAT ": %s", conversion, error->message);
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==, "verifying");
	return NULL;
}

/*
 * Every check of @conversion passed in an earlier turn of its finish job,
 * for its output as it is now, as a turn that stopped before placing leaves
 * it: the next turn makes checks 1, 2, 9 and 10 again (none needs an
 * encoder: the file asked for, the original unchanged, worth it, every
 * condition of the request) and places the output.
 */
static void
passed_before(
	Fixture *fx,
	gint64   conversion
){
	g_autofree gchar *path = output_path(fx, conversion);
	g_autofree gchar *sample = NULL;
	struct stat st;
	gint err = 0;
	gint fd;

	g_assert_cmpint(stat(path, &st), ==, 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(axis_file_sample_digest(fd, (gint64)st.st_size, &sample, &err));
	close(fd);
	sql(fx, "INSERT INTO conversion_check (conversion_id, number, result, detail) SELECT %" G_GINT64_FORMAT ", n,"
		" 'passed', 'as planned' FROM generate_series(1, 10) n", conversion);
	sql(fx, "UPDATE conversion SET checked_where = 'server', passed_at = now(), passed_inode = %" G_GINT64_FORMAT
		", passed_size = %" G_GINT64_FORMAT ", passed_mtime_sec = %" G_GINT64_FORMAT ", passed_mtime_nsec = %ld,"
		" passed_ctime_sec = %" G_GINT64_FORMAT ", passed_ctime_nsec = %ld, passed_sample = '%s', checks_version = %d"
		" WHERE id = %" G_GINT64_FORMAT, (gint64)st.st_ino, (gint64)st.st_size, (gint64)st.st_mtim.tv_sec,
		(long)st.st_mtim.tv_nsec, (gint64)st.st_ctim.tv_sec, (long)st.st_ctim.tv_nsec, sample,
		AXIS_CONVERSION_CHECKS_VERSION, conversion);
}

/* The finish job's turn for @conversion: its checks, then its placement. */
static void
finish(
	Fixture *fx,
	gint64   conversion
){
	g_autoptr(GError) error = NULL;

	if (!axis_conversions_finish(fx->conv, conversion, NULL, &error))
		g_error("the finish of conversion %" G_GINT64_FORMAT ": %s", conversion, error->message);
}

/*
 * Spec (conversion-execution, A kept file is converted only by choice), with
 * no encoder: a kept movie file asked for with the choice is taken by a
 * worker (the claim works the conditions out again with the choice the
 * conversion recorded); just before placement check 10 works them out
 * again, with it, and the output takes its original's place; the file is as
 * kept afterwards as before, in the same link and the same row.  The choice
 * covers a file kept at the request only: one chosen but not kept then,
 * and kept afterwards, is refused when a worker takes it; one asked for
 * without the choice that becomes kept after the claim is rejected by check
 * 10, the original as it was.
 */
static void
test_kept_claim_check_place(Fixture *fx, gconstpointer data)
{
	static const gchar *const kept_text = "Kept: Axis does not replace this file.";
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *original = NULL;
	g_autofree gchar *output = NULL;
	struct stat before;
	struct stat after;
	gint64 lantern;
	gint64 glass;
	gint64 paper;
	gint64 placed = 0;
	gint64 refused = 0;
	gint64 rejected = 0;

	kept_library(fx);
	movie(fx, "Paper Tide (2004)", 9);
	settle(fx);
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	lantern = file_of(fx, fx->movies, "Lantern Hill (2001)/Lantern Hill (2001).mkv");
	glass = file_of(fx, fx->movies, "Glass Orchard (2003)/Glass Orchard (2003).mkv");
	paper = file_of(fx, fx->movies, "Paper Tide (2004)/Paper Tide (2004).mkv");

	/* Kept by hand, asked for with the choice: recorded, and the claim lets
	 * it through. */
	g_assert_true(axis_conversions_request_full(fx->conv, lantern, fx->space_saver, AXIS_CONVERSION_REPLACE, TRUE, TRUE,
		"alice", &placed, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT, placed), ==, "true");
	why = claim_and_encode(fx, placed, 0.4);
	if (why != NULL)
		g_error("the claim refused a kept file asked for with the choice: %s", why);
	/* Check 10 just before placement, then the placement. */
	original = movie_path(fx, placed);
	g_assert_cmpint(stat(original, &before), ==, 0);
	passed_before(fx, placed);
	finish(fx, placed);
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, placed), ==, "done");
	g_assert_cmpstr(Q(fx, "SELECT detail FROM conversion_check WHERE conversion_id = %" G_GINT64_FORMAT
		" AND number = 10", placed), ==, "Every gate of the request holds now.");
	g_assert_cmpint(stat(original, &after), ==, 0);
	g_assert_cmpint((gint64)after.st_ino, !=, (gint64)before.st_ino);
	g_assert_cmpint((gint64)after.st_ino, ==, N(fx, "SELECT output_inode FROM conversion WHERE id = %" G_GINT64_FORMAT,
		placed));
	g_assert_cmpint((gint64)after.st_size, <, (gint64)before.st_size);
	/* The same row of the library, and still kept, as it was, in the same
	 * link. */
	g_assert_cmpint(N(fx, "SELECT inode FROM library_file WHERE id = %" G_GINT64_FORMAT, lantern), ==,
		(gint64)after.st_ino);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM movie_file mf JOIN movie m ON m.id = mf.movie_id JOIN meta_title t"
		" ON t.id = m.title_id WHERE mf.file_id = %" G_GINT64_FORMAT " AND mf.kept AND t.provider_id = 'kept-%"
		G_GINT64_FORMAT "'", lantern, lantern), ==, 1);
	g_assert_cmpint(kept_marks(fx), ==, 4);
	g_clear_pointer(&original, g_free);

	/* Chosen, but not kept at the request (a form that says so, or a file
	 * let go in between): nothing of the choice is recorded, so the mark set
	 * afterwards refuses it when a worker takes it. */
	g_assert_true(axis_conversions_request_full(fx->conv, glass, fx->space_saver, AXIS_CONVERSION_REPLACE, TRUE, TRUE,
		"alice", &refused, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT, refused), ==,
		"false");
	keep_file(fx, glass, FALSE);
	why = claim_and_encode(fx, refused, 0.4);
	g_assert_nonnull(why);
	g_assert_nonnull(strstr(why, "Refused when a worker took it: The file is not kept by hand or brought over"));
	g_assert_nonnull(strstr(why, kept_text));
	g_assert_cmpstr(Q(fx, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, refused), ==, "refused");
	g_clear_pointer(&why, g_free);

	/* Asked for without the choice, kept after the claim: check 10 rejects it
	 * before placement; the output is removed, the original as it was. */
	g_assert_true(axis_conversions_request(fx->conv, paper, fx->space_saver, AXIS_CONVERSION_REPLACE, TRUE, "alice",
		&rejected, &error));
	g_assert_no_error(error);
	why = claim_and_encode(fx, rejected, 0.4);
	g_assert_null(why);
	keep_file(fx, paper, TRUE);
	original = movie_path(fx, rejected);
	output = output_path(fx, rejected);
	g_assert_cmpint(stat(original, &before), ==, 0);
	passed_before(fx, rejected);
	finish(fx, rejected);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || failed_check FROM conversion WHERE id = %" G_GINT64_FORMAT, rejected),
		==, "rejected 10");
	g_assert_nonnull(strstr(Q(fx, "SELECT detail FROM conversion_check WHERE conversion_id = %" G_GINT64_FORMAT
		" AND number = 10 AND result = 'failed'", rejected), kept_text));
	g_assert_cmpint(stat(original, &after), ==, 0);
	g_assert_cmpint((gint64)after.st_ino, ==, (gint64)before.st_ino);
	g_assert_cmpint((gint64)after.st_size, ==, (gint64)before.st_size);
	g_assert_false(g_file_test(output, G_FILE_TEST_EXISTS));
	g_assert_cmpint(kept_marks(fx), ==, 6);
}

/*
 * Spec (conversion-runs, Kept files are converted only by choice): a run's
 * choice covers the files kept when its list was made.  A file of the list
 * that was not kept then and is marked kept afterwards is refused, as
 * without the choice, when its turn comes: skipped with the reason kept,
 * and nothing is asked for it; the run goes on with the next file.
 */
static void
test_kept_after_listing(Fixture *fx, gconstpointer data)
{
	gint64 run;
	gint64 glass;
	gint64 conversion;

	kept_library(fx);
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	glass = file_of(fx, fx->movies, "Glass Orchard (2003)/Glass Orchard (2003).mkv");
	run = make_run_kept(fx);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT " AND file_id = %"
		G_GINT64_FORMAT " AND NOT convert_kept", run, glass), ==, 1);
	/* Glass Orchard is kept by hand after the list was made. */
	keep_file(fx, glass, FALSE);

	start(fx, run, AXIS_CONVERSION_REPLACE);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT, conversion), ==,
		"true");
	end_conversion(fx, conversion, "done", 0, 0.4);
	/* Its turn: refused for the mark, and the run goes on with the next. */
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || reason_code FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, run, glass), ==, "skipped gate_kept");
	g_assert_nonnull(strstr(Q(fx, "SELECT reason_text FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, run, glass), "Kept: Axis does not replace this file."));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion WHERE file_id = %" G_GINT64_FORMAT, glass), ==, 0);
	g_assert_cmpstr(asked_names(fx, run), ==, "Lantern Hill (2001).mkv");
	g_assert_cmpstr(Q(fx, "SELECT convert_kept::text FROM conversion WHERE id = %" G_GINT64_FORMAT,
		asked_conversion(fx, run, "Lantern Hill (2001).mkv")), ==, "true");
	g_assert_cmpint(kept_marks(fx), ==, 5);
}

/*
 * Spec (conversion-runs, Kept files added to a run): adding the kept files to
 * a run that replaces originals needs the confirmation that their originals
 * are replaced; without it (a form forged without the tick) it is refused
 * and nothing changes.  With it, each file added records who confirmed it
 * and when, not the run's earlier confirmation; the run waits, paused, and
 * does not start by itself; once resumed, each file added is asked for as a
 * replacement with the choice, as the person who confirmed it.
 */
static void
test_kept_added_to_replacing(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionRunInfo) info = NULL;
	gint64 run;
	gint64 job;
	gint64 conversion;

	kept_library(fx);
	sql(fx, "INSERT INTO conversion_profile_proof (revision_id, method) VALUES (%" G_GINT64_FORMAT ", 'canary')",
		fx->space_saver);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_REPLACE);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || confirmed_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		==, "replace alice");
	job = N(fx, "SELECT coalesce(list_job_id, 0) FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run);

	/* Without the tick: refused, and the run is as it was. */
	g_assert_false(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "This run replaces originals"));
	g_assert_nonnull(strstr(error->message, "confirmation ticked"));
	g_clear_error(&error);
	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || convert_kept::text || ' ' || coalesce(convert_kept_by, '-') || ' ' ||"
		" coalesce(relist_state, '-') FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "finished false - -");
	g_assert_cmpint(N(fx, "SELECT coalesce(list_job_id, 0) FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		job);
	g_assert_cmpstr(reasons_of(fx, run), ==, "kept:4");

	/* With it: the files added carry dora's confirmation; the run waits. */
	add_kept_confirmed(fx, run, TRUE, "paused");
	assert_state(fx, run, "paused/person");
	g_assert_cmpstr(Q(fx, "SELECT pause_text || ' | ' || paused_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		==, "dora added 2 files Axis keeps to the list of this run, which had finished, and confirmed that their "
		"originals are replaced. Resume the run to convert them. | dora");
	g_assert_cmpstr(Q(fx, "SELECT string_agg(regexp_replace(e.rel_path, '^.*/', '') || '=' || coalesce(e.confirmed_by,"
		" '-') || CASE WHEN e.confirmed_at = r.convert_kept_at THEN ' then' ELSE '' END, ',' ORDER BY e.position)"
		" FROM conversion_run_entry e JOIN conversion_run r ON r.id = e.run_id WHERE e.run_id = %" G_GINT64_FORMAT, run),
		==, "Glass Orchard (2003).mkv=-,Copper Road (2002).mkv=dora then,Lantern Hill (2001).mkv=dora then");
	/* It does not start by itself. */
	g_assert_cmpuint(step(fx).asked, ==, 0);
	assert_state(fx, run, "paused/person");
	g_assert_true(axis_conversion_runs_get(fx->runs, run, &info, &error));
	g_assert_no_error(error);
	g_assert_cmpint(info->waiting_in, ==, 2);

	/* Resumed by another administrator: each file added is replaced on its
	 * own confirmation, as dora; the run's is alice's still. */
	g_assert_true(axis_conversion_runs_resume(fx->runs, run, "erin", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	conversion = asked_conversion(fx, run, "Copper Road (2002).mkv");
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || requested_by FROM conversion WHERE id = %"
		G_GINT64_FORMAT, conversion), ==, "replace true dora");
	end_conversion(fx, conversion, "done", 0, 0.4);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	g_assert_cmpstr(Q(fx, "SELECT outcome || ' ' || convert_kept::text || ' ' || requested_by FROM conversion WHERE id = %"
		G_GINT64_FORMAT, asked_conversion(fx, run, "Lantern Hill (2001).mkv")), ==, "replace true dora");
	g_assert_cmpstr(Q(fx, "SELECT confirmed_by FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==, "alice");
	g_assert_cmpint(kept_marks(fx), ==, 4);
}

/*
 * Spec (conversion-runs, Kept files added to a run): a finished run to which
 * the kept files are added but that can convert none of them (they are
 * beyond its cap) stays finished, and says why; it does not wait for a
 * resume that would do nothing.
 */
static void
test_kept_added_beyond_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionRunLimits limits;
	gint64 run;

	kept_library(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	memset(&limits, 0, sizeof(limits));
	limits.at_once = 1;
	limits.cap_files = 1;
	limits.window_start = limits.window_end = -1;
	g_assert_true(axis_conversion_runs_set_limits(fx->runs, run, &limits, "alice", &error));
	g_assert_no_error(error);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");

	add_kept(fx, run, "finished");
	assert_state(fx, run, "finished");
	g_assert_nonnull(strstr(Q(fx, "SELECT finish_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run),
		"dora added 2 files Axis keeps to the list after the run finished; it cannot convert any of them, as they are "
		"beyond its caps. Make a new run with \"Also convert files Axis keeps\" to convert them."));
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run WHERE id = %" G_GINT64_FORMAT " AND finished_at IS NOT NULL"
		" AND pause_code IS NULL", run), ==, 1);
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry e JOIN conversion_run r ON r.id = e.run_id"
		" WHERE e.run_id = %" G_GINT64_FORMAT " AND e.state = 'waiting' AND e.unit_position > r.cap_unit", run), ==, 2);
}

/*
 * The relist branch of a list job that fails (convert-kept-files): adding
 * the kept files to a finished run whose revision cannot decide files any
 * more leaves the run as it was (finished, at the same time, with its list)
 * with the choice off, and says that the files were not added.
 */
static void
test_kept_add_fails(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *finished_at = NULL;
	gint64 run;

	kept_library(fx);
	run = make_run(fx, AXIS_CONVERSION_RUN_MOVIES, NULL, 0, 0, fx->space_saver);
	start(fx, run, AXIS_CONVERSION_TRIAL);
	g_assert_cmpuint(step(fx).asked, ==, 1);
	end_asked(fx, run, "done", 0);
	step(fx);
	assert_state(fx, run, "finished");
	finished_at = QA(fx, "SELECT finished_at::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run);

	g_assert_true(axis_conversion_runs_include_kept(fx->runs, run, FALSE, "dora", &error));
	g_assert_no_error(error);
	/* A version of the profile whose stored text is not in canonical form
	 * (unusable), given to the run. */
	sql(fx, "INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin)"
		" SELECT r.profile_id, (SELECT max(x.number) + 1 FROM conversion_profile_revision x WHERE x.profile_id ="
		" r.profile_id), r.name, r.definition || ' ', encode(sha256(convert_to(r.definition || ' ', 'UTF8')), 'hex'),"
		" 'code' FROM conversion_profile_revision r WHERE r.id = %" G_GINT64_FORMAT, fx->space_saver);
	sql(fx, "UPDATE conversion_run SET revision_id = (SELECT max(id) FROM conversion_profile_revision) WHERE id = %"
		G_GINT64_FORMAT, run);
	g_assert_true(axis_conversion_runs_build_list(fx->runs, run, NULL, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(Q(fx, "SELECT state || ' ' || coalesce(relist_state, '-') || ' ' || convert_kept::text || ' ' ||"
		" coalesce(convert_kept_by, '-') || ' ' || coalesce(convert_kept_at::text, '-') FROM conversion_run WHERE id = %"
		G_GINT64_FORMAT, run), ==, "finished - false - -");
	g_assert_cmpstr(Q(fx, "SELECT finished_at::text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		finished_at);
	g_assert_cmpstr(Q(fx, "SELECT list_text FROM conversion_run WHERE id = %" G_GINT64_FORMAT, run), ==,
		"The files Axis keeps were not added, and the run is as it was. The run's profile revision cannot be used to "
		"decide files (its definition is not usable).");
	g_assert_cmpstr(reasons_of(fx, run), ==, "kept:4");
	g_assert_cmpint(N(fx, "SELECT count(*) FROM conversion_run_entry WHERE run_id = %" G_GINT64_FORMAT, run), ==, 1);
	g_assert_cmpint(kept_marks(fx), ==, 4);
}

/*
 * Spec (conversion-runs, A kept file with another reason): with the choice, a
 * kept file that is pinned (another hard link), remembered as not worth
 * converting with the revision, or that has a conversion in flight is left
 * out under that reason, as a file that is not kept would be.
 */
static void
test_kept_other_reasons(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *order = NULL;
	gint64 conversion = 0;
	gint64 run;

	movie(fx, "Glass Orchard (2003)", 18);
	movie(fx, "Worn Map (2007)", 12);
	movie(fx, "Second Look (2008)", 12);
	movie(fx, "Busy Line (2009)", 12);
	settle(fx);
	{
		g_autofree gchar *from = g_build_filename(fx->disk, "Movies", "Worn Map (2007)", "Worn Map (2007).mkv", NULL);
		g_autofree gchar *to = g_build_filename(fx->disk, "Movies", "Worn Map (2007)", "Worn Map copy.mkv", NULL);

		g_assert_cmpint(link(from, to), ==, 0);
		axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->movies, AXIS_SCAN_COMPLETE,
			AXIS_SCAN_REASON_NONE));
	}
	keep_file(fx, file_of(fx, fx->movies, "Worn Map (2007)/Worn Map (2007).mkv"), FALSE);
	keep_file(fx, file_of(fx, fx->movies, "Second Look (2008)/Second Look (2008).mkv"), TRUE);
	keep_file(fx, file_of(fx, fx->movies, "Busy Line (2009)/Busy Line (2009).mkv"), FALSE);
	sql(fx, "INSERT INTO conversion_memory (file_id, revision_id, reason) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", 'not_worth_it')", file_of(fx, fx->movies, "Second Look (2008)/Second Look (2008).mkv"), fx->space_saver);
	g_assert_true(axis_conversions_request(fx->conv, file_of(fx, fx->movies, "Busy Line (2009)/Busy Line (2009).mkv"),
		fx->space_saver, AXIS_CONVERSION_TRIAL, FALSE, "bob", &conversion, &error));
	g_assert_no_error(error);

	run = make_run_kept(fx);
	order = entry_order(fx, run);
	g_assert_cmpstr(order, ==, "Glass Orchard (2003).mkv");
	g_assert_cmpstr(reasons_of(fx, run), ==, "in_flight:1,not_worth_it:1,pinned:2");
	g_assert_cmpint(kept_marks(fx), ==, 3);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/by-choice", Fixture, NULL, test_kept_by_choice);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/trials-then-replace", Fixture, NULL, test_kept_trials_then_replace);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/added-to-ready", Fixture, NULL, test_kept_added_to_ready);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/added-later", Fixture, NULL, test_kept_added_later);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/added-to-series", Fixture, NULL, test_kept_added_to_series);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/claim-check-place", Fixture, NULL, test_kept_claim_check_place);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/after-listing", Fixture, NULL, test_kept_after_listing);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/added-to-replacing", Fixture, NULL, test_kept_added_to_replacing);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/added-beyond-cap", Fixture, NULL, test_kept_added_beyond_cap);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/add-fails", Fixture, NULL, test_kept_add_fails);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/kept/other-reasons", Fixture, NULL, test_kept_other_reasons);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/list/ranked", Fixture, NULL, test_list_ranked);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/list/series", Fixture, NULL, test_list_series);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/list/scopes", Fixture, NULL, test_list_scopes);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/list/lost", Fixture, NULL, test_lost_list);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/one-at-a-time", Fixture, NULL, test_one_at_a_time);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/series-together", Fixture, NULL, test_series_together);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/series-started-first", Fixture, NULL, test_series_started_first);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/skips", Fixture, NULL, test_skips);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/failures", Fixture, NULL, test_failures_pause);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/refusals", Fixture, NULL, test_refusals_pause);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/crash", Fixture, NULL, test_crash_pause);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/verdict", Fixture, NULL, test_verdict_pause);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/bad-trial", Fixture, NULL, test_bad_trial);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/no-worker", Fixture, NULL, test_no_worker_waits);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/read-only", Fixture, NULL, test_read_only_waits);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/offline", Fixture, NULL, test_offline_volume);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/full", Fixture, NULL, test_full_volume);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/restart", Fixture, NULL, test_restart);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/window", Fixture, NULL, test_window);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/trials-then-replace", Fixture, NULL, test_trials_then_replace);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/caps", Fixture, NULL, test_caps);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause-and-cancel", Fixture, NULL, test_pause_and_cancel);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/estimates", Fixture, NULL, test_estimates_and_reality);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/wait/in-flight-volume-without-worker", Fixture, NULL,
		test_in_flight_volume_without_worker);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/not-judged-asked-again", Fixture, NULL, test_not_judged_asked_again);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/list/kept", Fixture, NULL, test_list_kept);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/ask/link-after-stop",Fixture, NULL, test_link_after_stop);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/bad-trial-set-aside", Fixture, NULL, test_bad_trial_set_aside);
	AXIS_TEST_ADD_DB_CASE("/conversion-runs/pause/marks", Fixture, NULL, test_marks);
	return g_test_run();
}
