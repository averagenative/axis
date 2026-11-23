/*
 * axis-web-table.c - data tables whose columns line up
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-table.h"

#include <string.h>

/* A row whose cells already carry their roles. */
#define ROW_DONE_KEY "axis-web-table-row-done"

/*
 * Each role's class and its width in hundredths of a rem.  data/web/axis.css
 * sets the same widths on col.<class>; a test reads them from there and
 * compares.  The roles of width 0 share what the others leave.
 *
 * A fixed width holds its role's longest value in the widest face a browser
 * may draw it in, plus the cell's padding (1.5rem): measured in headless
 * Chrome at the tables' 15 px under nine faces (Noto Sans, Liberation Sans,
 * Cantarell, Adwaita Sans, Open Sans, FreeSans, Red Hat Text, Montserrat,
 * Comfortaa).  The widest, in pixels of content: a date
 * ("2026-11-15 00:10:10 UTC") 203.5, a count (7 digits) 73.5, a number
 * ("999.9 MiB/s", "1234 / 5678") 96.9, a size ("1023.9 MiB") 89.3, a state
 * badge 102.2.  A value that still does not fit is clipped inside its cell
 * (axis.css), never drawn over the next one, and its cell carries it whole
 * in its title (axis_web_table_apply_roles()).
 */
static const struct
{
	const gchar *css;
	guint        width;
	gboolean     numeric;
} roles[AXIS_WEB_COL_ROLE_COUNT] = {
	[AXIS_WEB_COL_NAME]    = { "col-name", 0, FALSE },
	[AXIS_WEB_COL_WIDE]    = { "col-wide", 0, FALSE },
	[AXIS_WEB_COL_TEXT]    = { "col-text", 800, FALSE },
	[AXIS_WEB_COL_CODE]    = { "col-code", 1200, FALSE },
	[AXIS_WEB_COL_COUNT]   = { "col-count", 625, TRUE },
	[AXIS_WEB_COL_NUMBER]  = { "col-number", 775, TRUE },
	[AXIS_WEB_COL_SIZE]    = { "col-size", 725, TRUE },
	[AXIS_WEB_COL_RANGE]   = { "col-range", 1200, TRUE },
	[AXIS_WEB_COL_DATE]    = { "col-date", 1425, FALSE },
	[AXIS_WEB_COL_STATE]   = { "col-state", 850, FALSE },
	[AXIS_WEB_COL_LINKS]   = { "col-links", 1050, FALSE },
	[AXIS_WEB_COL_ACTIONS] = { "col-actions", 750, FALSE },
	[AXIS_WEB_COL_CONTROLS] = { "col-controls", 0, FALSE }
};

/*
 * The least width, in rem, left for the columns that share the rest: the
 * first of them and each further one.  A table narrower than its fixed
 * columns and this much scrolls sideways in its card rather than squeezing
 * its names.
 */
#define SHARED_MIN_REM  8
#define SHARED_MORE_REM 6

/*
 * On a phone, where a table is as wide as its least width and scrolls
 * sideways in its card, the columns that share the rest need more than that
 * to hold text: a name about two words and a half wide (a release name wraps
 * after its dots and hyphens, so each line holds a few of them), a column of
 * details about four words.  These least widths are in "least-N" classes of
 * axis.css that apply only on a phone (a screen at most 40rem wide), so that
 * no table is laid out any differently on a wider screen.
 */
#define PHONE_NAME_REM 14
#define PHONE_WIDE_REM 16

/* And for a column of several controls, which need a line of their own. */
#define CONTROLS_MIN_REM 20

/* The steps of the tables' least widths, in rem: one class each in axis.css. */
#define MIN_STEP_REM  1
#define MIN_FIRST_REM 24
#define MIN_LAST_REM  96

const gchar *
axis_web_column_role_class(AxisWebColumnRole role)
{
	g_return_val_if_fail((guint)role < AXIS_WEB_COL_ROLE_COUNT, "col-text");

	return roles[role].css;
}

guint
axis_web_column_role_width(AxisWebColumnRole role)
{
	g_return_val_if_fail((guint)role < AXIS_WEB_COL_ROLE_COUNT, 0);

	return roles[role].width;
}

static gboolean
kind_is_valid(const gchar *kind)
{
	const gchar *p;

	if (kind == NULL || kind[0] == '\0' || strlen(kind) > 40)
		return FALSE;
	for (p = kind; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
			return FALSE;
	}

	return TRUE;
}

/* Adds @child to @parent and drops the caller's reference. */
static void
add_owned(
	HtmxNode *parent,
	gpointer  child
){
	htmx_node_add_child(parent, HTMX_NODE(child));
	g_object_unref(child);
}

HtmxNode *
axis_web_table_new(
	const gchar         *kind,
	const AxisWebColumn *columns
){
	return axis_web_table_new_sortable(kind, columns, NULL);
}

