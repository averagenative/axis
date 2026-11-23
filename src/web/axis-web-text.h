/*
 * axis-web-text.h - small pieces of text every page uses: names that break at
 * their separators, and counts that read right in the singular
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_TEXT_H
#define AXIS_WEB_TEXT_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/**
 * axis_web_breakable:
 * @text: (nullable): a release name, a file name or a path, as stored
 *
 * @text as a node a browser may wrap after a separator: a break opportunity
 * (<wbr>) follows each run of ".", "-", "_" or "/" that is followed by more
 * text, and nowhere else, so a word is never broken in the middle (unless
 * it is longer than the space, which the cell's stylesheet rule allows as a
 * last resort).  The node's text is @text exactly: copying the name from the
 * page gives the name, and every piece of it is set as text, so markup in a
 * name is escaped as everywhere else.
 *
 * Add it to the element that holds the name (a <bdi>, a cell).
 *
 * Returns: (transfer full): a node without an element of its own
 */
HtmxNode *
axis_web_breakable(const gchar *text);

/**
 * axis_web_count:
 * @n: a number
 * @one: what one of them is called ("release")
 * @many: what any other number of them are called ("releases")
 *
 * Returns: (transfer full): "1 release", "0 releases", "12 releases"
 */
gchar *
axis_web_count(
	guint        n,
	const gchar *one,
	const gchar *many
);

/**
 * axis_web_format_count:
 * @value: a whole number
 *
 * The one formatter of a count on a page: a comma between thousands, never
 * localised.
 *
 * Returns: (transfer full): "5,000"
 */
gchar *
axis_web_format_count(guint64 value);

G_END_DECLS

#endif /* AXIS_WEB_TEXT_H */
