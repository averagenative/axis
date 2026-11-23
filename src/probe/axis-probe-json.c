/*
 * axis-probe-json.c - a JSON reader that cannot be made to use much memory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-json.h"

#include <math.h>
#include <string.h>

#include "library/axis-library-name.h"

typedef struct _AxisJsonMember AxisJsonMember;

struct _AxisJsonNode
{
	guint8   type;
	guint32  count;	/* elements or members; 0 or 1 for a boolean */
	union
	{
		const gchar    *string;
		AxisJsonNode   *items;
		AxisJsonMember *members;
	} u;
};

struct _AxisJsonMember
{
	const gchar  *key;
	AxisJsonNode  value;
};

#define ARENA_BLOCK 65536

struct _AxisJsonDoc
{
	GPtrArray    *blocks;
	guint8       *cursor;
	gsize         left;
	AxisJsonNode  root;
};

typedef struct
{
	const guchar *p;
	const guchar *end;
	AxisJsonDoc  *doc;
	guint         nodes;
	GString      *scratch;
	const gchar  *problem;
} Parser;

/* ── Memory ─────────────────────────────────────────────────────── */

static gpointer
arena_alloc(
	AxisJsonDoc *doc,
	gsize        size
){
	gpointer result;

	size = (size + 7) & ~(gsize)7;
	if (size == 0)
		size = 8;
	if (size > ARENA_BLOCK / 4) {
		result = g_malloc0(size);
		g_ptr_array_add(doc->blocks, result);
		return result;
	}
	if (doc->left < size) {
		doc->cursor = g_malloc0(ARENA_BLOCK);
		doc->left = ARENA_BLOCK;
		g_ptr_array_add(doc->blocks, doc->cursor);
	}
	result = doc->cursor;
	doc->cursor += size;
	doc->left -= size;
	return result;
}

static const gchar *
arena_string(
	AxisJsonDoc *doc,
	const gchar *text,
	gsize        length
){
	gchar *copy = arena_alloc(doc, length + 1);

	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}

void
axis_json_doc_free(AxisJsonDoc *doc)
{
	if (doc == NULL)
		return;

	g_ptr_array_free(doc->blocks, TRUE);
	g_free(doc);
}

const AxisJsonNode *
axis_json_doc_get_root(AxisJsonDoc *doc)
{
	g_return_val_if_fail(doc != NULL, NULL);

	return &doc->root;
}

/* ── Reading ────────────────────────────────────────────────────── */

