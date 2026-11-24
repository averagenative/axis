/*
 * axis-portal-client.c - axis-portal's one way to the daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One call is one Exchange.  The chain of asynchronous calls that makes it
 * (send, then reads) ends in finish(), which removes the deadline, answers
 * the caller and frees; the deadline only cancels, so whatever happens the
 * chain is what ends.
 */

#include "axis-portal-client.h"

#include <libsoup/soup.h>
#include <string.h>

#include "axis-portal-log.h"
#include "axis-portal-wire.h"
#include "axis-version.h"

#define READ_CHUNK 16384

/* The kinds of failure, each with its budget of log lines. */
typedef enum
{
	FAILED_IN_TIME,
	FAILED_CERTIFICATE,
	FAILED_REACHED,
	FAILED_TOO_LONG,
	FAILED_REDIRECT,
	FAILED_TYPE,
	FAILED_TOO_MANY,
	FAILED_ADDRESS,
	N_FAILED
} Failed;

static const gchar *const failed_words[N_FAILED] = {
	"it did not answer in time",
	"its certificate was not accepted",
	"it could not be reached",
	"its answer is longer than the portal reads",
	"it answered with a redirect or no answer at all, which is never followed",
	"its answer is of a type the portal does not take",
	"too many calls are in flight; one was not made",
	"its address cannot be used"
};

static G_DEFINE_QUARK(axis-portal-client-error-quark, axis_portal_client_error)

/* The eleven operations, in the order of AxisPortalOp: the only texts that
 * are ever appended to the daemon's address.  (The daemon's own list is in
 * api/axis-portal-proto.c, which the portal does not link; a test compares
 * the two.) */
static const gchar *const op_names[AXIS_PORTAL_N_OPS] = {
	"hello", "login", "logout", "whoami", "search", "browse", "title", "art", "request", "requests", "withdraw"
};

const gchar *
axis_portal_client_op_name(AxisPortalOp op)
{
	return (guint)op < AXIS_PORTAL_N_OPS ? op_names[op] : "?";
}

struct _AxisPortalClient
{
	SoupSession *session;
	gchar       *base;		/* the URL up to the operation's name */
	gchar       *authorization;	/* "Bearer <token>": secret */
	gchar       *token;		/* to keep it out of a message: secret */
	guint        timeout_ms;
	guint        pending;
	AxisPortalLogBudget lines[N_FAILED];	/* of the failures' lines */
};

typedef struct
{
	AxisPortalClient     *client;
	AxisPortalOp          op;
	AxisPortalAnswerFunc  func;
	gpointer              user_data;
	SoupMessage          *message;
	GInputStream         *stream;
	GByteArray           *body;
	GCancellable         *cancel;
	GSource              *deadline;
	gboolean              timed_out;
	guint                 status;
	gchar                *content_type;
	gsize                 max_bytes;
	gchar                 buffer[READ_CHUNK];
} Exchange;

static void
wipe(gchar *secret)
{
	if (secret != NULL) {
		volatile gchar *p = secret;

		while (*p != '\0')
			*p++ = '\0';
		g_free(secret);
	}
}

static void
wipe_bytes(gpointer data)
{
	wipe(data);
}

/* Says, for each kind whose minute has ended by @now (every kind, with
 * %G_MAXINT64), how many of its lines were left out. */
static void
failures_flush(
	AxisPortalClient *client,
	gint64            now
){
	guint i;

	for (i = 0; i < N_FAILED; i++) {
		guint due = axis_portal_log_budget_due(&client->lines[i], now);

		if (due > 0)
			g_warning("the daemon: %u more lines of \"%s\" were not written in the last minute", due, failed_words[i]);
	}
}

/* Writes down that the daemon was not reached, or answered what cannot be
 * taken (a page was not served for it): AXIS_PORTAL_LOG_LINES_A_MINUTE of
 * a kind in a minute, then how many more there were.  The words are this
 * file's own, with at most GLib's reason in them, and never the token. */
static void
note_failure(
	Exchange    *exchange,
	Failed       why,
	const gchar *detail
){
	AxisPortalClient *client = exchange->client;
	g_autofree gchar *cut = detail != NULL ? g_strndup(detail, 200) : NULL;
	gint64 now = g_get_monotonic_time();

	failures_flush(client, now);
	if (!axis_portal_log_budget_take(&client->lines[why], now))
		return;
	/* The token is in a header only, and GLib's messages quote none; were
	 * one ever to, it would not be written. */
	if (cut != NULL && client->token != NULL && strstr(cut, client->token) != NULL)
		g_clear_pointer(&cut, g_free);
	g_warning("the daemon: %s, for %s%s%s%s", failed_words[why], axis_portal_client_op_name(exchange->op),
		cut != NULL ? " (" : "", cut != NULL ? cut : "", cut != NULL ? ")" : "");
}

