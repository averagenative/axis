/*
 * axis-db-pool.c - a fixed-size pool of PostgreSQL connections
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Locking.  One mutex guards the free stack, the waiter queue, the
 * counters, the settings and each slot's in_use/releasing/owner fields.
 * Nothing that talks to the server (connect, validate, roll back) runs
 * with it held.
 *
 * Hand-over.  A released connection goes straight to the oldest waiter,
 * if there is one, without passing through the free stack; so waiters are
 * served strictly in arrival order, a newcomer can never take a connection
 * from under someone who has been waiting, and the free stack is non-empty
 * only while nobody waits.  Each waiter has its own condition variable and
 * is signalled exactly once, by the thread that hands it a connection, so
 * no wake-up is lost or wasted.  A waiter re-checks its slot after every
 * wake-up (spurious ones included), and after a timeout it still takes a
 * connection handed over at the last moment rather than dropping it.
 */

#include "db/axis-db-pool.h"
#include "db/axis-db-private.h"
#include "core/axis-config.h"

#include <poll.h>
#include <string.h>

/* How long the round trip that validates an idle connection, and the
 * rollback on release, may take before the connection is given up. */
#define CHECK_TIMEOUT_US (5 * G_USEC_PER_SEC)

/* How often a thread that waits for a slot looks whether its work was
 * called off (AxisDbInterrupt). */
#define INTERRUPT_LOOK_US (50 * 1000)

typedef struct
{
	GList       link;	/* in AxisDbPool.waiters; data is the Waiter */
	GCond       cond;
	AxisDbConn *conn;	/* set by the thread that hands one over */
} Waiter;

struct _AxisDbPool
{
	GMutex       mutex;
	AxisDbConn  *slots;		/* size entries, never moved */
	AxisDbConn **free;		/* stack of free slots, top at n_free - 1 */
	guint        n_free;
	guint        n_in_use;
	GQueue       waiters;		/* of Waiter, oldest first */
	guint        acquire_timeout_ms;
	guint        validation_interval_ms;
	gint         connections_opened;	/* atomic */
	gint         statements_sent;		/* atomic */

	/* Fixed at creation */
	guint        size;
	gchar       *url;
	gchar       *redacted_url;
	gchar       *password;		/* to scrub from messages; NULL if none */
	gint64       connect_timeout_us;	/* per attempt; -1 for none */
};

/*
 * Messages
 */

static gchar *
scrub_secret(
	const gchar *text,
	const gchar *secret
){
	GString *out;
	const gchar *p;
	const gchar *hit;
	gsize secret_len;

	if (text == NULL)
		return NULL;
	if (secret == NULL || *secret == '\0')
		return g_strdup(text);

	out = g_string_new(NULL);
	secret_len = strlen(secret);
	p = text;
	while ((hit = strstr(p, secret)) != NULL) {
		g_string_append_len(out, p, (gssize)(hit - p));
		g_string_append(out, "****");
		p = hit + secret_len;
	}
	g_string_append(out, p);

	return g_string_free(out, FALSE);
}

gchar *
axis_db_pool_scrub(
	AxisDbPool  *self,
	const gchar *text
){
	return scrub_secret(text, self != NULL ? self->password : NULL);
}

const gchar *
axis_db_pool_get_redacted_url(AxisDbPool *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->redacted_url;
}

/*
 * When libpq cannot parse a URL we do not know which part is the
 * password, and its message quotes the offending part ("invalid
 * percent-encoded token: \"...\"").  Every quoted part goes.
 */
static gchar *
hide_quoted(const gchar *text)
{
	GString *out = g_string_new(NULL);
	gboolean quoted = FALSE;
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if (*p == '"') {
			if (!quoted)
				g_string_append(out, "\"...\"");
			quoted = !quoted;
			continue;
		}
		if (!quoted)
			g_string_append_c(out, *p);
	}

	return g_strstrip(g_string_free(out, FALSE));
}

/* One line, as in axis-db-conn.c. */
static gchar *
libpq_message(const gchar *text)
{
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *joined = NULL;
	guint i;

	lines = g_strsplit(text != NULL ? text : "", "\n", -1);
	for (i = 0; lines[i] != NULL; i++)
		g_strstrip(lines[i]);
	joined = g_strjoinv(" ", lines);

	return g_strdup(g_strstrip(joined));
}

