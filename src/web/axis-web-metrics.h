/*
 * axis-web-metrics.h - the optional Prometheus endpoint, /metrics
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Off unless a metrics token is configured (metrics.token,
 * AXIS_METRICS_TOKEN or AXIS_METRICS_TOKEN_FILE).  Off, /metrics has no
 * route and answers 404 like any unknown path.  On, it answers 200 with the
 * Prometheus text format only to a request whose Authorization header is
 * "Bearer <token>" (compared in constant time), and 401 to every other, with
 * nothing in the body; it never sets a cookie and never redirects.  Each
 * wrong token (a bearer token of the token's form that is not the token)
 * counts against the client's address (as the trusted-proxy rule decides
 * it, an IPv6 address by its /64): after ten, the address waits (2 s,
 * doubling to 15 minutes) and its wrong tokens are answered 429 with
 * Retry-After; the right token always works, and other addresses are not
 * affected.  Any other Authorization header is no guess and is not
 * counted.  Wrong tokens are logged at most once a minute.  The
 * token is the only protection: put the endpoint behind the same TLS as the
 * pages when it crosses a network, and give Prometheus the token from a file
 * (authorization.credentials_file).
 *
 * What it reports, and nothing else: requests answered by status class and
 * a histogram of their durations (from the moment the shell sees a request
 * to its answer; streams are counted but not timed), the jobs queued and
 * running by queue, downloads and imports by state (counts only: no title,
 * no path, no name), whether the database answered, and the build's version.
 * No label holds anything a request or a user typed.
 *
 * Main loop thread only, apart from the database reads, which run on a
 * worker thread with a deadline.
 */

#ifndef AXIS_WEB_METRICS_H
#define AXIS_WEB_METRICS_H

#include <glib.h>
#include <htmx-glib.h>

#include "db/axis-db-pool.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebMetrics AxisWebMetrics;

/* The upper bounds of the duration histogram, in seconds. */
#define AXIS_WEB_METRICS_N_BUCKETS 11

/* The database reads of one scrape get this long in all. */
#define AXIS_WEB_METRICS_QUERY_TIMEOUT_MS 2000

/* The lengths a metrics token may have (as the configuration accepts). */
#define AXIS_WEB_METRICS_TOKEN_MIN 24
#define AXIS_WEB_METRICS_TOKEN_MAX 256

/**
 * axis_web_metrics_new:
 * @token: (nullable): the metrics token; %NULL turns the endpoint off
 *
 * Returns: (transfer full): the metrics, counting nothing yet
 */
AxisWebMetrics *
axis_web_metrics_new(const gchar *token);

/* Waits for scrapes in flight (call after the server stopped), then frees. */
void
axis_web_metrics_free(AxisWebMetrics *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebMetrics, axis_web_metrics_free)

gboolean
axis_web_metrics_is_enabled(AxisWebMetrics *self);

/* Installs the middleware that counts and times requests (when enabled;
 * otherwise nothing). */
void
axis_web_metrics_install_timing(
	AxisWebMetrics *self,
	HtmxServer     *server
);

/* Registers /metrics on @web, reading @pool (when enabled; otherwise
 * nothing, so the path answers 404). */
void
axis_web_metrics_register(
	AxisWebMetrics *self,
	AxisWeb        *web,
	AxisDbPool     *pool
);

/* Blocks until no scrape is reading the database. */
void
axis_web_metrics_drain(AxisWebMetrics *self);

/**
 * AxisWebMetricsToken:
 * @AXIS_WEB_METRICS_TOKEN_NONE: the request carries no bearer token of the
 *   token's form (or the endpoint is off)
 * @AXIS_WEB_METRICS_TOKEN_GOOD: it carries the metrics token (always,
 *   whatever came before from its address)
 * @AXIS_WEB_METRICS_TOKEN_WRONG: it carries another; counted against its
 *   address
 * @AXIS_WEB_METRICS_TOKEN_THROTTLED: it carries another, and its address
 *   has had too many wrong ones
 */
typedef enum
{
	AXIS_WEB_METRICS_TOKEN_NONE,
	AXIS_WEB_METRICS_TOKEN_GOOD,
	AXIS_WEB_METRICS_TOKEN_WRONG,
	AXIS_WEB_METRICS_TOKEN_THROTTLED
} AxisWebMetricsToken;

/**
 * axis_web_metrics_check:
 * @request: a request the shell's middleware has seen (its client address)
 * @wait_us: (out) (optional): for THROTTLED, how long the address waits
 *
 * What @request's Authorization header is, for /metrics and for anything
 * else the metrics token opens (the full /healthz).  Main loop only.
 */
AxisWebMetricsToken
axis_web_metrics_check(
	AxisWebMetrics *self,
	HtmxRequest    *request,
	gint64         *wait_us
);

/* ── For tests ────────────────────────────────────────────────── */

/* Counts one answered request: @status, after @duration_us (negative: a
 * stream, counted but not timed). */
void
axis_web_metrics_record(
	AxisWebMetrics *self,
	guint           status,
	gint64          duration_us
);

/* Whether @authorization (an Authorization header, nullable) carries the
 * token.  Always %FALSE when the endpoint is off. */
gboolean
axis_web_metrics_authorized(
	AxisWebMetrics *self,
	const gchar    *authorization
);

/**
 * axis_web_metrics_render:
 * @pool: (nullable): the database; %NULL reports it down
 *
 * Returns: (transfer full): the text a scrape gets now (blocking: it reads
 *   @pool)
 */
gchar *
axis_web_metrics_render(
	AxisWebMetrics *self,
	AxisDbPool     *pool
);

G_END_DECLS

#endif /* AXIS_WEB_METRICS_H */
