/*
 * axis-conversion-run-list.c - the suggested list of a conversion run
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every video file of the scope is read with the estimator's own columns
 * (AXIS_ESTIMATE_SQL_COLUMNS_BASE, axis_estimator_read_row(), the streams by
 * axis_estimator_attach_streams()) and decided by the estimator's own
 * function for the run's revision (axis_estimate_eligibility(), the target
 * configured as every estimate is): there is no second estimator.  Before
 * the estimate, a file the conversions remember for this revision (not
 * worth it, or converted with it already: gate 13) or that has a
 * conversion in flight (gate 11) is not listed, with that reason.  A file
 * the estimator would convert (eligible, or eligible on a volume that is
 * away now, which then waits) is an entry; any other keeps its one reason.
 *
 * It runs as a job, through a cursor, a batch at a time, in one REPEATABLE
 * READ transaction: the list appears whole when the run becomes ready, and
 * describes one moment.  Units are made at the end in SQL: a series (every
 * episode file identification linked to it, whichever TV root it is in), a
 * TV folder that is no identified series (taken as one), or a file; ranked
 * by their estimated saving, largest first; a unit's files in season and
 * episode order (then by path).
 */

#include "transcode/axis-conversion-run-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "db/axis-db-error.h"
#include "jobs/axis-job-error.h"
#include "transcode/axis-estimator-load.h"

#define BATCH "2000"

/* The extra columns, after AXIS_ESTIMATE_SQL_COLUMNS_BASE and its 0 (54). */
#define COL_MEMORY    55
#define COL_IN_FLIGHT 56
#define COL_SERIES    57
#define COL_MOVIE     58
#define COL_FOLDER    59
#define COL_SEASON    60
#define COL_EPISODE   61
#define COL_KEPT      62

typedef struct
{
	gint64 files;
	gint64 bytes;
} Count;

typedef struct
{
	AxisConversionRuns       *self;
	AxisDbConn               *conn;
	AxisJobContext           *context;
	gint64                    run_id;
	gchar                    *run_text;
	AxisEstimateTarget        target;
	AxisEstimateModel        *model;
	GHashTable               *reasons;	/* reason -> Count */
	JsonBuilder              *entries;
	guint                     n_entries;
	JsonBuilder              *excluded;
	guint                     n_excluded;
	gint64                    scope_files;
	gint64                    listed;
	/* convert-kept-files: the run's choice (the reason "kept" is lifted,
	 * and no other), and whether this job adds the files left out as kept
	 * to a list there is already instead of making the list. */
	gboolean                  convert_kept;
	gboolean                  adding;
} List;

static gchar *
int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_is_null(result, row, column))
		axis_db_result_get_int64(result, row, column, &value);
	return value;
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

static void
count(
	List        *l,
	const gchar *reason,
	gint64       bytes
){
	Count *c = g_hash_table_lookup(l->reasons, reason);

	if (c == NULL) {
		c = g_new0(Count, 1);
		g_hash_table_insert(l->reasons, g_strdup(reason), c);
	}
	c->files++;
	c->bytes += MAX(bytes, 0);
}

static void
begin_batches(List *l)
{
	g_clear_object(&l->entries);
	g_clear_object(&l->excluded);
	l->entries = json_builder_new();
	l->excluded = json_builder_new();
	json_builder_begin_array(l->entries);
	json_builder_begin_array(l->excluded);
	l->n_entries = 0;
	l->n_excluded = 0;
}

