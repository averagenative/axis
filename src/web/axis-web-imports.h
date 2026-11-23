/*
 * axis-web-imports.h - the import module: its jobs, its settings, the
 * Imports page (Activity), an import's page with its actions, the manual
 * import, the Naming page with its preview, and the dashboard's items
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /imports                   waiting, blocked, and recent imports with
 *                                   how each was placed
 *   GET  /imports/:id               one import: every file found with its
 *                                   decision and reasons, the actions
 *   POST /imports/:id/act           an action: import_anyway, choose_file
 *                                   (position), assign (owner), dismiss,
 *                                   retry (admin)
 *   GET  /imports/manual            the completed downloads and the folders
 *                                   of the downloads roots (admin)
 *   GET  /imports/manual/inspect    what Axis reads from one, and the film to
 *                                   choose (?download=N or ?root=N&path=P;
 *                                   admin)
 *   POST /imports/manual            import it for the film chosen (admin)
 *   GET  /imports/naming            the naming settings with a preview of
 *                                   three sample films (admin)
 *   GET  /imports/naming/preview    the preview alone, for what is typed
 *                                   (htmx; admin)
 *   POST /imports/naming            save them (admin)
 *
 * Every action is an administrator's POST with the CSRF token, recorded
 * with the user by the import layer.
 */

#ifndef AXIS_WEB_IMPORTS_H
#define AXIS_WEB_IMPORTS_H

#include "core/axis-module.h"
#include "import/axis-import.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebImports AxisWebImports;

/* @settings: where the Naming page saves (not owned; outlives the module) */
AxisWebImports *
axis_web_imports_new(
	AxisImport   *import,
	AxisSettings *settings
);

/* Waits for the page work in flight (main loop). */
void
axis_web_imports_stop(AxisWebImports *self);

void
axis_web_imports_free(AxisWebImports *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebImports, axis_web_imports_free)

const AxisModuleInfo *
axis_web_imports_get_module(void);

/* The warnings of axis_import_volume_warnings() as a block for a page
 * (%NULL when there are none): the Download clients and Library roots pages
 * show it. */
HtmxNode *
axis_web_imports_volume_warnings(const GPtrArray *warnings);

G_END_DECLS

#endif /* AXIS_WEB_IMPORTS_H */
