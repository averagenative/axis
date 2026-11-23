/*
 * axis-db-conn.c - one pooled PostgreSQL connection
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every statement goes through axis_db_conn_run(), which drives libpq's
 * asynchronous calls itself so that a wait can have a deadline.  Without
 * a deadline it behaves as PQexecParams() (or PQexec() for a migration
 * script): it reads every result up to the end of the command, keeps the
 * first error, and stops early when the connection breaks or enters COPY.
 */

#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "db/axis-db-private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>

/* Without its pipe an interrupt is noticed by looking this often. */
#define INTERRUPT_LOOK_MS 50

struct _AxisDbInterrupt
{
	gint raised;	/* atomic; never lowered */
	gint wake[2];	/* a pipe: readable once raised; -1 when it could not be made */
};

/* The interrupt the calling thread's waits end by: borrowed. */
static GPrivate thread_interrupt;

AxisDbInterrupt *
axis_db_interrupt_new(void)
{
	AxisDbInterrupt *self = g_new0(AxisDbInterrupt, 1);
	g_autoptr(GError) error = NULL;

	if (!g_unix_open_pipe(self->wake, O_CLOEXEC, &error)) {
		g_debug("no pipe for a database interrupt, which is looked at every %d ms instead: %s", INTERRUPT_LOOK_MS,
			error->message);
		self->wake[0] = -1;
		self->wake[1] = -1;
	}
	return self;
}

void
axis_db_interrupt_free(AxisDbInterrupt *self)
{
	if (self == NULL)
		return;
	if (self->wake[0] >= 0)
		close(self->wake[0]);
	if (self->wake[1] >= 0)
		close(self->wake[1]);
	g_free(self);
}

void
axis_db_interrupt_raise(AxisDbInterrupt *self)
{
	g_return_if_fail(self != NULL);

	/* One byte, written once and never read: the pipe stays readable. */
	if (g_atomic_int_compare_and_exchange(&self->raised, 0, 1) && self->wake[1] >= 0) {
		gssize written;

		do {
			written = write(self->wake[1], "", 1);
		} while (written < 0 && errno == EINTR);
	}
}

gboolean
axis_db_interrupt_is_raised(AxisDbInterrupt *self)
{
	return self != NULL && g_atomic_int_get(&self->raised) != 0;
}

void
axis_db_interrupt_attach(AxisDbInterrupt *self)
{
	g_private_set(&thread_interrupt, self);
}

gboolean
axis_db_thread_is_interruptible(void)
{
	return g_private_get(&thread_interrupt) != NULL;
}

gboolean
axis_db_thread_is_interrupted(void)
{
	return axis_db_interrupt_is_raised(g_private_get(&thread_interrupt));
}

AxisDbWait
axis_db_wait_socket(
	PGconn   *pg,
	gboolean  for_write,
	gint64    deadline
){
	AxisDbInterrupt *interrupt = g_private_get(&thread_interrupt);
	int fd = PQsocket(pg);

	if (fd < 0)
		return AXIS_DB_WAIT_FAILED;

	for (;;) {
		struct pollfd pfd[2];
		nfds_t n = 1;
		int timeout_ms = -1;
		int rc;

		/* The thread's work was called off: as a deadline that has passed. */
		if (axis_db_interrupt_is_raised(interrupt))
			return AXIS_DB_WAIT_TIMEOUT;

		if (deadline >= 0) {
			gint64 now = g_get_monotonic_time();
			gint64 remaining;

			if (now >= deadline)
				return AXIS_DB_WAIT_TIMEOUT;
			/* Round up, so a wait never ends just short of the
			 * deadline and spins. */
			remaining = (deadline - now + 999) / 1000;
			timeout_ms = remaining > G_MAXINT ? G_MAXINT : (int)remaining;
		}

		pfd[0].fd = fd;
		pfd[0].events = for_write ? POLLOUT : POLLIN;
		pfd[0].revents = 0;
		if (interrupt != NULL && interrupt->wake[0] >= 0) {
			pfd[1].fd = interrupt->wake[0];
			pfd[1].events = POLLIN;
			pfd[1].revents = 0;
			n = 2;
		} else if (interrupt != NULL && (timeout_ms < 0 || timeout_ms > INTERRUPT_LOOK_MS)) {
			timeout_ms = INTERRUPT_LOOK_MS;
		}
		rc = poll(pfd, n, timeout_ms);
		if (rc > 0 && pfd[0].revents != 0)
			return AXIS_DB_WAIT_READY;
		if (rc >= 0 || errno == EINTR)
			continue;	/* the deadline and the interrupt are checked at the top */

		return AXIS_DB_WAIT_FAILED;
	}
}

