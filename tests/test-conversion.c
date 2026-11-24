/*
 * test-conversion.c - converting one file: a trial and a replacement made by
 * a real axis-worker with software encoders; both rename paths of the
 * placement; every check failing on an output made wrong on purpose; the
 * gates at request, when a worker takes the work and just before placement;
 * a daemon that stops before and after every step and recovers; the volume
 * going away; a scan of the volume and a placement excluding each other;
 * the work areas' leftovers, kept trials and recycled originals after their
 * time; proving a revision by hand; the volume write layer's refusals
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every media file is a clip generated here with ffmpeg's own sources, in
 * the test's scratch directory; every encode is made by a software encoder
 * (a real axis-worker is told to use software encoders only).  Most tests
 * play the worker themselves: they claim the work through the worker API,
 * run ffmpeg with the arguments the daemon gave (or changed on purpose),
 * write the output where a worker would and report it.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "db/axis-db-migrate.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-analysis.h"
#include "library/axis-library-kind.h"
#include "api/axis-worker-store.h"
#include "probe/axis-probe.h"
#include "storage/axis-file-check.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-guard.h"
#include "storage/axis-volume-marker.h"
#include "storage/axis-volume-write.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile-store.h"
#include "web/axis-password.h"
#include "web/axis-web-conversions.h"

#define LONG_WAIT_MS 180000

static gboolean have_ffmpeg;
static gchar *clips;	/* the generated clips, for the whole program */

typedef struct _Fixture Fixture;

struct _Fixture
{
	AxisTestLibrary          *t;
	AxisSettings             *settings;
	AxisEstimator            *estimator;
	AxisProbe                *probe;
	AxisConversions          *conv;
	AxisTestDaemon           *d;
	gchar                    *token;
	gchar                    *disk;
	gchar                    *uuid;
	gchar                    *scratch;
	gint64                    volume;
	gint64                    movies;
	guint                     n_movies;
	AxisCompatVerdict         verdict;
	GString                  *steps;
	const gchar              *stop_at;
	AxisConversionHookVerdict stop_how;
	void                    (*at_stop)(Fixture *fx);
	gboolean                  lock_was_free;
};

#define SKIP_UNLESS_READY(fx) do { \
	if (!have_ffmpeg) { g_test_skip("ffmpeg is not installed"); return; } \
	if ((fx)->t == NULL) return; \
} while (0)

/* ── Clips ──────────────────────────────────────────────────────── */

static gboolean
run(const gchar *const *argv)
{
	gint status = 0;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
	                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, NULL))
		return FALSE;
	return g_spawn_check_wait_status(status, NULL);
}

/* Four seconds of a moving test picture (BT.709, progressive) with a tone,
 * H.264 at a high rate, so that every profile saves on it; the same as
 * HEVC 10-bit HDR10 with its static metadata; the same in full range; and
 * the same stating no colour at all, not even its range, as many older
 * encodes do.  For that one x264 writes a raw stream (it writes no video
 * signal type for a limited range), which is copied into Matroska with the
 * tone: made straight into Matroska it would say "tv", the range ffmpeg
 * gives the frames.  No B-frames: the raw stream's timestamps are then its
 * order. */
static gboolean
make_clips(const gchar *dir)
{
	g_autofree gchar *movie = g_build_filename(dir, "movie.mkv", NULL);
	g_autofree gchar *hdr10 = g_build_filename(dir, "hdr10.mkv", NULL);
	g_autofree gchar *full = g_build_filename(dir, "full-range.mkv", NULL);
	g_autofree gchar *raw = g_build_filename(dir, "untagged.h264", NULL);
	g_autofree gchar *untagged = g_build_filename(dir, "untagged.mkv", NULL);
	const gchar *full_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-vf",
		"setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=pc",
		"-c:v", "libx264", "-b:v", "4M", "-pix_fmt", "yuv420p", "-x264-params",
		"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=pc", "-c:a", "aac", "-b:a", "96k",
		"-metadata:s:a:0", "language=eng", full, NULL
	};
	const gchar *raw_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-vf", "setparams=field_mode=prog", "-c:v", "libx264", "-bf", "0", "-b:v", "4M", "-pix_fmt", "yuv420p",
		"-f", "h264", raw, NULL
	};
	const gchar *untagged_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-framerate", "24", "-i", raw, "-f", "lavfi", "-i",
		"sine=frequency=440:duration=4", "-map", "0:0", "-map", "1:0", "-c:v", "copy", "-c:a", "aac", "-b:a", "96k",
		"-metadata:s:a:0", "language=eng", untagged, NULL
	};
	const gchar *movie_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-vf",
		"setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv",
		"-c:v", "libx264", "-b:v", "4M", "-pix_fmt", "yuv420p", "-x264-params",
		"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv", "-c:a", "aac", "-b:a", "96k",
		"-metadata:s:a:0", "language=eng", movie, NULL
	};
	const gchar *hdr10_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-vf",
		"setparams=field_mode=prog:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv,"
		"format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-b:v", "4M", "-x265-params",
		"colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:"
		"master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50):max-cll=1100,350:"
		"log-level=error", "-c:a", "aac", "-b:a", "96k", "-metadata:s:a:0", "language=eng", hdr10, NULL
	};

	return run(movie_argv) && run(hdr10_argv) && run(full_argv) && run(raw_argv) && run(untagged_argv);
}

#define SUBTITLE "1\n00:00:00,500 --> 00:00:01,500\nHello\n\n2\n00:00:02,000 --> 00:00:03,000\nGoodbye\n"
#define OTHER_SUBTITLE "1\n00:00:00,500 --> 00:00:01,500\nHullo\n\n2\n00:00:02,000 --> 00:00:03,000\nGoodbye\n"

/* The same picture with three sound tracks (two AAC of different tones,
 * one AC-3, all English), an English SubRip subtitle and, in tracks.mkv, a
 * font attached; tracks-mp4.mkv has no attachment (MP4 cannot hold one).
 * other.srt is the subtitle with one word changed, other/font.ttf another
 * font of the same name. */
static gboolean
make_track_clips(const gchar *dir)
{
	g_autofree gchar *srt = g_build_filename(dir, "en.srt", NULL);
	g_autofree gchar *other_srt = g_build_filename(dir, "other.srt", NULL);
	g_autofree gchar *font = g_build_filename(dir, "font.ttf", NULL);
	g_autofree gchar *other_dir = g_build_filename(dir, "other", NULL);
	g_autofree gchar *other_font = g_build_filename(other_dir, "font.ttf", NULL);
	g_autofree gchar *tracks = g_build_filename(dir, "tracks.mkv", NULL);
	g_autofree gchar *tracks_mp4 = g_build_filename(dir, "tracks-mp4.mkv", NULL);
	const gchar *with_font[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-f", "lavfi", "-i", "sine=frequency=550:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=660:duration=4", "-i", srt, "-map", "0", "-map", "1", "-map", "2", "-map",
		"3", "-map", "4", "-vf",
		"setparams=field_mode=prog:color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv",
		"-c:v", "libx264", "-b:v", "4M", "-pix_fmt", "yuv420p", "-x264-params",
		"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv", "-c:a:0", "aac", "-c:a:1", "aac", "-c:a:2", "ac3",
		"-ac:a:2", "2", "-c:s", "srt", "-metadata:s:a:0", "language=eng", "-metadata:s:a:1", "language=eng",
		"-metadata:s:a:2", "language=eng", "-metadata:s:s:0", "language=eng", "-attach", font, "-metadata:s:t:0",
		"mimetype=application/x-truetype-font", tracks, NULL
	};
	const gchar *without_font[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-i", tracks, "-map", "0:0", "-map", "0:1", "-map", "0:2", "-map",
		"0:3", "-map", "0:4", "-c", "copy", tracks_mp4, NULL
	};

	axis_test_storage_mkdir(other_dir);
	g_assert_true(g_file_set_contents(srt, SUBTITLE, -1, NULL));
	g_assert_true(g_file_set_contents(other_srt, OTHER_SUBTITLE, -1, NULL));
	g_assert_true(g_file_set_contents(font, "not really a font", -1, NULL));
	g_assert_true(g_file_set_contents(other_font, "not really a font either", -1, NULL));
	return run(with_font) && run(without_font);
}

static void
copy_file(
	const gchar *from,
	const gchar *to
){
	g_autofree gchar *content = NULL;
	g_autofree gchar *dir = g_path_get_dirname(to);
	gsize length = 0;

	g_assert_true(g_file_get_contents(from, &content, &length, NULL));
	axis_test_storage_mkdir(dir);
	g_assert_true(g_file_set_contents(to, content, (gssize)length, NULL));
}

/* ── The fixture ────────────────────────────────────────────────── */

static AxisCompatVerdict
verdict_of(
	gpointer     data,
	gint64       root_id,
	const gchar *slug
){
	Fixture *fx = data;

	return fx->verdict;
}

static AxisConversionHookVerdict
on_step(
	const gchar *step,
	gint64       conversion_id,
	gpointer     data
){
	Fixture *fx = data;

	g_string_append_printf(fx->steps, "%s ", step);
	if (fx->stop_at != NULL && strcmp(step, fx->stop_at) == 0) {
		AxisConversionHookVerdict how = fx->stop_how;

		fx->stop_at = NULL;
		if (fx->at_stop != NULL)
			fx->at_stop(fx);
		return how;
	}
	return AXIS_CONVERSION_HOOK_CONTINUE;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisConversionTuning tuning;
	AxisSampleTuning sample;
	AxisWorkerApiTiming timing;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "conversions: *");
	if (!have_ffmpeg)
		return;
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
	/* The scratch filesystem is small: the floor is the one thing tuned. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_MIN_FREE, "1MiB", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_CONVERSION_SETTING_MIN_FREE_PERCENT, "0", &error));
	g_assert_no_error(error);

	fx->estimator = axis_estimator_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->scratch = g_build_filename(fx->t->dir, "scratch", NULL);
	axis_test_storage_mkdir(fx->scratch);
	memset(&sample, 0, sizeof(sample));
	sample.scratch_parent = fx->scratch;
	axis_estimator_set_sample_tuning(fx->estimator, &sample);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	g_assert_true(axis_profile_store_sync_builtins(fx->t->pool, NULL, &error));
	g_assert_no_error(error);

	fx->conv = axis_conversions_new(fx->t->pool, fx->settings, fx->t->storage, fx->probe, fx->estimator);
	fx->verdict = AXIS_VERDICT_SAFE;
	axis_conversions_set_verdict(fx->conv, verdict_of, fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.software_preset = "fast";
	tuning.threads = 2;
	tuning.tool_timeout_ms = 120000;
	tuning.similarity_points = 2;
	tuning.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &tuning);
	fx->steps = g_string_new(NULL);
	axis_conversions_set_hook(fx->conv, on_step, fx);

	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk");
	fx->uuid = axis_test_library_q(fx->t, "SELECT uuid::text FROM volume WHERE id = %" G_GINT64_FORMAT, fx->volume);
	fx->movies = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Movies", AXIS_MEDIA_MOVIES);

	memset(&timing, 0, sizeof(timing));
	/* A test may ask for a short lease (its data, in milliseconds). */
	timing.lease_ms = data != NULL ? GPOINTER_TO_UINT(data) : 60000;
	timing.heartbeat_ms = 500;
	timing.poll_ms = 200;
	timing.reap_ms = 50;
	timing.backoff_base_ms = 50;
	timing.backoff_cap_ms = 100;
	fx->d = axis_test_daemon_new(fx->t->pool, fx->estimator, &timing, NULL);
	axis_worker_api_set_convert(fx->d->api, axis_web_conversions_worker_hooks(), fx->conv);
	fx->token = axis_test_daemon_add_worker(fx->d, "garage", NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_test_daemon_free(fx->d);
	axis_secret_free(fx->token);
	axis_conversions_unref(fx->conv);
	axis_probe_unref(fx->probe);
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	g_string_free(fx->steps, TRUE);
	g_free(fx->disk);
	g_free(fx->uuid);
	g_free(fx->scratch);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

/* ── Helpers: the library ───────────────────────────────────────── */

static gint64
n(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_library_n(fx->t, "%s", sql);
}

static void
sql(
	Fixture     *fx,
	const gchar *statement
){
	g_autoptr(GError) error = NULL;

	if (!axis_db_pool_exec(fx->t->pool, statement, 0, NULL, NULL, &error))
		g_error("%s: %s", statement, error->message);
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
read_only(
	Fixture  *fx,
	gboolean  on
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, on ? "true" : "false", &error));
	g_assert_no_error(error);
}

static void
scan(Fixture *fx)
{
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->movies, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
}

static void
probe_file(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;

	g_assert_true(axis_probe_file(fx->probe, id, NULL, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_PROBE_OUTCOME_STORED);
}

/* The file's path relative to the root and on disk. */
static gchar *
movie_rel(
	guint        number,
	const gchar *extension
){
	return g_strdup_printf("Movie %u (2001)/Movie %u (2001).%s", number, number, extension);
}

static gchar *
movie_path(
	Fixture     *fx,
	guint        number,
	const gchar *extension
){
	g_autofree gchar *rel = movie_rel(number, extension);

	return g_build_filename(fx->disk, "Movies", rel, NULL);
}

/* A new movie file from the clip @clip ("movie.mkv"), scanned and probed:
 * its number in *@number, its id returned. */
static gint64
add_movie(
	Fixture     *fx,
	const gchar *clip,
	guint       *number
){
	g_autofree gchar *from = g_build_filename(clips, clip, NULL);
	g_autofree gchar *to = NULL;
	g_autofree gchar *rel = NULL;
	gint64 id;

	*number = ++fx->n_movies;
	to = movie_path(fx, *number, "mkv");
	rel = movie_rel(*number, "mkv");
	copy_file(from, to);
	scan(fx);
	id = axis_test_scenario_file(fx->t, fx->movies, rel);
	probe_file(fx, id);
	return id;
}

static gint64
revision_of(
	Fixture     *fx,
	const gchar *slug
){
	return axis_test_library_n(fx->t, "SELECT current_revision_id FROM conversion_profile WHERE slug = '%s'", slug);
}

/* As if the canary step had proven it. */
static void
prove_on_devices(
	Fixture *fx,
	gint64   revision
){
	g_autofree gchar *statement = g_strdup_printf("INSERT INTO conversion_profile_proof (revision_id, method)"
		" VALUES (%" G_GINT64_FORMAT ", 'canary')", revision);

	sql(fx, statement);
}

static gint64
request(
	Fixture               *fx,
	gint64                 file,
	const gchar           *slug,
	AxisConversionOutcome  outcome
){
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	if (!axis_conversions_request(fx->conv, file, revision_of(fx, slug), outcome, TRUE, "root", &id, &error))
		g_error("the request was refused: %s", error->message);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* A request that must be refused, saying @needle; nothing is stored. */
static void
request_refused(
	Fixture               *fx,
	gint64                 file,
	gint64                 revision,
	AxisConversionOutcome  outcome,
	gboolean               confirmed,
	const gchar           *needle
){
	g_autoptr(GError) error = NULL;
	gint64 before = n(fx, "SELECT count(*) FROM conversion");
	gint64 jobs = n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode'");
	gint64 id = 0;

	g_test_message("expecting a refusal saying: %s", needle);
	g_assert_false(axis_conversions_request(fx->conv, file, revision, outcome, confirmed, "root", &id, &error));
	g_assert_nonnull(error);
	if (strstr(error->message, needle) == NULL)
		g_error("expected the refusal to say '%s'; it says: %s", needle, error->message);
	g_assert_cmpint(id, ==, 0);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion"), ==, before);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode'"), ==, jobs);
}

static gchar *
state_of(
	Fixture *fx,
	gint64   id
){
	return axis_test_library_q(fx->t, "SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
}

static void
assert_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state
){
	g_autofree gchar *now = state_of(fx, id);

	if (g_strcmp0(now, state) != 0) {
		g_autofree gchar *text = axis_test_library_q(fx->t, "SELECT coalesce(result_text, '') || ' (check ' ||"
			" coalesce(failed_check, 0) || ': ' || coalesce((SELECT string_agg(detail, '; ') FROM conversion_check k"
			" WHERE k.conversion_id = c.id AND k.result = 'failed'), '') || ')' FROM conversion c WHERE id = %"
			G_GINT64_FORMAT, id);

		g_error("conversion %" G_GINT64_FORMAT " is %s, not %s: %s", id, now, state, text);
	}
}

static gchar *
check_of(
	Fixture *fx,
	gint64   id,
	guint    number
){
	return axis_test_library_q(fx->t, "SELECT coalesce((SELECT result FROM conversion_check WHERE conversion_id = %"
		G_GINT64_FORMAT " AND number = %u), 'none')", id, number);
}

static void
assert_all_passed(
	Fixture *fx,
	gint64   id
){
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_check WHERE conversion_id = %"
		G_GINT64_FORMAT " AND result = 'passed'", id), ==, AXIS_CONVERSION_N_CHECKS);
}

/* Rejected by check @failed: the earlier ones passed, the later ones were
 * skipped. */
static void
assert_rejected_by(
	Fixture *fx,
	gint64   id,
	guint    failed
){
	guint k;

	assert_state(fx, id, "rejected");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT failed_check FROM conversion WHERE id = %" G_GINT64_FORMAT, id),
		==, failed);
	for (k = 1; k <= AXIS_CONVERSION_N_CHECKS; k++) {
		g_autofree gchar *result = check_of(fx, id, k);

		if (k < failed)
			g_assert_cmpstr(result, ==, "passed");
		else if (k == failed)
			g_assert_cmpstr(result, ==, "failed");
		else if (!(failed == 3 && k == 4) && g_strcmp0(result, "skipped") != 0 &&
		         !(failed == 1 && g_strcmp0(result, "none") == 0))
			g_error("check %u of conversion %" G_GINT64_FORMAT " is %s; check %u failed", k, id, result, failed);
	}
}

static gboolean
finish(
	Fixture *fx,
	gint64   id,
	GError **error
){
	return axis_conversions_finish(fx->conv, id, NULL, error);
}

static void
finish_ok(
	Fixture *fx,
	gint64   id
){
	g_autoptr(GError) error = NULL;

	if (!finish(fx, id, &error))
		g_error("finishing conversion %" G_GINT64_FORMAT " failed: %s", id, error->message);
}

static gboolean
exists(const gchar *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

static gint64
inode_of(const gchar *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	return (gint64)st.st_ino;
}

static gchar *
work_path(
	Fixture     *fx,
	const gchar *name
){
	return g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, name, NULL);
}

static gint
by_name(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* Names in a directory, sorted, space-separated ("" when it is missing). */
static gchar *
listing(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	const gchar *name;
	GString *s = g_string_new(NULL);
	guint i;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, by_name);
	for (i = 0; i < names->len; i++)
		g_string_append_printf(s, "%s%s", i > 0 ? " " : "", (const gchar *)g_ptr_array_index(names, i));
	return g_string_free(s, FALSE);
}

/* Every regular file under @dir, recursively. */
static guint
count_files(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint count = 0;

	while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		struct stat st;

		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			count += count_files(path);
		else
			count++;
	}
	return count;
}

/* ── Helpers: playing the worker ────────────────────────────────── */

static AxisTestReply
api(
	Fixture     *fx,
	const gchar *name,
	const gchar *body
){
	return axis_test_daemon_post(fx->d, "POST", name, fx->token, NULL, body, NULL);
}

static void
do_register_as(
	Fixture     *fx,
	const gchar *token
);

/* Registers as a worker whose libx265 was shown to make 10-bit HDR10 too
 * (tests/axis-test-worker.c's software encoders, with HDR10). */
static void
do_register(Fixture *fx)
{
	do_register_as(fx, fx->token);
}

static void
do_register_as(
	Fixture     *fx,
	const gchar *token
){
	g_autoptr(AxisWorkerRegister) request = axis_worker_register_new();
	AxisEncoderCapabilities *caps = axis_encoder_capabilities_new();
	AxisWorkerVolumeSeen *seen = g_new0(AxisWorkerVolumeSeen, 1);
	g_autofree gchar *json = NULL;
	AxisTestReply reply;

	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	caps->aac = TRUE;
	caps->mov_text = TRUE;
	caps->subrip = TRUE;
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), TRUE, TRUE, TRUE, NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, FALSE, FALSE, NULL);
	request->worker_version = g_strdup("0.1.0");
	request->max_jobs = 1;
	request->caps = caps;
	seen->uuid = g_strdup(fx->uuid);
	seen->path = g_strdup(fx->disk);
	seen->keeps_identity = TRUE;
	g_ptr_array_add(request->volumes, seen);
	json = axis_worker_register_to_json(request);
	reply = axis_test_daemon_post(fx->d, "POST", "register", token, NULL, json, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "\"accepted\":true"));
	axis_test_reply_clear(&reply);
}

/* Claims convert.encode work: %NULL when there is none. */
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
	/* The volume's UUID and paths relative to it, never where the daemon
	 * sees the disk. */
	g_assert_null(strstr(reply.body, fx->t->dir));
	if (!axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, &problem))
		g_error("%s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	return job;
}

static AxisWorkerJob *
claim_work(Fixture *fx)
{
	AxisWorkerJob *job = claim(fx);

	g_assert_nonnull(job);
	g_assert_cmpstr(job->kind, ==, AXIS_WORKER_KIND_CONVERT);
	g_assert_nonnull(job->convert);
	g_assert_cmpstr(job->convert->volume, ==, fx->uuid);
	g_assert_true(axis_convert_output_name_valid(job->convert->output));
	return job;
}

static guint
complete(
	Fixture             *fx,
	const AxisWorkerJob *job,
	const gchar         *output,
	gint64               size
){
	AxisConvertResult result;
	AxisWorkerComplete request;
	g_autofree gchar *json = NULL;
	AxisTestReply reply;
	guint status;

	result.output = (gchar *)output;
	result.size = size;
	memset(&request, 0, sizeof(request));
	request.job = job->id;
	request.lease_token = job->lease_token;
	request.kind = (gchar *)AXIS_WORKER_KIND_CONVERT;
	request.convert = &result;
	json = axis_worker_complete_to_json(&request);
	if (json == NULL) {
		/* A name the protocol itself refuses: sent as text. */
		json = g_strdup_printf("{\"protocol\":%d,\"job\":%" G_GINT64_FORMAT ",\"lease_token\":\"%s\",\"kind\":\"%s\","
			"\"result\":{\"output\":\"%s\",\"size\":%" G_GINT64_FORMAT "}}", AXIS_WORKER_PROTOCOL, job->id,
			job->lease_token, AXIS_WORKER_KIND_CONVERT, output, size);
	}
	reply = api(fx, "complete", json);
	status = reply.status;
	axis_test_reply_clear(&reply);
	return status;
}

static gchar *
source_of(
	Fixture               *fx,
	const AxisConvertWork *work
){
	return g_build_filename(fx->disk, work->root, work->path, NULL);
}

/* ffmpeg with @args (the daemon's, or changed), the source as descriptor 3
 * and a new file @output of the work area as 4, as a worker runs it. */
static gboolean
encode_with(
	Fixture               *fx,
	const AxisConvertWork *work,
	gchar                **args,
	const gchar           *output
){
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autofree gchar *src = source_of(fx, work);
	g_autofree gchar *out = work_path(fx, output);
	gint in;
	gint fd;
	guint i;

	in = open(src, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(in, >=, 0);
	fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	g_assert_cmpint(fd, >=, 0);
	g_subprocess_launcher_take_fd(launcher, in, 3);
	g_subprocess_launcher_take_fd(launcher, fd, 4);
	g_ptr_array_add(argv, (gpointer)"ffmpeg");
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, args[i]);
	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, NULL);
	return process != NULL && g_subprocess_wait_check(process, NULL, NULL);
}

static gint64
size_of(const gchar *path)
{
	struct stat st;

	g_assert_cmpint(stat(path, &st), ==, 0);
	return (gint64)st.st_size;
}

/* Encodes as told and reports it: the output's size. */
static gint64
encode_and_report(
	Fixture             *fx,
	const AxisWorkerJob *job,
	gchar              **args
){
	g_autofree gchar *out = work_path(fx, job->convert->output);
	gint64 size;

	g_assert_true(encode_with(fx, job->convert, args != NULL ? args : job->convert->args, job->convert->output));
	size = size_of(out);
	g_assert_cmpuint(complete(fx, job, job->convert->output, size), ==, 200);
	return size;
}

/* The plan's arguments with the value after @option changed to @value. */
static gchar **
args_set(
	const AxisConvertWork *work,
	const gchar           *option,
	const gchar           *value
){
	gchar **args = g_strdupv(work->args);
	guint i;

	for (i = 0; args[i] != NULL && args[i + 1] != NULL; i++) {
		if (strcmp(args[i], option) == 0) {
			g_free(args[i + 1]);
			args[i + 1] = g_strdup(value);
			return args;
		}
	}
	g_error("the plan has no %s", option);
	return NULL;
}

