/*
 * test-proxy-names.c - host names in server.trusted_proxies: how they are
 * parsed, looked up, kept and dropped
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No test uses the system's resolver: a fake GResolver answers from a table
 * (axis-test-resolver.h) and a fake clock ages the answers, so nothing
 * here waits for a real second.  Needs no database.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-resolver.h"
#include "core/axis-proxy.h"
#include "core/axis-proxy-resolver.h"

#define SECOND G_GINT64_CONSTANT(1000000)

static gint64 fake_now;

static gint64
clock_now(gpointer data)
{
	return fake_now;
}

typedef struct
{
	GResolver     *dns;
	GResolver     *system;	/* what was the default before */
	AxisProxyList *list;
} Fixture;

static void
fixture_setup_with(
	Fixture     *fx,
	const gchar *text
){
	g_autoptr(GError) error = NULL;

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "trusted proxy name*");
	fake_now = 100 * SECOND;
	fx->system = g_resolver_get_default();
	fx->dns = axis_test_resolver_new();
	g_resolver_set_default(fx->dns);
	fx->list = axis_proxy_list_parse(text, &error);
	g_assert_no_error(error);
	axis_proxy_list_set_clock(fx->list, clock_now, NULL);
}

static void
fixture_teardown(
	Fixture      *fx,
	gconstpointer data
){
	axis_proxy_list_free(fx->list);
	axis_test_resolver_drain();
	g_resolver_set_default(fx->system);	/* the fake is let go of with the default's change */
	g_object_unref(fx->system);
	g_object_unref(fx->dns);
}

static void
setup_traefik(
	Fixture      *fx,
	gconstpointer data
){
	fixture_setup_with(fx, "10.0.0.0/8, Traefik, 2001:db8::/32");
}

static gboolean
trusts(
	AxisProxyList *list,
	const gchar   *text
){
	AxisIpAddress address;

	g_assert_true(axis_ip_address_parse(text, &address));
	return axis_proxy_list_contains(list, &address);
}

static AxisProxyNameInfo *
info_of(
	AxisProxyList *list,
	const gchar   *name
){
	g_autoptr(GPtrArray) all = axis_proxy_list_describe_entries(list);
	guint i;

	for (i = 0; i < all->len; i++) {
		AxisProxyNameInfo *info = g_ptr_array_index(all, i);

		if (strcmp(info->name, name) == 0) {
			g_ptr_array_index(all, i) = NULL;
			return info;
		}
	}
	g_error("no entry %s", name);
	return NULL;
}

/* ── Parsing ────────────────────────────────────────────────────── */

static void
test_name_validity(void)
{
	static const gchar *const good[] = {
		"traefik", "Traefik", "proxy.example.net", "my_proxy", "a-b.c-d", "x", "a1", "1a", "web-2.internal",
		"proxy.123abc", NULL
	};
	static const gchar *const bad[] = {
		"", "-a", "a-", "a..b", ".a", "a.", "a b", "a,b", "a:80", "a/24", "a_b!", "10.0.0.256", "300.1.1.1",
		"10.0.0", "1234", "a.-b", "a-.b", "caf\xc3\xa9", "[::1]", "a@b", NULL
	};
	g_autofree gchar *long_label = g_strnfill(64, 'a');
	g_autofree gchar *ok_label = g_strnfill(63, 'a');
	g_autofree gchar *long_name = NULL;
	guint i;

	for (i = 0; good[i] != NULL; i++) {
		if (!axis_proxy_name_is_valid(good[i]))
			g_error("'%s' was refused as a name", good[i]);
	}
	for (i = 0; bad[i] != NULL; i++) {
		if (axis_proxy_name_is_valid(bad[i]))
			g_error("'%s' was accepted as a name", bad[i]);
	}
	g_assert_false(axis_proxy_name_is_valid(NULL));
	g_assert_true(axis_proxy_name_is_valid(ok_label));
	g_assert_false(axis_proxy_name_is_valid(long_label));

	/* Four labels of 63 characters and three dots make 255. */
	long_name = g_strdup_printf("%s.%s.%s.%s", ok_label, ok_label, ok_label, ok_label);
	g_assert_false(axis_proxy_name_is_valid(long_name));
	long_name[AXIS_PROXY_NAME_MAX_LEN] = '\0';
	g_assert_true(axis_proxy_name_is_valid(long_name));	/* 253 characters: the limit */
}

