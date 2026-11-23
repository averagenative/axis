/*
 * axis-web-limits.h - what one request and one client may cost, from the
 * moment the port is open
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Installed on the server before it listens, so they hold in every state of
 * the daemon (waiting for its database, standing by, serving, suspended),
 * and kept on the server whatever else is installed later.
 *
 *  - A request body: at most AXIS_WEB_MAX_REQUEST_BODY (the worker API
 *    AXIS_WORKER_MAX_REQUEST_BYTES).  One too long, or of unknown length, is
 *    answered 413 with Connection: close and is not kept (libsoup reads it
 *    to its end and drops it as it arrives, within the body deadline).
 *    While bodies are refused (until the daemon serves, and while it is
 *    suspended: nothing then needs one), any body is answered at once with
 *    503, Retry-After and what the start-up gate would say, and no byte of
 *    it is read.
 *  - Deadlines: a new connection has head_timeout for its first request's
 *    whole head; a request with a body then has body_timeout for the body;
 *    a kept-alive connection has idle_timeout, from the end of one
 *    response, until the next request's first byte, and that request then
 *    has head_timeout for its head.  A connection that misses one is
 *    closed.  An event stream, once its head has arrived, is under the
 *    hub's limits (axis-web-hub.h), not these: it has no body, and it is
 *    not idle.  A connection that carried a worker's valid token is not
 *    closed for being idle.
 *  - Connections: at most max_connections open at once, and at most
 *    max_per_address from one public address (an IPv6 address counts by
 *    its /64).  A loopback, private or link-local address (where reverse
 *    proxies and container gateways connect from) is held to the total
 *    only, and so is a trusted proxy (the same rule as the forwarding
 *    headers: core/axis-proxy.h); instead each client a trusted proxy
 *    forwards may have at most max_per_address requests in flight at once
 *    (event streams and the worker API not counted), and more are answered
 *    429 with Retry-After.  A connection over a cap is let in, up to
 *    max_overflow of them, with two seconds for its head: a probe (/livez,
 *    /readyz, /healthz) from a loopback or private address is then served,
 *    so that a platform's health checks hold however full the daemon is,
 *    and anything else is answered 503 with Retry-After; beyond max_overflow
 *    a connection is closed as soon as it is seen.  A request with a
 *    worker's valid token is not held to the per-address cap nor to the
 *    requests in flight.
 *
 * What libsoup does not let this do from outside (deploy/README.md says
 * what the reverse proxy must do instead): a connection is seen only once
 * libsoup has accepted it, so one over a cap costs an accept and a close
 * (and the kernel's backlog still queues connections not yet accepted);
 * the handler's time is not limited (a page waiting for the database
 * waits); and nothing times how fast a client reads a response, so a
 * client that reads slowly holds its connection until the response is
 * written (an event stream's reader is the hub's to drop).
 *
 * Main loop thread only, except axis_web_limits_set_bodies().
 */

#ifndef AXIS_WEB_LIMITS_H
#define AXIS_WEB_LIMITS_H

#include <glib.h>
#include <htmx-glib.h>

#include "core/axis-proxy.h"
#include "web/axis-web-auth.h"

G_BEGIN_DECLS

#define AXIS_WEB_LIMITS_HEAD_TIMEOUT_S     30
#define AXIS_WEB_LIMITS_BODY_TIMEOUT_S     60
#define AXIS_WEB_LIMITS_IDLE_TIMEOUT_S     60
#define AXIS_WEB_LIMITS_MAX_CONNECTIONS    512
#define AXIS_WEB_LIMITS_MAX_PER_ADDRESS    64

/* Connections over a cap let in at once, to be answered. */
#define AXIS_WEB_LIMITS_MAX_OVERFLOW       32

/**
 * AxisWebLimitsOptions:
 *
 * Every field left 0 (or %NULL) takes its default.  Tests shorten the
 * times, given in milliseconds here.
 */
typedef struct
{
	guint        head_timeout_ms;
	guint        body_timeout_ms;
	guint        idle_timeout_ms;
	guint        max_connections;
	guint        max_per_address;
	guint        max_overflow;
	const gchar *trusted_proxies;	/* server.trusted_proxies, until set_auth() */
	gboolean     local_is_public;	/* tests only: hold loopback to the per-address cap */
} AxisWebLimitsOptions;

typedef struct _AxisWebLimits AxisWebLimits;

/**
 * AxisWebLimitsRefusalFunc:
 *
 * What a refused body is told while bodies are refused (the start-up
 * gate's words for the daemon's state), or %NULL for a plain sentence.
 * Main loop.
 */
typedef const gchar *(*AxisWebLimitsRefusalFunc)(gpointer user_data);

/**
 * axis_web_limits_install:
 * @server: the server; the limits live as long as it does
 * @options: (nullable): used only when @server has no limits yet
 *
 * Installs the limits on @server, or finds the ones installed already.
 * Bodies are accepted until axis_web_limits_set_bodies() says otherwise.
 * The trusted proxies are @options' (addresses and ranges only: a name
 * trusts nothing until an auth, whose list looks names up, is set).
 *
 * Returns: (transfer none): the server's limits
 */
AxisWebLimits *
axis_web_limits_install(
	HtmxServer                 *server,
	const AxisWebLimitsOptions *options
);

/* Returns: (transfer none) (nullable): @server's limits, if installed */
AxisWebLimits *
axis_web_limits_get(HtmxServer *server);

AxisWebLimits *
axis_web_limits_ref(AxisWebLimits *self);

void
axis_web_limits_unref(AxisWebLimits *self);

/* Whether request bodies are accepted (within their limit) or every one is
 * refused.  Safe from any thread. */
void
axis_web_limits_set_bodies(
	AxisWebLimits *self,
	gboolean       accepted
);

/* What a refused body is told (see AxisWebLimitsRefusalFunc). */
void
axis_web_limits_set_refusal(
	AxisWebLimits            *self,
	AxisWebLimitsRefusalFunc  func,
	gpointer                  user_data
);

/* The auth whose trusted proxies the caps judge by and whose workers'
 * tokens exempt a request, borrowed until it is set again; %NULL goes back
 * to the proxies the limits were installed with, and no workers. */
void
axis_web_limits_set_auth(
	AxisWebLimits *self,
	AxisWebAuth   *auth
);

/* One paragraph for the administrators' pages when a cap has refused
 * anything since the start (how many, the last address and why, and what
 * to do), else %NULL. */
gchar *
axis_web_limits_describe_refused(AxisWebLimits *self);

/* ── For tests ────────────────────────────────────────────────── */

/* Connections open now (as last seen). */
guint
axis_web_limits_count_connections(AxisWebLimits *self);

/* Connections closed for a missed deadline, and connections or requests
 * refused for a cap, since the limits were installed. */
guint
axis_web_limits_count_timed_out(AxisWebLimits *self);

guint
axis_web_limits_count_refused(AxisWebLimits *self);

G_END_DECLS

#endif /* AXIS_WEB_LIMITS_H */
