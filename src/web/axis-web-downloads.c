/*
 * axis-web-downloads.c - the downloads module: its lifetime, its job, live
 * progress, the dashboard's "Coming in", and the work it hands to threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-downloads-private.h"

#include <string.h>

#include "jobs/axis-job-registry.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-jobs-view.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebDownloads         *owner;
	AxisWebDownloadsWorkFunc  work;
	AxisWebDownloadsDoneFunc  done;
	HtmxRequest              *request;	/* nullable */
	HtmxResponse             *deferred;	/* nullable */
	gpointer                  data;
	GDestroyNotify            free_data;
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
axis_web_downloads_dispatch(
	AxisWebDownloads         *self,
	HtmxRequest              *request,
	AxisWebDownloadsWorkFunc  work,
	AxisWebDownloadsDoneFunc  done,
	gpointer                  data,
	GDestroyNotify            free_data
){
	HtmxResponse *returned = NULL;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_WEB_DOWNLOADS_MAX_PENDING) {
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

/* ── Live progress ──────────────────────────────────────────────── */

typedef struct
{
	GArray    *ids;
	gboolean   set_changed;
	GPtrArray *rows;
	GError    *error;
} Flush;

static void
flush_free(gpointer data)
{
	Flush *flush = data;

	g_array_unref(flush->ids);
	g_clear_pointer(&flush->rows, g_ptr_array_unref);
	g_clear_error(&flush->error);
	g_free(flush);
}

static void
flush_work(
	AxisWebDownloads *self,
	gpointer          data
){
	Flush *flush = data;

	if (flush->ids->len > 0)
		axis_downloads_get_many(self->downloads, flush->ids, &flush->rows, &flush->error);
}

static gboolean on_flush(gpointer data);

/* Arms the next publication, unless one is armed or under way. */
static void
arm_flush(AxisWebDownloads *self)
{
	if (g_atomic_int_get(&self->closed))
		return;
	if (self->flush_id == 0)
		self->flush_id = g_timeout_add(AXIS_WEB_DOWNLOADS_COALESCE_MS, on_flush, self);
}

/* The rows, one event each topic, for whoever follows them. */
static void
publish_rows(
	AxisWebDownloads *self,
	GPtrArray        *rows,
	gboolean          admin
){
	const gchar *topic = admin ? AXIS_DOWNLOADS_TOPIC_ADMIN : AXIS_DOWNLOADS_TOPIC;
	GString *buffer;
	guint i;

	if (axis_web_hub_count_subscribers(self->hub, topic) == 0)
		return;
	buffer = g_string_new(NULL);
	for (i = 0; i < rows->len; i++) {
		const AxisDownloadInfo *info = g_ptr_array_index(rows, i);
		HtmxNode *row;
		g_autofree gchar *markup = NULL;

		/* Only rows in progress live in the table that moves. */
		if (!axis_download_state_is_active(info->state))
			continue;
		row = axis_web_downloads_active_row(info, admin);
		axis_web_table_row_roles(row, axis_web_downloads_active_columns(admin));
		markup = axis_web_jobs_render_oob(row);
		g_object_unref(row);
		if (buffer->len > 0 && buffer->len + strlen(markup) > AXIS_WEB_HUB_MAX_EVENT_BYTES / 2) {
			axis_web_hub_publish(self->hub, topic, AXIS_DOWNLOADS_EVENT_OOB, buffer->str);
			g_string_truncate(buffer, 0);
		}
		g_string_append(buffer, markup);
	}
	if (buffer->len > 0)
		axis_web_hub_publish(self->hub, topic, AXIS_DOWNLOADS_EVENT_OOB, buffer->str);
	g_string_free(buffer, TRUE);
}

/* The pages of the titles whose downloads changed (a series', a film's)
 * are told, once each and only when someone follows them: they ask for
 * their own lines (web/axis-web-activity.h).  So is the Activity page. */
static void
publish_titles(
	AxisWebDownloads *self,
	GPtrArray        *rows
){
	g_autoptr(GHashTable) told = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisDownloadInfo *info = g_ptr_array_index(rows, i);
		gchar *topic;

		if (info->title_id <= 0)
			continue;
		topic = axis_web_activity_title_topic(info->title_id);
		if (g_hash_table_contains(told, topic)) {
			g_free(topic);
			continue;
		}
		if (axis_web_hub_count_subscribers(self->hub, topic) > 0)
			axis_web_hub_publish(self->hub, topic, AXIS_WEB_ACTIVITY_EVENT, "changed");
		g_hash_table_add(told, topic);
	}
	if (rows->len > 0 && axis_web_hub_count_subscribers(self->hub, AXIS_WEB_ACTIVITY_TOPIC_ALL) > 0)
		axis_web_hub_publish(self->hub, AXIS_WEB_ACTIVITY_TOPIC_ALL, AXIS_WEB_ACTIVITY_EVENT, "changed");
}

