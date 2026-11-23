/*
 * axis-library-root.c - library roots: directories on volumes with a media
 * type
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A root is checked against its volume through a read guard: the volume
 * must be online, and the directory is opened beneath the guard's
 * descriptor without following a symbolic link or leaving the volume's
 * filesystem, then the guard is checked again so the walk cannot have
 * happened on a disk swapped in meanwhile.  Nesting is decided while the
 * volume's row is locked, so two roots added at once cannot nest.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-storage-private.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-guard.h"

#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#define ROOT_COLUMNS \
	"id, volume_id, rel_path, media_type, name, " \
	"(extract(epoch FROM created_at) * 1000000)::bigint"

static AxisRootInfo *
root_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisRootInfo *root = g_new0(AxisRootInfo, 1);

	axis_db_result_get_int64(result, row, 0, &root->id);
	axis_db_result_get_int64(result, row, 1, &root->volume_id);
	root->rel_path = g_strdup(axis_db_result_get_text(result, row, 2));
	if (!axis_media_type_from_string(axis_db_result_get_text(result, row, 3), &root->media_type))
		root->media_type = AXIS_MEDIA_OTHER;
	root->name = g_strdup(axis_db_result_get_text(result, row, 4));
	axis_db_result_get_int64(result, row, 5, &root->created_us);

	if (root->rel_path == NULL)
		root->rel_path = g_strdup("");
	if (root->name == NULL)
		root->name = g_strdup("");
	return root;
}

gboolean
axis_storage_list_roots(
	AxisStorage  *self,
	gint64        volume_id,
	GPtrArray   **roots,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, volume_id);
	const gchar *params[1];
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(roots != NULL, FALSE);

	*roots = NULL;
	params[0] = id;
	if (volume_id > 0)
		ok = axis_db_pool_exec(self->pool, "SELECT " ROOT_COLUMNS " FROM library_root"
			" WHERE volume_id = $1::bigint ORDER BY rel_path, id", 1, params, &result, error);
	else
		ok = axis_db_pool_exec(self->pool, "SELECT " ROOT_COLUMNS " FROM library_root"
			" ORDER BY volume_id, rel_path, id", 0, NULL, &result, error);
	if (!ok)
		return FALSE;

	*roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_root_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*roots, root_from_row(result, i));

	return TRUE;
}

gboolean
axis_storage_get_root(
	AxisStorage   *self,
	gint64         id,
	AxisRootInfo **root,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(root != NULL, FALSE);

	*root = NULL;
	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, "SELECT " ROOT_COLUMNS " FROM library_root WHERE id = $1::bigint",
	                       1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no root %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	*root = root_from_row(result, 0);
	return TRUE;
}

/* @input as a path relative to the volume at @volume_path. */
static gboolean
relative_path(
	const gchar  *input,
	const gchar  *volume_path,
	gchar       **relative,
	GError      **error
){
	const gchar *problem = NULL;
	const gchar *p = input != NULL ? input : "";
	g_autofree gchar *absolute = NULL;
	gsize length;

	while (*p != '\0' && g_ascii_isspace(*p))
		p++;

	if (*p != '/') {
		if (!axis_storage_path_normalize(input, FALSE, relative, &problem)) {
			g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, problem);
			return FALSE;
		}
		return TRUE;
	}

	/* An absolute path is accepted when it is inside the volume. */
	if (!axis_storage_path_normalize(input, TRUE, &absolute, &problem)) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, problem);
		return FALSE;
	}
	if (!axis_storage_path_contains(volume_path, absolute)) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
			"That directory is not inside the disk, which is at %s.", volume_path);
		return FALSE;
	}

	length = strlen(volume_path);
	*relative = g_strdup(absolute[length] == '/' ? absolute + length + 1 : "");
	return TRUE;
}

/* Opens @relative through a read guard to prove it is a directory inside
 * the volume, then checks the guard again. */
static gboolean
verify_directory(
	AxisStorage          *self,
	const AxisVolumeInfo *volume,
	const gchar          *relative,
	GError              **error
){
	g_autoptr(AxisVolumeGuard) guard = NULL;
	GError *local = NULL;
	gint fd;

	guard = axis_volume_guard_acquire(self, volume->id, AXIS_VOLUME_INTENT_READ, error);
	if (guard == NULL)
		return FALSE;

	fd = axis_volume_guard_open_dir(guard, relative, &local);
	if (fd < 0) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"There is no directory %s on disk '%s'.", relative, volume->name);
			g_error_free(local);
		} else if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"%s on disk '%s' is not a directory.", relative, volume->name);
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
		}
		return FALSE;
	}
	close(fd);

	return axis_volume_guard_check(guard, error);
}

