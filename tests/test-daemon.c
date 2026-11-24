/*
 * test-daemon.c - runs the real axisd binary (AXISD_PATH)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every child is killed by the kernel if this test process dies
 * (PR_SET_PDEATHSIG), is force-killed in teardown, and every wait has a
 * timeout, so a failing test cannot leave axisd running or hang.
 *
 * A daemon that gets as far as listening needs a database: each such test
 * makes its own (axis-test-db.h) and points AXIS_DATABASE_URL at it.
 */

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-storage.h"
#include "axis-version.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "web/axis-web-health.h"

#ifndef AXISD_PATH
#error "AXISD_PATH must be defined by the build"
#endif
#ifndef AXIS_MIGRATION_DIR
#error "AXIS_MIGRATION_DIR must be defined by the build"
#endif

/* How long a request is retried while the daemon says it is starting: a
 * sanitized build on a busy machine, through the test's proxy, takes
 * seconds to migrate, and the start takes the database lock too. */
#define STARTUP_TIMEOUT_MS 20000
#define EXIT_TIMEOUT_SECONDS 10
#define SECRET "hunter2pw"

typedef struct _Proxy Proxy;

static void proxy_free(Proxy *proxy);

typedef struct _Fixture Fixture;

static gchar *read_stderr_until(Fixture *fx, const gchar *needle);

struct _Fixture
{
	GSubprocess *proc;
	GMainLoop   *loop;
	gboolean     timed_out;
	gchar       *stderr_text;
	GSocket     *held;		/* a socket kept open by a test */
	gchar       *tmp_path;	/* a file to remove at teardown */
	AxisTestDb  *db;		/* the test's database, if it has one */
	Proxy       *proxy;		/* between axisd and the database, if any */
	gchar       *stderr_path;	/* the file the daemon's stderr goes to (inside @scratch) */
	gsize        stderr_seen;	/* how much of it read_stderr_until() has gone through */
	guint        n_spawned;
	gchar       *scratch;	/* a directory of the test's own */
	gchar       *data_dir;	/* AXIS_DATA_DIR of every daemon started (inside @scratch) */
};

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	fx->scratch = axis_test_storage_dir_new();
	fx->data_dir = g_build_filename(fx->scratch, "data", NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->proc != NULL) {
		g_subprocess_force_exit(fx->proc);
		g_subprocess_wait(fx->proc, NULL, NULL);
		g_object_unref(fx->proc);
	}
	if (fx->held != NULL) {
		g_socket_close(fx->held, NULL);
		g_object_unref(fx->held);
	}
	if (fx->tmp_path != NULL) {
		g_unlink(fx->tmp_path);
		g_free(fx->tmp_path);
	}
	if (fx->proxy != NULL)
		proxy_free(fx->proxy);
	axis_test_db_free(fx->db);
	g_free(fx->stderr_text);
	g_free(fx->stderr_path);
	g_free(fx->data_dir);
	axis_test_storage_dir_free(fx->scratch);
}

static void
child_setup(gpointer user_data)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

/* Binds 127.0.0.1:0 and returns the socket (still bound, not listening). */
static GSocket *
bind_ephemeral(void)
{
	GError *error = NULL;
	GSocket *socket;
	GSocketAddress *address;

	socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT, &error);
	g_assert_no_error(error);
	address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_socket_bind(socket, address, FALSE, &error);
	g_assert_no_error(error);
	g_object_unref(address);

	return socket;
}

static guint16
socket_port(GSocket *socket)
{
	GError *error = NULL;
	GSocketAddress *address = g_socket_get_local_address(socket, &error);
	guint16 port;

	g_assert_no_error(error);
	port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(address));
	g_object_unref(address);

	return port;
}

/*
 * A port for a daemon this test starts: free now, and not one another
 * program's own pick of a free port (a sibling test's daemon, run at the
 * same time) is likely to get (axis_test_free_port()).  The daemon binds it
 * a moment later; the requests below check that what answers is that
 * daemon (AXIS_WEB_HEALTH_PROCESS_HEADER).
 */
static guint16
pick_free_port(void)
{
	return axis_test_free_port();
}

/* What a test must not inherit from the environment it runs in. */
static const gchar *const clean[] = {
	"AXIS_LISTEN_HOST", "AXIS_LISTEN_PORT", "AXIS_DATABASE_URL", "AXIS_DATABASE_URL_FILE",
	"AXIS_DATABASE_POOL_SIZE", "AXIS_DATABASE_WAIT", "AXIS_LOG_LEVEL", "AXIS_LOG_FORMAT",
	"AXIS_JOBS_CONCURRENCY", "AXIS_JOBS_GRACE_PERIOD", "AXIS_TRUSTED_PROXIES", "AXIS_SECURE_COOKIES",
	"AXIS_WORKER_LEASE", "AXIS_DATA_DIR", "AXIS_ADMIN_USERNAME", "AXIS_ADMIN_PASSWORD",
	"AXIS_ADMIN_PASSWORD_FILE", "AXIS_METRICS_TOKEN", "AXIS_METRICS_TOKEN_FILE", "AXIS_HEAD_TIMEOUT",
	"AXIS_BODY_TIMEOUT", "AXIS_IDLE_TIMEOUT", "AXIS_MAX_CONNECTIONS", "AXIS_MAX_CONNECTIONS_PER_ADDRESS", NULL
};

/* The process id of the daemon the test started last: what answers a
 * probe must be it. */
static gchar *expected_process;

/*
 * Starts axisd with a clean AXIS_* environment, then the fixture's data
 * directory, @port (unless 0), @database_url (unless NULL), @env (NULL or
 * name/value pairs ending in NULL, which may name another data directory)
 * and up to two arguments.
 */
static void
spawn_axisd_env(
	Fixture             *fx,
	guint16              port,
	const gchar         *database_url,
	const gchar * const *env,
	const gchar         *extra_arg1,
	const gchar         *extra_arg2
){
	GSubprocessLauncher *launcher;
	GError *error = NULL;
	const gchar *argv[5];
	guint n = 0;
	guint i;

	/* Its stderr goes to a file, not to a pipe: a pipe holds only so much
	 * (64 KiB, and two pages once the user's programs together have many
	 * pipes open, as when the test programs run side by side), and a daemon
	 * whose log filled it would stand still in write() until the test read
	 * it, which it does when the daemon has ended. */
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	g_free(fx->stderr_path);
	fx->stderr_path = g_strdup_printf("%s/axisd-%u.log", fx->scratch, fx->n_spawned++);
	fx->stderr_seen = 0;
	g_subprocess_launcher_set_stderr_file_path(launcher, fx->stderr_path);
	for (i = 0; clean[i] != NULL; i++)
		g_subprocess_launcher_unsetenv(launcher, clean[i]);
	g_subprocess_launcher_setenv(launcher, "AXIS_DATA_DIR", fx->data_dir, TRUE);
	if (port != 0) {
		gchar *port_text = g_strdup_printf("%u", (guint)port);

		g_subprocess_launcher_setenv(launcher, "AXIS_LISTEN_PORT", port_text, TRUE);
		g_free(port_text);
	}
	if (database_url != NULL)
		g_subprocess_launcher_setenv(launcher, "AXIS_DATABASE_URL", database_url, TRUE);
	for (i = 0; env != NULL && env[i] != NULL; i += 2)
		g_subprocess_launcher_setenv(launcher, env[i], env[i + 1], TRUE);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);

	argv[n++] = AXISD_PATH;
	if (extra_arg1 != NULL)
		argv[n++] = extra_arg1;
	if (extra_arg2 != NULL)
		argv[n++] = extra_arg2;
	argv[n] = NULL;

	fx->proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_object_unref(launcher);
	g_free(expected_process);
	expected_process = g_strdup(g_subprocess_get_identifier(fx->proc));
}

static void
spawn_axisd(
	Fixture      *fx,
	guint16       port,
	const gchar  *extra_arg1,
	const gchar  *extra_arg2
){
	spawn_axisd_env(fx, port, NULL, NULL, extra_arg1, extra_arg2);
}

/* Makes the test's database and starts axisd on it.  Returns FALSE when
 * the test is skipped (no database tests). */
static gboolean
spawn_axisd_with_db(
	Fixture *fx,
	guint16  port
){
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return FALSE;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	return TRUE;
}

static gboolean
on_wait_timeout(gpointer user_data)
{
	Fixture *fx = user_data;

	fx->timed_out = TRUE;
	g_subprocess_force_exit(fx->proc);
	return G_SOURCE_REMOVE;
}

/* The child ended: all it wrote to stderr is in its file. */
static void
on_child_exited(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Fixture *fx = user_data;

	g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	g_free(fx->stderr_text);
	fx->stderr_text = NULL;
	if (!g_file_get_contents(fx->stderr_path, &fx->stderr_text, NULL, NULL))
		fx->stderr_text = g_strdup("");
	g_main_loop_quit(fx->loop);
}

/* Waits for the child to exit while collecting its stderr.  Fails the test
 * (after killing the child) if it does not exit in time. */
static void
wait_for_exit(Fixture *fx)
{
	guint timeout_id;

	fx->loop = g_main_loop_new(NULL, FALSE);
	g_subprocess_wait_async(fx->proc, NULL, on_child_exited, fx);
	timeout_id = g_timeout_add_seconds(EXIT_TIMEOUT_SECONDS, on_wait_timeout, fx);
	g_main_loop_run(fx->loop);
	if (!fx->timed_out)
		g_source_remove(timeout_id);
	g_main_loop_unref(fx->loop);
	fx->loop = NULL;

	if (fx->timed_out)
		g_test_fail_printf("axisd did not exit within %d s; stderr: %s",
			EXIT_TIMEOUT_SECONDS, fx->stderr_text);
	g_assert_false(fx->timed_out);

	/* A sanitizer report in the child must fail the test even when the
	 * exit status is one the test expects to be non-zero anyway. */
	g_assert_null(strstr(fx->stderr_text, "Sanitizer"));
	g_assert_null(strstr(fx->stderr_text, "runtime error"));
	/* Nor may any message ever show the database password. */
	if (fx->db != NULL && strstr(fx->stderr_text, axis_test_db_get_password(fx->db)) != NULL)
		g_error("axisd logged the database password: %s", fx->stderr_text);
}

static void
stop_and_expect_zero(Fixture *fx)
{
	g_subprocess_send_signal(fx->proc, SIGTERM);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), ==, 0);
}

