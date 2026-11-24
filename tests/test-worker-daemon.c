/*
 * test-worker-daemon.c - a real axis-worker against a real axisd: the gate
 * as axisd serves it, registration with a volume proved, one with another
 * disk's marker and one with none; a capability measurement for real;
 * fencing when the worker was stopped past its lease; SIGTERM giving the
 * work back; neither program ever logging the token
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * axisd runs with a 2 s worker lease on 127.0.0.1 and a database of the
 * test's own.  The worker runs software encoders only; its long encodes are
 * the stand-in ffmpeg of axis-test-worker.h.  No media is read.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "web/axis-password.h"

#ifndef AXISD_PATH
#error "AXISD_PATH must be defined by the build"
#endif

#define UUID_A     "2c3d4e5f-6a7b-4c8d-9e0f-1a2b3c4d5e6f"
#define UUID_OTHER "8f7e6d5c-4b3a-4291-8f7e-6d5c4b3a2918"
#define LONG_WAIT_MS 120000

static gboolean have_ffmpeg;

static guint16
pick_free_port(void)
{
	return axis_test_free_port();
}

static guint
send_to(
	SoupSession  *session,
	const gchar  *method,
	const gchar  *url,
	const gchar  *bearer,
	const gchar  *body
){
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	g_autoptr(GBytes) answer = NULL;
	g_autoptr(GError) error = NULL;

	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (bearer != NULL)
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", bearer);
	if (body != NULL) {
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	answer = soup_session_send_and_read(session, message, NULL, &error);
	g_assert_no_error(error);
	return soup_message_get_status(message);
}

static void
wait_log(
	AxisTestWorker *p,
	const gchar    *needle,
	guint           timeout_ms
){
	if (!axis_test_worker_wait_log(p, needle, timeout_ms))
		g_error("'%s' was not logged in time; the log:\n%s", needle, axis_test_worker_get_log(p));
}

static gboolean
wait_job_state(
	AxisTestDb  *db,
	gint64       id,
	const gchar *state,
	guint        timeout_ms
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_wait_query(db, sql, state, timeout_ms);
}

static gint64
request_measure(AxisDbPool *pool)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_worker_store_request_measure(pool, "garage", &id, NULL, &error));
	g_assert_no_error(error);
	return id;
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

/* The worker in the encoder of capability job @id. */
static GPid
wait_encoder(
	AxisTestWorker *w,
	gint64          id
){
	g_autofree gchar *started = g_strdup_printf("job %" G_GINT64_FORMAT " (worker.capabilities), attempt 1: started", id);
	ChildWait wait;

	wait_log(w, started, AXIS_TEST_WAIT_MS);
	wait.parent = axis_test_worker_get_pid(w);
	wait.found = 0;
	g_assert_true(axis_test_pump_until(child_there, &wait, AXIS_TEST_WAIT_MS));
	return wait.found;
}

