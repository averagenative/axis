/*
 * axis-web-setup-path.c - the setup guide's steps and addresses
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-setup-path.h"

#include <string.h>

static const struct
{
	const gchar *slug;
	const gchar *path;
	const gchar *label;
	gboolean     optional;
} steps[AXIS_SETUP_N_STEPS] = {
	[AXIS_SETUP_TITLES] = { "titles", AXIS_WEB_SETUP_PATH "/titles", "Details from TMDb", FALSE },
	[AXIS_SETUP_LIBRARY] = { "library", AXIS_WEB_SETUP_PATH "/library", "Disks and library folders", FALSE },
	[AXIS_SETUP_INDEXERS] = { "indexers", AXIS_WEB_SETUP_PATH "/indexers", "Indexers", FALSE },
	[AXIS_SETUP_CLIENTS] = { "clients", AXIS_WEB_SETUP_PATH "/clients", "Download clients", FALSE },
	[AXIS_SETUP_QUALITY] = { "quality", AXIS_WEB_SETUP_PATH "/quality", "What to download", FALSE },
	[AXIS_SETUP_PLAYERS] = { "players", AXIS_WEB_SETUP_PATH "/players", "Media servers (Plex, Jellyfin)", TRUE },
	[AXIS_SETUP_WRITING] = { "writing", AXIS_WEB_SETUP_PATH "/writing", "Letting Axis write", FALSE },
	[AXIS_SETUP_EXISTING] = { "existing", AXIS_WEB_SETUP_PATH "/existing", "Identifying the movies already on disk",
		TRUE }
};

const gchar *
axis_setup_step_slug(AxisSetupStep step)
{
	g_return_val_if_fail((guint)step < AXIS_SETUP_N_STEPS, "");

	return steps[step].slug;
}

const gchar *
axis_setup_step_path(AxisSetupStep step)
{
	g_return_val_if_fail((guint)step < AXIS_SETUP_N_STEPS, AXIS_WEB_SETUP_PATH);

	return steps[step].path;
}

const gchar *
axis_setup_step_label(AxisSetupStep step)
{
	g_return_val_if_fail((guint)step < AXIS_SETUP_N_STEPS, "");

	return steps[step].label;
}

gboolean
axis_setup_step_optional(AxisSetupStep step)
{
	g_return_val_if_fail((guint)step < AXIS_SETUP_N_STEPS, FALSE);

	return steps[step].optional;
}

gboolean
axis_setup_step_from_slug(
	const gchar   *slug,
	AxisSetupStep *step
){
	guint i;

	for (i = 0; slug != NULL && i < AXIS_SETUP_N_STEPS; i++) {
		if (strcmp(slug, steps[i].slug) == 0) {
			*step = (AxisSetupStep)i;
			return TRUE;
		}
	}
	return FALSE;
}

AxisSetupStep
axis_setup_step_of_ready_item(AxisReadyItem item)
{
	switch (item) {
	case AXIS_READY_METADATA:
		return AXIS_SETUP_TITLES;
	case AXIS_READY_INDEXER:
		return AXIS_SETUP_INDEXERS;
	case AXIS_READY_CLIENT:
		return AXIS_SETUP_CLIENTS;
	case AXIS_READY_FILMS_ROOT:
	case AXIS_READY_SERIES_ROOT:
		return AXIS_SETUP_LIBRARY;
	default:
		return AXIS_SETUP_WRITING;
	}
}

/* ── Coming back ────────────────────────────────────────────────── */

const gchar *
axis_web_setup_return_path(const gchar *value)
{
	guint i;

	if (value == NULL)
		return NULL;
	if (strcmp(value, AXIS_WEB_SETUP_PATH) == 0)
		return AXIS_WEB_SETUP_PATH;
	for (i = 0; i < AXIS_SETUP_N_STEPS; i++) {
		if (strcmp(value, steps[i].path) == 0)
			return steps[i].path;
	}
	return NULL;
}

const gchar *
axis_web_setup_request_return(HtmxRequest *request)
{
	return axis_web_setup_return_path(htmx_request_get_form_value(request, AXIS_WEB_SETUP_RETURN_FIELD));
}

HtmxNode *
axis_web_setup_return_field(const gchar *path)
{
	const gchar *known = axis_web_setup_return_path(path);

	if (known == NULL)
		return NULL;
	return HTMX_NODE(htmx_input_new_hidden(AXIS_WEB_SETUP_RETURN_FIELD, known));
}

gchar *
axis_web_setup_return_location(
	const gchar *path,
	const gchar *done
){
	const gchar *known = axis_web_setup_return_path(path);

	g_return_val_if_fail(known != NULL, g_strdup(AXIS_WEB_SETUP_PATH));

	if (done == NULL || done[0] == '\0')
		return g_strdup(known);
	return g_strdup_printf("%s?done=%s", known, done);
}

HtmxNode *
axis_web_setup_back_note(const gchar *path)
{
	const gchar *known = axis_web_setup_return_path(path);
	HtmxP *note;
	HtmxSpan *text;
	HtmxA *link;

	if (known == NULL)
		return NULL;
	note = htmx_p_new();
	text = htmx_span_new();
	link = htmx_a_new_with_href(known);
	htmx_element_add_class(HTMX_ELEMENT(note), "help");
	htmx_element_add_class(HTMX_ELEMENT(note), "setup-back");
	htmx_node_set_text_content(HTMX_NODE(text), "This form came from the setup guide: once it is done, you go back "
		"there. ");
	htmx_node_set_text_content(HTMX_NODE(link), "Back to the setup guide");
	htmx_node_add_child(HTMX_NODE(note), HTMX_NODE(text));
	g_object_unref(text);
	htmx_node_add_child(HTMX_NODE(note), HTMX_NODE(link));
	g_object_unref(link);
	return HTMX_NODE(note);
}