/*
 * Server versions
 */

gchar *
axis_db_format_server_version(gint server_version)
{
	if (server_version <= 0)
		return g_strdup("unknown");

	/* 10 and later: major * 10000 + minor.  Before: three parts. */
	if (server_version >= 100000)
		return g_strdup_printf("%d.%d", server_version / 10000, server_version % 10000);

	return g_strdup_printf("%d.%d.%d", server_version / 10000,
		(server_version / 100) % 100, server_version % 100);
}

gboolean
axis_db_check_server_version(
	gint     server_version,
	GError **error
){
	g_autofree gchar *found = NULL;

	if (server_version >= AXIS_DB_MIN_SERVER_VERSION)
		return TRUE;

	found = axis_db_format_server_version(server_version);
	g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_VERSION,
		"PostgreSQL %s is not supported: Axis requires PostgreSQL %d or newer",
		found, AXIS_DB_MIN_SERVER_VERSION / 10000);
	return FALSE;
}

/*
 * Connections
 */

/* Server notices (and the FATAL a server sends before closing an idle
 * session) go to the log instead of libpq's default, stderr. */
static void
on_notice(
	void           *arg,
	const PGresult *res
){
	AxisDbPool *self = arg;
	const gchar *severity = PQresultErrorField(res, PG_DIAG_SEVERITY_NONLOCALIZED);
	const gchar *primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	g_autofree gchar *text = NULL;
	g_autofree gchar *clean = NULL;

	text = libpq_message(primary != NULL ? primary : PQresultErrorMessage(res));
	clean = axis_db_pool_scrub(self, text);
	if (severity == NULL)
		severity = "NOTICE";

	if (strcmp(severity, "WARNING") == 0 || strcmp(severity, "FATAL") == 0 ||
	    strcmp(severity, "PANIC") == 0)
		g_message("database %s: %s", severity, clean);
	else
		g_debug("database %s: %s", severity, clean);
}

static const gchar settings_sql[] =
	"SELECT set_config('TimeZone', 'UTC', false),"
	" set_config('statement_timeout', $1, false),"
	" set_config('idle_in_transaction_session_timeout', $2, false)";

/* Applies the session settings to a fresh connection within @deadline.
 * Returns NULL on success, or what went wrong. */
static gchar *
apply_settings(
	PGconn *pg,
	gint64  deadline
){
	g_autofree gchar *statement_timeout = g_strdup_printf("%d", AXIS_DB_STATEMENT_TIMEOUT_MS);
	g_autofree gchar *idle_timeout = g_strdup_printf("%d", AXIS_DB_IDLE_IN_TRANSACTION_TIMEOUT_MS);
	const gchar *params[2];
	gchar *failure = NULL;

	params[0] = statement_timeout;
	params[1] = idle_timeout;
	if (!PQsendQueryParams(pg, settings_sql, 2, NULL, params, NULL, NULL, 0))
		return libpq_message(PQerrorMessage(pg));

	for (;;) {
		PGresult *res;

		while (PQisBusy(pg)) {
			AxisDbWait wait = axis_db_wait_socket(pg, FALSE, deadline);

			if (wait == AXIS_DB_WAIT_TIMEOUT) {
				g_free(failure);
				return g_strdup("no answer in time");
			}
			if (wait == AXIS_DB_WAIT_FAILED || !PQconsumeInput(pg))
				break;
		}

		res = PQgetResult(pg);
		if (res == NULL)
			break;
		if (PQresultStatus(res) != PGRES_TUPLES_OK && failure == NULL)
			failure = libpq_message(PQresultErrorMessage(res));
		PQclear(res);
		if (PQstatus(pg) == CONNECTION_BAD)
			break;
	}

	if (failure == NULL && PQstatus(pg) != CONNECTION_OK)
		failure = libpq_message(PQerrorMessage(pg));

	return failure;
}

/* The deadline for one connection attempt: the pool's connect timeout,
 * cut short by @work_deadline if that comes first (-1: none). */
