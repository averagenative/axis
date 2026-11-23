/*
 * axis-web-hub.c - server-sent events: topics, subscribers and limits
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * htmx-glib gives one HtmxSseConnection per client and no way to tell how
 * much of what was queued on it the client has read.  The hub counts the
 * chunks it queues and the chunks libsoup reports written
 * ("wrote-chunk"), which is the measure of a client that has stopped
 * reading.
 */

#include "web/axis-web-hub.h"
#include "web/axis-web-auth-private.h"

#include <string.h>

typedef struct
{
	AxisWebHub        *hub;
	HtmxSseConnection *conn;		/* owned */
	SoupServerMessage *message;		/* owned */
	gulong             wrote_id;
	gchar             *session;		/* token hash, owned */
	gint64             user_id;		/* the session's account */
	gchar            **topics;		/* owned */
	GQueue             sizes;		/* sizes of the chunks queued and not yet written */
	gsize              pending;		/* their sum */
	gint64             last_send_us;	/* monotonic */
	gint64             last_progress_us;	/* the last time the queue shrank, or started */
} Stream;

struct _AxisWebHub
{
	guint        max_per_session;
	guint        max_per_user;
	guint        max_streams;
	guint        admin_reserved;	/* of max_streams, only administrators may use */
	gint64       keepalive_us;
	gint64       stall_us;
	gsize        max_pending;

	GHashTable  *topics;		/* topic -> GPtrArray of Stream* */
	GHashTable  *by_session;	/* token hash -> GQueue of Stream*, oldest first */
	GHashTable  *by_user;		/* gint64 account -> GQueue of Stream*, oldest first */
	GHashTable  *live;		/* set of Stream* */
	guint        n_streams;
	gsize        pending_total;
	guint        tick_id;

	AxisWebAuth *auth;		/* nullable */
};

static void stream_close(Stream *stream);

/* ── Streams ────────────────────────────────────────────────────── */

static void
queue_free(gpointer data)
{
	g_queue_free(data);
}

static void
stream_unlink(Stream *stream)
{
	AxisWebHub *hub = stream->hub;
	GQueue *queue;
	guint i;

	g_hash_table_remove(hub->live, stream);

	for (i = 0; stream->topics[i] != NULL; i++) {
		GPtrArray *list = g_hash_table_lookup(hub->topics, stream->topics[i]);

		if (list != NULL && g_ptr_array_remove_fast(list, stream) && list->len == 0)
			g_hash_table_remove(hub->topics, stream->topics[i]);
	}

	queue = g_hash_table_lookup(hub->by_session, stream->session);
	if (queue != NULL) {
		g_queue_remove(queue, stream);
		if (g_queue_is_empty(queue))
			g_hash_table_remove(hub->by_session, stream->session);
	}

	queue = g_hash_table_lookup(hub->by_user, &stream->user_id);
	if (queue != NULL) {
		g_queue_remove(queue, stream);
		if (g_queue_is_empty(queue))
			g_hash_table_remove(hub->by_user, &stream->user_id);
	}

	hub->pending_total -= stream->pending;
	hub->n_streams--;
}

static void
on_stream_closed(
	HtmxSseConnection *conn,
	gpointer           user_data
){
	Stream *stream = user_data;
	AxisWebHub *hub = stream->hub;

	stream_unlink(stream);

	g_signal_handlers_disconnect_by_data(conn, stream);
	if (stream->wrote_id != 0)
		g_signal_handler_disconnect(stream->message, stream->wrote_id);

	g_object_unref(stream->conn);
	g_object_unref(stream->message);
	g_queue_clear(&stream->sizes);
	g_free(stream->session);
	g_strfreev(stream->topics);
	g_free(stream);

	if (hub->n_streams == 0 && hub->tick_id != 0) {
		g_source_remove(hub->tick_id);
		hub->tick_id = 0;
	}
}

/* Ends @stream; its "closed" signal does the rest, before this returns. */
static void
stream_close(Stream *stream)
{
	htmx_sse_connection_close(stream->conn);
}

/*
 * Ends @stream whose client is not reading.  A graceful close would leave
 * libsoup holding everything queued for it until the client read it (or
 * for ever), so the connection is taken from libsoup and closed, which
 * discards what was queued.
 */
