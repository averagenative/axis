/*
 * axis-indexer-xml.c - the bounded pull reader for indexers' XML answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One pass over the bytes, with a cursor; no recursion (the open elements
 * are a stack bounded by the depth limit), no allocation that grows with
 * anything but the document (bounded by the size limit).  The whole
 * document is checked to be UTF-8 without the characters XML forbids before
 * the first event, so every name, value and text the reader hands out is
 * valid text.
 */

#include "indexer/axis-indexer-xml.h"

#include <string.h>

typedef struct
{
	gchar *name;
	gchar *value;
} Attribute;

struct _AxisXmlReader
{
	const gchar   *data;
	gsize          length;
	gsize          pos;
	AxisXmlLimits  limits;
	gboolean       checked;	/* the bytes were looked at */
	gboolean       finished;	/* done or failed: nothing more */
	AxisXmlEvent   last;
	const gchar   *error;
	GPtrArray     *stack;	/* gchar *: the open elements */
	gboolean       root_seen;	/* the root element began */
	gboolean       root_closed;
	gboolean       pending_end;	/* an empty element's end is next */
	guint          n_elements;
	gchar         *name;	/* the current start's or end's */
	guint          depth;
	GArray        *attributes;	/* Attribute */
	GString       *text;
};

/* ── Errors ─────────────────────────────────────────────────────── */

static AxisXmlEvent
fail(
	AxisXmlReader *self,
	const gchar   *why
){
	self->finished = TRUE;
	self->error = why;
	self->last = AXIS_XML_ERROR;
	return AXIS_XML_ERROR;
}

/* ── Characters ─────────────────────────────────────────────────── */

/* Whether the whole document is UTF-8 without a character XML forbids:
 * the C0 controls but tab, line feed and carriage return, U+FFFE and
 * U+FFFF (surrogates are not UTF-8, and g_utf8_validate() refuses them). */
static gboolean
bytes_ok(
	const gchar *data,
	gsize        length
){
	const guchar *p = (const guchar *)data;
	gsize i;

	if (!g_utf8_validate(data, (gssize)length, NULL))
		return FALSE;
	for (i = 0; i < length; i++) {
		if (p[i] < 0x20 && p[i] != '\t' && p[i] != '\n' && p[i] != '\r')
			return FALSE;
		if (p[i] == 0xef && i + 2 < length && p[i + 1] == 0xbf && (p[i + 2] == 0xbe || p[i + 2] == 0xbf))
			return FALSE;
	}
	return TRUE;
}

