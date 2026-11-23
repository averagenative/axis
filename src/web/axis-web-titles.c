/*
 * axis-web-titles.c - a list of titles at scale: its three views, its filter
 * line, its count and its pages, and each account's choice of view
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-titles.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-table.h"

#include <string.h>

#include "core/axis-size.h"

static const gchar *const view_names[AXIS_WEB_VIEW_N] = { "posters", "overview", "table" };
static const gchar *const view_labels[AXIS_WEB_VIEW_N] = { "Posters", "List", "Table" };

/* Adds @child to @parent and drops the caller's reference. */
static void
add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

static HtmxNode *
text_node(
	gpointer     node,
	const gchar *css,
	const gchar *text
){
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(node), css);
	if (text != NULL)
		htmx_node_set_text_content(HTMX_NODE(node), text);
	return HTMX_NODE(node);
}

const gchar *
axis_web_view_name(AxisWebView view)
{
	return (guint)view < AXIS_WEB_VIEW_N ? view_names[view] : view_names[0];
}

gboolean
axis_web_view_from_name(
	const gchar *name,
	AxisWebView *view
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_WEB_VIEW_N; i++) {
		if (strcmp(view_names[i], name) == 0) {
			*view = (AxisWebView)i;
			return TRUE;
		}
	}
	return FALSE;
}

guint
axis_web_view_page_size(AxisWebView view)
{
	return view == AXIS_WEB_VIEW_TABLE ? 100 : 60;
}

void
axis_web_view_choose(
	AxisDbPool  *pool,
	gint64       user_id,
	const gchar *list,
	const gchar *asked,
	AxisWebView *view
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *user = g_strdup_printf("%" G_GINT64_FORMAT, user_id);
	AxisWebView wanted = AXIS_WEB_VIEW_POSTERS;
	AxisWebView kept = AXIS_WEB_VIEW_POSTERS;
	gboolean has_kept = FALSE;
	const gchar *params[3];

	*view = AXIS_WEB_VIEW_POSTERS;
	if (asked != NULL && axis_web_view_from_name(asked, &wanted))
		*view = wanted;
	if (pool == NULL || user_id <= 0)
		return;
	params[0] = user;
	params[1] = list;
	if (!axis_db_pool_exec(pool, "SELECT view FROM user_list_view WHERE user_id = $1::bigint AND list = $2", 2,
	        params, &result, &error)) {
		g_debug("lists: the view of %s could not be read: %s", list, error->message);
		return;
	}
	if (axis_db_result_get_n_rows(result) > 0)
		has_kept = axis_web_view_from_name(axis_db_result_get_text(result, 0, 0), &kept);
	if (asked == NULL || !axis_web_view_from_name(asked, &wanted)) {
		*view = has_kept ? kept : AXIS_WEB_VIEW_POSTERS;
		return;
	}
	/* Chosen now: kept, unless it is kept already. */
	if (has_kept && kept == wanted)
		return;
	params[2] = axis_web_view_name(wanted);
	if (!axis_db_pool_exec(pool, "INSERT INTO user_list_view (user_id, list, view) VALUES ($1::bigint, $2, $3)"
	        " ON CONFLICT (user_id, list) DO UPDATE SET view = EXCLUDED.view, updated_at = now()", 3, params, NULL,
	        &error))
		g_debug("lists: the view of %s could not be kept: %s", list, error->message);
}

/* ── The rows ───────────────────────────────────────────────────── */

void
axis_web_title_row_clear(AxisWebTitleRow *row)
{
	if (row == NULL)
		return;
	g_clear_object(&row->mark);
	g_clear_object(&row->extra);
}

/* The parts of a meta title a poster is drawn from. */
static void
title_of(
	const AxisWebTitleRow *row,
	AxisMetaTitle         *title
){
	memset(title, 0, sizeof(*title));
	title->kind = row->kind;
	title->title = (gchar *)row->title;
	title->year = row->year;
	title->provider = (gchar *)row->provider;
	title->poster_ref = (gchar *)row->poster_ref;
	title->overview = (gchar *)row->reason;
}

/* The row's mark, taken; for a row that needs attention and says where it is
 * put right, a link there that says why. */
static HtmxNode *
take_mark(AxisWebTitleRow *row)
{
	HtmxA *link;

	if (row->mark == NULL)
		return NULL;
	if (!row->attention || row->reason_href == NULL)
		return g_steal_pointer(&row->mark);
	link = htmx_a_new_with_href(row->reason_href);
	htmx_element_add_class(HTMX_ELEMENT(link), "mark-link");
	if (row->reason != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(link), "title", row->reason);
	add(link, g_steal_pointer(&row->mark));
	return HTMX_NODE(link);
}

