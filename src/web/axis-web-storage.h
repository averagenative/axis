/*
 * axis-web-storage.h - the storage module: volumes and roots pages, the
 * scheduled presence check and the read-only setting
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_storage_get_module() with the module registry, passing
 * the AxisWebStorage as the module's user data.  The module is called
 * "storage", declares "storage.read_only" (axis-storage.h), runs the job
 * kind "storage.check" on the maintenance queue from a schedule every
 * AXIS_STORAGE_CHECK_INTERVAL_SECONDS, and checks every volume once when
 * it starts.
 *
 * Pages (every logged-in user may look; changes are for administrators
 * and every POST needs the CSRF token):
 *
 *   GET  /storage                      the volumes, with state and space,
 *                                      live; the form to add one
 *   GET  /storage/list                 the list alone, for a refresh
 *   POST /storage/volumes/inspect      looks at a directory and shows what
 *                                      adding it would do: the confirmation
 *   POST /storage/volumes              adds it, exactly as confirmed
 *   GET  /storage/volumes/:id          one volume, its roots and actions
 *   POST /storage/volumes/:id/check    checks it now
 *   POST /storage/volumes/:id/path     points it at a new path
 *   GET  /storage/volumes/:id/remove   the confirmation; POST removes it
 *   GET  /storage/volumes/:id/marker   the confirmation; POST restores a
 *                                      deleted marker
 *   GET  /storage/roots                every root; the form to add one
 *   POST /storage/roots                adds a root
 *   GET  /storage/roots/:id/remove     the confirmation; POST removes it
 *
 * Nothing on a disk is written by any of these except the marker, after
 * the confirmation step.  Work that touches a disk or the database runs on
 * a worker thread; pages are built on the main loop.  Every change is
 * logged as an audit line with the acting user.
 *
 * The forms to add a volume and a root, shown in the setup guide (the
 * functions below, given a return path), post to these same handlers; the
 * confirmation page carries the return path on, and the guide is where the
 * browser goes once the volume or root is added (web/axis-web-setup-path.h).
 */

#ifndef AXIS_WEB_STORAGE_H
#define AXIS_WEB_STORAGE_H

#include <glib.h>

#include "core/axis-module.h"
#include "storage/axis-storage.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebStorage AxisWebStorage;

/* The topic the volume pages follow (every logged-in user). */
#define AXIS_STORAGE_TOPIC "storage"

/**
 * axis_web_storage_new:
 * @storage: (transfer none): the volumes; a reference is kept
 * @hub: (nullable): where live updates go; must outlive the result
 *
 * Create on the main thread.
 */
AxisWebStorage *
axis_web_storage_new(
	AxisStorage *storage,
	AxisWebHub  *hub
);

/**
 * axis_web_storage_stop:
 *
 * Stops listening and waits (running the default main context) for the
 * work in flight, each of which is bounded by the storage's deadlines.
 * Call after the server has stopped taking requests and before the pool
 * is freed.
 */
void
axis_web_storage_stop(AxisWebStorage *self);

/* Stops @self if needed, and frees it. */
void
axis_web_storage_free(AxisWebStorage *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebStorage, axis_web_storage_free)

/**
 * axis_web_storage_get_module:
 *
 * Returns: (transfer none): the module description, in static storage
 */
const AxisModuleInfo *
axis_web_storage_get_module(void);

/* ── The forms, wherever they are shown ────────────────────────── */

/**
 * axis_web_storage_add_volume_form:
 * @path_value, @name_value: (nullable): what was typed, shown again
 * @return_path: (nullable): an address of the setup guide to come back to
 *
 * The Storage page's "Add a volume" form: it posts the directory to look at,
 * and the next page (the confirmation) shows what adding it will do.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_storage_add_volume_form(
	HtmxRequest *request,
	const gchar *path_value,
	const gchar *name_value,
	const gchar *return_path
);

/**
 * axis_web_storage_add_root_form:
 * @volumes: (element-type AxisVolumeInfo): the volumes (those not online
 *   cannot be chosen)
 *
 * The Library roots page's "Add a root" form.
 *
 * Returns: (transfer full): the form
 */
HtmxNode *
axis_web_storage_add_root_form(
	HtmxRequest *request,
	GPtrArray   *volumes,
	const gchar *return_path
);

G_END_DECLS

#endif /* AXIS_WEB_STORAGE_H */
