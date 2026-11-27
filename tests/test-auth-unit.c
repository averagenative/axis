/*
 * test-auth-unit.c - the parts of web authentication that need no server
 * or database: trusted proxies, cookies, the "next" check, usernames,
 * origins, the login throttle and password hashing
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <sodium.h>
#include <string.h>

#include "core/axis-proxy.h"
#include "web/axis-login-throttle.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"

#define SECOND G_GINT64_CONSTANT(1000000)

/* ── Trusted proxies ────────────────────────────────────────────── */

static void
assert_list(const gchar *text, const gchar *canonical)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisProxyList) list = axis_proxy_list_parse(text, &error);
	g_autofree gchar *again = NULL;

	g_assert_no_error(error);
	g_assert_nonnull(list);
	again = axis_proxy_list_to_string(list);
	g_assert_cmpstr(again, ==, canonical);
}

static void
assert_list_refused(const gchar *text, const gchar *mentions)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisProxyList) list = axis_proxy_list_parse(text, &error);

	g_assert_null(list);
	g_assert_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID);
	if (mentions != NULL && strstr(error->message, mentions) == NULL)
		g_error("'%s': expected the error to mention '%s': %s", text, mentions, error->message);
}

static void
test_proxy_list_parse(void)
{
	assert_list("", "");
	assert_list("   ", "");
	assert_list("10.0.0.1", "10.0.0.1");
	assert_list(" 10.0.0.1 , 192.168.0.0/16 ", "10.0.0.1, 192.168.0.0/16");
	assert_list("192.168.1.10/24", "192.168.1.0/24");
	assert_list("10.0.0.1/32", "10.0.0.1");
	assert_list("2001:DB8::1", "2001:db8::1");
	assert_list("2001:db8::/32,::1", "2001:db8::/32, ::1");
	assert_list("::ffff:10.1.2.3", "10.1.2.3");
	assert_list("[::1]", "::1");

	assert_list_refused("-proxy.example", "'-proxy.example'");
	assert_list_refused("10.0.0.1,,10.0.0.2", "empty");
	assert_list_refused("10.0.0.1,", "empty");
	assert_list_refused("10.0.0.0/33", "'10.0.0.0/33'");
	assert_list_refused("10.0.0.0/", "'10.0.0.0/'");
	assert_list_refused("10.0.0.0/08", "'10.0.0.0/08'");
	assert_list_refused("10.0.0.0/+8", NULL);
	assert_list_refused("10.0.0.0/0", "every address");
	assert_list_refused("::/0", "every address");
	assert_list_refused("10.0.0.256", NULL);
	assert_list_refused("10.0.0", NULL);
	assert_list_refused("010.0.0.1", NULL);
	assert_list_refused("10.0.0.1:80", NULL);
	assert_list_refused("fe80::1%eth0", NULL);
	assert_list_refused("[::1]/128", NULL);
	assert_list_refused("[10.0.0.1]", NULL);
	assert_list_refused("2001:db8::/129", NULL);
}

static void
test_proxy_list_limits(void)
{
	GString *text = g_string_new(NULL);
	GString *canonical = g_string_new(NULL);
	guint i;

	for (i = 0; i < AXIS_PROXY_LIST_MAX_ENTRIES; i++) {
		g_string_append_printf(text, "%s10.0.%u.%u", i > 0 ? "," : "", i / 256, i % 256);
		g_string_append_printf(canonical, "%s10.0.%u.%u", i > 0 ? ", " : "", i / 256, i % 256);
	}
	assert_list(text->str, canonical->str);
	g_string_append(text, ",10.9.9.9");
	assert_list_refused(text->str, "more than");
	g_string_free(text, TRUE);
	g_string_free(canonical, TRUE);
}

static AxisIpAddress
address(const gchar *text)
{
	AxisIpAddress parsed;

	g_assert_true(axis_ip_address_parse(text, &parsed));
	return parsed;
}

static void
test_proxy_contains(void)
{
	g_autoptr(AxisProxyList) list = axis_proxy_list_parse("10.0.0.0/8, 192.168.1.5, 2001:db8::/32", NULL);
	AxisIpAddress a;

	a = address("10.200.3.4");
	g_assert_true(axis_proxy_list_contains(list, &a));
	a = address("11.0.0.1");
	g_assert_false(axis_proxy_list_contains(list, &a));
	a = address("192.168.1.5");
	g_assert_true(axis_proxy_list_contains(list, &a));
	a = address("192.168.1.6");
	g_assert_false(axis_proxy_list_contains(list, &a));
	a = address("2001:db8:ffff::9");
	g_assert_true(axis_proxy_list_contains(list, &a));
	a = address("2001:db9::9");
	g_assert_false(axis_proxy_list_contains(list, &a));
	/* An IPv4-mapped peer is its IPv4 address. */
	a = address("::ffff:10.1.1.1");
	g_assert_true(axis_proxy_list_contains(list, &a));
	a = address("10.1.1.1");
	g_assert_false(axis_proxy_list_contains(NULL, &a));
}

/* Whether two lists share an address (the portal's addresses against the
 * trusted proxies): a range holds an address, a range holds a range, either
 * way round; never across the families; a name stands for what its lookup
 * found, and for nothing before. */
