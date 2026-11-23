/*
 * axis-def-html.c - the page reader of indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One pass over the text: a tokenizer that hands start tags, end tags and
 * text to a tree builder with a stack of open elements.  The builder's
 * rules are a small part of HTML's: void elements, the ends HTML implies
 * (a <tr> ends the row before it, a <li> the item before it, a block ends
 * an open <p>), tbody and tr inserted where a browser inserts them, and an
 * end tag closing the nearest open element of its name (or ignored).
 */

#include "indexer/def/axis-def-html.h"

#include <string.h>

/* The longest name of an element or attribute kept, in bytes. */
#define NAME_MAX_BYTES 64

struct _AxisHtmlDoc
{
	AxisHtmlNode  root;
	GPtrArray    *nodes;	/* owned AxisHtmlNode *, their attrs owned too */
	GStringChunk *strings;
	gboolean      xml;
};

/* ── Entities ───────────────────────────────────────────────────── */

typedef struct
{
	const gchar *name;
	gunichar     value;
} Entity;

static const Entity entities[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 },
	{ "iexcl", 0xa1 }, { "cent", 0xa2 }, { "pound", 0xa3 }, { "curren", 0xa4 }, { "yen", 0xa5 },
	{ "brvbar", 0xa6 }, { "sect", 0xa7 }, { "uml", 0xa8 }, { "copy", 0xa9 }, { "ordf", 0xaa }, { "laquo", 0xab },
	{ "not", 0xac }, { "shy", 0xad }, { "reg", 0xae }, { "macr", 0xaf }, { "deg", 0xb0 }, { "plusmn", 0xb1 },
	{ "sup2", 0xb2 }, { "sup3", 0xb3 }, { "acute", 0xb4 }, { "micro", 0xb5 }, { "para", 0xb6 },
	{ "middot", 0xb7 }, { "cedil", 0xb8 }, { "sup1", 0xb9 }, { "ordm", 0xba }, { "raquo", 0xbb },
	{ "frac14", 0xbc }, { "frac12", 0xbd }, { "frac34", 0xbe }, { "iquest", 0xbf }, { "Agrave", 0xc0 },
	{ "Aacute", 0xc1 }, { "Acirc", 0xc2 }, { "Atilde", 0xc3 }, { "Auml", 0xc4 }, { "Aring", 0xc5 },
	{ "AElig", 0xc6 }, { "Ccedil", 0xc7 }, { "Egrave", 0xc8 }, { "Eacute", 0xc9 }, { "Ecirc", 0xca },
	{ "Euml", 0xcb }, { "Igrave", 0xcc }, { "Iacute", 0xcd }, { "Icirc", 0xce }, { "Iuml", 0xcf },
	{ "ETH", 0xd0 }, { "Ntilde", 0xd1 }, { "Ograve", 0xd2 }, { "Oacute", 0xd3 }, { "Ocirc", 0xd4 },
	{ "Otilde", 0xd5 }, { "Ouml", 0xd6 }, { "times", 0xd7 }, { "Oslash", 0xd8 }, { "Ugrave", 0xd9 },
	{ "Uacute", 0xda }, { "Ucirc", 0xdb }, { "Uuml", 0xdc }, { "Yacute", 0xdd }, { "THORN", 0xde },
	{ "szlig", 0xdf }, { "agrave", 0xe0 }, { "aacute", 0xe1 }, { "acirc", 0xe2 }, { "atilde", 0xe3 },
	{ "auml", 0xe4 }, { "aring", 0xe5 }, { "aelig", 0xe6 }, { "ccedil", 0xe7 }, { "egrave", 0xe8 },
	{ "eacute", 0xe9 }, { "ecirc", 0xea }, { "euml", 0xeb }, { "igrave", 0xec }, { "iacute", 0xed },
	{ "icirc", 0xee }, { "iuml", 0xef }, { "eth", 0xf0 }, { "ntilde", 0xf1 }, { "ograve", 0xf2 },
	{ "oacute", 0xf3 }, { "ocirc", 0xf4 }, { "otilde", 0xf5 }, { "ouml", 0xf6 }, { "divide", 0xf7 },
	{ "oslash", 0xf8 }, { "ugrave", 0xf9 }, { "uacute", 0xfa }, { "ucirc", 0xfb }, { "uuml", 0xfc },
	{ "yacute", 0xfd }, { "thorn", 0xfe }, { "yuml", 0xff }, { "OElig", 0x152 }, { "oelig", 0x153 },
	{ "Scaron", 0x160 }, { "scaron", 0x161 }, { "Yuml", 0x178 }, { "fnof", 0x192 }, { "circ", 0x2c6 },
	{ "tilde", 0x2dc }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200c },
	{ "zwj", 0x200d }, { "lrm", 0x200e }, { "rlm", 0x200f }, { "ndash", 0x2013 }, { "mdash", 0x2014 },
	{ "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201a }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
	{ "bdquo", 0x201e }, { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "bull", 0x2022 }, { "hellip", 0x2026 },
	{ "permil", 0x2030 }, { "prime", 0x2032 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203a }, { "euro", 0x20ac },
	{ "trade", 0x2122 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 },
	{ "hearts", 0x2665 }, { NULL, 0 }
};

