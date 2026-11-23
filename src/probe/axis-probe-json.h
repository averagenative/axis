/*
 * axis-probe-json.h - a JSON reader that cannot be made to use much memory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ffprobe's output is data from outside: it carries whatever a file's
 * author put in its tags.  This reader accepts strict JSON (RFC 8259) and
 * keeps everything it reads inside fixed limits, so that the memory it
 * takes is bounded by the limits and not by what the input says:
 *
 *  - nesting at most %AXIS_JSON_MAX_DEPTH deep (the recursion is bounded);
 *  - at most %AXIS_JSON_MAX_NODES values in all;
 *  - a string keeps its first %AXIS_JSON_MAX_STRING bytes (cut at a
 *    character boundary); a number may have at most %AXIS_JSON_MAX_NUMBER
 *    characters;
 *  - every string it hands out is valid UTF-8 without NUL: bytes that are
 *    not valid UTF-8 become U+FFFD, "\u0000" does too, and a lone
 *    surrogate escape does too.
 *
 * Input that breaks a limit or the grammar is refused whole: there is no
 * partial result.  A duplicate key keeps both members; lookups find the
 * first.  Numbers are kept as written and read on demand, so a value can
 * be a number or a string holding one (ffprobe prints both).
 *
 * GLib only, no I/O: this is read by the fuzz target
 * (tests/fuzz/fuzz-probe-map.c) through the mapper.
 */

#ifndef AXIS_PROBE_JSON_H
#define AXIS_PROBE_JSON_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_JSON_MAX_DEPTH    16
#define AXIS_JSON_MAX_NODES    400000
#define AXIS_JSON_MAX_STRING   8192
#define AXIS_JSON_MAX_NUMBER   64

typedef enum
{
	AXIS_JSON_NULL,
	AXIS_JSON_BOOLEAN,
	AXIS_JSON_NUMBER,
	AXIS_JSON_STRING,
	AXIS_JSON_ARRAY,
	AXIS_JSON_OBJECT
} AxisJsonType;

typedef struct _AxisJsonDoc  AxisJsonDoc;
typedef struct _AxisJsonNode AxisJsonNode;

/**
 * axis_json_parse:
 * @data: the text (need not end in NUL)
 * @length: its length in bytes
 * @problem: (out) (optional) (transfer full): why it was refused, a short
 *   sentence without any of the input in it
 *
 * Returns: (transfer full) (nullable): the document, or %NULL if the input
 *   is not one JSON value within the limits
 */
AxisJsonDoc *
axis_json_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

void
axis_json_doc_free(AxisJsonDoc *doc);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisJsonDoc, axis_json_doc_free)

/* Returns: (transfer none): the top value */
const AxisJsonNode *
axis_json_doc_get_root(AxisJsonDoc *doc);

/* The type of @node; %AXIS_JSON_NULL for %NULL. */
AxisJsonType
axis_json_type(const AxisJsonNode *node);

/* Array: the number of elements.  Object: the number of members.  0 for
 * anything else. */
guint
axis_json_length(const AxisJsonNode *node);

/* Returns: the @index-th element of an array, or %NULL */
const AxisJsonNode *
axis_json_element(
	const AxisJsonNode *node,
	guint               index
);

/* The @index-th member of an object, in the order written. */
const gchar *
axis_json_member_name(
	const AxisJsonNode *node,
	guint               index
);

const AxisJsonNode *
axis_json_member_value(
	const AxisJsonNode *node,
	guint               index
);

/* Returns: the first member of an object called @name, or %NULL (also for
 * a node that is not an object) */
const AxisJsonNode *
axis_json_member(
	const AxisJsonNode *node,
	const gchar        *name
);

/* Returns: (transfer none): the text of a string node, else %NULL */
const gchar *
axis_json_string(const AxisJsonNode *node);

/* The text of a string, or of a number as written; %NULL otherwise. */
const gchar *
axis_json_scalar_text(const AxisJsonNode *node);

/**
 * axis_json_get_integer:
 * @node: a number, or a string holding a whole number in decimal
 * @minimum: the smallest value accepted
 * @maximum: the largest value accepted
 * @value: (out): the value
 *
 * Returns: %TRUE only if the node holds a whole number within the range
 *   (an out-of-range or fractional number, "N/A" and the like are not
 *   values)
 */
gboolean
axis_json_get_integer(
	const AxisJsonNode *node,
	gint64              minimum,
	gint64              maximum,
	gint64             *value
);

/* As above for a finite number (or a string holding one) in [@minimum,
 * @maximum]. */
gboolean
axis_json_get_double(
	const AxisJsonNode *node,
	gdouble             minimum,
	gdouble             maximum,
	gdouble            *value
);

/* A boolean, or the strings "true" and "false", or 0 and 1. */
gboolean
axis_json_get_boolean(
	const AxisJsonNode *node,
	gboolean           *value
);

G_END_DECLS

#endif /* AXIS_PROBE_JSON_H */
