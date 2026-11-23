/*
 * axis-web-imports-duplicates.c - the Duplicates page: movies and episodes
 * that hold more than one linked file, which one Axis would keep, and the
 * clean-up of the others into the recycle folder (add-force-grab-and-assign,
 * slice D)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /duplicates                      every group, each file with its
 *                                         facts and the recommendation;
 *                                         anyone signed in, no actions for a
 *                                         viewer
 *   GET  /duplicates/review?module=&owner=&item=
 *                                         one group: which files would go to
 *                                         the recycle folder and which stay,
 *                                         a tick for each removable file;
 *                                         writes nothing
 *   POST /duplicates/remove               module, owner, item, digest, and
 *                                         remove_<library file id>=yes for
 *                                         each ticked file
 *   GET  /duplicates/recycled             the duplicates removed here: where
 *                                         each is in the recycle folder, what
 *                                         it was a file of, who removed it
 *                                         and when, and when it is deleted;
 *                                         anyone signed in; a list only
 *
 * The review and the post are an administrator's.  A post that is out of
 * date, or that the rules refuse (every file ticked, a file that is not
 * offered, an import or a grab open), removes nothing and shows the review
 * again with status 409.  File names are set as text.
 */

#include "web/axis-web-imports-private.h"

#include <string.h>

#include "core/axis-size.h"
#include "library/axis-library-name.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"

#define ADD axis_web_storage_add

#define REMOVE_PREFIX "remove_"
/* At most this many ticks are read from one post.  (The layer removes at
 * most AXIS_IMPORT_DUP_MAX_REMOVE files in one request, and says so; this
 * only bounds what is read.) */
#define REMOVE_MAX    200

typedef struct
{
	gboolean               list;
	gboolean               recycled;
	GPtrArray             *items;	/* AxisImportRecycledInfo */
	gboolean               remove;
	gchar                 *module;
	gint64                 owner_id;
	gint64                 item_id;
	GArray                *ticked;	/* gint64 */
	gchar                 *digest;
	gchar                 *user;
	GPtrArray             *groups;
	AxisImportDupGroup    *group;
	AxisImportDupOutcome   outcome;
	gchar                 *refusal;
	GError                *error;
} DupWork;

static void
dup_work_free(gpointer data)
{
	DupWork *work = data;

	g_free(work->module);
	g_clear_pointer(&work->ticked, g_array_unref);
	g_free(work->digest);
	g_free(work->user);
	g_clear_pointer(&work->groups, g_ptr_array_unref);
	g_clear_pointer(&work->items, g_ptr_array_unref);
	axis_import_dup_group_free(work->group);
	g_free(work->refusal);
	g_clear_error(&work->error);
	g_free(work);
}

