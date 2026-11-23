/*
 * axis-def-template.h - the Go-template subset indexer definitions write
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition builds its search paths, inputs and field values from text
 * with "{{ }}" actions in it.  What the public definitions use
 * (found by a survey of them) is read here, and the
 * rest is "not supported", named:
 *
 *  - {{ if X }} ... {{ else if Y }} ... {{ else }} ... {{ end }};
 *  - {{ range X }} ... {{ . }} ... {{ end }} over a list (.Categories);
 *  - the functions and, or, not, eq, ne, join (a list and a separator) and
 *    re_replace (text, a pattern, a replacement: indexer/def/axis-def-regex.h);
 *  - parentheses, quoted text ("..." or `...`; a backslash is itself, but
 *    \" is a quote), numbers, true and false;
 *  - the variables .Keywords, .Query.*, .Config.*, .Result.*, .Categories,
 *    .True, .False and .Today.Year (with .Today.Month and .Today.Day); one
 *    that has no value is empty;
 *  - "{{-" and "-}}", which drop the blanks beside them.
 *
 * Values are text, or a list of texts (.Categories); text is true when it is
 * not empty, a list when it has items.  and and or return one of their
 * arguments, as Go's do.  An action's output may be passed through an
 * escaping function (a search path's values are URL-encoded); the
 * template's own text is not.
 *
 * Limits: a template of at most %AXIS_TEMPLATE_MAX_BYTES and
 * %AXIS_TEMPLATE_MAX_NODES parts, nested %AXIS_TEMPLATE_MAX_DEPTH deep; an
 * output of at most %AXIS_TEMPLATE_MAX_OUTPUT bytes (cut there); a range of
 * at most %AXIS_TEMPLATE_MAX_RANGE items.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-template.c.
 */

#ifndef AXIS_DEF_TEMPLATE_H
#define AXIS_DEF_TEMPLATE_H

#include <glib.h>

#include "indexer/def/axis-def-regex.h"

G_BEGIN_DECLS

#define AXIS_TEMPLATE_MAX_BYTES   (16 * 1024)
#define AXIS_TEMPLATE_MAX_NODES   4096
#define AXIS_TEMPLATE_MAX_DEPTH   24
#define AXIS_TEMPLATE_MAX_OUTPUT  (64 * 1024)
#define AXIS_TEMPLATE_MAX_RANGE   256

typedef struct _AxisTemplate AxisTemplate;

/**
 * AxisTemplateVars:
 * @values: variable name without its dot ("Keywords", "Config.sort",
 *   "Result.title") -> text; borrowed
 * @categories: (nullable) (element-type utf8): .Categories; borrowed
 *
 * What a template is rendered with.
 */
typedef struct
{
	GHashTable *values;
	GPtrArray  *categories;
} AxisTemplateVars;

/* Applied to an action's output (a search path's values are
 * URL-encoded); returns a new string. */
typedef gchar *(*AxisTemplateEscape)(const gchar *text);

/**
 * axis_template_parse:
 * @text: the template
 * @problem: (out) (optional) (transfer full): why it was not read, with
 *   none of @text in it
 * @unsupported: (out) (optional): the reason is a feature that is not
 *   supported (not a mistake in the text)
 *
 * Returns: (transfer full) (nullable)
 */
AxisTemplate *
axis_template_parse(
	const gchar  *text,
	gchar       **problem,
	gboolean     *unsupported
);

void
axis_template_free(AxisTemplate *template_);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTemplate, axis_template_free)

/* Whether @text has an action in it at all ("{{"). */
gboolean
axis_template_has_actions(const gchar *text);

/**
 * axis_template_render:
 * @escape: (nullable): applied to each action's output
 * @regex: what re_replace runs in
 * @problem: (out) (optional) (transfer full): why it failed (a pattern that
 *   is refused, the regular expressions' budget spent)
 *
 * Returns: (transfer full) (nullable): the text, or %NULL with @problem
 */
gchar *
axis_template_render(
	const AxisTemplate      *template_,
	const AxisTemplateVars  *vars,
	AxisTemplateEscape       escape,
	AxisDefRegexCache       *regex,
	gchar                  **problem
);

/* Parses and renders @text in one go (a template used once); %NULL with
 * @problem when it cannot be read or rendered. */
gchar *
axis_template_expand(
	const gchar             *text,
	const AxisTemplateVars  *vars,
	AxisTemplateEscape       escape,
	AxisDefRegexCache       *regex,
	gchar                  **problem
);

G_END_DECLS

#endif /* AXIS_DEF_TEMPLATE_H */
