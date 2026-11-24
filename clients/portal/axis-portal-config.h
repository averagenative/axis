/*
 * axis-portal-config.h - axis-portal's configuration
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  A key file with one group, [portal], and
 * the environment, which wins: every key `name` is also AXIS_PORTAL_NAME.
 * A key the portal does not know stops it, with the key's name: there is no
 * key for a database, a media path, an indexer, a download client or a
 * details provider, and none can be smuggled in under another spelling.
 * The variables Kubernetes adds for a Service called axis-portal
 * (AXIS_PORTAL_SERVICE_HOST, AXIS_PORTAL_PORT_7802_TCP and the like, with
 * values of their shape) are not settings: they are left aside and named in
 * one line of the log.
 *
 *   url                   the daemon's portal listener, such as
 *                         http://axis:7801 (required)
 *   token_file            a file that holds the portal token (required,
 *                         unless `token` is given: AXIS_PORTAL_TOKEN in the
 *                         environment, or `token` in the file).  Never on
 *                         the command line.  The file is the way to give
 *                         it: one that every user may read is warned of at
 *                         start; AXIS_PORTAL_TOKEN is overwritten and taken
 *                         out of the environment once read (main.c); an
 *                         inline `token` stays in the key file reader's
 *                         memory, which is freed without being wiped.
 *   ca_file               certificate authorities for an https `url`
 *                         (default: the system's)
 *   allow_insecure_http   true: send the token over plain http to a host
 *                         that is not this machine (a private container
 *                         network).  Refused otherwise.
 *   request_timeout       seconds the daemon has to answer (1 to 120; 20)
 *   web_listen_host       the address the pages are served on (127.0.0.1);
 *                         --healthcheck needs a loopback address or all
 *                         addresses (0.0.0.0, ::)
 *   web_listen_port       their port (7802).  (Not "listen_port": the
 *                         daemon's AXIS_PORTAL_LISTEN_PORT is the port of
 *                         its portal API, and the two must not be mixed up;
 *                         the portal refuses to start with that one set.)
 *   trusted_proxies       the reverse proxies whose X-Forwarded-For,
 *                         X-Forwarded-Proto and X-Forwarded-Host are
 *                         believed: addresses, ranges and host names, as
 *                         the daemon's server.trusted_proxies
 *   insecure_browser_http for tests only: serve the login over plain http,
 *                         with cookies that are not Secure.  Without it the
 *                         portal serves nothing but its "needs HTTPS" page
 *                         to a request a trusted proxy does not report as
 *                         https.  Refused unless web_listen_host is a
 *                         loopback address.
 *   max_connections       connections open at once (256)
 *   max_connections_per_address  from one public address (32)
 *   log_level             debug, info, warning or error (info)
 *   log_format            text or json (text)
 *
 * The portal writes no file, and has no key that names a place to write.
 */

#ifndef AXIS_PORTAL_CONFIG_H
#define AXIS_PORTAL_CONFIG_H

#include <glib.h>

#include "core/axis-log.h"

G_BEGIN_DECLS

#define AXIS_PORTAL_CONFIG_MAX_FILE_SIZE (64 * 1024)
#define AXIS_PORTAL_DEFAULT_PORT         7802

typedef struct
{
	gchar         *url;		/* scheme://host[:port][/prefix], no trailing '/' */
	gchar         *host;
	gboolean       https;
	gboolean       loopback;
	gchar         *token;		/* secret; %NULL when it was not asked for */
	gboolean       token_file_readable_by_all;	/* its mode lets every user read it */
	gchar         *ca_file;
	gboolean       allow_insecure_http;
	guint          request_timeout_ms;
	gchar         *listen_host;
	guint16        listen_port;
	gchar         *trusted_proxies;
	gboolean       insecure_browser_http;
	guint          max_connections;
	guint          max_per_address;
	AxisLogLevel   log_level;
	AxisLogFormat  log_format;
	gchar         *service_links;	/* Kubernetes' variables left aside, by name; nullable */
} AxisPortalConfig;

/**
 * axis_portal_config_load:
 * @path: (nullable): the key file
 * @envp: the environment
 * @need_token: %FALSE for --healthcheck, which needs the port only
 * @error: (nullable): why not; it never holds the token
 *
 * Returns: (transfer full) (nullable): the configuration
 */
AxisPortalConfig *
axis_portal_config_load(
	const gchar  *path,
	gchar       **envp,
	gboolean      need_token,
	GError      **error
);

/* Wipes the token and frees. */
void
axis_portal_config_free(AxisPortalConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalConfig, axis_portal_config_free)

/* The settings in one line, for the log at start: without the token. */
gchar *
axis_portal_config_describe(const AxisPortalConfig *self);

G_END_DECLS

#endif /* AXIS_PORTAL_CONFIG_H */
