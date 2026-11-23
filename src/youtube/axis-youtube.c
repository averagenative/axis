/*
 * axis-youtube.c - the YouTube downloader: lifetime, settings, the switch and
 * what holds YouTube work back, the one command, a look at a video, the
 * records, and the built-in download kind
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "youtube/axis-youtube-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/axis-outbound.h"
#include "core/axis-size.h"
#include "db/axis-db-conn.h"
#include "import/axis-import.h"
#include "import/axis-import-name.h"
#include "jobs/axis-job-lifecycle.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-guard.h"

G_DEFINE_QUARK(axis-youtube-error-quark, axis_youtube_error)

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Settings ───────────────────────────────────────────────────── */

static const gchar *const height_choices[] = { "480", "720", "1080", "2160", NULL };
static const gchar *const runtime_choices[] = { "node", "deno", "default", NULL };
static const gchar *const runtime_labels[] = { "Node.js", "Deno", "Whatever yt-dlp finds", NULL };

static const AxisSettingInfo setting_table[] = {
	{ AXIS_YOUTUBE_SETTING_PROGRAM, AXIS_SETTING_STRING, "yt-dlp program",
	  "The yt-dlp that downloads from YouTube: a name found on Axis's PATH, or an absolute path, such as a newer "
	  "copy you put in a mounted folder. Axis runs it with a fixed set of options and never updates it by itself.",
	  0, "yt-dlp", 1, 255, 0, 0, NULL },
	{ AXIS_YOUTUBE_SETTING_HEIGHT, AXIS_SETTING_CHOICE, "Largest picture height",
	  "The tallest picture taken, in lines. A video is taken as YouTube serves it, H.264 with AAC when it has "
	  "them, and never re-encoded.", 0, "1080", 0, 0, 0, 0, height_choices },
	{ AXIS_YOUTUBE_SETTING_SIZE, AXIS_SETTING_BYTE_SIZE, "Largest video file",
	  "A video larger than this is not downloaded.", 0, "8GiB", 1024 * 1024, 0, 0, 0, NULL },
	{ AXIS_YOUTUBE_SETTING_PROXY, AXIS_SETTING_STRING, "Proxy for YouTube",
	  "An http, https or socks5 proxy that yt-dlp uses, such as socks5h://vpn:1080. yt-dlp makes its own "
	  "connections, which the outbound binding does not cover. With socks5:// the names of YouTube's servers are "
	  "looked up on this machine, by its own DNS, and only the connections go through the proxy; with socks5h:// "
	  "the proxy looks the names up too, so nothing about YouTube leaves this machine outside the proxy. Use "
	  "socks5h:// for a VPN.", AXIS_SETTING_SECRET, NULL, 0, 255, 0, 0, NULL },
	{ AXIS_YOUTUBE_SETTING_BOUND, AXIS_SETTING_BOOLEAN, "All of Axis's network goes through the VPN",
	  "Switch this on only when Axis runs in the VPN container's network, so that everything it and yt-dlp send "
	  "uses the VPN. While outbound.bind is set, YouTube work needs this or a proxy.", 0, "false", 0, 0, 0, 0, NULL },
	{ AXIS_YOUTUBE_SETTING_RUNTIME, AXIS_SETTING_CHOICE, "JavaScript runtime for yt-dlp",
	  "The program yt-dlp solves YouTube's player checks with. The image ships Node.js.", 0, "node", 0, 0, 0, 0,
	  runtime_choices },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

gboolean
axis_youtube_proxy_valid(const gchar *text)
{
	static const gchar *const schemes[] = { "http://", "https://", "socks4://", "socks4a://", "socks5://",
		"socks5h://" };
	const gchar *p;
	gboolean known = FALSE;
	guint i;

	if (text == NULL || strlen(text) > 255)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(schemes); i++)
		known = known || (g_str_has_prefix(text, schemes[i]) && text[strlen(schemes[i])] != '\0');
	if (!known)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return FALSE;
	}
	return TRUE;
}

static gboolean
validate_proxy(
	const gchar  *text,
	gchar       **reason
){
	if (axis_youtube_proxy_valid(text))
		return TRUE;
	*reason = g_strdup("must be an http, https, socks4 or socks5 address, such as socks5h://vpn:1080, with no spaces");
	return FALSE;
}

const AxisSettingInfo *
axis_youtube_get_setting_table(void)
{
	axis_setting_set_validator(AXIS_YOUTUBE_SETTING_PROXY, validate_proxy);
	axis_setting_set_choice_labels(AXIS_YOUTUBE_SETTING_RUNTIME, runtime_labels);
	return setting_table;
}

