/*
 * axis-web-browser-pages.c - the Library pages: overview, folders and the
 * flat file list
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything a request carries is parsed strictly before any work is
 * done: ids and cursors are digits, the list's filters go through
 * axis_library_filter_parse(), and a query with anything else in it is a
 * 400.  Nothing from a file name is ever put in a URL or an attribute: a
 * folder is addressed by its row id and a page's cursor is a row id.
 */

#include "web/axis-web-browser-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-library-name.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-titles.h"

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxResponse *
bad_request(HtmxRequest *request)
{
	return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
}

/* A page of htmx content alone, for a fragment the page asked for. */
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

/* A positive number made only of digits (at most 18, no leading zero). */
static gboolean
parse_id(
	const gchar *text,
	gint64      *value
){
	gint64 result = 0;
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length == 0 || length > 18 || text[0] == '0')
		return FALSE;
	for (i = 0; i < length; i++) {
		if (text[i] < '0' || text[i] > '9')
			return FALSE;
		result = result * 10 + (text[i] - '0');
	}
	*value = result;
	return TRUE;
}

/*
 * The query of a page that takes only the named parameters (the others it
 * may be given are the keys of @numeric: digits, @keys: any text from
 * the fixed list the page checks itself).  Returns %FALSE for anything
 * else.
 */
static gboolean
check_query(
	HtmxRequest        *request,
	const gchar *const *numeric,
	const gchar *const *plain
){
	GHashTable *table = htmx_request_get_query_params(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (table == NULL)
		return TRUE;
	g_hash_table_iter_init(&iter, table);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 ignored;

		if (g_strv_contains(numeric, key)) {
			if (!parse_id(value, &ignored))
				return FALSE;
		} else if (!g_strv_contains(plain, key)) {
			return FALSE;
		}
	}
	return TRUE;
}

static const gchar *
done_message(
	HtmxRequest *request,
	gchar      **text
){
	const gchar *key = htmx_request_get_query_param(request, "done");
	gint64 n = 0;

	*text = NULL;
	if (g_strcmp0(key, "scan-requested") == 0)
		return "A scan has been requested. It runs as soon as its disk's turn comes; this page shows "
			"it once it has.";
	if (g_strcmp0(key, "reprobe") == 0) {
		gboolean capped = g_strcmp0(htmx_request_get_query_param(request, "capped"), "1") == 0;

		if (!parse_id(htmx_request_get_query_param(request, "n"), &n))
			n = 0;
		if (n == 0)
			return "There are no video or audio files in this folder for Axis to read again.";
		*text = capped ?
			g_strdup_printf("%" G_GINT64_FORMAT " %s queued to be read again. This is as many "
				"as one request may ask for; there are more files in this folder than that. Ask again "
				"once these are done to queue the rest.", n, n == 1 ? "file was" : "files were") :
			g_strdup_printf("%" G_GINT64_FORMAT " %s queued to be read again. Axis reads them "
				"one disk at a time.", n, n == 1 ? "file was" : "files were");
		return *text;
	}
	return NULL;
}

static const AxisRootOverview *
overview_of(
	GPtrArray *overview,
	gint64     root_id
){
	guint i;

	for (i = 0; overview != NULL && i < overview->len; i++) {
		const AxisRootOverview *root = g_ptr_array_index(overview, i);

		if (root->root_id == root_id)
			return root;
	}
	return NULL;
}

/* A POST form with one button, or the same button disabled with a reason
 * beside it when the volume is not there. */
static HtmxNode *
action_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *aria,
	const gchar *css,
	gboolean     enabled,
	const gchar *hidden_name,
	const gchar *hidden_value
){
	if (enabled) {
		HtmxNode *form = axis_web_auth_post_form(request, action);
		HtmxNode *button = axis_web_auth_button(label, css);

		if (aria != NULL)
			htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		if (hidden_name != NULL) {
			HtmxInput *hidden = htmx_input_new(HTMX_INPUT_HIDDEN);

			htmx_input_set_name(hidden, hidden_name);
			htmx_input_set_value(hidden, hidden_value);
			BROWSER_ADD(form, hidden);
		}
		axis_web_auth_add(form, button);
		return form;
	} else {
		HtmxButton *button = htmx_button_new_submit();
		HtmxSpan *span = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(button), label);
		if (aria != NULL)
			htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
		htmx_button_set_disabled(button, TRUE);
		htmx_element_set_attribute(HTMX_ELEMENT(button), "title",
			"The disk is offline: mount it, then check it on its page");
		BROWSER_ADD(span, button);
		return HTMX_NODE(span);
	}
}

/* ── The overview ───────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *roots;	/* AxisRootInfo */
	GPtrArray *overview;	/* AxisRootOverview */
	GError    *error;
	gboolean   fragment;
} OverviewWork;

