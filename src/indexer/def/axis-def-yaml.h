/*
 * axis-def-yaml.h - a reader for the subset of YAML that indexer definitions
 * are written in
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An indexer definition (indexer/def/axis-def.h) is a YAML file written by
 * people Axis does not know: it is read as hostile text.  This reader takes
 * what the public definitions use (found by a survey of them)
 * and refuses the rest, with a reason that names the line:
 *
 *  - one document, optionally opened by "---" and closed by "...";
 *  - block maps and lists, in the compact form too ("- name: x" on one line,
 *    a list at its key's indentation);
 *  - flow lists and maps ("[q, season, ep]", "{id: 1, cat: TV/HD}"), over
 *    several lines;
 *  - plain scalars (over several lines, folded), single-quoted ('' is a
 *    quote) and double-quoted scalars with every YAML escape;
 *  - comments.
 *
 * Refused: anchors ("&a"), aliases ("*a"), tags ("!x"), block scalars ("|",
 * ">"), directives ("%YAML"), a second document, a key written twice in a
 * map, a tab in indentation, a NUL character (also as "\0" or "\x00"), text
 * that is not UTF-8.  Limits, each refusing the whole text: at most
 * %AXIS_YAML_MAX_BYTES of input, %AXIS_YAML_MAX_DEPTH levels of maps and
 * lists, %AXIS_YAML_MAX_NODES nodes, and %AXIS_YAML_MAX_SCALAR bytes in one
 * scalar.  Nothing is resolved but the text: a scalar keeps what was written
 * (numbers, booleans and null are read on demand by the accessors).
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-yaml.c.
 */

#ifndef AXIS_DEF_YAML_H
#define AXIS_DEF_YAML_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_YAML_MAX_BYTES   (256 * 1024)
#define AXIS_YAML_MAX_DEPTH   32
#define AXIS_YAML_MAX_NODES   100000
#define AXIS_YAML_MAX_SCALAR  (64 * 1024)

typedef enum
{
	AXIS_YAML_SCALAR,
	AXIS_YAML_MAP,
	AXIS_YAML_SEQ
} AxisYamlKind;

typedef struct _AxisYamlNode AxisYamlNode;

/**
 * AxisYamlNode:
 * @kind: what it is
 * @quoted: a scalar written in quotes (never null, a boolean or a number)
 * @text: a scalar's text, valid UTF-8 without NUL ("" for an empty one)
 * @line: the line it starts on, from 1
 * @keys: (element-type utf8): a map's keys, in the order written
 * @values: (element-type AxisYamlNode): a map's values, or a list's items
 */
struct _AxisYamlNode
{
	AxisYamlKind  kind;
	gboolean      quoted;
	gchar        *text;
	guint         line;
	GPtrArray    *keys;
	GPtrArray    *values;
};

/**
 * axis_yaml_parse:
 * @data: the text (need not end in NUL)
 * @length: its length in bytes
 * @problem: (out) (optional) (transfer full): why it was refused: "line 12:
 *   anchors are not supported", with none of the text in it
 *
 * Returns: (transfer full) (nullable): the document's root, or %NULL
 */
AxisYamlNode *
axis_yaml_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

void
axis_yaml_node_free(AxisYamlNode *node);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisYamlNode, axis_yaml_node_free)

/* The number of pairs of a map or items of a list; 0 for anything else. */
guint
axis_yaml_length(const AxisYamlNode *node);

/* A map's value of @key, or %NULL (also for a node that is not a map). */
const AxisYamlNode *
axis_yaml_get(
	const AxisYamlNode *map,
	const gchar        *key
);

/* The @index-th key of a map, or %NULL. */
const gchar *
axis_yaml_key_at(
	const AxisYamlNode *map,
	guint               index
);

/* The @index-th value of a map or item of a list, or %NULL. */
const AxisYamlNode *
axis_yaml_at(
	const AxisYamlNode *node,
	guint               index
);

/* A scalar's text, or %NULL for a map, a list, a null scalar or %NULL. */
const gchar *
axis_yaml_text(const AxisYamlNode *node);

/* Whether @node is a plain scalar meaning null: empty, "~" or "null". */
gboolean
axis_yaml_is_null(const AxisYamlNode *node);

/* A plain "true" or "false" (any case of its first letter, or all upper). */
gboolean
axis_yaml_get_bool(
	const AxisYamlNode *node,
	gboolean           *value
);

G_END_DECLS

#endif /* AXIS_DEF_YAML_H */
