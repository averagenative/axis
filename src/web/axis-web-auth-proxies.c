/*
 * axis-web-auth-proxies.c - the Reverse proxies page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Which reverse proxies Axis believes (server.trusted_proxies,
 * AXIS_TRUSTED_PROXIES), and for each host name in the list the addresses
 * it stands for now, when they were looked up, or why they were not.  The
 * list is a bootstrap setting: the page shows it and says how to change it,
 * and changes nothing.
 */

#include "web/axis-web-auth-private.h"

#include <string.h>

#define PROXIES_PATH "/admin/proxies"

static HtmxNode *
badge(
	const gchar *text,
	const gchar *variant
){
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "badge");
	htmx_element_add_class(HTMX_ELEMENT(span), variant);
	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

static HtmxNode *
paragraph(const gchar *text)
{
	HtmxP *p = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(p), text);
	return HTMX_NODE(p);
}

/* "12 seconds ago", "3 minutes ago", "2 hours ago". */
static gchar *
ago(gint64 us)
{
	gint64 seconds = us / G_USEC_PER_SEC;

	if (us < 0)
		return g_strdup("never");
	if (seconds < 1)
		return g_strdup("just now");
	if (seconds < 120)
		return g_strdup_printf("%" G_GINT64_FORMAT " seconds ago", seconds);
	if (seconds < 2 * 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " minutes ago", seconds / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " hours ago", seconds / 3600);
}

static const gchar *
state_label(
	const AxisProxyNameInfo *info,
	const gchar            **variant
){
	if (!info->is_name) {
		*variant = "badge-note";
		return "Fixed address";
	}
	switch (info->state) {
	case AXIS_PROXY_NAME_RESOLVED:
		*variant = "badge-succeeded";
		return "Resolved";
	case AXIS_PROXY_NAME_KEPT:
		*variant = "badge-warn";
		return "Lookup failing";
	case AXIS_PROXY_NAME_FAILED:
		*variant = "badge-failed";
		return "Not resolved";
	case AXIS_PROXY_NAME_PENDING:
	default:
		*variant = "badge-note";
		return "Looking up";
	}
}

/* What the entry trusts now, in words. */
static gchar *
trusted_text(const AxisProxyNameInfo *info)
{
	g_autofree gchar *list = g_strjoinv(", ", info->addresses);
	const gchar *why = info->error != NULL ? info->error : "no answer yet";

	if (!info->is_name)
		return g_strdup(list);

	switch (info->state) {
	case AXIS_PROXY_NAME_RESOLVED:
		return info->capped ?
			g_strdup_printf("%s (the name has more addresses; only the first %d are trusted)", list,
				AXIS_PROXY_NAME_MAX_ADDRESSES) : g_strdup(list);
	case AXIS_PROXY_NAME_KEPT:
		return g_strdup_printf("%s (kept for %d more seconds; the last lookup failed: %s)", list,
			(gint)(info->grace_left_us / G_USEC_PER_SEC) + 1, why);
	case AXIS_PROXY_NAME_FAILED:
		return g_strdup_printf("None. The name does not resolve (%s), so no address is trusted for it.", why);
	case AXIS_PROXY_NAME_PENDING:
	default:
		return g_strdup("None yet. The first lookup has not finished, so no address is trusted for it.");
	}
}

static HtmxNode *
entries_card(AxisWebAuth *auth)
{
	static const AxisWebColumn columns[] = {
		{ "Proxy", AXIS_WEB_COL_NAME },
		{ "Type", AXIS_WEB_COL_TEXT },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Trusted addresses", AXIS_WEB_COL_WIDE },
		{ "Last looked up", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	HtmxSection *section = htmx_section_new();
	HtmxHeading *heading = htmx_h2_new();
	g_autoptr(GPtrArray) entries = axis_proxy_list_describe_entries(auth->trusted);
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_node_set_text_content(HTMX_NODE(heading), "Trusted proxies");
	axis_web_auth_add(section, heading);

	if (entries->len == 0) {
		axis_web_auth_add(section, paragraph("None. Axis believes no X-Forwarded-For, X-Forwarded-Proto or "
			"X-Forwarded-Host header, so every request is taken to come from the address that connected."));
		axis_web_auth_add(section, paragraph("Behind a reverse proxy, set AXIS_TRUSTED_PROXIES (trusted_proxies "
			"in the configuration file) to the proxy's container name, such as traefik, or to its address, and "
			"restart Axis. The deployment guide, deploy/README.md, describes this under \"Behind a proxy\"."));
		return HTMX_NODE(section);
	}

	table = axis_web_table_new("trusted-proxies", columns);
	body = htmx_tbody_new();
	for (i = 0; i < entries->len; i++) {
		const AxisProxyNameInfo *info = g_ptr_array_index(entries, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *state = htmx_td_new();
		const gchar *variant = "badge-note";
		const gchar *label = state_label(info, &variant);
		g_autofree gchar *addresses = trusted_text(info);
		g_autofree gchar *when = info->is_name ? ago(info->age_us) : g_strdup("not looked up");

		axis_web_auth_add(row, htmx_td_new_with_text(info->name));
		axis_web_auth_add(row, htmx_td_new_with_text(info->is_name ? "Host name" :
			strchr(info->name, '/') != NULL ? "Range" : "Address"));
		axis_web_auth_add(state, badge(label, variant));
		axis_web_auth_add(row, state);
		axis_web_auth_add(row, htmx_td_new_with_text(addresses));
		axis_web_auth_add(row, htmx_td_new_with_text(when));
		axis_web_auth_add(body, row);
	}
	axis_web_auth_add(table, body);
	axis_web_auth_add(section, table);
	axis_web_auth_add(section, paragraph("Axis believes the forwarded headers only on a request that comes "
		"straight from one of the addresses above. A host name is looked up when Axis starts, every 30 seconds "
		"and when a request comes from an address that is not trusted, so a proxy that is recreated at a new "
		"address is believed again within seconds. The page shows the state when it was opened."));
	return HTMX_NODE(section);
}

static HtmxResponse *
handle_proxies(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	g_autofree gchar *hint = axis_web_auth_forwarding_hint(auth);

	htmx_node_set_text_content(HTMX_NODE(title), "Reverse proxies");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("The reverse proxies Axis trusts to pass on each visitor's "
		"real address and whether the visit used HTTPS. Set them in AXIS_TRUSTED_PROXIES (trusted_proxies in the "
		"configuration file) as addresses, ranges or host names, and restart Axis."));
	if (hint != NULL)
		axis_web_auth_add(content, axis_web_auth_notice(TRUE, hint));
	axis_web_auth_add(content, entries_card(auth));

	return axis_web_page_respond(request, "Reverse proxies", HTMX_NODE(content));
}

void
axis_web_auth_proxies_register(
	AxisWebAuth *self,
	AxisWeb     *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, PROXIES_PATH, AXIS_WEB_ROUTE_ADMIN, handle_proxies, self);
}
