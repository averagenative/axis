/*
 * axis-compat-report.c - coverage, devices, what fails today, the path map
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-report.h"

#include <string.h>

#include "compat/axis-compat-notify.h"
#include "compat/axis-compat-plex.h"
#include "compat/axis-compat-private.h"
#include "db/axis-db-error.h"

#define EPOCH_US(x) "coalesce((extract(epoch FROM " x ") * 1000000)::bigint, 0)"

static const gchar *
text_or_null(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : axis_db_result_get_text(result, row, column);
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
seconds_text(
	AxisCompat  *self,
	const gchar *key,
	gint64       fallback
){
	return g_strdup_printf("%" G_GINT64_FORMAT, self->settings != NULL ?
		axis_settings_get_duration(self->settings, key) : fallback);
}

/* ── Coverage ───────────────────────────────────────────────────── */

void
axis_compat_server_status_free(AxisCompatServerStatus *status)
{
	if (status == NULL)
		return;
	g_free(status->kind);
	g_free(status->label);
	g_free(status->last_error);
	g_free(status->history_error);
	g_free(status->server_name);
	g_free(status->server_version);
	g_free(status);
}

void
axis_compat_coverage_clear(AxisCompatCoverage *coverage)
{
	if (coverage == NULL)
		return;
	g_free(coverage->last_error);
	g_free(coverage->history_error);
	g_free(coverage->server_name);
	g_free(coverage->server_version);
	if (coverage->servers != NULL)
		g_ptr_array_unref(coverage->servers);
	memset(coverage, 0, sizeof(*coverage));
}

const AxisCompatServerStatus *
axis_compat_coverage_server(
	const AxisCompatCoverage *coverage,
	const gchar              *kind
){
	guint i;

	for (i = 0; coverage != NULL && coverage->servers != NULL && i < coverage->servers->len; i++) {
		const AxisCompatServerStatus *status = g_ptr_array_index(coverage->servers, i);

		if (g_strcmp0(status->kind, kind) == 0)
			return status;
	}
	return NULL;
}

/* The row of @result whose first column is @kind, or -1. */
static gint
row_of_kind(
	AxisDbResult *result,
	const gchar  *kind
){
	guint rows = axis_db_result_get_n_rows(result);
	guint i;

	for (i = 0; i < rows; i++) {
		if (g_strcmp0(axis_db_result_get_text(result, i, 0), kind) == 0)
			return (gint)i;
	}
	return -1;
}

