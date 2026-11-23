/*
 * axis-library-browse.c - what the Library pages read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every statement here is a constant, or assembled from constant pieces
 * chosen by enumerations; every value a request carries reaches the
 * database as a parameter.
 */

#include "library/axis-analysis-rules.h"
#include "library/axis-library-private.h"
#include "library/axis-library-browse.h"
#include "library/axis-library-browse-private.h"

#include <string.h>

/* ── Freeing ────────────────────────────────────────────────────── */

void
axis_dir_record_free(AxisDirRecord *dir)
{
	if (dir == NULL)
		return;

	g_free(dir->path);
	g_free(dir->name);
	g_free(dir);
}

void
axis_browse_file_free(AxisBrowseFile *file)
{
	if (file == NULL)
		return;

	g_free(file->rel_path);
	g_free(file->kind);
	g_free(file->state);
	g_free(file->probe_status);
	g_free(file->container);
	g_free(file->video_codec);
	g_free(file->resolution);
	g_free(file->hdr);
	g_free(file->audio_codec);
	g_free(file);
}

void
axis_folder_view_free(AxisFolderView *view)
{
	if (view == NULL)
		return;

	axis_dir_record_free(view->dir);
	g_clear_pointer(&view->crumbs, g_ptr_array_unref);
	g_clear_pointer(&view->subdirs, g_ptr_array_unref);
	g_clear_pointer(&view->files, g_ptr_array_unref);
	g_free(view);
}

void
axis_file_list_free(AxisFileList *list)
{
	if (list == NULL)
		return;

	g_clear_pointer(&list->files, g_ptr_array_unref);
	g_free(list);
}

/* ── Helpers ────────────────────────────────────────────────────── */

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

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

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(result, row, column));
}

/* One transaction under a deadline: a statement that runs out of time is
 * cancelled by the server, the transaction rolled back and the connection
 * left usable.  *@result is the statement's. */
static gboolean
timed_exec(
	AxisDbConn          *conn,
	const gchar         *timeout,
	const gchar         *sql,
	guint                n_params,
	const gchar *const  *params,
	AxisDbResult       **result,
	GError             **error
){
	g_autofree gchar *set = g_strdup_printf("SET LOCAL statement_timeout = '%s'", timeout);

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!axis_db_conn_exec(conn, set, 0, NULL, NULL, error) ||
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

/* ── The overview ───────────────────────────────────────────────── */

gboolean
axis_library_get_overview(
	AxisLibrary  *self,
	GPtrArray   **roots,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(roots != NULL, FALSE);

	*roots = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	ok = timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
	                "SELECT r.id, d.id, d.total_files, d.total_bytes,"
	                " (extract(epoch FROM cs.finished_at) * 1000000)::bigint, cs.id,"
	                " ls.status, (extract(epoch FROM ls.started_at) * 1000000)::bigint,"
	                " ls.files, ls.dirs, ls.previous_files,"
	                " (extract(epoch FROM s.scan_requested_at) * 1000000)::bigint,"
	                " ls.status = 'running' AND (ls.job_id IS NULL OR j.state = 'running')"
	                " FROM library_root r"
	                " LEFT JOIN library_root_state s ON s.root_id = r.id"
	                " LEFT JOIN library_scan cs ON cs.id = s.last_complete_scan_id"
	                " LEFT JOIN library_scan ls ON ls.id = s.last_scan_id"
	                " LEFT JOIN job j ON j.id = ls.job_id"
	                " LEFT JOIN library_dir d ON d.root_id = r.id AND d.path_hash = library_path_hash('')"
	                " ORDER BY r.id LIMIT 2000",
	                0, NULL, &result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisRootOverview *root = g_new0(AxisRootOverview, 1);
		g_autofree gchar *status = text_at(result, i, 6);

		root->root_id = int_at(result, i, 0, 0);
		root->dir_id = int_at(result, i, 1, 0);
		root->files = (guint64)MAX(int_at(result, i, 2, 0), 0);
		root->bytes = (guint64)MAX(int_at(result, i, 3, 0), 0);
		root->last_complete_us = int_at(result, i, 4, 0);
		root->last_complete_scan_id = int_at(result, i, 5, 0);
		root->has_last = status != NULL && axis_scan_status_from_string(status, &root->last_status);
		root->last_started_us = int_at(result, i, 7, 0);
		root->running = bool_at(result, i, 12);
		if (root->running) {
			root->run_files = (guint64)MAX(int_at(result, i, 8, 0), 0);
			root->run_dirs = (guint64)MAX(int_at(result, i, 9, 0), 0);
			root->run_previous = int_at(result, i, 10, 0);
		}
		root->requested_us = int_at(result, i, 11, 0);
		g_ptr_array_add(out, root);
	}

	*roots = g_steal_pointer(&out);
	return TRUE;
}

