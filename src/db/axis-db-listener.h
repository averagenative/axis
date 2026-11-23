/*
 * axis-db-listener.h - a dedicated connection that waits for notifications
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * LISTEN needs a session that stays open: a pooled connection goes back to
 * the pool after each use, and its LISTEN with it.  A listener opens one
 * connection of its own, outside the pool's slots but with the pool's URL
 * and session settings (plus idle_session_timeout off, since it is idle by
 * design), and LISTENs on one channel.
 *
 * A listener is used by one thread at a time; nothing here is thread-safe.
 * Every wait has a deadline (a g_get_monotonic_time() value, -1 for none).
 * A broken connection is reported with %AXIS_DB_ERROR_CONNECTION_LOST and
 * never silently re-established: the caller decides when to call
 * axis_db_listener_reconnect().  Notifications sent while the listener was
 * not connected are not delivered.
 *
 * PostgreSQL delivers the notifications of one transaction in the order
 * they were sent, and those of different transactions in the order the
 * transactions committed.  A notification sent from inside a transaction
 * is delivered only if, and when, that transaction commits.
 */

#ifndef AXIS_DB_LISTENER_H
#define AXIS_DB_LISTENER_H

#include <glib.h>
#include "db/axis-db-error.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

typedef struct _AxisDbListener AxisDbListener;

/**
 * AxisDbListenerWait:
 * @AXIS_DB_LISTENER_READY: at least one notification can be taken with
 *   axis_db_listener_pop()
 * @AXIS_DB_LISTENER_WOKEN: the wake descriptor became readable (the caller
 *   drains it)
 * @AXIS_DB_LISTENER_TIMEOUT: the deadline passed
 * @AXIS_DB_LISTENER_LOST: the connection is gone (the error says why);
 *   notifications received before that can still be popped
 */
typedef enum
{
	AXIS_DB_LISTENER_READY,
	AXIS_DB_LISTENER_WOKEN,
	AXIS_DB_LISTENER_TIMEOUT,
	AXIS_DB_LISTENER_LOST
} AxisDbListenerWait;

/**
 * axis_db_listener_new:
 * @pool: the pool whose URL and settings to use; it must outlive the
 *   listener
 * @channel: the channel to LISTEN on (any non-empty name; it is quoted)
 * @deadline: when to give up connecting, or -1 for the connect timeout
 *   alone
 * @error: (nullable): return location for an error
 *
 * Connects and LISTENs.  Fails as axis_db_pool_acquire() does when it has
 * to connect, with %AXIS_DB_ERROR_TIMEOUT when @deadline passes, and with
 * %AXIS_DB_ERROR_QUERY when the server refuses LISTEN.
 *
 * Returns: (transfer full) (nullable): the listener, or %NULL on error
 */
AxisDbListener *
axis_db_listener_new(
	AxisDbPool   *pool,
	const gchar  *channel,
	gint64        deadline,
	GError      **error
);

/**
 * axis_db_listener_free:
 *
 * Closes the connection (the server drops its LISTEN with it) and frees
 * any notifications not yet popped.
 */
void
axis_db_listener_free(AxisDbListener *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDbListener, axis_db_listener_free)

/**
 * axis_db_listener_is_connected:
 *
 * Returns: %TRUE unless the connection has been found broken (or a wait
 *   for it ran out) since it was last established
 */
gboolean
axis_db_listener_is_connected(AxisDbListener *self);

/**
 * axis_db_listener_reconnect:
 * @self: an #AxisDbListener
 * @deadline: when to give up, or -1 for the connect timeout alone
 * @error: (nullable): return location for an error
 *
 * Closes the connection if it is open and establishes a new one that
 * LISTENs on the same channel.  Notifications already received and not
 * yet popped are kept.  Fails as axis_db_listener_new(), leaving the
 * listener disconnected.
 *
 * Returns: %TRUE when connected again
 */
gboolean
axis_db_listener_reconnect(
	AxisDbListener  *self,
	gint64           deadline,
	GError         **error
);

/**
 * axis_db_listener_wait:
 * @self: an #AxisDbListener
 * @wake_fd: a descriptor that another thread makes readable to interrupt
 *   the wait (the end of a pipe, say), or -1
 * @deadline: when to stop waiting, or -1 for no limit
 * @error: (nullable): set for %AXIS_DB_LISTENER_LOST
 *
 * Returns at once with %AXIS_DB_LISTENER_READY when notifications are
 * already waiting to be popped.  Otherwise waits for the server to send
 * something, for @wake_fd to become readable, or for @deadline.  On a
 * disconnected listener it waits only for @wake_fd and @deadline.
 *
 * Returns: what ended the wait
 */
AxisDbListenerWait
axis_db_listener_wait(
	AxisDbListener  *self,
	gint             wake_fd,
	gint64           deadline,
	GError         **error
);

/**
 * axis_db_listener_pop:
 *
 * Returns: (transfer full) (nullable): the payload of the oldest
 *   notification received and not yet popped, or %NULL if there is none
 */
gchar *
axis_db_listener_pop(AxisDbListener *self);

/**
 * axis_db_listener_notify:
 * @self: a connected #AxisDbListener
 * @payload: the payload (at most 7999 bytes)
 * @deadline: when to give up, or -1 for no limit
 * @error: (nullable): return location for an error
 *
 * Sends a notification on the listener's own channel from its own
 * connection, and waits until the server has committed it.  The listener
 * receives it too, after everything committed before it: so once it has
 * been popped, so has every notification committed before this call.
 * Fails with %AXIS_DB_ERROR_TIMEOUT when @deadline passes (the listener is
 * then disconnected), and as axis_db_listener_wait() otherwise.
 *
 * Returns: %TRUE when sent
 */
gboolean
axis_db_listener_notify(
	AxisDbListener  *self,
	const gchar     *payload,
	gint64           deadline,
	GError         **error
);

/**
 * axis_db_listener_ping:
 * @self: a connected #AxisDbListener
 * @deadline: when to give up, or -1 for no limit
 * @error: (nullable): return location for an error
 *
 * Checks with a round trip that the connection still works: a listener
 * whose network has silently gone away otherwise waits for ever.  Fails as
 * axis_db_listener_notify().
 *
 * Returns: %TRUE if the server answered
 */
gboolean
axis_db_listener_ping(
	AxisDbListener  *self,
	gint64           deadline,
	GError         **error
);

G_END_DECLS

#endif /* AXIS_DB_LISTENER_H */
