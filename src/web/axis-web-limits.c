/*
 * axis-web-limits.c - what one request and one client may cost, from the
 * moment the port is open
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * libsoup's server has no read timeout and no cap on connections, and
 * htmx-glib adds neither, so this watches the server from outside, through
 * the signals libsoup gives: "request-started" (a blank message, before any
 * byte of the request is read: at once for a new connection, and on a
 * kept-alive one when the next request's first byte arrives), and each
 * message's "got-headers", "got-body" and "finished" (the response is
 * written, so the connection is idle).  A connection is known by its
 * GSocket, of which a reference is kept: libsoup closes it when the
 * connection ends, which is how its end is seen.  A connection is closed
 * from here by shutting its socket down, which libsoup reads as the client
 * going away; a refusal is written by taking the connection from libsoup
 * (soup_server_message_steal_connection()) as soon as the head has arrived,
 * so that no byte of a body is read for it.
 */

#include "web/axis-web-limits.h"

#include <libsoup/soup.h>
#include <string.h>

#include "web/axis-web-headers.h"

#define LIMITS_KEY    "axis-web-limits"
#define IN_FLIGHT_KEY "axis-web-limits-in-flight"

/* How often deadlines and closed connections are looked at. */
#define TICK_MS 250

/* A kind of refusal is logged at most this often. */
#define LOG_EVERY_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

/* A connection over a cap has this long for its head (it is answered 503,
 * or served when it is a probe from beside the daemon), then it is closed. */
#define OVERFLOW_HEAD_US (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)

/* After a refusal is written, what the client still sends is read and
 * dropped for at most this long, or this much, so that closing does not
 * reset the connection before the client has read the answer. */
#define LINGER_MS    1000
#define LINGER_BYTES (256 * 1024)

typedef struct
{
	GSocket     *socket;	/* a reference */
	gchar       *peer;	/* the peer's address, as counted */
	gboolean     via_proxy;	/* the peer is a trusted proxy */
	gboolean     local;	/* loopback, private or link-local: not held to the per-address cap */
	gboolean     counted;	/* in per_peer */
	gboolean     overflow;	/* over a cap: probes only, or 503 */
	gboolean     worker;	/* it carried a worker's valid token */
	guint        requests;	/* started on it so far */
	gint64       deadline_us;	/* monotonic; 0 for none */
	const gchar *waiting;	/* what the deadline is for, for the log */
} Conn;

/* A request in flight from a client a trusted proxy forwards. */
typedef struct
{
	AxisWebLimits *limits;	/* a reference */
	gchar         *client;
} InFlight;

struct _AxisWebLimits
{
	gint           ref_count;	/* atomic */
	SoupServer    *soup;		/* a weak pointer; NULL once it is gone */
	gint           bodies;		/* atomic: bodies are accepted */
	AxisWebLimitsRefusalFunc refusal;	/* what a refusal says while bodies are not */
	gpointer       refusal_data;
	gint64         head_us;
	gint64         body_us;
	gint64         idle_us;
	guint          max_connections;
	guint          max_per_address;
	guint          max_overflow;
	gboolean       local_is_public;	/* tests: loopback held to the per-address cap */
	goffset        max_body;	/* 0: the shell's limits */
	gchar         *too_large_json;	/* the body of a 413, or NULL for none */
	AxisProxyList *own_trusted;
	AxisWebLimitsTrustedFunc trusted;	/* whose proxies, or NULL for own_trusted */
	AxisWebLimitsExemptFunc  exempt;	/* which requests the caps are not for, or NULL */
	gpointer       sources_data;	/* borrowed */
	GHashTable    *conns;		/* GSocket* -> Conn* */
	guint          n_normal;	/* of them, not over a cap */
	guint          n_overflow;
	GHashTable    *per_peer;	/* address -> count */
	GHashTable    *in_flight;	/* client address -> count */
	guint          tick_id;
	guint          timed_out;
	guint          refused;
	gchar         *refused_peer;	/* the last refused */
	const gchar   *refused_why;
	gint64         refused_logged_us;
	gint64         timed_out_logged_us;
};

static AxisProxyList *
trusted_of(AxisWebLimits *self)
{
	AxisProxyList *list = self->trusted != NULL ? self->trusted(self->sources_data) : NULL;

	return list != NULL ? list : self->own_trusted;
}