static HtmxResponse *
flush_done(
	AxisWebDownloads *self,
	HtmxRequest      *request,
	gpointer          data
){
	Flush *flush = data;

	self->flushing = FALSE;
	if (self->stopping || self->hub == NULL)
		return NULL;
	if (flush->error != NULL) {
		g_debug("downloads page: cannot read changed downloads: %s", flush->error->message);
		flush->set_changed = TRUE;
	}
	if (flush->rows != NULL) {
		publish_rows(self, flush->rows, FALSE);
		publish_rows(self, flush->rows, TRUE);
		publish_titles(self, flush->rows);
	}
	if (flush->set_changed) {
		axis_web_hub_publish(self->hub, AXIS_DOWNLOADS_TOPIC, AXIS_DOWNLOADS_EVENT_CHANGED, "refresh");
		axis_web_hub_publish(self->hub, AXIS_DOWNLOADS_TOPIC_ADMIN, AXIS_DOWNLOADS_EVENT_CHANGED, "refresh");
	}
	g_mutex_lock(&self->lock);
	if (g_hash_table_size(self->dirty) > 0 || self->set_changed)
		arm_flush(self);
	g_mutex_unlock(&self->lock);
	return NULL;
}

static gboolean
on_flush(gpointer data)
{
	AxisWebDownloads *self = data;
	Flush *flush;
	GHashTableIter iter;
	gpointer key;

	g_mutex_lock(&self->lock);
	self->flush_id = 0;
	/* One read at a time, and none when the pages have no room. */
	if (self->stopping || self->flushing || self->pending >= AXIS_WEB_DOWNLOADS_MAX_PENDING) {
		if (!self->stopping)
			arm_flush(self);
		g_mutex_unlock(&self->lock);
		return G_SOURCE_REMOVE;
	}
	flush = g_new0(Flush, 1);
	flush->ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_hash_table_iter_init(&iter, self->dirty);
	while (flush->ids->len < 200 && g_hash_table_iter_next(&iter, &key, NULL)) {
		g_array_append_val(flush->ids, *(gint64 *)key);
		g_hash_table_iter_remove(&iter);
	}
	flush->set_changed = self->set_changed;
	self->set_changed = FALSE;
	g_mutex_unlock(&self->lock);

	/* Room was checked above, on this thread: it is dispatched. */
	self->flushing = TRUE;
	axis_web_downloads_dispatch(self, NULL, flush_work, flush_done, flush, flush_free);
	return G_SOURCE_REMOVE;
}

static gboolean
arm_from_any_thread(gpointer data)
{
	AxisWebDownloads *self = data;

	g_mutex_lock(&self->lock);
	if (!self->stopping)
		arm_flush(self);
	g_mutex_unlock(&self->lock);
	return G_SOURCE_REMOVE;
}

/* The layer's listener: any thread; only notes and wakes the main loop. */
static void
on_downloads_changed(
	GArray   *ids,
	gboolean  set_changed,
	gpointer  user_data
){
	AxisWebDownloads *self = user_data;
	gboolean wake;
	guint i;

	if (g_atomic_int_get(&self->closed) || self->hub == NULL)
		return;
	g_mutex_lock(&self->lock);
	for (i = 0; i < ids->len && g_hash_table_size(self->dirty) < 10000; i++) {
		gint64 *key = g_new(gint64, 1);

		*key = g_array_index(ids, gint64, i);
		g_hash_table_add(self->dirty, key);
	}
	if (ids->len > 0 && g_hash_table_size(self->dirty) >= 10000)
		set_changed = TRUE;
	self->set_changed = self->set_changed || set_changed;
	wake = self->flush_id == 0;
	g_mutex_unlock(&self->lock);
	if (wake)
		g_idle_add(arm_from_any_thread, self);
}

