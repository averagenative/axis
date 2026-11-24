/*
 * fuzz-portal-body.c - libFuzzer target for the daemon's one reader of the
 * portal API's bodies (hello, login, logout, whoami, search, browse, title,
 * art, request, requests, withdraw)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the operation; the rest is the body.  Whatever it
 * is, the reader must return without crashing or allocating without bound,
 * and what it accepts must keep the promises of api/axis-portal-proto.h:
 *
 *  - a body larger than the limit, or one that is not a JSON object, yields
 *    nothing (not even a proof);
 *  - a proof is 43 characters of the token alphabet, or it is none, and
 *    hello and login never have one;
 *  - a body that is not valid has none of its members set;
 *  - in a valid body every text is within its length and alphabet, every
 *    number in its range, every season once, and nothing is set that the
 *    operation does not define;
 *  - a refusal is a short sentence that holds nothing of the body;
 *  - what the writer writes of any text is one line of ASCII JSON that the
 *    reader reads as the same text.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api/axis-portal-proto.h"
#include "core/axis-proxy.h"
#include "probe/axis-probe-json.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static gboolean
token_text(
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

static void
check_problem(const gchar *problem)
{
	const guchar *p;

	if (problem == NULL)
		return;
	if (strlen(problem) > 200)
		abort();
	/* Fixed words only: letters, spaces and a little punctuation. */
	for (p = (const guchar *)problem; *p != '\0'; p++) {
		if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '<' || *p == '>')
			abort();
	}
}

static void
check_unset(const AxisPortalRequest *r)
{
	if (r->username != NULL || r->password != NULL || r->client_address != NULL || r->user_agent != NULL ||
	    r->device != NULL || r->query != NULL || r->list != NULL || r->request != NULL || r->has_kind || r->id != 0 ||
	    r->n_picked != 0 || r->seasons != -1 || r->page != 1)
		abort();
}

static void
check_valid(const AxisPortalRequest *r)
{
	AxisIpAddress address;
	guint i;
	guint j;

	if (r->problem != NULL || r->page < 1 || r->page > AXIS_PORTAL_MAX_PAGE)
		abort();
	if (r->has_kind && r->kind != AXIS_PORTAL_KIND_MOVIE && r->kind != AXIS_PORTAL_KIND_SERIES)
		abort();
	/* Only what the operation defines. */
	if (r->op != AXIS_PORTAL_LOGIN && (r->username != NULL || r->password != NULL || r->client_address != NULL ||
	    r->user_agent != NULL || r->device != NULL))
		abort();
	if (r->op != AXIS_PORTAL_SEARCH && r->query != NULL)
		abort();
	if (r->op != AXIS_PORTAL_BROWSE && r->list != NULL)
		abort();
	if (r->op != AXIS_PORTAL_WITHDRAW && r->request != NULL)
		abort();
	if (r->op != AXIS_PORTAL_TITLE && r->op != AXIS_PORTAL_ART && r->op != AXIS_PORTAL_REQUEST && r->id != 0)
		abort();
	if (r->op != AXIS_PORTAL_REQUEST && (r->seasons != -1 || r->n_picked != 0))
		abort();
	if (r->op != AXIS_PORTAL_SEARCH && r->op != AXIS_PORTAL_BROWSE && r->op != AXIS_PORTAL_REQUEST && r->has_kind)
		abort();
	if (r->op != AXIS_PORTAL_SEARCH && r->op != AXIS_PORTAL_BROWSE && r->op != AXIS_PORTAL_REQUESTS && r->page != 1)
		abort();

	switch (r->op) {
	case AXIS_PORTAL_LOGIN:
		if (r->username == NULL || r->username[0] == '\0' || strlen(r->username) > AXIS_PORTAL_USERNAME_MAX_BYTES ||
		    r->password == NULL || r->password[0] == '\0' || strlen(r->password) > AXIS_PORTAL_PASSWORD_MAX_BYTES ||
		    r->client_address == NULL || !axis_ip_address_parse(r->client_address, &address) ||
		    r->user_agent == NULL || strlen(r->user_agent) > AXIS_PORTAL_AGENT_MAX_BYTES)
			abort();
		if (r->device != NULL && (r->device[0] == '\0' || strlen(r->device) > AXIS_PORTAL_DEVICE_MAX_BYTES ||
		    !token_text(r->device, TRUE)))
			abort();
		break;
	case AXIS_PORTAL_SEARCH:
		if (r->query == NULL || r->query[0] == '\0' || !g_utf8_validate(r->query, -1, NULL) ||
		    g_utf8_strlen(r->query, -1) > AXIS_PORTAL_QUERY_MAX_CHARS)
			abort();
		{
			const gchar *p;

			for (p = r->query; *p != '\0'; p = g_utf8_next_char(p)) {
				if (g_utf8_get_char(p) < 0x20 || g_utf8_get_char(p) == 0x7f)
					abort();
			}
		}
		break;
	case AXIS_PORTAL_BROWSE:
		if (r->list == NULL || r->list[0] == '\0' || strlen(r->list) > AXIS_PORTAL_LIST_MAX_BYTES ||
		    !token_text(r->list, FALSE) || !r->has_kind)
			abort();
		break;
	case AXIS_PORTAL_TITLE:
	case AXIS_PORTAL_ART:
		if (r->id < 1 || r->id > AXIS_PORTAL_MAX_ID)
			abort();
		break;
	case AXIS_PORTAL_REQUEST:
		if (r->id < 1 || r->id > AXIS_PORTAL_MAX_ID || !r->has_kind)
			abort();
		if (r->kind == AXIS_PORTAL_KIND_MOVIE && (r->seasons != -1 || r->n_picked != 0))
			abort();
		if (r->kind == AXIS_PORTAL_KIND_SERIES && (r->seasons < AXIS_PORTAL_SEASONS_ALL ||
		    r->seasons > AXIS_PORTAL_SEASONS_PICKED))
			abort();
		if ((r->seasons == AXIS_PORTAL_SEASONS_PICKED) != (r->n_picked > 0) || r->n_picked > AXIS_PORTAL_MAX_SEASONS)
			abort();
		for (i = 0; i < r->n_picked; i++) {
			if (r->picked[i] < 0 || r->picked[i] > AXIS_PORTAL_MAX_SEASON_NUMBER)
				abort();
			for (j = 0; j < i; j++) {
				if (r->picked[j] == r->picked[i])
					abort();
			}
		}
		break;
	case AXIS_PORTAL_WITHDRAW:
		if (r->request == NULL || strlen(r->request) != AXIS_PORTAL_REQUEST_ID_LEN || !token_text(r->request, FALSE))
			abort();
		break;
	default:
		break;
	}
}

