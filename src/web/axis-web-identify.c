/*
 * axis-web-identify.c - the identification module: its lifetime, its jobs,
 * the work it hands to threads, the dashboard's item, and its part of a
 * title's page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-identify-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebIdentify         *owner;
	AxisWebIdentifyWorkFunc  work;
	AxisWebIdentifyDoneFunc  done;
	HtmxRequest             *request;
	HtmxResponse            *deferred;
	gpointer                 data;
	GDestroyNotify           free_data;
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
	if (task->deferred != NULL)
		htmx_response_complete(task->deferred, response);
	if (response != NULL)
		g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

HtmxResponse *
axis_web_identify_dispatch(
	AxisWebIdentify         *self,
	HtmxRequest             *request,
	AxisWebIdentifyWorkFunc  work,
	AxisWebIdentifyDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_WEB_IDENTIFY_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->data = data;
	task->free_data = free_data;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * What was found in the library and waits for a person to say which title it
 * is (add-attention-notices, slice N2): the movies and the series folders in
 * the state "review", in one statement over the partial index
 * library_identity_review (migration 0077).  Only a person ends it, so there
 * is no wait.  One item for them all, which names no title and no folder;
 * "newest" is the highest id among them.
 */
static gboolean
review_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebIdentify *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *movies_text = NULL;
	g_autofree gchar *series_text = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 movies = 0;
	gint64 series = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(axis_identify_get_pool(self->identify),
	        "SELECT count(*) FILTER (WHERE r.media_type = 'movies'), count(*) FILTER (WHERE r.media_type = 'tv'),"
	        " coalesce(max(i.id), 0) FROM library_identity i JOIN library_root r ON r.id = i.root_id"
	        " WHERE i.state = 'review'", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &movies);
	axis_db_result_get_int64(result, 0, 1, &series);
	axis_db_result_get_int64(result, 0, 2, &newest);
	if (movies + series <= 0)
		return TRUE;
	movies_text = movies == 1 ? g_strdup("1 movie") : g_strdup_printf("%" G_GINT64_FORMAT " movies", movies);
	series_text = series == 1 ? g_strdup("1 series folder") :
		g_strdup_printf("%" G_GINT64_FORMAT " series folders", series);
	text = g_strdup_printf("%s%s%s found in the library %s for a person to say which title %s.",
		movies > 0 ? movies_text : "", movies > 0 && series > 0 ? " and " : "", series > 0 ? series_text : "",
		movies + series == 1 ? "waits" : "wait", movies + series == 1 ? "it is" : "they are");
	item = axis_attention_item_new("identify.review", "", AXIS_ATTENTION_WAITING, (guint64)(movies + series), newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/identify", "Open Identify");
	g_ptr_array_add(items, item);
	return TRUE;
}

/*
 * What Axis linked to a title by itself and is no longer sure of (slice N2):
 * library_identity.doubt, set when a later look at a linked movie or series
 * folder no longer agrees with its title.  Its files stay linked meanwhile,
 * so it is "Worth a look".  One statement over the partial index
 * library_identity_doubt (migration 0077).
 */
static gboolean
doubt_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebIdentify *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(axis_identify_get_pool(self->identify),
	        "SELECT count(*), coalesce(max(id), 0) FROM library_identity WHERE doubt IS NOT NULL", reading->deadline_ms,
	        &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &n);
	axis_db_result_get_int64(result, 0, 1, &newest);
	if (n <= 0)
		return TRUE;
	text = n == 1 ? g_strdup("1 movie or series folder that Axis linked to a title by itself no longer looks like "
		"that title. Confirm it or choose another.") :
		g_strdup_printf("%" G_GINT64_FORMAT " movies or series folders that Axis linked to a title by itself no "
			"longer look like their titles. Confirm each or choose another.", n);
	item = axis_attention_item_new("identify.doubt", "", AXIS_ATTENTION_LOOK, (guint64)n, newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/identify", "Open Identify");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* ── A title's page ─────────────────────────────────────────────── */

typedef struct
{
	gint64     title_id;
	GPtrArray *units;
	gboolean   managed;
} TitleFigures;

static void
title_figures_free(gpointer data)
{
	TitleFigures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->units, g_ptr_array_unref);
	g_free(figures);
}

