/*
 * axis-def-yaml.c - the YAML subset reader of indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A recursive reader over the whole text with a cursor: block nodes are
 * told apart by their column, flow nodes by their brackets.  Every loop
 * moves the cursor forward or ends, the recursion is bounded by
 * AXIS_YAML_MAX_DEPTH, and every node counts against AXIS_YAML_MAX_NODES.
 */

#include "indexer/def/axis-def-yaml.h"

#include <string.h>

/* The longest key of a map, in bytes. */
#define KEY_MAX_BYTES 1024

typedef struct
{
	const gchar *s;		/* the text, NUL-terminated */
	gsize        len;
	gsize        pos;
	guint        line;	/* of pos, from 1 */
	gsize        line_start;
	guint        nodes;
	guint        depth;
	gchar       *problem;
} Parser;

/* ── Nodes ──────────────────────────────────────────────────────── */

void
axis_yaml_node_free(AxisYamlNode *node)
{
	if (node == NULL)
		return;
	g_free(node->text);
	g_clear_pointer(&node->keys, g_ptr_array_unref);
	g_clear_pointer(&node->values, g_ptr_array_unref);
	g_free(node);
}

static AxisYamlNode *
node_new(
	Parser       *p,
	AxisYamlKind  kind
){
	AxisYamlNode *node;

	if (p->nodes >= AXIS_YAML_MAX_NODES)
		return NULL;
	p->nodes++;
	node = g_new0(AxisYamlNode, 1);
	node->kind = kind;
	node->line = p->line;
	if (kind == AXIS_YAML_MAP)
		node->keys = g_ptr_array_new_with_free_func(g_free);
	if (kind != AXIS_YAML_SCALAR)
		node->values = g_ptr_array_new_with_free_func((GDestroyNotify)axis_yaml_node_free);
	return node;
}

guint
axis_yaml_length(const AxisYamlNode *node)
{
	return node != NULL && node->values != NULL ? node->values->len : 0;
}

const AxisYamlNode *
axis_yaml_get(
	const AxisYamlNode *map,
	const gchar        *key
){
	guint i;

	if (map == NULL || map->kind != AXIS_YAML_MAP || key == NULL)
		return NULL;
	for (i = 0; i < map->keys->len; i++) {
		if (strcmp(g_ptr_array_index(map->keys, i), key) == 0)
			return g_ptr_array_index(map->values, i);
	}
	return NULL;
}

const gchar *
axis_yaml_key_at(
	const AxisYamlNode *map,
	guint               index
){
	if (map == NULL || map->kind != AXIS_YAML_MAP || index >= map->keys->len)
		return NULL;
	return g_ptr_array_index(map->keys, index);
}

const AxisYamlNode *
axis_yaml_at(
	const AxisYamlNode *node,
	guint               index
){
	if (node == NULL || node->values == NULL || index >= node->values->len)
		return NULL;
	return g_ptr_array_index(node->values, index);
}

gboolean
axis_yaml_is_null(const AxisYamlNode *node)
{
	if (node == NULL || node->kind != AXIS_YAML_SCALAR || node->quoted)
		return node == NULL;
	return node->text[0] == '\0' || strcmp(node->text, "~") == 0 || strcmp(node->text, "null") == 0 ||
		strcmp(node->text, "Null") == 0 || strcmp(node->text, "NULL") == 0;
}

const gchar *
axis_yaml_text(const AxisYamlNode *node)
{
	if (node == NULL || node->kind != AXIS_YAML_SCALAR || axis_yaml_is_null(node))
		return NULL;
	return node->text;
}

gboolean
axis_yaml_get_bool(
	const AxisYamlNode *node,
	gboolean           *value
){
	const gchar *t;

	if (node == NULL || node->kind != AXIS_YAML_SCALAR || node->quoted)
		return FALSE;
	t = node->text;
	if (strcmp(t, "true") == 0 || strcmp(t, "True") == 0 || strcmp(t, "TRUE") == 0) {
		*value = TRUE;
		return TRUE;
	}
	if (strcmp(t, "false") == 0 || strcmp(t, "False") == 0 || strcmp(t, "FALSE") == 0) {
		*value = FALSE;
		return TRUE;
	}
	return FALSE;
}