/*
 * libpq's messages end with a newline and some continue on further lines
 * indented with a tab.  Error messages here are one line each.
 */
static gchar *
one_line(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p = text != NULL ? text : "";

	while (*p != '\0') {
		if (*p == '\n' || *p == '\r') {
			while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
				p++;
			if (*p != '\0')
				g_string_append_c(out, ' ');
			continue;
		}
		g_string_append_c(out, *p);
		p++;
	}

	return g_strstrip(g_string_free(out, FALSE));
}

static void
conn_set_error(
	AxisDbConn   *self,
	GError      **error,
	gint          code,
	const gchar  *format,
	...
) G_GNUC_PRINTF(4, 5);

/* Every error made here passes through the pool's password scrub. */
static void
conn_set_error(
	AxisDbConn   *self,
	GError      **error,
	gint          code,
	const gchar  *format,
	...
){
	g_autofree gchar *message = NULL;
	g_autofree gchar *clean = NULL;
	va_list ap;

	va_start(ap, format);
	message = g_strdup_vprintf(format, ap);
	va_end(ap);

	clean = axis_db_pool_scrub(self->pool, message);
	g_set_error_literal(error, AXIS_DB_ERROR, code, clean);
}

/* An error from libpq itself rather than from a result. */
static void
conn_set_libpq_error(
	AxisDbConn   *self,
	GError      **error,
	const gchar  *what
){
	g_autofree gchar *text = NULL;

	text = one_line(self->pg != NULL ? PQerrorMessage(self->pg) : "not connected");
	if (*text == '\0') {
		g_free(text);
		text = g_strdup("unknown error");
	}

	if (self->pg == NULL || PQstatus(self->pg) == CONNECTION_BAD) {
		self->close_on_release = TRUE;
		conn_set_error(self, error, AXIS_DB_ERROR_CONNECTION_LOST,
			"lost the database connection: %s", text);
	} else {
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY, "%s: %s", what, text);
	}
}

/* SQLSTATEs meaning the server has ended (or is ending) the session. */
static gboolean
sqlstate_ends_session(const gchar *sqlstate)
{
	static const gchar *const states[] = {
		"57P01",	/* admin_shutdown (pg_terminate_backend, shutdown) */
		"57P02",	/* crash_shutdown */
		"57P03",	/* cannot_connect_now */
		"57P05",	/* idle_session_timeout */
		"25P03",	/* idle_in_transaction_session_timeout */
		NULL
	};
	guint i;

	if (sqlstate == NULL)
		return FALSE;
	if (g_str_has_prefix(sqlstate, "08"))	/* connection exception */
		return TRUE;
	for (i = 0; states[i] != NULL; i++) {
		if (strcmp(sqlstate, states[i]) == 0)
			return TRUE;
	}

	return FALSE;
}

static void
conn_set_result_error(
	AxisDbConn  *self,
	PGresult    *res,
	GError     **error
){
	const gchar *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	const gchar *primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	const gchar *detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	g_autofree gchar *joined = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *suffix = NULL;
	gint code;

	if (primary != NULL && detail != NULL) {
		joined = g_strdup_printf("%s: %s", primary, detail);
		text = one_line(joined);
	} else if (primary != NULL) {
		text = one_line(primary);
	} else {
		text = one_line(*PQresultErrorMessage(res) != '\0' ?
			PQresultErrorMessage(res) : PQerrorMessage(self->pg));
	}

	suffix = sqlstate != NULL ? g_strdup_printf(" [SQLSTATE %s]", sqlstate) : g_strdup("");

	if (PQstatus(self->pg) == CONNECTION_BAD || sqlstate_ends_session(sqlstate)) {
		self->close_on_release = TRUE;
		conn_set_error(self, error, AXIS_DB_ERROR_CONNECTION_LOST,
			"lost the database connection: %s%s", text, suffix);
		return;
	}

	if (sqlstate != NULL && strcmp(sqlstate, "23505") == 0)
		code = AXIS_DB_ERROR_UNIQUE_VIOLATION;
	else if (sqlstate != NULL && strcmp(sqlstate, "57014") == 0)
		code = AXIS_DB_ERROR_TIMEOUT;
	else
		code = AXIS_DB_ERROR_QUERY;

	conn_set_error(self, error, code, "%s%s", text, suffix);
}