/* ── Files ──────────────────────────────────────────────────────── */

#define FILE_COLUMNS AXIS_LIBRARY_FILE_COLUMNS

static AxisBrowseFile *
read_file(
	AxisDbResult *result,
	guint         row
){
	AxisBrowseFile *file = g_new0(AxisBrowseFile, 1);

	file->id = int_at(result, row, 0, 0);
	file->root_id = int_at(result, row, 1, 0);
	file->rel_path = text_at(result, row, 2);
	file->kind = text_at(result, row, 3);
	file->state = text_at(result, row, 4);
	file->size = int_at(result, row, 5, 0);
	file->mtime_us = int_at(result, row, 6, 0) * G_USEC_PER_SEC;
	file->pinned = bool_at(result, row, 7);
	file->probe_status = text_at(result, row, 8);
	file->container = text_at(result, row, 9);
	file->video_codec = text_at(result, row, 10);
	file->resolution = text_at(result, row, 11);
	file->hdr = text_at(result, row, 12);
	file->hdr_uncertain = bool_at(result, row, 13);
	file->audio_codec = text_at(result, row, 14);
	file->duration_ms = int_at(result, row, 15, -1);
	file->bitrate = int_at(result, row, 16, -1);
	return file;
}

static GPtrArray *
read_files(
	AxisDbResult *result,
	guint         max,
	gint64       *next
){
	GPtrArray *files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_browse_file_free);
	guint n = axis_db_result_get_n_rows(result);
	guint i;

	*next = 0;
	for (i = 0; i < n && i < max; i++)
		g_ptr_array_add(files, read_file(result, i));
	if (n > max && files->len > 0)
		*next = ((AxisBrowseFile *)g_ptr_array_index(files, files->len - 1))->id;
	return files;
}

/* ── A folder ───────────────────────────────────────────────────── */

#define DIR_COLUMNS AXIS_LIBRARY_DIR_COLUMNS

static AxisDirRecord *
read_dir(
	AxisDbResult *result,
	guint         row
){
	AxisDirRecord *dir = g_new0(AxisDirRecord, 1);

	dir->id = int_at(result, row, 0, 0);
	dir->root_id = int_at(result, row, 1, 0);
	dir->parent_id = int_at(result, row, 2, 0);
	dir->path = text_at(result, row, 3);
	dir->name = text_at(result, row, 4);
	dir->direct_files = (guint64)MAX(int_at(result, row, 5, 0), 0);
	dir->direct_bytes = (guint64)MAX(int_at(result, row, 6, 0), 0);
	dir->total_files = (guint64)MAX(int_at(result, row, 7, 0), 0);
	dir->total_bytes = (guint64)MAX(int_at(result, row, 8, 0), 0);
	dir->subdirs = (guint64)MAX(int_at(result, row, 9, 0), 0);
	return dir;
}

gboolean
axis_library_get_dir(
	AxisLibrary     *self,
	gint64           dir_id,
	AxisDirRecord  **dir,
	GError         **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_library_int_text(dir_id);
	const gchar *params[1];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(dir != NULL, FALSE);

	*dir = NULL;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT " DIR_COLUMNS " FROM library_dir WHERE id = $1::bigint", 1,
	                       params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND,
			"there is no folder %" G_GINT64_FORMAT, dir_id);
		return FALSE;
	}
	*dir = read_dir(result, 0);
	return TRUE;
}

gboolean
axis_library_find_root_dir(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *dir_id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_library_int_text(root_id);
	const gchar *params[1];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(dir_id != NULL, FALSE);

	*dir_id = 0;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool,
	                       "SELECT id FROM library_dir WHERE root_id = $1::bigint"
	                       " AND path_hash = library_path_hash('')",
	                       1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1)
		*dir_id = int_at(result, 0, 0, 0);
	return TRUE;
}

