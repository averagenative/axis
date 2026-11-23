/*
 * axis-config.h - bootstrap configuration (file + environment)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Holds only what the daemon needs before it can reach its database.
 *
 *   [server]    host (string), port (1..65535),
 *               trusted_proxies (list of addresses, CIDR ranges and host names),
 *               secure_cookies (auto|always),
 *               head_timeout, body_timeout, idle_timeout (seconds, 1..3600;
 *               default 30, 60, 60), max_connections (16..65536, default
 *               512), max_connections_per_address (1..65536, default 64):
 *               what a request and a client may cost (web/axis-web-limits.h)
 *   [database]  url (string, secret), pool_size (1..64),
 *               wait (seconds 0..86400, or forever; default forever)
 *   [log]       level (debug|info|warning|error), format (text|json)
 *   [jobs]      concurrency (list of queue:count), grace_period (seconds)
 *   [workers]   lease (seconds, 2..3600, default 60)
 *   [data]      dir (an absolute path, default /var/lib/axis)
 *   [setup]     admin_username, admin_password (secret): the first
 *               administrator, created at start while no account exists
 *   [metrics]   token (secret): turns on /metrics, answered only to a
 *               request that carries it as a bearer token
 *   [outbound]  bind (an interface name or a local IP address, empty for
 *               none), bind_scope (internet|all, default internet): the
 *               interface outbound connections are tied to
 *               (core/axis-outbound.h)
 *
 * outbound.bind is checked for its form only (core/axis-outbound-parse.h):
 * the interface need not exist when the daemon starts.
 * database.wait is how long start-up keeps trying to reach the database
 * (with back-off) before it gives up and exits; 0 tries once.  While it
 * waits the daemon answers /livez (alive) and /readyz (not ready) and
 * nothing else.
 *
 * Every secret (database.url, setup.admin_password, metrics.token) can also
 * be read from a file named by its variable with _FILE added
 * (AXIS_DATABASE_URL_FILE, AXIS_ADMIN_PASSWORD_FILE,
 * AXIS_METRICS_TOKEN_FILE): a Docker or Kubernetes secret, so that the value
 * is not in the environment.  The file holds the value, with or without a
 * trailing newline (surrounding white space is removed); setting both the
 * variable and its _FILE form is an error, and no message ever contains a
 * secret or what a file holds.
 *
 * data.dir is the one directory the daemon keeps files of its own in
 * (core/axis-data-dir.h): it is created at start if absent, and the daemon
 * refuses to start when it cannot write there.  Only the syntax is checked
 * here (absolute, no "." or ".." part, no control character, not "/").
 *
 * server.trusted_proxies is a comma-separated list of IP addresses, CIDR
 * ranges and host names, empty by default, whose X-Forwarded-For and
 * X-Forwarded-Proto headers are believed (see core/axis-proxy.h for the
 * accepted forms).  server.secure_cookies is "auto" (the default: cookies
 * are marked Secure when the request arrived over HTTPS, directly or as a
 * trusted proxy reports it) or "always".
 *
 * jobs.concurrency sets how many jobs of each named queue run in the
 * daemon at once, as comma-separated queue:count pairs such as
 * "scan:2, probe:4, transcode:0"; a count is 0 (leave the queue to
 * external workers) to 64, and a queue not listed gets 1.  Only the
 * syntax is checked here: the job queue refuses to start when a name is
 * not a queue any job kind uses.  jobs.grace_period (0..3600, default 30)
 * is how long running jobs get to finish at shutdown.
 *
 * Environment overrides, applied after the file and validated the same
 * way: AXIS_LISTEN_HOST, AXIS_LISTEN_PORT, AXIS_TRUSTED_PROXIES,
 * AXIS_SECURE_COOKIES, AXIS_DATABASE_URL, AXIS_DATABASE_POOL_SIZE,
 * AXIS_DATABASE_WAIT, AXIS_LOG_LEVEL, AXIS_LOG_FORMAT, AXIS_JOBS_CONCURRENCY,
 * AXIS_JOBS_GRACE_PERIOD, AXIS_WORKER_LEASE, AXIS_DATA_DIR,
 * AXIS_ADMIN_USERNAME, AXIS_ADMIN_PASSWORD, AXIS_METRICS_TOKEN,
 * AXIS_OUTBOUND_BIND, AXIS_OUTBOUND_BIND_SCOPE, AXIS_HEAD_TIMEOUT,
 * AXIS_BODY_TIMEOUT, AXIS_IDLE_TIMEOUT, AXIS_MAX_CONNECTIONS,
 * AXIS_MAX_CONNECTIONS_PER_ADDRESS, and the _FILE forms of the three
 * secrets.
 *
 * The database URL has no default; axis_config_check_required() reports
 * it missing once the file and the environment have both been applied.
 */

