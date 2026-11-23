/*
 * axis-estimator-refresh.c - the Savings snapshots: every target's figures
 * computed from the index in one pass
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One REPEATABLE READ transaction, so that every figure (and the
 * calibration they use) describes one moment and readers block nobody.
 * The present video and audio files are read through a cursor in the order
 * of their album units, so that an album's files arrive together and are
 * decided as one; each file is decided for every target of its kind by the
 * same functions the file page and the lists use, and only aggregates and
 * bounded lists are written.  The statements are constants; values reach
 * the database as parameters.
 *
 * The targets are the current revisions of the profiles refreshed on
 * schedule (every built-in, and every profile assigned to a root) and of
 * those an administrator asked figures for: one revision per profile, so
 * a snapshot's facts of a profile are one revision's.  The cost of a
 * refresh grows with the number of targets only in the deciding (the files
 * are read once).  The same pass, without writing anything, is the edit
 * page's preview (axis_estimator_preview()).
 */

#include "transcode/axis-estimator-load.h"

#include <string.h>

#include "jobs/axis-job-error.h"
#include "library/axis-analysis.h"

#define BATCH "2000"

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
);

/* ── Aggregates ─────────────────────────────────────────────────── */

typedef struct
{
	AxisEstimateTotal *total;
	gint64             offline_paths;
	gint64             offline_bytes;
} Agg;

static void
agg_free(gpointer data)
{
	Agg *agg = data;

	axis_estimate_total_free(agg->total);
	g_free(agg);
}

typedef struct
{
	gint64 paths;
	gint64 bytes;
	gint64 offline_paths;
} Skip;

typedef struct
{
	gint64   id;
	gchar   *path;
	gint64   root_id;
	gint64   volume_id;
	gint64   now;
	gint64   low;
	gint64   mid;
	gint64   high;
	gboolean usable;
} Top;

static void
top_free(gpointer data)
{
	Top *top = data;

	g_free(top->path);
	g_free(top);
}

typedef struct
{
	AxisEstimateTarget  target;
	GHashTable         *aggs;	/* "report\x1fdim" -> Agg */
	Skip                skips[AXIS_ESTIMATE_N_REASONS];
	gint64              scope_paths;
	gint64              scope_bytes;
	gint64              outside_paths;
	GPtrArray          *top;	/* Top, by saving, at most twice the bound until trimmed */
} TargetAgg;

typedef struct
{
	AxisEstimator     *self;
	AxisDbConn        *conn;
	AxisJobContext    *context;
	AxisEstimateModel *model;
	gint64             started_us;
	gint64             stop_us;	/* the preview stops reading here (0: never) */
	gboolean           stopped;
	gint64             snapshot;
	gchar             *snapshot_text;
	GArray            *targets;	/* TargetAgg */
	GPtrArray         *scheduled;	/* AxisProfileRevision: the key's */
	GPtrArray         *revisions;	/* AxisProfileRevision: scheduled and asked for */
	/* Coverage of the media files read. */
	gint64             media_paths;
	gint64             not_probed;
	gint64             probe_failed;
	gint64             probe_timeout;
	gint64             hdr_uncertain;
	gint64             probe_stale;
	gint64             pinned_paths;
	gint64             offline_paths;
	gint64             offline_bytes;
	GHashTable        *offline_by_volume;	/* gint64 volume -> gint64[2] paths, bytes */
	gint64             total_rows;
	gint64             done_rows;
} Refresh;

static Agg *
agg_for(
	TargetAgg   *t,
	const gchar *report,
	const gchar *dim
){
	g_autofree gchar *key = g_strdup_printf("%s\x1f%s", report, dim != NULL ? dim : "");
	Agg *agg = g_hash_table_lookup(t->aggs, key);

	if (agg == NULL) {
		agg = g_new0(Agg, 1);
		agg->total = axis_estimate_total_new();
		g_hash_table_insert(t->aggs, g_steal_pointer(&key), agg);
	}
	return agg;
}

static void
agg_add(
	Refresh                  *rf,
	TargetAgg                *t,
	const gchar              *report,
	const gchar              *dim,
	const AxisEstimateFile   *file,
	const AxisEstimateResult *result
){
	Agg *agg = agg_for(t, report, dim);

	axis_estimate_total_add(agg->total, rf->model, result);
	if (!file->index.volume_usable) {
		agg->offline_paths++;
		agg->offline_bytes += result->now;
	}
}

static gint
compare_top(
	gconstpointer a,
	gconstpointer b
){
	const Top *x = *(Top *const *)a;
	const Top *y = *(Top *const *)b;
	gint64 sx = x->now - x->mid;
	gint64 sy = y->now - y->mid;

	if (sx != sy)
		return sx > sy ? -1 : 1;
	return x->id > y->id ? -1 : x->id < y->id;
}

static void
trim_top(GPtrArray *top)
{
	g_ptr_array_sort(top, compare_top);
	if (top->len > AXIS_ESTIMATE_TOP_ITEMS)
		g_ptr_array_set_size(top, AXIS_ESTIMATE_TOP_ITEMS);
}