static gpointer
title_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	AxisWebIdentify *self = user_data;
	const AxisMetaTitle *title;
	TitleFigures *figures;
	g_autoptr(GError) error = NULL;

	if (place != AXIS_WEB_ACTION_PAGE || titles->len != 1)
		return NULL;
	title = g_ptr_array_index(titles, 0);
	if ((title->kind != AXIS_META_KIND_FILM && title->kind != AXIS_META_KIND_SERIES) || title->id <= 0)
		return NULL;
	figures = g_new0(TitleFigures, 1);
	figures->title_id = title->id;
	if (!axis_identify_units_of_title(self->identify, title->id, &figures->units, &figures->managed, &error)) {
		g_message("identify: a title's units could not be read: %s", error->message);
		title_figures_free(figures);
		return NULL;
	}
	return figures;
}

static HtmxNode *
title_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             data,
	gpointer             user_data
){
	TitleFigures *figures = data;
	AxisWebIdentify *self = user_data;
	const gchar *owner = axis_identify_get_owner_module_for(self->identify, title->kind);
	AxisWeb *web = axis_web_from_request(request);
	AxisModuleRegistry *registry = web != NULL ? axis_web_get_registry(web) : NULL;
	HtmxNode *card;
	guint i;

	if (figures == NULL || figures->units == NULL || figures->units->len == 0 || title->id != figures->title_id)
		return NULL;
	/* A managed title shows its files in its own module's card, with their
	 * origin; so does one on disk its owner does not manage, while the
	 * owner's module (Movies, TV) is on. */
	if (figures->managed || (owner != NULL && registry != NULL && axis_module_registry_is_enabled(registry, owner)))
		return NULL;
	if (place == AXIS_WEB_ACTION_PAGE) {
		HtmxP *note = htmx_p_new();
		HtmxSpan *text = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(note), "help");
		htmx_element_add_class(HTMX_ELEMENT(note), "identify-note");
		htmx_node_set_text_content(HTMX_NODE(text), title->kind == AXIS_META_KIND_SERIES ? "Its episodes are already "
			"on disk (found by Identify). To manage this series, turn on the TV module under System, " :
			"Its file is already on disk (found by Identify). To manage this movie, turn on the Movies module under "
			"System, ");
		ADD(note, text);
		ADD(note, axis_web_browser_link("/admin/modules", "Modules"));
		return HTMX_NODE(note);
	}
	if (place != AXIS_WEB_ACTION_DETAIL)
		return NULL;
	card = axis_web_storage_card("Folders and files on disk");
	for (i = 0; i < figures->units->len; i++) {
		const AxisIdentifyUnitInfo *unit = g_ptr_array_index(figures->units, i);
		g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, unit->id);
		g_autofree gchar *where = g_strdup_printf("%s: ", unit->root_name != NULL ? unit->root_name : "a library folder");
		HtmxDiv *line = htmx_div_new();
		HtmxSpan *root = htmx_span_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(line), "identify-line");
		htmx_node_set_text_content(HTMX_NODE(root), where);
		ADD(line, root);
		ADD(link, axis_web_browser_name_node(unit->rel_path));
		ADD(line, link);
		ADD(line, axis_web_identify_badge(unit));
		ADD(card, line);
		ADD(card, axis_web_identify_files_table(unit, FALSE));
	}
	return card;
}

/* ── The jobs ───────────────────────────────────────────────────── */

static gboolean
root_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebIdentify *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	AxisIdentifyReport report;
	gint64 root = 0;

	if (payload != NULL && json_object_has_member(payload, "root")) {
		JsonNode *node = json_object_get_member(payload, "root");

		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
			root = json_node_get_int(node);
	}
	/* A payload that names no root never will: not tried again. */
	if (root <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The job names no root.");
		return FALSE;
	}
	if (!axis_identify_run_root(self->identify, root, context, &report, error))
		return FALSE;
	if (report.skipped != NULL)
		axis_job_context_set_progress(context, 1.0, report.skipped);
	else if (report.examined > 0)
		g_info("identify: root %" G_GINT64_FORMAT ": %u looked at, %u linked, %u to review, %u left", root,
			report.examined, report.linked, report.review, report.pending);
	return TRUE;
}

