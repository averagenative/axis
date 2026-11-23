/*
 * axis-http-cookie.c - session cookies of a download client's connection
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-http-cookie.h"

#include <string.h>

typedef struct
{
	gchar *name;
	gchar *value;
} Cookie;

struct _AxisHttpCookies
{
	GPtrArray *cookies;	/* Cookie */
};

static void
wipe(gchar *text)
{
	if (text == NULL)
		return;
	memset(text, 0, strlen(text));
	g_free(text);
}

static void
cookie_free(gpointer data)
{
	Cookie *cookie = data;

	g_free(cookie->name);
	wipe(cookie->value);
	g_free(cookie);
}

AxisHttpCookies *
axis_http_cookies_new(void)
{
	AxisHttpCookies *self = g_new0(AxisHttpCookies, 1);

	self->cookies = g_ptr_array_new_with_free_func(cookie_free);
	return self;
}

void
axis_http_cookies_free(AxisHttpCookies *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->cookies);
	g_free(self);
}

/* RFC 7230 tchar */
static gboolean
is_token_char(guchar c)
{
	if (g_ascii_isalnum(c))
		return TRUE;
	return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* RFC 6265 cookie-octet */
static gboolean
is_value_char(guchar c)
{
	return c == 0x21 || (c >= 0x23 && c <= 0x2b) || (c >= 0x2d && c <= 0x3a) ||
		(c >= 0x3c && c <= 0x5b) || (c >= 0x5d && c <= 0x7e);
}

gboolean
axis_http_cookie_name_valid(const gchar *name)
{
	gsize n;
	gsize i;

	if (name == NULL)
		return FALSE;
	n = strlen(name);
	if (n == 0 || n > AXIS_HTTP_MAX_COOKIE_NAME)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (!is_token_char((guchar)name[i]))
			return FALSE;
	}
	return TRUE;
}

static gint
find(
	AxisHttpCookies *self,
	const gchar     *name
){
	guint i;

	for (i = 0; i < self->cookies->len; i++) {
		const Cookie *cookie = g_ptr_array_index(self->cookies, i);

		if (strcmp(cookie->name, name) == 0)
			return (gint)i;
	}
	return -1;
}

static gboolean
is_space(gchar c)
{
	return c == ' ' || c == '\t';
}

/* @text trimmed of spaces and tabs, as a new string of at most @limit + 1
 * bytes (a longer one is refused by the caller by its length). */
static gchar *
trimmed(
	const gchar *start,
	const gchar *end
){
	while (start < end && is_space(*start))
		start++;
	while (end > start && is_space(end[-1]))
		end--;
	return g_strndup(start, (gsize)(end - start));
}

/* Whether the attributes after the first ';' ask for removal: a Max-Age of
 * zero or less.  Other attributes, and a malformed Max-Age, are ignored. */
static gboolean
attributes_remove(const gchar *attributes)
{
	g_auto(GStrv) parts = NULL;
	guint i;

	if (attributes == NULL)
		return FALSE;
	parts = g_strsplit(attributes, ";", 32);
	for (i = 0; parts[i] != NULL; i++) {
		gchar *eq = strchr(parts[i], '=');
		g_autofree gchar *name = NULL;
		g_autofree gchar *value = NULL;
		const gchar *p;
		gboolean negative = FALSE;
		gboolean digits = FALSE;
		gboolean positive = FALSE;

		if (eq == NULL)
			continue;
		name = trimmed(parts[i], eq);
		if (g_ascii_strcasecmp(name, "Max-Age") != 0)
			continue;
		value = trimmed(eq + 1, eq + 1 + strlen(eq + 1));
		p = value;
		if (*p == '-') {
			negative = TRUE;
			p++;
		}
		for (; g_ascii_isdigit(*p); p++) {
			digits = TRUE;
			positive = positive || *p != '0';
		}
		if (*p != '\0' || !digits)
			continue;
		return negative || !positive;
	}
	return FALSE;
}

