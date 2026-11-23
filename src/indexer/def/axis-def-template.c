/*
 * axis-def-template.c - the template evaluator of indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The text is cut into literal text and actions; the actions are read into
 * a tree of if, range and output nodes over small expressions; rendering
 * walks the tree.  Every list in the tree is bounded by the parse's node
 * count, every recursion by its depth.
 */

#include "indexer/def/axis-def-template.h"

#include <string.h>

typedef enum
{
	E_FIELD,
	E_DOT,
	E_STRING,
	E_BOOL,
	E_CALL
} ExprKind;

typedef enum
{
	F_AND,
	F_OR,
	F_NOT,
	F_EQ,
	F_NE,
	F_JOIN,
	F_RE_REPLACE
} Function;

typedef struct _Expr Expr;
struct _Expr
{
	ExprKind   kind;
	gchar     *text;	/* a field's name without its dot; a string's text */
	gboolean   truth;	/* E_BOOL */
	Function   function;
	GPtrArray *args;	/* Expr *, E_CALL */
};

typedef enum
{
	N_TEXT,
	N_OUTPUT,
	N_IF,
	N_RANGE
} NodeKind;

typedef struct
{
	NodeKind   kind;
	gchar     *text;
	Expr      *expr;
	GPtrArray *body;
	GPtrArray *otherwise;
} Node;

struct _AxisTemplate
{
	GPtrArray *nodes;
};

/* ── Freeing ────────────────────────────────────────────────────── */

static void
expr_free(Expr *expr)
{
	if (expr == NULL)
		return;
	g_free(expr->text);
	g_clear_pointer(&expr->args, g_ptr_array_unref);
	g_free(expr);
}

static void
node_free(Node *node)
{
	if (node == NULL)
		return;
	g_free(node->text);
	expr_free(node->expr);
	g_clear_pointer(&node->body, g_ptr_array_unref);
	g_clear_pointer(&node->otherwise, g_ptr_array_unref);
	g_free(node);
}

static GPtrArray *
node_list(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)node_free);
}

void
axis_template_free(AxisTemplate *template_)
{
	if (template_ == NULL)
		return;
	g_ptr_array_unref(template_->nodes);
	g_free(template_);
}

gboolean
axis_template_has_actions(const gchar *text)
{
	return text != NULL && strstr(text, "{{") != NULL;
}

/* ── Cutting the text ───────────────────────────────────────────── */

typedef struct
{
	gboolean  action;
	gchar    *text;	/* the literal text, or the action's inside */
} Segment;

static void
segment_free(Segment *segment)
{
	g_free(segment->text);
	g_free(segment);
}

/* The text as segments; "{{-" and "-}}" drop the blanks beside them. */
static GPtrArray *
cut(
	const gchar  *text,
	gchar       **problem
){
	GPtrArray *segments = g_ptr_array_new_with_free_func((GDestroyNotify)segment_free);
	const gchar *p = text;
	gboolean trim_next = FALSE;

	while (*p != '\0') {
		const gchar *open = strstr(p, "{{");
		const gchar *inside;
		const gchar *close;
		const gchar *end;
		gboolean trim_before;
		gboolean trim_after;
		Segment *segment;

		if (open == NULL)
			open = p + strlen(p);
		if (open > p || trim_next) {
			gchar *literal = g_strndup(p, (gsize)(open - p));

			if (trim_next)
				g_strchug(literal);
			trim_next = FALSE;
			segment = g_new0(Segment, 1);
			segment->text = literal;
			g_ptr_array_add(segments, segment);
		}
		if (*open == '\0')
			break;
		inside = open + 2;
		trim_before = inside[0] == '-' && (inside[1] == ' ' || inside[1] == '\t' || inside[1] == '\n');
		if (trim_before) {
			inside++;
			if (segments->len > 0) {
				Segment *last = g_ptr_array_index(segments, segments->len - 1);

				if (!last->action)
					g_strchomp(last->text);
			}
		}
		close = strstr(inside, "}}");
		if (close == NULL) {
			if (problem != NULL)
				*problem = g_strdup("a {{ that does not end");
			g_ptr_array_unref(segments);
			return NULL;
		}
		end = close;
		trim_after = close > inside && close[-1] == '-' && close - 1 > inside &&
			(close[-2] == ' ' || close[-2] == '\t' || close[-2] == '\n');
		if (trim_after)
			end = close - 1;
		segment = g_new0(Segment, 1);
		segment->action = TRUE;
		segment->text = g_strndup(inside, (gsize)(end - inside));
		g_ptr_array_add(segments, segment);
		trim_next = trim_after;
		p = close + 2;
	}
	return segments;
}