static void
skip_space(Parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static gint
hex_value(guchar c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Four hex digits at @p; -1 if they are not. */
static gint32
read_hex4(
	const guchar *p,
	const guchar *end
){
	gint32 value = 0;
	guint i;

	if (end - p < 4)
		return -1;
	for (i = 0; i < 4; i++) {
		gint digit = hex_value(p[i]);

		if (digit < 0)
			return -1;
		value = (value << 4) | digit;
	}
	return value;
}

/* Keeps text only up to AXIS_JSON_MAX_STRING bytes; *@full says it is
 * kept in full. */
static void
scratch_append(
	Parser      *ps,
	const gchar *bytes,
	gsize        length,
	gboolean    *full
){
	if (!*full)
		return;
	if (ps->scratch->len + length > AXIS_JSON_MAX_STRING) {
		*full = FALSE;
		return;
	}
	g_string_append_len(ps->scratch, bytes, (gssize)length);
}

/* After the opening quote.  The result is in ps->scratch. */
static gboolean
read_string(Parser *ps)
{
	gboolean full = TRUE;

	g_string_set_size(ps->scratch, 0);
	for (;;) {
		guchar c;

		if (ps->p >= ps->end) {
			ps->problem = "a string is not closed";
			return FALSE;
		}
		c = *ps->p;
		if (c == '"') {
			ps->p++;
			return TRUE;
		}
		if (c < 0x20) {
			ps->problem = "a string holds a raw control character";
			return FALSE;
		}
		if (c == '\\') {
			gchar utf8[6];
			gint length;
			gint32 code;

			ps->p++;
			if (ps->p >= ps->end) {
				ps->problem = "a string ends inside an escape";
				return FALSE;
			}
			c = *ps->p++;
			switch (c) {
			case '"':
			case '\\':
			case '/':
				scratch_append(ps, (const gchar *)&c, 1, &full);
				break;
			case 'b':
				scratch_append(ps, "\b", 1, &full);
				break;
			case 'f':
				scratch_append(ps, "\f", 1, &full);
				break;
			case 'n':
				scratch_append(ps, "\n", 1, &full);
				break;
			case 'r':
				scratch_append(ps, "\r", 1, &full);
				break;
			case 't':
				scratch_append(ps, "\t", 1, &full);
				break;
			case 'u':
				code = read_hex4(ps->p, ps->end);
				if (code < 0) {
					ps->problem = "a string has a malformed \\u escape";
					return FALSE;
				}
				ps->p += 4;
				if (code >= 0xd800 && code <= 0xdbff) {
					/* A high surrogate needs its low half right behind. */
					gint32 low = -1;

					if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u')
						low = read_hex4(ps->p + 2, ps->end);
					if (low >= 0xdc00 && low <= 0xdfff) {
						ps->p += 6;
						code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
					} else {
						code = 0xfffd;
					}
				} else if ((code >= 0xdc00 && code <= 0xdfff) || code == 0) {
					code = 0xfffd;
				}
				length = g_unichar_to_utf8((gunichar)code, utf8);
				scratch_append(ps, utf8, (gsize)length, &full);
				break;
			default:
				ps->problem = "a string has an unknown escape";
				return FALSE;
			}
			continue;
		}
		if (c < 0x80) {
			scratch_append(ps, (const gchar *)ps->p, 1, &full);
			ps->p++;
			continue;
		}
		{
			gsize n = axis_library_utf8_sequence(ps->p, (gsize)(ps->end - ps->p));

			if (n > 0) {
				scratch_append(ps, (const gchar *)ps->p, n, &full);
				ps->p += n;
			} else {
				scratch_append(ps, "\xef\xbf\xbd", 3, &full);
				ps->p++;
			}
		}
	}
}

static gboolean
read_number(
	Parser       *ps,
	AxisJsonNode *out
){
	const guchar *start = ps->p;
	const guchar *p = ps->p;
	gsize length;

	if (p < ps->end && *p == '-')
		p++;
	if (p >= ps->end || *p < '0' || *p > '9')
		goto bad;
	if (*p == '0') {
		p++;
	} else {
		while (p < ps->end && *p >= '0' && *p <= '9')
			p++;
	}
	if (p < ps->end && *p == '.') {
		p++;
		if (p >= ps->end || *p < '0' || *p > '9')
			goto bad;
		while (p < ps->end && *p >= '0' && *p <= '9')
			p++;
	}
	if (p < ps->end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < ps->end && (*p == '+' || *p == '-'))
			p++;
		if (p >= ps->end || *p < '0' || *p > '9')
			goto bad;
		while (p < ps->end && *p >= '0' && *p <= '9')
			p++;
	}
	length = (gsize)(p - start);
	if (length > AXIS_JSON_MAX_NUMBER) {
		ps->problem = "a number is too long";
		return FALSE;
	}
	out->type = AXIS_JSON_NUMBER;
	out->u.string = arena_string(ps->doc, (const gchar *)start, length);
	ps->p = p;
	return TRUE;

bad:
	ps->problem = "malformed number";
	return FALSE;
}

static gboolean
read_literal(
	Parser       *ps,
	const gchar  *word,
	AxisJsonType  type,
	guint32       count,
	AxisJsonNode *out
){
	gsize length = strlen(word);

	if ((gsize)(ps->end - ps->p) < length || memcmp(ps->p, word, length) != 0) {
		ps->problem = "unexpected text where a value should be";
		return FALSE;
	}
	ps->p += length;
	out->type = type;
	out->count = count;
	return TRUE;
}

static gboolean
read_value(
	Parser       *ps,
	guint         depth,
	AxisJsonNode *out);

static gboolean
read_array(
	Parser       *ps,
	guint         depth,
	AxisJsonNode *out
){
	GArray *items = g_array_new(FALSE, FALSE, sizeof(AxisJsonNode));

	/* The opening bracket is behind us. */
	skip_space(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		goto done;
	}
	for (;;) {
		AxisJsonNode item;

		memset(&item, 0, sizeof(item));
		if (!read_value(ps, depth + 1, &item))
			goto fail;
		g_array_append_val(items, item);
		skip_space(ps);
		if (ps->p >= ps->end) {
			ps->problem = "an array is not closed";
			goto fail;
		}
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		ps->problem = "malformed array";
		goto fail;
	}

done:
	out->type = AXIS_JSON_ARRAY;
	out->count = items->len;
	if (items->len > 0) {
		out->u.items = arena_alloc(ps->doc, items->len * sizeof(AxisJsonNode));
		memcpy(out->u.items, items->data, items->len * sizeof(AxisJsonNode));
	}
	g_array_free(items, TRUE);
	return TRUE;

fail:
	g_array_free(items, TRUE);
	return FALSE;
}

static gboolean
read_object(
	Parser       *ps,
	guint         depth,
	AxisJsonNode *out
){
	GArray *members = g_array_new(FALSE, FALSE, sizeof(AxisJsonMember));

	skip_space(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		goto done;
	}
	for (;;) {
		AxisJsonMember member;

		memset(&member, 0, sizeof(member));
		skip_space(ps);
		if (ps->p >= ps->end || *ps->p != '"') {
			ps->problem = "an object member has no name";
			goto fail;
		}
		ps->p++;
		if (!read_string(ps))
			goto fail;
		member.key = arena_string(ps->doc, ps->scratch->str, ps->scratch->len);
		skip_space(ps);
		if (ps->p >= ps->end || *ps->p != ':') {
			ps->problem = "an object member has no colon";
			goto fail;
		}
		ps->p++;
		if (!read_value(ps, depth + 1, &member.value))
			goto fail;
		g_array_append_val(members, member);
		skip_space(ps);
		if (ps->p >= ps->end) {
			ps->problem = "an object is not closed";
			goto fail;
		}
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			break;
		}
		ps->problem = "malformed object";
		goto fail;
	}

done:
	out->type = AXIS_JSON_OBJECT;
	out->count = members->len;
	if (members->len > 0) {
		out->u.members = arena_alloc(ps->doc, members->len * sizeof(AxisJsonMember));
		memcpy(out->u.members, members->data, members->len * sizeof(AxisJsonMember));
	}
	g_array_free(members, TRUE);
	return TRUE;

fail:
	g_array_free(members, TRUE);
	return FALSE;
}

