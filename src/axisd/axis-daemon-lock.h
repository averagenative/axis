/*
 * axis-daemon-lock.h - one daemon per database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * axisd holds its database with the single-daemon lock
 * (AXIS_DB_DAEMON_LOCK_KEY, db/axis-db-lock.h) for its whole life, on a
 * connection of its own.  What follows is the policy around it; the reasons
 * requirements are in openspec/specs/daemon-core/spec.md.
 *
 * At start, after connecting and before migrating: the lock is tried every
 * AXIS_DAEMON_LOCK_RETRY_MS.  While another session holds it the daemon is a
 * standby: it serves only the probes, runs no job and changes nothing,
 * logs who holds the database, and takes over when that one stops, within
 * database.wait (0: it exits at once).  Taking it before the migrations
 * means a newer daemon waiting for an older one never changes the schema
 * under it.
 *
 * While serving, a watcher thread waits on the lock's connection (a server
 * that ends the session or restarts is seen at once) and sends a check
 * every AXIS_DAEMON_LOCK_CHECK_MS, counted from the previous check's
 * sending.  A check not answered within AXIS_DAEMON_LOCK_CHECK_DEADLINE_MS
 * proves nothing (a busy server can be slow): it stays in flight on the
 * open connection, and the watcher asks on a fresh connection whether the
 * lock's session, by its process id and start, still holds the lock.  The
 * lock is lost when the connection closes, when either question is answered
 * no, or when no question sent in the last AXIS_DAEMON_LOCK_SILENCE_MS has
 * been answered yes.  So a loss is noticed at most that long after the lock
 * was last known held.  When the lock is lost:
 *
 *  1. hooks.lost(): the daemon stops starting work (the gate answers only
 *     the probes, the job queue is suspended and its handlers told to stop);
 *  2. hooks.wait_handlers(): every handler has AXIS_DAEMON_LOCK_STOP_GRACE_MS
 *     to return.  One that has not by then gets the process ended at once
 *     (_exit(3)), because another daemon may take the database and must
 *     never run beside it;
 *  3. the lock is taken again, retrying with back-off for as long as it
 *     takes.  Taken: hooks.regained(), and the daemon resumes.  Held by
 *     another session: hooks.taken(), and the daemon stops (it may have
 *     missed changes that one made, so it starts afresh, as a standby).
 *
 * The bound: a daemon has no handler running at most SILENCE + STOP_GRACE
 * after its lock was released.  The server can give the lock to another
 * daemon at once (it ended the session); that daemon cannot run the same
 * jobs before their leases (AXIS_DAEMON_LOCK_JOB_LEASE_MS, renewed every
 * third and only while the lock is held) run out, at least two thirds of a
 * lease after the release, nor take them back before its take-over delay.
 * Both are longer than the bound, with a margin.  A daemon cut off from the
 * server is ended by the server only after its keep-alives and user timeout
 * (about a minute), by which time it has stopped.
 *
 * A daemon that holds the lock may take back, without waiting for their
 * leases, the jobs that earlier runs left running, once
 * AXIS_DAEMON_LOCK_TAKE_OVER_MS has passed since its job queue started:
 * whoever held the lock before lost it before this daemon took it, and has
 * by then noticed (step 1 above) and stopped its handlers (step 2) or ended.
 *
 * Main thread unless said otherwise.
 */

#ifndef AXIS_DAEMON_LOCK_H
#define AXIS_DAEMON_LOCK_H

#include <glib.h>

#include "db/axis-db-lock.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

#define AXIS_DAEMON_LOCK_RETRY_MS           2000
#define AXIS_DAEMON_LOCK_CHECK_MS           5000
#define AXIS_DAEMON_LOCK_CHECK_DEADLINE_MS  5000
#define AXIS_DAEMON_LOCK_VERIFY_DEADLINE_MS 5000
#define AXIS_DAEMON_LOCK_SILENCE_MS         20000
#define AXIS_DAEMON_LOCK_STOP_GRACE_MS      25000
#define AXIS_DAEMON_LOCK_TRY_DEADLINE_MS    3000
#define AXIS_DAEMON_LOCK_RETAKE_MAX_MS      5000

/* The lease of the daemon's in-process jobs: renewed every third of it,
 * so at least two thirds of it are left when the lock is released. */
#define AXIS_DAEMON_LOCK_JOB_LEASE_MS       90000

/* How long a daemon that lost its lock can take, at most, to have no
 * handler running (noticing, then the grace), with a margin. */
#define AXIS_DAEMON_LOCK_STOPPED_WITHIN_MS (AXIS_DAEMON_LOCK_SILENCE_MS + AXIS_DAEMON_LOCK_STOP_GRACE_MS)
#define AXIS_DAEMON_LOCK_TAKE_OVER_MS      (AXIS_DAEMON_LOCK_STOPPED_WITHIN_MS + 5000)

