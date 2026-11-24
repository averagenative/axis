/*
 * axis-portal-proto.c - the portal API's messages: the one reader of an
 * operation's body and the one writer of its answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-portal-proto.h"

#include <string.h>

#include "core/axis-proxy.h"
#include "probe/axis-probe-json.h"

static const gchar *const op_names[AXIS_PORTAL_N_OPS] = {
	"hello", "login", "logout", "whoami", "search", "browse", "title", "art", "request", "requests", "withdraw"
};

const gchar *
axis_portal_op_name(AxisPortalOp op)
{
	return (guint)op < AXIS_PORTAL_N_OPS ? op_names[op] : "?";
}

gboolean
axis_portal_op_from_name(
	const gchar  *name,
	AxisPortalOp *op
){
	guint i;

	for (i = 0; name != NULL && i < AXIS_PORTAL_N_OPS; i++) {
		if (strcmp(name, op_names[i]) == 0) {
			*op = (AxisPortalOp)i;
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
axis_portal_op_needs_proof(AxisPortalOp op)
{
	return op != AXIS_PORTAL_HELLO && op != AXIS_PORTAL_LOGIN;
}

/* ── Reading ────────────────────────────────────────────────────── */

/* The members each operation defines; those before @required are required. */
typedef struct
{
	const gchar *members[6];
	guint        required;
} OpMembers;

static const OpMembers op_members[AXIS_PORTAL_N_OPS] = {
	/* hello */    { { NULL }, 0 },
	/* login */    { { "username", "password", "client_address", "user_agent", "device", NULL }, 4 },
	/* logout */   { { "proof", NULL }, 0 },
	/* whoami */   { { "proof", NULL }, 0 },
	/* search */   { { "query", "proof", "kind", "page", NULL }, 1 },
	/* browse */   { { "list", "kind", "proof", "page", NULL }, 2 },
	/* title */    { { "id", "proof", NULL }, 1 },
	/* art */      { { "id", "proof", NULL }, 1 },
	/* request */  { { "id", "kind", "proof", "seasons", NULL }, 2 },
	/* requests */ { { "proof", "page", NULL }, 0 },
	/* withdraw */ { { "request", "proof", NULL }, 1 }
};

static gboolean
refuse(
	AxisPortalRequest *request,
	const gchar       *text
){
	if (request->problem == NULL)
		request->problem = g_strdup(text);
	return FALSE;
}

/* Whether every character of @text is of [A-Za-z0-9_-] (@dots: or '.'). */
static gboolean
token_chars(
	const gchar *text,
	gboolean     dots
){
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && !(dots && *p == '.'))
			return FALSE;
	}
	return TRUE;
}

static gboolean
has_control(const gchar *text)
{
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f || g_unichar_iscntrl(c))
			return TRUE;
	}
	return FALSE;
}

/* A string member of 1 to @max_bytes bytes; %NULL (refused) for anything
 * else.  The reader has made it valid UTF-8 without NUL. */
static const gchar *
string_member(
	AxisPortalRequest  *request,
	const AxisJsonNode *root,
	const gchar        *name,
	gsize               max_bytes
){
	const AxisJsonNode *node = axis_json_member(root, name);
	const gchar *text;
	gsize length;

	if (axis_json_type(node) != AXIS_JSON_STRING) {
		refuse(request, "a member that must be a text is not one");
		return NULL;
	}
	text = axis_json_string(node);
	length = text != NULL ? strlen(text) : 0;
	if (length == 0 || length > max_bytes) {
		refuse(request, "a text is empty or longer than it may be");
		return NULL;
	}
	return text;
}

static gboolean
int_member(
	AxisPortalRequest  *request,
	const AxisJsonNode *root,
	const gchar        *name,
	gint64              minimum,
	gint64              maximum,
	gint64             *value
){
	const AxisJsonNode *node = axis_json_member(root, name);

	/* Only a number: a text that holds one is not. */
	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_integer(node, minimum, maximum, value))
		return refuse(request, "a member that must be a whole number in its range is not one");
	return TRUE;
}

