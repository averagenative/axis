/*
 * axis-conversion-tidy.c - the finish job (checks, then placement or
 * recovery), the tidy job (what ended without a result, finish jobs for what
 * is in flight, trials and recycled originals whose time has come, the work
 * areas' leftovers), and a trial removed on request
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Only the finish job of a conversion acts on that conversion's files (it
 * is de-duplicated per conversion: one at a time); the tidy job never does,
 * it queues the finish job.  The tidy job removes only what its records
 * name (a trial, a recycled original, by inode and size), and what a work
 * area holds that no conversion accounts for, after the grace time, by a
 * name of Axis's and the inode it was listed with.  A volume that is not
 * provably online is skipped: nothing is decided from an absence.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "transcode/axis-conversion-private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-write.h"

#define LEFTOVERS_PER_VOLUME 1000

/* ── Rejection ──────────────────────────────────────────────────── */

static gboolean
pool_state(
	AxisConversions  *self,
	gint64            id,
	const gchar      *state,
	gint              failed_check,
	const gchar      *text,
	GError          **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_set_state(conn, id, state, failed_check, text, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* A check failed: the output goes (a file of the work area, by its name and
 * the inode the checks saw), the original is not touched. */
static gboolean
reject(
	AxisConversions            *self,
	AxisConversionRow          *row,
	AxisVolumeGuard            *guard,
	gint                        work,
	guint                       failed,
	const AxisConversionVerify *found,
	GError                    **error
){
	g_autofree gchar *text = g_strdup_printf("Rejected by check %u, \"%s\": the converted file was removed and the "
		"original is as it was.", failed, axis_conversion_check_label(failed));
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;

	if (work >= 0 && found->output.st_ino != 0 &&
	    !axis_volume_write_unlink_work(guard, work, row->output_name, (guint64)found->output.st_ino, NULL, &local))
		g_message("conversions: the rejected output of conversion %" G_GINT64_FORMAT " stays for the tidy job: %s",
			row->id, local->message);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_conversion_set_state(conn, row->id, "rejected", (gint)failed, text, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	/* The rules it was judged by: a rejection by a rule that changes later
	 * stops counting against proving its revision. */
	{
		g_autofree gchar *id = axis_conversion_int_text(row->id);
		const gchar *params[1];

		params[0] = id;
		if (!axis_db_conn_exec(conn, "UPDATE conversion SET checks_version = " G_STRINGIFY(AXIS_CONVERSION_CHECKS_VERSION)
		        " WHERE id = $1::bigint", 1, params, NULL, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
	}
	/* Not smaller by the minimum: not offered again with this revision. */
	if (failed == 9 && row->file_id > 0) {
		g_autofree gchar *file = axis_conversion_int_text(row->file_id);
		g_autofree gchar *revision = axis_conversion_int_text(row->revision_id);
		g_autofree gchar *id = axis_conversion_int_text(row->id);
		const gchar *params[3];

		params[0] = file;
		params[1] = revision;
		params[2] = id;
		if (!axis_db_conn_exec(conn, "INSERT INTO conversion_memory (file_id, revision_id, reason, conversion_id)"
		        " VALUES ($1::bigint, $2::bigint, 'not_worth_it', $3::bigint) ON CONFLICT (file_id, revision_id) DO NOTHING",
		        3, params, NULL, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
	}
	axis_db_pool_release(self->pool, conn);
	g_message("conversions: conversion %" G_GINT64_FORMAT " rejected by check %u; the original is untouched", row->id,
		failed);
	return TRUE;
}

/* The settled output's stamp, recorded (it is what every later step
 * compares with). */
static gboolean
record_output(
	AxisConversions    *self,
	AxisConversionRow  *row,
	const struct stat  *st,
	GError            **error
){
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	g_autofree gchar *ino = axis_conversion_int_text((gint64)st->st_ino);
	g_autofree gchar *size = axis_conversion_int_text((gint64)st->st_size);
	g_autofree gchar *ms = axis_conversion_int_text((gint64)st->st_mtim.tv_sec);
	g_autofree gchar *mn = axis_conversion_int_text((gint64)st->st_mtim.tv_nsec);
	g_autofree gchar *cs = axis_conversion_int_text((gint64)st->st_ctim.tv_sec);
	g_autofree gchar *cn = axis_conversion_int_text((gint64)st->st_ctim.tv_nsec);
	const gchar *params[7];

	params[0] = id;
	params[1] = ino;
	params[2] = size;
	params[3] = ms;
	params[4] = mn;
	params[5] = cs;
	params[6] = cn;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET output_inode = $2::bigint, output_size = $3::bigint,"
	        " output_mtime_sec = $4::bigint, output_mtime_nsec = $5::integer, output_ctime_sec = $6::bigint,"
	        " output_ctime_nsec = $7::integer, updated_at = now() WHERE id = $1::bigint", 7, params, NULL, error))
		return FALSE;
	row->output_inode = (gint64)st->st_ino;
	row->output_size = (gint64)st->st_size;
	row->output_mtime_sec = (gint64)st->st_mtim.tv_sec;
	row->output_mtime_nsec = (gint)st->st_mtim.tv_nsec;
	row->output_ctime_sec = (gint64)st->st_ctim.tv_sec;
	row->output_ctime_nsec = (gint)st->st_ctim.tv_nsec;
	return TRUE;
}

/* Every check passed for the output as @found saw it: recorded before
 * anything changes the file (settling, a trial's kept name), so that a
 * finish job tried again later places the same file without judging it
 * again (axis_conversion_verify(), AGAIN). */
static gboolean
record_passed(
	AxisConversions             *self,
	AxisConversionRow           *row,
	const AxisConversionVerify  *found,
	GError                     **error
){
	const struct stat *st = &found->output;
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	g_autofree gchar *ino = axis_conversion_int_text((gint64)st->st_ino);
	g_autofree gchar *size = axis_conversion_int_text((gint64)st->st_size);
	g_autofree gchar *ms = axis_conversion_int_text((gint64)st->st_mtim.tv_sec);
	g_autofree gchar *mn = axis_conversion_int_text((gint64)st->st_mtim.tv_nsec);
	g_autofree gchar *cs = axis_conversion_int_text((gint64)st->st_ctim.tv_sec);
	g_autofree gchar *cn = axis_conversion_int_text((gint64)st->st_ctim.tv_nsec);
	const gchar *params[9];

	if (found->output_sample == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY,
			"no digest of the converted file was made");
		return FALSE;
	}
	params[0] = id;
	params[1] = ino;
	params[2] = size;
	params[3] = ms;
	params[4] = mn;
	params[5] = cs;
	params[6] = cn;
	params[7] = found->output_sample;
	params[8] = found->output_digest;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET passed_at = now(), passed_inode = $2::bigint,"
	        " passed_size = $3::bigint, passed_mtime_sec = $4::bigint, passed_mtime_nsec = $5::integer,"
	        " passed_ctime_sec = $6::bigint, passed_ctime_nsec = $7::integer, passed_sample = $8, passed_digest = $9,"
	        " checks_version = " G_STRINGIFY(AXIS_CONVERSION_CHECKS_VERSION) ", updated_at = now() WHERE id = $1::bigint",
	        9, params, NULL, error))
		return FALSE;
	row->passed = TRUE;
	return TRUE;
}

/* The output is not the file whose checks passed: forgotten, so that it is
 * checked from the start. */
static gboolean
forget_passed(
	AxisConversions    *self,
	AxisConversionRow  *row,
	GError            **error
){
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	const gchar *params[1];

	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET passed_at = NULL, passed_inode = NULL, passed_size = NULL,"
	        " passed_mtime_sec = NULL, passed_mtime_nsec = NULL, passed_ctime_sec = NULL, passed_ctime_nsec = NULL,"
	        " passed_sample = NULL, passed_digest = NULL, updated_at = now() WHERE id = $1::bigint", 1, params, NULL,
	        error))
		return FALSE;
	row->passed = FALSE;
	return TRUE;
}

