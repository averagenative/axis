/*
 * axis-job-lifecycle.h - the job lifecycle, one conditional statement each
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These are the only functions that change a job.  The in-process runner
 * (axis-job-queue.h) uses them, and so will the HTTP API through which
 * axis-worker processes claim jobs; nothing here holds state, starts a
 * thread or looks at the daemon's clock.  Each call is one SQL statement
 * on a connection the calling thread holds (axis-db-pool.h), so it is
 * atomic on its own.  Do not run them inside a long transaction: lease
 * and run times use now(), the start of the transaction.
 *
 * States and the only transitions there are:
 *
 *   queued  -> running    axis_job_claim()
 *   running -> succeeded  axis_job_succeed()
 *   running -> failed     axis_job_fail(), attempts used up or permanent
 *   running -> queued     axis_job_fail() with attempts left (a retry),
 *                         axis_job_hand_back(), axis_job_sweep_expired_leases()
 *   queued  -> cancelled  axis_job_cancel()
 *   queued  -> failed     axis_job_fail_unknown_kinds() (a kind no longer built)
 *   failed, cancelled -> queued   axis_job_retry() (a person's retry)
 *   running -> cancelled  axis_job_fail(), axis_job_hand_back() or
 *                         axis_job_sweep_expired_leases(), once a cancel has
 *                         been requested (axis_job_cancel() on a running
 *                         job); axis_job_succeed() still succeeds
 *
 * A running job belongs to whoever holds its lease token.  Every call that
 * takes a token changes the job only if the job is running under exactly
 * that token; otherwise it fails with %AXIS_JOB_ERROR_LEASE_LOST and
 * changes nothing, so a worker whose lease expired (and whose job may now
 * be someone else's) can never complete, fail, extend or hand back the job.
 * A lease is only taken away by axis_job_sweep_expired_leases(); a holder
 * whose lease has expired but not yet been swept still holds it.
 *
 * Every change is announced with NOTIFY on %AXIS_JOB_CHANNEL from inside
 * the same statement, so it is delivered only when (and if) the change
 * commits, and in commit order.  The payload is a JSON object:
 *
 *   {"type": "state" | "progress" | "cancel-requested", "id": 42,
 *    "kind": "...", "queue": "...", "state": "queued", "attempts": 1,
 *    "max_attempts": 5, "runnable": true, "progress": 0.5,
 *    "message": "..."}
 *
 * where "runnable" says a queued job may be claimed now, and "message" is
 * the progress message for "progress" and the error text for a job that
 * failed or is queued again after a failure.
 */

#ifndef AXIS_JOB_LIFECYCLE_H
#define AXIS_JOB_LIFECYCLE_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "db/axis-db-conn.h"

G_BEGIN_DECLS

#define AXIS_JOB_CHANNEL                   "axis_job"

/* Kind, queue and schedule names: lower-case ASCII letters, digits, '.',
 * '_' and '-', starting with a letter. */
#define AXIS_JOB_NAME_MAX_LEN              63

#define AXIS_JOB_DEFAULT_MAX_ATTEMPTS      5
#define AXIS_JOB_MAX_ATTEMPTS_LIMIT        100
#define AXIS_JOB_MAX_PAYLOAD_BYTES         (1024 * 1024)
#define AXIS_JOB_MAX_DEDUPE_KEY_LEN        200
#define AXIS_JOB_MAX_WORKER_ID_LEN         200

/* Error texts are kept up to this many characters, progress messages up
 * to the second; longer ones are cut. */
#define AXIS_JOB_MAX_ERROR_CHARS           2000
#define AXIS_JOB_MAX_PROGRESS_CHARS        200

#define AXIS_JOB_DEFAULT_LEASE_MS          60000
#define AXIS_JOB_DEFAULT_BACKOFF_BASE_MS   30000
#define AXIS_JOB_DEFAULT_BACKOFF_CAP_MS    3600000

