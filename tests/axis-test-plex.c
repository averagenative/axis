/*
 * axis-test-plex.c - test support: a fake Plex Media Server on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-plex.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
	gchar            *path;
	gint              start;
	AxisTestPlexMode  mode;
	guint             status;
	gchar            *text;
	gint64            number;
	GBytes           *bytes;	/* AXIS_TEST_PLEX_BYTES: the body, or NULL */
	gchar           **headers;	/* AXIS_TEST_PLEX_BYTES: name, value, ..., NULL */
} Route;

typedef struct
{
	AxisTestPlex     *owner;
	SoupServerMessage *message;
	GSource          *timer;
	gboolean          stopped;
	gchar            *chunk;
	gsize             chunk_length;
	gint64            interval_ms;
} Stream;

struct _AxisTestPlex
{
	GThread      *thread;
	GMainContext *context;
	GMainLoop    *loop;
	SoupServer   *server;
	GMutex        lock;
	GCond         ready;
	gboolean      started;
	gboolean      failed;
	guint16       port;
	gboolean      tls;
	gchar        *certificate_file;
	gchar        *key_file;
	GPtrArray    *routes;	/* (lock) Route */
	GPtrArray    *requests;	/* (lock) gchar *: the request as text */
	GPtrArray    *methods;	/* (lock) gchar * */
	GPtrArray    *paths;	/* (lock) gchar * (no query) */
	GArray       *times;	/* (lock) gint64: when each request arrived */
	GPtrArray    *streams;	/* (server thread) Stream */
	gint64        base_time;	/* Unix time when started: what @AGE:n@ counts back from */
	AxisTestPlexHandler handler;	/* (lock) */
	gpointer      handler_data;	/* (lock) */
};

static void
route_free(Route *route)
{
	g_free(route->path);
	g_free(route->text);
	if (route->bytes != NULL)
		g_bytes_unref(route->bytes);
	g_strfreev(route->headers);
	g_free(route);
}

/* ── Streams ────────────────────────────────────────────────────── */

static void
stream_stop(Stream *stream)
{
	stream->stopped = TRUE;
	if (stream->timer != NULL) {
		g_source_destroy(stream->timer);
		g_source_unref(stream->timer);
		stream->timer = NULL;
	}
}

static void
stream_free(gpointer data)
{
	Stream *stream = data;

	stream_stop(stream);
	g_free(stream->chunk);
	g_free(stream);
}

static gboolean
stream_tick(gpointer user_data)
{
	Stream *stream = user_data;
	SoupMessageBody *body;

	if (stream->stopped)
		return G_SOURCE_REMOVE;
	body = soup_server_message_get_response_body(stream->message);
	soup_message_body_append(body, SOUP_MEMORY_COPY, stream->chunk, stream->chunk_length);
	soup_server_message_unpause(stream->message);
	return G_SOURCE_CONTINUE;
}

static void
on_request_gone(
	SoupServer        *server,
	SoupServerMessage *message,
	gpointer           user_data
){
	AxisTestPlex *self = user_data;
	guint i;

	for (i = 0; i < self->streams->len; i++) {
		Stream *stream = g_ptr_array_index(self->streams, i);

		if (stream->message == message)
			stream_stop(stream);
	}
}

/* ── Serving ────────────────────────────────────────────────────── */

/* Replaces @AGE:n@ by the Unix time n seconds before @now (the server's
 * start, so that every answer gives the same times). */
static gchar *
substitute(
	const gchar *text,
	gint64       now
){
	GString *out = g_string_new(NULL);
	const gchar *p = text;

	while (*p != '\0') {
		if (strncmp(p, "@AGE:", 5) == 0) {
			gchar *end;
			gint64 seconds = g_ascii_strtoll(p + 5, &end, 10);

			if (*end == '@') {
				g_string_append_printf(out, "%" G_GINT64_FORMAT, now - seconds);
				p = end + 1;
				continue;
			}
		}
		g_string_append_c(out, *p++);
	}
	return g_string_free(out, FALSE);
}

