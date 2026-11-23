/*
 * axis-web-probe.h - the media probe module: its jobs, settings and pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_probe_get_module() after the library module (it
 * depends on it), passing the AxisWebProbe as its user data.  The module
 * "probe" runs:
 *
 *   probe.file (queue "probe", AXIS_PROBE_FILE_ATTEMPTS attempts), one job
 *   per file, probing it (src/probe/axis-probe.h);
 *   probe.due (queue "maintenance") every minute and probe.stale every ten
 *   minutes, from the schedules of the same names, which enqueue those jobs
 *   for new and changed files and for files recorded with an older probe
 *   version.
 *
 * Probes of different volumes run in parallel up to the "probe" queue's
 * concurrency (jobs.concurrency in the configuration; 1 unless set).
 *
 * Pages (every logged-in user may look; changes are for administrators and
 * every POST needs the CSRF token):
 *
 *   GET  /library/file/:id          a media file: its container, streams,
 *                                   HDR class, probe status and error, and
 *                                   (axis_web_probe_set_estimator()) what
 *                                   each conversion target would make of it
 *   POST /library/file/:id/probe    probes it again
 *   GET  /storage/roots/:id/probe   a root's probe results: counts, the
 *                                   files whose probe failed, the latest
 *   POST /storage/roots/:id/probe   probes the failed files of the root again
 *
 * Everything that comes from a file (tags, titles, language codes, error
 * texts) is shown as text content (which htmx-glib escapes), cleaned
 * before it was stored; names of files only through
 * axis_library_name_display(), inside <bdi>, never in an attribute or a
 * link.
 */

#ifndef AXIS_WEB_PROBE_H
#define AXIS_WEB_PROBE_H

#include <glib.h>
#include <htmx-glib.h>

#include "core/axis-module.h"
#include "probe/axis-probe.h"
#include "transcode/axis-estimator.h"

G_BEGIN_DECLS

typedef struct _AxisWebProbe AxisWebProbe;

/* Create on the main thread; @probe and @storage are referenced. */
AxisWebProbe *
axis_web_probe_new(
	AxisProbe   *probe,
	AxisStorage *storage
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_probe_stop(AxisWebProbe *self);

/* Gives the file page its "If converted" card (shown while the estimate
 * module is on); main thread, before serving.  @estimator is referenced;
 * %NULL removes it. */
void
axis_web_probe_set_estimator(
	AxisWebProbe  *self,
	AxisEstimator *estimator
);

/**
 * AxisWebFileCardCollect:
 * @kind: the file's kind ("video", "audio", ...)
 *
 * Reads what another module's card on the file page shows, on the page's
 * worker thread.
 */
typedef gpointer (*AxisWebFileCardCollect)(
	gint64       file_id,
	const gchar *kind,
	gpointer     user_data
);

/* Draws that card on the main loop (%NULL for none). */
typedef HtmxNode *(*AxisWebFileCardRender)(
	HtmxRequest *request,
	gint64       file_id,
	gpointer     figures,
	gpointer     user_data
);

/* One more card on the file page (the conversions' "Convert this file"),
 * after the others; main thread, before serving.  @user_data must outlive
 * the page work (axis_web_probe_stop()). */
void
axis_web_probe_set_file_card(
	AxisWebProbe           *self,
	AxisWebFileCardCollect  collect,
	AxisWebFileCardRender   render,
	GDestroyNotify          free_figures,
	gpointer                user_data
);

void
axis_web_probe_free(AxisWebProbe *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebProbe, axis_web_probe_free)

const AxisModuleInfo *
axis_web_probe_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_PROBE_H */