gboolean
axis_web_downloads_any_builtin(GPtrArray *downloads)
{
	guint i;

	for (i = 0; downloads != NULL && i < downloads->len; i++) {
		const AxisDownloadInfo *info = g_ptr_array_index(downloads, i);

		/* The one built-in kind's protocol (axis_downloads_add_kind()). */
		if (info->protocol == AXIS_DOWNLOAD_YOUTUBE)
			return TRUE;
	}
	return FALSE;
}

/* ── The dashboard: what is coming in ───────────────────────────── */

typedef struct
{
	guint      clients;
	GPtrArray *active;
	GPtrArray *arrived;
	GPtrArray *recent;
	GPtrArray *client_list;
	gint64     taken_us;
} DashFigures;

static void
dash_free(gpointer data)
{
	DashFigures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->active, g_ptr_array_unref);
	g_clear_pointer(&figures->arrived, g_ptr_array_unref);
	g_clear_pointer(&figures->recent, g_ptr_array_unref);
	g_clear_pointer(&figures->client_list, g_ptr_array_unref);
	g_free(figures);
}

static gpointer
dash_collect(gpointer user_data)
{
	AxisWebDownloads *self = user_data;
	DashFigures *figures = g_new0(DashFigures, 1);
	g_autoptr(GError) error = NULL;

	figures->taken_us = g_get_real_time();
	if (!axis_downloads_list_clients(self->downloads, &figures->client_list, &error) ||
	    !axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_ACTIVE, 6, &figures->active, &error) ||
	    !axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_ARRIVED, 4, &figures->arrived, &error) ||
	    !axis_downloads_list(self->downloads, AXIS_DOWNLOAD_LIST_RECENT, 50, &figures->recent, &error)) {
		g_debug("dashboard: cannot read the downloads: %s", error->message);
		dash_free(figures);
		return NULL;
	}
	figures->clients = figures->client_list->len;
	return figures;
}

static HtmxNode *
coming_item(
	const AxisDownloadInfo *info,
	gboolean                active
){
	HtmxLi *item = htmx_li_new();
	HtmxSpan *head = htmx_span_new();
	HtmxA *link = htmx_a_new_with_href("/downloads");
	HtmxBdi *name = htmx_bdi_new();
	HtmxSpan *figures = htmx_span_new();
	g_autofree gchar *text = NULL;
	gdouble progress = axis_download_info_progress(info);

	htmx_element_add_class(HTMX_ELEMENT(item), "space-item");
	htmx_element_add_class(HTMX_ELEMENT(head), "space-name");
	ADD(name, axis_web_breakable(axis_web_downloads_label(info)));
	ADD(link, name);
	ADD(head, link);
	if (active && info->stalled)
		ADD(head, axis_web_browser_badge("Stalled", "badge-warn"));
	if (active && info->problem != NULL)
		ADD(head, axis_web_browser_badge("Waiting", "badge-warn"));
	ADD(item, head);
	htmx_element_add_class(HTMX_ELEMENT(figures), "space-figures");
	if (active) {
		g_autofree gchar *size = info->size_bytes > 0 ? axis_size_format((guint64)info->size_bytes) : NULL;

		if (progress >= 0 && size != NULL)
			text = g_strdup_printf("%.0f%% of %s, %s", progress * 100.0, size, info->client_name);
		else
			text = g_strdup_printf("%s, %s", axis_download_state_label(info->state), info->client_name);
	} else {
		g_autofree gchar *age = axis_web_downloads_age_text(info->completed_us > 0 ? info->completed_us :
			info->updated_us);

		text = g_strdup_printf("arrived %s", age);
	}
	htmx_node_set_text_content(HTMX_NODE(figures), text);
	ADD(item, figures);
	if (active)
		ADD(item, axis_web_downloads_bar(progress >= 0 ? progress : 0.0));
	return HTMX_NODE(item);
}