/* The plan's arguments with @extra put before the output's format. */
static gchar **
args_insert(
	const AxisConvertWork *work,
	const gchar *const    *extra
){
	GPtrArray *out = g_ptr_array_new();
	guint last = 0;
	guint i;
	guint k;

	for (i = 0; work->args[i] != NULL; i++) {
		if (strcmp(work->args[i], "-f") == 0)
			last = i;
	}
	g_assert_cmpuint(last, >, 0);
	for (i = 0; work->args[i] != NULL; i++) {
		if (i == last) {
			for (k = 0; extra[k] != NULL; k++)
				g_ptr_array_add(out, g_strdup(extra[k]));
		}
		g_ptr_array_add(out, g_strdup(work->args[i]));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/* A request of a new movie, claimed: the conversion, the job. */
static gint64
requested_and_claimed(
	Fixture               *fx,
	const gchar           *clip,
	const gchar           *slug,
	AxisConversionOutcome  outcome,
	guint                 *number,
	AxisWorkerJob        **job
){
	gint64 file = add_movie(fx, clip, number);
	gint64 id = request(fx, file, slug, outcome);

	*job = claim_work(fx);
	assert_state(fx, id, "encoding");
	return id;
}

/* ── A real worker ──────────────────────────────────────────────── */

static AxisTestWorker *
start_worker_with(
	Fixture     *fx,
	const gchar *ffmpeg
){
	const gchar *volumes[2];
	AxisTestWorkerOptions options;
	AxisTestWorker *w;

	volumes[0] = fx->disk;
	volumes[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = fx->d->url;
	options.token = fx->token;
	options.volumes = volumes;
	options.ffmpeg = ffmpeg;
	w = axis_test_worker_start(&options);
	if (!axis_test_worker_wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS))
		g_error("the worker did not register; its log:\n%s", axis_test_worker_get_log(w));
	return w;
}

static AxisTestWorker *
start_worker(Fixture *fx)
{
	return start_worker_with(fx, NULL);
}

static void
stop_worker(
	Fixture        *fx,
	AxisTestWorker *w
){
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	if (axis_test_worker_get_status(w) != 0)
		g_error("the worker ended with %d; its log:\n%s", axis_test_worker_get_status(w), axis_test_worker_get_log(w));
	g_assert_null(strstr(axis_test_worker_get_log(w), fx->token));
	axis_test_worker_free(w);
}

static void
wait_state(
	Fixture        *fx,
	AxisTestWorker *w,
	gint64          id,
	const gchar    *state
){
	g_autofree gchar *statement = g_strdup_printf("SELECT state FROM conversion WHERE id = %" G_GINT64_FORMAT, id);

	if (!axis_test_wait_query(fx->t->db, statement, state, LONG_WAIT_MS))
		g_error("conversion %" G_GINT64_FORMAT " did not become %s; the worker's log:\n%s", id, state,
			w != NULL ? axis_test_worker_get_log(w) : "");
}

/* Spec: a trial is converted by a worker and kept aside; the library is not
 * touched. */
static void
test_trial_real_worker(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *library = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *trial = NULL;
	g_autofree gchar *kept = NULL;
	AxisTestWorker *w;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	file = add_movie(fx, "movie.mkv", &number);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	library = axis_test_storage_digest(movies);
	w = start_worker(fx);
	/* A locked revision: a trial is allowed. */
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND dedupe_key ="
		" 'convert.encode:' || (SELECT max(id) FROM conversion)::text AND payload->'requires'->>'encoder' = 'libx265'"),
		==, 1);
	wait_state(fx, w, id, "verifying");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.finish' AND state = 'queued'"), ==, 1);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	stop_worker(fx, w);

	/* The trial in the work area, the library as it was. */
	trial = axis_test_library_q(fx->t, "SELECT trial_name FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	kept = g_strdup_printf("trial-%" G_GINT64_FORMAT ".mkv", id);
	g_assert_cmpstr(trial, ==, kept);
	{
		g_autofree gchar *path = work_path(fx, trial);
		g_autofree gchar *names = NULL;
		g_autofree gchar *work = work_path(fx, "");

		g_assert_true(exists(path));
		names = listing(work);
		g_assert_cmpstr(names, ==, kept);
	}
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(library, ==, after);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE trial_state = 'kept' AND trial_remove_after >"
		" now() + interval '13 days'"), ==, 1);
	/* What was measured is a calibration sample of the estimator. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample WHERE origin = 'converted' AND codec = 'hevc'"
		" AND output_bitrate > 0"), ==, 1);
	/* Not remembered as converted: a trial changes nothing. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_memory"), ==, 0);

	/* Removed on request: the file goes, the record says so. */
	g_assert_true(axis_conversions_remove_trial(fx->conv, id, "root", &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *path = work_path(fx, trial);

		g_assert_false(exists(path));
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE trial_state = 'removed'"), ==, 1);
	g_assert_false(axis_conversions_remove_trial(fx->conv, id, "root", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
}

/* Spec: a replacement by a worker: the original's name holds the verified
 * new file, the index row is the same row, the sidecar is not touched, the
 * original is removed once recorded. */
static void
test_replace_real_worker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *sidecar = NULL;
	g_autofree gchar *recycle = NULL;
	g_autofree gchar *rel = NULL;
	AxisTestWorker *w;
	guint number;
	gint64 file;
	gint64 id;
	gint64 before_ino;
	gint64 before_size;
	gint64 output_ino;

	SKIP_UNLESS_READY(fx);
	file = add_movie(fx, "movie.mkv", &number);
	path = movie_path(fx, number, "mkv");
	sidecar = movie_path(fx, number, "en.srt");
	axis_test_storage_write(sidecar, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
	scan(fx);
	before_ino = inode_of(path);
	before_size = size_of(path);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	w = start_worker(fx);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_REPLACE);
	wait_state(fx, w, id, "verifying");
	finish_ok(fx, id);
	stop_worker(fx, w);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);

	/* The steps of a replacement that keeps the name, in order. */
	if (strstr(fx->steps->str, "settle settle-done placing adopt adopt-done link link-done swap swap-done unlink "
	           "unlink-done record remove-original") == NULL)
		g_error("the steps were: %s", fx->steps->str);
	output_ino = axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpint(inode_of(path), ==, output_ino);
	g_assert_cmpint(inode_of(path), !=, before_ino);
	g_assert_cmpint(size_of(path), <, before_size);
	/* The same index row, now the new file's. */
	rel = movie_rel(number, "mkv");
	g_assert_cmpint(axis_test_scenario_file(fx->t, fx->movies, rel), ==, file);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT inode FROM library_file WHERE id = %" G_GINT64_FORMAT, file), ==,
		output_ino);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_memory WHERE reason = 'converted'"), ==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE recycle_state = 'removed' AND recycle_path IS NOT NULL"),
		==, 1);
	/* The probe looks at it again. */
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind LIKE 'probe%' AND state = 'queued'"), >=, 1);
	/* Nothing left behind: no temporary name, an empty recycle folder, an
	 * empty work area; the sidecar as it was. */
	{
		g_autofree gchar *folder = g_path_get_dirname(path);
		g_autofree gchar *names = listing(folder);
		g_autofree gchar *work = work_path(fx, "");
		g_autofree gchar *left = listing(work);
		g_autofree gchar *expected = g_strdup_printf("Movie %u (2001).en.srt Movie %u (2001).mkv", number, number);

		g_assert_cmpstr(names, ==, expected);
		g_assert_cmpstr(left, ==, "");
	}
	recycle = g_build_filename(fx->disk, AXIS_VOLUME_RECYCLE_DIR, NULL);
	g_assert_cmpuint(count_files(recycle), ==, 0);
	{
		g_autofree gchar *text = axis_test_storage_read(sidecar);

		g_assert_cmpstr(text, ==, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
	}
	/* A scan afterwards finds the same file in the same row. */
	scan(fx);
	g_assert_cmpint(axis_test_scenario_file(fx->t, fx->movies, rel), ==, file);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND state = 'present'", fx->movies), ==, 2);
	/* Probed again, the file is not offered with the revision again. */
	probe_file(fx, file);
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisConversionPreview) preview = NULL;
		gboolean remembered = FALSE;
		guint i;

		g_assert_true(axis_conversions_preview(fx->conv, file, revision_of(fx, "space_saver"), AXIS_CONVERSION_TRIAL,
			&preview, &error));
		g_assert_false(preview->ok);
		for (i = 0; i < preview->gates->len; i++) {
			const AxisConversionGate *gate = g_ptr_array_index(preview->gates, i);

			if (strcmp(gate->id, "memory") == 0)
				remembered = !gate->passed && strstr(gate->detail, "converted with this version") != NULL;
		}
		g_assert_true(remembered);
	}
}

typedef struct
{
	GPid parent;
	GPid found;
} ChildWait;

static gboolean
child_there(gpointer data)
{
	ChildWait *wait = data;

	wait->found = axis_test_find_child(wait->parent, "sleep");
	return wait->found != 0;
}

static gboolean
process_gone(gpointer data)
{
	return !axis_test_process_alive(*(GPid *)data);
}

typedef struct
{
	gchar *dir;
} AreaWait;

static gboolean
area_empty(gpointer data)
{
	AreaWait *wait = data;
	g_autofree gchar *names = listing(wait->dir);

	return names[0] == '\0';
}

/* Spec: a worker stopped while it encodes (the conversion cancelled, the
 * volume replaced under it) stops its encoder and removes its output:
 * nothing is left in the work area, and the library is as it was. */
static void
test_worker_stops(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *flag = NULL;
	g_autofree gchar *slow = NULL;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *library = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *moved = NULL;
	AreaWait area;
	ChildWait wait;
	AxisTestWorker *w;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	flag = g_build_filename(fx->t->dir, "stand-still", NULL);
	slow = axis_test_slow_ffmpeg(fx->t->dir, flag);
	axis_test_storage_write(flag, "");
	file = add_movie(fx, "movie.mkv", &number);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	library = axis_test_storage_digest(movies);
	w = start_worker_with(fx, slow);

	/* Cancelled while it encodes. */
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "encoding");
	wait.parent = axis_test_worker_get_pid(w);
	wait.found = 0;
	g_assert_true(axis_test_pump_until(child_there, &wait, AXIS_TEST_WAIT_MS));
	area.dir = work_path(fx, "");
	g_assert_false(area_empty(&area));
	g_assert_true(axis_conversions_cancel(fx->conv, id, "root", &error));
	g_assert_no_error(error);
	g_assert_true(axis_test_pump_until(process_gone, &wait.found, AXIS_TEST_WAIT_MS));
	if (!axis_test_pump_until(area_empty, &area, AXIS_TEST_WAIT_MS))
		g_error("the work area still holds something; the worker's log:\n%s", axis_test_worker_get_log(w));
	assert_state(fx, id, "cancelled");
	g_free(area.dir);

	/* Another disk where the volume was, while it encodes. */
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "encoding");
	wait.found = 0;
	g_assert_true(axis_test_pump_until(child_there, &wait, AXIS_TEST_WAIT_MS));
	moved = g_strconcat(fx->disk, ".moved", NULL);
	g_assert_cmpint(g_rename(fx->disk, moved), ==, 0);
	axis_test_storage_mkdir(fx->disk);
	axis_test_storage_write_marker(fx->disk, "5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a");
	g_assert_true(axis_test_pump_until(process_gone, &wait.found, AXIS_TEST_WAIT_MS));
	area.dir = g_build_filename(moved, AXIS_VOLUME_WORK_DIR, NULL);
	if (!axis_test_pump_until(area_empty, &area, AXIS_TEST_WAIT_MS))
		g_error("the work area still holds something; the worker's log:\n%s", axis_test_worker_get_log(w));
	g_free(area.dir);
	g_assert_true(axis_conversions_cancel(fx->conv, id, "root", &error));
	g_assert_cmpint(g_unlink(flag), ==, 0);
	stop_worker(fx, w);
	{
		g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);

		g_assert_cmpint(g_unlink(marker), ==, 0);
		g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
		g_assert_cmpint(g_rename(moved, fx->disk), ==, 0);
	}
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(library, ==, after);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE state = 'done'"), ==, 0);
}

/* Spec: a replacement into another container: the new name is published
 * before the original is retired. */
static void
test_replace_new_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *old_path = NULL;
	g_autofree gchar *new_path = NULL;
	g_autofree gchar *new_rel = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "max_compat"));
	id = requested_and_claimed(fx, "movie.mkv", "max_compat", AXIS_CONVERSION_REPLACE, &number, &job);
	file = axis_test_library_n(fx->t, "SELECT file_id FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	old_path = movie_path(fx, number, "mkv");
	new_path = movie_path(fx, number, "mp4");
	/* A subtitle beside it, by its base name. */
	{
		g_autofree gchar *sidecar = movie_path(fx, number, "en.srt");

		axis_test_storage_write(sidecar, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
	}
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	if (strstr(fx->steps->str, "settle settle-done placing adopt adopt-done publish publish-done retire retire-done "
	           "record remove-original") == NULL)
		g_error("the steps were: %s", fx->steps->str);
	g_assert_false(exists(old_path));
	g_assert_cmpint(inode_of(new_path), ==, axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
	/* The subtitle as it was, and still the movie's by its base name. */
	{
		g_autofree gchar *sidecar = movie_path(fx, number, "en.srt");
		g_autofree gchar *text = axis_test_storage_read(sidecar);
		g_autofree gchar *folder = g_path_get_dirname(new_path);
		g_autofree gchar *names = listing(folder);
		g_autofree gchar *expected = g_strdup_printf("Movie %u (2001).en.srt Movie %u (2001).mp4", number, number);

		g_assert_cmpstr(text, ==, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
		g_assert_cmpstr(names, ==, expected);
	}
	new_rel = movie_rel(number, "mp4");
	g_assert_cmpint(axis_test_scenario_file(fx->t, fx->movies, new_rel), ==, file);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT
		" AND kind = 'video' AND moved_at IS NOT NULL", file), ==, 1);
	scan(fx);
	g_assert_cmpint(axis_test_scenario_file(fx->t, fx->movies, new_rel), ==, file);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT,
		fx->movies), ==, 2);
}

/* ── Every check, failing ───────────────────────────────────────── */

typedef enum
{
	BAD_NAME,	/* the worker reports another name */
	BAD_MISSING,	/* the reported file is not there */
	BAD_SIZE,	/* another size than the file's */
	BAD_SYMLINK,	/* the output is a link to the original */
	BAD_HARDLINK,	/* the output is another name of the original */
	BAD_SOURCE,	/* the original changed after the encode */
	BAD_SOURCE_LINKED,	/* the original got another name (a download client) after the encode */
	BAD_NO_AUDIO,	/* a stream missing */
	BAD_EXTRA,	/* a stream more */
	BAD_FLAGS,	/* the sound's default flag flipped */
	BAD_DEPTH,	/* 8 bits instead of 10 */
	BAD_HDR10,	/* the HDR10 values left out */
	BAD_TRUNCATED,	/* cut short after it was written */
	BAD_SHORTER,	/* half the movie */
	BAD_PICTURE,	/* another picture */
	BAD_LARGER,	/* not smaller */
	BAD_VERDICT,	/* the player check changed its mind */
	BAD_RANGE_FULL,	/* a source that states no colour made full range */
	BAD_RANGE_LIMITED,	/* a full-range source made limited */
	BAD_MATRIX,	/* BT.709 made BT.601's matrix */
	BAD_PRIMARIES,	/* BT.709 made BT.601's primaries */
	BAD_TRANSFER	/* a source that states no colour given a transfer function */
} Bad;

/* The clip each case starts from. */
static const gchar *
clip_for(Bad bad)
{
	switch (bad) {
	case BAD_HDR10:
		return "hdr10.mkv";
	case BAD_RANGE_FULL:
	case BAD_TRANSFER:
		return "untagged.mkv";
	case BAD_RANGE_LIMITED:
		return "full-range.mkv";
	default:
		return "movie.mkv";
	}
}

/* What checks 3 and 4 say of a colour made wrong: each names the property,
 * what the plan says and what the output has. */
static void
colour_said(
	Bad           bad,
	const gchar **three,
	const gchar **four
){
	*three = NULL;
	*four = NULL;
	switch (bad) {
	case BAD_RANGE_FULL:
		*three = "The output is not what the plan says: stream 0: the colour range is full (pc), the plan says "
			"unstated (limited).";
		*four = "The picture's colour range is not the planned one: the plan says unstated (limited), the output has "
			"full (pc).";
		break;
	case BAD_RANGE_LIMITED:
		*three = "The output is not what the plan says: stream 0: the colour range is limited (tv), the plan says "
			"full (pc).";
		*four = "The picture's colour range is not the planned one: the plan says full (pc), the output has limited "
			"(tv).";
		break;
	case BAD_MATRIX:
		*three = "The output is not what the plan says: stream 0: the colour matrix is bt470bg, the plan says bt709.";
		*four = "The picture's colour matrix is not the planned one: the plan says bt709, the output has bt470bg.";
		break;
	case BAD_PRIMARIES:
		*three = "The output is not what the plan says: stream 0: the colour primaries are smpte170m, the plan says "
			"bt709.";
		*four = "The picture's colour primaries are not the planned ones: the plan says bt709, the output has "
			"smpte170m.";
		break;
	case BAD_TRANSFER:
		*three = "The output is not what the plan says: stream 0: the transfer function is bt709, the plan says "
			"unstated.";
		*four = "The picture's transfer function is not the planned one: the plan says unstated, the output has bt709.";
		break;
	default:
		break;
	}
}

/* The plan's arguments with every @find in them made @put (at least one). */
static gchar **
args_replace(
	gchar       **args,
	const gchar  *find,
	const gchar  *put
){
	gchar **out = g_strdupv(args);
	gboolean changed = FALSE;
	guint i;

	for (i = 0; out[i] != NULL; i++) {
		if (strstr(out[i], find) != NULL) {
			g_auto(GStrv) parts = g_strsplit(out[i], find, -1);

			g_free(out[i]);
			out[i] = g_strjoinv(put, parts);
			changed = TRUE;
		}
	}
	if (!changed)
		g_error("the plan has no %s", find);
	return out;
}

static void
one_bad_output(
	Fixture *fx,
	Bad      bad,
	guint    expected
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *movies = g_build_filename(fx->disk, "Movies", NULL);
	g_autofree gchar *original = NULL;
	g_autofree gchar *library = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *out = NULL;
	g_auto(GStrv) args = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 id;
	gint64 size = 0;

	if (bad == BAD_VERDICT)
		prove_on_devices(fx, revision_of(fx, "space_saver"));
	id = requested_and_claimed(fx, clip_for(bad), "space_saver",
		bad == BAD_VERDICT ? AXIS_CONVERSION_REPLACE : AXIS_CONVERSION_TRIAL, &number, &job);
	original = movie_path(fx, number, "mkv");
	out = work_path(fx, job->convert->output);

	switch (bad) {
	case BAD_NAME:
	case BAD_MISSING:
	case BAD_SIZE:
	case BAD_SOURCE:
	case BAD_SOURCE_LINKED:
	case BAD_TRUNCATED:
	case BAD_VERDICT:
		g_assert_true(encode_with(fx, job->convert, job->convert->args, job->convert->output));
		break;
	case BAD_SYMLINK:
		g_assert_cmpint(symlink(original, out), ==, 0);
		break;
	case BAD_HARDLINK:
		g_assert_cmpint(link(original, out), ==, 0);
		break;
	case BAD_NO_AUDIO:
		{
			g_autofree gchar *src = source_of(fx, job->convert);
			const gchar *argv[] = {
				"ffmpeg", "-nostdin", "-v", "error", "-i", src, "-map", "0:0", "-c:0", "libx265", "-preset", "ultrafast",
				"-pix_fmt", "yuv420p10le", "-x265-params", "log-level=error", "-f", "matroska", out, NULL
			};

			g_assert_true(run(argv));
		}
		break;
	case BAD_EXTRA:
		{
			static const gchar *const extra[] = { "-map", "0:1", "-c:2", "copy", NULL };

			args = args_insert(job->convert, extra);
		}
		break;
	case BAD_FLAGS:
		{
			guint i;

			args = g_strdupv(job->convert->args);
			for (i = 0; args[i] != NULL && args[i + 1] != NULL; i++) {
				if (strcmp(args[i], "-disposition:1") == 0) {
					gchar *flipped = g_str_has_prefix(args[i + 1], "+default") ?
						g_strconcat("-default", args[i + 1] + strlen("+default"), NULL) :
						g_strconcat("+default", args[i + 1] + strlen("-default"), NULL);

					g_free(args[i + 1]);
					args[i + 1] = flipped;
				}
			}
		}
		break;
	case BAD_DEPTH:
		args = args_set(job->convert, "-pix_fmt:0", "yuv420p");
		break;
	case BAD_HDR10:
		/* Not only the encoder's parameters: ffmpeg hands x265 the frames'
		 * own HDR10 values, so those go too. */
		{
			guint i;

			args = args_set(job->convert, "-x265-params:0", "log-level=error");
			for (i = 0; args[i] != NULL && args[i + 1] != NULL; i++) {
				if (strcmp(args[i], "-filter:0") == 0) {
					gchar *changed = g_strconcat("sidedata=mode=delete:type=MASTERING_DISPLAY_METADATA,"
						"sidedata=mode=delete:type=CONTENT_LIGHT_LEVEL,", args[i + 1], NULL);

					g_free(args[i + 1]);
					args[i + 1] = changed;
				}
			}
		}
		break;
	case BAD_SHORTER:
		{
			static const gchar *const extra[] = { "-t", "2", NULL };

			args = args_insert(job->convert, extra);
		}
		break;
	case BAD_PICTURE:
		{
			guint i;
			gboolean found = FALSE;

			args = g_strdupv(job->convert->args);
			for (i = 0; args[i] != NULL && args[i + 1] != NULL; i++) {
				if (strcmp(args[i], "-filter:0") == 0) {
					gchar *changed = g_strconcat("negate,", args[i + 1], NULL);

					g_free(args[i + 1]);
					args[i + 1] = changed;
					found = TRUE;
				}
			}
			if (!found) {
				static const gchar *const extra[] = { "-filter:0", "negate", NULL };

				g_strfreev(args);
				args = args_insert(job->convert, extra);
			}
		}
		break;
	case BAD_LARGER:
		args = args_set(job->convert, "-crf:0", "0");
		break;
	/* The plan of a source that states no colour has no colour argument
	 * at all; the others state it on the frames and to x265. */
	case BAD_RANGE_FULL:
		{
			static const gchar *const extra[] = { "-filter:0", "setparams=range=pc", NULL };

			args = args_insert(job->convert, extra);
		}
		break;
	case BAD_TRANSFER:
		{
			static const gchar *const extra[] = { "-filter:0", "setparams=color_trc=bt709", NULL };

			args = args_insert(job->convert, extra);
		}
		break;
	case BAD_RANGE_LIMITED:
		{
			g_auto(GStrv) half = args_replace(job->convert->args, "range=pc", "range=tv");

			args = args_replace(half, "range=full", "range=limited");
		}
		break;
	case BAD_MATRIX:
		{
			g_auto(GStrv) half = args_replace(job->convert->args, "colorspace=bt709", "colorspace=bt470bg");

			args = args_replace(half, "colormatrix=bt709", "colormatrix=bt470bg");
		}
		break;
	case BAD_PRIMARIES:
		{
			g_auto(GStrv) half = args_replace(job->convert->args, "color_primaries=bt709",
				"color_primaries=smpte170m");

			args = args_replace(half, "colorprim=bt709", "colorprim=smpte170m");
		}
		break;
	default:
		g_assert_not_reached();
	}
	if (args != NULL)
		g_assert_true(encode_with(fx, job->convert, args, job->convert->output));

	library = axis_test_storage_digest(movies);
	if (bad == BAD_TRUNCATED) {
		size = size_of(out) * 6 / 10;
		g_assert_cmpint(truncate(out, size), ==, 0);
	} else {
		struct stat st;

		g_assert_cmpint(lstat(out, &st), ==, 0);
		size = (gint64)st.st_size;
	}
	if (bad == BAD_SOURCE) {
		axis_test_set_mtime(original, 1600000000, 0);
		g_free(library);
		library = axis_test_storage_digest(movies);
	}
	if (bad == BAD_SOURCE_LINKED) {
		g_autofree gchar *seeding = g_strdup_printf("%s/seeding-%" G_GINT64_FORMAT ".mkv", fx->t->dir, id);

		g_assert_cmpint(link(original, seeding), ==, 0);
	}

	if (bad == BAD_NAME) {
		g_autofree gchar *other = g_strdup_printf("%" G_GINT64_FORMAT "-9.tmp", id);

		g_assert_cmpuint(complete(fx, job, other, size), ==, 200);
		/* Rejected at once: nothing of the name it gave is touched. */
		assert_rejected_by(fx, id, 1);
		{
			g_autofree gchar *one = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE"
				" conversion_id = %" G_GINT64_FORMAT " AND number = 1", id);
			g_autofree gchar *says = g_strdup_printf("The worker reported a file of another name (\"%s\") than the one "
				"it was given (\"%s\"); nothing it named is touched.", other, job->convert->output);

			g_assert_cmpstr(one, ==, says);
		}
		g_assert_true(exists(out));
		g_assert_true(axis_volume_write_work_name_valid(job->convert->output));
		axis_worker_job_free(job);
		after = axis_test_storage_digest(movies);
		g_assert_cmpstr(library, ==, after);
		return;
	}
	if (bad == BAD_MISSING)
		g_assert_cmpint(g_unlink(out), ==, 0);
	g_assert_cmpuint(complete(fx, job, job->convert->output, bad == BAD_SIZE ? size + 1 : size), ==, 200);
	axis_worker_job_free(job);
	assert_state(fx, id, "verifying");
	if (bad == BAD_VERDICT)
		fx->verdict = AXIS_VERDICT_RISKY;

	finish_ok(fx, id);
	fx->verdict = AXIS_VERDICT_SAFE;
	if (bad == BAD_TRUNCATED) {
		gint64 failed = axis_test_library_n(fx->t, "SELECT failed_check FROM conversion WHERE id = %" G_GINT64_FORMAT,
			id);

		assert_state(fx, id, "rejected");
		g_assert_cmpint(failed, >=, 3);
		g_assert_cmpint(failed, <=, 7);
	} else {
		const gchar *three_says = NULL;
		const gchar *four_says = NULL;

		colour_said(bad, &three_says, &four_says);
		assert_rejected_by(fx, id, expected);
		if (expected == 3) {
			g_autofree gchar *four = check_of(fx, id, 4);

			g_assert_cmpstr(four, ==, bad == BAD_DEPTH || bad == BAD_HDR10 || four_says != NULL ? "failed" :
				"passed");
		}
		if (four_says != NULL) {
			g_autofree gchar *three = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE"
				" conversion_id = %" G_GINT64_FORMAT " AND number = 3", id);
			g_autofree gchar *four = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE"
				" conversion_id = %" G_GINT64_FORMAT " AND number = 4", id);

			g_assert_cmpstr(three, ==, three_says);
			g_assert_cmpstr(four, ==, four_says);
		}
		/* Check 2 says which of the original's figures changed, and how. */
		if (bad == BAD_SOURCE) {
			g_autofree gchar *two = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE"
				" conversion_id = %" G_GINT64_FORMAT " AND number = 2", id);

			g_assert_true(g_str_has_prefix(two, "The original changed since the request: its modification time is "
				"1600000000.000000000, was "));
		}
	}
	/* The original as it was; the output removed when it was a file of
	 * Axis's, a link left where it is. */
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(library, ==, after);
	if (bad == BAD_SYMLINK || bad == BAD_HARDLINK)
		g_assert_true(exists(out));
	else
		g_assert_false(exists(out));
	g_assert_true(g_file_test(original, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE state = 'done'"), ==, 0);
	if (bad == BAD_LARGER) {
		gint64 file = axis_test_library_n(fx->t, "SELECT file_id FROM conversion WHERE id = %" G_GINT64_FORMAT, id);

		g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_memory WHERE file_id = %"
			G_GINT64_FORMAT " AND reason = 'not_worth_it'", file), ==, 1);
		request_refused(fx, file, revision_of(fx, "space_saver"), AXIS_CONVERSION_TRIAL, FALSE,
			"not smaller by the profile's minimum saving");
	}
	g_clear_error(&error);
}