gboolean
axis_compat_load_coverage(
	AxisCompat          *self,
	AxisCompatCoverage  *coverage,
	GError             **error
){
	g_autoptr(AxisDbResult) state = NULL;
	g_autoptr(AxisDbResult) sessions = NULL;
	g_autoptr(AxisDbResult) kind_sessions = NULL;
	g_autoptr(AxisDbResult) plays = NULL;
	g_autoptr(AxisDbResult) kind_plays = NULL;
	g_autoptr(AxisDbResult) devices = NULL;
	g_autoptr(AxisDbResult) kind_devices = NULL;
	g_autofree gchar *lookback = seconds_text(self, AXIS_COMPAT_SETTING_LOOKBACK, 180 * 86400);
	const gchar *params[1];
	guint k;

	memset(coverage, 0, sizeof(*coverage));
	coverage->configured = axis_compat_is_configured(self);
	params[0] = lookback;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT server_kind, server_name, server_version, " EPOCH_US("started_at") ", " EPOCH_US("last_poll_at") ", "
	        EPOCH_US("last_poll_ok_at") ", " EPOCH_US("next_poll_at") ", " EPOCH_US("history_at") ","
	        " failures, playing_now, last_error, history_error FROM compat_state ORDER BY id",
	        0, NULL, &state, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT count(*), count(*) FILTER (WHERE NOT remote), count(*) FILTER (WHERE remote),"
	        " count(*) FILTER (WHERE file_id IS NOT NULL), " EPOCH_US("min(first_seen)") ", "
	        EPOCH_US("max(last_seen)") ", count(DISTINCT (server_kind, device_id)) FROM compat_session",
	        0, NULL, &sessions, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT server_kind, count(*), count(DISTINCT device_id) FROM compat_session GROUP BY server_kind",
	        0, NULL, &kind_sessions, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT count(*), " EPOCH_US("min(viewed_at)") ", " EPOCH_US("max(viewed_at)") " FROM compat_play",
	        0, NULL, &plays, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT server_kind, count(*) FROM compat_play GROUP BY server_kind", 0, NULL, &kind_plays, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT count(DISTINCT (server_kind, device_id)) FROM (SELECT server_kind, device_id FROM compat_play"
	        " WHERE viewed_at >= now() - $1::double precision * interval '1 second' UNION ALL"
	        " SELECT server_kind, device_id FROM compat_session"
	        " WHERE last_seen >= now() - $1::double precision * interval '1 second') x",
	        1, params, &devices, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT server_kind, count(*) FROM compat_device GROUP BY server_kind", 0, NULL, &kind_devices, error))
		return FALSE;

	coverage->servers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_server_status_free);
	for (k = 0; axis_compat_kind_at(k) != NULL; k++) {
		const AxisCompatKind *kind = axis_compat_kind_at(k);
		AxisCompatServerStatus *status = g_new0(AxisCompatServerStatus, 1);
		gint row;

		status->kind = g_strdup(kind->name);
		status->label = g_strdup(kind->label);
		status->has_history = kind->has_history;
		status->configured = axis_compat_is_configured_kind(self, kind->name);
		row = row_of_kind(state, kind->name);
		if (row >= 0) {
			status->server_name = g_strdup(text_or_null(state, (guint)row, 1));
			status->server_version = g_strdup(text_or_null(state, (guint)row, 2));
			status->started_us = int_at(state, (guint)row, 3);
			status->last_poll_us = int_at(state, (guint)row, 4);
			status->last_ok_us = int_at(state, (guint)row, 5);
			status->next_poll_us = int_at(state, (guint)row, 6);
			status->history_us = int_at(state, (guint)row, 7);
			status->failures = (gint)int_at(state, (guint)row, 8);
			status->playing_now = (gint)int_at(state, (guint)row, 9);
			status->last_error = g_strdup(text_or_null(state, (guint)row, 10));
			status->history_error = g_strdup(text_or_null(state, (guint)row, 11));
		}
		row = row_of_kind(kind_sessions, kind->name);
		if (row >= 0) {
			status->sessions = int_at(kind_sessions, (guint)row, 1);
			status->observed_devices = int_at(kind_sessions, (guint)row, 2);
		}
		row = row_of_kind(kind_plays, kind->name);
		if (row >= 0)
			status->plays = int_at(kind_plays, (guint)row, 1);
		row = row_of_kind(kind_devices, kind->name);
		if (row >= 0)
			status->devices = int_at(kind_devices, (guint)row, 1);
		if (status->started_us != 0 && (coverage->started_us == 0 || status->started_us < coverage->started_us))
			coverage->started_us = status->started_us;
		/* The figures about the connection are Plex's, as they always were. */
		if (g_strcmp0(kind->name, AXIS_COMPAT_KIND_PLEX) == 0) {
			coverage->server_name = g_strdup(status->server_name);
			coverage->server_version = g_strdup(status->server_version);
			coverage->last_poll_us = status->last_poll_us;
			coverage->last_ok_us = status->last_ok_us;
			coverage->next_poll_us = status->next_poll_us;
			coverage->history_us = status->history_us;
			coverage->failures = status->failures;
			coverage->playing_now = status->playing_now;
			coverage->last_error = g_strdup(status->last_error);
			coverage->history_error = g_strdup(status->history_error);
		}
		g_ptr_array_add(coverage->servers, status);
	}
	coverage->sessions = int_at(sessions, 0, 0);
	coverage->local_sessions = int_at(sessions, 0, 1);
	coverage->remote_sessions = int_at(sessions, 0, 2);
	coverage->matched_sessions = int_at(sessions, 0, 3);
	coverage->first_session_us = int_at(sessions, 0, 4);
	coverage->last_session_us = int_at(sessions, 0, 5);
	coverage->observed_devices = int_at(sessions, 0, 6);
	coverage->plays = int_at(plays, 0, 0);
	coverage->first_play_us = int_at(plays, 0, 1);
	coverage->last_play_us = int_at(plays, 0, 2);
	coverage->devices = int_at(devices, 0, 0);
	return TRUE;
}

/* ── Devices ────────────────────────────────────────────────────── */

void
axis_compat_obs_row_free(AxisCompatObsRow *row)
{
	if (row == NULL)
		return;
	g_free(row->codec);
	g_free(row->profile);
	g_free(row->res_class);
	g_free(row->container);
	g_free(row->out_codec);
	g_free(row->stated);
	g_free(row);
}

