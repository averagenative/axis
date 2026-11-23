/*
 * axis-web-storage-pages.c - the volume pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Adding a volume takes two requests.  The first looks at the directory
 * and shows what is there and exactly what adding it will do (write a
 * marker, use the one there, or, on a disk mounted read-only, write nothing
 * and recognise it by its filesystem until the marker can be written later);
 * the second carries what was shown (the
 * plan, the directory's device and inode, the marker) and is refused if
 * the directory is no longer that.  Nothing is written by the first, and
 * nothing that was not shown is written by the second.
 *
 * Messages shown come from the storage layer, which builds them from
 * checked paths and names, or from fixed keys in redirects; nothing a
 * request carries is put in a message unchecked.
 */

#include "web/axis-web-storage-private.h"

#include <string.h>

#include "jobs/axis-job-live.h"
#include "storage/axis-storage-path.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"

#define ADD axis_web_storage_add

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxNode *
fact(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
){
	ADD(list, htmx_dt_new_with_text(label));
	ADD(list, htmx_dd_new_with_text(text != NULL ? text : ""));
	return list;
}

static HtmxNode *
facts_new(void)
{
	HtmxDl *list = htmx_dl_new();

	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	return HTMX_NODE(list);
}

static HtmxNode *
link_to(
	const gchar *href,
	const gchar *text,
	const gchar *css_class
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(link), css_class);
	return HTMX_NODE(link);
}

static HtmxNode *
paragraph_with(HtmxNode *child)
{
	HtmxP *p = htmx_p_new();

	ADD(p, child);
	return HTMX_NODE(p);
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

static gchar *
space_line(
	gboolean known,
	guint64  total,
	guint64  free_bytes,
	guint64  available
){
	g_autofree gchar *t = NULL;
	g_autofree gchar *f = NULL;
	g_autofree gchar *a = NULL;

	if (!known)
		return g_strdup("Not known");

	t = axis_size_format(total);
	f = axis_size_format(free_bytes);
	a = axis_size_format(available);
	return g_strdup_printf("%s free of %s (%s available to Axis)", f, t, a);
}

static HtmxNode *
read_only_notice(AxisWebStorage *self)
{
	if (!axis_storage_is_read_only_mode(self->storage))
		return NULL;

	return axis_web_storage_text("notice notice-ok", "Read-only storage is on: Axis changes nothing "
		"on any disk. The one exception is the small marker file it writes for a disk you add "
		"and confirm: when you add it, or, for a disk you add while it is mounted read-only, the "
		"first time Axis finds it mounted read-write.");
}

static HtmxResponse *
respond(
	HtmxRequest *request,
	const gchar *title,
	HtmxNode    *content,
	guint        status
){
	HtmxResponse *response = axis_web_page_respond(request, title, content);

	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

/* ── The volume list ────────────────────────────────────────────── */

HtmxNode *
axis_web_storage_add_volume_form(
	HtmxRequest *request,
	const gchar *path_value,
	const gchar *name_value,
	const gchar *return_path
){
	HtmxNode *form = axis_web_auth_post_form(request, "/storage/volumes/inspect");
	HtmxNode *back = axis_web_setup_return_field(return_path);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_storage_text_field("volume-path", "Path", "path", path_value,
		"/media/disk1", NULL));
	ADD(form, axis_web_storage_text_field("volume-name", "Name", "name", name_value, NULL,
		"Optional. The directory's own name is used when this is empty."));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_button("Look at this directory", "button-primary"));
	return form;
}

static HtmxResponse *
volumes_page(
	AxisWebStorage *self,
	HtmxRequest    *request,
	guint           status,
	gboolean        error,
	const gchar    *notice,
	const gchar    *path_value,
	const gchar    *name_value,
	const gchar    *back
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *list_card = axis_web_storage_card("Disks");
	HtmxNode *ro = read_only_notice(self);

	htmx_node_set_text_content(HTMX_NODE(title), "Disks and library folders");
	ADD(content, title);
	ADD(content, axis_web_page_intro("The disks Axis may use, and the library folders on them: the folders that "
		"hold your movies, TV and other media, which Axis scans and imports into. Add a disk here before Axis "
		"can find anything on it or download to it."));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(error, notice));
	if (ro != NULL)
		ADD(content, ro);

	ADD(list_card, axis_web_storage_text("help", "Axis "
		"recognises each disk by a small marker file, .axis-volume, at its top, not by where it is "
		"mounted, so a disk that is not mounted shows as offline, never as empty. A disk you add "
		"while it is mounted read-only is recognised by its filesystem's identifier until Axis can "
		"write the marker."));
	ADD(list_card, axis_web_storage_volume_list(volumes, axis_web_request_is_admin(request)));
	ADD(list_card, paragraph_with(link_to("/storage/roots", "Library folders on these disks", NULL)));
	ADD(content, list_card);

	if (axis_web_request_is_admin(request)) {
		HtmxNode *card = axis_web_storage_card("Add a disk");
		HtmxNode *note = axis_web_setup_back_note(back);

		ADD(card, axis_web_storage_text("help", "Give the directory a disk is mounted at. Axis "
			"looks at it first and shows you what is there; nothing is written until you confirm "
			"on the next page."));
		if (note != NULL)
			ADD(card, note);
		ADD(card, axis_web_storage_add_volume_form(request, path_value, name_value, back));
		ADD(content, card);
	}

	axis_web_page_add_topic(request, AXIS_STORAGE_TOPIC);
	return respond(request, "Disks and library folders", HTMX_NODE(content), status);
}

static HtmxResponse *
handle_volumes(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return volumes_page(user_data, request, 200, FALSE,
		axis_web_storage_done_message(htmx_request_get_query_param(request, "done")), NULL, NULL, NULL);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(HtmxNode) list = NULL;
	g_autoptr(HtmxResponse) response = NULL;
	g_autofree gchar *markup = NULL;

	/* Nothing to refresh without htmx: send the browser to the page. */
	if (!htmx_request_is_htmx(request))
		return axis_web_auth_redirect(request, "/storage");

	volumes = axis_storage_list_volumes(self->storage);
	list = axis_web_storage_volume_list(volumes, axis_web_request_is_admin(request));
	axis_web_table_apply_roles(list);
	markup = htmx_element_render(HTMX_ELEMENT(list));
	response = htmx_response_new();
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");

	return (HtmxResponse *)g_steal_pointer(&response);
}

