/*
 * test-worker-sample.c - sample measurements on a worker: the plan the
 * daemon makes and describes, the results it refuses (a file the plan does
 * not have, another number of excerpts, an implausible bitrate, a file
 * changed since), and a real axis-worker measuring generated clips with
 * real software encoders, leaving the media and its scratch directory as
 * they were; the volume replaced while it reads; a link where the file was
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The clips are made here with ffmpeg's own generators (no media is read).
 * Every worker runs software encoders only.
 */

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
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-analysis.h"
#include "probe/axis-probe.h"
#include "storage/axis-volume-marker.h"
#include "transcode/axis-estimator.h"
#include "web/axis-password.h"

#define OTHER_UUID "5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a"
#define FILM_REL   "Film (2001)/Film (2001).mkv"
#define LONG_WAIT_MS 120000

static gboolean have_ffmpeg;

typedef struct
{
	AxisTestLibrary *t;
	AxisSettings    *settings;
	AxisEstimator   *estimator;
	AxisProbe       *probe;
	AxisTestDaemon  *d;
	gchar           *token;
	gchar           *disk;
	gchar           *uuid;
	gchar           *clips;
	gchar           *flag;
	gchar           *slow;
	gint64           films;
	gint64           music;
	gint64           film_id;
	gint64           track_id;
} Fixture;

#define SKIP_UNLESS_READY(fx) do { \
	if ((fx)->t == NULL) return; \
	if (!have_ffmpeg) { g_test_skip("ffmpeg is not installed"); return; } \
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

/* As test-estimate-sample.c makes them: four seconds of a generated
 * picture with a tone, and of stereo noise. */
static gboolean
make_clips(const gchar *dir)
{
	g_autofree gchar *film = g_build_filename(dir, "film.mkv", NULL);
	g_autofree gchar *track = g_build_filename(dir, "track.flac", NULL);
	const gchar *film_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=24:duration=4",
		"-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-c:v", "libx264", "-b:v", "2M", "-pix_fmt",
		"yuv420p", "-c:a", "aac", "-b:a", "96k", film, NULL
	};
	const gchar *track_argv[] = {
		"ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "anoisesrc=duration=4:sample_rate=44100",
		"-ac", "2", "-c:a", "flac", track, NULL
	};

	return run(film_argv) && run(track_argv);
}

static void
copy_into(
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

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 volume;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");
	if (!have_ffmpeg)
		return;
	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ESTIMATE_MODULE_ID, axis_estimator_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_ANALYSIS_MODULE_ID, axis_analysis_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_ESTIMATE_SETTING_SAMPLE_SECONDS, "2", &error));
	g_assert_no_error(error);
	fx->estimator = axis_estimator_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);

	fx->disk = axis_test_library_disk(fx->t, "disk1");
	volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk");
	fx->uuid = axis_test_library_q(fx->t, "SELECT uuid::text FROM volume WHERE id = %" G_GINT64_FORMAT, volume);
	fx->films = axis_test_library_add_root(fx->t, volume, fx->disk, "Films", AXIS_MEDIA_MOVIES);
	fx->music = axis_test_library_add_root(fx->t, volume, fx->disk, "Music", AXIS_MEDIA_MUSIC);

	fx->clips = g_build_filename(fx->t->dir, "clips", NULL);
	axis_test_storage_mkdir(fx->clips);
	g_assert_true(make_clips(fx->clips));
	{
		g_autofree gchar *film = g_build_filename(fx->clips, "film.mkv", NULL);
		g_autofree gchar *track = g_build_filename(fx->clips, "track.flac", NULL);
		g_autofree gchar *film_to = g_build_filename(fx->disk, "Films", FILM_REL, NULL);
		g_autofree gchar *track_to = g_build_filename(fx->disk, "Music", "Band", "Album", "01 Tone.flac", NULL);

		copy_into(film, film_to);
		copy_into(track, track_to);
	}
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->films, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->music, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	fx->film_id = axis_test_scenario_file(fx->t, fx->films, FILM_REL);
	fx->track_id = axis_test_scenario_file(fx->t, fx->music, "Band/Album/01 Tone.flac");
	probe_file(fx, fx->film_id);
	probe_file(fx, fx->track_id);

	fx->d = axis_test_daemon_new(fx->t->pool, fx->estimator, NULL, NULL);
	fx->token = axis_test_daemon_add_worker(fx->d, "garage", NULL);
	fx->flag = g_build_filename(fx->t->dir, "stand-still", NULL);
	fx->slow = axis_test_slow_ffmpeg(fx->t->dir, fx->flag);
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
	axis_probe_unref(fx->probe);
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	g_free(fx->disk);
	g_free(fx->uuid);
	g_free(fx->clips);
	g_free(fx->flag);
	g_free(fx->slow);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
