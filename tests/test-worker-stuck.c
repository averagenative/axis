/*
 * test-worker-stuck.c - a job whose thread does not end (also while the
 * worker stops), and a conversion the worker must not write (an output name
 * taken, a read-only mount): real axis-worker processes against a stand-in
 * daemon on the test's main loop, with their job threads held in the kernel
 * by the debug build's stall (AXIS_WORKER_TEST_STALL, a FIFO the test holds
 * open)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What happened on a real run: a conversion read its source through sshfs
 * when the server went down, so the daemon and the mount went away at once.
 * The lease could not be renewed and the encoder was stopped, but the job's
 * thread stayed blocked on the dead mount; the worker gave the job up after
 * waiting for it, and when the thread returned at last the worker crashed:
 * the job's state had been freed while the thread's completion still
 * pointed at it.
 *
 * Here: the lease lapses (the daemon answers 503, as a reverse proxy does
 * whose backend is down, or refuses the heartbeats), the worker gives the
 * job up while its thread is blocked, the disk goes away and comes back, and
 * the thread returns later.  The worker must go on, say nothing more about
 * the job, remove what the thread left in the work area, and take no work on
 * the volume until it is proved again; and it must not gather stuck threads
 * without end.
 *
 * No database: the daemon is a stand-in that hands out the jobs the test
 * describes, each to a claim that lists the job's volume.  ffmpeg is a
 * stand-in that stands still or writes a few bytes, as the test says; the
 * real one only says its version (the capability measurement is seeded for
 * it).  No GPU, no media.  The stall exists in debug builds only, so the
 * tests skip in a release build.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-convert.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "storage/axis-file-check.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-profile.h"
#include "transcode/axis-verify.h"

#define UUID_A        "6c2d8e4f-1a3b-4c5d-9e7f-0a1b2c3d4e5f"
#define TOKEN         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ"
#define LEASE_MS      2000
#define BEAT_MS       500
#define POLL_MS       200
#define SOURCE_ROOT   "Films"
#define SOURCE_NAME   "Made Up Film (2001).mkv"
#define SOURCE_BYTES  (2 * 1024 * 1024)
/* The worker waits 15 s for a stopped job's thread before it gives the job
 * up; the rest is a loaded machine's slack. */
#define LONG_WAIT_MS  90000

#ifdef AXIS_DEBUG
#define HAVE_STALL TRUE
#else
#define HAVE_STALL FALSE
#endif

static gboolean have_ffmpeg;

/* Nothing is made before this: a test that skips touches nothing. */
#define SKIP_UNLESS_READY() do { \
	if (!HAVE_STALL) { \
		g_test_skip("the worker's stall for tests is in debug builds only (make check)"); \
		return; \
	} \
	if (!have_ffmpeg) { \
		g_test_skip("ffmpeg is not installed"); \
		return; \
	} \
} while (0)

typedef struct
{
	gboolean  gone;		/* everything is answered 503 */
	gboolean  refuse_beats;	/* heartbeats only: no lease is ever renewed */
	GQueue    jobs;		/* gchar *: claim answers, one per claim that lists the volume */
	guint     registers;
	guint     claims;
	guint     claims_listing;	/* claims that listed the volume */
	guint     completes;
	guint     fails;
	GString  *reports;	/* the bodies of every complete and fail */
} Daemon;

typedef struct
{
	gchar       *dir;
	gchar       *volume;	/* the disk's top directory */
	gchar       *away;	/* where it is while it is gone */
	gchar       *area;	/* its work area */
	gchar       *stall;	/* the FIFO */
	gint         stall_fd;	/* its other end, held by the test */
	gchar       *mode;	/* what the stand-in encoder does */
	gchar       *ffmpeg;
	struct stat  st;	/* the source's */
	gchar       *sample;	/* the digest of samples of the source */
	SoupServer  *server;
	gchar       *url;
	Daemon       d;
} Fixture;

/* ── The stand-in daemon ────────────────────────────────────────── */

static void
answer(
	SoupServerMessage *message,
	guint              status,
	gchar             *body
){
	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_TAKE, body, strlen(body));
}

static gchar *
request_body(SoupServerMessage *message)
{
	g_autoptr(GBytes) bytes = soup_message_body_flatten(soup_server_message_get_request_body(message));
	gsize length = 0;
	const gchar *data = g_bytes_get_data(bytes, &length);

	return g_strndup(data != NULL ? data : "", length);
}

