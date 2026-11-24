/*
 * axis-web.c - the web shell: routes, middleware and the dashboard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-private.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-hub-topic.h"
#include "web/axis-web-limits.h"
#include "web/axis-web-path.h"

#include <string.h>

#include "api/axis-worker-proto.h"
#include "axis-version.h"

#define REQUEST_KEY    "axis-web"
#define KEEP_CACHE_KEY "axis-web-keep-cache-policy"

/*
 * One registered route: enough to tell 404 from 405, and what the router
 * really calls.  The router is given route_trampoline() with the record,
 * so the access check runs for whichever route the router picks, however
 * its pattern relates to the middleware's list of public paths.
 */
typedef struct
{
	HtmxMethod         method;
	gchar             *pattern;
	GRegex            *regex;
	AxisWebRouteFlags  flags;
	HtmxRouteCallback  callback;
	gpointer           user_data;
	AxisModuleSwitch  *module_switch;	/* the module that added it, or %NULL */
} RouteRecord;

struct _AxisWeb
{
	HtmxRouter         *router;	/* owned by the server */
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	GPtrArray          *routes;	/* of RouteRecord* */
	AxisWebHub         *hub;	/* nullable; owned by the caller */
	AxisWebHeaderWidgetFunc header_widget;
	gpointer            header_widget_data;
	AxisModuleSwitch   *header_widget_switch;	/* its module's flag, or %NULL */
	AxisWebDashboardParts *dashboard;	/* the modules' parts of "/" */
	AxisWebActions     *actions;	/* the modules' parts of a title's actions */
	AxisWebHeaderWidgetFunc footer_notices;
	gpointer            footer_notices_data;
	AxisModuleSwitch   *footer_notices_switch;	/* its module's flag, or %NULL */
	AxisWebHeaderWidgetFunc search_box;
	gpointer            search_box_data;
	AxisModuleSwitch   *search_box_switch;	/* its module's flag, or %NULL */
	AxisWebTitleMoreFunc title_more;
	gpointer            title_more_data;
	AxisModuleSwitch   *title_more_switch;	/* its module's flag, or %NULL */
	const gchar        *current_module;	/* while its register_routes runs */
	AxisWebLimits      *limits;	/* the server's, a reference */
	GPtrArray          *kept;	/* (nullable) of Kept*: routes' own data */
	GPtrArray          *words;	/* (nullable) of Words*: the Activity page's */
	AxisWebAttention   *attention;	/* what needs a person: the board and the bell */
};

/* A module's words for one kind of job. */
typedef struct
{
	gchar                    *kind;
	AxisWebActivityWordsFunc  func;
	gpointer                  data;
	AxisModuleSwitch         *module_switch;
} Words;

static void
words_free(Words *words)
{
	g_free(words->kind);
	axis_module_switch_unref(words->module_switch);
	g_free(words);
}

void
axis_web_add_activity_words(
	AxisWeb                  *self,
	const gchar              *kind,
	AxisWebActivityWordsFunc  func,
	gpointer                  user_data
){
	Words *words;

	g_return_if_fail(self != NULL && kind != NULL && func != NULL);

	if (self->words == NULL)
		self->words = g_ptr_array_new_with_free_func((GDestroyNotify)words_free);
	words = g_new0(Words, 1);
	words->kind = g_strdup(kind);
	words->func = func;
	words->data = user_data;
	words->module_switch = self->current_module != NULL ?
		axis_module_registry_ref_switch(self->registry, self->current_module) : NULL;
	g_ptr_array_add(self->words, words);
}

void
axis_web_activity_words(
	AxisWeb   *self,
	GPtrArray *items
){
	guint i;
	guint k;

	for (i = 0; self != NULL && self->words != NULL && i < self->words->len; i++) {
		const Words *words = g_ptr_array_index(self->words, i);
		g_autoptr(GPtrArray) mine = g_ptr_array_new();

		if (words->module_switch != NULL && !axis_module_switch_is_on(words->module_switch))
			continue;
		for (k = 0; k < items->len; k++) {
			AxisWebActivityItem *item = g_ptr_array_index(items, k);

			if (g_strcmp0(item->kind, words->kind) == 0)
				g_ptr_array_add(mine, item);
		}
		if (mine->len > 0)
			words->func(mine, words->data);
	}
}

