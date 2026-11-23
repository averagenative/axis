/*
 * axis-web-back.h - coming back to the page a form was posted from
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A form drawn on one page and handled by another module's route (an Add on
 * a Discover card, posted to the movies module) carries a hidden "back"
 * field: the page it was shown on.  The handler sends the browser there,
 * with what it did as "done=", when that field is a path on this site by
 * the rule the login's "next" uses (axis_auth_next_is_safe()); otherwise it
 * goes where it always went.
 */

#ifndef AXIS_WEB_BACK_H
#define AXIS_WEB_BACK_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/* The name of the field. */
#define AXIS_WEB_BACK_FIELD "back"

/**
 * axis_web_back_here:
 *
 * Returns: (transfer full) (nullable): the path and query @request is for,
 *   without any "done" or "at" parameter, when that is a path on this site;
 *   else %NULL
 */
gchar *
axis_web_back_here(HtmxRequest *request);

/**
 * axis_web_back_set_here:
 * @request: a request that draws a part of another page (a card that page
 *   loads by itself)
 * @page: (nullable): that page's path, written by the handler; %NULL: the
 *   request's own again
 *
 * From now on axis_web_back_here(), and so every way back drawn for
 * @request, is @page: a form in the part comes back to the page it is shown
 * on, not to the part's own address.
 */
void
axis_web_back_set_here(
	HtmxRequest *request,
	const gchar *page
);

/* A hidden "back" input holding axis_web_back_here(), or %NULL (transfer
 * full). */
HtmxNode *
axis_web_back_field(HtmxRequest *request);

/* The posted "back" field when it is a path on this site, else %NULL
 * (transfer full). */
gchar *
axis_web_back_posted(HtmxRequest *request);

/* The query parameter of a title page that names the list it was opened
 * from. */
#define AXIS_WEB_BACK_FROM "from"

/* The address of title @title_id's page opened from the list @request is
 * for (a Discover page, the search results): "/title/<id>?from=<list>", so
 * that its back link and its Add come back to the list at the title's card
 * ("#title-<id>").  Transfer full. */
gchar *
axis_web_back_title_href(
	HtmxRequest *request,
	gint64       title_id
);

/* The query parameter of a title page that carries the place an Add comes
 * back to when the list drops the card of what is added (see
 * axis_web_back_set_add_return()). */
#define AXIS_WEB_BACK_RETURN_AT "return_at"

/**
 * axis_web_back_from_add:
 * @request: a title page's request
 * @title_id: the title the page is for
 * @back: (out) (transfer full): the "back" for the Add form
 * @at: (out) (transfer full): the place for the Add form to come back to
 *
 * For an Add on a title page opened from a list: the list, with
 * "added=<id>" and the place from "return_at" when the list passed one, else
 * the list and the title's own card.
 *
 * Returns: %FALSE when the page was not opened from a list
 */
gboolean
axis_web_back_from_add(
	HtmxRequest *request,
	gint64       title_id,
	gchar      **back,
	gchar      **at
);

/* The list a title page was opened from (its "from"), when that is a path
 * on this site other than a title page; else %NULL.  Transfer full. */
gchar *
axis_web_back_from(HtmxRequest *request);

/* @back with "done=@key" added to its query (@key: letters, digits and
 * '_'). */
gchar *
axis_web_back_with_done(
	const gchar *back,
	const gchar *key
);

/* The query parameter that names the title an Add from a list added, when
 * its card is gone from the list (Discover with its "hide" on). */
#define AXIS_WEB_BACK_ADDED "added"

/**
 * axis_web_back_set_add_return:
 * @request: the list's request
 * @place: (nullable): the place an Add drawn from now on comes back to (the
 *   next card, or the block of the cards), its own card being gone after
 *   it; %NULL: each Add comes back to its own card again
 *
 * A list that drops the card of what is added (Discover, hiding what is had)
 * calls this before it draws each card's Add.
 */
void
axis_web_back_set_add_return(
	HtmxRequest *request,
	const gchar *place
);

/* The hidden "back" input of an Add on the card of @title_id: as
 * axis_web_back_field(), and, after axis_web_back_set_add_return(), naming
 * the title ("added=<id>").  Transfer full; %NULL as for that. */
HtmxNode *
axis_web_back_add_field(
	HtmxRequest *request,
	gint64       title_id
);

/* The place an Add on the card of @title_id comes back to: its card
 * ("title-<id>"), or what axis_web_back_set_add_return() set.  Transfer
 * full. */
gchar *
axis_web_back_add_place(
	HtmxRequest *request,
	gint64       title_id
);

G_END_DECLS

#endif /* AXIS_WEB_BACK_H */