static void
test_proxy_lists_overlap(void)
{
	static const struct
	{
		const gchar *mine;
		const gchar *theirs;
		const gchar *entry;	/* NULL: no overlap */
		const gchar *other;
	} cases[] = {
		{ "172.30.1.3", "172.30.0.2", NULL, NULL },
		{ "172.30.1.3", "172.30.1.3", "172.30.1.3", "172.30.1.3" },
		{ "172.30.1.3", "10.0.0.1, 172.30.0.0/16", "172.30.1.3", "172.30.0.0/16" },
		{ "172.30.1.0/24", "172.30.1.2", "172.30.1.0/24", "172.30.1.2" },
		{ "172.30.1.0/24", "172.30.0.0/16", "172.30.1.0/24", "172.30.0.0/16" },
		{ "172.30.0.0/16", "172.30.1.0/24", "172.30.0.0/16", "172.30.1.0/24" },
		{ "172.30.1.0/25", "172.30.1.128/25", NULL, NULL },
		{ "192.0.2.16/28", "192.0.2.32", NULL, NULL },
		{ "192.0.2.16/28", "192.0.2.31", "192.0.2.16/28", "192.0.2.31" },
		{ "::ffff:172.30.1.3", "172.30.1.3", "172.30.1.3", "172.30.1.3" },
		{ "2001:db8:1:2::7", "2001:db8:1:2::8", NULL, NULL },
		{ "2001:db8:1:2::7", "2001:db8:1:2::/64", "2001:db8:1:2::7", "2001:db8:1:2::/64" },
		{ "2001:db8::/32", "10.0.0.0/8", NULL, NULL },
		{ "", "10.0.0.0/8", NULL, NULL },
		{ "10.0.0.1", "", NULL, NULL },
		{ "10.0.0.1", "traefik", NULL, NULL }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(AxisProxyList) mine = axis_proxy_list_parse(cases[i].mine, NULL);
		g_autoptr(AxisProxyList) theirs = axis_proxy_list_parse(cases[i].theirs, NULL);
		g_autofree gchar *entry = NULL;
		g_autofree gchar *other = NULL;

		g_assert_nonnull(mine);
		g_assert_nonnull(theirs);
		g_test_message("%s against %s", cases[i].mine, cases[i].theirs);
		g_assert_true(axis_proxy_lists_overlap(mine, theirs, &entry, &other) == (cases[i].entry != NULL));
		g_assert_cmpstr(entry, ==, cases[i].entry);
		g_assert_cmpstr(other, ==, cases[i].other);
	}
	g_assert_false(axis_proxy_lists_overlap(NULL, NULL, NULL, NULL));
	{
		g_autoptr(AxisProxyList) mine = axis_proxy_list_parse("172.30.2.0/24", NULL);
		g_autoptr(AxisProxyList) theirs = axis_proxy_list_parse("10.9.9.9, traefik", NULL);
		AxisIpAddress found[2];
		g_autofree gchar *entry = NULL;
		g_autofree gchar *other = NULL;

		found[0] = address("172.30.0.2");
		found[1] = address("172.30.2.2");
		g_assert_false(axis_proxy_lists_overlap(mine, theirs, NULL, NULL));
		axis_proxy_list_name_result(theirs, "traefik", found, 2, NULL, NULL);
		g_assert_true(axis_proxy_lists_overlap(mine, theirs, &entry, &other));
		g_assert_cmpstr(entry, ==, "172.30.2.0/24");
		g_assert_cmpstr(other, ==, "traefik");
	}
}

typedef struct
{
	const gchar *trusted;
	const gchar *peer;
	gboolean     peer_https;
	const gchar *forwarded_for;
	const gchar *forwarded_proto;
	const gchar *client;
	gboolean     https;
} ResolveCase;

static void
test_proxy_resolve(void)
{
	static const ResolveCase cases[] = {
		/* Spec: header from an untrusted peer is ignored. */
		{ "",             "198.51.100.7", FALSE, "203.0.113.9", "https", "198.51.100.7", FALSE },
		{ "10.0.0.1",     "198.51.100.7", FALSE, "203.0.113.9", "https", "198.51.100.7", FALSE },
		/* Spec: header from a trusted proxy is used. */
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "203.0.113.9", "https", "203.0.113.9",  TRUE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "203.0.113.9", "HTTPS", "203.0.113.9",  TRUE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "203.0.113.9", "http",  "203.0.113.9",  FALSE },
		{ "10.0.0.1",     "10.0.0.1",     TRUE,  "203.0.113.9", NULL,    "203.0.113.9",  TRUE },
		{ "10.0.0.1",     "10.0.0.1",     TRUE,  "203.0.113.9", "http",  "203.0.113.9",  FALSE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "203.0.113.9", "bogus", "203.0.113.9",  FALSE },
		/* What the client wrote on the left is never believed. */
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "1.1.1.1, 203.0.113.9", NULL, "203.0.113.9", FALSE },
		{ "10.0.0.0/8",   "10.0.0.1",     FALSE, "1.1.1.1, 203.0.113.9, 10.0.0.2", "http, https",
		  "203.0.113.9", TRUE },
		/* Every hop trusted: the leftmost. */
		{ "10.0.0.0/8",   "10.0.0.1",     FALSE, "10.0.0.3,10.0.0.2", NULL, "10.0.0.3", FALSE },
		/* Garbage stops the walk at the last address read. */
		{ "10.0.0.0/8",   "10.0.0.1",     FALSE, "203.0.113.9, garbage", NULL, "10.0.0.1", FALSE },
		{ "10.0.0.0/8",   "10.0.0.1",     FALSE, "203.0.113.9, , 10.0.0.2", NULL, "10.0.0.2", FALSE },
		{ "10.0.0.0/8",   "10.0.0.1",     FALSE, "", NULL, "10.0.0.1", FALSE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, NULL, NULL, "10.0.0.1", FALSE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, " [2001:db8::5] ", NULL, "2001:db8::5", FALSE },
		{ "10.0.0.1",     "10.0.0.1",     FALSE, "203.0.113.9:4711", NULL, "10.0.0.1", FALSE },
		/* A peer that is not an address. */
		{ "10.0.0.1",     NULL,           FALSE, "203.0.113.9", "https", AXIS_PROXY_UNKNOWN_CLIENT, FALSE },
		{ "10.0.0.1",     "not-an-ip",    TRUE,  "203.0.113.9", "http",  AXIS_PROXY_UNKNOWN_CLIENT, TRUE },
		{ "::1",          "::ffff:127.0.0.1", FALSE, "203.0.113.9", NULL, "127.0.0.1", FALSE },
		{ "127.0.0.1",    "::ffff:127.0.0.1", FALSE, "203.0.113.9", NULL, "203.0.113.9", FALSE },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		const ResolveCase *c = &cases[i];
		g_autoptr(AxisProxyList) list = axis_proxy_list_parse(c->trusted, NULL);
		g_autofree gchar *client = NULL;
		gboolean https = !c->https;

		g_assert_nonnull(list);
		axis_proxy_resolve(list, c->peer, c->peer_https, c->forwarded_for, c->forwarded_proto,
			&client, &https);
		if (g_strcmp0(client, c->client) != 0 || https != c->https)
			g_error("case %u: got %s/%d, expected %s/%d", i, client, https, c->client, c->https);
	}
}

