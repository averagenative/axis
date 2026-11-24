/*
 * axis-test-worker.c - test support for the worker API and axis-worker
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-worker.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-convert-caps.h"
#include "web/axis-web.h"
#include "web/axis-password.h"

/* ── The daemon's side ──────────────────────────────────────────── */

/* As axisd does: the pages that wait for a worker hear of a registration. */
static void
on_registered(
	const gchar *worker,
	gpointer     user_data
){
	axis_web_workers_note_registered(user_data, worker);
}

static AxisTestDaemon *
daemon_new(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies,
	gboolean                   with_hub
);

AxisTestDaemon *
axis_test_daemon_new(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies
){
	return daemon_new(pool, estimator, timing, trusted_proxies, FALSE);
}

AxisTestDaemon *
axis_test_daemon_new_with_hub(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies
){
	return daemon_new(pool, estimator, timing, trusted_proxies, TRUE);
}

static AxisTestDaemon *
daemon_new(
	AxisDbPool                *pool,
	AxisEstimator             *estimator,
	const AxisWorkerApiTiming *timing,
	const gchar               *trusted_proxies,
	gboolean                   with_hub
){
	AxisTestDaemon *self = g_new0(AxisTestDaemon, 1);
	g_autoptr(GError) error = NULL;
	AxisWorkerApiTiming defaults;

	g_assert_true(axis_password_init());
	self->pool = pool;
	self->registry = axis_job_registry_new();
	g_assert_true(axis_worker_api_register_kinds(self->registry, NULL, &error));
	g_assert_no_error(error);
	self->tokens = axis_worker_tokens_new();
	self->modules = axis_module_registry_new();
	self->workers_ui = axis_web_workers_new(pool, self->tokens, estimator, 500);
	axis_web_workers_set_kinds(self->workers_ui, self->registry);
	g_assert_true(axis_module_registry_register(self->modules, axis_web_workers_get_module(), self->workers_ui,
		&error));
	g_assert_no_error(error);
	self->auth = axis_web_auth_new(NULL, trusted_proxies != NULL ? trusted_proxies : "", AXIS_SECURE_COOKIES_AUTO,
		&error);
	g_assert_no_error(error);
	axis_web_auth_set_worker_tokens(self->auth, self->tokens);
	self->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(self->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	self->web = axis_web_new(self->server, self->modules, self->auth);
	if (with_hub) {
		self->hub = axis_web_hub_new(NULL);
		axis_web_hub_install(self->hub, self->web, self->auth);
	}
	self->api = axis_worker_api_new(pool, self->registry, self->tokens, estimator);
	axis_worker_api_set_registered(self->api, on_registered, self->workers_ui);
	if (timing == NULL) {
		memset(&defaults, 0, sizeof(defaults));
		defaults.lease_ms = 2000;
		defaults.poll_ms = 200;
		defaults.reap_ms = 50;
		defaults.backoff_base_ms = 50;
		defaults.backoff_cap_ms = 100;
		timing = &defaults;
	}
	axis_worker_api_set_timing(self->api, timing);
	axis_worker_api_register_routes(self->api, self->web);
	g_assert_true(htmx_server_start(self->server, &error));
	g_assert_no_error(error);
	g_assert_true(axis_module_registry_start_all(self->modules, &error));
	g_assert_no_error(error);
	axis_worker_api_start(self->api);
	self->port = axis_test_web_port(self->server);
	self->url = g_strdup_printf("http://127.0.0.1:%u", (guint)self->port);
	self->session = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL, "timeout", 20u, NULL);
	return self;
}

void
axis_test_daemon_free(AxisTestDaemon *self)
{
	if (self == NULL)
		return;
	g_object_unref(self->session);
	htmx_server_stop(self->server);
	axis_worker_api_stop(self->api);
	axis_web_workers_stop(self->workers_ui);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_module_registry_stop_all(self->modules);
	if (self->hub != NULL)
		axis_web_hub_free(self->hub);
	axis_web_free(self->web);
	g_object_unref(self->server);
	axis_worker_api_free(self->api);
	axis_web_auth_free(self->auth);
	axis_module_registry_free(self->modules);
	axis_web_workers_free(self->workers_ui);
	axis_worker_tokens_free(self->tokens);
	axis_job_registry_unref(self->registry);
	g_free(self->url);
	g_free(self);
}