/* ── Looking at a directory ─────────────────────────────────────── */

typedef struct
{
	gchar                *path;
	gchar                *name;	/* as typed, may be empty */
	const gchar          *back;	/* the setup guide's address (static), or NULL */
	AxisVolumeInspection *inspection;
	GError               *error;
} InspectWork;

static void
inspect_work_free(gpointer data)
{
	InspectWork *work = data;

	g_free(work->path);
	g_free(work->name);
	axis_volume_inspection_free(work->inspection);
	g_clear_error(&work->error);
	g_free(work);
}

static void
inspect_work_run(
	AxisWebStorage *self,
	gpointer        data
){
	InspectWork *work = data;

	axis_storage_inspect_volume(self->storage, work->path, &work->inspection, &work->error);
}

/* The name the volume will get: as typed, else the directory's, else a
 * plain one. */
static gchar *
final_name(
	const gchar                *typed,
	const AxisVolumeInspection *inspection
){
	gchar *label = NULL;

	if (typed != NULL && typed[0] != '\0' && axis_storage_label_normalize(typed, &label, NULL))
		return label;
	if (axis_storage_label_normalize(inspection->suggested_name, &label, NULL))
		return label;
	return g_strdup("Volume");
}

static HtmxResponse *
confirm_page(
	AxisWebStorage             *self,
	HtmxRequest                *request,
	const AxisVolumeInspection *inspection,
	const gchar                *typed_name,
	guint                       status,
	const gchar                *notice,
	const gchar                *back
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *name = final_name(typed_name, inspection);
	g_autofree gchar *marker_path = g_strdup_printf("%s/.axis-volume", inspection->path);
	HtmxHeading *title = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What Axis found in this directory");
	HtmxNode *facts = facts_new();
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "Add a disk");
	ADD(content, title);
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(TRUE, notice));

	fact(facts, "Path", inspection->path);
	if (inspection->device != NULL) {
		g_autofree gchar *fs = g_strdup_printf("%s, on device %s",
			inspection->fs_type != NULL ? inspection->fs_type : "unknown", inspection->device);
		g_autofree gchar *space = space_line(inspection->has_space, inspection->total_bytes,
			inspection->free_bytes, inspection->available_bytes);

		fact(facts, "Filesystem", fs);
		if (inspection->fsid != NULL)
			fact(facts, "Filesystem identifier", inspection->fsid);
		if (inspection->fs_uuid != NULL)
			fact(facts, "Filesystem UUID", inspection->fs_uuid);
		fact(facts, "Mount point", inspection->mount_root ? "Yes" : "No");
		fact(facts, "Mounted", inspection->read_only ? "Read-only" : "Read-write");
		fact(facts, "Space", space);
		if (inspection->marker_uuid != NULL) {
			g_autofree gchar *when = axis_web_storage_format_time(inspection->marker_created_us,
				"unknown");
			g_autofree gchar *marker = g_strdup_printf("%s, written %s", inspection->marker_uuid,
				when);

			fact(facts, "Marker", marker);
		} else {
			fact(facts, "Marker", "None");
			fact(facts, "Contents", inspection->empty ? "Empty" : "Has files or folders");
		}
	}
	ADD(card, facts);
	ADD(content, card);

	if (inspection->plan == AXIS_VOLUME_PLAN_REFUSE) {
		ADD(content, axis_web_auth_notice(TRUE, inspection->refusal != NULL ? inspection->refusal :
			"This directory cannot be added as a disk."));
		ADD(content, paragraph_with(back != NULL ? link_to(back, "Back to the setup guide", NULL) :
			link_to("/storage", "Back to Disks and library folders", NULL)));
		return respond(request, "Add a disk", HTMX_NODE(content), status == 200 ? 409 : status);
	}

	for (i = 0; i < inspection->warnings->len; i++)
		ADD(content, warning(g_ptr_array_index(inspection->warnings, i)));

	{
		HtmxNode *confirm = axis_web_storage_card("Confirm adding this disk");
		HtmxNode *form = axis_web_auth_post_form(request, "/storage/volumes");
		HtmxNode *ro = read_only_notice(self);
		g_autofree gchar *dev = g_strdup_printf("%" G_GUINT64_FORMAT, inspection->dev);
		g_autofree gchar *ino = g_strdup_printf("%" G_GUINT64_FORMAT, inspection->ino);
		g_autofree gchar *size = g_strdup_printf("%" G_GUINT64_FORMAT, inspection->total_bytes);
		HtmxNode *actions;

		if (inspection->plan == AXIS_VOLUME_PLAN_WRITE_MARKER) {
			g_autofree gchar *text = g_strdup_printf("Axis will write one new file, %s, holding a "
				"random identifier, the date and a short note, and nothing else. It replaces "
				"nothing and touches no other file. This is the only change Axis makes to a disk "
				"while storage is read-only.", marker_path);

			ADD(confirm, axis_web_storage_text(NULL, text));
		} else if (inspection->plan == AXIS_VOLUME_PLAN_IDENTIFY) {
			g_autofree gchar *now = g_strdup_printf("The disk is mounted read-only and has no marker, so "
				"Axis writes nothing now. Until it can write its marker, Axis recognises this disk by what "
				"it can read: the filesystem's identifier (%s)%s, its type, and this directory. If "
				"any of these is different at a check, or the directory is empty, the disk shows as "
				"offline instead of being guessed.", inspection->fsid != NULL ? inspection->fsid : "",
				inspection->fs_uuid != NULL ? " and UUID" : "");
			g_autofree gchar *later = g_strdup_printf("The first time Axis finds the disk mounted "
				"read-write, it writes one new file, %s, holding this disk's identifier, the date and a "
				"short note, and from then on recognises the disk by it. That is the only change Axis makes "
				"to a disk while storage is read-only. Until then you can scan and read the disk, but "
				"nothing is written to it and no conversion worker is given work on it.", marker_path);

			ADD(confirm, axis_web_storage_text(NULL, now));
			ADD(confirm, axis_web_storage_text(NULL, later));
		} else {
			ADD(confirm, axis_web_storage_text(NULL, "This directory already carries an Axis "
				"marker. Axis will use it as it is and write nothing."));
		}
		if (ro != NULL)
			ADD(confirm, ro);

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		axis_web_auth_add(form, axis_web_auth_hidden("path", inspection->path));
		axis_web_auth_add(form, axis_web_auth_hidden("plan", axis_volume_plan_to_string(inspection->plan)));
		axis_web_auth_add(form, axis_web_auth_hidden("dev", dev));
		axis_web_auth_add(form, axis_web_auth_hidden("ino", ino));
		axis_web_auth_add(form, axis_web_auth_hidden("fsid", inspection->fsid != NULL ? inspection->fsid : ""));
		axis_web_auth_add(form, axis_web_auth_hidden("size", size));
		axis_web_auth_add(form, axis_web_auth_hidden("confirm", "yes"));
		if (inspection->marker_uuid != NULL)
			axis_web_auth_add(form, axis_web_auth_hidden("marker", inspection->marker_uuid));
		if (back != NULL) {
			axis_web_auth_add(form, axis_web_setup_return_field(back));
			ADD(confirm, axis_web_setup_back_note(back));
		}
		ADD(form, axis_web_storage_text_field("confirm-name", "Name", "name", name, NULL, NULL));
		if (!inspection->mount_root)
			ADD(form, axis_web_storage_checkbox("confirm-not-mount-point", "This directory is not a "
				"mount point; add it as a disk anyway.", "not_mount_point_ok", TRUE));

		actions = HTMX_NODE(htmx_div_new());
		htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
		axis_web_auth_add(actions, axis_web_auth_button(inspection->plan ==
			AXIS_VOLUME_PLAN_WRITE_MARKER ? "Write the marker and add the disk" : "Add the disk",
			"button-primary"));
		ADD(actions, link_to(back != NULL ? back : "/storage", "Cancel", NULL));
		ADD(form, actions);
		ADD(confirm, form);
		ADD(content, confirm);
	}

	return respond(request, "Add a disk", HTMX_NODE(content), status);
}

