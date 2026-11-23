/*
 * axis-web-library.c - the library module: scan jobs, their schedule, and
 * each root's page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Paths a scan found are stored in their reversible form
 * (axis-library-name.h); a page shows them only through
 * axis_library_name_display(), as text content (which htmx-glib escapes),
 * inside <bdi> so that a name cannot reorder the text around it.  Nothing
 * from a filesystem is ever put in an attribute or a URL.
 */

#include "web/axis-web-library.h"
#include "web/axis-web-storage-private.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library-name.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

#define ADD axis_web_storage_add

/* Page work waiting for a thread beyond this is answered 503. */
#define MAX_PENDING   16
#define RECENT_SCANS  10

struct _AxisWebLibrary
{
	AxisLibrary *library;	/* owned reference */
	AxisStorage *storage;	/* owned reference */
	guint        pending;	/* dispatched, not yet answered */
	gboolean     stopping;
};

/* ── Work off the main loop ─────────────────────────────────────── */

typedef void (*WorkFunc)(AxisWebLibrary *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebLibrary *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebLibrary *owner;
	WorkFunc        work;
	DoneFunc        done;
	HtmxRequest    *request;
	HtmxResponse   *deferred;
	gpointer        data;
	GDestroyNotify  free_data;
} Task;

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;

	task->work(task->owner, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = task->done(task->owner, task->request, task->data);
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);

	if (task->free_data != NULL)
		task->free_data(task->data);
	g_object_unref(task->request);
	g_object_unref(task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebLibrary *self,
	HtmxRequest    *request,
	WorkFunc        work,
	DoneFunc        done,
	gpointer        data,
	GDestroyNotify  free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		if (free_data != NULL)
			free_data(data);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	task->data = data;
	task->free_data = free_data;
	returned = g_object_ref(task->deferred);

	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);

	return returned;
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxNode *
link_to(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	return HTMX_NODE(link);
}

static HtmxNode *
facts_new(void)
{
	HtmxDl *list = htmx_dl_new();

	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	return HTMX_NODE(list);
}

static void
fact(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
){
	ADD(list, htmx_dt_new_with_text(label));
	ADD(list, htmx_dd_new_with_text(text != NULL ? text : ""));
}

static void
fact_node(
	HtmxNode    *list,
	const gchar *label,
	HtmxNode    *value
){
	HtmxDd *dd = htmx_dd_new();

	ADD(list, htmx_dt_new_with_text(label));
	ADD(dd, value);
	ADD(list, dd);
}

static void
fact_count(
	HtmxNode    *list,
	const gchar *label,
	guint64      value
){
	g_autofree gchar *text = g_strdup_printf("%" G_GUINT64_FORMAT, value);

	fact(list, label, text);
}

static HtmxNode *
warning(const gchar *text)
{
	HtmxP *notice = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
	htmx_element_add_class(HTMX_ELEMENT(notice), "notice-warning");
	htmx_element_set_attribute(HTMX_ELEMENT(notice), "role", "alert");
	htmx_node_set_text_content(HTMX_NODE(notice), text);
	return HTMX_NODE(notice);
}

/* A stored path as text a person can read, isolated from its
 * surroundings; "(the root's directory)" for "". */
static HtmxNode *
path_node(const gchar *stored)
{
	HtmxCode *code = htmx_code_new();
	HtmxBdi *bdi = htmx_bdi_new();
	g_autofree gchar *shown = NULL;

	htmx_element_add_class(HTMX_ELEMENT(code), "scan-path");
	if (stored == NULL || stored[0] == '\0') {
		htmx_node_set_text_content(HTMX_NODE(bdi), "(the library folder itself)");
	} else {
		shown = axis_library_name_display(stored);
		htmx_node_set_text_content(HTMX_NODE(bdi), shown);
	}
	ADD(code, bdi);

	if (stored != NULL && axis_library_name_is_invalid(stored)) {
		HtmxSpan *span = htmx_span_new();
		HtmxSpan *note = htmx_span_new();

		ADD(span, code);
		htmx_element_add_class(HTMX_ELEMENT(note), "badge");
		htmx_element_add_class(HTMX_ELEMENT(note), "badge-note");
		htmx_node_set_text_content(HTMX_NODE(note), "not valid UTF-8");
		ADD(span, note);
		return HTMX_NODE(span);
	}
	return HTMX_NODE(code);
}

