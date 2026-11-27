/*
 * axis-web-portal.c - the page "Request portal": the portal's tokens and
 * its listener
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2.)  An administrator makes the token a request
 * portal authenticates with.  It is shown once, on the page that answers its
 * making, and stored only as a hash (api/axis-portal-tokens.h); no page shows
 * it again and no log line holds it.  Revoking takes effect at the portal's
 * next request.  Up to four tokens can be active, so that one is replaced
 * without an interruption: make a second, give it to the portal, revoke the
 * first.
 *
 * The page belongs to the Requests module (a module that is off has no
 * pages); every route here is for administrators.
 */

#include "requests/axis-requests-private.h"

#include <string.h>

#include "api/axis-portal-proto.h"
#include "api/axis-portal-tokens.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-portal.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"

#define PAGE_PATH  "/admin/portal"
#define PAGE_TITLE "Request portal"

/* ── Work off the main loop ─────────────────────────────────────── */

typedef struct
{
	gint64  id;
	gchar  *created;
	gchar  *created_by;
	gchar  *revoked;	/* NULL: active */
	gchar  *revoked_by;
	gchar  *last_used;
	gchar  *last_address;
} TokenRow;

static void
token_row_free(gpointer data)
{
	TokenRow *row = data;

	g_free(row->created);
	g_free(row->created_by);
	g_free(row->revoked);
	g_free(row->revoked_by);
	g_free(row->last_used);
	g_free(row->last_address);
	g_free(row);
}

typedef enum
{
	WORK_PAGE,
	WORK_CREATE,
	WORK_REVOKE
} WorkKind;

typedef struct
{
	AxisRequests *owner;
	WorkKind      kind;
	HtmxRequest  *request;
	HtmxResponse *deferred;
	gchar        *by;
	gint64        id;
	gchar        *done;	/* the page's message key */
	/* Out. */
	GPtrArray    *rows;	/* TokenRow */
	gint64        today;	/* what the portal's requests count today */
	gint64        ceiling;	/* of requests.portal_per_day */
	gchar        *token;	/* secret: shown once */
	gboolean      changed;
	GError       *error;
} Work;

static void
work_free(Work *work)
{
	g_object_unref(work->request);
	g_object_unref(work->deferred);
	g_free(work->by);
	g_free(work->done);
	g_clear_pointer(&work->rows, g_ptr_array_unref);
	axis_secret_free(work->token);
	g_clear_error(&work->error);
	g_free(work);
}

static gchar *
column_time(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 us = 0;

	if (axis_db_result_is_null(result, row, column) || !axis_db_result_get_int64(result, row, column, &us))
		return NULL;
	return axis_web_format_time(us, "-");
}

static void
load_rows(Work *work)
{
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_requests_portal_today(work->owner, &work->today, &work->ceiling, &work->error))
		return;
	if (!axis_db_pool_exec(work->owner->pool, "SELECT id, (extract(epoch FROM created_at) * 1000000)::bigint, created_by,"
	        " (extract(epoch FROM revoked_at) * 1000000)::bigint, revoked_by,"
	        " (extract(epoch FROM last_used_at) * 1000000)::bigint, last_address"
	        " FROM portal_token ORDER BY (revoked_at IS NOT NULL), id DESC LIMIT 50", 0, NULL, &result, &work->error))
		return;
	work->rows = g_ptr_array_new_with_free_func(token_row_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		TokenRow *row = g_new0(TokenRow, 1);

		axis_db_result_get_int64(result, i, 0, &row->id);
		row->created = column_time(result, i, 1);
		row->created_by = g_strdup(axis_db_result_get_text(result, i, 2));
		row->revoked = column_time(result, i, 3);
		row->revoked_by = g_strdup(axis_db_result_get_text(result, i, 4));
		row->last_used = column_time(result, i, 5);
		row->last_address = g_strdup(axis_db_result_get_text(result, i, 6));
		g_ptr_array_add(work->rows, row);
	}
}

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Work *work = task_data;
	AxisRequests *self = work->owner;

	switch (work->kind) {
	case WORK_CREATE:
		axis_portal_tokens_create(self->portal_tokens, self->pool, work->by, &work->token, &work->id, &work->error);
		break;
	case WORK_REVOKE:
		axis_portal_tokens_revoke_stored(self->portal_tokens, self->pool, work->id, work->by, &work->changed,
			&work->error);
		break;
	default:
		load_rows(work);
		break;
	}
	g_task_return_boolean(task, TRUE);
}

