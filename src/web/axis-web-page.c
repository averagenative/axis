/*
 * axis-web-page.c - the common page layout, and the error pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pages are built from element objects only.  Their text is escaped by
 * the elements; nothing here uses the raw-HTML calls, and the document
 * carries no script body, no style element and no style attribute (the
 * content security policy forbids all three).
 */

#include "web/axis-web-private.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-theme.h"

#include <string.h>

#include "axis-version.h"

#define APP_NAME "Axis"

/*
 * htmx does not swap 4xx and 5xx responses unless told to, and the error
 * pages for htmx requests are exactly such responses.  It also injects a
 * style element for its indicators unless told not to; the rule it would
 * inject is in axis.css instead.
 */
#define HTMX_CONFIG \
	"{\"includeIndicatorStyles\":false,\"responseHandling\":[" \
	"{\"code\":\"204\",\"swap\":false}," \
	"{\"code\":\"[23]..\",\"swap\":true}," \
	"{\"code\":\"[45]..\",\"swap\":true,\"error\":true}," \
	"{\"code\":\"...\",\"swap\":false}]}"

const gchar *
axis_web_request_theme_id(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	return axis_web_theme_resolve(state != NULL && state->user_id != 0 ? state->theme : NULL)->id;
}

/* Adds @child to @parent and drops the caller's reference to it. */
static void
add_owned(
	HtmxNode *parent,
	gpointer  child
){
	htmx_node_add_child(parent, HTMX_NODE(child));
	g_object_unref(child);
}

static gboolean
nav_path_is_safe(const gchar *path)
{
	return path != NULL && path[0] == '/' && path[1] != '/' && strchr(path, '\\') == NULL;
}

static gboolean
nav_is_current(
	const gchar *entry_path,
	const gchar *request_path
){
	gsize length;

	if (request_path == NULL)
		return FALSE;
	if (strcmp(entry_path, request_path) == 0)
		return TRUE;
	if (strcmp(entry_path, "/") == 0)
		return FALSE;

	/* A page below an entry belongs to its section. */
	length = strlen(entry_path);
	return strncmp(request_path, entry_path, length) == 0 && request_path[length] == '/';
}

/*
 * The primary tabs after the dashboard, in the order the shell fixes: the
 * media, then what is happening, then looking after the collection, then
 * the installation.  A tab is a group of pages, not a page: there are five
 * whatever the number of modules (every media type is a section of Media),
 * so the top row stays one line.
 */
static const struct
{
	AxisNavGroup  group;
	const gchar  *label;
} primary_tabs[] = {
	{ AXIS_NAV_GROUP_MEDIA, "Media" },
	{ AXIS_NAV_GROUP_ACTIVITY, "Activity" },
	{ AXIS_NAV_GROUP_MANAGEMENT, "Library" },
	{ AXIS_NAV_GROUP_SYSTEM, "System" }
};

