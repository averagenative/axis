/*
 * test-estimate-sample.c - the sample measurement: real ffmpeg on generated
 * clips records samples, leaves nothing in scratch and writes nothing under
 * a media root; a fake tool for the hardware encoders this machine cannot
 * run; cancellation; the scratch directory's safety; the request and its
 * arguments
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-analysis.h"
#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "axis-test-case.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-private.h"
#include "library/axis-analysis.h"
#include "probe/axis-probe.h"
#include "probe/axis-probe-run.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-estimator-private.h"

typedef struct
{
	AxisTestLibrary *t;
	AxisSettings    *settings;
	AxisEstimator   *estimator;
	AxisProbe       *probe;
	gchar           *disk;
	gchar           *tmp;	/* the scratch directories' parent's parent */
	gchar           *scratch;	/* tmp/axis-scratch-<uid> */
	gchar           *clips;
	gint64           volume;
	gint64           films;
	gint64           music;
	gint64           film_id;
	gint64           track_id;
	gboolean         media;
	guint            hooks;
	gboolean         scratch_was_empty;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)
#define SKIP_WITHOUT_MEDIA(fx) do { if (!(fx)->media) { g_test_skip("ffmpeg is not installed"); return; } } while (0)

static gboolean
run(const gchar *const *argv)
{
	g_autoptr(GError) error = NULL;
	gint status = 0;

	if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
	                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, &error))
		return FALSE;
	return g_spawn_check_wait_status(status, NULL);
}

/* Four seconds of a 320x180 picture at a high bitrate, and of stereo noise
 * (a pure tone compresses so well that Opus would not shrink it). */
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
	g_autofree gchar *name = g_strdup_printf("axis-scratch-%u", (guint)getuid());
	g_autofree gchar *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree gchar *ffprobe = g_find_program_in_path("ffprobe");
	AxisSampleTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
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

	fx->tmp = g_build_filename(fx->t->dir, "tmp", NULL);
	fx->scratch = g_build_filename(fx->tmp, name, NULL);
	axis_test_storage_mkdir(fx->tmp);
	memset(&tuning, 0, sizeof(tuning));
	tuning.scratch_parent = fx->tmp;
	axis_estimator_set_sample_tuning(fx->estimator, &tuning);

	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk");
	fx->films = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Films", AXIS_MEDIA_MOVIES);
	fx->music = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Music", AXIS_MEDIA_MUSIC);

	fx->clips = g_build_filename(fx->t->dir, "clips", NULL);
	axis_test_storage_mkdir(fx->clips);
	fx->media = ffmpeg != NULL && ffprobe != NULL && make_clips(fx->clips);
	if (!fx->media)
		return;
	{
		g_autofree gchar *film = g_build_filename(fx->clips, "film.mkv", NULL);
		g_autofree gchar *track = g_build_filename(fx->clips, "track.flac", NULL);
		g_autofree gchar *film_to = g_build_filename(fx->disk, "Films", "Film (2001)", "Film (2001).mkv", NULL);
		g_autofree gchar *track_to = g_build_filename(fx->disk, "Music", "Band", "Album", "01 Tone.flac", NULL);

		copy_into(film, film_to);
		copy_into(track, track_to);
	}
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->films, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	axis_scan_result_free(axis_test_library_scan_expect(fx->t, fx->music, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE));
	fx->film_id = axis_test_scenario_file(fx->t, fx->films, "Film (2001)/Film (2001).mkv");
	fx->track_id = axis_test_scenario_file(fx->t, fx->music, "Band/Album/01 Tone.flac");
	probe_file(fx, fx->film_id);
	probe_file(fx, fx->track_id);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_probe_unref(fx->probe);
	axis_estimator_unref(fx->estimator);
	axis_settings_unref(fx->settings);
	g_free(fx->disk);
	g_free(fx->tmp);
	g_free(fx->scratch);
	g_free(fx->clips);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

/* A job context as the queue would make one for a measurement. */
static AxisJobContext *
context_for(
	Fixture     *fx,
	gint64       id,
	const gchar *target
){
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobClaim claim;

	json_object_set_string_member(payload, "target", target);
	memset(&claim, 0, sizeof(claim));
	claim.id = id;
	claim.kind = (gchar *)AXIS_ESTIMATE_SAMPLE_KIND;
	claim.queue = (gchar *)AXIS_ESTIMATE_SAMPLE_QUEUE;
	claim.payload = (gchar *)"{}";
	claim.attempt = 1;
	claim.max_attempts = 1;
	claim.lease_token = (gchar *)"00000000-0000-0000-0000-000000000000";
	return axis_job_context_new(&claim, payload, fx->t->pool, NULL, NULL, NULL);
}

