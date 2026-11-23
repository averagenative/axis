/*
 * axis-web-imports-delete.c - "Delete this download" for an import that is
 * blocked, waiting or partly imported: its confirmation and its post
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only: both routes are AXIS_WEB_ROUTE_ADMIN, and the form
 * posts with the session's CSRF field.  The confirmation (GET) writes
 * nothing: it shows what the import module's own decision says would go
 * (axis_import_delete_preview(): the place, whether it is a folder or one
 * file, the number of files and their size, who deletes them, what stays),
 * or which rule refuses, with no button.  The post carries the digest of
 * what the page showed; the import module makes the decision again, and when
 * it is not what was shown, nothing is written and the confirmation is shown
 * again with status 409.  Names from the disk, the release and the title are
 * set as text.
 */

#include "web/axis-web-imports-private.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

typedef struct
{
	gint64                    id;
	gboolean                  post;
	gchar                    *digest;
	gboolean                  block;
	gchar                    *user;
	AxisImportDeletePreview  *preview;
	AxisImportDeleteOutcome   outcome;
	/* The post was made: the import module said so (never assumed from the
	 * absence of an error). */
	gboolean                  done;
	/* The post was refused because what the page showed is not so any more:
	 * why, and what is so now (@preview). */
	gchar                    *changed;
	GError                   *error;
} DeleteWork;

static void
delete_work_free(gpointer data)
{
	DeleteWork *work = data;

	g_free(work->digest);
	g_free(work->user);
	g_free(work->changed);
	axis_import_delete_preview_free(work->preview);
	g_clear_error(&work->error);
	g_free(work);
}

static void
delete_run(
	AxisWebImports *self,
	gpointer        data
){
	DeleteWork *work = data;

	if (work->post) {
		if (axis_import_delete_download(self->import, work->id, work->digest, work->block, work->user, &work->outcome,
		        &work->error)) {
			work->done = TRUE;
			return;
		}
		/* A failure that gives no reason is a failure all the same: never
		 * recorded and answered as a deletion. */
		if (work->error == NULL)
			g_set_error_literal(&work->error, G_IO_ERROR, G_IO_ERROR_FAILED, "The deletion failed and gave no reason.");
		if (!g_error_matches(work->error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED))
			return;
		/* Nothing was written: the page again, with what is so now. */
		work->changed = g_strdup(work->error->message);
		g_clear_error(&work->error);
	}
	axis_import_delete_preview(self->import, work->id, &work->preview, &work->error);
}

static HtmxNode *
bullet(const gchar *text)
{
	HtmxLi *item = htmx_li_new();

	htmx_node_set_text_content(HTMX_NODE(item), text);
	return HTMX_NODE(item);
}