static HtmxResponse *
inspect_work_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	InspectWork *work = data;

	if (work->error != NULL) {
		if (work->error->domain == AXIS_STORAGE_ERROR)
			return volumes_page(self, request, axis_web_storage_error_status(work->error), TRUE,
				work->error->message, work->path, work->name, work->back);
		return axis_web_internal_error(request, work->error);
	}

	return confirm_page(self, request, work->inspection, work->name, 200, NULL, work->back);
}

static HtmxResponse *
handle_inspect(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	const gchar *path = htmx_request_get_form_value(request, "path");
	const gchar *name = htmx_request_get_form_value(request, "name");
	const gchar *back = axis_web_setup_request_return(request);
	const gchar *problem = NULL;
	InspectWork *work;

	if (!axis_storage_path_normalize(path, TRUE, NULL, &problem))
		return volumes_page(self, request, 400, TRUE, problem, path, name, back);
	if (name != NULL && name[0] != '\0' && !axis_storage_label_normalize(name, NULL, &problem))
		return volumes_page(self, request, 400, TRUE, problem, path, name, back);

	work = g_new0(InspectWork, 1);
	work->path = g_strdup(path);
	work->name = g_strdup(name != NULL ? name : "");
	work->back = back;

	return axis_web_storage_dispatch(self, request, inspect_work_run, inspect_work_done, work,
		inspect_work_free);
}

/* ── Adding ─────────────────────────────────────────────────────── */

typedef struct
{
	gchar          *path;
	gchar          *name;
	gchar          *marker;
	gchar          *fsid;
	AxisVolumePlan  plan;
	guint64         dev;
	guint64         ino;
	guint64         size;
	gboolean        not_mount_point_ok;
	const gchar    *back;	/* the setup guide's address (static), or NULL */
	gint64          id;
	GError         *error;
} AddWork;

static void
add_work_free(gpointer data)
{
	AddWork *work = data;

	g_free(work->path);
	g_free(work->name);
	g_free(work->marker);
	g_free(work->fsid);
	g_clear_error(&work->error);
	g_free(work);
}

/* A filesystem id as the confirmation carries it: up to 16 hex digits. */
static gboolean
fsid_is_valid(const gchar *text)
{
	const gchar *p;

	if (text == NULL || strlen(text) > 16)
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isxdigit(*p))
			return FALSE;
	}

	return TRUE;
}

static void
add_work_run(
	AxisWebStorage *self,
	gpointer        data
){
	AddWork *work = data;
	AxisVolumeAddRequest add;

	add.path = work->path;
	add.name = work->name;
	add.plan = work->plan;
	add.marker_uuid = work->marker;
	add.dev = work->dev;
	add.ino = work->ino;
	add.fsid = work->fsid;
	add.total_bytes = work->size;
	add.not_mount_point_ok = work->not_mount_point_ok;
	axis_storage_add_volume(self->storage, &add, &work->id, &work->error);
}

