/*
 * axis-job-queue.h - runs jobs in-process
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An AxisJobQueue claims jobs of the kinds in its registry and runs their
 * handlers on worker threads, using the lifecycle functions of
 * axis-job-lifecycle.h like any other worker would.  Once started it has:
 *
 *  - per queue (the queue names of the registered kinds), as many worker
 *    threads as that queue's concurrency (default 1; 0 leaves the queue to
 *    external workers).  A worker claims one job at a time, so no more
 *    jobs of a queue run here at once than its concurrency;
 *  - a dispatcher thread holding one connection of its own (not from the
 *    pool) that LISTENs on %AXIS_JOB_CHANNEL: it wakes a queue's workers
 *    when a job becomes claimable, passes cancel requests to running
 *    handlers, and turns every notification into a "job-event" signal.
 *    The workers also poll every poll interval (5 s), so a lost
 *    notification only delays a job;
 *  - a keeper thread that renews the lease of every job running here (every
 *    third of the lease) and writes progress (at most once a second per
 *    job), and that hands jobs back at the end of a shutdown's grace;
 *  - a maintenance thread that sweeps expired leases (every sweep interval)
 *    and runs the schedule tick (every schedule interval).  The sweep spares
 *    jobs whose handlers are running in this process: their workers are
 *    alive, and a job taken from under a running handler could be claimed
 *    and run a second time before the first run noticed.  A lease of a job
 *    running here that ran out (heartbeats failing while the database was
 *    away) is renewed by the next heartbeat instead.
 *
 * Worker ids.  The workers are named "axisd:HOST:PID:INSTANCE/QUEUE/N"
 * (axis-job-lifecycle.h), INSTANCE random for each queue that starts
 * (axis_job_queue_set_instance() gives one).  So a daemon started again in
 * the same container (same host name, process 1) never has a worker of the
 * previous run's name, and the sweep, which spares only this queue's own
 * busy workers, takes back what the previous run left running once its
 * lease has run out, however busy the new workers are.
 *
 * The single-daemon lock (axisd).  A daemon that holds the database alone
 * gives the queue its lock: claims and lease renewals are then fenced
 * (axis_job_queue_set_lock(), AxisJobFence), so that from the moment the
 * server releases the lock the queue can take no job and extend no lease,
 * whatever it believes.  When the daemon finds its lock lost it suspends the
 * queue (axis_job_queue_suspend()): nothing is claimed, swept or scheduled,
 * and every running handler is told to stop as for a shutdown; it resumes
 * the queue once it holds the lock again.  With axis_job_queue_set_take_over()
 * the queue also takes back, a while after it starts, every job that an
 * earlier run of a daemon left running, without waiting for the leases.
 *
 * Kinds stored in the database that this registry has no handler for are
 * never claimed or swept here: start-up fails their queued jobs (so that
 * the keys they hold are free) and leaves their running jobs and their
 * schedules, logging a warning for each kind.
 * A kind it has is claimed from the queue the registry gives it, whatever
 * queue an earlier build stored its unfinished jobs on: start-up moves them.
 *
 * Pool size.  A worker thread holds at most one pooled connection at a
 * time, whether for its own claim and result or for its handler (which
 * may use one, see axis-job-context.h), and never holds one while waiting
 * for another; the keeper and the maintenance thread hold one each.  So
 * axis_job_queue_start() refuses to start unless the pool has at least
 * the total concurrency plus %AXIS_JOB_QUEUE_RESERVED_CONNECTIONS
 * connections: then the queue's own threads can always get a connection
 * in turn and nothing here can deadlock on the pool.  Connections beyond
 * that are what the rest of the daemon (HTTP requests, the health checks,
 * the worker API) shares: axisd, when no pool size is set, makes the pool
 * AXIS_CONFIG_POOL_HEADROOM larger than the queue needs, and refuses a pool
 * size set too small before it touches the database.
 *
 * Threads.  Create, configure, start and stop the queue on one thread
 * (the main thread, in axisd).  start() and stop() block: start() for the
 * start-up queries, stop() for the grace period.  Neither runs a database
 * call on the calling thread.  axis_job_queue_enqueue() and
 * axis_job_queue_cancel() block on the database and are for worker
 * threads; they may be called from any thread.
 *
 * Events.  "job-event" is emitted on the thread that runs the default
 * main context (g_main_context_default()), never on another, in the order
 * PostgreSQL delivered the notifications: commit order, so for any one job
 * the order of its changes.  Events are only as complete as the
 * dispatcher's connection: while it is down (it reconnects by itself)
 * changes are not announced, though jobs still run.  If the default main
 * context is not being run, at most 10 000 events wait for it; later ones
 * are dropped, with one warning.
 */

