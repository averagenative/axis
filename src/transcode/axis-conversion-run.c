/*
 * axis-conversion-run.c - conversion runs: the object, its words, making a
 * run, and what a person does with one (start, pause, resume, cancel, move
 * from trials to replacing, limits, taking files out of the list)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-conversion-run-private.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"

/* ── Words ──────────────────────────────────────────────────────── */

const gchar *
axis_conversion_run_scope_to_string(AxisConversionRunScope scope)
{
	switch (scope) {
	case AXIS_CONVERSION_RUN_TV:
		return "tv";
	case AXIS_CONVERSION_RUN_ROOTS:
		return "roots";
	case AXIS_CONVERSION_RUN_SERIES:
		return "series";
	default:
		return "movies";
	}
}

gboolean
axis_conversion_run_scope_from_string(
	const gchar            *text,
	AxisConversionRunScope *scope
){
	if (g_strcmp0(text, "movies") == 0)
		*scope = AXIS_CONVERSION_RUN_MOVIES;
	else if (g_strcmp0(text, "tv") == 0)
		*scope = AXIS_CONVERSION_RUN_TV;
	else if (g_strcmp0(text, "roots") == 0)
		*scope = AXIS_CONVERSION_RUN_ROOTS;
	else if (g_strcmp0(text, "series") == 0)
		*scope = AXIS_CONVERSION_RUN_SERIES;
	else
		return FALSE;
	return TRUE;
}