static void
overview_work_free(gpointer data)
{
	OverviewWork *work = data;

	g_clear_pointer(&work->roots, g_ptr_array_unref);
	g_clear_pointer(&work->overview, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
overview_run(
	AxisWebBrowser *self,
	gpointer        data
){
	OverviewWork *work = data;

	if (!axis_storage_list_roots(self->storage, 0, &work->roots, &work->error))
		return;
	axis_library_get_overview(self->library, &work->overview, &work->error);
}

static HtmxNode *
roots_table(
	HtmxRequest          *request,
	const AxisVolumeInfo *volume,
	GPtrArray            *roots,
	GPtrArray            *overview,
	gboolean              admin,
	AxisWebNotice        *placed
){
	/* A root's scan, while one runs, is shown under its name. */
	static const AxisWebColumn columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Last complete scan", AXIS_WEB_COL_DATE },
		{ "Look inside", AXIS_WEB_COL_LINKS },
		{ NULL, 0 }
	};
	static const AxisWebColumn admin_columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Last complete scan", AXIS_WEB_COL_DATE },
		{ "Look inside", AXIS_WEB_COL_LINKS },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *table = admin ? axis_web_table_new("roots-admin", admin_columns) :
		axis_web_table_new("roots", columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxNode *here;
	gboolean usable = axis_volume_state_is_usable(volume->state);
	guint i;

	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		const AxisRootOverview *state;
		g_autofree gchar *row_id = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *files_href = NULL;
		g_autofree gchar *probe_href = NULL;
		g_autofree gchar *files_aria = g_strdup_printf("Files in %s", root->name);
		g_autofree gchar *read_aria = g_strdup_printf("Files read in %s", root->name);
		HtmxNode *files_link;
		HtmxNode *read_link;
		HtmxTr *row;
		HtmxTd *name_cell;
		HtmxTd *scan_cell;
		HtmxTd *more;
		HtmxSpan *separator;

		if (root->volume_id != volume->id)
			continue;
		state = overview_of(overview, root->id);
		row_id = g_strdup_printf("root-%" G_GINT64_FORMAT "-row", root->id);
		href = g_strdup_printf("/library/root/%" G_GINT64_FORMAT, root->id);
		files_href = g_strdup_printf("/library/files?root=%" G_GINT64_FORMAT, root->id);
		probe_href = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", root->id);
		row = htmx_tr_new();
		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		name_cell = htmx_td_new();
		/* A notice for this root comes first in its cell. */
		here = placed != NULL ? axis_web_notice_take(placed, row_id) : NULL;
		if (here != NULL)
			BROWSER_ADD(name_cell, here);
		BROWSER_ADD(name_cell, axis_web_browser_link(href, root->name));
		/* The scan, under the name: its progress while it runs (the live
		 * updates replace it by its id), or why the latest did not
		 * complete. */
		scan_cell = name_cell;
		if (state != NULL) {
			BROWSER_ADD(scan_cell, axis_web_browser_progress_block(state));
			if (!state->running && state->has_last && state->last_status != AXIS_SCAN_COMPLETE)
				BROWSER_ADD(scan_cell, axis_web_browser_badge(
					state->last_status == AXIS_SCAN_UNAVAILABLE ? "Latest scan: disk offline" :
					"Latest scan did not complete", "badge-warn"));
		}
		BROWSER_ADD(row, name_cell);
		BROWSER_ADD(row, htmx_td_new_with_text(axis_media_type_label(root->media_type)));

		if (state != NULL && state->last_complete_scan_id > 0) {
			g_autofree gchar *files = axis_web_browser_format_count(state->files);
			g_autofree gchar *size = axis_web_browser_format_size(state->bytes);
			g_autofree gchar *when = axis_web_storage_format_time(state->last_complete_us, "unknown");

			BROWSER_ADD(row, htmx_td_new_with_text(files));
			BROWSER_ADD(row, htmx_td_new_with_text(size));
			BROWSER_ADD(row, htmx_td_new_with_text(when));
		} else {
			BROWSER_ADD(row, htmx_td_new_with_text("-"));
			BROWSER_ADD(row, htmx_td_new_with_text("-"));
			BROWSER_ADD(row, htmx_td_new_with_text("Never"));
		}

		more = htmx_td_new();
		separator = htmx_span_new();
		htmx_node_set_text_content(HTMX_NODE(separator), " | ");
		files_link = axis_web_browser_link(files_href, "Files");
		htmx_element_set_attribute(HTMX_ELEMENT(files_link), "aria-label", files_aria);
		read_link = axis_web_browser_link(probe_href, "Files read");
		htmx_element_set_attribute(HTMX_ELEMENT(read_link), "aria-label", read_aria);
		BROWSER_ADD(more, files_link);
		BROWSER_ADD(more, separator);
		BROWSER_ADD(more, read_link);
		BROWSER_ADD(row, more);

		if (admin) {
			g_autofree gchar *action = g_strdup_printf("/library/root/%" G_GINT64_FORMAT "/scan", root->id);
			g_autofree gchar *scan_aria = g_strdup_printf("Scan %s now", root->name);
			HtmxTd *cell = htmx_td_new();

			BROWSER_ADD(cell, action_form(request, action, "Scan now", scan_aria, NULL, usable, NULL, NULL));
			BROWSER_ADD(row, cell);
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	return table;
}

static HtmxNode *
overview_list(
	HtmxRequest  *request,
	GPtrArray    *volumes,
	OverviewWork *work,
	AxisWebNotice *placed
){
	HtmxDiv *box = htmx_div_new();
	gboolean admin = axis_web_request_is_admin(request);
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(box), "library-list");
	/* After a scan starts or ends, a volume changes state or the stream
	 * opens (or reopens), the list fetches itself again, rendered for
	 * whoever is looking. */
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", "/library/list");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger",
		"sse:" AXIS_WEB_BROWSER_EVENT_REFRESH " throttle:2s, htmx:sseOpen from:body");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");

	if (volumes->len == 0) {
		HtmxNode *storage_link = axis_web_browser_link("/storage", "Disks and library folders");

		BROWSER_ADD(box, axis_web_storage_text("help", "No disks have been added yet. An administrator "
			"adds them under:"));
		BROWSER_ADD(box, storage_link);
	}

	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);
		HtmxSection *card = htmx_section_new();
		HtmxHeading *heading = htmx_h2_new();
		HtmxDiv *line = htmx_div_new();
		g_autofree gchar *href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, volume->id);

		/* A card whose heading is the volume's name, as a link; under it,
		 * its state and its space on one line. */
		htmx_element_add_class(HTMX_ELEMENT(card), "card");
		BROWSER_ADD(heading, axis_web_browser_link(href, volume->name));
		BROWSER_ADD(card, heading);
		htmx_element_add_class(HTMX_ELEMENT(line), "state-line");
		BROWSER_ADD(line, axis_web_storage_state_block(volume));
		BROWSER_ADD(line, axis_web_storage_space_block(volume));
		BROWSER_ADD(card, line);
		if (!axis_volume_state_is_usable(volume->state))
			BROWSER_ADD(card, axis_web_storage_text("help", "This disk is offline. What is shown "
				"is what was last known of it; scans and file reads wait until it is back, and nothing "
				"recorded about its files changes while it is away."));
		if (volume->n_roots == 0) {
			g_autofree gchar *disk_text = g_strdup_printf("Disk %s", volume->name);

			BROWSER_ADD(card, axis_web_storage_text("help", "No library folders on this disk. An "
				"administrator adds them on the disk's page:"));
			BROWSER_ADD(card, axis_web_browser_link(href, disk_text));
		} else
			BROWSER_ADD(card, roots_table(request, volume, work->roots, work->overview, admin, placed));
		BROWSER_ADD(box, card);
	}

	return HTMX_NODE(box);
}

static HtmxResponse *
overview_done(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
){
	OverviewWork *work = data;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *text = NULL;
	const gchar *notice;
	HtmxHeading *title;
	AxisWebNotice placed;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	volumes = axis_storage_list_volumes(self->storage);
	if (work->fragment)
		return fragment_response(overview_list(request, volumes, work, NULL));

	notice = done_message(request, &text);
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "Files");
	BROWSER_ADD(content, title);
	BROWSER_ADD(content, axis_web_page_intro("What is on each disk, folder by folder and file by file, as Axis "
		"last recorded it: to find a file, see what is inside it, or see what a folder holds. The counts and sizes "
		"are those of each library folder's last complete scan; a scan that does not complete changes none of "
		"them."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	BROWSER_ADD(content, overview_list(request, volumes, work, &placed));
	BROWSER_ADD(content, axis_web_storage_text(NULL, "Every file, with filters:"));
	BROWSER_ADD(content, axis_web_browser_link("/library/files", "All files"));
	axis_web_notice_end(&placed);

	axis_web_page_add_topic(request, AXIS_WEB_BROWSER_TOPIC);
	return axis_web_page_respond(request, "Files", HTMX_NODE(content));
}

static HtmxResponse *
overview_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     fragment
){
	static const gchar *const numeric[] = { "n", NULL };
	static const gchar *const plain[] = { "done", "capped", "at", NULL };
	OverviewWork *work;

	if (!check_query(request, numeric, plain))
		return bad_request(request);
	if (fragment && !htmx_request_is_htmx(request))
		return axis_web_auth_redirect(request, "/library");

	work = g_new0(OverviewWork, 1);
	work->fragment = fragment;
	return axis_web_browser_dispatch(user_data, request, overview_run, overview_done, work,
		overview_work_free);
}

