/*
 * axis-web-imports-leftovers.c - Downloads -> Leftovers: what is in each
 * download client's folder for Axis that no download and no import accounts
 * for, and the deliberate deletion of one entry at a time
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only (the routes are AXIS_WEB_ROUTE_ADMIN, and the forms
 * post with the session's CSRF field).  The list deletes nothing; a person
 * opens an entry's confirmation, ticks the box and posts, and the import
 * module asks every rule again (axis_import_leftover_delete()).  An entry
 * that vanished between the list and the deletion is said to be gone, on an
 * ordinary page.  Names from the disk are set as text.
 */

#include "web/axis-web-imports-private.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"

#define ADD axis_web_storage_add

static const AxisWebColumn columns[] = {
	{ "Name", AXIS_WEB_COL_NAME },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "Changed", AXIS_WEB_COL_DATE },
	{ "Can be deleted", AXIS_WEB_COL_WIDE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* ── The list ───────────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *folders;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_clear_pointer(&work->folders, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebImports *self,
	gpointer        data
){
	ListWork *work = data;

	axis_import_leftovers(self->import, 0, TRUE, &work->folders, &work->error);
}

static HtmxNode *
text_cell(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
entry_row(
	const AxisImportLeftoverFolder *folder,
	const AxisImportLeftover       *entry
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxTd *action = htmx_td_new();
	HtmxBdi *bdi = htmx_bdi_new();
	g_autofree gchar *size = axis_size_format(entry->size_bytes);
	g_autofree gchar *size_text = g_strdup_printf("%s%s", entry->size_partial ? "at least " : "", size);
	g_autofree gchar *changed = axis_web_format_time(entry->modified_us, "");

	ADD(bdi, axis_web_breakable(entry->name));
	ADD(name, bdi);
	if (entry->folder)
		ADD(name, axis_web_browser_badge("Folder", "badge-note"));
	ADD(row, name);
	ADD(row, text_cell(size_text));
	ADD(row, text_cell(changed));
	ADD(row, text_cell(entry->deletable ? "Yes" : entry->why));
	if (entry->deletable) {
		g_autofree gchar *escaped = g_uri_escape_string(entry->stored, NULL, FALSE);
		g_autofree gchar *href = g_strdup_printf("/downloads/leftovers/delete?client=%" G_GINT64_FORMAT "&name=%s"
			"&inode=%" G_GUINT64_FORMAT, folder->client_id, escaped, entry->inode);

		HtmxNode *link = axis_web_browser_link(href, "Delete");
		g_autofree gchar *aria = g_strdup_printf("Delete %s", entry->name);

		htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-label", aria);
		ADD(action, link);
	}
	ADD(row, action);
	return HTMX_NODE(row);
}

static HtmxNode *
folder_card(const AxisImportLeftoverFolder *folder)
{
	g_autofree gchar *card_title = g_strdup_printf("Leftovers at %s", folder->client_name);
	HtmxNode *card = axis_web_storage_card(card_title);
	HtmxNode *facts = axis_web_browser_facts_new();
	guint i;

	if (folder->folder != NULL)
		axis_web_browser_fact_node(facts, "Download folder for Axis", axis_web_breakable(folder->folder));
	if (folder->volume_name != NULL)
		axis_web_browser_fact(facts, "Disk", folder->volume_name);
	ADD(card, facts);
	if (folder->problem != NULL) {
		ADD(card, axis_web_storage_text("help", folder->problem));
		return card;
	}
	if (folder->caution != NULL)
		ADD(card, axis_web_storage_text("help", folder->caution));
	if (folder->n_total == 0 && folder->n_kept == 0) {
		ADD(card, axis_web_storage_text("help", "No leftovers: everything in this folder belongs to a download or an import."));
		return card;
	}
	{
		guint all = folder->n_total + folder->n_kept;
		g_autofree gchar *count = folder->more ?
			g_strdup_printf("More than %u files and folders in it belong to no download and no import; the first %u "
				"by name are shown.", (guint)AXIS_IMPORT_LEFTOVERS_COUNTED, folder->entries->len) :
			folder->n_total == 0 ?
			g_strdup("Nothing in it can be deleted now: what is listed is in use at the client, cannot be told apart "
				"from what it has, or is not Axis's to delete.") :
			all > folder->entries->len ?
			g_strdup_printf("%u files and folders in it belong to no download and no import; the first %u by name "
				"are shown.", folder->n_total, folder->entries->len) :
			g_strdup_printf("%u %s to no download and no import.", folder->n_total, folder->n_total == 1 ?
				"file or folder in it belongs" : "files and folders in it belong");

		ADD(card, axis_web_storage_text(NULL, count));
		if (folder->n_total > 0 && folder->n_kept > 0) {
			g_autofree gchar *kept = g_strdup_printf("%u more %s listed but cannot be deleted: %s in use at the "
				"download client, cannot be told apart from what it has, or %s not Axis's to delete.", folder->n_kept,
				folder->n_kept == 1 ? "file or folder is" : "files and folders are",
				folder->n_kept == 1 ? "it is" : "they are", folder->n_kept == 1 ? "is" : "are");

			ADD(card, axis_web_storage_text("help", kept));
		}
	}
	{
		HtmxNode *table = axis_web_table_new("leftovers", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < folder->entries->len; i++)
			ADD(body, entry_row(folder, g_ptr_array_index(folder->entries, i)));
		ADD(table, body);
		ADD(card, table);
	}
	return card;
}

static HtmxResponse *
list_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxHeading *title;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "Leftovers in the download folders");
	ADD(content, title);
	ADD(content, axis_web_storage_text(NULL, "What is in each download client's folder for Axis that no download Axis "
		"follows and no import still open accounts for. An import moves what it takes into the library, so none of "
		"this was imported: a download given up on whose files its client kept, one that failed, what a rule left of a "
		"download's folder (the import's page says which rule), something a person put there. Nothing here is "
		"deleted unless you ask, one at a time; a cleanup job of your own that deletes old things in these folders by "
		"age is safe too, as Axis expects that."));
	if (work->folders->len == 0) {
		HtmxP *line = htmx_p_new();
		HtmxSpan *words = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		htmx_node_set_text_content(HTMX_NODE(words), "No download client is set up yet. ");
		ADD(line, words);
		ADD(line, axis_web_browser_link("/downloads/clients", "Add a download client"));
		ADD(content, line);
	}
	for (i = 0; i < work->folders->len; i++)
		ADD(content, folder_card(g_ptr_array_index(work->folders, i)));
	ADD(content, axis_web_browser_link("/downloads", "Back to Downloads"));
	return axis_web_page_respond(request, "Leftovers in the download folders", HTMX_NODE(content));
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_imports_dispatch(user_data, request, list_run, list_done, g_new0(ListWork, 1), list_work_free);
}

