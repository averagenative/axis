/*
 * axis-portal-config.c - axis-portal's configuration
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-config.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/axis-proxy.h"

#define TOKEN_LEN       43
#define TOKEN_FILE_MAX  4096
#define GROUP           "portal"
#define PORTAL_ERROR    (axis_portal_config_error_quark())

static G_DEFINE_QUARK(axis-portal-config-error-quark, axis_portal_config_error)

/* Every key the file may have; the environment's names follow from them.
 * This list is the whole of what the portal can be told. */
static const struct
{
	const gchar *key;
	const gchar *env;
} keys[] = {
	{ "url", "AXIS_PORTAL_URL" },
	{ "token", "AXIS_PORTAL_TOKEN" },
	{ "token_file", "AXIS_PORTAL_TOKEN_FILE" },
	{ "ca_file", "AXIS_PORTAL_CA_FILE" },
	{ "allow_insecure_http", "AXIS_PORTAL_ALLOW_INSECURE_HTTP" },
	{ "request_timeout", "AXIS_PORTAL_REQUEST_TIMEOUT" },
	{ "web_listen_host", "AXIS_PORTAL_WEB_LISTEN_HOST" },
	{ "web_listen_port", "AXIS_PORTAL_WEB_LISTEN_PORT" },
	{ "trusted_proxies", "AXIS_PORTAL_TRUSTED_PROXIES" },
	{ "insecure_browser_http", "AXIS_PORTAL_INSECURE_BROWSER_HTTP" },
	{ "max_connections", "AXIS_PORTAL_MAX_CONNECTIONS" },
	{ "max_connections_per_address", "AXIS_PORTAL_MAX_CONNECTIONS_PER_ADDRESS" },
	{ "log_level", "AXIS_PORTAL_LOG_LEVEL" },
	{ "log_format", "AXIS_PORTAL_LOG_FORMAT" },
	{ NULL, NULL }
};

static void
wipe(gchar *secret)
{
	if (secret != NULL) {
		volatile gchar *p = secret;

		while (*p != '\0')
			*p++ = '\0';
		g_free(secret);
	}
}

void
axis_portal_config_free(AxisPortalConfig *self)
{
	if (self == NULL)
		return;
	g_free(self->url);
	g_free(self->host);
	wipe(self->token);
	g_free(self->ca_file);
	g_free(self->listen_host);
	g_free(self->trusted_proxies);
	g_free(self->service_links);
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
	g_set_error_literal(error, PORTAL_ERROR, 0, text);
	g_free(text);
	return FALSE;
}

/* ── Raw values: the file, then the environment ─────────────────── */

typedef struct
{
	GHashTable *values;	/* key -> value (copies) */
	GHashTable *from;	/* key -> where it was read, for messages */
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
	if (st.st_size > AXIS_PORTAL_CONFIG_MAX_FILE_SIZE)
		return fail(error, "the configuration file %s is larger than %d bytes", path, AXIS_PORTAL_CONFIG_MAX_FILE_SIZE);
	if (!g_file_get_contents(path, &text, &length, NULL))
		return fail(error, "the configuration file %s cannot be read", path);
	/* The reader's own messages quote the line they could not read, which
	 * could be the token: none of them is passed on. */
	if (!g_key_file_load_from_data(file, text, length, G_KEY_FILE_NONE, NULL)) {
		wipe(text);
		return fail(error, "the configuration file %s is not a key file (\"key = value\" lines under [" GROUP "])",
			path);
	}
	wipe(text);

	groups = g_key_file_get_groups(file, NULL);
	for (i = 0; groups[i] != NULL; i++) {
		if (strcmp(groups[i], GROUP) != 0)
			return fail(error, "the configuration file %s has a group other than [" GROUP "]", path);
	}
	if (!g_key_file_has_group(file, GROUP))
		return TRUE;
	names = g_key_file_get_keys(file, GROUP, NULL, NULL);
	for (i = 0; names != NULL && names[i] != NULL; i++) {
		const gchar *key = names[i];
		gchar *value;

		if (!key_known(key)) {
			if (printable_key(key))
				return fail(error, "the configuration file %s has the key '%s', which axis-portal does not know",
					path, key);
			return fail(error, "the configuration file %s has a key axis-portal does not know", path);
		}
		value = g_key_file_get_string(file, GROUP, key, NULL);
		if (value == NULL)
			return fail(error, GROUP ".%s in %s cannot be read", key, path);
		g_hash_table_replace(raw->values, g_strdup(key), value);
		g_hash_table_replace(raw->from, g_strdup(key), g_strdup_printf(GROUP ".%s in %s", key, path));
	}
	return TRUE;
}