static void
test_list_mixed(void)
{
	g_autoptr(AxisProxyList) list = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) names = NULL;

	list = axis_proxy_list_parse(" 10.0.0.0/8 , Traefik,2001:DB8::1 , proxy.Example.net, traefik", &error);
	g_assert_no_error(error);
	text = axis_proxy_list_to_string(list);
	g_assert_cmpstr(text, ==, "10.0.0.0/8, traefik, 2001:db8::1, proxy.example.net, traefik");
	g_assert_cmpuint(axis_proxy_list_get_length(list), ==, 5);
	g_assert_true(axis_proxy_list_has_names(list));
	names = axis_proxy_list_get_names(list);
	g_assert_cmpuint(g_strv_length(names), ==, 2);
	g_assert_cmpstr(names[0], ==, "traefik");
	g_assert_cmpstr(names[1], ==, "proxy.example.net");

	/* Only addresses and ranges: no names, nothing to look up. */
	g_clear_pointer(&list, axis_proxy_list_free);
	list = axis_proxy_list_parse("10.0.0.1, 192.168.0.0/16", &error);
	g_assert_no_error(error);
	g_assert_false(axis_proxy_list_has_names(list));
}

static void
test_list_malformed_names(void)
{
	static const gchar *const bad[] = {
		"traefik,-bad", "tra efik", "proxy..example", "proxy.example.", "traefik:8080", "traefik/24",
		"10.0.0.1, 300.1.1.1", "pro$y", "a,b,c!", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisProxyList) list = axis_proxy_list_parse(bad[i], &error);

		if (list != NULL)
			g_error("'%s' was accepted", bad[i]);
		g_assert_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID);
		g_assert_nonnull(strstr(error->message, "host name"));
	}
}

static void
test_list_name_counts_toward_limit(void)
{
	GString *text = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	AxisProxyList *list;
	guint i;

	for (i = 0; i < AXIS_PROXY_LIST_MAX_ENTRIES; i++)
		g_string_append_printf(text, "%sproxy%u", i > 0 ? "," : "", i);
	list = axis_proxy_list_parse(text->str, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(axis_proxy_list_get_length(list), ==, AXIS_PROXY_LIST_MAX_ENTRIES);
	axis_proxy_list_free(list);
	g_string_append(text, ",one-more");
	list = axis_proxy_list_parse(text->str, &error);
	g_assert_null(list);
	g_assert_nonnull(strstr(error->message, "more than"));
	g_string_free(text, TRUE);
}

/* ── Resolution ─────────────────────────────────────────────────── */

/* A name trusts nothing until its first lookup has finished, whatever
 * addresses the name will have; the others are in force at once. */
static void
test_pending_trusts_nothing(Fixture *fx, gconstpointer data)
{
	AxisProxyNameInfo *info = info_of(fx->list, "traefik");

	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_PENDING);
	g_assert_cmpint(info->age_us, <, 0);
	g_assert_null(info->addresses[0]);
	axis_proxy_name_info_free(info);
	g_assert_true(trusts(fx->list, "10.1.1.1"));
	g_assert_false(trusts(fx->list, "172.20.0.2"));
}

static void
test_resolves_ipv4_and_ipv6(Fixture *fx, gconstpointer data)
{
	static const gchar *const answer[] = { "172.20.0.2", "fd00:1::2", "::ffff:172.20.0.3", "0.0.0.0", "::", NULL };
	g_autoptr(AxisProxyResolver) resolver = NULL;
	AxisProxyNameInfo *info;

	axis_test_resolver_set(fx->dns, "traefik", answer);
	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_false(axis_proxy_resolver_is_busy(resolver));

	g_assert_true(trusts(fx->list, "172.20.0.2"));
	g_assert_true(trusts(fx->list, "172.20.0.3"));		/* the mapped form is the IPv4 address */
	g_assert_true(trusts(fx->list, "fd00:1::2"));
	g_assert_false(trusts(fx->list, "172.20.0.4"));
	g_assert_false(trusts(fx->list, "fd00:1::3"));
	g_assert_false(trusts(fx->list, "0.0.0.0"));		/* an unspecified address names nobody */

	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_RESOLVED);
	g_assert_cmpuint(g_strv_length(info->addresses), ==, 3);
	g_assert_cmpstr(info->addresses[0], ==, "172.20.0.2");	/* IPv4 first, in address order */
	g_assert_cmpstr(info->addresses[1], ==, "172.20.0.3");
	g_assert_cmpstr(info->addresses[2], ==, "fd00:1::2");
	g_assert_cmpint(info->age_us, ==, 0);
	g_assert_false(info->capped);
	axis_proxy_name_info_free(info);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "trusted proxy name traefik resolves to 172.20.0.2, "
		"172.20.0.3, fd00:1::2"), ==, 1);

	/* The same answer again says nothing more. */
	fake_now += 40 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_resolver_count(fx->dns, "traefik"), ==, 2);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "trusted proxy name traefik*"), ==, 1);
	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->age_us, ==, 0);
	axis_proxy_name_info_free(info);
}