static void
aggregate(
	Refresh                  *rf,
	TargetAgg                *t,
	const AxisEstimateFile   *file,
	const AxisEstimateResult *result
){
	const AxisEstimateIndex *idx = &file->index;
	g_autofree gchar *volume = NULL;
	g_autofree gchar *root = NULL;
	const gchar *codec;

	if (result->reason == AXIS_ESTIMATE_NOT_APPLICABLE) {
		t->outside_paths++;
		return;
	}
	t->scope_paths++;
	if (idx->counted)
		t->scope_bytes += idx->size;

	if (!axis_estimate_result_converts(result)) {
		Skip *skip = &t->skips[result->reason];

		skip->paths++;
		if (idx->counted)
			skip->bytes += idx->size;
		if (!idx->volume_usable)
			skip->offline_paths++;
		return;
	}

	volume = g_strdup_printf("%" G_GINT64_FORMAT, idx->volume_id);
	root = g_strdup_printf("%" G_GINT64_FORMAT, idx->root_id);
	agg_add(rf, t, "total", "", file, result);
	agg_add(rf, t, "volume", volume, file, result);
	agg_add(rf, t, "root", root, file, result);
	agg_add(rf, t, "category", axis_media_type_to_string(idx->root_type), file, result);
	if (t->target.media == AXIS_TARGET_VIDEO) {
		agg_add(rf, t, "res", file->info->resolution_class != NULL ? file->info->resolution_class : "unknown", file,
			result);
		codec = file->info->video_codec;
		if (codec != NULL && strlen(codec) <= 64)
			agg_add(rf, t, "vcodec", codec, file, result);
	} else {
		codec = file->info->audio_codec;
		if (codec != NULL && strlen(codec) <= 64)
			agg_add(rf, t, "acodec", codec, file, result);
	}

	{
		Top *top = g_new0(Top, 1);

		top->id = idx->file_id;
		top->path = g_strdup(idx->rel_path);
		top->root_id = idx->root_id;
		top->volume_id = idx->volume_id;
		top->now = result->now;
		top->low = result->low;
		top->mid = result->mid;
		top->high = result->high;
		top->usable = idx->volume_usable;
		g_ptr_array_add(t->top, top);
		if (t->top->len >= 2 * AXIS_ESTIMATE_TOP_ITEMS)
			trim_top(t->top);
	}
}

/* ── Coverage of what was read ──────────────────────────────────── */

static void
cover(
	Refresh                *rf,
	const AxisEstimateFile *file
){
	const AxisEstimateIndex *idx = &file->index;
	const AxisMediaInfo *info = file->info;

	rf->media_paths++;
	if (info == NULL)
		rf->not_probed++;
	else if (g_strcmp0(info->status, "failed") == 0)
		rf->probe_failed++;
	else if (g_strcmp0(info->status, "timeout") == 0)
		rf->probe_timeout++;
	else if (idx->changed)
		rf->probe_stale++;
	if (info != NULL && info->hdr_uncertain)
		rf->hdr_uncertain++;
	if (idx->pinned)
		rf->pinned_paths++;
	if (!idx->volume_usable) {
		gint64 *counts = g_hash_table_lookup(rf->offline_by_volume, &idx->volume_id);

		if (counts == NULL) {
			gint64 *key = g_new(gint64, 1);

			*key = idx->volume_id;
			counts = g_new0(gint64, 2);
			g_hash_table_insert(rf->offline_by_volume, key, counts);
		}
		rf->offline_paths++;
		counts[0]++;
		if (idx->counted) {
			rf->offline_bytes += idx->size;
			counts[1] += idx->size;
		}
	}
}

/* ── Deciding ───────────────────────────────────────────────────── */

static void
decide_single(
	Refresh                *rf,
	const AxisEstimateFile *file
){
	guint i;

	cover(rf, file);
	for (i = 0; i < rf->targets->len; i++) {
		TargetAgg *t = &g_array_index(rf->targets, TargetAgg, i);
		AxisEstimateResult result;

		axis_estimate_eligibility(file, &t->target, rf->model, &result);
		aggregate(rf, t, file, &result);
	}
}

/* An album unit, whole: every file of it is in @files (the cursor reads
 * units in order), so it is told its size. */
static void
decide_unit(
	Refresh   *rf,
	GPtrArray *files
){
	g_autofree AxisEstimateResult *results = NULL;
	guint i;
	guint t;

	if (files->len == 0)
		return;
	for (i = 0; i < files->len; i++) {
		AxisEstimateFile *file = g_ptr_array_index(files, i);

		file->index.unit_files = files->len;
		cover(rf, file);
	}
	results = g_new0(AxisEstimateResult, files->len);
	for (t = 0; t < rf->targets->len; t++) {
		TargetAgg *target = &g_array_index(rf->targets, TargetAgg, t);

		axis_estimate_evaluate((const AxisEstimateFile *const *)files->pdata, files->len, &target->target,
			rf->model, results);
		for (i = 0; i < files->len; i++)
			aggregate(rf, target, g_ptr_array_index(files, i), &results[i]);
	}
}

/* ── Running statements ─────────────────────────────────────────── */

static gboolean
checkpoint(
	Refresh  *rf,
	GError  **error
){
	if (rf->context != NULL && axis_job_context_is_cancelled(rf->context)) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "the refresh was stopped");
		return FALSE;
	}
	if (g_get_monotonic_time() - rf->started_us > (gint64)AXIS_ESTIMATE_DEADLINE_SECONDS * G_USEC_PER_SEC) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_DEADLINE,
			"the refresh took longer than its deadline");
		return FALSE;
	}
	return TRUE;
}

static void
progress(
	Refresh     *rf,
	const gchar *message
){
	gdouble fraction = rf->total_rows > 0 ? 0.05 + 0.9 * (gdouble)rf->done_rows / (gdouble)rf->total_rows : 0.5;

	if (rf->context != NULL)
		axis_job_context_set_progress(rf->context, MIN(fraction, 0.99), message);
}

/* ── Reading the files ──────────────────────────────────────────── */

#define SQL_CURSOR \
	"DECLARE axis_estimate_files NO SCROLL CURSOR FOR" \
	" SELECT " AXIS_ESTIMATE_SQL_COLUMNS_BASE ", 0 " AXIS_ESTIMATE_SQL_FROM \
	" WHERE f.state = 'present' AND f.kind IN ('video', 'audio')" \
	" ORDER BY f.root_id, " AXIS_ESTIMATE_SQL_UNIT " NULLS FIRST, f.id"

static gboolean
same_unit(
	const AxisEstimateFile *a,
	const AxisEstimateFile *b
){
	return a->index.root_id == b->index.root_id && g_strcmp0(a->index.unit, b->index.unit) == 0;
}