/* A pass judged by a rule found too lenient since (axis-conversion.h, the
 * marks): forgotten, with the measurements it was judged from, so that the
 * file is checked from the start: measured again (here or by a worker, as
 * convert.checks_where says), not judged again from what a worker measured,
 * whose binding to the file as it was handed out settling has changed. */
static gboolean
forget_voided_pass(
	AxisConversions           *self,
	AxisConversionRow         *row,
	const AxisConversionMark  *mark,
	GError                   **error
){
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	const gchar *params[1];

	g_message("conversions: conversion %" G_GINT64_FORMAT " passed its checks by version %d of their rules, and check %u "
		"was found too lenient before version %d (%s): it is checked again from the start", row->id,
		row->checks_version, mark->check, mark->before, mark->why != NULL ? mark->why : "?");
	if (!forget_passed(self, row, error))
		return FALSE;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET checked_where = NULL, verify_job_id = NULL, verifier = NULL,"
	        " check_plan = NULL, check_source_sample = NULL, handed_inode = NULL, handed_size = NULL,"
	        " handed_mtime_sec = NULL, handed_mtime_nsec = NULL, handed_ctime_sec = NULL, handed_ctime_nsec = NULL,"
	        " evidence = NULL, evidence_at = NULL, updated_at = now() WHERE id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	g_clear_pointer(&row->checked_where, g_free);
	g_clear_pointer(&row->verifier, g_free);
	g_clear_pointer(&row->check_plan, g_free);
	g_clear_pointer(&row->check_source_sample, g_free);
	g_clear_pointer(&row->evidence, g_free);
	row->verify_job_id = 0;
	row->handed = FALSE;
	return TRUE;
}

/* The converted file was not judged (the plan cannot be made again as the
 * worker was given it): not a rejection by a check, and not the file's or the
 * revision's fault.  The output goes, the original stays, and the conversion
 * ends saying that asking for it again converts it again (retryable). */
static gboolean
not_judged(
	AxisConversions             *self,
	AxisConversionRow           *row,
	AxisVolumeGuard             *guard,
	gint                         work,
	const AxisConversionVerify  *found,
	GError                     **error
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *text = g_strdup_printf("Not judged: %s. This is not a rejection by a check: the converted file "
		"was removed unjudged, the original is as it was, and asking for it again converts it again.", found->not_judged);
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;

	if (work >= 0 && found->output.st_ino != 0 &&
	    !axis_volume_write_unlink_work(guard, work, row->output_name, (guint64)found->output.st_ino, NULL, &local))
		g_message("conversions: the unjudged output of conversion %" G_GINT64_FORMAT " stays for the tidy job: %s",
			row->id, local->message);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	ok = axis_db_conn_begin(conn, error) && axis_conversion_set_state(conn, row->id, "failed", 0, text, error) &&
		axis_db_conn_exec(conn, "UPDATE conversion SET retryable = true, checks_waiting = NULL WHERE id = $1::bigint", 1,
			params, NULL, error) && axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		g_message("conversions: conversion %" G_GINT64_FORMAT " not judged: %s", row->id, found->not_judged);
	return ok;
}

/* ── The trial ──────────────────────────────────────────────────── */

static gboolean
keep_trial(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	gint                work,
	GError            **error
){
	g_autofree gchar *name = g_strdup_printf("trial-%" G_GINT64_FORMAT ".%s", row->id, row->container);
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	g_autofree gchar *keep = axis_conversion_int_text(axis_conversion_setting_integer(self,
		AXIS_CONVERSION_SETTING_TRIAL_RETENTION, 14 * 86400));
	const gchar *params[3];
	struct stat st;
	GError *local = NULL;

	if (axis_conversion_hook(self, "trial", row->id) != AXIS_CONVERSION_HOOK_CONTINUE) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Stopped before the trial's "
			"kept name (a test).");
		return FALSE;
	}
	/* The name first: a run that stops after the rename finds it. */
	params[0] = id;
	params[1] = name;
	params[2] = keep;
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET state = 'placing', step = 'placing', trial_name = $2,"
	        " updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	if (!(fstatat(work, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
	      (gint64)st.st_ino == row->output_inode)) {
		if (fstatat(work, row->output_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || (gint64)st.st_ino != row->output_inode)
			return pool_state(self, row->id, "failed", 0, "The trial's file is gone from the work area.", error);
		if (!axis_volume_write_rename_work(guard, work, row->output_name, (guint64)row->output_inode, name, &local)) {
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET state = 'done', step = 'recorded', trial_state = 'kept',"
	        " trial_name = $2, trial_remove_after = now() + $3::bigint * interval '1 second', finished_at = now(),"
	        " updated_at = now(), result_text = 'Trial: the converted file passed every check and is kept in the work "
	        "area.' WHERE id = $1::bigint", 3, params, NULL, error))
		return FALSE;
	g_message("conversions: trial %" G_GINT64_FORMAT " passed every check; its file is kept as %s", row->id, name);
	return TRUE;
}

/* ── Where the checks are measured ──────────────────────────────── */

/* Why the checks wait now (NULL: they do not), on the record for the page. */
static gboolean
note_waiting(
	AxisConversions  *self,
	gint64            id,
	const gchar      *why,
	GError          **error
){
	g_autofree gchar *text = axis_conversion_int_text(id);
	g_autofree gchar *clean = why != NULL ? axis_library_text_clean(why, 500) : NULL;
	const gchar *params[2];

	params[0] = text;
	params[1] = clean != NULL && clean[0] != '\0' ? clean : NULL;
	return axis_db_pool_exec(self->pool, "UPDATE conversion SET checks_waiting = $2, updated_at = now()"
		" WHERE id = $1::bigint", 2, params, NULL, error);
}