#define AXIS_JOB_DEFAULT_KEEP_FINISHED_MS  (G_GINT64_CONSTANT(7) * 24 * 3600 * 1000)
#define AXIS_JOB_DEFAULT_KEEP_FAILED_MS    (G_GINT64_CONSTANT(30) * 24 * 3600 * 1000)

typedef enum
{
	AXIS_JOB_STATE_QUEUED,
	AXIS_JOB_STATE_RUNNING,
	AXIS_JOB_STATE_SUCCEEDED,
	AXIS_JOB_STATE_FAILED,
	AXIS_JOB_STATE_CANCELLED
} AxisJobState;

/**
 * axis_job_state_to_string:
 *
 * Returns: (transfer none): the name the database uses ("queued", ...)
 */
const gchar *
axis_job_state_to_string(AxisJobState state);

/**
 * axis_job_state_from_string:
 * @text: (nullable): a state name as the database spells it
 * @state: (out): the state; untouched on failure
 *
 * Returns: %TRUE if @text names a state
 */
gboolean
axis_job_state_from_string(
	const gchar  *text,
	AxisJobState *state
);

/**
 * axis_job_name_is_valid:
 * @name: (nullable): a kind, queue or schedule name
 *
 * Returns: %TRUE if @name is 1 to %AXIS_JOB_NAME_MAX_LEN lower-case ASCII
 *   letters, digits, '.', '_' and '-', starting with a letter
 */
gboolean
axis_job_name_is_valid(const gchar *name);

/**
 * AxisJobSpec:
 * @kind: the kind (a valid name)
 * @queue: the queue it is claimed from (a valid name)
 * @payload: (nullable): the payload; %NULL is an empty object
 * @priority: higher is claimed first within the queue
 * @delay_ms: not claimable until this long after now, by the database
 *   clock; 0 (or less) is now
 * @dedupe_key: (nullable): at most one queued or running job has a given
 *   key; keys are global across kinds, so prefix them with the kind
 * @max_attempts: 1 to %AXIS_JOB_MAX_ATTEMPTS_LIMIT
 */
typedef struct
{
	const gchar *kind;
	const gchar *queue;
	JsonObject  *payload;
	gint         priority;
	gint64       delay_ms;
	const gchar *dedupe_key;
	guint        max_attempts;
} AxisJobSpec;

/**
 * axis_job_enqueue:
 * @conn: a connection held by the calling thread
 * @spec: the job
 * @id: (out) (optional): the job's identifier
 * @created: (out) (optional): %FALSE when @spec's de-duplication key
 *   belonged to a queued or running job, whose identifier @id then is
 * @error: (nullable): return location for an error
 *
 * Stores a queued job, or finds the unfinished job that already has the
 * de-duplication key; two callers enqueuing the same key at once both get
 * the one job's identifier.  This does not check that a handler exists
 * for the kind: use axis_job_registry_enqueue() or
 * axis_job_queue_enqueue(), which do.
 *
 * Fails with %AXIS_JOB_ERROR_INVALID, storing nothing, when a name is
 * malformed, the payload's JSON is larger than %AXIS_JOB_MAX_PAYLOAD_BYTES,
 * the key is empty or longer than %AXIS_JOB_MAX_DEDUPE_KEY_LEN bytes, or
 * @spec->max_attempts is out of range; and as axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_enqueue(
	AxisDbConn         *conn,
	const AxisJobSpec  *spec,
	gint64             *id,
	gboolean           *created,
	GError            **error
);

/**
 * AxisJobClaim:
 * @id: the job
 * @kind: its kind
 * @queue: its queue
 * @payload: its payload, a JSON object as text
 * @priority: its priority
 * @attempt: which attempt this is: 1 for the first run
 * @max_attempts: how many attempts it has in all
 * @lease_token: the token every later call for this run must pass
 * @cancel_requested: always %FALSE for a fresh claim (a queued job is
 *   cancelled outright); kept for symmetry with the heartbeat
 *
 * A job claimed with axis_job_claim().
 */
typedef struct
{
	gint64    id;
	gchar    *kind;
	gchar    *queue;
	gchar    *payload;
	gint      priority;
	guint     attempt;
	guint     max_attempts;
	gchar    *lease_token;
	gboolean  cancel_requested;
} AxisJobClaim;