/* Whether @body is what the start-up gate answers while the daemon waits
 * for its database (a probe's "starting", or the 503 page). */
static gboolean
body_is_starting(const gchar *body)
{
	return body != NULL && (strstr(body, "{\"status\":\"starting\"}") != NULL ||
		g_str_has_prefix(body, "Axis is starting"));
}

/*
 * GET http://127.0.0.1:port/path, retrying for up to @retry_ms while the
 * connection is refused or the daemon is still starting (the gate's 503).
 * Returns the body (caller frees) and the status.  A probe's answer that
 * names another process than the daemon this test started is not this
 * test's (a sibling test's daemon on a port it reused) and counts as a
 * failure; when the last try failed, the status is 0, no body is returned,
 * and why is said in the test's log, never an earlier answer.
 */
static gchar *
http_get_retry(
	guint16      port,
	const gchar *path,
	guint        retry_ms,
	guint       *status,
	gchar      **content_type
){
	SoupSession *session = soup_session_new_with_options(
		"timeout", 10, "idle-timeout", 10, NULL);
	gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	gchar *result = NULL;
	gchar *failure = NULL;
	gint64 deadline = g_get_monotonic_time() + (gint64)retry_ms * 1000;

	*status = 0;
	if (content_type != NULL)
		*content_type = NULL;

	do {
		SoupMessage *message = soup_message_new(SOUP_METHOD_GET, url);
		GError *error = NULL;
		GBytes *bytes;

		bytes = soup_session_send_and_read(session, message, NULL, &error);
		if (bytes != NULL) {
			SoupMessageHeaders *headers = soup_message_get_response_headers(message);
			const gchar *process = soup_message_headers_get_one(headers, AXIS_WEB_HEALTH_PROCESS_HEADER);
			gsize size;
			gconstpointer data = g_bytes_get_data(bytes, &size);

			if (process != NULL && expected_process != NULL && strcmp(process, expected_process) != 0) {
				g_free(failure);
				failure = g_strdup_printf("%s was answered by process %s, not by the daemon this test started "
					"(%s): another program uses the port", path, process, expected_process);
				g_bytes_unref(bytes);
				g_object_unref(message);
				g_usleep(100 * 1000);
				continue;
			}
			g_clear_pointer(&failure, g_free);
			*status = soup_message_get_status(message);
			if (content_type != NULL) {
				g_free(*content_type);
				*content_type = g_strdup(soup_message_headers_get_content_type(headers, NULL));
			}
			g_free(result);
			result = g_strndup(data, size);
			g_bytes_unref(bytes);
			g_object_unref(message);
			if (*status == 503 && body_is_starting(result) && g_get_monotonic_time() < deadline) {
				g_usleep(100 * 1000);
				continue;
			}
			break;
		}

		/* The last try is what is reported: not an earlier answer. */
		g_free(failure);
		failure = g_strdup_printf("%s could not be asked: %s", path, error->message);
		g_clear_error(&error);
		g_object_unref(message);
		g_usleep(100 * 1000);
	} while (g_get_monotonic_time() < deadline);

	if (failure != NULL) {
		g_test_message("%s", failure);
		*status = 0;
		g_clear_pointer(&result, g_free);
		if (content_type != NULL)
			g_clear_pointer(content_type, g_free);
		g_free(failure);
	}
	g_free(url);
	g_object_unref(session);
	return result;
}

static gchar *
http_get(
	guint16      port,
	const gchar *path,
	guint       *status,
	gchar      **content_type
){
	return http_get_retry(port, path, STARTUP_TIMEOUT_MS, status, content_type);
}

/* Fetches /healthz and checks its shape; returns the parsed object's
 * "status" through the out parameter, and the database's word it stands
 * for ("ok" or "unreachable"): without a session or the metrics token the
 * health check says its status word and nothing else. */
static void
get_health(
	guint16   port,
	guint    *http_status,
	gchar   **status,
	gchar   **database
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *content_type = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *object;
	const gchar *word;

	body = http_get(port, "/healthz", http_status, &content_type);
	g_assert_nonnull(body);
	g_assert_cmpstr(content_type, ==, "application/json");

	json_parser_load_from_data(parser, body, -1, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
	object = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpuint(json_object_get_size(object), ==, 1);
	word = json_object_get_string_member(object, "status");
	/* The start-up gate's answer (still starting after the retries, or
	 * not holding the database lock) has another word: say which. */
	if (g_strcmp0(word, "ok") != 0 && g_strcmp0(word, "degraded") != 0)
		g_error("/healthz answered %u %s, not the health check", *http_status, body);

	*status = g_strdup(word);
	*database = g_strdup(g_strcmp0(word, "ok") == 0 ? "ok" : "unreachable");
}

static gchar *
count_migration_files(void)
{
	g_autoptr(GDir) dir = g_dir_open(AXIS_MIGRATION_DIR, 0, NULL);
	guint n = 0;

	g_assert_nonnull(dir);
	while (g_dir_read_name(dir) != NULL)
		n++;

	return g_strdup_printf("%u", n);
}

/* Spec (daemon-core): Health check succeeds */
static void
test_health_and_sigterm(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_autofree gchar *content_type = NULL;
	g_autofree gchar *status_text = NULL;
	g_autofree gchar *database = NULL;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;

	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(status_text, ==, "ok");
	g_assert_cmpstr(database, ==, "ok");

	/* The home page is HTML with a doctype ... */
	body = http_get(port, "/", &status, &content_type);
	g_assert_nonnull(body);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(content_type, ==, "text/html");
	g_assert_true(g_str_has_prefix(body, "<!DOCTYPE html>"));
	g_assert_nonnull(strstr(body, "<h1>"));
	/* ... but not to someone who is not logged in: with no account yet,
	 * the shell's layout shows first-run setup (the redirect is followed
	 * here), and the dashboard's details are not in it. */
	g_assert_nonnull(strstr(body, "<title>Set up Axis - Axis</title>"));
	g_assert_null(strstr(body, "<title>Dashboard - Axis</title>"));
	g_assert_nonnull(strstr(body, AXIS_VERSION));

	/* An unknown path is not a crash; without a session it is not even
	 * told apart from a known one, and leads to setup the same way. */
	g_free(body);
	body = http_get(port, "/nope", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(body, "<title>Set up Axis - Axis</title>"));

	stop_and_expect_zero(fx);
}

/*
 * /healthz is answered from a worker thread and is not a page, but the
 * shell still puts its security headers on it, and a wrong method gets a
 * 405 with Allow.
 */
static void
test_health_has_shell_headers(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autoptr(SoupSession) session = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/healthz", (guint)port);
	g_autofree gchar *body = NULL;
	g_autofree gchar *content_type = NULL;
	guint status;
	g_autoptr(SoupMessage) get = NULL;
	g_autoptr(SoupMessage) post = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *headers;

	if (!spawn_axisd_with_db(fx, port))
		return;

	/* Wait until it is up. */
	body = http_get(port, "/healthz", &status, &content_type);
	g_assert_nonnull(body);
	g_assert_cmpstr(content_type, ==, "application/json");

	session = soup_session_new_with_options("timeout", 10, "idle-timeout", 10, NULL);

	get = soup_message_new(SOUP_METHOD_GET, url);
	bytes = soup_session_send_and_read(session, get, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	headers = soup_message_get_response_headers(get);
	g_assert_cmpuint(soup_message_get_status(get), ==, SOUP_STATUS_OK);
	g_assert_nonnull(soup_message_headers_get_one(headers, "Content-Security-Policy"));
	g_assert_cmpstr(soup_message_headers_get_one(headers, "X-Content-Type-Options"), ==, "nosniff");
	g_assert_cmpstr(soup_message_headers_get_one(headers, "Cache-Control"), ==, "no-store");

	g_clear_pointer(&bytes, g_bytes_unref);
	post = soup_message_new(SOUP_METHOD_POST, url);
	bytes = soup_session_send_and_read(session, post, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(soup_message_get_status(post), ==, SOUP_STATUS_METHOD_NOT_ALLOWED);
	headers = soup_message_get_response_headers(post);
	g_assert_nonnull(strstr(soup_message_headers_get_one(headers, "Allow"), "GET"));

	stop_and_expect_zero(fx);
}

static void
test_sigint_exits_zero(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_nonnull(body);

	g_subprocess_send_signal(fx->proc, SIGINT);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), ==, 0);
}

static void
test_port_in_use(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *port_text = NULL;

	fx->held = bind_ephemeral();
	g_socket_listen(fx->held, &error);
	g_assert_no_error(error);
	port_text = g_strdup_printf("%u", (guint)socket_port(fx->held));

	if (!spawn_axisd_with_db(fx, socket_port(fx->held)))
		return;
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "127.0.0.1"));
	g_assert_nonnull(strstr(fx->stderr_text, port_text));
}

static void
test_missing_config_file(Fixture *fx, gconstpointer data)
{
	spawn_axisd(fx, 0, "--config", "/nonexistent/path");
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "/nonexistent/path"));
}

static void
write_config(
	Fixture     *fx,
	const gchar *contents
){
	g_autoptr(GError) error = NULL;
	gint fd;

	fd = g_file_open_tmp("axisd-test-XXXXXX.conf", &fx->tmp_path, &error);
	g_assert_no_error(error);
	g_close(fd, NULL);
	g_file_set_contents(fx->tmp_path, contents, -1, &error);
	g_assert_no_error(error);
}

static void
test_unknown_config_key(Fixture *fx, gconstpointer data)
{
	write_config(fx, "[server]\nbogus_key = 1\n");
	spawn_axisd(fx, 0, "--config", fx->tmp_path);
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "bogus_key"));
	g_assert_nonnull(strstr(fx->stderr_text, "server"));
}

static void
test_invalid_environment_port(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_LISTEN_PORT", "80abc", NULL };

	spawn_axisd_env(fx, 0, NULL, env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_LISTEN_PORT"));
}

static void
test_version_flag(Fixture *fx, gconstpointer data)
{
	spawn_axisd(fx, 0, "--version", NULL);
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), ==, 0);
}

/* Spec: No database URL */
static void
test_no_database_url(Fixture *fx, gconstpointer data)
{
	spawn_axisd(fx, pick_free_port(), NULL, NULL);
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "database URL is required"));
	g_assert_null(strstr(fx->stderr_text, "listening"));
}

