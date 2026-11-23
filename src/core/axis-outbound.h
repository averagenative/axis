/*
 * axis-outbound.h - tying the daemon's outbound connections to one network
 * interface (a VPN's), and failing closed when it is not there
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * outbound.bind (AXIS_OUTBOUND_BIND, core/axis-outbound-parse.h) names an
 * interface ("tun0") or a local address it holds ("10.8.0.2").  Every
 * outbound connection of the daemon is made by core/axis-http.c, which asks
 * here, as each TCP connection is about to be made (after its socket is
 * created, before connect()), what to do with it:
 *
 *  - Whether the binding applies (axis_outbound_applies()): never to a
 *    loopback address; with scope "internet" (the default), to the indexers'
 *    traffic (searches, release files, the indexer's own site after a
 *    redirect), the metadata providers' (TMDb, its artwork) and a fetch of
 *    indexer definitions from Jackett's repository when the
 *    address connected to is public (core/axis-http.h,
 *    axis_http_address_is_public()), so an indexer such as a Prowlarr on the
 *    same network is reached directly; with scope "all", to every
 *    connection, the download clients', the media servers' and a Radarr's
 *    or Sonarr's too.  A
 *    connection of a kind that does not say what it is counts as an
 *    indexer's.
 *  - The interface's state, read from the system at that moment
 *    (axis_outbound_read()): whether it exists, is up (IFF_UP and
 *    IFF_RUNNING) and has an address of the connection's family (IPv4, or an
 *    IPv6 address that is not link-local).  With an address as the setting,
 *    the interface is whichever holds that address at that moment, and only
 *    that address (so only its family) is used.
 *  - When it applies and the interface is up with an address of that
 *    family: the socket is bound to the device itself (SO_BINDTODEVICE) and
 *    to that address (axis_outbound_bind_socket()).  The kernel then sends
 *    the connection's packets out of that interface only, whatever the
 *    routing table says; when the interface goes down or away they are not
 *    sent at all.  Binding the address alone would not do that: Linux routes
 *    a packet by its destination, so one from the VPN's address can still
 *    leave by the default route.
 *  - When it applies and the interface is missing, down or without an
 *    address, or the kernel refuses the device binding (before Linux 5.7 a
 *    process needs CAP_NET_RAW for it): the connection is not made, and the
 *    request fails at once with a message saying why ("the outbound
 *    interface tun0 is down").  Nothing falls back to the default route.
 *    When only the family is missing (the interface has IPv4 and the server
 *    an IPv6 address too), that one connection is not made and the server's
 *    other addresses are still tried.
 *
 * Not bound, whatever the scope: the database (libpq makes those
 * connections), name lookups (the system's resolver does them, through
 * whatever DNS server the host or container uses), `axisd --healthcheck`
 * (loopback), and axis-worker (a separate program that connects to the
 * daemon).
 *
 * The state is also read every AXIS_OUTBOUND_WATCH_SECONDS while the daemon
 * runs (axis_outbound_watch_start()), and logged once when it changes (and
 * once at start), never per request.  Start-up does not wait for the
 * interface, and the probes (/readyz) do not look at it.
 *
 * Thread-safe: the configuration and the last state read are behind one
 * lock.  Linux only (getifaddrs, SO_BINDTODEVICE).
 */

#ifndef AXIS_OUTBOUND_H
#define AXIS_OUTBOUND_H

#include <glib.h>

#include "core/axis-outbound-parse.h"

G_BEGIN_DECLS

#define AXIS_OUTBOUND_WATCH_SECONDS 10

/* At most this many interface entries (links and addresses) are read. */
#define AXIS_OUTBOUND_MAX_ENTRIES   4096

/**
 * AxisOutboundTraffic:
 * @AXIS_OUTBOUND_TRAFFIC_UNNAMED: a connection whose maker did not say what
 *   it is for: treated as an indexer's, so that one forgotten is bound
 * @AXIS_OUTBOUND_TRAFFIC_INDEXER: an indexer's searches and release files
 * @AXIS_OUTBOUND_TRAFFIC_METADATA: a metadata provider and its artwork
 * @AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT: a download client's API
 * @AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER: Plex or Jellyfin
 * @AXIS_OUTBOUND_TRAFFIC_ARR: a person's Radarr or Sonarr, read to bring
 *   their library over (arr/axis-arr.h): a local service, treated as a
 *   download client's
 * @AXIS_OUTBOUND_TRAFFIC_DEFINITIONS: indexer definitions an administrator
 *   fetches from Jackett's repository at GitHub (indexer/axis-indexer.h):
 *   the internet, bound as an indexer's is
 */
