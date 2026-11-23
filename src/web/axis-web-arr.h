/*
 * axis-web-arr.h - the pages of "Bring over from Radarr": the connections,
 * the reading and the preview
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/arr-migration/spec.md.  Every route is an
 * administrator's (anyone else gets the house's 403) and every post carries
 * the CSRF token.
 *
 *   GET  /migrate              the connections, and the form to add one
 *   POST /migrate              add a connection
 *   GET  /migrate/:id          one connection: its state, its reading, its
 *                                 settings, forgetting it
 *   POST /migrate/:id          change it
 *   POST /migrate/:id/test     "Test the connection"
 *   POST /migrate/:id/read     "Read what Radarr has" (the job arr.read)
 *   GET  /migrate/:id/status   the reading's state, as a fragment (polled
 *                                 while a reading runs)
 *   POST /migrate/:id/forget   forget the connection, its key and plans
 *   GET  /migrate/:id/preview  what a run would bring
 *   POST /migrate/:id/preview  change the mapping tables and options
 *
 * The API key is never put back in a page: its field is a password field
 * without a value, typed into only to replace the stored key.  Nothing an
 * arr app answered is shown but the fields Axis reads from its answers, as
 * text; an error is fixed words (arr/axis-arr.h).
 *
 * These pages write nothing to the library.
 */

#ifndef AXIS_WEB_ARR_H
#define AXIS_WEB_ARR_H

#include <glib.h>

#include "arr/axis-arr.h"
#include "core/axis-module.h"

G_BEGIN_DECLS

typedef struct _AxisWebArr AxisWebArr;

/* @arr is referenced. */
AxisWebArr *
axis_web_arr_new(AxisArr *arr);

/* Waits for the pages' work in flight (main loop only). */
void
axis_web_arr_stop(AxisWebArr *self);

void
axis_web_arr_free(AxisWebArr *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebArr, axis_web_arr_free)

/* The module: its jobs (arr.read, arr.cleanup), its pages and its place in
 * the navigation.  Register it with the AxisWebArr as its user data. */
const AxisModuleInfo *
axis_web_arr_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_ARR_H */