static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	DashFigures *figures = data;
	HtmxDiv *piece;
	guint i;

	/* No client: no section at all.  The built-in client (YouTube) is not
	 * one an administrator sets up, so it counts when it has downloads. */
	if (figures == NULL || (figures->clients == 0 && !axis_web_downloads_any_builtin(figures->active) &&
	    !axis_web_downloads_any_builtin(figures->arrived) && !axis_web_downloads_any_builtin(figures->recent)))
		return;
	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	if (figures->active->len == 0) {
		ADD(piece, axis_web_storage_text("help", "Nothing is downloading."));
	} else {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "space-list");
		for (i = 0; i < figures->active->len && i < 5; i++)
			ADD(list, coming_item(g_ptr_array_index(figures->active, i), TRUE));
		ADD(piece, list);
		if (figures->active->len > 5)
			ADD(piece, axis_web_browser_link("/downloads", "More on the Downloads page"));
	}
	if (figures->arrived->len > 0) {
		HtmxP *head = htmx_p_new();
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(head), "dash-subhead");
		htmx_node_set_text_content(HTMX_NODE(head), "Arrived lately");
		ADD(piece, head);
		htmx_element_add_class(HTMX_ELEMENT(list), "space-list");
		for (i = 0; i < figures->arrived->len && i < 3; i++)
			ADD(list, coming_item(g_ptr_array_index(figures->arrived, i), FALSE));
		ADD(piece, list);
	}
	ADD(piece, axis_web_dashboard_as_of("As of", figures->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_COMING_IN, 10, "/downloads", HTMX_NODE(piece));

	/* What needs a person (a client not working, finished downloads that
	 * cannot be imported, what is left in the download folders) is the
	 * attention sources' to say, below: the dashboard's card is drawn from
	 * them.  "Downloads failed today" is no longer a line of it: a failed
	 * download is put on the blocklist and the next release searched for by
	 * itself, and when none is left its movie or series needs attention. */
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The download clients' notices (add-attention-notices), from one statement
 * over the handful of rows of download_client.
 *
 *   refused       the client refused what Axis has (its password, its key):
 *                 only a person ends it, so it is a notice at once.
 *   unreachable,  the client is polled every minute and usually answers
 *   failing       again by itself (a restart, an update), so it is a notice
 *                 only once it has not been working for
 *                 AXIS_ATTENTION_CLIENT_WAIT_S, by not_working_since: set by
 *                 the contact that first found it so and kept whatever the
 *                 reason becomes (not answering, then failing, is one
 *                 outage), begun again when the client is switched on.  A
 *                 row that does not say since when is not judged.
 *
 * A client switched off with nothing in progress is left out, as on the
 * dashboard.  "newest" is when it stopped working.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebDownloads *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_query_bounded(axis_downloads_get_pool(self->downloads),
	        "SELECT c.id, c.name, c.state, coalesce(floor(extract(epoch FROM c.not_working_since))::bigint, 0)"
	        " FROM download_client c WHERE c.address <> '' AND c.state IN ('unreachable', 'refused', 'failing')"
	        " AND (c.enabled OR EXISTS (SELECT 1 FROM download d WHERE d.client_id = c.id"
	        "  AND d.state IN ('sending', 'queued', 'downloading', 'paused')))"
	        " ORDER BY c.id", reading->deadline_ms, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *name = axis_db_result_get_text(result, i, 1);
		const gchar *state = axis_db_result_get_text(result, i, 2);
		g_autofree gchar *subject = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;
		gint64 id = 0;
		gint64 since = 0;
		gint64 for_us;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 3, &since);
		for_us = reading->now_us - since * G_USEC_PER_SEC;
		if (g_strcmp0(state, "refused") == 0) {
			tail = g_strdup(" refused the password or key Axis has for it.");
		} else if (since > 0 && for_us >= (gint64)AXIS_ATTENTION_CLIENT_WAIT_S * G_USEC_PER_SEC) {
			g_autofree gchar *how_long = axis_attention_duration(for_us);

			tail = g_strdup_printf(g_strcmp0(state, "unreachable") == 0 ? " has not answered for %s." :
				" has been failing for %s.", how_long);
		} else {
			/* Axis is still trying: usually it mends itself within minutes. */
			continue;
		}
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		href = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, id);
		item = axis_attention_item_new("client.failing", subject, AXIS_ATTENTION_BROKEN, 1, since);
		axis_attention_item_say(item, "The download client ", name != NULL ? name : "", tail);
		axis_attention_item_link(item, href, "Open the download client");
		item->since_us = since * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/*
 * Finished downloads that cannot be imported yet (slice N2): the client says
 * they are complete, and Axis cannot find them where the client says they
 * are.  The row keeps why as words, one of about a dozen causes
 * (axis_download_locate_full(): no folder mapping, a mapping that leads
 * nowhere, a symbolic link, another filesystem, a client that gives no path),
 * so the sentence names none: it says how many and sends the person to the
 * Downloads page, which shows each one's reason.  One item for each client,
 * by one statement over the handful of clients and the partial index
 * download_active; "newest" is the highest id among them.
 *
 * A client that is still moving the finished file to its place gives the
 * same state for a moment and ends it by itself, so a download counts only
 * once it has been so for AXIS_ATTENTION_STUCK_WAIT_S.  Nothing records when
 * the problem began; what is kept is when the download last grew
 * (progress_at), which is when it finished, and the wait counts from that.
 *
 * Not known, which is neither a notice nor its end
 * (axis_attention_not_known()): every client's while any disk is offline or
 * not recognised (a disk that is away is one of the causes and has its own
 * notice; the two cannot be told apart in the row), and one client's while
 * that client is not working (its downloads are not polled; it has its own
 * notice).  The notice is then as it was, dismissed or not, until the disk
 * or the client is back.
 */