static gboolean
kind_member(
	AxisPortalRequest  *request,
	const AxisJsonNode *root
){
	const gchar *text = string_member(request, root, "kind", 8);

	if (text == NULL)
		return FALSE;
	if (strcmp(text, "movie") == 0)
		request->kind = AXIS_PORTAL_KIND_MOVIE;
	else if (strcmp(text, "series") == 0)
		request->kind = AXIS_PORTAL_KIND_SERIES;
	else
		return refuse(request, "the kind is neither movie nor series");
	request->has_kind = TRUE;
	return TRUE;
}

static gboolean
seasons_member(
	AxisPortalRequest  *request,
	const AxisJsonNode *root
){
	const AxisJsonNode *node = axis_json_member(root, "seasons");
	guint i;
	guint j;

	if (axis_json_type(node) == AXIS_JSON_STRING) {
		const gchar *text = axis_json_string(node);

		if (g_strcmp0(text, "all") == 0)
			request->seasons = AXIS_PORTAL_SEASONS_ALL;
		else if (g_strcmp0(text, "first") == 0)
			request->seasons = AXIS_PORTAL_SEASONS_FIRST;
		else if (g_strcmp0(text, "latest") == 0)
			request->seasons = AXIS_PORTAL_SEASONS_LATEST;
		else
			return refuse(request, "the seasons are not all, first, latest or a list of numbers");
		return TRUE;
	}
	if (axis_json_type(node) != AXIS_JSON_ARRAY || axis_json_length(node) == 0 ||
	    axis_json_length(node) > AXIS_PORTAL_MAX_SEASONS)
		return refuse(request, "the seasons are not all, first, latest or a list of numbers");
	for (i = 0; i < axis_json_length(node); i++) {
		const AxisJsonNode *item = axis_json_element(node, i);
		gint64 number = 0;

		if (axis_json_type(item) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(item, 0, AXIS_PORTAL_MAX_SEASON_NUMBER, &number))
			return refuse(request, "a season is not a whole number in its range");
		for (j = 0; j < i; j++) {
			if (request->picked[j] == (gint)number)
				return refuse(request, "a season is listed twice");
		}
		request->picked[i] = (gint)number;
	}
	request->n_picked = axis_json_length(node);
	request->seasons = AXIS_PORTAL_SEASONS_PICKED;
	return TRUE;
}

/* The members of @root are the operation's: none it does not define, none
 * twice, every required one there. */
static gboolean
check_members(
	AxisPortalRequest  *request,
	const AxisJsonNode *root
){
	const OpMembers *defined = &op_members[request->op];
	guint n = axis_json_length(root);
	guint i;
	guint j;

	/* More members than the operation defines: one is unknown or repeated
	 * (so the pairwise check below only ever sees a few). */
	if (n > g_strv_length((gchar **)defined->members))
		return refuse(request, "the body has a member its operation does not define, or one twice");
	for (i = 0; i < n; i++) {
		const gchar *name = axis_json_member_name(root, i);

		if (!g_strv_contains(defined->members, name))
			return refuse(request, "the body has a member its operation does not define");
		for (j = 0; j < i; j++) {
			if (strcmp(name, axis_json_member_name(root, j)) == 0)
				return refuse(request, "the body has a member twice");
		}
	}
	for (i = 0; i < defined->required; i++) {
		if (axis_json_member(root, defined->members[i]) == NULL)
			return refuse(request, "the body lacks a member its operation requires");
	}
	return TRUE;
}

