/*
 * axis-web-place.h - after a form, back to the place a person was working,
 * with the notice there
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A form that changes something posts, and its handler answers with a
 * redirect back to a page.  Without more the browser shows that page from
 * its top, and a person working down a list scrolls back after every
 * click.  So the handler names a place on the page: the id of an element
 * (a card, a row, a list's heading).  The redirect's address ends in
 * "#<id>", which the browser scrolls to, with or without script (a 303, or
 * HX-Redirect for a request htmx made), and its query carries "at=<id>" as
 * well, because the fragment never reaches the server: the page reads it
 * with axis_web_place_of() and draws its notice of "done=<word>" at that
 * element (directly before it, or under the heading it is), not at the top.
 *
 * Nothing of a request reaches the address unchecked: the path is the
 * handler's own, the word is one of its fixed words, and the place passes
 * axis_web_place_id_valid() or is left out.
 */

#ifndef AXIS_WEB_PLACE_H
#define AXIS_WEB_PLACE_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/* The query parameters: the notice's fixed word, and the place. */
#define AXIS_WEB_PLACE_DONE "done"
#define AXIS_WEB_PLACE_AT   "at"

/* Whether @id can name a place: an ASCII letter, then up to 63 letters,
 * digits, '-' or '_' (also the rule for a "done" word). */
gboolean
axis_web_place_id_valid(const gchar *id);
/**
 * axis_web_place_location:
 * @path: the page: an absolute path, with a query or not, written by the
 *   handler
 * @done: (nullable): the handler's word for what was done
 * @place: (nullable): the id of the element to land at
 *
 * Returns: (transfer full): "<path>?done=<done>&at=<place>#<place>", each
 *   part left out when it is %NULL or not valid
 */
gchar *
axis_web_place_location(
	const gchar *path,
	const gchar *done,
	const gchar *place
);

/* The redirect to axis_web_place_location(): a 303, or for a request htmx
 * made, HX-Redirect. */
HtmxResponse *
axis_web_place_redirect(
	HtmxRequest *request,
	const gchar *path,
	const gchar *done,
	const gchar *place
);

/* The place a page was sent back to ("at"), or %NULL when there is none or
 * it is not a valid id.  Transfer none: the request's own. */
const gchar *
axis_web_place_of(HtmxRequest *request);

G_END_DECLS

#endif /* AXIS_WEB_PLACE_H */