/* Spec: Database unreachable, with database.wait 0 (try once) */
static void
test_database_unreachable(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_DATABASE_WAIT", "0", NULL };
	guint16 db_port = pick_free_port();
	guint16 port = pick_free_port();
	g_autofree gchar *url = NULL;

	url = g_strdup_printf("postgresql://axis:" SECRET "@127.0.0.1:%u/axis", (guint)db_port);
	spawn_axisd_env(fx, port, url, env, NULL, NULL);
	wait_for_exit(fx);

	g_test_message("stderr: %s", fx->stderr_text);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "cannot connect to the database"));
	g_assert_nonnull(strstr(fx->stderr_text, "postgresql://axis:****@127.0.0.1"));
	g_assert_null(strstr(fx->stderr_text, SECRET));
	/* It never got as far as serving. */
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_assert_null(strstr(fx->stderr_text, "trying again"));
}

/* An unreadable URL is never waited for, whatever database.wait says. */
static void
test_database_url_invalid_not_waited(Fixture *fx, gconstpointer data)
{
	gint64 start = g_get_monotonic_time();

	spawn_axisd_env(fx, pick_free_port(), "postgresql://axis:" SECRET "@127.0.0.1/axis?no_such_option=1",
		NULL, NULL, NULL);
	wait_for_exit(fx);

	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "invalid database URL"));
	g_assert_null(strstr(fx->stderr_text, SECRET));
	g_assert_null(strstr(fx->stderr_text, "trying again"));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 8 * G_USEC_PER_SEC);
}

/* What a GET of @path answers right now, without retrying. */
static guint
status_now(
	guint16      port,
	const gchar *path,
	gchar      **body
){
	guint status = 0;
	gchar *text = http_get_retry(port, path, 0, &status, NULL);

	if (body != NULL)
		*body = text;
	else
		g_free(text);
	return status;
}

/* Waits up to @seconds for @path to answer @expected. */
static gboolean
wait_status(
	guint16      port,
	const gchar *path,
	guint        expected,
	guint        seconds
){
	gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;

	do {
		if (status_now(port, path, NULL) == expected)
			return TRUE;
		g_usleep(200 * 1000);
	} while (g_get_monotonic_time() < deadline);

	return FALSE;
}

/*
 * Spec (deployment): Database not up yet -- the daemon starts before its
 * database, is alive and not ready, serves nothing else, keeps trying, and
 * becomes ready when the database is.
 */
static void
test_database_waited_for(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *out = NULL;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	/* The database exists but takes no connection yet. */
	axis_test_db_disconnect(fx->db);
	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS false", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	g_assert_true(wait_status(port, "/livez", 200, 10));

	/* Alive, not ready, and nothing else is served. */
	g_assert_cmpuint(status_now(port, "/livez", &body), ==, 200);
	g_assert_cmpstr(body, ==, "{\"status\":\"alive\"}");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/readyz", &body), ==, 503);
	g_assert_cmpstr(body, ==, "{\"status\":\"starting\"}");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/healthz", &body), ==, 503);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/", &body), ==, 503);
	g_assert_true(g_str_has_prefix(body, "Axis is starting"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/login", NULL), ==, 503);
	g_assert_cmpuint(status_now(port, "/setup", NULL), ==, 503);

	/* Still waiting after at least one retry. */
	body = read_stderr_until(fx, "trying again in");
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, "not currently accepting connections"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/readyz", NULL), ==, 503);

	/* The database comes up: ready within the back-off. */
	g_free(sql);
	g_free(out);
	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS true", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
	g_assert_true(wait_status(port, "/readyz", 200, 40));
	g_assert_cmpuint(status_now(port, "/readyz", &body), ==, 200);
	g_assert_cmpstr(body, ==, "{\"status\":\"ready\"}");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/livez", NULL), ==, 200);
	g_assert_cmpuint(status_now(port, "/healthz", NULL), ==, 200);
	/* The pages now: "/" is the setup page, after its redirect. */
	g_assert_cmpuint(status_now(port, "/", &body), ==, 200);
	g_assert_nonnull(strstr(body, "Set up Axis"));
	g_clear_pointer(&body, g_free);

	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "listening on"));
}

/* With database.wait set, it gives up after about that long. */
static void
test_database_wait_gives_up(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_DATABASE_WAIT", "3", NULL };
	guint16 db_port = pick_free_port();
	g_autofree gchar *url = g_strdup_printf("postgresql://axis:" SECRET "@127.0.0.1:%u/axis", (guint)db_port);
	gint64 start = g_get_monotonic_time();

	spawn_axisd_env(fx, pick_free_port(), url, env, NULL, NULL);
	wait_for_exit(fx);

	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "trying again in 1 s"));
	g_assert_nonnull(strstr(fx->stderr_text, "gave up waiting for the database"));
	g_assert_null(strstr(fx->stderr_text, SECRET));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 9 * G_USEC_PER_SEC);
}

/* SIGTERM while it waits: a clean stop, at once. */
static void
test_database_wait_stopped(Fixture *fx, gconstpointer data)
{
	guint16 db_port = pick_free_port();
	guint16 port = pick_free_port();
	g_autofree gchar *url = g_strdup_printf("postgresql://axis:" SECRET "@127.0.0.1:%u/axis", (guint)db_port);
	gint64 start;

	spawn_axisd_env(fx, port, url, NULL, NULL, NULL);
	g_assert_true(wait_status(port, "/livez", 200, 10));
	start = g_get_monotonic_time();
	stop_and_expect_zero(fx);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(fx->stderr_text, "shutting down before the database was ready"));
}

/*
 * Spec: Migrations applied before serving.  The test holds the migration
 * lock, so axisd's migrations wait; meanwhile it is alive but serves no page
 * and is not ready.  Once the lock goes, the first page comes from a
 * migrated database.
 */
static void
test_migrations_before_serving(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *lock_sql = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *files = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *status_text = NULL;
	g_autofree gchar *database = NULL;
	const gchar *applied;
	const gchar *listening;
	guint status;
	guint i;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	lock_sql = g_strdup_printf("SELECT pg_advisory_lock(%" G_GINT64_FORMAT ")",
		AXIS_DB_MIGRATION_LOCK_KEY);
	out = axis_test_db_query(fx->db, lock_sql);

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	g_assert_true(wait_status(port, "/livez", 200, 10));

	/* 1.5 s of serving nothing but the probes while the migrations cannot
	 * run. */
	for (i = 0; i < 15; i++) {
		g_assert_cmpuint(status_now(port, "/", NULL), ==, 503);
		g_assert_cmpuint(status_now(port, "/readyz", NULL), ==, 503);
		g_assert_cmpuint(status_now(port, "/livez", NULL), ==, 200);
		g_usleep(100 * 1000);
	}
	g_free(out);
	out = axis_test_db_query(fx->db, "SELECT to_regclass('axis_schema_migration') IS NULL");
	g_assert_cmpstr(out, ==, "t");

	g_free(out);
	out = axis_test_db_query(fx->db, "SELECT pg_advisory_unlock_all()");

	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(database, ==, "ok");

	files = count_migration_files();
	rows = axis_test_db_query(fx->db, "SELECT count(*) FROM axis_schema_migration");
	g_assert_cmpstr(rows, ==, files);

	stop_and_expect_zero(fx);

	/* It waited for the lock, and applied the migrations before it began
	 * to listen. */
	g_assert_nonnull(strstr(fx->stderr_text, "waiting for it to finish"));
	applied = strstr(fx->stderr_text, "applied migration 0001_schema_baseline");
	listening = strstr(fx->stderr_text, "listening on");
	g_assert_nonnull(applied);
	g_assert_nonnull(listening);
	g_assert_true(applied < listening);
}

/* Spec: Out-of-range pool size */
static void
test_pool_size_out_of_range(Fixture *fx, gconstpointer data)
{
	static const gchar *const values[] = { "0", "65", NULL };
	guint i;

	for (i = 0; values[i] != NULL; i++) {
		const gchar *env[3];
		g_autofree gchar *contents = NULL;

		env[0] = "AXIS_DATABASE_POOL_SIZE";
		env[1] = values[i];
		env[2] = NULL;
		spawn_axisd_env(fx, pick_free_port(), "postgresql://axis@127.0.0.1/axis", env, NULL, NULL);
		wait_for_exit(fx);
		g_assert_true(g_subprocess_get_if_exited(fx->proc));
		g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
		g_assert_nonnull(strstr(fx->stderr_text, "AXIS_DATABASE_POOL_SIZE"));
		g_clear_object(&fx->proc);

		contents = g_strdup_printf("[database]\nurl = postgresql://axis@127.0.0.1/axis\npool_size = %s\n",
			values[i]);
		write_config(fx, contents);
		spawn_axisd(fx, pick_free_port(), "--config", fx->tmp_path);
		wait_for_exit(fx);
		g_assert_true(g_subprocess_get_if_exited(fx->proc));
		g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
		g_assert_nonnull(strstr(fx->stderr_text, "database.pool_size"));
		g_clear_object(&fx->proc);
		g_unlink(fx->tmp_path);
		g_clear_pointer(&fx->tmp_path, g_free);
	}
}

/* Stops new connections to the test database and ends the existing ones. */
static void
database_go_away(Fixture *fx)
{
	g_autofree gchar *sql = NULL;
	g_autofree gchar *out = NULL;

	axis_test_db_disconnect(fx->db);
	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS false",
		axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
	g_free(sql);
	g_free(out);
	sql = g_strdup_printf("SELECT count(pg_terminate_backend(pid, 5000)) FROM pg_stat_activity "
		"WHERE datname = '%s'", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
}

static void
database_come_back(Fixture *fx)
{
	g_autofree gchar *sql = NULL;
	g_autofree gchar *out = NULL;

	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS true",
		axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
}

/*
 * Spec: Database goes away.  The daemon's own database is closed to new
 * connections (ALLOW_CONNECTIONS false) and its sessions terminated, which
 * is what a database that has gone away looks like to it; then it is
 * opened again, and the daemon recovers without a restart.
 */
static void
test_health_database_goes_away(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *status_text = NULL;
	g_autofree gchar *database = NULL;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;

	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(database, ==, "ok");

	database_go_away(fx);

	/* A probe's answer may be up to a second old, or the answer to a
	 * question asked just before the change (spec: Health endpoint). */
	g_assert_true(wait_status(port, "/healthz", SOUP_STATUS_SERVICE_UNAVAILABLE, 10));
	g_free(status_text);
	g_free(database);
	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_SERVICE_UNAVAILABLE);
	g_assert_cmpstr(status_text, ==, "degraded");
	g_assert_cmpstr(database, ==, "unreachable");

	/* Still unreachable on a second look, still answering. */
	g_free(status_text);
	g_free(database);
	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_SERVICE_UNAVAILABLE);

	database_come_back(fx);

	g_assert_true(wait_status(port, "/healthz", SOUP_STATUS_OK, 10));
	g_free(status_text);
	g_free(database);
	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(status_text, ==, "ok");
	g_assert_cmpstr(database, ==, "ok");

	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "database is unreachable"));
	g_assert_nonnull(strstr(fx->stderr_text, "reachable again"));
}