/* The calling thread must hold the connection. */
static gboolean
check_holder(
	AxisDbConn  *self,
	GError     **error
){
	if (g_atomic_pointer_get(&self->owner) == (gpointer)g_thread_self())
		return TRUE;

	g_critical("database connection %u used by a thread that has not acquired it",
		self->index + 1);
	g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY,
		"database connection used by a thread that has not acquired it");
	return FALSE;
}

static gboolean
is_error_status(ExecStatusType status)
{
	return status == PGRES_FATAL_ERROR || status == PGRES_NONFATAL_ERROR ||
	       status == PGRES_BAD_RESPONSE;
}

static gboolean
is_copy_status(ExecStatusType status)
{
	return status == PGRES_COPY_IN || status == PGRES_COPY_OUT ||
	       status == PGRES_COPY_BOTH;
}

gboolean
axis_db_conn_run(
	AxisDbConn         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	gboolean            simple,
	gint64              deadline,
	AxisDbResult      **result,
	GError            **error
){
	PGresult *first_error = NULL;
	PGresult *last = NULL;
	PGresult *res;
	ExecStatusType status;
	gboolean copying = FALSE;
	int sent;

	if (result != NULL)
		*result = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(sql != NULL, FALSE);
	g_return_val_if_fail(n_params == 0 || params != NULL, FALSE);
	g_return_val_if_fail(!simple || n_params == 0, FALSE);

	if (!check_holder(self, error))
		return FALSE;

	if (n_params > AXIS_DB_MAX_PARAMS) {
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
			"too many parameters: %u (at most %u)", n_params, (guint)AXIS_DB_MAX_PARAMS);
		return FALSE;
	}

	if (self->pg == NULL || PQstatus(self->pg) != CONNECTION_OK) {
		conn_set_libpq_error(self, error, "cannot send the statement");
		return FALSE;
	}

	/* A query abandoned at its deadline is still running: nothing more
	 * can be sent until it ends, and the connection is closed on release
	 * anyway. */
	if (PQtransactionStatus(self->pg) == PQTRANS_ACTIVE) {
		self->close_on_release = TRUE;
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
			"the database connection is still busy with an abandoned statement");
		return FALSE;
	}

	if (simple)
		sent = PQsendQuery(self->pg, sql);
	else
		sent = PQsendQueryParams(self->pg, sql, (int)n_params, NULL,
			params, NULL, NULL, 0);
	if (!sent) {
		conn_set_libpq_error(self, error, "cannot send the statement");
		return FALSE;
	}
	axis_db_pool_count_statement(self->pool, sql, n_params, params);

	for (;;) {
		while (PQisBusy(self->pg)) {
			AxisDbWait wait = axis_db_wait_socket(self->pg, FALSE, deadline);

			if (wait == AXIS_DB_WAIT_TIMEOUT) {
				self->close_on_release = TRUE;
				PQclear(first_error);
				PQclear(last);
				conn_set_error(self, error, AXIS_DB_ERROR_TIMEOUT,
					"the database did not answer in time");
				return FALSE;
			}
			/* On failure the connection is marked bad, and the
			 * PQgetResult() below returns the error at once. */
			if (wait == AXIS_DB_WAIT_FAILED || !PQconsumeInput(self->pg))
				break;
		}

		res = PQgetResult(self->pg);
		if (res == NULL)
			break;

		status = PQresultStatus(res);
		if (is_error_status(status)) {
			if (first_error == NULL)
				first_error = res;
			else
				PQclear(res);
		} else {
			PQclear(last);
			last = res;
		}

		if (is_copy_status(status)) {
			copying = TRUE;
			break;
		}
		if (PQstatus(self->pg) == CONNECTION_BAD)
			break;
	}

	if (copying) {
		/* The connection is now in COPY mode, which nothing here can
		 * drive; it is not safe to reuse. */
		self->close_on_release = TRUE;
		PQclear(first_error);
		PQclear(last);
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY, "COPY is not supported");
		return FALSE;
	}

	if (first_error != NULL) {
		conn_set_result_error(self, first_error, error);
		PQclear(first_error);
		PQclear(last);
		return FALSE;
	}

	if (last == NULL) {
		conn_set_libpq_error(self, error, "the statement returned no result");
		return FALSE;
	}

	status = PQresultStatus(last);
	if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
		if (status == PGRES_EMPTY_QUERY)
			conn_set_error(self, error, AXIS_DB_ERROR_QUERY, "empty statement");
		else
			conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
				"unexpected result: %s", PQresStatus(status));
		PQclear(last);
		return FALSE;
	}

	if (result != NULL)
		*result = axis_db_result_new_take(last);
	else
		PQclear(last);

	return TRUE;
}