void
axis_job_claim_free(AxisJobClaim *claim);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobClaim, axis_job_claim_free)

/**
 * axis_job_claim:
 * @conn: a connection held by the calling thread
 * @queue: the queue to claim from
 * @kinds: (array zero-terminated=1): the kinds the caller can run; a job
 *   of any other kind is never claimed
 * @worker_id: who is claiming, recorded with the job (at most
 *   %AXIS_JOB_MAX_WORKER_ID_LEN bytes)
 * @lease_ms: the lease length; the claimer must heartbeat before it ends
 * @claim: (out) (transfer full) (nullable): the job, or %NULL when no
 *   job is claimable now
 * @error: (nullable): return location for an error
 *
 * Takes the next claimable job of @queue: queued, its run time reached,
 * of one of @kinds; the one with the highest priority, then the earliest
 * run time, then the lowest identifier.  In the same statement it becomes
 * running with a new random lease token and a lease that ends @lease_ms
 * from now, its attempt count goes up by one, and its start time is set.
 * Rows other claimers have locked are skipped (FOR UPDATE SKIP LOCKED), so
 * any number of claimers can run at once and each job goes to exactly one
 * of them.
 *
 * Fails with %AXIS_JOB_ERROR_INVALID when an argument is malformed, and
 * as axis_db_conn_exec().
 *
 * Returns: %TRUE on success, whether or not a job was claimed
 */
gboolean
axis_job_claim(
	AxisDbConn          *conn,
	const gchar         *queue,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	AxisJobClaim       **claim,
	GError             **error
);

/**
 * axis_job_claim_any:
 * @queues: (array zero-terminated=1): queues, in the order to try them
 *
 * As axis_job_claim(), trying each queue in turn until one yields a job.
 * Claim order (priority, run time) applies within a queue, not across
 * queues.
 *
 * Returns: %TRUE on success, whether or not a job was claimed
 */
gboolean
axis_job_claim_any(
	AxisDbConn          *conn,
	const gchar *const  *queues,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	AxisJobClaim       **claim,
	GError             **error
);

/* Room for a session's start as AXIS_DB_BACKEND_START_SQL gives it. */
#define AXIS_JOB_FENCE_START_MAX 32

/**
 * AxisJobFence:
 * @lock_key: a session advisory lock (pg_advisory_lock(bigint))
 * @holder_pid: the server process (pg_backend_pid()) of the session that
 *   must hold it
 * @holder_start: when that session started (pg_stat_activity.backend_start,
 *   as AXIS_DB_BACKEND_START_SQL in db/axis-db-lock.h gives it): process ids
 *   repeat after a server restart, the pair does not
 *
 * A condition that a fenced statement checks in the database, in the same
 * statement as its change: it changes nothing unless that session holds the
 * advisory lock @lock_key in this database at that moment.  axisd fences
 * with its single-daemon lock (db/axis-db-lock.h) every statement by which
 * it takes work or takes it back: its in-process workers' claims and lease
 * renewals, its sweeps and take-overs, and the worker API's claims and
 * expiries.  From the moment the server releases that lock, whatever the
 * daemon believes, it can neither take a new job, nor extend the lease of
 * one it runs, nor take a job back from anyone.
 */
typedef struct
{
	gint64 lock_key;
	gint   holder_pid;
	gchar  holder_start[AXIS_JOB_FENCE_START_MAX];
} AxisJobFence;

/**
 * axis_job_claim_fenced:
 * @fence: (nullable): the fence, or %NULL for none (as axis_job_claim())
 *
 * As axis_job_claim(), claiming nothing (and succeeding) when @fence does
 * not hold.
 */
gboolean
axis_job_claim_fenced(
	AxisDbConn          *conn,
	const gchar         *queue,
	const gchar *const  *kinds,
	const gchar         *worker_id,
	guint                lease_ms,
	const AxisJobFence  *fence,
	AxisJobClaim       **claim,
	GError             **error
);