/* ── Kubernetes service links ───────────────────────────────────── */

/* A port as Kubernetes writes one: 1 to 65535, plain digits. */
static gboolean
link_port(const gchar *text)
{
	guint port = 0;
	const gchar *p;

	if (text == NULL || text[0] == '\0' || text[0] == '0' || strlen(text) > 5)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
		port = port * 10 + (guint)(*p - '0');
	}
	return port <= 65535;
}

static gboolean
link_protocol(
	const gchar *text,
	gsize        length,
	gboolean     upper
){
	static const gchar *const lower_words[] = { "tcp", "udp", "sctp", NULL };
	static const gchar *const upper_words[] = { "TCP", "UDP", "SCTP", NULL };
	const gchar *const *words = upper ? upper_words : lower_words;
	guint i;

	for (i = 0; words[i] != NULL; i++) {
		if (strlen(words[i]) == length && strncmp(text, words[i], length) == 0)
			return TRUE;
	}
	return FALSE;
}

/* "tcp://10.0.0.7:7802", "udp://[fd00::7]:53": a service link's address. */
static gboolean
link_address(const gchar *text)
{
	const gchar *rest = text != NULL ? strstr(text, "://") : NULL;
	const gchar *colon;
	g_autofree gchar *host = NULL;

	if (rest == NULL || !link_protocol(text, (gsize)(rest - text), FALSE))
		return FALSE;
	rest += 3;
	colon = strrchr(rest, ':');
	if (colon == NULL || !link_port(colon + 1))
		return FALSE;
	if (rest[0] == '[') {
		if (colon - rest < 3 || colon[-1] != ']')
			return FALSE;
		host = g_strndup(rest + 1, (gsize)(colon - rest - 2));
	} else {
		host = g_strndup(rest, (gsize)(colon - rest));
	}
	return axis_ip_address_parse(host, NULL);
}

/* Whether @rest is "<digits>_<PROTOCOL>", and how long that is. */
static gsize
link_port_protocol(const gchar *rest)
{
	gsize digits = 0;
	gsize length;

	while (g_ascii_isdigit(rest[digits]))
		digits++;
	if (digits == 0 || rest[digits] != '_')
		return 0;
	{
		g_autofree gchar *port = g_strndup(rest, digits);

		if (!link_port(port))
			return 0;
	}
	for (length = digits + 1; g_ascii_isupper(rest[length]); length++)
		;
	if (!link_protocol(rest + digits + 1, length - digits - 1, TRUE))
		return 0;
	return length;
}

/*
 * Whether @name and @value are one of the variables Kubernetes puts into
 * every container of a namespace for a Service called axis-portal (unless
 * the pod says enableServiceLinks: false):
 *
 *   AXIS_PORTAL_SERVICE_HOST=10.0.0.7
 *   AXIS_PORTAL_SERVICE_PORT=7802, AXIS_PORTAL_SERVICE_PORT_<NAME>=7802
 *   AXIS_PORTAL_PORT=tcp://10.0.0.7:7802
 *   AXIS_PORTAL_PORT_7802_TCP=tcp://10.0.0.7:7802
 *   AXIS_PORTAL_PORT_7802_TCP_PROTO=tcp, _PORT=7802, _ADDR=10.0.0.7
 *
 * The value must have its shape too, so that a setting misspelled into one
 * of these names is still refused.
 */
static gboolean
is_service_link(
	const gchar *name,
	const gchar *value
){
	const gchar *rest = name + strlen("AXIS_PORTAL_");
	const gchar *p;
	gsize length;

	if (strcmp(rest, "SERVICE_HOST") == 0)
		return axis_ip_address_parse(value, NULL);
	if (strcmp(rest, "SERVICE_PORT") == 0)
		return link_port(value);
	if (g_str_has_prefix(rest, "SERVICE_PORT_") && rest[strlen("SERVICE_PORT_")] != '\0') {
		for (p = rest + strlen("SERVICE_PORT_"); *p != '\0'; p++) {
			if (!g_ascii_isupper(*p) && !g_ascii_isdigit(*p) && *p != '_')
				return FALSE;
		}
		return link_port(value);
	}
	if (strcmp(rest, "PORT") == 0)
		return link_address(value);
	if (!g_str_has_prefix(rest, "PORT_"))
		return FALSE;
	rest += strlen("PORT_");
	length = link_port_protocol(rest);
	if (length == 0)
		return FALSE;
	rest += length;
	if (rest[0] == '\0')
		return link_address(value);
	if (strcmp(rest, "_PROTO") == 0)
		return value != NULL && link_protocol(value, strlen(value), FALSE);
	if (strcmp(rest, "_PORT") == 0)
		return link_port(value);
	if (strcmp(rest, "_ADDR") == 0)
		return axis_ip_address_parse(value, NULL);
	return FALSE;
}

