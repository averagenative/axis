/*
 * axis-web-back.c - coming back to the page a form was posted from
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-back.h"

#include <string.h>

#include "web/axis-web-auth-input.h"
#include "web/axis-web-auth-private.h"

#define HERE_KEY "axis-web-back-here"

void
axis_web_back_set_here(
	HtmxRequest *request,
	const gchar *page
){
	g_object_set_data_full(G_OBJECT(request), HERE_KEY, g_strdup(page), g_free);
}

gchar *
axis_web_back_here(HtmxRequest *request)
{
	const gchar *page = g_object_get_data(G_OBJECT(request), HERE_KEY);
	const gchar *path = htmx_request_get_path(request);
	const gchar *query = htmx_request_get_query(request);
	g_autoptr(GString) out = NULL;
	g_auto(GStrv) parts = NULL;
	gboolean first = TRUE;
	guint i;

	/* A part drawn for another page: that page. */
	if (page != NULL)
		return axis_auth_next_is_safe(page) ? g_strdup(page) : NULL;
	if (path == NULL)
		return NULL;
	out = g_string_new(path);
	if (query != NULL && query[0] != '\0') {
		parts = g_strsplit(query, "&", -1);
		for (i = 0; parts[i] != NULL; i++) {
			/* What was done, and where (web/axis-web-place.h), are not
			 * part of the page. */
			if (parts[i][0] == '\0' || strcmp(parts[i], "done") == 0 || g_str_has_prefix(parts[i], "done=") ||
			    strcmp(parts[i], "at") == 0 || g_str_has_prefix(parts[i], "at=") ||
			    strcmp(parts[i], AXIS_WEB_BACK_ADDED) == 0 || g_str_has_prefix(parts[i], AXIS_WEB_BACK_ADDED "="))
				continue;
			g_string_append_c(out, first ? '?' : '&');
			g_string_append(out, parts[i]);
			first = FALSE;
		}
	}
	if (!axis_auth_next_is_safe(out->str))
		return NULL;
	return g_string_free(g_steal_pointer(&out), FALSE);
}

HtmxNode *
axis_web_back_field(HtmxRequest *request)
{
	g_autofree gchar *here = axis_web_back_here(request);

	return here != NULL ? axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, here) : NULL;
}

#define ADD_RETURN_KEY "axis-web-back-add-return"

void
axis_web_back_set_add_return(
	HtmxRequest *request,
	const gchar *place
){
	g_object_set_data_full(G_OBJECT(request), ADD_RETURN_KEY, g_strdup(place), g_free);
}

HtmxNode *
axis_web_back_add_field(
	HtmxRequest *request,
	gint64       title_id
){
	g_autofree gchar *here = axis_web_back_here(request);
	g_autofree gchar *back = NULL;

	if (here == NULL)
		return NULL;
	if (g_object_get_data(G_OBJECT(request), ADD_RETURN_KEY) == NULL)
		return axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, here);
	back = g_strdup_printf("%s%c" AXIS_WEB_BACK_ADDED "=%" G_GINT64_FORMAT, here, strchr(here, '?') != NULL ? '&' : '?',
		title_id);
	return axis_web_auth_hidden(AXIS_WEB_BACK_FIELD, back);
}

gchar *
axis_web_back_add_place(
	HtmxRequest *request,
	gint64       title_id
){
	const gchar *place = g_object_get_data(G_OBJECT(request), ADD_RETURN_KEY);

	return place != NULL ? g_strdup(place) : g_strdup_printf("title-%" G_GINT64_FORMAT, title_id);
}

gchar *
axis_web_back_posted(HtmxRequest *request)
{
	const gchar *back = htmx_request_get_form_value(request, AXIS_WEB_BACK_FIELD);

	if (back == NULL || strchr(back, '#') != NULL || !axis_auth_next_is_safe(back))
		return NULL;
	return g_strdup(back);
}

/* A place id as the pages make them: letters, digits, '-' and '_'. */
static gboolean
place_is_plain(const gchar *place)
{
	const gchar *p;

	if (place[0] == '\0')
		return FALSE;
	for (p = place; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
			return FALSE;
	return TRUE;
}

gchar *
axis_web_back_title_href(
	HtmxRequest *request,
	gint64       title_id
){
	g_autofree gchar *here = axis_web_back_here(request);
	g_autofree gchar *from = here != NULL ? g_uri_escape_string(here, NULL, FALSE) : NULL;

	const gchar *place = g_object_get_data(G_OBJECT(request), ADD_RETURN_KEY);

	if (from == NULL)
		return g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id);
	if (place != NULL && place_is_plain(place))
		return g_strdup_printf("/title/%" G_GINT64_FORMAT "?" AXIS_WEB_BACK_FROM "=%s&" AXIS_WEB_BACK_RETURN_AT "=%s",
			title_id, from, place);
	return g_strdup_printf("/title/%" G_GINT64_FORMAT "?" AXIS_WEB_BACK_FROM "=%s", title_id, from);
}

gboolean
axis_web_back_from_add(
	HtmxRequest *request,
	gint64       title_id,
	gchar      **back,
	gchar      **at
){
	g_autofree gchar *from = axis_web_back_from(request);
	const gchar *place = htmx_request_get_query_param(request, AXIS_WEB_BACK_RETURN_AT);

	if (from == NULL)
		return FALSE;
	if (place != NULL && place_is_plain(place)) {
		*back = g_strdup_printf("%s%c" AXIS_WEB_BACK_ADDED "=%" G_GINT64_FORMAT, from, strchr(from, '?') != NULL ? '&' : '?',
			title_id);
		*at = g_strdup(place);
	} else {
		*back = g_steal_pointer(&from);
		*at = g_strdup_printf("title-%" G_GINT64_FORMAT, title_id);
	}
	return TRUE;
}

gchar *
axis_web_back_from(HtmxRequest *request)
{
	const gchar *from = htmx_request_get_query_param(request, AXIS_WEB_BACK_FROM);

	/* Only a list's page: a title page's own "from" is never followed. */
	if (from == NULL || strchr(from, '#') != NULL || !axis_auth_next_is_safe(from) ||
	    g_str_has_prefix(from, "/title/"))
		return NULL;
	return g_strdup(from);
}

gchar *
axis_web_back_with_done(
	const gchar *back,
	const gchar *key
){
	return g_strdup_printf("%s%cdone=%s", back, strchr(back, '?') != NULL ? '&' : '?', key);
}
