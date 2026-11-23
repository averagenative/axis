/*
 * axis-web-jobs.h - the Jobs pages: list, detail, cancel and retry, live
 * updates and the running-jobs count in every page's header
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_jobs_get_module() with the module registry, passing
 * the AxisWebJobs as the module's user data; its pages then exist once
 * the shell is made.  After the job queue exists, axis_web_jobs_attach()
 * connects its events to the web hub, so that rows and the header count
 * change while a page is open.  Everything works without a hub (a page
 * is complete when it loads) and without the queue attached (the
 * list and detail are read from the database; cancel and retry answer
 * 503).
 *
 *   GET  /jobs            the list: filters "state" (queued, running,
 *                         succeeded, failed, cancelled, or active for
 *                         queued and running) and "queue", and "before"
 *                         for the next page
 *   GET  /jobs/:id        one job, with its last error
 *   POST /jobs/:id/cancel administrators only
 *   POST /jobs/:id/retry  administrators only: a new job with the same
 *                         kind and payload
 *
 * Main loop only; the database is read on worker threads.
 */

#ifndef AXIS_WEB_JOBS_H
#define AXIS_WEB_JOBS_H

#include <glib.h>

#include "core/axis-module.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-queue.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebJobs AxisWebJobs;

#define AXIS_WEB_JOBS_MODULE_ID "jobs-ui"

/**
 * axis_web_jobs_new:
 * @pool: the database; it must outlive the result
 * @hub: (nullable): where live updates go; must outlive the result
 */
AxisWebJobs *
axis_web_jobs_new(
	AxisDbPool *pool,
	AxisWebHub *hub
);

/* Stops, then frees. */
void
axis_web_jobs_free(AxisWebJobs *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebJobs, axis_web_jobs_free)

/**
 * axis_web_jobs_get_module:
 *
 * Returns: (transfer none): the module description, in static storage;
 *   register it with the AxisWebJobs as its user data
 */
const AxisModuleInfo *
axis_web_jobs_get_module(void);

/**
 * axis_web_jobs_attach:
 * @self: the pages
 * @queue: the job queue; borrowed, so call axis_web_jobs_stop() before it
 *   is freed
 * @error: (nullable): return location for an error
 *
 * Counts the running jobs (blocking: call it before the main loop runs),
 * then listens to @queue's "job-event" and publishes what changed on the
 * hub, coalesced to at most two updates a second per job.  Cancel and
 * retry use @queue.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_web_jobs_attach(
	AxisWebJobs   *self,
	AxisJobQueue  *queue,
	GError       **error
);

/**
 * axis_web_jobs_stop:
 *
 * Stops listening and waits for the database work in flight, answering
 * its requests.  Blocks; runs the default main context meanwhile.  Call
 * after the server has stopped taking requests and before the pool and
 * the queue are freed.
 */
void
axis_web_jobs_stop(AxisWebJobs *self);

/* The number of running jobs of work the header shows. */
gint
axis_web_jobs_get_running(AxisWebJobs *self);

/* The header's counts read again now, blocking; for a caller without the
 * queue attached (the demo instance), whose counts never change by
 * themselves. */
gboolean
axis_web_jobs_count_now(
	AxisWebJobs  *self,
	GError      **error
);

/**
 * axis_web_jobs_set_kinds:
 * @self: the pages
 * @kinds: (nullable): the job registry: what each kind is called in plain
 *   words and whether it is work or upkeep; referenced
 *
 * Call once, before the pages are served and before
 * axis_web_jobs_attach().  Without it a job is named by its kind and
 * counts as work.
 */
void
axis_web_jobs_set_kinds(
	AxisWebJobs     *self,
	AxisJobRegistry *kinds
);

G_END_DECLS

#endif /* AXIS_WEB_JOBS_H */
