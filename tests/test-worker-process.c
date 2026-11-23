/*
 * test-worker-process.c - real axis-worker processes against the worker API
 * on the test's own main loop: its configuration's refusals; registering
 * and measuring its encoders; fencing when it was stopped (SIGSTOP) or the
 * daemon did not answer; stopping when its job was cancelled; giving work
 * back on SIGTERM; a SIGKILL and a restart; proxies ignored; a hostile
 * daemon's answers refused; revoked
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every worker runs software encoders only.  Its capability measurement is
 * written beforehand for this machine's ffmpeg (axis_test_worker_seed_caps),
 * so a worker starts without measuring.  Long encodes are a stand-in
 * ffmpeg that, while a flag file exists, becomes `sleep 600`: a child of the
 * worker the test can watch die.  No test reads media.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "jobs/axis-job-lifecycle.h"
#include "web/axis-password.h"

#define UUID_A "7e1f2a3b-4c5d-4e6f-8a9b-0c1d2e3f4a5b"
#define LONG_WAIT_MS 120000

static gboolean have_ffmpeg;

typedef struct
{
	AxisTestDb     *db;
	AxisDbPool     *pool;
	AxisTestDaemon *d;
	gchar          *token;
	gchar          *dir;
	gchar          *volume;
	gchar          *flag;
	gchar          *ffmpeg;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)
#define SKIP_WITHOUT_FFMPEG() do { if (!have_ffmpeg) { g_test_skip("ffmpeg is not installed"); return; } } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");
	fx->dir = axis_test_storage_dir_new();
	fx->volume = g_build_filename(fx->dir, "disk", NULL);
	axis_test_storage_mkdir(fx->volume);
	axis_test_storage_write_marker(fx->volume, UUID_A);
	fx->flag = g_build_filename(fx->dir, "stand-still", NULL);
	fx->ffmpeg = axis_test_slow_ffmpeg(fx->dir, fx->flag);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->d = axis_test_daemon_new(fx->pool, NULL, NULL, NULL);
	fx->token = axis_test_daemon_add_worker(fx->d, "garage", NULL);
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO volume (uuid, name, path, mount_root, state,"
			" marker_created_at) VALUES ('" UUID_A "', 'Disk', '%s', false, 'online', now())", fx->volume);

		axis_test_db_exec(fx->db, sql);
	}
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db != NULL) {
		axis_test_daemon_free(fx->d);
		axis_db_pool_free(fx->pool);
		axis_test_db_free(fx->db);
		axis_secret_free(fx->token);
	}
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->volume);
	g_free(fx->flag);
	g_free(fx->ffmpeg);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestWorker *
start_worker(
	Fixture            *fx,
	const gchar        *url,
	const gchar *const *env,
	const gchar        *scratch
){
	const gchar *volumes[2];
	AxisTestWorkerOptions options;

	volumes[0] = fx->volume;
	volumes[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = url != NULL ? url : fx->d->url;
	options.token = fx->token;
	options.volumes = volumes;
	options.ffmpeg = fx->ffmpeg;
	options.env = env;
	options.scratch = scratch;
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
wait_registered(AxisTestWorker *w)
{
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
}

/* Stops it with SIGTERM and checks it ended well, its token never in its
 * log. */
static void
finish(
	Fixture        *fx,
	AxisTestWorker *w,
	gint            status
){
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	if (axis_test_worker_get_status(w) != status)
		g_error("the worker exited %d, not %d; its log:\n%s", axis_test_worker_get_status(w), status,
			axis_test_worker_get_log(w));
	if (fx->token != NULL)
		g_assert_null(strstr(axis_test_worker_get_log(w), fx->token));
	axis_test_worker_free(w);
}

static gint64
request_measure(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_worker_store_request_measure(fx->pool, "garage", &id, NULL, &error));
	g_assert_no_error(error);
	return id;
}

static gboolean
wait_job_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state,
	guint        timeout_ms
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_wait_query(fx->db, sql, state, timeout_ms);
}

