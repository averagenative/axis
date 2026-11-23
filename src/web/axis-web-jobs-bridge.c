/*
 * axis-web-jobs-bridge.c - job events to the web hub
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The queue says "job 12 changed" (a progress write, a new state); what
 * the pages need is the job's row.  So an event only marks its job dirty,
 * and at most every AXIS_JOBS_COALESCE_MS the dirty jobs are read from
 * the database in one query on a worker thread and one fragment per row
 * is published.  That is the coalescing: a job whose progress is written
 * a hundred times a second is shown twice a second, the memory held is
 * one id per job, and a burst of events costs one query, not one each.
 *
 * Rows are read rather than built from the events so that a live row and
 * the row of a page loaded a moment later are made by the same code from
 * the same data.
 */

#include "web/axis-web-jobs-private.h"

#include <string.h>

#include "web/axis-web-activity.h"

typedef struct
{
	GArray           *ids;		/* gint64 */
	GPtrArray        *rows;
	AxisJobsActivity  activity;
	gboolean          counted;
	GError           *error;
	gboolean          changed;
	gboolean          overflow;
	gboolean          recount;
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

static gboolean
on_flush(gpointer data);

static gboolean
work_pending(AxisWebJobs *jobs)
{
	return g_hash_table_size(jobs->dirty) > 0 || jobs->overflow || jobs->changed || jobs->recount;
}

/* Arms the next flush, no sooner than a coalescing interval after the
 * last one finished (and a moment from now, so that a burst of events is
 * one batch). */
static void
request_flush(AxisWebJobs *jobs)
{
	gint64 wait_ms;

	if (jobs->flush_id != 0 || jobs->fetching || jobs->stopping)
		return;

	wait_ms = (jobs->last_flush_us + (gint64)AXIS_JOBS_COALESCE_MS * 1000 -
	           g_get_monotonic_time()) / 1000;
	jobs->flush_id = g_timeout_add((guint)MAX(wait_ms, 20), on_flush, jobs);
}

/* ── Publishing ─────────────────────────────────────────────────── */

static void
publish_chunk(
	AxisWebJobs *jobs,
	const gchar *topic,
	GString     *buffer
){
	if (buffer->len == 0)
		return;

	axis_web_hub_publish(jobs->hub, topic, AXIS_JOBS_EVENT_OOB, buffer->str);
	g_string_truncate(buffer, 0);
}

/* The rows, as one event per about AXIS_JOBS_EVENT_CHUNK bytes. */
static void
publish_rows(
	AxisWebJobs *jobs,
	GPtrArray   *rows,
	gboolean     admin
){
	const gchar *topic = admin ? AXIS_JOBS_TOPIC_LIST_ADMIN : AXIS_JOBS_TOPIC_LIST;
	GString *buffer;
	guint i;

	if (axis_web_hub_count_subscribers(jobs->hub, topic) == 0)
		return;

	buffer = g_string_new("");
	for (i = 0; i < rows->len; i++) {
		HtmxNode *node = axis_web_jobs_row_new(g_ptr_array_index(rows, i), admin);
		g_autofree gchar *markup = axis_web_jobs_render_oob(node);

		g_object_unref(node);
		if (buffer->len > 0 && buffer->len + strlen(markup) > AXIS_JOBS_EVENT_CHUNK)
			publish_chunk(jobs, topic, buffer);
		g_string_append(buffer, markup);
	}
	publish_chunk(jobs, topic, buffer);
	g_string_free(buffer, TRUE);
}

static void
publish_details(
	AxisWebJobs *jobs,
	GPtrArray   *rows
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisJobRow *row = g_ptr_array_index(rows, i);
		guint variant;

		for (variant = 0; variant < 2; variant++) {
			g_autofree gchar *topic = axis_web_jobs_topic_for_job(row->id, variant == 1);
			HtmxNode *node;
			g_autofree gchar *markup = NULL;

			if (axis_web_hub_count_subscribers(jobs->hub, topic) == 0)
				continue;

			node = axis_web_jobs_detail_new(row, variant == 1);
			markup = axis_web_jobs_render_oob(node);
			g_object_unref(node);
			axis_web_hub_publish(jobs->hub, topic, AXIS_JOBS_EVENT_OOB, markup);
		}
	}
}