/* The shell's own pages: the users page comes with the accounts. */
static const AxisNavEntry shell_entries[] = {
	{ "Users", "/admin/users", 20, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ "Reverse proxies", "/admin/proxies", 21, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

/*
 * The pages of @group that the user may see: every enabled module's and
 * the shell's own, in order, without any whose path is not a plain local
 * path.
 */
static GArray *
collect_group(
	AxisModuleRegistry *registry,
	AxisNavGroup        group,
	gboolean            admin
){
	GArray *entries = registry != NULL ? axis_module_registry_collect_nav(registry, group, admin) :
		g_array_new(FALSE, FALSE, sizeof(AxisNavEntry));
	const AxisNavEntry *own;
	guint i;

	for (own = shell_entries; own->label != NULL; own++) {
		if (own->group == group && (admin || !own->admin_only))
			g_array_append_val(entries, *own);
	}

	for (i = entries->len; i > 0; i--) {
		const AxisNavEntry *entry = &g_array_index(entries, AxisNavEntry, i - 1);

		if (!nav_path_is_safe(entry->path)) {
			g_warning("ignoring navigation entry '%s' with path '%s'", entry->label,
				entry->path != NULL ? entry->path : "(null)");
			g_array_remove_index(entries, i - 1);
		}
	}

	g_array_sort(entries, axis_nav_entry_compare);
	return entries;
}

/*
 * A link of a navigation list.  @current is "page" for the page that is
 * open, "true" for the section it is in (a tab, or a page below the link),
 * or %NULL; either way the link carries the class that marks it.
 */
static HtmxNode *
nav_item(
	const gchar *label,
	const gchar *path,
	const gchar *current
){
	HtmxLi *item = htmx_li_new();
	HtmxA *link = htmx_a_new_with_href(path);

	htmx_node_set_text_content(HTMX_NODE(link), label);
	if (current != NULL) {
		htmx_element_add_class(HTMX_ELEMENT(link), "current");
		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", current);
	}
	add_owned(HTMX_NODE(item), link);

	return HTMX_NODE(item);
}

static const gchar *
page_current(
	const gchar *entry_path,
	const gchar *request_path
){
	if (!nav_is_current(entry_path, request_path))
		return NULL;
	return strcmp(entry_path, request_path) == 0 ? "page" : "true";
}

/*
 * The logged-in user's corner: their name (the account page) and a logout
 * button (a POST form: logging out changes state, so it is never a link).
 * The pages of the installation are in the System tab.
 */
static HtmxNode *
build_user_menu(
	HtmxRequest         *request,
	const AxisAuthState *state,
	const gchar         *request_path
){
	HtmxDiv *menu = htmx_div_new();
	HtmxA *name = htmx_a_new_with_href("/account");
	HtmxNode *logout = axis_web_auth_post_form(request, "/logout");

	htmx_element_add_class(HTMX_ELEMENT(menu), "user-menu");
	htmx_element_add_class(HTMX_ELEMENT(name), "user-name");
	htmx_node_set_text_content(HTMX_NODE(name), state->username);
	{
		g_autofree gchar *account_label =
			g_strdup_printf("Your account: %s", state->username);

		htmx_element_set_attribute(HTMX_ELEMENT(name), "aria-label", account_label);
	}
	if (request_path != NULL && strcmp(request_path, "/account") == 0)
		htmx_element_set_attribute(HTMX_ELEMENT(name), "aria-current", "page");
	add_owned(HTMX_NODE(menu), name);

	htmx_element_add_class(HTMX_ELEMENT(logout), "inline-form");
	add_owned(HTMX_NODE(logout), axis_web_auth_button("Log out", "button-quiet"));
	add_owned(HTMX_NODE(menu), logout);

	return HTMX_NODE(menu);
}

/* A row below the tabs: an empty list of links in its bar, labelled. */
static HtmxNode *
row_new(
	const gchar  *label,
	const gchar  *css,
	HtmxNode    **list_out
){
	HtmxNav *nav = htmx_nav_new();
	HtmxDiv *bar = htmx_div_new();
	HtmxUl *list = htmx_ul_new();

	htmx_element_add_class(HTMX_ELEMENT(nav), "page-nav");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(nav), css);
	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", label);
	htmx_element_add_class(HTMX_ELEMENT(bar), "bar");
	htmx_element_add_class(HTMX_ELEMENT(list), "nav-list");
	htmx_element_add_class(HTMX_ELEMENT(list), "page-list");
	*list_out = HTMX_NODE(list);
	add_owned(HTMX_NODE(bar), list);
	add_owned(HTMX_NODE(nav), bar);
	return HTMX_NODE(nav);
}

/* An entry's media type: its section, or its own name. */
static const gchar *
entry_section(const AxisNavEntry *entry)
{
	return entry->section != NULL ? entry->section : entry->label;
}

/*
 * A row of pages: those of @entries (all of them, or only those of
 * @section when it is set); @open_path is the page the request is at or
 * below (the longest that matches), the one marked.  Where the entries'
 * run changes, the first of the new run is set apart by a thin divider.
 */
static HtmxNode *
build_page_row(
	const gchar *row_label,
	GArray      *entries,
	const gchar *section,
	const gchar *css,
	const gchar *open_path,
	const gchar *request_path
){
	g_autofree gchar *label = g_strdup_printf("%s pages", row_label);
	const AxisNavEntry *previous = NULL;
	HtmxNode *list;
	HtmxNode *nav = row_new(label, css, &list);
	guint i;

	for (i = 0; i < entries->len; i++) {
		const AxisNavEntry *entry = &g_array_index(entries, AxisNavEntry, i);
		HtmxNode *item;

		if (section != NULL && strcmp(entry_section(entry), section) != 0)
			continue;
		item = nav_item(entry->label, entry->path,
			g_strcmp0(entry->path, open_path) == 0 ? page_current(entry->path, request_path) : NULL);
		if (previous != NULL && previous->run != entry->run)
			htmx_element_add_class(HTMX_ELEMENT(item), "nav-run-start");
		add_owned(list, item);
		previous = entry;
	}
	return nav;
}

/*
 * Media's second row: its media types, in the order of their first pages,
 * each linking to its first page; the open one marked.  Returns the open
 * type's name in @open_section (%NULL when none is open) and how many pages
 * it has in @open_pages.
 */
static HtmxNode *
build_section_row(
	GArray       *entries,
	const gchar  *open_path,
	const gchar  *request_path,
	const gchar **open_section,
	guint        *open_pages
){
	HtmxNode *list;
	HtmxNode *nav = row_new("Media sections","section-nav", &list);
	g_autoptr(GPtrArray) seen = g_ptr_array_new();
	guint i;
	guint k;

	*open_section = NULL;
	*open_pages = 0;
	for (i = 0; i < entries->len; i++) {
		const AxisNavEntry *entry = &g_array_index(entries, AxisNavEntry, i);

		if (g_strcmp0(entry->path, open_path) == 0)
			*open_section = entry_section(entry);
	}
	for (i = 0; i < entries->len; i++) {
		const AxisNavEntry *entry = &g_array_index(entries, AxisNavEntry, i);
		const gchar *section = entry_section(entry);
		const gchar *current = NULL;
		guint pages = 0;

		if (g_ptr_array_find_with_equal_func(seen, section, g_str_equal, NULL))
			continue;
		g_ptr_array_add(seen, (gpointer)section);
		for (k = 0; k < entries->len; k++)
			pages += strcmp(entry_section(&g_array_index(entries, AxisNavEntry, k)), section) == 0;
		if (g_strcmp0(section, *open_section) == 0) {
			*open_pages = pages;
			/* A type of one page is that page. */
			current = pages == 1 ? page_current(open_path, request_path) : "true";
		}
		add_owned(list, nav_item(section, entry->path, current));
	}
	return nav;
}

/* The ids of the parts of the header that follow the page that is open. */
#define TABS_ID "primary-nav"
#define ROWS_ID "nav-rows"

/*
 * The navigation for @request_path, as @state's user sees it: in @tabs_out
 * the primary tabs (nav#primary-nav), in @rows_out the rows below the top
 * row (div#nav-rows, empty when no tab is open or the open one has a single
 * page).  The Dashboard is marked on the dashboard only: a page in no tab
 * (the search results, a title's page) marks none.
 */
static void
build_nav(
	AxisWeb              *web,
	const AxisAuthState  *state,
	const gchar          *request_path,
	HtmxNode            **tabs_out,
	HtmxNode            **rows_out
){
	AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;
	gboolean admin = state->role == AXIS_USER_ROLE_ADMIN;
	GArray *groups[G_N_ELEMENTS(primary_tabs)];
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *list = htmx_ul_new();
	HtmxDiv *rows = htmx_div_new();
	gint open_tab = -1;
	const gchar *open_path = NULL;
	gsize best = 0;
	guint t;
	guint i;

	/* The open tab is the one holding the page whose path is the longest
	 * that the request is at or below. */
	for (t = 0; t < G_N_ELEMENTS(primary_tabs); t++) {
		groups[t] = collect_group(registry, primary_tabs[t].group, admin);
		for (i = 0; i < groups[t]->len; i++) {
			const AxisNavEntry *entry = &g_array_index(groups[t], AxisNavEntry, i);
			gsize length = strlen(entry->path);

			if (nav_is_current(entry->path, request_path) && length > best) {
				best = length;
				open_tab = (gint)t;
				open_path = entry->path;
			}
		}
	}

	htmx_element_set_id(HTMX_ELEMENT(nav), TABS_ID);
	htmx_element_add_class(HTMX_ELEMENT(nav), "primary-nav");
	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Main");
	htmx_element_add_class(HTMX_ELEMENT(list), "nav-list");
	htmx_element_add_class(HTMX_ELEMENT(list), "tab-list");

	add_owned(HTMX_NODE(list), nav_item("Dashboard", "/", page_current("/", request_path)));
	for (t = 0; t < G_N_ELEMENTS(primary_tabs); t++) {
		const AxisNavEntry *first;
		const gchar *current = NULL;

		/* A tab with no page this user may see is not shown. */
		if (groups[t]->len == 0)
			continue;
		/* A tab links to its first page (Media: its first type's). */
		first = &g_array_index(groups[t], AxisNavEntry, 0);
		if ((gint)t == open_tab)
			current = request_path != NULL && strcmp(first->path, request_path) == 0 &&
				groups[t]->len == 1 ? "page" : "true";
		add_owned(HTMX_NODE(list), nav_item(primary_tabs[t].label, first->path, current));
	}
	add_owned(HTMX_NODE(nav), list);

	/* Media: its types, then the open type's pages when it has more than
	 * one.  Any other tab: its pages, when it has more than one. */
	htmx_element_set_id(HTMX_ELEMENT(rows), ROWS_ID);
	if (open_tab >= 0 && primary_tabs[open_tab].group == AXIS_NAV_GROUP_MEDIA) {
		const gchar *section = NULL;
		guint pages = 0;

		add_owned(HTMX_NODE(rows), build_section_row(groups[open_tab], open_path, request_path, &section,
			&pages));
		if (section != NULL && pages > 1)
			add_owned(HTMX_NODE(rows), build_page_row(section, groups[open_tab], section, "type-nav",
				open_path, request_path));
	} else if (open_tab >= 0 && groups[open_tab]->len > 1) {
		add_owned(HTMX_NODE(rows), build_page_row(primary_tabs[open_tab].label, groups[open_tab], NULL,
			NULL, open_path, request_path));
	}

	for (t = 0; t < G_N_ELEMENTS(primary_tabs); t++)
		g_array_unref(groups[t]);
	*tabs_out = HTMX_NODE(nav);
	*rows_out = HTMX_NODE(rows);
}

/*
 * The header has two rows, three inside Media.  The top row: the brand, the
 * primary tabs, a place kept for the title search, the bell of what needs
 * attention, the running-jobs indicator and the user's corner; it is one
 * line at desktop widths.  The
 * second row, only when the open tab has more than one page: that tab's
 * pages; in Media, the media types (Movies, TV), and a third row with the
 * open type's pages.  All are rendered here for the page that is open;
 * nothing opens or closes, so no script is involved.
 */
static HtmxNode *
build_header(
	AxisWeb     *web,
	HtmxRequest *request,
	const gchar *request_path
){
	HtmxHeader *header = htmx_header_new();
	HtmxDiv *bar = htmx_div_new();
	HtmxA *brand = htmx_a_new_with_href("/");
	HtmxSpan *brand_name = htmx_span_new();
	const AxisAuthState *state = axis_web_auth_state(request);

	htmx_node_set_text_content(HTMX_NODE(brand_name), APP_NAME);
	htmx_element_add_class(HTMX_ELEMENT(header), "site-header");
	htmx_element_add_class(HTMX_ELEMENT(bar), "bar");
	htmx_element_add_class(HTMX_ELEMENT(bar), "top-bar");
	htmx_element_add_class(HTMX_ELEMENT(brand), "brand");
	htmx_element_set_attribute(HTMX_ELEMENT(brand), "aria-label", "Axis dashboard");
	htmx_element_add_class(HTMX_ELEMENT(brand_name), "brand-name");
	add_owned(HTMX_NODE(brand), axis_web_logo_new("axis-hub-header", NULL));
	add_owned(HTMX_NODE(brand), brand_name);
	add_owned(HTMX_NODE(bar), brand);

	/* Someone not logged in is shown nothing of what is inside. */
	if (state != NULL && state->user_id != 0) {
		HtmxDiv *search = htmx_div_new();
		HtmxNode *widget;
		HtmxNode *tabs;
		HtmxNode *rows;

		build_nav(web, state, request_path, &tabs, &rows);
		add_owned(HTMX_NODE(bar), tabs);

		/* The title search's box, when the module that serves it is on; the
		 * place is kept either way, so that the row does not change shape. */
		htmx_element_add_class(HTMX_ELEMENT(search), "search-slot");
		widget = axis_web_build_search_box(web, request);
		if (widget != NULL)
			add_owned(HTMX_NODE(search), widget);
		add_owned(HTMX_NODE(bar), search);

		/* What needs a person, to the right of the box: a link with a count,
		 * drawn from memory, for a user who can have a notice. */
		widget = axis_web_attention_bell_new(web, request, request_path);
		if (widget != NULL)
			add_owned(HTMX_NODE(bar), widget);

		widget = axis_web_build_header_widget(web, request);
		if (widget != NULL)
			add_owned(HTMX_NODE(bar), widget);

		add_owned(HTMX_NODE(bar), build_user_menu(request, state, request_path));
		add_owned(HTMX_NODE(header), bar);
		add_owned(HTMX_NODE(header), rows);
	} else {
		add_owned(HTMX_NODE(header), bar);
	}

	return HTMX_NODE(header);
}

/* The colour pinned-tab icons are drawn in: a step of the default brand. */
#define MASK_ICON_COLOR "#b00000"

/* A <link rel=@rel href=asset(@asset)> with the given type and sizes. */
static void
add_icon_link(
	HtmxNode    *head,
	const gchar *rel,
	const gchar *asset,
	const gchar *type,
	const gchar *sizes,
	const gchar *color
){
	HtmxLink *link = htmx_link_new();
	g_autofree gchar *href = axis_web_asset_url(asset);

	htmx_link_set_rel(link, rel);
	htmx_link_set_href(link, href);
	if (type != NULL)
		htmx_link_set_type(link, type);
	if (sizes != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(link), "sizes", sizes);
	if (color != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(link), "color", color);
	add_owned(head, link);
}

static void
add_meta(
	HtmxNode    *head,
	const gchar *name,
	const gchar *content,
	const gchar *media
){
	HtmxMeta *meta = htmx_meta_new_name_content(name, content);

	if (media != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(meta), "media", media);
	add_owned(head, meta);
}

static HtmxNode *
build_head(
	const gchar        *page_title,
	const AxisWebTheme *theme
){
	HtmxHead *head = htmx_head_new();
	HtmxNode *head_node = HTMX_NODE(head);
	g_autofree gchar *css = axis_web_asset_url("axis.css");
	g_autofree gchar *themes_css = axis_web_asset_url("themes.css");
	g_autofree gchar *htmx_js = axis_web_asset_url("vendor/htmx.min.js");
	g_autofree gchar *sse_js = axis_web_asset_url("vendor/htmx-ext-sse.js");
	HtmxScript *htmx_script = htmx_script_new_with_src(htmx_js);
	HtmxScript *sse_script = htmx_script_new_with_src(sse_js);

	htmx_script_set_defer(htmx_script, TRUE);
	htmx_script_set_defer(sse_script, TRUE);

	add_owned(head_node, htmx_meta_new_charset("utf-8"));
	/* The page reaches the screen's edges on a phone with a notch or a
	 * rounded corner; axis.css keeps the bars inside the safe area. */
	add_owned(head_node,
		htmx_meta_new_name_content("viewport", "width=device-width, initial-scale=1, viewport-fit=cover"));
	add_meta(head_node, "color-scheme", theme->color_scheme, NULL);
	/* The browser's own bar takes the header's colour; an automatic theme
	 * has one for each setting of the device. */
	if (theme->theme_color_light != NULL) {
		add_meta(head_node, "theme-color", theme->theme_color, "(prefers-color-scheme: dark)");
		add_meta(head_node, "theme-color", theme->theme_color_light, "(prefers-color-scheme: light)");
	} else {
		add_meta(head_node, "theme-color", theme->theme_color, NULL);
	}
	add_meta(head_node, "apple-mobile-web-app-capable", "yes", NULL);
	add_meta(head_node, "apple-mobile-web-app-title", APP_NAME, NULL);
	add_meta(head_node, "apple-mobile-web-app-status-bar-style",
		theme->kind == AXIS_WEB_THEME_LIGHT ? "default" : "black", NULL);
	add_meta(head_node, "htmx-config", HTMX_CONFIG, NULL);
	add_owned(head_node, htmx_title_new_with_text(page_title));
	add_icon_link(head_node, "icon", "icons/favicon.svg", "image/svg+xml", "any", NULL);
	add_icon_link(head_node, "icon", "icons/favicon-32.png", "image/png", "32x32", NULL);
	add_icon_link(head_node, "apple-touch-icon", "icons/apple-touch-icon.png", NULL, "180x180", NULL);
	add_icon_link(head_node, "mask-icon", "icons/safari-pinned-tab.svg", NULL, NULL, MASK_ICON_COLOR);
	add_icon_link(head_node, "manifest", "icons/manifest.webmanifest", NULL, NULL, NULL);
	add_owned(head_node, htmx_link_new_stylesheet(themes_css));
	add_owned(head_node, htmx_link_new_stylesheet(css));
	add_owned(HTMX_NODE(head), htmx_script);
	add_owned(HTMX_NODE(head), sse_script);

	return HTMX_NODE(head);
}

static gchar *
full_title(const gchar *title)
{
	if (title == NULL || title[0] == '\0')
		return g_strdup(APP_NAME);
	return g_strdup_printf("%s - %s", title, APP_NAME);
}

/* The daemon's version, and for someone logged in its build (once on the
 * dashboard, now here), and for everyone a link to where the source of this
 * build is offered (AXIS_SOURCE_URL, the build's SOURCE_URL: the licence's
 * section 13 asks that every user over a network be offered the source);
 * below, the notices of the details providers whose data the pages show (a
 * module's, see axis_web_set_footer_notices()). */
static HtmxNode *
build_footer(
	AxisWeb     *web,
	HtmxRequest *request,
	gboolean     logged_in
){
	HtmxFooter *footer = htmx_footer_new();
	HtmxDiv *inner = htmx_div_new();
	HtmxDiv *version = htmx_div_new();
	HtmxSpan *words = htmx_span_new();
	HtmxA *source = htmx_a_new_with_href(AXIS_SOURCE_URL);
	g_autofree gchar *text = logged_in ? g_strdup_printf("%s %s, build %s", APP_NAME, AXIS_VERSION, AXIS_GIT_SHA) :
		g_strdup_printf("%s %s", APP_NAME, AXIS_VERSION);
	HtmxNode *notices = axis_web_build_footer_notices(web, request);

	htmx_element_add_class(HTMX_ELEMENT(footer), "site-footer");
	htmx_element_add_class(HTMX_ELEMENT(inner), "bar");
	htmx_element_add_class(HTMX_ELEMENT(version), "footer-version");
	htmx_node_set_text_content(HTMX_NODE(words), text);
	add_owned(HTMX_NODE(version), words);
	htmx_element_add_class(HTMX_ELEMENT(source), "footer-source");
	htmx_node_set_text_content(HTMX_NODE(source), "Source");
	htmx_a_set_target(source, "_blank");
	htmx_a_set_rel(source, "noreferrer noopener");
	add_owned(HTMX_NODE(version), source);
	add_owned(HTMX_NODE(inner), version);
	if (notices != NULL)
		add_owned(HTMX_NODE(inner), notices);
	add_owned(HTMX_NODE(footer), inner);

	return HTMX_NODE(footer);
}

/*
 * Opens one event stream for the topics the page asked for (see
 * axis_web_page_add_topic()), on the body so that every element of the
 * page can react to it.  The htmx SSE extension turns "sse-connect" into
 * an EventSource and an element with "sse-swap" into a place events are
 * swapped in; the sink below takes the events named "oob", whose data is
 * a fragment of elements that replace their namesakes on the page.  No
 * script of ours is involved, and the page works without it.
 */
static void
attach_stream(
	AxisWeb             *web,
	HtmxRequest         *request,
	const AxisAuthState *state,
	HtmxBody            *body
){
	const gchar *const *topics = axis_web_request_get_topics(request);
	HtmxDiv *sink;
	GString *url;
	guint i;

	if (web == NULL || axis_web_get_hub(web) == NULL || topics == NULL ||
	    state == NULL || state->user_id == 0)
		return;

	url = g_string_new("/events?");
	for (i = 0; topics[i] != NULL; i++) {
		g_autofree gchar *escaped = g_uri_escape_string(topics[i], ":", FALSE);

		g_string_append_printf(url, "%stopic=%s", i > 0 ? "&" : "", escaped);
	}

	htmx_element_set_attribute(HTMX_ELEMENT(body), "hx-ext", "sse");
	htmx_element_set_attribute(HTMX_ELEMENT(body), "sse-connect", url->str);
	g_string_free(url, TRUE);

	sink = htmx_div_new();
	htmx_element_set_attribute(HTMX_ELEMENT(sink), "sse-swap", "oob");
	htmx_element_set_attribute(HTMX_ELEMENT(sink), "hx-swap", "none");
	htmx_element_set_attribute(HTMX_ELEMENT(sink), "hidden", "hidden");
	add_owned(HTMX_NODE(body), sink);
}

/*
 * The line that says, when the stream was refused, that this tab gets no
 * live updates (the hub swaps it in out of band, and hides it again when a
 * stream opens): only on a page that opens a stream.
 */
static HtmxNode *
live_status(
	AxisWeb             *web,
	HtmxRequest         *request,
	const AxisAuthState *state
){
	HtmxP *line;

	if (web == NULL || axis_web_get_hub(web) == NULL || axis_web_request_get_topics(request) == NULL ||
	    state == NULL || state->user_id == 0)
		return NULL;

	line = htmx_p_new();
	htmx_element_set_id(HTMX_ELEMENT(line), AXIS_WEB_HUB_STATUS_ID);
	htmx_element_set_attribute(HTMX_ELEMENT(line), "hidden", "hidden");
	return HTMX_NODE(line);
}

static HtmxNode *
build_document(
	AxisWeb     *web,
	HtmxRequest *request,
	const gchar *page_title,
	const gchar *request_path,
	HtmxNode    *content
){
	HtmxHtml *html = htmx_html_new();
	HtmxBody *body = htmx_body_new();
	HtmxMain *main_element = htmx_main_new();
	HtmxA *skip = htmx_a_new_with_href("#content");
	const AxisAuthState *state = axis_web_auth_state(request);
	const AxisWebTheme *theme;

	/* Every htmx request from the page inherits this header, which is how
	 * htmx requests carry the CSRF token without any script of ours.  The
	 * token is URL-safe base64, so the JSON needs no escaping. */
	if (state != NULL && state->user_id != 0 && state->csrf != NULL) {
		g_autofree gchar *headers = g_strdup_printf("{\"" AXIS_CSRF_HEADER "\":\"%s\"}", state->csrf);

		htmx_element_set_attribute(HTMX_ELEMENT(body), "hx-headers", headers);
	}

	htmx_html_set_lang(html, "en");
	htmx_element_add_class(HTMX_ELEMENT(skip), "skip-link");
	htmx_node_set_text_content(HTMX_NODE(skip), "Skip to content");
	htmx_element_set_id(HTMX_ELEMENT(main_element), "content");
	htmx_element_add_class(HTMX_ELEMENT(main_element), "content");
	htmx_node_add_child(HTMX_NODE(main_element), content);

	add_owned(HTMX_NODE(body), skip);
	add_owned(HTMX_NODE(body), build_header(web, request, request_path));
	{
		HtmxNode *status = live_status(web, request, state);

		if (status != NULL)
			add_owned(HTMX_NODE(body), status);
	}
	add_owned(HTMX_NODE(body), main_element);
	add_owned(HTMX_NODE(body), build_footer(web, request, state != NULL && state->user_id != 0));
	attach_stream(web, request, state, body);

	/* The theme is an attribute and nothing else: no script reads it. */
	theme = axis_web_theme_resolve(state != NULL && state->user_id != 0 ? state->theme : NULL);
	htmx_element_set_attribute(HTMX_ELEMENT(html), "data-theme", theme->id);

	add_owned(HTMX_NODE(html), build_head(page_title, theme));
	add_owned(HTMX_NODE(html), body);

	return HTMX_NODE(html);
}

HtmxNode *
axis_web_page_intro(const gchar *text)
{
	HtmxP *intro = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(intro), "page-intro");
	htmx_node_set_text_content(HTMX_NODE(intro), text);
	return HTMX_NODE(intro);
}

