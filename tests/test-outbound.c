/*
 * test-outbound.c - outbound.bind: the value, the interface's state, the
 * decision for each connection, and, in a private network namespace, what
 * actually leaves by which interface
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first part needs nothing: the parser, the resolver and the decision
 * are given made-up interfaces, and the sources are read to check that every
 * outbound connection is made in one place and says what it is for.
 *
 * The second part runs each case in a child process in its own user and
 * network namespace (unshare(CLONE_NEWUSER | CLONE_NEWNET)), where two tun
 * devices stand in for the networks: tun1 is the "LAN" and carries the
 * default route, tun0 is the "VPN".  The test holds both devices' file
 * descriptors, so every packet the kernel sends out of either is read here:
 * "nothing was sent" means no TCP packet left by any interface, and "left
 * from that interface's address" means the SYN came out of tun0 with tun0's
 * address as its source.  The interfaces are made while the child still has
 * its namespace's capabilities; then it drops every capability (capset) and
 * runs the daemon's own HTTP client, as an unprivileged daemon with all
 * capabilities dropped would.  Where unprivileged user and network
 * namespaces or /dev/net/tun are not available (inside an image build, for
 * one), or the kernel is older than 5.7 (when binding to a device needed
 * CAP_NET_RAW), those cases skip, saying why.
 *
 * The addresses are made up, inside a namespace with no way out: 1.2.3.4
 * and 2606:4700:4700::1111 are "public" only in the sense that
 * axis_http_address_is_public() says so.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <linux/capability.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "core/axis-http.h"
#include "core/axis-outbound.h"
#include "core/axis-outbound-parse.h"

#define PUBLIC_V4   "1.2.3.4"
#define PUBLIC_V6   "2606:4700:4700::1111"
#define LOCAL_V4    "192.168.77.50"
#define LAN_V4      "192.168.77.2"
#define LAN_V6      "fd00:77::2"
#define VPN_V4      "10.8.0.2"
#define VPN_V6      "fd00:8::2"

/* ── The value ──────────────────────────────────────────────────── */

static void
assert_parses(
	const gchar     *value,
	AxisOutboundKind kind,
	const gchar     *text
){
	AxisOutboundTarget target;
	g_autofree gchar *problem = NULL;

	if (!axis_outbound_target_parse(value, &target, &problem))
		g_error("'%s' was refused: %s", value, problem);
	g_assert_null(problem);
	g_assert_cmpint(target.kind, ==, kind);
	g_assert_cmpstr(target.text, ==, text);
}

static void
assert_refused(
	const gchar *value,
	const gchar *part
){
	AxisOutboundTarget target;
	g_autofree gchar *problem = NULL;

	if (axis_outbound_target_parse(value, &target, &problem))
		g_error("'%s' was accepted as '%s'", value, target.text);
	g_assert_nonnull(problem);
	g_assert_cmpint(target.kind, ==, AXIS_OUTBOUND_OFF);
	if (strstr(problem, part) == NULL)
		g_error("the problem with '%s' is \"%s\", which does not say \"%s\"", value, problem, part);
}

