/*
 * axis-web-compat.h - the compat module: its jobs, its settings and the
 * Compatibility pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Register axis_web_compat_get_module() after the Savings module (it
 * depends on the storage, library, probe and estimate modules: it matches
 * sessions to indexed files and takes the estimated savings from the
 * Savings snapshots), passing the AxisWebCompat as its user data.  The
 * module "compat" runs:
 *
 *   compat.poll (queue "compat"), every 30 s, which looks at the sessions
 *   in progress when the adaptive interval says it is due;
 *   compat.history (queue "compat"), hourly, which imports the play history
 *   at first connection and then daily.
 *
 * Pages (all administrator-only: they show who plays what, on which
 * device; every POST needs the CSRF token and is written to the audit log):
 *
 *   GET  /compat                     the overview: connection, evidence
 *                                    coverage, verdicts at a glance
 *   GET  /compat/devices             the devices
 *   GET  /compat/devices/:id         one device's capability table
 *   GET  /compat/verdicts            the verdict per target and library, with
 *                                    the devices behind it, and the
 *                                    recommendation
 *   GET  /compat/failures            what the server converts on play today
 *   GET  /compat/mapping             libraries, the path map, suggestions
 *   POST /compat/test                the connection test
 *   POST /compat/poll                look at the sessions now
 *   POST /compat/history             import the history now
 *   POST /compat/mapping/add         add a path map entry
 *   POST /compat/mapping/:id/remove  remove one
 *   POST /compat/devices/:id/pin     always consider (or no longer)
 *
 * Names from the media server (devices, libraries) are text content,
 * escaped, never part of a link or an attribute: links carry ids.  The
 * token is never rendered: it is a secret setting, shown on no page.
 *
 * The Savings pages (every logged-in user) show the verdicts and the
 * recommendation as counts only: no device, no account.
 */

#ifndef AXIS_WEB_COMPAT_H
#define AXIS_WEB_COMPAT_H

#include <glib.h>
#include <htmx-glib.h>

#include "compat/axis-compat-report.h"
#include "core/axis-module.h"

G_BEGIN_DECLS

typedef struct _AxisWebCompat AxisWebCompat;

/* Create on the main thread; @compat is referenced. */
AxisWebCompat *
axis_web_compat_new(AxisCompat *compat);

/* Waits (running the default main context) for the page work in flight. */
void
axis_web_compat_stop(AxisWebCompat *self);

void
axis_web_compat_free(AxisWebCompat *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebCompat, axis_web_compat_free)

const AxisModuleInfo *
axis_web_compat_get_module(void);

/* ── For the Savings pages (axis-web-compat-savings.c) ───────────── */

/* The cell beside a target on the overview: how many libraries it is safe,
 * risky or unknown for; counts only. */
HtmxNode *
axis_web_compat_target_cell(
	const AxisCompatReport *report,
	const gchar            *target_id
);

/* "Recommended per library": counts only, no device or account. @admin
 * adds the link to the Compatibility pages. */
HtmxNode *
axis_web_compat_recommendation_card(
	const AxisCompatReport *report,
	gboolean                admin
);

/* A target's verdict in each library it applies to; counts only. */
HtmxNode *
axis_web_compat_target_card(
	const AxisCompatReport *report,
	const gchar            *target_id,
	gboolean                admin
);

G_END_DECLS

#endif /* AXIS_WEB_COMPAT_H */
