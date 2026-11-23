/*
 * axis-web-table.h - data tables whose columns line up
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every data table of the interface is made here.  A table says what kind
 * of table it is and what each of its columns holds (its role); the
 * stylesheet gives every role a fixed width (the name takes what is left)
 * and lays the table out from those widths alone, never from what is in
 * it.  So two tables of one kind, on one page or on two, have their
 * columns at the same places whatever they hold.
 *
 * The cells take their column's role when the page is rendered:
 * axis_web_page_respond() calls axis_web_table_apply_roles() on the
 * content, which gives every cell of every such table its column's role
 * class (numbers are right-aligned in figures of equal width, actions sit
 * at the right edge).  A fragment rendered by other means (a live update)
 * calls axis_web_table_apply_roles() itself, or, for a lone row,
 * axis_web_table_row_roles() with the table's columns.
 *
 * A file or folder name in a table stays text in its <bdi>: the name
 * column is never cut, and its text wraps.
 */

#ifndef AXIS_WEB_TABLE_H
#define AXIS_WEB_TABLE_H

#include <glib.h>
#include <htmx-glib.h>

G_BEGIN_DECLS

/**
 * AxisWebColumnRole:
 * @AXIS_WEB_COL_NAME: a name or a title: takes the width the other
 *   columns leave, never cut, wraps
 * @AXIS_WEB_COL_WIDE: longer text (what a job is doing, what a file
 *   holds, a description): shares the width left with the name
 * @AXIS_WEB_COL_TEXT: a short label: a kind, a type, a codec, a user
 * @AXIS_WEB_COL_CODE: an identifier in code, with its number if it has
 *   one ("#46 probe.file")
 * @AXIS_WEB_COL_COUNT: a whole number, right-aligned
 * @AXIS_WEB_COL_NUMBER: another figure (a ratio, a share, "2 / 3"),
 *   right-aligned
 * @AXIS_WEB_COL_SIZE: a size with its unit, right-aligned
 * @AXIS_WEB_COL_RANGE: an estimate with its range ("about 1.2 GiB (1.0
 *   GiB to 1.4 GiB)"), right-aligned, on two lines if it must
 * @AXIS_WEB_COL_DATE: a date and time, in the one form
 *   (axis_web_format_time())
 * @AXIS_WEB_COL_STATE: a status badge
 * @AXIS_WEB_COL_LINKS: links to other pages
 * @AXIS_WEB_COL_ACTIONS: a button or two, at the right edge of the table
 * @AXIS_WEB_COL_CONTROLS: several controls (small forms with their own
 *   fields): shares the width left, at the right edge of the table
 */
typedef enum
{
	AXIS_WEB_COL_NAME,
	AXIS_WEB_COL_WIDE,
	AXIS_WEB_COL_TEXT,
	AXIS_WEB_COL_CODE,
	AXIS_WEB_COL_COUNT,
	AXIS_WEB_COL_NUMBER,
	AXIS_WEB_COL_SIZE,
	AXIS_WEB_COL_RANGE,
	AXIS_WEB_COL_DATE,
	AXIS_WEB_COL_STATE,
	AXIS_WEB_COL_LINKS,
	AXIS_WEB_COL_ACTIONS,
	AXIS_WEB_COL_CONTROLS
} AxisWebColumnRole;

#define AXIS_WEB_COL_ROLE_COUNT (AXIS_WEB_COL_CONTROLS + 1)

/**
 * AxisWebColumn:
 * @heading: the column's heading (plain text; "" for none); %NULL ends a
 *   list of columns
 * @role: what the column holds
 */
typedef struct
{
	const gchar       *heading;
	AxisWebColumnRole  role;
} AxisWebColumn;

/**
 * axis_web_table_new:
 * @kind: what the table lists, lower-case letters, digits and '-', such as
 *   "roots" or "jobs".  Tables of one kind must have the same columns.
 * @columns: the columns, ended by one whose heading is %NULL
 *
 * A table with class "data-table" and "kind-@kind", a <colgroup> with a
 * <col> per column carrying its role class, and a <thead> with the
 * headings (each with its column's role class too).  Add the <tbody>.
 *
 * Returns: (transfer full): the table
 */
HtmxNode *
axis_web_table_new(
	const gchar         *kind,
	const AxisWebColumn *columns
);

/**
 * AxisWebColumnSort:
 * @href: (nullable): where the heading's link goes (it sorts the table
 *   by the column); %NULL: a heading with no link
 * @sorted: (nullable): "ascending" or "descending" when the table is
 *   sorted by this column (the heading's aria-sort), else %NULL
 */
typedef struct
{
	const gchar *href;
	const gchar *sorted;
} AxisWebColumnSort;

/**
 * axis_web_table_new_sortable:
 * @kind: as for axis_web_table_new()
 * @columns: as for axis_web_table_new()
 * @sorts: one entry for each of @columns: a heading with an @href is a
 *   link, and the one with @sorted says how the table is sorted
 *
 * Like axis_web_table_new(), with links in the headings: the sorting is
 * done by the server, for a page's query.
 *
 * Returns: (transfer full): the table
 */
HtmxNode *
axis_web_table_new_sortable(
	const gchar             *kind,
	const AxisWebColumn     *columns,
	const AxisWebColumnSort *sorts
);

/**
 * axis_web_table_apply_roles:
 * @root: an element
 *
 * Gives every cell of every table made by axis_web_table_new() under
 * @root (itself included) its column's role class, and every body cell
 * (<td>) of one column its column's heading as "data-label" (none for a
 * column without a heading): on a phone a table's rows are stacked and
 * each cell shows its heading before its value.  Does nothing to a row that
 * already has them.  Cheap enough to run on every page.
 */
void
axis_web_table_apply_roles(HtmxNode *root);

/**
 * axis_web_table_row_roles:
 * @row: a <tr>
 * @columns: the columns of its table
 *
 * Gives the cells of @row their roles and labels now: for a row rendered on
 * its own, as a live update.
 */
void
axis_web_table_row_roles(
	HtmxNode            *row,
	const AxisWebColumn *columns
);

/* The class of @role, such as "col-count". */
const gchar *
axis_web_column_role_class(AxisWebColumnRole role);

/*
 * The width of @role in hundredths of a rem, as axis.css sets it on its
 * <col>; 0 for the roles that take what is left.  (A test checks that the
 * stylesheet agrees.)
 */
guint
axis_web_column_role_width(AxisWebColumnRole role);

/**
 * axis_web_format_time:
 * @us: microseconds since 1970, or 0 (or less) for none
 * @none: what to say for none
 *
 * The one form of a date and time in the interface:
 * "2026-11-13 16:55:09 UTC".
 *
 * Returns: (transfer full): the text
 */
gchar *
axis_web_format_time(
	gint64       us,
	const gchar *none
);

G_END_DECLS

#endif /* AXIS_WEB_TABLE_H */
