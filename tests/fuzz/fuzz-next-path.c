/*
 * fuzz-next-path.c - libFuzzer target for the "next" redirect check (and
 * the username and origin checks, which are small and take the same kind
 * of input)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Besides memory safety it checks what matters about every path
 * axis_auth_next_is_safe() accepts: resolved as a browser would, against
 * this site's URL, it never leaves the site (same scheme, same host, no
 * user information), and it holds nothing a browser strips or rewrites.
 * For usernames, an accepted name is in the stored form and normalises to
 * itself.  The origin check never accepts an origin naming another host.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web/axis-web-auth-input.h"

#define SITE "https://axis.example:8443/movies/list?x=1"

static void
check_next(const gchar *next)
{
	GUri *base;
	GUri *resolved;
	gsize len;
	gsize i;

	if (!axis_auth_next_is_safe(next))
		return;

	len = strlen(next);
	if (len == 0 || len > AXIS_NEXT_MAX_LEN || next[0] != '/' || next[1] == '/' || next[1] == '\\')
		abort();
	for (i = 0; i < len; i++) {
		guchar c = (guchar)next[i];

		if (c <= 0x20 || c >= 0x7f || c == '\\')
			abort();
	}

	base = g_uri_parse(SITE, G_URI_FLAGS_NONE, NULL);
	if (base == NULL)
		abort();
	resolved = g_uri_parse_relative(base, next, G_URI_FLAGS_NONE, NULL);
	if (resolved == NULL) {
		/* GLib refusing it (say, a bad escape it reads differently) is
		 * not a way off the site. */
		g_uri_unref(base);
		return;
	}
	if (g_strcmp0(g_uri_get_scheme(resolved), "https") != 0 ||
	    g_strcmp0(g_uri_get_host(resolved), "axis.example") != 0 ||
	    g_uri_get_port(resolved) != 8443 ||
	    g_uri_get_userinfo(resolved) != NULL)
		abort();

	g_uri_unref(resolved);
	g_uri_unref(base);
}

static void
check_username(const gchar *input)
{
	gchar *name = axis_auth_normalise_username(input);
	gchar *again;
	gsize i;

	if (name == NULL)
		return;

	if (name[0] == '\0' || strlen(name) > AXIS_USERNAME_MAX_LEN)
		abort();
	if (!g_ascii_islower(name[0]) && !g_ascii_isdigit(name[0]))
		abort();
	for (i = 0; name[i] != '\0'; i++) {
		gchar c = name[i];

		if (!g_ascii_islower(c) && !g_ascii_isdigit(c) && c != '.' && c != '_' && c != '-')
			abort();
	}
	again = axis_auth_normalise_username(name);
	if (again == NULL || strcmp(again, name) != 0)
		abort();

	g_free(again);
	g_free(name);
}

static void
check_origin(const gchar *origin)
{
	/* Whatever the header says, it only matches when it names this
	 * host. */
	if (axis_auth_origin_matches(origin, "axis.example", FALSE) &&
	    g_ascii_strcasecmp(origin, "http://axis.example") != 0 &&
	    g_ascii_strcasecmp(origin, "https://axis.example") != 0 &&
	    g_ascii_strcasecmp(origin, "http://axis.example:80") != 0 &&
	    g_ascii_strcasecmp(origin, "https://axis.example:443") != 0)
		abort();
	if (axis_auth_origin_matches(origin, "axis.example", TRUE) &&
	    g_ascii_strncasecmp(origin, "https://", 8) != 0)
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);

	check_next(text);
	check_username(text);
	check_origin(text);

	return 0;
}
