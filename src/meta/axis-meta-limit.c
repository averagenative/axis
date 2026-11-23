/*
 * axis-meta-limit.c - one metadata provider's request budget
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-limit.h"

#include <math.h>

struct _AxisMetaLimiter
{
	GMutex  lock;
	GCond   changed;
	gint64  interval_us;		/* (lock) */
	gdouble rate;			/* (lock) */
	guint   burst;			/* (lock) */
	gint64  tat;			/* (lock) monotonic */
	gint64  paused_until;		/* (lock) monotonic, 0 for none */
	gint64  paused_until_real;	/* (lock) */
	gint64  backoff_until;		/* (lock) monotonic */
	gint64  backoff_until_real;	/* (lock) */
	guint   failures;		/* (lock) */
	guint64 requests;		/* (lock) */
	gint64  last_request_real;	/* (lock) */
	gchar  *last_error;		/* (lock) */
	gint64  last_error_real;	/* (lock) */
};

gint64
axis_meta_gcra(
	gint64  *tat,
	gint64   now_us,
	gint64   interval_us,
	guint    burst,
	gboolean take
){
	gint64 tolerance = interval_us * (gint64)(MAX(burst, 1u) - 1);
	gint64 earliest = *tat - tolerance;

	if (earliest <= now_us) {
		if (take)
			*tat = MAX(*tat, now_us) + interval_us;
		return now_us;
	}
	return earliest;
}

static void
set_rate_locked(
	AxisMetaLimiter *self,
	gdouble          rate,
	guint            burst
){
	if (!isfinite(rate) || rate < 0.01)
		rate = 0.01;
	if (rate > 1000.0)
		rate = 1000.0;
	self->rate = rate;
	self->burst = CLAMP(burst, 1u, 100u);
	self->interval_us = (gint64)ceil(1e6 / rate);
}

AxisMetaLimiter *
axis_meta_limiter_new(
	gdouble rate,
	guint   burst
){
	AxisMetaLimiter *self = g_new0(AxisMetaLimiter, 1);

	g_mutex_init(&self->lock);
	g_cond_init(&self->changed);
	set_rate_locked(self, rate, burst);
	return self;
}

void
axis_meta_limiter_free(AxisMetaLimiter *self)
{
	if (self == NULL)
		return;
	g_mutex_clear(&self->lock);
	g_cond_clear(&self->changed);
	g_free(self->last_error);
	g_free(self);
}

void
axis_meta_limiter_set_rate(
	AxisMetaLimiter *self,
	gdouble          rate,
	guint            burst
){
	g_mutex_lock(&self->lock);
	set_rate_locked(self, rate, burst);
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

static void
on_cancel(
	GCancellable *cancellable,
	gpointer      user_data
){
	AxisMetaLimiter *self = user_data;

	g_mutex_lock(&self->lock);
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

AxisMetaLimitResult
axis_meta_limiter_acquire(
	AxisMetaLimiter *self,
	gint64           deadline_us,
	GCancellable    *cancellable,
	gint64          *until_us
){
	AxisMetaLimitResult result;
	gulong handler = 0;

	if (until_us != NULL)
		*until_us = 0;
	if (cancellable != NULL)
		handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancel), self, NULL);
	g_mutex_lock(&self->lock);
	for (;;) {
		gint64 now = g_get_monotonic_time();
		gint64 held = MAX(self->paused_until, self->backoff_until);
		gint64 earliest;

		if (g_cancellable_is_cancelled(cancellable)) {
			result = AXIS_META_LIMIT_CANCELLED;
			break;
		}
		if (held > now) {
			if (held > deadline_us) {
				if (until_us != NULL)
					*until_us = self->paused_until >= self->backoff_until ? self->paused_until_real :
						self->backoff_until_real;
				result = AXIS_META_LIMIT_PAUSED;
				break;
			}
			g_cond_wait_until(&self->changed, &self->lock, held);
			continue;
		}
		earliest = axis_meta_gcra(&self->tat, now, self->interval_us, self->burst, TRUE);
		if (earliest == now) {
			self->requests++;
			self->last_request_real = g_get_real_time();
			result = AXIS_META_LIMIT_OK;
			break;
		}
		if (earliest > deadline_us) {
			result = AXIS_META_LIMIT_DEADLINE;
			break;
		}
		g_cond_wait_until(&self->changed, &self->lock, earliest);
	}
	g_mutex_unlock(&self->lock);
	if (handler != 0)
		g_cancellable_disconnect(cancellable, handler);
	return result;
}

static void
note_error_locked(
	AxisMetaLimiter *self,
	const gchar     *message
){
	if (message == NULL)
		return;
	g_free(self->last_error);
	self->last_error = g_strdup(message);
	self->last_error_real = g_get_real_time();
}

void
axis_meta_limiter_pause(
	AxisMetaLimiter *self,
	gint64           seconds,
	const gchar     *message
){
	gint64 now = g_get_monotonic_time();
	gint64 until;

	if (seconds < 0)
		seconds = AXIS_META_LIMIT_DEFAULT_PAUSE_S;
	seconds = MIN(seconds, (gint64)AXIS_META_LIMIT_MAX_PAUSE_S);
	until = now + seconds * G_USEC_PER_SEC;
	g_mutex_lock(&self->lock);
	if (until > self->paused_until) {
		self->paused_until = until;
		self->paused_until_real = g_get_real_time() + seconds * G_USEC_PER_SEC;
	}
	note_error_locked(self, message);
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

void
axis_meta_limiter_failed(
	AxisMetaLimiter *self,
	const gchar     *message
){
	g_mutex_lock(&self->lock);
	self->failures++;
	if (self->failures >= 2) {
		guint shift = MIN(self->failures - 2, 16u);
		gint64 seconds = MIN((gint64)AXIS_META_LIMIT_BACKOFF_BASE_S << shift, (gint64)AXIS_META_LIMIT_BACKOFF_CAP_S);

		self->backoff_until = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;
		self->backoff_until_real = g_get_real_time() + seconds * G_USEC_PER_SEC;
	}
	note_error_locked(self, message);
	g_cond_broadcast(&self->changed);
	g_mutex_unlock(&self->lock);
}

void
axis_meta_limiter_answered(AxisMetaLimiter *self)
{
	g_mutex_lock(&self->lock);
	self->failures = 0;
	if (self->backoff_until != 0) {
		self->backoff_until = 0;
		self->backoff_until_real = 0;
		g_cond_broadcast(&self->changed);
	}
	g_mutex_unlock(&self->lock);
}

void
axis_meta_limiter_get_status(
	AxisMetaLimiter     *self,
	AxisMetaLimitStatus *status
){
	gint64 now = g_get_monotonic_time();

	g_mutex_lock(&self->lock);
	status->rate = self->rate;
	status->burst = self->burst;
	status->paused_until_us = self->paused_until > now ? self->paused_until_real : 0;
	status->backoff_until_us = self->backoff_until > now ? self->backoff_until_real : 0;
	status->failures = self->failures;
	status->requests = self->requests;
	status->last_request_us = self->last_request_real;
	status->last_error = g_strdup(self->last_error);
	status->last_error_us = self->last_error_real;
	g_mutex_unlock(&self->lock);
}

void
axis_meta_limit_status_clear(AxisMetaLimitStatus *status)
{
	if (status == NULL)
		return;
	g_clear_pointer(&status->last_error, g_free);
}
