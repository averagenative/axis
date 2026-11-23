/*
 * axis-web-storage-roots.c - the library roots pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Adding or removing a root writes nothing to a disk.  Both are refused
 * while the root's volume is not online, by the storage layer, whatever
 * the page offered.
 */

#include "web/axis-web-storage-private.h"

#include <string.h>

#include "import/axis-import.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"

#define ADD axis_web_storage_add

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static gchar *
directory_text(
	const AxisVolumeInfo *volume,
	const AxisRootInfo   *root
){
	if (volume == NULL)
		return g_strdup(root->rel_path);
	if (root->rel_path[0] == '\0')
		return g_strdup_printf("%s (the top of the disk)", volume->path);
	return g_strdup_printf("%s/%s", volume->path, root->rel_path);
}

static const AxisVolumeInfo *
volume_of(
	GPtrArray *volumes,
	gint64     id
){
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);

		if (volume->id == id)
			return volume;
	}

	return NULL;
}

/* ── The list ───────────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *roots;
	GPtrArray *warnings;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	if (work->roots != NULL)
		g_ptr_array_unref(work->roots);
	g_clear_pointer(&work->warnings, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_work_run(
	AxisWebStorage *self,
	gpointer        data
){
	ListWork *work = data;
	g_autoptr(GError) error = NULL;

	if (!axis_storage_list_roots(self->storage, 0, &work->roots, &work->error))
		return;
	/* A download client's folder and a films root on different volumes:
	 * every import between them is a copy. */
	if (!axis_import_volume_warnings(axis_storage_get_pool(self->storage), &work->warnings, &error))
		g_debug("library roots: the volumes could not be compared: %s", error->message);
}

HtmxNode *
axis_web_storage_add_root_form(
	HtmxRequest *request,
	GPtrArray   *volumes,
	const gchar *return_path
){
	HtmxNode *form = axis_web_auth_post_form(request, "/storage/roots");
	HtmxNode *back = axis_web_setup_return_field(return_path);
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for("roots-volume");
	HtmxSelect *select = htmx_select_new_with_name("volume");
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Disk");
	htmx_element_set_id(HTMX_ELEMENT(select), "roots-volume");
	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, volume->id);
		g_autofree gchar *text = g_strdup_printf("%s (%s, %s)", volume->name, volume->path,
			axis_volume_state_label(volume->state));
		HtmxOption *option = htmx_option_new_with_value(value, text);

		if (!axis_volume_state_is_usable(volume->state))
			htmx_option_set_disabled(option, TRUE);
		ADD(select, option);
	}
	ADD(field, label);
	ADD(field, select);
	ADD(form, field);
	ADD(form, axis_web_storage_text_field("roots-path", "Directory", "path", NULL, "Movies",
		"Relative to the top of the disk, or the full path of a folder inside it. Empty means "
		"the whole disk."));
	ADD(form, axis_web_storage_media_select("roots-type", "type"));
	ADD(form, axis_web_storage_text_field("roots-name", "Name", "name", NULL, NULL,
		"Optional. The directory's own name is used when this is empty."));
	if (back != NULL)
		axis_web_auth_add(form, back);
	axis_web_auth_add(form, axis_web_auth_button("Add the library folder", "button-primary"));

	return form;
}

