/*
 * axis-db-lock.c - a session advisory lock held on a connection of its own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Commands are sent with libpq's asynchronous calls and waited for with a
 * deadline, as in axis-db-listener.c.  pg_locks shows an advisory lock taken
 * with one bigint key as classid (its high 32 bits), objid (its low 32 bits)
 * and objsubid 1, in the database it was taken in.  A session is named by its
 * process id and its start (AXIS_DB_BACKEND_START_SQL), which every role can
 * read of its own sessions.
 */

#include "db/axis-db-lock.h"
#include "db/axis-db-private.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

/* Ending an earlier session of this lock waits for it at most the time left
 * before the attempt's deadline, less this, so the answer still comes in
 * time. */
#define TERMINATE_MARGIN_MS 500

struct _AxisDbLock
{
	AxisDbPool *pool;
	gint64      key;
	gchar      *key_text;	/* the key as a parameter */
	gchar      *classid;	/* its high 32 bits, as pg_locks shows them */
	gchar      *objid;	/* its low 32 bits */
	gchar      *name;	/* the session's application_name, cut */
	PGconn     *pg;		/* NULL while disconnected */
	gboolean    held;
	gint        backend_pid;	/* of the session holding it; 0 when not held */
	gchar       backend_start[AXIS_DB_BACKEND_START_MAX];
	gboolean    pending;		/* a check is in flight on @pg */
	gint64      pending_sent;	/* when it was sent */
	PGresult   *pending_last;	/* its row set, once read */
};

/* The holder of the lock in this database, if any. */
static const gchar holder_sql[] =
	"SELECT l.pid, a.application_name, host(a.client_addr),"
	" to_char(a.backend_start AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS') || ' UTC'"
	" FROM pg_locks l LEFT JOIN pg_stat_activity a ON a.pid = l.pid"
	" WHERE l.locktype = 'advisory' AND l.granted AND l.objsubid = 1"
	"  AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database())"
	"  AND l.classid = $1::bigint::oid AND l.objid = $2::bigint::oid"
	" ORDER BY l.pid LIMIT 1";

/* Whether the session ($1 its process, $2 its start) holds the lock ($3,
 * $4) in this database. */
#define HOLDS_SQL \
	"EXISTS (SELECT 1 FROM pg_locks l JOIN pg_stat_activity a ON a.pid = l.pid" \
	" WHERE l.locktype = 'advisory' AND l.granted AND l.objsubid = 1 AND l.pid = $1::integer" \
	"  AND " AXIS_DB_BACKEND_START_SQL("a") " = $2::text" \
	"  AND l.database = (SELECT oid FROM pg_database WHERE datname = current_database())" \
	"  AND l.classid = $3::bigint::oid AND l.objid = $4::bigint::oid)"

/* On the lock's own connection: still that session, still holding it. */
static const gchar check_sql[] =
	"SELECT pg_backend_pid() = $1::integer AND " HOLDS_SQL;

/* From another connection. */
static const gchar verify_sql[] =
	"SELECT " HOLDS_SQL;

static const gchar try_sql[] =
	"SELECT pg_try_advisory_lock($1::bigint), pg_backend_pid(),"
	" (SELECT " AXIS_DB_BACKEND_START_SQL("a") " FROM pg_stat_activity a WHERE a.pid = pg_backend_pid())";

static const gchar keepalive_sql[] =
	"SELECT set_config('tcp_keepalives_idle', '" G_STRINGIFY(AXIS_DB_LOCK_KEEPALIVE_IDLE_S) "', false),"
	" set_config('tcp_keepalives_interval', '" G_STRINGIFY(AXIS_DB_LOCK_KEEPALIVE_INTERVAL_S) "', false),"
	" set_config('tcp_keepalives_count', '" G_STRINGIFY(AXIS_DB_LOCK_KEEPALIVE_COUNT) "', false),"
	" set_config('tcp_user_timeout', '" G_STRINGIFY(AXIS_DB_LOCK_USER_TIMEOUT_MS) "', false)";

/*
 * Holders
 */

