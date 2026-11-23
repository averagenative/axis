/*
 * axis-job-registry.h - job kinds, their handlers and default schedules
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every kind of job the daemon can run is registered once, with the queue
 * it is claimed from, how many attempts it gets and the function that
 * runs it.  Modules declare theirs in AxisModuleInfo (core/axis-module.h)
 * and axis_job_registry_add_modules() registers them all.
 *
 * A registry is filled before the queue that uses it starts; from then on
 * it is read-only and adding to it fails.  It is thread-safe throughout.
 */

#ifndef AXIS_JOB_REGISTRY_H
#define AXIS_JOB_REGISTRY_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "core/axis-module.h"
#include "db/axis-db-conn.h"
#include "jobs/axis-job-context.h"

G_BEGIN_DECLS

/**
 * AxisJobHandlerFunc:
 * @context: the job (see axis-job-context.h for the contract)
 * @user_data: the module's user data (or what was given to
 *   axis_job_registry_add())
 * @error: return location for the reason the job failed
 *
 * Runs one attempt of a job, on a worker thread of the job's queue.
 *
 * Returns: %TRUE if the job succeeded
 */
typedef gboolean (*AxisJobHandlerFunc)(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
);

/**
 * AxisJobClass:
 * @AXIS_JOB_CLASS_UNSET: not said (a kind registered by a test); shown as
 *   work.  The daemon refuses to start with a kind of its modules that has
 *   no class (axis_job_registry_list_undescribed()).
 * @AXIS_JOB_CLASS_WORK: something a person would want to watch: a scan, a
 *   probe, a search, an import, a conversion, a refresh of a report.  The
 *   running indicator and the dashboard count it.
 * @AXIS_JOB_CLASS_UPKEEP: background upkeep Axis repeats on its own:
 *   following the download clients, a scheduler's tick, a refresh of
 *   metadata, an expiry or a clean-up.  Listed on the Jobs page under its
 *   own heading and counted nowhere else.
 */
typedef enum
{
	AXIS_JOB_CLASS_UNSET = 0,
	AXIS_JOB_CLASS_WORK,
	AXIS_JOB_CLASS_UPKEEP
} AxisJobClass;

/* The longest label of a kind, in bytes. */
#define AXIS_JOB_LABEL_MAX_LEN 120

/* The longest sentence saying where a kind is retried instead, in bytes. */
#define AXIS_JOB_RETRY_ELSEWHERE_MAX_LEN 300

/**
 * AxisJobHandlerInfo:
 * @kind: the kind (a valid job name, see axis_job_name_is_valid()); unique
 *   across all modules
 * @queue: the queue its jobs are claimed from (a valid job name)
 * @max_attempts: how many times a job of this kind runs before it is
 *   failed, 1 to %AXIS_JOB_MAX_ATTEMPTS_LIMIT; 0 means
 *   %AXIS_JOB_DEFAULT_MAX_ATTEMPTS
 * @handler: runs a job
 * @label: (nullable): what a job of this kind does, in plain words, as a
 *   person reads it in a list ("Checking the download clients"); shown
 *   wherever a job is named, with @kind as a detail beside it
 * @job_class: work or upkeep
 *
 * In AxisModuleInfo.job_handlers, an array ended by an entry whose @kind
 * is %NULL.  Where a kind's work is asked for again instead of retrying
 * its job is said apart (axis_job_registry_set_retry_elsewhere()).
 */
struct _AxisJobHandlerInfo
{
	const gchar        *kind;
	const gchar        *queue;
	guint               max_attempts;
	AxisJobHandlerFunc  handler;
	const gchar        *label;
	AxisJobClass        job_class;
};

/**
 * AxisJobScheduleInfo:
 * @name: the schedule's name (a valid job name); its jobs are
 *   de-duplicated on "schedule:" followed by it
 * @kind: the kind it enqueues
 * @payload: (nullable): the payload, a JSON object as text; %NULL for {}
 * @interval_seconds: how often, 1 or more
 *
 * A default schedule, the build's: created, enabled and due at once, the
 * first time the queue starts and finds no schedule of that name, and at
 * every later start brought to what the build declares (kind, payload,
 * interval) unless a person changed it (axis-job-schedule.h).  A stored
 * schedule of the build's that no module declares any more is disabled at
 * start, not deleted.  In AxisModuleInfo.job_schedules, an array ended by
 * an entry whose @name is %NULL.
 */
struct _AxisJobScheduleInfo
{
	const gchar *name;
	const gchar *kind;
	const gchar *payload;
	guint        interval_seconds;
};

typedef struct _AxisJobRegistry AxisJobRegistry;

/**
 * axis_job_registry_new:
 *
 * Returns: (transfer full): an empty registry, reference counted
 */
AxisJobRegistry *
axis_job_registry_new(void);

/**
 * axis_job_registry_ref:
 *
 * Returns: (transfer full): @self
 */
AxisJobRegistry *
axis_job_registry_ref(AxisJobRegistry *self);

void
axis_job_registry_unref(AxisJobRegistry *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobRegistry, axis_job_registry_unref)