/* Whatever text an answer is given, what is written is one line of ASCII
 * JSON without markup, and reads back as the text. */
static void
check_writer(
	const gchar *text,
	gsize        length
){
	g_autofree gchar *copy = g_strndup(text, length);
	g_autofree gchar *clean = g_utf8_make_valid(copy, -1);
	g_autofree gchar *json = NULL;
	AxisPortalWriter *w = axis_portal_writer_new();
	AxisJsonDoc *doc;
	const gchar *back;
	const guchar *p;
	AxisPortalTitle title;

	memset(&title, 0, sizeof(title));
	title.id = 7;
	title.kind = "movie";
	title.name = clean;
	title.overview = clean;
	title.state = "open";
	title.asked_by = clean;
	axis_portal_write_string(w, "text", clean);
	axis_portal_write_title(w, "title", &title);
	json = axis_portal_writer_finish(w);
	for (p = (const guchar *)json; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f || *p == '<' || *p == '>' || *p == '&')
			abort();
	}
	/* (The reader keeps at most its limit of a string, and makes U+0000 a
	 * replacement character: compared only where neither applies.) */
	doc = axis_json_parse(json, strlen(json), NULL);
	if (doc == NULL)
		abort();
	back = axis_json_string(axis_json_member(axis_json_doc_get_root(doc), "text"));
	if (back == NULL)
		abort();
	if (strlen(clean) < AXIS_JSON_MAX_STRING && strcmp(back, clean) != 0)
		abort();
	if (axis_json_type(axis_json_member(axis_json_doc_get_root(doc), "title")) != AXIS_JSON_OBJECT)
		abort();
	axis_json_doc_free(doc);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	AxisPortalRequest *request;
	gchar *problem = NULL;
	const gchar *body;
	AxisPortalOp op;
	gsize length;

	if (size < 1)
		return 0;
	body = (const gchar *)data + 1;
	length = size - 1;
	if ((guint)(guint8)(data[0] - '0') % (AXIS_PORTAL_N_OPS + 1) == AXIS_PORTAL_N_OPS) {
		check_writer(body, length);
		return 0;
	}
	op = (AxisPortalOp)((guint)(guint8)(data[0] - '0') % (AXIS_PORTAL_N_OPS + 1));

	request = axis_portal_request_parse(op, body, length, &problem);
	check_problem(problem);
	if (request == NULL) {
		if (problem == NULL)
			abort();
		g_free(problem);
		return 0;
	}
	if (problem != NULL || length > AXIS_PORTAL_MAX_REQUEST_BYTES || request->op != op)
		abort();
	if (request->proof != NULL && (!axis_portal_op_needs_proof(op) || strlen(request->proof) != AXIS_PORTAL_PROOF_LEN ||
	    !token_text(request->proof, FALSE)))
		abort();
	check_problem(request->problem);
	if (request->valid) {
		check_valid(request);
	} else {
		if (request->problem == NULL)
			abort();
		check_unset(request);
	}
	axis_portal_request_free(request);
	return 0;
}
