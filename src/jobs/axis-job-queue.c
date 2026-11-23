/*
 * axis-job-queue.c - runs jobs in-process
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Structure.  Everything the threads share lives in a Core, which is
 * reference counted separately from the AxisJobQueue object: the object
 * holds one reference and every thread holds one until its last
 * instruction.  So a thread that stop() gave up waiting for can still
 * read and lock the Core safely after the object is gone; what it must
 * not touch any more is the pool (see "Abandoned threads" below) and the
 * object (it never does: events reach the object through a GWeakRef on
 * the main thread).
 *
 * Locking.  core->lock guards the fields marked (core) and every Lane's
 * and Slot's mutable fields.  A job context has its own lock (see
 * axis-job-private.h); the order is core->lock, then a context's lock.
 * No database call, handler call, signal emission or g_cancellable_cancel()
 * happens with either held.
 *
 * Who decides what happens to a running job.  Every decision about a job
 * whose handler has been called is taken under its context's lock, from
 * three flags: handler_done (set by the worker when the handler returns),
 * lease_lost (set by the keeper when a heartbeat is refused) and abandoned
 * (set by the keeper when the shutdown grace period ends, only while
 * handler_done is still FALSE).  The worker reads all three when it sets
 * handler_done, so for each job exactly one of these happens:
 *
 *  - abandoned: the keeper hands the job back; the worker, whenever its
 *    handler returns, discards the result without a database call;
 *  - lease lost: the worker discards the result (the job is someone
 *    else's now);
 *  - otherwise the worker records the result with the lease token.  If
 *    the lease was lost after the last heartbeat, that statement matches
 *    no row (AXIS_JOB_ERROR_LEASE_LOST) and the result is discarded then.
 *
 * Events only ever come from the database's notifications, which are
 * sent by the same statement as the change, so no event announces a
 * change that did not happen.
 *
 * Abandoned threads.  stop() waits a bounded time.  A worker whose handler
 * is still running when the grace period ends is marked abandoned by the
 * keeper; any thread still not finished when stop() gives up is marked
 * abandoned by stop().  An abandoned thread finishes the call it is in
 * (handler or database), then makes no further database call: it updates
 * the Core under its lock, logs, and exits, dropping its references.
 */

#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-private.h"
#include "jobs/axis-job-schedule.h"
#include "db/axis-db-listener.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define US_PER_MS 1000

/* The dispatcher checks its idle connection this often, and gives each
 * connection attempt and round trip this long: less than stop() waits for
 * it (DISPATCHER_STOP_US), so a database that has stopped answering does
 * not keep it past that. */
#define LISTENER_PING_US        (30 * G_USEC_PER_SEC)
#define LISTENER_DEADLINE_US    (2 * G_USEC_PER_SEC)
#define LISTENER_BACKOFF_MAX_MS 30000

/* At the end of stop(), how long the dispatcher gets to deliver the last
 * notifications. */
#define DISPATCHER_STOP_US      (3 * G_USEC_PER_SEC)

/* A claim, heartbeat or sweep that failed is retried this soon at most. */
#define RETRY_US                (G_USEC_PER_SEC)

/* Events waiting for the main loop beyond this many are dropped. */
#define MAX_PENDING_EVENTS      10000

/* Events delivered per main-loop dispatch, so a flood cannot starve it. */
#define EVENTS_PER_DISPATCH     256

#define BARRIER_PREFIX          "axis-job-barrier:"

/* How a log message names one run of a job. */
#define RUN_FORMAT              "job %" G_GINT64_FORMAT " (%s, attempt %u of %u)"
#define RUN_ARGS(c)             (c)->id, (c)->kind, (c)->attempt, (c)->max_attempts

typedef struct _Core Core;

typedef enum
{
	ROLE_WORKER,
	ROLE_KEEPER,
	ROLE_MAINTENANCE,
	ROLE_DISPATCHER
} Role;

/* One queue name, and its workers' shared state. */
typedef struct
{
	gchar    *name;
	guint     concurrency;
	gchar   **kinds;	/* the kinds claimed from it */
	GCond     cond;	/* (core) idle workers wait here */
	gboolean  pending;	/* (core) there may be a claimable job */
	gint64    next_poll;	/* (core) monotonic */
	gboolean  failing;	/* (core) the last claim failed */
} Lane;

/* One thread. */
typedef struct
{
	Core           *core;
	Role            role;
	Lane           *lane;	/* workers */
	gchar          *name;	/* worker id, for workers */
	GThread        *thread;	/* (core) until joined or left to itself */
	gboolean        exited;	/* (core) */
	gboolean        abandoned;	/* (core) stop() no longer waits for it */
	gboolean        busy;	/* (core) from claim to recorded result */
	AxisJobContext *job;	/* (core) the job it runs, with a reference */
} Slot;

struct _Core
{
	gint              ref_count;	/* atomic */
	GMutex            lock;
	GCond             cond;		/* keeper, maintenance, stop(): any change */

	AxisDbPool       *pool;		/* borrowed; see axis_job_queue_stop() */
	AxisJobRegistry  *registry;
	GWeakRef          owner;		/* the AxisJobQueue */

	/* Settings: changed only before start, read-only after */
	GHashTable       *concurrency;	/* queue name -> GUINT_TO_POINTER(n + 1) */
	gint64            poll_us;
	guint             lease_ms;
	gint64            sweep_us;
	gint64            schedule_us;
	gint64            progress_us;
	guint             backoff_base_ms;
	guint             backoff_cap_ms;
	gchar            *instance;		/* NULL: made at start */
	gint64            take_over_us;	/* 0: never */
	gboolean          fenced;		/* claims and renewals use the lock */
	gint64            fence_key;

	/* Changed by axis_job_queue_set_lock() at any time */
	gint              fence_pid;		/* (core) the lock session's process */
	gchar             fence_start[AXIS_JOB_FENCE_START_MAX];	/* (core) and its start */

	/* Fixed at start */
	gchar            *daemon_id;		/* "axisd:HOST:PID:INSTANCE" */
	gint64            heartbeat_us;
	gchar           **all_kinds;
	GPtrArray        *lanes;		/* of Lane */
	GHashTable       *lanes_by_name;	/* name -> Lane */
	GPtrArray        *slots;		/* of Slot */
	Slot             *dispatcher;
	AxisDbListener   *listener;		/* used by the dispatcher only */
	gint              wake[2];		/* the dispatcher's wake pipe */

	gboolean          started;		/* (core) */
	gboolean          stopping;		/* (core) */
	gboolean          stopped;		/* (core) stop() has returned */
	gboolean          suspended;		/* (core) axis_job_queue_suspend() */
	gint64            resumed_at;		/* (core) monotonic: the last resume, or 0 */
	guint             n_handlers;		/* (core) handlers being called now */
	gboolean          dispatcher_stop;	/* (core) */
	gint64            grace_deadline;	/* (core) monotonic */
	GHashTable       *running;		/* (core) set of AxisJobContext (ref): a
						 * job id can be in it twice, when the job
						 * was taken from a handler still running
						 * here and claimed again by another */
	guint             n_busy;		/* (core) workers with a job, not abandoned */
	guint             n_alive;		/* (core) threads not exited */
	gboolean          keeper_failing;	/* (core) */
	gboolean          maintenance_failing;	/* (core) */
	GQueue            events;		/* (core) of AxisJobEvent, for the main loop */
	gboolean          event_source;		/* (core) a delivery is scheduled */
	gboolean          events_dropped;	/* (core) */
};

struct _AxisJobQueue
{
	GObject  parent_instance;
	Core    *core;
};

