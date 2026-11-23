/*
 * axis-db-lock.h - a session advisory lock held on a connection of its own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A session advisory lock lasts as long as the session that took it, so it
 * cannot live on a pooled connection (axis-db-pool.h) that goes back to the
 * pool after each use.  An AxisDbLock opens one connection of its own,
 * outside the pool's slots but with the pool's URL and session settings, as
 * a listener does (axis-db-listener.h), and takes the lock there without
 * waiting (pg_try_advisory_lock()).  The server releases the lock when that
 * session ends, however it ends: the daemon exits or is killed, the network
 * goes, the server restarts, or someone ends the session.
 *
 * The session is identified by its server process id together with its start
 * time (pg_stat_activity.backend_start): process ids repeat after a server
 * restart, the pair does not.
 *
 * The connection names itself (application_name) so that whoever finds the
 * lock taken can say who holds it, from pg_locks and pg_stat_activity, which
 * every role can read.  It also asks the server to end it when the client
 * stops answering: TCP keep-alives (AXIS_DB_LOCK_KEEPALIVE_*) for an idle
 * connection, and a user timeout (AXIS_DB_LOCK_USER_TIMEOUT_MS) for data the
 * client never acknowledges, so that the lock of a machine that vanished
 * without closing its connection (power lost, network cut) is released within
 * about a minute instead of the operating system's defaults (more than two
 * hours idle, about fifteen minutes with data in flight).  (Over a Unix socket
 * the server ignores them; there is no network to lose.)
 *
 * A check that gets no answer proves nothing: a busy server can be slow.
 * axis_db_lock_check() therefore leaves an unanswered check in flight, with
 * the connection open, and says so (%AXIS_DB_LOCK_SILENT); the caller can ask
 * on a fresh connection whether the session still holds the lock
 * (axis_db_lock_verify()) and decide how long silence may last.
 *
 * The lock needs the session to stay one server session: a connection
 * through a pooler in transaction or statement mode (PgBouncer's
 * pool_mode = transaction, for example) hands each statement to whichever
 * server connection is free, so the lock stays on a server connection that
 * then serves other clients.  axis_db_lock_check() notices when the server
 * session behind the connection has changed.
 *
 * A lock is used by one thread at a time; nothing here is thread-safe.
 * Every call has a deadline (a g_get_monotonic_time() value, -1 for none).
 */

#ifndef AXIS_DB_LOCK_H
#define AXIS_DB_LOCK_H

#include <glib.h>
#include "db/axis-db-error.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

/* The lock one daemon holds for its life (see axisd's axis-daemon-lock.h and
 * openspec/specs/daemon-core/spec.md).  Its bytes spell "axis_dmn";
 * the migration lock (axis-db-migrate.h) is another key. */
#define AXIS_DB_DAEMON_LOCK_KEY G_GINT64_CONSTANT(0x617869735f646d6e)

/* The longest application_name the server keeps (NAMEDATALEN - 1); a lock's
 * name is cut to this. */
#define AXIS_DB_LOCK_NAME_MAX 63

/* A session's start as text, the same on every connection whatever its
 * settings: SQL for the pg_stat_activity row @a. */
#define AXIS_DB_BACKEND_START_SQL(a) \
	"to_char(" a ".backend_start AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US')"

/* Room for that text and its terminator. */
#define AXIS_DB_BACKEND_START_MAX 32

/* What the lock's session asks of the server's TCP stack: the first
 * keep-alive probe after this many seconds without traffic, then one every
 * interval, and the session is ended after that many go unanswered; and it
 * is ended when data it sent stays unacknowledged this long. */
#define AXIS_DB_LOCK_KEEPALIVE_IDLE_S     30
#define AXIS_DB_LOCK_KEEPALIVE_INTERVAL_S 10
#define AXIS_DB_LOCK_KEEPALIVE_COUNT      3
#define AXIS_DB_LOCK_USER_TIMEOUT_MS      60000

typedef struct _AxisDbLock AxisDbLock;

/**
 * AxisDbLockResult:
 * @AXIS_DB_LOCK_TAKEN: this lock holds it now
 * @AXIS_DB_LOCK_BUSY: another session holds it; the connection stays open
 *   for the next try
 * @AXIS_DB_LOCK_OURS: an earlier session of this same lock (its name) still
 *   holds it, and could not be ended in time; try again
 * @AXIS_DB_LOCK_ERROR: the server could not be asked (the error says why);
 *   the connection is closed
 */