static gboolean
read_files(
	Refresh  *rf,
	GError  **error
){
	/* The batches the current unit's files are in (a unit may span two). */
	g_autoptr(GPtrArray) held = g_ptr_array_new_with_free_func((GDestroyNotify)axis_estimate_rows_free);
	g_autoptr(GPtrArray) unit = g_ptr_array_new();

	if (!axis_db_conn_exec(rf->conn, SQL_CURSOR, 0, NULL, NULL, error))
		return FALSE;

	for (;;) {
		g_autoptr(AxisDbResult) result = NULL;
		AxisEstimateRows *batch;
		guint n;
		guint i;

		if (!checkpoint(rf, error))
			return FALSE;
		/* The preview reads what it can in its time. */
		if (rf->stop_us > 0 && g_get_monotonic_time() > rf->stop_us) {
			rf->stopped = TRUE;
			break;
		}
		if (!axis_db_conn_exec(rf->conn, "FETCH " BATCH " FROM axis_estimate_files", 0, NULL, &result, error))
			return FALSE;
		n = axis_db_result_get_n_rows(result);
		if (n == 0)
			break;

		batch = axis_estimate_rows_new();
		for (i = 0; i < n; i++)
			g_ptr_array_add(batch->owned, axis_estimator_read_row(batch, result, i));
		if (!axis_estimator_attach_streams(rf->conn, batch, 0, error)) {
			axis_estimate_rows_free(batch);
			return FALSE;
		}
		g_clear_pointer(&result, axis_db_result_free);

		for (i = 0; i < batch->owned->len; i++) {
			AxisEstimateFile *file = &((AxisEstimateLoaded *)g_ptr_array_index(batch->owned, i))->file;

			if (unit->len > 0 && (file->index.unit == NULL || !same_unit(g_ptr_array_index(unit, 0), file))) {
				decide_unit(rf, unit);
				g_ptr_array_set_size(unit, 0);
				g_ptr_array_set_size(held, 0);
			}
			if (file->index.unit != NULL)
				g_ptr_array_add(unit, file);
			else
				decide_single(rf, file);
		}
		/* Kept while the unit it ends with may go on in the next batch. */
		if (unit->len > 0)
			g_ptr_array_add(held, batch);
		else
			axis_estimate_rows_free(batch);
		rf->done_rows += n;
		progress(rf, "deciding the files");
	}
	/* An album cut off by the preview's stop is not decided at all: part
	 * of an album is never taken for the whole. */
	if (!rf->stopped)
		decide_unit(rf, unit);
	g_ptr_array_set_size(unit, 0);
	return axis_db_conn_exec(rf->conn, "CLOSE axis_estimate_files", 0, NULL, NULL, error);
}

/* ── Writing the facts ──────────────────────────────────────────── */

static gchar *
builder_text(JsonBuilder *builder)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();
	JsonNode *root = json_builder_get_root(builder);
	gchar *text;

	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);
	json_node_unref(root);
	return text;
}

static void
add_int(
	JsonBuilder *builder,
	const gchar *name,
	gint64       value
){
	json_builder_set_member_name(builder, name);
	json_builder_add_int_value(builder, value);
}

static void
add_string(
	JsonBuilder *builder,
	const gchar *name,
	const gchar *value
){
	json_builder_set_member_name(builder, name);
	if (value != NULL)
		json_builder_add_string_value(builder, value);
	else
		json_builder_add_null_value(builder);
}

static gboolean
insert_fact(
	Refresh      *rf,
	gint64        revision_id,
	const gchar  *report,
	const gchar  *dim,
	gint64        paths,
	gint64        bytes,
	gint64        low,
	gint64        mid,
	gint64        high,
	const gchar  *extra,
	GError      **error
){
	g_autofree gchar *revision = g_strdup_printf("%" G_GINT64_FORMAT, revision_id);
	g_autofree gchar *p = g_strdup_printf("%" G_GINT64_FORMAT, paths);
	g_autofree gchar *b = g_strdup_printf("%" G_GINT64_FORMAT, bytes);
	g_autofree gchar *l = g_strdup_printf("%" G_GINT64_FORMAT, low);
	g_autofree gchar *m = g_strdup_printf("%" G_GINT64_FORMAT, mid);
	g_autofree gchar *h = g_strdup_printf("%" G_GINT64_FORMAT, high);
	const gchar *params[10];

	if (!checkpoint(rf, error))
		return FALSE;
	params[0] = rf->snapshot_text;
	params[1] = revision;
	params[2] = report;
	params[3] = dim != NULL ? dim : "";
	params[4] = p;
	params[5] = b;
	params[6] = l;
	params[7] = m;
	params[8] = h;
	params[9] = extra != NULL ? extra : "{}";
	return axis_db_conn_exec(rf->conn,
		"INSERT INTO estimate_fact (snapshot_id, revision_id, report, dim, paths, bytes, low, mid, high, extra)"
		" VALUES ($1::bigint, $2::bigint, $3::text, $4::text, $5::bigint, $6::bigint, $7::bigint, $8::bigint,"
		" $9::bigint, $10::jsonb)",
		10, params, NULL, error);
}

/* The calibration of the cells a figure's files came from (the target's
 * own cells when it has no file). */
static AxisCalibrationState
state_of(
	Refresh                  *rf,
	const AxisEstimateTarget *target,
	const GArray             *cells,
	guint                    *samples
){
	guint fallback[16];
	const guint *list;
	guint n;
	guint i;

	if (cells != NULL && cells->len > 0) {
		list = (const guint *)cells->data;
		n = cells->len;
	} else {
		n = axis_estimate_model_target_cells(rf->model, target, fallback, G_N_ELEMENTS(fallback));
		list = fallback;
	}
	*samples = 0;
	for (i = 0; i < n; i++) {
		AxisEstimateCellInfo info;

		axis_estimate_model_cell_info(rf->model, list[i], &info);
		*samples += info.samples;
	}
	return axis_estimate_model_state_of(rf->model, list, n);
}

