/*
 * axis-portal-api.c - the portal API: the eleven operations a request portal
 * may ask of the daemon, on a listener of their own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-portal-api.h"

#include <libsoup/soup.h>
#include <string.h>

#include "core/axis-proxy.h"
#include "web/axis-login-throttle.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-portal.h"
#include "web/axis-web-limits.h"

G_STATIC_ASSERT(AXIS_PORTAL_KIND_MOVIE == (gint)AXIS_REQUEST_MOVIE);
G_STATIC_ASSERT(AXIS_PORTAL_KIND_SERIES == (gint)AXIS_REQUEST_SERIES);
G_STATIC_ASSERT(AXIS_PORTAL_SEASONS_ALL == (gint)AXIS_REQUEST_SEASONS_ALL);
G_STATIC_ASSERT(AXIS_PORTAL_SEASONS_FIRST == (gint)AXIS_REQUEST_SEASONS_FIRST);
G_STATIC_ASSERT(AXIS_PORTAL_SEASONS_LATEST == (gint)AXIS_REQUEST_SEASONS_LATEST);
G_STATIC_ASSERT(AXIS_PORTAL_SEASONS_PICKED == (gint)AXIS_REQUEST_SEASONS_PICKED);
G_STATIC_ASSERT(AXIS_PORTAL_MAX_SEASONS == AXIS_REQUESTS_MAX_SEASONS);
G_STATIC_ASSERT(AXIS_PORTAL_PROOF_LEN == AXIS_TOKEN_LEN);
G_STATIC_ASSERT(AXIS_PORTAL_REQUEST_ID_LEN == AXIS_PUBLIC_ID_LEN);
G_STATIC_ASSERT(AXIS_PORTAL_ART_MAX_BYTES == AXIS_META_ART_MAX_BYTES);

#define GATE_KEY "axis-portal-gate"

/* Wrong tokens: the delays of the worker API's throttle. */
#define THROTTLE_BASE_US    (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)
#define THROTTLE_MAX_US     (G_GINT64_CONSTANT(15) * 60 * G_USEC_PER_SEC)
#define THROTTLE_FORGET_US  (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)
#define THROTTLE_MAX_KEYS   10000

#define MINUTE_US           (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

static const gchar *const operations[AXIS_PORTAL_N_OPS + 1] = {
	"hello", "login", "logout", "whoami", "search", "browse", "title", "art", "request", "requests", "withdraw", NULL
};

const gchar *const *
axis_portal_api_operations(void)
{
	return operations;
}

/* A search kept in memory: the titles it found, by their ids. */
typedef struct
{
	gint64    expires_us;	/* monotonic */
	GArray   *ids;		/* gint64 */
	gboolean  more;
} Cached;

static void
cached_free(gpointer data)
{
	Cached *cached = data;

	g_array_unref(cached->ids);
	g_free(cached);
}

struct _AxisPortalApi
{
	AxisDbPool               *pool;
	AxisWebAuth              *auth;		/* borrowed */
	AxisPortalTokens         *tokens;	/* borrowed */
	AxisRequests             *requests;
	AxisMeta                 *meta;
	AxisDiscover             *discover;	/* nullable */
	AxisLoginThrottle        *by_address;	/* wrong tokens, per client address */
	AxisPortalApiServingFunc  serving;
	gpointer                  serving_data;
	gint64                    time_offset_us;
	guint                     pending;	/* requests being handled */
	gboolean                  stopping;
	/* The search budget (main loop). */
	GQueue                    searches;	/* gint64*: when each search that asked the provider was */
	GHashTable               *by_person;	/* gint64* account -> GQueue* of gint64* */
	/* The searches kept (any thread). */
	GMutex                    cache_lock;
	GHashTable               *cache;	/* (cache_lock) key -> Cached* */
};

/* What the gate found out about a request, for its operation. */
typedef struct
{
	gint64  token;
	gchar  *address;
} Gate;

static void
gate_free(gpointer data)
{
	Gate *gate = data;

	g_free(gate->address);
	g_free(gate);
}

static gint64
now_monotonic(AxisPortalApi *self)
{
	return g_get_monotonic_time() + self->time_offset_us;
}

/* ── Answers ────────────────────────────────────────────────────── */

/* Every answer of this listener: JSON (or a poster), never kept, never a
 * page. */
static void
common_headers(HtmxResponse *response)
{
	htmx_response_add_header(response, "Cache-Control", "no-store");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
	htmx_response_add_header(response, "Referrer-Policy", "no-referrer");
}

static HtmxResponse *
json_response(
	guint        status,
	const gchar *body
){
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, status);
	htmx_response_set_content(response, body);
	htmx_response_set_content_type(response, "application/json");
	common_headers(response);
	return response;
}

static HtmxResponse *
refusal(
	guint        status,
	const gchar *word,
	const gchar *message,
	gint64       wait_s
){
	g_autofree gchar *body = axis_portal_error_wait_to_json(word, message, wait_s);
	HtmxResponse *response = json_response(status, body);

	if (wait_s > 0) {
		g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT, wait_s);

		htmx_response_add_header(response, "Retry-After", seconds);
	}
	return response;
}

static HtmxResponse *
busy(void)
{
	return refusal(503, "busy", "the daemon cannot do this now; try again", 5);
}

/* The one answer of a proof that is missing, malformed, unknown or ended. */
static HtmxResponse *
no_proof(void)
{
	return refusal(401, "proof", "a valid session proof is required", 0);
}

/* ── The gate ───────────────────────────────────────────────────── */

static const gchar *
request_header(
	HtmxRequest *request,
	const gchar *name
){
	SoupServerMessage *message = htmx_request_get_message(request);

	return message != NULL ? soup_message_headers_get_one(soup_server_message_get_request_headers(message), name) :
		NULL;
}

/* Whether the body is declared JSON: "application/json", with nothing but a
 * UTF-8 charset after it. */
static gboolean
json_content_type(const gchar *type)
{
	g_autofree gchar *lower = NULL;
	g_autofree gchar *compact = NULL;
	GString *out;
	const gchar *p;

	if (type == NULL)
		return FALSE;
	lower = g_ascii_strdown(type, -1);
	out = g_string_new(NULL);
	for (p = lower; *p != '\0'; p++) {
		if (*p != ' ' && *p != '\t')
			g_string_append_c(out, *p);
	}
	compact = g_string_free(out, FALSE);
	return strcmp(compact, "application/json") == 0 || strcmp(compact, "application/json;charset=utf-8") == 0;
}

static gboolean
under_prefix(const gchar *path)
{
	return path != NULL && g_str_has_prefix(path, AXIS_PORTAL_API_PREFIX);
}

