/*
 * axis-library-name.c - file names are bytes: storing, comparing and
 * showing them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-library-name.h"

#include <string.h>

static const gchar hex_digits[] = "0123456789ABCDEF";

gsize
axis_library_utf8_sequence(
	const guchar *p,
	gsize         left
){
	guchar c;

	if (left == 0)
		return 0;
	c = p[0];

	if (c >= 0x01 && c <= 0x7f)
		return 1;
	if (c >= 0xc2 && c <= 0xdf)
		return left >= 2 && (p[1] & 0xc0) == 0x80 ? 2 : 0;
	if (c >= 0xe0 && c <= 0xef) {
		guchar lo = 0x80;
		guchar hi = 0xbf;

		if (c == 0xe0)
			lo = 0xa0;	/* no overlong forms */
		else if (c == 0xed)
			hi = 0x9f;	/* no surrogates */
		if (left < 3 || p[1] < lo || p[1] > hi || (p[2] & 0xc0) != 0x80)
			return 0;
		return 3;
	}
	if (c >= 0xf0 && c <= 0xf4) {
		guchar lo = 0x80;
		guchar hi = 0xbf;

		if (c == 0xf0)
			lo = 0x90;	/* no overlong forms */
		else if (c == 0xf4)
			hi = 0x8f;	/* nothing above U+10FFFF */
		if (left < 4 || p[1] < lo || p[1] > hi || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80)
			return 0;
		return 4;
	}

	/* NUL, a continuation byte, 0xc0, 0xc1, 0xf5 and above. */
	return 0;
}

gboolean
axis_library_utf8_valid(
	const gchar *bytes,
	gsize        length
){
	const guchar *p = (const guchar *)bytes;
	gsize i = 0;

	while (i < length) {
		gsize n = axis_library_utf8_sequence(p + i, length - i);

		if (n == 0)
			return FALSE;
		i += n;
	}

	return TRUE;
}

static void
append_hex_byte(
	GString *out,
	guchar   byte
){
	g_string_append_c(out, '\\');
	g_string_append_c(out, 'x');
	g_string_append_c(out, hex_digits[byte >> 4]);
	g_string_append_c(out, hex_digits[byte & 0x0f]);
}

gboolean
axis_library_name_encode(
	GString     *out,
	const gchar *name,
	gsize        length
){
	const guchar *p = (const guchar *)name;
	gboolean plain = TRUE;
	gboolean valid = TRUE;
	gsize i = 0;

	g_return_val_if_fail(out != NULL, FALSE);
	g_return_val_if_fail(name != NULL || length == 0, FALSE);

	while (i < length) {
		gsize n = axis_library_utf8_sequence(p + i, length - i);

		if (n == 0 || p[i] == '\\') {
			plain = FALSE;
			break;
		}
		i += n;
	}
	if (plain) {
		g_string_append_len(out, name, (gssize)length);
		return TRUE;
	}

	for (i = 0; i < length;) {
		gsize n = axis_library_utf8_sequence(p + i, length - i);

		if (n == 0) {
			append_hex_byte(out, p[i]);
			valid = FALSE;
			i++;
		} else if (p[i] == '\\') {
			g_string_append(out, "\\\\");
			i++;
		} else {
			g_string_append_len(out, (const gchar *)p + i, (gssize)n);
			i += n;
		}
	}

	return valid;
}