gboolean
axis_storage_add_root(
	AxisStorage    *self,
	gint64          volume_id,
	const gchar    *path,
	AxisMediaType   media_type,
	const gchar    *name,
	gint64         *id,
	GError        **error
){
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *relative = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *default_name = NULL;
	g_autofree gchar *volume_text = g_strdup_printf("%" G_GINT64_FORMAT, volume_id);
	const gchar *problem = NULL;
	const gchar *params[4];
	AxisDbConn *conn;
	gint64 new_id = 0;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (id != NULL)
		*id = 0;
	if ((guint)media_type >= AXIS_MEDIA_TYPE_COUNT) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID,
			"The media type is not one Axis knows.");
		return FALSE;
	}

	volume = axis_storage_get_volume(self, volume_id);
	if (volume == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"That disk is no longer in Axis. Reload the page.");
		return FALSE;
	}

	if (!relative_path(path, volume->path, &relative, error))
		return FALSE;

	default_name = relative[0] != '\0' ? axis_storage_path_last_segment(relative) :
		g_strdup(volume->name);
	if (!axis_storage_label_normalize(name != NULL && name[0] != '\0' ? name : default_name, &label,
	                                  &problem)) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, problem);
		return FALSE;
	}

	if (!verify_directory(self, volume, relative, error))
		return FALSE;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = volume_text;
	if (!axis_db_conn_begin(conn, error) ||
	    !axis_db_conn_exec(conn, "SELECT id FROM volume WHERE id = $1::bigint FOR UPDATE", 1, params,
	                       &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"Disk '%s' was removed meanwhile.", volume->name);
		goto fail;
	}
	g_clear_pointer(&result, axis_db_result_free);

	if (!axis_db_conn_exec(conn, "SELECT rel_path, name FROM library_root WHERE volume_id = $1::bigint",
	                       1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) >= AXIS_STORAGE_MAX_ROOTS_PER_VOLUME) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
			"This disk has as many library folders as Axis allows.");
		goto fail;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *other = axis_db_result_get_text(result, i, 0);
		const gchar *other_name = axis_db_result_get_text(result, i, 1);

		if (other == NULL)
			continue;
		if (strcmp(other, relative) == 0) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"That directory is library folder '%s' already.", other_name);
			goto fail;
		}
		if (axis_storage_path_contains(other, relative)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"That directory is inside library folder '%s'. Library folders cannot be inside one another.",
				other_name);
			goto fail;
		}
		if (axis_storage_path_contains(relative, other)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"Library folder '%s' is inside that directory. Library folders cannot contain one another.",
				other_name);
			goto fail;
		}
	}
	g_clear_pointer(&result, axis_db_result_free);

	params[1] = relative;
	params[2] = axis_media_type_to_string(media_type);
	params[3] = label;
	if (!axis_db_conn_exec(conn, "INSERT INTO library_root (volume_id, rel_path, media_type, name)"
	                       " VALUES ($1::bigint, $2, $3, $4) RETURNING id", 4, params, &result, error) ||
	    !axis_db_result_get_int64(result, 0, 0, &new_id) ||
	    !axis_db_conn_commit(conn, error))
		goto fail;
	axis_db_pool_release(self->pool, conn);

	axis_storage_count_roots(self, volume_id, 1);
	g_message("root '%s' (%s) added on volume '%s' at %s%s%s", label,
		axis_media_type_to_string(media_type), volume->name, volume->path,
		relative[0] != '\0' ? "/" : "", relative);
	axis_storage_notify(self, NULL, FALSE);
	if (id != NULL)
		*id = new_id;
	return TRUE;

fail:
	if (error != NULL && *error == NULL)
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_FAILED,
			"The library folder could not be saved.");
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

gboolean
axis_storage_remove_root(
	AxisStorage  *self,
	gint64        id,
	GError      **error
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	GError *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_storage_get_root(self, id, &root, error))
		return FALSE;
	volume = axis_storage_get_volume(self, root->volume_id);
	if (volume == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, root->volume_id);
		return FALSE;
	}

	/* Nothing about a volume is forgotten while it is away. */
	guard = axis_volume_guard_acquire(self, root->volume_id, AXIS_VOLUME_INTENT_READ, &local);
	if (guard == NULL) {
		g_propagate_prefixed_error(error, local, "A library folder is only removed while its disk is "
			"online. ");
		return FALSE;
	}

	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, "DELETE FROM library_root WHERE id = $1::bigint", 1, params,
	                       &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) != 1) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no root %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	axis_storage_count_roots(self, root->volume_id, -1);
	g_message("root '%s' (id %" G_GINT64_FORMAT ") on volume '%s' removed; nothing on the disk was "
		"touched", root->name, id, volume->name);
	axis_storage_notify(self, NULL, FALSE);
	return TRUE;
}