api(
	Fixture     *fx,
	const gchar *request,
	const gchar *body
){
	return axis_test_daemon_post(fx->d, "POST", request, fx->token, NULL, body, NULL);
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
	g_assert_nonnull(strstr(reply.body, "\"accepted\":true"));
	g_assert_null(strstr(reply.body, fx->t->dir));
	axis_test_reply_clear(&reply);
}

static AxisWorkerJob *
do_claim(Fixture *fx)
{
	const gchar *volumes[2];
	g_autofree gchar *json = NULL;
	g_autofree gchar *problem = NULL;
	AxisWorkerJob *job = NULL;
	AxisTestReply reply;

	volumes[0] = fx->uuid;
	volumes[1] = NULL;
	json = axis_test_claim_json(volumes);
	reply = api(fx, "claim", json);
	g_assert_cmpuint(reply.status, ==, 200);
	/* A volume's UUID and paths relative to it, never an absolute path:
	 * nothing of where the daemon sees the disk is said. */
	g_assert_null(strstr(reply.body, fx->t->dir));
	g_assert_null(strstr(reply.body, "\"root\":\"/"));
	g_assert_null(strstr(reply.body, "\"path\":\"/"));
	if (!axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, &problem))
		g_error("%s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	return job;
}

static gint64
request_sample(
	Fixture     *fx,
	const gchar *target
){
	g_autoptr(GError) error = NULL;
	gboolean created = FALSE;
	gint64 id = 0;

	g_assert_true(axis_estimator_request_remote_sample(fx->estimator, target, "root", &id, &created, &error));
	g_assert_no_error(error);
	g_assert_true(created);
	return id;
}

/* A complete request for the sample job @job with these results. */
static gchar *
complete_json(
	const AxisWorkerJob *job,
	AxisSampleResult    *result
){
	AxisWorkerComplete complete;

	memset(&complete, 0, sizeof(complete));
	complete.job = job->id;
	complete.lease_token = job->lease_token;
	complete.kind = (gchar *)AXIS_WORKER_KIND_SAMPLE;
	complete.sample = result;
	return axis_worker_complete_to_json(&complete);
}

static gint64
n(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_library_n(fx->t, "%s", sql);
}

static gboolean
wait_job_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state,
	guint        timeout_ms
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_wait_query(fx->t->db, sql, state, timeout_ms);
}

static AxisTestWorker *
start_worker(
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
	if (!axis_test_worker_wait_log(w, "registered as garage: lease 2000 ms, heartbeat 500 ms, 1 volume(s) accepted",
	                               AXIS_TEST_WAIT_MS))
		g_error("the worker did not register; its log:\n%s", axis_test_worker_get_log(w));
	return w;
}

static void
finish(
	Fixture        *fx,
	AxisTestWorker *w
){
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, 0);
	g_assert_null(strstr(axis_test_worker_get_log(w), fx->token));
	axis_test_worker_free(w);
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

static gboolean
scratch_empty(gpointer data)
{
	return axis_test_worker_scratch_empty(data);
}