static HtmxResponse *
list_work_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	ListWork *work = data;
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	g_autoptr(HtmxDiv) content = NULL;
	gboolean admin = axis_web_request_is_admin(request);
	const gchar *notice = axis_web_storage_done_message(htmx_request_get_query_param(request, "done"));
	HtmxHeading *title;
	HtmxNode *card;
	AxisWebNotice placed;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), "Library folders");
	ADD(content, title);
	ADD(content, axis_web_page_intro("The folders on your disks that hold one kind of media each: movies, TV, "
		"music and so on. Axis scans them, and imports what it downloads into the one each title is set to."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	{
		HtmxNode *warning = axis_web_imports_volume_warnings(work->warnings);

		if (warning != NULL)
			ADD(content, warning);
	}

	card = axis_web_storage_card("All library folders");
	htmx_element_set_id(HTMX_ELEMENT(card), "roots");
	axis_web_notice_put(&placed, card, "roots");
	ADD(card, axis_web_storage_text("help", "A library folder is a folder on a disk that holds "
		"one kind of media. Library folders on one disk never contain one another."));
	if (work->roots->len == 0) {
		ADD(card, axis_web_storage_text("help", admin ?
			"No library folders yet. Add one with Add a library folder below." :
			"No library folders yet. An administrator adds them here."));
	} else {
		static const AxisWebColumn columns[] = {
			{ "Library folder", AXIS_WEB_COL_NAME },
			{ "Holds", AXIS_WEB_COL_TEXT },
			{ "Disk", AXIS_WEB_COL_NAME },
			{ "Directory", AXIS_WEB_COL_NAME },
			{ NULL, 0 }
		};
		static const AxisWebColumn admin_columns[] = {
			{ "Library folder", AXIS_WEB_COL_NAME },
			{ "Holds", AXIS_WEB_COL_TEXT },
			{ "Disk", AXIS_WEB_COL_NAME },
			{ "Directory", AXIS_WEB_COL_NAME },
			{ "", AXIS_WEB_COL_ACTIONS },
			{ NULL, 0 }
		};
		HtmxNode *table = admin ? axis_web_table_new("root-list-admin", admin_columns) :
			axis_web_table_new("root-list", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->roots->len; i++) {
			const AxisRootInfo *root = g_ptr_array_index(work->roots, i);
			const AxisVolumeInfo *volume = volume_of(volumes, root->volume_id);
			g_autofree gchar *directory = directory_text(volume, root);
			g_autofree gchar *href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT,
				root->volume_id);
			g_autofree gchar *root_href = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, root->id);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *name_cell = htmx_td_new();
			HtmxTd *volume_cell = htmx_td_new();
			HtmxTd *where = htmx_td_new();
			HtmxA *link = htmx_a_new_with_href(href);
			HtmxA *root_link = htmx_a_new_with_href(root_href);

			htmx_node_set_text_content(HTMX_NODE(link), volume != NULL ? volume->name : "unknown");
			ADD(volume_cell, link);
			ADD(where, htmx_code_new_with_text(directory));
			/* The root's page, with its scans (the library module). */
			htmx_node_set_text_content(HTMX_NODE(root_link), root->name);
			ADD(name_cell, root_link);
			ADD(row, name_cell);
			ADD(row, htmx_td_new_with_text(axis_media_type_label(root->media_type)));
			ADD(row, volume_cell);
			ADD(row, where);
			if (admin) {
				HtmxTd *action = htmx_td_new();
				g_autofree gchar *remove = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT
					"/remove", root->id);
				g_autofree gchar *aria = g_strdup_printf("Remove library folder %s", root->name);
				HtmxA *remove_link = htmx_a_new_with_href(remove);

				htmx_element_set_attribute(HTMX_ELEMENT(remove_link), "aria-label", aria);
				htmx_node_set_text_content(HTMX_NODE(remove_link), "Remove");
				ADD(action, remove_link);
				ADD(row, action);
			}
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);

	if (admin) {
		HtmxNode *add = axis_web_storage_card("Add a library folder");

		htmx_element_set_id(HTMX_ELEMENT(add), "roots-add");
		axis_web_notice_put(&placed, add, "roots-add");
		if (volumes->len == 0) {
			HtmxP *p = htmx_p_new();
			HtmxA *link = htmx_a_new_with_href("/storage");

			htmx_node_set_text_content(HTMX_NODE(p), "Add a disk first, under ");
			htmx_node_set_text_content(HTMX_NODE(link), "Disks and library folders");
			ADD(p, link);
			ADD(add, p);
		} else {
			ADD(add, axis_web_storage_add_root_form(request, volumes, NULL));
		}
		ADD(content, add);
	}

	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, "Library folders", HTMX_NODE(content));
}