static HtmxResponse *
add_work_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	AddWork *work = data;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autofree gchar *location = NULL;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "volume add refused: plan=%s code=%d",
			axis_volume_plan_to_string(work->plan), work->error->code);
		if (work->error->domain == AXIS_STORAGE_ERROR)
			return volumes_page(self, request, axis_web_storage_error_status(work->error), TRUE,
				work->error->message, work->path, work->name, work->back);
		return axis_web_internal_error(request, work->error);
	}

	volume = axis_storage_get_volume(self->storage, work->id);
	axis_web_storage_audit(request, "volume added: id=%" G_GINT64_FORMAT " name=%s path=%s plan=%s",
		work->id, volume != NULL ? volume->name : "", volume != NULL ? volume->path : "",
		axis_volume_plan_to_string(work->plan));
	location = work->back != NULL ? axis_web_setup_return_location(work->back, "added") :
		g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT "?done=added", work->id);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_add(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	const gchar *path = htmx_request_get_form_value(request, "path");
	const gchar *name = htmx_request_get_form_value(request, "name");
	const gchar *marker = htmx_request_get_form_value(request, "marker");
	const gchar *fsid = htmx_request_get_form_value(request, "fsid");
	AxisVolumePlan plan = AXIS_VOLUME_PLAN_REFUSE;
	guint64 dev = 0;
	guint64 ino = 0;
	guint64 size = 0;
	AddWork *work;

	if (!axis_web_storage_form_yes(request, "confirm") ||
	    !axis_volume_plan_from_string(htmx_request_get_form_value(request, "plan"), &plan) ||
	    plan == AXIS_VOLUME_PLAN_REFUSE ||
	    !axis_web_storage_form_u64(request, "dev", &dev) ||
	    !axis_web_storage_form_u64(request, "ino", &ino) ||
	    !axis_web_storage_form_u64(request, "size", &size) ||
	    !fsid_is_valid(fsid))
		return volumes_page(self, request, 400, TRUE, "Nothing was confirmed. Look at the "
			"directory first, then confirm on the page that shows it.", NULL, NULL,
			axis_web_setup_request_return(request));

	work = g_new0(AddWork, 1);
	work->path = g_strdup(path);
	work->name = g_strdup(name);
	work->marker = g_strdup(marker);
	work->fsid = g_strdup(fsid);
	work->plan = plan;
	work->dev = dev;
	work->ino = ino;
	work->size = size;
	work->not_mount_point_ok = axis_web_storage_form_yes(request, "not_mount_point_ok");
	work->back = axis_web_setup_request_return(request);

	return axis_web_storage_dispatch(self, request, add_work_run, add_work_done, work,
		add_work_free);
}

/* ── One volume ─────────────────────────────────────────────────── */

static gchar *
root_directory(
	const AxisVolumeInfo *volume,
	const AxisRootInfo   *root
){
	if (root->rel_path[0] == '\0')
		return g_strdup_printf("%s (the top of the disk)", volume->path);
	return g_strdup_printf("%s/%s", volume->path, root->rel_path);
}

static HtmxNode *
roots_table(
	const AxisVolumeInfo *volume,
	GPtrArray            *roots,
	gboolean              admin
){
	static const AxisWebColumn columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Directory", AXIS_WEB_COL_NAME },
		{ NULL, 0 }
	};
	static const AxisWebColumn admin_columns[] = {
		{ "Library folder", AXIS_WEB_COL_NAME },
		{ "Holds", AXIS_WEB_COL_TEXT },
		{ "Directory", AXIS_WEB_COL_NAME },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *table = admin ? axis_web_table_new("volume-roots-admin", admin_columns) :
		axis_web_table_new("volume-roots", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name_cell = htmx_td_new();
		HtmxTd *where = htmx_td_new();
		g_autofree gchar *directory = root_directory(volume, root);
		g_autofree gchar *root_href = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, root->id);

		ADD(name_cell, link_to(root_href, root->name, NULL));
		ADD(row, name_cell);
		ADD(row, htmx_td_new_with_text(axis_media_type_label(root->media_type)));
		ADD(where, htmx_code_new_with_text(directory));
		ADD(row, where);
		if (admin) {
			HtmxTd *action = htmx_td_new();
			g_autofree gchar *href = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT
				"/remove?return=volume", root->id);
			g_autofree gchar *aria = g_strdup_printf("Remove library folder %s", root->name);
			HtmxNode *remove = link_to(href, "Remove", NULL);

			htmx_element_set_attribute(HTMX_ELEMENT(remove), "aria-label", aria);
			ADD(action, remove);
			ADD(row, action);
		}
		ADD(body, row);
	}
	ADD(table, body);

	return table;
}

static HtmxNode *
add_root_form(
	HtmxRequest          *request,
	const AxisVolumeInfo *volume
){
	HtmxNode *form = axis_web_auth_post_form(request, "/storage/roots");
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, volume->id);
	g_autofree gchar *help = g_strdup_printf("Relative to the top of the disk, %s, such as Movies. "
		"Empty means the whole disk.", volume->path);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_hidden("volume", id));
	axis_web_auth_add(form, axis_web_auth_hidden("return", "volume"));
	ADD(form, axis_web_storage_text_field("root-path", "Directory", "path", NULL, "Movies", help));
	ADD(form, axis_web_storage_media_select("root-type", "type"));
	ADD(form, axis_web_storage_text_field("root-name", "Name", "name", NULL, NULL,
		"Optional. The directory's own name is used when this is empty."));
	axis_web_auth_add(form, axis_web_auth_button("Add the library folder", "button-primary"));

	return form;
}

static HtmxNode *
post_button(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *css_class
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	axis_web_auth_add(form, axis_web_auth_button(label, css_class));
	return form;
}

/* The key of a volume's scan job. */
static gchar *
scan_key(gint64 id)
{
	return g_strdup_printf("library.scan:volume:%" G_GINT64_FORMAT, id);
}

/* The line of the volume's scan in hand (read on the page's worker thread,
 * @live nullable), or a hidden place for it. */
static HtmxNode *
scan_line(
	const GPtrArray *live,
	gint64           id
){
	g_autofree gchar *key = scan_key(id);
	const AxisJobLive *job = axis_job_live_find(live, key);
	g_autofree gchar *state = NULL;
	g_autofree gchar *text = NULL;

	if (job == NULL)
		return axis_web_activity_line("activity-volume", AXIS_WEB_ACTIVITY_RUNNING, NULL);
	state = axis_web_activity_state(job, g_get_real_time());
	text = g_strdup_printf("A scan of this disk is %s.", state);
	return axis_web_activity_line("activity-volume", job->running ? AXIS_WEB_ACTIVITY_RUNNING :
		AXIS_WEB_ACTIVITY_WAITING, text);
}