/* Held after a restart (convert.wait_after_start): nothing is checked, here
 * or anywhere, until a person resumes conversion work.  *@held is TRUE when it
 * is; the conversion then says it waits. */
static gboolean
wait_if_held(
	AxisConversions          *self,
	const AxisConversionRow  *row,
	gboolean                 *held,
	GError                  **error
){
	g_autoptr(GDateTime) at = NULL;
	g_autofree gchar *when = NULL;
	g_autofree gchar *text = NULL;
	gint64 since = 0;

	*held = axis_conversions_is_held(self, &since);
	if (!*held)
		return TRUE;
	at = g_date_time_new_from_unix_utc(since / G_USEC_PER_SEC);
	when = at != NULL ? g_date_time_format(at, "%Y-%m-%d %H:%M UTC") : NULL;
	text = g_strdup_printf("Waiting: conversion work has been paused since Axis started (%s), because "
		"the setting \"Conversion work waits after a restart\" is on; resume it on the Conversions page.", when != NULL ? when : "?");
	return note_waiting(self, row->id, text, error);
}

/* How many workers could measure checks on @volume_id now: connected (heard
 * from in the last minute), of this protocol, neither revoked nor paused,
 * having proved the volume; other than @except (nullable). */
static gboolean
suitable_workers(
	AxisConversions  *self,
	gint64            volume_id,
	const gchar      *except,
	gint64           *count,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *volume = axis_conversion_int_text(volume_id);
	g_autofree gchar *seen = axis_conversion_int_text(AXIS_CONVERSION_WORKER_SEEN_S);
	const gchar *params[3];

	*count = 0;
	params[0] = volume;
	params[1] = except;
	params[2] = seen;
	if (!axis_db_pool_exec(self->pool, "SELECT count(DISTINCT w.id) FROM worker w"
	        " JOIN worker_volume wv ON wv.worker_id = w.id WHERE wv.volume_id = $1::bigint AND w.revoked_at IS NULL"
	        " AND w.paused_at IS NULL AND w.refused_protocol IS NULL AND w.registered_at IS NOT NULL"
	        " AND w.last_seen_at > now() - $3::bigint * interval '1 second' AND ($2::text IS NULL OR w.name <> $2::text)",
	        3, params, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, count);
	return TRUE;
}

/* The check job's state and last error ("gone" when there is none). */
static gboolean
verify_job_state(
	AxisConversions  *self,
	gint64            job,
	gchar           **state,
	gchar           **last_error,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = axis_conversion_int_text(job);
	const gchar *params[1];

	*state = NULL;
	*last_error = NULL;
	params[0] = text;
	if (!axis_db_pool_exec(self->pool, "SELECT state::text, last_error FROM job WHERE id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		*state = g_strdup("gone");
		return TRUE;
	}
	*state = g_strdup(axis_db_result_get_text(result, 0, 0));
	if (!axis_db_result_is_null(result, 0, 1))
		*last_error = g_strdup(axis_db_result_get_text(result, 0, 1));
	return TRUE;
}

/* The conversion fails without a check having failed (no measurements, or
 * measurements not of these files): the output goes, the original stays. */
static gboolean
fail_checks(
	AxisConversions    *self,
	AxisConversionRow  *row,
	AxisVolumeGuard    *guard,
	gint                work,
	guint64             inode,
	const gchar        *text,
	GError            **error
){
	g_autoptr(GError) local = NULL;

	if (work >= 0 && inode != 0 && row->output_name != NULL &&
	    !axis_volume_write_unlink_work(guard, work, row->output_name, inode, NULL, &local))
		g_message("conversions: the output of conversion %" G_GINT64_FORMAT " stays for the tidy job: %s", row->id,
			local->message);
	if (!note_waiting(self, row->id, NULL, error))
		return FALSE;
	g_message("conversions: conversion %" G_GINT64_FORMAT " failed: %s", row->id, text);
	return pool_state(self, row->id, "failed", 0, text, error);
}

/*
 * The checks handed to a worker: a convert.verify job whose description is
 * kept with the conversion (the plan, the original's digest), with the
 * converted file's stamp as it is now.  The job is offered to other workers
 * first for a while when another suitable one is connected.
 */
static gboolean
hand_out(
	AxisConversions             *self,
	AxisConversionRow           *row,
	const AxisConversionVerify  *found,
	const gchar                 *waiting,
	GError                     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(JsonObject) requires = json_object_new();
	g_autoptr(JsonArray) volumes = json_array_new();
	g_autoptr(AxisDbResult) uuid = NULL;
	g_autofree gchar *key = g_strdup_printf(AXIS_WORKER_KIND_VERIFY ":%" G_GINT64_FORMAT, row->id);
	g_autofree gchar *id = axis_conversion_int_text(row->id);
	g_autofree gchar *volume = axis_conversion_int_text(row->volume_id);
	g_autofree gchar *plan = axis_verify_plan_to_json(found->plan);
	g_autofree gchar *job_text = NULL;
	g_autofree gchar *ino = axis_conversion_int_text((gint64)found->output.st_ino);
	g_autofree gchar *size = axis_conversion_int_text((gint64)found->output.st_size);
	g_autofree gchar *ms = axis_conversion_int_text((gint64)found->output.st_mtim.tv_sec);
	g_autofree gchar *mn = axis_conversion_int_text((gint64)found->output.st_mtim.tv_nsec);
	g_autofree gchar *cs = axis_conversion_int_text((gint64)found->output.st_ctim.tv_sec);
	g_autofree gchar *cn = axis_conversion_int_text((gint64)found->output.st_ctim.tv_nsec);
	const gchar *params[11];
	AxisJobSpec spec;
	AxisDbConn *conn;
	gint64 others = 0;
	gint64 job = 0;

	params[0] = volume;
	if (!axis_db_pool_exec(self->pool, "SELECT uuid::text FROM volume WHERE id = $1::bigint", 1, params, &uuid, error))
		return FALSE;
	if (axis_db_result_get_n_rows(uuid) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "the volume is not known now");
		return FALSE;
	}
	/* Another worker than the one that converted it, when one could. */
	if (row->worker != NULL && !suitable_workers(self, row->volume_id, row->worker, &others, error))
		return FALSE;
	json_array_add_string_element(volumes, axis_db_result_get_text(uuid, 0, 0));
	json_object_set_array_member(requires, "volumes", json_array_ref(volumes));
	if (others > 0 && row->worker != NULL) {
		json_object_set_string_member(requires, "prefer_not", row->worker);
		json_object_set_int_member(requires, "prefer_not_until", g_get_real_time() / G_USEC_PER_SEC +
			AXIS_CONVERSION_PREFER_OTHER_S);
	}
	json_object_set_int_member(payload, "conversion", row->id);
	json_object_set_object_member(payload, "requires", json_object_ref(requires));

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto fail;
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_WORKER_KIND_VERIFY;
	spec.queue = AXIS_WORKER_QUEUE;
	spec.payload = payload;
	spec.dedupe_key = key;
	spec.max_attempts = AXIS_CONVERSION_VERIFY_ATTEMPTS;
	if (!axis_job_enqueue(conn, &spec, &job, NULL, error))
		goto rollback;
	job_text = axis_conversion_int_text(job);
	params[0] = id;
	params[1] = job_text;
	params[2] = plan;
	params[3] = found->source_sample;
	params[4] = ino;
	params[5] = size;
	params[6] = ms;
	params[7] = mn;
	params[8] = cs;
	params[9] = cn;
	params[10] = waiting;
	if (!axis_db_conn_exec(conn, "UPDATE conversion SET checked_where = 'worker', verify_job_id = $2::bigint,"
	        " verifier = NULL, check_plan = $3, check_source_sample = $4, handed_inode = $5::bigint,"
	        " handed_size = $6::bigint, handed_mtime_sec = $7::bigint, handed_mtime_nsec = $8::integer,"
	        " handed_ctime_sec = $9::bigint, handed_ctime_nsec = $10::integer, evidence = NULL, evidence_at = NULL,"
	        " checks_waiting = $11, updated_at = now() WHERE id = $1::bigint", 11, params, NULL, error))
		goto rollback;
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	axis_db_pool_release(self->pool, conn);
	g_message("conversions: the checks of conversion %" G_GINT64_FORMAT " are handed to a worker (job %"
		G_GINT64_FORMAT "%s)", row->id, job, others > 0 ? ", offered to another worker than the one that converted it "
		"first" : "");
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

/* The worker's measurements kept with the conversion, read again (strictly:
 * they were written by this daemon after it read them strictly). */
static AxisVerifyResult *
stored_evidence(
	const AxisConversionRow  *row,
	gchar                   **problem
){
	if (row->evidence == NULL)
		return NULL;
	return axis_verify_result_parse(row->evidence, strlen(row->evidence), problem);
}

/* ── The finish job ─────────────────────────────────────────────── */

static gboolean
busy(
	GError      **error,
	const GError *why
){
	g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Nothing concluded; tried again later: %s",
		why != NULL ? why->message : "?");
	return FALSE;
}

/*
 * Where the checks of a conversion being checked are measured now, and in
 * which mode they run: *@mode, and the worker's measurements in *@result for
 * JUDGE.  FALSE with *@done when there is nothing to do now (they wait, or
 * the conversion ended), and with @error for a database failure.
 */
static gboolean
decide(
	AxisConversions           *self,
	AxisConversionRow         *row,
	AxisVolumeGuard           *guard,
	gint                       work,
	AxisConversionCheckMode   *mode,
	AxisVerifyResult         **result,
	gboolean                  *done,
	GError                   **error
){
	AxisConversionChecksWhere where = axis_conversions_checks_where(self);
	gint64 workers = 0;

	*done = FALSE;
	*result = NULL;
	*mode = AXIS_CONVERSION_CHECK_ALL;
	if (row->evidence != NULL && g_strcmp0(row->checked_where, "worker") == 0) {
		g_autofree gchar *problem = NULL;

		*result = stored_evidence(row, &problem);
		if (*result == NULL) {
			g_autofree gchar *text = g_strdup_printf("The measurements made on a worker that this server kept cannot "
				"be read again (%s): nothing was placed; the converted file was removed and the original is as it "
				"was.", problem != NULL ? problem : "?");

			*done = TRUE;
			return fail_checks(self, row, guard, work, (guint64)row->handed_inode, text, error);
		}
		*mode = AXIS_CONVERSION_CHECK_JUDGE;
		return TRUE;
	}
	if (row->verify_job_id > 0 && g_strcmp0(row->checked_where, "worker") == 0) {
		g_autofree gchar *state = NULL;
		g_autofree gchar *last = NULL;

		if (!verify_job_state(self, row->verify_job_id, &state, &last, error))
			return FALSE;
		if (g_strcmp0(state, "queued") == 0 || g_strcmp0(state, "running") == 0) {
			/* A job no worker took yet, where this server may measure: when
			 * no worker could take it now (or checks run here again), it is
			 * measured here instead. */
			if (g_strcmp0(state, "queued") == 0 && where != AXIS_CONVERSION_CHECKS_WORKER_ONLY) {
				if (where == AXIS_CONVERSION_CHECKS_WORKER &&
				    !suitable_workers(self, row->volume_id, NULL, &workers, error))
					return FALSE;
				if (where == AXIS_CONVERSION_CHECKS_SERVER || workers == 0) {
					GError *local = NULL;
					AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);

					if (conn == NULL)
						return FALSE;
					if (!axis_job_cancel(conn, row->verify_job_id, NULL, &local) &&
					    !g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND)) {
						axis_db_pool_release(self->pool, conn);
						g_propagate_error(error, local);
						return FALSE;
					}
					g_clear_error(&local);
					axis_db_pool_release(self->pool, conn);
					g_message("conversions: no worker took the checks of conversion %" G_GINT64_FORMAT
						" and none can now: this server measures them", row->id);
					return TRUE;
				}
			}
			*done = TRUE;
			if (where == AXIS_CONVERSION_CHECKS_WORKER_ONLY && g_strcmp0(state, "queued") == 0 &&
			    !suitable_workers(self, row->volume_id, NULL, &workers, error))
				return FALSE;
			return note_waiting(self, row->id, g_strcmp0(state, "running") == 0 ? NULL :
				where == AXIS_CONVERSION_CHECKS_WORKER_ONLY && workers == 0 ?
				"Waiting for a worker to check it: no connected worker can reach its disk, and this server does "
				"no heavy work (Settings, \"Where converted files are checked\" is \"Only on a worker\")." :
				"Waiting for a worker to check it.", error);
		}
		/* The job ended without measurements (its attempts used up, or
		 * cancelled): here, unless this server does no heavy work. */
		if (where == AXIS_CONVERSION_CHECKS_WORKER_ONLY) {
			g_autofree gchar *text = g_strdup_printf("The checks could not be measured on a worker: %s Nothing was "
				"placed; the converted file was removed and the original is as it was.", last != NULL ? last :
				"the check job ended without measurements.");

			*done = TRUE;
			return fail_checks(self, row, guard, work, (guint64)row->handed_inode, text, error);
		}
		g_message("conversions: the checks of conversion %" G_GINT64_FORMAT " ended on workers without measurements "
			"(%s): this server measures them", row->id, last != NULL ? last : state);
		return TRUE;
	}
	if (where == AXIS_CONVERSION_CHECKS_SERVER)
		return TRUE;
	if (where == AXIS_CONVERSION_CHECKS_WORKER) {
		if (!suitable_workers(self, row->volume_id, NULL, &workers, error))
			return FALSE;
		if (workers == 0)
			return TRUE;
	}
	*mode = AXIS_CONVERSION_CHECK_HAND_OUT;
	return TRUE;
}

