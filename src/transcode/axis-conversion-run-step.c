/*
 * axis-conversion-run-step.c - the step of the conversion runs: what ended
 * of their files in flight, why the running run waits or pauses, and the
 * next files it asks for
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The step job runs every minute and when a run is started, resumed or
 * changed (de-duplicated, on the maintenance queue: one at a time).  It
 * holds one pooled connection at a time and none while it asks for a
 * conversion, which takes its own (the job contract).
 *
 * Asking for a file is axis_conversions_request_for_run(): the request of a
 * single conversion, its gates evaluated there and nowhere else.  What the
 * step adds is only when to ask and what to make of a refusal:
 *
 *  - read-only storage, a volume away or below its floor, no worker that can
 *    plan: the file stays in the list and waits (a volume's files are
 *    frozen and the run goes on with other volumes' files);
 *  - the player check not safe, the revision not proven or not current any
 *    more: the run pauses, for a person;
 *  - anything about the file itself (not eligible any more, changed,
 *    remembered, in flight): the file is skipped with the gate's words, and
 *    the run goes on; AXIS_CONVERSION_RUN_REFUSALS of those in a row pause it.
 */

#include "transcode/axis-conversion-run-private.h"

#include <string.h>

#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"

/* How many candidates a fetch reads, and how many fetches a step makes at
 * most (each skips the volumes found frozen before). */
#define CANDIDATES 16
#define MAX_FETCHES 64

static gchar *
int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_is_null(result, row, column))
		axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static const gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : axis_db_result_get_text(result, row, column);
}

/* A file's name for a sentence: its last part, displayed. */
static gchar *
file_text(const gchar *rel_path)
{
	const gchar *slash = rel_path != NULL ? strrchr(rel_path, '/') : NULL;

	return axis_library_name_display(slash != NULL ? slash + 1 : (rel_path != NULL ? rel_path : "?"));
}

static gboolean
exec(
	AxisConversionRuns  *self,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	AxisDbResult       **result,
	GError             **error
){
	return axis_db_pool_exec(self->conversions->pool, sql, n_params, params, result, error);
}

