/*
 * test-portal-process.c - the real axis-portal (AXIS_PORTAL_PATH) against
 * the real axisd (AXISD_PATH)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3; the `portal` specification.)  Both programs
 * are started as they are deployed: the daemon on a database of the test's
 * own with its portal listener on, the portal with the daemon's address and
 * a token in a file, behind a "proxy" on this machine that it trusts (the
 * test itself, which says X-Forwarded-Proto: https).  The accounts are real
 * ones, written to the database before the daemon reads it:
 *
 *   root        an administrator
 *   robin, sam  requesters
 *
 * Each child is killed with the test (PR_SET_PDEATHSIG) and force-killed in
 * teardown, and every wait has a deadline.
 */

#define _GNU_SOURCE

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define HAVE_LANDLOCK 1
#endif
#endif

#include "../clients/portal/axis-portal-app.h"
#include "api/axis-portal-proto.h"
#include "axis-test-db.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "web/axis-password.h"
#include "web/axis-web-headers.h"

#ifndef AXISD_PATH
#error "AXISD_PATH must be defined by the build"
#endif
#ifndef AXIS_PORTAL_PATH
#error "AXIS_PORTAL_PATH must be defined by the build"
#endif

#define ROOT_PASSWORD  "a made-up password of root"
#define ROBIN_PASSWORD "lantern-orchard-pass"
#define SAM_PASSWORD   "saltmarsh-ledger-pass"
#define OUTSIDE        "203.0.113.5"

#define START_TIMEOUT_US (G_GINT64_CONSTANT(40) * G_USEC_PER_SEC)

typedef struct
{
	AxisTestDb  *db;
	gchar       *scratch;
	GSubprocess *daemon;
	GSubprocess *portal;
	gchar       *daemon_log;
	gchar       *portal_log;
	guint        n_logs;
	guint16      daemon_port;	/* the engine's interface */
	guint16      api_port;		/* the daemon's portal listener */
	guint16      port;		/* the portal's pages */
	gchar       *token;
	gchar       *token_file;
	gboolean     token_in_env;	/* start_portal() gives the token in AXIS_PORTAL_TOKEN */
	gint64       root;
	gint64       robin;
	gint64       sam;
	gint64       film;		/* a title that can be asked for */
} Fixture;

/* ── Requests ───────────────────────────────────────────────────── */

typedef struct
{
	guint       status;
	gchar      *body;
	gchar      *location;
	GHashTable *cookies;	/* name -> value, of every Set-Cookie */
} Reply;

static void
reply_clear(Reply *reply)
{
	g_free(reply->body);
	g_free(reply->location);
	g_clear_pointer(&reply->cookies, g_hash_table_unref);
}

/* One request, redirects not followed.  @from: the client the test's
 * "proxy" forwards for (%NULL: no forwarding header at all); @bearer and
 * @json: a call of the portal API itself.  A status of 0: not reached. */
static Reply
send_request(
	guint16      port,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *form,
	const gchar *from,
	const gchar *bearer,
	const gchar *json
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, NULL);
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;
	gconstpointer data;
	gsize size = 0;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	reply.cookies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_append(headers, "Cookie", cookie);
	if (from != NULL) {
		soup_message_headers_append(headers, "X-Forwarded-Proto", "https");
		soup_message_headers_append(headers, "X-Forwarded-For", from);
	}
	if (bearer != NULL) {
		g_autofree gchar *authorization = g_strconcat("Bearer ", bearer, NULL);

		soup_message_headers_append(headers, "Authorization", authorization);
	}
	if (form != NULL || json != NULL) {
		const gchar *text = form != NULL ? form : json;
		g_autoptr(GBytes) body = g_bytes_new(text, strlen(text));

		soup_message_set_request_body_from_bytes(message, form != NULL ? "application/x-www-form-urlencoded" :
			"application/json", body);
	}
	bytes = soup_session_send_and_read(session, message, NULL, &error);
	if (bytes == NULL)
		return reply;
	data = g_bytes_get_data(bytes, &size);
	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(message));
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		gsize name_len;

		if (g_ascii_strcasecmp(name, "Set-Cookie") != 0)
			continue;
		name_len = strcspn(value, "=");
		g_hash_table_replace(reply.cookies, g_strndup(value, name_len),
			g_strndup(value + name_len + 1, strcspn(value + name_len + 1, ";")));
	}
	reply.status = soup_message_get_status(message);
	reply.body = g_strndup(data != NULL ? data : "", size);
	reply.location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	return reply;
}

/* The response header @name of GET @path on @port (transfer full). */
static gchar *
header_of(
	guint16      port,
	const gchar *path,
	const gchar *from,
	const gchar *name
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, NULL);
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autoptr(GBytes) bytes = NULL;

	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (from != NULL) {
		soup_message_headers_append(soup_message_get_request_headers(message), "X-Forwarded-Proto", "https");
		soup_message_headers_append(soup_message_get_request_headers(message), "X-Forwarded-For", from);
	}
	bytes = soup_session_send_and_read(session, message, NULL, NULL);
	g_assert_nonnull(bytes);
	return g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), name));
}

/* A page of the portal, as a browser behind the trusted proxy asks for it. */
static Reply
page(
	Fixture     *fx,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *form
){
	return send_request(fx->port, method, path, cookie, form, OUTSIDE, NULL, NULL);
}

#define assert_status(reply, expected) G_STMT_START { \
	if ((reply)->status != (expected)) \
		g_error("%s:%d: expected %u, got %u:\n%s", __FILE__, __LINE__, (guint)(expected), (reply)->status, \
			(reply)->body != NULL ? (reply)->body : "(not reached)"); \
} G_STMT_END

#define assert_has(text, needle) G_STMT_START { \
	if (strstr((text), (needle)) == NULL) \
		g_error("%s:%d: \"%s\" is not in:\n%s", __FILE__, __LINE__, (needle), (text)); \
} G_STMT_END

#define assert_lacks(text, needle) G_STMT_START { \
	if (strstr((text), (needle)) != NULL) \
		g_error("%s:%d: \"%s\" is in:\n%s", __FILE__, __LINE__, (needle), (text)); \
} G_STMT_END

/* The value of the first hidden field called @name in @html. */
static gchar *
hidden_value(
	const gchar *html,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("name=\"%s\" value=\"", name);
	const gchar *start = strstr(html, needle);

	if (start == NULL)
		return NULL;
	start += strlen(needle);
	return g_strndup(start, strcspn(start, "\""));
}