static gboolean
read_members(
	AxisPortalRequest  *request,
	const AxisJsonNode *root
){
	const gchar *text;
	gint64 number = 0;

	if (!check_members(request, root))
		return FALSE;
	/* A proof where one is defined must be a text; whether it is a proof is
	 * the daemon's to say (it was read before this). */
	if (axis_portal_op_needs_proof(request->op) && axis_json_member(root, "proof") != NULL &&
	    axis_json_type(axis_json_member(root, "proof")) != AXIS_JSON_STRING)
		return refuse(request, "a member that must be a text is not one");
	if (axis_json_member(root, "page") != NULL) {
		if (!int_member(request, root, "page", 1, AXIS_PORTAL_MAX_PAGE, &number))
			return FALSE;
		request->page = (guint)number;
	}

	switch (request->op) {
	case AXIS_PORTAL_LOGIN:
		if ((text = string_member(request, root, "username", AXIS_PORTAL_USERNAME_MAX_BYTES)) == NULL)
			return FALSE;
		request->username = g_strdup(text);
		if ((text = string_member(request, root, "password", AXIS_PORTAL_PASSWORD_MAX_BYTES)) == NULL)
			return FALSE;
		request->password = g_strdup(text);
		{
			AxisIpAddress address;

			if ((text = string_member(request, root, "client_address", AXIS_PORTAL_ADDRESS_MAX_BYTES)) == NULL)
				return FALSE;
			if (!axis_ip_address_parse(text, &address))
				return refuse(request, "the client address is not an address");
			request->client_address = g_strdup(text);
		}
		if ((text = string_member(request, root, "user_agent", AXIS_PORTAL_AGENT_MAX_BYTES)) == NULL)
			return FALSE;
		request->user_agent = g_strdup(text);
		if (axis_json_member(root, "device") != NULL) {
			if ((text = string_member(request, root, "device", AXIS_PORTAL_DEVICE_MAX_BYTES)) == NULL)
				return FALSE;
			if (!token_chars(text, TRUE))
				return refuse(request, "the device value is not one");
			request->device = g_strdup(text);
		}
		break;
	case AXIS_PORTAL_SEARCH:
		/* In bytes first (a character is at most four), then in characters. */
		if ((text = string_member(request, root, "query", AXIS_PORTAL_QUERY_MAX_CHARS * 4)) == NULL)
			return FALSE;
		if (g_utf8_strlen(text, -1) > AXIS_PORTAL_QUERY_MAX_CHARS)
			return refuse(request, "the query is longer than it may be");
		if (has_control(text))
			return refuse(request, "the query holds a control character");
		request->query = g_strdup(text);
		if (axis_json_member(root, "kind") != NULL && !kind_member(request, root))
			return FALSE;
		/* Refused here, before the search budgets count it. */
		if (request->page > AXIS_PORTAL_SEARCH_MAX_PAGE)
			return refuse(request, "a search has two pages at most");
		break;
	case AXIS_PORTAL_BROWSE:
		if ((text = string_member(request, root, "list", AXIS_PORTAL_LIST_MAX_BYTES)) == NULL)
			return FALSE;
		if (!token_chars(text, FALSE))
			return refuse(request, "the list is not one");
		request->list = g_strdup(text);
		if (!kind_member(request, root))
			return FALSE;
		break;
	case AXIS_PORTAL_TITLE:
	case AXIS_PORTAL_ART:
		if (!int_member(request, root, "id", 1, AXIS_PORTAL_MAX_ID, &request->id))
			return FALSE;
		break;
	case AXIS_PORTAL_REQUEST:
		if (!int_member(request, root, "id", 1, AXIS_PORTAL_MAX_ID, &request->id) || !kind_member(request, root))
			return FALSE;
		/* A series says which seasons; a movie has none to say. */
		if (request->kind == AXIS_PORTAL_KIND_SERIES) {
			if (axis_json_member(root, "seasons") == NULL)
				return refuse(request, "a request for a series lacks its seasons");
			if (!seasons_member(request, root))
				return FALSE;
		} else if (axis_json_member(root, "seasons") != NULL) {
			return refuse(request, "a request for a movie has no seasons");
		}
		break;
	case AXIS_PORTAL_WITHDRAW:
		if ((text = string_member(request, root, "request", AXIS_PORTAL_REQUEST_ID_LEN)) == NULL)
			return FALSE;
		if (strlen(text) != AXIS_PORTAL_REQUEST_ID_LEN || !token_chars(text, FALSE))
			return refuse(request, "the request's identifier is not one");
		request->request = g_strdup(text);
		break;
	default:
		break;
	}
	return TRUE;
}