void
axis_compat_device_info_free(AxisCompatDeviceInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->server_kind);
	g_free(info->client_id);
	g_free(info->name);
	g_free(info->platform);
	g_free(info->product);
	if (info->rows != NULL)
		g_ptr_array_unref(info->rows);
	g_free(info);
}

/* Loads the capability rows of one device (or of all when @client_id is
 * NULL) into @by_device: axis_compat_device_key() -> GPtrArray of
 * AxisCompatObsRow. */
gboolean
axis_compat_load_observations(
	AxisDbConn   *conn,
	const gchar  *kind,
	const gchar  *client_id,
	GHashTable   *by_device,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[2];
	guint rows;
	guint i;

	params[0] = kind;
	params[1] = client_id;
	if (!axis_db_conn_exec(conn,
	        "SELECT s.device_id, e.kind, e.codec, e.profile, e.bit_depth, e.res_class, e.hdr, e.channels, e.container,"
	        " e.effect, e.out_codec, count(*), count(*) FILTER (WHERE s.remote), " EPOCH_US("max(s.last_seen)") ","
	        " s.server_kind, e.failed_property, string_agg(DISTINCT e.stated_reason, '; ')"
	        " FROM compat_evidence e JOIN compat_session s ON s.id = e.session_id"
	        " WHERE ($2::text IS NULL OR (s.server_kind = $1::text AND s.device_id = $2::text))"
	        " GROUP BY s.server_kind, s.device_id, e.kind, e.codec, e.profile, e.bit_depth, e.res_class, e.hdr,"
	        " e.channels, e.container, e.effect, e.out_codec, e.failed_property"
	        " ORDER BY s.server_kind, s.device_id, e.kind, e.codec, e.bit_depth DESC, e.effect, e.failed_property",
	        2, params, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	for (i = 0; i < rows; i++) {
		g_autofree gchar *key = axis_compat_device_key(axis_db_result_get_text(result, i, 14),
			axis_db_result_get_text(result, i, 0));
		AxisCompatObsRow *row = g_new0(AxisCompatObsRow, 1);
		GPtrArray *list = g_hash_table_lookup(by_device, key);
		gint64 value;

		if (list == NULL) {
			list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_obs_row_free);
			g_hash_table_insert(by_device, g_strdup(key), list);
		}
		row->codec = g_strdup(axis_db_result_get_text(result, i, 2));
		row->profile = g_strdup(text_or_null(result, i, 3));
		row->res_class = g_strdup(text_or_null(result, i, 5));
		row->container = g_strdup(text_or_null(result, i, 8));
		row->out_codec = g_strdup(text_or_null(result, i, 10));
		row->stated = g_strdup(text_or_null(result, i, 16));
		row->obs.combo.kind = (gint)int_at(result, i, 1);
		row->obs.combo.codec = row->codec;
		row->obs.combo.profile = row->profile;
		row->obs.combo.bit_depth = (gint)int_at(result, i, 4);
		row->obs.combo.res_class = row->res_class;
		value = int_at(result, i, 6);
		row->obs.combo.hdr = (gint)value;
		row->channels = (gint)int_at(result, i, 7);
		row->obs.combo.container = row->container;
		axis_compat_effect_from_string(axis_db_result_get_text(result, i, 9), &row->obs.effect);
		axis_compat_property_from_string(text_or_null(result, i, 15), &row->obs.property);
		row->obs.plays = (guint)int_at(result, i, 11);
		row->obs.remote_plays = (guint)int_at(result, i, 12);
		row->last_us = int_at(result, i, 13);
		g_ptr_array_add(list, row);
	}
	return TRUE;
}

static AxisCompatDeviceInfo *
device_from_row(
	AxisDbResult *result,
	guint         i
){
	AxisCompatDeviceInfo *info = g_new0(AxisCompatDeviceInfo, 1);
	gboolean pinned = FALSE;

	info->client_id = g_strdup(axis_db_result_get_text(result, i, 0));
	info->name = g_strdup(text_or_null(result, i, 1));
	info->platform = g_strdup(text_or_null(result, i, 2));
	info->product = g_strdup(text_or_null(result, i, 3));
	axis_db_result_get_boolean(result, i, 4, &pinned);
	info->pinned = pinned;
	info->first_seen_us = int_at(result, i, 5);
	info->last_seen_us = int_at(result, i, 6);
	info->sessions = (guint)int_at(result, i, 7);
	info->remote_sessions = (guint)int_at(result, i, 8);
	info->plays_in_window = (guint)int_at(result, i, 9);
	info->last_play_us = int_at(result, i, 10);
	info->server_kind = g_strdup(axis_db_result_get_text(result, i, 11));
	return info;
}

#define DEVICE_COLUMNS \
	"d.client_id, d.name, d.platform, d.product, d.pinned, " EPOCH_US("d.first_seen") ", " EPOCH_US("d.last_seen") "," \
	" (SELECT count(*) FROM compat_session s WHERE s.device_id = d.client_id AND s.server_kind = d.server_kind)," \
	" (SELECT count(*) FROM compat_session s WHERE s.device_id = d.client_id AND s.server_kind = d.server_kind" \
	"   AND s.remote)," \
	" (SELECT count(*) FROM compat_play p WHERE p.device_id = d.client_id AND p.server_kind = d.server_kind" \
	"   AND p.viewed_at >= now() - $1::double precision * interval '1 second')" \
	" + (SELECT count(*) FROM compat_session s WHERE s.device_id = d.client_id AND s.server_kind = d.server_kind" \
	"   AND s.last_seen >= now() - $1::double precision * interval '1 second')," \
	" " EPOCH_US("greatest((SELECT max(p.viewed_at) FROM compat_play p WHERE p.device_id = d.client_id" \
	"  AND p.server_kind = d.server_kind)," \
	"  (SELECT max(s.last_seen) FROM compat_session s WHERE s.device_id = d.client_id" \
	"  AND s.server_kind = d.server_kind))") ", d.server_kind"

gboolean
axis_compat_list_devices(
	AxisCompat  *self,
	GPtrArray  **devices,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *lookback = seconds_text(self, AXIS_COMPAT_SETTING_LOOKBACK, 180 * 86400);
	const gchar *params[1];
	guint rows;
	guint i;

	*devices = NULL;
	params[0] = lookback;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT " DEVICE_COLUMNS " FROM compat_device d ORDER BY 11 DESC, d.name, d.client_id, d.server_kind",
	        1, params, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	*devices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_device_info_free);
	for (i = 0; i < rows; i++)
		g_ptr_array_add(*devices, device_from_row(result, i));
	return TRUE;
}