static void
stream_drop(Stream *stream)
{
	AxisWebHub *hub = stream->hub;
	GIOStream *io = soup_server_message_steal_connection(stream->message);

	if (io != NULL) {
		g_io_stream_close(io, NULL, NULL);
		g_object_unref(io);
	}

	/* Stealing may already have ended the stream. */
	if (g_hash_table_contains(hub->live, stream))
		stream_close(stream);
}

/* libsoup wrote one of the chunks queued on @stream to the socket. */
static void
on_wrote_chunk(
	SoupServerMessage *message,
	gpointer           user_data
){
	Stream *stream = user_data;
	gsize size;

	if (g_queue_is_empty(&stream->sizes))
		return;

	size = GPOINTER_TO_SIZE(g_queue_pop_head(&stream->sizes));
	stream->pending -= size;
	stream->hub->pending_total -= size;
	stream->last_progress_us = g_get_monotonic_time();
}

/* Counts a chunk of @size queued on @stream; drops the stream when the
 * client is too far behind.  Returns %FALSE if it was dropped. */
static gboolean
stream_queued(
	Stream *stream,
	gsize   size
){
	AxisWebHub *hub = stream->hub;
	gint64 now = g_get_monotonic_time();

	if (g_queue_is_empty(&stream->sizes))
		stream->last_progress_us = now;
	stream->last_send_us = now;
	g_queue_push_tail(&stream->sizes, GSIZE_TO_POINTER(size));
	stream->pending += size;
	hub->pending_total += size;

	if (stream->pending > hub->max_pending) {
		g_debug("web hub: closing a stream whose client has %" G_GSIZE_FORMAT
			" bytes unread", stream->pending);
		stream_drop(stream);
		return FALSE;
	}

	return TRUE;
}

/* What send_event() writes for @event and @data, to the byte. */
static gsize
frame_size(
	const gchar *event,
	const gchar *data
){
	gsize lines = 1;
	const gchar *p;

	for (p = data; *p != '\0'; p++) {
		if (*p == '\n')
			lines++;
	}

	return 7 + strlen(event) + 1 + (strlen(data) - (lines - 1)) + 7 * lines + 1;
}

/*
 * Whether the client has hung up.  libsoup does not watch the socket of a
 * paused message, so a tab closed while its stream is idle would be found
 * out only by a failed write, a keep-alive later.  A client sends nothing
 * once its request is made, so readable (the end of the stream, or
 * anything else) or a hang-up or error means it is gone.
 */
static gboolean
peer_gone(Stream *stream)
{
	GSocket *socket = soup_server_message_get_socket(stream->message);

	if (socket == NULL)
		return FALSE;

	return (g_socket_condition_check(socket, G_IO_IN | G_IO_HUP | G_IO_ERR) &
	        (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0;
}

static gboolean
on_tick(gpointer user_data)
{
	AxisWebHub *hub = user_data;
	g_autoptr(GPtrArray) streams = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	gint64 now = g_get_monotonic_time();
	guint i;

	g_hash_table_iter_init(&iter, hub->live);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(streams, key);

	for (i = 0; i < streams->len; i++) {
		Stream *stream = g_ptr_array_index(streams, i);

		if (!g_hash_table_contains(hub->live, stream))
			continue;

		if (peer_gone(stream)) {
			stream_drop(stream);
		} else if (stream->pending > 0 && now - stream->last_progress_us >= hub->stall_us) {
			g_debug("web hub: closing a stream whose client stopped reading");
			stream_drop(stream);
		} else if (now - stream->last_send_us >= hub->keepalive_us) {
			htmx_sse_connection_send_comment(stream->conn, "keep-alive");
			stream_queued(stream, 2 + strlen("keep-alive") + 2);
		}
	}

	return G_SOURCE_CONTINUE;
}

/* ── Hub ────────────────────────────────────────────────────────── */

AxisWebHub *
axis_web_hub_new(const AxisWebHubOptions *options)
{
	AxisWebHub *self = g_new0(AxisWebHub, 1);
	guint keepalive_ms = AXIS_WEB_HUB_KEEPALIVE_MS;
	guint stall_ms = AXIS_WEB_HUB_STALL_MS;

	self->max_per_session = AXIS_WEB_HUB_MAX_STREAMS_PER_SESSION;
	self->max_per_user = AXIS_WEB_HUB_MAX_STREAMS_PER_USER;
	self->max_streams = AXIS_WEB_HUB_MAX_STREAMS;
	self->max_pending = AXIS_WEB_HUB_MAX_PENDING_BYTES;
	if (options != NULL) {
		if (options->max_per_session > 0)
			self->max_per_session = options->max_per_session;
		if (options->max_per_user > 0)
			self->max_per_user = options->max_per_user;
		if (options->max_streams > 0)
			self->max_streams = options->max_streams;
		if (options->max_pending_bytes > 0)
			self->max_pending = options->max_pending_bytes;
		if (options->keepalive_ms > 0)
			keepalive_ms = options->keepalive_ms;
		if (options->stall_ms > 0)
			stall_ms = options->stall_ms;
	}
	self->keepalive_us = (gint64)keepalive_ms * 1000;
	self->stall_us = (gint64)stall_ms * 1000;
	/* A quarter of the total by default; never all of it. */
	self->admin_reserved = options != NULL && options->admin_reserved > 0 ? options->admin_reserved :
		self->max_streams / AXIS_WEB_HUB_ADMIN_RESERVED_PART;
	self->admin_reserved = MIN(self->admin_reserved, self->max_streams - 1);

	self->topics = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	self->by_session = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, queue_free);
	self->by_user = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, queue_free);
	self->live = g_hash_table_new(g_direct_hash, g_direct_equal);

	return self;
}

