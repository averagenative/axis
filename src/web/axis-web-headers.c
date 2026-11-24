/*
 * axis-web-headers.c - the security headers every answer of a web interface
 * of Axis carries
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-headers.h"

static const gchar *const names[] = {
	"Content-Security-Policy", "X-Content-Type-Options", "X-Frame-Options", "Referrer-Policy", NULL
};

static const gchar *const values[] = {
	AXIS_WEB_CSP, "nosniff", "DENY", "same-origin", NULL
};

const gchar *const *
axis_web_security_header_names(void)
{
	return names;
}

void
axis_web_add_security_headers(HtmxResponse *response)
{
	guint i;

	g_return_if_fail(HTMX_IS_RESPONSE(response));

	for (i = 0; names[i] != NULL; i++)
		htmx_response_add_header(response, names[i], values[i]);
}

void
axis_web_replace_security_headers(SoupMessageHeaders *headers)
{
	guint i;

	g_return_if_fail(headers != NULL);

	for (i = 0; names[i] != NULL; i++)
		soup_message_headers_replace(headers, names[i], values[i]);
	soup_message_headers_replace(headers, "Cache-Control", "no-store");
}