/* ── Deleting one ───────────────────────────────────────────────── */

typedef struct
{
	gboolean                   post;
	gint64                     client_id;
	gchar                     *stored;
	guint64                    inode;
	gchar                     *user;
	GPtrArray                 *folders;	/* the confirmation's: this client's */
	AxisImportLeftoverOutcome  outcome;
	gchar                     *text;
	GError                    *error;
} DeleteWork;

static void
delete_work_free(gpointer data)
{
	DeleteWork *work = data;

	g_free(work->stored);
	g_free(work->user);
	g_free(work->text);
	g_clear_pointer(&work->folders, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
delete_run(
	AxisWebImports *self,
	gpointer        data
){
	DeleteWork *work = data;

	if (!work->post) {
		axis_import_leftovers(self->import, work->client_id, TRUE, &work->folders, &work->error);
		return;
	}
	axis_import_leftover_delete(self->import, work->client_id, work->stored, work->inode, &work->outcome, &work->text,
		&work->error);
}

/* The entry the confirmation is for, as the list sees it now. */
static const AxisImportLeftover *
find_entry(
	DeleteWork                       *work,
	const AxisImportLeftoverFolder  **folder_out
){
	guint i;
	guint k;

	*folder_out = NULL;
	for (i = 0; work->folders != NULL && i < work->folders->len; i++) {
		const AxisImportLeftoverFolder *folder = g_ptr_array_index(work->folders, i);

		if (folder->client_id != work->client_id)
			continue;
		*folder_out = folder;
		for (k = 0; k < folder->entries->len; k++) {
			const AxisImportLeftover *entry = g_ptr_array_index(folder->entries, k);

			if (g_strcmp0(entry->stored, work->stored) == 0)
				return entry;
		}
	}
	return NULL;
}

static HtmxResponse *
result_page(
	HtmxRequest *request,
	gboolean     is_error,
	const gchar *text
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();

	htmx_node_set_text_content(HTMX_NODE(title), "Delete a leftover");
	ADD(content, title);
	ADD(content, axis_web_auth_notice(is_error, text));
	ADD(content, axis_web_browser_link("/downloads/leftovers", "Back to Leftovers in the download folders"));
	return axis_web_page_respond(request, "Delete a leftover", HTMX_NODE(content));
}

static HtmxResponse *
confirm_page(
	HtmxRequest *request,
	DeleteWork  *work
){
	const AxisImportLeftoverFolder *folder = NULL;
	const AxisImportLeftover *entry = find_entry(work, &folder);
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *client = NULL;
	g_autofree gchar *inode = NULL;
	g_autofree gchar *size = NULL;
	g_autofree gchar *confirm = NULL;
	HtmxHeading *title;
	HtmxNode *card;
	HtmxNode *facts;
	HtmxNode *form;
	HtmxDiv *actions;
	g_autofree gchar *card_title = NULL;

	/* Gone since the list was made: said so, not an error. */
	if (folder == NULL || entry == NULL || entry->inode != work->inode)
		return result_page(request, FALSE, folder != NULL && folder->problem != NULL ? folder->problem :
			"That entry is not in the client's folder for Axis any more as it was listed (it was already gone, or "
			"something else has its name now): nothing was deleted. Look at the list again.");
	if (!entry->deletable)
		return result_page(request, TRUE, entry->why != NULL ? entry->why : "Axis's rules do not let it be deleted.");
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "Delete a leftover");
	ADD(content, title);
	card_title = g_strdup_printf("Leftover at %s", folder->client_name);
	card = axis_web_storage_card(card_title);
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "Name", axis_web_breakable(entry->name));
	axis_web_browser_fact(facts, "What", entry->folder ? "A folder, with everything in it" : "A file");
	size = axis_size_format(entry->size_bytes);
	axis_web_browser_fact(facts, "Size", size);
	if (folder->folder != NULL)
		axis_web_browser_fact_node(facts, "In folder", axis_web_breakable(folder->folder));
	ADD(card, facts);
	ADD(card, axis_web_storage_text(NULL, "No download Axis follows and no import still open accounts for it, so it "
		"was not imported. Deleting it cannot be undone. Only this entry goes, through the same strict removal Axis uses "
		"for a download's folder: no symbolic link is followed, no other filesystem is entered, and a file that has "
		"another name (in the library) keeps it."));
	form = axis_web_auth_post_form(request, "/downloads/leftovers/delete");
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	client = g_strdup_printf("%" G_GINT64_FORMAT, work->client_id);
	inode = g_strdup_printf("%" G_GUINT64_FORMAT, entry->inode);
	ADD(form, axis_web_auth_hidden("client", client));
	ADD(form, axis_web_auth_hidden("name", entry->stored));
	ADD(form, axis_web_auth_hidden("inode", inode));
	confirm = g_strdup_printf("Delete %s for good", entry->name);
	ADD(form, axis_web_storage_checkbox("leftover-confirm", confirm, "confirm", TRUE));
	actions = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
	ADD(actions, axis_web_auth_button("Delete this leftover", "button-danger"));
	ADD(form, actions);
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/downloads/leftovers", "Back to Leftovers in the download folders"));
	return axis_web_page_respond(request, "Delete a leftover", HTMX_NODE(content));
}