static gboolean
declared(
	AxisYoutube *self,
	const gchar *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

static gchar *
resolve_program(AxisYoutube *self)
{
	g_autofree gchar *name = declared(self, AXIS_YOUTUBE_SETTING_PROGRAM) ?
		axis_settings_get_string(self->settings, AXIS_YOUTUBE_SETTING_PROGRAM) : NULL;

	if (name == NULL || name[0] == '\0') {
		g_free(name);
		name = g_strdup("yt-dlp");
	}
	/* As ffprobe's: a path must be absolute and a program; a name is looked
	 * for on the PATH. */
	if (strchr(name, '/') != NULL) {
		if (g_path_is_absolute(name) && g_file_test(name, G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_EXECUTABLE))
			return g_steal_pointer(&name);
		return NULL;
	}
	return g_find_program_in_path(name);
}

/* The largest video taken ("youtube.max_size"). */
static guint64
max_bytes_setting(AxisYoutube *self)
{
	if (declared(self, AXIS_YOUTUBE_SETTING_SIZE)) {
		gint64 size = axis_settings_get_byte_size(self->settings, AXIS_YOUTUBE_SETTING_SIZE);

		if (size >= 1024 * 1024)
			return (guint64)size;
	}
	return G_GUINT64_CONSTANT(8) * 1024 * 1024 * 1024;
}

/* A directory of Axis's own for yt-dlp, made when it is not there: a real
 * directory (not a link) that the daemon's user owns and nobody else can
 * write to.  With @kept_empty it must hold nothing: yt-dlp loads plugins
 * from under its home and its settings folder whatever its options say, so
 * anything found there (left by a run of yt-dlp, or by someone) is a reason
 * not to run it.  %NULL when it is as it must be; else why not. */
static gchar *
own_dir_problem(
	const gchar *path,
	gboolean     kept_empty
){
	struct stat st;
	GDir *dir;
	gboolean empty;

	if (g_mkdir_with_parents(path, 0700) != 0)
		return g_strdup_printf("The folder Axis keeps for yt-dlp (%s) could not be made: %s.", path, g_strerror(errno));
	if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
		return g_strdup_printf("The folder Axis keeps for yt-dlp (%s) is not a folder of Axis's own. Remove it; Axis "
			"makes it again.", path);
	if ((st.st_mode & 022) != 0 && (chmod(path, 0700) != 0 || lstat(path, &st) != 0 || (st.st_mode & 022) != 0))
		return g_strdup_printf("The folder Axis keeps for yt-dlp (%s) can be written by other users, and Axis could "
			"not change that.", path);
	if (!kept_empty)
		return NULL;
	dir = g_dir_open(path, 0, NULL);
	if (dir == NULL)
		return g_strdup_printf("The folder Axis keeps for yt-dlp (%s) cannot be read.", path);
	empty = g_dir_read_name(dir) == NULL;
	g_dir_close(dir);
	if (!empty)
		return g_strdup_printf("The folder Axis gives yt-dlp as its home (%s) is not empty. Axis keeps it empty so "
			"that yt-dlp loads no plugin and no settings from it, and does not run yt-dlp while something is in it: "
			"look at what is there, and remove it.", path);
	return NULL;
}

/* The three folders yt-dlp is given, made and looked at; each %NULL when the
 * layer has no directory of its own.  Returns why yt-dlp is not run with
 * them, or %NULL. */
static gchar *
surroundings(
	AxisYoutube  *self,
	gchar       **home,
	gchar       **config,
	gchar       **cache
){
	gchar *problem;

	*home = *config = *cache = NULL;
	if (self->state_dir == NULL)
		return NULL;
	*home = g_build_filename(self->state_dir, "home", NULL);
	*config = g_build_filename(self->state_dir, "config", NULL);
	*cache = g_build_filename(self->state_dir, "cache", NULL);
	problem = own_dir_problem(*home, TRUE);
	if (problem == NULL)
		problem = own_dir_problem(*config, TRUE);
	if (problem == NULL)
		problem = own_dir_problem(*cache, FALSE);
	return problem;
}

void
axis_youtube_run_read(
	AxisYoutube    *self,
	AxisYoutubeRun *run
){
	g_autofree gchar *home = NULL;
	g_autofree gchar *config = NULL;
	g_autofree gchar *cache = NULL;
	g_autofree gchar *proxy = NULL;

	memset(run, 0, sizeof(*run));
	run->program = resolve_program(self);
	run->max_height = 1080;
	run->max_bytes = max_bytes_setting(self);
	if (declared(self, AXIS_YOUTUBE_SETTING_HEIGHT)) {
		g_autofree gchar *height = axis_settings_get_string(self->settings, AXIS_YOUTUBE_SETTING_HEIGHT);
		guint64 value = height != NULL ? g_ascii_strtoull(height, NULL, 10) : 0;

		if (value == 480 || value == 720 || value == 1080 || value == 2160)
			run->max_height = (guint)value;
	}
	if (declared(self, AXIS_YOUTUBE_SETTING_PROXY)) {
		proxy = axis_settings_get_string(self->settings, AXIS_YOUTUBE_SETTING_PROXY);
		if (proxy != NULL && !axis_youtube_proxy_valid(proxy))
			g_clear_pointer(&proxy, g_free);
	}
	run->js_runtime = g_strdup("node");
	if (declared(self, AXIS_YOUTUBE_SETTING_RUNTIME)) {
		g_autofree gchar *runtime = axis_settings_get_string(self->settings, AXIS_YOUTUBE_SETTING_RUNTIME);

		g_clear_pointer(&run->js_runtime, g_free);
		if (g_strcmp0(runtime, "node") == 0 || g_strcmp0(runtime, "deno") == 0)
			run->js_runtime = g_steal_pointer(&runtime);
	}
	/* Its surroundings: a home and a settings folder that hold nothing, and
	 * a cache; made when first needed.  Without them (tests) each names a
	 * place that does not exist, and yt-dlp keeps no cache. */
	run->problem = surroundings(self, &home, &config, &cache);
	if (run->problem == NULL && cache != NULL)
		run->cache_dir = g_strdup(cache);
	run->environment = axis_youtube_build_environment(home, config, cache, proxy);
}

void
axis_youtube_run_clear(AxisYoutubeRun *run)
{
	g_free(run->program);
	g_free(run->js_runtime);
	g_free(run->cache_dir);
	g_free(run->problem);
	g_strfreev(run->environment);
	memset(run, 0, sizeof(*run));
}

gchar **
axis_youtube_build_environment(
	const gchar *home,
	const gchar *config,
	const gchar *cache,
	const gchar *proxy
){
	/* Not there, and nobody but root could make it. */
	static const gchar nowhere[] = "/nonexistent/axis-youtube";
	static const gchar *const proxy_names[] = { "ALL_PROXY", "all_proxy", "HTTPS_PROXY", "https_proxy", "HTTP_PROXY",
		"http_proxy" };
	GPtrArray *env = g_ptr_array_new();
	guint i;

	g_ptr_array_add(env, g_strconcat("HOME=", home != NULL ? home : nowhere, NULL));
	g_ptr_array_add(env, g_strconcat("XDG_CONFIG_HOME=", config != NULL ? config : nowhere, NULL));
	g_ptr_array_add(env, g_strconcat("XDG_CACHE_HOME=", cache != NULL ? cache : nowhere, NULL));
	/* The proxy here and not in the command: a command is in the process
	 * list for anyone on the machine, an environment only for the daemon's
	 * user.  With no proxy none of these is set, and nothing the daemon
	 * inherited reaches yt-dlp (the runner's environment is its own). */
	for (i = 0; proxy != NULL && i < G_N_ELEMENTS(proxy_names); i++)
		g_ptr_array_add(env, g_strconcat(proxy_names[i], "=", proxy, NULL));
	g_ptr_array_add(env, NULL);
	return (gchar **)g_ptr_array_free(env, FALSE);
}

/* ── The command ────────────────────────────────────────────────── */

gchar **
axis_youtube_build_argv(
	const gchar *program,
	const gchar *video_id,
	gboolean     look,
	guint        max_height,
	guint64      max_bytes,
	const gchar *cache_dir,
	const gchar *js_runtime
){
	GPtrArray *argv;

	g_return_val_if_fail(program != NULL && axis_youtube_video_id_valid(video_id), NULL);

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, g_strdup(program));
	/* No configuration file, no cookies, no hook, one video. */
	g_ptr_array_add(argv, g_strdup("--ignore-config"));
	g_ptr_array_add(argv, g_strdup("--no-playlist"));
	g_ptr_array_add(argv, g_strdup("--no-cookies"));
	g_ptr_array_add(argv, g_strdup("--no-cookies-from-browser"));
	g_ptr_array_add(argv, g_strdup("--no-exec"));
	/* Nothing is cut out of a video. */
	g_ptr_array_add(argv, g_strdup("--no-sponsorblock"));
	g_ptr_array_add(argv, g_strdup("--quiet"));
	g_ptr_array_add(argv, g_strdup("--no-warnings"));
	g_ptr_array_add(argv, g_strdup("--color"));
	g_ptr_array_add(argv, g_strdup("never"));
	g_ptr_array_add(argv, g_strdup("--socket-timeout"));
	g_ptr_array_add(argv, g_strdup("30"));
	g_ptr_array_add(argv, g_strdup("--retries"));
	g_ptr_array_add(argv, g_strdup("3"));
	g_ptr_array_add(argv, g_strdup("--fragment-retries"));
	g_ptr_array_add(argv, g_strdup("3"));
	if (js_runtime != NULL) {
		g_ptr_array_add(argv, g_strdup("--js-runtimes"));
		g_ptr_array_add(argv, g_strdup(js_runtime));
	}
	if (cache_dir != NULL) {
		g_ptr_array_add(argv, g_strdup("--cache-dir"));
		g_ptr_array_add(argv, g_strdup(cache_dir));
	} else {
		g_ptr_array_add(argv, g_strdup("--no-cache-dir"));
	}
	/* The proxy is in the environment, not here (the process list). */
	/* The best picture within the height, H.264 with AAC when YouTube has
	 * them (what every player handles), else whatever it has; the last
	 * choice is a video YouTube serves as one file, which is not merged and
	 * so keeps its own container (mp4 or webm). */
	g_ptr_array_add(argv, g_strdup("--format"));
	g_ptr_array_add(argv, g_strdup_printf("bv*[height<=%u][vcodec^=avc1]+ba[acodec^=mp4a]/bv*[height<=%u]+ba/"
		"b[height<=%u]", max_height, max_height, max_height));
	if (look) {
		g_ptr_array_add(argv, g_strdup("--skip-download"));
		g_ptr_array_add(argv, g_strdup("--dump-single-json"));
	} else {
		g_ptr_array_add(argv, g_strdup("--progress"));
		g_ptr_array_add(argv, g_strdup("--newline"));
		g_ptr_array_add(argv, g_strdup("--progress-delta"));
		g_ptr_array_add(argv, g_strdup("1"));
		g_ptr_array_add(argv, g_strdup("--progress-template"));
		g_ptr_array_add(argv, g_strdup(AXIS_YOUTUBE_PROGRESS_TEMPLATE));
		/* Separate streams are merged as they are (a stream copy). */
		g_ptr_array_add(argv, g_strdup("--merge-output-format"));
		g_ptr_array_add(argv, g_strdup("mkv"));
		if (max_bytes > 0) {
			g_ptr_array_add(argv, g_strdup("--max-filesize"));
			g_ptr_array_add(argv, g_strdup_printf("%" G_GUINT64_FORMAT, max_bytes));
		}
		g_ptr_array_add(argv, g_strdup("--no-mtime"));
		/* Into its working directory, under a name of Axis's. */
		g_ptr_array_add(argv, g_strdup("--output"));
		g_ptr_array_add(argv, g_strdup(AXIS_YOUTUBE_STAGING_STEM ".%(ext)s"));
	}
	/* The address is Axis's own, from the id, and the last argument: nothing
	 * after "--" is an option. */
	g_ptr_array_add(argv, g_strdup("--"));
	g_ptr_array_add(argv, axis_youtube_address_for(video_id));
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}