static void
dup_run(
	AxisWebImports *self,
	gpointer        data
){
	DupWork *work = data;

	if (work->list)
		axis_import_duplicates_list(self->import, &work->groups, &work->error);
	else if (work->recycled)
		axis_import_duplicates_recycled(self->import, &work->items, &work->error);
	else if (work->remove)
		axis_import_duplicates_remove(self->import, work->module, work->owner_id, work->item_id, work->ticked,
			work->digest, work->user, &work->outcome, &work->refusal, &work->group, &work->error);
	else
		axis_import_duplicates_review(self->import, work->module, work->owner_id, work->item_id, &work->group,
			&work->error);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

static const gchar *
quality_of(const AxisImportDupFile *f)
{
	if (f->file->quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(f->file->quality);
	return axis_quality_unknown_source_label(f->file->resolution);
}

/* The file's running time as the analysis read it from the file as it is
 * now, or why there is none. */
static gchar *
runs_text(const AxisImportDupFile *f)
{
	gint64 seconds = f->duration_ms / 1000;

	if (f->probe == NULL)
		return g_strdup("Not analysed yet");
	if (strcmp(f->probe, "ok") != 0 || f->duration_ms <= 0)
		return g_strdup("Could not be read");
	if (seconds >= 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %" G_GINT64_FORMAT " min", seconds / 3600, seconds % 3600 / 60);
	if (seconds >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " min", seconds / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " s", seconds);
}

/* The same for a sentence: "runs 1 h 32 min", "not analysed yet". */
static gchar *
runs_words(const AxisImportDupFile *f)
{
	g_autofree gchar *runs = runs_text(f);

	if (f->probe == NULL)
		return g_strdup("not analysed yet");
	if (strcmp(f->probe, "ok") != 0 || f->duration_ms <= 0)
		return g_strdup("could not be read by the analysis");
	return g_strdup_printf("runs %s", runs);
}

/* How the file came to be the title's, and who keeps it. */
static gchar *
linked_text(const AxisImportDupFile *f)
{
	const gchar *who = f->hand_by != NULL && f->hand_by[0] != '\0' ? f->hand_by : "a person";
	g_autofree gchar *how = NULL;

	if (g_strcmp0(f->hand_how, "forced") == 0)
		how = g_strdup_printf("Grabbed by force by %s.", who);
	else if (g_strcmp0(f->hand_how, "assigned") == 0)
		how = g_strdup_printf("Assigned by hand by %s.", who);
	else if (g_strcmp0(f->hand_how, "brought") == 0)
		how = g_strdup_printf("Migrated from Radarr or Sonarr by %s.", who);
	else if (f->hand_how != NULL)
		how = g_strdup_printf("Linked by hand by %s.", who);
	else if (f->by_import)
		how = g_strdup("Imported by Axis.");
	else
		how = g_strdup("Found on disk and identified.");
	return g_strdup_printf("%s%s%s%s%s", how, f->file->kept ? " Kept by hand." : "", f->volume_name != NULL ? " On " : "",
		f->volume_name != NULL ? f->volume_name : "", f->volume_name != NULL ? "." : "");
}

static gchar *
retention_text(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 86400, seconds == 86400 ? "day" : "days");
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 3600, seconds == 3600 ? "hour" : "hours");
	return g_strdup_printf("%" G_GINT64_FORMAT " seconds", seconds);
}

static gchar *
review_href(const AxisImportDupGroup *group)
{
	return g_strdup_printf("/duplicates/review?module=%s&owner=%" G_GINT64_FORMAT "&item=%" G_GINT64_FORMAT,
		group->module, group->owner_id, group->item_id);
}

/* A checkbox that posts "yes", ticked when @checked. */
static HtmxNode *
tick_box(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	ADD(field, box);
	ADD(field, text);
	return HTMX_NODE(field);
}

/* One group on the list: its files, the recommendation, and for an
 * administrator the way to its review. */
static HtmxNode *
group_card(
	const AxisImportDupGroup *group,
	gboolean                  admin
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_WIDE },
		{ "Quality", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Running time", AXIS_WEB_COL_TEXT },
		{ "Recommendation", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	g_autofree gchar *table_id = g_strdup_printf("duplicates-%s-%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT, group->module,
		group->owner_id, group->item_id);
	HtmxNode *card = axis_web_storage_card(group->label);
	HtmxNode *table = axis_web_table_new(table_id, columns);
	HtmxTbody *body = htmx_tbody_new();
	gboolean removable = FALSE;
	guint i;

	ADD(card, axis_web_storage_text(NULL, group->summary));
	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autofree gchar *size = axis_size_format((guint64)MAX(f->file->size, 0));
		g_autofree gchar *linked = linked_text(f);
		g_autofree gchar *runs = runs_text(f);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();

		ADD(name, axis_web_browser_name_node(f->file->rel_path));
		ADD(name, axis_web_storage_text("range-detail", linked));
		if (f->also != NULL)
			ADD(name, axis_web_storage_text("range-detail", f->also));
		ADD(row, name);
		ADD(row, htmx_td_new_with_text(quality_of(f)));
		ADD(row, htmx_td_new_with_text(size));
		ADD(row, htmx_td_new_with_text(runs));
		ADD(row, htmx_td_new_with_text(f->advice_text));
		ADD(body, row);
		removable = removable || f->fixed == NULL;
	}
	ADD(table, body);
	ADD(card, table);
	if (group->href != NULL)
		ADD(card, axis_web_browser_link(group->href, group->title_label));
	if (admin && removable) {
		g_autofree gchar *href = review_href(group);

		ADD(card, axis_web_browser_link(href, "Review the clean-up"));
	} else if (admin) {
		ADD(card, axis_web_storage_text("help", "No file of it can be removed now: each says why."));
	}
	return card;
}

static HtmxResponse *
list_done(
	HtmxRequest   *request,
	const DupWork *work
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean admin = axis_web_request_is_admin(request);
	HtmxHeading *h1 = htmx_h1_new();
	guint64 n = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), "Duplicates");
	ADD(content, h1);
	ADD(content, axis_web_storage_text(NULL, "Movies and episodes that have more than one file linked. Axis keeps one "
		"copy of each, so a second file is usually left from a grab by force, an assignment or a link made by hand. For "
		"each, Axis says which file it would keep and why. Nothing is removed until an administrator reviews a group "
		"and confirms, and a removed file goes to the recycle folder of its disk first."));
	if (done != NULL && g_ascii_string_to_unsigned(done, 10, 1, 1000, &n, NULL)) {
		g_autofree gchar *text = g_strdup_printf("%" G_GUINT64_FORMAT " %s removed from the library. The title's history "
			"says which.", n, n == 1 ? "file was" : "files were");

		ADD(content, axis_web_auth_notice(FALSE, text));
	}
	if (work->groups->len == 0)
		ADD(content, axis_web_storage_text(NULL, "No movie and no episode has more than one file linked."));
	for (i = 0; i < work->groups->len; i++)
		ADD(content, group_card(g_ptr_array_index(work->groups, i), admin));
	ADD(content, axis_web_storage_text("help", "Files with the same content, and folders of the same title in more than "
		"one place, whether or not Axis links them, are listed under Disk usage."));
	ADD(content, axis_web_browser_link("/analysis/duplicates", "Likely duplicate files (Disk usage)"));
	ADD(content, axis_web_storage_text("help", "A file removed here is kept in the recycle folder of its disk for a "
		"time. The list of them says where each one is and when it is deleted."));
	ADD(content, axis_web_browser_link("/duplicates/recycled", "Recycled duplicates"));
	return axis_web_page_respond(request, "Duplicates", HTMX_NODE(content));
}

