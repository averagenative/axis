/*
 * axis-outbound-parse.h - the outbound.bind and outbound.bind_scope values
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * outbound.bind (AXIS_OUTBOUND_BIND) names the one network interface the
 * daemon's outbound connections are tied to, by its name ("tun0", "wg0") or
 * by a local IP address it holds ("10.8.0.2", "fd00::2").  The value is
 * outside input (a file, the environment), so it is read here, with no help
 * from the network (no lookup, no interface is asked), and fuzzed
 * (tests/fuzz/fuzz-outbound-bind.c).  What the binding does with it is in
 * core/axis-outbound.h.
 *
 * The forms, after white space around the value is removed:
 *
 *  - empty: no binding (the default);
 *  - an IPv4 address in dotted decimal (four parts), or an IPv6 address
 *    (any value with a ':' in it is one: an interface name has none).  Not
 *    the unspecified address, a loopback, multicast or broadcast address, an
 *    IPv6 link-local address (it needs a zone: name the interface instead),
 *    an IPv4 address written inside IPv6, or one with a zone ("%eth0");
 *  - otherwise an interface name, as Linux allows them: 1 to 15 bytes of
 *    printable ASCII without a space, '/' or ':', and not "." or "..".  A
 *    value of digits and dots alone is taken for an IPv4 address, so a
 *    mistyped one is an error, never an interface name.
 *
 * outbound.bind_scope is "internet" (the default) or "all".
 */

#ifndef AXIS_OUTBOUND_PARSE_H
#define AXIS_OUTBOUND_PARSE_H

#include <glib.h>

G_BEGIN_DECLS

/* The longest value read, white space included. */
#define AXIS_OUTBOUND_MAX_VALUE_BYTES 255

/* The longest interface name (Linux's IFNAMSIZ less its terminator). */
#define AXIS_OUTBOUND_MAX_NAME_BYTES  15

typedef enum
{
	AXIS_OUTBOUND_OFF = 0,
	AXIS_OUTBOUND_INTERFACE,
	AXIS_OUTBOUND_ADDRESS
} AxisOutboundKind;

/**
 * AxisOutboundTarget:
 * @kind: off, an interface by name, or an interface by its address
 * @text: the interface's name, or the address in its canonical form
 *   ("10.8.0.2", "fd00::2"); "" when off
 * @family: for an address, AF_INET or AF_INET6; 0 otherwise
 * @address: for an address, its 4 or 16 bytes
 *
 * A plain value: copy it with "=".
 */
typedef struct
{
	AxisOutboundKind kind;
	gchar            text[64];
	gint             family;
	guint8           address[16];
} AxisOutboundTarget;

/**
 * AxisOutboundScope:
 * @AXIS_OUTBOUND_SCOPE_INTERNET: the indexers' and the metadata providers'
 *   connections to public addresses
 * @AXIS_OUTBOUND_SCOPE_ALL: every connection of the daemon's HTTP client to
 *   an address other than this machine's own (loopback)
 */
typedef enum
{
	AXIS_OUTBOUND_SCOPE_INTERNET = 0,
	AXIS_OUTBOUND_SCOPE_ALL
} AxisOutboundScope;

/**
 * axis_outbound_target_parse:
 * @value: (nullable): the setting's value; %NULL is empty
 * @target: (out): what it names; set to off on failure
 * @problem: (out) (optional) (transfer full): on failure, what is wrong, as
 *   the end of a sentence that starts with the setting's name
 *
 * Returns: %TRUE when @value is one of the forms above
 */
gboolean
axis_outbound_target_parse(
	const gchar        *value,
	AxisOutboundTarget *target,
	gchar             **problem
);

/**
 * axis_outbound_scope_parse:
 * @value: (nullable): "internet" or "all" (exactly); %NULL and "" are
 *   "internet"
 * @scope: (out): the scope; untouched on failure
 *
 * Returns: %TRUE when @value is one of them
 */
gboolean
axis_outbound_scope_parse(
	const gchar       *value,
	AxisOutboundScope *scope
);

/* "internet" or "all". */
const gchar *
axis_outbound_scope_to_string(AxisOutboundScope scope);

G_END_DECLS

#endif /* AXIS_OUTBOUND_PARSE_H */