#ifndef AXIS_CONFIG_H
#define AXIS_CONFIG_H

#include <glib.h>
#include "core/axis-log.h"
#include "core/axis-outbound-parse.h"

G_BEGIN_DECLS

#define AXIS_CONFIG_DEFAULT_HOST      "127.0.0.1"
#define AXIS_CONFIG_DEFAULT_PORT      7800
#define AXIS_CONFIG_DEFAULT_LOG_LEVEL AXIS_LOG_LEVEL_INFO
/* The pool when none is set, at the least: axisd takes what its job queue
 * needs (a connection for each worker thread, at the configured
 * concurrency, and the queue's own two) and AXIS_CONFIG_POOL_HEADROOM more
 * for the pages, the health checks and the worker API, when that is more
 * (15 with every queue at its default concurrency of 1). */
#define AXIS_CONFIG_DEFAULT_POOL_SIZE 10
#define AXIS_CONFIG_POOL_HEADROOM     4
#define AXIS_CONFIG_MIN_POOL_SIZE     1
#define AXIS_CONFIG_MAX_POOL_SIZE     64
#define AXIS_CONFIG_DEFAULT_GRACE_PERIOD 30
#define AXIS_CONFIG_MAX_GRACE_PERIOD  3600
#define AXIS_CONFIG_MAX_CONCURRENCY   64
#define AXIS_CONFIG_ENV_DATABASE_URL_FILE "AXIS_DATABASE_URL_FILE"
#define AXIS_CONFIG_MAX_SECRET_FILE_SIZE 4096
#define AXIS_CONFIG_MAX_URL_FILE_SIZE AXIS_CONFIG_MAX_SECRET_FILE_SIZE
/* database.wait: forever, or this many seconds at most. */
#define AXIS_CONFIG_WAIT_FOREVER      (-1)
#define AXIS_CONFIG_MAX_DATABASE_WAIT 86400
/* The shortest metrics token accepted. */
#define AXIS_CONFIG_MIN_METRICS_TOKEN 24
#define AXIS_CONFIG_DEFAULT_WORKER_LEASE 60
#define AXIS_CONFIG_MIN_WORKER_LEASE  2
#define AXIS_CONFIG_MAX_WORKER_LEASE  3600
#define AXIS_CONFIG_DEFAULT_DATA_DIR  "/var/lib/axis"
/* The request limits (web/axis-web-limits.h), in seconds and connections. */
#define AXIS_CONFIG_DEFAULT_HEAD_TIMEOUT    30
#define AXIS_CONFIG_DEFAULT_BODY_TIMEOUT    60
#define AXIS_CONFIG_DEFAULT_IDLE_TIMEOUT    60
#define AXIS_CONFIG_MAX_REQUEST_TIMEOUT     3600
#define AXIS_CONFIG_DEFAULT_MAX_CONNECTIONS 512
#define AXIS_CONFIG_DEFAULT_MAX_PER_ADDRESS 64
#define AXIS_CONFIG_MIN_MAX_CONNECTIONS     16
#define AXIS_CONFIG_MAX_MAX_CONNECTIONS     65536

typedef struct _AxisConfig AxisConfig;

/**
 * AxisSecureCookies:
 * @AXIS_SECURE_COOKIES_AUTO: Secure when the request arrived over HTTPS
 * @AXIS_SECURE_COOKIES_ALWAYS: always Secure
 */
typedef enum
{
	AXIS_SECURE_COOKIES_AUTO,
	AXIS_SECURE_COOKIES_ALWAYS
} AxisSecureCookies;

/**
 * axis_config_new:
 *
 * Returns: (transfer full): a configuration holding the defaults: host
 *   127.0.0.1, port 7800, no database URL, pool size 8, log level info
 */
AxisConfig *
axis_config_new(void);

void
axis_config_free(AxisConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConfig, axis_config_free)