static gint64
connect_deadline(
	AxisDbPool *self,
	gint64      work_deadline
){
	gint64 deadline = -1;

	if (self->connect_timeout_us >= 0)
		deadline = g_get_monotonic_time() + self->connect_timeout_us;
	if (work_deadline >= 0 && (deadline < 0 || work_deadline < deadline))
		deadline = work_deadline;

	return deadline;
}

/*
 * Opens one server connection with the session settings applied, giving
 * up at @deadline (-1: never).  libpq's non-blocking connect is driven
 * here, so the deadline covers everything, the TLS and authentication
 * exchanges included; libpq ignores connect_timeout on this path, and the
 * pool applies it instead.
 */
static PGconn *
pool_connect_counted(
	AxisDbPool  *self,
	gint64       deadline,
	gboolean     counted,
	GError     **error
){
	const gchar *keywords[4];
	const gchar *values[4];
	g_autofree gchar *text = NULL;
	g_autofree gchar *clean = NULL;
	g_autoptr(GError) local = NULL;
	PostgresPollingStatusType polling = PGRES_POLLING_WRITING;
	gint64 start = g_get_monotonic_time();
	PGconn *pg;

	/* The URL comes first; what follows overrides it. */
	keywords[0] = "dbname";
	values[0] = self->url;
	keywords[1] = "application_name";
	values[1] = "axis";
	keywords[2] = "client_encoding";
	values[2] = "UTF8";
	keywords[3] = NULL;
	values[3] = NULL;

	pg = PQconnectStartParams(keywords, values, 1);
	if (pg == NULL) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"cannot connect to the database at %s: out of memory", self->redacted_url);
		return NULL;
	}
	/* Before the handshake, so a warning sent while connecting is
	 * logged rather than printed. */
	PQsetNoticeReceiver(pg, on_notice, self);

	while (PQstatus(pg) != CONNECTION_BAD && polling != PGRES_POLLING_OK &&
	       polling != PGRES_POLLING_FAILED) {
		AxisDbWait wait = axis_db_wait_socket(pg, polling == PGRES_POLLING_WRITING, deadline);

		if (wait == AXIS_DB_WAIT_TIMEOUT) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
				"cannot connect to the database at %s: no answer within %" G_GINT64_FORMAT " ms",
				self->redacted_url, (g_get_monotonic_time() - start) / 1000);
			PQfinish(pg);
			return NULL;
		}
		if (wait == AXIS_DB_WAIT_FAILED)
			break;

		polling = PQconnectPoll(pg);
	}

	if (polling != PGRES_POLLING_OK || PQstatus(pg) != CONNECTION_OK) {
		text = libpq_message(PQerrorMessage(pg));
		clean = axis_db_pool_scrub(self, *text != '\0' ? text : "connection failed");
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"cannot connect to the database at %s: %s", self->redacted_url, clean);
		PQfinish(pg);
		return NULL;
	}

	if (!axis_db_check_server_version(PQserverVersion(pg), &local)) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_VERSION,
			"the database at %s: %s", self->redacted_url, local->message);
		PQfinish(pg);
		return NULL;
	}

	text = apply_settings(pg, deadline);
	if (text != NULL) {
		clean = axis_db_pool_scrub(self, text);
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"cannot apply the session settings on the database at %s: %s",
			self->redacted_url, clean);
		PQfinish(pg);
		return NULL;
	}

	if (counted)
		g_atomic_int_inc(&self->connections_opened);
	return pg;
}

static PGconn *
pool_connect(
	AxisDbPool  *self,
	gint64       deadline,
	GError     **error
){
	return pool_connect_counted(self, deadline, TRUE, error);
}

PGconn *
axis_db_pool_connect_dedicated(
	AxisDbPool  *self,
	gint64       deadline,
	GError     **error
){
	g_return_val_if_fail(self != NULL, NULL);

	return pool_connect_counted(self, connect_deadline(self, deadline), FALSE, error);
}

/* Something to read on an idle connection: the server has closed it, or
 * said something (a notice, or the FATAL before it closes). */
static gboolean
socket_has_input(PGconn *pg)
{
	struct pollfd pfd;

	pfd.fd = PQsocket(pg);
	if (pfd.fd < 0)
		return TRUE;
	pfd.events = POLLIN;
	pfd.revents = 0;

	return poll(&pfd, 1, 0) > 0;
}

/* Whether an idle connection still works.  @work_deadline bounds the
 * round trip, or -1 for the default. */