/*
 * A title element and the content, which is what htmx swaps in.  When the
 * content is the page's whole content (the title search, typed on any page,
 * answers that way, and the address follows it), the page changes without
 * the header being drawn again; so the tabs and the rows below them come
 * along out of band, marked for the new address.  Nothing else of the header
 * does: the box being typed in stays as it is, focus and all.
 */
static HtmxNode *
build_fragment(
	AxisWeb     *web,
	HtmxRequest *request,
	const gchar *page_title,
	HtmxNode    *content
){
	HtmxFragment *fragment = htmx_fragment_new();
	const AxisAuthState *state = axis_web_auth_state(request);

	add_owned(HTMX_NODE(fragment), htmx_title_new_with_text(page_title));
	htmx_node_add_child(HTMX_NODE(fragment), content);

	if (state != NULL && state->user_id != 0 &&
	    g_strcmp0(htmx_request_get_target_id(request), "content") == 0) {
		HtmxNode *tabs;
		HtmxNode *rows;

		build_nav(web, state, htmx_request_get_path(request), &tabs, &rows);
		htmx_element_set_attribute(HTMX_ELEMENT(tabs), "hx-swap-oob", "true");
		htmx_element_set_attribute(HTMX_ELEMENT(rows), "hx-swap-oob", "true");
		add_owned(HTMX_NODE(fragment), tabs);
		add_owned(HTMX_NODE(fragment), rows);
	}

	return HTMX_NODE(fragment);
}

