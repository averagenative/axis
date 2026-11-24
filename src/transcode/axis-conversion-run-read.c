/*
 * axis-conversion-run-read.c - what the pages read of the conversion runs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every read is a bounded number of statements whatever the size of the
 * list: a run's summary is five, a page of units two, a page of files one,
 * the files not in the list one; pages are by position (or by size and id),
 * never by offset.
 */

#include "transcode/axis-conversion-run-private.h"

#include <string.h>

#define US(column) "coalesce((extract(epoch FROM " column ") * 1000000)::bigint, 0)"

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

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : g_strdup(axis_db_result_get_text(result, row, column));
}

static gboolean
bool_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

static gdouble
double_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_is_null(result, row, column) ? NULL : axis_db_result_get_text(result, row, column);

	return text != NULL ? g_ascii_strtod(text, NULL) : 0.0;
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

/* ── Freeing ────────────────────────────────────────────────────── */

void
axis_conversion_run_wait_free(AxisConversionRunWait *wait)
{
	if (wait == NULL)
		return;
	g_free(wait->volume_name);
	g_free(wait->code);
	g_free(wait->text);
	g_free(wait);
}

void
axis_conversion_run_info_free(AxisConversionRunInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->profile_name);
	g_free(info->profile_slug);
	g_free(info->scope);
	g_free(info->scope_text);
	g_free(info->state);
	g_free(info->outcome);
	g_free(info->created_by);
	g_free(info->started_by);
	g_free(info->confirmed_by);
	g_free(info->finish_text);
	g_free(info->list_text);
	g_free(info->list_job_state);
	g_free(info->list_message);
	g_free(info->pause_code);
	g_free(info->pause_text);
	g_free(info->paused_by);
	g_free(info->wait_code);
	g_free(info->wait_text);
	g_free(info->convert_kept_by);
	g_free(info->relist_state);
	if (info->waits != NULL)
		g_ptr_array_unref(info->waits);
	g_free(info);
}

void
axis_conversion_run_unit_free(AxisConversionRunUnit *unit)
{
	if (unit == NULL)
		return;
	g_free(unit->kind);
	g_free(unit->title);
	g_free(unit->root_name);
	g_free(unit->rel_path);
	g_free(unit);
}

void
axis_conversion_run_entry_free(AxisConversionRunEntry *entry)
{
	if (entry == NULL)
		return;
	g_free(entry->volume_name);
	g_free(entry->rel_path);
	g_free(entry->state);
	g_free(entry->conversion_state);
	g_free(entry->outcome);
	g_free(entry->reason_code);
	g_free(entry->reason_text);
	g_free(entry->job_state);
	g_free(entry->verify_state);
	g_free(entry);
}

void
axis_conversion_run_reason_free(AxisConversionRunReason *reason)
{
	if (reason == NULL)
		return;
	g_free(reason->reason);
	g_free(reason);
}

void
axis_conversion_run_excluded_free(AxisConversionRunExcluded *file)
{
	if (file == NULL)
		return;
	g_free(file->root_name);
	g_free(file->rel_path);
	g_free(file->state);
	g_free(file);
}

/* ── A run ──────────────────────────────────────────────────────── */

/* The run's row (0..45), then for a list the counts of its files by state
 * (46..51). */
#define RUN_COLUMNS \
	"r.id, r.revision_id, pr.name, p.slug, pr.number," \
	" EXISTS (SELECT 1 FROM conversion_profile_proof pf WHERE pf.revision_id = r.revision_id)," \
	" p.current_revision_id = r.revision_id, r.scope, r.scope_text, r.state, r.outcome, r.at_once," \
	" coalesce(r.cap_files, 0), coalesce(r.cap_bytes, 0), coalesce(r.cap_seconds, 0), coalesce(r.cap_unit, -1)," \
	" coalesce(r.window_start, -1), coalesce(r.window_end, -1), r.created_by, " US("r.created_at") "," \
	" r.started_by, " US("r.started_at") ", r.confirmed_by, " US("r.confirmed_at") ", " US("r.finished_at") "," \
	" r.finish_text, r.list_text, " US("r.listed_at") ", j.state::text, coalesce(j.progress, 0), j.progress_message," \
	" r.scope_files, r.pause_code, r.pause_text, r.paused_by, " US("r.paused_at") ", r.wait_code, r.wait_text," \
	" " US("r.wait_since") ", r.failed_in_row, r.refused_in_row, p.archived," \
	" r.convert_kept, r.convert_kept_by, " US("r.convert_kept_at") ", r.relist_state"

