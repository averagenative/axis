/*
 * fuzz-portal-form.c - libFuzzer target for axis-portal's readers of what a
 * browser sends: a path, a query, a posted form
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  The input is a path, a query or a form as
 * a stranger wrote it.  Whatever it is, the readers of
 * clients/portal/axis-portal-form.h must return without crashing or
 * allocating without bound, and:
 *
 *  - a path that is a route is one of the portal's own, written in plain
 *    characters, with a title's number or a request's identifier in its
 *    range and form; nothing else is a route;
 *  - a form that is read has at most the fields, the names and the values
 *    the limits allow, every value valid UTF-8 without a NUL, and a field
 *    given twice has no value;
 *  - a number is plain digits in its range; picked seasons are 1 to 100
 *    numbers, each once;
 *
 * and whatever of it goes on to the daemon goes as a body that the daemon's
 * own reader (api/axis-portal-proto.h) takes whole, or does not go at all:
 * a search for what was typed, a login with the name and the password that
 * were typed, a request with the seasons that were ticked.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../clients/portal/axis-portal-form.h"
#include "../../clients/portal/axis-portal-wire.h"
#include "api/axis-portal-proto.h"

#define PROOF "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* A body the portal's writer made must be one the daemon's reader takes
 * whole, with the values it was made from. */
static AxisPortalRequest *
taken(
	AxisPortalOp  op,
	gchar        *body
){
	AxisPortalRequest *request;

	if (body == NULL)
		return NULL;
	request = axis_portal_request_parse(op, body, strlen(body), NULL);
	if (request == NULL || !request->valid)
		abort();
	axis_portal_wire_free(body);
	return request;
}

static void
check_route(const gchar *path)
{
	g_autofree gchar *request_id = NULL;
	gint64 id = 0;
	AxisPortalRoute route = axis_portal_route_parse(path, &id, &request_id);
	const gchar *p;

	if (route == AXIS_PORTAL_ROUTE_NONE) {
		if (id != 0 || request_id != NULL)
			abort();
		return;
	}
	if (strlen(path) > AXIS_PORTAL_PATH_MAX || path[0] != '/')
		abort();
	for (p = path; *p != '\0'; p++) {
		if (!g_ascii_isalnum(*p) && *p != '/' && *p != '.' && *p != '_' && *p != '-')
			abort();
	}
	if (strstr(path, "//") != NULL || strstr(path, "..") != NULL)
		abort();
	switch (route) {
	case AXIS_PORTAL_ROUTE_TITLE:
	case AXIS_PORTAL_ROUTE_ASK:
	case AXIS_PORTAL_ROUTE_ART:
		if (id < 1 || id > AXIS_PORTAL_MAX_ID || request_id != NULL)
			abort();
		break;
	case AXIS_PORTAL_ROUTE_WITHDRAW:
		if (id != 0 || !axis_portal_wire_is_request_id(request_id))
			abort();
		break;
	default:
		if (id != 0 || request_id != NULL)
			abort();
		break;
	}
}

static void
check_value(const gchar *value)
{
	if (value != NULL && (strlen(value) > AXIS_PORTAL_FORM_VALUE_MAX || !g_utf8_validate(value, -1, NULL)))
		abort();
}

