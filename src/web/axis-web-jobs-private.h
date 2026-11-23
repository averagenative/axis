/*
 * axis-web-jobs-private.h - shared by the files of the Jobs pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_JOBS_PRIVATE_H
#define AXIS_WEB_JOBS_PRIVATE_H

#include "web/axis-web-jobs.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-jobs-data.h"
#include "web/axis-web-jobs-view.h"

G_BEGIN_DECLS

/* Live updates are sent at most this often, so a job's row changes at
 * most twice a second whatever its events. */
#define AXIS_JOBS_COALESCE_MS      500

/* The running count is read again this often even if no event came (the
 * queue's notification connection may have been down). */
#define AXIS_JOBS_RECOUNT_S        30

/* Jobs waiting for their next update; more than this and the list is
 * told to fetch itself again instead. */
#define AXIS_JOBS_MAX_DIRTY        1000

/* A fragment is split into events of about this size. */
#define AXIS_JOBS_EVENT_CHUNK      (48 * 1024)

struct _AxisWebJobs
{
	AxisDbPool   *pool;
	AxisWebHub   *hub;		/* nullable; owned by the caller */
	AxisJobQueue *queue;		/* nullable until attached; borrowed */
	AxisJobRegistry *kinds;		/* nullable: labels and classes; set once, before use */
	gulong        event_id;

	guint         pending;		/* tasks off the main loop, not yet answered */
	gboolean      stopping;
	AxisJobsActivity activity;	/* what the header shows, as last counted */

	/* The bridge from job events to the hub. */
	GHashTable   *dirty;		/* of gint64*: jobs to send again */
	gboolean      overflow;		/* ids were dropped: the lists must refresh */
	gboolean      changed;		/* a state change is waiting: the lists must refresh */
	gboolean      recount;		/* read the running count */
	gboolean      fetching;
	guint         flush_id;
	guint         recount_id;
	gint64        last_flush_us;	/* monotonic */

	/* What needs attention is read again after a job ended (nullable: set
	 * when the routes are registered). */
	AxisAttentionPing *attention;
};

typedef void (*AxisJobsWorkFunc)(AxisWebJobs *jobs, gpointer data);
typedef void (*AxisJobsDoneFunc)(AxisWebJobs *jobs, gpointer data);

/**
 * axis_web_jobs_run:
 *
 * Runs @work on a thread of the global pool, then @done on the main loop.
 * Refuses (returns %FALSE after freeing @data) when too many tasks wait
 * or the jobs pages are stopping; neither function runs then.  @done
 * runs even while stopping, so that every request is answered.  Main
 * loop only.
 */
gboolean
axis_web_jobs_run(
	AxisWebJobs      *jobs,
	AxisJobsWorkFunc  work,
	AxisJobsDoneFunc  done,
	gpointer          data,
	GDestroyNotify    free_data
);

/* Runs @work(jobs, @data) on a worker thread, then answers @request with
 * @build(jobs, request, @data) on the main loop; returns the deferred
 * response for the handler (a 503 when too much waits). */
HtmxResponse *
axis_web_jobs_dispatch(
	AxisWebJobs    *jobs,
	HtmxRequest    *request,
	void          (*work)(AxisWebJobs *jobs, gpointer data),
	HtmxResponse *(*build)(AxisWebJobs *jobs, HtmxRequest *request, gpointer data),
	gpointer        data,
	GDestroyNotify  free_data
);

/* The Activity page (axis-web-jobs-activity.c): GET /activity. */
void
axis_web_jobs_activity_register(
	AxisWeb     *web,
	AxisWebJobs *jobs
);

/* The bridge. */
void
axis_web_jobs_bridge_start(AxisWebJobs *jobs);

void
axis_web_jobs_bridge_stop(AxisWebJobs *jobs);

void
axis_web_jobs_bridge_event(
	AxisWebJobs        *jobs,
	const AxisJobEvent *event
);

G_END_DECLS

#endif /* AXIS_WEB_JOBS_PRIVATE_H */
