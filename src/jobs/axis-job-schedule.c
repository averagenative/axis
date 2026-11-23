/*
 * axis-job-schedule.c - recurring jobs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jobs/axis-job-schedule.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-private.h"

#include <string.h>

/* A person's schedule, or a person's change to the build's: no longer the
 * build's to keep (declared false). */
static const gchar upsert_sql[] =
	"INSERT INTO job_schedule (name, kind, payload, run_interval, enabled, declared)"
	" VALUES ($1, $2, $3::jsonb, $4::bigint * interval '1 second', $5::boolean, false)"
	" ON CONFLICT (name) DO UPDATE SET kind = EXCLUDED.kind, payload = EXCLUDED.payload,"
	"  run_interval = EXCLUDED.run_interval, enabled = EXCLUDED.enabled, declared = false,"
	"  retired_at = NULL,"
	"  next_run_at = least(job_schedule.next_run_at, now() + EXCLUDED.run_interval),"
	"  updated_at = now()";

/*
 * The build's schedule: made, or brought to what the build declares while it
 * is still the build's (declared).  Enabled again only when the start-up
 * itself had disabled it (retired_at); one switched off any other way stays
 * off.  A shorter interval brings the next run closer; a longer one leaves
 * it.  "was" reads the row as it was before this statement; the upsert
 * returns a row only when it made or changed one.
 */
static const gchar ensure_sql[] =
	"WITH was AS ("
	" SELECT kind, payload::text AS payload, extract(epoch FROM run_interval)::bigint AS seconds, enabled,"
	"  declared, retired_at IS NOT NULL AS retired"
	" FROM job_schedule WHERE name = $1"
	"), up AS ("
	" INSERT INTO job_schedule (name, kind, payload, run_interval, declared)"
	" VALUES ($1, $2, $3::jsonb, $4::bigint * interval '1 second', true)"
	" ON CONFLICT (name) DO UPDATE SET kind = EXCLUDED.kind, payload = EXCLUDED.payload,"
	"  run_interval = EXCLUDED.run_interval,"
	"  enabled = job_schedule.enabled OR job_schedule.retired_at IS NOT NULL, retired_at = NULL,"
	"  next_run_at = least(job_schedule.next_run_at, now() + EXCLUDED.run_interval),"
	"  updated_at = now()"
	" WHERE job_schedule.declared AND (job_schedule.kind <> EXCLUDED.kind"
	"  OR job_schedule.payload <> EXCLUDED.payload OR job_schedule.run_interval <> EXCLUDED.run_interval"
	"  OR job_schedule.retired_at IS NOT NULL)"
	" RETURNING name, enabled"
	")"
	" SELECT (SELECT count(*) FROM up), (SELECT count(*) FROM was),"
	"  was.kind, was.payload, was.seconds, was.enabled, was.declared, was.retired"
	" FROM (SELECT 1) AS one LEFT JOIN was ON true";

/* The schedules the build declared once and declares no more: disabled,
 * marked as the build's doing. */
static const gchar retire_sql[] =
	"UPDATE job_schedule SET enabled = false, retired_at = now(), updated_at = now()"
	" WHERE declared AND enabled AND name <> ALL ($1::text[])"
	" RETURNING name, kind";

static const gchar set_enabled_sql[] =
	"UPDATE job_schedule SET enabled = $2::boolean, declared = false, retired_at = NULL, updated_at = now()"
	" WHERE name = $1";

/*
 * After the database's clock went back: a schedule of these kinds whose
 * next run lies more than its interval ahead (a tick moved it there by a
 * clock that read later) is brought to an interval from now.  "far" reads
 * how far ahead it was; the rows another ticker holds are left to it.
 */
static const gchar pull_back_sql[] =
	"WITH far AS ("
	" SELECT s.name, s.next_run_at AS was FROM job_schedule s"
	" WHERE s.enabled AND s.kind = ANY ($1::text[]) AND s.next_run_at > now() + s.run_interval"
	" FOR UPDATE OF s SKIP LOCKED"
	")"
	" UPDATE job_schedule s SET next_run_at = now() + s.run_interval"
	" FROM far WHERE s.name = far.name"
	" RETURNING s.name, extract(epoch FROM far.was - now())::bigint,"
	"  extract(epoch FROM s.run_interval)::bigint";

/*
 * The tick.  $1, $2, $3 are parallel arrays of the registered kinds, their
 * queues and their attempt counts.  The INSERT's DO NOTHING is what keeps a
 * schedule from overlapping itself (see axis-job-schedule.h); the schedule
 * advances either way.  Every data-modifying part of a WITH runs to
 * completion whether or not the final SELECT reads it.
 */
