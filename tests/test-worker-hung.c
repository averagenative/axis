/*
 * test-worker-hung.c - a volume that hangs: real axis-worker processes against
 * a stand-in daemon on the test's main loop, with one volume's proof or check
 * held in the kernel by the debug build's hang (AXIS_WORKER_TEST_HANG_VOLUME
 * and AXIS_WORKER_TEST_HANG_FIFO, a FIFO the test holds open)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What was wrong: the worker proved its volumes on its start-up thread before
 * it registered, so a mount whose server was gone (stat() never returns) kept
 * the worker from registering at all; and its volume checks ran on one
 * thread, one volume after the other, so one volume that hung held back the
 * checks of the others, for as long as it hung.
 *
 * Here the hang stands in for the dead mount: the worker must register
 * without the hung volume and prove it when it answers; and with one volume
 * hung, a disk that goes away and comes back must still be noticed and
 * proved again, with no more than one thread stuck on the hung volume.
 *
 * No database, no jobs, no media; ffmpeg is the real one but only says its
 * version.  The hang exists in debug builds only, so the tests skip in a
 * release build.
 */

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"

#define UUID_A        "6c2d8e4f-1a3b-4c5d-9e7f-0a1b2c3d4e5f"
#define UUID_B        "7d3e9f50-2b4c-4d6e-8f80-1b2c3d4e5f60"
#define TOKEN         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ"
#define LEASE_MS      2000
#define BEAT_MS       500
#define POLL_MS       200

#ifdef AXIS_DEBUG
#define HAVE_HANG TRUE
#else
#define HAVE_HANG FALSE
#endif

static gboolean have_ffmpeg;

/* Nothing is made before this: a test that skips touches nothing. */
#define SKIP_UNLESS_READY() do { \
	if (!HAVE_HANG) { \
		g_test_skip("the worker's hang for tests is in debug builds only (make check)"); \
		return; \
	} \
	if (!have_ffmpeg) { \
		g_test_skip("ffmpeg is not installed"); \
		return; \
	} \
} while (0)

typedef struct
{
	GPtrArray *registers;	/* gchar *: the body of each register request */
	guint      claims;
} Daemon;

typedef struct
{
	gchar       *dir;
	gchar       *disk_a;
	gchar       *disk_b;
	gchar       *away_b;	/* where disk B is while it is gone */
	gchar       *hang;		/* the FIFO's path */
	gint         hang_fd;	/* its other end, held by the test (-1: no FIFO) */
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
	g_autoptr(GBytes) bytes = soup_message_body_flatten(soup_server_message_get_request_body(message));
	gsize length = 0;
	const gchar *text = g_bytes_get_data(bytes, &length);
	g_autofree gchar *body = g_strndup(text != NULL ? text : "", length);

	if (g_str_has_suffix(path, "/register")) {
		g_autoptr(AxisWorkerRegistered) registered = axis_worker_registered_new();

		g_ptr_array_add(d->registers, g_strdup(body));
		registered->worker = g_strdup("garage");
		registered->lease_ms = LEASE_MS;
		registered->heartbeat_ms = BEAT_MS;
		registered->poll_ms = POLL_MS;
		if (strstr(body, UUID_A) != NULL)
			axis_worker_registered_add_volume(registered, UUID_A, TRUE, NULL);
		if (strstr(body, UUID_B) != NULL)
			axis_worker_registered_add_volume(registered, UUID_B, TRUE, NULL);
		answer(message, 200, axis_worker_registered_to_json(registered));
	} else if (g_str_has_suffix(path, "/claim")) {
		d->claims++;
		answer(message, 200, axis_worker_claimed_to_json(NULL, NULL));
	} else {
		answer(message, 404, axis_worker_error_to_json("not-found", "no such request"));
	}
}

/* ── The fixture ────────────────────────────────────────────────── */