static HtmxResponse *
handle_overview(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return overview_handle(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_overview_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return overview_handle(request, params, user_data, TRUE);
}

/* ── A folder ───────────────────────────────────────────────────── */

typedef struct
{
	gint64          dir_id;		/* the folder, or 0 with @root_id */
	gint64          root_id;	/* a root's top folder (0: by @dir_id) */
	gint64          after_dir;
	gint64          after_file;
	gchar          *name;		/* the subfolders' name filter, or NULL */
	gchar          *view_asked;	/* the view in the address, or NULL */
	gint64          user_id;
	/* Per media type: what its folders are may be asked (its module is on). */
	gboolean        asked[AXIS_MEDIA_TYPE_COUNT];
	AxisFolderView *view;
	AxisRootInfo   *root;
	GPtrArray      *titles;		/* AxisWebFolderTitle, or NULL */
	gboolean        views;		/* some subfolders hold a title: the views */
	AxisWebView     shown;		/* with @views */
	GError         *error;
} FolderWork;

static void
folder_work_free(gpointer data)
{
	FolderWork *work = data;

	axis_folder_view_free(work->view);
	axis_root_info_free(work->root);
	if (work->titles != NULL)
		g_ptr_array_unref(work->titles);
	g_free(work->name);
	g_free(work->view_asked);
	g_clear_error(&work->error);
	g_free(work);
}

/* The account's choice of view is kept per media type. */
static const gchar *
folder_list_name(AxisMediaType type)
{
	return type == AXIS_MEDIA_TV ? "library-series" : "library-films";
}

static const AxisWebFolderTitle *
folder_title(
	const FolderWork *work,
	gint64            dir_id
){
	guint i;

	for (i = 0; work->titles != NULL && i < work->titles->len; i++) {
		const AxisWebFolderTitle *item = g_ptr_array_index(work->titles, i);

		if (item->dir_id == dir_id)
			return item;
	}
	return NULL;
}

/* What the folder and its subfolders on this page are, when their root's
 * kind says it and its module is on. */
static void
folder_ask_titles(
	AxisWebBrowser *self,
	FolderWork     *work
){
	const AxisWebBrowserTitles *provider;
	g_autoptr(GPtrArray) dirs = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	if (work->root == NULL || work->view == NULL || (guint)work->root->media_type >= AXIS_MEDIA_TYPE_COUNT ||
	    !work->asked[work->root->media_type])
		return;
	provider = &self->titles[work->root->media_type];
	if (provider->func == NULL)
		return;
	dirs = g_ptr_array_new();
	for (i = 0; i < work->view->subdirs->len; i++)
		g_ptr_array_add(dirs, g_ptr_array_index(work->view->subdirs, i));
	g_ptr_array_add(dirs, work->view->dir);
	work->titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_web_folder_title_free);
	if (!provider->func(provider->user_data, work->root->id, dirs, work->titles, &error)) {
		g_warning("library: what the folders of root %" G_GINT64_FORMAT " hold could not be read: %s",
			work->root->id, error != NULL ? error->message : "no reason given");
		g_clear_pointer(&work->titles, g_ptr_array_unref);
		return;
	}
	for (i = 0; i < work->titles->len && !work->views; i++) {
		const AxisWebFolderTitle *item = g_ptr_array_index(work->titles, i);

		work->views = item->dir_id != work->view->dir->id;
	}
	if (work->views)
		axis_web_view_choose(axis_storage_get_pool(self->storage), work->user_id,
			folder_list_name(work->root->media_type), work->view_asked, &work->shown);
}

static void
folder_run(
	AxisWebBrowser *self,
	gpointer        data
){
	FolderWork *work = data;

	if (work->root_id > 0) {
		if (!axis_storage_get_root(self->storage, work->root_id, &work->root, &work->error))
			return;
		if (!axis_library_find_root_dir(self->library, work->root_id, &work->dir_id, &work->error))
			return;
		if (work->dir_id == 0)
			return;
	}
	if (!axis_library_get_folder_named(self->library, work->dir_id, work->name, work->after_dir, work->after_file,
	                                   &work->view, &work->error))
		return;
	if (work->root == NULL)
		axis_storage_get_root(self->storage, work->view->dir->root_id, &work->root, NULL);
	folder_ask_titles(self, work);
}

static HtmxNode *
breadcrumbs(const FolderWork *work)
{
	HtmxNav *nav = htmx_nav_new();
	HtmxP *line = htmx_p_new();
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Breadcrumb");
	htmx_element_add_class(HTMX_ELEMENT(line), "crumbs");
	BROWSER_ADD(line, axis_web_browser_link("/library", "Files"));

	if (work->view == NULL) {
		/* A root nothing has been scanned of. */
		HtmxSpan *separator = htmx_span_new();
		HtmxSpan *name = htmx_span_new();

		htmx_node_set_text_content(HTMX_NODE(separator), " / ");
		BROWSER_ADD(line, separator);
		htmx_node_set_text_content(HTMX_NODE(name), work->root != NULL ? work->root->name : "");
		BROWSER_ADD(line, name);
		BROWSER_ADD(nav, line);
		return HTMX_NODE(nav);
	}

	for (i = 0; i < work->view->crumbs->len; i++) {
		const AxisDirRecord *dir = g_ptr_array_index(work->view->crumbs, i);
		g_autofree gchar *href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, dir->id);
		HtmxSpan *separator = htmx_span_new();
		gboolean last = i + 1 == work->view->crumbs->len;
		HtmxNode *label;

		htmx_node_set_text_content(HTMX_NODE(separator), " / ");
		BROWSER_ADD(line, separator);
		if (dir->path[0] == '\0') {
			HtmxSpan *name = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(name), work->root != NULL ? work->root->name : "Library folder");
			label = HTMX_NODE(name);
		} else {
			label = axis_web_browser_name_node(dir->name);
		}
		if (last) {
			BROWSER_ADD(line, label);
		} else {
			HtmxA *link = htmx_a_new_with_href(href);

			BROWSER_ADD(link, label);
			BROWSER_ADD(line, link);
		}
	}
	BROWSER_ADD(nav, line);
	return HTMX_NODE(nav);
}

/*
 * The folder's address: the name filter and the view as they are (or
 * @name as given), and the two cursors as given.
 */
static gchar *
folder_href_full(
	const FolderWork *work,
	const gchar      *name,
	const gchar      *view,
	gint64            after_dir,
	gint64            after_file
){
	GString *href = g_string_new(NULL);
	gboolean first = TRUE;

	g_string_append_printf(href, "/library/folder/%" G_GINT64_FORMAT, work->view->dir->id);
	if (name != NULL && name[0] != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);

		g_string_append_printf(href, "%cq=%s", first ? '?' : '&', escaped);
		first = FALSE;
	}
	if (view != NULL) {
		g_string_append_printf(href, "%cview=%s", first ? '?' : '&', view);
		first = FALSE;
	}
	if (after_dir > 0) {
		g_string_append_printf(href, "%cafter_dir=%" G_GINT64_FORMAT, first ? '?' : '&', after_dir);
		first = FALSE;
	}
	if (after_file > 0)
		g_string_append_printf(href, "%cafter_file=%" G_GINT64_FORMAT, first ? '?' : '&', after_file);
	return g_string_free(href, FALSE);
}

/* The view a link keeps: the one shown, when there is a choice. */
static const gchar *
folder_view_kept(const FolderWork *work)
{
	return work->views ? axis_web_view_name(work->shown) : NULL;
}

/* The query that keeps the filter, the view and one cursor and replaces
 * the other. */
static gchar *
folder_href(
	const FolderWork *work,
	gint64            after_dir,
	gint64            after_file
){
	return folder_href_full(work, work->name, folder_view_kept(work), after_dir, after_file);
}

/* "3 files, 4.0 GiB" */
static gchar *
folder_figures(const AxisDirRecord *dir)
{
	g_autofree gchar *files = axis_web_browser_format_count(dir->total_files);
	g_autofree gchar *size = axis_web_browser_format_size(dir->total_bytes);

	return g_strdup_printf("%s %s, %s", files, dir->total_files == 1 ? "file" : "files", size);
}

/* What a title folder holds, by the kind of library folder it is in. */
static const gchar *
title_word(const AxisRootInfo *root)
{
	if (root != NULL && root->media_type == AXIS_MEDIA_TV)
		return "Series";
	if (root != NULL && root->media_type == AXIS_MEDIA_MOVIES)
		return "Movie";
	return "Title";
}

/*
 * What a folder that is not identified says: a quiet mark, a link to its
 * unit on the Identify page when it has one.
 */
static HtmxNode *
folder_mark(
	const AxisWebFolderTitle *item,
	const AxisRootInfo       *root
){
	const gchar *words = item->not_title ? (root != NULL && root->media_type == AXIS_MEDIA_TV ?
		"Not a series" : "Not a movie") : "Not identified";
	HtmxNode *mark;

	if (item->unit_id > 0) {
		g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, item->unit_id);

		mark = HTMX_NODE(htmx_a_new_with_href(href));
	} else {
		mark = HTMX_NODE(htmx_span_new());
	}
	htmx_element_add_class(HTMX_ELEMENT(mark), "film-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), "folder-mark");
	htmx_node_set_text_content(mark, words);
	return mark;
}

/* "Hearth (1995)", linking to the title's page. */
static HtmxNode *
folder_title_link(const AxisMetaTitle *title)
{
	g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, title->id);
	g_autofree gchar *words = title->year > 0 ? g_strdup_printf("%s (%d)", title->title, title->year) :
		g_strdup(title->title);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *text = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(text), words);
	BROWSER_ADD(link, text);
	return HTMX_NODE(link);
}

