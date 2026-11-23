/*
 * axis-def-html.h - a forgiving reader of the HTML (and XML) pages a site
 * answers with, into a tree for selectors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A site's page is hostile text.  This reader builds the tree a browser
 * would build closely enough for the selectors definitions write (the tags
 * a page leaves open are closed where HTML implies, a table's rows get the
 * tbody a browser gives them), and nothing more: no script is run, no style
 * is read, nothing the page names is fetched.  Script and style contents
 * are kept as text, never parsed.  Limits, each refusing the page: at most
 * %AXIS_HTML_MAX_BYTES of input and %AXIS_HTML_MAX_NODES nodes; deeper than
 * %AXIS_HTML_MAX_DEPTH elements, new ones are put beside the deepest
 * instead of inside it; an element keeps at most %AXIS_HTML_MAX_ATTRS
 * attributes (the first of a name).  Text is valid UTF-8 without NUL: a byte
 * that is not UTF-8 or a NUL becomes U+FFFD.  Entities: the numeric ones and
 * a fixed table of named ones (an unknown one is left as written).
 *
 * In XML mode (an RSS or Torznab feed) there are no void elements and no
 * implied ends, names keep their case, "<x/>" is empty and CDATA is text.
 *
 * GLib only, no I/O: read by tests/fuzz/fuzz-def-html.c.
 */

#ifndef AXIS_DEF_HTML_H
#define AXIS_DEF_HTML_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_HTML_MAX_BYTES  (8 * 1024 * 1024)
#define AXIS_HTML_MAX_NODES  400000
#define AXIS_HTML_MAX_DEPTH  256
#define AXIS_HTML_MAX_ATTRS  64

typedef enum
{
	AXIS_HTML_DOCUMENT,
	AXIS_HTML_ELEMENT,
	AXIS_HTML_TEXT
} AxisHtmlNodeType;

typedef struct _AxisHtmlNode AxisHtmlNode;

/**
 * AxisHtmlNode:
 * @type: what it is
 * @name: an element's name (lower case in HTML mode)
 * @text: a text node's text
 * @n_attrs: an element's number of attributes
 * @attrs: their names and values, alternately (names lower case in HTML)
 * @excluded: set while a field's text is read without the parts its
 *   "remove" selector names
 *
 * A node of the tree; the links are the tree's.
 */
struct _AxisHtmlNode
{
	AxisHtmlNodeType  type;
	gchar            *name;
	gchar            *text;
	guint             n_attrs;
	gchar           **attrs;
	gboolean          excluded;
	AxisHtmlNode     *parent;
	AxisHtmlNode     *first;
	AxisHtmlNode     *last;
	AxisHtmlNode     *prev;
	AxisHtmlNode     *next;
};

typedef struct _AxisHtmlDoc AxisHtmlDoc;

/**
 * axis_html_parse:
 * @data: the page, UTF-8 (a site's own encoding is converted first)
 * @length: its length in bytes
 * @xml: read as XML
 * @problem: (out) (optional) (transfer full): why it was refused
 *
 * Returns: (transfer full) (nullable): the tree
 */
AxisHtmlDoc *
axis_html_parse(
	const gchar  *data,
	gsize         length,
	gboolean      xml,
	gchar       **problem
);

void
axis_html_doc_free(AxisHtmlDoc *doc);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisHtmlDoc, axis_html_doc_free)

/* The document node, the parent of the top elements. */
AxisHtmlNode *
axis_html_doc_root(AxisHtmlDoc *doc);

/* How many nodes the tree has. */
guint
axis_html_doc_n_nodes(AxisHtmlDoc *doc);

/* An element's attribute (by its lower-case name in HTML), or %NULL. */
const gchar *
axis_html_attr(
	const AxisHtmlNode *node,
	const gchar        *name
);

/* The next and previous element siblings, the first element child. */
AxisHtmlNode *
axis_html_next_element(const AxisHtmlNode *node);

AxisHtmlNode *
axis_html_prev_element(const AxisHtmlNode *node);

AxisHtmlNode *
axis_html_first_element(const AxisHtmlNode *node);

/* The node after @node in document order within @root's subtree, or %NULL:
 * a walk of @root's descendants. */
AxisHtmlNode *
axis_html_following(
	const AxisHtmlNode *node,
	const AxisHtmlNode *root
);

/**
 * axis_html_text:
 * @node: any node
 * @max_bytes: the most kept
 *
 * The text of @node and its descendants in order, as a browser's
 * textContent, without the subtrees marked excluded; cut at @max_bytes (at
 * a character's boundary).
 */
gchar *
axis_html_text(
	const AxisHtmlNode *node,
	gsize               max_bytes
);

/**
 * axis_html_markup:
 * @node: any node (a document gives its top nodes)
 * @max_bytes: the most kept
 *
 * @node and its descendants written back as markup, for a person to read
 * (an excerpt of a page a definition is checked against): elements with
 * their attributes in double quotes, &, <, > (and " in an attribute) as
 * references, runs of blanks in text as one space, the end tags of void
 * elements left out; cut at @max_bytes (at a character's boundary).  It is
 * text to show, never markup to send anywhere.
 */
gchar *
axis_html_markup(
	const AxisHtmlNode *node,
	gsize               max_bytes
);

/* Moves @from's children to the end of @to's (a definition's rows.after). */
void
axis_html_move_children(
	AxisHtmlNode *from,
	AxisHtmlNode *to
);

/* @text with its HTML entities decoded (the filter htmldecode). */
gchar *
axis_html_decode_entities(const gchar *text);

G_END_DECLS

#endif /* AXIS_DEF_HTML_H */