/**
 * AxisJobProgress:
 * @fraction: 0 to 1
 * @message: (nullable): a short description of where the job is
 */
typedef struct
{
	gdouble      fraction;
	const gchar *message;
} AxisJobProgress;

/**
 * axis_job_heartbeat:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @lease_token: the token from the claim
 * @lease_ms: the lease now ends this long from now
 * @progress: (nullable): progress to record with it, or %NULL to leave the
 *   recorded progress as it is
 * @cancel_requested: (out) (optional): whether cancellation of the job has
 *   been requested; the holder should stop and report a failure (which
 *   then ends the job as cancelled)
 * @error: (nullable): return location for an error
 *
 * Extends the lease, and records @progress if given.  The fraction is
 * clamped to 0..1 and the message cut to %AXIS_JOB_MAX_PROGRESS_CHARS.
 * Fails with %AXIS_JOB_ERROR_LEASE_LOST, changing nothing, when the job is
 * not running under @lease_token, and as axis_db_conn_exec().
 *
 * Returns: %TRUE if the caller still holds the job
 */
gboolean
axis_job_heartbeat(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	gboolean               *cancel_requested,
	GError                **error
);

/**
 * axis_job_heartbeat_fenced:
 * @fence: (nullable): the fence, or %NULL for none (as axis_job_heartbeat())
 *
 * As axis_job_heartbeat(); when @fence does not hold, nothing is renewed or
 * recorded and it fails with %AXIS_JOB_ERROR_FENCED (not
 * %AXIS_JOB_ERROR_LEASE_LOST: the lease token may still be good, so the
 * holder should stop and hand the job back).
 */
gboolean
axis_job_heartbeat_fenced(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	const AxisJobFence     *fence,
	gboolean               *cancel_requested,
	GError                **error
);

/**
 * axis_job_succeed:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @lease_token: the token from the claim
 * @error: (nullable): return location for an error
 *
 * Marks the job succeeded and ends the lease.  A cancel requested while it
 * ran does not change that: the work was done.  Fails with
 * %AXIS_JOB_ERROR_LEASE_LOST, changing nothing, when the job is not
 * running under @lease_token, and as axis_db_conn_exec().
 *
 * Returns: %TRUE if the job is now succeeded
 */
gboolean
axis_job_succeed(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *lease_token,
	GError      **error
);

/**
 * axis_job_fail:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @lease_token: the token from the claim
 * @message: (nullable): what went wrong, kept as the job's last error (cut
 *   to %AXIS_JOB_MAX_ERROR_CHARS; invalid UTF-8 is replaced)
 * @permanent: retrying cannot help: fail the job now
 * @backoff_base_ms: the delay before the first retry
 * @backoff_cap_ms: no delay is longer than this
 * @state: (out) (optional): what the job became
 * @error: (nullable): return location for an error
 *
 * Ends the lease and records @message.  The job becomes:
 *
 *  - cancelled, if a cancel was requested while it ran;
 *  - failed, if @permanent or this was its last attempt;
 *  - queued again otherwise, claimable after a delay of
 *    min(@backoff_cap_ms, @backoff_base_ms * 2^(attempt - 1)), scaled by a
 *    random factor between 0.5 and 1 so that jobs failing together do not
 *    retry together.
 *
 * Fails with %AXIS_JOB_ERROR_LEASE_LOST, changing nothing, when the job is
 * not running under @lease_token, and as axis_db_conn_exec().
 *
 * Returns: %TRUE if the failure was recorded
 */
gboolean
axis_job_fail(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	const gchar   *message,
	gboolean       permanent,
	guint          backoff_base_ms,
	guint          backoff_cap_ms,
	AxisJobState  *state,
	GError       **error
);