static HtmxNode *
subdirs_table(const FolderWork *work)
{
	static const AxisWebColumn columns[] = {
		{ "Folder", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Subfolders", AXIS_WEB_COL_NUMBER },
		{ NULL, 0 }
	};
	/* Under a films or series root: what each holds, beside its name. */
	AxisWebColumn title_columns[] = {
		{ "Folder", AXIS_WEB_COL_NAME },
		{ "Title", AXIS_WEB_COL_WIDE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Subfolders", AXIS_WEB_COL_NUMBER },
		{ NULL, 0 }
	};
	const AxisFolderView *view = work->view;
	HtmxNode *table;
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	title_columns[1].heading = title_word(work->root);
	table = axis_web_table_new("folders", work->views ? title_columns : columns);

	for (i = 0; i < view->subdirs->len; i++) {
		const AxisDirRecord *dir = g_ptr_array_index(view->subdirs, i);
		g_autofree gchar *href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, dir->id);
		g_autofree gchar *files = axis_web_browser_format_count(dir->total_files);
		g_autofree gchar *size = axis_web_browser_format_size(dir->total_bytes);
		g_autofree gchar *subdirs = axis_web_browser_format_count(dir->subdirs);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(name), "scan-path");
		BROWSER_ADD(link, axis_web_browser_name_node(dir->name));
		BROWSER_ADD(name, link);
		BROWSER_ADD(row, name);
		if (work->views) {
			const AxisWebFolderTitle *item = folder_title(work, dir->id);
			HtmxTd *cell = htmx_td_new();

			if (item != NULL && item->title != NULL)
				BROWSER_ADD(cell, folder_title_link(item->title));
			else if (item != NULL)
				BROWSER_ADD(cell, folder_mark(item, work->root));
			BROWSER_ADD(row, cell);
		}
		BROWSER_ADD(row, htmx_td_new_with_text(files));
		BROWSER_ADD(row, htmx_td_new_with_text(size));
		BROWSER_ADD(row, htmx_td_new_with_text(subdirs));
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	return table;
}

/*
 * The subfolders as posters or an overview: each its title's poster and
 * name when it holds one, else its own name; the folder's name and figures
 * under it; the mark of one not identified.  Every row links to the folder.
 */
static HtmxNode *
subdirs_titles(const FolderWork *work)
{
	const AxisFolderView *view = work->view;
	g_autoptr(GPtrArray) kept = g_ptr_array_new_with_free_func(g_free);
	AxisWebTitleRow *rows = g_new0(AxisWebTitleRow, MAX(view->subdirs->len, 1));
	AxisMetaKind kind = work->root != NULL && work->root->media_type == AXIS_MEDIA_TV ? AXIS_META_KIND_SERIES :
		AXIS_META_KIND_FILM;
	HtmxNode *node;
	guint i;

	for (i = 0; i < view->subdirs->len; i++) {
		const AxisDirRecord *dir = g_ptr_array_index(view->subdirs, i);
		const AxisWebFolderTitle *item = folder_title(work, dir->id);
		const AxisMetaTitle *title = item != NULL ? item->title : NULL;
		gchar *href = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, dir->id);
		gchar *shown = axis_library_name_display(dir->name);
		g_autofree gchar *figures = folder_figures(dir);
		gchar *facts = title != NULL ? g_strdup_printf("%s \xc2\xb7 %s", shown, figures) : g_strdup(figures);
		AxisWebTitleRow *row = &rows[i];

		g_ptr_array_add(kept, href);
		g_ptr_array_add(kept, shown);
		g_ptr_array_add(kept, facts);
		row->href = href;
		row->title = title != NULL && title->title != NULL ? title->title : shown;
		row->year = title != NULL ? title->year : 0;
		row->kind = title != NULL ? title->kind : kind;
		row->provider = title != NULL ? title->provider : NULL;
		row->poster_ref = title != NULL ? title->poster_ref : NULL;
		row->poster = item != NULL ? item->poster : NULL;
		/* A folder that is no title yet is not called a film. */
		row->line = title == NULL ? "Folder" : NULL;
		row->size = -1;
		/* An overview's line of facts; a poster's two lines under its name. */
		if (work->shown == AXIS_WEB_VIEW_OVERVIEW)
			row->facts = facts;
		else
			row->reason = facts;
		if (item != NULL && title == NULL)
			row->mark = folder_mark(item, work->root);
	}
	node = axis_web_titles_new(work->shown, "Folder", title_word(work->root), rows, view->subdirs->len);
	for (i = 0; i < view->subdirs->len; i++)
		axis_web_title_row_clear(&rows[i]);
	g_free(rows);
	return node;
}

/*
 * The filter line of the subfolders: the name box with its button (a GET
 * form that keeps the view), and at its right the views when there is a
 * choice, each a link, the current one marked as a tab is.
 */
static HtmxNode *
folder_bar(const FolderWork *work)
{
	static const gchar *const view_labels[AXIS_WEB_VIEW_N] = { "Posters", "Overview", "Table" };
	HtmxDiv *bar = htmx_div_new();
	HtmxForm *form = htmx_form_new();
	HtmxLabel *label = htmx_label_new_for("folder-name");
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);
	HtmxButton *button = htmx_button_new_submit();
	g_autofree gchar *action = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, work->view->dir->id);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(bar), "list-bar");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "role", "search");
	htmx_element_add_class(HTMX_ELEMENT(form), "list-filter");
	htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(label), "Filter the folders by name");
	BROWSER_ADD(form, label);
	htmx_element_set_id(HTMX_ELEMENT(input), "folder-name");
	htmx_input_set_name(input, "q");
	htmx_input_set_placeholder(input, "Filter by name");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "100");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	if (work->name != NULL)
		htmx_input_set_value(input, work->name);
	BROWSER_ADD(form, input);
	if (work->views) {
		HtmxInput *kept = htmx_input_new(HTMX_INPUT_HIDDEN);

		htmx_input_set_name(kept, "view");
		htmx_input_set_value(kept, axis_web_view_name(work->shown));
		BROWSER_ADD(form, kept);
	}
	htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
	htmx_node_set_text_content(HTMX_NODE(button), "Filter");
	BROWSER_ADD(form, button);
	if (work->name != NULL) {
		g_autofree gchar *all = folder_href_full(work, NULL, folder_view_kept(work), 0, 0);

		BROWSER_ADD(form, axis_web_browser_link(all, "Clear the name filter"));
	}
	BROWSER_ADD(bar, form);

	if (work->views) {
		HtmxDiv *choices = htmx_div_new();
		HtmxDiv *views = htmx_div_new();
		HtmxSpan *words = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(choices), "list-choices");
		htmx_element_add_class(HTMX_ELEMENT(views), "list-links");
		htmx_element_set_attribute(HTMX_ELEMENT(views), "role", "group");
		htmx_element_set_attribute(HTMX_ELEMENT(views), "aria-label", "View");
		htmx_element_add_class(HTMX_ELEMENT(words), "list-links-label");
		htmx_node_set_text_content(HTMX_NODE(words), "View");
		BROWSER_ADD(views, words);
		for (i = 0; i < AXIS_WEB_VIEW_N; i++) {
			/* The same folders, so the same page. */
			g_autofree gchar *href = folder_href_full(work, work->name, axis_web_view_name((AxisWebView)i),
				work->after_dir, work->after_file);
			HtmxA *link = htmx_a_new_with_href(href);

			htmx_node_set_text_content(HTMX_NODE(link), view_labels[i]);
			if (work->shown == (AxisWebView)i) {
				htmx_element_add_class(HTMX_ELEMENT(link), "current");
				htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "true");
			}
			BROWSER_ADD(views, link);
		}
		BROWSER_ADD(choices, views);
		BROWSER_ADD(bar, choices);
	}
	return HTMX_NODE(bar);
}