static void
test_proxy_host_valid(void)
{
	static const gchar *const good[] = {
		"axis.example", "AXIS.example:8443", "localhost", "127.0.0.1", "127.0.0.1:7800",
		"[::1]", "[2001:db8::7]:443", "media_box.lan", "a", "x.y-z:65535", "x:1", NULL
	};
	static const gchar *const bad[] = {
		"", ":443", "axis.example:", "axis.example:0", "axis.example:65536", "axis.example:0443",
		"axis.example:443:1", "axis.example:12a", "axis example", "axis.example,evil.example",
		"user@axis.example", "axis.example/path", "[::1", "[]", "[10.0.0.1]", "[::1]x", "[::1]:",
		"[fe80::1%eth0]", "axis.example\r\nX: y", "\xc3\xa9.example", "axis.example:123456", NULL
	};
	g_autofree gchar *longest = g_strnfill(AXIS_PROXY_HOST_MAX_LEN, 'a');
	g_autofree gchar *too_long = g_strnfill(AXIS_PROXY_HOST_MAX_LEN + 1, 'a');
	guint i;

	for (i = 0; good[i] != NULL; i++) {
		if (!axis_proxy_host_is_valid(good[i]))
			g_error("'%s' was refused", good[i]);
	}
	for (i = 0; bad[i] != NULL; i++) {
		if (axis_proxy_host_is_valid(bad[i]))
			g_error("'%s' was accepted", bad[i]);
	}
	g_assert_false(axis_proxy_host_is_valid(NULL));
	g_assert_true(axis_proxy_host_is_valid(longest));
	g_assert_false(axis_proxy_host_is_valid(too_long));
}

typedef struct
{
	const gchar *trusted;
	const gchar *peer;
	gboolean     peer_https;
	const gchar *forwarded_for;
	const gchar *forwarded_proto;
	const gchar *forwarded_host;
	const gchar *client;
	gboolean     https;
	const gchar *host;
} ResolveFullCase;

/*
 * The decisions that the audit lines, the throttles, the Secure cookies and
 * the cross-site check depend on: what an unlisted peer sends is never
 * believed, IPv6 and IPv4-mapped peers are compared as addresses, a chain of
 * listed proxies is walked from the right, and junk is ignored, not guessed.
 */