/*
 * A TCP proxy between axisd and PostgreSQL, run on its own thread and main
 * context.  It forwards until told to black-hole: then it drops every
 * forwarded connection and accepts new ones without ever answering, which
 * is a database host that has stopped responding.  The connection numbered
 * @spare (from 1, in the order accepted; 0 for none) is never dropped.
 */
struct _Proxy
{
	GThread         *thread;
	GMainContext    *context;
	GMainLoop       *loop;
	GSocketListener *listener;
	GCancellable    *forwarding;	/* cancelled to drop every forwarded connection */
	GCancellable    *spared;		/* the spared connection's, cancelled at the end */
	GPtrArray       *held;		/* connections accepted while black-holing */
	gchar           *upstream_host;
	guint16          upstream_port;
	guint16          port;
	guint            spare;		/* set before anything connects */
	guint            accepted;	/* the proxy thread's */
	gint             blackhole;	/* atomic */
	GMutex           lock;
	GCond            cond;
	gboolean         ready;
};

typedef struct
{
	Proxy             *proxy;
	GSocketConnection *client;
	guint              number;
} Pending;

static void proxy_accept_next(Proxy *proxy);

static void
on_spliced(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_io_stream_splice_finish(result, NULL);
}

static void
on_upstream(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Pending *pending = user_data;
	GSocketConnection *upstream;

	upstream = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, NULL);
	if (upstream != NULL) {
		Proxy *proxy = pending->proxy;

		g_io_stream_splice_async(G_IO_STREAM(pending->client), G_IO_STREAM(upstream),
			G_IO_STREAM_SPLICE_CLOSE_STREAM1 | G_IO_STREAM_SPLICE_CLOSE_STREAM2, G_PRIORITY_DEFAULT,
			pending->number == proxy->spare ? proxy->spared : proxy->forwarding, on_spliced, NULL);
		g_object_unref(upstream);
	} else {
		g_io_stream_close(G_IO_STREAM(pending->client), NULL, NULL);
	}

	g_object_unref(pending->client);
	g_free(pending);
}

static void
on_accepted(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Proxy *proxy = user_data;
	GSocketConnection *client;

	client = g_socket_listener_accept_finish(G_SOCKET_LISTENER(source), result, NULL, NULL);
	if (client == NULL)
		return;	/* closed: shutting down */

	proxy->accepted++;
	if (g_atomic_int_get(&proxy->blackhole)) {
		g_ptr_array_add(proxy->held, client);
	} else {
		g_autoptr(GSocketClient) connector = g_socket_client_new();
		Pending *pending = g_new0(Pending, 1);

		pending->proxy = proxy;
		pending->client = client;
		pending->number = proxy->accepted;
		g_socket_client_connect_to_host_async(connector, proxy->upstream_host,
			proxy->upstream_port, NULL, on_upstream, pending);
	}

	proxy_accept_next(proxy);
}

static void
proxy_accept_next(Proxy *proxy)
{
	g_socket_listener_accept_async(proxy->listener, NULL, on_accepted, proxy);
}

static gboolean
proxy_quit(gpointer data)
{
	Proxy *proxy = data;

	g_socket_listener_close(proxy->listener);
	g_cancellable_cancel(proxy->forwarding);
	g_cancellable_cancel(proxy->spared);
	g_ptr_array_set_size(proxy->held, 0);
	g_main_loop_quit(proxy->loop);
	return G_SOURCE_REMOVE;
}

static gpointer
proxy_thread(gpointer data)
{
	Proxy *proxy = data;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketAddress) bound = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	g_main_context_push_thread_default(proxy->context);

	address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_socket_listener_add_address(proxy->listener, address, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error);
	g_assert_no_error(error);

	g_mutex_lock(&proxy->lock);
	proxy->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	proxy->ready = TRUE;
	g_cond_broadcast(&proxy->cond);
	g_mutex_unlock(&proxy->lock);

	proxy_accept_next(proxy);
	g_main_loop_run(proxy->loop);

	/* Let the cancelled operations finish so nothing is left behind. */
	for (i = 0; i < 100; i++) {
		while (g_main_context_iteration(proxy->context, FALSE))
			;
		g_usleep(1000);
	}

	g_main_context_pop_thread_default(proxy->context);
	return NULL;
}

static Proxy *
proxy_new(
	const gchar *upstream_host,
	guint16      upstream_port
){
	Proxy *proxy = g_new0(Proxy, 1);

	proxy->context = g_main_context_new();
	proxy->loop = g_main_loop_new(proxy->context, FALSE);
	proxy->listener = g_socket_listener_new();
	proxy->forwarding = g_cancellable_new();
	proxy->spared = g_cancellable_new();
	proxy->held = g_ptr_array_new_with_free_func(g_object_unref);
	proxy->upstream_host = g_strdup(upstream_host);
	proxy->upstream_port = upstream_port;
	g_mutex_init(&proxy->lock);
	g_cond_init(&proxy->cond);

	proxy->thread = g_thread_new("proxy", proxy_thread, proxy);
	g_mutex_lock(&proxy->lock);
	while (!proxy->ready)
		g_cond_wait(&proxy->cond, &proxy->lock);
	g_mutex_unlock(&proxy->lock);

	return proxy;
}

/* From now on nothing is forwarded, and what was is cut. */
static void
proxy_blackhole(Proxy *proxy)
{
	g_atomic_int_set(&proxy->blackhole, TRUE);
	g_cancellable_cancel(proxy->forwarding);
}

static void
proxy_free(Proxy *proxy)
{
	g_main_context_invoke(proxy->context, proxy_quit, proxy);
	g_thread_join(proxy->thread);

	g_ptr_array_unref(proxy->held);
	g_object_unref(proxy->listener);
	g_object_unref(proxy->forwarding);
	g_object_unref(proxy->spared);
	g_main_loop_unref(proxy->loop);
	g_main_context_unref(proxy->context);
	g_free(proxy->upstream_host);
	g_mutex_clear(&proxy->lock);
	g_cond_clear(&proxy->cond);
	g_free(proxy);
}

typedef struct
{
	guint16  port;
	guint    status;
	gchar   *body;
	gint64   took_us;
	gint     done;	/* atomic */
} SlowGet;

static gpointer
slow_get_thread(gpointer data)
{
	SlowGet *get = data;
	gint64 start = g_get_monotonic_time();

	get->body = http_get_retry(get->port, "/healthz", 0, &get->status, NULL);
	get->took_us = g_get_monotonic_time() - start;
	g_atomic_int_set(&get->done, TRUE);
	return NULL;
}

/*
 * The health check must not block the main loop: with the database host
 * not answering at all, a health check takes as long as its connection
 * attempt is allowed to, and other requests are answered meanwhile.
 */
static void
test_health_does_not_block(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *url = NULL;
	g_autofree gchar *status_text = NULL;
	g_autofree gchar *database = NULL;
	g_autofree gchar *body = NULL;
	GThread *thread;
	SlowGet get;
	gint64 start;
	gint64 took;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	fx->proxy = proxy_new(axis_test_db_get_host(fx->db), axis_test_db_get_port(fx->db));
	/* The daemon's database lock stays on its own connection, the second
	 * it opens (the pool's first comes before it): the proxy spares it, so
	 * the daemon keeps its lock and serves while the database is gone for
	 * everything else.  (Losing the lock is tests/test-daemon-lock.c's.) */
	fx->proxy->spare = 2;
	url = axis_test_db_url_via(fx->db, fx->proxy->port, "connect_timeout=3");
	spawn_axisd_env(fx, port, url, NULL, NULL, NULL);

	get_health(port, &status, &status_text, &database);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	proxy_blackhole(fx->proxy);
	/* The answer just given is reused for a second (spec: Health
	 * endpoint): the next probe after that asks the database again. */
	g_usleep((AXIS_WEB_HEALTH_REUSE_MS + 200) * 1000);

	memset(&get, 0, sizeof(get));
	get.port = port;
	thread = g_thread_new("slow-get", slow_get_thread, &get);

	/* While that health check waits on the database, the home page is
	 * served at once. */
	g_usleep(300 * 1000);
	start = g_get_monotonic_time();
	body = http_get(port, "/", &status, NULL);
	took = g_get_monotonic_time() - start;
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpint(took, <, 1 * G_USEC_PER_SEC);
	g_assert_false(g_atomic_int_get(&get.done));

	g_thread_join(thread);
	g_test_message("blocked health check took %" G_GINT64_FORMAT " ms", get.took_us / 1000);
	g_assert_cmpuint(get.status, ==, SOUP_STATUS_SERVICE_UNAVAILABLE);
	g_assert_cmpstr(get.body, ==, "{\"status\":\"degraded\"}");
	g_assert_cmpint(get.took_us, >=, 1 * G_USEC_PER_SEC);
	g_assert_cmpint(get.took_us, <, 8 * G_USEC_PER_SEC);
	g_free(get.body);

	/* Shutdown does not hang on a database that does not answer.  The
	 * spared connection was the lock's: the lock was never lost. */
	stop_and_expect_zero(fx);
	g_assert_null(strstr(fx->stderr_text, "lost the database lock"));
}

/* Applies the migrations to the test's database, as axisd would. */
static void
migrate_test_database(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbPool) pool = NULL;

	pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 1, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_migrate(pool, &error));
	g_assert_no_error(error);
}

/* Polls the test database until @sql gives @expected (bounded). */
static gboolean
await_query(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	for (;;) {
		g_autofree gchar *value = axis_test_db_query(fx->db, sql);

		if (g_strcmp0(value, expected) == 0)
			return TRUE;
		if (g_get_monotonic_time() >= deadline) {
			g_test_message("%s is %s, not %s", sql, value != NULL ? value : "NULL", expected);
			return FALSE;
		}
		g_usleep(20 * 1000);
	}
}

