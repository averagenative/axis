/*
 * axis-config.c - bootstrap configuration (file + environment)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-config.h"
#include "core/axis-error.h"
#include "core/axis-outbound-parse.h"
#include "core/axis-proxy.h"
#include "storage/axis-storage-path.h"

#include <string.h>

struct _AxisConfig
{
	gchar        *host;
	guint16       port;
	gchar        *trusted_proxies;	/* canonical list, "" for none */
	AxisSecureCookies secure_cookies;
	gchar        *database_url;	/* secret */
	guint         database_pool_size;
	gboolean      database_pool_size_set;	/* by the file or the environment */
	guint         database_pool_size_used;	/* the size axisd chose (0: not chosen yet) */
	gint          database_wait;	/* seconds, or AXIS_CONFIG_WAIT_FOREVER */
	AxisLogLevel  log_level;
	AxisLogFormat log_format;
	guint         jobs_grace_period;
	gchar        *jobs_concurrency;	/* normalised "queue:n,queue:n", or NULL */
	guint         worker_lease;
	gchar        *data_dir;		/* absolute, normalised */
	gchar        *admin_username;	/* as given; checked when it is used */
	gchar        *admin_password;	/* secret */
	gchar        *metrics_token;	/* secret */
	gchar        *outbound_bind;	/* canonical (an interface or an address), "" for none */
	AxisOutboundScope outbound_scope;
	guint         head_timeout;	/* seconds */
	guint         body_timeout;
	guint         idle_timeout;
	guint         max_connections;
	guint         max_per_address;
};

/*
 * Setters apply one validated value to a staging config.  @label names
 * the setting in error messages ("server.port" or "AXIS_LISTEN_PORT").
 * The setter of a secret never puts the value in a message.
 */
typedef gboolean (*SetFunc)(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
);

typedef struct
{
	const gchar *group;
	const gchar *key;
	const gchar *env;
	SetFunc      set;
	gboolean     secret;	/* also read from the file <env>_FILE names */
} Setting;