/* A recreated proxy comes from a new address: the request from it, which is
 * not trusted yet, asks for a new lookup; the next request is believed. */
static void
test_new_address_after_request(Fixture *fx, gconstpointer data)
{
	static const gchar *const before[] = { "172.20.0.2", NULL };
	static const gchar *const after[] = { "172.20.0.9", NULL };
	g_autoptr(AxisProxyResolver) resolver = NULL;

	axis_test_resolver_set(fx->dns, "traefik", before);
	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));
	g_assert_false(trusts(fx->list, "172.20.0.9"));

	/* The proxy is recreated; the first request from the new address. */
	axis_test_resolver_set(fx->dns, "traefik", after);
	fake_now += 2 * SECOND;
	axis_proxy_resolver_poke(resolver);	/* too soon after the last round: ignored */
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_resolver_count(fx->dns, "traefik"), ==, 1);
	g_assert_false(trusts(fx->list, "172.20.0.9"));

	fake_now += AXIS_PROXY_RESOLVE_MIN_INTERVAL_US;
	g_assert_false(trusts(fx->list, "172.20.0.9"));	/* still untrusted when the request arrives */
	axis_proxy_resolver_poke(resolver);
	axis_proxy_resolver_poke(resolver);	/* a burst asks once */
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_resolver_count(fx->dns, "traefik"), ==, 2);
	g_assert_true(trusts(fx->list, "172.20.0.9"));
	g_assert_false(trusts(fx->list, "172.20.0.2"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "trusted proxy name traefik resolves to 172.20.0.9"),
		==, 1);
}

/* A lookup that fails keeps the earlier addresses for the grace and then
 * drops them; each change is logged once. */
static void
test_failure_grace_then_drop(Fixture *fx, gconstpointer data)
{
	static const gchar *const good[] = { "172.20.0.2", NULL };
	g_autoptr(AxisProxyResolver) resolver = NULL;
	AxisProxyNameInfo *info;

	axis_test_resolver_set(fx->dns, "traefik", good);
	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));

	/* The name stops resolving. */
	axis_test_resolver_set(fx->dns, "traefik", NULL);
	fake_now += 30 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));	/* kept: a blip must not log everyone out */
	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_KEPT);
	g_assert_nonnull(strstr(info->error, "no such host"));
	g_assert_cmpuint(g_strv_length(info->addresses), ==, 1);
	g_assert_cmpint(info->grace_left_us, ==, AXIS_PROXY_NAME_GRACE_US);
	axis_proxy_name_info_free(info);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "trusted proxy name traefik does not resolve*"
		"earlier addresses are kept for 30 seconds"), ==, 1);

	/* More failures inside the grace say nothing new. */
	fake_now += 10 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "trusted proxy name*"), ==, 1);

	/* The grace ends: nothing is trusted, whether or not a lookup has said so. */
	fake_now += 20 * SECOND - 1;
	g_assert_true(trusts(fx->list, "172.20.0.2"));
	fake_now += 1;
	g_assert_false(trusts(fx->list, "172.20.0.2"));
	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_FAILED);
	g_assert_null(info->addresses[0]);
	axis_proxy_name_info_free(info);

	/* The next failed lookup logs the drop, once. */
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "trusted proxy name traefik does not resolve*"
		"no address is trusted for it"), ==, 1);
	fake_now += 10 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "trusted proxy name*"), ==, 2);
	g_assert_false(trusts(fx->list, "172.20.0.2"));

	/* And when it resolves again, it is believed again and that is logged. */
	axis_test_resolver_set(fx->dns, "traefik", good);
	fake_now += 10 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "trusted proxy name traefik resolves to*"), ==, 2);
}

