/*
 * axis-def-trace.h - an account, step by step, of how a definition read a
 * site's answers: for a person repairing a definition
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The engine fills a trace while it builds a search and reads its answers
 * (axis_def_search_set_trace(), indexer/def/axis-def-engine.h), when one is
 * given; the "Check this indexer" page and axisd --check-definition show it.
 * What it records:
 *
 *  - the definition (its id and version) and the words searched for;
 *  - each request the search makes, as a path on the site;
 *  - for each answer: its path, its status and size (the caller's, as it
 *    came), how it was read (HTML, XML, JSON), whether it was a browser
 *    challenge, whether the part every answer has was found, how many rows
 *    the row selector matched, how many were kept, lost for a field every
 *    result must have, or left out on purpose (not video, not every word,
 *    listed twice, a download only elsewhere), and the verdict;
 *  - for the first %AXIS_DEF_TRACE_ROWS rows: each field's selector (or
 *    template), what it read, what its filters made of it, or that it
 *    matched nothing, and the row's fate;
 *  - when excerpts are asked for, a bounded piece of each traced row's
 *    markup (or JSON), and of the page when no row matched.
 *
 * Everything in it is text from the site or the definition, cleaned (valid
 * UTF-8, no control characters) and cut: it is to be shown as text, and
 * nothing in it is ever followed or fetched.  A trace holds no key (the sites
 * definitions run take none) and no address other than paths on the site.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-trace.c.
 */

#ifndef AXIS_DEF_TRACE_H
#define AXIS_DEF_TRACE_H

#include <glib.h>

G_BEGIN_DECLS

/* The rows of an answer whose fields are recorded, the answers recorded,
 * the characters of a value kept, the characters of an excerpt (one that is
 * cut ends in "..."). */
#define AXIS_DEF_TRACE_ROWS           5
#define AXIS_DEF_TRACE_MAX_ANSWERS    16
#define AXIS_DEF_TRACE_VALUE_CHARS    200
#define AXIS_DEF_TRACE_EXCERPT_BYTES  1500

/**
 * AxisDefTraceField:
 * @name: the field's name ("title", "_cut")
 * @how: how it is read ("selector td:nth-child(3)", "attribute href of
 *   div.tt-name > a", "path seeders", "text {{ .Result.size }}")
 * @read: (nullable): what the selector read, before the filters
 * @value: (nullable): what the field's value became
 * @note: (nullable): "matched nothing", "matched nothing: optional, so its
 *   default", "a filter failed: ..."
 */
typedef struct
{
	gchar *name;
	gchar *how;
	gchar *read;
	gchar *value;
	gchar *note;
} AxisDefTraceField;

/**
 * AxisDefTraceRow:
 * @number: the row's place in the answer, from 1
 * @fields: (element-type AxisDefTraceField): in the definition's order, up
 *   to the field that lost the row
 * @fate: "kept", or why it was left out ("left out: the field seeders
 *   matched nothing")
 * @excerpt: (nullable): its markup or JSON, cut, when excerpts are asked for
 */
typedef struct
{
	guint      number;
	GPtrArray *fields;
	gchar     *fate;
	gchar     *excerpt;
} AxisDefTraceRow;

/**
 * AxisDefTraceAnswer:
 * @path: the request's path on the site
 * @status: the answer's status (0 when the caller did not say)
 * @length: its size in bytes
 * @kind: "HTML", "XML" or "JSON": how it was read
 * @challenge: it was a browser challenge's page
 * @expect: (nullable): the part every answer has, as the definition names it
 * @expect_found: it was found
 * @rows_selector: (nullable): the row selector (or path)
 * @rows_matched, @kept, @lost, @left_out: how many rows the selector
 *   matched, were kept, were lost for a field every result must have, and
 *   were left out on purpose
 * @not_video: of @left_out, those whose category is not video
 * @verdict: what came of the answer: "read: 12 releases", "no results",
 *   "its pages no longer look as expected: ...", "refused: ..."
 * @excerpt: (nullable): a piece of the page, when no row matched and
 *   excerpts are asked for
 * @rows: (element-type AxisDefTraceRow): the first rows
 */
typedef struct
{
	gchar     *path;
	guint      status;
	gsize      length;
	gchar     *kind;
	gboolean   challenge;
	gchar     *expect;
	gboolean   expect_found;
	gchar     *rows_selector;
	guint      rows_matched;
	guint      kept;
	guint      lost;
	guint      left_out;
	guint      not_video;
	gchar     *verdict;
	gchar     *excerpt;
	GPtrArray *rows;
} AxisDefTraceAnswer;

/**
 * AxisDefTrace:
 * @excerpts: excerpts are recorded
 * @definition: (nullable): "axis-limetorrents, version 3"
 * @words: (nullable): the words searched for
 * @requests: (element-type utf8): the paths the search asks
 * @answers: (element-type AxisDefTraceAnswer)
 * @outcome: (nullable): how it ended ("12 releases", or why it failed)
 * @download: (nullable): for a release's page read at grab time, what was
 *   found there
 */
typedef struct _AxisDefTrace AxisDefTrace;

struct _AxisDefTrace
{
	gboolean   excerpts;
	gchar     *definition;
	gchar     *words;
	GPtrArray *requests;
	GPtrArray *answers;
	gchar     *outcome;
	gchar     *download;
};

AxisDefTrace *
axis_def_trace_new(gboolean excerpts);

void
axis_def_trace_free(AxisDefTrace *trace);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDefTrace, axis_def_trace_free)

/* @text cleaned and cut to @max_chars characters ("..." when cut), never
 * %NULL: what a trace keeps of anything. */
gchar *
axis_def_trace_text(
	const gchar *text,
	guint        max_chars
);

/* A new answer at the end of @trace (%NULL when it holds the most already). */
AxisDefTraceAnswer *
axis_def_trace_add_answer(
	AxisDefTrace *trace,
	const gchar  *path,
	gsize         length
);

/* A new row at the end of @answer's (%NULL when it holds the first
 * %AXIS_DEF_TRACE_ROWS already, or @answer is %NULL). */
AxisDefTraceRow *
axis_def_trace_add_row(
	AxisDefTraceAnswer *answer,
	guint               number
);

/* A field at the end of @row's, each text cleaned and cut (nullable ones
 * left out). */
void
axis_def_trace_add_field(
	AxisDefTraceRow *row,
	const gchar     *name,
	const gchar     *how,
	const gchar     *read,
	const gchar     *value,
	const gchar     *note
);

/* The last answer, or %NULL. */
AxisDefTraceAnswer *
axis_def_trace_last(AxisDefTrace *trace);

/* A request that got no answer to read: its path and why, as an answer
 * whose verdict says so. */
void
axis_def_trace_add_failure(
	AxisDefTrace *trace,
	const gchar  *path,
	guint         status,
	const gchar  *why
);

/* Sets @trace's outcome (replacing one set before). */
void
axis_def_trace_set_outcome(
	AxisDefTrace *trace,
	const gchar  *outcome
);

/**
 * axis_def_trace_to_text:
 *
 * The account as plain text, one step a line, indented by what it belongs
 * to: what axisd --check-definition prints.
 */
gchar *
axis_def_trace_to_text(const AxisDefTrace *trace);

G_END_DECLS

#endif /* AXIS_DEF_TRACE_H */
