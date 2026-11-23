/*
 * axis-web-jobs-data.h - what the Jobs pages read from the database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every function here blocks on the database through the pool, on the
 * calling thread, and so is for worker threads only.  The rows it returns
 * are plain data: the pages are built from them on the main loop by
 * web/axis-web-jobs-view.h, for the first load and for live updates alike.
 */

#ifndef AXIS_WEB_JOBS_DATA_H
#define AXIS_WEB_JOBS_DATA_H

#include <glib.h>

#include "db/axis-db-pool.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"

G_BEGIN_DECLS

/*
 * Every function here takes the job registry as @kinds (nullable): it says
 * what each kind is called in plain words and whether it is work or
 * upkeep.  Without it, and for a kind it does not know (one left in the
 * table by an older version), a job is named by its kind and is work.
 */

/* Jobs on one page of the list. */
#define AXIS_JOBS_PAGE_SIZE 50

/* Upkeep jobs shown under their heading on the first page of all jobs. */
#define AXIS_JOBS_BRIEF_SIZE 10

/* The job ids one live update looks up at most. */
#define AXIS_JOBS_MAX_BATCH 200

/**
 * AxisJobEarlierRun:
 * @state: how it ended: failed or cancelled
 * @attempts: the attempts it had used
 * @error: (nullable): its last error
 * @finished_us: when it ended, or 0
 * @retried_us: when a person retried it, or 0
 * @retried_by: (nullable): who
 *
 * A run of a job before a person retried it (job.earlier_runs).
 */
typedef struct
{
	AxisJobState  state;
	guint         attempts;
	gchar        *error;
	gint64        finished_us;
	gint64        retried_us;
	gchar        *retried_by;
} AxisJobEarlierRun;

/**
 * AxisJobRow:
 * @progress: 0 to 1, or -1 when none is recorded
 * @created_us: microseconds since 1970; the times that do not apply (a
 *   job that has not started has no @started_us) are 0
 * @payload: (nullable): the payload as JSON text; only filled by
 *   axis_web_jobs_get(), and cut to %AXIS_JOBS_PAYLOAD_SHOWN_CHARS
 * @label: (nullable): what the kind does, in plain words; %NULL when the
 *   registry gives none
 * @upkeep: the kind is upkeep, not work
 * @key: (nullable): its de-duplication key
 * @earlier: (nullable) (element-type AxisJobEarlierRun): its runs before
 *   each retry, oldest first; only filled by axis_web_jobs_get() (one job's
 *   page shows them, apart from its live-updated detail), else %NULL
 */
typedef struct
{
	gint64        id;
	gchar        *kind;
	gchar        *label;
	gboolean      upkeep;
	gchar        *queue;
	AxisJobState  state;
	guint         attempts;
	guint         max_attempts;
	gdouble       progress;
	gchar        *progress_message;
	gchar        *last_error;
	gboolean      cancel_requested;
	gint64        created_us;
	gint64        started_us;
	gint64        finished_us;
	gint64        run_at_us;
	gchar        *payload;
	gchar        *key;
	GPtrArray    *earlier;
} AxisJobRow;

#define AXIS_JOBS_PAYLOAD_SHOWN_CHARS 4000

void
axis_job_row_free(AxisJobRow *row);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobRow, axis_job_row_free)

/**
 * AxisJobFilter:
 * @state: (nullable): %NULL or "" for every state, "active" for queued
 *   and running, else one state name
 * @queue: (nullable): %NULL or "" for every queue, else a queue name
 * @cursor: (nullable): %NULL for the first page, else what the previous
 *   page gave as its "next" (see axis_web_jobs_cursor_parse())
 * @job_class: (nullable): %NULL or "" for work and upkeep, else "work" or
 *   "upkeep"
 * @limit: the most rows; 0 for %AXIS_JOBS_PAGE_SIZE
 *
 * The list is ordered with the unfinished jobs (queued, running) before
 * the finished ones, newest first in each group.
 */
typedef struct
{
	const gchar *state;
	const gchar *queue;
	const gchar *cursor;
	const gchar *job_class;
	guint        limit;
} AxisJobFilter;

/* Whether @state is one the filter accepts. */
gboolean
axis_web_jobs_filter_state_is_valid(const gchar *state);