/* The run pauses itself (when it is running), for a person. */
static gboolean
pause_run(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *code,
	const gchar         *text,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *clean = axis_library_text_clean(text, 2000);
	const gchar *params[3];

	params[0] = id;
	params[1] = code;
	params[2] = clean;
	if (!exec(self, "UPDATE conversion_run SET state = 'paused', pause_code = $2, pause_text = $3, paused_at = now(),"
	        " paused_by = NULL, wait_code = NULL, wait_text = NULL, wait_since = NULL, updated_at = now()"
	        " WHERE id = $1::bigint AND state = 'running' RETURNING id", 3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		g_message("conversion runs: run %" G_GINT64_FORMAT " paused itself: %s", run_id, clean);
	return TRUE;
}

/* The run waits (it goes on by itself), or waits no more (@code NULL). */
static gboolean
set_wait(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *code,
	const gchar         *text,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *clean = text != NULL ? axis_library_text_clean(text, 2000) : NULL;
	const gchar *params[3];

	params[0] = id;
	params[1] = code;
	params[2] = code != NULL ? clean : NULL;
	return exec(self, "UPDATE conversion_run SET wait_code = $2, wait_text = $3,"
		" wait_since = CASE WHEN $2::text IS NULL THEN NULL WHEN wait_code IS DISTINCT FROM $2::text THEN now()"
		"  ELSE wait_since END, stepped_at = now() WHERE id = $1::bigint", 3, params, NULL, error);
}

static gboolean
finish_run(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *text,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[2];

	params[0] = id;
	params[1] = text;
	if (!exec(self, "UPDATE conversion_run SET state = 'finished', finished_at = now(), finish_text = $2,"
	        " wait_code = NULL, wait_text = NULL, wait_since = NULL, updated_at = now() WHERE id = $1::bigint"
	        " AND state = 'running' RETURNING id", 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		g_message("conversion runs: run %" G_GINT64_FORMAT " finished: %s", run_id, text);
	return TRUE;
}

/* ── What ended ─────────────────────────────────────────────────── */

static gboolean
settle_entry(
	AxisConversionRuns  *self,
	gint64               entry_id,
	const gchar         *state,
	const gchar         *code,
	const gchar         *text,
	gint64               result_size,
	GError             **error
){
	g_autofree gchar *id = int_text(entry_id);
	g_autofree gchar *size = result_size >= 0 ? int_text(result_size) : NULL;
	g_autofree gchar *clean = text != NULL ? axis_library_text_clean(text, 2000) : NULL;
	const gchar *params[5];

	params[0] = id;
	params[1] = state;
	params[2] = code;
	params[3] = clean;
	params[4] = size;
	return exec(self, "UPDATE conversion_run_entry SET state = $2, reason_code = $3, reason_text = $4,"
		" result_size = $5::bigint, settled_at = now() WHERE id = $1::bigint AND state = 'asked'", 5, params, NULL,
		error);
}

/* @column of the run += 1, or = 0 with @reset. */
static gboolean
bump(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *column,
	gboolean             reset,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *sql = NULL;
	const gchar *params[1];

	/* Only the two counters' own names reach the statement. */
	if (strcmp(column, "failed_in_row") != 0 && strcmp(column, "refused_in_row") != 0)
		return TRUE;
	if (reset)
		sql = g_strdup_printf("UPDATE conversion_run SET %s = 0 WHERE id = $1::bigint AND %s <> 0", column, column);
	else
		sql = g_strdup_printf("UPDATE conversion_run SET %s = %s + 1, updated_at = now() WHERE id = $1::bigint", column,
			column);
	params[0] = id;
	return exec(self, sql, 1, params, NULL, error);
}

static gboolean
settle(
	AxisConversionRuns           *self,
	AxisConversionRunStepReport  *report,
	GError                      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	/* An entry marked asked whose request committed is linked to its
	 * conversion: the newest one requested for it since it was marked (an
	 * entry that moved from trials to replacing has its trial too, and one
	 * given back has the conversion it gave back); one whose request never
	 * committed (the step stopped in between) waits again. */
	if (!exec(self, "UPDATE conversion_run_entry e SET conversion_id = (SELECT max(c.id) FROM conversion c"
	        "  WHERE c.run_entry_id = e.id AND c.requested_at >= coalesce(e.asked_at, '-infinity'::timestamptz))"
	        " WHERE e.state = 'asked' AND e.conversion_id IS NULL", 0, NULL, NULL, error) ||
	    !exec(self, "UPDATE conversion_run_entry SET state = 'waiting', outcome = NULL, asked_at = NULL"
	        " WHERE state = 'asked' AND conversion_id IS NULL", 0, NULL, NULL, error))
		return FALSE;
	if (!exec(self, "SELECT e.id, e.run_id, e.conversion_id, c.state, coalesce(c.failed_check, 0), c.result_text,"
	        " coalesce(c.output_size, c.output_reported, 0), c.outcome, w.name, w.paused_reason, r.state, r.outcome,"
	        " e.rel_path, coalesce(c.retryable, false)"
	        " FROM conversion_run_entry e JOIN conversion_run r ON r.id = e.run_id"
	        " LEFT JOIN conversion c ON c.id = e.conversion_id"
	        " LEFT JOIN LATERAL (SELECT ww.name, ww.paused_reason FROM worker ww WHERE ww.paused_at IS NOT NULL"
	        "  AND c.job_id IS NOT NULL AND ww.paused_job = c.job_id LIMIT 1) w ON true"
	        " WHERE e.state = 'asked' ORDER BY e.run_id, e.position", 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 entry = int_at(rows, i, 0);
		gint64 run = int_at(rows, i, 1);
		gint64 conversion = int_at(rows, i, 2);
		const gchar *state = text_at(rows, i, 3);
		gint check = (gint)int_at(rows, i, 4);
		const gchar *result_text = text_at(rows, i, 5);
		gint64 output = int_at(rows, i, 6);
		const gchar *outcome = text_at(rows, i, 7);
		const gchar *crashed_on = text_at(rows, i, 8);
		const gchar *crash_reason = text_at(rows, i, 9);
		const gchar *run_state = text_at(rows, i, 10);
		const gchar *run_outcome = text_at(rows, i, 11);
		g_autofree gchar *name = file_text(text_at(rows, i, 12));
		g_autofree gchar *code = NULL;
		gboolean retryable = FALSE;

		axis_db_result_get_boolean(rows, i, 13, &retryable);

		if (state == NULL) {
			/* The conversion's row is gone: the file waits again. */
			g_autofree gchar *id = int_text(entry);
			const gchar *params[1];

			params[0] = id;
			if (!exec(self, "UPDATE conversion_run_entry SET state = 'waiting', conversion_id = NULL, outcome = NULL"
			        " WHERE id = $1::bigint AND state = 'asked'", 1, params, NULL, error))
				return FALSE;
			continue;
		}
		if (g_strcmp0(state, "queued") == 0 || g_strcmp0(state, "encoding") == 0 ||
		    g_strcmp0(state, "verifying") == 0 || g_strcmp0(state, "placing") == 0) {
			/* In flight.  Its worker's encoder crashed: the worker paused
			 * itself, and so does the run. */
			if (crashed_on != NULL && g_strcmp0(run_state, "running") == 0) {
				g_autofree gchar *text = g_strdup_printf("The encoder crashed on worker %s while it converted %s (%s). "
					"The worker takes no work until a person resumes it on the Workers page; this run paused itself so "
					"that a fault does not go on through the list. Look at the worker and the conversion, resume the "
					"worker, then resume this run.", crashed_on, name, crash_reason != NULL ? crash_reason : "no reason given");

				if (!pause_run(self, run, "crash", text, error))
					return FALSE;
			}
			continue;
		}
		if (report != NULL)
			report->settled++;
		if (g_strcmp0(state, "done") == 0) {
			if (g_strcmp0(outcome, "trial") == 0 && g_strcmp0(run_outcome, "replace") == 0) {
				/* A trial that finished after the run moved on to replacing:
				 * the file is asked for again as a replacement. */
				g_autofree gchar *id = int_text(entry);
				g_autofree gchar *trial = int_text(conversion);
				const gchar *params[2];

				params[0] = id;
				params[1] = trial;
				if (!exec(self, "UPDATE conversion_run_entry SET state = 'waiting', trial_id = $2::bigint,"
				        " conversion_id = NULL, outcome = NULL WHERE id = $1::bigint AND state = 'asked'", 2, params, NULL,
				        error))
					return FALSE;
			} else if (!settle_entry(self, entry, "done", NULL, NULL, output, error)) {
				return FALSE;
			}
			if (!bump(self, run, "failed_in_row", TRUE, error) || !bump(self, run, "refused_in_row", TRUE, error))
				return FALSE;
			continue;
		}
		if (g_strcmp0(state, "rejected") == 0) {
			code = g_strdup_printf("check_%d", check);
			/* The source changed, it was not worth it, or a gate failed just
			 * before placement: the file's own matter. */
			if (check == 2 || check == 9 || check == 10) {
				if (!settle_entry(self, entry, "skipped", code, result_text, -1, error))
					return FALSE;
				continue;
			}
			if (!settle_entry(self, entry, "failed", code, result_text, -1, error) ||
			    !bump(self, run, "failed_in_row", FALSE, error))
				return FALSE;
			/* With trials, one rejected by a check of the converted file
			 * means the version cannot be proven by hand. */
			if (g_strcmp0(outcome, "trial") == 0 && check >= 3 && check <= 8) {
				g_autofree gchar *text = g_strdup_printf("The trial of %s was rejected by check %d, \"%s\". Proving a "
					"version by hand needs trials none of which a check of the converted file rejected, so this "
					"version cannot be proven from its trials as they are: look at the conversion. If the profile is at "
					"fault, change it (that makes a new version) and make a new run with it; if it is not (a check "
					"found wrong since), set the trial aside on its page, then resume this run.", name, check,
					axis_conversion_check_label((guint)check));

				if (!pause_run(self, run, "bad_trial", text, error))
					return FALSE;
			}
			continue;
		}
		/* Not judged (the plan could not be made again as the worker was
		 * given it): neither the file's matter nor a failure of the revision;
		 * the file is asked for again (a refusal in a row, so that it never
		 * goes round for ever). */
		if (g_strcmp0(state, "failed") == 0 && retryable) {
			g_autofree gchar *id = int_text(entry);
			const gchar *params[1];

			params[0] = id;
			if (!exec(self, "UPDATE conversion_run_entry SET state = 'waiting', conversion_id = NULL, outcome = NULL,"
			        " asked_at = NULL WHERE id = $1::bigint AND state = 'asked'", 1, params, NULL, error) ||
			    !bump(self, run, "refused_in_row", FALSE, error))
				return FALSE;
			continue;
		}
		if (g_strcmp0(state, "failed") == 0) {
			if (!settle_entry(self, entry, "failed", "failed", result_text, -1, error) ||
			    !bump(self, run, "failed_in_row", FALSE, error))
				return FALSE;
			continue;
		}
		if (g_strcmp0(state, "refused") == 0) {
			if (!settle_entry(self, entry, "skipped", "refused", result_text, -1, error) ||
			    !bump(self, run, "refused_in_row", FALSE, error))
				return FALSE;
			continue;
		}
		/* Cancelled: with the run, or by a person on its page. */
		if (!settle_entry(self, entry, "skipped", "cancelled", g_strcmp0(run_state, "cancelled") == 0 ?
		        "Cancelled with the run." : (result_text != NULL ? result_text : "The conversion was cancelled."), -1,
		        error))
			return FALSE;
	}
	return TRUE;
}

/* Runs that failed in a row too often pause. */
static gboolean
pause_failing(
	AxisConversionRuns  *self,
	GError             **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *limit = int_text(axis_conversion_setting_integer(self->conversions,
		AXIS_CONVERSION_SETTING_RUN_FAILURES, 3));
	g_autofree gchar *refusals = int_text(AXIS_CONVERSION_RUN_REFUSALS);
	const gchar *params[2];
	guint i;

	params[0] = limit;
	params[1] = refusals;
	if (!exec(self, "SELECT r.id, r.failed_in_row, r.refused_in_row, e.rel_path, e.reason_text FROM conversion_run r"
	        " LEFT JOIN LATERAL (SELECT x.rel_path, x.reason_text FROM conversion_run_entry x WHERE x.run_id = r.id"
	        "  AND x.state IN ('failed', 'skipped') ORDER BY x.settled_at DESC, x.id DESC LIMIT 1) e ON true"
	        " WHERE r.state = 'running' AND (r.failed_in_row >= $1::integer OR r.refused_in_row >= $2::integer)", 2,
	        params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 failed = int_at(rows, i, 1);
		g_autofree gchar *name = file_text(text_at(rows, i, 3));
		const gchar *why = text_at(rows, i, 4);
		g_autofree gchar *text = NULL;

		if (failed >= g_ascii_strtoll(limit, NULL, 10))
			text = g_strdup_printf("%" G_GINT64_FORMAT " files in a row failed or were rejected by a check of the converted "
				"file (the last, %s: %s). Something they share (the profile, a worker, a disk) is more likely than bad "
				"luck, so the run paused itself rather than go on through the list (convert.run_failures). Look at "
				"their conversions, then resume the run.", failed, name, why != NULL ? why : "no reason given");
		else
			text = g_strdup_printf("%" G_GINT64_FORMAT " files in a row were refused when the run asked for them (the "
				"last, %s: %s). A condition they all share is more likely than each file's own: look at it, then resume "
				"the run.", int_at(rows, i, 2), name, why != NULL ? why : "no reason given");
		if (!pause_run(self, int_at(rows, i, 0), failed >= g_ascii_strtoll(limit, NULL, 10) ? "failures" : "refusals",
		        text, error))
			return FALSE;
	}
	return TRUE;
}

/* Runs being listed whose list job ended without a list. */
static gboolean
fail_lost_lists(
	AxisConversionRuns  *self,
	GError             **error
){
	/* A run to whose list the kept files were being added (convert-kept-
	 * files) is as it was before: its state, its list, the choice off.  The
	 * job works in one transaction, so nothing of it is there. */
	if (!exec(self, "UPDATE conversion_run r SET state = r.relist_state, relist_state = NULL, convert_kept = false,"
	        " convert_kept_by = NULL, convert_kept_at = NULL, updated_at = now(),"
	        " list_text = left('The files Axis keeps were not added, and the run is as it was: the job ended without "
	        "adding them (' || coalesce(j.state::text, 'gone') || coalesce(': ' || j.last_error, '') || ').', 2000)"
	        " FROM conversion_run r2 LEFT JOIN job j ON j.id = r2.list_job_id"
	        " WHERE r.id = r2.id AND r.state = 'listing' AND r.relist_state IS NOT NULL"
	        "  AND (j.id IS NULL OR j.state IN ('failed', 'cancelled', 'succeeded'))"
	        "  AND r.updated_at < now() - interval '5 seconds'", 0, NULL, NULL, error))
		return FALSE;
	return exec(self, "UPDATE conversion_run r SET state = 'failed', finished_at = now(), updated_at = now(),"
		" list_text = left('The list job ended without a list (' || coalesce(j.state::text, 'gone')"
		"  || coalesce(': ' || j.last_error, '') || '). Make a new run.', 2000)"
		" FROM conversion_run r2 LEFT JOIN job j ON j.id = r2.list_job_id"
		" WHERE r.id = r2.id AND r.state = 'listing' AND r.relist_state IS NULL"
		"  AND (j.id IS NULL OR j.state IN ('failed', 'cancelled', 'succeeded'))"
		"  AND r.created_at < now() - interval '5 seconds'", 0, NULL, NULL, error);
}

/* ── The running run ────────────────────────────────────────────── */

typedef struct
{
	gint64  id;
	gint64  revision_id;
	gchar  *outcome;
	gint    at_once;
	gint    cap_unit;
	gint    current_unit;
	gint64  cap_seconds;
	gint    window_start;
	gint    window_end;
	gint64  started_us;
	gchar  *started_by;
	gchar  *confirmed_by;
	gint64  in_flight;
	gint64  in_flight_trials;
} Running;

static void
running_clear(Running *r)
{
	g_free(r->outcome);
	g_free(r->started_by);
	g_free(r->confirmed_by);
	memset(r, 0, sizeof(*r));
}

static gboolean
load_running(
	AxisConversionRuns  *self,
	Running             *r,
	gboolean            *found,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*found = FALSE;
	memset(r, 0, sizeof(*r));
	if (!exec(self, "SELECT r.id, r.revision_id, r.outcome, r.at_once, coalesce(r.cap_unit, -1),"
	        " coalesce(r.cap_seconds, 0), coalesce(r.window_start, -1), coalesce(r.window_end, -1),"
	        " coalesce((extract(epoch FROM r.started_at) * 1000000)::bigint, 0), r.started_by, r.confirmed_by,"
	        " (SELECT count(*) FROM conversion_run_entry e WHERE e.run_id = r.id AND e.state = 'asked'),"
	        " (SELECT count(*) FROM conversion_run_entry e WHERE e.run_id = r.id AND e.state = 'asked'"
	        "  AND e.outcome = 'trial'), coalesce(r.current_unit, 0)"
	        " FROM conversion_run r WHERE r.state = 'running' ORDER BY r.id LIMIT 1", 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1)
		return TRUE;
	*found = TRUE;
	r->id = int_at(result, 0, 0);
	r->revision_id = int_at(result, 0, 1);
	r->outcome = g_strdup(text_at(result, 0, 2));
	r->at_once = (gint)CLAMP(int_at(result, 0, 3), 1, AXIS_CONVERSION_RUN_MAX_AT_ONCE);
	r->cap_unit = (gint)int_at(result, 0, 4);
	r->cap_seconds = int_at(result, 0, 5);
	r->window_start = (gint)int_at(result, 0, 6);
	r->window_end = (gint)int_at(result, 0, 7);
	r->started_us = int_at(result, 0, 8);
	r->started_by = g_strdup(text_at(result, 0, 9));
	r->confirmed_by = g_strdup(text_at(result, 0, 10));
	r->in_flight = int_at(result, 0, 11);
	r->in_flight_trials = int_at(result, 0, 12);
	r->current_unit = (gint)int_at(result, 0, 13);
	return TRUE;
}

/* How many files the connected workers can take at once (heard from in the
 * last minute, of this protocol, neither revoked nor paused, with a volume
 * proved), and how many workers are registered at all. */
static gboolean
worker_capacity(
	AxisConversionRuns  *self,
	gint64              *capacity,
	gint64              *registered,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *seen = int_text(AXIS_CONVERSION_WORKER_SEEN_S);
	const gchar *params[1];

	params[0] = seen;
	if (!exec(self, "SELECT coalesce(sum(coalesce(w.max_jobs, 1)) FILTER (WHERE w.paused_at IS NULL"
	        "  AND w.refused_protocol IS NULL AND w.last_seen_at > now() - $1::bigint * interval '1 second'"
	        "  AND EXISTS (SELECT 1 FROM worker_volume wv WHERE wv.worker_id = w.id)), 0),"
	        " count(*) FROM worker w WHERE w.revoked_at IS NULL AND w.registered_at IS NOT NULL", 1, params, &result,
	        error))
		return FALSE;
	*capacity = int_at(result, 0, 0);
	*registered = int_at(result, 0, 1);
	return TRUE;
}

/* Whether a connected, unpaused worker has proved @volume_id. */
static gboolean
volume_has_worker(
	AxisConversionRuns  *self,
	gint64               volume_id,
	gboolean            *has,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *volume = int_text(volume_id);
	g_autofree gchar *seen = int_text(AXIS_CONVERSION_WORKER_SEEN_S);
	const gchar *params[2];

	params[0] = volume;
	params[1] = seen;
	if (!exec(self, "SELECT EXISTS (SELECT 1 FROM worker w JOIN worker_volume wv ON wv.worker_id = w.id"
	        " WHERE wv.volume_id = $1::bigint AND w.revoked_at IS NULL AND w.paused_at IS NULL"
	        " AND w.refused_protocol IS NULL AND w.registered_at IS NOT NULL"
	        " AND w.last_seen_at > now() - $2::bigint * interval '1 second')", 2, params, &result, error))
		return FALSE;
	*has = FALSE;
	axis_db_result_get_boolean(result, 0, 0, has);
	return TRUE;
}

/* A volume whose files wait this step. */
typedef struct
{
	gint64  volume_id;
	gchar  *code;
	gchar  *text;
} Frozen;

static void
frozen_free(gpointer data)
{
	Frozen *f = data;

	g_free(f->code);
	g_free(f->text);
	g_free(f);
}

static void
freeze(
	GPtrArray   *frozen,
	gint64       volume_id,
	const gchar *code,
	const gchar *text
){
	Frozen *f = g_new0(Frozen, 1);

	f->volume_id = volume_id;
	f->code = g_strdup(code);
	f->text = axis_library_text_clean(text, 2000);
	g_ptr_array_add(frozen, f);
}

static gboolean
is_frozen(
	GPtrArray *frozen,
	gint64     volume_id
){
	guint i;

	for (i = 0; i < frozen->len; i++) {
		if (((Frozen *)g_ptr_array_index(frozen, i))->volume_id == volume_id)
			return TRUE;
	}
	return FALSE;
}

static gchar *
frozen_list(GPtrArray *frozen)
{
	GString *list = g_string_new("{");
	guint i;

	for (i = 0; i < frozen->len; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			((Frozen *)g_ptr_array_index(frozen, i))->volume_id);
	g_string_append_c(list, '}');
	return g_string_free(list, FALSE);
}

/* Whether the files of @volume_id can be taken by a worker now: it is online
 * and a connected worker has proved it.  When not, the volume is frozen for
 * this step, saying why. */
static gboolean
volume_usable(
	AxisConversionRuns  *self,
	gint64               volume_id,
	GPtrArray           *frozen,
	gboolean            *usable,
	GError             **error
){
	g_autoptr(AxisVolumeInfo) info = axis_storage_get_volume(self->conversions->storage, volume_id);
	gboolean has = FALSE;

	*usable = FALSE;
	if (is_frozen(frozen, volume_id))
		return TRUE;
	if (info == NULL || info->state != AXIS_VOLUME_ONLINE) {
		g_autofree gchar *text = g_strdup_printf("%s is %s: its files wait (an offline disk is not a failure of the "
			"run); the run goes on with others and comes back to them when it is online.",
			info != NULL ? info->name : "Its disk", info == NULL ? "not known" :
			info->state == AXIS_VOLUME_READ_ONLY ? "mounted read-only" :
			info->state == AXIS_VOLUME_MISMATCH ? "not the disk Axis knows (its marker does not match)" : "offline");

		freeze(frozen, volume_id, "offline", text);
		return TRUE;
	}
	if (!volume_has_worker(self, volume_id, &has, error))
		return FALSE;
	if (!has) {
		g_autofree gchar *text = g_strdup_printf("No connected worker can reach %s (Conversion workers): its files "
			"wait for one; the run goes on with others and comes back to them when one has.", info->name);

		freeze(frozen, volume_id, "no_worker", text);
		return TRUE;
	}
	*usable = TRUE;
	return TRUE;
}

/*
 * Files in flight whose conversion waits for a worker that none of the
 * connected ones can be (their volume is not online, or no connected worker
 * has proved it): given back (nothing was made of them), their entries
 * waiting in the list again and their volume frozen, so that the run does
 * not hold its places for them but goes on with other volumes' files, and
 * says which volumes wait and why.  *@given and *@trials: how many, and how
 * many of them trials.
 */
static gboolean
give_back_stranded(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray           *frozen,
	gint64              *given,
	gint64              *trials,
	GError             **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[1];
	guint i;

	*given = 0;
	*trials = 0;
	params[0] = id;
	if (!exec(self, "SELECT e.id, e.conversion_id, coalesce(e.volume_id, 0), coalesce(e.outcome, '') FROM"
	        " conversion_run_entry e JOIN conversion c ON c.id = e.conversion_id WHERE e.run_id = $1::bigint"
	        " AND e.state = 'asked' AND c.state = 'queued' ORDER BY e.position", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 entry = int_at(rows, i, 0);
		gint64 conversion = int_at(rows, i, 1);
		gint64 volume = int_at(rows, i, 2);
		g_autofree gchar *text = NULL;
		gboolean usable = FALSE;
		gboolean back = FALSE;
		guint k;

		if (!volume_usable(self, volume, frozen, &usable, error))
			return FALSE;
		if (usable)
			continue;
		for (k = 0; k < frozen->len; k++) {
			const Frozen *f = g_ptr_array_index(frozen, k);

			if (f->volume_id == volume)
				text = g_strdup_printf("Given back by conversion run %" G_GINT64_FORMAT " before a worker took it: %s",
					run_id, f->text);
		}
		if (!axis_conversions_give_back_for_run(self->conversions, conversion, entry, text != NULL ? text :
		        "Given back by its conversion run before a worker took it.", &back, error))
			return FALSE;
		if (back) {
			(*given)++;
			if (g_strcmp0(text_at(rows, i, 3), "trial") == 0)
				(*trials)++;
		}
	}
	return TRUE;
}

/* The volume waits as this step found them. */
static gboolean
write_waits(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray           *frozen,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *list = frozen_list(frozen);
	const gchar *params[4];
	guint i;

	params[0] = id;
	params[1] = list;
	if (!exec(self, "DELETE FROM conversion_run_wait WHERE run_id = $1::bigint AND NOT (volume_id = ANY ($2::bigint[]))",
	        2, params, NULL, error))
		return FALSE;
	for (i = 0; i < frozen->len; i++) {
		const Frozen *f = g_ptr_array_index(frozen, i);
		g_autofree gchar *volume = int_text(f->volume_id);

		params[1] = volume;
		params[2] = f->code;
		params[3] = f->text;
		if (!exec(self, "INSERT INTO conversion_run_wait (run_id, volume_id, code, text) VALUES ($1::bigint, $2::bigint,"
		        " $3, $4) ON CONFLICT (run_id, volume_id) DO UPDATE SET code = EXCLUDED.code, text = EXCLUDED.text,"
		        " since = CASE WHEN conversion_run_wait.code = EXCLUDED.code THEN conversion_run_wait.since ELSE now() END",
		        4, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

/* Puts an entry marked asked back (@code NULL) or skips it with what the
 * gate said. */
static gboolean
unask(
	AxisConversionRuns  *self,
	gint64               entry_id,
	const gchar         *code,
	const gchar         *text,
	GError             **error
){
	g_autofree gchar *id = int_text(entry_id);
	g_autofree gchar *clean = text != NULL ? axis_library_text_clean(text, 2000) : NULL;
	const gchar *params[3];

	params[0] = id;
	params[1] = code;
	params[2] = clean;
	if (code == NULL)
		return exec(self, "UPDATE conversion_run_entry SET state = 'waiting', outcome = NULL, asked_at = NULL"
			" WHERE id = $1::bigint AND state = 'asked' AND conversion_id IS NULL", 1, params, NULL, error);
	return exec(self, "UPDATE conversion_run_entry SET state = 'skipped', reason_code = $2, reason_text = $3,"
		" settled_at = now() WHERE id = $1::bigint AND state = 'asked' AND conversion_id IS NULL", 3, params, NULL, error);
}

typedef enum
{
	ASK_ASKED,	/* a conversion was asked for */
	ASK_SKIPPED,	/* the file is skipped; go on */
	ASK_FROZEN,	/* its volume's files wait; go on with others */
	ASK_STOP	/* the run waits or paused: no more this step */
} AskOutcome;

static AskOutcome
ask_one(
	AxisConversionRuns  *self,
	const Running       *run,
	gint64               entry_id,
	gint64               file_id,
	gint64               volume_id,
	const gchar         *rel_path,
	GPtrArray           *frozen,
	gint64               registered,
	GError             **error
){
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisDbResult) marked = NULL;
	g_autofree gchar *gate = NULL;
	g_autofree gchar *id = int_text(entry_id);
	g_autofree gchar *run_text = int_text(run->id);
	g_autofree gchar *name = file_text(rel_path);
	g_autofree gchar *kept_by = NULL;
	g_autofree gchar *entry_confirmed_by = NULL;
	gboolean replace = g_strcmp0(run->outcome, "replace") == 0;
	const gchar *user = replace ? run->confirmed_by : run->started_by;
	const gchar *params[3];
	gint64 conversion = 0;

	if (file_id <= 0) {
		if (!unask(self, entry_id, NULL, NULL, error))
			return ASK_STOP;
		params[0] = id;
		if (!exec(self, "UPDATE conversion_run_entry SET state = 'skipped', reason_code = 'gone',"
		        " reason_text = 'The file is no longer in the library.', settled_at = now() WHERE id = $1::bigint"
		        " AND state = 'waiting'", 1, params, NULL, error))
			return ASK_STOP;
		return ASK_SKIPPED;
	}
	params[0] = id;
	params[1] = run_text;
	params[2] = replace ? "replace" : "trial";
	/* Marked asked, and read with it as the run is now (not as the step
	 * found it when it began): who chose to convert kept files when the
	 * choice covers this file (it was kept when it was listed, or added),
	 * and who confirmed that its original is replaced when it was added to
	 * a run that replaces originals. */
	if (!exec(self, "UPDATE conversion_run_entry e SET state = 'asked', outcome = $3, asked_at = now()"
	        " FROM conversion_run r WHERE e.id = $1::bigint AND e.run_id = $2::bigint AND e.state = 'waiting'"
	        " AND r.id = e.run_id AND r.state = 'running'"
	        " RETURNING e.id, CASE WHEN r.convert_kept AND e.convert_kept THEN r.convert_kept_by END, e.confirmed_by", 3,
	        params, &marked, error))
		return ASK_STOP;
	if (axis_db_result_get_n_rows(marked) != 1)
		return ASK_STOP;
	kept_by = g_strdup(text_at(marked, 0, 1));
	entry_confirmed_by = g_strdup(text_at(marked, 0, 2));
	/* A file added to a replacing run is replaced on the confirmation given
	 * for it, by the person who gave it. */
	if (replace && entry_confirmed_by != NULL)
		user = entry_confirmed_by;
	if (axis_conversions_request_for_run(self->conversions, file_id, run->revision_id, replace ?
	        AXIS_CONVERSION_REPLACE : AXIS_CONVERSION_TRIAL, replace && user != NULL, kept_by,
	        user != NULL ? user : "?", run->id, entry_id, &conversion, &gate, &local)) {
		g_autoptr(AxisDbResult) state = NULL;
		g_autofree gchar *conv = int_text(conversion);
		const gchar *more[2];

		more[0] = id;
		more[1] = conv;
		if (!exec(self, "UPDATE conversion_run_entry SET conversion_id = $2::bigint WHERE id = $1::bigint", 2, more, NULL,
		        error))
			return ASK_STOP;
		more[0] = run_text;
		if (!exec(self, "UPDATE conversion_run SET refused_in_row = 0 WHERE id = $1::bigint RETURNING state", 1, more,
		        &state, error))
			return ASK_STOP;
		/* Cancelled while it was asked for: it is given back at once. */
		if (axis_db_result_get_n_rows(state) == 1 && g_strcmp0(text_at(state, 0, 0), "cancelled") == 0) {
			g_autoptr(GError) cancel = NULL;

			if (!axis_conversions_cancel(self->conversions, conversion, user, &cancel))
				g_message("conversion runs: conversion %" G_GINT64_FORMAT " could not be cancelled: %s", conversion,
					cancel->message);
		}
		return ASK_ASKED;
	}

	/* Not asked: what the gate (or the request) said decides. */
	if (local->domain != AXIS_CONVERSION_ERROR ||
	    (local->code != AXIS_CONVERSION_ERROR_REFUSED && local->code != AXIS_CONVERSION_ERROR_NOT_FOUND)) {
		if (!unask(self, entry_id, NULL, NULL, error))
			return ASK_STOP;
		g_propagate_error(error, g_steal_pointer(&local));
		return ASK_STOP;
	}
	if (g_strcmp0(gate, "read_only") == 0) {
		if (!unask(self, entry_id, NULL, NULL, error) ||
		    !set_wait(self, run->id, "read_only", "Storage is read-only (Settings, \"Read-only storage\"): nothing is converted until an "
		        "administrator switches it off in Settings. The run goes on by itself then.", error))
			return ASK_STOP;
		return ASK_STOP;
	}
	if (g_strcmp0(gate, "volume") == 0 || g_strcmp0(gate, "space") == 0) {
		g_autofree gchar *text = g_strdup_printf("%s The files on it wait; the run goes on with others and comes back "
			"to them.", local->message);

		if (!unask(self, entry_id, NULL, NULL, error))
			return ASK_STOP;
		freeze(frozen, volume_id, g_strcmp0(gate, "space") == 0 ? "full" : "offline", text);
		return ASK_FROZEN;
	}
	if (g_strcmp0(gate, "plan") == 0 && registered == 0) {
		if (!unask(self, entry_id, NULL, NULL, error) ||
		    !set_wait(self, run->id, "no_worker", "There is no registered conversion worker (Conversion workers): the "
		        "run waits for one.", error))
			return ASK_STOP;
		return ASK_STOP;
	}
	if (g_strcmp0(gate, "verdict") == 0 || g_strcmp0(gate, "proven") == 0 || g_strcmp0(gate, "revision") == 0) {
		g_autofree gchar *text = g_strdup_printf("%s was refused: %s The run paused itself: it replaces originals only "
			"while every condition a single replacement needs holds. When it holds again, resume the run.", name,
			local->message);

		if (!unask(self, entry_id, NULL, NULL, error) ||
		    !pause_run(self, run->id, g_strcmp0(gate, "verdict") == 0 ? "verdict" : "revision", text, error))
			return ASK_STOP;
		return ASK_STOP;
	}
	/* The file's own matter: skipped, with the gate's words. */
	{
		g_autofree gchar *code = g_strdup_printf("gate_%s", gate != NULL ? gate : "request");

		if (!unask(self, entry_id, code, local->message, error) || !bump(self, run->id, "refused_in_row", FALSE, error))
			return ASK_STOP;
	}
	return ASK_SKIPPED;
}

/* Asks for up to @slots files of the run's list, in its order. */
static gboolean
ask(
	AxisConversionRuns           *self,
	const Running                *run,
	gint64                        slots,
	gint64                        registered,
	GPtrArray                    *frozen,
	AxisConversionRunStepReport  *report,
	gboolean                     *stopped,
	GError                      **error
){
	g_autofree gchar *id = int_text(run->id);
	g_autofree gchar *cap = int_text(run->cap_unit);
	/* The unit it works through first (a series started is finished before
	 * the next is begun), then the list in its order. */
	gint current = run->current_unit;
	gboolean in_current = current > 0;
	guint fetches;

	*stopped = FALSE;
	for (fetches = 0; slots > 0 && fetches < MAX_FETCHES && !*stopped; fetches++) {
		g_autoptr(AxisDbResult) rows = NULL;
		g_autofree gchar *list = frozen_list(frozen);
		g_autofree gchar *unit = int_text(in_current ? current : 0);
		const gchar *params[4];
		guint n;
		guint i;

		params[0] = id;
		params[1] = cap;
		params[2] = list;
		params[3] = unit;
		if (!exec(self, "SELECT e.id, coalesce(e.file_id, 0), coalesce(e.volume_id, 0), e.rel_path, e.unit_position"
		        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint AND e.state = 'waiting'"
		        " AND ($2::integer < 0 OR e.unit_position <= $2::integer)"
		        " AND NOT (coalesce(e.volume_id, 0) = ANY ($3::bigint[]))"
		        " AND ($4::integer <= 0 OR e.unit_position = $4::integer)"
		        " ORDER BY e.position LIMIT " G_STRINGIFY(CANDIDATES), 4, params, &rows, error))
			return FALSE;
		n = axis_db_result_get_n_rows(rows);
		if (n == 0) {
			if (!in_current)
				break;
			in_current = FALSE;
			continue;
		}
		for (i = 0; i < n && slots > 0 && !*stopped; i++) {
			gint64 entry = int_at(rows, i, 0);
			gint64 file = int_at(rows, i, 1);
			gint64 volume = int_at(rows, i, 2);
			const gchar *rel_path = text_at(rows, i, 3);
			gint entry_unit = (gint)int_at(rows, i, 4);
			AskOutcome outcome;

			if (is_frozen(frozen, volume))
				continue;
			/* A volume that is not online, or that no connected worker has
			 * proved: its files wait, nothing is asked. */
			if (file > 0) {
				gboolean usable = FALSE;

				if (!volume_usable(self, volume, frozen, &usable, error))
					return FALSE;
				if (!usable)
					continue;
			}
			outcome = ask_one(self, run, entry, file, volume, rel_path, frozen, registered, error);
			if (error != NULL && *error != NULL)
				return FALSE;
			switch (outcome) {
			case ASK_ASKED:
				slots--;
				if (report != NULL)
					report->asked++;
				/* Its unit is the one to finish now. */
				if (entry_unit != current || !in_current) {
					g_autofree gchar *now_unit = int_text(entry_unit);
					const gchar *more[2];

					more[0] = id;
					more[1] = now_unit;
					if (!exec(self, "UPDATE conversion_run SET current_unit = $2::integer WHERE id = $1::bigint", 2, more,
					        NULL, error))
						return FALSE;
					current = entry_unit;
					in_current = TRUE;
					/* The rest of this batch is the list's order: read again. */
					i = n;
				}
				break;
			case ASK_SKIPPED:
				if (report != NULL)
					report->skipped++;
				break;
			case ASK_FROZEN:
				break;
			default:
				*stopped = TRUE;
				break;
			}
			/* Refusals in a row pause the run at once. */
			if (!*stopped && outcome == ASK_SKIPPED) {
				g_autoptr(AxisDbResult) row = NULL;
				const gchar *more[1];

				more[0] = id;
				if (!exec(self, "SELECT refused_in_row, state FROM conversion_run WHERE id = $1::bigint", 1, more, &row,
				        error))
					return FALSE;
				if (axis_db_result_get_n_rows(row) != 1 || int_at(row, 0, 0) >= AXIS_CONVERSION_RUN_REFUSALS ||
				    g_strcmp0(text_at(row, 0, 1), "running") != 0)
					*stopped = TRUE;
			}
		}
	}
	return TRUE;
}

static gchar *
duration_text(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " day%s", seconds / 86400, seconds == 86400 ? "" : "s");
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " hour%s", seconds / 3600, seconds == 3600 ? "" : "s");
	return g_strdup_printf("%" G_GINT64_FORMAT " minutes", MAX(seconds / 60, 1));
}

/* The running run: whether it waits or pauses, then its next files. */
static gboolean
step_running(
	AxisConversionRuns           *self,
	AxisConversionRunStepReport  *report,
	GError                      **error
){
	g_autoptr(GPtrArray) frozen = g_ptr_array_new_with_free_func(frozen_free);
	g_autoptr(AxisConversionProving) proving = NULL;
	g_autoptr(AxisDbResult) left = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *cap = NULL;
	Running run;
	gboolean found = FALSE;
	gboolean stopped = FALSE;
	gint64 now = axis_conversion_run_now(self);
	gint64 capacity = 0;
	gint64 registered = 0;
	gint64 slots;
	gboolean ok = FALSE;

	if (!load_running(self, &run, &found, error))
		return FALSE;
	if (!found)
		return TRUE;
	id = int_text(run.id);

	/* Its time budget. */
	if (run.cap_seconds > 0 && run.started_us > 0 && now - run.started_us >= run.cap_seconds * G_USEC_PER_SEC) {
		g_autofree gchar *budget = duration_text(run.cap_seconds);
		g_autofree gchar *text = g_strdup_printf("Its time budget of %s is used up.", budget);

		if (run.in_flight > 0) {
			g_autofree gchar *waiting = g_strdup_printf("%s It asks for nothing more; what is in flight finishes.", text);

			ok = set_wait(self, run.id, "budget", waiting, error);
		} else {
			ok = finish_run(self, run.id, text, error);
		}
		goto out;
	}
	/* Conversion work held after a restart. */
	if (axis_conversions_is_held(self->conversions, NULL)) {
		ok = set_wait(self, run.id, "held", "Conversion work has been paused since Axis started (Settings, \"Conversion work waits after a restart\"): "
			"the run asks for nothing until a person presses Resume conversion work on the Conversions page. It goes "
			"on by itself then.", error);
		goto out;
	}
	if (axis_storage_is_read_only_mode(self->conversions->storage)) {
		ok = set_wait(self, run.id, "read_only", "Storage is read-only (Settings, \"Read-only storage\"): nothing is converted until "
			"an administrator switches it off in Settings. The run goes on by itself then.", error);
		goto out;
	}
	if (!axis_conversion_run_in_window(run.window_start, run.window_end, axis_conversion_run_minute_of_day(now))) {
		g_autofree gchar *from = axis_conversion_run_clock_text(run.window_start);
		g_autofree gchar *to = axis_conversion_run_clock_text(run.window_end);
		g_autofree gchar *at = axis_conversion_run_clock_text(axis_conversion_run_minute_of_day(now));
		g_autofree gchar *text = g_strdup_printf("It asks for files between %s and %s only (it is %s now); what is in "
			"flight finishes.", from, to, at);

		ok = set_wait(self, run.id, "window", text, error);
		goto out;
	}

	/* What its version allows now. */
	if (!axis_conversions_proving(self->conversions, run.revision_id, &proving, error))
		goto out;
	if (g_strcmp0(run.outcome, "replace") == 0) {
		const gchar *why = !proving->revision->usable ? "The run's profile version is not usable any more." :
			!axis_profile_revision_is_unlocked(proving->revision) ? "The run's profile version is not proven." :
			!proving->revision->current ? "The run's profile version is no longer its profile's current one: the "
			"playback verdict, which every replacement needs, is for the current one only. Make a new run with the "
			"current version." : NULL;

		if (why != NULL) {
			ok = pause_run(self, run.id, "revision", why, error);
			goto out;
		}
	} else {
		gint64 passed = proving->trials != NULL ? proving->trials->len : 0;

		if (proving->bad_trials > 0) {
			g_autofree gchar *text = g_strdup_printf("%u trial%s of this version %s rejected by a check of the converted "
				"file: proving it by hand needs trials none of which were, so it cannot be proven from its trials as "
				"they are. Look at the conversions. If the profile is at fault, change it (that makes a new version) "
				"and make a new run with it; if it is not (a check found wrong since), set those trials aside on their "
				"pages, then resume this run.", proving->bad_trials, proving->bad_trials == 1 ? "" : "s",
				proving->bad_trials == 1 ? "was" : "were");

			ok = pause_run(self, run.id, "bad_trial", text, error);
			goto out;
		}
		if (axis_profile_revision_is_unlocked(proving->revision)) {
			ok = pause_run(self, run.id, "proven", "The version is proven now: switch the run to replacing originals "
				"(it goes on with the same list), or cancel it.", error);
			goto out;
		}
		if (passed >= (gint64)proving->needed) {
			g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " trials of this version passed every check, "
				"which is what proving it by hand needs (Settings, \"Trials to prove a version by hand\"). Play them on "
				"your players, prove the version on the Conversions page (Review and prove this version), then switch this run to replacing originals: it goes on with the "
				"same list.%s", passed, run.in_flight_trials > 0 ? " The trials still in flight finish." : "");

			ok = pause_run(self, run.id, "prove", text, error);
			goto out;
		}
	}

	/* A worker to take the files. */
	if (!worker_capacity(self, &capacity, &registered, error))
		goto out;
	if (capacity <= 0) {
		const gchar *text = axis_conversions_checks_where(self->conversions) == AXIS_CONVERSION_CHECKS_WORKER_ONLY ?
			"No conversion worker is connected (heard from in the last minute, not paused, that can reach a disk). The "
			"run waits and goes on by itself when one is: this server neither converts nor checks in a worker's place "
			"(Settings, \"Where converted files are checked\" is \"Only on a worker\")." :
			"No conversion worker is connected (heard from in the last minute, not paused, that can reach a disk). The "
			"run waits and goes on by itself when one is; this server never converts in a worker's place.";

		ok = set_wait(self, run.id, "no_worker", text, error);
		goto out;
	}
	/* The estimates, which the gates decide eligibility from. */
	{
		g_autoptr(GError) local = NULL;
		AxisEstimateModel *model = axis_estimator_get_model(self->conversions->estimator, &local);

		if (model == NULL) {
			g_autofree gchar *text = g_strdup_printf("The estimates cannot be worked out just now (%s); the run waits.",
				local != NULL ? local->message : "?");

			ok = set_wait(self, run.id, "estimates", text, error);
			goto out;
		}
		axis_estimator_model_unref(model);
	}

	/* Its places are not held for files no connected worker can take. */
	{
		gint64 given = 0;
		gint64 trials = 0;

		if (!give_back_stranded(self, run.id, frozen, &given, &trials, error))
			goto out;
		run.in_flight -= given;
		run.in_flight_trials -= trials;
	}
	slots = MIN((gint64)run.at_once, capacity) - run.in_flight;
	if (g_strcmp0(run.outcome, "trial") == 0)
		slots = MIN(slots, (gint64)proving->needed - (gint64)(proving->trials != NULL ? proving->trials->len : 0) -
			run.in_flight_trials);
	if (slots > 0 && !ask(self, &run, slots, registered, frozen, report, &stopped, error))
		goto out;
	if (stopped) {
		ok = write_waits(self, run.id, frozen, error);
		goto out;
	}
	if (!write_waits(self, run.id, frozen, error))
		goto out;

	/* What is left of the list. */
	cap = int_text(run.cap_unit);
	{
		const gchar *params[2];

		params[0] = id;
		params[1] = cap;
		if (!exec(self, "SELECT count(*) FILTER (WHERE e.state = 'waiting' AND ($2::integer < 0"
		        "  OR e.unit_position <= $2::integer)), count(*) FILTER (WHERE e.state = 'asked')"
		        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint", 2, params, &left, error))
			goto out;
	}
	if (int_at(left, 0, 0) == 0 && int_at(left, 0, 1) == 0) {
		ok = finish_run(self, run.id, run.cap_unit >= 0 ? "Every file within its caps is done or was skipped." :
			"Every file of the list is done or was skipped.", error);
		goto out;
	}
	if (frozen->len > 0 && int_at(left, 0, 1) == 0) {
		ok = set_wait(self, run.id, "volumes", "Every file left waits for its disk (below): the run goes on by itself "
			"when one is back or has room.", error);
		goto out;
	}
	ok = set_wait(self, run.id, NULL, NULL, error);

out:
	running_clear(&run);
	return ok;
}

gboolean
axis_conversion_runs_step(
	AxisConversionRuns           *self,
	AxisConversionRunStepReport  *report,
	GError                      **error
){
	if (report != NULL)
		memset(report, 0, sizeof(*report));
	if (!fail_lost_lists(self, error))
		return FALSE;
	if (!settle(self, report, error))
		return FALSE;
	/* Failures counted as they ended; refusals counted as it asked. */
	if (!pause_failing(self, error))
		return FALSE;
	if (!step_running(self, report, error))
		return FALSE;
	return pause_failing(self, error);
}

gboolean
axis_conversion_runs_run_step_job(
	AxisConversionRuns  *self,
	AxisJobContext      *context,
	GError             **error
){
	AxisConversionRunStepReport report;

	if (!axis_conversion_runs_step(self, &report, error))
		return FALSE;
	if (report.settled > 0 || report.asked > 0 || report.skipped > 0)
		g_message("conversion runs: %u ended, %u asked for, %u skipped", report.settled, report.asked, report.skipped);
	return TRUE;
}