static void
on_request(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           data
){
	Fixture *fx = data;
	Daemon *d = &fx->d;
	g_autofree gchar *body = request_body(message);

	if (d->gone) {
		answer(message, 503, axis_worker_error_to_json("internal", "the service is not available"));
		return;
	}
	if (g_str_has_suffix(path, "/register")) {
		g_autoptr(AxisWorkerRegistered) registered = axis_worker_registered_new();

		d->registers++;
		registered->worker = g_strdup("garage");
		registered->lease_ms = LEASE_MS;
		registered->heartbeat_ms = BEAT_MS;
		registered->poll_ms = POLL_MS;
		axis_worker_registered_add_volume(registered, UUID_A, TRUE, NULL);
		answer(message, 200, axis_worker_registered_to_json(registered));
	} else if (g_str_has_suffix(path, "/claim")) {
		gboolean listing = strstr(body, UUID_A) != NULL;
		gchar *job = listing ? g_queue_pop_head(&d->jobs) : NULL;

		d->claims++;
		if (listing)
			d->claims_listing++;
		answer(message, 200, job != NULL ? job : axis_worker_claimed_to_json(NULL, NULL));
	} else if (g_str_has_suffix(path, "/heartbeat")) {
		AxisWorkerBeat beat;

		if (d->refuse_beats) {
			answer(message, 503, axis_worker_error_to_json("internal", "the service is not available"));
			return;
		}
		memset(&beat, 0, sizeof(beat));
		beat.lease_ms = LEASE_MS;
		answer(message, 200, axis_worker_beat_to_json(&beat));
	} else if (g_str_has_suffix(path, "/complete")) {
		AxisWorkerCompleted completed;

		d->completes++;
		g_string_append_printf(d->reports, "%s\n", body);
		completed.recorded = 1;
		completed.refused = 0;
		answer(message, 200, axis_worker_completed_to_json(&completed));
	} else if (g_str_has_suffix(path, "/fail")) {
		d->fails++;
		g_string_append_printf(d->reports, "%s\n", body);
		answer(message, 200, axis_worker_failed_to_json("queued"));
	} else {
		answer(message, 404, axis_worker_error_to_json("not-found", "no such request"));
	}
}

/* ── The fixture ────────────────────────────────────────────────── */

/* A stand-in for ffmpeg: the real one says its version; any other run does
 * what the mode file says: "sleep" is an encode that takes long, "write" one
 * that writes a few bytes to its output (descriptor 4) and succeeds. */