/*
 * The entity at @s (just after its '&'), at most @n bytes: its character in
 * @out and the bytes it took (with the ';'), or 0 for none.
 */
static gsize
read_entity(
	const gchar *s,
	gsize        n,
	GString     *out
){
	gsize i = 0;
	gunichar value = 0;

	if (n >= 2 && s[0] == '#') {
		gboolean hex = s[1] == 'x' || s[1] == 'X';
		guint digits = 0;

		i = hex ? 2 : 1;
		while (i < n && (hex ? g_ascii_isxdigit(s[i]) : g_ascii_isdigit(s[i])) && digits < 8) {
			value = value * (hex ? 16 : 10) + (gunichar)(hex ? g_ascii_xdigit_value(s[i]) : s[i] - '0');
			i++;
			digits++;
		}
		if (digits == 0)
			return 0;
		if (i < n && s[i] == ';')
			i++;
		if (value == 0 || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
			value = 0xfffd;
		g_string_append_unichar(out, value);
		return i;
	}
	while (i < n && i < 10 && g_ascii_isalnum(s[i]))
		i++;
	if (i == 0 || i >= n || s[i] != ';')
		return 0;
	for (value = 0; entities[value].name != NULL; value++) {
		if (strlen(entities[value].name) == i && strncmp(entities[value].name, s, i) == 0) {
			g_string_append_unichar(out, entities[value].value);
			return i + 1;
		}
	}
	return 0;
}

/* @n bytes of @s, entities decoded, appended to @out. */
static void
append_decoded(
	GString     *out,
	const gchar *s,
	gsize        n
){
	gsize i = 0;

	while (i < n) {
		const gchar *amp = memchr(s + i, '&', n - i);
		gsize took;

		if (amp == NULL) {
			g_string_append_len(out, s + i, (gssize)(n - i));
			return;
		}
		g_string_append_len(out, s + i, amp - (s + i));
		i = (gsize)(amp - s) + 1;
		took = read_entity(s + i, n - i, out);
		if (took == 0)
			g_string_append_c(out, '&');
		i += took;
	}
}

gchar *
axis_html_decode_entities(const gchar *text)
{
	GString *out = g_string_new(NULL);

	if (text != NULL)
		append_decoded(out, text, strlen(text));
	return g_string_free(out, FALSE);
}

/* ── The tree ───────────────────────────────────────────────────── */

typedef struct
{
	AxisHtmlDoc  *doc;
	GPtrArray    *stack;	/* open elements, root first */
	GString      *pending;	/* text not yet in a node */
	gchar        *problem;
} Builder;

static AxisHtmlNode *
current(Builder *b)
{
	return b->stack->len > 0 ? g_ptr_array_index(b->stack, b->stack->len - 1) : &b->doc->root;
}

static void
append_child(
	AxisHtmlNode *parent,
	AxisHtmlNode *child
){
	child->parent = parent;
	child->prev = parent->last;
	child->next = NULL;
	if (parent->last != NULL)
		parent->last->next = child;
	else
		parent->first = child;
	parent->last = child;
}

static AxisHtmlNode *
new_node(
	Builder          *b,
	AxisHtmlNodeType  type
){
	AxisHtmlNode *node;

	if (b->doc->nodes->len >= AXIS_HTML_MAX_NODES) {
		if (b->problem == NULL)
			b->problem = g_strdup("the page has more elements than the limit");
		return NULL;
	}
	node = g_new0(AxisHtmlNode, 1);
	node->type = type;
	g_ptr_array_add(b->doc->nodes, node);
	return node;
}

/* The text read since the last node, as a text node (merged into a text
 * node just before it). */
static void
flush_text(Builder *b)
{
	AxisHtmlNode *parent;
	AxisHtmlNode *node;

	if (b->pending->len == 0)
		return;
	parent = current(b);
	if (parent->last != NULL && parent->last->type == AXIS_HTML_TEXT) {
		g_autofree gchar *joined = g_strconcat(parent->last->text, b->pending->str, NULL);

		parent->last->text = g_string_chunk_insert(b->doc->strings, joined);
		g_string_truncate(b->pending, 0);
		return;
	}
	node = new_node(b, AXIS_HTML_TEXT);
	if (node != NULL) {
		node->text = g_string_chunk_insert_len(b->doc->strings, b->pending->str, (gssize)b->pending->len);
		append_child(parent, node);
	}
	g_string_truncate(b->pending, 0);
}

static gboolean
name_in(
	const gchar        *name,
	const gchar *const *names
){
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		if (strcmp(names[i], name) == 0)
			return TRUE;
	}
	return FALSE;
}