void
axis_db_lock_holder_free(AxisDbLockHolder *holder)
{
	if (holder == NULL)
		return;

	g_free(holder->application);
	g_free(holder->address);
	g_free(holder->since);
	g_free(holder);
}

gchar *
axis_db_lock_holder_describe(const AxisDbLockHolder *holder)
{
	GString *out;

	if (holder == NULL || holder->pid <= 0)
		return g_strdup("a session the database would not name");

	out = g_string_new(NULL);
	g_string_append_printf(out, "session %d", holder->pid);
	if (holder->application != NULL && *holder->application != '\0')
		g_string_append_printf(out, " (%s)", holder->application);
	else
		g_string_append(out, " (no application name)");
	if (holder->address != NULL)
		g_string_append_printf(out, " from %s", holder->address);
	if (holder->since != NULL)
		g_string_append_printf(out, ", connected since %s", holder->since);

	return g_string_free(out, FALSE);
}

/*
 * Connections
 */

/* libpq's messages on one line, scrubbed of the password. */
static gchar *
lock_message(
	AxisDbLock  *self,
	const gchar *text
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
lock_close(AxisDbLock *self)
{
	if (self->pending_last != NULL) {
		PQclear(self->pending_last);
		self->pending_last = NULL;
	}
	self->pending = FALSE;
	if (self->pg != NULL) {
		PQfinish(self->pg);
		self->pg = NULL;
	}
	self->held = FALSE;
	self->backend_pid = 0;
	self->backend_start[0] = '\0';
}

static void
set_broken(
	AxisDbLock  *self,
	PGconn      *pg,
	GError     **error
){
	g_autofree gchar *text = NULL;

	text = lock_message(self, pg != NULL ? PQerrorMessage(pg) : "not connected");
	g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST, "its connection is gone: %s", text);
}

static void
set_lost(
	AxisDbLock  *self,
	GError     **error
){
	set_broken(self, self->pg, error);
	lock_close(self);
}

typedef enum
{
	READ_DONE,
	READ_TIMEOUT,
	READ_BROKEN
} ReadStatus;

/*
 * Reads every result of the statement in flight on @pg, giving up at
 * @deadline.  *@last keeps the last row set read (it may already hold one
 * from an earlier call that timed out); *@failure the first error.
 */
static ReadStatus
read_results(
	AxisDbLock  *self,
	PGconn      *pg,
	gint64       deadline,
	PGresult   **last,
	gchar      **failure
){
	PGresult *res;

	for (;;) {
		while (PQisBusy(pg)) {
			AxisDbWait wait = axis_db_wait_socket(pg, FALSE, deadline);

			if (wait == AXIS_DB_WAIT_TIMEOUT)
				return READ_TIMEOUT;
			if (wait == AXIS_DB_WAIT_FAILED || !PQconsumeInput(pg))
				break;
		}

		res = PQgetResult(pg);
		if (res == NULL)
			break;
		if (PQresultStatus(res) == PGRES_TUPLES_OK) {
			if (*last != NULL)
				PQclear(*last);
			*last = res;
		} else {
			if (PQresultStatus(res) != PGRES_COMMAND_OK && *failure == NULL)
				*failure = lock_message(self, PQresultErrorMessage(res));
			PQclear(res);
		}
		if (PQstatus(pg) == CONNECTION_BAD)
			break;
	}

	return PQstatus(pg) == CONNECTION_OK ? READ_DONE : READ_BROKEN;
}

/*
 * Runs one statement on @pg and reads every result, giving up at @deadline
 * (AXIS_DB_ERROR_TIMEOUT: the statement is abandoned, @pg's state unknown).
 * Closes nothing.  @result gets the last row set, if any.
 */
