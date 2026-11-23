/*
 * axis-web-storage-dashboard.c - the volumes on the dashboard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The Space card's list of volumes, from what the daemon knows of them (no
 * I/O: the last check), and a line under what needs attention for every
 * volume that is offline or holds something else.  The figures are those
 * of the Storage pages, in the same words.  A volume's name is text in
 * <bdi>; its link carries its id.
 */

#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* A volume at least this full has its bar in the warning colour. */
#define FULL_PERCENT 90

/* A thin bar of how full a volume is, drawn as SVG: its length is an
 * attribute, which the content security policy allows where a style
 * would not be. */
static HtmxNode *
fullness_bar(guint percent)
{
	HtmxNode *svg = htmx_node_new("svg");
	HtmxNode *track = htmx_node_new("rect");
	HtmxNode *used = htmx_node_new("rect");
	g_autofree gchar *label = g_strdup_printf("%u%% used", percent);
	g_autofree gchar *width = g_strdup_printf("%u", percent);

	htmx_element_add_class(HTMX_ELEMENT(svg), "space-bar");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "viewBox", "0 0 100 4");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "preserveAspectRatio", "none");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "role", "img");
	htmx_element_set_attribute(HTMX_ELEMENT(svg), "aria-label", label);
	htmx_element_add_class(HTMX_ELEMENT(track), "space-bar-track");
	htmx_element_set_attribute(HTMX_ELEMENT(track), "width", "100");
	htmx_element_set_attribute(HTMX_ELEMENT(track), "height", "4");
	htmx_element_add_class(HTMX_ELEMENT(used), percent >= FULL_PERCENT ? "space-bar-full" : "space-bar-used");
	htmx_element_set_attribute(HTMX_ELEMENT(used), "width", width);
	htmx_element_set_attribute(HTMX_ELEMENT(used), "height", "4");
	ADD(svg, track);
	ADD(svg, used);

	return svg;
}

static HtmxNode *
volume_item(const AxisVolumeInfo *volume)
{
	HtmxLi *item = htmx_li_new();
	HtmxSpan *head = htmx_span_new();
	g_autofree gchar *href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, volume->id);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *name = htmx_bdi_new();
	HtmxSpan *figures = htmx_span_new();
	gboolean usable = axis_volume_state_is_usable(volume->state);

	htmx_element_add_class(HTMX_ELEMENT(item), "space-item");
	htmx_node_set_text_content(HTMX_NODE(name), volume->name);
	ADD(link, name);
	ADD(head, link);
	/* A volume that is not there is called out. */
	if (!usable) {
		HtmxSpan *badge = htmx_span_new();
		g_autofree gchar *css = g_strdup_printf("state-%s", axis_volume_state_to_string(volume->state));

		htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
		htmx_element_add_class(HTMX_ELEMENT(badge), css);
		htmx_node_set_text_content(HTMX_NODE(badge), axis_volume_state_label(volume->state));
		ADD(head, badge);
	}
	htmx_element_add_class(HTMX_ELEMENT(head), "space-name");
	ADD(item, head);

	htmx_element_add_class(HTMX_ELEMENT(figures), "space-figures");
	if (!volume->has_space) {
		htmx_node_set_text_content(HTMX_NODE(figures), "Space not known yet");
		ADD(item, figures);
	} else {
		g_autofree gchar *total = axis_size_format(volume->total_bytes);
		g_autofree gchar *free_text = axis_size_format(volume->free_bytes);
		g_autofree gchar *text = g_strdup_printf("%s%s free of %s", usable ? "" : "Last known: ", free_text,
			total);
		guint percent = volume->total_bytes > 0 && volume->free_bytes <= volume->total_bytes ?
			(guint)((volume->total_bytes - volume->free_bytes) * 100 / volume->total_bytes) : 0;

		htmx_node_set_text_content(HTMX_NODE(figures), text);
		ADD(item, figures);
		ADD(item, fullness_bar(percent));
	}

	return HTMX_NODE(item);
}

static void
render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
){
	AxisWebStorage *self = user_data;
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	HtmxDiv *piece = htmx_div_new();
	gint64 checked = 0;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	if (volumes->len == 0) {
		HtmxA *add = htmx_a_new_with_href("/storage");

		ADD(piece, axis_web_storage_text("help", "No disk has been added yet."));
		htmx_node_set_text_content(HTMX_NODE(add), "Add one under Disks and library folders");
		ADD(piece, add);
	} else {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "space-list");
		for (i = 0; i < volumes->len; i++) {
			const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);

			/* A disk that is away is called out here at once, by its badge;
			 * the card of what needs attention says so after its wait, as
			 * the bell does (the disks' attention source). */
			ADD(list, volume_item(volume));
			checked = MAX(checked, volume->space_checked_us);
		}
		ADD(piece, list);
	}
	ADD(piece, axis_web_dashboard_as_of("Space checked", checked));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SPACE, 10, "/storage", HTMX_NODE(piece));
}

void
axis_web_storage_register_dashboard(
	AxisWebStorage *self,
	AxisWeb        *web
){
	axis_web_add_dashboard_part(web, NULL, render, NULL, self);
}