gchar *
axis_youtube_run_problem(
	const AxisProbeRunResult *result,
	guint                     timeout_ms
){
	switch (result->status) {
	case AXIS_PROBE_RUN_TIMEOUT:
		return g_strdup_printf("yt-dlp did not finish within %u minutes and was stopped.", MAX(timeout_ms / 60000, 1u));
	case AXIS_PROBE_RUN_TOO_MUCH:
		return g_strdup("yt-dlp printed more than Axis accepts and was stopped.");
	case AXIS_PROBE_RUN_CANCELLED:
		return g_strdup("Stopped.");
	case AXIS_PROBE_RUN_SPAWN_FAILED:
		return g_strdup_printf("yt-dlp could not be started: %s", result->message != NULL ? result->message :
			"the system refused");
	case AXIS_PROBE_RUN_UNKILLABLE:
		return g_strdup("yt-dlp could not be stopped; the system is waiting for it.");
	case AXIS_PROBE_RUN_SIGNALLED:
		return g_strdup_printf("yt-dlp crashed (signal %s).", axis_probe_run_signal_name(result->exit_status));
	default:
		return g_strdup("yt-dlp failed.");
	}
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static gboolean kind_test(const AxisDownloadKindInfo *kind, AxisDownloadConn *conn, const gchar *category,
	AxisDownloadTestResult *result, GError **error);
static gboolean kind_add(const AxisDownloadKindInfo *kind, AxisDownloadConn *conn, const AxisDownloadRelease *release,
	const gchar *category, AxisDownloadPriority priority, gchar **client_ref, GError **error);
static gboolean kind_list(const AxisDownloadKindInfo *kind, AxisDownloadConn *conn, const gchar *category,
	GPtrArray **items, GError **error);
static gboolean kind_remove(const AxisDownloadKindInfo *kind, AxisDownloadConn *conn, const gchar *client_ref,
	gboolean delete_data, GError **error);

AxisYoutube *
axis_youtube_new(
	AxisDbPool     *pool,
	AxisSettings   *settings,
	AxisStorage    *storage,
	AxisDownloads  *downloads,
	AxisLibrary    *library,
	const gchar    *state_dir,
	GError        **error
){
	AxisYoutube *self;

	g_return_val_if_fail(pool != NULL && storage != NULL && downloads != NULL, NULL);

	self = g_new0(AxisYoutube, 1);
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->storage = axis_storage_ref(storage);
	self->downloads = axis_downloads_ref(downloads);
	self->library = library != NULL ? axis_library_ref(library) : NULL;
	self->state_dir = g_strdup(state_dir);
	self->stopping = g_cancellable_new();
	g_mutex_init(&self->lock);
	g_mutex_init(&self->look_lock);
	self->kind.name = AXIS_YOUTUBE_KIND_NAME;
	self->kind.label = AXIS_YOUTUBE_KIND_LABEL;
	self->kind.protocol = AXIS_DOWNLOAD_YOUTUBE;
	self->kind.test = kind_test;
	self->kind.add = kind_add;
	self->kind.list = kind_list;
	self->kind.remove = kind_remove;
	self->kind.data = self;
	self->kind.builtin = TRUE;
	if (!axis_downloads_add_kind(downloads, &self->kind, error)) {
		axis_youtube_free(self);
		return NULL;
	}
	return self;
}

void
axis_youtube_free(AxisYoutube *self)
{
	if (self == NULL)
		return;
	g_free(self->state_dir);
	g_clear_object(&self->stopping);
	g_free(self->version_program);
	g_free(self->version_text);
	g_mutex_clear(&self->lock);
	g_mutex_clear(&self->look_lock);
	if (self->library != NULL)
		axis_library_unref(self->library);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_free(self);
}

void
axis_youtube_stop(AxisYoutube *self)
{
	g_return_if_fail(self != NULL);

	g_cancellable_cancel(self->stopping);
}

void
axis_youtube_set_tuning(
	AxisYoutube             *self,
	const AxisYoutubeTuning *tuning
){
	g_mutex_lock(&self->lock);
	if (tuning != NULL)
		self->tuning = *tuning;
	else
		memset(&self->tuning, 0, sizeof(self->tuning));
	g_mutex_unlock(&self->lock);
}

guint
axis_youtube_get_runs(AxisYoutube *self)
{
	return (guint)g_atomic_int_get(&self->runs);
}

gboolean
axis_youtube_client(
	AxisYoutube  *self,
	gint64       *client_id,
	GError      **error
){
	return axis_downloads_builtin_client(self->downloads, AXIS_YOUTUBE_KIND_NAME, client_id, error);
}

gboolean
axis_youtube_poll_now(AxisYoutube *self)
{
	g_autoptr(GError) error = NULL;
	gint64 client_id = 0;

	if (axis_youtube_client(self, &client_id, &error) &&
	    axis_downloads_poll_client(self->downloads, client_id, NULL, NULL, &error))
		return TRUE;
	g_debug("youtube: the downloads layer could not look at once: %s", error->message);
	return FALSE;
}

/* ── The switch, and what holds work back ───────────────────────── */

typedef struct
{
	gboolean  enabled;
	gchar    *enabled_by;
	gint64    enabled_us;
	gint64    paused_until_us;	/* 0 when not paused now */
} Switch;

static void
switch_clear(Switch *state)
{
	g_free(state->enabled_by);
	memset(state, 0, sizeof(*state));
}

static gboolean
switch_read(
	AxisYoutube  *self,
	Switch       *state,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	gboolean paused = FALSE;

	memset(state, 0, sizeof(*state));
	if (!axis_db_pool_exec(self->pool,
	        "SELECT enabled, enabled_by, " US("enabled_at") ", " US("paused_until") ","
	        " coalesce(paused_until > now(), false) FROM youtube_state", 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_boolean(result, 0, 0, &state->enabled);
	state->enabled_by = g_strdup(axis_db_result_get_text(result, 0, 1));
	axis_db_result_get_int64(result, 0, 2, &state->enabled_us);
	axis_db_result_get_boolean(result, 0, 4, &paused);
	if (paused)
		axis_db_result_get_int64(result, 0, 3, &state->paused_until_us);
	return TRUE;
}

static gchar *
time_words(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%Y-%m-%d %H:%M UTC") : g_strdup("later");
}

static gboolean
bound_without_way(AxisYoutube *self)
{
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	g_autofree gchar *proxy = NULL;

	axis_outbound_get_config(&target, &scope);
	if (target.kind == AXIS_OUTBOUND_OFF)
		return FALSE;
	if (declared(self, AXIS_YOUTUBE_SETTING_BOUND) && axis_settings_get_boolean(self->settings, AXIS_YOUTUBE_SETTING_BOUND))
		return FALSE;
	proxy = declared(self, AXIS_YOUTUBE_SETTING_PROXY) ?
		axis_settings_get_string(self->settings, AXIS_YOUTUBE_SETTING_PROXY) : NULL;
	return proxy == NULL || !axis_youtube_proxy_valid(proxy);
}

static gchar *
blocked_from(
	AxisYoutube  *self,
	const Switch *state
){
	g_autofree gchar *program = NULL;

	if (!state->enabled)
		return g_strdup("YouTube is switched off. An administrator switches it on under System, YouTube downloader.");
	if (state->paused_until_us > 0) {
		g_autofree gchar *until = time_words(state->paused_until_us);

		return g_strdup_printf("YouTube refused this server (too many requests, or a bot check), so YouTube work is "
			"paused until %s. Nothing is tried again before then.", until);
	}
	if (g_cancellable_is_cancelled(self->stopping))
		return g_strdup("Axis is stopping, so nothing is downloaded from YouTube now.");
	/* Fail closed: yt-dlp makes its own connections, outside the binding. */
	if (bound_without_way(self))
		return g_strdup("Axis's connections are bound to a network interface (outbound.bind), and yt-dlp makes its "
			"own connections, which the binding does not cover. Set a proxy for YouTube, or, when Axis runs in the "
			"VPN container's network, switch on \"All of Axis's network goes through the VPN\", both in Settings "
			"under YouTube.");
	program = resolve_program(self);
	if (program == NULL)
		return g_strdup("yt-dlp is not installed where the setting \"yt-dlp program\" says (youtube.ytdlp_path).");
	/* Its home and its settings folder hold nothing, or it is not run. */
	{
		g_autofree gchar *home = NULL;
		g_autofree gchar *config = NULL;
		g_autofree gchar *cache = NULL;

		return surroundings(self, &home, &config, &cache);
	}
}

gchar *
axis_youtube_blocked(AxisYoutube *self)
{
	g_autoptr(GError) error = NULL;
	Switch state;
	gchar *why;

	if (!switch_read(self, &state, &error))
		return g_strdup("The database could not be asked whether YouTube is switched on.");
	why = blocked_from(self, &state);
	switch_clear(&state);
	return why;
}

void
axis_youtube_note_refused(AxisYoutube *self)
{
	g_autoptr(GError) error = NULL;

	/* An hour, then twice as long each time, up to a day. */
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE youtube_state SET paused_until = now() + least(interval '1 hour' * power(2, pause_step),"
	        " interval '1 day'), pause_step = least(pause_step + 1, 16)", 0, NULL, NULL, &error))
		g_warning("youtube: the pause after YouTube's refusal could not be recorded: %s", error->message);
	else
		g_message("youtube: YouTube refused this server; YouTube work is paused");
	/* A pause holds what runs too: a fetch beside the look that was refused
	 * would go on asking.  (A fetch that was refused itself has failed by
	 * now, and is not touched.) */
	axis_youtube_stop_fetches(self, "Stopped: YouTube refused this server, and YouTube work is paused. Download it "
		"again after the pause.");
}

guint
axis_youtube_stop_fetches(
	AxisYoutube *self,
	const gchar *message
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1];
	AxisDbConn *db;
	guint n = 0;
	guint i;

	params[0] = message;
	db = axis_db_pool_acquire(self->pool, &error);
	if (db == NULL || !axis_db_conn_exec(db,
	        "UPDATE youtube_video SET state = 'cancelled', message = $1::text, rate_bps = NULL, eta_s = NULL,"
	        " finished_at = now(), updated_at = now() WHERE state IN ('wanted', 'fetching') RETURNING job_id", 1, params,
	        &result, &error)) {
		g_warning("youtube: the downloads that run could not be stopped: %s", error->message);
		if (db != NULL)
			axis_db_pool_release(self->pool, db);
		return 0;
	}
	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		g_autoptr(GError) local = NULL;
		gint64 job_id = 0;

		/* Cancelling its job kills yt-dlp; the job removes its folder. */
		if (axis_db_result_get_int64(result, i, 0, &job_id) && job_id > 0 && !axis_job_cancel(db, job_id, NULL, &local))
			g_warning("youtube: the job of a download that was stopped could not be cancelled: %s", local->message);
	}
	axis_db_pool_release(self->pool, db);
	return n;
}

