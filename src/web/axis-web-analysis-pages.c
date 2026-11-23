/*
 * axis-web-analysis-pages.c - the Analysis pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every page reads one snapshot (axis-analysis.h) and says when it was
 * taken.  Every count is a link to the file list of the files it counts,
 * built from the same conditions the refresh used; a link is only made
 * when the list's own parser accepts its query.  A name from the library
 * (a path, a root's or volume's name, a codec) is text content inside
 * <bdi>, never part of a link or an attribute: links carry ids.
 */

#include "web/axis-web-analysis-private.h"

#include <stdlib.h>
#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-analysis-rank.h"
#include "library/axis-analysis-rules.h"
#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"
#include "probe/axis-probe-model.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

typedef enum
{
	PAGE_OVERVIEW,
	PAGE_SPACE,
	PAGE_COMPOSITION,
	PAGE_LARGEST,
	PAGE_DUPLICATES,
	PAGE_SPLIT,
	PAGE_LEFTOVERS
} PageId;

static const struct
{
	const gchar *path;
	const gchar *title;
	const gchar *const *reports;
} page_table[] = {
	{ "/analysis", "Disk usage",
	  (const gchar *const[]) { "overall", "space_volume", "space_root", "space_type", NULL } },
	{ "/analysis/space", "Space by disk, library folder and type",
	  (const gchar *const[]) { "overall", "space_volume", "space_root", "space_type", "space_kind", NULL } },
	{ "/analysis/composition", "File formats: codecs, resolutions and bit rates",
	  (const gchar *const[]) { "overall", "space_volume", "space_root", "comp_vcodec", "comp_res", "comp_hdr",
		"comp_container", "comp_acodec", "bpp", "large", "large_list", NULL } },
	{ "/analysis/largest", "Largest files and folders",
	  (const gchar *const[]) { "space_volume", "space_root", "largest_files", "largest_dirs", NULL } },
	{ "/analysis/duplicates", "Likely duplicate files",
	  (const gchar *const[]) { "space_volume", "space_root", "dup_content", "dup_title", NULL } },
	{ "/analysis/split", "Series and albums split across disks",
	  (const gchar *const[]) { "space_volume", "space_root", "split", NULL } },
	{ "/analysis/leftovers", "Leftovers in the library",
	  (const gchar *const[]) { "space_volume", "space_root", "leftover", NULL } }
};

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
bad_request(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
}

static HtmxResponse *
fragment_response(HtmxNode *node)
{
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *markup = NULL;

	axis_web_table_apply_roles(node);
	markup = htmx_element_render(HTMX_ELEMENT(node));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Vary", "HX-Request, HX-History-Restore-Request");
	g_object_unref(node);
	return response;
}

static JsonArray *
items_of(const AxisAnalysisFact *fact)
{
	if (fact == NULL || !json_object_has_member(fact->extra, "items"))
		return NULL;
	return json_object_get_array_member(fact->extra, "items");
}

static gint64
oi(
	JsonObject  *object,
	const gchar *member
){
	return json_object_get_int_member_with_default(object, member, 0);
}

static const gchar *
os(
	JsonObject  *object,
	const gchar *member
){
	return json_object_get_string_member_with_default(object, member, "");
}

static gboolean
ob(
	JsonObject  *object,
	const gchar *member
){
	return json_object_get_boolean_member_with_default(object, member, FALSE);
}

static HtmxNode *
bdi_text(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text);
	return HTMX_NODE(bdi);
}

static gchar *
size_text(gint64 bytes)
{
	return axis_web_browser_format_size((guint64)MAX(bytes, 0));
}

static gchar *
count_text(gint64 count)
{
	return axis_web_browser_format_count((guint64)MAX(count, 0));
}

/* Bits per pixel as "0.250", whatever the locale. */
static gchar *
bpp_text(gdouble value)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value);
	return g_strdup(buffer);
}

static gchar *
milli_text(gint64 milli)
{
	return g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT, milli / 1000, milli % 1000);
}

static HtmxNode *
count_cell(
	gint64       count,
	const gchar *href
){
	g_autofree gchar *text = count_text(count);
	HtmxTd *cell = htmx_td_new();

	htmx_element_add_class(HTMX_ELEMENT(cell), "num");
	BROWSER_ADD(cell, axis_web_analysis_figure(text, href));
	return HTMX_NODE(cell);
}

static HtmxNode *
text_cell(
	const gchar *css,
	const gchar *text
){
	HtmxTd *cell = htmx_td_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(cell), css);
	htmx_node_set_text_content(HTMX_NODE(cell), text);
	return HTMX_NODE(cell);
}

static HtmxNode *
bytes_cell(gint64 bytes)
{
	g_autofree gchar *text = size_text(bytes);

	return text_cell("num", text);
}

/* Link to the files of a volume, root, type or kind. */
static gchar *
href_volume(gint64 id)
{
	AxisLibraryFilter filter;
	gchar *href;

	axis_library_filter_init(&filter);
	filter.volume_id = id;
	href = axis_web_analysis_files_href(&filter);
	axis_library_filter_clear(&filter);
	return href;
}

static gchar *
href_root(gint64 id)
{
	AxisLibraryFilter filter;
	gchar *href;

	axis_library_filter_init(&filter);
	filter.root_id = id;
	href = axis_web_analysis_files_href(&filter);
	axis_library_filter_clear(&filter);
	return href;
}

/* A note under a table: paths that are hard links of files already
 * counted. */
static HtmxNode *
linked_note(gint64 linked)
{
	g_autofree gchar *text = NULL;

	if (linked <= 0)
		return NULL;
	text = g_strdup_printf("%" G_GINT64_FORMAT " %s hard links of files that are counted already: the "
		"number of files above counts every path (it is the length of the list it links to), the size "
		"counts each file once.", linked, linked == 1 ? "path is one of the" : "paths are");
	return axis_web_storage_text("help", text);
}

static gint
compare_by_bytes(
	gconstpointer a,
	gconstpointer b
){
	const AxisAnalysisFact *x = *(AxisAnalysisFact *const *)a;
	const AxisAnalysisFact *y = *(AxisAnalysisFact *const *)b;

	if (x->bytes != y->bytes)
		return x->bytes > y->bytes ? -1 : 1;
	return g_strcmp0(x->dim1, y->dim1);
}

static gint
compare_by_id(
	gconstpointer a,
	gconstpointer b
){
	const AxisAnalysisFact *x = *(AxisAnalysisFact *const *)a;
	const AxisAnalysisFact *y = *(AxisAnalysisFact *const *)b;
	gint64 nx = g_ascii_strtoll(x->dim1, NULL, 10);
	gint64 ny = g_ascii_strtoll(y->dim1, NULL, 10);

	return nx < ny ? -1 : nx > ny;
}

/* The facts of a report in an order of @compare; the array borrows. */
static GPtrArray *
sorted_facts(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	GCompareFunc                compare
){
	const GPtrArray *facts = axis_analysis_snapshot_facts(snapshot, report);
	GPtrArray *copy = g_ptr_array_new();
	guint i;

	for (i = 0; facts != NULL && i < facts->len; i++)
		g_ptr_array_add(copy, g_ptr_array_index((GPtrArray *)facts, i));
	g_ptr_array_sort(copy, compare);
	return copy;
}

static gboolean
volume_usable(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      id
){
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, "space_volume", key, NULL);

	return fact == NULL || ob(fact->extra, "usable");
}

static HtmxNode *
away_badge(void)
{
	return axis_web_browser_badge("Disk offline: last known", "badge-failed");
}

/* A file's name as a link to its page, with the folder it is in. */
static HtmxNode *
file_cell(
	gint64       id,
	const gchar *stored
){
	g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, id);
	g_autofree gchar *directory = NULL;
	const gchar *slash = strrchr(stored, '/');
	HtmxTd *cell = htmx_td_new();
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_element_add_class(HTMX_ELEMENT(cell), "scan-path");
	BROWSER_ADD(link, axis_web_browser_name_node(slash != NULL ? slash + 1 : stored));
	BROWSER_ADD(cell, link);
	if (slash != NULL) {
		HtmxDiv *dir = htmx_div_new();

		directory = g_strndup(stored, (gsize)(slash - stored));
		htmx_element_add_class(HTMX_ELEMENT(dir), "file-dir");
		BROWSER_ADD(dir, axis_web_browser_name_node(directory));
		BROWSER_ADD(cell, dir);
	}
	return HTMX_NODE(cell);
}

static HtmxNode *
where_cell(
	const AxisAnalysisSnapshot *snapshot,
	gint64                      root_id,
	gint64                      volume_id,
	gboolean                    usable
){
	HtmxTd *cell = htmx_td_new();
	g_autofree gchar *text = g_strdup_printf("%s / %s", axis_web_analysis_volume_name(snapshot, volume_id),
		axis_web_analysis_root_name(snapshot, root_id));

	BROWSER_ADD(cell, bdi_text(text));
	if (!usable)
		BROWSER_ADD(cell, away_badge());
	return HTMX_NODE(cell);
}

/* ── Work ───────────────────────────────────────────────────────── */

