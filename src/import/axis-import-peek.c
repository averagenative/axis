/*
 * axis-import-peek.c - what the manual import page shows before anything is
 * decided: the sources a person can pick, and what Axis reads from one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Read only, through read guards.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "import/axis-import-private.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/axis-storage-path.h"
#include "storage/axis-volume-guard.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* Entries listed per downloads root. */
#define ROOT_ENTRIES 200

void
axis_import_source_free(AxisImportSource *source)
{
	if (source == NULL)
		return;
	g_free(source->path);
	g_free(source->label);
	g_free(source->where);
	g_free(source);
}

void
axis_import_peek_free(AxisImportPeek *peek)
{
	if (peek == NULL)
		return;
	g_free(peek->path);
	g_free(peek->kind);
	g_free(peek->note);
	g_free(peek);
}

static gint
compare_sources(
	gconstpointer a,
	gconstpointer b
){
	const AxisImportSource *x = *(const AxisImportSource *const *)a;
	const AxisImportSource *y = *(const AxisImportSource *const *)b;

	return g_utf8_collate(x->label, y->label);
}

static void
list_root(
	AxisImport         *self,
	const AxisRootInfo *root,
	GPtrArray          *sources
){
	g_autoptr(AxisVolumeGuard) guard = axis_volume_guard_acquire(self->storage, root->volume_id,
		AXIS_VOLUME_INTENT_READ, NULL);
	g_autoptr(AxisVolumeInfo) volume = axis_storage_get_volume(self->storage, root->volume_id);
	g_autoptr(GPtrArray) found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_source_free);
	struct dirent *entry;
	DIR *dir;
	gint fd;

	if (guard == NULL)
		return;
	fd = axis_volume_guard_open_dir(guard, root->rel_path, NULL);
	if (fd < 0)
		return;
	dir = fdopendir(fd);
	if (dir == NULL) {
		close(fd);
		return;
	}
	while ((entry = readdir(dir)) != NULL && found->len < ROOT_ENTRIES) {
		g_autofree gchar *normal = NULL;
		struct stat st;
		AxisImportSource *source;

		if (entry->d_name[0] == '.')
			continue;
		/* Only names a path can carry (valid UTF-8, no control characters). */
		if (!axis_storage_path_normalize(entry->d_name, FALSE, &normal, NULL) || strcmp(normal, entry->d_name) != 0)
			continue;
		if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (!S_ISDIR(st.st_mode) && !(S_ISREG(st.st_mode) &&
		    axis_import_file_kind(entry->d_name, strlen(entry->d_name)) == AXIS_IMPORT_FILE_VIDEO))
			continue;
		source = g_new0(AxisImportSource, 1);
		source->root_id = root->id;
		source->path = g_strdup(normal);
		source->label = g_strdup(normal);
		source->where = g_strdup_printf("%s: %s%s%s", volume != NULL ? volume->name : "a disk", root->rel_path,
			root->rel_path[0] != '\0' ? "/" : "", normal);
		g_ptr_array_add(found, source);
	}
	closedir(dir);
	g_ptr_array_sort(found, compare_sources);
	while (found->len > 0)
		g_ptr_array_add(sources, g_ptr_array_steal_index(found, 0));
}

gboolean
axis_import_list_sources(
	AxisImport  *self,
	GPtrArray  **sources_out,
	GError     **error
){
	g_autoptr(GPtrArray) sources = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_source_free);
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	guint i;

	*sources_out = NULL;
	if (!axis_db_pool_exec(self->pool, "SELECT d.id, d.rel_path, coalesce(d.title_label, d.release_name), v.name, "
	        US("coalesce(d.completed_at, d.updated_at)") " FROM download d JOIN volume v ON v.id = d.volume_id"
	        /* Never a YouTube download: its file is in the library already,
	         * put there by its own kind, and is nothing to import. */
	        " WHERE d.state = 'completed' AND d.rel_path IS NOT NULL AND d.protocol <> 'youtube'"
	        " AND NOT EXISTS (SELECT 1 FROM import i WHERE i.download_id = d.id AND i.state IN ('waiting', 'running',"
	        "  'blocked', 'imported')) ORDER BY d.completed_at DESC NULLS LAST LIMIT 200", 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportSource *source = g_new0(AxisImportSource, 1);

		axis_db_result_get_int64(rows, i, 0, &source->download_id);
		source->path = g_strdup(axis_db_result_get_text(rows, i, 1));
		source->label = g_strdup(axis_db_result_get_text(rows, i, 2));
		source->where = g_strdup_printf("%s: %s", axis_db_result_get_text(rows, i, 3), source->path);
		axis_db_result_get_int64(rows, i, 4, &source->when_us);
		g_ptr_array_add(sources, source);
	}
	if (!axis_storage_list_roots(self->storage, 0, &roots, error))
		return FALSE;
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);

		if (root->media_type == AXIS_MEDIA_DOWNLOADS)
			list_root(self, root, sources);
	}
	*sources_out = g_steal_pointer(&sources);
	return TRUE;
}