/* A variable's name may be printed when it is of the characters a shell
 * writes names with: a token (which has lower-case letters and '-' almost
 * always) pasted into a name would not be. */
static gboolean
printable_name(const gchar *name)
{
	const gchar *p;

	if (strlen(name) > 128)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if (!g_ascii_isupper(*p) && !g_ascii_isdigit(*p) && *p != '_')
			return FALSE;
	}
	return TRUE;
}

/* The environment replaces the file's value.  A variable that begins with
 * AXIS_PORTAL_ and is not one of the keys is refused like a key of the file:
 * a setting that is believed to be in force and is not is worse than none.
 * Kubernetes' service links for a Service called axis-portal are not
 * settings: they are left aside, and their names kept for one line of the
 * log. */
static gboolean
read_environment(
	Raw               *raw,
	gchar            **envp,
	AxisPortalConfig  *config,
	GError           **error
){
	GString *links = NULL;
	guint i;
	guint j;

	for (i = 0; envp != NULL && envp[i] != NULL; i++) {
		const gchar *equals = strchr(envp[i], '=');
		g_autofree gchar *name = NULL;
		gboolean known = FALSE;

		if (equals == NULL || !g_str_has_prefix(envp[i], "AXIS_PORTAL_"))
			continue;
		name = g_strndup(envp[i], (gsize)(equals - envp[i]));
		for (j = 0; keys[j].key != NULL; j++)
			known = known || strcmp(keys[j].env, name) == 0;
		if (known)
			continue;
		/* The daemon's own settings for its portal listener have names that
		 * look like the portal's. */
		if (strcmp(name, "AXIS_PORTAL_LISTEN_PORT") == 0 || strcmp(name, "AXIS_PORTAL_LISTEN_HOST") == 0) {
			if (links != NULL)
				g_string_free(links, TRUE);
			return fail(error, "the environment has %s, which is a setting of the daemon (axisd), not of "
				"axis-portal: the portal's own address is AXIS_PORTAL_WEB_LISTEN_HOST and "
				"AXIS_PORTAL_WEB_LISTEN_PORT, and the daemon's is given in AXIS_PORTAL_URL", name);
		}
		if (is_service_link(name, equals + 1)) {
			if (links == NULL)
				links = g_string_new(name);
			else
				g_string_append_printf(links, ", %s", name);
			continue;
		}
		if (links != NULL)
			g_string_free(links, TRUE);
		if (!printable_name(name))
			return fail(error, "the environment has a variable whose name begins with AXIS_PORTAL_, which "
				"axis-portal does not know; its name has characters other than A-Z, 0-9 and _ or is longer than "
				"128, and is not printed in case it holds a secret");
		if (strstr(name, "_SERVICE_HOST") != NULL || strstr(name, "_SERVICE_PORT") != NULL ||
		    strstr(name, "_PORT_") != NULL || g_str_has_suffix(name, "_PORT"))
			return fail(error, "the environment has %s, which axis-portal does not know (if it is a Kubernetes "
				"service link of another Service whose name begins with axis-portal, give that Service another "
				"name or set enableServiceLinks: false in the pod's spec)", name);
		return fail(error, "the environment has %s, which axis-portal does not know", name);
	}
	if (links != NULL)
		config->service_links = g_string_free(links, FALSE);
	for (i = 0; keys[i].key != NULL; i++) {
		const gchar *value = g_environ_getenv(envp, keys[i].env);

		if (value == NULL || value[0] == '\0')
			continue;
		g_hash_table_replace(raw->values, g_strdup(keys[i].key), g_strdup(value));
		g_hash_table_replace(raw->from, g_strdup(keys[i].key), g_strdup(keys[i].env));
	}
	return TRUE;
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

	if (text == NULL || !g_ascii_isdigit(text[0]) || strlen(text) > 9)
		return FALSE;
	v = g_ascii_strtoull(text, &end, 10);
	if (end == NULL || *end != '\0' || v < minimum || v > maximum)
		return FALSE;
	*value = (guint)v;
	return TRUE;
}