enum
{
	SIGNAL_JOB_EVENT,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(AxisJobQueue, axis_job_queue, G_TYPE_OBJECT)

/*
 * Events
 */

AxisJobEvent *
axis_job_event_copy(const AxisJobEvent *event)
{
	AxisJobEvent *copy;

	g_return_val_if_fail(event != NULL, NULL);

	copy = g_new0(AxisJobEvent, 1);
	*copy = *event;
	copy->kind = g_strdup(event->kind);
	copy->queue = g_strdup(event->queue);
	copy->message = g_strdup(event->message);

	return copy;
}

void
axis_job_event_free(AxisJobEvent *event)
{
	if (event == NULL)
		return;

	g_free(event->kind);
	g_free(event->queue);
	g_free(event->message);
	g_free(event);
}

G_DEFINE_BOXED_TYPE(AxisJobEvent, axis_job_event, axis_job_event_copy, axis_job_event_free)

static JsonNode *
member_value(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
		return NULL;

	return node;
}

static const gchar *
member_string(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = member_value(object, name);

	if (node == NULL || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

static gint64
member_int(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = member_value(object, name);

	if (node == NULL || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;

	return json_node_get_int(node);
}

/* A notification from axis-job-lifecycle.c, or NULL if it is not one. */
static AxisJobEvent *
parse_event(const gchar *payload)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AxisJobEvent) event = NULL;
	JsonObject *object;
	JsonNode *root;
	JsonNode *node;
	const gchar *type;

	if (!json_parser_load_from_data(parser, payload, -1, NULL))
		return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;
	object = json_node_get_object(root);

	event = g_new0(AxisJobEvent, 1);
	type = member_string(object, "type");
	if (g_strcmp0(type, "state") == 0)
		event->type = AXIS_JOB_EVENT_STATE;
	else if (g_strcmp0(type, "progress") == 0)
		event->type = AXIS_JOB_EVENT_PROGRESS;
	else if (g_strcmp0(type, "cancel-requested") == 0)
		event->type = AXIS_JOB_EVENT_CANCEL_REQUESTED;
	else
		return NULL;

	if (!axis_job_state_from_string(member_string(object, "state"), &event->state))
		return NULL;
	event->id = member_int(object, "id");
	event->kind = g_strdup(member_string(object, "kind"));
	event->queue = g_strdup(member_string(object, "queue"));
	if (event->kind == NULL || event->queue == NULL || event->id <= 0)
		return NULL;
	event->attempts = (guint)CLAMP(member_int(object, "attempts"), 0, G_MAXUINT);
	event->max_attempts = (guint)CLAMP(member_int(object, "max_attempts"), 0, G_MAXUINT);
	node = member_value(object, "runnable");
	event->runnable = node != NULL && json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
		json_node_get_boolean(node);
	node = member_value(object, "progress");
	event->progress = node != NULL && json_node_get_value_type(node) != G_TYPE_STRING ?
		json_node_get_double(node) : -1.0;
	event->message = g_strdup(member_string(object, "message"));

	return g_steal_pointer(&event);
}

/*
 * The Core
 */

static Core *
core_ref(Core *core)
{
	g_atomic_int_inc(&core->ref_count);
	return core;
}

static void
lane_free(gpointer data)
{
	Lane *lane = data;

	g_free(lane->name);
	g_strfreev(lane->kinds);
	g_cond_clear(&lane->cond);
	g_free(lane);
}

static void
slot_free(gpointer data)
{
	Slot *slot = data;

	/* Only reached once no thread is left, so nothing is running. */
	if (slot->thread != NULL)
		g_thread_unref(slot->thread);
	if (slot->job != NULL)
		axis_job_context_unref(slot->job);
	g_free(slot->name);
	g_free(slot);
}

static void
core_unref(gpointer data)
{
	Core *core = data;

	if (!g_atomic_int_dec_and_test(&core->ref_count))
		return;

	g_ptr_array_unref(core->slots);
	g_hash_table_unref(core->lanes_by_name);
	g_ptr_array_unref(core->lanes);
	g_hash_table_unref(core->running);
	g_hash_table_unref(core->concurrency);
	g_queue_clear_full(&core->events, (GDestroyNotify)axis_job_event_free);
	axis_db_listener_free(core->listener);
	if (core->wake[0] >= 0)
		close(core->wake[0]);
	if (core->wake[1] >= 0)
		close(core->wake[1]);
	g_strfreev(core->all_kinds);
	g_free(core->instance);
	g_free(core->daemon_id);
	axis_job_registry_unref(core->registry);
	g_weak_ref_clear(&core->owner);
	g_mutex_clear(&core->lock);
	g_cond_clear(&core->cond);
	g_free(core);
}

static Core *
core_new(
	AxisDbPool      *pool,
	AxisJobRegistry *registry
){
	Core *core = g_new0(Core, 1);

	core->ref_count = 1;
	g_mutex_init(&core->lock);
	g_cond_init(&core->cond);
	core->pool = pool;
	core->registry = axis_job_registry_ref(registry);
	g_weak_ref_init(&core->owner, NULL);
	core->concurrency = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	core->poll_us = (gint64)AXIS_JOB_QUEUE_DEFAULT_POLL_INTERVAL_MS * US_PER_MS;
	core->lease_ms = AXIS_JOB_DEFAULT_LEASE_MS;
	core->sweep_us = (gint64)AXIS_JOB_QUEUE_DEFAULT_SWEEP_INTERVAL_MS * US_PER_MS;
	core->schedule_us = (gint64)AXIS_JOB_QUEUE_DEFAULT_SCHEDULE_INTERVAL_MS * US_PER_MS;
	core->progress_us = (gint64)AXIS_JOB_QUEUE_PROGRESS_INTERVAL_MS * US_PER_MS;
	core->backoff_base_ms = AXIS_JOB_DEFAULT_BACKOFF_BASE_MS;
	core->backoff_cap_ms = AXIS_JOB_DEFAULT_BACKOFF_CAP_MS;
	core->lanes = g_ptr_array_new_with_free_func(lane_free);
	core->lanes_by_name = g_hash_table_new(g_str_hash, g_str_equal);
	core->slots = g_ptr_array_new_with_free_func(slot_free);
	core->running = g_hash_table_new_full(g_direct_hash, g_direct_equal,
		(GDestroyNotify)axis_job_context_unref, NULL);
	core->wake[0] = -1;
	core->wake[1] = -1;
	g_queue_init(&core->events);

	return core;
}

/* The progress hook of every context: progress waits to be written. */
static void
core_wake_keeper(gpointer data)
{
	Core *core = data;

	g_mutex_lock(&core->lock);
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);
}

/* The last thing a thread does. */
static void
thread_exit(Slot *slot)
{
	Core *core = slot->core;

	g_mutex_lock(&core->lock);
	slot->exited = TRUE;
	core->n_alive--;
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	/* @slot may be freed from here on. */
	core_unref(core);
}

/* stop() no longer waits for @slot. */
static void
slot_abandon_locked(
	Core *core,
	Slot *slot
){
	if (slot->abandoned)
		return;

	slot->abandoned = TRUE;
	if (slot->job != NULL)
		core->n_busy--;
}

/* Tells every handler running here for job @id to stop because it was
 * cancelled (there is one, normally). */
static void
running_cancel(
	Core   *core,
	gint64  id
){
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func(
		(GDestroyNotify)axis_job_context_unref);
	GHashTableIter iter;
	gpointer key;
	guint i;

	g_mutex_lock(&core->lock);
	g_hash_table_iter_init(&iter, core->running);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		AxisJobContext *context = key;

		if (context->id == id)
			g_ptr_array_add(found, axis_job_context_ref(context));
	}
	g_mutex_unlock(&core->lock);

	for (i = 0; i < found->len; i++)
		axis_job_context_stop(g_ptr_array_index(found, i), AXIS_JOB_STOP_CANCELLED);
}

/*
 * Delivering events on the main loop
 */

