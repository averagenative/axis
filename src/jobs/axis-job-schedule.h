/*
 * axis-job-schedule.h - recurring jobs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A schedule (table job_schedule) enqueues a job of its kind every
 * interval.  Each tick (axis_job_schedule_tick(), run by the queue about
 * once a second) enqueues every enabled schedule whose next run time has
 * passed, then moves that time to now plus the interval: from now, not
 * from the time it was due, so a daemon that was down for many intervals
 * runs the schedule once on its return, not once per missed interval.
 * The job's de-duplication key is "schedule:" followed by the schedule's
 * name, so while one is queued or running the tick enqueues no other: a
 * schedule never overlaps itself.
 *
 * The build's schedules and a person's.  A module's default schedule
 * (AxisJobScheduleInfo) is the build's: at every start the queue makes it
 * or brings its kind, payload and interval to what the build declares
 * (axis_job_schedule_ensure()), and disables, without deleting, the build's
 * schedules that no module declares any more (axis_job_schedule_retire()).
 * A schedule a person made or changed (axis_job_schedule_upsert(),
 * axis_job_schedule_set_enabled()) is the person's from then on: no build
 * changes or disables it.  The column job_schedule.declared tells the two.
 *
 * All times are the database's, and a clock that goes back is expected: a
 * schedule whose next run lies more than its interval ahead is brought back
 * to one interval from now, with a warning, before it can stall.  The
 * functions here each run one statement on a connection the calling thread
 * holds, except the tick, which runs two.
 */

#ifndef AXIS_JOB_SCHEDULE_H
#define AXIS_JOB_SCHEDULE_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "db/axis-db-conn.h"
#include "jobs/axis-job-registry.h"

G_BEGIN_DECLS

#define AXIS_JOB_SCHEDULE_DEDUPE_PREFIX "schedule:"

/* The longest interval: a hundred years. */
#define AXIS_JOB_SCHEDULE_MAX_INTERVAL_SECONDS (G_GUINT64_CONSTANT(100) * 366 * 24 * 3600)

/**
 * axis_job_schedule_upsert:
 * @conn: a connection held by the calling thread
 * @name: the schedule (a valid job name)
 * @kind: the kind it enqueues (a valid job name; it need not be
 *   registered, but a schedule for a kind with no handler does nothing)
 * @payload: (nullable): the payload; %NULL is an empty object
 * @interval_seconds: 1 to %AXIS_JOB_SCHEDULE_MAX_INTERVAL_SECONDS
 * @enabled: whether it runs
 * @error: (nullable): return location for an error
 *
 * A person's schedule: creates it, due at once, or replaces its kind,
 * payload, interval and enabled flag.  A replaced schedule keeps its next
 * run time unless the new interval brings it closer.  Either way it is the
 * person's from then on: a build no longer changes or disables it.  Fails
 * with %AXIS_JOB_ERROR_INVALID when an argument is malformed, and as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_schedule_upsert(
	AxisDbConn   *conn,
	const gchar  *name,
	const gchar  *kind,
	JsonObject   *payload,
	guint64       interval_seconds,
	gboolean      enabled,
	GError      **error
);

/**
 * axis_job_schedule_ensure:
 * @conn: a connection held by the calling thread
 * @info: a default schedule
 * @created: (out) (optional): whether it was created now
 * @error: (nullable): return location for an error
 *
 * The build's schedule: creates it as @info describes it, enabled and due
 * at once; or, while the stored schedule of that name is still the
 * build's, gives it @info's kind, payload and interval (a shorter interval
 * brings its next run closer, a longer one leaves it), and enables it again
 * when axis_job_schedule_retire() had disabled it.  One disabled any other
 * way (by hand in the database) stays disabled, and the log says so.  A
 * person's schedule of that name is left as it is.  Logs what it created
 * (info) and what it changed (a message naming the old and the new).
 * Fails as axis_job_schedule_upsert().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_schedule_ensure(
	AxisDbConn                 *conn,
	const AxisJobScheduleInfo  *info,
	gboolean                   *created,
	GError                    **error
);

/**
 * axis_job_schedule_retire:
 * @conn: a connection held by the calling thread
 * @declared: (array zero-terminated=1): the names of every schedule the
 *   build declares
 * @n_retired: (out) (optional): how many were disabled now
 * @error: (nullable): return location for an error
 *
 * Disables every enabled schedule of the build's that is not in @declared,
 * marking it retired (job_schedule.retired_at) and logging each; none is
 * deleted, and axis_job_schedule_ensure() enables it again for a build that
 * declares it.  A person's schedules are left as they are.  Fails as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_schedule_retire(
	AxisDbConn          *conn,
	const gchar *const  *declared,
	guint               *n_retired,
	GError             **error
);

/**
 * axis_job_schedule_set_enabled:
 * @conn: a connection held by the calling thread
 * @name: the schedule
 * @enabled: whether it runs
 * @error: (nullable): return location for an error
 *
 * A person enables or disables a schedule, which is the person's from then
 * on (see above).  A disabled schedule enqueues nothing; one enabled again
 * after its run time passed runs at the next tick, once.  Fails with
 * %AXIS_JOB_ERROR_NOT_FOUND when there is no schedule @name, and as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_schedule_set_enabled(
	AxisDbConn   *conn,
	const gchar  *name,
	gboolean      enabled,
	GError      **error
);

/**
 * axis_job_schedule_tick:
 * @conn: a connection held by the calling thread
 * @registry: the kinds that have handlers; schedules for other kinds are
 *   skipped (left due, never advanced)
 * @n_enqueued: (out) (optional): how many jobs were enqueued
 * @error: (nullable): return location for an error
 *
 * First brings back to one interval from now every enabled schedule of a
 * registered kind whose next run lies more than its interval ahead (the
 * database's clock went back), with one warning each.  Then enqueues every
 * due, enabled schedule of a registered kind (on the kind's queue, with its
 * attempt count) unless the schedule's previous job is still queued or
 * running, and moves each due schedule's next run time to now plus its
 * interval.  Schedules another ticker is handling at the same moment are
 * skipped.  Fails as axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_schedule_tick(
	AxisDbConn       *conn,
	AxisJobRegistry  *registry,
	guint            *n_enqueued,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_JOB_SCHEDULE_H */