static HtmxResponse *
delete_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	DeleteWork *work = data;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (!work->post)
		return confirm_page(request, work);
	axis_web_storage_audit(request, "leftover of download client %" G_GINT64_FORMAT ": %s", work->client_id,
		work->text != NULL ? work->text : "");
	return result_page(request, work->outcome == AXIS_IMPORT_LEFTOVER_REFUSED ||
		work->outcome == AXIS_IMPORT_LEFTOVER_CHANGED, work->text != NULL ? work->text : "Nothing was done.");
}

static HtmxResponse *
handle_delete(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gboolean post = htmx_request_get_method(request) == HTMX_METHOD_POST;
	const gchar *client = post ? htmx_request_get_form_value(request, "client") :
		htmx_request_get_query_param(request, "client");
	const gchar *name = post ? htmx_request_get_form_value(request, "name") :
		htmx_request_get_query_param(request, "name");
	const gchar *inode = post ? htmx_request_get_form_value(request, "inode") :
		htmx_request_get_query_param(request, "inode");
	DeleteWork *work;
	guint64 client_id = 0;
	guint64 ino = 0;

	if (client == NULL || name == NULL || inode == NULL || name[0] == '\0' || strlen(name) > 4096 ||
	    !g_ascii_string_to_unsigned(client, 10, 1, G_MAXINT64, &client_id, NULL) ||
	    !g_ascii_string_to_unsigned(inode, 10, 1, G_MAXUINT64, &ino, NULL))
		return axis_web_error_respond(request, 400, "Bad request", "That is not an entry of a client's folder.");
	if (post && !axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to delete it.");
	work = g_new0(DeleteWork, 1);
	work->post = post;
	work->client_id = (gint64)client_id;
	work->stored = g_strdup(name);
	work->inode = ino;
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_imports_dispatch(user_data, request, delete_run, delete_done, work, delete_work_free);
}

void
axis_web_imports_register_leftovers(
	AxisWebImports *self,
	AxisWeb        *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/leftovers", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/downloads/leftovers/delete", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/downloads/leftovers/delete", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
}