/**
 * axis_job_hand_back:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @lease_token: the token from the claim
 * @state: (out) (optional): what the job became
 * @error: (nullable): return location for an error
 *
 * Gives a running job back without it counting as an attempt (shutdown):
 * it is queued again with its attempt count as it was before the claim,
 * and keeps its place in the claim order.  If a cancel had been requested,
 * it becomes cancelled instead.  Fails with %AXIS_JOB_ERROR_LEASE_LOST,
 * changing nothing, when the job is not running under @lease_token, and
 * as axis_db_conn_exec().
 *
 * Returns: %TRUE if the job was handed back
 */
gboolean
axis_job_hand_back(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	AxisJobState  *state,
	GError       **error
);

/**
 * AxisJobCancelOutcome:
 * @AXIS_JOB_CANCEL_DONE: the job was queued and is now cancelled; it will
 *   never run
 * @AXIS_JOB_CANCEL_REQUESTED: the job is running; its holder has been asked
 *   to stop (through the heartbeat, and in-process through the handler's
 *   context) and the job ends as cancelled when it does
 * @AXIS_JOB_CANCEL_TOO_LATE: the job had already succeeded, failed or been
 *   cancelled; nothing changed
 */
typedef enum
{
	AXIS_JOB_CANCEL_DONE,
	AXIS_JOB_CANCEL_REQUESTED,
	AXIS_JOB_CANCEL_TOO_LATE
} AxisJobCancelOutcome;

/**
 * axis_job_cancel:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @outcome: (out) (optional): what happened
 * @error: (nullable): return location for an error
 *
 * Cancels a queued job, or requests cancellation of a running one.  Fails
 * with %AXIS_JOB_ERROR_NOT_FOUND when there is no job @id, and as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_cancel(
	AxisDbConn            *conn,
	gint64                 id,
	AxisJobCancelOutcome  *outcome,
	GError               **error
);

/* How many earlier runs a job retried by a person keeps (job.earlier_runs),
 * and how much of each one's error (already cleaned as every job error is,
 * when it was written).  axis_job_prune() drops an earlier run that ended
 * longer ago than failed jobs are kept. */
#define AXIS_JOB_EARLIER_RUNS_KEPT   20
#define AXIS_JOB_EARLIER_ERROR_CHARS 500

/**
 * AxisJobRetryOutcome:
 * @AXIS_JOB_RETRY_DONE: the job is queued again, as itself
 * @AXIS_JOB_RETRY_LIVE: another job of its de-duplication key is queued or
 *   running (its identifier is given): that one is this work, and nothing
 *   changed.  The identifier is the job's own when someone else retried it
 *   at the same moment, and 0 when another job of its key did the work and
 *   ended while the retry was being made
 * @AXIS_JOB_RETRY_NOT_ENDED: the job is not failed or cancelled (it is
 *   queued, running or succeeded); nothing changed
 */
typedef enum
{
	AXIS_JOB_RETRY_DONE,
	AXIS_JOB_RETRY_LIVE,
	AXIS_JOB_RETRY_NOT_ENDED
} AxisJobRetryOutcome;

/**
 * axis_job_retry:
 * @conn: a connection held by the calling thread
 * @id: the job
 * @kinds: (array zero-terminated=1): the kinds this daemon has handlers for
 * @queues: (array zero-terminated=1): each kind's queue, in the same order
 * @by: (nullable): who asked, kept with the earlier run
 * @outcome: (out) (optional): what happened
 * @live_id: (out) (optional): for %AXIS_JOB_RETRY_LIVE, the live job; else 0
 * @error: (nullable): return location for an error
 *
 * Retries a failed or cancelled job as the same piece of work: the same
 * job (its identifier, payload, de-duplication key, priority and attempt
 * limit) is queued again, claimable now, on the queue its kind is claimed
 * from, with its attempt count from none and no cancel request, progress
 * or error.  How its last run ended (state, attempts, error, worker and
 * times, and @by) is added to its earlier runs, the latest
 * %AXIS_JOB_EARLIER_RUNS_KEPT kept.  Nothing changes while another job of
 * its key is queued or running.  Fails with %AXIS_JOB_ERROR_NOT_FOUND when
 * there is no job @id, with %AXIS_JOB_ERROR_UNKNOWN_KIND when a failed or
 * cancelled job's kind is not in @kinds, and as axis_db_conn_exec().
 *
 * Returns: %TRUE on success, whatever the outcome
 */