/* ── The cursor ─────────────────────────────────────────────────── */

static gboolean
fail(
	Parser      *p,
	const gchar *what
){
	if (p->problem == NULL)
		p->problem = g_strdup_printf("line %u: %s", p->line, what);
	return FALSE;
}

static gchar
at(
	const Parser *p,
	gsize         offset
){
	return p->pos + offset < p->len ? p->s[p->pos + offset] : '\0';
}

static gsize
column(const Parser *p)
{
	return p->pos - p->line_start;
}

static void
advance(Parser *p)
{
	if (p->pos >= p->len)
		return;
	if (p->s[p->pos] == '\n') {
		p->line++;
		p->line_start = p->pos + 1;
	}
	p->pos++;
}

static gboolean
is_break(gchar c)
{
	return c == '\n' || c == '\r' || c == '\0';
}

static gboolean
is_blank(gchar c)
{
	return c == ' ' || c == '\t';
}

/* Spaces and tabs on this line. */
static void
skip_blanks(Parser *p)
{
	while (is_blank(at(p, 0)))
		advance(p);
}

/* To the end of the line (not past its break). */
static void
skip_comment(Parser *p)
{
	while (!is_break(at(p, 0)))
		advance(p);
}

/*
 * To the next character that is content: past blank lines, comments and
 * the rest of this line's blanks.  %FALSE at the end of the text (or after
 * a problem).  A tab in the indentation of a line with content is refused.
 */
static gboolean
skip_to_content(Parser *p)
{
	while (p->problem == NULL) {
		gsize lead;

		skip_blanks(p);
		if (at(p, 0) == '#') {
			skip_comment(p);
			continue;
		}
		if (at(p, 0) == '\r') {
			advance(p);
			continue;
		}
		if (at(p, 0) == '\n') {
			advance(p);
			continue;
		}
		if (p->pos >= p->len)
			return FALSE;
		/* Content: a tab among the blanks before it, when nothing else is
		 * before it on its line, is indentation, which YAML does not
		 * allow. */
		lead = strspn(p->s + p->line_start, " \t");
		if (lead == column(p) && memchr(p->s + p->line_start, '\t', lead) != NULL)
			return fail(p, "a tab in the indentation");
		return TRUE;
	}
	return FALSE;
}

/* A "---" or "..." at the start of a line, alone or before a blank. */
static gboolean
at_document_marker(const Parser *p)
{
	if (column(p) != 0)
		return FALSE;
	if (!((at(p, 0) == '-' && at(p, 1) == '-' && at(p, 2) == '-') ||
	      (at(p, 0) == '.' && at(p, 1) == '.' && at(p, 2) == '.')))
		return FALSE;
	return is_blank(at(p, 3)) || is_break(at(p, 3));
}

static gboolean
at_seq_entry(const Parser *p)
{
	return at(p, 0) == '-' && (is_blank(at(p, 1)) || is_break(at(p, 1)));
}

/* What a character at the start of a value means that is not supported, or
 * %NULL for one that is. */
static const gchar *
refused_start(gchar c)
{
	switch (c) {
	case '&':
		return "anchors are not supported";
	case '*':
		return "aliases are not supported";
	case '!':
		return "tags are not supported";
	case '|':
	case '>':
		return "block scalars (| and >) are not supported";
	case '%':
		return "directives are not supported";
	case '@':
	case '`':
		return "a value cannot start with @ or `";
	default:
		return NULL;
	}
}

/* ── Scalars ────────────────────────────────────────────────────── */

static gboolean
append_checked(
	Parser  *p,
	GString *text,
	gchar    c
){
	if (text->len >= AXIS_YAML_MAX_SCALAR)
		return fail(p, "a value longer than the limit");
	g_string_append_c(text, c);
	return TRUE;
}