static gboolean set_host(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_port(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_trusted_proxies(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_secure_cookies(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_url(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_pool_size(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_level(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_concurrency(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_grace_period(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_worker_lease(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_data_dir(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_database_wait(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_log_format(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_admin_username(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_admin_password(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_metrics_token(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_outbound_bind(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_outbound_scope(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_head_timeout(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_body_timeout(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_idle_timeout(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_max_connections(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);
static gboolean set_max_per_address(AxisConfig *cfg, const gchar *value, const gchar *label, GError **error);

/* The one list of settings; file keys and environment names both come
 * from here.  A secret may also come from the file that its variable's
 * _FILE form names. */
static const Setting settings[] = {
	{ "server",   "host",         "AXIS_LISTEN_HOST",        set_host,         FALSE },
	{ "server",   "port",         "AXIS_LISTEN_PORT",        set_port,         FALSE },
	{ "server",   "trusted_proxies", "AXIS_TRUSTED_PROXIES", set_trusted_proxies, FALSE },
	{ "server",   "secure_cookies", "AXIS_SECURE_COOKIES",   set_secure_cookies, FALSE },
	{ "server",   "head_timeout", "AXIS_HEAD_TIMEOUT",       set_head_timeout, FALSE },
	{ "server",   "body_timeout", "AXIS_BODY_TIMEOUT",       set_body_timeout, FALSE },
	{ "server",   "idle_timeout", "AXIS_IDLE_TIMEOUT",       set_idle_timeout, FALSE },
	{ "server",   "max_connections", "AXIS_MAX_CONNECTIONS", set_max_connections, FALSE },
	{ "server",   "max_connections_per_address", "AXIS_MAX_CONNECTIONS_PER_ADDRESS", set_max_per_address, FALSE },
	{ "database", "url",         "AXIS_DATABASE_URL",       set_url,          TRUE  },
	{ "database", "pool_size",    "AXIS_DATABASE_POOL_SIZE", set_pool_size,    FALSE },
	{ "database", "wait",         "AXIS_DATABASE_WAIT",      set_database_wait, FALSE },
	{ "log",      "level",        "AXIS_LOG_LEVEL",          set_level,        FALSE },
	{ "log",      "format",       "AXIS_LOG_FORMAT",         set_log_format,   FALSE },
	{ "jobs",     "concurrency",  "AXIS_JOBS_CONCURRENCY",   set_concurrency,  FALSE },
	{ "jobs",     "grace_period", "AXIS_JOBS_GRACE_PERIOD",  set_grace_period, FALSE },
	{ "workers",  "lease",        "AXIS_WORKER_LEASE",       set_worker_lease, FALSE },
	{ "data",     "dir",          "AXIS_DATA_DIR",           set_data_dir,     FALSE },
	{ "setup",    "admin_username", "AXIS_ADMIN_USERNAME",   set_admin_username, FALSE },
	{ "setup",    "admin_password", "AXIS_ADMIN_PASSWORD",   set_admin_password, TRUE  },
	{ "metrics",  "token",        "AXIS_METRICS_TOKEN",      set_metrics_token, TRUE  },
	{ "outbound", "bind",         "AXIS_OUTBOUND_BIND",      set_outbound_bind, FALSE },
	{ "outbound", "bind_scope",   "AXIS_OUTBOUND_BIND_SCOPE", set_outbound_scope, FALSE }
};

/* Frees a string that may hold a secret, wiping it first. */
static void
secret_free(gchar *value)
{
	if (value == NULL)
		return;
	memset(value, 0, strlen(value));
	g_free(value);
}

AxisConfig *
axis_config_new(void)
{
	AxisConfig *self = g_new0(AxisConfig, 1);

	self->host = g_strdup(AXIS_CONFIG_DEFAULT_HOST);
	self->port = AXIS_CONFIG_DEFAULT_PORT;
	self->trusted_proxies = g_strdup("");
	self->secure_cookies = AXIS_SECURE_COOKIES_AUTO;
	self->database_url = NULL;
	self->database_pool_size = AXIS_CONFIG_DEFAULT_POOL_SIZE;
	self->database_wait = AXIS_CONFIG_WAIT_FOREVER;
	self->log_level = AXIS_CONFIG_DEFAULT_LOG_LEVEL;
	self->log_format = AXIS_LOG_FORMAT_TEXT;
	self->jobs_grace_period = AXIS_CONFIG_DEFAULT_GRACE_PERIOD;
	self->jobs_concurrency = NULL;
	self->worker_lease = AXIS_CONFIG_DEFAULT_WORKER_LEASE;
	self->data_dir = g_strdup(AXIS_CONFIG_DEFAULT_DATA_DIR);
	self->outbound_bind = g_strdup("");
	self->outbound_scope = AXIS_OUTBOUND_SCOPE_INTERNET;
	self->head_timeout = AXIS_CONFIG_DEFAULT_HEAD_TIMEOUT;
	self->body_timeout = AXIS_CONFIG_DEFAULT_BODY_TIMEOUT;
	self->idle_timeout = AXIS_CONFIG_DEFAULT_IDLE_TIMEOUT;
	self->max_connections = AXIS_CONFIG_DEFAULT_MAX_CONNECTIONS;
	self->max_per_address = AXIS_CONFIG_DEFAULT_MAX_PER_ADDRESS;

	return self;
}

/* Frees what @self points to, wiping the secrets, but not @self. */
static void
config_clear(AxisConfig *self)
{
	g_free(self->host);
	g_free(self->trusted_proxies);
	secret_free(self->database_url);
	g_free(self->jobs_concurrency);
	g_free(self->data_dir);
	g_free(self->admin_username);
	secret_free(self->admin_password);
	secret_free(self->metrics_token);
	g_free(self->outbound_bind);
}

void
axis_config_free(AxisConfig *self)
{
	if (self == NULL)
		return;

	config_clear(self);
	g_free(self);
}

static AxisConfig *
config_copy(const AxisConfig *self)
{
	AxisConfig *copy = g_new0(AxisConfig, 1);

	*copy = *self;
	copy->host = g_strdup(self->host);
	copy->trusted_proxies = g_strdup(self->trusted_proxies);
	copy->database_url = g_strdup(self->database_url);
	copy->jobs_concurrency = g_strdup(self->jobs_concurrency);
	copy->data_dir = g_strdup(self->data_dir);
	copy->admin_username = g_strdup(self->admin_username);
	copy->admin_password = g_strdup(self->admin_password);
	copy->metrics_token = g_strdup(self->metrics_token);
	copy->outbound_bind = g_strdup(self->outbound_bind);

	return copy;
}

/* Replace the contents of @self with those of @staged and free @staged. */
static void
config_commit(
	AxisConfig *self,
	AxisConfig *staged
){
	AxisConfig old = *self;

	*self = *staged;
	config_clear(&old);
	g_free(staged);
}

/* Whole decimal integer in @min..@max: digits only, no sign, no space. */
static gboolean
parse_uint(
	const gchar *text,
	guint        min,
	guint        max,
	guint       *result
){
	const gchar *p;
	guint value = 0;

	if (text == NULL || *text == '\0')
		return FALSE;

	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return FALSE;
		value = value * 10 + (guint)(*p - '0');
		if (value > max)
			return FALSE;
	}

	if (value < min)
		return FALSE;

	*result = value;
	return TRUE;
}

static gboolean
set_host(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	if (value == NULL || *value == '\0') {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: host must not be empty", label);
		return FALSE;
	}

	g_free(cfg->host);
	cfg->host = g_strdup(value);
	return TRUE;
}

static gboolean
set_port(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	guint port;

	if (!parse_uint(value, 1, 65535, &port)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid port '%s' (expected a number from 1 to 65535)",
			label, value != NULL ? value : "");
		return FALSE;
	}

	cfg->port = (guint16)port;
	return TRUE;
}

static gboolean
set_trusted_proxies(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	g_autoptr(AxisProxyList) list = NULL;
	g_autoptr(GError) local = NULL;

	list = axis_proxy_list_parse(value != NULL ? value : "", &local);
	if (list == NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG, "%s: %s", label, local->message);
		return FALSE;
	}

	g_free(cfg->trusted_proxies);
	cfg->trusted_proxies = axis_proxy_list_to_string(list);
	return TRUE;
}

static gboolean
set_secure_cookies(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	if (g_strcmp0(value, "auto") == 0) {
		cfg->secure_cookies = AXIS_SECURE_COOKIES_AUTO;
	} else if (g_strcmp0(value, "always") == 0) {
		cfg->secure_cookies = AXIS_SECURE_COOKIES_ALWAYS;
	} else {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid value '%s' (expected auto or always)",
			label, value != NULL ? value : "");
		return FALSE;
	}

	return TRUE;
}

static gboolean
set_url(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	secret_free(cfg->database_url);
	cfg->database_url = (value != NULL && *value != '\0') ? g_strdup(value) : NULL;
	return TRUE;
}

static gboolean
set_pool_size(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	guint size;

	if (!parse_uint(value, AXIS_CONFIG_MIN_POOL_SIZE, AXIS_CONFIG_MAX_POOL_SIZE, &size)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid pool size '%s' (expected a number from %d to %d)",
			label, value != NULL ? value : "",
			AXIS_CONFIG_MIN_POOL_SIZE, AXIS_CONFIG_MAX_POOL_SIZE);
		return FALSE;
	}

	cfg->database_pool_size = size;
	cfg->database_pool_size_set = TRUE;
	return TRUE;
}

static gboolean
set_level(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	AxisLogLevel level;

	if (!axis_log_level_from_string(value, &level)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid log level '%s' (expected debug, info, warning or error)",
			label, value != NULL ? value : "");
		return FALSE;
	}

	cfg->log_level = level;
	return TRUE;
}

/* A queue name as the job queue spells them: lower-case ASCII letters,
 * digits, '.', '_' and '-', starting with a letter, at most 63. */
static gboolean
queue_name_is_valid(const gchar *name)
{
	const gchar *p;

	if (name[0] < 'a' || name[0] > 'z' || strlen(name) > 63)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      *p == '.' || *p == '_' || *p == '-'))
			return FALSE;
	}

	return TRUE;
}

/*
 * "queue:n, queue:n" -> "queue:n,queue:n", checked.  Returns NULL with
 * @error set when it is malformed, and "" for an empty list.
 */
static gchar *
normalise_concurrency(
	const gchar  *value,
	const gchar  *label,
	GError      **error
){
	g_auto(GStrv) entries = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	GString *out = g_string_new(NULL);
	guint i;

	entries = g_strsplit(value, ",", -1);
	for (i = 0; entries[i] != NULL; i++) {
		gchar *entry = g_strstrip(entries[i]);
		g_autofree gchar *original = g_strdup(entry);
		gchar *colon;
		guint count;

		if (*entry == '\0' && entries[1] == NULL)
			break;	/* the empty list */

		colon = strchr(entry, ':');
		if (colon != NULL)
			*colon = '\0';
		if (colon == NULL || !queue_name_is_valid(g_strstrip(entry)) ||
		    !parse_uint(g_strstrip(colon + 1), 0, AXIS_CONFIG_MAX_CONCURRENCY, &count)) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
				"%s: invalid entry '%s' (expected queue:count, such as scan:2, with a "
				"queue name of lower-case letters, digits, '.', '_' and '-' and a count from "
				"0 to %d, separated by commas)",
				label, original, AXIS_CONFIG_MAX_CONCURRENCY);
			g_string_free(out, TRUE);
			return NULL;
		}
		if (!g_hash_table_add(seen, entry)) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
				"%s: queue '%s' is listed more than once", label, entry);
			g_string_free(out, TRUE);
			return NULL;
		}
		g_string_append_printf(out, "%s%s:%u", out->len > 0 ? "," : "", entry, count);
	}

	return g_string_free(out, FALSE);
}

static gboolean
set_concurrency(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	gchar *normalised = normalise_concurrency(value != NULL ? value : "", label, error);

	if (normalised == NULL)
		return FALSE;

	g_free(cfg->jobs_concurrency);
	cfg->jobs_concurrency = normalised;
	return TRUE;
}

static gboolean
set_grace_period(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	guint seconds;

	if (!parse_uint(value, 0, AXIS_CONFIG_MAX_GRACE_PERIOD, &seconds)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid grace period '%s' (expected a number of seconds from 0 to %d)",
			label, value != NULL ? value : "", AXIS_CONFIG_MAX_GRACE_PERIOD);
		return FALSE;
	}

	cfg->jobs_grace_period = seconds;
	return TRUE;
}

static gboolean
set_worker_lease(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	guint seconds;

	if (!parse_uint(value, AXIS_CONFIG_MIN_WORKER_LEASE, AXIS_CONFIG_MAX_WORKER_LEASE, &seconds)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid worker lease '%s' (expected a number of seconds from %d to %d)",
			label, value != NULL ? value : "", AXIS_CONFIG_MIN_WORKER_LEASE, AXIS_CONFIG_MAX_WORKER_LEASE);
		return FALSE;
	}

	cfg->worker_lease = seconds;
	return TRUE;
}

/* A whole number from @min to @max into *@result, or an error naming
 * @label and what was expected (@what: "seconds", "connections"). */
static gboolean
set_bounded(
	guint       *result,
	const gchar *value,
	const gchar *label,
	guint        min,
	guint        max,
	const gchar *what,
	GError     **error
){
	guint number;

	if (!parse_uint(value, min, max, &number)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid value '%s' (expected a number of %s from %u to %u)",
			label, value != NULL ? value : "", what, min, max);
		return FALSE;
	}

	*result = number;
	return TRUE;
}

static gboolean
set_head_timeout(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	return set_bounded(&cfg->head_timeout, value, label, 1, AXIS_CONFIG_MAX_REQUEST_TIMEOUT, "seconds", error);
}

static gboolean
set_body_timeout(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	return set_bounded(&cfg->body_timeout, value, label, 1, AXIS_CONFIG_MAX_REQUEST_TIMEOUT, "seconds", error);
}

static gboolean
set_idle_timeout(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	return set_bounded(&cfg->idle_timeout, value, label, 1, AXIS_CONFIG_MAX_REQUEST_TIMEOUT, "seconds", error);
}

static gboolean
set_max_connections(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	return set_bounded(&cfg->max_connections, value, label, AXIS_CONFIG_MIN_MAX_CONNECTIONS,
		AXIS_CONFIG_MAX_MAX_CONNECTIONS, "connections", error);
}

static gboolean
set_max_per_address(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	return set_bounded(&cfg->max_per_address, value, label, 1, AXIS_CONFIG_MAX_MAX_CONNECTIONS, "connections",
		error);
}

/* An absolute path in the storage code's one form: no "." or ".." part,
 * no control character, not "/" itself.  The message never repeats it. */
static gboolean
set_data_dir(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	g_autofree gchar *normalised = NULL;
	const gchar *problem = NULL;

	if (value == NULL || !axis_storage_path_normalize(value, TRUE, &normalised, &problem)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: the data directory %s (expected an absolute path such as %s)",
			label, problem != NULL ? problem : "must not be empty", AXIS_CONFIG_DEFAULT_DATA_DIR);
		return FALSE;
	}

	g_free(cfg->data_dir);
	cfg->data_dir = g_steal_pointer(&normalised);
	return TRUE;
}

/* A number of seconds from 0 to AXIS_CONFIG_MAX_DATABASE_WAIT, or "forever". */
static gboolean
set_database_wait(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	guint seconds;

	if (g_strcmp0(value, "forever") == 0) {
		cfg->database_wait = AXIS_CONFIG_WAIT_FOREVER;
		return TRUE;
	}
	if (!parse_uint(value, 0, AXIS_CONFIG_MAX_DATABASE_WAIT, &seconds)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid wait '%s' (expected forever, or a number of seconds from 0 to %d; "
			"0 tries once)", label, value != NULL ? value : "", AXIS_CONFIG_MAX_DATABASE_WAIT);
		return FALSE;
	}

	cfg->database_wait = (gint)seconds;
	return TRUE;
}

static gboolean
set_log_format(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	if (g_strcmp0(value, "text") == 0) {
		cfg->log_format = AXIS_LOG_FORMAT_TEXT;
	} else if (g_strcmp0(value, "json") == 0) {
		cfg->log_format = AXIS_LOG_FORMAT_JSON;
	} else {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid log format '%s' (expected text or json)", label, value != NULL ? value : "");
		return FALSE;
	}

	return TRUE;
}

/* Only its presence and length are checked here; the web layer applies
 * the username rules when it creates the account. */
static gboolean
set_admin_username(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	g_free(cfg->admin_username);
	cfg->admin_username = (value != NULL && *value != '\0') ? g_strdup(value) : NULL;
	return TRUE;
}

/* A secret: the web layer checks it against the password rules, and no
 * message ever repeats it. */
static gboolean
set_admin_password(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	secret_free(cfg->admin_password);
	cfg->admin_password = (value != NULL && *value != '\0') ? g_strdup(value) : NULL;
	return TRUE;
}

/* A secret: at least AXIS_CONFIG_MIN_METRICS_TOKEN printable ASCII
 * characters with no space, so that it fits in an Authorization header and
 * is not guessable.  The message states the rule, never the value. */
static gboolean
set_metrics_token(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	const gchar *p;

	if (value == NULL || *value == '\0') {
		secret_free(cfg->metrics_token);
		cfg->metrics_token = NULL;
		return TRUE;
	}
	for (p = value; *p != '\0'; p++) {
		if (*p <= ' ' || *p > '~')
			break;
	}
	if (*p != '\0' || strlen(value) < AXIS_CONFIG_MIN_METRICS_TOKEN || strlen(value) > 256) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: the metrics token must be %d to 256 printable ASCII characters with no space "
			"(for instance from: openssl rand -hex 32)", label, AXIS_CONFIG_MIN_METRICS_TOKEN);
		return FALSE;
	}

	secret_free(cfg->metrics_token);
	cfg->metrics_token = g_strdup(value);
	return TRUE;
}

/* An interface name or a local address (core/axis-outbound-parse.h); empty
 * for none.  Only the form is checked here: the interface may well not
 * exist yet (a VPN that comes up later), which is no reason not to start. */
static gboolean
set_outbound_bind(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	AxisOutboundTarget target;
	g_autofree gchar *problem = NULL;

	if (!axis_outbound_target_parse(value, &target, &problem)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: %s (expected the VPN's interface, such as tun0 or wg0, or an address it holds, such as "
			"10.8.0.2; empty for none)", label, problem);
		return FALSE;
	}

	g_free(cfg->outbound_bind);
	cfg->outbound_bind = g_strdup(target.text);
	return TRUE;
}

static gboolean
set_outbound_scope(
	AxisConfig  *cfg,
	const gchar *value,
	const gchar *label,
	GError     **error
){
	if (!axis_outbound_scope_parse(value, &cfg->outbound_scope)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: invalid scope '%s' (expected internet, for the indexers and metadata providers, or all, for "
			"every connection but the database's)", label, value != NULL ? value : "");
		return FALSE;
	}

	return TRUE;
}

static gboolean
is_known_key(
	const gchar *group,
	const gchar *key
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		if (strcmp(settings[i].group, group) == 0 &&
		    strcmp(settings[i].key, key) == 0)
			return TRUE;
	}

	return FALSE;
}

static gboolean
is_known_group(const gchar *group)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		if (strcmp(settings[i].group, group) == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
axis_config_load_data(
	AxisConfig   *self,
	const gchar  *data,
	GError      **error
){
	g_autoptr(GKeyFile) keyfile = g_key_file_new();
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisConfig) staged = NULL;
	g_auto(GStrv) groups = NULL;
	gsize n_groups = 0;
	gsize g;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(data != NULL, FALSE);

	if (!g_key_file_load_from_data(keyfile, data, (gsize)-1, G_KEY_FILE_NONE, &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"invalid configuration syntax: %s", local->message);
		return FALSE;
	}

	/* Reject anything we do not know before looking at any value. */
	groups = g_key_file_get_groups(keyfile, &n_groups);
	for (g = 0; g < n_groups; g++) {
		g_auto(GStrv) keys = NULL;
		gsize n_keys = 0;
		gsize k;

		if (!is_known_group(groups[g])) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
				"unknown group [%s]", groups[g]);
			return FALSE;
		}

		keys = g_key_file_get_keys(keyfile, groups[g], &n_keys, NULL);
		for (k = 0; k < n_keys; k++) {
			if (!is_known_key(groups[g], keys[k])) {
				g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
					"unknown key '%s' in group [%s]", keys[k], groups[g]);
				return FALSE;
			}
		}
	}

	staged = config_copy(self);
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		g_autofree gchar *label = NULL;
		gchar *value;
		gboolean ok;

		if (!g_key_file_has_key(keyfile, settings[i].group, settings[i].key, NULL))
			continue;

		value = g_key_file_get_string(keyfile, settings[i].group, settings[i].key, &local);
		if (value == NULL) {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
				"%s.%s: %s", settings[i].group, settings[i].key, local->message);
			return FALSE;
		}

		label = g_strdup_printf("%s.%s", settings[i].group, settings[i].key);
		ok = settings[i].set(staged, value, label, error);
		secret_free(value);
		if (!ok)
			return FALSE;
	}

	config_commit(self, g_steal_pointer(&staged));
	return TRUE;
}

