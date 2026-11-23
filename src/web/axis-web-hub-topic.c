/*
 * axis-web-hub-topic.c - topic names and the query parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-hub-topic.h"

#include <string.h>

G_DEFINE_QUARK(axis-web-hub-topic-error-quark, axis_web_hub_topic_error)

gboolean
axis_web_hub_topic_is_valid(const gchar *topic)
{
	gsize length;
	gsize i;

	if (topic == NULL)
		return FALSE;
	length = strlen(topic);
	if (length == 0 || length > AXIS_WEB_HUB_TOPIC_MAX_LEN)
		return FALSE;

	for (i = 0; i < length; i++) {
		gchar c = topic[i];
		gboolean alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

		if (alnum)
			continue;
		if (i > 0 && (c == ':' || c == '.' || c == '_' || c == '-'))
			continue;
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_web_hub_event_name_is_valid(const gchar *name)
{
	gsize length;
	gsize i;

	if (name == NULL)
		return FALSE;
	length = strlen(name);
	if (length == 0 || length > 32 || name[0] < 'a' || name[0] > 'z')
		return FALSE;

	for (i = 1; i < length; i++) {
		gchar c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return FALSE;
	}

	return TRUE;
}

static gint
hex_value(gchar c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decodes the @length bytes at @text; %NULL for a bad escape or a NUL. */
static gchar *
decode(
	const gchar *text,
	gsize        length
){
	GString *out = g_string_sized_new(length);
	gsize i;

	for (i = 0; i < length; i++) {
		if (text[i] == '%') {
			gint high;
			gint low;

			if (i + 2 >= length)
				goto bad;
			high = hex_value(text[i + 1]);
			low = hex_value(text[i + 2]);
			if (high < 0 || low < 0 || (high == 0 && low == 0))
				goto bad;
			g_string_append_c(out, (gchar)(high * 16 + low));
			i += 2;
		} else {
			g_string_append_c(out, text[i]);
		}
	}

	return g_string_free(out, FALSE);

bad:
	g_string_free(out, TRUE);
	return NULL;
}

gchar **
axis_web_hub_parse_topics(
	const gchar  *query,
	GError      **error
){
	GPtrArray *topics = g_ptr_array_new_with_free_func(g_free);
	const gchar *p;
	gsize length;

	if (query == NULL)
		query = "";
	length = strlen(query);
	if (length > AXIS_WEB_HUB_MAX_QUERY_LEN) {
		g_set_error_literal(error, AXIS_WEB_HUB_TOPIC_ERROR,
			AXIS_WEB_HUB_TOPIC_ERROR_TOO_LONG, "the query string is too long");
		goto fail;
	}

	p = query;
	while (*p != '\0') {
		const gchar *end = strchr(p, '&');
		gsize piece = end != NULL ? (gsize)(end - p) : strlen(p);

		if (piece > 6 && strncmp(p, "topic=", 6) == 0) {
			gchar *topic = decode(p + 6, piece - 6);
			guint i;
			gboolean seen = FALSE;

			if (topic == NULL || !axis_web_hub_topic_is_valid(topic)) {
				g_free(topic);
				g_set_error_literal(error, AXIS_WEB_HUB_TOPIC_ERROR,
					AXIS_WEB_HUB_TOPIC_ERROR_INVALID, "a topic is not valid");
				goto fail;
			}

			for (i = 0; i < topics->len; i++) {
				if (strcmp(g_ptr_array_index(topics, i), topic) == 0)
					seen = TRUE;
			}
			if (seen) {
				g_free(topic);
			} else if (topics->len >= AXIS_WEB_HUB_MAX_TOPICS) {
				g_free(topic);
				g_set_error_literal(error, AXIS_WEB_HUB_TOPIC_ERROR,
					AXIS_WEB_HUB_TOPIC_ERROR_TOO_MANY, "too many topics");
				goto fail;
			} else {
				g_ptr_array_add(topics, topic);
			}
		} else if (piece == 6 && strncmp(p, "topic=", 6) == 0) {
			g_set_error_literal(error, AXIS_WEB_HUB_TOPIC_ERROR,
				AXIS_WEB_HUB_TOPIC_ERROR_INVALID, "a topic is empty");
			goto fail;
		}

		if (end == NULL)
			break;
		p = end + 1;
	}

	if (topics->len == 0) {
		g_set_error_literal(error, AXIS_WEB_HUB_TOPIC_ERROR,
			AXIS_WEB_HUB_TOPIC_ERROR_NONE, "no topic was asked for");
		goto fail;
	}

	g_ptr_array_add(topics, NULL);
	return (gchar **)g_ptr_array_free(topics, FALSE);

fail:
	g_ptr_array_free(topics, TRUE);
	return NULL;
}