/* "/api/v1/worker" itself, or anything below it. */
static gboolean
is_worker_path(const gchar *path)
{
	static const gchar bare[] = AXIS_WEB_LIMITS_WORKER_PATH;

	return path != NULL && strncmp(path, bare, sizeof(bare) - 1) == 0 &&
		(path[sizeof(bare) - 1] == '\0' || path[sizeof(bare) - 1] == '/');
}

static gboolean
is_trusted(
	AxisWebLimits       *self,
	const AxisIpAddress *address
){
	return axis_proxy_list_contains(trusted_of(self), address);
}

static guint
count_of(
	GHashTable  *table,
	const gchar *key
){
	return GPOINTER_TO_UINT(g_hash_table_lookup(table, key));
}

static void
count_add(
	GHashTable  *table,
	const gchar *key,
	gint         delta
){
	guint count = count_of(table, key);

	if (delta < 0 && count <= (guint)-delta) {
		g_hash_table_remove(table, key);
		return;
	}
	g_hash_table_replace(table, g_strdup(key), GUINT_TO_POINTER(count + (guint)delta));
}

/* Logs @text, unless a line of its kind was logged in the last minute. */
static void
log_now_and_then(
	gint64      *last_us,
	const gchar *text
){
	gint64 now = g_get_monotonic_time();

	if (*last_us != 0 && now - *last_us < LOG_EVERY_US)
		return;
	*last_us = now;
	g_message("%s (more like it are not logged for a minute)", text);
}

/* Counts a refusal of @peer for @why, for the log and the hint. */
static void
note_refused(
	AxisWebLimits *self,
	const gchar   *peer,
	const gchar   *why,
	const gchar   *what
){
	g_autofree gchar *text = g_strdup_printf("refused %s from %s: %s", what, peer, why);

	self->refused++;
	g_free(self->refused_peer);
	self->refused_peer = g_strdup(peer);
	self->refused_why = why;
	log_now_and_then(&self->refused_logged_us, text);
}

/* ── Connections ────────────────────────────────────────────────────── */

static void
conn_free(Conn *conn)
{
	g_object_unref(conn->socket);
	g_free(conn->peer);
	g_free(conn);
}

static void
conn_uncount(
	AxisWebLimits *self,
	Conn          *conn
){
	if (conn->counted) {
		count_add(self->per_peer, conn->peer, -1);
		conn->counted = FALSE;
	}
}

/* Forgets @conn (its socket is closed), with its counts. */
static void
conn_forget(
	AxisWebLimits *self,
	Conn          *conn
){
	conn_uncount(self, conn);
	if (conn->overflow)
		self->n_overflow--;
	else
		self->n_normal--;
	g_hash_table_remove(self->conns, conn->socket);
}

/* Closes @conn: libsoup reads the end of the stream and lets it go. */
static void
conn_close(Conn *conn)
{
	conn->deadline_us = 0;
	g_socket_shutdown(conn->socket, TRUE, TRUE, NULL);
}

static gboolean
on_tick(gpointer data)
{
	AxisWebLimits *self = data;
	g_autoptr(GPtrArray) gone = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	gint64 now = g_get_monotonic_time();
	guint i;

	g_hash_table_iter_init(&iter, self->conns);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Conn *conn = value;

		if (g_socket_is_closed(conn->socket)) {
			g_ptr_array_add(gone, conn);
		} else if (conn->deadline_us != 0 && now >= conn->deadline_us) {
			g_autofree gchar *text = g_strdup_printf("closed a connection from %s: %s", conn->peer,
				conn->waiting);

			self->timed_out++;
			log_now_and_then(&self->timed_out_logged_us, text);
			conn_close(conn);
		}
	}
	for (i = 0; i < gone->len; i++)
		conn_forget(self, g_ptr_array_index(gone, i));

	if (g_hash_table_size(self->conns) > 0)
		return G_SOURCE_CONTINUE;
	self->tick_id = 0;
	return G_SOURCE_REMOVE;
}

static void
ensure_tick(AxisWebLimits *self)
{
	if (self->tick_id == 0)
		self->tick_id = g_timeout_add(TICK_MS, on_tick, self);
}

/* Forgets the connections whose sockets are closed, so that the counts a
 * new one is judged by are current. */