static void
assert_job(
	Fixture     *fx,
	gint64       id,
	const gchar *column,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_queryf(fx->db, "SELECT %s FROM job WHERE id = %" G_GINT64_FORMAT, column, id);

	if (g_strcmp0(got, expected) != 0)
		g_error("job %" G_GINT64_FORMAT " %s: expected %s, got %s", id, column, expected != NULL ? expected : "NULL",
			got != NULL ? got : "NULL");
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

static void
wait_gone(GPid pid)
{
	g_assert_true(axis_test_pump_until(process_gone, &pid, AXIS_TEST_WAIT_MS));
}

static gboolean
scratch_empty(gpointer data)
{
	return axis_test_worker_scratch_empty(data);
}

static void
stand_still(
	Fixture  *fx,
	gboolean  on
){
	if (on)
		axis_test_storage_write(fx->flag, "");
	else
		g_assert_cmpint(g_unlink(fx->flag), ==, 0);
}

/* A worker that has claimed a capability job and is in its encoder. */
static AxisTestWorker *
start_busy(
	Fixture     *fx,
	const gchar *scratch,
	gint64      *id,
	GPid        *encoder
){
	AxisTestWorker *w;
	g_autofree gchar *started = NULL;

	stand_still(fx, TRUE);
	w = start_worker(fx, NULL, NULL, scratch);
	wait_registered(w);
	*id = request_measure(fx);
	started = g_strdup_printf("job %" G_GINT64_FORMAT " (worker.capabilities), attempt 1: started", *id);
	wait_log(w, started, AXIS_TEST_WAIT_MS);
	*encoder = wait_encoder(w);
	return w;
}

/* ── Configuration ──────────────────────────────────────────────── */

static gint
run_refused(
	Fixture            *fx,
	const gchar        *url,
	const gchar *const *env,
	const gchar *const *args,
	const gchar        *extra,
	const gchar        *expect
){
	const gchar *volumes[2];
	AxisTestWorkerOptions options;
	AxisTestWorker *w;
	gint status;

	volumes[0] = fx->volume;
	volumes[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = url;
	options.token = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";
	options.volumes = volumes;
	options.env = env;
	options.args = args;
	options.extra_config = extra;
	w = axis_test_worker_start(&options);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	status = axis_test_worker_get_status(w);
	if (strstr(axis_test_worker_get_log(w), expect) == NULL)
		g_error("expected '%s' in:\n%s", expect, axis_test_worker_get_log(w));
	/* The token is never repeated, not even in a refusal. */
	g_assert_null(strstr(axis_test_worker_get_log(w), options.token));
	axis_test_worker_free(w);
	return status;
}

static void
test_config(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *inside = NULL;
	gint wait_status = 0;
	const gchar *argv[] = { AXIS_WORKER_PATH, "--version", NULL };

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
		&wait_status, NULL));
	g_assert_true(g_spawn_check_wait_status(wait_status, NULL));
	g_assert_true(g_str_has_prefix(out, "axis-worker "));

	{
		const gchar *const args[] = { "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ", NULL };

		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, args, NULL, "unexpected argument"), ==, 1);
	}
	/* Plain http to another machine: never, unless told. */
	g_assert_cmpint(run_refused(fx, "http://192.0.2.10:8080", NULL, NULL, NULL,
		"refusing to send the worker's token over plain http to 192.0.2.10"), ==, 1);
	g_assert_cmpint(run_refused(fx, "http://user:secret@127.0.0.1:9", NULL, NULL, NULL,
		"may have no user name, password, query or fragment"), ==, 1);
	g_assert_cmpint(run_refused(fx, "ftp://127.0.0.1/", NULL, NULL, NULL, "is not an http or https URL"), ==, 1);
	/* The token from one place only. */
	{
		const gchar *const env[] = { "AXIS_WORKER_TOKEN", "zyxwvutsrqponmlkjihgfedcbaZYXWVUTSRQPONMLKJ", NULL };

		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", env, NULL, NULL, "the token is given twice"), ==, 1);
	}
	/* Nothing it writes on a volume: refused before anything is made there,
	 * also through a link. */
	{
		g_autofree gchar *before = axis_test_storage_digest(fx->volume);
		g_autofree gchar *after = NULL;
		g_autofree gchar *link = g_build_filename(fx->dir, "to-disk", NULL);
		g_autofree gchar *through = NULL;

		inside = g_strdup_printf("scratch = %s/scratch\n", fx->volume);
		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, NULL, inside, "is on or holds the volume path"),
			==, 1);
		g_assert_cmpint(symlink(fx->volume, link), ==, 0);
		through = g_strdup_printf("state = %s/state\n", link);
		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, NULL, through, "is on or holds the volume path"),
			==, 1);
		after = axis_test_storage_digest(fx->volume);
		g_assert_cmpstr(before, ==, after);
	}
	g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, NULL, "encoder_families = software, cuda\n",
		"names an encoder family other than"), ==, 1);
	/* A mount without file identities is one of the volume paths, as
	 * written there, and a plain absolute path. */
	g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, NULL,
		"volumes_without_file_identity = /mnt/elsewhere\n", "which is not one of the volume paths"), ==, 1);
	{
		g_autofree gchar *slash = g_strdup_printf("volumes_without_file_identity = %s/\n", fx->volume);
		const gchar *const env[] = { "AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY", "relative/disk", NULL };

		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", NULL, NULL, slash, "must be an absolute path"), ==, 1);
		g_assert_cmpint(run_refused(fx, "http://127.0.0.1:9", env, NULL, NULL,
			"every path of AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY must be an absolute path"), ==, 1);
	}
}

