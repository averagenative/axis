/*
 * axis-estimator-load.c - the one loader of the estimator's facts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every caller (the refresh, the file lists, the file page, a conversion
 * later) reads a file's facts through the column list and row reader
 * here, so that they all decide from the same facts.  Unknown is NULL in
 * the database and "unknown" in the model (axis-probe-model.h), never a
 * default that looks like a fact.
 */

#include "transcode/axis-estimator-load.h"

#include <string.h>

/* ── Freeing ────────────────────────────────────────────────────── */

static void
loaded_free(gpointer data)
{
	AxisEstimateLoaded *loaded = data;

	axis_media_info_free(loaded->info);
	g_free(loaded);
}

AxisEstimateRows *
axis_estimate_rows_new(void)
{
	AxisEstimateRows *rows = g_new0(AxisEstimateRows, 1);

	rows->files = g_ptr_array_new();
	rows->owned = g_ptr_array_new_with_free_func(loaded_free);
	rows->strings = g_string_chunk_new(4096);
	return rows;
}

void
axis_estimate_rows_free(AxisEstimateRows *rows)
{
	if (rows == NULL)
		return;
	g_ptr_array_unref(rows->files);
	g_ptr_array_unref(rows->owned);
	g_string_chunk_free(rows->strings);
	g_free(rows);
}

/* ── Reading columns ────────────────────────────────────────────── */

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

static gint
tri_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	if (axis_db_result_is_null(result, row, column) || !axis_db_result_get_boolean(result, row, column, &value))
		return AXIS_TRI_UNKNOWN;
	return value ? AXIS_TRI_YES : AXIS_TRI_NO;
}

static gdouble
double_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);
	gchar *end = NULL;
	gdouble value;

	if (text == NULL)
		return -1.0;
	value = g_ascii_strtod(text, &end);
	if (end == text)
		return -1.0;
	return value;
}

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(result, row, column));
}

static AxisHdrFormat
hdr_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	AxisHdrFormat format = AXIS_HDR_NONE;
	const gchar *text = axis_db_result_get_text(result, row, column);

	if (text != NULL && !axis_hdr_format_from_string(text, &format))
		format = AXIS_HDR_NONE;
	return format;
}

/* The constant strings the model uses. */
static const gchar *
resolution_of(const gchar *text)
{
	static const gchar *const classes[] = { "sd", "720p", "1080p", "2160p", "other", NULL };
	guint i;

	for (i = 0; text != NULL && classes[i] != NULL; i++) {
		if (strcmp(text, classes[i]) == 0)
			return classes[i];
	}
	return NULL;
}

static const gchar *
chunk(
	AxisEstimateRows *rows,
	const gchar      *text
){
	return text != NULL ? g_string_chunk_insert_const(rows->strings, text) : NULL;
}

AxisEstimateLoaded *
axis_estimator_read_row(
	AxisEstimateRows *rows,
	AxisDbResult     *result,
	guint             row
){
	AxisEstimateLoaded *loaded = g_new0(AxisEstimateLoaded, 1);
	AxisEstimateIndex *idx = &loaded->file.index;
	AxisMediaType type = AXIS_MEDIA_OTHER;

	idx->file_id = int_at(result, row, 0, 0);
	idx->root_id = int_at(result, row, 1, 0);
	idx->volume_id = int_at(result, row, 2, 0);
	idx->kind = chunk(rows, axis_db_result_get_text(result, row, 3));
	if (!axis_media_type_from_string(axis_db_result_get_text(result, row, 4), &type))
		/* Out of every target's libraries. */
		type = (AxisMediaType)AXIS_MEDIA_TYPE_COUNT;
	idx->root_type = type;
	idx->state = chunk(rows, axis_db_result_get_text(result, row, 5));
	idx->size = int_at(result, row, 6, 0);
	idx->nlink = int_at(result, row, 7, 0);
	idx->pinned = bool_at(result, row, 8);
	idx->changed = bool_at(result, row, 9);
	idx->mtime_sec = int_at(result, row, 10, 0);
	idx->mtime_nsec = (gint)int_at(result, row, 11, 0);
	idx->ctime_sec = int_at(result, row, 12, 0);
	idx->ctime_nsec = (gint)int_at(result, row, 13, 0);
	idx->inode = int_at(result, row, 14, 0);
	idx->volume_usable = bool_at(result, row, 15);
	idx->counted = bool_at(result, row, 16);
	idx->rel_path = chunk(rows, axis_db_result_get_text(result, row, 17));
	idx->unit = chunk(rows, axis_db_result_get_text(result, row, 18));
	idx->unit_files = (guint)CLAMP(int_at(result, row, 54, 0), 0, G_MAXINT32);

	if (bool_at(result, row, 19)) {
		AxisMediaInfo *info = axis_media_info_new();

		info->file_id = idx->file_id;
		info->status = text_at(result, row, 20);
		info->probe_version = (gint)int_at(result, row, 21, 0);
		info->file_size = int_at(result, row, 22, -1);
		info->mtime_sec = int_at(result, row, 23, -1);
		info->mtime_nsec = (gint)int_at(result, row, 24, -1);
		info->ctime_sec = int_at(result, row, 25, -1);
		info->ctime_nsec = (gint)int_at(result, row, 26, -1);
		info->inode = int_at(result, row, 27, -1);
		info->container = text_at(result, row, 28);
		info->duration_ms = int_at(result, row, 29, -1);
		info->bitrate = int_at(result, row, 30, -1);
		info->bitrate_estimated = bool_at(result, row, 31);
		info->streams_omitted = (gint)int_at(result, row, 32, 0);
		info->video_streams = (gint)int_at(result, row, 33, 0);
		info->audio_streams = (gint)int_at(result, row, 34, 0);
		info->subtitle_streams = (gint)int_at(result, row, 35, 0);
		info->video_codec = text_at(result, row, 36);
		info->width = (gint)int_at(result, row, 37, -1);
		info->height = (gint)int_at(result, row, 38, -1);
		info->resolution_class = resolution_of(axis_db_result_get_text(result, row, 39));
		info->bit_depth = (gint)int_at(result, row, 40, -1);
		info->frame_rate = double_at(result, row, 41);
		info->hdr_format = hdr_at(result, row, 42);
		info->hdr_uncertain = bool_at(result, row, 43);
		info->bits_per_pixel = double_at(result, row, 44);
		info->interlaced = tri_at(result, row, 45);
		info->audio_codec = text_at(result, row, 46);
		info->audio_channels = (gint)int_at(result, row, 47, -1);
		info->audio_sample_rate = (gint)int_at(result, row, 48, -1);
		info->audio_bit_depth = (gint)int_at(result, row, 49, -1);
		info->audio_lossless = tri_at(result, row, 50);
		info->has_embedded_art = bool_at(result, row, 51);
		info->art_width = (gint)int_at(result, row, 52, -1);
		info->art_height = (gint)int_at(result, row, 53, -1);
		loaded->info = info;
		loaded->file.info = info;
	}
	return loaded;
}