guint
axis_youtube_fail_orphans(AxisYoutube *self)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1];

	params[0] = AXIS_YOUTUBE_STOPPED_REASON;
	/* A record is made 'wanted' and given its job in one transaction
	 * (kind_add()), and a fetch writes how it ended before its job ends: a
	 * record in hand whose job is not is one nothing will ever finish. */
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE youtube_video y SET state = 'failed', message = $1::text, rate_bps = NULL, eta_s = NULL,"
	        " finished_at = now(), updated_at = now()"
	        " WHERE y.state IN ('wanted', 'fetching') AND NOT EXISTS (SELECT 1 FROM job j WHERE j.id = y.job_id"
	        " AND j.state IN ('queued', 'running')) RETURNING y.video_id", 1, params, &result, &error)) {
		g_debug("youtube: the downloads nothing will finish could not be looked for: %s", error->message);
		return 0;
	}
	if (axis_db_result_get_n_rows(result) > 0)
		g_message("youtube: %u video download(s) were cut short when Axis stopped; each says so and can be asked for "
			"again", axis_db_result_get_n_rows(result));
	return axis_db_result_get_n_rows(result);
}

void
axis_youtube_note_worked(AxisYoutube *self)
{
	axis_db_pool_exec(self->pool, "UPDATE youtube_state SET pause_step = 0 WHERE pause_step <> 0", 0, NULL, NULL, NULL);
}

/* yt-dlp's version as @program said it before, or %NULL. */
static gchar *
kept_version(
	AxisYoutube *self,
	const gchar *program
){
	gchar *version = NULL;

	g_mutex_lock(&self->lock);
	if (g_strcmp0(self->version_program, program) == 0 && self->version_text != NULL)
		version = g_strdup(self->version_text);
	g_mutex_unlock(&self->lock);
	return version;
}

/* yt-dlp's version, asked of @program once and kept. */
static gchar *
ask_version(
	AxisYoutube *self,
	const gchar *program
){
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	AxisYoutubeRun run;
	const gchar *argv[3];
	gchar *version = kept_version(self, program);

	if (version != NULL)
		return version;
	/* In the surroundings of every run: it loads its plugins before it
	 * reads its arguments. */
	axis_youtube_run_read(self, &run);
	if (run.problem != NULL) {
		axis_youtube_run_clear(&run);
		return NULL;
	}

	argv[0] = program;
	argv[1] = "--version";
	argv[2] = NULL;
	memset(&spec, 0, sizeof(spec));
	spec.argv = argv;
	spec.fd = -1;
	spec.timeout_ms = 20000;
	spec.output_cap = 4096;
	spec.error_cap = 4096;
	/* It has nothing to write; a little, for an interpreter's own files. */
	spec.file_size_limit = 1024 * 1024;
	spec.cancellable = self->stopping;
	spec.environment = (const gchar *const *)run.environment;
	g_atomic_int_inc(&self->runs);
	axis_probe_run(&spec, &result);
	if (result.status == AXIS_PROBE_RUN_OK)
		version = axis_youtube_version_read((const gchar *)result.output->data, result.output->len);
	axis_probe_run_result_clear(&result);
	axis_youtube_run_clear(&run);
	if (version != NULL) {
		g_mutex_lock(&self->lock);
		g_free(self->version_program);
		g_free(self->version_text);
		self->version_program = g_strdup(program);
		self->version_text = g_strdup(version);
		g_mutex_unlock(&self->lock);
	}
	return version;
}

void
axis_youtube_status_clear(AxisYoutubeStatus *status)
{
	if (status == NULL)
		return;
	g_free(status->enabled_by);
	g_free(status->blocked);
	g_free(status->program);
	g_free(status->version);
	memset(status, 0, sizeof(*status));
}

gboolean
axis_youtube_get_status(
	AxisYoutube        *self,
	gboolean            ask,
	AxisYoutubeStatus  *status,
	GError            **error
){
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	Switch state;

	memset(status, 0, sizeof(*status));
	if (!switch_read(self, &state, error))
		return FALSE;
	status->enabled = state.enabled;
	status->enabled_by = g_strdup(state.enabled_by);
	status->enabled_us = state.enabled_us;
	status->paused_until_us = state.paused_until_us;
	status->blocked = blocked_from(self, &state);
	status->program = resolve_program(self);
	axis_outbound_get_config(&target, &scope);
	status->bound = target.kind != AXIS_OUTBOUND_OFF;
	/* yt-dlp is not run, even for its version, until YouTube is switched
	 * on: before that, what the image's build recorded is shown.  And only
	 * a caller that says so starts it: otherwise what it answered before. */
	if (state.enabled && status->program != NULL)
		status->version = ask ? ask_version(self, status->program) : kept_version(self, status->program);
	if (status->version == NULL) {
		g_autofree gchar *recorded = NULL;
		gsize length = 0;

		if (g_file_get_contents(AXIS_YOUTUBE_VERSION_FILE, &recorded, &length, NULL)) {
			status->version = axis_youtube_version_read(recorded, length);
			status->version_from_build = status->version != NULL;
		}
	}
	switch_clear(&state);
	return TRUE;
}

gboolean
axis_youtube_set_enabled(
	AxisYoutube  *self,
	gboolean      enabled,
	const gchar  *by,
	guint        *stopped,
	GError      **error
){
	g_autofree gchar *who = axis_download_text_clean(by, 200);
	const gchar *params[2];
	guint n = 0;

	if (stopped != NULL)
		*stopped = 0;
	params[0] = enabled ? "true" : "false";
	params[1] = who;
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE youtube_state SET enabled = $1::boolean,"
	        " enabled_by = CASE WHEN $1::boolean THEN $2::text ELSE enabled_by END,"
	        " enabled_at = CASE WHEN $1::boolean AND NOT enabled THEN now() ELSE enabled_at END", 2, params, NULL, error))
		return FALSE;
	/* Off means off now: after the switch, so that none starts meanwhile. */
	if (!enabled)
		n = axis_youtube_stop_fetches(self, "Stopped: YouTube was switched off.");
	if (stopped != NULL)
		*stopped = n;
	return TRUE;
}

