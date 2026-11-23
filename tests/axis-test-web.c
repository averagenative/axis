/*
 * axis-test-web.c - test support for the web pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-web.h"
#include "axis-test-jobs.h"
#include "web/axis-web-attention.h"

#include <stdio.h>
#include <string.h>

guint16
axis_test_web_port(HtmxServer *server)
{
	GSList *uris = htmx_server_get_listen_uris(server);
	const gchar *colon;
	guint16 port;

	g_assert_nonnull(uris);
	colon = strrchr(uris->data, ':');
	g_assert_nonnull(colon);
	port = (guint16)g_ascii_strtoull(colon + 1, NULL, 10);
	g_assert_cmpuint(port, !=, 0);
	g_slist_free_full(uris, g_free);

	return port;
}

/* ── Requests ───────────────────────────────────────────────────── */

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} Pending;

static void
on_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Pending *pending = user_data;

	pending->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	g_main_loop_quit(pending->loop);
}

/*
 * Spec (web-shell): every page of Media management and of System opens
 * with what it is for: its heading, then the muted intro.  Checked of every
 * whole page any web test fetches: a page its tab's row marks as the one
 * open (a page below one of the row's, such as one profile, is not).
 */
static void
check_page_intro(
	const gchar *path,
	const gchar *html
){
	static const gchar *const rows[] = {
		"aria-label=\"Library pages\"", "aria-label=\"System pages\"", NULL
	};
	guint i;

	for (i = 0; rows[i] != NULL; i++) {
		const gchar *start = strstr(html, rows[i]);
		const gchar *end;
		const gchar *heading;
		g_autofree gchar *row = NULL;

		if (start == NULL)
			continue;
		end = strstr(start, "</nav>");
		g_assert_nonnull(end);
		row = g_strndup(start, (gsize)(end - start));
		if (strstr(row, "aria-current=\"page\"") == NULL)
			return;
		/* (A test's own stand-in page, with no heading, is not one.) */
		heading = strstr(html, "</h1>");
		if (heading != NULL && !g_str_has_prefix(heading + strlen("</h1>"), "<p class=\"page-intro\">"))
			g_error("%s opens without saying what it is for (a <p class=\"page-intro\"> under its heading)", path);
		return;
	}
}

AxisTestReply
axis_test_web_request(
	SoupSession        *session,
	guint16             port,
	const gchar        *method,
	const gchar        *path,
	const gchar        *cookie,
	const gchar *const *extra,
	const gchar        *form
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	GError *error = NULL;
	GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, &error);
	SoupMessage *message;
	SoupMessageHeaders *headers;
	Pending pending;
	AxisTestReply reply;
	gsize size;
	gconstpointer data;
	guint i;

	g_assert_no_error(error);
	message = soup_message_new_from_uri(method, uri);
	g_uri_unref(uri);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	headers = soup_message_get_request_headers(message);
	if (cookie != NULL)
		soup_message_headers_append(headers, "Cookie", cookie);
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, extra[i], extra[i + 1]);
	if (form != NULL) {
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}

	memset(&pending, 0, sizeof(pending));
	pending.loop = g_main_loop_new(NULL, FALSE);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, on_done, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);

	g_assert_no_error(pending.error);
	g_assert_nonnull(pending.bytes);

	data = g_bytes_get_data(pending.bytes, &size);
	reply.message = message;
	reply.status = soup_message_get_status(message);
	/* Every byte, a binary body included (g_strndup() would stop at a
	 * zero byte), and one more as the terminator that text tests rely on. */
	reply.body = g_malloc(size + 1);
	if (size > 0)
		memcpy(reply.body, data, size);
	reply.body[size] = '\0';
	reply.length = size;
	g_bytes_unref(pending.bytes);

	if (reply.status == 200 && g_str_has_prefix(reply.body, "<!DOCTYPE html>"))
		check_page_intro(path, reply.body);
	return reply;
}

void
axis_test_reply_clear(AxisTestReply *reply)
{
	g_clear_object(&reply->message);
	g_clear_pointer(&reply->body, g_free);
}

const gchar *
axis_test_reply_header(
	AxisTestReply *reply,
	const gchar   *name
){
	return soup_message_headers_get_one(soup_message_get_response_headers(reply->message), name);
}

gchar *
axis_test_web_csrf_token(const gchar *html)
{
	static const gchar marker[] = "X-CSRF-Token&quot;:&quot;";
	const gchar *start = strstr(html, marker);
	const gchar *end;

	if (start == NULL)
		return NULL;
	start += strlen(marker);
	end = strstr(start, "&quot;");

	return end != NULL ? g_strndup(start, (gsize)(end - start)) : NULL;
}

gchar *
axis_test_web_unbroken(const gchar *html)
{
	g_auto(GStrv) pieces = g_strsplit(html, "<wbr />", -1);

	return g_strjoinv("", pieces);
}

