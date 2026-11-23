/*
 * axis-web-text.c - small pieces of text every page uses
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-text.h"

#include <string.h>

static gboolean
is_separator(gchar c)
{
	return c == '.' || c == '-' || c == '_' || c == '/';
}

/* @length bytes of @text as a node of its own, as text. */
static void
add_piece(
	HtmxNode    *parent,
	const gchar *text,
	gsize        length
){
	HtmxFragment *piece = htmx_fragment_new();
	g_autofree gchar *copy = g_strndup(text, length);

	htmx_node_set_text_content(HTMX_NODE(piece), copy);
	htmx_node_add_child(parent, HTMX_NODE(piece));
	g_object_unref(piece);
}

HtmxNode *
axis_web_breakable(const gchar *text)
{
	HtmxFragment *fragment = htmx_fragment_new();
	HtmxNode *node = HTMX_NODE(fragment);
	const gchar *start = text != NULL ? text : "";
	const gchar *p;
	gboolean broken = FALSE;
	gboolean word = FALSE;

	/* A separator that ends a run, with more of the same word after it, ends
	 * a piece (the separators stay at its end, so the pieces add up to the
	 * text).  A sentence's own full stop, before a space, ends nothing, and
	 * neither does a run of separators with no word before it. */
	for (p = start; *p != '\0'; p++) {
		HtmxWbr *wbr;

		if (!is_separator(*p)) {
			word = TRUE;
			continue;
		}
		if (!word || p[1] == '\0' || is_separator(p[1]) || g_ascii_isspace(p[1]))
			continue;
		add_piece(node, start, (gsize)(p + 1 - start));
		wbr = htmx_wbr_new();
		htmx_node_add_child(node, HTMX_NODE(wbr));
		g_object_unref(wbr);
		start = p + 1;
		broken = TRUE;
		word = FALSE;
	}
	if (!broken)
		htmx_node_set_text_content(node, start);
	else if (*start != '\0')
		add_piece(node, start, strlen(start));

	return node;
}

gchar *
axis_web_count(
	guint        n,
	const gchar *one,
	const gchar *many
){
	g_autofree gchar *number = axis_web_format_count(n);

	return g_strdup_printf("%s %s", number, n == 1 ? one : many);
}

gchar *
axis_web_format_count(guint64 value)
{
	g_autofree gchar *digits = g_strdup_printf("%" G_GUINT64_FORMAT, value);
	GString *out = g_string_new(NULL);
	gsize n = strlen(digits);
	gsize i;

	for (i = 0; i < n; i++) {
		if (i > 0 && (n - i) % 3 == 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, digits[i]);
	}
	return g_string_free(out, FALSE);
}
