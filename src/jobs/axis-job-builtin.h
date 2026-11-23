/*
 * axis-job-builtin.h - the job queue's own module: retention
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The "jobs" module registers one job kind and its default schedule:
 *
 *   jobs.prune (queue "maintenance"), every hour: deletes succeeded and
 *   cancelled jobs that finished more than 7 days ago and failed jobs that
 *   finished more than 30 days ago.  The ages are the settings
 *   "jobs.keep_finished" and "jobs.keep_failed" (1 day to 10 years); the
 *   payload may override them with "keep_finished_days" and
 *   "keep_failed_days" (whole days, 1 to 3650).  Pass the settings store
 *   (core/axis-settings.h) as the module's user data; without it the
 *   defaults above apply.
 *
 * The module is required: it cannot be switched off.
 */

#ifndef AXIS_JOB_BUILTIN_H
#define AXIS_JOB_BUILTIN_H

#include <glib.h>

#include "core/axis-module.h"

G_BEGIN_DECLS

#define AXIS_JOB_BUILTIN_MODULE_ID      "jobs"
#define AXIS_JOB_PRUNE_KIND             "jobs.prune"
#define AXIS_JOB_PRUNE_SCHEDULE         "jobs.prune"
#define AXIS_JOB_MAINTENANCE_QUEUE      "maintenance"
#define AXIS_JOB_PRUNE_INTERVAL_SECONDS 3600
#define AXIS_JOB_PRUNE_MAX_DAYS         3650
#define AXIS_JOB_SETTING_KEEP_FINISHED  "jobs.keep_finished"
#define AXIS_JOB_SETTING_KEEP_FAILED    "jobs.keep_failed"

/**
 * axis_job_builtin_get_module:
 *
 * The module to register (with %NULL user data) so that the queue prunes
 * its own history.
 *
 * Returns: (transfer none): the module description, in static storage
 */
const AxisModuleInfo *
axis_job_builtin_get_module(void);

G_END_DECLS

#endif /* AXIS_JOB_BUILTIN_H */