static gboolean
host_is_loopback(const gchar *host)
{
	g_autoptr(GInetAddress) address = NULL;

	if (host == NULL || host[0] == '\0')
		return FALSE;
	if (g_ascii_strcasecmp(host, "localhost") == 0)
		return TRUE;
	address = g_inet_address_new_from_string(host);
	return address != NULL && g_inet_address_get_is_loopback(address);
}

/* The daemon's address: http or https, a host, maybe a port and a path
 * prefix of plain characters; nothing else. */
static gboolean
parse_url(
	AxisPortalConfig  *config,
	const gchar       *text,
	const gchar       *from,
	GError           **error
){
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *host = NULL;
	const gchar *scheme;
	const gchar *p;
	gint port;

	if (text == NULL)
		return fail(error, "the daemon's address is required: set url in the configuration file or AXIS_PORTAL_URL "
			"(the daemon's portal listener, such as http://axis:7801)");
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
	config->loopback = host_is_loopback(config->host);
	port = g_uri_get_port(uri);
	host = strchr(config->host, ':') != NULL ? g_strdup_printf("[%s]", config->host) : g_strdup(config->host);
	config->url = port > 0 ? g_strdup_printf("%s://%s:%d%s", config->https ? "https" : "http", host, port, path) :
		g_strdup_printf("%s://%s%s", config->https ? "https" : "http", host, path);
	return TRUE;
}

/* The token: from exactly one place, in the one form tokens have. */
static gboolean
load_token(
	AxisPortalConfig  *config,
	Raw               *raw,
	GError           **error
){
	const gchar *inline_token = raw_get(raw, "token");
	const gchar *file = raw_get(raw, "token_file");
	const gchar *from = raw_from(raw, inline_token != NULL ? "token" : "token_file");
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
		return fail(error, "the portal's token is required: set token_file in the configuration file or "
			"AXIS_PORTAL_TOKEN_FILE (a file holding the token the page \"Request portal\" showed)");
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
		/* A mounted secret is often readable by all: said at start, not
		 * refused. */
		config->token_file_readable_by_all = (st.st_mode & S_IROTH) != 0;
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
		return fail(error, "the token (%s) is not in the form the daemon gives", from);
	for (i = 0; i < TOKEN_LEN; i++) {
		gchar c = config->token[i];

		if (!(g_ascii_isalnum(c) || c == '-' || c == '_'))
			return fail(error, "the token (%s) is not in the form the daemon gives", from);
	}
	return TRUE;
}