/* Writes what the batch kept: the entries, the files left out. */
static gboolean
flush(
	List    *l,
	GError **error
){
	g_autofree gchar *run = int_text(l->run_id);
	const gchar *params[2];

	json_builder_end_array(l->entries);
	json_builder_end_array(l->excluded);
	params[0] = run;
	if (l->n_entries > 0) {
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autoptr(JsonNode) root = json_builder_get_root(l->entries);
		g_autofree gchar *json = NULL;

		json_generator_set_root(generator, root);
		json = json_generator_to_data(generator, NULL);
		params[1] = json;
		if (!axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_entry (run_id, unit_key, unit_kind, unit_title_id,"
		        " unit_path, file_id, volume_id, root_id, rel_path, season_number, episode_number, size, low, mid, high)"
		        " SELECT $1::bigint, x.k, x.uk, x.t, x.up, x.f, x.v, x.r, x.p, x.s, x.e, x.z, x.l, x.m, x.h"
		        " FROM jsonb_to_recordset($2::jsonb) AS x(k text, uk text, t bigint, up text, f bigint, v bigint,"
		        "  r bigint, p text, s integer, e integer, z bigint, l bigint, m bigint, h bigint)", 2, params, NULL, error))
			return FALSE;
	}
	if (l->n_excluded > 0) {
		g_autoptr(JsonGenerator) generator = json_generator_new();
		g_autoptr(JsonNode) root = json_builder_get_root(l->excluded);
		g_autofree gchar *json = NULL;

		json_generator_set_root(generator, root);
		json = json_generator_to_data(generator, NULL);
		params[1] = json;
		if (!axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_excluded (run_id, reason, file_id, size)"
		        " SELECT $1::bigint, x.w, x.f, x.z FROM jsonb_to_recordset($2::jsonb) AS x(w text, f bigint, z bigint)"
		        " ON CONFLICT DO NOTHING", 2, params, NULL, error))
			return FALSE;
	}
	begin_batches(l);
	return TRUE;
}

static void
exclude(
	List                   *l,
	const AxisEstimateFile *file,
	const gchar            *reason
){
	count(l, reason, file->index.size);
	json_builder_begin_object(l->excluded);
	json_builder_set_member_name(l->excluded, "w");
	json_builder_add_string_value(l->excluded, reason);
	json_builder_set_member_name(l->excluded, "f");
	json_builder_add_int_value(l->excluded, file->index.file_id);
	json_builder_set_member_name(l->excluded, "z");
	json_builder_add_int_value(l->excluded, MAX(file->index.size, 0));
	json_builder_end_object(l->excluded);
	l->n_excluded++;
}

static void
add_null_or_int(
	JsonBuilder *b,
	const gchar *member,
	gint64       value,
	gboolean     present
){
	json_builder_set_member_name(b, member);
	if (present)
		json_builder_add_int_value(b, value);
	else
		json_builder_add_null_value(b);
}

