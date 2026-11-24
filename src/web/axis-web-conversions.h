/*
 * axis-web-conversions.h - the conversions module: its jobs, settings and
 * pages (Media management -> Conversions), and the file page's "Convert this
 * file" card
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pages (administrators only; every POST needs the CSRF token):
 *
 *   GET  /conversions                 every conversion, the revisions tried
 *                                     with their passed trials, the work
 *                                     areas' leftovers
 *   GET  /conversions/new             the confirmation of a request
 *                                     (?file=&revision=&outcome=): what will
 *                                     happen, every gate worked out now
 *   POST /conversions                 the request (every gate again)
 *   GET  /conversions/latest          the table of the latest conversions, as
 *                                     the page asks for it again while one is
 *                                     in flight
 *   GET  /conversions/:id             a conversion: its checks with what each
 *                                     found, its placement, its files
 *   GET  /conversions/:id/progress    the block that follows a conversion in
 *                                     flight (its stage, a bar, the time left)
 *   POST /conversions/:id/cancel      stops one that is queued or encoding
 *   POST /conversions/:id/remove-trial removes a trial's kept file now
 *   GET  /conversions/prove/:revision the confirmation of proving a revision
 *                                     by hand, naming the trials
 *   POST /conversions/prove/:revision the proof
 *   POST /conversions/resume          resumes conversion work held since the
 *                                     daemon started (convert.wait_after_start)
 */

#ifndef AXIS_WEB_CONVERSIONS_H
#define AXIS_WEB_CONVERSIONS_H

#include <glib.h>
#include <htmx-glib.h>

#include "api/axis-worker-api.h"
#include "core/axis-module.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-conversion-run.h"

G_BEGIN_DECLS

typedef struct _AxisWebConversions AxisWebConversions;

/* @conversions is referenced.  Create on the main thread.  It makes the
 * conversion runs too (their jobs and pages are the module's:
 * web/axis-web-conversion-runs.h). */
AxisWebConversions *
axis_web_conversions_new(AxisConversions *conversions);

/* The conversion runs (transfer none). */
AxisConversionRuns *
axis_web_conversions_get_runs(AxisWebConversions *self);

/* What a replacement's confirmation says of the original, with
 * convert.keep_original at @seconds. */
gchar *
axis_web_conversions_keep_sentence(gint64 seconds);

/* The sentence of the choice to convert kept files, the same on a run's plan,
 * a run's page and a single conversion's confirmation. */
#define AXIS_WEB_CONVERT_KEPT_LABEL "Also convert files Axis keeps"
#define AXIS_WEB_CONVERT_KEPT_HELP \
	"A kept file is re-encoded in place after every check passes. It stays kept, so Axis still does not replace it " \
	"with another release."

/* Where a conversion in flight is, in words: "Encoding: 42%, 2.5 times real
 * time, about 12 minutes left"; "Checking: no progress reported yet". */
gchar *
axis_web_conversions_progress_text(const AxisConversionProgress *progress);

/* The same as a bar (a <progress> element: max 100 and the percent as its
 * value, or no value while nothing is known, which draws it indeterminate)
 * with the words beside it and as the bar's accessible name. */
HtmxNode *
axis_web_conversions_progress_node(const AxisConversionProgress *progress);

/* A bar of @value of @max with @text beside it and as its accessible name. */
HtmxNode *
axis_web_conversions_bar(
	gdouble      value,
	gdouble      max,
	const gchar *text
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_conversions_stop(AxisWebConversions *self);

void
axis_web_conversions_free(AxisWebConversions *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebConversions, axis_web_conversions_free)

const AxisModuleInfo *
axis_web_conversions_get_module(void);

/* The file page's card (web/axis-web-probe.h's file card hooks): what can
 * be asked for a video file, and its conversions.  The collect function
 * runs on a worker thread. */
gpointer
axis_web_conversions_file_collect(
	gint64       file_id,
	const gchar *kind,
	gpointer     user_data
);

HtmxNode *
axis_web_conversions_file_render(
	HtmxRequest *request,
	gint64       file_id,
	gpointer     figures,
	gpointer     user_data
);

void
axis_web_conversions_file_free(gpointer figures);

/* The worker API's hooks for convert.encode and convert.verify (their data:
 * the AxisConversions). */
const AxisWorkerConvertHooks *
axis_web_conversions_worker_hooks(void);

/* While conversion work is held after a restart: a notice saying so, since
 * when and why, with the one button that resumes it; %NULL otherwise.  Main
 * loop (the Workers page shows it too). */
HtmxNode *
axis_web_conversions_paused_notice(
	HtmxRequest     *request,
	AxisConversions *conversions
);

/* The player check's verdict (AxisConversionVerdictFunc), from @compat (an
 * AxisCompat, nullable): worked out now; unknown while the compatibility
 * module is off. */
AxisCompatVerdict
axis_web_conversions_compat_verdict(
	gpointer     compat,
	gint64       root_id,
	const gchar *slug
);

G_END_DECLS

#endif /* AXIS_WEB_CONVERSIONS_H */