const gchar *
axis_conversion_run_state_label(const gchar *state)
{
	static const gchar *const states[][2] = {
		{ "listing", "Making the list" },
		{ "ready", "Ready to start" },
		{ "running", "Running" },
		{ "paused", "Paused" },
		{ "finished", "Finished" },
		{ "cancelled", "Cancelled" },
		{ "failed", "The list could not be made" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; states[i][0] != NULL; i++) {
		if (g_strcmp0(states[i][0], state) == 0)
			return states[i][1];
	}
	return "Unknown";
}

const gchar *
axis_conversion_run_reason_label(const gchar *reason)
{
	AxisEstimateReason estimated;

	if (g_strcmp0(reason, "not_worth_it") == 0)
		return "Remembered as not worth it";
	if (g_strcmp0(reason, "converted") == 0)
		return "Converted with this version already";
	if (g_strcmp0(reason, "in_flight") == 0)
		return "Another conversion of it is in flight";
	if (g_strcmp0(reason, "kept") == 0)
		return "Kept: Axis does not replace this file";
	if (axis_estimate_reason_from_string(reason, &estimated))
		return axis_estimate_reason_label(estimated);
	return "Other";
}

const gchar *
axis_conversion_run_reason_help(const gchar *reason)
{
	AxisEstimateReason estimated;

	if (g_strcmp0(reason, "not_worth_it") == 0)
		return "A conversion of it with this version was not smaller by the profile's minimum saving, so it is not "
			"offered again with this version.";
	if (g_strcmp0(reason, "converted") == 0)
		return "It replaced its original with this version already.";
	if (g_strcmp0(reason, "in_flight") == 0)
		return "Someone asked for a conversion of it that has not ended yet; plan a new run when it has.";
	if (g_strcmp0(reason, "kept") == 0)
		return "Let Axis upgrade it again on its links page to allow a conversion.";
	if (axis_estimate_reason_from_string(reason, &estimated))
		return axis_estimate_reason_help(estimated);
	return "";
}

/* ── The object ─────────────────────────────────────────────────── */

AxisConversionRuns *
axis_conversion_runs_new(AxisConversions *conversions)
{
	AxisConversionRuns *self;

	g_return_val_if_fail(conversions != NULL, NULL);

	self = g_new0(AxisConversionRuns, 1);
	self->refs = 1;
	self->conversions = axis_conversions_ref(conversions);
	g_mutex_init(&self->lock);
	return self;
}

AxisConversionRuns *
axis_conversion_runs_ref(AxisConversionRuns *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_conversion_runs_unref(AxisConversionRuns *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	axis_conversions_unref(self->conversions);
	g_mutex_clear(&self->lock);
	g_free(self);
}

AxisConversions *
axis_conversion_runs_get_conversions(AxisConversionRuns *self)
{
	return self->conversions;
}

void
axis_conversion_runs_set_now(
	AxisConversionRuns *self,
	gint64              now_us
){
	g_mutex_lock(&self->lock);
	self->now_us = now_us;
	g_mutex_unlock(&self->lock);
}

gint64
axis_conversion_run_now(AxisConversionRuns *self)
{
	gint64 now;

	g_mutex_lock(&self->lock);
	now = self->now_us;
	g_mutex_unlock(&self->lock);
	return now > 0 ? now : g_get_real_time();
}

gchar *
axis_conversion_run_profile_text(
	const gchar *name,
	gint         number
){
	return g_strdup_printf("%s, version %d", name != NULL ? name : "?", number);
}

gint
axis_conversion_run_minute_of_day(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(us / G_USEC_PER_SEC);

	if (when == NULL)
		return 0;
	return g_date_time_get_hour(when) * 60 + g_date_time_get_minute(when);
}

gboolean
axis_conversion_run_in_window(
	gint start,
	gint end,
	gint minute
){
	if (start < 0 || end < 0 || start == end)
		return TRUE;
	if (start < end)
		return minute >= start && minute < end;
	/* Past midnight: 22:00 to 06:00. */
	return minute >= start || minute < end;
}

gchar *
axis_conversion_run_clock_text(gint minute)
{
	return g_strdup_printf("%02d:%02d", CLAMP(minute, 0, 1439) / 60, CLAMP(minute, 0, 1439) % 60);
}

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

static void
refuse(
	GError     **error,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
refuse(
	GError     **error,
	const gchar *format,
	...
){
	va_list args;
	gchar *text;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED, text);
	g_free(text);
}

/* ── Caps ───────────────────────────────────────────────────────── */

gboolean
axis_conversion_run_cap_unit(
	AxisDbConn  *conn,
	gint64       run_id,
	gint64       cap_files,
	gint64       cap_bytes,
	gint        *cap_unit,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *files = int_text(MAX(cap_files, 0));
	g_autofree gchar *bytes = int_text(MAX(cap_bytes, 0));
	const gchar *params[3];

	*cap_unit = -1;
	if (cap_files <= 0 && cap_bytes <= 0)
		return TRUE;
	params[0] = id;
	params[1] = files;
	params[2] = bytes;
	/* The first unit at which a running total reaches a cap: the list ends
	 * after it, so that a series is kept whole. */
	if (!axis_db_conn_exec(conn, "SELECT coalesce(min(t.position), -1) FROM (SELECT u.position,"
	        " sum(u.files) OVER w AS files, sum(u.bytes - u.mid) OVER w AS saving FROM conversion_run_unit u"
	        " WHERE u.run_id = $1::bigint AND NOT u.removed WINDOW w AS (ORDER BY u.position)) t"
	        " WHERE ($2::bigint > 0 AND t.files >= $2::bigint) OR ($3::bigint > 0 AND t.saving >= $3::bigint)",
	        3, params, &result, error))
		return FALSE;
	*cap_unit = (gint)int_at(result, 0, 0);
	return TRUE;
}

/* ── Making a run ───────────────────────────────────────────────── */

/* What a scope is, for people; NULL and @error when it is none. */
static gchar *
scope_text(
	AxisDbConn              *conn,
	AxisConversionRunScope   scope,
	const gint64            *root_ids,
	guint                    n_roots,
	gint64                   title_id,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	switch (scope) {
	case AXIS_CONVERSION_RUN_MOVIES:
		return g_strdup("Every movie library folder");
	case AXIS_CONVERSION_RUN_TV:
		return g_strdup("Every TV library folder");
	case AXIS_CONVERSION_RUN_ROOTS: {
		GString *list = g_string_new("{");
		GString *names = g_string_new(NULL);
		g_autofree gchar *literal = NULL;
		const gchar *params[1];

		if (n_roots == 0 || n_roots > 64) {
			g_string_free(list, TRUE);
			g_string_free(names, TRUE);
			refuse(error, "Choose between one and 64 library folders.");
			return NULL;
		}
		for (i = 0; i < n_roots; i++)
			g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", root_ids[i]);
		g_string_append_c(list, '}');
		literal = g_string_free(list, FALSE);
		params[0] = literal;
		if (!axis_db_conn_exec(conn, "SELECT name, media_type IN ('movies', 'tv', 'other') FROM library_root"
		        " WHERE id = ANY($1::bigint[]) ORDER BY name", 1, params, &result, error)) {
			g_string_free(names, TRUE);
			return NULL;
		}
		if (axis_db_result_get_n_rows(result) != n_roots) {
			g_string_free(names, TRUE);
			refuse(error, "One of the library folders chosen is not there.");
			return NULL;
		}
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			gboolean video = FALSE;

			axis_db_result_get_boolean(result, i, 1, &video);
			if (!video) {
				g_string_free(names, TRUE);
				refuse(error, "%s is not a movie, TV or other video library folder.", axis_db_result_get_text(result, i, 0));
				return NULL;
			}
			g_string_append_printf(names, "%s%s", i > 0 ? ", " : "", axis_db_result_get_text(result, i, 0));
		}
		{
			gchar *text = g_strdup_printf("%s %s", n_roots == 1 ? "The library folder" : "The library folders", names->str);

			g_string_free(names, TRUE);
			return text;
		}
	}
	case AXIS_CONVERSION_RUN_SERIES: {
		g_autofree gchar *id = int_text(title_id);
		const gchar *params[1];

		params[0] = id;
		if (!axis_db_conn_exec(conn, "SELECT t.title, t.year FROM meta_title t WHERE t.id = $1::bigint"
		        " AND t.kind = 'series' AND EXISTS (SELECT 1 FROM library_identity li JOIN library_root r ON r.id = li.root_id"
		        "  WHERE li.title_id = t.id AND li.state = 'linked' AND r.media_type = 'tv')", 1, params, &result, error))
			return NULL;
		if (axis_db_result_get_n_rows(result) != 1) {
			refuse(error, "There is no such series on disk.");
			return NULL;
		}
		if (axis_db_result_is_null(result, 0, 1))
			return g_strdup_printf("The series %s", axis_db_result_get_text(result, 0, 0));
		return g_strdup_printf("The series %s (%s)", axis_db_result_get_text(result, 0, 0),
			axis_db_result_get_text(result, 0, 1));
	}
	default:
		refuse(error, "There is no such scope.");
		return NULL;
	}
}

gboolean
axis_conversion_runs_create(
	AxisConversionRuns     *self,
	AxisConversionRunScope  scope,
	const gint64           *root_ids,
	guint                   n_roots,
	gint64                  title_id,
	gint64                  revision_id,
	const gchar            *user,
	gint64                 *run_id,
	GError                **error
){
	return axis_conversion_runs_create_full(self, scope, root_ids, n_roots, title_id, revision_id, user, run_id, NULL,
		error);
}

gboolean
axis_conversion_runs_create_full(
	AxisConversionRuns     *self,
	AxisConversionRunScope  scope,
	const gint64           *root_ids,
	guint                   n_roots,
	gint64                  title_id,
	gint64                  revision_id,
	const gchar            *user,
	gint64                 *run_id,
	gboolean               *created,
	GError                **error
){
	g_autoptr(AxisProfileRevision) revision = NULL;
	g_autoptr(AxisDbResult) inserted = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *text = NULL;
	g_autofree gchar *roots = NULL;
	g_autofree gchar *rev = int_text(revision_id);
	g_autofree gchar *title = scope == AXIS_CONVERSION_RUN_SERIES ? int_text(title_id) : NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *job_text = NULL;
	const gchar *params[6];
	AxisJobSpec spec;
	AxisDbConn *conn;
	gint64 job = 0;
	gboolean ok = FALSE;

	*run_id = 0;
	if (user == NULL || user[0] == '\0') {
		refuse(error, "A conversion run needs a person.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_profile_store_revision_by_id(conn, revision_id, &revision, error))
		goto out;
	if (revision == NULL) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND,
			"There is no such profile version.");
		goto out;
	}
	if (!revision->usable || revision->kind != AXIS_PROFILE_KIND_VIDEO) {
		refuse(error, "%s cannot convert video: %s", revision->name,
			revision->unusable != NULL ? revision->unusable : "it is an audio profile.");
		goto out;
	}
	text = scope_text(conn, scope, root_ids, n_roots, title_id, error);
	if (text == NULL)
		goto out;
	if (scope == AXIS_CONVERSION_RUN_ROOTS) {
		GString *list = g_string_new("{");
		guint i;

		for (i = 0; i < n_roots; i++)
			g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", root_ids[i]);
		g_string_append_c(list, '}');
		roots = g_string_free(list, FALSE);
	}
	if (!axis_db_conn_begin(conn, error))
		goto out;
	params[0] = rev;
	params[1] = axis_conversion_run_scope_to_string(scope);
	params[2] = roots;
	params[3] = title;
	params[4] = text;
	params[5] = user;
	/* A person's (created asked for): the same selection with the same
	 * revision, not started yet, is that run, not another (a second click
	 * of Make the run).  A later run beside a started one stays possible:
	 * it skips what the started one holds. */
	if (created != NULL) {
		g_autoptr(AxisDbResult) same = NULL;

		if (!axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(hashtext('conversion_run:' || $1))", 1, params,
		        NULL, error) ||
		    !axis_db_conn_exec(conn, "SELECT id FROM conversion_run WHERE revision_id = $1::bigint AND scope = $2"
		        " AND (SELECT array_agg(DISTINCT r ORDER BY r) FROM unnest(root_ids) r)"
		        "   IS NOT DISTINCT FROM (SELECT array_agg(DISTINCT r ORDER BY r) FROM unnest($3::bigint[]) r)"
		        " AND title_id IS NOT DISTINCT FROM $4::bigint"
		        " AND state IN ('listing', 'ready') ORDER BY id DESC LIMIT 1", 4, params, &same, error))
			goto rollback;
		if (axis_db_result_get_n_rows(same) > 0) {
			axis_db_result_get_int64(same, 0, 0, run_id);
			axis_db_conn_rollback(conn, NULL);
			if (created != NULL)
				*created = FALSE;
			ok = TRUE;
			goto out;
		}
	}
	if (!axis_db_conn_exec(conn, "INSERT INTO conversion_run (revision_id, scope, root_ids, title_id, scope_text,"
	        " created_by) VALUES ($1::bigint, $2, $3::bigint[], $4::bigint, left($5, 500), $6) RETURNING id", 6, params,
	        &inserted, error))
		goto rollback;
	axis_db_result_get_int64(inserted, 0, 0, run_id);
	id_text = int_text(*run_id);
	key = g_strdup_printf(AXIS_CONVERSION_RUN_LIST_KIND ":%" G_GINT64_FORMAT, *run_id);
	json_object_set_int_member(payload, "run", *run_id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_CONVERSION_RUN_LIST_KIND;
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.payload = payload;
	spec.dedupe_key = key;
	spec.max_attempts = 1;
	if (!axis_job_enqueue(conn, &spec, &job, NULL, error))
		goto rollback;
	job_text = int_text(job);
	params[0] = id_text;
	params[1] = job_text;
	if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET list_job_id = $2::bigint WHERE id = $1::bigint", 2, params,
	        NULL, error) || !axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s made conversion run %" G_GINT64_FORMAT " (%s, %s, revision %d)", user, *run_id, text,
		revision->name, revision->number);
	if (created != NULL)
		*created = TRUE;
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
	*run_id = 0;