/* The duplicates removed on this page: where each is now, and until when.
 * A list only: a file is put back by hand. */
static HtmxResponse *
recycled_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	const DupWork  *work
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_WIDE },
		{ "Was a file of", AXIS_WEB_COL_TEXT },
		{ "Removed", AXIS_WEB_COL_DATE },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Deleted", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *h1 = htmx_h1_new();
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), "Recycled duplicates");
	ADD(content, h1);
	ADD(content, axis_web_storage_text(NULL, "The files removed on the Duplicates page. Each was moved into the recycle "
		"folder of its own disk (.axis-recycle at the top of the disk), under the path it had in the library, and is "
		"deleted from there after the time set in \"Keep files replaced by an upgrade for\" (Settings, Imports). Until "
		"then you can put one back by hand: move it from the place shown here to where it was, then scan the "
		"library. Axis has no button for that."));
	if (work->items->len == 0) {
		ADD(content, axis_web_storage_text(NULL, "No duplicate was removed yet."));
	} else {
		HtmxNode *card = axis_web_storage_card("Removed duplicates, the latest first");
		HtmxNode *table = axis_web_table_new("duplicates-recycled", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->items->len; i++) {
			const AxisImportRecycledInfo *item = g_ptr_array_index(work->items, i);
			g_autofree gchar *retired = axis_web_format_time(item->retired_us, "");
			g_autofree gchar *who = g_strdup_printf("%s%s%s", retired, item->by_user != NULL ? ", by " : "",
				item->by_user != NULL ? item->by_user : "");
			g_autofree gchar *where = NULL;
			g_autofree gchar *when = NULL;
			const gchar *state;
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name = htmx_td_new();
			HtmxTd *due = htmx_td_new();

			ADD(name, axis_web_browser_name_node(item->original_path));
			if (item->recycle_path != NULL) {
				g_autofree gchar *shown = axis_library_name_display(item->recycle_path);

				where = g_strdup_printf("Now %s on %s", shown, item->volume_name != NULL ? item->volume_name : "its disk");
				ADD(name, axis_web_storage_text("range-detail", where));
			}
			ADD(row, name);
			ADD(row, htmx_td_new_with_text(item->label));
			ADD(row, htmx_td_new_with_text(who));
			if (g_strcmp0(item->state, "kept") == 0) {
				/* Due and still here: say what holds the deletion back. */
				g_autofree gchar *wait = axis_web_storage_removal_wait_note(axis_import_get_storage(self->import),
					item->volume_id, item->remove_after_us);

				state = "In the recycle folder";
				when = axis_web_format_time(item->remove_after_us, "");
				htmx_node_set_text_content(HTMX_NODE(due), when);
				if (wait != NULL)
					ADD(due, axis_web_storage_text("range-detail", wait));
			} else if (g_strcmp0(item->state, "removed") == 0) {
				state = "Deleted";
				when = axis_web_format_time(item->removed_us, "");
				htmx_node_set_text_content(HTMX_NODE(due), when);
			} else if (g_strcmp0(item->state, "moving") == 0) {
				state = "Being recorded";
				htmx_node_set_text_content(HTMX_NODE(due), "Not set yet: the clean-up job finishes its record");
			} else if (g_strcmp0(item->state, "gone") == 0) {
				state = "Gone from the recycle folder";
				htmx_node_set_text_content(HTMX_NODE(due), "It was no longer there when its time came");
			} else {
				state = "Left alone";
				htmx_node_set_text_content(HTMX_NODE(due), "Something else is at its place in the recycle folder");
			}
			ADD(row, htmx_td_new_with_text(state));
			ADD(row, due);
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
		ADD(content, card);
	}
	ADD(content, axis_web_browser_link("/duplicates", "Back to Duplicates"));
	return axis_web_page_respond(request, "Recycled duplicates", HTMX_NODE(content));
}