static gboolean
stuck_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebDownloads *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	guint i;

	sql = g_strdup_printf("SELECT c.id, c.name, count(d.id), coalesce(max(d.id), 0),"
		" c.state IN ('unreachable', 'refused', 'failing'),"
		" EXISTS (SELECT 1 FROM volume v WHERE v.state IN ('offline', 'mismatch'))"
		" FROM download_client c LEFT JOIN download d ON d.client_id = c.id"
		"  AND d.state IN ('queued', 'downloading', 'paused') AND d.client_path IS NOT NULL AND d.problem IS NOT NULL"
		"  AND coalesce(d.progress_at, d.created_at) <= to_timestamp(%" G_GINT64_FORMAT ")"
		" GROUP BY c.id, c.name, c.state ORDER BY c.id",
		reading->now_us / G_USEC_PER_SEC - AXIS_ATTENTION_STUCK_WAIT_S);
	if (!axis_db_pool_query_bounded(axis_downloads_get_pool(self->downloads), sql, reading->deadline_ms, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *name = axis_db_result_get_text(result, i, 1);
		g_autofree gchar *subject = NULL;
		g_autofree gchar *text = NULL;
		AxisAttentionItem *item;
		gboolean client_away = FALSE;
		gboolean disk_away = FALSE;
		gint64 id = 0;
		gint64 n = 0;
		gint64 newest = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 2, &n);
		axis_db_result_get_int64(result, i, 3, &newest);
		axis_db_result_get_boolean(result, i, 4, &client_away);
		axis_db_result_get_boolean(result, i, 5, &disk_away);
		if (disk_away) {
			axis_attention_not_known(items, "download.stuck", NULL);
			return TRUE;
		}
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		if (client_away) {
			axis_attention_not_known(items, "download.stuck", subject);
			continue;
		}
		if (n <= 0)
			continue;
		text = n == 1 ? g_strdup("1 finished download at the download client ") :
			g_strdup_printf("%" G_GINT64_FORMAT " finished downloads at the download client ", n);
		item = axis_attention_item_new("download.stuck", subject, AXIS_ATTENTION_WAITING, (guint64)n, newest);
		axis_attention_item_say(item, text, name != NULL ? name : "", n == 1 ?
			" cannot be imported yet. The Downloads page says why." :
			" cannot be imported yet. The Downloads page says why for each.");
		axis_attention_item_link(item, "/downloads", "Open Downloads");
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/*
 * What is left in the download folders and was never imported (slice N2):
 * the number the hourly import cleanup leaves on each client's row
 * (leftover_count), summed.  It uses space and stops nothing, so it is
 * "Worth a look"; only a person deletes them.  Nothing says which are new, so
 * "newest" is 0: a dismissal lasts until there are more, or none.
 */
static gboolean
leftovers_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebDownloads *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;

	if (!axis_db_pool_query_bounded(axis_downloads_get_pool(self->downloads),
	        "SELECT coalesce(sum(leftover_count), 0)::bigint FROM download_client WHERE leftover_count > 0",
	        reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &n);
	if (n <= 0)
		return TRUE;
	text = n == 1 ? g_strdup("1 file or folder is left in the download folders and was never imported.") :
		g_strdup_printf("%" G_GINT64_FORMAT " files and folders are left in the download folders and were never "
			"imported.", n);
	item = axis_attention_item_new("download.leftovers", "", AXIS_ATTENTION_LOOK, (guint64)n, 0);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/downloads/leftovers", "Open Leftovers in the download folders");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
track_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebDownloads *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	guint run_ms = 0;

	/* A shorter run, if asked (a test); never longer than the default. */
	if (payload != NULL && json_object_has_member(payload, "run_ms")) {
		JsonNode *node = json_object_get_member(payload, "run_ms");

		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64) {
			gint64 value = json_node_get_int(node);

			if (value > 0 && value < AXIS_DOWNLOAD_TRACK_RUN_S * 1000)
				run_ms = (guint)value;
		}
	}
	return axis_downloads_run_tracking(self->downloads, context, run_ms, error);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebDownloads *self = user_data;

	axis_web_add_dashboard_part(web, dash_collect, dash_render, dash_free, self);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_COMING_IN);
	axis_web_add_attention_source(web, "the download clients", attention_source, self);
	axis_web_add_attention_source(web, "the finished downloads", stuck_source, self);
	axis_web_add_attention_source(web, "the download folders' leftovers", leftovers_source, self);
	axis_web_downloads_register_pages(self, web);
	axis_web_downloads_register_clients(self, web);
}

