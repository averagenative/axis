/*
 * axis-web-ready.c - what is needed to download, as the pages show it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-ready.h"
#include "web/axis-web-private.h"
#include "web/axis-web-setup.h"

#include <string.h>

/* The media modules whose roots the card asks about (their ids, from
 * modules/movies/axis-movies.h and modules/tv/axis-tv.h). */
#define FILMS_MODULE_ID  "movies"
#define SERIES_MODULE_ID "tv"

struct _AxisWebReady
{
	AxisMeta      *meta;		/* nullable */
	AxisIndexers  *indexers;
	AxisDownloads *downloads;
	AxisStorage   *storage;
};

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
	HtmxNode    *node,
	const gchar *css,
	const gchar *text
){
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(node), css);
	htmx_node_set_text_content(node, text);
	return node;
}

static HtmxNode *
link_new(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	return HTMX_NODE(link);
}

/* ── The checklist ──────────────────────────────────────────────── */

/* The words of an item's link into the setup guide's step. */
static const gchar *
guide_action(AxisReadyItem item)
{
	switch (item) {
	case AXIS_READY_METADATA:
		return "Add a TMDb token";
	case AXIS_READY_INDEXER:
		return "Set up an indexer";
	case AXIS_READY_CLIENT:
		return "Set up a download client";
	case AXIS_READY_FILMS_ROOT:
		return "Add a library folder for Movies";
	case AXIS_READY_SERIES_ROOT:
		return "Add a library folder for TV";
	default:
		return "Allow Axis to write to the library";
	}
}

/* As axis_web_ready_checklist_new(); with @guide, each link goes to the
 * setup guide's step for the item rather than to the page it is set on. */
static HtmxNode *
checklist_new(
	const AxisReadiness *ready,
	gboolean             admin,
	gboolean             guide
);

HtmxNode *
axis_web_ready_checklist_new(
	const AxisReadiness *ready,
	gboolean             admin
){
	return checklist_new(ready, admin, FALSE);
}

static HtmxNode *
checklist_new(
	const AxisReadiness *ready,
	gboolean             admin,
	gboolean             guide
){
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "ready-list");
	for (i = 0; i < AXIS_READY_N_ITEMS; i++) {
		const AxisReadyCheck *check = &ready->checks[i];
		HtmxLi *item;
		HtmxSpan *what;

		if (!check->asked)
			continue;
		item = htmx_li_new();
		what = htmx_span_new();
		htmx_element_add_class(HTMX_ELEMENT(item), "ready-item");
		htmx_element_add_class(HTMX_ELEMENT(item), check->done ? "ready-done" : "ready-needed");
		add(item, text_node(HTMX_NODE(htmx_span_new()), check->done ? "badge badge-succeeded" : "badge badge-warn",
			check->done ? "Done" : "Needed"));
		htmx_element_add_class(HTMX_ELEMENT(what), "ready-what");
		add(what, text_node(HTMX_NODE(htmx_span_new()), "ready-label", axis_ready_item_label(i)));
		add(what, text_node(HTMX_NODE(htmx_span_new()), "ready-text", check->text != NULL ? check->text : ""));
		add(item, what);
		if (admin && !check->done) {
			HtmxNode *link = guide ? link_new(axis_setup_step_path(axis_setup_step_of_ready_item(i)), guide_action(i)) :
				link_new(axis_ready_item_href(i), axis_ready_item_action(i));

			htmx_element_add_class(HTMX_ELEMENT(link), "ready-link");
			add(item, link);
		}
		add(list, item);
	}
	return HTMX_NODE(list);
}

/* ── The notice and the note ────────────────────────────────────── */

/* The items that stop a title of @kinds being searched for or sent. */
static gboolean
blocks(
	AxisReadyItem item,
	guint         kinds
){
	switch (item) {
	case AXIS_READY_INDEXER:
	case AXIS_READY_CLIENT:
		return TRUE;
	case AXIS_READY_FILMS_ROOT:
		return (kinds & AXIS_READY_FILMS) != 0;
	case AXIS_READY_SERIES_ROOT:
		return (kinds & AXIS_READY_SERIES) != 0;
	default:
		return FALSE;
	}
}

/* "a", "a and b", "a, b and c" of what blocks; %NULL when nothing does. */
static gchar *
missing_phrase(
	const AxisReadiness *ready,
	guint                kinds
){
	g_autoptr(GPtrArray) parts = g_ptr_array_new();
	GString *text;
	guint i;

	for (i = 0; i < AXIS_READY_N_ITEMS; i++) {
		const AxisReadyCheck *check = &ready->checks[i];

		if (blocks(i, kinds) && check->asked && !check->done && check->missing != NULL)
			g_ptr_array_add(parts, check->missing);
	}
	if (parts->len == 0)
		return NULL;
	text = g_string_new(NULL);
	for (i = 0; i < parts->len; i++) {
		if (i > 0)
			g_string_append(text, i + 1 == parts->len ? " and " : ", ");
		g_string_append(text, g_ptr_array_index(parts, i));
	}
	return g_string_free(text, FALSE);
}