typedef struct
{
	PageId                page;
	AxisAnalysisSnapshot *snapshot;
	GPtrArray            *history;
	AxisAnalysisStatus    status;
	GError               *error;
	gboolean              fragment;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	axis_analysis_snapshot_free(work->snapshot);
	g_clear_pointer(&work->history, g_ptr_array_unref);
	axis_analysis_status_clear(&work->status);
	g_clear_error(&work->error);
	g_free(work);
}

static void
page_run(
	AxisWebAnalysis *self,
	gpointer         data
){
	PageWork *work = data;

	if (!axis_analysis_get_status(self->analysis, &work->status, &work->error))
		return;
	if (work->fragment)
		return;
	if (!axis_analysis_load_snapshot(self->analysis, 0, page_table[work->page].reports, &work->snapshot,
	        &work->error))
		return;
	if (work->page == PAGE_OVERVIEW && work->snapshot != NULL)
		axis_analysis_load_history(self->analysis, 400, &work->history, &work->error);
}

/* The page's start: heading, the reports' navigation, when the figures are
 * from. */
#define REFRESH_PLACE "analysis-refresh"

/* With @notice, the refresh's notice (@message, nullable) is drawn in a block
 * around the header, where "Refresh now" is, or at the top. */
static HtmxDiv *
page_start_notice(
	HtmxRequest    *request,
	const PageWork *work,
	const gchar    *path,
	const gchar    *title,
	AxisWebNotice  *notice,
	const gchar    *message
){
	HtmxDiv *content = htmx_div_new();
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *header;
	HtmxDiv *block;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	BROWSER_ADD(content, heading);
	BROWSER_ADD(content, axis_web_page_intro("What is taking the space on your disks: by disk, library folder, media type, "
		"codec and resolution, with the largest files and folders, the likely duplicates and the leftovers. The "
		"figures come from Axis's last count of your files, and nothing here changes a file."));
	BROWSER_ADD(content, axis_web_analysis_tabs(path));
	if (notice != NULL)
		axis_web_notice_begin(notice, request, FALSE, g_strdup(message), HTMX_NODE(content));
	header = work->snapshot == NULL ? axis_web_analysis_empty(request, &work->status) :
		axis_web_analysis_snapshot_header(request, work->snapshot, &work->status);
	if (notice == NULL) {
		BROWSER_ADD(content, header);
		return content;
	}
	block = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(block), REFRESH_PLACE);
	axis_web_notice_put(notice, HTMX_NODE(block), REFRESH_PLACE);
	BROWSER_ADD(block, header);
	BROWSER_ADD(content, block);
	return content;
}

static HtmxDiv *
page_start(
	HtmxRequest    *request,
	const PageWork *work,
	const gchar    *path,
	const gchar    *title
){
	return page_start_notice(request, work, path, title, NULL, NULL);
}

/* ── Overview ───────────────────────────────────────────────────── */

static HtmxNode *
volume_table(
	const AxisAnalysisSnapshot *snapshot,
	GPtrArray                  *volumes
){
	static const AxisWebColumn columns[] = {
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Library size", AXIS_WEB_COL_SIZE },
		{ "Used on the disk", AXIS_WEB_COL_SIZE },
		{ "Free", AXIS_WEB_COL_SIZE },
		{ "Full", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("analysis-volumes", columns);
	HtmxTbody *body = htmx_tbody_new();
	gint64 linked = 0;
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(volumes, i);
		gint64 id = g_ascii_strtoll(fact->dim1, NULL, 10);
		gint64 total = json_object_has_member(fact->extra, "total_bytes") &&
			!json_node_is_null(json_object_get_member(fact->extra, "total_bytes")) ?
			oi(fact->extra, "total_bytes") : -1;
		gint64 free_bytes = json_object_has_member(fact->extra, "free_bytes") &&
			!json_node_is_null(json_object_get_member(fact->extra, "free_bytes")) ?
			oi(fact->extra, "free_bytes") : -1;
		g_autofree gchar *href = href_volume(id);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *state = htmx_td_new();
		HtmxTd *full = htmx_td_new();
		gboolean usable = ob(fact->extra, "usable");

		linked += oi(fact->extra, "linked_paths");
		BROWSER_ADD(name, bdi_text(os(fact->extra, "name")));
		BROWSER_ADD(row, name);
		BROWSER_ADD(state, axis_web_browser_badge(usable ? "Online" : "Offline: last known",
			usable ? NULL : "badge-failed"));
		BROWSER_ADD(row, state);
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, bytes_cell(fact->bytes));
		if (total >= 0 && free_bytes >= 0) {
			g_autofree gchar *used = size_text(total - free_bytes);
			g_autofree gchar *free_text = size_text(free_bytes);
			g_autofree gchar *percent = g_strdup_printf("%" G_GINT64_FORMAT "%%",
				total > 0 ? (total - free_bytes) * 100 / total : 0);

			BROWSER_ADD(row, text_cell("num", used));
			BROWSER_ADD(row, text_cell("num", free_text));
			if (total > 0)
				BROWSER_ADD(full, htmx_progress_new_with_value((gdouble)((total - free_bytes) * 100 / total), 100.0));
			BROWSER_ADD(full, bdi_text(percent));
		} else {
			BROWSER_ADD(row, text_cell("num", "unknown"));
			BROWSER_ADD(row, text_cell("num", "unknown"));
			BROWSER_ADD(full, bdi_text("unknown"));
		}
		BROWSER_ADD(row, full);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	if (volumes->len == 0)
		return table;
	{
		HtmxDiv *box = htmx_div_new();
		HtmxNode *note = linked_note(linked);

		BROWSER_ADD(box, table);
		if (note != NULL)
			BROWSER_ADD(box, note);
		return HTMX_NODE(box);
	}
}

static HtmxNode *
volume_chart(
	const AxisAnalysisSnapshot *snapshot,
	GPtrArray                  *volumes
){
	g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(volumes, i);
		g_autofree gchar *href = href_volume(g_ascii_strtoll(fact->dim1, NULL, 10));
		g_autofree gchar *text = size_text(fact->bytes);

		g_ptr_array_add(bars, axis_chart_bar_new(os(fact->extra, "name"), fact->bytes, text, href,
			!ob(fact->extra, "usable")));
	}
	return axis_web_analysis_bar_chart("Library size by disk", bars);
}

static const gchar *
type_label(const gchar *media_type)
{
	AxisMediaType parsed;

	if (media_type != NULL && axis_media_type_from_string(media_type, &parsed))
		return axis_media_type_label(parsed);
	return "Other";
}

