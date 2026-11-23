/*
 * fuzz-http-cookie.c - libFuzzer target for the session cookies a download
 * client's connection keeps (core/axis-http-cookie.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a series of Set-Cookie header values, one per line.  Whatever
 * they hold: the jar never holds more than its limit, the Cookie header it
 * makes is name=value pairs of the cookie syntax only (no control
 * character, no space inside a value, no line break), and blanking the held
 * values out of the header leaves none of them in it.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/axis-http-cookie.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *text = g_strndup((const gchar *)data, size);
	gchar **lines = g_strsplit(text, "\n", 64);
	AxisHttpCookies *jar = axis_http_cookies_new();
	gchar *header;
	guint i;

	for (i = 0; lines[i] != NULL; i++)
		axis_http_cookies_take(jar, lines[i]);
	if (axis_http_cookies_count(jar) > AXIS_HTTP_MAX_COOKIES)
		abort();
	header = axis_http_cookies_header(jar);
	if ((header == NULL) != (axis_http_cookies_count(jar) == 0))
		abort();
	if (header != NULL) {
		gchar **pairs = g_strsplit(header, "; ", -1);
		const gchar *p;

		if (g_strv_length(pairs) != axis_http_cookies_count(jar))
			abort();
		for (i = 0; pairs[i] != NULL; i++) {
			const gchar *eq = strchr(pairs[i], '=');
			gchar *name;

			if (eq == NULL || eq == pairs[i])
				abort();
			name = g_strndup(pairs[i], (gsize)(eq - pairs[i]));
			if (!axis_http_cookie_name_valid(name))
				abort();
			g_free(name);
			for (p = eq + 1; *p != '\0'; p++) {
				if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == ';' || *p == ',' || *p == '"' || *p == '\\')
					abort();
			}
		}
		g_strfreev(pairs);
	}
	axis_http_cookies_header_free(header);
	axis_http_cookies_clear(jar);
	if (axis_http_cookies_count(jar) != 0 || axis_http_cookies_header(jar) != NULL)
		abort();
	axis_http_cookies_free(jar);
	g_strfreev(lines);
	g_free(text);
	return 0;
}