/**
 * axis_config_load_file:
 * @self: an #AxisConfig
 * @path: the configuration file
 * @error: (nullable): return location for an error
 *
 * Applies the file on top of the current values.  Fails with
 * %AXIS_ERROR_CONFIG, leaving @self unchanged, when the file cannot be
 * read (message names the path), when it contains a group or key that is
 * not listed above (message names the group and key), or when a value is
 * invalid (message names the setting as "group.key").
 *
 * Returns: %TRUE on success
 */
gboolean
axis_config_load_file(
	AxisConfig   *self,
	const gchar  *path,
	GError      **error
);

/**
 * axis_config_load_data:
 * @self: an #AxisConfig
 * @data: the file contents
 * @error: (nullable): return location for an error
 *
 * As axis_config_load_file(), from memory.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_config_load_data(
	AxisConfig   *self,
	const gchar  *data,
	GError      **error
);

/**
 * axis_config_apply_environment:
 * @self: an #AxisConfig
 * @envp: (array zero-terminated=1): an environment as from g_get_environ()
 * @error: (nullable): return location for an error
 *
 * Applies the AXIS_* overrides found in @envp.  A variable that is set
 * but empty is treated as not set.  Fails with %AXIS_ERROR_CONFIG, leaving
 * @self unchanged, when a value is invalid; the message names the
 * variable.  Also fails when AXIS_DATABASE_URL and AXIS_DATABASE_URL_FILE
 * are both set (the message names both), and when the file cannot be read
 * or holds nothing (the message names its path, never its content).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_config_apply_environment(
	AxisConfig   *self,
	gchar       **envp,
	GError      **error
);

const gchar *
axis_config_get_host(AxisConfig *self);

guint16
axis_config_get_port(AxisConfig *self);

/**
 * axis_config_get_trusted_proxies:
 *
 * Returns: (transfer none): server.trusted_proxies in canonical form, as
 *   axis_proxy_list_to_string() writes it; "" when there are none.  It
 *   was validated on the way in, so axis_proxy_list_parse() accepts it.
 */
const gchar *
axis_config_get_trusted_proxies(AxisConfig *self);

AxisSecureCookies
axis_config_get_secure_cookies(AxisConfig *self);

/**
 * axis_config_get_database_url:
 *
 * Returns: (transfer none) (nullable): the database URL, or %NULL if unset
 */
const gchar *
axis_config_get_database_url(AxisConfig *self);

/**
 * axis_config_get_database_pool_size:
 *
 * Returns: the number of database connections to keep, 1 to 64, as set;
 *   %AXIS_CONFIG_DEFAULT_POOL_SIZE when it is not set, which axisd raises
 *   to what its job queue needs and some (see
 *   axis_config_has_database_pool_size())
 */
guint
axis_config_get_database_pool_size(AxisConfig *self);

/**
 * axis_config_set_database_pool_size_used:
 * @size: the pool size axisd will use, or 0 for none chosen
 *
 * Records the size actually used, which axis_config_describe() then prints
 * in place of the configured one.
 */
void
axis_config_set_database_pool_size_used(
	AxisConfig *self,
	guint       size
);

/**
 * axis_config_has_database_pool_size:
 *
 * Returns: %TRUE when the file or the environment set the pool size; then
 *   axisd uses it as it is, and refuses to start when it is smaller than
 *   its job queue needs
 */
gboolean
axis_config_has_database_pool_size(AxisConfig *self);

AxisLogLevel
axis_config_get_log_level(AxisConfig *self);

/**
 * axis_config_get_jobs_grace_period:
 *
 * Returns: how long running jobs get to finish at shutdown, in seconds
 */
guint
axis_config_get_jobs_grace_period(AxisConfig *self);

/**
 * axis_config_get_worker_lease:
 *
 * Returns: workers.lease: how long a remote worker's lease lasts after a
 *   claim or a heartbeat, in seconds (it is told to heartbeat four times
 *   as often)
 */
guint
axis_config_get_worker_lease(AxisConfig *self);

/* server.head_timeout, server.body_timeout and server.idle_timeout, in
 * seconds; server.max_connections and server.max_connections_per_address. */
guint
axis_config_get_head_timeout(AxisConfig *self);

guint
axis_config_get_body_timeout(AxisConfig *self);

guint
axis_config_get_idle_timeout(AxisConfig *self);

guint
axis_config_get_max_connections(AxisConfig *self);

guint
axis_config_get_max_connections_per_address(AxisConfig *self);