gchar *
axis_test_daemon_add_worker(
	AxisTestDaemon *self,
	const gchar    *name,
	gint64         *id
){
	g_autoptr(GError) error = NULL;
	gchar *token = NULL;

	g_assert_true(axis_worker_store_create(self->pool, self->tokens, name, "root", &token, id, &error));
	g_assert_no_error(error);
	return token;
}

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} Pending;

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	Pending *pending = data;

	pending->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	g_main_loop_quit(pending->loop);
}

AxisTestReply
axis_test_daemon_post(
	AxisTestDaemon     *self,
	const gchar        *method,
	const gchar        *request,
	const gchar        *token,
	const gchar        *content_type,
	const gchar        *body,
	const gchar *const *extra
){
	g_autofree gchar *path = request[0] == '/' ? g_strdup(request) : g_strconcat(AXIS_WORKER_API_PREFIX, request, NULL);
	g_autofree gchar *bearer = token != NULL ? g_strconcat("Bearer ", token, NULL) : NULL;
	g_autofree gchar *url = g_strdup_printf("%s%s", self->url, path);
	SoupMessageHeaders *headers;
	AxisTestReply reply;
	Pending pending;
	gconstpointer data;
	gsize size = 0;
	guint i;

	memset(&reply, 0, sizeof(reply));
	reply.message = soup_message_new(method, url);
	g_assert_nonnull(reply.message);
	soup_message_add_flags(reply.message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(reply.message);
	if (bearer != NULL)
		soup_message_headers_replace(headers, "Authorization", bearer);
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, extra[i], extra[i + 1]);
	if (body != NULL) {
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(reply.message,
			content_type != NULL ? content_type : "application/json", payload);
	}
	memset(&pending, 0, sizeof(pending));
	pending.loop = g_main_loop_new(NULL, FALSE);
	soup_session_send_and_read_async(self->session, reply.message, G_PRIORITY_DEFAULT, NULL, on_sent, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);
	g_assert_no_error(pending.error);
	data = g_bytes_get_data(pending.bytes, &size);
	reply.status = soup_message_get_status(reply.message);
	reply.body = g_malloc(size + 1);
	if (size > 0)
		memcpy(reply.body, data, size);
	reply.body[size] = '\0';
	reply.length = size;
	g_bytes_unref(pending.bytes);
	return reply;
}

gchar *
axis_test_register_json(
	gboolean            no_x265,
	const gchar *const *volumes
){
	g_autoptr(AxisWorkerRegister) request = axis_worker_register_new();
	AxisEncoderCapabilities *caps = axis_encoder_capabilities_new();
	guint i;

	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	caps->aac = TRUE;
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), !no_x265, !no_x265, FALSE,
		no_x265 ? "not usable here" : NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, FALSE, FALSE, NULL);
	request->worker_version = g_strdup("0.1.0");
	request->max_jobs = 1;
	request->caps = caps;
	for (i = 0; volumes != NULL && volumes[i] != NULL; i += 2) {
		AxisWorkerVolumeSeen *seen = g_new0(AxisWorkerVolumeSeen, 1);

		seen->uuid = g_strdup(volumes[i]);
		seen->path = g_strdup(volumes[i + 1]);
		seen->keeps_identity = TRUE;
		g_ptr_array_add(request->volumes, seen);
	}
	return axis_worker_register_to_json(request);
}

gchar *
axis_test_claim_json(const gchar *const *volumes)
{
	static const gchar *const kinds[] = { AXIS_WORKER_KIND_CAPABILITIES, AXIS_WORKER_KIND_SAMPLE, NULL };
	static const gchar *const none[] = { NULL };
	AxisWorkerClaim claim;

	memset(&claim, 0, sizeof(claim));
	claim.kinds = (gchar **)kinds;
	claim.volumes = (gchar **)(volumes != NULL ? volumes : none);
	return axis_worker_claim_to_json(&claim);
}

