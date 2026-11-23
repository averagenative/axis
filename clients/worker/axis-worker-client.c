/*
 * axis-worker-client.c - axis-worker's one HTTP client
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One exchange is one GTask.  The chain of asynchronous calls that makes
 * it (send, then reads) owns the task's one reference and ends with
 * complete(), which removes the deadline and returns the result; the
 * deadline only cancels, so whatever happens the chain is what ends.
 */

#include "axis-worker-client.h"

#include <libsoup/soup.h>
#include <string.h>

#include "api/axis-worker-proto.h"
#include "axis-version.h"
#include "library/axis-library-name.h"

#define READ_CHUNK 8192

static G_DEFINE_QUARK(axis-worker-client-error-quark, axis_worker_client_error)
#define CLIENT_ERROR (axis_worker_client_error_quark())

struct _AxisWorkerClient
{
	SoupSession *session;
	gchar       *base;		/* the URL up to the request's name */
	gchar       *authorization;	/* "Bearer <token>": secret */
	gchar       *token;		/* to scrub messages: secret */
};

typedef struct
{
	AxisWorkerClient *client;
	SoupMessage      *message;
	GInputStream     *stream;
	GByteArray       *body;
	GCancellable     *cancel;
	GSource          *deadline;
	gboolean          timed_out;
	guint             status;
	gchar             buffer[READ_CHUNK];
} Exchange;

static void
wipe(gchar *secret)
{
	if (secret != NULL) {
		memset(secret, 0, strlen(secret));
		g_free(secret);
	}
}

/* A message for the log: cleaned, cut, and never with the token in it. */
static gchar *
scrub(
	AxisWorkerClient *client,
	const gchar      *text
){
	g_autofree gchar *clean = axis_library_text_clean(text != NULL ? text : "", 300);
	gchar **parts;
	gchar *joined;

	if (client->token == NULL || client->token[0] == '\0')
		return g_steal_pointer(&clean);
	parts = g_strsplit(clean, client->token, -1);
	joined = g_strjoinv("***", parts);
	g_strfreev(parts);
	return joined;
}

static void
drop_deadline(Exchange *exchange)
{
	if (exchange->deadline != NULL) {
		g_source_destroy(exchange->deadline);
		g_source_unref(exchange->deadline);
		exchange->deadline = NULL;
	}
}

static void
exchange_free(gpointer data)
{
	Exchange *exchange = data;

	drop_deadline(exchange);
	g_clear_object(&exchange->stream);
	g_clear_object(&exchange->message);
	g_clear_object(&exchange->cancel);
	if (exchange->body != NULL)
		g_byte_array_unref(exchange->body);
	g_free(exchange);
}

