/*
 * axis-job-live.h - the jobs queued or running for something a page shows,
 * and where each stands
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A page that lets a person start work shows the work already in hand for
 * what it shows (a search of a season, a scan of a volume): so that a
 * button can say it is running and not be clicked again, and so that a
 * click while it runs is answered with it.  Jobs are found by their
 * de-duplication keys, which every module prefixes with its kind and the
 * thing they are for ("tv.search:12:season:1"): one statement reads every
 * live job under a prefix, with its place in its queue, whatever the
 * number of rows the page has.
 *
 * Reads only; for worker threads (it blocks on the database).
 */

#ifndef AXIS_JOB_LIVE_H
#define AXIS_JOB_LIVE_H

#include <glib.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

/* The most jobs one read returns. */
#define AXIS_JOB_LIVE_MAX 500

/**
 * AxisJobLive:
 * @id: the job
 * @kind: its kind
 * @key: its de-duplication key
 * @running: running; else queued
 * @queued_us: when it was queued (microseconds since 1970)
 * @started_us: when it started, 0 while queued
 * @run_at_us: when it may be claimed (a retry waits)
 * @progress: 0 to 1, -1 when none is recorded
 * @message: (nullable): what it says it is doing
 * @ahead: while queued, how many jobs of its queue are claimed before it
 *   (queued with a higher priority, or the same and earlier), 0 when running
 *
 * A job queued or running.
 */
typedef struct
{
	gint64    id;
	gchar    *kind;
	gchar    *key;
	gboolean  running;
	gint64    queued_us;
	gint64    started_us;
	gint64    run_at_us;
	gdouble   progress;
	gchar    *message;
	guint     ahead;
} AxisJobLive;

void
axis_job_live_free(AxisJobLive *live);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobLive, axis_job_live_free)

/**
 * axis_job_live_read:
 * @conn: a connection held by the calling thread
 * @prefix: the keys' prefix, ending in a delimiter ("tv.search:12:", never
 *   "movies.search:1", which is also the prefix of film 10's keys)
 * @live: (out) (transfer container) (element-type AxisJobLive): the jobs
 *   queued or running whose key starts with @prefix, running first, at most
 *   %AXIS_JOB_LIVE_MAX
 * @error: (nullable): return location for an error
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_live_read(
	AxisDbConn   *conn,
	const gchar  *prefix,
	GPtrArray   **live,
	GError      **error
);

/* As axis_job_live_read(), of the jobs whose key is one of @keys
 * (%NULL-ended). */
gboolean
axis_job_live_read_keys(
	AxisDbConn          *conn,
	const gchar *const  *keys,
	GPtrArray          **live,
	GError             **error
);

/* As axis_job_live_read_keys(), on a connection of @pool. */
gboolean
axis_job_live_read_pool(
	AxisDbPool          *pool,
	const gchar *const  *keys,
	GPtrArray          **live,
	GError             **error
);

/* The job of @live whose key is @key, or %NULL.  Transfer none. */
const AxisJobLive *
axis_job_live_find(
	const GPtrArray *live,
	const gchar     *key
);

G_END_DECLS

#endif /* AXIS_JOB_LIVE_H */