static gchar *
like_pattern(const gchar *text);

gboolean
axis_library_get_folder(
	AxisLibrary      *self,
	gint64            dir_id,
	gint64            after_dir,
	gint64            after_file,
	AxisFolderView  **view,
	GError          **error
){
	return axis_library_get_folder_named(self, dir_id, NULL, after_dir, after_file, view, error);
}

gboolean
axis_library_get_folder_named(
	AxisLibrary      *self,
	gint64            dir_id,
	const gchar      *name,
	gint64            after_dir,
	gint64            after_file,
	AxisFolderView  **view,
	GError          **error
){
	g_autoptr(AxisFolderView) out = g_new0(AxisFolderView, 1);
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = axis_library_int_text(dir_id);
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_LIBRARY_PAGE_SIZE + 1);
	g_autofree gchar *key_name = NULL;
	g_autofree gchar *key_id = NULL;
	g_autofree gchar *pattern = NULL;
	const gchar *params[5];
	AxisDbConn *conn;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(view != NULL, FALSE);
	g_return_val_if_fail(name == NULL || (strlen(name) <= AXIS_LIBRARY_FOLDER_NAME_MAX &&
		g_utf8_validate(name, -1, NULL)), FALSE);

	*view = NULL;
	if (name != NULL && name[0] != '\0')
		pattern = like_pattern(name);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, "SELECT " DIR_COLUMNS " FROM library_dir WHERE id = $1::bigint",
	                1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND,
			"there is no folder %" G_GINT64_FORMAT, dir_id);
		goto fail;
	}
	out->dir = read_dir(result, 0);
	g_clear_pointer(&result, axis_db_result_free);

	/* From the top to here. */
	if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
	                "WITH RECURSIVE up(id, root_id, parent_id, path, name, direct_files, direct_bytes,"
	                "   total_files, total_bytes, subdirs, depth) AS ("
	                " SELECT " DIR_COLUMNS ", 0 FROM library_dir WHERE id = $1::bigint"
	                " UNION ALL"
	                " SELECT d.id, d.root_id, coalesce(d.parent_id, 0), d.path, d.name, d.direct_files,"
	                "   d.direct_bytes, d.total_files, d.total_bytes, d.subdirs, up.depth + 1"
	                " FROM library_dir d JOIN up ON d.id = up.parent_id WHERE up.depth < 130)"
	                " SELECT " DIR_COLUMNS " FROM up ORDER BY depth DESC",
	                1, params, &result, error))
		goto fail;
	out->crumbs = g_ptr_array_new_with_free_func((GDestroyNotify)axis_dir_record_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(out->crumbs, read_dir(result, i));
	g_clear_pointer(&result, axis_db_result_free);

	/* Subfolders after the cursor, in name order. */
	if (after_dir > 0) {
		key_id = axis_library_int_text(after_dir);
		params[0] = key_id;
		params[1] = id;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
		                "SELECT name FROM library_dir WHERE id = $1::bigint AND parent_id = $2::bigint",
		                2, params, &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1)
			key_name = text_at(result, 0, 0);
		else
			out->dir_cursor_gone = TRUE;
		g_clear_pointer(&result, axis_db_result_free);
	}
	params[0] = id;
	params[1] = limit;
	if (key_name != NULL) {
		params[2] = key_name;
		params[3] = key_id;
		params[4] = pattern;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, pattern != NULL ? AXIS_LIBRARY_SQL_DIRS_NAMED_AFTER :
		                AXIS_LIBRARY_SQL_DIRS_AFTER, pattern != NULL ? 5 : 4, params, &result, error))
			goto fail;
	} else {
		params[2] = pattern;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, pattern != NULL ? AXIS_LIBRARY_SQL_DIRS_NAMED_FIRST :
		                AXIS_LIBRARY_SQL_DIRS_FIRST, pattern != NULL ? 3 : 2, params, &result, error))
			goto fail;
	}
	out->subdirs =g_ptr_array_new_with_free_func((GDestroyNotify)axis_dir_record_free);
	for (i = 0; i < axis_db_result_get_n_rows(result) && i < AXIS_LIBRARY_PAGE_SIZE; i++)
		g_ptr_array_add(out->subdirs, read_dir(result, i));
	if (axis_db_result_get_n_rows(result) > AXIS_LIBRARY_PAGE_SIZE)
		out->next_dir = ((AxisDirRecord *)g_ptr_array_index(out->subdirs, out->subdirs->len - 1))->id;
	g_clear_pointer(&result, axis_db_result_free);

	/* Files directly in it, after the cursor, in name order. */
	g_clear_pointer(&key_name, g_free);
	g_clear_pointer(&key_id, g_free);
	if (after_file > 0) {
		key_id = axis_library_int_text(after_file);
		params[0] = key_id;
		params[1] = id;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
		                "SELECT f.base_name FROM library_file f JOIN library_dir d ON d.id = $2::bigint"
		                " WHERE f.id = $1::bigint AND f.root_id = d.root_id AND f.dir_hash = d.path_hash",
		                2, params, &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1)
			key_name = text_at(result, 0, 0);
		else
			out->file_cursor_gone = TRUE;
		g_clear_pointer(&result, axis_db_result_free);
	}
	params[0] = id;
	params[1] = limit;
	if (key_name != NULL) {
		params[2] = key_name;
		params[3] = key_id;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, AXIS_LIBRARY_SQL_FILES_AFTER, 4, params, &result, error))
			goto fail;
	} else {
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, AXIS_LIBRARY_SQL_FILES_FIRST, 2, params, &result, error))
			goto fail;
	}
	out->files = read_files(result, AXIS_LIBRARY_PAGE_SIZE, &out->next_file);
	g_clear_pointer(&result, axis_db_result_free);

	/* How fresh the totals are. */
	{
		g_autofree gchar *root = axis_library_int_text(out->dir->root_id);

		params[0] = root;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
		                "SELECT s.last_complete_scan_id, (extract(epoch FROM c.finished_at) * 1000000)::bigint"
		                " FROM library_root_state s JOIN library_scan c ON c.id = s.last_complete_scan_id"
		                " WHERE s.root_id = $1::bigint",
		                1, params, &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1) {
			out->scan_id = int_at(result, 0, 0, 0);
			out->scan_us = int_at(result, 0, 1, 0);
		}
	}

	axis_db_pool_release(self->pool, conn);
	*view = g_steal_pointer(&out);
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

