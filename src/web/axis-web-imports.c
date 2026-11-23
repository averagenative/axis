/*
 * axis-web-imports.c - the import module: its lifetime, its jobs, the work
 * it hands to threads, and what the dashboard says of imports
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-imports-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-storage-private.h"

/* The row of attention_count the hourly cleanup leaves: how many movies and
 * episodes have more than one file. */
#define AXIS_WEB_IMPORTS_DUPLICATES_COUNT "duplicates"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebImports         *owner;
	AxisWebImportsWorkFunc  work;
	AxisWebImportsDoneFunc  done;
	HtmxRequest            *request;	/* nullable */
	HtmxResponse           *deferred;	/* nullable */
	gpointer                data;
	GDestroyNotify          free_data;
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
axis_web_imports_dispatch(
	AxisWebImports         *self,
	HtmxRequest            *request,
	AxisWebImportsWorkFunc  work,
	AxisWebImportsDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
){
	HtmxResponse *returned = NULL;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_WEB_IMPORTS_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		if (request == NULL)
			return NULL;
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
	if (request != NULL) {
		task->request = g_object_ref(request);
		task->deferred = htmx_response_new_deferred();
		returned = g_object_ref(task->deferred);
	}
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── The volume warning ─────────────────────────────────────────── */

HtmxNode *
axis_web_imports_volume_warnings(const GPtrArray *warnings)
{
	HtmxDiv *block;
	guint i;

	if (warnings == NULL || warnings->len == 0)
		return NULL;
	block = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(block), "notice");
	htmx_element_add_class(HTMX_ELEMENT(block), "notice-error");
	htmx_element_add_class(HTMX_ELEMENT(block), "import-volume-warning");
	htmx_element_set_attribute(HTMX_ELEMENT(block), "role", "status");
	for (i = 0; i < warnings->len && i < 10; i++) {
		g_autofree gchar *text = axis_import_volume_warning_text(g_ptr_array_index(warnings, i));
		HtmxP *p = htmx_p_new();

		htmx_node_set_text_content(HTMX_NODE(p), text);
		axis_web_storage_add(block, p);
	}
	if (warnings->len > 10) {
		g_autofree gchar *more = g_strdup_printf("And %u more such pairs.", warnings->len - 10);
		HtmxP *p = htmx_p_new();

		htmx_node_set_text_content(HTMX_NODE(p), more);
		axis_web_storage_add(block, p);
	}
	return HTMX_NODE(block);
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The imports' notice (add-attention-notices): how many are blocked or partly
 * done, in one statement over the partial index import_attention.  Only a
 * person's decision ends either, so there is no wait.  One item for them all
 * ("3 imports are blocked"), which names no title and no file; "newest" is
 * the highest id among them, so a new one is a new occurrence.
 *
 * Slice N2: the imports that wait because Read-only storage is on
 * ("import.read_only"), a second statement (waiting_source() below) run only
 * while the setting is on.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebImports *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 blocked = 0;
	gint64 partial = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(axis_import_get_pool(self->import),
	        "SELECT count(*) FILTER (WHERE state = 'blocked'), count(*) FILTER (WHERE state = 'partial'),"
	        " coalesce(max(id), 0) FROM import WHERE state IN ('blocked', 'partial')", reading->deadline_ms, &result,
	        error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &blocked);
	axis_db_result_get_int64(result, 0, 1, &partial);
	axis_db_result_get_int64(result, 0, 2, &newest);
	if (blocked + partial <= 0)
		return TRUE;

	if (partial == 0)
		text = blocked == 1 ?
			g_strdup("1 import is blocked: it needs a person to decide what to do with its files.") :
			g_strdup_printf("%" G_GINT64_FORMAT " imports are blocked: each needs a person to decide what to do with "
				"its files.", blocked);
	else if (blocked == 0)
		text = partial == 1 ?
			g_strdup("1 import is partly done: it needs a person to decide what to do with the files that were not "
				"imported.") :
			g_strdup_printf("%" G_GINT64_FORMAT " imports are partly done: each needs a person to decide what to do "
				"with the files that were not imported.", partial);
	else
		text = g_strdup_printf("%" G_GINT64_FORMAT " %s blocked and %" G_GINT64_FORMAT " %s partly done: each needs a "
			"person to decide what to do with its files.", blocked, blocked == 1 ? "import is" : "imports are",
			partial, partial == 1 ? "is" : "are");
	item = axis_attention_item_new("import.blocked", "", AXIS_ATTENTION_WAITING, (guint64)(blocked + partial), newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/imports", "Open Imports");
	g_ptr_array_add(items, item);
	return TRUE;
}

/*
 * The imports that wait because Read-only storage is on (slice N2): the
 * setting is a person's, and nothing but switching it off ends the wait.  One
 * statement over the partial index import_waiting (migration 0077), and none
 * while the setting is off.
 *
 * It is one thing, however many imports wait: the item's number is 1 and its
 * "newest" 0 (the sentence says how many), so that a person who keeps
 * Read-only storage on for a purpose dismisses it once, and it stays
 * dismissed while downloads go on arriving; it is shown again when the
 * setting has been off and is switched on again with imports waiting (the
 * notice ended meanwhile, and its dismissals with it).
 */
static gboolean
waiting_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebImports *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 waiting = 0;

	if (!axis_storage_is_read_only_mode(axis_import_get_storage(self->import)))
		return TRUE;
	if (!axis_db_pool_query_bounded(axis_import_get_pool(self->import),
	        "SELECT count(*) FROM import WHERE state = 'waiting' AND reason = 'read_only'", reading->deadline_ms, &result,
	        error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &waiting);
	if (waiting <= 0)
		return TRUE;
	text = waiting == 1 ?
		g_strdup("1 import waits because Read-only storage is on: nothing is imported until you switch it off.") :
		g_strdup_printf("%" G_GINT64_FORMAT " imports wait because Read-only storage is on: nothing is imported until "
			"you switch it off.", waiting);
	item = axis_attention_item_new("import.read_only", "", AXIS_ATTENTION_WAITING, 1, 0);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/admin/settings#module-" AXIS_STORAGE_MODULE_ID, "Open Settings, Storage");
	g_ptr_array_add(items, item);
	return TRUE;
}

/*
 * The movies and episodes that have more than one file (slice N2).  Which
 * they are is worked out from the files' links on each view of the
 * Duplicates page; a reading does not do that.  The hourly import cleanup
 * counts them as the page does and leaves the number in attention_count
 * (count_duplicates() below; migration 0077), and this reads that row: so the
 * notice can be up to an hour behind the page, and its sentence says when it
 * was counted.  A duplicate uses space and stops nothing: "Worth a look".
 * "newest" is 0: more of them is what shows a dismissed notice again.
 */
static gboolean
duplicates_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebImports *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;

	if (!axis_db_pool_query_bounded(axis_import_get_pool(self->import),
	        "SELECT count FROM attention_count WHERE kind = '" AXIS_WEB_IMPORTS_DUPLICATES_COUNT "'",
	        reading->deadline_ms, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_int64(result, 0, 0, &n);
	if (n <= 0)
		return TRUE;
	text = n == 1 ? g_strdup("1 movie or episode has more than one file, at the last hourly count.") :
		g_strdup_printf("%" G_GINT64_FORMAT " movies or episodes have more than one file, at the last hourly count.",
			n);
	item = axis_attention_item_new("import.duplicates", "", AXIS_ATTENTION_LOOK, (guint64)n, 0);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/duplicates", "Open Duplicates");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* Counts the duplicates as the Duplicates page lists them, and leaves the
 * number for duplicates_source().  From the hourly cleanup's job. */
static gboolean
count_duplicates(
	AxisWebImports  *self,
	GError         **error
){
	g_autoptr(GPtrArray) groups = NULL;
	g_autofree gchar *count = NULL;
	const gchar *params[1];

	if (!axis_import_duplicates_list(self->import, &groups, error))
		return FALSE;
	count = g_strdup_printf("%u", groups != NULL ? groups->len : 0);
	params[0] = count;
	return axis_db_pool_exec(axis_import_get_pool(self->import),
		"INSERT INTO attention_count (kind, count) VALUES ('" AXIS_WEB_IMPORTS_DUPLICATES_COUNT "', $1::bigint)"
		" ON CONFLICT (kind) DO UPDATE SET count = excluded.count, counted_at = now()", 1, params, NULL, error);
}

/* ── The jobs ───────────────────────────────────────────────────── */

static gboolean
run_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebImports *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	AxisImportReport report;
	gint64 id = 0;

	if (payload != NULL && json_object_has_member(payload, "import")) {
		JsonNode *node = json_object_get_member(payload, "import");

		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
			id = json_node_get_int(node);
	}
	/* A payload that names no import never will: not tried again. */
	if (id <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The job names no import.");
		return FALSE;
	}
	return axis_import_run(self->import, id, context, &report, error);
}

static gboolean
cleanup_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebImports *self = user_data;
	AxisImportCleanupReport report;
	g_autoptr(GPtrArray) folders = NULL;
	g_autoptr(GError) local = NULL;

	if (!axis_import_run_cleanup(self->import, 0, &report, error))
		return FALSE;
	/* What is left in the clients' folders for Axis, counted for the links
	 * on the Downloads page and the dashboard (nothing is deleted). */
	if (!axis_import_leftovers(self->import, 0, FALSE, &folders, &local))
		g_debug("import cleanup: the leftovers could not be counted: %s", local->message);
	g_clear_error(&local);
	/* The movies and episodes with more than one file, counted for the page
	 * "Needs attention" (nothing is touched): the Duplicates page works them
	 * out on each view, which a reading of the notices must not. */
	if (!count_duplicates(self, &local))
		g_debug("import cleanup: the duplicates could not be counted: %s", local->message);
	return TRUE;
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_IMPORT_KIND, AXIS_DOWNLOAD_QUEUE, 3, run_handler,
	  "Importing a download into the library", AXIS_JOB_CLASS_WORK },
	{ AXIS_IMPORT_CLEANUP_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, cleanup_handler,
	  "Removing old replaced files and download leftovers after imports", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_IMPORT_CLEANUP_SCHEDULE, AXIS_IMPORT_CLEANUP_KIND, NULL, AXIS_IMPORT_CLEANUP_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Imports", "/imports", 6, AXIS_NAV_GROUP_ACTIVITY, FALSE, NULL, 0 },
	{ "File naming", "/imports/naming", 36, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	/* Movies and episodes with more than one file: a page of the media, of
	 * every type, after the types' own. */
	{ "Duplicates", "/duplicates", 300, AXIS_NAV_GROUP_MEDIA, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { "storage", "downloads", NULL };

/* An import is tried again by its own row: the job, retried as it was, finds
 * a blocked import and does nothing. */
static const gchar *const retry_elsewhere[] = {
	AXIS_IMPORT_KIND, "Retry the import on its page under Imports, which also checks that no other import of its "
		"title is open. Axis takes up a waiting import by itself.",
	NULL
};

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebImports *self = user_data;

	axis_web_add_attention_source(web, "the imports", attention_source, self);
	axis_web_add_attention_source(web, "the imports that wait", waiting_source, self);
	axis_web_add_attention_source(web, "the duplicates", duplicates_source, self);
	axis_web_imports_register_pages(self, web);
	axis_web_imports_register_leftovers(self, web);
	axis_web_imports_register_duplicates(self, web);
}

const AxisModuleInfo *
axis_web_imports_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_IMPORT_MODULE_ID;
		info.name = "Imports";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Takes completed downloads into the library for the movie or series each was grabbed for: "
			"it moves their files (a rename on the same disk, else a verified copy, the download's file deleted after), "
			"or hardlinks them where torrents are left seeding, under the names Plex and Jellyfin read; an upgrade "
			"keeps the old file in a recycle folder for a while. Nothing is written while storage is read-only.";
		info.settings = axis_import_get_setting_table();
		info.depends_on = depends_on;
		info.job_retry_elsewhere = retry_elsewhere;
	}
	return &info;
}

AxisWebImports *
axis_web_imports_new(
	AxisImport   *import,
	AxisSettings *settings
){
	AxisWebImports *self;

	g_return_val_if_fail(import != NULL, NULL);

	self = g_new0(AxisWebImports, 1);
	self->import = axis_import_ref(import);
	self->settings = settings;
	return self;
}

void
axis_web_imports_stop(AxisWebImports *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_imports_free(AxisWebImports *self)
{
	if (self == NULL)
		return;
	axis_web_imports_stop(self);
	axis_import_unref(self->import);
	g_free(self);
}