HtmxNode *
axis_web_table_new_sortable(
	const gchar             *kind,
	const AxisWebColumn     *columns,
	const AxisWebColumnSort *sorts
){
	HtmxTable *table = htmx_table_new();
	HtmxNode *group = htmx_node_new("colgroup");
	HtmxThead *head = htmx_thead_new();
	HtmxTr *row = htmx_tr_new();
	g_autofree gchar *kind_class = NULL;
	g_autofree gchar *min_class = NULL;
	guint shares = 0;
	guint phone_shared = 0;
	gboolean controls = FALSE;
	guint fixed = 0;
	guint least;
	guint phone;
	guint i;

	g_return_val_if_fail(kind_is_valid(kind), HTMX_NODE(table));
	g_return_val_if_fail(columns != NULL, HTMX_NODE(table));

	kind_class = g_strconcat("kind-", kind, NULL);
	htmx_element_add_class(HTMX_ELEMENT(table), "data-table");
	htmx_element_add_class(HTMX_ELEMENT(table), kind_class);

	for (i = 0; columns[i].heading != NULL; i++) {
		AxisWebColumnRole role = (guint)columns[i].role < AXIS_WEB_COL_ROLE_COUNT ?
			columns[i].role : AXIS_WEB_COL_TEXT;
		HtmxNode *col = htmx_node_new("col");
		HtmxTh *th = NULL;

		if (sorts != NULL && sorts[i].href != NULL) {
			HtmxA *link = htmx_a_new_with_href(sorts[i].href);

			th = htmx_th_new();
			htmx_node_set_text_content(HTMX_NODE(link), columns[i].heading);
			htmx_element_add_class(HTMX_ELEMENT(link), "sort-link");
			add_owned(HTMX_NODE(th), link);
			if (sorts[i].sorted != NULL)
				htmx_element_set_attribute(HTMX_ELEMENT(th), "aria-sort", sorts[i].sorted);
		} else {
			th = htmx_th_new_with_text(columns[i].heading);
		}

		htmx_node_set_void_element(col, TRUE);
		htmx_element_add_class(HTMX_ELEMENT(col), roles[role].css);
		add_owned(group, col);

		htmx_element_add_class(HTMX_ELEMENT(th), roles[role].css);
		htmx_element_set_attribute(HTMX_ELEMENT(th), "scope", "col");
		/* A heading with no words still has a name for a screen reader
		 * (and no data-label on a phone, which reads the text). */
		if (columns[i].heading[0] == '\0')
			htmx_element_set_attribute(HTMX_ELEMENT(th), "aria-label", role == AXIS_WEB_COL_STATE ?
				"State" : "Actions");
		add_owned(HTMX_NODE(row), th);

		fixed += roles[role].width;
		if (role == AXIS_WEB_COL_CONTROLS) {
			controls = TRUE;
		} else if (roles[role].width == 0) {
			shares++;
			phone_shared += role == AXIS_WEB_COL_WIDE ? PHONE_WIDE_REM : PHONE_NAME_REM;
		}
	}

	/* The least width that keeps every fixed column and some room for each
	 * name, rounded up to a step the stylesheet has. */
	least = (fixed + 99) / 100 + (controls ? CONTROLS_MIN_REM : 0);
	phone = least + phone_shared;
	if (shares > 0)
		least += SHARED_MIN_REM + (shares - 1) * SHARED_MORE_REM;
	least = (least + MIN_STEP_REM - 1) / MIN_STEP_REM * MIN_STEP_REM;
	least = CLAMP(least, MIN_FIRST_REM, MIN_LAST_REM);
	min_class = g_strdup_printf("min-%u", least);
	htmx_element_add_class(HTMX_ELEMENT(table), min_class);
	/* On a phone, more for the names and details when that is more. */
	phone = (phone + MIN_STEP_REM - 1) / MIN_STEP_REM * MIN_STEP_REM;
	phone = CLAMP(phone, MIN_FIRST_REM, MIN_LAST_REM);
	if (phone > least) {
		g_autofree gchar *phone_class = g_strdup_printf("least-%u", phone);

		htmx_element_add_class(HTMX_ELEMENT(table), phone_class);
	}

	g_object_set_data(G_OBJECT(row), ROW_DONE_KEY, GINT_TO_POINTER(1));
	add_owned(HTMX_NODE(table), group);
	add_owned(HTMX_NODE(head), row);
	add_owned(HTMX_NODE(table), head);

	return HTMX_NODE(table);
}

/* ── Giving cells their roles ───────────────────────────────────── */

static gboolean
is_tag(
	HtmxNode    *node,
	const gchar *tag
){
	const gchar *name = htmx_node_get_tag_name(node);

	return name != NULL && g_ascii_strcasecmp(name, tag) == 0;
}