/* Waits until @path on @port is answered 200. */
static void
wait_ready(
	guint16      port,
	const gchar *path
){
	gint64 deadline = g_get_monotonic_time() + START_TIMEOUT_US;

	for (;;) {
		Reply reply = send_request(port, "GET", path, NULL, NULL, NULL, NULL, NULL);
		guint status = reply.status;

		reply_clear(&reply);
		if (status == 200)
			return;
		if (g_get_monotonic_time() >= deadline)
			g_error("port %u did not answer %s in time (last status %u)", (guint)port, path, status);
		g_usleep(50 * 1000);
	}
}

/* ── The two programs ───────────────────────────────────────────── */

typedef struct
{
	gint abi;	/* Landlock's, or 0: no file system rule is made */
} ChildRules;

/* In the child, between fork and exec.  With rules: from here on the
 * process can write no file, make none and remove none, anywhere. */
static void
child_setup(gpointer user_data)
{
	const ChildRules *rules = user_data;

	prctl(PR_SET_PDEATHSIG, SIGKILL);
#ifdef HAVE_LANDLOCK
	if (rules != NULL && rules->abi > 0) {
		struct landlock_ruleset_attr attr;
		int fd;

		memset(&attr, 0, sizeof(attr));
		attr.handled_access_fs = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
			LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
			LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
			LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
		fd = (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
		/* No rule allows anything: every write to a file is refused. */
		if (fd < 0 || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || syscall(SYS_landlock_restrict_self, fd, 0) != 0)
			_exit(97);
		close(fd);
	}
#else
	(void)rules;
#endif
}

static gint
landlock_abi(void)
{
#ifdef HAVE_LANDLOCK
	long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);

	return abi > 0 ? (gint)abi : 0;
#else
	return 0;
#endif
}

/* What a test must not inherit from the environment it runs in. */
static const gchar *const clean[] = {
	"AXIS_LISTEN_HOST", "AXIS_LISTEN_PORT", "AXIS_DATABASE_URL", "AXIS_DATABASE_URL_FILE",
	"AXIS_DATABASE_POOL_SIZE", "AXIS_DATABASE_WAIT", "AXIS_LOG_LEVEL", "AXIS_LOG_FORMAT",
	"AXIS_JOBS_CONCURRENCY", "AXIS_JOBS_GRACE_PERIOD", "AXIS_TRUSTED_PROXIES", "AXIS_SECURE_COOKIES",
	"AXIS_WORKER_LEASE", "AXIS_DATA_DIR", "AXIS_ADMIN_USERNAME", "AXIS_ADMIN_PASSWORD",
	"AXIS_ADMIN_PASSWORD_FILE", "AXIS_METRICS_TOKEN", "AXIS_METRICS_TOKEN_FILE", "AXIS_HEAD_TIMEOUT",
	"AXIS_BODY_TIMEOUT", "AXIS_IDLE_TIMEOUT", "AXIS_MAX_CONNECTIONS", "AXIS_MAX_CONNECTIONS_PER_ADDRESS",
	"AXIS_PORTAL_LISTEN_PORT", "AXIS_PORTAL_LISTEN_HOST", NULL
};

static gchar *
next_log(
	Fixture     *fx,
	const gchar *name
){
	return g_strdup_printf("%s/%s-%u.log", fx->scratch, name, fx->n_logs++);
}

static void
start_daemon(Fixture *fx)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	g_autofree gchar *data_dir = g_build_filename(fx->scratch, "data", NULL);
	g_autofree gchar *port = g_strdup_printf("%u", (guint)fx->daemon_port);
	g_autofree gchar *api_port = g_strdup_printf("%u", (guint)fx->api_port);
	g_autoptr(GError) error = NULL;
	guint i;

	g_free(fx->daemon_log);
	fx->daemon_log = next_log(fx, "axisd");
	g_subprocess_launcher_set_stderr_file_path(launcher, fx->daemon_log);
	for (i = 0; clean[i] != NULL; i++)
		g_subprocess_launcher_unsetenv(launcher, clean[i]);
	g_subprocess_launcher_setenv(launcher, "AXIS_DATA_DIR", data_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_LISTEN_PORT", port, TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_PORTAL_LISTEN_PORT", api_port, TRUE);
	/* The administrator of these tests reaches the daemon through a proxy
	 * on this machine, from elsewhere (admin_change()). */
	g_subprocess_launcher_setenv(launcher, "AXIS_TRUSTED_PROXIES", "127.0.0.1", TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_DATABASE_URL", axis_test_db_get_url(fx->db), TRUE);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
	fx->daemon = g_subprocess_launcher_spawn(launcher, &error, AXISD_PATH, NULL);
	g_assert_no_error(error);
	wait_ready(fx->daemon_port, "/healthz");
}

/* The portal's environment: nothing of the test's own but what a sanitized
 * build needs, and its settings.  @extra: name and value pairs, %NULL-ended. */
static GSubprocessLauncher *
portal_launcher(
	Fixture            *fx,
	const gchar *const *extra,
	GSubprocessFlags    flags
){
	static const gchar *const kept[] = { "PATH", "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", NULL };
	GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);
	g_auto(GStrv) envp = g_new0(gchar *, 1);
	guint i;

	for (i = 0; kept[i] != NULL; i++) {
		if (g_getenv(kept[i]) != NULL)
			envp = g_environ_setenv(envp, kept[i], g_getenv(kept[i]), TRUE);
	}
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		envp = g_environ_setenv(envp, extra[i], extra[i + 1], TRUE);
	g_subprocess_launcher_set_environ(launcher, envp);
	return launcher;
}

/* Starts axis-portal as it is deployed.  @rules: (nullable) what its process
 * may not do; @directory: (nullable) where it runs and what it is told is
 * its home and its place for temporary files. */