static gboolean
conn_run(
	AxisDbLock          *self,
	PGconn              *pg,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	gint64               deadline,
	AxisDbResult       **result,
	GError             **error
){
	g_autofree gchar *failure = NULL;
	PGresult *last = NULL;
	ReadStatus status;

	if (result != NULL)
		*result = NULL;
	if (pg == NULL || PQstatus(pg) != CONNECTION_OK ||
	    !PQsendQueryParams(pg, sql, (int)n_params, NULL, params, NULL, NULL, 0)) {
		set_broken(self, pg, error);
		return FALSE;
	}

	status = read_results(self, pg, deadline, &last, &failure);
	if (status == READ_TIMEOUT) {
		PQclear(last);
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT, "the database did not answer in time");
		return FALSE;
	}
	if (status == READ_BROKEN) {
		PQclear(last);
		set_broken(self, pg, error);
		return FALSE;
	}
	if (failure != NULL) {
		PQclear(last);
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY, "%s", failure);
		return FALSE;
	}

	if (result != NULL && last != NULL)
		*result = axis_db_result_new_take(last);
	else
		PQclear(last);
	return TRUE;
}

/* conn_run() on the lock's connection, closing it when its state is unknown
 * or it broke. */
static gboolean
lock_run(
	AxisDbLock          *self,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	gint64               deadline,
	AxisDbResult       **result,
	GError             **error
){
	g_autoptr(GError) local = NULL;

	if (conn_run(self, self->pg, sql, n_params, params, deadline, result, &local))
		return TRUE;
	if (!g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY))
		lock_close(self);
	g_propagate_error(error, g_steal_pointer(&local));
	return FALSE;
}

