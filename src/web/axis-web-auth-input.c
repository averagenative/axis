/*
 * axis-web-auth-input.c - checks on what a client sends to the auth code
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-auth-input.h"

#include <string.h>

static gboolean
is_space_or_tab(gchar c)
{
	return c == ' ' || c == '\t';
}

gchar *
axis_cookie_find(
	const gchar *header,
	const gchar *name
){
	const gchar *p;
	gsize name_len;
	gchar *found = NULL;

	g_return_val_if_fail(name != NULL, NULL);

	if (header == NULL || strlen(header) > AXIS_COOKIE_HEADER_MAX)
		return NULL;

	name_len = strlen(name);
	p = header;
	while (*p != '\0') {
		const gchar *end = strchr(p, ';');
		const gchar *part_end = end != NULL ? end : p + strlen(p);
		const gchar *key = p;
		const gchar *key_end;
		const gchar *eq;

		while (key < part_end && is_space_or_tab(*key))
			key++;
		eq = memchr(key, '=', (gsize)(part_end - key));
		if (eq != NULL) {
			key_end = eq;
			while (key_end > key && is_space_or_tab(key_end[-1]))
				key_end--;

			if ((gsize)(key_end - key) == name_len && memcmp(key, name, name_len) == 0) {
				const gchar *value = eq + 1;
				const gchar *value_end = part_end;

				if (found != NULL) {
					/* The same name twice: trust neither. */
					g_free(found);
					return NULL;
				}
				while (value < value_end && is_space_or_tab(*value))
					value++;
				while (value_end > value && is_space_or_tab(value_end[-1]))
					value_end--;
				found = g_strndup(value, (gsize)(value_end - value));
			}
		}

		if (end == NULL)
			break;
		p = end + 1;
	}

	return found;
}

static gboolean
is_hex(gchar c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

gboolean
axis_auth_next_is_safe(const gchar *next)
{
	gsize len;
	gsize i;

	if (next == NULL)
		return FALSE;

	len = strlen(next);
	if (len == 0 || len > AXIS_NEXT_MAX_LEN)
		return FALSE;
	if (next[0] != '/' || next[1] == '/' || next[1] == '\\')
		return FALSE;

	for (i = 0; i < len; i++) {
		guchar c = (guchar)next[i];

		if (c < 0x21 || c > 0x7e)
			return FALSE;
		if (strchr("\\<>\"`{}|^", c) != NULL)
			return FALSE;
		if (c == '%') {
			if (i + 2 >= len || !is_hex(next[i + 1]) || !is_hex(next[i + 2]))
				return FALSE;
		}
	}

	return TRUE;
}

gchar *
axis_auth_normalise_username(const gchar *input)
{
	const gchar *start;
	const gchar *end;
	gchar *name;
	gsize len;
	gsize i;

	if (input == NULL)
		return NULL;

	start = input;
	while (*start == ' ')
		start++;
	end = start + strlen(start);
	while (end > start && end[-1] == ' ')
		end--;

	len = (gsize)(end - start);
	if (len == 0 || len > AXIS_USERNAME_MAX_LEN)
		return NULL;

	name = g_strndup(start, len);
	for (i = 0; i < len; i++) {
		gchar c = g_ascii_tolower(name[i]);

		name[i] = c;
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			continue;
		if (i > 0 && (c == '.' || c == '_' || c == '-'))
			continue;

		g_free(name);
		return NULL;
	}

	return name;
}

gboolean
axis_auth_token_is_wellformed(
	const gchar *token,
	gsize        length
){
	gsize i;

	if (token == NULL)
		return FALSE;

	for (i = 0; i < length; i++) {
		gchar c = token[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return FALSE;	/* also stops at a NUL before @length */
	}

	return token[length] == '\0';
}

const gchar *
axis_auth_check_password(const gchar *password)
{
	if (password == NULL || !g_utf8_validate(password, -1, NULL))
		return "The password must be valid text.";
	if (strlen(password) > AXIS_PASSWORD_MAX_BYTES)
		return "The password must be at most 1024 bytes long.";
	if (g_utf8_strlen(password, -1) < AXIS_PASSWORD_MIN_CHARS)
		return "The password must be at least 10 characters long.";

	return NULL;
}

/*
 * @authority without a default port for @scheme_https, lower-cased; %NULL
 * if it holds anything an authority cannot.
 */
static gchar *
normalise_authority(
	const gchar *authority,
	gsize        len,
	gboolean     scheme_https
){
	const gchar *default_port = scheme_https ? ":443" : ":80";
	gsize port_len = strlen(default_port);
	gsize i;

	if (len == 0)
		return NULL;
	for (i = 0; i < len; i++) {
		guchar c = (guchar)authority[i];

		if (c < 0x21 || c > 0x7e || c == '/' || c == '\\' || c == '?' || c == '#' || c == '@')
			return NULL;
	}

	if (len > port_len && g_ascii_strncasecmp(authority + len - port_len, default_port, port_len) == 0)
		len -= port_len;

	return g_ascii_strdown(authority, (gssize)len);
}

gboolean
axis_auth_origin_matches(
	const gchar *origin,
	const gchar *host,
	gboolean     https
){
	g_autofree gchar *origin_authority = NULL;
	g_autofree gchar *host_authority = NULL;
	gboolean origin_https;
	const gchar *rest;

	if (origin == NULL || host == NULL)
		return FALSE;

	if (g_ascii_strncasecmp(origin, "https://", 8) == 0) {
		origin_https = TRUE;
		rest = origin + 8;
	} else if (g_ascii_strncasecmp(origin, "http://", 7) == 0) {
		origin_https = FALSE;
		rest = origin + 7;
	} else {
		return FALSE;
	}

	if (https && !origin_https)
		return FALSE;

	origin_authority = normalise_authority(rest, strlen(rest), origin_https);
	if (origin_authority == NULL)
		return FALSE;

	/* The Host header carries no scheme: drop the default port of the
	 * origin's scheme from it too. */
	host_authority = normalise_authority(host, strlen(host), origin_https);
	if (host_authority == NULL)
		return FALSE;

	return strcmp(origin_authority, host_authority) == 0;
}

gchar *
axis_auth_clean_user_agent(const gchar *user_agent)
{
	gchar *clean;
	gsize len;
	gsize i;

	if (user_agent == NULL)
		return g_strdup("");

	len = strlen(user_agent);
	if (len > AXIS_USER_AGENT_MAX_LEN)
		len = AXIS_USER_AGENT_MAX_LEN;

	clean = g_strndup(user_agent, len);
	for (i = 0; i < len; i++) {
		guchar c = (guchar)clean[i];

		if (c < 0x20 || c > 0x7e)
			clean[i] = '?';
	}

	return clean;
}