static gboolean
schedule_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebIdentify *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	guint queued = 0;

	/* The minute's schedule: only the roots whose last run left work. */
	if (payload != NULL && json_object_has_member(payload, "continue") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(payload, "continue")) &&
	    json_object_get_boolean_member(payload, "continue"))
		return axis_identify_run_continue(self->identify, &queued, error);
	return axis_identify_run_schedule(self->identify, &queued, error);
}

static const AxisJobHandlerInfo handlers[] = {
	/* The scan queue: identification follows the scans, and never holds up
	 * the maintenance jobs while it waits for the provider. */
	{ AXIS_IDENTIFY_KIND, AXIS_LIBRARY_SCAN_QUEUE, 2, root_handler,
	  "Identifying the movies and series in a library folder", AXIS_JOB_CLASS_WORK },
	{ AXIS_IDENTIFY_SCHEDULE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, schedule_handler,
	  "Choosing library folders to identify", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_IDENTIFY_SCHEDULE, AXIS_IDENTIFY_SCHEDULE_KIND, NULL, AXIS_IDENTIFY_SCHEDULE_INTERVAL_S },
	{ AXIS_IDENTIFY_CONTINUE, AXIS_IDENTIFY_SCHEDULE_KIND, "{\"continue\": true}", AXIS_IDENTIFY_CONTINUE_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Identify", "/identify", 12, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { "storage", AXIS_META_MODULE_ID, NULL };

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebIdentify *self = user_data;

	axis_web_add_attention_source(web, "what waits in Identify", review_source, self);
	axis_web_add_attention_source(web, "the links Axis is no longer sure of", doubt_source, self);
	axis_web_add_title_actions(web, title_collect, title_render, title_figures_free, self);
	axis_web_identify_register_pages(self, web);
}

const AxisModuleInfo *
axis_web_identify_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_IDENTIFY_MODULE_ID;
		info.name = "Identify";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Recognises the movies and series already in the library: each movie folder or file, and "
			"each series' folder, is matched to a title by the identifiers in its names (or a movie's .nfo file), or "
			"by its title, year and runtime, and each episode file is read for the episodes it holds. Only sure "
			"matches are linked by themselves; the rest wait for a person. Nothing on disk is changed.";
		info.settings = axis_identify_get_setting_table();
		info.depends_on = depends_on;
	}
	return &info;
}

AxisWebIdentify *
axis_web_identify_new(
	AxisIdentify     *identify,
	AxisQualityStore *quality
){
	AxisWebIdentify *self;

	g_return_val_if_fail(identify != NULL && quality != NULL, NULL);

	self = g_new0(AxisWebIdentify, 1);
	self->identify = axis_identify_ref(identify);
	self->quality = axis_quality_store_ref(quality);
	return self;
}

void
axis_web_identify_stop(AxisWebIdentify *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

gboolean
axis_web_identify_folder_titles(
	gpointer    user_data,
	gint64      root_id,
	GPtrArray  *dirs,
	GPtrArray  *titles,
	GError    **error
){
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gint64 *ids = NULL;
	guint n;
	guint i;

	g_return_val_if_fail(user_data != NULL, FALSE);
	g_return_val_if_fail(dirs != NULL && titles != NULL, FALSE);

	n = MIN(dirs->len, AXIS_IDENTIFY_FOLDERS_MAX);
	ids = g_new0(gint64, MAX(n, 1));
	for (i = 0; i < n; i++)
		ids[i] = ((const AxisDirRecord *)g_ptr_array_index(dirs, i))->id;
	if (!axis_identify_folders(user_data, ids, n, &folders, error))
		return FALSE;
	for (i = 0; i < folders->len; i++) {
		AxisIdentifyFolder *folder = g_ptr_array_index(folders, i);
		AxisWebFolderTitle *item = g_new0(AxisWebFolderTitle, 1);

		item->dir_id = folder->dir_id;
		item->unit_id = folder->unit_id;
		item->not_title = g_strcmp0(folder->state, "not_film") == 0;
		item->title = g_steal_pointer(&folder->title);
		item->poster = g_steal_pointer(&folder->poster);
		g_ptr_array_add(titles, item);
	}
	return TRUE;
}

void
axis_web_identify_free(AxisWebIdentify *self)
{
	if (self == NULL)
		return;
	axis_web_identify_stop(self);
	axis_identify_unref(self->identify);
	axis_quality_store_unref(self->quality);
	g_free(self);
}