static HtmxNode *
type_table(
	GPtrArray *types,
	gint64    *linked_out
){
	static const AxisWebColumn columns[] = {
		{ "Media type", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("breakdown", columns);
	HtmxTbody *body = htmx_tbody_new();
	gint64 linked = 0;
	guint i;

	for (i = 0; i < types->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(types, i);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		HtmxTr *row = htmx_tr_new();

		axis_library_filter_init(&filter);
		filter.type = fact->dim1;
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		linked += oi(fact->extra, "linked_paths");
		BROWSER_ADD(row, text_cell(NULL, type_label(fact->dim1)));
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, bytes_cell(fact->bytes));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	*linked_out = linked;
	return table;
}

static HtmxNode *
type_chart(GPtrArray *types)
{
	g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
	guint i;

	for (i = 0; i < types->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(types, i);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		g_autofree gchar *text = size_text(fact->bytes);

		axis_library_filter_init(&filter);
		filter.type = fact->dim1;
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		g_ptr_array_add(bars, axis_chart_bar_new(type_label(fact->dim1), fact->bytes, text, href, FALSE));
	}
	return axis_web_analysis_bar_chart("Library size by media type", bars);
}

static HtmxNode *
growth_card(GPtrArray *history)
{
	static const AxisWebColumn columns[] = {
		{ "Date", AXIS_WEB_COL_DATE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Library size", AXIS_WEB_COL_SIZE },
		{ "Used on the disks", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Library size over time");
	HtmxNode *table = axis_web_table_new("growth", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint shown = 0;
	guint i;

	if (history == NULL || history->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No history yet: one report is kept for every day, so the line starts on the second day."));
		return card;
	}
	BROWSER_ADD(card, axis_web_storage_text("help", history->len < 2 ?
		"Only one report exists so far. The size of the library over time appears once there are more: "
		"one is kept for every day." :
		"The size of the library (every file once) at each report that was kept: the last one of each day "
		"for a long time, others for a few days. These are totals from the past; the files they counted "
		"as they were then are not kept, so they link to no list."));
	BROWSER_ADD(card, axis_web_analysis_growth_chart("Library size over time", history));

	for (i = history->len; i > 0 && shown < 14; i--, shown++) {
		const AxisAnalysisPoint *point = g_ptr_array_index(history, i - 1);
		g_autofree gchar *when = axis_web_storage_format_time(point->taken_us, "unknown");
		g_autofree gchar *used = point->volumes_used >= 0 ? size_text(point->volumes_used) : g_strdup("unknown");
		HtmxTr *row = htmx_tr_new();

		BROWSER_ADD(row, text_cell(NULL, when));
		BROWSER_ADD(row, count_cell(point->paths, NULL));
		BROWSER_ADD(row, bytes_cell(point->bytes));
		BROWSER_ADD(row, text_cell("num", used));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	return card;
}

static HtmxNode *
reports_card(void)
{
	HtmxNode *card = axis_web_storage_card("Other Disk usage reports");
	HtmxUl *list = htmx_ul_new();
	static const struct
	{
		const gchar *path;
		const gchar *label;
		const gchar *help;
	} entries[] = {
		{ "/analysis/space", "Space by disk, library folder and type", "by disk, library folder, media type and kind of file" },
		{ "/analysis/composition", "File formats", "codecs, resolutions, HDR, containers, bit rates, and what is "
			"large for its class" },
		{ "/analysis/largest", "Largest files and folders", "where the space went" },
		{ "/analysis/duplicates", "Likely duplicates", "the same content, and the same title in more than one place" },
		{ "/analysis/split", "Split across disks", "series and albums on more than one disk" },
		{ "/analysis/leftovers", "Leftovers in the library", "samples, extras, subtitles without a video, junk" },
		{ NULL, NULL, NULL }
	};
	guint i;

	for (i = 0; entries[i].path != NULL; i++) {
		HtmxLi *item = htmx_li_new();
		HtmxSpan *tail = htmx_span_new();
		g_autofree gchar *text = g_strdup_printf(": %s", entries[i].help);

		BROWSER_ADD(item, axis_web_browser_link(entries[i].path, entries[i].label));
		htmx_node_set_text_content(HTMX_NODE(tail), text);
		BROWSER_ADD(item, tail);
		BROWSER_ADD(list, item);
	}
	BROWSER_ADD(card, list);
	return card;
}

static HtmxResponse *
overview_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	const gchar *done = htmx_request_get_query_param(request, "done");
	AxisWebNotice notice;
	g_autoptr(HtmxDiv) content = page_start_notice(request, work, "/analysis", "Disk usage", &notice,
		g_strcmp0(done, "refresh-requested") == 0 ? "A refresh has been requested. The reports below are replaced "
		"when it is done." : NULL);
	const AxisAnalysisSnapshot *snapshot = work->snapshot;
	const AxisAnalysisFact *overall;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GPtrArray) types = NULL;
	AxisLibraryFilter filter;
	g_autofree gchar *all_href = NULL;
	g_autofree gchar *files = NULL;
	g_autofree gchar *size = NULL;
	HtmxNode *card;
	HtmxNode *facts;
	HtmxNode *note;
	gint64 linked = 0;

	if (snapshot == NULL) {
		axis_web_notice_end(&notice);
		return axis_web_page_respond(request, "Disk usage", HTMX_NODE(content));
	}

	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_ALL));

	overall = axis_analysis_snapshot_fact(snapshot, "overall", "", NULL);
	card = axis_web_storage_card("Size of the whole library");
	facts = axis_web_browser_facts_new();
	axis_library_filter_init(&filter);
	all_href = axis_web_analysis_files_href(&filter);
	axis_library_filter_clear(&filter);
	files = overall != NULL ? count_text(overall->paths) : g_strdup("0");
	size = size_text(overall != NULL ? overall->bytes : 0);
	axis_web_browser_fact_node(facts, "Files", axis_web_analysis_figure(files, all_href));
	axis_web_browser_fact(facts, "Size", size);
	axis_web_browser_fact(facts, "What is counted", "Files that are present; every hard-linked file once in the size");
	BROWSER_ADD(card, facts);
	BROWSER_ADD(content, card);

	volumes = sorted_facts(snapshot, "space_volume", compare_by_id);
	card = axis_web_storage_card("Space by disk");
	if (volumes->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No disk is known yet."));
		BROWSER_ADD(card, axis_web_browser_link("/storage", "Add a disk on the Disks page"));
	} else {
		BROWSER_ADD(card, volume_chart(snapshot, volumes));
		BROWSER_ADD(card, volume_table(snapshot, volumes));
	}
	BROWSER_ADD(content, card);

	types = sorted_facts(snapshot, "space_type", compare_by_bytes);
	card = axis_web_storage_card("Space by media type");
	BROWSER_ADD(card, axis_web_storage_text("help", "The media type is what the library folder a file is in "
		"holds, as set on the Storage page."));
	if (types->len > 0) {
		BROWSER_ADD(card, type_chart(types));
		BROWSER_ADD(card, type_table(types, &linked));
		note = linked_note(linked);
		if (note != NULL)
			BROWSER_ADD(card, note);
	}
	BROWSER_ADD(content, card);

	BROWSER_ADD(content, growth_card(work->history));
	BROWSER_ADD(content, reports_card());

	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Disk usage", HTMX_NODE(content));
}

/* ── Space ──────────────────────────────────────────────────────── */

static HtmxNode *
roots_table(
	const AxisAnalysisSnapshot *snapshot,
	GPtrArray                  *roots
){
	static const AxisWebColumn columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Last complete scan", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("analysis-roots", columns);
	HtmxTbody *body = htmx_tbody_new();
	gint64 linked = 0;
	guint i;

	for (i = 0; i < roots->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(roots, i);
		gint64 id = g_ascii_strtoll(fact->dim1, NULL, 10);
		gint64 volume_id = oi(fact->extra, "volume_id");
		g_autofree gchar *href = href_root(id);
		g_autofree gchar *when = NULL;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *volume = htmx_td_new();
		HtmxTd *scan = htmx_td_new();

		linked += oi(fact->extra, "linked_paths");
		BROWSER_ADD(name, bdi_text(os(fact->extra, "name")));
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, text_cell(NULL, type_label(os(fact->extra, "media_type"))));
		BROWSER_ADD(volume, bdi_text(axis_web_analysis_volume_name(snapshot, volume_id)));
		if (!volume_usable(snapshot, volume_id))
			BROWSER_ADD(volume, away_badge());
		BROWSER_ADD(row, volume);
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, bytes_cell(fact->bytes));
		if (ob(fact->extra, "complete_scan")) {
			when = axis_web_storage_format_time(oi(fact->extra, "last_complete_us"), "unknown");
			htmx_node_set_text_content(HTMX_NODE(scan), when);
		} else {
			BROWSER_ADD(scan, axis_web_browser_badge("Never completely scanned", "badge-failed"));
		}
		BROWSER_ADD(row, scan);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	if (linked > 0) {
		HtmxDiv *box = htmx_div_new();

		BROWSER_ADD(box, table);
		BROWSER_ADD(box, linked_note(linked));
		return HTMX_NODE(box);
	}
	return table;
}

static HtmxNode *
kind_table(
	GPtrArray *kinds,
	HtmxNode **chart
){
	static const AxisWebColumn columns[] = {
		{ "Kind of file", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("breakdown", columns);
	HtmxTbody *body = htmx_tbody_new();
	g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
	guint i;

	for (i = 0; i < kinds->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(kinds, i);
		AxisLibraryFilter filter;
		AxisFileKind kind;
		g_autofree gchar *href = NULL;
		g_autofree gchar *text = size_text(fact->bytes);
		const gchar *label = axis_file_kind_from_string(fact->dim1, &kind) ? axis_file_kind_label(kind) : "Other";
		HtmxTr *row = htmx_tr_new();

		axis_library_filter_init(&filter);
		filter.kind = fact->dim1;
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		BROWSER_ADD(row, text_cell(NULL, label));
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, bytes_cell(fact->bytes));
		BROWSER_ADD(body, row);
		g_ptr_array_add(bars, axis_chart_bar_new(label, fact->bytes, text, href, FALSE));
	}
	BROWSER_ADD(table, body);
	*chart = axis_web_analysis_bar_chart("Library size by kind of file", bars);
	return table;
}

static HtmxResponse *
space_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/space", "Space by disk, library folder and type");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GPtrArray) types = NULL;
	g_autoptr(GPtrArray) kinds = NULL;
	HtmxNode *card;
	HtmxNode *chart = NULL;
	HtmxNode *note;
	gint64 linked = 0;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "Space by disk, library folder and type", HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_SCAN));

	volumes = sorted_facts(snapshot, "space_volume", compare_by_id);
	card = axis_web_storage_card("Space by disk");
	BROWSER_ADD(card, axis_web_storage_text("help", "The size is what the library holds on the disk; the "
		"disk's own figures are as last measured and include everything else on it."));
	if (volumes->len > 0) {
		BROWSER_ADD(card, volume_chart(snapshot, volumes));
		BROWSER_ADD(card, volume_table(snapshot, volumes));
	}
	BROWSER_ADD(content, card);

	roots = sorted_facts(snapshot, "space_root", compare_by_id);
	card = axis_web_storage_card("Space by library folder");
	BROWSER_ADD(card, axis_web_storage_text("help", "A file with several hard links on one disk counts its "
		"size once, in the library folder of the path Axis found first."));
	BROWSER_ADD(card, roots_table(snapshot, roots));
	BROWSER_ADD(content, card);

	types = sorted_facts(snapshot, "space_type", compare_by_bytes);
	card = axis_web_storage_card("Space by media type");
	BROWSER_ADD(card, axis_web_storage_text("help", "The media type is what the library folder a file is in holds "
		"(set on the Storage page), not a guess about the file."));
	if (types->len > 0) {
		BROWSER_ADD(card, type_chart(types));
		BROWSER_ADD(card, type_table(types, &linked));
		note = linked_note(linked);
		if (note != NULL)
			BROWSER_ADD(card, note);
	}
	BROWSER_ADD(content, card);

	kinds = sorted_facts(snapshot, "space_kind", compare_by_bytes);
	card = axis_web_storage_card("Space by kind of file");
	if (kinds->len > 0) {
		HtmxNode *table = kind_table(kinds, &chart);

		BROWSER_ADD(card, chart);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	return axis_web_page_respond(request, "Space by disk, library folder and type", HTMX_NODE(content));
}

/* ── Composition ────────────────────────────────────────────────── */

typedef enum
{
	DIM_VCODEC,
	DIM_RES,
	DIM_HDR,
	DIM_CONTAINER,
	DIM_ACODEC
} Dimension;

/* A filter for one value of a dimension. */
static void
filter_for(
	AxisLibraryFilter *filter,
	Dimension          dimension,
	const gchar       *value
){
	switch (dimension) {
	case DIM_VCODEC:
		filter->video_codec = g_strdup(value);
		break;
	case DIM_RES:
		filter->resolution = value;
		break;
	case DIM_HDR:
		filter->hdr = value;
		break;
	case DIM_CONTAINER:
		filter->container = g_strdup(value);
		break;
	case DIM_ACODEC:
	default:
		filter->audio_codec = g_strdup(value);
		break;
	}
}

static gchar *
dimension_label(
	Dimension    dimension,
	const gchar *value
){
	AxisHdrFormat hdr;

	if (dimension == DIM_RES)
		return g_strdup(axis_probe_resolution_label(value));
	if (dimension == DIM_HDR && axis_hdr_format_from_string(value, &hdr))
		return g_strdup(axis_hdr_format_label(hdr));
	return g_strdup(value);
}

/* @text with its first letter in capitals ("video codec" to "Video codec"). */
static gchar *
capitalised(const gchar *text)
{
	gchar *copy = g_strdup(text);

	if (copy[0] != '\0')
		copy[0] = g_ascii_toupper(copy[0]);
	return copy;
}

static HtmxNode *
breakdown_card(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *heading,
	const gchar                *report,
	Dimension                   dimension
){
	g_autofree gchar *card_title = g_strdup_printf("Space by %s", heading);
	g_autofree gchar *dimension_name = capitalised(heading);
	AxisWebColumn columns[] = {
		{ dimension_name, AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Share of the size", AXIS_WEB_COL_NUMBER },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card(card_title);
	g_autoptr(GPtrArray) facts = sorted_facts(snapshot, report, compare_by_bytes);
	g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
	HtmxNode *table = axis_web_table_new("composition", columns);
	HtmxTbody *body = htmx_tbody_new();
	gint64 total = 0;
	gint64 offline = 0;
	gint64 linked = 0;
	guint i;

	if (facts->len == 0) {
		g_object_unref(table);
		g_object_unref(body);
		BROWSER_ADD(card, axis_web_storage_text("help", "No file with a value here has had its details read."));
		return card;
	}
	for (i = 0; i < facts->len; i++)
		total += ((const AxisAnalysisFact *)g_ptr_array_index(facts, i))->bytes;

	for (i = 0; i < facts->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(facts, i);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		g_autofree gchar *label = dimension_label(dimension, fact->dim1);
		g_autofree gchar *text = size_text(fact->bytes);
		g_autofree gchar *share = g_strdup_printf("%" G_GINT64_FORMAT "%%",
			total > 0 ? fact->bytes * 100 / total : 0);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();

		axis_library_filter_init(&filter);
		filter_for(&filter, dimension, fact->dim1);
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		offline += oi(fact->extra, "offline_paths");
		linked += oi(fact->extra, "linked_paths");
		BROWSER_ADD(name, bdi_text(label));
		BROWSER_ADD(row, name);
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, bytes_cell(fact->bytes));
		BROWSER_ADD(row, text_cell("num", share));
		BROWSER_ADD(body, row);
		g_ptr_array_add(bars, axis_chart_bar_new(label, fact->bytes, text, href, FALSE));
	}
	BROWSER_ADD(table, body);
	{
		g_autofree gchar *title = g_strdup_printf("Size by %s", heading);

		BROWSER_ADD(card, axis_web_analysis_bar_chart(title, bars));
	}
	BROWSER_ADD(card, table);
	if (offline > 0) {
		g_autofree gchar *text = g_strdup_printf(offline == 1 ?
				"%" G_GINT64_FORMAT " of these files is on a disk that is offline; it is counted as Axis last saw it." :
				"%" G_GINT64_FORMAT " of these files are on disks that are offline; they are counted as Axis last "
				"saw them.", offline);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}
	{
		HtmxNode *note = linked_note(linked);

		if (note != NULL)
			BROWSER_ADD(card, note);
	}
	return card;
}

/* Whether a codec has a "large for its class" threshold. */
static gboolean
codec_is_judged(const gchar *codec)
{
	guint i;

	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++) {
		if (g_strcmp0(codec, axis_analysis_large_codecs[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

static gint
compare_bpp_cells(
	gconstpointer a,
	gconstpointer b
){
	const AxisAnalysisFact *x = *(AxisAnalysisFact *const *)a;
	const AxisAnalysisFact *y = *(AxisAnalysisFact *const *)b;
	gint rx = axis_analysis_resolution_rank(x->dim1);
	gint ry = axis_analysis_resolution_rank(y->dim1);

	if (rx != ry)
		return rx > ry ? -1 : 1;
	return g_strcmp0(x->dim2, y->dim2);
}

static HtmxNode *
bpp_card(const AxisAnalysisSnapshot *snapshot)
{
	static const AxisWebColumn columns[] = {
		{ "Resolution", AXIS_WEB_COL_NAME },
		{ "Codec", AXIS_WEB_COL_TEXT },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Median bits per pixel", AXIS_WEB_COL_NUMBER },
		{ "95th percentile (bits per pixel)", AXIS_WEB_COL_NUMBER },
		{ "Files large for their class", AXIS_WEB_COL_NUMBER },
		{ "Size of those files", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Bit rate by resolution class");
	g_autoptr(GPtrArray) cells = sorted_facts(snapshot, "bpp", compare_bpp_cells);
	JsonObject *milli = json_object_has_member(snapshot->config, "large_bpp_milli") ?
		json_object_get_object_member(snapshot->config, "large_bpp_milli") : NULL;
	HtmxNode *table = axis_web_table_new("bitrate", columns);
	HtmxTbody *body = htmx_tbody_new();
	g_autoptr(GString) rules = g_string_new(NULL);
	guint estimated = 0;
	guint i;

	BROWSER_ADD(card, axis_web_storage_text("help", "Video bits per pixel and frame: the video's bit rate "
		"divided by width, height and frame rate. It lets a 4K file and a 720p file be compared. A file is "
		"large for its class when it is at or above the threshold for its codec (set under Settings)."));
	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++) {
		gint64 value = milli != NULL ? json_object_get_int_member_with_default(milli,
			axis_analysis_large_codecs[i], 0) : 0;
		g_autofree gchar *text = milli_text(value);

		g_string_append_printf(rules, "%s%s: %s", i > 0 ? "; " : "", axis_analysis_large_codecs[i], text);
	}
	{
		g_autofree gchar *text = g_strdup_printf("Thresholds used for this report: %s. Other codecs are not "
			"judged, and a file whose bit rate is not known is left out of this section.", rules->str);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}

	if (cells->len == 0) {
		g_object_unref(table);
		g_object_unref(body);
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No video file whose details have been read has a known bit rate."));
		return card;
	}

	for (i = 0; i < cells->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(cells, i);
		const AxisAnalysisFact *large = axis_analysis_snapshot_fact(snapshot, "large", fact->dim1, fact->dim2);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		g_autofree gchar *large_href = NULL;
		g_autofree gchar *median = NULL;
		g_autofree gchar *p95 = NULL;
		HtmxTr *row = htmx_tr_new();
		gint64 threshold = 0;

		axis_library_filter_init(&filter);
		filter.resolution = fact->dim1;
		filter.video_codec = g_strdup(fact->dim2);
		href = axis_web_analysis_files_href(&filter);
		if (large != NULL) {
			threshold = oi(large->extra, "threshold_milli");
			filter.min_bpp = threshold;
			large_href = axis_web_analysis_files_href(&filter);
		}
		axis_library_filter_clear(&filter);
		median = bpp_text(json_object_get_double_member_with_default(fact->extra, "median", 0));
		p95 = bpp_text(json_object_get_double_member_with_default(fact->extra, "p95", 0));
		estimated += (guint)oi(fact->extra, "estimated");

		BROWSER_ADD(row, text_cell(NULL, axis_probe_resolution_label(fact->dim1)));
		BROWSER_ADD(row, text_cell(NULL, fact->dim2));
		BROWSER_ADD(row, count_cell(fact->paths, href));
		BROWSER_ADD(row, text_cell("num", median));
		BROWSER_ADD(row, text_cell("num", p95));
		if (large != NULL) {
			BROWSER_ADD(row, count_cell(large->paths, large_href));
			BROWSER_ADD(row, bytes_cell(large->bytes));
		} else {
			BROWSER_ADD(row, text_cell("num", codec_is_judged(fact->dim2) ? "0" : "not judged"));
			BROWSER_ADD(row, text_cell("num", ""));
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	if (estimated > 0) {
		g_autofree gchar *text = g_strdup_printf(estimated == 1 ?
			"%u of these files has a bit rate worked out from its size and length, because it does not state one." :
			"%u of these files have a bit rate worked out from their size and length, because they do not state "
			"one.", estimated);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}

	/* One histogram per class and codec. */
	for (i = 0; i < cells->len && i < 12; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index(cells, i);
		g_autoptr(GPtrArray) bars = g_ptr_array_new_with_free_func((GDestroyNotify)axis_chart_bar_free);
		g_autofree gchar *title = g_strdup_printf("%s %s: files by bits per pixel",
			axis_probe_resolution_label(fact->dim1), fact->dim2);
		JsonArray *buckets = json_object_has_member(fact->extra, "buckets") ?
			json_object_get_array_member(fact->extra, "buckets") : NULL;
		HtmxNode *section = axis_web_storage_card(title);
		guint k;

		for (k = 0; k < AXIS_ANALYSIS_BPP_BUCKETS; k++) {
			gint64 lo = (gint64)k * AXIS_ANALYSIS_BPP_BUCKET;
			gint64 hi = lo + AXIS_ANALYSIS_BPP_BUCKET;
			gint64 n = 0;
			guint j;
			AxisLibraryFilter filter;
			g_autofree gchar *href = NULL;
			g_autofree gchar *label = NULL;
			g_autofree gchar *count = NULL;
			g_autofree gchar *low = milli_text(lo);
			g_autofree gchar *high = milli_text(hi);

			for (j = 0; buckets != NULL && j < json_array_get_length(buckets); j++) {
				JsonArray *entry = json_array_get_array_element(buckets, j);

				if (entry != NULL && json_array_get_int_element(entry, 0) == (gint64)k)
					n = json_array_get_int_element(entry, 1);
			}
			axis_library_filter_init(&filter);
			filter.resolution = fact->dim1;
			filter.video_codec = g_strdup(fact->dim2);
			filter.min_bpp = lo;
			if (k + 1 < AXIS_ANALYSIS_BPP_BUCKETS)
				filter.max_bpp = hi;
			href = n > 0 ? axis_web_analysis_files_href(&filter) : NULL;
			axis_library_filter_clear(&filter);
			label = k + 1 < AXIS_ANALYSIS_BPP_BUCKETS ? g_strdup_printf("%s to below %s", low, high) :
				g_strdup_printf("%s and more", low);
			count = count_text(n);
			g_ptr_array_add(bars, axis_chart_bar_new(label, n, count, href, FALSE));
		}
		BROWSER_ADD(section, axis_web_analysis_bar_chart(title, bars));
		BROWSER_ADD(card, section);
	}
	return card;
}

static HtmxNode *
large_list_card(const AxisAnalysisSnapshot *snapshot)
{
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Codec", AXIS_WEB_COL_TEXT },
		{ "Resolution", AXIS_WEB_COL_TEXT },
		{ "Bits per pixel", AXIS_WEB_COL_NUMBER },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("The largest files that are large for their class");
	const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, "large_list", "", NULL);
	JsonArray *items = items_of(fact);
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (items == NULL || json_array_get_length(items) == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No file whose details have been read is at or above the threshold of its codec."));
		return card;
	}
	{
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " %s large for their class, a hard-linked "
			"file counted once. The largest (up to %d) are listed. Nothing is changed or suggested here.",
			fact->paths, fact->paths == 1 ? "file is" : "files are", AXIS_ANALYSIS_LIST_ITEMS);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}
	table = axis_web_table_new("large-files", columns);
	body = htmx_tbody_new();
	for (i = 0; i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		g_autofree gchar *bpp = bpp_text(json_object_get_double_member_with_default(item, "bpp", 0));
		HtmxTr *row = htmx_tr_new();
		gint64 volume_id = oi(item, "volume_id");

		BROWSER_ADD(row, file_cell(oi(item, "id"), os(item, "path")));
		BROWSER_ADD(row, text_cell(NULL, os(item, "codec")));
		BROWSER_ADD(row, text_cell(NULL, axis_probe_resolution_label(os(item, "res"))));
		BROWSER_ADD(row, text_cell("num", bpp));
		BROWSER_ADD(row, bytes_cell(oi(item, "size")));
		BROWSER_ADD(row, where_cell(snapshot, oi(item, "root_id"), volume_id, volume_usable(snapshot, volume_id)));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	return card;
}

static HtmxResponse *
composition_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/composition",
		"File formats: codecs, resolutions and bit rates");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;
	HtmxNode *note;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "File formats: codecs, resolutions and bit rates",
			HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_ALL));
	note = axis_web_storage_text("help", "Only files whose details have been read are here, and only those where "
		"that value was found: an audio file has no video codec. Each number is the count of paths in the list it links to.");
	BROWSER_ADD(content, note);

	BROWSER_ADD(content, breakdown_card(snapshot, "video codec", "comp_vcodec", DIM_VCODEC));
	BROWSER_ADD(content, breakdown_card(snapshot, "resolution class", "comp_res", DIM_RES));
	BROWSER_ADD(content, breakdown_card(snapshot, "HDR format", "comp_hdr", DIM_HDR));
	BROWSER_ADD(content, breakdown_card(snapshot, "container", "comp_container", DIM_CONTAINER));
	BROWSER_ADD(content, breakdown_card(snapshot, "audio codec", "comp_acodec", DIM_ACODEC));
	BROWSER_ADD(content, bpp_card(snapshot));
	BROWSER_ADD(content, large_list_card(snapshot));

	return axis_web_page_respond(request, "File formats: codecs, resolutions and bit rates",
		HTMX_NODE(content));
}

/* ── Largest ────────────────────────────────────────────────────── */

static HtmxResponse *
largest_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	static const AxisWebColumn file_columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where", AXIS_WEB_COL_NAME },
		{ "", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	static const AxisWebColumn dir_columns[] = {
		{ "Folder", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/largest", "Largest files and folders");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;
	const AxisAnalysisFact *fact;
	JsonArray *items;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	AxisLibraryFilter filter;
	g_autofree gchar *by_size = NULL;
	guint i;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "Largest files and folders", HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_SCAN));

	axis_library_filter_init(&filter);
	filter.sort = AXIS_LIBRARY_SORT_SIZE;
	filter.descending = TRUE;
	by_size = axis_web_analysis_files_href(&filter);
	axis_library_filter_clear(&filter);

	fact = axis_analysis_snapshot_fact(snapshot, "largest_files", "", NULL);
	items = items_of(fact);
	card = axis_web_storage_card("Largest files");
	if (items == NULL || json_array_get_length(items) == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No file has been found yet: add a library "
			"folder on the Storage page and let its scan finish."));
		BROWSER_ADD(card, axis_web_browser_link("/storage", "Add a library folder on the Disks page"));
	} else {
		HtmxP *line = htmx_p_new();
		g_autofree gchar *text = g_strdup_printf("The %u largest of %" G_GINT64_FORMAT " files. A file with "
			"several hard links appears once. ", json_array_get_length(items), fact->paths);

		htmx_node_set_text_content(HTMX_NODE(line), text);
		BROWSER_ADD(line, axis_web_browser_link(by_size, "All files, largest first"));
		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		BROWSER_ADD(card, line);
		table = axis_web_table_new("largest-files", file_columns);
		body = htmx_tbody_new();
		for (i = 0; i < json_array_get_length(items); i++) {
			JsonObject *item = json_array_get_object_element(items, i);
			gint64 volume_id = oi(item, "volume_id");
			HtmxTr *row = htmx_tr_new();
			HtmxTd *flags = htmx_td_new();

			BROWSER_ADD(row, file_cell(oi(item, "id"), os(item, "path")));
			BROWSER_ADD(row, bytes_cell(oi(item, "size")));
			BROWSER_ADD(row, where_cell(snapshot, oi(item, "root_id"), volume_id, volume_usable(snapshot, volume_id)));
			if (oi(item, "nlink") > 1)
				BROWSER_ADD(flags, axis_web_browser_badge("Hard-linked: counted once", "badge-note"));
			BROWSER_ADD(row, flags);
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	fact = axis_analysis_snapshot_fact(snapshot, "largest_dirs", "", NULL);
	items = items_of(fact);
	card = axis_web_storage_card("Largest folders");
	if (items == NULL || json_array_get_length(items) == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No folders yet: no library folder has been "
			"completely scanned."));
	} else {
		g_autofree gchar *text = g_strdup_printf("The %u largest of %" G_GINT64_FORMAT " folders directly "
			"inside a library folder (a movie, a show, an artist). Folder sizes are those of the Files pages, as "
			"of each library folder's last complete scan: every path counts, so hard-linked files appear in each folder "
			"that holds one.", json_array_get_length(items), fact->paths);

		BROWSER_ADD(card, axis_web_storage_text("help", text));
		table = axis_web_table_new("largest-folders", dir_columns);
		body = htmx_tbody_new();
		for (i = 0; i < json_array_get_length(items); i++) {
			JsonObject *item = json_array_get_object_element(items, i);
			g_autofree gchar *href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, oi(item, "id"));
			gint64 volume_id = oi(item, "volume_id");
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();
			HtmxA *link = htmx_a_new_with_href(href);
			g_autofree gchar *files = count_text(oi(item, "files"));

			htmx_element_add_class(HTMX_ELEMENT(name), "scan-path");
			BROWSER_ADD(link, axis_web_browser_name_node(os(item, "name")));
			BROWSER_ADD(name, link);
			BROWSER_ADD(row, name);
			BROWSER_ADD(row, text_cell("num", files));
			BROWSER_ADD(row, bytes_cell(oi(item, "bytes")));
			BROWSER_ADD(row, where_cell(snapshot, oi(item, "root_id"), volume_id, volume_usable(snapshot, volume_id)));
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
	}
	BROWSER_ADD(content, card);

	return axis_web_page_respond(request, "Largest files and folders", HTMX_NODE(content));
}

/* ── Duplicates ─────────────────────────────────────────────────── */

static gint
compare_numbers(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y;
}

/* The link to exactly the files of a group's members: only when all of the
 * copies are listed and few enough for the list's filter. */
static gchar *
group_href(
	JsonArray *members,
	gint64     copies
){
	AxisLibraryFilter filter;
	g_autofree gint64 *ids = NULL;
	guint n = json_array_get_length(members);
	gchar *href;
	guint i;

	if (n == 0 || (gint64)n != copies || n > AXIS_LIBRARY_FILTER_MAX_IDS)
		return NULL;
	ids = g_new(gint64, n);
	for (i = 0; i < n; i++)
		ids[i] = oi(json_array_get_object_element(members, i), "id");
	qsort(ids, n, sizeof(gint64), compare_numbers);
	axis_library_filter_init(&filter);
	filter.ids = ids;
	filter.n_ids = n;
	href = axis_web_analysis_files_href(&filter);
	/* The filter does not own the array. */
	filter.ids = NULL;
	filter.n_ids = 0;
	axis_library_filter_clear(&filter);
	return href;
}

static gchar *
contents_text(JsonObject *member)
{
	GString *text = g_string_new(NULL);
	AxisHdrFormat hdr;
	const gchar *res = os(member, "res");
	const gchar *hdr_name = os(member, "hdr");

	if (!ob(member, "probed")) {
		g_string_assign(text, "Not read yet");
	} else {
		if (res[0] != '\0')
			g_string_append(text, axis_probe_resolution_label(res));
		if (hdr_name[0] != '\0' && axis_hdr_format_from_string(hdr_name, &hdr) && hdr != AXIS_HDR_SDR)
			g_string_append_printf(text, "%s%s", text->len > 0 ? " " : "", axis_hdr_format_label(hdr));
		if (json_object_has_member(member, "bpp") && !json_node_is_null(json_object_get_member(member, "bpp"))) {
			g_autofree gchar *bpp = bpp_text(json_object_get_double_member_with_default(member, "bpp", 0));

			g_string_append_printf(text, "%s%s bits per pixel", text->len > 0 ? ", " : "", bpp);
		}
		/* Audio: the codec, and whether it keeps everything. */
		if (res[0] == '\0' && os(member, "acodec")[0] != '\0') {
			g_string_append(text, os(member, "acodec"));
			if (json_object_has_member(member, "lossless") &&
			    !json_node_is_null(json_object_get_member(member, "lossless")))
				g_string_append(text, ob(member, "lossless") ? ", lossless" : ", lossy");
		}
		if (text->len == 0)
			g_string_assign(text, "Details read");
	}
	return g_string_free(text, FALSE);
}

static HtmxNode *
members_table(
	const AxisAnalysisSnapshot *snapshot,
	JsonArray                  *members,
	gboolean                    ranked
){
	static const AxisWebColumn columns[] = {
		{ "Best copy", AXIS_WEB_COL_STATE },
		{ "Copy", AXIS_WEB_COL_NAME },
		{ "What is in it", AXIS_WEB_COL_WIDE },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where", AXIS_WEB_COL_NAME },
		{ "Notes", AXIS_WEB_COL_STATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("duplicates", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < json_array_get_length(members); i++) {
		JsonObject *member = json_array_get_object_element(members, i);
		g_autofree gchar *contents = contents_text(member);
		gint64 volume_id = oi(member, "volume_id");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *first = htmx_td_new();
		HtmxTd *flags = htmx_td_new();

		if (ranked && i == 0)
			BROWSER_ADD(first, axis_web_browser_badge("Looks best", "badge-note"));
		BROWSER_ADD(row, first);
		BROWSER_ADD(row, file_cell(oi(member, "id"), os(member, "path")));
		BROWSER_ADD(row, text_cell(NULL, contents));
		BROWSER_ADD(row, bytes_cell(oi(member, "size")));
		BROWSER_ADD(row, where_cell(snapshot, oi(member, "root_id"), volume_id, ob(member, "usable")));
		if (ob(member, "pinned"))
			BROWSER_ADD(flags, axis_web_browser_badge("Pinned: hard-linked or in a library folder that holds downloads", "badge-note"));
		BROWSER_ADD(row, flags);
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	return table;
}

static HtmxNode *
likely_badge(void)
{
	return axis_web_browser_badge("Likely", "badge-note");
}

static HtmxNode *
content_card(
	const AxisAnalysisSnapshot *snapshot,
	const AxisAnalysisFact     *fact
){
	HtmxNode *card = axis_web_storage_card("Likely the same content");
	JsonArray *items = items_of(fact);
	guint i;

	BROWSER_ADD(card, axis_web_storage_text("help", "Files with exactly the same size, the same length and "
		"the same streams, on different paths. Two paths that are hard links of one file are one file and "
		"never appear as a pair. This is a strong sign of identical copies, not a proof: nothing is "
		"compared byte by byte, and nothing is changed from here."));
	if (fact == NULL || fact->paths == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No files look like copies of each other."));
		return card;
	}
	{
		g_autofree gchar *size = size_text(fact->bytes);
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " %s; the copies beyond the first occupy %s "
			"in all.%s", fact->paths, fact->paths == 1 ? "group" : "groups", size,
			items != NULL && json_array_get_length(items) < (guint)fact->paths ?
			" The groups that would free the most are listed." : "");

		BROWSER_ADD(card, axis_web_storage_text(NULL, text));
	}
	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		JsonArray *members = json_object_get_array_member(item, "members");
		gint64 copies = oi(item, "copies");
		g_autofree gchar *href = group_href(members, copies);
		g_autofree gchar *label = g_strdup_printf("%" G_GINT64_FORMAT " copies", copies);
		g_autofree gchar *size = size_text(oi(item, "size"));
		g_autofree gchar *wasted = size_text(oi(item, "wasted"));
		g_autofree gchar *length = g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT " min",
			oi(item, "duration_ms") / 60000, (oi(item, "duration_ms") / 1000) % 60);
		g_autofree gchar *evidence = g_strdup_printf(" of %s each, %s long, streams %s. The extra copies "
			"occupy %s.", size, length, os(item, "layout"), wasted);
		HtmxDiv *group = htmx_div_new();
		HtmxP *head = htmx_p_new();
		HtmxSpan *tail = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(group), "dup-group");
		BROWSER_ADD(head, likely_badge());
		BROWSER_ADD(head, axis_web_analysis_figure(label, href));
		htmx_node_set_text_content(HTMX_NODE(tail), evidence);
		BROWSER_ADD(head, tail);
		BROWSER_ADD(group, head);
		if (members != NULL)
			BROWSER_ADD(group, members_table(snapshot, members, FALSE));
		if ((gint64)(members != NULL ? json_array_get_length(members) : 0) < copies)
			BROWSER_ADD(group, axis_web_storage_text("help", "Only the first copies are listed."));
		BROWSER_ADD(card, group);
	}
	return card;
}

/* What the key of a group says, in words. */
static gchar *
title_evidence(JsonObject *item)
{
	JsonArray *parts = json_object_has_member(item, "parts") ? json_object_get_array_member(item, "parts") : NULL;
	const gchar *kind = os(item, "kind");
	const gchar *a = parts != NULL && json_array_get_length(parts) > 0 ? json_array_get_string_element(parts, 0) : "";
	const gchar *b = parts != NULL && json_array_get_length(parts) > 1 ? json_array_get_string_element(parts, 1) : "";
	const gchar *c = parts != NULL && json_array_get_length(parts) > 2 ? json_array_get_string_element(parts, 2) : "";
	const gchar *d = parts != NULL && json_array_get_length(parts) > 3 ? json_array_get_string_element(parts, 3) : "";

	if (g_strcmp0(kind, "film") == 0)
		return g_strdup_printf("The same movie: \"%s\", %s (read from the names).", a, b);
	if (g_strcmp0(kind, "episode") == 0)
		return g_strdup_printf("The same episode: \"%s\"%s%s, season %s, episode %s (read from the names).", a,
			g_strcmp0(b, "0") == 0 ? "" : ", ", g_strcmp0(b, "0") == 0 ? "" : b, c, d);
	return g_strdup_printf("The same track: \"%s\" by %s on %s (from the tags).", c, a, b);
}

static HtmxNode *
title_card(
	const AxisAnalysisSnapshot *snapshot,
	const AxisAnalysisFact     *fact
){
	HtmxNode *card = axis_web_storage_card("Likely the same title");
	JsonArray *items = items_of(fact);
	guint i;

	BROWSER_ADD(card, axis_web_storage_text("help", "Files in the same kind of library folder whose names (or tags, for "
		"music) say the same title, and for movies and shows the same year. This is a guess from names and "
		"the names are read by a small, strict rule: a movie without a year, two movies of one title and "
		"different years, different episodes and names with two episodes in them are never grouped. A remake, "
		"an edition or a trailer can still be taken for a copy, so each group shows its evidence. The copy that "
		"looks best is first (details read before not read yet, then resolution, HDR, lossless audio, bit rate, "
		"size). Nothing is changed or suggested from here."));
	if (fact != NULL) {
		JsonObject *considered = json_object_has_member(fact->extra, "considered") ?
			json_object_get_object_member(fact->extra, "considered") : NULL;
		JsonObject *candidates = json_object_has_member(fact->extra, "candidates") ?
			json_object_get_object_member(fact->extra, "candidates") : NULL;
		gint64 read = 0;
		gint64 all = 0;
		static const gchar *const kinds[] = { "film", "episode", "track", NULL };
		guint k;

		for (k = 0; kinds[k] != NULL; k++) {
			read += considered != NULL ? json_object_get_int_member_with_default(considered, kinds[k], 0) : 0;
			all += candidates != NULL ? json_object_get_int_member_with_default(candidates, kinds[k], 0) : 0;
		}
		{
			g_autofree gchar *text = g_strdup_printf("Names read: %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
				" movies, episodes and tracks in movie, TV and music library folders. The rest have a name the rule does "
				"not read, or are samples and extras, and are not compared.", read, all);

			BROWSER_ADD(card, axis_web_storage_text("help", text));
		}
		if (ob(fact->extra, "truncated"))
			BROWSER_ADD(card, axis_web_browser_warning("The library has more names than one report reads; "
				"the comparison covers only the first of them."));
	}
	if (fact == NULL || fact->paths == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No two files have names that say the same title."));
		return card;
	}
	{
		g_autofree gchar *size = size_text(fact->bytes);
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " %s; the copies that do not look best "
			"occupy %s in all.%s", fact->paths, fact->paths == 1 ? "group" : "groups", size,
			items != NULL && json_array_get_length(items) < (guint)fact->paths ?
			" The groups that would free the most are listed." : "");

		BROWSER_ADD(card, axis_web_storage_text(NULL, text));
	}
	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		JsonArray *members = json_object_get_array_member(item, "members");
		gint64 copies = oi(item, "copies");
		g_autofree gchar *href = group_href(members, copies);
		g_autofree gchar *label = g_strdup_printf("%" G_GINT64_FORMAT " copies", copies);
		g_autofree gchar *evidence = title_evidence(item);
		g_autofree gchar *size = size_text(oi(item, "reclaimable"));
		g_autofree gchar *rest = g_strdup_printf(" %s The ones that do not look best occupy %s.", evidence, size);
		HtmxDiv *group = htmx_div_new();
		HtmxP *head = htmx_p_new();
		HtmxSpan *tail = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(group), "dup-group");
		BROWSER_ADD(head, likely_badge());
		BROWSER_ADD(head, axis_web_analysis_figure(label, href));
		htmx_node_set_text_content(HTMX_NODE(tail), rest);
		BROWSER_ADD(head, tail);
		BROWSER_ADD(group, head);
		if (members != NULL)
			BROWSER_ADD(group, members_table(snapshot, members, TRUE));
		if ((gint64)(members != NULL ? json_array_get_length(members) : 0) < copies)
			BROWSER_ADD(group, axis_web_storage_text("help", "Only the first copies are listed."));
		BROWSER_ADD(card, group);
	}
	return card;
}

