/*
 * axis-http-cookie.h - the session cookies one download client's connection
 * holds, in memory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Some download clients (Deluge) answer a login with a session cookie that
 * every later request must carry.  The outbound client (core/axis-http.h)
 * keeps such cookies for a connection made for a download client, and only
 * there: they come from that connection's one origin (its requests go
 * nowhere else, and a redirect elsewhere is refused before anything is
 * sent), so they are sent back to that origin only.  They are never
 * written anywhere: not to the database, not to a log, not into a message
 * (every message of the connection has their values blanked out), and they
 * are wiped when the connection is freed.
 *
 * Only the name and value of a Set-Cookie header are kept.  Its attributes
 * (Domain, Path, Secure, Expires) are ignored, because the cookie is bound
 * to the connection's origin already, except that a Max-Age of zero or
 * less, or an empty value, removes it.  A header whose name or value has a
 * character the cookie syntax forbids is ignored, as is a new cookie once
 * %AXIS_HTTP_MAX_COOKIES are held.
 *
 * GLib only, no I/O: fuzzed through tests/fuzz/fuzz-http-cookie.c.
 */

#ifndef AXIS_HTTP_COOKIE_H
#define AXIS_HTTP_COOKIE_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_HTTP_MAX_COOKIES           16
#define AXIS_HTTP_MAX_COOKIE_NAME       64
#define AXIS_HTTP_MAX_COOKIE_VALUE      4096

/* A value shorter than this is not blanked out of messages (it would blank
 * every occurrence of a common short string); such a value is no secret. */
#define AXIS_HTTP_COOKIE_SCRUB_MIN      6

typedef struct _AxisHttpCookies AxisHttpCookies;

AxisHttpCookies *
axis_http_cookies_new(void);

/* Wipes every value, then frees. */
void
axis_http_cookies_free(AxisHttpCookies *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisHttpCookies, axis_http_cookies_free)

/**
 * axis_http_cookies_take:
 * @header: (nullable): the value of one Set-Cookie header
 *
 * Returns: %TRUE when the header set or removed a cookie; %FALSE when it was
 *   ignored
 */
gboolean
axis_http_cookies_take(
	AxisHttpCookies *self,
	const gchar     *header
);

/* Returns: (transfer full) (nullable): "name=value; name=value" for a Cookie
 *   header, or %NULL when none is held; free it with
 *   axis_http_cookies_header_free(), which wipes it first. */
gchar *
axis_http_cookies_header(AxisHttpCookies *self);

/* Wipes and frees what axis_http_cookies_header() returned. */
void
axis_http_cookies_header_free(gchar *header);

guint
axis_http_cookies_count(AxisHttpCookies *self);

/* Wipes and forgets every cookie. */
void
axis_http_cookies_clear(AxisHttpCookies *self);

/**
 * axis_http_cookies_scrub:
 * @text: what is to be shown or logged
 *
 * Returns: (transfer full): @text with every held value of at least
 *   %AXIS_HTTP_COOKIE_SCRUB_MIN bytes replaced by "***"
 */
gchar *
axis_http_cookies_scrub(
	AxisHttpCookies *self,
	const gchar     *text
);

/* Whether @name is a cookie name (an HTTP token of 1 to
 * %AXIS_HTTP_MAX_COOKIE_NAME characters). */
gboolean
axis_http_cookie_name_valid(const gchar *name);

G_END_DECLS

#endif /* AXIS_HTTP_COOKIE_H */