static HtmxNode *
posters_new(
	AxisWebTitleRow *rows,
	guint            n_rows
){
	HtmxDiv *grid = htmx_div_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(grid), "title-grid");
	htmx_element_add_class(HTMX_ELEMENT(grid), "film-grid");
	for (i = 0; i < n_rows; i++) {
		AxisWebTitleRow *row = &rows[i];
		HtmxDiv *corner = NULL;
		AxisMetaTitle title;

		title_of(row, &title);
		if (row->mark != NULL) {
			corner = htmx_div_new();
			htmx_element_add_class(HTMX_ELEMENT(corner), "card-actions");
			htmx_element_add_class(HTMX_ELEMENT(corner), "film-corner");
			add(corner, take_mark(row));
		}
		/* The card takes the corner and the extra. */
		add(grid, axis_web_meta_card_new_full(&title, row->poster, corner != NULL ? HTMX_NODE(corner) : NULL,
			FALSE, row->href, row->line, g_steal_pointer(&row->extra)));
	}
	return HTMX_NODE(grid);
}

/* One dense line: a small poster, the title and year, the facts, the state,
 * and the reason when it needs attention. */
static HtmxNode *
overview_new(
	AxisWebTitleRow *rows,
	guint            n_rows
){
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "title-rows");
	for (i = 0; i < n_rows; i++) {
		AxisWebTitleRow *row = &rows[i];
		HtmxLi *item = htmx_li_new();
		HtmxA *thumb = htmx_a_new_with_href(row->href);
		HtmxDiv *main = htmx_div_new();
		HtmxP *head = htmx_p_new();
		HtmxA *link = htmx_a_new_with_href(row->href);
		HtmxDiv *side = htmx_div_new();
		AxisMetaTitle title;

		title_of(row, &title);
		htmx_element_add_class(HTMX_ELEMENT(item), "title-row");
		/* The poster repeats the link beside it: one stop for a keyboard. */
		htmx_element_add_class(HTMX_ELEMENT(thumb), "title-row-thumb");
		htmx_element_set_attribute(HTMX_ELEMENT(thumb), "tabindex", "-1");
		htmx_element_set_attribute(HTMX_ELEMENT(thumb), "aria-hidden", "true");
		add(thumb, axis_web_meta_poster_new(&title, row->poster, FALSE, 0));
		add(item, thumb);

		htmx_element_add_class(HTMX_ELEMENT(main), "title-row-main");
		htmx_element_add_class(HTMX_ELEMENT(head), "title-row-head");
		htmx_element_add_class(HTMX_ELEMENT(link), "title-row-name");
		htmx_node_set_text_content(HTMX_NODE(link), row->title);
		add(head, link);
		if (row->year > 0) {
			g_autofree gchar *year = g_strdup_printf("%d", row->year);

			add(head, text_node(htmx_span_new(), "title-row-year", year));
		}
		add(main, head);
		if (row->facts != NULL && row->facts[0] != '\0')
			add(main, text_node(htmx_p_new(), "title-row-facts", row->facts));
		if (row->reason != NULL && row->reason[0] != '\0' && row->attention && row->reason_href != NULL) {
			/* What is wrong, a link to where it is put right. */
			HtmxP *line = htmx_p_new();
			HtmxA *why = htmx_a_new_with_href(row->reason_href);

			htmx_element_add_class(HTMX_ELEMENT(line), "title-row-reason");
			htmx_element_add_class(HTMX_ELEMENT(line), "title-row-attention");
			htmx_node_set_text_content(HTMX_NODE(why), row->reason);
			add(line, why);
			add(main, line);
		} else if (row->reason != NULL && row->reason[0] != '\0') {
			add(main, text_node(htmx_p_new(), row->attention ? "title-row-reason title-row-attention" :
				"title-row-reason", row->reason));
		}
		add(item, main);

		htmx_element_add_class(HTMX_ELEMENT(side), "title-row-mark");
		if (row->mark != NULL)
			add(side, take_mark(row));
		add(item, side);
		add(list, item);
	}
	return HTMX_NODE(list);
}