gboolean
axis_conversions_finish(
	AxisConversions  *self,
	gint64            conversion_id,
	AxisJobContext   *context,
	GError          **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisVerifyResult) result = NULL;
	AxisConversionCheckMode mode = AXIS_CONVERSION_CHECK_ALL;
	AxisConversionVerify found;
	AxisDbConn *conn;
	gboolean done = FALSE;
	gboolean held = FALSE;
	guint failed = 0;
	gboolean ok = FALSE;
	gint work;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_row_load(conn, conversion_id, FALSE, &row, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (row == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such conversion.");
		return FALSE;
	}
	if (g_strcmp0(row->state, "verifying") != 0 && g_strcmp0(row->state, "placing") != 0)
		return TRUE;
	/* Held after a restart: nothing is checked, here or anywhere, until a
	 * person resumes conversion work (placement and its recovery go on, and
	 * so does a conversion whose checks all passed: only the light ones are
	 * made again before it is placed; one whose pass is lost below waits
	 * then). */
	if (g_strcmp0(row->state, "verifying") == 0 && !row->passed) {
		if (!wait_if_held(self, row, &held, error))
			return FALSE;
		if (held)
			return TRUE;
	}
	if (row->volume_id <= 0)
		return pool_state(self, row->id, "failed", 0, "The disk is no longer known to Axis: nothing was done; the "
			"work area and the folder must be looked at by a person.", error);
	guard = axis_volume_guard_acquire(self->storage, row->volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
	if (guard == NULL)
		return busy(error, local);

	if (g_strcmp0(row->state, "placing") == 0) {
		if (row->outcome == AXIS_CONVERSION_TRIAL) {
			work = axis_conversion_work_area(guard, FALSE, &local);
			if (work < 0)
				return busy(error, local);
			ok = keep_trial(self, row, guard, work, error);
			close(work);
			return ok;
		}
		return axis_conversion_recover(self, row, guard, error);
	}

	work = axis_conversion_work_area(guard, FALSE, &local);
	if (work < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			if (!axis_conversion_record_check_pool(self->pool, row->id, 1, "failed", "The disk has no work area: the "
			        "output is not there.", error))
				return FALSE;
			return pool_state(self, row->id, "rejected", 1, "Rejected by check 1, \"The converted file is the one "
				"asked for\": the disk has no work area, so the converted file is not there.", error);
		}
		return busy(error, local);
	}
	/* Every check passed in an earlier turn (it stopped before placing: a
	 * scan held the volume, a database error, a daemon that stopped): the
	 * same file is placed without being judged again; anything else is
	 * checked from the start, and so is a pass by a rule found too lenient
	 * since (the marks). */
	if (row->passed) {
		const AxisConversionMark *voided = axis_conversion_pass_voided(row->checks_version);

		if (voided != NULL) {
			if (!forget_voided_pass(self, row, voided, error)) {
				close(work);
				return FALSE;
			}
		} else {
			mode = AXIS_CONVERSION_CHECK_AGAIN;
			ok = axis_conversion_verify(self, row, guard, work, context, mode, NULL, &found, &failed, &local);
			if (!ok && failed == 0 && found.not_passed) {
				axis_conversion_verify_clear(&found);
				if (!forget_passed(self, row, error)) {
					close(work);
					return FALSE;
				}
			}
		}
		/* Its pass lost: checked from the start, which waits while
		 * conversion work is held, as any check does. */
		if (!row->passed) {
			if (!wait_if_held(self, row, &held, error)) {
				close(work);
				return FALSE;
			}
			if (held) {
				close(work);
				return TRUE;
			}
		}
	}
	if (!row->passed) {
		if (!decide(self, row, guard, work, &mode, &result, &done, error)) {
			close(work);
			return FALSE;
		}
		if (done) {
			close(work);
			return TRUE;
		}
		if (mode != AXIS_CONVERSION_CHECK_HAND_OUT) {
			g_autofree gchar *id = axis_conversion_int_text(row->id);
			const gchar *params[2];

			/* Measured here, or judged from a worker's measurements. */
			params[0] = id;
			params[1] = mode == AXIS_CONVERSION_CHECK_JUDGE ? "worker" : "server";
			if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET checked_where = $2, checks_waiting = NULL,"
			        " verifier = CASE WHEN $2 = 'server' THEN NULL ELSE verifier END, updated_at = now()"
			        " WHERE id = $1::bigint", 2, params, NULL, error)) {
				close(work);
				return FALSE;
			}
		}
		g_clear_error(&local);
		ok = axis_conversion_verify(self, row, guard, work, context, mode, result != NULL ? result->evidence : NULL,
			&found, &failed, &local);
	}
	if (!ok && failed == 0 && found.not_judged != NULL) {
		ok = not_judged(self, row, guard, work, &found, error);
		axis_conversion_verify_clear(&found);
		close(work);
		return ok;
	}
	if (ok && mode == AXIS_CONVERSION_CHECK_HAND_OUT) {
		gint64 workers = 0;
		gboolean none = axis_conversions_checks_where(self) == AXIS_CONVERSION_CHECKS_WORKER_ONLY &&
			suitable_workers(self, row->volume_id, NULL, &workers, NULL) && workers == 0;

		ok = hand_out(self, row, &found, none ? "Waiting for a worker to check it: no connected worker can reach its "
			"disk, and this server does no heavy work (Settings, \"Where converted files are checked\" is \"Only on a "
			"worker\")." :
			"Waiting for a worker to check it.", &local);
		axis_conversion_verify_clear(&found);
		close(work);
		if (!ok)
			return busy(error, local);
		return TRUE;
	}
	if (!ok && failed == 0 && found.unbound != NULL) {
		g_autofree gchar *text = g_strdup_printf("The measurements made on worker %s are not of this file: %s. Nothing "
			"was placed; the converted file was removed and the original is as it was.", row->verifier != NULL ?
			row->verifier : "?", found.unbound);

		ok = fail_checks(self, row, guard, work, (guint64)found.output.st_ino, text, error);
		axis_conversion_verify_clear(&found);
		close(work);
		return ok;
	}
	if (!ok && failed == 0) {
		axis_conversion_verify_clear(&found);
		close(work);
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		return busy(error, local);
	}
	if (!ok) {
		axis_conversion_verify_clear(&found);
		ok = reject(self, row, guard, work, failed, &found, error);
		close(work);
		return ok;
	}
	/* Recorded before anything changes the file. */
	if (mode != AXIS_CONVERSION_CHECK_AGAIN && !record_passed(self, row, &found, error)) {
		axis_conversion_verify_clear(&found);
		close(work);
		return FALSE;
	}
	axis_conversion_verify_clear(&found);

	if (row->outcome == AXIS_CONVERSION_TRIAL) {
		ok = record_output(self, row, &found.output, error) && keep_trial(self, row, guard, work, error);
		close(work);
		return ok;
	}

	/* The original's owner, mode and time, flushed; then its stamp is the
	 * one every step compares with. */
	{
		struct stat settled;
		gboolean any_owner = axis_conversion_setting_boolean(self, AXIS_CONVERSION_SETTING_OTHER_OWNER, FALSE);

		if (axis_conversion_hook(self, "settle", row->id) != AXIS_CONVERSION_HOOK_CONTINUE) {
			close(work);
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Stopped before settling "
				"(a test).");
			return FALSE;
		}
		if (!axis_volume_write_settle(guard, work, row->output_name, (guint64)found.output.st_ino,
		        (guint)found.source.st_mode, (guint)found.source.st_uid, (guint)found.source.st_gid,
		        (gint64)found.source.st_mtim.tv_sec, (gint)found.source.st_mtim.tv_nsec, !any_owner, &settled, &local)) {
			if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
				g_autoptr(GError) gone = NULL;

				if (!axis_volume_write_unlink_work(guard, work, row->output_name, (guint64)found.output.st_ino, NULL,
				        &gone))
					g_message("conversions: an output stays for the tidy job: %s", gone->message);
				close(work);
				return pool_state(self, row->id, "rejected", 0, "The new file's owner and group cannot be made the "
					"original's (a worker of another user; convert.allow_other_owner): nothing was replaced.", error);
			}
			close(work);
			return busy(error, local);
		}
		if (axis_conversion_hook(self, "settle-done", row->id) != AXIS_CONVERSION_HOOK_CONTINUE) {
			close(work);
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "Stopped after settling "
				"(a test).");
			return FALSE;
		}
		if (!record_output(self, row, &settled, error)) {
			close(work);
			return FALSE;
		}
	}
	ok = axis_conversion_place(self, row, guard, work, &found, error);
	close(work);
	return ok;
}