static HtmxResponse *
confirm_page(
	HtmxRequest *request,
	DeleteWork  *work
){
	const AxisImportDeletePreview *p = work->preview;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *back = g_strdup_printf("/imports/%" G_GINT64_FORMAT, work->id);
	g_autofree gchar *where = NULL;
	gboolean tv = g_strcmp0(p->module, "tv") == 0;
	HtmxHeading *title = htmx_h1_new();
	HtmxResponse *response;
	HtmxNode *card;
	HtmxNode *facts;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "Delete this download?");
	ADD(content, title);
	if (work->changed != NULL)
		ADD(content, axis_web_auth_notice(TRUE, work->changed));

	card = axis_web_storage_card("What would be deleted");
	htmx_element_set_id(HTMX_ELEMENT(card), "delete-download");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "Import of", axis_web_breakable(p->title_label != NULL ? p->title_label : ""));
	if (p->release_name != NULL)
		axis_web_browser_fact_node(facts, "Release", axis_web_breakable(p->release_name));
	where = g_strdup_printf("%s: %s", p->volume_name != NULL ? p->volume_name : "a disk no longer known",
		p->path != NULL ? p->path : "a place that was not recorded");
	axis_web_browser_fact_node(facts, "On the disk", axis_web_breakable(where));
	if (p->client_path != NULL)
		axis_web_browser_fact_node(facts, "At the download client", axis_web_breakable(p->client_path));
	if (p->allowed && !p->gone) {
		g_autofree gchar *size = axis_size_format(p->bytes);
		/* What a torrent's client deletes is the torrent's own files, which
		 * Axis does not know one by one: no promise about everything in the
		 * folder, whose count is what Axis sees there now. */
		g_autofree gchar *what = p->by == AXIS_IMPORT_DELETE_BY_CLIENT ?
			(p->file ? g_strdup("The torrent's own file, which its client deletes") :
			g_strdup_printf("The torrent's own files, which its client deletes. Its folder holds %u %s now; a file in "
				"it that is not the torrent's stays, and the folder with it.", p->n_files, p->n_files == 1 ? "file" :
				"files")) :
			p->file ? g_strdup("One file") :
			g_strdup_printf("A folder with everything in it: %u %s", p->n_files, p->n_files == 1 ? "file" : "files");

		axis_web_browser_fact(facts, "What", what);
		axis_web_browser_fact(facts, p->by == AXIS_IMPORT_DELETE_BY_CLIENT ? "Size of what is there now" : "Size", size);
		axis_web_browser_fact(facts, "Who deletes it", p->by == AXIS_IMPORT_DELETE_BY_CLIENT ?
			"The torrent client deletes the torrent's own files when Axis asks it to remove the torrent. Axis deletes "
			"nothing itself." :
			p->by == AXIS_IMPORT_DELETE_BY_AXIS_KEPT ?
			"The Usenet client removes the job and keeps its folder; Axis deletes the folder." :
			"The client no longer has it; Axis deletes what it left.");
	} else if (p->allowed) {
		axis_web_browser_fact(facts, "What", p->by == AXIS_IMPORT_DELETE_BY_AXIS_KEPT ?
			"Nothing is at that place any more. The import is closed and the client is asked to take the job off its "
			"list, without deleting anything; nothing is left on the disk to delete." :
			"Nothing is at that place any more, and the client no longer lists the download. The import is closed; "
			"nothing is left on the disk to delete, and the client is asked to delete nothing.");
	}
	ADD(card, facts);

	if (!p->allowed) {
		/* Which rule, where the files are, and no button. */
		ADD(card, axis_web_auth_notice(TRUE, p->refusal != NULL ? p->refusal : "Axis's rules do not let it be deleted."));
		ADD(content, card);
		ADD(content, axis_web_browser_link(back, "Back to the import"));
		response = axis_web_page_respond(request, "Delete this download?", HTMX_NODE(content));
		if (work->changed != NULL)
			htmx_response_set_status(response, 409);
		return response;
	}
	ADD(content, card);

	card = axis_web_storage_card("What stays, and what else happens");
	{
		HtmxUl *list = htmx_ul_new();
		g_autofree gchar *wanted = p->has_grab ?
			g_strdup_printf("The import is closed and its grab is closed. The %s wanted again; no search is started "
				"now, and the next scheduled search takes it up.", tv ? "episodes it had not imported are" : "movie is") :
			g_strdup("The import is closed. No grab is open for it, so there is none to close, and no search is "
				"started.");

		htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
		ADD(list, bullet("Deleting cannot be undone: the files do not go to a recycle folder."));
		ADD(list, bullet("Only this download's own folder or file goes. The library's folders, the client's download "
			"folder itself and everything else in it stay. No symbolic link is followed."));
		if (p->placed != NULL && p->placed->len > 0) {
			g_autofree gchar *kept = g_strdup_printf("%u %s this import already placed in the library %s:",
				p->placed->len, p->placed->len == 1 ? "file" : "files", p->placed->len == 1 ? "stays" : "stay");
			HtmxLi *item = htmx_li_new();
			HtmxUl *inner = htmx_ul_new();

			htmx_node_set_text_content(HTMX_NODE(item), kept);
			for (i = 0; i < p->placed->len && i < 20; i++) {
				HtmxLi *line = htmx_li_new();

				ADD(line, axis_web_breakable(g_ptr_array_index(p->placed, i)));
				ADD(inner, line);
			}
			if (p->placed->len > 20) {
				g_autofree gchar *more = g_strdup_printf("and %u more", p->placed->len - 20);

				ADD(inner, bullet(more));
			}
			ADD(item, inner);
			ADD(list, item);
		}
		if (p->n_linked > 0) {
			g_autofree gchar *linked = g_strdup_printf("%u of these files %s also in the library under %s (hard "
				"links). %s and %s content stay.", p->n_linked, p->n_linked == 1 ? "is" : "are",
				p->n_linked == 1 ? "another name" : "other names", p->n_linked == 1 ? "That name" : "Those names",
				p->n_linked == 1 ? "its" : "their");

			ADD(list, bullet(linked));
		}
		ADD(list, bullet(wanted));
		if (p->has_grab)
			ADD(list, bullet("The release is not blocked unless you tick the box, so the next search may take this "
				"release again."));
		ADD(list, bullet("If the client does not answer or the disk cannot be written in that moment, the import's "
			"page says what is still to be done, and Axis finishes it within the hour."));
		ADD(card, list);
	}
	{
		g_autofree gchar *action = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/delete-download", work->id);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		ADD(form, axis_web_auth_hidden("confirm", "yes"));
		ADD(form, axis_web_auth_hidden("digest", p->digest));
		if (p->has_grab)
			ADD(form, axis_web_storage_checkbox("delete-download-block", "Also put this release on the blocklist, so "
				"that no search takes it again", "block", FALSE));
		htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
		ADD(actions, axis_web_auth_button("Delete this download", "button-danger"));
		ADD(form, actions);
		ADD(card, form);
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, "Back to the import, without deleting"));
	response = axis_web_page_respond(request, "Delete this download?", HTMX_NODE(content));
	if (work->changed != NULL)
		htmx_response_set_status(response, 409);
	return response;
}