/* ── Reading actions ────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *segments;
	guint      index;
	guint      nodes;
	guint      depth;
	gchar     *problem;
	gboolean   unsupported;
} Parser;

static gboolean
refuse(
	Parser      *p,
	gboolean     unsupported,
	const gchar *what
){
	if (p->problem == NULL) {
		p->problem = g_strdup(what);
		p->unsupported = unsupported;
	}
	return FALSE;
}

typedef struct
{
	const gchar *s;
	gsize        pos;
} Lexer;

static void
lex_space(Lexer *l)
{
	while (l->s[l->pos] == ' ' || l->s[l->pos] == '\t' || l->s[l->pos] == '\n' || l->s[l->pos] == '\r')
		l->pos++;
}

static gboolean
is_word_char(gchar c)
{
	return g_ascii_isalnum(c) || c == '_' || c == '-' || c == '.';
}

/* A word at the lexer (a keyword, a function, a field), %NULL for none. */
static gchar *
lex_word(Lexer *l)
{
	gsize start = l->pos;

	while (is_word_char(l->s[l->pos]))
		l->pos++;
	return l->pos > start ? g_strndup(l->s + start, l->pos - start) : NULL;
}

static gboolean
known_root(const gchar *name)
{
	static const gchar *const roots[] = { "Keywords", "Query", "Config", "Result", "Categories", "True", "False",
		"Today", NULL };
	g_autofree gchar *root = g_strdup(name);
	gchar *dot = strchr(root, '.');
	guint i;

	if (dot != NULL)
		*dot = '\0';
	for (i = 0; roots[i] != NULL; i++) {
		if (strcmp(roots[i], root) == 0)
			return TRUE;
	}
	return FALSE;
}

static Expr *parse_command(Parser *p, Lexer *l, gchar close);

static Expr *
new_expr(
	Parser   *p,
	ExprKind  kind
){
	Expr *expr;

	if (p->nodes >= AXIS_TEMPLATE_MAX_NODES) {
		refuse(p, FALSE, "a template with more parts than the limit");
		return NULL;
	}
	p->nodes++;
	expr = g_new0(Expr, 1);
	expr->kind = kind;
	return expr;
}

/* One operand: a field, ".", a quoted text, a number, true or false, a
 * parenthesised command; %NULL at the end of the action. */