gboolean
axis_compat_get_device(
	AxisCompat            *self,
	const gchar           *handle,
	AxisCompatDeviceInfo **info,
	GError               **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *lookback = seconds_text(self, AXIS_COMPAT_SETTING_LOOKBACK, 180 * 86400);
	g_autoptr(GHashTable) by_device = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	g_autofree gchar *kind = NULL;
	g_autofree gchar *client_id = NULL;
	g_autofree gchar *key = NULL;
	const gchar *params[3];
	AxisDbConn *conn;
	GPtrArray *rows;

	*info = NULL;
	if (!axis_compat_handle_split(handle, &kind, &client_id)) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND, "no such device");
		return FALSE;
	}
	params[0] = lookback;
	params[1] = client_id;
	params[2] = kind;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_exec(conn, "SELECT " DEVICE_COLUMNS " FROM compat_device d WHERE d.client_id = $2::text"
	        " AND d.server_kind = $3::text", 3, params, &result, error) ||
	    !axis_compat_load_observations(conn, kind, client_id, by_device, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND, "no such device");
		return FALSE;
	}
	*info = device_from_row(result, 0);
	key = axis_compat_device_key(kind, client_id);
	rows = g_hash_table_lookup(by_device, key);
	(*info)->rows = rows != NULL ? g_ptr_array_ref(rows) : g_ptr_array_new_with_free_func(
		(GDestroyNotify)axis_compat_obs_row_free);
	return TRUE;
}

gboolean
axis_compat_set_pinned(
	AxisCompat   *self,
	const gchar  *handle,
	gboolean      pinned,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *client_id = NULL;
	const gchar *params[3];

	if (!axis_compat_handle_split(handle, &kind, &client_id)) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND, "no such device");
		return FALSE;
	}
	params[0] = client_id;
	params[1] = pinned ? "true" : "false";
	params[2] = kind;
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE compat_device SET pinned = $2::boolean WHERE client_id = $1::text AND server_kind = $3::text",
	        3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND, "no such device");
		return FALSE;
	}
	return TRUE;
}