static void
start_portal(
	Fixture          *fx,
	const ChildRules *rules,
	const gchar      *directory
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->api_port);
	g_autofree gchar *port = g_strdup_printf("%u", (guint)fx->port);
	const gchar *env[32];
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	guint n = 0;

	env[n++] = "AXIS_PORTAL_URL";
	env[n++] = url;
	env[n++] = fx->token_in_env ? "AXIS_PORTAL_TOKEN" : "AXIS_PORTAL_TOKEN_FILE";
	env[n++] = fx->token_in_env ? fx->token : fx->token_file;
	env[n++] = "AXIS_PORTAL_WEB_LISTEN_PORT";
	env[n++] = port;
	env[n++] = "AXIS_PORTAL_TRUSTED_PROXIES";
	env[n++] = "127.0.0.1";
	env[n++] = "AXIS_PORTAL_LOG_LEVEL";
	env[n++] = "debug";
	if (directory != NULL) {
		env[n++] = "HOME";
		env[n++] = directory;
		env[n++] = "TMPDIR";
		env[n++] = directory;
		env[n++] = "XDG_CACHE_HOME";
		env[n++] = directory;
		env[n++] = "XDG_CONFIG_HOME";
		env[n++] = directory;
		env[n++] = "XDG_DATA_HOME";
		env[n++] = directory;
		env[n++] = "XDG_RUNTIME_DIR";
		env[n++] = directory;
	}
	env[n] = NULL;
	launcher = portal_launcher(fx, env, G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	g_free(fx->portal_log);
	fx->portal_log = next_log(fx, "axis-portal");
	g_subprocess_launcher_set_stderr_file_path(launcher, fx->portal_log);
	if (directory != NULL)
		g_subprocess_launcher_set_cwd(launcher, directory);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, (gpointer)rules, NULL);
	fx->portal = g_subprocess_launcher_spawn(launcher, &error, AXIS_PORTAL_PATH, NULL);
	g_assert_no_error(error);
	wait_ready(fx->port, "/healthz");
}

/* Ends @proc with SIGTERM and waits; its exit status (-1: it had to be
 * killed, or was killed by a signal). */
static gint
stop(GSubprocess **proc)
{
	gint64 deadline = g_get_monotonic_time() + G_GINT64_CONSTANT(30) * G_USEC_PER_SEC;
	gint status = -1;

	if (*proc == NULL)
		return -1;
	g_subprocess_send_signal(*proc, SIGTERM);
	while (g_subprocess_get_identifier(*proc) != NULL && g_get_monotonic_time() < deadline) {
		g_main_context_iteration(NULL, FALSE);
		g_usleep(20 * 1000);
	}
	if (g_subprocess_get_identifier(*proc) != NULL)
		g_subprocess_force_exit(*proc);
	g_subprocess_wait(*proc, NULL, NULL);
	if (g_subprocess_get_if_exited(*proc))
		status = g_subprocess_get_exit_status(*proc);
	g_clear_object(proc);
	return status;
}

/* Runs axis-portal to its end with @env and @argument; its exit status and
 * what it wrote to standard error. */