static void
test_end_to_end(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *right = NULL;
	g_autofree gchar *wrong = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *flag = NULL;
	g_autofree gchar *slow = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *port_text = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *bearer = NULL;
	g_autofree gchar *line = NULL;
	gchar *token = NULL;
	AxisTestDb *db;
	AxisDbPool *pool;
	AxisTestWorker *axisd;
	AxisTestWorker *w;
	AxisTestWorkerOptions options;
	GPid encoder;
	gint64 id;

	if (!have_ffmpeg) {
		g_test_skip("ffmpeg is not installed");
		return;
	}
	db = axis_test_db_new();
	if (db == NULL)
		return;
	axis_test_log_init();
	axis_test_log_reset();
	pool = axis_test_jobs_pool(db, 4);

	/* A worker made as the Workers page makes one: only its token's hash
	 * is stored. */
	g_assert_true(axis_password_init());
	token = axis_random_token();
	hash = axis_token_hash(token);
	bearer = g_strconcat("Bearer ", token, NULL);
	dir = axis_test_storage_dir_new();
	right = g_build_filename(dir, "right", NULL);
	wrong = g_build_filename(dir, "wrong", NULL);
	none = g_build_filename(dir, "none", NULL);
	axis_test_storage_mkdir(right);
	axis_test_storage_mkdir(wrong);
	axis_test_storage_mkdir(none);
	axis_test_storage_write_marker(right, UUID_A);
	axis_test_storage_write_marker(wrong, UUID_OTHER);
	sql = g_strdup_printf("INSERT INTO worker (name, token_hash, created_by) VALUES ('garage', '%s', 'test');"
		" INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at)"
		" VALUES ('" UUID_A "', 'Disk', '%s', false, 'online', now())", hash, right);
	axis_test_db_exec(db, sql);
	flag = g_build_filename(dir, "stand-still", NULL);
	slow = axis_test_slow_ffmpeg(dir, flag);

	/* axisd, on 127.0.0.1 only. */
	port_text = g_strdup_printf("%u", (guint)pick_free_port());
	url = g_strdup_printf("http://127.0.0.1:%s", port_text);
	{
		const gchar *const argv[] = { AXISD_PATH, NULL };
		g_autofree gchar *data_dir = g_build_filename(dir, "axisd-data", NULL);
		const gchar *const env[] = {
			"AXIS_LISTEN_HOST", "127.0.0.1", "AXIS_LISTEN_PORT", port_text,
			"AXIS_DATABASE_URL", axis_test_db_get_url(db), "AXIS_WORKER_LEASE", "2",
			"AXIS_DATA_DIR", data_dir, NULL
		};

		axisd = axis_test_process_spawn(argv, env);
	}
	line = g_strdup_printf("listening on 127.0.0.1:%s", port_text);
	wait_log(axisd, line, AXIS_TEST_WAIT_MS);

	/* Its gate: only a worker's token, only POST, only the worker API. */
	session = soup_session_new_with_options("timeout", 10u, NULL);
	{
		g_autofree gchar *claim = g_strdup_printf("%s%sclaim", url, AXIS_WORKER_API_PREFIX);
		g_autofree gchar *workers = g_strdup_printf("%s/workers", url);
		g_autofree gchar *body = axis_test_claim_json(NULL);

		g_assert_cmpuint(send_to(session, "POST", claim, NULL, body), ==, 401);
		g_assert_cmpuint(send_to(session, "POST", claim, "Bearer abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ", body), ==,
			401);
		g_assert_cmpuint(send_to(session, "GET", claim, bearer, NULL), ==, 405);
		g_assert_cmpuint(send_to(session, "POST", claim, bearer, body), ==, 409);
		g_assert_cmpuint(send_to(session, "GET", workers, bearer, NULL), !=, 200);
	}

	/* The worker: one volume proved and accepted, one with another disk's
	 * marker (not a volume the daemon has), one with no marker. */
	{
		const gchar *volumes[4];

		volumes[0] = right;
		volumes[1] = wrong;
		volumes[2] = none;
		volumes[3] = NULL;
		memset(&options, 0, sizeof(options));
		options.url = url;
		options.token = token;
		options.volumes = volumes;
		options.ffmpeg = slow;
		w = axis_test_worker_start(&options);
	}
	wait_log(w, "registered as garage: lease 2000 ms, heartbeat 500 ms, 1 volume(s) accepted", AXIS_TEST_WAIT_MS);
	{
		g_autofree gchar *accepted = g_strdup_printf("volume " UUID_A " at %s: proved and accepted", right);
		g_autofree gchar *other = g_strdup_printf("volume " UUID_OTHER " at %s: the daemon did not accept it", wrong);
		g_autofree gchar *unproved = g_strdup_printf("the volume path %s cannot be proved", none);
		g_autofree gchar *path = axis_test_db_query(db, "SELECT string_agg(path, ',') FROM worker_volume");

		g_assert_nonnull(strstr(axis_test_worker_get_log(w), accepted));
		g_assert_nonnull(strstr(axis_test_worker_get_log(w), other));
		g_assert_nonnull(strstr(axis_test_worker_get_log(w), unproved));
		g_assert_cmpstr(path, ==, right);
	}

	/* Its encoders measured for real, through axisd. */
	id = request_measure(pool);
	if (!wait_job_state(db, id, "succeeded", LONG_WAIT_MS))
		g_error("the measurement did not succeed; the worker's log:\n%s", axis_test_worker_get_log(w));
	{
		g_autofree gchar *tool = axis_test_db_query(db, "SELECT count(*) FROM encoder_tool WHERE machine = 'worker-garage'");

		g_assert_cmpstr(tool, ==, "1");
	}

	/* Stopped past its lease: axisd takes the job back, and the worker,
	 * going on, kills its encoder and says nothing more of the job. */
	axis_test_storage_write(flag, "");
	id = request_measure(pool);
	encoder = wait_encoder(w, id);
	axis_test_worker_signal(w, SIGSTOP);
	g_assert_true(wait_job_state(db, id, "failed", AXIS_TEST_WAIT_MS));
	axis_test_worker_signal(w, SIGCONT);
	wait_log(w, "its encoder is stopped and what it made is dropped", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(process_gone, &encoder, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(scratch_empty, w, AXIS_TEST_WAIT_MS));
	{
		g_autofree gchar *reported = g_strdup_printf("job %" G_GINT64_FORMAT ": reported", id);

		g_assert_null(strstr(axis_test_worker_get_log(w), reported));
	}

	/* SIGTERM: the job given back unspent, the encoder gone, a clean exit. */
	id = request_measure(pool);
	encoder = wait_encoder(w, id);
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(w), ==, 0);
	g_assert_false(axis_test_process_alive(encoder));
	g_assert_true(axis_test_worker_scratch_empty(w));
	{
		g_autofree gchar *state = axis_test_db_queryf(db, "SELECT state || ' ' || attempts FROM job WHERE id = %"
			G_GINT64_FORMAT, id);

		g_assert_cmpstr(state, ==, "queued 0");
	}
	g_assert_null(strstr(axis_test_worker_get_log(w), token));
	axis_test_worker_free(w);

	axis_test_worker_signal(axisd, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(axisd, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_worker_get_status(axisd), ==, 0);
	g_assert_null(strstr(axis_test_worker_get_log(axisd), token));
	g_assert_null(strstr(axis_test_worker_get_log(axisd), hash));
	g_assert_nonnull(strstr(axis_test_worker_get_log(axisd), "worker garage registered from 127.0.0.1"));
	axis_test_worker_free(axisd);

	g_object_unref(session);
	session = NULL;
	axis_db_pool_free(pool);
	axis_test_db_free(db);
	axis_test_storage_dir_free(g_steal_pointer(&dir));
	axis_secret_free(token);
	axis_test_log_reset();
}

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = axis_test_worker_seed_caps(FALSE);
	g_test_add_func("/worker/daemon/end-to-end", test_end_to_end);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