static gboolean
deliver_events(gpointer data)
{
	Core *core = data;
	AxisJobQueue *queue = g_weak_ref_get(&core->owner);
	gboolean more = TRUE;
	guint i;

	for (i = 0; i < EVENTS_PER_DISPATCH; i++) {
		AxisJobEvent *event;

		g_mutex_lock(&core->lock);
		event = g_queue_pop_head(&core->events);
		if (event == NULL) {
			core->event_source = FALSE;
			more = FALSE;
		}
		g_mutex_unlock(&core->lock);

		if (event == NULL)
			break;
		if (queue != NULL)
			g_signal_emit(queue, signals[SIGNAL_JOB_EVENT], 0, event);
		axis_job_event_free(event);
	}

	if (queue != NULL)
		g_object_unref(queue);

	return more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Takes @event.  Called by the dispatcher only, so events are queued in
 * the order the notifications arrived; one delivery source at a time
 * drains them in that order. */
static void
post_event(
	Core         *core,
	AxisJobEvent *event
){
	gboolean attach = FALSE;
	gboolean dropped = FALSE;

	g_mutex_lock(&core->lock);
	if (g_queue_get_length(&core->events) >= MAX_PENDING_EVENTS) {
		dropped = !core->events_dropped;
		core->events_dropped = TRUE;
	} else {
		g_queue_push_tail(&core->events, event);
		event = NULL;
		if (!core->event_source) {
			core->event_source = TRUE;
			attach = TRUE;
		}
	}
	g_mutex_unlock(&core->lock);

	if (dropped)
		g_warning("job events are not being delivered (is the main loop running?); "
			"%d are waiting and further ones are dropped", MAX_PENDING_EVENTS);
	axis_job_event_free(event);

	if (attach) {
		GSource *source = g_idle_source_new();

		g_source_set_name(source, "axis-job-events");
		g_source_set_callback(source, deliver_events, core_ref(core), core_unref);
		g_source_attach(source, NULL);
		g_source_unref(source);
	}
}

/*
 * Workers
 */

/* Some worker of @queue should look for a job. */
static void
lane_wake(
	Core        *core,
	const gchar *queue
){
	Lane *lane;

	/* Fixed since start: no lock needed to look it up. */
	lane = queue != NULL ? g_hash_table_lookup(core->lanes_by_name, queue) : NULL;
	if (lane == NULL || lane->concurrency == 0)
		return;

	g_mutex_lock(&core->lock);
	lane->pending = TRUE;
	g_cond_signal(&lane->cond);
	g_mutex_unlock(&core->lock);
}

/* The fence for a claim, a renewal, a sweep or a take-over: @fence filled
 * in, or NULL when the queue has none.  Not with core->lock held. */
static const AxisJobFence *
core_fence(
	Core         *core,
	AxisJobFence *fence
){
	gboolean fenced;

	g_mutex_lock(&core->lock);
	fenced = core->fenced;
	if (fenced) {
		fence->lock_key = core->fence_key;
		fence->holder_pid = core->fence_pid;
		g_strlcpy(fence->holder_start, core->fence_start, sizeof(fence->holder_start));
	}
	g_mutex_unlock(&core->lock);

	return fenced ? fence : NULL;
}

/* Blocks until a worker of @lane should try to claim.  FALSE: stop. */
static gboolean
lane_wait(
	Core *core,
	Slot *slot
){
	Lane *lane = slot->lane;
	gboolean claim = FALSE;

	g_mutex_lock(&core->lock);
	for (;;) {
		gint64 now;

		if (core->stopping || slot->abandoned)
			break;
		/* Suspended: nothing is claimed until it resumes (which wakes
		 * every lane). */
		if (core->suspended) {
			g_cond_wait(&lane->cond, &core->lock);
			continue;
		}
		if (lane->pending) {
			lane->pending = FALSE;
			claim = TRUE;
			break;
		}
		now = g_get_monotonic_time();
		if (now >= lane->next_poll) {
			lane->next_poll = now + core->poll_us;
			claim = TRUE;
			break;
		}
		g_cond_wait_until(&lane->cond, &core->lock, lane->next_poll);
	}
	g_mutex_unlock(&core->lock);

	return claim;
}

static AxisJobClaim *
worker_claim(Slot *slot)
{
	Core *core = slot->core;
	Lane *lane = slot->lane;
	g_autoptr(GError) error = NULL;
	AxisJobClaim *claim = NULL;
	AxisJobFence fence;
	AxisDbConn *conn;
	gboolean ok = FALSE;
	gboolean report = FALSE;

	g_mutex_lock(&core->lock);
	if (core->stopping || core->suspended || slot->abandoned) {
		g_mutex_unlock(&core->lock);
		return NULL;
	}
	/* From now until its result is recorded, the sweeper leaves what this
	 * worker holds alone. */
	slot->busy = TRUE;
	g_mutex_unlock(&core->lock);

	/* Kinds of a module that is switched off are not claimed: their jobs
	 * wait, queued, until it is switched on. */
	{
		g_autoptr(GPtrArray) allowed = g_ptr_array_new();
		guint k;

		for (k = 0; lane->kinds[k] != NULL; k++) {
			const AxisJobKind *kind = axis_job_registry_lookup(core->registry, lane->kinds[k]);

			if (kind == NULL || axis_job_registry_kind_enabled(core->registry, kind))
				g_ptr_array_add(allowed, lane->kinds[k]);
		}
		g_ptr_array_add(allowed, NULL);

		if (allowed->len == 1) {
			ok = TRUE;
		} else {
			conn = axis_db_pool_acquire(core->pool, &error);
			if (conn != NULL) {
				/* Fenced: nothing is claimed unless the daemon's lock is
				 * held at that moment, by the database's own account. */
				ok = axis_job_claim_fenced(conn, lane->name, (const gchar *const *)allowed->pdata,
					slot->name, core->lease_ms, core_fence(core, &fence), &claim, &error);
				axis_db_pool_release(core->pool, conn);
			}
		}
	}

	g_mutex_lock(&core->lock);
	if (ok != !lane->failing) {
		report = TRUE;
		lane->failing = !ok;
	}
	if (!ok) {
		/* Not before a second from now, whatever wakes us. */
		lane->pending = FALSE;
		lane->next_poll = g_get_monotonic_time() + MIN(core->poll_us, RETRY_US);
	}
	/* A failed claim may still have committed (the connection dropped
	 * after the server did it): such a job is an orphan for the sweeper. */
	if (claim == NULL)
		slot->busy = FALSE;
	g_mutex_unlock(&core->lock);

	if (report && !ok)
		g_warning("queue %s: cannot claim jobs: %s; retrying", lane->name, error->message);
	else if (report)
		g_message("queue %s: claiming jobs works again", lane->name);

	return claim;
}

static JsonObject *
parse_payload(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	return json_object_ref(json_node_get_object(root));
}

typedef enum
{
	ACTION_SUCCEED,
	ACTION_FAIL,
	ACTION_FAIL_PERMANENTLY,
	ACTION_HAND_BACK
} Action;

static const gchar *
action_name(Action action)
{
	switch (action) {
	case ACTION_SUCCEED:
		return "succeeded";
	case ACTION_HAND_BACK:
		return "stopped for the shutdown or the suspension";
	case ACTION_FAIL:
	case ACTION_FAIL_PERMANENTLY:
	default:
		return "failed";
	}
}

/*
 * Records a result with the job's lease token, unless the thread has been
 * abandoned meanwhile.  Logs what happened.
 */
static void
record_result(
	Slot              *slot,
	AxisJobContext    *context,
	Action             action,
	const gchar       *message
){
	Core *core = slot->core;
	g_autoptr(GError) error = NULL;
	AxisJobState state = AXIS_JOB_STATE_QUEUED;
	AxisDbConn *conn;
	gboolean ok = FALSE;
	gboolean abandoned;

	g_mutex_lock(&core->lock);
	abandoned = slot->abandoned;
	g_mutex_unlock(&core->lock);
	if (abandoned) {
		g_message(RUN_FORMAT " %s after the job queue stopped waiting for it; the result is "
			"discarded and the job will run again once its lease expires",
			RUN_ARGS(context), action_name(action));
		return;
	}

	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn != NULL) {
		switch (action) {
		case ACTION_SUCCEED:
			ok = axis_job_succeed(conn, context->id, context->lease_token, &error);
			state = AXIS_JOB_STATE_SUCCEEDED;
			break;
		case ACTION_HAND_BACK:
			ok = axis_job_hand_back(conn, context->id, context->lease_token, &state, &error);
			break;
		case ACTION_FAIL:
		case ACTION_FAIL_PERMANENTLY:
		default:
			ok = axis_job_fail(conn, context->id, context->lease_token, message,
				action == ACTION_FAIL_PERMANENTLY, core->backoff_base_ms, core->backoff_cap_ms,
				&state, &error);
			break;
		}
		axis_db_pool_release(core->pool, conn);
	}

	if (!ok && g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
		g_message(RUN_FORMAT " %s, but its lease had been lost by then; the result is discarded",
			RUN_ARGS(context), action_name(action));
		return;
	}
	if (!ok) {
		g_warning(RUN_FORMAT " %s, but that could not be recorded: %s; it will run again once "
			"its lease expires", RUN_ARGS(context), action_name(action), error->message);
		return;
	}

	switch (state) {
	case AXIS_JOB_STATE_SUCCEEDED:
		g_debug("job %" G_GINT64_FORMAT " (%s) succeeded", context->id, context->kind);
		break;
	case AXIS_JOB_STATE_QUEUED:
		if (action == ACTION_HAND_BACK)
			g_info("job %" G_GINT64_FORMAT " (%s) was handed back (the job queue is stopping or "
				"suspended)", context->id, context->kind);
		else
			g_info("job %" G_GINT64_FORMAT " (%s) failed (attempt %u of %u) and will be "
				"retried: %s", context->id, context->kind, context->attempt,
				context->max_attempts, message);
		break;
	case AXIS_JOB_STATE_FAILED:
		g_message("job %" G_GINT64_FORMAT " (%s) failed%s: %s", context->id, context->kind,
			action == ACTION_FAIL_PERMANENTLY ? " permanently" : " on its last attempt",
			message);
		break;
	case AXIS_JOB_STATE_CANCELLED:
		g_info("job %" G_GINT64_FORMAT " (%s) was cancelled", context->id, context->kind);
		break;
	case AXIS_JOB_STATE_RUNNING:
	default:
		break;
	}
}

/* A job claimed just as the queue began to stop: give it back unrun. */
static void
hand_back_unrun(
	Slot               *slot,
	const AxisJobClaim *claim
){
	Core *core = slot->core;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	gboolean abandoned;

	g_mutex_lock(&core->lock);
	abandoned = slot->abandoned;
	g_mutex_unlock(&core->lock);
	if (abandoned)
		return;

	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn == NULL ||
	    !axis_job_hand_back(conn, claim->id, claim->lease_token, NULL, &error))
		g_warning("job %" G_GINT64_FORMAT " (%s) was claimed as the queue stopped and could "
			"not be handed back: %s; it will run once its lease expires",
			claim->id, claim->kind, error->message);
	if (conn != NULL)
		axis_db_pool_release(core->pool, conn);
}

/* A job that cannot be run at all: failed for good. */
static void
fail_unrun(
	Slot               *slot,
	const AxisJobClaim *claim,
	const gchar        *message
){
	Core *core = slot->core;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;

	g_critical("job %" G_GINT64_FORMAT " (%s): %s; failing it", claim->id, claim->kind, message);
	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn == NULL ||
	    !axis_job_fail(conn, claim->id, claim->lease_token, message, TRUE, 0, 0, NULL, &error))
		g_warning("job %" G_GINT64_FORMAT " (%s) could not be failed: %s",
			claim->id, claim->kind, error->message);
	if (conn != NULL)
		axis_db_pool_release(core->pool, conn);
}

static void
worker_run(
	Slot               *slot,
	const AxisJobClaim *claim
){
	Core *core = slot->core;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = NULL;
	g_autofree gchar *message = NULL;
	const AxisJobKind *kind;
	AxisJobContext *context;
	Action action;
	gboolean stopping;
	gboolean ok;
	gboolean abandoned;
	gboolean lease_lost;
	gboolean shutdown;
	gboolean cancel_requested;

	/* The claim only takes registered kinds, and the table only holds JSON
	 * objects, so neither of these can happen; but a job claimed by
	 * mistake is never failed for want of a handler, and a payload that
	 * cannot be read fails rather than coming back for ever. */
	kind = axis_job_registry_lookup(core->registry, claim->kind);
	if (kind == NULL) {
		g_critical("job %" G_GINT64_FORMAT " (%s): claimed with no handler; handing it back",
			claim->id, claim->kind);
		hand_back_unrun(slot, claim);
		return;
	}
	payload = parse_payload(claim->payload);
	if (payload == NULL) {
		fail_unrun(slot, claim, "the payload is not a JSON object");
		return;
	}

	context = axis_job_context_new(claim, payload, core->pool, core_wake_keeper,
		core_ref(core), core_unref);
	context->next_beat = g_get_monotonic_time() + core->heartbeat_us;

	/* A claim that was under way when the queue was suspended is given
	 * back unrun, as one under way when it began to stop. */
	g_mutex_lock(&core->lock);
	stopping = core->stopping || core->suspended;
	if (!stopping) {
		slot->job = axis_job_context_ref(context);
		g_hash_table_add(core->running, axis_job_context_ref(context));
		core->n_busy++;
		core->n_handlers++;
		g_cond_broadcast(&core->cond);
	}
	g_mutex_unlock(&core->lock);

	if (stopping) {
		hand_back_unrun(slot, claim);
		axis_job_context_unref(context);
		return;
	}

	ok = kind->handler(context, kind->user_data, &error);

	/* The handler has returned: whatever it touches, it no longer does
	 * (axis_job_queue_wait_handlers()). */
	g_mutex_lock(&core->lock);
	core->n_handlers--;
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	g_mutex_lock(&context->lock);
	context->handler_done = TRUE;
	abandoned = context->abandoned;
	lease_lost = context->lease_lost;
	shutdown = context->shutdown;
	cancel_requested = context->cancel_requested;
	g_mutex_unlock(&context->lock);

	if (ok)
		action = ACTION_SUCCEED;
	else if (shutdown)
		action = ACTION_HAND_BACK;
	else if (cancel_requested)
		action = ACTION_FAIL;	/* recorded as cancelled */
	else if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT))
		action = ACTION_FAIL_PERMANENTLY;
	else
		action = ACTION_FAIL;

	if (!ok && error == NULL)
		message = g_strdup("the handler failed without saying why");
	else if (!ok && cancel_requested)
		message = g_strdup_printf("cancelled: %s", error->message);
	else if (!ok && (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED) ||
	                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)))
		message = g_strdup_printf("the handler reported that it was cancelled, but nothing "
			"cancelled it: %s", error->message);
	else if (!ok)
		message = g_strdup(error->message);

	if (abandoned)
		g_message(RUN_FORMAT " %s after the shutdown grace period ended; it had been handed "
			"back, so the result is discarded", RUN_ARGS(context), action_name(action));
	else if (lease_lost)
		g_message(RUN_FORMAT " %s after losing its lease; the result is discarded",
			RUN_ARGS(context), action_name(action));
	else
		record_result(slot, context, action, message);

	g_mutex_lock(&core->lock);
	g_hash_table_remove(core->running, context);
	if (slot->job == context) {
		if (!slot->abandoned)
			core->n_busy--;
		slot->job = NULL;
		axis_job_context_unref(context);
	}
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	axis_job_context_unref(context);
}

