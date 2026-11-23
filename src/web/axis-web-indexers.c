/*
 * axis-web-indexers.c - the indexers module: its lifetime, its job and the
 * work it hands to threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-indexers-private.h"

#include <string.h>

#include "indexer/axis-indexer-cardigann.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web-auth-private.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebIndexers         *owner;
	AxisWebIndexersWorkFunc  work;
	AxisWebIndexersDoneFunc  done;
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
axis_web_indexers_dispatch(
	AxisWebIndexers         *self,
	HtmxRequest             *request,
	AxisWebIndexersWorkFunc  work,
	AxisWebIndexersDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_WEB_INDEXERS_MAX_PENDING) {
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

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
expire_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebIndexers *self = user_data;
	guint n = 0;

	if (!axis_indexers_expire(self->indexers, &n, error))
		return FALSE;
	if (n > 0)
		g_debug("indexers: forgot %u searches whose life had ended", n);
	return TRUE;
}

static gboolean
caps_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebIndexers *self = user_data;
	guint n = 0;

	if (!axis_indexers_refresh_caps(self->indexers, axis_job_context_get_cancellable(context), &n, error))
		return FALSE;
	if (n > 0)
		g_debug("indexers: read the capabilities of %u indexers again", n);
	return TRUE;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * Whether the row s of indexer_source is held by Axis now: an indexer run
 * from a site definition whose pages no longer look as the definition
 * expects, or that answers with a browser challenge.  The hold has an end
 * time (an hour), after which a search asks the indexer again and, the site
 * being as it was, holds it again: the time passing ends nothing.  So an
 * indexer counts as held from the hold until a contact works, whatever
 * held_until says: the hold's words stay in the row until then (the contact
 * that works forgets a hold whose time has passed,
 * axis_indexer_store_source_state(); a person's test that passes, or a new
 * definition, lifts it at once).  A notice that ended for the minutes between
 * the hold's end and the next search would come back undismissed every hour.
 * (An indexer held because it asked Axis to wait, or whose allowance is used
 * up, ends by itself and is none of these.)
 *
 * The words are the ones axis_indexer_hold_cause() reads, taken from the
 * same macros: written as dollar-quoted constants, so that the apostrophe in
 * them needs no doubling and the statement cannot come to differ from them.
 */
#define WORDS_SQL(words) "$w$" words "$w$"
#define ENDS_WITH_SQL(text, words) "right(" text ", length(" WORDS_SQL(words) ")) = " WORDS_SQL(words)
#define DRIFT_SQL \
	"(s.definition_id IS NOT NULL AND ((s.hold_message IS NOT NULL" \
	"  AND " ENDS_WITH_SQL("s.hold_message", AXIS_INDEXER_HOLD_DRIFT_WORDS) ")" \
	" OR (s.state = 'failing' AND position(" WORDS_SQL(AXIS_INDEXER_HOLD_DRIFT_WORDS) " IN" \
	"  coalesce(s.state_message, '')) > 0)))"
#define CHALLENGE_SQL \
	"(s.definition_id IS NOT NULL AND s.hold_message IS NOT NULL" \
	" AND " ENDS_WITH_SQL("s.hold_message", AXIS_INDEXER_HOLD_CHALLENGE_WORDS) ")"
#define HELD_BY_AXIS_SQL "(" DRIFT_SQL " OR " CHALLENGE_SQL ")"

/* One indexer that is not working, as the statement below gave it. */
typedef struct
{
	gint64       id;
	const gchar *name;	/* the result's */
	const gchar *state;
	gint64       since;	/* seconds since 1970; 0: its row does not say */
} Failing;