gboolean
axis_job_retry(
	AxisDbConn           *conn,
	gint64                id,
	const gchar *const   *kinds,
	const gchar *const   *queues,
	const gchar          *by,
	AxisJobRetryOutcome  *outcome,
	gint64               *live_id,
	GError              **error
);

/**
 * axis_job_move_to_kind_queues:
 * @conn: a connection held by the calling thread
 * @kinds: (array zero-terminated=1): the kinds this daemon has handlers for
 * @queues: (array zero-terminated=1): each kind's queue, in the same order
 * @n_moved: (out) (optional): how many jobs were moved
 * @error: (nullable): return location for an error
 *
 * Moves every queued or running job of one of @kinds that is stored on
 * another queue than its kind's (a build that claims the kind from another
 * queue than the build that queued it) to its kind's queue, logging one
 * message per kind and old queue.  Its state, lease and place in the claim
 * order are as they were; finished jobs keep the queue they ran on.  Fails
 * as axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_move_to_kind_queues(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *queues,
	guint               *n_moved,
	GError             **error
);

/**
 * axis_job_fail_unknown_kinds:
 * @conn: a connection held by the calling thread
 * @kinds: (array zero-terminated=1): the kinds this daemon has handlers for
 * @n_failed: (out) (optional): how many jobs were failed
 * @error: (nullable): return location for an error
 *
 * Fails for good every queued job of a kind not in @kinds (a kind an
 * earlier build had), with an error saying why, so that the
 * de-duplication key it held is free: a schedule whose kind changed would
 * otherwise never fire again behind its old job.  The jobs are kept, and a
 * build that has the kind can retry them.  Running jobs of such kinds are
 * left to their leases.  Logs one warning per kind.  Fails as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_fail_unknown_kinds(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	guint               *n_failed,
	GError             **error
);

/**
 * axis_job_sweep_expired_leases:
 * @conn: a connection held by the calling thread
 * @kinds: (array zero-terminated=1): the kinds this daemon has handlers for
 * @except_workers: (array zero-terminated=1) (nullable): worker ids whose
 *   jobs to leave alone however late their lease: workers the caller knows
 *   are alive and still running what they claimed (its own busy threads)
 * @n_swept: (out) (optional): how many jobs were taken back
 * @error: (nullable): return location for an error
 *
 * Takes back every running job of one of @kinds whose lease has expired:
 * it is queued again at once (the run that lost its lease counted as an
 * attempt), or failed if that was its last attempt, or cancelled if a
 * cancel had been requested.  Its last error says the lease expired and
 * names the worker.  Jobs of other kinds, and of @except_workers, are left
 * alone.  Each job taken back is logged as a warning.  Fails as
 * axis_db_conn_exec().
 *
 * A job taken back from a worker that is in fact still running it may be
 * claimed and run by another before the first notices (at its next
 * heartbeat) that it has lost the job.  That is why a worker should never
 * stop renewing a lease it still needs, and why the queue passes its own
 * busy workers in @except_workers: a job running in this process is never
 * taken from under it here, even if its lease ran out while the database
 * was unreachable; its next heartbeat renews it instead.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_sweep_expired_leases(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *except_workers,
	guint               *n_swept,
	GError             **error
);

/**
 * axis_job_sweep_expired_leases_fenced:
 * @fence: (nullable): the fence, or %NULL for none
 *
 * As axis_job_sweep_expired_leases(), taking nothing back when @fence does
 * not hold: a daemon that lost its lock does not know which jobs the daemon
 * that holds it spares.
 */
gboolean
axis_job_sweep_expired_leases_fenced(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar *const  *except_workers,
	const AxisJobFence  *fence,
	guint               *n_swept,
	GError             **error
);