/* Spec: each check rejects an output that is wrong in its way; the original
 * is never touched. */
static void
test_checks_fail(Fixture *fx, gconstpointer data)
{
	static const struct { Bad bad; guint check; } cases[] = {
		{ BAD_NAME, 1 }, { BAD_MISSING, 1 }, { BAD_SIZE, 1 }, { BAD_SYMLINK, 1 }, { BAD_HARDLINK, 1 },
		{ BAD_SOURCE, 2 }, { BAD_SOURCE_LINKED, 2 }, { BAD_NO_AUDIO, 3 }, { BAD_EXTRA, 3 }, { BAD_FLAGS, 3 }, { BAD_DEPTH, 3 },
		{ BAD_HDR10, 3 }, { BAD_TRUNCATED, 0 }, { BAD_SHORTER, 6 }, { BAD_PICTURE, 8 }, { BAD_LARGER, 9 },
		{ BAD_VERDICT, 10 }
	};
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_test_message("case %u", i);
		one_bad_output(fx, cases[i].bad, cases[i].check);
	}
}

/*
 * Spec: the colour the plan carries through is accepted as the output has
 * it, by meaning: a source that states no colour at all (the trial
 * conversions that were rejected were of such sources) comes out of x265
 * with a limited range stated and the rest unstated, 10 bits that the probe
 * does not classify, and passes every check; a source stating limited range
 * and BT.709, and one stating full range, come out as they are and pass.
 */
static void
test_colour_as_planned(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *clip;
		const gchar *says;	/* what check 4 says of the output */
		const gchar *or_says;	/* or this, when the encoder writes no range */
	} cases[] = {
		{ "untagged.mkv", "320x180, 10 bits; colour as planned: primaries unstated, transfer unstated, matrix "
		  "unstated, range limited (tv); SDR as planned (the probe does not classify a picture of more than 8 bits "
		  "that states no transfer function).", "320x180, 10 bits; colour as planned: primaries unstated, transfer "
		  "unstated, matrix unstated, range unstated (limited); SDR as planned (the probe does not classify a "
		  "picture of more than 8 bits that states no transfer function)." },
		{ "movie.mkv", "320x180, 10 bits; colour as planned: primaries bt709, transfer bt709, matrix bt709, range "
		  "limited (tv); SDR.", NULL },
		{ "full-range.mkv", "320x180, 10 bits; colour as planned: primaries bt709, transfer bt709, matrix bt709, "
		  "range full (pc); SDR.", NULL }
	};
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *four = NULL;
		AxisWorkerJob *job;
		guint number;
		gint64 id;

		g_test_message("%s", cases[i].clip);
		id = requested_and_claimed(fx, cases[i].clip, "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
		/* The daemon's arguments, as a worker runs them (x265). */
		g_assert_true(g_strv_contains((const gchar *const *)job->convert->args, "libx265"));
		encode_and_report(fx, job, NULL);
		axis_worker_job_free(job);
		finish_ok(fx, id);
		assert_state(fx, id, "done");
		assert_all_passed(fx, id);
		four = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE conversion_id = %"
			G_GINT64_FORMAT " AND number = 4", id);
		if (g_strcmp0(four, cases[i].says) != 0 && g_strcmp0(four, cases[i].or_says) != 0)
			g_error("%s: check 4 says \"%s\"; expected \"%s\"", cases[i].clip, four, cases[i].says);
	}
}

/*
 * Spec (client-compatibility): A file Axis made from an SDR source is SDR.
 * A real replacement of the clip that states no colour, by Space saver: the
 * conversion records its source's class as the probe was certain of it (SDR)
 * and the class of the picture its plan made (SDR); the new file, probed,
 * is 10 bits without a transfer function, which the probe does not classify;
 * and it is among the files Axis knows to be SDR (conversion_known_sdr),
 * which the player check counts as SDR.
 */
static void
test_replace_untagged_known_sdr(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;
	g_autofree gchar *classes = NULL;
	g_autofree gchar *probed = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	id = requested_and_claimed(fx, "untagged.mkv", "space_saver", AXIS_CONVERSION_REPLACE, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	classes = axis_test_library_q(fx->t, "SELECT source_hdr || ' ' || made_hdr FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id);
	g_assert_cmpstr(classes, ==, "sdr sdr");
	file = axis_test_library_n(fx->t, "SELECT file_id FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(axis_probe_file(fx->probe, file, NULL, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_PROBE_OUTCOME_STORED);
	probed = axis_test_library_q(fx->t, "SELECT bit_depth || ' ' || (hdr_uncertain OR hdr_format IS NULL)"
		" FROM media_info WHERE file_id = %" G_GINT64_FORMAT, file);
	g_assert_cmpstr(probed, ==, "10 true");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_known_sdr WHERE file_id = %"
		G_GINT64_FORMAT, file), ==, 1);
	/* Changed since by anything else, it is no longer what Axis made. */
	g_assert_true(axis_db_pool_exec(fx->t->pool, "UPDATE library_file SET mtime_nsec = (mtime_nsec + 1) % 1000000000",
		0, NULL, NULL, &error));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_known_sdr WHERE file_id = %"
		G_GINT64_FORMAT, file), ==, 0);
}

/* Spec: a real change of the picture's colour is rejected by checks 3 and
 * 4, each saying which property, what the plan says and what the output
 * has: full range against a range the source leaves unstated, limited
 * against full, another matrix, other primaries, a transfer function where
 * the source states none.  The original is never touched. */
static void
test_colour_changed(Fixture *fx, gconstpointer data)
{
	static const Bad cases[] = { BAD_RANGE_FULL, BAD_RANGE_LIMITED, BAD_MATRIX, BAD_PRIMARIES, BAD_TRANSFER };
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_test_message("case %u", i);
		one_bad_output(fx, cases[i], 3);
	}
}

/* ── The copied tracks ──────────────────────────────────────────── */

typedef enum
{
	TRACKS_AS_PLANNED,
	TRACKS_SILENCE,	/* the first copied sound track made silence of the same length */
	TRACKS_SWAPPED,	/* the first two sound tracks (both AAC) exchanged */
	TRACKS_SUBTITLE,	/* the subtitle with one word changed */
	TRACKS_FONT,	/* another font of the same name attached */
	TRACKS_NO_FONT	/* the font left out */
} Tracks;

/* The plan's arguments as a list, to change. */
static GPtrArray *
args_list(const AxisConvertWork *work)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; work->args[i] != NULL; i++)
		g_ptr_array_add(list, g_strdup(work->args[i]));
	return list;
}

static guint
find_pair(
	GPtrArray   *list,
	const gchar *option,
	const gchar *value
){
	guint i;

	for (i = 0; i + 1 < list->len; i++) {
		if (strcmp(g_ptr_array_index(list, i), option) == 0 &&
		    (value == NULL || strcmp(g_ptr_array_index(list, i + 1), value) == 0))
			return i;
	}
	g_error("the plan has no %s %s", option, value != NULL ? value : "");
	return 0;
}

static void
set_at(
	GPtrArray   *list,
	guint        at,
	const gchar *value
){
	g_free(g_ptr_array_index(list, at));
	g_ptr_array_index(list, at) = g_strdup(value);
}

/* Inserts @extra (ended by %NULL) at @at. */
static void
insert_at(
	GPtrArray          *list,
	guint               at,
	const gchar *const *extra
){
	guint k;

	for (k = 0; extra[k] != NULL; k++)
		g_ptr_array_insert(list, (gint)(at + k), g_strdup(extra[k]));
}

static gchar **
list_args(GPtrArray *list)
{
	g_ptr_array_add(list, NULL);
	return (gchar **)g_ptr_array_free(list, FALSE);
}

/* The arguments of @work changed as @how says. */
static gchar **
tracks_args(
	const AxisConvertWork *work,
	Tracks                 how
){
	GPtrArray *list = args_list(work);
	guint input = find_pair(list, "-i", "fd:") + 2;
	g_autofree gchar *other_srt = g_build_filename(clips, "other.srt", NULL);
	g_autofree gchar *other_font = g_build_filename(clips, "other", "font.ttf", NULL);

	switch (how) {
	case TRACKS_SILENCE:
		{
			const gchar *const silence[] = {
				"-t", "4", "-f", "lavfi", "-i", "anullsrc=channel_layout=mono:sample_rate=44100", NULL
			};

			insert_at(list, input, silence);
			set_at(list, find_pair(list, "-map", "0:1") + 1, "1:0");
			set_at(list, find_pair(list, "-c:1", "copy") + 1, "aac");
			{
				const gchar *const language[] = { "-metadata:s:1", "language=eng", NULL };

				insert_at(list, find_pair(list, "-f", "matroska"), language);
			}
		}
		break;
	case TRACKS_SWAPPED:
		set_at(list, find_pair(list, "-map", "0:1") + 1, "0:x");
		set_at(list, find_pair(list, "-map", "0:2") + 1, "0:1");
		set_at(list, find_pair(list, "-map", "0:x") + 1, "0:2");
		break;
	case TRACKS_SUBTITLE:
		{
			const gchar *const other[] = { "-i", other_srt, NULL };
			const gchar *const language[] = { "-metadata:s:4", "language=eng", NULL };

			insert_at(list, input, other);
			set_at(list, find_pair(list, "-map", "0:4") + 1, "1:0");
			insert_at(list, find_pair(list, "-f", "matroska"), language);
		}
		break;
	case TRACKS_FONT:
	case TRACKS_NO_FONT:
		{
			guint at = find_pair(list, "-map", "0:5");
			guint i;

			g_ptr_array_remove_range(list, at, 2);
			/* What the plan says of the attachment's output stream (5). */
			for (i = 0; i + 1 < list->len;) {
				if (g_str_has_suffix(g_ptr_array_index(list, i), ":5") &&
				    ((const gchar *)g_ptr_array_index(list, i))[0] == '-')
					g_ptr_array_remove_range(list, i, 2);
				else
					i++;
			}
			if (how == TRACKS_FONT) {
				const gchar *const font[] = {
					"-attach", other_font, "-metadata:s:5", "mimetype=application/x-truetype-font", NULL
				};

				insert_at(list, find_pair(list, "-f", "matroska"), font);
			}
		}
		break;
	default:
		break;
	}
	return list_args(list);
}

static gint64
one_tracks_case(
	Fixture     *fx,
	const gchar *clip,
	const gchar *slug,
	Tracks       how
){
	g_autofree gchar *movies = g_build_filename(fx->disk, "Movies", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *out = NULL;
	g_auto(GStrv) args = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 id;

	g_test_message("tracks case %d with %s", how, slug);
	id = requested_and_claimed(fx, clip, slug, AXIS_CONVERSION_TRIAL, &number, &job);
	out = work_path(fx, job->convert->output);
	before = axis_test_storage_digest(movies);
	args = tracks_args(job->convert, how);
	if (!encode_with(fx, job->convert, args, job->convert->output))
		g_error("the changed encode failed");
	g_assert_cmpuint(complete(fx, job, job->convert->output, size_of(out)), ==, 200);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(before, ==, after);
	return id;
}

/* Spec: every copied track is the original's, bit for bit: MKV to MKV and
 * MKV to MP4 with AAC, AC-3 (and in Matroska a subtitle and a font) pass;
 * silence of the same length, another track of the same codec, a changed
 * subtitle and another font of the same name are each rejected by check 7,
 * with the original unchanged; a font left out is rejected by check 3,
 * which sees one stream less. */
static void
test_copied_tracks(Fixture *fx, gconstpointer data)
{
	static const struct { Tracks how; guint check; } bad[] = {
		{ TRACKS_SILENCE, 7 }, { TRACKS_SWAPPED, 7 }, { TRACKS_SUBTITLE, 7 }, { TRACKS_FONT, 7 }, { TRACKS_NO_FONT, 3 }
	};
	g_autofree gchar *detail = NULL;
	gint64 id;
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);

	/* Matroska to Matroska: three sound tracks, a subtitle and a font
	 * copied. */
	id = one_tracks_case(fx, "tracks.mkv", "space_saver", TRACKS_AS_PLANNED);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	detail = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE number = 7 AND conversion_id = %"
		G_GINT64_FORMAT, id);
	g_assert_cmpstr(detail, ==, "Bit for bit the original's (every packet's data, in order): 3 sound tracks, "
		"1 subtitle track, 1 attachment.");
	g_free(detail);

	/* Matroska to MP4: the AAC and AC-3 tracks copied (the first is AAC of
	 * two channels or fewer already, so no stereo track is added); the
	 * subtitle made anew in MP4's own text format, and said so. */
	id = one_tracks_case(fx, "tracks-mp4.mkv", "max_compat", TRACKS_AS_PLANNED);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	detail = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE number = 7 AND conversion_id = %"
		G_GINT64_FORMAT, id);
	g_assert_cmpstr(detail, ==, "Bit for bit the original's (every packet's data, in order): 3 sound tracks. "
		"Made anew, so compared by the streams, the length and a clean decode only, not bit for bit: 1 subtitle "
		"track in the container's own text format.");

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autofree gchar *output = NULL;

		id = one_tracks_case(fx, "tracks.mkv", "space_saver", bad[i].how);
		assert_rejected_by(fx, id, bad[i].check);
		output = axis_test_library_q(fx->t, "SELECT output_name FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
		{
			g_autofree gchar *path = work_path(fx, output);

			g_assert_false(exists(path));
		}
	}
}

/* Spec: a name that would reach outside the work area is refused by the
 * protocol itself. */
static void
test_path_trickery(Fixture *fx, gconstpointer data)
{
	static const gchar *const names[] = {
		"../Movies/x.mkv", "/etc/passwd", "1-1.tmp/../../x", "01-1.tmp", ".axis-partial-1", "trial-1.mkv", "", NULL
	};
	g_autofree gchar *movies = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 id;
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	before = axis_test_storage_digest(movies);
	for (i = 0; names[i] != NULL; i++)
		g_assert_cmpuint(complete(fx, job, names[i], 1000), ==, 400);
	assert_state(fx, id, "encoding");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND state = 'running'"), ==, 1);
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(before, ==, after);
	axis_worker_job_free(job);
}

/* ── The gates ──────────────────────────────────────────────────── */

/* Spec: every gate at request, each refusing on its own, nothing stored. */
static void
test_gates_at_request(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionPreview) preview = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *linked = NULL;
	g_autofree gchar *srt_rel = NULL;
	gint64 space_saver;
	gint64 max_compat;
	gint64 file;
	gint64 other;
	gint64 id;
	guint number;
	guint more;

	SKIP_UNLESS_READY(fx);
	space_saver = revision_of(fx, "space_saver");
	max_compat = revision_of(fx, "max_compat");
	file = add_movie(fx, "movie.mkv", &number);

	/* No registered worker: no plan. */
	request_refused(fx, file, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "no registered conversion worker");
	do_register(fx);

	/* The preview: every gate, with whether it applies. */
	g_assert_true(axis_conversions_preview(fx->conv, file, space_saver, AXIS_CONVERSION_TRIAL, &preview, &error));
	g_assert_no_error(error);
	g_assert_true(preview->ok);
	g_assert_cmpuint(preview->gates->len, >=, 13);
	g_assert_nonnull(preview->encoder);
	g_assert_cmpstr(preview->encoder, ==, "libx265");
	g_assert_true(preview->estimated);
	g_clear_pointer(&preview, axis_conversion_preview_free);

	/* Storage read-only (the default). */
	read_only(fx, TRUE);
	request_refused(fx, file, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "Read-only storage is on");
	g_assert_true(axis_conversions_preview(fx->conv, file, space_saver, AXIS_CONVERSION_TRIAL, &preview, &error));
	g_assert_false(preview->ok);
	g_assert_cmpstr(axis_conversion_preview_refusal(preview)->id, ==, "read_only");
	g_clear_pointer(&preview, axis_conversion_preview_free);
	read_only(fx, FALSE);

	/* A replacement: needs the box ticked, a proven revision and a safe
	 * verdict. */
	request_refused(fx, file, space_saver, AXIS_CONVERSION_REPLACE, FALSE, "needs the confirmation ticked");
	request_refused(fx, file, space_saver, AXIS_CONVERSION_REPLACE, TRUE, "It is locked");
	prove_on_devices(fx, space_saver);
	fx->verdict = AXIS_VERDICT_UNKNOWN;
	request_refused(fx, file, space_saver, AXIS_CONVERSION_REPLACE, TRUE, "The verdict is unknown");
	fx->verdict = AXIS_VERDICT_RISKY;
	request_refused(fx, file, space_saver, AXIS_CONVERSION_REPLACE, TRUE, "The verdict is risky");
	fx->verdict = AXIS_VERDICT_SAFE;
	/* A trial needs neither. */
	fx->verdict = AXIS_VERDICT_UNKNOWN;
	g_assert_true(axis_conversions_preview(fx->conv, file, max_compat, AXIS_CONVERSION_TRIAL, &preview, &error));
	g_assert_true(preview->ok);
	g_clear_pointer(&preview, axis_conversion_preview_free);
	fx->verdict = AXIS_VERDICT_SAFE;

	/* An audio profile; no such revision; not a video file. */
	request_refused(fx, file, revision_of(fx, "music_space_saver"), AXIS_CONVERSION_TRIAL, FALSE,
		"Axis does not convert audio files yet");
	{
		gint64 nothing = 0;

		g_assert_false(axis_conversions_request(fx->conv, file, 999999, AXIS_CONVERSION_TRIAL, FALSE, "root", &nothing,
			&error));
		g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND);
		g_clear_error(&error);
		g_assert_false(axis_conversions_request(fx->conv, 999999, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "root",
			&nothing, &error));
		g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND);
		g_clear_error(&error);
	}
	{
		g_autofree gchar *srt = movie_path(fx, number, "en.srt");

		axis_test_storage_write(srt, "1\n00:00:00,000 --> 00:00:01,000\nHello\n");
		scan(fx);
		srt_rel = movie_rel(number, "en.srt");
		request_refused(fx, axis_test_scenario_file(fx->t, fx->movies, srt_rel), space_saver, AXIS_CONVERSION_TRIAL,
			FALSE, "not a video file");
	}

	/* A file with another hard link (a download client seeding it). */
	other = add_movie(fx, "movie.mkv", &more);
	path = movie_path(fx, more, "mkv");
	linked = g_build_filename(fx->t->dir, "seeding.mkv", NULL);
	g_assert_cmpint(link(path, linked), ==, 0);
	scan(fx);
	request_refused(fx, other, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "It is pinned");
	g_assert_cmpint(g_unlink(linked), ==, 0);
	scan(fx);
	probe_file(fx, other);

	/* The file changed since the last scan. */
	axis_test_set_mtime(path, 1600000000, 0);
	request_refused(fx, other, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "not what the index says");
	scan(fx);
	probe_file(fx, other);

	/* The new name is taken (another container). */
	{
		g_autofree gchar *mp4 = movie_path(fx, more, "mp4");

		prove_on_devices(fx, max_compat);
		axis_test_storage_write(mp4, "not a movie");
		request_refused(fx, other, max_compat, AXIS_CONVERSION_REPLACE, TRUE, "A file of the new name");
		scan(fx);
		request_refused(fx, other, max_compat, AXIS_CONVERSION_REPLACE, TRUE, "The index has a file of the new name");
		g_assert_cmpint(g_unlink(mp4), ==, 0);
		scan(fx);
	}

	/* Not enough space: the floor above what the volume has. */
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE, "1000TiB");
	request_refused(fx, other, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "Enough free space");
	set(fx, AXIS_CONVERSION_SETTING_MIN_FREE, "1MiB");

	/* A network filesystem would need convert.allow_network_fs: tmpfs is
	 * local, so the gate holds here. */
	g_assert_true(axis_conversions_preview(fx->conv, other, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
	g_assert_true(preview->ok);
	g_clear_pointer(&preview, axis_conversion_preview_free);

	/* One in flight per file. */
	id = request(fx, other, "space_saver", AXIS_CONVERSION_TRIAL);
	request_refused(fx, other, space_saver, AXIS_CONVERSION_TRIAL, FALSE, "in flight");
	/* Cancelled, it frees the file. */
	g_assert_true(axis_conversions_cancel(fx->conv, id, "root", &error));
	g_assert_no_error(error);
	assert_state(fx, id, "cancelled");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND state = 'cancelled'"), ==, 1);
	g_assert_false(axis_conversions_cancel(fx->conv, id, "root", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE);
	g_clear_error(&error);
	g_assert_cmpint(request(fx, other, "space_saver", AXIS_CONVERSION_TRIAL), >, id);
}

/*
 * A file whose link is kept (by hand, or brought over from Radarr or Sonarr)
 * is not replaced by a conversion until a person lets it go: the gate "kept"
 * refuses a replacement, for a movie's file and an episode's alike, and not a
 * trial (nothing is replaced); letting it go makes the file eligible again.
 */
static void
test_gate_kept(Fixture *fx, gconstpointer data)
{
	static const gchar *const kept_text = "Kept: Axis does not replace this file. Let Axis upgrade it again on its links "
		"page to allow a conversion.";
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionPreview) preview = NULL;
	gint64 space_saver;
	gint64 film;
	gint64 episode;
	gint64 plain;
	guint number;
	guint i;

	SKIP_UNLESS_READY(fx);
	space_saver = revision_of(fx, "space_saver");
	do_register(fx);
	prove_on_devices(fx, space_saver);
	film = add_movie(fx, "movie.mkv", &number);
	episode = add_movie(fx, "movie.mkv", &number);
	plain = add_movie(fx, "movie.mkv", &number);
	sql(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Kept Film', 2001,"
		" 'fixture', 'kept-film'), ('series', 'Kept Show', 2002, 'fixture', 'kept-show')");
	sql(fx, "INSERT INTO movie (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider_id = 'kept-film'");
	sql(fx, "INSERT INTO series (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider_id = 'kept-show'");

	/* Linked, and not kept: eligible. */
	{
		g_autofree gchar *statement = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, kept)"
			" SELECT %" G_GINT64_FORMAT ", id, 'webdl_1080p', false FROM movie", film);

		sql(fx, statement);
		g_free(statement);
		statement = g_strdup_printf("INSERT INTO series_file (file_id, series_id, quality, kept)"
			" SELECT %" G_GINT64_FORMAT ", id, 'webdl_1080p', false FROM series", episode);
		sql(fx, statement);
	}
	for (i = 0; i < 3; i++) {
		gint64 file = i == 0 ? film : i == 1 ? episode : plain;

		g_assert_true(axis_conversions_preview(fx->conv, file, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
		g_assert_no_error(error);
		g_assert_true(preview->ok);
		g_clear_pointer(&preview, axis_conversion_preview_free);
	}

	/* Kept by hand (a movie) and brought over (an episode). */
	{
		g_autofree gchar *a = g_strdup_printf("UPDATE movie_file SET kept = true WHERE file_id = %" G_GINT64_FORMAT, film);
		g_autofree gchar *b = g_strdup_printf("UPDATE series_file SET kept = true, kept_how = 'brought' WHERE file_id = %"
			G_GINT64_FORMAT, episode);

		sql(fx, a);
		sql(fx, b);
	}
	request_refused(fx, film, space_saver, AXIS_CONVERSION_REPLACE, TRUE, kept_text);
	request_refused(fx, episode, space_saver, AXIS_CONVERSION_REPLACE, TRUE, kept_text);
	g_assert_true(axis_conversions_preview(fx->conv, film, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
	g_assert_no_error(error);
	g_assert_false(preview->ok);
	g_assert_cmpstr(axis_conversion_preview_refusal(preview)->id, ==, "kept");
	g_clear_pointer(&preview, axis_conversion_preview_free);
	/* A file with no link at all is not kept. */
	g_assert_true(axis_conversions_preview(fx->conv, plain, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
	g_assert_true(preview->ok);
	g_clear_pointer(&preview, axis_conversion_preview_free);
	/* A trial replaces nothing: the gate does not apply. */
	g_assert_true(axis_conversions_preview(fx->conv, film, space_saver, AXIS_CONVERSION_TRIAL, &preview, &error));
	g_assert_true(preview->ok);
	g_assert_true(preview->kept);
	g_clear_pointer(&preview, axis_conversion_preview_free);

	/* (convert-kept-files.)  With the choice to convert kept files the gate
	 * holds, and says that the file stays kept; it lifts that gate only: a
	 * version that is not proven refuses the kept file as it refuses any. */
	for (i = 0; i < 2; i++) {
		const AxisConversionGate *gate = NULL;
		guint k;

		g_assert_true(axis_conversions_preview_full(fx->conv, i == 0 ? film : episode, space_saver,
			AXIS_CONVERSION_REPLACE, TRUE, &preview, &error));
		g_assert_no_error(error);
		g_assert_true(preview->ok);
		g_assert_true(preview->kept);
		g_assert_true(preview->convert_kept);
		for (k = 0; k < preview->gates->len; k++) {
			if (strcmp(((const AxisConversionGate *)g_ptr_array_index(preview->gates, k))->id, "kept") == 0)
				gate = g_ptr_array_index(preview->gates, k);
		}
		g_assert_nonnull(gate);
		g_assert_true(gate->applies && gate->passed);
		g_assert_nonnull(strstr(gate->detail, "stays kept"));
		g_clear_pointer(&preview, axis_conversion_preview_free);
	}
	{
		gint64 nothing = 0;

		g_assert_false(axis_conversions_request_full(fx->conv, film, revision_of(fx, "max_compat"),
			AXIS_CONVERSION_REPLACE, TRUE, TRUE, "root", &nothing, &error));
		g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
		g_assert_nonnull(strstr(error->message, "The profile version is proven"));
		g_clear_error(&error);
		g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion"), ==, 0);
	}
	/* Nothing above changed a mark. */
	g_assert_cmpint(n(fx, "SELECT (SELECT count(*) FROM movie_file WHERE kept) + (SELECT count(*) FROM series_file"
		" WHERE kept)"), ==, 2);

	/* A person lets the file go: eligible again. */
	{
		g_autofree gchar *a = g_strdup_printf("UPDATE movie_file SET kept = false, kept_how = NULL WHERE file_id = %"
			G_GINT64_FORMAT, film);

		sql(fx, a);
	}
	g_assert_true(axis_conversions_preview(fx->conv, film, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
	g_assert_true(preview->ok);
	g_clear_pointer(&preview, axis_conversion_preview_free);
	g_assert_cmpint(request(fx, film, "space_saver", AXIS_CONVERSION_REPLACE), >, 0);
}

/*
 * Spec (conversion-execution, A kept file is converted only by choice): a
 * kept movie file asked for with the choice is converted by a real worker,
 * passes the same ten checks (the tenth works the conditions out again, with
 * the choice the conversion recorded) and takes its original's place; the
 * file is as kept afterwards as it was before, in the same link, and a later
 * request without the choice is refused by the condition "kept" as before.
 * A conversion asked for without the choice, of a file that is kept by the
 * time it is checked, is rejected by check 10 as before.
 */
static void
test_replace_kept_by_choice(Fixture *fx, gconstpointer data)
{
	static const gchar *const kept_text = "Kept: Axis does not replace this file.";
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionPreview) preview = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *detail = NULL;
	AxisTestWorker *w;
	guint number;
	guint other_number;
	gint64 space_saver;
	gint64 file;
	gint64 other;
	gint64 before_ino;
	gint64 id = 0;
	gint64 plain;
	guint k;

	SKIP_UNLESS_READY(fx);
	space_saver = revision_of(fx, "space_saver");
	file = add_movie(fx, "movie.mkv", &number);
	other = add_movie(fx, "movie.mkv", &other_number);
	path = movie_path(fx, number, "mkv");
	before_ino = inode_of(path);
	prove_on_devices(fx, space_saver);
	sql(fx, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES ('film', 'Kept Film', 2001,"
		" 'fixture', 'kept-film'), ('film', 'Other Film', 2002, 'fixture', 'other-film')");
	sql(fx, "INSERT INTO movie (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM quality_profile)"
		" FROM meta_title t WHERE t.provider = 'fixture'");
	link = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, kept, kept_how) SELECT %" G_GINT64_FORMAT
		", m.id, 'webdl_1080p', true, 'brought' FROM movie m JOIN meta_title t ON t.id = m.title_id"
		" WHERE t.provider_id = 'kept-film'", file);
	sql(fx, link);
	g_clear_pointer(&link, g_free);
	link = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, kept) SELECT %" G_GINT64_FORMAT
		", m.id, 'webdl_1080p', false FROM movie m JOIN meta_title t ON t.id = m.title_id"
		" WHERE t.provider_id = 'other-film'", other);
	sql(fx, link);

	w = start_worker(fx);
	/* Not without the choice. */
	request_refused(fx, file, space_saver, AXIS_CONVERSION_REPLACE, TRUE, kept_text);
	if (!axis_conversions_request_full(fx->conv, file, space_saver, AXIS_CONVERSION_REPLACE, TRUE, TRUE, "root", &id,
	        &error))
		g_error("the request with the choice was refused: %s", error->message);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND convert_kept AND outcome = 'replace'", id), ==, 1);
	wait_state(fx, w, id, "verifying");
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	detail = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE conversion_id = %" G_GINT64_FORMAT
		" AND number = 10", id);
	g_assert_cmpstr(detail, ==, "Every gate of the request holds now.");
	/* Replaced in place: another file under the same name, the same row. */
	g_assert_cmpint(inode_of(path), !=, before_ino);
	g_assert_cmpint(inode_of(path), ==, axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
	/* Still kept, as it was, in the same link. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM movie_file WHERE file_id = %" G_GINT64_FORMAT
		" AND kept AND kept_how = 'brought'", file), ==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM movie_file WHERE kept"), ==, 1);
	/* And what reads the mark reads it as before: the condition refuses
	 * without the choice. */
	probe_file(fx, file);
	g_assert_true(axis_conversions_preview(fx->conv, file, space_saver, AXIS_CONVERSION_REPLACE, &preview, &error));
	g_assert_no_error(error);
	g_assert_true(preview->kept);
	for (k = 0; k < preview->gates->len; k++) {
		const AxisConversionGate *gate = g_ptr_array_index(preview->gates, k);

		if (strcmp(gate->id, "kept") == 0)
			g_assert_true(gate->applies && !gate->passed);
	}

	/* A file that becomes kept while its conversion, asked for without the
	 * choice, is in flight: rejected by check 10, the original as it was. */
	plain = request(fx, other, "space_saver", AXIS_CONVERSION_REPLACE);
	wait_state(fx, w, plain, "verifying");
	g_clear_pointer(&link, g_free);
	link = g_strdup_printf("UPDATE movie_file SET kept = true WHERE file_id = %" G_GINT64_FORMAT, other);
	sql(fx, link);
	finish_ok(fx, plain);
	stop_worker(fx, w);
	assert_rejected_by(fx, plain, 10);
	g_clear_pointer(&detail, g_free);
	detail = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE conversion_id = %" G_GINT64_FORMAT
		" AND number = 10", plain);
	g_assert_nonnull(strstr(detail, kept_text));
}

/* Spec: the gates again when a worker takes the work. */
static void
test_gates_at_claim(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *why = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	file = add_movie(fx, "movie.mkv", &number);
	path = movie_path(fx, number, "mkv");
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);

	/* Read-only for now: not given, tried again later. */
	read_only(fx, TRUE);
	job = claim(fx);
	g_assert_null(job);
	assert_state(fx, id, "queued");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND state = 'queued'"
		" AND last_error LIKE '%Read-only storage is on%'"), ==, 1);
	read_only(fx, FALSE);

	/* The file changed since the request: refused for good. */
	axis_test_set_mtime(path, 1600000000, 0);
	scan(fx);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'convert.encode'"
		" AND state = 'queued' AND run_at <= now()", "1", AXIS_TEST_WAIT_MS));
	job = claim(fx);
	g_assert_null(job);
	assert_state(fx, id, "refused");
	why = axis_test_library_q(fx->t, "SELECT result_text FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(why, "Refused when a worker took it"));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND state = 'failed'"), ==, 1);
	(void)file;
}