/* ── The plan, and what is refused ──────────────────────────────── */

static void
test_plan_and_refusals(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisSampleResult) bad_index = axis_sample_result_new();
	g_autoptr(AxisSampleResult) bad_count = axis_sample_result_new();
	g_autoptr(AxisSampleResult) tiny = axis_sample_result_new();
	g_autofree gchar *film = NULL;
	const AxisSampleFilePlan *f;
	AxisWorkerCompleted completed;
	AxisWorkerJob *job;
	AxisTestReply reply;
	struct stat st;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	do_register(fx);
	id = request_sample(fx, "space_saver");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND queue = 'remote' AND payload->'requires'->>'encoder' = 'libx265'"
		" AND payload->'requires'->'volumes' = '[\"%s\"]'::jsonb", id, fx->uuid), ==, 1);
	/* One measurement at a time, the daemon's own or a worker's: asking for
	 * the daemon's own now finds this one. */
	{
		g_autoptr(GError) error = NULL;
		gboolean created = TRUE;
		gint64 other = 0;

		g_assert_true(axis_estimator_request_sample(fx->estimator, "balanced", "root", &other, &created, &error));
		g_assert_no_error(error);
		g_assert_false(created);
		g_assert_cmpint(other, ==, id);
	}

	/* Described as planned: the film, by its volume and paths. */
	job = do_claim(fx);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, id);
	g_assert_cmpstr(job->kind, ==, AXIS_WORKER_KIND_SAMPLE);
	g_assert_nonnull(job->sample);
	g_assert_cmpint(job->sample->media, ==, AXIS_TARGET_VIDEO);
	g_assert_cmpint(job->sample->codec, ==, AXIS_TCODEC_HEVC);
	g_assert_cmpint(job->sample->family, ==, AXIS_ENCODER_SOFTWARE);
	g_assert_cmpuint(job->sample->files->len, ==, 1);
	f = g_ptr_array_index(job->sample->files, 0);
	g_assert_cmpstr(f->volume, ==, fx->uuid);
	g_assert_cmpstr(f->root, ==, "Films");
	g_assert_cmpstr(f->path, ==, FILM_REL);
	film = g_build_filename(fx->disk, "Films", FILM_REL, NULL);
	g_assert_cmpint(g_stat(film, &st), ==, 0);
	g_assert_cmpint(f->size, ==, (gint64)st.st_size);
	g_assert_cmpuint(f->n_excerpts, ==, 3);
	/* The depth the profile encodes at; a worker is offered a 10-bit plan
	 * only if it showed 10 bits. */
	g_assert_true(f->bit_depth == 8 || f->bit_depth == 10);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND payload->'requires'->>'ten_bit' = '%s'", id, f->bit_depth == 10 ? "true" : "false"), ==, 1);

	/* A file the plan does not have, another number of excerpts: refused
	 * as malformed, the job still held. */
	{
		const guint64 bytes[] = { 30000, 30000, 30000 };
		g_autofree gchar *json = NULL;

		axis_sample_result_add(bad_index, 5, bytes, 3, NULL);
		json = complete_json(job, bad_index);
		reply = api(fx, "complete", json);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
		axis_sample_result_add(bad_count, 0, bytes, 2, NULL);
		g_free(json);
		json = complete_json(job, bad_count);
		reply = api(fx, "complete", json);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND state = 'running'", id), ==, 1);

	/* A bitrate no encoder would make: the job ends, nothing is kept. */
	{
		const guint64 bytes[] = { 1, 1, 1 };
		g_autofree gchar *json = NULL;

		axis_sample_result_add(tiny, 0, bytes, 3, NULL);
		json = complete_json(job, tiny);
		reply = api(fx, "complete", json);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_true(axis_worker_completed_parse(reply.body, reply.length, &completed, NULL));
		g_assert_cmpuint(completed.recorded, ==, 0);
		g_assert_cmpuint(completed.refused, ==, 1);
		axis_test_reply_clear(&reply);
	}
	g_assert_true(wait_job_state(fx, id, "succeeded", AXIS_TEST_WAIT_MS));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker garage: the result for file*is not recorded:*"),
		==, 1);
	axis_worker_job_free(job);

	/* A file changed after the plan was made: what was measured of it is
	 * not kept, however plausible. */
	id = request_sample(fx, "space_saver");
	job = do_claim(fx);
	g_assert_nonnull(job);
	axis_test_set_mtime(film, 1600000000, 0);
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->films, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	{
		g_autoptr(AxisSampleResult) result = axis_sample_result_new();
		const guint64 bytes[] = { 30000, 30000, 30000 };
		g_autofree gchar *json = NULL;

		axis_sample_result_add(result, 0, bytes, 3, NULL);
		json = complete_json(job, result);
		reply = api(fx, "complete", json);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_true(axis_worker_completed_parse(reply.body, reply.length, &completed, NULL));
		g_assert_cmpuint(completed.recorded, ==, 0);
		g_assert_cmpuint(completed.refused, ==, 1);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample"), ==, 0);
	axis_worker_job_free(job);
}