static gboolean
import_blocked(const AxisReadiness *ready)
{
	return ready->checks[AXIS_READY_WRITABLE].asked && !ready->checks[AXIS_READY_WRITABLE].done;
}

/* The links to what is missing (of what blocks, and the import), or for
 * someone else, who can set it up. */
static HtmxNode *
links_line(
	const AxisReadiness *ready,
	guint                kinds,
	gboolean             admin,
	const gchar         *css
){
	HtmxP *line = htmx_p_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(line), css);
	if (!admin) {
		htmx_node_set_text_content(HTMX_NODE(line), "An administrator can set this up.");
		return HTMX_NODE(line);
	}
	for (i = 0; i < AXIS_READY_N_ITEMS; i++) {
		const AxisReadyCheck *check = &ready->checks[i];

		if (!check->asked || check->done || (!blocks(i, kinds) && i != AXIS_READY_WRITABLE))
			continue;
		add(line, link_new(axis_ready_item_href(i), axis_ready_item_action(i)));
	}
	return HTMX_NODE(line);
}

HtmxNode *
axis_web_ready_notice_new(
	const AxisReadiness *ready,
	guint                kinds,
	const gchar         *noun,
	gboolean             admin
){
	g_autofree gchar *missing = NULL;
	HtmxDiv *box;

	g_return_val_if_fail(ready != NULL, NULL);

	missing = missing_phrase(ready, kinds);
	if (missing == NULL && !import_blocked(ready))
		return NULL;
	box = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(box), "ready-notice");
	htmx_element_add_class(HTMX_ELEMENT(box), "notice");
	htmx_element_add_class(HTMX_ELEMENT(box), "notice-warning");
	htmx_element_add_class(HTMX_ELEMENT(box), "ready-notice");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "role", "status");
	if (missing != NULL) {
		g_autofree gchar *text = g_strdup_printf("Nothing can search for or download this %s yet: %s.", noun, missing);

		add(box, text_node(HTMX_NODE(htmx_p_new()), "ready-statement", text));
	}
	if (import_blocked(ready)) {
		g_autofree gchar *text = missing != NULL ? g_strdup("Nor could what is downloaded be imported into the "
			"library yet: storage is in read-only mode.") : g_strdup_printf("What is downloaded for this %s cannot "
			"be imported into the library yet: storage is in read-only mode.", noun);

		add(box, text_node(HTMX_NODE(htmx_p_new()), "ready-statement", text));
	}
	add(box, links_line(ready, kinds, admin, "ready-links"));
	return HTMX_NODE(box);
}

HtmxNode *
axis_web_ready_note_new(
	const AxisReadiness *ready,
	guint                kinds,
	const gchar         *noun
){
	g_autofree gchar *missing = NULL;
	g_autoptr(GString) text = NULL;
	HtmxDiv *note;

	g_return_val_if_fail(ready != NULL, NULL);

	missing = missing_phrase(ready, kinds);
	if (missing == NULL && !import_blocked(ready))
		return NULL;
	text = g_string_new(NULL);
	if (missing != NULL)
		g_string_append_printf(text, "You can add it, but nothing can search for or download a %s yet: %s.", noun,
			missing);
	if (import_blocked(ready) && text->len > 0)
		g_string_append(text, " Nor could what is downloaded be imported yet: storage is in read-only mode.");
	else if (import_blocked(ready))
		g_string_append(text, "You can add it and it can be downloaded, but what is downloaded cannot be imported "
			"yet: storage is in read-only mode.");
	note = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(note), "ready-note");
	add(note, text_node(HTMX_NODE(htmx_p_new()), "help", text->str));
	add(note, links_line(ready, kinds, TRUE, "ready-links"));
	return HTMX_NODE(note);
}

/* ── The dashboard ──────────────────────────────────────────────── */

typedef struct
{
	AxisReadiness ready;
	gboolean      ok;
	gint64        taken_us;
	guint         dismissed;	/* the setup guide's steps not used (AXIS_SETUP_BIT()) */
} Figures;

static void
figures_free(gpointer data)
{
	Figures *figures = data;

	if (figures == NULL)
		return;
	axis_readiness_clear(&figures->ready);
	g_free(figures);
}

