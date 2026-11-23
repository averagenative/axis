/*
 * axis-web-analysis.h - the analysis module: its jobs, its settings and the
 * Analysis pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_analysis_get_module() after the Library pages' module
 * (it depends on the storage, library, probe and library-ui modules, the
 * last because every figure links to its file list), passing the
 * AxisWebAnalysis as its user data.  The module "analysis" runs:
 *
 *   analysis.refresh (queue "maintenance", one attempt), computing the
 *   reports into a snapshot; analysis.due (queue "maintenance") every five
 *   minutes, from the schedule of the same name, which enqueues it once
 *   scans and probes have settled.
 *
 * Pages (every logged-in user may look; the refresh is an administrator's,
 * and the POST needs the CSRF token; the thresholds and times are settings,
 * edited under Settings by administrators):
 *
 *   GET  /analysis             overview: totals, space by volume and
 *                              category, growth over time, what could not be
 *                              seen
 *   GET  /analysis/space       space by volume, root, category and file kind
 *   GET  /analysis/composition video codec, resolution, HDR, container,
 *                              audio codec; bit rate by resolution class;
 *                              files large for their class
 *   GET  /analysis/largest     the largest files and folders
 *   GET  /analysis/duplicates  likely duplicates, by content and by title
 *   GET  /analysis/split       series and albums on more than one volume
 *   GET  /analysis/leftovers   samples, extras, orphaned subtitles, junk
 *   GET  /analysis/status      the refreshing state (htmx; it refreshes
 *                              the page when the refresh ended)
 *   POST /analysis/refresh     asks for a refresh now
 *
 * Every figure on these pages is a link to the file list
 * (/library/files?...) of the files it counts, built from the same
 * conditions.  Nothing from a file name is ever part of a URL or an
 * attribute; names are shown as text inside <bdi>.  Charts are inline SVG
 * built with the element builder: geometry in attributes, colour in the
 * stylesheet, a link per row, and always a table beside them.
 */

#ifndef AXIS_WEB_ANALYSIS_H
#define AXIS_WEB_ANALYSIS_H

#include <glib.h>

#include "core/axis-module.h"
#include "library/axis-analysis.h"

G_BEGIN_DECLS

typedef struct _AxisWebAnalysis AxisWebAnalysis;

/* Create on the main thread; @analysis is referenced. */
AxisWebAnalysis *
axis_web_analysis_new(AxisAnalysis *analysis);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_analysis_stop(AxisWebAnalysis *self);

void
axis_web_analysis_free(AxisWebAnalysis *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebAnalysis, axis_web_analysis_free)

const AxisModuleInfo *
axis_web_analysis_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_ANALYSIS_H */