static gboolean
conn_is_alive(
	AxisDbPool *self,
	AxisDbConn *conn,
	gint64      work_deadline
){
	g_autoptr(GError) local = NULL;
	gboolean must_check;
	gint64 now;
	gint64 deadline;
	guint interval_ms;

	if (PQstatus(conn->pg) != CONNECTION_OK)
		return FALSE;
	if (PQtransactionStatus(conn->pg) != PQTRANS_IDLE)
		return FALSE;

	must_check = socket_has_input(conn->pg);
	if (must_check && (!PQconsumeInput(conn->pg) || PQstatus(conn->pg) != CONNECTION_OK))
		return FALSE;

	g_mutex_lock(&self->mutex);
	interval_ms = self->validation_interval_ms;
	g_mutex_unlock(&self->mutex);

	now = g_get_monotonic_time();
	if (!must_check && now - conn->idle_since < (gint64)interval_ms * 1000)
		return TRUE;

	deadline = now + CHECK_TIMEOUT_US;
	if (work_deadline >= 0 && work_deadline < deadline)
		deadline = work_deadline;

	if (!axis_db_conn_run(conn, "SELECT 1", 0, NULL, FALSE, deadline, NULL, &local)) {
		g_debug("database connection %u failed its check: %s", conn->index + 1, local->message);
		return FALSE;
	}

	return TRUE;
}

/* Makes a just-acquired connection usable: checks it, and opens it (once)
 * if it is closed or dead. */
static gboolean
conn_prepare(
	AxisDbPool  *self,
	AxisDbConn  *conn,
	gint64       work_deadline,
	GError     **error
){
	if (conn->pg != NULL && !conn_is_alive(self, conn, work_deadline)) {
		g_warning("database connection %u is no longer usable (closed by the server?); reconnecting",
			conn->index + 1);
		PQfinish(conn->pg);
		conn->pg = NULL;
	}

	if (conn->pg == NULL) {
		conn->pg = pool_connect(self, connect_deadline(self, work_deadline), error);
		if (conn->pg == NULL)
			return FALSE;
	}

	conn->close_on_release = FALSE;
	return TRUE;
}

/*
 * The pool
 */

AxisDbPool *
axis_db_pool_new(
	const gchar  *url,
	guint         size,
	GError      **error
){
	g_autoptr(AxisDbPool) self = NULL;
	PQconninfoOption *options;
	PQconninfoOption *option;
	gchar *connect_timeout = NULL;
	gchar *parse_error = NULL;
	gint64 seconds;
	guint i;

	g_return_val_if_fail(url != NULL, NULL);
	g_return_val_if_fail(size >= AXIS_DB_POOL_MIN_SIZE && size <= AXIS_DB_POOL_MAX_SIZE, NULL);

	if (!PQisthreadsafe()) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"this libpq was built without thread safety");
		return NULL;
	}

	options = PQconninfoParse(url, &parse_error);
	if (options == NULL) {
		g_autofree gchar *redacted = axis_config_redact_url(url);
		g_autofree gchar *text = libpq_message(parse_error != NULL ? parse_error : "out of memory");
		g_autofree gchar *detail = hide_quoted(text);

		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"invalid database URL %s: %s", redacted, detail);
		PQfreemem(parse_error);
		return NULL;
	}

	self = g_new0(AxisDbPool, 1);
	g_mutex_init(&self->mutex);
	g_queue_init(&self->waiters);
	self->size = size;
	self->acquire_timeout_ms = AXIS_DB_POOL_DEFAULT_ACQUIRE_TIMEOUT_MS;
	self->validation_interval_ms = AXIS_DB_POOL_DEFAULT_VALIDATION_INTERVAL_MS;
	self->url = g_strdup(url);
	self->redacted_url = axis_config_redact_url(url);

	/* connect_timeout as libpq reads it (URL, else PGCONNECT_TIMEOUT):
	 * zero or less is no limit, 1 is 2; unset or unreadable is our
	 * default. */
	for (option = options; option->keyword != NULL; option++) {
		if (option->val == NULL || *option->val == '\0')
			continue;
		if (strcmp(option->keyword, "password") == 0)
			self->password = g_strdup(option->val);
	}
	for (option = options; option->keyword != NULL; option++) {
		if (strcmp(option->keyword, "connect_timeout") == 0 && option->val != NULL)
			connect_timeout = g_strdup(option->val);
	}
	PQconninfoFree(options);
	if (connect_timeout == NULL && g_getenv("PGCONNECT_TIMEOUT") != NULL)
		connect_timeout = g_strdup(g_getenv("PGCONNECT_TIMEOUT"));

	self->connect_timeout_us = (gint64)AXIS_DB_DEFAULT_CONNECT_TIMEOUT_S * G_USEC_PER_SEC;
	if (connect_timeout != NULL &&
	    g_ascii_string_to_signed(connect_timeout, 10, G_MININT32, G_MAXINT32, &seconds, NULL)) {
		if (seconds <= 0)
			self->connect_timeout_us = -1;
		else
			self->connect_timeout_us = MAX(seconds, 2) * G_USEC_PER_SEC;
	}
	g_free(connect_timeout);

	self->slots = g_new0(AxisDbConn, size);
	self->free = g_new0(AxisDbConn *, size);
	for (i = 0; i < size; i++) {
		self->slots[i].pool = self;
		self->slots[i].index = i;
	}
	/* Slot 0 on top: it is the one connected below. */
	for (i = size; i > 0; i--)
		self->free[self->n_free++] = &self->slots[i - 1];

	self->slots[0].pg = pool_connect(self, connect_deadline(self, -1), error);
	if (self->slots[0].pg == NULL)
		return NULL;
	self->slots[0].idle_since = g_get_monotonic_time();

	return g_steal_pointer(&self);
}