out:
	axis_db_pool_release(self->conversions->pool, conn);
	return ok;
}

gboolean
axis_conversion_runs_run_list_job(
	AxisConversionRuns  *self,
	AxisJobContext      *context,
	GError             **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	gint64 run = payload != NULL ? json_object_get_int_member_with_default(payload, "run", 0) : 0;

	if (run <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "the job names no conversion run");
		return FALSE;
	}
	return axis_conversion_runs_build_list(self, run, context, error);
}

gboolean
axis_conversion_runs_request_step(
	AxisConversionRuns  *self,
	GError             **error
){
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean ok;

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_CONVERSION_RUN_STEP_KIND;
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.dedupe_key = "schedule:" AXIS_CONVERSION_RUN_STEP_SCHEDULE;
	spec.max_attempts = 1;
	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_job_enqueue(conn, &spec, NULL, NULL, error);
	axis_db_pool_release(self->conversions->pool, conn);
	return ok;
}

/* ── What a person does ─────────────────────────────────────────── */

/* A run's row as the actions need it, locked. */
typedef struct
{
	gint64  id;
	gint64  revision_id;
	gchar  *state;
	gchar  *outcome;
	gchar  *pause_code;
	gint64  cap_files;
	gint64  cap_bytes;
	gint64  list_job_id;
} RunRow;

