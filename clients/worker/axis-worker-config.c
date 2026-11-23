/*
 * axis-worker-config.c - axis-worker's configuration
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-worker-config.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/axis-storage-fs.h"
#include "storage/axis-storage-path.h"
#include "transcode/axis-convert-caps.h"
#include "transcode/axis-estimate-target.h"

#define TOKEN_LEN       43
#define TOKEN_FILE_MAX  4096
#define WORKER_ERROR    (axis_worker_config_error_quark())

static G_DEFINE_QUARK(axis-worker-config-error-quark, axis_worker_config_error)

/* Every key the file may have; the environment's names follow from them. */
static const struct
{
	const gchar *key;
	const gchar *env;
} keys[] = {
	{ "url", "AXIS_WORKER_URL" },
	{ "token", "AXIS_WORKER_TOKEN" },
	{ "token_file", "AXIS_WORKER_TOKEN_FILE" },
	{ "ca_file", "AXIS_WORKER_CA_FILE" },
	{ "allow_insecure_http", "AXIS_WORKER_ALLOW_INSECURE_HTTP" },
	{ "request_timeout", "AXIS_WORKER_REQUEST_TIMEOUT" },
	{ "scratch", "AXIS_WORKER_SCRATCH" },
	{ "state", "AXIS_WORKER_STATE" },
	{ "volumes", "AXIS_WORKER_VOLUMES" },
	{ "volumes_without_file_identity", "AXIS_WORKER_VOLUMES_WITHOUT_FILE_IDENTITY" },
	{ "max_jobs", "AXIS_WORKER_MAX_JOBS" },
	{ "encoder_families", "AXIS_WORKER_ENCODER_FAMILIES" },
	{ "ffmpeg", "AXIS_WORKER_FFMPEG" },
	{ "ffprobe", "AXIS_WORKER_FFPROBE" },
	{ "volume_check_interval", "AXIS_WORKER_VOLUME_CHECK_INTERVAL" },
	{ "log_level", "AXIS_WORKER_LOG_LEVEL" },
	{ NULL, NULL }
};

void
axis_worker_config_free(AxisWorkerConfig *self)
{
	if (self == NULL)
		return;
	g_free(self->url);
	g_free(self->host);
	if (self->token != NULL) {
		memset(self->token, 0, strlen(self->token));
		g_free(self->token);
	}
	g_free(self->ca_file);
	g_free(self->scratch);
	g_free(self->state);
	g_strfreev(self->volumes);
	g_strfreev(self->no_identity);
	g_strfreev(self->ffmpeg);
	g_strfreev(self->ffprobe);
	g_free(self);
}

static gboolean
fail(
	GError      **error,
	const gchar  *format,
	...
) G_GNUC_PRINTF(2, 3);

static gboolean
fail(
	GError      **error,
	const gchar  *format,
	...
){
	va_list args;
	gchar *text;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error_literal(error, WORKER_ERROR, 0, text);
	g_free(text);
	return FALSE;
}

static void
wipe(gchar *secret)
{
	if (secret != NULL) {
		memset(secret, 0, strlen(secret));
		g_free(secret);
	}
}

/* ── Raw values: the file, then the environment ─────────────────── */

typedef struct
{
	GHashTable *values;	/* key -> value (copies), the token's wiped */
	GHashTable *from;	/* key -> "file" or the variable's name */
	GHashTable *lists;	/* key -> GStrv, from the file's lists */
} Raw;

static void
raw_clear(Raw *raw)
{
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (raw->values != NULL) {
		g_hash_table_iter_init(&iter, raw->values);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			if (strcmp(key, "token") == 0)
				memset(value, 0, strlen(value));
		}
		g_hash_table_unref(raw->values);
	}
	if (raw->from != NULL)
		g_hash_table_unref(raw->from);
	if (raw->lists != NULL)
		g_hash_table_unref(raw->lists);
}

static gboolean
key_known(const gchar *key)
{
	guint i;

	for (i = 0; keys[i].key != NULL; i++) {
		if (strcmp(keys[i].key, key) == 0)
			return TRUE;
	}
	return FALSE;
}

/* A key name may be printed only when it cannot be a token pasted by
 * mistake: short, and lower-case letters and '_' only. */