static void
sweep_closed(AxisWebLimits *self)
{
	g_autoptr(GPtrArray) gone = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;
	guint i;

	g_hash_table_iter_init(&iter, self->conns);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		if (g_socket_is_closed(((Conn *)value)->socket))
			g_ptr_array_add(gone, value);
	}
	for (i = 0; i < gone->len; i++)
		conn_forget(self, g_ptr_array_index(gone, i));
}

static Conn *
conn_of(
	AxisWebLimits     *self,
	SoupServerMessage *message
){
	GSocket *socket = soup_server_message_get_socket(message);

	return socket != NULL ? g_hash_table_lookup(self->conns, socket) : NULL;
}

/* ── Answers written without libsoup ────────────────────────────────── */

typedef struct
{
	GIOStream *io;
	GSource   *source;
	guint      timer;
	gsize      drained;
} Linger;

static void
linger_end(Linger *linger)
{
	if (linger->source != NULL) {
		g_source_destroy(linger->source);
		g_source_unref(linger->source);
	}
	if (linger->timer != 0)
		g_source_remove(linger->timer);
	g_io_stream_close(linger->io, NULL, NULL);
	g_object_unref(linger->io);
	g_free(linger);
}

static gboolean
on_linger_timeout(gpointer data)
{
	Linger *linger = data;

	linger->timer = 0;
	linger_end(linger);
	return G_SOURCE_REMOVE;
}

/* What the client still sends after the answer, read and dropped. */
static gboolean
on_linger_readable(
	GObject  *stream,
	gpointer  data
){
	Linger *linger = data;
	gchar buffer[8192];

	for (;;) {
		g_autoptr(GError) error = NULL;
		gssize n = g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(stream), buffer,
			sizeof(buffer), NULL, &error);

		if (n > 0) {
			linger->drained += (gsize)n;
			if (linger->drained < LINGER_BYTES)
				continue;
		} else if (n < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			return G_SOURCE_CONTINUE;
		}
		/* The end, an error, or enough: done. */
		g_source_unref(linger->source);
		linger->source = NULL;
		g_source_remove(linger->timer);
		linger->timer = 0;
		linger_end(linger);
		return G_SOURCE_REMOVE;
	}
}

/*
 * Answers @message with @status, @text and the shell's security headers
 * (and Retry-After: @retry_after, when given) on its connection, taken from
 * libsoup: no byte of a body is read for it, no handler runs, and the
 * connection is closed after a moment in which what the client still sends
 * is dropped.  Falls back to libsoup's own refusal if the connection cannot
 * be taken.
 */
static void
answer_now(
	SoupServerMessage *message,
	guint              status,
	const gchar       *retry_after,
	const gchar       *text
){
	g_autoptr(SoupMessageHeaders) headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;
	GString *out;
	GIOStream *io;
	GOutputStream *output;
	GInputStream *input;
	Linger *linger;
	gsize done = 0;

	io = soup_server_message_steal_connection(message);
	if (io == NULL) {
		SoupMessageHeaders *response = soup_server_message_get_response_headers(message);

		soup_message_body_set_accumulate(soup_server_message_get_request_body(message), FALSE);
		soup_server_message_set_status(message, status, NULL);
		axis_web_replace_security_headers(response);
		soup_message_headers_replace(response, "Connection", "close");
		if (retry_after != NULL)
			soup_message_headers_replace(response, "Retry-After", retry_after);
		return;
	}

	axis_web_replace_security_headers(headers);
	soup_message_headers_replace(headers, "Content-Type", "text/plain; charset=utf-8");
	soup_message_headers_replace(headers, "Connection", "close");
	if (retry_after != NULL)
		soup_message_headers_replace(headers, "Retry-After", retry_after);
	out = g_string_new(NULL);
	g_string_append_printf(out, "HTTP/1.1 %u %s\r\n", status, soup_status_get_phrase(status));
	soup_message_headers_iter_init(&iter, headers);
	while (soup_message_headers_iter_next(&iter, &name, &value))
		g_string_append_printf(out, "%s: %s\r\n", name, value);
	g_string_append_printf(out, "Content-Length: %" G_GSIZE_FORMAT "\r\n\r\n%s", strlen(text), text);

	/* The answer is small: it fits in the socket's buffer at once, or the
	 * client is not reading and gets less of it. */
	output = g_io_stream_get_output_stream(io);
	while (G_IS_POLLABLE_OUTPUT_STREAM(output) && done < out->len) {
		gssize n = g_pollable_output_stream_write_nonblocking(G_POLLABLE_OUTPUT_STREAM(output), out->str + done,
			out->len - done, NULL, NULL);

		if (n <= 0)
			break;
		done += (gsize)n;
	}
	g_string_free(out, TRUE);
	if (G_IS_SOCKET_CONNECTION(io))
		g_socket_shutdown(g_socket_connection_get_socket(G_SOCKET_CONNECTION(io)), FALSE, TRUE, NULL);

	linger = g_new0(Linger, 1);
	linger->io = io;
	linger->timer = g_timeout_add(LINGER_MS, on_linger_timeout, linger);
	input = g_io_stream_get_input_stream(io);
	if (G_IS_POLLABLE_INPUT_STREAM(input)) {
		linger->source = g_pollable_input_stream_create_source(G_POLLABLE_INPUT_STREAM(input), NULL);
		g_source_set_callback(linger->source, (GSourceFunc)(void (*)(void))on_linger_readable, linger, NULL);
		g_source_attach(linger->source, NULL);
	}
}