/*
 * The worker ids of a daemon's in-process workers: "axisd:HOST:PID:INSTANCE"
 * (its daemon id), then "/QUEUE/N".  INSTANCE is
 * AXIS_JOB_INSTANCE_LEN lower-case hexadecimal digits, random for each start
 * of the queue, so no two runs share a worker id even with the same host
 * name and process id (a container started again).  Daemons before
 * add-single-daemon-lock wrote "axisd:HOST:PID/QUEUE/N", with no instance.
 * The pattern matches a daemon id of the current form.
 */
#define AXIS_JOB_INSTANCE_LEN        16
#define AXIS_JOB_DAEMON_ID_PATTERN   "^axisd:[^/]*:[0-9]+:[0-9a-f]{16}$"

/**
 * axis_job_take_back_from_daemons:
 * @conn: a connection held by the calling thread
 * @kinds: (array zero-terminated=1): the kinds this daemon has handlers for
 * @own_daemon: this daemon's id ("axisd:HOST:PID:INSTANCE"), whose jobs are
 *   left alone
 * @fence: (nullable): the daemon's lock: nothing is taken back when it does
 *   not hold (a daemon that lost its lock would take back the live jobs of
 *   the one that took it)
 * @n_taken: (out) (optional): how many jobs were taken back
 * @error: (nullable): return location for an error
 *
 * Takes back at once, whatever their leases, the running jobs of @kinds
 * whose worker is an in-process worker of another daemon run: its daemon id
 * matches %AXIS_JOB_DAEMON_ID_PATTERN and is not @own_daemon.  Each is
 * treated as axis_job_sweep_expired_leases() treats a job whose lease
 * expired (queued again at once, failed after its last attempt, cancelled
 * when a cancel was requested), its last error naming the daemon, and is
 * logged as a warning.  Jobs of remote workers ("worker:NAME") and of
 * daemons of the earlier form, which do not take the single-daemon lock and
 * may still be running, are left to their leases.
 *
 * Safe only for a daemon that holds the database alone, once every daemon
 * that held it before must have stopped its handlers (see
 * axis_job_queue_set_take_over()).  Fails as axis_db_conn_exec(), with
 * %AXIS_JOB_ERROR_FENCED when @fence does not hold (nothing changed), and
 * with %AXIS_JOB_ERROR_INVALID when @own_daemon does not match the pattern.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_take_back_from_daemons(
	AxisDbConn          *conn,
	const gchar *const  *kinds,
	const gchar         *own_daemon,
	const AxisJobFence  *fence,
	guint               *n_taken,
	GError             **error
);

/*
 * Remote workers (api/axis-worker-api.h).  A remote worker's job is
 * claimed, renewed and expired by these; it ends through the functions
 * above, inside a transaction that first locks it with
 * axis_job_lock_remote().  Every one of them changes or locks a job only
 * when it is running under the given token, for the given worker id, and
 * (except the expiry) when its lease has not run out by the database's
 * clock: a lease that ran out is lost even before the sweep takes it back.
 */

/**
 * AxisJobRemoteFilter:
 * @worker: the worker's name: a job whose payload's "requires"."worker"
 *   names another worker is not claimed; nor, before the time (seconds
 *   since the epoch) in "requires"."prefer_not_until", one whose
 *   "requires"."prefer_not" names this worker (other workers are offered it
 *   first)
 * @machine: the encoder_tool machine its capabilities are stored under: a
 *   job whose "requires"."encoder" is not usable there (or not 10-bit
 *   capable where "requires"."ten_bit" is true) is not claimed
 * @volumes_json: a JSON array of the volume UUIDs it has proved: a job
 *   whose "requires"."volumes" are not all among them is not claimed
 */
typedef struct
{
	const gchar *worker;
	const gchar *machine;
	const gchar *volumes_json;
} AxisJobRemoteFilter;

/**
 * axis_job_claim_remote:
 *
 * As axis_job_claim(), for a remote worker: only jobs that @filter allows
 * are taken (see above).
 */
gboolean
axis_job_claim_remote(
	AxisDbConn                 *conn,
	const gchar                *queue,
	const gchar *const         *kinds,
	const gchar                *worker_id,
	const AxisJobRemoteFilter  *filter,
	guint                       lease_ms,
	AxisJobClaim              **claim,
	GError                    **error
);