static gint
run_portal(
	Fixture             *fx,
	const gchar *const  *env,
	const gchar         *argument,
	const gchar         *value,
	gchar              **stderr_text
){
	g_autoptr(GSubprocessLauncher) launcher = portal_launcher(fx, env, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;

	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
	proc = g_subprocess_launcher_spawn(launcher, &error, AXIS_PORTAL_PATH, argument, value, NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, NULL, stderr_text, &error);
	g_assert_no_error(error);
	return g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
}

/* ── The fixture ────────────────────────────────────────────────── */

static gint64
insert(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
insert(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *value = NULL;
	va_list args;

	va_start(args, format);
	sql = g_strdup_vprintf(format, args);
	va_end(args);
	value = axis_test_db_query(fx->db, sql);
	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

static gint64
add_account(
	Fixture     *fx,
	const gchar *username,
	const gchar *role,
	const gchar *password
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = axis_password_hash(password, &error);

	g_assert_no_error(error);
	g_assert_null(strchr(hash, '\''));
	return insert(fx, "INSERT INTO app_user (username, password_hash, role) VALUES ('%s', '%s', '%s') RETURNING id",
		username, hash, role);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;

	memset(fx, 0, sizeof(*fx));
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->scratch = axis_test_storage_dir_new();
	g_assert_true(axis_password_init());
	/* The accounts' hashes are made with the smallest limits: the daemon
	 * checks a password with the limits its hash was made with. */
	axis_password_use_test_cost();

	/* The database as the daemon would make it, with the accounts, a token
	 * and a title in it before the daemon reads them. */
	{
		g_autoptr(AxisDbPool) pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 1, &error);

		g_assert_no_error(error);
		g_assert_true(axis_db_migrate(pool, &error));
		g_assert_no_error(error);
	}
	fx->root = add_account(fx, "root", "admin", ROOT_PASSWORD);
	fx->robin = add_account(fx, "robin", "requester", ROBIN_PASSWORD);
	fx->sam = add_account(fx, "sam", "requester", SAM_PASSWORD);
	fx->token = axis_random_token();
	hash = axis_token_hash(fx->token);
	insert(fx, "INSERT INTO portal_token (token_hash, created_by) VALUES ('%s', 'root') RETURNING id", hash);
	fx->film = insert(fx, "INSERT INTO meta_title (kind, title, year, overview, provider, provider_id, adult,"
		" detail_fetched_at) VALUES ('film', 'The Lantern Orchard', 2019, 'A made-up film about lanterns.', 'tmdb',"
		" '990001', false, now()) RETURNING id");
	fx->token_file = g_build_filename(fx->scratch, "portal-token", NULL);
	g_assert_true(g_file_set_contents(fx->token_file, fx->token, -1, NULL));

	fx->daemon_port = axis_test_free_port();
	fx->api_port = axis_test_free_port();
	fx->port = axis_test_free_port();
	start_daemon(fx);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL)
		return;
	if (fx->portal != NULL) {
		g_subprocess_force_exit(fx->portal);
		g_subprocess_wait(fx->portal, NULL, NULL);
		g_object_unref(fx->portal);
	}
	if (fx->daemon != NULL) {
		g_subprocess_force_exit(fx->daemon);
		g_subprocess_wait(fx->daemon, NULL, NULL);
		g_object_unref(fx->daemon);
	}
	axis_test_db_free(fx->db);
	g_free(fx->daemon_log);
	g_free(fx->portal_log);
	axis_secret_free(fx->token);
	g_free(fx->token_file);
	axis_test_storage_dir_free(fx->scratch);
}

/* ── What people do ─────────────────────────────────────────────── */

typedef struct
{
	gchar *session;	/* "name=value" of the session cookie */
	gchar *device;	/* of the device cookie */
	gchar *proof;	/* the proof in the session cookie's value */
	gchar *value;	/* that value: the proof and its seal */
} Browser;

static void
browser_clear(Browser *browser)
{
	g_free(browser->session);
	g_free(browser->device);
	g_free(browser->proof);
	g_free(browser->value);
	memset(browser, 0, sizeof(*browser));
}

/* A login at the portal, as a browser at @from does it: the form, then its
 * fields, with @device (nullable: "name=value") beside the form's cookie.
 * The reply's status; @browser: (out) (optional) what the browser holds
 * after a login that worked. */
static guint
portal_login(
	Fixture     *fx,
	const gchar *username,
	const gchar *password,
	const gchar *from,
	const gchar *device,
	Browser     *browser
){
	Reply form_page = send_request(fx->port, "GET", "/login", NULL, NULL, from, NULL, NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *cookies = NULL;
	g_autofree gchar *form = NULL;
	const gchar *form_cookie;
	Reply reply;
	guint status;

	assert_status(&form_page, 200);
	token = hidden_value(form_page.body, "_csrf");
	form_cookie = g_hash_table_lookup(form_page.cookies, AXIS_PORTAL_FORM_COOKIE);
	g_assert_nonnull(token);
	g_assert_nonnull(form_cookie);
	cookies = device != NULL ? g_strdup_printf("%s=%s; %s", AXIS_PORTAL_FORM_COOKIE, form_cookie, device) :
		g_strdup_printf("%s=%s", AXIS_PORTAL_FORM_COOKIE, form_cookie);
	form = g_strdup_printf("_csrf=%s&username=%s&password=%s", token, username, password);
	reply_clear(&form_page);

	reply = send_request(fx->port, "POST", "/login", cookies, form, from, NULL, NULL);
	status = reply.status;
	if (browser != NULL) {
		memset(browser, 0, sizeof(*browser));
		if (status == 303) {
			const gchar *proof = g_hash_table_lookup(reply.cookies, AXIS_PORTAL_SESSION_COOKIE);
			const gchar *value = g_hash_table_lookup(reply.cookies, AXIS_PORTAL_DEVICE_COOKIE);

			g_assert_nonnull(proof);
			/* The proof and its seal. */
			g_assert_cmpuint(strlen(proof), ==, 2 * AXIS_PORTAL_PROOF_LEN + 1);
			g_assert_cmpint(proof[AXIS_PORTAL_PROOF_LEN], ==, '.');
			browser->proof = g_strndup(proof, AXIS_PORTAL_PROOF_LEN);
			browser->value = g_strdup(proof);
			browser->session = g_strdup_printf("%s=%s", AXIS_PORTAL_SESSION_COOKIE, proof);
			browser->device = value != NULL ? g_strdup_printf("%s=%s", AXIS_PORTAL_DEVICE_COOKIE, value) : NULL;
		}
	}
	reply_clear(&reply);
	return status;
}

/* The session's CSRF token, from a page's form. */
static gchar *
csrf_of(
	Fixture       *fx,
	const Browser *browser
){
	Reply reply = page(fx, "GET", "/requests", browser->session, NULL);
	gchar *token;

	assert_status(&reply, 200);
	token = hidden_value(reply.body, "_csrf");
	g_assert_nonnull(token);
	reply_clear(&reply);
	return token;
}

/* Where the administrator of these tests is, as a proxy on this machine
 * tells the daemon (which believes it: AXIS_TRUSTED_PROXIES). */
#define ADMIN_FROM "192.0.2.50"

/* root's right password at the engine's own login page, from @from (%NULL:
 * this machine's own address, the one the portal calls from): the status,
 * 303 when it opened a session. */
static guint
engine_login_status(
	Fixture     *fx,
	const gchar *from
){
	g_autoptr(GRegex) field = g_regex_new("name=\"_csrf\"[^>]*value=\"([^\"]+)\"|value=\"([^\"]+)\"[^>]*name=\"_csrf\"", 0,
		0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *form = NULL;
	const gchar *name = from != NULL ? "__Host-axis_presession" : "axis_presession";
	const gchar *made = from != NULL ? "__Host-axis_session" : "axis_session";
	Reply reply = send_request(fx->daemon_port, "GET", "/login", NULL, NULL, from, NULL, NULL);
	guint status;

	assert_status(&reply, 200);
	g_assert_true(g_regex_match(field, reply.body, 0, &match));
	token = g_match_info_fetch(match, 1);
	if (token == NULL || token[0] == '\0') {
		g_free(token);
		token = g_match_info_fetch(match, 2);
	}
	g_assert_nonnull(g_hash_table_lookup(reply.cookies, name));
	cookie = g_strdup_printf("%s=%s", name, (const gchar *)g_hash_table_lookup(reply.cookies, name));
	reply_clear(&reply);
	escaped = g_uri_escape_string(token, NULL, FALSE);
	form = g_strdup_printf("_csrf=%s&username=root&password=%s", escaped, "a+made-up+password+of+root");
	reply = send_request(fx->daemon_port, "POST", "/login", cookie, form, from, NULL, NULL);
	status = reply.status;
	/* A session only with the 303. */
	g_assert_true((status == 303) == (g_hash_table_lookup(reply.cookies, made) != NULL));
	reply_clear(&reply);
	return status;
}

/* An administrator's change at the engine's own interface: logs root in
 * there and posts @form to @path of the Users page. */
static void
admin_change(
	Fixture     *fx,
	const gchar *path,
	const gchar *form
){
	Reply reply;
	g_autofree gchar *presession = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	/* Slice P4 (the real daemon and the real portal): the portal has called
	 * the daemon from this machine's own address, so the administrator's
	 * right password is refused at the engine's login from that address, as
	 * a wrong one is, and no session is made. */
	g_assert_cmpuint(engine_login_status(fx, NULL), ==, 401);
	/* From another address, through a proxy the daemon believes, the same
	 * administrator logs in. */
	reply = send_request(fx->daemon_port, "GET", "/login", NULL, NULL, ADMIN_FROM, NULL, NULL);
	assert_status(&reply, 200);
	token = hidden_value(reply.body, "_csrf");
	if (token == NULL) {
		/* (The engine's builder writes a field's attributes in an order of
		 * its own.) */
		g_autoptr(GRegex) field = g_regex_new("name=\"_csrf\"[^>]*value=\"([^\"]+)\"|value=\"([^\"]+)\"[^>]*name=\"_csrf\"",
			0, 0, NULL);
		g_autoptr(GMatchInfo) match = NULL;

		g_assert_true(g_regex_match(field, reply.body, 0, &match));
		token = g_match_info_fetch(match, 1);
		if (token == NULL || token[0] == '\0') {
			g_free(token);
			token = g_match_info_fetch(match, 2);
		}
	}
	/* (The proxy says https, so the cookies have the prefix.) */
	g_assert_nonnull(g_hash_table_lookup(reply.cookies, "__Host-axis_presession"));
	presession = g_strdup_printf("__Host-axis_presession=%s", (const gchar *)g_hash_table_lookup(reply.cookies,
		"__Host-axis_presession"));
	reply_clear(&reply);
	escaped = g_uri_escape_string(token, NULL, FALSE);
	login = g_strdup_printf("_csrf=%s&username=root&password=%s", escaped, "a+made-up+password+of+root");
	reply = send_request(fx->daemon_port, "POST", "/login", presession, login, ADMIN_FROM, NULL, NULL);
	assert_status(&reply, 303);
	g_assert_nonnull(g_hash_table_lookup(reply.cookies, "__Host-axis_session"));
	session = g_strdup_printf("__Host-axis_session=%s", (const gchar *)g_hash_table_lookup(reply.cookies,
		"__Host-axis_session"));
	reply_clear(&reply);

	reply = send_request(fx->daemon_port, "GET", "/admin/users", session, NULL, ADMIN_FROM, NULL, NULL);
	assert_status(&reply, 200);
	csrf = axis_test_web_csrf_token(reply.body);
	g_assert_nonnull(csrf);
	reply_clear(&reply);
	g_free(escaped);
	escaped = g_uri_escape_string(csrf, NULL, FALSE);
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form[0] != '\0' ? "&" : "", form);
	reply = send_request(fx->daemon_port, "POST", path, session, body, ADMIN_FROM, NULL, NULL);
	if (reply.status != 303 && reply.status != 200)
		g_error("the administrator's change at %s was answered %u:\n%s", path, reply.status, reply.body);
	reply_clear(&reply);
}

static gchar *
read_log(const gchar *path)
{
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec (portal): A restart -- a person is logged in, the portal restarts,
 * and their next page is served without logging in again.  And: the line at
 * start that says the portal is not reviewed for the internet; its settings
 * in the log without its token; --healthcheck. */
static void
test_restart(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *port = NULL;
	g_autofree gchar *form_cookie = NULL;
	g_autofree gchar *form_token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *log = NULL;
	g_autofree gchar *err = NULL;
	const gchar *health_env[3];
	Browser robin;
	Reply reply;

	if (fx->db == NULL)
		return;
	start_portal(fx, NULL, NULL);
	port = g_strdup_printf("%u", (guint)fx->port);
	health_env[0] = "AXIS_PORTAL_WEB_LISTEN_PORT";
	health_env[1] = port;
	health_env[2] = NULL;
	g_assert_cmpint(run_portal(fx, health_env, "--healthcheck", NULL, &err), ==, 0);
	g_clear_pointer(&err, g_free);

	/* Spec (portal): Headers -- a page of the portal carries the security
	 * headers of the engine's interface, compared with what the daemon
	 * itself sends with its login page, and a content security policy of its
	 * own that allows less; over HTTPS, Strict-Transport-Security. */
	{
		static const gchar *const pages[] = { "/login", "/", "/nothing-here", "/static/portal.css", NULL };
		const gchar *const *names = axis_web_security_header_names();
		guint i;
		guint j;

		for (i = 0; names[i] != NULL; i++) {
			g_autofree gchar *engines = header_of(fx->daemon_port, "/login", NULL, names[i]);

			g_assert_nonnull(engines);
			for (j = 0; pages[j] != NULL; j++) {
				g_autofree gchar *portals = header_of(fx->port, pages[j], OUTSIDE, names[i]);

				if (strcmp(names[i], "Content-Security-Policy") == 0)
					g_assert_cmpstr(portals, ==, AXIS_PORTAL_CSP);
				else
					g_assert_cmpstr(portals, ==, engines);
			}
		}
		for (j = 0; pages[j] != NULL; j++) {
			g_autofree gchar *hsts = header_of(fx->port, pages[j], OUTSIDE, "Strict-Transport-Security");

			g_assert_cmpstr(hsts, ==, AXIS_PORTAL_HSTS);
		}
	}

	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	reply = page(fx, "GET", "/requests", robin.session, NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "<span class=\"name\">robin</span>");
	assert_has(reply.body, "You have asked for nothing yet.");
	reply_clear(&reply);
	reply = page(fx, "GET", "/", robin.session, NULL);
	assert_status(&reply, 200);
	reply_clear(&reply);
	/* A login form that is open now. */
	reply = page(fx, "GET", "/login", NULL, NULL);
	form_token = hidden_value(reply.body, "_csrf");
	form_cookie = g_strdup_printf("%s=%s", AXIS_PORTAL_FORM_COOKIE,
		(const gchar *)g_hash_table_lookup(reply.cookies, AXIS_PORTAL_FORM_COOKIE));
	reply_clear(&reply);

	g_assert_cmpint(stop(&fx->portal), ==, 0);
	log = read_log(fx->portal_log);
	assert_has(log, "axis-portal has not been reviewed for exposure to the internet");
	assert_has(log, "starts: url=http://127.0.0.1:");
	assert_has(log, "login: robin from " OUTSIDE);
	assert_lacks(log, fx->token);
	assert_lacks(log, robin.proof);
	assert_lacks(log, robin.value + AXIS_PORTAL_PROOF_LEN + 1);
	assert_lacks(log, ROBIN_PASSWORD);
	g_assert_cmpint(run_portal(fx, health_env, "--healthcheck", NULL, &err), ==, 1);
	g_clear_pointer(&err, g_free);

	/* A new process: it knows nobody, and the person is still logged in,
	 * because the daemon says so. */
	start_portal(fx, NULL, NULL);
	reply = page(fx, "GET", "/requests", robin.session, NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "<span class=\"name\">robin</span>");
	reply_clear(&reply);
	/* The form that was open must be loaded again: its key went with the
	 * process. */
	form = g_strdup_printf("_csrf=%s&username=robin&password=%s", form_token, ROBIN_PASSWORD);
	reply = page(fx, "POST", "/login", form_cookie, form);
	assert_status(&reply, 403);
	reply_clear(&reply);
	g_assert_cmpint(stop(&fx->portal), ==, 0);

	/* The portal is given a new token: the cookies sealed under the old one
	 * are none (the daemon is not asked about them), and the person logs in
	 * again. */
	{
		gchar *token = axis_random_token();
		g_autofree gchar *hash = axis_token_hash(token);
		Browser again;

		insert(fx, "INSERT INTO portal_token (token_hash, created_by) VALUES ('%s', 'root') RETURNING id", hash);
		g_assert_true(g_file_set_contents(fx->token_file, token, -1, NULL));
		axis_secret_free(token);
		/* (The daemon reads the tokens it knows when it starts.) */
		g_assert_cmpint(stop(&fx->daemon), ==, 0);
		start_daemon(fx);
		start_portal(fx, NULL, NULL);
		reply = page(fx, "GET", "/requests", robin.session, NULL);
		assert_status(&reply, 303);
		g_assert_true(g_str_has_prefix(reply.location, "/login"));
		reply_clear(&reply);
		g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &again), ==, 303);
		g_assert_cmpstr(again.value, !=, robin.value);
		reply = page(fx, "GET", "/requests", again.session, NULL);
		assert_status(&reply, 200);
		reply_clear(&reply);
		g_assert_cmpint(stop(&fx->portal), ==, 0);
		browser_clear(&again);
	}
	browser_clear(&robin);
}

/* Spec (portal): An unknown key; Plain HTTP refused -- the real program
 * does not start, and says why. */
static void
test_refuses_to_start(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *config = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *err = NULL;
	const gchar *env[8];

	if (fx->db == NULL)
		return;
	config = g_build_filename(fx->scratch, "axis-portal.conf", NULL);
	text = g_strdup_printf("[portal]\nurl = http://127.0.0.1:%u\ntoken_file = %s\n"
		"database_url = postgresql://axis@db/axis\n", (guint)fx->api_port, fx->token_file);
	g_assert_true(g_file_set_contents(config, text, -1, NULL));
	g_assert_cmpint(run_portal(fx, NULL, "--config", config, &err), ==, 1);
	assert_has(err, "the key 'database_url', which axis-portal does not know");
	g_clear_pointer(&err, g_free);

	env[0] = "AXIS_PORTAL_URL";
	env[1] = "http://192.0.2.10:7801";
	env[2] = "AXIS_PORTAL_TOKEN_FILE";
	env[3] = fx->token_file;
	env[4] = NULL;
	g_assert_cmpint(run_portal(fx, env, NULL, NULL, &err), ==, 1);
	assert_has(err, "refusing to send the portal's token over plain http to 192.0.2.10");
	g_clear_pointer(&err, g_free);

	/* The token is never an argument. */
	g_assert_cmpint(run_portal(fx, env, fx->token, NULL, &err), ==, 1);
	assert_has(err, "the token is never given on the command line");
	assert_lacks(err, fx->token);
	g_clear_pointer(&err, g_free);

	/* Found by the review of P3 (M8): --healthcheck asked 127.0.0.1
	 * whatever the listener was bound to.  A listener on one address that is
	 * not loopback cannot answer it, and it says so. */
	env[0] = "AXIS_PORTAL_WEB_LISTEN_HOST";
	env[1] = "192.0.2.10";
	env[2] = NULL;
	g_assert_cmpint(run_portal(fx, env, "--healthcheck", NULL, &err), ==, 1);
	assert_has(err, "web_listen_host is 192.0.2.10, which is neither a loopback address nor all addresses");
	g_clear_pointer(&err, g_free);
	/* (M10) Nor does the test-only override start on such a listener. */
	env[2] = "AXIS_PORTAL_INSECURE_BROWSER_HTTP";
	env[3] = "true";
	env[4] = NULL;
	g_assert_cmpint(run_portal(fx, env, "--healthcheck", NULL, &err), ==, 1);
	assert_has(err, "is for tests only and is refused unless web_listen_host is a loopback address");
	g_clear_pointer(&err, g_free);
	/* A token the daemon did not give is found out at the first page, not
	 * at start: the portal starts, and its pages are the plain one. */
}

/* Found by the review of P3 (M9): a token file that every user could read
 * was taken in silence, and a token given in the environment stayed in the
 * process's environment for as long as it ran, where the user's other
 * processes can read it (/proc/<pid>/environ).  The first is said at start;
 * the second is overwritten and taken out once read.  Since P4 the process
 * is not dumpable either, so that file is refused to anyone but root; the
 * overwriting is then seen only by a test that runs as root. */
static void
test_token_out_of_sight(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *log = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *environ_text = NULL;
	g_autofree gchar *status_path = NULL;
	g_autofree gchar *status_text = NULL;
	g_autoptr(GError) read_error = NULL;
	const gchar *pid;
	gsize length = 0;
	Browser robin;

	if (fx->db == NULL)
		return;
	g_assert_cmpint(g_chmod(fx->token_file, 0644), ==, 0);
	start_portal(fx, NULL, NULL);
	g_assert_cmpint(stop(&fx->portal), ==, 0);
	log = read_log(fx->portal_log);
	assert_has(log, "the token file can be read by every user of this machine");
	g_clear_pointer(&log, g_free);
	g_assert_cmpint(g_chmod(fx->token_file, 0600), ==, 0);
	start_portal(fx, NULL, NULL);
	g_assert_cmpint(stop(&fx->portal), ==, 0);
	log = read_log(fx->portal_log);
	assert_lacks(log, "can be read by every user");
	g_clear_pointer(&log, g_free);

	fx->token_in_env = TRUE;
	start_portal(fx, NULL, NULL);
	pid = g_subprocess_get_identifier(fx->portal);
	g_assert_nonnull(pid);
	/* Slice P4 (task 4.2): the portal sets no-new-privileges and makes
	 * itself not dumpable at start, so its environment, its memory and the
	 * rest under /proc/<pid> belong to root and its own user's other
	 * processes are refused them. */
	status_path = g_strdup_printf("/proc/%s/status", pid);
	g_assert_true(g_file_get_contents(status_path, &status_text, NULL, NULL));
	assert_has(status_text, "NoNewPrivs:\t1");
	path = g_strdup_printf("/proc/%s/environ", pid);
	if (g_file_get_contents(path, &environ_text, &length, &read_error)) {
		/* Read all the same (the test runs as root): what was read is the
		 * portal's environment, and the token is not in it. */
		g_assert_cmpuint(geteuid(), ==, 0);
		g_assert_nonnull(memmem(environ_text, length, "AXIS_PORTAL_URL=http://127.0.0.1:",
			strlen("AXIS_PORTAL_URL=http://127.0.0.1:")));
		g_assert_null(memmem(environ_text, length, fx->token, strlen(fx->token)));
	} else {
		g_assert_error(read_error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
		g_clear_pointer(&path, g_free);
		path = g_strdup_printf("/proc/%s/mem", pid);
		g_assert_cmpint(g_open(path, O_RDONLY, 0), ==, -1);
		g_assert_cmpint(errno, ==, EACCES);
	}
	/* The portal has it where it needs it. */
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	browser_clear(&robin);
	g_assert_cmpint(stop(&fx->portal), ==, 0);
	fx->token_in_env = FALSE;
}

/* Spec (portal): Disabled while logged in -- the requester's next request,
 * page or post, is sent to /login, and no request can be made in between.
 * And: A role change. */
static void
test_account_changes(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *title = NULL;
	g_autofree gchar *ask = NULL;
	g_autofree gchar *robin_csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *path = NULL;
	Browser robin;
	Browser sam;
	Reply reply;

	if (fx->db == NULL)
		return;
	start_portal(fx, NULL, NULL);
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	g_assert_cmpuint(portal_login(fx, "sam", SAM_PASSWORD, "203.0.113.6", NULL, &sam), ==, 303);

	/* robin has the title's page open, with its form. */
	title = g_strdup_printf("/title/%" G_GINT64_FORMAT, fx->film);
	ask = g_strconcat(title, "/request", NULL);
	reply = page(fx, "GET", title, robin.session, NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "The Lantern Orchard");
	assert_has(reply.body, "Request this movie");
	robin_csrf = hidden_value(reply.body, "_csrf");
	reply_clear(&reply);
	form = g_strdup_printf("_csrf=%s&kind=movie", robin_csrf);

	/* The administrator disables robin. */
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable", fx->robin);
	admin_change(fx, path, "");
	reply = page(fx, "POST", ask, robin.session, form);
	assert_status(&reply, 303);
	g_assert_cmpstr(reply.location, ==, "/login");
	/* The cookie goes. */
	g_assert_cmpstr(g_hash_table_lookup(reply.cookies, AXIS_PORTAL_SESSION_COOKIE), ==, "");
	reply_clear(&reply);
	g_assert_cmpint(insert(fx, "SELECT count(*) FROM request"), ==, 0);
	reply = page(fx, "GET", "/requests", robin.session, NULL);
	assert_status(&reply, 303);
	g_assert_true(g_str_has_prefix(reply.location, "/login"));
	reply_clear(&reply);
	/* And cannot log in again: the one answer of a login that fails. */
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, NULL), ==, 401);

	/* sam is still logged in, until made an ordinary user. */
	reply = page(fx, "GET", "/requests", sam.session, NULL);
	assert_status(&reply, 200);
	reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/role", fx->sam);
	admin_change(fx, path, "role=user&password=a-new-password-of-sams");
	reply = page(fx, "GET", "/requests", sam.session, NULL);
	assert_status(&reply, 303);
	g_assert_true(g_str_has_prefix(reply.location, "/login"));
	reply_clear(&reply);
	/* An ordinary user's password opens nothing at the portal. */
	g_assert_cmpuint(portal_login(fx, "sam", "a-new-password-of-sams", "203.0.113.6", NULL, NULL), ==, 401);
	g_assert_cmpuint(portal_login(fx, "root", "a+made-up+password+of+root", "203.0.113.7", NULL, NULL), ==, 401);

	g_assert_cmpint(stop(&fx->portal), ==, 0);
	browser_clear(&robin);
	browser_clear(&sam);
}

