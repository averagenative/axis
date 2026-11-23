/*
 * axis-web-place.c - after a form, back to the place a person was working,
 * with the notice there
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-place.h"

#include <string.h>

#include "web/axis-web-auth-private.h"

#define PLACE_MAX_BYTES 64

gboolean
axis_web_place_id_valid(const gchar *id)
{
	gsize i;

	if (id == NULL || !g_ascii_isalpha(id[0]))
		return FALSE;
	for (i = 1; id[i] != '\0'; i++) {
		if (i >= PLACE_MAX_BYTES || !(g_ascii_isalnum(id[i]) || id[i] == '-' || id[i] == '_'))
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_web_place_location(
	const gchar *path,
	const gchar *done,
	const gchar *place
){
	GString *location;
	gboolean query;

	g_return_val_if_fail(path != NULL && path[0] == '/', g_strdup("/"));

	location = g_string_new(path);
	query = strchr(path, '?') != NULL;
	if (axis_web_place_id_valid(done)) {
		g_string_append_printf(location, "%c" AXIS_WEB_PLACE_DONE "=%s", query ? '&' : '?', done);
		query = TRUE;
	}
	if (axis_web_place_id_valid(place))
		g_string_append_printf(location, "%c" AXIS_WEB_PLACE_AT "=%s#%s", query ? '&' : '?', place, place);
	return g_string_free(location, FALSE);
}

HtmxResponse *
axis_web_place_redirect(
	HtmxRequest *request,
	const gchar *path,
	const gchar *done,
	const gchar *place
){
	g_autofree gchar *location = axis_web_place_location(path, done, place);

	return axis_web_auth_redirect(request, location);
}

const gchar *
axis_web_place_of(HtmxRequest *request)
{
	const gchar *at = htmx_request_get_query_param(request, AXIS_WEB_PLACE_AT);

	return axis_web_place_id_valid(at) ? at : NULL;
}