/*
 * The indexers' notices (add-attention-notices), from one statement over the
 * handful of switched-on rows of indexer_source.
 *
 *   refused       the indexer refused its key: only a person ends it, so it
 *                 is a notice at once.
 *   unreachable,  an indexer is contacted only when it is used, so its state
 *   failing       may be one contact's bad luck.  It is a notice once it has
 *                 not been working for AXIS_ATTENTION_INDEXER_WAIT_S (by
 *                 not_working_since: set by the contact that first found it
 *                 so, kept whatever the reason becomes, begun again when the
 *                 indexer is switched on) and a later contact found it so
 *                 too (last_checked_at is later than not_working_since: the
 *                 first failed contact writes both at once, a later one
 *                 moves only last_checked_at).
 *
 * An indexer asked to wait or whose allowance is used up is in none of these
 * states and has no notice: that ends by itself.  The level is "Worth a
 * look" while another switched-on indexer works, "Not working" when none
 * does.  More than AXIS_ATTENTION_MAX_OF_A_KIND are one notice with their
 * number.  "newest" is when the state began.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebIndexers *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GArray) failing = g_array_new(FALSE, TRUE, sizeof(Failing));
	AxisAttentionLevel level;
	gboolean one_works = FALSE;
	gint64 newest = 0;
	guint i;

	if (!axis_db_pool_query_bounded(axis_indexers_get_pool(self->indexers),
	        "SELECT s.id, s.name, s.state, coalesce(floor(extract(epoch FROM s.not_working_since))::bigint, 0),"
	        " coalesce(s.last_checked_at > s.not_working_since, false), " HELD_BY_AXIS_SQL
	        " FROM indexer_source s WHERE s.enabled ORDER BY s.id", reading->deadline_ms, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *state = axis_db_result_get_text(result, i, 2);
		gboolean again = FALSE;
		gboolean held = FALSE;
		Failing one;

		memset(&one, 0, sizeof(one));
		/* Held by Axis (its pages changed, or a browser challenge): that is
		 * its notice (held_source() below), at once, and this one's wait and
		 * second contact are not asked of it. */
		axis_db_result_get_boolean(result, i, 5, &held);
		if (held)
			continue;
		if (g_strcmp0(state, "ok") == 0) {
			one_works = TRUE;
			continue;
		}
		axis_db_result_get_int64(result, i, 0, &one.id);
		axis_db_result_get_int64(result, i, 3, &one.since);
		axis_db_result_get_boolean(result, i, 4, &again);
		one.name = axis_db_result_get_text(result, i, 1);
		one.state = state;
		if (g_strcmp0(state, "refused") == 0) {
			g_array_append_val(failing, one);
		} else if ((g_strcmp0(state, "unreachable") == 0 || g_strcmp0(state, "failing") == 0) && again &&
		           one.since > 0 && reading->now_us - one.since * G_USEC_PER_SEC >=
		           (gint64)AXIS_ATTENTION_INDEXER_WAIT_S * G_USEC_PER_SEC) {
			g_array_append_val(failing, one);
		}
	}
	if (failing->len == 0)
		return TRUE;
	level = one_works ? AXIS_ATTENTION_LOOK : AXIS_ATTENTION_BROKEN;

	if (failing->len > AXIS_ATTENTION_MAX_OF_A_KIND) {
		g_autofree gchar *text = g_strdup_printf(one_works ? "%u indexers are not working. The others still answer." :
			"%u indexers are not working, and none of the others answers: nothing can be searched for.",
			failing->len);
		AxisAttentionItem *item;

		for (i = 0; i < failing->len; i++)
			newest = MAX(newest, g_array_index(failing, Failing, i).since);
		item = axis_attention_item_new("indexer.failing", "", level, failing->len, newest);
		axis_attention_item_say(item, text, NULL, NULL);
		axis_attention_item_link(item, "/indexers", "Open Indexers");
		g_ptr_array_add(items, item);
		return TRUE;
	}

	for (i = 0; i < failing->len; i++) {
		const Failing *one = &g_array_index(failing, Failing, i);
		g_autofree gchar *subject = g_strdup_printf("%" G_GINT64_FORMAT, one->id);
		g_autofree gchar *href = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, one->id);
		g_autofree gchar *how_long = axis_attention_duration(reading->now_us - one->since * G_USEC_PER_SEC);
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;

		if (g_strcmp0(one->state, "refused") == 0)
			tail = g_strdup(" refused its API key.");
		else
			tail = g_strdup_printf(g_strcmp0(one->state, "unreachable") == 0 ? " has not answered for %s." :
				" has been failing for %s.", how_long);
		item = axis_attention_item_new("indexer.failing", subject, level, 1, one->since);
		axis_attention_item_say(item, "The indexer ", one->name != NULL ? one->name : "", tail);
		axis_attention_item_link(item, href, "Open the indexer");
		item->since_us = one->since * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/*
 * The indexers Axis holds (slice N2; see HELD_BY_AXIS_SQL): one item each,
 * at once.  A state that also says the pages changed (the state of the last
 * contact) is the same thing, said the day it happens, as the dashboard's
 * card did.  "newest" is 0: a hold is renewed at every contact, and that is
 * not a new occurrence.
 *
 * The level is "Worth a look" while another switched-on indexer can be
 * searched (it is not held, and its last contact worked or it was not
 * contacted yet), and "Not working" when none can: every one is held, or
 * not answering, refused or failing, so nothing can be searched for, and the
 * sentence says so.  One statement over the switched-on rows gives both.
 */