gboolean
axis_conversions_run_finish(
	AxisConversions  *self,
	AxisJobContext   *context,
	GError          **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	gint64 id = 0;

	if (payload != NULL && json_object_has_member(payload, "conversion")) {
		JsonNode *node = json_object_get_member(payload, "conversion");

		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
			id = json_node_get_int(node);
	}
	if (id <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The job names no conversion.");
		return FALSE;
	}
	return axis_conversions_finish(self, id, context, error);
}

/* ── Tidying ────────────────────────────────────────────────────── */

/* Conversions whose encode ended without a result: their state follows the
 * job's. */
static gboolean
reconcile(
	AxisConversions           *self,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT c.id, coalesce(j.state::text, 'gone'), j.last_error FROM conversion c"
	        " LEFT JOIN job j ON j.id = c.job_id WHERE c.state IN ('queued', 'encoding')"
	        " AND (j.id IS NULL OR j.state IN ('failed', 'cancelled', 'succeeded')) LIMIT 500", 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *state = axis_db_result_get_text(rows, i, 1);
		const gchar *why = axis_db_result_get_text(rows, i, 2);
		g_autofree gchar *text = NULL;
		gint64 id = 0;

		axis_db_result_get_int64(rows, i, 0, &id);
		if (g_strcmp0(state, "cancelled") == 0)
			text = g_strdup("The encode was cancelled.");
		else if (g_strcmp0(state, "failed") == 0)
			text = g_strdup_printf("The encode failed: %s", why != NULL ? why : "no reason was given.");
		else
			text = g_strdup("The encode ended without a result being recorded.");
		if (!pool_state(self, id, g_strcmp0(state, "cancelled") == 0 ? "cancelled" : "failed", 0, text, error))
			return FALSE;
		report->failed++;
	}
	return TRUE;
}

