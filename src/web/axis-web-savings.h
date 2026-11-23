/*
 * axis-web-savings.h - the estimate module: its jobs, its settings and the
 * Savings pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_savings_get_module() after the Analysis module (it
 * depends on the storage, library, probe, Library pages and analysis
 * modules: every figure links to a file list, and its refresh follows the
 * Analysis refresh), passing the AxisWebSavings as its user data.  The
 * module "estimate" runs:
 *
 *   estimate.refresh (queue "maintenance", one attempt), computing every
 *   target's figures into a snapshot; estimate.due (queue "maintenance")
 *   every minute, which enqueues it after an Analysis refresh, a new
 *   calibration sample or a change of the settings; estimate.sample (queue
 *   "sample", one at a time, lowest priority), the sample measurement an
 *   administrator asks for.
 *
 * Pages (every logged-in user may look; refresh, measurement and its
 * cancellation are an administrator's, and each POST needs the CSRF token):
 *
 *   GET  /savings                  every target side by side
 *   GET  /savings/target/:id       one target: totals, projected free space
 *                                  per volume, breakdowns, skip reasons,
 *                                  the files with the largest saving
 *   GET  /savings/calibration      calibration per target, the measurement
 *   GET  /savings/status           the refreshing state (htmx)
 *   GET  /savings/measure/status   the measurement's progress (htmx)
 *   POST /savings/refresh          asks for a refresh now
 *   POST /savings/measure          starts a measurement (field "target")
 *   POST /savings/measure/cancel   cancels it
 *
 * Every figure that counts files links to the file list
 * (/library/files?target=...&outcome=...), which the estimator decides
 * again with the same functions.  Names from the library are text content,
 * never part of a link or an attribute: links carry ids and fixed names.
 */

#ifndef AXIS_WEB_SAVINGS_H
#define AXIS_WEB_SAVINGS_H

#include <glib.h>
#include <htmx-glib.h>

#include "compat/axis-compat.h"
#include "core/axis-module.h"
#include "transcode/axis-estimator.h"

G_BEGIN_DECLS

typedef struct _AxisWebSavings AxisWebSavings;

/* Create on the main thread; @estimator is referenced. */
AxisWebSavings *
axis_web_savings_new(AxisEstimator *estimator);

/**
 * axis_web_savings_set_compat:
 * @compat: (nullable): the client compatibility report; referenced
 *
 * Makes the overview and the target pages show each target's verdict per
 * library and the recommendation per library (while the "compat" module is
 * on): counts only, for every logged-in user, no device and no account.
 * Call it before the pages are served.
 */
void
axis_web_savings_set_compat(
	AxisWebSavings *self,
	AxisCompat     *compat
);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_savings_stop(AxisWebSavings *self);

void
axis_web_savings_free(AxisWebSavings *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebSavings, axis_web_savings_free)

const AxisModuleInfo *
axis_web_savings_get_module(void);

/* The file page's "If converted" card for @estimates (AxisFileEstimate),
 * for the probe pages (axis-web-probe.c). */
HtmxNode *
axis_web_savings_file_card(GArray *estimates);

G_END_DECLS

#endif /* AXIS_WEB_SAVINGS_H */