static Expr *
parse_operand(
	Parser *p,
	Lexer  *l
){
	gchar c;
	Expr *expr;

	lex_space(l);
	c = l->s[l->pos];
	if (c == '\0' || c == ')')
		return NULL;
	if (c == '(') {
		if (p->depth >= AXIS_TEMPLATE_MAX_DEPTH) {
			refuse(p, FALSE, "a template nested deeper than the limit");
			return NULL;
		}
		l->pos++;
		p->depth++;
		expr = parse_command(p, l, ')');
		p->depth--;
		if (expr == NULL)
			return NULL;
		lex_space(l);
		if (l->s[l->pos] != ')') {
			expr_free(expr);
			refuse(p, FALSE, "a ( in a template that does not end");
			return NULL;
		}
		l->pos++;
		return expr;
	}
	if (c == '"' || c == '`') {
		GString *text = g_string_new(NULL);

		/* Backslashes are kept as they are (the definitions' patterns are
		 * written so, as the engine they are written for reads them), but
		 * for a quote escaped inside quotes. */
		l->pos++;
		while (l->s[l->pos] != '\0' && l->s[l->pos] != c) {
			if (c == '"' && l->s[l->pos] == '\\' && l->s[l->pos + 1] == '"')
				l->pos++;
			g_string_append_c(text, l->s[l->pos]);
			l->pos++;
		}
		if (l->s[l->pos] != c) {
			g_string_free(text, TRUE);
			refuse(p, FALSE, "a quoted text in a template that does not end");
			return NULL;
		}
		l->pos++;
		expr = new_expr(p, E_STRING);
		if (expr == NULL) {
			g_string_free(text, TRUE);
			return NULL;
		}
		expr->text = g_string_free(text, FALSE);
		return expr;
	}
	if (c == '$') {
		refuse(p, TRUE, "template variables ($) are not supported");
		return NULL;
	}
	if (c == '|') {
		refuse(p, TRUE, "template pipes (|) are not supported");
		return NULL;
	}
	if (c == '.' || g_ascii_isalnum(c) || c == '-' || c == '_') {
		g_autofree gchar *word = lex_word(l);

		if (word == NULL) {
			refuse(p, FALSE, "a template with a character it cannot read");
			return NULL;
		}
		if (strcmp(word, ".") == 0)
			return new_expr(p, E_DOT);
		if (word[0] == '.') {
			if (!known_root(word + 1)) {
				g_autofree gchar *what = g_strdup_printf("the template variable %.40s is not supported", word);

				refuse(p, TRUE, what);
				return NULL;
			}
			expr = new_expr(p, E_FIELD);
			if (expr != NULL)
				expr->text = g_strdup(word + 1);
			return expr;
		}
		if (strcmp(word, "true") == 0 || strcmp(word, "false") == 0) {
			expr = new_expr(p, E_BOOL);
			if (expr != NULL)
				expr->truth = word[0] == 't';
			return expr;
		}
		if (g_ascii_isdigit(word[0]) || (word[0] == '-' && g_ascii_isdigit(word[1]))) {
			expr = new_expr(p, E_STRING);
			if (expr != NULL)
				expr->text = g_strdup(word);
			return expr;
		}
		/* A function's name where an operand was wanted. */
		{
			g_autofree gchar *what = g_strdup_printf("the template function %.40s without parentheses is not "
				"supported here", word);

			refuse(p, TRUE, what);
		}
		return NULL;
	}
	refuse(p, FALSE, "a template with a character it cannot read");
	return NULL;
}