static HtmxResponse *
duplicates_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/duplicates", "Likely duplicate files");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "Likely duplicate files", HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_ALL));
	BROWSER_ADD(content, content_card(snapshot, axis_analysis_snapshot_fact(snapshot, "dup_content", "", NULL)));
	BROWSER_ADD(content, title_card(snapshot, axis_analysis_snapshot_fact(snapshot, "dup_title", "", NULL)));
	/* What Axis knows from its links is another page: a movie or an episode
	 * with more than one file linked, with a recommendation and the clean-up. */
	BROWSER_ADD(content, axis_web_browser_link("/duplicates", "Movies and episodes with more than one file linked "
		"(Duplicates)"));
	return axis_web_page_respond(request, "Likely duplicate files", HTMX_NODE(content));
}

/* ── Split across volumes ───────────────────────────────────────── */

static HtmxNode *
split_card(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *heading,
	const gchar                *dim,
	const gchar                *help
){
	static const AxisWebColumn columns[] = {
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "Folder", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card(heading);
	const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, "split", dim, NULL);
	JsonArray *items = items_of(fact);
	guint i;

	BROWSER_ADD(card, axis_web_storage_text("help", help));
	if (fact == NULL || fact->paths == 0) {
		BROWSER_ADD(card, axis_web_storage_text(NULL, g_strcmp0(dim, "series") == 0 ?
			"No series is on more than one disk." : "No album is on more than one disk."));
		return card;
	}
	{
		g_autofree gchar *size = size_text(fact->bytes);
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " %s on more than one disk, %s in all.%s",
			fact->paths, g_strcmp0(dim, "series") == 0 ? "series" : (fact->paths == 1 ? "album" : "albums"), size, items != NULL && json_array_get_length(items) < (guint)fact->paths ?
			" The ones with the most to move are listed." : "");

		BROWSER_ADD(card, axis_web_storage_text(NULL, text));
	}
	if (axis_analysis_extra_int(fact->extra, "not_compared_paths") > 0) {
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " %s not compared: %s not in "
			"a folder of the kind this report reads, or the folder's name says nothing.",
			axis_analysis_extra_int(fact->extra, "not_compared_paths"),
			axis_analysis_extra_int(fact->extra, "not_compared_paths") == 1 ? "file is" : "files are",
			axis_analysis_extra_int(fact->extra, "not_compared_paths") == 1 ? "it is" : "they are");

		BROWSER_ADD(card, axis_web_storage_text("help", text));
	}
	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		JsonObject *item = json_array_get_object_element(items, i);
		JsonArray *parts = json_object_get_array_member(item, "parts");
		g_autofree gchar *total = size_text(oi(item, "bytes"));
		g_autofree gchar *moved = size_text(oi(item, "moved"));
		g_autofree gchar *rest = g_strdup_printf(" on %" G_GINT64_FORMAT " disks, %s in all; %s are not on "
			"the disk that holds the most.", oi(item, "volumes"), total, moved);
		HtmxDiv *group = htmx_div_new();
		HtmxP *head = htmx_p_new();
		HtmxSpan *tail = htmx_span_new();
		HtmxNode *table = axis_web_table_new("split", columns);
		HtmxTbody *body = htmx_tbody_new();
		guint k;

		htmx_element_add_class(HTMX_ELEMENT(group), "dup-group");
		BROWSER_ADD(head, axis_web_browser_name_node(os(item, "name")));
		htmx_node_set_text_content(HTMX_NODE(tail), rest);
		BROWSER_ADD(head, tail);
		BROWSER_ADD(group, head);
		for (k = 0; parts != NULL && k < json_array_get_length(parts); k++) {
			JsonObject *part = json_array_get_object_element(parts, k);
			AxisLibraryFilter filter;
			g_autofree gchar *href = NULL;
			HtmxTr *row = htmx_tr_new();
			HtmxTd *volume = htmx_td_new();
			HtmxTd *folder = htmx_td_new();

			if (oi(part, "dir_id") > 0) {
				axis_library_filter_init(&filter);
				filter.dir_id = oi(part, "dir_id");
				href = axis_web_analysis_files_href(&filter);
				axis_library_filter_clear(&filter);
			}
			BROWSER_ADD(volume, bdi_text(axis_web_analysis_volume_name(snapshot, oi(part, "volume_id"))));
			if (!ob(part, "usable"))
				BROWSER_ADD(volume, away_badge());
			BROWSER_ADD(row, volume);
			htmx_element_add_class(HTMX_ELEMENT(folder), "scan-path");
			BROWSER_ADD(folder, axis_web_browser_name_node(os(part, "folder")));
			BROWSER_ADD(row, folder);
			BROWSER_ADD(row, count_cell(oi(part, "paths"), href));
			BROWSER_ADD(row, bytes_cell(oi(part, "bytes")));
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(group, table);
		BROWSER_ADD(card, group);
	}
	return card;
}