static guint
entries(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	guint n = 0;

	if (d == NULL)
		return 0;
	while (g_dir_read_name(d) != NULL)
		n++;
	return n;
}

/* After each excerpt: the run's scratch directory exists and is empty (the
 * encoder writes to its pipe). */
static void
excerpt_hook(
	const gchar *scratch,
	gpointer     data
){
	Fixture *fx = data;

	fx->hooks++;
	if (!g_file_test(scratch, G_FILE_TEST_IS_DIR) || entries(scratch) != 0)
		fx->scratch_was_empty = FALSE;
}

static void
tune(
	Fixture            *fx,
	const gchar *const *tool,
	guint               timeout_ms
){
	AxisSampleTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.tool_command = tool;
	tuning.scratch_parent = fx->tmp;
	tuning.timeout_ms = timeout_ms;
	tuning.excerpt_hook = excerpt_hook;
	tuning.hook_data = fx;
	axis_estimator_set_sample_tuning(fx->estimator, &tuning);
}

/* ── Real ffmpeg ────────────────────────────────────────────────── */

static void
test_measure_real(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisJobContext *context;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_MEDIA(fx);
	/* It only reads: storage is read-only here (the test library's storage
	 * has no settings store, so it always is) and it runs. */
	g_assert_true(axis_storage_is_read_only_mode(fx->t->storage));
	tune(fx, NULL, 0);
	fx->scratch_was_empty = TRUE;
	before = axis_test_storage_digest(fx->disk);

	context = context_for(fx, 41, "space_saver");
	g_assert_true(axis_estimator_run_sample_job(fx->estimator, context, &error));
	g_assert_no_error(error);
	axis_job_context_unref(context);
	g_assert_cmpuint(fx->hooks, ==, 3);
	g_assert_true(fx->scratch_was_empty);

	/* One sample for the film, from its three excerpts. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = 41"), ==, 1);
	/* Samples name the profile revision measured (the target id before). */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = 41"
		" AND revision_id = (SELECT current_revision_id FROM conversion_profile WHERE slug = 'space_saver')"
		" AND codec = 'hevc' AND family = 'software' AND part = 'video'"
		" AND origin = 'measured' AND output_bitrate > 0 AND width = 320 AND file_id = %" G_GINT64_FORMAT,
		fx->film_id), ==, 1);

	context = context_for(fx, 42, "music_space_saver");
	g_assert_true(axis_estimator_run_sample_job(fx->estimator, context, &error));
	g_assert_no_error(error);
	axis_job_context_unref(context);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = 42"
		" AND codec = 'opus' AND part = 'audio' AND file_id = %" G_GINT64_FORMAT, fx->track_id), ==, 1);
	/* Opus at 128 kbit/s comes out near it (the excerpts are short). */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT output_bitrate FROM estimate_sample WHERE job_id = 42"), >,
		40000);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT output_bitrate FROM estimate_sample WHERE job_id = 42"), <,
		400000);

	/* Nothing left behind, nothing changed on the media volume. */
	g_assert_cmpuint(entries(fx->scratch), ==, 0);
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpuint(axis_probe_run_get_unreaped(), ==, 0);
}

/* ── A fake tool ────────────────────────────────────────────────── */

static gchar *
write_tool(
	Fixture     *fx,
	const gchar *name,
	const gchar *body
){
	g_autofree gchar *path = g_build_filename(fx->t->dir, name, NULL);

	g_assert_true(g_file_set_contents(path, body, -1, NULL));
	return g_steal_pointer(&path);
}

/* The hardware encoders' arguments, with a tool that checks them: it
 * insists on the encoder, the descriptor and the pipe, reads the input
 * through descriptor 3 and writes some bytes. */