/* Spec: The value names an interface or an address */
static void
test_parse_forms(void)
{
	AxisOutboundTarget target;
	AxisOutboundScope scope = AXIS_OUTBOUND_SCOPE_ALL;
	g_autofree gchar *problem = NULL;
	gchar *long_value;

	/* Off. */
	g_assert_true(axis_outbound_target_parse(NULL, &target, &problem));
	g_assert_cmpint(target.kind, ==, AXIS_OUTBOUND_OFF);
	g_assert_cmpstr(target.text, ==, "");
	assert_parses("", AXIS_OUTBOUND_OFF, "");
	assert_parses("  \t ", AXIS_OUTBOUND_OFF, "");

	/* Interfaces. */
	assert_parses("tun0", AXIS_OUTBOUND_INTERFACE, "tun0");
	assert_parses(" wg0 ", AXIS_OUTBOUND_INTERFACE, "wg0");
	assert_parses("eth0.100", AXIS_OUTBOUND_INTERFACE, "eth0.100");
	assert_parses("br-1a2b3c4d5e6f", AXIS_OUTBOUND_INTERFACE, "br-1a2b3c4d5e6f");
	assert_parses("vpn_pia-2", AXIS_OUTBOUND_INTERFACE, "vpn_pia-2");
	assert_parses("123abc", AXIS_OUTBOUND_INTERFACE, "123abc");

	/* Addresses, in canonical form. */
	assert_parses(VPN_V4, AXIS_OUTBOUND_ADDRESS, VPN_V4);
	assert_parses("100.64.0.7", AXIS_OUTBOUND_ADDRESS, "100.64.0.7");
	assert_parses("FD00:0000:0::2", AXIS_OUTBOUND_ADDRESS, "fd00::2");
	assert_parses("2a01:4f8::1", AXIS_OUTBOUND_ADDRESS, "2a01:4f8::1");
	g_assert_true(axis_outbound_target_parse(VPN_V4, &target, NULL));
	g_assert_cmpint(target.family, ==, AF_INET);
	g_assert_cmpuint(target.address[0], ==, 10);
	g_assert_cmpuint(target.address[3], ==, 2);
	g_assert_true(axis_outbound_target_parse("fd00::2", &target, NULL));
	g_assert_cmpint(target.family, ==, AF_INET6);
	g_assert_cmpuint(target.address[0], ==, 0xfd);
	g_assert_cmpuint(target.address[15], ==, 2);

	/* Refused, each saying why. */
	assert_refused("tun 0", "is not an interface name");
	assert_refused("tun0/1", "is not an interface name");
	assert_refused("averyveryverylong0", "is not an interface name");
	/* Digits and dots alone are an IPv4 address, or a mistake. */
	assert_refused(".", "is not an IPv4 address");
	assert_refused("..", "is not an IPv4 address");
	assert_refused("tun\x01", "the value is not an interface name");
	assert_refused("t\xc3\xbcn0", "is not an interface name");
	assert_refused("10.8.0", "is not an IPv4 address");
	assert_refused("10.8.0.256", "is not an IPv4 address");
	assert_refused("010.8.0.2", "is not an IPv4 address");
	assert_refused("1.2.3.4.5", "is not an IPv4 address");
	assert_refused("eth0:1", "is not an IPv6 address");
	assert_refused("fd00::2::3", "is not an IPv6 address");
	assert_refused("fe80::1%tun0", "has a zone");
	assert_refused("fe80::1", "link-local");
	assert_refused("::ffff:10.8.0.2", "written as IPv6");
	assert_refused("::10.8.0.2", "written as IPv6");
	assert_refused("0.0.0.0", "not an address a connection can come from");
	assert_refused("0.1.2.3", "not an address a connection can come from");
	assert_refused("::", "not an address a connection can come from");
	assert_refused("127.0.0.1", "loopback");
	assert_refused("::1", "loopback");
	assert_refused("224.0.0.1", "multicast");
	assert_refused("ff02::1", "multicast");
	assert_refused("255.255.255.255", "broadcast");
	long_value = g_strnfill(AXIS_OUTBOUND_MAX_VALUE_BYTES + 1, 'a');
	assert_refused(long_value, "longer than");
	g_free(long_value);

	/* The scope. */
	g_assert_true(axis_outbound_scope_parse(NULL, &scope));
	g_assert_cmpint(scope, ==, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_true(axis_outbound_scope_parse("all", &scope));
	g_assert_cmpint(scope, ==, AXIS_OUTBOUND_SCOPE_ALL);
	g_assert_true(axis_outbound_scope_parse("internet", &scope));
	g_assert_cmpint(scope, ==, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(axis_outbound_scope_parse("All", &scope));
	g_assert_false(axis_outbound_scope_parse("everything", &scope));
	g_assert_false(axis_outbound_scope_parse(" all", &scope));
	g_assert_cmpint(scope, ==, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_cmpstr(axis_outbound_scope_to_string(AXIS_OUTBOUND_SCOPE_ALL), ==, "all");
	g_assert_cmpstr(axis_outbound_scope_to_string(AXIS_OUTBOUND_SCOPE_INTERNET), ==, "internet");
}

/* ── The interface's state ──────────────────────────────────────── */

static AxisOutboundEntry
entry(
	const gchar *name,
	guint        flags,
	const gchar *address
){
	AxisOutboundEntry made;

	memset(&made, 0, sizeof(made));
	g_strlcpy(made.name, name, sizeof(made.name));
	made.flags = flags;
	if (address == NULL)
		made.family = 0;
	else if (strchr(address, ':') != NULL) {
		made.family = AF_INET6;
		g_assert_cmpint(inet_pton(AF_INET6, address, made.address), ==, 1);
	} else {
		made.family = AF_INET;
		g_assert_cmpint(inet_pton(AF_INET, address, made.address), ==, 1);
	}
	return made;
}

static AxisOutboundTarget
target_of(const gchar *value)
{
	AxisOutboundTarget target;

	g_assert_true(axis_outbound_target_parse(value, &target, NULL));
	return target;
}

#define UP_FLAGS (IFF_UP | IFF_RUNNING | IFF_POINTOPOINT)

/* Spec: The interface's state decides */
static void
test_resolve(void)
{
	AxisOutboundEntry entries[11];
	AxisOutboundTarget target;
	AxisOutboundState state;
	g_autofree gchar *addresses = NULL;
	g_autofree gchar *problem = NULL;
	guint n = 0;

	entries[n++] = entry("lo", IFF_UP | IFF_RUNNING | IFF_LOOPBACK, NULL);
	entries[n++] = entry("lo", IFF_UP | IFF_RUNNING | IFF_LOOPBACK, "127.0.0.1");
	entries[n++] = entry("eth0", IFF_UP | IFF_RUNNING, NULL);
	entries[n++] = entry("eth0", IFF_UP | IFF_RUNNING, "192.168.1.20");
	entries[n++] = entry("tun0", UP_FLAGS, NULL);
	entries[n++] = entry("tun0", UP_FLAGS, "fe80::1234");
	entries[n++] = entry("tun0", UP_FLAGS, VPN_V4);
	entries[n++] = entry("tun0", UP_FLAGS, VPN_V6);
	entries[n++] = entry("wg0", IFF_UP, NULL);		/* up, not running: no carrier */
	entries[n++] = entry("wg0", IFF_UP, "10.9.0.2");
	entries[n++] = entry("tun9", UP_FLAGS, "fe80::9");	/* a link-local address only */

	/* By name: up, with its IPv4 address and its IPv6 address that is not
	 * link-local. */
	target = target_of("tun0");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_UP);
	g_assert_cmpstr(state.device, ==, "tun0");
	g_assert_true(state.has_ipv4);
	g_assert_true(state.has_ipv6);
	addresses = axis_outbound_describe_addresses(&state);
	g_assert_cmpstr(addresses, ==, VPN_V4 ", " VPN_V6);
	g_assert_null(axis_outbound_describe_problem(&target, &state));

	/* Missing, down (no carrier counts), no address. */
	target = target_of("tun5");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_MISSING);
	problem = axis_outbound_describe_problem(&target, &state);
	g_assert_cmpstr(problem, ==, "the outbound interface tun5 does not exist");
	g_clear_pointer(&problem, g_free);
	target = target_of("wg0");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_DOWN);
	problem = axis_outbound_describe_problem(&target, &state);
	g_assert_cmpstr(problem, ==, "the outbound interface wg0 is down");
	g_clear_pointer(&problem, g_free);
	target = target_of("tun9");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_NO_ADDRESS);
	problem = axis_outbound_describe_problem(&target, &state);
	g_assert_cmpstr(problem, ==, "the outbound interface tun9 has no address");
	g_clear_pointer(&problem, g_free);

	/* By address: the interface that holds it, and that family only. */
	target = target_of(VPN_V4);
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_UP);
	g_assert_cmpstr(state.device, ==, "tun0");
	g_assert_true(state.has_ipv4);
	g_assert_false(state.has_ipv6);
	target = target_of(VPN_V6);
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_UP);
	g_assert_false(state.has_ipv4);
	g_assert_true(state.has_ipv6);
	target = target_of("10.9.0.2");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_DOWN);
	problem = axis_outbound_describe_problem(&target, &state);
	g_assert_cmpstr(problem, ==, "the outbound address 10.9.0.2 is on wg0, which is down");
	g_clear_pointer(&problem, g_free);
	target = target_of("10.8.0.3");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_MISSING);
	problem = axis_outbound_describe_problem(&target, &state);
	g_assert_cmpstr(problem, ==, "no interface has the outbound address 10.8.0.3");
	g_clear_pointer(&problem, g_free);

	/* Nothing to read finds nothing: refused, not passed. */
	target = target_of("tun0");
	axis_outbound_resolve(&target, NULL, 0, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_MISSING);

	/* Off. */
	target = target_of("");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_OFF);
	g_clear_pointer(&addresses, g_free);
	addresses = axis_outbound_describe_addresses(&state);
	g_assert_cmpstr(addresses, ==, "none");
}

/* The system's own list has the loopback device, with its address. */
static void
test_read_entries(void)
{
	GArray *entries = axis_outbound_read_entries();
	gboolean has_link = FALSE;
	gboolean address = FALSE;
	guint i;

	for (i = 0; i < entries->len; i++) {
		const AxisOutboundEntry *one = &g_array_index(entries, AxisOutboundEntry, i);

		g_assert_null(strchr(one->name, ':'));
		if (strcmp(one->name, "lo") != 0)
			continue;
		if (one->family == 0)
			has_link = TRUE;
		if (one->family == AF_INET && one->address[0] == 127)
			address = TRUE;
	}
	g_array_unref(entries);
	if (!has_link && !address) {
		g_test_skip("this system has no loopback device to look at");
		return;
	}
	g_assert_true(address);
}

/* ── The decision ───────────────────────────────────────────────── */