/* ── The flat list ──────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *values;	/* gchar *, owned */
} Params;

/* Adds @value (owned) and returns its number for "$n". */
static guint
param_add(
	Params *params,
	gchar  *value
){
	g_ptr_array_add(params->values, value);
	return params->values->len;
}

static void
where_text(
	GString     *where,
	Params      *params,
	const gchar *column,
	const gchar *type,
	const gchar *value
){
	guint n = param_add(params, g_strdup(value));

	g_string_append_printf(where, " AND %s = $%u::%s", column, n, type);
}

/* A LIKE pattern that finds @text anywhere, with \ % and _ taken literally. */
static gchar *
like_pattern(const gchar *text)
{
	GString *pattern = g_string_new("%");
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if (*p == '\\' || *p == '%' || *p == '_')
			g_string_append_c(pattern, '\\');
		g_string_append_c(pattern, *p);
	}
	g_string_append_c(pattern, '%');
	return g_string_free(pattern, FALSE);
}

/* The sort's column, its SQL type, and the statement that reads a row's
 * key (the row's id is $1). */
typedef struct
{
	const gchar *column;
	const gchar *type;
	const gchar *key_sql;
	const gchar *not_null;
} SortSpec;

static const SortSpec sort_specs[AXIS_LIBRARY_SORT_COUNT] = {
	{ "f.base_name", "text COLLATE \"C\"",
	  "SELECT base_name FROM library_file WHERE id = $1::bigint", NULL },
	{ "f.size", "bigint",
	  "SELECT size::text FROM library_file WHERE id = $1::bigint", NULL },
	{ "f.mtime_sec", "bigint",
	  "SELECT mtime_sec::text FROM library_file WHERE id = $1::bigint", NULL },
	{ "m.duration_ms", "bigint",
	  "SELECT duration_ms::text FROM media_info WHERE file_id = $1::bigint", "m.duration_ms IS NOT NULL" },
	{ "m.bitrate", "bigint",
	  "SELECT bitrate::text FROM media_info WHERE file_id = $1::bigint", "m.bitrate IS NOT NULL" }
};