typedef enum
{
	AXIS_DB_LOCK_TAKEN,
	AXIS_DB_LOCK_BUSY,
	AXIS_DB_LOCK_OURS,
	AXIS_DB_LOCK_ERROR
} AxisDbLockResult;

/**
 * AxisDbLockWait:
 * @AXIS_DB_LOCK_WOKEN: the wake descriptor became readable (the caller
 *   drains it)
 * @AXIS_DB_LOCK_TIMEOUT: the deadline passed, nothing happened
 * @AXIS_DB_LOCK_LOST: the connection is gone, and the lock with it (the
 *   error says why)
 */
typedef enum
{
	AXIS_DB_LOCK_WOKEN,
	AXIS_DB_LOCK_TIMEOUT,
	AXIS_DB_LOCK_LOST
} AxisDbLockWait;

/**
 * AxisDbLockState:
 * @AXIS_DB_LOCK_HELD: the server says the lock's session holds it
 * @AXIS_DB_LOCK_SILENT: no answer by the deadline; nothing is known
 * @AXIS_DB_LOCK_GONE: the session is gone or no longer holds the lock (the
 *   error says why)
 */
typedef enum
{
	AXIS_DB_LOCK_HELD,
	AXIS_DB_LOCK_SILENT,
	AXIS_DB_LOCK_GONE
} AxisDbLockState;

/**
 * AxisDbLockHolder:
 * @pid: the server process of the session holding the lock (0: unknown)
 * @application: (nullable): its application_name
 * @address: (nullable): the address it connected from (%NULL for a Unix
 *   socket, or when unknown)
 * @since: (nullable): when it connected, in UTC
 *
 * Who holds a lock, as far as the server says.
 */
typedef struct
{
	gint   pid;
	gchar *application;
	gchar *address;
	gchar *since;
} AxisDbLockHolder;

void
axis_db_lock_holder_free(AxisDbLockHolder *holder);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDbLockHolder, axis_db_lock_holder_free)

/**
 * axis_db_lock_holder_describe:
 * @holder: (nullable): a holder, or %NULL when it could not be read
 *
 * Returns: (transfer full): one line for a log: "session 4242 (NAME) from
 *   ADDRESS, connected since TIME", leaving out what is unknown
 */
gchar *
axis_db_lock_holder_describe(const AxisDbLockHolder *holder);

/**
 * axis_db_lock_new:
 * @pool: the pool whose URL and settings to use; it must outlive the lock
 * @key: the advisory lock (pg_try_advisory_lock(bigint))
 * @name: the application_name the lock's session gives itself, cut to
 *   %AXIS_DB_LOCK_NAME_MAX bytes; make it unique to this process and start
 *   (axis_db_lock_try() takes a session with the same name for one of its
 *   own)
 *
 * Returns: (transfer full): a lock, not connected and not held
 */
AxisDbLock *
axis_db_lock_new(
	AxisDbPool  *pool,
	gint64       key,
	const gchar *name
);

/**
 * axis_db_lock_free:
 *
 * Closes the connection: the server releases the lock if it was held.
 */
void
axis_db_lock_free(AxisDbLock *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDbLock, axis_db_lock_free)

/**
 * axis_db_lock_get_name:
 *
 * Returns: (transfer none): the name its session gives itself, as cut
 */
const gchar *
axis_db_lock_get_name(AxisDbLock *self);

/**
 * axis_db_lock_get_key:
 *
 * Returns: the advisory lock's key
 */
gint64
axis_db_lock_get_key(AxisDbLock *self);

/**
 * axis_db_lock_try:
 * @self: an #AxisDbLock
 * @deadline: when to give up connecting or waiting for an answer
 * @holder: (out) (optional) (transfer full) (nullable): for
 *   %AXIS_DB_LOCK_BUSY, who holds it (%NULL if the server would not say)
 * @error: (nullable): set for %AXIS_DB_LOCK_ERROR
 *
 * Connects if it is not connected, then tries to take the lock without
 * waiting.  When the session holding it carries this lock's own name, it is
 * an earlier connection of this same lock that the server has not yet
 * noticed is gone (the network failed under it): that session is ended
 * (pg_terminate_backend(), waiting for it within @deadline) and the lock
 * tried once more.  A session of this name that still holds it then is
 * %AXIS_DB_LOCK_OURS, never %AXIS_DB_LOCK_BUSY.  A lock already held is
 * simply %AXIS_DB_LOCK_TAKEN.
 *
 * Returns: what happened
 */
