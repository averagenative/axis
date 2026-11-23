/*
 * axis-web-actions.c - the action slot: what media modules offer for a title
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The search and title pages know nothing of wanted lists or libraries.  A
 * module registers a part (axis_web_add_title_actions()): a function that
 * looks at the titles about to be shown on a worker thread, and one that
 * draws that module's piece of a title's action area and of a result card's
 * corner on the main loop.  A request runs every part of a module that is
 * switched on; with no part the area is not drawn at all.
 */

#include "web/axis-web-private.h"

#include <string.h>

typedef struct
{
	AxisModuleSwitch          *sw;	/* its module's flag, or %NULL */
	AxisWebActionCollectFunc   collect;
	AxisWebActionRenderFunc    render;
	GDestroyNotify             free_figures;
	gpointer                   user_data;
} Part;

struct _AxisWebActions
{
	GPtrArray *parts;	/* of Part*: only added to before the first request */
};

struct _AxisWebActionRun
{
	GPtrArray *parts;	/* of Part*, borrowed: the parts that were live */
	gpointer  *figures;	/* one per part */
};

static void
part_free(Part *part)
{
	axis_module_switch_unref(part->sw);
	g_free(part);
}

AxisWebActions *
axis_web_actions_new(void)
{
	AxisWebActions *self = g_new0(AxisWebActions, 1);

	self->parts = g_ptr_array_new_with_free_func((GDestroyNotify)part_free);
	return self;
}

void
axis_web_actions_free(AxisWebActions *self)
{
	if (self == NULL)
		return;
	g_ptr_array_free(self->parts, TRUE);
	g_free(self);
}

void
axis_web_actions_add(
	AxisWebActions           *self,
	AxisModuleSwitch         *sw,
	AxisWebActionCollectFunc  collect,
	AxisWebActionRenderFunc   render,
	GDestroyNotify            free_figures,
	gpointer                  user_data
){
	Part *part = g_new0(Part, 1);

	part->sw = sw;
	part->collect = collect;
	part->render = render;
	part->free_figures = free_figures;
	part->user_data = user_data;
	g_ptr_array_add(self->parts, part);
}

static gboolean
part_is_live(const Part *part)
{
	return part->sw == NULL || axis_module_switch_is_on(part->sw);
}

AxisWebActionRun *
axis_web_actions_collect(
	AxisWebActions     *self,
	const GPtrArray    *titles,
	AxisWebActionPlace  place
){
	AxisWebActionRun *run;
	guint i;

	if (self == NULL || self->parts->len == 0)
		return NULL;
	run = g_new0(AxisWebActionRun, 1);
	run->parts = g_ptr_array_new();
	for (i = 0; i < self->parts->len; i++) {
		Part *part = g_ptr_array_index(self->parts, i);

		if (part_is_live(part))
			g_ptr_array_add(run->parts, part);
	}
	run->figures = g_new0(gpointer, MAX(run->parts->len, 1));
	for (i = 0; i < run->parts->len; i++) {
		Part *part = g_ptr_array_index(run->parts, i);

		if (part->collect != NULL)
			run->figures[i] = part->collect(titles, place, part->user_data);
	}
	return run;
}

void
axis_web_actions_run_free(AxisWebActionRun *run)
{
	guint i;

	if (run == NULL)
		return;
	for (i = 0; i < run->parts->len; i++) {
		Part *part = g_ptr_array_index(run->parts, i);

		if (run->figures[i] != NULL && part->free_figures != NULL)
			part->free_figures(run->figures[i]);
	}
	g_free(run->figures);
	g_ptr_array_unref(run->parts);
	g_free(run);
}

HtmxNode *
axis_web_actions_render(
	AxisWebActionRun    *run,
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title
){
	HtmxDiv *area = NULL;
	guint i;

	if (run == NULL)
		return NULL;
	for (i = 0; i < run->parts->len; i++) {
		Part *part = g_ptr_array_index(run->parts, i);
		HtmxNode *piece;

		/* A module switched off while its figures were read shows nothing. */
		if (!part_is_live(part))
			continue;
		piece = part->render(request, place, title, run->figures[i], part->user_data);
		if (piece == NULL)
			continue;
		if (area == NULL) {
			area = htmx_div_new();
			htmx_element_add_class(HTMX_ELEMENT(area), place == AXIS_WEB_ACTION_PAGE ? "title-actions" :
				place == AXIS_WEB_ACTION_DETAIL ? "title-detail" : place == AXIS_WEB_ACTION_NOTICE ?
				"title-notices" : place == AXIS_WEB_ACTION_DISCOVER ? "discover-state" :
				place == AXIS_WEB_ACTION_BAR ? "add-defaults-bar" : "card-actions");
		}
		htmx_node_add_child(HTMX_NODE(area), piece);
		g_object_unref(piece);
	}
	return area != NULL ? HTMX_NODE(area) : NULL;
}

const gchar *
axis_web_actions_bar_place(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? "add-defaults-movies" : "add-defaults-series";
}

HtmxNode *
axis_web_actions_render_bar(
	AxisWebActionRun *run,
	HtmxRequest      *request,
	AxisMetaKind      kind
){
	AxisMetaTitle kind_only;

	/* (The title only names the kind.) */
	memset(&kind_only, 0, sizeof(kind_only));
	kind_only.kind = kind;
	return axis_web_actions_render(run, request, AXIS_WEB_ACTION_BAR, &kind_only);
}
