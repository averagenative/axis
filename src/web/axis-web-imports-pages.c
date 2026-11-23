/*
 * axis-web-imports-pages.c - the import pages: Activity -> Imports, an
 * import's page with its decisions and actions, the manual import, and the
 * Naming page with its preview
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything that came from a download (file names, release names) and
 * from the metadata (titles) is set as text, a file's name through its
 * stored form (axis_web_browser_name_node()).  Every form posts with the
 * session's CSRF field and is drawn for administrators only; the routes that
 * take them are AXIS_WEB_ROUTE_ADMIN whatever a page offered.
 */

#include "web/axis-web-imports-private.h"

#include <string.h>

#include "core/axis-error.h"
#include "jobs/axis-job-live.h"
#include "library/axis-library-name.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : level == 2 ? htmx_h2_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	if (node != NULL)
		ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
cell_time(gint64 us)
{
	g_autofree gchar *text = axis_web_format_time(us, "");

	return cell_text(text);
}

static HtmxNode *
text_link(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	ADD(link, bdi);
	return HTMX_NODE(link);
}

static HtmxNode *
state_badge_for(
	const gchar *state,
	const gchar *reason
){
	/* Its files disappeared before they were imported: final, said so. */
	if (g_strcmp0(state, "dismissed") == 0 && g_strcmp0(reason, "files_gone") == 0)
		return axis_web_browser_badge("Files gone", "badge-failed");
	/* A person deleted its download ("Delete this download"): not the same
	 * as a dismissal, which leaves the download where it is. */
	if (g_strcmp0(state, "dismissed") == 0 && g_strcmp0(reason, "download_deleted") == 0)
		return axis_web_browser_badge("Download deleted", "badge-note");
	if (g_strcmp0(state, "imported") == 0)
		return axis_web_browser_badge("Imported", "badge-succeeded");
	if (g_strcmp0(state, "blocked") == 0)
		return axis_web_browser_badge("Blocked", "badge-failed");
	/* Some of its files were imported and some were not: ended, and for a
	 * person to finish or dismiss.  (It read "Waiting", as if Axis would go
	 * on by itself.) */
	if (g_strcmp0(state, "partial") == 0)
		return axis_web_browser_badge("Partly imported", "badge-warn");
	if (g_strcmp0(state, "running") == 0)
		return axis_web_browser_badge("Importing", "badge-running");
	if (g_strcmp0(state, "dismissed") == 0)
		return axis_web_browser_badge("Dismissed", "badge-note");
	return axis_web_browser_badge("Waiting", "badge-warn");
}

#define state_badge(info) state_badge_for((info)->state, (info)->reason)

static const gchar *
mode_label(const gchar *mode)
{
	if (g_strcmp0(mode, "move") == 0)
		return "Moved";
	if (g_strcmp0(mode, "move_copy") == 0)
		return "Moved (copied, verified)";
	if (g_strcmp0(mode, "hardlink") == 0)
		return "Hardlinked";
	if (g_strcmp0(mode, "copy") == 0)
		return "Copied, verified";
	return "";
}

/* "Hearth (1995) {tmdb-6001}/Hearth (1995) ....mkv" in the root, as text. */
static gchar *
destination_text(const AxisImportInfo *info)
{
	g_autofree gchar *dir = info->dest_dir != NULL ? axis_library_name_display(info->dest_dir) : NULL;
	g_autofree gchar *file = info->dest_file != NULL ? axis_library_name_display(info->dest_file) : NULL;

	if (dir == NULL)
		return NULL;
	return g_strdup_printf("%s: %s/%s", info->root_name != NULL ? info->root_name : "its library folder", dir,
		file != NULL ? file : "");
}