static void
handle(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	AxisTestPlex *self = user_data;
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;
	const gchar *start_text = soup_message_headers_get_one(headers, "X-Plex-Container-Start");
	gint start = start_text != NULL ? atoi(start_text) : 0;
	GUri *uri = soup_server_message_get_uri(message);
	GString *record = g_string_new(NULL);
	g_autoptr(GBytes) request_body = soup_message_body_flatten(soup_server_message_get_request_body(message));
	g_autoptr(GPtrArray) header_list = g_ptr_array_new_with_free_func(g_free);
	AxisTestPlexHandler handler = NULL;
	gpointer handler_data = NULL;
	Route *route = NULL;
	guint i;

	g_string_append_printf(record, "%s %s%s%s\n", soup_server_message_get_method(message), path,
		g_uri_get_query(uri) != NULL ? "?" : "", g_uri_get_query(uri) != NULL ? g_uri_get_query(uri) : "");
	soup_message_headers_iter_init(&iter, headers);
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		g_string_append_printf(record, "%s: %s\n", name, value);
		g_ptr_array_add(header_list, g_strdup(name));
		g_ptr_array_add(header_list, g_strdup(value));
	}
	g_ptr_array_add(header_list, NULL);
	/* The body after an empty line, as on the wire. */
	if (g_bytes_get_size(request_body) > 0) {
		g_string_append_c(record, '\n');
		g_string_append_len(record, g_bytes_get_data(request_body, NULL), (gssize)g_bytes_get_size(request_body));
		g_string_append_c(record, '\n');
	}

	g_mutex_lock(&self->lock);
	handler = self->handler;
	handler_data = self->handler_data;
	g_ptr_array_add(self->requests, g_string_free(record, FALSE));
	g_ptr_array_add(self->methods, g_strdup(soup_server_message_get_method(message)));
	g_ptr_array_add(self->paths, g_strdup(path));
	{
		gint64 now = g_get_monotonic_time();

		g_array_append_val(self->times, now);
	}
	for (i = 0; i < self->routes->len && route == NULL; i++) {
		Route *r = g_ptr_array_index(self->routes, i);

		if (strcmp(r->path, path) == 0 && (r->start < 0 || r->start == start))
			route = r;
	}
	if (route != NULL) {
		Route *copy = g_new0(Route, 1);

		copy->path = g_strdup(route->path);
		copy->mode = route->mode;
		copy->status = route->status;
		copy->text = g_strdup(route->text);
		copy->number = route->number;
		copy->bytes = route->bytes != NULL ? g_bytes_ref(route->bytes) : NULL;
		copy->headers = g_strdupv(route->headers);
		route = copy;
	}
	g_mutex_unlock(&self->lock);

	if (route == NULL && handler != NULL) {
		AxisTestPlexRequest req;
		AxisTestPlexAnswer answer;

		memset(&answer, 0, sizeof(answer));
		req.method = soup_server_message_get_method(message);
		req.path = path;
		req.query = g_uri_get_query(uri);
		req.headers = (const gchar *const *)header_list->pdata;
		req.body = request_body;
		handler(&req, &answer, handler_data);
		if (answer.hang) {
			soup_server_message_pause(message);
		} else {
			SoupMessageHeaders *out = soup_server_message_get_response_headers(message);
			gsize n = 0;
			gconstpointer data = answer.body != NULL ? g_bytes_get_data(answer.body, &n) : NULL;
			guint h;

			soup_server_message_set_status(message, answer.status != 0 ? answer.status : 500, NULL);
			for (h = 0; answer.headers != NULL && answer.headers[h] != NULL && answer.headers[h + 1] != NULL; h += 2)
				soup_message_headers_append(out, answer.headers[h], answer.headers[h + 1]);
			if (answer.content_type != NULL)
				soup_message_headers_replace(out, "Content-Type", answer.content_type);
			soup_message_body_append(soup_server_message_get_response_body(message), SOUP_MEMORY_COPY,
				data != NULL ? data : "", n);
		}
		g_free(answer.content_type);
		if (answer.body != NULL)
			g_bytes_unref(answer.body);
		g_strfreev(answer.headers);
		return;
	}
	if (route == NULL) {
		soup_server_message_set_status(message, 404, NULL);
		return;
	}
	switch (route->mode) {
	case AXIS_TEST_PLEX_NORMAL: {
		g_autofree gchar *body = substitute(route->text != NULL ? route->text : "", self->base_time);

		soup_server_message_set_status(message, route->status, NULL);
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, body, strlen(body));
		break;
	}
	case AXIS_TEST_PLEX_STATUS:
		soup_server_message_set_status(message, (guint)route->number, NULL);
		break;
	case AXIS_TEST_PLEX_BYTES: {
		SoupMessageHeaders *out = soup_server_message_get_response_headers(message);
		gsize n = 0;
		gconstpointer data = route->bytes != NULL ? g_bytes_get_data(route->bytes, &n) : NULL;
		guint h;

		soup_server_message_set_status(message, route->status, NULL);
		/* Appended: a header may be given twice (Set-Cookie). */
		for (h = 0; route->headers != NULL && route->headers[h] != NULL && route->headers[h + 1] != NULL; h += 2)
			soup_message_headers_append(out, route->headers[h], route->headers[h + 1]);
		if (route->text != NULL)
			soup_message_headers_replace(out, "Content-Type", route->text);
		soup_message_body_append(soup_server_message_get_response_body(message), SOUP_MEMORY_COPY,
			data != NULL ? data : "", n);
		break;
	}
	case AXIS_TEST_PLEX_REDIRECT:
		soup_server_message_set_status(message, 302, NULL);
		soup_message_headers_replace(soup_server_message_get_response_headers(message), "Location",
			route->text != NULL ? route->text : "/");
		break;
	case AXIS_TEST_PLEX_OVERSIZE: {
		gsize n = (gsize)route->number;
		gchar *filler = g_malloc(n);

		/* Valid JSON would end in a brace; the size is the point. */
		memset(filler, 'a', n);
		soup_server_message_set_status(message, 200, NULL);
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_TAKE, filler, n);
		break;
	}
	case AXIS_TEST_PLEX_HANG:
		soup_server_message_pause(message);
		break;
	case AXIS_TEST_PLEX_ENDLESS:
	case AXIS_TEST_PLEX_DRIP: {
		Stream *stream = g_new0(Stream, 1);
		SoupMessageBody *body = soup_server_message_get_response_body(message);
		gboolean drip = route->mode == AXIS_TEST_PLEX_DRIP;

		stream->owner = self;
		stream->message = message;
		stream->chunk_length = drip ? 1 : 65536;
		stream->chunk = g_malloc(stream->chunk_length);
		memset(stream->chunk, ' ', stream->chunk_length);
		soup_server_message_set_status(message, 200, NULL);
		soup_message_headers_set_encoding(soup_server_message_get_response_headers(message), SOUP_ENCODING_CHUNKED);
		soup_message_headers_replace(soup_server_message_get_response_headers(message), "Content-Type",
			"application/json");
		soup_message_body_set_accumulate(body, FALSE);
		stream->interval_ms = drip && route->number > 0 ? route->number : 5;
		g_ptr_array_add(self->streams, stream);
		soup_server_message_pause(message);
		stream->timer = g_timeout_source_new((guint)stream->interval_ms);
		g_source_set_callback(stream->timer, stream_tick, stream, NULL);
		g_source_attach(stream->timer, self->context);
		break;
	}
	}
	route_free(route);
}