static void
hardware(
	Fixture     *fx,
	const gchar *family,
	const gchar *encoder,
	const gchar *option
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *script = g_strdup_printf(
		"want() { for a in \"$@\"; do [ \"$a\" = \"$W\" ] && return 0; done; exit 9; }\n"
		"W=%s; want \"$@\"\nW=%s; want \"$@\"\nW=fd:; want \"$@\"\nW=pipe:1; want \"$@\"\nW=3; want \"$@\"\n"
		"head -c 64 <&3 > /dev/null || exit 8\n"
		"head -c 20000 /dev/zero\n", encoder, option);
	g_autofree gchar *tool = write_tool(fx, "fake-ffmpeg.sh", script);
	const gchar *command[] = { "/bin/sh", tool, NULL };
	AxisJobContext *context;

	g_assert_true(axis_settings_set(fx->settings, AXIS_ESTIMATE_SETTING_ENCODER, family, &error));
	tune(fx, command, 10000);
	context = context_for(fx, 50, "space_saver");
	g_assert_true(axis_estimator_run_sample_job(fx->estimator, context, &error));
	g_assert_no_error(error);
	axis_job_context_unref(context);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample WHERE job_id = 50"
		" AND family = '%s'", family), ==, 1);
	/* 20000 bytes for each of three excerpts of a quarter of the file
	 * (about a second: the container says 4.02 s). */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT output_bitrate FROM estimate_sample WHERE job_id = 50"), >,
		155000);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT output_bitrate FROM estimate_sample WHERE job_id = 50"), <=,
		160000);
	g_assert_cmpuint(entries(fx->scratch), ==, 0);
	axis_test_db_exec(fx->t->db, "DELETE FROM estimate_sample");
}

static void
test_measure_nvenc_fake(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_MEDIA(fx);
	hardware(fx, "nvenc", "hevc_nvenc", "-cq");
	hardware(fx, "vaapi", "hevc_vaapi", "-vaapi_device");
}

/* A tool that fails, and one that tries to write a file (in its working
 * directory, the scratch one, and on the media volume): neither can (the
 * child may not write files), the file is left out, nothing is recorded,
 * nothing is left behind. */
static void
test_measure_tool_fails(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *tool = write_tool(fx, "broken-ffmpeg.sh", "echo 'no such encoder' >&2\nexit 1\n");
	g_autofree gchar *film_dir = g_build_filename(fx->disk, "Films", "Film (2001)", NULL);
	g_autofree gchar *script = g_strdup_printf("echo out > '%s/out.hevc'\necho out > out.hevc\n"
		"head -c 1000 /dev/zero\n", film_dir);
	g_autofree gchar *writer = write_tool(fx, "writing-ffmpeg.sh", script);
	const gchar *command[] = { "/bin/sh", tool, NULL };
	AxisJobContext *context;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_MEDIA(fx);
	tune(fx, command, 10000);
	context = context_for(fx, 60, "space_saver");
	g_assert_true(axis_estimator_run_sample_job(fx->estimator, context, &error));
	axis_job_context_unref(context);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample"), ==, 0);
	g_assert_cmpuint(entries(fx->scratch), ==, 0);

	/* The media volume is writable to the daemon's user here: only the
	 * child's file-size limit stops the write.  What it guarantees is that
	 * no data reaches a file; a misbehaving encoder can still create an
	 * empty one (or remove one: it is not sandboxed, see the report). */
	{
		g_autofree gchar *leak = g_build_filename(film_dir, "out.hevc", NULL);
		g_autofree gchar *film = g_build_filename(film_dir, "Film (2001).mkv", NULL);
		g_autofree gchar *content_before = NULL;
		g_autofree gchar *content_after = NULL;
		gsize length_before = 0;
		gsize length_after = 0;

		g_assert_true(g_file_get_contents(film, &content_before, &length_before, NULL));
		command[1] = writer;
		tune(fx, command, 10000);
		context = context_for(fx, 61, "space_saver");
		g_assert_true(axis_estimator_run_sample_job(fx->estimator, context, &error));
		axis_job_context_unref(context);
		g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample"), ==, 0);
		g_assert_cmpuint(entries(fx->scratch), ==, 0);
		if (g_file_test(leak, G_FILE_TEST_EXISTS)) {
			struct stat st;

			g_assert_cmpint(stat(leak, &st), ==, 0);
			g_assert_cmpint(st.st_size, ==, 0);
			g_assert_cmpint(unlink(leak), ==, 0);
		}
		g_assert_true(g_file_get_contents(film, &content_after, &length_after, NULL));
		g_assert_cmpmem(content_before, length_before, content_after, length_after);
	}
}