static gpointer
worker_main(gpointer data)
{
	Slot *slot = data;
	Core *core = slot->core;
	Lane *lane = slot->lane;

	while (lane_wait(core, slot)) {
		for (;;) {
			AxisJobClaim *claim = worker_claim(slot);
			gboolean stop;

			if (claim == NULL)
				break;
			/* There may be more: let an idle sibling look too. */
			if (lane->concurrency > 1)
				lane_wake(core, lane->name);
			worker_run(slot, claim);
			axis_job_claim_free(claim);

			g_mutex_lock(&core->lock);
			slot->busy = FALSE;
			stop = core->stopping || slot->abandoned;
			g_mutex_unlock(&core->lock);
			if (stop)
				goto out;
		}
	}

out:
	thread_exit(slot);
	return NULL;
}

/*
 * The keeper: heartbeats, progress, and the end of the grace period
 */

static void
keeper_beat(
	Core           *core,
	AxisJobContext *context,
	gint64          now
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *message = NULL;
	AxisJobProgress progress;
	AxisJobFence fence;
	AxisDbConn *conn;
	gboolean with_progress = FALSE;
	gboolean cancel_requested = FALSE;
	gboolean ok = FALSE;
	gboolean lost = FALSE;
	gboolean report = FALSE;

	g_mutex_lock(&context->lock);
	if (context->handler_done || context->lease_lost || context->abandoned) {
		g_mutex_unlock(&context->lock);
		return;
	}
	if (context->progress_dirty && now >= context->last_progress_write + core->progress_us) {
		with_progress = TRUE;
		progress.fraction = context->progress;
		message = g_strdup(context->progress_message);
		progress.message = message;
		context->progress_dirty = FALSE;
		context->last_progress_write = now;
	}
	g_mutex_unlock(&context->lock);

	/* Fenced: no lease is extended unless the daemon's lock is held at
	 * that moment, so a daemon that lost it cannot keep a job from the one
	 * that took it for longer than one lease. */
	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn != NULL) {
		ok = axis_job_heartbeat_fenced(conn, context->id, context->lease_token, core->lease_ms,
			with_progress ? &progress : NULL, core_fence(core, &fence), &cancel_requested, &error);
		axis_db_pool_release(core->pool, conn);
	}

	g_mutex_lock(&context->lock);
	if (ok) {
		context->next_beat = now + core->heartbeat_us;
	} else if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
		/* Finished meanwhile is not lost: the worker knows what it did. */
		lost = !context->handler_done;
	} else {
		context->next_beat = now + MIN(core->heartbeat_us, RETRY_US);
		if (with_progress)
			context->progress_dirty = TRUE;
	}
	g_mutex_unlock(&context->lock);

	if (cancel_requested)
		axis_job_context_stop(context, AXIS_JOB_STOP_CANCELLED);
	if (lost && axis_job_context_stop(context, AXIS_JOB_STOP_LEASE_LOST))
		g_warning(RUN_FORMAT " lost its lease (the job was taken back by a sweeper); its "
			"handler is told to stop and whatever it reports will be discarded", RUN_ARGS(context));
	/* Refused by the fence: the daemon's lock is gone, whatever its watcher
	 * has noticed yet.  The lease token is still good, so the handler is
	 * told to stop as for a shutdown and its job handed back, rather than
	 * left running until the lease runs out. */
	if (g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED) &&
	    axis_job_context_stop(context, AXIS_JOB_STOP_SHUTDOWN))
		g_warning(RUN_FORMAT ": its lease was not renewed, because this daemon does not hold its database "
			"lock; its handler is told to stop and the job will be handed back", RUN_ARGS(context));

	if (!lost && error == NULL) {
		g_mutex_lock(&core->lock);
		report = core->keeper_failing;
		core->keeper_failing = FALSE;
		g_mutex_unlock(&core->lock);
		if (report)
			g_message("renewing job leases works again");
	} else if (!lost && !g_error_matches(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST)) {
		g_mutex_lock(&core->lock);
		report = !core->keeper_failing;
		core->keeper_failing = TRUE;
		g_mutex_unlock(&core->lock);
		if (report)
			g_warning("cannot renew job leases: %s; retrying", error->message);
	}
}

/*
 * The grace period is over: every job whose handler is still running is
 * abandoned, and handed back unless its lease is already lost.  The
 * decision for each is taken under its lock against handler_done.
 */
static void
keeper_give_up(Core *core)
{
	g_autoptr(GPtrArray) handed = g_ptr_array_new_with_free_func(
		(GDestroyNotify)axis_job_context_unref);
	guint i;

	g_mutex_lock(&core->lock);
	for (i = 0; i < core->slots->len; i++) {
		Slot *slot = g_ptr_array_index(core->slots, i);
		AxisJobContext *context = slot->job;
		gboolean give_up = FALSE;
		gboolean lost;

		if (context == NULL || slot->abandoned)
			continue;

		g_mutex_lock(&context->lock);
		if (!context->handler_done) {
			context->abandoned = TRUE;
			give_up = TRUE;
		}
		lost = context->lease_lost;
		g_mutex_unlock(&context->lock);

		if (!give_up)
			continue;

		slot_abandon_locked(core, slot);
		g_hash_table_remove(core->running, context);
		if (!lost)
			g_ptr_array_add(handed, axis_job_context_ref(context));
	}
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	for (i = 0; i < handed->len; i++) {
		AxisJobContext *context = g_ptr_array_index(handed, i);
		g_autoptr(GError) error = NULL;
		AxisDbConn *conn;
		gboolean ok = FALSE;

		conn = axis_db_pool_acquire(core->pool, &error);
		if (conn != NULL) {
			ok = axis_job_hand_back(conn, context->id, context->lease_token, NULL, &error);
			axis_db_pool_release(core->pool, conn);
		}

		if (ok)
			g_warning(RUN_FORMAT " did not finish within the shutdown grace period; it was "
				"handed back to the queue, and whatever its handler reports will be discarded",
				RUN_ARGS(context));
		else
			g_warning(RUN_FORMAT " did not finish within the shutdown grace period and could "
				"not be handed back (%s); it will run again once its lease expires",
				RUN_ARGS(context), error->message);
	}
}

static gpointer
keeper_main(gpointer data)
{
	Slot *slot = data;
	Core *core = slot->core;
	g_autoptr(GPtrArray) due = g_ptr_array_new_with_free_func(
		(GDestroyNotify)axis_job_context_unref);

	g_mutex_lock(&core->lock);
	for (;;) {
		GHashTableIter iter;
		gpointer key;
		gint64 now = g_get_monotonic_time();
		gint64 wake_at = G_MAXINT64;
		guint i;

		if (core->stopping) {
			if (core->n_busy == 0)
				break;
			if (now >= core->grace_deadline) {
				g_mutex_unlock(&core->lock);
				keeper_give_up(core);
				g_mutex_lock(&core->lock);
				break;
			}
			wake_at = core->grace_deadline;
		}

		/* Suspended (the daemon lost its lock): no lease is renewed.  The
		 * handlers are stopping, and their hand-backs need only their lease
		 * tokens; a renewal would be refused by the fence anyway. */
		g_hash_table_iter_init(&iter, core->running);
		while (!core->suspended && g_hash_table_iter_next(&iter, &key, NULL)) {
			AxisJobContext *context = key;
			gint64 at;

			g_mutex_lock(&context->lock);
			if (!context->handler_done && !context->lease_lost && !context->abandoned) {
				at = context->next_beat;
				if (context->progress_dirty)
					at = MIN(at, context->last_progress_write + core->progress_us);
				if (at <= now)
					g_ptr_array_add(due, axis_job_context_ref(context));
				else
					wake_at = MIN(wake_at, at);
			}
			g_mutex_unlock(&context->lock);
		}

		if (due->len == 0) {
			if (wake_at == G_MAXINT64)
				g_cond_wait(&core->cond, &core->lock);
			else
				g_cond_wait_until(&core->cond, &core->lock, wake_at);
			continue;
		}

		g_mutex_unlock(&core->lock);
		for (i = 0; i < due->len; i++)
			keeper_beat(core, g_ptr_array_index(due, i), now);
		g_ptr_array_set_size(due, 0);
		g_mutex_lock(&core->lock);
	}
	g_mutex_unlock(&core->lock);

	thread_exit(slot);
	return NULL;
}

/*
 * Maintenance: lease sweeps and the schedule tick
 */

static void
maintenance_report(
	Core        *core,
	const gchar *what,
	GError      *error
){
	gboolean report;

	g_mutex_lock(&core->lock);
	report = (error != NULL) != core->maintenance_failing;
	core->maintenance_failing = error != NULL;
	g_mutex_unlock(&core->lock);

	if (report && error != NULL)
		g_warning("job queue maintenance (%s) failed: %s; retrying", what, error->message);
	else if (report)
		g_message("job queue maintenance works again");
}

static void
maintenance_sweep(Core *core)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) busy = g_ptr_array_new_with_free_func(g_free);
	AxisJobFence fence;
	AxisDbConn *conn;
	guint i;

	/* Workers here that are running what they claimed are alive: their
	 * jobs are not to be taken back even if a lease ran out (heartbeats
	 * failing while the database was away); the next heartbeat renews
	 * it.  A worker that turns busy after this list is made has a fresh
	 * lease; one that turns idle has its jobs swept next time. */
	g_mutex_lock(&core->lock);
	for (i = 0; i < core->slots->len; i++) {
		Slot *slot = g_ptr_array_index(core->slots, i);

		if (slot->role == ROLE_WORKER && slot->busy && !slot->abandoned)
			g_ptr_array_add(busy, g_strdup(slot->name));
	}
	g_mutex_unlock(&core->lock);
	g_ptr_array_add(busy, NULL);

	/* Fenced: a daemon that lost its lock does not know which jobs the one
	 * that holds it spares. */
	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn != NULL) {
		axis_job_sweep_expired_leases_fenced(conn, (const gchar *const *)core->all_kinds,
			(const gchar *const *)busy->pdata, core_fence(core, &fence), NULL, &error);
		axis_db_pool_release(core->pool, conn);
	}
	maintenance_report(core, "lease sweep", error);
}