AxisDbLockResult
axis_db_lock_try(
	AxisDbLock        *self,
	gint64             deadline,
	AxisDbLockHolder **holder,
	GError           **error
);

/**
 * axis_db_lock_is_held:
 *
 * Returns: %TRUE from a successful axis_db_lock_try() until the lock is
 *   found lost or released
 */
gboolean
axis_db_lock_is_held(AxisDbLock *self);

/**
 * axis_db_lock_get_backend_pid:
 *
 * Returns: the server process of the session that holds the lock (the
 *   value pg_locks shows for it), or 0 when not held
 */
gint
axis_db_lock_get_backend_pid(AxisDbLock *self);

/**
 * axis_db_lock_get_backend_start:
 *
 * Returns: (transfer none) (nullable): when the session that holds the lock
 *   started, as %AXIS_DB_BACKEND_START_SQL gives it, or %NULL when not held
 */
const gchar *
axis_db_lock_get_backend_start(AxisDbLock *self);

/**
 * axis_db_lock_wait:
 * @self: a held #AxisDbLock
 * @wake_fd: a descriptor that another thread makes readable to interrupt
 *   the wait, or -1
 * @deadline: when to stop waiting, or -1 for no limit
 * @error: (nullable): set for %AXIS_DB_LOCK_LOST
 *
 * Waits for the connection to change (the server closed it or ended the
 * session: %AXIS_DB_LOCK_LOST at once), for @wake_fd, or for @deadline.  A
 * notice the server sends is logged, and the answer to a check still in
 * flight is read and kept for axis_db_lock_check(); the wait goes on.  On a
 * lock that is not held it waits only for @wake_fd and @deadline.  It cannot
 * see a network that went silent; that is what axis_db_lock_check() is for.
 *
 * Returns: what ended the wait
 */
AxisDbLockWait
axis_db_lock_wait(
	AxisDbLock  *self,
	gint         wake_fd,
	gint64       deadline,
	GError     **error
);

/**
 * axis_db_lock_check:
 * @self: a held #AxisDbLock
 * @deadline: when to stop waiting for the answer
 * @sent: (out) (optional): for %AXIS_DB_LOCK_HELD, when the check that was
 *   answered was sent (a g_get_monotonic_time() value): the lock was held at
 *   some moment after it
 * @error: (nullable): set for %AXIS_DB_LOCK_GONE
 *
 * Asks, with a round trip on the lock's connection, whether the server
 * session behind it is still the one that took the lock and still holds it.
 * If an earlier check is still in flight, it waits for that one's answer
 * rather than sending another.  No answer by @deadline is
 * %AXIS_DB_LOCK_SILENT: the check stays in flight and the connection open
 * (closing it would release the lock).  A negative answer, an error or a
 * closed connection is %AXIS_DB_LOCK_GONE: the connection is closed and the
 * lock no longer held.
 *
 * Returns: what the server said, if anything
 */
AxisDbLockState
axis_db_lock_check(
	AxisDbLock  *self,
	gint64       deadline,
	gint64      *sent,
	GError     **error
);

/**
 * axis_db_lock_verify:
 * @self: a held #AxisDbLock
 * @deadline: when to give up connecting or waiting for the answer
 * @sent: (out) (optional): for %AXIS_DB_LOCK_HELD, when the question was
 *   sent
 * @error: (nullable): set for %AXIS_DB_LOCK_GONE and %AXIS_DB_LOCK_SILENT
 *
 * Asks on a fresh connection of its own (closed afterwards) whether the
 * lock's session, by its process id and start, still holds the lock: for a
 * check that went unanswered.  %AXIS_DB_LOCK_GONE (the server says no) does
 * not close the lock's connection; the caller does, with
 * axis_db_lock_release().  A fresh connection that cannot be made or gets
 * no answer by @deadline is %AXIS_DB_LOCK_SILENT.
 *
 * Returns: what the server said, if anything
 */
AxisDbLockState
axis_db_lock_verify(
	AxisDbLock  *self,
	gint64       deadline,
	gint64      *sent,
	GError     **error
);

/**
 * axis_db_lock_release:
 *
 * Closes the connection (so the server releases the lock).  The lock can
 * be tried again afterwards.
 */
void
axis_db_lock_release(AxisDbLock *self);

G_END_DECLS

#endif /* AXIS_DB_LOCK_H */
