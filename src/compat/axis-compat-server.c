/*
 * axis-compat-server.c - the server's identity, its libraries, the path map
 * and roots as the collection reads them, and the retention
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-private.h"

#include <string.h>

#include "jobs/axis-job-error.h"

/* ── Loaders ────────────────────────────────────────────────────── */

gboolean
axis_compat_load_path_map(
	AxisDbConn  *conn,
	const gchar *kind,
	GPtrArray  **entries,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint rows;
	guint i;

	*entries = NULL;
	params[0] = kind;
	if (!axis_db_conn_exec(conn,
	        "SELECT id, server_path, axis_path, server_kind FROM compat_path_map"
	        " WHERE $1::text IS NULL OR server_kind = $1::text ORDER BY id",
	        1, params, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	*entries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_path_entry_free);
	for (i = 0; i < rows; i++) {
		AxisCompatPathEntry *entry = g_new0(AxisCompatPathEntry, 1);

		axis_db_result_get_int64(result, i, 0, &entry->id);
		entry->server = axis_compat_path_normalize(axis_db_result_get_text(result, i, 1), NULL);
		entry->axis = axis_compat_path_normalize(axis_db_result_get_text(result, i, 2), NULL);
		entry->kind = g_strdup(axis_db_result_get_text(result, i, 3));
		if (entry->server == NULL || entry->axis == NULL)
			axis_compat_path_entry_free(entry);
		else
			g_ptr_array_add(*entries, entry);
	}
	return TRUE;
}

gboolean
axis_compat_load_roots(
	AxisDbConn  *conn,
	GPtrArray  **roots,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint rows;
	guint i;

	*roots = NULL;
	if (!axis_db_conn_exec(conn,
	        "SELECT r.id, r.media_type, CASE WHEN r.rel_path = '' THEN v.path ELSE v.path || '/' || r.rel_path END"
	        " FROM library_root r JOIN volume v ON v.id = r.volume_id ORDER BY r.id",
	        0, NULL, &result, error))
		return FALSE;
	rows = axis_db_result_get_n_rows(result);
	*roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_root_ref_free);
	for (i = 0; i < rows; i++) {
		AxisCompatRootRef *root = g_new0(AxisCompatRootRef, 1);

		axis_db_result_get_int64(result, i, 0, &root->id);
		root->media_type = g_strdup(axis_db_result_get_text(result, i, 1));
		root->path = axis_compat_path_normalize(axis_db_result_get_text(result, i, 2), NULL);
		if (root->path == NULL)
			axis_compat_root_ref_free(root);
		else
			g_ptr_array_add(*roots, root);
	}
	return TRUE;
}

gchar *
axis_compat_device_key(
	const gchar *kind,
	const gchar *client_id
){
	/* Neither a kind nor an id has a slash. */
	return g_strconcat(kind, "/", client_id, NULL);
}