/* ── axis-worker processes ──────────────────────────────────────── */

struct _AxisTestWorker
{
	GSubprocess  *process;
	GInputStream *err;
	GString      *log;
	gchar        *dir;
	gchar        *scratch;
	gchar        *config;
	gboolean      reading;
	gboolean      exited;
	gint          status;
	gchar         buffer[4096];
};

static gchar *shared_state;
static gchar *shared_parent;

const gchar *
axis_test_worker_state_dir(void)
{
	if (shared_state == NULL) {
		shared_parent = axis_test_storage_dir_new();
		shared_state = g_build_filename(shared_parent, "state", NULL);
		g_assert_cmpint(g_mkdir(shared_state, 0700), ==, 0);
	}
	return shared_state;
}

void
axis_test_worker_cleanup(void)
{
	if (shared_parent != NULL)
		axis_test_storage_dir_free(shared_parent);
	g_clear_pointer(&shared_state, g_free);
	shared_parent = NULL;
}

static void
child_setup(gpointer data)
{
	/* A test that dies takes its workers with it. */
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	AxisTestWorker *self = data;
	g_autoptr(GError) error = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

	if (n <= 0) {
		self->reading = FALSE;
		return;
	}
	g_string_append_len(self->log, self->buffer, n);
	if (g_test_verbose())
		g_printerr("%.*s", (int)n, self->buffer);
	g_input_stream_read_async(self->err, self->buffer, sizeof(self->buffer), G_PRIORITY_DEFAULT, NULL, on_read, self);
}

static void
on_exited(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	AxisTestWorker *self = data;

	(void)g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL);
	self->exited = TRUE;
	self->status = g_subprocess_get_if_exited(self->process) ? g_subprocess_get_exit_status(self->process) : -1;
}

/* Starts @argv with no AXIS_* variable of the test's environment, then
 * @env's, reading its standard error into its log. */
static void
spawn(
	AxisTestWorker     *self,
	const gchar *const *argv,
	const gchar *const *env
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	gchar **names = g_listenv();
	guint i;

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	for (i = 0; names[i] != NULL; i++) {
		if (g_str_has_prefix(names[i], "AXIS_"))
			g_subprocess_launcher_unsetenv(launcher, names[i]);
	}
	g_strfreev(names);
	for (i = 0; env != NULL && env[i] != NULL; i += 2)
		g_subprocess_launcher_setenv(launcher, env[i], env[i + 1], TRUE);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
	self->process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	self->err = g_subprocess_get_stderr_pipe(self->process);
	self->reading = TRUE;
	g_input_stream_read_async(self->err, self->buffer, sizeof(self->buffer), G_PRIORITY_DEFAULT, NULL, on_read, self);
	g_subprocess_wait_async(self->process, NULL, on_exited, self);
}