#ifndef AXIS_JOB_QUEUE_H
#define AXIS_JOB_QUEUE_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "db/axis-db-pool.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"

G_BEGIN_DECLS

#define AXIS_JOB_QUEUE_DEFAULT_CONCURRENCY          1
#define AXIS_JOB_QUEUE_MAX_CONCURRENCY              64
#define AXIS_JOB_QUEUE_DEFAULT_POLL_INTERVAL_MS     5000
#define AXIS_JOB_QUEUE_DEFAULT_SWEEP_INTERVAL_MS    10000
#define AXIS_JOB_QUEUE_DEFAULT_SCHEDULE_INTERVAL_MS 1000
#define AXIS_JOB_QUEUE_DEFAULT_GRACE_MS             30000

/* Progress is written at most this often per job. */
#define AXIS_JOB_QUEUE_PROGRESS_INTERVAL_MS         1000

/* Connections the queue's keeper and maintenance threads need, on top of
 * one per worker thread. */
#define AXIS_JOB_QUEUE_RESERVED_CONNECTIONS         2

/* After the grace period, how long stop() still waits for its own threads
 * to finish handing jobs back and recording results. */
#define AXIS_JOB_QUEUE_STOP_MARGIN_MS               5000

/**
 * AxisJobEventType:
 * @AXIS_JOB_EVENT_STATE: the job entered @state
 * @AXIS_JOB_EVENT_PROGRESS: a running job's progress was written
 * @AXIS_JOB_EVENT_CANCEL_REQUESTED: cancellation of a running job was
 *   requested
 */
typedef enum
{
	AXIS_JOB_EVENT_STATE,
	AXIS_JOB_EVENT_PROGRESS,
	AXIS_JOB_EVENT_CANCEL_REQUESTED
} AxisJobEventType;

/**
 * AxisJobEvent:
 * @type: what happened
 * @id: the job
 * @kind: its kind
 * @queue: its queue
 * @state: its state after the change
 * @attempts: its attempt count after the change
 * @max_attempts: its attempt limit
 * @runnable: for a queued job, whether it is claimable now (not waiting
 *   for a retry delay or a run time in the future)
 * @progress: its progress, 0 to 1, or -1 if none is recorded
 * @message: (nullable): the progress message (for %AXIS_JOB_EVENT_PROGRESS)
 *   or the error (for a job that failed or is queued for a retry)
 */
typedef struct
{
	AxisJobEventType  type;
	gint64            id;
	gchar            *kind;
	gchar            *queue;
	AxisJobState      state;
	guint             attempts;
	guint             max_attempts;
	gboolean          runnable;
	gdouble           progress;
	gchar            *message;
} AxisJobEvent;

#define AXIS_TYPE_JOB_EVENT (axis_job_event_get_type())

GType
axis_job_event_get_type(void) G_GNUC_CONST;

/**
 * axis_job_event_copy:
 *
 * For a signal handler that keeps an event beyond the emission.
 *
 * Returns: (transfer full): a deep copy of @event
 */
AxisJobEvent *
axis_job_event_copy(const AxisJobEvent *event);

void
axis_job_event_free(AxisJobEvent *event);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobEvent, axis_job_event_free)

#define AXIS_TYPE_JOB_QUEUE (axis_job_queue_get_type())

G_DECLARE_FINAL_TYPE(AxisJobQueue, axis_job_queue, AXIS, JOB_QUEUE, GObject)

/**
 * axis_job_queue_new:
 * @pool: the pool to use; it must outlive the queue's threads (see
 *   axis_job_queue_stop())
 * @registry: the kinds to run; referenced, and frozen when the queue starts
 *
 * The queue's signal:
 *
 *   "job-event" (AxisJobQueue *queue, AxisJobEvent *event, gpointer data):
 *   a job changed; @event is owned by the emission.
 *
 * Returns: (transfer full): a queue, not started
 */
AxisJobQueue *
axis_job_queue_new(
	AxisDbPool      *pool,
	AxisJobRegistry *registry
);

