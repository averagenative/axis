/*
 * axis-meta-json.c - reading the fields of a provider's JSON answer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Moved out of the TMDb parser when TheTVDB's became the second
 * (add-metadata-tvdb); what each reader accepts is unchanged.
 */

#include "meta/axis-meta-json.h"

#include <string.h>

gboolean
axis_meta_json_unreadable(
	const gchar  *label,
	GError      **error,
	const gchar  *why
){
	g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "%s's answer cannot be read: %s", label, why);
	return FALSE;
}

AxisJsonDoc *
axis_meta_json_parse(
	const gchar  *label,
	const gchar  *data,
	gsize         length,
	GError      **error
){
	g_autofree gchar *problem = NULL;
	AxisJsonDoc *doc;

	if (data == NULL || length == 0) {
		axis_meta_json_unreadable(label, error, "it is empty");
		return NULL;
	}
	doc = axis_json_parse(data, length, &problem);
	if (doc == NULL) {
		axis_meta_json_unreadable(label, error, problem != NULL ? problem : "it is not JSON");
		return NULL;
	}
	if (axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_OBJECT) {
		axis_json_doc_free(doc);
		axis_meta_json_unreadable(label, error, "it is not an object");
		return NULL;
	}
	return doc;
}

gchar *
axis_meta_json_text(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_meta_clean_text(axis_json_string(axis_json_member(object, name)), max_chars);
}

gint
axis_meta_json_int(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              min,
	gint64              max
){
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 value = 0;

	if (axis_json_type(node) != AXIS_JSON_NUMBER || !axis_json_get_integer(node, min, max, &value))
		return 0;
	return (gint)value;
}

gchar *
axis_meta_json_date_of(const gchar *text)
{
	gsize n;

	if (text == NULL)
		return NULL;
	n = strlen(text);
	if (n < 10 || (n > 10 && text[10] != 'T'))
		return NULL;
	{
		g_autofree gchar *day = g_strndup(text, 10);

		return axis_meta_date_valid(day) ? g_steal_pointer(&day) : NULL;
	}
}

gchar *
axis_meta_json_date(
	const AxisJsonNode *object,
	const gchar        *name
){
	return axis_meta_json_date_of(axis_json_string(axis_json_member(object, name)));
}

gint
axis_meta_json_year_of(const gchar *date)
{
	gint year;

	if (date == NULL)
		return 0;
	year = (gint)g_ascii_strtoll(date, NULL, 10);
	return axis_meta_year_valid(year) ? year : 0;
}