/* One file of the scope, with the run's own columns of @result's @row. */
static void
decide(
	List                   *l,
	const AxisEstimateFile *file,
	AxisDbResult           *result,
	guint                   row
){
	AxisEstimateResult estimate;
	AxisEstimateReason reason;
	const gchar *memory = axis_db_result_is_null(result, row, COL_MEMORY) ? NULL :
		axis_db_result_get_text(result, row, COL_MEMORY);
	gboolean series = !axis_db_result_is_null(result, row, COL_SERIES);
	gboolean movie = !axis_db_result_is_null(result, row, COL_MOVIE);
	const gchar *folder = axis_db_result_is_null(result, row, COL_FOLDER) ? NULL :
		axis_db_result_get_text(result, row, COL_FOLDER);
	g_autofree gchar *key = NULL;
	const gchar *kind;
	const gchar *unit_path;
	gint64 title = 0;

	l->scope_files++;
	/* A file a person keeps, or one brought over: a run replaces files, and
	 * does not replace this one (gate "kept"), until the keep is lifted, or
	 * unless an administrator chose, for this run, to convert kept files.
	 * The choice passes over this reason only: the file is decided by
	 * everything below exactly as a file that is not kept. */
	if (bool_at(result, row, COL_KEPT) && !l->convert_kept) {
		exclude(l, file, "kept");
		return;
	}
	/* What the conversions remember of the file for this revision, and a
	 * conversion in flight: gates 13 and 11, before the estimate. */
	if (g_strcmp0(memory, "not_worth_it") == 0 || g_strcmp0(memory, "converted") == 0) {
		exclude(l, file, memory);
		return;
	}
	if (bool_at(result, row, COL_IN_FLIGHT)) {
		exclude(l, file, "in_flight");
		return;
	}
	memset(&estimate, 0, sizeof(estimate));
	reason = axis_estimate_eligibility(file, &l->target, l->model, &estimate);
	if (!axis_estimate_result_converts(&estimate) || !estimate.sized) {
		exclude(l, file, axis_estimate_reason_to_string(reason == AXIS_ESTIMATE_ELIGIBLE ?
			AXIS_ESTIMATE_FACTS_MISSING : reason));
		return;
	}
	/* Its unit. */
	if (series) {
		title = int_at(result, row, COL_SERIES, 0);
		key = g_strdup_printf("s:%" G_GINT64_FORMAT, title);
		kind = "series";
		unit_path = folder != NULL ? folder : file->index.rel_path;
	} else if (folder != NULL) {
		key = g_strdup_printf("d:%" G_GINT64_FORMAT ":%s", file->index.root_id, folder);
		kind = "folder";
		unit_path = folder;
	} else {
		if (movie)
			title = int_at(result, row, COL_MOVIE, 0);
		key = g_strdup_printf("f:%" G_GINT64_FORMAT, file->index.file_id);
		kind = movie ? "movie" : "file";
		unit_path = file->index.rel_path;
	}
	json_builder_begin_object(l->entries);
	json_builder_set_member_name(l->entries, "k");
	json_builder_add_string_value(l->entries, key);
	json_builder_set_member_name(l->entries, "uk");
	json_builder_add_string_value(l->entries, kind);
	add_null_or_int(l->entries, "t", title, title > 0);
	json_builder_set_member_name(l->entries, "up");
	json_builder_add_string_value(l->entries, unit_path);
	json_builder_set_member_name(l->entries, "f");
	json_builder_add_int_value(l->entries, file->index.file_id);
	json_builder_set_member_name(l->entries, "v");
	json_builder_add_int_value(l->entries, file->index.volume_id);
	json_builder_set_member_name(l->entries, "r");
	json_builder_add_int_value(l->entries, file->index.root_id);
	json_builder_set_member_name(l->entries, "p");
	json_builder_add_string_value(l->entries, file->index.rel_path);
	add_null_or_int(l->entries, "s", int_at(result, row, COL_SEASON, 0), !axis_db_result_is_null(result, row, COL_SEASON));
	add_null_or_int(l->entries, "e", int_at(result, row, COL_EPISODE, 0),
		!axis_db_result_is_null(result, row, COL_EPISODE));
	json_builder_set_member_name(l->entries, "z");
	json_builder_add_int_value(l->entries, MAX(file->index.size, 0));
	json_builder_set_member_name(l->entries, "l");
	json_builder_add_int_value(l->entries, estimate.low);
	json_builder_set_member_name(l->entries, "m");
	json_builder_add_int_value(l->entries, estimate.mid);
	json_builder_set_member_name(l->entries, "h");
	json_builder_add_int_value(l->entries, estimate.high);
	json_builder_end_object(l->entries);
	l->n_entries++;
	l->listed++;
}

/* The scope as SQL over f and r (numbers only, from the run's row). */
static gchar *
scope_sql(
	const gchar *scope,
	const gchar *root_ids,
	gint64       title_id
){
	if (g_strcmp0(scope, "tv") == 0)
		return g_strdup("r.media_type = 'tv'");
	if (g_strcmp0(scope, "roots") == 0) {
		g_autoptr(GString) ids = g_string_new(NULL);
		const gchar *p;

		/* The array as PostgreSQL wrote it: digits and commas only. */
		for (p = root_ids != NULL ? root_ids : ""; *p != '\0'; p++) {
			if (g_ascii_isdigit(*p) || *p == ',')
				g_string_append_c(ids, *p);
		}
		if (ids->len == 0)
			g_string_append(ids, "0");
		return g_strdup_printf("r.media_type IN ('movies', 'tv', 'other') AND r.id IN (%s)", ids->str);
	}
	if (g_strcmp0(scope, "series") == 0)
		return g_strdup_printf("r.media_type = 'tv' AND li.title_id = %" G_GINT64_FORMAT, title_id);
	return g_strdup("r.media_type = 'movies'");
}