/* Wipes and frees a text that was a secret. */
static void
secret_free(gchar *text)
{
	volatile gchar *p = text;

	if (text == NULL)
		return;
	while (*p != '\0')
		*p++ = '\0';
	g_free(text);
}

/* Drops what was read of a body that is not valid: nothing of it is used. */
static void
clear_members(AxisPortalRequest *request)
{
	g_clear_pointer(&request->username, g_free);
	g_clear_pointer(&request->password, secret_free);
	g_clear_pointer(&request->client_address, g_free);
	g_clear_pointer(&request->user_agent, g_free);
	g_clear_pointer(&request->device, g_free);
	g_clear_pointer(&request->query, g_free);
	g_clear_pointer(&request->list, g_free);
	g_clear_pointer(&request->request, g_free);
	request->has_kind = FALSE;
	request->kind = 0;
	request->page = 1;
	request->id = 0;
	request->seasons = -1;
	request->n_picked = 0;
}

AxisPortalRequest *
axis_portal_request_parse(
	AxisPortalOp   op,
	const gchar   *data,
	gsize          length,
	gchar        **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	AxisPortalRequest *request;

	if (problem != NULL)
		*problem = NULL;
	if ((guint)op >= AXIS_PORTAL_N_OPS || data == NULL || length == 0 || length > AXIS_PORTAL_MAX_REQUEST_BYTES) {
		if (problem != NULL)
			*problem = g_strdup("the body is empty or larger than it may be");
		return NULL;
	}
	doc = axis_json_parse(data, length, NULL);
	root = doc != NULL ? axis_json_doc_get_root(doc) : NULL;
	if (root == NULL || axis_json_type(root) != AXIS_JSON_OBJECT) {
		if (problem != NULL)
			*problem = g_strdup("the body is not one JSON object");
		return NULL;
	}

	request = g_new0(AxisPortalRequest, 1);
	request->op = op;
	request->page = 1;
	request->seasons = -1;
	/* The proof first, by itself: whether the body's other members are the
	 * operation's is told only to a caller the daemon knows. */
	if (axis_portal_op_needs_proof(op)) {
		const gchar *proof = axis_json_string(axis_json_member(root, "proof"));

		if (proof != NULL && strlen(proof) == AXIS_PORTAL_PROOF_LEN && token_chars(proof, FALSE))
			request->proof = g_strdup(proof);
	}
	request->valid = read_members(request, root);
	if (!request->valid)
		clear_members(request);
	return request;
}

void
axis_portal_request_free(AxisPortalRequest *request)
{
	if (request == NULL)
		return;
	clear_members(request);
	secret_free(request->proof);
	g_free(request->problem);
	g_free(request);
}

/* ── Writing ────────────────────────────────────────────────────── */

struct _AxisPortalWriter
{
	GString *out;
	gboolean first;		/* no member written yet at the level being written */
};

static void
w_text(
	GString     *out,
	const gchar *text
){
	const guchar *p;

	g_string_append_c(out, '"');
	for (p = (const guchar *)(text != NULL ? text : ""); *p != '\0'; p++) {
		switch (*p) {
		case '"':
			g_string_append(out, "\\\"");
			break;
		case '\\':
			g_string_append(out, "\\\\");
			break;
		case '\n':
			g_string_append(out, "\\n");
			break;
		case '\r':
			g_string_append(out, "\\r");
			break;
		case '\t':
			g_string_append(out, "\\t");
			break;
		default:
			/* '<', '>' and '&' too: an answer is never markup, and is
			 * written so that it cannot be read as any. */
			if (*p < 0x20 || *p == 0x7f || *p == '<' || *p == '>' || *p == '&')
				g_string_append_printf(out, "\\u%04x", (guint)*p);
			else
				g_string_append_c(out, (gchar)*p);
			break;
		}
	}
	g_string_append_c(out, '"');
}