static void
test_proxy_resolve_full(void)
{
	static const ResolveFullCase cases[] = {
		/* Spoofing from a peer that is not listed: everything ignored. */
		{ "",              "198.51.100.7", FALSE, "203.0.113.9", "https", "axis.example",
		  "198.51.100.7", FALSE, NULL },
		{ "10.0.0.0/8",    "198.51.100.7", FALSE, "10.0.0.5", "https", "axis.example",
		  "198.51.100.7", FALSE, NULL },
		{ "10.0.0.0/8",    "11.0.0.1",     FALSE, "203.0.113.9", "https", "axis.example",
		  "11.0.0.1", FALSE, NULL },
		/* A listed proxy: all three believed. */
		{ "10.0.0.1",      "10.0.0.1",     FALSE, "203.0.113.9", "https", "Axis.Example:8443",
		  "203.0.113.9", TRUE, "axis.example:8443" },
		/* IPv6: a listed range, an IPv6 client, an IPv6 host. */
		{ "fd00::/8",      "fd00::2",      FALSE, "2001:db8::9", "https", "[2001:db8::1]:443",
		  "2001:db8::9", TRUE, "[2001:db8::1]:443" },
		{ "fd00::/8",      "fe80::2",      FALSE, "2001:db8::9", "https", "axis.example",
		  "fe80::2", FALSE, NULL },
		/* An IPv4-mapped peer is its IPv4 address, either way round. */
		{ "172.16.0.0/12", "::ffff:172.18.0.2", FALSE, "::ffff:203.0.113.9", "https", "axis.example",
		  "203.0.113.9", TRUE, "axis.example" },
		{ "::ffff:172.18.0.2", "172.18.0.2", FALSE, "203.0.113.9", NULL, NULL,
		  "203.0.113.9", FALSE, NULL },
		{ "2001:db8::/32", "::ffff:10.0.0.1", FALSE, "203.0.113.9", "https", "axis.example",
		  "10.0.0.1", FALSE, NULL },
		/* A chain: the rightmost address that is not a listed proxy, and the
		 * nearest proxy's scheme and host. */
		{ "10.0.0.0/8, 192.168.1.1", "10.0.0.1", FALSE, "1.2.3.4, 203.0.113.9, 192.168.1.1, 10.0.0.2",
		  "http, https", "evil.example, axis.example", "203.0.113.9", TRUE, "axis.example" },
		/* The client wrote "trusted-looking" addresses on the left: never read. */
		{ "10.0.0.0/8",    "10.0.0.1",     FALSE, "10.0.0.9, 198.51.100.4", NULL, NULL,
		  "198.51.100.4", FALSE, NULL },
		/* Junk: ignored, the connection's own word stands. */
		{ "10.0.0.1",      "10.0.0.1",     TRUE,  "203.0.113.9", "bogus", "bad host", "203.0.113.9", TRUE, NULL },
		{ "10.0.0.1",      "10.0.0.1",     FALSE, "203.0.113.9", "", "", "203.0.113.9", FALSE, NULL },
		{ "10.0.0.1",      "10.0.0.1",     FALSE, "203.0.113.9", "https, junk", "axis.example, a b",
		  "203.0.113.9", FALSE, NULL },
		{ "10.0.0.1",      "10.0.0.1",     FALSE, "not-an-address", "https", "axis.example",
		  "10.0.0.1", TRUE, "axis.example" },
		{ "10.0.0.1",      "10.0.0.1",     FALSE, "203.0.113.9", "https", "axis.example\r\nSet-Cookie: x",
		  "203.0.113.9", TRUE, NULL },
		/* No headers at all from a listed proxy: the proxy is the client. */
		{ "10.0.0.1",      "10.0.0.1",     FALSE, NULL, NULL, NULL, "10.0.0.1", FALSE, NULL },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		const ResolveFullCase *c = &cases[i];
		g_autoptr(AxisProxyList) list = axis_proxy_list_parse(c->trusted, NULL);
		g_autofree gchar *client = NULL;
		g_autofree gchar *host = NULL;
		gboolean https = !c->https;

		g_assert_nonnull(list);
		axis_proxy_resolve_full(list, c->peer, c->peer_https, c->forwarded_for, c->forwarded_proto,
			c->forwarded_host, &client, &https, &host);
		if (g_strcmp0(client, c->client) != 0 || https != c->https || g_strcmp0(host, c->host) != 0)
			g_error("case %u: got %s/%d/%s, expected %s/%d/%s", i, client, https, host,
				c->client, c->https, c->host);
	}
}

static void
test_proxy_resolve_hop_limit(void)
{
	g_autoptr(AxisProxyList) list = axis_proxy_list_parse("10.0.0.0/8", NULL);
	GString *chain = g_string_new("203.0.113.9");
	g_autofree gchar *client = NULL;
	gboolean https;
	guint i;

	for (i = 0; i < AXIS_PROXY_MAX_HOPS + 5; i++)
		g_string_append_printf(chain, ", 10.0.0.%u", i + 2);
	axis_proxy_resolve(list, "10.0.0.1", FALSE, chain->str, NULL, &client, &https);
	/* The walk gives up inside the proxies rather than reading on. */
	g_assert_true(g_str_has_prefix(client, "10.0.0."));
	g_string_free(chain, TRUE);
}

/* ── Cookies ────────────────────────────────────────────────────── */

static void
assert_cookie(const gchar *header, const gchar *name, const gchar *expected)
{
	g_autofree gchar *value = axis_cookie_find(header, name);

	if (g_strcmp0(value, expected) != 0)
		g_error("cookie '%s' in '%s': got '%s', expected '%s'", name,
			header != NULL ? header : "(null)", value, expected);
}

static void
test_cookie_find(void)
{
	gchar *long_header;

	assert_cookie(NULL, "a", NULL);
	assert_cookie("", "a", NULL);
	assert_cookie("a=1", "a", "1");
	assert_cookie("a=1; b=2", "b", "2");
	assert_cookie("  a = 1 ;b=2", "a", "1");
	assert_cookie("ab=1; a=2", "a", "2");
	assert_cookie("a=", "a", "");
	assert_cookie("a=x=y", "a", "x=y");
	assert_cookie("A=1", "a", NULL);
	assert_cookie("a", "a", NULL);
	assert_cookie(";;; a=1 ;;", "a", "1");
	/* Twice is ambiguous: neither is taken. */
	assert_cookie("a=1; a=2", "a", NULL);
	assert_cookie("a=1; b=2; a=1", "a", NULL);
	assert_cookie("__Host-a=1; a=2", "a", "2");
	assert_cookie("__Host-a=1; a=2", "__Host-a", "1");

	long_header = g_strnfill(AXIS_COOKIE_HEADER_MAX + 1, 'x');
	long_header[0] = 'a';
	long_header[1] = '=';
	assert_cookie(long_header, "a", NULL);
	g_free(long_header);
}

/* ── next ───────────────────────────────────────────────────────── */