static HtmxResponse *
volume_page(
	AxisWebStorage  *self,
	HtmxRequest     *request,
	gint64           id,
	GPtrArray       *roots,
	const GPtrArray *live,
	guint            status,
	gboolean         error,
	const gchar     *notice
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, id);
	g_autoptr(HtmxDiv) content = NULL;
	gboolean admin = axis_web_request_is_admin(request);
	gboolean usable;
	HtmxHeading *title;
	HtmxNode *state_card;
	HtmxNode *facts;
	HtmxNode *roots_card;
	AxisWebNotice placed;
	g_autofree gchar *base = NULL;

	if (volume == NULL)
		return not_found(request);

	usable = axis_volume_state_is_usable(volume->state);
	base = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, id);
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), volume->name);
	ADD(content, paragraph_with(link_to("/storage", "Back to Disks and library folders", NULL)));
	ADD(content, title);
	axis_web_notice_begin(&placed, request, error, g_strdup(notice), HTMX_NODE(content));

	state_card = axis_web_storage_card("State of this disk");
	ADD(state_card, axis_web_storage_state_block(volume));
	ADD(state_card, axis_web_storage_space_block(volume));
	ADD(state_card, axis_web_storage_checked_block(volume));
	/* Its scan in hand, where it stands (add-activity-feedback). */
	ADD(state_card, scan_line(live, id));
	facts = facts_new();
	fact(facts, "Path", volume->path);
	fact(facts, "Identifier", volume->uuid);
	{
		g_autofree gchar *fs = g_strdup_printf("%s, on device %s",
			volume->fs_type[0] != '\0' ? volume->fs_type : "unknown",
			volume->device[0] != '\0' ? volume->device : "unknown");
		g_autofree gchar *since = axis_web_storage_format_time(volume->state_changed_us, "unknown");
		g_autofree gchar *written = axis_web_storage_format_time(volume->marker_created_us, "unknown");
		g_autofree gchar *added = axis_web_storage_format_time(volume->created_us, "unknown");

		fact(facts, "Filesystem", fs);
		fact(facts, "Mount point", volume->mount_root ? "Yes: the disk shows as offline whenever its "
			"directory is not a mount point" : "No");
		fact(facts, "State since", since);
		if (volume->has_marker) {
			fact(facts, "Recognised by", "Its marker, .axis-volume, at its top");
			fact(facts, "Marker written", written);
		} else {
			g_autofree gchar *by = axis_volume_fs_identity_describe(&volume->fs_identity);
			g_autofree gchar *text = g_strdup_printf("Its filesystem identifier: %s. It has no marker yet.", by);

			fact(facts, "Recognised by", text);
			fact(facts, "Marker written", "Not yet");
		}
		fact(facts, "Added", added);
	}
	ADD(state_card, facts);
	if (!volume->has_marker) {
		ADD(state_card, axis_web_storage_text("help", "This disk was added while it was mounted read-only, "
			"so Axis could not write its marker. Until it can, Axis recognises the disk by its filesystem's "
			"identifier (or its UUID) and type and by this directory. If any of these changes, or the directory is "
			"empty, the disk shows as offline instead of being guessed. Axis can scan and read it, but "
			"nothing is written to it and no conversion worker is given work on it, because a worker "
			"recognises a disk only by its marker."));
		ADD(state_card, axis_web_storage_text("help", "Axis writes the marker the first time it finds the "
			"disk mounted read-write: at its next check, which runs every minute, or when you select Check "
			"this disk now. From then on the marker identifies the disk."));
		if (volume->marker_problem != NULL) {
			g_autofree gchar *problem = g_strdup_printf("The last attempt to write the marker failed: %s. "
				"Axis tries again at every check.", volume->marker_problem);

			ADD(state_card, warning(problem));
		}
	}
	ADD(content, state_card);

	roots_card = axis_web_storage_card("Library folders on this disk");
	htmx_element_set_id(HTMX_ELEMENT(roots_card), "volume-roots");
	axis_web_notice_put(&placed, roots_card, "volume-roots");
	if (roots == NULL || roots->len == 0)
		ADD(roots_card, axis_web_storage_text("help", admin ?
			"No library folders on this disk yet. Add one with the form below." :
			"No library folders on this disk yet. An administrator adds them here."));
	else
		ADD(roots_card, roots_table(volume, roots, admin));
	if (admin && usable)
		ADD(roots_card, add_root_form(request, volume));
	else if (admin)
		ADD(roots_card, axis_web_storage_text("help", "Library folders can be added and removed only while the "
			"disk is online. Mount the disk, then select Check this disk now."));
	ADD(content, roots_card);

	if (admin) {
		HtmxNode *actions = axis_web_storage_card("Actions for this disk");
		g_autofree gchar *check = g_strconcat(base, "/check", NULL);
		g_autofree gchar *move = g_strconcat(base, "/path", NULL);
		g_autofree gchar *remove = g_strconcat(base, "/remove", NULL);
		g_autofree gchar *marker = g_strconcat(base, "/marker", NULL);
		HtmxNode *row = HTMX_NODE(htmx_div_new());
		HtmxNode *form;

		htmx_element_set_id(HTMX_ELEMENT(actions), "volume-actions");
		axis_web_notice_put(&placed, actions, "volume-actions");
		htmx_element_add_class(HTMX_ELEMENT(row), "actions");
		ADD(row, post_button(request, check, "Check this disk now", NULL));
		ADD(row, link_to(remove, "Remove this disk", "link-danger"));
		if (volume->state == AXIS_VOLUME_MISMATCH && volume->reason == AXIS_VOLUME_REASON_NO_MARKER)
			ADD(row, link_to(marker, "Restore this disk's marker", NULL));
		ADD(actions, row);

		ADD(actions, axis_web_storage_text("help", volume->has_marker ? "When this disk is mounted "
			"somewhere else, give its new path. Axis checks that the marker there is this disk's; it "
			"writes nothing." : "When this disk is mounted somewhere else, give its new path. Axis checks "
			"that the filesystem and directory there are the ones it recognises this disk by; it "
			"writes nothing."));
		ADD(actions, axis_web_storage_text("help", "When this disk and another disk were swapped between "
			"their mount points, give the other disk's path: when the other disk is at this disk's "
			"path, Axis changes both paths at once."));
		form = axis_web_auth_post_form(request, move);
		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		ADD(form, axis_web_storage_text_field("volume-new-path", "Path", "path", NULL,
			volume->path, NULL));
		ADD(form, axis_web_storage_checkbox("volume-new-not-mount-point", "The new directory is not "
			"a mount point, and that is intended.", "not_mount_point_ok", FALSE));
		axis_web_auth_add(form, axis_web_auth_button("Change the path", NULL));
		ADD(actions, form);
		ADD(content, actions);
	}

	axis_web_notice_end(&placed);
	axis_web_page_add_topic(request, AXIS_STORAGE_TOPIC);
	{
		g_autofree gchar *page_title = g_strdup_printf("Disk %s", volume->name);

		return respond(request, page_title, HTMX_NODE(content), status);
	}
}

