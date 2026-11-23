/*
 * axis-web-browser.c - the Library pages: their lifetime, the work they
 * hand to threads, their live updates and the module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Live updates.  The scanner writes its counters to the database as it
 * goes; while someone listens to the "library" topic a timer reads the
 * roots' state every AXIS_WEB_BROWSER_TICK_SECONDS on a worker thread and
 * publishes what changed: the progress block of a root whose scan moved,
 * "library-refresh" when a scan started or ended or a volume changed state
 * (the overview fetches itself again, rendered for its viewer), and
 * "scan-done" when a scan completed (the file lists show their notice).
 * With nobody listening nothing is read.
 */

#include "web/axis-web-browser-private.h"

#include <string.h>

/* What was last published about a root. */
typedef struct
{
	gint64  complete_scan;
	gboolean running;
	gchar  *progress;	/* the markup published */
} LiveRoot;

static void
live_root_free(gpointer data)
{
	LiveRoot *live = data;

	g_free(live->progress);
	g_free(live);
}

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebBrowser         *owner;
	AxisWebBrowserWorkFunc  work;
	AxisWebBrowserDoneFunc  done;		/* with a request */
	void                  (*finished)(AxisWebBrowser *self, gpointer data);	/* without one */
	HtmxRequest            *request;	/* nullable: background work */
	HtmxResponse           *deferred;
	gpointer                data;
	GDestroyNotify          free_data;
} Task;

static void
task_free(Task *task)
{
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

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

	task->owner->pending--;
	if (task->request != NULL) {
		HtmxResponse *response = task->done(task->owner, task->request, task->data);

		htmx_response_complete(task->deferred, response);
		g_object_unref(response);
	} else if (task->finished != NULL && !task->owner->stopping) {
		task->finished(task->owner, task->data);
	}
	task_free(task);
}

static void
run_task(Task *task)
{
	GTask *gtask;

	task->owner->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
}

HtmxResponse *
axis_web_browser_dispatch(
	AxisWebBrowser         *self,
	HtmxRequest            *request,
	AxisWebBrowserWorkFunc  work,
	AxisWebBrowserDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
){
	Task *task;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_BROWSER_MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		if (free_data != NULL)
			free_data(data);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	task->data = data;
	task->free_data = free_data;
	returned = g_object_ref(task->deferred);

	/* The answer is delivered on the main loop, never from this call. */
	run_task(task);
	return returned;
}

/* ── Live updates ───────────────────────────────────────────────── */

static gboolean
someone_listens(AxisWebBrowser *self)
{
	return self->hub != NULL && !self->stopping &&
	       axis_web_hub_count_subscribers(self->hub, AXIS_WEB_BROWSER_TOPIC) > 0;
}

/* A volume came or went, or went on or offline: the overview is fetched
 * again (its actions depend on it).  Checks that only refresh the numbers
 * change nothing here. */
static void
on_volume_event(
	AxisStorage          *storage,
	const AxisVolumeInfo *volume,
	gboolean              state_changed,
	gpointer              user_data
){
	AxisWebBrowser *self = user_data;

	if (!someone_listens(self))
		return;
	if (volume == NULL || state_changed)
		axis_web_hub_publish(self->hub, AXIS_WEB_BROWSER_TOPIC, AXIS_WEB_BROWSER_EVENT_REFRESH, "refresh");
}

typedef struct
{
	GPtrArray *roots;	/* AxisRootOverview */
	GError    *error;
} TickWork;

