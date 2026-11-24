/*
 * axis-web-workers-private.h - what the Conversion workers pages share:
 * the page itself (axis-web-workers.c) and "Attach a worker"
 * (axis-web-workers-attach.c)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_WORKERS_PRIVATE_H
#define AXIS_WEB_WORKERS_PRIVATE_H

#include <htmx-glib.h>

#include "api/axis-worker-store.h"
#include "web/axis-web.h"
#include "web/axis-web-workers.h"

G_BEGIN_DECLS

/* What a page that waits for a worker follows: an event with no content,
 * for administrators only, each time a worker registers. */
#define AXIS_WORKERS_TOPIC         "workers:admin"
#define AXIS_WORKERS_EVENT_CHANGED "workers-changed"

struct _AxisWebWorkers
{
	AxisDbPool       *pool;
	AxisWorkerTokens *tokens;
	AxisEstimator    *estimator;	/* owned reference, or NULL */
	AxisJobRegistry  *kinds;	/* owned reference, or NULL */
	AxisConversions  *conversions;	/* owned reference, or NULL: where checks run, the pause */
	AxisWeb          *web;		/* borrowed, from register_routes: its hub */
	guint             heartbeat_ms;
	guint             pending;
	gboolean          stopping;
	gint64            started_us;	/* when these pages were made: the daemon's start */
};

typedef void (*AxisWebWorkersWorkFunc)(AxisWebWorkers *self, gpointer data);
typedef HtmxResponse *(*AxisWebWorkersDoneFunc)(AxisWebWorkers *self, HtmxRequest *request, gpointer data);

/*
 * Runs @work on a worker thread, then answers @request with what @done
 * returns, on the main loop.  Returns the deferred response for the handler
 * to return, or at once a 503 when too much is waiting or the pages are
 * stopping (then neither runs).  @free_data is called on the main loop in
 * every case.
 */
HtmxResponse *
axis_web_workers_dispatch(
	AxisWebWorkers         *self,
	HtmxRequest            *request,
	AxisWebWorkersWorkFunc  work,
	AxisWebWorkersDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
);

/* How long after its last request a worker still counts as connected. */
guint
axis_web_workers_connected_ms(AxisWebWorkers *self);

/* The encoders a worker measured, as the Conversion workers page shows them. */
HtmxNode *
axis_web_workers_encoders_table(const AxisEncoderCapabilities *caps);

/* The routes of "Attach a worker" (axis-web-workers-attach.c). */
void
axis_web_workers_attach_register(
	AxisWeb        *web,
	AxisWebWorkers *self
);

G_END_DECLS

#endif /* AXIS_WEB_WORKERS_PRIVATE_H */