gboolean
axis_config_load_file(
	AxisConfig   *self,
	const gchar  *path,
	GError      **error
){
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_file_get_contents(path, &contents, NULL, &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"cannot read configuration file '%s': %s", path, local->message);
		return FALSE;
	}

	if (!axis_config_load_data(self, contents, &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s: %s", path, local->message);
		return FALSE;
	}

	return TRUE;
}

/*
 * The secret held in the file at @path, without surrounding white space
 * (editors and "echo" add a newline).  @variable is the _FILE variable that
 * named it.  Nothing here puts any of the content into a message: the errors
 * name the variable, the path and the reason only.
 */
static gchar *
read_secret_file(
	const gchar *variable,
	const gchar *path,
	GError     **error
){
	g_autoptr(GError) local = NULL;
	gchar *contents = NULL;
	gsize length = 0;

	if (!g_file_get_contents(path, &contents, &length, &local)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"cannot read %s '%s': %s", variable, path, local->message);
		return NULL;
	}

	if (length > AXIS_CONFIG_MAX_SECRET_FILE_SIZE) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"%s '%s' is larger than %d bytes: it should hold one value",
			variable, path, AXIS_CONFIG_MAX_SECRET_FILE_SIZE);
		memset(contents, 0, length);
		g_free(contents);
		return NULL;
	}

	g_strstrip(contents);
	if (*contents == '\0') {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG, "%s '%s' is empty", variable, path);
		g_free(contents);
		return NULL;
	}

	return contents;
}