AxisTestWorker *
axis_test_worker_start(const AxisTestWorkerOptions *options)
{
	g_autoptr(GString) config = g_string_new("[worker]\n");
	g_autofree gchar *token_file = NULL;
	g_autofree gchar *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree gchar *ffprobe = g_find_program_in_path("ffprobe");
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	AxisTestWorker *self = g_new0(AxisTestWorker, 1);
	guint i;

	self->log = g_string_new(NULL);
	self->status = -1;
	self->dir = axis_test_storage_dir_new();
	self->scratch = options->scratch != NULL ? g_strdup(options->scratch) :
		g_build_filename(self->dir, "scratch", NULL);
	token_file = g_build_filename(self->dir, "token", NULL);
	self->config = g_build_filename(self->dir, "worker.conf", NULL);
	g_assert_true(g_file_set_contents(token_file, options->token != NULL ? options->token : "", -1, NULL));
	g_assert_cmpint(g_chmod(token_file, 0600), ==, 0);

	g_string_append_printf(config, "url = %s\n", options->url);
	g_string_append_printf(config, "token_file = %s\n", token_file);
	g_string_append_printf(config, "scratch = %s\n", self->scratch);
	g_string_append_printf(config, "state = %s\n", axis_test_worker_state_dir());
	/* Never a GPU from a test. */
	g_string_append(config, "encoder_families = software\n");
	g_string_append(config, "request_timeout = 5\n");
	{
		gchar number[G_ASCII_DTOSTR_BUF_SIZE];

		g_ascii_dtostr(number, sizeof(number), options->volume_check > 0 ? options->volume_check : 0.2);
		g_string_append_printf(config, "volume_check_interval = %s\n", number);
	}
	if (options->volumes != NULL && options->volumes[0] != NULL) {
		g_string_append(config, "volumes = ");
		for (i = 0; options->volumes[i] != NULL; i++)
			g_string_append_printf(config, "%s%s", i > 0 ? ";" : "", options->volumes[i]);
		g_string_append_c(config, '\n');
	}
	g_string_append_printf(config, "ffmpeg = %s\n", options->ffmpeg != NULL ? options->ffmpeg :
		(ffmpeg != NULL ? ffmpeg : "/usr/bin/ffmpeg"));
	g_string_append_printf(config, "ffprobe = %s\n", ffprobe != NULL ? ffprobe : "/usr/bin/ffprobe");
	g_string_append(config, "log_level = debug\n");
	if (options->extra_config != NULL)
		g_string_append(config, options->extra_config);
	g_assert_true(g_file_set_contents(self->config, config->str, -1, NULL));

	g_ptr_array_add(argv, (gpointer)AXIS_WORKER_PATH);
	g_ptr_array_add(argv, (gpointer)"--config");
	g_ptr_array_add(argv, self->config);
	for (i = 0; options->args != NULL && options->args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)options->args[i]);
	g_ptr_array_add(argv, NULL);
	spawn(self, (const gchar *const *)argv->pdata, options->env);
	return self;
}

AxisTestWorker *
axis_test_process_spawn(
	const gchar *const *argv,
	const gchar *const *env
){
	AxisTestWorker *self = g_new0(AxisTestWorker, 1);

	self->log = g_string_new(NULL);
	self->status = -1;
	self->dir = axis_test_storage_dir_new();
	self->scratch = g_build_filename(self->dir, "scratch", NULL);
	self->config = g_build_filename(self->dir, "none", NULL);
	spawn(self, argv, env);
	return self;
}

typedef struct
{
	AxisTestWorker *worker;
	const gchar    *needle;
} LogWait;

static gboolean
log_has(gpointer data)
{
	LogWait *wait = data;

	return strstr(wait->worker->log->str, wait->needle) != NULL;
}

gboolean
axis_test_worker_wait_log(
	AxisTestWorker *self,
	const gchar    *needle,
	guint           timeout_ms
){
	LogWait wait;

	wait.worker = self;
	wait.needle = needle;
	return axis_test_pump_until(log_has, &wait, timeout_ms);
}

static gboolean
has_exited(gpointer data)
{
	AxisTestWorker *self = data;

	return self->exited && !self->reading;
}

gboolean
axis_test_worker_wait_exit(
	AxisTestWorker *self,
	guint           timeout_ms
){
	return axis_test_pump_until(has_exited, self, timeout_ms);
}

void
axis_test_worker_signal(
	AxisTestWorker *self,
	gint            signal_number
){
	if (!self->exited)
		g_subprocess_send_signal(self->process, signal_number);
}

GPid
axis_test_worker_get_pid(AxisTestWorker *self)
{
	const gchar *id = g_subprocess_get_identifier(self->process);

	return id != NULL ? (GPid)g_ascii_strtoll(id, NULL, 10) : 0;
}

gint
axis_test_worker_get_status(AxisTestWorker *self)
{
	return self->exited ? self->status : -1;
}

const gchar *
axis_test_worker_get_log(AxisTestWorker *self)
{
	return self->log->str;
}

const gchar *
axis_test_worker_get_scratch(AxisTestWorker *self)
{
	return self->scratch;
}

const gchar *
axis_test_worker_get_config(AxisTestWorker *self)
{
	return self->config;
}