typedef enum
{
	AXIS_OUTBOUND_TRAFFIC_UNNAMED = 0,
	AXIS_OUTBOUND_TRAFFIC_INDEXER,
	AXIS_OUTBOUND_TRAFFIC_METADATA,
	AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT,
	AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER,
	AXIS_OUTBOUND_TRAFFIC_ARR,
	AXIS_OUTBOUND_TRAFFIC_DEFINITIONS
} AxisOutboundTraffic;

/* The largest value of AxisOutboundTraffic. */
#define AXIS_OUTBOUND_TRAFFIC_LAST AXIS_OUTBOUND_TRAFFIC_DEFINITIONS

/* Where a connection goes, as core/axis-http.c judges its address. */
typedef enum
{
	AXIS_OUTBOUND_TO_LOOPBACK,	/* this machine: 127.0.0.0/8, ::1 */
	AXIS_OUTBOUND_TO_LOCAL,		/* a private, shared, link-local ... network */
	AXIS_OUTBOUND_TO_PUBLIC		/* the internet */
} AxisOutboundDestination;

typedef enum
{
	AXIS_OUTBOUND_STATE_OFF = 0,	/* no binding is configured */
	AXIS_OUTBOUND_STATE_UP,
	AXIS_OUTBOUND_STATE_MISSING,	/* no such interface, or none holds the address */
	AXIS_OUTBOUND_STATE_DOWN,
	AXIS_OUTBOUND_STATE_NO_ADDRESS
} AxisOutboundCondition;

/**
 * AxisOutboundState:
 * @condition: what was found
 * @device: the interface connections are bound to ("" when missing)
 * @has_ipv4: an IPv4 address to bind, @ipv4
 * @has_ipv6: an IPv6 address (not link-local) to bind, @ipv6
 *
 * A plain value: copy it with "=".
 */
typedef struct
{
	AxisOutboundCondition condition;
	gchar                 device[16];
	gboolean              has_ipv4;
	guint8                ipv4[4];
	gboolean              has_ipv6;
	guint8                ipv6[16];
} AxisOutboundState;

/**
 * AxisOutboundEntry:
 * @name: the device ("eth0" for an address labelled "eth0:1")
 * @flags: its flags (IFF_UP, IFF_RUNNING ...)
 * @family: AF_INET or AF_INET6 for an address, 0 for the link itself
 * @address: the address's 4 or 16 bytes
 *
 * One line of what the system says about its interfaces: what getifaddrs()
 * gives, without the parts that do not matter here.
 */
typedef struct
{
	gchar  name[16];
	guint  flags;
	gint   family;
	guint8 address[16];
} AxisOutboundEntry;

typedef enum
{
	AXIS_OUTBOUND_PASS,		/* not bound: the system's routes */
	AXIS_OUTBOUND_BIND,		/* bind to the device and its address */
	AXIS_OUTBOUND_REFUSE,		/* the request fails; nothing is sent */
	AXIS_OUTBOUND_REFUSE_FAMILY	/* this connection is not made; another of the server's addresses may be */
} AxisOutboundVerdict;

/* ── Pure parts (the tests give them made-up interfaces) ─────────── */

/* Whether @traffic is of the kinds the "internet" scope binds. */
gboolean
axis_outbound_traffic_is_internet(AxisOutboundTraffic traffic);

/* Whether a connection of @traffic to @destination is bound under @target
 * and @scope (whatever the interface's state). */
gboolean
axis_outbound_applies(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	AxisOutboundTraffic       traffic,
	AxisOutboundDestination   destination
);