static gboolean
printable_key(const gchar *key)
{
	const gchar *p;

	if (strlen(key) > 32)
		return FALSE;
	for (p = key; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || *p == '_'))
			return FALSE;
	}
	return TRUE;
}

static gboolean
read_file(
	Raw          *raw,
	const gchar  *path,
	GError      **error
){
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_auto(GStrv) groups = NULL;
	g_auto(GStrv) names = NULL;
	gchar *text = NULL;
	gsize length = 0;
	struct stat st;
	guint i;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return fail(error, "the configuration file %s cannot be read", path);
	if (st.st_size > AXIS_WORKER_CONFIG_MAX_FILE_SIZE)
		return fail(error, "the configuration file %s is larger than %d bytes", path, AXIS_WORKER_CONFIG_MAX_FILE_SIZE);
	if (!g_file_get_contents(path, &text, &length, NULL))
		return fail(error, "the configuration file %s cannot be read", path);
	/* The reader's own messages quote the line they could not read, which
	 * could be the token: none of them is passed on. */
	if (!g_key_file_load_from_data(file, text, length, G_KEY_FILE_NONE, NULL)) {
		wipe(text);
		return fail(error, "the configuration file %s is not a key file (\"key = value\" lines under [worker])",
			path);
	}
	wipe(text);

	groups = g_key_file_get_groups(file, NULL);
	for (i = 0; groups[i] != NULL; i++) {
		if (strcmp(groups[i], "worker") != 0)
			return fail(error, "the configuration file %s has a group other than [worker]", path);
	}
	if (!g_key_file_has_group(file, "worker"))
		return TRUE;
	names = g_key_file_get_keys(file, "worker", NULL, NULL);
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		const gchar *key = names[i];

		if (!key_known(key)) {
			if (printable_key(key))
				return fail(error, "the configuration file %s has the key '%s', which axis-worker does not know",
					path, key);
			return fail(error, "the configuration file %s has a key axis-worker does not know", path);
		}
		if (strcmp(key, "volumes") == 0 || strcmp(key, "volumes_without_file_identity") == 0 ||
		    strcmp(key, "ffmpeg") == 0 || strcmp(key, "ffprobe") == 0) {
			gchar **list = g_key_file_get_string_list(file, "worker", key, NULL, NULL);

			if (list == NULL)
				return fail(error, "worker.%s in %s cannot be read", key, path);
			g_hash_table_replace(raw->lists, g_strdup(key), list);
		} else {
			gchar *value = g_key_file_get_string(file, "worker", key, NULL);

			if (value == NULL)
				return fail(error, "worker.%s in %s cannot be read", key, path);
			g_hash_table_replace(raw->values, g_strdup(key), value);
		}
		g_hash_table_replace(raw->from, g_strdup(key), g_strdup_printf("worker.%s in %s", key, path));
	}
	return TRUE;
}

static void
read_environment(
	Raw    *raw,
	gchar **envp
){
	guint i;

	for (i = 0; keys[i].key != NULL; i++) {
		const gchar *value = g_environ_getenv(envp, keys[i].env);

		if (value == NULL || value[0] == '\0')
			continue;
		/* The environment replaces the file's value (and a list of it). */
		g_hash_table_remove(raw->lists, keys[i].key);
		g_hash_table_replace(raw->values, g_strdup(keys[i].key), g_strdup(value));
		g_hash_table_replace(raw->from, g_strdup(keys[i].key), g_strdup(keys[i].env));
	}
}

static const gchar *
raw_get(
	Raw         *raw,
	const gchar *key
){
	return g_hash_table_lookup(raw->values, key);
}

static const gchar *
raw_from(
	Raw         *raw,
	const gchar *key
){
	const gchar *from = g_hash_table_lookup(raw->from, key);

	return from != NULL ? from : key;
}

/* ── Values ─────────────────────────────────────────────────────── */

static gboolean
parse_bool(
	const gchar *text,
	gboolean    *value
){
	if (g_ascii_strcasecmp(text, "true") == 0 || g_ascii_strcasecmp(text, "yes") == 0 || strcmp(text, "1") == 0) {
		*value = TRUE;
		return TRUE;
	}
	if (g_ascii_strcasecmp(text, "false") == 0 || g_ascii_strcasecmp(text, "no") == 0 || strcmp(text, "0") == 0) {
		*value = FALSE;
		return TRUE;
	}
	return FALSE;
}

