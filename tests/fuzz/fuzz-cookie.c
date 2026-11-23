/*
 * fuzz-cookie.c - libFuzzer target for the Cookie header parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a cookie name, a newline, and a Cookie header.  Besides
 * memory safety it checks what axis_cookie_find() promises about any
 * value it returns: it is part of the header, holds no ';', has no space
 * or tab at either end, and the header (split at ';', names trimmed) has
 * exactly one cookie of that name, whose value it is.  A header longer
 * than the limit finds nothing.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web/axis-web-auth-input.h"

/* @text without the spaces and tabs at either end. */
static gchar *
trim_blanks(const gchar *text)
{
	gsize len;

	while (*text == ' ' || *text == '\t')
		text++;
	len = strlen(text);
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'))
		len--;

	return g_strndup(text, len);
}

/* A second, plainer reading of the header: how many cookies are called
 * @name, and the trimmed value of the last. */
static guint
count_named(
	const gchar  *header,
	const gchar  *name,
	gchar       **value
){
	gchar **parts = g_strsplit(header, ";", -1);
	guint count = 0;
	guint i;

	*value = NULL;
	for (i = 0; parts[i] != NULL; i++) {
		gchar *eq = strchr(parts[i], '=');
		gchar *key;

		if (eq == NULL)
			continue;
		*eq = '\0';
		key = trim_blanks(parts[i]);
		if (strcmp(key, name) == 0) {
			g_free(*value);
			*value = trim_blanks(eq + 1);
			count++;
		}
		g_free(key);
	}
	g_strfreev(parts);

	return count;
}

static void
check(
	const gchar *header,
	const gchar *name
){
	gchar *value = axis_cookie_find(header, name);
	gchar *expected = NULL;
	guint count;
	gsize len;

	if (strlen(header) > AXIS_COOKIE_HEADER_MAX) {
		if (value != NULL)
			abort();
		return;
	}

	/* Names with ';', '=' or surrounding blanks cannot be matched the
	 * plain way; only memory safety is checked for them. */
	if (name[0] == '\0' || strpbrk(name, ";= \t") != NULL) {
		g_free(value);
		return;
	}

	count = count_named(header, name, &expected);
	if (count == 1) {
		if (value == NULL || strcmp(value, expected) != 0)
			abort();
	} else if (value != NULL) {
		abort();
	}

	if (value != NULL) {
		len = strlen(value);
		if (strchr(value, ';') != NULL || strstr(header, value) == NULL)
			abort();
		if (len > 0 && (value[0] == ' ' || value[0] == '\t' ||
		                value[len - 1] == ' ' || value[len - 1] == '\t'))
			abort();
	}

	g_free(expected);
	g_free(value);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);
	gchar *newline = strchr(text, '\n');

	if (newline != NULL) {
		*newline = '\0';
		check(newline + 1, text);
		check(newline + 1, "axis_session");
		check(newline + 1, "__Host-axis_session");
	} else {
		check(text, "axis_session");
		check(text, "axis_presession");
	}

	return 0;
}
