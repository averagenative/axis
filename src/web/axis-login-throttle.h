/*
 * axis-login-throttle.h - exponential delay after failed logins
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Counts failures per key (a username, or a client address) in memory.
 * After a number of free failures, each further failure locks the key for
 * a delay that doubles each time, up to a cap; while locked, attempts are
 * refused before any password work.  A key nobody has failed on for a day
 * is forgotten.  At most a fixed number of keys are tracked: when the
 * table is full, forgotten keys are dropped first, then the key whose last
 * failure is oldest (never one with attempts in flight or still in its
 * delay: then the table grows by one instead).  A throttle told
 * axis_login_throttle_share_when_full() drops no key that has failures
 * instead: see there.
 *
 * An attempt whose password is checked off the main loop is admitted with
 * axis_login_throttle_begin() and ended with axis_login_throttle_end().
 * Until it ends it counts as the failure it may be: no more attempts are
 * admitted at once than would have been let through one after another (the
 * free failures and one more, then one at a time each time a delay ends).
 *
 * Time is passed in by the caller (monotonic microseconds), so tests can
 * drive it.  Not thread-safe: use it from one thread (the main loop's).
 */

#ifndef AXIS_LOGIN_THROTTLE_H
#define AXIS_LOGIN_THROTTLE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisLoginThrottle AxisLoginThrottle;

/**
 * axis_login_throttle_new:
 * @free_failures: failures allowed before the first delay
 * @base_delay_us: the first delay
 * @max_delay_us: the longest delay
 * @forget_us: a key with no failure for this long is forgotten
 * @max_keys: the most keys tracked at once (at least 1)
 *
 * Returns: (transfer full): a throttle
 */
AxisLoginThrottle *
axis_login_throttle_new(
	guint  free_failures,
	gint64 base_delay_us,
	gint64 max_delay_us,
	gint64 forget_us,
	guint  max_keys
);

void
axis_login_throttle_free(AxisLoginThrottle *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisLoginThrottle, axis_login_throttle_free)

/**
 * axis_login_throttle_share_when_full:
 * @self: a throttle
 * @shared_forget_us: the shared count is forgotten after this long
 *   without a failure
 *
 * From now on a full table makes room only by dropping keys that are
 * forgotten.  A key that is not in a full table is counted, with every
 * other such key, on one count of the throttle's own (its failures, its
 * delay, its attempts in flight), until room is made.  A sender who
 * changes keys (an address that is one of many) then cannot make the table
 * forget the keys it used and start each of them again: while the table is
 * full, every new key it brings shares one count.  The table never grows
 * past its bound.  (add-request-portal: axis-portal's throttles.)
 */
void
axis_login_throttle_share_when_full(
	AxisLoginThrottle *self,
	gint64             shared_forget_us
);

/**
 * axis_login_throttle_check:
 * @self: a throttle
 * @key: (nullable): what is being tried; %NULL is never locked
 * @now_us: the current monotonic time
 *
 * Returns: 0 if an attempt for @key may go ahead now, else how many
 *   microseconds remain until it may (while attempts are in flight, the
 *   delay their failures would start, at least a second)
 */
gint64
axis_login_throttle_check(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
);

/**
 * axis_login_throttle_fail:
 * @self: a throttle
 * @key: (nullable): what failed; %NULL is ignored
 * @now_us: the current monotonic time
 *
 * Records a failure for @key.
 */
void
axis_login_throttle_fail(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
);

/* Admits an attempt for @key (%NULL is ignored): it counts until
 * axis_login_throttle_end().  Call only after a check that returned 0. */
void
axis_login_throttle_begin(
	AxisLoginThrottle *self,
	const gchar       *key,
	gint64             now_us
);

/* Ends an attempt admitted with axis_login_throttle_begin(); record its
 * failure with axis_login_throttle_fail() as well when it failed. */
void
axis_login_throttle_end(
	AxisLoginThrottle *self,
	const gchar       *key
);

/* Forgets every failure of @key (a successful login); attempts still in
 * flight keep counting. */
void
axis_login_throttle_clear(
	AxisLoginThrottle *self,
	const gchar       *key
);

/* How many attempts of @key are in flight; for tests. */
guint
axis_login_throttle_get_pending(
	AxisLoginThrottle *self,
	const gchar       *key
);

/* How many keys are tracked right now. */
guint
axis_login_throttle_get_size(AxisLoginThrottle *self);

G_END_DECLS

#endif /* AXIS_LOGIN_THROTTLE_H */