/**
 * axis_outbound_resolve:
 * @target: the setting
 * @entries: (array length=n_entries): the system's interfaces
 * @state: (out): what @target finds among them
 *
 * An interface name finds the entries of that device; an address finds the
 * device that holds it.  Up means IFF_UP and IFF_RUNNING both.
 */
void
axis_outbound_resolve(
	const AxisOutboundTarget *target,
	const AxisOutboundEntry  *entries,
	guint                     n_entries,
	AxisOutboundState        *state
);

/**
 * axis_outbound_decide:
 * @family: the connection's, AF_INET or AF_INET6
 * @message: (out) (optional) (transfer full): for a refusal, why, without a
 *   full stop ("the outbound interface tun0 is down")
 *
 * What to do with one connection.
 */
AxisOutboundVerdict
axis_outbound_decide(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	AxisOutboundTraffic       traffic,
	AxisOutboundDestination   destination,
	gint                      family,
	const AxisOutboundState  *state,
	gchar                   **message
);

/* Why connections are refused in @state ("the outbound interface tun0 is
 * down"), or %NULL when it is up or off. */
gchar *
axis_outbound_describe_problem(
	const AxisOutboundTarget *target,
	const AxisOutboundState  *state
);

/* The addresses @state binds, "10.8.0.2, fd00::2", or "none". */
gchar *
axis_outbound_describe_addresses(const AxisOutboundState *state);

/* Whether @a and @b would be described alike (for the log). */
gboolean
axis_outbound_state_equal(
	const AxisOutboundState *a,
	const AxisOutboundState *b
);

/* ── The system ─────────────────────────────────────────────────── */

/**
 * axis_outbound_read_entries:
 *
 * Returns: (transfer full) (element-type AxisOutboundEntry): the system's
 *   interfaces now (an empty array when they cannot be read, which finds no
 *   interface: connections that must be bound are then refused)
 */
GArray *
axis_outbound_read_entries(void);

/**
 * axis_outbound_bind_socket:
 * @fd: an unconnected TCP socket of @family
 * @state: an %AXIS_OUTBOUND_STATE_UP state with an address of @family
 * @message: (out) (optional) (transfer full): why not, on failure
 *
 * Binds @fd to @state's device (SO_BINDTODEVICE) and then to its address of
 * @family, with any port.
 *
 * Returns: %TRUE when both are done
 */
gboolean
axis_outbound_bind_socket(
	gint                     fd,
	gint                     family,
	const AxisOutboundState *state,
	gchar                  **message
);

/**
 * axis_outbound_device_binding_check:
 * @message: (out) (optional) (transfer full): why not, when it is refused
 *
 * Whether this process may bind a socket to a device: tried on a socket of
 * its own, to the loopback device.
 *
 * Returns: %TRUE when the kernel allows it
 */
gboolean
axis_outbound_device_binding_check(gchar **message);

/* ── The daemon's binding ───────────────────────────────────────── */

/**
 * axis_outbound_configure:
 * @target: (nullable): the setting; %NULL (or one that is off) for none
 * @scope: its scope
 *
 * Sets the binding every later connection follows, forgets the last state
 * read, and reads (and logs) the state once.  The daemon calls it at start;
 * the tests call it as they please (%NULL to end).
 */
void
axis_outbound_configure(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope
);

/* The binding in effect (off until configured). */
void
axis_outbound_get_config(
	AxisOutboundTarget *target,
	AxisOutboundScope  *scope
);

/* Whether any connection of @traffic can be bound now: a binding is
 * configured and its scope covers @traffic for some destination. */
gboolean
axis_outbound_may_apply(AxisOutboundTraffic traffic);

/**
 * axis_outbound_read:
 * @state: (out): the configured interface's state now
 *
 * Reads the system's interfaces; when the state differs from the last one
 * read, logs the new one (a warning when connections are refused, a
 * message when the interface is up).
 */
void
axis_outbound_read(AxisOutboundState *state);

/* While the default main context runs: reads the state every
 * AXIS_OUTBOUND_WATCH_SECONDS, so that a change is logged when it happens
 * and not at the next request.  Nothing when no binding is configured. */
void
axis_outbound_watch_start(void);

void
axis_outbound_watch_stop(void);

G_END_DECLS

#endif /* AXIS_OUTBOUND_H */