/* ── What fails today ───────────────────────────────────────────── */

void
axis_compat_failure_free(AxisCompatFailure *failure)
{
	if (failure == NULL)
		return;
	g_free(failure->server_kind);
	g_free(failure->client_id);
	g_free(failure->device_name);
	g_free(failure->codec);
	g_free(failure->out_codec);
	g_free(failure->stated);
	g_free(failure);
}

void
axis_compat_failed_file_free(AxisCompatFailedFile *file)
{
	if (file == NULL)
		return;
	g_free(file->rel_path);
	g_free(file);
}

gboolean
axis_compat_report_failures(
	AxisCompat  *self,
	GPtrArray  **failures,
	GPtrArray  **files,
	GError     **error
){
	g_autoptr(AxisDbResult) groups = NULL;
	g_autoptr(AxisDbResult) top = NULL;
	guint rows;
	guint i;

	*failures = NULL;
	*files = NULL;
	if (!axis_db_pool_exec(self->pool,
	        "WITH c AS (SELECT s.server_kind, s.device_id, e.kind, e.codec, coalesce(e.out_codec, '') AS out_codec,"
	        "  e.effect, e.failed_property, e.stated_reason, s.remote, s.file_id"
	        "  FROM compat_evidence e JOIN compat_session s ON s.id = e.session_id"
	        "  WHERE e.effect IN ('converted_codec', 'converted_limit', 'converted_unclear')),"
	        " g AS (SELECT server_kind, device_id, kind, codec, out_codec, effect, failed_property,"
	        "  string_agg(DISTINCT stated_reason, '; ') AS stated, count(*) AS plays,"
	        "  count(*) FILTER (WHERE remote) AS remote_plays FROM c GROUP BY 1, 2, 3, 4, 5, 6, 7),"
	        " f AS (SELECT x.server_kind, x.device_id, x.kind, x.codec, x.out_codec, x.effect, x.failed_property,"
	        "  count(*) AS files, sum(l.size)::bigint AS bytes FROM (SELECT DISTINCT server_kind, device_id, kind,"
	        "  codec, out_codec, effect, failed_property, file_id FROM c WHERE file_id IS NOT NULL) x"
	        "  JOIN library_file l ON l.id = x.file_id GROUP BY 1, 2, 3, 4, 5, 6, 7)"
	        " SELECT g.device_id, d.name, g.kind, g.codec, g.out_codec, g.effect, g.plays, g.remote_plays,"
	        "  coalesce(f.files, 0), coalesce(f.bytes, 0), g.server_kind, g.failed_property, g.stated"
	        " FROM g JOIN compat_device d ON (d.server_kind, d.client_id) = (g.server_kind, g.device_id)"
	        " LEFT JOIN f ON (f.server_kind, f.device_id, f.kind, f.codec, f.out_codec, f.effect, f.failed_property) ="
	        "  (g.server_kind, g.device_id, g.kind, g.codec, g.out_codec, g.effect, g.failed_property)"
	        " ORDER BY g.plays DESC, d.name, g.device_id, g.kind, g.codec, g.server_kind, g.failed_property LIMIT 200",
	        0, NULL, &groups, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT l.id, l.rel_path, l.size, count(*), count(DISTINCT (s.server_kind, s.device_id))"
	        " FROM compat_evidence e JOIN compat_session s ON s.id = e.session_id"
	        " JOIN library_file l ON l.id = s.file_id"
	        " WHERE e.effect IN ('converted_codec', 'converted_limit', 'converted_unclear')"
	        " GROUP BY l.id, l.rel_path, l.size ORDER BY count(*) DESC, l.size DESC, l.id LIMIT 50",
	        0, NULL, &top, error))
		return FALSE;
	*failures = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_failure_free);
	rows = axis_db_result_get_n_rows(groups);
	for (i = 0; i < rows; i++) {
		AxisCompatFailure *f = g_new0(AxisCompatFailure, 1);

		f->client_id = g_strdup(axis_db_result_get_text(groups, i, 0));
		f->device_name = g_strdup(text_or_null(groups, i, 1));
		f->kind = (gint)int_at(groups, i, 2);
		f->codec = g_strdup(axis_db_result_get_text(groups, i, 3));
		f->out_codec = g_strdup(axis_db_result_get_text(groups, i, 4));
		axis_compat_effect_from_string(axis_db_result_get_text(groups, i, 5), &f->effect);
		f->plays = (guint)int_at(groups, i, 6);
		f->remote_plays = (guint)int_at(groups, i, 7);
		f->files = (guint)int_at(groups, i, 8);
		f->bytes = int_at(groups, i, 9);
		f->server_kind = g_strdup(axis_db_result_get_text(groups, i, 10));
		axis_compat_property_from_string(text_or_null(groups, i, 11), &f->property);
		f->stated = g_strdup(text_or_null(groups, i, 12));
		g_ptr_array_add(*failures, f);
	}
	*files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_failed_file_free);
	rows = axis_db_result_get_n_rows(top);
	for (i = 0; i < rows; i++) {
		AxisCompatFailedFile *f = g_new0(AxisCompatFailedFile, 1);

		f->file_id = int_at(top, i, 0);
		f->rel_path = g_strdup(axis_db_result_get_text(top, i, 1));
		f->size = int_at(top, i, 2);
		f->plays = (guint)int_at(top, i, 3);
		f->devices = (guint)int_at(top, i, 4);
		g_ptr_array_add(*files, f);
	}
	return TRUE;
}

