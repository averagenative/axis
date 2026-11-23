/*
 * axis-probe-store.c - what the probe writes to and reads from the database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-private.h"

#include <string.h>

/* ── Building an INSERT ─────────────────────────────────────────── */

typedef struct
{
	GString   *names;
	GString   *values;
	GPtrArray *params;	/* of gchar *, a NULL being SQL NULL */
} Row;

static void
row_init(Row *row)
{
	row->names = g_string_new(NULL);
	row->values = g_string_new(NULL);
	row->params = g_ptr_array_new_with_free_func(g_free);
}

static void
row_clear(Row *row)
{
	g_string_free(row->names, TRUE);
	g_string_free(row->values, TRUE);
	g_ptr_array_unref(row->params);
}

/* Takes @param. */
static void
row_add(
	Row         *row,
	const gchar *column,
	const gchar *cast,
	gchar       *param
){
	if (row->params->len > 0) {
		g_string_append(row->names, ", ");
		g_string_append(row->values, ", ");
	}
	g_string_append(row->names, column);
	g_string_append_printf(row->values, "$%u::%s", row->params->len + 1, cast);
	g_ptr_array_add(row->params, param);
}

static void
row_text(
	Row         *row,
	const gchar *column,
	const gchar *value
){
	row_add(row, column, "text", g_strdup(value));
}

/* A number; @unknown (also: any negative) is NULL. */
static void
row_int(
	Row         *row,
	const gchar *column,
	gint64       value
){
	row_add(row, column, "bigint", value < 0 ? NULL : g_strdup_printf("%" G_GINT64_FORMAT, value));
}

/* A number that may be negative. */
static void
row_signed(
	Row         *row,
	const gchar *column,
	gint64       value
){
	row_add(row, column, "bigint", g_strdup_printf("%" G_GINT64_FORMAT, value));
}

static void
row_double(
	Row         *row,
	const gchar *column,
	gdouble      value
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	if (value < 0.0) {
		row_add(row, column, "double precision", NULL);
		return;
	}
	g_ascii_formatd(buffer, sizeof(buffer), "%.17g", value);
	row_add(row, column, "double precision", g_strdup(buffer));
}

static void
row_bool(
	Row         *row,
	const gchar *column,
	gboolean     value
){
	row_add(row, column, "boolean", g_strdup(value ? "true" : "false"));
}

/* Three-valued: unknown is NULL. */
static void
row_tri(
	Row         *row,
	const gchar *column,
	gint         value
){
	row_add(row, column, "boolean", value < 0 ? NULL : g_strdup(value ? "true" : "false"));
}

static gboolean
row_insert(
	Row         *row,
	AxisDbConn  *conn,
	const gchar *table,
	GError     **error
){
	g_autofree gchar *sql = g_strdup_printf("INSERT INTO %s (%s) VALUES (%s)", table, row->names->str,
		row->values->str);

	return axis_db_conn_exec(conn, sql, row->params->len, (const gchar *const *)row->params->pdata, NULL, error);
}

/* ── Writing ────────────────────────────────────────────────────── */

