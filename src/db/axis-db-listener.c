/*
 * axis-db-listener.c - a dedicated connection that waits for notifications
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Commands are sent with libpq's asynchronous calls and waited for with a
 * deadline, as in axis-db-conn.c.  Notifications that arrive while a
 * command runs are kept by libpq and moved to our own queue whenever input
 * has been consumed, so none is lost between a command and a wait.
 */

#include "db/axis-db-listener.h"
#include "db/axis-db-private.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

struct _AxisDbListener
{
	AxisDbPool *pool;
	gchar      *channel;
	gchar      *listen_sql;
	PGconn     *pg;		/* NULL while disconnected */
	GQueue      pending;	/* of gchar *, payloads oldest first */
};

/* libpq's messages on one line, scrubbed of the password. */
static gchar *
listener_message(
	AxisDbListener *self,
	const gchar    *text
){
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *joined = NULL;
	guint i;

	lines = g_strsplit(text != NULL && *text != '\0' ? text : "unknown error", "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
		g_strstrip(lines[i]);
	joined = g_strjoinv(" ", lines);

	return axis_db_pool_scrub(self->pool, g_strstrip(joined));
}

static void
listener_close(AxisDbListener *self)
{
	if (self->pg != NULL) {
		PQfinish(self->pg);
		self->pg = NULL;
	}
}

/* Moves what libpq has received to the pending queue. */
static void
collect_notifications(AxisDbListener *self)
{
	PGnotify *notify;

	if (self->pg == NULL)
		return;

	while ((notify = PQnotifies(self->pg)) != NULL) {
		g_queue_push_tail(&self->pending, g_strdup(notify->extra != NULL ? notify->extra : ""));
		PQfreemem(notify);
	}
}

static void
set_lost(
	AxisDbListener  *self,
	GError         **error
){
	g_autofree gchar *text = NULL;

	text = listener_message(self, self->pg != NULL ? PQerrorMessage(self->pg) : "not connected");
	g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST,
		"lost the database connection that listens for notifications: %s", text);
	listener_close(self);
}

/*
 * Runs one statement and reads every result, giving up at @deadline.  A
 * timeout leaves the connection in an unknown state, so it is closed.
 */
static gboolean
listener_run(
	AxisDbListener      *self,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	gint64               deadline,
	GError             **error
){
	g_autofree gchar *failure = NULL;
	PGresult *res;

	if (self->pg == NULL || PQstatus(self->pg) != CONNECTION_OK) {
		set_lost(self, error);
		return FALSE;
	}

	if (!PQsendQueryParams(self->pg, sql, (int)n_params, NULL, params, NULL, NULL, 0)) {
		set_lost(self, error);
		return FALSE;
	}

	for (;;) {
		while (PQisBusy(self->pg)) {
			AxisDbWait wait = axis_db_wait_socket(self->pg, FALSE, deadline);

			if (wait == AXIS_DB_WAIT_TIMEOUT) {
				collect_notifications(self);
				listener_close(self);
				g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT,
					"the database did not answer in time");
				return FALSE;
			}
			if (wait == AXIS_DB_WAIT_FAILED || !PQconsumeInput(self->pg))
				break;
		}

		res = PQgetResult(self->pg);
		if (res == NULL)
			break;
		if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK &&
		    failure == NULL)
			failure = listener_message(self, PQresultErrorMessage(res));
		PQclear(res);
		if (PQstatus(self->pg) == CONNECTION_BAD)
			break;
	}

	collect_notifications(self);

	if (PQstatus(self->pg) != CONNECTION_OK) {
		set_lost(self, error);
		return FALSE;
	}
	if (failure != NULL) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY, "%s", failure);
		return FALSE;
	}

	return TRUE;
}

static gboolean
listener_connect(
	AxisDbListener  *self,
	gint64           deadline,
	GError         **error
){
	listener_close(self);

	self->pg = axis_db_pool_connect_dedicated(self->pool, deadline, error);
	if (self->pg == NULL)
		return FALSE;

	/* Idle is this connection's normal state. */
	if (!listener_run(self, "SELECT set_config('idle_session_timeout', '0', false)",
	                  0, NULL, deadline, error) ||
	    !listener_run(self, self->listen_sql, 0, NULL, deadline, error)) {
		listener_close(self);
		return FALSE;
	}

	return TRUE;
}

AxisDbListener *
axis_db_listener_new(
	AxisDbPool   *pool,
	const gchar  *channel,
	gint64        deadline,
	GError      **error
){
	AxisDbListener *self;
	g_autofree gchar *quoted = NULL;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(channel != NULL && *channel != '\0', NULL);

	quoted = axis_db_quote_identifier(channel);
	g_return_val_if_fail(quoted != NULL, NULL);

	self = g_new0(AxisDbListener, 1);
	self->pool = pool;
	self->channel = g_strdup(channel);
	self->listen_sql = g_strconcat("LISTEN ", quoted, NULL);
	g_queue_init(&self->pending);

	if (!listener_connect(self, deadline, error)) {
		axis_db_listener_free(self);
		return NULL;
	}

	return self;
}