/* The conditions every statement of a list shares (not the cursor).  The
 * estimator's (target, outcome) are not here: the estimator evaluates
 * them (axis-estimator.h).  @with_root joins library_root r whatever the
 * filter. */
static void
build_where(
	const AxisLibraryFilter *filter,
	GString                 *where,
	GString                 *joins,
	Params                  *params,
	gboolean                 with_root
){
	const SortSpec *spec = &sort_specs[filter->sort];
	guint n;

	g_string_append(where, " WHERE true");
	where_text(where, params, "f.state", "text", filter->state);
	if (filter->root_id > 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->root_id));
		g_string_append_printf(where, " AND f.root_id = $%u::bigint", n);
	}
	if (filter->kind != NULL)
		where_text(where, params, "f.kind", "text", filter->kind);
	if (filter->pinned >= 0)
		where_text(where, params, "f.pinned", "boolean", filter->pinned == 1 ? "true" : "false");
	if (filter->container != NULL)
		where_text(where, params, "m.container", "text", filter->container);
	if (filter->video_codec != NULL)
		where_text(where, params, "m.video_codec", "text", filter->video_codec);
	if (filter->resolution != NULL)
		where_text(where, params, "m.resolution_class", "text", filter->resolution);
	if (filter->audio_codec != NULL)
		where_text(where, params, "m.audio_codec", "text", filter->audio_codec);
	if (filter->hdr != NULL) {
		if (strcmp(filter->hdr, "uncertain") == 0)
			g_string_append(where, " AND m.hdr_uncertain");
		else
			where_text(where, params, "m.hdr_format", "text", filter->hdr);
	}
	if (filter->probe != NULL) {
		if (strcmp(filter->probe, "none") == 0)
			g_string_append(where, " AND m.file_id IS NULL AND f.kind IN ('video', 'audio')");
		else
			where_text(where, params, "m.status", "text", filter->probe);
	}
	if (filter->min_mib >= 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->min_mib * 1048576));
		g_string_append_printf(where, " AND f.size >= $%u::bigint", n);
	}
	if (filter->max_mib >= 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->max_mib * 1048576));
		g_string_append_printf(where, " AND f.size <= $%u::bigint", n);
	}
	if (filter->name != NULL) {
		n = param_add(params, like_pattern(filter->name));
		g_string_append_printf(where, " AND f.base_name ILIKE $%u::text", n);
	}
	if (filter->changed >= 0)
		where_text(where, params, "f.changed", "boolean", filter->changed == 1 ? "true" : "false");
	if (filter->volume_id > 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->volume_id));
		g_string_append_printf(where, " AND f.volume_id = $%u::bigint", n);
	}
	if (filter->type != NULL || filter->leftover != NULL || with_root)
		g_string_append(joins, " JOIN library_root r ON r.id = f.root_id");
	if (filter->type != NULL)
		where_text(where, params, "r.media_type", "text", filter->type);
	if (filter->leftover != NULL) {
		const gchar *condition = axis_leftover_condition(filter->leftover);

		/* The parser accepted the name, so there is a condition. */
		g_string_append_printf(where, " AND %s", condition != NULL ? condition : "false");
	}
	if (filter->dir_id > 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->dir_id));
		g_string_append_printf(joins, " JOIN library_dir d ON d.id = $%u::bigint AND d.root_id = f.root_id", n);
		g_string_append(where,
			" AND (d.path = '' OR (starts_with(f.rel_path, d.path || '/')"
			" AND left(f.rel_path, 600) COLLATE \"C\" >= left(d.path || '/', 600)"
			" AND (length(d.path) >= 599 OR left(f.rel_path, 600) COLLATE \"C\" < d.path || '0')))");
	}
	if (filter->n_ids > 0) {
		GString *list = g_string_new("{");
		guint i;

		for (i = 0; i < filter->n_ids; i++)
			g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", filter->ids[i]);
		g_string_append_c(list, '}');
		n = param_add(params, g_string_free(list, FALSE));
		g_string_append_printf(where, " AND f.id = ANY($%u::bigint[])", n);
	}
	if (filter->min_bpp >= 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->min_bpp));
		g_string_append_printf(where, " AND m.bits_per_pixel * 1000 >= $%u::float8", n);
	}
	if (filter->max_bpp >= 0) {
		n = param_add(params, g_strdup_printf("%" G_GINT64_FORMAT, filter->max_bpp));
		g_string_append_printf(where, " AND m.bits_per_pixel * 1000 < $%u::float8", n);
	}
	if (spec->not_null != NULL)
		g_string_append_printf(where, " AND %s", spec->not_null);
}

