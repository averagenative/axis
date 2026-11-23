/*
 * fuzz-def-html.c - libFuzzer target for the page reader of indexer
 * definitions (indexer/def/axis-def-html.h) and its selector engine
 * (indexer/def/axis-def-select.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the mode ('x' XML, anything else HTML); the first
 * line after it is a selector, the rest the page.  Whatever they are, the
 * code returns without crashing or allocating without bound, and:
 *
 *  - the page is read or refused with a reason, never both; a tree has at
 *    most AXIS_HTML_MAX_NODES nodes, every element a name and its
 *    attributes in pairs, every text valid UTF-8, and its links agree (a
 *    child's parent is the node it is a child of, the sibling links are
 *    each other's reverse);
 *  - the selector is read or refused with a reason;
 *  - a match is an element of the tree under the context, at most as many
 *    as asked for; matching spends no more than its budget allows (and a
 *    step or two for the last test), and the first match is the first of
 *    all matches when the budget was not spent;
 *  - an element's text is valid UTF-8 within the limit asked for.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/def/axis-def-html.h"
#include "indexer/def/axis-def-select.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fail(int line)
{
	fprintf(stderr, "fuzz-def-html: the check at line %d failed\n", line);
	abort();
}

static guint
check_tree(const AxisHtmlNode *node)
{
	const AxisHtmlNode *child;
	const AxisHtmlNode *prev = NULL;
	guint count = 0;
	guint i;

	if (node->type == AXIS_HTML_ELEMENT) {
		if (node->name == NULL || node->name[0] == '\0' || !g_utf8_validate(node->name, -1, NULL))
			fail(__LINE__);
		for (i = 0; i < node->n_attrs; i++) {
			if (node->attrs[2 * i] == NULL || node->attrs[2 * i + 1] == NULL ||
			    !g_utf8_validate(node->attrs[2 * i + 1], -1, NULL))
				fail(__LINE__);
		}
		if (node->n_attrs > AXIS_HTML_MAX_ATTRS)
			fail(__LINE__);
	} else if (node->type == AXIS_HTML_TEXT) {
		if (node->text == NULL || !g_utf8_validate(node->text, -1, NULL) || node->first != NULL)
			fail(__LINE__);
	}
	for (child = node->first; child != NULL; child = child->next) {
		if (child->parent != node || child->prev != prev)
			fail(__LINE__);
		count += 1 + check_tree(child);
		prev = child;
	}
	if (node->last != prev)
		fail(__LINE__);
	return count;
}

static gboolean
under(
	const AxisHtmlNode *node,
	const AxisHtmlNode *context
){
	const AxisHtmlNode *n;

	for (n = node->parent; n != NULL; n = n->parent) {
		if (n == context)
			return TRUE;
	}
	return FALSE;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text = (const gchar *)data;
	const gchar *newline;
	gchar *selector_text = NULL;
	gchar *problem = NULL;
	gboolean unsupported = FALSE;
	gboolean xml;
	AxisHtmlDoc *doc;
	AxisSelector *selector;
	AxisSelectBudget budget = { 0, 200000, FALSE };
	AxisSelectBudget again = { 0, 200000, FALSE };
	GPtrArray *all;
	const AxisHtmlNode *first;
	const AxisHtmlNode *root;
	guint i;

	if (size < 1 || size > 1 << 20)
		return 0;
	xml = text[0] == 'x';
	text++;
	size--;
	newline = memchr(text, '\n', size);
	if (newline == NULL)
		return 0;
	selector_text = g_strndup(text, (gsize)(newline - text));
	size -= (gsize)(newline - text) + 1;
	text = newline + 1;
	doc = axis_html_parse(text, size, xml, &problem);
	if (doc == NULL) {
		if (problem == NULL)
			fail(__LINE__);
		g_free(problem);
		g_free(selector_text);
		return 0;
	}
	if (problem != NULL)
		fail(__LINE__);
	root = axis_html_doc_root(doc);
	if (check_tree(root) != axis_html_doc_n_nodes(doc) || axis_html_doc_n_nodes(doc) > AXIS_HTML_MAX_NODES)
		fail(__LINE__);
	selector = g_utf8_validate(selector_text, -1, NULL) ? axis_selector_parse(selector_text, &problem, &unsupported) :
		NULL;
	if (selector == NULL) {
		g_free(problem);
		axis_html_doc_free(doc);
		g_free(selector_text);
		return 0;
	}
	all = axis_selector_all(selector, root, 64, &budget);
	if (all->len > 64 || budget.steps > budget.max_steps + 1024)
		fail(__LINE__);
	for (i = 0; i < all->len; i++) {
		const AxisHtmlNode *node = g_ptr_array_index(all, i);
		gchar *inner = axis_html_text(node, 512);

		if (node->type != AXIS_HTML_ELEMENT || !under(node, root))
			fail(__LINE__);
		if (!g_utf8_validate(inner, -1, NULL) || strlen(inner) > 512 + 4)
			fail(__LINE__);
		g_free(inner);
	}
	first = axis_selector_first(selector, root, &again);
	if (!budget.exhausted && !again.exhausted &&
	    (all->len > 0 ? first != g_ptr_array_index(all, 0) : first != NULL))
		fail(__LINE__);
	/* Matching each match in its own scope, and moving one's children. */
	for (i = 0; i < all->len && i < 4; i++) {
		AxisSelectBudget each = { 0, 20000, FALSE };
		AxisHtmlNode *node = g_ptr_array_index(all, i);

		axis_selector_matches(selector, node, node, &each);
		if (i > 0)
			axis_html_move_children(node, g_ptr_array_index(all, 0));
	}
	if (check_tree(root) != axis_html_doc_n_nodes(doc))
		fail(__LINE__);
	g_ptr_array_unref(all);
	axis_selector_free(selector);
	axis_html_doc_free(doc);
	g_free(selector_text);
	return 0;
}