static gboolean
parse_uint(
	const gchar *text,
	guint        minimum,
	guint        maximum,
	guint       *value
){
	guint64 v = 0;
	gchar *end = NULL;

	if (text == NULL || !g_ascii_isdigit(text[0]))
		return FALSE;
	v = g_ascii_strtoull(text, &end, 10);
	if (end == NULL || *end != '\0' || v < minimum || v > maximum)
		return FALSE;
	*value = (guint)v;
	return TRUE;
}

gboolean
axis_worker_host_is_loopback(const gchar *host)
{
	g_autoptr(GInetAddress) address = NULL;
	g_autofree gchar *bare = NULL;

	if (host == NULL || host[0] == '\0')
		return FALSE;
	if (g_ascii_strcasecmp(host, "localhost") == 0)
		return TRUE;
	bare = host[0] == '[' && host[strlen(host) - 1] == ']' ? g_strndup(host + 1, strlen(host) - 2) : g_strdup(host);
	address = g_inet_address_new_from_string(bare);
	return address != NULL && g_inet_address_get_is_loopback(address);
}

/* The daemon's address: http or https, a host, maybe a port and a path
 * prefix of plain characters; nothing else. */
static gboolean
parse_url(
	AxisWorkerConfig  *config,
	const gchar       *text,
	const gchar       *from,
	GError           **error
){
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *path = NULL;
	const gchar *scheme;
	const gchar *p;
	gint port;

	if (text == NULL)
		return fail(error, "the daemon's address is required: set url in the configuration file or AXIS_WORKER_URL");
	uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return fail(error, "%s is not an http or https URL", from);
	scheme = g_uri_get_scheme(uri);
	if (scheme == NULL || (g_ascii_strcasecmp(scheme, "http") != 0 && g_ascii_strcasecmp(scheme, "https") != 0))
		return fail(error, "%s is not an http or https URL", from);
	if (g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL)
		return fail(error, "%s may have no user name, password, query or fragment", from);
	if (g_uri_get_host(uri) == NULL || g_uri_get_host(uri)[0] == '\0')
		return fail(error, "%s has no host", from);
	path = g_strdup(g_uri_get_path(uri) != NULL ? g_uri_get_path(uri) : "");
	for (p = path; *p != '\0'; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-' || *p == '~'))
			return fail(error, "%s has a path with characters other than letters, digits and / . _ - ~", from);
	}
	if (strstr(path, "//") != NULL || strstr(path, "/../") != NULL || g_str_has_suffix(path, "/..") ||
	    strstr(path, "/./") != NULL || g_str_has_suffix(path, "/."))
		return fail(error, "%s has a path with empty, \".\" or \"..\" parts", from);
	while (path[0] != '\0' && path[strlen(path) - 1] == '/')
		path[strlen(path) - 1] = '\0';

	config->https = g_ascii_strcasecmp(scheme, "https") == 0;
	config->host = g_strdup(g_uri_get_host(uri));
	config->loopback = axis_worker_host_is_loopback(config->host);
	port = g_uri_get_port(uri);
	{
		g_autofree gchar *host = strchr(config->host, ':') != NULL ? g_strdup_printf("[%s]", config->host) :
			g_strdup(config->host);

		config->url = port > 0 ? g_strdup_printf("%s://%s:%d%s", config->https ? "https" : "http", host, port, path) :
			g_strdup_printf("%s://%s%s", config->https ? "https" : "http", host, path);
	}
	return TRUE;
}