static void
publish_activity(AxisWebJobs *jobs)
{
	HtmxNode *node = axis_web_jobs_activity_new(&jobs->activity);
	g_autofree gchar *markup = axis_web_jobs_render_oob(node);

	g_object_unref(node);
	axis_web_hub_publish(jobs->hub, AXIS_JOBS_TOPIC_ACTIVITY, AXIS_JOBS_EVENT_OOB, markup);
}

/* The pages that show work of the changed jobs' families (a series' page for
 * its searches) are told, once each, and only those someone follows: they
 * ask for their own status lines. */
static void
publish_families(
	AxisWebJobs *jobs,
	GPtrArray   *rows
){
	g_autoptr(GHashTable) told = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisJobRow *row = g_ptr_array_index(rows, i);
		gchar *topic = axis_web_activity_topic(row->key);

		if (topic == NULL || g_hash_table_contains(told, topic)) {
			g_free(topic);
			continue;
		}
		if (axis_web_hub_count_subscribers(jobs->hub, topic) > 0)
			axis_web_hub_publish(jobs->hub, topic, AXIS_WEB_ACTIVITY_EVENT, "changed");
		g_hash_table_add(told, topic);
	}
	/* The Activity page: of any job of work. */
	for (i = 0; i < rows->len; i++) {
		const AxisJobRow *row = g_ptr_array_index(rows, i);

		if (row->upkeep)
			continue;
		if (axis_web_hub_count_subscribers(jobs->hub, AXIS_WEB_ACTIVITY_TOPIC_ALL) > 0)
			axis_web_hub_publish(jobs->hub, AXIS_WEB_ACTIVITY_TOPIC_ALL, AXIS_WEB_ACTIVITY_EVENT, "changed");
		break;
	}
}

/* ── Flushing ───────────────────────────────────────────────────── */

static void
flush_work(
	AxisWebJobs *jobs,
	gpointer     data
){
	Flush *flush = data;

	if (!axis_web_jobs_get_many(jobs->pool, jobs->kinds, flush->ids, &flush->rows, NULL, &flush->error))
		return;
	/* The header's counts change with a job's state (and are read again now
	 * and then: a download's progress is no job's). */
	if (flush->changed || flush->recount || flush->overflow)
		flush->counted = axis_web_jobs_count_activity(jobs->pool, jobs->kinds, &flush->activity, &flush->error);
}

static void
flush_done(
	AxisWebJobs *jobs,
	gpointer     data
){
	Flush *flush = data;

	jobs->fetching = FALSE;
	jobs->last_flush_us = g_get_monotonic_time();
	if (jobs->stopping)
		return;

	if (flush->error != NULL) {
		static gint64 last_warning_us;

		/* The database is away: try again with the next event or the
		 * periodic recount, and say so now and then, not every time. */
		if (jobs->last_flush_us - last_warning_us > 60 * G_USEC_PER_SEC) {
			g_warning("jobs page: cannot read changed jobs: %s", flush->error->message);
			last_warning_us = jobs->last_flush_us;
		}
		jobs->changed = jobs->changed || flush->changed;
		jobs->overflow = TRUE;
		jobs->recount = TRUE;
		return;
	}

	if (flush->counted && memcmp(&flush->activity, &jobs->activity, sizeof(jobs->activity)) != 0) {
		jobs->activity = flush->activity;
		if (jobs->hub != NULL)
			publish_activity(jobs);
	}

	if (jobs->hub != NULL) {
		publish_rows(jobs, flush->rows, FALSE);
		publish_rows(jobs, flush->rows, TRUE);
		publish_details(jobs, flush->rows);
		publish_families(jobs, flush->rows);

		if (flush->changed || flush->overflow) {
			axis_web_hub_publish(jobs->hub, AXIS_JOBS_TOPIC_LIST, AXIS_JOBS_EVENT_CHANGED, "refresh");
			axis_web_hub_publish(jobs->hub, AXIS_JOBS_TOPIC_LIST_ADMIN, AXIS_JOBS_EVENT_CHANGED,
				"refresh");
		}
	}

	if (work_pending(jobs))
		request_flush(jobs);
}

