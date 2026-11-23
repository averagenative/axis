/*
 * axis-web-hub-topic.h - the names a client may subscribe to, and the
 * parser of the query string that asks for them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A topic is plain text such as "jobs" or "job:123": 1 to 64 characters,
 * lower-case ASCII letters and digits first, then also ':', '.', '_' and
 * '-'.  Everything a client sends goes through axis_web_hub_parse_topics(),
 * which has a fuzz target (tests/fuzz/fuzz-web-hub-topic.c).  This file
 * depends on GLib only.
 */

#ifndef AXIS_WEB_HUB_TOPIC_H
#define AXIS_WEB_HUB_TOPIC_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_WEB_HUB_TOPIC_MAX_LEN       64
#define AXIS_WEB_HUB_MAX_TOPICS          8

/* The longest query string looked at; a longer one is refused. */
#define AXIS_WEB_HUB_MAX_QUERY_LEN       2048

#define AXIS_WEB_HUB_TOPIC_ERROR (axis_web_hub_topic_error_quark())

typedef enum
{
	AXIS_WEB_HUB_TOPIC_ERROR_NONE,		/* no topic asked for */
	AXIS_WEB_HUB_TOPIC_ERROR_TOO_MANY,
	AXIS_WEB_HUB_TOPIC_ERROR_INVALID,	/* malformed name or encoding */
	AXIS_WEB_HUB_TOPIC_ERROR_TOO_LONG	/* the query string */
} AxisWebHubTopicError;

GQuark
axis_web_hub_topic_error_quark(void);

/* Returns: %TRUE if @topic is a well-formed topic name */
gboolean
axis_web_hub_topic_is_valid(const gchar *topic);

/* Returns: %TRUE if @name is usable as an event name: 1 to 32 lower-case
 * letters, digits and '-', starting with a letter */
gboolean
axis_web_hub_event_name_is_valid(const gchar *name);

/**
 * axis_web_hub_parse_topics:
 * @query: (nullable): the raw query string of a request, without the '?'
 * @error: (nullable): return location for an error
 *
 * Collects the value of every "topic" parameter, percent-decoded, in
 * order and without duplicates.  Other parameters are ignored.  Fails
 * (returning %NULL) when there is no topic, more than
 * %AXIS_WEB_HUB_MAX_TOPICS distinct ones, a malformed one, a bad
 * percent-escape (or an encoded NUL), or when @query is longer than
 * %AXIS_WEB_HUB_MAX_QUERY_LEN.
 *
 * Returns: (transfer full) (nullable): a %NULL-terminated vector of valid
 *   topics
 */
gchar **
axis_web_hub_parse_topics(
	const gchar  *query,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_WEB_HUB_TOPIC_H */