static const gchar tick_sql[] =
	"WITH known AS ("
	" SELECT * FROM unnest($1::text[], $2::text[], $3::integer[]) AS k (kind, queue, max_attempts)"
	"), due AS ("
	" SELECT s.name, s.kind, s.payload, known.queue, known.max_attempts"
	" FROM job_schedule s JOIN known ON known.kind = s.kind"
	" WHERE s.enabled AND s.next_run_at <= now()"
	" FOR UPDATE OF s SKIP LOCKED"
	"), enq AS ("
	" INSERT INTO job (kind, queue, payload, max_attempts, dedupe_key)"
	" SELECT kind, queue, payload, max_attempts, '" AXIS_JOB_SCHEDULE_DEDUPE_PREFIX "' || name"
	" FROM due"
	" ON CONFLICT (dedupe_key) WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running')"
	" DO NOTHING"
	" RETURNING id, kind, queue, state, attempts, max_attempts, run_at, progress, dedupe_key"
	"), adv AS ("
	" UPDATE job_schedule s SET next_run_at = now() + s.run_interval, last_run_at = now(),"
	"  last_job_id = coalesce(enq.id, s.last_job_id)"
	" FROM due LEFT JOIN enq ON enq.dedupe_key = '" AXIS_JOB_SCHEDULE_DEDUPE_PREFIX "' || due.name"
	" WHERE s.name = due.name"
	" RETURNING s.name"
	")"
	" SELECT enq.id, pg_notify('" AXIS_JOB_CHANNEL "', json_build_object('type', 'state',"
	"  'id', enq.id, 'kind', enq.kind, 'queue', enq.queue, 'state', enq.state,"
	"  'attempts', enq.attempts, 'max_attempts', enq.max_attempts,"
	"  'runnable', enq.run_at <= now(), 'progress', enq.progress, 'message', NULL)::text)"
	" FROM enq";

static gboolean
check_names(
	const gchar  *name,
	const gchar  *kind,
	GError      **error
){
	if (!axis_job_name_is_valid(name)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"invalid schedule name '%s'", name != NULL ? name : "(null)");
		return FALSE;
	}
	if (!axis_job_name_is_valid(kind)) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
			"schedule '%s': invalid job kind name '%s'", name, kind != NULL ? kind : "(null)");
		return FALSE;
	}

	return TRUE;
}

static gboolean
check_interval(
	const gchar  *name,
	guint64       interval_seconds,
	GError      **error
){
	if (interval_seconds >= 1 && interval_seconds <= AXIS_JOB_SCHEDULE_MAX_INTERVAL_SECONDS)
		return TRUE;

	g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID,
		"schedule '%s': the interval must be from 1 to %" G_GUINT64_FORMAT " seconds",
		name, (guint64)AXIS_JOB_SCHEDULE_MAX_INTERVAL_SECONDS);
	return FALSE;
}

gboolean
axis_job_schedule_upsert(
	AxisDbConn   *conn,
	const gchar  *name,
	const gchar  *kind,
	JsonObject   *payload,
	guint64       interval_seconds,
	gboolean      enabled,
	GError      **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *interval = NULL;
	const gchar *params[5];

	g_return_val_if_fail(conn != NULL, FALSE);

	if (!check_names(name, kind, error) || !check_interval(name, interval_seconds, error))
		return FALSE;
	text = axis_job_payload_to_text(payload, error);
	if (text == NULL)
		return FALSE;
	interval = g_strdup_printf("%" G_GUINT64_FORMAT, interval_seconds);

	params[0] = name;
	params[1] = kind;
	params[2] = text;
	params[3] = interval;
	params[4] = enabled ? "true" : "false";
	return axis_db_conn_exec(conn, upsert_sql, G_N_ELEMENTS(params), params, NULL, error);
}