/* ── A real worker ──────────────────────────────────────────────── */

static void
test_real_worker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *reported = NULL;
	AxisTestWorker *w;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	before = axis_test_storage_digest(fx->disk);
	w = start_worker(fx, NULL);

	/* A picture: libx265, software. */
	id = request_sample(fx, "space_saver");
	if (!wait_job_state(fx, id, "succeeded", LONG_WAIT_MS))
		g_error("the measurement did not succeed; the worker's log:\n%s", axis_test_worker_get_log(w));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = %" G_GINT64_FORMAT
		" AND origin = 'measured' AND codec = 'hevc' AND family = 'software' AND part = 'video'"
		" AND output_bitrate > 0 AND file_id = %" G_GINT64_FORMAT, id, fx->film_id), ==, 1);
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);
	g_assert_true(axis_test_worker_wait_log(w, reported, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker garage completed job*1 recorded, 0 refused*"),
		==, 1);

	/* Sound: Opus. */
	id = request_sample(fx, "music_space_saver");
	if (!wait_job_state(fx, id, "succeeded", LONG_WAIT_MS))
		g_error("the measurement did not succeed; the worker's log:\n%s", axis_test_worker_get_log(w));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = %" G_GINT64_FORMAT
		" AND codec = 'opus' AND part = 'audio' AND output_bitrate BETWEEN 40000 AND 400000 AND file_id = %"
		G_GINT64_FORMAT, id, fx->track_id), ==, 1);

	/* The media as it was, nothing left in scratch. */
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	finish(fx, w);
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(before, ==, after);
}

/* ── The volume replaced while it reads ─────────────────────────── */