gboolean
axis_import_peek(
	AxisImport   *self,
	gint64        download_id,
	gint64        root_id,
	const gchar  *path,
	GPtrArray   **files_out,
	gchar       **problem,
	GError      **error
){
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_peek_free);
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autofree gchar *source_path = NULL;
	gint64 volume_id = 0;
	gboolean is_file = FALSE;
	GError *local = NULL;
	guint i;

	*files_out = NULL;
	*problem = NULL;
	if (download_id > 0) {
		g_autoptr(AxisDbResult) row = NULL;
		g_autofree gchar *id = axis_import_int_text(download_id);
		const gchar *params[1];

		params[0] = id;
		if (!axis_db_pool_exec(self->pool, "SELECT volume_id, rel_path FROM download WHERE id = $1::bigint"
		        " AND volume_id IS NOT NULL AND rel_path IS NOT NULL", 1, params, &row, error))
			return FALSE;
		if (axis_db_result_get_n_rows(row) == 0) {
			*problem = g_strdup("That download has no known place on a volume.");
			return TRUE;
		}
		axis_db_result_get_int64(row, 0, 0, &volume_id);
		source_path = g_strdup(axis_db_result_get_text(row, 0, 1));
	} else {
		g_autoptr(AxisRootInfo) root = NULL;
		g_autofree gchar *normal = NULL;

		if (!axis_storage_get_root(self->storage, root_id, &root, NULL) || root->media_type != AXIS_MEDIA_DOWNLOADS) {
			*problem = g_strdup("That is not a downloads folder.");
			return TRUE;
		}
		if (!axis_storage_path_normalize(path, FALSE, &normal, NULL) || normal[0] == '\0') {
			*problem = g_strdup("That is not a folder inside that downloads folder.");
			return TRUE;
		}
		volume_id = root->volume_id;
		source_path = root->rel_path[0] != '\0' ? g_strconcat(root->rel_path, "/", normal, NULL) : g_strdup(normal);
	}
	guard = axis_volume_guard_acquire(self->storage, volume_id, AXIS_VOLUME_INTENT_READ, &local);
	if (guard == NULL) {
		*problem = g_strdup_printf("Its disk cannot be read now: %s", local->message);
		g_error_free(local);
		return TRUE;
	}
	if (!axis_import_walk(guard, source_path, &found, &is_file, problem, error))
		return FALSE;
	if (*problem != NULL)
		return TRUE;
	if (!axis_import_probe_found(self->probe, guard, source_path, found, &local)) {
		/* Without ffprobe the files are still listed. */
		g_clear_error(&local);
	}
	for (i = 0; i < found->len; i++) {
		const AxisImportFound *f = g_ptr_array_index(found, i);
		AxisImportPeek *peek = g_new0(AxisImportPeek, 1);

		peek->position = f->position;
		peek->path = g_strdup(f->path[0] != '\0' ? f->path : f->name);
		peek->kind = g_strdup(f->type == AXIS_IMPORT_FOUND_LINK ? "link" : f->type == AXIS_IMPORT_FOUND_FOLDER ?
			"folder" : f->type == AXIS_IMPORT_FOUND_SPECIAL ? "special" : f->kind == AXIS_IMPORT_FILE_VIDEO ? "video" :
			f->kind == AXIS_IMPORT_FILE_SUBTITLE ? "subtitle" : f->kind == AXIS_IMPORT_FILE_ARCHIVE ? "archive" :
			"other");
		peek->size = (gint64)f->size;
		peek->duration_ms = f->probe_ok ? f->duration_ms : 0;
		peek->width = f->width;
		peek->height = f->height;
		peek->note = g_strdup(f->type == AXIS_IMPORT_FOUND_FOLDER ? f->folder_note :
			f->type == AXIS_IMPORT_FOUND_LINK ? "a symbolic link: not followed" : f->probe_text);
		g_ptr_array_add(files, peek);
	}
	*files_out = g_steal_pointer(&files);
	return TRUE;
}