static gpointer
server_thread(gpointer data)
{
	AxisTestPlex *self = data;
	g_autoptr(GError) error = NULL;
	GSList *uris;
	GTlsCertificate *certificate = NULL;

	g_main_context_push_thread_default(self->context);
	if (self->tls) {
		certificate = g_tls_certificate_new_from_files(self->certificate_file, self->key_file, &error);
		if (certificate == NULL)
			goto failed;
		self->server = soup_server_new("tls-certificate", certificate, NULL);
	} else {
		self->server = soup_server_new(NULL, NULL);
	}
	soup_server_add_handler(self->server, NULL, handle, self, NULL);
	g_signal_connect(self->server, "request-aborted", G_CALLBACK(on_request_gone), self);
	g_signal_connect(self->server, "request-finished", G_CALLBACK(on_request_gone), self);
	if (!soup_server_listen_local(self->server, 0, self->tls ? SOUP_SERVER_LISTEN_HTTPS : 0, &error))
		goto failed;
	uris = soup_server_get_uris(self->server);
	self->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	g_mutex_lock(&self->lock);
	self->started = TRUE;
	g_cond_broadcast(&self->ready);
	g_mutex_unlock(&self->lock);
	g_main_loop_run(self->loop);

	g_ptr_array_set_size(self->streams, 0);
	soup_server_disconnect(self->server);
	g_clear_object(&self->server);
	g_clear_object(&certificate);
	while (g_main_context_iteration(self->context, FALSE))
		;
	g_main_context_pop_thread_default(self->context);
	return NULL;
failed:
	g_clear_object(&certificate);
	g_main_context_pop_thread_default(self->context);
	g_mutex_lock(&self->lock);
	self->failed = TRUE;
	self->started = TRUE;
	g_cond_broadcast(&self->ready);
	g_mutex_unlock(&self->lock);
	return NULL;
}