static void
run_row_clear(RunRow *row)
{
	g_free(row->state);
	g_free(row->outcome);
	g_free(row->pause_code);
	memset(row, 0, sizeof(*row));
}

static gboolean
lock_run(
	AxisDbConn  *conn,
	gint64       run_id,
	RunRow      *row,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[1];

	memset(row, 0, sizeof(*row));
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT id, revision_id, state, outcome, pause_code, coalesce(cap_files, 0),"
	        " coalesce(cap_bytes, 0), coalesce(list_job_id, 0) FROM conversion_run WHERE id = $1::bigint FOR UPDATE", 1,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such conversion run.");
		return FALSE;
	}
	row->id = int_at(result, 0, 0);
	row->revision_id = int_at(result, 0, 1);
	row->state = g_strdup(axis_db_result_get_text(result, 0, 2));
	row->outcome = g_strdup(axis_db_result_get_text(result, 0, 3));
	row->pause_code = axis_db_result_is_null(result, 0, 4) ? NULL : g_strdup(axis_db_result_get_text(result, 0, 4));
	row->cap_files = int_at(result, 0, 5);
	row->cap_bytes = int_at(result, 0, 6);
	row->list_job_id = int_at(result, 0, 7);
	return TRUE;
}

static gboolean
state_is(
	const RunRow *row,
	const gchar  *a,
	const gchar  *b,
	const gchar  *c
){
	return g_strcmp0(row->state, a) == 0 || (b != NULL && g_strcmp0(row->state, b) == 0) ||
		(c != NULL && g_strcmp0(row->state, c) == 0);
}

static void
wrong_state(
	GError      **error,
	const RunRow *row,
	const gchar  *what
){
	g_autofree gchar *state = g_ascii_strdown(axis_conversion_run_state_label(row->state), -1);

	g_set_error(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE, "The run (%s) cannot be %s.", state, what);
}