/* Spec: Which traffic is bound */
static void
test_applies(void)
{
	AxisOutboundTarget off = target_of("");
	AxisOutboundTarget on = target_of("tun0");
	static const AxisOutboundTraffic internet[] = {
		AXIS_OUTBOUND_TRAFFIC_UNNAMED, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TRAFFIC_METADATA,
		AXIS_OUTBOUND_TRAFFIC_DEFINITIONS
	};
	static const AxisOutboundTraffic local[] = {
		AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT, AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER, AXIS_OUTBOUND_TRAFFIC_ARR
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(internet); i++) {
		g_assert_true(axis_outbound_traffic_is_internet(internet[i]));
		/* Internet: public addresses only. */
		g_assert_true(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_INTERNET, internet[i], AXIS_OUTBOUND_TO_PUBLIC));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_INTERNET, internet[i], AXIS_OUTBOUND_TO_LOCAL));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_INTERNET, internet[i],
			AXIS_OUTBOUND_TO_LOOPBACK));
		/* All: everything but loopback. */
		g_assert_true(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, internet[i], AXIS_OUTBOUND_TO_PUBLIC));
		g_assert_true(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, internet[i], AXIS_OUTBOUND_TO_LOCAL));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, internet[i], AXIS_OUTBOUND_TO_LOOPBACK));
		/* Off: nothing. */
		g_assert_false(axis_outbound_applies(&off, AXIS_OUTBOUND_SCOPE_ALL, internet[i], AXIS_OUTBOUND_TO_PUBLIC));
		g_assert_false(axis_outbound_applies(NULL, AXIS_OUTBOUND_SCOPE_ALL, internet[i], AXIS_OUTBOUND_TO_PUBLIC));
	}
	for (i = 0; i < G_N_ELEMENTS(local); i++) {
		g_assert_false(axis_outbound_traffic_is_internet(local[i]));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_INTERNET, local[i], AXIS_OUTBOUND_TO_PUBLIC));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_INTERNET, local[i], AXIS_OUTBOUND_TO_LOCAL));
		g_assert_true(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, local[i], AXIS_OUTBOUND_TO_PUBLIC));
		g_assert_true(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, local[i], AXIS_OUTBOUND_TO_LOCAL));
		g_assert_false(axis_outbound_applies(&on, AXIS_OUTBOUND_SCOPE_ALL, local[i], AXIS_OUTBOUND_TO_LOOPBACK));
	}
}

static AxisOutboundVerdict
decide(
	const AxisOutboundTarget *target,
	AxisOutboundTraffic       traffic,
	AxisOutboundDestination   destination,
	gint                      family,
	const AxisOutboundState  *state,
	gchar                   **message
){
	return axis_outbound_decide(target, AXIS_OUTBOUND_SCOPE_INTERNET, traffic, destination, family, state, message);
}

/* Spec: Fail closed when the interface is not there */
static void
test_decide(void)
{
	AxisOutboundEntry entries[6];
	AxisOutboundTarget target;
	AxisOutboundState state;
	gchar *message = NULL;
	guint n = 0;

	entries[n++] = entry("tun0", UP_FLAGS, VPN_V4);
	entries[n++] = entry("wg0", IFF_RUNNING, "10.9.0.2");	/* down */
	entries[n++] = entry("wg1", UP_FLAGS, NULL);		/* no address */
	entries[n++] = entry("tun6", UP_FLAGS, VPN_V6);

	/* Up, the right family: bound. */
	target = target_of("tun0");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, &state, &message),
		==, AXIS_OUTBOUND_BIND);
	g_assert_null(message);
	/* Up, no IPv6 address: this connection only. */
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET6, &state,
		&message), ==, AXIS_OUTBOUND_REFUSE_FAMILY);
	g_assert_cmpstr(message, ==, "the outbound interface tun0 has no IPv6 address");
	g_clear_pointer(&message, g_free);
	/* Not covered: passed, whatever the state. */
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_LOCAL, AF_INET, &state, &message),
		==, AXIS_OUTBOUND_PASS);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, &state,
		&message), ==, AXIS_OUTBOUND_PASS);
	g_assert_null(message);

	/* Missing, down, no address: refused, saying why. */
	target = target_of("tun1");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_METADATA, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, &state,
		&message), ==, AXIS_OUTBOUND_REFUSE);
	g_assert_cmpstr(message, ==, "the outbound interface tun1 does not exist");
	g_clear_pointer(&message, g_free);
	target = target_of("wg0");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_UNNAMED, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, &state, &message),
		==, AXIS_OUTBOUND_REFUSE);
	g_assert_cmpstr(message, ==, "the outbound interface wg0 is down");
	g_clear_pointer(&message, g_free);
	target = target_of("wg1");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET6, &state,
		&message), ==, AXIS_OUTBOUND_REFUSE);
	g_assert_cmpstr(message, ==, "the outbound interface wg1 has no address");
	g_clear_pointer(&message, g_free);
	/* Down, but the traffic is not covered: still passed. */
	target = target_of("wg0");
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER, AXIS_OUTBOUND_TO_LOCAL, AF_INET, &state,
		&message), ==, AXIS_OUTBOUND_PASS);
	/* With scope all, a local download client is refused while it is down. */
	g_assert_cmpint(axis_outbound_decide(&target, AXIS_OUTBOUND_SCOPE_ALL, AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT,
		AXIS_OUTBOUND_TO_LOCAL, AF_INET, &state, &message), ==, AXIS_OUTBOUND_REFUSE);
	g_assert_cmpstr(message, ==, "the outbound interface wg0 is down");
	g_clear_pointer(&message, g_free);
	/* No state at all: refused. */
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, NULL, &message),
		==, AXIS_OUTBOUND_REFUSE);
	g_assert_nonnull(message);
	g_clear_pointer(&message, g_free);

	/* By address: the other family is never used. */
	target = target_of(VPN_V6);
	axis_outbound_resolve(&target, entries, n, &state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_UP);
	g_assert_cmpstr(state.device, ==, "tun6");
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET6, &state,
		&message), ==, AXIS_OUTBOUND_BIND);
	g_assert_cmpint(decide(&target, AXIS_OUTBOUND_TRAFFIC_INDEXER, AXIS_OUTBOUND_TO_PUBLIC, AF_INET, &state,
		&message), ==, AXIS_OUTBOUND_REFUSE_FAMILY);
	g_assert_cmpstr(message, ==, "the outbound address " VPN_V6 " is IPv6, and this connection is IPv4");
	g_clear_pointer(&message, g_free);
}

static void
test_state_equal(void)
{
	AxisOutboundEntry entries[2];
	AxisOutboundTarget target = target_of("tun0");
	AxisOutboundState a;
	AxisOutboundState b;

	entries[0] = entry("tun0", UP_FLAGS, VPN_V4);
	axis_outbound_resolve(&target, entries, 1, &a);
	axis_outbound_resolve(&target, entries, 1, &b);
	g_assert_true(axis_outbound_state_equal(&a, &b));
	/* A reconnect that changes the address is a change. */
	entries[0] = entry("tun0", UP_FLAGS, "10.8.0.3");
	axis_outbound_resolve(&target, entries, 1, &b);
	g_assert_false(axis_outbound_state_equal(&a, &b));
	entries[0] = entry("tun0", IFF_RUNNING, VPN_V4);
	axis_outbound_resolve(&target, entries, 1, &b);
	g_assert_false(axis_outbound_state_equal(&a, &b));
}