/* ── Cancel ─────────────────────────────────────────────────────── */

typedef struct
{
	AxisJobContext *context;
	guint           delay_ms;
} Canceller;

static gpointer
cancel_later(gpointer data)
{
	Canceller *c = data;

	g_usleep(c->delay_ms * 1000);
	axis_job_context_stop(c->context, AXIS_JOB_STOP_CANCELLED);
	return NULL;
}

/* Whether a live process runs "sleep @arg" (looked at once; the kernel's
 * dead states count as gone). */
static gboolean
sleeper_alive(const gchar *arg)
{
	g_autoptr(GDir) proc = g_dir_open("/proc", 0, NULL);
	const gchar *name;

	while (proc != NULL && (name = g_dir_read_name(proc)) != NULL) {
		g_autofree gchar *cmdline_path = NULL;
		g_autofree gchar *stat_path = NULL;
		g_autofree gchar *cmdline = NULL;
		g_autofree gchar *stat = NULL;
		gsize length = 0;
		const gchar *close_paren;

		if (!g_ascii_isdigit(name[0]))
			continue;
		cmdline_path = g_build_filename("/proc", name, "cmdline", NULL);
		if (!g_file_get_contents(cmdline_path, &cmdline, &length, NULL) || length < 6 ||
		    strcmp(cmdline, "sleep") != 0 || strlen(cmdline) + 1 >= length ||
		    strcmp(cmdline + strlen(cmdline) + 1, arg) != 0)
			continue;
		stat_path = g_build_filename("/proc", name, "stat", NULL);
		if (!g_file_get_contents(stat_path, &stat, NULL, NULL))
			continue;
		close_paren = strrchr(stat, ')');
		if (close_paren != NULL && close_paren[1] == ' ' && close_paren[2] != 'Z' && close_paren[2] != 'X')
			return TRUE;
	}
	return FALSE;
}