/* ── Requests ───────────────────────────────────────────────────────── */

static void
in_flight_free(gpointer data)
{
	InFlight *flight = data;

	count_add(flight->limits->in_flight, flight->client, -1);
	axis_web_limits_unref(flight->limits);
	g_free(flight->client);
	g_free(flight);
}

/* A refusal libsoup writes: the body is read and dropped first. */
static void
refuse(
	SoupServerMessage *message,
	guint              status
){
	SoupMessageHeaders *headers = soup_server_message_get_response_headers(message);

	soup_message_body_set_accumulate(soup_server_message_get_request_body(message), FALSE);
	soup_server_message_set_status(message, status, NULL);
	axis_web_replace_security_headers(headers);
	soup_message_headers_replace(headers, "Connection", "close");
}

static gboolean
is_probe(const gchar *path)
{
	return g_strcmp0(path, "/livez") == 0 || g_strcmp0(path, "/readyz") == 0 || g_strcmp0(path, "/healthz") == 0;
}

/* Whether the request carries a worker's valid token (only on the worker
 * API's paths, where it is the credential). */
static gboolean
from_worker(
	AxisWebLimits      *self,
	const gchar        *path,
	SoupMessageHeaders *headers
){
	return self->exempt != NULL && path != NULL &&
	       self->exempt(path, soup_message_headers_get_list(headers, "Authorization"), self->sources_data);
}

/*
 * A request's head has arrived, before its body.  A body too long (or of
 * unknown length) is not kept: libsoup is told to drop it as it arrives,
 * and the status set here means no handler runs.  Any other refusal is
 * written at once, without reading a body.
 */