/* The review of one group: what goes and what stays, and the form. */
static HtmxNode *
review_card(
	HtmxRequest              *request,
	const AxisImportDupGroup *group
){
	g_autofree gchar *owner = g_strdup_printf("%" G_GINT64_FORMAT, group->owner_id);
	g_autofree gchar *item = g_strdup_printf("%" G_GINT64_FORMAT, group->item_id);
	HtmxNode *card = axis_web_storage_card("Which files go, and which stay");
	HtmxNode *form = axis_web_auth_post_form(request, "/duplicates/remove");
	HtmxUl *stays = htmx_ul_new();
	gboolean any_stays = FALSE;
	gboolean any_offered = FALSE;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(stays), "release-reasons");
	ADD(card, axis_web_storage_text(NULL, group->summary));
	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autofree gchar *shown = axis_library_name_display(f->file->rel_path);
		g_autofree gchar *size = axis_size_format((guint64)MAX(f->file->size, 0));
		g_autofree gchar *linked = linked_text(f);
		g_autofree gchar *runs = runs_words(f);

		if (f->fixed != NULL) {
			g_autofree gchar *text = g_strdup_printf("%s (%s, %s, %s). %s %s", shown, quality_of(f), size, runs, linked,
				f->advice_text);
			HtmxLi *li = htmx_li_new();

			ADD(li, axis_web_breakable(text));
			ADD(stays, li);
			any_stays = TRUE;
		} else {
			g_autofree gchar *id = g_strdup_printf("dup-remove-%" G_GINT64_FORMAT, f->file->library_file_id);
			g_autofree gchar *name = g_strdup_printf(REMOVE_PREFIX "%" G_GINT64_FORMAT, f->file->library_file_id);
			g_autofree gchar *label = g_strdup_printf("Remove %s (%s, %s, %s). %s %s%s%s", shown, quality_of(f), size, runs,
				linked, f->advice_text, f->also != NULL ? " " : "", f->also != NULL ? f->also : "");

			ADD(form, tick_box(id, label, name, f->ticked));
			any_offered = TRUE;
		}
	}
	if (any_stays) {
		ADD(card, axis_web_storage_text("help", "These stay whatever you tick: Axis does not offer them for removal."));
		ADD(card, stays);
	} else {
		g_object_unref(stays);
	}
	if (!any_offered) {
		g_object_unref(form);
		ADD(card, axis_web_storage_text("help", "No file of it can be removed now."));
		return card;
	}
	ADD(form, axis_web_storage_text("help", "A file you tick goes; a file you do not tick stays where it is, linked as "
		"it is. At least one file must stay, and it must be one Axis can see on its disk and could read when it "
		"analysed it: a request that would leave only a file that is missing, empty or unreadable is refused, as is "
		"one that ticks them all. Subtitle files beside a removed file are left where they are."));
	if (group->retention_s == 0) {
		HtmxNode *warning = axis_web_auth_notice(TRUE, "The recycle folder keeps nothing (import.recycle_retention is "
			"0): a file removed here is deleted for good at the next clean-up, within the hour. It cannot be brought "
			"back.");

		ADD(form, warning);
	} else {
		g_autofree gchar *kept = retention_text(group->retention_s);
		g_autofree gchar *text = g_strdup_printf("A removed file is moved into the recycle folder of its own disk "
			"(.axis-recycle), under its library path, and deleted from there after %s. Until then it can be moved back "
			"by hand.", kept);

		ADD(form, axis_web_storage_text("help", text));
	}
	ADD(form, axis_web_auth_hidden("module", group->module));
	ADD(form, axis_web_auth_hidden("owner", owner));
	ADD(form, axis_web_auth_hidden("item", item));
	ADD(form, axis_web_auth_hidden("digest", group->digest));
	ADD(form, axis_web_auth_button(group->retention_s == 0 ? "Delete the ticked files for good" :
		"Move the ticked files to the recycle folder", "button-danger"));
	ADD(card, form);
	return card;
}