/* ── The page ───────────────────────────────────────────────────── */

static HtmxNode *
card(
	const gchar *heading,
	const gchar *id
){
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_element_set_id(HTMX_ELEMENT(section), id);
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	axis_web_auth_add(section, title);
	return HTMX_NODE(section);
}

static HtmxNode *
paragraph(const gchar *text)
{
	HtmxP *p = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(p), text);
	return HTMX_NODE(p);
}

static const gchar *
done_message(
	const gchar *key,
	gboolean    *is_error
){
	*is_error = FALSE;
	if (g_strcmp0(key, "revoked") == 0)
		return "The token has been revoked. A portal that still uses it is refused from its next request.";
	if (g_strcmp0(key, "already-revoked") == 0)
		return "The token was revoked already.";
	*is_error = TRUE;
	if (g_strcmp0(key, "full") == 0)
		return "Four tokens are active already. Revoke one first.";
	if (g_strcmp0(key, "no-token") == 0)
		return "There is no such token.";
	if (g_strcmp0(key, "no-api") == 0)
		return "This daemon has no portal API.";
	return NULL;
}

static HtmxResponse *
page_response(Work *work)
{
	AxisRequests *self = work->owner;
	HtmxRequest *request = work->request;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	static const AxisWebColumn columns[] = {
		{ "Made", AXIS_WEB_COL_DATE },
		{ "By", AXIS_WEB_COL_TEXT },
		{ "Last used", AXIS_WEB_COL_DATE },
		{ "From", AXIS_WEB_COL_TEXT },
		{ "State", AXIS_WEB_COL_STATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *listener = card("The portal's listener", "portal-listener");
	HtmxNode *tokens = card("Tokens", "portal-tokens");
	HtmxNode *limits = card("Limits", "portal-limits");
	HtmxNode *table = axis_web_table_new("portal-tokens", columns);
	HtmxTbody *body = htmx_tbody_new();
	const gchar *message;
	gboolean is_error = FALSE;
	guint active = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), PAGE_TITLE);
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("A request portal is a separate program that lets people who are "
		"not administrators ask for titles from outside your network. It talks to Axis through an API of its own, "
		"on a port of its own, with a token made here. The token opens that API and nothing else, and by itself it "
		"acts as no one: every request needs the session of a requester who logged in."));
	message = done_message(work->done, &is_error);
	if (message != NULL)
		axis_web_auth_add(content, axis_web_auth_notice(is_error, message));

	if (self->portal_port != 0) {
		g_autofree gchar *text = g_strdup_printf("On: the portal API is answered on port %u, and only there. That "
			"port answers nothing else: no page, no login form and no other API.", self->portal_port);

		axis_web_auth_add(listener, paragraph(text));
	} else {
		axis_web_auth_add(listener, paragraph("Off: Axis answers no portal API. To switch it on, set "
			"portal_listen_port in the [server] section of the configuration file, or AXIS_PORTAL_LISTEN_PORT in the "
			"environment, to a port of its own, and restart Axis. Allow only the portal to reach that port."));
	}
	axis_web_auth_add(listener, paragraph("The request portal (axis-portal) and this API have not been reviewed for "
		"exposure to the internet. Until the documents of Axis say that the review has passed, let the portal be "
		"reached only from a network you trust."));
	/* The portal's addresses: what they are, what they mean, and what an
	 * administrator who was refused at the login needs to find out why. */
	{
		g_autofree gchar *peers = axis_web_auth_portal_peers_text(self->auth);
		g_autofree gchar *peer = NULL;
		g_autofree gchar *proxy = NULL;
		gint64 last_us = 0;
		guint barred = axis_web_auth_portal_logins_barred(self->auth, &last_us);

		if (peers[0] == '\0' && self->portal_port != 0) {
			axis_web_auth_add(listener, axis_web_auth_notice(TRUE, "The portal's addresses are not set, so the portal "
				"API answers nobody: every request to it gets 503. Set portal_peers in the [server] section of the "
				"configuration file, or AXIS_PORTAL_PEERS in the environment, to the address the portal connects from "
				"and every other address the portal has, and restart Axis."));
		} else if (peers[0] == '\0') {
			axis_web_auth_add(listener, paragraph("The portal's addresses (portal_peers in the configuration file, "
				"AXIS_PORTAL_PEERS in the environment) are not set. The portal API answers only the addresses in that "
				"list."));
		} else {
			g_autofree gchar *text = g_strdup_printf("The portal's addresses (portal_peers in the configuration file, "
				"AXIS_PORTAL_PEERS in the environment): %s. The portal API answers these addresses and no other. Nobody "
				"logs in to Axis itself from one of them: such a login is refused as a wrong password would be, "
				"whether the connection comes from the address or a trusted proxy names it as the client. Never type "
				"an administrator's password into the portal.", peers);

			axis_web_auth_add(listener, paragraph(text));
		}
		if (axis_web_auth_portal_peers_overlap(self->auth, &peer, &proxy)) {
			g_autofree gchar *text = g_strdup_printf("%s is among the portal's addresses and %s among the trusted "
				"proxies: they share an address. A login to Axis that arrives from an address in both is refused, "
				"whoever it is forwarded for: if the reverse proxy you reach Axis through is at such an address, nobody "
				"can log in through it. List only the proxy's own addresses in trusted_proxies and only the portal's "
				"own in portal_peers, then restart Axis.", peer, proxy);

			axis_web_auth_add(listener, axis_web_auth_notice(TRUE, text));
		}
		if (barred > 0) {
			g_autofree gchar *when = axis_web_format_time(last_us, "-");
			g_autofree gchar *text = g_strdup_printf("A login to Axis from one of the portal's addresses was last "
				"refused %s (%u since Axis started). The log has a line for it that names the address. If it was your "
				"own login, your browser reaches Axis from an address in the list above: reach Axis from another "
				"address, or correct portal_peers and restart Axis.", when, barred);

			axis_web_auth_add(listener, paragraph(text));
		} else if (peers[0] != '\0') {
			axis_web_auth_add(listener, paragraph("No login to Axis was refused for coming from one of these addresses "
				"since Axis started."));
		}
	}
	axis_web_auth_add(content, listener);

	/* The ceilings over all people: where today stands, and how often each
	 * was reached.  No account is named. */
	{
		guint day = axis_requests_portal_limit_count(self, AXIS_REQUESTS_PORTAL_LIMIT_DAY, NULL);
		guint logins = axis_requests_portal_limit_count(self, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL);
		guint failures = self->auth != NULL ? axis_web_auth_portal_failures(self->auth) : 0;
		g_autofree gchar *today = g_strdup_printf("Requests made through the portal today count %" G_GINT64_FORMAT
			" of %" G_GINT64_FORMAT ", withdrawn ones included (the setting requests.portal_per_day). One person's "
			"share of the day is %" G_GINT64_FORMAT ".", work->today, work->ceiling,
			(gint64)AXIS_REQUESTS_PORTAL_SHARE_OF(work->ceiling));
		g_autofree gchar *hour = g_strdup_printf("Failed logins through the portal in the last hour: %u of %d. At %d, "
			"a login from a browser that Axis does not remember is admitted once a minute until the hour has passed.",
			failures, AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR, AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR);
		g_autofree gchar *reached = day == 0 && logins == 0 ?
			g_strdup("Neither limit was reached in the last 24 hours, or since Axis started if that is less.") :
			g_strdup_printf("Reached in the last 24 hours, or since Axis started if that is less: the day's limit of "
				"requests %u %s, the hour's limit of failed logins %u %s.", day, day == 1 ? "time" : "times", logins,
				logins == 1 ? "time" : "times");

		axis_web_auth_add(limits, paragraph(today));
		axis_web_auth_add(limits, paragraph(hour));
		axis_web_auth_add(limits, paragraph(reached));
		axis_web_auth_add(content, limits);
	}

	for (i = 0; work->rows != NULL && i < work->rows->len; i++) {
		const TokenRow *row = g_ptr_array_index(work->rows, i);
		HtmxTr *tr = htmx_tr_new();
		HtmxTd *action = htmx_td_new();
		g_autofree gchar *state = row->revoked != NULL ?
			g_strdup_printf("Revoked %s by %s", row->revoked, row->revoked_by != NULL ? row->revoked_by : "-") :
			g_strdup("Active");

		axis_web_auth_add(tr, htmx_td_new_with_text(row->created != NULL ? row->created : "-"));
		axis_web_auth_add(tr, htmx_td_new_with_text(row->created_by != NULL ? row->created_by : "-"));
		axis_web_auth_add(tr, htmx_td_new_with_text(row->last_used != NULL ? row->last_used : "Never"));
		axis_web_auth_add(tr, htmx_td_new_with_text(row->last_address != NULL ? row->last_address : "-"));
		axis_web_auth_add(tr, htmx_td_new_with_text(state));
		if (row->revoked == NULL) {
			g_autofree gchar *path = g_strdup_printf(PAGE_PATH "/tokens/%" G_GINT64_FORMAT "/revoke", row->id);
			HtmxNode *form = axis_web_auth_post_form(request, path);

			htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
			axis_web_auth_add(form, axis_web_auth_button("Revoke this token", "button-danger"));
			axis_web_auth_add(action, form);
			active++;
		}
		axis_web_auth_add(tr, action);
		axis_web_auth_add(body, tr);
	}
	axis_web_auth_add(table, body);
	axis_web_auth_add(tokens, paragraph("A token is shown once, when it is made, and only a hash of it is kept. To "
		"replace one: make a second, restart the portal with it, then revoke the first."));
	if (work->rows != NULL && work->rows->len > 0)
		axis_web_auth_add(tokens, table);
	else
		g_object_unref(table);
	if (self->portal_tokens == NULL) {
		axis_web_auth_add(tokens, paragraph("This daemon has no portal API, so no token can be made."));
	} else if (active >= AXIS_PORTAL_TOKENS_MAX_ACTIVE) {
		axis_web_auth_add(tokens, paragraph("Four tokens are active, which is the most there can be. Revoke one to "
			"make another."));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, PAGE_PATH "/tokens");

		axis_web_auth_add(form, axis_web_auth_button("Make a token", "button-primary"));
		axis_web_auth_add(tokens, form);
	}
	axis_web_auth_add(content, tokens);
	return axis_web_page_respond(request, PAGE_TITLE, HTMX_NODE(content));
}

