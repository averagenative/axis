/*
 * axis-test-web.h - test support for the web pages: requests to a server
 * running on the test's own main loop, and a client for event streams
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The server under test runs on the default main context, so a client
 * that blocked would deadlock it: every request here pumps the context
 * while it waits, and every wait is bounded.
 */

#ifndef AXIS_TEST_WEB_H
#define AXIS_TEST_WEB_H

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/* The port an HtmxServer listens on (it was told port 0). */
guint16
axis_test_web_port(HtmxServer *server);

/* ── Requests ─────────────────────────────────────────────────── */

typedef struct
{
	guint        status;
	SoupMessage *message;
	gchar       *body;		/* every byte, then a zero byte */
	gsize        length;	/* the body's length in bytes, not counting that zero */
} AxisTestReply;

/**
 * axis_test_web_request:
 * @extra: (nullable): header name and value pairs, ended by %NULL
 * @form: (nullable): an urlencoded body
 *
 * Sends the request as given (redirects are not followed) and waits for
 * the whole answer.
 */
AxisTestReply
axis_test_web_request(
	SoupSession        *session,
	guint16             port,
	const gchar        *method,
	const gchar        *path,
	const gchar        *cookie,
	const gchar *const *extra,
	const gchar        *form
);

void
axis_test_reply_clear(AxisTestReply *reply);

/* Returns: (nullable): the first response header called @name */
const gchar *
axis_test_reply_header(
	AxisTestReply *reply,
	const gchar   *name
);

/* The CSRF token a page carries for its htmx requests, or %NULL. */
gchar *
axis_test_web_csrf_token(const gchar *html);

/*
 * @html as a person reads and copies it: without the break opportunities
 * (<wbr />) that release names, file names and paths carry, so that a name is
 * found in a page as the name.
 */
gchar *
axis_test_web_unbroken(const gchar *html);

/**
 * axis_test_web_assert_tables:
 * @html: a page
 *
 * Fails the test unless every table of @html is a data table that declares
 * its kind, with a <colgroup> of one role per column and a heading row of
 * as many headings, each carrying its column's role; and unless every
 * table of one kind has the same <colgroup>.
 *
 * Returns: how many tables there were
 */
guint
axis_test_web_assert_tables(const gchar *html);

/**
 * axis_test_web_read_attention:
 * @web: the shell (an #AxisWeb, as a pointer: this header names no type of
 *   the daemon's)
 *
 * What needs attention, as it is after everything the test did so far: the
 * board is started if it was not (without a database for the dismissals,
 * read again only when asked), and two readings are waited for, one after
 * the other, so that the second began after this call.  The dashboard's card
 * "Needs attention", the bell and the page /attention are drawn from the
 * board, and say nothing until it has been read.  Runs the main loop.
 */
void
axis_test_web_read_attention(gpointer web);

/* As axis_test_web_read_attention(), with the readings' clock @minutes ahead
 * of the real one (the waits are judged by that clock). */
void
axis_test_web_read_attention_at(
	gpointer web,
	gint     minutes
);

/* ── Event streams ────────────────────────────────────────────── */

typedef struct _AxisTestStream AxisTestStream;

/**
 * axis_test_stream_open:
 * @path: the path and query, such as "/events?topic=jobs"
 * @cookie: (nullable): "name=value"
 * @receive_buffer: 0 for the system's, else a small receive buffer in
 *   bytes, so that a client that stops reading fills the path quickly
 *
 * Connects with a plain socket and sends the request.  The answer is read
 * whenever one of the functions below pumps the main context.
 */
AxisTestStream *
axis_test_stream_open(
	guint16      port,
	const gchar *path,
	const gchar *cookie,
	gint         receive_buffer
);

/* The status of the answer (0 if its head has not arrived in time). */
guint
axis_test_stream_get_status(AxisTestStream *self);

/* Waits for @needle to appear in what was received so far. */
gboolean
axis_test_stream_wait_for(
	AxisTestStream *self,
	const gchar    *needle,
	guint           timeout_ms
);

/* Waits until the server has closed the stream. */
gboolean
axis_test_stream_wait_closed(
	AxisTestStream *self,
	guint           timeout_ms
);

/* Whether the stream has been closed by the server (as far as read). */
gboolean
axis_test_stream_is_closed(AxisTestStream *self);

/* How many times @needle occurs in what was received. */
guint
axis_test_stream_count(
	AxisTestStream *self,
	const gchar    *needle
);

/* Everything received, head and body, as text. */
const gchar *
axis_test_stream_get_text(AxisTestStream *self);

/* The response header called @name (nullable), copied. */
gchar *
axis_test_stream_get_header(
	AxisTestStream *self,
	const gchar    *name
);

/* While paused the client reads nothing: the server's data piles up. */
void
axis_test_stream_set_paused(
	AxisTestStream *self,
	gboolean        paused
);

void
axis_test_stream_free(AxisTestStream *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTestStream, axis_test_stream_free)

G_END_DECLS

#endif /* AXIS_TEST_WEB_H */