#define RUN_FROM \
	" FROM conversion_run r JOIN conversion_profile_revision pr ON pr.id = r.revision_id" \
	" JOIN conversion_profile p ON p.id = pr.profile_id LEFT JOIN job j ON j.id = r.list_job_id"

static AxisConversionRunInfo *
info_from(
	AxisDbResult *result,
	guint         i
){
	AxisConversionRunInfo *info = g_new0(AxisConversionRunInfo, 1);

	info->id = int_at(result, i, 0);
	info->revision_id = int_at(result, i, 1);
	info->profile_name = text_at(result, i, 2);
	info->profile_slug = text_at(result, i, 3);
	info->revision_number = (gint)int_at(result, i, 4);
	info->proven = bool_at(result, i, 5);
	info->current = bool_at(result, i, 6);
	info->scope = text_at(result, i, 7);
	info->scope_text = text_at(result, i, 8);
	info->state = text_at(result, i, 9);
	info->outcome = text_at(result, i, 10);
	info->at_once = (gint)int_at(result, i, 11);
	info->cap_files = int_at(result, i, 12);
	info->cap_bytes = int_at(result, i, 13);
	info->cap_seconds = int_at(result, i, 14);
	info->cap_unit = (gint)int_at(result, i, 15);
	info->window_start = (gint)int_at(result, i, 16);
	info->window_end = (gint)int_at(result, i, 17);
	info->created_by = text_at(result, i, 18);
	info->created_us = int_at(result, i, 19);
	info->started_by = text_at(result, i, 20);
	info->started_us = int_at(result, i, 21);
	info->confirmed_by = text_at(result, i, 22);
	info->confirmed_us = int_at(result, i, 23);
	info->finished_us = int_at(result, i, 24);
	info->finish_text = text_at(result, i, 25);
	info->list_text = text_at(result, i, 26);
	info->listed_us = int_at(result, i, 27);
	info->list_job_state = text_at(result, i, 28);
	info->list_progress = double_at(result, i, 29);
	info->list_message = text_at(result, i, 30);
	info->scope_files = int_at(result, i, 31);
	info->pause_code = text_at(result, i, 32);
	info->pause_text = text_at(result, i, 33);
	info->paused_by = text_at(result, i, 34);
	info->paused_us = int_at(result, i, 35);
	info->wait_code = text_at(result, i, 36);
	info->wait_text = text_at(result, i, 37);
	info->wait_us = int_at(result, i, 38);
	info->failed_in_row = (gint)int_at(result, i, 39);
	info->refused_in_row = (gint)int_at(result, i, 40);
	info->convert_kept = bool_at(result, i, 42);
	info->convert_kept_by = text_at(result, i, 43);
	info->convert_kept_us = int_at(result, i, 44);
	info->relist_state = text_at(result, i, 45);
	return info;
}

/* The counts of a run's files by state, over e (in that order). */
#define ENTRY_COUNTS \
	"count(*) FILTER (WHERE e.state = 'waiting'), count(*) FILTER (WHERE e.state = 'asked')," \
	" count(*) FILTER (WHERE e.state = 'done'), count(*) FILTER (WHERE e.state = 'skipped')," \
	" count(*) FILTER (WHERE e.state = 'failed'), count(*) FILTER (WHERE e.state = 'removed')"

static void
counts_from(
	AxisConversionRunInfo *info,
	AxisDbResult          *result,
	guint                  i,
	guint                  first
){
	info->waiting = int_at(result, i, first);
	info->asked = int_at(result, i, first + 1);
	info->done = int_at(result, i, first + 2);
	info->skipped = int_at(result, i, first + 3);
	info->failed = int_at(result, i, first + 4);
	info->removed = int_at(result, i, first + 5);
}