typedef struct
{
	gint64     id;
	gchar     *path;	/* relocate */
	gboolean   flag;	/* relocate: not a mount point; remove: offline is fine */
	GPtrArray *roots;
	GPtrArray *live;	/* (nullable) AxisJobLive: its scan in hand */
	GError    *error;
	GError    *roots_error;
} VolumeWork;

static void
volume_work_free(gpointer data)
{
	VolumeWork *work = data;

	g_free(work->path);
	if (work->roots != NULL)
		g_ptr_array_unref(work->roots);
	g_clear_pointer(&work->live, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_clear_error(&work->roots_error);
	g_free(work);
}

static void
volume_view_run(
	AxisWebStorage *self,
	gpointer        data
){
	VolumeWork *work = data;
	g_autofree gchar *key = scan_key(work->id);
	const gchar *keys[2];

	keys[0] = key;
	keys[1] = NULL;
	if (axis_storage_list_roots(self->storage, work->id, &work->roots, &work->error))
		axis_job_live_read_pool(axis_storage_get_pool(self->storage), keys, &work->live, NULL);
}

static HtmxResponse *
volume_view_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	VolumeWork *work = data;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	return volume_page(self, request, work->id, work->roots, work->live, 200, FALSE,
		axis_web_storage_done_message(htmx_request_get_query_param(request, "done")));
}

static HtmxResponse *
handle_volume(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	gint64 id = axis_web_storage_param_id(params);
	g_autoptr(AxisVolumeInfo) volume = NULL;
	VolumeWork *work;

	if (id == 0 || (volume = axis_storage_get_volume(self->storage, id)) == NULL)
		return not_found(request);

	work = g_new0(VolumeWork, 1);
	work->id = id;
	return axis_web_storage_dispatch(self, request, volume_view_run, volume_view_done, work,
		volume_work_free);
}

/* ── Checking now ───────────────────────────────────────────────── */

static void
check_run(
	AxisWebStorage *self,
	gpointer        data
){
	VolumeWork *work = data;

	axis_storage_check_volume(self->storage, work->id, NULL, &work->error);
}

static HtmxResponse *
check_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	VolumeWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	location = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "checked", "volume-actions");
}

static HtmxResponse *
handle_check(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	gint64 id = axis_web_storage_param_id(params);
	VolumeWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(VolumeWork, 1);
	work->id = id;
	return axis_web_storage_dispatch(self, request, check_run, check_done, work, volume_work_free);
}

/* ── Re-pointing ────────────────────────────────────────────────── */

static void
relocate_run(
	AxisWebStorage *self,
	gpointer        data
){
	VolumeWork *work = data;

	if (!axis_storage_relocate_volume(self->storage, work->id, work->path, work->flag, &work->error))
		axis_storage_list_roots(self->storage, work->id, &work->roots, &work->roots_error);
}

static HtmxResponse *
relocate_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	VolumeWork *work = data;
	g_autofree gchar *location = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "volume path change refused: id=%" G_GINT64_FORMAT
			" code=%d", work->id, work->error->code);
		if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
			return not_found(request);
		if (work->error->domain != AXIS_STORAGE_ERROR)
			return axis_web_internal_error(request, work->error);
		return volume_page(self, request, work->id, work->roots, NULL,
			axis_web_storage_error_status(work->error), TRUE, work->error->message);
	}

	volume = axis_storage_get_volume(self->storage, work->id);
	axis_web_storage_audit(request, "volume path changed: id=%" G_GINT64_FORMAT " path=%s",
		work->id, volume != NULL ? volume->path : "");
	location = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "moved", "volume-actions");
}

static HtmxResponse *
handle_relocate(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	gint64 id = axis_web_storage_param_id(params);
	VolumeWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(VolumeWork, 1);
	work->id = id;
	work->path = g_strdup(htmx_request_get_form_value(request, "path"));
	work->flag = axis_web_storage_form_yes(request, "not_mount_point_ok");
	return axis_web_storage_dispatch(self, request, relocate_run, relocate_done, work,
		volume_work_free);
}

/* ── Removing ───────────────────────────────────────────────────── */