/* The token: from exactly one place, in the one form tokens have. */
static gboolean
load_token(
	AxisWorkerConfig  *config,
	Raw               *raw,
	GError           **error
){
	const gchar *inline_token = raw_get(raw, "token");
	const gchar *file = raw_get(raw, "token_file");
	gchar buffer[TOKEN_FILE_MAX + 1];
	gssize n;
	gsize length;
	struct stat st;
	gint fd;
	guint i;

	if (inline_token != NULL && file != NULL)
		return fail(error, "the token is given twice (%s and %s): give it once", raw_from(raw, "token"),
			raw_from(raw, "token_file"));
	if (inline_token == NULL && file == NULL)
		return fail(error, "the worker's token is required: set token_file in the configuration file or "
			"AXIS_WORKER_TOKEN_FILE (a file holding the token the Workers page showed)");
	if (inline_token != NULL) {
		config->token = g_strdup(inline_token);
	} else {
		fd = open(file, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			return fail(error, "the token file %s cannot be read: %s", file, g_strerror(errno));
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			close(fd);
			return fail(error, "the token file %s is not a regular file", file);
		}
		if ((st.st_mode & 077) != 0)
			g_warning("the token file %s can be read by other users; it should be readable by this one only", file);
		length = 0;
		while (length < sizeof(buffer) - 1 && (n = read(fd, buffer + length, sizeof(buffer) - 1 - length)) > 0)
			length += (gsize)n;
		close(fd);
		buffer[length] = '\0';
		while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r' || buffer[length - 1] == ' ' ||
		       buffer[length - 1] == '\t'))
			buffer[--length] = '\0';
		config->token = g_strdup(buffer);
		memset(buffer, 0, sizeof(buffer));
	}
	if (strlen(config->token) != TOKEN_LEN)
		return fail(error, "the token (%s) is not in the form the daemon gives", raw_from(raw,
			inline_token != NULL ? "token" : "token_file"));
	for (i = 0; i < TOKEN_LEN; i++) {
		gchar c = config->token[i];

		if (!(g_ascii_isalnum(c) || c == '-' || c == '_'))
			return fail(error, "the token (%s) is not in the form the daemon gives", raw_from(raw,
				inline_token != NULL ? "token" : "token_file"));
	}
	return TRUE;
}

/* An absolute, normalised path, as given. */
static gboolean
check_path(
	const gchar  *path,
	const gchar  *what,
	GError      **error
){
	g_autofree gchar *normalised = NULL;

	if (path == NULL || !axis_storage_path_normalize(path, TRUE, &normalised, NULL) || strcmp(normalised, path) != 0)
		return fail(error, "%s must be an absolute path in its plain form (no '.', '..', '//' or trailing '/')", what);
	return TRUE;
}

/* Made 0700 if missing; then it must be a directory of this user that
 * nobody else can open, and not a link. */
static gboolean
private_directory(
	const gchar  *path,
	const gchar  *what,
	GError      **error
){
	struct stat st;

	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return fail(error, "the %s directory %s cannot be made: %s", what, path, g_strerror(errno));
	if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0)
		return fail(error, "the %s directory %s is not a private directory of this user (mode 0700, not a link)",
			what, path);
	return TRUE;
}

/* @path with every link resolved; when it is not there yet, its parent's
 * real path and its last part.  NULL when neither resolves. */
static gchar *
real_path(const gchar *path)
{
	g_autofree gchar *parent = NULL;
	g_autofree gchar *base = NULL;
	gchar *real = realpath(path, NULL);
	gchar *out;

	if (real != NULL) {
		out = g_strdup(real);
		free(real);
		return out;
	}
	if (errno != ENOENT)
		return NULL;
	parent = g_path_get_dirname(path);
	base = g_path_get_basename(path);
	real = realpath(parent, NULL);
	if (real == NULL)
		return NULL;
	out = g_build_filename(real, base, NULL);
	free(real);
	return out;
}

/* Nothing an encoder could write may land on a volume: the directory is
 * neither on one's path (as given, and with links resolved) nor on the
 * filesystem of one that is a mount of its own.  Checked before the
 * directory is made (so that it is never made on a volume) and again
 * after. */
static gboolean
off_the_volumes(
	const gchar  *path,
	const gchar  *what,
	gchar       **volumes,
	GError      **error
){
	g_autofree gchar *real = real_path(path);
	struct stat st;
	gboolean exists;
	guint i;

	if (real == NULL)
		return fail(error, "the %s directory %s cannot be looked at", what, path);
	exists = stat(path, &st) == 0;
	for (i = 0; volumes != NULL && volumes[i] != NULL; i++) {
		g_autofree gchar *volume = real_path(volumes[i]);
		AxisStorageFsIdentity identity;
		gint err = 0;
		gint fd;

		if (axis_storage_path_contains(volumes[i], path) || axis_storage_path_contains(path, volumes[i]) ||
		    (volume != NULL && (axis_storage_path_contains(volume, real) || axis_storage_path_contains(real, volume))))
			return fail(error, "the %s directory %s is on or holds the volume path %s", what, path, volumes[i]);
		if (!exists)
			continue;
		fd = axis_storage_fs_open_path(volumes[i], &err);
		if (fd < 0)
			continue;
		if (axis_storage_fs_identify(fd, &identity, &err) && identity.mount_root && identity.dev == (guint64)st.st_dev) {
			close(fd);
			return fail(error, "the %s directory %s is on the filesystem of the volume at %s", what, path, volumes[i]);
		}
		close(fd);
	}
	return TRUE;
}