gboolean
axis_test_worker_scratch_empty(AxisTestWorker *self)
{
	g_autoptr(GDir) dir = g_dir_open(self->scratch, 0, NULL);

	return dir == NULL || g_dir_read_name(dir) == NULL;
}

void
axis_test_worker_free(AxisTestWorker *self)
{
	if (self == NULL)
		return;
	if (!self->exited) {
		g_subprocess_force_exit(self->process);
		(void)axis_test_worker_wait_exit(self, AXIS_TEST_WAIT_MS);
	}
	g_assert_null(strstr(self->log->str, "Sanitizer"));
	g_assert_null(strstr(self->log->str, "runtime error"));
	g_object_unref(self->process);
	g_string_free(self->log, TRUE);
	axis_test_storage_dir_free(self->dir);
	g_free(self->scratch);
	g_free(self->config);
	g_free(self);
}

gboolean
axis_test_process_alive(GPid pid)
{
	g_autofree gchar *path = g_strdup_printf("/proc/%d/stat", (int)pid);
	g_autofree gchar *text = NULL;
	const gchar *close_paren;

	if (pid <= 0 || !g_file_get_contents(path, &text, NULL, NULL))
		return FALSE;
	/* "pid (comm) S ...": the state after the last ')'; a zombie or a
	 * process being torn down is gone. */
	close_paren = strrchr(text, ')');
	if (close_paren == NULL || close_paren[1] != ' ')
		return FALSE;
	return close_paren[2] != 'Z' && close_paren[2] != 'X' && close_paren[2] != 'x';
}

GPid
axis_test_read_pid(const gchar *path)
{
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;
	return (GPid)g_ascii_strtoll(g_strstrip(text), NULL, 10);
}

GPid
axis_test_find_child(
	GPid         parent,
	const gchar *comm
){
	g_autoptr(GDir) proc = g_dir_open("/proc", 0, NULL);
	const gchar *name;

	if (proc == NULL)
		return 0;
	while ((name = g_dir_read_name(proc)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		g_auto(GStrv) fields = NULL;
		const gchar *open_paren;
		const gchar *close_paren;
		GPid pid;

		if (!g_ascii_isdigit(name[0]))
			continue;
		path = g_strdup_printf("/proc/%s/stat", name);
		if (!g_file_get_contents(path, &text, NULL, NULL))
			continue;
		/* "pid (comm) S ppid ..." */
		open_paren = strchr(text, '(');
		close_paren = strrchr(text, ')');
		if (open_paren == NULL || close_paren == NULL || close_paren < open_paren || close_paren[1] != ' ')
			continue;
		if ((gsize)(close_paren - open_paren - 1) != strlen(comm) ||
		    strncmp(open_paren + 1, comm, strlen(comm)) != 0)
			continue;
		fields = g_strsplit(close_paren + 2, " ", 3);
		if (fields[0] == NULL || fields[1] == NULL || fields[0][0] == 'Z' || fields[0][0] == 'X')
			continue;
		if ((GPid)g_ascii_strtoll(fields[1], NULL, 10) != parent)
			continue;
		pid = (GPid)g_ascii_strtoll(name, NULL, 10);
		return pid;
	}
	return 0;
}

gboolean
axis_test_worker_seed_caps(gboolean no_x265)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ffmpeg = g_find_program_in_path("ffmpeg");
	g_autofree gchar *ffprobe = g_find_program_in_path("ffprobe");
	g_autofree gchar *version = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	const gchar *ffmpeg_argv[2];
	const gchar *ffprobe_argv[2];
	AxisCapabilityTool tool;
	guint skip;

	if (ffmpeg == NULL || ffprobe == NULL)
		return FALSE;
	ffmpeg_argv[0] = ffmpeg;
	ffmpeg_argv[1] = NULL;
	ffprobe_argv[0] = ffprobe;
	ffprobe_argv[1] = NULL;
	memset(&tool, 0, sizeof(tool));
	tool.ffmpeg = ffmpeg_argv;
	tool.ffprobe = ffprobe_argv;
	tool.scratch = axis_test_worker_state_dir();
	if (!axis_convert_tool_identity(&tool, &version, &digest, &error))
		return FALSE;
	caps = axis_encoder_capabilities_new();
	caps->tool_version = g_strdup(version);
	caps->tool_digest = g_strdup(digest);
	caps->measured_at_us = g_get_real_time();
	caps->aac = TRUE;
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, FALSE, FALSE, NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), !no_x265, !no_x265, FALSE,
		no_x265 ? "not usable here" : NULL);
	/* The worker of a test runs software encoders only: these are the
	 * families it skips. */
	skip = AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_NVENC) | AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_VAAPI);
	name = g_strdup_printf("capabilities-%u.json", skip);
	path = g_build_filename(axis_test_worker_state_dir(), name, NULL);
	json = axis_worker_tool_to_json(caps);
	g_assert_true(g_file_set_contents(path, json, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0600), ==, 0);
	return TRUE;
}