/* Spec: Changes are logged once */
static void
test_configure_and_log(void)
{
	AxisOutboundTarget target;
	AxisOutboundState state;
	AxisOutboundScope scope;

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "outbound connections: *");

	/* Off until configured, and nothing may apply. */
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	axis_outbound_read(&state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_OFF);
	g_assert_false(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_INDEXER));

	/* A name that is not here: logged once, at start; not again on every
	 * read. */
	target = target_of("axis-absent0");
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING,
		"outbound connections: the outbound interface axis-absent0 does not exist; connections to indexers and "
		"metadata providers at public addresses are refused until it is up, and nothing is sent another way"), ==, 1);
	axis_outbound_read(&state);
	axis_outbound_read(&state);
	g_assert_cmpint(state.condition, ==, AXIS_OUTBOUND_STATE_MISSING);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*axis-absent0*"), ==, 1);
	g_assert_true(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_INDEXER));
	g_assert_true(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_UNNAMED));
	g_assert_false(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT));
	axis_outbound_get_config(&target, &scope);
	g_assert_cmpstr(target.text, ==, "axis-absent0");
	g_assert_cmpint(scope, ==, AXIS_OUTBOUND_SCOPE_INTERNET);

	/* The loopback device is up: logged as a message. */
	target = target_of("lo");
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_ALL);
	axis_outbound_read(&state);
	if (state.condition == AXIS_OUTBOUND_STATE_UP)
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "outbound connections: lo is up with 127.0.0.1*"),
			==, 1);
	g_assert_true(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT));

	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(axis_outbound_may_apply(AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT));
	axis_test_log_reset();
}

/* Spec: A connection that must be bound is refused at once.  Without the
 * namespace: the interface does not exist here, and the request fails
 * before anything is sent (the address is the discard port of a documentation
 * range; with scope all even a local one is covered). */
static void
test_request_refused(void)
{
	AxisOutboundTarget target = target_of("axis-absent0");
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	AxisHttpResponse response;
	g_autoptr(GError) error = NULL;
	GBytes *body = NULL;
	gint64 start;

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "outbound connections: *");
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_ALL);

	g_assert_true(axis_http_origin_parse("http://192.0.2.1:9", &origin, NULL));
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.peer = "the indexer";
	config.timeout_ms = 5000;
	config.traffic = AXIS_OUTBOUND_TRAFFIC_INDEXER;
	start = g_get_monotonic_time();
	g_assert_false(axis_http_get_full(&config, "/api", NULL, &body, &response, &error));
	g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT);
	g_assert_cmpstr(error->message, ==,
		"the outbound interface axis-absent0 does not exist; nothing was sent to the indexer");
	g_assert_false(response.sent);
	/* At once: not after the deadline. */
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2 * G_USEC_PER_SEC);
	axis_http_response_clear(&response);
	g_clear_error(&error);

	/* A session (a download client's) under scope all: the same. */
	config.traffic = AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT;
	config.peer = "the download client";
	{
		g_autoptr(AxisHttpSession) session = axis_http_session_new(&config, AXIS_HTTP_PURPOSE_DOWNLOAD_CLIENT);

		g_assert_false(axis_http_session_get(session, "/", NULL, &body, NULL, &error));
		g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT);
		g_assert_cmpstr(error->message, ==,
			"the outbound interface axis-absent0 does not exist; nothing was sent to the download client");
		g_clear_error(&error);
	}

	/* Loopback is never bound: the refusal there is the server's own. */
	axis_http_origin_clear(&origin);
	g_assert_true(axis_http_origin_parse("http://127.0.0.1:9", &origin, NULL));
	config.origin = &origin;
	g_assert_false(axis_http_get(&config, "/", NULL, &body, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "outbound interface"));
	g_clear_error(&error);

	axis_http_origin_clear(&origin);
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	axis_test_log_reset();
}

/* ── The sources ────────────────────────────────────────────────── */

static void
collect_sources(
	const gchar *dir,
	GPtrArray   *files
){
	g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
	const gchar *name;

	g_assert_nonnull(handle);
	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			collect_sources(path, files);
		else if (g_str_has_suffix(name, ".c"))
			g_ptr_array_add(files, g_steal_pointer(&path));
	}
}

/* Spec: Every outbound connection is made in one place.  libsoup sessions
 * are made in core/axis-http.c, and for `axisd --healthcheck` (loopback) in
 * axisd/main.c; nothing else in the daemon makes a socket client or calls
 * connect(); and every place that fills an AxisHttpConfig says what its
 * traffic is. */
static void
test_sources(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	guint configs = 0;
	guint i;

	collect_sources("src", files);
	g_assert_cmpuint(files->len, >, 100);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = NULL;

		g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
		if (strstr(text, "soup_session_new") != NULL && strcmp(path, "src/core/axis-http.c") != 0 &&
		    strcmp(path, "src/axisd/main.c") != 0)
			g_error("%s makes a libsoup session: outbound connections are made in core/axis-http.c", path);
		if (strstr(text, "g_socket_client_") != NULL || strstr(text, "G_TYPE_SOCKET_CLIENT") != NULL ||
		    strstr(text, "g_socket_connect(") != NULL || strstr(text, "soup_session_websocket") != NULL)
			g_error("%s makes a connection of its own: outbound connections are made in core/axis-http.c", path);
		if (strstr(text, "AxisHttpConfig config;") != NULL) {
			configs++;
			if (strstr(text, "config.traffic = AXIS_OUTBOUND_TRAFFIC_") == NULL)
				g_error("%s fills an AxisHttpConfig without saying what its traffic is (config.traffic)", path);
		}
	}
	/* Indexers, metadata, download clients, the media servers' reads,
	 * Jellyfin's rescan, the reading of a Radarr or Sonarr, and the fetch of
	 * indexer definitions from Jackett's repository. */
	g_assert_cmpuint(configs, ==, 7);
}

/* ── In a private network namespace ─────────────────────────────── */

static gboolean
write_file(
	const gchar *path,
	const gchar *text
){
	gint fd = open(path, O_WRONLY | O_CLOEXEC);
	gboolean ok;

	if (fd < 0)
		return FALSE;
	ok = write(fd, text, strlen(text)) == (gssize)strlen(text);
	close(fd);
	return ok;
}

static gboolean
enter_namespace(void)
{
	g_autofree gchar *uid_map = g_strdup_printf("0 %u 1\n", (guint)getuid());
	g_autofree gchar *gid_map = g_strdup_printf("0 %u 1\n", (guint)getgid());

	/* A test's subprocess is made undumpable (no core files), which would
	 * leave /proc/self/uid_map to root. */
	if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0)
		return FALSE;
	return write_file("/proc/self/setgroups", "deny") && write_file("/proc/self/uid_map", uid_map) &&
		write_file("/proc/self/gid_map", gid_map);
}

