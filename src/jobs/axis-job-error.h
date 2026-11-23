/*
 * axis-job-error.h - error domain of the job queue
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Database failures keep their own domain (axis-db-error.h): a function
 * here that fails because the database did reports the AXIS_DB_ERROR it
 * got.  AXIS_JOB_ERROR is for what the queue itself decides.
 */

#ifndef AXIS_JOB_ERROR_H
#define AXIS_JOB_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_JOB_ERROR (axis_job_error_quark())

/**
 * AxisJobError:
 * @AXIS_JOB_ERROR_INVALID: an argument is malformed: a kind, queue or
 *   schedule name, a payload that is too large, an attempt count out of
 *   range, a handler or schedule table entry
 * @AXIS_JOB_ERROR_UNKNOWN_KIND: no handler is registered for the kind (the
 *   message names it)
 * @AXIS_JOB_ERROR_NOT_FOUND: no job (or schedule) has that identifier
 * @AXIS_JOB_ERROR_LEASE_LOST: the caller does not hold the job under that
 *   lease token any more: its lease expired and it was returned to the
 *   queue (and perhaps claimed by someone else), it was handed back, or it
 *   has finished.  Nothing was changed.
 * @AXIS_JOB_ERROR_PERMANENT: set by a handler: the job failed in a way
 *   that retrying cannot fix, so it is failed without further attempts
 * @AXIS_JOB_ERROR_CANCELLED: set by a handler: it stopped because
 *   axis_job_context_is_cancelled() said so
 * @AXIS_JOB_ERROR_CONFIG: the queue cannot start as configured (pool too
 *   small for the concurrency, a concurrency set for a queue no kind uses)
 * @AXIS_JOB_ERROR_STATE: the call is not allowed now (starting a queue
 *   twice, registering a handler after the registry is in use)
 * @AXIS_JOB_ERROR_FENCED: a fenced statement (AxisJobFence) found that the
 *   daemon's database lock is not held by the session it names; nothing
 *   was changed, and the caller's lease token may still be good
 */
typedef enum
{
	AXIS_JOB_ERROR_INVALID,
	AXIS_JOB_ERROR_UNKNOWN_KIND,
	AXIS_JOB_ERROR_NOT_FOUND,
	AXIS_JOB_ERROR_LEASE_LOST,
	AXIS_JOB_ERROR_PERMANENT,
	AXIS_JOB_ERROR_CANCELLED,
	AXIS_JOB_ERROR_CONFIG,
	AXIS_JOB_ERROR_STATE,
	AXIS_JOB_ERROR_FENCED
} AxisJobError;

GQuark
axis_job_error_quark(void);

G_END_DECLS

#endif /* AXIS_JOB_ERROR_H */