static const gchar *const void_elements[] = {
	"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track",
	"wbr", "keygen", "command", "basefont", "bgsound", "frame", NULL
};

static const gchar *const closes_p[] = {
	"address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset",
	"figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hgroup", "hr",
	"main", "menu", "nav", "ol", "p", "pre", "section", "summary", "table", "ul", "li", "dd", "dt", NULL
};

static const gchar *const table_scope[] = { "table", "html", "template", NULL };

/* The index in the stack of the nearest open @name, not looking past an
 * element of @boundary; -1 for none. */
static gint
open_index(
	Builder            *b,
	const gchar *const *names,
	const gchar *const *boundary
){
	gint i;

	for (i = (gint)b->stack->len - 1; i >= 0; i--) {
		AxisHtmlNode *node = g_ptr_array_index(b->stack, i);

		if (name_in(node->name, names))
			return i;
		if (boundary != NULL && name_in(node->name, boundary))
			return -1;
	}
	return -1;
}

static void
pop_to(
	Builder *b,
	gint     index
){
	if (index >= 0 && (guint)index < b->stack->len)
		g_ptr_array_set_size(b->stack, (guint)index);
}

static void push(Builder *b, AxisHtmlNode *node);

/* An element with no attributes, inserted where a browser inserts one. */
static void
insert_implied(
	Builder     *b,
	const gchar *name
){
	AxisHtmlNode *node = new_node(b, AXIS_HTML_ELEMENT);

	if (node == NULL)
		return;
	node->name = g_string_chunk_insert_const(b->doc->strings, name);
	append_child(current(b), node);
	push(b, node);
}

static void
push(
	Builder      *b,
	AxisHtmlNode *node
){
	if (b->stack->len < AXIS_HTML_MAX_DEPTH)
		g_ptr_array_add(b->stack, node);
}