static void
maintenance_tick(Core *core)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	guint n = 0;

	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn != NULL) {
		axis_job_schedule_tick(conn, core->registry, &n, &error);
		axis_db_pool_release(core->pool, conn);
	}
	maintenance_report(core, "schedules", error);
	if (n > 0)
		g_debug("schedules enqueued %u job(s)", n);
}

/* The jobs earlier daemon runs left running, taken back at once; FALSE when
 * that could not be done (it is tried again). */
static gboolean
maintenance_take_over(Core *core)
{
	g_autoptr(GError) error = NULL;
	AxisJobFence fence;
	AxisDbConn *conn;
	guint n = 0;
	gboolean ok = FALSE;

	/* Fenced: a daemon that lost its lock would take back the live jobs of
	 * the one that took it.  Refused by the fence, it is tried again. */
	conn = axis_db_pool_acquire(core->pool, &error);
	if (conn != NULL) {
		ok = axis_job_take_back_from_daemons(conn, (const gchar *const *)core->all_kinds, core->daemon_id,
			core_fence(core, &fence), &n, &error);
		axis_db_pool_release(core->pool, conn);
	}
	maintenance_report(core, "taking back the jobs of earlier runs", error);
	if (ok && n > 0)
		g_message("took back %u job(s) that earlier runs of the daemon left running (this daemon holds the "
			"database alone)", n);
	else if (ok)
		g_debug("no job of an earlier run of the daemon was left running");

	return ok;
}

static gpointer
maintenance_main(gpointer data)
{
	Slot *slot = data;
	Core *core = slot->core;
	gint64 next_sweep = g_get_monotonic_time() + core->sweep_us;
	gint64 next_tick = g_get_monotonic_time();
	gint64 take_over_at = core->take_over_us > 0 ? g_get_monotonic_time() + core->take_over_us : G_MAXINT64;

	g_mutex_lock(&core->lock);
	while (!core->stopping) {
		gint64 now = g_get_monotonic_time();

		/* Suspended: no sweep, no take-over, no schedule until it resumes
		 * (which wakes this thread).  A take-over still to come waits a
		 * whole delay from the resume: another daemon may have held the
		 * database meanwhile, and the delay is what it needs to stop. */
		if (core->suspended) {
			g_cond_wait(&core->cond, &core->lock);
			continue;
		}
		if (take_over_at != G_MAXINT64 && core->resumed_at > 0)
			take_over_at = MAX(take_over_at, core->resumed_at + core->take_over_us);
		if (now >= take_over_at) {
			gboolean done;

			g_mutex_unlock(&core->lock);
			done = maintenance_take_over(core);
			g_mutex_lock(&core->lock);
			take_over_at = done ? G_MAXINT64 : g_get_monotonic_time() + RETRY_US;
			continue;
		}
		if (now >= next_sweep) {
			g_mutex_unlock(&core->lock);
			maintenance_sweep(core);
			g_mutex_lock(&core->lock);
			next_sweep = g_get_monotonic_time() + core->sweep_us;
			continue;
		}
		if (now >= next_tick) {
			g_mutex_unlock(&core->lock);
			maintenance_tick(core);
			g_mutex_lock(&core->lock);
			next_tick = g_get_monotonic_time() + core->schedule_us;
			continue;
		}
		g_cond_wait_until(&core->cond, &core->lock, MIN(MIN(next_sweep, next_tick), take_over_at));
	}
	g_mutex_unlock(&core->lock);

	thread_exit(slot);
	return NULL;
}

/*
 * The dispatcher: notifications become wake-ups, cancel requests and events
 */

static void
dispatch(
	Core        *core,
	const gchar *payload
){
	AxisJobEvent *event = parse_event(payload);

	if (event == NULL) {
		g_debug("ignoring a notification that is not a job event: %s", payload);
		return;
	}

	if (event->type == AXIS_JOB_EVENT_STATE && event->state == AXIS_JOB_STATE_QUEUED &&
	    event->runnable)
		lane_wake(core, event->queue);

	if (event->type == AXIS_JOB_EVENT_CANCEL_REQUESTED)
		running_cancel(core, event->id);

	post_event(core, event);
}

/* Dispatches what has been received; returns TRUE if @barrier was among
 * it (which is not dispatched). */
static gboolean
dispatch_received(
	Core           *core,
	AxisDbListener *listener,
	const gchar    *barrier
){
	gboolean seen = FALSE;
	gchar *payload;

	while ((payload = axis_db_listener_pop(listener)) != NULL) {
		if (barrier != NULL && strcmp(payload, barrier) == 0)
			seen = TRUE;
		else if (!g_str_has_prefix(payload, BARRIER_PREFIX))
			dispatch(core, payload);
		g_free(payload);
	}

	return seen;
}

static void
drain_wake_pipe(Core *core)
{
	gchar buffer[64];

	while (read(core->wake[0], buffer, sizeof(buffer)) > 0)
		;
}

/*
 * The last notifications: one sent from the dispatcher's own connection
 * arrives after everything committed before it, so once it is back every
 * change the queue made before stopping has been dispatched.
 */
static void
dispatcher_finish(Core *core)
{
	AxisDbListener *listener = core->listener;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *barrier = NULL;
	gint64 deadline = g_get_monotonic_time() + DISPATCHER_STOP_US - G_USEC_PER_SEC / 2;

	dispatch_received(core, listener, NULL);
	if (!axis_db_listener_is_connected(listener))
		return;

	barrier = g_strdup_printf(BARRIER_PREFIX "%08x%08x", g_random_int(), g_random_int());
	if (!axis_db_listener_notify(listener, barrier, deadline, &error)) {
		g_debug("job events: could not flush the last notifications: %s", error->message);
		dispatch_received(core, listener, NULL);
		return;
	}

	for (;;) {
		AxisDbListenerWait wait;

		if (dispatch_received(core, listener, barrier))
			return;
		wait = axis_db_listener_wait(listener, -1, deadline, &error);
		if (wait == AXIS_DB_LISTENER_TIMEOUT || wait == AXIS_DB_LISTENER_LOST) {
			dispatch_received(core, listener, NULL);
			return;
		}
	}
}

static gpointer
dispatcher_main(gpointer data)
{
	Slot *slot = data;
	Core *core = slot->core;
	AxisDbListener *listener = core->listener;
	gint64 next_ping = g_get_monotonic_time() + LISTENER_PING_US;
	gint64 reconnect_at = 0;
	guint backoff_ms = 500;
	gboolean warned = FALSE;

	for (;;) {
		g_autoptr(GError) error = NULL;
		AxisDbListenerWait wait;
		gboolean stop;
		gint64 now;

		g_mutex_lock(&core->lock);
		stop = core->dispatcher_stop;
		g_mutex_unlock(&core->lock);
		if (stop)
			break;

		now = g_get_monotonic_time();
		if (!axis_db_listener_is_connected(listener) && now >= reconnect_at) {
			if (axis_db_listener_reconnect(listener, now + LISTENER_DEADLINE_US, &error)) {
				g_message("job notifications: reconnected");
				warned = FALSE;
				backoff_ms = 500;
				next_ping = g_get_monotonic_time() + LISTENER_PING_US;
			} else {
				if (!warned)
					g_warning("job notifications: cannot reconnect: %s; retrying "
						"(jobs are still found by polling)", error->message);
				warned = TRUE;
				reconnect_at = g_get_monotonic_time() + (gint64)backoff_ms * US_PER_MS;
				backoff_ms = MIN(backoff_ms * 2, LISTENER_BACKOFF_MAX_MS);
			}
			g_clear_error(&error);
		}

		wait = axis_db_listener_wait(listener, core->wake[0],
			axis_db_listener_is_connected(listener) ? next_ping : reconnect_at, &error);
		switch (wait) {
		case AXIS_DB_LISTENER_READY:
			dispatch_received(core, listener, NULL);
			break;
		case AXIS_DB_LISTENER_WOKEN:
			drain_wake_pipe(core);
			break;
		case AXIS_DB_LISTENER_LOST:
			dispatch_received(core, listener, NULL);
			g_warning("job notifications: lost the connection: %s; reconnecting "
				"(jobs are still found by polling)", error->message);
			warned = TRUE;
			reconnect_at = g_get_monotonic_time();
			break;
		case AXIS_DB_LISTENER_TIMEOUT:
		default:
			if (axis_db_listener_is_connected(listener) && g_get_monotonic_time() >= next_ping) {
				if (!axis_db_listener_ping(listener,
				                           g_get_monotonic_time() + LISTENER_DEADLINE_US, &error)) {
					g_warning("job notifications: the connection stopped answering: %s; "
						"reconnecting", error->message);
					warned = TRUE;
					reconnect_at = g_get_monotonic_time();
				}
				dispatch_received(core, listener, NULL);
				next_ping = g_get_monotonic_time() + LISTENER_PING_US;
			}
			break;
		}
	}

	dispatcher_finish(core);
	thread_exit(slot);
	return NULL;
}

/*
 * Start-up
 */

typedef struct
{
	Core           *core;
	AxisDbListener *listener;
	GError         *error;
} Startup;

static const gchar stored_kinds_sql[] =
	"SELECT kind, queue, count(*) FILTER (WHERE state = 'queued'),"
	" count(*) FILTER (WHERE state = 'running')"
	" FROM job WHERE state IN ('queued', 'running')"
	" GROUP BY kind, queue ORDER BY kind, queue";

static const gchar stored_schedules_sql[] =
	"SELECT name, kind FROM job_schedule WHERE enabled ORDER BY kind, name";

typedef struct
{
	gint64   queued;
	gint64   running;
	GString *schedules;
} Unknown;