typedef struct
{
	gpointer       data;
	GDestroyNotify free_data;
} Kept;

static void
kept_free(Kept *kept)
{
	if (kept->free_data != NULL)
		kept->free_data(kept->data);
	g_free(kept);
}

static void
route_record_free(RouteRecord *record)
{
	g_regex_unref(record->regex);
	axis_module_switch_unref(record->module_switch);
	g_free(record->pattern);
	g_free(record);
}

GPtrArray *
axis_web_list_routes(AxisWeb *self)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_return_val_if_fail(self != NULL, list);

	for (i = 0; i < self->routes->len; i++) {
		const RouteRecord *record = g_ptr_array_index(self->routes, i);

		g_ptr_array_add(list, g_strdup_printf("%s %s%s", htmx_method_to_string(record->method),
			(record->flags & AXIS_WEB_ROUTE_PUBLIC) ? "public " : "", record->pattern));
	}
	return list;
}

/* Whether the module that added @record is switched on. */
static gboolean
route_is_live(const RouteRecord *record)
{
	return record->module_switch == NULL || axis_module_switch_is_on(record->module_switch);
}

/* The regular expression htmx-glib's router builds for @pattern. */
static GRegex *
pattern_to_regex(const gchar *pattern)
{
	GString *text = g_string_new("^");
	GRegex *regex;
	const gchar *p = pattern;

	while (*p != '\0') {
		if (*p == ':') {
			p++;
			while (*p != '\0' && *p != '/')
				p++;
			g_string_append(text, "([^/]+)");
		} else if (*p == '*') {
			g_string_append(text, ".*");
			p++;
		} else {
			if (strchr(".?+[](){}^$|\\", *p) != NULL)
				g_string_append_c(text, '\\');
			g_string_append_c(text, *p);
			p++;
		}
	}
	g_string_append_c(text, '$');

	regex = g_regex_new(text->str, 0, 0, NULL);
	g_string_free(text, TRUE);

	return regex;
}

AxisModuleRegistry *
axis_web_get_registry(AxisWeb *self)
{
	return self->registry;
}

AxisWebAuth *
axis_web_get_auth(AxisWeb *self)
{
	return self->auth;
}

AxisWeb *
axis_web_from_request(HtmxRequest *request)
{
	return g_object_get_data(G_OBJECT(request), REQUEST_KEY);
}

AxisWebHub *
axis_web_get_hub(AxisWeb *self)
{
	return self->hub;
}

AxisWebLimits *
axis_web_get_limits(AxisWeb *self)
{
	return self != NULL ? self->limits : NULL;
}

void
axis_web_set_hub(
	AxisWeb    *self,
	AxisWebHub *hub
){
	g_return_if_fail(self != NULL);

	self->hub = hub;
}

void
axis_web_set_header_widget(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
){
	g_return_if_fail(self != NULL);

	self->header_widget = func;
	self->header_widget_data = user_data;
	g_clear_pointer(&self->header_widget_switch, axis_module_switch_unref);
	if (func != NULL && self->current_module != NULL)
		self->header_widget_switch = axis_module_registry_ref_switch(self->registry,
			self->current_module);
}

HtmxNode *
axis_web_build_header_widget(
	AxisWeb     *self,
	HtmxRequest *request
){
	if (self == NULL || self->header_widget == NULL)
		return NULL;
	/* The widget goes with its module. */
	if (self->header_widget_switch != NULL && !axis_module_switch_is_on(self->header_widget_switch))
		return NULL;

	return self->header_widget(request, self->header_widget_data);
}

void
axis_web_set_footer_notices(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
){
	g_return_if_fail(self != NULL);

	self->footer_notices = func;
	self->footer_notices_data = user_data;
	g_clear_pointer(&self->footer_notices_switch, axis_module_switch_unref);
	if (func != NULL && self->current_module != NULL)
		self->footer_notices_switch = axis_module_registry_ref_switch(self->registry, self->current_module);
}

HtmxNode *
axis_web_build_footer_notices(
	AxisWeb     *self,
	HtmxRequest *request
){
	if (self == NULL || self->footer_notices == NULL)
		return NULL;
	/* The notices go with their module. */
	if (self->footer_notices_switch != NULL && !axis_module_switch_is_on(self->footer_notices_switch))
		return NULL;
	return self->footer_notices(request, self->footer_notices_data);
}