static void
test_measure_cancel(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *arg = g_strdup_printf("30.%u", (guint)getpid());
	g_autofree gchar *script = NULL;
	g_autofree gchar *tool = NULL;
	const gchar *command[] = { "/bin/sh", NULL, NULL };
	Canceller canceller;
	GThread *thread;
	gint64 started;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_MEDIA(fx);
	script = g_strdup_printf("exec sleep %s\n", arg);
	tool = write_tool(fx, "slow-ffmpeg.sh", script);
	command[1] = tool;
	tune(fx, command, 60000);

	canceller.context = context_for(fx, 70, "space_saver");
	canceller.delay_ms = 500;
	thread = g_thread_new("cancel", cancel_later, &canceller);
	started = g_get_monotonic_time();
	g_assert_false(axis_estimator_run_sample_job(fx->estimator, canceller.context, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED);
	g_thread_join(thread);
	axis_job_context_unref(canceller.context);

	/* The encoder stopped at once, its scratch is gone, nothing recorded,
	 * no process left. */
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
	g_assert_cmpuint(entries(fx->scratch), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_sample"), ==, 0);
	g_assert_false(sleeper_alive(arg));
	g_assert_cmpuint(axis_probe_run_get_unreaped(), ==, 0);
}

/* ── Scratch safety ─────────────────────────────────────────────── */

static void
test_scratch_cleanup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *stale = NULL;
	g_autofree gchar *inner = NULL;
	g_autofree gchar *kept = NULL;
	g_autofree gchar *media_file = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	SKIP_WITHOUT_DB(fx);
	parent = axis_estimator_scratch_parent(fx->estimator, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(parent, ==, fx->scratch);

	/* What a killed daemon left: a run's directory with a file, a folder
	 * and a link into the media volume; and something that is not a run's. */
	media_file = g_build_filename(fx->disk, "Films", "keep-me.mkv", NULL);
	axis_test_storage_write(media_file, "precious");
	before = axis_test_storage_digest(fx->disk);
	stale = g_build_filename(parent, "sample-7-abcdef", NULL);
	inner = g_build_filename(stale, "deeper", NULL);
	axis_test_storage_mkdir(inner);
	{
		g_autofree gchar *junk = g_build_filename(inner, "x.hevc", NULL);
		g_autofree gchar *link = g_build_filename(stale, "media", NULL);
		g_autofree gchar *file_link = g_build_filename(stale, "film", NULL);
		g_autofree gchar *films = g_build_filename(fx->disk, "Films", NULL);

		axis_test_storage_write(junk, "partial");
		g_assert_cmpint(symlink(films, link), ==, 0);
		g_assert_cmpint(symlink(media_file, file_link), ==, 0);
	}
	kept = g_build_filename(parent, "not-a-run", NULL);
	axis_test_storage_write(kept, "left alone");

	axis_estimator_clean_scratch(fx->estimator);
	g_assert_false(g_file_test(stale, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(kept, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(media_file, G_FILE_TEST_IS_REGULAR));
	after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(before, ==, after);
}

static void
test_scratch_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *elsewhere = g_build_filename(fx->t->dir, "elsewhere", NULL);
	g_autofree gchar *parent = NULL;
	AxisSampleTuning tuning;
	AxisJobContext *context;

	SKIP_WITHOUT_DB(fx);
	/* A link where the directory should be. */
	axis_test_storage_mkdir(elsewhere);
	g_assert_cmpint(symlink(elsewhere, fx->scratch), ==, 0);
	parent = axis_estimator_scratch_parent(fx->estimator, &error);
	g_assert_null(parent);
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH);
	g_clear_error(&error);
	/* The job refuses to run (and writes nothing anywhere). */
	if (fx->media) {
		context = context_for(fx, 80, "space_saver");
		g_assert_false(axis_estimator_run_sample_job(fx->estimator, context, &error));
		g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH);
		g_clear_error(&error);
		axis_job_context_unref(context);
	}
	g_assert_cmpuint(entries(elsewhere), ==, 0);
	g_assert_cmpint(unlink(fx->scratch), ==, 0);

	/* Open to others. */
	axis_test_storage_mkdir(fx->scratch);
	g_assert_cmpint(chmod(fx->scratch, 0777), ==, 0);
	parent = axis_estimator_scratch_parent(fx->estimator, &error);
	g_assert_null(parent);
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH);
	g_clear_error(&error);
	g_assert_cmpint(rmdir(fx->scratch), ==, 0);

	/* Under a volume. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.scratch_parent = fx->disk;
	axis_estimator_set_sample_tuning(fx->estimator, &tuning);
	{
		g_autofree gchar *before = axis_test_storage_digest(fx->disk);
		g_autofree gchar *after = NULL;

		parent = axis_estimator_scratch_parent(fx->estimator, &error);
		g_assert_null(parent);
		g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_SCRATCH);
		g_clear_error(&error);
		/* Not even its directory was made there. */
		after = axis_test_storage_digest(fx->disk);
		g_assert_cmpstr(before, ==, after);
	}
}

/* ── Requests ───────────────────────────────────────────────────── */

static void
test_request_and_cancel(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisSampleStatus status;
	gint64 first = 0;
	gint64 second = 0;
	gboolean created = FALSE;
	gboolean cancelled = FALSE;

	SKIP_WITHOUT_DB(fx);
	g_assert_false(axis_estimator_request_sample(fx->estimator, "nope", "root", &first, &created, &error));
	g_assert_error(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_true(axis_estimator_request_sample(fx->estimator, "space_saver", "root", &first, &created, &error));
	g_assert_true(created);
	/* One at a time: a second request is the same job. */
	g_assert_true(axis_estimator_request_sample(fx->estimator, "balanced", "root", &second, &created, &error));
	g_assert_false(created);
	g_assert_cmpint(first, ==, second);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE id = %" G_GINT64_FORMAT
		" AND queue = 'sample' AND priority = -100 AND max_attempts = 1", first), ==, 1);

	g_assert_true(axis_estimator_get_sample_status(fx->estimator, &status, &error));
	g_assert_cmpint(status.job_id, ==, first);
	g_assert_cmpstr(status.state, ==, "queued");
	g_assert_cmpstr(status.target, ==, "space_saver");
	axis_sample_status_clear(&status);

	g_assert_true(axis_estimator_cancel_sample(fx->estimator, &cancelled, &error));
	g_assert_true(cancelled);
	g_assert_true(axis_estimator_get_sample_status(fx->estimator, &status, &error));
	g_assert_cmpstr(status.state, ==, "cancelled");
	axis_sample_status_clear(&status);
	g_assert_true(axis_estimator_cancel_sample(fx->estimator, &cancelled, &error));
	g_assert_false(cancelled);

	/* A later one, cancelled too; then the first retried by a person, as
	 * itself (it keeps its older id): the status is the retried one. */
	g_assert_true(axis_estimator_request_sample(fx->estimator, "balanced", "root", &second, &created, &error));
	g_assert_true(created);
	g_assert_cmpint(second, >, first);
	g_assert_true(axis_estimator_cancel_sample(fx->estimator, &cancelled, &error));
	{
		static const gchar *const kinds[] = { AXIS_ESTIMATE_SAMPLE_KIND, NULL };
		static const gchar *const queues[] = { AXIS_ESTIMATE_SAMPLE_QUEUE, NULL };
		AxisJobRetryOutcome outcome = AXIS_JOB_RETRY_NOT_ENDED;
		AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);

		g_assert_no_error(error);
		g_assert_true(axis_job_retry(conn, first, kinds, queues, NULL, &outcome, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_DONE);
		axis_db_pool_release(fx->t->pool, conn);
	}
	g_assert_true(axis_estimator_get_sample_status(fx->estimator, &status, &error));
	g_assert_cmpint(status.job_id, ==, first);
	g_assert_cmpstr(status.state, ==, "queued");
	axis_sample_status_clear(&status);
}

