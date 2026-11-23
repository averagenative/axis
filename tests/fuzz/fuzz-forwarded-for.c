/*
 * fuzz-forwarded-for.c - libFuzzer target for the trusted-proxy list
 * parser and the X-Forwarded-For / X-Forwarded-Proto resolution
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is up to five lines: the trusted-proxy list, the peer
 * address, the X-Forwarded-For value, the X-Forwarded-Proto value and the
 * X-Forwarded-Host value.  Besides memory safety it checks what the code
 * promises:
 *
 *  - a list either fails with an error or parses; a parsed list prints in
 *    a canonical form that parses back to the same form, and has at most
 *    AXIS_PROXY_LIST_MAX_ENTRIES entries;
 *  - the client address is always "unknown" or an address in canonical
 *    form;
 *  - the headers are ignored unless the peer is a trusted proxy: then the
 *    client is the peer itself, HTTPS is the peer's own and there is no
 *    forwarded host;
 *  - with no trusted list at all, the headers are always ignored;
 *  - a forwarded host, when there is one, passes axis_proxy_host_is_valid(),
 *    is lower case, has no comma, space or control character, and is the
 *    rightmost value of the header;
 *  - axis_proxy_resolve() and axis_proxy_resolve_full() agree;
 *  - axis_proxy_host_is_valid() never accepts a comma, a space, a control
 *    character, a byte above 0x7e or more than AXIS_PROXY_HOST_MAX_LEN
 *    characters.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/axis-proxy.h"

static void
check_client(
	const gchar *client
){
	AxisIpAddress address;
	gchar *again;

	if (client == NULL)
		abort();
	if (strcmp(client, AXIS_PROXY_UNKNOWN_CLIENT) == 0)
		return;
	if (!axis_ip_address_parse(client, &address))
		abort();
	again = axis_ip_address_to_string(&address);
	if (strcmp(again, client) != 0)
		abort();
	g_free(again);
}

static void
check_host_rule(const gchar *text)
{
	const gchar *p;

	if (!axis_proxy_host_is_valid(text))
		return;
	if (strlen(text) == 0 || strlen(text) > AXIS_PROXY_HOST_MAX_LEN)
		abort();
	for (p = text; *p != '\0'; p++) {
		if (*p == ',' || *p == ' ' || (guchar)*p < 0x21 || (guchar)*p > 0x7e)
			abort();
	}
}

static void
check_resolve(
	const AxisProxyList *list,
	const gchar         *peer,
	const gchar         *forwarded_for,
	const gchar         *forwarded_proto,
	const gchar         *forwarded_host
){
	AxisIpAddress peer_address;
	gboolean peer_ok = axis_ip_address_parse(peer, &peer_address);
	gboolean trusted = peer_ok && axis_proxy_list_contains(list, &peer_address);
	guint i;

	for (i = 0; i < 2; i++) {
		gboolean peer_https = i == 1;
		gchar *client = NULL;
		gchar *plain_client = NULL;
		gchar *host = NULL;
		gboolean https = !peer_https;
		gboolean plain_https = peer_https;

		axis_proxy_resolve_full(list, peer, peer_https, forwarded_for, forwarded_proto, forwarded_host,
			&client, &https, &host);
		axis_proxy_resolve(list, peer, peer_https, forwarded_for, forwarded_proto, &plain_client, &plain_https);
		check_client(client);
		if (strcmp(client, plain_client) != 0 || https != plain_https)
			abort();

		if (!peer_ok) {
			if (strcmp(client, AXIS_PROXY_UNKNOWN_CLIENT) != 0 || https != peer_https || host != NULL)
				abort();
		} else if (!trusted) {
			gchar *canonical = axis_ip_address_to_string(&peer_address);

			if (strcmp(client, canonical) != 0 || https != peer_https || host != NULL)
				abort();
			g_free(canonical);
		} else if (forwarded_proto == NULL && https != peer_https) {
			abort();
		}

		if (host != NULL) {
			gchar *lower = g_ascii_strdown(host, -1);
			const gchar *comma;
			const gchar *last;
			gchar *last_lower;

			if (forwarded_host == NULL || !axis_proxy_host_is_valid(host) || strcmp(lower, host) != 0)
				abort();
			comma = strrchr(forwarded_host, ',');
			last = comma != NULL ? comma + 1 : forwarded_host;
			check_host_rule(host);
			/* The rightmost value, spaces and tabs around it aside. */
			while (*last == ' ' || *last == '\t')
				last++;
			last_lower = g_ascii_strdown(last, -1);
			if (!g_str_has_prefix(last_lower, host))
				abort();
			g_free(last_lower);
			g_free(lower);
		}

		g_free(client);
		g_free(plain_client);
		g_free(host);
	}
}