static void
check_form(
	const gchar *data,
	gsize        length
){
	static const gchar *const names[] = {
		"q", "kind", "page", "list", "what", "season", "username", "password", "next", "_csrf", NULL
	};
	g_autoptr(AxisPortalForm) form = axis_portal_form_parse(data, length, AXIS_PORTAL_FORM_MAX_BYTES);
	gint picked[AXIS_PORTAL_MAX_SEASONS];
	guint n_picked = 0;
	AxisPortalAsk ask = AXIS_PORTAL_ASK_ALL;
	AxisPortalKind kind = AXIS_PORTAL_KIND_IS_MOVIE;
	gint64 number = 0;
	guint total = 0;
	guint i;
	guint j;

	if (form == NULL)
		return;
	if (length > AXIS_PORTAL_FORM_MAX_BYTES)
		abort();
	for (i = 0; names[i] != NULL; i++) {
		guint count = axis_portal_form_count(form, names[i]);
		const gchar *value = axis_portal_form_get(form, names[i]);

		total += count;
		check_value(value);
		if ((count == 1) != (value != NULL))
			abort();
	}
	if (total > AXIS_PORTAL_FORM_MAX_FIELDS)
		abort();

	if (axis_portal_form_number(axis_portal_form_get(form, "page"), 1, AXIS_PORTAL_MAX_PAGE, &number) &&
	    (number < 1 || number > AXIS_PORTAL_MAX_PAGE))
		abort();
	if (axis_portal_form_seasons(form, &ask, picked, &n_picked)) {
		if ((guint)ask > AXIS_PORTAL_ASK_PICKED || (ask == AXIS_PORTAL_ASK_PICKED) != (n_picked > 0) ||
		    n_picked > AXIS_PORTAL_MAX_SEASONS)
			abort();
		for (i = 0; i < n_picked; i++) {
			if (picked[i] < 0 || picked[i] > AXIS_PORTAL_MAX_SEASON_NUMBER)
				abort();
			for (j = 0; j < i; j++) {
				if (picked[j] == picked[i])
					abort();
			}
		}
		/* What was ticked goes to the daemon as its own member. */
		{
			AxisPortalRequest *request = taken(AXIS_PORTAL_REQUEST, axis_portal_wire_request(PROOF, 7,
				AXIS_PORTAL_KIND_IS_SERIES, ask, picked, n_picked));

			if (request == NULL || request->seasons != (gint)ask || request->n_picked != n_picked || request->id != 7)
				abort();
			axis_portal_request_free(request);
		}
	} else if (n_picked > AXIS_PORTAL_MAX_SEASONS) {
		abort();
	}

	/* What was typed into the search box. */
	{
		const gchar *q = axis_portal_form_get(form, "q");
		gint search_kind = axis_portal_form_kind(axis_portal_form_get(form, "kind"), &kind) ? (gint)kind : -1;
		AxisPortalRequest *request = taken(AXIS_PORTAL_SEARCH, axis_portal_wire_search(PROOF, q, search_kind, 1));

		if (request != NULL) {
			if (q == NULL || strcmp(request->query, q) != 0 || request->has_kind != (search_kind >= 0))
				abort();
			axis_portal_request_free(request);
		}
	}
	/* The name and the password that were typed. */
	{
		const gchar *username = axis_portal_form_get(form, "username");
		const gchar *password = axis_portal_form_get(form, "password");
		AxisPortalRequest *request = taken(AXIS_PORTAL_LOGIN, axis_portal_wire_login(username, password, "203.0.113.5",
			"a browser", NULL));

		if (request != NULL) {
			if (username == NULL || password == NULL || strcmp(request->username, username) != 0 ||
			    strcmp(request->password, password) != 0 || request->device != NULL)
				abort();
			axis_portal_request_free(request);
		}
	}
	/* A list's name, and a request's identifier, as a browser may write
	 * them: sent only in their form. */
	{
		const gchar *list = axis_portal_form_get(form, "list");
		const gchar *next = axis_portal_form_get(form, "next");
		AxisPortalRequest *request = taken(AXIS_PORTAL_BROWSE, axis_portal_wire_browse(PROOF, list,
			AXIS_PORTAL_KIND_IS_MOVIE, 1));

		if (request != NULL) {
			if (!axis_portal_wire_is_list(list) || strcmp(request->list, list) != 0)
				abort();
			axis_portal_request_free(request);
		}
		request = taken(AXIS_PORTAL_WITHDRAW, axis_portal_wire_withdraw(PROOF, next));
		if (request != NULL) {
			if (!axis_portal_wire_is_request_id(next))
				abort();
			axis_portal_request_free(request);
		}
		/* And a proof a browser's cookie may hold. */
		request = taken(AXIS_PORTAL_WHOAMI, axis_portal_wire_proof(next));
		if (request != NULL) {
			if (!axis_portal_wire_is_proof(next) || request->proof == NULL || strcmp(request->proof, next) != 0)
				abort();
			axis_portal_request_free(request);
		}
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);
	gint64 number = 0;

	/* As a path (up to its first NUL, as a path arrives). */
	check_route(text);
	/* As a number. */
	if (axis_portal_form_number(text, 0, AXIS_PORTAL_MAX_ID, &number)) {
		g_autofree gchar *back = g_strdup_printf("%" G_GINT64_FORMAT, number);

		if (strcmp(back, text) != 0)
			abort();
	}
	/* As a query or a form (every byte, a NUL too). */
	check_form((const gchar *)data, size);
	return 0;
}
