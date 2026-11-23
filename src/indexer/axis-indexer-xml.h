/*
 * axis-indexer-xml.h - a small, bounded pull reader for the XML documents an
 * indexer answers with (Newznab and Torznab)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An indexer is a third party, and its answer is read as hostile input.  No
 * general XML library is linked for it: this reader knows exactly what the
 * answers need and refuses everything else.
 *
 * What it reads: elements with attributes (in double or single quotes),
 * text, CDATA sections, comments and processing instructions (both skipped,
 * never acted on), an XML declaration at the very start (whose encoding must
 * be UTF-8 or US-ASCII when it names one), the five predefined entities
 * (&lt; &gt; &amp; &quot; &apos;) and numeric character references (&#65;
 * &#x41;) that name a character XML allows.
 *
 * What it refuses, ending the read: a document type declaration of any kind
 * (so no DTD, no internal or external entity definitions, no external
 * references: "<!DOCTYPE" is an error wherever it is), any other "<!"
 * declaration, an entity reference that is not one of the five, a bare '&',
 * a character XML does not allow (control characters other than tab, line
 * feed and carriage return; U+FFFE, U+FFFF), bytes that are not UTF-8, a
 * '<' inside an attribute value, an attribute named twice on one element, an
 * end tag that does not match, text or a second element outside the root,
 * and a document that ends before its root does (truncated input).  Beyond
 * those, the limits below: the document's size, the depth of nesting, the
 * attributes of one element, the elements of the whole document and the
 * length of a name.
 *
 * Namespaces are not processed: a name is reported as written
 * ("newznab:attr"), and axis_xml_local_name() gives the part after the
 * colon, so that "newznab:attr" and "torznab:attr" are both matched as
 * "attr" whatever prefix an indexer chose.
 *
 * Invalid input ends the read with an error, and the reader says nothing
 * more: a caller keeps what it took from the events only once
 * axis_xml_reader_next() has returned %AXIS_XML_DONE (the Newznab mapping
 * discards everything otherwise).  Error messages are fixed text and never
 * hold anything of the document.
 *
 * GLib only, no I/O: in the fuzz library, read by
 * tests/fuzz/fuzz-indexer-xml.c.
 */

#ifndef AXIS_INDEXER_XML_H
#define AXIS_INDEXER_XML_H

#include <glib.h>

G_BEGIN_DECLS

/* The defaults of AxisXmlLimits. */
#define AXIS_XML_MAX_BYTES       (16 * 1024 * 1024)
#define AXIS_XML_MAX_DEPTH       32
#define AXIS_XML_MAX_ATTRIBUTES  32
#define AXIS_XML_MAX_ELEMENTS    200000
#define AXIS_XML_MAX_NAME_BYTES  128

/**
 * AxisXmlLimits:
 * @max_bytes: the largest document read (0: %AXIS_XML_MAX_BYTES)
 * @max_depth: the deepest nesting of elements (0: %AXIS_XML_MAX_DEPTH)
 * @max_attributes: the most attributes of one element (0: %AXIS_XML_MAX_ATTRIBUTES)
 * @max_elements: the most elements in the document (0: %AXIS_XML_MAX_ELEMENTS)
 */
typedef struct
{
	gsize max_bytes;
	guint max_depth;
	guint max_attributes;
	guint max_elements;
} AxisXmlLimits;

/**
 * AxisXmlEvent:
 * @AXIS_XML_START: an element begins: its name and attributes can be read
 * @AXIS_XML_END: the element of that name ends (an empty element, <a/>,
 *   gives a start and an end)
 * @AXIS_XML_TEXT: a run of text or a CDATA section inside an element, with
 *   its references resolved (a text may come in several runs)
 * @AXIS_XML_DONE: the document ended well; nothing follows
 * @AXIS_XML_ERROR: the document is refused; nothing follows
 */
typedef enum
{
	AXIS_XML_START,
	AXIS_XML_END,
	AXIS_XML_TEXT,
	AXIS_XML_DONE,
	AXIS_XML_ERROR
} AxisXmlEvent;

typedef struct _AxisXmlReader AxisXmlReader;

/**
 * axis_xml_reader_new:
 * @data: the document; borrowed, it must outlive the reader
 * @length: its length in bytes
 * @limits: (nullable): the limits (%NULL: the defaults)
 *
 * Returns: (transfer full): a reader at the start of @data
 */
AxisXmlReader *
axis_xml_reader_new(
	const gchar         *data,
	gsize                length,
	const AxisXmlLimits *limits
);

void
axis_xml_reader_free(AxisXmlReader *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisXmlReader, axis_xml_reader_free)

/* The next event.  After %AXIS_XML_DONE or %AXIS_XML_ERROR, the same again. */
AxisXmlEvent
axis_xml_reader_next(AxisXmlReader *self);

/* For a start or an end: the element's name as written ("newznab:attr"),
 * valid until the next event. */
const gchar *
axis_xml_reader_name(AxisXmlReader *self);

/* The depth of the element a start or an end is of (the root is 1), or of
 * the element a text is in. */
guint
axis_xml_reader_depth(AxisXmlReader *self);

/* For a start: how many attributes it has, and each one's name and value
 * (references resolved); valid until the next event. */
guint
axis_xml_reader_n_attributes(AxisXmlReader *self);

const gchar *
axis_xml_reader_attribute_name(
	AxisXmlReader *self,
	guint          index
);

const gchar *
axis_xml_reader_attribute_value(
	AxisXmlReader *self,
	guint          index
);

/* For a start: the value of the attribute whose local name is @local_name
 * (the first such, prefixed or not), or %NULL. */
const gchar *
axis_xml_reader_attribute(
	AxisXmlReader *self,
	const gchar   *local_name
);

/* For a text: the text (valid UTF-8, NUL-ended) and its length in bytes;
 * valid until the next event. */
const gchar *
axis_xml_reader_text(
	AxisXmlReader *self,
	gsize         *length
);

/* After %AXIS_XML_ERROR: why, a fixed sentence fragment. */
const gchar *
axis_xml_reader_error(AxisXmlReader *self);

/* "attr" for "newznab:attr" and for "attr". */
const gchar *
axis_xml_local_name(const gchar *name);

/**
 * axis_xml_check:
 * @problem: (out) (optional) (transfer none): why not
 *
 * Reads the whole of @data and says whether it is a document this reader
 * accepts.
 */
gboolean
axis_xml_check(
	const gchar         *data,
	gsize                length,
	const AxisXmlLimits *limits,
	const gchar        **problem
);

G_END_DECLS

#endif /* AXIS_INDEXER_XML_H */