/* See the order in axis-portal-api.h. */
static void
portal_gate(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	AxisPortalApi *self = user_data;
	HtmxRequest *request = htmx_context_get_request(context);
	SoupServerMessage *message = htmx_request_get_message(request);
	const gchar *path = htmx_request_get_path(request);
	const gchar *peer = message != NULL ? soup_server_message_get_remote_host(message) : NULL;
	g_autofree gchar *address = axis_proxy_address_key(peer != NULL ? peer : "");
	gint64 now = now_monotonic(self);
	const gchar *authorization;
	const gchar *token = NULL;
	AxisPortalOp op;
	GBytes *body;
	gint64 wait;
	gint64 id = 0;
	Gate *gate;

	/* This listener answers the portal API and nothing else. */
	if (!under_prefix(path)) {
		htmx_context_set_response(context, refusal(404, "not-found", "there is no such address", 0));
		return;
	}
	/* Not while the daemon does not serve, nor while the Requests module is
	 * switched off: then nothing can be asked for, here as on its pages. */
	if (self->stopping || (self->serving != NULL && !self->serving(self->serving_data)) ||
	    !axis_requests_is_on(self->requests)) {
		htmx_context_set_response(context, busy());
		return;
	}
	wait = axis_login_throttle_check(self->by_address, address, now);
	if (wait > 0) {
		htmx_context_set_response(context, refusal(429, "throttled", "too many wrong tokens from this address; wait",
			MAX((wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1)));
		return;
	}
	/* Nothing is taken from an address: not a token, not anything else. */
	if (message == NULL || soup_server_message_get_uri(message) == NULL ||
	    g_uri_get_query(soup_server_message_get_uri(message)) != NULL) {
		htmx_context_set_response(context, refusal(400, "query", "the portal API takes no query string", 0));
		return;
	}

	/* As a list: two Authorization headers join with a comma, which makes
	 * no well-formed token. */
	authorization = soup_message_headers_get_list(soup_server_message_get_request_headers(message), "Authorization");
	if (authorization != NULL && g_ascii_strncasecmp(authorization, "Bearer ", 7) == 0)
		token = authorization + 7;
	if (token == NULL || !axis_portal_tokens_lookup(self->tokens, token, &id)) {
		HtmxResponse *response = refusal(401, "unauthorized", "a portal token is required", 0);

		if (token != NULL) {
			axis_login_throttle_fail(self->by_address, address, now);
			/* The token is never written anywhere; the address is. */
			g_message("portal API: a request from %s carried a token that is not a portal's (or is revoked)",
				peer != NULL ? peer : "?");
		}
		htmx_response_add_header(response, "WWW-Authenticate", "Bearer");
		htmx_context_set_response(context, response);
		return;
	}

	/* A browser can never be made to call this: what it would send is
	 * refused even with a valid token. */
	if (request_header(request, "Origin") != NULL || request_header(request, "Sec-Fetch-Site") != NULL ||
	    request_header(request, "Sec-Fetch-Mode") != NULL || request_header(request, "Cookie") != NULL) {
		htmx_context_set_response(context, refusal(403, "forbidden", "the portal API takes no request a browser sends",
			0));
		return;
	}
	if (htmx_request_get_method(request) != HTMX_METHOD_POST) {
		HtmxResponse *response = refusal(405, "method", "the portal API takes POST only", 0);

		htmx_response_add_header(response, "Allow", "POST");
		htmx_context_set_response(context, response);
		return;
	}
	if (!json_content_type(htmx_request_get_content_type(request))) {
		htmx_context_set_response(context, refusal(415, "content-type", "the portal API takes application/json only",
			0));
		return;
	}
	body = htmx_request_get_body_bytes(request);
	if (body != NULL && g_bytes_get_size(body) > AXIS_PORTAL_MAX_REQUEST_BYTES) {
		htmx_context_set_response(context, refusal(413, "too-large", "the body is larger than the portal API takes",
			0));
		return;
	}
	if (!axis_portal_op_from_name(path + strlen(AXIS_PORTAL_API_PREFIX), &op)) {
		htmx_context_set_response(context, refusal(404, "not-found", "there is no such operation", 0));
		return;
	}

	gate = g_new0(Gate, 1);
	gate->token = id;
	gate->address = g_strdup(peer != NULL ? peer : "");
	g_object_set_data_full(G_OBJECT(request), GATE_KEY, gate, gate_free);

	next(context, next_data);
	if (htmx_context_get_response(context) == NULL)
		htmx_context_set_response(context, refusal(404, "not-found", "there is no such operation", 0));
}

/* ── Work off the main loop ─────────────────────────────────────── */

typedef struct
{
	AxisPortalApi     *api;
	AxisPortalOp       op;
	AxisPortalRequest *req;
	gint64             user_id;
	gchar             *user_name;
	HtmxResponse      *deferred;
	gint64             token;
	gchar             *address;
	gboolean           note_token;
	/* search: what the memory already holds, for each kind asked. */
	gboolean           asked[2];
	GArray            *cached[2];
	gboolean           cached_more[2];
	/* The answer. */
	guint              status;
	gchar             *body;
	GBytes            *bytes;
	gchar             *content_type;
	gint64             wait_s;
} Work;

static void
work_free(Work *work)
{
	g_clear_object(&work->deferred);
	axis_portal_request_free(work->req);
	g_free(work->user_name);
	g_free(work->address);
	g_clear_pointer(&work->cached[0], g_array_unref);
	g_clear_pointer(&work->cached[1], g_array_unref);
	g_free(work->body);
	g_clear_pointer(&work->bytes, g_bytes_unref);
	g_free(work->content_type);
	g_free(work);
}

static void
reply_json(
	Work  *work,
	guint  status,
	gchar *body
){
	g_free(work->body);
	work->status = status;
	work->body = body;
}

static void
reply_error(
	Work        *work,
	guint        status,
	const gchar *word,
	const gchar *message,
	gint64       wait_s
){
	work->wait_s = wait_s;
	reply_json(work, status, axis_portal_error_wait_to_json(word, message, wait_s));
}

/* A failure of the database or of a layer: logged here, never said. */
static void
reply_busy(
	Work         *work,
	const GError *error
){
	g_warning("portal API: %s could not be answered: %s", axis_portal_op_name(work->op),
		error != NULL ? error->message : "?");
	reply_error(work, 503, "busy", "the daemon cannot do this now; try again", 5);
}

static void
reply_not_found(Work *work)
{
	reply_error(work, 404, "not-found", "there is no such thing", 0);
}

/* How long until the calendar day ends, in seconds (at least one). */
static gint64
until_tomorrow(void)
{
	g_autoptr(GTimeZone) zone = g_time_zone_new_local();
	gint64 now = g_get_real_time();
	gint64 start = 0;
	gint64 end = 0;

	axis_request_window_bounds(zone, now, AXIS_REQUEST_WINDOW_DAY, &start, &end);
	return MAX((end - now + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1);
}

/* The answer to a refusal of the requests layer.  Its message is the layer's
 * own words and holds nothing that was posted. */
static void
reply_requests_error(
	Work         *work,
	const GError *error
){
	AxisRequestQuota quota;

	if (error == NULL || error->domain != AXIS_REQUESTS_ERROR) {
		reply_busy(work, error);
		return;
	}
	switch (error->code) {
	case AXIS_REQUESTS_ERROR_NOT_FOUND:
		reply_not_found(work);
		break;
	case AXIS_REQUESTS_ERROR_INVALID:
		reply_error(work, 422, "invalid", error->message, 0);
		break;
	case AXIS_REQUESTS_ERROR_QUOTA:
		/* Until the person's window ends. */
		memset(&quota, 0, sizeof(quota));
		if (!axis_requests_get_quota(work->api->requests, work->user_id, &quota, NULL))
			quota.end_us = 0;
		reply_error(work, 429, "quota", error->message,
			MAX((quota.end_us - g_get_real_time()) / G_USEC_PER_SEC, 1));
		break;
	case AXIS_REQUESTS_ERROR_TOO_MANY:
		reply_error(work, 429, "too-many", error->message, 3600);
		break;
	case AXIS_REQUESTS_ERROR_PORTAL_FULL:
		reply_error(work, 429, "portal-limit", error->message, until_tomorrow());
		break;
	case AXIS_REQUESTS_ERROR_KIND_OFF:
		reply_error(work, 409, "kind-off", error->message, 0);
		break;
	case AXIS_REQUESTS_ERROR_CLOSED:
		reply_error(work, 409, "closed", error->message, 0);
		break;
	case AXIS_REQUESTS_ERROR_DECIDED:
		reply_error(work, 409, "decided", error->message, 0);
		break;
	default:
		reply_error(work, 409, "refused", "the request cannot be made", 0);
		break;
	}
}

/* ── What an answer says of a title and of a request ────────────── */

static const gchar *
kind_name(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_SERIES ? "series" : "movie";
}

/* A title's (or a season's) one state, in the API's words. */
static const gchar *
title_state_name(AxisRequestState state)
{
	switch (state) {
	case AXIS_REQUEST_STATE_AVAILABLE:
		return "available";
	case AXIS_REQUEST_STATE_PARTLY:
		return "partly_available";
	case AXIS_REQUEST_STATE_DOWNLOADING:
		return "downloading";
	case AXIS_REQUEST_STATE_APPROVED:
	case AXIS_REQUEST_STATE_FAILED:
		return "approved";
	case AXIS_REQUEST_STATE_REQUESTED:
		return "requested";
	case AXIS_REQUEST_STATE_DECLINED:
		return "declined";
	case AXIS_REQUEST_STATE_CLOSED:
		return "closed";
	default:
		return "open";
	}
}

/* A request's one state, and the cause of a failed one (%NULL: none). */
static const gchar *
request_state_name(
	const AxisRequestInfo  *info,
	const gchar           **cause
){
	*cause = NULL;
	switch (info->state) {
	case AXIS_REQUEST_STATE_REQUESTED:
		return "waiting";
	case AXIS_REQUEST_STATE_APPROVED:
		return "approved";
	case AXIS_REQUEST_STATE_DOWNLOADING:
		return "downloading";
	case AXIS_REQUEST_STATE_PARTLY:
		return "partly_available";
	case AXIS_REQUEST_STATE_AVAILABLE:
		return "available";
	case AXIS_REQUEST_STATE_DECLINED:
		return "declined";
	case AXIS_REQUEST_STATE_FAILED:
		*cause = info->cause == AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED ? "download_failed" :
			info->cause == AXIS_REQUEST_CAUSE_NEEDS_ADMIN ? "needs_admin" : "not_added";
		return "failed";
	case AXIS_REQUEST_STATE_REMOVED:
		/* Its title was added and then removed from Axis: the fourth of the
		 * fixed causes. */
		*cause = "removed";
		return "failed";
	default:
		return "withdrawn";
	}
}

static void
fill_view(
	const AxisRequestInfo *info,
	gint64                 viewer,
	AxisPortalRequestView *view
){
	memset(view, 0, sizeof(*view));
	view->id = info->public_id;
	view->title = info->title_id;
	view->kind = axis_request_kind_name(info->kind);
	view->name = info->title;
	view->year = info->year;
	view->seasons = info->seasons != NULL ? (const gint *)(gpointer)info->seasons->data : NULL;
	view->n_seasons = info->seasons != NULL ? info->seasons->len : 0;
	view->state = request_state_name(info, &view->cause);
	view->counts = info->kind == AXIS_REQUEST_SERIES;
	view->have = info->have;
	view->of = info->aired;
	/* A decline's reason is the administrator's own words, for the person
	 * who asked and for followers: the lists here hold no other request. */
	if (info->state == AXIS_REQUEST_STATE_DECLINED)
		view->reason = info->decline_reason;
	view->mine = info->requested_by == viewer ? "made" : "followed";
	view->asked_at = info->created_us / G_USEC_PER_SEC;
	view->changed_at = (info->decided_us != 0 ? info->decided_us : info->created_us) / G_USEC_PER_SEC;
}

static void
write_quota(
	AxisPortalWriter       *w,
	const AxisRequestQuota *quota
){
	AxisPortalAllowance movies;
	AxisPortalAllowance seasons;

	movies.limit = quota->movies.limit;
	movies.left = quota->movies.left;
	movies.window_ends = quota->end_us / G_USEC_PER_SEC;
	seasons.limit = quota->seasons.limit;
	seasons.left = quota->seasons.left;
	seasons.window_ends = quota->end_us / G_USEC_PER_SEC;
	axis_portal_write_allowance(w, "movies", &movies);
	axis_portal_write_allowance(w, "seasons", &seasons);
}

/* "{1,2,3}" of gint64 values. */
static gchar *
id_array_text(const GArray *ids)
{
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; i < ids->len; i++)
		g_string_append_printf(text, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(text, '}');
	return g_string_free(text, FALSE);
}

/*
 * Which of @ids are titles their provider marks as adult: none of them is
 * ever in an answer, however it came to be stored and whatever the search's
 * own setting says.  Read from the record itself, here, for every answer.
 */
static gboolean
adult_titles(
	AxisPortalApi  *self,
	const GArray   *ids,
	GHashTable    **adult,
	GError        **error
){
	g_autofree gchar *array = id_array_text(ids);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { array };
	guint i;

	*adult = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	if (ids->len == 0)
		return TRUE;
	if (!axis_db_pool_exec(self->pool, "SELECT id FROM meta_title WHERE id = ANY ($1::bigint[]) AND adult", 1, params,
	                       &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 *id = g_new0(gint64, 1);

		axis_db_result_get_int64(result, i, 0, id);
		g_hash_table_add(*adult, id);
	}
	return TRUE;
}

/*
 * Writes @titles (stored records, borrowed) as "titles", or with @single the
 * one title as "title", with its seasons when it is a series.  Titles marked
 * as adult are left out; *@written says how many were written.
 */
static gboolean
write_titles(
	Work              *work,
	AxisPortalWriter  *w,
	GPtrArray         *titles,
	gboolean           single,
	guint             *written,
	GError           **error
){
	AxisPortalApi *self = work->api;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GHashTable) states = NULL;
	g_autoptr(GHashTable) adult = NULL;
	g_autoptr(GPtrArray) record_seasons = NULL;
	g_autofree const gchar **providers = g_new0(const gchar *, titles->len + 1);
	g_autofree const gchar **refs = g_new0(const gchar *, titles->len + 1);
	g_autofree gchar **hashes = g_new0(gchar *, titles->len + 1);
	g_autofree AxisPortalTitle *out = g_new0(AxisPortalTitle, titles->len + 1);
	g_autofree AxisPortalSeason *seasons = NULL;
	gboolean show_who = axis_requests_show_who(self->requests);
	guint n = 0;
	guint i;

	*written = 0;
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		g_array_append_val(ids, title->id);
		providers[i] = title->provider;
		refs[i] = title->poster_ref;
	}
	if (!adult_titles(self, ids, &adult, error) ||
	    !axis_requests_title_states(self->requests, ids, &states, error))
		return FALSE;
	/* Whether the daemon holds each poster.  Nothing is fetched on a
	 * requester's account. */
	if (titles->len > 0)
		axis_meta_artwork_find_many(self->meta, providers, refs, titles->len, AXIS_META_ART_POSTER_SMALL, FALSE, hashes);

	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);
		const AxisRequestTitleState *state = g_hash_table_lookup(states, &title->id);
		AxisRequestTitleView view;
		AxisPortalTitle *item;

		if (g_hash_table_contains(adult, &title->id) || title->adult)
			continue;
		if (title->kind != AXIS_META_KIND_FILM && title->kind != AXIS_META_KIND_SERIES)
			continue;
		item = &out[n++];
		item->id = title->id;
		item->kind = kind_name(title->kind);
		item->name = title->title;
		item->year = title->year;
		item->overview = title->overview != NULL ? title->overview : "";
		item->has_poster = hashes[i] != NULL;
		item->state = title_state_name(state != NULL ? state->state : AXIS_REQUEST_STATE_CAN);
		/* A name only while the setting shows names to everyone who asks:
		 * with it off, no answer carries one. */
		if (state != NULL && show_who) {
			axis_request_title_view(state, work->user_id, FALSE, TRUE, &view);
			item->asked_by = view.asked_by;
		}
		if (single && title->kind == AXIS_META_KIND_SERIES) {
			g_autofree gchar *shown = NULL;
			guint k;
			guint j;

			if (!axis_meta_load_source_seasons(self->meta, title->id, NULL, &shown, NULL, NULL, &record_seasons, error))
				goto fail;
			item->with_seasons = TRUE;
			if (state != NULL && state->seasons != NULL) {
				seasons = g_new0(AxisPortalSeason, state->seasons->len + 1);
				for (k = 0; k < state->seasons->len; k++) {
					const AxisRequestSeasonState *s = &g_array_index(state->seasons, AxisRequestSeasonState, k);

					seasons[k].number = s->season;
					seasons[k].aired = (gint)s->aired;
					seasons[k].state = title_state_name(s->state);
					for (j = 0; record_seasons != NULL && j < record_seasons->len; j++) {
						const AxisMetaSeason *record = g_ptr_array_index(record_seasons, j);

						if (record->number == s->season)
							seasons[k].episodes = MAX(record->episode_count, 0);
					}
				}
				item->seasons = seasons;
				item->n_seasons = state->seasons->len;
			}
		}
	}
	if (single) {
		if (n > 0)
			axis_portal_write_title(w, "title", &out[0]);
	} else {
		axis_portal_write_titles(w, "titles", out, n);
	}
	*written = n;
	for (i = 0; i < titles->len; i++)
		g_free(hashes[i]);
	return TRUE;