static int
compare_strings(
	const void *a,
	const void *b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void
unknown_free(gpointer data)
{
	Unknown *unknown = data;

	if (unknown->schedules != NULL)
		g_string_free(unknown->schedules, TRUE);
	g_free(unknown);
}

/* One warning per kind stored here that has no handler.  (A kind of this
 * build's stored under another queue was moved to its own before this.) */
static gboolean
warn_about_unknown_kinds(
	Core        *core,
	AxisDbConn  *conn,
	GError     **error
){
	g_autoptr(AxisDbResult) jobs = NULL;
	g_autoptr(AxisDbResult) schedules = NULL;
	g_autoptr(GHashTable) unknown = NULL;
	guint i;

	if (!axis_db_conn_exec(conn, stored_kinds_sql, 0, NULL, &jobs, error) ||
	    !axis_db_conn_exec(conn, stored_schedules_sql, 0, NULL, &schedules, error))
		return FALSE;

	unknown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unknown_free);

	for (i = 0; i < axis_db_result_get_n_rows(jobs); i++) {
		const gchar *kind = axis_db_result_get_text(jobs, i, 0);
		gint64 queued = 0;
		gint64 running = 0;
		Unknown *u;

		axis_db_result_get_int64(jobs, i, 2, &queued);
		axis_db_result_get_int64(jobs, i, 3, &running);

		if (axis_job_registry_lookup(core->registry, kind) != NULL)
			continue;

		u = g_hash_table_lookup(unknown, kind);
		if (u == NULL) {
			u = g_new0(Unknown, 1);
			g_hash_table_insert(unknown, g_strdup(kind), u);
		}
		u->queued += queued;
		u->running += running;
	}

	for (i = 0; i < axis_db_result_get_n_rows(schedules); i++) {
		const gchar *name = axis_db_result_get_text(schedules, i, 0);
		const gchar *kind = axis_db_result_get_text(schedules, i, 1);
		Unknown *u;

		if (axis_job_registry_lookup(core->registry, kind) != NULL)
			continue;

		u = g_hash_table_lookup(unknown, kind);
		if (u == NULL) {
			u = g_new0(Unknown, 1);
			g_hash_table_insert(unknown, g_strdup(kind), u);
		}
		if (u->schedules == NULL)
			u->schedules = g_string_new(NULL);
		else
			g_string_append(u->schedules, ", ");
		g_string_append_printf(u->schedules, "'%s'", name);
	}

	{
		g_autofree gpointer *keys = NULL;
		guint n_keys = 0;

		keys = g_hash_table_get_keys_as_array(unknown, &n_keys);
		qsort(keys, n_keys, sizeof(keys[0]), compare_strings);
		for (i = 0; i < n_keys; i++) {
			const gchar *kind = keys[i];
			Unknown *u = g_hash_table_lookup(unknown, kind);
			g_autofree gchar *jobs_text = NULL;

			if (u->queued + u->running > 0)
				jobs_text = g_strdup_printf("%" G_GINT64_FORMAT " queued and %" G_GINT64_FORMAT
					" running job(s)", u->queued, u->running);

			g_warning("job kind '%s' has no handler in this daemon: %s%s%s%s are left "
				"untouched until a daemon that has one runs them",
				kind, jobs_text != NULL ? jobs_text : "",
				jobs_text != NULL && u->schedules != NULL ? " and " : "",
				u->schedules != NULL ? "schedule(s) " : "",
				u->schedules != NULL ? u->schedules->str : "");
		}
	}

	return TRUE;
}

static gpointer
startup_main(gpointer data)
{
	Startup *startup = data;
	Core *core = startup->core;
	g_autoptr(GPtrArray) schedules = NULL;
	AxisJobFence fence;
	AxisDbConn *conn;
	guint i;

	startup->listener = axis_db_listener_new(core->pool, AXIS_JOB_CHANNEL, -1, &startup->error);
	if (startup->listener == NULL)
		return NULL;

	conn = axis_db_pool_acquire(core->pool, &startup->error);
	if (conn == NULL)
		return NULL;

	/* The build's schedules as it declares them, and those it no longer
	 * declares disabled (each logged by the calls). */
	schedules = axis_job_registry_list_schedules(core->registry);
	{
		g_autofree const gchar **names = g_new0(const gchar *, schedules->len + 1);

		for (i = 0; i < schedules->len; i++) {
			const AxisJobScheduleInfo *info = g_ptr_array_index(schedules, i);

			names[i] = info->name;
			if (!axis_job_schedule_ensure(conn, info, NULL, &startup->error))
				goto out;
		}
		if (!axis_job_schedule_retire(conn, names, NULL, &startup->error))
			goto out;
	}

	/* Jobs a build that claimed their kind from another queue left: on
	 * their kind's queue now, or no thread here would claim them. */
	{
		g_autoptr(GPtrArray) kinds = axis_job_registry_list_kinds(core->registry);
		g_autofree const gchar **kind_names = g_new0(const gchar *, kinds->len + 1);
		g_autofree const gchar **queue_names = g_new0(const gchar *, kinds->len + 1);

		for (i = 0; i < kinds->len; i++) {
			const AxisJobKind *kind = g_ptr_array_index(kinds, i);

			kind_names[i] = kind->kind;
			queue_names[i] = kind->queue;
		}
		if (!axis_job_move_to_kind_queues(conn, kind_names, queue_names, NULL, &startup->error))
			goto out;
		/* Queued jobs of kinds this build does not have: failed, so that
		 * no key (a schedule's) is held by a job nothing will run. */
		if (!axis_job_fail_unknown_kinds(conn, kind_names, NULL, &startup->error))
			goto out;
	}

	if (!warn_about_unknown_kinds(core, conn, &startup->error))
		goto out;

	axis_job_sweep_expired_leases_fenced(conn, (const gchar *const *)core->all_kinds, NULL,
		core_fence(core, &fence), NULL, &startup->error);

out:
	axis_db_pool_release(core->pool, conn);
	return NULL;
}

static gint
compare_lanes(
	gconstpointer a,
	gconstpointer b
){
	const Lane *x = *(const Lane *const *)a;
	const Lane *y = *(const Lane *const *)b;

	return strcmp(x->name, y->name);
}

/* Lanes from the registry's kinds; checks the configured concurrency. */
static gboolean
build_lanes(
	Core       *core,
	GPtrArray  *kinds,
	guint      *total,
	GError    **error
){
	g_autoptr(GHashTable) kinds_by_queue = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	kinds_by_queue = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
		(GDestroyNotify)g_ptr_array_unref);
	core->all_kinds = g_new0(gchar *, kinds->len + 1);
	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);
		GPtrArray *list;

		/* Every kind's expired leases are swept, but a remote kind is
		 * claimed only through the worker API: it has no lane, so no
		 * thread here ever claims it. */
		core->all_kinds[i] = g_strdup(kind->kind);
		if (kind->remote)
			continue;
		list = g_hash_table_lookup(kinds_by_queue, kind->queue);
		if (list == NULL) {
			list = g_ptr_array_new_with_free_func(g_free);
			g_hash_table_insert(kinds_by_queue, kind->queue, list);
		}
		g_ptr_array_add(list, g_strdup(kind->kind));
	}

	g_hash_table_iter_init(&iter, core->concurrency);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		if (!g_hash_table_contains(kinds_by_queue, key)) {
			g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CONFIG,
				"a concurrency is set for queue '%s', but no registered job kind uses it",
				(const gchar *)key);
			return FALSE;
		}
	}

	*total = 0;
	g_hash_table_iter_init(&iter, kinds_by_queue);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		GPtrArray *list = value;
		Lane *lane = g_new0(Lane, 1);
		gpointer n = g_hash_table_lookup(core->concurrency, key);

		lane->name = g_strdup(key);
		lane->concurrency = n != NULL ? GPOINTER_TO_UINT(n) - 1 : AXIS_JOB_QUEUE_DEFAULT_CONCURRENCY;
		g_ptr_array_add(list, NULL);
		lane->kinds = (gchar **)g_ptr_array_steal(list, NULL);
		g_cond_init(&lane->cond);
		g_ptr_array_add(core->lanes, lane);
		g_hash_table_insert(core->lanes_by_name, lane->name, lane);
		*total += lane->concurrency;
	}
	g_ptr_array_sort(core->lanes, compare_lanes);

	return TRUE;
}

/* Undoes build_lanes() after a failed start. */
static void
forget_lanes(Core *core)
{
	g_hash_table_remove_all(core->lanes_by_name);
	g_ptr_array_set_size(core->lanes, 0);
	g_clear_pointer(&core->all_kinds, g_strfreev);
}

static Slot *
add_slot(
	Core        *core,
	Role         role,
	Lane        *lane,
	const gchar *name
){
	Slot *slot = g_new0(Slot, 1);

	slot->core = core;
	slot->role = role;
	slot->lane = lane;
	slot->name = g_strdup(name);
	g_ptr_array_add(core->slots, slot);

	return slot;
}

static GThreadFunc
slot_function(Slot *slot)
{
	switch (slot->role) {
	case ROLE_KEEPER:
		return keeper_main;
	case ROLE_MAINTENANCE:
		return maintenance_main;
	case ROLE_DISPATCHER:
		return dispatcher_main;
	case ROLE_WORKER:
	default:
		return worker_main;
	}
}

/*
 * The object
 */

static void
axis_job_queue_dispose(GObject *object)
{
	AxisJobQueue *self = AXIS_JOB_QUEUE(object);
	gboolean running;

	g_mutex_lock(&self->core->lock);
	running = self->core->started && !self->core->stopping;
	g_mutex_unlock(&self->core->lock);

	if (running) {
		g_warning("a running job queue was released; stopping it without a grace period");
		axis_job_queue_stop(self, 0);
	}

	G_OBJECT_CLASS(axis_job_queue_parent_class)->dispose(object);
}

static void
axis_job_queue_finalize(GObject *object)
{
	AxisJobQueue *self = AXIS_JOB_QUEUE(object);

	core_unref(self->core);

	G_OBJECT_CLASS(axis_job_queue_parent_class)->finalize(object);
}