/**
 * axis_job_queue_set_concurrency:
 * @self: an #AxisJobQueue, not started
 * @queue: a queue name
 * @concurrency: 0 to %AXIS_JOB_QUEUE_MAX_CONCURRENCY worker threads
 *
 * How many of @queue's jobs run here at once; queues not set get
 * %AXIS_JOB_QUEUE_DEFAULT_CONCURRENCY.  With 0 the queue is served only by
 * external workers: its jobs are never run here, but its leases are still
 * swept and its schedules still enqueue.  Setting it for a queue no
 * registered kind uses makes axis_job_queue_start() fail.
 */
void
axis_job_queue_set_concurrency(
	AxisJobQueue *self,
	const gchar  *queue,
	guint         concurrency
);

/**
 * axis_job_queue_get_concurrency:
 *
 * Returns: the concurrency @queue has or will have
 */
guint
axis_job_queue_get_concurrency(
	AxisJobQueue *self,
	const gchar  *queue
);

/* Timing, for a queue not started; all in milliseconds and at least 1.
 * The defaults are the AXIS_JOB_QUEUE_DEFAULT_* and AXIS_JOB_DEFAULT_*
 * constants; tests shorten them. */

/* How often idle workers look for claimable jobs without being woken. */
void
axis_job_queue_set_poll_interval(
	AxisJobQueue *self,
	guint         interval_ms
);

/* The lease taken with each claim; it is renewed every third of it. */
void
axis_job_queue_set_lease(
	AxisJobQueue *self,
	guint         lease_ms
);

/* How often expired leases are swept. */
void
axis_job_queue_set_sweep_interval(
	AxisJobQueue *self,
	guint         interval_ms
);

/* How often the schedule tick runs. */
void
axis_job_queue_set_schedule_interval(
	AxisJobQueue *self,
	guint         interval_ms
);

/*
 * Makes every queue look for a job now rather than at its next poll: call
 * it after a module is switched on, so that the jobs held for it start at
 * once.  Main loop thread, while the queue is started (otherwise it does
 * nothing).
 */
void
axis_job_queue_wake(AxisJobQueue *self);

/* The retry delay (see axis_job_fail()). */
void
axis_job_queue_set_backoff(
	AxisJobQueue *self,
	guint         base_ms,
	guint         cap_ms
);

/**
 * axis_job_queue_set_instance:
 * @self: an #AxisJobQueue, not started
 * @instance: %AXIS_JOB_INSTANCE_LEN lower-case hexadecimal digits
 *
 * The INSTANCE part of the workers' ids (see above); without it the queue
 * makes a random one when it starts.  axisd passes the one its lock's
 * session is named with, so that the log and the job list name a run the
 * same way.
 */
void
axis_job_queue_set_instance(
	AxisJobQueue *self,
	const gchar  *instance
);

/**
 * axis_job_queue_get_daemon_id:
 *
 * Returns: (transfer none) (nullable): "axisd:HOST:PID:INSTANCE", the part
 *   of its workers' ids before the first '/', once the queue has started;
 *   %NULL before
 */
const gchar *
axis_job_queue_get_daemon_id(AxisJobQueue *self);

/**
 * axis_job_queue_set_lock:
 * @self: an #AxisJobQueue
 * @lock_key: the session advisory lock the daemon holds the database with
 * @holder_pid: the server process of the session that holds it
 * @holder_start: when that session started (AXIS_DB_BACKEND_START_SQL)
 *
 * Fences with the lock (AxisJobFence) every claim, lease renewal, sweep and
 * take-over of the queue: each takes or changes nothing unless that session
 * holds that lock at that moment.  A renewal refused so stops its handler
 * as for a shutdown (%AXIS_JOB_STOP_SHUTDOWN: the job is handed back), not
 * as for a lost lease.  Set it before the queue starts; afterwards only the
 * holder may change (the daemon took its lock again on a new connection),
 * and a different @lock_key is refused with a critical.  Callable from any
 * thread.
 */
void
axis_job_queue_set_lock(
	AxisJobQueue *self,
	gint64        lock_key,
	gint          holder_pid,
	const gchar  *holder_start
);

/**
 * axis_job_queue_get_fence:
 * @self: an #AxisJobQueue
 * @fence: (out): the queue's fence as it is now
 *
 * For whatever else takes work on the daemon's behalf (the worker API).
 * Callable from any thread.
 *
 * Returns: %TRUE if the queue has a fence (axis_job_queue_set_lock())
 */
gboolean
axis_job_queue_get_fence(
	AxisJobQueue *self,
	AxisJobFence *fence
);

