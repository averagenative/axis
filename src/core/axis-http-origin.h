/*
 * axis-http-origin.h - the one place an outbound connection may go: an
 * http or https origin (scheme, host and port)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An origin is an http or https URL with a host, an optional port, no
 * credentials, no query and no fragment, and no path beyond "/" (the caller
 * adds its own paths).  Anything else is refused when it is parsed.  The
 * outbound client (core/axis-http.h) sends requests to one origin and
 * follows a redirect only within it.
 *
 * Media-server addresses (compat/axis-compat-path.h) and metadata providers'
 * API and image addresses (meta/) are origins.
 *
 * GLib only, no I/O: fuzzed through tests/fuzz/fuzz-compat-path.c.
 */

#ifndef AXIS_HTTP_ORIGIN_H
#define AXIS_HTTP_ORIGIN_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_HTTP_ORIGIN_MAX_BYTES 255

/* The longest path and query a request may have, in bytes. */
#define AXIS_HTTP_MAX_PATH_BYTES 4096

typedef struct
{
	gboolean  https;
	gchar    *host;		/* lower case; an IPv6 address without brackets */
	guint     port;		/* explicit or the scheme's default */
	gchar    *base;		/* "http://host:port", for building URLs */
} AxisHttpOrigin;

void
axis_http_origin_clear(AxisHttpOrigin *origin);

/**
 * axis_http_origin_parse:
 * @text: the address
 * @origin: (out): the parts
 * @problem: (out) (optional) (transfer full): why not, a sentence
 *   fragment without any of the text in it
 *
 * Returns: %TRUE if @text is an acceptable origin
 */
gboolean
axis_http_origin_parse(
	const gchar    *text,
	AxisHttpOrigin *origin,
	gchar         **problem
);

/* Whether @a and @b are the same origin (scheme, host and port). */
gboolean
axis_http_origin_equal(
	const AxisHttpOrigin *a,
	const AxisHttpOrigin *b
);

G_END_DECLS

#endif /* AXIS_HTTP_ORIGIN_H */