/**
 * axis_job_claim_remote_fenced:
 * @fence: (nullable): the fence, or %NULL for none
 *
 * As axis_job_claim_remote(), claiming nothing (and succeeding) when
 * @fence does not hold.
 */
gboolean
axis_job_claim_remote_fenced(
	AxisDbConn                 *conn,
	const gchar                *queue,
	const gchar *const         *kinds,
	const gchar                *worker_id,
	const AxisJobRemoteFilter  *filter,
	guint                       lease_ms,
	const AxisJobFence         *fence,
	AxisJobClaim              **claim,
	GError                    **error
);

/**
 * axis_job_heartbeat_remote:
 *
 * As axis_job_heartbeat(), and fails with %AXIS_JOB_ERROR_LEASE_LOST also
 * when the job is not held by @worker_id or its lease has run out.
 */
gboolean
axis_job_heartbeat_remote(
	AxisDbConn             *conn,
	gint64                  id,
	const gchar            *lease_token,
	const gchar            *worker_id,
	guint                   lease_ms,
	const AxisJobProgress  *progress,
	gboolean               *cancel_requested,
	GError                **error
);

/**
 * axis_job_lock_remote:
 * @kind: (out) (optional) (transfer full): the job's kind
 * @payload: (out) (optional) (transfer full): its payload as JSON text
 *
 * Inside a transaction: locks the job (FOR UPDATE) if it is running under
 * @lease_token for @worker_id with a lease that has not run out, so that
 * nothing else changes it before the transaction ends; then end it with
 * axis_job_succeed(), axis_job_fail() or axis_job_hand_back() in the same
 * transaction.  Fails with %AXIS_JOB_ERROR_LEASE_LOST otherwise.
 */
gboolean
axis_job_lock_remote(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *lease_token,
	const gchar  *worker_id,
	gchar       **kind,
	gchar       **payload,
	GError      **error
);

/**
 * axis_job_expire_remote:
 * @state: (out) (optional): what the job became
 *
 * Takes back one job whose lease the caller found expired by its own
 * clock, exactly as axis_job_sweep_expired_leases() takes back those the
 * database's clock finds expired: queued again at once (the run counted
 * as an attempt), failed when that was its last attempt, cancelled when a
 * cancel was requested.  Changes nothing, and fails with
 * %AXIS_JOB_ERROR_LEASE_LOST, when the job is not running under
 * @lease_token any more.
 */
gboolean
axis_job_expire_remote(
	AxisDbConn    *conn,
	gint64         id,
	const gchar   *lease_token,
	AxisJobState  *state,
	GError       **error
);

/**
 * axis_job_expire_remote_fenced:
 * @fence: (nullable): the fence, or %NULL for none
 *
 * As axis_job_expire_remote(); when @fence does not hold, nothing changes
 * and it fails with %AXIS_JOB_ERROR_FENCED.
 */
gboolean
axis_job_expire_remote_fenced(
	AxisDbConn          *conn,
	gint64               id,
	const gchar         *lease_token,
	const AxisJobFence  *fence,
	AxisJobState        *state,
	GError             **error
);

/**
 * axis_job_prune:
 * @conn: a connection held by the calling thread
 * @keep_finished_ms: succeeded and cancelled jobs that finished longer ago
 *   than this are deleted
 * @keep_failed_ms: failed jobs that finished longer ago than this are
 *   deleted, and so are the earlier runs (job.earlier_runs) of any job left
 *   that ended longer ago than this
 * @n_deleted: (out) (optional): how many jobs were deleted
 * @error: (nullable): return location for an error
 *
 * Deletes old finished jobs (queued and running jobs are never deleted),
 * then the old earlier runs of the jobs left; two statements.  Fails as
 * axis_db_conn_exec().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_prune(
	AxisDbConn   *conn,
	gint64        keep_finished_ms,
	gint64        keep_failed_ms,
	guint64      *n_deleted,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_JOB_LIFECYCLE_H */