static gboolean
lock_connect(
	AxisDbLock  *self,
	gint64       deadline,
	GError     **error
){
	g_autoptr(GError) local = NULL;
	const gchar *params[1];

	lock_close(self);
	self->pg = axis_db_pool_connect_dedicated(self->pool, deadline, error);
	if (self->pg == NULL)
		return FALSE;

	/* Named, so whoever finds the lock taken can say by whom; and idle is
	 * this connection's normal state. */
	params[0] = self->name;
	if (!lock_run(self, "SELECT set_config('application_name', $1, false),"
	              " set_config('idle_session_timeout', '0', false)", 1, params, deadline, NULL, error)) {
		lock_close(self);
		return FALSE;
	}

	/* Ended by the server when this client stops answering, so a lock
	 * whose machine vanished is released within about a minute.  A server
	 * that will not is not a reason to run without the lock. */
	if (!lock_run(self, keepalive_sql, 0, NULL, deadline, NULL, &local)) {
		if (self->pg == NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_debug("database lock: the server would not set TCP keep-alives and the user timeout on the lock's "
			"connection: %s", local->message);
	}

	return TRUE;
}

/*
 * The lock
 */

AxisDbLock *
axis_db_lock_new(
	AxisDbPool  *pool,
	gint64       key,
	const gchar *name
){
	AxisDbLock *self;
	guint64 bits = (guint64)key;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(name != NULL && *name != '\0', NULL);

	self = g_new0(AxisDbLock, 1);
	self->pool = pool;
	self->key = key;
	self->key_text = g_strdup_printf("%" G_GINT64_FORMAT, key);
	self->classid = g_strdup_printf("%u", (guint)(bits >> 32));
	self->objid = g_strdup_printf("%u", (guint)(bits & G_GUINT64_CONSTANT(0xffffffff)));
	self->name = g_strndup(name, AXIS_DB_LOCK_NAME_MAX);

	return self;
}

void
axis_db_lock_free(AxisDbLock *self)
{
	if (self == NULL)
		return;

	lock_close(self);
	g_free(self->key_text);
	g_free(self->classid);
	g_free(self->objid);
	g_free(self->name);
	g_free(self);
}

const gchar *
axis_db_lock_get_name(AxisDbLock *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->name;
}

gint64
axis_db_lock_get_key(AxisDbLock *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->key;
}

gboolean
axis_db_lock_is_held(AxisDbLock *self)
{
	g_return_val_if_fail(self != NULL, FALSE);

	return self->held;
}

gint
axis_db_lock_get_backend_pid(AxisDbLock *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->held ? self->backend_pid : 0;
}

const gchar *
axis_db_lock_get_backend_start(AxisDbLock *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->held ? self->backend_start : NULL;
}

/* Who holds the lock now; *found stays NULL when nobody does. */
static gboolean
read_holder(
	AxisDbLock        *self,
	gint64             deadline,
	AxisDbLockHolder **found,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisDbLockHolder *holder;
	const gchar *params[2];
	gint64 pid = 0;

	*found = NULL;
	params[0] = self->classid;
	params[1] = self->objid;
	if (!lock_run(self, holder_sql, 2, params, deadline, &result, error))
		return FALSE;
	if (result == NULL || axis_db_result_get_n_rows(result) == 0)
		return TRUE;

	holder = g_new0(AxisDbLockHolder, 1);
	if (axis_db_result_get_int64(result, 0, 0, &pid) && pid > 0 && pid <= G_MAXINT)
		holder->pid = (gint)pid;
	holder->application = g_strdup(axis_db_result_get_text(result, 0, 1));
	holder->address = g_strdup(axis_db_result_get_text(result, 0, 2));
	holder->since = g_strdup(axis_db_result_get_text(result, 0, 3));
	*found = holder;
	return TRUE;
}

/*
 * Ends an earlier session of this lock, waiting for it to end for the time
 * left before @deadline (less a margin for the answer).  TRUE when the
 * server says it ended (or, with no time to wait, that it was told to).
 * FALSE with @error set only when the lock's own connection broke.
 */
static gboolean
end_earlier_session(
	AxisDbLock  *self,
	gint         pid,
	gint64       deadline,
	GError     **error
){
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *pid_text = g_strdup_printf("%d", pid);
	g_autofree gchar *wait_text = NULL;
	const gchar *params[2];
	gint64 wait_ms = 0;
	gboolean ended = FALSE;

	if (deadline >= 0)
		wait_ms = MAX((deadline - g_get_monotonic_time()) / 1000 - TERMINATE_MARGIN_MS, 0);
	wait_text = g_strdup_printf("%" G_GINT64_FORMAT, wait_ms);
	params[0] = pid_text;
	params[1] = wait_text;
	if (!lock_run(self, "SELECT pg_terminate_backend($1::integer, $2::bigint)", 2, params, deadline, &result,
	              &local)) {
		if (self->pg == NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_warning("database lock: session %d carries this daemon's name and holds the lock, but it could not be "
			"ended: %s", pid, local->message);
		return FALSE;
	}
	if (result == NULL || !axis_db_result_get_boolean(result, 0, 0, &ended) || !ended) {
		g_warning("database lock: an earlier connection of this daemon (session %d) still holds the lock and did "
			"not end within %" G_GINT64_FORMAT " ms of being told to", pid, wait_ms);
		return FALSE;
	}

	g_warning("database lock: an earlier connection of this daemon (session %d) still held the lock, as the "
		"server had not yet noticed that it was gone; it %s", pid, wait_ms > 0 ? "was ended" : "was told to end");
	return TRUE;
}

AxisDbLockResult
axis_db_lock_try(
	AxisDbLock        *self,
	gint64             deadline,
	AxisDbLockHolder **holder,
	GError           **error
){
	guint round;

	if (holder != NULL)
		*holder = NULL;
	g_return_val_if_fail(self != NULL, AXIS_DB_LOCK_ERROR);

	if (self->held)
		return AXIS_DB_LOCK_TAKEN;
	if ((self->pg == NULL || PQstatus(self->pg) != CONNECTION_OK) && !lock_connect(self, deadline, error))
		return AXIS_DB_LOCK_ERROR;

	for (round = 0; round < 2; round++) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autoptr(AxisDbLockHolder) found = NULL;
		const gchar *params[1];
		const gchar *start;
		gboolean taken = FALSE;
		gint64 pid = 0;

		params[0] = self->key_text;
		if (!lock_run(self, try_sql, 1, params, deadline, &result, error)) {
			lock_close(self);
			return AXIS_DB_LOCK_ERROR;
		}
		start = result != NULL && axis_db_result_get_n_rows(result) == 1 ?
			axis_db_result_get_text(result, 0, 2) : NULL;
		if (result == NULL || axis_db_result_get_n_rows(result) != 1 ||
		    !axis_db_result_get_boolean(result, 0, 0, &taken) ||
		    !axis_db_result_get_int64(result, 0, 1, &pid) || pid <= 0 || pid > G_MAXINT ||
		    start == NULL || strlen(start) >= sizeof(self->backend_start)) {
			/* Closing releases a lock that was taken all the same. */
			lock_close(self);
			g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY,
				"unexpected answer from pg_try_advisory_lock()");
			return AXIS_DB_LOCK_ERROR;
		}
		if (taken) {
			self->held = TRUE;
			self->backend_pid = (gint)pid;
			g_strlcpy(self->backend_start, start, sizeof(self->backend_start));
			return AXIS_DB_LOCK_TAKEN;
		}

		if (!read_holder(self, deadline, &found, error)) {
			lock_close(self);
			return AXIS_DB_LOCK_ERROR;
		}
		/* Released between the two statements: try again. */
		if (found == NULL)
			continue;
		/* A session of this lock's own name is an earlier one of this
		 * lock: never another daemon. */
		if (found->pid > 0 && g_strcmp0(found->application, self->name) == 0) {
			g_autoptr(GError) local = NULL;

			if (round == 0) {
				if (!end_earlier_session(self, found->pid, deadline, &local) && local != NULL) {
					g_propagate_error(error, g_steal_pointer(&local));
					return AXIS_DB_LOCK_ERROR;
				}
				continue;
			}
			return AXIS_DB_LOCK_OURS;
		}
		if (holder != NULL)
			*holder = g_steal_pointer(&found);
		return AXIS_DB_LOCK_BUSY;
	}

	return AXIS_DB_LOCK_BUSY;
}

