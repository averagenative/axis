/*
 * axis-log.c - log levels and the stderr log writer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "core/axis-log.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Read from whichever thread logs; always access with g_atomic_int_*. */
static gint axis_log_threshold = AXIS_LOG_LEVEL_INFO;
static gint axis_log_output_format = AXIS_LOG_FORMAT_TEXT;

static const gchar *const level_names[] = {
	"debug", "info", "warning", "error"
};

gboolean
axis_log_level_from_string(
	const gchar  *name,
	AxisLogLevel *level
){
	guint i;

	if (name == NULL)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(level_names); i++) {
		if (strcmp(name, level_names[i]) == 0) {
			if (level != NULL)
				*level = (AxisLogLevel)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
axis_log_level_to_string(AxisLogLevel level)
{
	if ((guint)level >= G_N_ELEMENTS(level_names))
		return "error";

	return level_names[level];
}

AxisLogLevel
axis_log_level_from_glib(GLogLevelFlags flags)
{
	if (flags & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
		return AXIS_LOG_LEVEL_ERROR;
	if (flags & G_LOG_LEVEL_WARNING)
		return AXIS_LOG_LEVEL_WARNING;
	if (flags & (G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO))
		return AXIS_LOG_LEVEL_INFO;
	if (flags & G_LOG_LEVEL_DEBUG)
		return AXIS_LOG_LEVEL_DEBUG;

	/* No level bit at all: do not drop something we cannot classify. */
	return AXIS_LOG_LEVEL_ERROR;
}

gboolean
axis_log_should_write(
	GLogLevelFlags flags,
	AxisLogLevel   threshold
){
	return axis_log_level_from_glib(flags) >= threshold;
}

/* The time of a line: UTC with milliseconds, 2026-11-27T17:54:06.123Z. */
static gchar *
log_stamp(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *head = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S");

	return g_strdup_printf("%s.%03dZ", head, g_date_time_get_microsecond(now) / 1000);
}

gchar *
axis_log_format(
	GLogLevelFlags  flags,
	const gchar    *domain,
	const gchar    *message
){
	g_autofree gchar *stamp = log_stamp();
	g_autofree gchar *level = NULL;
	GString *line;
	const gchar *p;

	level = g_ascii_strup(axis_log_level_to_string(axis_log_level_from_glib(flags)), -1);

	line = g_string_new(NULL);
	g_string_append_printf(line, "%s %s %s: ",
		stamp, level, domain != NULL ? domain : "-");

	/* Control characters are single bytes in UTF-8, so a byte loop cannot
	 * split a multi-byte sequence. */
	for (p = message != NULL ? message : ""; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		g_string_append_c(line, (c < 0x20 || c == 0x7f) ? ' ' : (gchar)c);
	}

	return g_string_free(line, FALSE);
}

/* @text as a JSON string, quotes included: valid UTF-8 (invalid sequences
 * become U+FFFD), with '"', '\' and every control character escaped. */
static void
append_json_string(
	GString     *out,
	const gchar *text
){
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	const gchar *p;

	g_string_append_c(out, '"');
	for (p = valid; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		switch (c) {
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
			if (c < 0x20 || c == 0x7f)
				g_string_append_printf(out, "\\u%04x", (guint)c);
			else
				g_string_append_c(out, (gchar)c);
		}
	}
	g_string_append_c(out, '"');
}

gchar *
axis_log_format_json(
	GLogLevelFlags  flags,
	const gchar    *domain,
	const gchar    *message
){
	g_autofree gchar *stamp = log_stamp();
	GString *line = g_string_new("{\"time\":");

	append_json_string(line, stamp);
	g_string_append(line, ",\"level\":");
	append_json_string(line, axis_log_level_to_string(axis_log_level_from_glib(flags)));
	g_string_append(line, ",\"domain\":");
	append_json_string(line, domain != NULL ? domain : "-");
	g_string_append(line, ",\"message\":");
	append_json_string(line, message);
	g_string_append_c(line, '}');

	return g_string_free(line, FALSE);
}

/* Value of the field called @key, or NULL.  Fields may be NUL-terminated
 * (length -1) or carry an explicit length. */
static gchar *
field_dup(
	const GLogField *fields,
	gsize            n_fields,
	const gchar     *key
){
	gsize i;

	for (i = 0; i < n_fields; i++) {
		if (strcmp(fields[i].key, key) != 0)
			continue;
		if (fields[i].value == NULL)
			return NULL;
		if (fields[i].length < 0)
			return g_strdup(fields[i].value);
		return g_strndup(fields[i].value, (gsize)fields[i].length);
	}

	return NULL;
}

static GLogWriterOutput
axis_log_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	g_autofree gchar *domain = NULL;
	g_autofree gchar *message = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *out = NULL;
	gssize written;

	if (!axis_log_should_write(log_level, axis_log_get_level()))
		return G_LOG_WRITER_HANDLED;

	domain = field_dup(fields, n_fields, "GLIB_DOMAIN");
	message = field_dup(fields, n_fields, "MESSAGE");
	if (g_atomic_int_get(&axis_log_output_format) == AXIS_LOG_FORMAT_JSON)
		line = axis_log_format_json(log_level, domain, message);
	else
		line = axis_log_format(log_level, domain, message);
	out = g_strconcat(line, "\n", NULL);

	/* One write() so lines from different threads or processes sharing
	 * stderr are not interleaved mid-line. */
	do {
		written = write(STDERR_FILENO, out, strlen(out));
	} while (written < 0 && errno == EINTR);

	return G_LOG_WRITER_HANDLED;
}

void
axis_log_init(AxisLogLevel threshold)
{
	axis_log_set_level(threshold);
	g_log_set_writer_func(axis_log_writer, NULL, NULL);
}

void
axis_log_set_level(AxisLogLevel threshold)
{
	g_atomic_int_set(&axis_log_threshold, (gint)threshold);
}

AxisLogLevel
axis_log_get_level(void)
{
	return (AxisLogLevel)g_atomic_int_get(&axis_log_threshold);
}

void
axis_log_set_format(AxisLogFormat format)
{
	g_atomic_int_set(&axis_log_output_format, (gint)format);
}

AxisLogFormat
axis_log_get_format(void)
{
	return (AxisLogFormat)g_atomic_int_get(&axis_log_output_format);
}