/* The other run that is running, or 0. */
static gboolean
other_running(
	AxisDbConn  *conn,
	gint64       run_id,
	gint64      *other,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[1];

	*other = 0;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT id FROM conversion_run WHERE state = 'running' AND id <> $1::bigint LIMIT 1", 1,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*other = int_at(result, 0, 0);
	return TRUE;
}

/* Whether @revision may replace files now: proven and its profile's
 * current one (the gates check it again for every file). */
static gboolean
revision_may_replace(
	AxisDbConn  *conn,
	gint64       revision_id,
	gchar      **why_not,
	GError     **error
){
	g_autoptr(AxisProfileRevision) revision = NULL;

	*why_not = NULL;
	if (!axis_profile_store_revision_by_id(conn, revision_id, &revision, error))
		return FALSE;
	if (revision == NULL || !revision->usable)
		*why_not = g_strdup("The run's profile version is not usable any more.");
	else if (!axis_profile_revision_is_unlocked(revision))
		*why_not = g_strdup_printf("%s, version %d, is locked: prove it on the Conversions page (Review and prove "
			"this version) from passed trials first; a run with it can make trials.", revision->name, revision->number);
	else if (!revision->current)
		*why_not = g_strdup_printf("%s, version %d, is not its profile's current version: the playback "
			"verdict is for the current one only.", revision->name, revision->number);
	return TRUE;
}

gboolean
axis_conversion_runs_start(
	AxisConversionRuns     *self,
	gint64                  run_id,
	AxisConversionOutcome   outcome,
	gboolean                confirmed,
	const gchar            *user,
	GError                **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *why = NULL;
	g_autofree gchar *cap = NULL;
	RunRow row;
	AxisDbConn *conn;
	gint64 other = 0;
	gint cap_unit = -1;
	gboolean replace = outcome == AXIS_CONVERSION_REPLACE;
	gboolean ok = FALSE;

	if (user == NULL || user[0] == '\0') {
		refuse(error, "A conversion run needs a person.");
		return FALSE;
	}
	if (replace && !confirmed) {
		refuse(error, "Replacing originals deletes them: it needs the confirmation ticked.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "ready", NULL, NULL)) {
		wrong_state(error, &row, "started");
		goto rollback;
	}
	if (!other_running(conn, run_id, &other, error))
		goto rollback;
	if (other > 0) {
		refuse(error, "Conversion run %" G_GINT64_FORMAT " is running: one run works at a time. Pause it, or wait for it "
			"to finish.", other);
		goto rollback;
	}
	if (replace) {
		if (!revision_may_replace(conn, row.revision_id, &why, error))
			goto rollback;
		if (why != NULL) {
			refuse(error, "%s", why);
			goto rollback;
		}
	}
	if (!axis_conversion_run_cap_unit(conn, run_id, row.cap_files, row.cap_bytes, &cap_unit, error))
		goto rollback;
	cap = cap_unit >= 0 ? int_text(cap_unit) : NULL;
	{
		const gchar *params[4];

		params[0] = id;
		params[1] = user;
		params[2] = axis_conversion_outcome_to_string(outcome);
		params[3] = cap;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET state = 'running', started_by = $2, started_at = now(),"
		        " outcome = $3, confirmed_by = CASE WHEN $3 = 'replace' THEN $2 END,"
		        " confirmed_at = CASE WHEN $3 = 'replace' THEN now() END, cap_unit = $4::integer, pause_code = NULL,"
		        " pause_text = NULL, wait_code = NULL, wait_text = NULL, updated_at = now() WHERE id = $1::bigint", 4, params,
		        NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s started conversion run %" G_GINT64_FORMAT " (%s)", user, run_id,
		replace ? "replacing originals" : "trials");
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	if (ok) {
		g_autoptr(GError) local = NULL;

		if (!axis_conversion_runs_request_step(self, &local))
			g_message("conversion runs: the step could not be queued: %s", local->message);
	}
	return ok;
}

gboolean
axis_conversion_runs_pause(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *text = g_strdup_printf("Paused by %s.", user != NULL ? user : "an administrator");
	const gchar *params[3];

	params[0] = id;
	params[1] = text;
	params[2] = user != NULL ? user : "?";
	if (!axis_db_pool_exec(self->conversions->pool, "UPDATE conversion_run SET state = 'paused', pause_code = 'person',"
	        " pause_text = $2, paused_at = now(), paused_by = $3, wait_code = NULL, wait_text = NULL, updated_at = now()"
	        " WHERE id = $1::bigint AND state = 'running' RETURNING id", 3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE, "Only a running run can be paused.");
		return FALSE;
	}
	g_message("audit: %s paused conversion run %" G_GINT64_FORMAT, user != NULL ? user : "?", run_id);
	return TRUE;
}

gboolean
axis_conversion_runs_resume(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	RunRow row;
	AxisDbConn *conn;
	gint64 other = 0;
	gboolean ok = FALSE;

	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "paused", NULL, NULL)) {
		wrong_state(error, &row, "resumed");
		goto rollback;
	}
	if (!other_running(conn, run_id, &other, error))
		goto rollback;
	if (other > 0) {
		refuse(error, "Conversion run %" G_GINT64_FORMAT " is running: one run works at a time. Pause it first.", other);
		goto rollback;
	}
	/* Paused for a bad trial: it goes on once nothing counts against its
	 * revision (the trials set aside, or cleared by a check found wrongly
	 * strict since). */
	if (g_strcmp0(row.pause_code, "bad_trial") == 0 && g_strcmp0(row.outcome, "trial") == 0) {
		g_autoptr(AxisDbResult) bad = NULL;
		g_autofree gchar *revision = int_text(row.revision_id);
		g_autofree gchar *bad_sql = axis_conversion_bad_trial_sql();
		g_autofree gchar *sql = g_strconcat("SELECT count(*) FROM conversion c WHERE c.revision_id = $1::bigint AND ",
			bad_sql, NULL);
		const gchar *params[1];
		gint64 count;

		params[0] = revision;
		if (!axis_db_conn_exec(conn, sql, 1, params, &bad, error))
			goto rollback;
		count = int_at(bad, 0, 0);
		if (count > 0) {
			refuse(error, "%" G_GINT64_FORMAT " trial%s of this version still count%s against proving it (rejected by a "
				"check of the converted file). Set aside, on its page, a trial rejected for a reason that is not the "
				"profile's (a check found wrong since); otherwise change the profile and make a new run.", count,
				count == 1 ? "" : "s", count == 1 ? "s" : "");
			goto rollback;
		}
	}
	{
		const gchar *params[1];

		params[0] = id;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET state = 'running', pause_code = NULL, pause_text = NULL,"
		        " paused_at = NULL, paused_by = NULL, failed_in_row = 0, refused_in_row = 0, updated_at = now()"
		        " WHERE id = $1::bigint", 1, params, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s resumed conversion run %" G_GINT64_FORMAT, user != NULL ? user : "?", run_id);
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	if (ok) {
		g_autoptr(GError) local = NULL;

		if (!axis_conversion_runs_request_step(self, &local))
			g_message("conversion runs: the step could not be queued: %s", local->message);
	}
	return ok;
}