void
axis_web_set_search_box(
	AxisWeb                 *self,
	AxisWebHeaderWidgetFunc  func,
	gpointer                 user_data
){
	g_return_if_fail(self != NULL);

	self->search_box = func;
	self->search_box_data = user_data;
	g_clear_pointer(&self->search_box_switch, axis_module_switch_unref);
	if (func != NULL && self->current_module != NULL)
		self->search_box_switch = axis_module_registry_ref_switch(self->registry, self->current_module);
}

HtmxNode *
axis_web_build_search_box(
	AxisWeb     *self,
	HtmxRequest *request
){
	if (self == NULL || self->search_box == NULL)
		return NULL;
	/* The box goes with its module. */
	if (self->search_box_switch != NULL && !axis_module_switch_is_on(self->search_box_switch))
		return NULL;

	return self->search_box(request, self->search_box_data);
}

void
axis_web_set_title_more(
	AxisWeb              *self,
	AxisWebTitleMoreFunc  func,
	gpointer              user_data
){
	g_return_if_fail(self != NULL);

	self->title_more = func;
	self->title_more_data = user_data;
	g_clear_pointer(&self->title_more_switch, axis_module_switch_unref);
	if (func != NULL && self->current_module != NULL)
		self->title_more_switch = axis_module_registry_ref_switch(self->registry, self->current_module);
}

HtmxNode *
axis_web_title_more_new(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	gint64        title_id
){
	AxisWeb *self = axis_web_from_request(request);

	if (self == NULL || self->title_more == NULL || title_id <= 0)
		return NULL;
	/* The card goes with its module. */
	if (self->title_more_switch != NULL && !axis_module_switch_is_on(self->title_more_switch))
		return NULL;
	return self->title_more(request, kind, title_id, self->title_more_data);
}

AxisWebActions *
axis_web_get_actions(AxisWeb *self)
{
	return self->actions;
}

void
axis_web_add_title_actions(
	AxisWeb                  *self,
	AxisWebActionCollectFunc  collect,
	AxisWebActionRenderFunc   render,
	GDestroyNotify            free_figures,
	gpointer                  user_data
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(render != NULL);

	/* The part goes with its module. */
	axis_web_actions_add(self->actions, self->current_module != NULL ?
		axis_module_registry_ref_switch(self->registry, self->current_module) : NULL,
		collect, render, free_figures, user_data);
}

AxisWebDashboardParts *
axis_web_get_dashboard_parts(AxisWeb *self)
{
	return self->dashboard;
}

void
axis_web_add_dashboard_part(
	AxisWeb                     *self,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(render != NULL);

	/* The part goes with its module. */
	axis_web_dashboard_parts_add(self->dashboard, self->current_module != NULL ?
		axis_module_registry_ref_switch(self->registry, self->current_module) : NULL, FALSE,
		collect, render, free_figures, user_data);
}

void
axis_web_add_dashboard_part_for_admins(
	AxisWeb                     *self,
	AxisWebDashboardCollectFunc  collect,
	AxisWebDashboardRenderFunc   render,
	GDestroyNotify               free_figures,
	gpointer                     user_data
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(render != NULL);

	axis_web_dashboard_parts_add(self->dashboard, self->current_module != NULL ?
		axis_module_registry_ref_switch(self->registry, self->current_module) : NULL, TRUE,
		collect, render, free_figures, user_data);
}

void
axis_web_set_dashboard_part_card(
	AxisWeb              *self,
	AxisWebDashboardSlot  slot
){
	g_return_if_fail(self != NULL);

	axis_web_dashboard_parts_set_card(self->dashboard, slot);
}

AxisWebAttention *
axis_web_get_attention(AxisWeb *self)
{
	return self->attention;
}

void
axis_web_add_attention_source(
	AxisWeb                 *self,
	const gchar             *name,
	AxisAttentionSourceFunc  func,
	gpointer                 user_data
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(name != NULL && func != NULL);

	/* The source goes with its module. */
	axis_web_attention_add_source(self->attention, self->current_module != NULL ?
		axis_module_registry_ref_switch(self->registry, self->current_module) : NULL, name, func, user_data);
}

void
axis_web_stop(AxisWeb *self)
{
	g_return_if_fail(self != NULL);

	axis_web_dashboard_parts_stop(self->dashboard);
	/* A reading of what needs attention runs the modules' sources. */
	axis_web_attention_stop(self->attention);
}