/* Waits for @wake_fd alone (no connection to watch). */
static AxisDbLockWait
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
				return AXIS_DB_LOCK_TIMEOUT;
			remaining = (deadline - now + 999) / 1000;
			timeout_ms = remaining > G_MAXINT ? G_MAXINT : (int)remaining;
		}

		if (wake_fd < 0) {
			if (timeout_ms < 0)
				return AXIS_DB_LOCK_TIMEOUT;
			g_usleep((gulong)timeout_ms * 1000);
			continue;
		}

		pfd.fd = wake_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		rc = poll(&pfd, 1, timeout_ms);
		if (rc > 0)
			return AXIS_DB_LOCK_WOKEN;
		if (rc < 0 && errno != EINTR)
			return AXIS_DB_LOCK_WOKEN;	/* let the caller look */
	}
}

AxisDbLockWait
axis_db_lock_wait(
	AxisDbLock  *self,
	gint         wake_fd,
	gint64       deadline,
	GError     **error
){
	g_return_val_if_fail(self != NULL, AXIS_DB_LOCK_TIMEOUT);

	if (self->pg == NULL || !self->held)
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
				return AXIS_DB_LOCK_TIMEOUT;
			remaining = (deadline - now + 999) / 1000;
			timeout_ms = remaining > G_MAXINT ? G_MAXINT : (int)remaining;
		}

		pfd[0].fd = PQsocket(self->pg);
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		if (pfd[0].fd < 0) {
			set_lost(self, error);
			return AXIS_DB_LOCK_LOST;
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
			return AXIS_DB_LOCK_LOST;
		}
		if (rc == 0)
			continue;	/* the deadline is checked at the top */

		if (pfd[0].revents != 0) {
			/* The server says something only to end the session, to
			 * send a notice (the pool's receiver logs it), or to answer
			 * a check in flight (kept by libpq for the next check). */
			if (!PQconsumeInput(self->pg) || PQstatus(self->pg) != CONNECTION_OK) {
				set_lost(self, error);
				return AXIS_DB_LOCK_LOST;
			}
			/* An answer read in full leaves nothing more to read: wait
			 * on the clock and the pipe until the next check takes it. */
			if (self->pending && !PQisBusy(self->pg)) {
				AxisDbLockWait wait = wait_wake_only(wake_fd, deadline);

				return wait;
			}
			continue;
		}
		if (n == 2 && pfd[1].revents != 0)
			return AXIS_DB_LOCK_WOKEN;
	}
}

static void
held_params(
	AxisDbLock   *self,
	gchar         pid_text[16],
	const gchar **params
){
	g_snprintf(pid_text, 16, "%d", self->backend_pid);
	params[0] = pid_text;
	params[1] = self->backend_start;
	params[2] = self->classid;
	params[3] = self->objid;
}