static gboolean
has_class(
	HtmxNode    *node,
	const gchar *css
){
	GList *classes = htmx_element_get_classes(HTMX_ELEMENT(node));
	gboolean found = FALSE;
	GList *l;

	for (l = classes; l != NULL && !found; l = l->next)
		found = strcmp(l->data, css) == 0;
	g_list_free_full(classes, g_free);

	return found;
}

/* How many columns a cell spans (its colspan, 1 when it has none). */
static guint
cell_span(HtmxNode *cell)
{
	GList *attributes = htmx_element_get_attributes(HTMX_ELEMENT(cell));
	guint span = 1;
	GList *l;

	for (l = attributes; l != NULL; l = l->next) {
		HtmxAttribute *attribute = l->data;

		if (g_strcmp0(htmx_attribute_get_name(attribute), "colspan") == 0) {
			guint64 value = g_ascii_strtoull(htmx_attribute_get_value(attribute), NULL, 10);

			span = (guint)CLAMP(value, 1, 1000);
		}
	}
	g_list_free_full(attributes, (GDestroyNotify)htmx_attribute_free);

	return span;
}

static gboolean
has_attribute(
	HtmxNode    *node,
	const gchar *name
){
	GList *attributes = htmx_element_get_attributes(HTMX_ELEMENT(node));
	gboolean found = FALSE;
	GList *l;

	for (l = attributes; l != NULL && !found; l = l->next)
		found = g_strcmp0(htmx_attribute_get_name(l->data), name) == 0;
	g_list_free_full(attributes, (GDestroyNotify)htmx_attribute_free);

	return found;
}

/*
 * The roles whose cells are cut (with an ellipsis) when their value is
 * longer than the column: the fewest characters a value of @css may be cut
 * at, or 0 for a role that is never cut.  A character of a figure is at
 * most 0.7em wide in the widest face measured, at the tables' 15 px; a
 * value shorter than the column's content box holds at that width always
 * fits, and is not given a title.
 */
static guint
cut_from(const gchar *css)
{
	static const AxisWebColumnRole cut[] = {
		AXIS_WEB_COL_DATE, AXIS_WEB_COL_COUNT, AXIS_WEB_COL_NUMBER, AXIS_WEB_COL_SIZE
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cut); i++) {
		if (strcmp(css, roles[cut[i]].css) == 0) {
			/* hundredths of a rem -> px, less the padding (24 px), over
			 * 10.5 px a character */
			guint room = roles[cut[i]].width * 16 / 100 - 24;

			return room * 10 / 105 + 1;
		}
	}
	return 0;
}

/* The text a cell shows when it is plain: its own text, or the text of its
 * one child (a <time>, a <span>); %NULL otherwise. */
static const gchar *
plain_text(HtmxNode *cell)
{
	GList *children = htmx_node_get_children(cell);
	const gchar *text = htmx_node_get_text_content(cell);

	if (text != NULL && text[0] != '\0')
		return text;
	if (children == NULL || children->next != NULL)
		return NULL;
	text = htmx_node_get_text_content(children->data);
	return text != NULL && text[0] != '\0' && htmx_node_get_children(children->data) == NULL ? text : NULL;
}

/* The role classes of a table's columns, from its <colgroup>, or %NULL
 * when it is not one of ours. */
static GPtrArray *
table_roles(HtmxNode *table)
{
	GList *l;

	for (l = htmx_node_get_children(table); l != NULL; l = l->next) {
		HtmxNode *child = l->data;
		GPtrArray *found;
		GList *c;

		if (!is_tag(child, "colgroup"))
			continue;

		found = g_ptr_array_new_with_free_func(g_free);
		for (c = htmx_node_get_children(child); c != NULL; c = c->next) {
			GList *classes = htmx_element_get_classes(HTMX_ELEMENT(c->data));

			g_ptr_array_add(found, g_strdup(classes != NULL ? classes->data : ""));
			g_list_free_full(classes, g_free);
		}
		return found;
	}

	return NULL;
}

/* The text of the headings of @table's heading row, by column ("" for a
 * heading that has none, or a cell across several columns); %NULL when it
 * has no heading row. */
static GPtrArray *
table_headings(HtmxNode *table)
{
	GList *l;

	for (l = htmx_node_get_children(table); l != NULL; l = l->next) {
		GList *r;

		if (!is_tag(l->data, "thead"))
			continue;
		for (r = htmx_node_get_children(l->data); r != NULL; r = r->next) {
			GPtrArray *found;
			GList *c;

			if (!is_tag(r->data, "tr"))
				continue;
			found = g_ptr_array_new_with_free_func(g_free);
			for (c = htmx_node_get_children(r->data); c != NULL; c = c->next) {
				const gchar *text;
				guint span;
				guint k;

				if (!is_tag(c->data, "th") && !is_tag(c->data, "td"))
					continue;
				text = plain_text(c->data);
				span = cell_span(c->data);
				for (k = 0; k < span; k++)
					g_ptr_array_add(found, g_strdup(span == 1 && text != NULL ? text : ""));
			}
			return found;
		}
	}
	return NULL;
}