/* What HTML implies before an element called @name opens. */
static void
implied_before(
	Builder     *b,
	const gchar *name
){
	static const gchar *const li[] = { "li", NULL };
	static const gchar *const list_scope[] = { "ul", "ol", "table", "td", "th", NULL };
	static const gchar *const dd_dt[] = { "dd", "dt", NULL };
	static const gchar *const dl_scope[] = { "dl", "table", "td", "th", NULL };
	static const gchar *const cells[] = { "td", "th", NULL };
	static const gchar *const row_scope[] = { "tr", "table", "html", NULL };
	static const gchar *const tr[] = { "tr", NULL };
	static const gchar *const section_scope[] = { "thead", "tbody", "tfoot", "table", "html", NULL };
	static const gchar *const sections[] = { "thead", "tbody", "tfoot", NULL };
	static const gchar *const option[] = { "option", NULL };
	static const gchar *const option_scope[] = { "select", "datalist", "optgroup", NULL };
	static const gchar *const optgroup[] = { "optgroup", NULL };
	static const gchar *const p[] = { "p", NULL };
	static const gchar *const p_scope[] = { "button", "table", "td", "th", "html", "object", "marquee", NULL };
	static const gchar *const a[] = { "a", NULL };
	static const gchar *const headings[] = { "h1", "h2", "h3", "h4", "h5", "h6", NULL };
	AxisHtmlNode *now;

	if (name_in(name, closes_p))
		pop_to(b, open_index(b, p, p_scope));
	if (strcmp(name, "li") == 0) {
		pop_to(b, open_index(b, li, list_scope));
	} else if (strcmp(name, "dd") == 0 || strcmp(name, "dt") == 0) {
		pop_to(b, open_index(b, dd_dt, dl_scope));
	} else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
		pop_to(b, open_index(b, cells, row_scope));
		now = current(b);
		if (now->name != NULL && (strcmp(now->name, "table") == 0 || name_in(now->name, sections))) {
			if (strcmp(now->name, "table") == 0)
				insert_implied(b, "tbody");
			insert_implied(b, "tr");
		}
	} else if (strcmp(name, "tr") == 0) {
		pop_to(b, open_index(b, cells, row_scope));
		pop_to(b, open_index(b, tr, section_scope));
		now = current(b);
		if (now->name != NULL && strcmp(now->name, "table") == 0)
			insert_implied(b, "tbody");
	} else if (name_in(name, sections)) {
		pop_to(b, open_index(b, cells, row_scope));
		pop_to(b, open_index(b, tr, section_scope));
		pop_to(b, open_index(b, sections, table_scope));
	} else if (strcmp(name, "option") == 0) {
		pop_to(b, open_index(b, option, option_scope));
	} else if (strcmp(name, "optgroup") == 0) {
		pop_to(b, open_index(b, option, option_scope));
		pop_to(b, open_index(b, optgroup, option_scope));
	} else if (strcmp(name, "a") == 0) {
		pop_to(b, open_index(b, a, table_scope));
	} else if (name_in(name, headings)) {
		now = current(b);
		if (now->name != NULL && name_in(now->name, headings))
			pop_to(b, (gint)b->stack->len - 1);
	}
}

static void
start_element(
	Builder      *b,
	AxisHtmlNode *node,
	gboolean      self_closing
){
	gboolean xml = b->doc->xml;

	flush_text(b);
	if (!xml)
		implied_before(b, node->name);
	append_child(current(b), node);
	if (xml ? !self_closing : !name_in(node->name, void_elements))
		push(b, node);
}

static void
end_element(
	Builder     *b,
	const gchar *name
){
	const gchar *names[2] = { name, NULL };
	gint index;

	flush_text(b);
	index = open_index(b, names, NULL);
	if (index >= 0)
		pop_to(b, index);
}

/* ── The tokenizer ──────────────────────────────────────────────── */

typedef struct
{
	const gchar *s;
	gsize        len;
	gsize        pos;
} Cursor;