static void
on_got_headers(
	SoupServerMessage *message,
	gpointer           user_data
){
	AxisWebLimits *self = user_data;
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
	SoupEncoding encoding = soup_message_headers_get_encoding(headers);
	GUri *uri = soup_server_message_get_uri(message);
	const gchar *path = uri != NULL ? g_uri_get_path(uri) : NULL;
	Conn *conn = conn_of(self, message);
	gboolean worker = from_worker(self, path, headers);
	gboolean has_body;
	goffset limit;

	has_body = encoding != SOUP_ENCODING_NONE &&
		!(encoding == SOUP_ENCODING_CONTENT_LENGTH && soup_message_headers_get_content_length(headers) == 0);
	if (conn != NULL) {
		/* A body has its own deadline; a request without one is read. */
		conn->deadline_us = has_body ? g_get_monotonic_time() + self->body_us : 0;
		conn->waiting = "its request body did not arrive in time (server.body_timeout)";

		/* A worker with its valid token is not one of the clients the caps
		 * are for: the worker API throttles wrong tokens itself, and a
		 * report lost here would be hours of encoding. */
		if (worker && !conn->worker) {
			conn->worker = TRUE;
			conn_uncount(self, conn);
			if (conn->overflow) {
				conn->overflow = FALSE;
				self->n_overflow--;
				self->n_normal++;
			}
		}

		/* Over a cap: a probe from beside the daemon is answered (and the
		 * connection then closed), anything else is told to come back. */
		if (conn->overflow) {
			if (conn->local && is_probe(path) && !has_body) {
				soup_message_headers_replace(soup_server_message_get_response_headers(message), "Connection",
					"close");
				return;
			}
			note_refused(self, conn->peer, conn->waiting, "a request");
			answer_now(message, 503, "5", "Axis has too many connections open right now. Try again in a few seconds.\n");
			return;
		}
	}

	/* Before the daemon serves, and while it has lost its lock, nothing
	 * takes a body: the answer is the start-up gate's, and the body is not
	 * read. */
	if (has_body && !g_atomic_int_get(&self->bodies)) {
		const gchar *text = self->refusal != NULL ? self->refusal(self->refusal_data) : NULL;

		answer_now(message, 503, "5", text != NULL ? text : "Axis is not ready for this request. Try again in a few "
			"seconds.\n");
		return;
	}

	/* The worker API takes less than a form may be; a listener with a limit
	 * of its own (the portal API's) takes that. */
	limit = self->max_body > 0 ? self->max_body :
		is_worker_path(path) ? AXIS_WEB_LIMITS_WORKER_BODY : AXIS_WEB_MAX_REQUEST_BODY;
	if (has_body && (encoding != SOUP_ENCODING_CONTENT_LENGTH ||
	                 soup_message_headers_get_content_length(headers) > limit)) {
		refuse(message, 413);
		if (self->too_large_json != NULL)
			soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, self->too_large_json,
				strlen(self->too_large_json));
		return;
	}

	/* Behind a trusted proxy: what one client it forwards has in flight.
	 * An event stream is held by the hub's own limits, and a worker by
	 * none of these. */
	if (conn != NULL && conn->via_proxy && !worker && g_strcmp0(path, AXIS_WEB_LIMITS_STREAM_PATH) != 0) {
		g_autofree gchar *client = NULL;
		InFlight *flight;
		gboolean https;

		axis_proxy_resolve(trusted_of(self), soup_server_message_get_remote_host(message), FALSE,
			soup_message_headers_get_list(headers, "X-Forwarded-For"), NULL, &client, &https);
		flight = g_new0(InFlight, 1);
		flight->client = axis_proxy_address_key(client);
		if (count_of(self->in_flight, flight->client) >= self->max_per_address) {
			note_refused(self, flight->client, "that client has as many requests in flight as it may "
				"(server.max_connections_per_address)", "a request");
			g_free(flight->client);
			g_free(flight);
			answer_now(message, 429, "5", "Too many requests from your address at once. Try again in a few "
				"seconds.\n");
			return;
		}
		count_add(self->in_flight, flight->client, 1);
		flight->limits = axis_web_limits_ref(self);
		/* Released when the request ends, or when the message goes. */
		g_object_set_data_full(G_OBJECT(message), IN_FLIGHT_KEY, flight, in_flight_free);
	}
}

static void
on_got_body(
	SoupServerMessage *message,
	gpointer           user_data
){
	Conn *conn = conn_of(user_data, message);

	/* The handler's time, and an event stream's, is not limited here. */
	if (conn != NULL)
		conn->deadline_us = 0;
}

static void
on_finished(
	SoupServerMessage *message,
	gpointer           user_data
){
	AxisWebLimits *self = user_data;
	Conn *conn = conn_of(self, message);

	g_object_set_data(G_OBJECT(message), IN_FLIGHT_KEY, NULL);

	/* The response is written; until the next request's first byte, the
	 * connection is idle (a closed one is forgotten at the next tick).  A
	 * worker keeps its connection for its next report. */
	if (conn != NULL && !conn->worker) {
		conn->deadline_us = g_get_monotonic_time() + (conn->overflow ? OVERFLOW_HEAD_US : self->idle_us);
		conn->waiting = "no new request on the kept-alive connection in time (server.idle_timeout)";
	}
}

static void
unref_closure(
	gpointer  data,
	GClosure *closure
){
	axis_web_limits_unref(data);
}

static void
connect_message(
	AxisWebLimits     *self,
	SoupServerMessage *message,
	const gchar       *signal,
	GCallback          callback
){
	g_signal_connect_data(message, signal, callback, axis_web_limits_ref(self), unref_closure, 0);
}

/*
 * A request is about to be read: on a new connection, which is counted (or,
 * over a cap, let in only to be answered, or closed at once when too many
 * are), or on one kept alive after a response.
 */