/* A text that is valid UTF-8 (anything else is written as nothing). */
static void
w_string(
	GString     *out,
	const gchar *text
){
	w_text(out, text != NULL && g_utf8_validate(text, -1, NULL) ? text : "");
}

static void
w_key(
	AxisPortalWriter *w,
	const gchar      *name
){
	if (!w->first)
		g_string_append_c(w->out, ',');
	w->first = FALSE;
	w_text(w->out, name);
	g_string_append_c(w->out, ':');
}

AxisPortalWriter *
axis_portal_writer_new(void)
{
	AxisPortalWriter *w = g_new0(AxisPortalWriter, 1);

	w->out = g_string_new("{");
	w->first = TRUE;
	return w;
}

gchar *
axis_portal_writer_finish(AxisPortalWriter *w)
{
	gchar *json;

	g_string_append_c(w->out, '}');
	json = g_string_free(w->out, FALSE);
	g_free(w);
	return json;
}

void
axis_portal_write_string(
	AxisPortalWriter *w,
	const gchar      *name,
	const gchar      *value
){
	w_key(w, name);
	w_string(w->out, value);
}

void
axis_portal_write_int(
	AxisPortalWriter *w,
	const gchar      *name,
	gint64            value
){
	w_key(w, name);
	g_string_append_printf(w->out, "%" G_GINT64_FORMAT, value);
}

void
axis_portal_write_boolean(
	AxisPortalWriter *w,
	const gchar      *name,
	gboolean          value
){
	w_key(w, name);
	g_string_append(w->out, value ? "true" : "false");
}

void
axis_portal_write_strings(
	AxisPortalWriter   *w,
	const gchar        *name,
	const gchar *const *values
){
	guint i;

	w_key(w, name);
	g_string_append_c(w->out, '[');
	for (i = 0; values != NULL && values[i] != NULL; i++) {
		if (i > 0)
			g_string_append_c(w->out, ',');
		w_string(w->out, values[i]);
	}
	g_string_append_c(w->out, ']');
}

/* The members of a title, as an object, into @out. */
static void
title_object(
	GString               *out,
	const AxisPortalTitle *title
){
	g_autofree gchar *overview = NULL;
	guint i;

	if (title->overview != NULL && g_utf8_validate(title->overview, -1, NULL))
		overview = g_utf8_substring(title->overview, 0,
			MIN(g_utf8_strlen(title->overview, -1), AXIS_PORTAL_OVERVIEW_MAX_CHARS));
	g_string_append_printf(out, "{\"id\":%" G_GINT64_FORMAT ",\"kind\":", title->id);
	w_string(out, title->kind);
	g_string_append(out, ",\"name\":");
	w_string(out, title->name);
	g_string_append_printf(out, ",\"year\":%d,\"overview\":", title->year);
	w_string(out, overview);
	g_string_append_printf(out, ",\"has_poster\":%s,\"state\":", title->has_poster ? "true" : "false");
	w_string(out, title->state);
	if (title->asked_by != NULL) {
		g_string_append(out, ",\"asked_by\":");
		w_string(out, title->asked_by);
	}
	if (title->with_seasons) {
		g_string_append(out, ",\"seasons\":[");
		for (i = 0; i < title->n_seasons; i++) {
			const AxisPortalSeason *season = &title->seasons[i];

			g_string_append_printf(out, "%s{\"number\":%d,\"episodes\":%d,\"aired\":%d,\"state\":", i > 0 ? "," : "",
				season->number, season->episodes, season->aired);
			w_string(out, season->state);
			g_string_append_c(out, '}');
		}
		g_string_append_c(out, ']');
	}
	g_string_append_c(out, '}');
}

void
axis_portal_write_title(
	AxisPortalWriter      *w,
	const gchar           *name,
	const AxisPortalTitle *title
){
	w_key(w, name);
	title_object(w->out, title);
}