static gchar *
stand_in_ffmpeg(Fixture *fx)
{
	g_autofree gchar *path = g_build_filename(fx->dir, "stand-in-ffmpeg.sh", NULL);
	static const gchar script[] =
		"# A stand-in for ffmpeg (test-worker-stuck.c).\n"
		"mode=$1\n"
		"shift\n"
		"for a in \"$@\"; do\n"
		"\tif [ \"$a\" = -version ]; then exec ffmpeg \"$@\"; fi\n"
		"done\n"
		"if [ \"$(cat \"$mode\")\" = write ]; then\n"
		"\tprintf 'a made-up conversion\\n' >&4\n"
		"\texit 0\n"
		"fi\n"
		"exec sleep 600\n";

	g_assert_true(g_file_set_contents(path, script, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return g_strdup_printf("/bin/sh;%s;%s", path, fx->mode);
}

static void
set_mode(
	Fixture     *fx,
	const gchar *mode
){
	axis_test_storage_write(fx->mode, mode);
}

/* The film the jobs are about: made-up bytes, with its stamp and the digest
 * of samples of it, as the daemon reads them when it hands out the work. */
static void
make_source(Fixture *fx)
{
	g_autofree gchar *root = g_build_filename(fx->volume, SOURCE_ROOT, NULL);
	g_autofree gchar *path = g_build_filename(root, SOURCE_NAME, NULL);
	g_autofree gchar *bytes = g_malloc(SOURCE_BYTES);
	gint err = 0;
	gint fd;
	guint i;

	for (i = 0; i < SOURCE_BYTES; i++)
		bytes[i] = (gchar)((i * 2654435761u) >> 24);
	axis_test_storage_mkdir(root);
	g_assert_true(g_file_set_contents(path, bytes, SOURCE_BYTES, NULL));
	g_assert_cmpint(g_stat(path, &fx->st), ==, 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(axis_file_sample_digest(fd, (gint64)fx->st.st_size, &fx->sample, &err));
	close(fd);
}

static Fixture *
fixture_new(void)
{
	g_autoptr(GError) error = NULL;
	Fixture *fx = g_new0(Fixture, 1);
	GSList *uris;

	axis_test_log_init();
	axis_test_log_reset();
	fx->dir = axis_test_storage_dir_new();
	fx->volume = g_build_filename(fx->dir, "disk", NULL);
	fx->away = g_build_filename(fx->dir, "disk-gone", NULL);
	fx->area = g_build_filename(fx->volume, ".axis-work", NULL);
	axis_test_storage_mkdir(fx->volume);
	axis_test_storage_write_marker(fx->volume, UUID_A);
	g_assert_cmpint(g_mkdir(fx->area, 0700), ==, 0);
	make_source(fx);

	/* Opened for reading and writing, the FIFO never blocks the test, and a
	 * job's thread that opens it to read blocks in read() until it is
	 * closed here. */
	fx->stall = g_build_filename(fx->dir, "stall", NULL);
	g_assert_cmpint(mkfifo(fx->stall, 0600), ==, 0);
	fx->stall_fd = open(fx->stall, O_RDWR | O_CLOEXEC);
	g_assert_cmpint(fx->stall_fd, >=, 0);

	fx->mode = g_build_filename(fx->dir, "encoder-mode", NULL);
	set_mode(fx, "sleep");
	fx->ffmpeg = stand_in_ffmpeg(fx);

	g_queue_init(&fx->d.jobs);
	fx->d.reports = g_string_new(NULL);
	fx->server = soup_server_new(NULL, NULL);
	g_assert_true(soup_server_listen_local(fx->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(fx->server);
	fx->url = g_strdup_printf("http://127.0.0.1:%u", (guint)g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	soup_server_add_handler(fx->server, NULL, on_request, fx, NULL);
	return fx;
}

static void
fixture_free(Fixture *fx)
{
	soup_server_disconnect(fx->server);
	g_object_unref(fx->server);
	if (fx->stall_fd >= 0)
		close(fx->stall_fd);
	g_queue_clear_full(&fx->d.jobs, g_free);
	g_string_free(fx->d.reports, TRUE);
	/* A disk that went away is put back, so that it is removed with the
	 * rest. */
	if (g_file_test(fx->away, G_FILE_TEST_IS_DIR))
		g_assert_cmpint(g_rename(fx->away, fx->volume), ==, 0);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->volume);
	g_free(fx->away);
	g_free(fx->area);
	g_free(fx->stall);
	g_free(fx->mode);
	g_free(fx->ffmpeg);
	g_free(fx->sample);
	g_free(fx->url);
	g_free(fx);
	axis_test_log_reset();
}

/* ── Jobs ───────────────────────────────────────────────────────── */

static AxisWorkerJob *
new_job(
	gint64       id,
	const gchar *kind
){
	AxisWorkerJob *job = g_new0(AxisWorkerJob, 1);

	job->id = id;
	job->kind = g_strdup(kind);
	job->lease_token = g_uuid_string_random();
	job->lease_ms = LEASE_MS;
	job->heartbeat_ms = BEAT_MS;
	job->attempt = 1;
	return job;
}

static gchar *
claimed(AxisWorkerJob *job)
{
	gchar *json = axis_worker_claimed_to_json(job, NULL);

	axis_worker_job_free(job);
	g_assert_nonnull(json);
	return json;
}

/* The picture and two tracks, as a probe would record them. */
static AxisMediaInfo *
film(void)
{
	AxisMediaInfo *info = axis_test_convert_info("matroska,webm");

	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_audio(info, 1, "ac3", 6, "eng", TRUE);
	axis_test_convert_subtitle(info, 2, "subrip", "eng", FALSE, FALSE);
	axis_test_convert_finish(info);
	return info;
}

/* A conversion of the source, with the vector the plan builder makes, its
 * output named @output (NULL: "<id>-1.tmp"). */
static void
add_convert_named(
	Fixture     *fx,
	gint64       id,
	const gchar *output
){
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisProfileDefinition def;
	AxisWorkerJob *job = new_job(id, AXIS_WORKER_KIND_CONVERT);
	AxisConvertWork *work = g_new0(AxisConvertWork, 1);

	g_assert_true(axis_profile_definition_parse(axis_profile_builtin("space_saver")->definition, -1, &def, NULL));
	if (!axis_convert_plan_build(&def, info, G_GINT64_CONSTANT(8000000000), AXIS_ENCODER_SOFTWARE, caps, NULL, &plan,
	                             &refusal))
		g_error("no plan: %s", axis_convert_refusal_to_string(refusal));
	work->volume = g_strdup(UUID_A);
	work->root = g_strdup(SOURCE_ROOT);
	work->path = g_strdup(SOURCE_NAME);
	work->size = (gint64)fx->st.st_size;
	work->mtime_sec = (gint64)fx->st.st_mtim.tv_sec;
	work->mtime_nsec = (gint)fx->st.st_mtim.tv_nsec;
	work->inode = (gint64)fx->st.st_ino;
	work->source_sample = g_strdup(fx->sample);
	work->output = output != NULL ? g_strdup(output) : g_strdup_printf("%" G_GINT64_FORMAT "-1.tmp", id);
	work->args = g_strdupv(plan->args);
	work->source_streams = info->streams->len;
	work->duration_ms = 60000;
	work->timeout_ms = 600000;
	job->convert = work;
	g_queue_push_tail(&fx->d.jobs, claimed(job));
}

static void
add_convert(
	Fixture *fx,
	gint64   id
){
	add_convert_named(fx, id, NULL);
}

/* The checks of a converted file that is not in the work area: the job
 * fails at once, then its thread stands still. */
static void
add_verify(
	Fixture *fx,
	gint64   id
){
	AxisWorkerJob *job = new_job(id, AXIS_WORKER_KIND_VERIFY);
	AxisVerifyWork *work = g_new0(AxisVerifyWork, 1);

	work->volume = g_strdup(UUID_A);
	work->root = g_strdup(SOURCE_ROOT);
	work->path = g_strdup(SOURCE_NAME);
	work->size = (gint64)fx->st.st_size;
	work->mtime_sec = (gint64)fx->st.st_mtim.tv_sec;
	work->mtime_nsec = (gint)fx->st.st_mtim.tv_nsec;
	work->inode = (gint64)fx->st.st_ino;
	work->source_sample = g_strdup(fx->sample);
	work->output = g_strdup_printf("%" G_GINT64_FORMAT "-1.tmp", id);
	work->output_size = 1000;
	work->plan = axis_verify_plan_new();
	work->plan->picture_source = 0;
	work->plan->picture_output = 0;
	work->plan->timeout_ms = 60000;
	job->verify = work;
	g_queue_push_tail(&fx->d.jobs, claimed(job));
}

/* A sample measurement of a film that is not there: a result at once (the
 * file skipped), then its thread stands still. */
static void
add_sample(
	Fixture *fx,
	gint64   id
){
	g_queue_push_tail(&fx->d.jobs, g_strdup_printf("{\"protocol\":%u,\"job\":{\"id\":%" G_GINT64_FORMAT ","
		"\"kind\":\"" AXIS_WORKER_KIND_SAMPLE "\",\"lease_token\":\"3b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c\","
		"\"lease_ms\":%u,\"heartbeat_ms\":%u,\"attempt\":1,"
		"\"description\":{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,"
		"\"audio_bitrate\":0,\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,"
		"\"excerpt_timeout_ms\":60000,\"files\":[{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"" SOURCE_ROOT "\","
		"\"path\":\"Another Made Up Film (1999).mkv\",\"size\":1000,\"mtime_sec\":1,\"mtime_nsec\":0,\"stream\":0,"
		"\"quality\":4,\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,\"sample_bits\":0,\"length_ms\":2000,"
		"\"starts_ms\":[0]}]}},\"paused\":null}", AXIS_WORKER_PROTOCOL, id, LEASE_MS, BEAT_MS));
}

/* ── The worker ─────────────────────────────────────────────────── */

static AxisTestWorker *
start_worker(
	Fixture     *fx,
	const gchar *extra
){
	const gchar *volumes[2];
	const gchar *env[3];
	AxisTestWorkerOptions options;

	volumes[0] = fx->volume;
	volumes[1] = NULL;
	env[0] = "AXIS_WORKER_TEST_STALL";
	env[1] = fx->stall;
	env[2] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = fx->url;
	options.token = TOKEN;
	options.volumes = volumes;
	options.ffmpeg = fx->ffmpeg;
	options.env = env;
	options.extra_config = extra;
	return axis_test_worker_start(&options);
}

static void
wait_log(
	AxisTestWorker *w,
	const gchar    *needle,
	guint           timeout_ms
){
	if (!axis_test_worker_wait_log(w, needle, timeout_ms))
		g_error("the worker did not log '%s' in time; its log:\n%s", needle, axis_test_worker_get_log(w));
}

static void
wait_logf(
	AxisTestWorker *w,
	guint           timeout_ms,
	const gchar    *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
wait_logf(
	AxisTestWorker *w,
	guint           timeout_ms,
	const gchar    *format,
	...
){
	g_autofree gchar *needle = NULL;
	va_list args;

	va_start(args, format);
	needle = g_strdup_vprintf(format, args);
	va_end(args);
	wait_log(w, needle, timeout_ms);
}

/* It is still there: no crash, no exit. */
static void
assert_running(AxisTestWorker *w)
{
	if (!axis_test_process_alive(axis_test_worker_get_pid(w)) || strstr(axis_test_worker_get_log(w), "Sanitizer") != NULL)
		g_error("the worker is gone; its log:\n%s", axis_test_worker_get_log(w));
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

/* The stand-in encoder the worker runs now. */
static GPid
wait_encoder(AxisTestWorker *w)
{
	ChildWait wait;

	wait.parent = axis_test_worker_get_pid(w);
	wait.found = 0;
	if (!axis_test_pump_until(child_there, &wait, AXIS_TEST_WAIT_MS))
		g_error("no encoder started; the worker's log:\n%s", axis_test_worker_get_log(w));
	return wait.found;
}

static gboolean
process_gone(gpointer data)
{
	return !axis_test_process_alive(*(GPid *)data);
}

static gboolean
dir_empty(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);

	return dir != NULL && g_dir_read_name(dir) == NULL;
}

static gboolean
scratch_empty(gpointer data)
{
	return axis_test_worker_scratch_empty(data);
}

static gboolean
area_empty(gpointer data)
{
	return dir_empty(((Fixture *)data)->area);
}

static gboolean
claims_listing(gpointer data)
{
	return ((Fixture *)data)->d.claims_listing > 0;
}

/* The threads blocked on the FIFO return: the FIFO goes first, so that no
 * later job stands still, then the test's end of it. */
static void
release(Fixture *fx)
{
	g_assert_cmpint(g_unlink(fx->stall), ==, 0);
	g_assert_cmpint(close(fx->stall_fd), ==, 0);
	fx->stall_fd = -1;
}

/* Stops it with SIGTERM and checks it ended with @status. */
static void
finish(
	AxisTestWorker *w,
	gint            status
){
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	if (axis_test_worker_get_status(w) != status)
		g_error("the worker exited %d, not %d; its log:\n%s", axis_test_worker_get_status(w), status,
			axis_test_worker_get_log(w));
	g_assert_null(strstr(axis_test_worker_get_log(w), TOKEN));
	axis_test_worker_free(w);
}

/* ── The tests ──────────────────────────────────────────────────── */

/* The real run's sequence: the daemon goes away during an encode, the lease
 * lapses and the encoder is stopped, the job's thread stays blocked, the disk
 * goes away, the worker gives the job up, and then the thread returns. */
static void
test_encode_given_up(void)
{
	g_autofree gchar *lost = NULL;
	AxisTestWorker *w;
	Fixture *fx;
	GPid encoder;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	add_convert(fx, 7001);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7001 (convert.encode), attempt 1: started", AXIS_TEST_WAIT_MS);
	encoder = wait_encoder(w);

	/* The server goes down: no heartbeat is answered, the lease lapses and
	 * the encoder is stopped; the job's thread, done with it, stays blocked
	 * in the kernel. */
	fx->d.gone = TRUE;
	wait_log(w, "job 7001: its lease could not be renewed before its deadline: its encoder is stopped",
		AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(process_gone, &encoder, AXIS_TEST_WAIT_MS));
	wait_log(w, "job 7001: its thread stands still", AXIS_TEST_WAIT_MS);
	/* The daemon answers again; the job's lease is lost for good. */
	fx->d.gone = FALSE;

	/* The disk goes too: no more work on it. */
	g_assert_cmpint(g_rename(fx->volume, fx->away), ==, 0);
	lost = g_strdup_printf("volume " UUID_A " at %s is not the one proved any more", fx->volume);
	wait_log(w, lost, AXIS_TEST_WAIT_MS);

	/* The thread does not end: the job is given up while the thread is
	 * still blocked. */
	wait_log(w, "job 7001: its thread does not end; giving the job up", LONG_WAIT_MS);
	axis_test_pump_for(300);
	assert_running(w);

	/* The thread returns at last.  The worker goes on, and says nothing
	 * about the job: it was fenced. */
	release(fx);
	wait_log(w, "job 7001: its thread ended", AXIS_TEST_WAIT_MS);
	axis_test_pump_for(500);
	assert_running(w);
	g_assert_cmpuint(fx->d.completes, ==, 0);
	g_assert_cmpuint(fx->d.fails, ==, 0);
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));

	/* While the disk is gone, no claim offers it. */
	fx->d.claims_listing = 0;
	{
		guint before = fx->d.claims;

		axis_test_pump_for(4 * POLL_MS);
		g_assert_cmpuint(fx->d.claims, >, before);
		g_assert_cmpuint(fx->d.claims_listing, ==, 0);
	}

	/* It comes back: proved again, registered again, offered again. */
	g_assert_cmpint(g_rename(fx->away, fx->volume), ==, 0);
	if (!axis_test_pump_until(claims_listing, fx, AXIS_TEST_WAIT_MS))
		g_error("the volume was not offered again; the worker's log:\n%s", axis_test_worker_get_log(w));
	/* The encode removed its output when it was stopped. */
	g_assert_true(area_empty(fx));
	finish(w, 0);
	fixture_free(fx);
}

/* A conversion whose encode finished, a converted file's checks and a
 * sample measurement, each with its thread blocked after its work: all three
 * leases lapse and the three jobs are given up.  When the threads return,
 * nothing they did is reported, and the conversion's output, which the daemon
 * never took, is removed. */
static void
test_kinds_given_up(void)
{
	g_autofree gchar *output = NULL;
	AxisTestWorker *w;
	Fixture *fx;
	gint64 id;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	set_mode(fx, "write");
	add_convert(fx, 7101);
	add_verify(fx, 7102);
	add_sample(fx, 7103);
	w = start_worker(fx, "max_jobs = 3\n");
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7101 (convert.encode), attempt 1: started", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7102 (convert.verify), attempt 1: started", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7103 (" AXIS_WORKER_KIND_SAMPLE "), attempt 1: started", AXIS_TEST_WAIT_MS);
	for (id = 7101; id <= 7103; id++)
		wait_logf(w, AXIS_TEST_WAIT_MS, "job %" G_GINT64_FORMAT ": its thread stands still", id);
	/* The finished encode's output is there, held by the blocked thread. */
	output = g_build_filename(fx->area, "7101-1.tmp", NULL);
	g_assert_true(g_file_test(output, G_FILE_TEST_IS_REGULAR));

	/* No lease is renewed from now on. */
	fx->d.refuse_beats = TRUE;
	for (id = 7101; id <= 7103; id++)
		wait_logf(w, AXIS_TEST_WAIT_MS, "job %" G_GINT64_FORMAT ": its lease could not be renewed", id);

	for (id = 7101; id <= 7103; id++)
		wait_logf(w, LONG_WAIT_MS, "job %" G_GINT64_FORMAT ": its thread does not end; giving the job up", id);
	axis_test_pump_for(300);
	assert_running(w);

	release(fx);
	for (id = 7101; id <= 7103; id++)
		wait_logf(w, AXIS_TEST_WAIT_MS, "job %" G_GINT64_FORMAT ": its thread ended", id);
	wait_log(w, "job 7101: its output was removed from the work area", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(area_empty, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	axis_test_pump_for(500);
	assert_running(w);
	/* Fenced, all three: not a word about them, results and all. */
	if (fx->d.completes != 0 || fx->d.fails != 0)
		g_error("the worker reported on a job it gave up:\n%s", fx->d.reports->str);
	/* Still asking for work, on the volume too. */
	fx->d.claims_listing = 0;
	g_assert_true(axis_test_pump_until(claims_listing, fx, AXIS_TEST_WAIT_MS));
	finish(w, 0);
	fixture_free(fx);
}

/* Threads that stay stuck are not gathered without end: one more than the
 * worker runs at once, and it gives its work back and exits 1, saying why, so
 * that it is started again. */
static void
test_too_many_stuck(void)
{
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	fx->d.refuse_beats = TRUE;
	add_convert(fx, 7201);
	add_sample(fx, 7202);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7201 (convert.encode), attempt 1: started", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7201: its thread stands still", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7201: its thread does not end; giving the job up", LONG_WAIT_MS);

	/* One stuck thread: the worker goes on, and takes work on the volume
	 * once it proved it again. */
	wait_log(w, "job 7202 (" AXIS_WORKER_KIND_SAMPLE "), attempt 1: started", AXIS_TEST_WAIT_MS);
	assert_running(w);
	wait_log(w, "job 7202: its thread stands still", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7202: its thread does not end; giving the job up", LONG_WAIT_MS);

	/* Two: more than the one job it runs at once. */
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	if (axis_test_worker_get_status(w) != 1)
		g_error("the worker exited %d, not 1; its log:\n%s", axis_test_worker_get_status(w),
			axis_test_worker_get_log(w));
	wait_log(w, "the threads of 2 jobs (7201, 7202) do not end", AXIS_TEST_WAIT_MS);
	g_assert_cmpuint(fx->d.completes, ==, 0);
	axis_test_worker_free(w);
	fixture_free(fx);
}

/* Spec (worker-protocol, A job whose thread does not end; hunt finding H2): a
 * worker that is stopped (SIGTERM: a container restarted) while a job's
 * thread stands still gives the job up and reports it as a failure for
 * another try, never as a job given back by a clean stop (which uses no
 * attempt).  What the thread left in the work area stays until the daemon's
 * tidy job; the next claim, under a name of its own, is not blocked by it. */
static void
test_stopped_while_stuck(void)
{
	g_autofree gchar *output = NULL;
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	set_mode(fx, "write");
	add_convert(fx, 7301);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7301 (convert.encode), attempt 1: started", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7301: its thread stands still", AXIS_TEST_WAIT_MS);
	output = g_build_filename(fx->area, "7301-1.tmp", NULL);
	g_assert_true(g_file_test(output, G_FILE_TEST_IS_REGULAR));

	/* Stopped while the thread stands still. */
	axis_test_worker_signal(w, SIGTERM);
	wait_log(w, "job 7301: its thread does not end; giving the job up", LONG_WAIT_MS);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, 0);
	g_assert_cmpuint(fx->d.fails, ==, 1);
	g_assert_cmpuint(fx->d.completes, ==, 0);
	/* A failure for another try, not a give-back. */
	g_assert_nonnull(strstr(fx->d.reports->str, "\"retry\":true,\"stopping\":false"));
	g_assert_nonnull(strstr(fx->d.reports->str, "the worker gave the job up"));
	/* What the thread made is still in the work area. */
	g_assert_true(g_file_test(output, G_FILE_TEST_IS_REGULAR));
	axis_test_worker_free(w);
	release(fx);

	/* The next claim has a name of its own (the daemon names the output by
	 * the claim): it is not blocked by what the given-up one left. */
	fx->d.fails = 0;
	g_string_truncate(fx->d.reports, 0);
	add_convert_named(fx, 7301, "7301-2.tmp");
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7301 (convert.encode), attempt 1: started", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7301: reported", AXIS_TEST_WAIT_MS);
	g_assert_cmpuint(fx->d.fails, ==, 0);
	g_assert_cmpuint(fx->d.completes, ==, 1);
	g_assert_nonnull(strstr(fx->d.reports->str, "\"output\":\"7301-2.tmp\""));
	finish(w, 0);
	fixture_free(fx);
}

/* Spec (conversion-execution, Workers write only in the work area: Name
 * already exists): a worker given an output name that is taken in the work
 * area fails the job for good without overwriting the file there. */
static void
test_name_exists(void)
{
	g_autofree gchar *taken = NULL;
	g_autofree gchar *content = NULL;
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	release(fx);
	set_mode(fx, "write");
	taken = g_build_filename(fx->area, "7401-1.tmp", NULL);
	axis_test_storage_write(taken, "somebody else's file");
	add_convert(fx, 7401);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7401: failed", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7401: reported", AXIS_TEST_WAIT_MS);
	g_test_message("the worker's report: %s", fx->d.reports->str);
	g_assert_nonnull(strstr(fx->d.reports->str, "exists already in the work area: it is not overwritten"));
	g_assert_nonnull(strstr(fx->d.reports->str, "\"retry\":false"));
	g_assert_true(g_file_get_contents(taken, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "somebody else's file");
	finish(w, 0);
	fixture_free(fx);
}

/* Spec (worker-protocol, A job given up while the worker stops; review of
 * fix-conversion-hunt-findings, item 7): a job whose thread failed for good
 * (its output name is taken) and then stands still is given up, and is
 * reported with its own failure, not tried again: being given up does not
 * turn a permanent failure into one for another try, nor hide why. */
static void
test_given_up_after_failing(void)
{
	g_autofree gchar *taken = NULL;
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	set_mode(fx, "write");
	taken = g_build_filename(fx->area, "7501-1.tmp", NULL);
	axis_test_storage_write(taken, "somebody else's file");
	add_convert(fx, 7501);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7501: its thread stands still", AXIS_TEST_WAIT_MS);

	axis_test_worker_signal(w, SIGTERM);
	wait_log(w, "job 7501: its thread does not end; giving the job up", LONG_WAIT_MS);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_test_message("the worker's report: %s", fx->d.reports->str);
	g_assert_cmpuint(fx->d.fails, ==, 1);
	g_assert_nonnull(strstr(fx->d.reports->str, "exists already in the work area: it is not overwritten"));
	g_assert_nonnull(strstr(fx->d.reports->str, "\"retry\":false,\"stopping\":false"));
	g_assert_null(strstr(fx->d.reports->str, "the worker gave the job up"));
	axis_test_worker_free(w);
	release(fx);
	fixture_free(fx);
}

/* ── A read-only mount (in a user and mount namespace) ──────────── */

static gboolean
namespaces_available(void)
{
	pid_t pid = fork();
	gint status = 0;

	if (pid == 0)
		_exit(unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0 ? 0 : 1);
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return FALSE;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void
write_proc(
	const gchar *path,
	const gchar *text
){
	gint fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		g_error("open %s: %s", path, g_strerror(errno));
	if (write(fd, text, strlen(text)) != (gssize)strlen(text))
		g_error("write %s: %s", path, g_strerror(errno));
	close(fd);
}

/* In the child, before any thread exists: root of a new user namespace with
 * a private mount namespace (as tests/test-storage-mounts.c does). */
static void
enter_namespace(void)
{
	g_autofree gchar *uid_map = g_strdup_printf("0 %u 1\n", (guint)getuid());
	g_autofree gchar *gid_map = g_strdup_printf("0 %u 1\n", (guint)getgid());

	if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0)
		g_error("prctl: %s", g_strerror(errno));
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
		g_error("unshare: %s", g_strerror(errno));
	write_proc("/proc/self/setgroups", "deny");
	write_proc("/proc/self/uid_map", uid_map);
	write_proc("/proc/self/gid_map", gid_map);
	g_assert_cmpint(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), ==, 0);
}

/* Spec (worker-protocol, Conversion work: Read-only mount): a worker whose
 * mount of the volume is read-only fails the job without creating
 * anything. */
static void
test_read_only_mount(void)
{
	AxisTestWorker *w;
	Fixture *fx;

	if (!g_test_subprocess()) {
		if (!HAVE_STALL || !have_ffmpeg) {
			g_test_skip("needs a debug build and ffmpeg");
			return;
		}
		if (!namespaces_available()) {
			g_test_skip("unprivileged user namespaces are not available here");
			return;
		}
		g_test_trap_subprocess(NULL, 120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
		g_test_trap_assert_passed();
		return;
	}
	enter_namespace();
	if (!axis_test_worker_seed_caps(FALSE))
		g_error("the capability measurement could not be seeded");
	fx = fixture_new();
	release(fx);
	set_mode(fx, "write");
	/* The disk: a filesystem of its own (the worker keeps its state and
	 * scratch off its volumes' filesystems), made again on it, then mounted
	 * read-only where the worker sees it. */
	if (mount("axis-test", fx->volume, "tmpfs", 0, "size=16m,mode=0755") != 0)
		g_error("mount tmpfs on %s: %s", fx->volume, g_strerror(errno));
	axis_test_storage_write_marker(fx->volume, UUID_A);
	g_assert_cmpint(g_mkdir(fx->area, 0700), ==, 0);
	g_clear_pointer(&fx->sample, g_free);
	make_source(fx);
	if (mount(NULL, fx->volume, NULL, MS_REMOUNT | MS_RDONLY, "size=16m,mode=0755") != 0)
		g_error("remount %s read-only: %s", fx->volume, g_strerror(errno));
	add_convert(fx, 7501);
	w = start_worker(fx, NULL);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7501: failed", AXIS_TEST_WAIT_MS);
	wait_log(w, "job 7501: reported", AXIS_TEST_WAIT_MS);
	g_test_message("the worker's report: %s", fx->d.reports->str);
	g_assert_nonnull(strstr(fx->d.reports->str, "the volume is mounted read-only on this worker: nothing is written"));
	g_assert_true(area_empty(fx));
	finish(w, 0);
	g_assert_cmpint(umount2(fx->volume, 0), ==, 0);
	fixture_free(fx);
	axis_test_worker_cleanup();
}

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	/* A subprocess (the read-only mount's) enters a user namespace before
	 * any thread exists, and measures there. */
	if (!g_test_subprocess())
		have_ffmpeg = HAVE_STALL && axis_test_worker_seed_caps(FALSE);
	g_test_add_func("/worker/stuck/encode-given-up", test_encode_given_up);
	g_test_add_func("/worker/stuck/kinds-given-up", test_kinds_given_up);
	g_test_add_func("/worker/stuck/too-many", test_too_many_stuck);
	g_test_add_func("/worker/stuck/stopped-while-stuck", test_stopped_while_stuck);
	g_test_add_func("/worker/stuck/name-exists", test_name_exists);
	g_test_add_func("/worker/stuck/given-up-after-failing", test_given_up_after_failing);
	g_test_add_func("/worker/stuck/read-only-mount", test_read_only_mount);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