/* A name that never resolved trusts nothing and is said so once. */
static void
test_never_resolved_fails_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisProxyResolver) resolver = NULL;
	AxisProxyNameInfo *info;

	/* The table has no answer for it at all. */
	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_false(trusts(fx->list, "172.20.0.2"));
	g_assert_true(trusts(fx->list, "10.9.9.9"));	/* the fixed entries are unaffected */
	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_FAILED);
	g_assert_nonnull(info->error);
	g_assert_cmpint(info->age_us, <, 0);
	axis_proxy_name_info_free(info);
	fake_now += 40 * SECOND;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "trusted proxy name traefik does not resolve*"), ==, 1);
}

static void
test_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisProxyResolver) resolver = NULL;
	g_autoptr(GPtrArray) answer = g_ptr_array_new_with_free_func(g_free);
	AxisProxyNameInfo *info;
	guint i;

	/* Forty addresses, given in a scrambled order. */
	for (i = 0; i < 40; i++)
		g_ptr_array_add(answer, g_strdup_printf("172.20.1.%u", (i * 7) % 40 + 1));
	g_ptr_array_add(answer, NULL);
	axis_test_resolver_set(fx->dns, "traefik", (const gchar *const *)answer->pdata);
	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();

	info = info_of(fx->list, "traefik");
	g_assert_cmpint(info->state, ==, AXIS_PROXY_NAME_RESOLVED);
	g_assert_true(info->capped);
	g_assert_cmpuint(g_strv_length(info->addresses), ==, AXIS_PROXY_NAME_MAX_ADDRESSES);
	axis_proxy_name_info_free(info);
	g_assert_true(trusts(fx->list, "172.20.1.1"));
	g_assert_true(trusts(fx->list, "172.20.1.16"));
	g_assert_false(trusts(fx->list, "172.20.1.17"));
	g_assert_false(trusts(fx->list, "172.20.1.40"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "*only the first 16 are trusted*"), ==, 1);
}

/* Nothing that serves a request waits for a lookup, however slow. */
static void
test_never_waits(Fixture *fx, gconstpointer data)
{
	static const gchar *const good[] = { "172.20.0.2", NULL };
	AxisProxyResolver *resolver;
	gint64 started;

	axis_test_resolver_set(fx->dns, "traefik", good);
	axis_test_resolver_hold(fx->dns, TRUE);
	resolver = axis_proxy_resolver_new(fx->list);
	started = g_get_monotonic_time();
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_true(axis_proxy_resolver_is_busy(resolver));
	g_assert_cmpuint(axis_test_resolver_count(fx->dns, "traefik"), ==, 1);

	/* The lookup is under way and unanswered: a request is decided now. */
	g_assert_false(trusts(fx->list, "172.20.0.2"));
	g_assert_true(trusts(fx->list, "10.1.1.1"));
	fake_now += AXIS_PROXY_RESOLVE_MIN_INTERVAL_US;
	axis_proxy_resolver_poke(resolver);
	axis_test_resolver_drain();
	g_assert_cmpuint(axis_test_resolver_count(fx->dns, "traefik"), ==, 1);	/* no second one over the first */
	g_assert_cmpint(g_get_monotonic_time() - started, <, 2 * SECOND);

	/* Freed with the lookup unanswered; the late answer finds nobody. */
	axis_proxy_resolver_free(resolver);
	axis_test_resolver_release(fx->dns);
	axis_test_resolver_drain();
	g_assert_false(trusts(fx->list, "172.20.0.2"));
}

/* From another thread a poke is safe and quick too. */
static gpointer
poke_thread(gpointer data)
{
	axis_proxy_resolver_poke(data);
	return NULL;
}

static void
test_poke_from_thread(Fixture *fx, gconstpointer data)
{
	static const gchar *const good[] = { "172.20.0.2", NULL };
	g_autoptr(AxisProxyResolver) resolver = NULL;
	GThread *thread;

	axis_test_resolver_set(fx->dns, "traefik", good);
	resolver = axis_proxy_resolver_new(fx->list);
	thread = g_thread_new("poke", poke_thread, resolver);
	g_thread_join(thread);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx->list, "172.20.0.2"));
}

