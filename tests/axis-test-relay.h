/*
 * axis-test-relay.h - test support: a TCP relay that can make one
 * connection go silent without closing it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Between a client and PostgreSQL, on 127.0.0.1 at a port of its own.  Each
 * accepted connection gets a number, from 1, in the order accepted, and its
 * own threads, so one connection can be stalled (nothing passes either way,
 * both ends stay open: a server that has stopped answering, or a network
 * that went quiet) while the others carry on.  New connections can also be
 * held: accepted and never answered.  Every wait is bounded; freeing the
 * relay closes everything.
 */

#ifndef AXIS_TEST_RELAY_H
#define AXIS_TEST_RELAY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisTestRelay AxisTestRelay;

AxisTestRelay *
axis_test_relay_new(
	const gchar *upstream_host,
	guint16      upstream_port
);

void
axis_test_relay_free(AxisTestRelay *self);

guint16
axis_test_relay_get_port(AxisTestRelay *self);

/* How many connections have been accepted so far. */
guint
axis_test_relay_count(AxisTestRelay *self);

/* While @stalled, connection @n passes nothing in either direction. */
void
axis_test_relay_stall(
	AxisTestRelay *self,
	guint          n,
	gboolean       stalled
);

/* While @hold, new connections are accepted and never answered (they are
 * closed when the relay is freed). */
void
axis_test_relay_hold_new(
	AxisTestRelay *self,
	gboolean       hold
);

G_END_DECLS

#endif /* AXIS_TEST_RELAY_H */