void
axis_web_hub_free(AxisWebHub *self)
{
	g_autoptr(GPtrArray) streams = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	guint i;

	if (self == NULL)
		return;

	if (self->auth != NULL) {
		axis_session_store_set_end_func(self->auth->sessions, NULL, NULL);
		axis_session_store_set_role_func(self->auth->sessions, NULL, NULL);
	}

	g_hash_table_iter_init(&iter, self->live);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(streams, key);
	for (i = 0; i < streams->len; i++)
		stream_close(g_ptr_array_index(streams, i));

	if (self->tick_id != 0)
		g_source_remove(self->tick_id);
	g_hash_table_unref(self->live);
	g_hash_table_unref(self->by_session);
	g_hash_table_unref(self->by_user);
	g_hash_table_unref(self->topics);
	g_free(self);
}

guint
axis_web_hub_count_subscribers(
	AxisWebHub  *self,
	const gchar *topic
){
	GPtrArray *list;

	g_return_val_if_fail(self != NULL, 0);

	list = topic != NULL ? g_hash_table_lookup(self->topics, topic) : NULL;
	return list != NULL ? list->len : 0;
}

guint
axis_web_hub_get_n_streams(AxisWebHub *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->n_streams;
}

gsize
axis_web_hub_get_pending_bytes(AxisWebHub *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->pending_total;
}

guint
axis_web_hub_publish(
	AxisWebHub  *self,
	const gchar *topic,
	const gchar *event_name,
	const gchar *html
){
	g_autoptr(GPtrArray) targets = NULL;
	g_autofree gchar *data = NULL;
	GPtrArray *list;
	gsize size;
	guint delivered = 0;
	guint i;

	g_return_val_if_fail(self != NULL, 0);
	g_return_val_if_fail(axis_web_hub_topic_is_valid(topic), 0);
	g_return_val_if_fail(axis_web_hub_event_name_is_valid(event_name), 0);
	g_return_val_if_fail(html != NULL, 0);

	list = g_hash_table_lookup(self->topics, topic);
	if (list == NULL)
		return 0;

	if (strlen(html) > AXIS_WEB_HUB_MAX_EVENT_BYTES) {
		g_warning("web hub: not publishing a fragment of %" G_GSIZE_FORMAT
			" bytes on '%s'", strlen(html), topic);
		return 0;
	}

	data = g_strdup(html);
	g_strdelimit(data, "\r", '\n');
	size = frame_size(event_name, data);

	/* Sending can close a stream (a client too far behind), which
	 * changes the list: work from a copy and check each is still there. */
	targets = g_ptr_array_new();
	for (i = 0; i < list->len; i++)
		g_ptr_array_add(targets, g_ptr_array_index(list, i));

	for (i = 0; i < targets->len; i++) {
		Stream *stream = g_ptr_array_index(targets, i);

		if (!g_hash_table_contains(self->live, stream))
			continue;

		htmx_sse_connection_send_event(stream->conn, event_name, data, NULL);
		if (stream_queued(stream, size))
			delivered++;
	}

	return delivered;
}

/* ── GET /events ────────────────────────────────────────────────── */