/* Conversions being encoded whose job is back in the queue (the worker's
 * lease ran out, or it gave the job back): the attempt ended without a
 * result, so the conversion waits for a worker again and says why, and the
 * attempt's output, if anything is left of it, is no longer accounted for
 * (a leftover).  The job's row is locked while it is looked at, so that a
 * worker claiming it now is not missed: a claim commits the job running
 * before the conversion is told of the new attempt. */
static gboolean
given_back(
	AxisConversions           *self,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;

	if (!axis_db_pool_exec(self->pool, "WITH back AS (SELECT j.id, j.attempts, j.max_attempts, j.last_error FROM job j"
	        " JOIN conversion c ON c.job_id = j.id WHERE c.state = 'encoding' AND j.state = 'queued' LIMIT 500"
	        " FOR UPDATE OF j)"
	        " UPDATE conversion c SET state = 'queued', updated_at = now(), result_text = left('Attempt ' ||"
	        "  coalesce(c.attempt, 0) || ' on ' || coalesce(c.worker, 'a worker') || ' ended without a result' ||"
	        "  coalesce(': ' || back.last_error, '') || '. It is tried again (' || back.attempts || ' of ' ||"
	        "  back.max_attempts || ' attempts used).', 2000)"
	        " FROM back WHERE c.job_id = back.id AND c.state = 'encoding' RETURNING c.id", 0, NULL, &rows, error))
		return FALSE;
	report->waiting += axis_db_result_get_n_rows(rows);
	return TRUE;
}

/* Conversions being checked or placed with no finish job queued or running
 * (a daemon that stopped, a job whose attempts ran out): one is queued. */