gboolean
axis_conversion_runs_cancel(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *user,
	GError             **error
){
	g_autoptr(AxisDbResult) asked = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *text = g_strdup_printf("Cancelled by %s.", user != NULL ? user : "an administrator");
	RunRow row;
	AxisDbConn *conn;
	gboolean ok = FALSE;
	guint i;

	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "listing", "ready", "running") && !state_is(&row, "paused", NULL, NULL)) {
		wrong_state(error, &row, "cancelled");
		goto rollback;
	}
	{
		const gchar *params[2];

		params[0] = id;
		params[1] = text;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET state = 'cancelled', finished_at = now(),"
		        " finish_text = $2, wait_code = NULL, wait_text = NULL, updated_at = now() WHERE id = $1::bigint", 2, params,
		        NULL, error))
			goto rollback;
		if (!axis_db_conn_exec(conn, "SELECT c.id FROM conversion_run_entry e JOIN conversion c ON c.id = e.conversion_id"
		        " WHERE e.run_id = $1::bigint AND e.state = 'asked' AND c.state IN ('queued', 'encoding')", 1, params, &asked,
		        error))
			goto rollback;
	}
	if (state_is(&row, "listing", NULL, NULL) && row.list_job_id > 0) {
		GError *local = NULL;

		if (!axis_job_cancel(conn, row.list_job_id, NULL, &local))
			g_clear_error(&local);
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s cancelled conversion run %" G_GINT64_FORMAT, user != NULL ? user : "?", run_id);
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	/* What waits for a worker or is being converted is given back, as any
	 * cancelled conversion is; what is being checked or put in place
	 * finishes (the step records it). */
	for (i = 0; ok && i < axis_db_result_get_n_rows(asked); i++) {
		g_autoptr(GError) local = NULL;
		gint64 conversion = int_at(asked, i, 0);

		if (!axis_conversions_cancel(self->conversions, conversion, user, &local) &&
		    !g_error_matches(local, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE))
			g_message("conversion runs: conversion %" G_GINT64_FORMAT " of run %" G_GINT64_FORMAT " could not be "
				"cancelled: %s", conversion, run_id, local->message);
	}
	if (ok) {
		g_autoptr(GError) local = NULL;

		if (!axis_conversion_runs_request_step(self, &local))
			g_message("conversion runs: the step could not be queued: %s", local->message);
	}
	return ok;
}