/**
 * axis_job_registry_add:
 * @self: an #AxisJobRegistry
 * @info: the kind; copied
 * @user_data: (nullable): passed to the handler; it must stay valid for as
 *   long as a handler may run
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_JOB_ERROR_INVALID, changing nothing, when a name is
 * malformed, the kind is already registered (the message names it), the
 * handler is %NULL or @info->max_attempts is out of range; and with
 * %AXIS_JOB_ERROR_STATE once a queue has started with this registry.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_add(
	AxisJobRegistry           *self,
	const AxisJobHandlerInfo  *info,
	gpointer                   user_data,
	GError                   **error
);

/**
 * axis_job_registry_add_remote:
 * @self: an #AxisJobRegistry
 * @kind: the kind (a valid job name); unique across all kinds
 * @queue: its queue (a valid job name); every kind of the queue must be
 *   remote too, so give remote kinds a queue of their own
 * @max_attempts: as for AxisJobHandlerInfo
 * @label: (nullable): as for AxisJobHandlerInfo
 * @job_class: as for AxisJobHandlerInfo
 * @module_switch: (nullable) (transfer full): the module the kind follows
 *   (from axis_module_registry_ref_switch()): while it is switched off its
 *   jobs are not claimed (axis_job_registry_kind_claimable()); taken even
 *   when the call fails
 * @error: (nullable): return location for an error
 *
 * Registers a kind that only remote workers run (through the worker API,
 * api/axis-worker-api.h): it has no handler, the in-process queue never
 * claims it and makes no worker thread for its queue, but it is a
 * registered kind like any other for enqueueing, cancelling, retrying and
 * for the sweep that hands back jobs whose lease expired.  Fails as
 * axis_job_registry_add(), and with %AXIS_JOB_ERROR_INVALID when @queue is
 * also the queue of a kind that is not remote.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_add_remote(
	AxisJobRegistry   *self,
	const gchar       *kind,
	const gchar       *queue,
	guint              max_attempts,
	const gchar       *label,
	AxisJobClass       job_class,
	AxisModuleSwitch  *module_switch,
	GError           **error
);

/**
 * AxisJobHold:
 *
 * A flag that holds the jobs of the kinds that follow it (see
 * axis_job_registry_set_hold()): while it is on, the queue does not claim
 * them, the worker API does not hand them out, and they keep their state.
 * Reference counted, like a module's switch, so that the queue's threads can
 * ask it without holding its owner; read and set atomically, from any
 * thread.  It starts off.
 */
typedef struct _AxisJobHold AxisJobHold;

AxisJobHold *
axis_job_hold_new(void);

AxisJobHold *
axis_job_hold_ref(AxisJobHold *self);

void
axis_job_hold_unref(AxisJobHold *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJobHold, axis_job_hold_unref)

void
axis_job_hold_set(
	AxisJobHold *self,
	gboolean     held
);

gboolean
axis_job_hold_is_held(AxisJobHold *self);

/**
 * axis_job_registry_set_hold:
 * @self: an #AxisJobRegistry
 * @kind: a registered kind
 * @hold: (nullable): the hold it follows (referenced), %NULL for none
 * @error: (nullable): return location for an error
 *
 * Before the queue starts.  Fails with %AXIS_JOB_ERROR_UNKNOWN_KIND for a
 * kind that is not registered, and %AXIS_JOB_ERROR_STATE once a queue has
 * started with this registry.  A held kind is not claimable
 * (axis_job_registry_kind_claimable()).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_set_hold(
	AxisJobRegistry  *self,
	const gchar      *kind,
	AxisJobHold      *hold,
	GError          **error
);

/**
 * axis_job_registry_get_label:
 *
 * Returns: (transfer none) (nullable): the plain label of @kind, valid for
 *   the registry's life; %NULL when the kind is not registered or has none
 */
