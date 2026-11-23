/*
 * axis-def-select.c - the selector engine of indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A selector list is a list of complex selectors, each a list of compound
 * parts joined by combinators.  Matching goes from the rightmost part to the
 * left.  A :has() argument is a relative selector: it is read with a
 * virtual first part that matches only the element :has() is tested on (its
 * anchor), joined by the argument's first combinator (descendant when it
 * has none), so that one matcher serves both.
 */

#include "indexer/def/axis-def-select.h"

#include <string.h>

typedef enum
{
	COMB_DESCENDANT,
	COMB_CHILD,
	COMB_ADJACENT,
	COMB_SIBLING
} Comb;

typedef enum
{
	T_CLASS,
	T_ID,
	T_ATTR,
	T_HAS,
	T_NOT,
	T_CONTAINS,
	T_NTH_CHILD,
	T_NTH_LAST_CHILD,
	T_NTH_OF_TYPE,
	T_NTH_LAST_OF_TYPE,
	T_ONLY_CHILD,
	T_ONLY_OF_TYPE,
	T_EMPTY,
	T_ROOT,
	T_SCOPE,
	T_ANCHOR	/* the virtual first part of a :has() argument */
} TestKind;

typedef enum
{
	OP_EXISTS,
	OP_EQ,
	OP_PREFIX,
	OP_SUFFIX,
	OP_SUBSTRING,
	OP_WORD,
	OP_DASH,
	OP_NE
} AttrOp;

typedef struct
{
	TestKind      kind;
	gchar        *name;
	gchar        *value;
	AttrOp        op;
	gboolean      ci;
	gint          a;
	gint          b;
	AxisSelector *list;
} Test;

typedef struct
{
	gchar     *tag;	/* NULL: any */
	GPtrArray *tests;
	Comb       comb;	/* to the part on its left */
} Compound;

typedef struct
{
	GPtrArray *parts;	/* Compound *, left to right */
} Complex;

struct _AxisSelector
{
	GPtrArray *items;	/* Complex * */
};

/* ── Freeing ────────────────────────────────────────────────────── */

static void
test_free(Test *test)
{
	g_free(test->name);
	g_free(test->value);
	axis_selector_free(test->list);
	g_free(test);
}

static void
compound_free(Compound *compound)
{
	g_free(compound->tag);
	g_ptr_array_unref(compound->tests);
	g_free(compound);
}

static void
complex_free(Complex *complex)
{
	g_ptr_array_unref(complex->parts);
	g_free(complex);
}

void
axis_selector_free(AxisSelector *selector)
{
	if (selector == NULL)
		return;
	g_ptr_array_unref(selector->items);
	g_free(selector);
}

/* ── Reading ────────────────────────────────────────────────────── */

typedef struct
{
	const gchar *s;
	gsize        len;
	gsize        pos;
	guint        parts;
	guint        nesting;
	gchar       *problem;
	gboolean     unsupported;
} Reader;

static gboolean
refuse(
	Reader      *r,
	gboolean     unsupported,
	const gchar *what
){
	if (r->problem == NULL) {
		r->problem = g_strdup(what);
		r->unsupported = unsupported;
	}
	return FALSE;
}

static gchar
peek(const Reader *r)
{
	return r->pos < r->len ? r->s[r->pos] : '\0';
}