/* ── Topics a page follows ──────────────────────────────────────── */

#define TOPICS_KEY "axis-web-topics"

void
axis_web_page_add_topic(
	HtmxRequest *request,
	const gchar *topic
){
	GPtrArray *topics;
	guint i;

	g_return_if_fail(HTMX_IS_REQUEST(request));
	g_return_if_fail(axis_web_hub_topic_is_valid(topic));

	topics = g_object_get_data(G_OBJECT(request), TOPICS_KEY);
	if (topics == NULL) {
		topics = g_ptr_array_new_null_terminated(AXIS_WEB_HUB_MAX_TOPICS, g_free, TRUE);
		g_object_set_data_full(G_OBJECT(request), TOPICS_KEY, topics,
			(GDestroyNotify)g_ptr_array_unref);
	}

	for (i = 0; i < topics->len; i++) {
		if (strcmp(g_ptr_array_index(topics, i), topic) == 0)
			return;
	}
	if (topics->len >= AXIS_WEB_HUB_MAX_TOPICS) {
		g_warning("a page follows more than %d topics; ignoring '%s'",
			AXIS_WEB_HUB_MAX_TOPICS, topic);
		return;
	}

	g_ptr_array_add(topics, g_strdup(topic));
}

const gchar *const *
axis_web_request_get_topics(HtmxRequest *request)
{
	GPtrArray *topics = g_object_get_data(G_OBJECT(request), TOPICS_KEY);

	return topics != NULL && topics->len > 0 ? (const gchar *const *)topics->pdata : NULL;
}

void
axis_web_keep_cache_policy(HtmxResponse *response)
{
	g_object_set_data(G_OBJECT(response), KEEP_CACHE_KEY, GINT_TO_POINTER(1));
}

gboolean
axis_web_knows_path(
	AxisWeb     *self,
	const gchar *path
){
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	for (i = 0; path != NULL && i < self->routes->len; i++) {
		const RouteRecord *record = g_ptr_array_index(self->routes, i);

		if (g_regex_match(record->regex, path, 0, NULL))
			return TRUE;
	}

	return FALSE;
}

/* What the router calls for every route registered through the shell. */
static HtmxResponse *
route_trampoline(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const RouteRecord *record = user_data;
	HtmxResponse *refusal;

	/* A module that is switched off has no pages, whoever asks.  The
	 * flag is read on every request, so switching takes effect at once. */
	if (!route_is_live(record))
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	refusal = axis_web_auth_authorize(request, record->flags);
	if (refusal != NULL)
		return refusal;

	return record->callback(request, params, record->user_data);
}

void
axis_web_route_full(
	AxisWeb           *self,
	HtmxMethod         method,
	const gchar       *pattern,
	AxisWebRouteFlags  flags,
	HtmxRouteCallback  callback,
	gpointer           user_data
){
	RouteRecord *record;

	g_return_if_fail(self != NULL);
	g_return_if_fail(method != HTMX_METHOD_UNKNOWN);
	g_return_if_fail(pattern != NULL);
	g_return_if_fail(callback != NULL);

	if ((flags & AXIS_WEB_ROUTE_PUBLIC) && !axis_web_auth_is_public_pattern(pattern)) {
		g_critical("refusing to register '%s' as a public route: only /healthz, /login, "
			"/setup, the three well-known icon paths and the static assets may be used "
			"without logging in", pattern);
		return;
	}
	/* The worker API is a class of its own: only its routes are there, and
	 * they are nowhere else. */
	if ((flags & AXIS_WEB_ROUTE_WORKER) &&
	    (flags != AXIS_WEB_ROUTE_WORKER || method != HTMX_METHOD_POST || !axis_web_auth_is_worker_pattern(pattern))) {
		g_critical("refusing to register '%s' as a worker route: a worker route is a POST under the worker "
			"API's prefix, with no other flag", pattern);
		return;
	}
	if (!(flags & AXIS_WEB_ROUTE_WORKER) && axis_web_auth_is_worker_pattern(pattern)) {
		g_critical("refusing to register '%s': only worker routes are under the worker API's prefix", pattern);
		return;
	}
	/* The portal API has a listener and a router of its own
	 * (api/axis-portal-api.h): nothing of this one is under its prefix. */
	if (axis_web_auth_is_portal_path(pattern)) {
		g_critical("refusing to register '%s': the portal API is not answered on this listener", pattern);
		return;
	}

	record = g_new0(RouteRecord, 1);
	record->method = method;
	record->pattern = g_strdup(pattern);
	record->regex = pattern_to_regex(pattern);
	record->flags = flags;
	record->callback = callback;
	record->user_data = user_data;
	if (self->current_module != NULL)
		record->module_switch = axis_module_registry_ref_switch(self->registry,
			self->current_module);
	if (record->regex == NULL) {
		g_warning("route pattern '%s' is not valid", pattern);
		g_free(record);
		return;
	}
	g_ptr_array_add(self->routes, record);

	htmx_router_add_route(self->router, method, pattern, route_trampoline, record);
}

