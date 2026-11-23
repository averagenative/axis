/*
 * axis-web-conversion-runs.h - the pages of the conversion runs, under
 * Media management -> Conversions, and their piece of the dashboard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pages (administrators only; every POST needs the CSRF token):
 *
 *   GET  /conversions/runs                  every run, newest first
 *   GET  /conversions/runs/new              planning a run: its scope and
 *                                           profile revision
 *   POST /conversions/runs                  makes it (its list is made by a
 *                                           job)
 *   GET  /conversions/runs/:id              a run: where it is and why, its
 *                                           list (a page of units), what is not
 *                                           in it, what became of its files;
 *                                           the review and start while ready
 *   GET  /conversions/runs/:id/status       the "where it is" block (polled)
 *   GET  /conversions/runs/:id/units/:pos   a movie or series of the list
 *                                           with its files
 *   GET  /conversions/runs/:id/left-out     the files not in the list for a
 *                                           reason (?reason=)
 *   POST /conversions/runs/:id/start        ready -> running (trials, or
 *                                           replacing with the box ticked)
 *   POST /conversions/runs/:id/pause, /resume, /cancel
 *   POST /conversions/runs/:id/replace      trials -> replacing originals
 *   POST /conversions/runs/:id/limits       at once, caps, hours
 *   POST /conversions/runs/:id/units/:pos/remove
 *   POST /conversions/runs/:id/entries/:entry/remove
 */

#ifndef AXIS_WEB_CONVERSION_RUNS_H
#define AXIS_WEB_CONVERSION_RUNS_H

#include <glib.h>
#include <htmx-glib.h>

#include "transcode/axis-conversion-run.h"
#include "web/axis-web.h"

G_BEGIN_DECLS

typedef struct _AxisWebRuns AxisWebRuns;

AxisWebRuns *
axis_web_runs_new(AxisConversionRuns *runs);

/* The routes and the dashboard's piece (from the conversions module's
 * register_routes). */
void
axis_web_runs_register(
	AxisWebRuns *self,
	AxisWeb     *web
);

/* Waits for the page work in flight. */
void
axis_web_runs_stop(AxisWebRuns *self);

void
axis_web_runs_free(AxisWebRuns *self);

/* The Conversions page's card that leads to the runs. */
HtmxNode *
axis_web_runs_card(void);

G_END_DECLS

#endif /* AXIS_WEB_CONVERSION_RUNS_H */