static AxisMediaStream *
read_stream(
	AxisDbResult *result,
	guint         row
){
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);
	const gchar *kind = axis_db_result_get_text(result, row, 2);
	const gchar *sub = axis_db_result_get_text(result, row, 24);

	s->index = (gint)int_at(result, row, 1, 0);
	s->kind = AXIS_STREAM_UNKNOWN;
	if (kind != NULL)
		axis_stream_kind_from_string(kind, &s->kind);
	s->codec = text_at(result, row, 3);
	s->profile = text_at(result, row, 4);
	s->duration_ms = int_at(result, row, 5, -1);
	s->bitrate = int_at(result, row, 6, -1);
	s->bitrate_estimated = bool_at(result, row, 7);
	s->language = text_at(result, row, 8);
	s->is_default = bool_at(result, row, 9);
	s->is_forced = bool_at(result, row, 10);
	s->attached_pic = bool_at(result, row, 11);
	s->width = (gint)int_at(result, row, 12, -1);
	s->height = (gint)int_at(result, row, 13, -1);
	s->bit_depth = (gint)int_at(result, row, 14, -1);
	s->frame_rate = double_at(result, row, 15);
	s->real_frame_rate = double_at(result, row, 16);
	s->field_order = text_at(result, row, 17);
	s->hdr_format = hdr_at(result, row, 18);
	s->hdr_uncertain = bool_at(result, row, 19);
	s->channels = (gint)int_at(result, row, 20, -1);
	s->sample_rate = (gint)int_at(result, row, 21, -1);
	s->bit_depth_audio = (gint)int_at(result, row, 22, -1);
	s->lossless = tri_at(result, row, 23);
	s->subtitle_kind = g_strcmp0(sub, "text") == 0 ? AXIS_SUBTITLE_TEXT :
		g_strcmp0(sub, "image") == 0 ? AXIS_SUBTITLE_IMAGE : AXIS_SUBTITLE_UNKNOWN;
	s->dv_profile = AXIS_PROBE_UNKNOWN;
	s->dv_level = AXIS_PROBE_UNKNOWN;
	s->dv_bl_compat_id = AXIS_PROBE_UNKNOWN;
	s->dv_hdr10_compatible = AXIS_TRI_UNKNOWN;
	s->tag_bitrate = AXIS_PROBE_UNKNOWN;
	return s;
}