/* A tun device called @name, read without blocking; -1 if it cannot be made. */
static gint
tun_open(const gchar *name)
{
	struct ifreq ifr;
	gint fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);

	if (fd < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	g_strlcpy(ifr.ifr_name, name, IFNAMSIZ);
	if (ioctl(fd, TUNSETIFF, &ifr) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Whether this kernel lets a process without CAP_NET_RAW bind a socket to a
 * device (Linux 5.7 and later). */
static gboolean
kernel_allows_device_binding(void)
{
	struct utsname name;
	guint major = 0;
	guint minor = 0;

	if (uname(&name) != 0 || sscanf(name.release, "%u.%u", &major, &minor) != 2)
		return FALSE;
	return major > 5 || (major == 5 && minor >= 7);
}

/* Whether the cases below can run here; @why says what is missing. */
static gboolean
namespace_usable(const gchar **why)
{
	pid_t pid;
	gint status = 0;

	if (!kernel_allows_device_binding()) {
		*why = "Linux before 5.7 lets only a process with CAP_NET_RAW bind a socket to an interface";
		return FALSE;
	}
	pid = fork();
	if (pid == 0) {
		gint fd;

		if (!enter_namespace())
			_exit(1);
		fd = tun_open("tun0");
		_exit(fd >= 0 ? 0 : 2);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
		*why = "a child process could not be checked";
		return FALSE;
	}
	if (WEXITSTATUS(status) == 1) {
		*why = "unprivileged user and network namespaces are not available here";
		return FALSE;
	}
	if (WEXITSTATUS(status) != 0) {
		*why = "a tun device (/dev/net/tun) cannot be made in a network namespace here";
		return FALSE;
	}
	return TRUE;
}

static gint
control_socket(gint family)
{
	gint fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	g_assert_cmpint(fd, >=, 0);
	return fd;
}

static gint
link_flags(
	const gchar *name,
	gboolean     up
){
	struct ifreq ifr;
	gint fd = control_socket(AF_INET);
	gint result;

	memset(&ifr, 0, sizeof(ifr));
	g_strlcpy(ifr.ifr_name, name, IFNAMSIZ);
	g_assert_cmpint(ioctl(fd, SIOCGIFFLAGS, &ifr), ==, 0);
	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;
	result = ioctl(fd, SIOCSIFFLAGS, &ifr);
	if (result != 0)
		result = errno;
	close(fd);
	return result;
}

static void
link_set(
	const gchar *name,
	gboolean     up
){
	g_assert_cmpint(link_flags(name, up), ==, 0);
}

static void
add_ipv4(
	const gchar *name,
	const gchar *address,
	const gchar *mask
){
	struct ifreq ifr;
	struct sockaddr_in sin;
	gint fd = control_socket(AF_INET);

	memset(&ifr, 0, sizeof(ifr));
	memset(&sin, 0, sizeof(sin));
	g_strlcpy(ifr.ifr_name, name, IFNAMSIZ);
	sin.sin_family = AF_INET;
	g_assert_cmpint(inet_pton(AF_INET, address, &sin.sin_addr), ==, 1);
	memcpy(&ifr.ifr_addr, &sin, sizeof(sin));
	g_assert_cmpint(ioctl(fd, SIOCSIFADDR, &ifr), ==, 0);
	g_assert_cmpint(inet_pton(AF_INET, mask, &sin.sin_addr), ==, 1);
	memcpy(&ifr.ifr_netmask, &sin, sizeof(sin));
	g_assert_cmpint(ioctl(fd, SIOCSIFNETMASK, &ifr), ==, 0);
	close(fd);
}

/* The kernel's struct in6_ifreq (linux/ipv6.h, which clashes with netinet/in.h). */
typedef struct
{
	struct in6_addr address;
	guint32         prefix_length;
	gint            ifindex;
} In6IfReq;

static void
add_ipv6(
	const gchar *name,
	const gchar *address
){
	In6IfReq request;
	gint fd = control_socket(AF_INET6);

	memset(&request, 0, sizeof(request));
	g_assert_cmpint(inet_pton(AF_INET6, address, &request.address), ==, 1);
	request.prefix_length = 64;
	request.ifindex = (gint)if_nametoindex(name);
	g_assert_cmpint(request.ifindex, >, 0);
	g_assert_cmpint(ioctl(fd, SIOCSIFADDR, &request), ==, 0);
	close(fd);
}

static void
default_route(const gchar *name)
{
	struct rtentry route;
	struct sockaddr_in any;
	gint fd = control_socket(AF_INET);

	memset(&route, 0, sizeof(route));
	memset(&any, 0, sizeof(any));
	any.sin_family = AF_INET;
	memcpy(&route.rt_dst, &any, sizeof(any));
	memcpy(&route.rt_genmask, &any, sizeof(any));
	memcpy(&route.rt_gateway, &any, sizeof(any));
	route.rt_flags = RTF_UP;
	route.rt_dev = (gchar *)name;
	g_assert_cmpint(ioctl(fd, SIOCADDRT, &route), ==, 0);
	close(fd);
}

/* An IPv6 default route out of @name, preferred by its lower @metric. */
static void
default_route6(
	const gchar *name,
	guint32      metric
){
	struct in6_rtmsg route;
	gint fd = control_socket(AF_INET6);

	memset(&route, 0, sizeof(route));
	route.rtmsg_dst_len = 0;
	route.rtmsg_flags = RTF_UP;
	route.rtmsg_metric = metric;
	route.rtmsg_ifindex = (gint)if_nametoindex(name);
	g_assert_cmpint(ioctl(fd, SIOCADDRT, &route), ==, 0);
	close(fd);
}

/* Every capability of this thread, gone: as a daemon started without any. */
static void
drop_capabilities(void)
{
	struct __user_cap_header_struct header;
	struct __user_cap_data_struct data[2];

	memset(&header, 0, sizeof(header));
	memset(data, 0, sizeof(data));
	header.version = _LINUX_CAPABILITY_VERSION_3;
	header.pid = 0;
	g_assert_cmpint(syscall(SYS_capset, &header, data), ==, 0);
	/* Proof: the network can no longer be changed from here. */
	g_assert_cmpint(link_flags("lo", TRUE), ==, EPERM);
}

typedef enum
{
	VPN_MISSING,
	VPN_DOWN,
	VPN_NO_ADDRESS,
	VPN_UP,
	VPN_UP_PUBLIC	/* tun0 holds PUBLIC_V4 itself: a server can listen there */
} VpnState;

typedef struct
{
	gint vpn;	/* tun0, or -1 */
	gint lan;	/* tun1, which carries the default route */
} World;

static gboolean
ipv6_here(void)
{
	return g_file_test("/proc/sys/net/ipv6", G_FILE_TEST_IS_DIR);
}

/*
 * lo up; tun1 up with LAN_V4/24 and the IPv4 default route (no IPv4 route
 * goes out of tun0 at all: a socket bound to tun0 still leaves by it); with
 * @lan6, LAN_V6 and the preferred IPv6 default route; tun0 as @vpn says,
 * with VPN_V6 and a second, less preferred, IPv6 default route when @vpn6
 * (IPv6 has no route of last resort for a socket bound to a device: the VPN
 * must route, as a real one does).
 */
static void
world_make(
	World   *world,
	VpnState vpn,
	gboolean lan6,
	gboolean vpn6
){
	link_set("lo", TRUE);
	world->lan = tun_open("tun1");
	g_assert_cmpint(world->lan, >=, 0);
	link_set("tun1", TRUE);
	add_ipv4("tun1", LAN_V4, "255.255.255.0");
	default_route("tun1");
	if (lan6) {
		add_ipv6("tun1", LAN_V6);
		default_route6("tun1", 1);
	}
	world->vpn = -1;
	if (vpn == VPN_MISSING)
		return;
	world->vpn = tun_open("tun0");
	g_assert_cmpint(world->vpn, >=, 0);
	if (vpn == VPN_DOWN) {
		/* It keeps its address: down is what differs. */
		add_ipv4("tun0", VPN_V4, "255.255.255.0");
		return;
	}
	link_set("tun0", TRUE);
	if (vpn == VPN_NO_ADDRESS)
		return;
	if (vpn == VPN_UP_PUBLIC)
		add_ipv4("tun0", PUBLIC_V4, "255.255.255.255");
	else
		add_ipv4("tun0", VPN_V4, "255.255.255.0");
	if (vpn6) {
		add_ipv6("tun0", VPN_V6);
		default_route6("tun0", 100);
	}
}

typedef struct
{
	gint    family;
	guint8  source[16];
	guint8  destination[16];
	guint16 port;
	gboolean syn;
} Packet;

/* The TCP packets that came out of @fd's device (the kernel's own neighbour
 * discovery and multicast reports are not TCP), waiting up to @wait_ms for
 * the first, then taking whatever else is there. */
static GArray *
drain(
	gint  fd,
	guint wait_ms
){
	GArray *packets = g_array_new(FALSE, TRUE, sizeof(Packet));
	gint64 deadline = g_get_monotonic_time() + (gint64)wait_ms * 1000;
	guint8 buffer[2048];
	struct pollfd pfd;

	if (fd < 0)
		return packets;
	pfd.fd = fd;
	pfd.events = POLLIN;
	for (;;) {
		gint64 left = packets->len > 0 ? 0 : MAX(deadline - g_get_monotonic_time(), 0) / 1000;
		gssize n;
		Packet packet;

		if (poll(&pfd, 1, (gint)left) <= 0)
			break;
		n = read(fd, buffer, sizeof(buffer));
		if (n <= 0)
			break;
		memset(&packet, 0, sizeof(packet));
		if ((buffer[0] >> 4) == 4 && n >= 20 && buffer[9] == IPPROTO_TCP) {
			gsize header = (gsize)(buffer[0] & 0x0f) * 4;

			if ((gsize)n < header + 14)
				continue;
			packet.family = AF_INET;
			memcpy(packet.source, buffer + 12, 4);
			memcpy(packet.destination, buffer + 16, 4);
			packet.port = (guint16)((buffer[header + 2] << 8) | buffer[header + 3]);
			packet.syn = (buffer[header + 13] & 0x02) != 0;
			g_array_append_val(packets, packet);
		} else if ((buffer[0] >> 4) == 6 && n >= 40 + 14 && buffer[6] == IPPROTO_TCP) {
			packet.family = AF_INET6;
			memcpy(packet.source, buffer + 8, 16);
			memcpy(packet.destination, buffer + 24, 16);
			packet.port = (guint16)((buffer[40 + 2] << 8) | buffer[40 + 3]);
			packet.syn = (buffer[40 + 13] & 0x02) != 0;
			g_array_append_val(packets, packet);
		}
	}
	return packets;
}

static void
assert_nothing(
	gint         fd,
	const gchar *where
){
	GArray *packets = drain(fd, 300);

	if (packets->len != 0)
		g_error("%u TCP packets left by %s", packets->len, where);
	g_array_unref(packets);
}

/* At least one SYN on @fd, each from @source to @destination:@port. */
static void
assert_syn(
	gint         fd,
	const gchar *where,
	const gchar *source,
	const gchar *destination,
	guint16      port
){
	GArray *packets = drain(fd, 1000);
	gint family = strchr(source, ':') != NULL ? AF_INET6 : AF_INET;
	gsize length = family == AF_INET ? 4 : 16;
	guint8 want_source[16];
	guint8 want_destination[16];
	guint syns = 0;
	guint i;

	g_assert_cmpint(inet_pton(family, source, want_source), ==, 1);
	g_assert_cmpint(inet_pton(family, destination, want_destination), ==, 1);
	for (i = 0; i < packets->len; i++) {
		const Packet *packet = &g_array_index(packets, Packet, i);
		gchar from[INET6_ADDRSTRLEN];

		inet_ntop(packet->family, packet->source, from, sizeof(from));
		if (packet->family != family || memcmp(packet->source, want_source, length) != 0)
			g_error("a packet left by %s from %s, not %s", where, from, source);
		g_assert_true(memcmp(packet->destination, want_destination, length) == 0);
		g_assert_cmpuint(packet->port, ==, port);
		if (packet->syn)
			syns++;
	}
	if (syns == 0)
		g_error("no connection was attempted by %s", where);
	g_array_unref(packets);
}

static void
bind_to(
	const gchar      *value,
	AxisOutboundScope scope
){
	AxisOutboundTarget target = target_of(value);

	axis_outbound_configure(&target, scope);
}

/* One request by the daemon's client, as an indexer's (or @traffic's). */
static gboolean
request_to(
	const gchar        *address,
	AxisOutboundTraffic traffic,
	guint               timeout_ms,
	GBytes            **body,
	GError            **error
){
	AxisHttpOrigin origin;
	AxisHttpConfig config;
	GBytes *ignored = NULL;
	gboolean ok;

	g_assert_true(axis_http_origin_parse(address, &origin, NULL));
	memset(&config, 0, sizeof(config));
	config.origin = &origin;
	config.peer = "the indexer";
	config.timeout_ms = timeout_ms;
	config.traffic = traffic;
	config.accept = "text/plain";
	ok = axis_http_get(&config, "/", NULL, body != NULL ? body : &ignored, error);
	if (ignored != NULL)
		g_bytes_unref(ignored);
	axis_http_origin_clear(&origin);
	return ok;
}

static void
in_namespace(void (*body)(void))
{
	const gchar *why = NULL;

	if (g_test_subprocess()) {
		g_assert_true(enter_namespace());
		axis_test_log_init();
		axis_test_log_allow(G_LOG_LEVEL_WARNING, "outbound connections: *");
		body();
		return;
	}
	if (!namespace_usable(&why)) {
		g_test_skip(why);
		return;
	}
	g_test_trap_subprocess(NULL, 60 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

/* Refused at once, saying @message, and no packet left by the default route
 * or any other; a media server, which "internet" does not cover, is still
 * reached by the default route. */
static void
assert_refused_in(
	VpnState     vpn,
	const gchar *message
){
	World world;
	g_autoptr(GError) error = NULL;
	gint64 start;

	world_make(&world, vpn, FALSE, FALSE);
	drop_capabilities();
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);
	/* At once: well before the request's deadline. */
	start = g_get_monotonic_time();
	g_assert_false(request_to("http://" PUBLIC_V4 ":80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 10000, NULL, &error));
	g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT);
	g_assert_cmpstr(error->message, ==, message);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 3 * G_USEC_PER_SEC);
	assert_nothing(world.lan, "the default route (tun1)");
	assert_nothing(world.vpn, "tun0");
	g_clear_error(&error);

	g_assert_false(request_to("http://" PUBLIC_V4 ":80", AXIS_OUTBOUND_TRAFFIC_MEDIA_SERVER, 600, NULL, &error));
	assert_syn(world.lan, "the default route (tun1)", LAN_V4, PUBLIC_V4, 80);
}

/* Spec: Nothing leaves while the interface is down */
static void
run_down(void)
{
	assert_refused_in(VPN_DOWN, "the outbound interface tun0 is down; nothing was sent to the indexer");
}

static void
run_missing(void)
{
	assert_refused_in(VPN_MISSING, "the outbound interface tun0 does not exist; nothing was sent to the indexer");
}

static void
run_no_address(void)
{
	assert_refused_in(VPN_NO_ADDRESS, "the outbound interface tun0 has no address; nothing was sent to the indexer");
}

static void
test_namespace_down(void)
{
	in_namespace(run_down);
}

static void
test_namespace_missing(void)
{
	in_namespace(run_missing);
}

static void
test_namespace_no_address(void)
{
	in_namespace(run_no_address);
}

/* Spec: Up, the connection leaves from the interface and its address */
static void
run_up(void)
{
	World world;
	g_autoptr(GError) error = NULL;

	world_make(&world, VPN_UP, FALSE, FALSE);
	drop_capabilities();
	g_assert_true(axis_outbound_device_binding_check(NULL));

	/* Bound by name. */
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://" PUBLIC_V4 ":80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 1200, NULL, &error));
	assert_syn(world.vpn, "tun0", VPN_V4, PUBLIC_V4, 80);
	assert_nothing(world.lan, "the default route (tun1)");
	g_clear_error(&error);

	/* Bound by address. */
	bind_to(VPN_V4, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://" PUBLIC_V4 ":81", AXIS_OUTBOUND_TRAFFIC_METADATA, 1200, NULL, &error));
	assert_syn(world.vpn, "tun0", VPN_V4, PUBLIC_V4, 81);
	assert_nothing(world.lan, "the default route (tun1)");
	g_clear_error(&error);

	/* Not bound: the default route, from the LAN's address (the control). */
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://" PUBLIC_V4 ":82", AXIS_OUTBOUND_TRAFFIC_INDEXER, 600, NULL, &error));
	assert_syn(world.lan, "the default route (tun1)", LAN_V4, PUBLIC_V4, 82);
	assert_nothing(world.vpn, "tun0");
}

static void
test_namespace_up(void)
{
	in_namespace(run_up);
}

/* Why the device and not only the address: a socket bound to the VPN's
 * address alone leaves by the default route, from that address; bound to
 * the device as well, by the VPN.  (The kernel's behaviour, which the design
 * relies on, shown without Axis's code.) */
static void
run_kernel(void)
{
	World world;
	struct sockaddr_in local;
	struct sockaddr_in remote;
	gint fd;

	world_make(&world, VPN_UP, FALSE, FALSE);
	drop_capabilities();
	memset(&local, 0, sizeof(local));
	memset(&remote, 0, sizeof(remote));
	local.sin_family = AF_INET;
	g_assert_cmpint(inet_pton(AF_INET, VPN_V4, &local.sin_addr), ==, 1);
	remote.sin_family = AF_INET;
	remote.sin_port = htons(83);
	g_assert_cmpint(inet_pton(AF_INET, PUBLIC_V4, &remote.sin_addr), ==, 1);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&local, sizeof(local)), ==, 0);
	g_assert_cmpint(connect(fd, (struct sockaddr *)&remote, sizeof(remote)), ==, -1);
	g_assert_cmpint(errno, ==, EINPROGRESS);
	assert_syn(world.lan, "the default route (tun1)", VPN_V4, PUBLIC_V4, 83);
	assert_nothing(world.vpn, "tun0");
	close(fd);

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "tun0", 4), ==, 0);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&local, sizeof(local)), ==, 0);
	g_assert_cmpint(connect(fd, (struct sockaddr *)&remote, sizeof(remote)), ==, -1);
	g_assert_cmpint(errno, ==, EINPROGRESS);
	assert_syn(world.vpn, "tun0", VPN_V4, PUBLIC_V4, 83);
	assert_nothing(world.lan, "the default route (tun1)");
	close(fd);
}