/* Spec (portal): Logout -- a person logs out, and their old cookie is
 * treated as no session, by the portal and by the daemon. */
static void
test_logout(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *json = NULL;
	Browser robin;
	Reply reply;

	if (fx->db == NULL)
		return;
	start_portal(fx, NULL, NULL);
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	csrf = csrf_of(fx, &robin);
	json = g_strdup_printf("{\"proof\":\"%s\"}", robin.proof);
	/* The proof is good at the daemon, with the portal's token. */
	reply = send_request(fx->api_port, "POST", AXIS_PORTAL_API_PREFIX "whoami", NULL, NULL, NULL, fx->token, json);
	assert_status(&reply, 200);
	reply_clear(&reply);

	form = g_strdup_printf("_csrf=%s", csrf);
	reply = page(fx, "POST", "/logout", robin.session, form);
	assert_status(&reply, 303);
	g_assert_cmpstr(reply.location, ==, "/login");
	g_assert_cmpstr(g_hash_table_lookup(reply.cookies, AXIS_PORTAL_SESSION_COOKIE), ==, "");
	reply_clear(&reply);

	reply = page(fx, "GET", "/requests", robin.session, NULL);
	assert_status(&reply, 303);
	g_assert_true(g_str_has_prefix(reply.location, "/login"));
	reply_clear(&reply);
	reply = send_request(fx->api_port, "POST", AXIS_PORTAL_API_PREFIX "whoami", NULL, NULL, NULL, fx->token, json);
	assert_status(&reply, 401);
	reply_clear(&reply);

	g_assert_cmpint(stop(&fx->portal), ==, 0);
	browser_clear(&robin);
}