static gboolean
read_scope(
	List         *l,
	const gchar  *scope,
	const gchar  *root_ids,
	gint64        title_id,
	gint64        revision_id,
	GError      **error
){
	g_autofree gchar *where = NULL;
	g_autofree gchar *sql = NULL;

	where = scope_sql(scope, root_ids, title_id);
	/* Adding to a list there is: only the files it left out as kept. */
	if (l->adding) {
		g_autofree gchar *scope_only = g_steal_pointer(&where);

		where = g_strdup_printf("%s AND f.id IN (SELECT x.file_id FROM conversion_run_excluded x WHERE x.run_id = %"
			G_GINT64_FORMAT " AND x.reason = 'kept')", scope_only, l->run_id);
	}
	sql = g_strdup_printf("DECLARE axis_run_files NO SCROLL CURSOR FOR"
		" SELECT " AXIS_ESTIMATE_SQL_COLUMNS_BASE ", 0,"
		" mem.reason,"
		" EXISTS (SELECT 1 FROM conversion c WHERE c.file_id = f.id"
		"  AND c.state IN ('queued', 'encoding', 'verifying', 'placing')),"
		" CASE WHEN r.media_type = 'tv' AND mt.kind = 'series' THEN li.title_id END,"
		" CASE WHEN r.media_type = 'movies' AND mt.kind = 'movie' THEN li.title_id END,"
		" CASE WHEN r.media_type = 'tv' AND strpos(f.rel_path, '/') > 0 THEN split_part(f.rel_path, '/', 1) END,"
		" ep.season_number, ep.number,"
		" (EXISTS (SELECT 1 FROM movie_file kf WHERE kf.file_id = f.id AND kf.kept)"
		"  OR EXISTS (SELECT 1 FROM series_file ks WHERE ks.file_id = f.id AND ks.kept))"
		" " AXIS_ESTIMATE_SQL_FROM
		" LEFT JOIN conversion_memory mem ON mem.file_id = f.id AND mem.revision_id = %" G_GINT64_FORMAT
		" LEFT JOIN library_identity_file lif ON lif.file_id = f.id"
		" LEFT JOIN library_identity li ON li.id = lif.identity_id AND li.state = 'linked'"
		" LEFT JOIN meta_title mt ON mt.id = li.title_id"
		" LEFT JOIN library_identity_episode ep ON ep.file_id = lif.file_id AND ep.position = 0"
		"  AND ep.air_date IS NULL"
		" WHERE f.kind = 'video' AND f.state = 'present' AND %s"
		" ORDER BY f.id", revision_id, where);
	if (!axis_db_conn_exec(l->conn, sql, 0, NULL, NULL, error))
		return FALSE;
	begin_batches(l);
	for (;;) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autoptr(AxisEstimateRows) batch = NULL;
		guint n;
		guint i;

		if (l->context != NULL && axis_job_context_is_cancelled(l->context)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
			return FALSE;
		}
		if (!axis_db_conn_exec(l->conn, "FETCH " BATCH " FROM axis_run_files", 0, NULL, &result, error))
			return FALSE;
		n = axis_db_result_get_n_rows(result);
		if (n == 0)
			break;
		batch = axis_estimate_rows_new();
		for (i = 0; i < n; i++)
			g_ptr_array_add(batch->owned, axis_estimator_read_row(batch, result, i));
		if (!axis_estimator_attach_streams(l->conn, batch, 0, error))
			return FALSE;
		for (i = 0; i < n; i++)
			decide(l, &((AxisEstimateLoaded *)g_ptr_array_index(batch->owned, i))->file, result, i);
		if (!flush(l, error))
			return FALSE;
		if (l->context != NULL) {
			g_autofree gchar *message = g_strdup_printf("%" G_GINT64_FORMAT " files looked at, %" G_GINT64_FORMAT
				" listed", l->scope_files, l->listed);

			axis_job_context_set_progress(l->context, 0.5, message);
		}
	}
	json_builder_end_array(l->entries);
	json_builder_end_array(l->excluded);
	g_clear_object(&l->entries);
	g_clear_object(&l->excluded);
	return axis_db_conn_exec(l->conn, "CLOSE axis_run_files", 0, NULL, NULL, error);
}