/* ── Stopping at every step ─────────────────────────────────────── */

static gchar *
file_checksum(const gchar *path)
{
	g_autofree gchar *content = NULL;
	gsize length = 0;

	g_assert_true(g_file_get_contents(path, &content, &length, NULL));
	return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)content, length);
}

static AxisWorkerJob *claim_checks(Fixture *fx, const gchar *token);
static AxisVerifyResult *measure_as_worker(Fixture *fx, const AxisWorkerJob *job);
static guint complete_checks(Fixture *fx, const gchar *token, const AxisWorkerJob *job, const AxisVerifyResult *result);

/* With @on_worker, the checks' measurements are a worker's (handed out,
 * measured and reported before the turn that is stopped). */
static void
stop_and_recover(
	Fixture     *fx,
	const gchar *slug,
	const gchar *step,
	const gchar *extension,
	gboolean     on_worker
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *original = NULL;
	g_autofree gchar *final = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *output_checksum = NULL;
	g_autofree gchar *folder = NULL;
	g_autofree gchar *names = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *recycle = NULL;
	g_autofree gchar *work = NULL;
	g_autofree gchar *left = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 id;

	g_test_message("stopping %s at %s%s", slug, step, on_worker ? ", checked on a worker" : "");
	id = requested_and_claimed(fx, "movie.mkv", slug, AXIS_CONVERSION_REPLACE, &number, &job);
	original = movie_path(fx, number, "mkv");
	final = movie_path(fx, number, extension);
	checksum = file_checksum(original);
	encode_and_report(fx, job, NULL);
	{
		g_autofree gchar *out = work_path(fx, job->convert->output);

		output_checksum = file_checksum(out);
	}
	axis_worker_job_free(job);
	if (on_worker) {
		g_autoptr(AxisVerifyResult) result = NULL;
		AxisWorkerJob *checks;

		finish_ok(fx, id);
		assert_state(fx, id, "verifying");
		checks = claim_checks(fx, fx->token);
		result = measure_as_worker(fx, checks);
		g_assert_cmpuint(complete_checks(fx, fx->token, checks, result), ==, 200);
		axis_worker_job_free(checks);
	}

	g_string_truncate(fx->steps, 0);
	fx->stop_at = step;
	fx->stop_how = AXIS_CONVERSION_HOOK_CRASH;
	g_assert_false(finish(fx, id, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY);
	g_clear_error(&error);
	if (fx->stop_at != NULL)
		g_error("the step %s was not reached: %s", step, fx->steps->str);

	/* Stopped there: a whole file under the library name. */
	{
		gint64 src = axis_test_library_n(fx->t, "SELECT src_inode FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
		gint64 out = axis_test_library_n(fx->t, "SELECT coalesce(output_inode, -1) FROM conversion WHERE id = %"
			G_GINT64_FORMAT, id);
		gboolean has_original = exists(original) && inode_of(original) == src;
		gboolean has_new = exists(final) && inode_of(final) == out;

		if (has_original) {
			g_autofree gchar *now = file_checksum(original);

			g_assert_cmpstr(now, ==, checksum);
		} else if (has_new) {
			g_autofree gchar *now = file_checksum(final);

			g_assert_cmpstr(now, ==, output_checksum);
		} else {
			g_error("stopped at %s: neither the original nor the new file is under a library name", step);
		}
	}

	/* The next run finishes it from the records and the disk (an original
	 * whose removal was cut short goes with the tidy job). */
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	{
		AxisConversionTidyReport report;

		g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.originals, ==, strcmp(step, "remove-original") == 0 ? 1 : 0);
	}
	{
		g_autofree gchar *now = file_checksum(final);

		g_assert_cmpstr(now, ==, output_checksum);
	}
	folder = g_path_get_dirname(original);
	names = listing(folder);
	expected = g_strdup_printf("Movie %u (2001).%s", number, extension);
	g_assert_cmpstr(names, ==, expected);
	recycle = g_build_filename(fx->disk, AXIS_VOLUME_RECYCLE_DIR, NULL);
	g_assert_cmpuint(count_files(recycle), ==, 0);
	work = work_path(fx, "");
	left = listing(work);
	g_assert_cmpstr(left, ==, "");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE recycle_state = 'kept'"), ==, 0);
}

/* Spec: a daemon that stops before or after any step of a replacement that
 * keeps the name finishes it at its next run. */
static void
test_recover_same_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = {
		"settle", "settle-done", "placing", "adopt", "adopt-done", "link", "link-done", "swap", "swap-done", "unlink",
		"unlink-done", "record", "remove-original", NULL
	};
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	for (i = 0; steps[i] != NULL; i++)
		stop_and_recover(fx, "space_saver", steps[i], "mkv", FALSE);
}

/* Spec: the same for a replacement into another container. */
static void
test_recover_new_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const steps[] = {
		"settle", "settle-done", "placing", "adopt", "adopt-done", "publish", "publish-done", "retire", "retire-done",
		"record", "remove-original", NULL
	};
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "max_compat"));
	for (i = 0; steps[i] != NULL; i++)
		stop_and_recover(fx, "max_compat", steps[i], "mp4", FALSE);
}

/* Spec (conversion-execution, Checks that passed are not judged again): the
 * same, with the checks measured on a worker: a finish job tried again after
 * the output was settled places the file whose checks passed (it does not
 * bind the measurements again to the stamp the output had when they were
 * handed out, which settling changed). */
static void
test_recover_on_worker(Fixture *fx, gconstpointer data)
{
	static const gchar *const same[] = {
		"settle", "settle-done", "placing", "adopt", "adopt-done", "link", "link-done", "swap", "swap-done", "unlink",
		"unlink-done", "record", "remove-original", NULL
	};
	static const gchar *const other[] = {
		"settle", "settle-done", "placing", "adopt", "publish", "publish-done", "retire", "record", NULL
	};
	guint i;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	prove_on_devices(fx, revision_of(fx, "max_compat"));
	for (i = 0; same[i] != NULL; i++)
		stop_and_recover(fx, "space_saver", same[i], "mkv", TRUE);
	for (i = 0; other[i] != NULL; i++)
		stop_and_recover(fx, "max_compat", other[i], "mp4", TRUE);
}

/* Spec (conversion-execution, Replacement is atomic or recoverable): a
 * replacement into another container that stopped after the new file was
 * published, and whose original was changed in place meanwhile (the same
 * size; a tag editor, say), keeps both files and says so: the changed
 * original is not retired, let alone removed. */
static void
test_recover_original_changed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *original = NULL;
	g_autofree gchar *final = NULL;
	g_autofree gchar *changed = NULL;
	g_autofree gchar *text = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 size;
	gint64 id;
	gint fd;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "max_compat"));
	id = requested_and_claimed(fx, "movie.mkv", "max_compat", AXIS_CONVERSION_REPLACE, &number, &job);
	original = movie_path(fx, number, "mkv");
	final = movie_path(fx, number, "mp4");
	size = size_of(original);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	fx->stop_at = "publish-done";
	fx->stop_how = AXIS_CONVERSION_HOOK_CRASH;
	g_assert_false(finish(fx, id, &error));
	g_clear_error(&error);
	g_assert_true(exists(final));
	g_assert_true(exists(original));

	/* Changed in place while the daemon was down: the same inode and size. */
	fd = open(original, O_WRONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(pwrite(fd, "changed", 7, size / 2), ==, 7);
	g_assert_cmpint(close(fd), ==, 0);
	changed = file_checksum(original);

	finish_ok(fx, id);
	text = axis_test_library_q(fx->t, "SELECT state || ': ' || coalesce(result_text, '') FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id);
	g_test_message("conversion %" G_GINT64_FORMAT ": %s", id, text);
	/* Both files are there; the original as it was changed. */
	g_assert_true(exists(final));
	g_assert_true(exists(original));
	{
		g_autofree gchar *now = file_checksum(original);

		g_assert_cmpstr(now, ==, changed);
	}
	g_assert_cmpint(size_of(original), ==, size);
	assert_state(fx, id, "failed");
	g_assert_nonnull(strstr(text, "changed"));
}

/* ── The volume away, the scan's lock ───────────────────────────── */

static void
unplug(Fixture *fx)
{
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
}

/* Spec: the volume goes away before or during placement: nothing is
 * concluded from it; when it is back, the placement ends. */
static void
test_volume_away(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));

	/* Away before the checks: tried again later, nothing recorded. */
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_REPLACE, &number, &job);
	path = movie_path(fx, number, "mkv");
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	unplug(fx);
	g_assert_false(finish(fx, id, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY);
	g_clear_error(&error);
	assert_state(fx, id, "verifying");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_check WHERE conversion_id = %"
		G_GINT64_FORMAT, id), ==, 0);
	axis_test_library_plug(fx->t, fx->disk, "disk1");

	/* Away in the middle of the placement, just before the exchange. */
	fx->stop_at = "swap";
	fx->stop_how = AXIS_CONVERSION_HOOK_CONTINUE;
	fx->at_stop = unplug;
	g_assert_false(finish(fx, id, &error));
	g_clear_error(&error);
	fx->at_stop = NULL;
	assert_state(fx, id, "placing");
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	/* The original under its name, untouched. */
	g_assert_cmpint(inode_of(path), ==, axis_test_library_n(fx->t, "SELECT src_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	g_assert_cmpint(inode_of(path), ==, axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
}

static gboolean
scan_lock_free(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *volume = g_strdup_printf("%" G_GINT64_FORMAT, fx->volume);
	const gchar *params[1];
	gboolean got = FALSE;
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);

	g_assert_nonnull(conn);
	params[0] = volume;
	g_assert_true(axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		&result, &error));
	axis_db_result_get_boolean(result, 0, 0, &got);
	if (got)
		g_assert_true(axis_db_conn_exec(conn, "SELECT pg_advisory_unlock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
			NULL, &error));
	axis_db_pool_release(fx->t->pool, conn);
	return got;
}

static void
look_at_lock(Fixture *fx)
{
	fx->lock_was_free = scan_lock_free(fx);
}

/* Spec: a placement waits for a scan of its volume, and no scan runs while
 * a placement changes the library. */
static void
test_scan_lock(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *volume = NULL;
	const gchar *params[1];
	AxisWorkerJob *job;
	AxisDbConn *scanner;
	guint number;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_REPLACE, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);

	/* A scan holds the volume: the placement waits, then gives up for now. */
	scanner = axis_db_pool_acquire(fx->t->pool, &error);
	volume = g_strdup_printf("%" G_GINT64_FORMAT, fx->volume);
	params[0] = volume;
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_lock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	g_assert_false(finish(fx, id, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY);
	g_assert_nonnull(strstr(error->message, "scan"));
	g_clear_error(&error);
	assert_state(fx, id, "verifying");
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_unlock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	axis_db_pool_release(fx->t->pool, scanner);

	/* During the exchange, a scan could not start. */
	fx->stop_at = "swap";
	fx->stop_how = AXIS_CONVERSION_HOOK_CONTINUE;
	fx->at_stop = look_at_lock;
	fx->lock_was_free = TRUE;
	finish_ok(fx, id);
	fx->at_stop = NULL;
	assert_state(fx, id, "done");
	g_assert_false(fx->lock_was_free);
	g_assert_true(scan_lock_free(fx));
}

/* ── Tidying ────────────────────────────────────────────────────── */

/* Spec: what the work area holds that no conversion accounts for is listed,
 * and removed after the grace time when it is a file of Axis's. */
static void
test_leftovers(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	g_autofree gchar *ours = NULL;
	g_autofree gchar *trial = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *busy = NULL;
	AxisConversionTidyReport report;
	AxisWorkerJob *job;
	guint number;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	/* One being encoded: its output is accounted for. */
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	busy = work_path(fx, job->convert->output);
	axis_test_storage_write(busy, "half an encode");
	ours = work_path(fx, "77-1.tmp");
	trial = work_path(fx, "trial-78.mkv");
	notes = work_path(fx, "notes.txt");
	axis_test_storage_write(ours, "left by a worker that died");
	axis_test_storage_write(trial, "a trial nobody records");
	axis_test_storage_write(notes, "someone's");

	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.leftovers, ==, 3);
	g_assert_cmpuint(report.removed, ==, 0);
	g_assert_true(axis_conversions_leftovers(fx->conv, &listed, &error));
	g_assert_cmpuint(listed->len, ==, 3);
	g_clear_pointer(&listed, g_ptr_array_unref);

	/* After the grace time: Axis's names go, someone's file stays, the one
	 * being encoded stays. */
	sql(fx, "UPDATE conversion_leftover SET first_seen_at = now() - interval '2 days'");
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_cmpuint(report.removed, ==, 2);
	g_assert_false(exists(ours));
	g_assert_false(exists(trial));
	g_assert_true(exists(notes));
	g_assert_true(exists(busy));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_leftover"), ==, 1);

	/* A leftover replaced by another file of the same name is new again. */
	axis_test_storage_write(ours, "another");
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	sql(fx, "UPDATE conversion_leftover SET first_seen_at = now() - interval '2 days', inode = inode + 1"
		" WHERE name = '77-1.tmp'");
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_true(exists(ours));
	g_assert_cmpuint(report.removed, ==, 0);

	/* Read-only storage: the tidy job removes nothing. */
	sql(fx, "UPDATE conversion_leftover SET first_seen_at = now() - interval '2 days'");
	read_only(fx, TRUE);
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_true(exists(ours));
	read_only(fx, FALSE);
	axis_worker_job_free(job);
	(void)id;
}

/* Spec: kept trials and recycled originals go after their time, by the inode
 * and size recorded; an encode that ended without a result is recorded;
 * a conversion left being checked gets its finish job again. */
static void
test_tidy_after_time(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *trial = NULL;
	g_autofree gchar *recycled = NULL;
	AxisConversionTidyReport report;
	AxisWorkerJob *job;
	guint number;
	gint64 trial_id;
	gint64 replace_id;
	gint64 lost;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	set(fx, AXIS_CONVERSION_SETTING_KEEP_ORIGINAL, "1d");
	prove_on_devices(fx, revision_of(fx, "space_saver"));

	trial_id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, trial_id);
	assert_state(fx, trial_id, "done");
	trial = axis_test_library_q(fx->t, "SELECT trial_name FROM conversion WHERE id = %" G_GINT64_FORMAT, trial_id);

	replace_id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_REPLACE, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, replace_id);
	assert_state(fx, replace_id, "done");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE recycle_state = 'kept' AND recycle_remove_after >"
		" now() + interval '23 hours'"), ==, 1);
	{
		g_autofree gchar *stored = axis_test_library_q(fx->t, "SELECT recycle_path FROM conversion WHERE id = %"
			G_GINT64_FORMAT, replace_id);

		recycled = g_build_filename(fx->disk, stored, NULL);
	}
	g_assert_true(exists(recycled));

	/* Not yet. */
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_cmpuint(report.trials + report.originals, ==, 0);
	{
		g_autofree gchar *path = work_path(fx, trial);

		g_assert_true(exists(path));
	}

	/* The recycled original was changed by someone: it is not removed. */
	sql(fx, "UPDATE conversion SET trial_remove_after = now(), recycle_remove_after = now()");
	{
		g_autofree gchar *content = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(recycled, &content, &length, NULL));
		g_assert_true(g_file_set_contents(recycled, content, (gssize)length / 2, NULL));
	}
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.trials, ==, 1);
	g_assert_cmpuint(report.originals, ==, 0);
	g_assert_true(exists(recycled));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE recycle_state = 'changed'"), ==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion WHERE trial_state = 'removed'"), ==, 1);
	{
		g_autofree gchar *path = work_path(fx, trial);

		g_assert_false(exists(path));
	}

	/* An encode whose job failed; a conversion being checked with no finish
	 * job. */
	lost = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	axis_worker_job_free(job);
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE job SET state = 'failed', last_error = 'the worker"
			" died', finished_at = now(), lease_token = NULL, lease_expires_at = NULL"
			" WHERE id = (SELECT job_id FROM conversion WHERE id = %" G_GINT64_FORMAT ")", lost);

		sql(fx, statement);
	}
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_cmpuint(report.failed, ==, 1);
	assert_state(fx, lost, "failed");
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE conversion SET state = 'verifying' WHERE id = %"
			G_GINT64_FORMAT, lost);

		sql(fx, statement);
	}
	sql(fx, "DELETE FROM job WHERE kind = 'convert.finish'");
	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_cmpuint(report.queued, ==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.finish' AND state = 'queued'"), ==, 1);
	/* Its output is not there: check 1. */
	finish_ok(fx, lost);
	assert_rejected_by(fx, lost, 1);
}

/* ── A worker that dies, and one that is late ──────────────────── */

/*
 * A real axis-worker converting with a real ffmpeg, slowed down
 * (axis_test_paced_ffmpeg(): it reads its input at a fiftieth of its rate,
 * so that it is still converting, and writing, when the test acts) or held
 * back before it starts.  The lease is short (SHORT_LEASE_MS) so that it runs
 * out within seconds of the worker falling silent; the API's reaper gives the
 * job back.
 */

#define SHORT_LEASE_MS 4000

typedef struct
{
	GPid         parent;
	const gchar *comm;
	const gchar *output;
	GPid         found;
} EncoderWait;

static gboolean
encoder_running(gpointer data)
{
	EncoderWait *wait = data;
	struct stat st;

	wait->found = axis_test_find_child(wait->parent, wait->comm);
	if (wait->found == 0)
		return FALSE;
	return wait->output == NULL || (stat(wait->output, &st) == 0 && st.st_size > 0);
}

/* Waits until the worker @w runs its encoder (its child @comm) and, when
 * @output is given, that file has bytes: the encoder's pid. */
static GPid
wait_encoder(
	AxisTestWorker *w,
	const gchar    *comm,
	const gchar    *output
){
	EncoderWait wait;

	wait.parent = axis_test_worker_get_pid(w);
	wait.comm = comm;
	wait.output = output;
	wait.found = 0;
	if (!axis_test_pump_until(encoder_running, &wait, LONG_WAIT_MS))
		g_error("the worker's encoder did not get going; its log:\n%s", axis_test_worker_get_log(w));
	return wait.found;
}

static gboolean
file_gone(gpointer data)
{
	return !exists(data);
}