static HtmxResponse *
remove_page(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gint64          id,
	guint           status,
	const gchar    *notice
){
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, id);
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *heading = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *back = NULL;
	HtmxHeading *title;
	HtmxNode *form;
	HtmxNode *actions;

	if (volume == NULL)
		return not_found(request);

	content = htmx_div_new();
	title = htmx_h1_new();
	heading = g_strdup_printf("Remove disk %s", volume->name);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(content, title);
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(TRUE, notice));

	text = g_strdup_printf("Axis will forget this disk (%s), its %u library %s, and everything "
		"it knows about the files in them. Nothing on the disk is deleted or changed.%s", volume->path,
		volume->n_roots, volume->n_roots == 1 ? "folder" : "folders",
		volume->has_marker ? " Its marker stays, so adding this directory again later recognises "
		"the same disk." : "");
	ADD(content, axis_web_storage_text(NULL, text));

	action = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT "/remove", id);
	back = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, id);
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_hidden("confirm", "yes"));
	if (!axis_volume_state_is_usable(volume->state)) {
		g_autofree gchar *why = g_strdup_printf("This disk is not online now: %s If it is "
			"only unplugged or not mounted, bring it back instead. Remove it now only if the disk "
			"is gone for good.", axis_volume_reason_describe(volume->reason));

		ADD(content, warning(why));
		ADD(form, axis_web_storage_checkbox("remove-offline", "The disk is gone for good: forget "
			"it while it is not online.", "offline_ok", TRUE));
	}
	actions = HTMX_NODE(htmx_div_new());
	htmx_element_add_class(HTMX_ELEMENT(actions), "actions");
	axis_web_auth_add(actions, axis_web_auth_button("Remove this disk from Axis", "button-danger"));
	ADD(actions, link_to(back, "Cancel", NULL));
	ADD(form, actions);
	ADD(content, form);

	return respond(request, heading, HTMX_NODE(content), status);
}

static HtmxResponse *
handle_remove_form(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);

	if (id == 0)
		return not_found(request);
	return remove_page(user_data, request, id, 200, NULL);
}

static void
remove_run(
	AxisWebStorage *self,
	gpointer        data
){
	VolumeWork *work = data;

	axis_storage_remove_volume(self->storage, work->id, work->flag, &work->error);
}

static HtmxResponse *
remove_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	VolumeWork *work = data;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "volume removal refused: id=%" G_GINT64_FORMAT " code=%d",
			work->id, work->error->code);
		if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
			return not_found(request);
		if (work->error->domain != AXIS_STORAGE_ERROR)
			return axis_web_internal_error(request, work->error);
		return remove_page(self, request, work->id, axis_web_storage_error_status(work->error),
			work->error->message);
	}

	axis_web_storage_audit(request, "volume removed: id=%" G_GINT64_FORMAT " path=%s offline_ok=%s",
		work->id, work->path, work->flag ? "yes" : "no");
	return axis_web_auth_redirect(request, "/storage?done=removed");
}

static HtmxResponse *
handle_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebStorage *self = user_data;
	gint64 id = axis_web_storage_param_id(params);
	g_autoptr(AxisVolumeInfo) volume = NULL;
	VolumeWork *work;

	if (id == 0 || (volume = axis_storage_get_volume(self->storage, id)) == NULL)
		return not_found(request);
	if (!axis_web_storage_form_yes(request, "confirm"))
		return remove_page(self, request, id, 400, "Nothing was confirmed.");

	work = g_new0(VolumeWork, 1);
	work->id = id;
	work->path = g_strdup(volume->path);
	work->flag = axis_web_storage_form_yes(request, "offline_ok");
	return axis_web_storage_dispatch(self, request, remove_run, remove_done, work,
		volume_work_free);
}

/* ── Restoring the marker ───────────────────────────────────────── */

typedef struct
{
	gint64                       id;
	guint64                      dev;
	guint64                      ino;
	gchar                       *fsid;
	gboolean                     other_filesystem_ok;
	AxisMarkerRestoreInspection *inspection;
	GError                      *error;
} MarkerWork;

static void
marker_work_free(gpointer data)
{
	MarkerWork *work = data;

	g_free(work->fsid);
	axis_marker_restore_inspection_free(work->inspection);
	g_clear_error(&work->error);
	g_free(work);
}

static void
marker_look_run(
	AxisWebStorage *self,
	gpointer        data
){
	MarkerWork *work = data;

	axis_storage_inspect_marker_restore(self->storage, work->id, &work->inspection, &work->error);
}