static gchar *
minutes_text(gint64 ms)
{
	gint64 minutes = (ms + 30000) / 60000;

	if (ms <= 0)
		return g_strdup("");
	if (ms < 60000)
		return g_strdup_printf("%" G_GINT64_FORMAT " s", (ms + 500) / 1000);
	if (minutes >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", minutes / 60, minutes % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " min", minutes);
}

static HtmxNode *
read_only_notice(AxisWebImports *self)
{
	if (!axis_storage_is_read_only_mode(axis_import_get_storage(self->import)))
		return NULL;
	return axis_web_storage_text("notice notice-warning", "Read-only storage is on (Settings, Storage; "
		"storage.read_only), so Axis imports nothing and writes nothing to any disk. Completed downloads wait here "
		"under Waiting imports. Turn it off to let imports into the library.");
}

/* ── Activity -> Imports ────────────────────────────────────────── */

typedef struct
{
	GPtrArray *waiting;
	GPtrArray *blocked;
	GPtrArray *recent;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_clear_pointer(&work->waiting, g_ptr_array_unref);
	g_clear_pointer(&work->blocked, g_ptr_array_unref);
	g_clear_pointer(&work->recent, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebImports *self,
	gpointer        data
){
	ListWork *work = data;

	if (axis_import_list(self->import, AXIS_IMPORT_LIST_BLOCKED, 100, &work->blocked, &work->error) &&
	    axis_import_list(self->import, AXIS_IMPORT_LIST_WAITING, 100, &work->waiting, &work->error))
		axis_import_list(self->import, AXIS_IMPORT_LIST_RECENT, 50, &work->recent, &work->error);
}

static const AxisWebColumn open_columns[] = {
	{ "Import", AXIS_WEB_COL_NAME },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Why", AXIS_WEB_COL_WIDE },
	{ "Since", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static const AxisWebColumn recent_columns[] = {
	{ "Import", AXIS_WEB_COL_NAME },
	{ "State", AXIS_WEB_COL_STATE },
	{ "How", AXIS_WEB_COL_TEXT },
	{ "Where", AXIS_WEB_COL_WIDE },
	{ "When", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static HtmxNode *
name_cell(const AxisImportInfo *info)
{
	g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT, info->id);
	HtmxTd *cell = htmx_td_new();

	ADD(cell, text_link(href, info->title_label));
	if (info->release_name != NULL) {
		HtmxSpan *release = htmx_span_new();
		HtmxBdi *bdi = htmx_bdi_new();

		htmx_element_add_class(HTMX_ELEMENT(release), "range-detail");
		ADD(bdi, axis_web_breakable(info->release_name));
		ADD(release, bdi);
		ADD(cell, release);
	}
	return HTMX_NODE(cell);
}

static HtmxNode *
open_table(GPtrArray *imports)
{
	HtmxNode *table = axis_web_table_new("imports-open", open_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < imports->len; i++) {
		const AxisImportInfo *info = g_ptr_array_index(imports, i);
		HtmxTr *row = htmx_tr_new();

		ADD(row, name_cell(info));
		ADD(row, cell_node(state_badge(info)));
		ADD(row, cell_text(info->reason_text != NULL ? info->reason_text : g_strcmp0(info->state, "running") == 0 ?
			"Being placed now." : "Queued."));
		ADD(row, cell_time(info->updated_us));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
recent_table(GPtrArray *imports)
{
	HtmxNode *table = axis_web_table_new("imports-recent", recent_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < imports->len; i++) {
		const AxisImportInfo *info = g_ptr_array_index(imports, i);
		g_autofree gchar *where = destination_text(info);
		g_autofree gchar *how = NULL;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *place = htmx_td_new();

		ADD(row, name_cell(info));
		ADD(row, cell_node(state_badge(info)));
		how = g_strdup_printf("%s%s", mode_label(info->mode), info->upgrade ? ", upgrade" : "");
		ADD(row, cell_text(how));
		if (g_strcmp0(info->state, "imported") == 0 && where != NULL) {
			HtmxBdi *bdi = htmx_bdi_new();

			ADD(bdi, axis_web_breakable(where));
			ADD(place, bdi);
		} else if (info->reason_text != NULL) {
			ADD(place, axis_web_breakable(info->reason_text));
		}
		ADD(row, place);
		ADD(row, cell_time(info->finished_us));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxResponse *
list_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxNode *notice;
	HtmxNode *card;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	ADD(content, heading(1, "Imports"));
	ADD(content, axis_web_storage_text(NULL, "A completed download is taken into the library for the movie or series "
		"it was grabbed for, under a name Plex and Jellyfin both read. Its files are moved there: renamed when the "
		"download is on the library's disk, else copied, verified, and the download's files deleted afterwards (a "
		"torrent's only once its download client has let go of it). They are hardlinked instead when torrents are "
		"left seeding or when \"Files taken from a download\" (Settings, Imports) is set to hardlink. An import that cannot go on by itself waits, saying why; one that needs a "
		"person is blocked, with its reasons and what resolves them."));
	notice = read_only_notice(self);
	if (notice != NULL)
		ADD(content, notice);
	if (axis_web_request_is_admin(request)) {
		HtmxDiv *links = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(links), "film-actions");
		ADD(links, axis_web_browser_link("/imports/manual", "Import a movie by hand"));
		ADD(links, axis_web_browser_link("/imports/naming", "File naming"));
		ADD(content, links);
	}

	card = axis_web_storage_card("Blocked imports");
	if (work->blocked->len == 0)
		ADD(card, axis_web_storage_text("help", "Nothing is blocked."));
	else
		ADD(card, open_table(work->blocked));
	ADD(content, card);

	card = axis_web_storage_card("Waiting imports");
	if (work->waiting->len == 0)
		ADD(card, axis_web_storage_text("help", "Nothing waits to be imported."));
	else
		ADD(card, open_table(work->waiting));
	ADD(content, card);

	card = axis_web_storage_card("Recent imports");
	if (work->recent->len == 0)
		ADD(card, axis_web_storage_text("help", "Nothing has been imported yet."));
	else
		ADD(card, recent_table(work->recent));
	ADD(content, card);
	return axis_web_page_respond(request, "Imports", HTMX_NODE(content));
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_imports_dispatch(user_data, request, list_run, list_done, g_new0(ListWork, 1), list_work_free);
}

/* ── One import ─────────────────────────────────────────────────── */

typedef struct
{
	gint64          id;
	gchar          *done;
	AxisImportInfo *info;
	GPtrArray      *choices;
	GPtrArray      *live;	/* (nullable) AxisJobLive: its job in hand */
	gint64          now_us;
	GError         *error;
} ImportWork;

static void
import_work_free(gpointer data)
{
	ImportWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->live, g_ptr_array_unref);
	axis_import_info_free(work->info);
	g_clear_pointer(&work->choices, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
import_run(
	AxisWebImports *self,
	gpointer        data
){
	ImportWork *work = data;
	g_autofree gchar *key = g_strdup_printf(AXIS_IMPORT_KEY_PREFIX "%" G_GINT64_FORMAT, work->id);
	const gchar *keys[2];
	g_autoptr(GError) local = NULL;

	keys[0] = key;
	keys[1] = NULL;
	if (axis_import_get(self->import, work->id, &work->info, &work->error) &&
	    (g_strcmp0(work->info->state, "blocked") == 0 || g_strcmp0(work->info->state, "waiting") == 0))
		axis_import_list_choices(self->import, work->info->module, &work->choices, &work->error);
	/* Its job in hand: where it stands, for its line. */
	work->now_us = g_get_real_time();
	if (work->error == NULL && !axis_job_live_read_pool(axis_import_get_pool(self->import), keys, &work->live, &local))
		g_debug("imports: an import's job could not be read: %s", local->message);
}

/* "the runtime check, the name check" for the codes an override was given for. */
static gchar *
override_codes_text(gchar **codes)
{
	static const struct
	{
		const gchar *code;
		const gchar *name;
	} names[] = {
		{ "profile", "the quality profile check" },
		{ "not_upgrade", "the upgrade check" },
		{ "kept_by_hand", "the file being kept by hand" },
		{ "quality", "the quality check" },
		{ "wrong_name", "the name check" },
		{ "probe_tool", "the file reading check" },
		{ "unreadable", "the file reading check" },
		{ "runtime", "the runtime check" },
		{ NULL, NULL }
	};
	g_autoptr(GPtrArray) parts = g_ptr_array_new();
	guint i;
	guint k;

	for (i = 0; codes != NULL && codes[i] != NULL; i++) {
		const gchar *shown = codes[i];

		for (k = 0; names[k].code != NULL; k++) {
			if (strcmp(codes[i], names[k].code) == 0)
				shown = names[k].name;
		}
		g_ptr_array_add(parts, (gpointer)shown);
	}
	g_ptr_array_add(parts, NULL);
	return g_strjoinv(", ", (gchar **)parts->pdata);
}

/* What its job is doing now, or a hidden place for the line. */
static HtmxNode *
import_line(const ImportWork *work)
{
	g_autofree gchar *key = g_strdup_printf(AXIS_IMPORT_KEY_PREFIX "%" G_GINT64_FORMAT, work->id);
	const AxisJobLive *job = axis_job_live_find(work->live, key);
	g_autofree gchar *state = NULL;
	g_autofree gchar *text = NULL;

	if (job == NULL)
		return axis_web_activity_line("activity-import", AXIS_WEB_ACTIVITY_RUNNING, NULL);
	state = axis_web_activity_state(job, work->now_us);
	text = g_strdup_printf("Its import is %s.", state);
	return axis_web_activity_line("activity-import", job->running ? AXIS_WEB_ACTIVITY_RUNNING :
		AXIS_WEB_ACTIVITY_WAITING, text);
}

static const gchar *
import_done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "import_anyway", "Its objections were overridden: the import is queued again." },
		{ "choose_file", "The file was chosen: the import is queued again." },
		{ "assign", "This download was assigned to another title, whose import is opened." },
		{ "dismiss", "The import was dismissed. The download was left where it is." },
		{ "retry", "The import is queued again." },
		{ "assign_files", "The files were assigned by hand: the import is queued again, and takes them as you said." },
		{ "already_queued", "Nothing new was asked for: the import is waiting for its turn already, and Axis looks at "
		  "it again as soon as its turn comes." },
		{ "manual", "The import is queued." },
		/* "Delete this download": what became of it is in "About this
		 * import", under "At the download client" and the line after it. */
		{ "download_deleted", "The download was deleted, and the import is closed. What was deleted is said below." },
		{ "download_gone", "The import is closed. Nothing was left of the download to delete; what Axis found is said "
		  "below." },
		{ "download_left", "The import is closed, but the download's files were left where they are: a rule did not "
		  "let them be deleted. Which rule, and where the files are, is said below." },
		{ "download_waiting", "The import is closed, but the download is not deleted yet: the client did not answer, "
		  "or the disk could not be written. Axis looks again every hour and finishes it then; what is still to be done "
		  "is said below." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].text;
	}
	return NULL;
}

static const gchar *
role_label(const AxisImportFileInfo *file)
{
	if (!file->accepted)
		return file->role != NULL && g_strcmp0(file->role, "film") == 0 ? "The movie, not imported yet" : "Not imported";
	if (g_strcmp0(file->role, "film") == 0)
		return "The movie";
	if (g_strcmp0(file->role, "subtitle") == 0)
		return "Subtitle";
	if (g_strcmp0(file->role, "extra") == 0)
		return "Extra";
	return "Imported";
}

static const AxisWebColumn file_columns[] = {
	{ "File", AXIS_WEB_COL_NAME },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Runtime", AXIS_WEB_COL_NUMBER },
	{ "Decision", AXIS_WEB_COL_STATE },
	{ NULL, 0 }
};

static HtmxNode *
files_card(const AxisImportInfo *info)
{
	HtmxNode *card = axis_web_storage_card("Files in the download");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (info->files == NULL || info->files->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing has been looked at yet: the import has not run, or it waits "
			"before it looks (storage read-only, a disk away)."));
		return card;
	}
	table = axis_web_table_new("import-files", file_columns);
	body = htmx_tbody_new();
	for (i = 0; i < info->files->len; i++) {
		const AxisImportFileInfo *file = g_ptr_array_index(info->files, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		g_autofree gchar *size = file->size >= 0 && g_strcmp0(file->kind, "special") != 0 ?
			axis_size_format((guint64)file->size) : g_strdup("");
		g_autofree gchar *runs = minutes_text(file->duration_ms);
		const gchar *css = file->accepted ? "badge-succeeded" : g_strcmp0(file->role, "film") == 0 ? "badge-failed" :
			"badge-note";
		guint k;

		if (g_strcmp0(file->role, "film") == 0)
			htmx_element_add_class(HTMX_ELEMENT(row), "release-accepted");
		if (file->path[0] != '\0')
			ADD(name, axis_web_browser_name_node(file->path));
		else
			ADD(name, axis_web_browser_name_node(info->source_path));
		if (file->probe_text != NULL || file->width > 0 || (file->reasons != NULL && file->reasons->len > 0) ||
		    file->dest_name != NULL || file->note != NULL) {
			HtmxUl *list = htmx_ul_new();

			htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
			for (k = 0; file->reasons != NULL && k < file->reasons->len; k++) {
				const AxisImportReason *reason = g_ptr_array_index(file->reasons, k);
				HtmxLi *item = htmx_li_new();

				if (reason->overridable)
					htmx_element_add_class(HTMX_ELEMENT(item), "reason-note");
				htmx_node_set_text_content(HTMX_NODE(item), reason->text);
				ADD(list, item);
			}
			if (file->width > 0 && file->probe_text != NULL) {
				HtmxLi *item = htmx_li_new();
				g_autofree gchar *text = g_strdup_printf("Read from the file: %s.", file->probe_text);

				htmx_node_set_text_content(HTMX_NODE(item), text);
				ADD(list, item);
			} else if (file->probe_text != NULL && g_strcmp0(file->kind, "video") == 0) {
				HtmxLi *item = htmx_li_new();
				g_autofree gchar *text = g_strdup_printf("Reading the file: %s", file->probe_text);

				htmx_node_set_text_content(HTMX_NODE(item), text);
				ADD(list, item);
			}
			if (file->dest_name != NULL && g_strcmp0(file->step, "published") == 0) {
				HtmxLi *item = htmx_li_new();
				g_autofree gchar *shown = axis_library_name_display(file->dest_name);
				g_autofree gchar *text = g_strdup_printf("Placed as %s (%s%s%s).", shown, mode_label(file->mode),
					file->checksum != NULL ? ", SHA-256 " : "", file->checksum != NULL ? file->checksum : "");

				htmx_element_add_class(HTMX_ELEMENT(item), "reason-note");
				htmx_node_set_text_content(HTMX_NODE(item), text);
				ADD(list, item);
			}
			if (file->note != NULL) {
				HtmxLi *item = htmx_li_new();

				htmx_node_set_text_content(HTMX_NODE(item), file->note);
				ADD(list, item);
			}
			ADD(name, list);
		}
		ADD(row, name);
		ADD(row, cell_text(size));
		ADD(row, cell_text(runs));
		ADD(row, cell_node(axis_web_browser_badge(role_label(file), css)));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

/* What blocks it, and whether "import anyway" may override all of it. */
static gboolean
overridable(const AxisImportInfo *info)
{
	gboolean any = FALSE;
	guint i;
	guint k;

	for (i = 0; info->files != NULL && i < info->files->len; i++) {
		const AxisImportFileInfo *file = g_ptr_array_index(info->files, i);

		if (g_strcmp0(file->role, "film") != 0 || file->accepted)
			continue;
		for (k = 0; file->reasons != NULL && k < file->reasons->len; k++) {
			const AxisImportReason *reason = g_ptr_array_index(file->reasons, k);

			if (reason->overridable)
				any = TRUE;
		}
	}
	/* The reasons with no file (several films, no video) are never. */
	return any && g_strcmp0(info->reason, "several") != 0 && g_strcmp0(info->reason, "no_video") != 0 &&
		g_strcmp0(info->reason, "archive") != 0 && g_strcmp0(info->reason, "multi_part") != 0 &&
		g_strcmp0(info->reason, "hardlink_only") != 0 && g_strcmp0(info->reason, "name_taken") != 0 &&
		g_strcmp0(info->reason, "place_failed") != 0 && g_strcmp0(info->reason, "inside_root") != 0;
}

static HtmxNode *
act_form(
	HtmxRequest *request,
	gint64       id,
	const gchar *action,
	const gchar *label,
	const gchar *css
){
	g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", id);
	HtmxNode *form = axis_web_auth_post_form(request, href);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("action", action));
	ADD(form, axis_web_auth_button(label, css));
	return form;
}

/* The way to the confirmation of "Delete this download" (a link: the
 * confirmation writes nothing, and the deletion is its post). */
static HtmxNode *
delete_download_link(const AxisImportInfo *info)
{
	g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/delete-download", info->id);
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_element_add_class(HTMX_ELEMENT(link), "button");
	htmx_element_add_class(HTMX_ELEMENT(link), "button-danger");
	htmx_node_set_text_content(HTMX_NODE(link), "Delete this download");
	return HTMX_NODE(link);
}

static HtmxNode *
actions_card(
	HtmxRequest          *request,
	const AxisImportInfo *info,
	GPtrArray            *choices,
	AxisWebNotice        *placed
){
	HtmxNode *card = axis_web_storage_card("Ways to resolve this import");
	g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", info->id);
	gboolean blocked = g_strcmp0(info->state, "blocked") == 0;
	guint videos = 0;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "import-actions");
	axis_web_notice_put(placed, card, "import-actions");
	htmx_element_add_class(HTMX_ELEMENT(card), "import-actions");
	if (blocked && overridable(info)) {
		HtmxNode *form = axis_web_auth_post_form(request, href);

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		ADD(card, axis_web_storage_text("help", "Import anyway: the name, the runtime or the quality check said no (or "
			"the file could not be read). If you know it is the right movie (or the right episodes), import it: what "
			"you override is recorded with your name. A disk that is away or read-only, a copy that does not verify, "
			"or a path outside a known disk is never overridden."));
		ADD(form, axis_web_auth_hidden("action", "import_anyway"));
		ADD(form, axis_web_storage_checkbox("import-anyway-confirm", g_strcmp0(info->module, "tv") == 0 ?
			"I checked: these are its episodes" : "I checked: it is this movie", "confirm", TRUE));
		ADD(form, axis_web_auth_button("Import anyway", "button-primary"));
		ADD(card, form);
	}
	for (i = 0; info->files != NULL && i < info->files->len; i++) {
		const AxisImportFileInfo *file = g_ptr_array_index(info->files, i);

		if (g_strcmp0(file->kind, "video") == 0)
			videos++;
	}
	if (blocked && videos > 0) {
		HtmxNode *form = axis_web_auth_post_form(request, href);
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("import-choose");
		HtmxSelect *select = htmx_select_new_with_name("position");

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_element_add_class(HTMX_ELEMENT(field), "field-wide");
		htmx_node_set_text_content(HTMX_NODE(label), "The file to import");
		htmx_element_set_id(HTMX_ELEMENT(select), "import-choose");
		for (i = 0; i < info->files->len; i++) {
			const AxisImportFileInfo *file = g_ptr_array_index(info->files, i);
			g_autofree gchar *value = NULL;
			g_autofree gchar *shown = NULL;
			HtmxOption *option;

			if (g_strcmp0(file->kind, "video") != 0)
				continue;
			value = g_strdup_printf("%d", file->position);
			shown = axis_library_name_display(file->path[0] != '\0' ? file->path : info->source_path);
			option = htmx_option_new_with_value(value, shown);
			ADD(select, option);
		}
		ADD(field, label);
		ADD(field, select);
		ADD(form, axis_web_auth_hidden("action", "choose_file"));
		ADD(form, field);
		ADD(form, axis_web_auth_button("Import this file", "button-primary"));
		ADD(card, form);
	}
	if (choices != NULL && choices->len > 1) {
		HtmxNode *form = axis_web_auth_post_form(request, href);
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("import-assign");
		HtmxSelect *select = htmx_select_new_with_name("owner");

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_element_add_class(HTMX_ELEMENT(field), "field-wide");
		htmx_node_set_text_content(HTMX_NODE(label), g_strcmp0(info->module, "tv") == 0 ?
			"It is another series" : "It is another movie");
		htmx_element_set_id(HTMX_ELEMENT(select), "import-assign");
		for (i = 0; i < choices->len && i < 2000; i++) {
			const AxisImportChoice *choice = g_ptr_array_index(choices, i);
			g_autofree gchar *value = NULL;

			if (choice->owner_id == info->owner_id)
				continue;
			value = g_strdup_printf("%" G_GINT64_FORMAT, choice->owner_id);
			ADD(select, htmx_option_new_with_value(value, choice->label));
		}
		ADD(field, label);
		ADD(field, select);
		ADD(form, axis_web_auth_hidden("action", "assign"));
		ADD(form, field);
		ADD(form, axis_web_auth_button(g_strcmp0(info->module, "tv") == 0 ? "Import it for that series" :
			"Import it for that movie", NULL));
		ADD(card, form);
	}
	{
		HtmxDiv *row = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(row), "film-actions");
		ADD(row, act_form(request, info->id, "retry", "Retry this import", NULL));
		ADD(row, act_form(request, info->id, "dismiss", "Dismiss this import (the download stays where it is)",
			"button-danger"));
		/* Only for what a download of Axis's brought: a folder a person
		 * named is theirs, and the Leftovers page is where it can go. */
		if (info->download_id > 0)
			ADD(row, delete_download_link(info));
		ADD(card, row);
	}
	return card;
}

static HtmxNode *
recycled_card(
	AxisWebImports       *self,
	const AxisImportInfo *info
){
	static const AxisWebColumn columns[] = {
		{ "Replaced file", AXIS_WEB_COL_NAME },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Removed after", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Replaced files in the recycle folder");
	HtmxNode *table = axis_web_table_new("import-recycled", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	ADD(card, axis_web_storage_text("help", "The files this upgrade replaced, kept in .axis-recycle at the top of "
		"their disk for the time set in \"Keep replaced files for\" (Settings, Imports; import.recycle_retention), so "
		"that you can put one back by hand."));
	for (i = 0; i < info->recycled->len; i++) {
		const AxisImportRecycledInfo *item = g_ptr_array_index(info->recycled, i);
		g_autofree gchar *size = axis_size_format((guint64)MAX(item->size, 0));
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();

		ADD(name, axis_web_browser_name_node(item->original_path));
		if (item->recycle_path != NULL) {
			HtmxSpan *where = htmx_span_new();
			g_autofree gchar *shown = axis_library_name_display(item->recycle_path);
			g_autofree gchar *text = g_strdup_printf("Now %s", shown);

			htmx_element_add_class(HTMX_ELEMENT(where), "range-detail");
			htmx_node_set_text_content(HTMX_NODE(where), text);
			ADD(name, where);
		}
		ADD(row, name);
		ADD(row, cell_text(size));
		ADD(row, cell_node(axis_web_browser_badge(g_strcmp0(item->state, "kept") == 0 ? "Kept" :
			g_strcmp0(item->state, "removed") == 0 ? "Removed" : g_strcmp0(item->state, "moving") == 0 ? "Moving" :
			g_strcmp0(item->state, "gone") == 0 ? "Gone" : "Left alone", g_strcmp0(item->state, "kept") == 0 ?
			"badge-note" : "badge-warn")));
		if (g_strcmp0(item->state, "kept") == 0) {
			/* Due and still here: say what holds the deletion back. */
			g_autofree gchar *wait = axis_web_storage_removal_wait_note(axis_import_get_storage(self->import),
				info->dest_volume_id, item->remove_after_us);

			if (wait != NULL) {
				g_autofree gchar *when = axis_web_format_time(item->remove_after_us, "");
				HtmxTd *due = htmx_td_new();
				HtmxSpan *why = htmx_span_new();

				htmx_node_set_text_content(HTMX_NODE(due), when);
				htmx_element_add_class(HTMX_ELEMENT(why), "range-detail");
				htmx_node_set_text_content(HTMX_NODE(why), wait);
				ADD(due, why);
				ADD(row, due);
			} else {
				ADD(row, cell_time(item->remove_after_us));
			}
		} else {
			ADD(row, cell_time(item->remove_after_us));
		}
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
actions_history(const AxisImportInfo *info)
{
	HtmxNode *card = axis_web_storage_card("What people did");
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
	for (i = 0; i < info->actions->len; i++) {
		const AxisImportActionInfo *action = g_ptr_array_index(info->actions, i);
		g_autofree gchar *when = axis_web_format_time(action->created_us, "");
		g_autofree gchar *text = g_strdup_printf("%s, %s: %s", when, action->by_user, action->text != NULL ?
			action->text : action->action);
		HtmxLi *item = htmx_li_new();

		htmx_node_set_text_content(HTMX_NODE(item), text);
		ADD(list, item);
	}
	ADD(card, list);
	return card;
}

static HtmxResponse *
import_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	ImportWork *work = data;
	const AxisImportInfo *info = work->info;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *where = NULL;
	const gchar *notice = import_done_message(work->done);
	HtmxNode *facts;
	HtmxDiv *line;
	HtmxNode *card;
	AxisWebNotice placed;

	if (g_error_matches(work->error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	{
		HtmxHeading *h1 = htmx_h1_new();
		HtmxBdi *bdi = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(bdi), info->title_label);
		ADD(h1, bdi);
		ADD(content, h1);
	}
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	line = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
	htmx_element_add_class(HTMX_ELEMENT(line), "import-state");
	ADD(line, state_badge(info));
	if (info->reason_text != NULL)
		ADD(line, axis_web_storage_text("film-reason", info->reason_text));
	ADD(content, line);
	/* What its job is doing now (add-activity-feedback). */
	ADD(content, import_line(work));
	if (g_strcmp0(info->state, "waiting") == 0 || g_strcmp0(info->state, "blocked") == 0) {
		HtmxNode *ro = read_only_notice(self);

		if (ro != NULL)
			ADD(content, ro);
	}

	card = axis_web_storage_card("About this import");
	facts = axis_web_browser_facts_new();
	if (info->title_id > 0) {
		g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, info->title_id);

		axis_web_browser_fact_node(facts, "Imported for", text_link(href, info->title_label));
	}
	source = g_strdup_printf("%s: %s", info->source_volume != NULL ? info->source_volume : "a disk no longer known",
		info->source_path);
	axis_web_browser_fact_node(facts, "From", axis_web_breakable(source));
	if (info->release_name != NULL)
		axis_web_browser_fact_node(facts, "Release", axis_web_breakable(info->release_name));
	where = destination_text(info);
	if (where != NULL)
		axis_web_browser_fact(facts, g_strcmp0(info->state, "imported") == 0 ? "Placed in" : "Goes to", where);
	else if (info->root_name != NULL)
		axis_web_browser_fact(facts, "Goes to", info->root_name);
	if (info->mode_text != NULL)
		axis_web_browser_fact(facts, "How", info->mode_text);
	if (info->quality_text != NULL)
		axis_web_browser_fact(facts, "Quality", info->quality_text);
	if (info->upgrade)
		axis_web_browser_fact(facts, "Upgrade", info->n_recycled > 0 ? "Yes: the file it replaced is in the recycle "
			"folder." : "Yes");
	if (info->override_by != NULL) {
		g_autofree gchar *codes = override_codes_text(info->override_codes);
		g_autofree gchar *text = g_strdup_printf("%s, overriding: %s", info->override_by, codes);

		axis_web_browser_fact(facts, "Imported anyway by", text);
	}
	if (info->client_removal != NULL) {
		gboolean usenet = g_strcmp0(info->protocol, "usenet") == 0;

		axis_web_browser_fact(facts, "At the download client",
			info->client_removal_text != NULL ? info->client_removal_text :
			g_strcmp0(info->client_removal, "kept") == 0 ? (usenet ? "Left with its client." :
			"Left with its client (a torrent is left seeding).") :
			g_strcmp0(info->client_removal, "done") == 0 ? (usenet ? "The job was removed from its client." :
			"Removed from its client with its downloaded files.") :
			g_strcmp0(info->client_removal, "gone") == 0 ? "It was already gone from its client: something else removed "
			"it, and the client deleted none of its files." :
			usenet ? "To be removed from its client." : "To be removed from its client with its downloaded files.");
	}
	/* What SABnzbd keeps of a Usenet job, or what a torrent gone from its
	 * client left: what Axis did about it. */
	if (info->folder_removal != NULL) {
		gboolean usenet = g_strcmp0(info->protocol, "usenet") == 0;

		axis_web_browser_fact(facts, usenet ? "Folder at the download client" : "Files left at the download client",
			info->folder_removal_text != NULL ? info->folder_removal_text :
			g_strcmp0(info->folder_removal, "done") == 0 ? "Deleted, with everything the client left in it." :
			g_strcmp0(info->folder_removal, "gone") == 0 ? "It was already gone." :
			!usenet ? "To be looked at: the torrent is gone from its client, and what it left is deleted when every "
			"rule allows it." :
			g_strcmp0(info->client_removal, "done") == 0 || g_strcmp0(info->client_removal, "gone") == 0 ?
			"To be deleted, with everything the client left in it." :
			"Deleted once its client has removed the job (the client keeps a finished job's folder).");
	}
	if (info->requested_by != NULL)
		axis_web_browser_fact(facts, "Opened by", info->requested_by);
	{
		g_autofree gchar *created = axis_web_format_time(info->created_us, "");

		axis_web_browser_fact(facts, "Opened", created);
	}
	if (info->finished_us > 0) {
		g_autofree gchar *finished = axis_web_format_time(info->finished_us, "");

		axis_web_browser_fact(facts, "Finished", finished);
	}
	ADD(card, facts);
	ADD(content, card);

	/* Assign by hand: for an import a person must decide, or one that ended
	 * partly imported, once its video files were read. */
	if (axis_web_request_is_admin(request) &&
	    (g_strcmp0(info->state, "blocked") == 0 || g_strcmp0(info->state, "partial") == 0)) {
		gboolean videos = FALSE;
		guint i;

		for (i = 0; info->files != NULL && i < info->files->len; i++)
			videos = videos || g_strcmp0(((const AxisImportFileInfo *)g_ptr_array_index(info->files, i))->kind,
				"video") == 0;
		if (videos) {
			g_autofree gchar *href = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", info->id);

			card = axis_web_storage_card("Assign by hand");
			htmx_element_set_id(HTMX_ELEMENT(card), "import-assign-card");
			ADD(card, axis_web_storage_text("help", g_strcmp0(info->module, "tv") == 0 ?
				"When Axis cannot tell what the files are, or reads them wrongly, say it yourself: which episodes each "
				"video file holds. Axis imports those files as you said, without judging their names, and leaves the "
				"others alone. Nothing in the library is replaced without a tick for that file." :
				"When Axis cannot tell which file is the movie, or reads its name as another movie, say it yourself. "
				"Axis imports that file as this movie, without judging its name, and leaves the others alone. Nothing "
				"in the library is replaced without a tick for that file."));
			ADD(card, axis_web_browser_link(href, "Assign by hand"));
			ADD(content, card);
		}
	}
	if (axis_web_request_is_admin(request) &&
	    (g_strcmp0(info->state, "blocked") == 0 || g_strcmp0(info->state, "waiting") == 0))
		ADD(content, actions_card(request, info, work->choices, &placed));
	/* A partly imported one has no card of actions; its download can be
	 * deleted all the same (what it placed stays). */
	if (axis_web_request_is_admin(request) && g_strcmp0(info->state, "partial") == 0 && info->download_id > 0) {
		card = axis_web_storage_card("Delete the rest of this download");
		htmx_element_set_id(HTMX_ELEMENT(card), "import-delete-card");
		ADD(card, axis_web_storage_text("help", "When the files that were not imported are not wanted, delete the "
			"download, on the disk and at its client. The files this import placed in the library stay. A page shows "
			"what would go before anything is deleted."));
		ADD(card, delete_download_link(info));
		ADD(content, card);
	}
	ADD(content, files_card(info));
	if (info->recycled != NULL && info->recycled->len > 0)
		ADD(content, recycled_card(self, info));
	if (info->actions != NULL && info->actions->len > 0)
		ADD(content, actions_history(info));
	ADD(content, axis_web_browser_link("/imports", "All imports"));
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, info->title_label, HTMX_NODE(content));
}

static HtmxResponse *
handle_import(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ImportWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	work = g_new0(ImportWork, 1);
	work->id = id;
	work->done = g_strdup(htmx_request_get_query_param(request, "done"));
	return axis_web_imports_dispatch(user_data, request, import_run, import_done, work, import_work_free);
}

/* ── Acting ─────────────────────────────────────────────────────── */

typedef struct
{
	gint64            id;
	AxisImportAction  action;
	gint              position;
	gint64            owner_id;
	gchar            *user;
	gint64            opened;
	GError           *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
act_run(
	AxisWebImports *self,
	gpointer        data
){
	ActWork *work = data;

	axis_import_act(self->import, work->id, work->action, work->position, work->owner_id, work->user,
		&work->opened, &work->error);
}

static HtmxResponse *
act_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActWork *work = data;
	g_autofree gchar *location = NULL;

	/* Asked again while it waits for its turn: nothing new, said there. */
	if (g_error_matches(work->error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_IN_HAND)) {
		location = g_strdup_printf("/imports/%" G_GINT64_FORMAT, work->id);
		return axis_web_place_redirect(request, location, "already_queued", "import-actions");
	}
	if (work->error != NULL) {
		guint status;

		if (work->error->domain != AXIS_IMPORT_ERROR)
			return axis_web_internal_error(request, work->error);
		status = work->error->code == AXIS_IMPORT_ERROR_NOT_FOUND ? 404 :
			work->error->code == AXIS_IMPORT_ERROR_INVALID ? 400 : 409;
		return axis_web_error_respond(request, status, "Not done", work->error->message);
	}
	axis_web_storage_audit(request, "import %" G_GINT64_FORMAT " %s", work->id, axis_import_action_name(work->action));
	location = g_strdup_printf("/imports/%" G_GINT64_FORMAT, work->opened > 0 ? work->opened : work->id);
	return axis_web_place_redirect(request, location, work->opened > 0 ? "manual" :
		axis_import_action_name(work->action), "import-actions");
}

static HtmxResponse *
handle_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ActWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *action = htmx_request_get_form_value(request, "action");
	AxisImportAction which;
	guint64 value = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	if (!axis_import_action_from_name(action, &which))
		return axis_web_error_respond(request, 400, "Not done", "Choose what to do.");
	work = g_new0(ActWork, 1);
	work->id = id;
	work->action = which;
	work->position = -1;
	work->user = g_strdup(axis_web_request_get_username(request));
	if (which == AXIS_IMPORT_ACT_CHOOSE) {
		if (!axis_web_storage_form_u64(request, "position", &value) || value > 100000) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Choose a file.");
		}
		work->position = (gint)value;
	} else if (which == AXIS_IMPORT_ACT_ASSIGN) {
		if (!axis_web_storage_form_u64(request, "owner", &value) || value == 0 || value > (guint64)G_MAXINT64) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Choose the movie or series.");
		}
		work->owner_id = (gint64)value;
	} else if (which == AXIS_IMPORT_ACT_ANYWAY && !axis_web_storage_form_yes(request, "confirm")) {
		act_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Tick the box to confirm.");
	}
	return axis_web_imports_dispatch(user_data, request, act_run, act_done, work, act_work_free);
}

/* ── Manual import ──────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *sources;
	GError    *error;
} SourcesWork;

static void
sources_work_free(gpointer data)
{
	SourcesWork *work = data;

	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
sources_run(
	AxisWebImports *self,
	gpointer        data
){
	SourcesWork *work = data;

	axis_import_list_sources(self->import, &work->sources, &work->error);
}

static HtmxResponse *
sources_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	static const AxisWebColumn columns[] = {
		{ "Download or folder", AXIS_WEB_COL_NAME },
		{ "Where", AXIS_WEB_COL_WIDE },
		{ "Completed", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	SourcesWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxNode *card;
	HtmxNode *notice;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	ADD(content, heading(1, "Import a movie by hand"));
	ADD(content, axis_web_storage_text(NULL, "For a download Axis could not decide, or a folder you put in a "
		"downloads folder yourself: look at what Axis reads from it, choose the movie, and import. A download's "
		"files are moved (or hardlinked) as any import's are; the files of a folder you put there yourself are "
		"hardlinked or copied and stay where they are, unchanged."));
	notice = read_only_notice(self);
	if (notice != NULL)
		ADD(content, notice);
	card = axis_web_storage_card("Completed downloads and downloads folders");
	if (work->sources->len == 0) {
		HtmxNode *none = axis_web_storage_text("help", "No completed download waits, and no downloads folder has a "
			"folder or a video at its top. A downloads folder is a library folder of type Downloads.");

		ADD(card, none);
		ADD(card, axis_web_browser_link("/storage", "Add a downloads folder under Disks"));
	} else {
		HtmxNode *table = axis_web_table_new("import-sources", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->sources->len; i++) {
			const AxisImportSource *source = g_ptr_array_index(work->sources, i);
			g_autofree gchar *href = NULL;
			g_autofree gchar *escaped = NULL;
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();

			if (source->download_id > 0) {
				href = g_strdup_printf("/imports/manual/inspect?download=%" G_GINT64_FORMAT, source->download_id);
			} else {
				escaped = g_uri_escape_string(source->path, NULL, FALSE);
				href = g_strdup_printf("/imports/manual/inspect?root=%" G_GINT64_FORMAT "&path=%s", source->root_id,
					escaped);
			}
			ADD(name, text_link(href, source->label));
			ADD(row, name);
			ADD(row, cell_text(source->where));
			ADD(row, source->when_us > 0 ? cell_time(source->when_us) : cell_text(""));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link("/imports", "All imports"));
	return axis_web_page_respond(request, "Import a movie by hand", HTMX_NODE(content));
}

static HtmxResponse *
handle_manual(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_imports_dispatch(user_data, request, sources_run, sources_done, g_new0(SourcesWork, 1),
		sources_work_free);
}

typedef struct
{
	gint64     download_id;
	gint64     root_id;
	gchar     *path;
	GPtrArray *files;
	GPtrArray *choices;
	gchar     *problem;
	GError    *error;
} PeekWork;

static void
peek_work_free(gpointer data)
{
	PeekWork *work = data;

	g_free(work->path);
	g_clear_pointer(&work->files, g_ptr_array_unref);
	g_clear_pointer(&work->choices, g_ptr_array_unref);
	g_free(work->problem);
	g_clear_error(&work->error);
	g_free(work);
}

static void
peek_run(
	AxisWebImports *self,
	gpointer        data
){
	PeekWork *work = data;

	if (axis_import_peek(self->import, work->download_id, work->root_id, work->path, &work->files, &work->problem,
	        &work->error))
		axis_import_list_choices(self->import, "movies", &work->choices, &work->error);
}

/* A kind the peek stores ("link", "special"), as a person says it. */
static const gchar *
peek_kind_label(const gchar *kind)
{
	if (g_strcmp0(kind, "video") == 0)
		return "Video";
	if (g_strcmp0(kind, "subtitle") == 0)
		return "Subtitle";
	if (g_strcmp0(kind, "folder") == 0)
		return "Folder";
	if (g_strcmp0(kind, "link") == 0)
		return "Symbolic link";
	if (g_strcmp0(kind, "special") == 0)
		return "Not a regular file";
	if (g_strcmp0(kind, "archive") == 0)
		return "Archive";
	return "Other";
}

static HtmxResponse *
peek_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Kind", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Runtime", AXIS_WEB_COL_NUMBER },
		{ "Picture", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	PeekWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxNode *card;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	ADD(content, heading(1, "Import a movie by hand"));
	if (work->path != NULL && work->download_id <= 0) {
		g_autofree gchar *from = g_strdup_printf("From the downloads folder: %s", work->path);

		ADD(content, axis_web_storage_text(NULL, from));
	}
	if (work->problem != NULL) {
		ADD(content, axis_web_auth_notice(TRUE, work->problem));
		ADD(content, axis_web_browser_link("/imports/manual", "Choose another download or folder"));
		return axis_web_page_respond(request, "Import a movie by hand", HTMX_NODE(content));
	}
	card = axis_web_storage_card("Files in this download or folder");
	if (work->files->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing Axis would look at is in it."));
	} else {
		HtmxNode *table = axis_web_table_new("import-peek", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->files->len; i++) {
			const AxisImportPeek *peek = g_ptr_array_index(work->files, i);
			g_autofree gchar *size = g_strcmp0(peek->kind, "folder") != 0 && g_strcmp0(peek->kind, "link") != 0 ?
				axis_size_format((guint64)MAX(peek->size, 0)) : g_strdup("");
			g_autofree gchar *runs = minutes_text(peek->duration_ms);
			g_autofree gchar *picture = peek->width > 0 ? g_strdup_printf("%dx%d", peek->width, peek->height) :
				g_strdup("");
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();

			ADD(name, axis_web_browser_name_node(peek->path));
			if (peek->note != NULL && peek->width == 0) {
				HtmxSpan *note = htmx_span_new();

				htmx_element_add_class(HTMX_ELEMENT(note), "range-detail");
				htmx_node_set_text_content(HTMX_NODE(note), peek->note);
				ADD(name, note);
			}
			ADD(row, name);
			ADD(row, cell_text(peek_kind_label(peek->kind)));
			ADD(row, cell_text(size));
			ADD(row, cell_text(runs));
			ADD(row, cell_text(picture));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);

	card = axis_web_storage_card("Choose the movie and import");
	if (work->choices == NULL || work->choices->len == 0) {
		ADD(card, axis_web_storage_text("help", "No movie is managed yet: add the movie first, then come back here."));
		ADD(card, axis_web_browser_link("/search", "Search for a movie"));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, "/imports/manual");
		HtmxDiv *film_field = htmx_div_new();
		HtmxLabel *film_label = htmx_label_new_for("manual-film");
		HtmxSelect *films = htmx_select_new_with_name("owner");
		HtmxDiv *file_field = htmx_div_new();
		HtmxLabel *file_label = htmx_label_new_for("manual-file");
		HtmxSelect *files = htmx_select_new_with_name("position");

		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		htmx_element_add_class(HTMX_ELEMENT(film_field), "field");
		htmx_element_add_class(HTMX_ELEMENT(film_field), "field-wide");
		htmx_node_set_text_content(HTMX_NODE(film_label), "The movie");
		htmx_element_set_id(HTMX_ELEMENT(films), "manual-film");
		for (i = 0; i < work->choices->len && i < 5000; i++) {
			const AxisImportChoice *choice = g_ptr_array_index(work->choices, i);
			g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, choice->owner_id);

			ADD(films, htmx_option_new_with_value(value, choice->label));
		}
		ADD(film_field, film_label);
		ADD(film_field, films);
		htmx_element_add_class(HTMX_ELEMENT(file_field), "field");
		htmx_element_add_class(HTMX_ELEMENT(file_field), "field-wide");
		htmx_node_set_text_content(HTMX_NODE(file_label), "The movie's file");
		htmx_element_set_id(HTMX_ELEMENT(files), "manual-file");
		ADD(files, htmx_option_new_with_value("auto", "Let Axis decide"));
		for (i = 0; i < work->files->len; i++) {
			const AxisImportPeek *peek = g_ptr_array_index(work->files, i);
			g_autofree gchar *value = NULL;
			g_autofree gchar *shown = NULL;

			if (g_strcmp0(peek->kind, "video") != 0)
				continue;
			value = g_strdup_printf("%d", peek->position);
			shown = axis_library_name_display(peek->path);
			ADD(files, htmx_option_new_with_value(value, shown));
		}
		ADD(file_field, file_label);
		ADD(file_field, files);
		if (work->download_id > 0) {
			g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->download_id);

			ADD(form, axis_web_auth_hidden("download", id));
		} else {
			g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, work->root_id);

			ADD(form, axis_web_auth_hidden("root", root));
			ADD(form, axis_web_auth_hidden("path", work->path));
		}
		ADD(form, film_field);
		ADD(form, file_field);
		/* Opened to be assigned by hand: Axis reads the files and waits. */
		ADD(form, axis_web_storage_checkbox("manual-assign", "Assign by hand: Axis reads the files and imports nothing "
			"until I say which file is the movie (its name is then not judged)", "assign", FALSE));
		ADD(form, axis_web_auth_button("Import the file for this movie", "button-primary"));
		ADD(card, form);
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link("/imports/manual", "Choose another download or folder"));
	return axis_web_page_respond(request, "Import a movie by hand", HTMX_NODE(content));
}