/* ── Records ────────────────────────────────────────────────────── */

static const gchar *const state_names[] = { "listed", "wanted", "fetching", "fetched", "failed", "cancelled" };

const gchar *
axis_youtube_video_state_label(AxisYoutubeVideoState state)
{
	static const gchar *const labels[] = { "Looked at", "Queued", "Downloading", "In the library", "Failed",
		"Stopped" };

	return (guint)state < G_N_ELEMENTS(labels) ? labels[state] : "Failed";
}

void
axis_youtube_video_free(AxisYoutubeVideo *video)
{
	if (video == NULL)
		return;
	g_free(video->video_id);
	g_free(video->title);
	g_free(video->channel);
	g_free(video->channel_id);
	g_free(video->upload_date);
	g_free(video->format_note);
	g_free(video->volume_name);
	g_free(video->rel_path);
	g_free(video->message);
	g_free(video);
}

#define VIDEO_COLUMNS \
	"y.video_id, y.state, y.title, y.channel, y.channel_id, y.upload_date::text, y.duration_s, y.format_note," \
	" y.expected_bytes, y.root_id, y.volume_id, v.name, y.rel_path, y.size_bytes, y.done_bytes, y.rate_bps, y.eta_s," \
	" y.message, y.job_id, " US("y.updated_at")

#define VIDEO_FROM " FROM youtube_video y LEFT JOIN volume v ON v.id = y.volume_id"

static gint64
figure_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = -1;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return -1;
	return value;
}

static AxisYoutubeVideo *
video_at(
	AxisDbResult *result,
	guint         row
){
	AxisYoutubeVideo *video = g_new0(AxisYoutubeVideo, 1);
	const gchar *state = axis_db_result_get_text(result, row, 1);
	guint i;

	video->video_id = g_strdup(axis_db_result_get_text(result, row, 0));
	video->state = AXIS_YOUTUBE_FAILED;
	for (i = 0; state != NULL && i < G_N_ELEMENTS(state_names); i++) {
		if (strcmp(state, state_names[i]) == 0)
			video->state = (AxisYoutubeVideoState)i;
	}
	video->title = g_strdup(axis_db_result_get_text(result, row, 2));
	video->channel = g_strdup(axis_db_result_get_text(result, row, 3));
	video->channel_id = g_strdup(axis_db_result_get_text(result, row, 4));
	video->upload_date = g_strdup(axis_db_result_get_text(result, row, 5));
	video->duration_s = figure_at(result, row, 6);
	video->format_note = g_strdup(axis_db_result_get_text(result, row, 7));
	video->expected_bytes = figure_at(result, row, 8);
	video->root_id = MAX(figure_at(result, row, 9), 0);
	video->volume_id = MAX(figure_at(result, row, 10), 0);
	video->volume_name = g_strdup(axis_db_result_get_text(result, row, 11));
	video->rel_path = g_strdup(axis_db_result_get_text(result, row, 12));
	video->size_bytes = figure_at(result, row, 13);
	video->done_bytes = figure_at(result, row, 14);
	video->rate_bps = figure_at(result, row, 15);
	video->eta_s = figure_at(result, row, 16);
	video->message = g_strdup(axis_db_result_get_text(result, row, 17));
	video->job_id = MAX(figure_at(result, row, 18), 0);
	video->updated_us = MAX(figure_at(result, row, 19), 0);
	return video;
}

gboolean
axis_youtube_get_video(
	AxisYoutube       *self,
	const gchar       *video_id,
	AxisYoutubeVideo **video,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	*video = NULL;
	params[0] = video_id;
	if (!axis_youtube_video_id_valid(video_id)) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_NOT_FOUND, "There is no such video.");
		return FALSE;
	}
	if (!axis_db_pool_exec(self->pool, "SELECT " VIDEO_COLUMNS VIDEO_FROM " WHERE y.video_id = $1::text", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_NOT_FOUND, "There is no such video.");
		return FALSE;
	}
	*video = video_at(result, 0);
	return TRUE;
}

gboolean
axis_youtube_list_videos(
	AxisYoutube  *self,
	guint         limit,
	GPtrArray   **videos,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *limit_text = g_strdup_printf("%u", CLAMP(limit, 1u, 1000u));
	const gchar *params[1];
	guint i;

	*videos = NULL;
	params[0] = limit_text;
	if (!axis_db_pool_exec(self->pool, "SELECT " VIDEO_COLUMNS VIDEO_FROM
	        " ORDER BY y.updated_at DESC, y.video_id LIMIT $1::integer", 1, params, &result, error))
		return FALSE;
	*videos = g_ptr_array_new_with_free_func((GDestroyNotify)axis_youtube_video_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*videos, video_at(result, i));
	return TRUE;
}

/* ── Names ──────────────────────────────────────────────────────── */

/* @text within @max_bytes, cut at a character boundary, with nothing left
 * at its end that the character rule removes. */
static gchar *
fit(
	const gchar *text,
	gsize        max_bytes
){
	g_autofree gchar *cut = NULL;
	gsize length = strlen(text);
	const gchar *end;

	if (length <= max_bytes)
		return g_strdup(text);
	end = text + max_bytes;
	while (end > text && ((guchar)*end & 0xc0) == 0x80)
		end--;
	cut = g_strndup(text, (gsize)(end - text));
	return axis_import_name_clean(cut, -1);
}

static gchar *
clean_or(
	const gchar *text,
	const gchar *fallback
){
	gchar *clean = axis_import_name_clean(text, -1);

	if (clean[0] != '\0')
		return clean;
	g_free(clean);
	return g_strdup(fallback);
}

const gchar *
axis_youtube_extension(const gchar *extension)
{
	static const gchar *const known[] = { "mkv", "mp4", "webm" };
	guint i;

	for (i = 0; extension != NULL && i < G_N_ELEMENTS(known); i++) {
		if (g_ascii_strcasecmp(extension, known[i]) == 0)
			return known[i];
	}
	return NULL;
}

void
axis_youtube_names(
	const AxisYoutubeVideo  *video,
	gchar                  **channel_dir,
	gchar                  **season_dir,
	gchar                  **file_name
){
	axis_youtube_names_as(video, "mkv", channel_dir, season_dir, file_name);
}

void
axis_youtube_names_as(
	const AxisYoutubeVideo  *video,
	const gchar             *extension,
	gchar                  **channel_dir,
	gchar                  **season_dir,
	gchar                  **file_name
){
	const gchar *ext = axis_youtube_extension(extension) != NULL ? axis_youtube_extension(extension) : "mkv";
	g_autofree gchar *channel = clean_or(video->channel, "Unknown channel");
	g_autofree gchar *title = clean_or(video->title, "Untitled");
	g_autofree gchar *channel_in_dir = NULL;
	g_autofree gchar *channel_in_file = fit(channel, 60);
	g_autofree gchar *title_cut = NULL;
	g_autofree gchar *middle = NULL;
	g_autofree gchar *tail = NULL;
	gboolean dated = video->upload_date != NULL && strlen(video->upload_date) == 10;
	gsize fixed;

	/* "<Channel> [<channel id>]": the id keeps two channels of one name
	 * apart, and lets the folder be recognised again. */
	if (video->channel_id != NULL && video->channel_id[0] != '\0') {
		g_autofree gchar *suffix = g_strdup_printf(" [%s]", video->channel_id);

		channel_in_dir = fit(channel, AXIS_IMPORT_NAME_BUDGET - strlen(suffix));
		*channel_dir = g_strconcat(channel_in_dir, suffix, NULL);
	} else {
		*channel_dir = fit(channel, AXIS_IMPORT_NAME_BUDGET);
	}
	/* A year is a season. */
	*season_dir = dated ? g_strdup_printf("Season %.4s", video->upload_date) : g_strdup("Season 0000");
	/* "<Channel> - <date> - <Title> [<video id>].mkv": the title is what is
	 * cut to fit, never the id or the extension. */
	middle = dated ? g_strdup_printf(" - %s - ", video->upload_date) : g_strdup(" - ");
	tail = g_strdup_printf(" [%s].%s", video->video_id, ext);
	fixed = strlen(channel_in_file) + strlen(middle) + strlen(tail);
	title_cut = fit(title, AXIS_IMPORT_NAME_BUDGET - fixed);
	if (title_cut[0] == '\0') {
		g_free(title_cut);
		title_cut = g_strdup("Untitled");
	}
	*file_name = g_strconcat(channel_in_file, middle, title_cut, tail, NULL);
}