static void
exchange_free(Exchange *exchange)
{
	if (exchange->deadline != NULL) {
		g_source_destroy(exchange->deadline);
		g_source_unref(exchange->deadline);
	}
	g_clear_object(&exchange->stream);
	g_clear_object(&exchange->message);
	g_clear_object(&exchange->cancel);
	if (exchange->body != NULL)
		g_byte_array_unref(exchange->body);
	g_free(exchange->content_type);
	g_free(exchange);
}

/* The end of the chain.  @reached: the answer in @exchange stands. */
static void
finish(
	Exchange *exchange,
	gboolean  reached
){
	AxisPortalClient *client = exchange->client;
	GBytes *body = NULL;

	if (reached)
		body = g_bytes_new(exchange->body->data, exchange->body->len);
	exchange->func(reached ? exchange->status : 0, body, reached ? exchange->content_type : NULL,
		exchange->user_data);
	g_clear_pointer(&body, g_bytes_unref);
	exchange_free(exchange);
	client->pending--;
}

static void
fail_from(
	Exchange     *exchange,
	const GError *error
){
	if (exchange->timed_out)
		note_failure(exchange, FAILED_IN_TIME, NULL);
	else if (error != NULL && error->domain == G_TLS_ERROR)
		note_failure(exchange, FAILED_CERTIFICATE, error->message);
	else
		note_failure(exchange, FAILED_REACHED, error != NULL ? error->message : NULL);
	finish(exchange, FALSE);
}

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Exchange *exchange = user_data;
	g_autoptr(GError) error = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

	if (n < 0) {
		fail_from(exchange, error);
		return;
	}
	if (n == 0) {
		finish(exchange, TRUE);
		return;
	}
	if (exchange->body->len + (gsize)n > exchange->max_bytes) {
		note_failure(exchange, FAILED_TOO_LONG, NULL);
		finish(exchange, FALSE);
		return;
	}
	g_byte_array_append(exchange->body, (const guint8 *)exchange->buffer, (guint)n);
	g_input_stream_read_async(exchange->stream, exchange->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, exchange->cancel,
		on_read, exchange);
}

/* Whether an answer of this type is one the operation gives: JSON, or for a
 * poster that was found one of the three image types. */
static gboolean
type_taken(const Exchange *exchange)
{
	const gchar *type = exchange->content_type;

	if (type == NULL)
		return FALSE;
	if (exchange->op == AXIS_PORTAL_ART && exchange->status == 200)
		return strcmp(type, "image/jpeg") == 0 || strcmp(type, "image/png") == 0 || strcmp(type, "image/webp") == 0;
	return strcmp(type, "application/json") == 0;
}

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Exchange *exchange = user_data;
	g_autoptr(GError) error = NULL;
	GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	SoupMessageHeaders *headers;
	const gchar *type;
	goffset length;

	if (stream == NULL) {
		fail_from(exchange, error);
		return;
	}
	exchange->stream = stream;
	exchange->status = soup_message_get_status(exchange->message);
	headers = soup_message_get_response_headers(exchange->message);
	type = soup_message_headers_get_content_type(headers, NULL);
	exchange->content_type = type != NULL ? g_ascii_strdown(type, -1) : NULL;
	if (exchange->status < 200 || (exchange->status >= 300 && exchange->status <= 399)) {
		note_failure(exchange, FAILED_REDIRECT, NULL);
		finish(exchange, FALSE);
		return;
	}
	if (!type_taken(exchange)) {
		note_failure(exchange, FAILED_TYPE, NULL);
		finish(exchange, FALSE);
		return;
	}
	exchange->max_bytes = exchange->op == AXIS_PORTAL_ART && exchange->status == 200 ?
		AXIS_PORTAL_CLIENT_MAX_ART_BYTES : AXIS_PORTAL_WIRE_MAX_ANSWER;
	length = soup_message_headers_get_content_length(headers);
	if (length > (goffset)exchange->max_bytes) {
		note_failure(exchange, FAILED_TOO_LONG, NULL);
		finish(exchange, FALSE);
		return;
	}
	g_input_stream_read_async(exchange->stream, exchange->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, exchange->cancel,
		on_read, exchange);
}

/* The deadline only cancels: the call in flight then ends the chain. */
static gboolean
on_deadline(gpointer user_data)
{
	Exchange *exchange = user_data;

	exchange->timed_out = TRUE;
	g_cancellable_cancel(exchange->cancel);
	return G_SOURCE_REMOVE;
}

/* A call that was never sent is answered from the main loop, like any. */
static gboolean
on_not_sent(gpointer user_data)
{
	finish(user_data, FALSE);
	return G_SOURCE_REMOVE;
}