/**
 * axis_config_get_data_dir:
 *
 * Returns: (transfer none): data.dir, absolute and normalised
 */
const gchar *
axis_config_get_data_dir(AxisConfig *self);

/**
 * axis_config_list_jobs_concurrency:
 *
 * Returns: (transfer full): the queues jobs.concurrency names, in the
 *   order given; an empty array if it names none
 */
gchar **
axis_config_list_jobs_concurrency(AxisConfig *self);

/**
 * axis_config_get_jobs_concurrency:
 * @self: an #AxisConfig
 * @queue: a queue name
 * @concurrency: (out) (optional): the count jobs.concurrency gives
 *   @queue; untouched if it gives none
 *
 * Returns: %TRUE if jobs.concurrency names @queue
 */
gboolean
axis_config_get_jobs_concurrency(
	AxisConfig  *self,
	const gchar *queue,
	guint       *concurrency
);

/**
 * axis_config_check_required:
 * @self: an #AxisConfig
 * @error: (nullable): return location for an error
 *
 * Checks that every setting without a default has been given; call it
 * after the file and the environment have been applied.  Fails with
 * %AXIS_ERROR_CONFIG when the database URL is missing (the message says
 * a database URL is required and where to set it), and when only one of
 * the first administrator's username and password is given.
 *
 * Returns: %TRUE if nothing required is missing
 */
gboolean
axis_config_check_required(
	AxisConfig   *self,
	GError      **error
);

/**
 * axis_config_get_database_wait:
 *
 * Returns: database.wait in seconds (0: try once), or
 *   %AXIS_CONFIG_WAIT_FOREVER
 */
gint
axis_config_get_database_wait(AxisConfig *self);

AxisLogFormat
axis_config_get_log_format(AxisConfig *self);

/**
 * axis_config_get_admin_username:
 *
 * Returns: (transfer none) (nullable): setup.admin_username as given (the
 *   web layer checks it), or %NULL
 */
const gchar *
axis_config_get_admin_username(AxisConfig *self);

/**
 * axis_config_get_admin_password:
 *
 * Returns: (transfer none) (nullable): setup.admin_password, or %NULL
 *   (also after axis_config_forget_admin_password())
 */
const gchar *
axis_config_get_admin_password(AxisConfig *self);

/* Wipes the administrator's password from @self once it has been used. */
void
axis_config_forget_admin_password(AxisConfig *self);

/**
 * axis_config_get_metrics_token:
 *
 * Returns: (transfer none) (nullable): metrics.token, or %NULL when the
 *   metrics endpoint is off
 */
const gchar *
axis_config_get_metrics_token(AxisConfig *self);

/**
 * axis_config_get_outbound_bind:
 *
 * Returns: (transfer none): outbound.bind in canonical form (an interface
 *   name, or an address as inet_ntop() writes it), "" for none; it was
 *   validated on the way in, so axis_outbound_target_parse() accepts it
 */
const gchar *
axis_config_get_outbound_bind(AxisConfig *self);

AxisOutboundScope
axis_config_get_outbound_scope(AxisConfig *self);

/**
 * axis_config_environ_without_secrets:
 * @envp: (nullable): an environment
 *
 * Returns: (transfer full): a copy of @envp without the secrets' variables
 *   and their _FILE forms (for `axisd --healthcheck`, which needs none of
 *   them and may not be able to read their files)
 */
gchar **
axis_config_environ_without_secrets(gchar **envp);

/**
 * axis_config_redact_url:
 * @url: (nullable): a database URL or libpq "key=value" connection string
 *
 * Replaces any password with "****".  Handles the URL form
 * (scheme://user:PASSWORD@host/...), a "password=" query parameter, and
 * the "password=..." keyword of a key=value connection string (quoted
 * with single quotes or not).  Anything that cannot be understood is
 * returned as "<redacted>" rather than passed through.
 *
 * Returns: (transfer full) (nullable): the redacted string, %NULL for %NULL
 */
gchar *
axis_config_redact_url(const gchar *url);

/**
 * axis_config_describe:
 *
 * Returns: (transfer full): a one-line summary of the effective settings,
 *   safe to log: the database URL is passed through
 *   axis_config_redact_url()
 */
gchar *
axis_config_describe(AxisConfig *self);

G_END_DECLS

#endif /* AXIS_CONFIG_H */