static void
test_volume_replaced(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *moved = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *marker = NULL;
	g_autofree gchar *started = NULL;
	g_autofree gchar *last_error = NULL;
	AxisTestWorker *w;
	ChildWait wait;
	AxisDbConn *conn;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	before = axis_test_storage_digest(fx->disk);
	axis_test_storage_write(fx->flag, "");
	w = start_worker(fx, fx->slow);
	id = request_sample(fx, "space_saver");
	started = g_strdup_printf("job %" G_GINT64_FORMAT " (estimate.sample.remote), attempt 1: started", id);
	g_assert_true(axis_test_worker_wait_log(w, started, AXIS_TEST_WAIT_MS));
	wait.parent = axis_test_worker_get_pid(w);
	wait.found = 0;
	g_assert_true(axis_test_pump_until(child_there, &wait, AXIS_TEST_WAIT_MS));

	/* Another disk where the volume was. */
	moved = g_strconcat(fx->disk, ".moved", NULL);
	g_assert_cmpint(g_rename(fx->disk, moved), ==, 0);
	axis_test_storage_mkdir(fx->disk);
	axis_test_storage_write_marker(fx->disk, OTHER_UUID);

	/* The job stops at once: its encoder killed, a failure to retry. */
	g_assert_true(axis_test_pump_until(process_gone, &wait.found, AXIS_TEST_WAIT_MS));
	g_assert_true(wait_job_state(fx, id, "queued", AXIS_TEST_WAIT_MS));
	last_error = axis_test_library_q(fx->t, "SELECT last_error FROM job WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(g_str_has_prefix(last_error, "worker garage: volume unavailable"));
	g_assert_true(axis_test_worker_wait_log(w, "is not the one proved any more", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	/* The other disk is not taken for the volume. */
	g_assert_true(axis_test_worker_wait_log(w, "volume " OTHER_UUID, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM worker_volume"), ==, 0);

	/* Nothing more of it runs. */
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_true(axis_job_cancel(conn, id, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	g_assert_cmpint(g_unlink(fx->flag), ==, 0);
	finish(fx, w);

	marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
	g_assert_cmpint(g_rename(moved, fx->disk), ==, 0);
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample"), ==, 0);
}

/* ── A link where the file was ──────────────────────────────────── */

static void
test_link_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *film = g_build_filename(fx->disk != NULL ? fx->disk : "/", "Films", FILM_REL, NULL);
	g_autofree gchar *aside = NULL;
	g_autofree gchar *reported = NULL;
	AxisTestWorker *w;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	/* Any encode would stand still, and the test would see it. */
	axis_test_storage_write(fx->flag, "");
	w = start_worker(fx, fx->slow);
	id = request_sample(fx, "space_saver");
	/* After the plan: the film is a link to a copy off the volume. */
	aside = g_build_filename(fx->t->dir, "film-aside.mkv", NULL);
	g_assert_cmpint(g_rename(film, aside), ==, 0);
	g_assert_cmpint(symlink(aside, film), ==, 0);

	if (!wait_job_state(fx, id, "succeeded", AXIS_TEST_WAIT_MS))
		g_error("the job did not end; the worker's log:\n%s", axis_test_worker_get_log(w));
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": file 0 (" FILM_REL "):", id);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), reported));
	g_assert_cmpint(axis_test_find_child(axis_test_worker_get_pid(w), "sleep"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker garage completed job*0 recorded, 0 refused*"),
		==, 1);
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample"), ==, 0);
	g_assert_cmpint(g_unlink(fx->flag), ==, 0);
	finish(fx, w);

	g_assert_cmpint(g_unlink(film), ==, 0);
	g_assert_cmpint(g_rename(aside, film), ==, 0);
}

/* ── Through a mount without file identities; a crash ───────────── */

static AxisTestWorker *
start_worker_on(
	Fixture     *fx,
	const gchar *volume,
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
	options.token = fx->token;
	options.volumes = volumes;
	options.ffmpeg = ffmpeg;
	options.extra_config = extra;
	w = axis_test_worker_start(&options);
	if (!axis_test_worker_wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS))
		g_error("the worker did not register; its log:\n%s", axis_test_worker_get_log(w));
	return w;
}

/* Spec: the sample measurement of a file seen through a mount that does not
 * keep file identities compares its size and its time to the second there;
 * by default, to the nanosecond, as before. */
static void
test_mount_without_identity(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *film = NULL;
	g_autofree gchar *view = NULL;
	g_autofree gchar *copy = NULL;
	g_autofree gchar *marker = NULL;
	g_autofree gchar *extra = NULL;
	g_autofree gchar *film_sql = NULL;
	AxisTestWorker *w;
	struct stat st;
	gint64 id;

	SKIP_UNLESS_READY(fx);
	film = g_build_filename(fx->disk, "Films", FILM_REL, NULL);
	g_assert_cmpint(stat(film, &st), ==, 0);
	if (st.st_mtim.tv_nsec == 0) {
		g_test_skip("this filesystem keeps no time below a second");
		return;
	}
	/* The disk as sshfs shows it: the marker, a copy of the film (another
	 * inode), its time to the second only. */
	view = g_build_filename(fx->t->dir, "view", NULL);
	copy = g_build_filename(view, "Films", FILM_REL, NULL);
	marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);
	{
		g_autofree gchar *to = g_build_filename(view, AXIS_VOLUME_MARKER_NAME, NULL);

		copy_into(marker, to);
	}
	copy_into(film, copy);
	axis_test_set_mtime(copy, st.st_mtim.tv_sec, 0);
	film_sql = g_strdup_printf("SELECT count(*) FROM estimate_sample WHERE file_id = %" G_GINT64_FORMAT, fx->film_id);

	/* As strict as before: not measured, "changed". */
	w = start_worker_on(fx, view, NULL, NULL);
	id = request_sample(fx, "space_saver");
	if (!wait_job_state(fx, id, "succeeded", LONG_WAIT_MS))
		g_error("the measurement did not end; the worker's log:\n%s", axis_test_worker_get_log(w));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "not measured: its modification time differs below a second"));
	g_assert_cmpint(n(fx, film_sql), ==, 0);
	finish(fx, w);

	/* Told so: measured. */
	extra = g_strdup_printf("volumes_without_file_identity = %s\n", view);
	w = start_worker_on(fx, view, NULL, extra);
	id = request_sample(fx, "space_saver");
	if (!wait_job_state(fx, id, "succeeded", LONG_WAIT_MS))
		g_error("the measurement did not succeed; the worker's log:\n%s", axis_test_worker_get_log(w));
	g_assert_cmpint(n(fx, film_sql), ==, 1);
	finish(fx, w);
}

