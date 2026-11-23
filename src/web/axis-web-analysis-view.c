/*
 * axis-web-analysis-view.c - pieces of the Analysis pages: links to file
 * lists, the coverage block, the snapshot header
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A name (a root's, a volume's, a codec's) is shown as text content, which
 * htmx-glib escapes; a link carries numbers and values the file list's own
 * parser accepts, nothing else.
 */

#include "web/axis-web-analysis-private.h"

#include <string.h>

/* ── Links ──────────────────────────────────────────────────────── */

gchar *
axis_web_analysis_files_href(const AxisLibraryFilter *filter)
{
	g_autofree gchar *query = axis_library_filter_to_query(filter, FALSE);
	AxisLibraryFilter check;
	gboolean ok;

	if (query[0] == '\0')
		return g_strdup("/library/files");
	/* What the list would refuse is not offered as a link. */
	ok = axis_library_filter_parse(query, &check, NULL);
	axis_library_filter_clear(&check);
	if (!ok)
		return NULL;
	return g_strdup_printf("/library/files?%s", query);
}

HtmxNode *
axis_web_analysis_figure(
	const gchar *text,
	const gchar *href
){
	if (href != NULL)
		return axis_web_browser_link(href, text);
	{
		HtmxSpan *span = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(span), text);
		return HTMX_NODE(span);
	}
}

gchar *
axis_web_analysis_count_label(gint64 count)
{
	return g_strdup_printf("%" G_GINT64_FORMAT " %s", count, count == 1 ? "file" : "files");
}

gchar *
axis_web_analysis_profile_label(
	const gchar *family,
	const gchar *name
){
	if (g_strcmp0(family, name) == 0)
		return g_strdup(name);
	return g_strdup_printf("%s: %s", family != NULL ? family : "?", name != NULL ? name : "?");
}

/* ── Names in the snapshot ──────────────────────────────────────── */

static const gchar *
name_in(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	gint64                      id
){
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, report, key, NULL);

	if (fact == NULL)
		return "(removed)";
	return axis_analysis_extra_string(fact->extra, "name");
}

const gchar *
axis_web_analysis_volume_name(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      id
){
	return name_in(snapshot, "space_volume", id);
}

const gchar *
axis_web_analysis_root_name(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      id
){
	return name_in(snapshot, "space_root", id);
}

/* ── Navigation ─────────────────────────────────────────────────── */

HtmxNode *
axis_web_analysis_tabs(const gchar *current)
{
	static const struct
	{
		const gchar *path;
		const gchar *label;
	} tabs[] = {
		{ "/analysis",             "Overview" },
		{ "/analysis/space",       "Space" },
		{ "/analysis/composition", "File formats" },
		{ "/analysis/largest",     "Largest" },
		{ "/analysis/duplicates",  "Likely duplicates" },
		{ "/analysis/split",       "Split across disks" },
		{ "/analysis/leftovers",   "Leftovers" },
		{ NULL, NULL }
	};
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Disk usage reports");
	htmx_element_add_class(HTMX_ELEMENT(list), "tabs");
	for (i = 0; tabs[i].path != NULL; i++) {
		HtmxLi *item = htmx_li_new();

		if (g_strcmp0(current, tabs[i].path) == 0) {
			HtmxSpan *here = htmx_span_new();

			htmx_element_set_attribute(HTMX_ELEMENT(here), "aria-current", "page");
			htmx_node_set_text_content(HTMX_NODE(here), tabs[i].label);
			BROWSER_ADD(item, here);
		} else {
			BROWSER_ADD(item, axis_web_browser_link(tabs[i].path, tabs[i].label));
		}
		BROWSER_ADD(list, item);
	}
	BROWSER_ADD(nav, list);
	return HTMX_NODE(nav);
}

/* ── Refreshing ─────────────────────────────────────────────────── */

HtmxNode *
axis_web_analysis_status_block(const AxisAnalysisStatus *status)
{
	HtmxDiv *block = htmx_div_new();

	htmx_element_set_id(HTMX_ELEMENT(block), "analysis-status");
	if (status != NULL && status->refreshing) {
		g_autofree gchar *text = g_strdup_printf("%s%s%s", status->running ? "Refreshing the reports" :
			"A refresh is waiting for its turn", status->message != NULL && status->message[0] != '\0' ?
			": " : "", status->message != NULL ? status->message : "");

		htmx_element_add_class(HTMX_ELEMENT(block), "notice");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "role", "status");
		/* Asks again until it is done; the answer to that reloads the page. */
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", "/analysis/status");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", "every 2s");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
		if (status->running && status->progress >= 0)
			BROWSER_ADD(block, htmx_progress_new_with_value(status->progress, 1.0));
		BROWSER_ADD(block, axis_web_storage_text(NULL, text));
	}
	return HTMX_NODE(block);
}

