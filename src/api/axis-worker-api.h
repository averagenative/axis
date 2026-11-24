/*
 * axis-worker-api.h - the worker API: register, claim, heartbeat, complete
 * and fail, for axis-worker processes on this machine or others
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The routes are the third class of the auth middleware
 * (web/axis-web-auth.h): a request reaches a handler here only with a
 * worker's valid token, as POST with a JSON body of at most
 * AXIS_WORKER_MAX_REQUEST_BYTES, and every body is read by the strict
 * parsers of api/axis-worker-proto.h.  Each request runs on a thread of
 * its own (the database), at most AXIS_WORKER_API_MAX_PENDING at once
 * (503 beyond).  What a worker says is a claim from an untrusted party:
 *
 *  - register: the worker's versions, capabilities and, per volume, the
 *    UUID it read and where; a volume mapping is accepted only for a UUID
 *    of a volume the daemon has.  The answer carries the lease and
 *    heartbeat timing.
 *  - claim: only remote kinds, only of modules that are on, only jobs the
 *    worker can do (an encoder it showed usable, 10 bits where needed,
 *    every volume among those it accepted and proved just now, the worker
 *    itself where a job is for one worker), at most as many at once as it
 *    said it runs.  The job is held under a new lease token for the worker.
 *  - heartbeat, complete, fail: only for a job the worker holds now, under
 *    the token it was given, with a lease that has not run out; anything
 *    else is 409 and changes nothing.  A cancelled job's heartbeat ends it
 *    and answers "stop".  A result is checked against the job's own plan
 *    and recorded in the same transaction that ends the job, while the job
 *    is locked.
 *
 * Leases.  A remote job's lease is counted twice, and the shorter wins:
 * by the database's clock (lease_expires_at, renewed by each heartbeat,
 * checked in every statement, and swept by the job queue), and by this
 * daemon's monotonic clock from the moment it handled the claim or the
 * heartbeat.  A lease whose monotonic time is up is handed back here
 * (axis_job_expire_remote()) at once, unless a request for it is being
 * handled; one of a worker the daemon has not heard from since it started
 * is adopted at its next heartbeat if the database still holds it.  A
 * worker counts its own deadline from before it sent the request, with a
 * margin, so it stops before either count ends (clients/worker).
 *
 * Main loop thread, except where said.
 */

#ifndef AXIS_WORKER_API_H
#define AXIS_WORKER_API_H

#include <glib.h>

#include "api/axis-worker-proto.h"
#include "api/axis-worker-tokens.h"
#include "core/axis-module.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "transcode/axis-estimator.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

#define AXIS_WORKER_API_DEFAULT_LEASE_MS 60000
#define AXIS_WORKER_API_DEFAULT_POLL_MS  5000
#define AXIS_WORKER_API_MAX_PENDING      64

/* The module whose switch worker.capabilities follows. */
#define AXIS_WORKERS_MODULE_ID           "workers"

typedef struct _AxisWorkerApi AxisWorkerApi;

/**
 * AxisWorkerApiTiming:
 * @lease_ms: a lease, from a claim or a heartbeat (default 60 s)
 * @heartbeat_ms: how often a worker is told to heartbeat (0: a quarter of
 *   the lease)
 * @poll_ms: how long a worker waits after a claim that found nothing
 * @reap_ms: how often expired leases are looked for (0: 250 ms)
 * @backoff_base_ms, @backoff_cap_ms: the retry delay after a worker's
 *   failure (0: the job queue's defaults)
 */
typedef struct
{
	guint lease_ms;
	guint heartbeat_ms;
	guint poll_ms;
	guint reap_ms;
	guint backoff_base_ms;
	guint backoff_cap_ms;
} AxisWorkerApiTiming;

/* Registers the remote kinds (worker.capabilities, estimate.sample.remote,
 * convert.encode, convert.verify) in @registry, following the modules'
 * switches when @modules is given. */
gboolean
axis_worker_api_register_kinds(
	AxisJobRegistry     *registry,
	AxisModuleRegistry  *modules,
	GError             **error
);

/**
 * axis_worker_api_new:
 * @pool: the database; must outlive the API
 * @registry: the job kinds (referenced)
 * @tokens: the workers' tokens (borrowed; must outlive the API)
 * @estimator: (nullable): for estimate.sample.remote (referenced); without
 *   it such a job is failed when claimed
 */
AxisWorkerApi *
axis_worker_api_new(
	AxisDbPool       *pool,
	AxisJobRegistry  *registry,
	AxisWorkerTokens *tokens,
	AxisEstimator    *estimator
);