static gboolean
on_flush(gpointer data)
{
	AxisWebJobs *jobs = data;
	Flush *flush = g_new0(Flush, 1);
	GHashTableIter iter;
	gpointer key;

	jobs->flush_id = 0;
	flush->ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	/* A batch; what does not fit stays for the next round. */
	g_hash_table_iter_init(&iter, jobs->dirty);
	while (flush->ids->len < AXIS_JOBS_MAX_BATCH && g_hash_table_iter_next(&iter, &key, NULL)) {
		g_array_append_val(flush->ids, *(gint64 *)key);
		g_hash_table_iter_remove(&iter);
	}

	flush->changed = jobs->changed;
	flush->overflow = jobs->overflow;
	flush->recount = jobs->recount;
	jobs->changed = FALSE;
	jobs->overflow = FALSE;
	jobs->recount = FALSE;

	jobs->fetching = TRUE;
	if (!axis_web_jobs_run(jobs, flush_work, flush_done, flush, flush_free)) {
		/* Too much waiting: the pages will have to fetch their lists. */
		jobs->fetching = FALSE;
		jobs->last_flush_us = g_get_monotonic_time();
		jobs->overflow = TRUE;
		jobs->recount = TRUE;
		request_flush(jobs);
	}

	return G_SOURCE_REMOVE;
}

/* ── Events ─────────────────────────────────────────────────────── */

void
axis_web_jobs_bridge_event(
	AxisWebJobs        *jobs,
	const AxisJobEvent *event
){
	gint64 *key;

	if (jobs->stopping)
		return;

	if (event->type == AXIS_JOB_EVENT_STATE) {
		jobs->changed = TRUE;
		jobs->recount = TRUE;
		/* A job that ended may have begun or ended what a notice is about
		 * (an import, a search, a check of the disks or the clients). */
		if (event->state != AXIS_JOB_STATE_QUEUED && event->state != AXIS_JOB_STATE_RUNNING)
			axis_attention_ping_job_ended(jobs->attention, event->kind);
	}

	if (g_hash_table_size(jobs->dirty) >= AXIS_JOBS_MAX_DIRTY) {
		jobs->overflow = TRUE;
	} else {
		key = g_new(gint64, 1);
		*key = event->id;
		g_hash_table_add(jobs->dirty, key);
	}

	request_flush(jobs);
}

static void
on_job_event(
	AxisJobQueue *queue,
	AxisJobEvent *event,
	gpointer      user_data
){
	axis_web_jobs_bridge_event(user_data, event);
}

static gboolean
on_recount(gpointer data)
{
	AxisWebJobs *jobs = data;

	jobs->recount = TRUE;
	request_flush(jobs);

	return G_SOURCE_CONTINUE;
}

void
axis_web_jobs_bridge_start(AxisWebJobs *jobs)
{
	jobs->event_id = g_signal_connect(jobs->queue, "job-event", G_CALLBACK(on_job_event), jobs);
	jobs->recount_id = g_timeout_add_seconds(AXIS_JOBS_RECOUNT_S, on_recount, jobs);
}

void
axis_web_jobs_bridge_stop(AxisWebJobs *jobs)
{
	if (jobs->event_id != 0 && jobs->queue != NULL)
		g_signal_handler_disconnect(jobs->queue, jobs->event_id);
	jobs->event_id = 0;

	if (jobs->recount_id != 0)
		g_source_remove(jobs->recount_id);
	jobs->recount_id = 0;
	if (jobs->flush_id != 0)
		g_source_remove(jobs->flush_id);
	jobs->flush_id = 0;
}