static gboolean
requeue(
	AxisConversions           *self,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	AxisDbConn *conn;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT c.id FROM conversion c WHERE c.state IN ('verifying', 'placing')"
	        " AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_CONVERSION_FINISH_KEY "' || c.id::text"
	        "  AND j.state IN ('queued', 'running')) LIMIT 500", 0, NULL, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 id = 0;

		axis_db_result_get_int64(rows, i, 0, &id);
		if (!axis_conversion_queue_finish(conn, id, 0, NULL, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
		report->queued++;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

/* Guards by volume, for one tidy run; a volume that cannot be had is
 * remembered as away. */
typedef struct
{
	AxisConversions *self;
	GHashTable      *guards;	/* gint64 -> AxisVolumeGuard */
	GHashTable      *away;	/* gint64 */
} Guards;

static void
guards_init(
	Guards          *g,
	AxisConversions *self
){
	g->self = self;
	g->guards = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)axis_volume_guard_release);
	g->away = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
}

static void
guards_clear(Guards *g)
{
	g_hash_table_unref(g->guards);
	g_hash_table_unref(g->away);
}

static AxisVolumeGuard *
guard_for(
	Guards *g,
	gint64  volume
){
	AxisVolumeGuard *guard;
	gint64 *key;

	if (volume <= 0 || g_hash_table_contains(g->away, &volume))
		return NULL;
	guard = g_hash_table_lookup(g->guards, &volume);
	if (guard != NULL)
		return guard;
	key = g_new(gint64, 1);
	*key = volume;
	guard = axis_volume_guard_acquire(g->self->storage, volume, AXIS_VOLUME_INTENT_WRITE, NULL);
	if (guard == NULL) {
		g_hash_table_add(g->away, key);
		return NULL;
	}
	g_hash_table_insert(g->guards, key, guard);
	return guard;
}

/* Trials and recycled originals whose time has come. */
static gboolean
expire(
	AxisConversions           *self,
	Guards                    *guards,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT c.id FROM conversion c WHERE c.state = 'done' AND"
	        " ((c.trial_state = 'kept' AND c.trial_remove_after <= now()) OR"
	        "  (c.recycle_state = 'kept' AND c.recycle_remove_after <= now())) ORDER BY c.id LIMIT 500", 0, NULL, &rows,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisConversionRow) row = NULL;
		AxisVolumeGuard *guard;
		AxisDbConn *conn;
		gint64 id = 0;
		gboolean ok;

		axis_db_result_get_int64(rows, i, 0, &id);
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL)
			return FALSE;
		ok = axis_conversion_row_load(conn, id, FALSE, &row, error);
		if (ok && row != NULL && (guard = guard_for(guards, row->volume_id)) != NULL) {
			GError *local = NULL;

			if (g_strcmp0(row->recycle_state, "kept") == 0) {
				gboolean removed = FALSE;

				if (!axis_conversion_remove_original(self, conn, row, guard, &removed, &local)) {
					g_message("conversions: a recycled original stays for now: %s", local->message);
					g_clear_error(&local);
				} else if (removed) {
					report->originals++;
				}
			}
			if (g_strcmp0(row->trial_state, "kept") == 0 && row->trial_name != NULL) {
				gint work = axis_conversion_work_area(guard, FALSE, &local);
				gboolean removed = FALSE;
				const gchar *params[2];
				g_autofree gchar *text = axis_conversion_int_text(row->id);

				/* The write layer proves the volume first; a refusal other than
				 * "not that file" is tried again at the next run. */
				if (work >= 0) {
					if (axis_volume_write_unlink_work(guard, work, row->trial_name, (guint64)row->output_inode, &removed,
					        &local)) {
						params[0] = text;
						params[1] = removed ? "removed" : "gone";
						ok = axis_db_conn_exec(conn, "UPDATE conversion SET trial_state = $2, updated_at = now()"
							" WHERE id = $1::bigint", 2, params, NULL, error);
						if (removed)
							report->trials++;
					} else if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED)) {
						params[0] = text;
						params[1] = "changed";
						ok = axis_db_conn_exec(conn, "UPDATE conversion SET trial_state = $2, updated_at = now()"
							" WHERE id = $1::bigint", 2, params, NULL, error);
					} else {
						g_message("conversions: a kept trial stays for now: %s", local->message);
					}
				}
				if (work >= 0)
					close(work);
				g_clear_error(&local);
			}
		}
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

/* Whether a conversion accounts for @name in its volume's work area: the
 * output of the attempt being encoded, checked or placed, or a kept
 * trial. */
static gboolean
accounted(
	AxisDbConn   *conn,
	gint64        volume,
	const gchar  *name,
	gboolean     *yes,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *volume_text = axis_conversion_int_text(volume);
	const gchar *params[2];

	*yes = FALSE;
	params[0] = volume_text;
	params[1] = name;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM conversion WHERE volume_id = $1::bigint AND"
	        " ((output_name = $2 AND state IN ('encoding', 'verifying', 'placing')) OR"
	        "  (trial_name = $2 AND (trial_state = 'kept' OR state = 'placing'))) LIMIT 1", 2, params, &result, error))
		return FALSE;
	*yes = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

/* One work area: what is there that nothing accounts for is listed, and
 * removed after the grace time when it is still the same file. */