/*
 * Spec (job-queue): Queued job survives restart, Handler missing after
 * restart, Old finished jobs are pruned -- through the real daemon: jobs
 * stored before it starts are run (or, for an unknown kind, failed with a
 * warning, so that their keys are free), and the built-in retention job runs
 * from its default schedule.
 */
static void
test_jobs_run_in_daemon(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	migrate_test_database(fx);
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, max_attempts, last_error) VALUES"
		" ('jobs.prune', 'maintenance', 3, 'stored before start'),"
		" ('gone.kind', 'gone', 3, 'unknown kind')");
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, finished_at, last_error) VALUES"
		" ('jobs.prune', 'maintenance', 'succeeded', 3, 1, now() - interval '8 days', 'old'),"
		" ('jobs.prune', 'maintenance', 'failed', 3, 3, now() - interval '8 days', 'kept')");

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	g_assert_true(await_query(fx, "SELECT state || '/' || attempts FROM job "
		"WHERE last_error = 'stored before start'", "succeeded/1"));
	g_assert_true(await_query(fx, "SELECT count(*) FROM job WHERE last_error = 'old'", "0"));
	g_assert_true(await_query(fx, "SELECT enabled::text || ' ' || run_interval FROM job_schedule "
		"WHERE name = 'jobs.prune'", "true 01:00:00"));
	g_assert_true(await_query(fx, "SELECT state || '/' || attempts || '/' || (last_error LIKE "
		"'this build of Axis has no handler for its kind, gone.kind:%') FROM job WHERE kind = 'gone.kind'",
		"failed/0/true"));
	g_assert_true(await_query(fx, "SELECT count(*) FROM job WHERE last_error = 'kept'", "1"));

	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "job queue started: compat (1), convert (1), downloads (1), maintenance (1)"));
	g_assert_nonnull(strstr(fx->stderr_text, "job kind 'gone.kind' has no handler"));
	g_assert_nonnull(strstr(fx->stderr_text, "job queue stopped"));
}

/* A jobs.concurrency naming a queue no kind uses, or more workers than
 * the pool can serve, stops the daemon before it serves. */
static void
test_jobs_settings_checked(Fixture *fx, gconstpointer data)
{
	const gchar *env[5];

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	env[0] = "AXIS_JOBS_CONCURRENCY";
	env[1] = "maintenance:1, nosuch:2";
	env[2] = NULL;
	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "'nosuch'"));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_clear_object(&fx->proc);

	/* (The library's scan queue, the probe queue, the Savings
	 * measurement's queue, the compat queue, the metadata queue, the
	 * downloads queue, the searches' queue, the conversions' queue and the YouTube queue are
	 * set to 0 so that the sum stays 4.) */
	env[1] = "maintenance:4, scan:0, probe:0, sample:0, compat:0, metadata:0, downloads:0, searches:0, convert:0, "
		"youtube:0";
	env[2] = "AXIS_DATABASE_POOL_SIZE";
	env[3] = "5";
	env[4] = NULL;
	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "the database pool has 5 connections, but the job "
		"queue needs 6"));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_clear_object(&fx->proc);

	/* Leaving the queue to external workers needs only the queue's own
	 * two connections. */
	{
		guint16 port = pick_free_port();
		g_autofree gchar *body = NULL;
		guint status;

		env[1] = "maintenance:0, scan:0, probe:0, sample:0, compat:0, metadata:0, downloads:0, searches:0, "
			"convert:0, youtube:0";
		env[3] = "2";
		spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
		body = http_get(port, "/healthz", &status, NULL);
		g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
		stop_and_expect_zero(fx);
		g_assert_nonnull(strstr(fx->stderr_text, "job queue started: compat (0), convert (0), downloads (0), "
			"maintenance (0)"));
	}
}

/*
 * Spec (daemon-core): A pool set too small changes nothing; a pool not set
 * fits the queues.  A pool set for an earlier build, with fewer queues, is
 * refused before any migration of this one is applied, so that the earlier
 * image still starts on the database; with no pool set, the pool is what the
 * job queue needs and 4 more, whatever the concurrency.
 */
static void
test_pool_sized_before_migrations(Fixture *fx, gconstpointer data)
{
	const gchar *env[5];
	g_autofree gchar *table = NULL;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	/* scan:2,probe:2 and eight queues at 1: 12 threads and 2, so 12 (the
	 * number the configuration example once gave) is two short. */
	env[0] = "AXIS_DATABASE_POOL_SIZE";
	env[1] = "12";
	env[2] = "AXIS_JOBS_CONCURRENCY";
	env[3] = "scan:2,probe:2";
	env[4] = NULL;
	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "the database pool has 12 connections, but the job queue needs 14"));
	g_assert_nonnull(strstr(fx->stderr_text, "at least 14"));
	g_assert_null(strstr(fx->stderr_text, "applied migration"));
	table = axis_test_db_query(fx->db, "SELECT coalesce(to_regclass('axis_schema_migration')::text, 'none')");
	g_assert_cmpstr(table, ==, "none");
	g_clear_object(&fx->proc);

	/* Not set: 8 and 8 and eight at 1 are 24 threads, 26 with the queue's
	 * own two, and 30 with the room for the pages. */
	{
		guint16 port = pick_free_port();
		g_autofree gchar *body = NULL;
		guint status;

		env[0] = "AXIS_JOBS_CONCURRENCY";
		env[1] = "scan:8,probe:8";
		env[2] = NULL;
		spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
		body = http_get(port, "/healthz", &status, NULL);
		g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
		stop_and_expect_zero(fx);
		g_assert_nonnull(strstr(fx->stderr_text, "pool of 30 connections"));
	}
}

/* Bad [jobs] values are refused naming the setting. */
static void
test_jobs_settings_invalid(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_JOBS_GRACE_PERIOD", "soon", NULL };

	write_config(fx, "[jobs]\nconcurrency = scan=2\n");
	spawn_axisd(fx, 0, "--config", fx->tmp_path);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "jobs.concurrency"));
	g_clear_object(&fx->proc);

	spawn_axisd_env(fx, 0, NULL, env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_JOBS_GRACE_PERIOD"));
}

/* ── Accounts and sessions through the real daemon ──────────────── */

/*
 * Reads the child's stderr (its file), line by line, from where the last
 * call stopped, until a line contains @needle; returns that line, or NULL
 * when the child has ended without writing one.  It waits for as long as
 * the child runs.
 */
static gchar *
read_stderr_until(
	Fixture     *fx,
	const gchar *needle
){
	gboolean ended = FALSE;

	for (;;) {
		g_autofree gchar *text = NULL;
		gsize length = 0;
		const gchar *end;

		if (!g_file_get_contents(fx->stderr_path, &text, &length, NULL))
			length = 0;
		while (fx->stderr_seen < length &&
		       (end = memchr(text + fx->stderr_seen, '\n', length - fx->stderr_seen)) != NULL) {
			gchar *line = g_strndup(text + fx->stderr_seen, (gsize)(end - (text + fx->stderr_seen)));

			fx->stderr_seen = (gsize)(end - text) + 1;
			if (strstr(line, needle) != NULL)
				return line;
			g_free(line);
		}
		if (ended)
			return NULL;
		/* GSubprocess forgets the child's id once it has seen it end,
		 * which it does on the main context: one more look at the file
		 * then, for what the child wrote last. */
		g_main_context_iteration(NULL, FALSE);
		if (g_subprocess_get_identifier(fx->proc) == NULL)
			ended = TRUE;
		else
			g_usleep(20 * 1000);
	}
}

typedef struct
{
	guint  status;
	gchar *body;
	gchar *cookies;		/* the name=value of every Set-Cookie, "; "-joined */
	gchar *location;
} HttpReply;

static void
http_reply_clear(HttpReply *reply)
{
	g_free(reply->body);
	g_free(reply->cookies);
	g_free(reply->location);
}

/* One request, redirects not followed, with @cookie and an urlencoded
 * @form (both nullable). */
static HttpReply
http_send(
	guint16      port,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *form
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;
	GString *cookies = g_string_new(NULL);
	HttpReply reply;
	gsize size;
	gconstpointer data;

	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (form != NULL) {
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}

	bytes = soup_session_send_and_read(session, message, NULL, &error);
	g_assert_no_error(error);
	data = g_bytes_get_data(bytes, &size);

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(message));
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		if (g_ascii_strcasecmp(name, "Set-Cookie") != 0 || strstr(value, "Max-Age=0") != NULL)
			continue;
		if (cookies->len > 0)
			g_string_append(cookies, "; ");
		g_string_append_len(cookies, value, (gssize)strcspn(value, ";"));
	}

	reply.status = soup_message_get_status(message);
	reply.body = g_strndup(data != NULL ? data : "", size);
	reply.cookies = g_string_free(cookies, FALSE);
	reply.location = g_strdup(soup_message_headers_get_one(
		soup_message_get_response_headers(message), "Location"));

	return reply;
}

/* The value of the form field called @name on @html. */
static gchar *
form_field(
	const gchar *html,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("name=\"%s\"", name);
	const gchar *p = html;

	while ((p = strstr(p, "<input")) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *tag = g_strndup(p, end - p);
		const gchar *value;

		if (strstr(tag, needle) != NULL && (value = strstr(tag, " value=\"")) != NULL) {
			value += strlen(" value=\"");
			return g_strndup(value, strchr(value, '"') - value);
		}
		p = end;
	}

	return NULL;
}

/*
 * Spec (web-auth): Setup with the token, Session survives restart; and
 * every page but the public ones needs the session that setup opened.
 */