AxisPortalConfig *
axis_portal_config_load(
	const gchar  *path,
	gchar       **envp,
	gboolean      need_token,
	GError      **error
){
	g_autoptr(AxisPortalConfig) config = g_new0(AxisPortalConfig, 1);
	const gchar *text;
	guint number = 0;
	Raw raw;
	gboolean ok = FALSE;

	memset(&raw, 0, sizeof(raw));
	raw.values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	raw.from = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	config->request_timeout_ms = 20000;
	config->listen_port = AXIS_PORTAL_DEFAULT_PORT;
	config->max_connections = 256;
	config->max_per_address = 32;
	config->log_level = AXIS_LOG_LEVEL_INFO;
	config->log_format = AXIS_LOG_FORMAT_TEXT;

	if (path != NULL && !read_file(&raw, path, error))
		goto out;
	if (!read_environment(&raw, envp, config, error))
		goto out;

	if ((text = raw_get(&raw, "log_level")) != NULL && !axis_log_level_from_string(text, &config->log_level)) {
		fail(error, "%s is not one of debug, info, warning, error", raw_from(&raw, "log_level"));
		goto out;
	}
	if ((text = raw_get(&raw, "log_format")) != NULL) {
		if (strcmp(text, "json") == 0) {
			config->log_format = AXIS_LOG_FORMAT_JSON;
		} else if (strcmp(text, "text") != 0) {
			fail(error, "%s is not text or json", raw_from(&raw, "log_format"));
			goto out;
		}
	}
	if ((text = raw_get(&raw, "allow_insecure_http")) != NULL && !parse_bool(text, &config->allow_insecure_http)) {
		fail(error, "%s is not true or false", raw_from(&raw, "allow_insecure_http"));
		goto out;
	}
	if ((text = raw_get(&raw, "insecure_browser_http")) != NULL &&
	    !parse_bool(text, &config->insecure_browser_http)) {
		fail(error, "%s is not true or false", raw_from(&raw, "insecure_browser_http"));
		goto out;
	}
	if ((text = raw_get(&raw, "request_timeout")) != NULL) {
		if (!parse_uint(text, 1, 120, &number)) {
			fail(error, "%s is not a number of seconds from 1 to 120", raw_from(&raw, "request_timeout"));
			goto out;
		}
		config->request_timeout_ms = number * 1000;
	}
	if ((text = raw_get(&raw, "web_listen_port")) != NULL) {
		if (!parse_uint(text, 1, 65535, &number)) {
			fail(error, "%s is not a port from 1 to 65535", raw_from(&raw, "web_listen_port"));
			goto out;
		}
		config->listen_port = (guint16)number;
	}
	if ((text = raw_get(&raw, "max_connections")) != NULL && !parse_uint(text, 1, 65535, &config->max_connections)) {
		fail(error, "%s is not a number from 1 to 65535", raw_from(&raw, "max_connections"));
		goto out;
	}
	if ((text = raw_get(&raw, "max_connections_per_address")) != NULL &&
	    !parse_uint(text, 1, 65535, &config->max_per_address)) {
		fail(error, "%s is not a number from 1 to 65535", raw_from(&raw, "max_connections_per_address"));
		goto out;
	}
	text = raw_get(&raw, "web_listen_host");
	if (text != NULL && !axis_ip_address_parse(text, NULL)) {
		fail(error, "%s is not an IPv4 or IPv6 address", raw_from(&raw, "web_listen_host"));
		goto out;
	}
	config->listen_host = g_strdup(text != NULL ? text : "127.0.0.1");
	/* The test-only override turns the cookies' Secure and __Host- off: not
	 * on a listener that anything but this machine can reach. */
	if (config->insecure_browser_http && !host_is_loopback(config->listen_host)) {
		fail(error, "%s is for tests only and is refused unless web_listen_host is a loopback address (it is %s)",
			raw_from(&raw, "insecure_browser_http"), config->listen_host);
		goto out;
	}
	text = raw_get(&raw, "trusted_proxies");
	if (text != NULL) {
		g_autoptr(GError) local = NULL;
		g_autoptr(AxisProxyList) list = axis_proxy_list_parse(text, &local);

		if (list == NULL) {
			fail(error, "%s: %s", raw_from(&raw, "trusted_proxies"), local->message);
			goto out;
		}
	}
	config->trusted_proxies = g_strdup(text != NULL ? text : "");

	/* --healthcheck asks this machine's own port, and nothing of the daemon. */
	if (!need_token) {
		ok = TRUE;
		goto out;
	}
	if (!parse_url(config, raw_get(&raw, "url"), raw_from(&raw, "url"), error))
		goto out;
	/* The token, and every password and session proof that passes through,
	 * go only where they are protected, unless told otherwise. */
	if (!config->https && !config->loopback && !config->allow_insecure_http) {
		fail(error, "refusing to send the portal's token over plain http to %s, which is not this machine: use https, "
			"or set allow_insecure_http = true if the network between the portal and the daemon holds those two "
			"only", config->host);
		goto out;
	}
	if ((text = raw_get(&raw, "ca_file")) != NULL) {
		if (!g_path_is_absolute(text) || !g_file_test(text, G_FILE_TEST_IS_REGULAR)) {
			fail(error, "%s is not a readable file", raw_from(&raw, "ca_file"));
			goto out;
		}
		config->ca_file = g_strdup(text);
	}
	if (!load_token(config, &raw, error))
		goto out;
	ok = TRUE;

out:
	raw_clear(&raw);
	return ok ? g_steal_pointer(&config) : NULL;
}

gchar *
axis_portal_config_describe(const AxisPortalConfig *self)
{
	return g_strdup_printf("url=%s ca_file=%s allow_insecure_http=%s request_timeout=%us listen=%s:%u "
		"trusted_proxies=%s insecure_browser_http=%s max_connections=%u max_connections_per_address=%u log_level=%s "
		"log_format=%s", self->url != NULL ? self->url : "(none)", self->ca_file != NULL ? self->ca_file : "(system)",
		self->allow_insecure_http ? "true" : "false", self->request_timeout_ms / 1000, self->listen_host,
		(guint)self->listen_port, self->trusted_proxies[0] != '\0' ? self->trusted_proxies : "(none)",
		self->insecure_browser_http ? "true" : "false", self->max_connections, self->max_per_address,
		axis_log_level_to_string(self->log_level), self->log_format == AXIS_LOG_FORMAT_JSON ? "json" : "text");
}