AxisDbLockState
axis_db_lock_check(
	AxisDbLock  *self,
	gint64       deadline,
	gint64      *sent,
	GError     **error
){
	g_autofree gchar *failure = NULL;
	gchar pid_text[16];
	const gchar *params[4];
	ReadStatus status;
	gboolean held = FALSE;

	g_return_val_if_fail(self != NULL, AXIS_DB_LOCK_GONE);

	if (!self->held || self->pg == NULL) {
		g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST, "the daemon lock is not held");
		return AXIS_DB_LOCK_GONE;
	}

	if (!self->pending) {
		held_params(self, pid_text, params);
		if (!PQsendQueryParams(self->pg, check_sql, 4, NULL, params, NULL, NULL, 0)) {
			set_lost(self, error);
			return AXIS_DB_LOCK_GONE;
		}
		self->pending = TRUE;
		self->pending_sent = g_get_monotonic_time();
	}

	status = read_results(self, self->pg, deadline, &self->pending_last, &failure);
	if (status == READ_TIMEOUT)
		return AXIS_DB_LOCK_SILENT;

	self->pending = FALSE;
	if (status == READ_BROKEN) {
		set_lost(self, error);
		return AXIS_DB_LOCK_GONE;
	}
	if (failure != NULL || self->pending_last == NULL || PQntuples(self->pending_last) != 1 ||
	    PQgetisnull(self->pending_last, 0, 0)) {
		/* The server answered, but not the question: nothing is known
		 * about the lock (the caller can ask on another connection). */
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY, "the lock's check failed: %s",
			failure != NULL ? failure : "no answer");
		if (self->pending_last != NULL) {
			PQclear(self->pending_last);
			self->pending_last = NULL;
		}
		return AXIS_DB_LOCK_SILENT;
	}
	held = strcmp(PQgetvalue(self->pending_last, 0, 0), "t") == 0;
	PQclear(self->pending_last);
	self->pending_last = NULL;
	if (!held) {
		lock_close(self);
		g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST,
			"the server session behind the daemon lock's connection no longer holds the lock (is a "
			"connection pooler in transaction mode between Axis and PostgreSQL?)");
		return AXIS_DB_LOCK_GONE;
	}

	if (sent != NULL)
		*sent = self->pending_sent;
	return AXIS_DB_LOCK_HELD;
}

AxisDbLockState
axis_db_lock_verify(
	AxisDbLock  *self,
	gint64       deadline,
	gint64      *sent,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	gchar pid_text[16];
	const gchar *params[4];
	gint64 at = g_get_monotonic_time();
	gboolean held = FALSE;
	gboolean ok;
	PGconn *pg;

	g_return_val_if_fail(self != NULL, AXIS_DB_LOCK_GONE);

	if (!self->held) {
		g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST, "the daemon lock is not held");
		return AXIS_DB_LOCK_GONE;
	}

	pg = axis_db_pool_connect_dedicated(self->pool, deadline, &local);
	if (pg == NULL) {
		g_propagate_prefixed_error(error, g_steal_pointer(&local), "cannot ask on another connection: ");
		return AXIS_DB_LOCK_SILENT;
	}
	held_params(self, pid_text, params);
	ok = conn_run(self, pg, verify_sql, 4, params, deadline, &result, &local);
	PQfinish(pg);
	if (!ok || result == NULL || axis_db_result_get_n_rows(result) != 1 ||
	    !axis_db_result_get_boolean(result, 0, 0, &held)) {
		if (local != NULL)
			g_propagate_prefixed_error(error, g_steal_pointer(&local), "no answer on another connection: ");
		else
			g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY, "no answer on another connection");
		return AXIS_DB_LOCK_SILENT;
	}
	if (!held) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST,
			"the server says that the daemon lock's session (process %d, started %s UTC) no longer holds the "
			"lock", self->backend_pid, self->backend_start);
		return AXIS_DB_LOCK_GONE;
	}

	if (sent != NULL)
		*sent = at;
	return AXIS_DB_LOCK_HELD;
}

void
axis_db_lock_release(AxisDbLock *self)
{
	g_return_if_fail(self != NULL);

	lock_close(self);
}