/**
 * axis_job_queue_set_take_over:
 * @self: an #AxisJobQueue, not started
 * @delay_ms: how long after the start (at least 1)
 *
 * For a daemon that holds the database alone: @delay_ms after the queue
 * starts, its maintenance thread takes back every job of its kinds that an
 * in-process worker of another daemon run left running, whatever its
 * lease (axis_job_take_back_from_daemons()); remote workers' jobs, and
 * those of daemons that do not take the lock, keep theirs.  That is safe
 * only when every daemon that held the database before has stopped its
 * handlers by then, which is what @delay_ms must allow for (axisd: the
 * time a daemon takes to notice its lock is lost and stop its handlers).
 * A take-over that has not happened when the queue is suspended waits
 * @delay_ms again from the resume: another daemon may have held the
 * database meanwhile.
 */
void
axis_job_queue_set_take_over(
	AxisJobQueue *self,
	guint         delay_ms
);

/**
 * axis_job_queue_suspend:
 * @self: an #AxisJobQueue
 *
 * Stops claiming, sweeping, taking over and firing schedules, and tells
 * every handler running here to stop as for a shutdown
 * (%AXIS_JOB_STOP_SHUTDOWN: unless it succeeds, its job is handed back
 * without using an attempt).  Returns at once; leases of jobs still running
 * are renewed as before (through the fence, if one is set).  Does nothing on
 * a queue that is not running, is stopping, or is suspended already.
 * Callable from any thread.
 */
void
axis_job_queue_suspend(AxisJobQueue *self);

/**
 * axis_job_queue_wait_handlers:
 * @self: an #AxisJobQueue
 * @timeout_ms: how long to wait at most
 *
 * Waits until no handler is running here (each has returned, whether or
 * not its result is recorded yet).  Callable from any thread.
 *
 * Returns: %TRUE if none is running
 */
gboolean
axis_job_queue_wait_handlers(
	AxisJobQueue *self,
	guint         timeout_ms
);

/**
 * axis_job_queue_resume:
 *
 * Undoes axis_job_queue_suspend(): every queue looks for a job at once.
 * Callable from any thread.
 */
void
axis_job_queue_resume(AxisJobQueue *self);

/**
 * axis_job_queue_is_suspended:
 *
 * Returns: %TRUE between axis_job_queue_suspend() and
 *   axis_job_queue_resume()
 */
gboolean
axis_job_queue_is_suspended(AxisJobQueue *self);

/**
 * axis_job_queue_get_total_concurrency:
 *
 * Returns: the number of worker threads the queue has or will have
 */
guint
axis_job_queue_get_total_concurrency(AxisJobQueue *self);

/**
 * axis_job_queue_start:
 * @self: an #AxisJobQueue, not started
 * @error: (nullable): return location for an error
 *
 * Freezes the registry, opens the notification connection, makes the
 * registry's default schedules or brings them to what it declares, and
 * disables the build's schedules it no longer declares
 * (axis-job-schedule.h), moves queued and running jobs of its kinds that an
 * earlier build left on another queue to their kind's queue
 * (axis_job_move_to_kind_queues()), fails the queued jobs of kinds that have
 * no handler here (axis_job_fail_unknown_kinds()), warns (once per kind)
 * about the running jobs and enabled schedules of such kinds left as they are,
 * sweeps expired leases (a previous run that was killed leaves some), and
 * starts the threads.  Blocks until all that is done; the database calls
 * run on a thread of their own.
 *
 * Fails with %AXIS_JOB_ERROR_CONFIG when a concurrency is set for a queue
 * that no registered kind uses or the pool is too small (see above; the
 * message gives both numbers), with %AXIS_JOB_ERROR_STATE when the queue
 * has been started before, and with the database's error when the
 * start-up queries or the notification connection fail.  A queue that
 * failed to start has no threads and may be unreffed.
 *
 * Returns: %TRUE when running
 */
gboolean
axis_job_queue_start(
	AxisJobQueue  *self,
	GError       **error
);