gchar *
axis_compat_array_literal(GPtrArray *items)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		const gchar *p;

		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, '"');
		for (p = g_ptr_array_index(items, i); *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* ── The server ─────────────────────────────────────────────────── */

gboolean
axis_compat_ensure_state(
	AxisDbConn  *conn,
	const gchar *kind,
	GError     **error
){
	const gchar *params[1];

	params[0] = kind;
	/* The Plex row is made by the migration (id 1); another kind's row gets
	 * the next free id when its connection is first used. */
	return axis_db_conn_exec(conn,
		"INSERT INTO compat_state (id, server_kind)"
		" SELECT (SELECT coalesce(max(id), 0) + 1 FROM compat_state), $1::text"
		" WHERE NOT EXISTS (SELECT 1 FROM compat_state WHERE server_kind = $1::text)"
		" ON CONFLICT DO NOTHING", 1, params, NULL, error);
}

gboolean
axis_compat_adopt_server(
	AxisDbConn               *conn,
	const gchar              *kind,
	const AxisCompatIdentity *identity,
	GError                  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[4];
	const gchar *previous;

	if (!axis_compat_ensure_state(conn, kind, error))
		return FALSE;
	params[0] = kind;
	if (!axis_db_conn_exec(conn, "SELECT server_id FROM compat_state WHERE server_kind = $1::text", 1, params,
	        &result, error))
		return FALSE;
	previous = axis_db_result_get_n_rows(result) > 0 && !axis_db_result_is_null(result, 0, 0) ?
		axis_db_result_get_text(result, 0, 0) : NULL;
	if (previous != NULL && g_strcmp0(previous, identity->machine_id) != 0) {
		g_warning("compat: the media server is not the one the records belong to; they are removed and "
			"collection starts again");
		if (!axis_db_conn_exec(conn, "DELETE FROM compat_session WHERE server_kind = $1::text", 1, params, NULL,
		        error) ||
		    !axis_db_conn_exec(conn, "DELETE FROM compat_play WHERE server_kind = $1::text", 1, params, NULL,
		        error) ||
		    !axis_db_conn_exec(conn, "DELETE FROM compat_library WHERE server_kind = $1::text", 1, params, NULL,
		        error) ||
		    !axis_db_conn_exec(conn, "DELETE FROM compat_device WHERE NOT pinned AND server_kind = $1::text", 1,
		        params, NULL, error) ||
		    !axis_db_conn_exec(conn,
		        "UPDATE compat_state SET started_at = NULL, history_at = NULL, libraries_at = NULL,"
		        " history_error = NULL WHERE server_kind = $1::text", 1, params, NULL, error))
			return FALSE;
	}
	params[0] = identity->machine_id;
	params[1] = identity->name;
	params[2] = identity->version;
	params[3] = kind;
	return axis_db_conn_exec(conn,
		"UPDATE compat_state SET server_id = $1::text, server_name = $2::text, server_version = $3::text,"
		" started_at = coalesce(started_at, now()) WHERE server_kind = $4::text", 4, params, NULL, error);
}

/* Stores the devices a server lists (those it has not played anything on
 * are listed too: they are known, and a person may want to pin one). */
static gboolean
store_devices(
	AxisCompat           *self,
	const AxisCompatKind *kind,
	GPtrArray            *devices,
	GError              **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok = FALSE;
	guint i;

	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto out;
	for (i = 0; i < devices->len; i++) {
		const AxisCompatDevice *d = g_ptr_array_index(devices, i);
		const gchar *params[5];

		if (d->client_id == NULL)
			continue;
		params[0] = kind->name;
		params[1] = d->client_id;
		params[2] = d->name;
		params[3] = d->platform;
		params[4] = d->product;
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO compat_device (server_kind, client_id, name, platform, product) VALUES ($1::text,"
		        " $2::text, $3::text, $4::text, $5::text) ON CONFLICT (server_kind, client_id) DO UPDATE SET"
		        " name = coalesce(EXCLUDED.name, compat_device.name),"
		        " platform = coalesce(EXCLUDED.platform, compat_device.platform),"
		        " product = coalesce(EXCLUDED.product, compat_device.product)",
		        5, params, NULL, error))
			goto out;
	}
	if (!axis_db_conn_commit(conn, error))
		goto out;
	ok = TRUE;
out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_compat_refresh_libraries(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GError                    **error
){
	const AxisCompatKind *kind = connection->kind;
	g_autoptr(GPtrArray) libraries = NULL;
	AxisDbConn *conn;
	gboolean ok = FALSE;
	guint i;

	if (!kind->fetch_libraries(self, connection, cancellable, &libraries, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error))
		goto out;
	for (i = 0; i < libraries->len; i++) {
		const AxisCompatLibrary *library = g_ptr_array_index(libraries, i);
		g_autofree gchar *folders = axis_compat_array_literal(library->folders);
		const gchar *params[5];

		params[0] = library->key;
		params[1] = library->kind;
		params[2] = library->title;
		params[3] = folders;
		params[4] = kind->name;
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO compat_library (key, kind, title, folders, server_kind) VALUES ($1::text, $2::text,"
		        " $3::text, $4::text[], $5::text) ON CONFLICT (server_kind, key) DO UPDATE SET"
		        " kind = EXCLUDED.kind, title = EXCLUDED.title, folders = EXCLUDED.folders, seen_at = now()",
		        5, params, NULL, error))
			goto out;
	}
	{
		const gchar *params[1];

		params[0] = kind->name;
		/* A library the server no longer lists is no longer one. */
		if (!axis_db_conn_exec(conn,
		        "DELETE FROM compat_library WHERE server_kind = $1::text AND seen_at < transaction_timestamp()",
		        1, params, NULL, error) ||
		    !axis_db_conn_exec(conn, "UPDATE compat_state SET libraries_at = now() WHERE server_kind = $1::text", 1,
		        params, NULL, error) ||
		    !axis_db_conn_commit(conn, error))
			goto out;
	}
	ok = TRUE;
out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && !kind->has_history && kind->fetch_devices != NULL) {
		g_autoptr(GPtrArray) devices = NULL;
		g_autoptr(GError) local = NULL;

		/* A server with no history is asked for its devices instead, so
		 * that the ones that never played are known by name.  A failure
		 * here does not undo the libraries. */
		if (!kind->fetch_devices(self, connection, cancellable, &devices, &local) ||
		    !store_devices(self, kind, devices, &local)) {
			if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			g_message("compat: the devices could not be read: %s", local->message);
		}
	}
	return ok;
}

/* ── Retention ──────────────────────────────────────────────────── */

gboolean
axis_compat_apply_retention(
	AxisCompat  *self,
	guint       *sessions,
	guint       *plays,
	GError     **error
){
	g_autoptr(AxisDbResult) deleted_sessions = NULL;
	g_autoptr(AxisDbResult) deleted_plays = NULL;
	g_autofree gchar *seconds = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok = FALSE;

	if (sessions != NULL)
		*sessions = 0;
	if (plays != NULL)
		*plays = 0;
	seconds = g_strdup_printf("%" G_GINT64_FORMAT, self->settings != NULL ?
		axis_settings_get_duration(self->settings, AXIS_COMPAT_SETTING_RETENTION) : (gint64)400 * 86400);
	params[0] = seconds;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_exec(conn,
	        "DELETE FROM compat_session WHERE last_seen < now() - $1::double precision * interval '1 second'",
	        1, params, &deleted_sessions, error) ||
	    !axis_db_conn_exec(conn,
	        "DELETE FROM compat_play WHERE viewed_at < now() - $1::double precision * interval '1 second'",
	        1, params, &deleted_plays, error) ||
	    !axis_db_conn_exec(conn,
	        "DELETE FROM compat_device d WHERE NOT d.pinned"
	        " AND d.last_seen < now() - $1::double precision * interval '1 second'"
	        " AND NOT EXISTS (SELECT 1 FROM compat_session s WHERE s.device_id = d.client_id"
	        "  AND s.server_kind = d.server_kind)"
	        " AND NOT EXISTS (SELECT 1 FROM compat_play p WHERE p.device_id = d.client_id"
	        "  AND p.server_kind = d.server_kind)",
	        1, params, NULL, error))
		goto out;
	if (sessions != NULL)
		*sessions = (guint)axis_db_result_get_affected_rows(deleted_sessions);
	if (plays != NULL)
		*plays = (guint)axis_db_result_get_affected_rows(deleted_plays);
	ok = TRUE;
out:
	axis_db_pool_release(self->pool, conn);
	return ok;
}
