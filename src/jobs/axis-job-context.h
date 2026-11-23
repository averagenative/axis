/*
 * axis-job-context.h - what a job handler is given
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The queue calls a handler (axis-job-registry.h) on one of its worker
 * threads with a context describing the job.  The handler contract:
 *
 *  - Be idempotent.  Execution is at least once: after a crash, a lost
 *    lease or a shutdown, the same job runs again, perhaps after part of
 *    it was done.  A handler that changes files must re-check the world
 *    before acting.
 *  - Validate the payload: the queue never interprets it.
 *  - Check axis_job_context_is_cancelled() (or use the cancellable) often
 *    enough to stop within a few seconds, and kill any subprocess it
 *    started when told to stop.
 *  - Use at most one pooled database connection at a time, and release it
 *    before returning.  The queue holds no connection while a handler
 *    runs, and requires the pool to have one connection per in-process
 *    worker plus its own (AXIS_JOB_QUEUE_RESERVED_CONNECTIONS), so a
 *    handler that keeps to this can always get one.
 *  - Report the outcome by the return value and error: %TRUE for success;
 *    %FALSE with an error for a failure, retried later while attempts
 *    remain; %AXIS_JOB_ERROR_PERMANENT for a failure retrying cannot fix;
 *    %AXIS_JOB_ERROR_CANCELLED (or %G_IO_ERROR_CANCELLED) after stopping
 *    because the context said so.
 *
 * Every function here is thread-safe, so a handler may pass its context
 * to helper threads (holding a reference with axis_job_context_ref()).
 */

#ifndef AXIS_JOB_CONTEXT_H
#define AXIS_JOB_CONTEXT_H

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "db/axis-db-pool.h"

G_BEGIN_DECLS

typedef struct _AxisJobContext AxisJobContext;

/**
 * AxisJobStopReason:
 * @AXIS_JOB_STOP_NONE: carry on
 * @AXIS_JOB_STOP_CANCELLED: a user (or API client) cancelled the job; it
 *   ends as cancelled whatever the handler reports, unless it succeeds
 * @AXIS_JOB_STOP_SHUTDOWN: the daemon is shutting down; unless the handler
 *   succeeds, the job is handed back to the queue without using an attempt
 * @AXIS_JOB_STOP_LEASE_LOST: the queue could no longer renew the job's
 *   lease and the job has been returned to the queue (it may already be
 *   running elsewhere); whatever the handler reports is discarded
 *
 * Why a handler should stop.
 */
typedef enum
{
	AXIS_JOB_STOP_NONE,
	AXIS_JOB_STOP_CANCELLED,
	AXIS_JOB_STOP_SHUTDOWN,
	AXIS_JOB_STOP_LEASE_LOST
} AxisJobStopReason;

/**
 * axis_job_context_ref:
 *
 * For a handler that passes its context to a helper thread: the context
 * stays valid until the matching axis_job_context_unref().  The queue
 * holds its own reference while the handler runs.
 *
 * Returns: (transfer full): @self
 */
AxisJobContext *
axis_job_context_ref(AxisJobContext *self);

void
axis_job_context_unref(AxisJobContext *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobContext, axis_job_context_unref)

/**
 * axis_job_context_get_id:
 *
 * Returns: the job's identifier
 */
gint64
axis_job_context_get_id(AxisJobContext *self);

/**
 * axis_job_context_get_kind:
 *
 * Returns: (transfer none): the job's kind
 */
const gchar *
axis_job_context_get_kind(AxisJobContext *self);

/**
 * axis_job_context_get_queue:
 *
 * Returns: (transfer none): the queue the job was claimed from
 */
const gchar *
axis_job_context_get_queue(AxisJobContext *self);

/**
 * axis_job_context_get_attempt:
 *
 * Returns: which attempt this run is: 1 for the first
 */
guint
axis_job_context_get_attempt(AxisJobContext *self);

/**
 * axis_job_context_get_max_attempts:
 *
 * Returns: how many attempts the job has in all; when
 *   axis_job_context_get_attempt() equals it, a failure is final
 */
guint
axis_job_context_get_max_attempts(AxisJobContext *self);

/**
 * axis_job_context_get_payload:
 *
 * Returns: (transfer none): the job's payload, owned by the context; do
 *   not modify it
 */
JsonObject *
axis_job_context_get_payload(AxisJobContext *self);

/**
 * axis_job_context_get_pool:
 *
 * Returns: (transfer none): the queue's database pool, for a handler that
 *   needs the database (see the contract above)
 */
AxisDbPool *
axis_job_context_get_pool(AxisJobContext *self);

/**
 * axis_job_context_is_cancelled:
 *
 * Returns: %TRUE once the handler should stop, for any reason (see
 *   axis_job_context_get_stop_reason()); it never becomes %FALSE again
 */
gboolean
axis_job_context_is_cancelled(AxisJobContext *self);

/**
 * axis_job_context_get_stop_reason:
 *
 * Returns: the first reason the handler was told to stop, or
 *   %AXIS_JOB_STOP_NONE
 */
AxisJobStopReason
axis_job_context_get_stop_reason(AxisJobContext *self);

/**
 * axis_job_context_get_cancellable:
 *
 * Returns: (transfer none): a cancellable that is cancelled when
 *   axis_job_context_is_cancelled() becomes %TRUE, for GIO calls and
 *   subprocess waits
 */
GCancellable *
axis_job_context_get_cancellable(AxisJobContext *self);

/**
 * axis_job_context_wait:
 * @self: an #AxisJobContext
 * @timeout_ms: how long to wait at most
 *
 * Sleeps until the handler is told to stop or @timeout_ms has passed: a
 * pause between steps that ends early on cancellation.
 *
 * Returns: the value of axis_job_context_is_cancelled() at the end
 */
gboolean
axis_job_context_wait(
	AxisJobContext *self,
	guint           timeout_ms
);

/**
 * axis_job_context_set_progress:
 * @self: an #AxisJobContext
 * @fraction: how much is done, 0 to 1 (clamped; NaN is taken as 0)
 * @message: (nullable): where the job is, in a few words (cut to
 *   %AXIS_JOB_MAX_PROGRESS_CHARS characters)
 *
 * Records progress.  This never blocks on the database: the queue writes
 * the latest value with the job's next heartbeat, at most once a second,
 * so calling it often is cheap.  Each write is also published as a
 * progress event.
 *
 * Returns: %FALSE when the handler should stop (as
 *   axis_job_context_is_cancelled())
 */
gboolean
axis_job_context_set_progress(
	AxisJobContext *self,
	gdouble         fraction,
	const gchar    *message
);

G_END_DECLS

#endif /* AXIS_JOB_CONTEXT_H */