static void
info_row(
	Row                   *row,
	const AxisProbeTarget *target,
	const AxisMediaInfo   *info
){
	row_signed(row, "file_id", target->file_id);
	row_text(row, "status", info->status);
	row_text(row, "error", info->error);
	row_signed(row, "probe_version", info->probe_version);
	row_int(row, "probe_ms", info->probe_ms);
	row_text(row, "tool_version", info->tool_version);

	row_signed(row, "file_size", target->size);
	row_signed(row, "mtime_sec", target->mtime_sec);
	row_signed(row, "mtime_nsec", target->mtime_nsec);
	row_signed(row, "ctime_sec", target->ctime_sec);
	row_signed(row, "ctime_nsec", target->ctime_nsec);
	row_signed(row, "inode", target->inode);

	if (g_strcmp0(info->status, "ok") != 0)
		return;

	row_text(row, "container", info->container);
	row_int(row, "duration_ms", info->duration_ms);
	row_int(row, "bitrate", info->bitrate);
	row_bool(row, "bitrate_estimated", info->bitrate_estimated);
	row_int(row, "chapter_count", info->chapter_count);
	row_int(row, "stream_count", info->stream_count);
	row_int(row, "streams_omitted", info->streams_omitted);
	row_int(row, "video_streams", info->video_streams);
	row_int(row, "audio_streams", info->audio_streams);
	row_int(row, "subtitle_streams", info->subtitle_streams);

	row_text(row, "video_codec", info->video_codec);
	row_int(row, "width", info->width);
	row_int(row, "height", info->height);
	row_text(row, "resolution_class", info->resolution_class);
	row_int(row, "bit_depth", info->bit_depth);
	row_double(row, "frame_rate", info->frame_rate);
	row_text(row, "hdr_format", axis_hdr_format_to_string(info->hdr_format));
	row_bool(row, "hdr_uncertain", info->hdr_uncertain);
	row_text(row, "hdr_note", info->hdr_note);
	row_double(row, "bits_per_pixel", info->bits_per_pixel);
	row_bool(row, "bits_per_pixel_estimated", info->bits_per_pixel_estimated);
	row_tri(row, "interlaced", info->interlaced);
	row_tri(row, "vfr_suspected", info->vfr_suspected);

	row_text(row, "audio_codec", info->audio_codec);
	row_int(row, "audio_channels", info->audio_channels);
	row_int(row, "audio_sample_rate", info->audio_sample_rate);
	row_int(row, "audio_bit_depth", info->audio_bit_depth);
	row_tri(row, "audio_lossless", info->audio_lossless);
	row_int(row, "audio_bitrate_total", info->audio_bitrate_total);
	row_bool(row, "audio_bitrate_total_estimated", info->audio_bitrate_total_estimated);

	row_text(row, "tag_title", info->tag_title);
	row_text(row, "tag_artist", info->tag_artist);
	row_text(row, "tag_album", info->tag_album);
	row_bool(row, "has_track", info->has_track);
	row_bool(row, "has_disc", info->has_disc);
	row_bool(row, "has_musicbrainz", info->has_musicbrainz);
	row_bool(row, "has_replaygain", info->has_replaygain);
	row_bool(row, "has_embedded_art", info->has_embedded_art);
	row_int(row, "art_width", info->art_width);
	row_int(row, "art_height", info->art_height);
	row_text(row, "album_key", info->album_key);
}

static void
stream_row(
	Row                  *row,
	gint64                file_id,
	const AxisMediaStream *s
){
	row_signed(row, "file_id", file_id);
	row_signed(row, "idx", s->index);
	row_text(row, "kind", axis_stream_kind_to_string(s->kind));
	row_text(row, "codec", s->codec);
	row_text(row, "profile", s->profile);
	row_int(row, "duration_ms", s->duration_ms);
	row_int(row, "bitrate", s->bitrate);
	row_bool(row, "bitrate_estimated", s->bitrate > 0 && s->bitrate_estimated);
	row_text(row, "language", s->language);
	row_text(row, "title", s->title);
	row_bool(row, "is_default", s->is_default);
	row_bool(row, "is_forced", s->is_forced);
	row_bool(row, "attached_pic", s->attached_pic);

	row_int(row, "width", s->width);
	row_int(row, "height", s->height);
	row_text(row, "pixel_format", s->pixel_format);
	row_int(row, "bit_depth", s->bit_depth);
	row_double(row, "frame_rate", s->frame_rate);
	row_double(row, "real_frame_rate", s->real_frame_rate);
	row_text(row, "field_order", s->field_order);
	row_text(row, "color_range", s->color_range);
	row_text(row, "color_space", s->color_space);
	row_text(row, "color_transfer", s->color_transfer);
	row_text(row, "color_primaries", s->color_primaries);
	row_text(row, "hdr_format", axis_hdr_format_to_string(s->hdr_format));
	row_bool(row, "hdr_uncertain", s->hdr_uncertain);
	row_text(row, "hdr_note", s->hdr_note);
	row_bool(row, "has_mastering_display", s->signals.has_mastering);
	row_bool(row, "has_content_light", s->signals.has_content_light);
	/* The values only when complete and valid (axis-probe-hdr.c), else
	 * NULL: the table's checks restate the ranges. */
	if (s->hdr10.mastering && axis_hdr10_metadata_valid(&s->hdr10)) {
		row_int(row, "md_red_x", s->hdr10.red_x);
		row_int(row, "md_red_y", s->hdr10.red_y);
		row_int(row, "md_green_x", s->hdr10.green_x);
		row_int(row, "md_green_y", s->hdr10.green_y);
		row_int(row, "md_blue_x", s->hdr10.blue_x);
		row_int(row, "md_blue_y", s->hdr10.blue_y);
		row_int(row, "md_white_x", s->hdr10.white_x);
		row_int(row, "md_white_y", s->hdr10.white_y);
		row_int(row, "md_min_luminance", s->hdr10.min_luminance);
		row_int(row, "md_max_luminance", s->hdr10.max_luminance);
	}
	if (s->hdr10.light && axis_hdr10_metadata_valid(&s->hdr10)) {
		row_int(row, "cll_max_content", s->hdr10.max_content);
		row_int(row, "cll_max_average", s->hdr10.max_average);
	}
	row_int(row, "dv_profile", s->dv_profile);
	row_int(row, "dv_level", s->dv_level);
	row_int(row, "dv_bl_compat_id", s->dv_bl_compat_id);
	row_tri(row, "dv_hdr10_compatible", s->dv_hdr10_compatible);

	row_int(row, "channels", s->channels);
	row_text(row, "channel_layout", s->channel_layout);
	row_int(row, "sample_rate", s->sample_rate);
	row_int(row, "bit_depth_audio", s->bit_depth_audio);
	row_tri(row, "lossless", s->lossless);

	row_text(row, "subtitle_kind", s->subtitle_kind == AXIS_SUBTITLE_TEXT ? "text" :
		s->subtitle_kind == AXIS_SUBTITLE_IMAGE ? "image" : NULL);
}