const gchar *
axis_library_browse_key_sql(AxisLibrarySort sort)
{
	return sort_specs[sort].key_sql;
}

gchar *
axis_library_browse_list_sql(
	const AxisLibraryFilter  *filter,
	const gchar              *key,
	const gchar              *after,
	GPtrArray                *values,
	guint                    *n_base,
	gchar                   **count_sql
){
	g_autoptr(GString) where = g_string_new(NULL);
	g_autoptr(GString) joins = g_string_new(NULL);
	GString *sql = g_string_new(NULL);
	const SortSpec *spec = &sort_specs[filter->sort];
	const gchar *direction = filter->descending ? "DESC" : "ASC";
	const gchar *op = filter->descending ? "<" : ">";
	Params params;
	guint n;

	params.values = values;
	build_where(filter, where, joins, &params, FALSE);
	*n_base = params.values->len;

	*count_sql = g_strdup_printf("SELECT count(*) FROM (SELECT 1 FROM library_file f"
		" LEFT JOIN media_info m ON m.file_id = f.id%s%s LIMIT %d) c", joins->str, where->str,
		AXIS_LIBRARY_COUNT_CAP + 1);

	g_string_append(sql, "SELECT " FILE_COLUMNS " FROM library_file f LEFT JOIN media_info m ON m.file_id = f.id");
	g_string_append(sql, joins->str);
	g_string_append(sql, where->str);
	if (key != NULL) {
		n = param_add(&params, g_strdup(key));
		g_string_append_printf(sql, " AND (%s, f.id) %s ($%u::%s, $%u::bigint)", spec->column, op, n,
			spec->type, n + 1);
		param_add(&params, g_strdup(after));
	}
	g_string_append_printf(sql, " ORDER BY %s %s, f.id %s LIMIT %d", spec->column, direction, direction,
		AXIS_LIBRARY_PAGE_SIZE + 1);

	return g_string_free(sql, FALSE);
}

gchar *
axis_library_browse_scan_sql(
	const AxisLibraryFilter  *filter,
	const gchar              *extra,
	const gchar              *key,
	const gchar              *after,
	guint                     limit,
	GPtrArray                *values
){
	g_autoptr(GString) where = g_string_new(NULL);
	g_autoptr(GString) joins = g_string_new(NULL);
	GString *sql = g_string_new(NULL);
	const SortSpec *spec = &sort_specs[filter->sort];
	const gchar *direction = filter->descending ? "DESC" : "ASC";
	const gchar *op = filter->descending ? "<" : ">";
	Params params;
	guint n;

	params.values = values;
	build_where(filter, where, joins, &params, TRUE);
	g_string_append_printf(sql, "SELECT f.id, (%s)::text FROM library_file f LEFT JOIN media_info m ON m.file_id = f.id",
		spec->column);
	g_string_append(sql, joins->str);
	g_string_append(sql, where->str);
	if (extra != NULL)
		g_string_append_printf(sql, " AND (%s)", extra);
	if (key != NULL) {
		n = param_add(&params, g_strdup(key));
		g_string_append_printf(sql, " AND (%s, f.id) %s ($%u::%s, $%u::bigint)", spec->column, op, n,
			spec->type, n + 1);
		param_add(&params, g_strdup(after));
	}
	g_string_append_printf(sql, " ORDER BY %s %s, f.id %s LIMIT %u", spec->column, direction, direction, limit);
	return g_string_free(sql, FALSE);
}

