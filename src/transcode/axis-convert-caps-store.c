/*
 * axis-convert-caps-store.c - the measured capabilities in the database
 * (encoder_tool, encoder_capability: migration 0016), and the rule that a
 * changed ffmpeg is measured again before it is used
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-convert-caps.h"

#include <string.h>

gboolean
axis_convert_capabilities_load(
	AxisDbConn               *conn,
	const gchar              *machine,
	AxisEncoderCapabilities **caps_out,
	GError                  **error
){
	g_autoptr(AxisDbResult) tool = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisEncoderCapabilities) caps = NULL;
	const gchar *params[1];
	guint i;

	*caps_out = NULL;
	params[0] = machine;
	if (!axis_db_conn_exec(conn,
	                       "SELECT tool_version, tool_digest, (extract(epoch FROM measured_at) * 1000000)::bigint,"
	                       " aac, mov_text, subrip FROM encoder_tool WHERE machine = $1::text",
	                       1, params, &tool, error))
		return FALSE;
	if (axis_db_result_get_n_rows(tool) != 1)
		return TRUE;
	caps = axis_encoder_capabilities_new();
	caps->tool_version = g_strdup(axis_db_result_get_text(tool, 0, 0));
	caps->tool_digest = g_strdup(axis_db_result_get_text(tool, 0, 1));
	axis_db_result_get_int64(tool, 0, 2, &caps->measured_at_us);
	axis_db_result_get_boolean(tool, 0, 3, &caps->aac);
	axis_db_result_get_boolean(tool, 0, 4, &caps->mov_text);
	axis_db_result_get_boolean(tool, 0, 5, &caps->subrip);

	if (!axis_db_conn_exec(conn,
	                       "SELECT encoder, usable, ten_bit, hdr10, reason FROM encoder_capability"
	                       " WHERE machine = $1::text",
	                       1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const AxisConvertEncoder *encoder = axis_convert_encoder_named(axis_db_result_get_text(rows, i, 0));
		gboolean usable = FALSE;
		gboolean ten_bit = FALSE;
		gboolean hdr10 = FALSE;

		/* A row for an encoder this daemon does not know is not used. */
		if (encoder == NULL)
			continue;
		axis_db_result_get_boolean(rows, i, 1, &usable);
		axis_db_result_get_boolean(rows, i, 2, &ten_bit);
		axis_db_result_get_boolean(rows, i, 3, &hdr10);
		axis_encoder_capabilities_set(caps, encoder, usable, ten_bit, hdr10,
			axis_db_result_is_null(rows, i, 4) ? NULL : axis_db_result_get_text(rows, i, 4));
	}
	*caps_out = g_steal_pointer(&caps);
	return TRUE;
}

gboolean
axis_convert_capabilities_store(
	AxisDbConn                    *conn,
	const gchar                   *machine,
	const AxisEncoderCapabilities *caps,
	GError                       **error
){
	const AxisConvertEncoder *table = axis_convert_encoders();
	const gchar *params[6];
	guint i;

	g_return_val_if_fail(caps != NULL && caps->tool_version != NULL && caps->tool_digest != NULL, FALSE);

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	params[0] = machine;
	/* The rows of the encoders go with the tool's row. */
	if (!axis_db_conn_exec(conn, "DELETE FROM encoder_tool WHERE machine = $1::text", 1, params, NULL, error))
		goto fail;
	params[1] = caps->tool_version;
	params[2] = caps->tool_digest;
	params[3] = caps->aac ? "true" : "false";
	params[4] = caps->mov_text ? "true" : "false";
	params[5] = caps->subrip ? "true" : "false";
	if (!axis_db_conn_exec(conn,
	                       "INSERT INTO encoder_tool (machine, tool_version, tool_digest, aac, mov_text, subrip)"
	                       " VALUES ($1::text, $2::text, $3::text, $4::boolean, $5::boolean, $6::boolean)",
	                       6, params, NULL, error))
		goto fail;
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisEncoderCapability *cap = &caps->encoders[i];

		/* What was not measured is not stored: it reads back as not
		 * measured. */
		if (!cap->measured)
			continue;
		params[1] = table[i].name;
		params[2] = cap->usable ? "true" : "false";
		params[3] = cap->ten_bit ? "true" : "false";
		params[4] = cap->hdr10 ? "true" : "false";
		params[5] = cap->reason;
		if (!axis_db_conn_exec(conn,
		                       "INSERT INTO encoder_capability (machine, encoder, usable, ten_bit, hdr10, reason)"
		                       " VALUES ($1::text, $2::text, $3::boolean, $4::boolean, $5::boolean, $6::text)",
		                       6, params, NULL, error))
			goto fail;
	}
	if (!axis_db_conn_commit(conn, error))
		goto fail;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

gboolean
axis_convert_capabilities_ensure(
	AxisDbPool                *pool,
	const gchar               *machine,
	const AxisCapabilityTool  *tool,
	AxisEncoderCapabilities  **caps_out,
	gboolean                  *measured,
	GError                   **error
){
	g_autoptr(AxisEncoderCapabilities) stored = NULL;
	g_autoptr(AxisEncoderCapabilities) fresh = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *digest = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*caps_out = NULL;
	if (measured != NULL)
		*measured = FALSE;

	/* What ffmpeg is now, before anything is read or measured. */
	if (!axis_convert_tool_identity(tool, &version, &digest, error))
		return FALSE;

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_convert_capabilities_load(conn, machine, &stored, error);
	axis_db_pool_release(pool, conn);
	if (!ok)
		return FALSE;
	if (stored != NULL && g_strcmp0(stored->tool_digest, digest) == 0) {
		*caps_out = g_steal_pointer(&stored);
		return TRUE;
	}

	/* Another build, or none measured: measure (no connection held), then
	 * store.  A build that changes during the measurement is caught by the
	 * next call, which sees its digest differ. */
	fresh = axis_convert_capabilities_measure(tool, error);
	if (fresh == NULL)
		return FALSE;
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_convert_capabilities_store(conn, machine, fresh, error);
	axis_db_pool_release(pool, conn);
	if (!ok)
		return FALSE;
	if (measured != NULL)
		*measured = TRUE;
	*caps_out = g_steal_pointer(&fresh);
	return TRUE;
}