/* What a removal did, when a file could not be moved. */
static HtmxNode *
result_card(const AxisImportDupGroup *group)
{
	HtmxNode *card = axis_web_storage_card("What was done");
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);
		g_autofree gchar *shown = axis_library_name_display(f->file->rel_path);
		g_autofree gchar *text = NULL;
		HtmxLi *li = htmx_li_new();

		if (f->removed)
			text = g_strdup_printf("%s: removed from the library.", shown);
		else if (f->unrecorded)
			text = g_strdup_printf("%s: moved, but not recorded. %s", shown, f->failure != NULL ? f->failure : "");
		else if (f->failure != NULL)
			text = g_strdup_printf("%s: not removed, and still linked as it was. %s", shown, f->failure);
		else
			text = g_strdup_printf("%s: stays.", shown);
		ADD(li, axis_web_breakable(text));
		ADD(list, li);
	}
	ADD(card, list);
	return card;
}

static HtmxResponse *
dup_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	DupWork *work = data;
	const AxisImportDupGroup *group = work->group;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *title = NULL;
	HtmxResponse *response;
	HtmxHeading *h1;
	guint status = 200;
	guint removed = 0;
	guint unrecorded = 0;
	guint failed = 0;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->list)
		return list_done(request, work);
	if (work->recycled)
		return recycled_done(self, request, work);
	if (group == NULL)
		return axis_web_error_respond(request, work->remove ? 409 : 404, work->remove ? "Nothing removed" : "Not found",
			"That title does not have more than one file linked now. Nothing was removed. Open Duplicates to see how it "
			"stands.");
	for (i = 0; i < group->files->len; i++) {
		const AxisImportDupFile *f = g_ptr_array_index(group->files, i);

		removed += f->removed ? 1 : 0;
		unrecorded += f->unrecorded ? 1 : 0;
		failed += f->failure != NULL ? 1 : 0;
	}
	if (work->remove && work->outcome == AXIS_IMPORT_DUP_DONE) {
		axis_web_storage_audit(request, "duplicates of %s: %u file(s) removed into the recycle folder, %u moved there "
			"with the record unfinished, %u could not be moved%s", group->label, removed, unrecorded, failed - unrecorded,
			group->history_lost ? "; the title's history could not be written" : "");
		if (failed == 0 && !group->history_lost) {
			g_autofree gchar *location = g_strdup_printf("/duplicates?done=%u", removed);

			return axis_web_auth_redirect(request, location);
		}
	}
	content = htmx_div_new();
	title = g_strdup_printf("Clean up duplicates: %s", group->label);
	h1 = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	if (work->remove && work->outcome == AXIS_IMPORT_DUP_DONE) {
		if (failed > 0)
			ADD(content, axis_web_auth_notice(TRUE, unrecorded > 0 ? "A file was moved into the recycle folder, but "
				"Axis could not record that: it is no longer at its place, though the library still lists it for now. "
				"Each line says how it went." : "Not every ticked file could be removed. Each line says how it went."));
		if (group->history_lost)
			ADD(content, axis_web_auth_notice(TRUE, "The files were removed as the lines below say, but the title's "
				"history could not be written (the database could not be reached). The removal is in Axis's log, and "
				"Recycled duplicates lists the files."));
		ADD(content, result_card(group));
		ADD(content, axis_web_browser_link("/duplicates/recycled", "Recycled duplicates"));
	} else {
		if (work->remove) {
			/* Nothing was removed: say why, and show it as it stands. */
			status = 409;
			ADD(content, axis_web_auth_notice(TRUE, work->outcome == AXIS_IMPORT_DUP_CHANGED ?
				"Nothing was removed: the files, their links or their disks changed since this page was shown. This is "
				"how it stands now: look again, then confirm." : work->refusal != NULL ? work->refusal :
				"Nothing was removed."));
		}
		if (group->busy != NULL) {
			if (!work->remove)
				ADD(content, axis_web_auth_notice(TRUE, group->busy));
			ADD(content, axis_web_storage_text(NULL, group->summary));
		} else {
			ADD(content, review_card(request, group));
		}
	}
	ADD(content, axis_web_browser_link("/duplicates", "Back to Duplicates"));
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