/* Spec (worker-protocol): a volume listed as seen through a mount that does
 * not keep file identities is reported so at registration; the others are
 * not. */
static void
test_mount_without_identity(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *extra = g_strdup_printf("volumes_without_file_identity = %s\n", fx->volume);
	g_autofree gchar *said = NULL;
	g_autofree gchar *kept = NULL;
	const gchar *volumes[2];
	AxisTestWorkerOptions options;
	AxisTestWorker *w;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	volumes[0] = fx->volume;
	volumes[1] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = fx->d->url;
	options.token = fx->token;
	options.volumes = volumes;
	options.ffmpeg = fx->ffmpeg;
	options.extra_config = extra;
	w = axis_test_worker_start(&options);
	wait_registered(w);
	said = g_strdup_printf("volume " UUID_A " at %s: proved and accepted (a mount that does not keep file identities",
		fx->volume);
	wait_log(w, said, AXIS_TEST_WAIT_MS);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "volumes_without_file_identity="));
	kept = axis_test_db_query(fx->db, "SELECT keeps_identity::text FROM worker_volume");
	g_assert_cmpstr(kept, ==, "false");
	finish(fx, w, 0);

	/* Without the setting: as strict as before. */
	w = start_worker(fx, NULL, NULL, NULL);
	wait_registered(w);
	g_clear_pointer(&kept, g_free);
	kept = axis_test_db_query(fx->db, "SELECT keeps_identity::text FROM worker_volume");
	g_assert_cmpstr(kept, ==, "true");
	g_assert_null(strstr(axis_test_worker_get_log(w), "a mount that does not keep file identities"));
	finish(fx, w, 0);
}

/* ── Registering, measuring ─────────────────────────────────────── */

static void
test_register_measure(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *reported = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *proved = NULL;
	AxisTestWorker *w;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_worker(fx, NULL, NULL, NULL);
	wait_registered(w);
	proved = g_strdup_printf("volume " UUID_A " at %s: proved and accepted", fx->volume);
	wait_log(w, proved, AXIS_TEST_WAIT_MS);
	path = axis_test_db_query(fx->db, "SELECT path FROM worker_volume");
	g_assert_cmpstr(path, ==, fx->volume);
	version = axis_test_db_query(fx->db, "SELECT ffmpeg_version FROM worker WHERE name = 'garage'");
	g_assert_nonnull(version);

	/* Measured again on request, for real (software encoders only). */
	id = request_measure(fx);
	g_assert_true(wait_job_state(fx, id, "succeeded", LONG_WAIT_MS));
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);
	wait_log(w, reported, AXIS_TEST_WAIT_MS);
	{
		g_autofree gchar *tool = axis_test_db_query(fx->db,
			"SELECT tool_version FROM encoder_tool WHERE machine = 'worker-garage'");
		g_autofree gchar *hardware = axis_test_db_query(fx->db, "SELECT count(*) FROM encoder_capability"
			" WHERE machine = 'worker-garage' AND (encoder LIKE '%nvenc%' OR encoder LIKE '%vaapi%') AND usable");

		g_assert_cmpstr(tool, ==, version);
		g_assert_cmpstr(hardware, ==, "0");
	}
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));

	/* In touch: healthy. */
	{
		const gchar *argv[] = { AXIS_WORKER_PATH, "--config", axis_test_worker_get_config(w), "--healthcheck", NULL };
		gint status = 0;

		g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
			NULL, NULL, NULL, NULL, &status, NULL));
		g_assert_true(g_spawn_check_wait_status(status, NULL));
	}
	finish(fx, w, 0);
}

/* Answers the first @limit of the worker's reports of a job's end (or its
 * heartbeats, with @heartbeats) 503, 429, 503 ..., as a proxy or the
 * daemon's connection limits would: plain text, not the worker API's own
 * refusal. */
typedef struct
{
	gboolean heartbeats;
	guint    limit;
	guint    refused;
} Refuser;

static void
refuse_some(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	Refuser *refuser = user_data;
	const gchar *path = htmx_request_get_path(htmx_context_get_request(context));
	gboolean is = refuser->heartbeats ? g_str_has_suffix(path, "/heartbeat") :
		(g_str_has_suffix(path, "/complete") || g_str_has_suffix(path, "/fail"));

	if (is && refuser->refused < refuser->limit) {
		HtmxResponse *response = htmx_response_new();

		htmx_response_set_status(response, refuser->refused % 2 == 0 ? 503 : 429);
		htmx_response_set_content(response, "busy, try again");
		htmx_response_set_content_type(response, "text/plain");
		htmx_response_add_header(response, "Retry-After", "1");
		refuser->refused++;
		htmx_context_set_response(context, response);
		return;
	}
	next(context, next_data);
}

