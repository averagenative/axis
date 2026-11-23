/*
 * axis-proxy-resolver.h - keeps the addresses of trusted proxy names current
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An entry of server.trusted_proxies may be a host name.  This looks the
 * names of an AxisProxyList up through GLib's default resolver (so in a
 * container through Docker's or Kubernetes' DNS), asynchronously on the main
 * loop: nothing that handles a request ever waits for a lookup.
 *
 * When it looks:
 *   - at start;
 *   - every AXIS_PROXY_RESOLVE_PERIOD_S seconds: a recreated proxy that
 *     gets a new address is believed again within one period at the most;
 *   - at once, but at most once every AXIS_PROXY_RESOLVE_MIN_INTERVAL_US,
 *     when a request arrives from an address that is not trusted while the
 *     list has a name (axis_proxy_resolver_poke()): that is what a proxy at
 *     its new address looks like, so it is believed again within seconds.
 *
 * A round asks for every name; a round is not started while the last one is
 * still running.  What each lookup found goes to axis_proxy_list_name_result(),
 * which holds the failure rules.  A change of what a name stands for is
 * logged, once.
 */

#ifndef AXIS_PROXY_RESOLVER_H
#define AXIS_PROXY_RESOLVER_H

#include <gio/gio.h>

#include "core/axis-proxy.h"

G_BEGIN_DECLS

/* The period of the lookups.  Thirty seconds: a recreated container is
 * found again in half a minute at the worst even if no request arrives from
 * its new address, and a resolver is asked only 2 times a minute for each
 * name, which no DNS server minds. */
#define AXIS_PROXY_RESOLVE_PERIOD_S 30

/* The shortest time between two rounds started by requests. */
#define AXIS_PROXY_RESOLVE_MIN_INTERVAL_US (5 * G_USEC_PER_SEC)

typedef struct _AxisProxyResolver AxisProxyResolver;

/**
 * axis_proxy_resolver_new:
 * @list: the list whose names are kept current; it must outlive the resolver
 *
 * Takes the thread's default main context as the one the lookups are
 * answered on.  Nothing is looked up until axis_proxy_resolver_start().
 *
 * Returns: (transfer full): a resolver
 */
AxisProxyResolver *
axis_proxy_resolver_new(AxisProxyList *list);

/** Starts the first round and the periodic timer. */
void
axis_proxy_resolver_start(AxisProxyResolver *self);

/**
 * axis_proxy_resolver_poke:
 *
 * Asks for a round now, unless one ran, or was asked for, less than
 * %AXIS_PROXY_RESOLVE_MIN_INTERVAL_US ago.  Never blocks; safe from any
 * thread; does nothing once stopped.
 */
void
axis_proxy_resolver_poke(AxisProxyResolver *self);

/** Cancels the lookups under way and the timer.  Idempotent. */
void
axis_proxy_resolver_stop(AxisProxyResolver *self);

void
axis_proxy_resolver_free(AxisProxyResolver *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisProxyResolver, axis_proxy_resolver_free)

/** Whether a round is running; for tests. */
gboolean
axis_proxy_resolver_is_busy(AxisProxyResolver *self);

G_END_DECLS

#endif /* AXIS_PROXY_RESOLVER_H */