static void
axis_job_queue_class_init(AxisJobQueueClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = axis_job_queue_dispose;
	object_class->finalize = axis_job_queue_finalize;

	signals[SIGNAL_JOB_EVENT] = g_signal_new("job-event",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AXIS_TYPE_JOB_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

static void
axis_job_queue_init(AxisJobQueue *self)
{
}

AxisJobQueue *
axis_job_queue_new(
	AxisDbPool      *pool,
	AxisJobRegistry *registry
){
	AxisJobQueue *self;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(registry != NULL, NULL);

	self = g_object_new(AXIS_TYPE_JOB_QUEUE, NULL);
	self->core = core_new(pool, registry);
	g_weak_ref_set(&self->core->owner, self);

	return self;
}

/* Settings may only change before start. */
static gboolean
settings_open(AxisJobQueue *self)
{
	gboolean open;

	g_mutex_lock(&self->core->lock);
	open = !self->core->started && !self->core->stopped;
	g_mutex_unlock(&self->core->lock);

	if (!open)
		g_critical("job queue settings cannot change once the queue has started");
	return open;
}

void
axis_job_queue_set_concurrency(
	AxisJobQueue *self,
	const gchar  *queue,
	guint         concurrency
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(queue != NULL);
	g_return_if_fail(concurrency <= AXIS_JOB_QUEUE_MAX_CONCURRENCY);

	if (!settings_open(self))
		return;

	g_hash_table_replace(self->core->concurrency, g_strdup(queue),
		GUINT_TO_POINTER(concurrency + 1));
}

guint
axis_job_queue_get_concurrency(
	AxisJobQueue *self,
	const gchar  *queue
){
	gpointer n;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), 0);
	g_return_val_if_fail(queue != NULL, 0);

	n = g_hash_table_lookup(self->core->concurrency, queue);
	return n != NULL ? GPOINTER_TO_UINT(n) - 1 : AXIS_JOB_QUEUE_DEFAULT_CONCURRENCY;
}

guint
axis_job_queue_get_total_concurrency(AxisJobQueue *self)
{
	g_autoptr(GPtrArray) kinds = NULL;
	g_autoptr(GHashTable) queues = NULL;
	GHashTableIter iter;
	gpointer key;
	guint total = 0;
	guint i;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), 0);

	kinds = axis_job_registry_list_kinds(self->core->registry);
	queues = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);

		/* No thread here runs a remote kind. */
		if (!kind->remote)
			g_hash_table_add(queues, kind->queue);
	}

	g_hash_table_iter_init(&iter, queues);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		total += axis_job_queue_get_concurrency(self, key);

	return total;
}

void
axis_job_queue_set_poll_interval(
	AxisJobQueue *self,
	guint         interval_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(interval_ms > 0);

	if (settings_open(self))
		self->core->poll_us = (gint64)interval_ms * US_PER_MS;
}

void
axis_job_queue_set_lease(
	AxisJobQueue *self,
	guint         lease_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(lease_ms > 0);

	if (settings_open(self))
		self->core->lease_ms = lease_ms;
}

void
axis_job_queue_set_sweep_interval(
	AxisJobQueue *self,
	guint         interval_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(interval_ms > 0);

	if (settings_open(self))
		self->core->sweep_us = (gint64)interval_ms * US_PER_MS;
}

void
axis_job_queue_set_schedule_interval(
	AxisJobQueue *self,
	guint         interval_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(interval_ms > 0);

	if (settings_open(self))
		self->core->schedule_us = (gint64)interval_ms * US_PER_MS;
}

void
axis_job_queue_wake(AxisJobQueue *self)
{
	GHashTableIter iter;
	gpointer key;

	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));

	if (self->core->lanes_by_name == NULL)
		return;

	/* Fixed since start: no lock needed to walk it. */
	g_hash_table_iter_init(&iter, self->core->lanes_by_name);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		lane_wake(self->core, key);
}

void
axis_job_queue_set_backoff(
	AxisJobQueue *self,
	guint         base_ms,
	guint         cap_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));

	if (settings_open(self)) {
		self->core->backoff_base_ms = base_ms;
		self->core->backoff_cap_ms = MAX(cap_ms, base_ms);
	}
}

void
axis_job_queue_set_instance(
	AxisJobQueue *self,
	const gchar  *instance
){
	const gchar *p;

	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(instance != NULL && strlen(instance) == AXIS_JOB_INSTANCE_LEN);
	for (p = instance; *p != '\0'; p++)
		g_return_if_fail((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));

	if (settings_open(self)) {
		g_free(self->core->instance);
		self->core->instance = g_strdup(instance);
	}
}

const gchar *
axis_job_queue_get_daemon_id(AxisJobQueue *self)
{
	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), NULL);

	/* Fixed once set at start. */
	return self->core->daemon_id;
}

gboolean
axis_job_queue_get_fence(
	AxisJobQueue *self,
	AxisJobFence *fence
){
	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);
	g_return_val_if_fail(fence != NULL, FALSE);

	return core_fence(self->core, fence) != NULL;
}

void
axis_job_queue_set_lock(
	AxisJobQueue *self,
	gint64        lock_key,
	gint          holder_pid,
	const gchar  *holder_start
){
	Core *core;
	gboolean refused = FALSE;

	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(holder_pid > 0);
	g_return_if_fail(holder_start != NULL && strlen(holder_start) < AXIS_JOB_FENCE_START_MAX);

	core = self->core;
	g_mutex_lock(&core->lock);
	if (!core->started && !core->stopped) {
		core->fenced = TRUE;
		core->fence_key = lock_key;
	} else if (!core->fenced || core->fence_key != lock_key) {
		refused = TRUE;
	}
	if (!refused) {
		core->fence_pid = holder_pid;
		g_strlcpy(core->fence_start, holder_start, sizeof(core->fence_start));
	}
	g_mutex_unlock(&core->lock);

	if (refused)
		g_critical("the job queue's lock can only be given before it starts; afterwards only its holder may change");
}

void
axis_job_queue_set_take_over(
	AxisJobQueue *self,
	guint         delay_ms
){
	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));
	g_return_if_fail(delay_ms > 0);

	if (settings_open(self))
		self->core->take_over_us = (gint64)delay_ms * US_PER_MS;
}

void
axis_job_queue_suspend(AxisJobQueue *self)
{
	Core *core;
	g_autoptr(GPtrArray) contexts = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;

	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));

	core = self->core;
	contexts = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_context_unref);
	g_mutex_lock(&core->lock);
	if (!core->started || core->stopping || core->suspended) {
		g_mutex_unlock(&core->lock);
		return;
	}
	core->suspended = TRUE;
	g_hash_table_iter_init(&iter, core->running);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(contexts, axis_job_context_ref(key));
	for (i = 0; i < core->lanes->len; i++) {
		Lane *lane = g_ptr_array_index(core->lanes, i);

		g_cond_broadcast(&lane->cond);
	}
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	/* Outside the lock: a cancellable's handlers are the handlers' code. */
	for (i = 0; i < contexts->len; i++)
		axis_job_context_stop(g_ptr_array_index(contexts, i), AXIS_JOB_STOP_SHUTDOWN);

	g_message("job queue suspended: no job is claimed and no schedule fires until it resumes; %u running "
		"job(s) told to stop", contexts->len);
}

gboolean
axis_job_queue_wait_handlers(
	AxisJobQueue *self,
	guint         timeout_ms
){
	Core *core;
	gint64 deadline;
	gboolean done;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	core = self->core;
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * US_PER_MS;
	g_mutex_lock(&core->lock);
	while (core->n_handlers > 0) {
		if (!g_cond_wait_until(&core->cond, &core->lock, deadline))
			break;
	}
	done = core->n_handlers == 0;
	g_mutex_unlock(&core->lock);

	return done;
}

void
axis_job_queue_resume(AxisJobQueue *self)
{
	Core *core;
	guint i;

	g_return_if_fail(AXIS_IS_JOB_QUEUE(self));

	core = self->core;
	g_mutex_lock(&core->lock);
	if (!core->suspended || core->stopping) {
		g_mutex_unlock(&core->lock);
		return;
	}
	core->suspended = FALSE;
	core->resumed_at = g_get_monotonic_time();
	for (i = 0; i < core->lanes->len; i++) {
		Lane *lane = g_ptr_array_index(core->lanes, i);

		lane->pending = TRUE;
		g_cond_broadcast(&lane->cond);
	}
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	g_message("job queue resumed");
}

gboolean
axis_job_queue_is_suspended(AxisJobQueue *self)
{
	gboolean suspended;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	g_mutex_lock(&self->core->lock);
	suspended = self->core->suspended;
	g_mutex_unlock(&self->core->lock);

	return suspended;
}