static Fixture *
fixture_new(void)
{
	g_autoptr(GError) error = NULL;
	Fixture *fx = g_new0(Fixture, 1);
	GSList *uris;

	axis_test_log_init();
	axis_test_log_reset();
	fx->dir = axis_test_storage_dir_new();
	fx->disk_a = g_build_filename(fx->dir, "disk-a", NULL);
	fx->disk_b = g_build_filename(fx->dir, "disk-b", NULL);
	fx->away_b = g_build_filename(fx->dir, "disk-b-gone", NULL);
	axis_test_storage_mkdir(fx->disk_a);
	axis_test_storage_mkdir(fx->disk_b);
	axis_test_storage_write_marker(fx->disk_a, UUID_A);
	axis_test_storage_write_marker(fx->disk_b, UUID_B);
	fx->hang = g_build_filename(fx->dir, "hang", NULL);
	fx->hang_fd = -1;
	fx->d.registers = g_ptr_array_new_with_free_func(g_free);
	fx->server = soup_server_new(NULL, NULL);
	g_assert_true(soup_server_listen_local(fx->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	uris = soup_server_get_uris(fx->server);
	fx->url = g_strdup_printf("http://127.0.0.1:%u", (guint)g_uri_get_port(uris->data));
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	soup_server_add_handler(fx->server, NULL, on_request, fx, NULL);
	return fx;
}

/* Disk A hangs from now on: a FIFO that the test holds open, which a thread
 * that opens it to read blocks on until the test lets go. */
static void
start_hanging(Fixture *fx)
{
	g_assert_cmpint(mkfifo(fx->hang, 0600), ==, 0);
	fx->hang_fd = open(fx->hang, O_RDWR | O_CLOEXEC);
	g_assert_cmpint(fx->hang_fd, >=, 0);
}

/* The thread blocked on the FIFO returns: the FIFO goes first, so that no
 * later check hangs, then the test's end of it. */
static void
stop_hanging(Fixture *fx)
{
	g_assert_cmpint(g_unlink(fx->hang), ==, 0);
	g_assert_cmpint(close(fx->hang_fd), ==, 0);
	fx->hang_fd = -1;
}

static void
fixture_free(Fixture *fx)
{
	soup_server_disconnect(fx->server);
	g_object_unref(fx->server);
	if (fx->hang_fd >= 0)
		close(fx->hang_fd);
	if (g_file_test(fx->away_b, G_FILE_TEST_IS_DIR))
		g_assert_cmpint(g_rename(fx->away_b, fx->disk_b), ==, 0);
	axis_test_storage_dir_free(fx->dir);
	g_ptr_array_unref(fx->d.registers);
	g_free(fx->disk_a);
	g_free(fx->disk_b);
	g_free(fx->away_b);
	g_free(fx->hang);
	g_free(fx->url);
	g_free(fx);
	axis_test_log_reset();
}

/* ── The worker ─────────────────────────────────────────────────── */

/* A worker with both disks, and disk A's proofs and checks held by the FIFO
 * whenever it exists. */
static AxisTestWorker *
start_worker(Fixture *fx)
{
	const gchar *volumes[3];
	const gchar *env[5];
	AxisTestWorkerOptions options;

	volumes[0] = fx->disk_a;
	volumes[1] = fx->disk_b;
	volumes[2] = NULL;
	env[0] = "AXIS_WORKER_TEST_HANG_VOLUME";
	env[1] = fx->disk_a;
	env[2] = "AXIS_WORKER_TEST_HANG_FIFO";
	env[3] = fx->hang;
	env[4] = NULL;
	memset(&options, 0, sizeof(options));
	options.url = fx->url;
	options.token = TOKEN;
	options.volumes = volumes;
	options.env = env;
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
assert_running(AxisTestWorker *w)
{
	if (!axis_test_process_alive(axis_test_worker_get_pid(w)) || strstr(axis_test_worker_get_log(w), "Sanitizer") != NULL)
		g_error("the worker is gone; its log:\n%s", axis_test_worker_get_log(w));
}

/* How many times @needle is in the worker's log. */
static guint
count_log(
	AxisTestWorker *w,
	const gchar    *needle
){
	const gchar *at = axis_test_worker_get_log(w);
	guint n = 0;

	while ((at = strstr(at, needle)) != NULL) {
		n++;
		at += strlen(needle);
	}
	return n;
}

static void
finish(AxisTestWorker *w)
{
	axis_test_worker_signal(w, SIGTERM);
	g_assert_true(axis_test_worker_wait_exit(w, AXIS_TEST_WAIT_MS));
	if (axis_test_worker_get_status(w) != 0)
		g_error("the worker exited %d, not 0; its log:\n%s", axis_test_worker_get_status(w), axis_test_worker_get_log(w));
	g_assert_null(strstr(axis_test_worker_get_log(w), TOKEN));
	axis_test_worker_free(w);
}

/* The last register request's body names @uuid. */
static gboolean
last_register_has(
	Fixture     *fx,
	const gchar *uuid
){
	return fx->d.registers->len > 0 &&
		strstr(g_ptr_array_index(fx->d.registers, fx->d.registers->len - 1), uuid) != NULL;
}

static gboolean
register_lists_a(gpointer data)
{
	return last_register_has(data, UUID_A);
}

static gboolean
register_lists_b(gpointer data)
{
	return last_register_has(data, UUID_B);
}

static gboolean
register_lacks_b(gpointer data)
{
	Fixture *fx = data;

	return fx->d.registers->len > 0 && !last_register_has(fx, UUID_B);
}

static gboolean
claims_started(gpointer data)
{
	return ((Fixture *)data)->d.claims > 0;
}

/* ── The tests ──────────────────────────────────────────────────── */

/* A volume that hangs when the worker starts does not hold back its
 * registration: it registers with the other volume, says why the hung one is
 * not proven, takes work, and registers the hung one when it answers. */
static void
test_start_hung(void)
{
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	start_hanging(fx);
	w = start_worker(fx);

	/* It registers (a short wait for the first proofs, no more) and takes
	 * work, with the volume that answers only. */
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	wait_log(w, "has not answered in", AXIS_TEST_WAIT_MS);
	g_assert_cmpuint(fx->d.registers->len, ==, 1);
	g_assert_true(last_register_has(fx, UUID_B));
	g_assert_false(last_register_has(fx, UUID_A));
	g_assert_true(axis_test_pump_until(claims_started, fx, AXIS_TEST_WAIT_MS));
	assert_running(w);

	/* It is still hung much later: one thread on it, no more, and the
	 * worker goes on. */
	axis_test_pump_for(1500);
	g_assert_cmpuint(count_log(w, "does not answer (a test)"), ==, 1);
	wait_log(w, "did not answer within", AXIS_TEST_WAIT_MS * 2);
	assert_running(w);

	/* The mount comes back: the volume is proved and registered. */
	stop_hanging(fx);
	wait_log(w, "answers again", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(register_lists_a, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(last_register_has(fx, UUID_B));
	assert_running(w);
	finish(w);
	fixture_free(fx);
}

/* With one volume hung, the others are still checked: a disk that goes away
 * is noticed, one that comes back is proved again, and the hung volume has
 * one thread stuck, however long it hangs. */
static void
test_check_hung(void)
{
	g_autofree gchar *lost = NULL;
	AxisTestWorker *w;
	Fixture *fx;

	SKIP_UNLESS_READY();
	fx = fixture_new();
	w = start_worker(fx);
	wait_log(w, "registered as garage", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(register_lists_a, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(last_register_has(fx, UUID_B));

	/* Disk A stops answering. */
	start_hanging(fx);
	wait_log(w, "does not answer (a test)", AXIS_TEST_WAIT_MS);

	/* Disk B goes away: the worker notices, though A's check is stuck. */
	g_assert_cmpint(g_rename(fx->disk_b, fx->away_b), ==, 0);
	lost = g_strdup_printf("volume " UUID_B " at %s is not the one proved any more", fx->disk_b);
	wait_log(w, lost, 3000);
	g_assert_true(axis_test_pump_until(register_lacks_b, fx, AXIS_TEST_WAIT_MS));

	/* It comes back: proved again and registered again. */
	g_assert_cmpint(g_rename(fx->away_b, fx->disk_b), ==, 0);
	g_assert_true(axis_test_pump_until(register_lists_b, fx, 3000));

	/* A is dropped after the limit, with one stuck thread however many
	 * checks came due meanwhile. */
	wait_log(w, "did not answer within", AXIS_TEST_WAIT_MS * 2);
	axis_test_pump_for(1000);
	g_assert_cmpuint(count_log(w, "does not answer (a test)"), ==, 1);
	assert_running(w);

	stop_hanging(fx);
	wait_log(w, "answers again", AXIS_TEST_WAIT_MS);
	g_assert_true(axis_test_pump_until(register_lists_a, fx, AXIS_TEST_WAIT_MS));
	finish(w);
	fixture_free(fx);
}

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	have_ffmpeg = HAVE_HANG && axis_test_worker_seed_caps(FALSE);
	g_test_add_func("/worker/hung/start", test_start_hung);
	g_test_add_func("/worker/hung/check", test_check_hung);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