static void
test_auth_setup_and_restart(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *line = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *body = NULL;
	HttpReply reply;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;

	line = read_stderr_until(fx, "setup token");
	g_assert_nonnull(line);
	g_assert_nonnull(strstr(line, "WARNING"));
	token = g_strdup(line + strlen(line) - 32);

	/* Wait until it serves; then nothing but the public paths without a
	 * session. */
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	reply = http_send(port, "GET", "/", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(reply.location, ==, "/setup");
	http_reply_clear(&reply);

	reply = http_send(port, "GET", "/setup", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_OK);
	csrf = form_field(reply.body, "_csrf");
	g_assert_nonnull(csrf);
	g_assert_true(g_str_has_prefix(reply.cookies, "axis_presession="));
	form = g_strdup_printf("_csrf=%s&setup_token=%s&username=Admin&password=first-admin-pw"
		"&password_confirm=first-admin-pw", csrf, token);
	{
		g_autofree gchar *presession = g_strdup(reply.cookies);

		http_reply_clear(&reply);
		reply = http_send(port, "POST", "/setup", presession, form);
	}
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(reply.location, ==, "/");
	g_assert_true(g_str_has_prefix(reply.cookies, "axis_session="));
	session = g_strdup(reply.cookies);
	http_reply_clear(&reply);

	reply = http_send(port, "GET", "/", session, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(reply.body, "<title>Dashboard - Axis</title>"));
	http_reply_clear(&reply);

	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "audit: setup: created administrator user=admin"));
	/* The password went nowhere near the log. */
	g_assert_null(strstr(fx->stderr_text, "first-admin-pw"));
	g_assert_null(strstr(fx->stderr_text, session + strlen("axis_session=")));
	g_clear_object(&fx->proc);

	/* A new daemon on the same database: the session is still good, and
	 * no setup token is printed now. */
	port = pick_free_port();
	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	g_free(body);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	reply = http_send(port, "GET", "/account", session, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(reply.body, "admin"));
	http_reply_clear(&reply);
	reply = http_send(port, "GET", "/setup", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_NOT_FOUND);
	http_reply_clear(&reply);

	stop_and_expect_zero(fx);
	g_assert_null(strstr(fx->stderr_text, "setup token"));
}

/* Spec (daemon-core): Effective list is logged */
static void
test_auth_trusted_proxies_logged(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_TRUSTED_PROXIES", "10.0.0.7/8, ::1", NULL };
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	stop_and_expect_zero(fx);

	g_assert_nonnull(strstr(fx->stderr_text, "INFO axis: trusted proxies: 10.0.0.0/8, ::1"));
}

/* Spec (outbound-binding): Start-up and readiness do not wait for the
 * interface, and its state is logged once at start; a value of the wrong
 * form stops the start, saying what is expected. */
static void
test_outbound_bind(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_OUTBOUND_BIND", "axis-absent0", NULL };
	static const gchar *const bad[] = { "AXIS_OUTBOUND_BIND", "tun 0", NULL };
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_true(wait_status(port, "/readyz", 200, 10));
	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "outbound_bind=axis-absent0 outbound_bind_scope=internet"));
	g_assert_nonnull(strstr(fx->stderr_text, "WARNING axis: outbound connections: the outbound interface axis-absent0 "
		"does not exist; connections to indexers and metadata providers at public addresses are refused until it "
		"is up, and nothing is sent another way"));
	g_clear_object(&fx->proc);

	spawn_axisd_env(fx, 0, NULL, bad, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_OUTBOUND_BIND: 'tun 0' is not an interface name"));
}

/* Spec (daemon-core): Invalid proxy entry */
static void
test_auth_trusted_proxies_invalid(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_TRUSTED_PROXIES", "10.0.0.0/8, tra efik", NULL };
	static const gchar *const cookies[] = { "AXIS_SECURE_COOKIES", "maybe", NULL };

	spawn_axisd_env(fx, 0, NULL, env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_TRUSTED_PROXIES"));
	g_assert_nonnull(strstr(fx->stderr_text, "'tra efik'"));
	g_clear_object(&fx->proc);

	write_config(fx, "[server]\ntrusted_proxies = 192.168.1.0/24, 300.1.1.1\n");
	spawn_axisd(fx, 0, "--config", fx->tmp_path);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "server.trusted_proxies"));
	g_assert_nonnull(strstr(fx->stderr_text, "'300.1.1.1'"));
	g_clear_object(&fx->proc);

	spawn_axisd_env(fx, 0, NULL, cookies, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_SECURE_COOKIES"));
}

/* ── The first administrator from the configuration ─────────────── */

#define ADMIN_PASSWORD "from-a-secret-file-7"
#define METRICS_TOKEN  "metrics-token-0123456789abcdef"

/* Logs in through the login page; returns the session cookie or NULL. */
static gchar *
log_in(
	guint16      port,
	const gchar *username,
	const gchar *password
){
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *presession = NULL;
	g_autofree gchar *form = NULL;
	HttpReply reply;
	gchar *session = NULL;

	reply = http_send(port, "GET", "/login", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_OK);
	csrf = form_field(reply.body, "_csrf");
	presession = g_strdup(reply.cookies);
	http_reply_clear(&reply);
	g_assert_nonnull(csrf);

	form = g_strdup_printf("_csrf=%s&username=%s&password=%s", csrf, username, password);
	reply = http_send(port, "POST", "/login", presession, form);
	if (reply.status == 303 && g_str_has_prefix(reply.cookies, "axis_session="))
		session = g_strdup(reply.cookies);
	http_reply_clear(&reply);

	return session;
}

/*
 * Spec (deployment): First administrator from a secret -- with no account,
 * the daemon creates the administrator the configuration names, its
 * password from a file, instead of a setup token; the password logs in and
 * is never logged; later starts leave the accounts alone.
 */
static void
test_first_admin_from_secret(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *password_file = g_build_filename(fx->scratch, "admin-password", NULL);
	const gchar *env[5];
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *wrong = NULL;
	HttpReply reply;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	g_assert_true(g_file_set_contents(password_file, ADMIN_PASSWORD "\n", -1, NULL));
	env[0] = "AXIS_ADMIN_USERNAME";
	env[1] = "Boss";
	env[2] = "AXIS_ADMIN_PASSWORD_FILE";
	env[3] = password_file;
	env[4] = NULL;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	/* No setup to do: the page is gone, and "/" asks for a login. */
	reply = http_send(port, "GET", "/setup", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_NOT_FOUND);
	http_reply_clear(&reply);
	reply = http_send(port, "GET", "/", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_prefix(reply.location, "/login"));
	http_reply_clear(&reply);

	/* The password from the file logs in, as an administrator; another does not. */
	wrong = log_in(port, "boss", "not-the-password-1");
	g_assert_null(wrong);
	session = log_in(port, "boss", ADMIN_PASSWORD);
	g_assert_nonnull(session);
	reply = http_send(port, "GET", "/admin/users", session, NULL);
	g_assert_cmpuint(reply.status, ==, SOUP_STATUS_OK);
	http_reply_clear(&reply);

	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text,
		"audit: setup: created administrator user=boss from the configuration"));
	g_assert_null(strstr(fx->stderr_text, "setup token"));
	g_assert_null(strstr(fx->stderr_text, ADMIN_PASSWORD));
	g_clear_object(&fx->proc);

	/* Started again with the same settings: the account exists, nothing is
	 * created, and that is said. */
	port = pick_free_port();
	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	g_free(body);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "accounts exist already"));
	g_assert_null(strstr(fx->stderr_text, "created administrator"));
	g_assert_null(strstr(fx->stderr_text, ADMIN_PASSWORD));
}

/* The same rules as the setup page: refused at start, saying which rule,
 * never quoting the password; nothing is created. */
static void
test_first_admin_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const short_password[] = {
		"AXIS_ADMIN_USERNAME", "boss", "AXIS_ADMIN_PASSWORD", "nine-char", NULL
	};
	static const gchar *const bad_name[] = {
		"AXIS_ADMIN_USERNAME", "-boss", "AXIS_ADMIN_PASSWORD", ADMIN_PASSWORD, NULL
	};
	static const gchar *const half[] = { "AXIS_ADMIN_USERNAME", "boss", NULL };
	g_autofree gchar *users = NULL;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), short_password, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "the first administrator's password"));
	g_assert_nonnull(strstr(fx->stderr_text, "cannot be used"));
	g_assert_null(strstr(fx->stderr_text, "nine-char"));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_clear_object(&fx->proc);

	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), bad_name, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "the first administrator's username"));
	g_assert_nonnull(strstr(fx->stderr_text, "A username is"));
	g_assert_null(strstr(fx->stderr_text, ADMIN_PASSWORD));
	g_clear_object(&fx->proc);

	spawn_axisd_env(fx, pick_free_port(), axis_test_db_get_url(fx->db), half, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "needs both a username and a password"));

	users = axis_test_db_query(fx->db, "SELECT count(*) FROM app_user");
	g_assert_cmpstr(users, ==, "0");
}

/* ── Probes, metrics, JSON logs ─────────────────────────────────── */

/* GET @path with an Authorization header (nullable); the status. */
static guint
get_with_authorization(
	guint16      port,
	const gchar *path,
	const gchar *authorization,
	gchar      **body
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, url);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;

	if (authorization != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization", authorization);
	bytes = soup_session_send_and_read(session, message, NULL, &error);
	g_assert_no_error(error);
	if (body != NULL)
		*body = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	return soup_message_get_status(message);
}

/*
 * Spec (deployment): liveness and readiness are separate; ready only at the
 * expected schema version.  Spec (deployment): the metrics endpoint answers
 * only with its token, and is absent without one.
 */
static void
test_probes_and_metrics(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_METRICS_TOKEN", METRICS_TOKEN, NULL };
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_autofree gchar *metrics = NULL;
	g_autofree gchar *out = NULL;
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpuint(status_now(port, "/livez", NULL), ==, 200);
	g_assert_true(wait_status(port, "/readyz", 200, 10));

	/* Neither probe needs a session, nor says anything but its word. */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(status_now(port, "/readyz", &body), ==, 200);
	g_assert_cmpstr(body, ==, "{\"status\":\"ready\"}");
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(get_with_authorization(port, "/healthz", NULL, &body), ==, 200);
	g_assert_cmpstr(body, ==, "{\"status\":\"ok\"}");
	/* The metrics token opens the version (spec web-auth: the probes say
	 * only their status words to strangers). */
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(get_with_authorization(port, "/healthz", "Bearer " METRICS_TOKEN, &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"version\":\"" AXIS_VERSION "\""));
	g_assert_nonnull(strstr(body, "\"database\":\"ok\""));

	/* The metrics: refused without the token or with another, 200 with it. */
	g_assert_cmpuint(get_with_authorization(port, "/metrics", NULL, NULL), ==, 401);
	g_assert_cmpuint(get_with_authorization(port, "/metrics", "Bearer " METRICS_TOKEN "x", NULL), ==, 401);
	g_assert_cmpuint(get_with_authorization(port, "/metrics", "Basic " METRICS_TOKEN, NULL), ==, 401);
	g_assert_cmpuint(get_with_authorization(port, "/metrics", "Bearer " METRICS_TOKEN, &metrics), ==, 200);
	g_assert_nonnull(strstr(metrics, "# TYPE axis_http_requests_total counter"));
	g_assert_nonnull(strstr(metrics, "axis_http_requests_total{code=\"2xx\"}"));
	g_assert_nonnull(strstr(metrics, "axis_http_request_duration_seconds_bucket{le=\"+Inf\"}"));
	g_assert_nonnull(strstr(metrics, "# TYPE axis_jobs gauge"));
	g_assert_nonnull(strstr(metrics, "# TYPE axis_downloads gauge"));
	g_assert_nonnull(strstr(metrics, "# TYPE axis_imports gauge"));
	g_assert_nonnull(strstr(metrics, "axis_database_up 1"));
	g_assert_null(strstr(metrics, METRICS_TOKEN));

	/* A schema that is not this build's (a newer build migrated it): alive,
	 * not ready. */
	out = axis_test_db_query(fx->db,
		"INSERT INTO axis_schema_migration (version, name, checksum) VALUES (9999, 'from_a_newer_build', "
		"repeat('0', 64))");
	g_assert_true(wait_status(port, "/readyz", 503, 10));
	g_assert_cmpuint(status_now(port, "/livez", NULL), ==, 200);

	stop_and_expect_zero(fx);
	g_assert_null(strstr(fx->stderr_text, METRICS_TOKEN));
	g_assert_nonnull(strstr(fx->stderr_text, "metrics: /metrics is on"));
	g_clear_object(&fx->proc);

	/* Without a token: there is no /metrics at all. */
	port = pick_free_port();
	g_free(out);
	out = axis_test_db_query(fx->db, "DELETE FROM axis_schema_migration WHERE version = 9999");
	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), NULL, NULL, NULL);
	g_free(body);
	body = http_get(port, "/livez", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	/* Alive is not yet serving: until the database is ready every other
	 * path answers 503, which is not the 404 this is about. */
	g_assert_true(wait_status(port, "/readyz", 200, 30));
	g_assert_cmpuint(get_with_authorization(port, "/metrics", "Bearer " METRICS_TOKEN, NULL), ==, 404);
	stop_and_expect_zero(fx);
}