static gboolean
held_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebIndexers *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	gboolean one_works = FALSE;
	guint i;

	if (!axis_db_pool_query_bounded(axis_indexers_get_pool(self->indexers),
	        "SELECT s.id, s.name, " DRIFT_SQL ", " HELD_BY_AXIS_SQL ", s.state IN ('ok', 'unchecked')"
	        " FROM indexer_source s WHERE s.enabled ORDER BY s.id", reading->deadline_ms, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result) && !one_works; i++) {
		gboolean held = FALSE;
		gboolean usable = FALSE;

		axis_db_result_get_boolean(result, i, 3, &held);
		axis_db_result_get_boolean(result, i, 4, &usable);
		one_works = !held && usable;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *name = axis_db_result_get_text(result, i, 1);
		g_autofree gchar *subject = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;
		gboolean drift = FALSE;
		gboolean held = FALSE;
		gint64 id = 0;

		axis_db_result_get_boolean(result, i, 3, &held);
		if (!held)
			continue;
		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_boolean(result, i, 2, &drift);
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		href = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
		tail = g_strdup_printf("%s%s", drift ?
			": its pages no longer look as its site definition expects. Test it, or install a newer definition." :
			": it answers with a browser challenge Axis cannot pass. Test it once the site lets programs in again.",
			one_works ? "" : " No other indexer can be searched either: nothing can be searched for.");
		item = axis_attention_item_new("indexer.held", subject, one_works ? AXIS_ATTENTION_LOOK : AXIS_ATTENTION_BROKEN,
			1, 0);
		axis_attention_item_say(item, "Axis does not ask the indexer ", name != NULL ? name : "", tail);
		axis_attention_item_link(item, href, "Open the indexer");
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_attention_source(web, "the indexers", attention_source, user_data);
	axis_web_add_attention_source(web, "the indexers Axis holds", held_source, user_data);
	axis_web_indexers_register_pages(user_data, web);
}

/*
 * The definitions Axis ships, written to the database (and the overrides an
 * administrator placed in the data directory taken).  An indexer runs the
 * compiled-in copy whatever its row holds, so a failure here costs only the
 * pages' listing until the next start: it is logged, not fatal.
 */
static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	AxisWebIndexers *self = user_data;
	g_autoptr(GError) local = NULL;

	if (!axis_indexers_sync_shipped(self->indexers, self->definitions_dir, &local))
		g_warning("indexers: the built-in definitions could not be written: %s", local->message);
	return TRUE;
}

static const AxisJobHandlerInfo handlers[] = {
	/* On the maintenance queue: a quick delete, and no queue of its own. */
	{ AXIS_INDEXER_EXPIRE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, expire_handler,
	  "Clearing old indexer search results", AXIS_JOB_CLASS_UPKEEP },
	/* Also on the maintenance queue: one small request per indexer, a day
	 * apart at most. */
	{ AXIS_INDEXER_CAPS_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, caps_handler,
	  "Reading what the indexers can search by", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_INDEXER_EXPIRE_SCHEDULE, AXIS_INDEXER_EXPIRE_KIND, NULL, AXIS_INDEXER_EXPIRE_INTERVAL_S },
	{ AXIS_INDEXER_CAPS_SCHEDULE, AXIS_INDEXER_CAPS_KIND, NULL, AXIS_INDEXER_CAPS_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Indexers", "/indexers", 36, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_indexers_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_INDEXER_MODULE_ID;
		info.name = "Indexers";
		info.start = module_start;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Finds releases: asks Usenet indexers (Newznab) and torrent feeds (Torznab) directly "
			"with your own keys, and Prowlarr if you connect it, all at once, by identifier or by words; keeps a "
			"search's releases for a short while, and fetches a release only from an indexer that returned it. Each "
			"key goes only to its own indexer's address.";
		info.settings = axis_indexers_get_setting_table();
	}
	return &info;
}

AxisWebIndexers *
axis_web_indexers_new(AxisIndexers *indexers)
{
	AxisWebIndexers *self;

	g_return_val_if_fail(indexers != NULL, NULL);

	self = g_new0(AxisWebIndexers, 1);
	self->indexers = axis_indexers_ref(indexers);
	return self;
}

void
axis_web_indexers_set_definitions_dir(
	AxisWebIndexers *self,
	const gchar     *path
){
	g_return_if_fail(self != NULL);

	g_free(self->definitions_dir);
	self->definitions_dir = g_strdup(path);
}

void
axis_web_indexers_stop(AxisWebIndexers *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_indexers_free(AxisWebIndexers *self)
{
	if (self == NULL)
		return;
	axis_web_indexers_stop(self);
	axis_indexers_unref(self->indexers);
	g_free(self->definitions_dir);
	g_free(self);
}