static void
on_session_ended(
	const gchar *token_hash,
	gpointer     user_data
){
	AxisWebHub *self = user_data;
	GQueue *queue = g_hash_table_lookup(self->by_session, token_hash);
	g_autoptr(GPtrArray) streams = NULL;
	GList *l;
	guint i;

	if (queue == NULL)
		return;

	streams = g_ptr_array_new();
	for (l = queue->head; l != NULL; l = l->next)
		g_ptr_array_add(streams, l->data);
	for (i = 0; i < streams->len; i++)
		stream_close(g_ptr_array_index(streams, i));
}

/*
 * An account's role changed: every stream it has was let in under the old
 * one (its ":admin" topics, its share of the total), so each is closed.
 * The browser opens it again at once, and that request is judged by the
 * role the account has now.
 */
static void
on_role_changed(
	gint64   user_id,
	gpointer user_data
){
	AxisWebHub *self = user_data;
	GQueue *queue = g_hash_table_lookup(self->by_user, &user_id);
	g_autoptr(GPtrArray) streams = NULL;
	GList *l;
	guint i;

	if (queue == NULL)
		return;

	streams = g_ptr_array_new();
	for (l = queue->head; l != NULL; l = l->next)
		g_ptr_array_add(streams, l->data);
	for (i = 0; i < streams->len; i++)
		stream_close(g_ptr_array_index(streams, i));
}

#define STATUS_OFF \
	"<p id=\"" AXIS_WEB_HUB_STATUS_ID "\" class=\"notice notice-error\" role=\"status\" hx-swap-oob=\"true\">" \
	"Live updates are off in this tab: your account already has the most live pages open that Axis " \
	"allows. Close another Axis tab, or reload this page to see changes. Axis tries again every minute.</p>"
#define STATUS_ON \
	"<p id=\"" AXIS_WEB_HUB_STATUS_ID "\" hidden=\"hidden\" hx-swap-oob=\"true\"></p>"

/*
 * A stream refused at the account's cap: answered as a stream (so that the
 * browser waits the stream's retry, a minute, rather than its own back-off
 * of an error), with the line that says live updates are off in this tab,
 * and ended at once.  It holds nothing.
 */
static HtmxResponse *
refuse_stream(SoupServerMessage *message)
{
	HtmxSseConnection *conn = htmx_sse_connection_new(message);

	htmx_sse_connection_set_retry(conn, AXIS_WEB_HUB_REFUSED_RETRY_MS);
	htmx_sse_connection_send_event(conn, "oob", STATUS_OFF, NULL);
	htmx_sse_connection_close(conn);
	g_object_unref(conn);
	return htmx_response_new_streaming();
}

/* Closes the oldest streams of @queue_of (by_session or by_user) under
 * @key until it has fewer than @max. */
static void
make_room_in(
	AxisWebHub    *self,
	GHashTable    *queue_of,
	gconstpointer  key,
	guint          max
){
	GQueue *queue = g_hash_table_lookup(queue_of, key);

	while (queue != NULL && g_queue_get_length(queue) >= max) {
		stream_close(g_queue_peek_head(queue));
		queue = g_hash_table_lookup(queue_of, key);
	}
}