fail:
	for (i = 0; i < titles->len; i++)
		g_free(hashes[i]);
	return FALSE;
}

/* The stored record of title @id, or %NULL when there is none (*@error set
 * only for a failure). */
static AxisMetaTitle *
load_title(
	AxisPortalApi  *self,
	gint64          id,
	GError        **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) titles = NULL;
	AxisMetaTitle *title;

	g_array_append_val(ids, id);
	if (!axis_meta_load_titles(self->meta, ids, &titles, error) || titles == NULL || titles->len == 0)
		return NULL;
	title = g_ptr_array_steal_index(titles, 0);
	return title;
}

/* ── The operations (worker threads) ────────────────────────────── */

/* The first provider that serves @kind and is able to answer (has its key),
 * as the engine's own search chooses it. */
static const AxisMetaProviderInfo *
ready_provider(
	AxisMeta     *meta,
	AxisMetaKind  kind
){
	const AxisMetaProviderInfo *found = NULL;
	guint i;

	for (i = 0; found == NULL && i < axis_meta_n_providers(meta); i++) {
		AxisMetaProviderStatus status;

		axis_meta_provider_status(meta, i, &status);
		if ((status.info->kinds & AXIS_META_KIND_BIT(kind)) != 0 && !status.info->secondary &&
		    (!status.needs_key || status.key_set))
			found = status.info;
		axis_meta_provider_status_clear(&status);
	}
	return found;
}