static HtmxResponse *
delete_done(
	AxisWebImports *self,
	HtmxRequest    *request,
	gpointer        data
){
	DeleteWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL) {
		guint status;

		if (work->error->domain != AXIS_IMPORT_ERROR)
			return axis_web_internal_error(request, work->error);
		status = work->error->code == AXIS_IMPORT_ERROR_NOT_FOUND ? 404 :
			work->error->code == AXIS_IMPORT_ERROR_INVALID ? 400 : 409;
		return axis_web_error_respond(request, status, status == 404 ? "Not found" : "Not deleted",
			work->error->message);
	}
	if (work->preview != NULL)
		return confirm_page(request, work);
	/* Neither made nor a page to show: nothing is recorded as deleted. */
	if (!work->done)
		return axis_web_error_respond(request, 500, "Not deleted", "The deletion could not be made, and Axis has no "
			"reason to give. Open the import's page to see what is so now.");
	axis_web_storage_audit(request, "import %" G_GINT64_FORMAT " delete_download%s", work->id, work->block ?
		" (and blocked)" : "");
	location = g_strdup_printf("/imports/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, work->outcome == AXIS_IMPORT_DELETE_DELETED ? "download_deleted" :
		work->outcome == AXIS_IMPORT_DELETE_GONE ? "download_gone" : work->outcome == AXIS_IMPORT_DELETE_LEFT ?
		"download_left" : "download_waiting", NULL);
}

static HtmxResponse *
handle_delete(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gboolean post = htmx_request_get_method(request) == HTMX_METHOD_POST;
	gint64 id = axis_web_storage_param_id(params);
	DeleteWork *work;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such import.");
	if (post) {
		const gchar *digest = htmx_request_get_form_value(request, "digest");

		if (!axis_web_storage_form_yes(request, "confirm") || digest == NULL || strlen(digest) != 64)
			return axis_web_error_respond(request, 400, "Not confirmed", "Open the confirmation from the import's "
				"page and confirm there.");
	}
	work = g_new0(DeleteWork, 1);
	work->id = id;
	work->post = post;
	if (post) {
		work->digest = g_strdup(htmx_request_get_form_value(request, "digest"));
		work->block = axis_web_storage_form_yes(request, "block");
		work->user = g_strdup(axis_web_request_get_username(request));
	}
	return axis_web_imports_dispatch(user_data, request, delete_run, delete_done, work, delete_work_free);
}

void
axis_web_imports_register_delete(
	AxisWebImports *self,
	AxisWeb        *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/imports/:id/delete-download", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/imports/:id/delete-download", AXIS_WEB_ROUTE_ADMIN, handle_delete, self);
}