HtmxNode *
axis_web_analysis_snapshot_header(
	HtmxRequest                *request,
	const AxisAnalysisSnapshot *snapshot,
	const AxisAnalysisStatus   *status
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *when = axis_web_storage_format_time(snapshot->taken_us, "unknown");
	g_autofree gchar *line = g_strdup_printf("Figures as of %s. They were computed from the index in %.1f s "
		"and describe the library at that moment, not as it is now.", when, snapshot->took_ms / 1000.0);

	htmx_element_add_class(HTMX_ELEMENT(box), "snapshot-header");
	BROWSER_ADD(box, axis_web_storage_text("snapshot-time", line));
	BROWSER_ADD(box, axis_web_analysis_status_block(status));
	if (axis_web_request_is_admin(request)) {
		HtmxNode *form = axis_web_auth_post_form(request, "/analysis/refresh");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button("Refresh the reports now", NULL));
		BROWSER_ADD(box, form);
	}
	return HTMX_NODE(box);
}

HtmxNode *
axis_web_analysis_empty(
	HtmxRequest              *request,
	const AxisAnalysisStatus *status
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *card = axis_web_storage_card("No report yet");

	BROWSER_ADD(card, axis_web_storage_text(NULL, "The reports are worked out once scans have finished and file details have been read, "
		"or when an administrator asks. None has been worked out yet, so there is nothing to show."));
	BROWSER_ADD(card, axis_web_analysis_status_block(status));
	if (axis_web_request_is_admin(request)) {
		HtmxNode *form = axis_web_auth_post_form(request, "/analysis/refresh");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button("Refresh the reports now", "button-primary"));
		BROWSER_ADD(card, form);
	}
	BROWSER_ADD(box, card);
	return HTMX_NODE(box);
}

/* ── Coverage ───────────────────────────────────────────────────── */

/* "Label: figure rest": what was left out, how much, and what follows. */
static void
item(
	HtmxNode    *list,
	const gchar *label,
	const gchar *figure,
	const gchar *href,
	const gchar *rest
){
	HtmxLi *entry = htmx_li_new();
	HtmxSpan *head = htmx_span_new();
	HtmxSpan *tail = htmx_span_new();
	g_autofree gchar *head_text = g_strdup_printf("%s: ", label);

	htmx_node_set_text_content(HTMX_NODE(head), head_text);
	BROWSER_ADD(entry, head);
	BROWSER_ADD(entry, axis_web_analysis_figure(figure, href));
	htmx_node_set_text_content(HTMX_NODE(tail), rest);
	BROWSER_ADD(entry, tail);
	BROWSER_ADD(list, entry);
}

static void
item_files(
	HtmxNode          *list,
	const gchar       *label,
	gint64             count,
	const gchar       *rest,
	AxisLibraryFilter *filter
){
	g_autofree gchar *href = axis_web_analysis_files_href(filter);
	g_autofree gchar *figure = axis_web_analysis_count_label(count);

	if (count > 0)
		item(list, label, figure, href, rest);
}