/* Spec (deployment): JSON logs -- every line the daemon writes is one JSON
 * object, and secrets stay out of it as they do out of the text format. */
static void
test_json_logs(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_LOG_FORMAT", "json", NULL };
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_auto(GStrv) lines = NULL;
	guint status;
	guint n = 0;
	guint i;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	spawn_axisd_env(fx, port, axis_test_db_get_url(fx->db), env, NULL, NULL);
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	stop_and_expect_zero(fx);

	lines = g_strsplit(fx->stderr_text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		JsonObject *object;

		if (lines[i][0] == '\0')
			continue;
		if (!json_parser_load_from_data(parser, lines[i], -1, &error))
			g_error("not a JSON line: %s", lines[i]);
		object = json_node_get_object(json_parser_get_root(parser));
		g_assert_nonnull(object);
		g_assert_cmpuint(json_object_get_size(object), ==, 4);
		g_assert_true(json_object_has_member(object, "message"));
		n++;
	}
	g_assert_cmpuint(n, >, 3);
	g_assert_nonnull(strstr(fx->stderr_text, "\"message\":\"listening on 127.0.0.1:"));
}

/* ── The database URL from a file ──────────────────────────────── */

/* Spec (daemon-core): Read from file.  The file holds the URL and a
 * newline, as a container secret or an "echo" leaves it. */
static void
test_database_url_from_file(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *contents = NULL;
	g_autofree gchar *body = NULL;
	const gchar *env[3];
	guint status;

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	contents = g_strdup_printf("%s\n", axis_test_db_get_url(fx->db));
	write_config(fx, contents);
	env[0] = "AXIS_DATABASE_URL_FILE";
	env[1] = fx->tmp_path;
	env[2] = NULL;
	spawn_axisd_env(fx, port, NULL, env, NULL, NULL);

	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpstr(body, ==, "{\"status\":\"ok\"}");

	/* wait_for_exit() fails the test if the password shows up in the log. */
	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, "connected to the database"));
}

/* Spec (daemon-core): Both set */
static void
test_database_url_and_file_both_set(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = {
		"AXIS_DATABASE_URL_FILE", "/run/secrets/axis-database-url", NULL
	};

	spawn_axisd_env(fx, pick_free_port(), "postgresql://axis@127.0.0.1/axis", env, NULL, NULL);
	wait_for_exit(fx);

	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_DATABASE_URL "));
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_DATABASE_URL_FILE"));
	g_assert_null(strstr(fx->stderr_text, "listening"));
}

/* Spec (daemon-core): Unreadable file.  Three paths that give no URL: one
 * that is not there, a directory, and a file with nothing in it.  Each
 * error names the path. */
static void
test_database_url_file_unreadable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *dir = g_dir_make_tmp("axisd-test-dir-XXXXXX", NULL);
	const gchar *paths[3];
	guint i;

	write_config(fx, "\n");
	paths[0] = "/nonexistent/axis-database-url";
	paths[1] = dir;
	paths[2] = fx->tmp_path;

	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		const gchar *env[3];

		env[0] = "AXIS_DATABASE_URL_FILE";
		env[1] = paths[i];
		env[2] = NULL;
		spawn_axisd_env(fx, pick_free_port(), NULL, env, NULL, NULL);
		wait_for_exit(fx);
		g_assert_true(g_subprocess_get_if_exited(fx->proc));
		g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
		g_assert_nonnull(strstr(fx->stderr_text, "AXIS_DATABASE_URL_FILE"));
		g_assert_nonnull(strstr(fx->stderr_text, paths[i]));
		g_assert_null(strstr(fx->stderr_text, "listening"));
		g_clear_object(&fx->proc);
	}

	g_rmdir(dir);
}

/* ── The data directory ────────────────────────────────────────── */

/* Spec (daemon-core): Created -- the configured directory does not exist
 * and its parent is writable: the daemon creates it (with its artwork
 * directory) and starts. */
static void
test_data_dir_created(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	g_autofree gchar *artwork = g_build_filename(fx->data_dir, "artwork", NULL);
	GStatBuf st;
	guint status;

	g_assert_false(g_file_test(fx->data_dir, G_FILE_TEST_EXISTS));
	if (!spawn_axisd_with_db(fx, port))
		return;
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);
	g_assert_cmpint(g_stat(fx->data_dir, &st), ==, 0);
	g_assert_true(S_ISDIR(st.st_mode));
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_true(g_file_test(artwork, G_FILE_TEST_IS_DIR));
	stop_and_expect_zero(fx);
	g_assert_nonnull(strstr(fx->stderr_text, fx->data_dir));
}

/* Spec (daemon-core): Not writable -- the daemon exits with an error naming
 * the directory, before it listens.  (Root may write anywhere, so the
 * permission case is skipped for root; the missing parent is not.) */
static void
test_data_dir_not_writable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *orphan = g_build_filename(fx->scratch, "no-such-parent", "data", NULL);
	const gchar *env[3];

	/* A parent that is not there: nothing is created. */
	env[0] = "AXIS_DATA_DIR";
	env[1] = orphan;
	env[2] = NULL;
	spawn_axisd_env(fx, pick_free_port(), "postgresql://axis@127.0.0.1:1/axis", env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_true(g_subprocess_get_if_exited(fx->proc));
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, orphan));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_assert_false(g_file_test(orphan, G_FILE_TEST_EXISTS));
	g_clear_object(&fx->proc);

	if (geteuid() == 0) {
		g_test_message("running as root: the permission case is skipped");
		return;
	}
	/* A directory the daemon may not write in. */
	g_assert_cmpint(g_mkdir(fx->data_dir, 0500), ==, 0);
	spawn_axisd_env(fx, pick_free_port(), "postgresql://axis@127.0.0.1:1/axis", NULL, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, fx->data_dir));
	g_assert_nonnull(strstr(fx->stderr_text, "cannot be written"));
	g_assert_null(strstr(fx->stderr_text, "listening on"));
	g_clear_object(&fx->proc);
	g_assert_cmpint(g_chmod(fx->data_dir, 0700), ==, 0);

	/* One anybody may write in is not the daemon's own. */
	g_assert_cmpint(g_chmod(fx->data_dir, 0777), ==, 0);
	spawn_axisd_env(fx, pick_free_port(), "postgresql://axis@127.0.0.1:1/axis", NULL, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, fx->data_dir));
	g_assert_nonnull(strstr(fx->stderr_text, "every user"));
	g_assert_cmpint(g_chmod(fx->data_dir, 0700), ==, 0);
}

/* A data directory that is not an absolute path is refused as a setting,
 * naming it. */
static void
test_data_dir_invalid(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_DATA_DIR", "relative/data", NULL };

	spawn_axisd_env(fx, 0, NULL, env, NULL, NULL);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "AXIS_DATA_DIR"));
	g_clear_object(&fx->proc);

	write_config(fx, "[data]\ndir = /var/lib/../axis\n");
	spawn_axisd(fx, 0, "--config", fx->tmp_path);
	wait_for_exit(fx);
	g_assert_cmpint(g_subprocess_get_exit_status(fx->proc), !=, 0);
	g_assert_nonnull(strstr(fx->stderr_text, "data.dir"));
}

/* ── axisd --healthcheck ───────────────────────────────────────── */

typedef struct
{
	gint    status;		/* the exit status, -1 if killed by a signal */
	gint64  took_us;
	gchar  *stderr_text;
} CheckResult;

/*
 * Runs `axisd --healthcheck` for a daemon on 127.0.0.1:@port, with a clean
 * environment (so no database URL: the mode needs none) plus @env.
 */
static CheckResult
run_healthcheck(
	guint16              port,
	const gchar * const *env
){
	GSubprocessLauncher *launcher;
	GSubprocess *proc;
	g_autofree gchar *port_text = g_strdup_printf("%u", (guint)port);
	g_autoptr(GError) error = NULL;
	const gchar *argv[3];
	CheckResult result;
	gint64 start;
	guint i;

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_PIPE |
		G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	for (i = 0; clean[i] != NULL; i++)
		g_subprocess_launcher_unsetenv(launcher, clean[i]);
	g_subprocess_launcher_setenv(launcher, "AXIS_LISTEN_PORT", port_text, TRUE);
	for (i = 0; env != NULL && env[i] != NULL; i += 2)
		g_subprocess_launcher_setenv(launcher, env[i], env[i + 1], TRUE);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);

	argv[0] = AXISD_PATH;
	argv[1] = "--healthcheck";
	argv[2] = NULL;
	start = g_get_monotonic_time();
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_object_unref(launcher);

	g_subprocess_communicate_utf8(proc, NULL, NULL, NULL, &result.stderr_text, &error);
	g_assert_no_error(error);
	result.took_us = g_get_monotonic_time() - start;
	result.status = g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
	g_object_unref(proc);

	g_assert_null(strstr(result.stderr_text, "Sanitizer"));
	g_assert_null(strstr(result.stderr_text, "runtime error"));

	return result;
}

