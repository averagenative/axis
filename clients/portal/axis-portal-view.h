/*
 * axis-portal-view.h - the pages of axis-portal, as text
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  Every page is written here, on the
 * server, from what the readers of axis-portal-wire.h gave: no script, no
 * inline style, nothing loaded from anywhere but the portal itself.
 *
 * One rule holds the pages together.  The markup is this file's own
 * literals; everything else (a title's name, an overview, a reason, a name,
 * what was typed into the search box) goes through axis_portal_view_escape(),
 * in element text and in attribute values alike, and every attribute value
 * is written between double quotes.  Text from a provider, from an
 * administrator or from a browser is therefore never markup, wherever it is
 * put.  No page holds the session proof, the portal's token or a device
 * value; the CSRF token is in a hidden field of a posted form and nowhere
 * else, never in an address.
 *
 * GLib only, no I/O: read by the fuzz target (tests/fuzz/fuzz-portal-answer.c).
 */

#ifndef AXIS_PORTAL_VIEW_H
#define AXIS_PORTAL_VIEW_H

#include <glib.h>

#include "axis-portal-wire.h"

G_BEGIN_DECLS

/* The name of the CSRF token's field in every posted form. */
#define AXIS_PORTAL_CSRF_FIELD "_csrf"

/**
 * axis_portal_view_escape:
 * @out: where the text goes
 * @text: (nullable): any text
 *
 * Appends @text so that it can stand in element text or inside a quoted
 * attribute value: '&', '<', '>', '"', ''' and '`' as character references,
 * every control character dropped (a line break and a tab become a space),
 * and bytes that are not valid UTF-8 replaced.
 */
void
axis_portal_view_escape(
	GString     *out,
	const gchar *text
);

/* Who a page is for: %NULL @who is nobody (no navigation is drawn). */
typedef struct
{
	const AxisPortalWho *who;
	const gchar         *csrf;	/* the session's CSRF token, for its forms */
} AxisPortalFrame;

/* The login page.  @form_token: the pre-login form's token, or %NULL for no
 * form at all (the page then says only @message).  @next: (nullable) a path
 * of the portal to go to afterwards, checked by the caller. */
gchar *
axis_portal_view_login(
	const gchar *form_token,
	const gchar *next,
	const gchar *message
);

/* A page that says one thing: a refusal, a failure, a path that is not one. */
gchar *
axis_portal_view_message(
	const AxisPortalFrame *frame,
	const gchar           *heading,
	const gchar           *text
);

/* The lists to browse.  @list: the list shown (one of @hello's), or %NULL
 * when there is none; @titles: its titles, or %NULL. */
gchar *
axis_portal_view_home(
	const AxisPortalFrame  *frame,
	const AxisPortalHello  *hello,
	AxisPortalKind          kind,
	const gchar            *list,
	const AxisPortalTitles *titles
);

/* The search page.  @query: (nullable) what was looked for; @kind: -1 for
 * both; @titles: (nullable) what was found. */
gchar *
axis_portal_view_search(
	const AxisPortalFrame  *frame,
	const gchar            *query,
	gint                    kind,
	const AxisPortalTitles *titles
);

/* A title's page, with its request form.  @asked: (nullable) what the
 * request just made answered. */
gchar *
axis_portal_view_title(
	const AxisPortalFrame *frame,
	const AxisPortalTitleInfo *title,
	const AxisPortalAsked *asked
);

/* My requests.  @notice: (nullable) one sentence above the list. */
gchar *
axis_portal_view_requests(
	const AxisPortalFrame    *frame,
	const AxisPortalRequests *requests,
	const gchar              *notice
);

/* The portal's one stylesheet. */
const gchar *
axis_portal_view_style(void);

/* "A download failed. Axis is looking again." and the like; "" for none. */
const gchar *
axis_portal_view_cause_words(AxisPortalCause cause);

G_END_DECLS

#endif /* AXIS_PORTAL_VIEW_H */
