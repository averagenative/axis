/*
 * axis-web-logo.c - the Axis mark, drawn inline
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The mark is a line that has pivoted about an axis point, with the sweep
 * behind each half shaded in three steps that brighten toward the line.
 * It is built from element nodes as the charts are, so that the theme can
 * colour it: the geometry is in attributes, the colours are the classes
 * logo-step-1 (furthest from the line) to logo-step-3 and logo-line, which
 * data/web/axis.css fills from the theme's variables.  An <img> could not
 * be recoloured.  The hub is cut with a mask, so whatever is behind the
 * mark shows through it.
 *
 * The geometry is copied exactly from data/web/icons/logo-mark.svg, which
 * is final; do not redraw it here.  The mask's white and black are the
 * mask's own (a mask reads brightness), not the theme's.
 */

#include "web/axis-web-private.h"

#include <string.h>

#define VIEW_BOX "0 0 64 64"

/* The six wedges of the sweep and the line, as in logo-mark.svg. */
static const struct
{
	const gchar *css;
	const gchar *path;
} wedges[] = {
	{ "logo-step-1", "M32 32 L32 3.3 A28.7 28.7 0 0 1 40.87 4.7 Z" },
	{ "logo-step-2", "M32 32 L40.87 4.7 A28.7 28.7 0 0 1 48.87 8.78 Z" },
	{ "logo-step-3", "M32 32 L48.87 8.78 A28.7 28.7 0 0 1 55.5 15.54 Z" },
	{ "logo-step-1", "M32 32 L32 60.7 A28.7 28.7 0 0 1 23.13 59.3 Z" },
	{ "logo-step-2", "M32 32 L23.13 59.3 A28.7 28.7 0 0 1 15.13 55.22 Z" },
	{ "logo-step-3", "M32 32 L15.13 55.22 A28.7 28.7 0 0 1 8.5 48.46 Z" },
	{ NULL, NULL }
};

#define LINE_PATH "M6.6 49.8 L57.4 14.2"

static HtmxNode *
node_with(
	const gchar *tag,
	const gchar *const *attributes
){
	HtmxNode *node = htmx_node_new(tag);
	guint i;

	for (i = 0; attributes[i] != NULL; i += 2)
		htmx_element_set_attribute(HTMX_ELEMENT(node), attributes[i], attributes[i + 1]);

	return node;
}

/* Adds @child to @parent and drops the caller's reference. */
static void
add_owned(
	HtmxNode *parent,
	HtmxNode *child
){
	htmx_node_add_child(parent, child);
	g_object_unref(child);
}

HtmxNode *
axis_web_logo_new(
	const gchar *mask_id,
	const gchar *css_class
){
	HtmxNode *svg = htmx_node_new("svg");
	HtmxNode *defs = htmx_node_new("defs");
	HtmxNode *mask = htmx_node_new("mask");
	HtmxNode *group = htmx_node_new("g");
	g_autofree gchar *mask_ref = g_strdup_printf("url(#%s)", mask_id);
	const gchar *const veil[] = { "width", "64", "height", "64", "fill", "white", NULL };
	const gchar *const hub[] = { "cx", "32", "cy", "32", "r", "3.6", "fill", "black", NULL };
	const gchar *const line[] = { "d", LINE_PATH, "stroke-width", "3", NULL };
	HtmxNode *line_node;
	guint i;

	g_return_val_if_fail(mask_id != NULL, NULL);

	htmx_element_add_class(HTMX_ELEMENT(svg), "logo");
	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(svg), css_class);
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "viewBox", VIEW_BOX);
	/* The name is always beside the mark, so the mark is not announced. */
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "aria-hidden", "true");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "focusable", "false");

	htmx_element_set_attribute(HTMX_ELEMENT(mask), "id", mask_id);
	add_owned(mask, node_with("rect", veil));
	add_owned(mask, node_with("circle", hub));
	add_owned(defs, mask);
	add_owned(svg, defs);

	htmx_element_set_attribute(HTMX_ELEMENT(group), "mask", mask_ref);
	for (i = 0; wedges[i].css != NULL; i++) {
		HtmxNode *wedge = htmx_node_new("path");

		htmx_element_add_class(HTMX_ELEMENT(wedge), wedges[i].css);
		htmx_element_set_attribute(HTMX_ELEMENT(wedge), "d", wedges[i].path);
		add_owned(group, wedge);
	}
	line_node = node_with("path", line);
	htmx_element_add_class(HTMX_ELEMENT(line_node), "logo-line");
	add_owned(group, line_node);
	add_owned(svg, group);

	return svg;
}

HtmxNode *
axis_web_lockup_new(void)
{
	HtmxDiv *lockup = htmx_div_new();
	HtmxSpan *name = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(lockup), "lockup");
	add_owned(HTMX_NODE(lockup), axis_web_logo_new("axis-hub-lockup", NULL));
	htmx_element_add_class(HTMX_ELEMENT(name), "lockup-name");
	htmx_node_set_text_content(HTMX_NODE(name), "Axis");
	add_owned(HTMX_NODE(lockup), HTMX_NODE(name));

	return HTMX_NODE(lockup);
}