static gint64
job_of(
	Fixture *fx,
	gint64   id
){
	return axis_test_library_n(fx->t, "SELECT job_id FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
}

/* Waits until the lease of @job has run out and the daemon has given the job
 * back to the queue, with one attempt used. */
static void
wait_given_back(
	Fixture *fx,
	gint64   job
){
	g_autofree gchar *statement = g_strdup_printf("SELECT state::text || ' ' || attempts FROM job WHERE id = %"
		G_GINT64_FORMAT, job);

	if (!axis_test_wait_query(fx->t->db, statement, "queued 1", LONG_WAIT_MS))
		g_error("the lease of job %" G_GINT64_FORMAT " did not run out", job);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND last_error = 'the lease of worker worker:garage expired: it stopped renewing it' AND lease_token IS NULL",
		job),
		==, 1);
}

static AxisConversionTidyReport
tidy(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisConversionTidyReport report;

	g_assert_true(axis_conversions_tidy(fx->conv, &report, &error));
	g_assert_no_error(error);
	return report;
}

/* The leftovers listed now, by name, sorted, space-separated. */
static gchar *
leftovers_listed(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	GString *s = g_string_new(NULL);
	guint i;

	g_assert_true(axis_conversions_leftovers(fx->conv, &listed, &error));
	g_assert_no_error(error);
	for (i = 0; i < listed->len; i++)
		g_ptr_array_add(names, ((AxisConversionLeftover *)g_ptr_array_index(listed, i))->name);
	g_ptr_array_sort(names, by_name);
	for (i = 0; i < names->len; i++)
		g_string_append_printf(s, "%s%s", i > 0 ? " " : "", (const gchar *)g_ptr_array_index(names, i));
	return g_string_free(s, FALSE);
}

/* After its attempt's lease ran out, the conversion waits for a worker
 * again and says why. */
static void
assert_given_back(
	Fixture *fx,
	gint64   id
){
	g_autofree gchar *text = axis_test_library_q(fx->t, "SELECT coalesce(result_text, '') FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id);

	assert_state(fx, id, "queued");
	if (strstr(text, "the lease of worker worker:garage expired") == NULL || strstr(text, "tried again") == NULL)
		g_error("conversion %" G_GINT64_FORMAT " does not say why it waits again: \"%s\"", id, text);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND finished_at IS NULL AND output_reported IS NULL", id), ==, 1);
}

/* The conversion and its job, as a person or the daemon reads them. */
static gchar *
record_of(
	Fixture *fx,
	gint64   id
){
	return axis_test_library_q(fx->t, "SELECT c.state || '|' || coalesce(c.result_text, '') || '|' || c.attempt || '|' ||"
		" coalesce(c.output_name, '') || '|' || coalesce(c.output_reported::text, '') || '|' ||"
		" coalesce(c.output_inode::text, '') || '|' || j.state::text || '|' || j.attempts || '|' ||"
		" coalesce(j.lease_token::text, '') || '|' || coalesce(j.last_error, '') ||"
		" (SELECT '|' || count(*) FROM conversion_check k WHERE k.conversion_id = c.id) ||"
		" (SELECT '|' || count(*) FROM job f WHERE f.kind = 'convert.finish')"
		" FROM conversion c JOIN job j ON j.id = c.job_id WHERE c.id = %" G_GINT64_FORMAT, id);
}

/* What a worker that lost the lease of @job could still send for it (a
 * heartbeat, its result, a failure), each refused, and nothing changed by
 * any of them. */
static void
late_reports_refused(
	Fixture     *fx,
	gint64       id,
	gint64       job,
	const gchar *token,
	const gchar *output,
	gint64       size
){
	g_autofree gchar *before = record_of(fx, id);
	g_autofree gchar *work = work_path(fx, "");
	g_autofree gchar *names = listing(work);
	g_autofree gchar *after = NULL;
	g_autofree gchar *names_after = NULL;
	AxisWorkerHeartbeat beat;
	AxisWorkerFail fail;
	AxisWorkerJob held;

	memset(&beat, 0, sizeof(beat));
	beat.job = job;
	beat.lease_token = (gchar *)token;
	beat.has_progress = TRUE;
	beat.progress = 0.5;
	beat.message = (gchar *)"converting";
	{
		g_autofree gchar *json = axis_worker_heartbeat_to_json(&beat);
		AxisTestReply reply = api(fx, "heartbeat", json);

		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	memset(&held, 0, sizeof(held));
	held.id = job;
	held.lease_token = (gchar *)token;
	g_assert_cmpuint(complete(fx, &held, output, size), ==, 409);
	memset(&fail, 0, sizeof(fail));
	fail.job = job;
	fail.lease_token = (gchar *)token;
	fail.error = (gchar *)"the encoder failed";
	fail.retry = FALSE;
	{
		g_autofree gchar *json = axis_worker_fail_to_json(&fail);
		AxisTestReply reply = api(fx, "fail", json);

		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	after = record_of(fx, id);
	g_assert_cmpstr(after, ==, before);
	names_after = listing(work);
	g_assert_cmpstr(names_after, ==, names);
}

/* Whether the worker's log, up to its attempt @attempt of @job, says that it
 * reported anything about the job (a result, a failure, a giving back). */
static gboolean
reported_before(
	AxisTestWorker *w,
	gint64          job,
	guint           attempt
){
	const gchar *log = axis_test_worker_get_log(w);
	g_autofree gchar *started = g_strdup_printf("job %" G_GINT64_FORMAT " (%s), attempt %u: started", job,
		AXIS_WORKER_KIND_CONVERT, attempt);
	g_autofree gchar *upto = NULL;
	g_autofree gchar *reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", job);
	g_autofree gchar *failed = g_strdup_printf("job %" G_GINT64_FORMAT ": failed", job);
	g_autofree gchar *back = g_strdup_printf("job %" G_GINT64_FORMAT ": giving it back", job);
	g_autofree gchar *refused = g_strdup_printf("job %" G_GINT64_FORMAT ": the daemon refused the report", job);
	g_autofree gchar *again = g_strdup_printf("job %" G_GINT64_FORMAT ": the report was answered", job);
	const gchar *end = strstr(log, started);

	upto = end != NULL ? g_strndup(log, (gsize)(end - log)) : g_strdup(log);
	return strstr(upto, reported) != NULL || strstr(upto, failed) != NULL || strstr(upto, back) != NULL ||
		strstr(upto, refused) != NULL || strstr(upto, again) != NULL;
}

/* The library file of movie @number is the conversion's verified output,
 * alone in its folder, and the conversion is done on attempt 2. */
static void
assert_replaced_on_retry(
	Fixture *fx,
	gint64   id,
	gint64   file,
	guint    number,
	gint64   job
){
	g_autofree gchar *path = movie_path(fx, number, "mkv");
	g_autofree gchar *folder = g_path_get_dirname(path);
	g_autofree gchar *names = listing(folder);
	g_autofree gchar *expected = g_strdup_printf("Movie %u (2001).mkv", number);
	g_autofree gchar *second = g_strdup_printf("%" G_GINT64_FORMAT "-2.tmp", id);
	g_autofree gchar *name = axis_test_library_q(fx->t, "SELECT output_name FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id);

	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT attempt FROM conversion WHERE id = %" G_GINT64_FORMAT, id), ==,
		2);
	g_assert_cmpstr(name, ==, second);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND state = 'succeeded' AND attempts = 2", job), ==, 1);
	g_assert_cmpint(inode_of(path), ==, axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT inode FROM library_file WHERE id = %" G_GINT64_FORMAT, file), ==,
		inode_of(path));
	g_assert_cmpstr(names, ==, expected);
}

/* Spec: a worker killed while it converts.  Its encoder dies with it; when
 * its lease runs out the job is given back and tried again (one of its
 * attempts used), the conversion says so, and the original is byte for byte
 * what it was, under its name; the partial file the dead worker left in the
 * work area is listed, removed after the grace time and not before, and the
 * conversion completes on its second attempt. */
static void
test_worker_killed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slow = NULL;
	g_autofree gchar *hold = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *library = NULL;
	g_autofree gchar *original = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *output = NULL;
	AxisConversionTidyReport report;
	AxisTestWorker *w;
	gint64 ino;
	gint64 file;
	gint64 job;
	gint64 id;
	guint number;
	GPid encoder;

	SKIP_UNLESS_READY(fx);
	slow = g_build_filename(fx->t->dir, "slow", NULL);
	hold = g_build_filename(fx->t->dir, "hold", NULL);
	ffmpeg = axis_test_paced_ffmpeg(fx->t->dir, slow, hold);
	axis_test_storage_write(slow, "");
	file = add_movie(fx, "movie.mkv", &number);
	path = movie_path(fx, number, "mkv");
	original = file_checksum(path);
	ino = inode_of(path);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	library = axis_test_storage_digest(movies);
	prove_on_devices(fx, revision_of(fx, "space_saver"));

	/* Killed while it converts, its output half written. */
	w = start_worker_with(fx, ffmpeg);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_REPLACE);
	wait_state(fx, w, id, "encoding");
	first = g_strdup_printf("%" G_GINT64_FORMAT "-1.tmp", id);
	output = work_path(fx, first);
	encoder = wait_encoder(w, "ffmpeg", output);
	job = job_of(fx, id);
	axis_test_worker_signal(w, SIGKILL);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, -1);
	axis_test_worker_free(w);
	/* Its encoder goes with it. */
	g_assert_true(axis_test_pump_until(process_gone, &encoder, AXIS_TEST_WAIT_MS));

	/* The lease runs out: the job is given back, one attempt used; the
	 * conversion is not done and says why; nothing in the library moved. */
	wait_given_back(fx, job);
	report = tidy(fx);
	g_assert_cmpuint(report.waiting, ==, 1);
	assert_given_back(fx, id);
	{
		g_autofree gchar *now = file_checksum(path);
		g_autofree gchar *after = axis_test_storage_digest(movies);

		g_assert_cmpstr(now, ==, original);
		g_assert_cmpint(inode_of(path), ==, ino);
		g_assert_cmpstr(after, ==, library);
	}
	/* What the dead worker left is listed, and stays until the grace time. */
	g_assert_true(exists(output));
	g_assert_cmpint(size_of(output), >, 0);
	g_assert_cmpuint(report.leftovers, ==, 1);
	g_assert_cmpuint(report.removed, ==, 0);
	{
		g_autofree gchar *listed = leftovers_listed(fx);

		g_assert_cmpstr(listed, ==, first);
	}
	report = tidy(fx);
	g_assert_cmpuint(report.removed, ==, 0);
	g_assert_true(exists(output));

	/* Tried again by a worker that converts at full speed: attempt 2, under
	 * its own name, beside the dead one's file. */
	g_assert_cmpint(g_unlink(slow), ==, 0);
	w = start_worker_with(fx, ffmpeg);
	wait_state(fx, w, id, "verifying");
	stop_worker(fx, w);
	finish_ok(fx, id);
	assert_replaced_on_retry(fx, id, file, number, job);

	/* The dead worker's file: still there and listed until the grace time,
	 * then removed. */
	report = tidy(fx);
	g_assert_cmpuint(report.removed, ==, 0);
	g_assert_true(exists(output));
	{
		g_autofree gchar *listed = leftovers_listed(fx);

		g_assert_cmpstr(listed, ==, first);
	}
	sql(fx, "UPDATE conversion_leftover SET first_seen_at = now() - interval '2 days'");
	report = tidy(fx);
	g_assert_cmpuint(report.removed, ==, 1);
	g_assert_false(exists(output));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_leftover"), ==, 0);
	{
		g_autofree gchar *work = work_path(fx, "");
		g_autofree gchar *left = listing(work);

		g_assert_cmpstr(left, ==, "");
	}
}

/* Spec: a worker stopped (SIGSTOP) while it converts, past its lease, and
 * continued after the job was given back; twice: its encoder still
 * converting when it is continued, and its encoder done while it was
 * stopped.  The original stays as it was until the job's next attempt
 * replaces it; the late worker says nothing more about the job it lost (and
 * what it could say is refused and changes nothing); it stops its encoder
 * and removes its output itself; what is in the work area meanwhile is
 * listed, and nothing of it is removed before the grace time. */
static void
test_worker_late(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slow = NULL;
	g_autofree gchar *hold = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *movies = NULL;
	AxisTestWorker *w;
	guint round;

	SKIP_UNLESS_READY(fx);
	slow = g_build_filename(fx->t->dir, "slow", NULL);
	hold = g_build_filename(fx->t->dir, "hold", NULL);
	ffmpeg = axis_test_paced_ffmpeg(fx->t->dir, slow, hold);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	prove_on_devices(fx, revision_of(fx, "space_saver"));

	for (round = 0; round < 2; round++) {
		gboolean finished = round == 1;
		g_autofree gchar *path = NULL;
		g_autofree gchar *original = NULL;
		g_autofree gchar *library = NULL;
		g_autofree gchar *first = NULL;
		g_autofree gchar *output = NULL;
		g_autofree gchar *token = NULL;
		g_autofree gchar *fenced = NULL;
		AxisConversionTidyReport report;
		gint64 ino;
		gint64 file;
		gint64 job;
		gint64 id;
		guint number;
		GPid encoder;

		g_test_message("the encoder %s when its worker is continued", finished ? "is done" : "still converts");
		file = add_movie(fx, "movie.mkv", &number);
		path = movie_path(fx, number, "mkv");
		original = file_checksum(path);
		ino = inode_of(path);
		library = axis_test_storage_digest(movies);
		axis_test_storage_write(finished ? hold : slow, "");
		w = start_worker_with(fx, ffmpeg);
		id = request(fx, file, "space_saver", AXIS_CONVERSION_REPLACE);
		wait_state(fx, w, id, "encoding");
		first = g_strdup_printf("%" G_GINT64_FORMAT "-1.tmp", id);
		output = work_path(fx, first);
		/* Converting, its output being written; or held back before
		 * ffmpeg starts, its output made. */
		encoder = wait_encoder(w, finished ? "sh" : "ffmpeg", finished ? NULL : output);
		g_assert_true(exists(output));
		job = job_of(fx, id);
		token = axis_test_library_q(fx->t, "SELECT lease_token::text FROM job WHERE id = %" G_GINT64_FORMAT, job);
		g_assert_nonnull(token);

		/* Stopped past its lease: the job is given back. */
		axis_test_worker_signal(w, SIGSTOP);
		wait_given_back(fx, job);
		if (finished) {
			/* Its encoder runs to the end while it is stopped. */
			g_assert_cmpint(g_unlink(hold), ==, 0);
			g_assert_true(axis_test_pump_until(process_gone, &encoder, LONG_WAIT_MS));
			g_assert_cmpint(size_of(output), >, 0);
		}
		report = tidy(fx);
		g_assert_cmpuint(report.waiting, ==, 1);
		assert_given_back(fx, id);
		g_assert_cmpuint(report.removed, ==, 0);
		{
			g_autofree gchar *listed = leftovers_listed(fx);
			g_autofree gchar *now = file_checksum(path);
			g_autofree gchar *after = axis_test_storage_digest(movies);

			g_assert_cmpstr(listed, ==, first);
			g_assert_cmpstr(now, ==, original);
			g_assert_cmpint(inode_of(path), ==, ino);
			g_assert_cmpstr(after, ==, library);
		}
		/* Whatever it would say about the job now is refused. */
		late_reports_refused(fx, id, job, token, first, size_of(output));

		/* Continued: it finds its deadline passed, stops its encoder, drops
		 * its output and says nothing; then it takes the job's next attempt,
		 * which converts at full speed. */
		if (!finished)
			g_assert_cmpint(g_unlink(slow), ==, 0);
		axis_test_worker_signal(w, SIGCONT);
		fenced = g_strdup_printf("job %" G_GINT64_FORMAT ": its lease", job);
		if (!axis_test_worker_wait_log(w, fenced, LONG_WAIT_MS))
			g_error("the late worker did not give the job up; its log:\n%s", axis_test_worker_get_log(w));
		g_assert_true(axis_test_pump_until(process_gone, &encoder, AXIS_TEST_WAIT_MS));
		if (!axis_test_pump_until(file_gone, output, AXIS_TEST_WAIT_MS))
			g_error("the late worker left its output %s; its log:\n%s", first, axis_test_worker_get_log(w));
		wait_state(fx, w, id, "verifying");
		if (reported_before(w, job, 2))
			g_error("the late worker reported on the job it had lost; its log:\n%s", axis_test_worker_get_log(w));
		stop_worker(fx, w);
		finish_ok(fx, id);
		assert_replaced_on_retry(fx, id, file, number, job);
		report = tidy(fx);
		g_assert_cmpuint(report.removed, ==, 0);
	}
	{
		g_autofree gchar *work = work_path(fx, "");
		g_autofree gchar *left = listing(work);

		g_assert_cmpstr(left, ==, "");
		g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_leftover"), ==, 0);
	}
}

typedef struct
{
	GPid         pid;
	const gchar *path;
} OpenWait;

/* Whether the process @pid no longer has @path open. */
static gboolean
closed_by(gpointer data)
{
	OpenWait *wait = data;
	g_autofree gchar *fds = g_strdup_printf("/proc/%d/fd", (int)wait->pid);
	g_autoptr(GDir) dir = g_dir_open(fds, 0, NULL);
	const gchar *name;

	if (dir == NULL)
		return FALSE;
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *link = g_build_filename(fds, name, NULL);
		g_autofree gchar *target = g_file_read_link(link, NULL);

		if (g_strcmp0(target, wait->path) == 0)
			return FALSE;
	}
	return TRUE;
}

/* Spec: a worker cut off from the daemon while its encode finishes: the
 * daemon cannot answer its heartbeat (its database holds the job's row) and
 * takes the job back, while the worker has its result in hand.  The worker
 * never gets the result taken: what it sends for the job is refused and
 * changes nothing, it removes its finished output itself (the daemon never
 * took it), and the job's next attempt replaces the original. */
static void
test_worker_cut_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slow = NULL;
	g_autofree gchar *hold = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *original = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *job_text = NULL;
	g_autofree gchar *blocked = NULL;
	g_autofree gchar *reported = NULL;
	const gchar *params[1];
	AxisConversionTidyReport report;
	AxisTestWorker *w;
	AxisDbConn *conn;
	OpenWait open;
	gint64 file;
	gint64 job;
	gint64 id;
	gint64 ino;
	guint number;
	GPid encoder;

	SKIP_UNLESS_READY(fx);
	slow = g_build_filename(fx->t->dir, "slow", NULL);
	hold = g_build_filename(fx->t->dir, "hold", NULL);
	ffmpeg = axis_test_paced_ffmpeg(fx->t->dir, slow, hold);
	axis_test_storage_write(hold, "");
	file = add_movie(fx, "movie.mkv", &number);
	path = movie_path(fx, number, "mkv");
	original = file_checksum(path);
	ino = inode_of(path);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	w = start_worker_with(fx, ffmpeg);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_REPLACE);
	wait_state(fx, w, id, "encoding");
	first = g_strdup_printf("%" G_GINT64_FORMAT "-1.tmp", id);
	output = work_path(fx, first);
	encoder = wait_encoder(w, "sh", NULL);
	job = job_of(fx, id);
	job_text = g_strdup_printf("%" G_GINT64_FORMAT, job);
	token = axis_test_library_q(fx->t, "SELECT lease_token::text FROM job WHERE id = %" G_GINT64_FORMAT, job);

	/* The daemon's database holds the job's row: its next heartbeat waits. */
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	params[0] = job_text;
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SELECT 1 FROM job WHERE id = $1::bigint FOR UPDATE", 1, params, NULL,
		&error));
	g_assert_no_error(error);
	blocked = g_strdup("SELECT (count(*) > 0)::text FROM pg_stat_activity WHERE datname = current_database()"
		" AND wait_event_type = 'Lock' AND query LIKE 'WITH hb AS%'");
	if (!axis_test_wait_query(fx->t->db, blocked, "true", LONG_WAIT_MS))
		g_error("no heartbeat of the worker waited for the job's row");

	/* Meanwhile its encode runs to the end, and the worker has its result:
	 * the source is closed, the output kept. */
	g_assert_cmpint(g_unlink(hold), ==, 0);
	g_assert_true(axis_test_pump_until(process_gone, &encoder, LONG_WAIT_MS));
	open.pid = axis_test_worker_get_pid(w);
	open.path = path;
	g_assert_true(axis_test_pump_until(closed_by, &open, LONG_WAIT_MS));
	g_assert_true(exists(output));
	g_assert_cmpint(size_of(output), >, 0);

	/* Then the daemon takes the job back: the lease is over, and the job
	 * queue's sweep of expired leases gives it back (as the daemon's queue
	 * does for remote kinds).  The job's next attempt, which the same
	 * worker takes at once, is held back until the end of the test's
	 * looking. */
	axis_test_storage_write(hold, "");
	g_assert_true(axis_db_conn_exec(conn, "UPDATE job SET lease_expires_at = now() - interval '1 second'"
		" WHERE id = $1::bigint", 1, params, NULL, &error));
	{
		static const gchar *const kinds[] = { AXIS_WORKER_KIND_CONVERT, NULL };
		guint swept = 0;

		g_assert_true(axis_job_sweep_expired_leases(conn, kinds, NULL, &swept, &error));
		g_assert_cmpuint(swept, ==, 1);
	}
	g_assert_true(axis_db_conn_commit(conn, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);

	/* The worker learns it (its heartbeat, or its report, refused), says
	 * nothing more, and removes the output the daemon never took. */
	if (!axis_test_pump_until(file_gone, output, LONG_WAIT_MS))
		g_error("the worker left its output %s; its log:\n%s", first, axis_test_worker_get_log(w));
	g_assert_true(axis_test_worker_wait_log(w, "its output was removed from the work area: the daemon did not take it",
		AXIS_TEST_WAIT_MS));
	/* The job was given back (the lease ended) and taken again. */
	{
		g_autofree gchar *statement = g_strdup_printf("SELECT state::text || ' ' || attempts || ' ' || last_error FROM"
			" job WHERE id = %" G_GINT64_FORMAT, job);

		if (!axis_test_wait_query(fx->t->db, statement, "running 2 the lease of worker worker:garage expired: it "
		                          "stopped renewing it", LONG_WAIT_MS))
			g_error("the job was not given back and taken again; the worker's log:\n%s", axis_test_worker_get_log(w));
	}
	encoder = wait_encoder(w, "sh", NULL);
	wait_state(fx, w, id, "encoding");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT attempt FROM conversion WHERE id = %" G_GINT64_FORMAT, id), ==,
		2);
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", job);
	{
		g_autofree gchar *started = g_strdup_printf("job %" G_GINT64_FORMAT " (%s), attempt 2: started", job,
			AXIS_WORKER_KIND_CONVERT);
		const gchar *log = axis_test_worker_get_log(w);
		const gchar *end = strstr(log, started);
		g_autofree gchar *upto = end != NULL ? g_strndup(log, (gsize)(end - log)) : g_strdup(log);

		if (strstr(upto, reported) != NULL)
			g_error("the result was taken after the lease ended; the worker's log:\n%s", log);
	}
	report = tidy(fx);
	{
		g_autofree gchar *listed = leftovers_listed(fx);
		g_autofree gchar *now = file_checksum(path);

		g_assert_cmpstr(listed, ==, "");
		g_assert_cmpstr(now, ==, original);
		g_assert_cmpint(inode_of(path), ==, ino);
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND output_reported IS NULL", id), ==, 1);
	late_reports_refused(fx, id, job, token, first, 1000);

	/* The next attempt, on time. */
	g_assert_cmpint(g_unlink(hold), ==, 0);
	wait_state(fx, w, id, "verifying");
	stop_worker(fx, w);
	finish_ok(fx, id);
	assert_replaced_on_retry(fx, id, file, number, job);
	report = tidy(fx);
	g_assert_cmpuint(report.removed, ==, 0);
	{
		g_autofree gchar *work = work_path(fx, "");
		g_autofree gchar *left = listing(work);

		g_assert_cmpstr(left, ==, "");
	}
}

/* ── Proving by hand ────────────────────────────────────────────── */

/* Spec: a revision is proven by hand only from enough passed trials on
 * different files, exactly those the confirmation named; then a
 * replacement with it is allowed. */
