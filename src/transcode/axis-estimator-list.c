/*
 * axis-estimator-list.c - file lists whose condition is the estimator's
 * decision
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A Savings figure ("128 files skipped: would not shrink enough") links to
 * the file list of the files it counts.  The decision is C, not SQL, so the
 * list reads the rows of the ordinary list (axis-library-browse.c) in its
 * order, narrowed by an SQL condition that every matching file meets (a
 * superset: the condition never decides, it only spares reading files that
 * cannot match), loads each batch's facts through the one loader, decides
 * them with the same functions as the snapshot, and keeps those that match.
 * So a figure and its list agree whenever the index has not changed in
 * between, as with the Analysis lists.
 */

#include "transcode/axis-estimator-load.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-library-browse-private.h"

gboolean
axis_estimate_outcome_matches(
	const gchar              *outcome,
	const AxisEstimateResult *result
){
	AxisEstimateReason reason;

	if (outcome == NULL)
		return result->reason != AXIS_ESTIMATE_NOT_APPLICABLE;
	if (strcmp(outcome, "eligible") == 0)
		return axis_estimate_result_converts(result);
	if (strcmp(outcome, "skipped") == 0)
		return !axis_estimate_result_converts(result) && result->reason != AXIS_ESTIMATE_NOT_APPLICABLE;
	return axis_estimate_reason_from_string(outcome, &reason) && result->reason == reason;
}

/* The files a target is for, as SQL over f and r (constant pieces). */
static gchar *
scope_sql(const AxisEstimateTarget *target)
{
	GString *sql = g_string_new(NULL);
	guint type;
	guint n = 0;

	g_string_append(sql, target->media == AXIS_TARGET_VIDEO ? "f.kind = 'video'" : "f.kind = 'audio'");
	g_string_append(sql, " AND r.media_type IN (");
	for (type = 0; type < AXIS_MEDIA_TYPE_COUNT; type++) {
		if ((target->root_types & AXIS_TARGET_ROOT(type)) == 0)
			continue;
		g_string_append_printf(sql, "%s'%s'", n++ > 0 ? ", " : "", axis_media_type_to_string((AxisMediaType)type));
	}
	if (n == 0)
		g_string_append(sql, "NULL");
	g_string_append_c(sql, ')');
	return g_string_free(sql, FALSE);
}

/* What every file that reaches a reason after the probe's checks meets. */
#define SQL_PROBED_OK \
	"f.state = 'present' AND NOT f.pinned AND f.nlink = 1 AND m.status = 'ok' AND NOT f.changed"

/* A picture stream whose HDR facts satisfy @cond (over s). */
#define SQL_PICTURE(cond) \
	"EXISTS (SELECT 1 FROM media_stream s WHERE s.file_id = f.id AND s.kind = 'video' AND NOT s.attached_pic" \
	" AND (" cond "))"