/*
 * The names of a list, given what lookups found: a good answer for a name
 * is trusted at once (so is the peer when it was the answer), a failed one
 * keeps it for the grace, and no entry of the description is lost.
 */
static void
check_names(
	AxisProxyList *list,
	const gchar   *peer
){
	g_auto(GStrv) names = axis_proxy_list_get_names(list);
	AxisIpAddress answer[AXIS_PROXY_NAME_MAX_ADDRESSES + 4];
	AxisIpAddress from;
	GPtrArray *entries;
	guint n = 0;
	guint i;

	if (peer != NULL && axis_ip_address_parse(peer, &from)) {
		for (i = 0; i < AXIS_PROXY_NAME_MAX_ADDRESSES + 4; i++) {
			/* The peer's neighbours, the lowest first: more than the cap, so the
			 * lowest is among those trusted. */
			answer[n] = from;
			answer[n].bytes[from.family == 4 ? 3 : 15] = (guint8)i;
			n++;
		}
	}

	for (i = 0; names[i] != NULL; i++) {
		gchar *change = NULL;

		if (n > 0) {
			axis_proxy_list_name_result(list, names[i], answer, n, NULL, &change);
			g_free(change);
			if (!axis_proxy_list_contains(list, &answer[0]))
				abort();
		}
		axis_proxy_list_name_result(list, names[i], NULL, 0, "fuzz", &change);
		g_free(change);
		if (n > 0 && !axis_proxy_list_contains(list, &answer[0]))
			abort();
	}

	entries = axis_proxy_list_describe_entries(list);
	if (entries->len != axis_proxy_list_get_length(list))
		abort();
	for (i = 0; i < entries->len; i++) {
		AxisProxyNameInfo *info = g_ptr_array_index(entries, i);

		if (g_strv_length(info->addresses) > AXIS_PROXY_NAME_MAX_ADDRESSES && info->is_name)
			abort();
	}
	g_ptr_array_unref(entries);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);
	g_auto(GStrv) parts = g_strsplit(text, "\n", 5);
	const gchar *list_text = parts[0] != NULL ? parts[0] : "";
	const gchar *peer = parts[0] != NULL ? parts[1] : NULL;
	const gchar *forwarded_for = peer != NULL ? parts[2] : NULL;
	const gchar *forwarded_proto = forwarded_for != NULL ? parts[3] : NULL;
	const gchar *forwarded_host = forwarded_proto != NULL ? parts[4] : NULL;
	GError *error = NULL;
	AxisProxyList *list;
	AxisProxyList *fixed;

	list = axis_proxy_list_parse(list_text, &error);
	if (list == NULL) {
		if (error == NULL || error->domain != AXIS_PROXY_ERROR)
			abort();
		g_error_free(error);
	} else {
		gchar *canonical;
		AxisProxyList *again;
		gchar *canonical_again;

		if (error != NULL || axis_proxy_list_get_length(list) > AXIS_PROXY_LIST_MAX_ENTRIES)
			abort();
		canonical = axis_proxy_list_to_string(list);
		again = axis_proxy_list_parse(canonical, NULL);
		if (again == NULL)
			abort();
		canonical_again = axis_proxy_list_to_string(again);
		if (strcmp(canonical, canonical_again) != 0 ||
		    axis_proxy_list_get_length(again) != axis_proxy_list_get_length(list))
			abort();
		g_free(canonical);
		g_free(canonical_again);
		axis_proxy_list_free(again);

		check_resolve(list, peer, forwarded_for, forwarded_proto, forwarded_host);
		check_names(list, peer);
		check_resolve(list, peer, forwarded_for, forwarded_proto, forwarded_host);
		axis_proxy_list_free(list);
	}

	/* The headers against a list that trusts the usual private ranges,
	 * whatever the first line was, so they are always exercised. */
	fixed = axis_proxy_list_parse("10.0.0.0/8, 127.0.0.1, 192.168.0.0/16, ::1, fd00::/8", NULL);
	check_resolve(fixed, peer, forwarded_for, forwarded_proto, forwarded_host);
	check_resolve(fixed, "10.1.2.3", peer, forwarded_proto, forwarded_for);
	check_resolve(fixed, "10.1.2.3", forwarded_for, forwarded_proto, list_text);
	check_resolve(NULL, peer, forwarded_for, forwarded_proto, forwarded_host);
	axis_proxy_list_free(fixed);

	/* The host rule alone, on every line. */
	check_host_rule(list_text);
	check_host_rule(peer);
	check_host_rule(forwarded_host);
	check_host_rule(text);

	return 0;
}