gboolean
axis_probe_store_result(
	AxisDbConn            *conn,
	const AxisProbeTarget *target,
	const AxisMediaInfo   *info,
	gboolean              *stored,
	GError               **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, target->file_id);
	g_autofree gchar *generation = g_strdup_printf("%" G_GINT64_FORMAT, target->changed_generation);
	g_autofree gchar *size = g_strdup_printf("%" G_GINT64_FORMAT, target->size);
	g_autofree gchar *mtime_sec = g_strdup_printf("%" G_GINT64_FORMAT, target->mtime_sec);
	g_autofree gchar *mtime_nsec = g_strdup_printf("%d", target->mtime_nsec);
	g_autofree gchar *ctime_sec = g_strdup_printf("%" G_GINT64_FORMAT, target->ctime_sec);
	g_autofree gchar *ctime_nsec = g_strdup_printf("%d", target->ctime_nsec);
	g_autofree gchar *inode = g_strdup_printf("%" G_GINT64_FORMAT, target->inode);
	const gchar *params[8];
	Row row;
	guint i;

	*stored = FALSE;
	if (!axis_db_conn_begin(conn, error))
		return FALSE;

	/* The flag is cleared only for the file this result describes. */
	params[0] = id;
	params[1] = generation;
	params[2] = size;
	params[3] = mtime_sec;
	params[4] = mtime_nsec;
	params[5] = ctime_sec;
	params[6] = ctime_nsec;
	params[7] = inode;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_file SET changed = false"
	                       " WHERE id = $1::bigint AND state = 'present' AND changed_generation = $2::bigint"
	                       " AND size = $3::bigint AND mtime_sec = $4::bigint AND mtime_nsec = $5::integer"
	                       " AND ctime_sec = $6::bigint AND ctime_nsec = $7::integer AND inode = $8::bigint",
	                       8, params, &result, error))
		goto fail;
	if (axis_db_result_get_affected_rows(result) != 1) {
		axis_db_conn_rollback(conn, NULL);
		return TRUE;
	}

	params[0] = id;
	if (!axis_db_conn_exec(conn, "DELETE FROM media_info WHERE file_id = $1::bigint", 1, params, NULL, error))
		goto fail;

	row_init(&row);
	info_row(&row, target, info);
	if (!row_insert(&row, conn, "media_info", error)) {
		row_clear(&row);
		goto fail;
	}
	row_clear(&row);

	if (g_strcmp0(info->status, "ok") == 0) {
		for (i = 0; i < info->streams->len; i++) {
			row_init(&row);
			stream_row(&row, target->file_id, g_ptr_array_index(info->streams, i));
			if (!row_insert(&row, conn, "media_stream", error)) {
				row_clear(&row);
				goto fail;
			}
			row_clear(&row);
		}
	}

	params[0] = id;
	if (!axis_db_conn_exec(conn, "DELETE FROM media_probe_defer WHERE file_id = $1::bigint", 1, params, NULL,
	                       error))
		goto fail;
	if (!axis_db_conn_commit(conn, error))
		goto fail;

	*stored = TRUE;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