/**
 * AxisWorkerConvertHooks:
 * @describe: at claim, for a convert.encode job the worker now holds: the
 *   work for this worker, or why not (@permanent: the job fails for good;
 *   else it is tried again later).  FALSE only for a database failure (the
 *   job is then handed back).
 * @encoded: at complete, inside the transaction that ends the job under
 *   its lease: record the result
 * @describe_verify: at claim, for a convert.verify job: what to measure, or
 *   why not, as @describe
 * @verified: at complete of a convert.verify job, inside the transaction
 *   that ends it: keep the measurements; FALSE with
 *   AXIS_CONVERSION_ERROR_REFUSED for measurements that do not fit the job
 *   (the report is refused as malformed, nothing is recorded)
 *
 * What the conversions (transcode/axis-conversion.h) do for their kinds.
 */
typedef struct
{
	gboolean (*describe)(gpointer data, AxisDbConn *conn, const AxisJobClaim *claim, const gchar *worker,
	                     const gchar *machine, AxisConvertWork **work, gchar **why_not, gboolean *permanent,
	                     GError **error);
	gboolean (*encoded)(gpointer data, AxisDbConn *conn, gint64 job, const AxisConvertResult *result,
	                    const gchar *worker, GError **error);
	gboolean (*describe_verify)(gpointer data, AxisDbConn *conn, const AxisJobClaim *claim, const gchar *worker,
	                            AxisVerifyWork **work, gchar **why_not, gboolean *permanent, GError **error);
	gboolean (*verified)(gpointer data, AxisDbConn *conn, gint64 job, const AxisVerifyResult *result,
	                     const gchar *worker, GError **error);
} AxisWorkerConvertHooks;

/* Before start.  Without hooks a convert.encode job that is claimed fails
 * for good. */
void
axis_worker_api_set_convert(
	AxisWorkerApi                *self,
	const AxisWorkerConvertHooks *hooks,
	gpointer                      data
);

/**
 * AxisWorkerRegisteredFunc:
 * @worker: the name of the worker whose registration was just accepted
 *
 * Called on the main loop, after the registration is stored and before its
 * answer is sent.
 */
typedef void (*AxisWorkerRegisteredFunc)(
	const gchar *worker,
	gpointer     user_data
);

/* Before start.  @func (nullable) hears of every accepted registration, so
 * that a page waiting for a worker can look again. */
void
axis_worker_api_set_registered(
	AxisWorkerApi            *self,
	AxisWorkerRegisteredFunc  func,
	gpointer                  user_data
);

/* Before start. */
void
axis_worker_api_set_timing(
	AxisWorkerApi             *self,
	const AxisWorkerApiTiming *timing
);

void
axis_worker_api_get_timing(
	AxisWorkerApi       *self,
	AxisWorkerApiTiming *timing
);

/* The five routes, AXIS_WEB_ROUTE_WORKER. */
void
axis_worker_api_register_routes(
	AxisWorkerApi *self,
	AxisWeb       *web
);

/* Starts handing back leases whose time is up. */
void
axis_worker_api_start(AxisWorkerApi *self);

/* Refuses new requests (503) and waits for those being handled.  Runs the
 * default main context while it waits. */
void
axis_worker_api_stop(AxisWorkerApi *self);

/* The claims and the lease expiries of the API are fenced by the daemon's
 * lock, as @jobs has it (axis_job_queue_get_fence()): a daemon that lost
 * its lock hands out nothing and takes nothing back.  Renewals and reports
 * are not fenced: they change only the job a worker holds, under its lease
 * token and the database's own lease clock, so they are safe whichever
 * daemon passes them on, and a renewal refused would make the worker throw
 * away its work over a lock that may be held again in a moment
 * (openspec/specs/daemon-core/spec.md).  Main loop thread,
 * before serving. */
void
axis_worker_api_set_fence(
	AxisWorkerApi *self,
	AxisJobQueue  *jobs
);

/* While @paused, no lease is handed back for its time being up: axisd pauses
 * the API while it does not hold its database lock (its gate then refuses
 * the workers' requests, so their silence says nothing).  Unpaused, every
 * lease held in memory runs at least one full lease from now.  Main loop
 * thread. */
void
axis_worker_api_pause(
	AxisWorkerApi *self,
	gboolean       paused
);

void
axis_worker_api_free(AxisWorkerApi *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerApi, axis_worker_api_free)

/* Leases held in memory now (tests).  Any thread. */
guint
axis_worker_api_count_leases(AxisWorkerApi *self);

G_END_DECLS

#endif /* AXIS_WORKER_API_H */