gboolean
axis_job_queue_start(
	AxisJobQueue  *self,
	GError       **error
){
	Core *core;
	g_autoptr(GPtrArray) kinds = NULL;
	g_autoptr(GString) summary = NULL;
	Startup startup;
	GThread *thread;
	guint total = 0;
	guint needed;
	guint i;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	core = self->core;
	g_mutex_lock(&core->lock);
	if (core->started || core->stopped) {
		g_mutex_unlock(&core->lock);
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE,
			"the job queue has already been started");
		return FALSE;
	}
	g_mutex_unlock(&core->lock);

	axis_job_registry_freeze(core->registry);
	kinds = axis_job_registry_list_kinds(core->registry);
	if (!build_lanes(core, kinds, &total, error)) {
		forget_lanes(core);
		return FALSE;
	}

	needed = total + AXIS_JOB_QUEUE_RESERVED_CONNECTIONS;
	if (axis_db_pool_get_size(core->pool) < needed) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CONFIG,
			"the database pool has %u connections, but the job queue needs %u: one for each "
			"of its %u worker threads (the queues' concurrency added up) and %d for itself; "
			"make the pool larger or the concurrency smaller",
			axis_db_pool_get_size(core->pool), needed, total, AXIS_JOB_QUEUE_RESERVED_CONNECTIONS);
		forget_lanes(core);
		return FALSE;
	}

	/* The start-up queries, on a thread of their own. */
	memset(&startup, 0, sizeof(startup));
	startup.core = core;
	thread = g_thread_new("axis-job-start", startup_main, &startup);
	g_thread_join(thread);
	if (startup.error != NULL) {
		g_propagate_prefixed_error(error, startup.error, "cannot start the job queue: ");
		axis_db_listener_free(startup.listener);
		forget_lanes(core);
		return FALSE;
	}
	core->listener = startup.listener;

	if (!g_unix_open_pipe(core->wake, O_CLOEXEC, error) ||
	    !g_unix_set_fd_nonblocking(core->wake[0], TRUE, error) ||
	    !g_unix_set_fd_nonblocking(core->wake[1], TRUE, error)) {
		g_prefix_error(error, "cannot start the job queue: ");
		return FALSE;
	}

	core->lease_ms = MAX(core->lease_ms, 3);
	core->heartbeat_us = (gint64)core->lease_ms * US_PER_MS / 3;

	/* This run's name, unlike any earlier run's on this host and process
	 * id.  A host name with a '/' in it would break the worker ids' parts. */
	if (core->instance == NULL)
		core->instance = g_strdup_printf("%08x%08x", g_random_int(), g_random_int());
	{
		g_autofree gchar *host = g_strdup(g_get_host_name());

		g_strdelimit(host, "/", '-');
		core->daemon_id = g_strdup_printf("axisd:%s:%d:%s", host, (int)getpid(), core->instance);
	}

	add_slot(core, ROLE_KEEPER, NULL, "keeper");
	add_slot(core, ROLE_MAINTENANCE, NULL, "maintenance");
	core->dispatcher = add_slot(core, ROLE_DISPATCHER, NULL, "dispatcher");
	summary = g_string_new(NULL);
	for (i = 0; i < core->lanes->len; i++) {
		Lane *lane = g_ptr_array_index(core->lanes, i);
		guint n;

		g_string_append_printf(summary, "%s%s (%u)", i > 0 ? ", " : "", lane->name,
			lane->concurrency);
		for (n = 0; n < lane->concurrency; n++) {
			g_autofree gchar *name = g_strdup_printf("%s/%s/%u", core->daemon_id, lane->name, n + 1);

			add_slot(core, ROLE_WORKER, lane, name);
		}
	}

	g_mutex_lock(&core->lock);
	core->started = TRUE;
	core->n_alive = core->slots->len;
	g_mutex_unlock(&core->lock);

	for (i = 0; i < core->slots->len; i++) {
		Slot *slot = g_ptr_array_index(core->slots, i);
		const gchar *thread_name;

		switch (slot->role) {
		case ROLE_KEEPER:
			thread_name = "axis-job-keeper";
			break;
		case ROLE_MAINTENANCE:
			thread_name = "axis-job-maint";
			break;
		case ROLE_DISPATCHER:
			thread_name = "axis-job-events";
			break;
		case ROLE_WORKER:
		default:
			thread_name = "axis-job-worker";
			break;
		}
		core_ref(core);
		slot->thread = g_thread_new(thread_name, slot_function(slot), slot);
	}

	g_info("job queue started: %s", summary->len > 0 ? summary->str : "no queues");
	g_info("job queue workers are named %s/QUEUE/N%s", core->daemon_id,
		core->take_over_us > 0 ? "; jobs that earlier runs left running are taken back shortly" : "");
	return TRUE;
}

static gboolean
others_done_locked(Core *core)
{
	guint i;

	for (i = 0; i < core->slots->len; i++) {
		Slot *slot = g_ptr_array_index(core->slots, i);

		if (slot != core->dispatcher && !slot->exited && !slot->abandoned)
			return FALSE;
	}

	return TRUE;
}

gboolean
axis_job_queue_stop(
	AxisJobQueue *self,
	guint         grace_ms
){
	Core *core;
	g_autoptr(GPtrArray) contexts = NULL;
	g_autoptr(GPtrArray) finished = NULL;
	g_autoptr(GPtrArray) left = NULL;
	GHashTableIter iter;
	gpointer key;
	gint64 limit;
	gint64 deadline;
	gboolean clean;
	guint i;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), TRUE);

	core = self->core;
	contexts = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_context_unref);

	g_mutex_lock(&core->lock);
	if (!core->started || core->stopping) {
		g_mutex_unlock(&core->lock);
		return TRUE;
	}
	core->stopping = TRUE;
	core->grace_deadline = g_get_monotonic_time() + (gint64)grace_ms * US_PER_MS;
	g_hash_table_iter_init(&iter, core->running);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(contexts, axis_job_context_ref(key));
	for (i = 0; i < core->lanes->len; i++) {
		Lane *lane = g_ptr_array_index(core->lanes, i);

		g_cond_broadcast(&lane->cond);
	}
	g_cond_broadcast(&core->cond);
	g_mutex_unlock(&core->lock);

	if (contexts->len > 0)
		g_info("stopping the job queue: %u job(s) running, %u ms to finish",
			contexts->len, grace_ms);
	for (i = 0; i < contexts->len; i++)
		axis_job_context_stop(g_ptr_array_index(contexts, i), AXIS_JOB_STOP_SHUTDOWN);

	/* Until no worker has a job (the keeper ends the grace period), then
	 * the other threads, each wait bounded. */
	g_mutex_lock(&core->lock);
	limit = core->grace_deadline + (gint64)AXIS_JOB_QUEUE_STOP_MARGIN_MS * US_PER_MS;
	while (core->n_busy > 0) {
		if (!g_cond_wait_until(&core->cond, &core->lock, limit))
			break;
	}
	deadline = MIN(g_get_monotonic_time() + (gint64)AXIS_JOB_QUEUE_STOP_MARGIN_MS * US_PER_MS,
		limit);
	while (!others_done_locked(core)) {
		if (!g_cond_wait_until(&core->cond, &core->lock, deadline))
			break;
	}

	core->dispatcher_stop = TRUE;
	g_mutex_unlock(&core->lock);
	if (write(core->wake[1], "x", 1) < 0 && errno != EAGAIN)
		g_debug("job queue: cannot wake the dispatcher: %s", g_strerror(errno));

	g_mutex_lock(&core->lock);
	deadline = g_get_monotonic_time() + DISPATCHER_STOP_US;
	while (!core->dispatcher->exited) {
		if (!g_cond_wait_until(&core->cond, &core->lock, deadline))
			break;
	}

	finished = g_ptr_array_new();
	left = g_ptr_array_new();
	for (i = 0; i < core->slots->len; i++) {
		Slot *slot = g_ptr_array_index(core->slots, i);

		if (slot->thread == NULL)
			continue;
		if (slot->exited) {
			g_ptr_array_add(finished, slot->thread);
		} else {
			if (slot->job != NULL) {
				g_mutex_lock(&slot->job->lock);
				if (!slot->job->handler_done)
					slot->job->abandoned = TRUE;
				g_mutex_unlock(&slot->job->lock);
			}
			slot_abandon_locked(core, slot);
			g_ptr_array_add(left, slot->thread);
		}
		slot->thread = NULL;
	}
	core->stopped = TRUE;
	g_mutex_unlock(&core->lock);

	for (i = 0; i < finished->len; i++)
		g_thread_join(g_ptr_array_index(finished, i));
	for (i = 0; i < left->len; i++)
		g_thread_unref(g_ptr_array_index(left, i));

	clean = left->len == 0;
	if (!clean)
		g_warning("the job queue stopped with %u thread(s) still running (handlers that did "
			"not stop, or a database that does not answer); they finish on their own",
			left->len);
	else
		g_info("job queue stopped");

	return clean;
}

gboolean
axis_job_queue_wait_threads(
	AxisJobQueue *self,
	guint         timeout_ms
){
	Core *core;
	gint64 deadline;
	gboolean done;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	core = self->core;
	deadline = g_get_monotonic_time() + (gint64)timeout_ms * US_PER_MS;
	g_mutex_lock(&core->lock);
	while (core->n_alive > 0) {
		if (!g_cond_wait_until(&core->cond, &core->lock, deadline))
			break;
	}
	done = core->n_alive == 0;
	g_mutex_unlock(&core->lock);

	return done;
}

gboolean
axis_job_queue_enqueue(
	AxisJobQueue                 *self,
	const gchar                  *kind,
	JsonObject                   *payload,
	const AxisJobEnqueueOptions  *options,
	gint64                       *id,
	gboolean                     *created,
	GError                      **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	/* Refused before a connection is taken. */
	if (!axis_job_registry_contains(self->core->registry, kind)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND,
			"cannot enqueue a job of kind '%s': no handler is registered for it",
			kind != NULL ? kind : "(null)");
		return FALSE;
	}

	conn = axis_db_pool_acquire(self->core->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_registry_enqueue(self->core->registry, conn, kind, payload, options,
		id, created, error);
	axis_db_pool_release(self->core->pool, conn);

	return ok;
}

gboolean
axis_job_queue_cancel(
	AxisJobQueue          *self,
	gint64                 id,
	AxisJobCancelOutcome  *outcome,
	GError               **error
){
	AxisJobCancelOutcome out = AXIS_JOB_CANCEL_TOO_LATE;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	conn = axis_db_pool_acquire(self->core->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_cancel(conn, id, &out, error);
	axis_db_pool_release(self->core->pool, conn);
	if (!ok)
		return FALSE;

	if (out == AXIS_JOB_CANCEL_REQUESTED)
		running_cancel(self->core, id);

	if (outcome != NULL)
		*outcome = out;
	return TRUE;
}

gboolean
axis_job_queue_retry(
	AxisJobQueue         *self,
	gint64                id,
	const gchar          *by,
	AxisJobRetryOutcome  *outcome,
	gint64               *live_id,
	GError              **error
){
	g_autoptr(GPtrArray) kinds = NULL;
	g_autofree const gchar **kind_names = NULL;
	g_autofree const gchar **queue_names = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(self), FALSE);

	kinds = axis_job_registry_list_kinds(self->core->registry);
	kind_names = g_new0(const gchar *, kinds->len + 1);
	queue_names = g_new0(const gchar *, kinds->len + 1);
	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);

		kind_names[i] = kind->kind;
		queue_names[i] = kind->queue;
	}

	conn = axis_db_pool_acquire(self->core->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_retry(conn, id, kind_names, queue_names, by, outcome, live_id, error);
	axis_db_pool_release(self->core->pool, conn);

	return ok;
}