gboolean
axis_probe_store_defer(
	AxisDbConn  *conn,
	gint64       file_id,
	const gchar *reason,
	GError     **error
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	const gchar *params[2];

	params[0] = id;
	params[1] = reason;
	/* Five minutes, then twice as long each time, up to six hours. */
	return axis_db_conn_exec(conn,
	                         "INSERT INTO media_probe_defer (file_id, until, attempts, reason)"
	                         " VALUES ($1::bigint, now() + interval '5 minutes', 1, $2::text)"
	                         " ON CONFLICT (file_id) DO UPDATE SET"
	                         "   attempts = LEAST(media_probe_defer.attempts + 1, 30),"
	                         "   until = now() + make_interval(secs =>"
	                         "     LEAST(300.0 * power(2, LEAST(media_probe_defer.attempts, 6)), 21600)),"
	                         "   reason = EXCLUDED.reason",
	                         2, params, NULL, error);
}

/* ── Reading ────────────────────────────────────────────────────── */

gboolean
axis_probe_store_load_target(
	AxisDbConn       *conn,
	gint64            file_id,
	AxisProbeTarget  *target,
	gboolean         *found,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	const gchar *params[1];
	gint64 value;

	*found = FALSE;
	memset(target, 0, sizeof(*target));
	params[0] = id;
	if (!axis_db_conn_exec(conn,
	                       "SELECT f.id, f.root_id, f.volume_id, f.rel_path, r.rel_path, f.kind, f.state,"
	                       " f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, f.changed,"
	                       " f.changed_generation, coalesce(s.complete_generation, 0),"
	                       " m.file_id IS NOT NULL, coalesce(m.probe_version, 0)"
	                       " FROM library_file f"
	                       " JOIN library_root r ON r.id = f.root_id"
	                       " LEFT JOIN library_root_state s ON s.root_id = f.root_id"
	                       " LEFT JOIN media_info m ON m.file_id = f.id"
	                       " WHERE f.id = $1::bigint",
	                       1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1)
		return TRUE;

	axis_db_result_get_int64(result, 0, 0, &target->file_id);
	axis_db_result_get_int64(result, 0, 1, &target->root_id);
	axis_db_result_get_int64(result, 0, 2, &target->volume_id);
	target->rel_path = g_strdup(axis_db_result_get_text(result, 0, 3));
	target->root_rel_path = g_strdup(axis_db_result_get_text(result, 0, 4));
	target->kind = g_strdup(axis_db_result_get_text(result, 0, 5));
	target->state = g_strdup(axis_db_result_get_text(result, 0, 6));
	axis_db_result_get_int64(result, 0, 7, &target->size);
	axis_db_result_get_int64(result, 0, 8, &target->mtime_sec);
	if (axis_db_result_get_int64(result, 0, 9, &value))
		target->mtime_nsec = (gint)value;
	axis_db_result_get_int64(result, 0, 10, &target->ctime_sec);
	if (axis_db_result_get_int64(result, 0, 11, &value))
		target->ctime_nsec = (gint)value;
	axis_db_result_get_int64(result, 0, 12, &target->inode);
	axis_db_result_get_boolean(result, 0, 13, &target->changed);
	axis_db_result_get_int64(result, 0, 14, &target->changed_generation);
	axis_db_result_get_int64(result, 0, 15, &target->complete_generation);
	axis_db_result_get_boolean(result, 0, 16, &target->has_info);
	if (axis_db_result_get_int64(result, 0, 17, &value))
		target->info_version = (gint)value;
	*found = TRUE;
	return TRUE;
}

/* Column readers by name. */

static gint64
col_int(
	AxisDbResult *result,
	guint         row,
	const gchar  *name,
	gint64        otherwise
){
	gint column = axis_db_result_get_column_index(result, name);
	gint64 value = otherwise;

	if (column >= 0 && !axis_db_result_is_null(result, row, (guint)column))
		axis_db_result_get_int64(result, row, (guint)column, &value);
	return value;
}

static gchar *
col_text(
	AxisDbResult *result,
	guint         row,
	const gchar  *name
){
	gint column = axis_db_result_get_column_index(result, name);

	if (column < 0 || axis_db_result_is_null(result, row, (guint)column))
		return NULL;
	return g_strdup(axis_db_result_get_text(result, row, (guint)column));
}

static gboolean
col_bool(
	AxisDbResult *result,
	guint         row,
	const gchar  *name
){
	gint column = axis_db_result_get_column_index(result, name);
	gboolean value = FALSE;

	if (column >= 0 && !axis_db_result_is_null(result, row, (guint)column))
		axis_db_result_get_boolean(result, row, (guint)column, &value);
	return value;
}

static gint
col_tri(
	AxisDbResult *result,
	guint         row,
	const gchar  *name
){
	gint column = axis_db_result_get_column_index(result, name);
	gboolean value = FALSE;

	if (column < 0 || axis_db_result_is_null(result, row, (guint)column))
		return AXIS_TRI_UNKNOWN;
	axis_db_result_get_boolean(result, row, (guint)column, &value);
	return value ? AXIS_TRI_YES : AXIS_TRI_NO;
}

static gdouble
col_double(
	AxisDbResult *result,
	guint         row,
	const gchar  *name
){
	gint column = axis_db_result_get_column_index(result, name);

	if (column < 0 || axis_db_result_is_null(result, row, (guint)column))
		return -1.0;
	return g_ascii_strtod(axis_db_result_get_text(result, row, (guint)column), NULL);
}

static AxisHdrFormat
col_hdr(
	AxisDbResult *result,
	guint         row,
	const gchar  *name
){
	g_autofree gchar *text = col_text(result, row, name);
	AxisHdrFormat format = AXIS_HDR_NONE;

	if (text != NULL)
		axis_hdr_format_from_string(text, &format);
	return format;
}

static void
stream_from_row(
	AxisDbResult    *result,
	guint            row,
	AxisMediaStream *s
){
	g_autofree gchar *kind = col_text(result, row, "kind");
	g_autofree gchar *sub = col_text(result, row, "subtitle_kind");

	s->index = (gint)col_int(result, row, "idx", 0);
	s->kind = AXIS_STREAM_UNKNOWN;
	if (kind != NULL)
		axis_stream_kind_from_string(kind, &s->kind);
	s->codec = col_text(result, row, "codec");
	s->profile = col_text(result, row, "profile");
	s->duration_ms = col_int(result, row, "duration_ms", -1);
	s->bitrate = col_int(result, row, "bitrate", -1);
	s->bitrate_estimated = col_bool(result, row, "bitrate_estimated");
	s->language = col_text(result, row, "language");
	s->title = col_text(result, row, "title");
	s->is_default = col_bool(result, row, "is_default");
	s->is_forced = col_bool(result, row, "is_forced");
	s->attached_pic = col_bool(result, row, "attached_pic");
	s->width = (gint)col_int(result, row, "width", -1);
	s->height = (gint)col_int(result, row, "height", -1);
	s->pixel_format = col_text(result, row, "pixel_format");
	s->bit_depth = (gint)col_int(result, row, "bit_depth", -1);
	s->frame_rate = col_double(result, row, "frame_rate");
	s->real_frame_rate = col_double(result, row, "real_frame_rate");
	s->field_order = col_text(result, row, "field_order");
	s->color_range = col_text(result, row, "color_range");
	s->color_space = col_text(result, row, "color_space");
	s->color_transfer = col_text(result, row, "color_transfer");
	s->color_primaries = col_text(result, row, "color_primaries");
	s->hdr_format = col_hdr(result, row, "hdr_format");
	s->hdr_uncertain = col_bool(result, row, "hdr_uncertain");
	s->signals.has_mastering = col_bool(result, row, "has_mastering_display");
	s->signals.has_content_light = col_bool(result, row, "has_content_light");
	memset(&s->hdr10, 0, sizeof(s->hdr10));
	if (col_int(result, row, "md_max_luminance", -1) >= 0) {
		s->hdr10.mastering = TRUE;
		s->hdr10.red_x = (gint)col_int(result, row, "md_red_x", -1);
		s->hdr10.red_y = (gint)col_int(result, row, "md_red_y", -1);
		s->hdr10.green_x = (gint)col_int(result, row, "md_green_x", -1);
		s->hdr10.green_y = (gint)col_int(result, row, "md_green_y", -1);
		s->hdr10.blue_x = (gint)col_int(result, row, "md_blue_x", -1);
		s->hdr10.blue_y = (gint)col_int(result, row, "md_blue_y", -1);
		s->hdr10.white_x = (gint)col_int(result, row, "md_white_x", -1);
		s->hdr10.white_y = (gint)col_int(result, row, "md_white_y", -1);
		s->hdr10.min_luminance = col_int(result, row, "md_min_luminance", -1);
		s->hdr10.max_luminance = col_int(result, row, "md_max_luminance", -1);
	}
	if (col_int(result, row, "cll_max_content", -1) >= 0) {
		s->hdr10.light = TRUE;
		s->hdr10.max_content = (gint)col_int(result, row, "cll_max_content", -1);
		s->hdr10.max_average = (gint)col_int(result, row, "cll_max_average", -1);
	}
	/* What the database holds is checked by its constraints; a row that
	 * somehow is not valid is not used. */
	if (!axis_hdr10_metadata_valid(&s->hdr10))
		memset(&s->hdr10, 0, sizeof(s->hdr10));
	s->dv_profile = (gint)col_int(result, row, "dv_profile", -1);
	s->dv_level = (gint)col_int(result, row, "dv_level", -1);
	s->dv_bl_compat_id = (gint)col_int(result, row, "dv_bl_compat_id", -1);
	s->dv_hdr10_compatible = col_tri(result, row, "dv_hdr10_compatible");
	s->channels = (gint)col_int(result, row, "channels", -1);
	s->channel_layout = col_text(result, row, "channel_layout");
	s->sample_rate = (gint)col_int(result, row, "sample_rate", -1);
	s->bit_depth_audio = (gint)col_int(result, row, "bit_depth_audio", -1);
	s->lossless = col_tri(result, row, "lossless");
	s->subtitle_kind = g_strcmp0(sub, "text") == 0 ? AXIS_SUBTITLE_TEXT :
		g_strcmp0(sub, "image") == 0 ? AXIS_SUBTITLE_IMAGE : AXIS_SUBTITLE_UNKNOWN;
}

gboolean
axis_probe_store_load_info(
	AxisDbConn      *conn,
	gint64           file_id,
	AxisMediaInfo  **out,
	GError         **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) streams = NULL;
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	const gchar *params[1];
	guint i;

	*out = NULL;
	params[0] = id;
	if (!axis_db_conn_exec(conn,
	                       "SELECT m.*, (extract(epoch FROM m.probed_at) * 1000000)::bigint AS probed_at_us"
	                       " FROM media_info m WHERE m.file_id = $1::bigint",
	                       1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1)
		return TRUE;

	info = axis_media_info_new();
	info->file_id = file_id;
	info->status = col_text(result, 0, "status");
	info->error = col_text(result, 0, "error");
	info->probe_version = (gint)col_int(result, 0, "probe_version", 0);
	info->probed_at_us = col_int(result, 0, "probed_at_us", 0);
	info->probe_ms = (gint)col_int(result, 0, "probe_ms", -1);
	info->tool_version = col_text(result, 0, "tool_version");
	info->file_size = col_int(result, 0, "file_size", 0);
	info->mtime_sec = col_int(result, 0, "mtime_sec", 0);
	info->mtime_nsec = (gint)col_int(result, 0, "mtime_nsec", 0);
	info->ctime_sec = col_int(result, 0, "ctime_sec", 0);
	info->ctime_nsec = (gint)col_int(result, 0, "ctime_nsec", 0);
	info->inode = col_int(result, 0, "inode", 0);
	info->container = col_text(result, 0, "container");
	info->duration_ms = col_int(result, 0, "duration_ms", -1);
	info->bitrate = col_int(result, 0, "bitrate", -1);
	info->bitrate_estimated = col_bool(result, 0, "bitrate_estimated");
	info->chapter_count = (gint)col_int(result, 0, "chapter_count", -1);
	info->stream_count = (gint)col_int(result, 0, "stream_count", -1);
	info->streams_omitted = (gint)col_int(result, 0, "streams_omitted", 0);
	info->video_streams = (gint)col_int(result, 0, "video_streams", 0);
	info->audio_streams = (gint)col_int(result, 0, "audio_streams", 0);
	info->subtitle_streams = (gint)col_int(result, 0, "subtitle_streams", 0);
	info->video_codec = col_text(result, 0, "video_codec");
	info->width = (gint)col_int(result, 0, "width", -1);
	info->height = (gint)col_int(result, 0, "height", -1);
	{
		g_autofree gchar *class_name = col_text(result, 0, "resolution_class");

		/* The constant strings the model uses. */
		info->resolution_class = g_strcmp0(class_name, "sd") == 0 ? "sd" :
			g_strcmp0(class_name, "720p") == 0 ? "720p" :
			g_strcmp0(class_name, "1080p") == 0 ? "1080p" :
			g_strcmp0(class_name, "2160p") == 0 ? "2160p" :
			g_strcmp0(class_name, "other") == 0 ? "other" : NULL;
	}
	info->bit_depth = (gint)col_int(result, 0, "bit_depth", -1);
	info->frame_rate = col_double(result, 0, "frame_rate");
	info->hdr_format = col_hdr(result, 0, "hdr_format");
	info->hdr_uncertain = col_bool(result, 0, "hdr_uncertain");
	/* The note is one of the fixed sentences; a page shows what is
	 * stored, copied into the model's own storage. */
	info->hdr_note_owned = col_text(result, 0, "hdr_note");
	info->hdr_note = info->hdr_note_owned;
	info->bits_per_pixel = col_double(result, 0, "bits_per_pixel");
	info->bits_per_pixel_estimated = col_bool(result, 0, "bits_per_pixel_estimated");
	info->interlaced = col_tri(result, 0, "interlaced");
	info->vfr_suspected = col_tri(result, 0, "vfr_suspected");
	info->audio_codec = col_text(result, 0, "audio_codec");
	info->audio_channels = (gint)col_int(result, 0, "audio_channels", -1);
	info->audio_sample_rate = (gint)col_int(result, 0, "audio_sample_rate", -1);
	info->audio_bit_depth = (gint)col_int(result, 0, "audio_bit_depth", -1);
	info->audio_lossless = col_tri(result, 0, "audio_lossless");
	info->audio_bitrate_total = col_int(result, 0, "audio_bitrate_total", -1);
	info->audio_bitrate_total_estimated = col_bool(result, 0, "audio_bitrate_total_estimated");
	info->tag_title = col_text(result, 0, "tag_title");
	info->tag_artist = col_text(result, 0, "tag_artist");
	info->tag_album = col_text(result, 0, "tag_album");
	info->has_track = col_bool(result, 0, "has_track");
	info->has_disc = col_bool(result, 0, "has_disc");
	info->has_musicbrainz = col_bool(result, 0, "has_musicbrainz");
	info->has_replaygain = col_bool(result, 0, "has_replaygain");
	info->has_embedded_art = col_bool(result, 0, "has_embedded_art");
	info->art_width = (gint)col_int(result, 0, "art_width", -1);
	info->art_height = (gint)col_int(result, 0, "art_height", -1);
	info->album_key = col_text(result, 0, "album_key");

	if (!axis_db_conn_exec(conn, "SELECT * FROM media_stream WHERE file_id = $1::bigint ORDER BY idx", 1,
	                       params, &streams, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(streams); i++) {
		AxisMediaStream *s = g_new0(AxisMediaStream, 1);

		stream_from_row(streams, i, s);
		g_ptr_array_add(info->streams, s);
	}

	*out = g_steal_pointer(&info);
	return TRUE;
}