static gchar *
cells_extra(
	Refresh      *rf,
	const GArray *cells,
	JsonBuilder  *builder
){
	guint i;

	json_builder_set_member_name(builder, "cells");
	json_builder_begin_array(builder);
	for (i = 0; cells != NULL && i < cells->len && i < 64; i++) {
		AxisEstimateCellInfo info;

		axis_estimate_model_cell_info(rf->model, g_array_index(cells, guint, i), &info);
		json_builder_begin_object(builder);
		add_string(builder, "name", info.name);
		add_int(builder, "samples", info.samples);
		add_string(builder, "state", axis_calibration_state_to_string(info.state));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	return NULL;
}

static gboolean
write_agg(
	Refresh      *rf,
	TargetAgg    *t,
	const gchar  *report,
	const gchar  *dim,
	Agg          *agg,
	JsonObject   *more,
	GError      **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *extra = NULL;
	gint64 files = 0;
	gint64 now = 0;
	gint64 low = 0;
	gint64 mid = 0;
	gint64 high = 0;
	guint samples = 0;
	AxisCalibrationState state;

	if (agg != NULL)
		axis_estimate_total_get(agg->total, &files, &now, &low, &mid, &high);
	state = state_of(rf, &t->target, agg != NULL ? axis_estimate_total_cells(agg->total) : NULL, &samples);

	json_builder_begin_object(builder);
	add_int(builder, "offline_paths", agg != NULL ? agg->offline_paths : 0);
	add_int(builder, "offline_bytes", agg != NULL ? agg->offline_bytes : 0);
	add_string(builder, "calibration", axis_calibration_state_to_string(state));
	add_int(builder, "samples", samples);
	if (more != NULL) {
		GList *members = json_object_get_members(more);
		GList *l;

		for (l = members; l != NULL; l = l->next) {
			json_builder_set_member_name(builder, l->data);
			json_builder_add_value(builder, json_node_copy(json_object_get_member(more, l->data)));
		}
		g_list_free(members);
	}
	json_builder_end_object(builder);
	extra = builder_text(builder);
	return insert_fact(rf, t->target.revision_id, report, dim, files, now, low, mid, high, extra, error);
}

/* The volumes and roots, with what the pages show of them. */
static gboolean
write_places(
	Refresh    *rf,
	TargetAgg  *t,
	GError    **error
){
	g_autoptr(AxisDbResult) volumes = NULL;
	g_autoptr(AxisDbResult) roots = NULL;
	guint i;

	if (!axis_db_conn_exec(rf->conn,
	        "SELECT id, name, state, state IN ('online', 'read_only'), total_bytes, free_bytes,"
	        " (extract(epoch FROM space_checked_at) * 1000000)::bigint FROM volume ORDER BY id",
	        0, NULL, &volumes, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(volumes); i++) {
		g_autoptr(JsonObject) more = json_object_new();
		const gchar *id = axis_db_result_get_text(volumes, i, 0);
		g_autofree gchar *key = g_strdup_printf("volume\x1f%s", id);
		gboolean usable = FALSE;
		gint64 value = 0;

		json_object_set_string_member(more, "name", axis_db_result_get_text(volumes, i, 1));
		json_object_set_string_member(more, "state", axis_db_result_get_text(volumes, i, 2));
		axis_db_result_get_boolean(volumes, i, 3, &usable);
		json_object_set_boolean_member(more, "usable", usable);
		if (axis_db_result_get_int64(volumes, i, 4, &value))
			json_object_set_int_member(more, "total_bytes", value);
		if (axis_db_result_get_int64(volumes, i, 5, &value))
			json_object_set_int_member(more, "free_bytes", value);
		if (axis_db_result_get_int64(volumes, i, 6, &value))
			json_object_set_int_member(more, "space_checked_us", value);
		if (!write_agg(rf, t, "volume", id, g_hash_table_lookup(t->aggs, key), more, error))
			return FALSE;
	}

	if (!axis_db_conn_exec(rf->conn, "SELECT id, name, media_type, volume_id FROM library_root ORDER BY id", 0, NULL,
	        &roots, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(roots); i++) {
		g_autoptr(JsonObject) more = json_object_new();
		const gchar *id = axis_db_result_get_text(roots, i, 0);
		g_autofree gchar *key = g_strdup_printf("root\x1f%s", id);
		Agg *agg = g_hash_table_lookup(t->aggs, key);
		gint64 volume = 0;

		/* A root the target is not for is left out. */
		if (agg == NULL) {
			AxisMediaType type;

			if (!axis_media_type_from_string(axis_db_result_get_text(roots, i, 2), &type) ||
			    (t->target.root_types & AXIS_TARGET_ROOT(type)) == 0)
				continue;
		}
		json_object_set_string_member(more, "name", axis_db_result_get_text(roots, i, 1));
		json_object_set_string_member(more, "media_type", axis_db_result_get_text(roots, i, 2));
		axis_db_result_get_int64(roots, i, 3, &volume);
		json_object_set_int_member(more, "volume_id", volume);
		if (!write_agg(rf, t, "root", id, agg, more, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
write_target(
	Refresh    *rf,
	TargetAgg  *t,
	GError    **error
){
	static const gchar *const breakdowns[] = { "category", "res", "vcodec", "acodec", NULL };
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	/* The total, with what is in and out of the target's scope. */
	{
		g_autoptr(JsonObject) more = json_object_new();

		json_object_set_int_member(more, "scope_paths", t->scope_paths);
		json_object_set_int_member(more, "scope_bytes", t->scope_bytes);
		json_object_set_int_member(more, "outside_paths", t->outside_paths);
		if (!write_agg(rf, t, "total", "", g_hash_table_lookup(t->aggs, "total\x1f"), more, error))
			return FALSE;
	}
	if (!write_places(rf, t, error))
		return FALSE;

	g_hash_table_iter_init(&iter, t->aggs);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const gchar *sep = strchr(key, '\x1f');
		g_autofree gchar *report = g_strndup(key, (gsize)(sep - (const gchar *)key));

		if (!g_strv_contains(breakdowns, report))
			continue;
		if (!write_agg(rf, t, report, sep + 1, value, NULL, error))
			return FALSE;
	}

	for (i = 1; i < AXIS_ESTIMATE_N_REASONS; i++) {
		g_autofree gchar *extra = NULL;

		if (t->skips[i].paths == 0)
			continue;
		extra = g_strdup_printf("{\"offline_paths\": %" G_GINT64_FORMAT "}", t->skips[i].offline_paths);
		if (!insert_fact(rf, t->target.revision_id, "skip", axis_estimate_reason_to_string((AxisEstimateReason)i),
		        t->skips[i].paths, t->skips[i].bytes, 0, 0, 0, extra, error))
			return FALSE;
	}

	/* The files with the largest estimated saving. */
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_autofree gchar *extra = NULL;
		gint64 bytes = 0;
		gint64 low = 0;
		gint64 mid = 0;
		gint64 high = 0;

		trim_top(t->top);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "items");
		json_builder_begin_array(builder);
		for (i = 0; i < t->top->len; i++) {
			const Top *top = g_ptr_array_index(t->top, i);

			json_builder_begin_object(builder);
			add_int(builder, "id", top->id);
			add_string(builder, "path", top->path);
			add_int(builder, "root_id", top->root_id);
			add_int(builder, "volume_id", top->volume_id);
			add_int(builder, "now", top->now);
			add_int(builder, "low", top->low);
			add_int(builder, "mid", top->mid);
			add_int(builder, "high", top->high);
			json_builder_set_member_name(builder, "usable");
			json_builder_add_boolean_value(builder, top->usable);
			json_builder_end_object(builder);
			bytes += top->now;
			low += top->low;
			mid += top->mid;
			high += top->high;
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
		extra = builder_text(builder);
		if (!insert_fact(rf, t->target.revision_id, "top", "", t->top->len, bytes, low, mid, high, extra, error))
			return FALSE;
	}
	return TRUE;
}

/* What the snapshot could not see, in the keys the Analysis coverage block
 * reads (axis_web_analysis_coverage()). */
static gboolean
write_coverage(
	Refresh      *rf,
	gchar       **coverage,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) away = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonBuilder) builder = json_builder_new();
	JsonObject *base;
	GList *members;
	GList *l;
	guint i;

	if (!axis_db_conn_exec(rf->conn,
	        "SELECT jsonb_build_object("
	        " 'present_paths', (SELECT count(*) FROM library_file WHERE state = 'present'),"
	        " 'suspected_paths', count(*) FILTER (WHERE state = 'suspected'),"
	        " 'suspected_bytes', coalesce(sum(size) FILTER (WHERE state = 'suspected'), 0),"
	        " 'missing_paths', count(*) FILTER (WHERE state = 'missing'),"
	        " 'missing_bytes', coalesce(sum(size) FILTER (WHERE state = 'missing'), 0),"
	        " 'roots', (SELECT count(*) FROM library_root),"
	        " 'roots_unscanned', (SELECT coalesce(jsonb_agg(jsonb_build_object('id', r.id, 'name', r.name)"
	        "   ORDER BY r.id), '[]'::jsonb) FROM library_root r"
	        "   LEFT JOIN library_root_state st ON st.root_id = r.id WHERE coalesce(st.complete_generation, 0) = 0))::text"
	        " FROM library_file WHERE state <> 'present'",
	        0, NULL, &result, error) ||
	    !axis_db_conn_exec(rf->conn, "SELECT id, name, state FROM volume WHERE state NOT IN ('online', 'read_only')"
	        " ORDER BY id", 0, NULL, &away, error))
		return FALSE;
	if (!json_parser_load_from_data(parser, axis_db_result_get_text(result, 0, 0), -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "the coverage is not readable");
		return FALSE;
	}
	base = json_node_get_object(json_parser_get_root(parser));

	json_builder_begin_object(builder);
	members = json_object_get_members(base);
	for (l = members; l != NULL; l = l->next) {
		json_builder_set_member_name(builder, l->data);
		json_builder_add_value(builder, json_node_copy(json_object_get_member(base, l->data)));
	}
	g_list_free(members);
	/* The video and audio files of each volume that is away, counted as
	 * the index last knew them (hard links once). */
	json_builder_set_member_name(builder, "volumes_unavailable");
	json_builder_begin_array(builder);
	for (i = 0; i < axis_db_result_get_n_rows(away); i++) {
		gint64 id = int_at(away, i, 0);
		const gint64 *counts = g_hash_table_lookup(rf->offline_by_volume, &id);

		json_builder_begin_object(builder);
		add_int(builder, "id", id);
		add_string(builder, "name", axis_db_result_get_text(away, i, 1));
		add_string(builder, "state", axis_db_result_get_text(away, i, 2));
		add_int(builder, "paths", counts != NULL ? counts[0] : 0);
		add_int(builder, "bytes", counts != NULL ? counts[1] : 0);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	add_int(builder, "media_paths", rf->media_paths);
	add_int(builder, "not_probed", rf->not_probed);
	add_int(builder, "probe_failed", rf->probe_failed);
	add_int(builder, "probe_timeout", rf->probe_timeout);
	add_int(builder, "hdr_uncertain", rf->hdr_uncertain);
	add_int(builder, "probe_stale", rf->probe_stale);
	add_int(builder, "pinned_paths", rf->pinned_paths);
	add_int(builder, "offline_paths", rf->offline_paths);
	add_int(builder, "offline_bytes", rf->offline_bytes);
	json_builder_end_object(builder);
	*coverage = builder_text(builder);
	return TRUE;
}

static gchar *
config_text(Refresh *rf)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	/* The key of what is refreshed on schedule: figures asked for besides
	 * do not make the next scheduled refresh due again. */
	g_autofree gchar *key = axis_estimator_config_key(rf->self, rf->scheduled);
	guint i;

	json_builder_begin_object(builder);
	add_string(builder, "key", key);
	add_int(builder, "model_version", AXIS_ESTIMATE_MODEL_VERSION);
	json_builder_set_member_name(builder, "targets");
	json_builder_begin_object(builder);
	for (i = 0; i < rf->targets->len; i++) {
		TargetAgg *t = &g_array_index(rf->targets, TargetAgg, i);
		Agg *agg = g_hash_table_lookup(t->aggs, "total\x1f");
		const GArray *cells = agg != NULL ? axis_estimate_total_cells(agg->total) : NULL;
		g_autofree gchar *description = axis_estimate_target_describe(&t->target);
		guint samples = 0;
		AxisCalibrationState state = state_of(rf, &t->target, cells, &samples);
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

		json_builder_set_member_name(builder, t->target.id);
		json_builder_begin_object(builder);
		add_int(builder, "revision_id", t->target.revision_id);
		add_int(builder, "revision", t->target.revision);
		add_string(builder, "name", t->target.name);
		add_string(builder, "family", t->target.family_name);
		add_string(builder, "description", description);
		add_string(builder, "encoder", axis_encoder_family_to_string(t->target.encoder));
		add_int(builder, "quality", t->target.quality);
		g_ascii_formatd(buffer, sizeof(buffer), "%.4f", t->target.min_saving);
		add_string(builder, "min_saving", buffer);
		add_int(builder, "efficient_milli", t->target.efficient_milli);
		add_string(builder, "calibration", axis_calibration_state_to_string(state));
		add_int(builder, "samples", samples);
		cells_extra(rf, cells, builder);
		json_builder_end_object(builder);
	}
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	return builder_text(builder);
}

/* ── The refresh ────────────────────────────────────────────────── */

static void
add_target(
	Refresh                  *rf,
	const AxisEstimateTarget *target
){
	TargetAgg t;

	memset(&t, 0, sizeof(t));
	t.target = *target;
	t.aggs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, agg_free);
	t.top = g_ptr_array_new_with_free_func(top_free);
	g_array_append_val(rf->targets, t);
}

/* The revisions refreshed (scheduled and asked for) and their targets; a
 * revision whose definition is not usable gives none. */
static gboolean
load_targets(
	Refresh  *rf,
	GError  **error
){
	g_autoptr(GArray) targets = NULL;
	guint i;

	if (!axis_estimator_scheduled(rf->conn, FALSE, &rf->scheduled, error) ||
	    !axis_estimator_scheduled(rf->conn, TRUE, &rf->revisions, error))
		return FALSE;
	targets = axis_estimator_targets_of(rf->self, rf->revisions);
	for (i = 0; i < targets->len; i++)
		add_target(rf, &g_array_index(targets, AxisEstimateTarget, i));
	return TRUE;
}

static void
refresh_clear(Refresh *rf)
{
	guint i;

	for (i = 0; rf->targets != NULL && i < rf->targets->len; i++) {
		TargetAgg *t = &g_array_index(rf->targets, TargetAgg, i);

		g_hash_table_unref(t->aggs);
		g_ptr_array_unref(t->top);
	}
	g_clear_pointer(&rf->targets, g_array_unref);
	g_clear_pointer(&rf->scheduled, g_ptr_array_unref);
	g_clear_pointer(&rf->revisions, g_ptr_array_unref);
	axis_estimate_model_free(rf->model);
	rf->model = NULL;
	g_clear_pointer(&rf->offline_by_volume, g_hash_table_unref);
	g_clear_pointer(&rf->snapshot_text, g_free);
}

static gboolean
refresh_in_transaction(
	Refresh      *rf,
	const gchar  *trigger,
	gint64        job_id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *job = job_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, job_id) : NULL;
	g_autofree gchar *coverage = NULL;
	g_autofree gchar *config = NULL;
	g_autofree gchar *took = NULL;
	g_autofree gchar *newest_text = NULL;
	gint64 analysis_id = 0;
	gint64 newest = 0;
	gboolean locked = FALSE;
	const gchar *params[6];
	guint i;

	if (!axis_db_conn_begin(rf->conn, error))
		return FALSE;
	if (!axis_db_conn_exec(rf->conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", 0, NULL, NULL, error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL statement_timeout = '" AXIS_ESTIMATE_STATEMENT_TIMEOUT "'", 0,
	        NULL, NULL, error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL idle_in_transaction_session_timeout = '30min'", 0, NULL, NULL,
	        error) ||
	    !axis_db_conn_exec(rf->conn, "SET LOCAL work_mem = '128MB'", 0, NULL, NULL, error))
		return FALSE;
	if (!axis_db_conn_exec(rf->conn, "SELECT pg_try_advisory_xact_lock(hashtext('axis.estimate.refresh'), 0)", 0,
	        NULL, &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, &locked);
	g_clear_pointer(&result, axis_db_result_free);
	if (!locked) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_BUSY, "another refresh is running");
		return FALSE;
	}

	/* The calibration and the profiles as of the same moment. */
	if (!axis_estimator_read_model(rf->conn, TRUE, &rf->model, &newest, error))
		return FALSE;
	if (!load_targets(rf, error))
		return FALSE;
	if (!axis_db_conn_exec(rf->conn,
	        "SELECT coalesce((SELECT max(id) FROM analysis_snapshot), 0),"
	        " (SELECT count(*) FROM library_file WHERE state = 'present' AND kind IN ('video', 'audio'))",
	        0, NULL, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &analysis_id);
	axis_db_result_get_int64(result, 0, 1, &rf->total_rows);
	g_clear_pointer(&result, axis_db_result_free);

	newest_text = g_strdup_printf("%" G_GINT64_FORMAT, newest);
	{
		g_autofree gchar *analysis = analysis_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, analysis_id) : NULL;

		params[0] = trigger;
		params[1] = job;
		params[2] = analysis;
		params[3] = newest_text;
		if (!axis_db_conn_exec(rf->conn,
		        "INSERT INTO estimate_snapshot (taken_at, trigger, job_id, took_ms, analysis_snapshot_id,"
		        " last_sample_id, coverage, config)"
		        " VALUES (clock_timestamp(), $1::text, $2::bigint, 0, $3::bigint, $4::bigint, '{}'::jsonb,"
		        " '{}'::jsonb) RETURNING id",
		        4, params, &result, error))
			return FALSE;
	}
	axis_db_result_get_int64(result, 0, 0, &rf->snapshot);
	rf->snapshot_text = g_strdup_printf("%" G_GINT64_FORMAT, rf->snapshot);
	g_clear_pointer(&result, axis_db_result_free);
	progress(rf, "reading the files");

	if (!read_files(rf, error))
		return FALSE;

	progress(rf, "writing the figures");
	for (i = 0; i < rf->targets->len; i++) {
		if (!write_target(rf, &g_array_index(rf->targets, TargetAgg, i), error))
			return FALSE;
	}
	if (!write_coverage(rf, &coverage, error))
		return FALSE;
	config = config_text(rf);
	took = g_strdup_printf("%" G_GINT64_FORMAT, (g_get_monotonic_time() - rf->started_us) / 1000);
	params[0] = rf->snapshot_text;
	params[1] = coverage;
	params[2] = config;
	params[3] = took;
	if (!axis_db_conn_exec(rf->conn,
	        "UPDATE estimate_snapshot SET coverage = $2::jsonb, config = $3::jsonb,"
	        " took_ms = LEAST($4::bigint, 2000000000), finished_at = clock_timestamp() WHERE id = $1::bigint",
	        4, params, NULL, error))
		return FALSE;
	return axis_db_conn_commit(rf->conn, error);
}

