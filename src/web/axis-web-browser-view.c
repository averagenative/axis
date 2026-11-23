/*
 * axis-web-browser-view.c - pieces of the Library pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A name from a filesystem is shown only through
 * axis_library_name_display(), as text content inside <bdi> (which
 * htmx-glib escapes, and which keeps a name from reordering what is
 * around it); a link carries a row id and nothing else.  Text that came
 * out of a media file (codec and container names) was cleaned when it was
 * stored and is shown the same way.
 */

#include "web/axis-web-browser-private.h"

#include <string.h>

#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"
#include "probe/axis-probe-model.h"
#include "web/axis-web-text.h"

HtmxNode *
axis_web_browser_link(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	return HTMX_NODE(link);
}

HtmxNode *
axis_web_browser_facts_new(void)
{
	HtmxDl *list = htmx_dl_new();

	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	return HTMX_NODE(list);
}

void
axis_web_browser_fact(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
){
	BROWSER_ADD(list, htmx_dt_new_with_text(label));
	BROWSER_ADD(list, htmx_dd_new_with_text(text != NULL ? text : ""));
}

void
axis_web_browser_fact_node(
	HtmxNode    *list,
	const gchar *label,
	HtmxNode    *value
){
	HtmxDd *dd = htmx_dd_new();

	BROWSER_ADD(list, htmx_dt_new_with_text(label));
	BROWSER_ADD(dd, value);
	BROWSER_ADD(list, dd);
}

HtmxNode *
axis_web_browser_badge(
	const gchar *text,
	const gchar *css
){
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "badge");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), css);
	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

HtmxNode *
axis_web_browser_warning(const gchar *text)
{
	HtmxP *notice = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
	htmx_element_add_class(HTMX_ELEMENT(notice), "notice-warning");
	htmx_element_set_attribute(HTMX_ELEMENT(notice), "role", "alert");
	htmx_node_set_text_content(HTMX_NODE(notice), text);
	return HTMX_NODE(notice);
}

HtmxNode *
axis_web_browser_name_node(const gchar *stored)
{
	HtmxBdi *bdi = htmx_bdi_new();
	g_autofree gchar *shown = axis_library_name_display(stored);

	BROWSER_ADD(bdi, axis_web_breakable(shown));
	if (stored != NULL && axis_library_name_is_invalid(stored)) {
		HtmxSpan *span = htmx_span_new();

		BROWSER_ADD(span, bdi);
		BROWSER_ADD(span, axis_web_browser_badge("not valid UTF-8", "badge-note"));
		return HTMX_NODE(span);
	}
	return HTMX_NODE(bdi);
}

gchar *
axis_web_browser_format_size(guint64 bytes)
{
	return axis_size_format(bytes);
}

gchar *
axis_web_browser_format_count(guint64 value)
{
	return axis_web_format_count(value);
}

/* ── Scan progress ──────────────────────────────────────────────── */

HtmxNode *
axis_web_browser_progress_block(const AxisRootOverview *root)
{
	HtmxDiv *block = htmx_div_new();
	g_autofree gchar *id = g_strdup_printf("root-%" G_GINT64_FORMAT "-progress", root->root_id);

	htmx_element_set_id(HTMX_ELEMENT(block), id);
	htmx_element_add_class(HTMX_ELEMENT(block), "scan-progress");

	if (root->running) {
		g_autofree gchar *files = axis_web_browser_format_count(root->run_files);
		g_autofree gchar *dirs = axis_web_browser_format_count(root->run_dirs);
		g_autofree gchar *text = g_strdup_printf("Scanning: %s %s in %s %s", files,
			root->run_files == 1 ? "file" : "files", dirs, root->run_dirs == 1 ? "folder" : "folders");
		HtmxProgress *bar;

		if (root->run_previous > 0) {
			gdouble fraction = MIN(0.99, (gdouble)root->run_files / (gdouble)root->run_previous);

			bar = htmx_progress_new_with_value(fraction, 1.0);
			BROWSER_ADD(block, bar);
		} else {
			/* No earlier scan to measure against: a bar that only moves. */
			bar = htmx_progress_new();
			BROWSER_ADD(block, bar);
		}
		BROWSER_ADD(block, axis_web_storage_text("job-message", text));
	} else if (root->requested_us > 0) {
		BROWSER_ADD(block, axis_web_storage_text("job-message",
			"A scan has been requested and waits for its turn."));
	}

	return HTMX_NODE(block);
}

/* ── Files ──────────────────────────────────────────────────────── */

static HtmxNode *
text_in_bdi(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text);
	return HTMX_NODE(bdi);
}

static const gchar *
kind_label(const gchar *kind)
{
	AxisFileKind parsed;

	if (kind != NULL && axis_file_kind_from_string(kind, &parsed))
		return axis_file_kind_label_one(parsed);
	return "Other";
}

