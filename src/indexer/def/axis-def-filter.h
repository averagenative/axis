/*
 * axis-def-filter.h - the filters of indexer definitions, and the readers
 * of a field's text as a date, a size or a count
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition's field is a selector's text passed through a chain of
 * filters.  The filters the first cut supports (the ones a survey of
 * the public definitions found in use): replace, re_replace, regexp, split, trim,
 * append, prepend, tolower, toupper, urldecode, urlencode, querystring,
 * dateparse (and timeparse, the same), timeago (and reltime), fuzzytime,
 * validfilename, htmldecode, htmlencode, diacritics, base64decode,
 * base64encode and strdump (which changes nothing).  Their arguments arrive
 * as text, already rendered by the caller where they are templates.
 *
 * Every filter returns new text that is valid UTF-8 without NUL and at most
 * %AXIS_DEF_FILTER_MAX_TEXT bytes (a longer result is cut), or fails with a
 * reason: a regular expression that is refused or past its bounds
 * (indexer/def/axis-def-regex.h), arguments that are not the filter's.
 *
 * Dates.  dateparse reads .NET's custom layouts ("yyyy-MM-dd HH:mm zzz",
 * "ddd, dd MMM yyyy HH:mm:ss zzz", "htt MMM. d" ...: the letters yyyy yy y
 * MMMM MMM MM M dddd ddd dd d HH H hh h mm m ss s f.. tt t zzz zz z K and
 * quoted text), with English names; a layout without a year takes the
 * current one, without a date today's; a time without a zone is taken as
 * UTC.  Its result, and timeago's and fuzzytime's, is ISO 8601 in UTC
 * ("2024-03-01T12:00:00Z"), which axis_def_parse_date() reads back.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-filter.c.
 */

#ifndef AXIS_DEF_FILTER_H
#define AXIS_DEF_FILTER_H

#include <glib.h>

#include "indexer/def/axis-def-regex.h"

G_BEGIN_DECLS

#define AXIS_DEF_FILTER_MAX_TEXT (64 * 1024)

/* Whether the engine has a filter of this name. */
gboolean
axis_def_filter_known(const gchar *name);

/**
 * axis_def_filter_apply:
 * @name: the filter
 * @args: its arguments as text (%NULL-ended; may be empty)
 * @value: what it is applied to
 * @now_us: the time relative dates are counted from
 * @regex: where its regular expressions run
 * @problem: (out) (optional) (transfer full): why it failed
 *
 * Returns: (transfer full) (nullable): the new value, or %NULL
 */
gchar *
axis_def_filter_apply(
	const gchar         *name,
	const gchar *const  *args,
	const gchar         *value,
	gint64               now_us,
	AxisDefRegexCache   *regex,
	gchar              **problem
);

/**
 * axis_def_parse_date:
 * @text: a field's text: ISO 8601, RFC 1123, seconds (or milliseconds)
 *   since 1970, "now", "today", "yesterday", "5 hours ago", "Today
 *   12:25am", and a few common layouts ("yyyy-MM-dd", "dd.MM.yyyy",
 *   "dd MMM yyyy" ...)
 * @now_us: now, for the relative forms
 *
 * Returns: microseconds since 1970 in UTC, or 0 when it is not a date
 */
gint64
axis_def_parse_date(
	const gchar *text,
	gint64       now_us
);

/* A .NET layout read against @text: microseconds, or 0 when it does not
 * fit. */
gint64
axis_def_parse_layout(
	const gchar *layout,
	const gchar *text,
	gint64       now_us
);

/* A size as sites write it ("1.5 GB", "700 MiB", "1,234.5 KB", "123456"):
 * bytes, or -1 when it is not one.  KB, MB, GB, TB and PB are binary, as
 * the definitions mean them. */
gint64
axis_def_parse_size(const gchar *text);

/* A count ("1,234", "12"): the number, or -1 when there is none. */
gint64
axis_def_parse_count(const gchar *text);

/* Form encoding as .NET's WebUtility.UrlEncode writes it: a space as "+",
 * letters, digits and "-_.!*()" kept, every other byte %XX. */
gchar *
axis_def_url_encode(const gchar *text);

/* The reverse: "+" as a space, %XX as its byte (the result made valid
 * UTF-8). */
gchar *
axis_def_url_decode(const gchar *text);

G_END_DECLS

#endif /* AXIS_DEF_FILTER_H */
