/*
 * axis-web-notice.c - a page's notice drawn where the person was working
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-notice.h"

#include "web/axis-web-auth-private.h"
#include "web/axis-web-place.h"

static void
add(
	HtmxNode *parent,
	HtmxNode *child
){
	htmx_node_add_child(parent, child);
	g_object_unref(child);
}

/* The notice itself; one placed in an element says so (its own class). */
static HtmxNode *
notice_node(
	const AxisWebNotice *notice,
	gboolean             here
){
	HtmxNode *node = axis_web_auth_notice(notice->error, notice->text);

	if (here)
		htmx_element_add_class(HTMX_ELEMENT(node), "notice-here");
	return node;
}

void
axis_web_notice_begin(
	AxisWebNotice *notice,
	HtmxRequest   *request,
	gboolean       error,
	gchar         *text,
	HtmxNode      *content
){
	notice->place = axis_web_place_of(request);
	notice->text = text;
	notice->error = error;
	notice->slot = NULL;
	notice->drawn = FALSE;
	if (text == NULL)
		return;
	if (notice->place == NULL) {
		add(content, notice_node(notice, FALSE));
		notice->drawn = TRUE;
		return;
	}
	notice->slot = HTMX_NODE(htmx_div_new());
	htmx_element_add_class(HTMX_ELEMENT(notice->slot), "notice-slot");
	/* The content holds it from here; the pointer is borrowed. */
	add(content, notice->slot);
}

gboolean
axis_web_notice_is_at(
	const AxisWebNotice *notice,
	const gchar         *id
){
	return notice->text != NULL && !notice->drawn && notice->place != NULL && g_strcmp0(notice->place, id) == 0;
}

void
axis_web_notice_put(
	AxisWebNotice *notice,
	HtmxNode      *element,
	const gchar   *id
){
	HtmxNode *node = axis_web_notice_take(notice, id);

	if (node != NULL)
		add(element, node);
}

HtmxNode *
axis_web_notice_take(
	AxisWebNotice *notice,
	const gchar   *id
){
	if (!axis_web_notice_is_at(notice, id))
		return NULL;
	notice->drawn = TRUE;
	return notice_node(notice, TRUE);
}

void
axis_web_notice_end(AxisWebNotice *notice)
{
	if (notice->text != NULL && !notice->drawn && notice->slot != NULL)
		add(notice->slot, notice_node(notice, FALSE));
	notice->drawn = TRUE;
	notice->slot = NULL;
	g_clear_pointer(&notice->text, g_free);
}