static gint64
query_id(
	HtmxRequest *request,
	const gchar *name
){
	const gchar *text = htmx_request_get_query_param(request, name);
	guint64 value = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, G_MAXINT64, &value, NULL))
		return 0;
	return (gint64)value;
}

static HtmxResponse *
handle_inspect(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	PeekWork *work = g_new0(PeekWork, 1);

	work->download_id = query_id(request, "download");
	work->root_id = query_id(request, "root");
	work->path = g_strdup(htmx_request_get_query_param(request, "path"));
	if (work->download_id <= 0 && (work->root_id <= 0 || work->path == NULL)) {
		peek_work_free(work);
		return axis_web_error_respond(request, 400, "Not found", "Choose a download or a folder.");
	}
	return axis_web_imports_dispatch(user_data, request, peek_run, peek_done, work, peek_work_free);
}

typedef struct
{
	AxisImportManualSpec spec;
	gchar               *path;
	gchar               *user;
	gint64               id;
	GError              *error;
} ManualWork;

static void
manual_work_free(gpointer data)
{
	ManualWork *work = data;

	g_free(work->path);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
manual_run(
	AxisWebImports *self,
	gpointer        data
){
	ManualWork *work = data;

	work->spec.path = work->path;
	work->spec.user = work->user;
	axis_import_manual(self->import, &work->spec, &work->id, &work->error);
}

static HtmxResponse *
manual_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	ManualWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL) {
		if (work->error->domain != AXIS_IMPORT_ERROR)
			return axis_web_internal_error(request, work->error);
		return axis_web_error_respond(request, work->error->code == AXIS_IMPORT_ERROR_BUSY ? 409 : 400, "Not imported",
			work->error->message);
	}
	axis_web_storage_audit(request, "import %" G_GINT64_FORMAT " opened by hand", work->id);
	location = g_strdup_printf("/imports/%" G_GINT64_FORMAT "?done=manual", work->id);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_manual_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ManualWork *work = g_new0(ManualWork, 1);
	const gchar *position = htmx_request_get_form_value(request, "position");
	guint64 value = 0;

	work->spec.module = "movies";
	work->spec.position = -1;
	work->spec.assign = axis_web_storage_form_yes(request, "assign");
	work->user = g_strdup(axis_web_request_get_username(request));
	if (!axis_web_storage_form_u64(request, "owner", &value) || value == 0 || value > (guint64)G_MAXINT64) {
		manual_work_free(work);
		return axis_web_error_respond(request, 400, "Not imported", "Choose the movie.");
	}
	work->spec.owner_id = (gint64)value;
	if (axis_web_storage_form_u64(request, "download", &value) && value > 0 && value <= (guint64)G_MAXINT64) {
		work->spec.download_id = (gint64)value;
	} else if (axis_web_storage_form_u64(request, "root", &value) && value > 0 && value <= (guint64)G_MAXINT64) {
		work->spec.root_id = (gint64)value;
		work->path = g_strdup(htmx_request_get_form_value(request, "path"));
	} else {
		manual_work_free(work);
		return axis_web_error_respond(request, 400, "Not imported", "Choose a download or a folder.");
	}
	if (position != NULL && g_strcmp0(position, "auto") != 0) {
		if (!g_ascii_string_to_unsigned(position, 10, 0, 100000, &value, NULL)) {
			manual_work_free(work);
			return axis_web_error_respond(request, 400, "Not imported", "Choose the file.");
		}
		work->spec.position = (gint)value;
	}
	return axis_web_imports_dispatch(user_data, request, manual_run, manual_done, work, manual_work_free);
}