gchar *
axis_youtube_release_name(const AxisYoutubeVideo *video)
{
	g_autofree gchar *text = g_strdup_printf("%s - %s", video->channel, video->title);
	g_autofree gchar *clean = axis_download_text_clean(text, AXIS_DOWNLOAD_NAME_MAX_CHARS - 16);

	return g_strdup_printf("%s [%s]", clean != NULL ? clean : "YouTube video", video->video_id);
}

/* ── A look ─────────────────────────────────────────────────────── */

static gboolean
store_look(
	AxisYoutube            *self,
	const AxisYoutubeInfo  *info,
	const gchar            *by,
	GError                **error
){
	g_autofree gchar *date = info->year > 0 ? g_strdup_printf("%04d-%02d-%02d", info->year, info->month, info->day) :
		NULL;
	g_autofree gchar *duration = info->duration_s >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, info->duration_s) : NULL;
	g_autofree gchar *expected = info->expected_bytes >= 0 ?
		g_strdup_printf("%" G_GINT64_FORMAT, info->expected_bytes) : NULL;
	g_autofree gchar *note = axis_download_text_clean(info->format_note, 200);
	g_autofree gchar *who = axis_download_text_clean(by, 200);
	const gchar *params[9];

	params[0] = info->video_id;
	params[1] = info->title;
	params[2] = info->channel;
	params[3] = info->channel_id;
	params[4] = date;
	params[5] = duration;
	params[6] = note;
	params[7] = expected;
	params[8] = who;
	/* A video being fetched or in the library keeps its state: only what
	 * yt-dlp said of it is not written over it either. */
	return axis_db_pool_exec(self->pool,
		"INSERT INTO youtube_video (video_id, title, channel, channel_id, upload_date, duration_s, format_note,"
		" expected_bytes, asked_by)"
		" VALUES ($1::text, $2::text, $3::text, $4::text, $5::date, $6::integer, $7::text, $8::bigint, $9::text)"
		" ON CONFLICT (video_id) DO UPDATE SET title = excluded.title, channel = excluded.channel,"
		" channel_id = excluded.channel_id, upload_date = excluded.upload_date, duration_s = excluded.duration_s,"
		" format_note = excluded.format_note, expected_bytes = excluded.expected_bytes, asked_by = excluded.asked_by,"
		" state = 'listed', message = NULL, done_bytes = NULL, rate_bps = NULL, eta_s = NULL, looked_at = now(),"
		" updated_at = now()"
		" WHERE youtube_video.state IN ('listed', 'failed', 'cancelled')", 9, params, NULL, error);
}

gboolean
axis_youtube_look(
	AxisYoutube       *self,
	const gchar       *address,
	const gchar       *by,
	AxisYoutubeVideo **video,
	GError           **error
){
	g_autofree gchar *video_id = NULL;
	g_autofree gchar *blocked = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisYoutubeInfo) info = NULL;
	g_autoptr(GError) local = NULL;
	g_auto(GStrv) argv = NULL;
	const gchar *why = NULL;
	AxisProbeRunSpec spec;
	AxisProbeRunResult result;
	AxisYoutubeRun run;
	guint timeout_ms;

	*video = NULL;
	if (!axis_youtube_address_parse(address, &video_id, &why)) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_INVALID, why);
		return FALSE;
	}
	/* One Axis has in hand already is shown as it is; yt-dlp is not asked. */
	if (axis_youtube_get_video(self, video_id, video, &local)) {
		if ((*video)->state == AXIS_YOUTUBE_WANTED || (*video)->state == AXIS_YOUTUBE_FETCHING ||
		    (*video)->state == AXIS_YOUTUBE_FETCHED)
			return TRUE;
		g_clear_pointer(video, axis_youtube_video_free);
	} else if (!g_error_matches(local, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	blocked = axis_youtube_blocked(self);
	if (blocked != NULL) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_OFF, blocked);
		return FALSE;
	}
	if (!g_mutex_trylock(&self->look_lock)) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_BUSY,
			"Another address is being looked at. Try again in a moment.");
		return FALSE;
	}
	axis_youtube_run_read(self, &run);
	if (run.program == NULL || run.problem != NULL) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_OFF, run.problem != NULL ? run.problem :
			"yt-dlp is not installed where the setting \"yt-dlp program\" says (youtube.ytdlp_path).");
		axis_youtube_run_clear(&run);
		g_mutex_unlock(&self->look_lock);
		return FALSE;
	}
	g_mutex_lock(&self->lock);
	timeout_ms = self->tuning.look_timeout_ms > 0 ? self->tuning.look_timeout_ms : AXIS_YOUTUBE_LOOK_TIMEOUT_MS;
	g_mutex_unlock(&self->lock);
	argv = axis_youtube_build_argv(run.program, video_id, TRUE, run.max_height, 0, run.cache_dir, run.js_runtime);
	memset(&spec, 0, sizeof(spec));
	spec.argv = (const gchar *const *)argv;
	spec.fd = -1;
	spec.timeout_ms = timeout_ms;
	spec.output_cap = AXIS_YOUTUBE_LOOK_OUTPUT_MAX;
	spec.error_cap = AXIS_YOUTUBE_ERROR_MAX;
	/* The daemon stopping ends it: nobody waits out its time limit. */
	spec.cancellable = self->stopping;
	spec.environment = (const gchar *const *)run.environment;
	/* It downloads nothing; its cache is all it may write. */
	spec.file_size_limit = 64 * 1024 * 1024;
	g_atomic_int_inc(&self->runs);
	axis_probe_run(&spec, &result);
	axis_youtube_run_clear(&run);
	g_mutex_unlock(&self->look_lock);

	if (result.status == AXIS_PROBE_RUN_CANCELLED) {
		problem = g_strdup("Axis is stopping, so the video was not looked up.");
	} else if (result.status == AXIS_PROBE_RUN_OK) {
		info = axis_youtube_info_parse((const gchar *)result.output->data, result.output->len, &problem);
	} else if (result.status == AXIS_PROBE_RUN_EXITED) {
		if (axis_youtube_failure_read((const gchar *)result.errors->data, result.errors->len, &problem) ==
		    AXIS_YOUTUBE_FAILURE_REFUSED)
			axis_youtube_note_refused(self);
	} else {
		problem = axis_youtube_run_problem(&result, timeout_ms);
	}
	axis_probe_run_result_clear(&result);
	if (info == NULL) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_FAILED, problem != NULL ? problem :
			"yt-dlp failed.");
		return FALSE;
	}
	if (info->refusal != NULL) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_REFUSED, info->refusal);
		return FALSE;
	}
	/* What it describes is the video that was asked about, or nothing. */
	if (g_strcmp0(info->video_id, video_id) != 0) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_FAILED,
			"yt-dlp answered about another video than the one asked for.");
		return FALSE;
	}
	if (!store_look(self, info, by, error))
		return FALSE;
	return axis_youtube_get_video(self, video_id, video, error);
}

/* ── Asking for the download ────────────────────────────────────── */

static gboolean
storage_refusal(
	GError       **error,
	const gchar   *format,
	...
) G_GNUC_PRINTF(2, 3);

static gboolean
storage_refusal(
	GError       **error,
	const gchar   *format,
	...
){
	va_list args;
	g_autofree gchar *text = NULL;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_STORAGE, text);
	return FALSE;
}

/* Whether something is at @rel (a path from the volume's top) on the volume
 * the guard holds: 1, 0, or -1 when it cannot be told. */
static gint
exists_at(
	AxisVolumeGuard *guard,
	const gchar     *rel
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *parent = NULL;
	const gchar *slash = strrchr(rel, '/');
	const gchar *base = slash != NULL ? slash + 1 : rel;
	struct stat st;
	gint fd;
	gint rc;

	parent = slash != NULL ? g_strndup(rel, (gsize)(slash - rel)) : g_strdup("");
	fd = axis_volume_guard_open_dir(guard, parent, &error);
	if (fd < 0)
		return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
			g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ? 0 : -1;
	rc = fstatat(fd, base, &st, AT_SYMLINK_NOFOLLOW);
	close(fd);
	if (rc == 0)
		return 1;
	return errno == ENOENT ? 0 : -1;
}