static HtmxResponse *
folder_done(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
){
	FolderWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autofree gchar *text = NULL;
	const AxisFolderView *view = work->view;
	const gchar *notice;
	HtmxHeading *title;
	HtmxNode *card;
	HtmxNode *facts;
	gboolean admin = axis_web_request_is_admin(request);
	gboolean usable = TRUE;
	const gchar *heading;
	g_autofree gchar *shown_heading = NULL;
	gint64 root_id;
	AxisWebNotice placed;

	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND) ||
	    g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
			return axis_web_error_respond(request, 503, "Too slow",
				"This folder could not be read in time. Try again in a moment.");
		return axis_web_internal_error(request, work->error);
	}

	root_id = view != NULL ? view->dir->root_id : work->root_id;
	if (work->root != NULL)
		volume = axis_storage_get_volume(self->storage, work->root->volume_id);
	if (volume != NULL)
		usable = axis_volume_state_is_usable(volume->state);

	content = htmx_div_new();
	BROWSER_ADD(content, breadcrumbs(work));

	title = htmx_h1_new();
	if (view == NULL || view->dir->path[0] == '\0') {
		heading = work->root != NULL ? work->root->name : "Library folder";
		htmx_node_set_text_content(HTMX_NODE(title), heading);
	} else {
		BROWSER_ADD(title, axis_web_browser_name_node(view->dir->name));
		heading = shown_heading = axis_library_name_display(view->dir->name);
	}
	BROWSER_ADD(content, title);

	notice = done_message(request, &text);
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	if (volume != NULL && !usable) {
		g_autofree gchar *warning = g_strdup_printf("Disk '%s' is not online: %s What is shown is what was "
			"last known of it. Scans and file reads wait until it is back.", volume->name,
			axis_volume_reason_describe(volume->reason));

		BROWSER_ADD(content, axis_web_browser_warning(warning));
	}

	if (view == NULL) {
		g_autofree gchar *action = g_strdup_printf("/library/root/%" G_GINT64_FORMAT "/scan", root_id);

		card = axis_web_storage_card("Not scanned yet");
		htmx_element_set_id(HTMX_ELEMENT(card), "folder-actions");
		axis_web_notice_put(&placed, card, "folder-actions");
		BROWSER_ADD(card, axis_web_storage_text(NULL, "No scan of this library folder has completed yet, so "
			"there is nothing to show. Axis scans it on its schedule while its disk is online."));
		if (admin)
			BROWSER_ADD(card, action_form(request, action, "Scan this library folder now", NULL,
				"button-primary", usable, NULL, NULL));
		BROWSER_ADD(content, card);
		axis_web_notice_end(&placed);
		return axis_web_page_respond(request, heading, HTMX_NODE(content));
	}

	card = axis_web_storage_card("In this folder");
	facts = axis_web_browser_facts_new();
	{
		g_autofree gchar *direct_files = axis_web_browser_format_count(view->dir->direct_files);
		g_autofree gchar *direct_size = axis_web_browser_format_size(view->dir->direct_bytes);
		g_autofree gchar *total_files = axis_web_browser_format_count(view->dir->total_files);
		g_autofree gchar *total_size = axis_web_browser_format_size(view->dir->total_bytes);
		g_autofree gchar *subdirs = axis_web_browser_format_count(view->dir->subdirs);
		g_autofree gchar *direct = g_strdup_printf("%s (%s)", direct_files, direct_size);
		g_autofree gchar *total = g_strdup_printf("%s (%s)", total_files, total_size);
		g_autofree gchar *when = axis_web_storage_format_time(view->scan_us, "unknown");
		g_autofree gchar *asof = g_strdup_printf("These totals are from the last complete scan of "
			"the library folder, %s. The lists below are what Axis has on record now.", when);

		const AxisWebFolderTitle *own = folder_title(work, view->dir->id);

		/* What the folder itself holds, when it is a title's folder. */
		if (own != NULL && own->title != NULL)
			axis_web_browser_fact_node(facts, title_word(work->root), folder_title_link(own->title));
		else if (own != NULL)
			axis_web_browser_fact_node(facts, title_word(work->root), folder_mark(own, work->root));
		axis_web_browser_fact(facts, "Files directly here", direct);
		axis_web_browser_fact(facts, "Files here and below", total);
		axis_web_browser_fact(facts, "Subfolders", subdirs);
		BROWSER_ADD(card, facts);
		BROWSER_ADD(card, axis_web_storage_text("help", asof));
	}
	BROWSER_ADD(content, card);

	if (admin) {
		g_autofree gchar *scan = g_strdup_printf("/library/root/%" G_GINT64_FORMAT "/scan", root_id);
		g_autofree gchar *probe = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT "/probe",
			view->dir->id);
		g_autofree gchar *folder = g_strdup_printf("%" G_GINT64_FORMAT, view->dir->id);
		HtmxDiv *actions = htmx_div_new();

		card = axis_web_storage_card("Actions for this folder");
		htmx_element_set_id(HTMX_ELEMENT(card), "folder-actions");
		axis_web_notice_put(&placed, card, "folder-actions");
		htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
		BROWSER_ADD(actions, action_form(request, scan, "Scan the whole library folder now", NULL, NULL,
			usable, "folder", folder));
		BROWSER_ADD(actions, action_form(request, probe, "Read the video and audio files here again", NULL,
			NULL, usable, NULL, NULL));
		BROWSER_ADD(card, actions);
		BROWSER_ADD(card, axis_web_storage_text("help", "These only ask for work to be done; nothing on the "
			"disk is changed. Asking twice queues the work once. A request covers at most 10,000 files; "
			"when a folder holds more, the page says so."));
		BROWSER_ADD(content, card);
	}

	if (view->dir_cursor_gone || view->file_cursor_gone)
		BROWSER_ADD(content, axis_web_browser_warning("The list changed since the page before: it starts "
			"again from the top."));

	/* Every list of folders can be filtered by name. */
	if (view->subdirs->len > 0 || view->dir->subdirs > 0 || work->name != NULL) {
		g_autofree gchar *first = view->dir_cursor_gone || work->after_dir > 0 ?
			folder_href(work, 0, work->after_file) : NULL;
		g_autofree gchar *next = view->next_dir > 0 ?
			folder_href(work, view->next_dir, work->after_file) : NULL;

		card = axis_web_storage_card("Subfolders");
		htmx_element_add_class(HTMX_ELEMENT(card), "folder-list");
		BROWSER_ADD(card, folder_bar(work));
		if (view->subdirs->len == 0 && work->name != NULL) {
			g_autofree gchar *none = g_strdup_printf("No subfolder here has a name that holds \"%s\".",
				work->name);

			BROWSER_ADD(card, axis_web_storage_text("help", none));
		} else if (view->subdirs->len == 0) {
			BROWSER_ADD(card, axis_web_storage_text("help", "No subfolders here."));
		} else if (work->views && work->shown != AXIS_WEB_VIEW_TABLE) {
			BROWSER_ADD(card, subdirs_titles(work));
		} else {
			BROWSER_ADD(card, subdirs_table(work));
		}
		if (first != NULL || next != NULL)
			BROWSER_ADD(card, axis_web_browser_pager(first, next));
		BROWSER_ADD(content, card);
	}

	card = axis_web_storage_card("Files");
	if (view->files->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No files directly in this folder."));
	} else {
		g_autofree gchar *first = view->file_cursor_gone || work->after_file > 0 ?
			folder_href(work, work->after_dir, 0) : NULL;
		g_autofree gchar *next = view->next_file > 0 ?
			folder_href(work, work->after_dir, view->next_file) : NULL;

		BROWSER_ADD(card, axis_web_browser_file_table(view->files, FALSE));
		if (first != NULL || next != NULL)
			BROWSER_ADD(card, axis_web_browser_pager(first, next));
	}
	BROWSER_ADD(content, card);

	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, heading, HTMX_NODE(content));
}