/* Replaces the fixture's daemon with one of @lease_ms and @heartbeat_ms
 * whose answers @refuser may take, and a worker "shed" for it. */
static void
own_daemon(
	Fixture *fx,
	guint    lease_ms,
	guint    heartbeat_ms,
	Refuser *refuser
){
	AxisWorkerApiTiming timing;

	axis_test_daemon_free(fx->d);
	axis_secret_free(fx->token);
	memset(&timing, 0, sizeof(timing));
	timing.lease_ms = lease_ms;
	timing.heartbeat_ms = heartbeat_ms;
	timing.poll_ms = 200;
	timing.reap_ms = 50;
	timing.backoff_base_ms = 50;
	timing.backoff_cap_ms = 100;
	fx->d = axis_test_daemon_new(fx->pool, NULL, &timing, NULL);
	fx->token = axis_test_daemon_add_worker(fx->d, "shed", NULL);
	htmx_server_use(fx->d->server, refuse_some, refuser, NULL);
}

static gint64
request_measure_shed(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_worker_store_request_measure(fx->pool, "shed", &id, NULL, &error));
	g_assert_no_error(error);
	return id;
}

/*
 * Found by the review of fix-web-hunt-findings: a heartbeat answered 503 or
 * 429 waited for the next beat, so three in a row (a busy proxy for a few
 * seconds) let the deadline pass and the worker killed a good encode.  A
 * heartbeat that did not renew is sent again sooner, with a back-off,
 * while the deadline allows.  Here the deadline is 6 s after a renewal and
 * the beat every 2 s: the third regular beat would come too late.
 */
static void
test_heartbeat_tried_again(Fixture *fx, gconstpointer data)
{
	Refuser refuser = { TRUE, 3, 0 };
	g_autofree gchar *started = NULL;
	AxisTestWorker *w;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	own_daemon(fx, 8000, 2000, &refuser);
	stand_still(fx, TRUE);
	w = start_worker(fx, NULL, NULL, NULL);
	wait_log(w, "registered as shed", AXIS_TEST_WAIT_MS);
	id = request_measure_shed(fx);
	started = g_strdup_printf("job %" G_GINT64_FORMAT " (worker.capabilities), attempt 1: started", id);
	wait_log(w, started, AXIS_TEST_WAIT_MS);
	encoder = wait_encoder(w);

	/* Three heartbeats refused, then renewed, the encoder still at work. */
	wait_log(w, "a heartbeat was answered 503", LONG_WAIT_MS);
	wait_log(w, "a heartbeat was answered 429", LONG_WAIT_MS);
	{
		gint64 deadline = g_get_monotonic_time() + (gint64)LONG_WAIT_MS * 1000;

		while (refuser.refused < 3 && g_get_monotonic_time() < deadline)
			axis_test_pump_for(50);
	}
	g_assert_cmpuint(refuser.refused, ==, 3);
	/* Past the deadline the last renewal before them gave: renewed since. */
	axis_test_pump_for(8000);
	g_assert_true(axis_test_process_alive(encoder));
	g_assert_null(strstr(axis_test_worker_get_log(w), "could not be renewed"));
	g_assert_null(strstr(axis_test_worker_get_log(w), "its encoder is stopped"));
	assert_job(fx, id, "state", "running");

	stand_still(fx, FALSE);
	finish(fx, w, 0);
}

/*
 * Found by the review of fix-web-hunt-findings: a report answered anything
 * but 200 was taken for a refusal and the job dropped, so a 429 or 503 from
 * a proxy or the daemon's connection limits threw an encode away.  It is
 * sent again, with a back-off, until the lease's deadline; only the worker
 * API's own refusal ends it.
 */
static void
test_report_tried_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *reported = NULL;
	Refuser refuser = { FALSE, 2, 0 };
	AxisTestWorker *w;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	/* A daemon of its own, with a lease long enough for a few tries. */
	own_daemon(fx, 10000, 0, &refuser);
	w = start_worker(fx, NULL, NULL, NULL);
	wait_log(w, "registered as shed", AXIS_TEST_WAIT_MS);
	id = request_measure_shed(fx);
	g_assert_true(wait_job_state(fx, id, "succeeded", LONG_WAIT_MS));
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);
	wait_log(w, reported, AXIS_TEST_WAIT_MS);
	g_assert_cmpuint(refuser.refused, ==, 2);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "the report was answered 503"));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "the report was answered 429"));
	g_assert_null(strstr(axis_test_worker_get_log(w), "refused the report"));
	finish(fx, w, 0);
}

/* ── Fencing ────────────────────────────────────────────────────── */