void
axis_db_pool_free(AxisDbPool *self)
{
	guint i;

	if (self == NULL)
		return;

	g_mutex_lock(&self->mutex);
	if (self->n_in_use > 0 || !g_queue_is_empty(&self->waiters)) {
		g_critical("database pool freed while in use (%u connections held, %u callers waiting); leaking it",
			self->n_in_use, g_queue_get_length(&self->waiters));
		g_mutex_unlock(&self->mutex);
		return;
	}
	g_mutex_unlock(&self->mutex);

	for (i = 0; i < self->size; i++) {
		if (self->slots[i].pg != NULL)
			PQfinish(self->slots[i].pg);
	}

	g_free(self->slots);
	g_free(self->free);
	g_free(self->url);
	g_free(self->redacted_url);
	g_free(self->password);
	g_mutex_clear(&self->mutex);
	g_free(self);
}

/* Takes a slot off the free stack, preferring the most recently released
 * one that is still connected. */
static AxisDbConn *
take_free_locked(AxisDbPool *self)
{
	AxisDbConn *conn;
	guint i;

	g_assert(self->n_free > 0);

	for (i = self->n_free; i > 0; i--) {
		if (self->free[i - 1]->pg != NULL)
			break;
	}
	if (i == 0)
		i = self->n_free;

	conn = self->free[i - 1];
	memmove(&self->free[i - 1], &self->free[i],
		(self->n_free - i) * sizeof(self->free[0]));
	self->n_free--;

	conn->in_use = TRUE;
	self->n_in_use++;
	return conn;
}

/* Gives a held slot to the oldest waiter, or back to the free stack. */
static void
give_locked(
	AxisDbPool *self,
	AxisDbConn *conn
){
	GList *link;

	g_atomic_pointer_set(&conn->owner, NULL);
	conn->releasing = FALSE;

	link = g_queue_pop_head_link(&self->waiters);
	if (link != NULL) {
		Waiter *waiter = link->data;

		/* Still in use: it changes hands without being free. */
		waiter->conn = conn;
		g_cond_signal(&waiter->cond);
		return;
	}

	conn->in_use = FALSE;
	self->n_in_use--;
	g_assert(self->n_free < self->size);
	self->free[self->n_free++] = conn;
}

/*
 * @wait_deadline bounds the wait for a free slot; @work_deadline bounds
 * the check of an idle connection (-1: the default).  Both are
 * g_get_monotonic_time() values.
 */