static gint
upper_hex_value(gchar c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

gboolean
axis_library_name_decode(
	const gchar  *stored,
	GString     **bytes
){
	GString *out;
	GString *again;
	const gchar *p;
	gboolean canonical;

	if (bytes != NULL)
		*bytes = NULL;
	if (stored == NULL)
		return FALSE;

	if (strchr(stored, '\\') == NULL) {
		if (!axis_library_utf8_valid(stored, strlen(stored)))
			return FALSE;
		if (bytes != NULL)
			*bytes = g_string_new(stored);
		return TRUE;
	}

	out = g_string_new(NULL);
	for (p = stored; *p != '\0';) {
		if (*p != '\\') {
			g_string_append_c(out, *p);
			p++;
			continue;
		}
		if (p[1] == '\\') {
			g_string_append_c(out, '\\');
			p += 2;
			continue;
		}
		if (p[1] == 'x' && upper_hex_value(p[2]) >= 0 && upper_hex_value(p[3]) >= 0) {
			guchar byte = (guchar)((upper_hex_value(p[2]) << 4) | upper_hex_value(p[3]));

			if (byte == 0)
				goto refuse;
			g_string_append_c(out, (gchar)byte);
			p += 4;
			continue;
		}
		goto refuse;
	}

	/* Canonical: the one form encoding gives. */
	again = g_string_sized_new(strlen(stored));
	axis_library_name_encode(again, out->str, out->len);
	canonical = strcmp(again->str, stored) == 0;
	g_string_free(again, TRUE);
	if (!canonical)
		goto refuse;

	if (bytes != NULL)
		*bytes = out;
	else
		g_string_free(out, TRUE);
	return TRUE;

refuse:
	g_string_free(out, TRUE);
	return FALSE;
}

gboolean
axis_library_name_is_invalid(const gchar *stored)
{
	GString *bytes = NULL;
	gboolean invalid;

	if (!axis_library_name_decode(stored, &bytes))
		return FALSE;
	invalid = !axis_library_utf8_valid(bytes->str, bytes->len);
	g_string_free(bytes, TRUE);

	return invalid;
}

/* The code point of the valid sequence of @n bytes at @p. */
static gunichar
code_point(
	const guchar *p,
	gsize         n
){
	switch (n) {
	case 1:
		return p[0];
	case 2:
		return ((gunichar)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
	case 3:
		return ((gunichar)(p[0] & 0x0f) << 12) | ((gunichar)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
	default:
		return ((gunichar)(p[0] & 0x07) << 18) | ((gunichar)(p[1] & 0x3f) << 12) |
			((gunichar)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
	}
}

/* Characters that change the direction of the text around them, or end a
 * line in some renderers, and so must never be shown raw. */
static gboolean
is_layout_control(gunichar c)
{
	return c == 0x061c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) ||
		(c >= 0x2066 && c <= 0x2069) || c == 0x2028 || c == 0x2029;
}

static gboolean
is_control(gunichar c)
{
	return c < 0x20 || c == 0x7f || (c >= 0x80 && c <= 0x9f);
}

gchar *
axis_library_name_display(const gchar *stored)
{
	GString *bytes = NULL;
	GString *out;
	const guchar *p;
	gsize i;

	if (stored == NULL)
		return g_strdup("");
	if (!axis_library_name_decode(stored, &bytes))
		return g_strdup("(unreadable)");

	out = g_string_sized_new(bytes->len + 8);
	p = (const guchar *)bytes->str;
	for (i = 0; i < bytes->len;) {
		gsize n = axis_library_utf8_sequence(p + i, bytes->len - i);
		gunichar c;

		if (n == 0) {
			g_string_append(out, "\xef\xbf\xbd");
			i++;
			continue;
		}
		c = code_point(p + i, n);
		if (c == '\n') {
			g_string_append(out, "\\n");
		} else if (c == '\t') {
			g_string_append(out, "\\t");
		} else if (c == '\r') {
			g_string_append(out, "\\r");
		} else if (c < 0x20 || c == 0x7f) {
			append_hex_byte(out, (guchar)c);
		} else if (is_control(c) || is_layout_control(c)) {
			g_string_append_printf(out, "\\u{%04X}", (guint)c);
		} else {
			g_string_append_len(out, (const gchar *)p + i, (gssize)n);
		}
		i += n;
	}

	g_string_free(bytes, TRUE);
	return g_string_free(out, FALSE);
}

gchar *
axis_library_name_exact(const gchar *stored)
{
	GString *bytes = NULL;
	GString *out;
	const guchar *p;
	gsize i;

	if (!axis_library_name_decode(stored, &bytes))
		return g_strdup("(unreadable)");

	out = g_string_sized_new(bytes->len + 8);
	p = (const guchar *)bytes->str;
	for (i = 0; i < bytes->len;) {
		gsize n = axis_library_utf8_sequence(p + i, bytes->len - i);
		gunichar c;
		gsize k;

		if (n == 0) {
			append_hex_byte(out, p[i]);
			i++;
			continue;
		}
		c = code_point(p + i, n);
		if (c == '\\') {
			g_string_append(out, "\\\\");
		} else if (is_control(c) || is_layout_control(c)) {
			for (k = 0; k < n; k++)
				append_hex_byte(out, p[i + k]);
		} else {
			g_string_append_len(out, (const gchar *)p + i, (gssize)n);
		}
		i += n;
	}

	g_string_free(bytes, TRUE);
	return g_string_free(out, FALSE);
}

gchar *
axis_library_text_clean(
	const gchar *text,
	guint        max_chars
){
	GString *out;
	const guchar *p;
	gsize length;
	gsize i;
	guint chars = 0;

	if (text == NULL)
		return NULL;

	length = strlen(text);
	out = g_string_sized_new(MIN(length, (gsize)max_chars * 4) + 1);
	p = (const guchar *)text;
	for (i = 0; i < length && chars < max_chars; chars++) {
		gsize n = axis_library_utf8_sequence(p + i, length - i);
		gunichar c;

		if (n == 0) {
			g_string_append(out, "\xef\xbf\xbd");
			i++;
			continue;
		}
		c = code_point(p + i, n);
		if (is_control(c) || is_layout_control(c))
			g_string_append_c(out, ' ');
		else
			g_string_append_len(out, (const gchar *)p + i, (gssize)n);
		i += n;
	}

	return g_string_free(out, FALSE);
}

void
axis_library_json_append_string(
	GString     *out,
	const gchar *utf8
){
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)utf8; *p != '\0'; p++) {
		switch (*p) {
		case '"':
			g_string_append(out, "\\\"");
			break;
		case '\\':
			g_string_append(out, "\\\\");
			break;
		case '\n':
			g_string_append(out, "\\n");
			break;
		case '\r':
			g_string_append(out, "\\r");
			break;
		case '\t':
			g_string_append(out, "\\t");
			break;
		default:
			if (*p < 0x20)
				g_string_append_printf(out, "\\u%04x", (guint)*p);
			else
				g_string_append_c(out, (gchar)*p);
			break;
		}
	}
	g_string_append_c(out, '"');
}