gboolean
axis_config_apply_environment(
	AxisConfig   *self,
	gchar       **envp,
	GError      **error
){
	g_autoptr(AxisConfig) staged = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	staged = config_copy(self);
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		const gchar *value = g_environ_getenv(envp, settings[i].env);

		if (value == NULL || *value == '\0')
			continue;
		if (!settings[i].set(staged, value, settings[i].env, error))
			return FALSE;
	}

	/* A secret may instead come from a file, so that it can be a container
	 * secret (Docker, Podman, Kubernetes) rather than an environment
	 * variable that `inspect` shows. */
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		g_autofree gchar *file_variable = NULL;
		const gchar *path;
		const gchar *value;
		gchar *contents;
		gboolean ok;

		if (!settings[i].secret)
			continue;
		file_variable = g_strconcat(settings[i].env, "_FILE", NULL);
		path = g_environ_getenv(envp, file_variable);
		if (path == NULL || *path == '\0')
			continue;

		value = g_environ_getenv(envp, settings[i].env);
		if (value != NULL && *value != '\0') {
			g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
				"%s and %s are both set: set only one of them", settings[i].env, file_variable);
			return FALSE;
		}

		contents = read_secret_file(file_variable, path, error);
		if (contents == NULL)
			return FALSE;
		ok = settings[i].set(staged, contents, file_variable, error);
		secret_free(contents);
		if (!ok)
			return FALSE;
	}

	config_commit(self, g_steal_pointer(&staged));
	return TRUE;
}