static HtmxResponse *
handle_events(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebHub *self = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	SoupServerMessage *message = htmx_request_get_message(request);
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) topics = NULL;
	HtmxResponse *response;
	Stream *stream;
	GQueue *queue;
	gboolean admin;
	guint limit;
	guint i;

	if (state == NULL || state->token_hash == NULL || message == NULL) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"an event stream was asked for without a session or a connection");
		return axis_web_internal_error(request, error);
	}

	if (htmx_request_get_method(request) != HTMX_METHOD_GET) {
		response = axis_web_error_respond(request, 405, "Not a page to open",
			"An event stream is read with GET.");
		htmx_response_add_header(response, "Allow", "GET");
		return response;
	}

	topics = axis_web_hub_parse_topics(htmx_request_get_query(request), &error);
	if (topics == NULL)
		return axis_web_error_respond(request, 400, "Bad request",
			"The topics asked for are missing or not valid.");

	/* A topic whose name ends in ":admin" is for administrators only.  The
	 * role is the account's now (build_state() reads it on every request). */
	admin = state->role == AXIS_USER_ROLE_ADMIN;
	for (i = 0; topics[i] != NULL; i++) {
		if (g_str_has_suffix(topics[i], AXIS_WEB_HUB_ADMIN_SUFFIX) && !admin)
			return axis_web_error_respond(request, 403, "Administrators only",
				"This live update is only for administrators.");
	}

	/* The oldest of the session's streams makes room for this one (a page
	 * left by navigating, not yet seen gone).  At the account's cap the new
	 * one is refused instead, so that tabs do not end each other's streams
	 * without end: one account, however many sessions it opens, holds no
	 * more than its own share. */
	make_room_in(self, self->by_session, state->token_hash, self->max_per_session);
	queue = g_hash_table_lookup(self->by_user, &state->user_id);
	if (queue != NULL && g_queue_get_length(queue) >= self->max_per_user)
		return refuse_stream(message);

	/* The last part of the total is kept for administrators, so that no
	 * number of other accounts can take every stream. */
	limit = admin ? self->max_streams : self->max_streams - self->admin_reserved;
	if (self->n_streams >= limit) {
		response = axis_web_error_respond(request, 503, "Axis is busy",
			"Too many pages are following live updates. Try again in a few seconds.");
		htmx_response_add_header(response, "Retry-After", "5");
		return response;
	}

	stream = g_new0(Stream, 1);
	stream->hub = self;
	stream->message = g_object_ref(message);
	stream->session = g_strdup(state->token_hash);
	stream->user_id = state->user_id;
	stream->topics = g_steal_pointer(&topics);
	g_queue_init(&stream->sizes);
	stream->last_send_us = stream->last_progress_us = g_get_monotonic_time();

	stream->conn = htmx_sse_connection_new(message);
	g_signal_connect(stream->conn, "closed", G_CALLBACK(on_stream_closed), stream);
	stream->wrote_id = g_signal_connect(message, "wrote-chunk", G_CALLBACK(on_wrote_chunk), stream);

	g_hash_table_add(self->live, stream);
	for (i = 0; stream->topics[i] != NULL; i++) {
		GPtrArray *list = g_hash_table_lookup(self->topics, stream->topics[i]);

		if (list == NULL) {
			list = g_ptr_array_new();
			g_hash_table_insert(self->topics, g_strdup(stream->topics[i]), list);
		}
		g_ptr_array_add(list, stream);
	}
	queue = g_hash_table_lookup(self->by_session, stream->session);
	if (queue == NULL) {
		queue = g_queue_new();
		g_hash_table_insert(self->by_session, g_strdup(stream->session), queue);
	}
	g_queue_push_tail(queue, stream);
	queue = g_hash_table_lookup(self->by_user, &stream->user_id);
	if (queue == NULL) {
		gint64 *key = g_new(gint64, 1);

		*key = stream->user_id;
		queue = g_queue_new();
		g_hash_table_insert(self->by_user, key, queue);
	}
	g_queue_push_tail(queue, stream);
	self->n_streams++;

	if (self->tick_id == 0) {
		guint interval = (guint)MIN(MIN(self->keepalive_us, self->stall_us) / 4000, 1000);

		self->tick_id = g_timeout_add(MAX(interval, 20), on_tick, self);
	}

	/* The comment sends the headers, so the browser knows the stream is
	 * open; the retry tells it how soon to try again after a break. */
	htmx_sse_connection_set_retry(stream->conn, 3000);
	if (stream_queued(stream, strlen("retry: 3000\n\n"))) {
		/* A tab refused before gets its live updates now. */
		htmx_sse_connection_send_event(stream->conn, "oob", STATUS_ON, NULL);
		if (stream_queued(stream, frame_size("oob", STATUS_ON))) {
			htmx_sse_connection_send_comment(stream->conn, "open");
			stream_queued(stream, 2 + strlen("open") + 2);
		}
	}

	return htmx_response_new_streaming();
}

void
axis_web_hub_install(
	AxisWebHub  *self,
	AxisWeb     *web,
	AxisWebAuth *auth
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(web != NULL);
	g_return_if_fail(auth != NULL);
	g_return_if_fail(self->auth == NULL);

	self->auth = auth;
	axis_session_store_set_end_func(auth->sessions, on_session_ended, self);
	axis_session_store_set_role_func(auth->sessions, on_role_changed, self);
	axis_web_set_hub(web, self);
	axis_web_get(web, AXIS_WEB_HUB_EVENTS_PATH, handle_events, self);
}