static void
on_request_started(
	SoupServer        *server,
	SoupServerMessage *message,
	gpointer           user_data
){
	AxisWebLimits *self = user_data;
	GSocket *socket = soup_server_message_get_socket(message);
	gint64 now = g_get_monotonic_time();
	const gchar *remote;
	AxisIpAddress address;
	gboolean parsed;
	gboolean local;
	gboolean via_proxy;
	const gchar *why = NULL;
	Conn *conn;

	connect_message(self, message, "got-headers", G_CALLBACK(on_got_headers));
	connect_message(self, message, "got-body", G_CALLBACK(on_got_body));
	connect_message(self, message, "finished", G_CALLBACK(on_finished));
	if (socket == NULL)
		return;

	/* On a kept-alive connection libsoup starts the next request when its
	 * first byte arrives: from then on, the head has its own deadline. */
	conn = g_hash_table_lookup(self->conns, socket);
	if (conn != NULL) {
		conn->requests++;
		conn->deadline_us = now + (conn->overflow ? OVERFLOW_HEAD_US : self->head_us);
		conn->waiting = "its request head did not arrive in time (server.head_timeout)";
		return;
	}

	sweep_closed(self);
	remote = soup_server_message_get_remote_host(message);
	parsed = remote != NULL && axis_ip_address_parse(remote, &address);
	local = parsed && axis_ip_address_is_private(&address) && !self->local_is_public;
	via_proxy = parsed && is_trusted(self, &address);

	conn = g_new0(Conn, 1);
	conn->socket = g_object_ref(socket);
	conn->peer = axis_proxy_address_key(remote);
	conn->via_proxy = via_proxy;
	conn->local = local;

	/* A trusted proxy, and an address beside the daemon (where proxies and
	 * container gateways are), are held to the total only. */
	if (self->n_normal >= self->max_connections)
		why = "Axis has as many connections open as it allows (server.max_connections)";
	else if (!via_proxy && !local && count_of(self->per_peer, conn->peer) >= self->max_per_address)
		why = "that address has as many connections as it may have (server.max_connections_per_address)";

	if (why != NULL && self->n_overflow >= self->max_overflow) {
		note_refused(self, conn->peer, why, "a connection");
		g_socket_shutdown(socket, TRUE, TRUE, NULL);
		conn_free(conn);
		return;
	}

	conn->requests = 1;
	if (why != NULL) {
		/* Let in to be answered (or, a probe from beside the daemon,
		 * served), with a short deadline. */
		conn->overflow = TRUE;
		conn->deadline_us = now + OVERFLOW_HEAD_US;
		conn->waiting = why;
		self->n_overflow++;
	} else {
		conn->deadline_us = now + self->head_us;
		conn->waiting = "its request head did not arrive in time (server.head_timeout)";
		self->n_normal++;
		if (!via_proxy && !local) {
			count_add(self->per_peer, conn->peer, 1);
			conn->counted = TRUE;
		}
	}
	g_hash_table_insert(self->conns, conn->socket, conn);
	ensure_tick(self);
}

/* ── Lifetime ───────────────────────────────────────────────────────── */

/* The server is going: nothing more will be started, and what is open is
 * no longer counted. */
static void
limits_detach(gpointer data)
{
	AxisWebLimits *self = data;

	if (self->tick_id != 0) {
		g_source_remove(self->tick_id);
		self->tick_id = 0;
	}
	if (self->soup != NULL) {
		g_signal_handlers_disconnect_by_data(self->soup, self);
		g_object_remove_weak_pointer(G_OBJECT(self->soup), (gpointer *)&self->soup);
		self->soup = NULL;
	}
	g_hash_table_remove_all(self->conns);
	g_hash_table_remove_all(self->per_peer);
	self->n_normal = 0;
	self->n_overflow = 0;
	self->trusted = NULL;
	self->exempt = NULL;
	self->sources_data = NULL;
	axis_web_limits_unref(self);
}

static gint64
ms_or(
	guint  ms,
	gint64 default_s
){
	return ms > 0 ? (gint64)ms * 1000 : default_s * G_USEC_PER_SEC;
}