/* The YouTube library folder for a download: @root_id, or the only one. */
static gboolean
choose_root(
	AxisYoutube   *self,
	gint64         root_id,
	AxisRootInfo **root,
	GError       **error
){
	g_autoptr(GPtrArray) roots = NULL;
	AxisRootInfo *found = NULL;
	guint n = 0;
	guint i;

	*root = NULL;
	if (!axis_storage_list_roots(self->storage, 0, &roots, error))
		return FALSE;
	for (i = 0; i < roots->len; i++) {
		AxisRootInfo *candidate = g_ptr_array_index(roots, i);

		if (candidate->media_type != AXIS_MEDIA_YOUTUBE)
			continue;
		n++;
		if (root_id == 0 ? found == NULL : candidate->id == root_id)
			found = candidate;
	}
	if (n == 0)
		return storage_refusal(error, "There is no YouTube library folder yet. Add one on the Storage page: a library "
			"folder of the kind YouTube.");
	if (root_id != 0 && (found == NULL || found->id != root_id))
		return storage_refusal(error, "That is not a YouTube library folder.");
	if (root_id == 0 && n > 1)
		return storage_refusal(error, "There are several YouTube library folders. Choose the one this video goes to.");
	for (i = 0; i < roots->len; i++) {
		if (g_ptr_array_index(roots, i) == found) {
			*root = g_ptr_array_steal_index(roots, i);
			break;
		}
	}
	return TRUE;
}

guint64
axis_youtube_need_bytes(
	gint64  expected_bytes,
	guint64 max_bytes
){
	/* The streams and the merged file; when yt-dlp gave no size, the most
	 * the video may be. */
	return expected_bytes > 0 ? 2 * MIN((guint64)expected_bytes, max_bytes) : 2 * max_bytes;
}

gboolean
axis_youtube_free_space(
	AxisYoutube *self,
	gint         fd,
	guint64     *available,
	guint64     *floor
){
	struct statvfs space;

	*available = 0;
	*floor = 100 * 1024 * 1024;
	if (declared(self, AXIS_IMPORT_SETTING_FREE_FLOOR))
		*floor = (guint64)MAX(axis_settings_get_byte_size(self->settings, AXIS_IMPORT_SETTING_FREE_FLOOR), 0);
	if (fstatvfs(fd, &space) != 0)
		return FALSE;
	*available = (guint64)space.f_bavail * (guint64)space.f_frsize;
	return TRUE;
}

/* Whether the root can take the video now: storage is not read-only, the
 * disk is there, no file has the video's name, and the disk has room for it
 * with the free space imports keep.  The fetch job asks again; this is so
 * that nothing is queued that cannot be written. */
static gboolean
root_takes(
	AxisYoutube             *self,
	const AxisRootInfo      *root,
	const AxisYoutubeVideo  *video,
	GError                 **error
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, root->volume_id);
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *channel_dir = NULL;
	g_autofree gchar *season_dir = NULL;
	g_autofree gchar *file_name = NULL;
	g_autofree gchar *rel = NULL;
	const gchar *name = volume != NULL ? volume->name : "the library's";
	guint64 available = 0;
	guint64 floor = 0;
	guint64 need;
	gboolean roomy;
	gint fd;

	guard = axis_volume_guard_acquire(self->storage, root->volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
	if (guard == NULL) {
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY))
			return storage_refusal(error, axis_storage_is_read_only_mode(self->storage) ?
				"Storage is read-only (the setting storage.read_only is on), so nothing is downloaded. Switch it off in "
				"Settings, under Storage, to let Axis write to the disk %s." :
				"The disk %s is mounted read-only, so nothing is downloaded to it.", name);
		return storage_refusal(error, "The disk %s is not online, so nothing is downloaded to it.", name);
	}
	axis_youtube_names(video, &channel_dir, &season_dir, &file_name);
	rel = g_strdup_printf("%s%s%s/%s/%s", root->rel_path, root->rel_path[0] != '\0' ? "/" : "", channel_dir, season_dir,
		file_name);
	if (exists_at(guard, rel) != 0)
		return storage_refusal(error, "A file with this video's name is already in the library folder (%s). Nothing is "
			"replaced.", file_name);
	fd = axis_volume_guard_open_dir(guard, root->rel_path, NULL);
	if (fd < 0)
		return storage_refusal(error, "The YouTube library folder on the disk %s cannot be opened.", name);
	need = axis_youtube_need_bytes(video->expected_bytes, max_bytes_setting(self));
	roomy = !axis_youtube_free_space(self, fd, &available, &floor) || available >= need + floor;
	close(fd);
	if (!roomy) {
		g_autofree gchar *have = axis_size_format(available);
		g_autofree gchar *want = axis_size_format(need + floor);

		return storage_refusal(error, "The disk %s has %s free, and this download needs %s (the video while it is put "
			"together, and the free space Axis keeps, import.free_space_floor). Nothing is downloaded.", name, have,
			want);
	}
	return TRUE;
}

gboolean
axis_youtube_download(
	AxisYoutube  *self,
	const gchar  *video_id,
	gint64        root_id,
	const gchar  *by,
	gint64       *download_id,
	GError      **error
){
	g_autoptr(AxisYoutubeVideo) video = NULL;
	g_autoptr(AxisRootInfo) root = NULL;
	g_autofree gchar *blocked = NULL;
	g_autofree gchar *address = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *root_text = NULL;
	g_autofree gchar *who = axis_download_text_clean(by, 200);
	AxisDownloadRequest request;
	const gchar *params[3];

	if (download_id != NULL)
		*download_id = 0;
	/* One cut short when Axis was killed is not "being downloaded"; and its
	 * download's record ends before another is made for the same video. */
	if (axis_youtube_fail_orphans(self) > 0)
		axis_youtube_poll_now(self);
	if (!axis_youtube_get_video(self, video_id, &video, error))
		return FALSE;
	if (video->state == AXIS_YOUTUBE_WANTED || video->state == AXIS_YOUTUBE_FETCHING) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_EXISTS, "This video is being downloaded "
			"already. It is on the Downloads page.");
		return FALSE;
	}
	/* Fetched once: again only when its file is no longer there. */
	if (video->state == AXIS_YOUTUBE_FETCHED) {
		g_autoptr(AxisVolumeGuard) guard = video->volume_id > 0 ?
			axis_volume_guard_acquire(self->storage, video->volume_id, AXIS_VOLUME_INTENT_READ, NULL) : NULL;

		if (video->volume_id > 0 && (guard == NULL || video->rel_path == NULL || exists_at(guard, video->rel_path) != 0)) {
			g_set_error(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_EXISTS, guard == NULL ?
				"This video is in the library already, on a disk that is not online now (%s)." :
				"This video is in the library already (%s).", video->rel_path != NULL ? video->rel_path : "its file");
			return FALSE;
		}
	}
	blocked = axis_youtube_blocked(self);
	if (blocked != NULL) {
		g_set_error_literal(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_OFF, blocked);
		return FALSE;
	}
	if (!choose_root(self, root_id, &root, error) || !root_takes(self, root, video, error))
		return FALSE;

	/* Where it goes; and a video whose file is gone is one to fetch. */
	root_text = g_strdup_printf("%" G_GINT64_FORMAT, root->id);
	params[0] = video_id;
	params[1] = root_text;
	params[2] = who;
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE youtube_video SET root_id = $2::bigint, asked_by = coalesce($3::text, asked_by),"
	        " state = CASE WHEN state = 'fetched' THEN 'listed' ELSE state END,"
	        " volume_id = CASE WHEN state = 'fetched' THEN NULL ELSE volume_id END,"
	        " rel_path = CASE WHEN state = 'fetched' THEN NULL ELSE rel_path END, updated_at = now()"
	        " WHERE video_id = $1::text AND state IN ('listed', 'failed', 'cancelled', 'fetched')", 3, params, NULL,
	        error))
		return FALSE;

	/* Through the downloads layer, to the built-in client: recorded once,
	 * listed, followed and removed like any download. */
	address = axis_youtube_address_for(video_id);
	name = axis_youtube_release_name(video);
	memset(&request, 0, sizeof(request));
	request.protocol = AXIS_DOWNLOAD_YOUTUBE;
	request.address = address;
	request.release_name = name;
	request.module = AXIS_YOUTUBE_MODULE_ID;
	return axis_downloads_add(self->downloads, &request, download_id, error);
}