gboolean
axis_conversion_runs_replace(
	AxisConversionRuns  *self,
	gint64               run_id,
	gboolean             confirmed,
	const gchar         *user,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *why = NULL;
	RunRow row;
	AxisDbConn *conn;
	gint64 other = 0;
	gboolean wake = FALSE;
	gboolean ok = FALSE;

	if (user == NULL || user[0] == '\0') {
		refuse(error, "A conversion run needs a person.");
		return FALSE;
	}
	if (!confirmed) {
		refuse(error, "Replacing originals deletes them: it needs the confirmation ticked.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "ready", "running", "paused")) {
		wrong_state(error, &row, "changed to replacing originals");
		goto rollback;
	}
	if (g_strcmp0(row.outcome, "replace") == 0) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE,
			"The run replaces originals already.");
		goto rollback;
	}
	if (!revision_may_replace(conn, row.revision_id, &why, error))
		goto rollback;
	if (why != NULL) {
		refuse(error, "%s", why);
		goto rollback;
	}
	if (!other_running(conn, run_id, &other, error))
		goto rollback;
	/* A run that paused because its trials could prove the revision goes
	 * on, replacing, unless another is running. */
	wake = state_is(&row, "paused", NULL, NULL) && other == 0 &&
		(g_strcmp0(row.pause_code, "prove") == 0 || g_strcmp0(row.pause_code, "proven") == 0);
	{
		const gchar *params[3];

		params[0] = id;
		params[1] = user;
		params[2] = wake ? "yes" : "no";
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET outcome = 'replace', confirmed_by = $2,"
		        " confirmed_at = now(), state = CASE WHEN $3 = 'yes' THEN 'running' ELSE state END,"
		        " pause_code = CASE WHEN $3 = 'yes' THEN NULL ELSE pause_code END,"
		        " pause_text = CASE WHEN $3 = 'yes' THEN NULL ELSE pause_text END,"
		        " paused_at = CASE WHEN $3 = 'yes' THEN NULL ELSE paused_at END,"
		        " paused_by = CASE WHEN $3 = 'yes' THEN NULL ELSE paused_by END, failed_in_row = 0, refused_in_row = 0,"
		        " current_unit = NULL, updated_at = now() WHERE id = $1::bigint", 3, params, NULL, error))
			goto rollback;
		/* The files converted as trials are asked for again as
		 * replacements; their trials stay on record. */
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run_entry SET state = 'waiting', trial_id = conversion_id,"
		        " conversion_id = NULL, outcome = NULL, result_size = NULL, reason_code = NULL, reason_text = NULL,"
		        " settled_at = NULL WHERE run_id = $1::bigint AND state = 'done' AND outcome = 'trial'", 1, params, NULL,
		        error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s changed conversion run %" G_GINT64_FORMAT " to replacing originals", user, run_id);
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	if (ok) {
		g_autoptr(GError) local = NULL;

		if (!axis_conversion_runs_request_step(self, &local))
			g_message("conversion runs: the step could not be queued: %s", local->message);
	}
	return ok;
}

gboolean
axis_conversion_runs_set_limits(
	AxisConversionRuns            *self,
	gint64                         run_id,
	const AxisConversionRunLimits *limits,
	const gchar                   *user,
	GError                       **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *at_once = NULL;
	g_autofree gchar *files = NULL;
	g_autofree gchar *bytes = NULL;
	g_autofree gchar *seconds = NULL;
	g_autofree gchar *start = NULL;
	g_autofree gchar *end = NULL;
	g_autofree gchar *cap = NULL;
	RunRow row;
	AxisDbConn *conn;
	gint cap_unit = -1;
	gboolean ok = FALSE;

	if (limits->at_once < 1 || limits->at_once > AXIS_CONVERSION_RUN_MAX_AT_ONCE) {
		refuse(error, "A run asks for between 1 and %d files at once.", AXIS_CONVERSION_RUN_MAX_AT_ONCE);
		return FALSE;
	}
	if (limits->cap_files < 0 || limits->cap_files > G_MAXINT32 || limits->cap_bytes < 0 || limits->cap_seconds < 0 ||
	    (limits->cap_seconds > 0 && limits->cap_seconds < 60)) {
		refuse(error, "A cap is a number of files, a size, or a time of at least a minute.");
		return FALSE;
	}
	if ((limits->window_start < 0) != (limits->window_end < 0) || limits->window_start > 1439 ||
	    limits->window_end > 1439 || (limits->window_start >= 0 && limits->window_start == limits->window_end)) {
		refuse(error, "The hours a run may work in are a start and an end that differ.");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "ready", "running", "paused")) {
		wrong_state(error, &row, "changed");
		goto rollback;
	}
	if (!axis_conversion_run_cap_unit(conn, run_id, limits->cap_files, limits->cap_bytes, &cap_unit, error))
		goto rollback;
	at_once = int_text(limits->at_once);
	files = limits->cap_files > 0 ? int_text(limits->cap_files) : NULL;
	bytes = limits->cap_bytes > 0 ? int_text(limits->cap_bytes) : NULL;
	seconds = limits->cap_seconds > 0 ? int_text(limits->cap_seconds) : NULL;
	start = limits->window_start >= 0 ? int_text(limits->window_start) : NULL;
	end = limits->window_end >= 0 ? int_text(limits->window_end) : NULL;
	cap = cap_unit >= 0 ? int_text(cap_unit) : NULL;
	{
		const gchar *params[8];

		params[0] = id;
		params[1] = at_once;
		params[2] = files;
		params[3] = bytes;
		params[4] = seconds;
		params[5] = start;
		params[6] = end;
		params[7] = cap;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET at_once = $2::integer, cap_files = $3::integer,"
		        " cap_bytes = $4::bigint, cap_seconds = $5::bigint, window_start = $6::integer, window_end = $7::integer,"
		        " cap_unit = $8::integer, updated_at = now() WHERE id = $1::bigint", 8, params, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	g_message("audit: %s changed the limits of conversion run %" G_GINT64_FORMAT, user != NULL ? user : "?", run_id);
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	return ok;
}

