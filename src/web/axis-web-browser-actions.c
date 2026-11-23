/*
 * axis-web-browser-actions.c - the Library pages' actions: scan a root,
 * probe the files under a folder again
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An action only creates jobs: it marks what is to be done in the index and
 * puts the work on the queue, whose de-duplication keys make a second
 * request for the same thing the same job.  No file is opened or touched
 * here.  The routes are administrators' (anyone else gets 403 before
 * these run) and every POST carries the CSRF token (checked by the shell).
 * Nothing the request carries but ids is used, and nothing but numbers is
 * put in the redirect.
 */

#include "web/axis-web-browser-private.h"

#include "db/axis-db-error.h"
#include "web/axis-web-place.h"

typedef struct
{
	gint64        id;		/* the root, or the folder */
	gint64        folder;	/* where to return to (a scan from a folder's page) */
	gint64        job_id;
	guint         requested;
	gboolean      capped;
	gboolean      offline;
	AxisDirRecord *dir;
	GError       *error;
} ActionWork;

static void
action_work_free(gpointer data)
{
	ActionWork *work = data;

	axis_dir_record_free(work->dir);
	g_clear_error(&work->error);
	g_free(work);
}

static HtmxResponse *
offline_response(HtmxRequest *request)
{
	return axis_web_error_respond(request, 409, "The disk is offline",
		"Axis cannot scan or read files on a disk that is offline. Its files are shown as last known; try "
		"again when the disk is back online.");
}

/* Whether the volume of @root_id is there; *@found says whether the root
 * exists at all. */
static gboolean
volume_usable(
	AxisWebBrowser *self,
	gint64          root_id,
	gboolean       *found
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;

	*found = axis_storage_get_root(self->storage, root_id, &root, NULL);
	if (!*found)
		return FALSE;
	volume = axis_storage_get_volume(self->storage, root->volume_id);
	return volume != NULL && axis_volume_state_is_usable(volume->state);
}

/* ── Scan now ───────────────────────────────────────────────────── */

static void
scan_run(
	AxisWebBrowser *self,
	gpointer        data
){
	ActionWork *work = data;
	gboolean found = FALSE;

	if (!volume_usable(self, work->id, &found)) {
		if (found)
			work->offline = TRUE;
		else
			g_set_error(&work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND, "no such root");
		return;
	}
	axis_library_request_scan(self->library, work->id, &work->job_id, &work->error);
}

static HtmxResponse *
scan_done(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActionWork *work = data;
	g_autofree gchar *location = NULL;
	g_autofree gchar *place = NULL;

	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	if (work->offline)
		return offline_response(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "root scan requested: id=%" G_GINT64_FORMAT " job=%" G_GINT64_FORMAT,
		work->id, work->job_id);
	if (work->folder > 0) {
		location = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, work->folder);
		return axis_web_place_redirect(request, location, "scan-requested", "folder-actions");
	}
	place = g_strdup_printf("root-%" G_GINT64_FORMAT "-row", work->id);
	return axis_web_place_redirect(request, "/library", "scan-requested", place);
}

static HtmxResponse *
handle_scan(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *folder = htmx_request_get_form_value(request, "folder");
	ActionWork *work;
	gint64 back = 0;

	if (id == 0)
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	/* A scan asked for from a folder's page returns there: the folder is
	 * a number and nothing else. */
	if (folder != NULL && folder[0] != '\0') {
		gsize i;

		if (strlen(folder) > 18 || folder[0] == '0')
			return axis_web_error_respond(request, 400, "Bad request", "The request is not valid.");
		for (i = 0; folder[i] != '\0'; i++) {
			if (folder[i] < '0' || folder[i] > '9')
				return axis_web_error_respond(request, 400, "Bad request", "The request is not valid.");
			back = back * 10 + (folder[i] - '0');
		}
	}

	work = g_new0(ActionWork, 1);
	work->id = id;
	work->folder = back;
	return axis_web_browser_dispatch(user_data, request, scan_run, scan_done, work, action_work_free);
}

/* ── Probe a folder again ───────────────────────────────────────── */

static void
reprobe_run(
	AxisWebBrowser *self,
	gpointer        data
){
	ActionWork *work = data;
	gboolean found = FALSE;

	if (!axis_library_get_dir(self->library, work->id, &work->dir, &work->error))
		return;
	if (!volume_usable(self, work->dir->root_id, &found)) {
		if (found) {
			work->offline = TRUE;
			return;
		}
		/* The root is gone with its volume; so is the folder. */
		g_set_error(&work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND, "no such folder");
		return;
	}
	axis_probe_request_folder(self->probe, work->id, &work->requested, &work->capped, &work->error);
}

static HtmxResponse *
reprobe_done(
	AxisWebBrowser *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActionWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND) ||
	    g_error_matches(work->error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	if (work->offline)
		return offline_response(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_web_storage_audit(request, "folder probe requested: folder=%" G_GINT64_FORMAT " files=%u capped=%d",
		work->id, work->requested, work->capped ? 1 : 0);
	/* The count rides in the path's query: the helper adds done and at. "n=0"
	 * is not an id: the page reads it as none. */
	if (work->requested == 0)
		location = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT, work->id);
	else
		location = g_strdup_printf("/library/folder/%" G_GINT64_FORMAT "?n=%u%s", work->id,
			work->requested, work->capped ? "&capped=1" : "");
	return axis_web_place_redirect(request, location, "reprobe", "folder-actions");
}

static HtmxResponse *
handle_reprobe(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	ActionWork *work;

	if (id == 0)
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");

	work = g_new0(ActionWork, 1);
	work->id = id;
	return axis_web_browser_dispatch(user_data, request, reprobe_run, reprobe_done, work, action_work_free);
}

void
axis_web_browser_register_actions(
	AxisWebBrowser *self,
	AxisWeb        *web
){
	axis_web_route_full(web, HTMX_METHOD_POST, "/library/root/:id/scan", AXIS_WEB_ROUTE_ADMIN, handle_scan,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/library/folder/:id/probe", AXIS_WEB_ROUTE_ADMIN,
		handle_reprobe, self);
}