static gboolean
read_value(
	Parser       *ps,
	guint         depth,
	AxisJsonNode *out
){
	if (depth > AXIS_JSON_MAX_DEPTH) {
		ps->problem = "nested too deeply";
		return FALSE;
	}
	if (++ps->nodes > AXIS_JSON_MAX_NODES) {
		ps->problem = "too many values";
		return FALSE;
	}
	skip_space(ps);
	if (ps->p >= ps->end) {
		ps->problem = "the text ends where a value should be";
		return FALSE;
	}

	switch (*ps->p) {
	case '{':
		ps->p++;
		return read_object(ps, depth, out);
	case '[':
		ps->p++;
		return read_array(ps, depth, out);
	case '"':
		ps->p++;
		if (!read_string(ps))
			return FALSE;
		out->type = AXIS_JSON_STRING;
		out->u.string = arena_string(ps->doc, ps->scratch->str, ps->scratch->len);
		return TRUE;
	case 't':
		return read_literal(ps, "true", AXIS_JSON_BOOLEAN, 1, out);
	case 'f':
		return read_literal(ps, "false", AXIS_JSON_BOOLEAN, 0, out);
	case 'n':
		return read_literal(ps, "null", AXIS_JSON_NULL, 0, out);
	default:
		return read_number(ps, out);
	}
}

AxisJsonDoc *
axis_json_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
){
	AxisJsonDoc *doc;
	Parser ps;

	if (problem != NULL)
		*problem = NULL;

	doc = g_new0(AxisJsonDoc, 1);
	doc->blocks = g_ptr_array_new_with_free_func(g_free);

	memset(&ps, 0, sizeof(ps));
	ps.p = (const guchar *)data;
	ps.end = (const guchar *)data + length;
	ps.doc = doc;
	ps.scratch = g_string_sized_new(256);

	if (!read_value(&ps, 0, &doc->root)) {
		if (problem != NULL)
			*problem = g_strdup(ps.problem != NULL ? ps.problem : "malformed JSON");
		goto fail;
	}
	skip_space(&ps);
	if (ps.p != ps.end) {
		if (problem != NULL)
			*problem = g_strdup("text follows the JSON value");
		goto fail;
	}

	g_string_free(ps.scratch, TRUE);
	return doc;

fail:
	g_string_free(ps.scratch, TRUE);
	axis_json_doc_free(doc);
	return NULL;
}

/* ── Looking ────────────────────────────────────────────────────── */

AxisJsonType
axis_json_type(const AxisJsonNode *node)
{
	return node != NULL ? (AxisJsonType)node->type : AXIS_JSON_NULL;
}

guint
axis_json_length(const AxisJsonNode *node)
{
	if (node == NULL || (node->type != AXIS_JSON_ARRAY && node->type != AXIS_JSON_OBJECT))
		return 0;
	return node->count;
}