static gchar *
id_array(
	const gint64 *ids,
	guint         n_ids
){
	GString *list = g_string_new("{");
	guint i;

	for (i = 0; i < n_ids; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", ids[i]);
	g_string_append_c(list, '}');
	return g_string_free(list, FALSE);
}

gboolean
axis_estimator_attach_streams(
	AxisDbConn       *conn,
	AxisEstimateRows *rows,
	guint             from,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *list = NULL;
	const gchar *params[1];
	guint i;

	for (i = from; i < rows->owned->len; i++) {
		AxisEstimateLoaded *loaded = g_ptr_array_index(rows->owned, i);

		if (loaded->info != NULL && g_strcmp0(loaded->info->status, "ok") == 0) {
			g_array_append_val(ids, loaded->file.index.file_id);
			g_hash_table_insert(by_id, &loaded->file.index.file_id, loaded);
		}
	}
	if (ids->len == 0)
		return TRUE;
	list = id_array((const gint64 *)ids->data, ids->len);
	params[0] = list;
	if (!axis_db_conn_exec(conn, AXIS_ESTIMATE_SQL_STREAMS, 1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 file_id = int_at(result, i, 0, 0);
		AxisEstimateLoaded *loaded = g_hash_table_lookup(by_id, &file_id);

		if (loaded != NULL)
			g_ptr_array_add(loaded->info->streams, read_stream(result, i));
	}
	return TRUE;
}

gboolean
axis_estimator_timed_exec(
	AxisDbConn          *conn,
	const gchar         *timeout,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	AxisDbResult       **result,
	GError             **error
){
	g_autofree gchar *set = g_strdup_printf("SET LOCAL statement_timeout = '%s'", timeout);

	/* Inside the caller's transaction (a worker's result, recorded while
	 * its job is locked), the statement runs in it, as it is. */
	if (axis_db_conn_in_transaction(conn))
		return axis_db_conn_exec(conn, sql, n_params, params, result, error);
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_db_conn_exec(conn, set, 0, NULL, NULL, error) ||
	    !axis_db_conn_exec(conn, AXIS_ESTIMATE_SQL_NO_JIT, 0, NULL, NULL, error) ||
	    !axis_db_conn_exec(conn, sql, n_params, params, result, error)) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	if (!axis_db_conn_commit(conn, error)) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	return TRUE;
}

/* Loads the rows of @sql (whose $1 is the id array) and their streams, in
 * one transaction (the caller's, when one is open on @conn: a worker's
 * result is checked while its job is locked), ordered as @ids (or as the
 * statement when @ids is NULL). */
static gboolean
load_by(
	AxisDbConn         *conn,
	const gchar        *sql,
	const gchar        *param,
	const gint64       *ids,
	guint               n_ids,
	AxisEstimateRows  **rows,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisEstimateRows) out = axis_estimate_rows_new();
	const gchar *params[1];
	gboolean own = !axis_db_conn_in_transaction(conn);
	guint i;

	*rows = NULL;
	params[0] = param;
	if (own) {
		if (!axis_db_conn_begin(conn, error))
			return FALSE;
		if (!axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '" AXIS_LIBRARY_LIST_TIMEOUT "'", 0, NULL, NULL,
		        error) ||
		    !axis_db_conn_exec(conn, AXIS_ESTIMATE_SQL_NO_JIT, 0, NULL, NULL, error))
			goto fail;
	}
	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(out->owned, axis_estimator_read_row(out, result, i));
	if (!axis_estimator_attach_streams(conn, out, 0, error))
		goto fail;
	if (own && !axis_db_conn_commit(conn, error))
		goto fail;

	if (ids != NULL) {
		g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);

		for (i = 0; i < out->owned->len; i++) {
			AxisEstimateLoaded *loaded = g_ptr_array_index(out->owned, i);

			g_hash_table_insert(by_id, &loaded->file.index.file_id, loaded);
		}
		for (i = 0; i < n_ids; i++) {
			AxisEstimateLoaded *loaded = g_hash_table_lookup(by_id, &ids[i]);

			if (loaded != NULL)
				g_ptr_array_add(out->files, &loaded->file);
		}
	} else {
		for (i = 0; i < out->owned->len; i++)
			g_ptr_array_add(out->files, &((AxisEstimateLoaded *)g_ptr_array_index(out->owned, i))->file);
	}
	*rows = g_steal_pointer(&out);
	return TRUE;

fail:
	if (own)
		axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

gboolean
axis_estimator_load_files(
	AxisDbConn         *conn,
	const gint64       *ids,
	guint               n_ids,
	AxisEstimateRows  **rows,
	GError            **error
){
	g_autofree gchar *list = id_array(ids, n_ids);

	g_return_val_if_fail(conn != NULL && rows != NULL, FALSE);

	return load_by(conn, AXIS_ESTIMATE_SQL_FILES, list, ids, n_ids, rows, error);
}

gboolean
axis_estimator_load_units(
	AxisDbConn         *conn,
	const gint64       *ids,
	guint               n_ids,
	AxisEstimateRows  **rows,
	GError            **error
){
	g_autofree gchar *list = id_array(ids, n_ids);

	g_return_val_if_fail(conn != NULL && rows != NULL, FALSE);

	return load_by(conn, AXIS_ESTIMATE_SQL_UNIT_MEMBERS, list, NULL, 0, rows, error);
}