static void
test_fence_stopped(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *reported = NULL;
	AxisTestWorker *w;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_busy(fx, NULL, &id, &encoder);

	/* Stopped: it cannot renew; the daemon takes the job back (its one
	 * attempt is used: it fails). */
	axis_test_worker_signal(w, SIGSTOP);
	g_assert_true(wait_job_state(fx, id, "failed", AXIS_TEST_WAIT_MS));
	assert_job(fx, id, "last_error", "the lease of worker worker:garage expired: it stopped renewing it");

	/* Going on, it finds its deadline passed: the encoder is killed, and
	 * nothing is said about the job. */
	axis_test_worker_signal(w, SIGCONT);
	wait_log(w, "its encoder is stopped and what it made is dropped", AXIS_TEST_WAIT_MS);
	wait_gone(encoder);
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	axis_test_pump_for(500);
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);
	g_assert_null(strstr(axis_test_worker_get_log(w), reported));
	g_assert_null(strstr(axis_test_worker_get_log(w), "refused the report"));
	assert_job(fx, id, "state", "failed");
	stand_still(fx, FALSE);
	finish(fx, w, 0);
}

typedef struct
{
	Fixture *fx;
	gint64   id;
	gchar   *before;
} BeatWait;

static gboolean
heartbeat_handled(gpointer data)
{
	BeatWait *wait = data;
	g_autofree gchar *now = axis_test_db_queryf(wait->fx->db, "SELECT heartbeat_at::text FROM job WHERE id = %"
		G_GINT64_FORMAT, wait->id);

	return g_strcmp0(now, wait->before) != 0;
}

static void
test_fence_unanswered(Fixture *fx, gconstpointer data)
{
	AxisTestWorker *w;
	BeatWait wait;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_busy(fx, NULL, &id, &encoder);

	/* Just after the daemon renewed the lease (for 2000 ms from now, by
	 * its clock), it answers nothing more.  The worker's own deadline is
	 * at most 1500 ms after it sent that heartbeat (lease - max(lease/4,
	 * 250 ms)), so its encoder is gone before the daemon's lease ends,
	 * without a word from the daemon. */
	wait.fx = fx;
	wait.id = id;
	wait.before = axis_test_db_queryf(fx->db, "SELECT heartbeat_at::text FROM job WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(axis_test_pump_until(heartbeat_handled, &wait, AXIS_TEST_WAIT_MS));
	g_free(wait.before);
	g_usleep(1900 * 1000);
	g_assert_false(axis_test_process_alive(encoder));
	g_usleep(1600 * 1000);

	/* The daemon, going on, refuses what was late and takes the job back. */
	g_assert_true(wait_job_state(fx, id, "failed", AXIS_TEST_WAIT_MS));
	wait_log(w, "its encoder is stopped and what it made is dropped", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 0);
	stand_still(fx, FALSE);
	finish(fx, w, 0);
}

static void
test_cancelled(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reported = NULL;
	AxisJobCancelOutcome outcome;
	AxisTestWorker *w;
	AxisDbConn *conn;
	gint64 cancelled_at;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_busy(fx, NULL, &id, &encoder);
	conn = axis_db_pool_acquire(fx->pool, &error);
	cancelled_at = g_get_monotonic_time();
	g_assert_true(axis_job_cancel(conn, id, &outcome, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);

	/* Told to stop at its next heartbeat (every 500 ms here, with slack for
	 * a loaded machine): the encoder goes, nothing is reported. */
	wait_log(w, "the daemon said stop: it was cancelled", AXIS_TEST_WAIT_MS);
	g_assert_cmpint(g_get_monotonic_time() - cancelled_at, <, 1500 * 1000);
	wait_gone(encoder);
	g_assert_true(wait_job_state(fx, id, "cancelled", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);
	g_assert_null(strstr(axis_test_worker_get_log(w), reported));
	stand_still(fx, FALSE);
	finish(fx, w, 0);
}

/* ── Stopping ───────────────────────────────────────────────────── */

static void
test_sigterm(Fixture *fx, gconstpointer data)
{
	AxisTestWorker *w;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_busy(fx, NULL, &id, &encoder);
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, 0);
	g_assert_false(axis_test_process_alive(encoder));
	g_assert_true(axis_test_worker_scratch_empty(w));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "giving it back"));
	/* Given back: queued, no attempt used. */
	assert_job(fx, id, "state || ' ' || attempts", "queued 0");
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 0);
	g_assert_null(strstr(axis_test_worker_get_log(w), fx->token));
	axis_test_worker_free(w);
}