gchar **
axis_config_environ_without_secrets(gchar **envp)
{
	gchar **out = g_strdupv(envp);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		g_autofree gchar *file_variable = NULL;

		if (!settings[i].secret)
			continue;
		file_variable = g_strconcat(settings[i].env, "_FILE", NULL);
		out = g_environ_unsetenv(out, settings[i].env);
		out = g_environ_unsetenv(out, file_variable);
	}

	return out;
}

const gchar *
axis_config_get_host(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->host;
}

guint16
axis_config_get_port(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->port;
}

const gchar *
axis_config_get_trusted_proxies(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->trusted_proxies;
}

AxisSecureCookies
axis_config_get_secure_cookies(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_SECURE_COOKIES_AUTO);
	return self->secure_cookies;
}

const gchar *
axis_config_get_database_url(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->database_url;
}

guint
axis_config_get_database_pool_size(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_POOL_SIZE);
	return self->database_pool_size;
}

void
axis_config_set_database_pool_size_used(
	AxisConfig *self,
	guint       size
){
	g_return_if_fail(self != NULL);
	self->database_pool_size_used = size;
}

gboolean
axis_config_has_database_pool_size(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, FALSE);
	return self->database_pool_size_set;
}

gboolean
axis_config_check_required(
	AxisConfig   *self,
	GError      **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	if (self->database_url == NULL) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"a database URL is required: set url in the [database] group of the "
			"configuration file, or AXIS_DATABASE_URL, or name a file holding it in "
			AXIS_CONFIG_ENV_DATABASE_URL_FILE);
		return FALSE;
	}
	if ((self->admin_username == NULL) != (self->admin_password == NULL)) {
		g_set_error(error, AXIS_ERROR, AXIS_ERROR_CONFIG,
			"the first administrator needs both a username and a password: %s is set and %s is not "
			"(AXIS_ADMIN_USERNAME, and AXIS_ADMIN_PASSWORD or AXIS_ADMIN_PASSWORD_FILE, or "
			"admin_username and admin_password in the [setup] group)",
			self->admin_username != NULL ? "the username" : "the password",
			self->admin_username != NULL ? "the password" : "the username");
		return FALSE;
	}

	return TRUE;
}