const gchar *
axis_job_registry_get_label(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * axis_job_registry_get_class:
 *
 * Returns: the class of @kind; %AXIS_JOB_CLASS_UNSET when it is not
 *   registered or has none
 */
AxisJobClass
axis_job_registry_get_class(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * axis_job_registry_get_retry_elsewhere:
 *
 * Returns: (transfer none) (nullable): where a person asks for the work of
 *   a job of @kind again instead of retrying it (AxisJobHandlerInfo); %NULL
 *   when it is retried as itself, or the kind is not registered
 */
const gchar *
axis_job_registry_get_retry_elsewhere(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * axis_job_registry_set_retry_elsewhere:
 * @self: an #AxisJobRegistry
 * @kind: a registered kind
 * @text: (nullable): for a kind whose jobs are tried again by their owner
 *   and not as themselves (an import, which its own page resets; a
 *   conversion's encode, which the conversion hands out), one plain
 *   sentence saying where a person asks for the work again: the Jobs page
 *   then refuses Retry with it.  %NULL (the default): a retry is the job
 *   queued again as itself (axis_job_retry()).
 * @error: (nullable): return location for an error
 *
 * Modules say it in AxisModuleInfo.job_retry_elsewhere, which
 * axis_job_registry_add_modules() applies; a remote kind's registration
 * calls this.  Before the queue starts.  Fails with %AXIS_JOB_ERROR_UNKNOWN_KIND for a
 * kind that is not registered, %AXIS_JOB_ERROR_INVALID for a @text that is
 * not one line of 1 to %AXIS_JOB_RETRY_ELSEWHERE_MAX_LEN bytes, and
 * %AXIS_JOB_ERROR_STATE once a queue has started with this registry.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_set_retry_elsewhere(
	AxisJobRegistry  *self,
	const gchar      *kind,
	const gchar      *text,
	GError          **error
);

/**
 * axis_job_registry_list_class:
 *
 * Returns: (transfer full): the kinds of @job_class, sorted, %NULL
 *   terminated (empty when there is none)
 */
GStrv
axis_job_registry_list_class(
	AxisJobRegistry *self,
	AxisJobClass     job_class
);

/**
 * axis_job_registry_list_undescribed:
 *
 * Every kind must say in plain words what it does and whether it is work
 * or upkeep; the daemon refuses to start otherwise.
 *
 * Returns: (transfer full): the kinds with no label or no class, sorted,
 *   %NULL terminated (empty when every kind has both)
 */
GStrv
axis_job_registry_list_undescribed(AxisJobRegistry *self);

/**
 * axis_job_registry_is_remote:
 *
 * Returns: %TRUE if @kind is registered as a remote kind
 */
gboolean
axis_job_registry_is_remote(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * axis_job_registry_kind_claimable:
 *
 * Returns: %TRUE if @kind is registered, its module (if it has one) is
 *   switched on, and its hold (if it has one) is off
 */
gboolean
axis_job_registry_kind_claimable(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * axis_job_registry_add_schedule:
 * @self: an #AxisJobRegistry
 * @info: the default schedule; copied
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_JOB_ERROR_INVALID, changing nothing, when a name is
 * malformed, a default schedule of that name exists, the kind is not
 * registered (register kinds first), the payload is not a JSON object or
 * the interval is 0; and with %AXIS_JOB_ERROR_STATE once a queue has
 * started with this registry.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_add_schedule(
	AxisJobRegistry            *self,
	const AxisJobScheduleInfo  *info,
	GError                    **error
);

/**
 * axis_job_registry_add_modules:
 * @self: an #AxisJobRegistry
 * @modules: the module registry
 * @error: (nullable): return location for an error
 *
 * Registers the job handlers and then the default schedules of every
 * module in @modules, in registration order, each handler with its
 * module's user data.  Modules with neither table are skipped.  Fails as
 * axis_job_registry_add() and axis_job_registry_add_schedule(), with the
 * module's id prefixed to the message; what was registered before the
 * failure stays registered.  The registry then remembers which module each
 * kind came from, and the queue neither claims jobs of a kind whose module
 * is switched off nor fires its schedules (axis_module_registry_set_enabled()).
 * The registry keeps the modules' switches, not @modules, so @modules need
 * not outlive it.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_add_modules(
	AxisJobRegistry     *self,
	AxisModuleRegistry  *modules,
	GError             **error
);

/**
 * axis_job_registry_contains:
 *
 * Returns: %TRUE if a handler is registered for @kind
 */
gboolean
axis_job_registry_contains(
	AxisJobRegistry *self,
	const gchar     *kind
);

/**
 * AxisJobEnqueueOptions:
 * @priority: higher is claimed first within the queue; default 0
 * @delay_ms: not claimable before this long from now (database clock)
 * @dedupe_key: (nullable): see AxisJobSpec
 * @max_attempts: 0 for the kind's
 */
typedef struct
{
	gint         priority;
	gint64       delay_ms;
	const gchar *dedupe_key;
	guint        max_attempts;
} AxisJobEnqueueOptions;

/**
 * axis_job_registry_enqueue:
 * @self: an #AxisJobRegistry
 * @conn: a connection held by the calling thread
 * @kind: the kind
 * @payload: (nullable): the payload; %NULL is an empty object
 * @options: (nullable): %NULL for the defaults
 * @id: (out) (optional): the job's identifier
 * @created: (out) (optional): see axis_job_enqueue()
 * @error: (nullable): return location for an error
 *
 * Enqueues a job of a registered kind on the kind's queue, as
 * axis_job_enqueue().  Fails with %AXIS_JOB_ERROR_UNKNOWN_KIND, storing
 * nothing, when no handler is registered for @kind (the message names
 * it), and as axis_job_enqueue().
 *
 * Returns: %TRUE on success
 */
gboolean
axis_job_registry_enqueue(
	AxisJobRegistry              *self,
	AxisDbConn                   *conn,
	const gchar                  *kind,
	JsonObject                   *payload,
	const AxisJobEnqueueOptions  *options,
	gint64                       *id,
	gboolean                     *created,
	GError                      **error
);

G_END_DECLS

#endif /* AXIS_JOB_REGISTRY_H */