/* ── The path map ───────────────────────────────────────────────── */

/* After a change of the map: the recent rescan requests that no mapping
 * reached, and that it reaches now, are asked again (best effort: the map is
 * changed whatever happens here). */
static void
retry_unmapped(
	AxisCompat  *self,
	const gchar *kind
){
	g_autoptr(GError) local = NULL;
	guint count = 0;

	if (!axis_compat_notify_retry_unmapped(self, kind, &count, &local))
		g_warning("compat: the rescan requests that no path mapping reached were not looked at again: %s",
			local->message);
	else if (count > 0)
		g_message("compat: %u rescan request%s that no path mapping reached %s asked again", count,
			count == 1 ? "" : "s", count == 1 ? "is" : "are");
}

gboolean
axis_compat_path_map_add_kind(
	AxisCompat   *self,
	const gchar  *kind,
	const gchar  *server_path,
	const gchar  *axis_path,
	gchar       **problem,
	GError      **error
){
	g_autofree gchar *server = NULL;
	g_autofree gchar *axis = NULL;
	g_autofree gchar *why = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[3];

	if (problem != NULL)
		*problem = NULL;
	if (axis_compat_kind_find(kind) == NULL) {
		if (problem != NULL)
			*problem = g_strdup("That is not a kind of media server.");
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID, "invalid kind");
		return FALSE;
	}
	server = axis_compat_path_normalize(server_path, &why);
	if (server == NULL) {
		if (problem != NULL)
			*problem = g_strdup_printf("The media server's path %s.", why);
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID, "invalid path");
		return FALSE;
	}
	g_clear_pointer(&why, g_free);
	axis = axis_compat_path_normalize(axis_path, &why);
	if (axis == NULL) {
		if (problem != NULL)
			*problem = g_strdup_printf("The Axis path %s.", why);
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID, "invalid path");
		return FALSE;
	}
	params[0] = server;
	params[1] = axis;
	params[2] = kind;
	if (!axis_db_pool_exec(self->pool,
	        "INSERT INTO compat_path_map (server_path, axis_path, server_kind) VALUES ($1::text, $2::text, $3::text)",
	        3, params, NULL, &local)) {
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			if (problem != NULL)
				*problem = g_strdup("That media server path is already mapped; remove its entry first.");
			g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID, "already mapped");
		} else {
			g_propagate_error(error, g_steal_pointer(&local));
		}
		return FALSE;
	}
	retry_unmapped(self, kind);
	return TRUE;
}

gboolean
axis_compat_path_map_add(
	AxisCompat   *self,
	const gchar  *server_path,
	const gchar  *axis_path,
	gchar       **problem,
	GError      **error
){
	return axis_compat_path_map_add_kind(self, AXIS_COMPAT_KIND_PLEX, server_path, axis_path, problem, error);
}

gboolean
axis_compat_list_path_map(
	AxisCompat  *self,
	GPtrArray  **entries,
	GError     **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	*entries = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_compat_load_path_map(conn, NULL, entries, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_compat_path_map_remove(
	AxisCompat  *self,
	gint64       id,
	GError     **error
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];

	params[0] = text;
	if (!axis_db_pool_exec(self->pool, "DELETE FROM compat_path_map WHERE id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	/* A shorter mapping may reach what the removed one did not. */
	retry_unmapped(self, NULL);
	return TRUE;
}