AxisWebLimits *
axis_web_limits_install(
	HtmxServer                 *server,
	const AxisWebLimitsOptions *options
){
	AxisWebLimits *self;
	AxisWebLimitsOptions defaults;

	g_return_val_if_fail(HTMX_IS_SERVER(server), NULL);

	self = axis_web_limits_get(server);
	if (self != NULL)
		return self;

	memset(&defaults, 0, sizeof(defaults));
	if (options == NULL)
		options = &defaults;

	self = g_new0(AxisWebLimits, 1);
	self->ref_count = 1;
	self->bodies = TRUE;
	self->head_us = ms_or(options->head_timeout_ms, AXIS_WEB_LIMITS_HEAD_TIMEOUT_S);
	self->body_us = ms_or(options->body_timeout_ms, AXIS_WEB_LIMITS_BODY_TIMEOUT_S);
	self->idle_us = ms_or(options->idle_timeout_ms, AXIS_WEB_LIMITS_IDLE_TIMEOUT_S);
	self->max_connections = options->max_connections > 0 ? options->max_connections : AXIS_WEB_LIMITS_MAX_CONNECTIONS;
	self->max_per_address = options->max_per_address > 0 ? options->max_per_address : AXIS_WEB_LIMITS_MAX_PER_ADDRESS;
	self->max_overflow = options->max_overflow > 0 ? options->max_overflow : AXIS_WEB_LIMITS_MAX_OVERFLOW;
	self->local_is_public = options->local_is_public;
	self->max_body = (goffset)options->max_body_bytes;
	self->too_large_json = g_strdup(options->too_large_json);
	self->own_trusted = axis_proxy_list_parse(options->trusted_proxies != NULL ? options->trusted_proxies : "", NULL);
	if (self->own_trusted == NULL)
		self->own_trusted = axis_proxy_list_parse("", NULL);
	self->conns = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)conn_free);
	self->per_peer = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->in_flight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	self->soup = htmx_server_get_soup_server(server);
	g_object_add_weak_pointer(G_OBJECT(self->soup), (gpointer *)&self->soup);
	g_signal_connect(self->soup, "request-started", G_CALLBACK(on_request_started), self);

	/* The server's reference: dropped when the server goes. */
	g_object_set_data_full(G_OBJECT(server), LIMITS_KEY, self, limits_detach);
	return self;
}

AxisWebLimits *
axis_web_limits_get(HtmxServer *server)
{
	g_return_val_if_fail(HTMX_IS_SERVER(server), NULL);

	return g_object_get_data(G_OBJECT(server), LIMITS_KEY);
}

AxisWebLimits *
axis_web_limits_ref(AxisWebLimits *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_web_limits_unref(AxisWebLimits *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_hash_table_unref(self->conns);
	g_hash_table_unref(self->per_peer);
	g_hash_table_unref(self->in_flight);
	axis_proxy_list_free(self->own_trusted);
	g_free(self->refused_peer);
	g_free(self->too_large_json);
	g_free(self);
}

void
axis_web_limits_set_bodies(
	AxisWebLimits *self,
	gboolean       accepted
){
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->bodies, accepted ? TRUE : FALSE);
}

void
axis_web_limits_set_refusal(
	AxisWebLimits            *self,
	AxisWebLimitsRefusalFunc  func,
	gpointer                  user_data
){
	g_return_if_fail(self != NULL);

	self->refusal = func;
	self->refusal_data = user_data;
}

void
axis_web_limits_set_sources(
	AxisWebLimits            *self,
	AxisWebLimitsTrustedFunc  trusted,
	AxisWebLimitsExemptFunc   exempt,
	gpointer                  user_data
){
	g_return_if_fail(self != NULL);

	self->trusted = trusted;
	self->exempt = exempt;
	self->sources_data = user_data;
}

gchar *
axis_web_limits_describe_refused(AxisWebLimits *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	if (self->refused == 0)
		return NULL;

	{
		g_autofree gchar *count = self->refused == 1 ? g_strdup("1 connection or request") :
			g_strdup_printf("%u connections and requests", self->refused);

		return g_strdup_printf("Since Axis started, its connection limits have turned away %s, the last from "
			"%s: %s. If that address is a reverse proxy, set AXIS_TRUSTED_PROXIES to it and restart Axis, so that "
			"its visitors are counted one by one. If it really is many people, raise the limit named in the "
			"reason.", count, self->refused_peer, self->refused_why != NULL ? self->refused_why : "a limit");
	}
}

guint
axis_web_limits_count_connections(AxisWebLimits *self)
{
	g_return_val_if_fail(self != NULL, 0);

	sweep_closed(self);
	return g_hash_table_size(self->conns);
}

guint
axis_web_limits_count_timed_out(AxisWebLimits *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->timed_out;
}

guint
axis_web_limits_count_refused(AxisWebLimits *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->refused;
}
