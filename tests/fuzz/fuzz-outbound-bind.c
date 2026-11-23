/*
 * fuzz-outbound-bind.c - libFuzzer target for the outbound.bind and
 * outbound.bind_scope values, and the decision made from them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is the setting's value (up to the first NUL).  Besides memory
 * safety it checks what core/axis-outbound-parse.h promises:
 *
 *  - a value either fails, with a problem that is valid UTF-8 without a
 *    control character (it goes to the log), and leaves the target off; or
 *    parses with no problem;
 *  - a parsed target's text parses again to the same target;
 *  - an interface name is 1 to 15 bytes of printable ASCII without a space,
 *    '/' or ':', and is not digits and dots alone;
 *  - an address is IPv4 or IPv6, its text is its bytes as inet_ntop()
 *    writes them, and it is never the unspecified, a loopback, a multicast
 *    or an IPv6 link-local address;
 *  - the scope is "internet" or "all", and nothing else.
 *
 * And of the decision (core/axis-outbound.h), with the target found on a
 * made-up interface in each state: a connection is bound only when the
 * interface is up with an address of its family, passed only when the
 * binding does not apply, and otherwise refused with a reason.
 */

#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "core/axis-outbound.h"
#include "core/axis-outbound-parse.h"

static void
check_problem(const gchar *problem)
{
	const gchar *p;

	if (problem == NULL || problem[0] == '\0' || !g_utf8_validate(problem, -1, NULL))
		abort();
	for (p = problem; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			abort();
	}
}

static void
check_target(const AxisOutboundTarget *target)
{
	AxisOutboundTarget again;
	gchar buffer[INET6_ADDRSTRLEN];
	const gchar *p;
	gboolean digits_and_dots = TRUE;

	switch (target->kind) {
	case AXIS_OUTBOUND_OFF:
		if (target->text[0] != '\0')
			abort();
		break;
	case AXIS_OUTBOUND_INTERFACE:
		if (strlen(target->text) == 0 || strlen(target->text) > AXIS_OUTBOUND_MAX_NAME_BYTES)
			abort();
		for (p = target->text; *p != '\0'; p++) {
			if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '/' || *p == ':')
				abort();
			if (!g_ascii_isdigit(*p) && *p != '.')
				digits_and_dots = FALSE;
		}
		if (digits_and_dots)
			abort();
		break;
	case AXIS_OUTBOUND_ADDRESS:
		if (target->family != AF_INET && target->family != AF_INET6)
			abort();
		if (inet_ntop(target->family, target->address, buffer, sizeof(buffer)) == NULL ||
		    strcmp(buffer, target->text) != 0)
			abort();
		if (target->family == AF_INET &&
		    (target->address[0] == 0 || target->address[0] == 127 || (target->address[0] & 0xf0) == 0xe0))
			abort();
		if (target->family == AF_INET6 &&
		    (target->address[0] == 0xff || (target->address[0] == 0xfe && (target->address[1] & 0xc0) == 0x80)))
			abort();
		break;
	default:
		abort();
	}

	if (!axis_outbound_target_parse(target->text, &again, NULL) || again.kind != target->kind ||
	    strcmp(again.text, target->text) != 0 || again.family != target->family ||
	    memcmp(again.address, target->address, sizeof(again.address)) != 0)
		abort();
}

/* The target on a made-up interface with @flags, and the decision for a
 * connection of each family, traffic, destination and scope. */
static void
check_decisions(
	const AxisOutboundTarget *target,
	guint                     flags,
	gboolean                  with_address
){
	AxisOutboundEntry entries[3];
	AxisOutboundState state;
	guint n = 0;
	gint families[] = { AF_INET, AF_INET6 };
	guint scope;
	guint traffic;
	guint destination;
	guint f;

	memset(entries, 0, sizeof(entries));
	g_strlcpy(entries[n].name, target->kind == AXIS_OUTBOUND_INTERFACE ? target->text : "vpn0",
		sizeof(entries[n].name));
	entries[n].flags = flags;
	n++;
	if (with_address) {
		entries[n] = entries[0];
		entries[n].family = target->kind == AXIS_OUTBOUND_ADDRESS ? target->family : AF_INET;
		if (target->kind == AXIS_OUTBOUND_ADDRESS)
			memcpy(entries[n].address, target->address, 16);
		else
			entries[n].address[0] = 10;
		n++;
	}
	axis_outbound_resolve(target, entries, n, &state);
	if (target->kind == AXIS_OUTBOUND_OFF && state.condition != AXIS_OUTBOUND_STATE_OFF)
		abort();
	if (state.condition == AXIS_OUTBOUND_STATE_UP &&
	    ((flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) || (!state.has_ipv4 && !state.has_ipv6)))
		abort();

	for (scope = 0; scope < 2; scope++) {
		for (traffic = 0; traffic <= AXIS_OUTBOUND_TRAFFIC_LAST; traffic++) {
			for (destination = 0; destination <= AXIS_OUTBOUND_TO_PUBLIC; destination++) {
				for (f = 0; f < G_N_ELEMENTS(families); f++) {
					gboolean applies = axis_outbound_applies(target, (AxisOutboundScope)scope,
						(AxisOutboundTraffic)traffic, (AxisOutboundDestination)destination);
					gchar *message = NULL;
					AxisOutboundVerdict verdict = axis_outbound_decide(target, (AxisOutboundScope)scope,
						(AxisOutboundTraffic)traffic, (AxisOutboundDestination)destination, families[f], &state,
						&message);
					gboolean has = families[f] == AF_INET ? state.has_ipv4 : state.has_ipv6;

					if (destination == AXIS_OUTBOUND_TO_LOOPBACK && applies)
						abort();
					if ((verdict == AXIS_OUTBOUND_PASS) != !applies)
						abort();
					if (verdict == AXIS_OUTBOUND_BIND && (state.condition != AXIS_OUTBOUND_STATE_UP || !has))
						abort();
					if ((verdict == AXIS_OUTBOUND_REFUSE || verdict == AXIS_OUTBOUND_REFUSE_FAMILY))
						check_problem(message);
					else if (message != NULL)
						abort();
					g_free(message);
				}
			}
		}
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);
	AxisOutboundTarget target;
	AxisOutboundScope scope = AXIS_OUTBOUND_SCOPE_INTERNET;
	gchar *problem = NULL;

	if (axis_outbound_target_parse(text, &target, &problem)) {
		if (problem != NULL)
			abort();
		check_target(&target);
		check_decisions(&target, IFF_UP | IFF_RUNNING, TRUE);
		check_decisions(&target, IFF_UP | IFF_RUNNING, FALSE);
		check_decisions(&target, IFF_RUNNING, TRUE);
		check_decisions(&target, IFF_UP, TRUE);
	} else {
		check_problem(problem);
		if (target.kind != AXIS_OUTBOUND_OFF || target.text[0] != '\0')
			abort();
		g_free(problem);
	}

	if (axis_outbound_scope_parse(text, &scope)) {
		if (text[0] != '\0' && strcmp(text, "internet") != 0 && strcmp(text, "all") != 0)
			abort();
		if (strcmp(axis_outbound_scope_to_string(scope), strcmp(text, "all") == 0 ? "all" : "internet") != 0)
			abort();
	} else if (text[0] == '\0' || strcmp(text, "internet") == 0 || strcmp(text, "all") == 0) {
		abort();
	}
	return 0;
}