/* The same name twice, or two names for one proxy, both work. */
static void
test_two_names(void)
{
	static const gchar *const a[] = { "172.20.0.2", NULL };
	static const gchar *const b[] = { "172.20.0.3", NULL };
	Fixture fx;
	g_autoptr(AxisProxyResolver) resolver = NULL;

	fixture_setup_with(&fx, "traefik, edge.internal, traefik");
	axis_test_resolver_set(fx.dns, "traefik", a);
	axis_test_resolver_set(fx.dns, "edge.internal", b);
	resolver = axis_proxy_resolver_new(fx.list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	g_assert_true(trusts(fx.list, "172.20.0.2"));
	g_assert_true(trusts(fx.list, "172.20.0.3"));
	g_assert_cmpuint(axis_test_resolver_count(fx.dns, "traefik"), ==, 1);
	g_clear_pointer(&resolver, axis_proxy_resolver_free);
	fixture_teardown(&fx, NULL);
}

/* Through the forwarding headers: a request from a name's address is believed. */
static void
test_forwarded_headers_follow_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const good[] = { "172.20.0.2", NULL };
	g_autoptr(AxisProxyResolver) resolver = NULL;
	g_autofree gchar *client = NULL;
	gboolean https = FALSE;

	axis_test_resolver_set(fx->dns, "traefik", good);
	axis_proxy_resolve(fx->list, "172.20.0.2", FALSE, "203.0.113.9", "https", &client, &https);
	g_assert_cmpstr(client, ==, "172.20.0.2");	/* not looked up yet: not believed */
	g_assert_false(https);
	g_clear_pointer(&client, g_free);

	resolver = axis_proxy_resolver_new(fx->list);
	axis_proxy_resolver_start(resolver);
	axis_test_resolver_drain();
	axis_proxy_resolve(fx->list, "172.20.0.2", FALSE, "203.0.113.9", "https", &client, &https);
	g_assert_cmpstr(client, ==, "203.0.113.9");
	g_assert_true(https);
	g_clear_pointer(&client, g_free);
	/* A neighbour's address is not. */
	axis_proxy_resolve(fx->list, "172.20.0.3", FALSE, "203.0.113.9", "https", &client, &https);
	g_assert_cmpstr(client, ==, "172.20.0.3");
	g_assert_false(https);
}

/* Entries of every kind are described, in order. */
static void
test_describe_entries(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) all = axis_proxy_list_describe_entries(fx->list);
	AxisProxyNameInfo *info;

	g_assert_cmpuint(all->len, ==, 3);
	info = g_ptr_array_index(all, 0);
	g_assert_cmpstr(info->name, ==, "10.0.0.0/8");
	g_assert_false(info->is_name);
	g_assert_cmpstr(info->addresses[0], ==, "10.0.0.0/8");
	info = g_ptr_array_index(all, 1);
	g_assert_cmpstr(info->name, ==, "traefik");
	g_assert_true(info->is_name);
	info = g_ptr_array_index(all, 2);
	g_assert_cmpstr(info->name, ==, "2001:db8::/32");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/proxy-names/validity", test_name_validity);
	g_test_add_func("/proxy-names/list-mixed", test_list_mixed);
	g_test_add_func("/proxy-names/list-malformed", test_list_malformed_names);
	g_test_add_func("/proxy-names/list-limit", test_list_name_counts_toward_limit);
	g_test_add("/proxy-names/pending", Fixture, NULL, setup_traefik, test_pending_trusts_nothing,
		fixture_teardown);
	g_test_add("/proxy-names/resolves", Fixture, NULL, setup_traefik, test_resolves_ipv4_and_ipv6,
		fixture_teardown);
	g_test_add("/proxy-names/new-address", Fixture, NULL, setup_traefik, test_new_address_after_request,
		fixture_teardown);
	g_test_add("/proxy-names/failure-grace", Fixture, NULL, setup_traefik, test_failure_grace_then_drop,
		fixture_teardown);
	g_test_add("/proxy-names/never-resolved", Fixture, NULL, setup_traefik, test_never_resolved_fails_closed,
		fixture_teardown);
	g_test_add("/proxy-names/cap", Fixture, NULL, setup_traefik, test_cap, fixture_teardown);
	g_test_add("/proxy-names/never-waits", Fixture, NULL, setup_traefik, test_never_waits, fixture_teardown);
	g_test_add("/proxy-names/poke-thread", Fixture, NULL, setup_traefik, test_poke_from_thread,
		fixture_teardown);
	g_test_add_func("/proxy-names/two-names", test_two_names);
	g_test_add("/proxy-names/headers", Fixture, NULL, setup_traefik, test_forwarded_headers_follow_name,
		fixture_teardown);
	g_test_add("/proxy-names/describe", Fixture, NULL, setup_traefik, test_describe_entries,
		fixture_teardown);

	return g_test_run();
}