static void
test_prove_by_hand(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	gint64 revision;
	gint64 trials[3];
	gint64 wrong[3];
	gint64 file = 0;
	guint number;
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	revision = revision_of(fx, "space_saver");
	g_assert_true(axis_conversions_proving(fx->conv, revision, &proving, &error));
	g_assert_false(proving->allowed);
	g_assert_cmpuint(proving->needed, ==, 3);
	g_assert_nonnull(strstr(proving->why_not, "0 passed trials"));
	g_clear_pointer(&proving, axis_conversion_proving_free);

	for (i = 0; i < 3; i++) {
		AxisWorkerJob *job;

		trials[i] = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
		encode_and_report(fx, job, NULL);
		axis_worker_job_free(job);
		finish_ok(fx, trials[i]);
		assert_state(fx, trials[i], "done");
		if (i == 0)
			file = axis_test_library_n(fx->t, "SELECT file_id FROM conversion WHERE id = %" G_GINT64_FORMAT, trials[i]);
	}
	g_assert_true(axis_conversions_proving(fx->conv, revision, &proving, &error));
	g_assert_true(proving->allowed);
	g_assert_cmpuint(proving->trials->len, ==, 3);
	g_clear_pointer(&proving, axis_conversion_proving_free);

	/* Not the trials it lists: refused. */
	wrong[0] = trials[0];
	wrong[1] = trials[1];
	wrong[2] = trials[2] + 1000;
	g_assert_false(axis_conversions_prove(fx->conv, revision, wrong, 3, "root", &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(axis_conversions_prove(fx->conv, revision, trials, 2, "root", &error));
	g_clear_error(&error);
	/* The database refuses a proof of trials that did not pass. */
	{
		g_autofree gchar *statement = g_strdup_printf("INSERT INTO conversion_profile_proof (revision_id, method,"
			" recorded_by, trials) VALUES (%" G_GINT64_FORMAT ", 'trials', 'root', '{%" G_GINT64_FORMAT "}')", revision,
			trials[0] + 1000);
		gboolean refused = !axis_db_pool_exec(fx->t->pool, statement, 0, NULL, NULL, &error);

		g_assert_true(refused);
		g_clear_error(&error);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM conversion_profile_proof"), ==, 0);

	g_assert_true(axis_conversions_prove(fx->conv, revision, trials, 3, "root", &error));
	g_assert_no_error(error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion_profile_proof WHERE revision_id = %"
		G_GINT64_FORMAT " AND method = 'trials' AND recorded_by = 'root' AND cardinality(trials) = 3", revision), ==, 1);
	g_assert_true(axis_conversions_proving(fx->conv, revision, &proving, &error));
	g_assert_false(proving->allowed);
	g_assert_nonnull(strstr(proving->why_not, "proven already"));
	g_clear_pointer(&proving, axis_conversion_proving_free);

	/* Proven: a replacement is allowed now. */
	{
		gint64 id = request(fx, file, "space_saver", AXIS_CONVERSION_REPLACE);

		g_assert_cmpint(id, >, 0);
	}
}

/* Spec: a trial rejected by a check of the output itself blocks proving. */
static void
test_prove_blocked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	g_auto(GStrv) args = NULL;
	AxisWorkerJob *job;
	gint64 revision;
	gint64 id;
	guint number;
	guint i;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	set(fx, AXIS_CONVERSION_SETTING_PROVE_TRIALS, "1");
	revision = revision_of(fx, "space_saver");
	for (i = 0; i < 2; i++) {
		id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
		if (i == 0) {
			g_autofree gchar *out = work_path(fx, job->convert->output);

			args = args_set(job->convert, "-pix_fmt:0", "yuv420p");
			g_assert_true(encode_with(fx, job->convert, args, job->convert->output));
			g_assert_cmpuint(complete(fx, job, job->convert->output, size_of(out)), ==, 200);
		} else {
			encode_and_report(fx, job, NULL);
		}
		axis_worker_job_free(job);
		finish_ok(fx, id);
	}
	g_assert_true(axis_conversions_proving(fx->conv, revision, &proving, &error));
	g_assert_cmpuint(proving->bad_trials, ==, 1);
	g_assert_cmpuint(proving->trials->len, ==, 1);
	g_assert_false(proving->allowed);
	g_assert_nonnull(strstr(proving->why_not, "rejected by a check"));
	{
		gint64 one = ((AxisConversionRecord *)g_ptr_array_index(proving->trials, 0))->id;

		g_assert_false(axis_conversions_prove(fx->conv, revision, &one, 1, "root", &error));
		g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED);
	}
}

/* ── The write layer ────────────────────────────────────────────── */

/* Spec: the work-area operations refuse what they must. */
static void
test_write_layer(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autofree gchar *work_dir = NULL;
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *taken = NULL;
	struct stat st;
	gboolean removed = FALSE;
	gint work;
	gint dir;

	SKIP_UNLESS_READY(fx);
	/* The scanner never indexes the work area or the recycle folder. */
	g_assert_cmpint(axis_library_ignore_builtin(AXIS_VOLUME_WORK_DIR, strlen(AXIS_VOLUME_WORK_DIR), TRUE), ==,
		AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(axis_library_ignore_builtin(AXIS_VOLUME_RECYCLE_DIR, strlen(AXIS_VOLUME_RECYCLE_DIR), TRUE), ==,
		AXIS_IGNORE_SYSTEM);
	g_assert_true(axis_volume_write_work_name_valid("1-1.tmp"));
	g_assert_true(axis_volume_write_work_name_valid("123-12.tmp"));
	g_assert_true(axis_volume_write_work_name_valid("trial-5.mkv"));
	g_assert_true(axis_volume_write_work_name_valid("trial-5.mp4"));
	g_assert_false(axis_volume_write_work_name_valid("01-1.tmp"));
	g_assert_false(axis_volume_write_work_name_valid("1-01.tmp"));
	g_assert_false(axis_volume_write_work_name_valid("../1-1.tmp"));
	g_assert_false(axis_volume_write_work_name_valid("1-1.tmp/x"));
	g_assert_false(axis_volume_write_work_name_valid("trial-5.avi"));
	g_assert_false(axis_volume_write_work_name_valid(".axis-partial-5"));
	g_assert_false(axis_volume_write_work_name_valid("notes.txt"));
	g_assert_false(axis_volume_write_work_name_valid(""));

	/* Read-only: no write guard at all. */
	read_only(fx, TRUE);
	g_assert_null(axis_volume_guard_acquire(fx->t->storage, fx->volume, AXIS_VOLUME_INTENT_WRITE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);
	read_only(fx, FALSE);

	guard = axis_volume_guard_acquire(fx->t->storage, fx->volume, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	work = axis_volume_write_mkdir(guard, axis_volume_guard_get_fd(guard), AXIS_VOLUME_WORK_DIR, 0700, NULL, NULL,
		&error);
	g_assert_cmpint(work, >=, 0);
	work_dir = work_path(fx, "");
	a = work_path(fx, "1-1.tmp");
	b = work_path(fx, "2-1.tmp");
	axis_test_storage_write(a, "one");
	axis_test_storage_write(b, "two");

	/* Another inode than the one expected: nothing is removed. */
	g_assert_false(axis_volume_write_unlink_work(guard, work, "1-1.tmp", (guint64)inode_of(b), &removed, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	g_assert_true(exists(a));
	/* A name that is not Axis's. */
	g_assert_false(axis_volume_write_unlink_work(guard, work, "../readme", 0, &removed, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* A rename that would replace a name. */
	g_assert_false(axis_volume_write_rename_work(guard, work, "1-1.tmp", (guint64)inode_of(a), "2-1.tmp", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	{
		g_autofree gchar *text = axis_test_storage_read(b);

		g_assert_cmpstr(text, ==, "two");
	}
	/* A link is never settled. */
	{
		g_autofree gchar *l = work_path(fx, "3-1.tmp");
		struct stat settled;

		g_assert_cmpint(symlink(a, l), ==, 0);
		g_assert_false(axis_volume_write_settle(guard, work, "3-1.tmp", (guint64)inode_of(l), 0644, getuid(), getgid(),
			1, 0, TRUE, &settled, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_cmpint(g_unlink(l), ==, 0);
	}
	/* Moved into the library under a temporary name only, never over a
	 * name. */
	movies = g_build_filename(fx->disk, "Movies", NULL);
	dir = axis_volume_guard_open_dir(guard, "Movies", &error);
	g_assert_cmpint(dir, >=, 0);
	taken = g_build_filename(movies, AXIS_VOLUME_TEMP_PREFIX "1", NULL);
	axis_test_storage_write(taken, "someone else's");
	g_assert_false(axis_volume_write_adopt(guard, work, "1-1.tmp", (guint64)inode_of(a), dir, AXIS_VOLUME_TEMP_PREFIX
		"1", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	{
		g_autofree gchar *text = axis_test_storage_read(taken);

		g_assert_cmpstr(text, ==, "someone else's");
	}
	g_assert_true(exists(a));
	g_assert_false(axis_volume_write_adopt(guard, work, "1-1.tmp", (guint64)inode_of(a), dir, "Movie.mkv", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(axis_volume_write_adopt(guard, work, "1-1.tmp", (guint64)inode_of(a), dir, AXIS_VOLUME_TEMP_PREFIX
		"2", &error));
	g_assert_no_error(error);
	g_assert_false(exists(a));
	/* A work area that is not the volume's: the library folder given as
	 * one. */
	g_assert_false(axis_volume_write_unlink_work(guard, dir, AXIS_VOLUME_TEMP_PREFIX "2", 0, &removed, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	{
		g_autofree gchar *moved = g_build_filename(movies, AXIS_VOLUME_TEMP_PREFIX "2", NULL);

		g_assert_cmpint(lstat(moved, &st), ==, 0);
	}
	/* Removed by its name and inode. */
	g_assert_true(axis_volume_write_unlink_work(guard, work, "2-1.tmp", (guint64)inode_of(b), &removed, &error));
	g_assert_true(removed);
	g_assert_false(exists(b));
	close(dir);
	close(work);
	(void)work_dir;
}

/* ── A worker through a network mount; a crashed encoder ────────── */

/* A movie whose time has nanoseconds, as a local disk keeps it. */
static gint64
add_movie_at(
	Fixture     *fx,
	const gchar *clip,
	guint       *number,
	gint64       sec,
	glong        nsec
){
	g_autofree gchar *from = g_build_filename(clips, clip, NULL);
	g_autofree gchar *to = NULL;
	g_autofree gchar *rel = NULL;
	gint64 id;

	*number = ++fx->n_movies;
	to = movie_path(fx, *number, "mkv");
	rel = movie_rel(*number, "mkv");
	copy_file(from, to);
	axis_test_set_mtime(to, sec, nsec);
	scan(fx);
	id = axis_test_scenario_file(fx->t, fx->movies, rel);
	probe_file(fx, id);
	return id;
}

/* The disk as sshfs shows it to a worker: another directory with the
 * volume's marker, its work area and movie @number copied, so with other
 * inode numbers, and its time to the second only.  (Nothing in the tests can
 * mount a filesystem; the worker writes its output here, and the test moves
 * it onto the disk, where a real mount would have written it.) */
static gchar *
mount_view(
	Fixture *fx,
	guint    number
){
	g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *rel = movie_rel(number, "mkv");
	g_autofree gchar *source = movie_path(fx, number, "mkv");
	gchar *view = g_build_filename(fx->t->dir, "view", NULL);
	g_autofree gchar *view_marker = g_build_filename(view, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *work = g_build_filename(view, AXIS_VOLUME_WORK_DIR, NULL);
	g_autofree gchar *copy = g_build_filename(view, "Movies", rel, NULL);
	struct stat st;

	axis_test_storage_mkdir(view);
	if (!exists(view_marker))
		copy_file(marker, view_marker);
	if (!exists(work))
		g_assert_cmpint(g_mkdir(work, 0700), ==, 0);
	copy_file(source, copy);
	g_assert_cmpint(stat(source, &st), ==, 0);
	g_assert_cmpint(st.st_mtim.tv_nsec, !=, 0);
	axis_test_set_mtime(copy, st.st_mtim.tv_sec, 0);
	return view;
}

static AxisTestWorker *
start_worker_on(
	Fixture     *fx,
	const gchar *volume,
	const gchar *token,
	const gchar *ffmpeg,
	const gchar *extra
){
	const gchar *volumes[2];
	AxisTestWorkerOptions options;
	AxisTestWorker *w;

	volumes[0] = volume;
	volumes[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = fx->d->url;
	options.token = token != NULL ? token : fx->token;
	options.volumes = volumes;
	options.ffmpeg = ffmpeg;
	options.extra_config = extra;
	w = axis_test_worker_start(&options);
	if (!axis_test_worker_wait_log(w, "registered as ", AXIS_TEST_WAIT_MS))
		g_error("the worker did not register; its log:\n%s", axis_test_worker_get_log(w));
	return w;
}

/* The conversion's encode job ended as failed, and the tidy job has said so
 * on the conversion: its text. */
static gchar *
encode_failed(
	Fixture        *fx,
	AxisTestWorker *w,
	gint64          id
){
	g_autofree gchar *statement = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, job_of(fx, id));

	if (!axis_test_wait_query(fx->t->db, statement, "failed", LONG_WAIT_MS))
		g_error("the encode of conversion %" G_GINT64_FORMAT " did not fail; the worker's log:\n%s", id,
			w != NULL ? axis_test_worker_get_log(w) : "");
	(void)tidy(fx);
	assert_state(fx, id, "failed");
	return axis_test_library_q(fx->t, "SELECT coalesce(result_text, '') FROM conversion WHERE id = %" G_GINT64_FORMAT,
		id);
}

static void
assert_says(
	const gchar *text,
	const gchar *needle
){
	if (text == NULL || strstr(text, needle) == NULL)
		g_error("expected '%s' in: %s", needle, text != NULL ? text : "(null)");
}

/* Spec: a worker that sees the volume through a network mount that does
 * not keep file identities (made-up inode numbers, whole seconds) is refused
 * by default, with a message that says what to set; told so, it checks the
 * size, the time to the second and samples of the content instead, and the
 * daemon's checks of what it made, on its own disk, are as for any other;
 * other content of the same size and time is refused. */
static void
test_mount_without_identity(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *view = NULL;
	g_autofree gchar *extra = NULL;
	g_autofree gchar *movies = NULL;
	g_autofree gchar *library = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *view_work = NULL;
	AxisTestWorker *w;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	file = add_movie_at(fx, "movie.mkv", &number, 1700000000, 123456789);
	view = mount_view(fx, number);
	view_work = g_build_filename(view, AXIS_VOLUME_WORK_DIR, NULL);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	library = axis_test_storage_digest(movies);

	/* As strict as ever by default: refused before anything is made. */
	w = start_worker_on(fx, view, NULL, NULL, NULL);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	why = encode_failed(fx, w, id);
	assert_says(why, "source changed: the file is not the one the conversion was requested for");
	assert_says(why, "its modification time differs below a second");
	assert_says(why, "AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY");
	stop_worker(fx, w);
	{
		g_autofree gchar *left = listing(view_work);

		g_assert_cmpstr(left, ==, "");
	}

	/* Told the mount does not keep them: converted, and every check of the
	 * daemon passes on what it made. */
	extra = g_strdup_printf("volumes_without_file_identity = %s\n", view);
	w = start_worker_on(fx, view, NULL, NULL, extra);
	{
		g_autofree gchar *kept = axis_test_library_q(fx->t, "SELECT string_agg(keeps_identity::text, ',')"
			" FROM worker_volume");

		g_assert_cmpstr(kept, ==, "false");
	}
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "verifying");
	stop_worker(fx, w);
	name = axis_test_library_q(fx->t, "SELECT output_name FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	{
		g_autofree gchar *from = g_build_filename(view_work, name, NULL);
		g_autofree gchar *to = work_path(fx, name);

		g_assert_cmpint(g_rename(from, to), ==, 0);
	}
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(after, ==, library);

	/* Other content, the same size and time to the second: the samples
	 * differ, refused. */
	{
		g_autofree gchar *rel = movie_rel(number, "mkv");
		g_autofree gchar *copy = g_build_filename(view, "Movies", rel, NULL);
		g_autofree gchar *content = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(copy, &content, &length, NULL));
		g_assert_cmpuint(length, >, 200);
		content[100] ^= 0x5a;
		g_assert_true(g_file_set_contents(copy, content, (gssize)length, NULL));
		axis_test_set_mtime(copy, 1700000000, 0);
	}
	w = start_worker_on(fx, view, NULL, NULL, extra);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	g_clear_pointer(&why, g_free);
	why = encode_failed(fx, w, id);
	assert_says(why, "source changed: its content is not what the daemon read");
	stop_worker(fx, w);
	{
		g_autofree gchar *left = listing(view_work);

		g_assert_cmpstr(left, ==, "");
	}
}

/* Spec: when a worker takes the work, the daemon looks at the file on its
 * own disk: the whole stamp of the request (a change the index does not know
 * of yet is refused), and the digest of samples of it, which the description
 * carries. */
static void
test_claim_reads_source(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *why = NULL;
	AxisWorkerJob *job;
	struct stat st;
	guint number;
	gint64 file;
	gint64 id;
	gint err = 0;
	gint fd;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	file = add_movie(fx, "movie.mkv", &number);
	path = movie_path(fx, number, "mkv");
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	job = claim_work(fx);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(fstat(fd, &st), ==, 0);
	g_assert_true(axis_file_sample_digest(fd, (gint64)st.st_size, &digest, &err));
	close(fd);
	g_assert_cmpstr(job->convert->source_sample, ==, digest);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");

	/* Rewritten in place since the request, its size kept and its time put
	 * back to the nanosecond, with no scan since: the index cannot tell, the
	 * change time on the disk can. */
	file = add_movie(fx, "movie.mkv", &number);
	g_clear_pointer(&path, g_free);
	path = movie_path(fx, number, "mkv");
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	g_assert_cmpint(stat(path, &st), ==, 0);
	g_usleep(20000);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(pwrite(fd, "changed", 7, st.st_size / 2), ==, 7);
	close(fd);
	axis_test_set_mtime(path, st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
	{
		struct stat now;

		g_assert_cmpint(stat(path, &now), ==, 0);
		g_assert_cmpint(now.st_ino, ==, st.st_ino);
		g_assert_cmpint(now.st_size, ==, st.st_size);
		g_assert_cmpint(now.st_mtim.tv_nsec, ==, st.st_mtim.tv_nsec);
	}
	g_assert_null(claim(fx));
	assert_state(fx, id, "refused");
	why = axis_test_library_q(fx->t, "SELECT result_text FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	assert_says(why, "the file on disk is not the one the conversion was requested for");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM job WHERE kind = 'convert.encode' AND state = 'failed'"), ==, 1);
}

/* Spec: a crashed encoder (it died of a signal) fails its conversion as any
 * encoder failure does and pauses its worker, which takes no more work until
 * it is resumed or restarted; an encoder that exits with an error (the
 * input's doing) does not pause it; another worker is not affected. */
static void
test_encoder_crash_pauses(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *flag = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *shed = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *crashed = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestWorker *w;
	AxisTestWorker *other;
	gboolean resumed = FALSE;
	guint number;
	gint64 file;
	gint64 id;
	gint64 job;

	/* Nothing of the fixture before this: without ffmpeg or a database it
	 * was never set up. */
	SKIP_UNLESS_READY(fx);
	flag = g_build_filename(fx->t->dir, "fail-how", NULL);
	ffmpeg = axis_test_failing_ffmpeg(fx->t->dir, flag);
	w = start_worker_with(fx, ffmpeg);

	/* An exit with an error: the conversion fails; the worker goes on. */
	axis_test_storage_write(flag, "fail");
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	why = encode_failed(fx, w, id);
	assert_says(why, "Invalid data found when processing input");
	g_assert_cmpint(n(fx, "SELECT count(*) FROM worker WHERE paused_at IS NOT NULL"), ==, 0);

	/* A crash: the conversion fails just the same, and the worker pauses. */
	axis_test_storage_write(flag, "SEGV");
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	job = job_of(fx, id);
	g_clear_pointer(&why, g_free);
	why = encode_failed(fx, w, id);
	assert_says(why, "the encoder crashed (signal 11, SIGSEGV)");
	assert_says(why, "this worker takes no more work until it is resumed on the Workers page or restarted");
	crashed = g_strdup("SELECT paused_job || ' ' || paused_reason FROM worker WHERE name = 'garage'");
	{
		g_autofree gchar *expected = g_strdup_printf("%" G_GINT64_FORMAT " signal 11, SIGSEGV", job);

		if (!axis_test_wait_query(fx->t->db, crashed, expected, AXIS_TEST_WAIT_MS))
			g_error("the worker was not paused; its log:\n%s", axis_test_worker_get_log(w));
	}
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "its encoder crashed (signal 11, SIGSEGV): this worker takes "
		"no more work"));

	/* No more work for it, however long it waits, though the encoder would
	 * now work. */
	g_assert_cmpint(g_unlink(flag), ==, 0);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	axis_test_pump_for(2000);
	assert_state(fx, id, "queued");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT attempts FROM job WHERE id = %" G_GINT64_FORMAT,
		job_of(fx, id)), ==, 0);

	/* Another worker takes it. */
	shed = axis_test_daemon_add_worker(fx->d, "shed", NULL);
	other = start_worker_on(fx, fx->disk, shed, NULL, NULL);
	wait_state(fx, other, id, "verifying");
	{
		g_autofree gchar *by = axis_test_library_q(fx->t, "SELECT worker FROM conversion WHERE id = %" G_GINT64_FORMAT,
			id);

		g_assert_cmpstr(by, ==, "shed");
	}
	stop_worker(fx, other);

	/* Resumed (the Workers page's button): it takes work again. */
	g_assert_true(axis_worker_store_resume(fx->t->pool, "garage", "root", &resumed, &error));
	g_assert_no_error(error);
	g_assert_true(resumed);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "verifying");
	{
		g_autofree gchar *by = axis_test_library_q(fx->t, "SELECT worker FROM conversion WHERE id = %" G_GINT64_FORMAT,
			id);

		g_assert_cmpstr(by, ==, "garage");
	}
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "resumed on the Workers page"));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM worker WHERE paused_at IS NOT NULL"), ==, 0);

	/* Crashed again, then restarted: the new process has no pause. */
	axis_test_storage_write(flag, "ABRT");
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	g_clear_pointer(&why, g_free);
	why = encode_failed(fx, w, id);
	assert_says(why, "the encoder crashed (signal 6, SIGABRT)");
	if (!axis_test_wait_query(fx->t->db, "SELECT count(*) FROM worker WHERE name = 'garage' AND paused_at IS NOT NULL",
	        "1", AXIS_TEST_WAIT_MS))
		g_error("the worker was not paused again; its log:\n%s", axis_test_worker_get_log(w));
	stop_worker(fx, w);
	g_assert_cmpint(g_unlink(flag), ==, 0);
	w = start_worker_with(fx, ffmpeg);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM worker WHERE paused_at IS NOT NULL"), ==, 0);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "verifying");
	stop_worker(fx, w);
	axis_secret_free(g_steal_pointer(&shed));
}

/* ── The checks measured on a worker (verify-on-worker) ─────────── */

/* Claims work of @kind as the worker of @token: %NULL when there is none. */
static AxisWorkerJob *
claim_as(
	Fixture     *fx,
	const gchar *token,
	const gchar *kind
){
	const gchar *kinds[2];
	const gchar *volumes[2];
	g_autofree gchar *json = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerJob *job = NULL;
	AxisWorkerClaim request;
	AxisTestReply reply;

	kinds[0] = kind;
	kinds[1] = NULL;
	volumes[0] = fx->uuid;
	volumes[1] = NULL;
	memset(&request, 0, sizeof(request));
	request.kinds = (gchar **)kinds;
	request.volumes = (gchar **)volumes;
	json = axis_worker_claim_to_json(&request);
	reply = axis_test_daemon_post(fx->d, "POST", "claim", token, NULL, json, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	if (!axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, &problem))
		g_error("%s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	return job;
}

/* The check job as the worker of @token claims it. */
static AxisWorkerJob *
claim_checks(
	Fixture     *fx,
	const gchar *token
){
	AxisWorkerJob *job = claim_as(fx, token, AXIS_WORKER_KIND_VERIFY);

	g_assert_nonnull(job);
	g_assert_nonnull(job->verify);
	g_assert_cmpstr(job->verify->volume, ==, fx->uuid);
	return job;
}

/* What a worker measures, made here on the test's own view of the disk with
 * the shared measurement code (ffmpeg's own threads). */
static AxisVerifyResult *
measure_as_worker(
	Fixture             *fx,
	const AxisWorkerJob *job
){
	const AxisVerifyWork *work = job->verify;
	g_autofree gchar *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree gchar *ffprobe = g_find_program_in_path("ffprobe");
	g_autofree gchar *src_path = g_build_filename(fx->disk, work->root, work->path, NULL);
	g_autofree gchar *out_path = work_path(fx, work->output);
	AxisVerifyResult *result = g_new0(AxisVerifyResult, 1);
	AxisVerifyEvidence *e = axis_verify_evidence_new();
	const gchar *programs[2];
	const gchar *probes[2];
	AxisVerifyTools tools;
	gint src;
	gint out;
	guint k;

	programs[0] = ffmpeg;
	programs[1] = NULL;
	probes[0] = ffprobe;
	probes[1] = NULL;
	memset(&tools, 0, sizeof(tools));
	tools.ffmpeg = programs;
	tools.ffprobe = probes;
	tools.scratch = fx->scratch;
	src = open(src_path, O_RDONLY | O_CLOEXEC);
	out = open(out_path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(src, >=, 0);
	g_assert_cmpint(out, >=, 0);
	axis_verify_measure_decode(&tools, work->plan, out, e);
	axis_verify_measure_packets(&tools, work->plan, src, out, e);
	axis_verify_measure_tracks(&tools, work->plan, src, e);
	for (k = 0; k < work->plan->n_points; k++)
		axis_verify_measure_point(&tools, work->plan, k, src, out, e);
	g_assert_true(axis_file_whole_digest(out, work->output_size, NULL, &e->output_digest, NULL));
	g_assert_true(axis_file_sample_digest(out, work->output_size, &e->output_sample, NULL));
	g_assert_true(axis_file_sample_digest(src, work->size, &e->source_sample, NULL));
	close(src);
	close(out);
	result->output = g_strdup(work->output);
	result->output_size = work->output_size;
	result->evidence = e;
	return result;
}

static guint
complete_checks(
	Fixture                *fx,
	const gchar            *token,
	const AxisWorkerJob    *job,
	const AxisVerifyResult *result
){
	g_autofree gchar *json = NULL;
	AxisWorkerComplete request;
	AxisTestReply reply;
	guint status;

	memset(&request, 0, sizeof(request));
	request.job = job->id;
	request.lease_token = job->lease_token;
	request.kind = (gchar *)AXIS_WORKER_KIND_VERIFY;
	request.verify = (AxisVerifyResult *)result;
	json = axis_worker_complete_to_json(&request);
	g_assert_nonnull(json);
	reply = axis_test_daemon_post(fx->d, "POST", "complete", token, NULL, json, NULL);
	status = reply.status;
	axis_test_reply_clear(&reply);
	return status;
}

/* A new movie converted by "garage" as told, then the finish job's first
 * turn: checks 1 to 4 made here and the rest handed to a worker; the check
 * job in *@checks. */
static gint64
handed_out(
	Fixture               *fx,
	AxisConversionOutcome  outcome,
	guint                 *number,
	gint64                *checks
){
	AxisWorkerJob *job = NULL;
	gint64 id = requested_and_claimed(fx, "movie.mkv", "space_saver", outcome, number, &job);
	guint k;

	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	for (k = 1; k <= 4; k++) {
		g_autofree gchar *result = check_of(fx, id, k);

		g_assert_cmpstr(result, ==, "passed");
	}
	{
		g_autofree gchar *fifth = check_of(fx, id, 5);

		g_assert_cmpstr(fifth, ==, "none");
	}
	*checks = axis_test_library_n(fx->t, "SELECT coalesce(verify_job_id, 0) FROM conversion WHERE id = %"
		G_GINT64_FORMAT " AND checked_where = 'worker' AND check_plan IS NOT NULL AND check_source_sample IS NOT NULL"
		" AND handed_inode IS NOT NULL", id);
	g_assert_cmpint(*checks, >, 0);
	return id;
}

static gchar *
record_text(
	Fixture     *fx,
	gint64       id,
	const gchar *column
){
	return axis_test_library_q(fx->t, "SELECT coalesce(%s::text, '') FROM conversion WHERE id = %" G_GINT64_FORMAT,
		column, id);
}

static void
assert_record(
	Fixture     *fx,
	gint64       id,
	const gchar *column,
	const gchar *expected
){
	g_autofree gchar *got = record_text(fx, id, column);

	if (g_strcmp0(got, expected) != 0)
		g_error("conversion %" G_GINT64_FORMAT " %s: expected \"%s\", got \"%s\"", id, column, expected, got);
}

/* The worker heard from long ago: not connected for the checks. */
static void
gone_quiet(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *statement = g_strdup_printf("UPDATE worker SET last_seen_at = now() - interval '1 hour'"
		" WHERE name = '%s'", name);

	sql(fx, statement);
}

/* Spec (conversion-execution): with convert.checks_where "worker", checks 1
 * to 4 are made here, the measurements of 5 to 8 handed to a worker, and the
 * daemon decides every check from them; the page names who converted and
 * who checked. */
static void
test_checks_on_worker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movies = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(AxisVerifyResult) result = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 checks;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	before = axis_test_storage_digest(movies);
	assert_record(fx, id, "checks_waiting", "Waiting for a worker to check it.");
	/* One worker only: nobody else to prefer. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND kind = 'convert.verify' AND state = 'queued' AND payload->'requires'->>'prefer_not' IS NULL", checks),
		==, 1);
	/* While it waits, a finish job does nothing more. */
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	{
		g_autofree gchar *fifth = check_of(fx, id, 5);

		g_assert_cmpstr(fifth, ==, "none");
	}

	job = claim_checks(fx, fx->token);
	g_assert_cmpint(job->id, ==, checks);
	g_assert_cmpuint(job->verify->plan->n_tracks, ==, 1);
	g_assert_cmpuint(job->verify->plan->n_points, ==, 2);
	result = measure_as_worker(fx, job);
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
	assert_record(fx, id, "verifier", "garage");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND evidence IS NOT NULL", id), ==, 1);
	axis_worker_job_free(job);

	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "checked_where", "worker");
	{
		g_autofree gchar *detail = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE"
			" conversion_id = %" G_GINT64_FORMAT " AND number = 5", id);

		g_assert_cmpstr(detail, ==, "The picture and every sound track decode from start to end without an error.");
	}
	/* A trial: the library as it was. */
	after = axis_test_storage_digest(movies);
	g_assert_cmpstr(before, ==, after);
}

/* Spec (conversion-execution): a worker's measurements that are not of the
 * files on this disk are not used: nothing is placed, the output goes and
 * the original stays; with convert.confirm_whole_file off, only the stamp
 * and the samples are compared. */
static void
test_checks_not_of_this_file(Fixture *fx, gconstpointer data)
{
	static const gchar *const said[] = {
		"is not the one the worker read", "samples of the original the worker read",
		"changed after its checks were handed out"
	};
	g_autofree gchar *movies = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint c;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	movies = g_build_filename(fx->disk, "Movies", NULL);
	for (c = 0; c < G_N_ELEMENTS(said); c++) {
		g_autoptr(AxisVerifyResult) result = NULL;
		g_autofree gchar *output = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		AxisWorkerJob *job;
		guint number;
		gint64 checks;
		gint64 id;
		guint k;

		id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
		g_clear_pointer(&before, g_free);
		before = axis_test_storage_digest(movies);
		output = record_text(fx, id, "output_name");
		path = work_path(fx, output);
		if (c == 2) {
			/* Written after it was handed out (the same byte again: the
			 * content is as it was, its change time is not). */
			gint fd = open(path, O_RDWR | O_CLOEXEC);
			gchar byte = 0;

			g_assert_cmpint(fd, >=, 0);
			g_usleep(20000);
			g_assert_cmpint(pread(fd, &byte, 1, 100), ==, 1);
			g_assert_cmpint(pwrite(fd, &byte, 1, 100), ==, 1);
			close(fd);
		}
		job = claim_checks(fx, fx->token);
		result = measure_as_worker(fx, job);
		if (c == 0) {
			g_free(result->evidence->output_digest);
			result->evidence->output_digest = g_strnfill(64, 'f');
		} else if (c == 1) {
			g_free(result->evidence->source_sample);
			result->evidence->source_sample = g_strnfill(64, 'e');
		}
		/* Well formed and fitting: taken, then found not of this file. */
		g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
		axis_worker_job_free(job);
		finish_ok(fx, id);
		assert_state(fx, id, "failed");
		text = record_text(fx, id, "result_text");
		if (strstr(text, "The measurements made on worker garage are not of this file") == NULL ||
		    strstr(text, said[c]) == NULL)
			g_error("case %u: %s", c, text);
		g_assert_false(exists(path));
		for (k = 5; k <= AXIS_CONVERSION_N_CHECKS; k++) {
			g_autofree gchar *result_k = check_of(fx, id, k);

			g_assert_cmpstr(result_k, ==, "skipped");
		}
		g_clear_pointer(&after, g_free);
		after = axis_test_storage_digest(movies);
		g_assert_cmpstr(before, ==, after);
	}

	/* Off: the whole file is not read here, so a wrong whole digest is not
	 * seen (the stamp and the samples are still compared). */
	set(fx, AXIS_CONVERSION_SETTING_CONFIRM_WHOLE, "false");
	{
		g_autoptr(AxisVerifyResult) result = NULL;
		AxisWorkerJob *job;
		guint number;
		gint64 checks;
		gint64 id;

		id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
		job = claim_checks(fx, fx->token);
		result = measure_as_worker(fx, job);
		g_free(result->evidence->output_digest);
		result->evidence->output_digest = g_strnfill(64, 'f');
		g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
		axis_worker_job_free(job);
		finish_ok(fx, id);
		assert_state(fx, id, "done");
		assert_all_passed(fx, id);
	}
}

/* Spec (worker-protocol): measurements that do not fit the plan the worker
 * was given, or of another file, are refused and nothing of them is kept;
 * a truthful report is then taken. */
static void
test_checks_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisVerifyResult) result = NULL;
	gchar **hashes;
	gchar *output;
	AxisWorkerJob *job;
	guint number;
	gint64 checks;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "worker API: worker garage's measurements*are refused*");
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	job = claim_checks(fx, fx->token);
	result = measure_as_worker(fx, job);

	/* One hash more than the plan copies tracks. */
	hashes = result->evidence->output_hashes;
	result->evidence->output_hashes = g_new0(gchar *, 3);
	result->evidence->output_hashes[0] = g_strdup(hashes[0]);
	result->evidence->output_hashes[1] = g_strnfill(64, 'a');
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 400);
	g_strfreev(result->evidence->output_hashes);
	result->evidence->output_hashes = hashes;
	/* Another file's. */
	output = result->output;
	result->output = g_strdup("999-1.tmp");
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 400);
	g_free(result->output);
	result->output = output;
	result->output_size--;
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 400);
	result->output_size++;
	/* Nothing was kept; the job is still the worker's. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND evidence IS NULL AND verifier IS NULL", id), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND state = 'running'", checks), ==, 1);

	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
}

/* A worker's failure report (retry) for @job. */
static void
fail_checks_job(
	Fixture             *fx,
	const AxisWorkerJob *job
){
	g_autofree gchar *json = NULL;
	AxisWorkerFail fail;
	AxisTestReply reply;

	memset(&fail, 0, sizeof(fail));
	fail.job = job->id;
	fail.lease_token = job->lease_token;
	fail.error = (gchar *)"the decode could not be measured: the tool did not finish in time";
	fail.retry = TRUE;
	json = axis_worker_fail_to_json(&fail);
	reply = axis_test_daemon_post(fx->d, "POST", "fail", fx->token, NULL, json, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
}

/* The check job @checks, claimed again once its back-off is over. */
static AxisWorkerJob *
claim_checks_again(
	Fixture *fx,
	gint64   checks
){
	gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < until) {
		AxisWorkerJob *job = claim_as(fx, fx->token, AXIS_WORKER_KIND_VERIFY);

		if (job != NULL) {
			g_assert_cmpint(job->id, ==, checks);
			return job;
		}
		axis_test_pump_for(100);
	}
	g_error("the check job %" G_GINT64_FORMAT " was not offered again", checks);
	return NULL;
}

/* Spec (conversion-execution): "worker_only": this server does no heavy
 * work; with no worker the conversion waits and says why; when the attempts
 * on workers are used up the conversion fails, the original untouched. */
static void
test_checks_only_on_worker(Fixture *fx, gconstpointer data)
{
	static const gchar *const no_tool[] = { "/bin/false", NULL };
	g_autoptr(AxisVerifyResult) result = NULL;
	g_autofree gchar *waiting = NULL;
	AxisConversionTuning tuning;
	AxisWorkerJob *job;
	guint number;
	gint64 checks;
	gint64 id;
	guint i;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker_only");
	do_register(fx);
	{
		AxisWorkerJob *encode = NULL;

		id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &encode);
		encode_and_report(fx, encode, NULL);
		axis_worker_job_free(encode);
	}
	/* This server's ffmpeg would fail every check it ran from here on: none
	 * of 5 to 8 is measured here. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.ffmpeg = no_tool;
	tuning.software_preset = "fast";
	tuning.threads = 2;
	tuning.tool_timeout_ms = 120000;
	tuning.similarity_points = 2;
	tuning.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &tuning);

	gone_quiet(fx, "garage");
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	waiting = record_text(fx, id, "checks_waiting");
	assert_says(waiting, "Waiting for a worker to check it: no connected worker can reach its disk, and this "
		"server does no heavy work");
	finish_ok(fx, id);
	{
		g_autofree gchar *fifth = check_of(fx, id, 5);

		g_assert_cmpstr(fifth, ==, "none");
	}
	/* A worker comes: it waits for it to take the checks. */
	do_register(fx);
	finish_ok(fx, id);
	assert_record(fx, id, "checks_waiting", "Waiting for a worker to check it.");
	job = claim_checks(fx, fx->token);
	result = measure_as_worker(fx, job);
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);

	/* Three attempts on workers that end without measurements: the
	 * conversion fails, its output goes, the original stays. */
	{
		g_autofree gchar *movies = g_build_filename(fx->disk, "Movies", NULL);
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		g_autofree gchar *output = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;

		axis_conversions_set_tuning(fx->conv, &tuning);
		{
			AxisConversionTuning real = tuning;

			real.ffmpeg = NULL;
			axis_conversions_set_tuning(fx->conv, &real);
			id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
			axis_conversions_set_tuning(fx->conv, &tuning);
		}
		before = axis_test_storage_digest(movies);
		output = record_text(fx, id, "output_name");
		path = work_path(fx, output);
		for (i = 0; i < AXIS_CONVERSION_VERIFY_ATTEMPTS; i++) {
			job = i == 0 ? claim_checks(fx, fx->token) : claim_checks_again(fx, checks);
			fail_checks_job(fx, job);
			axis_worker_job_free(job);
		}
		g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
			" AND state = 'failed'", checks), ==, 1);
		finish_ok(fx, id);
		assert_state(fx, id, "failed");
		text = record_text(fx, id, "result_text");
		assert_says(text, "The checks could not be measured on a worker");
		g_assert_false(exists(path));
		after = axis_test_storage_digest(movies);
		g_assert_cmpstr(before, ==, after);
	}
}

