/*
 * axis-web-setup-path.h - the setup guide's steps and addresses, and the one
 * rule by which a form shown in the guide comes back to it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The guide is a path through pages that exist.  Each of its steps shows the
 * real form of a page elsewhere (Settings, Storage, Indexers, Download
 * clients, Identify), which posts to that page's own handler.  A form shown
 * in the guide carries a field "return" holding the step's address; the
 * handler, once its work is done, sends the browser there instead of to its
 * own page, with its own fixed "done" word.
 *
 * Only the guide's own addresses are ever followed.  The value a request
 * carries is compared whole with the fixed list below and, when it is one of
 * them, the list's own string is used: an address elsewhere, another of
 * Axis's pages, a path with "..", a query, an encoding or anything added is
 * not one, and the handler goes where it always went.  Nothing of the
 * request reaches a Location header.
 */

#ifndef AXIS_WEB_SETUP_PATH_H
#define AXIS_WEB_SETUP_PATH_H

#include <glib.h>
#include <htmx-glib.h>

#include "acquire/axis-acquire-ready.h"

G_BEGIN_DECLS

/* The guide's overview; each step is below it, at "/setup-guide/<slug>". */
#define AXIS_WEB_SETUP_PATH "/setup-guide"

/* The form field that carries the address to come back to. */
#define AXIS_WEB_SETUP_RETURN_FIELD "return"

/**
 * AxisSetupStep:
 * @AXIS_SETUP_TITLES: the metadata provider's key (TMDb's token)
 * @AXIS_SETUP_LIBRARY: volumes, and library roots by media type
 * @AXIS_SETUP_INDEXERS: an indexer source (Prowlarr)
 * @AXIS_SETUP_CLIENTS: download clients and their path mappings
 * @AXIS_SETUP_QUALITY: the default quality profile
 * @AXIS_SETUP_PLAYERS: Plex and Jellyfin (optional)
 * @AXIS_SETUP_WRITING: storage.read_only off
 * @AXIS_SETUP_EXISTING: identification of the films roots (optional)
 *
 * The steps, in the guide's order.
 */
typedef enum
{
	AXIS_SETUP_TITLES,
	AXIS_SETUP_LIBRARY,
	AXIS_SETUP_INDEXERS,
	AXIS_SETUP_CLIENTS,
	AXIS_SETUP_QUALITY,
	AXIS_SETUP_PLAYERS,
	AXIS_SETUP_WRITING,
	AXIS_SETUP_EXISTING
} AxisSetupStep;

#define AXIS_SETUP_N_STEPS (AXIS_SETUP_EXISTING + 1)

/* A bit per step, for sets of steps. */
#define AXIS_SETUP_BIT(step) (1u << (guint)(step))

/* "titles", as stored and in addresses. */
const gchar *
axis_setup_step_slug(AxisSetupStep step);

/* "/setup-guide/titles" */
const gchar *
axis_setup_step_path(AxisSetupStep step);

/* "Titles and artwork": the step's name wherever it is shown. */
const gchar *
axis_setup_step_label(AxisSetupStep step);

/* Whether Axis can find and download without it (players, recognising). */
gboolean
axis_setup_step_optional(AxisSetupStep step);

gboolean
axis_setup_step_from_slug(
	const gchar   *slug,
	AxisSetupStep *step
);

/* The step where @item of the "what is needed to download" checklist is
 * set up. */
AxisSetupStep
axis_setup_step_of_ready_item(AxisReadyItem item);

/**
 * axis_web_setup_return_path:
 * @value: (nullable): what a request carried
 *
 * Returns: (transfer none) (nullable): the guide's own address @value is,
 *   from the fixed list (static storage), or %NULL when it is not one
 */
const gchar *
axis_web_setup_return_path(const gchar *value);

/* As above, for the form field "return" of @request. */
const gchar *
axis_web_setup_request_return(HtmxRequest *request);

/**
 * axis_web_setup_return_field:
 * @path: (nullable): a value axis_web_setup_return_path() gave
 *
 * Returns: (transfer full) (nullable): the hidden field that brings the form
 *   back to @path, or %NULL for none
 */
HtmxNode *
axis_web_setup_return_field(const gchar *path);

/* "<path>?done=<done>": @path from axis_web_setup_return_path(), @done one
 * of the handler's own fixed words. */
gchar *
axis_web_setup_return_location(
	const gchar *path,
	const gchar *done
);

/**
 * axis_web_setup_back_note:
 * @path: (nullable): where the form came from
 *
 * For a page that shows a form again after refusing it: the line that says
 * saving goes back to the guide, with a link back.  %NULL without @path.
 *
 * Returns: (transfer full) (nullable): a <p class="help">
 */
HtmxNode *
axis_web_setup_back_note(const gchar *path);

G_END_DECLS

#endif /* AXIS_WEB_SETUP_PATH_H */