static const AxisJobHandlerInfo handlers[] = {
	/* One attempt: a run that fails is simply followed by the next. */
	{ AXIS_DOWNLOAD_TRACK_KIND, AXIS_DOWNLOAD_QUEUE, 1, track_handler,
	  "Checking the download clients", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_DOWNLOAD_TRACK_SCHEDULE, AXIS_DOWNLOAD_TRACK_KIND, NULL, AXIS_DOWNLOAD_TRACK_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Downloads", "/downloads", 5, AXIS_NAV_GROUP_ACTIVITY, FALSE, NULL, 0 },
	{ "Download clients", "/downloads/clients", 35, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_downloads_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_DOWNLOAD_MODULE_ID;
		info.name = "Downloads";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Drives the download clients that are already running (SABnzbd, Deluge, qBittorrent): hands them "
			"releases once, follows what Axis added and only that, and records where each finished download's "
			"files are on a disk. API keys and passwords go only to their own download client.";
		info.settings = axis_downloads_get_setting_table();
	}
	return &info;
}

AxisWebDownloads *
axis_web_downloads_new(
	AxisDownloads *downloads,
	AxisStorage   *storage,
	AxisWebHub    *hub
){
	AxisWebDownloads *self;

	g_return_val_if_fail(downloads != NULL && storage != NULL, NULL);

	self = g_new0(AxisWebDownloads, 1);
	self->downloads = axis_downloads_ref(downloads);
	self->storage = axis_storage_ref(storage);
	self->hub = hub;
	g_mutex_init(&self->lock);
	self->dirty = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	axis_downloads_set_listener(downloads, on_downloads_changed, self);
	return self;
}

void
axis_web_downloads_stop(AxisWebDownloads *self)
{
	g_return_if_fail(self != NULL);

	/* Once this returns the listener is never called again. */
	g_atomic_int_set(&self->closed, 1);
	axis_downloads_set_listener(self->downloads, NULL, NULL);
	g_mutex_lock(&self->lock);
	self->stopping = TRUE;
	if (self->flush_id != 0) {
		g_source_remove(self->flush_id);
		self->flush_id = 0;
	}
	g_mutex_unlock(&self->lock);
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
	/* An idle a listener queued before it was removed. */
	while (g_idle_remove_by_data(self))
		;
}

void
axis_web_downloads_free(AxisWebDownloads *self)
{
	if (self == NULL)
		return;
	axis_web_downloads_stop(self);
	g_hash_table_unref(self->dirty);
	g_mutex_clear(&self->lock);
	axis_storage_unref(self->storage);
	axis_downloads_unref(self->downloads);
	g_free(self);
}