void
axis_db_listener_free(AxisDbListener *self)
{
	if (self == NULL)
		return;

	listener_close(self);
	g_queue_clear_full(&self->pending, g_free);
	g_free(self->channel);
	g_free(self->listen_sql);
	g_free(self);
}

gboolean
axis_db_listener_is_connected(AxisDbListener *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	return self->pg != NULL && PQstatus(self->pg) == CONNECTION_OK;
}

gboolean
axis_db_listener_reconnect(
	AxisDbListener  *self,
	gint64           deadline,
	GError         **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	collect_notifications(self);
	return listener_connect(self, deadline, error);
}

/* Waits for @wake_fd alone (a disconnected listener). */
static AxisDbListenerWait
wait_wake_only(
	gint   wake_fd,
	gint64 deadline
){
	for (;;) {
		struct pollfd pfd;
		int timeout_ms = -1;
		int rc;

		if (deadline >= 0) {
			gint64 now = g_get_monotonic_time();
			gint64 remaining;

			if (now >= deadline)
				return AXIS_DB_LISTENER_TIMEOUT;
			remaining = (deadline - now + 999) / 1000;
			timeout_ms = remaining > G_MAXINT ? G_MAXINT : (int)remaining;
		}

		if (wake_fd < 0) {
			/* Nothing to wait for but the clock. */
			if (timeout_ms < 0)
				return AXIS_DB_LISTENER_TIMEOUT;
			g_usleep((gulong)timeout_ms * 1000);
			continue;
		}

		pfd.fd = wake_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		rc = poll(&pfd, 1, timeout_ms);
		if (rc > 0)
			return AXIS_DB_LISTENER_WOKEN;
		if (rc < 0 && errno != EINTR)
			return AXIS_DB_LISTENER_WOKEN;	/* let the caller look */
	}
}

AxisDbListenerWait
axis_db_listener_wait(
	AxisDbListener  *self,
	gint             wake_fd,
	gint64           deadline,
	GError         **error
){
	g_return_val_if_fail(self != NULL, AXIS_DB_LISTENER_TIMEOUT);

	if (!g_queue_is_empty(&self->pending))
		return AXIS_DB_LISTENER_READY;
	if (self->pg == NULL)
		return wait_wake_only(wake_fd, deadline);

	for (;;) {
		struct pollfd pfd[2];
		nfds_t n = 1;
		int timeout_ms = -1;
		int rc;

		if (deadline >= 0) {
			gint64 now = g_get_monotonic_time();
			gint64 remaining;

			if (now >= deadline)
				return AXIS_DB_LISTENER_TIMEOUT;
			remaining = (deadline - now + 999) / 1000;
			timeout_ms = remaining > G_MAXINT ? G_MAXINT : (int)remaining;
		}

		pfd[0].fd = PQsocket(self->pg);
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		if (pfd[0].fd < 0) {
			set_lost(self, error);
			return AXIS_DB_LISTENER_LOST;
		}
		if (wake_fd >= 0) {
			pfd[1].fd = wake_fd;
			pfd[1].events = POLLIN;
			pfd[1].revents = 0;
			n = 2;
		}

		rc = poll(pfd, n, timeout_ms);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			set_lost(self, error);
			return AXIS_DB_LISTENER_LOST;
		}
		if (rc == 0)
			continue;	/* the deadline is checked at the top */

		if (pfd[0].revents != 0) {
			if (!PQconsumeInput(self->pg) || PQstatus(self->pg) != CONNECTION_OK) {
				collect_notifications(self);
				set_lost(self, error);
				return AXIS_DB_LISTENER_LOST;
			}
			collect_notifications(self);
			if (!g_queue_is_empty(&self->pending))
				return AXIS_DB_LISTENER_READY;
		}
		if (n == 2 && pfd[1].revents != 0)
			return AXIS_DB_LISTENER_WOKEN;
	}
}

gchar *
axis_db_listener_pop(AxisDbListener *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return g_queue_pop_head(&self->pending);
}

gboolean
axis_db_listener_notify(
	AxisDbListener  *self,
	const gchar     *payload,
	gint64           deadline,
	GError         **error
){
	const gchar *params[2];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(payload != NULL, FALSE);

	params[0] = self->channel;
	params[1] = payload;
	return listener_run(self, "SELECT pg_notify($1, $2)", 2, params, deadline, error);
}

gboolean
axis_db_listener_ping(
	AxisDbListener  *self,
	gint64           deadline,
	GError         **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	return listener_run(self, "SELECT 1", 0, NULL, deadline, error);
}
