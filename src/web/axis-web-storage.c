/*
 * axis-web-storage.c - the storage module: its lifetime, its job, its live
 * updates and the work it hands to threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-storage-private.h"

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web-attention.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebStorage         *owner;
	AxisWebStorageWorkFunc  work;
	AxisWebStorageDoneFunc  done;
	HtmxRequest            *request;	/* nullable: work without a request */
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
axis_web_storage_dispatch(
	AxisWebStorage         *self,
	HtmxRequest            *request,
	AxisWebStorageWorkFunc  work,
	AxisWebStorageDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
){
	Task *task;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_STORAGE_MAX_PENDING) {
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

static void
on_volume_event(
	AxisStorage          *storage,
	const AxisVolumeInfo *volume,
	gboolean              state_changed,
	gpointer              user_data
){
	AxisWebStorage *self = user_data;
	g_autofree gchar *markup = NULL;

	/* A disk that went or came back, or was added or removed. */
	if (!self->stopping && (state_changed || volume == NULL))
		axis_attention_ping_changed(self->attention);

	if (self->hub == NULL || self->stopping ||
	    axis_web_hub_count_subscribers(self->hub, AXIS_STORAGE_TOPIC) == 0)
		return;

	if (volume == NULL) {
		axis_web_hub_publish(self->hub, AXIS_STORAGE_TOPIC, AXIS_STORAGE_EVENT_CHANGED, "refresh");
		return;
	}

	markup = axis_web_storage_render_live(volume);
	axis_web_hub_publish(self->hub, AXIS_STORAGE_TOPIC, AXIS_STORAGE_EVENT_OOB, markup);
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The disks' notices (add-attention-notices), from what the daemon knows of
 * them in memory: no statement and no I/O.
 *
 *   volume.mismatch  a disk not recognised at its path: only a person ends
 *                    it, so it is a notice at once.
 *   volume.offline   a disk offline: checked again every minute and usually
 *                    back by itself (a mount that was slow, a share that
 *                    dropped), so it is a notice only once it has been
 *                    away for AXIS_ATTENTION_DISK_WAIT_S.
 *
 * A disk is judged only once this daemon has checked it (its last check is
 * after these pages were made): what its row says from before the daemon
 * started is how it was left, hours or days ago, not how it is.  The wait
 * and "newest" count from when the disk stopped being usable
 * (unusable_since_us), which a change of the reason does not move, so a disk
 * that came back and went again is a new occurrence and one whose empty
 * mount point was then removed is the same one.  The sentence names the
 * disk, never its path.
 *
 * Slice N2 adds three for a disk that is there (attention_usable()), all
 * "Worth a look": the rest still works.
 */

/*
 * The notices of a disk that is found at its path:
 *
 *   volume.low_space  less free space than the setting
 *                     "storage.low_space_percent" of its size (0: never
 *                     told).  Nothing but a person ends it, so there is no
 *                     wait; nothing says when it began, so "newest" is 0 and
 *                     a dismissal lasts until the disk has room again.
 *   volume.read_only  mounted read-only while Axis may write (Read-only
 *                     storage is off): nothing can be imported onto it.
 *                     While Axis writes nothing anyway it is no notice.
 *                     "newest" is when the state began.
 *   volume.marker     its marker file could not be written although the disk
 *                     takes writes.  Tried again at every check, but nothing
 *                     records since when, so it has no wait: a write that
 *                     fails on a disk that takes writes is not expected to
 *                     mend itself.
 */
static const gchar *const usable_kinds[] = { "volume.low_space", "volume.read_only", "volume.marker" };

/* Every kind a disk can have. */
static const gchar *const disk_kinds[] = { "volume.mismatch", "volume.offline", "volume.low_space", "volume.read_only",
	"volume.marker" };

static void
attention_usable(
	AxisWebStorage       *self,
	const AxisVolumeInfo *volume,
	const gchar          *subject,
	const gchar          *href,
	guint                 low_percent,
	gboolean              read_only_mode,
	GPtrArray            *items
){
	AxisAttentionItem *item;

	if (!axis_volume_state_is_usable(volume->state))
		return;
	/* Its space could not be read at this check: not known, not "has room". */
	if (low_percent > 0 && !volume->has_space)
		axis_attention_not_known(items, "volume.low_space", subject);
	if (low_percent > 0 && volume->has_space && volume->total_bytes > 0 && volume->free_bytes <= volume->total_bytes &&
	    volume->free_bytes < volume->total_bytes / 100 * low_percent) {
		g_autofree gchar *free_text = axis_size_format(volume->free_bytes);
		g_autofree gchar *total = axis_size_format(volume->total_bytes);
		g_autofree gchar *tail = g_strdup_printf(" is almost full: %s free of %s, less than %u percent.", free_text, total,
			low_percent);

		item = axis_attention_item_new("volume.low_space", subject, AXIS_ATTENTION_LOOK, 1, 0);
		axis_attention_item_say(item, "The disk ", volume->name, tail);
		axis_attention_item_link(item, href, "Open the disk");
		g_ptr_array_add(items, item);
	}
	if (volume->state == AXIS_VOLUME_READ_ONLY && !read_only_mode) {
		item = axis_attention_item_new("volume.read_only", subject, AXIS_ATTENTION_LOOK, 1,
			volume->state_changed_us / G_USEC_PER_SEC);
		axis_attention_item_say(item, "The disk ", volume->name, " is mounted read-only: nothing can be imported onto "
			"it until it is mounted read-write.");
		axis_attention_item_link(item, href, "Open the disk");
		item->since_us = volume->state_changed_us;
		g_ptr_array_add(items, item);
	} else if (volume->state != AXIS_VOLUME_READ_ONLY && volume->marker_problem != NULL && !volume->has_marker) {
		item = axis_attention_item_new("volume.marker", subject, AXIS_ATTENTION_LOOK, 1, 0);
		axis_attention_item_say(item, "Axis could not write its marker file on the disk ", volume->name,
			", so the disk is recognised by its filesystem only.");
		axis_attention_item_link(item, href, "Open the disk");
		g_ptr_array_add(items, item);
	}
	(void)self;
}

static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebStorage *self = user_data;
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	guint low_percent = axis_storage_get_low_space_percent(self->storage);
	gboolean read_only_mode = axis_storage_is_read_only_mode(self->storage);
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);
		g_autofree gchar *subject = g_strdup_printf("%" G_GINT64_FORMAT, volume->id);
		g_autofree gchar *href = g_strdup_printf("/storage/volumes/%" G_GINT64_FORMAT, volume->id);
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;
		gint64 since_us = volume->unusable_since_us > 0 ? volume->unusable_since_us : volume->state_changed_us;

		/* Not checked by this daemon yet: not judged, and not taken as well
		 * either.  "Not known yet" keeps what users dismissed of it before the
		 * daemon was started until its first check has said how it is. */
		if (volume->last_checked_us < self->started_us) {
			guint k;

			for (k = 0; k < G_N_ELEMENTS(disk_kinds); k++)
				axis_attention_not_known(items, disk_kinds[k], subject);
			continue;
		}
		/* A disk that is away: whether it is almost full, mounted read-only
		 * or without its marker is not known while it is (its last notices of
		 * those stay as they are, and come true or end when it is back). */
		if (!axis_volume_state_is_usable(volume->state)) {
			guint k;

			for (k = 0; k < G_N_ELEMENTS(usable_kinds); k++)
				axis_attention_not_known(items, usable_kinds[k], subject);
		}
		if (volume->state == AXIS_VOLUME_MISMATCH) {
			item = axis_attention_item_new("volume.mismatch", subject, AXIS_ATTENTION_BROKEN, 1,
				since_us / G_USEC_PER_SEC);
			tail = g_strdup_printf(" is not recognised at its path. %s", axis_volume_reason_describe(volume->reason));
		} else if (volume->state == AXIS_VOLUME_OFFLINE &&
		           since_us > 0 && reading->now_us - since_us >= (gint64)AXIS_ATTENTION_DISK_WAIT_S * G_USEC_PER_SEC) {
			g_autofree gchar *how_long = axis_attention_duration(reading->now_us - since_us);

			item = axis_attention_item_new("volume.offline", subject, AXIS_ATTENTION_BROKEN, 1,
				since_us / G_USEC_PER_SEC);
			tail = g_strdup_printf(" has been offline for %s. %s", how_long,
				axis_volume_reason_describe(volume->reason));
		} else {
			attention_usable(self, volume, subject, href, low_percent, read_only_mode, items);
			continue;
		}
		axis_attention_item_say(item, "The disk ", volume->name, tail);
		axis_attention_item_link(item, href, "Open the disk");
		item->since_us = since_us;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/* ── The check ──────────────────────────────────────────────────── */

static gboolean
check_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebStorage *self = user_data;

	return axis_storage_check_all(self->storage, axis_job_context_get_cancellable(context), error);
}