/* The newest snapshot is always kept, the others for seven days; in a
 * transaction of its own after the snapshot is committed. */
static void
apply_retention(
	AxisEstimator *self,
	gint64         newest
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *keep = g_strdup_printf("%" G_GINT64_FORMAT, newest);
	const gchar *params[1];

	params[0] = keep;
	if (!axis_db_pool_exec(self->pool,
	        "DELETE FROM estimate_snapshot WHERE id <> $1::bigint AND taken_at < now() - interval '7 days'",
	        1, params, NULL, &error))
		g_warning("estimate: cannot apply the retention of snapshots: %s", error->message);
}

/* The requests the snapshot answered, after it is committed: those of its
 * profiles made before it was taken (one made since stays for the next). */
static void
forget_requests(Refresh *rf)
{
	g_autoptr(GError) error = NULL;
	GString *profiles = g_string_new("{");
	const gchar *params[2];
	guint i;

	for (i = 0; rf->revisions != NULL && i < rf->revisions->len; i++) {
		const AxisProfileRevision *rev = g_ptr_array_index(rf->revisions, i);

		g_string_append_printf(profiles, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", rev->profile_id);
	}
	g_string_append_c(profiles, '}');
	params[0] = profiles->str;
	params[1] = rf->snapshot_text;
	if (!axis_db_pool_exec(rf->self->pool,
	        "DELETE FROM estimate_refresh_request q USING conversion_profile_revision r"
	        " WHERE r.id = q.revision_id AND r.profile_id = ANY($1::bigint[])"
	        " AND q.requested_at <= (SELECT taken_at FROM estimate_snapshot WHERE id = $2::bigint)",
	        2, params, NULL, &error))
		g_warning("estimate: cannot clear the figures asked for: %s", error->message);
	g_string_free(profiles, TRUE);
}

gboolean
axis_estimator_refresh(
	AxisEstimator   *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
){
	Refresh rf;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (snapshot_id != NULL)
		*snapshot_id = 0;
	if (g_strcmp0(trigger, "schedule") != 0 && g_strcmp0(trigger, "request") != 0 &&
	    g_strcmp0(trigger, "direct") != 0) {
		g_set_error_literal(error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED,
			"the trigger is not one of schedule, request and direct");
		return FALSE;
	}

	memset(&rf, 0, sizeof(rf));
	rf.self = self;
	rf.context = context;
	rf.started_us = g_get_monotonic_time();
	rf.offline_by_volume = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	rf.targets = g_array_new(FALSE, TRUE, sizeof(TargetAgg));

	rf.conn = axis_db_pool_acquire(self->pool, error);
	if (rf.conn == NULL) {
		ok = FALSE;
	} else {
		ok = refresh_in_transaction(&rf, trigger, job_id, error);
		if (!ok)
			axis_db_conn_rollback(rf.conn, NULL);
		axis_db_pool_release(self->pool, rf.conn);
	}
	if (ok) {
		if (snapshot_id != NULL)
			*snapshot_id = rf.snapshot;
		apply_retention(self, rf.snapshot);
		forget_requests(&rf);
	}
	refresh_clear(&rf);
	return ok;
}

gboolean
axis_estimator_preview(
	AxisEstimator            *self,
	const AxisEstimateTarget *before,
	const AxisEstimateTarget *after,
	guint                     deadline_ms,
	AxisEstimatePreview      *preview,
	GError                  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	Refresh rf;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && before != NULL && after != NULL && preview != NULL, FALSE);

	memset(preview, 0, sizeof(*preview));
	memset(&rf, 0, sizeof(rf));
	rf.self = self;
	rf.started_us = g_get_monotonic_time();
	rf.stop_us = rf.started_us + (gint64)deadline_ms * 1000;
	rf.offline_by_volume = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	rf.targets = g_array_new(FALSE, TRUE, sizeof(TargetAgg));
	add_target(&rf, before);
	add_target(&rf, after);
	rf.model = axis_estimator_get_model(self, error);
	if (rf.model == NULL) {
		refresh_clear(&rf);
		return FALSE;
	}
	rf.conn = axis_db_pool_acquire(self->pool, error);
	if (rf.conn == NULL) {
		refresh_clear(&rf);
		return FALSE;
	}
	ok = axis_db_conn_begin(rf.conn, error) &&
		axis_db_conn_exec(rf.conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ, READ ONLY", 0, NULL, NULL,
			error) &&
		axis_db_conn_exec(rf.conn, "SET LOCAL statement_timeout = '30s'", 0, NULL, NULL, error) &&
		axis_db_conn_exec(rf.conn, "SELECT count(*) FROM library_file WHERE state = 'present'"
			" AND kind IN ('video', 'audio')", 0, NULL, &result, error);
	if (ok) {
		axis_db_result_get_int64(result, 0, 0, &preview->total);
		ok = read_files(&rf, error);
	}
	axis_db_conn_rollback(rf.conn, NULL);
	axis_db_pool_release(self->pool, rf.conn);
	if (ok) {
		for (i = 0; i < 2; i++) {
			TargetAgg *t = &g_array_index(rf.targets, TargetAgg, i);
			AxisEstimateCounts *counts = i == 0 ? &preview->before : &preview->after;
			Agg *total = g_hash_table_lookup(t->aggs, "total\x1f");
			guint r;

			counts->scope = t->scope_paths;
			if (total != NULL)
				axis_estimate_total_get(total->total, &counts->converts, &counts->now, NULL, &counts->mid, NULL);
			for (r = 0; r < AXIS_ESTIMATE_N_REASONS; r++)
				counts->skips[r] = t->skips[r].paths;
		}
		preview->read = rf.media_paths;
		preview->complete = !rf.stopped;
	}
	refresh_clear(&rf);
	return ok;
}

