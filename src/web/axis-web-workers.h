/*
 * axis-web-workers.h - the Conversion workers page: the workers, what each
 * reported, what each is doing; creating one (its token shown once),
 * revoking one, asking one to measure its encoders again, and asking for a
 * sample measurement on a worker
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every route is for administrators only (403 for anyone else).
 */

#ifndef AXIS_WEB_WORKERS_H
#define AXIS_WEB_WORKERS_H

#include <glib.h>

#include "api/axis-worker-tokens.h"
#include "core/axis-module.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-registry.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-estimator.h"

G_BEGIN_DECLS

typedef struct _AxisWebWorkers AxisWebWorkers;

/**
 * axis_web_workers_new:
 * @pool: the database; must outlive the result
 * @tokens: the workers' tokens (borrowed)
 * @estimator: (nullable): for a sample measurement on a worker
 * @heartbeat_ms: what the workers are told, to tell a connected worker
 *   from a silent one
 */
AxisWebWorkers *
axis_web_workers_new(
	AxisDbPool       *pool,
	AxisWorkerTokens *tokens,
	AxisEstimator    *estimator,
	guint             heartbeat_ms
);

/* Waits for the pages' work in flight; runs the default main context. */
void
axis_web_workers_stop(AxisWebWorkers *self);

/**
 * axis_web_workers_set_kinds:
 * @kinds: (nullable): the job registry, so that a worker's jobs are named
 *   in plain words (their kind beside, as a detail); referenced
 *
 * Main thread, before serving.  Without it a job is named by its kind.
 */
void
axis_web_workers_set_kinds(
	AxisWebWorkers  *self,
	AxisJobRegistry *kinds
);

/**
 * axis_web_workers_set_conversions:
 * @conversions: (nullable): the conversions (referenced), so that the page
 *   says what workers do where convert.checks_where says, and shows that
 *   conversion work is held after a restart
 *
 * Main thread, before serving.  Without it the page speaks of checks on the
 * server.
 */
void
axis_web_workers_set_conversions(
	AxisWebWorkers  *self,
	AxisConversions *conversions
);

void
axis_web_workers_free(AxisWebWorkers *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebWorkers, axis_web_workers_free)

/* The module ("workers", required): the page and its navigation entry.
 * Its user data is an AxisWebWorkers. */
const AxisModuleInfo *
axis_web_workers_get_module(void);

G_END_DECLS

#endif /* AXIS_WEB_WORKERS_H */