static AxisDbConn *
pool_acquire(
	AxisDbPool  *self,
	gint64       wait_deadline,
	gint64       work_deadline,
	GError     **error
){
	AxisDbConn *conn = NULL;
	gint64 start = g_get_monotonic_time();
	gboolean interruptible = axis_db_thread_is_interruptible();
	guint size;

#ifdef AXIS_DEBUG
	if (g_main_context_is_owner(g_main_context_default()))
		g_warning("axis_db_pool_acquire() called on the main loop's thread: "
			"database calls block; run them on a worker thread");
#endif

	g_mutex_lock(&self->mutex);
	size = self->size;

	if (g_queue_is_empty(&self->waiters) && self->n_free > 0) {
		conn = take_free_locked(self);
	} else {
		Waiter waiter;

		memset(&waiter, 0, sizeof(waiter));
		waiter.link.data = &waiter;
		g_cond_init(&waiter.cond);
		g_queue_push_tail_link(&self->waiters, &waiter.link);

		while (waiter.conn == NULL) {
			gint64 until = wait_deadline;

			/* A thread whose work can be called off (AxisDbInterrupt) looks
			 * for that while it waits, and gives up as at the deadline. */
			if (interruptible)
				until = MIN(until, g_get_monotonic_time() + INTERRUPT_LOOK_US);
			if (!g_cond_wait_until(&waiter.cond, &self->mutex, until) && waiter.conn == NULL &&
			    (until == wait_deadline || axis_db_thread_is_interrupted())) {
				g_queue_unlink(&self->waiters, &waiter.link);
				break;
			}
		}
		conn = waiter.conn;

		/* Whoever handed over a connection did so with the mutex held
		 * and has finished with the waiter. */
		g_cond_clear(&waiter.cond);
	}

	if (conn != NULL)
		g_atomic_pointer_set(&conn->owner, g_thread_self());
	g_mutex_unlock(&self->mutex);

	if (conn == NULL) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT,
			"no database connection became free within %" G_GINT64_FORMAT " ms (all %u are in use)",
			(g_get_monotonic_time() - start) / 1000, size);
		return NULL;
	}

	if (!conn_prepare(self, conn, work_deadline, error)) {
		g_mutex_lock(&self->mutex);
		give_locked(self, conn);
		g_mutex_unlock(&self->mutex);
		return NULL;
	}

	return conn;
}

AxisDbConn *
axis_db_pool_acquire(
	AxisDbPool  *self,
	GError     **error
){
	guint timeout_ms;

	g_return_val_if_fail(self != NULL, NULL);

	g_mutex_lock(&self->mutex);
	timeout_ms = self->acquire_timeout_ms;
	g_mutex_unlock(&self->mutex);

	return pool_acquire(self, g_get_monotonic_time() + (gint64)timeout_ms * 1000, -1, error);
}

AxisDbConn *
axis_db_pool_acquire_until(
	AxisDbPool  *self,
	gint64       deadline,
	GError     **error
){
	g_return_val_if_fail(self != NULL, NULL);

	return pool_acquire(self, deadline, deadline, error);
}

void
axis_db_pool_release(
	AxisDbPool *self,
	AxisDbConn *conn
){
	g_autoptr(GError) local = NULL;
	PGTransactionStatusType status;

	g_return_if_fail(self != NULL);
	g_return_if_fail(conn != NULL);

	if (conn->pool != self) {
		g_critical("database connection released to a pool it does not belong to");
		return;
	}

	g_mutex_lock(&self->mutex);
	if (!conn->in_use || conn->releasing ||
	    g_atomic_pointer_get(&conn->owner) != (gpointer)g_thread_self()) {
		g_mutex_unlock(&self->mutex);
		g_critical("database connection %u released by a thread that does not hold it "
			"(released twice?); ignored", conn->index + 1);
		return;
	}
	conn->releasing = TRUE;
	g_mutex_unlock(&self->mutex);

	/* Still ours (owner is unchanged until give_locked()), so the
	 * rollback below may use it. */
	if (conn->pg != NULL) {
		if (PQstatus(conn->pg) != CONNECTION_OK) {
			conn->close_on_release = TRUE;
		} else {
			status = PQtransactionStatus(conn->pg);
			if (status == PQTRANS_INTRANS || status == PQTRANS_INERROR) {
				g_critical("database connection %u released with %s; rolling it back",
					conn->index + 1,
					status == PQTRANS_INERROR ? "a failed transaction" : "a transaction open");
				if (!axis_db_conn_run(conn, "ROLLBACK", 0, NULL, FALSE,
				                      g_get_monotonic_time() + CHECK_TIMEOUT_US, NULL, &local)) {
					g_warning("database connection %u: rollback failed, closing it: %s",
						conn->index + 1, local->message);
					conn->close_on_release = TRUE;
				}
			} else if (status != PQTRANS_IDLE) {
				/* A statement abandoned at its deadline is still
				 * running, or the state is unknown. */
				conn->close_on_release = TRUE;
			}
		}

		if (conn->close_on_release) {
			PQfinish(conn->pg);
			conn->pg = NULL;
		}
	}
	conn->close_on_release = FALSE;
	conn->idle_since = g_get_monotonic_time();

	g_mutex_lock(&self->mutex);
	give_locked(self, conn);
	g_mutex_unlock(&self->mutex);
}