static gboolean
append_unichar(
	Parser   *p,
	GString  *text,
	gunichar  c
){
	if (c == 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return fail(p, "an escape that is not a character Axis keeps");
	if (text->len + 6 > AXIS_YAML_MAX_SCALAR)
		return fail(p, "a value longer than the limit");
	g_string_append_unichar(text, c);
	return TRUE;
}

/* @digits hexadecimal digits at the cursor, consumed. */
static gboolean
read_hex(
	Parser   *p,
	guint     digits,
	gunichar *value
){
	guint i;

	*value = 0;
	for (i = 0; i < digits; i++) {
		gchar c = at(p, 0);

		if (!g_ascii_isxdigit(c))
			return fail(p, "an escape with a digit that is not hexadecimal");
		*value = *value * 16 + (gunichar)g_ascii_xdigit_value(c);
		advance(p);
	}
	return TRUE;
}

/*
 * A line break inside a quoted scalar, at the cursor (a '\r' or '\n'):
 * the blanks before it are dropped (by the caller), the break and the
 * indentation after it are consumed; one break is a space, each empty line
 * after it a '\n'.
 */
static gboolean
fold_break(
	Parser  *p,
	GString *text,
	gboolean join
){
	guint empty = 0;

	if (at(p, 0) == '\r')
		advance(p);
	if (at(p, 0) == '\n')
		advance(p);
	while (TRUE) {
		skip_blanks(p);
		if (at(p, 0) == '\r')
			advance(p);
		if (at(p, 0) == '\n') {
			advance(p);
			empty++;
			continue;
		}
		break;
	}
	if (at_document_marker(p))
		return fail(p, "a document marker inside a quoted value");
	if (empty > 0) {
		while (empty-- > 0) {
			if (!append_checked(p, text, '\n'))
				return FALSE;
		}
	} else if (!join && !append_checked(p, text, ' ')) {
		return FALSE;
	}
	return TRUE;
}

/* Drops the blanks at the end of @text (before a folded break). */
static void
trim_blanks(GString *text)
{
	while (text->len > 0 && is_blank(text->str[text->len - 1]))
		g_string_truncate(text, text->len - 1);
}

static gboolean
read_double_quoted(
	Parser  *p,
	GString *text
){
	advance(p);
	while (TRUE) {
		gchar c = at(p, 0);
		gunichar u;

		if (p->pos >= p->len)
			return fail(p, "a quoted value that does not end");
		if (c == '"') {
			advance(p);
			return TRUE;
		}
		if (c == '\r' || c == '\n') {
			trim_blanks(text);
			if (!fold_break(p, text, FALSE))
				return FALSE;
			continue;
		}
		if (c != '\\') {
			if (!append_checked(p, text, c))
				return FALSE;
			advance(p);
			continue;
		}
		advance(p);
		c = at(p, 0);
		if (c == '\r' || c == '\n') {
			/* An escaped break joins the lines. */
			if (!fold_break(p, text, TRUE))
				return FALSE;
			continue;
		}
		advance(p);
		switch (c) {
		case 'a': u = 0x07; break;
		case 'b': u = 0x08; break;
		case 't': case '\t': u = 0x09; break;
		case 'n': u = 0x0a; break;
		case 'v': u = 0x0b; break;
		case 'f': u = 0x0c; break;
		case 'r': u = 0x0d; break;
		case 'e': u = 0x1b; break;
		case ' ': u = 0x20; break;
		case '"': u = '"'; break;
		case '/': u = '/'; break;
		case '\\': u = '\\'; break;
		case 'N': u = 0x85; break;
		case '_': u = 0xa0; break;
		case 'L': u = 0x2028; break;
		case 'P': u = 0x2029; break;
		case 'x':
			if (!read_hex(p, 2, &u))
				return FALSE;
			break;
		case 'u':
			if (!read_hex(p, 4, &u))
				return FALSE;
			break;
		case 'U':
			if (!read_hex(p, 8, &u))
				return FALSE;
			break;
		case '0':
			return fail(p, "a NUL character");
		default:
			return fail(p, "an escape that YAML does not have");
		}
		if (!append_unichar(p, text, u))
			return FALSE;
	}
}

static gboolean
read_single_quoted(
	Parser  *p,
	GString *text
){
	advance(p);
	while (TRUE) {
		gchar c = at(p, 0);

		if (p->pos >= p->len)
			return fail(p, "a quoted value that does not end");
		if (c == '\'') {
			if (at(p, 1) == '\'') {
				if (!append_checked(p, text, '\''))
					return FALSE;
				advance(p);
				advance(p);
				continue;
			}
			advance(p);
			return TRUE;
		}
		if (c == '\r' || c == '\n') {
			trim_blanks(text);
			if (!fold_break(p, text, FALSE))
				return FALSE;
			continue;
		}
		if (!append_checked(p, text, c))
			return FALSE;
		advance(p);
	}
}

static AxisYamlNode *
scalar_node(
	Parser  *p,
	guint    line,
	GString *text,
	gboolean quoted
){
	AxisYamlNode *node = node_new(p, AXIS_YAML_SCALAR);

	if (node == NULL) {
		g_string_free(text, TRUE);
		fail(p, "more values than the limit");
		return NULL;
	}
	node->line = line;
	node->quoted = quoted;
	node->text = g_string_free(text, FALSE);
	return node;
}

static AxisYamlNode *
quoted_scalar(Parser *p)
{
	GString *text = g_string_new(NULL);
	guint line = p->line;
	gboolean ok = at(p, 0) == '"' ? read_double_quoted(p, text) : read_single_quoted(p, text);

	if (!ok) {
		g_string_free(text, TRUE);
		return NULL;
	}
	return scalar_node(p, line, text, TRUE);
}

/*
 * One line of a plain scalar in block context, from the cursor to the end of
 * the line or a comment, blanks at its end dropped.  ": " inside it is
 * refused (YAML has no such scalar).
 */
static gboolean
read_plain_line(
	Parser  *p,
	GString *text
){
	while (!is_break(at(p, 0))) {
		gchar c = at(p, 0);

		if (is_blank(c) && at(p, 1) == '#')
			break;
		if (c == ':' && (is_blank(at(p, 1)) || is_break(at(p, 1))))
			return fail(p, "a \": \" inside a value without quotes");
		if (!append_checked(p, text, c))
			return FALSE;
		advance(p);
	}
	trim_blanks(text);
	return TRUE;
}

/*
 * A plain scalar in block context, continued on the lines after it that are
 * indented more than @parent (folded: a break is a space, an empty line a
 * '\n').  A comment, a line indented no more than @parent, or a document
 * marker ends it.
 */
static AxisYamlNode *
plain_scalar(
	Parser *p,
	gssize  parent
){
	GString *text = g_string_new(NULL);
	guint line = p->line;

	if (!read_plain_line(p, text)) {
		g_string_free(text, TRUE);
		return NULL;
	}
	while (TRUE) {
		gsize pos = p->pos;
		guint saved_line = p->line;
		gsize saved_start = p->line_start;
		guint empty = 0;
		gboolean more = FALSE;

		if (is_break(at(p, 0)) && p->pos < p->len) {
			if (at(p, 0) == '\r')
				advance(p);
			if (at(p, 0) == '\n')
				advance(p);
			while (TRUE) {
				skip_blanks(p);
				if (at(p, 0) == '\r')
					advance(p);
				if (at(p, 0) == '\n' && p->pos < p->len) {
					advance(p);
					empty++;
					continue;
				}
				break;
			}
			more = p->pos < p->len && at(p, 0) != '#' && (gssize)column(p) > parent && !at_document_marker(p);
		}
		if (!more) {
			p->pos = pos;
			p->line = saved_line;
			p->line_start = saved_start;
			break;
		}
		if (empty > 0) {
			while (empty-- > 0) {
				if (!append_checked(p, text, '\n')) {
					g_string_free(text, TRUE);
					return NULL;
				}
			}
		} else if (!append_checked(p, text, ' ')) {
			g_string_free(text, TRUE);
			return NULL;
		}
		if (!read_plain_line(p, text)) {
			g_string_free(text, TRUE);
			return NULL;
		}
	}
	return scalar_node(p, line, text, FALSE);
}

/* ── Flow nodes ─────────────────────────────────────────────────── */

/* Blanks, line breaks and comments inside a flow node. */
static void
skip_flow_space(Parser *p)
{
	while (TRUE) {
		gchar c = at(p, 0);

		if (is_blank(c) || c == '\r' || (c == '\n' && p->pos < p->len)) {
			advance(p);
			continue;
		}
		if (c == '#' && p->pos > 0 && (is_blank(p->s[p->pos - 1]) || p->s[p->pos - 1] == '\n')) {
			skip_comment(p);
			continue;
		}
		return;
	}
}

static gboolean
is_flow_indicator(gchar c)
{
	return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

/* A plain scalar inside a flow node: up to a flow indicator, ": ", " #" or
 * the end of the line. */
static AxisYamlNode *
flow_plain(Parser *p)
{
	GString *text = g_string_new(NULL);
	guint line = p->line;

	while (TRUE) {
		gchar c = at(p, 0);

		if (is_break(c) || is_flow_indicator(c))
			break;
		if (c == ':' && (is_blank(at(p, 1)) || is_break(at(p, 1)) || is_flow_indicator(at(p, 1))))
			break;
		if (is_blank(c) && at(p, 1) == '#')
			break;
		if (!append_checked(p, text, c)) {
			g_string_free(text, TRUE);
			return NULL;
		}
		advance(p);
	}
	trim_blanks(text);
	return scalar_node(p, line, text, FALSE);
}

static AxisYamlNode *flow_node(Parser *p);

/* One value inside a flow node: a nested one, a quoted or a plain scalar;
 * an empty plain scalar where there is none. */
static AxisYamlNode *
flow_value(Parser *p)
{
	gchar c = at(p, 0);
	const gchar *refused = refused_start(c);

	if (refused != NULL && c != '|' && c != '>') {
		fail(p, refused);
		return NULL;
	}
	if (c == '[' || c == '{')
		return flow_node(p);
	if (c == '"' || c == '\'')
		return quoted_scalar(p);
	return flow_plain(p);
}

/* A key and the map's keys so far: a key written twice is refused. */
static gboolean
add_pair(
	Parser       *p,
	AxisYamlNode *map,
	gchar        *key,
	AxisYamlNode *value
){
	guint i;

	for (i = 0; i < map->keys->len; i++) {
		if (strcmp(g_ptr_array_index(map->keys, i), key) == 0) {
			g_free(key);
			axis_yaml_node_free(value);
			return fail(p, "a key written twice in one map");
		}
	}
	g_ptr_array_add(map->keys, key);
	g_ptr_array_add(map->values, value);
	return TRUE;
}

/* "[a, b]" or "{k: v, ...}" at the cursor. */
static AxisYamlNode *
flow_node(Parser *p)
{
	gboolean is_map = at(p, 0) == '{';
	gchar close = is_map ? '}' : ']';
	AxisYamlNode *node;

	if (p->depth >= AXIS_YAML_MAX_DEPTH) {
		fail(p, "maps and lists nested deeper than the limit");
		return NULL;
	}
	node = node_new(p, is_map ? AXIS_YAML_MAP : AXIS_YAML_SEQ);
	if (node == NULL) {
		fail(p, "more values than the limit");
		return NULL;
	}
	p->depth++;
	advance(p);
	while (TRUE) {
		AxisYamlNode *first;

		skip_flow_space(p);
		if (p->pos >= p->len) {
			fail(p, "a list or map that does not end");
			break;
		}
		if (at(p, 0) == close) {
			advance(p);
			p->depth--;
			return node;
		}
		if (at(p, 0) == ',' || at(p, 0) == ']' || at(p, 0) == '}') {
			fail(p, "a list or map with a misplaced , ] or }");
			break;
		}
		first = flow_value(p);
		if (first == NULL)
			break;
		skip_flow_space(p);
		if (is_map) {
			AxisYamlNode *value = NULL;
			gchar *key;

			if (first->kind != AXIS_YAML_SCALAR) {
				axis_yaml_node_free(first);
				fail(p, "a key that is a list or a map");
				break;
			}
			if (at(p, 0) == ':') {
				advance(p);
				skip_flow_space(p);
				if (at(p, 0) == ',' || at(p, 0) == close) {
					value = node_new(p, AXIS_YAML_SCALAR);
					if (value != NULL)
						value->text = g_strdup("");
				} else {
					value = flow_value(p);
				}
			} else {
				/* A key alone: its value is null. */
				value = node_new(p, AXIS_YAML_SCALAR);
				if (value != NULL)
					value->text = g_strdup("");
			}
			if (value == NULL) {
				axis_yaml_node_free(first);
				fail(p, "more values than the limit");
				break;
			}
			key = g_steal_pointer(&first->text);
			axis_yaml_node_free(first);
			if (strlen(key) > KEY_MAX_BYTES) {
				g_free(key);
				axis_yaml_node_free(value);
				fail(p, "a key longer than the limit");
				break;
			}
			if (!add_pair(p, node, key, value))
				break;
		} else {
			if (at(p, 0) == ':') {
				axis_yaml_node_free(first);
				fail(p, "a key and value inside a list");
				break;
			}
			g_ptr_array_add(node->values, first);
		}
		skip_flow_space(p);
		if (at(p, 0) == ',') {
			advance(p);
			continue;
		}
		if (p->pos >= p->len) {
			fail(p, "a list or map that does not end");
			break;
		}
		if (at(p, 0) != close) {
			fail(p, "a list or map with something after a value where a , was expected");
			break;
		}
	}
	p->depth--;
	axis_yaml_node_free(node);
	return NULL;
}

/* ── Block nodes ────────────────────────────────────────────────── */

static AxisYamlNode *block_node(Parser *p, gssize parent);

/* After an inline value: only blanks and a comment to the end of the line. */
static gboolean
end_of_inline(Parser *p)
{
	skip_blanks(p);
	if (at(p, 0) == '#')
		skip_comment(p);
	if (!is_break(at(p, 0)))
		return fail(p, "text after a value on the same line");
	return TRUE;
}

/* A value on the line of its key or its "- ": a flow node, a quoted or a
 * plain scalar (which may continue on lines indented more than @parent). */
static AxisYamlNode *
inline_value(
	Parser *p,
	gssize  parent
){
	gchar c = at(p, 0);
	const gchar *refused = refused_start(c);
	AxisYamlNode *node;

	if (refused != NULL) {
		fail(p, refused);
		return NULL;
	}
	if (c == '[' || c == '{' || c == '"' || c == '\'') {
		node = c == '[' || c == '{' ? flow_node(p) : quoted_scalar(p);
		if (node != NULL && !end_of_inline(p)) {
			axis_yaml_node_free(node);
			return NULL;
		}
		return node;
	}
	if (c == ']' || c == '}' || c == ',') {
		fail(p, "a value cannot start with ] } or ,");
		return NULL;
	}
	return plain_scalar(p, parent);
}

/*
 * Whether the line at the cursor is a key of a block map: a quoted scalar
 * on one line, or plain text without a flow bracket at its start, then ':'
 * and a blank or the end of the line.
 */
static gboolean
line_is_key(const Parser *p)
{
	gsize i = p->pos;
	gchar c = p->s[i];

	if (c == '[' || c == '{' || c == '#' || refused_start(c) != NULL)
		return FALSE;
	if (c == '"' || c == '\'') {
		i++;
		while (i < p->len && p->s[i] != c && !is_break(p->s[i])) {
			if (c == '"' && p->s[i] == '\\' && i + 1 < p->len && !is_break(p->s[i + 1]))
				i++;
			else if (c == '\'' && p->s[i] == '\'' && i + 1 < p->len && p->s[i + 1] == '\'')
				i++;
			i++;
		}
		if (i >= p->len || p->s[i] != c)
			return FALSE;
		i++;
		while (i < p->len && is_blank(p->s[i]))
			i++;
		return i < p->len && p->s[i] == ':' && (i + 1 >= p->len || is_blank(p->s[i + 1]) || is_break(p->s[i + 1]));
	}
	for (; i < p->len && !is_break(p->s[i]); i++) {
		if (is_blank(p->s[i]) && i + 1 < p->len && p->s[i + 1] == '#')
			return FALSE;
		if (p->s[i] == ':' && (i + 1 >= p->len || is_blank(p->s[i + 1]) || is_break(p->s[i + 1])))
			return TRUE;
	}
	return FALSE;
}

/* A block map's key at the cursor, up to and past its ':'. */
static gchar *
read_key(Parser *p)
{
	GString *text = g_string_new(NULL);

	if (at(p, 0) == '"' || at(p, 0) == '\'') {
		gboolean ok = at(p, 0) == '"' ? read_double_quoted(p, text) : read_single_quoted(p, text);

		if (!ok) {
			g_string_free(text, TRUE);
			return NULL;
		}
		skip_blanks(p);
	} else {
		while (!(at(p, 0) == ':' && (is_blank(at(p, 1)) || is_break(at(p, 1))))) {
			if (is_break(at(p, 0))) {
				g_string_free(text, TRUE);
				fail(p, "a key without its ':'");
				return NULL;
			}
			if (!append_checked(p, text, at(p, 0))) {
				g_string_free(text, TRUE);
				return NULL;
			}
			advance(p);
		}
		trim_blanks(text);
	}
	if (at(p, 0) != ':') {
		g_string_free(text, TRUE);
		fail(p, "a key without its ':'");
		return NULL;
	}
	advance(p);
	if (text->len > KEY_MAX_BYTES) {
		g_string_free(text, TRUE);
		fail(p, "a key longer than the limit");
		return NULL;
	}
	return g_string_free(text, FALSE);
}

static AxisYamlNode *
null_node(Parser *p)
{
	AxisYamlNode *node = node_new(p, AXIS_YAML_SCALAR);

	if (node == NULL) {
		fail(p, "more values than the limit");
		return NULL;
	}
	node->text = g_strdup("");
	return node;
}

/* A block list whose "- " are at column @col; the cursor is on the first. */
static AxisYamlNode *
block_seq(
	Parser *p,
	gsize   col
){
	AxisYamlNode *node = node_new(p, AXIS_YAML_SEQ);

	if (node == NULL) {
		fail(p, "more values than the limit");
		return NULL;
	}
	while (TRUE) {
		AxisYamlNode *item;

		advance(p);
		skip_blanks(p);
		if (at(p, 0) == '#' || is_break(at(p, 0))) {
			/* The item is on the lines below, indented more, or null. */
			if (skip_to_content(p) && column(p) > col && !at_document_marker(p))
				item = block_node(p, (gssize)col);
			else
				item = p->problem == NULL ? null_node(p) : NULL;
		} else {
			item = block_node(p, (gssize)col);
		}
		if (item == NULL)
			break;
		g_ptr_array_add(node->values, item);
		if (!skip_to_content(p))
			return p->problem == NULL ? node : (axis_yaml_node_free(node), NULL);
		/* The list ends at a line indented less, or at one of its own
		 * column that is not an item (its map's next key). */
		if (at_document_marker(p) || column(p) < col || (column(p) == col && !at_seq_entry(p)))
			return node;
		if (column(p) > col) {
			fail(p, "a line indented more than the list it is in");
			break;
		}
	}
	axis_yaml_node_free(node);
	return NULL;
}

/* A block map whose keys are at column @col; the cursor is on the first. */
static AxisYamlNode *
block_map(
	Parser *p,
	gsize   col
){
	AxisYamlNode *node = node_new(p, AXIS_YAML_MAP);

	if (node == NULL) {
		fail(p, "more values than the limit");
		return NULL;
	}
	while (TRUE) {
		AxisYamlNode *value;
		gchar *key = read_key(p);

		if (key == NULL)
			break;
		skip_blanks(p);
		if (at(p, 0) == '#' || is_break(at(p, 0))) {
			/* The value is on the lines below: indented more, or a list at
			 * the key's own column; else it is null. */
			if (skip_to_content(p) && !at_document_marker(p) &&
			    (column(p) > col || (column(p) == col && at_seq_entry(p))))
				value = column(p) > col ? block_node(p, (gssize)col) : block_seq(p, col);
			else
				value = p->problem == NULL ? null_node(p) : NULL;
		} else {
			value = inline_value(p, (gssize)col);
		}
		if (value == NULL) {
			g_free(key);
			break;
		}
		if (!add_pair(p, node, key, value))
			break;
		if (!skip_to_content(p))
			return p->problem == NULL ? node : (axis_yaml_node_free(node), NULL);
		if (at_document_marker(p) || column(p) < col)
			return node;
		if (column(p) > col) {
			fail(p, "a line indented more than the map it is in");
			break;
		}
		if (at_seq_entry(p) || !line_is_key(p)) {
			fail(p, "a map followed by something that is not a key of it");
			break;
		}
	}
	axis_yaml_node_free(node);
	return NULL;
}

/* The node at the cursor, which is content; lines that continue it are
 * indented more than @parent. */
static AxisYamlNode *
block_node(
	Parser *p,
	gssize  parent
){
	AxisYamlNode *node;
	gsize col = column(p);

	if (p->depth >= AXIS_YAML_MAX_DEPTH) {
		fail(p, "maps and lists nested deeper than the limit");
		return NULL;
	}
	p->depth++;
	if (at_seq_entry(p))
		node = block_seq(p, col);
	else if (line_is_key(p))
		node = block_map(p, col);
	else
		node = inline_value(p, parent);
	p->depth--;
	return node;
}

/* ── The document ───────────────────────────────────────────────── */

AxisYamlNode *
axis_yaml_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	Parser p;
	g_autofree gchar *copy = NULL;
	AxisYamlNode *root = NULL;

	if (problem != NULL)
		*problem = NULL;
	if (data == NULL || length == 0) {
		if (problem != NULL)
			*problem = g_strdup("the file is empty");
		return NULL;
	}
	if (length > AXIS_YAML_MAX_BYTES) {
		if (problem != NULL)
			*problem = g_strdup("the file is larger than the limit");
		return NULL;
	}
	if (memchr(data, '\0', length) != NULL || !g_utf8_validate(data, (gssize)length, NULL)) {
		if (problem != NULL)
			*problem = g_strdup("the file is not UTF-8 text");
		return NULL;
	}
	copy = g_strndup(data, length);
	memset(&p, 0, sizeof(p));
	p.s = copy;
	p.len = length;
	p.line = 1;
	/* A byte order mark. */
	if (length >= 3 && (guchar)copy[0] == 0xef && (guchar)copy[1] == 0xbb && (guchar)copy[2] == 0xbf) {
		p.pos = 3;
		p.line_start = 3;
	}
	if (skip_to_content(&p)) {
		if (column(&p) == 0 && at(&p, 0) == '%') {
			fail(&p, "directives are not supported");
		} else if (at_document_marker(&p) && at(&p, 0) == '-') {
			advance(&p);
			advance(&p);
			advance(&p);
			skip_blanks(&p);
			if (at(&p, 0) == '#')
				skip_comment(&p);
			if (!is_break(at(&p, 0)))
				fail(&p, "text after the document's ---");
		}
	}
	if (p.problem == NULL && skip_to_content(&p)) {
		if (at_document_marker(&p))
			fail(&p, at(&p, 0) == '-' ? "more than one document" : "a document with nothing in it");
		else
			root = block_node(&p, -1);
	}
	if (root != NULL && p.problem == NULL && skip_to_content(&p)) {
		if (at_document_marker(&p) && at(&p, 0) == '.') {
			advance(&p);
			advance(&p);
			advance(&p);
			if (skip_to_content(&p))
				fail(&p, at_document_marker(&p) && at(&p, 0) == '-' ? "more than one document" :
					"text after the end of the document");
		} else if (at_document_marker(&p)) {
			fail(&p, "more than one document");
		} else {
			fail(&p, "text after the document");
		}
	}
	if (p.problem == NULL && root == NULL)
		fail(&p, "the file has no map in it");
	if (p.problem != NULL) {
		axis_yaml_node_free(root);
		if (problem != NULL)
			*problem = g_steal_pointer(&p.problem);
		g_free(p.problem);
		return NULL;
	}
	return root;
}