void
axis_web_route(
	AxisWeb           *self,
	HtmxMethod         method,
	const gchar       *pattern,
	HtmxRouteCallback  callback,
	gpointer           user_data
){
	axis_web_route_full(self, method, pattern, AXIS_WEB_ROUTE_DEFAULT, callback, user_data);
}

void
axis_web_get(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data)
{
	axis_web_route(self, HTMX_METHOD_GET, pattern, callback, user_data);
}

void
axis_web_post(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data)
{
	axis_web_route(self, HTMX_METHOD_POST, pattern, callback, user_data);
}

void
axis_web_put(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data)
{
	axis_web_route(self, HTMX_METHOD_PUT, pattern, callback, user_data);
}

void
axis_web_patch(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data)
{
	axis_web_route(self, HTMX_METHOD_PATCH, pattern, callback, user_data);
}

void
axis_web_delete(AxisWeb *self, const gchar *pattern, HtmxRouteCallback callback, gpointer user_data)
{
	axis_web_route(self, HTMX_METHOD_DELETE, pattern, callback, user_data);
}

/* ── Security headers (the list is in axis-web-headers.c) ───────── */

static void
apply_headers(HtmxResponse *response)
{
	axis_web_add_security_headers(response);

	/* Only the static assets have a cache policy of their own. */
	if (g_object_get_data(G_OBJECT(response), KEEP_CACHE_KEY) == NULL)
		htmx_response_add_header(response, "Cache-Control", "no-store");
}

static void
on_deferred_complete(
	HtmxResponse *deferred,
	HtmxResponse *response,
	gpointer      user_data
){
	apply_headers(response);
	/* @user_data: the shell, when the request was a person's action. */
	if (user_data != NULL)
		axis_web_attention_changed(user_data);
}

/* A streaming response has written its own message; so do the headers. */
static void
apply_headers_to_message(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);

	if (message == NULL)
		return;

	axis_web_replace_security_headers(soup_server_message_get_response_headers(message));
}

/* ── 404 and 405 ────────────────────────────────────────────────── */

static gboolean
route_answers(
	const RouteRecord *record,
	HtmxMethod         method
){
	/* The router's own rule: a GET route answers HEAD. */
	return record->method == method ||
	       (record->method == HTMX_METHOD_GET && method == HTMX_METHOD_HEAD);
}

/*
 * What to say about a request no handler answered: 405 with the methods
 * the path does have, or 404.  A route that does answer this method but
 * produced nothing is a bug in its handler.
 */
static HtmxResponse *
unanswered(
	AxisWeb     *self,
	HtmxRequest *request
){
	static const HtmxMethod order[] = {
		HTMX_METHOD_GET, HTMX_METHOD_HEAD, HTMX_METHOD_POST,
		HTMX_METHOD_PUT, HTMX_METHOD_PATCH, HTMX_METHOD_DELETE
	};
	const gchar *path = htmx_request_get_path(request);
	HtmxMethod method = htmx_request_get_method(request);
	gboolean path_known = FALSE;
	gboolean allowed[G_N_ELEMENTS(order)];
	GString *allow;
	HtmxResponse *response;
	guint i;
	guint j;

	memset(allowed, 0, sizeof(allowed));
	for (i = 0; path != NULL && i < self->routes->len; i++) {
		const RouteRecord *record = g_ptr_array_index(self->routes, i);

		/* The routes of a module that is off do not exist. */
		if (!route_is_live(record) || !g_regex_match(record->regex, path, 0, NULL))
			continue;

		if (route_answers(record, method)) {
			g_autoptr(GError) error = NULL;

			g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"the handler produced no response");
			return axis_web_internal_error(request, error);
		}

		path_known = TRUE;
		for (j = 0; j < G_N_ELEMENTS(order); j++) {
			if (route_answers(record, order[j]))
				allowed[j] = TRUE;
		}
	}

	if (!path_known)
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	allow = g_string_new("");
	for (j = 0; j < G_N_ELEMENTS(order); j++) {
		if (!allowed[j])
			continue;
		if (allow->len > 0)
			g_string_append(allow, ", ");
		g_string_append(allow, htmx_method_to_string(order[j]));
	}

	response = axis_web_error_respond(request, 405, "Not a page to open",
		"This address only answers a button or a form on an Axis page. Go back to that page and use it there.");
	htmx_response_add_header(response, "Allow", allow->str);
	g_string_free(allow, TRUE);

	return response;
}