gboolean
axis_conversion_runs_list(
	AxisConversionRuns  *self,
	guint                limit,
	GPtrArray          **out,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *n = int_text(CLAMP(limit, 1, 200));
	const gchar *params[1];
	GPtrArray *list;
	guint i;

	*out = NULL;
	params[0] = n;
	if (!exec(self, "SELECT " RUN_COLUMNS ", s.* " RUN_FROM
	        " LEFT JOIN LATERAL (SELECT " ENTRY_COUNTS ", coalesce(sum(e.size - e.result_size) FILTER (WHERE"
	        "  e.state = 'done' AND e.outcome = 'replace'), 0) FROM conversion_run_entry e WHERE e.run_id = r.id) s ON true"
	        " ORDER BY r.id DESC LIMIT $1::integer", 1, params, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionRunInfo *info = info_from(result, i);

		counts_from(info, result, i, 46);
		/* Saved for real: replacements done. */
		info->done_after = int_at(result, i, 52);
		g_ptr_array_add(list, info);
	}
	*out = list;
	return TRUE;
}

gboolean
axis_conversion_runs_get(
	AxisConversionRuns     *self,
	gint64                  run_id,
	AxisConversionRunInfo **out,
	GError                **error
){
	g_autoptr(AxisConversionRunInfo) info = NULL;
	g_autoptr(AxisDbResult) run = NULL;
	g_autoptr(AxisDbResult) entries = NULL;
	g_autoptr(AxisDbResult) units = NULL;
	g_autoptr(AxisDbResult) waits = NULL;
	g_autoptr(AxisDbResult) trials = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *cap = NULL;
	g_autofree gchar *revision = NULL;
	const gchar *params[2];
	guint i;

	*out = NULL;
	params[0] = id;
	if (!exec(self, "SELECT " RUN_COLUMNS RUN_FROM " WHERE r.id = $1::bigint", 1, params, &run, error))
		return FALSE;
	if (axis_db_result_get_n_rows(run) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such conversion run.");
		return FALSE;
	}
	info = info_from(run, 0);
	cap = int_text(info->cap_unit);
	revision = int_text(info->revision_id);

	/* Its files by state, what the done ones came to, its units done (every
	 * file of the unit settled and one done at least), the files that wait
	 * within its caps. */
	params[1] = cap;
	if (!exec(self, "SELECT " ENTRY_COUNTS ","
	        " coalesce(sum(e.size) FILTER (WHERE e.state = 'done'), 0), coalesce(sum(e.mid) FILTER (WHERE e.state = 'done'), 0),"
	        " coalesce(sum(e.result_size) FILTER (WHERE e.state = 'done'), 0),"
	        " count(*) FILTER (WHERE e.state = 'done' AND e.outcome = 'trial'),"
	        " (SELECT count(*) FROM (SELECT 1 FROM conversion_run_entry x WHERE x.run_id = $1::bigint GROUP BY x.unit_position"
	        "   HAVING count(*) FILTER (WHERE x.state IN ('waiting', 'asked')) = 0"
	        "   AND count(*) FILTER (WHERE x.state = 'done') > 0) d),"
	        " count(*) FILTER (WHERE e.state = 'waiting' AND ($2::integer < 0 OR e.unit_position <= $2::integer)),"
	        " count(*) FILTER (WHERE (e.state = 'waiting' OR (e.state = 'done' AND e.outcome = 'trial'))"
	        "   AND ($2::integer < 0 OR e.unit_position <= $2::integer)),"
	        " count(*) FILTER (WHERE (e.state = 'waiting' OR (e.state = 'done' AND e.outcome = 'trial'))"
	        "   AND e.convert_kept AND ($2::integer < 0 OR e.unit_position <= $2::integer))"
	        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint", 2, params, &entries, error))
		return FALSE;
	counts_from(info, entries, 0, 0);
	info->done_before = int_at(entries, 0, 6);
	info->done_estimate = int_at(entries, 0, 7);
	info->done_after = int_at(entries, 0, 8);
	info->done_trials = int_at(entries, 0, 9);
	info->units_done = int_at(entries, 0, 10);
	info->waiting_in = int_at(entries, 0, 11);
	info->replace_in = int_at(entries, 0, 12);
	info->replace_kept_in = int_at(entries, 0, 13);

	if (!exec(self, "SELECT count(*), coalesce(sum(u.files), 0), coalesce(sum(u.bytes), 0), coalesce(sum(u.mid), 0),"
	        " count(*) FILTER (WHERE $2::integer < 0 OR u.position <= $2::integer),"
	        " coalesce(sum(u.files) FILTER (WHERE $2::integer < 0 OR u.position <= $2::integer), 0),"
	        " coalesce(sum(u.bytes) FILTER (WHERE $2::integer < 0 OR u.position <= $2::integer), 0),"
	        " coalesce(sum(u.mid) FILTER (WHERE $2::integer < 0 OR u.position <= $2::integer), 0)"
	        " FROM conversion_run_unit u WHERE u.run_id = $1::bigint AND NOT u.removed", 2, params, &units, error))
		return FALSE;
	info->units = int_at(units, 0, 0);
	info->files = int_at(units, 0, 1);
	info->bytes = int_at(units, 0, 2);
	info->mid = int_at(units, 0, 3);
	info->cap_units_in = int_at(units, 0, 4);
	info->cap_files_in = int_at(units, 0, 5);
	info->cap_bytes_in = int_at(units, 0, 6);
	info->cap_mid_in = int_at(units, 0, 7);

	if (!exec(self, "SELECT w.volume_id, v.name, w.code, w.text, " US("w.since") " FROM conversion_run_wait w"
	        " JOIN volume v ON v.id = w.volume_id WHERE w.run_id = $1::bigint ORDER BY v.name", 1, params, &waits, error))
		return FALSE;
	info->waits = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_wait_free);
	for (i = 0; i < axis_db_result_get_n_rows(waits); i++) {
		AxisConversionRunWait *w = g_new0(AxisConversionRunWait, 1);

		w->volume_id = int_at(waits, i, 0);
		w->volume_name = text_at(waits, i, 1);
		w->code = text_at(waits, i, 2);
		w->text = text_at(waits, i, 3);
		w->since_us = int_at(waits, i, 4);
		g_ptr_array_add(info->waits, w);
	}

	/* The revision's trials (as proving by hand counts them) and what its
	 * finished conversions came to beside their estimates. */
	params[0] = revision;
	{
		g_autofree gchar *passed = axis_conversion_passed_trial_sql();
		g_autofree gchar *bad = axis_conversion_bad_trial_sql();
		g_autofree gchar *sql = g_strconcat("SELECT"
			" (SELECT count(DISTINCT c.file_id) FROM conversion c WHERE c.revision_id = $1::bigint AND c.file_id IS NOT NULL"
			"  AND ", passed, "),"
			" (SELECT count(*) FROM conversion c WHERE c.revision_id = $1::bigint AND ", bad, "),"
			" coalesce(s.files, 0), coalesce(s.estimated_bytes, 0), coalesce(s.actual_bytes, 0)"
			" FROM (SELECT 1) one LEFT JOIN conversion_estimate_seen s ON s.revision_id = $1::bigint", NULL);

		if (!exec(self, sql, 1, params, &trials, error))
			return FALSE;
	}
	info->passed_trials = int_at(trials, 0, 0);
	info->bad_trials = int_at(trials, 0, 1);
	info->seen_files = int_at(trials, 0, 2);
	info->seen_estimated = int_at(trials, 0, 3);
	info->seen_actual = int_at(trials, 0, 4);
	info->needed_trials = axis_conversion_setting_integer(self->conversions, AXIS_CONVERSION_SETTING_PROVE_TRIALS, 3);
	*out = g_steal_pointer(&info);
	return TRUE;
}

gboolean
axis_conversion_runs_active(
	AxisConversionRuns     *self,
	AxisConversionRunInfo **info,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*info = NULL;
	if (!exec(self, "SELECT id FROM conversion_run WHERE state IN ('running', 'paused')"
	        " ORDER BY (state = 'running') DESC, id DESC LIMIT 1", 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1)
		return TRUE;
	return axis_conversion_runs_get(self, int_at(result, 0, 0), info, error);
}

gboolean
axis_conversion_runs_waiting(
	AxisConversionRuns  *self,
	guint                timeout_ms,
	AxisDbResult       **result,
	GError             **error
){
	g_return_val_if_fail(self != NULL && result != NULL, FALSE);

	return axis_db_pool_query_bounded(self->conversions->pool,
		"SELECT id, state, coalesce(pause_code, ''), coalesce(floor(extract(epoch FROM paused_at))::bigint, 0),"
		" floor(extract(epoch FROM coalesce(listed_at, created_at)))::bigint"
		" FROM conversion_run WHERE state IN ('paused', 'ready') ORDER BY id DESC LIMIT 6", timeout_ms, result, error);
}

/* ── Units ──────────────────────────────────────────────────────── */

#define UNIT_COLUMNS \
	"u.position, u.kind, coalesce(u.title_id, 0), t.title, coalesce(t.year, 0), lr.name, u.rel_path, u.files, u.bytes," \
	" u.mid, u.removed, (SELECT coalesce(cap_unit, -1) FROM conversion_run WHERE id = u.run_id)"

#define UNIT_FROM \
	" FROM conversion_run_unit u LEFT JOIN meta_title t ON t.id = u.title_id" \
	" LEFT JOIN library_root lr ON lr.id = u.root_id"

static AxisConversionRunUnit *
unit_from(
	AxisDbResult *result,
	guint         i
){
	AxisConversionRunUnit *u = g_new0(AxisConversionRunUnit, 1);
	gint cap;

	u->position = (gint)int_at(result, i, 0);
	u->kind = text_at(result, i, 1);
	u->title_id = int_at(result, i, 2);
	u->title = text_at(result, i, 3);
	u->year = (gint)int_at(result, i, 4);
	u->root_name = text_at(result, i, 5);
	u->rel_path = text_at(result, i, 6);
	u->files = (gint)int_at(result, i, 7);
	u->bytes = int_at(result, i, 8);
	u->mid = int_at(result, i, 9);
	u->removed = bool_at(result, i, 10);
	cap = (gint)int_at(result, i, 11);
	u->beyond_cap = cap >= 0 && u->position > cap;
	return u;
}

/* The counts of the units of @units from their files, in one statement. */
static gboolean
unit_counts(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray           *units,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *first = NULL;
	g_autofree gchar *last = NULL;
	const gchar *params[3];
	guint i;
	guint j;

	if (units->len == 0)
		return TRUE;
	first = int_text(((AxisConversionRunUnit *)g_ptr_array_index(units, 0))->position);
	last = int_text(((AxisConversionRunUnit *)g_ptr_array_index(units, units->len - 1))->position);
	params[0] = id;
	params[1] = first;
	params[2] = last;
	if (!exec(self, "SELECT e.unit_position, count(*) FILTER (WHERE e.state = 'done'),"
	        " count(*) FILTER (WHERE e.state = 'skipped'), count(*) FILTER (WHERE e.state = 'failed'),"
	        " count(*) FILTER (WHERE e.state = 'asked'), count(*) FILTER (WHERE e.state = 'waiting'),"
	        " coalesce(sum(e.result_size) FILTER (WHERE e.state = 'done'), 0),"
	        " coalesce(sum(e.size) FILTER (WHERE e.state = 'done'), 0)"
	        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint AND e.unit_position BETWEEN $2::integer AND $3::integer"
	        " GROUP BY e.unit_position", 3, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint position = (gint)int_at(result, i, 0);

		for (j = 0; j < units->len; j++) {
			AxisConversionRunUnit *u = g_ptr_array_index(units, j);

			if (u->position != position)
				continue;
			u->done = (gint)int_at(result, i, 1);
			u->skipped = (gint)int_at(result, i, 2);
			u->failed = (gint)int_at(result, i, 3);
			u->asked = (gint)int_at(result, i, 4);
			u->waiting = (gint)int_at(result, i, 5);
			u->done_after = int_at(result, i, 6);
			u->done_before = int_at(result, i, 7);
			break;
		}
	}
	return TRUE;
}

gboolean
axis_conversion_runs_units(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 after,
	guint                limit,
	GPtrArray          **out,
	gboolean            *more,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) units = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_unit_free);
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *from = int_text(MAX(after, 0));
	g_autofree gchar *n = int_text((gint64)CLAMP(limit, 1, 500) + 1);
	const gchar *params[3];
	guint i;

	*out = NULL;
	*more = FALSE;
	params[0] = id;
	params[1] = from;
	params[2] = n;
	if (!exec(self, "SELECT " UNIT_COLUMNS UNIT_FROM " WHERE u.run_id = $1::bigint AND u.position > $2::integer"
	        " ORDER BY u.position LIMIT $3::integer", 3, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		if (i == CLAMP(limit, 1, 500)) {
			*more = TRUE;
			break;
		}
		g_ptr_array_add(units, unit_from(result, i));
	}
	if (!unit_counts(self, run_id, units, error))
		return FALSE;
	*out = g_steal_pointer(&units);
	return TRUE;
}

gboolean
axis_conversion_runs_unit(
	AxisConversionRuns     *self,
	gint64                  run_id,
	gint                    position,
	AxisConversionRunUnit **out,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) units = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_unit_free);
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *pos = int_text(position);
	const gchar *params[2];

	*out = NULL;
	params[0] = id;
	params[1] = pos;
	if (!exec(self, "SELECT " UNIT_COLUMNS UNIT_FROM " WHERE u.run_id = $1::bigint AND u.position = $2::integer", 2,
	        params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error_literal(error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND, "There is no such unit.");
		return FALSE;
	}
	g_ptr_array_add(units, unit_from(result, 0));
	if (!unit_counts(self, run_id, units, error))
		return FALSE;
	*out = g_ptr_array_steal_index(units, 0);
	return TRUE;
}

/* ── Files ──────────────────────────────────────────────────────── */

#define ENTRY_COLUMNS \
	"e.id, e.position, e.unit_position, coalesce(e.file_id, 0), coalesce(e.volume_id, 0), v.name, e.rel_path," \
	" coalesce(e.season_number, -1), coalesce(e.episode_number, -1), e.size, e.low, e.mid, e.high, e.state," \
	" coalesce(e.conversion_id, 0), c.state, coalesce(e.trial_id, 0), e.outcome, coalesce(e.result_size, -1)," \
	" e.reason_code, e.reason_text, " US("e.settled_at") "," \
	" j.state::text, coalesce(j.progress, -1)," \
	" CASE WHEN j.state = 'running' AND j.started_at IS NOT NULL" \
	"  THEN greatest(extract(epoch FROM now() - j.started_at), 0)::bigint ELSE -1 END," \
	" vj.state::text, coalesce(vj.progress, -1)," \
	" CASE WHEN vj.state = 'running' AND vj.started_at IS NOT NULL" \
	"  THEN greatest(extract(epoch FROM now() - vj.started_at), 0)::bigint ELSE -1 END," \
	" CASE WHEN e.state = 'asked' THEN coalesce((SELECT mi.duration_ms FROM media_info mi" \
	"  WHERE mi.file_id = e.file_id), 0) ELSE 0 END"

/* The jobs are those of a file in flight only: a settled entry's row joins
 * none (its page of the list reads no job). */
#define ENTRY_FROM \
	" FROM conversion_run_entry e LEFT JOIN volume v ON v.id = e.volume_id" \
	" LEFT JOIN conversion c ON c.id = e.conversion_id" \
	" LEFT JOIN job j ON e.state = 'asked' AND j.id = c.job_id" \
	" LEFT JOIN job vj ON e.state = 'asked' AND vj.id = c.verify_job_id"

static AxisConversionRunEntry *
entry_from(
	AxisDbResult *result,
	guint         i
){
	AxisConversionRunEntry *e = g_new0(AxisConversionRunEntry, 1);

	e->id = int_at(result, i, 0);
	e->position = (gint)int_at(result, i, 1);
	e->unit_position = (gint)int_at(result, i, 2);
	e->file_id = int_at(result, i, 3);
	e->volume_id = int_at(result, i, 4);
	e->volume_name = text_at(result, i, 5);
	e->rel_path = text_at(result, i, 6);
	e->season_number = (gint)int_at(result, i, 7);
	e->episode_number = (gint)int_at(result, i, 8);
	e->size = int_at(result, i, 9);
	e->low = int_at(result, i, 10);
	e->mid = int_at(result, i, 11);
	e->high = int_at(result, i, 12);
	e->state = text_at(result, i, 13);
	e->conversion_id = int_at(result, i, 14);
	e->conversion_state = text_at(result, i, 15);
	e->trial_id = int_at(result, i, 16);
	e->outcome = text_at(result, i, 17);
	e->result_size = int_at(result, i, 18);
	e->reason_code = text_at(result, i, 19);
	e->reason_text = text_at(result, i, 20);
	e->settled_us = int_at(result, i, 21);
	e->job_state = text_at(result, i, 22);
	e->job_fraction = double_at(result, i, 23);
	e->job_running_s = int_at(result, i, 24);
	e->verify_state = text_at(result, i, 25);
	e->verify_fraction = double_at(result, i, 26);
	e->verify_running_s = int_at(result, i, 27);
	e->duration_ms = int_at(result, i, 28);
	return e;
}

static gboolean
entries_where(
	AxisConversionRuns  *self,
	const gchar         *where,
	guint                n_params,
	const gchar *const  *params,
	guint                limit,
	GPtrArray          **out,
	gboolean            *more,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = g_strdup_printf("SELECT " ENTRY_COLUMNS ENTRY_FROM " %s", where);
	GPtrArray *list;
	guint i;

	*out = NULL;
	if (more != NULL)
		*more = FALSE;
	if (!exec(self, sql, n_params, params, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_entry_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		if (limit > 0 && i == limit) {
			if (more != NULL)
				*more = TRUE;
			break;
		}
		g_ptr_array_add(list, entry_from(result, i));
	}
	*out = list;
	return TRUE;
}

gboolean
axis_conversion_runs_entries(
	AxisConversionRuns  *self,
	gint64               run_id,
	gint                 unit_position,
	gint                 after,
	guint                limit,
	GPtrArray          **entries,
	gboolean            *more,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *unit = int_text(unit_position);
	g_autofree gchar *from = int_text(MAX(after, 0));
	g_autofree gchar *n = int_text((gint64)CLAMP(limit, 1, 1000) + 1);
	const gchar *params[4];

	params[0] = id;
	params[1] = unit;
	params[2] = from;
	params[3] = n;
	return entries_where(self, "WHERE e.run_id = $1::bigint AND e.unit_position = $2::integer"
		" AND e.position > $3::integer ORDER BY e.position LIMIT $4::integer", 4, params, CLAMP(limit, 1, 1000), entries,
		more, error);
}

gboolean
axis_conversion_runs_in_flight(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray          **entries,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[1];

	params[0] = id;
	return entries_where(self, "WHERE e.run_id = $1::bigint AND e.state = 'asked' ORDER BY e.position LIMIT 20", 1,
		params, 0, entries, NULL, error);
}

gboolean
axis_conversion_runs_recent(
	AxisConversionRuns  *self,
	gint64               run_id,
	guint                limit,
	GPtrArray          **entries,
	GError             **error
){
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *n = int_text(CLAMP(limit, 1, 200));
	const gchar *params[2];

	params[0] = id;
	params[1] = n;
	return entries_where(self, "WHERE e.run_id = $1::bigint AND e.settled_at IS NOT NULL AND e.state <> 'removed'"
		" ORDER BY e.settled_at DESC, e.id DESC LIMIT $2::integer", 2, params, 0, entries, NULL, error);
}

/* ── Series on disk ─────────────────────────────────────────────── */

void
axis_conversion_run_series_free(AxisConversionRunSeries *series)
{
	if (series == NULL)
		return;
	g_free(series->title);
	g_free(series);
}

gboolean
axis_conversion_runs_series_on_disk(
	AxisConversionRuns  *self,
	guint                limit,
	GPtrArray          **out,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *n = int_text(CLAMP(limit, 1, 10000));
	const gchar *params[1];
	GPtrArray *list;
	guint i;

	*out = NULL;
	params[0] = n;
	if (!exec(self, "SELECT t.id, t.title, coalesce(t.year, 0) FROM meta_title t WHERE t.kind = 'series'"
	        " AND EXISTS (SELECT 1 FROM library_identity li JOIN library_root r ON r.id = li.root_id"
	        "  WHERE li.title_id = t.id AND li.state = 'linked' AND r.media_type = 'tv')"
	        " ORDER BY lower(t.title), t.year, t.id LIMIT $1::integer", 1, params, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_series_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionRunSeries *s = g_new0(AxisConversionRunSeries, 1);

		s->title_id = int_at(result, i, 0);
		s->title = text_at(result, i, 1);
		s->year = (gint)int_at(result, i, 2);
		g_ptr_array_add(list, s);
	}
	*out = list;
	return TRUE;
}

/* ── What is not in the list ────────────────────────────────────── */

gboolean
axis_conversion_runs_reasons(
	AxisConversionRuns  *self,
	gint64               run_id,
	GPtrArray          **out,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[1];
	GPtrArray *list;
	guint i;

	*out = NULL;
	params[0] = id;
	if (!exec(self, "SELECT reason, files, bytes FROM conversion_run_reason WHERE run_id = $1::bigint"
	        " ORDER BY files DESC, reason", 1, params, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_reason_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionRunReason *r = g_new0(AxisConversionRunReason, 1);

		r->reason = text_at(result, i, 0);
		r->files = int_at(result, i, 1);
		r->bytes = int_at(result, i, 2);
		g_ptr_array_add(list, r);
	}
	*out = list;
	return TRUE;
}

gboolean
axis_conversion_runs_excluded(
	AxisConversionRuns  *self,
	gint64               run_id,
	const gchar         *reason,
	gint64               after_size,
	gint64               after_file,
	guint                limit,
	GPtrArray          **out,
	gboolean            *more,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *size = int_text(after_size);
	g_autofree gchar *file = int_text(after_file);
	g_autofree gchar *n = int_text((gint64)CLAMP(limit, 1, 1000) + 1);
	const gchar *params[5];
	GPtrArray *list;
	guint i;

	*out = NULL;
	*more = FALSE;
	params[0] = id;
	params[1] = reason;
	params[2] = size;
	params[3] = file;
	params[4] = n;
	if (!exec(self, "SELECT x.file_id, x.size, lr.name, f.rel_path, f.state FROM conversion_run_excluded x"
	        " LEFT JOIN library_file f ON f.id = x.file_id LEFT JOIN library_root lr ON lr.id = f.root_id"
	        " WHERE x.run_id = $1::bigint AND x.reason = $2"
	        " AND ($4::bigint <= 0 OR x.size < $3::bigint OR (x.size = $3::bigint AND x.file_id < $4::bigint))"
	        " ORDER BY x.size DESC, x.file_id DESC LIMIT $5::integer", 5, params, &result, error))
		return FALSE;
	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_conversion_run_excluded_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisConversionRunExcluded *x;

		if (i == CLAMP(limit, 1, 1000)) {
			*more = TRUE;
			break;
		}
		x = g_new0(AxisConversionRunExcluded, 1);
		x->file_id = int_at(result, i, 0);
		x->size = int_at(result, i, 1);
		x->root_name = text_at(result, i, 2);
		x->rel_path = text_at(result, i, 3);
		x->state = text_at(result, i, 4);
		g_ptr_array_add(list, x);
	}
	*out = list;
	return TRUE;
}