HtmxResponse *
axis_web_page_respond(
	HtmxRequest *request,
	const gchar *title,
	HtmxNode    *content
){
	HtmxRequestHeaders *headers;
	g_autoptr(HtmxNode) root = NULL;
	g_autoptr(HtmxResponse) response = htmx_response_new();
	g_autofree gchar *page_title = full_title(title);
	g_autofree gchar *markup = NULL;
	g_autofree gchar *document = NULL;
	gboolean fragment;

	g_return_val_if_fail(HTMX_IS_REQUEST(request), NULL);
	g_return_val_if_fail(HTMX_IS_NODE(content), NULL);

	headers = htmx_request_get_headers(request);
	fragment = htmx_request_is_htmx(request) &&
	           !(headers != NULL && htmx_request_headers_is_history_restore(headers));

	/* Every cell of every table takes its column's role. */
	axis_web_table_apply_roles(content);

	if (fragment) {
		root = build_fragment(axis_web_from_request(request), request, page_title, content);
		markup = htmx_element_render(HTMX_ELEMENT(root));
		document = g_steal_pointer(&markup);
	} else {
		root = build_document(axis_web_from_request(request), request, page_title,
			htmx_request_get_path(request), content);
		markup = htmx_element_render(HTMX_ELEMENT(root));
		/* The element classes do not emit a doctype. */
		document = g_strconcat("<!DOCTYPE html>\n", markup, NULL);
	}

	htmx_response_set_content(response, document);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	/* The shapes of one URL must not be mixed up by a cache: the page, the
	 * content, and the content with the tabs (by the element replaced). */
	htmx_response_add_header(response, "Vary", "HX-Request, HX-History-Restore-Request, HX-Target");

	return (HtmxResponse *)g_steal_pointer(&response);
}