/**
 * axis_job_queue_stop:
 * @self: an #AxisJobQueue
 * @grace_ms: how long running handlers get to finish
 *
 * Stops claiming, tells every running handler to stop
 * (%AXIS_JOB_STOP_SHUTDOWN) and waits up to @grace_ms for them.  A handler
 * that returns in time has its result recorded, except that any outcome
 * but success hands the job back (queued, attempt count as before it
 * started).  When the grace period ends, every job whose handler is still
 * running is handed back the same way, and its handler is abandoned: its
 * result, whenever it comes, is discarded and logged.  Leases stay renewed
 * until then.  Finally the dispatcher delivers the events of everything
 * committed before it stopped.  Does nothing on a queue that is not
 * running.  A stopped queue cannot be started again.
 *
 * stop() returns once every thread of the queue has finished, or, failing
 * that, AXIS_JOB_QUEUE_STOP_MARGIN_MS after the grace period (a thread
 * blocked on an unresponsive database, an abandoned handler).  It then
 * leaves those threads to finish on their own and returns %FALSE.  What
 * such a thread may still touch:
 *
 *  - an abandoned handler: whatever the handler itself touches (its
 *    module's user data, and the pool if it uses the database), and its
 *    job context, which it holds a reference to.  When it returns, the
 *    queue's code in that thread makes no database call and emits nothing:
 *    it records under the queue's internal lock (kept alive by the thread's
 *    own reference, independent of the AxisJobQueue object) that it has
 *    finished, logs one message and drops its references.
 *  - a thread of the queue blocked in a database call: the pool, until
 *    that call returns; then the same as above.
 *
 * So after %FALSE the caller must keep the pool (and anything the
 * handlers use) alive until those threads have finished
 * (axis_job_queue_wait_threads()) or the process exits; axisd exits
 * without freeing the pool.  The AxisJobQueue object itself may be
 * unreffed at once either way.
 *
 * Returns: %TRUE if every thread of the queue has finished
 */
gboolean
axis_job_queue_stop(
	AxisJobQueue *self,
	guint         grace_ms
);

/**
 * axis_job_queue_wait_threads:
 * @self: an #AxisJobQueue that has been stopped
 * @timeout_ms: how long to wait at most
 *
 * Waits for threads that axis_job_queue_stop() left running to finish.
 *
 * Returns: %TRUE if none is left
 */
gboolean
axis_job_queue_wait_threads(
	AxisJobQueue *self,
	guint         timeout_ms
);

/**
 * axis_job_queue_enqueue:
 * @self: an #AxisJobQueue
 * @kind: a registered kind
 * @payload: (nullable): the payload
 * @options: (nullable): %NULL for the defaults
 * @id: (out) (optional): the job's identifier
 * @created: (out) (optional): see axis_job_enqueue()
 * @error: (nullable): return location for an error
 *
 * As axis_job_registry_enqueue(), on a connection from the queue's pool.
 * Blocks; call it from a worker thread.  The queue need not be running.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_queue_enqueue(
	AxisJobQueue                 *self,
	const gchar                  *kind,
	JsonObject                   *payload,
	const AxisJobEnqueueOptions  *options,
	gint64                       *id,
	gboolean                     *created,
	GError                      **error
);

/**
 * axis_job_queue_cancel:
 * @self: an #AxisJobQueue
 * @id: the job
 * @outcome: (out) (optional): see axis_job_cancel()
 * @error: (nullable): return location for an error
 *
 * As axis_job_cancel(), on a connection from the queue's pool; when the
 * job is running here its handler is told at once rather than at the next
 * heartbeat.  Blocks; call it from a worker thread.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_queue_cancel(
	AxisJobQueue          *self,
	gint64                 id,
	AxisJobCancelOutcome  *outcome,
	GError               **error
);

/**
 * axis_job_queue_retry:
 * @self: an #AxisJobQueue
 * @id: a failed or cancelled job
 * @by: (nullable): who asked
 * @outcome: (out) (optional): see axis_job_retry()
 * @live_id: (out) (optional): see axis_job_retry()
 * @error: (nullable): return location for an error
 *
 * As axis_job_retry() with this registry's kinds and queues, on a
 * connection from the queue's pool: the job queued again as itself, or,
 * while another job of its de-duplication key is queued or running,
 * nothing.  It does not look at where a kind is retried instead
 * (axis_job_registry_get_retry_elsewhere()): the caller does.  Blocks; call
 * it from a worker thread.  The queue need not be running.
 *
 * Returns: %TRUE on success, whatever the outcome
 */
gboolean
axis_job_queue_retry(
	AxisJobQueue         *self,
	gint64                id,
	const gchar          *by,
	AxisJobRetryOutcome  *outcome,
	gint64               *live_id,
	GError              **error
);

G_END_DECLS

#endif /* AXIS_JOB_QUEUE_H */
