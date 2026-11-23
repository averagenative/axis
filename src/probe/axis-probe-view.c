/*
 * axis-probe-view.c - what the probe pages read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-private.h"

#include <string.h>

/* The conditions under which a file needs probing; the same as
 * axis_probe_target_pending(), as SQL.  $2 is the current version. */
#define PENDING_SQL \
	"(f.kind IN ('video', 'audio') AND f.state = 'present'" \
	" AND ((f.changed AND f.changed_generation <= coalesce(s.complete_generation, 0))" \
	"   OR (m.file_id IS NOT NULL AND m.probe_version < $2::integer)))"

void
axis_file_view_free(AxisFileView *view)
{
	if (view == NULL)
		return;

	g_free(view->rel_path);
	g_free(view->kind);
	g_free(view->state);
	g_free(view->delayed_reason);
	g_free(view->link_target);
	axis_media_info_free(view->info);
	g_free(view);
}

static void
listing_free(gpointer data)
{
	AxisProbeListing *listing = data;

	g_free(listing->rel_path);
	g_free(listing->status);
	g_free(listing->text);
	g_free(listing);
}

void
axis_root_probe_view_free(AxisRootProbeView *view)
{
	if (view == NULL)
		return;

	if (view->failures != NULL)
		g_ptr_array_unref(view->failures);
	if (view->recent != NULL)
		g_ptr_array_unref(view->recent);
	g_free(view);
}

gboolean
axis_probe_get_file_view(
	AxisProbe     *self,
	gint64         file_id,
	AxisFileView **view,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisFileView) out = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	g_autofree gchar *version = g_strdup_printf("%d", axis_probe_current_version(self));
	const gchar *params[2];
	AxisDbConn *conn;
	gint64 sec = 0;
	gint64 nsec = 0;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(view != NULL, FALSE);

	*view = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	params[1] = version;
	if (!axis_db_conn_exec(conn,
	                       "SELECT f.id, f.root_id, f.volume_id, f.rel_path, f.kind, f.state, f.size,"
	                       " f.mtime_sec, f.mtime_nsec, f.pinned, f.pinned_links, f.pinned_downloads, f.changed,"
	                       " " PENDING_SQL ", d.reason,"
	                       " (extract(epoch FROM d.until) * 1000000)::bigint,"
	                       " f.nlink, f.name_invalid_utf8,"
	                       " (extract(epoch FROM f.created_at) * 1000000)::bigint,"
	                       " (extract(epoch FROM f.suspected_at) * 1000000)::bigint,"
	                       " (extract(epoch FROM f.missing_at) * 1000000)::bigint,"
	                       " f.link_target, dd.id"
	                       " FROM library_file f"
	                       " LEFT JOIN library_root_state s ON s.root_id = f.root_id"
	                       " LEFT JOIN media_info m ON m.file_id = f.id"
	                       " LEFT JOIN media_probe_defer d ON d.file_id = f.id"
	                       " LEFT JOIN library_dir dd ON dd.root_id = f.root_id AND dd.path_hash = f.dir_hash"
	                       " WHERE f.id = $1::bigint",
	                       2, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND, "there is no file %" G_GINT64_FORMAT,
			file_id);
		goto fail;
	}

	out = g_new0(AxisFileView, 1);
	out->file_id = file_id;
	axis_db_result_get_int64(result, 0, 1, &out->root_id);
	axis_db_result_get_int64(result, 0, 2, &out->volume_id);
	out->rel_path = g_strdup(axis_db_result_get_text(result, 0, 3));
	out->kind = g_strdup(axis_db_result_get_text(result, 0, 4));
	out->state = g_strdup(axis_db_result_get_text(result, 0, 5));
	axis_db_result_get_int64(result, 0, 6, &out->size);
	axis_db_result_get_int64(result, 0, 7, &sec);
	axis_db_result_get_int64(result, 0, 8, &nsec);
	out->mtime_us = sec * G_USEC_PER_SEC + nsec / 1000;
	axis_db_result_get_boolean(result, 0, 9, &out->pinned);
	axis_db_result_get_boolean(result, 0, 10, &out->pinned_links);
	axis_db_result_get_boolean(result, 0, 11, &out->pinned_downloads);
	axis_db_result_get_boolean(result, 0, 12, &out->changed);
	axis_db_result_get_boolean(result, 0, 13, &out->waiting);
	if (!axis_db_result_is_null(result, 0, 14)) {
		out->delayed_reason = g_strdup(axis_db_result_get_text(result, 0, 14));
		axis_db_result_get_int64(result, 0, 15, &out->delayed_until_us);
	}
	axis_db_result_get_int64(result, 0, 16, &out->nlink);
	axis_db_result_get_boolean(result, 0, 17, &out->name_invalid);
	axis_db_result_get_int64(result, 0, 18, &out->first_seen_us);
	axis_db_result_get_int64(result, 0, 19, &out->suspected_us);
	axis_db_result_get_int64(result, 0, 20, &out->missing_us);
	out->link_target = g_strdup(axis_db_result_get_text(result, 0, 21));
	axis_db_result_get_int64(result, 0, 22, &out->dir_id);
	g_clear_pointer(&result, axis_db_result_free);

	if (!axis_probe_store_load_info(conn, file_id, &out->info, error))
		goto fail;
	axis_db_pool_release(self->pool, conn);

	*view = g_steal_pointer(&out);
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

