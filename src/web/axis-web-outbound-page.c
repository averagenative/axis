/*
 * axis-web-outbound-page.c - the Outbound network page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What outbound.bind (AXIS_OUTBOUND_BIND, core/axis-outbound.h) ties Axis's
 * connections to, the interface's state read when the page is opened, and
 * which kinds of traffic the binding covers and which it does not, so that
 * nobody takes it to cover more than it does.  The setting is a bootstrap
 * one (the environment or the configuration file): the page shows it and
 * says how to change it, and changes nothing.
 */

#include "web/axis-web-settings-private.h"
#include "core/axis-outbound.h"

#define ADD axis_web_settings_add

#define OUTBOUND_PATH "/admin/network"

static HtmxNode *
badge(
	const gchar *text,
	const gchar *variant
){
	HtmxSpan *span = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(span), "badge");
	if (variant != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), variant);
	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

static void
fact(
	HtmxDl      *facts,
	const gchar *term,
	HtmxNode    *value
){
	HtmxDd *dd = htmx_dd_new();

	ADD(facts, htmx_dt_new_with_text(term));
	ADD(dd, value);
	ADD(facts, dd);
}

static HtmxNode *
text_node(const gchar *text)
{
	HtmxSpan *span = htmx_span_new();

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

static const gchar *
condition_label(AxisOutboundCondition condition)
{
	switch (condition) {
	case AXIS_OUTBOUND_STATE_UP:
		return "Up";
	case AXIS_OUTBOUND_STATE_MISSING:
		return "Missing";
	case AXIS_OUTBOUND_STATE_DOWN:
		return "Down";
	case AXIS_OUTBOUND_STATE_NO_ADDRESS:
		return "No address";
	case AXIS_OUTBOUND_STATE_OFF:
	default:
		return "Off";
	}
}

/* The binding as set, and the interface as it is now. */
static HtmxNode *
binding_card(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope,
	const AxisOutboundState  *state
){
	HtmxNode *card = axis_web_settings_card("Interface Axis is bound to");
	HtmxDl *facts;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *addresses = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *named = NULL;
	HtmxSpan *state_value;

	if (target->kind == AXIS_OUTBOUND_OFF) {
		ADD(card, paragraph("Not set: Axis's connections follow the system's routes, whatever they are."));
		ADD(card, paragraph("To send them through a VPN, either run Axis in the VPN container's network, so that "
			"everything it sends, name lookups included, uses the VPN; or set AXIS_OUTBOUND_BIND (outbound.bind "
			"in the configuration file) to the VPN's interface, such as tun0 or wg0, and restart Axis. The "
			"deployment guide, deploy/README.md, describes both under \"Sending Axis's traffic through a VPN\"."));
		return card;
	}

	facts = htmx_dl_new();
	htmx_element_add_class(HTMX_ELEMENT(facts), "facts");
	named = target->kind == AXIS_OUTBOUND_ADDRESS ?
		g_strdup_printf("%s (the interface that holds this address)", target->text) :
		g_strdup_printf("%s (an interface)", target->text);
	fact(facts, "Bound to", text_node(named));
	fact(facts, "Connections covered", text_node(scope == AXIS_OUTBOUND_SCOPE_ALL ?
		"Every connection except to this machine: indexers, TMDb, download clients and media servers "
		"(AXIS_OUTBOUND_BIND_SCOPE=all)" :
		"Connections to public addresses only: indexers and TMDb (AXIS_OUTBOUND_BIND_SCOPE=internet)"));

	state_value = htmx_span_new();
	ADD(state_value, badge(condition_label(state->condition),
		state->condition == AXIS_OUTBOUND_STATE_UP ? "badge-succeeded" : "badge-failed"));
	if (state->condition == AXIS_OUTBOUND_STATE_UP) {
		g_autofree gchar *up = g_strdup_printf(" %s is up.", state->device);

		ADD(state_value, text_node(up));
	} else if ((problem = axis_outbound_describe_problem(target, state)) != NULL) {
		g_autofree gchar *sentence = g_strdup_printf(" %c%s.", g_ascii_toupper(problem[0]), problem + 1);

		ADD(state_value, text_node(sentence));
	}
	fact(facts, "State now", HTMX_NODE(state_value));
	addresses = axis_outbound_describe_addresses(state);
	fact(facts, "Addresses used", text_node(addresses));
	if (axis_outbound_device_binding_check(&refused))
		fact(facts, "System allows binding", text_node("Allowed by the system."));
	else {
		HtmxSpan *value = htmx_span_new();
		g_autofree gchar *sentence = g_strdup_printf(" %c%s. Every connection that must be bound is refused "
			"until this is fixed.", g_ascii_toupper(refused[0]), refused + 1);

		ADD(value, badge("Refused", "badge-failed"));
		ADD(value, text_node(sentence));
		fact(facts, "System allows binding", HTMX_NODE(value));
	}
	ADD(card, facts);

	if (state->condition != AXIS_OUTBOUND_STATE_UP)
		axis_web_auth_add(card, axis_web_auth_notice(TRUE, "Until the interface is up, every connection the "
			"binding covers is refused at once, saying so, and nothing is sent another way. The rest of Axis "
			"keeps working."));
	ADD(card, paragraph("Read when this page was opened. The log records each change, within ten seconds, and "
		"not every request."));
	return card;
}

typedef enum
{
	BOUND,
	NOT_BOUND,
	NEVER
} Coverage;

static void
traffic_row(
	HtmxTbody   *body,
	const gchar *traffic,
	Coverage     coverage,
	const gchar *how
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *state = htmx_td_new();

	ADD(row, htmx_td_new_with_text(traffic));
	ADD(state, badge(coverage == BOUND ? "Bound" : coverage == NOT_BOUND ? "Not bound" : "Never bound",
		coverage == BOUND ? "badge-succeeded" : coverage == NOT_BOUND ? "badge-note" : "badge-warn"));
	ADD(row, state);
	ADD(row, htmx_td_new_with_text(how));
	ADD(body, row);
}

/* Every kind of traffic Axis has, and whether the binding covers it. */
static HtmxNode *
coverage_card(
	const AxisOutboundTarget *target,
	AxisOutboundScope         scope
){
	static const AxisWebColumn columns[] = {
		{ "Traffic", AXIS_WEB_COL_NAME },
		{ "Bound now", AXIS_WEB_COL_STATE },
		{ "Route", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_settings_card("Which connections use the interface");
	HtmxNode *table = axis_web_table_new("outbound-traffic", columns);
	HtmxTbody *body = htmx_tbody_new();
	gboolean on = target->kind != AXIS_OUTBOUND_OFF;
	gboolean all = on && scope == AXIS_OUTBOUND_SCOPE_ALL;

	traffic_row(body, "Indexer searches and release files", on ? BOUND : NOT_BOUND,
		!on ? "The system's routes." :
		all ? "To every address but this machine's." :
		"To public addresses. An indexer on a local network, such as a Prowlarr beside Axis, is reached "
		"directly: it makes its own connections to the indexers, and must be sent through the VPN itself.");
	traffic_row(body, "An indexer's own site, after a redirect", on ? BOUND : NOT_BOUND,
		!on ? "The system's routes." :
		"As the indexer's searches: such a redirect is only followed to a public address.");
	traffic_row(body, "Details and artwork (TMDb)", on ? BOUND : NOT_BOUND,
		!on ? "The system's routes." :
		all ? "To every address but this machine's." : "To public addresses.");
	traffic_row(body, "Download clients", all ? BOUND : NOT_BOUND,
		all ? "To every address but this machine's." :
		"They are usually on the same network as Axis. Set AXIS_OUTBOUND_BIND_SCOPE=all to bind them too.");
	traffic_row(body, "Media servers (Plex, Jellyfin)", all ? BOUND : NOT_BOUND,
		all ? "To every address but this machine's." :
		"They are usually on the same network as Axis. Set AXIS_OUTBOUND_BIND_SCOPE=all to bind them too.");
	traffic_row(body, "Radarr and Sonarr (Migrate)", all ? BOUND : NOT_BOUND,
		all ? "To every address but this machine's." :
		"They are usually on the same network as Axis. Set AXIS_OUTBOUND_BIND_SCOPE=all to bind them too.");
	traffic_row(body, "Anything on this machine", NEVER,
		"A connection to a loopback address (127.0.0.1, ::1) does not leave the machine.");
	traffic_row(body, "The database", NEVER, "PostgreSQL is always reached by the system's routes.");
	traffic_row(body, "Name lookups (DNS)", NEVER,
		"The system's resolver makes them, through the DNS server the host or the container is given, so the "
		"names Axis looks up can be seen there. To send them through the VPN too, run Axis in the VPN "
		"container's network.");
	traffic_row(body, "Remote workers", NEVER, "Workers connect to Axis; Axis makes no connection to them.");
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
guarantee_card(void)
{
	HtmxNode *card = axis_web_settings_card("What the binding guarantees");

	ADD(card, paragraph("Each bound connection is tied to the interface itself and to its address as it is made, "
		"from the interface's state at that moment. The system then sends it out of that interface only, "
		"whatever its routing table says; if the interface goes down or away during the connection, its packets "
		"are not sent at all."));
	ADD(card, paragraph("When the interface is missing, down or has no address, a connection that must be bound "
		"is not made: the request fails at once with the reason (\"the outbound interface tun0 is down\"), and "
		"nothing is sent another way. When the interface has an address of one kind only (IPv4, say), a server's "
		"addresses of the other kind are not used."));
	ADD(card, paragraph("It does not cover name lookups, the database or anything outside Axis itself, such as "
		"the download clients' own traffic: put those behind the VPN where they run."));
	return card;
}

static HtmxResponse *
handle_outbound(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	AxisOutboundTarget target;
	AxisOutboundScope scope;
	AxisOutboundState state;

	axis_outbound_get_config(&target, &scope);
	axis_outbound_read(&state);

	htmx_node_set_text_content(HTMX_NODE(title), "Outbound network and VPN");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("The network interface Axis's own connections leave by. Set it "
		"to a VPN's interface, and a connection that must use the VPN is refused while the VPN is down instead of "
		"going out another way."));
	axis_web_auth_add(content, binding_card(&target, scope, &state));
	axis_web_auth_add(content, coverage_card(&target, scope));
	axis_web_auth_add(content, guarantee_card());

	return axis_web_page_respond(request, "Outbound network and VPN", HTMX_NODE(content));
}

void
axis_web_outbound_register_page(
	AxisWebSettings *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, OUTBOUND_PATH, AXIS_WEB_ROUTE_ADMIN, handle_outbound, self);
}
