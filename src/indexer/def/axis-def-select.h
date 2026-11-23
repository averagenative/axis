/*
 * axis-def-select.h - the CSS selectors indexer definitions write, over a
 * page read by indexer/def/axis-def-html.h
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What the public definitions use (found by a survey of them), and
 * nothing else: type selectors and "*", classes, ids, the
 * attribute tests [a], [a=v], [a^=v], [a$=v], [a*=v], [a~=v], [a|=v],
 * [a!=v] (with " i" for case), the combinators descendant, ">", "+", "~"
 * and ",", and :has() (with a ">", "+" or "~" first inside), :not(),
 * :contains(), :nth-child(), :nth-last-child(), :nth-of-type(),
 * :nth-last-of-type(), :first-child, :last-child, :first-of-type,
 * :last-of-type, :only-child, :empty, :root and :scope.  Anything else is
 * "not supported", named ("the selector :eq()"), apart from text that is no
 * selector at all.  Names compare without regard to case.
 *
 * Limits: a selector of at most %AXIS_SELECT_MAX_BYTES, %AXIS_SELECT_MAX_PARTS
 * compound parts and :has()/:not() nested %AXIS_SELECT_MAX_NESTING deep.
 * Matching counts its steps against a budget the caller gives
 * (#AxisSelectBudget), so that a selector that would take long on a large
 * page (":has()" inside ":has()" on every element) stops, saying so, instead
 * of taking long.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-html.c.
 */

#ifndef AXIS_DEF_SELECT_H
#define AXIS_DEF_SELECT_H

#include <glib.h>

#include "indexer/def/axis-def-html.h"

G_BEGIN_DECLS

#define AXIS_SELECT_MAX_BYTES    4096
#define AXIS_SELECT_MAX_PARTS    64
#define AXIS_SELECT_MAX_NESTING  8

/* The most text an element's :contains() looks in. */
#define AXIS_SELECT_CONTAINS_BYTES (256 * 1024)

/**
 * AxisSelectBudget:
 * @steps: spent so far
 * @max_steps: the most allowed
 * @exhausted: set when @max_steps was reached: every match after it fails
 *
 * What matching may cost: one step for each element looked at, and one for
 * each kilobyte of text :contains() reads.
 */
typedef struct
{
	guint64  steps;
	guint64  max_steps;
	gboolean exhausted;
} AxisSelectBudget;

typedef struct _AxisSelector AxisSelector;

/**
 * axis_selector_parse:
 * @text: the selector
 * @problem: (out) (optional) (transfer full): why it was not read: a
 *   sentence that names what is not supported, or says the text is not a
 *   selector; none of @text in it
 * @unsupported: (out) (optional): the reason is a feature that is not
 *   supported (not a mistake in the text)
 *
 * Returns: (transfer full) (nullable): the selector
 */
AxisSelector *
axis_selector_parse(
	const gchar  *text,
	gchar       **problem,
	gboolean     *unsupported
);

void
axis_selector_free(AxisSelector *selector);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSelector, axis_selector_free)

/**
 * axis_selector_matches:
 * @element: an element
 * @scope: (nullable): what :scope is (the element a field is read in)
 *
 * Whether @element matches.
 */
gboolean
axis_selector_matches(
	const AxisSelector *selector,
	const AxisHtmlNode *element,
	const AxisHtmlNode *scope,
	AxisSelectBudget   *budget
);

/* The first descendant of @context (in document order) that matches, with
 * :scope @context; %NULL for none. */
const AxisHtmlNode *
axis_selector_first(
	const AxisSelector *selector,
	const AxisHtmlNode *context,
	AxisSelectBudget   *budget
);

/* Every descendant of @context that matches, in document order, at most
 * @max (element-type AxisHtmlNode, borrowed). */
GPtrArray *
axis_selector_all(
	const AxisSelector *selector,
	const AxisHtmlNode *context,
	guint               max,
	AxisSelectBudget   *budget
);

G_END_DECLS

#endif /* AXIS_DEF_SELECT_H */