/* The units, ranked; every entry's unit and place in the run's order. */
static gboolean
make_units(
	List    *l,
	GError **error
){
	g_autofree gchar *run = int_text(l->run_id);
	const gchar *params[1];

	params[0] = run;
	if (!axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_unit (run_id, position, unit_key, kind, title_id,"
	        " root_id, rel_path, files, bytes, mid)"
	        " SELECT $1::bigint, row_number() OVER (ORDER BY sum(e.size - e.mid) DESC, e.unit_key), e.unit_key,"
	        "  min(e.unit_kind), min(e.unit_title_id), min(e.root_id), min(e.unit_path), count(*), sum(e.size), sum(e.mid)"
	        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint GROUP BY e.unit_key", 1, params, NULL, error))
		return FALSE;
	return axis_db_conn_exec(l->conn, "UPDATE conversion_run_entry e SET unit_position = o.unit, position = o.n"
		" FROM (SELECT e2.id, u.position AS unit, row_number() OVER (ORDER BY u.position, e2.season_number NULLS LAST,"
		"  e2.episode_number NULLS LAST, e2.rel_path, e2.id) AS n FROM conversion_run_entry e2"
		"  JOIN conversion_run_unit u ON u.run_id = e2.run_id AND u.unit_key = e2.unit_key"
		"  WHERE e2.run_id = $1::bigint) o WHERE e.id = o.id", 1, params, NULL, error);
}

static gboolean
write_reasons(
	List    *l,
	GError **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *run = int_text(l->run_id);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	const gchar *params[2];

	json_builder_begin_array(builder);
	g_hash_table_iter_init(&iter, l->reasons);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const Count *c = value;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "w");
		json_builder_add_string_value(builder, key);
		json_builder_set_member_name(builder, "n");
		json_builder_add_int_value(builder, c->files);
		json_builder_set_member_name(builder, "b");
		json_builder_add_int_value(builder, c->bytes);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	json = json_generator_to_data(generator, NULL);
	params[0] = run;
	params[1] = json;
	return axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_reason (run_id, reason, files, bytes)"
		" SELECT $1::bigint, x.w, x.n, x.b FROM jsonb_to_recordset($2::jsonb) AS x(w text, n bigint, b bigint)", 2,
		params, NULL, error);
}

/*
 * Adding to a list there is (convert-kept-files): the new entries are those
 * with position 0.  Those of a unit the list has join it, after its files;
 * the others make new units after the ones there are, ranked among
 * themselves by their estimated saving.  Nothing of an entry or a unit that
 * was there is changed but a unit's totals.  A unit a person took out of
 * the list stays out: its new files are taken out with it.
 */
static gboolean
add_units(
	List    *l,
	GError **error
){
	g_autofree gchar *run = int_text(l->run_id);
	const gchar *params[1];

	params[0] = run;
	if (!axis_db_conn_exec(l->conn, "UPDATE conversion_run_unit u SET files = u.files + n.files,"
	        " bytes = u.bytes + n.bytes, mid = u.mid + n.mid"
	        " FROM (SELECT e.unit_key, count(*) AS files, sum(e.size) AS bytes, sum(e.mid) AS mid"
	        "  FROM conversion_run_entry e WHERE e.run_id = $1::bigint AND e.position = 0 GROUP BY e.unit_key) n"
	        " WHERE u.run_id = $1::bigint AND u.unit_key = n.unit_key", 1, params, NULL, error))
		return FALSE;
	if (!axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_unit (run_id, position, unit_key, kind, title_id,"
	        " root_id, rel_path, files, bytes, mid)"
	        " SELECT $1::bigint, (SELECT coalesce(max(u.position), 0) FROM conversion_run_unit u WHERE u.run_id = $1::bigint)"
	        "  + row_number() OVER (ORDER BY sum(e.size - e.mid) DESC, e.unit_key), e.unit_key,"
	        "  min(e.unit_kind), min(e.unit_title_id), min(e.root_id), min(e.unit_path), count(*), sum(e.size), sum(e.mid)"
	        " FROM conversion_run_entry e WHERE e.run_id = $1::bigint AND e.position = 0"
	        "  AND NOT EXISTS (SELECT 1 FROM conversion_run_unit u WHERE u.run_id = $1::bigint AND u.unit_key = e.unit_key)"
	        " GROUP BY e.unit_key", 1, params, NULL, error))
		return FALSE;
	return axis_db_conn_exec(l->conn, "UPDATE conversion_run_entry e SET unit_position = o.unit, position = o.n,"
		" state = CASE WHEN o.removed THEN 'removed' ELSE e.state END,"
		" reason_code = CASE WHEN o.removed THEN 'removed' ELSE e.reason_code END,"
		" reason_text = CASE WHEN o.removed THEN 'Its movie or series was taken out of the list.' ELSE e.reason_text END,"
		" settled_at = CASE WHEN o.removed THEN now() ELSE e.settled_at END"
		" FROM (SELECT e2.id, u.position AS unit, u.removed,"
		"  (SELECT coalesce(max(x.position), 0) FROM conversion_run_entry x WHERE x.run_id = $1::bigint)"
		"  + row_number() OVER (ORDER BY u.position, e2.season_number NULLS LAST, e2.episode_number NULLS LAST,"
		"   e2.rel_path, e2.id) AS n"
		"  FROM conversion_run_entry e2 JOIN conversion_run_unit u ON u.run_id = e2.run_id AND u.unit_key = e2.unit_key"
		"  WHERE e2.run_id = $1::bigint AND e2.position = 0) o WHERE e.id = o.id", 1, params, NULL, error);
}

