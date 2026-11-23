/*
 * axis-outbound-parse.c - the outbound.bind and outbound.bind_scope values
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GLib and the C library only: it is compiled into the fuzz targets.
 */

#include "core/axis-outbound-parse.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

/* @value in quotes for a message, when it is short printable ASCII;
 * otherwise "the value", so that nothing odd reaches a log. */
static gchar *
quoted(const gchar *value)
{
	const gchar *p;

	if (strlen(value) > 64)
		return g_strdup("the value");
	for (p = value; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || (guchar)*p >= 0x7f)
			return g_strdup("the value");
	}
	return g_strdup_printf("'%s'", value);
}

static gboolean
refuse(
	gchar      **problem,
	const gchar *value,
	const gchar *reason
){
	if (problem != NULL) {
		g_autofree gchar *what = quoted(value);

		*problem = g_strdup_printf("%s %s", what, reason);
	}
	return FALSE;
}

/* Why the IPv4 address @a cannot be a connection's source, or NULL. */
static const gchar *
ipv4_problem(const guint8 *a)
{
	if (a[0] == 0)
		return "is not an address a connection can come from";
	if (a[0] == 127)
		return "is a loopback address: name the VPN's interface (such as tun0) or an address it holds";
	if ((a[0] & 0xf0) == 0xe0)
		return "is a multicast address: name the VPN's interface (such as tun0) or an address it holds";
	if (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255)
		return "is the broadcast address: name the VPN's interface (such as tun0) or an address it holds";
	return NULL;
}

/* Why the IPv6 address @a cannot be named here, or NULL. */
static const gchar *
ipv6_problem(const guint8 *a)
{
	static const guint8 zeros[16] = { 0 };

	if (memcmp(a, zeros, 16) == 0)
		return "is not an address a connection can come from";
	if (memcmp(a, zeros, 15) == 0 && a[15] == 1)
		return "is a loopback address: name the VPN's interface (such as tun0) or an address it holds";
	if (a[0] == 0xff)
		return "is a multicast address: name the VPN's interface (such as tun0) or an address it holds";
	if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)
		return "is an IPv6 link-local address, which needs a zone: name the interface instead (such as tun0)";
	/* ::ffff:a.b.c.d and the old ::a.b.c.d */
	if (memcmp(a, zeros, 10) == 0 && ((a[10] == 0xff && a[11] == 0xff) || (a[10] == 0 && a[11] == 0)))
		return "is an IPv4 address written as IPv6: write it as IPv4 (such as 10.8.0.2)";
	return NULL;
}

/* Linux's rule for a device name (dev_valid_name()), within printable ASCII. */
static gboolean
name_is_valid(const gchar *name)
{
	gsize length = strlen(name);
	const gchar *p;

	if (length == 0 || length > AXIS_OUTBOUND_MAX_NAME_BYTES)
		return FALSE;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '/' || *p == ':')
			return FALSE;
	}
	return TRUE;
}

static gboolean
digits_and_dots(const gchar *text)
{
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p) && *p != '.')
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_outbound_target_parse(
	const gchar        *value,
	AxisOutboundTarget *target,
	gchar             **problem
){
	g_autofree gchar *text = NULL;
	const gchar *reason;
	guint8 bytes[16];

	g_return_val_if_fail(target != NULL, FALSE);

	memset(target, 0, sizeof(*target));
	if (problem != NULL)
		*problem = NULL;
	if (value == NULL)
		return TRUE;
	if (strlen(value) > AXIS_OUTBOUND_MAX_VALUE_BYTES) {
		if (problem != NULL)
			*problem = g_strdup_printf("the value is longer than %d characters", AXIS_OUTBOUND_MAX_VALUE_BYTES);
		return FALSE;
	}
	text = g_strstrip(g_strdup(value));
	if (text[0] == '\0')
		return TRUE;

	if (strchr(text, ':') != NULL) {
		if (strchr(text, '%') != NULL)
			return refuse(problem, text, "has a zone: name the interface instead (such as tun0)");
		if (inet_pton(AF_INET6, text, bytes) != 1)
			return refuse(problem, text, "is not an IPv6 address (an interface name has no ':')");
		if ((reason = ipv6_problem(bytes)) != NULL)
			return refuse(problem, text, reason);
		target->kind = AXIS_OUTBOUND_ADDRESS;
		target->family = AF_INET6;
		memcpy(target->address, bytes, 16);
		inet_ntop(AF_INET6, bytes, target->text, sizeof(target->text));
		return TRUE;
	}
	if (digits_and_dots(text)) {
		if (inet_pton(AF_INET, text, bytes) != 1)
			return refuse(problem, text, "is not an IPv4 address (four numbers from 0 to 255, such as 10.8.0.2)");
		if ((reason = ipv4_problem(bytes)) != NULL)
			return refuse(problem, text, reason);
		target->kind = AXIS_OUTBOUND_ADDRESS;
		target->family = AF_INET;
		memcpy(target->address, bytes, 4);
		inet_ntop(AF_INET, bytes, target->text, sizeof(target->text));
		return TRUE;
	}
	if (!name_is_valid(text))
		return refuse(problem, text, "is not an interface name (1 to 15 letters, digits and punctuation, without a "
			"space, '/' or ':', such as tun0 or wg0) nor an IP address");
	target->kind = AXIS_OUTBOUND_INTERFACE;
	g_strlcpy(target->text, text, sizeof(target->text));
	return TRUE;
}

gboolean
axis_outbound_scope_parse(
	const gchar       *value,
	AxisOutboundScope *scope
){
	g_return_val_if_fail(scope != NULL, FALSE);

	if (value == NULL || value[0] == '\0' || strcmp(value, "internet") == 0) {
		*scope = AXIS_OUTBOUND_SCOPE_INTERNET;
		return TRUE;
	}
	if (strcmp(value, "all") == 0) {
		*scope = AXIS_OUTBOUND_SCOPE_ALL;
		return TRUE;
	}
	return FALSE;
}

const gchar *
axis_outbound_scope_to_string(AxisOutboundScope scope)
{
	return scope == AXIS_OUTBOUND_SCOPE_ALL ? "all" : "internet";
}