static void
test_next_path(void)
{
	static const gchar *const good[] = {
		"/", "/movies", "/movies?sort=size&dir=desc", "/a/b/c", "/%2F%2Fevil.example",
		"/x#frag", "/~user", "/a:b", "/a@b", NULL
	};
	static const gchar *const bad[] = {
		"", "movies", "//evil.example", "/\\evil.example", "\\\\evil.example",
		"https://evil.example/", "http:/evil.example", "//", "/\tevil", "/ evil", "/\n/evil.example",
		"/a\\b", "/%", "/%2", "/%zz", "/<script>", "/\"", "/a b", "/\x7f", "/\xc3\xa9",
		" /movies", "javascript:alert(1)", "/{x}", "/a|b", "/a^b", "/a`b", NULL
	};
	gchar *longest;
	guint i;

	for (i = 0; good[i] != NULL; i++) {
		if (!axis_auth_next_is_safe(good[i]))
			g_error("'%s' should be accepted", good[i]);
	}
	for (i = 0; bad[i] != NULL; i++) {
		if (axis_auth_next_is_safe(bad[i]))
			g_error("'%s' should be refused", bad[i]);
	}
	g_assert_false(axis_auth_next_is_safe(NULL));

	longest = g_strnfill(AXIS_NEXT_MAX_LEN, 'a');
	longest[0] = '/';
	g_assert_true(axis_auth_next_is_safe(longest));
	g_free(longest);
	longest = g_strnfill(AXIS_NEXT_MAX_LEN + 1, 'a');
	longest[0] = '/';
	g_assert_false(axis_auth_next_is_safe(longest));
	g_free(longest);
}

/* ── Usernames, tokens, passwords, origins ──────────────────────── */

static void
assert_username(const gchar *input, const gchar *expected)
{
	g_autofree gchar *name = axis_auth_normalise_username(input);

	if (g_strcmp0(name, expected) != 0)
		g_error("username '%s': got '%s', expected '%s'", input, name, expected);
}

static void
test_username(void)
{
	gchar *longest;

	assert_username("alice", "alice");
	assert_username("Alice", "alice");
	assert_username("  ALICE.B_c-9 ", "alice.b_c-9");
	assert_username("9lives", "9lives");
	assert_username("", NULL);
	assert_username("   ", NULL);
	assert_username(NULL, NULL);
	assert_username(".alice", NULL);
	assert_username("-alice", NULL);
	assert_username("al ice", NULL);
	assert_username("al@ice", NULL);
	assert_username("\xc3\x84lice", NULL);
	assert_username("alice\n", NULL);

	longest = g_strnfill(AXIS_USERNAME_MAX_LEN, 'a');
	assert_username(longest, longest);
	g_free(longest);
	longest = g_strnfill(AXIS_USERNAME_MAX_LEN + 1, 'a');
	assert_username(longest, NULL);
	g_free(longest);
}

static void
test_token_shape(void)
{
	g_autofree gchar *token = axis_random_token();
	g_autofree gchar *other = axis_random_token();
	g_autofree gchar *public_id = axis_random_public_id();
	g_autofree gchar *setup = axis_random_setup_token();
	g_autofree gchar *hash = axis_token_hash(token);
	g_autofree gchar *derived = axis_token_derive(token, "csrf");
	g_autofree gchar *derived_again = axis_token_derive(token, "csrf");
	g_autofree gchar *derived_other = axis_token_derive(token, "other");
	g_autofree gchar *derived_from_other = axis_token_derive(other, "csrf");
	guint i;

	g_assert_true(axis_auth_token_is_wellformed(token, AXIS_TOKEN_LEN));
	g_assert_cmpstr(token, !=, other);
	g_assert_true(axis_auth_token_is_wellformed(public_id, AXIS_PUBLIC_ID_LEN));
	g_assert_cmpuint(strlen(setup), ==, AXIS_SETUP_TOKEN_LEN);
	for (i = 0; setup[i] != '\0'; i++)
		g_assert_true(g_ascii_isxdigit(setup[i]) && !g_ascii_isupper(setup[i]));

	g_assert_cmpuint(strlen(hash), ==, 64);
	g_assert_null(strstr(hash, token));
	g_assert_true(axis_auth_token_is_wellformed(derived, AXIS_TOKEN_LEN));
	g_assert_cmpstr(derived, ==, derived_again);
	g_assert_cmpstr(derived, !=, derived_other);
	g_assert_cmpstr(derived, !=, derived_from_other);
	g_assert_cmpstr(derived, !=, token);

	g_assert_false(axis_auth_token_is_wellformed(NULL, 4));
	g_assert_false(axis_auth_token_is_wellformed("abc", 4));
	g_assert_false(axis_auth_token_is_wellformed("abcde", 4));
	g_assert_false(axis_auth_token_is_wellformed("ab+d", 4));
	g_assert_true(axis_auth_token_is_wellformed("a-_9", 4));

	g_assert_true(axis_secret_equal("abc", "abc"));
	g_assert_false(axis_secret_equal("abc", "abd"));
	g_assert_false(axis_secret_equal("abc", "abcd"));
	g_assert_false(axis_secret_equal(NULL, "abc"));
	g_assert_false(axis_secret_equal("abc", NULL));
}

static void
test_password_rules(void)
{
	gchar *longest;

	g_assert_null(axis_auth_check_password("0123456789"));
	g_assert_null(axis_auth_check_password("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
		"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"));
	g_assert_nonnull(strstr(axis_auth_check_password("012345678"), "at least 10 characters"));
	g_assert_nonnull(strstr(axis_auth_check_password(""), "at least 10 characters"));
	/* Ten bytes, five characters. */
	g_assert_nonnull(strstr(axis_auth_check_password("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"),
		"at least 10 characters"));
	g_assert_nonnull(axis_auth_check_password("0123456789\xff"));
	g_assert_nonnull(axis_auth_check_password(NULL));

	longest = g_strnfill(AXIS_PASSWORD_MAX_BYTES, 'p');
	g_assert_null(axis_auth_check_password(longest));
	g_free(longest);
	longest = g_strnfill(AXIS_PASSWORD_MAX_BYTES + 1, 'p');
	g_assert_nonnull(strstr(axis_auth_check_password(longest), "1024"));
	g_free(longest);
}