/* Spec (daemon-core): Healthy.  The check is given no database URL and
 * no file: it must not need one. */
static void
test_healthcheck_healthy(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	CheckResult result;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	result = run_healthcheck(port, NULL);
	g_assert_cmpint(result.status, ==, 0);
	g_assert_cmpuint(strlen(result.stderr_text), ==, 0);
	g_free(result.stderr_text);

	/* The check does not touch the data directory: one it could not use
	 * changes nothing. */
	{
		const gchar *const env[] = { "AXIS_DATA_DIR", "/nonexistent/axis-data", NULL };

		result = run_healthcheck(port, env);
		g_assert_cmpint(result.status, ==, 0);
		g_assert_cmpuint(strlen(result.stderr_text), ==, 0);
		g_free(result.stderr_text);
		g_assert_false(g_file_test("/nonexistent/axis-data", G_FILE_TEST_EXISTS));
	}

	stop_and_expect_zero(fx);
}

/* Spec (daemon-core): Unhealthy -- the daemon answers 503 while its
 * database is gone, and the check says so within five seconds; when the
 * database is back, it says healthy again. */
static void
test_healthcheck_unhealthy(Fixture *fx, gconstpointer data)
{
	guint16 port = pick_free_port();
	g_autofree gchar *body = NULL;
	CheckResult result;
	guint status;

	if (!spawn_axisd_with_db(fx, port))
		return;
	body = http_get(port, "/healthz", &status, NULL);
	g_assert_cmpuint(status, ==, SOUP_STATUS_OK);

	/* A probe's answer may be up to a second old, or the answer to a
	 * question asked just before the change (spec: Health endpoint). */
	database_go_away(fx);
	g_assert_true(wait_status(port, "/healthz", SOUP_STATUS_SERVICE_UNAVAILABLE, 10));
	result = run_healthcheck(port, NULL);
	g_assert_cmpint(result.status, ==, 1);
	g_assert_cmpint(result.took_us, <, 5 * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(result.stderr_text, "503"));
	g_free(result.stderr_text);

	database_come_back(fx);
	g_assert_true(wait_status(port, "/healthz", SOUP_STATUS_OK, 10));
	result = run_healthcheck(port, NULL);
	g_assert_cmpint(result.status, ==, 0);
	g_free(result.stderr_text);

	stop_and_expect_zero(fx);
}

/* Spec (daemon-core): Unhealthy or absent -- nothing listens, or something
 * accepts connections and never answers; both exit non-zero within five
 * seconds.  A URL file that cannot be read does not matter to the check. */
static void
test_healthcheck_absent(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = {
		"AXIS_DATABASE_URL_FILE", "/nonexistent/axis-database-url", NULL
	};
	g_autoptr(GError) error = NULL;
	CheckResult result;

	result = run_healthcheck(pick_free_port(), env);
	g_assert_cmpint(result.status, ==, 1);
	g_assert_cmpint(result.took_us, <, 5 * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(result.stderr_text, "health check failed"));
	g_assert_null(strstr(result.stderr_text, "AXIS_DATABASE_URL_FILE"));
	g_free(result.stderr_text);

	/* The kernel completes the connection, so this one hangs until the
	 * check gives up. */
	fx->held = bind_ephemeral();
	g_socket_listen(fx->held, &error);
	g_assert_no_error(error);
	result = run_healthcheck(socket_port(fx->held), NULL);
	g_assert_cmpint(result.status, ==, 1);
	g_assert_cmpint(result.took_us, <, 5 * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(result.stderr_text, "health check failed"));
	g_free(result.stderr_text);
}

/* A configuration that is wrong is not "healthy" either. */
static void
test_healthcheck_bad_configuration(Fixture *fx, gconstpointer data)
{
	static const gchar *const env[] = { "AXIS_LOG_LEVEL", "loud", NULL };
	CheckResult result;

	result = run_healthcheck(pick_free_port(), env);
	g_assert_cmpint(result.status, ==, 1);
	g_assert_nonnull(strstr(result.stderr_text, "AXIS_LOG_LEVEL"));
	g_free(result.stderr_text);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	/* Backstop against a hang anywhere: SIGALRM kills this process and
	 * the death signal takes any axisd with it. */
	alarm(240);

	g_test_add("/daemon/auth/setup-and-restart", Fixture, NULL,
		fixture_setup, test_auth_setup_and_restart, fixture_teardown);
	g_test_add("/daemon/auth/trusted-proxies-logged", Fixture, NULL,
		fixture_setup, test_auth_trusted_proxies_logged, fixture_teardown);
	g_test_add("/daemon/auth/trusted-proxies-invalid", Fixture, NULL,
		fixture_setup, test_auth_trusted_proxies_invalid, fixture_teardown);
	g_test_add("/daemon/outbound/bind", Fixture, NULL,
		fixture_setup, test_outbound_bind, fixture_teardown);

	g_test_add("/daemon/health-and-sigterm", Fixture, NULL,
		fixture_setup, test_health_and_sigterm, fixture_teardown);
	g_test_add("/daemon/health/has-shell-headers", Fixture, NULL,
		fixture_setup, test_health_has_shell_headers, fixture_teardown);
	g_test_add("/daemon/sigint-exits-zero", Fixture, NULL,
		fixture_setup, test_sigint_exits_zero, fixture_teardown);
	g_test_add("/daemon/port-in-use", Fixture, NULL,
		fixture_setup, test_port_in_use, fixture_teardown);
	g_test_add("/daemon/missing-config-file", Fixture, NULL,
		fixture_setup, test_missing_config_file, fixture_teardown);
	g_test_add("/daemon/unknown-config-key", Fixture, NULL,
		fixture_setup, test_unknown_config_key, fixture_teardown);
	g_test_add("/daemon/invalid-environment-port", Fixture, NULL,
		fixture_setup, test_invalid_environment_port, fixture_teardown);
	g_test_add("/daemon/version-flag", Fixture, NULL,
		fixture_setup, test_version_flag, fixture_teardown);
	g_test_add("/daemon/database/no-url", Fixture, NULL,
		fixture_setup, test_no_database_url, fixture_teardown);
	g_test_add("/daemon/database/unreachable", Fixture, NULL,
		fixture_setup, test_database_unreachable, fixture_teardown);
	g_test_add("/daemon/database/url-invalid-not-waited", Fixture, NULL,
		fixture_setup, test_database_url_invalid_not_waited, fixture_teardown);
	g_test_add("/daemon/database/waited-for", Fixture, NULL,
		fixture_setup, test_database_waited_for, fixture_teardown);
	g_test_add("/daemon/database/wait-gives-up", Fixture, NULL,
		fixture_setup, test_database_wait_gives_up, fixture_teardown);
	g_test_add("/daemon/database/wait-stopped", Fixture, NULL,
		fixture_setup, test_database_wait_stopped, fixture_teardown);
	g_test_add("/daemon/setup/first-admin-from-secret", Fixture, NULL,
		fixture_setup, test_first_admin_from_secret, fixture_teardown);
	g_test_add("/daemon/setup/first-admin-refused", Fixture, NULL,
		fixture_setup, test_first_admin_refused, fixture_teardown);
	g_test_add("/daemon/probes-and-metrics", Fixture, NULL,
		fixture_setup, test_probes_and_metrics, fixture_teardown);
	g_test_add("/daemon/log/json", Fixture, NULL,
		fixture_setup, test_json_logs, fixture_teardown);
	g_test_add("/daemon/database/migrations-before-serving", Fixture, NULL,
		fixture_setup, test_migrations_before_serving, fixture_teardown);
	g_test_add("/daemon/database/pool-size-out-of-range", Fixture, NULL,
		fixture_setup, test_pool_size_out_of_range, fixture_teardown);
	g_test_add("/daemon/health/database-goes-away", Fixture, NULL,
		fixture_setup, test_health_database_goes_away, fixture_teardown);
	g_test_add("/daemon/health/does-not-block", Fixture, NULL,
		fixture_setup, test_health_does_not_block, fixture_teardown);
	g_test_add("/daemon/jobs/run-in-daemon", Fixture, NULL,
		fixture_setup, test_jobs_run_in_daemon, fixture_teardown);
	g_test_add("/daemon/jobs/settings-checked", Fixture, NULL,
		fixture_setup, test_jobs_settings_checked, fixture_teardown);
	g_test_add("/daemon/jobs/pool-sized-before-migrations", Fixture, NULL,
		fixture_setup, test_pool_sized_before_migrations, fixture_teardown);
	g_test_add("/daemon/jobs/settings-invalid", Fixture, NULL,
		fixture_setup, test_jobs_settings_invalid, fixture_teardown);

	g_test_add("/daemon/database/url-from-file", Fixture, NULL,
		fixture_setup, test_database_url_from_file, fixture_teardown);
	g_test_add("/daemon/database/url-and-file-both-set", Fixture, NULL,
		fixture_setup, test_database_url_and_file_both_set, fixture_teardown);
	g_test_add("/daemon/database/url-file-unreadable", Fixture, NULL,
		fixture_setup, test_database_url_file_unreadable, fixture_teardown);
	g_test_add("/daemon/data-dir/created", Fixture, NULL,
		fixture_setup, test_data_dir_created, fixture_teardown);
	g_test_add("/daemon/data-dir/not-writable", Fixture, NULL,
		fixture_setup, test_data_dir_not_writable, fixture_teardown);
	g_test_add("/daemon/data-dir/invalid", Fixture, NULL,
		fixture_setup, test_data_dir_invalid, fixture_teardown);
	g_test_add("/daemon/healthcheck/healthy", Fixture, NULL,
		fixture_setup, test_healthcheck_healthy, fixture_teardown);
	g_test_add("/daemon/healthcheck/unhealthy", Fixture, NULL,
		fixture_setup, test_healthcheck_unhealthy, fixture_teardown);
	g_test_add("/daemon/healthcheck/absent", Fixture, NULL,
		fixture_setup, test_healthcheck_absent, fixture_teardown);
	g_test_add("/daemon/healthcheck/bad-configuration", Fixture, NULL,
		fixture_setup, test_healthcheck_bad_configuration, fixture_teardown);

	return g_test_run();
}