gchar *
axis_estimator_list_narrowing(
	const AxisEstimateTarget *target,
	const gchar              *outcome
){
	g_autofree gchar *scope = scope_sql(target);
	const gchar *more = SQL_PROBED_OK;

	if (outcome == NULL || strcmp(outcome, "skipped") == 0)
		return g_steal_pointer(&scope);
	if (strcmp(outcome, "not_applicable") == 0)
		return g_strdup_printf("NOT (%s)", scope);
	if (strcmp(outcome, "not_present") == 0)
		more = "f.state <> 'present'";
	else if (strcmp(outcome, "pinned") == 0)
		more = "f.state = 'present' AND (f.pinned OR f.nlink <> 1)";
	else if (strcmp(outcome, "not_probed") == 0)
		more = "f.state = 'present' AND m.file_id IS NULL";
	else if (strcmp(outcome, "probe_failed") == 0)
		more = "f.state = 'present' AND m.status <> 'ok'";
	else if (strcmp(outcome, "probe_stale") == 0)
		return g_strdup_printf("%s AND f.state = 'present' AND m.status = 'ok' AND (f.changed"
			" OR m.probe_version < %d OR m.file_size <> f.size OR m.mtime_sec <> f.mtime_sec"
			" OR m.mtime_nsec <> f.mtime_nsec OR m.ctime_sec <> f.ctime_sec OR m.ctime_nsec <> f.ctime_nsec"
			" OR m.inode <> f.inode)", scope, AXIS_PROBE_VERSION);
	else if (strcmp(outcome, "hdr_uncertain") == 0)
		more = SQL_PROBED_OK " AND (m.hdr_uncertain OR m.hdr_format IS NULL"
			" OR " SQL_PICTURE("s.hdr_uncertain OR s.hdr_format IS NULL") ")";
	else if (strcmp(outcome, "dolby_vision") == 0)
		more = SQL_PROBED_OK " AND (m.hdr_format = 'dolby_vision' OR " SQL_PICTURE("s.hdr_format = 'dolby_vision'") ")";
	else if (strcmp(outcome, "hdr10plus") == 0)
		more = SQL_PROBED_OK " AND (m.hdr_format = 'hdr10plus' OR " SQL_PICTURE("s.hdr_format = 'hdr10plus'") ")";
	else if (strcmp(outcome, "hdr_to_sdr") == 0)
		more = SQL_PROBED_OK " AND (m.hdr_format IN ('hdr10', 'hlg')"
			" OR " SQL_PICTURE("s.hdr_format IN ('hdr10', 'hlg')") ")";
	else if (strcmp(outcome, "interlaced") == 0)
		more = SQL_PROBED_OK " AND (m.interlaced IS TRUE OR " SQL_PICTURE("s.field_order IN ('tt', 'bb', 'tb', 'bt')") ")";
	else if (strcmp(outcome, "field_order_unknown") == 0)
		more = SQL_PROBED_OK " AND m.interlaced IS NULL";
	else if (strcmp(outcome, "offline") == 0)
		more = SQL_PROBED_OK " AND NOT EXISTS (SELECT 1 FROM volume vv WHERE vv.id = f.volume_id"
			" AND vv.state IN ('online', 'read_only'))";
	return g_strdup_printf("%s AND %s", scope, more);
}

/* ── The walk ───────────────────────────────────────────────────── */

typedef struct
{
	AxisDbConn               *conn;
	const AxisLibraryFilter  *filter;
	const AxisEstimateTarget *target;
	const AxisEstimateModel  *model;
	const gchar              *narrowing;
} Walk;

/*
 * Walks the list from the cursor (@key, @after; both %NULL for the top),
 * keeping the ids of matching files in @kept until it holds @want, the
 * list ends (*@ended), or @deadline_us passes (*@late).
 */
static gboolean
walk(
	Walk         *w,
	const gchar  *key,
	const gchar  *after,
	guint         want,
	gint64        deadline_us,
	GArray       *kept,
	gboolean     *ended,
	gboolean     *late,
	GError      **error
){
	g_autofree gchar *scan_key = g_strdup(key);
	g_autofree gchar *scan_after = g_strdup(after);

	*ended = FALSE;
	*late = FALSE;
	while (kept->len < want) {
		g_autoptr(GPtrArray) params = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(AxisDbResult) result = NULL;
		g_autoptr(AxisEstimateRows) rows = NULL;
		g_autofree gchar *sql = NULL;
		g_autofree gint64 *ids = NULL;
		g_autofree AxisEstimateResult *results = NULL;
		guint n;
		guint i;

		if (g_get_monotonic_time() > deadline_us) {
			*late = TRUE;
			return TRUE;
		}
		sql = axis_library_browse_scan_sql(w->filter, w->narrowing, scan_key, scan_after, AXIS_ESTIMATE_LIST_BATCH,
			params);
		if (!axis_estimator_timed_exec(w->conn, AXIS_LIBRARY_LIST_TIMEOUT, sql, params->len,
		        (const gchar *const *)params->pdata, &result, error))
			return FALSE;
		n = axis_db_result_get_n_rows(result);
		if (n == 0) {
			*ended = TRUE;
			return TRUE;
		}
		ids = g_new(gint64, n);
		for (i = 0; i < n; i++) {
			ids[i] = 0;
			axis_db_result_get_int64(result, i, 0, &ids[i]);
		}
		g_free(scan_key);
		g_free(scan_after);
		scan_key = g_strdup(axis_db_result_get_text(result, n - 1, 1));
		scan_after = g_strdup_printf("%" G_GINT64_FORMAT, ids[n - 1]);

		if (!axis_estimator_load_files(w->conn, ids, n, &rows, error))
			return FALSE;
		results = g_new0(AxisEstimateResult, MAX(rows->files->len, 1));
		if (!axis_estimator_evaluate_rows(w->conn, rows, w->target, w->model, results, error))
			return FALSE;
		for (i = 0; i < rows->files->len && kept->len < want; i++) {
			const AxisEstimateFile *file = g_ptr_array_index(rows->files, i);

			if (axis_estimate_outcome_matches(w->filter->outcome, &results[i]))
				g_array_append_val(kept, file->index.file_id);
		}
		/* The end of the list only when this batch was the last and was
		 * read to its end. */
		if (i == rows->files->len && (n < AXIS_ESTIMATE_LIST_BATCH || scan_key == NULL)) {
			*ended = TRUE;
			return TRUE;
		}
		if (n < AXIS_ESTIMATE_LIST_BATCH || scan_key == NULL)
			return TRUE;
	}
	return TRUE;
}