gboolean
axis_library_browse_read_files(
	AxisDbConn    *conn,
	const gint64  *ids,
	guint          n_ids,
	GPtrArray    **files,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GString) list = g_string_new("{");
	GPtrArray *out;
	const gchar *params[1];
	guint i;

	*files = NULL;
	for (i = 0; i < n_ids; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", ids[i]);
	g_string_append_c(list, '}');
	params[0] = list->str;
	if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT,
	                "SELECT " FILE_COLUMNS " FROM library_file f LEFT JOIN media_info m ON m.file_id = f.id"
	                " WHERE f.id = ANY($1::bigint[])",
	                1, params, &result, error))
		return FALSE;

	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_browse_file_free);
	{
		g_autoptr(GPtrArray) read = g_ptr_array_new_with_free_func((GDestroyNotify)axis_browse_file_free);

		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			AxisBrowseFile *file = read_file(result, i);

			g_ptr_array_add(read, file);
			g_hash_table_insert(by_id, &file->id, file);
		}
		/* In the order asked for; a row gone meanwhile is left out. */
		for (i = 0; i < n_ids; i++) {
			AxisBrowseFile *file = g_hash_table_lookup(by_id, &ids[i]);
			guint at;

			if (file == NULL || !g_ptr_array_find(read, file, &at))
				continue;
			g_ptr_array_add(out, g_ptr_array_steal_index(read, at));
		}
	}
	*files = out;
	return TRUE;
}

gboolean
axis_library_list_files(
	AxisLibrary              *self,
	const AxisLibraryFilter  *filter,
	AxisFileList            **list,
	GError                  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *count_sql = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *after = NULL;
	AxisFileList *out = g_new0(AxisFileList, 1);
	Params params;
	AxisDbConn *conn;
	guint n_base = 0;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(filter != NULL, FALSE);
	g_return_val_if_fail(list != NULL, FALSE);

	*list = NULL;
	/* Never a list that silently leaves out a condition it was asked for. */
	if (axis_library_filter_needs_estimator(filter)) {
		g_set_error_literal(error, AXIS_LIBRARY_FILTER_ERROR, AXIS_LIBRARY_FILTER_ERROR_INVALID,
			"This list needs the estimator.");
		g_free(out);
		return FALSE;
	}
	params.values = g_ptr_array_new_with_free_func(g_free);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		g_ptr_array_unref(params.values);
		g_free(out);
		return FALSE;
	}

	/* The row the page starts after: its key comes from the database. */
	if (filter->after > 0) {
		const gchar *id_params[1];

		after = axis_library_int_text(filter->after);
		id_params[0] = after;
		if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, axis_library_browse_key_sql(filter->sort), 1, id_params,
		                &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1 && !axis_db_result_is_null(result, 0, 0))
			key = text_at(result, 0, 0);
		else
			out->cursor_gone = TRUE;
		g_clear_pointer(&result, axis_db_result_free);
	}

	sql = axis_library_browse_list_sql(filter, key, after, params.values, &n_base, &count_sql);

	if (!timed_exec(conn, AXIS_LIBRARY_LIST_TIMEOUT, sql, params.values->len,
	                (const gchar *const *)params.values->pdata, &result, error))
		goto fail;
	out->files = read_files(result, AXIS_LIBRARY_PAGE_SIZE, &out->next);
	g_clear_pointer(&result, axis_db_result_free);

	{
		GError *local = NULL;

		if (timed_exec(conn, AXIS_LIBRARY_COUNT_TIMEOUT, count_sql, n_base,
		               (const gchar *const *)params.values->pdata, &result, &local)) {
			gint64 count = int_at(result, 0, 0, 0);

			out->count_capped = count > AXIS_LIBRARY_COUNT_CAP;
			out->count = (guint64)MIN(count, (gint64)AXIS_LIBRARY_COUNT_CAP);
		} else if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT)) {
			out->count_unknown = TRUE;
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
			goto fail;
		}
	}

	axis_db_pool_release(self->pool, conn);
	g_ptr_array_unref(params.values);
	*list = out;
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	g_ptr_array_unref(params.values);
	axis_file_list_free(out);
	return FALSE;
}

gboolean
axis_library_latest_scan(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *scan_id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = axis_library_int_text(root_id);
	const gchar *params[1];

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(scan_id != NULL, FALSE);

	*scan_id = 0;
	params[0] = root;
	if (!axis_db_pool_exec(self->pool,
	                       "SELECT coalesce(max(last_complete_scan_id), 0) FROM library_root_state"
	                       " WHERE $1::bigint = 0 OR root_id = $1::bigint",
	                       1, params, &result, error))
		return FALSE;
	*scan_id = int_at(result, 0, 0, 0);
	return TRUE;
}