/* ── Reading ────────────────────────────────────────────────────── */

static void
fact_free(gpointer data)
{
	AxisEstimateFact *fact = data;

	g_free(fact->target);
	g_free(fact->report);
	g_free(fact->dim);
	if (fact->extra != NULL)
		json_object_unref(fact->extra);
	g_free(fact);
}

void
axis_estimate_snapshot_free(AxisEstimateSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;
	g_free(snapshot->trigger);
	if (snapshot->coverage != NULL)
		json_object_unref(snapshot->coverage);
	if (snapshot->config != NULL)
		json_object_unref(snapshot->config);
	g_clear_pointer(&snapshot->facts, g_ptr_array_unref);
	g_free(snapshot);
}

/* A JSON object from text written by this code, never NULL. */
static JsonObject *
object_of(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL)) {
		root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
			return json_node_dup_object(root);
	}
	return json_object_new();
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

gboolean
axis_estimator_load_snapshot(
	AxisEstimator          *self,
	const gchar            *target,
	AxisEstimateSnapshot  **snapshot,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) facts = NULL;
	g_autofree gchar *id = NULL;
	AxisEstimateSnapshot *out;
	const gchar *params[2];
	AxisDbConn *conn;
	guint i;

	g_return_val_if_fail(self != NULL && snapshot != NULL, FALSE);

	*snapshot = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto fail;
	/* For a profile, the newest snapshot with its figures: one that is not
	 * refreshed on schedule has them only when they were asked for. */
	params[0] = target;
	if (!axis_db_conn_exec(conn,
	        "SELECT s.id, (extract(epoch FROM s.taken_at) * 1000000)::bigint,"
	        " (extract(epoch FROM s.finished_at) * 1000000)::bigint, s.trigger, s.took_ms, s.coverage::text,"
	        " s.config::text FROM estimate_snapshot s"
	        " WHERE $1::text IS NULL OR EXISTS (SELECT 1 FROM estimate_fact f"
	        "  JOIN conversion_profile_revision r ON r.id = f.revision_id JOIN conversion_profile p ON p.id = r.profile_id"
	        "  WHERE f.snapshot_id = s.id AND p.slug = $1::text)"
	        " ORDER BY s.taken_at DESC, s.id DESC LIMIT 1",
	        1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) == 0) {
		axis_db_conn_commit(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		return TRUE;
	}
	out = g_new0(AxisEstimateSnapshot, 1);
	out->id = int_at(result, 0, 0);
	out->taken_us = int_at(result, 0, 1);
	out->finished_us = int_at(result, 0, 2);
	out->trigger = g_strdup(axis_db_result_get_text(result, 0, 3));
	out->took_ms = int_at(result, 0, 4);
	out->coverage = object_of(axis_db_result_get_text(result, 0, 5));
	out->config = object_of(axis_db_result_get_text(result, 0, 6));
	out->facts = g_ptr_array_new_with_free_func(fact_free);

	id = g_strdup_printf("%" G_GINT64_FORMAT, out->id);
	params[0] = id;
	params[1] = target;
	if (!axis_db_conn_exec(conn,
	        "SELECT p.slug, f.report, f.dim, f.paths, f.bytes, f.low, f.mid, f.high, f.extra::text, f.revision_id,"
	        " r.number FROM estimate_fact f JOIN conversion_profile_revision r ON r.id = f.revision_id"
	        " JOIN conversion_profile p ON p.id = r.profile_id"
	        " WHERE f.snapshot_id = $1::bigint AND ($2::text IS NULL OR p.slug = $2::text)"
	        " ORDER BY p.slug, f.report, f.dim",
	        2, params, &facts, error)) {
		axis_estimate_snapshot_free(out);
		goto fail;
	}
	axis_db_conn_commit(conn, NULL);
	axis_db_pool_release(self->pool, conn);

	for (i = 0; i < axis_db_result_get_n_rows(facts); i++) {
		AxisEstimateFact *fact = g_new0(AxisEstimateFact, 1);

		fact->target = g_strdup(axis_db_result_get_text(facts, i, 0));
		fact->report = g_strdup(axis_db_result_get_text(facts, i, 1));
		fact->dim = g_strdup(axis_db_result_get_text(facts, i, 2));
		fact->paths = int_at(facts, i, 3);
		fact->bytes = int_at(facts, i, 4);
		fact->low = int_at(facts, i, 5);
		fact->mid = int_at(facts, i, 6);
		fact->high = int_at(facts, i, 7);
		fact->extra = object_of(axis_db_result_get_text(facts, i, 8));
		fact->revision_id = int_at(facts, i, 9);
		fact->revision = (gint)int_at(facts, i, 10);
		g_ptr_array_add(out->facts, fact);
	}
	*snapshot = out;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

const AxisEstimateFact *
axis_estimate_snapshot_fact(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *report,
	const gchar                *dim
){
	guint i;

	for (i = 0; snapshot != NULL && i < snapshot->facts->len; i++) {
		const AxisEstimateFact *fact = g_ptr_array_index(snapshot->facts, i);

		if (g_strcmp0(fact->target, target) == 0 && g_strcmp0(fact->report, report) == 0 &&
		    g_strcmp0(fact->dim, dim != NULL ? dim : "") == 0)
			return fact;
	}
	return NULL;
}

GPtrArray *
axis_estimate_snapshot_facts(
	const AxisEstimateSnapshot *snapshot,
	const gchar                *target,
	const gchar                *report
){
	GPtrArray *out = g_ptr_array_new();
	guint i;

	for (i = 0; snapshot != NULL && i < snapshot->facts->len; i++) {
		AxisEstimateFact *fact = g_ptr_array_index(snapshot->facts, i);

		if (g_strcmp0(fact->target, target) == 0 && g_strcmp0(fact->report, report) == 0)
			g_ptr_array_add(out, fact);
	}
	return out;
}