static void
test_namespace_kernel(void)
{
	in_namespace(run_kernel);
}

/* Spec: Which traffic is bound (internet and all), on the wire */
static void
run_scope(void)
{
	World world;
	g_autoptr(GError) error = NULL;

	world_make(&world, VPN_UP, FALSE, FALSE);
	drop_capabilities();

	/* Internet: a download client at a public address, and an indexer at a
	 * local one (a Prowlarr on the same network), take the default route. */
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);
	request_to("http://" PUBLIC_V4 ":84", AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT, 600, NULL, &error);
	g_clear_error(&error);
	assert_syn(world.lan, "the default route (tun1)", LAN_V4, PUBLIC_V4, 84);
	request_to("http://" LOCAL_V4 ":9696", AXIS_OUTBOUND_TRAFFIC_INDEXER, 600, NULL, &error);
	g_clear_error(&error);
	assert_syn(world.lan, "the LAN (tun1)", LAN_V4, LOCAL_V4, 9696);
	assert_nothing(world.vpn, "tun0");

	/* All: both through tun0. */
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_ALL);
	request_to("http://" PUBLIC_V4 ":84", AXIS_OUTBOUND_TRAFFIC_DOWNLOAD_CLIENT, 600, NULL, &error);
	g_clear_error(&error);
	assert_syn(world.vpn, "tun0", VPN_V4, PUBLIC_V4, 84);
	request_to("http://" LOCAL_V4 ":9696", AXIS_OUTBOUND_TRAFFIC_INDEXER, 600, NULL, &error);
	g_clear_error(&error);
	assert_syn(world.vpn, "tun0", VPN_V4, LOCAL_V4, 9696);
	assert_nothing(world.lan, "the default route (tun1)");
}