static void
test_sigkill_restart(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *scratch = NULL;
	g_autoptr(GDir) dir = NULL;
	AxisTestWorker *w;
	GPid encoder;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	scratch = g_build_filename(fx->dir, "scratch", NULL);
	w = start_busy(fx, scratch, &id, &encoder);
	axis_test_worker_signal(w, SIGKILL);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	/* Its encoder does not outlive it. */
	wait_gone(encoder);
	/* What it was doing is left in the scratch directory... */
	dir = g_dir_open(scratch, 0, NULL);
	g_assert_nonnull(dir);
	{
		const gchar *left = g_dir_read_name(dir);

		g_assert_nonnull(left);
		g_assert_true(g_str_has_prefix(left, "job-"));
	}
	g_clear_pointer(&dir, g_dir_close);
	axis_test_worker_free(w);
	/* ...the daemon takes the job back when the lease runs out... */
	g_assert_true(wait_job_state(fx, id, "failed", AXIS_TEST_WAIT_MS));

	/* ...and the next start removes it. */
	stand_still(fx, FALSE);
	w = start_worker(fx, NULL, NULL, scratch);
	wait_log(w, "removed what an earlier run left in the scratch directory", AXIS_TEST_WAIT_MS);
	wait_registered(w);
	g_assert_true(axis_test_worker_scratch_empty(w));
	finish(fx, w, 0);
}

static void
test_revoked(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestWorker *w;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	w = start_worker(fx, NULL, NULL, NULL);
	wait_registered(w);
	g_assert_true(axis_worker_store_revoke(fx->pool, fx->d->tokens, "garage", "root", NULL, &error));
	g_assert_no_error(error);
	/* Its next claim is refused: it stops, and says why. */
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, 1);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "refused this worker's token"));
	axis_test_worker_free(w);
}

/* ── Proxies ────────────────────────────────────────────────────── */

static gboolean
on_proxy_incoming(
	GSocketService    *service,
	GSocketConnection *connection,
	GObject           *source,
	gpointer           data
){
	(*(guint *)data)++;
	return FALSE;
}

static void
test_proxy_ignored(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketService) service = g_socket_service_new();
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, 0);
	g_autoptr(GSocketAddress) bound = NULL;
	g_autofree gchar *proxy = NULL;
	AxisTestWorker *w;
	guint connections = 0;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error));
	g_assert_no_error(error);
	g_signal_connect(service, "incoming", G_CALLBACK(on_proxy_incoming), &connections);
	g_socket_service_start(service);
	proxy = g_strdup_printf("http://127.0.0.1:%u", (guint)g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound)));
	{
		const gchar *const env[] = {
			"http_proxy", proxy, "https_proxy", proxy, "HTTP_PROXY", proxy, "HTTPS_PROXY", proxy,
			"all_proxy", proxy, "ALL_PROXY", proxy, "no_proxy", "", "NO_PROXY", "", NULL
		};

		w = start_worker(fx, NULL, env, NULL);
	}
	wait_registered(w);
	axis_test_pump_for(300);
	g_assert_cmpuint(connections, ==, 0);
	finish(fx, w, 0);
	g_socket_service_stop(service);
}

/* ── A hostile daemon ───────────────────────────────────────────── */

typedef struct
{
	gchar   *bearer;
	guint    registers;
	guint    claims;
	guint    fails;
	guint    requests;
	gboolean cookie_seen;
	gboolean bad_method;
	gboolean bad_path;
	gboolean bad_auth;
	GString *fail_bodies;
	gchar   *redirect;
} Hostile;

static const gchar claimed_traversal[] =
	"{\"protocol\":3,\"job\":{\"id\":41,\"kind\":\"estimate.sample.remote\","
	"\"lease_token\":\"0b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c\",\"lease_ms\":2000,\"heartbeat_ms\":500,\"attempt\":1,"
	"\"description\":{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,"
	"\"audio_bitrate\":0,\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,"
	"\"excerpt_timeout_ms\":60000,\"files\":[{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"Films\","
	"\"path\":\"../../../etc/passwd\",\"size\":1000,\"mtime_sec\":1,\"mtime_nsec\":0,\"stream\":0,\"quality\":4,"
	"\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}]}},"
	"\"paused\":null}";

static const gchar claimed_absolute[] =
	"{\"protocol\":3,\"job\":{\"id\":42,\"kind\":\"estimate.sample.remote\","
	"\"lease_token\":\"1b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c\",\"lease_ms\":2000,\"heartbeat_ms\":500,\"attempt\":1,"
	"\"description\":{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,"
	"\"audio_bitrate\":0,\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,"
	"\"excerpt_timeout_ms\":60000,\"files\":[{\"index\":0,\"volume\":\"" UUID_A "\",\"root\":\"/etc\","
	"\"path\":\"passwd\",\"size\":1000,\"mtime_sec\":1,\"mtime_nsec\":0,\"stream\":0,\"quality\":4,"
	"\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,\"sample_bits\":0,\"length_ms\":2000,\"starts_ms\":[0]}]}},"
	"\"paused\":null}";