static gchar *
duration_text(gint64 ms)
{
	gint64 seconds;

	if (ms < 0)
		return g_strdup("");
	seconds = ms / 1000;
	if (seconds >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
			seconds / 3600, (seconds % 3600) / 60, seconds % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60);
}

/* What is known of the contents, as a cell. */
static void
format_cell(
	HtmxNode             *row,
	const AxisBrowseFile *file
){
	HtmxTd *td = htmx_td_new();
	gboolean media = g_strcmp0(file->kind, "video") == 0 || g_strcmp0(file->kind, "audio") == 0;

	if (file->probe_status == NULL) {
		if (media)
			BROWSER_ADD(td, axis_web_browser_badge("Not read yet", "badge-note"));
	} else if (g_strcmp0(file->probe_status, "ok") != 0) {
		BROWSER_ADD(td, axis_web_browser_badge(g_strcmp0(file->probe_status, "timeout") == 0 ?
			"Reading timed out" : "Could not be read", "badge-failed"));
	} else {
		g_autoptr(GString) text = g_string_new(NULL);
		AxisHdrFormat hdr = AXIS_HDR_NONE;

		if (file->video_codec != NULL) {
			g_string_append(text, file->video_codec);
			if (file->resolution != NULL)
				g_string_append_printf(text, " %s", axis_probe_resolution_label(file->resolution));
		}
		if (file->hdr != NULL && axis_hdr_format_from_string(file->hdr, &hdr) && hdr != AXIS_HDR_SDR)
			g_string_append_printf(text, "%s%s", text->len > 0 ? " " : "", axis_hdr_format_label(hdr));
		if (file->audio_codec != NULL)
			g_string_append_printf(text, "%s%s", text->len > 0 ? ", " : "", file->audio_codec);
		if (text->len > 0)
			BROWSER_ADD(td, text_in_bdi(text->str));
		if (file->hdr_uncertain)
			BROWSER_ADD(td, axis_web_browser_badge("HDR uncertain", "badge-warn"));
	}
	BROWSER_ADD(row, td);
}

HtmxNode *
axis_web_browser_file_table(
	GPtrArray *files,
	gboolean   show_directory
){
	static const AxisWebColumn columns[] = {
		{ "Name", AXIS_WEB_COL_NAME },
		{ "Type", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Modified", AXIS_WEB_COL_DATE },
		{ "Contents", AXIS_WEB_COL_WIDE },
		{ "Length", AXIS_WEB_COL_NUMBER },
		{ "", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("files", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(table), "file-table");
	for (i = 0; i < files->len; i++) {
		const AxisBrowseFile *file = g_ptr_array_index(files, i);
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, file->id);
		g_autofree gchar *size = axis_web_browser_format_size((guint64)file->size);
		g_autofree gchar *when = axis_web_storage_format_time(file->mtime_us, "unknown");
		g_autofree gchar *length = duration_text(file->duration_ms);
		const gchar *slash = strrchr(file->rel_path, '/');
		g_autofree gchar *directory = slash != NULL ? g_strndup(file->rel_path,
			(gsize)(slash - file->rel_path)) : NULL;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *flags = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(name), "scan-path");
		BROWSER_ADD(link, axis_web_browser_name_node(slash != NULL ? slash + 1 : file->rel_path));
		BROWSER_ADD(name, link);
		if (show_directory && directory != NULL) {
			HtmxDiv *dir = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(dir), "file-dir");
			BROWSER_ADD(dir, axis_web_browser_name_node(directory));
			BROWSER_ADD(name, dir);
		}
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, htmx_td_new_with_text(kind_label(file->kind)));
		BROWSER_ADD(row, htmx_td_new_with_text(size));
		BROWSER_ADD(row, htmx_td_new_with_text(when));
		format_cell(HTMX_NODE(row), file);
		BROWSER_ADD(row, htmx_td_new_with_text(length));
		if (file->pinned)
			BROWSER_ADD(flags, axis_web_browser_badge("Pinned", "badge-note"));
		if (g_strcmp0(file->state, "suspected") == 0)
			BROWSER_ADD(flags, axis_web_browser_badge("Not found by the last scan", "badge-warn"));
		else if (g_strcmp0(file->state, "missing") == 0)
			BROWSER_ADD(flags, axis_web_browser_badge("Missing", "badge-failed"));
		BROWSER_ADD(row, flags);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	return table;
}

HtmxNode *
axis_web_browser_pager(
	const gchar *first_href,
	const gchar *next_href
){
	HtmxDiv *pager = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
	if (first_href != NULL)
		BROWSER_ADD(pager, axis_web_browser_link(first_href, "First page"));
	if (next_href != NULL)
		BROWSER_ADD(pager, axis_web_browser_link(next_href, "Next page"));
	return HTMX_NODE(pager);
}