/* The reasons' counts, from the files left out as they are now. */
static gboolean
recount_reasons(
	List    *l,
	GError **error
){
	g_autofree gchar *run = int_text(l->run_id);
	const gchar *params[1];

	params[0] = run;
	return axis_db_conn_exec(l->conn, "DELETE FROM conversion_run_reason WHERE run_id = $1::bigint", 1, params, NULL,
	        error) &&
		axis_db_conn_exec(l->conn, "INSERT INTO conversion_run_reason (run_id, reason, files, bytes)"
		" SELECT x.run_id, x.reason, count(*), coalesce(sum(x.size), 0) FROM conversion_run_excluded x"
		" WHERE x.run_id = $1::bigint GROUP BY x.run_id, x.reason", 1, params, NULL, error);
}

/* The run is not listed: it says why.  A run that had a list, to which the
 * kept files were being added, is as it was before: its state, its list, and
 * the choice off (nothing of the adding was committed). */
static void
list_failed(
	AxisConversionRuns *self,
	gint64              run_id,
	const gchar        *text
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id = int_text(run_id);
	const gchar *params[2];

	params[0] = id;
	params[1] = text;
	if (!axis_db_pool_exec(self->conversions->pool, "UPDATE conversion_run SET"
	        " state = CASE WHEN relist_state IS NULL THEN 'failed' ELSE relist_state END,"
	        " list_text = left(CASE WHEN relist_state IS NULL THEN $2 ELSE"
	        "  'The files Axis keeps were not added, and the run is as it was. ' || $2 END, 2000),"
	        " finished_at = CASE WHEN relist_state IS NULL THEN now() ELSE finished_at END,"
	        " convert_kept = CASE WHEN relist_state IS NULL THEN convert_kept ELSE false END,"
	        " convert_kept_by = CASE WHEN relist_state IS NULL THEN convert_kept_by END,"
	        " convert_kept_at = CASE WHEN relist_state IS NULL THEN convert_kept_at END,"
	        " relist_state = NULL, updated_at = now() WHERE id = $1::bigint AND state = 'listing'", 2, params, NULL, &local))
		g_message("conversion runs: run %" G_GINT64_FORMAT " could not be marked: %s", run_id, local->message);
}