/* Whether @job_class is one the filter accepts. */
gboolean
axis_web_jobs_filter_class_is_valid(const gchar *job_class);

/* Whether @cursor is well formed: "a" or "f" and then an identifier. */
gboolean
axis_web_jobs_cursor_is_valid(const gchar *cursor);

/**
 * axis_web_jobs_list:
 * @pool: the database
 * @filter: what to list
 * @rows: (out) (transfer container) (element-type AxisJobRow): at most
 *   %AXIS_JOBS_PAGE_SIZE rows
 * @next: (out) (transfer full) (nullable): the cursor of the next page,
 *   or %NULL when there is none
 * @queues: (out) (transfer full) (optional): every queue that has jobs
 * @error: (nullable): return location for an error
 *
 * Returns: %TRUE on success
 */
gboolean
axis_web_jobs_list(
	AxisDbPool          *pool,
	AxisJobRegistry     *kinds,
	const AxisJobFilter *filter,
	GPtrArray          **rows,
	gchar              **next,
	GStrv               *queues,
	GError             **error
);

/**
 * axis_web_jobs_get:
 * @whole_payload: %FALSE to cut the payload to
 *   %AXIS_JOBS_PAYLOAD_SHOWN_CHARS (to show it), %TRUE to read all of it
 *   (to copy it)
 * @row: (out) (transfer full) (nullable): the job with its payload, or
 *   %NULL when there is none
 *
 * Returns: %TRUE on success, whether or not the job exists
 */
gboolean
axis_web_jobs_get(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	gint64            id,
	gboolean      whole_payload,
	AxisJobRow  **row,
	GError      **error
);

/**
 * axis_web_jobs_get_many:
 * @ids: (element-type gint64): at most %AXIS_JOBS_MAX_BATCH jobs
 * @rows: (out) (transfer container) (element-type AxisJobRow): those that
 *   exist, without payloads
 * @running: (out) (optional): how many jobs of work are running now
 *
 * Returns: %TRUE on success
 */
gboolean
axis_web_jobs_get_many(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	GArray           *ids,
	GPtrArray       **rows,
	gint             *running,
	GError          **error
);

/**
 * AxisJobsActivity:
 * @running: jobs of work running now
 * @queued: jobs of work waiting for their turn
 * @searches, @imports, @scans, @conversions: of @running, the searches, the
 *   imports, the scans (of a disk, of what is inside files, of a library
 *   folder to identify) and the conversions
 * @downloads: downloads in progress at a client (not jobs)
 *
 * What the header shows: what Axis is doing now, as a person would name it.
 */
typedef struct
{
	gint running;
	gint queued;
	gint searches;
	gint imports;
	gint scans;
	gint conversions;
	gint downloads;
} AxisJobsActivity;

/* All of AxisJobsActivity, in one statement. */
gboolean
axis_web_jobs_count_activity(
	AxisDbPool        *pool,
	AxisJobRegistry   *kinds,
	AxisJobsActivity  *activity,
	GError           **error
);

/* How many jobs of work are running now: what the header shows. */
gboolean
axis_web_jobs_count_running(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	gint             *running,
	GError          **error
);

/*
 * How many jobs of work (never upkeep) are failed for good, and the highest
 * id among them, for the page "Needs attention": exactly the jobs the Jobs
 * page lists for state=failed&class=work (axis_web_jobs_list()), so that the
 * notice's number is the list's.  One statement over the partial index
 * job_failed_idx (migration 0077), every wait bounded by @timeout_ms.  Any
 * thread.
 */
gboolean
axis_web_jobs_count_failed(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	guint             timeout_ms,
	gint64           *count,
	gint64           *newest,
	GError          **error
);

/**
 * axis_web_jobs_count_active:
 * @job_class: "work" or "upkeep"
 * @running: (out): jobs of that class running now
 * @queued: (out): jobs of that class waiting for their turn
 *
 * Both in one statement, so they describe one moment.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_web_jobs_count_active(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	const gchar      *job_class,
	gint64           *running,
	gint64           *queued,
	GError          **error
);

G_END_DECLS

#endif /* AXIS_WEB_JOBS_DATA_H */