static AxisTestPlex *
start(
	gboolean     tls,
	const gchar *certificate_file,
	const gchar *key_file
){
	AxisTestPlex *self = g_new0(AxisTestPlex, 1);

	g_mutex_init(&self->lock);
	g_cond_init(&self->ready);
	self->tls = tls;
	self->base_time = (gint64)time(NULL);
	self->certificate_file = g_strdup(certificate_file);
	self->key_file = g_strdup(key_file);
	self->routes = g_ptr_array_new_with_free_func((GDestroyNotify)route_free);
	self->requests = g_ptr_array_new_with_free_func(g_free);
	self->methods = g_ptr_array_new_with_free_func(g_free);
	self->paths = g_ptr_array_new_with_free_func(g_free);
	self->times = g_array_new(FALSE, FALSE, sizeof(gint64));
	self->streams = g_ptr_array_new_with_free_func(stream_free);
	self->context = g_main_context_new();
	self->loop = g_main_loop_new(self->context, FALSE);
	self->thread = g_thread_new("fake-plex", server_thread, self);
	g_mutex_lock(&self->lock);
	while (!self->started)
		g_cond_wait(&self->ready, &self->lock);
	g_mutex_unlock(&self->lock);
	if (self->failed) {
		axis_test_plex_free(self);
		return NULL;
	}
	return self;
}

AxisTestPlex *
axis_test_plex_new(void)
{
	return start(FALSE, NULL, NULL);
}

static gboolean
quit_loop(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

AxisTestPlex *
axis_test_plex_new_tls(
	const gchar *certificate_file,
	const gchar *key_file
){
	return start(TRUE, certificate_file, key_file);
}

void
axis_test_plex_free(AxisTestPlex *self)
{
	if (self == NULL)
		return;
	if (self->thread != NULL) {
		/* Quit from inside the loop: a g_main_loop_quit() made here before
		 * the server thread has reached g_main_loop_run() would be lost, and
		 * the join below would wait for ever (seen under load). */
		g_main_context_invoke(self->context, quit_loop, self->loop);
		g_thread_join(self->thread);
	}
	g_main_loop_unref(self->loop);
	g_main_context_unref(self->context);
	g_ptr_array_unref(self->routes);
	g_ptr_array_unref(self->requests);
	g_ptr_array_unref(self->methods);
	g_ptr_array_unref(self->paths);
	g_array_unref(self->times);
	g_ptr_array_unref(self->streams);
	g_free(self->certificate_file);
	g_free(self->key_file);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->ready);
	g_free(self);
}

guint16
axis_test_plex_port(AxisTestPlex *self)
{
	return self->port;
}

gchar *
axis_test_plex_base(AxisTestPlex *self)
{
	return g_strdup_printf("%s://127.0.0.1:%u", self->tls ? "https" : "http", self->port);
}

/* ── Routes ─────────────────────────────────────────────────────── */

void
axis_test_plex_route(
	AxisTestPlex     *self,
	const gchar      *path,
	gint              start,
	AxisTestPlexMode  mode,
	guint             status,
	const gchar      *text,
	gint64            number
){
	Route *route = g_new0(Route, 1);

	route->path = g_strdup(path);
	route->start = start;
	route->mode = mode;
	route->status = status;
	route->text = g_strdup(text);
	route->number = number;
	g_mutex_lock(&self->lock);
	/* A later route for the same request replaces an earlier one. */
	g_ptr_array_insert(self->routes, 0, route);
	g_mutex_unlock(&self->lock);
}

void
axis_test_plex_route_full(
	AxisTestPlex       *self,
	const gchar        *path,
	guint               status,
	const gchar        *content_type,
	GBytes             *body,
	const gchar *const *headers
){
	Route *route = g_new0(Route, 1);

	route->path = g_strdup(path);
	route->start = -1;
	route->mode = AXIS_TEST_PLEX_BYTES;
	route->status = status;
	route->text = g_strdup(content_type);
	route->bytes = body != NULL ? g_bytes_ref(body) : NULL;
	route->headers = g_strdupv((gchar **)headers);
	g_mutex_lock(&self->lock);
	g_ptr_array_insert(self->routes, 0, route);
	g_mutex_unlock(&self->lock);
}