static gboolean
is_space(gchar c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* A name at the cursor: up to a space, '/', '>' or '='. */
static gchar *
read_name(
	Cursor   *c,
	gboolean  lower
){
	gsize start = c->pos;
	gsize n;

	while (c->pos < c->len && !is_space(c->s[c->pos]) && c->s[c->pos] != '/' && c->s[c->pos] != '>' &&
	       (c->pos == start || c->s[c->pos] != '='))
		c->pos++;
	n = MIN(c->pos - start, (gsize)NAME_MAX_BYTES);
	/* Cut at a character's boundary. */
	while (n > 0 && n < c->pos - start && ((guchar)c->s[start + n] & 0xc0) == 0x80)
		n--;
	if (n == 0)
		return NULL;
	return lower ? g_ascii_strdown(c->s + start, (gssize)n) : g_strndup(c->s + start, n);
}

static void
skip_space(Cursor *c)
{
	while (c->pos < c->len && is_space(c->s[c->pos]))
		c->pos++;
}

/* A start tag after its '<'; the element, or %NULL when it is not one.
 * @self_closing: it ended in "/>". */
static AxisHtmlNode *
read_start_tag(
	Builder  *b,
	Cursor   *c,
	gboolean *self_closing
){
	gboolean xml = b->doc->xml;
	g_autofree gchar *name = read_name(c, !xml);
	GPtrArray *attrs = g_ptr_array_new_with_free_func(g_free);
	AxisHtmlNode *node;
	guint i;

	*self_closing = FALSE;
	while (c->pos < c->len) {
		g_autofree gchar *attr = NULL;
		GString *value;

		skip_space(c);
		if (c->pos >= c->len)
			break;
		if (c->s[c->pos] == '>') {
			c->pos++;
			break;
		}
		if (c->s[c->pos] == '/') {
			c->pos++;
			if (c->pos < c->len && c->s[c->pos] == '>') {
				*self_closing = TRUE;
				c->pos++;
				break;
			}
			continue;
		}
		attr = read_name(c, !xml);
		if (attr == NULL) {
			c->pos++;
			continue;
		}
		skip_space(c);
		value = g_string_new(NULL);
		if (c->pos < c->len && c->s[c->pos] == '=') {
			c->pos++;
			skip_space(c);
			if (c->pos < c->len && (c->s[c->pos] == '"' || c->s[c->pos] == '\'')) {
				gchar quote = c->s[c->pos++];
				const gchar *end = memchr(c->s + c->pos, quote, c->len - c->pos);
				gsize n = end != NULL ? (gsize)(end - (c->s + c->pos)) : c->len - c->pos;

				append_decoded(value, c->s + c->pos, n);
				c->pos += n + (end != NULL ? 1 : 0);
			} else {
				gsize start = c->pos;

				while (c->pos < c->len && !is_space(c->s[c->pos]) && c->s[c->pos] != '>')
					c->pos++;
				append_decoded(value, c->s + start, c->pos - start);
			}
		}
		/* The first of a name, and no more than the limit. */
		for (i = 0; i < attrs->len; i += 2) {
			if (strcmp(g_ptr_array_index(attrs, i), attr) == 0)
				break;
		}
		if (i >= attrs->len && attrs->len / 2 < AXIS_HTML_MAX_ATTRS) {
			g_ptr_array_add(attrs, g_steal_pointer(&attr));
			g_ptr_array_add(attrs, g_string_free(value, FALSE));
		} else {
			g_string_free(value, TRUE);
		}
	}
	node = name != NULL ? new_node(b, AXIS_HTML_ELEMENT) : NULL;
	if (node != NULL) {
		node->name = g_string_chunk_insert_const(b->doc->strings, name);
		node->n_attrs = attrs->len / 2;
		node->attrs = g_new0(gchar *, attrs->len + 1);
		for (i = 0; i < attrs->len; i++)
			node->attrs[i] = g_string_chunk_insert(b->doc->strings, g_ptr_array_index(attrs, i));
	}
	g_ptr_array_unref(attrs);
	return node;
}

/* Where "</@name" starts at or after the cursor (any case), or the end. */
static gsize
find_end_tag(
	const Cursor *c,
	const gchar  *name
){
	gsize n = strlen(name);
	gsize i;

	for (i = c->pos; i + 2 + n <= c->len; i++) {
		if (c->s[i] == '<' && c->s[i + 1] == '/' && g_ascii_strncasecmp(c->s + i + 2, name, n) == 0 &&
		    (i + 2 + n == c->len || is_space(c->s[i + 2 + n]) || c->s[i + 2 + n] == '>' || c->s[i + 2 + n] == '/'))
			return i;
	}
	return c->len;
}

/* Text up to the cursor's next '<', or a raw element's content. */
static void
add_text(
	Builder     *b,
	const gchar *s,
	gsize        n,
	gboolean     decode
){
	if (decode)
		append_decoded(b->pending, s, n);
	else
		g_string_append_len(b->pending, s, (gssize)n);
}

static void
tokenize(
	Builder *b,
	Cursor  *c
){
	static const gchar *const raw_text[] = { "script", "style", "xmp", "iframe", "noembed", "noframes", NULL };
	static const gchar *const rcdata[] = { "title", "textarea", NULL };
	gboolean xml = b->doc->xml;

	while (c->pos < c->len && b->problem == NULL) {
		const gchar *lt = memchr(c->s + c->pos, '<', c->len - c->pos);
		gsize at;

		if (lt == NULL) {
			add_text(b, c->s + c->pos, c->len - c->pos, TRUE);
			c->pos = c->len;
			break;
		}
		at = (gsize)(lt - c->s);
		add_text(b, c->s + c->pos, at - c->pos, TRUE);
		c->pos = at + 1;
		if (c->pos >= c->len) {
			g_string_append_c(b->pending, '<');
			break;
		}
		if (c->len - c->pos >= 3 && strncmp(c->s + c->pos, "!--", 3) == 0) {
			const gchar *end = g_strstr_len(c->s + c->pos + 3, (gssize)(c->len - c->pos - 3), "-->");

			c->pos = end != NULL ? (gsize)(end - c->s) + 3 : c->len;
			continue;
		}
		if (c->len - c->pos >= 8 && strncmp(c->s + c->pos, "![CDATA[", 8) == 0) {
			const gchar *start = c->s + c->pos + 8;
			const gchar *end = g_strstr_len(start, (gssize)(c->len - c->pos - 8), "]]>");
			gsize n = end != NULL ? (gsize)(end - start) : c->len - c->pos - 8;

			add_text(b, start, n, FALSE);
			c->pos = c->pos + 8 + n + (end != NULL ? 3 : 0);
			continue;
		}
		if (c->s[c->pos] == '!' || c->s[c->pos] == '?') {
			const gchar *end = memchr(c->s + c->pos, '>', c->len - c->pos);

			c->pos = end != NULL ? (gsize)(end - c->s) + 1 : c->len;
			continue;
		}
		if (c->s[c->pos] == '/') {
			g_autofree gchar *name = NULL;
			const gchar *end;

			c->pos++;
			name = read_name(c, !xml);
			end = memchr(c->s + c->pos, '>', c->len - c->pos);
			c->pos = end != NULL ? (gsize)(end - c->s) + 1 : c->len;
			if (name != NULL)
				end_element(b, name);
			continue;
		}
		if (g_ascii_isalpha(c->s[c->pos]) || (xml && (c->s[c->pos] == '_' || (guchar)c->s[c->pos] >= 0x80))) {
			gboolean self_closing = FALSE;
			AxisHtmlNode *node;

			flush_text(b);
			node = read_start_tag(b, c, &self_closing);
			if (node == NULL)
				break;
			start_element(b, node, self_closing);
			if (!xml && !self_closing && (name_in(node->name, raw_text) || name_in(node->name, rcdata))) {
				gsize end = find_end_tag(c, node->name);

				add_text(b, c->s + c->pos, end - c->pos, name_in(node->name, rcdata));
				c->pos = end;
			}
			continue;
		}
		/* A '<' that starts no tag is text. */
		g_string_append_c(b->pending, '<');
	}
	flush_text(b);
}

AxisHtmlDoc *
axis_html_parse(
	const gchar  *data,
	gsize         length,
	gboolean      xml,
	gchar       **problem
){
	AxisHtmlDoc *doc;
	Builder b;
	Cursor c;
	GString *clean;
	gchar *valid;
	gsize i;

	if (problem != NULL)
		*problem = NULL;
	if (data == NULL)
		length = 0;
	if (length > AXIS_HTML_MAX_BYTES) {
		if (problem != NULL)
			*problem = g_strdup("the page is larger than the limit");
		return NULL;
	}
	/* Valid UTF-8 without NUL: each NUL, and each byte that is not UTF-8,
	 * as U+FFFD. */
	clean = g_string_sized_new(length + 1);
	for (i = 0; i < length; i++) {
		if (data[i] == '\0')
			g_string_append(clean, "\xef\xbf\xbd");
		else
			g_string_append_c(clean, data[i]);
	}
	valid = g_utf8_make_valid(clean->str, (gssize)clean->len);
	g_string_free(clean, TRUE);
	doc = g_new0(AxisHtmlDoc, 1);
	doc->root.type = AXIS_HTML_DOCUMENT;
	doc->nodes = g_ptr_array_new();
	doc->strings = g_string_chunk_new(4096);
	doc->xml = xml;
	memset(&b, 0, sizeof(b));
	b.doc = doc;
	b.stack = g_ptr_array_new();
	b.pending = g_string_new(NULL);
	c.s = valid;
	c.len = strlen(valid);
	c.pos = 0;
	tokenize(&b, &c);
	g_ptr_array_unref(b.stack);
	g_string_free(b.pending, TRUE);
	g_free(valid);
	if (b.problem != NULL) {
		if (problem != NULL)
			*problem = g_steal_pointer(&b.problem);
		g_free(b.problem);
		axis_html_doc_free(doc);
		return NULL;
	}
	return doc;
}

void
axis_html_doc_free(AxisHtmlDoc *doc)
{
	guint i;

	if (doc == NULL)
		return;
	for (i = 0; i < doc->nodes->len; i++) {
		AxisHtmlNode *node = g_ptr_array_index(doc->nodes, i);

		g_free(node->attrs);
		g_free(node);
	}
	g_ptr_array_unref(doc->nodes);
	g_string_chunk_free(doc->strings);
	g_free(doc);
}

AxisHtmlNode *
axis_html_doc_root(AxisHtmlDoc *doc)
{
	return &doc->root;
}

guint
axis_html_doc_n_nodes(AxisHtmlDoc *doc)
{
	return doc->nodes->len;
}

/* ── Reading the tree ───────────────────────────────────────────── */

const gchar *
axis_html_attr(
	const AxisHtmlNode *node,
	const gchar        *name
){
	guint i;

	if (node == NULL || node->type != AXIS_HTML_ELEMENT || name == NULL)
		return NULL;
	for (i = 0; i < node->n_attrs; i++) {
		if (g_ascii_strcasecmp(node->attrs[2 * i], name) == 0)
			return node->attrs[2 * i + 1];
	}
	return NULL;
}

AxisHtmlNode *
axis_html_next_element(const AxisHtmlNode *node)
{
	AxisHtmlNode *n;

	for (n = node->next; n != NULL; n = n->next) {
		if (n->type == AXIS_HTML_ELEMENT)
			return n;
	}
	return NULL;
}

AxisHtmlNode *
axis_html_prev_element(const AxisHtmlNode *node)
{
	AxisHtmlNode *n;

	for (n = node->prev; n != NULL; n = n->prev) {
		if (n->type == AXIS_HTML_ELEMENT)
			return n;
	}
	return NULL;
}

AxisHtmlNode *
axis_html_first_element(const AxisHtmlNode *node)
{
	AxisHtmlNode *n;

	for (n = node->first; n != NULL; n = n->next) {
		if (n->type == AXIS_HTML_ELEMENT)
			return n;
	}
	return NULL;
}

AxisHtmlNode *
axis_html_following(
	const AxisHtmlNode *node,
	const AxisHtmlNode *root
){
	if (node->first != NULL)
		return node->first;
	while (node != NULL && node != root) {
		if (node->next != NULL)
			return node->next;
		node = node->parent;
	}
	return NULL;
}

gchar *
axis_html_text(
	const AxisHtmlNode *node,
	gsize               max_bytes
){
	GString *out = g_string_new(NULL);
	const AxisHtmlNode *n;

	if (node == NULL)
		return g_string_free(out, FALSE);
	if (node->type == AXIS_HTML_TEXT) {
		g_string_append(out, node->text);
	} else {
		n = node->first;
		while (n != NULL && out->len < max_bytes) {
			if (n->excluded) {
				/* The node after this subtree. */
				while (n != node && n->next == NULL)
					n = n->parent;
				n = n != node ? n->next : NULL;
				continue;
			}
			if (n->type == AXIS_HTML_TEXT)
				g_string_append(out, n->text);
			n = axis_html_following(n, node);
		}
	}
	if (out->len > max_bytes) {
		const gchar *end = out->str + max_bytes;

		while (end > out->str && ((guchar)*end & 0xc0) == 0x80)
			end--;
		g_string_truncate(out, (gsize)(end - out->str));
	}
	return g_string_free(out, FALSE);
}

/* @text with &, <, > (and in an attribute ") written as references; in a
 * text node, runs of blanks as one space. */
static void
append_escaped(
	GString     *out,
	const gchar *text,
	gboolean     attribute,
	gsize        limit
){
	const gchar *p;
	gboolean blank = FALSE;

	/* No more than @limit in all: a text node of megabytes is not copied
	 * whole to be cut. */
	for (p = text; *p != '\0' && out->len < limit; p++) {
		gboolean is_blank = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f';

		if (!attribute && is_blank) {
			if (!blank)
				g_string_append_c(out, ' ');
			blank = TRUE;
			continue;
		}
		blank = FALSE;
		if (*p == '&')
			g_string_append(out, "&amp;");
		else if (*p == '<')
			g_string_append(out, "&lt;");
		else if (*p == '>')
			g_string_append(out, "&gt;");
		else if (*p == '"' && attribute)
			g_string_append(out, "&quot;");
		else
			g_string_append_c(out, *p);
	}
}

/* @text, as far as @limit in all. */
static void
append_bounded(
	GString     *out,
	const gchar *text,
	gsize        limit
){
	gsize room = out->len < limit ? limit - out->len : 0;

	g_string_append_len(out, text, (gssize)strnlen(text, room));
}

static void
append_open(
	GString            *out,
	const AxisHtmlNode *node,
	gsize               limit
){
	guint i;

	g_string_append_c(out, '<');
	append_bounded(out, node->name, limit);
	for (i = 0; i < node->n_attrs && out->len < limit; i++) {
		g_string_append_c(out, ' ');
		append_bounded(out, node->attrs[2 * i], limit);
		g_string_append(out, "=\"");
		append_escaped(out, node->attrs[2 * i + 1], TRUE, limit);
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '>');
}

static void
append_close(
	GString            *out,
	const AxisHtmlNode *node,
	gsize               limit
){
	if (out->len >= limit || (node->first == NULL && name_in(node->name, void_elements)))
		return;
	g_string_append(out, "</");
	append_bounded(out, node->name, limit);
	g_string_append_c(out, '>');
}

gchar *
axis_html_markup(
	const AxisHtmlNode *node,
	gsize               max_bytes
){
	GString *out = g_string_new(NULL);
	const AxisHtmlNode *n;
	/* What is copied stops a little past the cut: never a whole text node
	 * or attribute of megabytes. */
	gsize limit = max_bytes + 8;

	if (node == NULL)
		return g_string_free(out, FALSE);
	if (node->type == AXIS_HTML_TEXT) {
		append_escaped(out, node->text, FALSE, limit);
	} else {
		/* A walk in document order, without recursion: an element is opened
		 * on the way down and closed on the way back up. */
		n = node->type == AXIS_HTML_DOCUMENT ? node->first : node;
		while (n != NULL && out->len < max_bytes) {
			if (n->type == AXIS_HTML_TEXT) {
				append_escaped(out, n->text, FALSE, limit);
			} else if (n->type == AXIS_HTML_ELEMENT) {
				append_open(out, n, limit);
				if (n->first != NULL) {
					n = n->first;
					continue;
				}
				append_close(out, n, limit);
			}
			while (n != node && n->next == NULL) {
				n = n->parent;
				if (n == NULL)
					break;
				if (n->type == AXIS_HTML_ELEMENT)
					append_close(out, n, limit);
			}
			if (n == NULL || n == node)
				break;
			n = n->next;
		}
	}
	if (out->len > max_bytes) {
		const gchar *end = out->str + max_bytes;

		while (end > out->str && ((guchar)*end & 0xc0) == 0x80)
			end--;
		g_string_truncate(out, (gsize)(end - out->str));
	}
	return g_string_free(out, FALSE);
}

void
axis_html_move_children(
	AxisHtmlNode *from,
	AxisHtmlNode *to
){
	AxisHtmlNode *n;

	if (from == to)
		return;
	for (n = to; n != NULL; n = n->parent) {
		if (n == from)
			return;
	}
	while (from->first != NULL) {
		AxisHtmlNode *child = from->first;

		from->first = child->next;
		if (from->first != NULL)
			from->first->prev = NULL;
		else
			from->last = NULL;
		append_child(to, child);
	}
}