/* Well formed, for a volume the worker has not proved. */
static const gchar claimed_unproved[] =
	"{\"protocol\":3,\"job\":{\"id\":43,\"kind\":\"estimate.sample.remote\","
	"\"lease_token\":\"2b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c\",\"lease_ms\":2000,\"heartbeat_ms\":500,\"attempt\":1,"
	"\"description\":{\"media\":\"video\",\"codec\":\"hevc\",\"family\":\"software\",\"target_quality\":4,"
	"\"audio_bitrate\":0,\"audio_bitrate_mono\":0,\"audio_quality\":-1,\"reduce_to_cd\":false,"
	"\"excerpt_timeout_ms\":60000,\"files\":[{\"index\":0,\"volume\":\"11111111-2222-4333-8444-555555555555\","
	"\"root\":\"Films\",\"path\":\"Film.mkv\",\"size\":1000,\"mtime_sec\":1,\"mtime_nsec\":0,\"stream\":0,"
	"\"quality\":4,\"bit_depth\":8,\"channels\":0,\"sample_rate\":0,\"sample_bits\":0,\"length_ms\":2000,"
	"\"starts_ms\":[0]}]}},\"paused\":null}";

static void
respond(
	SoupServerMessage *message,
	guint              status,
	const gchar       *body,
	gsize              length
){
	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, body, length);
}

static void
on_hostile(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           data
){
	Hostile *h = data;
	SoupMessageHeaders *request = soup_server_message_get_request_headers(message);
	SoupMessageHeaders *response = soup_server_message_get_response_headers(message);

	h->requests++;
	if (soup_message_headers_get_one(request, "Cookie") != NULL)
		h->cookie_seen = TRUE;
	if (strcmp(soup_server_message_get_method(message), "POST") != 0)
		h->bad_method = TRUE;
	if (!g_str_has_prefix(path, AXIS_WORKER_API_PREFIX))
		h->bad_path = TRUE;
	if (g_strcmp0(soup_message_headers_get_one(request, "Authorization"), h->bearer) != 0)
		h->bad_auth = TRUE;
	soup_message_headers_append(response, "Set-Cookie", "axis_session=hostile; Path=/");

	if (g_str_has_suffix(path, "/register")) {
		static const gchar short_lease[] = "{\"protocol\":3,\"worker\":\"garage\",\"lease_ms\":5,\"heartbeat_ms\":1,"
			"\"poll_ms\":100,\"volumes\":[]}";
		static const gchar fine[] = "{\"protocol\":3,\"worker\":\"garage\",\"lease_ms\":2000,\"heartbeat_ms\":500,"
			"\"poll_ms\":100,\"volumes\":[{\"uuid\":\"" UUID_A "\",\"accepted\":true,\"reason\":null}]}";

		h->registers++;
		if (h->registers == 1) {
			respond(message, 200, short_lease, strlen(short_lease));
		} else if (h->registers == 2) {
			soup_message_headers_replace(response, "Location", h->redirect);
			respond(message, 302, "{}", 2);
		} else if (h->registers == 3) {
			gsize size = 2 * 1024 * 1024;
			gchar *big = g_malloc(size);

			memset(big, ' ', size);
			big[0] = '{';
			big[size - 1] = '}';
			respond(message, 200, big, size);
			g_free(big);
		} else {
			respond(message, 200, fine, strlen(fine));
		}
	} else if (g_str_has_suffix(path, "/claim")) {
		static const gchar nothing[] = "{\"protocol\":3,\"job\":null,\"paused\":null}";

		h->claims++;
		if (h->claims == 1)
			respond(message, 200, claimed_traversal, strlen(claimed_traversal));
		else if (h->claims == 2)
			respond(message, 200, claimed_absolute, strlen(claimed_absolute));
		else if (h->claims == 3)
			respond(message, 200, claimed_unproved, strlen(claimed_unproved));
		else
			respond(message, 200, nothing, strlen(nothing));
	} else if (g_str_has_suffix(path, "/fail")) {
		static const gchar queued[] = "{\"protocol\":3,\"state\":\"queued\"}";
		SoupMessageBody *body = soup_server_message_get_request_body(message);
		g_autoptr(GBytes) bytes = soup_message_body_flatten(body);
		gsize length = 0;
		const gchar *text = g_bytes_get_data(bytes, &length);

		h->fails++;
		g_string_append_len(h->fail_bodies, text, length);
		respond(message, 200, queued, strlen(queued));
	} else {
		static const gchar none[] = "{\"protocol\":3,\"error\":\"not-found\",\"message\":\"no\"}";

		respond(message, 404, none, strlen(none));
	}
}

static gboolean
hostile_done(gpointer data)
{
	Hostile *h = data;

	return h->claims >= 5 && h->fails >= 1;
}