gboolean
axis_job_schedule_ensure(
	AxisDbConn                 *conn,
	const AxisJobScheduleInfo  *info,
	gboolean                   *created,
	GError                    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *interval = NULL;
	const gchar *params[4];
	gint64 changed = 0;
	gint64 existed = 0;

	g_return_val_if_fail(conn != NULL, FALSE);
	g_return_val_if_fail(info != NULL, FALSE);

	if (created != NULL)
		*created = FALSE;
	if (!check_names(info->name, info->kind, error) ||
	    !check_interval(info->name, info->interval_seconds, error))
		return FALSE;
	interval = g_strdup_printf("%u", info->interval_seconds);

	params[0] = info->name;
	params[1] = info->kind;
	params[2] = info->payload != NULL ? info->payload : "{}";
	params[3] = interval;
	if (!axis_db_conn_exec(conn, ensure_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	axis_db_result_get_int64(result, 0, 0, &changed);
	axis_db_result_get_int64(result, 0, 1, &existed);
	if (changed > 0 && existed == 0) {
		g_info("created the default schedule '%s' (%s every %u s)", info->name, info->kind,
			info->interval_seconds);
		if (created != NULL)
			*created = TRUE;
	} else if (existed > 0) {
		gint64 seconds = 0;
		gboolean enabled = TRUE;
		gboolean declared = TRUE;
		gboolean retired = FALSE;

		axis_db_result_get_int64(result, 0, 4, &seconds);
		axis_db_result_get_boolean(result, 0, 5, &enabled);
		axis_db_result_get_boolean(result, 0, 6, &declared);
		axis_db_result_get_boolean(result, 0, 7, &retired);
		if (changed > 0 && retired)
			g_message("schedule '%s' is enabled again: this build declares it (%s every %u s with payload %s), "
				"which an earlier build did not", info->name, info->kind, info->interval_seconds, params[2]);
		else if (changed > 0)
			g_message("schedule '%s' follows this build now: %s every %u s with payload %s (it was %s every %"
				G_GINT64_FORMAT " s with payload %s)%s", info->name, info->kind, info->interval_seconds,
				params[2], axis_db_result_get_text(result, 0, 2), seconds, axis_db_result_get_text(result, 0, 3),
				enabled ? "" : "; it stays disabled, as it was switched off");
		else if (declared && !enabled)
			g_message("schedule '%s' stays disabled: it was switched off, not by a build", info->name);
	}
	return TRUE;
}

gboolean
axis_job_schedule_retire(
	AxisDbConn          *conn,
	const gchar *const  *declared,
	guint               *n_retired,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *names = NULL;
	const gchar *params[1];
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);

	if (n_retired != NULL)
		*n_retired = 0;
	names = axis_job_text_array(declared);
	params[0] = names;
	if (!axis_db_conn_exec(conn, retire_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_message("schedule '%s' (%s) is disabled: no module of this build declares it any more; it is kept, "
			"not deleted, and a build that declares it again enables it again",
			axis_db_result_get_text(result, i, 0), axis_db_result_get_text(result, i, 1));
	if (n_retired != NULL)
		*n_retired = axis_db_result_get_n_rows(result);
	return TRUE;
}

gboolean
axis_job_schedule_set_enabled(
	AxisDbConn   *conn,
	const gchar  *name,
	gboolean      enabled,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];

	g_return_val_if_fail(conn != NULL, FALSE);

	params[0] = name;
	params[1] = enabled ? "true" : "false";
	if (!axis_db_conn_exec(conn, set_enabled_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND,
			"there is no schedule '%s'", name != NULL ? name : "(null)");
		return FALSE;
	}

	return TRUE;
}

/* The schedules of @kinds (a text array) that lie more than an interval
 * ahead, brought back to one; each said once, when it is brought back. */
static gboolean
pull_back(
	AxisDbConn   *conn,
	const gchar  *kinds,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	params[0] = kinds;
	if (!axis_db_conn_exec(conn, pull_back_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 ahead = 0;
		gint64 seconds = 0;

		axis_db_result_get_int64(result, i, 1, &ahead);
		axis_db_result_get_int64(result, i, 2, &seconds);
		g_warning("schedule '%s' was next due in %" G_GINT64_FORMAT " s, more than its interval of %"
			G_GINT64_FORMAT " s ahead (the database's clock went back?): it is due in %" G_GINT64_FORMAT
			" s now", axis_db_result_get_text(result, i, 0), ahead, seconds, seconds);
	}
	return TRUE;
}

gboolean
axis_job_schedule_tick(
	AxisDbConn       *conn,
	AxisJobRegistry  *registry,
	guint            *n_enqueued,
	GError          **error
){
	g_autoptr(GPtrArray) kinds = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kind_array = NULL;
	g_autofree gchar *queue_array = NULL;
	g_autofree gchar *attempt_array = NULL;
	g_autofree const gchar **kind_names = NULL;
	g_autofree const gchar **queue_names = NULL;
	GString *attempts;
	const gchar *params[3];
	guint i;

	g_return_val_if_fail(conn != NULL, FALSE);
	g_return_val_if_fail(registry != NULL, FALSE);

	if (n_enqueued != NULL)
		*n_enqueued = 0;

	/* The schedules of a kind whose module is switched off stay where they
	 * are, due, and fire once it is switched on again. */
	{
		g_autoptr(GPtrArray) all = axis_job_registry_list_kinds(registry);

		kinds = g_ptr_array_new();
		for (i = 0; i < all->len; i++) {
			const AxisJobKind *kind = g_ptr_array_index(all, i);

			if (axis_job_registry_kind_enabled(registry, kind))
				g_ptr_array_add(kinds, (gpointer)kind);
		}
	}
	if (kinds->len == 0)
		return TRUE;

	kind_names = g_new0(const gchar *, kinds->len + 1);
	queue_names = g_new0(const gchar *, kinds->len + 1);
	attempts = g_string_new("{");
	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);

		kind_names[i] = kind->kind;
		queue_names[i] = kind->queue;
		g_string_append_printf(attempts, "%s%u", i > 0 ? "," : "", kind->max_attempts);
	}
	g_string_append_c(attempts, '}');
	kind_array = axis_job_text_array(kind_names);
	queue_array = axis_job_text_array(queue_names);
	attempt_array = g_string_free(attempts, FALSE);

	params[0] = kind_array;
	params[1] = queue_array;
	params[2] = attempt_array;
	if (!pull_back(conn, kind_array, error) ||
	    !axis_db_conn_exec(conn, tick_sql, G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	if (n_enqueued != NULL)
		*n_enqueued = axis_db_result_get_n_rows(result);
	return TRUE;
}