static void
op_hello(Work *work)
{
	AxisPortalApi *self = work->api;
	AxisPortalWriter *w = axis_portal_writer_new();
	const gchar *kinds[3] = { NULL, NULL, NULL };
	const gchar *lists[AXIS_META_N_LISTS + 1];
	guint n_kinds = 0;
	guint n_lists = 0;
	guint k;

	memset(lists, 0, sizeof(lists));
	for (k = 0; k < 2; k++) {
		AxisRequestKind kind = k == 0 ? AXIS_REQUEST_MOVIE : AXIS_REQUEST_SERIES;
		AxisMetaKind meta_kind = k == 0 ? AXIS_META_KIND_FILM : AXIS_META_KIND_SERIES;
		const AxisMetaProviderInfo *provider;
		AxisMetaList offered[AXIS_META_N_LISTS];
		guint n;
		guint i;
		guint j;

		if (!axis_requests_kind_on(self->requests, kind))
			continue;
		kinds[n_kinds++] = axis_request_kind_name(kind);
		provider = self->discover != NULL ? axis_discover_provider(self->discover, meta_kind, NULL) : NULL;
		n = provider != NULL ? axis_discover_lists(provider, meta_kind, offered) : 0;
		for (i = 0; i < n; i++) {
			const gchar *name = axis_meta_list_to_string(offered[i]);
			gboolean known = FALSE;

			/* What is recommended is worked out from what the library
			 * holds: not a list for people who only ask. */
			if (offered[i] == AXIS_META_LIST_RECOMMENDED || name == NULL)
				continue;
			for (j = 0; j < n_lists; j++)
				known = known || strcmp(lists[j], name) == 0;
			if (!known && n_lists < AXIS_META_N_LISTS)
				lists[n_lists++] = name;
		}
	}
	axis_portal_write_int(w, "protocol", AXIS_PORTAL_PROTOCOL);
	axis_portal_write_strings(w, "kinds", kinds);
	axis_portal_write_strings(w, "lists", lists);
	axis_portal_write_boolean(w, "show_names", axis_requests_show_who(self->requests));
	reply_json(work, 200, axis_portal_writer_finish(w));
}

static void
op_whoami(Work *work)
{
	g_autoptr(GError) error = NULL;
	AxisRequestQuota quota;
	AxisPortalWriter *w;

	if (!axis_requests_get_quota(work->api->requests, work->user_id, &quota, &error)) {
		/* The account went between the proof's check and this: no one. */
		if (g_error_matches(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND))
			reply_error(work, 401, "proof", "a valid session proof is required", 0);
		else
			reply_busy(work, error);
		return;
	}
	w = axis_portal_writer_new();
	axis_portal_write_string(w, "name", work->user_name);
	write_quota(w, &quota);
	reply_json(work, 200, axis_portal_writer_finish(w));
}