/* Spec (portal): The owner still gets in -- robin logged in from a browser
 * once; a stranger then fails for robin until the username is delayed, and
 * is refused even with the right password, while robin logs in from the
 * remembered browser at once.  And: An altered device value -- one that was
 * changed, or that belongs to another account, is refused like none. */
static void
test_devices(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *altered = NULL;
	Browser robin;
	Browser sam;
	Browser again;
	gboolean delayed = FALSE;
	guint i;

	if (fx->db == NULL)
		return;
	start_portal(fx, NULL, NULL);
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	g_assert_cmpuint(portal_login(fx, "sam", SAM_PASSWORD, "203.0.113.6", NULL, &sam), ==, 303);
	g_assert_nonnull(robin.device);
	g_assert_nonnull(sam.device);
	g_assert_cmpstr(robin.device, !=, sam.device);

	/* A stranger guesses robin's password, each time from another address
	 * (so that it is the daemon's count for the name that refuses, not the
	 * portal's for an address). */
	for (i = 0; i < 12 && !delayed; i++) {
		g_autofree gchar *from = g_strdup_printf("198.51.100.%u", 20 + i);
		guint status = portal_login(fx, "robin", "not-robins-password", from, NULL, NULL);

		if (status == 429)
			delayed = TRUE;
		else
			g_assert_cmpuint(status, ==, 401);
	}
	g_assert_true(delayed);
	/* The right password, from a browser the daemon does not remember. */
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, "198.51.100.60", NULL, NULL), ==, 429);
	/* A device value that was changed, and another account's. */
	altered = g_strdup(robin.device);
	altered[strlen(altered) - 1] = altered[strlen(altered) - 1] == 'A' ? 'B' : 'A';
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, "198.51.100.61", altered, NULL), ==, 429);
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, "198.51.100.62", sam.device, NULL), ==, 429);
	/* robin, from the remembered browser: at once. */
	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, robin.device, &again), ==, 303);
	g_assert_nonnull(again.session);

	g_assert_cmpint(stop(&fx->portal), ==, 0);
	{
		g_autofree gchar *log = read_log(fx->portal_log);

		/* No device value, changed or not, is in the log. */
		assert_lacks(log, strchr(robin.device, '=') + 1);
		assert_lacks(log, strchr(altered, '=') + 1);
		assert_lacks(log, "not-robins-password");
	}
	browser_clear(&robin);
	browser_clear(&sam);
	browser_clear(&again);
}