/* Spec (conversion-execution): "worker" measures here when no worker is
 * connected, when the one there was goes before taking the checks, and when
 * the attempts on workers are used up. */
static void
test_checks_fallback(Fixture *fx, gconstpointer data)
{
	AxisWorkerJob *job = NULL;
	guint number;
	gint64 checks;
	gint64 id;
	guint i;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	gone_quiet(fx, "garage");
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "checked_where", "server");

	/* Handed out, then the worker went before it took them. */
	do_register(fx);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	gone_quiet(fx, "garage");
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "checked_where", "server");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND state = 'cancelled'", checks), ==, 1);

	/* Its attempts used up on a worker: measured here. */
	do_register(fx);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	for (i = 0; i < AXIS_CONVERSION_VERIFY_ATTEMPTS; i++) {
		job = i == 0 ? claim_checks(fx, fx->token) : claim_checks_again(fx, checks);
		fail_checks_job(fx, job);
		axis_worker_job_free(job);
	}
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "checked_where", "server");
}

/* Spec (conversion-execution, worker-protocol): a worker that goes away
 * while it checks loses the job, which is offered again; what it says late
 * is refused; the next attempt's measurements are taken. */
static void
test_checks_worker_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisVerifyResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisWorkerJob *job;
	AxisWorkerJob *again;
	AxisDbConn *conn;
	guint number;
	gint64 checks;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	job = claim_checks(fx, fx->token);
	result = measure_as_worker(fx, job);
	/* Gone silent: its lease is taken back as the reaper does it. */
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_job_expire_remote(conn, job->id, job->lease_token, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT attempts FROM job WHERE id = %" G_GINT64_FORMAT
		" AND state = 'queued'", checks), ==, 1);
	/* Its late report changes nothing. */
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 409);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
		" AND evidence IS NULL", id), ==, 1);

	again = claim_checks_again(fx, checks);
	g_assert_cmpuint(again->attempt, ==, 2);
	g_assert_cmpuint(complete_checks(fx, fx->token, again, result), ==, 200);
	axis_worker_job_free(job);
	axis_worker_job_free(again);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
}

/* Spec (conversion-execution, job-queue): after the daemon starts with
 * convert.wait_after_start on, no encode, check or sample measurement is
 * handed out or run until a person resumes conversion work; the pages say
 * so. */
static void
test_held_after_start(Fixture *fx, gconstpointer data)
{
	const gchar *const *held = axis_conversion_held_kinds();
	g_autoptr(AxisVerifyResult) result = NULL;
	g_autofree gchar *admin = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *waiting = NULL;
	AxisWorkerJob *job;
	gboolean was_held = FALSE;
	gint64 since = 0;
	guint number;
	gint64 file;
	gint64 id;
	guint i;

	SKIP_UNLESS_READY(fx);
	for (i = 0; held[i] != NULL; i++) {
		/* The daemon's own sample measurement is not a kind of this test's
		 * registry. */
		if (g_strcmp0(held[i], AXIS_ESTIMATE_SAMPLE_KIND) == 0)
			continue;
		g_assert_true(axis_job_registry_set_hold(fx->d->registry, held[i], axis_conversions_get_hold(fx->conv),
			&error));
		g_assert_no_error(error);
	}
	axis_web_workers_set_conversions(fx->d->workers_ui, fx->conv);
	admin = axis_web_auth_open_test_session(fx->d->auth, "root", AXIS_USER_ROLE_ADMIN);
	do_register(fx);

	/* Off: nothing is held. */
	set(fx, AXIS_CONVERSION_SETTING_WAIT_AFTER_START, "false");
	axis_conversions_hold_after_start(fx->conv);
	g_assert_false(axis_conversions_is_held(fx->conv, NULL));
	/* On (the default): held from now. */
	set(fx, AXIS_CONVERSION_SETTING_WAIT_AFTER_START, "true");
	axis_conversions_hold_after_start(fx->conv);
	g_assert_true(axis_conversions_is_held(fx->conv, &since));
	g_assert_cmpint(since, >, 0);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	g_assert_null(claim(fx));
	g_assert_null(claim_as(fx, fx->token, AXIS_WORKER_KIND_SAMPLE));
	{
		AxisTestReply page = axis_test_web_request(fx->d->session, fx->d->port, "GET", "/workers", admin, NULL, NULL);

		g_assert_cmpuint(page.status, ==, 200);
		assert_says(page.body, "Conversion work has been paused since Axis started");
		assert_says(page.body, "Resume conversion work");
		axis_test_reply_clear(&page);
	}

	/* Resumed: the encode is handed out. */
	g_assert_true(axis_conversions_resume(fx->conv, "root", &was_held, &error));
	g_assert_no_error(error);
	g_assert_true(was_held);
	g_assert_false(axis_conversions_is_held(fx->conv, NULL));
	job = claim_work(fx);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);

	/* A restart with the conversion being checked: its checks wait. */
	axis_conversions_hold_after_start(fx->conv);
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	{
		g_autofree gchar *first = check_of(fx, id, 1);

		g_assert_cmpstr(first, ==, "none");
	}
	waiting = record_text(fx, id, "checks_waiting");
	assert_says(waiting, "conversion work has been paused since Axis started");

	/* Checks on a worker wait as well. */
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	g_assert_true(axis_conversions_resume(fx->conv, "root", &was_held, &error));
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	axis_conversions_hold_after_start(fx->conv);
	g_assert_null(claim_as(fx, fx->token, AXIS_WORKER_KIND_VERIFY));
	g_assert_true(axis_conversions_resume(fx->conv, "root", &was_held, &error));
	job = claim_checks(fx, fx->token);
	result = measure_as_worker(fx, job);
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");

	/* Resuming what is not held says so. */
	g_assert_true(axis_conversions_resume(fx->conv, "root", &was_held, &error));
	g_assert_false(was_held);
}

/* Spec (worker-protocol): with another suitable worker connected, the
 * checks go to it first; the record names who converted and who checked. */
static void
test_checks_prefer_other(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisVerifyResult) result = NULL;
	gchar *shed = NULL;
	AxisWorkerJob *job;
	guint number;
	gint64 checks;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	shed = axis_test_daemon_add_worker(fx->d, "shed", NULL);
	do_register(fx);
	do_register_as(fx, shed);
	id = handed_out(fx, AXIS_CONVERSION_TRIAL, &number, &checks);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND payload->'requires'->>'prefer_not' = 'garage'"
		" AND (payload->'requires'->>'prefer_not_until')::bigint > extract(epoch FROM now())", checks), ==, 1);
	g_assert_null(claim_as(fx, fx->token, AXIS_WORKER_KIND_VERIFY));
	job = claim_checks(fx, shed);
	result = measure_as_worker(fx, job);
	g_assert_cmpuint(complete_checks(fx, shed, job, result), ==, 200);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_record(fx, id, "worker", "garage");
	assert_record(fx, id, "verifier", "shed");
	axis_secret_free(shed);
}

/* Spec (worker-protocol): a real axis-worker converts and checks; the
 * Workers page says what workers do in each mode. */
static void
test_checks_real_worker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *admin = NULL;
	AxisTestWorker *w;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	w = start_worker(fx);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "verifying");
	finish_ok(fx, id);
	{
		g_autofree gchar *statement = g_strdup_printf("SELECT (evidence IS NOT NULL)::text FROM conversion WHERE id = %"
			G_GINT64_FORMAT, id);

		if (!axis_test_wait_query(fx->t->db, statement, "true", LONG_WAIT_MS))
			g_error("the worker did not measure the checks; its log:\n%s", axis_test_worker_get_log(w));
	}
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "verifier", "garage");
	assert_record(fx, id, "checked_where", "worker");
	stop_worker(fx, w);

	axis_web_workers_set_conversions(fx->d->workers_ui, fx->conv);
	admin = axis_web_auth_open_test_session(fx->d->auth, "root", AXIS_USER_ROLE_ADMIN);
	{
		static const struct
		{
			const gchar *mode;
			const gchar *said;
		} modes[] = {
			{ "server", "this server then checks it itself" },
			{ "worker", "with no such worker this server measures them itself" },
			{ "worker_only", "this server does no heavy work, and without a worker conversions wait" }
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS(modes); i++) {
			AxisTestReply page;

			set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, modes[i].mode);
			page = axis_test_web_request(fx->d->session, fx->d->port, "GET", "/workers", admin, NULL, NULL);
			g_assert_cmpuint(page.status, ==, 200);
			assert_says(page.body, modes[i].said);
			assert_says(page.body, axis_conversion_checks_where_label(axis_conversions_checks_where(fx->conv)));
			axis_test_reply_clear(&page);
		}
	}
}

/* Spec (worker-protocol): a measuring ffmpeg that dies of a signal fails the
 * check job for another try and pauses its worker. */
static void
test_checks_crash_pauses(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *flag = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *expected = NULL;
	AxisTestWorker *w;
	guint number;
	gint64 checks;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker_only");
	flag = g_build_filename(fx->t->dir, "fail-how", NULL);
	ffmpeg = axis_test_failing_ffmpeg(fx->t->dir, flag);
	w = start_worker_with(fx, ffmpeg);
	file = add_movie(fx, "movie.mkv", &number);
	id = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);
	wait_state(fx, w, id, "verifying");
	/* From now on its ffmpeg crashes. */
	axis_test_storage_write(flag, "SEGV");
	finish_ok(fx, id);
	checks = axis_test_library_n(fx->t, "SELECT coalesce(verify_job_id, 0) FROM conversion WHERE id = %" G_GINT64_FORMAT,
		id);
	g_assert_cmpint(checks, >, 0);
	expected = g_strdup_printf("%" G_GINT64_FORMAT " signal 11, SIGSEGV", checks);
	if (!axis_test_wait_query(fx->t->db, "SELECT paused_job || ' ' || paused_reason FROM worker WHERE name = 'garage'",
	        expected, LONG_WAIT_MS))
		g_error("the worker was not paused; its log:\n%s", axis_test_worker_get_log(w));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "ffmpeg crashed while it measured the decode (signal 11, "
		"SIGSEGV)"));
	/* The job is tried again; the conversion still waits; the original is
	 * as it was. */
	{
		g_autofree gchar *statement = g_strdup_printf("SELECT state || ' ' || attempts FROM job WHERE id = %"
			G_GINT64_FORMAT, checks);

		g_assert_true(axis_test_wait_query(fx->t->db, statement, "queued 1", AXIS_TEST_WAIT_MS));
	}
	assert_state(fx, id, "verifying");
	g_assert_cmpint(g_unlink(flag), ==, 0);
	stop_worker(fx, w);
}

/* Spec (conversion-execution): every ffmpeg of the checks this server runs
 * asks for convert.check_threads threads for each input and for its
 * filters, and runs at the lowest CPU priority. */
static void
test_checks_threads(Fixture *fx, gconstpointer data)
{
	/* The checks' tools may write no file (a size limit of 0): the stand-in
	 * notes each call by an empty file's name, which writes nothing. */
	static const gchar script_text[] =
		"# A stand-in for ffmpeg: notes what kind of check called it, whether\n"
		"# it asked for three threads, and its niceness; then runs the real one.\n"
		"dir=$1\n"
		"shift\n"
		"args=\" $* \"\n"
		"kind=other\n"
		"case \"$args\" in\n"
		"*' -xerror '*) kind=decode; need=' -filter_threads 3 ' ;;\n"
		"*'ssim='*) kind=point; need=' -filter_complex_threads 3 ' ;;\n"
		"*' streamhash '*) kind=tracks; need=' -threads 3 ' ;;\n"
		"esac\n"
		"if [ \"$kind\" != other ]; then\n"
		"\tok=ok\n"
		"\tcase \"$args\" in *' -threads 3 '*) ;; *) ok=bad ;; esac\n"
		"\tcase \"$args\" in *\"$need\"*) ;; *) ok=bad ;; esac\n"
		"\t: > \"$dir/$kind-$ok-$(nice)-$$\"\n"
		"fi\n"
		"exec ffmpeg \"$@\"\n";
	g_autofree gchar *script = NULL;
	g_autofree gchar *calls = NULL;
	g_autoptr(GDir) dir = NULL;
	AxisConversionTuning tuning;
	AxisWorkerJob *job = NULL;
	const gchar *tool[4];
	const gchar *name;
	guint decodes = 0;
	guint points = 0;
	guint tracks = 0;
	guint number;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	script = g_build_filename(fx->t->dir, "recording-ffmpeg.sh", NULL);
	calls = g_build_filename(fx->t->dir, "ffmpeg-calls", NULL);
	g_assert_true(g_file_set_contents(script, script_text, -1, NULL));
	g_assert_cmpint(g_chmod(script, 0700), ==, 0);
	axis_test_storage_mkdir(calls);
	tool[0] = "/bin/sh";
	tool[1] = script;
	tool[2] = calls;
	tool[3] = NULL;
	memset(&tuning, 0, sizeof(tuning));
	tuning.ffmpeg = tool;
	tuning.software_preset = "fast";
	tuning.threads = 2;
	tuning.tool_timeout_ms = 120000;
	tuning.similarity_points = 2;
	tuning.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &tuning);
	set(fx, AXIS_CONVERSION_SETTING_CHECK_THREADS, "3");
	do_register(fx);

	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);

	dir = g_dir_open(calls, 0, NULL);
	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL) {
		/* ok: three threads for each input and for the filters; 19: the
		 * lowest CPU priority. */
		if (g_str_has_prefix(name, "decode-ok-19-"))
			decodes++;
		else if (g_str_has_prefix(name, "point-ok-19-"))
			points++;
		else if (g_str_has_prefix(name, "tracks-ok-19-"))
			tracks++;
		else
			g_error("a check's ffmpeg without its threads or its priority: %s", name);
	}
	g_assert_cmpuint(decodes, ==, 1);
	g_assert_cmpuint(points, ==, 2);
	g_assert_cmpuint(tracks, ==, 1);
}

