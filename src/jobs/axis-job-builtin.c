/*
 * axis-job-builtin.c - the job queue's own module: retention
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jobs/axis-job-builtin.h"
#include "core/axis-setting-units.h"
#include "core/axis-settings.h"
#include "jobs/axis-job-context.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"

#define MS_PER_DAY (G_GINT64_CONSTANT(24) * 3600 * 1000)

/* How long the history is kept, in milliseconds: the payload's whole
 * number of days when it has one, else @fallback_ms. */
static gboolean
payload_age_ms(
	JsonObject   *payload,
	const gchar  *member,
	gint64        fallback_ms,
	gint64       *age_ms,
	GError      **error
){
	JsonNode *node = json_object_get_member(payload, member);

	if (node == NULL) {
		*age_ms = fallback_ms;
		return TRUE;
	}

	if (JSON_NODE_TYPE(node) != JSON_NODE_VALUE || json_node_get_value_type(node) != G_TYPE_INT64 ||
	    json_node_get_int(node) < 1 || json_node_get_int(node) > AXIS_JOB_PRUNE_MAX_DAYS) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT,
			"%s must be a whole number of days from 1 to %d", member, AXIS_JOB_PRUNE_MAX_DAYS);
		return FALSE;
	}

	*age_ms = json_node_get_int(node) * MS_PER_DAY;
	return TRUE;
}

/*
 * The ages come from the settings "jobs.keep_finished" and
 * "jobs.keep_failed", read once here, at the start of the run, so that one
 * run uses one pair of values; the payload may override them.  Without a
 * settings store (user data %NULL) the built-in defaults apply.
 */
static gboolean
prune_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisSettings *settings = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	AxisDbPool *pool = axis_job_context_get_pool(context);
	AxisDbConn *conn;
	gint64 finished_ms = AXIS_JOB_DEFAULT_KEEP_FINISHED_MS;
	gint64 failed_ms = AXIS_JOB_DEFAULT_KEEP_FAILED_MS;
	guint64 deleted = 0;
	gboolean ok;

	if (settings != NULL) {
		finished_ms = axis_settings_get_duration(settings, AXIS_JOB_SETTING_KEEP_FINISHED) * 1000;
		failed_ms = axis_settings_get_duration(settings, AXIS_JOB_SETTING_KEEP_FAILED) * 1000;
	}
	if (!payload_age_ms(payload, "keep_finished_days", finished_ms, &finished_ms, error) ||
	    !payload_age_ms(payload, "keep_failed_days", failed_ms, &failed_ms, error))
		return FALSE;

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_prune(conn, finished_ms, failed_ms, &deleted, error);
	axis_db_pool_release(pool, conn);
	if (!ok)
		return FALSE;

	if (deleted > 0) {
		g_autofree gchar *finished = axis_setting_format_duration(finished_ms / 1000);
		g_autofree gchar *failed = axis_setting_format_duration(failed_ms / 1000);

		g_info("pruned %" G_GUINT64_FORMAT " finished job(s) (succeeded or cancelled over %s ago, "
			"failed over %s ago)", deleted, finished, failed);
	}
	return TRUE;
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_JOB_PRUNE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 3, prune_handler,
	  "Deleting old jobs from the Jobs page", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_JOB_PRUNE_SCHEDULE, AXIS_JOB_PRUNE_KIND, NULL, AXIS_JOB_PRUNE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisSettingInfo setting_table[] = {
	{
		AXIS_JOB_SETTING_KEEP_FINISHED, AXIS_SETTING_DURATION,
		"Keep succeeded and cancelled jobs for",
		"How long succeeded and cancelled jobs stay in the job list before the hourly "
		"clean-up deletes them.",
		0, "7d",
		24 * 3600, (gint64)AXIS_JOB_PRUNE_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_JOB_SETTING_KEEP_FAILED, AXIS_SETTING_DURATION,
		"Keep failed jobs for",
		"How long failed jobs stay in the job list, with their error, before the hourly "
		"clean-up deletes them.",
		0, "30d",
		24 * 3600, (gint64)AXIS_JOB_PRUNE_MAX_DAYS * 24 * 3600, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static const AxisModuleInfo module = {
	AXIS_JOB_BUILTIN_MODULE_ID,
	"Job queue",
	NULL,
	NULL,
	handlers,
	schedules,
	NULL,
	NULL,
	"Runs background work and keeps the job history tidy.",
	setting_table,
	AXIS_MODULE_REQUIRED,
	NULL,
	NULL
};

const AxisModuleInfo *
axis_job_builtin_get_module(void)
{
	return &module;
}