static gboolean
is_space(gchar c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void
skip_space(Reader *r)
{
	while (is_space(peek(r)))
		r->pos++;
}

static gboolean
is_ident_char(gchar c)
{
	return g_ascii_isalnum(c) || c == '-' || c == '_' || (guchar)c >= 0x80;
}

/* An identifier, with backslash escapes; %NULL when there is none. */
static gchar *
read_ident(Reader *r)
{
	GString *out = g_string_new(NULL);

	while (r->pos < r->len) {
		gchar c = peek(r);

		if (c == '\\' && r->pos + 1 < r->len) {
			g_string_append_c(out, r->s[r->pos + 1]);
			r->pos += 2;
			continue;
		}
		if (!is_ident_char(c))
			break;
		g_string_append_c(out, c);
		r->pos++;
	}
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

/* A quoted string at the cursor, with backslash escapes. */
static gchar *
read_string(Reader *r)
{
	gchar quote = peek(r);
	GString *out = g_string_new(NULL);

	r->pos++;
	while (r->pos < r->len && peek(r) != quote) {
		if (peek(r) == '\\' && r->pos + 1 < r->len)
			r->pos++;
		g_string_append_c(out, peek(r));
		r->pos++;
	}
	if (r->pos >= r->len) {
		g_string_free(out, TRUE);
		refuse(r, FALSE, "is not a selector: a quoted text does not end");
		return NULL;
	}
	r->pos++;
	return g_string_free(out, FALSE);
}

/* The text of a pseudo-class's argument up to its closing ')' (parentheses
 * inside counted, quotes kept), the cursor after the ')'. */
static gchar *
read_argument(Reader *r)
{
	gsize start = r->pos;
	guint depth = 0;
	gchar quote = 0;

	while (r->pos < r->len) {
		gchar c = peek(r);

		if (quote != 0) {
			if (c == '\\')
				r->pos++;
			else if (c == quote)
				quote = 0;
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (depth == 0) {
				gchar *text = g_strndup(r->s + start, r->pos - start);

				r->pos++;
				return text;
			}
			depth--;
		}
		r->pos++;
	}
	refuse(r, FALSE, "is not a selector: a ( does not end");
	return NULL;
}

/* "2n+1", "odd", "even", "3", "-n+2" into @a and @b. */
static gboolean
read_nth(
	const gchar *text,
	gint        *a,
	gint        *b
){
	g_autofree gchar *t = g_strstrip(g_ascii_strdown(text, -1));
	g_autofree gchar *compact = NULL;
	gchar *n;
	gchar *end = NULL;
	gint64 value;
	GString *s = g_string_new(NULL);
	const gchar *p;

	for (p = t; *p != '\0'; p++) {
		if (!is_space(*p))
			g_string_append_c(s, *p);
	}
	compact = g_string_free(s, FALSE);
	if (strcmp(compact, "odd") == 0) {
		*a = 2;
		*b = 1;
		return TRUE;
	}
	if (strcmp(compact, "even") == 0) {
		*a = 2;
		*b = 0;
		return TRUE;
	}
	if (compact[0] == '\0' || strlen(compact) > 16)
		return FALSE;
	n = strchr(compact, 'n');
	if (n == NULL) {
		value = g_ascii_strtoll(compact, &end, 10);
		if (end == NULL || *end != '\0' || end == compact || value < -100000 || value > 100000)
			return FALSE;
		*a = 0;
		*b = (gint)value;
		return TRUE;
	}
	if (n == compact || (n == compact + 1 && (compact[0] == '+' || compact[0] == '-'))) {
		*a = compact[0] == '-' ? -1 : 1;
	} else {
		*n = '\0';
		value = g_ascii_strtoll(compact, &end, 10);
		if (end == NULL || *end != '\0' || value < -100000 || value > 100000)
			return FALSE;
		*a = (gint)value;
	}
	p = n + 1;
	if (*p == '\0') {
		*b = 0;
		return TRUE;
	}
	if (*p != '+' && *p != '-')
		return FALSE;
	value = g_ascii_strtoll(p, &end, 10);
	if (end == NULL || *end != '\0' || end == p + 1 || value < -100000 || value > 100000)
		return FALSE;
	*b = (gint)value;
	return TRUE;
}

static AxisSelector *read_list(Reader *r, gboolean relative, gchar close);

/* A :has() or :not() argument, read as a selector list of its own. */
static AxisSelector *
nested_list(
	Reader      *r,
	const gchar *text,
	gboolean     relative
){
	Reader inner;
	AxisSelector *list;

	if (r->nesting >= AXIS_SELECT_MAX_NESTING) {
		refuse(r, FALSE, "is too complex: :has() and :not() nested deeper than the limit");
		return NULL;
	}
	memset(&inner, 0, sizeof(inner));
	inner.s = text;
	inner.len = strlen(text);
	inner.parts = r->parts;
	inner.nesting = r->nesting + 1;
	list = read_list(&inner, relative, '\0');
	r->parts = inner.parts;
	if (list == NULL) {
		refuse(r, inner.unsupported, inner.problem != NULL ? inner.problem : "is not a selector");
		g_free(inner.problem);
		return NULL;
	}
	g_free(inner.problem);
	return list;
}

static Test *
read_pseudo(Reader *r)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *arg = NULL;
	Test *test;

	r->pos++;
	if (peek(r) == ':') {
		refuse(r, TRUE, "pseudo-elements (::) are not supported");
		return NULL;
	}
	name = read_ident(r);
	if (name == NULL) {
		refuse(r, FALSE, "is not a selector: a : without a name");
		return NULL;
	}
	{
		gchar *lower = g_ascii_strdown(name, -1);

		g_free(name);
		name = lower;
	}
	if (peek(r) == '(') {
		r->pos++;
		arg = read_argument(r);
		if (arg == NULL)
			return NULL;
	}
	test = g_new0(Test, 1);
	if (strcmp(name, "has") == 0 || strcmp(name, "not") == 0) {
		test->kind = name[0] == 'h' ? T_HAS : T_NOT;
		if (arg == NULL || (test->list = nested_list(r, arg, test->kind == T_HAS)) == NULL) {
			if (arg == NULL)
				refuse(r, FALSE, "is not a selector: :has() or :not() without its argument");
			test_free(test);
			return NULL;
		}
		return test;
	}
	if (strcmp(name, "contains") == 0) {
		g_autofree gchar *stripped = arg != NULL ? g_strstrip(g_strdup(arg)) : NULL;
		gsize n = stripped != NULL ? strlen(stripped) : 0;

		test->kind = T_CONTAINS;
		if (stripped == NULL) {
			test_free(test);
			refuse(r, FALSE, "is not a selector: :contains() without its argument");
			return NULL;
		}
		if (n >= 2 && (stripped[0] == '"' || stripped[0] == '\'') && stripped[n - 1] == stripped[0])
			test->value = g_strndup(stripped + 1, n - 2);
		else
			test->value = g_strdup(stripped);
		return test;
	}
	if (g_str_has_prefix(name, "nth-")) {
		if (strcmp(name, "nth-child") == 0)
			test->kind = T_NTH_CHILD;
		else if (strcmp(name, "nth-last-child") == 0)
			test->kind = T_NTH_LAST_CHILD;
		else if (strcmp(name, "nth-of-type") == 0)
			test->kind = T_NTH_OF_TYPE;
		else if (strcmp(name, "nth-last-of-type") == 0)
			test->kind = T_NTH_LAST_OF_TYPE;
		else {
			g_autofree gchar *what = g_strdup_printf("the selector :%s() is not supported", name);

			test_free(test);
			refuse(r, TRUE, what);
			return NULL;
		}
		if (arg == NULL || !read_nth(arg, &test->a, &test->b)) {
			test_free(test);
			refuse(r, FALSE, "is not a selector: an :nth- argument that is not a number or an+b");
			return NULL;
		}
		return test;
	}
	if (arg == NULL) {
		static const struct { const gchar *name; TestKind kind; gint a; gint b; } simple[] = {
			{ "first-child", T_NTH_CHILD, 0, 1 }, { "last-child", T_NTH_LAST_CHILD, 0, 1 },
			{ "first-of-type", T_NTH_OF_TYPE, 0, 1 }, { "last-of-type", T_NTH_LAST_OF_TYPE, 0, 1 },
			{ "only-child", T_ONLY_CHILD, 0, 0 }, { "only-of-type", T_ONLY_OF_TYPE, 0, 0 },
			{ "empty", T_EMPTY, 0, 0 }, { "root", T_ROOT, 0, 0 }, { "scope", T_SCOPE, 0, 0 }
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS(simple); i++) {
			if (strcmp(name, simple[i].name) == 0) {
				test->kind = simple[i].kind;
				test->a = simple[i].a;
				test->b = simple[i].b;
				return test;
			}
		}
	}
	test_free(test);
	{
		g_autofree gchar *what = g_strdup_printf("the selector :%s%s is not supported", name, arg != NULL ? "()" : "");

		refuse(r, TRUE, what);
	}
	return NULL;
}

static Test *
read_attribute(Reader *r)
{
	Test *test = g_new0(Test, 1);
	gchar c;

	test->kind = T_ATTR;
	r->pos++;
	skip_space(r);
	test->name = read_ident(r);
	if (test->name == NULL) {
		test_free(test);
		refuse(r, FALSE, "is not a selector: an attribute test without a name");
		return NULL;
	}
	skip_space(r);
	c = peek(r);
	test->op = OP_EXISTS;
	if (c == '=') {
		test->op = OP_EQ;
		r->pos++;
	} else if ((c == '^' || c == '$' || c == '*' || c == '~' || c == '|' || c == '!') && r->pos + 1 < r->len &&
	           r->s[r->pos + 1] == '=') {
		test->op = c == '^' ? OP_PREFIX : c == '$' ? OP_SUFFIX : c == '*' ? OP_SUBSTRING : c == '~' ? OP_WORD :
			c == '|' ? OP_DASH : OP_NE;
		r->pos += 2;
	} else if (c != ']') {
		test_free(test);
		refuse(r, FALSE, c == '\0' ? "is not a selector: an attribute test that does not end" :
			"is not a selector: an attribute test that is not one");
		return NULL;
	}
	if (test->op != OP_EXISTS) {
		skip_space(r);
		if (peek(r) == '"' || peek(r) == '\'') {
			test->value = read_string(r);
		} else {
			gsize start = r->pos;

			while (r->pos < r->len && peek(r) != ']' && !is_space(peek(r)))
				r->pos++;
			test->value = g_strndup(r->s + start, r->pos - start);
		}
		if (test->value == NULL) {
			test_free(test);
			return NULL;
		}
		skip_space(r);
		if (peek(r) == 'i' || peek(r) == 'I' || peek(r) == 's' || peek(r) == 'S') {
			test->ci = peek(r) == 'i' || peek(r) == 'I';
			r->pos++;
			skip_space(r);
		}
	}
	if (peek(r) != ']') {
		test_free(test);
		refuse(r, FALSE, "is not a selector: an attribute test that does not end");
		return NULL;
	}
	r->pos++;
	return test;
}

/* One compound part at the cursor; %NULL with no problem when there is
 * nothing to read. */
static Compound *
read_compound(Reader *r)
{
	Compound *compound;
	gchar c = peek(r);
	gboolean universal = FALSE;

	if (r->parts >= AXIS_SELECT_MAX_PARTS) {
		refuse(r, FALSE, "is too complex: more parts than the limit");
		return NULL;
	}
	compound = g_new0(Compound, 1);
	compound->tests = g_ptr_array_new_with_free_func((GDestroyNotify)test_free);
	if (c == '*') {
		r->pos++;
		universal = TRUE;
	} else if (is_ident_char(c) || c == '\\') {
		compound->tag = read_ident(r);
	}
	while (r->problem == NULL) {
		Test *test = NULL;

		c = peek(r);
		if (c == '.' || c == '#') {
			r->pos++;
			test = g_new0(Test, 1);
			test->kind = c == '.' ? T_CLASS : T_ID;
			test->name = read_ident(r);
			if (test->name == NULL) {
				test_free(test);
				refuse(r, FALSE, "is not a selector: a . or # without a name");
				break;
			}
		} else if (c == '[') {
			test = read_attribute(r);
		} else if (c == ':') {
			test = read_pseudo(r);
		} else {
			break;
		}
		if (test == NULL)
			break;
		g_ptr_array_add(compound->tests, test);
	}
	if (r->problem != NULL || (compound->tag == NULL && compound->tests->len == 0 && !universal)) {
		compound_free(compound);
		if (r->problem == NULL)
			refuse(r, FALSE, "is not a selector: a character that starts no part of one");
		return NULL;
	}
	r->parts++;
	return compound;
}

/* A combinator at the cursor (after the blanks before it), or descendant
 * when there were blanks and nothing else; @found FALSE when neither. */
static Comb
read_comb(
	Reader   *r,
	gboolean *found
){
	gboolean space = is_space(peek(r));
	gchar c;

	skip_space(r);
	c = peek(r);
	*found = TRUE;
	if (c == '>') {
		r->pos++;
		skip_space(r);
		return COMB_CHILD;
	}
	if (c == '+') {
		r->pos++;
		skip_space(r);
		return COMB_ADJACENT;
	}
	if (c == '~') {
		r->pos++;
		skip_space(r);
		return COMB_SIBLING;
	}
	*found = space;
	return COMB_DESCENDANT;
}

static Complex *
read_complex(
	Reader   *r,
	gboolean  relative,
	gchar     close
){
	Complex *complex = g_new0(Complex, 1);

	complex->parts = g_ptr_array_new_with_free_func((GDestroyNotify)compound_free);
	skip_space(r);
	if (relative) {
		/* The anchor, and how the argument relates to it. */
		Compound *anchor = g_new0(Compound, 1);
		Test *test = g_new0(Test, 1);
		gboolean found = FALSE;
		Comb comb;

		anchor->tests = g_ptr_array_new_with_free_func((GDestroyNotify)test_free);
		test->kind = T_ANCHOR;
		g_ptr_array_add(anchor->tests, test);
		g_ptr_array_add(complex->parts, anchor);
		comb = read_comb(r, &found);
		if (!found)
			comb = COMB_DESCENDANT;
		{
			Compound *first = read_compound(r);

			if (first == NULL) {
				complex_free(complex);
				return NULL;
			}
			first->comb = comb;
			g_ptr_array_add(complex->parts, first);
		}
	} else {
		Compound *first = read_compound(r);

		if (first == NULL) {
			complex_free(complex);
			return NULL;
		}
		g_ptr_array_add(complex->parts, first);
	}
	while (TRUE) {
		gboolean found = FALSE;
		Comb comb = read_comb(r, &found);
		Compound *next;

		if (r->pos >= r->len || peek(r) == ',' || peek(r) == close)
			break;
		if (!found) {
			refuse(r, FALSE, "is not a selector: two parts without a combinator between them");
			complex_free(complex);
			return NULL;
		}
		next = read_compound(r);
		if (next == NULL) {
			complex_free(complex);
			return NULL;
		}
		next->comb = comb;
		g_ptr_array_add(complex->parts, next);
	}
	return complex;
}

static AxisSelector *
read_list(
	Reader   *r,
	gboolean  relative,
	gchar     close
){
	AxisSelector *list = g_new0(AxisSelector, 1);

	list->items = g_ptr_array_new_with_free_func((GDestroyNotify)complex_free);
	while (TRUE) {
		Complex *complex = read_complex(r, relative, close);

		if (complex == NULL) {
			axis_selector_free(list);
			return NULL;
		}
		g_ptr_array_add(list->items, complex);
		skip_space(r);
		if (peek(r) == ',') {
			r->pos++;
			continue;
		}
		if (r->pos >= r->len)
			return list;
		refuse(r, FALSE, "is not a selector: something after its end");
		axis_selector_free(list);
		return NULL;
	}
}

AxisSelector *
axis_selector_parse(
	const gchar  *text,
	gchar       **problem,
	gboolean     *unsupported
){
	Reader r;
	AxisSelector *list;

	if (problem != NULL)
		*problem = NULL;
	if (unsupported != NULL)
		*unsupported = FALSE;
	memset(&r, 0, sizeof(r));
	if (text == NULL || strlen(text) > AXIS_SELECT_MAX_BYTES) {
		if (problem != NULL)
			*problem = g_strdup(text == NULL ? "is empty" : "is longer than the limit");
		return NULL;
	}
	r.s = text;
	r.len = strlen(text);
	skip_space(&r);
	if (r.pos >= r.len) {
		if (problem != NULL)
			*problem = g_strdup("is empty");
		return NULL;
	}
	list = read_list(&r, FALSE, '\0');
	if (list == NULL) {
		if (problem != NULL)
			*problem = g_strdup(r.problem != NULL ? r.problem : "is not a selector");
		if (unsupported != NULL)
			*unsupported = r.unsupported;
	}
	g_free(r.problem);
	return list;
}

/* ── Matching ───────────────────────────────────────────────────── */

static gboolean
spend(
	AxisSelectBudget *budget,
	guint64           steps
){
	if (budget->exhausted)
		return FALSE;
	budget->steps += steps;
	if (budget->steps > budget->max_steps) {
		budget->exhausted = TRUE;
		return FALSE;
	}
	return TRUE;
}

static const AxisHtmlNode *
parent_element(const AxisHtmlNode *node)
{
	return node->parent != NULL && node->parent->type == AXIS_HTML_ELEMENT ? node->parent : NULL;
}

static gboolean
same_name(
	const gchar *a,
	const gchar *b
){
	return g_ascii_strcasecmp(a, b) == 0;
}

/* Whether @word is one of @list's blank-separated words. */
static gboolean
has_word(
	const gchar *list,
	const gchar *word,
	gboolean     ci
){
	gsize n = strlen(word);
	const gchar *p = list;

	if (n == 0)
		return FALSE;
	while (*p != '\0') {
		const gchar *end;

		while (is_space(*p))
			p++;
		end = p;
		while (*end != '\0' && !is_space(*end))
			end++;
		if ((gsize)(end - p) == n && (ci ? g_ascii_strncasecmp(p, word, n) == 0 : strncmp(p, word, n) == 0))
			return TRUE;
		p = end;
	}
	return FALSE;
}

static gboolean
attr_matches(
	const Test         *test,
	const AxisHtmlNode *el
){
	const gchar *value = axis_html_attr(el, test->name);
	g_autofree gchar *v = NULL;
	g_autofree gchar *want = NULL;
	gsize n;
	gsize m;

	if (test->op == OP_NE)
		return value == NULL || (test->ci ? g_ascii_strcasecmp(value, test->value) != 0 :
			strcmp(value, test->value) != 0);
	if (value == NULL)
		return FALSE;
	if (test->op == OP_EXISTS)
		return TRUE;
	if (test->op == OP_WORD)
		return has_word(value, test->value, test->ci);
	v = test->ci ? g_ascii_strdown(value, -1) : g_strdup(value);
	want = test->ci ? g_ascii_strdown(test->value, -1) : g_strdup(test->value);
	n = strlen(v);
	m = strlen(want);
	switch (test->op) {
	case OP_EQ:
		return strcmp(v, want) == 0;
	case OP_PREFIX:
		return m > 0 && g_str_has_prefix(v, want);
	case OP_SUFFIX:
		return m > 0 && n >= m && strcmp(v + n - m, want) == 0;
	case OP_SUBSTRING:
		return m > 0 && strstr(v, want) != NULL;
	case OP_DASH:
		return strcmp(v, want) == 0 || (g_str_has_prefix(v, want) && v[m] == '-');
	default:
		return FALSE;
	}
}

/* @el's position among its element siblings (1 = first), counted from the
 * end with @from_end, of its own name only with @of_type. */
static gint
position(
	const AxisHtmlNode *el,
	gboolean            from_end,
	gboolean            of_type,
	AxisSelectBudget   *budget
){
	const AxisHtmlNode *n;
	gint index = 1;

	for (n = from_end ? axis_html_next_element(el) : axis_html_prev_element(el); n != NULL;
	     n = from_end ? axis_html_next_element(n) : axis_html_prev_element(n)) {
		if (!spend(budget, 1))
			return 0;
		if (!of_type || same_name(n->name, el->name))
			index++;
	}
	return index;
}

static gboolean
nth_matches(
	gint a,
	gint b,
	gint index
){
	if (index <= 0)
		return FALSE;
	if (a == 0)
		return index == b;
	return (index - b) % a == 0 && (index - b) / a >= 0;
}

static gboolean complex_matches(const Complex *complex, guint index, const AxisHtmlNode *el,
                                const AxisHtmlNode *scope, const AxisHtmlNode *anchor, AxisSelectBudget *budget);

static gboolean list_matches(const AxisSelector *list, const AxisHtmlNode *el, const AxisHtmlNode *scope,
                             const AxisHtmlNode *anchor, AxisSelectBudget *budget);

/* :has(): an element among @anchor's descendants (or its following siblings
 * and theirs, for "+" and "~") that matches the relative selector. */
static gboolean
has_matches(
	const AxisSelector *list,
	const AxisHtmlNode *anchor,
	const AxisHtmlNode *scope,
	AxisSelectBudget   *budget
){
	guint i;

	for (i = 0; i < list->items->len; i++) {
		const Complex *complex = g_ptr_array_index(list->items, i);
		const Compound *second = g_ptr_array_index(complex->parts, 1);
		guint last = complex->parts->len - 1;
		const AxisHtmlNode *start;
		const AxisHtmlNode *n;

		if (second->comb == COMB_DESCENDANT || second->comb == COMB_CHILD) {
			for (n = anchor->first; n != NULL; n = axis_html_following(n, anchor)) {
				if (budget->exhausted)
					return FALSE;
				if (n->type == AXIS_HTML_ELEMENT && complex_matches(complex, last, n, scope, anchor, budget))
					return TRUE;
			}
			continue;
		}
		for (start = axis_html_next_element(anchor); start != NULL; start = axis_html_next_element(start)) {
			if (complex_matches(complex, last, start, scope, anchor, budget))
				return TRUE;
			for (n = start->first; n != NULL; n = axis_html_following(n, start)) {
				if (budget->exhausted)
					return FALSE;
				if (n->type == AXIS_HTML_ELEMENT && complex_matches(complex, last, n, scope, anchor, budget))
					return TRUE;
			}
		}
	}
	return FALSE;
}

static gboolean
test_matches(
	const Test         *test,
	const AxisHtmlNode *el,
	const AxisHtmlNode *scope,
	const AxisHtmlNode *anchor,
	AxisSelectBudget   *budget
){
	switch (test->kind) {
	case T_CLASS: {
		const gchar *value = axis_html_attr(el, "class");

		return value != NULL && has_word(value, test->name, FALSE);
	}
	case T_ID: {
		const gchar *value = axis_html_attr(el, "id");

		return value != NULL && strcmp(value, test->name) == 0;
	}
	case T_ATTR:
		return attr_matches(test, el);
	case T_HAS:
		return has_matches(test->list, el, scope, budget);
	case T_NOT:
		return !list_matches(test->list, el, scope, anchor, budget) && !budget->exhausted;
	case T_CONTAINS: {
		g_autofree gchar *text = axis_html_text(el, AXIS_SELECT_CONTAINS_BYTES);

		if (!spend(budget, strlen(text) / 1024 + 1))
			return FALSE;
		return strstr(text, test->value) != NULL;
	}
	case T_NTH_CHILD:
		return nth_matches(test->a, test->b, position(el, FALSE, FALSE, budget));
	case T_NTH_LAST_CHILD:
		return nth_matches(test->a, test->b, position(el, TRUE, FALSE, budget));
	case T_NTH_OF_TYPE:
		return nth_matches(test->a, test->b, position(el, FALSE, TRUE, budget));
	case T_NTH_LAST_OF_TYPE:
		return nth_matches(test->a, test->b, position(el, TRUE, TRUE, budget));
	case T_ONLY_CHILD:
		return axis_html_prev_element(el) == NULL && axis_html_next_element(el) == NULL;
	case T_ONLY_OF_TYPE:
		return position(el, FALSE, TRUE, budget) == 1 && position(el, TRUE, TRUE, budget) == 1;
	case T_EMPTY:
		return el->first == NULL;
	case T_ROOT:
		return el->parent != NULL && el->parent->type == AXIS_HTML_DOCUMENT;
	case T_SCOPE:
		return scope != NULL ? el == scope : el->parent != NULL && el->parent->type == AXIS_HTML_DOCUMENT;
	case T_ANCHOR:
		return el == anchor;
	default:
		return FALSE;
	}
}

static gboolean
compound_matches(
	const Compound     *compound,
	const AxisHtmlNode *el,
	const AxisHtmlNode *scope,
	const AxisHtmlNode *anchor,
	AxisSelectBudget   *budget
){
	guint i;

	if (el->type != AXIS_HTML_ELEMENT)
		return FALSE;
	if (compound->tag != NULL && !same_name(compound->tag, el->name))
		return FALSE;
	for (i = 0; i < compound->tests->len; i++) {
		if (!test_matches(g_ptr_array_index(compound->tests, i), el, scope, anchor, budget))
			return FALSE;
	}
	return TRUE;
}

static gboolean
complex_matches(
	const Complex      *complex,
	guint               index,
	const AxisHtmlNode *el,
	const AxisHtmlNode *scope,
	const AxisHtmlNode *anchor,
	AxisSelectBudget   *budget
){
	const Compound *compound = g_ptr_array_index(complex->parts, index);
	const AxisHtmlNode *n;

	if (!spend(budget, 1))
		return FALSE;
	if (!compound_matches(compound, el, scope, anchor, budget))
		return FALSE;
	if (index == 0)
		return TRUE;
	switch (compound->comb) {
	case COMB_CHILD:
		n = parent_element(el);
		if (n == NULL && anchor != NULL && el->parent == anchor)
			n = anchor;
		return n != NULL && complex_matches(complex, index - 1, n, scope, anchor, budget);
	case COMB_DESCENDANT:
		for (n = parent_element(el); n != NULL; n = parent_element(n)) {
			if (complex_matches(complex, index - 1, n, scope, anchor, budget))
				return TRUE;
			if (budget->exhausted)
				return FALSE;
		}
		return FALSE;
	case COMB_ADJACENT:
		n = axis_html_prev_element(el);
		return n != NULL && complex_matches(complex, index - 1, n, scope, anchor, budget);
	case COMB_SIBLING:
		for (n = axis_html_prev_element(el); n != NULL; n = axis_html_prev_element(n)) {
			if (complex_matches(complex, index - 1, n, scope, anchor, budget))
				return TRUE;
			if (budget->exhausted)
				return FALSE;
		}
		return FALSE;
	default:
		return FALSE;
	}
}

static gboolean
list_matches(
	const AxisSelector *list,
	const AxisHtmlNode *el,
	const AxisHtmlNode *scope,
	const AxisHtmlNode *anchor,
	AxisSelectBudget   *budget
){
	guint i;

	for (i = 0; i < list->items->len; i++) {
		const Complex *complex = g_ptr_array_index(list->items, i);

		if (complex_matches(complex, complex->parts->len - 1, el, scope, anchor, budget))
			return TRUE;
		if (budget->exhausted)
			return FALSE;
	}
	return FALSE;
}

gboolean
axis_selector_matches(
	const AxisSelector *selector,
	const AxisHtmlNode *element,
	const AxisHtmlNode *scope,
	AxisSelectBudget   *budget
){
	if (selector == NULL || element == NULL || element->type != AXIS_HTML_ELEMENT)
		return FALSE;
	return list_matches(selector, element, scope, NULL, budget);
}

const AxisHtmlNode *
axis_selector_first(
	const AxisSelector *selector,
	const AxisHtmlNode *context,
	AxisSelectBudget   *budget
){
	const AxisHtmlNode *n;
	const AxisHtmlNode *scope = context != NULL && context->type == AXIS_HTML_ELEMENT ? context : NULL;

	if (selector == NULL || context == NULL)
		return NULL;
	for (n = context->first; n != NULL && !budget->exhausted; n = axis_html_following(n, context)) {
		if (n->type == AXIS_HTML_ELEMENT && list_matches(selector, n, scope, NULL, budget))
			return n;
	}
	return NULL;
}

GPtrArray *
axis_selector_all(
	const AxisSelector *selector,
	const AxisHtmlNode *context,
	guint               max,
	AxisSelectBudget   *budget
){
	GPtrArray *found = g_ptr_array_new();
	const AxisHtmlNode *n;
	const AxisHtmlNode *scope = context != NULL && context->type == AXIS_HTML_ELEMENT ? context : NULL;

	if (selector == NULL || context == NULL)
		return found;
	for (n = context->first; n != NULL && found->len < max && !budget->exhausted;
	     n = axis_html_following(n, context)) {
		if (n->type == AXIS_HTML_ELEMENT && list_matches(selector, n, scope, NULL, budget))
			g_ptr_array_add(found, (gpointer)n);
	}
	return found;
}