static void
test_origin(void)
{
	g_assert_true(axis_auth_origin_matches("http://axis.local:7800", "axis.local:7800", FALSE));
	g_assert_true(axis_auth_origin_matches("HTTP://AXIS.local:7800", "axis.LOCAL:7800", FALSE));
	g_assert_true(axis_auth_origin_matches("https://axis.example", "axis.example", TRUE));
	g_assert_true(axis_auth_origin_matches("https://axis.example", "axis.example:443", TRUE));
	g_assert_true(axis_auth_origin_matches("https://axis.example:443", "axis.example", TRUE));
	g_assert_true(axis_auth_origin_matches("http://axis.example", "axis.example:80", FALSE));
	/* A proxy we do not know of may have terminated TLS. */
	g_assert_true(axis_auth_origin_matches("https://axis.example", "axis.example", FALSE));

	g_assert_false(axis_auth_origin_matches("http://axis.example", "axis.example", TRUE));
	g_assert_false(axis_auth_origin_matches("https://evil.example", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("https://axis.example.evil", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("https://axis.example:8443", "axis.example", TRUE));
	g_assert_false(axis_auth_origin_matches("https://axis.example/", "axis.example", TRUE));
	g_assert_false(axis_auth_origin_matches("https://user@axis.example", "axis.example", TRUE));
	g_assert_false(axis_auth_origin_matches("null", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("ftp://axis.example", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("https://", "axis.example", FALSE));
	g_assert_false(axis_auth_origin_matches("https://axis.example", NULL, FALSE));
	g_assert_false(axis_auth_origin_matches(NULL, "axis.example", FALSE));
}

static void
test_user_agent(void)
{
	g_autofree gchar *empty = axis_auth_clean_user_agent(NULL);
	g_autofree gchar *clean = axis_auth_clean_user_agent("Mozilla/5.0 \x01\xc3\xa9");
	g_autofree gchar *longest = g_strnfill(500, 'u');
	g_autofree gchar *cut = axis_auth_clean_user_agent(longest);

	g_assert_cmpstr(empty, ==, "");
	g_assert_cmpstr(clean, ==, "Mozilla/5.0 ???");
	g_assert_cmpuint(strlen(cut), ==, AXIS_USER_AGENT_MAX_LEN);
}

/* ── Throttle ───────────────────────────────────────────────────── */

static void
test_throttle_delays(void)
{
	g_autoptr(AxisLoginThrottle) throttle = axis_login_throttle_new(3, 2 * SECOND, 60 * SECOND,
		3600 * SECOND, 100);
	gint64 now = 1000 * SECOND;
	guint i;

	for (i = 0; i < 3; i++) {
		g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 0);
		axis_login_throttle_fail(throttle, "alice", now);
	}
	/* Three free failures, no delay yet. */
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 0);

	axis_login_throttle_fail(throttle, "alice", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 2 * SECOND);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now + SECOND), ==, SECOND);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now + 2 * SECOND), ==, 0);
	/* Another key is unaffected. */
	g_assert_cmpint(axis_login_throttle_check(throttle, "bob", now), ==, 0);
	g_assert_cmpint(axis_login_throttle_check(throttle, NULL, now), ==, 0);

	now += 2 * SECOND;
	axis_login_throttle_fail(throttle, "alice", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 4 * SECOND);
	now += 4 * SECOND;
	axis_login_throttle_fail(throttle, "alice", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 8 * SECOND);

	/* Doubling stops at the cap. */
	for (i = 0; i < 40; i++)
		axis_login_throttle_fail(throttle, "alice", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 60 * SECOND);

	/* Success clears it. */
	axis_login_throttle_clear(throttle, "alice");
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 0);
	axis_login_throttle_fail(throttle, "alice", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "alice", now), ==, 0);
}

static void
test_throttle_forgets(void)
{
	g_autoptr(AxisLoginThrottle) throttle = axis_login_throttle_new(0, SECOND, 10 * SECOND,
		100 * SECOND, 100);
	gint64 now = 5000 * SECOND;

	axis_login_throttle_fail(throttle, "k", now);
	axis_login_throttle_fail(throttle, "k", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "k", now), ==, 2 * SECOND);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 1);

	/* A day without failures: the count starts again. */
	now += 100 * SECOND;
	g_assert_cmpint(axis_login_throttle_check(throttle, "k", now), ==, 0);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 0);
	axis_login_throttle_fail(throttle, "k", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "k", now), ==, SECOND);
}

static void
test_throttle_bounded(void)
{
	g_autoptr(AxisLoginThrottle) throttle = axis_login_throttle_new(0, SECOND, 10 * SECOND,
		1000 * SECOND, 50);
	gint64 now = 1;
	g_autofree gchar *huge = g_strnfill(10000, 'k');
	guint i;

	axis_login_throttle_fail(throttle, "first", now);
	for (i = 0; i < 1000; i++) {
		g_autofree gchar *key = g_strdup_printf("key-%u", i);

		/* Each earlier key's delay (a second) has ended. */
		now += 2 * SECOND;
		axis_login_throttle_fail(throttle, key, now);
		g_assert_cmpuint(axis_login_throttle_get_size(throttle), <=, 50);
	}
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 50);

	/* The oldest went first; the newest are still counted. */
	g_assert_cmpint(axis_login_throttle_check(throttle, "first", now), ==, 0);
	g_assert_cmpint(axis_login_throttle_check(throttle, "key-999", now), >, 0);

	/* A long key is cut, not stored whole. */
	axis_login_throttle_fail(throttle, huge, now);
	g_assert_cmpint(axis_login_throttle_check(throttle, huge, now), >, 0);
}