static gpointer
dash_collect(gpointer user_data)
{
	AxisWebReady *self = user_data;
	Figures *figures = g_new0(Figures, 1);
	g_autoptr(GError) error = NULL;

	figures->taken_us = g_get_real_time();
	figures->ok = axis_acquire_check_ready(self->meta, self->indexers, self->downloads, self->storage,
		AXIS_READY_FILMS | AXIS_READY_SERIES, &figures->ready, &error);
	if (!figures->ok) {
		g_debug("dashboard: cannot tell what is needed to download: %s", error->message);
		return figures;
	}
	/* What an administrator said is not used is not asked for. */
	{
		guint skipped = 0;

		if (!axis_web_setup_load_choices(axis_storage_get_pool(self->storage), &skipped, &figures->dismissed,
		        &error))
			g_debug("dashboard: the setup guide's choices could not be read: %s", error->message);
	}
	return figures;
}

static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	Figures *figures = data;
	AxisWeb *web = axis_web_from_request(request);
	AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;
	gboolean films = registry == NULL || axis_module_registry_is_enabled(registry, FILMS_MODULE_ID);
	gboolean series = registry == NULL || axis_module_registry_is_enabled(registry, SERIES_MODULE_ID);
	gboolean admin = axis_web_request_is_admin(request);
	gboolean guide = admin && registry != NULL && axis_module_registry_is_enabled(registry, AXIS_WEB_SETUP_MODULE_ID);
	HtmxDiv *piece;
	guint i;

	if (figures == NULL || !figures->ok || (!films && !series))
		return;
	/* A media module that is off has no root to ask for. */
	if (!films)
		figures->ready.checks[AXIS_READY_FILMS_ROOT].asked = FALSE;
	if (!series)
		figures->ready.checks[AXIS_READY_SERIES_ROOT].asked = FALSE;
	/* Nor is what the setup guide was told is not used (only where the
	 * guide is: it is where that is said, and undone). */
	for (i = 0; registry != NULL && axis_module_registry_is_enabled(registry, AXIS_WEB_SETUP_MODULE_ID) &&
	     i < AXIS_READY_N_ITEMS; i++) {
		if ((figures->dismissed & AXIS_SETUP_BIT(axis_setup_step_of_ready_item(i))) != 0)
			figures->ready.checks[i].asked = FALSE;
	}
	/* Gone once everything is there. */
	if (axis_readiness_complete(&figures->ready))
		return;

	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	add(piece, text_node(HTMX_NODE(htmx_p_new()), "help", films && series ?
		"Before movies and series can be found and downloaded, Axis needs each of these. This card goes once they "
		"are all here." : films ? "Before movies can be found and downloaded, Axis needs each of these. This card "
		"goes once they are all here." : "Before series can be found and downloaded, Axis needs each of these. "
		"This card goes once they are all here."));
	add(piece, checklist_new(&figures->ready, admin, guide));
	if (!admin)
		add(piece, text_node(HTMX_NODE(htmx_p_new()), "help", "An administrator can set these up."));
	if (guide) {
		HtmxP *line = htmx_p_new();
		HtmxNode *link = link_new(AXIS_WEB_SETUP_PATH, "Open the setup guide");

		htmx_element_add_class(HTMX_ELEMENT(line), "card-foot");
		htmx_element_add_class(HTMX_ELEMENT(line), "ready-guide");
		add(line, link);
		add(line, text_node(HTMX_NODE(htmx_span_new()), NULL, ": each step says what the thing is, where its key or "
			"address is found, and tests it."));
		add(piece, line);
	}
	add(piece, axis_web_dashboard_as_of("Checked", figures->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SETUP, 0, guide ? AXIS_WEB_SETUP_PATH : NULL, HTMX_NODE(piece));
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_dashboard_part(web, dash_collect, dash_render, figures_free, user_data);
}

const AxisModuleInfo *
axis_web_ready_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_WEB_READY_MODULE_ID;
		info.name = "Getting started";
		info.register_routes = register_routes;
		info.description = "Lists on the dashboard what is still needed before movies and series can be found "
			"and downloaded (a TMDb token, an indexer, a download client, a library folder, permission "
			"to write), until it is all there.";
		info.flags = AXIS_MODULE_REQUIRED;
	}
	return &info;
}

AxisWebReady *
axis_web_ready_new(
	AxisMeta      *meta,
	AxisIndexers  *indexers,
	AxisDownloads *downloads,
	AxisStorage   *storage
){
	AxisWebReady *self;

	g_return_val_if_fail(indexers != NULL && downloads != NULL && storage != NULL, NULL);

	self = g_new0(AxisWebReady, 1);
	self->meta = meta != NULL ? axis_meta_ref(meta) : NULL;
	self->indexers = axis_indexers_ref(indexers);
	self->downloads = axis_downloads_ref(downloads);
	self->storage = axis_storage_ref(storage);
	return self;
}

void
axis_web_ready_free(AxisWebReady *self)
{
	if (self == NULL)
		return;
	g_clear_pointer(&self->meta, axis_meta_unref);
	axis_indexers_unref(self->indexers);
	axis_downloads_unref(self->downloads);
	axis_storage_unref(self->storage);
	g_free(self);
}