const AxisJsonNode *
axis_json_element(
	const AxisJsonNode *node,
	guint               index
){
	if (node == NULL || node->type != AXIS_JSON_ARRAY || index >= node->count)
		return NULL;
	return &node->u.items[index];
}

const gchar *
axis_json_member_name(
	const AxisJsonNode *node,
	guint               index
){
	if (node == NULL || node->type != AXIS_JSON_OBJECT || index >= node->count)
		return NULL;
	return node->u.members[index].key;
}

const AxisJsonNode *
axis_json_member_value(
	const AxisJsonNode *node,
	guint               index
){
	if (node == NULL || node->type != AXIS_JSON_OBJECT || index >= node->count)
		return NULL;
	return &node->u.members[index].value;
}

const AxisJsonNode *
axis_json_member(
	const AxisJsonNode *node,
	const gchar        *name
){
	guint i;

	if (node == NULL || node->type != AXIS_JSON_OBJECT)
		return NULL;
	for (i = 0; i < node->count; i++) {
		if (strcmp(node->u.members[i].key, name) == 0)
			return &node->u.members[i].value;
	}
	return NULL;
}

const gchar *
axis_json_string(const AxisJsonNode *node)
{
	if (node == NULL || node->type != AXIS_JSON_STRING)
		return NULL;
	return node->u.string;
}

const gchar *
axis_json_scalar_text(const AxisJsonNode *node)
{
	if (node == NULL || (node->type != AXIS_JSON_STRING && node->type != AXIS_JSON_NUMBER))
		return NULL;
	return node->u.string;
}

/* ── Values ─────────────────────────────────────────────────────── */

/* Optional minus sign and 1 to 18 digits. */
static gboolean
parse_whole(
	const gchar *text,
	gint64      *value
){
	const gchar *p = text;
	gboolean negative = FALSE;
	gint64 result = 0;
	guint digits = 0;

	if (*p == '-') {
		negative = TRUE;
		p++;
	}
	for (; *p >= '0' && *p <= '9'; p++) {
		if (++digits > 18)
			return FALSE;
		result = result * 10 + (*p - '0');
	}
	if (digits == 0 || *p != '\0')
		return FALSE;
	*value = negative ? -result : result;
	return TRUE;
}

/* Digits, sign, point and exponent only: no "inf", "nan" or hex. */
static gboolean
numeric_characters(const gchar *text)
{
	const gchar *p;

	if (text[0] == '\0')
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' ||
		      *p == 'E'))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_json_get_double(
	const AxisJsonNode *node,
	gdouble             minimum,
	gdouble             maximum,
	gdouble            *value
){
	const gchar *text = axis_json_scalar_text(node);
	gchar *end = NULL;
	gdouble result;

	if (text == NULL || strlen(text) > AXIS_JSON_MAX_NUMBER || !numeric_characters(text))
		return FALSE;
	result = g_ascii_strtod(text, &end);
	if (end == NULL || *end != '\0' || !isfinite(result) || result < minimum || result > maximum)
		return FALSE;
	*value = result;
	return TRUE;
}

gboolean
axis_json_get_integer(
	const AxisJsonNode *node,
	gint64              minimum,
	gint64              maximum,
	gint64             *value
){
	const gchar *text = axis_json_scalar_text(node);
	gint64 result;
	gdouble real;

	if (text == NULL)
		return FALSE;
	if (parse_whole(text, &result)) {
		if (result < minimum || result > maximum)
			return FALSE;
		*value = result;
		return TRUE;
	}
	/* "25.0" and "1e3" are whole numbers too, if small enough to be exact. */
	if (axis_json_get_double(node, -9007199254740992.0, 9007199254740992.0, &real) &&
	    real == (gdouble)(gint64)real) {
		result = (gint64)real;
		if (result < minimum || result > maximum)
			return FALSE;
		*value = result;
		return TRUE;
	}
	return FALSE;
}

gboolean
axis_json_get_boolean(
	const AxisJsonNode *node,
	gboolean           *value
){
	const gchar *text;

	if (node == NULL)
		return FALSE;
	if (node->type == AXIS_JSON_BOOLEAN) {
		*value = node->count != 0;
		return TRUE;
	}
	text = axis_json_scalar_text(node);
	if (text == NULL)
		return FALSE;
	if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
		*value = TRUE;
		return TRUE;
	}
	if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
		*value = FALSE;
		return TRUE;
	}
	return FALSE;
}
