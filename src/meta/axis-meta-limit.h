/*
 * axis-meta-limit.h - one metadata provider's request budget: a token
 * bucket, a pause when the provider asks for one, and a back-off after
 * repeated failures
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every request to a provider (its API and its image host) takes a token
 * from that provider's limiter first, whoever makes it: searches from a
 * page, the refresh job, artwork.  Each provider has its own, so one that
 * is slow, paused or failing never delays another.
 *
 *  - The bucket holds @burst tokens and refills at @rate a second (a
 *    generic cell-rate algorithm: in any stretch of t seconds at most
 *    burst + rate * t requests start).
 *  - A pause (a 429, or Retry-After on a 503) stops every request until the
 *    time stated.
 *  - After the second failure in a row (the provider unreachable, timing
 *    out or answering 5xx), requests wait for a back-off that doubles with
 *    each further failure (5 s, 10 s, ... up to 10 minutes); a success ends
 *    it.
 *
 * A caller says how long it may wait: a request that could not start
 * before then is refused at once, without waiting for nothing.
 *
 * GLib only; thread-safe.
 */

#ifndef AXIS_META_LIMIT_H
#define AXIS_META_LIMIT_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define AXIS_META_LIMIT_BACKOFF_BASE_S 5
#define AXIS_META_LIMIT_BACKOFF_CAP_S  600
/* A pause asked for without a time lasts this long. */
#define AXIS_META_LIMIT_DEFAULT_PAUSE_S 60
/* No pause is longer than this. */
#define AXIS_META_LIMIT_MAX_PAUSE_S    3600

typedef struct _AxisMetaLimiter AxisMetaLimiter;

AxisMetaLimiter *
axis_meta_limiter_new(
	gdouble rate,
	guint   burst
);

void
axis_meta_limiter_free(AxisMetaLimiter *self);

/* A new rate (requests a second, clamped to 0.01 .. 1000) and burst (1 ..
 * 100); waiting callers see it at once. */
void
axis_meta_limiter_set_rate(
	AxisMetaLimiter *self,
	gdouble          rate,
	guint            burst
);

typedef enum
{
	AXIS_META_LIMIT_OK,		/* a token was taken: send the request */
	AXIS_META_LIMIT_PAUSED,		/* paused or backing off beyond the deadline */
	AXIS_META_LIMIT_DEADLINE,	/* no token before the deadline */
	AXIS_META_LIMIT_CANCELLED
} AxisMetaLimitResult;

/**
 * axis_meta_limiter_acquire:
 * @deadline_us: the latest monotonic time (g_get_monotonic_time()) a
 *   request may start, or %G_MAXINT64 for none
 * @cancellable: (nullable): ends the wait
 * @until_us: (out) (optional): for %AXIS_META_LIMIT_PAUSED, the real time
 *   (g_get_real_time()) the pause or back-off ends
 *
 * Waits for a token, as long as the deadline allows.
 */
AxisMetaLimitResult
axis_meta_limiter_acquire(
	AxisMetaLimiter *self,
	gint64           deadline_us,
	GCancellable    *cancellable,
	gint64          *until_us
);

/* The provider asked to be left alone for @seconds (negative: it did not
 * say how long, so AXIS_META_LIMIT_DEFAULT_PAUSE_S); @message (nullable,
 * already safe to show) is kept as the last error. */
void
axis_meta_limiter_pause(
	AxisMetaLimiter *self,
	gint64           seconds,
	const gchar     *message
);

/* The request failed in a way that counts toward the back-off; @message
 * (already safe to show) is kept as the last error. */
void
axis_meta_limiter_failed(
	AxisMetaLimiter *self,
	const gchar     *message
);

/* The provider answered (whatever it said): the back-off ends. */
void
axis_meta_limiter_answered(AxisMetaLimiter *self);

/**
 * AxisMetaLimitStatus:
 * @rate, @burst: the budget
 * @paused_until_us: real time the provider's pause ends, 0 for none
 * @backoff_until_us: real time the back-off ends, 0 for none
 * @failures: failures in a row
 * @requests: requests started since the daemon started
 * @last_request_us: real time of the latest, 0 for none
 * @last_error: (nullable): the latest failure or pause, safe to show
 * @last_error_us: when
 */
typedef struct
{
	gdouble  rate;
	guint    burst;
	gint64   paused_until_us;
	gint64   backoff_until_us;
	guint    failures;
	guint64  requests;
	gint64   last_request_us;
	gchar   *last_error;
	gint64   last_error_us;
} AxisMetaLimitStatus;

void
axis_meta_limiter_get_status(
	AxisMetaLimiter     *self,
	AxisMetaLimitStatus *status
);

void
axis_meta_limit_status_clear(AxisMetaLimitStatus *status);

/**
 * axis_meta_gcra:
 * @tat: (inout): the theoretical arrival time (µs); 0 at first
 * @now_us: the time of the request
 * @interval_us: 1 / rate
 * @burst: the bucket size, 1 or more
 * @take: whether to take the slot when it conforms now
 *
 * The arithmetic of the bucket, for the tests.
 *
 * Returns: the earliest time (µs) at which a request conforms: @now_us or
 *   later.  When that is @now_us and @take is set, @tat is advanced.
 */
gint64
axis_meta_gcra(
	gint64  *tat,
	gint64   now_us,
	gint64   interval_us,
	guint    burst,
	gboolean take
);

G_END_DECLS

#endif /* AXIS_META_LIMIT_H */