static HtmxResponse *
folder_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     by_root
){
	static const gchar *const numeric[] = { "after_dir", "after_file", "n", NULL };
	static const gchar *const plain[] = { "done", "capped", "q", "view", "at", NULL };
	AxisWebBrowser *self = user_data;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *name = htmx_request_get_query_param(request, "q");
	const gchar *view = htmx_request_get_query_param(request, "view");
	const AxisAuthState *auth = axis_web_auth_state(request);
	AxisWeb *web = axis_web_from_request(request);
	AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;
	AxisWebView ignored;
	FolderWork *work;
	guint i;

	if (id == 0)
		return not_found(request);
	if (!check_query(request, numeric, plain))
		return bad_request(request);
	/* The name filter is text of a bounded length; the view is one there is. */
	if (name != NULL && (strlen(name) > AXIS_LIBRARY_FOLDER_NAME_MAX || !g_utf8_validate(name, -1, NULL)))
		return bad_request(request);
	if (view != NULL && !axis_web_view_from_name(view, &ignored))
		return bad_request(request);

	work = g_new0(FolderWork, 1);
	if (by_root)
		work->root_id = id;
	else
		work->dir_id = id;
	parse_id(htmx_request_get_query_param(request, "after_dir"), &work->after_dir);
	parse_id(htmx_request_get_query_param(request, "after_file"), &work->after_file);
	if (name != NULL) {
		work->name = g_strstrip(g_strdup(name));
		if (work->name[0] == '\0')
			g_clear_pointer(&work->name, g_free);
	}
	work->view_asked = g_strdup(view);
	work->user_id = auth != NULL ? auth->user_id : 0;
	/* What a root's folders hold is asked of a module that is on. */
	for (i = 0; i < AXIS_MEDIA_TYPE_COUNT; i++)
		work->asked[i] = self->titles[i].func != NULL && (registry == NULL ||
			axis_module_registry_is_enabled(registry, self->titles[i].module_id));
	return axis_web_browser_dispatch(user_data, request, folder_run, folder_done, work, folder_work_free);
}

static HtmxResponse *
handle_folder(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return folder_handle(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_root(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return folder_handle(request, params, user_data, TRUE);
}

/* ── The flat list ──────────────────────────────────────────────── */

typedef struct
{
	AxisLibraryFilter  filter;
	GPtrArray         *roots;	/* AxisRootInfo, for the filter form */
	AxisFileList      *list;
	gint64             latest;	/* the newest complete scan, for the notice */
	gboolean           notice;	/* the notice fragment, not the page */
	GError            *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	axis_library_filter_clear(&work->filter);
	g_clear_pointer(&work->roots, g_ptr_array_unref);
	axis_file_list_free(work->list);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebBrowser *self,
	gpointer        data
){
	ListWork *work = data;

	if (work->notice) {
		axis_library_latest_scan(self->library, work->filter.root_id, &work->latest, &work->error);
		return;
	}
	if (!axis_storage_list_roots(self->storage, 0, &work->roots, &work->error))
		return;
	if (!axis_library_latest_scan(self->library, work->filter.root_id, &work->latest, &work->error))
		return;
	/* A target and outcome are the estimator's decision, never ignored. */
	if (axis_library_filter_needs_estimator(&work->filter)) {
		if (self->estimator == NULL || !axis_estimator_is_enabled(self->estimator)) {
			g_set_error_literal(&work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND,
				"the estimate module is switched off");
			return;
		}
		axis_estimator_list_files(self->estimator, &work->filter, &work->list, &work->error);
		return;
	}
	axis_library_list_files(self->library, &work->filter, &work->list, &work->error);
}

/* A select whose option is the current value. */
static HtmxNode *
select_field(
	const gchar         *id,
	const gchar         *label,
	const gchar         *name,
	const gchar *const  *values,
	const gchar *const  *labels,
	const gchar         *current
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);

		if (g_strcmp0(values[i], current != NULL ? current : "") == 0)
			htmx_option_set_selected(option, TRUE);
		BROWSER_ADD(select, option);
	}
	BROWSER_ADD(field, text);
	BROWSER_ADD(field, select);
	return HTMX_NODE(field);
}

/* "", then @values, with @labels (or the values themselves). */
static HtmxNode *
choice_field(
	const gchar         *id,
	const gchar         *label,
	const gchar         *name,
	const gchar *const  *values,
	const gchar *const  *shown,
	const gchar         *current
){
	g_autoptr(GPtrArray) all = g_ptr_array_new();
	guint i;
	HtmxNode *field;

	g_ptr_array_add(all, (gpointer)"");
	for (i = 0; values[i] != NULL; i++)
		g_ptr_array_add(all, (gpointer)values[i]);
	g_ptr_array_add(all, NULL);
	{
		g_autoptr(GPtrArray) labels = g_ptr_array_new();

		g_ptr_array_add(labels, (gpointer)"Any");
		for (i = 0; values[i] != NULL; i++)
			g_ptr_array_add(labels, (gpointer)(shown != NULL ? shown[i] : values[i]));
		g_ptr_array_add(labels, NULL);
		field = select_field(id, label, name, (const gchar *const *)all->pdata,
			(const gchar *const *)labels->pdata, current);
	}
	return field;
}

static gchar *
number_text(gint64 value)
{
	return value >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, value) : NULL;
}