static void
tick_work_free(gpointer data)
{
	TickWork *work = data;

	g_clear_pointer(&work->roots, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
tick_run(
	AxisWebBrowser *self,
	gpointer        data
){
	TickWork *work = data;

	axis_library_get_overview(self->library, &work->roots, &work->error);
}

static void
publish_progress(
	AxisWebBrowser *self,
	const gchar    *markup
){
	axis_web_hub_publish(self->hub, AXIS_WEB_BROWSER_TOPIC, AXIS_WEB_BROWSER_EVENT_OOB, markup);
}

static void
tick_finished(
	AxisWebBrowser *self,
	gpointer        data
){
	TickWork *work = data;
	g_autoptr(GHashTable) seen = g_hash_table_new(g_int64_hash, g_int64_equal);
	GHashTableIter iter;
	gpointer key;
	gboolean refresh = !self->primed;
	gboolean scanned = FALSE;
	guint i;

	self->tick_busy = FALSE;
	if (work->error != NULL) {
		g_debug("library pages: cannot read the roots' state: %s", work->error->message);
		self->primed = FALSE;
		return;
	}
	if (!someone_listens(self)) {
		self->primed = FALSE;
		return;
	}

	for (i = 0; i < work->roots->len; i++) {
		const AxisRootOverview *root = g_ptr_array_index(work->roots, i);
		LiveRoot *live = g_hash_table_lookup(self->live, &root->root_id);
		HtmxNode *block = axis_web_browser_progress_block(root);
		gchar *markup;

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap-oob", "true");
		markup = htmx_element_render(HTMX_ELEMENT(block));
		g_object_unref(block);

		if (live == NULL) {
			gint64 *id = g_new(gint64, 1);

			live = g_new0(LiveRoot, 1);
			live->complete_scan = root->last_complete_scan_id;
			live->running = root->running;
			*id = root->root_id;
			g_hash_table_insert(self->live, id, live);
			/* A root that appeared: the lists are fetched again. */
			refresh = TRUE;
		} else {
			if (live->running != root->running)
				refresh = TRUE;
			if (live->complete_scan != root->last_complete_scan_id) {
				refresh = TRUE;
				scanned = TRUE;
			}
		}
		if (self->primed && g_strcmp0(live->progress, markup) != 0)
			publish_progress(self, markup);
		g_free(live->progress);
		live->progress = markup;
		live->complete_scan = root->last_complete_scan_id;
		live->running = root->running;
		g_hash_table_add(seen, (gpointer)&root->root_id);
	}

	/* Roots that are gone. */
	g_hash_table_iter_init(&iter, self->live);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (!g_hash_table_contains(seen, key)) {
			g_hash_table_iter_remove(&iter);
			refresh = TRUE;
		}
	}

	self->primed = TRUE;
	if (scanned)
		axis_web_hub_publish(self->hub, AXIS_WEB_BROWSER_TOPIC, AXIS_WEB_BROWSER_EVENT_SCANNED, "scanned");
	if (refresh)
		axis_web_hub_publish(self->hub, AXIS_WEB_BROWSER_TOPIC, AXIS_WEB_BROWSER_EVENT_REFRESH, "refresh");
}

static gboolean
on_tick(gpointer data)
{
	AxisWebBrowser *self = data;
	Task *task;
	TickWork *work;

	if (self->stopping)
		return G_SOURCE_REMOVE;
	if (self->tick_busy)
		return G_SOURCE_CONTINUE;
	if (!someone_listens(self)) {
		self->primed = FALSE;
		g_hash_table_remove_all(self->live);
		return G_SOURCE_CONTINUE;
	}
	if (self->pending >= AXIS_WEB_BROWSER_MAX_PENDING)
		return G_SOURCE_CONTINUE;

	work = g_new0(TickWork, 1);
	task = g_new0(Task, 1);
	task->owner = self;
	task->work = tick_run;
	task->finished = tick_finished;
	task->data = work;
	task->free_data = tick_work_free;
	self->tick_busy = TRUE;
	run_task(task);

	return G_SOURCE_CONTINUE;
}

void
axis_web_browser_live_start(AxisWebBrowser *self)
{
	if (self->hub == NULL || self->stopping)
		return;

	if (self->listener == 0)
		self->listener = axis_storage_add_listener(self->storage, on_volume_event, self);
	if (self->tick_id == 0)
		self->tick_id = g_timeout_add_seconds(AXIS_WEB_BROWSER_TICK_SECONDS, on_tick, self);
}

void
axis_web_browser_live_stop(AxisWebBrowser *self)
{
	if (self->listener != 0) {
		axis_storage_remove_listener(self->storage, self->listener);
		self->listener = 0;
	}
	if (self->tick_id != 0) {
		g_source_remove(self->tick_id);
		self->tick_id = 0;
	}
	self->primed = FALSE;
	g_hash_table_remove_all(self->live);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebBrowser *
axis_web_browser_new(
	AxisLibrary *library,
	AxisProbe   *probe,
	AxisStorage *storage,
	AxisWebHub  *hub
){
	AxisWebBrowser *self;

	g_return_val_if_fail(library != NULL, NULL);
	g_return_val_if_fail(probe != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisWebBrowser, 1);
	self->library = axis_library_ref(library);
	self->probe = axis_probe_ref(probe);
	self->storage = axis_storage_ref(storage);
	self->hub = hub;
	self->live = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, live_root_free);
	return self;
}

void
axis_web_browser_stop(AxisWebBrowser *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	axis_web_browser_live_stop(self);
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_browser_free(AxisWebBrowser *self)
{
	guint i;

	if (self == NULL)
		return;

	axis_web_browser_stop(self);
	for (i = 0; i < AXIS_MEDIA_TYPE_COUNT; i++)
		g_free(self->titles[i].module_id);
	g_hash_table_unref(self->live);
	axis_library_unref(self->library);
	axis_probe_unref(self->probe);
	axis_storage_unref(self->storage);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	g_free(self);
}

void
axis_web_browser_set_estimator(
	AxisWebBrowser *self,
	AxisEstimator  *estimator
){
	g_return_if_fail(self != NULL);

	if (estimator != NULL)
		axis_estimator_ref(estimator);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	self->estimator = estimator;
}

void
axis_web_folder_title_free(AxisWebFolderTitle *item)
{
	if (item == NULL)
		return;
	axis_meta_title_free(item->title);
	g_free(item->poster);
	g_free(item);
}

void
axis_web_browser_set_folder_titles(
	AxisWebBrowser          *self,
	AxisMediaType            type,
	const gchar             *module_id,
	AxisWebFolderTitlesFunc  func,
	gpointer                 user_data
){
	AxisWebBrowserTitles *titles;

	g_return_if_fail(self != NULL);
	g_return_if_fail((guint)type < AXIS_MEDIA_TYPE_COUNT);
	g_return_if_fail(func == NULL || module_id != NULL);

	titles = &self->titles[type];
	g_clear_pointer(&titles->module_id, g_free);
	titles->func = func;
	titles->user_data = func != NULL ? user_data : NULL;
	if (func != NULL)
		titles->module_id = g_strdup(module_id);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	axis_web_browser_live_start(user_data);
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	axis_web_browser_live_stop(user_data);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_browser_register_pages(user_data, web);
	axis_web_browser_register_actions(user_data, web);
	axis_web_browser_register_dashboard(user_data, web);
}

static const AxisNavEntry nav_entries[] = {
	{ "Files", "/library", 10, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = {
	AXIS_STORAGE_MODULE_ID, AXIS_LIBRARY_MODULE_ID, AXIS_PROBE_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_web_browser_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_WEB_BROWSER_MODULE_ID;
		info.name = "Files";
		info.start = module_start;
		info.stop = module_stop;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Pages to look at the files: disks and library folders, folders, a list of every "
			"file that can be filtered, and what Axis read inside each file. It only reads Axis's records, "
			"and asks for scans and file reads when an administrator does.";
		info.depends_on = depends_on;
	}

	return &info;
}