/* ── Middleware ─────────────────────────────────────────────────── */

static void
web_middleware(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	AxisWeb *self = user_data;
	HtmxRequest *request = htmx_context_get_request(context);
	HtmxResponse *response;
	gboolean acted;

	g_object_set_data(G_OBJECT(request), REQUEST_KEY, self);

	next(context, next_data);

	response = htmx_context_get_response(context);
	if (response == NULL) {
		response = unanswered(self, request);
		htmx_context_set_response(context, response);
	}

	/* A person's action (any request of a logged-in user that is not a GET)
	 * may have ended what a notice is about, or begun one: what needs
	 * attention is read again soon, once the action is done. */
	acted = htmx_request_get_method(request) != HTMX_METHOD_GET &&
		htmx_request_get_method(request) != HTMX_METHOD_HEAD && axis_web_request_get_user_id(request) != 0;

	if (htmx_response_is_streaming(response)) {
		apply_headers_to_message(request);
	} else if (htmx_response_is_deferred(response)) {
		htmx_response_on_complete(response, on_deferred_complete, acted ? self : NULL, NULL);
	} else {
		apply_headers(response);
		if (acted)
			axis_web_attention_changed(self);
	}
}

/* ── Lifetime ───────────────────────────────────────────────────── */

/* What the request limits (axis-web-limits.h) ask of the auth: whose
 * proxies are trusted, and whether a request is a worker's with its valid
 * token (only on the worker API's paths, where it is the credential).  The
 * limits themselves know nothing of accounts. */
G_STATIC_ASSERT(sizeof(AXIS_WEB_LIMITS_WORKER_PATH) == sizeof(AXIS_WORKER_API_PREFIX) - 1);
G_STATIC_ASSERT(AXIS_WEB_LIMITS_WORKER_BODY == AXIS_WORKER_MAX_REQUEST_BYTES);

static AxisProxyList *
limits_trusted(gpointer user_data)
{
	return axis_web_auth_get_trusted(user_data);
}

static gboolean
limits_exempt(
	const gchar *path,
	const gchar *authorization,
	gpointer     user_data
){
	return axis_web_auth_is_worker_path(path) && axis_web_auth_is_worker_token(user_data, authorization);
}

AxisWeb *
axis_web_new(
	HtmxServer         *server,
	AxisModuleRegistry *registry,
	AxisWebAuth        *auth
){
	AxisWeb *self;
	guint i;

	g_return_val_if_fail(HTMX_IS_SERVER(server), NULL);
	g_return_val_if_fail(registry != NULL, NULL);
	g_return_val_if_fail(auth != NULL, NULL);

	self = g_new0(AxisWeb, 1);
	self->router = htmx_server_get_router(server);
	self->registry = registry;
	self->auth = auth;
	self->routes = g_ptr_array_new_with_free_func((GDestroyNotify)route_record_free);
	self->dashboard = axis_web_dashboard_parts_new(self);
	self->actions = axis_web_actions_new();
	self->attention = axis_web_attention_new(self);

	/* Outermost, so that whatever the auth middleware answers gets the
	 * security headers too. */
	htmx_server_use(server, web_middleware, self, NULL);
	/* The limits on a request: the daemon installed them before it
	 * listened (axis-web-limits.h); anything else gets them here.  From now
	 * on they judge proxies by the auth's list, whose names are looked up,
	 * and know its workers' tokens. */
	self->limits = axis_web_limits_ref(axis_web_limits_install(server, NULL));
	axis_web_limits_set_sources(self->limits, limits_trusted, limits_exempt, auth);

	/* The shell's own routes first: the router takes the first match, so
	 * no module route can shadow the static assets or the login pages. */
	axis_web_route_full(self, HTMX_METHOD_GET, AXIS_WEB_STATIC_PREFIX "*",
		AXIS_WEB_ROUTE_PUBLIC, axis_web_static_handle, self);
	axis_web_icons_register(self);
	axis_web_auth_install(auth, self, server);
	axis_web_get(self, "/", axis_web_dashboard_handle, self);
	axis_web_attention_register(self->attention, self);

	for (i = 0; i < axis_module_registry_get_count(registry); i++) {
		self->current_module = axis_module_registry_get_id(registry, i);
		axis_module_registry_register_routes(registry, i, self);
	}
	self->current_module = NULL;

	return self;
}