static gboolean
program(
	AxisWorkerConfig  *config,
	Raw               *raw,
	const gchar       *key,
	gchar           ***out,
	GError           **error
){
	gchar **list = g_hash_table_lookup(raw->lists, key);
	const gchar *single = raw_get(raw, key);
	gchar **command;

	if (list != NULL) {
		if (list[0] == NULL)
			return fail(error, "%s is empty", raw_from(raw, key));
		command = g_strdupv(list);
	} else if (single != NULL) {
		command = g_new0(gchar *, 2);
		command[0] = g_strdup(single);
	} else {
		gchar *found = g_find_program_in_path(key);

		if (found == NULL)
			return fail(error, "%s is not on PATH: set %s in the configuration file", key, key);
		command = g_new0(gchar *, 2);
		command[0] = found;
	}
	if (!g_path_is_absolute(command[0]) || !g_file_test(command[0], G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_EXECUTABLE)) {
		g_strfreev(command);
		return fail(error, "%s is not an absolute path to a program", raw_from(raw, key));
	}
	*out = command;
	return TRUE;
}

static gboolean
families(
	AxisWorkerConfig  *config,
	Raw               *raw,
	GError           **error
){
	const gchar *text = raw_get(raw, "encoder_families");
	g_auto(GStrv) words = NULL;
	guint allowed = 0;
	guint i;

	if (text == NULL) {
		config->skip_families = 0;
		return TRUE;
	}
	words = g_strsplit_set(text, ", ", -1);
	for (i = 0; words[i] != NULL; i++) {
		AxisEncoderFamily family;

		if (words[i][0] == '\0')
			continue;
		if (!axis_encoder_family_from_string(words[i], &family))
			return fail(error, "%s names an encoder family other than software, nvenc and vaapi",
				raw_from(raw, "encoder_families"));
		allowed |= AXIS_CONVERT_FAMILY_BIT(family);
	}
	if (allowed == 0)
		return fail(error, "%s names no encoder family", raw_from(raw, "encoder_families"));
	config->skip_families = (AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_SOFTWARE) | AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_NVENC) |
		AXIS_CONVERT_FAMILY_BIT(AXIS_ENCODER_VAAPI)) & ~allowed;
	return TRUE;
}

/* A list of distinct paths: the file's ';'-separated list, or the
 * environment's ','-separated one. */
static gboolean
path_list(
	Raw          *raw,
	const gchar  *key,
	gchar      ***paths,
	GError      **error
){
	gchar **list = g_hash_table_lookup(raw->lists, key);
	const gchar *single = raw_get(raw, key);
	g_auto(GStrv) parts = NULL;
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	guint i;
	guint j;

	if (list == NULL && single != NULL)
		parts = g_strsplit(single, ",", -1);
	for (i = 0; (list != NULL ? list[i] : parts != NULL ? parts[i] : NULL) != NULL; i++) {
		g_autofree gchar *path = g_strstrip(g_strdup(list != NULL ? list[i] : parts[i]));
		g_autofree gchar *what = g_strdup_printf("every path of %s", raw_from(raw, key));

		if (path[0] == '\0')
			continue;
		if (!check_path(path, what, error)) {
			g_ptr_array_unref(out);
			return FALSE;
		}
		for (j = 0; j < out->len; j++) {
			if (strcmp(g_ptr_array_index(out, j), path) == 0) {
				g_ptr_array_unref(out);
				return fail(error, "the path %s is given twice in %s", path, raw_from(raw, key));
			}
		}
		if (out->len >= AXIS_WORKER_CONFIG_MAX_VOLUMES) {
			g_ptr_array_unref(out);
			return fail(error, "more than %d paths are given in %s", AXIS_WORKER_CONFIG_MAX_VOLUMES,
				raw_from(raw, key));
		}
		g_ptr_array_add(out, g_steal_pointer(&path));
	}
	g_ptr_array_add(out, NULL);
	*paths = (gchar **)g_ptr_array_free(out, FALSE);
	return TRUE;
}

