/*
 * axis-def-regex.h - the regular expressions of indexer definitions, run
 * under bounds
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition's patterns are written for .NET's engine; they run here on
 * GLib's (PCRE2), which reads the forms definitions use the same way
 * (classes, groups, (?i), look-ahead and look-behind, \p{..}).  A
 * replacement is .NET's: "$1", "${1}", "${name}", "$0" and "$&" for the
 * match, "$$" for a dollar; a backslash is itself.  A reference to a group
 * the pattern does not have is left as written, as .NET does.
 *
 * A pattern is hostile: one written to backtrack forever must not hold a
 * search.  What bounds it:
 *
 *  - a pattern of at most %AXIS_DEF_REGEX_MAX_PATTERN bytes, refused if it
 *    holds "(*" (PCRE2's verbs and start-of-pattern options, so that the
 *    limits below are the ones in force) or "\C";
 *  - every pattern is compiled with "(*LIMIT_MATCH=n)(*LIMIT_DEPTH=n)
 *    (*LIMIT_HEAP=n)" in front (%AXIS_DEF_REGEX_MATCH_LIMIT,
 *    %AXIS_DEF_REGEX_DEPTH_LIMIT, %AXIS_DEF_REGEX_HEAP_KIB), which PCRE2
 *    applies to each match attempt: an attempt past them ends as an error,
 *    and the filter fails;
 *  - a subject is cut to %AXIS_DEF_REGEX_MAX_SUBJECT bytes first, so that
 *    the attempts of one call (one at each starting position at the most)
 *    are bounded too;
 *  - the time all the matches of one answer take is added up, and once it
 *    passes the budget the cache was made with, every later match fails
 *    ("exhausted"): the answer is refused as taking too long.
 *
 * Compiled patterns are kept in the cache by their text, so the rows of an
 * answer compile each pattern once.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-filter.c.
 */

#ifndef AXIS_DEF_REGEX_H
#define AXIS_DEF_REGEX_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_DEF_REGEX_MAX_PATTERN  2048
#define AXIS_DEF_REGEX_MAX_SUBJECT  (16 * 1024)
#define AXIS_DEF_REGEX_MATCH_LIMIT  20000
#define AXIS_DEF_REGEX_DEPTH_LIMIT  2000
#define AXIS_DEF_REGEX_HEAP_KIB     4096

/* The most patterns a cache keeps compiled. */
#define AXIS_DEF_REGEX_MAX_CACHED   512

/* The time the matches of one answer may take in all, by default. */
#define AXIS_DEF_REGEX_BUDGET_US    (3 * G_USEC_PER_SEC)

typedef struct _AxisDefRegexCache AxisDefRegexCache;

AxisDefRegexCache *
axis_def_regex_cache_new(gint64 budget_us);

void
axis_def_regex_cache_free(AxisDefRegexCache *cache);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDefRegexCache, axis_def_regex_cache_free)

/* Whether the time budget has run out. */
gboolean
axis_def_regex_cache_exhausted(const AxisDefRegexCache *cache);

/* The time the matches have taken so far, in microseconds. */
gint64
axis_def_regex_cache_spent_us(const AxisDefRegexCache *cache);

/**
 * axis_def_regex_check:
 * @problem: (out) (optional) (transfer full): why it cannot be used, with
 *   none of the pattern in it
 *
 * Whether @pattern is accepted and compiles.
 */
gboolean
axis_def_regex_check(
	const gchar  *pattern,
	gchar       **problem
);

/**
 * axis_def_regex_replace:
 * @replacement: in .NET's syntax
 *
 * Every match of @pattern in @subject replaced.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL with @problem
 *   (a pattern that is refused or does not compile, a match past the
 *   limits, the budget spent)
 */
gchar *
axis_def_regex_replace(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	const gchar        *replacement,
	gchar             **problem
);

/* The text of group @group of the first match of @pattern in @subject; ""
 * when there is no match or no such group; %NULL with @problem as above. */
gchar *
axis_def_regex_group(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	gint                group,
	gchar             **problem
);

/* Whether @pattern matches @subject; %FALSE with @problem set on failure. */
gboolean
axis_def_regex_matches(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	gchar             **problem
);

G_END_DECLS

#endif /* AXIS_DEF_REGEX_H */