static gboolean
leftovers_of(
	AxisConversions           *self,
	AxisVolumeGuard           *guard,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *volume = axis_conversion_int_text(axis_volume_guard_get_volume_id(guard));
	g_autofree gchar *grace = axis_conversion_int_text(axis_conversion_setting_integer(self,
		AXIS_CONVERSION_SETTING_LEFTOVER_GRACE, 86400));
	g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
	struct dirent *entry;
	AxisDbConn *conn;
	DIR *listing;
	gint work;
	gint fd;
	guint n = 0;
	guint i;

	work = axis_conversion_work_area(guard, FALSE, &local);
	if (work < 0)
		return TRUE;
	fd = openat(work, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	listing = fd >= 0 ? fdopendir(fd) : NULL;
	if (listing == NULL) {
		if (fd >= 0)
			close(fd);
		close(work);
		return TRUE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		closedir(listing);
		close(work);
		return FALSE;
	}
	while ((entry = readdir(listing)) != NULL && n < LEFTOVERS_PER_VOLUME) {
		g_autofree gchar *ino = NULL;
		g_autofree gchar *size = NULL;
		g_autofree gchar *stored = NULL;
		gboolean yes = FALSE;
		const gchar *params[5];
		struct stat st;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (fstatat(work, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (!accounted(conn, axis_volume_guard_get_volume_id(guard), entry->d_name, &yes, error))
			goto fail;
		if (yes)
			continue;
		n++;
		{
			GString *s = g_string_new(NULL);

			axis_library_name_encode(s, entry->d_name, strlen(entry->d_name));
			stored = g_string_free(s, FALSE);
		}
		g_ptr_array_add(seen, g_strdup(stored));
		ino = axis_conversion_int_text((gint64)st.st_ino);
		size = axis_conversion_int_text((gint64)st.st_size);
		params[0] = volume;
		params[1] = stored;
		params[2] = ino;
		params[3] = size;
		params[4] = S_ISREG(st.st_mode) && axis_volume_write_work_name_valid(entry->d_name) ? "true" : "false";
		/* First seen now, or again when it is another file than listed. */
		if (!axis_db_conn_exec(conn, "INSERT INTO conversion_leftover (volume_id, name, inode, size, ours)"
		        " VALUES ($1::bigint, $2, $3::bigint, $4::bigint, $5::boolean) ON CONFLICT (volume_id, name) DO UPDATE"
		        " SET last_seen_at = now(), size = EXCLUDED.size, ours = EXCLUDED.ours,"
		        "  first_seen_at = CASE WHEN conversion_leftover.inode = EXCLUDED.inode THEN conversion_leftover.first_seen_at"
		        "   ELSE now() END, inode = EXCLUDED.inode", 5, params, NULL, error))
			goto fail;
	}
	closedir(listing);
	listing = NULL;
	report->leftovers += n;

	/* What is not there any more is forgotten. */
	{
		GString *names = g_string_new("{");
		const gchar *params[2];

		for (i = 0; i < seen->len; i++) {
			const gchar *name = g_ptr_array_index(seen, i);
			const gchar *p;

			g_string_append(names, i > 0 ? ",\"" : "\"");
			for (p = name; *p != '\0'; p++) {
				if (*p == '"' || *p == '\\')
					g_string_append_c(names, '\\');
				g_string_append_c(names, *p);
			}
			g_string_append_c(names, '"');
		}
		g_string_append_c(names, '}');
		params[0] = volume;
		params[1] = names->str;
		if (!axis_db_conn_exec(conn, "DELETE FROM conversion_leftover WHERE volume_id = $1::bigint"
		        " AND NOT (name = ANY ($2::text[]))", 2, params, NULL, error)) {
			g_string_free(names, TRUE);
			goto fail;
		}
		g_string_free(names, TRUE);
	}

	/* Removed after the grace time: a name of Axis's, the same file, and
	 * still not accounted for; on a volume proven again just now. */
	if (!axis_volume_guard_check(guard, &local)) {
		g_message("conversions: no leftover is removed from volume %s now: %s", volume, local->message);
		axis_db_pool_release(self->pool, conn);
		close(work);
		return TRUE;
	}
	{
		g_autoptr(AxisDbResult) due = NULL;
		const gchar *params[2];

		params[0] = volume;
		params[1] = grace;
		if (!axis_db_conn_exec(conn, "SELECT name, inode FROM conversion_leftover WHERE volume_id = $1::bigint AND ours"
		        " AND first_seen_at <= now() - $2::bigint * interval '1 second' ORDER BY name LIMIT 500", 2, params, &due,
		        error))
			goto fail;
		for (i = 0; i < axis_db_result_get_n_rows(due); i++) {
			g_autoptr(GString) bytes = NULL;
			const gchar *stored = axis_db_result_get_text(due, i, 0);
			gboolean yes = TRUE;
			gboolean removed = FALSE;
			gint64 inode = 0;
			const gchar *more[2];
			GError *gone = NULL;

			axis_db_result_get_int64(due, i, 1, &inode);
			if (!axis_library_name_decode(stored, &bytes) || memchr(bytes->str, '\0', bytes->len) != NULL)
				continue;
			if (!accounted(conn, axis_volume_guard_get_volume_id(guard), bytes->str, &yes, error))
				goto fail;
			if (yes)
				continue;
			if (axis_conversion_hook(self, "leftover", 0) != AXIS_CONVERSION_HOOK_CONTINUE)
				continue;
			if (!axis_volume_write_unlink_work(guard, work, bytes->str, (guint64)inode, &removed, &gone)) {
				g_message("conversions: a leftover of the work area stays: %s", gone->message);
				g_clear_error(&gone);
				continue;
			}
			more[0] = volume;
			more[1] = stored;
			if (!axis_db_conn_exec(conn, "DELETE FROM conversion_leftover WHERE volume_id = $1::bigint AND name = $2", 2,
			        more, NULL, error))
				goto fail;
			if (removed) {
				report->removed++;
				g_message("conversions: removed a leftover of the work area of volume %s (after the grace time)",
					volume);
			}
		}
	}
	axis_db_pool_release(self->pool, conn);
	close(work);
	return TRUE;

fail:
	if (listing != NULL)
		closedir(listing);
	axis_db_pool_release(self->pool, conn);
	close(work);
	return FALSE;
}

static gboolean
leftovers(
	AxisConversions           *self,
	Guards                    *guards,
	AxisConversionTidyReport  *report,
	GError                   **error
){
	g_autoptr(GPtrArray) volumes = axis_storage_list_volumes(self->storage);
	guint i;

	for (i = 0; volumes != NULL && i < volumes->len; i++) {
		const AxisVolumeInfo *v = g_ptr_array_index(volumes, i);
		AxisVolumeGuard *guard;

		if (!axis_volume_state_is_usable(v->state))
			continue;
		guard = guard_for(guards, v->id);
		if (guard == NULL)
			continue;
		if (!leftovers_of(self, guard, report, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_conversions_tidy(
	AxisConversions           *self,
	AxisConversionTidyReport  *report_out,
	GError                   **error
){
	AxisConversionTidyReport report;
	Guards guards;
	gboolean ok;

	memset(&report, 0, sizeof(report));
	guards_init(&guards, self);
	ok = reconcile(self, &report, error) && given_back(self, &report, error) && requeue(self, &report, error) &&
		expire(self, &guards, &report, error) && leftovers(self, &guards, &report, error);
	guards_clear(&guards);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}

gboolean
axis_conversions_run_tidy(
	AxisConversions  *self,
	AxisJobContext   *context,
	GError          **error
){
	AxisConversionTidyReport report;

	if (!axis_conversions_tidy(self, &report, error))
		return FALSE;
	if (report.failed + report.waiting + report.queued + report.trials + report.originals + report.removed > 0)
		g_message("conversions: tidied: %u ended without a result, %u wait for a worker again, %u finish jobs queued, "
			"%u trials and %u originals removed after their time, %u leftovers removed (%u listed)", report.failed,
			report.waiting, report.queued, report.trials, report.originals, report.removed, report.leftovers);
	return TRUE;
}

/* ── A trial removed on request ─────────────────────────────────── */

gboolean
axis_conversions_remove_trial(
	AxisConversions  *self,
	gint64            conversion_id,
	const gchar      *user,
	GError          **error
){
	g_autoptr(AxisConversionRow) row = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id = axis_conversion_int_text(conversion_id);
	const gchar *params[2];
	gboolean removed = FALSE;
	AxisDbConn *conn;
	gboolean ok;
	gint work;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_conversion_row_load(conn, conversion_id, FALSE, &row, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (row == NULL || g_strcmp0(row->trial_state, "kept") != 0 || row->trial_name == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE, "There is no kept trial file.");
		return FALSE;
	}
	guard = axis_volume_guard_acquire(self->storage, row->volume_id, AXIS_VOLUME_INTENT_WRITE, &local);
	if (guard == NULL) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "%s", local->message);
		return FALSE;
	}
	work = axis_conversion_work_area(guard, FALSE, &local);
	if (work < 0) {
		g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_BUSY, "%s", local->message);
		return FALSE;
	}
	ok = axis_volume_write_unlink_work(guard, work, row->trial_name, (guint64)row->output_inode, &removed, error);
	close(work);
	if (!ok)
		return FALSE;
	params[0] = id;
	params[1] = removed ? "removed" : "gone";
	if (!axis_db_pool_exec(self->pool, "UPDATE conversion SET trial_state = $2, updated_at = now() WHERE id = $1::bigint",
	        2, params, NULL, error))
		return FALSE;
	g_message("audit: %s removed the trial file of conversion %" G_GINT64_FORMAT, user != NULL ? user : "?",
		conversion_id);
	return TRUE;
}
