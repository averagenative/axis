/*
 * axis-http-origin.c - the one place an outbound connection may go
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-http-origin.h"

#include <string.h>

void
axis_http_origin_clear(AxisHttpOrigin *origin)
{
	if (origin == NULL)
		return;
	g_clear_pointer(&origin->host, g_free);
	g_clear_pointer(&origin->base, g_free);
	origin->port = 0;
	origin->https = FALSE;
}

static void
set_problem(
	gchar      **problem,
	const gchar *text
){
	if (problem != NULL)
		*problem = g_strdup(text);
}

static gboolean
host_is_plain(const gchar *host)
{
	const gchar *p;

	if (host[0] == '\0' || strlen(host) > 253)
		return FALSE;
	for (p = host; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-' && *p != ':' && *p != '_')
			return FALSE;
	}
	/* A colon belongs to an IPv6 address and to nothing else. */
	if (strchr(host, ':') != NULL && !g_hostname_is_ip_address(host))
		return FALSE;
	return TRUE;
}

gboolean
axis_http_origin_parse(
	const gchar    *text,
	AxisHttpOrigin *origin,
	gchar         **problem
){
	GUri *uri;
	const gchar *scheme;
	const gchar *host;
	const gchar *path;
	gint port;

	memset(origin, 0, sizeof(*origin));
	if (text == NULL || text[0] == '\0' || strlen(text) > AXIS_HTTP_ORIGIN_MAX_BYTES) {
		set_problem(problem, "must be an address of at most 255 bytes");
		return FALSE;
	}
	/* No percent-encoding: an address is a host and a port, and an encoded
	 * character in a host would be decoded into something that is not one
	 * (found by the fuzzer: "http://----%3A/"). */
	if (!g_utf8_validate(text, -1, NULL) || strpbrk(text, " \t\r\n\\%") != NULL) {
		set_problem(problem, "must be an http or https address without spaces");
		return FALSE;
	}
	uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL) {
		set_problem(problem, "must be an http or https address such as http://192.168.1.10:32400");
		return FALSE;
	}
	scheme = g_uri_get_scheme(uri);
	host = g_uri_get_host(uri);
	path = g_uri_get_path(uri);
	port = g_uri_get_port(uri);
	if (scheme == NULL || (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0)) {
		set_problem(problem, "must start with http:// or https://");
		g_uri_unref(uri);
		return FALSE;
	}
	if (host == NULL || !host_is_plain(host)) {
		set_problem(problem, "must name a host");
		g_uri_unref(uri);
		return FALSE;
	}
	if (g_uri_get_user(uri) != NULL || g_uri_get_password(uri) != NULL || g_uri_get_auth_params(uri) != NULL) {
		set_problem(problem, "must not contain a user name or password");
		g_uri_unref(uri);
		return FALSE;
	}
	if (g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
	    (path != NULL && path[0] != '\0' && strcmp(path, "/") != 0)) {
		set_problem(problem, "must be only a host and a port, with no path, query or fragment");
		g_uri_unref(uri);
		return FALSE;
	}
	if (port == 0 || port > 65535) {
		set_problem(problem, "must have a port from 1 to 65535");
		g_uri_unref(uri);
		return FALSE;
	}
	origin->https = strcmp(scheme, "https") == 0;
	origin->host = g_ascii_strdown(host, -1);
	origin->port = port > 0 ? (guint)port : (origin->https ? 443 : 80);
	if (strchr(origin->host, ':') != NULL)
		origin->base = g_strdup_printf("%s://[%s]:%u", scheme, origin->host, origin->port);
	else
		origin->base = g_strdup_printf("%s://%s:%u", scheme, origin->host, origin->port);
	g_uri_unref(uri);
	return TRUE;
}

gboolean
axis_http_origin_equal(
	const AxisHttpOrigin *a,
	const AxisHttpOrigin *b
){
	if (a == NULL || b == NULL || a->host == NULL || b->host == NULL)
		return FALSE;
	return a->https == b->https && a->port == b->port && strcmp(a->host, b->host) == 0;
}