/*
 * Gives the cells of @row the classes in @classes, by column, and each body
 * cell its column's heading in @headings (nullable) as "data-label": on a
 * phone, where a table's rows are stacked, a cell shows it before its value
 * (axis.css).
 */
static void
row_apply(
	HtmxNode           *row,
	const gchar *const *classes,
	guint               n,
	GPtrArray          *headings
){
	guint column = 0;
	GList *l;

	if (g_object_get_data(G_OBJECT(row), ROW_DONE_KEY) != NULL)
		return;
	g_object_set_data(G_OBJECT(row), ROW_DONE_KEY, GINT_TO_POINTER(1));

	for (l = htmx_node_get_children(row); l != NULL && column < n; l = l->next) {
		HtmxNode *cell = l->data;
		guint span;

		if (!is_tag(cell, "td") && !is_tag(cell, "th"))
			continue;
		span = cell_span(cell);
		/* A cell across several columns has no one role. */
		if (span == 1 && classes[column][0] != '\0' && !has_class(cell, classes[column]))
			htmx_element_add_class(HTMX_ELEMENT(cell), classes[column]);
		/* A value that may be cut is there whole, for a pointer. */
		if (span == 1 && is_tag(cell, "td") && cut_from(classes[column]) > 0) {
			const gchar *text = plain_text(cell);

			if (text != NULL && g_utf8_validate(text, -1, NULL) &&
			    (guint)g_utf8_strlen(text, -1) >= cut_from(classes[column]) && !has_attribute(cell, "title"))
				htmx_element_set_attribute(HTMX_ELEMENT(cell), "title", text);
		}
		/* Its heading, for a stacked row. */
		if (span == 1 && is_tag(cell, "td") && headings != NULL && column < headings->len &&
		    ((const gchar *)g_ptr_array_index(headings, column))[0] != '\0' && !has_attribute(cell, "data-label"))
			htmx_element_set_attribute(HTMX_ELEMENT(cell), "data-label", g_ptr_array_index(headings, column));
		column += span;
	}
}

static void
apply_table(HtmxNode *table)
{
	g_autoptr(GPtrArray) classes = table_roles(table);
	g_autoptr(GPtrArray) headings = NULL;
	GList *l;

	if (classes == NULL)
		return;
	headings = table_headings(table);

	for (l = htmx_node_get_children(table); l != NULL; l = l->next) {
		HtmxNode *child = l->data;
		GList *r;

		if (is_tag(child, "tr")) {
			row_apply(child, (const gchar *const *)classes->pdata, classes->len, headings);
			continue;
		}
		if (!is_tag(child, "thead") && !is_tag(child, "tbody") && !is_tag(child, "tfoot"))
			continue;
		for (r = htmx_node_get_children(child); r != NULL; r = r->next) {
			if (is_tag(r->data, "tr"))
				row_apply(r->data, (const gchar *const *)classes->pdata, classes->len, headings);
		}
	}
}

void
axis_web_table_apply_roles(HtmxNode *root)
{
	GList *l;

	g_return_if_fail(HTMX_IS_NODE(root));

	if (is_tag(root, "table") && has_class(root, "data-table"))
		apply_table(root);

	for (l = htmx_node_get_children(root); l != NULL; l = l->next)
		axis_web_table_apply_roles(l->data);
}

void
axis_web_table_row_roles(
	HtmxNode            *row,
	const AxisWebColumn *columns
){
	g_autoptr(GPtrArray) classes = g_ptr_array_new();
	g_autoptr(GPtrArray) headings = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_return_if_fail(HTMX_IS_NODE(row));
	g_return_if_fail(columns != NULL);

	for (i = 0; columns[i].heading != NULL; i++) {
		g_ptr_array_add(classes, (gpointer)axis_web_column_role_class(columns[i].role));
		g_ptr_array_add(headings, g_strdup(columns[i].heading));
	}
	row_apply(row, (const gchar *const *)classes->pdata, classes->len, headings);
}

/* ── Dates ──────────────────────────────────────────────────────── */

gchar *
axis_web_format_time(
	gint64       us,
	const gchar *none
){
	g_autoptr(GDateTime) when = NULL;

	if (us <= 0)
		return g_strdup(none != NULL ? none : "");
	when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);
	if (when == NULL)
		return g_strdup(none != NULL ? none : "");

	return g_date_time_format(when, "%Y-%m-%d %H:%M:%S UTC");
}