void
axis_portal_client_call(
	AxisPortalClient     *self,
	AxisPortalOp          op,
	gchar                *json,
	AxisPortalAnswerFunc  func,
	gpointer              user_data
){
	g_autofree gchar *url = NULL;
	SoupMessageHeaders *headers;
	Exchange *exchange;
	GBytes *body;

	g_return_if_fail(self != NULL && func != NULL);

	exchange = g_new0(Exchange, 1);
	exchange->client = self;
	exchange->op = op;
	exchange->func = func;
	exchange->user_data = user_data;
	exchange->body = g_byte_array_new();
	exchange->cancel = g_cancellable_new();
	self->pending++;

	/* The operation is one of the eleven, by its number: its name is the
	 * shared list's, and nothing else is ever appended to the address. */
	if (json == NULL || (guint)op >= AXIS_PORTAL_N_OPS || self->pending > AXIS_PORTAL_CLIENT_MAX_CALLS) {
		if (json != NULL && (guint)op < AXIS_PORTAL_N_OPS)
			note_failure(exchange, FAILED_TOO_MANY, NULL);
		wipe(json);
		g_idle_add(on_not_sent, exchange);
		return;
	}
	url = g_strconcat(self->base, axis_portal_client_op_name(op), NULL);
	exchange->message = soup_message_new(SOUP_METHOD_POST, url);
	if (exchange->message == NULL) {
		note_failure(exchange, FAILED_ADDRESS, NULL);
		wipe(json);
		g_idle_add(on_not_sent, exchange);
		return;
	}
	/* Every hop would be the daemon's choice: none is followed. */
	soup_message_add_flags(exchange->message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(exchange->message);
	soup_message_headers_replace(headers, "Authorization", self->authorization);
	soup_message_headers_replace(headers, "Accept", "application/json");
	/* The body may hold a password or a proof: wiped when libsoup lets go. */
	body = g_bytes_new_with_free_func(json, strlen(json), wipe_bytes, json);
	soup_message_set_request_body_from_bytes(exchange->message, "application/json", body);
	g_bytes_unref(body);

	exchange->deadline = g_timeout_source_new(MAX(self->timeout_ms, 100u));
	g_source_set_callback(exchange->deadline, on_deadline, exchange, NULL);
	g_source_attach(exchange->deadline, g_main_context_get_thread_default());
	soup_session_send_async(self->session, exchange->message, G_PRIORITY_DEFAULT, exchange->cancel, on_sent, exchange);
}

guint
axis_portal_client_get_pending(AxisPortalClient *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->pending;
}

AxisPortalClient *
axis_portal_client_new(
	const AxisPortalConfig  *config,
	GError                 **error
){
	AxisPortalClient *self;
	g_autofree gchar *agent = g_strdup_printf("axis-portal/%s", AXIS_VERSION);

	g_return_val_if_fail(config != NULL && config->token != NULL && config->url != NULL, NULL);

	self = g_new0(AxisPortalClient, 1);
	self->timeout_ms = config->request_timeout_ms;
	/* No proxy (a NULL resolver: the environment is not read), no cookie
	 * jar (none is added), no decoding of what comes back. */
	self->session = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL, "user-agent", agent,
		"timeout", (guint)MAX(config->request_timeout_ms / 1000, 1u), "idle-timeout", 30u, "max-conns",
		AXIS_PORTAL_CLIENT_MAX_CALLS, "max-conns-per-host", AXIS_PORTAL_CLIENT_MAX_CALLS, NULL);
	soup_session_remove_feature_by_type(self->session, SOUP_TYPE_CONTENT_DECODER);
	if (config->ca_file != NULL) {
		g_autoptr(GError) local = NULL;
		GTlsDatabase *database = g_tls_file_database_new(config->ca_file, &local);

		if (database == NULL) {
			g_set_error(error, axis_portal_client_error_quark(), 0, "the CA file %s cannot be used: %s",
				config->ca_file, local->message);
			axis_portal_client_free(self);
			return NULL;
		}
		soup_session_set_tls_database(self->session, database);
		g_object_unref(database);
	}
	self->base = g_strconcat(config->url, AXIS_PORTAL_API_PREFIX, NULL);
	self->authorization = g_strconcat("Bearer ", config->token, NULL);
	self->token = g_strdup(config->token);
	return self;
}

void
axis_portal_client_free(AxisPortalClient *self)
{
	if (self == NULL)
		return;
	if (self->session != NULL) {
		soup_session_abort(self->session);
		/* Each call in flight ends now, as not reached. */
		while (self->pending > 0)
			g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
		g_object_unref(self->session);
	}
	/* What was left out of the log is said now. */
	failures_flush(self, G_MAXINT64);
	g_free(self->base);
	wipe(self->authorization);
	wipe(self->token);
	g_free(self);
}