gboolean
axis_db_conn_exec(
	AxisDbConn         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	AxisDbResult      **result,
	GError            **error
){
	return axis_db_conn_run(self, sql, n_params, params, FALSE, -1, result, error);
}

PGTransactionStatusType
axis_db_conn_get_transaction_status(AxisDbConn *self)
{
	g_return_val_if_fail(self != NULL, PQTRANS_UNKNOWN);

	if (self->pg == NULL)
		return PQTRANS_UNKNOWN;

	return PQtransactionStatus(self->pg);
}

void
axis_db_conn_set_close_on_release(AxisDbConn *self)
{
	g_return_if_fail(self != NULL);

	self->close_on_release = TRUE;
}

gboolean
axis_db_conn_in_transaction(AxisDbConn *self)
{
	PGTransactionStatusType status;

	g_return_val_if_fail(self != NULL, FALSE);

	status = axis_db_conn_get_transaction_status(self);
	return status == PQTRANS_INTRANS || status == PQTRANS_INERROR;
}

gboolean
axis_db_conn_begin(
	AxisDbConn  *self,
	GError     **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_holder(self, error))
		return FALSE;

	if (axis_db_conn_in_transaction(self)) {
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
			"a transaction is already open on this connection");
		return FALSE;
	}

	return axis_db_conn_run(self, "BEGIN", 0, NULL, FALSE, -1, NULL, error);
}

gboolean
axis_db_conn_commit(
	AxisDbConn  *self,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	PGTransactionStatusType status;
	const gchar *tag;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_holder(self, error))
		return FALSE;

	status = axis_db_conn_get_transaction_status(self);
	if (status == PQTRANS_INERROR) {
		/* COMMIT would "succeed" with the tag ROLLBACK. */
		axis_db_conn_rollback(self, NULL);
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
			"the transaction was aborted by an earlier error and has been rolled back");
		return FALSE;
	}
	if (status != PQTRANS_INTRANS) {
		if (self->pg == NULL || PQstatus(self->pg) != CONNECTION_OK) {
			conn_set_libpq_error(self, error, "cannot commit");
			return FALSE;
		}
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY, "no transaction is open");
		return FALSE;
	}

	if (!axis_db_conn_run(self, "COMMIT", 0, NULL, FALSE, -1, &result, error))
		return FALSE;

	tag = axis_db_result_get_command_tag(result);
	if (tag == NULL || strcmp(tag, "COMMIT") != 0) {
		conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
			"the server rolled the transaction back instead of committing it");
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_db_conn_rollback(
	AxisDbConn  *self,
	GError     **error
){
	PGTransactionStatusType status;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!check_holder(self, error))
		return FALSE;

	status = axis_db_conn_get_transaction_status(self);
	if (status == PQTRANS_IDLE)
		return TRUE;
	if (status != PQTRANS_INTRANS && status != PQTRANS_INERROR) {
		/* Gone (the server discards the transaction with the session)
		 * or still busy with an abandoned statement. */
		self->close_on_release = TRUE;
		if (self->pg == NULL || PQstatus(self->pg) != CONNECTION_OK) {
			conn_set_libpq_error(self, error, "cannot roll back");
		} else {
			conn_set_error(self, error, AXIS_DB_ERROR_QUERY,
				"cannot roll back while an abandoned statement is running");
		}
		return FALSE;
	}

	return axis_db_conn_run(self, "ROLLBACK", 0, NULL, FALSE, -1, NULL, error);
}

gchar *
axis_db_quote_identifier(const gchar *identifier)
{
	GString *out;
	const gchar *p;

	if (identifier == NULL || *identifier == '\0')
		return NULL;
	if (!g_utf8_validate(identifier, -1, NULL))
		return NULL;

	out = g_string_new("\"");
	for (p = identifier; *p != '\0'; p++) {
		if (*p == '"')
			g_string_append_c(out, '"');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');

	return g_string_free(out, FALSE);
}