void
axis_test_plex_route_file(
	AxisTestPlex *self,
	const gchar  *path,
	gint          start,
	const gchar  *fixture
){
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_get_contents(fixture, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", fixture, error->message);
	axis_test_plex_route(self, path, start, AXIS_TEST_PLEX_NORMAL, 200, text, 0);
}

void
axis_test_plex_clear_routes(AxisTestPlex *self)
{
	g_mutex_lock(&self->lock);
	g_ptr_array_set_size(self->routes, 0);
	g_mutex_unlock(&self->lock);
}

void
axis_test_plex_set_handler(
	AxisTestPlex        *self,
	AxisTestPlexHandler  handler,
	gpointer             user_data
){
	g_mutex_lock(&self->lock);
	self->handler = handler;
	self->handler_data = user_data;
	g_mutex_unlock(&self->lock);
}

const gchar *
axis_test_plex_request_header(
	const AxisTestPlexRequest *request,
	const gchar               *name
){
	guint i;

	for (i = 0; request->headers != NULL && request->headers[i] != NULL && request->headers[i + 1] != NULL; i += 2) {
		if (g_ascii_strcasecmp(request->headers[i], name) == 0)
			return request->headers[i + 1];
	}
	return NULL;
}

void
axis_test_plex_standard(AxisTestPlex *self)
{
	axis_test_plex_route_file(self, "/identity", -1, "tests/fixtures/compat/identity.json");
	axis_test_plex_route_file(self, "/", -1, "tests/fixtures/compat/constructed/root.json");
	axis_test_plex_route_file(self, "/library/sections", -1, "tests/fixtures/compat/sections.json");
	axis_test_plex_route_file(self, "/devices", -1, "tests/fixtures/compat/devices.json");
	axis_test_plex_route_file(self, "/accounts", -1, "tests/fixtures/compat/accounts.json");
	axis_test_plex_route_file(self, "/status/sessions/history/all", 0, "tests/fixtures/compat/history-1.json");
	axis_test_plex_route_file(self, "/status/sessions/history/all", 3, "tests/fixtures/compat/history-2.json");
	axis_test_plex_route_file(self, "/status/sessions", -1, "tests/fixtures/compat/sessions-empty.json");
}

/* ── What it received ───────────────────────────────────────────── */

guint
axis_test_plex_n_requests(AxisTestPlex *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->requests->len;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_plex_count(
	AxisTestPlex *self,
	const gchar  *path
){
	guint n = 0;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->paths->len; i++)
		n += strcmp(g_ptr_array_index(self->paths, i), path) == 0 ? 1 : 0;
	g_mutex_unlock(&self->lock);
	return n;
}

gboolean
axis_test_plex_saw_non_get(AxisTestPlex *self)
{
	gboolean other = FALSE;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->methods->len; i++)
		other = other || strcmp(g_ptr_array_index(self->methods, i), "GET") != 0;
	g_mutex_unlock(&self->lock);
	return other;
}

gchar *
axis_test_plex_transcript(AxisTestPlex *self)
{
	GString *out = g_string_new(NULL);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->requests->len; i++) {
		g_string_append(out, g_ptr_array_index(self->requests, i));
		g_string_append_c(out, '\n');
	}
	g_mutex_unlock(&self->lock);
	return g_string_free(out, FALSE);
}

gchar *
axis_test_plex_last_header(
	AxisTestPlex *self,
	const gchar  *path,
	const gchar  *name
){
	gchar *found = NULL;
	guint i;

	g_mutex_lock(&self->lock);
	for (i = self->requests->len; i > 0 && found == NULL; i--) {
		if (strcmp(g_ptr_array_index(self->paths, i - 1), path) == 0) {
			g_auto(GStrv) lines = g_strsplit(g_ptr_array_index(self->requests, i - 1), "\n", -1);
			g_autofree gchar *prefix = g_strdup_printf("%s: ", name);
			guint j;

			/* The headers end at the empty line before a body. */
			for (j = 1; lines[j] != NULL && lines[j][0] != '\0'; j++) {
				if (g_ascii_strncasecmp(lines[j], prefix, strlen(prefix)) == 0)
					found = g_strdup(lines[j] + strlen(prefix));
			}
			break;
		}
	}
	g_mutex_unlock(&self->lock);
	return found;
}

void
axis_test_plex_reset_requests(AxisTestPlex *self)
{
	g_mutex_lock(&self->lock);
	g_ptr_array_set_size(self->requests, 0);
	g_ptr_array_set_size(self->methods, 0);
	g_ptr_array_set_size(self->paths, 0);
	g_array_set_size(self->times, 0);
	g_mutex_unlock(&self->lock);
}

GArray *
axis_test_plex_request_times(AxisTestPlex *self)
{
	GArray *copy = g_array_new(FALSE, FALSE, sizeof(gint64));

	g_mutex_lock(&self->lock);
	g_array_append_vals(copy, self->times->data, self->times->len);
	g_mutex_unlock(&self->lock);
	return copy;
}

GPtrArray *
axis_test_plex_requests(AxisTestPlex *self)
{
	GPtrArray *copy = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->requests->len; i++)
		g_ptr_array_add(copy, g_strdup(g_ptr_array_index(self->requests, i)));
	g_mutex_unlock(&self->lock);
	return copy;
}