static HtmxNode *
filter_form(
	const AxisLibraryFilter *filter,
	GPtrArray               *roots
){
	static const gchar *const pinned_values[] = { "", "yes", "no", NULL };
	static const gchar *const pinned_labels[] = { "Any", "Pinned", "Not pinned", NULL };
	static const gchar *const direction_values[] = { "asc", "desc", NULL };
	static const gchar *const direction_labels[] = { "Ascending", "Descending", NULL };
	static const gchar *const kind_shown[] = {
		"Video", "Audio", "Subtitle", "Image", "Text or info file", "Other", "Symbolic link", NULL
	};
	static const gchar *const res_shown[] = { "SD", "720p", "1080p", "2160p (4K)", "Other size", NULL };
	static const gchar *const hdr_shown[] = {
		"SDR", "HLG", "HDR10", "HDR10+", "Dolby Vision", "Uncertain", NULL
	};
	static const gchar *const probe_shown[] = {
		"Read", "Could not be read", "Reading timed out", "Not read yet", NULL
	};
	static const gchar *const state_labels[] = { "Present", "Not found by the last scan (suspected)", "Missing", NULL };
	g_autoptr(GPtrArray) root_values = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) root_labels = g_ptr_array_new();
	g_autoptr(GPtrArray) sort_values = g_ptr_array_new();
	g_autoptr(GPtrArray) sort_labels = g_ptr_array_new();
	g_autofree gchar *root_current = filter->root_id > 0 ?
		g_strdup_printf("%" G_GINT64_FORMAT, filter->root_id) : NULL;
	g_autofree gchar *min = number_text(filter->min_mib);
	g_autofree gchar *max = number_text(filter->max_mib);
	HtmxForm *form = htmx_form_new();
	HtmxDiv *buttons = htmx_div_new();
	HtmxButton *apply = htmx_button_new_submit();
	const gchar *pinned = filter->pinned == 1 ? "yes" : filter->pinned == 0 ? "no" : "";
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/library/files");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "filters");

	g_ptr_array_add(root_values, g_strdup(""));
	g_ptr_array_add(root_labels, (gpointer)"All library folders");
	for (i = 0; roots != NULL && i < roots->len && i < 200; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);

		g_ptr_array_add(root_values, g_strdup_printf("%" G_GINT64_FORMAT, root->id));
		g_ptr_array_add(root_labels, root->name);
	}
	g_ptr_array_add(root_values, NULL);
	g_ptr_array_add(root_labels, NULL);
	BROWSER_ADD(form, select_field("f-root", "Library folder", "root", (const gchar *const *)root_values->pdata,
		(const gchar *const *)root_labels->pdata, root_current));

	BROWSER_ADD(form, choice_field("f-kind", "Type of file", "kind", axis_library_filter_kinds(), kind_shown,
		filter->kind));
	BROWSER_ADD(form, axis_web_storage_text_field("f-container", "Container", "container", filter->container,
		"matroska,webm", NULL));
	BROWSER_ADD(form, axis_web_storage_text_field("f-vcodec", "Video codec", "vcodec", filter->video_codec,
		"hevc", NULL));
	BROWSER_ADD(form, choice_field("f-res", "Resolution", "res", axis_library_filter_resolutions(),
		res_shown, filter->resolution));
	BROWSER_ADD(form, choice_field("f-hdr", "HDR format", "hdr", axis_library_filter_hdr_classes(), hdr_shown,
		filter->hdr));
	BROWSER_ADD(form, axis_web_storage_text_field("f-acodec", "Audio codec", "acodec", filter->audio_codec,
		"eac3", NULL));
	BROWSER_ADD(form, select_field("f-pinned", "Pinned", "pinned", pinned_values, pinned_labels, pinned));
	BROWSER_ADD(form, choice_field("f-probe", "File reading (probe)", "probe",
		axis_library_filter_probe_states(), probe_shown, filter->probe));
	BROWSER_ADD(form, select_field("f-state", "Scan state", "state", axis_library_filter_states(), state_labels,
		filter->state));
	BROWSER_ADD(form, axis_web_storage_text_field("f-min", "Smallest (MiB)", "min_mib", min, NULL, NULL));
	BROWSER_ADD(form, axis_web_storage_text_field("f-max", "Largest (MiB)", "max_mib", max, NULL, NULL));
	BROWSER_ADD(form, axis_web_storage_text_field("f-name", "Name contains", "name", filter->name, NULL,
		"At least 3 characters."));

	for (i = 0; i < AXIS_LIBRARY_SORT_COUNT; i++) {
		g_ptr_array_add(sort_values, (gpointer)axis_library_sort_to_string((AxisLibrarySort)i));
		g_ptr_array_add(sort_labels, (gpointer)axis_library_sort_label((AxisLibrarySort)i));
	}
	g_ptr_array_add(sort_values, NULL);
	g_ptr_array_add(sort_labels, NULL);
	BROWSER_ADD(form, select_field("f-sort", "Sort by", "sort", (const gchar *const *)sort_values->pdata,
		(const gchar *const *)sort_labels->pdata, axis_library_sort_to_string(filter->sort)));
	BROWSER_ADD(form, select_field("f-dir", "Order", "dir", direction_values, direction_labels,
		filter->descending ? "desc" : "asc"));

	/* The conditions the form has no field for (the Analysis pages' links
	 * set them) are kept when it is sent. */
	{
		g_autofree gchar *volume = filter->volume_id > 0 ?
			g_strdup_printf("%" G_GINT64_FORMAT, filter->volume_id) : NULL;
		g_autofree gchar *under = filter->dir_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, filter->dir_id) : NULL;
		g_autofree gchar *min_bpp = number_text(filter->min_bpp);
		g_autofree gchar *max_bpp = number_text(filter->max_bpp);
		g_autofree gchar *ids = NULL;
		g_autofree gchar *rev = filter->revision > 0 ? g_strdup_printf("%d", filter->revision) : NULL;
		const struct
		{
			const gchar *name;
			const gchar *value;
		} kept[] = {
			{ "volume", volume }, { "type", filter->type }, { "under", under }, { "min_bpp", min_bpp },
			{ "max_bpp", max_bpp }, { "leftover", filter->leftover },
			{ "changed", filter->changed == 1 ? "yes" : filter->changed == 0 ? "no" : NULL },
			{ "target", filter->target }, { "rev", rev }, { "outcome", filter->outcome },
			{ NULL, NULL }
		};
		guint k;

		if (filter->n_ids > 0) {
			GString *list = g_string_new(NULL);

			for (k = 0; k < filter->n_ids; k++)
				g_string_append_printf(list, "%s%" G_GINT64_FORMAT, k > 0 ? "," : "", filter->ids[k]);
			ids = g_string_free(list, FALSE);
		}
		for (k = 0; kept[k].name != NULL; k++) {
			if (kept[k].value != NULL) {
				HtmxInput *hidden = htmx_input_new(HTMX_INPUT_HIDDEN);

				htmx_input_set_name(hidden, kept[k].name);
				htmx_input_set_value(hidden, kept[k].value);
				BROWSER_ADD(form, hidden);
			}
		}
		if (ids != NULL) {
			HtmxInput *hidden = htmx_input_new(HTMX_INPUT_HIDDEN);

			htmx_input_set_name(hidden, "ids");
			htmx_input_set_value(hidden, ids);
			BROWSER_ADD(form, hidden);
		}
	}

	htmx_node_set_text_content(HTMX_NODE(apply), "Filter");
	BROWSER_ADD(buttons, apply);
	BROWSER_ADD(buttons, axis_web_browser_link("/library/files", "Clear the filters"));
	BROWSER_ADD(form, buttons);

	return HTMX_NODE(form);
}

/*
 * The conditions a link from the Analysis pages sets that the form has no
 * field for, in words, so that a list says why it is short.  %NULL when
 * there are none.  Only numbers and fixed names: nothing from a file.
 */
static HtmxNode *
extra_conditions_note(const AxisLibraryFilter *filter)
{
	g_autoptr(GString) text = g_string_new(NULL);
	g_autofree gchar *note = NULL;

	if (filter->volume_id > 0)
		g_string_append_printf(text, "%sthe disk with number %" G_GINT64_FORMAT, text->len > 0 ? "; " : "",
			filter->volume_id);
	if (filter->type != NULL)
		g_string_append_printf(text, "%slibrary folders that hold %s", text->len > 0 ? "; " : "", filter->type);
	if (filter->dir_id > 0)
		g_string_append_printf(text, "%sthe folder with number %" G_GINT64_FORMAT " and everything below it",
			text->len > 0 ? "; " : "", filter->dir_id);
	if (filter->n_ids > 0)
		g_string_append_printf(text, "%s%u chosen %s", text->len > 0 ? "; " : "", filter->n_ids,
			filter->n_ids == 1 ? "file" : "files");
	if (filter->min_bpp >= 0 || filter->max_bpp >= 0) {
		g_autofree gchar *low = filter->min_bpp >= 0 ?
			g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT, filter->min_bpp / 1000,
				filter->min_bpp % 1000) : NULL;
		g_autofree gchar *high = filter->max_bpp >= 0 ?
			g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT, filter->max_bpp / 1000,
				filter->max_bpp % 1000) : NULL;

		g_autofree gchar *below = high != NULL ? g_strconcat("below ", high, NULL) : NULL;

		g_string_append_printf(text, "%svideo at %s%s%s%s bits per pixel", text->len > 0 ? "; " : "",
			low != NULL ? "least " : "", low != NULL ? low : "", low != NULL && high != NULL ? ", " : "",
			below != NULL ? below : "");
	}
	if (filter->leftover != NULL)
		g_string_append_printf(text, "%sthe leftover rule %s", text->len > 0 ? "; " : "", filter->leftover);
	if (filter->changed >= 0)
		g_string_append_printf(text, "%s%s since Axis last read them", text->len > 0 ? "; " : "",
			filter->changed == 1 ? "changed" : "not changed");
	if (filter->target != NULL) {
		/* A built-in by its name, another profile by its slug (both fixed
		 * text: a profile's name is not looked up here). */
		const AxisEstimateTarget *target = axis_estimate_builtin_target(filter->target);
		g_autofree gchar *which = target != NULL ? g_strdup_printf("%s (%s)", target->name, target->family_name) :
			g_strdup(filter->target);
		g_autofree gchar *revision = filter->revision > 0 ? g_strdup_printf(", version %d", filter->revision) :
			g_strdup("");
		AxisEstimateReason reason;
		const gchar *outcome = filter->outcome == NULL ? "the files it is for" :
			strcmp(filter->outcome, "eligible") == 0 ? "would be converted" :
			strcmp(filter->outcome, "skipped") == 0 ? "would be skipped" :
			axis_estimate_reason_from_string(filter->outcome, &reason) ? axis_estimate_reason_label(reason) : "";

		g_string_append_printf(text, "%swhat Space savings decides with the conversion profile %s%s: %s, as the files are now",
			text->len > 0 ? "; " : "", which, revision, outcome);
	}
	if (text->len == 0)
		return NULL;

	note = g_strdup_printf("Also narrowed by: %s.", text->str);
	return axis_web_storage_text("help", note);
}