gboolean
axis_estimator_list_files(
	AxisEstimator            *self,
	const AxisLibraryFilter  *filter,
	AxisFileList            **list,
	GError                  **error
){
	g_autoptr(GArray) kept = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *narrowing = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *after = NULL;
	AxisEstimateTarget target;
	AxisEstimateModel *model;
	AxisFileList *out;
	AxisDbConn *conn;
	gboolean ended = FALSE;
	gboolean late = FALSE;
	Walk w;

	g_return_val_if_fail(self != NULL && filter != NULL && list != NULL, FALSE);

	*list = NULL;
	if (filter->target == NULL) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_NOT_FOUND, "no such target");
		return FALSE;
	}
	/* The revision the list names (a figure's link names the one it was
	 * computed for), else the profile's current one. */
	if (!axis_estimator_profile_target(self, filter->target, filter->revision, &target, NULL, error))
		return FALSE;
	model = axis_estimator_get_model(self, error);
	if (model == NULL)
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_estimator_model_unref(model);
		return FALSE;
	}

	out = g_new0(AxisFileList, 1);
	narrowing = axis_estimator_list_narrowing(&target, filter->outcome);
	w.conn = conn;
	w.filter = filter;
	w.target = &target;
	w.model = model;
	w.narrowing = narrowing;

	/* The row the page starts after: its key from the database. */
	if (filter->after > 0) {
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *params[1];

		after = g_strdup_printf("%" G_GINT64_FORMAT, filter->after);
		params[0] = after;
		if (!axis_estimator_timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, axis_library_browse_key_sql(filter->sort), 1,
		        params, &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1 && !axis_db_result_is_null(result, 0, 0)) {
			key = g_strdup(axis_db_result_get_text(result, 0, 0));
		} else {
			out->cursor_gone = TRUE;
			g_clear_pointer(&after, g_free);
		}
	}

	if (!walk(&w, key, after, AXIS_LIBRARY_PAGE_SIZE + 1,
	        g_get_monotonic_time() + (gint64)AXIS_ESTIMATE_LIST_DEADLINE_MS * 1000, kept, &ended, &late, error))
		goto fail;
	if (late) {
		g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT, "the list did not come in time");
		goto fail;
	}
	if (!axis_library_browse_read_files(conn, (const gint64 *)kept->data, MIN(kept->len, AXIS_LIBRARY_PAGE_SIZE),
	        &out->files, error))
		goto fail;
	if (kept->len > AXIS_LIBRARY_PAGE_SIZE)
		out->next = g_array_index(kept, gint64, AXIS_LIBRARY_PAGE_SIZE - 1);

	/* The count: the whole list when the page is the whole list, else a
	 * walk of its own from the top. */
	if (key == NULL && ended) {
		out->count = kept->len;
	} else {
		g_autoptr(GArray) counted = g_array_new(FALSE, FALSE, sizeof(gint64));

		if (!walk(&w, NULL, NULL, AXIS_LIBRARY_COUNT_CAP + 1,
		        g_get_monotonic_time() + (gint64)AXIS_ESTIMATE_COUNT_DEADLINE_MS * 1000, counted, &ended, &late,
		        error))
			goto fail;
		if (late) {
			out->count_unknown = TRUE;
		} else {
			out->count_capped = counted->len > AXIS_LIBRARY_COUNT_CAP;
			out->count = MIN(counted->len, (guint)AXIS_LIBRARY_COUNT_CAP);
		}
	}

	axis_db_pool_release(self->pool, conn);
	axis_estimator_model_unref(model);
	*list = out;
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	axis_estimator_model_unref(model);
	axis_file_list_free(out);
	return FALSE;
}