/* The class attribute's value in the tag @tag ("<...>"), or NULL. */
static gchar *
class_of(const gchar *tag)
{
	const gchar *start = strstr(tag, " class=\"");
	const gchar *end;

	if (start == NULL)
		return NULL;
	start += strlen(" class=\"");
	end = strchr(start, '"');
	return end != NULL ? g_strndup(start, (gsize)(end - start)) : NULL;
}

void
axis_test_web_read_attention(gpointer web)
{
	axis_test_web_read_attention_at(web, 0);
}

void
axis_test_web_read_attention_at(
	gpointer web,
	gint     minutes
){
	AxisAttentionTuning tuning;
	guint round;

	memset(&tuning, 0, sizeof(tuning));
	tuning.min_gap_ms = 20;
	tuning.period_ms = 3600 * 1000;
	tuning.deadline_ms = 10000;
	tuning.clock_offset_us = (gint64)minutes * 60 * G_USEC_PER_SEC;
	axis_web_attention_set_tuning(web, &tuning);
	axis_web_attention_start(web, NULL);
	for (round = 0; round < 2; round++) {
		guint before = axis_web_attention_get_readings(web);
		gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;

		axis_web_attention_changed(web);
		while (axis_web_attention_get_readings(web) == before) {
			g_assert_cmpint(g_get_monotonic_time(), <, deadline);
			g_main_context_iteration(NULL, TRUE);
		}
	}
}

guint
axis_test_web_assert_tables(const gchar *html)
{
	g_autoptr(GHashTable) kinds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	const gchar *p = html;
	guint tables = 0;

	while ((p = strstr(p, "<table")) != NULL) {
		const gchar *tag_end = strchr(p, '>');
		const gchar *group;
		const gchar *group_end;
		const gchar *head;
		const gchar *head_end;
		const gchar *q;
		g_autofree gchar *tag = NULL;
		g_autofree gchar *classes = NULL;
		g_autofree gchar *colgroup = NULL;
		g_autofree gchar *kind = NULL;
		g_autoptr(GPtrArray) roles = g_ptr_array_new_with_free_func(g_free);
		const gchar *k;
		guint headings = 0;

		g_assert_nonnull(tag_end);
		tag = g_strndup(p, (gsize)(tag_end - p + 1));
		classes = class_of(tag);
		if (classes == NULL || strstr(classes, "data-table") == NULL)
			g_error("a table that is not a data table: %s", tag);
		k = strstr(classes, "kind-");
		if (k == NULL)
			g_error("a data table without a kind: %s", tag);
		kind = g_strndup(k, strcspn(k, " "));

		/* The colgroup comes first: one role per column. */
		group = tag_end + 1;
		if (!g_str_has_prefix(group, "<colgroup>"))
			g_error("table %s has no colgroup first", kind);
		group_end = strstr(group, "</colgroup>");
		g_assert_nonnull(group_end);
		colgroup = g_strndup(group, (gsize)(group_end - group));
		for (q = colgroup; (q = strstr(q, "<col ")) != NULL; q++) {
			const gchar *e = strchr(q, '>');
			g_autofree gchar *col = g_strndup(q, (gsize)(e - q + 1));
			gchar *role = class_of(col);

			if (role == NULL || !g_str_has_prefix(role, "col-"))
				g_error("table %s: a column without a role: %s", kind, col);
			g_ptr_array_add(roles, role);
		}
		if (roles->len == 0)
			g_error("table %s has no columns", kind);

		/* Its headings, each with its column's role. */
		head = strstr(group_end, "<thead>");
		g_assert_nonnull(head);
		head_end = strstr(head, "</thead>");
		g_assert_nonnull(head_end);
		for (q = head; (q = strstr(q, "<th ")) != NULL && q < head_end; q++) {
			const gchar *e = strchr(q, '>');
			g_autofree gchar *th = g_strndup(q, (gsize)(e - q + 1));
			g_autofree gchar *role = class_of(th);

			if (headings >= roles->len)
				g_error("table %s has more headings than columns", kind);
			if (g_strcmp0(role, g_ptr_array_index(roles, headings)) != 0)
				g_error("table %s: heading %u has role %s, its column %s", kind, headings,
					role != NULL ? role : "(none)", (gchar *)g_ptr_array_index(roles, headings));
			headings++;
		}
		if (headings != roles->len)
			g_error("table %s has %u headings for %u columns", kind, headings, roles->len);

		/* Tables of one kind have the same columns. */
		if (g_hash_table_contains(kinds, kind)) {
			if (g_strcmp0(g_hash_table_lookup(kinds, kind), colgroup) != 0)
				g_error("two tables of %s with different columns", kind);
		} else {
			g_hash_table_insert(kinds, g_strdup(kind), g_strdup(colgroup));
		}

		tables++;
		p = tag_end;
	}

	return tables;
}

/* ── Event streams ──────────────────────────────────────────────── */

struct _AxisTestStream
{
	GSocket  *socket;
	GString  *text;		/* everything received */
	gboolean  closed;
	gboolean  paused;
};

