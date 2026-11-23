/*
 * axis-web-identify.h - the identification module: its jobs, Media
 * management -> Identify, a unit's page, and its part of a title's page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /identify                      every films root, with its figures
 *   GET  /identify/roots/:id            one root: figures, review lists, links,
 *                                       adding every identified film
 *                                       (?list=likely|unknown|linked|not_film
 *                                       and &from= for one whole list)
 *   GET  /identify/units/:id            one unit: evidence, candidates, files,
 *                                       history; ?q= and &year= search
 *   POST /identify/units/:id/act        confirm, choose (title=), not_film, undo
 *   POST /identify/roots/:id/run        look at the root again now
 *   POST /identify/roots/:id/add        manage every identified film (profile=,
 *                                       monitored=yes)
 *
 * Any logged-in user sees the pages; every action is an administrator's
 * POST (AXIS_WEB_ROUTE_ADMIN, the CSRF token), recorded with the user, and
 * the pages offer no form to anyone else.  Everything that came from disk (a
 * path, a name) is shown through its stored form as text, and everything
 * from a provider as text.
 */

#ifndef AXIS_WEB_IDENTIFY_H
#define AXIS_WEB_IDENTIFY_H

#include <glib.h>

#include "core/axis-module.h"
#include "identify/axis-identify.h"
#include "meta/axis-meta.h"
#include "quality/axis-quality-store.h"
#include "web/axis-web-browser.h"

G_BEGIN_DECLS

typedef struct _AxisWebIdentify AxisWebIdentify;

/* @identify and @quality are referenced. */
AxisWebIdentify *
axis_web_identify_new(
	AxisIdentify     *identify,
	AxisQualityStore *quality
);

/* For axis_module_registry_register() with the module as user data. */
const AxisModuleInfo *
axis_web_identify_get_module(void);

/* Waits for the work in flight (main loop). */
void
axis_web_identify_stop(AxisWebIdentify *self);

/**
 * axis_web_identify_folder_titles:
 * @user_data: the #AxisIdentify (not owned)
 *
 * An #AxisWebFolderTitlesFunc for the films roots
 * (axis_web_browser_set_folder_titles()): a film folder that is a unit is
 * listed with the film it is linked to, or as not identified (or not a
 * film) with a link to its unit.  Blocks: a worker thread.
 */
gboolean
axis_web_identify_folder_titles(
	gpointer    user_data,
	gint64      root_id,
	GPtrArray  *dirs,
	GPtrArray  *titles,
	GError    **error
);

void
axis_web_identify_free(AxisWebIdentify *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebIdentify, axis_web_identify_free)

/**
 * axis_web_identify_run_form:
 * @root_id: a films root
 * @return_path: (nullable): an address of the setup guide to come back to
 *
 * A root's "Look again now" button, as its page has it: it queues a run of
 * identification of the root (POST /identify/roots/@root_id/run).
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_identify_run_form(
	HtmxRequest *request,
	gint64       root_id,
	const gchar *return_path
);

G_END_DECLS

#endif /* AXIS_WEB_IDENTIFY_H */