/* Takes what is not asked for yet of a unit (@entry_id 0) or one entry out of
 * the list. */
static gboolean
remove_from_list(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 position,
	gint64               entry_id,
	const gchar         *user,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *pos = int_text(position);
	g_autofree gchar *entry = int_text(entry_id);
	g_autofree gchar *text = g_strdup_printf("Taken out of the list by %s.", user != NULL ? user : "an administrator");
	g_autofree gchar *cap = NULL;
	RunRow row;
	AxisDbConn *conn;
	gint cap_unit = -1;
	gboolean ok = FALSE;
	const gchar *params[4];

	conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (conn == NULL)
		return FALSE;
	memset(&row, 0, sizeof(row));
	if (!axis_db_conn_begin(conn, error))
		goto out;
	if (!lock_run(conn, run_id, &row, error))
		goto rollback;
	if (!state_is(&row, "ready", "running", "paused")) {
		wrong_state(error, &row, "changed");
		goto rollback;
	}
	params[0] = id;
	params[1] = pos;
	params[2] = entry;
	params[3] = text;
	if (entry_id > 0) {
		const gchar *one[3];

		one[0] = id;
		one[1] = entry;
		one[2] = text;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run_entry SET state = 'removed', reason_code = 'removed',"
		        " reason_text = $3, settled_at = now() WHERE run_id = $1::bigint AND id = $2::bigint AND state = 'waiting'"
		        " RETURNING id", 3, one, &result, error))
			goto rollback;
		if (axis_db_result_get_n_rows(result) != 1) {
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_STATE,
				"Only a file the run has not asked for yet can be taken out of the list.");
			goto rollback;
		}
	} else {
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run_unit SET removed = true WHERE run_id = $1::bigint"
		        " AND position = $2::integer RETURNING position", 2, params, &result, error))
			goto rollback;
		if (axis_db_result_get_n_rows(result) != 1) {
			g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such unit.");
			goto rollback;
		}
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run_entry SET state = 'removed', reason_code = 'removed',"
		        " reason_text = $4, settled_at = now() WHERE run_id = $1::bigint AND unit_position = $2::integer"
		        " AND state = 'waiting' AND $3::bigint = 0", 4, params, NULL, error))
			goto rollback;
		/* The caps count what is left. */
		if (!axis_conversion_run_cap_unit(conn, run_id, row.cap_files, row.cap_bytes, &cap_unit, error))
			goto rollback;
		cap = cap_unit >= 0 ? int_text(cap_unit) : NULL;
		params[1] = cap;
		if (!axis_db_conn_exec(conn, "UPDATE conversion_run SET cap_unit = $2::integer, updated_at = now()"
		        " WHERE id = $1::bigint", 2, params, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;
	ok = TRUE;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	run_row_clear(&row);
	axis_db_pool_release(self->conversions->pool, conn);
	return ok;
}

gboolean
axis_conversion_runs_remove_unit(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 position,
	const gchar         *user,
	GError             **error
){
	return remove_from_list(self, run_id, position, 0, user, error);
}

gboolean
axis_conversion_runs_remove_entry(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint64               entry_id,
	const gchar         *user,
	GError             **error
){
	return remove_from_list(self, run_id, 0, entry_id, user, error);
}