/* The page that answers the making of a token: the only time it is shown. */
static HtmxResponse *
created_response(Work *work)
{
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	HtmxNode *shown = card("The new token", "portal-token");
	HtmxPre *pre = htmx_pre_new();
	HtmxCode *code = htmx_code_new_with_text(work->token);
	HtmxA *back = htmx_a_new_with_href(PAGE_PATH);
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(title), PAGE_TITLE);
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_auth_notice(FALSE, "The token has been made."));
	axis_web_auth_add(shown, paragraph("This is the only time this token is shown. Copy it now: only a hash of it is "
		"kept, so it cannot be shown again. If it is lost, revoke it and make another."));
	htmx_element_add_class(HTMX_ELEMENT(pre), "worker-token");
	axis_web_auth_add(pre, code);
	axis_web_auth_add(shown, pre);
	axis_web_auth_add(shown, paragraph("Give it to the portal as a file only the portal's user can read, or as a "
		"container secret. Never put it on a command line or in an address."));
	axis_web_auth_add(content, shown);
	htmx_node_set_text_content(HTMX_NODE(back), "Back to Request portal");
	axis_web_auth_add(content, back);
	response = axis_web_page_respond(work->request, PAGE_TITLE, HTMX_NODE(content));
	return response;
}

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxRequest *request = work->request;
	HtmxResponse *response;

	work->owner->pending--;
	if (work->kind == WORK_CREATE && g_error_matches(work->error, AXIS_PORTAL_TOKENS_ERROR,
	        AXIS_PORTAL_TOKENS_ERROR_FULL)) {
		response = axis_web_place_redirect(request, PAGE_PATH, "full", "portal-tokens");
	} else if (work->kind == WORK_REVOKE && g_error_matches(work->error, AXIS_PORTAL_TOKENS_ERROR,
	               AXIS_PORTAL_TOKENS_ERROR_NOT_FOUND)) {
		response = axis_web_place_redirect(request, PAGE_PATH, "no-token", "portal-tokens");
	} else if (work->error != NULL) {
		response = axis_web_internal_error(request, work->error);
	} else if (work->kind == WORK_CREATE) {
		/* Never the token: that it was made, and by whom. */
		axis_web_auth_audit("portal token %" G_GINT64_FORMAT " created by %s", work->id, work->by);
		response = created_response(work);
	} else if (work->kind == WORK_REVOKE) {
		axis_web_auth_audit("portal token %" G_GINT64_FORMAT " revoked by %s: %s", work->id, work->by,
			work->changed ? "yes" : "already");
		response = axis_web_place_redirect(request, PAGE_PATH, work->changed ? "revoked" : "already-revoked",
			"portal-tokens");
	} else {
		response = page_response(work);
	}
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	work_free(work);
}