static gboolean
function_of(
	const gchar *name,
	Function    *function
){
	static const struct { const gchar *name; Function function; } table[] = {
		{ "and", F_AND }, { "or", F_OR }, { "not", F_NOT }, { "eq", F_EQ }, { "ne", F_NE }, { "join", F_JOIN },
		{ "re_replace", F_RE_REPLACE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		if (strcmp(table[i].name, name) == 0) {
			*function = table[i].function;
			return TRUE;
		}
	}
	return FALSE;
}

/* A command: a function and its operands, or one operand. */
static Expr *
parse_command(
	Parser *p,
	Lexer  *l,
	gchar   close
){
	gsize start;
	Expr *expr;
	Function function;

	lex_space(l);
	start = l->pos;
	if (g_ascii_isalpha(l->s[l->pos]) || l->s[l->pos] == '_') {
		g_autofree gchar *word = lex_word(l);

		if (word != NULL && strcmp(word, "true") != 0 && strcmp(word, "false") != 0) {
			if (!function_of(word, &function)) {
				g_autofree gchar *what = g_strdup_printf("the template function %.40s is not supported", word);

				refuse(p, TRUE, what);
				return NULL;
			}
			expr = new_expr(p, E_CALL);
			if (expr == NULL)
				return NULL;
			expr->function = function;
			expr->args = g_ptr_array_new_with_free_func((GDestroyNotify)expr_free);
			while (TRUE) {
				Expr *arg = parse_operand(p, l);

				if (arg == NULL)
					break;
				g_ptr_array_add(expr->args, arg);
			}
			if (p->problem != NULL) {
				expr_free(expr);
				return NULL;
			}
			if ((function == F_NOT && expr->args->len != 1) ||
			    ((function == F_EQ || function == F_NE || function == F_JOIN) && expr->args->len < 2) ||
			    (function == F_NE && expr->args->len != 2) ||
			    (function == F_JOIN && expr->args->len != 2) ||
			    (function == F_RE_REPLACE && expr->args->len != 3) ||
			    ((function == F_AND || function == F_OR) && expr->args->len < 1)) {
				expr_free(expr);
				refuse(p, FALSE, "a template function with the wrong number of arguments");
				return NULL;
			}
			return expr;
		}
		l->pos = start;
	}
	expr = parse_operand(p, l);
	if (expr == NULL) {
		if (p->problem == NULL)
			refuse(p, FALSE, "a template action with nothing in it");
		return NULL;
	}
	lex_space(l);
	if (l->s[l->pos] != '\0' && l->s[l->pos] != close) {
		expr_free(expr);
		if (l->s[l->pos] == '|')
			refuse(p, TRUE, "template pipes (|) are not supported");
		else
			refuse(p, FALSE, "a template action with more than one value and no function");
		return NULL;
	}
	return expr;
}

/* An action's whole text as one command. */
static Expr *
parse_action_expr(
	Parser      *p,
	const gchar *text
){
	Lexer l;
	Expr *expr;

	l.s = text;
	l.pos = 0;
	expr = parse_command(p, &l, '\0');
	if (expr == NULL)
		return NULL;
	lex_space(&l);
	if (l.s[l.pos] != '\0') {
		expr_free(expr);
		refuse(p, FALSE, "a template action with something after its end");
		return NULL;
	}
	return expr;
}

/* The keyword an action starts with ("if", "else", ...), and the rest. */
static gchar *
action_keyword(
	const gchar  *text,
	const gchar **rest
){
	Lexer l;
	gsize start;

	l.s = text;
	l.pos = 0;
	lex_space(&l);
	start = l.pos;
	while (g_ascii_isalpha(l.s[l.pos]))
		l.pos++;
	*rest = l.s + l.pos;
	return g_strndup(l.s + start, l.pos - start);
}

static GPtrArray *parse_list(Parser *p, gchar **stop);

static Node *
new_node(
	Parser   *p,
	NodeKind  kind
){
	Node *node;

	if (p->nodes >= AXIS_TEMPLATE_MAX_NODES) {
		refuse(p, FALSE, "a template with more parts than the limit");
		return NULL;
	}
	p->nodes++;
	node = g_new0(Node, 1);
	node->kind = kind;
	return node;
}

/* An if (or an else if) whose condition is @condition; reads up to and
 * past its {{ end }}. */
static Node *
parse_if(
	Parser      *p,
	const gchar *condition
){
	Node *node = new_node(p, N_IF);
	g_autofree gchar *stop = NULL;

	if (node == NULL)
		return NULL;
	node->expr = parse_action_expr(p, condition);
	if (node->expr == NULL) {
		node_free(node);
		return NULL;
	}
	node->body = parse_list(p, &stop);
	if (node->body == NULL) {
		node_free(node);
		return NULL;
	}
	if (stop != NULL && g_str_has_prefix(stop, "else")) {
		const gchar *rest = NULL;
		g_autofree gchar *keyword = action_keyword(stop + 4, &rest);

		if (strcmp(keyword, "if") == 0) {
			Node *inner;

			node->otherwise = node_list();
			if (p->depth >= AXIS_TEMPLATE_MAX_DEPTH) {
				refuse(p, FALSE, "a template nested deeper than the limit");
				node_free(node);
				return NULL;
			}
			p->depth++;
			inner = parse_if(p, rest);
			p->depth--;
			if (inner == NULL) {
				node_free(node);
				return NULL;
			}
			g_ptr_array_add(node->otherwise, inner);
			return node;
		}
		if (keyword[0] != '\0' || rest[strspn(rest, " \t\r\n")] != '\0') {
			refuse(p, FALSE, "a template's else with something after it");
			node_free(node);
			return NULL;
		}
		g_clear_pointer(&stop, g_free);
		node->otherwise = parse_list(p, &stop);
		if (node->otherwise == NULL) {
			node_free(node);
			return NULL;
		}
	}
	if (stop == NULL || strcmp(stop, "end") != 0) {
		refuse(p, FALSE, "a template's if without its end");
		node_free(node);
		return NULL;
	}
	return node;
}

static Node *
parse_range(
	Parser      *p,
	const gchar *what
){
	Node *node = new_node(p, N_RANGE);
	g_autofree gchar *stop = NULL;

	if (node == NULL)
		return NULL;
	if (strstr(what, ":=") != NULL || strchr(what, '$') != NULL) {
		refuse(p, TRUE, "template variables ($) are not supported");
		node_free(node);
		return NULL;
	}
	node->expr = parse_action_expr(p, what);
	if (node->expr == NULL) {
		node_free(node);
		return NULL;
	}
	node->body = parse_list(p, &stop);
	if (node->body == NULL) {
		node_free(node);
		return NULL;
	}
	if (stop != NULL && strcmp(stop, "else") == 0) {
		g_clear_pointer(&stop, g_free);
		node->otherwise = parse_list(p, &stop);
		if (node->otherwise == NULL) {
			node_free(node);
			return NULL;
		}
	}
	if (stop == NULL || strcmp(stop, "end") != 0) {
		refuse(p, FALSE, "a template's range without its end");
		node_free(node);
		return NULL;
	}
	return node;
}

/*
 * Nodes up to an {{ else ... }} or {{ end }} (put in @stop: "end", "else",
 * or "else" and the rest of that action) or the end of the text (@stop
 * %NULL).
 */
static GPtrArray *
parse_list(
	Parser  *p,
	gchar  **stop
){
	GPtrArray *nodes = node_list();

	*stop = NULL;
	if (p->depth >= AXIS_TEMPLATE_MAX_DEPTH) {
		refuse(p, FALSE, "a template nested deeper than the limit");
		g_ptr_array_unref(nodes);
		return NULL;
	}
	p->depth++;
	while (p->index < p->segments->len && p->problem == NULL) {
		Segment *segment = g_ptr_array_index(p->segments, p->index++);
		const gchar *rest = NULL;
		g_autofree gchar *keyword = NULL;
		Node *node = NULL;

		if (!segment->action) {
			node = new_node(p, N_TEXT);
			if (node != NULL)
				node->text = g_strdup(segment->text);
		} else {
			keyword = action_keyword(segment->text, &rest);
			if (strcmp(keyword, "end") == 0 || strcmp(keyword, "else") == 0) {
				if (strcmp(keyword, "end") == 0 && rest[strspn(rest, " \t\r\n")] != '\0') {
					refuse(p, FALSE, "a template's end with something after it");
					break;
				}
				if (strcmp(keyword, "end") == 0) {
					*stop = g_strdup("end");
				} else {
					g_autofree gchar *after = g_strstrip(g_strdup(rest));

					*stop = after[0] != '\0' ? g_strconcat("else ", after, NULL) : g_strdup("else");
				}
				p->depth--;
				return nodes;
			}
			if (strcmp(keyword, "if") == 0) {
				node = parse_if(p, rest);
			} else if (strcmp(keyword, "range") == 0) {
				node = parse_range(p, rest);
			} else if (strcmp(keyword, "with") == 0 || strcmp(keyword, "define") == 0 ||
			           strcmp(keyword, "template") == 0 || strcmp(keyword, "block") == 0 ||
			           strcmp(keyword, "break") == 0 || strcmp(keyword, "continue") == 0) {
				g_autofree gchar *what = g_strdup_printf("the template keyword %s is not supported", keyword);

				refuse(p, TRUE, what);
			} else if (g_str_has_prefix(g_strchug(segment->text), "/*")) {
				/* A comment. */
				continue;
			} else {
				node = new_node(p, N_OUTPUT);
				if (node != NULL) {
					node->expr = parse_action_expr(p, segment->text);
					if (node->expr == NULL)
						g_clear_pointer(&node, node_free);
				}
			}
		}
		if (node == NULL)
			break;
		g_ptr_array_add(nodes, node);
	}
	p->depth--;
	if (p->problem != NULL) {
		g_ptr_array_unref(nodes);
		return NULL;
	}
	return nodes;
}

AxisTemplate *
axis_template_parse(
	const gchar  *text,
	gchar       **problem,
	gboolean     *unsupported
){
	Parser p;
	g_autofree gchar *stop = NULL;
	GPtrArray *nodes;
	AxisTemplate *template_;

	if (problem != NULL)
		*problem = NULL;
	if (unsupported != NULL)
		*unsupported = FALSE;
	if (text == NULL)
		text = "";
	if (strlen(text) > AXIS_TEMPLATE_MAX_BYTES) {
		if (problem != NULL)
			*problem = g_strdup("a template longer than the limit");
		return NULL;
	}
	memset(&p, 0, sizeof(p));
	p.segments = cut(text, problem);
	if (p.segments == NULL)
		return NULL;
	nodes = parse_list(&p, &stop);
	if (nodes != NULL && stop != NULL) {
		refuse(&p, FALSE, g_str_has_prefix(stop, "else") ? "a template's else without its if" :
			"a template's end without its if or range");
		g_ptr_array_unref(nodes);
		nodes = NULL;
	}
	g_ptr_array_unref(p.segments);
	if (nodes == NULL) {
		if (problem != NULL)
			*problem = g_strdup(p.problem != NULL ? p.problem : "a template that cannot be read");
		if (unsupported != NULL)
			*unsupported = p.unsupported;
		g_free(p.problem);
		return NULL;
	}
	g_free(p.problem);
	template_ = g_new0(AxisTemplate, 1);
	template_->nodes = nodes;
	return template_;
}

/* ── Rendering ──────────────────────────────────────────────────── */

typedef enum
{
	V_NIL,
	V_TEXT,
	V_LIST,
	V_BOOL
} ValueKind;

typedef struct
{
	ValueKind   kind;
	gchar      *text;	/* owned, V_TEXT */
	GPtrArray  *list;	/* borrowed, V_LIST */
	gboolean    truth;
} Value;

typedef struct
{
	const AxisTemplateVars *vars;
	AxisTemplateEscape      escape;
	AxisDefRegexCache      *regex;
	const gchar            *dot;	/* the item of the range we are in */
	GString                *out;
	gchar                  *problem;
} Render;

static void
value_clear(Value *value)
{
	g_clear_pointer(&value->text, g_free);
	value->kind = V_NIL;
}

static gboolean
truthy(const Value *value)
{
	switch (value->kind) {
	case V_TEXT:
		return value->text[0] != '\0';
	case V_LIST:
		return value->list->len > 0;
	case V_BOOL:
		return value->truth;
	default:
		return FALSE;
	}
}

static gchar *
value_text(const Value *value)
{
	GString *out;
	guint i;

	switch (value->kind) {
	case V_TEXT:
		return g_strdup(value->text);
	case V_BOOL:
		return g_strdup(value->truth ? "true" : "false");
	case V_LIST:
		out = g_string_new("[");
		for (i = 0; i < value->list->len; i++)
			g_string_append_printf(out, "%s%s", i > 0 ? " " : "", (const gchar *)g_ptr_array_index(value->list, i));
		g_string_append_c(out, ']');
		return g_string_free(out, FALSE);
	default:
		return g_strdup("");
	}
}

static gboolean eval(Render *r, const Expr *expr, Value *value);

static gboolean
eval_call(
	Render     *r,
	const Expr *expr,
	Value      *value
){
	Value args[3];
	guint i;
	gboolean ok = TRUE;

	memset(value, 0, sizeof(*value));
	memset(args, 0, sizeof(args));
	switch (expr->function) {
	case F_AND:
	case F_OR:
		/* The first that decides, else the last. */
		for (i = 0; i < expr->args->len; i++) {
			Value v;

			if (!eval(r, g_ptr_array_index(expr->args, i), &v))
				return FALSE;
			if (i + 1 == expr->args->len || (expr->function == F_AND ? !truthy(&v) : truthy(&v))) {
				*value = v;
				return TRUE;
			}
			value_clear(&v);
		}
		return TRUE;
	case F_NOT:
		if (!eval(r, g_ptr_array_index(expr->args, 0), &args[0]))
			return FALSE;
		value->kind = V_BOOL;
		value->truth = !truthy(&args[0]);
		value_clear(&args[0]);
		return TRUE;
	case F_EQ:
	case F_NE: {
		g_autofree gchar *first = NULL;
		gboolean equal = FALSE;

		if (!eval(r, g_ptr_array_index(expr->args, 0), &args[0]))
			return FALSE;
		first = value_text(&args[0]);
		value_clear(&args[0]);
		for (i = 1; i < expr->args->len && !equal; i++) {
			g_autofree gchar *other = NULL;

			if (!eval(r, g_ptr_array_index(expr->args, i), &args[1]))
				return FALSE;
			other = value_text(&args[1]);
			value_clear(&args[1]);
			equal = strcmp(first, other) == 0;
		}
		value->kind = V_BOOL;
		value->truth = expr->function == F_EQ ? equal : !equal;
		return TRUE;
	}
	case F_JOIN: {
		g_autofree gchar *separator = NULL;

		if (!eval(r, g_ptr_array_index(expr->args, 0), &args[0]))
			return FALSE;
		if (!eval(r, g_ptr_array_index(expr->args, 1), &args[1])) {
			value_clear(&args[0]);
			return FALSE;
		}
		separator = value_text(&args[1]);
		value->kind = V_TEXT;
		if (args[0].kind == V_LIST) {
			GString *out = g_string_new(NULL);

			for (i = 0; i < args[0].list->len; i++)
				g_string_append_printf(out, "%s%s", i > 0 ? separator : "",
					(const gchar *)g_ptr_array_index(args[0].list, i));
			value->text = g_string_free(out, FALSE);
		} else {
			value->text = args[0].kind == V_NIL ? g_strdup("") : value_text(&args[0]);
		}
		value_clear(&args[0]);
		value_clear(&args[1]);
		return TRUE;
	}
	case F_RE_REPLACE: {
		g_autofree gchar *input = NULL;
		g_autofree gchar *pattern = NULL;
		g_autofree gchar *replacement = NULL;
		gchar *problem = NULL;

		for (i = 0; i < 3 && ok; i++)
			ok = eval(r, g_ptr_array_index(expr->args, i), &args[i]);
		if (!ok) {
			for (i = 0; i < 3; i++)
				value_clear(&args[i]);
			return FALSE;
		}
		input = value_text(&args[0]);
		pattern = value_text(&args[1]);
		replacement = value_text(&args[2]);
		for (i = 0; i < 3; i++)
			value_clear(&args[i]);
		value->text = axis_def_regex_replace(r->regex, pattern, input, replacement, &problem);
		if (value->text == NULL) {
			if (r->problem == NULL)
				r->problem = problem;
			else
				g_free(problem);
			return FALSE;
		}
		value->kind = V_TEXT;
		return TRUE;
	}
	default:
		return FALSE;
	}
}

static gboolean
eval(
	Render     *r,
	const Expr *expr,
	Value      *value
){
	const gchar *text;

	memset(value, 0, sizeof(*value));
	switch (expr->kind) {
	case E_STRING:
		value->kind = V_TEXT;
		value->text = g_strdup(expr->text);
		return TRUE;
	case E_BOOL:
		value->kind = V_BOOL;
		value->truth = expr->truth;
		return TRUE;
	case E_DOT:
		if (r->dot != NULL) {
			value->kind = V_TEXT;
			value->text = g_strdup(r->dot);
		}
		return TRUE;
	case E_FIELD:
		if (strcmp(expr->text, "Categories") == 0) {
			if (r->vars->categories != NULL) {
				value->kind = V_LIST;
				value->list = r->vars->categories;
			}
			return TRUE;
		}
		if (strcmp(expr->text, "True") == 0) {
			value->kind = V_TEXT;
			value->text = g_strdup("True");
			return TRUE;
		}
		if (strcmp(expr->text, "False") == 0) {
			value->kind = V_TEXT;
			value->text = g_strdup("");
			return TRUE;
		}
		text = r->vars->values != NULL ? g_hash_table_lookup(r->vars->values, expr->text) : NULL;
		if (text != NULL) {
			value->kind = V_TEXT;
			value->text = g_strdup(text);
		}
		return TRUE;
	case E_CALL:
		return eval_call(r, expr, value);
	default:
		return FALSE;
	}
}

static gboolean
emit(
	Render      *r,
	const gchar *text
){
	if (r->out->len + strlen(text) > AXIS_TEMPLATE_MAX_OUTPUT) {
		if (r->problem == NULL)
			r->problem = g_strdup("a template's text came out longer than the limit");
		return FALSE;
	}
	g_string_append(r->out, text);
	return TRUE;
}

static gboolean render_list(Render *r, const GPtrArray *nodes);

static gboolean
render_node(
	Render     *r,
	const Node *node
){
	Value value;
	gboolean ok = TRUE;
	guint i;

	switch (node->kind) {
	case N_TEXT:
		return emit(r, node->text);
	case N_OUTPUT: {
		g_autofree gchar *text = NULL;

		if (!eval(r, node->expr, &value))
			return FALSE;
		text = value_text(&value);
		value_clear(&value);
		if (r->escape != NULL) {
			gchar *escaped = r->escape(text);

			g_free(text);
			text = escaped;
		}
		return emit(r, text);
	}
	case N_IF:
		if (!eval(r, node->expr, &value))
			return FALSE;
		ok = truthy(&value);
		value_clear(&value);
		if (ok)
			return render_list(r, node->body);
		return node->otherwise == NULL || render_list(r, node->otherwise);
	case N_RANGE:
		if (!eval(r, node->expr, &value))
			return FALSE;
		if (value.kind == V_LIST && value.list->len > 0) {
			const gchar *saved = r->dot;

			for (i = 0; i < value.list->len && i < AXIS_TEMPLATE_MAX_RANGE && ok; i++) {
				r->dot = g_ptr_array_index(value.list, i);
				ok = render_list(r, node->body);
			}
			r->dot = saved;
		} else if (value.kind == V_TEXT && value.text[0] != '\0') {
			/* Ranging over text: Go refuses it; one item is kinder. */
			const gchar *saved = r->dot;

			r->dot = value.text;
			ok = render_list(r, node->body);
			r->dot = saved;
		} else if (node->otherwise != NULL) {
			ok = render_list(r, node->otherwise);
		}
		value_clear(&value);
		return ok;
	default:
		return FALSE;
	}
}

static gboolean
render_list(
	Render          *r,
	const GPtrArray *nodes
){
	guint i;

	for (i = 0; i < nodes->len; i++) {
		if (!render_node(r, g_ptr_array_index(nodes, i)))
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_template_render(
	const AxisTemplate      *template_,
	const AxisTemplateVars  *vars,
	AxisTemplateEscape       escape,
	AxisDefRegexCache       *regex,
	gchar                  **problem
){
	Render r;
	AxisTemplateVars none;

	if (problem != NULL)
		*problem = NULL;
	memset(&none, 0, sizeof(none));
	memset(&r, 0, sizeof(r));
	r.vars = vars != NULL ? vars : &none;
	r.escape = escape;
	r.regex = regex;
	r.out = g_string_new(NULL);
	if (!render_list(&r, template_->nodes)) {
		g_string_free(r.out, TRUE);
		if (problem != NULL)
			*problem = r.problem != NULL ? g_steal_pointer(&r.problem) : g_strdup("a template could not be rendered");
		g_free(r.problem);
		return NULL;
	}
	g_free(r.problem);
	return g_string_free(r.out, FALSE);
}

gchar *
axis_template_expand(
	const gchar             *text,
	const AxisTemplateVars  *vars,
	AxisTemplateEscape       escape,
	AxisDefRegexCache       *regex,
	gchar                  **problem
){
	g_autoptr(AxisTemplate) template_ = NULL;

	if (!axis_template_has_actions(text))
		return g_strdup(text != NULL ? text : "");
	template_ = axis_template_parse(text, problem, NULL);
	if (template_ == NULL)
		return NULL;
	return axis_template_render(template_, vars, escape, regex, problem);
}