static void
test_namespace_scope(void)
{
	in_namespace(run_scope);
}

typedef struct
{
	gint     vpn;
	gboolean saw_syn;
} Cutter;

/* Waits for the connection's first SYN on tun0, then takes tun0 down: this
 * thread keeps the capabilities the main one drops. */
static gpointer
cut_vpn(gpointer data)
{
	Cutter *cutter = data;
	GArray *packets = drain(cutter->vpn, 10000);

	cutter->saw_syn = packets->len > 0 && g_array_index(packets, Packet, 0).syn;
	g_array_unref(packets);
	link_set("tun0", FALSE);
	return NULL;
}

/* Spec: The interface goes down during a connection */
static void
run_in_flight(void)
{
	World world;
	Cutter cutter;
	GThread *thread;
	g_autoptr(GError) error = NULL;

	world_make(&world, VPN_UP, FALSE, FALSE);
	cutter.vpn = world.vpn;
	cutter.saw_syn = FALSE;
	thread = g_thread_new("cut-vpn", cut_vpn, &cutter);
	drop_capabilities();
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);

	/* The SYN is sent again after a second, and again after three: none of
	 * them by the default route once tun0 is down. */
	g_assert_false(request_to("http://" PUBLIC_V4 ":85", AXIS_OUTBOUND_TRAFFIC_INDEXER, 3500, NULL, &error));
	g_thread_join(thread);
	g_assert_true(cutter.saw_syn);
	assert_nothing(world.lan, "the default route (tun1)");
	g_clear_error(&error);

	/* The next request finds it down, at once. */
	g_assert_false(request_to("http://" PUBLIC_V4 ":85", AXIS_OUTBOUND_TRAFFIC_INDEXER, 3500, NULL, &error));
	g_assert_cmpstr(error->message, ==, "the outbound interface tun0 is down; nothing was sent to the indexer");
	assert_nothing(world.lan, "the default route (tun1)");
}

static void
test_namespace_in_flight(void)
{
	in_namespace(run_in_flight);
}

typedef struct
{
	gint   listener;
	gchar  peer[INET6_ADDRSTRLEN];
} Server;