gint
axis_config_get_database_wait(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_WAIT_FOREVER);
	return self->database_wait;
}

AxisLogFormat
axis_config_get_log_format(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_LOG_FORMAT_TEXT);
	return self->log_format;
}

const gchar *
axis_config_get_admin_username(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->admin_username;
}

const gchar *
axis_config_get_admin_password(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->admin_password;
}

const gchar *
axis_config_get_metrics_token(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->metrics_token;
}

const gchar *
axis_config_get_outbound_bind(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->outbound_bind;
}

AxisOutboundScope
axis_config_get_outbound_scope(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	return self->outbound_scope;
}

void
axis_config_forget_admin_password(AxisConfig *self)
{
	g_return_if_fail(self != NULL);

	secret_free(self->admin_password);
	self->admin_password = NULL;
}

AxisLogLevel
axis_config_get_log_level(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_LOG_LEVEL);
	return self->log_level;
}

guint
axis_config_get_jobs_grace_period(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_GRACE_PERIOD);
	return self->jobs_grace_period;
}

guint
axis_config_get_head_timeout(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_HEAD_TIMEOUT);
	return self->head_timeout;
}

guint
axis_config_get_body_timeout(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_BODY_TIMEOUT);
	return self->body_timeout;
}

guint
axis_config_get_idle_timeout(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_IDLE_TIMEOUT);
	return self->idle_timeout;
}