/* The key a search is kept under: its kind, its page and what was typed, as
 * the metadata layer compares searches. */
static gchar *
cache_key(
	guint        kind,
	guint        page,
	const gchar *query
){
	g_autofree gchar *folded = axis_meta_query_key(query);

	return folded != NULL ? g_strdup_printf("%u|%u|%s", kind, page, folded) : NULL;
}

static void
cache_put(
	AxisPortalApi *self,
	gchar         *key,
	GArray        *ids,
	gboolean       more
){
	Cached *cached = g_new0(Cached, 1);
	gint64 now = now_monotonic(self);

	cached->expires_us = now + (gint64)AXIS_PORTAL_SEARCH_CACHE_S * G_USEC_PER_SEC;
	cached->ids = g_array_ref(ids);
	cached->more = more;
	g_mutex_lock(&self->cache_lock);
	/* Bounded: what has ended goes first, then everything. */
	if (g_hash_table_size(self->cache) >= AXIS_PORTAL_SEARCH_CACHE_MAX) {
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, self->cache);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			if (((const Cached *)value)->expires_us <= now)
				g_hash_table_iter_remove(&iter);
		}
		if (g_hash_table_size(self->cache) >= AXIS_PORTAL_SEARCH_CACHE_MAX)
			g_hash_table_remove_all(self->cache);
	}
	g_hash_table_replace(self->cache, key, cached);
	g_mutex_unlock(&self->cache_lock);
}

/* What the memory holds of a search, or %NULL. */
static GArray *
cache_get(
	AxisPortalApi *self,
	const gchar   *key,
	gboolean      *more
){
	GArray *ids = NULL;
	const Cached *cached;

	g_mutex_lock(&self->cache_lock);
	cached = g_hash_table_lookup(self->cache, key);
	if (cached != NULL && cached->expires_us > now_monotonic(self)) {
		ids = g_array_ref(cached->ids);
		*more = cached->more;
	}
	g_mutex_unlock(&self->cache_lock);
	return ids;
}

static void
op_search(Work *work)
{
	AxisPortalApi *self = work->api;
	g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	g_autoptr(GError) error = NULL;
	AxisPortalWriter *w;
	gboolean stale = FALSE;
	gboolean more = FALSE;
	guint written = 0;
	guint k;

	for (k = 0; k < 2; k++) {
		AxisMetaKind kind = k == 0 ? AXIS_META_KIND_FILM : AXIS_META_KIND_SERIES;
		const AxisMetaProviderInfo *provider;
		AxisMetaSearchAnswer answer;
		g_autoptr(GArray) ids = NULL;
		guint i;

		if (!work->asked[k])
			continue;
		/* Asked within the last ten minutes, by anyone: from the store. */
		if (work->cached[k] != NULL) {
			g_autoptr(GPtrArray) titles = NULL;

			if (!axis_meta_load_titles(self->meta, work->cached[k], &titles, &error)) {
				reply_busy(work, error);
				return;
			}
			more = more || work->cached_more[k];
			while (titles != NULL && titles->len > 0)
				g_ptr_array_add(all, g_ptr_array_steal_index(titles, 0));
			continue;
		}
		provider = ready_provider(self->meta, kind);
		if (provider == NULL) {
			stale = TRUE;
			continue;
		}
		memset(&answer, 0, sizeof(answer));
		if (!axis_meta_search_page(self->meta, provider->name, kind, work->req->query, 0, work->req->page,
		                           AXIS_META_DEFAULT_WAIT_MS, &answer, &error)) {
			if (g_error_matches(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID)) {
				reply_error(work, 422, "invalid", "there is nothing to look for in the query", 0);
			} else {
				reply_busy(work, error);
			}
			return;
		}
		ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		for (i = 0; answer.titles != NULL && i < answer.titles->len; i++)
			g_array_append_val(ids, ((const AxisMetaTitle *)g_ptr_array_index(answer.titles, i))->id);
		/* The provider did not answer: what was saved before, marked. */
		if (answer.saved || answer.stale) {
			stale = TRUE;
		} else {
			gchar *key = cache_key(k, work->req->page, work->req->query);

			if (key != NULL)
				cache_put(self, key, ids, work->req->page == 1 && ids->len >= AXIS_PORTAL_TITLES_A_PAGE);
		}
		more = more || (work->req->page == 1 && ids->len >= AXIS_PORTAL_TITLES_A_PAGE);
		while (answer.titles != NULL && answer.titles->len > 0)
			g_ptr_array_add(all, g_ptr_array_steal_index(answer.titles, 0));
		axis_meta_search_answer_clear(&answer);
	}

	w = axis_portal_writer_new();
	if (!write_titles(work, w, all, FALSE, &written, &error)) {
		g_free(axis_portal_writer_finish(w));
		reply_busy(work, error);
		return;
	}
	axis_portal_write_int(w, "page", work->req->page);
	axis_portal_write_boolean(w, "more", more);
	axis_portal_write_boolean(w, "stale", stale);
	reply_json(work, 200, axis_portal_writer_finish(w));
}

static void
op_browse(Work *work)
{
	AxisPortalApi *self = work->api;
	AxisMetaKind kind = work->req->kind == AXIS_PORTAL_KIND_SERIES ? AXIS_META_KIND_SERIES : AXIS_META_KIND_FILM;
	g_autoptr(GPtrArray) titles = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	AxisPortalWriter *w;
	AxisDiscoverQuery query;
	AxisDiscoverPage page;
	AxisMetaList list;
	gboolean more = FALSE;
	gboolean have = FALSE;
	gboolean called = FALSE;
	guint written = 0;
	guint i;

	/* Only the provider's own lists: what is recommended is worked out from
	 * what the library holds. */
	if (!axis_meta_list_from_string(work->req->list, &list) || list == AXIS_META_LIST_RECOMMENDED) {
		reply_error(work, 422, "invalid", "there is no such list", 0);
		return;
	}
	memset(&page, 0, sizeof(page));
	memset(&query, 0, sizeof(query));
	/* The stored snapshot, and nothing else: the provider is never asked. */
	if (self->discover != NULL &&
	    axis_requests_kind_on(self->requests, (AxisRequestKind)work->req->kind)) {
		query.kind = kind;
		query.list = list;
		query.offset = (work->req->page - 1) * AXIS_PORTAL_TITLES_A_PAGE;
		query.limit = AXIS_PORTAL_TITLES_A_PAGE;
		called = TRUE;
		if (axis_discover_read(self->discover, &query, &page, &error)) {
			have = TRUE;
		} else if (!g_error_matches(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_INVALID)) {
			reply_busy(work, error);
			axis_discover_page_clear(&page);
			return;
		}
		/* (A list the provider does not offer for the kind holds nothing.) */
		g_clear_error(&error);
	}
	for (i = 0; have && page.items != NULL && i < page.items->len; i++)
		g_ptr_array_add(titles, ((const AxisDiscoverItem *)g_ptr_array_index(page.items, i))->title);
	if (have)
		more = query.offset + titles->len < page.total;

	w = axis_portal_writer_new();
	if (!write_titles(work, w, titles, FALSE, &written, &error)) {
		g_free(axis_portal_writer_finish(w));
		reply_busy(work, error);
	} else {
		axis_portal_write_int(w, "page", work->req->page);
		axis_portal_write_boolean(w, "more", more);
		reply_json(work, 200, axis_portal_writer_finish(w));
	}
	if (called)
		axis_discover_page_clear(&page);
}

