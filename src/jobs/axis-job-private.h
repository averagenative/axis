/*
 * axis-job-private.h - shared internals of src/jobs; not for use elsewhere
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_JOB_PRIVATE_H
#define AXIS_JOB_PRIVATE_H

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"

G_BEGIN_DECLS

/*
 * Lifecycle helpers (axis-job-lifecycle.c)
 */

/* A text[] literal ({"a","b"}) for a parameter. */
gchar *
axis_job_text_array(const gchar *const *items);

/* Valid UTF-8 of at most @max_chars characters; NULL for NULL. */
gchar *
axis_job_clean_text(
	const gchar *text,
	guint        max_chars
);

/* The payload as JSON text ("{}" for NULL); fails with
 * AXIS_JOB_ERROR_INVALID when it is too large. */
gchar *
axis_job_payload_to_text(
	JsonObject  *payload,
	GError     **error
);

/*
 * Registry internals (axis-job-registry.c).  Entries are never removed or
 * changed once added, so a pointer to one stays valid for the registry's
 * life.
 */

typedef struct
{
	gchar              *kind;
	gchar              *queue;
	guint               max_attempts;
	AxisJobHandlerFunc  handler;	/* %NULL for a remote kind */
	gpointer            user_data;
	AxisModuleSwitch   *module_switch;	/* its module's flag, or %NULL */
	AxisJobHold        *hold;	/* (nullable) set before the queue starts */
	gboolean            remote;	/* only the worker API claims it */
	gchar              *label;	/* nullable */
	AxisJobClass        job_class;
	gchar              *retry_elsewhere;	/* nullable: see AxisJobHandlerInfo */
} AxisJobKind;

/*
 * Whether jobs of @kind may be claimed and its schedules fire: %FALSE for a
 * kind that came from a module (axis_job_registry_add_modules()) that is
 * switched off, and for a kind whose hold is on.  Callable from any thread.
 */
gboolean
axis_job_registry_kind_enabled(
	AxisJobRegistry   *self,
	const AxisJobKind *kind
);

/* No more additions from now on (the queue has started). */
void
axis_job_registry_freeze(AxisJobRegistry *self);

const AxisJobKind *
axis_job_registry_lookup(
	AxisJobRegistry *self,
	const gchar     *kind
);

/* (transfer container): const AxisJobKind *, sorted by kind. */
GPtrArray *
axis_job_registry_list_kinds(AxisJobRegistry *self);

/* (transfer container): const AxisJobScheduleInfo *, in the order added. */
GPtrArray *
axis_job_registry_list_schedules(AxisJobRegistry *self);

/*
 * The handler context (axis-job-context.c).  The queue reads and writes
 * the fields marked (lock) with @lock held.  Lock order: the queue's own
 * lock, then a context's, never the other way round; and nothing calls
 * out of this file (cancellable, progress hook) with @lock held.
 */
struct _AxisJobContext
{
	gint               ref_count;	/* atomic */

	/* Fixed at creation */
	gint64             id;
	gchar             *kind;
	gchar             *queue;
	guint              attempt;
	guint              max_attempts;
	gchar             *lease_token;
	JsonObject        *payload;
	AxisDbPool        *pool;
	GCancellable      *cancellable;
	void             (*progress_hook)(gpointer data);
	gpointer           progress_hook_data;
	GDestroyNotify     progress_hook_destroy;

	GMutex             lock;
	GCond              cond;		/* signalled when a stop reason is set */
	AxisJobStopReason  reason;		/* (lock) the first one */
	gboolean           cancel_requested;	/* (lock) */
	gboolean           shutdown;		/* (lock) */
	gboolean           lease_lost;		/* (lock) */
	gboolean           handler_done;	/* (lock) the handler has returned */
	gboolean           abandoned;		/* (lock) shutdown gave up waiting and
						 * handed the job back */
	gboolean           progress_dirty;	/* (lock) not yet written */
	gdouble            progress;		/* (lock) */
	gchar             *progress_message;	/* (lock) */
	gint64             next_beat;		/* (lock) monotonic: heartbeat due */
	gint64             last_progress_write;	/* (lock) monotonic */
};

/* @hook (nullable) is called, without any lock held, when progress goes
 * from written to not yet written; it gets @hook_data, which @destroy
 * frees with the context. */
AxisJobContext *
axis_job_context_new(
	const AxisJobClaim *claim,
	JsonObject         *payload,
	AxisDbPool         *pool,
	void              (*hook)(gpointer data),
	gpointer            hook_data,
	GDestroyNotify      destroy
);

/* Tells the handler to stop for @reason (not NONE): records it, wakes
 * axis_job_context_wait() and cancels the cancellable.  Returns TRUE the
 * first time it is called with this @reason. */
gboolean
axis_job_context_stop(
	AxisJobContext    *self,
	AxisJobStopReason  reason
);

G_END_DECLS

#endif /* AXIS_JOB_PRIVATE_H */