/* ── Naming ─────────────────────────────────────────────────────── */

/* The preview of three sample films, for what the form says now. */
static HtmxNode *
preview_block(
	gboolean     rename,
	const gchar *layout,
	const gchar *format
){
	static const AxisWebColumn columns[] = {
		{ "Movie", AXIS_WEB_COL_NAME },
		{ "Folder and file", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	g_autoptr(GPtrArray) previews = NULL;
	g_autofree gchar *problem = NULL;
	HtmxDiv *block = htmx_div_new();
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(block), "naming-preview");
	htmx_element_add_class(HTMX_ELEMENT(block), "naming-preview");
	htmx_element_set_attribute(HTMX_ELEMENT(block), "aria-live", "polite");
	if (!axis_import_preview(rename, layout, format, &previews, &problem)) {
		HtmxNode *error = axis_web_storage_text("field-error", problem != NULL ? problem : "The format is not valid.");

		htmx_element_set_attribute(HTMX_ELEMENT(error), "role", "alert");
		ADD(block, error);
		return HTMX_NODE(block);
	}
	{
		HtmxNode *table = axis_web_table_new("naming-preview", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < previews->len; i++) {
			const AxisImportPreview *preview = g_ptr_array_index(previews, i);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *film = htmx_td_new();
			HtmxTd *names = htmx_td_new();
			HtmxSpan *release = htmx_span_new();
			HtmxCode *folder = htmx_code_new();
			HtmxCode *file = htmx_code_new();
			HtmxDiv *folder_line = htmx_div_new();
			HtmxDiv *file_line = htmx_div_new();

			htmx_node_set_text_content(HTMX_NODE(film), preview->label);
			htmx_element_add_class(HTMX_ELEMENT(release), "range-detail");
			ADD(release, axis_web_breakable(preview->release));
			ADD(film, release);
			ADD(folder, axis_web_breakable(preview->folder));
			ADD(file, axis_web_breakable(preview->file));
			ADD(folder_line, folder);
			ADD(file_line, file);
			htmx_element_add_class(HTMX_ELEMENT(folder_line), "naming-folder");
			htmx_element_add_class(HTMX_ELEMENT(file_line), "naming-file");
			ADD(names, folder_line);
			ADD(names, file_line);
			ADD(row, film);
			ADD(row, names);
			ADD(body, row);
		}
		ADD(table, body);
		ADD(block, table);
	}
	return HTMX_NODE(block);
}

static const gchar *
setting_or(
	AxisWebImports *self,
	const gchar    *key,
	gchar         **owned,
	const gchar    *fallback
){
	if (self->settings == NULL || axis_settings_lookup(self->settings, key, NULL) == NULL)
		return fallback;
	*owned = axis_settings_format(self->settings, key);
	return *owned != NULL ? *owned : fallback;
}

static HtmxResponse *
naming_page(
	AxisWebImports *self,
	HtmxRequest    *request,
	guint           status,
	const gchar    *notice,
	gboolean        notice_error,
	const gchar    *rename_value,
	const gchar    *layout_value,
	const gchar    *format_value
){
	static const struct
	{
		const gchar *value;
		const gchar *label;
	} layouts[] = {
		{ "tmdb", "Title (Year) {tmdb-12345}: Plex and Jellyfin" },
		{ "jellyfin", "Title (Year) [tmdbid-12345]: Jellyfin only" },
		{ "plain", "Title (Year): no identifier" }
	};
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *rename_owned = NULL;
	g_autofree gchar *layout_owned = NULL;
	g_autofree gchar *format_owned = NULL;
	const gchar *rename = rename_value != NULL ? rename_value :
		setting_or(self, AXIS_IMPORT_SETTING_RENAME, &rename_owned, "true");
	const gchar *layout = layout_value != NULL ? layout_value :
		setting_or(self, AXIS_IMPORT_SETTING_LAYOUT, &layout_owned, "tmdb");
	const gchar *format = format_value != NULL ? format_value :
		setting_or(self, AXIS_IMPORT_SETTING_FORMAT, &format_owned, AXIS_IMPORT_DEFAULT_FORMAT);
	HtmxNode *card = axis_web_storage_card("Names of imported movies and episodes");
	HtmxNode *form = axis_web_auth_post_form(request, "/imports/naming");
	HtmxResponse *response;
	AxisWebNotice placed;
	guint i;

	ADD(content, heading(1, "File naming"));
	ADD(content, axis_web_page_intro("How Axis names the folders and files it imports into the library, in the "
		"form Plex and Jellyfin read, so that a download arrives where your media server finds it."));
	axis_web_notice_begin(&placed, request, notice_error, g_strdup(notice), HTMX_NODE(content));
	htmx_element_set_id(HTMX_ELEMENT(card), "naming-card");
	axis_web_notice_put(&placed, card, "naming-card");
	ADD(card, axis_web_storage_text("help", "Every movie goes in a folder of its own, named from its title, year and "
		"identifier, and its file begins with that same text, then what its release says. Both media servers read "
		"this. '/' and '\\' become '-', ':' becomes ' -', control characters are dropped, and no name begins with a "
		"dot or ends with a dot or a space; a name too long for its filesystem loses the bracketed part first and "
		"the title last. A change names new imports only: files already imported keep their names."));
	/* The fields in a narrow column, the preview as wide as the card. */
	htmx_element_add_class(HTMX_ELEMENT(form), "form-wide");
	htmx_element_add_class(HTMX_ELEMENT(form), "naming-form");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-get", "/imports/naming/preview");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-trigger", "input changed delay:400ms, change");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-target", "#naming-preview");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "hx-swap", "outerHTML");
	{
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("naming-rename");
		HtmxSelect *select = htmx_select_new_with_name(AXIS_IMPORT_SETTING_RENAME);
		HtmxOption *on = htmx_option_new_with_value("true", "On: named from the movie");
		HtmxOption *off = htmx_option_new_with_value("false", "Off: the release's own file name, in the movie's folder");

		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Rename imported files");
		htmx_element_set_id(HTMX_ELEMENT(select), "naming-rename");
		htmx_option_set_selected(on, g_strcmp0(rename, "false") != 0);
		htmx_option_set_selected(off, g_strcmp0(rename, "false") == 0);
		ADD(select, on);
		ADD(select, off);
		ADD(field, label);
		ADD(field, select);
		ADD(form, field);
	}
	{
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("naming-layout");
		HtmxSelect *select = htmx_select_new_with_name(AXIS_IMPORT_SETTING_LAYOUT);

		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Folder and file names");
		htmx_element_set_id(HTMX_ELEMENT(select), "naming-layout");
		for (i = 0; i < G_N_ELEMENTS(layouts); i++) {
			HtmxOption *option = htmx_option_new_with_value(layouts[i].value, layouts[i].label);

			htmx_option_set_selected(option, g_strcmp0(layout, layouts[i].value) == 0);
			ADD(select, option);
		}
		ADD(field, label);
		ADD(field, select);
		ADD(form, field);
	}
	ADD(form, axis_web_storage_text_field("naming-format", "What follows the title in a file's name",
		AXIS_IMPORT_SETTING_FORMAT, format, AXIS_IMPORT_DEFAULT_FORMAT, "Tokens: edition, quality, hdr, audio, codec, "
		"group and airdate (an episode's), each in braces with what should surround it: {[quality]} gives [WEB-DL 1080p], {-group} gives "
		"-GROUP; a token with nothing to say is left out with what surrounds it."));
	ADD(form, preview_block(g_strcmp0(rename, "false") != 0, layout, format));
	ADD(form, axis_web_auth_button("Save the naming", "button-primary"));
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_storage_text("help", "The other import settings (the hardlink policy, how long replaced "
		"files are kept, the mode of new folders, the free space to keep, subtitles and extras) are on the Settings "
		"page, under Imports."));
	ADD(content, axis_web_browser_link("/admin/settings#module-import", "Imports settings"));
	axis_web_notice_end(&placed);
	response = axis_web_page_respond(request, "File naming", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_naming(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *done = htmx_request_get_query_param(request, "done");

	return naming_page(user_data, request, 200, g_strcmp0(done, "saved") == 0 ? "The naming has been saved: new "
		"imports are named so." : g_strcmp0(done, "unchanged") == 0 ? "Nothing needed saving." : NULL, FALSE, NULL,
		NULL, NULL);
}

static HtmxResponse *
handle_preview(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *rename = htmx_request_get_query_param(request, AXIS_IMPORT_SETTING_RENAME);
	const gchar *layout = htmx_request_get_query_param(request, AXIS_IMPORT_SETTING_LAYOUT);
	const gchar *format = htmx_request_get_query_param(request, AXIS_IMPORT_SETTING_FORMAT);
	g_autoptr(HtmxNode) block = preview_block(g_strcmp0(rename, "false") != 0, layout != NULL ? layout : "tmdb",
		format != NULL ? format : "");
	g_autofree gchar *markup = NULL;
	HtmxResponse *response;

	axis_web_table_apply_roles(block);
	markup = htmx_element_render(HTMX_ELEMENT(block));
	response = htmx_response_new();
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return response;
}

typedef struct
{
	gchar     *rename;
	gchar     *layout;
	gchar     *format;
	GStrv      changed;
	GPtrArray *problems;
	GError    *error;
} SaveWork;

static void
save_work_free(gpointer data)
{
	SaveWork *work = data;

	g_free(work->rename);
	g_free(work->layout);
	g_free(work->format);
	g_strfreev(work->changed);
	g_clear_pointer(&work->problems, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
save_run(
	AxisWebImports *self,
	gpointer        data
){
	SaveWork *work = data;
	AxisSettingChange changes[3];

	changes[0].key = AXIS_IMPORT_SETTING_RENAME;
	changes[0].text = work->rename;
	changes[1].key = AXIS_IMPORT_SETTING_LAYOUT;
	changes[1].text = work->layout;
	changes[2].key = AXIS_IMPORT_SETTING_FORMAT;
	changes[2].text = work->format;
	axis_settings_set_many(self->settings, changes, 3, &work->changed, &work->problems, &work->error);
}

static HtmxResponse *
save_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	SaveWork *work = data;
	guint i;

	if (g_error_matches(work->error, AXIS_ERROR, AXIS_ERROR_SETTING)) {
		const AxisSettingProblem *problem = work->problems != NULL && work->problems->len > 0 ?
			g_ptr_array_index(work->problems, 0) : NULL;
		const AxisSettingInfo *declared = problem != NULL && self->settings != NULL ?
			axis_settings_lookup(self->settings, problem->key, NULL) : NULL;
		g_autofree gchar *text = problem == NULL ? g_strdup("Nothing was saved: a value is not valid.") :
			g_strdup_printf("Nothing was saved: \"%s\" %s (%s).", declared != NULL ? declared->label : problem->key,
				problem->reason, problem->key);

		return naming_page(self, request, 400, text, TRUE, work->rename, work->layout, work->format);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	for (i = 0; work->changed != NULL && work->changed[i] != NULL; i++)
		axis_web_storage_audit(request, "setting changed: key=%s", work->changed[i]);
	return axis_web_place_redirect(request, "/imports/naming", work->changed == NULL || work->changed[0] == NULL ?
		"unchanged" : "saved", "naming-card");
}

static HtmxResponse *
handle_naming_save(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebImports *self = user_data;
	const gchar *rename = htmx_request_get_form_value(request, AXIS_IMPORT_SETTING_RENAME);
	const gchar *layout = htmx_request_get_form_value(request, AXIS_IMPORT_SETTING_LAYOUT);
	const gchar *format = htmx_request_get_form_value(request, AXIS_IMPORT_SETTING_FORMAT);
	g_autofree gchar *reason = NULL;
	SaveWork *work;

	if (self->settings == NULL || rename == NULL || layout == NULL || format == NULL)
		return axis_web_error_respond(request, 400, "Not saved", "The form is not complete.");
	if (!axis_import_format_check(format, &reason)) {
		g_autofree gchar *text = g_strdup_printf("Nothing was saved: %s", reason);

		return naming_page(self, request, 400, text, TRUE, rename, layout, format);
	}
	work = g_new0(SaveWork, 1);
	work->rename = g_strdup(rename);
	work->layout = g_strdup(layout);
	work->format = g_strdup(format);
	return axis_web_imports_dispatch(self, request, save_run, save_done, work, save_work_free);
}

/* ── Routes ─────────────────────────────────────────────────────── */

void
axis_web_imports_register_pages(
	AxisWebImports *self,
	AxisWeb        *web
){
	axis_web_get(web, "/imports", handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/manual", AXIS_WEB_ROUTE_ADMIN, handle_manual, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/manual/inspect", AXIS_WEB_ROUTE_ADMIN, handle_inspect, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/imports/manual", AXIS_WEB_ROUTE_ADMIN, handle_manual_post, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/naming", AXIS_WEB_ROUTE_ADMIN, handle_naming, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/naming/preview", AXIS_WEB_ROUTE_ADMIN, handle_preview, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/imports/naming", AXIS_WEB_ROUTE_ADMIN, handle_naming_save, self);
	axis_web_get(web, "/imports/:id", handle_import, self);
	axis_web_imports_register_assign(self, web);
	axis_web_imports_register_delete(self, web);
	axis_web_route_full(web, HTMX_METHOD_POST, "/imports/:id/act", AXIS_WEB_ROUTE_ADMIN, handle_act, self);
}