/* Spec (portal): Nothing writable -- the portal runs where it can write no
 * file at all, people log in, search and ask, everything works, and no file
 * was created. */
static void
test_nothing_writable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *nowhere = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *ask = NULL;
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *request = NULL;
	g_autofree gchar *withdraw = NULL;
	g_autofree gchar *log = NULL;
	g_autoptr(GDir) dir = NULL;
	ChildRules rules;
	Browser robin;
	Reply reply;

	if (fx->db == NULL)
		return;
	/* A directory nobody can write to is all the portal is told of: where
	 * it runs, its home, its place for temporary files.  And, where the
	 * kernel has Landlock, the process is denied every write to every file
	 * system (the test's own strictest stand-in for a read-only root with
	 * no volume and no tmpfs). */
	/* Found by the review of P3 (M15): without Landlock this passed on a
	 * directory of mode 0555 alone, which root (an image build) can write
	 * to.  Without Landlock it is skipped, and says so. */
	rules.abi = landlock_abi();
	if (rules.abi == 0) {
		g_test_skip("no Landlock in this kernel: nothing here can deny the portal every write");
		return;
	}
	nowhere = g_build_filename(fx->scratch, "nowhere", NULL);
	g_assert_cmpint(g_mkdir(nowhere, 0555), ==, 0);
	start_portal(fx, &rules, nowhere);

	g_assert_cmpuint(portal_login(fx, "robin", ROBIN_PASSWORD, OUTSIDE, NULL, &robin), ==, 303);
	reply = page(fx, "GET", "/", robin.session, NULL);
	assert_status(&reply, 200);
	reply_clear(&reply);
	/* (This daemon has no details provider: the search answers from what
	 * is stored, which is nothing, and says so.) */
	reply = page(fx, "GET", "/search?q=lantern", robin.session, NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "The details provider did not answer.");
	reply_clear(&reply);
	title = g_strdup_printf("/title/%" G_GINT64_FORMAT, fx->film);
	ask = g_strconcat(title, "/request", NULL);
	reply = page(fx, "GET", title, robin.session, NULL);
	assert_status(&reply, 200);
	csrf = hidden_value(reply.body, "_csrf");
	reply_clear(&reply);
	form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	reply = page(fx, "POST", ask, robin.session, form);
	assert_status(&reply, 200);
	assert_has(reply.body, "Your request is made. It waits for the administrator.");
	reply_clear(&reply);
	g_assert_cmpint(insert(fx, "SELECT count(*) FROM request WHERE decision = 'waiting' AND via = 'portal'"), ==, 1);

	reply = page(fx, "GET", "/requests", robin.session, NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "The Lantern Orchard");
	assert_has(reply.body, "Waiting for approval");
	request = g_strdup(strstr(reply.body, "action=\"/requests/"));
	g_assert_nonnull(request);
	withdraw = g_strndup(request + strlen("action=\""), strcspn(request + strlen("action=\""), "\""));
	reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("_csrf=%s", csrf);
	reply = page(fx, "POST", withdraw, robin.session, form);
	assert_status(&reply, 200);
	assert_has(reply.body, "The request is withdrawn.");
	reply_clear(&reply);
	g_assert_cmpint(insert(fx, "SELECT count(*) FROM request WHERE decision = 'withdrawn'"), ==, 1);
	reply = page(fx, "GET", "/art/1", robin.session, NULL);
	assert_status(&reply, 404);
	reply_clear(&reply);

	g_assert_cmpint(stop(&fx->portal), ==, 0);
	/* Nothing was made where it ran, and it never asked to write. */
	dir = g_dir_open(nowhere, 0, NULL);
	g_assert_nonnull(dir);
	g_assert_null(g_dir_read_name(dir));
	log = read_log(fx->portal_log);
	assert_lacks(log, "Permission denied");
	assert_lacks(log, "Read-only file system");
	assert_has(log, "request: robin asked for title");
	assert_has(log, "request: robin withdrew a request");
	g_chmod(nowhere, 0755);
	browser_clear(&robin);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/portal/process/restart", Fixture, NULL, fixture_setup, test_restart, fixture_teardown);
	g_test_add("/portal/process/refuses-to-start", Fixture, NULL, fixture_setup, test_refuses_to_start,
		fixture_teardown);
	g_test_add("/portal/process/token-out-of-sight", Fixture, NULL, fixture_setup, test_token_out_of_sight,
		fixture_teardown);
	g_test_add("/portal/process/disabled-while-logged-in", Fixture, NULL, fixture_setup, test_account_changes,
		fixture_teardown);
	g_test_add("/portal/process/logout", Fixture, NULL, fixture_setup, test_logout, fixture_teardown);
	g_test_add("/portal/process/owner-gets-in", Fixture, NULL, fixture_setup, test_devices, fixture_teardown);
	g_test_add("/portal/process/nothing-writable", Fixture, NULL, fixture_setup, test_nothing_writable,
		fixture_teardown);

	return g_test_run();
}
