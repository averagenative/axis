/*
 * axis-test-plex.h - test support: a fake Plex Media Server on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs in a thread of its own with its own main context, so the code under
 * test may block on it from the test's thread (the daemon's HTTP client
 * does).  It answers from a table of routes the test fills (usually from
 * the files of tests/fixtures/compat), can misbehave on purpose (redirect,
 * answer too much, a byte at a time, forever, or never), and records every
 * request it receives, headers included, so that a test can assert which
 * methods and which headers arrived, and that nothing arrived at a server
 * that should never have been contacted.
 *
 * The fixture files may hold "@AGE:<seconds>@" inside a JSON string; the
 * server replaces it with the Unix time that many seconds before it was
 * started, the same in every answer.
 */

#ifndef AXIS_TEST_PLEX_H
#define AXIS_TEST_PLEX_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisTestPlex AxisTestPlex;

typedef enum
{
	AXIS_TEST_PLEX_NORMAL,		/* the body, with the status */
	AXIS_TEST_PLEX_REDIRECT,	/* status 302 to the location in @text */
	AXIS_TEST_PLEX_OVERSIZE,	/* @number bytes of JSON-ish filler, length known */
	AXIS_TEST_PLEX_ENDLESS,		/* chunked, never ends */
	AXIS_TEST_PLEX_DRIP,		/* chunked, one small chunk every @number ms, for ever */
	AXIS_TEST_PLEX_HANG,		/* the head never comes */
	AXIS_TEST_PLEX_STATUS,		/* an empty body with status @number */
	AXIS_TEST_PLEX_BYTES		/* axis_test_plex_route_full() */
} AxisTestPlexMode;

/* A server on 127.0.0.1 and a free port. */
AxisTestPlex *
axis_test_plex_new(void);

/* The same over TLS with the certificate and key (PEM files), or %NULL when
 * the TLS backend is missing. */
AxisTestPlex *
axis_test_plex_new_tls(
	const gchar *certificate_file,
	const gchar *key_file
);

void
axis_test_plex_free(AxisTestPlex *self);

guint16
axis_test_plex_port(AxisTestPlex *self);

/* "http://127.0.0.1:PORT" ("https" for the TLS server) */
gchar *
axis_test_plex_base(AxisTestPlex *self);

/**
 * axis_test_plex_route:
 * @path: the request path, without the query
 * @start: the value of the X-Plex-Container-Start header this route is for,
 *   or -1 for any
 * @mode: how it answers
 * @status: the status of a normal answer
 * @text: the body (a normal answer) or the location (a redirect), copied;
 *   may be %NULL
 * @number: see AxisTestPlexMode
 *
 * Adds a route; the first that matches a request answers it.
 */
void
axis_test_plex_route(
	AxisTestPlex     *self,
	const gchar      *path,
	gint              start,
	AxisTestPlexMode  mode,
	guint             status,
	const gchar      *text,
	gint64            number
);

/* A normal 200 answer from a fixture file (path relative to the project
 * root), read now. */
void
axis_test_plex_route_file(
	AxisTestPlex *self,
	const gchar  *path,
	gint          start,
	const gchar  *fixture
);

/**
 * axis_test_plex_route_full:
 * @path: the request path, without the query
 * @status: the status
 * @content_type: (nullable): the Content-Type, or none
 * @body: (nullable): the body as it is (any bytes, no substitution); copied
 * @headers: (nullable): further response headers, name and value pairs
 *   ended by %NULL; copied
 *
 * A route for any X-Plex-Container-Start, answering exactly this: for a
 * server that is not Plex (a metadata provider, its image host).
 */
void
axis_test_plex_route_full(
	AxisTestPlex       *self,
	const gchar        *path,
	guint               status,
	const gchar        *content_type,
	GBytes             *body,
	const gchar *const *headers
);

/* Forgets the routes. */
void
axis_test_plex_clear_routes(AxisTestPlex *self);

/**
 * AxisTestPlexRequest:
 * @method: "GET", "POST", ...
 * @path: without the query
 * @query: (nullable): the query, without the '?'
 * @headers: name, value, ..., %NULL
 * @body: the request body (empty for none)
 *
 * A request as a handler (axis_test_plex_set_handler()) sees it.
 */
typedef struct
{
	const gchar        *method;
	const gchar        *path;
	const gchar        *query;
	const gchar *const *headers;
	GBytes             *body;
} AxisTestPlexRequest;

/**
 * AxisTestPlexAnswer:
 * @status: the status (the handler must set it)
 * @content_type: (nullable): the Content-Type, owned (g_free)
 * @body: (nullable): the body, owned
 * @headers: (nullable): further response headers, name and value pairs
 *   ended by %NULL, owned (g_strfreev)
 * @hang: never answer (the head never comes)
 */
typedef struct
{
	guint    status;
	gchar   *content_type;
	GBytes  *body;
	gchar  **headers;
	gboolean hang;
} AxisTestPlexAnswer;

/* Called on the server's own thread for every request no route matches. */
typedef void (*AxisTestPlexHandler)(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
);

/* @handler answers what no route does (%NULL: a 404, as before); @user_data
 * must outlive the server or the next call. */
void
axis_test_plex_set_handler(
	AxisTestPlex        *self,
	AxisTestPlexHandler  handler,
	gpointer             user_data
);

/* The value of header @name of @request, or %NULL. */
const gchar *
axis_test_plex_request_header(
	const AxisTestPlexRequest *request,
	const gchar               *name
);

/* The standard answers of tests/fixtures/compat for identity, libraries,
 * devices, accounts, two pages of history and no sessions. */
void
axis_test_plex_standard(AxisTestPlex *self);

/* ── What it received ───────────────────────────────────────────── */

guint
axis_test_plex_n_requests(AxisTestPlex *self);

/* How many requests were for @path (without the query). */
guint
axis_test_plex_count(
	AxisTestPlex *self,
	const gchar  *path
);

/* Whether any request used a method other than GET. */
gboolean
axis_test_plex_saw_non_get(AxisTestPlex *self);

/* Everything received as text: method, path with query, every header and
 * the body (when there is one) of every request, for searching. */
gchar *
axis_test_plex_transcript(AxisTestPlex *self);

/* The value of header @name of the latest request for @path, or %NULL. */
gchar *
axis_test_plex_last_header(
	AxisTestPlex *self,
	const gchar  *path,
	const gchar  *name
);

void
axis_test_plex_reset_requests(AxisTestPlex *self);

/* When each request arrived (g_get_monotonic_time()), in order: a GArray of
 * gint64 the caller frees. */
GArray *
axis_test_plex_request_times(AxisTestPlex *self);

/* Each request as text (as in the transcript), in order: a GPtrArray of
 * strings the caller frees. */
GPtrArray *
axis_test_plex_requests(AxisTestPlex *self);

G_END_DECLS

#endif /* AXIS_TEST_PLEX_H */
