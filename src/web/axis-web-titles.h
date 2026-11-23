/*
 * axis-web-titles.h - a list of titles at scale: its three views, its filter
 * line, its count and its pages, and each account's choice of view
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Films and Series lists and the library's folder lists of identified
 * titles are drawn here from the same rows in one of three views:
 *
 *   posters   the poster cards, a grid (the default)
 *   overview  one dense line each: a small poster, the title and year, the
 *             state, a line of facts, and the reason when it needs attention
 *   table     a data table (title, year, state, quality, size, added) for
 *             scanning many at once
 *
 * A list's address carries everything that decides what it shows (state,
 * name, sort, view, the page), so a filtered list can be bookmarked; links
 * are plain links and the name filter a plain GET form: no script.  The view
 * last chosen is kept per account and per list (user_list_view,
 * 0029_title_lists.sql).
 */

#ifndef AXIS_WEB_TITLES_H
#define AXIS_WEB_TITLES_H

#include <glib.h>
#include <htmx-glib.h>

#include "db/axis-db-pool.h"
#include "meta/axis-meta-model.h"
#include "web/axis-web-text.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_WEB_VIEW_POSTERS,
	AXIS_WEB_VIEW_OVERVIEW,
	AXIS_WEB_VIEW_TABLE
} AxisWebView;

#define AXIS_WEB_VIEW_N (AXIS_WEB_VIEW_TABLE + 1)

/* "posters", "overview", "table". */
const gchar *
axis_web_view_name(AxisWebView view);

gboolean
axis_web_view_from_name(
	const gchar *name,
	AxisWebView *view
);

/* Titles on a page of a list in @view: 60 as posters or an overview, 100 in
 * a table. */
guint
axis_web_view_page_size(AxisWebView view);

/**
 * axis_web_view_choose:
 * @pool: the database
 * @user_id: the account (0: none, nothing is kept)
 * @list: the list's name ("films", "series", "library-films",
 *   "library-series")
 * @asked: (nullable): the view in the address, if any
 * @view: (out): the view to show
 *
 * The view asked for (kept as the account's choice for @list when it is not
 * the one kept already), else the one kept, else posters.  A view that
 * cannot be kept is still shown.  Blocks: a worker thread.
 */
void
axis_web_view_choose(
	AxisDbPool  *pool,
	gint64       user_id,
	const gchar *list,
	const gchar *asked,
	AxisWebView *view
);

/* ── The rows ───────────────────────────────────────────────────── */

/**
 * AxisWebTitleRow:
 * @href: the title's page
 * @title, @year, @kind, @provider, @poster_ref: what its poster is drawn from
 * @poster: (nullable): the kept poster's hash
 * @mark: (nullable) (transfer full): its state's mark
 * @state: (nullable): its state in words, for the table
 * @reason: (nullable): why it is in its state
 * @attention: it needs a person: the overview shows the reason
 * @reason_href: (nullable): with @attention, where what is wrong is put
 *   right: the reason and the state's mark link there
 * @facts: (nullable): one line for the overview ("Balanced, WEB-DL 1080p,
 *   12 GiB")
 * @quality: (nullable): the table's detail column: a film's quality, a
 *   series' episodes
 * @size: its size on disk, -1 for none known
 * @added_us: when it was added, 0 for none known
 * @line: (nullable): a poster card's words above the name, in place of the
 *   kind and year (a library folder that is no title yet: "Folder")
 * @extra: (nullable) (transfer full): drawn under a poster card's overview
 *
 * One title as a list shows it; every field is borrowed but the nodes.
 */
typedef struct
{
	const gchar  *href;
	const gchar  *title;
	gint          year;
	AxisMetaKind  kind;
	const gchar  *provider;
	const gchar  *poster_ref;
	const gchar  *poster;
	HtmxNode     *mark;
	const gchar  *state;
	const gchar  *reason;
	gboolean      attention;
	const gchar  *reason_href;
	const gchar  *facts;
	const gchar  *quality;
	gint64        size;
	gint64        added_us;
	const gchar  *line;
	HtmxNode     *extra;
} AxisWebTitleRow;

/* Drops the row's nodes. */
void
axis_web_title_row_clear(AxisWebTitleRow *row);

/**
 * axis_web_titles_new:
 * @view: the view
 * @noun: the table's first heading ("Film", "Series")
 * @detail: the table's detail heading ("Quality", "Episodes")
 * @rows: (array length=n_rows): the titles, in order
 *
 * Returns: (transfer full): the titles in @view; their nodes are taken
 */
HtmxNode *
axis_web_titles_new(
	AxisWebView      view,
	const gchar     *noun,
	const gchar     *detail,
	AxisWebTitleRow *rows,
	guint            n_rows
);

/* ── The address and its controls ──────────────────────────────── */

/**
 * AxisWebListAddress:
 * @path: the list's page ("/films")
 * @state: (nullable): the state tab, by its name
 * @name: (nullable): what the name filter holds
 * @sort: (nullable): the order's name; @default_sort is not written
 * @default_sort: the order when none is given
 * @view: the view (always written: a link keeps it)
 * @after, @before: (nullable): a page's cursor
 */
typedef struct
{
	const gchar *path;
	const gchar *state;
	const gchar *name;
	const gchar *sort;
	const gchar *default_sort;
	AxisWebView  view;
	const gchar *after;
	const gchar *before;
} AxisWebListAddress;

/* The address as a link: the path and its query, escaped. */
gchar *
axis_web_list_href(const AxisWebListAddress *address);

/**
 * axis_web_list_bar_new:
 * @address: the list's address now
 * @what: what is filtered, for the box's label ("films")
 * @sorts: (array zero-terminated=1): the orders' names
 * @sort_labels: (array zero-terminated=1): their words
 *
 * The filter line: the name box with its button (a GET form that keeps the
 * state, the order and the view), and at its right the orders and the
 * views, each a link, the current one marked as a tab is.
 */
HtmxNode *
axis_web_list_bar_new(
	const AxisWebListAddress *address,
	const gchar              *what,
	const gchar *const       *sorts,
	const gchar *const       *sort_labels
);

/**
 * axis_web_list_tab_new:
 *
 * One state tab: its words and its count, linking to the list in that state
 * (@state %NULL: every state), the page and cursor left behind.
 *
 * Returns: (transfer full): an <li>
 */
HtmxNode *
axis_web_list_tab_new(
	const AxisWebListAddress *address,
	const gchar              *state,
	const gchar              *label,
	guint                     count
);

/**
 * axis_web_list_foot_new:
 * @matching: how many titles the list holds with its filters
 * @shown: how many are on this page
 * @noun: one title ("film"), @nouns: several ("films")
 * @next, @previous: (nullable): the cursors of the neighbouring pages
 *
 * The count ("5,000 films") and the links to the first, previous and next
 * pages.
 */
HtmxNode *
axis_web_list_foot_new(
	const AxisWebListAddress *address,
	guint                     matching,
	guint                     shown,
	const gchar              *noun,
	const gchar              *nouns,
	const gchar              *next,
	const gchar              *previous
);

G_END_DECLS

#endif /* AXIS_WEB_TITLES_H */