/* Spec: a crashed encoder ends the measurement (for another worker to make
 * again) and pauses the worker. */
static void
test_crash(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *flag = NULL;
	g_autofree gchar *ffmpeg = NULL;
	g_autofree gchar *last_error = NULL;
	AxisTestWorker *w;
	gint64 id;

	/* Nothing of the fixture before this: without ffmpeg or a database it
	 * was never set up. */
	SKIP_UNLESS_READY(fx);
	flag = g_build_filename(fx->t->dir, "fail-how", NULL);
	ffmpeg = axis_test_failing_ffmpeg(fx->t->dir, flag);
	axis_test_storage_write(flag, "SEGV");
	w = start_worker(fx, ffmpeg);
	id = request_sample(fx, "space_saver");
	if (!axis_test_wait_query(fx->t->db, "SELECT count(*) FROM worker WHERE name = 'garage' AND paused_at IS NOT NULL",
	        "1", LONG_WAIT_MS))
		g_error("the worker was not paused; its log:\n%s", axis_test_worker_get_log(w));
	/* Given back to be measured again (one attempt used), nothing recorded. */
	g_assert_true(wait_job_state(fx, id, "queued", AXIS_TEST_WAIT_MS));
	last_error = axis_test_library_q(fx->t, "SELECT last_error FROM job WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(last_error, "the encoder crashed (signal 11, SIGSEGV)"));
	g_assert_cmpint(n(fx, "SELECT count(*) FROM estimate_sample"), ==, 0);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "this worker takes no more work"));
	/* Paused: it does not take the measurement again. */
	g_assert_cmpint(g_unlink(flag), ==, 0);
	axis_test_pump_for(1500);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT attempts FROM job WHERE id = %" G_GINT64_FORMAT, id), ==, 1);
	finish(fx, w);
}

#define ADD(name, func) g_test_add("/worker/sample/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = axis_test_worker_seed_caps(FALSE);
	ADD("plan-and-refusals", test_plan_and_refusals);
	ADD("real-worker", test_real_worker);
	ADD("volume-replaced", test_volume_replaced);
	ADD("link-refused", test_link_refused);
	ADD("mount-without-identity", test_mount_without_identity);
	ADD("crash", test_crash);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