HtmxResponse *
axis_web_error_respond(
	HtmxRequest *request,
	guint        status,
	const gchar *heading,
	const gchar *message
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	g_autoptr(HtmxP) text = htmx_p_new();
	g_autoptr(HtmxA) back = htmx_a_new_with_href("/");
	g_autoptr(HtmxP) back_line = htmx_p_new();
	HtmxResponse *response;
	g_autofree gchar *status_text = g_strdup_printf("%u", status);

	htmx_element_add_class(HTMX_ELEMENT(content), "error-page");
	htmx_element_set_attribute(HTMX_ELEMENT(content), "data-status", status_text);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	htmx_node_set_text_content(HTMX_NODE(text), message);
	htmx_node_set_text_content(HTMX_NODE(back), "Back to the dashboard");
	htmx_node_add_child(HTMX_NODE(back_line), HTMX_NODE(back));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(text));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(back_line));

	response = axis_web_page_respond(request, heading, HTMX_NODE(content));
	htmx_response_set_status(response, status);

	return response;
}

HtmxResponse *
axis_web_internal_error(
	HtmxRequest  *request,
	const GError *error
){
	const gchar *path = htmx_request_get_path(request);

	g_warning("internal error answering %s %s: %s",
		htmx_method_to_string(htmx_request_get_method(request)),
		path != NULL ? path : "(unknown)",
		error != NULL ? error->message : "(no detail)");

	return axis_web_error_respond(request, 500, "Something went wrong",
		"Axis could not complete the request. Try again; if it fails again, the reason is in the Axis log.");
}
