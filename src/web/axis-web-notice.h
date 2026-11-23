/*
 * axis-web-notice.h - a page's notice drawn where the person was working
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * After a form, the handler sends the browser back to the element it acted
 * on (web/axis-web-place.h: "?done=<word>&at=<id>#<id>").  The page then
 * draws its notice of what was done inside that element, not at the top:
 *
 *   AxisWebNotice notice;
 *
 *   axis_web_notice_begin(&notice, request, FALSE, text, content);
 *   ...
 *   card = ...;
 *   axis_web_notice_put(&notice, card, "settings");   (inside the card)
 *   ...
 *   axis_web_notice_end(&notice);
 *
 * Without a place the notice is drawn at the top of @content at once, as
 * before.  A place that the page did not draw (the row is gone) gets it at
 * the top after all, in a slot kept there for it.
 */

#ifndef AXIS_WEB_NOTICE_H
#define AXIS_WEB_NOTICE_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

typedef struct
{
	const gchar *place;	/* the request's, or %NULL */
	gchar       *text;	/* the notice, or %NULL for none */
	gboolean     error;
	HtmxNode    *slot;	/* (borrowed) at the top, for a place not drawn */
	gboolean     drawn;
} AxisWebNotice;

/**
 * axis_web_notice_begin:
 * @notice: the page's notice
 * @request: the page's request (its place: axis_web_place_of())
 * @error: drawn as an error
 * @text: (transfer full) (nullable): the notice; %NULL for none
 * @content: the page's content, to whose top it goes without a place
 */
void
axis_web_notice_begin(
	AxisWebNotice *notice,
	HtmxRequest   *request,
	gboolean       error,
	gchar         *text,
	HtmxNode      *content
);

/* Whether the notice belongs inside the element @id (and is not drawn yet). */
gboolean
axis_web_notice_is_at(
	const AxisWebNotice *notice,
	const gchar         *id
);

/* Draws the notice inside @element when @id is its place (once). */
void
axis_web_notice_put(
	AxisWebNotice *notice,
	HtmxNode      *element,
	const gchar   *id
);

/* The notice as an element when @id is its place (once), else %NULL; for an
 * element that cannot hold it directly (a table row: the caller puts it in
 * a cell). */
HtmxNode *
axis_web_notice_take(
	AxisWebNotice *notice,
	const gchar   *id
);

/* The notice of a place not drawn goes at the top; frees the text. */
void
axis_web_notice_end(AxisWebNotice *notice);

G_END_DECLS

#endif /* AXIS_WEB_NOTICE_H */
