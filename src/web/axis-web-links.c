/*
 * axis-web-links.c - the pieces the modules' links pages share
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-links.h"

#include <string.h>

#include "web/axis-web-storage-private.h"

gboolean
axis_web_links_field_file(
	const gchar *key,
	const gchar *prefix,
	gint64      *file_id
){
	guint64 value = 0;

	if (!g_str_has_prefix(key, prefix) ||
	    !g_ascii_string_to_unsigned(key + strlen(prefix), 10, 1, G_MAXINT64, &value, NULL))
		return FALSE;
	*file_id = (gint64)value;
	return TRUE;
}

gboolean
axis_web_links_digest_valid(const gchar *digest)
{
	gsize i;

	if (digest == NULL || strlen(digest) != 32)
		return FALSE;
	for (i = 0; i < 32; i++) {
		if (!g_ascii_isxdigit(digest[i]) || g_ascii_isupper(digest[i]))
			return FALSE;
	}
	return TRUE;
}

HtmxNode *
axis_web_links_tick_box(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	axis_web_storage_add(field, box);
	axis_web_storage_add(field, text);
	return HTMX_NODE(field);
}