static HtmxResponse *
split_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/split", "Series and albums split across disks");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "Series and albums split across disks",
			HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_SCAN));
	BROWSER_ADD(content, split_card(snapshot, "Series on more than one disk", "series",
		"A series is a folder directly inside a TV library folder; two folders with the same name (and year, if it has "
		"one) on different disks are one series. A folder with a year and the same folder without one are "
		"not matched. The number of files is the length of the list of the folder's files."));
	BROWSER_ADD(content, split_card(snapshot, "Albums on more than one disk", "album",
		"An album is a folder two levels inside a music library folder (artist and album); two with the same "
		"names on different disks are one album. This only describes where files are: it proposes no move."));
	return axis_web_page_respond(request, "Series and albums split across disks", HTMX_NODE(content));
}

/* ── Leftovers ──────────────────────────────────────────────────── */

static HtmxResponse *
leftovers_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	PageWork        *work
){
	static const AxisWebColumn columns[] = {
		{ "Rule", AXIS_WEB_COL_NAME },
		{ "What it matches", AXIS_WEB_COL_WIDE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	static const AxisWebColumn item_columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Where", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = page_start(request, work, "/analysis/leftovers", "Leftovers in the library");
	const AxisAnalysisSnapshot *snapshot = work->snapshot;
	const AxisLeftoverRule *rules = axis_leftover_rules();
	const AxisAnalysisFact *all;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	gint64 linked = 0;
	guint i;

	if (snapshot == NULL)
		return axis_web_page_respond(request, "Leftovers in the library", HTMX_NODE(content));
	BROWSER_ADD(content, axis_web_analysis_coverage(snapshot, AXIS_ANALYSIS_COVER_SCAN));

	card = axis_web_storage_card("Files that look like leftovers");
	BROWSER_ADD(card, axis_web_storage_text("help", "Files that look like debris from their name and place. They "
		"are candidates only: some people keep extras, and a name can mislead. Files in a library folder that holds "
		"downloads are never listed (a download client may still use them). Nothing is deleted or moved from here. Empty folders cannot be "
		"listed: the index records files, not folders."));
	table = axis_web_table_new("leftover-rules", columns);
	body = htmx_tbody_new();
	for (i = 0; rules[i].id != NULL; i++) {
		const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, "leftover", rules[i].id, NULL);
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		HtmxTr *row = htmx_tr_new();

		axis_library_filter_init(&filter);
		filter.leftover = rules[i].id;
		filter.sort = AXIS_LIBRARY_SORT_SIZE;
		filter.descending = TRUE;
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		BROWSER_ADD(row, text_cell(NULL, rules[i].label));
		BROWSER_ADD(row, text_cell(NULL, rules[i].help));
		BROWSER_ADD(row, count_cell(fact != NULL ? fact->paths : 0, href));
		BROWSER_ADD(row, bytes_cell(fact != NULL ? fact->bytes : 0));
		if (fact != NULL)
			linked += oi(fact->extra, "linked_paths");
		BROWSER_ADD(body, row);
	}
	all = axis_analysis_snapshot_fact(snapshot, "leftover", "any", NULL);
	{
		AxisLibraryFilter filter;
		g_autofree gchar *href = NULL;
		HtmxTr *row = htmx_tr_new();

		axis_library_filter_init(&filter);
		filter.leftover = "any";
		filter.sort = AXIS_LIBRARY_SORT_SIZE;
		filter.descending = TRUE;
		href = axis_web_analysis_files_href(&filter);
		axis_library_filter_clear(&filter);
		BROWSER_ADD(row, text_cell(NULL, "All together"));
		BROWSER_ADD(row, text_cell(NULL, "Files matching any rule, each once"));
		BROWSER_ADD(row, count_cell(all != NULL ? all->paths : 0, href));
		BROWSER_ADD(row, bytes_cell(all != NULL ? all->bytes : 0));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	{
		HtmxNode *note = linked_note(linked);

		if (note != NULL)
			BROWSER_ADD(card, note);
	}
	BROWSER_ADD(content, card);

	for (i = 0; rules[i].id != NULL; i++) {
		const AxisAnalysisFact *fact = axis_analysis_snapshot_fact(snapshot, "leftover", rules[i].id, NULL);
		JsonArray *items = items_of(fact);
		guint k;

		if (items == NULL || json_array_get_length(items) == 0)
			continue;
		{
			g_autofree gchar *heading = g_strdup_printf("%s: the largest files", rules[i].label);

			card = axis_web_storage_card(heading);
		}
		table = axis_web_table_new("leftover-files", item_columns);
		body = htmx_tbody_new();
		for (k = 0; k < json_array_get_length(items); k++) {
			JsonObject *item = json_array_get_object_element(items, k);
			gint64 volume_id = oi(item, "volume_id");
			HtmxTr *row = htmx_tr_new();

			BROWSER_ADD(row, file_cell(oi(item, "id"), os(item, "path")));
			BROWSER_ADD(row, bytes_cell(oi(item, "size")));
			BROWSER_ADD(row, where_cell(snapshot, oi(item, "root_id"), volume_id, volume_usable(snapshot, volume_id)));
			BROWSER_ADD(body, row);
		}
		BROWSER_ADD(table, body);
		BROWSER_ADD(card, table);
		BROWSER_ADD(content, card);
	}
	return axis_web_page_respond(request, "Leftovers in the library", HTMX_NODE(content));
}

/* ── Handlers ───────────────────────────────────────────────────── */

static HtmxResponse *
page_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	gpointer         data
){
	PageWork *work = data;

	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
			return axis_web_error_respond(request, 503, "Too slow",
				"The reports could not be read in time. Try again in a moment.");
		return axis_web_internal_error(request, work->error);
	}
	if (work->fragment) {
		HtmxNode *block = axis_web_analysis_status_block(&work->status);
		HtmxResponse *response = fragment_response(block);

		/* The refresh ended: the page is replaced by one with the new figures. */
		if (!work->status.refreshing)
			htmx_response_add_header(response, "HX-Refresh", "true");
		return response;
	}
	switch (work->page) {
	case PAGE_SPACE:
		return space_done(self, request, work);
	case PAGE_COMPOSITION:
		return composition_done(self, request, work);
	case PAGE_LARGEST:
		return largest_done(self, request, work);
	case PAGE_DUPLICATES:
		return duplicates_done(self, request, work);
	case PAGE_SPLIT:
		return split_done(self, request, work);
	case PAGE_LEFTOVERS:
		return leftovers_done(self, request, work);
	case PAGE_OVERVIEW:
	default:
		return overview_done(self, request, work);
	}
}