gboolean
axis_http_cookies_take(
	AxisHttpCookies *self,
	const gchar     *header
){
	const gchar *semicolon;
	const gchar *eq;
	const gchar *end;
	g_autofree gchar *name = NULL;
	gchar *value = NULL;
	gsize n;
	gsize i;
	gint index;

	g_return_val_if_fail(self != NULL, FALSE);

	if (header == NULL || strlen(header) > AXIS_HTTP_MAX_COOKIE_VALUE + AXIS_HTTP_MAX_COOKIE_NAME + 1024)
		return FALSE;
	semicolon = strchr(header, ';');
	end = semicolon != NULL ? semicolon : header + strlen(header);
	eq = memchr(header, '=', (gsize)(end - header));
	if (eq == NULL)
		return FALSE;
	name = trimmed(header, eq);
	if (!axis_http_cookie_name_valid(name))
		return FALSE;
	value = trimmed(eq + 1, end);
	n = strlen(value);
	/* A quoted value is the same value without its quotes. */
	if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
		memmove(value, value + 1, n - 2);
		value[n - 2] = '\0';
		n -= 2;
	}
	if (n > AXIS_HTTP_MAX_COOKIE_VALUE) {
		wipe(value);
		return FALSE;
	}
	for (i = 0; i < n; i++) {
		if (!is_value_char((guchar)value[i])) {
			wipe(value);
			return FALSE;
		}
	}
	index = find(self, name);
	if (n == 0 || attributes_remove(semicolon != NULL ? semicolon + 1 : NULL)) {
		wipe(value);
		if (index < 0)
			return FALSE;
		g_ptr_array_remove_index(self->cookies, (guint)index);
		return TRUE;
	}
	if (index >= 0) {
		Cookie *cookie = g_ptr_array_index(self->cookies, (guint)index);

		wipe(cookie->value);
		cookie->value = value;
		return TRUE;
	}
	if (self->cookies->len >= AXIS_HTTP_MAX_COOKIES) {
		wipe(value);
		return FALSE;
	}
	{
		Cookie *cookie = g_new0(Cookie, 1);

		cookie->name = g_steal_pointer(&name);
		cookie->value = value;
		g_ptr_array_add(self->cookies, cookie);
	}
	return TRUE;
}

gchar *
axis_http_cookies_header(AxisHttpCookies *self)
{
	GString *out;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	if (self->cookies->len == 0)
		return NULL;
	out = g_string_new(NULL);
	for (i = 0; i < self->cookies->len; i++) {
		const Cookie *cookie = g_ptr_array_index(self->cookies, i);

		g_string_append_printf(out, "%s%s=%s", i > 0 ? "; " : "", cookie->name, cookie->value);
	}
	return g_string_free(out, FALSE);
}

void
axis_http_cookies_header_free(gchar *header)
{
	wipe(header);
}

guint
axis_http_cookies_count(AxisHttpCookies *self)
{
	return self != NULL ? self->cookies->len : 0;
}

void
axis_http_cookies_clear(AxisHttpCookies *self)
{
	if (self != NULL)
		g_ptr_array_set_size(self->cookies, 0);
}

gchar *
axis_http_cookies_scrub(
	AxisHttpCookies *self,
	const gchar     *text
){
	gchar *out = g_strdup(text != NULL ? text : "");
	guint i;

	if (self == NULL)
		return out;
	for (i = 0; i < self->cookies->len; i++) {
		const Cookie *cookie = g_ptr_array_index(self->cookies, i);
		gchar **parts;

		if (strlen(cookie->value) < AXIS_HTTP_COOKIE_SCRUB_MIN)
			continue;
		parts = g_strsplit(out, cookie->value, -1);
		g_free(out);
		out = g_strjoinv("***", parts);
		g_strfreev(parts);
	}
	return out;
}