/* The arithmetic the safety argument rests on, with margins. */
G_STATIC_ASSERT(AXIS_DAEMON_LOCK_STOPPED_WITHIN_MS + 10000 <=
	AXIS_DAEMON_LOCK_JOB_LEASE_MS - AXIS_DAEMON_LOCK_JOB_LEASE_MS / 3);
G_STATIC_ASSERT(AXIS_DAEMON_LOCK_STOPPED_WITHIN_MS + 10000 <= AXIS_DB_LOCK_USER_TIMEOUT_MS);
G_STATIC_ASSERT(AXIS_DAEMON_LOCK_STOPPED_WITHIN_MS + 10000 <= 1000 *
	(AXIS_DB_LOCK_KEEPALIVE_IDLE_S + AXIS_DB_LOCK_KEEPALIVE_INTERVAL_S * AXIS_DB_LOCK_KEEPALIVE_COUNT));
G_STATIC_ASSERT(AXIS_DAEMON_LOCK_CHECK_DEADLINE_MS + AXIS_DAEMON_LOCK_VERIFY_DEADLINE_MS <
	AXIS_DAEMON_LOCK_SILENCE_MS);

/* The process's exit status when a handler did not stop after the lock was
 * lost. */
#define AXIS_DAEMON_LOCK_EXIT_STUCK 3

typedef struct _AxisDaemonLock AxisDaemonLock;

/**
 * AxisDaemonLockHooks:
 * @lost: the lock is gone: stop starting work and tell running work to stop;
 *   return at once
 * @wait_handlers: whether no handler runs any more, waiting up to
 *   @timeout_ms
 * @regained: the lock is held again, by the session @backend_pid that
 *   started at @backend_start: resume
 * @taken: another session holds the lock: the daemon must stop
 *   (axis_daemon_lock_get_taken_by() says by whom)
 *
 * What the watcher calls, on its own thread.
 */
typedef struct
{
	void     (*lost)(gpointer data);
	gboolean (*wait_handlers)(gpointer data, guint timeout_ms);
	void     (*regained)(gpointer data, gint backend_pid, const gchar *backend_start);
	void     (*taken)(gpointer data);
} AxisDaemonLockHooks;

/* A lock for @pool's database, named after this process and a random
 * instance; not tried yet. */
AxisDaemonLock *
axis_daemon_lock_new(AxisDbPool *pool);

/* Releases the lock (closes its connection).  Stop watching first. */
void
axis_daemon_lock_free(AxisDaemonLock *self);

/* AXIS_JOB_INSTANCE_LEN hexadecimal digits, new for each start: the job
 * queue's workers are named with it too. */
const gchar *
axis_daemon_lock_get_instance(AxisDaemonLock *self);

/* "axisd INSTANCE VERSION pid PID on HOST": the lock session's name. */
const gchar *
axis_daemon_lock_get_name(AxisDaemonLock *self);

gint64
axis_daemon_lock_get_key(AxisDaemonLock *self);

/*
 * Tries to take the lock (any one thread at a time, before watching).
 * BUSY: *@holder (transfer full) describes who holds it.  OURS: an earlier
 * session of this daemon still holds it; try again.  ERROR: @error.
 */
AxisDbLockResult
axis_daemon_lock_try(
	AxisDaemonLock  *self,
	gchar          **holder,
	GError         **error
);

/* Whether the lock is held, as far as this daemon knows.  Any thread. */
gboolean
axis_daemon_lock_is_held(AxisDaemonLock *self);

/* The server process of the session that holds it (0: not held).  Any
 * thread. */
gint
axis_daemon_lock_get_backend_pid(AxisDaemonLock *self);

/* When that session started (AXIS_DB_BACKEND_START_SQL), or NULL when not
 * held (transfer none).  Before watching, or in a hook. */
const gchar *
axis_daemon_lock_get_backend_start(AxisDaemonLock *self);

/* After hooks.taken(): who took it (transfer none).  Any thread. */
const gchar *
axis_daemon_lock_get_taken_by(AxisDaemonLock *self);

/* Starts the watcher on a held lock. */
void
axis_daemon_lock_watch(
	AxisDaemonLock            *self,
	const AxisDaemonLockHooks *hooks,
	gpointer                   data
);

/* Stops the watcher and waits for it; the lock stays held (or not) as it
 * is.  A watcher that is waiting for handlers after a loss finishes that
 * wait first (at most AXIS_DAEMON_LOCK_STOP_GRACE_MS).  Calls no hook after
 * it returns. */
void
axis_daemon_lock_stop_watching(AxisDaemonLock *self);

G_END_DECLS

#endif /* AXIS_DAEMON_LOCK_H */