AxisTestStream *
axis_test_stream_open(
	guint16      port,
	const gchar *path,
	const gchar *cookie,
	gint         receive_buffer
){
	AxisTestStream *self = g_new0(AxisTestStream, 1);
	GError *error = NULL;
	GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	GSocketAddress *address = g_inet_socket_address_new(loopback, port);
	g_autofree gchar *request = NULL;

	self->text = g_string_new("");
	self->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
		&error);
	g_assert_no_error(error);
	if (receive_buffer > 0) {
		/* SO_RCVBUF, before connecting, so that the window starts small. */
		g_assert_true(g_socket_set_option(self->socket, 1 /* SOL_SOCKET */, 8 /* SO_RCVBUF */,
			receive_buffer, &error));
		g_assert_no_error(error);
	}
	g_assert_true(g_socket_connect(self->socket, address, NULL, &error));
	g_assert_no_error(error);
	g_object_unref(address);
	g_object_unref(loopback);

	request = g_strdup_printf("GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nAccept: text/event-stream\r\n"
		"%s%s%sConnection: keep-alive\r\n\r\n",
		path, (guint)port,
		cookie != NULL ? "Cookie: " : "", cookie != NULL ? cookie : "", cookie != NULL ? "\r\n" : "");
	g_assert_cmpint(g_socket_send(self->socket, request, strlen(request), NULL, &error), ==,
		(gssize)strlen(request));
	g_assert_no_error(error);
	g_socket_set_blocking(self->socket, FALSE);

	return self;
}

/* Reads what has arrived. */
static void
drain(AxisTestStream *self)
{
	gchar buffer[16384];

	while (!self->closed && !self->paused) {
		GError *error = NULL;
		gssize n = g_socket_receive(self->socket, buffer, sizeof(buffer), NULL, &error);

		if (n > 0) {
			g_string_append_len(self->text, buffer, n);
			/* The last chunk: the daemon ended the response, which is
			 * all a client can tell of a stream closed gracefully (the
			 * connection stays open for the next request). */
			if (g_str_has_suffix(self->text->str, "\r\n0\r\n\r\n"))
				self->closed = TRUE;
			continue;
		}
		if (n == 0) {
			self->closed = TRUE;
		} else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			g_error_free(error);
			return;
		} else {
			/* A reset is the server closing too. */
			self->closed = TRUE;
			g_error_free(error);
		}
		return;
	}
}

typedef struct
{
	AxisTestStream *stream;
	const gchar    *needle;
} Wait;

static gboolean
needle_seen(gpointer data)
{
	Wait *wait = data;

	drain(wait->stream);
	return strstr(wait->stream->text->str, wait->needle) != NULL;
}

static gboolean
stream_closed(gpointer data)
{
	AxisTestStream *self = data;

	drain(self);
	return self->closed;
}

guint
axis_test_stream_get_status(AxisTestStream *self)
{
	Wait wait = { self, "\r\n\r\n" };
	guint status = 0;

	if (!axis_test_pump_until(needle_seen, &wait, AXIS_TEST_WAIT_MS))
		return 0;
	if (sscanf(self->text->str, "HTTP/1.1 %u", &status) != 1)
		return 0;

	return status;
}

gboolean
axis_test_stream_wait_for(
	AxisTestStream *self,
	const gchar    *needle,
	guint           timeout_ms
){
	Wait wait = { self, needle };

	return axis_test_pump_until(needle_seen, &wait, timeout_ms);
}

gboolean
axis_test_stream_wait_closed(
	AxisTestStream *self,
	guint           timeout_ms
){
	return axis_test_pump_until(stream_closed, self, timeout_ms);
}

gboolean
axis_test_stream_is_closed(AxisTestStream *self)
{
	drain(self);
	return self->closed;
}

guint
axis_test_stream_count(
	AxisTestStream *self,
	const gchar    *needle
){
	const gchar *p;
	guint n = 0;

	drain(self);
	p = self->text->str;
	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}

	return n;
}

const gchar *
axis_test_stream_get_text(AxisTestStream *self)
{
	drain(self);
	return self->text->str;
}

gchar *
axis_test_stream_get_header(
	AxisTestStream *self,
	const gchar    *name
){
	g_autofree gchar *needle = g_ascii_strdown(name, -1);
	g_autofree gchar *lower = g_ascii_strdown(self->text->str, -1);
	g_autofree gchar *wanted = g_strdup_printf("\r\n%s: ", needle);
	const gchar *head_end = strstr(lower, "\r\n\r\n");
	const gchar *found = strstr(lower, wanted);
	const gchar *start;
	const gchar *end;

	if (found == NULL || (head_end != NULL && found > head_end))
		return NULL;
	start = self->text->str + (found - lower) + strlen(wanted);
	end = strstr(start, "\r\n");

	return end != NULL ? g_strndup(start, (gsize)(end - start)) : NULL;
}

void
axis_test_stream_set_paused(
	AxisTestStream *self,
	gboolean        paused
){
	self->paused = paused;
}

void
axis_test_stream_free(AxisTestStream *self)
{
	if (self == NULL)
		return;

	g_socket_close(self->socket, NULL);
	g_object_unref(self->socket);
	g_string_free(self->text, TRUE);
	g_free(self);
}