/* ── The built-in download kind ─────────────────────────────────── */

static gboolean
kind_test(
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	const gchar                *category,
	AxisDownloadTestResult     *result,
	GError                    **error
){
	AxisYoutube *self = kind->data;
	AxisYoutubeStatus status;

	if (!axis_youtube_get_status(self, TRUE, &status, error))
		return FALSE;
	result->version = g_strdup(status.version);
	result->category_exists = -1;
	result->note = g_strdup(status.blocked);
	axis_youtube_status_clear(&status);
	return TRUE;
}

/* Queues the fetch of one video: its record becomes 'wanted' and its job is
 * stored in one transaction, so the job never runs before its record says
 * whose it is.  The client's identifier is the video's id. */
static gboolean
kind_add(
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	const AxisDownloadRelease  *release,
	const gchar                *category,
	AxisDownloadPriority        priority,
	gchar                     **client_ref,
	GError                    **error
){
	AxisYoutube *self = kind->data;
	g_autoptr(AxisDbResult) claimed = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(GError) local = NULL;
	g_autofree gchar *video_id = NULL;
	g_autofree gchar *blocked = NULL;
	g_autofree gchar *job_text = NULL;
	const gchar *why = NULL;
	const gchar *params[2];
	AxisJobSpec spec;
	AxisDbConn *db;
	gint64 job_id = 0;
	gboolean ok;

	*client_ref = NULL;
	/* Only an address of the form Axis writes itself, read again here. */
	if (release->address == NULL || !axis_youtube_address_parse(release->address, &video_id, &why)) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a YouTube video's address%s%s",
			why != NULL ? ": " : "", why != NULL ? why : "");
		return FALSE;
	}
	blocked = axis_youtube_blocked(self);
	if (blocked != NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED, blocked);
		return FALSE;
	}
	db = axis_db_pool_acquire(self->pool, error);
	if (db == NULL)
		return FALSE;
	params[0] = video_id;
	ok = axis_db_conn_begin(db, &local) &&
		axis_db_conn_exec(db,
			"UPDATE youtube_video SET state = 'wanted', wanted_at = now(), started_at = NULL, finished_at = NULL,"
			" message = NULL, size_bytes = NULL, done_bytes = NULL, rate_bps = NULL, eta_s = NULL, updated_at = now()"
			" WHERE video_id = $1::text AND state IN ('listed', 'failed', 'cancelled') AND root_id IS NOT NULL"
			" RETURNING video_id", 1, params, &claimed, &local);
	if (ok && axis_db_result_get_n_rows(claimed) == 0) {
		ok = FALSE;
		g_set_error_literal(&local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED,
			"that video is being downloaded or is in the library already, or was not looked at first");
	}
	if (ok) {
		json_object_set_string_member(payload, "video", video_id);
		memset(&spec, 0, sizeof(spec));
		spec.kind = AXIS_YOUTUBE_FETCH_KIND;
		spec.queue = AXIS_YOUTUBE_QUEUE;
		spec.payload = payload;
		spec.max_attempts = 1;
		ok = axis_job_enqueue(db, &spec, &job_id, NULL, &local);
	}
	if (ok) {
		job_text = g_strdup_printf("%" G_GINT64_FORMAT, job_id);
		params[1] = job_text;
		ok = axis_db_conn_exec(db, "UPDATE youtube_video SET job_id = $2::bigint WHERE video_id = $1::text", 2, params,
			NULL, &local);
	}
	ok = ok && axis_db_conn_commit(db, &local);
	if (!ok)
		axis_db_conn_rollback(db, NULL);
	axis_db_pool_release(self->pool, db);
	if (!ok) {
		if (local->domain == AXIS_DOWNLOAD_ERROR)
			g_propagate_error(error, g_steal_pointer(&local));
		else
			g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_FAILED, "the download could not be queued: %s",
				local->message);
		return FALSE;
	}
	*client_ref = g_steal_pointer(&video_id);
	return TRUE;
}

/* The fetches as the downloads layer follows them: those in hand, and those
 * that ended within a day (so that tracking sees how each ended). */
static gboolean
kind_list(
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	const gchar                *category,
	GPtrArray                 **items,
	GError                    **error
){
	AxisYoutube *self = kind->data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	guint i;

	*items = NULL;
	/* One whose job is gone (the daemon was killed during its fetch) is not
	 * shown as downloading for ever: it fails here, and is listed so. */
	axis_youtube_fail_orphans(self);
	if (!axis_db_pool_exec(self->pool, "SELECT " VIDEO_COLUMNS VIDEO_FROM
	        " WHERE y.state IN ('wanted', 'fetching') OR (y.state IN ('fetched', 'failed', 'cancelled')"
	        " AND y.updated_at > now() - interval '1 day') ORDER BY y.updated_at DESC LIMIT 1000", 0, NULL, &result,
	        &local)) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE, "the YouTube downloads could not be "
			"read: %s", local->message);
		return FALSE;
	}
	*items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		g_autoptr(AxisYoutubeVideo) video = video_at(result, i);
		AxisDownloadItem *item = axis_download_item_new();

		item->client_ref = g_strdup(video->video_id);
		item->name = axis_youtube_release_name(video);
		item->own_name = g_strdup(item->name);
		item->category = g_strdup(category != NULL ? category : AXIS_DOWNLOAD_DEFAULT_CATEGORY);
		item->size_bytes = video->size_bytes >= 0 ? video->size_bytes : video->expected_bytes;
		item->done_bytes = video->done_bytes;
		item->rate_bps = video->rate_bps;
		item->eta_s = video->eta_s;
		switch (video->state) {
		case AXIS_YOUTUBE_WANTED:
			item->state = AXIS_DOWNLOAD_ITEM_QUEUED;
			break;
		case AXIS_YOUTUBE_FETCHING:
			item->state = AXIS_DOWNLOAD_ITEM_DOWNLOADING;
			break;
		case AXIS_YOUTUBE_FETCHED:
			item->state = AXIS_DOWNLOAD_ITEM_COMPLETED;
			/* Axis's own path: the volume and the path on it. */
			if (video->volume_id > 0 && video->rel_path != NULL)
				item->path = g_strdup_printf("/%" G_GINT64_FORMAT "/%s", video->volume_id, video->rel_path);
			break;
		default:
			item->state = AXIS_DOWNLOAD_ITEM_FAILED;
			item->message = g_strdup(video->message != NULL ? video->message : "The download was stopped.");
			break;
		}
		g_ptr_array_add(*items, item);
	}
	return TRUE;
}

/* Stops a fetch: its record says so and its job is cancelled, which kills
 * yt-dlp; the job removes the folder it worked in.  A video already in the
 * library stays there: nothing of the library is deleted here. */
static gboolean
kind_remove(
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	const gchar                *client_ref,
	gboolean                    delete_data,
	GError                    **error
){
	AxisYoutube *self = kind->data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[1];
	AxisDbConn *db;
	gint64 job_id = 0;

	if (!axis_youtube_video_id_valid(client_ref)) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE, "there is no such YouTube download");
		return FALSE;
	}
	params[0] = client_ref;
	db = axis_db_pool_acquire(self->pool, &local);
	if (db == NULL || !axis_db_conn_exec(db,
	        "UPDATE youtube_video SET state = 'cancelled', message = 'Stopped by a person.', rate_bps = NULL,"
	        " eta_s = NULL, finished_at = now(), updated_at = now()"
	        " WHERE video_id = $1::text AND state IN ('wanted', 'fetching') RETURNING job_id", 1, params, &result,
	        &local)) {
		if (db != NULL)
			axis_db_pool_release(self->pool, db);
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREACHABLE, "the YouTube download could not be "
			"stopped: %s", local->message);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(result) > 0 && axis_db_result_get_int64(result, 0, 0, &job_id) && job_id > 0 &&
	    !axis_job_cancel(db, job_id, NULL, &local))
		g_warning("youtube: the job of a download that was stopped could not be cancelled: %s", local->message);
	axis_db_pool_release(self->pool, db);
	return TRUE;
}
