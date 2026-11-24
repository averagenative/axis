/*
 * axis-portal-form.h - how axis-portal reads what a browser sends: a path, a
 * query, a posted form
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  Everything a browser sends is a claim
 * from a stranger.  These are the only readers of it:
 *
 *  - a path is one of the portal's own, written exactly, or it is no path
 *    (axis_portal_route_parse()): there is no pattern that a prefix, an
 *    encoded character or a longer path could match;
 *  - a query or a posted form is "name=value" pairs within fixed limits
 *    (axis_portal_form_parse()): names of lower-case letters and '_', values
 *    of valid UTF-8 without a NUL, every escape two hexadecimal digits.
 *    Anything else refuses the whole form.  A field given twice has no value
 *    (only "season" may repeat), so two values can never be told apart
 *    differently by the portal and by whoever reads next.
 *
 * GLib only, no I/O: read by the fuzz target (tests/fuzz/fuzz-portal-form.c).
 */

#ifndef AXIS_PORTAL_FORM_H
#define AXIS_PORTAL_FORM_H

#include <glib.h>

#include "axis-portal-wire.h"

G_BEGIN_DECLS

/* The largest posted body, and the largest query, that is read. */
#define AXIS_PORTAL_FORM_MAX_BYTES     (64 * 1024)
#define AXIS_PORTAL_QUERY_MAX_BYTES    2048
#define AXIS_PORTAL_FORM_MAX_FIELDS    128
#define AXIS_PORTAL_FORM_NAME_MAX      32
#define AXIS_PORTAL_FORM_VALUE_MAX     4096

/* The longest path that can be one of the portal's. */
#define AXIS_PORTAL_PATH_MAX           64

typedef enum
{
	AXIS_PORTAL_ROUTE_NONE,
	AXIS_PORTAL_ROUTE_HOME,		/* /                          GET */
	AXIS_PORTAL_ROUTE_LOGIN,	/* /login                     GET, POST */
	AXIS_PORTAL_ROUTE_LOGOUT,	/* /logout                    POST */
	AXIS_PORTAL_ROUTE_SEARCH,	/* /search                    GET */
	AXIS_PORTAL_ROUTE_TITLE,	/* /title/<id>                GET */
	AXIS_PORTAL_ROUTE_ASK,		/* /title/<id>/request        POST */
	AXIS_PORTAL_ROUTE_REQUESTS,	/* /requests                  GET */
	AXIS_PORTAL_ROUTE_WITHDRAW,	/* /requests/<id>/withdraw    POST */
	AXIS_PORTAL_ROUTE_ART,		/* /art/<id>                  GET */
	AXIS_PORTAL_ROUTE_STYLE,	/* /static/portal.css         GET */
	AXIS_PORTAL_ROUTE_HEALTH	/* /healthz                   GET, loopback only */
} AxisPortalRoute;

#define AXIS_PORTAL_STYLE_PATH "/static/portal.css"

/**
 * axis_portal_route_parse:
 * @path: (nullable): a request's path
 * @id: (out): the title of a title, ask or art route; else 0
 * @request_id: (out) (transfer full) (nullable): the request of a withdraw
 *   route
 *
 * Returns: which of the portal's routes @path is, written exactly: a title's
 *   number has 1 to 15 digits and no leading zero, a request's identifier is
 *   22 characters of [A-Za-z0-9_-].  %AXIS_PORTAL_ROUTE_NONE for anything
 *   else, however close
 */
AxisPortalRoute
axis_portal_route_parse(
	const gchar  *path,
	gint64       *id,
	gchar       **request_id
);

/* Every path pattern of the portal, %NULL-ended, for the tests that walk
 * them ("<id>" stands for a title's number or a request's identifier). */
const gchar *const *
axis_portal_route_patterns(void);

typedef struct _AxisPortalForm AxisPortalForm;

/**
 * axis_portal_form_parse:
 * @data: (nullable): the pairs ("a=1&b=two+words")
 * @length: its length in bytes
 * @max_bytes: the most bytes read
 *
 * Returns: (transfer full) (nullable): the fields, or %NULL when @data is
 *   longer than @max_bytes or is not exactly such pairs (see the header of
 *   this file).  No pairs at all is a form with no field
 */
AxisPortalForm *
axis_portal_form_parse(
	const gchar *data,
	gsize        length,
	gsize        max_bytes
);

/* Wipes the values (one may be a password) and frees. */
void
axis_portal_form_free(AxisPortalForm *form);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalForm, axis_portal_form_free)

/* The value of the only field called @name; %NULL when there is none or
 * more than one. */
const gchar *
axis_portal_form_get(
	const AxisPortalForm *form,
	const gchar          *name
);

/* How many fields are called @name. */
guint
axis_portal_form_count(
	const AxisPortalForm *form,
	const gchar          *name
);

/* Whether every field's name is among @names (%NULL-ended). */
gboolean
axis_portal_form_only(
	const AxisPortalForm *form,
	const gchar *const   *names
);

/* A whole number written in plain decimal digits, without a sign, a space
 * or a leading zero, in [@minimum, @maximum]. */
gboolean
axis_portal_form_number(
	const gchar *text,
	gint64       minimum,
	gint64       maximum,
	gint64      *value
);

/* "movie" or "series". */
gboolean
axis_portal_form_kind(
	const gchar    *text,
	AxisPortalKind *kind
);

/**
 * axis_portal_form_seasons:
 * @form: a posted request for a series
 * @ask: (out): what "what" says: all, first, latest or picked
 * @picked: (out) (array length=n_picked): the "season" fields' numbers, for
 *   picked; room for 100
 * @n_picked: (out)
 *
 * Returns: %FALSE when "what" is not one of the four, or when picked comes
 *   with no season, with more than 100, with one twice or with one that is
 *   not a season's number
 */
gboolean
axis_portal_form_seasons(
	const AxisPortalForm *form,
	AxisPortalAsk        *ask,
	gint                 *picked,
	guint                *n_picked
);

G_END_DECLS

#endif /* AXIS_PORTAL_FORM_H */