static void
op_title(Work *work)
{
	g_autoptr(GPtrArray) titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	g_autoptr(GError) error = NULL;
	AxisMetaTitle *title = load_title(work->api, work->req->id, &error);
	AxisPortalWriter *w;
	guint written = 0;

	if (title == NULL) {
		if (error != NULL)
			reply_busy(work, error);
		else
			reply_not_found(work);
		return;
	}
	g_ptr_array_add(titles, title);
	w = axis_portal_writer_new();
	if (!write_titles(work, w, titles, TRUE, &written, &error)) {
		g_free(axis_portal_writer_finish(w));
		reply_busy(work, error);
		return;
	}
	/* A title the adult filter excludes is one that does not exist. */
	if (written == 0) {
		g_free(axis_portal_writer_finish(w));
		reply_not_found(work);
		return;
	}
	reply_json(work, 200, axis_portal_writer_finish(w));
}

static void
op_art(Work *work)
{
	AxisPortalApi *self = work->api;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GHashTable) adult = NULL;
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *type = NULL;
	GBytes *data = NULL;

	title = load_title(self, work->req->id, &error);
	if (title == NULL && error != NULL) {
		reply_busy(work, error);
		return;
	}
	g_array_append_val(ids, work->req->id);
	if (title != NULL && !adult_titles(self, ids, &adult, &error)) {
		reply_busy(work, error);
		return;
	}
	if (title == NULL || title->adult || g_hash_table_contains(adult, &work->req->id) || title->poster_ref == NULL) {
		reply_not_found(work);
		return;
	}
	/* From the artwork the daemon holds, by the title: nothing is fetched,
	 * and no address, file name or hash is taken from the caller. */
	hash = axis_meta_artwork_find(self->meta, title->provider, title->poster_ref, AXIS_META_ART_POSTER_SMALL, FALSE);
	if (hash == NULL || !axis_meta_artwork_read(self->meta, hash, &data, &type, NULL) || data == NULL) {
		reply_not_found(work);
		return;
	}
	if (g_bytes_get_size(data) > AXIS_PORTAL_ART_MAX_BYTES || type == NULL ||
	    (strcmp(type, "image/jpeg") != 0 && strcmp(type, "image/png") != 0 && strcmp(type, "image/webp") != 0)) {
		g_bytes_unref(data);
		reply_not_found(work);
		return;
	}
	work->status = 200;
	work->bytes = data;
	work->content_type = g_steal_pointer(&type);
}

static void
op_request(Work *work)
{
	AxisPortalApi *self = work->api;
	const AxisPortalRequest *req = work->req;
	g_autoptr(AxisRequestInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisPortalRequestView view;
	AxisRequestMade made;
	AxisPortalWriter *w;
	AxisRequestAsk ask;
	const gchar *outcome;

	memset(&ask, 0, sizeof(ask));
	/* Who asks is the proof's person, and the request came through here:
	 * neither is the caller's to say. */
	ask.user_id = work->user_id;
	ask.portal = TRUE;
	ask.title_id = req->id;
	ask.kind = (AxisRequestKind)req->kind;
	if (req->kind == AXIS_PORTAL_KIND_SERIES) {
		ask.seasons = (AxisRequestSeasons)req->seasons;
		ask.picked = req->picked;
		ask.n_picked = req->n_picked;
	}
	if (!axis_requests_ask(self->requests, &ask, &made, &error)) {
		axis_request_made_clear(&made);
		reply_requests_error(work, error);
		return;
	}
	switch (made.outcome) {
	case AXIS_REQUEST_MADE:
		outcome = "made";
		break;
	case AXIS_REQUEST_FOLLOWED:
		outcome = "followed";
		break;
	case AXIS_REQUEST_ALREADY_AVAILABLE:
		outcome = "available";
		break;
	default:
		outcome = "unchanged";
		break;
	}
	w = axis_portal_writer_new();
	axis_portal_write_string(w, "outcome", outcome);
	if (made.public_id != NULL && axis_requests_get(self->requests, made.public_id, work->user_id, FALSE, &info, NULL) &&
	    info != NULL) {
		fill_view(info, work->user_id, &view);
		axis_portal_write_request(w, "request", &view);
	}
	axis_portal_write_left_out(w, "left_out", (const gint *)(gpointer)made.left_available->data,
		made.left_available->len, (const gint *)(gpointer)made.left_requested->data, made.left_requested->len);
	axis_portal_write_int(w, "followed", made.n_followed);
	write_quota(w, &made.quota);
	g_message("portal API: request by %s for title %" G_GINT64_FORMAT ": %s", work->user_name, req->id, outcome);
	axis_request_made_clear(&made);
	reply_json(work, 200, axis_portal_writer_finish(w));
}

static void
op_requests(Work *work)
{
	AxisPortalApi *self = work->api;
	guint page = work->req->page;
	guint first = (page - 1) * AXIS_PORTAL_REQUESTS_A_PAGE;
	guint limit = MIN(page * AXIS_PORTAL_REQUESTS_A_PAGE + 1, AXIS_REQUESTS_PAGE_MAX);
	g_autoptr(GPtrArray) requests = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree AxisPortalRequestView *views = NULL;
	AxisPortalWriter *w;
	guint n = 0;
	guint i;

	/* The caller's, made and followed: the layer lists no other, and no
	 * member of the body could name another person. */
	if (first >= AXIS_REQUESTS_PAGE_MAX) {
		requests = g_ptr_array_new();
	} else if (!axis_requests_list_mine(self->requests, work->user_id, limit, &requests, &error)) {
		reply_requests_error(work, error);
		return;
	}
	views = g_new0(AxisPortalRequestView, AXIS_PORTAL_REQUESTS_A_PAGE + 1);
	for (i = first; i < requests->len && n < AXIS_PORTAL_REQUESTS_A_PAGE; i++)
		fill_view(g_ptr_array_index(requests, i), work->user_id, &views[n++]);
	w = axis_portal_writer_new();
	axis_portal_write_requests(w, "requests", views, n);
	axis_portal_write_int(w, "page", page);
	axis_portal_write_boolean(w, "more", requests->len > first + n);
	reply_json(work, 200, axis_portal_writer_finish(w));
}

static void
op_withdraw(Work *work)
{
	g_autoptr(GError) error = NULL;
	gboolean handed = FALSE;

	/* The layer answers a request the person did not make as one that does
	 * not exist: the same answer as for an identifier never issued. */
	if (!axis_requests_withdraw(work->api->requests, work->req->request, work->user_id, &handed, &error)) {
		reply_requests_error(work, error);
		return;
	}
	g_message("portal API: %s withdrew a request", work->user_name);
	reply_json(work, 200, g_strdup("{}"));
}

/* Writes down that the token was used, and from where (at most once a
 * minute for a token: axis_portal_tokens_note_use()). */
static void
note_token(Work *work)
{
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, work->token);
	g_autofree gchar *address = g_strndup(work->address != NULL && work->address[0] != '\0' ? work->address : "?", 64);
	g_autoptr(GError) error = NULL;
	const gchar *params[] = { id, address };

	if (!axis_db_pool_exec(work->api->pool, "UPDATE portal_token SET last_used_at = now(), last_address = $2"
	        " WHERE id = $1::bigint", 2, params, NULL, &error))
		g_debug("portal API: a token's use could not be written down: %s", error->message);
}