/* The place the "list changed" notice appears, or the notice itself. */
static HtmxNode *
notice_node(
	const AxisLibraryFilter *filter,
	gint64                   since,
	gboolean                 changed
){
	HtmxDiv *box = htmx_div_new();
	AxisLibraryFilter plain;
	g_autofree gchar *query = NULL;
	g_autofree gchar *reload = NULL;

	axis_library_filter_copy(&plain, filter);
	plain.after = 0;
	plain.since = 0;
	query = axis_library_filter_to_query(&plain, FALSE);
	reload = query[0] != '\0' ? g_strdup_printf("/library/files?%s", query) : g_strdup("/library/files");
	axis_library_filter_clear(&plain);

	htmx_element_set_id(HTMX_ELEMENT(box), "list-notice");
	if (changed) {
		HtmxP *line = htmx_p_new();

		htmx_element_add_class(HTMX_ELEMENT(box), "notice");
		htmx_element_set_attribute(HTMX_ELEMENT(box), "role", "status");
		htmx_node_set_text_content(HTMX_NODE(line), "A scan has completed since this list was made, so it "
			"may be out of date. ");
		BROWSER_ADD(line, axis_web_browser_link(reload, "Reload the list"));
		BROWSER_ADD(box, line);
	} else {
		g_autofree gchar *watch = query[0] != '\0' ?
			g_strdup_printf("/library/files/changed?%s&since=%" G_GINT64_FORMAT, query, since) :
			g_strdup_printf("/library/files/changed?since=%" G_GINT64_FORMAT, since);

		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", watch);
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger", "sse:" AXIS_WEB_BROWSER_EVENT_SCANNED);
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");
	}
	return HTMX_NODE(box);
}

static gchar *
count_text(const AxisFileList *list)
{
	g_autofree gchar *count = NULL;

	if (list->count_unknown)
		return g_strdup("The files could not be counted in time; the list below is still right.");
	count = g_strdup_printf("%" G_GUINT64_FORMAT, list->count);
	if (list->count_capped)
		return g_strdup_printf("More than %s files.", count);
	return g_strdup_printf("%s %s.", count, list->count == 1 ? "file" : "files");
}

static HtmxResponse *
list_done(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
){
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *query = NULL;
	HtmxHeading *title;
	HtmxNode *card;
	AxisLibraryFilter plain;

	if (work->error != NULL) {
		if (g_error_matches(work->error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
			return axis_web_error_respond(request, 503, "Too slow",
				"This list could not be built in time. Narrow it with another filter, or sort it another way.");
		/* The module on: the profile (or its revision) the query names is
		 * not there, a query that cannot be answered. */
		if (g_error_matches(work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND) &&
		    self->estimator != NULL && axis_estimator_is_enabled(self->estimator))
			return axis_web_error_respond(request, 400, "Bad request",
				"There is no such conversion profile, or no such version of it, to decide this list with.");
		if (g_error_matches(work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND))
			return axis_web_error_respond(request, 404, "Not available",
				"This list needs the Space savings module, which is switched off.");
		if (g_error_matches(work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED))
			return axis_web_error_respond(request, 409, "Not usable",
				"That version of the conversion profile cannot be used, so Axis decides nothing with it.");
		return axis_web_internal_error(request, work->error);
	}

	if (work->notice) {
		/* The scan that ended after the page was made, if any. */
		return fragment_response(notice_node(&work->filter, work->filter.since,
			work->latest > work->filter.since));
	}

	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "All files");
	BROWSER_ADD(content, title);
	BROWSER_ADD(content, axis_web_page_intro("Every file of every library folder in one list, to filter and "
		"sort by type, library folder, codec, resolution, size, age and more, to find the files a question is about."));

	card = axis_web_storage_card("Filter");
	BROWSER_ADD(card, filter_form(&work->filter, work->roots));
	BROWSER_ADD(content, card);

	BROWSER_ADD(content, notice_node(&work->filter, work->latest, FALSE));
	{
		HtmxNode *extra = extra_conditions_note(&work->filter);

		if (extra != NULL)
			BROWSER_ADD(content, extra);
	}

	count = count_text(work->list);
	BROWSER_ADD(content, axis_web_storage_text("help", count));
	if (work->list->cursor_gone)
		BROWSER_ADD(content, axis_web_browser_warning("The list changed since the page before: it starts "
			"again from the top."));
	if (work->filter.sort == AXIS_LIBRARY_SORT_DURATION || work->filter.sort == AXIS_LIBRARY_SORT_BITRATE)
		BROWSER_ADD(content, axis_web_storage_text("help", "Sorted by duration or bitrate, which only "
			"files Axis has read have: other files are not listed."));

	card = axis_web_storage_card("Matching files");
	if (work->list->files->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No file matches these filters. Change them, or clear "
			"the filters:"));
		BROWSER_ADD(card, axis_web_browser_link("/library/files", "Clear the filters"));
	} else {
		g_autofree gchar *next = NULL;
		g_autofree gchar *first = NULL;

		axis_library_filter_copy(&plain, &work->filter);
		plain.after = 0;
		plain.since = 0;
		query = axis_library_filter_to_query(&plain, FALSE);
		if (work->list->next > 0) {
			next = query[0] != '\0' ?
				g_strdup_printf("/library/files?%s&after=%" G_GINT64_FORMAT, query, work->list->next) :
				g_strdup_printf("/library/files?after=%" G_GINT64_FORMAT, work->list->next);
		}
		if (work->filter.after > 0)
			first = query[0] != '\0' ? g_strdup_printf("/library/files?%s", query) :
				g_strdup("/library/files");
		axis_library_filter_clear(&plain);

		BROWSER_ADD(card, axis_web_browser_file_table(work->list->files, TRUE));
		if (first != NULL || next != NULL)
			BROWSER_ADD(card, axis_web_browser_pager(first, next));
	}
	BROWSER_ADD(content, card);

	axis_web_page_add_topic(request, AXIS_WEB_BROWSER_TOPIC);
	return axis_web_page_respond(request, "All files", HTMX_NODE(content));
}

static HtmxResponse *
list_handle(
	HtmxRequest *request,
	gpointer     user_data,
	gboolean     notice
){
	ListWork *work = g_new0(ListWork, 1);
	GError *error = NULL;

	if (!axis_library_filter_parse(htmx_request_get_query(request), &work->filter, &error)) {
		HtmxResponse *response = axis_web_error_respond(request, 400, "The filter is not valid",
			error->message);

		g_error_free(error);
		list_work_free(work);
		return response;
	}
	work->notice = notice;
	return axis_web_browser_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return list_handle(request, user_data, FALSE);
}

static HtmxResponse *
handle_list_changed(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return list_handle(request, user_data, TRUE);
}

/* ── Routes ─────────────────────────────────────────────────────── */

void
axis_web_browser_register_pages(
	AxisWebBrowser *self,
	AxisWeb        *web
){
	axis_web_get(web, "/library", handle_overview, self);
	axis_web_get(web, "/library/list", handle_overview_list, self);
	axis_web_get(web, "/library/root/:id", handle_root, self);
	axis_web_get(web, "/library/folder/:id", handle_folder, self);
	axis_web_get(web, "/library/files", handle_list, self);
	axis_web_get(web, "/library/files/changed", handle_list_changed, self);
}