/* The end of the chain: @error (transfer full) or success. */
static void
complete(
	GTask  *task,
	GError *error
){
	Exchange *exchange = g_task_get_task_data(task);

	drop_deadline(exchange);
	if (error != NULL)
		g_task_return_error(task, error);
	else
		g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

static void
complete_error(
	GTask       *task,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
complete_error(
	GTask       *task,
	const gchar *format,
	...
){
	Exchange *exchange = g_task_get_task_data(task);
	g_autofree gchar *text = NULL;
	g_autofree gchar *clean = NULL;
	va_list args;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	clean = scrub(exchange->client, text);
	complete(task, g_error_new_literal(CLIENT_ERROR, 0, clean));
}

static void
complete_from(
	GTask        *task,
	const GError *error
){
	Exchange *exchange = g_task_get_task_data(task);

	if (exchange->timed_out)
		complete_error(task, "the daemon did not answer in time");
	else if (error != NULL && error->domain == G_TLS_ERROR)
		complete_error(task, "the daemon's certificate was not accepted (%s)", error->message);
	else
		complete_error(task, "the daemon could not be reached (%s)", error != NULL ? error->message : "?");
}

static void
on_read(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GTask *task = user_data;
	Exchange *exchange = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

	if (n < 0) {
		complete_from(task, error);
		return;
	}
	if (n == 0) {
		complete(task, NULL);
		return;
	}
	if (exchange->body->len + (gsize)n > AXIS_WORKER_MAX_RESPONSE_BYTES) {
		complete_error(task, "the daemon's answer is longer than %d bytes", AXIS_WORKER_MAX_RESPONSE_BYTES);
		return;
	}
	g_byte_array_append(exchange->body, (const guint8 *)exchange->buffer, (guint)n);
	g_input_stream_read_async(exchange->stream, exchange->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, exchange->cancel,
		on_read, task);
}

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	GTask *task = user_data;
	Exchange *exchange = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	goffset length;

	if (stream == NULL) {
		complete_from(task, error);
		return;
	}
	exchange->stream = stream;
	exchange->status = soup_message_get_status(exchange->message);
	if (exchange->status >= 300 && exchange->status <= 399) {
		complete_error(task, "the daemon answered with a redirect (%u), which is never followed", exchange->status);
		return;
	}
	length = soup_message_headers_get_content_length(soup_message_get_response_headers(exchange->message));
	if (length > AXIS_WORKER_MAX_RESPONSE_BYTES) {
		complete_error(task, "the daemon's answer is longer than %d bytes", AXIS_WORKER_MAX_RESPONSE_BYTES);
		return;
	}
	g_input_stream_read_async(exchange->stream, exchange->buffer, READ_CHUNK, G_PRIORITY_DEFAULT, exchange->cancel,
		on_read, task);
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

void
axis_worker_client_post(
	AxisWorkerClient    *self,
	const gchar         *request,
	const gchar         *json,
	guint                timeout_ms,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autofree gchar *url = g_strconcat(self->base, request, NULL);
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));
	SoupMessageHeaders *headers;
	Exchange *exchange = g_new0(Exchange, 1);
	GTask *task = g_task_new(NULL, NULL, callback, user_data);

	exchange->client = self;
	exchange->body = g_byte_array_new();
	exchange->cancel = g_cancellable_new();
	g_task_set_task_data(task, exchange, exchange_free);

	exchange->message = soup_message_new(SOUP_METHOD_POST, url);
	if (exchange->message == NULL) {
		complete_error(task, "the daemon's address cannot be used");
		return;
	}
	/* Every hop would be the daemon's choice: none is followed. */
	soup_message_add_flags(exchange->message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(exchange->message);
	soup_message_headers_replace(headers, "Authorization", self->authorization);
	soup_message_headers_replace(headers, "Accept", "application/json");
	soup_message_set_request_body_from_bytes(exchange->message, "application/json", body);

	/* The source does not hold the task: complete() removes it first. */
	exchange->deadline = g_timeout_source_new(MAX(timeout_ms, 100u));
	g_source_set_callback(exchange->deadline, on_deadline, exchange, NULL);
	g_source_attach(exchange->deadline, g_main_context_get_thread_default());
	soup_session_send_async(self->session, exchange->message, G_PRIORITY_DEFAULT, exchange->cancel, on_sent, task);
}

gboolean
axis_worker_client_post_finish(
	AxisWorkerClient  *self,
	GAsyncResult      *result,
	guint             *status,
	GBytes           **body,
	GError           **error
){
	GTask *task = G_TASK(result);
	Exchange *exchange = g_task_get_task_data(task);

	*status = 0;
	*body = NULL;
	if (!g_task_propagate_boolean(task, error))
		return FALSE;
	*status = exchange->status;
	*body = g_bytes_new(exchange->body->data, exchange->body->len);
	return TRUE;
}

AxisWorkerClient *
axis_worker_client_new(
	const AxisWorkerConfig  *config,
	GError                 **error
){
	AxisWorkerClient *self;
	g_autofree gchar *agent = g_strdup_printf("axis-worker/%s", AXIS_VERSION);

	g_return_val_if_fail(config != NULL && config->token != NULL, NULL);

	self = g_new0(AxisWorkerClient, 1);
	/* No proxy (a NULL resolver: the environment is not read), no cookie
	 * jar (none is added), no decoding of what comes back. */
	self->session = soup_session_new_with_options("proxy-resolver", (GProxyResolver *)NULL, "user-agent", agent,
		"timeout", (guint)MAX(config->request_timeout_ms / 1000, 1u), "idle-timeout", 30u, NULL);
	soup_session_remove_feature_by_type(self->session, SOUP_TYPE_CONTENT_DECODER);
	if (config->ca_file != NULL) {
		g_autoptr(GError) local = NULL;
		GTlsDatabase *database = g_tls_file_database_new(config->ca_file, &local);

		if (database == NULL) {
			g_set_error(error, CLIENT_ERROR, 0, "the CA file %s cannot be used: %s", config->ca_file, local->message);
			axis_worker_client_free(self);
			return NULL;
		}
		soup_session_set_tls_database(self->session, database);
		g_object_unref(database);
	}
	self->base = g_strconcat(config->url, AXIS_WORKER_API_PREFIX, NULL);
	self->authorization = g_strconcat("Bearer ", config->token, NULL);
	self->token = g_strdup(config->token);
	return self;
}

void
axis_worker_client_free(AxisWorkerClient *self)
{
	if (self == NULL)
		return;
	if (self->session != NULL) {
		soup_session_abort(self->session);
		g_object_unref(self->session);
	}
	g_free(self->base);
	wipe(self->authorization);
	wipe(self->token);
	g_free(self);
}