static void
work_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Work *work = task_data;

	if (work->note_token)
		note_token(work);
	switch (work->op) {
	case AXIS_PORTAL_HELLO:
		op_hello(work);
		break;
	case AXIS_PORTAL_WHOAMI:
		op_whoami(work);
		break;
	case AXIS_PORTAL_SEARCH:
		op_search(work);
		break;
	case AXIS_PORTAL_BROWSE:
		op_browse(work);
		break;
	case AXIS_PORTAL_TITLE:
		op_title(work);
		break;
	case AXIS_PORTAL_ART:
		op_art(work);
		break;
	case AXIS_PORTAL_REQUEST:
		op_request(work);
		break;
	case AXIS_PORTAL_REQUESTS:
		op_requests(work);
		break;
	case AXIS_PORTAL_WITHDRAW:
		op_withdraw(work);
		break;
	default:
		break;
	}
	if (work->body == NULL && work->bytes == NULL)
		reply_error(work, 500, "internal", "the request could not be answered", 0);
	g_task_return_boolean(task, TRUE);
}

static void
work_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Work *work = user_data;
	HtmxResponse *response;

	if (work->bytes != NULL) {
		response = htmx_response_new();
		htmx_response_set_status(response, 200);
		htmx_response_set_bytes(response, work->bytes);
		htmx_response_set_content_type(response, work->content_type);
		common_headers(response);
	} else {
		response = json_response(work->status, work->body);
		if (work->wait_s > 0) {
			g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT, work->wait_s);

			htmx_response_add_header(response, "Retry-After", seconds);
		}
	}
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	work->api->pending--;
	work_free(work);
}

/* ── The search budget (main loop) ──────────────────────────────── */

static void
times_trim(
	GQueue *times,
	gint64  now
){
	gint64 *oldest;

	while ((oldest = g_queue_peek_head(times)) != NULL && now - *oldest >= MINUTE_US)
		g_free(g_queue_pop_head(times));
}

/* How long until @n more fit under @limit a minute; 0: they fit now. */
static gint64
times_wait(
	GQueue *times,
	guint   limit,
	guint   n,
	gint64  now
){
	const gint64 *oldest;

	times_trim(times, now);
	if (g_queue_get_length(times) + n <= limit)
		return 0;
	oldest = g_queue_peek_head(times);
	return oldest != NULL ? MAX(*oldest + MINUTE_US - now, G_USEC_PER_SEC) : MINUTE_US;
}

static void
times_add(
	GQueue *times,
	guint   n,
	gint64  now
){
	guint i;

	for (i = 0; i < n; i++) {
		gint64 *when = g_new(gint64, 1);

		*when = now;
		g_queue_push_tail(times, when);
	}
}

static void
times_free(gpointer data)
{
	g_queue_free_full(data, g_free);
}

/*
 * A search is about to be made for @work's person: what the memory holds is
 * taken, and the budgets are counted.  Returns: %NULL to go ahead, else the
 * refusal.
 */
