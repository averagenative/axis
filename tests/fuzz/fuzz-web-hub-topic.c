/*
 * fuzz-web-hub-topic.c - libFuzzer target for the parser of the topics a
 * client asks an event stream for
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Built with clang -fsanitize=fuzzer,address,undefined by `make fuzz`.
 * Besides memory safety, it checks what the parser promises about every
 * query it accepts: one to eight distinct topics, each valid (so nothing
 * but lower-case letters, digits and ':', '.', '_', '-' ever reaches the
 * hub's tables or an event stream); a query it refuses sets an error and
 * returns nothing; and writing the topics back out as a query and parsing
 * that gives the same list.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web/axis-web-hub-topic.h"

static void
check(const gchar *query)
{
	GError *error = NULL;
	gchar **topics = axis_web_hub_parse_topics(query, &error);
	GString *again;
	gchar **second;
	guint n;
	guint i;
	guint j;

	if (topics == NULL) {
		if (error == NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (error != NULL)
		abort();

	n = g_strv_length(topics);
	if (n == 0 || n > AXIS_WEB_HUB_MAX_TOPICS || strlen(query) > AXIS_WEB_HUB_MAX_QUERY_LEN)
		abort();

	again = g_string_new("");
	for (i = 0; i < n; i++) {
		if (!axis_web_hub_topic_is_valid(topics[i]))
			abort();
		for (j = 0; j < i; j++) {
			if (strcmp(topics[i], topics[j]) == 0)
				abort();
		}
		g_string_append_printf(again, "%stopic=%s", i > 0 ? "&" : "", topics[i]);
	}

	second = axis_web_hub_parse_topics(again->str, &error);
	if (second == NULL || error != NULL || !g_strv_equal((const gchar *const *)topics,
	                                                      (const gchar *const *)second))
		abort();

	g_strfreev(second);
	g_string_free(again, TRUE);
	g_strfreev(topics);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* The parser takes a C string; an embedded NUL ends the input. */
	g_autofree gchar *text = g_strndup((const gchar *)data, size);

	check(text);

	return 0;
}