void
axis_portal_write_titles(
	AxisPortalWriter      *w,
	const gchar           *name,
	const AxisPortalTitle *titles,
	guint                  n
){
	guint i;

	w_key(w, name);
	g_string_append_c(w->out, '[');
	for (i = 0; i < n; i++) {
		if (i > 0)
			g_string_append_c(w->out, ',');
		title_object(w->out, &titles[i]);
	}
	g_string_append_c(w->out, ']');
}

static void
request_object(
	GString                     *out,
	const AxisPortalRequestView *request
){
	guint i;

	g_string_append(out, "{\"id\":");
	w_string(out, request->id);
	g_string_append_printf(out, ",\"title\":%" G_GINT64_FORMAT ",\"kind\":", request->title);
	w_string(out, request->kind);
	g_string_append(out, ",\"name\":");
	w_string(out, request->name);
	g_string_append_printf(out, ",\"year\":%d,\"seasons\":[", request->year);
	for (i = 0; i < request->n_seasons; i++)
		g_string_append_printf(out, "%s%d", i > 0 ? "," : "", request->seasons[i]);
	g_string_append(out, "],\"state\":");
	w_string(out, request->state);
	if (request->counts)
		g_string_append_printf(out, ",\"have\":%u,\"of\":%u", request->have, request->of);
	if (request->cause != NULL) {
		g_string_append(out, ",\"cause\":");
		w_string(out, request->cause);
	}
	if (request->reason != NULL) {
		g_string_append(out, ",\"reason\":");
		w_string(out, request->reason);
	}
	g_string_append(out, ",\"mine\":");
	w_string(out, request->mine);
	g_string_append_printf(out, ",\"asked_at\":%" G_GINT64_FORMAT ",\"changed_at\":%" G_GINT64_FORMAT "}",
		request->asked_at, request->changed_at);
}

void
axis_portal_write_request(
	AxisPortalWriter            *w,
	const gchar                 *name,
	const AxisPortalRequestView *request
){
	w_key(w, name);
	request_object(w->out, request);
}

void
axis_portal_write_requests(
	AxisPortalWriter            *w,
	const gchar                 *name,
	const AxisPortalRequestView *requests,
	guint                        n
){
	guint i;

	w_key(w, name);
	g_string_append_c(w->out, '[');
	for (i = 0; i < n; i++) {
		if (i > 0)
			g_string_append_c(w->out, ',');
		request_object(w->out, &requests[i]);
	}
	g_string_append_c(w->out, ']');
}

void
axis_portal_write_allowance(
	AxisPortalWriter          *w,
	const gchar               *name,
	const AxisPortalAllowance *allowance
){
	w_key(w, name);
	g_string_append_printf(w->out, "{\"limit\":%u,\"left\":%u,\"window_ends\":%" G_GINT64_FORMAT "}", allowance->limit,
		allowance->left, allowance->window_ends);
}

void
axis_portal_write_left_out(
	AxisPortalWriter *w,
	const gchar      *name,
	const gint       *available,
	guint             n_available,
	const gint       *requested,
	guint             n_requested
){
	guint i;

	w_key(w, name);
	g_string_append_c(w->out, '[');
	for (i = 0; i < n_available; i++)
		g_string_append_printf(w->out, "%s{\"season\":%d,\"why\":\"available\"}", i > 0 ? "," : "", available[i]);
	for (i = 0; i < n_requested; i++)
		g_string_append_printf(w->out, "%s{\"season\":%d,\"why\":\"requested\"}", i + n_available > 0 ? "," : "",
			requested[i]);
	g_string_append_c(w->out, ']');
}

gchar *
axis_portal_error_wait_to_json(
	const gchar *word,
	const gchar *message,
	gint64       wait_s
){
	AxisPortalWriter *w = axis_portal_writer_new();

	axis_portal_write_string(w, "error", word);
	axis_portal_write_string(w, "message", message);
	if (wait_s > 0)
		axis_portal_write_int(w, "wait", wait_s);
	return axis_portal_writer_finish(w);
}

gchar *
axis_portal_error_to_json(
	const gchar *word,
	const gchar *message
){
	return axis_portal_error_wait_to_json(word, message, 0);
}