/* The encoder's arguments: the file is descriptor 3, never a name; only
 * the one protocol; the output a pipe; the target's quality. */
static void
test_arguments(void)
{
	const gchar *const prefix[] = { "/usr/bin/ffmpeg", NULL };
	AxisEstimateTarget space = *axis_estimate_builtin_target("space_saver");
	AxisEstimateTarget compat = *axis_estimate_builtin_target("max_compat");
	AxisEstimateTarget flac = *axis_estimate_builtin_target("keep_lossless");
	AxisMediaStream stream;
	g_auto(GStrv) a = NULL;
	g_auto(GStrv) b = NULL;
	g_auto(GStrv) c = NULL;
	g_autofree gchar *joined = NULL;

	memset(&stream, 0, sizeof(stream));
	stream.index = 2;
	stream.channels = 2;
	stream.sample_rate = 96000;
	stream.bit_depth_audio = 24;
	a = axis_estimate_sample_argv(prefix, &space, &stream, 4, 10, 600.5, 20.0);
	joined = g_strjoinv(" ", a);
	g_assert_true(g_str_has_prefix(joined, "/usr/bin/ffmpeg -nostdin -hide_banner -loglevel error"));
	g_assert_nonnull(strstr(joined, " -protocol_whitelist fd "));
	g_assert_nonnull(strstr(joined, " -fd 3 -ss 600.500 -t 20.000 -i fd: -map 0:2 "));
	g_assert_nonnull(strstr(joined, " -c:v libx265 -preset medium -crf 26 -pix_fmt yuv420p10le "));
	g_assert_true(g_str_has_suffix(joined, " -f hevc pipe:1"));
	g_assert_null(strstr(joined, "/media"));
	g_clear_pointer(&joined, g_free);

	b = axis_estimate_sample_argv(prefix, &compat, &stream, 5, 8, 0, 2);
	joined = g_strjoinv(" ", b);
	g_assert_nonnull(strstr(joined, " -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p "));
	g_assert_true(g_str_has_suffix(joined, " -f h264 pipe:1"));
	g_clear_pointer(&joined, g_free);

	flac.reduce_to_cd = TRUE;
	c = axis_estimate_sample_argv(prefix, &flac, &stream, 4, 0, 0, 2);
	joined = g_strjoinv(" ", c);
	g_assert_nonnull(strstr(joined, " -vn -c:a flac -compression_level 8 -sample_fmt s16 -ar 44100 -f flac pipe:1"));
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

#define ADD(name, func) AXIS_TEST_ADD_DB_CASE("/estimate/sample/" name, Fixture, NULL, func)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("measure-real", test_measure_real);
	ADD("measure-nvenc-fake", test_measure_nvenc_fake);
	ADD("measure-tool-fails", test_measure_tool_fails);
	ADD("measure-cancel", test_measure_cancel);
	ADD("scratch-cleanup", test_scratch_cleanup);
	ADD("scratch-refused", test_scratch_refused);
	ADD("request-and-cancel", test_request_and_cancel);
	g_test_add_func("/estimate/sample/arguments", test_arguments);

	return g_test_run();
}