/* The volume paths, and those of them seen through a mount that does not
 * keep file identities: each of the latter must be one of the former, as
 * written there. */
static gboolean
volumes(
	AxisWorkerConfig  *config,
	Raw               *raw,
	GError           **error
){
	guint i;

	if (!path_list(raw, "volumes", &config->volumes, error) ||
	    !path_list(raw, "volumes_without_file_identity", &config->no_identity, error))
		return FALSE;
	for (i = 0; config->no_identity[i] != NULL; i++) {
		if (!g_strv_contains((const gchar *const *)config->volumes, config->no_identity[i]))
			return fail(error, "%s names %s, which is not one of the volume paths (volumes, AXIS_WORKER_VOLUMES); "
				"write it as it is written there", raw_from(raw, "volumes_without_file_identity"),
				config->no_identity[i]);
	}
	return TRUE;
}

gboolean
axis_worker_config_keeps_identity(
	const AxisWorkerConfig *self,
	const gchar            *path
){
	g_return_val_if_fail(self != NULL, TRUE);

	return path == NULL || self->no_identity == NULL ||
		!g_strv_contains((const gchar *const *)self->no_identity, path);
}

AxisWorkerConfig *
axis_worker_config_load(
	const gchar  *path,
	gchar       **envp,
	gboolean      need_token,
	GError      **error
){
	g_autoptr(AxisWorkerConfig) config = g_new0(AxisWorkerConfig, 1);
	g_autofree gchar *uid_scratch = g_strdup_printf("axis-worker-%u", (guint)getuid());
	g_autofree gchar *uid_state = g_strdup_printf("axis-worker-state-%u", (guint)getuid());
	const gchar *text;
	Raw raw;
	gboolean ok = FALSE;

	memset(&raw, 0, sizeof(raw));
	raw.values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	raw.from = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	raw.lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
	config->max_jobs = 1;
	config->request_timeout_ms = 30000;
	config->volume_check_ms = 60000;
	config->log_level = AXIS_LOG_LEVEL_INFO;

	if (path != NULL && !read_file(&raw, path, error))
		goto out;
	read_environment(&raw, envp);

	if ((text = raw_get(&raw, "log_level")) != NULL && !axis_log_level_from_string(text, &config->log_level)) {
		fail(error, "%s is not one of debug, info, warning, error", raw_from(&raw, "log_level"));
		goto out;
	}
	if ((text = raw_get(&raw, "allow_insecure_http")) != NULL && !parse_bool(text, &config->allow_insecure_http)) {
		fail(error, "%s is not true or false", raw_from(&raw, "allow_insecure_http"));
		goto out;
	}
	if ((text = raw_get(&raw, "request_timeout")) != NULL) {
		guint seconds = 0;

		if (!parse_uint(text, 1, 300, &seconds)) {
			fail(error, "%s is not a number of seconds from 1 to 300", raw_from(&raw, "request_timeout"));
			goto out;
		}
		config->request_timeout_ms = seconds * 1000;
	}
	if ((text = raw_get(&raw, "max_jobs")) != NULL && !parse_uint(text, 1, 16, &config->max_jobs)) {
		fail(error, "%s is not a number from 1 to 16", raw_from(&raw, "max_jobs"));
		goto out;
	}
	if ((text = raw_get(&raw, "volume_check_interval")) != NULL) {
		gchar *end = NULL;
		gdouble seconds = g_ascii_strtod(text, &end);

		if (end == NULL || *end != '\0' || !(seconds >= 0.05 && seconds <= 3600.0)) {
			fail(error, "%s is not a number of seconds from 0.05 to 3600", raw_from(&raw, "volume_check_interval"));
			goto out;
		}
		config->volume_check_ms = (guint)(seconds * 1000.0);
	}
	if (!families(config, &raw, error) || !volumes(config, &raw, error))
		goto out;
	if (!parse_url(config, raw_get(&raw, "url"), raw_from(&raw, "url"), error))
		goto out;
	/* The token goes only where it is protected, unless told otherwise. */
	if (!config->https && !config->loopback && !config->allow_insecure_http) {
		fail(error, "refusing to send the worker's token over plain http to %s, which is not this machine: use https "
			"(through a reverse proxy with TLS), or set allow_insecure_http = true if the network between them is "
			"trusted", config->host);
		goto out;
	}
	if (!config->https && !config->loopback)
		g_warning("the daemon's address is plain http to %s, which is not this machine: the worker's token crosses "
			"the network unencrypted (allow_insecure_http is set)", config->host);
	if ((text = raw_get(&raw, "ca_file")) != NULL) {
		if (!g_path_is_absolute(text) || !g_file_test(text, G_FILE_TEST_IS_REGULAR)) {
			fail(error, "%s is not a readable file", raw_from(&raw, "ca_file"));
			goto out;
		}
		config->ca_file = g_strdup(text);
	}
	if (need_token && !load_token(config, &raw, error))
		goto out;

	config->scratch = g_strdup(raw_get(&raw, "scratch") != NULL ? raw_get(&raw, "scratch") : NULL);
	if (config->scratch == NULL)
		config->scratch = g_build_filename(g_get_tmp_dir(), uid_scratch, NULL);
	config->state = g_strdup(raw_get(&raw, "state") != NULL ? raw_get(&raw, "state") : NULL);
	if (config->state == NULL)
		config->state = g_build_filename(g_get_tmp_dir(), uid_state, NULL);
	if (!check_path(config->scratch, "the scratch directory", error) ||
	    !check_path(config->state, "the state directory", error))
		goto out;
	if (strcmp(config->scratch, config->state) == 0 || axis_storage_path_contains(config->scratch, config->state) ||
	    axis_storage_path_contains(config->state, config->scratch)) {
		fail(error, "the scratch and state directories must be apart");
		goto out;
	}
	if (need_token) {
		if (!off_the_volumes(config->scratch, "scratch", config->volumes, error) ||
		    !off_the_volumes(config->state, "state", config->volumes, error) ||
		    !private_directory(config->scratch, "scratch", error) || !private_directory(config->state, "state", error) ||
		    !off_the_volumes(config->scratch, "scratch", config->volumes, error) ||
		    !off_the_volumes(config->state, "state", config->volumes, error))
			goto out;
		if (!program(config, &raw, "ffmpeg", &config->ffmpeg, error) ||
		    !program(config, &raw, "ffprobe", &config->ffprobe, error))
			goto out;
	}
	ok = TRUE;

out:
	raw_clear(&raw);
	return ok ? g_steal_pointer(&config) : NULL;
}