guint
axis_config_get_max_connections(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_MAX_CONNECTIONS);
	return self->max_connections;
}

guint
axis_config_get_max_connections_per_address(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_MAX_PER_ADDRESS);
	return self->max_per_address;
}

guint
axis_config_get_worker_lease(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, AXIS_CONFIG_DEFAULT_WORKER_LEASE);
	return self->worker_lease;
}

const gchar *
axis_config_get_data_dir(AxisConfig *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->data_dir;
}

gchar **
axis_config_list_jobs_concurrency(AxisConfig *self)
{
	g_auto(GStrv) entries = NULL;
	GPtrArray *names;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	names = g_ptr_array_new();
	if (self->jobs_concurrency != NULL && *self->jobs_concurrency != '\0') {
		entries = g_strsplit(self->jobs_concurrency, ",", -1);
		for (i = 0; entries[i] != NULL; i++) {
			gchar *colon = strchr(entries[i], ':');

			g_ptr_array_add(names, g_strndup(entries[i], (gsize)(colon - entries[i])));
		}
	}
	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(names, FALSE);
}

gboolean
axis_config_get_jobs_concurrency(
	AxisConfig  *self,
	const gchar *queue,
	guint       *concurrency
){
	g_auto(GStrv) entries = NULL;
	gsize len;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(queue != NULL, FALSE);

	if (self->jobs_concurrency == NULL || *self->jobs_concurrency == '\0')
		return FALSE;

	len = strlen(queue);
	entries = g_strsplit(self->jobs_concurrency, ",", -1);
	for (i = 0; entries[i] != NULL; i++) {
		if (strncmp(entries[i], queue, len) == 0 && entries[i][len] == ':') {
			if (concurrency != NULL)
				*concurrency = (guint)g_ascii_strtoull(entries[i] + len + 1, NULL, 10);
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * Redaction
 *
 * Deliberately conservative: when in doubt, hide more.
 */

#define REDACTED_PASSWORD "****"
#define REDACTED_ALL      "<redacted>"

/* "password", "sslpassword", ... */
static gboolean
key_is_password(
	const gchar *key,
	gsize        len
){
	static const gchar suffix[] = "password";
	gsize n = sizeof(suffix) - 1;

	return len >= n && g_ascii_strncasecmp(key + len - n, suffix, n) == 0;
}

static gboolean
has_url_scheme(const gchar *s)
{
	const gchar *p = s;

	if (!g_ascii_isalpha(*p))
		return FALSE;
	for (p = s + 1; g_ascii_isalnum(*p) || *p == '+' || *p == '-' || *p == '.'; p++)
		;

	return p[0] == ':' && p[1] == '/' && p[2] == '/';
}

/* Replace the value of every "...password=value" parameter in @query
 * (the text after '?', up to but excluding any '#'). */
static void
redact_query(
	GString     *out,
	const gchar *query,
	gsize        len
){
	gsize pos = 0;

	while (pos <= len) {
		const gchar *param = query + pos;
		gsize plen = 0;
		const gchar *eq;

		while (pos + plen < len && param[plen] != '&')
			plen++;

		eq = memchr(param, '=', plen);
		if (eq != NULL && key_is_password(param, (gsize)(eq - param))) {
			g_string_append_len(out, param, (gssize)(eq - param) + 1);
			g_string_append(out, REDACTED_PASSWORD);
		} else {
			g_string_append_len(out, param, (gssize)plen);
		}

		pos += plen;
		if (pos < len)
			g_string_append_c(out, '&');
		pos++;
	}
}

static gchar *
redact_uri(const gchar *url)
{
	GString *out = g_string_new(NULL);
	const gchar *authority = strstr(url, "://") + 3;
	const gchar *tail;
	const gchar *query_start;
	const gchar *fragment;
	const gchar *at = NULL;
	const gchar *p;

	/* Everything before the first '?' or '#' is scheme, userinfo, host and
	 * path.  The userinfo ends at the last '@' in that part, which also
	 * covers a password containing an unescaped '@' or '/'. */
	tail = authority + strcspn(authority, "?#");
	for (p = authority; p < tail; p++) {
		if (*p == '@')
			at = p;
	}

	if (at != NULL) {
		const gchar *colon = memchr(authority, ':', (gsize)(at - authority));

		if (colon != NULL) {
			g_string_append_len(out, url, (gssize)(colon - url) + 1);
			g_string_append(out, REDACTED_PASSWORD);
			g_string_append_len(out, at, (gssize)(tail - at));
		} else {
			g_string_append_len(out, url, (gssize)(tail - url));
		}
	} else {
		g_string_append_len(out, url, (gssize)(tail - url));
	}

	if (*tail == '?') {
		query_start = tail + 1;
		fragment = strchr(query_start, '#');
		if (fragment == NULL)
			fragment = query_start + strlen(query_start);

		g_string_append_c(out, '?');
		redact_query(out, query_start, (gsize)(fragment - query_start));
		g_string_append(out, fragment);
	} else {
		g_string_append(out, tail);
	}

	return g_string_free(out, FALSE);
}

/* libpq "key=value key2='quoted value'" form.  Returns NULL if the text
 * is not in that form. */
static gchar *
redact_keyword_string(const gchar *s)
{
	GString *out = g_string_new(NULL);
	const gchar *p = s;
	gboolean any = FALSE;

	for (;;) {
		const gchar *key;
		gsize key_len;
		const gchar *value;
		gboolean is_secret;

		while (g_ascii_isspace(*p)) {
			g_string_append_c(out, *p);
			p++;
		}
		if (*p == '\0')
			break;

		key = p;
		while (g_ascii_isalnum(*p) || *p == '_')
			p++;
		key_len = (gsize)(p - key);
		if (key_len == 0)
			goto fail;
		g_string_append_len(out, key, (gssize)key_len);

		while (*p == ' ' || *p == '\t') {
			g_string_append_c(out, *p);
			p++;
		}
		if (*p != '=')
			goto fail;
		g_string_append_c(out, '=');
		p++;
		while (*p == ' ' || *p == '\t')
			p++;

		is_secret = key_is_password(key, key_len);
		value = p;
		if (*p == '\'') {
			p++;
			while (*p != '\'') {
				if (*p == '\0')
					goto fail;	/* unterminated quote */
				if (*p == '\\' && p[1] != '\0')
					p++;
				p++;
			}
			p++;
		} else {
			while (*p != '\0' && !g_ascii_isspace(*p))
				p++;
		}

		if (is_secret)
			g_string_append(out, REDACTED_PASSWORD);
		else
			g_string_append_len(out, value, (gssize)(p - value));
		any = TRUE;
	}

	if (!any)
		goto fail;

	return g_string_free(out, FALSE);

fail:
	g_string_free(out, TRUE);
	return NULL;
}

gchar *
axis_config_redact_url(const gchar *url)
{
	gchar *result;
	const gchar *p;

	if (url == NULL)
		return NULL;
	if (*url == '\0')
		return g_strdup("");

	if (has_url_scheme(url)) {
		for (p = url; *p != '\0'; p++) {
			if (g_ascii_isspace(*p) || g_ascii_iscntrl(*p))
				return g_strdup(REDACTED_ALL);
		}
		return redact_uri(url);
	}

	result = redact_keyword_string(url);
	if (result == NULL)
		return g_strdup(REDACTED_ALL);

	return result;
}

gchar *
axis_config_describe(AxisConfig *self)
{
	g_autofree gchar *url = NULL;
	g_autofree gchar *wait = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	url = axis_config_redact_url(self->database_url);
	if (self->database_wait == AXIS_CONFIG_WAIT_FOREVER)
		wait = g_strdup("forever");
	else
		wait = g_strdup_printf("%d", self->database_wait);

	/* Neither the administrator's password nor the metrics token: only
	 * whether they are set. */
	return g_strdup_printf(
		"host=%s port=%u trusted_proxies=%s secure_cookies=%s log_level=%s log_format=%s database_url=%s "
		"database_pool_size=%u database_wait=%s jobs_concurrency=%s jobs_grace_period=%u worker_lease=%u "
		"data_dir=%s first_admin=%s metrics=%s outbound_bind=%s outbound_bind_scope=%s head_timeout=%u "
		"body_timeout=%u idle_timeout=%u max_connections=%u max_connections_per_address=%u",
		self->host, (guint)self->port,
		self->trusted_proxies[0] != '\0' ? self->trusted_proxies : "(none)",
		self->secure_cookies == AXIS_SECURE_COOKIES_ALWAYS ? "always" : "auto",
		axis_log_level_to_string(self->log_level),
		self->log_format == AXIS_LOG_FORMAT_JSON ? "json" : "text",
		url != NULL ? url : "(none)",
			self->database_pool_size_used != 0 ? self->database_pool_size_used : self->database_pool_size, wait,
		self->jobs_concurrency != NULL && *self->jobs_concurrency != '\0' ?
			self->jobs_concurrency : "(default)",
		self->jobs_grace_period, self->worker_lease, self->data_dir,
		self->admin_username != NULL ? "(given)" : "(none)",
		self->metrics_token != NULL ? "on" : "off",
		self->outbound_bind[0] != '\0' ? self->outbound_bind : "(none)",
		axis_outbound_scope_to_string(self->outbound_scope), self->head_timeout, self->body_timeout,
		self->idle_timeout, self->max_connections, self->max_per_address);
}