gchar *
axis_test_slow_ffmpeg(
	const gchar *dir,
	const gchar *flag
){
	g_autofree gchar *path = g_build_filename(dir, "slow-ffmpeg.sh", NULL);
	static const gchar script[] =
		"# A stand-in for ffmpeg: the real one, unless the flag file is there.\n"
		"flag=$1\n"
		"shift\n"
		"for a in \"$@\"; do\n"
		"\tif [ \"$a\" = -version ]; then exec ffmpeg \"$@\"; fi\n"
		"done\n"
		"if [ -e \"$flag\" ]; then exec sleep 600; fi\n"
		"exec ffmpeg \"$@\"\n";

	g_assert_true(g_file_set_contents(path, script, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return g_strdup_printf("/bin/sh;%s;%s", path, flag);
}

gchar *
axis_test_failing_ffmpeg(
	const gchar *dir,
	const gchar *flag
){
	g_autofree gchar *path = g_build_filename(dir, "failing-ffmpeg.sh", NULL);
	static const gchar script[] =
		"# A stand-in for ffmpeg: the real one, unless the flag file is there;\n"
		"# then it fails as the file says (\"fail\": an exit with an error, as for\n"
		"# input it cannot read; a signal's name: it dies of that signal).\n"
		"flag=$1\n"
		"shift\n"
		"for a in \"$@\"; do\n"
		"\tif [ \"$a\" = -version ]; then exec ffmpeg \"$@\"; fi\n"
		"done\n"
		"if [ -e \"$flag\" ]; then\n"
		"\thow=$(cat \"$flag\")\n"
		"\tif [ \"$how\" = fail ]; then\n"
		"\t\techo 'Invalid data found when processing input' >&2\n"
		"\t\texit 1\n"
		"\tfi\n"
		"\techo 'a fault in the encoder' >&2\n"
		"\tkill -s \"$how\" $$\n"
		"\tsleep 5\n"
		"\texit 1\n"
		"fi\n"
		"exec ffmpeg \"$@\"\n";

	g_assert_true(g_file_set_contents(path, script, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return g_strdup_printf("/bin/sh;%s;%s", path, flag);
}

gchar *
axis_test_paced_ffmpeg(
	const gchar *dir,
	const gchar *slow,
	const gchar *hold
){
	g_autofree gchar *path = g_build_filename(dir, "paced-ffmpeg.sh", NULL);
	static const gchar script[] =
		"# A stand-in for ffmpeg: the real one, held back or reading slowly.\n"
		"slow=$1\n"
		"hold=$2\n"
		"shift 2\n"
		"for a in \"$@\"; do\n"
		"\tif [ \"$a\" = -version ]; then exec ffmpeg \"$@\"; fi\n"
		"done\n"
		"while [ -e \"$hold\" ]; do sleep 0.1; done\n"
		"if [ -e \"$slow\" ]; then exec ffmpeg -readrate 0.02 \"$@\"; fi\n"
		"exec ffmpeg \"$@\"\n";

	g_assert_true(g_file_set_contents(path, script, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return g_strdup_printf("/bin/sh;%s;%s;%s", path, slow, hold);
}
