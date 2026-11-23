/*
 * axis-job-context.c - what a job handler is given
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jobs/axis-job-context.h"
#include "jobs/axis-job-private.h"

#include <math.h>

AxisJobContext *
axis_job_context_new(
	const AxisJobClaim *claim,
	JsonObject         *payload,
	AxisDbPool         *pool,
	void              (*hook)(gpointer data),
	gpointer            hook_data,
	GDestroyNotify      destroy
){
	AxisJobContext *self;

	g_return_val_if_fail(claim != NULL, NULL);
	g_return_val_if_fail(payload != NULL, NULL);

	self = g_new0(AxisJobContext, 1);
	self->ref_count = 1;
	self->id = claim->id;
	self->kind = g_strdup(claim->kind);
	self->queue = g_strdup(claim->queue);
	self->attempt = claim->attempt;
	self->max_attempts = claim->max_attempts;
	self->lease_token = g_strdup(claim->lease_token);
	self->payload = json_object_ref(payload);
	self->pool = pool;
	self->cancellable = g_cancellable_new();
	self->progress_hook = hook;
	self->progress_hook_data = hook_data;
	self->progress_hook_destroy = destroy;
	g_mutex_init(&self->lock);
	g_cond_init(&self->cond);
	self->reason = AXIS_JOB_STOP_NONE;
	self->progress = -1.0;

	return self;
}

AxisJobContext *
axis_job_context_ref(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_job_context_unref(AxisJobContext *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	if (self->progress_hook_destroy != NULL)
		self->progress_hook_destroy(self->progress_hook_data);
	g_free(self->kind);
	g_free(self->queue);
	g_free(self->lease_token);
	json_object_unref(self->payload);
	g_object_unref(self->cancellable);
	g_free(self->progress_message);
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->cond);
	g_free(self);
}

gint64
axis_job_context_get_id(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->id;
}

const gchar *
axis_job_context_get_kind(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->kind;
}

const gchar *
axis_job_context_get_queue(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->queue;
}

guint
axis_job_context_get_attempt(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->attempt;
}

guint
axis_job_context_get_max_attempts(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, 0);
	return self->max_attempts;
}

JsonObject *
axis_job_context_get_payload(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->payload;
}

AxisDbPool *
axis_job_context_get_pool(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->pool;
}

GCancellable *
axis_job_context_get_cancellable(AxisJobContext *self)
{
	g_return_val_if_fail(self != NULL, NULL);
	return self->cancellable;
}

AxisJobStopReason
axis_job_context_get_stop_reason(AxisJobContext *self)
{
	AxisJobStopReason reason;

	g_return_val_if_fail(self != NULL, AXIS_JOB_STOP_NONE);

	g_mutex_lock(&self->lock);
	reason = self->reason;
	g_mutex_unlock(&self->lock);

	return reason;
}

gboolean
axis_job_context_is_cancelled(AxisJobContext *self)
{
	return axis_job_context_get_stop_reason(self) != AXIS_JOB_STOP_NONE;
}

gboolean
axis_job_context_stop(
	AxisJobContext    *self,
	AxisJobStopReason  reason
){
	gboolean *flag;
	gboolean first;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(reason != AXIS_JOB_STOP_NONE, FALSE);

	g_mutex_lock(&self->lock);
	switch (reason) {
	case AXIS_JOB_STOP_CANCELLED:
		flag = &self->cancel_requested;
		break;
	case AXIS_JOB_STOP_SHUTDOWN:
		flag = &self->shutdown;
		break;
	case AXIS_JOB_STOP_LEASE_LOST:
	default:
		flag = &self->lease_lost;
		break;
	}
	first = !*flag;
	*flag = TRUE;
	if (self->reason == AXIS_JOB_STOP_NONE)
		self->reason = reason;
	g_cond_broadcast(&self->cond);
	g_mutex_unlock(&self->lock);

	/* Outside the lock: the cancellable's handlers are the handler's
	 * code and may call back in here. */
	g_cancellable_cancel(self->cancellable);

	return first;
}

gboolean
axis_job_context_wait(
	AxisJobContext *self,
	guint           timeout_ms
){
	gint64 deadline;
	gboolean stopped;

	g_return_val_if_fail(self != NULL, TRUE);

	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	g_mutex_lock(&self->lock);
	while (self->reason == AXIS_JOB_STOP_NONE) {
		if (!g_cond_wait_until(&self->cond, &self->lock, deadline))
			break;
	}
	stopped = self->reason != AXIS_JOB_STOP_NONE;
	g_mutex_unlock(&self->lock);

	return stopped;
}

gboolean
axis_job_context_set_progress(
	AxisJobContext *self,
	gdouble         fraction,
	const gchar    *message
){
	gchar *clean;
	gboolean was_dirty;
	gboolean stopped;

	g_return_val_if_fail(self != NULL, FALSE);

	if (isnan(fraction) || fraction < 0.0)
		fraction = 0.0;
	else if (fraction > 1.0)
		fraction = 1.0;
	clean = axis_job_clean_text(message, AXIS_JOB_MAX_PROGRESS_CHARS);

	g_mutex_lock(&self->lock);
	g_free(self->progress_message);
	self->progress_message = clean;
	self->progress = fraction;
	was_dirty = self->progress_dirty;
	self->progress_dirty = TRUE;
	stopped = self->reason != AXIS_JOB_STOP_NONE;
	g_mutex_unlock(&self->lock);

	if (!was_dirty && self->progress_hook != NULL)
		self->progress_hook(self->progress_hook_data);

	return !stopped;
}