gchar *
axis_worker_config_describe(const AxisWorkerConfig *self)
{
	g_autofree gchar *volumes = self->volumes != NULL && self->volumes[0] != NULL ?
		g_strjoinv(",", self->volumes) : g_strdup("(none)");
	g_autofree gchar *no_identity = self->no_identity != NULL && self->no_identity[0] != NULL ?
		g_strjoinv(",", self->no_identity) : g_strdup("(none)");
	GString *families = g_string_new(NULL);
	guint family;

	for (family = 0; family < AXIS_ENCODER_COUNT; family++) {
		if ((self->skip_families & AXIS_CONVERT_FAMILY_BIT(family)) == 0)
			g_string_append_printf(families, "%s%s", families->len > 0 ? "," : "",
				axis_encoder_family_to_string((AxisEncoderFamily)family));
	}
	{
		gchar *text = g_strdup_printf("url=%s ca_file=%s scratch=%s state=%s volumes=%s "
			"volumes_without_file_identity=%s max_jobs=%u "
			"encoder_families=%s ffmpeg=%s ffprobe=%s request_timeout=%us volume_check_interval=%ums log_level=%s",
			self->url, self->ca_file != NULL ? self->ca_file : "(system)", self->scratch, self->state, volumes,
			no_identity, self->max_jobs, families->str, self->ffmpeg != NULL ? self->ffmpeg[0] : "?",
			self->ffprobe != NULL ? self->ffprobe[0] : "?", self->request_timeout_ms / 1000, self->volume_check_ms,
			axis_log_level_to_string(self->log_level));

		g_string_free(families, TRUE);
		return text;
	}
}
