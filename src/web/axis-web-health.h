/*
 * axis-web-health.h - the probes: /livez, /readyz and /healthz
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Public routes (they need no session), answered without ever blocking the
 * main loop.  Each says only its status word, {"status":"..."}:
 *
 *   /livez    200 "alive": the process serves; asks nothing.
 *   /readyz   200 "ready" when this daemon holds the database lock and the
 *             database answers at this build's schema; 503 "not ready", or
 *             "database lock lost".
 *   /healthz  200 "ok" when the database answers, 503 "degraded" when not.
 *             To a request with a session, or with the metrics token as a
 *             bearer token, it also says the version and "database": "ok"
 *             or "unreachable".  A wrong token counts against the address
 *             as at /metrics (axis_web_metrics_check()), and a throttled
 *             address gets the status word only.
 *
 * A probe's database question is shared: at most one of each kind (the
 * ping of /healthz, the schema check of /readyz) is asked at a time, on a
 * thread of its own, and every request that arrives meanwhile waits for
 * that one answer; an answer is then reused for AXIS_WEB_HEALTH_REUSE_MS.
 * However many probes arrive, they hold at most two pooled connections.
 *
 * Main loop thread only, apart from the questions' own threads.
 */

#ifndef AXIS_WEB_HEALTH_H
#define AXIS_WEB_HEALTH_H

#include <glib.h>
#include <htmx-glib.h>

#include "web/axis-web.h"
#include "web/axis-web-metrics.h"

G_BEGIN_DECLS

/* An answer is reused for this long. */
#define AXIS_WEB_HEALTH_REUSE_MS 1000

/* Runs on a thread of its own: whether the database answers (or is at the
 * schema expected); @error says why not. */
typedef gboolean (*AxisWebHealthAsk)(
	gpointer  data,
	GError  **error
);

/* Any thread, quick: whether this daemon holds the database lock. */
typedef gboolean (*AxisWebHealthHeld)(gpointer data);

typedef struct
{
	AxisWebHealthAsk   ping;		/* /healthz */
	AxisWebHealthAsk   current;	/* /readyz */
	AxisWebHealthHeld  held;		/* /readyz; NULL: always held */
	gpointer           data;
	AxisWebMetrics    *metrics;	/* nullable: whose token opens the full /healthz */
	guint              reuse_ms;	/* 0: AXIS_WEB_HEALTH_REUSE_MS */
} AxisWebHealthHooks;

typedef struct _AxisWebHealth AxisWebHealth;

AxisWebHealth *
axis_web_health_new(const AxisWebHealthHooks *hooks);

/* Registers /livez, /readyz and /healthz on @web. */
void
axis_web_health_register(
	AxisWebHealth *self,
	AxisWeb       *web
);

/* Blocks until no question is being asked (call after the server stopped,
 * before what the questions use goes). */
void
axis_web_health_drain(AxisWebHealth *self);

/* Drains, delivers what the last answers owe, and frees. */
void
axis_web_health_free(AxisWebHealth *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebHealth, axis_web_health_free)

/* {"status":@word} with @status, never cached: what a probe says, also
 * before the daemon serves (the start-up gate's answer). */
HtmxResponse *
axis_web_health_probe_response(
	guint        status,
	const gchar *word
);

/* A probe's answer to a process on this machine (the connection's peer is
 * a loopback address) also says which process answered, in this header:
 * so that a test, or a health check, knows it asked the daemon it meant to
 * and not another one on a port it reused. */
#define AXIS_WEB_HEALTH_PROCESS_HEADER "X-Axis-Process"

void
axis_web_health_mark(
	HtmxRequest  *request,
	HtmxResponse *response
);

/* ── For tests ────────────────────────────────────────────────── */

/* How many times each question has been asked. */
guint
axis_web_health_count_pings(AxisWebHealth *self);

guint
axis_web_health_count_checks(AxisWebHealth *self);

G_END_DECLS

#endif /* AXIS_WEB_HEALTH_H */