static HtmxResponse *
page_handle(
	HtmxRequest *request,
	gpointer     user_data,
	PageId       page,
	gboolean     fragment
){
	GHashTable *query = htmx_request_get_query_params(request);
	PageWork *work;

	/* Only the overview takes parameters: the notice after a request, and
	 * the place it was made from. */
	if (query != NULL && g_hash_table_size(query) > 0) {
		guint expected = axis_web_place_of(request) != NULL ? 2 : 1;

		if (page != PAGE_OVERVIEW || fragment || g_hash_table_size(query) != expected ||
		    g_strcmp0(htmx_request_get_query_param(request, "done"), "refresh-requested") != 0)
			return bad_request(request);
	}
	if (fragment && !htmx_request_is_htmx(request))
		return axis_web_auth_redirect(request, "/analysis");

	work = g_new0(PageWork, 1);
	work->page = page;
	work->fragment = fragment;
	return axis_web_analysis_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

#define HANDLER(name, page, fragment) \
	static HtmxResponse * \
	name( \
		HtmxRequest *request, \
		GHashTable  *params, \
		gpointer     user_data \
	){ \
		return page_handle(request, user_data, page, fragment); \
	}

HANDLER(handle_overview, PAGE_OVERVIEW, FALSE)
HANDLER(handle_space, PAGE_SPACE, FALSE)
HANDLER(handle_composition, PAGE_COMPOSITION, FALSE)
HANDLER(handle_largest, PAGE_LARGEST, FALSE)
HANDLER(handle_duplicates, PAGE_DUPLICATES, FALSE)
HANDLER(handle_split, PAGE_SPLIT, FALSE)
HANDLER(handle_leftovers, PAGE_LEFTOVERS, FALSE)
HANDLER(handle_status, PAGE_OVERVIEW, TRUE)

/* ── Refresh now ────────────────────────────────────────────────── */

typedef struct
{
	gint64  job_id;
	GError *error;
} RefreshWork;

static void
refresh_work_free(gpointer data)
{
	RefreshWork *work = data;

	g_clear_error(&work->error);
	g_free(work);
}

static void
refresh_run(
	AxisWebAnalysis *self,
	gpointer         data
){
	RefreshWork *work = data;

	axis_analysis_request_refresh(self->analysis, &work->job_id, &work->error);
}

static HtmxResponse *
refresh_done(
	AxisWebAnalysis *self,
	HtmxRequest     *request,
	gpointer         data
){
	RefreshWork *work = data;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_storage_audit(request, "analysis refresh requested: job=%" G_GINT64_FORMAT, work->job_id);
	return axis_web_place_redirect(request, "/analysis", "refresh-requested", REFRESH_PLACE);
}

static HtmxResponse *
handle_refresh(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_analysis_dispatch(user_data, request, refresh_run, refresh_done, g_new0(RefreshWork, 1),
		refresh_work_free);
}

void
axis_web_analysis_register_pages(
	AxisWebAnalysis *self,
	AxisWeb         *web
){
	axis_web_get(web, "/analysis", handle_overview, self);
	axis_web_get(web, "/analysis/space", handle_space, self);
	axis_web_get(web, "/analysis/composition", handle_composition, self);
	axis_web_get(web, "/analysis/largest", handle_largest, self);
	axis_web_get(web, "/analysis/duplicates", handle_duplicates, self);
	axis_web_get(web, "/analysis/split", handle_split, self);
	axis_web_get(web, "/analysis/leftovers", handle_leftovers, self);
	axis_web_get(web, "/analysis/status", handle_status, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/analysis/refresh", AXIS_WEB_ROUTE_ADMIN, handle_refresh, self);
}