/* ── For feature code ───────────────────────────────────────────── */

const gchar *
axis_web_request_get_username(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	return state != NULL && state->user_id != 0 ? state->username : NULL;
}

gboolean
axis_web_request_is_admin(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	return state != NULL && state->user_id != 0 && state->role == AXIS_USER_ROLE_ADMIN;
}

gint64
axis_web_request_get_user_id(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	return state != NULL ? state->user_id : 0;
}

JsonObject *
axis_web_request_add_defaults(
	HtmxRequest *request,
	const gchar *kind
){
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;
	JsonNode *member;

	if (state == NULL || state->user_id == 0 || state->add_defaults == NULL)
		return NULL;
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, state->add_defaults, -1, NULL))
		return NULL;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;
	member = json_object_get_member(json_node_get_object(root), kind);
	if (member == NULL || !JSON_NODE_HOLDS_OBJECT(member))
		return NULL;
	return json_object_ref(json_node_get_object(member));
}

void
axis_web_request_note_add_defaults(
	HtmxRequest *request,
	const gchar *all
){
	const AxisAuthState *state = axis_web_auth_state(request);
	AxisWebAuth *auth = axis_web_auth_from_request(request);
	const AxisUser *cached;

	if (state == NULL || state->user_id == 0 || auth == NULL)
		return;
	/* This request's answer is drawn with them too. */
	g_free(((AxisAuthState *)state)->add_defaults);
	((AxisAuthState *)state)->add_defaults = g_strdup(all);
	cached = axis_session_store_get_user(auth->sessions, state->user_id);
	if (cached != NULL) {
		g_autoptr(AxisUser) updated = axis_user_copy(cached);

		g_free(updated->add_defaults);
		updated->add_defaults = g_strdup(all);
		axis_session_store_put_user(auth->sessions, updated);
	}
}

const gchar *
axis_web_request_get_client_address(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	return state != NULL ? state->client_address : AXIS_PROXY_UNKNOWN_CLIENT;
}

HtmxNode *
axis_web_csrf_field(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);

	if (state == NULL || state->user_id == 0)
		return NULL;

	return axis_web_auth_hidden(AXIS_CSRF_FIELD, state->csrf);
}

void
axis_web_keep(
	AxisWeb        *self,
	gpointer        data,
	GDestroyNotify  free_data
){
	Kept *kept;

	g_return_if_fail(self != NULL);

	if (self->kept == NULL)
		self->kept = g_ptr_array_new_with_free_func((GDestroyNotify)kept_free);
	kept = g_new0(Kept, 1);
	kept->data = data;
	kept->free_data = free_data;
	g_ptr_array_add(self->kept, kept);
}

void
axis_web_free(AxisWeb *self)
{
	if (self == NULL)
		return;

	/* The auth's list goes with the auth. */
	axis_web_limits_set_sources(self->limits, NULL, NULL, NULL);
	axis_web_limits_unref(self->limits);
	axis_web_dashboard_parts_free(self->dashboard);
	axis_web_attention_free(self->attention);
	axis_web_actions_free(self->actions);
	g_ptr_array_free(self->routes, TRUE);
	g_clear_pointer(&self->kept, g_ptr_array_unref);
	g_clear_pointer(&self->words, g_ptr_array_unref);
	axis_module_switch_unref(self->header_widget_switch);
	axis_module_switch_unref(self->footer_notices_switch);
	axis_module_switch_unref(self->search_box_switch);
	axis_module_switch_unref(self->title_more_switch);
	g_free(self);
}