gboolean
axis_probe_load_info(
	AxisDbConn     *conn,
	gint64          file_id,
	AxisMediaInfo **info,
	GError        **error
){
	g_return_val_if_fail(conn != NULL && info != NULL, FALSE);

	return axis_probe_store_load_info(conn, file_id, info, error);
}

static GPtrArray *
read_listing(AxisDbResult *result)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(listing_free);
	guint i;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisProbeListing *listing = g_new0(AxisProbeListing, 1);

		axis_db_result_get_int64(result, i, 0, &listing->file_id);
		listing->rel_path = g_strdup(axis_db_result_get_text(result, i, 1));
		listing->status = g_strdup(axis_db_result_get_text(result, i, 2));
		listing->text = g_strdup(axis_db_result_get_text(result, i, 3));
		axis_db_result_get_int64(result, i, 4, &listing->probed_at_us);
		g_ptr_array_add(list, listing);
	}
	return list;
}

gboolean
axis_probe_get_root_view(
	AxisProbe          *self,
	gint64              root_id,
	AxisRootProbeView **view,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisRootProbeView) out = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, root_id);
	g_autofree gchar *version = g_strdup_printf("%d", axis_probe_current_version(self));
	g_autofree gchar *listed = g_strdup_printf("%d", AXIS_PROBE_LISTED_FILES);
	const gchar *params[3];
	AxisDbConn *conn;
	gint64 value = 0;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(view != NULL, FALSE);

	*view = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	out = g_new0(AxisRootProbeView, 1);
	out->root_id = root_id;

	params[0] = id;
	params[1] = version;
	if (!axis_db_conn_exec(conn,
	                       "SELECT count(*),"
	                       " count(*) FILTER (WHERE m.status = 'ok'),"
	                       " count(*) FILTER (WHERE m.status = 'failed'),"
	                       " count(*) FILTER (WHERE m.status = 'timeout'),"
	                       " count(*) FILTER (WHERE m.file_id IS NULL),"
	                       " count(*) FILTER (WHERE " PENDING_SQL "),"
	                       " count(*) FILTER (WHERE " PENDING_SQL " AND d.until > now()),"
	                       " count(*) FILTER (WHERE m.file_id IS NOT NULL AND m.probe_version < $2::integer),"
	                       " count(*) FILTER (WHERE m.hdr_uncertain)"
	                       " FROM library_file f"
	                       " LEFT JOIN library_root_state s ON s.root_id = f.root_id"
	                       " LEFT JOIN media_info m ON m.file_id = f.id"
	                       " LEFT JOIN media_probe_defer d ON d.file_id = f.id"
	                       " WHERE f.root_id = $1::bigint AND f.kind IN ('video', 'audio') AND f.state = 'present'",
	                       2, params, &result, error))
		goto fail;
	if (axis_db_result_get_int64(result, 0, 0, &value))
		out->media_files = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 1, &value))
		out->probed = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 2, &value))
		out->failed = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 3, &value))
		out->timed_out = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 4, &value))
		out->never_probed = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 5, &value))
		out->waiting = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 6, &value))
		out->delayed = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 7, &value))
		out->stale = (guint64)value;
	if (axis_db_result_get_int64(result, 0, 8, &value))
		out->hdr_uncertain = (guint64)value;
	g_clear_pointer(&result, axis_db_result_free);

	params[0] = id;
	params[1] = listed;
	if (!axis_db_conn_exec(conn,
	                       "SELECT f.id, f.rel_path, m.status, m.error,"
	                       " (extract(epoch FROM m.probed_at) * 1000000)::bigint"
	                       " FROM media_info m JOIN library_file f ON f.id = m.file_id"
	                       " WHERE f.root_id = $1::bigint AND m.status <> 'ok'"
	                       " ORDER BY m.probed_at DESC, f.id LIMIT $2::integer",
	                       2, params, &result, error))
		goto fail;
	out->failures = read_listing(result);
	g_clear_pointer(&result, axis_db_result_free);

	if (!axis_db_conn_exec(conn,
	                       "SELECT f.id, f.rel_path, m.status, m.container,"
	                       " (extract(epoch FROM m.probed_at) * 1000000)::bigint"
	                       " FROM media_info m JOIN library_file f ON f.id = m.file_id"
	                       " WHERE f.root_id = $1::bigint AND m.status = 'ok'"
	                       " ORDER BY m.probed_at DESC, f.id LIMIT $2::integer",
	                       2, params, &result, error))
		goto fail;
	out->recent = read_listing(result);
	axis_db_pool_release(self->pool, conn);

	*view = g_steal_pointer(&out);
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}