static HtmxResponse *
marker_look_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	MarkerWork *work = data;
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, work->id);
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *heading = NULL;
	AxisMarkerRestoreInspection *in = work->inspection;
	HtmxHeading *title;
	HtmxNode *card;
	guint i;

	if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND) ||
	    volume == NULL)
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	content = htmx_div_new();
	title = htmx_h1_new();
	heading = g_strdup_printf("Restore the marker of %s", volume->name);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	ADD(content, title);
	ADD(content, axis_web_storage_text(NULL, "The marker is the small file .axis-volume at the top of "
		"the disk, by which Axis recognises it. Writing it again changes nothing else."));

	card = axis_web_storage_card("What is at this disk's path now");
	ADD(card, axis_web_storage_text(NULL, axis_volume_reason_describe(in->reason)));
	if (in->filesystem_text != NULL)
		ADD(card, axis_web_storage_text(in->filesystem == AXIS_MARKER_RESTORE_FS_SAME ? NULL : "notice",
			in->filesystem_text));
	if (in->roots->len > 0) {
		static const AxisWebColumn columns[] = {
			{ "Library folder", AXIS_WEB_COL_NAME },
			{ "Directory", AXIS_WEB_COL_NAME },
			{ "There now", AXIS_WEB_COL_STATE },
			{ NULL, 0 }
		};
		HtmxNode *table = axis_web_table_new("marker-roots", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < in->roots->len; i++) {
			const AxisRootInfo *root = g_ptr_array_index(in->roots, i);
			HtmxTr *row = htmx_tr_new();
			g_autofree gchar *directory = root_directory(volume, root);

			ADD(row, htmx_td_new_with_text(root->name));
			ADD(row, htmx_td_new_with_text(directory));
			ADD(row, htmx_td_new_with_text(g_array_index(in->root_present, gboolean, i) ?
				"Present" : "Missing"));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, axis_web_storage_text("help", in->filesystem == AXIS_MARKER_RESTORE_FS_SAME ?
			"The library folders of this disk, and whether each is there now. If they are, this is the disk "
			"and only its marker is missing." : "The library folders of this disk, and whether each is "
			"there now. Folders of these names can be on another disk too: they alone do not show that this "
			"is the same disk."));
		ADD(card, table);
	}
	ADD(content, card);

	if (!volume->has_marker) {
		ADD(content, axis_web_auth_notice(TRUE, "This disk was added without a marker, so there is none "
			"to restore. Axis writes its marker itself the first time it finds the disk mounted read-write "
			"and recognised by its filesystem's identifier."));
	} else if (!in->possible) {
		ADD(content, axis_web_auth_notice(TRUE, "Axis cannot restore the marker here: it "
			"writes a marker only into a directory that has files and no marker. The line above says "
			"what is at the path now."));
	} else if (axis_storage_is_read_only_mode(self->storage)) {
		ADD(content, axis_web_auth_notice(TRUE, "Read-only storage is on, so Axis does not restore "
			"markers now. Turn off Read-only storage in Settings first, or remove this disk "
			"and add it again."));
	} else {
		g_autofree gchar *action = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT "/marker",
			work->id);
		g_autofree gchar *dev = g_strdup_printf("%" G_GUINT64_FORMAT, in->dev);
		g_autofree gchar *ino = g_strdup_printf("%" G_GUINT64_FORMAT, in->ino);
		g_autofree gchar *text = g_strdup_printf("Axis will write %s/.axis-volume with this "
			"disk's own identifier. It replaces nothing and touches no other file.", volume->path);
		HtmxNode *form = axis_web_auth_post_form(request, action);

		ADD(content, axis_web_storage_text(NULL, text));
		axis_web_auth_add(form, axis_web_auth_hidden("dev", dev));
		axis_web_auth_add(form, axis_web_auth_hidden("ino", ino));
		axis_web_auth_add(form, axis_web_auth_hidden("fsid", in->fsid));
		axis_web_auth_add(form, axis_web_auth_hidden("confirm", "yes"));
		/* Said plainly above; written only once that is confirmed. */
		if (in->filesystem != AXIS_MARKER_RESTORE_FS_SAME)
			ADD(form, axis_web_storage_checkbox("marker-other-filesystem", "I am sure this is the same "
				"disk, although Axis cannot show that its filesystem is the one it was last seen on.",
				"other_filesystem_ok", FALSE));
		axis_web_auth_add(form, axis_web_auth_button("Write the marker again", "button-primary"));
		ADD(content, form);
	}
	{
		g_autofree gchar *back = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, work->id);

		g_autofree gchar *back_text = g_strdup_printf("Back to disk %s", volume->name);

		ADD(content, paragraph_with(link_to(back, back_text, NULL)));
	}

	return respond(request, heading, HTMX_NODE(content), 200);
}

static HtmxResponse *
handle_marker_form(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	MarkerWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(MarkerWork, 1);
	work->id = id;
	return axis_web_storage_dispatch(user_data, request, marker_look_run, marker_look_done, work,
		marker_work_free);
}

static void
marker_write_run(
	AxisWebStorage *self,
	gpointer        data
){
	MarkerWork *work = data;

	axis_storage_restore_marker(self->storage, work->id, work->dev, work->ino, work->fsid,
		work->other_filesystem_ok, &work->error);
}

static HtmxResponse *
marker_write_done(
	AxisWebStorage *self,
	HtmxRequest    *request,
	gpointer        data
){
	MarkerWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL) {
		axis_web_storage_audit(request, "marker restore refused: id=%" G_GINT64_FORMAT " code=%d",
			work->id, work->error->code);
		if (g_error_matches(work->error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND))
			return not_found(request);
		if (work->error->domain != AXIS_STORAGE_ERROR)
			return axis_web_internal_error(request, work->error);
		return axis_web_error_respond(request, axis_web_storage_error_status(work->error),
			"The marker was not written", work->error->message);
	}

	axis_web_storage_audit(request, "marker restored: id=%" G_GINT64_FORMAT, work->id);
	location = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT "?done=restored", work->id);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_marker(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *fsid = htmx_request_get_form_value(request, "fsid");
	MarkerWork *work;
	guint64 dev = 0;
	guint64 ino = 0;

	if (id == 0)
		return not_found(request);
	if (!axis_web_storage_form_yes(request, "confirm") ||
	    !axis_web_storage_form_u64(request, "dev", &dev) ||
	    !axis_web_storage_form_u64(request, "ino", &ino) ||
	    !fsid_is_valid(fsid))
		return axis_web_error_respond(request, 400, "Nothing was confirmed",
			"Look at the disk's directory first, then confirm on the page that shows it.");

	work = g_new0(MarkerWork, 1);
	work->id = id;
	work->dev = dev;
	work->ino = ino;
	work->fsid = g_strdup(fsid);
	work->other_filesystem_ok = axis_web_storage_form_yes(request, "other_filesystem_ok");
	return axis_web_storage_dispatch(user_data, request, marker_write_run, marker_write_done, work,
		marker_work_free);
}

void
axis_web_storage_register_volume_pages(
	AxisWebStorage *self,
	AxisWeb        *web
){
	axis_web_get(web, "/storage", handle_volumes, self);
	axis_web_get(web, "/storage/list", handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes/inspect", AXIS_WEB_ROUTE_ADMIN,
		handle_inspect, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes", AXIS_WEB_ROUTE_ADMIN,
		handle_add, self);
	axis_web_get(web, "/storage/volumes/:id", handle_volume, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes/:id/check", AXIS_WEB_ROUTE_ADMIN,
		handle_check, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes/:id/path", AXIS_WEB_ROUTE_ADMIN,
		handle_relocate, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/storage/volumes/:id/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_form, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes/:id/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/storage/volumes/:id/marker", AXIS_WEB_ROUTE_ADMIN,
		handle_marker_form, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/storage/volumes/:id/marker", AXIS_WEB_ROUTE_ADMIN,
		handle_marker, self);
}