HtmxNode *
axis_web_analysis_coverage(
	const AxisAnalysisSnapshot *snapshot,
	guint                       scope
){
	JsonObject *cov = snapshot->coverage;
	HtmxNode *card = axis_web_storage_card("What these figures could not see");
	HtmxUl *list = htmx_ul_new();
	AxisLibraryFilter filter;
	JsonArray *array;
	guint lines = 0;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(card), "coverage");
	htmx_element_add_class(HTMX_ELEMENT(list), "coverage-list");

	if (scope & AXIS_ANALYSIS_COVER_SCAN) {
		array = json_object_has_member(cov, "roots_unscanned") ?
			json_object_get_array_member(cov, "roots_unscanned") : NULL;
		if (array != NULL && json_array_get_length(array) > 0) {
			g_autoptr(GString) names = g_string_new(NULL);
			g_autofree gchar *count = g_strdup_printf("%u %s", json_array_get_length(array),
				json_array_get_length(array) == 1 ? "library folder" : "library folders");
			g_autofree gchar *rest = NULL;

			for (i = 0; i < json_array_get_length(array) && i < 8; i++) {
				JsonObject *root = json_array_get_object_element(array, i);

				g_string_append_printf(names, "%s%s", i > 0 ? ", " : "",
					json_object_get_string_member_with_default(root, "name", ""));
			}
			rest = g_strdup_printf(" (%s%s): what %s holds may be missing from every figure.", names->str,
				json_array_get_length(array) > 8 ? ", ..." : "", json_array_get_length(array) == 1 ? "it" : "they");
			item(HTMX_NODE(list), "Never completely scanned", count, "/library", rest);
			lines++;
		}
		array = json_object_has_member(cov, "volumes_unavailable") ?
			json_object_get_array_member(cov, "volumes_unavailable") : NULL;
		for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
			JsonObject *volume = json_array_get_object_element(array, i);
			gint64 id = json_object_get_int_member_with_default(volume, "id", 0);
			gint64 paths = json_object_get_int_member_with_default(volume, "paths", 0);
			gint64 bytes = json_object_get_int_member_with_default(volume, "bytes", 0);
			g_autofree gchar *size = axis_web_browser_format_size((guint64)bytes);
			g_autofree gchar *label = axis_web_analysis_count_label(paths);
			g_autofree gchar *href = NULL;
			g_autofree gchar *rest = NULL;
			AxisVolumeState vstate;

			axis_library_filter_init(&filter);
			filter.volume_id = id;
			href = axis_web_analysis_files_href(&filter);
			axis_library_filter_clear(&filter);
			if (!axis_volume_state_from_string(json_object_get_string_member_with_default(volume, "state", ""),
				&vstate))
				vstate = AXIS_VOLUME_OFFLINE;
			rest = g_strdup_printf(" (%s) on disk %s, which is %s. They are counted as Axis last saw them, "
				"and totals do not change while the disk is offline.", size,
				json_object_get_string_member_with_default(volume, "name", ""),
				axis_volume_state_label(vstate));
			item(HTMX_NODE(list), "Disk offline", label, href, rest);
			lines++;
		}
		axis_library_filter_init(&filter);
		filter.state = "suspected";
		if (axis_analysis_extra_int(cov, "suspected_paths") > 0) {
			item_files(HTMX_NODE(list), "Not found by the last complete scan (suspected)",
				axis_analysis_extra_int(cov, "suspected_paths"), ", in no total.", &filter);
			lines++;
		}
		filter.state = "missing";
		if (axis_analysis_extra_int(cov, "missing_paths") > 0) {
			item_files(HTMX_NODE(list), "Missing (not found again by a later complete scan)",
				axis_analysis_extra_int(cov, "missing_paths"), ", in no total.", &filter);
			lines++;
		}
		axis_library_filter_clear(&filter);
	}

	if (scope & AXIS_ANALYSIS_COVER_PROBE) {
		axis_library_filter_init(&filter);
		filter.probe = "none";
		if (axis_analysis_extra_int(cov, "not_probed") > 0) {
			item_files(HTMX_NODE(list), "Not read yet (video and audio)",
				axis_analysis_extra_int(cov, "not_probed"),
				", left out of every figure that needs what is inside a file.", &filter);
			lines++;
		}
		filter.probe = "failed";
		if (axis_analysis_extra_int(cov, "probe_failed") > 0) {
			item_files(HTMX_NODE(list), "Could not be read",axis_analysis_extra_int(cov, "probe_failed"),
				", left out the same way.", &filter);
			lines++;
		}
		filter.probe = "timeout";
		if (axis_analysis_extra_int(cov, "probe_timeout") > 0) {
			item_files(HTMX_NODE(list), "Reading timed out",axis_analysis_extra_int(cov, "probe_timeout"),
				", left out the same way.", &filter);
			lines++;
		}
		filter.probe = "ok";
		filter.changed = 1;
		if (axis_analysis_extra_int(cov, "probe_stale") > 0) {
			item_files(HTMX_NODE(list), "Changed since they were read",
				axis_analysis_extra_int(cov, "probe_stale"),
				", what is known of them may describe older content.", &filter);
			lines++;
		}
		axis_library_filter_clear(&filter);
	}

	if (scope & AXIS_ANALYSIS_COVER_HDR) {
		axis_library_filter_init(&filter);
		filter.hdr = "uncertain";
		if (axis_analysis_extra_int(cov, "hdr_uncertain") > 0) {
			item_files(HTMX_NODE(list), "HDR class uncertain", axis_analysis_extra_int(cov, "hdr_uncertain"),
				", counted under the class the probe named, and not to be treated as safe to convert.",
				&filter);
			lines++;
		}
		axis_library_filter_clear(&filter);
	}

	if (lines == 0) {
		g_autofree gchar *text = g_strdup_printf("Nothing is left out of the figures on this page%s%s.",
			(scope & AXIS_ANALYSIS_COVER_SCAN) ? ": every library folder has been completely scanned, every disk was "
				"online and no file was missing" : "",
			(scope & AXIS_ANALYSIS_COVER_PROBE) ? ", and the details of every video and audio file were read" : "");

		BROWSER_ADD(card, axis_web_storage_text("help", text));
		g_object_unref(list);
	} else {
		BROWSER_ADD(card, list);
		BROWSER_ADD(card, axis_web_storage_text("help", "Files the last complete scan did not find, and files "
			"on disks that are offline, are never dropped silently: they are listed here."));
	}
	return card;
}