gboolean
axis_conversion_runs_build_list(
	AxisConversionRuns  *self,
	gint64               run_id,
	AxisJobContext      *context,
	GError             **error
){
	g_autoptr(AxisDbResult) run = NULL;
	g_autoptr(AxisProfileRevision) revision = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	g_autofree gchar *id = int_text(run_id);
	g_autofree gchar *scope = NULL;
	g_autofree gchar *roots = NULL;
	g_autofree gchar *relist = NULL;
	g_autofree gchar *chosen_by = NULL;
	AxisEstimateTarget built;
	const gchar *params[4];
	gint64 title_id = 0;
	gint64 revision_id = 0;
	gint64 cap_files = 0;
	gint64 cap_bytes = 0;
	List l;
	gboolean ok = FALSE;

	memset(&l, 0, sizeof(l));
	l.self = self;
	l.context = context;
	l.run_id = run_id;
	params[0] = id;
	if (!axis_db_pool_exec(self->conversions->pool, "SELECT state, scope, root_ids::text, coalesce(title_id, 0),"
	        " revision_id, convert_kept, relist_state, coalesce(cap_files, 0), coalesce(cap_bytes, 0), convert_kept_by"
	        " FROM conversion_run WHERE id = $1::bigint", 1, params, &run, error))
		return FALSE;
	if (axis_db_result_get_n_rows(run) != 1 || g_strcmp0(axis_db_result_get_text(run, 0, 0), "listing") != 0)
		return TRUE;
	scope = g_strdup(axis_db_result_get_text(run, 0, 1));
	roots = axis_db_result_is_null(run, 0, 2) ? NULL : g_strdup(axis_db_result_get_text(run, 0, 2));
	title_id = int_at(run, 0, 3, 0);
	revision_id = int_at(run, 0, 4, 0);
	l.convert_kept = bool_at(run, 0, 5);
	relist = axis_db_result_is_null(run, 0, 6) ? NULL : g_strdup(axis_db_result_get_text(run, 0, 6));
	/* Kept files are added only to a run that has the choice (the row's
	 * constraint says so too). */
	l.adding = relist != NULL && l.convert_kept;
	cap_files = int_at(run, 0, 7, 0);
	cap_bytes = int_at(run, 0, 8, 0);
	chosen_by = axis_db_result_is_null(run, 0, 9) ? NULL : g_strdup(axis_db_result_get_text(run, 0, 9));

	/* The revision's target, configured as every estimate's is, and the
	 * model with its calibration. */
	l.conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (l.conn == NULL)
		return FALSE;
	if (!axis_profile_store_revision_by_id(l.conn, revision_id, &revision, error))
		goto out;
	if (revision == NULL || !revision->usable || !axis_profile_revision_target(revision, &built)) {
		axis_db_pool_release(self->conversions->pool, l.conn);
		l.conn = NULL;
		list_failed(self, run_id, "The run's profile revision cannot be used to decide files (its definition is not "
			"usable).");
		return TRUE;
	}
	axis_estimator_configure(self->conversions->estimator, &built, &l.target);
	axis_db_pool_release(self->conversions->pool, l.conn);
	l.conn = NULL;
	l.model = axis_estimator_get_model(self->conversions->estimator, error);
	if (l.model == NULL)
		return FALSE;
	l.reasons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	l.conn = axis_db_pool_acquire(self->conversions->pool, error);
	if (l.conn == NULL)
		goto out;
	if (!axis_db_conn_exec(l.conn, "BEGIN ISOLATION LEVEL REPEATABLE READ", 0, NULL, NULL, error))
		goto out;
	if (!axis_db_conn_exec(l.conn, "SET LOCAL statement_timeout = '" AXIS_ESTIMATE_STATEMENT_TIMEOUT "'", 0, NULL, NULL,
	        error))
		goto rollback;
	if (l.adding) {
		g_autofree gchar *cap = NULL;
		g_autofree gchar *pause = NULL;
		const gchar *target;
		const gchar *more[4];
		gint cap_unit = -1;

		/* The list there is stays: only the files it left out as kept are
		 * decided, listed after what is there, or left out for their other
		 * reason; then no file is left out as kept. */
		if (!read_scope(&l, scope, roots, title_id, revision_id, error))
			goto rollback;
		if (context != NULL)
			axis_job_context_set_progress(context, 0.9, "adding to the list");
		if (!axis_db_conn_exec(l.conn, "DELETE FROM conversion_run_excluded WHERE run_id = $1::bigint AND reason = 'kept'",
		        1, params, NULL, error) ||
		    !add_units(&l, error) || !recount_reasons(&l, error) ||
		    !axis_conversion_run_cap_unit(l.conn, run_id, cap_files, cap_bytes, &cap_unit, error))
			goto rollback;
		/* A finished run with files to convert now waits for a person. */
		target = g_strcmp0(relist, "finished") == 0 && l.listed > 0 ? "paused" : relist;
		if (g_strcmp0(relist, "finished") == 0 && l.listed > 0)
			pause = g_strdup_printf("%s added %" G_GINT64_FORMAT " file%s Axis keeps to the list of this run, which had "
				"finished. Resume the run to convert them.", chosen_by != NULL ? chosen_by : "An administrator", l.listed,
				l.listed == 1 ? "" : "s");
		cap = cap_unit >= 0 ? int_text(cap_unit) : NULL;
		more[0] = id;
		more[1] = target;
		more[2] = cap;
		more[3] = pause;
		if (!axis_db_conn_exec(l.conn, "UPDATE conversion_run SET state = $2, relist_state = NULL, cap_unit = $3::integer,"
		        " list_text = NULL, listed_at = now(),"
		        " pause_code = CASE WHEN $4::text IS NOT NULL THEN 'person' ELSE pause_code END,"
		        " pause_text = coalesce($4::text, pause_text),"
		        " paused_at = CASE WHEN $4::text IS NOT NULL THEN now() ELSE paused_at END,"
		        " paused_by = CASE WHEN $4::text IS NOT NULL THEN convert_kept_by ELSE paused_by END,"
		        " finished_at = CASE WHEN $2 = 'finished' THEN finished_at END,"
		        " finish_text = CASE WHEN $2 = 'finished' THEN finish_text END,"
		        " failed_in_row = CASE WHEN $4::text IS NOT NULL THEN 0 ELSE failed_in_row END,"
		        " refused_in_row = CASE WHEN $4::text IS NOT NULL THEN 0 ELSE refused_in_row END,"
		        " updated_at = now() WHERE id = $1::bigint AND state = 'listing' RETURNING id", 4, more, &updated, error))
			goto rollback;
		if (axis_db_result_get_n_rows(updated) != 1)
			goto rollback_quietly;
		if (!axis_db_conn_commit(l.conn, error))
			goto rollback;
		g_message("conversion runs: run %" G_GINT64_FORMAT ": %" G_GINT64_FORMAT " of the %" G_GINT64_FORMAT " files "
			"left out as kept were added to its list (chosen by %s); the run is %s", run_id, l.listed, l.scope_files,
			chosen_by != NULL ? chosen_by : "?", target);
		ok = TRUE;
		goto out;
	}
	/* Built once: a list job run again after an interruption starts over. */
	if (!axis_db_conn_exec(l.conn, "DELETE FROM conversion_run_entry WHERE run_id = $1::bigint", 1, params, NULL,
	        error) ||
	    !axis_db_conn_exec(l.conn, "DELETE FROM conversion_run_excluded WHERE run_id = $1::bigint", 1, params, NULL,
	        error) ||
	    !axis_db_conn_exec(l.conn, "DELETE FROM conversion_run_reason WHERE run_id = $1::bigint", 1, params, NULL,
	        error) ||
	    !axis_db_conn_exec(l.conn, "DELETE FROM conversion_run_unit WHERE run_id = $1::bigint", 1, params, NULL, error))
		goto rollback;
	if (!read_scope(&l, scope, roots, title_id, revision_id, error))
		goto rollback;
	if (context != NULL)
		axis_job_context_set_progress(context, 0.9, "ranking the list");
	if (!make_units(&l, error) || !write_reasons(&l, error))
		goto rollback;
	{
		g_autofree gchar *files = int_text(l.scope_files);

		params[1] = files;
		if (!axis_db_conn_exec(l.conn, "UPDATE conversion_run SET state = 'ready', listed_at = now(),"
		        " scope_files = $2::bigint, list_text = NULL, updated_at = now() WHERE id = $1::bigint"
		        " AND state = 'listing' RETURNING id", 2, params, &updated, error))
			goto rollback;
	}
	/* Cancelled while it was listed: nothing of it is kept. */
	if (axis_db_result_get_n_rows(updated) != 1)
		goto rollback_quietly;
	if (!axis_db_conn_commit(l.conn, error))
		goto rollback;
	g_message("conversion runs: run %" G_GINT64_FORMAT " listed: %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
		" files", run_id, l.listed, l.scope_files);
	ok = TRUE;
	goto out;

rollback_quietly:
	axis_db_conn_rollback(l.conn, NULL);
	ok = TRUE;
	goto out;
rollback:
	axis_db_conn_rollback(l.conn, NULL);
	if (error != NULL && *error != NULL && !g_error_matches(*error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
		g_autofree gchar *text = g_strdup_printf("The list could not be made: %s", (*error)->message);

		axis_db_pool_release(self->conversions->pool, l.conn);
		l.conn = NULL;
		list_failed(self, run_id, text);
	}
out:
	if (l.conn != NULL)
		axis_db_pool_release(self->conversions->pool, l.conn);
	if (l.model != NULL)
		axis_estimator_model_unref(l.model);
	if (l.reasons != NULL)
		g_hash_table_unref(l.reasons);
	g_clear_object(&l.entries);
	g_clear_object(&l.excluded);
	return ok;
}
