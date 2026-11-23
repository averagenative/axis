/*
 * axis-web-library.h - the library module: scan jobs, their schedule, and
 * each root's page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_library_get_module() after the storage module (it
 * depends on it), passing the AxisWebLibrary as its user data.  The module
 * "library" runs:
 *
 *   library.scan (queue "scan", AXIS_LIBRARY_SCAN_ATTEMPTS attempts), one
 *   job per volume at a time, scanning its roots that are requested or due;
 *   library.scan-due (queue "maintenance") every minute, from the schedule
 *   of the same name, which enqueues those jobs.
 *
 * Scans of different volumes run in parallel up to the "scan" queue's
 * concurrency (jobs.concurrency in the configuration; 1 unless set).
 *
 * Pages (every logged-in user may look; changes are for administrators
 * and every POST needs the CSRF token):
 *
 *   GET  /storage/roots/:id             a root: its last scan, what it found,
 *                                       the index's counts, recent scans
 *   POST /storage/roots/:id/scan        requests a scan now
 *   POST /storage/roots/:id/settings    sets the interval and ignore patterns
 *
 * Every name from a filesystem is shown in its display form
 * (axis_library_name_display()) as text, inside <bdi>, never in an
 * attribute or a link.
 */

#ifndef AXIS_WEB_LIBRARY_H
#define AXIS_WEB_LIBRARY_H

#include <glib.h>

#include "core/axis-module.h"
#include "library/axis-library.h"

G_BEGIN_DECLS

typedef struct _AxisWebLibrary AxisWebLibrary;

/* Create on the main thread; @library and @storage are referenced. */
AxisWebLibrary *
axis_web_library_new(
	AxisLibrary *library,
	AxisStorage *storage
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_library_stop(AxisWebLibrary *self);

void
axis_web_library_free(AxisWebLibrary *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebLibrary, axis_web_library_free)

const AxisModuleInfo *
axis_web_library_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_LIBRARY_H */
