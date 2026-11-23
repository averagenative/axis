/*
 * fuzz-indexer-xml.c - libFuzzer target for the bounded XML reader of the
 * indexers' answers (indexer/axis-indexer-xml.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the limits ('0' the defaults; '1' small ones: 512
 * bytes, depth 4, 3 attributes, 8 elements); the rest is the document.
 * Whatever it is, the reader returns without crashing, reading past the
 * input or allocating without bound, and what it gives holds together:
 *
 *  - every start is ended once, by an end of the same name, at the same
 *    depth, in order; the depth never passes the limit, and no more
 *    elements start than the limit allows;
 *  - names, attribute names and values, and texts are valid UTF-8, a
 *    name is never empty, and no element has two attributes of one name;
 *  - a text comes only inside an element;
 *  - the read ends with done or an error and then says the same again; done
 *    means every element was ended, there was a root, no error is given,
 *    and axis_xml_check() accepts the same document; an error always says
 *    why.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/axis-indexer-xml.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	AxisXmlLimits limits;
	AxisXmlReader *reader;
	GPtrArray *open;
	AxisXmlEvent event;
	const gchar *text;
	gsize length;
	guint starts = 0;

	if (size < 1 || size > 1 << 20)
		return 0;
	memset(&limits, 0, sizeof(limits));
	if (data[0] == '1') {
		limits.max_bytes = 512;
		limits.max_depth = 4;
		limits.max_attributes = 3;
		limits.max_elements = 8;
	}
	text = (const gchar *)data + 1;
	length = size - 1;
	reader = axis_xml_reader_new(text, length, &limits);
	open = g_ptr_array_new_with_free_func(g_free);
	while ((event = axis_xml_reader_next(reader)) != AXIS_XML_DONE && event != AXIS_XML_ERROR) {
		const gchar *name = axis_xml_reader_name(reader);
		guint depth = axis_xml_reader_depth(reader);
		guint i;
		guint j;

		switch (event) {
		case AXIS_XML_START:
			starts++;
			if (name == NULL || name[0] == '\0' || !g_utf8_validate(name, -1, NULL))
				abort();
			g_ptr_array_add(open, g_strdup(name));
			if (depth != open->len || depth > (limits.max_depth != 0 ? limits.max_depth : AXIS_XML_MAX_DEPTH))
				abort();
			if (starts > (limits.max_elements != 0 ? limits.max_elements : AXIS_XML_MAX_ELEMENTS))
				abort();
			for (i = 0; i < axis_xml_reader_n_attributes(reader); i++) {
				const gchar *a = axis_xml_reader_attribute_name(reader, i);
				const gchar *v = axis_xml_reader_attribute_value(reader, i);

				if (a == NULL || a[0] == '\0' || v == NULL || !g_utf8_validate(a, -1, NULL) ||
				    !g_utf8_validate(v, -1, NULL))
					abort();
				for (j = 0; j < i; j++) {
					if (strcmp(axis_xml_reader_attribute_name(reader, j), a) == 0)
						abort();
				}
			}
			if (axis_xml_reader_n_attributes(reader) > (limits.max_attributes != 0 ? limits.max_attributes :
			    AXIS_XML_MAX_ATTRIBUTES))
				abort();
			(void)axis_xml_reader_attribute(reader, "value");
			(void)axis_xml_local_name(name);
			break;
		case AXIS_XML_END:
			if (open->len == 0 || name == NULL || strcmp(g_ptr_array_index(open, open->len - 1), name) != 0 ||
			    depth != open->len)
				abort();
			g_ptr_array_remove_index(open, open->len - 1);
			break;
		default: {
			gsize n = 0;
			const gchar *t = axis_xml_reader_text(reader, &n);

			if (open->len == 0 || t == NULL || strlen(t) != n || !g_utf8_validate(t, (gssize)n, NULL))
				abort();
			break;
		}
		}
	}
	/* The end is the end. */
	if (axis_xml_reader_next(reader) != event)
		abort();
	if (event == AXIS_XML_DONE) {
		if (open->len != 0 || starts == 0 || axis_xml_reader_error(reader) != NULL)
			abort();
		if (!axis_xml_check(text, length, &limits, NULL))
			abort();
	} else if (axis_xml_reader_error(reader) == NULL) {
		abort();
	}
	g_ptr_array_unref(open);
	axis_xml_reader_free(reader);
	return 0;
}