static void
startup_check(
	AxisWebStorage *self,
	gpointer        data
){
	g_autoptr(GError) error = NULL;

	if (!axis_storage_check_all(self->storage, NULL, &error))
		g_warning("storage: the first check of the volumes failed: %s", error->message);
}

/* Every volume is checked as soon as the module starts, rather than at
 * the schedule's next turn, so that no page shows a state from before the
 * daemon started for long. */
static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	AxisWebStorage *self = user_data;
	Task *task;

	if (self->stopping)
		return TRUE;

	task = g_new0(Task, 1);
	task->owner = self;
	task->work = startup_check;
	run_task(task);
	return TRUE;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebStorage *
axis_web_storage_new(
	AxisStorage *storage,
	AxisWebHub  *hub
){
	AxisWebStorage *self;

	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisWebStorage, 1);
	self->storage = axis_storage_ref(storage);
	self->hub = hub;
	self->started_us = g_get_real_time();
	self->listener = axis_storage_add_listener(storage, on_volume_event, self);

	return self;
}

void
axis_web_storage_stop(AxisWebStorage *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	if (self->listener != 0) {
		axis_storage_remove_listener(self->storage, self->listener);
		self->listener = 0;
	}
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_storage_free(AxisWebStorage *self)
{
	if (self == NULL)
		return;

	axis_web_storage_stop(self);
	axis_attention_ping_unref(self->attention);
	axis_storage_unref(self->storage);
	g_free(self);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebStorage *self = user_data;

	self->web = web;
	axis_web_storage_register_volume_pages(self, web);
	axis_web_storage_register_root_pages(self, web);
	axis_web_storage_register_dashboard(self, web);
	axis_web_add_attention_source(web, "the disks", attention_source, self);
	axis_attention_ping_unref(self->attention);
	self->attention = axis_web_attention_ref_ping(web);
}

static const AxisJobHandlerInfo handlers[] = {
	/* One attempt: a failed check simply waits for the next turn. */
	{ AXIS_STORAGE_CHECK_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, check_handler,
	  "Checking that the disks are online", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_STORAGE_CHECK_SCHEDULE, AXIS_STORAGE_CHECK_KIND, NULL, AXIS_STORAGE_CHECK_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Disks", "/storage", 14, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_storage_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_STORAGE_MODULE_ID;
		info.name = "Disks";
		info.start = module_start;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Keeps track of the disks (each recognised by a marker file): whether each is "
			"there, its free space and the library folders on it.";
		info.settings = axis_storage_get_setting_table();
	}

	return &info;
}