/* ── Handlers ───────────────────────────────────────────────────── */

static gboolean
module_valid(const gchar *module)
{
	gsize i;

	if (module == NULL || module[0] == '\0' || strlen(module) > 32)
		return FALSE;
	for (i = 0; module[i] != '\0'; i++) {
		if (!g_ascii_islower(module[i]) && !g_ascii_isdigit(module[i]) && module[i] != '-')
			return FALSE;
	}
	return TRUE;
}

static gboolean
number_of(
	const gchar *text,
	gint64       min,
	gint64      *value
){
	guint64 number = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, (guint64)min, G_MAXINT64, &number, NULL))
		return FALSE;
	*value = (gint64)number;
	return TRUE;
}

static gboolean
digest_valid(const gchar *digest)
{
	guint i;

	if (digest == NULL || strlen(digest) != 32)
		return FALSE;
	for (i = 0; i < 32; i++) {
		if (!g_ascii_isxdigit(digest[i]) || g_ascii_isupper(digest[i]))
			return FALSE;
	}
	return TRUE;
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DupWork *work = g_new0(DupWork, 1);

	work->list = TRUE;
	return axis_web_imports_dispatch(user_data, request, dup_run, dup_done, work, dup_work_free);
}

static HtmxResponse *
handle_recycled(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DupWork *work = g_new0(DupWork, 1);

	work->recycled = TRUE;
	return axis_web_imports_dispatch(user_data, request, dup_run, dup_done, work, dup_work_free);
}

static HtmxResponse *
handle_review(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *module = htmx_request_get_query_param(request, "module");
	DupWork *work;
	gint64 owner = 0;
	gint64 item = 0;

	if (!module_valid(module) || !number_of(htmx_request_get_query_param(request, "owner"), 1, &owner) ||
	    !number_of(htmx_request_get_query_param(request, "item"), 0, &item))
		return axis_web_error_respond(request, 404, "Not found", "There is no such duplicate.");
	work = g_new0(DupWork, 1);
	work->module = g_strdup(module);
	work->owner_id = owner;
	work->item_id = item;
	return axis_web_imports_dispatch(user_data, request, dup_run, dup_done, work, dup_work_free);
}

static HtmxResponse *
handle_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *module = htmx_request_get_form_value(request, "module");
	const gchar *digest = htmx_request_get_form_value(request, "digest");
	GHashTable *fields = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	DupWork *work;
	gint64 owner = 0;
	gint64 item = 0;

	if (!module_valid(module) || !number_of(htmx_request_get_form_value(request, "owner"), 1, &owner) ||
	    !number_of(htmx_request_get_form_value(request, "item"), 0, &item) || !digest_valid(digest))
		return axis_web_error_respond(request, 400, "Nothing removed", "Review the duplicates first.");
	work = g_new0(DupWork, 1);
	work->remove = TRUE;
	work->module = g_strdup(module);
	work->owner_id = owner;
	work->item_id = item;
	work->digest = g_strdup(digest);
	work->user = g_strdup(axis_web_request_get_username(request));
	work->ticked = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (fields != NULL) {
		g_hash_table_iter_init(&iter, fields);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			const gchar *name = key;
			gint64 file = 0;

			if (name == NULL || !g_str_has_prefix(name, REMOVE_PREFIX) || g_strcmp0(value, "yes") != 0)
				continue;
			/* A tick that cannot be read is never dropped silently: the
			 * request is refused whole. */
			if (!number_of(name + strlen(REMOVE_PREFIX), 1, &file) || work->ticked->len >= REMOVE_MAX) {
				dup_work_free(work);
				return axis_web_error_respond(request, 400, "Nothing removed", "A tick of the request could not be read. "
					"Nothing was removed.");
			}
			g_array_append_val(work->ticked, file);
		}
	}
	return axis_web_imports_dispatch(user_data, request, dup_run, dup_done, work, dup_work_free);
}

void
axis_web_imports_register_duplicates(
	AxisWebImports *self,
	AxisWeb        *web
){
	axis_web_get(web, "/duplicates", handle_list, self);
	axis_web_get(web, "/duplicates/recycled", handle_recycled, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/duplicates/review", AXIS_WEB_ROUTE_ADMIN, handle_review, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/duplicates/remove", AXIS_WEB_ROUTE_ADMIN, handle_remove, self);
}