static HtmxResponse *
search_admit(
	AxisPortalApi *self,
	Work          *work
){
	gint64 now = now_monotonic(self);
	GQueue *mine = g_hash_table_lookup(self->by_person, &work->user_id);
	guint to_ask = 0;
	gint64 wait;
	guint k;

	if (mine == NULL) {
		gint64 *key = g_new(gint64, 1);

		/* Bounded by the accounts; those with nothing in the last minute
		 * are dropped when the table has grown. */
		if (g_hash_table_size(self->by_person) >= 4096)
			g_hash_table_remove_all(self->by_person);
		*key = work->user_id;
		mine = g_queue_new();
		g_hash_table_insert(self->by_person, key, mine);
	}
	/* Each person's own: every search counts, answered from memory or not. */
	wait = times_wait(mine, AXIS_PORTAL_SEARCHES_A_MINUTE, 1, now);
	if (wait > 0)
		return refusal(429, "search-limit", "too many searches for this person; wait",
			(wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);

	for (k = 0; k < 2; k++) {
		g_autofree gchar *key = NULL;

		if (!work->asked[k])
			continue;
		key = cache_key(k, work->req->page, work->req->query);
		work->cached[k] = key != NULL ? cache_get(self, key, &work->cached_more[k]) : NULL;
		if (work->cached[k] == NULL)
			to_ask++;
	}
	/* In all: what asks the provider. */
	wait = to_ask > 0 ? times_wait(&self->searches, AXIS_PORTAL_SEARCHES_IN_ALL, to_ask, now) : 0;
	if (wait > 0) {
		g_message("portal API: the search budget of %d a minute is used up; a search was refused",
			AXIS_PORTAL_SEARCHES_IN_ALL);
		return refusal(429, "search-limit", "too many searches through the portal; wait",
			(wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);
	}
	times_add(mine, 1, now);
	times_add(&self->searches, to_ask, now);
	return NULL;
}

/* ── The login (main loop) ──────────────────────────────────────── */

typedef struct
{
	AxisPortalApi *api;
	HtmxResponse  *deferred;
} LoginContext;

static HtmxResponse *
login_response(const AxisPortalLoginResult *result)
{
	AxisPortalWriter *w;
	g_autofree gchar *body = NULL;

	switch (result->status) {
	case AXIS_PORTAL_LOGIN_OK:
		w = axis_portal_writer_new();
		axis_portal_write_string(w, "proof", result->proof);
		axis_portal_write_string(w, "name", result->name);
		axis_portal_write_int(w, "expires_in", result->expires_in);
		axis_portal_write_string(w, "device", result->device);
		body = axis_portal_writer_finish(w);
		return json_response(200, body);
	case AXIS_PORTAL_LOGIN_THROTTLED:
		return refusal(429, "throttled", "too many failed logins; wait", MAX(result->wait_s, 1));
	case AXIS_PORTAL_LOGIN_BUSY:
		return busy();
	case AXIS_PORTAL_LOGIN_REFUSED:
	default:
		/* One answer, whatever it was that failed. */
		return refusal(401, "login", "the username or password is incorrect", 0);
	}
}

static void
on_login_done(
	const AxisPortalLoginResult *result,
	gpointer                     user_data
){
	LoginContext *context = user_data;
	HtmxResponse *response = login_response(result);

	htmx_response_complete(context->deferred, response);
	g_object_unref(response);
	g_object_unref(context->deferred);
	context->api->pending--;
	g_free(context);
}

static HtmxResponse *
handle_login(
	AxisPortalApi           *self,
	const AxisPortalRequest *req
){
	LoginContext *context = g_new0(LoginContext, 1);
	AxisPortalLoginResult at_once;
	HtmxResponse *returned;

	context->api = self;
	context->deferred = htmx_response_new_deferred();
	returned = g_object_ref(context->deferred);
	self->pending++;
	if (axis_web_auth_portal_login(self->auth, req->username, req->password, req->device, req->client_address,
	                               req->user_agent, &at_once, on_login_done, context))
		return returned;
	/* Answered at once: throttled, or too many wait. */
	self->pending--;
	g_object_unref(returned);
	g_object_unref(context->deferred);
	g_free(context);
	return login_response(&at_once);
}

/* ── Dispatch (main loop) ───────────────────────────────────────── */

static HtmxResponse *
handle_operation(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisPortalApi *self = user_data;
	const Gate *gate = g_object_get_data(G_OBJECT(request), GATE_KEY);
	const gchar *path = htmx_request_get_path(request);
	g_autoptr(AxisPortalRequest) req = NULL;
	g_autofree gchar *name = NULL;
	HtmxResponse *refused;
	AxisPortalOp op;
	GBytes *body;
	const gchar *data = NULL;
	gsize length = 0;
	gint64 user_id = 0;
	GTask *task;
	Work *work;

	/* Only what the gate let through, as the operation the path names. */
	if (gate == NULL || gate->token == 0)
		return refusal(401, "unauthorized", "a portal token is required", 0);
	if (!under_prefix(path) || !axis_portal_op_from_name(path + strlen(AXIS_PORTAL_API_PREFIX), &op))
		return refusal(404, "not-found", "there is no such operation", 0);
	if (self->stopping || self->pending >= AXIS_PORTAL_API_MAX_PENDING)
		return busy();

	body = htmx_request_get_body_bytes(request);
	if (body != NULL)
		data = g_bytes_get_data(body, &length);
	req = axis_portal_request_parse(op, data, length, NULL);
	if (req == NULL)
		return refusal(422, "malformed", "the body is not one JSON object within the limits", 0);

	/* The person, from the proof and from nothing else, before anything
	 * else of the body is judged. */
	if (axis_portal_op_needs_proof(op) && !axis_web_auth_portal_person(self->auth, req->proof, &user_id, &name))
		return no_proof();
	if (!req->valid)
		return refusal(422, "invalid", req->problem != NULL ? req->problem : "the body is not the operation's", 0);

	if (op == AXIS_PORTAL_LOGIN)
		return handle_login(self, req);
	if (op == AXIS_PORTAL_LOGOUT) {
		if (!axis_web_auth_portal_logout(self->auth, req->proof))
			return no_proof();
		return json_response(200, "{}");
	}

	work = g_new0(Work, 1);
	work->api = self;
	work->op = op;
	work->user_id = user_id;
	work->user_name = g_steal_pointer(&name);
	work->token = gate->token;
	work->address = g_strdup(gate->address);
	if (op == AXIS_PORTAL_SEARCH) {
		work->asked[0] = !req->has_kind || req->kind == AXIS_PORTAL_KIND_MOVIE;
		work->asked[1] = !req->has_kind || req->kind == AXIS_PORTAL_KIND_SERIES;
		work->asked[0] = work->asked[0] && axis_requests_kind_on(self->requests, AXIS_REQUEST_MOVIE);
		work->asked[1] = work->asked[1] && axis_requests_kind_on(self->requests, AXIS_REQUEST_SERIES);
		work->req = g_steal_pointer(&req);
		refused = search_admit(self, work);
		if (refused != NULL) {
			work_free(work);
			return refused;
		}
	} else {
		work->req = g_steal_pointer(&req);
	}
	work->note_token = axis_portal_tokens_note_use(self->tokens, gate->token, gate->address, g_get_monotonic_time());
	work->deferred = htmx_response_new_deferred();
	refused = g_object_ref(work->deferred);

	self->pending++;
	task = g_task_new(NULL, NULL, work_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, work_thread);
	g_object_unref(task);
	return refused;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisPortalApi *
axis_portal_api_new(
	AxisDbPool       *pool,
	AxisWebAuth      *auth,
	AxisPortalTokens *tokens,
	AxisRequests     *requests,
	AxisMeta         *meta,
	AxisDiscover     *discover
){
	AxisPortalApi *self;

	g_return_val_if_fail(pool != NULL && auth != NULL && tokens != NULL && requests != NULL && meta != NULL, NULL);

	self = g_new0(AxisPortalApi, 1);
	self->pool = pool;
	self->auth = auth;
	self->tokens = tokens;
	self->requests = axis_requests_ref(requests);
	self->meta = axis_meta_ref(meta);
	self->discover = discover != NULL ? axis_discover_ref(discover) : NULL;
	self->by_address = axis_login_throttle_new(AXIS_PORTAL_TOKEN_FREE_FAILURES, THROTTLE_BASE_US, THROTTLE_MAX_US,
		THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	g_queue_init(&self->searches);
	self->by_person = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, times_free);
	g_mutex_init(&self->cache_lock);
	self->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_free);
	return self;
}

void
axis_portal_api_install(
	AxisPortalApi *self,
	HtmxServer    *server,
	guint          max_connections
){
	static const gchar too_large[] = "{\"error\":\"too-large\",\"message\":\"the body is larger than the portal API "
		"takes\"}";
	AxisWebLimitsOptions options;
	HtmxRouter *router;
	guint i;

	g_return_if_fail(self != NULL && HTMX_IS_SERVER(server));

	memset(&options, 0, sizeof(options));
	options.max_connections = max_connections;
	options.max_body_bytes = AXIS_PORTAL_MAX_REQUEST_BYTES;
	options.too_large_json = too_large;
	axis_web_limits_install(server, &options);

	htmx_server_use(server, portal_gate, self, NULL);
	/* The router of this listener: the eleven operations, and nothing else. */
	router = htmx_server_get_router(server);
	for (i = 0; operations[i] != NULL; i++) {
		g_autofree gchar *pattern = g_strconcat(AXIS_PORTAL_API_PREFIX, operations[i], NULL);

		htmx_router_add_route(router, HTMX_METHOD_POST, pattern, handle_operation, self);
	}
}

void
axis_portal_api_set_serving(
	AxisPortalApi            *self,
	AxisPortalApiServingFunc  func,
	gpointer                  user_data
){
	g_return_if_fail(self != NULL);

	self->serving = func;
	self->serving_data = user_data;
}

void
axis_portal_api_stop(AxisPortalApi *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_portal_api_free(AxisPortalApi *self)
{
	if (self == NULL)
		return;
	axis_portal_api_stop(self);
	axis_login_throttle_free(self->by_address);
	g_queue_clear_full(&self->searches, g_free);
	g_hash_table_unref(self->by_person);
	g_hash_table_unref(self->cache);
	g_mutex_clear(&self->cache_lock);
	axis_requests_unref(self->requests);
	axis_meta_unref(self->meta);
	g_clear_pointer(&self->discover, axis_discover_unref);
	g_free(self);
}

void
axis_portal_api_set_time_offset(
	AxisPortalApi *self,
	gint64         offset_us
){
	g_return_if_fail(self != NULL);

	self->time_offset_us = offset_us;
}