/*
 * Found by the review of fix-web-hunt-findings: making room dropped the
 * key whose last failure was oldest even while its delay ran, so a flood of
 * other keys ended a delay.  A key in its delay is kept; the table grows.
 */
static void
test_throttle_flood_keeps_delay(void)
{
	g_autoptr(AxisLoginThrottle) throttle = axis_login_throttle_new(0, 10 * SECOND, 60 * SECOND,
		1000 * SECOND, 20);
	gint64 now = 1;
	guint i;

	axis_login_throttle_fail(throttle, "target", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "target", now), ==, 10 * SECOND);
	for (i = 0; i < 100; i++) {
		g_autofree gchar *key = g_strdup_printf("flood-%u", i);

		now++;
		axis_login_throttle_fail(throttle, key, now);
	}
	g_assert_cmpint(axis_login_throttle_check(throttle, "target", now), >, 9 * SECOND);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 101);

	/* Once the delays have ended, room is made again. */
	now += 11 * SECOND;
	axis_login_throttle_fail(throttle, "later", now);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), <=, 101);
}

/*
 * Found by the review of add-request-portal P3: a sender with many
 * addresses (an IPv6 /48 is 65,536 keys of a /64) made a full table drop the
 * keys it had used, each with its free failures spent, and started each of
 * them again.  With axis_login_throttle_share_when_full() no key that has
 * failures is dropped: the new keys of a full table share one count, which
 * the sender then spends for all of them, and the table never grows.
 */
static void
test_throttle_shared_when_full(void)
{
	g_autoptr(AxisLoginThrottle) throttle = axis_login_throttle_new(2, 10 * SECOND, 60 * SECOND,
		1000 * SECOND, 3);
	gint64 now = 1;
	guint i;

	axis_login_throttle_share_when_full(throttle, 100 * SECOND);
	/* Three keys, each with its free failures spent. */
	for (i = 0; i < 2; i++) {
		axis_login_throttle_fail(throttle, "a", now);
		axis_login_throttle_fail(throttle, "b", now);
		axis_login_throttle_fail(throttle, "c", now);
	}
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 3);
	g_assert_cmpint(axis_login_throttle_check(throttle, "a", now), ==, 0);

	/* A hundred new keys: none is given room, none drops a key, and after
	 * the shared free failures each of them waits. */
	for (i = 0; i < 100; i++) {
		g_autofree gchar *key = g_strdup_printf("rotated-%u", i);

		now++;
		if (axis_login_throttle_check(throttle, key, now) == 0)
			axis_login_throttle_fail(throttle, key, now);
		g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 3);
	}
	g_assert_cmpint(axis_login_throttle_check(throttle, "rotated-500", now), >, 0);
	/* The keys that were there keep what they had: one more failure each
	 * starts a delay. */
	axis_login_throttle_fail(throttle, "a", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "a", now), ==, 10 * SECOND);

	/* Attempts in flight on the shared count are counted, and end there. */
	now += 200 * SECOND;
	g_assert_cmpint(axis_login_throttle_check(throttle, "new", now), ==, 0);
	axis_login_throttle_begin(throttle, "new", now);
	axis_login_throttle_begin(throttle, "other", now);
	axis_login_throttle_begin(throttle, "third", now);
	g_assert_cmpint(axis_login_throttle_check(throttle, "fourth", now), >, 0);
	axis_login_throttle_end(throttle, "new");
	axis_login_throttle_end(throttle, "other");
	axis_login_throttle_end(throttle, "third");
	g_assert_cmpint(axis_login_throttle_check(throttle, "fourth", now), ==, 0);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 3);
	/* A success of a key on the shared count clears nobody's count. */
	axis_login_throttle_clear(throttle, "new");
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 3);

	/* Once the keys of the table are forgotten (a day without a failure,
	 * here 1000 seconds), new keys have room of their own again. */
	now += 1000 * SECOND;
	axis_login_throttle_fail(throttle, "after", now);
	g_assert_cmpuint(axis_login_throttle_get_size(throttle), ==, 1);
	g_assert_cmpint(axis_login_throttle_check(throttle, "after", now), ==, 0);
}

/* What a per-address count counts an address by: itself, or its /64; and
 * which addresses are beside the daemon. */
