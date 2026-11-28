/*
 * axis-log.h - log levels and the stderr log writer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_LOG_H
#define AXIS_LOG_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AxisLogLevel:
 *
 * Ordered from most to least verbose; a message is written when its level
 * is at or above the configured one.
 */
typedef enum
{
	AXIS_LOG_LEVEL_DEBUG,
	AXIS_LOG_LEVEL_INFO,
	AXIS_LOG_LEVEL_WARNING,
	AXIS_LOG_LEVEL_ERROR
} AxisLogLevel;

/**
 * axis_log_level_from_string:
 * @name: "debug", "info", "warning" or "error" (exact, lower case)
 * @level: (out): the parsed level; untouched on failure
 *
 * Returns: %TRUE if @name is a known level
 */
gboolean
axis_log_level_from_string(
	const gchar  *name,
	AxisLogLevel *level
);

/**
 * axis_log_level_to_string:
 *
 * Returns: (transfer none): the name accepted by axis_log_level_from_string()
 */
const gchar *
axis_log_level_to_string(AxisLogLevel level);

/**
 * axis_log_level_from_glib:
 * @flags: a GLib log level, possibly with the fatal/recursion flags set
 *
 * DEBUG maps to debug; INFO and MESSAGE to info; WARNING to warning;
 * CRITICAL and ERROR to error.
 */
AxisLogLevel
axis_log_level_from_glib(GLogLevelFlags flags);

/**
 * axis_log_should_write:
 * @flags: the GLib level of a message
 * @threshold: the configured level
 *
 * Returns: %TRUE if a message at @flags is written at @threshold
 */
gboolean
axis_log_should_write(
	GLogLevelFlags flags,
	AxisLogLevel   threshold
);

/**
 * axis_log_format:
 * @flags: the GLib level of the message
 * @domain: (nullable): the log domain
 * @message: (nullable): the message text
 *
 * Builds the single line written for a message, without a trailing
 * newline: "<UTC ISO-8601 time> <LEVEL> <domain>: <message>".  Newlines
 * and other control characters inside @message are replaced with a space
 * so that one message is always one line.
 *
 * Returns: (transfer full): the formatted line
 */
gchar *
axis_log_format(
	GLogLevelFlags  flags,
	const gchar    *domain,
	const gchar    *message
);

/**
 * AxisLogFormat:
 * @AXIS_LOG_FORMAT_TEXT: axis_log_format()'s line (the default)
 * @AXIS_LOG_FORMAT_JSON: axis_log_format_json()'s object, one per line
 */
typedef enum
{
	AXIS_LOG_FORMAT_TEXT,
	AXIS_LOG_FORMAT_JSON
} AxisLogFormat;

/**
 * axis_log_format_json:
 * @flags: the GLib level of the message
 * @domain: (nullable): the log domain
 * @message: (nullable): the message text
 *
 * Builds the single line written for a message in the JSON format, without
 * a trailing newline: an object with exactly the members "time" (UTC
 * ISO-8601), "level" (as axis_log_level_to_string()), "domain" ("-" for
 * none) and "message".  The message is the same text the text format
 * writes, so the same redaction applies: nothing but those four members is
 * ever written (no code location, no other structured field).  Invalid
 * UTF-8 becomes U+FFFD, and every control character is escaped, so one
 * message is always one line of valid JSON.
 *
 * Returns: (transfer full): the formatted line
 */
gchar *
axis_log_format_json(
	GLogLevelFlags  flags,
	const gchar    *domain,
	const gchar    *message
);

/**
 * axis_log_init:
 * @threshold: messages below this level are dropped
 *
 * Installs the process-wide structured log writer (stderr, one line per
 * message, in the text format until axis_log_set_format()).  GLib allows a
 * writer to be installed once per process, so call this once, early in
 * main().  Use axis_log_set_level() afterwards.
 */
void
axis_log_init(AxisLogLevel threshold);

/**
 * axis_log_set_configured_level:
 * @level: the level the configuration asks for
 *
 * Remembers the configured level (axis_log_init() does it) so that a
 * setting that overrides the level can say what the configuration holds
 * and go back to it.  It does not change the level in use.
 */
void
axis_log_set_configured_level(AxisLogLevel level);

AxisLogLevel
axis_log_get_configured_level(void);

void
axis_log_set_level(AxisLogLevel threshold);

AxisLogLevel
axis_log_get_level(void);

void
axis_log_set_format(AxisLogFormat format);

/* The format last set; safe in a signal handler (an atomic read). */
AxisLogFormat
axis_log_get_format(void);

G_END_DECLS

#endif /* AXIS_LOG_H */