/* ── fix-conversion-hunt-findings ───────────────────────────────── */

/* Spec (conversion-execution, Checks that passed are not judged again;
 * hunt finding H1): with the checks measured on a worker, a finish job that
 * fails after it settled the output (here a scan holds the volume past the
 * placement's wait; a database error or a daemon stopped at that moment do
 * the same) is tried again, and places the same file: the worker's
 * measurements are not bound again to the stamp the output had when they
 * were handed out, which settling (owner, mode, time) changed. */
static void
test_checks_on_worker_retried(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisVerifyResult) result = NULL;
	g_autofree gchar *volume = NULL;
	g_autofree gchar *path = NULL;
	const gchar *params[1];
	AxisWorkerJob *job;
	AxisDbConn *scanner;
	guint number;
	gint64 checks;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	set(fx, AXIS_CONVERSION_SETTING_CHECKS_WHERE, "worker");
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	id = handed_out(fx, AXIS_CONVERSION_REPLACE, &number, &checks);
	path = movie_path(fx, number, "mkv");
	job = claim_checks(fx, fx->token);
	result = measure_as_worker(fx, job);
	g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
	axis_worker_job_free(job);

	/* A scan holds the volume: every check passes, the output is settled,
	 * and the placement gives up for now. */
	scanner = axis_db_pool_acquire(fx->t->pool, &error);
	volume = g_strdup_printf("%" G_GINT64_FORMAT, fx->volume);
	params[0] = volume;
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_lock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	g_assert_false(finish(fx, id, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY);
	g_clear_error(&error);
	assert_state(fx, id, "verifying");
	assert_all_passed(fx, id);
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_unlock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	axis_db_pool_release(fx->t->pool, scanner);

	/* The scan ended and the finish job is tried again: the same converted
	 * file, unchanged since its checks passed, is put in place; checks 5 to
	 * 8 keep what the worker measured. */
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	g_assert_cmpint(inode_of(path), ==, axis_test_library_n(fx->t, "SELECT output_inode FROM conversion WHERE id = %"
		G_GINT64_FORMAT, id));
	{
		g_autofree gchar *first = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE conversion_id = %"
			G_GINT64_FORMAT " AND number = 1", id);
		g_autofree gchar *fifth = axis_test_library_q(fx->t, "SELECT detail FROM conversion_check WHERE conversion_id = %"
			G_GINT64_FORMAT " AND number = 5", id);

		g_assert_nonnull(strstr(first, "checks 3 to 8 are not made again"));
		g_assert_cmpstr(fifth, ==, "The picture and every sound track decode from start to end without an error.");
	}

	/* Not the same file any more (touched by hand): checked from the start,
	 * which for a worker's measurements means they are not of this file. */
	{
		guint other;
		gint64 again;
		g_autofree gchar *out = NULL;
		struct timespec times[2];

		again = handed_out(fx, AXIS_CONVERSION_REPLACE, &other, &checks);
		job = claim_checks(fx, fx->token);
		g_clear_pointer(&result, axis_verify_result_free);
		result = measure_as_worker(fx, job);
		g_assert_cmpuint(complete_checks(fx, fx->token, job, result), ==, 200);
		out = work_path(fx, job->verify->output);
		axis_worker_job_free(job);
		fx->stop_at = "settle";
		fx->stop_how = AXIS_CONVERSION_HOOK_CRASH;
		g_assert_false(finish(fx, again, &error));
		g_clear_error(&error);
		times[0].tv_sec = 0;
		times[0].tv_nsec = UTIME_OMIT;
		times[1].tv_sec = 1000000000;
		times[1].tv_nsec = 0;
		g_assert_cmpint(utimensat(AT_FDCWD, out, times, 0), ==, 0);
		finish_ok(fx, again);
		assert_state(fx, again, "failed");
		{
			g_autofree gchar *text = record_text(fx, again, "result_text");

			g_assert_nonnull(strstr(text, "not of this file"));
		}
	}
}

/* A replacement whose checks, measured on this server by a stand-in ffmpeg
 * that notes each heavy measurement as an empty file in *@calls (a decode,
 * a picture comparison), all passed, and whose placement then gave up for
 * now (a scan held the volume past its wait): its pass is kept, settled.
 * *@out is the converted file. */
static gint64
passed_then_busy(
	Fixture  *fx,
	gchar   **calls,
	gchar   **out
){
	/* The checks' tools may write no file: the stand-in notes each call by
	 * an empty file's name. */
	static const gchar script_text[] =
		"dir=$1\n"
		"shift\n"
		"args=\" $* \"\n"
		"case \"$args\" in\n"
		"*' -xerror '*) : > \"$dir/decode-$$\" ;;\n"
		"*'ssim='*) : > \"$dir/point-$$\" ;;\n"
		"esac\n"
		"exec ffmpeg \"$@\"\n";
	g_autoptr(GError) error = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *volume = NULL;
	AxisConversionTuning tuning;
	AxisWorkerJob *job = NULL;
	AxisDbConn *scanner;
	const gchar *params[1];
	const gchar *tool[4];
	guint number;
	gint64 id;

	/* The tuning keeps copies of the tool's words. */
	script = g_build_filename(fx->t->dir, "noting-ffmpeg.sh", NULL);
	*calls = g_build_filename(fx->t->dir, "ffmpeg-calls", NULL);
	g_assert_true(g_file_set_contents(script, script_text, -1, NULL));
	axis_test_storage_mkdir(*calls);
	tool[0] = "/bin/sh";
	tool[1] = script;
	tool[2] = *calls;
	tool[3] = NULL;
	memset(&tuning, 0, sizeof(tuning));
	tuning.ffmpeg = tool;
	tuning.software_preset = "fast";
	tuning.threads = 2;
	tuning.tool_timeout_ms = 120000;
	tuning.similarity_points = 2;
	tuning.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &tuning);
	do_register(fx);
	prove_on_devices(fx, revision_of(fx, "space_saver"));
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_REPLACE, &number, &job);
	*out = work_path(fx, job->convert->output);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);

	scanner = axis_db_pool_acquire(fx->t->pool, &error);
	volume = g_strdup_printf("%" G_GINT64_FORMAT, fx->volume);
	params[0] = volume;
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_lock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	g_assert_false(finish(fx, id, &error));
	g_assert_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY);
	g_clear_error(&error);
	g_assert_true(axis_db_conn_exec(scanner, "SELECT pg_advisory_unlock(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1, params,
		NULL, &error));
	axis_db_pool_release(fx->t->pool, scanner);
	assert_state(fx, id, "verifying");
	assert_all_passed(fx, id);
	return id;
}

/* Spec (conversion-execution, Checks that passed are not judged again;
 * hunt finding S3): with the checks measured on this server, a finish job
 * tried again after every check passed (a scan held the volume) makes none
 * of the heavy measurements again: no second decode, no second picture
 * comparison. */
static void
test_retry_measures_nothing_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *calls = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	id = passed_then_busy(fx, &calls, &out);
	before = listing(calls);
	g_assert_nonnull(strstr(before, "decode-"));
	g_assert_nonnull(strstr(before, "point-"));

	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	after = listing(calls);
	g_assert_cmpstr(after, ==, before);
}

/* Review of fix-conversion-hunt-findings, item 1 (conversion-execution,
 * Checks that passed are not judged again; job-queue, conversion work held
 * after a restart): a finish job tried again while conversion work is held,
 * for a conversion whose checks all passed but whose file is not that file
 * any more (touched since), measures nothing: it waits, saying why, and is
 * checked from the start once conversion work is resumed. */
static void
test_retry_held_not_same(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *calls = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	struct timespec times[2];
	gboolean was_held = FALSE;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	id = passed_then_busy(fx, &calls, &out);
	before = listing(calls);
	times[0].tv_sec = 0;
	times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = 1000000000;
	times[1].tv_nsec = 0;
	g_assert_cmpint(utimensat(AT_FDCWD, out, times, 0), ==, 0);

	/* The daemon restarted with convert.wait_after_start on. */
	set(fx, AXIS_CONVERSION_SETTING_WAIT_AFTER_START, "true");
	axis_conversions_hold_after_start(fx->conv);
	g_assert_true(axis_conversions_is_held(fx->conv, NULL));
	finish_ok(fx, id);
	assert_state(fx, id, "verifying");
	{
		g_autofree gchar *waiting = record_text(fx, id, "checks_waiting");

		g_assert_nonnull(strstr(waiting, "conversion work has been paused since Axis started"));
	}
	after = listing(calls);
	g_assert_cmpstr(after, ==, before);

	/* Resumed: checked from the start, and placed. */
	g_assert_true(axis_conversions_resume(fx->conv, "root", &was_held, &error));
	g_assert_no_error(error);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	g_clear_pointer(&after, g_free);
	after = listing(calls);
	g_assert_cmpstr(after, !=, before);
}

/* Review of fix-conversion-hunt-findings, items 2 and 3 (conversion-execution,
 * Rejections and passes by a rule found wrong since): a finish job tried
 * again for a conversion whose checks all passed by rules that a check was
 * found too lenient in since is not placed on that pass: the file is checked
 * from the start (measured again), then placed. */
static void
test_retry_after_lenient_mark(Fixture *fx, gconstpointer data)
{
	static const AxisConversionMark lenient[] = {
		{ 7, AXIS_CONVERSION_MARK_LENIENT, AXIS_CONVERSION_CHECKS_VERSION, "it compared too few points of the picture" }
	};
	g_autofree gchar *calls = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	id = passed_then_busy(fx, &calls, &out);
	before = listing(calls);
	/* Its pass judged by the version before this build's, which a mark says
	 * was too lenient in check 7. */
	assert_record(fx, id, "checks_version", G_STRINGIFY(AXIS_CONVERSION_CHECKS_VERSION));
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE conversion SET checks_version = %d WHERE id = %"
			G_GINT64_FORMAT, AXIS_CONVERSION_CHECKS_VERSION - 1, id);

		sql(fx, statement);
	}
	axis_conversion_marks_set_for_test(lenient, G_N_ELEMENTS(lenient));
	finish_ok(fx, id);
	axis_conversion_marks_set_for_test(NULL, 0);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	assert_record(fx, id, "checks_version", G_STRINGIFY(AXIS_CONVERSION_CHECKS_VERSION));
	after = listing(calls);
	g_assert_cmpstr(after, !=, before);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*was found too lenient before version*checked again from "
		"the start*"), ==, 1);
}

/* Spec (conversion-execution, A plan judged by what it was made from; hunt
 * finding H5): a trial is judged against the plan made from what it was
 * handed out with.  A person changing convert.software_preset, or the
 * worker's measured encoders changing, while the trial waits for its checks
 * makes no difference: it passes.  When the plan still cannot be made as it
 * was given (Axis itself changed), the trial is not judged: it is neither a
 * pass nor a rejection by a check (the revision can still be proven by
 * hand), the output goes, the original stays, it says so, and it can be
 * asked for again. */
static void
test_plan_as_handed_out(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisConversionProving) proving = NULL;
	g_autofree gchar *machine = axis_worker_machine("garage");
	g_autofree gchar *original = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *out = NULL;
	AxisConversionTuning tuning;
	AxisWorkerJob *job = NULL;
	guint number;
	gint64 file;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	/* The preset from the setting, as on a real install. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.threads = 2;
	tuning.tool_timeout_ms = 120000;
	tuning.similarity_points = 2;
	tuning.scan_wait_ms = 300;
	axis_conversions_set_tuning(fx->conv, &tuning);
	set(fx, AXIS_CONVERSION_SETTING_PRESET, "fast");
	do_register(fx);

	/* A setting changed while it waits for its checks. */
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	set(fx, AXIS_CONVERSION_SETTING_PRESET, "slow");
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	set(fx, AXIS_CONVERSION_SETTING_PRESET, "fast");

	/* The worker's measured encoders changed (it registered again with
	 * another ffmpeg, say). */
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE encoder_capability SET usable = false, ten_bit = false,"
			" hdr10 = false, reason = 'measured again: not usable' WHERE machine = '%s'", machine);

		sql(fx, statement);
	}
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	do_register(fx);

	/* Axis changed between the encode and the checks (an upgrade): the
	 * plan made again from the records differs from the one given. */
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	original = movie_path(fx, number, "mkv");
	checksum = file_checksum(original);
	out = work_path(fx, job->convert->output);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE conversion SET args = replace(args, '\"-preset:0\",\"fast\"',"
			" '\"-preset:0\",\"veryfast\"') WHERE id = %" G_GINT64_FORMAT " AND args LIKE '%%\"-preset:0\",\"fast\"%%'",
			id);

		sql(fx, statement);
		g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM conversion WHERE id = %" G_GINT64_FORMAT
			" AND args LIKE '%%veryfast%%'", id), ==, 1);
	}
	finish_ok(fx, id);
	assert_state(fx, id, "failed");
	assert_record(fx, id, "retryable", "true");
	assert_record(fx, id, "failed_check", "");
	{
		g_autofree gchar *text = record_text(fx, id, "result_text");
		g_autofree gchar *third = check_of(fx, id, 3);
		g_autofree gchar *fifth = check_of(fx, id, 5);

		g_assert_nonnull(strstr(text, "Not judged"));
		g_assert_nonnull(strstr(text, "not a rejection by a check"));
		g_assert_cmpstr(third, ==, "skipped");
		g_assert_cmpstr(fifth, ==, "skipped");
	}
	g_assert_false(exists(out));
	{
		g_autofree gchar *now = file_checksum(original);

		g_assert_cmpstr(now, ==, checksum);
	}
	/* Not a bad trial: the revision can still be proven from its trials. */
	g_assert_true(axis_conversions_proving(fx->conv, revision_of(fx, "space_saver"), &proving, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(proving->bad_trials, ==, 0);
	/* And it can be asked for again. */
	file = axis_test_library_n(fx->t, "SELECT file_id FROM conversion WHERE id = %" G_GINT64_FORMAT, id);
	{
		gint64 again = request(fx, file, "space_saver", AXIS_CONVERSION_TRIAL);

		g_assert_cmpint(again, >, id);
		g_assert_true(axis_conversions_cancel(fx->conv, again, "root", &error));
		g_assert_no_error(error);
	}

	/* Handed out before 0049 (nothing recorded of what its plan was made
	 * from), and a setting changed since: the plan is made from the settings
	 * of now, and the difference cannot be told from a setting changed since,
	 * so it is not judged either: never a rejection by check 3, which would
	 * count against proving the revision for good (review item 5). */
	g_clear_pointer(&proving, axis_conversion_proving_free);
	g_clear_pointer(&original, g_free);
	g_clear_pointer(&checksum, g_free);
	g_clear_pointer(&out, g_free);
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	original = movie_path(fx, number, "mkv");
	checksum = file_checksum(original);
	out = work_path(fx, job->convert->output);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	{
		g_autofree gchar *statement = g_strdup_printf("UPDATE conversion SET plan_caps = NULL, plan_preset = NULL,"
			" plan_threads = NULL WHERE id = %" G_GINT64_FORMAT, id);

		sql(fx, statement);
	}
	set(fx, AXIS_CONVERSION_SETTING_PRESET, "slow");
	finish_ok(fx, id);
	assert_state(fx, id, "failed");
	assert_record(fx, id, "retryable", "true");
	assert_record(fx, id, "failed_check", "");
	{
		g_autofree gchar *text = record_text(fx, id, "result_text");

		g_assert_nonnull(strstr(text, "Not judged"));
	}
	g_assert_false(exists(out));
	{
		g_autofree gchar *now = file_checksum(original);

		g_assert_cmpstr(now, ==, checksum);
	}
	g_assert_true(axis_conversions_proving(fx->conv, revision_of(fx, "space_saver"), &proving, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(proving->bad_trials, ==, 0);
	set(fx, AXIS_CONVERSION_SETTING_PRESET, "fast");
}

/* Spec (conversion-execution, A name of its own for every claim; hunt finding
 * H2): a job given back (a worker that stops) uses no attempt, and the next
 * claim gets an output name of its own, so that what an earlier claim left
 * in the work area (a thread that did not end) never blocks it. */
static void
test_name_per_claim(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *stale = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *json = NULL;
	AxisWorkerFail fail;
	AxisTestReply reply;
	AxisWorkerJob *job = NULL;
	guint number;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	id = requested_and_claimed(fx, "movie.mkv", "space_saver", AXIS_CONVERSION_TRIAL, &number, &job);
	first = g_strdup(job->convert->output);
	g_assert_cmpint(job->attempt, ==, 1);
	/* What a thread that did not end left, and the job given back. */
	stale = work_path(fx, first);
	g_assert_true(g_file_set_contents(stale, "left by a thread that did not end", -1, NULL));
	memset(&fail, 0, sizeof(fail));
	fail.job = job->id;
	fail.lease_token = job->lease_token;
	fail.error = (gchar *)"the worker is stopping";
	fail.retry = TRUE;
	fail.stopping = TRUE;
	json = axis_worker_fail_to_json(&fail);
	reply = api(fx, "fail", json);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	axis_worker_job_free(job);
	tidy(fx);
	assert_state(fx, id, "queued");

	/* The same attempt of the job, another name. */
	job = claim_work(fx);
	g_assert_cmpint(job->attempt, ==, 1);
	g_assert_cmpstr(job->convert->output, !=, first);
	encode_and_report(fx, job, NULL);
	axis_worker_job_free(job);
	finish_ok(fx, id);
	assert_state(fx, id, "done");
	assert_all_passed(fx, id);
	g_assert_true(exists(stale));
}

/* ── Migration 0049 on what an earlier build wrote ──────────────── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The migrations 1 to @last as files, for a database left at @last. */
static AxisDbMigration *
migrations_up_to(
	guint        last,
	GPtrArray  **owned
){
	g_autoptr(GDir) dir = g_dir_open(AXIS_MIGRATION_DIR, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	AxisDbMigration *list = g_new0(AxisDbMigration, last);
	const gchar *name;
	guint i;

	g_assert_nonnull(dir);
	while ((name = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, compare_names);
	*owned = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < last; i++) {
		const gchar *file = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(AXIS_MIGRATION_DIR, file, NULL);
		gchar *sql = NULL;
		gchar *migration_name = NULL;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(file, &version, &migration_name));
		g_assert_cmpuint(version, ==, i + 1);
		g_assert_true(g_file_get_contents(path, &sql, NULL, NULL));
		g_ptr_array_add(*owned, sql);
		g_ptr_array_add(*owned, migration_name);
		list[i].version = version;
		list[i].name = migration_name;
		list[i].sql = sql;
		list[i].no_transaction = axis_db_migration_has_no_transaction_marker(sql);
	}
	return list;
}

/*
 * Review of fix-conversion-hunt-findings, items 4 and 6 (migration 0049).  An
 * encode a worker took before 0049 had its output named by its attempt
 * (<id>-<attempt>.tmp, the attempt recorded at each claim): it starts
 * counting its claims at that attempt, so that its next claim, named by the
 * claim, takes a name none of its earlier claims can have left.  One never
 * taken starts at nothing.  And the database refuses to set aside anything
 * but a trial rejected by checks 3 to 8, a rejected trial whose failed check
 * is not recorded included.
 */
static void
test_migration_0049(void)
{
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GPtrArray) owned = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *claims = NULL;
	AxisDbMigration *list;
	AxisDbPool *pool;

	if (db == NULL)
		return;
	axis_test_log_init();
	pool = axis_db_pool_new(axis_test_db_get_url(db), 2, &error);
	g_assert_no_error(error);
	list = migrations_up_to(48, &owned);
	g_assert_true(axis_db_migrate_with(pool, list, 48, &error));
	g_assert_no_error(error);
	g_free(list);

	/* What the earlier build wrote: an encode taken twice (both attempts
	 * named, the second in flight), one never taken, and a trial rejected
	 * with no failed check recorded. */
	axis_test_db_exec(db, "INSERT INTO conversion (root_path, rel_path, new_rel_path, container, revision_id, outcome,"
		" state, requested_by, verdict, src_size, src_mtime_sec, src_mtime_nsec, src_ctime_sec, src_ctime_nsec,"
		" src_inode, src_nlink, attempt, output_name, failed_check) SELECT 'Films', v.rel, v.rel, 'mkv',"
		" p.current_revision_id, v.outcome, v.state, 'alice', 'safe', 1000, 1, 0, 1, 0, v.inode, 1, v.attempt, NULL, NULL"
		" FROM conversion_profile p, (VALUES ('Taken Twice (2001).mkv', 'replace', 'encoding', 11, 2),"
		" ('Never Taken (2002).mkv', 'replace', 'queued', 12, NULL), ('Rejected Unknown (2003).mkv', 'trial',"
		" 'rejected', 13, 1)) AS v (rel, outcome, state, inode, attempt) WHERE p.slug = 'space_saver'");
	axis_test_db_exec(db, "UPDATE conversion SET output_name = id || '-2.tmp' WHERE rel_path = 'Taken Twice (2001).mkv'");

	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
	claims = axis_test_db_query(db, "SELECT string_agg(rel_path || '=' || claims, ',' ORDER BY rel_path) FROM conversion");
	/* The next claim is named by claims + 1 (/conversion/name-per-claim):
	 * <id>-3.tmp for the one taken twice, not the <id>-2.tmp it may have
	 * left. */
	g_assert_cmpstr(claims, ==, "Never Taken (2002).mkv=0,Rejected Unknown (2003).mkv=1,Taken Twice (2001).mkv=2");

	/* Nothing but a trial rejected by checks 3 to 8 can be set aside. */
	g_assert_false(axis_db_pool_exec(pool, "UPDATE conversion SET set_aside_at = now(), set_aside_by = 'alice',"
		" set_aside_reason = 'a check found wrong' WHERE rel_path = 'Rejected Unknown (2003).mkv'", 0, NULL, NULL,
		&error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "conversion_set_aside_what"));
	g_clear_error(&error);
	g_assert_true(axis_db_pool_exec(pool, "UPDATE conversion SET failed_check = 4 WHERE rel_path ="
		" 'Rejected Unknown (2003).mkv'", 0, NULL, NULL, &error));
	g_assert_true(axis_db_pool_exec(pool, "UPDATE conversion SET set_aside_at = now(), set_aside_by = 'alice',"
		" set_aside_reason = 'a check found wrong' WHERE rel_path = 'Rejected Unknown (2003).mkv'", 0, NULL, NULL,
		&error));
	g_assert_no_error(error);
	axis_db_pool_free(pool);
}

#define ADD(name, func) g_test_add("/conversion/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)
#define ADD_LEASE(name, func, lease_ms) g_test_add("/conversion/" name, Fixture, GUINT_TO_POINTER(lease_ms), \
	fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = axis_test_worker_seed_caps(FALSE);
	if (have_ffmpeg) {
		clips = axis_test_storage_dir_new();
		if (!make_clips(clips) || !make_track_clips(clips))
			g_error("the clips could not be made");
	}
	g_test_add_func("/conversion/migration-0049", test_migration_0049);
	ADD("trial-real-worker", test_trial_real_worker);
	ADD("replace-real-worker", test_replace_real_worker);
	ADD("worker-stops", test_worker_stops);
	ADD("replace-new-name", test_replace_new_name);
	ADD("checks-fail", test_checks_fail);
	ADD("colour-as-planned", test_colour_as_planned);
	ADD("replace-untagged-known-sdr", test_replace_untagged_known_sdr);
	ADD("colour-changed", test_colour_changed);
	ADD("copied-tracks", test_copied_tracks);
	ADD("path-trickery", test_path_trickery);
	ADD("gates-at-request", test_gates_at_request);
	ADD("gate-kept", test_gate_kept);
	ADD("replace-kept-by-choice", test_replace_kept_by_choice);
	ADD("gates-at-claim", test_gates_at_claim);
	ADD("recover-same-name", test_recover_same_name);
	ADD("recover-new-name", test_recover_new_name);
	ADD("recover-original-changed", test_recover_original_changed);
	ADD("volume-away", test_volume_away);
	ADD("scan-lock", test_scan_lock);
	ADD("leftovers", test_leftovers);
	ADD("tidy-after-time", test_tidy_after_time);
	ADD_LEASE("worker-killed", test_worker_killed, SHORT_LEASE_MS);
	ADD_LEASE("worker-late", test_worker_late, SHORT_LEASE_MS);
	ADD("worker-cut-off", test_worker_cut_off);
	ADD("prove-by-hand", test_prove_by_hand);
	ADD("prove-blocked", test_prove_blocked);
	ADD("write-layer", test_write_layer);
	ADD("mount-without-identity", test_mount_without_identity);
	ADD("claim-reads-source", test_claim_reads_source);
	ADD("encoder-crash-pauses", test_encoder_crash_pauses);
	ADD("checks-on-worker", test_checks_on_worker);
	ADD("checks-not-of-this-file", test_checks_not_of_this_file);
	ADD("checks-refused", test_checks_refused);
	ADD("checks-only-on-worker", test_checks_only_on_worker);
	ADD("checks-fallback", test_checks_fallback);
	ADD("checks-worker-gone", test_checks_worker_gone);
	ADD("held-after-start", test_held_after_start);
	ADD("checks-prefer-other", test_checks_prefer_other);
	ADD("checks-real-worker", test_checks_real_worker);
	ADD("checks-crash-pauses", test_checks_crash_pauses);
	ADD("checks-threads", test_checks_threads);
	ADD("checks-on-worker-retried", test_checks_on_worker_retried);
	ADD("recover-on-worker", test_recover_on_worker);
	ADD("retry-measures-nothing-again", test_retry_measures_nothing_again);
	ADD("retry-held-not-same", test_retry_held_not_same);
	ADD("retry-after-lenient-mark", test_retry_after_lenient_mark);
	ADD("plan-as-handed-out", test_plan_as_handed_out);
	ADD("name-per-claim", test_name_per_claim);
	status = g_test_run();
	axis_test_worker_cleanup();
	if (clips != NULL)
		axis_test_storage_dir_free(clips);
	return status;
}