static const gchar *
status_label(AxisScanStatus status)
{
	switch (status) {
	case AXIS_SCAN_RUNNING:
		return "Running";
	case AXIS_SCAN_COMPLETE:
		return "Complete";
	case AXIS_SCAN_UNAVAILABLE:
		return "Disk offline";
	case AXIS_SCAN_INCOMPLETE:
	default:
		return "Incomplete";
	}
}

static HtmxNode *
status_badge(AxisScanStatus status)
{
	HtmxSpan *badge = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("scan-%s", axis_scan_status_to_string(status));

	htmx_element_add_class(HTMX_ELEMENT(badge), "badge");
	htmx_element_add_class(HTMX_ELEMENT(badge), css);
	htmx_node_set_text_content(HTMX_NODE(badge), status_label(status));
	return HTMX_NODE(badge);
}

static const gchar *
trigger_label(const gchar *trigger)
{
	if (g_strcmp0(trigger, "schedule") == 0)
		return "On schedule";
	if (g_strcmp0(trigger, "request") == 0)
		return "Requested";
	return "Started directly";
}

static const gchar *
finding_label(const gchar *kind)
{
	static const struct
	{
		const gchar *kind;
		const gchar *label;
	} labels[] = {
		{ "other_filesystem", "Another filesystem or mount: skipped" },
		{ "too_deep",         "Too deep: skipped" },
		{ "too_long",         "Path too long: skipped" },
		{ "unreadable",       "Could not be read" },
		{ "io_error",         "Read error" },
		{ "vanished",         "Went away while scanned" },
		{ "replaced",         "Replaced while scanned" },
		{ "partial",          "Partial download: left out" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; labels[i].kind != NULL; i++) {
		if (g_strcmp0(kind, labels[i].kind) == 0)
			return labels[i].label;
	}
	return "Other";
}

static gchar *
duration_text(
	gint64 from_us,
	gint64 to_us
){
	gint64 seconds;

	if (from_us <= 0 || to_us < from_us)
		return g_strdup("");
	seconds = (to_us - from_us) / G_USEC_PER_SEC;
	if (seconds < 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " s", seconds);
	if (seconds < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " min %" G_GINT64_FORMAT " s", seconds / 60,
			seconds % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " h %" G_GINT64_FORMAT " min", seconds / 3600,
		(seconds % 3600) / 60);
}

static HtmxNode *
count_cell(guint64 value)
{
	g_autofree gchar *text = g_strdup_printf("%" G_GUINT64_FORMAT, value);

	return HTMX_NODE(htmx_td_new_with_text(text));
}

/* ── The root's page ────────────────────────────────────────────── */

static HtmxNode *
kinds_table(const AxisScanRecord *scan)
{
	static const AxisWebColumn columns[] = {
		{ "Type of file", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("breakdown", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < AXIS_FILE_KIND_COUNT; i++) {
		HtmxTr *row;
		g_autofree gchar *size = NULL;

		if (scan->walk.kind_files[i] == 0)
			continue;
		row = htmx_tr_new();
		size = axis_size_format(scan->walk.kind_bytes[i]);
		ADD(row, htmx_td_new_with_text(axis_file_kind_label((AxisFileKind)i)));
		ADD(row, count_cell(scan->walk.kind_files[i]));
		ADD(row, htmx_td_new_with_text(size));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
findings_table(
	const AxisScanRecord *scan,
	GPtrArray            *findings
){
	static const AxisWebColumn columns[] = {
		{ "What", AXIS_WEB_COL_TEXT },
		{ "Where", AXIS_WEB_COL_NAME },
		{ "Detail", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("scan-findings", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(table), "scan-findings");
	for (i = 0; i < findings->len; i++) {
		const AxisScanFinding *finding = g_ptr_array_index(findings, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *where = htmx_td_new();

		ADD(row, htmx_td_new_with_text(finding_label(finding->kind)));
		ADD(where, path_node(finding->path));
		ADD(row, where);
		ADD(row, htmx_td_new_with_text(finding->detail != NULL ? finding->detail : ""));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
last_scan_card(
	const AxisScanRecord *scan,
	GPtrArray            *findings
){
	HtmxNode *card = axis_web_storage_card("Last scan");
	HtmxNode *facts;
	g_autofree gchar *started = NULL;
	g_autofree gchar *finished = NULL;
	g_autofree gchar *took = NULL;
	g_autofree gchar *size = NULL;
	guint64 skipped;

	if (scan == NULL) {
		ADD(card, axis_web_storage_text("help", "This library folder has not been scanned yet. Axis "
			"scans it on its schedule while its disk is online; to scan it now, select Scan this "
			"library folder now."));
		return card;
	}

	{
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "scan-status");
		ADD(line, status_badge(scan->status));
		ADD(card, line);
	}
	if (scan->status != AXIS_SCAN_COMPLETE && scan->status != AXIS_SCAN_RUNNING) {
		ADD(card, axis_web_storage_text(NULL, scan->message != NULL ? scan->message :
			axis_scan_reason_describe(scan->reason)));
		ADD(card, axis_web_storage_text("help", "A scan that does not complete changes nothing about "
			"files it did not see: no file is marked as not found because of it."));
	}

	facts = facts_new();
	started = axis_web_storage_format_time(scan->started_us, "unknown");
	fact(facts, "Started", started);
	if (scan->finished_us > 0) {
		finished = axis_web_storage_format_time(scan->finished_us, "unknown");
		took = duration_text(scan->started_us, scan->finished_us);
		fact(facts, "Finished", finished);
		fact(facts, "Took", took);
	}
	fact(facts, "Why", trigger_label(scan->trigger));
	fact_count(facts, "Folders", scan->walk.dirs);
	fact_count(facts, "Files", scan->walk.files);
	size = axis_size_format(scan->walk.bytes);
	fact(facts, "Size of the files", size);
	fact_count(facts, "Symbolic links", scan->walk.symlinks);
	ADD(card, facts);

	if (scan->walk.files > 0 || scan->walk.symlinks > 0)
		ADD(card, kinds_table(scan));

	facts = facts_new();
	fact_count(facts, "New", scan->new_files);
	fact_count(facts, "Changed", scan->changed_files);
	fact_count(facts, "Moved or renamed", scan->moved);
	fact_count(facts, "Seen again", scan->reappeared);
	if (scan->status == AXIS_SCAN_COMPLETE) {
		fact_count(facts, "Not found (suspected)", scan->suspected);
		fact_count(facts, "Not found again (missing)", scan->confirmed);
		fact_count(facts, "Records removed", scan->removed);
	}
	ADD(card, axis_web_storage_text("scan-subhead", "Changes to the files on record"));
	ADD(card, facts);

	facts = facts_new();
	fact_count(facts, "Names left out", scan->walk.ignored);
	fact_count(facts, "Partial downloads", scan->walk.partials);
	fact_count(facts, "Other files (pipes, sockets, devices)", scan->walk.special);
	ADD(card, axis_web_storage_text("scan-subhead", "Left out"));
	ADD(card, facts);

	skipped = scan->walk.other_fs + scan->walk.too_deep + scan->walk.too_long + scan->walk.unreadable +
		scan->walk.io_errors + scan->walk.vanished + scan->walk.replaced;
	if (skipped > 0) {
		facts = facts_new();
		if (scan->walk.other_fs > 0)
			fact_count(facts, "On another filesystem", scan->walk.other_fs);
		if (scan->walk.too_deep > 0)
			fact_count(facts, "Too deep", scan->walk.too_deep);
		if (scan->walk.too_long > 0)
			fact_count(facts, "Path too long", scan->walk.too_long);
		if (scan->walk.unreadable > 0)
			fact_count(facts, "Could not be read", scan->walk.unreadable);
		if (scan->walk.io_errors > 0)
			fact_count(facts, "Read errors", scan->walk.io_errors);
		if (scan->walk.vanished > 0)
			fact_count(facts, "Went away while scanned", scan->walk.vanished);
		if (scan->walk.replaced > 0)
			fact_count(facts, "Replaced while scanned", scan->walk.replaced);
		ADD(card, axis_web_storage_text("scan-subhead", "Not examined"));
		ADD(card, axis_web_storage_text("help", "Nothing recorded under these is taken for gone."));
		ADD(card, facts);
	}

	if (findings != NULL && findings->len > 0) {
		ADD(card, axis_web_storage_text("scan-subhead", "What the scan skipped or could not read"));
		ADD(card, findings_table(scan, findings));
		if (scan->walk.findings > findings->len) {
			g_autofree gchar *more = g_strdup_printf("%" G_GUINT64_FORMAT " more skipped files and folders are not listed.",
				scan->walk.findings - findings->len);

			ADD(card, axis_web_storage_text("help", more));
		}
	}

	return card;
}

static HtmxNode *
index_card(const AxisScanRecord *complete)
{
	HtmxNode *card = axis_web_storage_card("Files on record");
	HtmxNode *facts;
	g_autofree gchar *when = NULL;
	g_autofree gchar *line = NULL;

	ADD(card, axis_web_storage_text("help", "A file that one complete scan does not find is suspected; "
		"not found again by a complete scan an hour or more later, it is missing; its record is "
		"removed 30 days after that. A file seen again is back at once, with its record."));
	if (complete == NULL) {
		ADD(card, axis_web_storage_text(NULL, "No scan of this library folder has completed yet."));
		return card;
	}

	when = axis_web_storage_format_time(complete->finished_us, "unknown");
	line = g_strdup_printf("As of the last complete scan, %s.", when);
	ADD(card, axis_web_storage_text(NULL, line));
	facts = facts_new();
	fact_count(facts, "Present", (guint64)MAX(complete->present_after, 0));
	fact_count(facts, "Not found by the last scan (suspected)", (guint64)MAX(complete->suspected_after, 0));
	fact_count(facts, "Missing", (guint64)MAX(complete->missing_after, 0));
	fact_count(facts, "Pinned (hard-linked, or in a library folder that holds downloads)",
		(guint64)MAX(complete->pinned_after, 0));
	ADD(card, facts);
	return card;
}

/* Files whose names are not UTF-8: as shown elsewhere, and their exact
 * bytes. */
static HtmxNode *
names_card(const AxisRootScanView *view)
{
	static const AxisWebColumn columns[] = {
		{ "Shown as", AXIS_WEB_COL_NAME },
		{ "Exact name", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("File names with characters that cannot be shown");
	HtmxNode *table = axis_web_table_new("invalid-names", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	ADD(card, axis_web_storage_text("help", "These names are kept exactly as their bytes, and Axis "
		"never renames them. Elsewhere a byte that is not UTF-8 shows as \xef\xbf\xbd; here each "
		"one is written \\xHH."));
	for (i = 0; i < view->invalid_names->len; i++) {
		const gchar *stored = g_ptr_array_index(view->invalid_names, i);
		g_autofree gchar *exact = axis_library_name_exact(stored);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *shown = htmx_td_new();
		HtmxTd *bytes = htmx_td_new();
		HtmxCode *code = htmx_code_new();
		HtmxBdi *bdi = htmx_bdi_new();

		ADD(shown, path_node(stored));
		htmx_element_add_class(HTMX_ELEMENT(code), "scan-path");
		htmx_node_set_text_content(HTMX_NODE(bdi), exact);
		ADD(code, bdi);
		ADD(bytes, code);
		ADD(row, shown);
		ADD(row, bytes);
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	if (view->n_invalid_names > view->invalid_names->len) {
		g_autofree gchar *more = g_strdup_printf("%" G_GUINT64_FORMAT " more names are not listed.",
			view->n_invalid_names - view->invalid_names->len);

		ADD(card, axis_web_storage_text("help", more));
	}
	return card;
}

static HtmxNode *
recent_card(GPtrArray *recent)
{
	static const AxisWebColumn columns[] = {
		{ "Started", AXIS_WEB_COL_DATE },
		{ "Status", AXIS_WEB_COL_STATE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "New", AXIS_WEB_COL_COUNT },
		{ "Changed", AXIS_WEB_COL_COUNT },
		{ "Not found", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Recent scans");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (recent->len == 0) {
		ADD(card, axis_web_storage_text("help", "No scans yet."));
		return card;
	}

	table = axis_web_table_new("recent-scans", columns);
	body = htmx_tbody_new();
	for (i = 0; i < recent->len; i++) {
		const AxisScanRecord *scan = g_ptr_array_index(recent, i);
		g_autofree gchar *started = axis_web_storage_format_time(scan->started_us, "unknown");
		HtmxTr *row = htmx_tr_new();
		HtmxTd *status = htmx_td_new();

		ADD(row, htmx_td_new_with_text(started));
		ADD(status, status_badge(scan->status));
		ADD(row, status);
		ADD(row, count_cell(scan->walk.files));
		ADD(row, count_cell(scan->new_files));
		ADD(row, count_cell(scan->changed_files));
		ADD(row, count_cell(scan->suspected));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
actions_card(
	HtmxRequest            *request,
	const AxisRootInfo     *root,
	const AxisRootScanView *view,
	AxisWebNotice          *placed
){
	HtmxNode *card = axis_web_storage_card("Scan settings for this library folder");
	g_autofree gchar *scan_action = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/scan", root->id);
	g_autofree gchar *settings_action = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/settings",
		root->id);
	g_autofree gchar *hours = g_strdup_printf("%u", view->interval_seconds / 3600);
	g_autofree gchar *patterns = g_strjoinv("\n", view->patterns);
	g_autofree gchar *interval_help = g_strdup_printf("From %d to %d hours.",
		AXIS_LIBRARY_MIN_INTERVAL_SECONDS / 3600, AXIS_LIBRARY_MAX_INTERVAL_SECONDS / 3600);
	HtmxNode *form;
	HtmxDiv *field;
	HtmxLabel *label;
	HtmxTextarea *area;

	htmx_element_set_id(HTMX_ELEMENT(card), "root-actions");
	axis_web_notice_put(placed, card, "root-actions");
	form = axis_web_auth_post_form(request, scan_action);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_button("Scan this library folder now", "button-primary"));
	ADD(card, form);
	ADD(card, axis_web_storage_text("help", "The scan starts when its disk's turn comes: Axis scans the "
		"library folders of one disk one after another. A scan only reads; nothing on the disk "
		"changes."));

	form = axis_web_auth_post_form(request, settings_action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_storage_text_field("root-interval", "Scan every (hours)", "interval_hours", hours,
		NULL, interval_help));
	field = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	label = htmx_label_new_for("root-patterns");
	htmx_node_set_text_content(HTMX_NODE(label), "Names to leave out");
	area = htmx_textarea_new_with_name("patterns");
	htmx_element_set_id(HTMX_ELEMENT(area), "root-patterns");
	htmx_textarea_set_rows(area, 4);
	htmx_element_set_attribute(HTMX_ELEMENT(area), "spellcheck", "false");
	if (patterns[0] != '\0')
		htmx_node_set_text_content(HTMX_NODE(area), patterns);
	ADD(field, label);
	ADD(field, area);
	ADD(field, axis_web_storage_text("help", "One pattern per line, matched against each file and "
		"folder name: * matches anything, ? one character. Housekeeping names (lost+found, @eaDir, "
		".Trash-*, ...) and partial downloads (.part, .!qB, ...) are always left out. Files under a "
		"name left out keep their records."));
	ADD(form, field);
	axis_web_auth_add(form, axis_web_auth_button("Save scan settings", NULL));
	ADD(card, form);

	return card;
}

static gchar *
root_directory(
	const AxisVolumeInfo *volume,
	const AxisRootInfo   *root
){
	if (volume == NULL)
		return g_strdup(root->rel_path);
	if (root->rel_path[0] == '\0')
		return g_strdup_printf("%s (the top of the disk)", volume->path);
	return g_strdup_printf("%s/%s", volume->path, root->rel_path);
}

static const gchar *
done_message(const gchar *key)
{
	if (g_strcmp0(key, "scan-requested") == 0)
		return "A scan has been requested. It runs as soon as its disk's turn comes; this page "
			"shows it once it has.";
	if (g_strcmp0(key, "settings-saved") == 0)
		return "The scan settings have been saved. They apply from the next scan.";
	return NULL;
}

static HtmxResponse *
root_page(
	AxisWebLibrary         *self,
	HtmxRequest            *request,
	const AxisRootInfo     *root,
	const AxisRootScanView *view
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, root->volume_id);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *volume_href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, root->volume_id);
	g_autofree gchar *directory = root_directory(volume, root);
	g_autofree gchar *every = NULL;
	const gchar *notice = done_message(htmx_request_get_query_param(request, "done"));
	HtmxHeading *title = htmx_h1_new();
	HtmxP *crumbs = htmx_p_new();
	HtmxNode *card;
	HtmxNode *facts;
	AxisWebNotice placed;

	ADD(crumbs, link_to("/storage/roots", "Back to Library folders"));
	ADD(content, crumbs);
	htmx_node_set_text_content(HTMX_NODE(title), root->name);
	ADD(content, title);
	{
		g_autofree gchar *browse = g_strdup_printf("/library/root/%" G_GINT64_FORMAT, root->id);
		g_autofree gchar *probe = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", root->id);
		HtmxP *links = htmx_p_new();
		HtmxSpan *separator = htmx_span_new();

		/* The pages that look into the root: its folders and files, and
		 * what the probe found in them. */
		htmx_node_set_text_content(HTMX_NODE(separator), " | ");
		ADD(links, link_to(browse, "Browse the files in this library folder"));
		ADD(links, separator);
		ADD(links, link_to(probe, "Files read in this library folder"));
		ADD(content, links);
	}
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	if (volume != NULL && !axis_volume_state_is_usable(volume->state)) {
		g_autofree gchar *text = g_strdup_printf("Disk '%s' is not online: %s Scans wait until it "
			"is back, and nothing recorded about its files changes while it is away.", volume->name,
			axis_volume_reason_describe(volume->reason));

		ADD(content, warning(text));
	}

	card = axis_web_storage_card("Location and scan schedule");
	facts = facts_new();
	fact(facts, "Holds", axis_media_type_label(root->media_type));
	if (volume != NULL) {
		g_autofree gchar *state = g_strdup_printf("%s (%s)", volume->name,
			axis_volume_state_label(volume->state));

		fact_node(facts, "Disk", link_to(volume_href, state));
	}
	fact_node(facts, "Directory", HTMX_NODE(htmx_code_new_with_text(directory)));
	every = view->interval_seconds == 3600 ? g_strdup("Every hour") :
		view->interval_seconds % 3600 == 0 ?
		g_strdup_printf("Every %u hours", view->interval_seconds / 3600) :
		g_strdup_printf("Every %u minutes", view->interval_seconds / 60);
	fact(facts, "Scanned", every);
	if (view->patterns[0] != NULL) {
		g_autofree gchar *joined = g_strjoinv("  ", view->patterns);

		fact_node(facts, "Names left out",HTMX_NODE(htmx_code_new_with_text(joined)));
	}
	if (root->media_type == AXIS_MEDIA_DOWNLOADS)
		fact(facts, "Pinned", "Every file: this library folder holds downloads");
	if (view->requested_us > 0) {
		g_autofree gchar *when = axis_web_storage_format_time(view->requested_us, "unknown");
		g_autofree gchar *text = g_strdup_printf("%s, waiting for its turn", when);

		fact(facts, "Scan requested", text);
	}
	ADD(card, facts);
	ADD(content, card);

	ADD(content, last_scan_card(view->last, view->findings));
	ADD(content, index_card(view->last_complete));
	if (view->n_invalid_names > 0)
		ADD(content, names_card(view));
	ADD(content, recent_card(view->recent));
	if (axis_web_request_is_admin(request)) {
		ADD(content, actions_card(request, root, view, &placed));
	}

	axis_web_notice_end(&placed);
	{
		g_autofree gchar *page_title = g_strdup_printf("Library folder %s", root->name);

		return axis_web_page_respond(request, page_title, HTMX_NODE(content));
	}
}

typedef struct
{
	gint64            id;
	guint             interval;
	gchar           **patterns;
	AxisRootInfo     *root;
	AxisRootScanView *view;
	gint64            job_id;
	GError           *error;
} RootWork;

static void
root_work_free(gpointer data)
{
	RootWork *work = data;

	g_strfreev(work->patterns);
	axis_root_info_free(work->root);
	axis_root_scan_view_free(work->view);
	g_clear_error(&work->error);
	g_free(work);
}

static void
view_run(
	AxisWebLibrary *self,
	gpointer        data
){
	RootWork *work = data;

	if (!axis_storage_get_root(self->storage, work->id, &work->root, &work->error))
		return;
	axis_library_get_root_view(self->library, work->id, RECENT_SCANS, &work->view, &work->error);
}

static HtmxResponse *
view_done(
	AxisWebLibrary *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	return root_page(self, request, work->root, work->view);
}

static HtmxResponse *
handle_root(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RootWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(RootWork, 1);
	work->id = id;
	return dispatch(user_data, request, view_run, view_done, work, root_work_free);
}

/* ── Scan now ───────────────────────────────────────────────────── */

static void
scan_run(
	AxisWebLibrary *self,
	gpointer        data
){
	RootWork *work = data;

	axis_library_request_scan(self->library, work->id, &work->job_id, &work->error);
}

static HtmxResponse *
scan_done(
	AxisWebLibrary *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "root scan requested: id=%" G_GINT64_FORMAT " job=%" G_GINT64_FORMAT,
		work->id, work->job_id);
	location = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "scan-requested", "root-actions");
}

static HtmxResponse *
handle_scan(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RootWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(RootWork, 1);
	work->id = id;
	return dispatch(user_data, request, scan_run, scan_done, work, root_work_free);
}

/* ── Settings ───────────────────────────────────────────────────── */

static void
settings_run(
	AxisWebLibrary *self,
	gpointer        data
){
	RootWork *work = data;

	axis_library_set_root_config(self->library, work->id, work->interval,
		(const gchar *const *)work->patterns, &work->error);
}

static HtmxResponse *
settings_done(
	AxisWebLibrary *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND))
		return not_found(request);
	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID))
		return axis_web_error_respond(request, 400, "The scan settings were not saved", work->error->message);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "root scan settings changed: id=%" G_GINT64_FORMAT
		" interval=%us patterns=%u", work->id, work->interval, g_strv_length(work->patterns));
	location = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "settings-saved", "root-actions");
}

/* The lines of a textarea, trimmed, without empty ones; at most @max + 1
 * (so that too many can be refused). */
static gchar **
form_lines(
	const gchar *text,
	guint        max
){
	GPtrArray *lines = g_ptr_array_new();
	gchar **split;
	guint i;

	split = g_strsplit_set(text != NULL ? text : "", "\r\n", -1);
	for (i = 0; split[i] != NULL && lines->len <= max; i++) {
		g_strstrip(split[i]);
		if (split[i][0] != '\0')
			g_ptr_array_add(lines, g_strdup(split[i]));
	}
	g_strfreev(split);
	g_ptr_array_add(lines, NULL);

	return (gchar **)g_ptr_array_free(lines, FALSE);
}

static HtmxResponse *
handle_settings(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	guint64 hours = 0;
	RootWork *work;

	if (id == 0)
		return not_found(request);
	if (!axis_web_storage_form_u64(request, "interval_hours", &hours) ||
	    hours < AXIS_LIBRARY_MIN_INTERVAL_SECONDS / 3600 || hours > AXIS_LIBRARY_MAX_INTERVAL_SECONDS / 3600)
		return axis_web_error_respond(request, 400, "The scan settings were not saved",
			"The interval must be a whole number of hours from 1 to 720.");

	work = g_new0(RootWork, 1);
	work->id = id;
	work->interval = (guint)hours * 3600;
	work->patterns = form_lines(htmx_request_get_form_value(request, "patterns"),
		AXIS_LIBRARY_MAX_PATTERNS);
	return dispatch(user_data, request, settings_run, settings_done, work, root_work_free);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebLibrary *
axis_web_library_new(
	AxisLibrary *library,
	AxisStorage *storage
){
	AxisWebLibrary *self;

	g_return_val_if_fail(library != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisWebLibrary, 1);
	self->library = axis_library_ref(library);
	self->storage = axis_storage_ref(storage);
	return self;
}

void
axis_web_library_stop(AxisWebLibrary *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_library_free(AxisWebLibrary *self)
{
	if (self == NULL)
		return;

	axis_web_library_stop(self);
	axis_library_unref(self->library);
	axis_storage_unref(self->storage);
	g_free(self);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
scan_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebLibrary *self = user_data;

	return axis_library_run_scan_job(self->library, context, error);
}

static gboolean
due_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebLibrary *self = user_data;

	return axis_library_run_due_job(self->library, context, error);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_get(web, "/storage/roots/:id", handle_root, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/roots/:id/scan", AXIS_WEB_ROUTE_ADMIN,
		handle_scan, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/roots/:id/settings", AXIS_WEB_ROUTE_ADMIN,
		handle_settings, user_data);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_LIBRARY_SCAN_KIND, AXIS_LIBRARY_SCAN_QUEUE, AXIS_LIBRARY_SCAN_ATTEMPTS, scan_handler,
	  "Scanning a library folder", AXIS_JOB_CLASS_WORK },
	/* One attempt: the next turn comes a minute later anyway. */
	{ AXIS_LIBRARY_DUE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, due_handler,
	  "Checking which library folders are due a scan", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_LIBRARY_DUE_SCHEDULE, AXIS_LIBRARY_DUE_KIND, NULL, AXIS_LIBRARY_DUE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const gchar *const depends_on[] = { AXIS_STORAGE_MODULE_ID, NULL };

const AxisModuleInfo *
axis_web_library_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_LIBRARY_MODULE_ID;
		info.name = "Library folder scans";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.description = "Keeps a record of every file in the library folders: scans each library folder on "
			"its schedule, notices new, changed, moved and missing files, and marks hard-linked files and "
			"files still downloading as pinned, so that they are never converted. It only reads the disks.";
		info.depends_on = depends_on;
	}

	return &info;
}