static HtmxNode *
table_new(
	const gchar     *noun,
	const gchar     *detail,
	AxisWebTitleRow *rows,
	guint            n_rows
){
	const AxisWebColumn columns[] = {
		{ noun, AXIS_WEB_COL_NAME },
		{ "Year", AXIS_WEB_COL_COUNT },
		{ "State", AXIS_WEB_COL_STATE },
		{ detail, AXIS_WEB_COL_WIDE },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Added", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("titles", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < n_rows; i++) {
		AxisWebTitleRow *row = &rows[i];
		HtmxTr *tr = htmx_tr_new();
		HtmxTd *cell = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(row->href);
		HtmxBdi *name = htmx_bdi_new();
		g_autofree gchar *year = row->year > 0 ? g_strdup_printf("%d", row->year) : g_strdup("");
		g_autofree gchar *size = row->size >= 0 ? axis_size_format((guint64)row->size) : g_strdup("");
		g_autofree gchar *added = axis_web_format_time(row->added_us, "");

		htmx_node_set_text_content(HTMX_NODE(name), row->title);
		add(link, name);
		add(cell, link);
		add(tr, cell);
		add(tr, htmx_td_new_with_text(year));
		cell = htmx_td_new();
		if (row->mark != NULL)
			add(cell, take_mark(row));
		else if (row->state != NULL)
			htmx_node_set_text_content(HTMX_NODE(cell), row->state);
		add(tr, cell);
		add(tr, htmx_td_new_with_text(row->quality != NULL ? row->quality : ""));
		add(tr, htmx_td_new_with_text(size));
		add(tr, htmx_td_new_with_text(added));
		add(body, tr);
	}
	add(table, body);
	{
		HtmxDiv *card = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(card), "card");
		htmx_element_add_class(HTMX_ELEMENT(card), "title-table");
		add(card, table);
		return HTMX_NODE(card);
	}
}

HtmxNode *
axis_web_titles_new(
	AxisWebView      view,
	const gchar     *noun,
	const gchar     *detail,
	AxisWebTitleRow *rows,
	guint            n_rows
){
	switch (view) {
	case AXIS_WEB_VIEW_OVERVIEW:
		return overview_new(rows, n_rows);
	case AXIS_WEB_VIEW_TABLE:
		return table_new(noun, detail, rows, n_rows);
	default:
		return posters_new(rows, n_rows);
	}
}

/* ── The address ────────────────────────────────────────────────── */

static void
query_add(
	GString     *query,
	const gchar *name,
	const gchar *value
){
	g_autofree gchar *escaped = NULL;

	if (value == NULL || value[0] == '\0')
		return;
	escaped = g_uri_escape_string(value, NULL, FALSE);
	g_string_append_printf(query, "%c%s=%s", query->len > 0 ? '&' : '?', name, escaped);
}

gchar *
axis_web_list_href(const AxisWebListAddress *address)
{
	g_autoptr(GString) query = g_string_new(NULL);

	query_add(query, "state", address->state);
	query_add(query, "q", address->name);
	if (address->sort != NULL && g_strcmp0(address->sort, address->default_sort) != 0)
		query_add(query, "sort", address->sort);
	query_add(query, "view", axis_web_view_name(address->view));
	query_add(query, "after", address->after);
	query_add(query, "before", address->before);
	return g_strconcat(address->path, query->str, NULL);
}

/* A link of a row of choices; the current one marked as a tab is. */
static HtmxNode *
choice_link(
	const AxisWebListAddress *address,
	const gchar              *label,
	gboolean                  current
){
	g_autofree gchar *href = axis_web_list_href(address);
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), label);
	if (current) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "true");
	}
	return HTMX_NODE(link);
}

static void
hidden(
	HtmxForm    *form,
	const gchar *name,
	const gchar *value
){
	HtmxInput *input;

	if (value == NULL || value[0] == '\0')
		return;
	input = htmx_input_new(HTMX_INPUT_HIDDEN);
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, value);
	add(form, input);
}