gboolean
axis_db_pool_exec(
	AxisDbPool         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	AxisDbResult      **result,
	GError            **error
){
	AxisDbConn *conn;
	gboolean ok;

	if (result != NULL)
		*result = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	conn = axis_db_pool_acquire(self, error);
	if (conn == NULL)
		return FALSE;

	ok = axis_db_conn_exec(conn, sql, n_params, params, result, error);
	axis_db_pool_release(self, conn);

	return ok;
}

gboolean
axis_db_pool_ping(
	AxisDbPool  *self,
	guint        timeout_ms,
	GError     **error
){
	return axis_db_pool_query_bounded(self, "SELECT 1", timeout_ms, NULL, error);
}

gboolean
axis_db_pool_query_bounded(
	AxisDbPool    *self,
	const gchar   *sql,
	guint          timeout_ms,
	AxisDbResult **result,
	GError       **error
){
	AxisDbConn *conn;
	gint64 deadline;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(sql != NULL, FALSE);

	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	conn = pool_acquire(self, deadline, deadline, error);
	if (conn == NULL)
		return FALSE;

	ok = axis_db_conn_run(conn, sql, 0, NULL, FALSE, deadline, result, error);
	axis_db_pool_release(self, conn);

	return ok;
}

gboolean
axis_db_url_check(
	const gchar  *url,
	GError      **error
){
	PQconninfoOption *options;
	gchar *parse_error = NULL;

	g_return_val_if_fail(url != NULL, FALSE);

	options = PQconninfoParse(url, &parse_error);
	if (options == NULL) {
		g_autofree gchar *redacted = axis_config_redact_url(url);
		g_autofree gchar *text = libpq_message(parse_error != NULL ? parse_error : "out of memory");
		g_autofree gchar *detail = hide_quoted(text);

		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT,
			"invalid database URL %s: %s", redacted, detail);
		PQfreemem(parse_error);
		return FALSE;
	}
	PQconninfoFree(options);
	return TRUE;
}

void
axis_db_pool_set_acquire_timeout(
	AxisDbPool *self,
	guint       timeout_ms
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->mutex);
	self->acquire_timeout_ms = timeout_ms;
	g_mutex_unlock(&self->mutex);
}

void
axis_db_pool_set_validation_interval(
	AxisDbPool *self,
	guint       interval_ms
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->mutex);
	self->validation_interval_ms = interval_ms;
	g_mutex_unlock(&self->mutex);
}

guint
axis_db_pool_get_size(AxisDbPool *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->size;
}

guint
axis_db_pool_get_connections_opened(AxisDbPool *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return (guint)g_atomic_int_get(&self->connections_opened);
}

guint
axis_db_pool_get_statements_sent(AxisDbPool *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return (guint)g_atomic_int_get(&self->statements_sent);
}

void
axis_db_pool_count_statement(AxisDbPool *self)
{
	if (self != NULL)
		g_atomic_int_inc(&self->statements_sent);
}

guint
axis_db_pool_get_n_in_use(AxisDbPool *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->mutex);
	n = self->n_in_use;
	g_mutex_unlock(&self->mutex);

	return n;
}

guint
axis_db_pool_get_n_waiting(AxisDbPool *self)
{
	guint n;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->mutex);
	n = g_queue_get_length(&self->waiters);
	g_mutex_unlock(&self->mutex);

	return n;
}
