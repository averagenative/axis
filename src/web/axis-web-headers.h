/*
 * axis-web-headers.h - the security headers every answer of a web interface
 * of Axis carries
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One list, for the engine's interface (web/axis-web.c) and for the request
 * portal (clients/portal/): the portal's pages are to send exactly what the
 * engine's do, so both take them from here.  Split from axis-web.c
 * (add-request-portal, slice P3); htmx-glib and libsoup only.
 */

#ifndef AXIS_WEB_HEADERS_H
#define AXIS_WEB_HEADERS_H

#include <htmx-glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/*
 * No inline script or style is ever allowed (the pages need neither), and
 * nothing may be loaded from anywhere but the program itself.
 */
#define AXIS_WEB_CSP "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"

/* The headers' names, %NULL-ended: for the tests that compare two
 * interfaces' answers. */
const gchar *const *
axis_web_security_header_names(void);

/* Adds the security headers to @response (not a cache policy). */
void
axis_web_add_security_headers(HtmxResponse *response);

/* Puts the security headers and "no-store" on a message's response headers
 * (an answer written without the shell: a streaming one, or a refusal of
 * axis-web-limits.c). */
void
axis_web_replace_security_headers(SoupMessageHeaders *headers);

G_END_DECLS

#endif /* AXIS_WEB_HEADERS_H */