static HtmxResponse *
handle_roots(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_storage_dispatch(user_data, request, list_work_run, list_work_done,
		g_new0(ListWork, 1), list_work_free);
}

/* ── Adding ─────────────────────────────────────────────────────── */

typedef struct
{
	gint64         id;		/* the root (removal) or the new root */
	gint64         volume_id;
	gchar         *path;
	gchar         *name;
	AxisMediaType  type;
	gboolean       back_to_volume;
	const gchar   *back;	/* the setup guide's address (static), or NULL */
	AxisRootInfo  *root;
	GError        *error;
} RootWork;

static void
root_work_free(gpointer data)
{
	RootWork *work = data;

	g_free(work->path);
	g_free(work->name);
	axis_root_info_free(work->root);
	g_clear_error(&work->error);
	g_free(work);
}

static void
add_run(
	AxisWebStorage *self,
	gpointer        data
){
	RootWork *work = data;

	axis_storage_add_root(self->storage, work->volume_id, work->path, work->type, work->name,
		&work->id, &work->error);
}

static HtmxResponse *
done_redirect(
	HtmxRequest *request,
	gboolean     back_to_volume,
	gint64       volume_id,
	const gchar *done,
	const gchar *roots_place
){
	g_autofree gchar *path = back_to_volume ?
		g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, volume_id) :
		g_strdup("/storage/roots");

	return axis_web_place_redirect(request, path, done, back_to_volume ? "volume-roots" : roots_place);
}

static HtmxResponse *
add_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "root add refused: volume=%" G_GINT64_FORMAT " code=%d",
			work->volume_id, work->error->code);
		if (work->error->domain != AXIS_STORAGE_ERROR)
			return axis_web_internal_error(request, work->error);
		return axis_web_error_respond(request, axis_web_storage_error_status(work->error),
			"The library folder was not added", work->error->message);
	}

	axis_web_storage_audit(request, "root added: id=%" G_GINT64_FORMAT " volume=%" G_GINT64_FORMAT
		" type=%s", work->id, work->volume_id, axis_media_type_to_string(work->type));
	if (work->back != NULL) {
		g_autofree gchar *location = axis_web_setup_return_location(work->back, "root-added");

		return axis_web_auth_redirect(request, location);
	}
	return done_redirect(request, work->back_to_volume, work->volume_id, "root-added", "roots-add");
}

static HtmxResponse *
handle_add(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	RootWork *work;
	guint64 volume_id = 0;
	AxisMediaType type;

	if (!axis_web_storage_form_u64(request, "volume", &volume_id) || volume_id == 0 ||
	    volume_id > G_MAXINT64)
		return axis_web_error_respond(request, 400, "The library folder was not added",
			"Choose a disk.");
	if (!axis_media_type_from_string(htmx_request_get_form_value(request, "type"), &type))
		return axis_web_error_respond(request, 400, "The library folder was not added",
			"Choose what the library folder holds.");

	work = g_new0(RootWork, 1);
	work->volume_id = (gint64)volume_id;
	work->path = g_strdup(htmx_request_get_form_value(request, "path"));
	work->name = g_strdup(htmx_request_get_form_value(request, "name"));
	work->type = type;
	work->back_to_volume = g_strcmp0(htmx_request_get_form_value(request, "return"), "volume") == 0;
	/* "return" is "volume" from a volume's page, or the setup guide's. */
	work->back = axis_web_setup_request_return(request);

	return axis_web_storage_dispatch(user_data, request, add_run, add_done, work, root_work_free);
}

/* ── Removing ───────────────────────────────────────────────────── */

static void
look_run(
	AxisWebStorage *self,
	gpointer        data
){
	RootWork *work = data;

	axis_storage_get_root(self->storage, work->id, &work->root, &work->error);
}