static void
test_proxy_address_key(void)
{
	static const struct
	{
		const gchar *address;
		gboolean     is_private;
		gboolean     is_loopback;
	} cases[] = {
		{ "127.0.0.1", TRUE, TRUE }, { "127.8.9.10", TRUE, TRUE }, { "::1", TRUE, TRUE },
		{ "10.1.2.3", TRUE, FALSE }, { "172.16.0.1", TRUE, FALSE }, { "172.31.255.255", TRUE, FALSE },
		{ "172.32.0.1", FALSE, FALSE }, { "192.168.4.5", TRUE, FALSE }, { "169.254.1.1", TRUE, FALSE },
		{ "fd12:3456::1", TRUE, FALSE }, { "fe80::1", TRUE, FALSE }, { "203.0.113.7", FALSE, FALSE },
		{ "2001:db8::1", FALSE, FALSE }, { "::ffff:192.168.1.1", TRUE, FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisIpAddress address;

		g_assert_true(axis_ip_address_parse(cases[i].address, &address));
		if (axis_ip_address_is_private(&address) != cases[i].is_private ||
		    axis_ip_address_is_loopback(&address) != cases[i].is_loopback)
			g_error("%s: private %d, loopback %d", cases[i].address, axis_ip_address_is_private(&address),
				axis_ip_address_is_loopback(&address));
	}

	{
		g_autofree gchar *v4 = axis_proxy_address_key("203.0.113.7");
		g_autofree gchar *a = axis_proxy_address_key("2001:db8:1:2:aaaa::1");
		g_autofree gchar *b = axis_proxy_address_key("2001:db8:1:2:bbbb:cccc:dddd:eeee");
		g_autofree gchar *other = axis_proxy_address_key("2001:db8:1:3::1");
		g_autofree gchar *mapped = axis_proxy_address_key("::ffff:203.0.113.7");
		g_autofree gchar *odd = axis_proxy_address_key("unknown");

		g_assert_cmpstr(v4, ==, "203.0.113.7");
		g_assert_cmpstr(a, ==, "2001:db8:1:2::/64");
		g_assert_cmpstr(a, ==, b);
		g_assert_cmpstr(other, !=, a);
		g_assert_cmpstr(mapped, ==, "203.0.113.7");
		g_assert_cmpstr(odd, ==, "unknown");
	}
}

/* ── Password hashing ───────────────────────────────────────────── */

static void
test_password_hash(void)
{
	const gchar *password = "correct horse battery";
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = axis_password_hash(password, &error);
	g_autofree gchar *again = axis_password_hash(password, NULL);

	g_assert_no_error(error);
	g_assert_nonnull(hash);
	g_assert_true(g_str_has_prefix(hash, "$argon2id$"));
	g_assert_null(strstr(hash, password));
	g_assert_null(strstr(hash, "horse"));
	/* Salted: the same password never hashes the same. */
	g_assert_cmpstr(hash, !=, again);

	g_assert_true(axis_password_verify(hash, password));
	g_assert_true(axis_password_verify(again, password));
	g_assert_false(axis_password_verify(hash, "correct horse battery "));
	g_assert_false(axis_password_verify(hash, ""));
	g_assert_false(axis_password_verify(NULL, password));
	g_assert_false(axis_password_verify("", password));
	g_assert_false(axis_password_verify("$2y$10$notargon", password));
	g_assert_false(axis_password_needs_rehash(hash));
}

static void
test_password_rehash(void)
{
	/* A hash made with other limits than the ones in use (as one made
	 * before a change of limits would be) still verifies, and needs
	 * rehashing. */
	gchar stored[crypto_pwhash_argon2id_STRBYTES];

	g_assert_cmpint(crypto_pwhash_argon2id_str(stored, "older password", 14,
		crypto_pwhash_argon2id_OPSLIMIT_MIN + 1, crypto_pwhash_argon2id_MEMLIMIT_MIN * 2), ==, 0);
	g_assert_true(axis_password_verify(stored, "older password"));
	g_assert_true(axis_password_needs_rehash(stored));
	g_assert_true(axis_password_needs_rehash(NULL));
	g_assert_true(axis_password_needs_rehash("garbage"));
}

static void
test_secret_free(void)
{
	gchar *secret = g_strdup("s3cret-value");

	axis_secret_free(secret);
	axis_secret_free(NULL);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_assert_true(axis_password_init());
	axis_password_use_test_cost();

	g_test_add_func("/auth/proxy/list-parse", test_proxy_list_parse);
	g_test_add_func("/auth/proxy/list-limits", test_proxy_list_limits);
	g_test_add_func("/auth/proxy/contains", test_proxy_contains);
	g_test_add_func("/auth/proxy/lists-overlap", test_proxy_lists_overlap);
	g_test_add_func("/auth/proxy/address-key", test_proxy_address_key);
	g_test_add_func("/auth/proxy/resolve", test_proxy_resolve);
	g_test_add_func("/auth/proxy/resolve-hop-limit", test_proxy_resolve_hop_limit);
	g_test_add_func("/auth/proxy/host-valid", test_proxy_host_valid);
	g_test_add_func("/auth/proxy/resolve-full", test_proxy_resolve_full);
	g_test_add_func("/auth/cookie/find", test_cookie_find);
	g_test_add_func("/auth/next/path", test_next_path);
	g_test_add_func("/auth/username", test_username);
	g_test_add_func("/auth/tokens", test_token_shape);
	g_test_add_func("/auth/password/rules", test_password_rules);
	g_test_add_func("/auth/origin", test_origin);
	g_test_add_func("/auth/user-agent", test_user_agent);
	g_test_add_func("/auth/throttle/delays", test_throttle_delays);
	g_test_add_func("/auth/throttle/forgets", test_throttle_forgets);
	g_test_add_func("/auth/throttle/bounded", test_throttle_bounded);
	g_test_add_func("/auth/throttle/flood-keeps-delay", test_throttle_flood_keeps_delay);
	g_test_add_func("/auth/throttle/shared-when-full", test_throttle_shared_when_full);
	g_test_add_func("/auth/password/hash", test_password_hash);
	g_test_add_func("/auth/password/rehash", test_password_rehash);
	g_test_add_func("/auth/password/secret-free", test_secret_free);

	return g_test_run();
}