/* Answers one request "ok" and notes where it came from. */
static gpointer
serve_once(gpointer data)
{
	static const gchar answer[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n"
		"Connection: close\r\n\r\nok";
	Server *server = data;
	struct sockaddr_in peer;
	socklen_t length = sizeof(peer);
	gchar request[4096];
	gsize got = 0;
	gint fd = accept(server->listener, (struct sockaddr *)&peer, &length);

	g_assert_cmpint(fd, >=, 0);
	inet_ntop(AF_INET, &peer.sin_addr, server->peer, sizeof(server->peer));
	while (got < sizeof(request) - 1) {
		gssize n = read(fd, request + got, sizeof(request) - 1 - got);

		if (n <= 0)
			break;
		got += (gsize)n;
		request[got] = '\0';
		if (strstr(request, "\r\n\r\n") != NULL)
			break;
	}
	g_assert_cmpint(write(fd, answer, sizeof(answer) - 1), ==, (gssize)(sizeof(answer) - 1));
	close(fd);
	return NULL;
}

static gint
listen_on(
	const gchar *address,
	guint16      port
){
	struct sockaddr_in sin;
	gint fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	gint one = 1;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	g_assert_cmpint(inet_pton(AF_INET, address, &sin.sin_addr), ==, 1);
	g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), ==, 0);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&sin, sizeof(sin)), ==, 0);
	g_assert_cmpint(listen(fd, 4), ==, 0);
	return fd;
}

/* Spec: Up, the connection leaves from the interface and its address (a
 * whole request: the server is on tun0's own, public, address and sees the
 * request come from it; and one not covered, on the LAN's address, works
 * unbound). */
static void
run_exchange(void)
{
	World world;
	Server server;
	GThread *thread;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;

	world_make(&world, VPN_UP_PUBLIC, FALSE, FALSE);
	drop_capabilities();
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);

	memset(&server, 0, sizeof(server));
	server.listener = listen_on(PUBLIC_V4, 8080);
	thread = g_thread_new("serve", serve_once, &server);
	if (!request_to("http://" PUBLIC_V4 ":8080", AXIS_OUTBOUND_TRAFFIC_INDEXER, 5000, &body, &error))
		g_error("the bound request failed: %s", error->message);
	g_thread_join(thread);
	g_assert_cmpstr(server.peer, ==, PUBLIC_V4);
	g_assert_cmpmem(g_bytes_get_data(body, NULL), g_bytes_get_size(body), "ok", 2);
	close(server.listener);
	g_clear_pointer(&body, g_bytes_unref);

	server.listener = listen_on(LAN_V4, 8081);
	thread = g_thread_new("serve", serve_once, &server);
	if (!request_to("http://" LAN_V4 ":8081", AXIS_OUTBOUND_TRAFFIC_INDEXER, 5000, &body, &error))
		g_error("the request on the LAN failed: %s", error->message);
	g_thread_join(thread);
	g_assert_cmpstr(server.peer, ==, LAN_V4);
	close(server.listener);
	assert_nothing(world.lan, "the default route (tun1)");
	assert_nothing(world.vpn, "tun0");
}

static void
test_namespace_exchange(void)
{
	in_namespace(run_exchange);
}

/* Spec: IPv6 goes out of the interface, from its IPv6 address (the LAN's
 * IPv6 default route is the preferred one: unbound, it is taken); and a
 * family the interface lacks is not used */
static void
run_ipv6(void)
{
	World world;
	g_autoptr(GError) error = NULL;

	if (!ipv6_here()) {
		g_test_message("no IPv6 in this kernel: nothing to check");
		return;
	}
	world_make(&world, VPN_UP, TRUE, TRUE);
	drop_capabilities();
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://[" PUBLIC_V6 "]:80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 600, NULL, &error));
	assert_syn(world.lan, "the default route (tun1)", LAN_V6, PUBLIC_V6, 80);
	assert_nothing(world.vpn, "tun0");
	g_clear_error(&error);

	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://[" PUBLIC_V6 "]:80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 1200, NULL, &error));
	assert_syn(world.vpn, "tun0", VPN_V6, PUBLIC_V6, 80);
	assert_nothing(world.lan, "the default route (tun1)");
	g_clear_error(&error);

	/* Bound by its IPv4 address: an IPv6 server is not reached at all. */
	bind_to(VPN_V4, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://[" PUBLIC_V6 "]:80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 1200, NULL, &error));
	g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT);
	g_assert_cmpstr(error->message, ==, "the indexer cannot be reached: the outbound address " VPN_V4 " is IPv4, and "
		"this connection is IPv6; nothing was sent");
	assert_nothing(world.lan, "the default route (tun1)");
	assert_nothing(world.vpn, "tun0");
}

static void
test_namespace_ipv6(void)
{
	in_namespace(run_ipv6);
}

/* IPv6 bound to an interface with an IPv6 address but no IPv6 route: the
 * connection is not made at all (the kernel finds no route out of tun0), and
 * nothing leaves by the LAN's default route. */
static void
run_ipv6_no_route(void)
{
	World world;
	g_autoptr(GError) error = NULL;

	if (!ipv6_here()) {
		g_test_message("no IPv6 in this kernel: nothing to check");
		return;
	}
	world_make(&world, VPN_UP, TRUE, FALSE);
	add_ipv6("tun0", VPN_V6);
	drop_capabilities();
	bind_to("tun0", AXIS_OUTBOUND_SCOPE_INTERNET);
	g_assert_false(request_to("http://[" PUBLIC_V6 "]:80", AXIS_OUTBOUND_TRAFFIC_INDEXER, 1200, NULL, &error));
	g_assert_error(error, AXIS_HTTP_ERROR, AXIS_HTTP_ERROR_CONNECT);
	assert_nothing(world.lan, "the default route (tun1)");
	assert_nothing(world.vpn, "tun0");
}

static void
test_namespace_ipv6_no_route(void)
{
	in_namespace(run_ipv6_no_route);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/outbound/parse", test_parse_forms);
	g_test_add_func("/outbound/resolve", test_resolve);
	g_test_add_func("/outbound/read-entries", test_read_entries);
	g_test_add_func("/outbound/applies", test_applies);
	g_test_add_func("/outbound/decide", test_decide);
	g_test_add_func("/outbound/state-equal", test_state_equal);
	g_test_add_func("/outbound/configure-and-log", test_configure_and_log);
	g_test_add_func("/outbound/request-refused", test_request_refused);
	g_test_add_func("/outbound/sources", test_sources);
	g_test_add_func("/outbound/namespace/down", test_namespace_down);
	g_test_add_func("/outbound/namespace/missing", test_namespace_missing);
	g_test_add_func("/outbound/namespace/no-address", test_namespace_no_address);
	g_test_add_func("/outbound/namespace/up", test_namespace_up);
	g_test_add_func("/outbound/namespace/kernel", test_namespace_kernel);
	g_test_add_func("/outbound/namespace/scope", test_namespace_scope);
	g_test_add_func("/outbound/namespace/in-flight", test_namespace_in_flight);
	g_test_add_func("/outbound/namespace/exchange", test_namespace_exchange);
	g_test_add_func("/outbound/namespace/ipv6", test_namespace_ipv6);
	g_test_add_func("/outbound/namespace/ipv6-no-route", test_namespace_ipv6_no_route);

	return g_test_run();
}