static gboolean
is_space(gchar c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static gboolean
name_start(guchar c)
{
	return g_ascii_isalpha(c) || c == '_' || c == ':' || c >= 0x80;
}

static gboolean
name_char(guchar c)
{
	return name_start(c) || g_ascii_isdigit(c) || c == '-' || c == '.';
}

/* A character XML allows (as a numeric reference). */
static gboolean
char_allowed(gunichar c)
{
	return c == 0x9 || c == 0xa || c == 0xd || (c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd) ||
		(c >= 0x10000 && c <= 0x10ffff);
}

static gboolean
at(
	AxisXmlReader *self,
	const gchar   *text
){
	gsize n = strlen(text);

	return self->pos + n <= self->length && memcmp(self->data + self->pos, text, n) == 0;
}

/* The position of @text at or after @from, or -1. */
static gssize
find(
	AxisXmlReader *self,
	gsize          from,
	const gchar   *text
){
	gsize n = strlen(text);
	gsize i;

	for (i = from; i + n <= self->length; i++) {
		if (self->data[i] == text[0] && memcmp(self->data + i, text, n) == 0)
			return (gssize)i;
	}
	return -1;
}

static void
skip_space(AxisXmlReader *self)
{
	while (self->pos < self->length && is_space(self->data[self->pos]))
		self->pos++;
}

/* A name at the cursor, moved past it; %NULL (cursor not moved) when there
 * is none or it is too long. */
static gchar *
read_name(AxisXmlReader *self)
{
	gsize start = self->pos;
	gsize end = start;

	if (end >= self->length || !name_start((guchar)self->data[end]))
		return NULL;
	while (end < self->length && name_char((guchar)self->data[end]))
		end++;
	if (end - start > AXIS_XML_MAX_NAME_BYTES)
		return NULL;
	self->pos = end;
	return g_strndup(self->data + start, end - start);
}

/*
 * Appends @data[from, to) to @out with its references resolved.  Returns
 * %NULL, or why it is refused.  @attribute: a '<' is refused (in text the
 * caller stops at it).
 */
static const gchar *
decode(
	const gchar *data,
	gsize        from,
	gsize        to,
	gboolean     attribute,
	GString     *out
){
	gsize i = from;

	while (i < to) {
		gchar c = data[i];
		gsize end;

		if (c == '<' && attribute)
			return "an attribute value holds a '<'";
		if (c != '&') {
			g_string_append_c(out, c);
			i++;
			continue;
		}
		for (end = i + 1; end < to && end < i + 12 && data[end] != ';'; end++)
			;
		if (end >= to || data[end] != ';')
			return "a '&' does not begin a reference this reader knows";
		if (data[i + 1] == '#') {
			gunichar value = 0;
			gsize p = i + 2;
			gboolean hex = p < end && data[p] == 'x';

			if (hex)
				p++;
			if (p >= end)
				return "a character reference has no number";
			for (; p < end; p++) {
				gint digit = hex ? g_ascii_xdigit_value(data[p]) : g_ascii_digit_value(data[p]);

				if (digit < 0)
					return "a character reference is not a number";
				value = value * (hex ? 16 : 10) + (gunichar)digit;
				if (value > 0x10ffff)
					return "a character reference is out of range";
			}
			if (!char_allowed(value))
				return "a character reference names a character XML does not allow";
			g_string_append_unichar(out, value);
		} else {
			static const struct
			{
				const gchar *name;
				gchar        value;
			} predefined[] = { { "lt", '<' }, { "gt", '>' }, { "amp", '&' }, { "quot", '"' }, { "apos", '\'' } };
			gsize n = end - i - 1;
			guint k;
			gboolean known = FALSE;

			for (k = 0; k < G_N_ELEMENTS(predefined) && !known; k++) {
				if (strlen(predefined[k].name) == n && memcmp(data + i + 1, predefined[k].name, n) == 0) {
					g_string_append_c(out, predefined[k].value);
					known = TRUE;
				}
			}
			if (!known)
				return "an entity reference is not one of the five XML defines (entities are not read)";
		}
		i = end + 1;
	}
	return NULL;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static void
clear_attributes(AxisXmlReader *self)
{
	guint i;

	for (i = 0; i < self->attributes->len; i++) {
		Attribute *a = &g_array_index(self->attributes, Attribute, i);

		g_free(a->name);
		g_free(a->value);
	}
	g_array_set_size(self->attributes, 0);
}

AxisXmlReader *
axis_xml_reader_new(
	const gchar         *data,
	gsize                length,
	const AxisXmlLimits *limits
){
	AxisXmlReader *self = g_new0(AxisXmlReader, 1);

	self->data = data != NULL ? data : "";
	self->length = data != NULL ? length : 0;
	if (limits != NULL)
		self->limits = *limits;
	if (self->limits.max_bytes == 0)
		self->limits.max_bytes = AXIS_XML_MAX_BYTES;
	if (self->limits.max_depth == 0)
		self->limits.max_depth = AXIS_XML_MAX_DEPTH;
	if (self->limits.max_attributes == 0)
		self->limits.max_attributes = AXIS_XML_MAX_ATTRIBUTES;
	if (self->limits.max_elements == 0)
		self->limits.max_elements = AXIS_XML_MAX_ELEMENTS;
	self->stack = g_ptr_array_new_with_free_func(g_free);
	self->attributes = g_array_new(FALSE, TRUE, sizeof(Attribute));
	self->text = g_string_new(NULL);
	return self;
}

void
axis_xml_reader_free(AxisXmlReader *self)
{
	if (self == NULL)
		return;
	clear_attributes(self);
	g_array_unref(self->attributes);
	g_ptr_array_unref(self->stack);
	g_string_free(self->text, TRUE);
	g_free(self->name);
	g_free(self);
}

/* ── The prolog ─────────────────────────────────────────────────── */

/* The XML declaration at the very start: its encoding, when it names one,
 * must be UTF-8 (or US-ASCII, a part of it). */
static const gchar *
read_declaration(AxisXmlReader *self)
{
	gssize end = find(self, self->pos, "?>");
	g_autofree gchar *head = NULL;
	const gchar *encoding;

	if (end < 0 || (gsize)end - self->pos > 512)
		return "the XML declaration does not end";
	head = g_strndup(self->data + self->pos, (gsize)end - self->pos);
	self->pos = (gsize)end + 2;
	encoding = strstr(head, "encoding");
	if (encoding != NULL) {
		const gchar *p = encoding + strlen("encoding");
		gchar quote;
		const gchar *close;
		g_autofree gchar *value = NULL;

		while (is_space(*p))
			p++;
		if (*p++ != '=')
			return "the XML declaration is not readable";
		while (is_space(*p))
			p++;
		quote = *p++;
		if ((quote != '"' && quote != '\'') || (close = strchr(p, quote)) == NULL)
			return "the XML declaration is not readable";
		value = g_strndup(p, (gsize)(close - p));
		if (g_ascii_strcasecmp(value, "utf-8") != 0 && g_ascii_strcasecmp(value, "utf8") != 0 &&
		    g_ascii_strcasecmp(value, "us-ascii") != 0 && g_ascii_strcasecmp(value, "ascii") != 0)
			return "the document is in an encoding other than UTF-8";
	}
	return NULL;
}

/* ── Tags ───────────────────────────────────────────────────────── */

static AxisXmlEvent
read_start(AxisXmlReader *self)
{
	g_autofree gchar *name = NULL;
	const gchar *why;

	self->pos++;	/* '<' */
	name = read_name(self);
	if (name == NULL)
		return fail(self, "an element has no name, or one that is too long");
	if (self->root_closed)
		return fail(self, "there is more than one root element");
	if (self->stack->len >= self->limits.max_depth)
		return fail(self, "the elements are nested too deeply");
	if (++self->n_elements > self->limits.max_elements)
		return fail(self, "the document has too many elements");
	clear_attributes(self);
	while (TRUE) {
		gsize before = self->pos;
		Attribute attribute;
		gchar quote;
		gssize close;
		guint i;

		skip_space(self);
		if (self->pos >= self->length)
			return fail(self, "the document ends inside a tag");
		if (at(self, "/>") || self->data[self->pos] == '>')
			break;
		if (self->pos == before)
			return fail(self, "an attribute is not separated from what is before it");
		memset(&attribute, 0, sizeof(attribute));
		attribute.name = read_name(self);
		if (attribute.name == NULL)
			return fail(self, "an attribute has no name, or one that is too long");
		for (i = 0; i < self->attributes->len; i++) {
			if (strcmp(g_array_index(self->attributes, Attribute, i).name, attribute.name) == 0) {
				g_free(attribute.name);
				return fail(self, "an element has the same attribute twice");
			}
		}
		if (self->attributes->len >= self->limits.max_attributes) {
			g_free(attribute.name);
			return fail(self, "an element has too many attributes");
		}
		g_array_append_val(self->attributes, attribute);
		skip_space(self);
		if (self->pos >= self->length || self->data[self->pos] != '=')
			return fail(self, "an attribute has no value");
		self->pos++;
		skip_space(self);
		if (self->pos >= self->length || (self->data[self->pos] != '"' && self->data[self->pos] != '\''))
			return fail(self, "an attribute's value is not in quotes");
		quote = self->data[self->pos++];
		close = find(self, self->pos, quote == '"' ? "\"" : "'");
		if (close < 0)
			return fail(self, "the document ends inside an attribute value");
		{
			GString *value = g_string_new(NULL);

			why = decode(self->data, self->pos, (gsize)close, TRUE, value);
			g_array_index(self->attributes, Attribute, self->attributes->len - 1).value = g_string_free(value,
				FALSE);
			if (why != NULL)
				return fail(self, why);
		}
		self->pos = (gsize)close + 1;
	}
	if (at(self, "/>")) {
		self->pos += 2;
		self->pending_end = TRUE;
	} else {
		self->pos++;
	}
	g_free(self->name);
	self->name = g_steal_pointer(&name);
	g_ptr_array_add(self->stack, g_strdup(self->name));
	self->depth = self->stack->len;
	self->root_seen = TRUE;
	self->last = AXIS_XML_START;
	return AXIS_XML_START;
}

static AxisXmlEvent
end_of_top(AxisXmlReader *self)
{
	g_free(self->name);
	self->name = g_ptr_array_steal_index(self->stack, self->stack->len - 1);
	self->depth = self->stack->len + 1;
	if (self->stack->len == 0)
		self->root_closed = TRUE;
	clear_attributes(self);
	self->last = AXIS_XML_END;
	return AXIS_XML_END;
}

static AxisXmlEvent
read_end(AxisXmlReader *self)
{
	g_autofree gchar *name = NULL;

	self->pos += 2;	/* "</" */
	name = read_name(self);
	if (name == NULL)
		return fail(self, "an end tag has no name");
	skip_space(self);
	if (self->pos >= self->length || self->data[self->pos] != '>')
		return fail(self, "an end tag does not end");
	self->pos++;
	if (self->stack->len == 0 || strcmp(g_ptr_array_index(self->stack, self->stack->len - 1), name) != 0)
		return fail(self, "an end tag does not match the element it would end");
	return end_of_top(self);
}

/* ── Events ─────────────────────────────────────────────────────── */

AxisXmlEvent
axis_xml_reader_next(AxisXmlReader *self)
{
	g_return_val_if_fail(self != NULL, AXIS_XML_ERROR);

	if (self->finished)
		return self->last;
	if (!self->checked) {
		self->checked = TRUE;
		if (self->length > self->limits.max_bytes)
			return fail(self, "the document is larger than the limit");
		if (!bytes_ok(self->data, self->length))
			return fail(self, "the document is not UTF-8 text, or holds a character XML does not allow");
		if (self->length >= 3 && memcmp(self->data, "\xef\xbb\xbf", 3) == 0)
			self->pos = 3;
		/* Strictly the declaration comes first; a script that printed a line
		 * break before it is forgiven. */
		skip_space(self);
		if (at(self, "<?xml") && self->pos + 5 < self->length && is_space(self->data[self->pos + 5])) {
			const gchar *why = read_declaration(self);

			if (why != NULL)
				return fail(self, why);
		}
	}
	if (self->pending_end) {
		self->pending_end = FALSE;
		return end_of_top(self);
	}
	while (TRUE) {
		gsize start;

		if (self->pos >= self->length) {
			if (self->stack->len > 0)
				return fail(self, "the document ends before its root element does");
			if (!self->root_seen)
				return fail(self, "the document has no root element");
			self->finished = TRUE;
			self->last = AXIS_XML_DONE;
			return AXIS_XML_DONE;
		}
		if (self->data[self->pos] != '<') {
			gssize next = find(self, self->pos, "<");
			gsize to = next >= 0 ? (gsize)next : self->length;
			const gchar *why;
			gsize i;

			start = self->pos;
			self->pos = to;
			if (self->stack->len == 0) {
				for (i = start; i < to; i++) {
					if (!is_space(self->data[i]))
						return fail(self, "there is text outside the root element");
				}
				continue;
			}
			for (i = start; i + 3 <= to; i++) {
				if (self->data[i] == ']' && memcmp(self->data + i, "]]>", 3) == 0)
					return fail(self, "text holds \"]]>\" outside a CDATA section");
			}
			g_string_truncate(self->text, 0);
			why = decode(self->data, start, to, FALSE, self->text);
			if (why != NULL)
				return fail(self, why);
			self->depth = self->stack->len;
			self->last = AXIS_XML_TEXT;
			return AXIS_XML_TEXT;
		}
		if (at(self, "<?")) {
			/* A processing instruction: skipped, never acted on; the
			 * declaration is only at the very start. */
			gssize end = find(self, self->pos + 2, "?>");

			if (end < 0)
				return fail(self, "a processing instruction does not end");
			if (self->pos + 5 <= self->length && g_ascii_strncasecmp(self->data + self->pos + 2, "xml", 3) == 0 &&
			    (self->pos + 5 == (gsize)end || is_space(self->data[self->pos + 5])))
				return fail(self, "an XML declaration is not at the start");
			self->pos = (gsize)end + 2;
			continue;
		}
		if (at(self, "<!--")) {
			gssize end = find(self, self->pos + 4, "-->");

			if (end < 0)
				return fail(self, "a comment does not end");
			self->pos = (gsize)end + 3;
			continue;
		}
		if (at(self, "<![CDATA[")) {
			gssize end = find(self, self->pos + 9, "]]>");

			if (self->stack->len == 0)
				return fail(self, "there is text outside the root element");
			if (end < 0)
				return fail(self, "a CDATA section does not end");
			g_string_truncate(self->text, 0);
			g_string_append_len(self->text, self->data + self->pos + 9, (gssize)((gsize)end - self->pos - 9));
			self->pos = (gsize)end + 3;
			self->depth = self->stack->len;
			self->last = AXIS_XML_TEXT;
			return AXIS_XML_TEXT;
		}
		if (at(self, "<!DOCTYPE") || at(self, "<!doctype"))
			return fail(self, "the document has a document type declaration (entities and external references are "
				"not read)");
		if (at(self, "<!"))
			return fail(self, "the document has a declaration this reader does not read");
		if (at(self, "</"))
			return read_end(self);
		return read_start(self);
	}
}

/* ── What an event holds ────────────────────────────────────────── */

const gchar *
axis_xml_reader_name(AxisXmlReader *self)
{
	return self->last == AXIS_XML_START || self->last == AXIS_XML_END ? self->name : NULL;
}

guint
axis_xml_reader_depth(AxisXmlReader *self)
{
	return self->depth;
}

guint
axis_xml_reader_n_attributes(AxisXmlReader *self)
{
	return self->last == AXIS_XML_START ? self->attributes->len : 0;
}

const gchar *
axis_xml_reader_attribute_name(
	AxisXmlReader *self,
	guint          index
){
	if (self->last != AXIS_XML_START || index >= self->attributes->len)
		return NULL;
	return g_array_index(self->attributes, Attribute, index).name;
}

const gchar *
axis_xml_reader_attribute_value(
	AxisXmlReader *self,
	guint          index
){
	if (self->last != AXIS_XML_START || index >= self->attributes->len)
		return NULL;
	return g_array_index(self->attributes, Attribute, index).value;
}

const gchar *
axis_xml_reader_attribute(
	AxisXmlReader *self,
	const gchar   *local_name
){
	guint i;

	if (self->last != AXIS_XML_START || local_name == NULL)
		return NULL;
	for (i = 0; i < self->attributes->len; i++) {
		const Attribute *a = &g_array_index(self->attributes, Attribute, i);

		if (strcmp(axis_xml_local_name(a->name), local_name) == 0)
			return a->value;
	}
	return NULL;
}

const gchar *
axis_xml_reader_text(
	AxisXmlReader *self,
	gsize         *length
){
	if (self->last != AXIS_XML_TEXT) {
		if (length != NULL)
			*length = 0;
		return NULL;
	}
	if (length != NULL)
		*length = self->text->len;
	return self->text->str;
}

const gchar *
axis_xml_reader_error(AxisXmlReader *self)
{
	return self->error;
}

const gchar *
axis_xml_local_name(const gchar *name)
{
	const gchar *colon;

	if (name == NULL)
		return "";
	colon = strrchr(name, ':');
	return colon != NULL ? colon + 1 : name;
}

gboolean
axis_xml_check(
	const gchar         *data,
	gsize                length,
	const AxisXmlLimits *limits,
	const gchar        **problem
){
	g_autoptr(AxisXmlReader) reader = axis_xml_reader_new(data, length, limits);
	AxisXmlEvent event;

	do
		event = axis_xml_reader_next(reader);
	while (event != AXIS_XML_DONE && event != AXIS_XML_ERROR);
	if (problem != NULL)
		*problem = event == AXIS_XML_ERROR ? axis_xml_reader_error(reader) : NULL;
	return event == AXIS_XML_DONE;
}