static HtmxResponse *
look_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *heading = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *return_value = NULL;
	HtmxHeading *title;
	HtmxNode *form;
	HtmxDiv *actions;
	HtmxA *cancel;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	volume = axis_storage_get_volume(self->storage, work->root->volume_id);
	directory = directory_text(volume, work->root);
	content = htmx_div_new();
	title = htmx_h1_new();
	heading = g_strdup_printf("Remove library folder %s", work->root->name);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(content, title);

	text = g_strdup_printf("Axis will forget the library folder %s (%s) at %s, and what it knows about the "
		"files under it. Nothing on the disk is deleted or changed.", work->root->name,
		axis_media_type_label(work->root->media_type), directory);
	ADD(content, axis_web_storage_text(NULL, text));
	if (volume != NULL && !axis_volume_state_is_usable(volume->state))
		ADD(content, axis_web_auth_notice(TRUE, "Its disk is not online now, and a library folder can be "
			"removed only while its disk is online. Mount the disk again first."));

	action = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/remove", work->root->id);
	back = work->back_to_volume ?
		g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, work->root->volume_id) :
		g_strdup("/storage/roots");
	return_value = g_strdup(work->back_to_volume ? "volume" : "roots");
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_hidden("confirm", "yes"));
	axis_web_auth_add(form, axis_web_auth_hidden("return", return_value));
	actions = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
	axis_web_auth_add(actions, axis_web_auth_button("Remove this library folder from Axis", "button-danger"));
	cancel = htmx_a_new_with_href(back);
	htmx_node_set_text_content(HTMX_NODE(cancel), "Cancel");
	ADD(actions, cancel);
	ADD(form, actions);
	ADD(content, form);

	return axis_web_page_respond(request, heading, HTMX_NODE(content));
}

static HtmxResponse *
handle_remove_form(
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
	work->back_to_volume = g_strcmp0(htmx_request_get_query_param(request, "return"), "volume") == 0;
	return axis_web_storage_dispatch(user_data, request, look_run, look_done, work, root_work_free);
}

static void
remove_run(
	AxisWebStorage *self,
	gpointer        data
){
	RootWork *work = data;

	if (!axis_storage_get_root(self->storage, work->id, &work->root, &work->error))
		return;
	work->volume_id = work->root->volume_id;
	axis_storage_remove_root(self->storage, work->id, &work->error);
}

static HtmxResponse *
remove_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	RootWork *work = data;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "root removal refused: id=%" G_GINT64_FORMAT " code=%d",
			work->id, work->error->code);
		if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND) &&
		    work->root == NULL)
			return not_found(request);
		if (work->error->domain != AXIS_STORAGE_ERROR)
			return axis_web_internal_error(request, work->error);
		return axis_web_error_respond(request, axis_web_storage_error_status(work->error),
			"The library folder was not removed", work->error->message);
	}

	axis_web_storage_audit(request, "root removed: id=%" G_GINT64_FORMAT " volume=%" G_GINT64_FORMAT,
		work->id, work->volume_id);
	return done_redirect(request, work->back_to_volume, work->volume_id, "root-removed", "roots");
}

static HtmxResponse *
handle_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	RootWork *work;

	if (id == 0)
		return not_found(request);
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Nothing was confirmed",
			"Use the confirmation page to remove a library folder.");

	work = g_new0(RootWork, 1);
	work->id = id;
	work->back_to_volume = g_strcmp0(htmx_request_get_form_value(request, "return"), "volume") == 0;
	return axis_web_storage_dispatch(user_data, request, remove_run, remove_done, work,
		root_work_free);
}

void
axis_web_storage_register_root_pages(
	AxisWebStorage *self,
	AxisWeb        *web
){
	axis_web_get(web, "/storage/roots", handle_roots, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/roots", AXIS_WEB_ROUTE_ADMIN, handle_add,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/storage/roots/:id/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_form, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/roots/:id/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove, self);
}