static HtmxResponse *
dispatch(
	AxisRequests *self,
	HtmxRequest  *request,
	WorkKind      kind,
	gint64        id
){
	HtmxResponse *returned;
	GTask *task;
	Work *work;

	if (self->stopping || self->pending >= AXIS_REQUESTS_MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}
	work = g_new0(Work, 1);
	work->owner = self;
	work->kind = kind;
	work->id = id;
	work->request = g_object_ref(request);
	work->deferred = htmx_response_new_deferred();
	work->by = g_strdup(axis_web_request_get_username(request));
	work->done = g_strdup(htmx_request_get_query_param(request, "done"));
	returned = g_object_ref(work->deferred);
	self->pending++;
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return returned;
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return dispatch(user_data, request, WORK_PAGE, 0);
}

static HtmxResponse *
handle_create(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisRequests *self = user_data;

	if (self->portal_tokens == NULL)
		return axis_web_place_redirect(request, PAGE_PATH, "no-api", "portal-tokens");
	return dispatch(self, request, WORK_CREATE, 0);
}

static HtmxResponse *
handle_revoke(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisRequests *self = user_data;
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	guint64 id = 0;

	if (self->portal_tokens == NULL)
		return axis_web_place_redirect(request, PAGE_PATH, "no-api", "portal-tokens");
	if (text == NULL || strlen(text) > 15 || !g_ascii_string_to_unsigned(text, 10, 1, G_MAXINT64, &id, NULL))
		return axis_web_place_redirect(request, PAGE_PATH, "no-token", "portal-tokens");
	return dispatch(self, request, WORK_REVOKE, (gint64)id);
}

void
axis_web_portal_register(
	AxisRequests *self,
	AxisWeb      *web
){
	self->auth = axis_web_get_auth(web);
	axis_web_route_full(web, HTMX_METHOD_GET, PAGE_PATH, AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, PAGE_PATH "/tokens", AXIS_WEB_ROUTE_ADMIN, handle_create, self);
	axis_web_route_full(web, HTMX_METHOD_POST, PAGE_PATH "/tokens/:id/revoke", AXIS_WEB_ROUTE_ADMIN, handle_revoke,
		self);
}