HtmxNode *
axis_web_list_bar_new(
	const AxisWebListAddress *address,
	const gchar              *what,
	const gchar *const       *sorts,
	const gchar *const       *sort_labels
){
	HtmxDiv *bar = htmx_div_new();
	HtmxForm *form = htmx_form_new();
	HtmxLabel *label = htmx_label_new_for("list-name");
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);
	HtmxButton *button = htmx_button_new_submit();
	HtmxDiv *choices = htmx_div_new();
	HtmxDiv *order = htmx_div_new();
	HtmxDiv *views = htmx_div_new();
	g_autofree gchar *label_text = g_strdup_printf("Filter %s by name", what);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(bar), "list-bar");

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", address->path);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "role", "search");
	htmx_element_add_class(HTMX_ELEMENT(form), "list-filter");
	htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	add(form, label);
	htmx_element_set_id(HTMX_ELEMENT(input), "list-name");
	htmx_input_set_name(input, "q");
	htmx_input_set_placeholder(input, "Filter by name");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "100");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	if (address->name != NULL)
		htmx_input_set_value(input, address->name);
	add(form, input);
	/* The state, the order and the view stay as they are; a new name starts
	 * again from the first page. */
	hidden(form, "state", address->state);
	if (address->sort != NULL && g_strcmp0(address->sort, address->default_sort) != 0)
		hidden(form, "sort", address->sort);
	hidden(form, "view", axis_web_view_name(address->view));
	htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	htmx_node_set_text_content(HTMX_NODE(button), "Filter");
	add(form, button);
	if (address->name != NULL && address->name[0] != '\0') {
		AxisWebListAddress all = *address;

		all.name = NULL;
		all.after = all.before = NULL;
		add(form, choice_link(&all, "Clear the name filter", FALSE));
	}
	add(bar, form);

	htmx_element_add_class(HTMX_ELEMENT(choices), "list-choices");
	htmx_element_add_class(HTMX_ELEMENT(order), "list-links");
	htmx_element_set_attribute(HTMX_ELEMENT(order), "role", "group");
	htmx_element_set_attribute(HTMX_ELEMENT(order), "aria-label", "Sort by");
	add(order, text_node(htmx_span_new(), "list-links-label", "Sort"));
	for (i = 0; sorts[i] != NULL && sort_labels[i] != NULL; i++) {
		AxisWebListAddress sorted = *address;
		const gchar *current = address->sort != NULL ? address->sort : address->default_sort;

		sorted.sort = sorts[i];
		sorted.after = sorted.before = NULL;
		add(order, choice_link(&sorted, sort_labels[i], g_strcmp0(current, sorts[i]) == 0));
	}
	add(choices, order);

	htmx_element_add_class(HTMX_ELEMENT(views), "list-links");
	htmx_element_set_attribute(HTMX_ELEMENT(views), "role", "group");
	htmx_element_set_attribute(HTMX_ELEMENT(views), "aria-label", "View");
	add(views, text_node(htmx_span_new(), "list-links-label", "View"));
	for (i = 0; i < AXIS_WEB_VIEW_N; i++) {
		AxisWebListAddress viewed = *address;

		/* The same titles, so the same page. */
		viewed.view = (AxisWebView)i;
		add(views, choice_link(&viewed, view_labels[i], address->view == (AxisWebView)i));
	}
	add(choices, views);
	add(bar, choices);
	return HTMX_NODE(bar);
}

HtmxNode *
axis_web_list_tab_new(
	const AxisWebListAddress *address,
	const gchar              *state,
	const gchar              *label,
	guint                     count
){
	AxisWebListAddress tab = *address;
	HtmxLi *item = htmx_li_new();
	g_autofree gchar *href = NULL;
	g_autofree gchar *number = axis_web_format_count(count);
	HtmxA *link;

	tab.state = state;
	tab.after = tab.before = NULL;
	href = axis_web_list_href(&tab);
	link = htmx_a_new_with_href(href);
	if (g_strcmp0(state, address->state) == 0) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "page");
	}
	add(link, text_node(htmx_span_new(), NULL, label));
	add(link, text_node(htmx_span_new(), "tab-count", number));
	add(item, link);
	return HTMX_NODE(item);
}

HtmxNode *
axis_web_list_foot_new(
	const AxisWebListAddress *address,
	guint                     matching,
	guint                     shown,
	const gchar              *noun,
	const gchar              *nouns,
	const gchar              *next,
	const gchar              *previous
){
	HtmxDiv *foot = htmx_div_new();
	g_autofree gchar *number = axis_web_format_count(matching);
	g_autofree gchar *count = NULL;

	htmx_element_add_class(HTMX_ELEMENT(foot), "list-foot");
	if (matching > shown)
		count = g_strdup_printf("%u of %s %s on this page", shown, number, nouns);
	else
		count = g_strdup_printf("%s %s", number, matching == 1 ? noun : nouns);
	add(foot, text_node(htmx_p_new(), "list-count", count));
	if (next != NULL || previous != NULL) {
		HtmxNav *pager = htmx_nav_new();
		AxisWebListAddress page = *address;

		htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
		htmx_element_set_attribute(HTMX_ELEMENT(pager), "aria-label", "Pages");
		page.after = page.before = NULL;
		if (previous != NULL) {
			add(pager, choice_link(&page, "First page", FALSE));
			page.before = previous;
			add(pager, choice_link(&page, "Previous page", FALSE));
			page.before = NULL;
		}
		if (next != NULL) {
			page.after = next;
			add(pager, choice_link(&page, "Next page", FALSE));
		}
		add(foot, pager);
	}
	return HTMX_NODE(foot);
}