static void
test_hostile_daemon(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autofree gchar *url = NULL;
	AxisTestWorker *w;
	GSList *uris;
	Hostile h;
	guint port;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	memset(&h, 0, sizeof(h));
	h.bearer = g_strconcat("Bearer ", fx->token, NULL);
	h.fail_bodies = g_string_new(NULL);
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	port = (guint)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	url = g_strdup_printf("http://127.0.0.1:%u", port);
	h.redirect = g_strdup_printf("%s/elsewhere", url);
	soup_server_add_handler(server, NULL, on_hostile, &h, NULL);

	/* Anything it would run would stand still where the test can see it. */
	stand_still(fx, TRUE);
	w = start_worker(fx, url, NULL, NULL);
	g_assert_true(axis_test_pump_until(hostile_done, &h, 4 * AXIS_TEST_WAIT_MS));

	/* Every answer that was not acceptable was refused. */
	g_assert_cmpuint(h.registers, ==, 4);
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "the daemon's answer to register is not acceptable"));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "answered with a redirect (302), which is never followed"));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "answer is longer than 1048576 bytes"));
	g_assert_nonnull(strstr(axis_test_worker_get_log(w), "the daemon's answer to a claim is not acceptable"));
	/* The third job was well formed but not one it can run: given back. */
	g_assert_nonnull(strstr(h.fail_bodies->str, "\"job\":43"));
	g_assert_nonnull(strstr(h.fail_bodies->str, "has not proved"));
	g_assert_null(strstr(h.fail_bodies->str, "\"job\":41"));
	g_assert_null(strstr(h.fail_bodies->str, "\"job\":42"));
	/* It ran nothing, followed no redirect, sent no cookie, always its
	 * token, always POST to the worker API. */
	g_assert_cmpint(axis_test_find_child(axis_test_worker_get_pid(w), "sleep"), ==, 0);
	g_assert_null(strstr(axis_test_worker_get_log(w), "started"));
	g_assert_false(h.cookie_seen);
	g_assert_false(h.bad_method);
	g_assert_false(h.bad_path);
	g_assert_false(h.bad_auth);
	stand_still(fx, FALSE);
	finish(fx, w, 0);
	soup_server_disconnect(server);
	g_free(h.bearer);
	g_free(h.redirect);
	g_string_free(h.fail_bodies, TRUE);
}

/* ── A daemon of protocol 2 ─────────────────────────────────────── */

static void
on_old_daemon(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           data
){
	/* What a daemon of protocol 2 (before the check job) answers to every
	 * request of protocol 3. */
	static const gchar refused[] = "{\"protocol\":2,\"error\":\"malformed\",\"message\":\"the message is of another "
		"protocol version\"}";
	guint *registers = data;

	if (g_str_has_suffix(path, "/register"))
		(*registers)++;
	respond(message, 400, refused, strlen(refused));
}

static gboolean
registered_twice(gpointer data)
{
	return *(guint *)data >= 2;
}

/* Spec (worker-protocol): against an older daemon the worker says, in its
 * log, that the two speak different protocols and what to do; it takes no
 * work and keeps trying. */
static void
test_old_daemon(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autofree gchar *url = NULL;
	AxisTestWorker *w;
	GSList *uris;
	guint registers = 0;
	guint port;

	SKIP_WITHOUT_DB(fx);
	SKIP_WITHOUT_FFMPEG();
	g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(server);
	port = (guint)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	url = g_strdup_printf("http://127.0.0.1:%u", port);
	soup_server_add_handler(server, NULL, on_old_daemon, &registers, NULL);

	w = start_worker(fx, url, NULL, NULL);
	g_assert_true(axis_test_pump_until(registered_twice, &registers, AXIS_TEST_WAIT_MS));
	wait_log(w, "the daemon speaks worker protocol 2 and this axis-worker 3: it gives this worker no work; update "
		"the daemon, or run the axis-worker of its version (it said: the message is of another protocol version)",
		AXIS_TEST_WAIT_MS);
	g_assert_null(strstr(axis_test_worker_get_log(w), "registered as"));
	g_assert_null(strstr(axis_test_worker_get_log(w), "started"));
	finish(fx, w, 0);
	soup_server_disconnect(server);
}

#define ADD(name, func) g_test_add("/worker/process/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = axis_test_worker_seed_caps(FALSE);
	ADD("config", test_config);
	ADD("register-measure", test_register_measure);
	ADD("report-tried-again", test_report_tried_again);
	ADD("heartbeat-tried-again", test_heartbeat_tried_again);
	ADD("fence-stopped", test_fence_stopped);
	ADD("fence-unanswered", test_fence_unanswered);
	ADD("cancelled", test_cancelled);
	ADD("sigterm", test_sigterm);
	ADD("sigkill-restart", test_sigkill_restart);
	ADD("revoked", test_revoked);
	ADD("proxy-ignored", test_proxy_ignored);
	ADD("hostile-daemon", test_hostile_daemon);
	ADD("mount-without-identity", test_mount_without_identity);
	ADD("old-daemon", test_old_daemon);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
