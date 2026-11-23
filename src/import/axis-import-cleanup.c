/*
 * axis-import-cleanup.c - the recycle folders after their retention,
 * temporary files a job left behind, and downloads still to be removed from
 * their clients (Usenet always, a torrent unless import.torrents_after is
 * "seed"; one its client says it no longer has is "gone", not retried), then
 * the folders SABnzbd keeps of the Usenet jobs it removed and the files of
 * torrents already gone from their clients (axis-import-folder.c); the
 * import.cleanup job, hourly
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Only what import_recycle records is ever removed from a recycle folder:
 * the file at its recorded place, when it is the regular file recorded
 * (inode and size), reached through a write guard of its volume without a
 * link or another filesystem.  Something else there is left alone and noted.
 * Nothing is removed while storage is read-only (the guard refuses), and a
 * volume that is offline is simply looked at again next time.
 *
 * A temporary name is removed only for an import that no job is placing
 * (blocked, dismissed or imported), only when it still names the file
 * recorded, and only when another name of that file exists or it is a copy
 * whose source is still there; otherwise it is kept and noted.  The temporary
 * name of a file moved out of a download goes only beside a name of its own
 * that leads to it, looked at then (its final name, or the download's): a
 * link count would count a hardlink outside too (a cross-seed, a backup), and
 * the temporary name would be its one name in the library and the download.
 * Its row is never cleared while its folder cannot be opened: the import
 * looks again; nor is a copy's or a link's that may be alone there (a copy
 * made, a link whose download's name no longer leads to its file).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "import/axis-import-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-write.h"

#define BATCH 500

/* A recycle row with no import that is still 'moving' after this long is not
 * a removal under way (one moves a file and records it at once): its record
 * was cut short (axis_import_recover_duplicates()). */
#define DUPLICATE_RECOVER_AFTER_S 300

/* ── Recycled files ─────────────────────────────────────────────── */

static gboolean
remove_due(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) guards = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_volume_guard_release);
	g_autoptr(GHashTable) away = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	guint i;

	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT id, volume_id, recycle_path, inode, size FROM import_recycle"
	        " WHERE state = 'kept' AND remove_after IS NOT NULL AND remove_after <= now()"
	        " AND ($1::bigint = 0 OR import_id = $1::bigint) ORDER BY remove_after LIMIT 500", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(GString) bytes = NULL;
		g_auto(GStrv) parts = NULL;
		g_autofree gchar *row_text = NULL;
		const gchar *state;
		const gchar *note = NULL;
		const gchar *more[3];
		AxisVolumeRecycledOutcome outcome;
		AxisVolumeGuard *guard;
		gint64 row = 0;
		gint64 volume = 0;
		gint64 inode = 0;
		gint64 size = 0;
		GError *local = NULL;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 1, &volume);
		axis_db_result_get_int64(rows, i, 3, &inode);
		axis_db_result_get_int64(rows, i, 4, &size);
		if (g_hash_table_contains(away, &volume))
			continue;
		guard = g_hash_table_lookup(guards, &volume);
		if (guard == NULL) {
			gint64 *key = g_new(gint64, 1);

			*key = volume;
			guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_WRITE, &local);
			if (guard == NULL) {
				/* Offline, read-only, or storage read-only: next time. */
				g_hash_table_add(away, key);
				g_clear_error(&local);
				continue;
			}
			g_hash_table_insert(guards, key, guard);
		}
		if (axis_db_result_get_text(rows, i, 2) == NULL ||
		    !axis_library_name_decode(axis_db_result_get_text(rows, i, 2), &bytes) ||
		    memchr(bytes->str, '\0', bytes->len) != NULL)
			continue;
		parts = g_strsplit(bytes->str, "/", 70);
		if (!axis_volume_write_remove_recycled(guard, (const gchar *const *)parts, (guint64)inode, (guint64)size,
		        &outcome, &local)) {
			if (local->domain == AXIS_STORAGE_ERROR && (local->code == AXIS_STORAGE_ERROR_UNAVAILABLE ||
			    local->code == AXIS_STORAGE_ERROR_READ_ONLY)) {
				gint64 *key = g_new(gint64, 1);

				*key = volume;
				g_hash_table_add(away, key);
				g_error_free(local);
				continue;
			}
			g_warning("import cleanup: a recycled file could not be removed: %s", local->message);
			g_error_free(local);
			continue;
		}
		switch (outcome) {
		case AXIS_VOLUME_RECYCLED_REMOVED:
			state = "removed";
			if (report != NULL)
				report->removed++;
			break;
		case AXIS_VOLUME_RECYCLED_GONE:
			state = "gone";
			note = "It was no longer in the recycle folder when its time came.";
			if (report != NULL)
				report->gone++;
			break;
		default:
			state = "changed";
			note = "Something else is at its place in the recycle folder now; it was left alone.";
			if (report != NULL)
				report->changed++;
			break;
		}
		row_text = axis_import_int_text(row);
		more[0] = row_text;
		more[1] = state;
		more[2] = note;
		if (!axis_db_pool_exec(self->pool, "UPDATE import_recycle SET state = $2, removed_at = now(), note = $3"
		        " WHERE id = $1::bigint AND state = 'kept'", 3, more, NULL, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_import_cleanup_due(
	AxisImport  *self,
	gint64       import_id,
	GError     **error
){
	return remove_due(self, import_id, NULL, error);
}

/* ── Temporary files left behind ────────────────────────────────── */

/*
 * What a look on disk tells: yes, no (proven: absent, or another file), or
 * nothing now (a volume, a folder or a file that cannot be looked at for
 * another reason than its absence).  Only a proven answer is acted on.
 */
typedef enum
{
	LOOK_YES,
	LOOK_NO,
	LOOK_UNKNOWN
} Look;

/* Row @i's file's folder in the download and its name there (of a query
 * with leftovers()'s columns), or %FALSE when the row does not say them. */
static gboolean
download_parts(
	AxisDbResult  *rows,
	guint          i,
	gchar        **parent,
	gchar        **name
){
	g_autoptr(GString) bytes = NULL;
	const gchar *source_path = axis_db_result_get_text(rows, i, 11);
	const gchar *path = axis_db_result_get_text(rows, i, 12);

	*parent = NULL;
	*name = NULL;
	if (source_path == NULL || path == NULL || !axis_library_name_decode(path, &bytes) ||
	    memchr(bytes->str, '\0', bytes->len) != NULL)
		return FALSE;
	if (bytes->len == 0) {
		const gchar *last = strrchr(source_path, '/');

		*parent = last != NULL ? g_strndup(source_path, (gsize)(last - source_path)) : g_strdup("");
		*name = g_strdup(last != NULL ? last + 1 : source_path);
	} else {
		const gchar *slash = strrchr(bytes->str, '/');
		g_autofree gchar *inside = slash != NULL ? g_strndup(bytes->str, (gsize)(slash - bytes->str)) : NULL;

		*parent = inside != NULL ? g_strconcat(source_path, "/", inside, NULL) : g_strdup(source_path);
		*name = g_strdup(slash != NULL ? slash + 1 : bytes->str);
	}
	return TRUE;
}

/* @parent opened through @guard: the descriptor, or -1 with *@missing when it
 * is not there (and %FALSE in *@missing when it cannot be opened for another
 * reason). */
static gint
open_parent(
	AxisVolumeGuard  *guard,
	const gchar      *parent,
	gboolean         *missing
){
	g_autoptr(GError) local = NULL;
	gint dir = axis_volume_guard_open_dir(guard, parent, &local);

	*missing = dir < 0 && g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	return dir;
}

/* The download's folder of row @i's file (of leftovers()'s @rows), opened
 * through @guard (the library's volume: the download must be on it), and the
 * file's name there in *@name, whatever is at that name now; -1 when that
 * folder cannot be reached now, with *@missing when it is not there. */
static gint
download_place(
	AxisVolumeGuard  *guard,
	AxisDbResult     *rows,
	guint             i,
	gchar           **name,
	gboolean         *missing
){
	g_autofree gchar *parent = NULL;
	gint64 source_volume = 0;
	gint64 volume = 0;
	gint src;

	*name = NULL;
	*missing = FALSE;
	axis_db_result_get_int64(rows, i, 10, &source_volume);
	axis_db_result_get_int64(rows, i, 5, &volume);
	if (source_volume != volume || !download_parts(rows, i, &parent, name))
		return -1;
	src = open_parent(guard, parent, missing);
	if (src < 0)
		g_clear_pointer(name, g_free);
	return src;
}

/* Whether row @i's download's name leads, looked at now through @guard (the
 * library's volume: the download must be on it), to @temp_inode: %LOOK_YES
 * with the download's folder in *@src and the name in *@name; %LOOK_NO when
 * proven not (its folder or its name is not there, or another file is). */
static Look
leads_state(
	AxisVolumeGuard  *guard,
	gint64            temp_inode,
	AxisDbResult     *rows,
	guint             i,
	gint             *src,
	gchar           **name
){
	g_autofree gchar *parent = NULL;
	gint64 source_volume = 0;
	gint64 volume = 0;
	gboolean missing = FALSE;
	struct stat st;
	gint dir;

	*src = -1;
	*name = NULL;
	axis_db_result_get_int64(rows, i, 10, &source_volume);
	axis_db_result_get_int64(rows, i, 5, &volume);
	if (source_volume != volume || temp_inode <= 0 || !download_parts(rows, i, &parent, name)) {
		g_clear_pointer(name, g_free);
		return LOOK_UNKNOWN;
	}
	dir = open_parent(guard, parent, &missing);
	if (dir < 0) {
		g_clear_pointer(name, g_free);
		return missing ? LOOK_NO : LOOK_UNKNOWN;
	}
	if (fstatat(dir, *name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gint err = errno;

		close(dir);
		g_clear_pointer(name, g_free);
		return err == ENOENT ? LOOK_NO : LOOK_UNKNOWN;
	}
	if (!S_ISREG(st.st_mode) || (gint64)st.st_ino != temp_inode) {
		close(dir);
		g_clear_pointer(name, g_free);
		return LOOK_NO;
	}
	*src = dir;
	return LOOK_YES;
}

/* The download's folder of row @i's file, and its name there, when that name
 * leads, looked at now, to @temp_inode; else -1. */
static gint
download_leads(
	AxisVolumeGuard  *guard,
	gint64            temp_inode,
	AxisDbResult     *rows,
	guint             i,
	gchar           **name
){
	gint src = -1;

	if (leads_state(guard, temp_inode, rows, i, &src, name) != LOOK_YES)
		return -1;
	return src;
}

/* Whether the download's file a copy of row @i (of leftovers()'s @rows) was
 * made from is still at its place in the download, the very file, untouched
 * since: its inode and size as the walk recorded, and its modification and
 * change times as recorded when it was copied (a file deleted and another
 * made at its name can have its inode number and its size; not its times, as
 * axis_volume_write_remove_moved() holds too), looked at through a read guard
 * of the download's own volume.  %LOOK_YES: the copy's temporary name is not
 * the file's only copy.  %LOOK_NO: proven not (not there, or another file).
 * %LOOK_UNKNOWN: it cannot be told now, or the copy's times were never
 * recorded (its run died during the copy). */
static Look
copy_source_state(
	AxisImport    *self,
	AxisDbResult  *rows,
	guint          i
){
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *name = NULL;
	const gchar *source_path = axis_db_result_get_text(rows, i, 11);
	gint64 source_volume = 0;
	gint64 inode = 0;
	gint64 size = -1;
	gint64 mtime = 0;
	gint64 ctime = 0;
	gboolean missing = FALSE;
	struct stat st;
	gint dir;
	gint err;

	axis_db_result_get_int64(rows, i, 10, &source_volume);
	if (!axis_db_result_is_null(rows, i, 13))
		axis_db_result_get_int64(rows, i, 13, &inode);
	if (!axis_db_result_is_null(rows, i, 14))
		axis_db_result_get_int64(rows, i, 14, &size);
	if (!axis_db_result_is_null(rows, i, 15))
		axis_db_result_get_int64(rows, i, 15, &mtime);
	if (!axis_db_result_is_null(rows, i, 16))
		axis_db_result_get_int64(rows, i, 16, &ctime);
	if (source_volume <= 0 || source_path == NULL || source_path[0] == '\0' || inode <= 0 || size < 0 ||
	    mtime == 0 || ctime == 0 || !download_parts(rows, i, &parent, &name))
		return LOOK_UNKNOWN;
	reader = axis_volume_guard_acquire(self->storage, source_volume, AXIS_VOLUME_INTENT_READ, NULL);
	if (reader == NULL)
		return LOOK_UNKNOWN;
	dir = open_parent(reader, parent, &missing);
	if (dir < 0)
		return missing ? LOOK_NO : LOOK_UNKNOWN;
	err = fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : errno;
	close(dir);
	if (err != 0)
		return err == ENOENT ? LOOK_NO : LOOK_UNKNOWN;
	if (!S_ISREG(st.st_mode) || (gint64)st.st_ino != inode || (gint64)st.st_size != size ||
	    (guint64)st.st_dev != axis_volume_guard_get_dev(reader) ||
	    (gint64)st.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + st.st_mtim.tv_nsec != mtime ||
	    (gint64)st.st_ctim.tv_sec * G_GINT64_CONSTANT(1000000000) + st.st_ctim.tv_nsec != ctime)
		return LOOK_NO;
	return LOOK_YES;
}

/* The temporary name @temp in @dir of a file moved out of a download (row @i
 * of leftovers()'s @rows), removed only beside the download's own name of it,
 * looked at now; %FALSE (kept, with @error) when that name does not lead to
 * it: then the temporary name may be its one name in the library and the
 * download. */
static gboolean
beside_download(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp,
	gint64            temp_inode,
	AxisDbResult     *rows,
	guint             i,
	gboolean         *removed,
	GError          **error
){
	g_autofree gchar *name = NULL;
	gboolean ok;
	gint src = download_leads(guard, temp_inode, rows, i, &name);

	if (src < 0)
		goto kept;
	ok = axis_volume_write_unlink_temp_beside(guard, dir, temp, (guint64)temp_inode, src, name, removed, error);
	close(src);
	return ok;

kept:
	g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED, "it may be the one name of a file "
		"this import moved out of the download (the download's name does not lead to it): it stays for the import, "
		"which looks again when it is retried.");
	return FALSE;
}

/* The library's folder of row @i (of a query with leftovers()'s columns),
 * through @guard, never through a link or onto another filesystem; -1 when it
 * cannot be opened, with *@missing when it is not there. */
static gint
library_folder(
	AxisVolumeGuard  *guard,
	AxisDbResult     *rows,
	guint             i,
	gboolean         *missing
){
	g_autoptr(GString) dir_bytes = NULL;
	g_autoptr(GError) local = NULL;
	g_auto(GStrv) parts = NULL;
	struct stat st;
	gint root;
	gint dir;
	guint k;

	*missing = FALSE;
	if (!axis_library_name_decode(axis_db_result_get_text(rows, i, 1), &dir_bytes) ||
	    memchr(dir_bytes->str, '\0', dir_bytes->len) != NULL)
		return -1;
	root = axis_volume_guard_open_dir(guard, axis_db_result_get_text(rows, i, 6), &local);
	if (root < 0) {
		*missing = g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
		return -1;
	}
	parts = g_strsplit(dir_bytes->str, "/", 140);
	dir = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	close(root);
	for (k = 0; dir >= 0 && parts[k] != NULL; k++) {
		gint next = openat(dir, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		if (next < 0 && errno == ENOENT)
			*missing = TRUE;
		close(dir);
		dir = next;
		if (dir >= 0 && (fstat(dir, &st) != 0 || (guint64)st.st_dev != axis_volume_guard_get_dev(guard))) {
			close(dir);
			dir = -1;
		}
	}
	return dir;
}

#define SETTLE_COLUMNS "f.id, f.dest_dir, f.temp_name, f.temp_inode, f.mode, r.volume_id, r.rel_path, f.dest_name," \
	" f.step, f.replaced_inode, i.source_volume_id, i.source_path, f.path, f.inode, f.size, f.source_mtime_ns," \
	" f.source_ctime_ns"

/* A statement on @conn when given, else on the pool. */
static gboolean
settle_exec(
	AxisImport     *self,
	AxisDbConn     *conn,
	const gchar    *sql,
	gint            n_params,
	const gchar   **params,
	AxisDbResult  **result,
	GError        **error
){
	if (conn != NULL)
		return axis_db_conn_exec(conn, sql, n_params, params, result, error);
	return axis_db_pool_exec(self->pool, sql, n_params, params, result, error);
}

/* One row's outcome: settled (nothing said), or *@why for a person. */
static AxisImportSettle
settle_row(
	AxisImport    *self,
	AxisDbConn    *conn,
	gint64         import_id,
	AxisDbResult  *rows,
	guint          i,
	guint         *put_back,
	gchar        **why,
	GError       **error
){
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *shown = axis_library_name_display(axis_db_result_get_text(rows, i, 1));
	g_autofree gchar *name = NULL;
	const gchar *temp = axis_db_result_get_text(rows, i, 2);
	const gchar *mode = axis_db_result_get_text(rows, i, 4);
	gboolean copies = g_strcmp0(mode, "copy") == 0 || g_strcmp0(mode, "move_copy") == 0;
	gboolean moved = g_strcmp0(mode, "move") == 0;
	gboolean missing = FALSE;
	gint64 volume = 0;
	gint64 temp_inode = 0;
	struct stat st;
	Look look;
	gint dir;
	gint wdir;
	gint src = -1;

	*why = NULL;
	axis_db_result_get_int64(rows, i, 3, &temp_inode);
	axis_db_result_get_int64(rows, i, 5, &volume);
	reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, &local);
	if (reader == NULL) {
		*why = g_strdup_printf("a file of it that an interrupted run placed under the temporary name %s, in the "
			"library's folder %s, cannot be looked at now (%s)", temp, shown, local->message);
		return AXIS_IMPORT_UNKNOWN;
	}
	dir = library_folder(reader, rows, i, &missing);
	if (dir < 0) {
		if (!missing) {
			*why = g_strdup_printf("a file of it that an interrupted run placed under the temporary name %s is in the "
				"library's folder %s, which cannot be opened now", temp, shown);
			return AXIS_IMPORT_UNKNOWN;
		}
		/* No folder: nothing under its temporary name there; but a file whose
		 * download's name does not lead to it may be in a folder renamed
		 * since. */
		if (copies) {
			look = copy_source_state(self, rows, i);
		} else {
			look = leads_state(reader, temp_inode, rows, i, &src, &name);
			if (src >= 0)
				close(src);
		}
		if (look == LOOK_YES)
			return AXIS_IMPORT_SETTLED;
		if (look == LOOK_UNKNOWN) {
			*why = g_strdup_printf("a file of it that an interrupted run placed under the temporary name %s should be in "
				"the library's folder %s, which is not there, and its download's file cannot be looked at now", temp,
				shown);
			return AXIS_IMPORT_UNKNOWN;
		}
		*why = g_strdup_printf("a file of it that an interrupted run placed under the temporary name %s should be in the "
			"library's folder %s, which is not there now", temp, shown);
		return AXIS_IMPORT_UNSETTLED;
	}
	if (fstatat(dir, temp, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gint err = errno;

		close(dir);
		if (err == ENOENT)
			return AXIS_IMPORT_SETTLED;
		*why = g_strdup_printf("the temporary name %s in the library's folder %s cannot be looked at now (%s)", temp,
			shown, g_strerror(err));
		return AXIS_IMPORT_UNKNOWN;
	}
	close(dir);
	/* Not the file the row placed: not this import's to settle. */
	if (!S_ISREG(st.st_mode) || (temp_inode > 0 && (gint64)st.st_ino != temp_inode))
		return AXIS_IMPORT_SETTLED;
	if (copies) {
		look = copy_source_state(self, rows, i);
		if (look == LOOK_YES)
			return AXIS_IMPORT_SETTLED;
		if (look == LOOK_UNKNOWN) {
			*why = g_strdup_printf("the copy under the temporary name %s, in the library's folder %s, cannot be told now "
				"from the only copy of its file: the download's file it was made from cannot be looked at, or the copy "
				"was never finished", temp, shown);
			return AXIS_IMPORT_UNKNOWN;
		}
		*why = g_strdup_printf("the copy under the temporary name %s, in the library's folder %s, may be the only copy of "
			"its file: the download's file it was made from is not there any more, or is another file now", temp,
			shown);
		return AXIS_IMPORT_UNSETTLED;
	}
	/* Its download's name leads to it too: not its only name. */
	look = leads_state(reader, temp_inode, rows, i, &src, &name);
	if (src >= 0)
		close(src);
	g_clear_pointer(&name, g_free);
	if (look == LOOK_YES)
		return AXIS_IMPORT_SETTLED;
	if (look == LOOK_UNKNOWN) {
		*why = g_strdup_printf("the temporary name %s, in the library's folder %s, may be the only name of its file: its "
			"download's folder cannot be looked at now", temp, shown);
		return AXIS_IMPORT_UNKNOWN;
	}
	if (!moved) {
		*why = g_strdup_printf("the temporary name %s, in the library's folder %s, is the only name of its file: its "
			"download's name no longer leads to it", temp, shown);
		return AXIS_IMPORT_UNSETTLED;
	}
	/* Moved out of the download and not named: back under its download name,
	 * never over a name taken.  Nothing is written while storage is
	 * read-only, nor on a volume that cannot be written now. */
	writer = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_WRITE, &local);
	if (writer == NULL) {
		*why = g_strdup_printf("a file of it that an interrupted run moved out of the download is under the temporary "
			"name %s in the library's folder %s, and cannot be put back in the download now (%s)", temp, shown,
			local->message);
		return AXIS_IMPORT_UNKNOWN;
	}
	wdir = library_folder(writer, rows, i, &missing);
	if (wdir < 0) {
		*why = g_strdup_printf("a file of it that an interrupted run moved out of the download is under the temporary "
			"name %s in the library's folder %s, which cannot be opened now", temp, shown);
		return AXIS_IMPORT_UNKNOWN;
	}
	src = download_place(writer, rows, i, &name, &missing);
	if (src < 0) {
		close(wdir);
		*why = g_strdup_printf("a file of it that an interrupted run moved out of the download is under the temporary "
			"name %s in the library's folder %s, and %s", temp, shown, missing ? "its download's folder is not there any "
			"more, so it cannot be put back" : "its download's folder cannot be opened now");
		return missing ? AXIS_IMPORT_UNSETTLED : AXIS_IMPORT_UNKNOWN;
	}
	if (!axis_volume_write_move_back(writer, wdir, temp, (guint64)temp_inode, src, name, &local)) {
		gboolean taken = g_error_matches(local, G_IO_ERROR, G_IO_ERROR_EXISTS);

		close(wdir);
		close(src);
		*why = g_strdup_printf("a file of it that an interrupted run moved out of the download is under the temporary "
			"name %s in the library's folder %s, and could not be put back in the download%s (%s)", temp, shown,
			taken ? "" : " now", local->message);
		return taken ? AXIS_IMPORT_UNSETTLED : AXIS_IMPORT_UNKNOWN;
	}
	close(wdir);
	close(src);
	{
		const gchar *more[1];

		more[0] = axis_db_result_get_text(rows, i, 0);
		if (!settle_exec(self, conn, "UPDATE import_file SET step = NULL, temp_inode = NULL, replaced_inode = NULL,"
		        " note = 'An interrupted run had moved it into the library under its temporary name; it was put back in"
		        " the download.' WHERE id = $1::bigint", 1, more, NULL, error))
			return AXIS_IMPORT_UNKNOWN;
	}
	g_message("import %" G_GINT64_FORMAT ": a file an interrupted run had moved into the library was put back in the "
		"download", import_id);
	(*put_back)++;
	return AXIS_IMPORT_SETTLED;
}

gboolean
axis_import_settle_temporaries(
	AxisImport        *self,
	AxisDbConn        *conn,
	gint64             import_id,
	guint             *put_back,
	AxisImportSettle  *outcome,
	gchar            **why,
	GError           **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	guint i;

	*put_back = 0;
	*outcome = AXIS_IMPORT_SETTLED;
	*why = NULL;
	params[0] = id;
	if (!settle_exec(self, conn, "SELECT " SETTLE_COLUMNS
	        " FROM import_file f JOIN import i ON i.id = f.import_id JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.import_id = $1::bigint AND f.step = 'temp' AND f.temp_name IS NOT NULL AND f.dest_dir IS NOT NULL"
	        " ORDER BY f.id", 1, params, &rows, error))
		return FALSE;
	/* A proven file left is said at once; one that cannot be told now is
	 * said unless a later one is proven. */
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autofree gchar *row_why = NULL;
		GError *failure = NULL;
		AxisImportSettle row = settle_row(self, conn, import_id, rows, i, put_back, &row_why, &failure);

		if (failure != NULL) {
			g_propagate_error(error, failure);
			return FALSE;
		}
		if (row == AXIS_IMPORT_UNSETTLED) {
			g_free(*why);
			*why = g_steal_pointer(&row_why);
			*outcome = AXIS_IMPORT_UNSETTLED;
			return TRUE;
		}
		if (row == AXIS_IMPORT_UNKNOWN && *outcome == AXIS_IMPORT_SETTLED) {
			*why = g_steal_pointer(&row_why);
			*outcome = AXIS_IMPORT_UNKNOWN;
		}
	}
	return TRUE;
}

/*
 * Imports dismissed (or assigned to another title) while a file an
 * interrupted run had placed was still under its temporary name, as could be
 * done before Dismiss settled such files first: no run takes them up again,
 * so the file would stay there for good, in neither the download nor its
 * place.  Each is settled now (a moved file put back in the download); one
 * proven unsettled by what is on disk now is opened again for a person,
 * blocked, saying where the file is.  One that cannot be told now (a volume
 * away or read-only, a folder that cannot be opened, storage read-only) is
 * left as it is and looked at next time; nothing at all is done while storage
 * is read-only.  At most %DISMISSED_BATCH a run (or the import's
 * settle_batch), going on each time after the last looked at, so that none
 * waits behind those that cannot be settled.
 */
#define DISMISSED_BATCH 100

static gboolean
dismissed_settle(
	AxisImport               *self,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *after_text = NULL;
	g_autofree gchar *batch_text = NULL;
	guint batch = self->settle_batch > 0 ? self->settle_batch : DISMISSED_BATCH;
	const gchar *query[2];
	gint64 after;
	gint64 last = 0;
	guint i;

	if (axis_storage_is_read_only_mode(self->storage))
		return TRUE;
	batch_text = g_strdup_printf("%u", batch);
	g_mutex_lock(&self->hook_lock);
	after = self->settle_after;
	g_mutex_unlock(&self->hook_lock);
	after_text = axis_import_int_text(after);
	query[0] = after_text;
	query[1] = batch_text;
	if (!axis_db_pool_exec(self->pool, "SELECT DISTINCT i.id, r.volume_id FROM import i"
	        " JOIN import_file f ON f.import_id = i.id JOIN library_root r ON r.id = i.root_id"
	        " WHERE i.state = 'dismissed' AND f.step = 'temp' AND f.temp_name IS NOT NULL AND f.dest_dir IS NOT NULL"
	        " AND i.id > $1::bigint"
	        " AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || i.id::text"
	        "  AND j.state = 'running') ORDER BY i.id LIMIT $2::integer", 2, query, &rows, error))
		return FALSE;
	/* A batch not full: the next run starts from the beginning again. */
	if (axis_db_result_get_n_rows(rows) > 0)
		axis_db_result_get_int64(rows, axis_db_result_get_n_rows(rows) - 1, 0, &last);
	g_mutex_lock(&self->hook_lock);
	self->settle_after = axis_db_result_get_n_rows(rows) < batch ? 0 : last;
	g_mutex_unlock(&self->hook_lock);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisVolumeGuard) reader = NULL;
		g_autoptr(GPtrArray) blockers = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *why = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *json = NULL;
		const gchar *params[3];
		AxisImportSettle outcome = AXIS_IMPORT_SETTLED;
		gint64 id = 0;
		gint64 volume = 0;
		guint put_back = 0;

		axis_db_result_get_int64(rows, i, 0, &id);
		axis_db_result_get_int64(rows, i, 1, &volume);
		reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
		if (reader == NULL)
			continue;
		if (!axis_import_settle_temporaries(self, NULL, id, &put_back, &outcome, &why, error))
			return FALSE;
		if (report != NULL)
			report->temporaries += put_back;
		/* Only what is proven now opens it again. */
		if (outcome != AXIS_IMPORT_UNSETTLED) {
			if (outcome == AXIS_IMPORT_UNKNOWN)
				g_debug("import %" G_GINT64_FORMAT ": dismissed; looked at again next time: %s", id, why);
			continue;
		}
		text = g_strdup_printf("It was dismissed, but %s. Nothing of it is lost: select \"Retry this import\" so that Axis "
			"gives the file its name (or puts it back once its download's folder is there again), or put it where it "
			"belongs by hand.", why);
		blockers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_reason_free);
		g_ptr_array_add(blockers, axis_import_reason_new("moved_file_kept", text, FALSE));
		json = axis_import_reasons_to_json(blockers);
		params[0] = axis_db_result_get_text(rows, i, 0);
		params[1] = text;
		params[2] = json;
		if (!axis_db_pool_exec(self->pool, "UPDATE import SET state = 'blocked', reason = 'moved_file_kept',"
		        " reason_text = left($2, 2000), blockers = $3::jsonb, finished_at = NULL, retry_after = NULL,"
		        " updated_at = now() WHERE id = $1::bigint AND state = 'dismissed'", 3, params, NULL, &local)) {
			/* Another import of the title is open: this one stays dismissed,
			 * its file recorded, and is looked at again next time. */
			if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
				g_warning("import %" G_GINT64_FORMAT ": %s; it cannot be opened again while another import of its "
					"title is open", id, why);
				continue;
			}
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		g_message("import %" G_GINT64_FORMAT ": opened again for a person: %s", id, why);
	}
	return TRUE;
}

static gboolean
leftovers(
	AxisImport               *self,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	/* Imports no job is placing: blocked, dismissed, imported. */
	if (!axis_db_pool_exec(self->pool, "SELECT " SETTLE_COLUMNS
	        " FROM import_file f JOIN import i ON i.id = f.import_id JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.step IN ('temp', 'swapped') AND f.temp_name IS NOT NULL AND f.dest_dir IS NOT NULL"
	        " AND i.state IN ('blocked', 'dismissed', 'imported')"
	        " AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || i.id::text"
	        "  AND j.state = 'running') ORDER BY f.id LIMIT 500", 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisVolumeGuard) guard = NULL;
		g_autoptr(GString) dir_bytes = NULL;
		g_autoptr(GString) final_bytes = NULL;
		g_auto(GStrv) parts = NULL;
		g_autofree gchar *row_text = NULL;
		const gchar *temp = axis_db_result_get_text(rows, i, 2);
		const gchar *mode = axis_db_result_get_text(rows, i, 4);
		gint64 row = 0;
		gint64 volume = 0;
		gint64 temp_inode = 0;
		gint64 replaced = 0;
		gboolean removed = FALSE;
		gboolean published = FALSE;
		gboolean moved;
		gboolean ok;
		struct stat st;
		const gchar *params[2];
		GError *local = NULL;
		gint root;
		gint dir;
		guint k;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 3, &temp_inode);
		axis_db_result_get_int64(rows, i, 5, &volume);
		axis_db_result_get_int64(rows, i, 9, &replaced);
		guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_WRITE, NULL);
		if (guard == NULL)
			continue;
		if (!axis_library_name_decode(axis_db_result_get_text(rows, i, 1), &dir_bytes) ||
		    memchr(dir_bytes->str, '\0', dir_bytes->len) != NULL)
			continue;
		root = axis_volume_guard_open_dir(guard, axis_db_result_get_text(rows, i, 6), NULL);
		if (root < 0)
			continue;
		parts = g_strsplit(dir_bytes->str, "/", 140);
		dir = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		for (k = 0; dir >= 0 && parts[k] != NULL; k++) {
			gint next = openat(dir, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

			close(dir);
			dir = next;
			if (dir >= 0 && (fstat(dir, &st) != 0 || (guint64)st.st_dev != axis_volume_guard_get_dev(guard))) {
				close(dir);
				dir = -1;
			}
		}
		close(root);
		row_text = axis_import_int_text(row);
		params[0] = row_text;
		moved = g_strcmp0(mode, "move") == 0 && g_strcmp0(axis_db_result_get_text(rows, i, 8), "swapped") != 0;
		if (dir < 0) {
			/* No folder, no temporary name in it to remove; but a file moved
			 * out of the download may be in a folder that cannot be opened now,
			 * and so may a copy or a link whose download's name no longer leads
			 * to it (its only copy): its row is left for its import, which
			 * looks again (and says where it should be).  Only a row of which
			 * nothing can be there alone forgets its step: a copy not begun, a
			 * link whose download's name leads to its file. */
			gboolean alone = moved;

			if (!alone && temp_inode > 0) {
				if (g_strcmp0(mode, "copy") == 0 || g_strcmp0(mode, "move_copy") == 0) {
					alone = TRUE;
				} else {
					g_autofree gchar *name = NULL;
					gint src = download_leads(guard, temp_inode, rows, i, &name);

					alone = src < 0;
					if (src >= 0)
						close(src);
				}
			}
			if (!alone)
				axis_db_pool_exec(self->pool, "UPDATE import_file SET step = NULL WHERE id = $1::bigint", 1, params,
					NULL, NULL);
			continue;
		}
		/* Published before the job died: the final name has the file. */
		if (axis_library_name_decode(axis_db_result_get_text(rows, i, 7), &final_bytes) &&
		    memchr(final_bytes->str, '\0', final_bytes->len) == NULL &&
		    fstatat(dir, final_bytes->str, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
		    (gint64)st.st_ino == temp_inode)
			published = TRUE;
		if (moved && !(published && replaced > 0)) {
			/* A file moved out of the download: its temporary name goes only
			 * beside a name of its own, looked at now (its final name, or the
			 * download's); never for its link count, which a name outside (a
			 * cross-seed, a backup) counts too.  Otherwise it is its one name
			 * in the library and the download, and stays. */
			ok = published ?
				axis_volume_write_unlink_temp_beside(guard, dir, temp, (guint64)MAX(temp_inode, 0), dir,
					final_bytes->str, &removed, &local) :
				beside_download(guard, dir, temp, temp_inode, rows, i, &removed, &local);
		} else {
			/* A copy not given its name is not the file's only copy while the
			 * download's file it was made from is still there: it may go
			 * (as recover() lets it).  A link's goes beside its other name. */
			gboolean last_ok = !published && (g_strcmp0(mode, "copy") == 0 || g_strcmp0(mode, "move_copy") == 0) &&
				copy_source_state(self, rows, i) == LOOK_YES;

			ok = axis_volume_write_unlink_temp(guard, dir, temp, (guint64)(published && replaced > 0 ? replaced :
				MAX(temp_inode, 0)), last_ok, &removed, &local);
		}
		if (!ok) {
			g_autofree gchar *text = g_strdup_printf("A temporary file was kept: %s", local->message);

			params[1] = text;
			axis_db_pool_exec(self->pool, "UPDATE import_file SET note = left($2, 1000) WHERE id = $1::bigint", 2,
				params, NULL, NULL);
			if (report != NULL)
				report->kept++;
			g_error_free(local);
			close(dir);
			continue;
		}
		close(dir);
		if (removed && report != NULL)
			report->temporaries++;
		(void)mode;
		if (!axis_db_pool_exec(self->pool, published ? "UPDATE import_file SET step = 'published' WHERE id = $1::bigint" :
		        "UPDATE import_file SET step = NULL WHERE id = $1::bigint", 1, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

/* ── A download's files gone ────────────────────────────────────── */

static gint library_has(AxisImport *self, gint64 volume, const gchar *root_path, const gchar *dest_dir,
	const gchar *dest_name, gint64 inode, gint64 size);

gint
axis_import_source_absent(
	AxisVolumeGuard *guard,
	const gchar     *source_path,
	guint64         *inode
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *last = NULL;
	const gchar *slash;
	struct stat st;
	gint fd;
	gint rc;

	if (inode != NULL)
		*inode = 0;
	if (guard == NULL || source_path == NULL || source_path[0] == '\0')
		return -1;
	slash = strrchr(source_path, '/');
	parent = slash != NULL ? g_strndup(source_path, (gsize)(slash - source_path)) : g_strdup("");
	last = g_strdup(slash != NULL ? slash + 1 : source_path);
	fd = axis_volume_guard_open_dir(guard, parent, &local);
	if (fd < 0)
		return g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
			g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY) ? 1 : -1;
	rc = fstatat(fd, last, &st, AT_SYMLINK_NOFOLLOW);
	if (rc != 0) {
		gint err = errno;

		close(fd);
		return err == ENOENT ? 1 : -1;
	}
	close(fd);
	if (S_ISLNK(st.st_mode) || (guint64)st.st_dev != axis_volume_guard_get_dev(guard))
		return -1;
	if (inode != NULL)
		*inode = (guint64)st.st_ino;
	return 0;
}

gint
axis_import_placed_count(
	AxisImport  *self,
	gint64       import_id,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	gint64 count = 0;

	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM import_file WHERE import_id = $1::bigint"
	        " AND (step IS NOT NULL OR library_file_id IS NOT NULL OR source_state IS NOT NULL)", 1, params, &rows,
	        error))
		return -1;
	axis_db_result_get_int64(rows, 0, 0, &count);
	return (gint)MIN(count, G_MAXINT);
}

gboolean
axis_import_files_gone(
	AxisImport  *self,
	gint64       import_id,
	gboolean     by_sweep,
	gboolean    *done,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[2];

	*done = FALSE;
	params[0] = id;
	params[1] = by_sweep ? "true" : "false";
	/* Where they were and when Axis last saw them: the import's walk, or the
	 * download's completion.  Everything that must hold is in this one
	 * statement: an import of which nothing is placed or moved (a file with a
	 * step was being placed); by the sweep, one that waits or is blocked and
	 * that no job runs (the run of an import sets it running, and records a
	 * file's step before it moves the file, so a sweep that saw it waiting
	 * before cannot end it now); by the import's own run, one that is
	 * running. */
	if (!axis_db_pool_exec(self->pool, "UPDATE import i SET state = 'dismissed', reason = 'files_gone',"
	        " blockers = '[]', retry_after = NULL, finished_at = now(), updated_at = now(),"
	        " reason_text = left('The download''s files were no longer at ' || coalesce(v.name, 'its volume') || ': '"
	        "  || j.source_path || ' when Axis came to import them' || coalesce(' (Axis last saw them there on '"
	        "  || to_char(coalesce(j.seen_at, d.completed_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI') || ' UTC)', '')"
	        "  || ': something else deleted or moved them (a cleanup of the download folder, a person). Nothing of it"
	        " was imported, nothing is tried again, and its title is wanted again.', 2000)"
	        " FROM import j LEFT JOIN volume v ON v.id = j.source_volume_id LEFT JOIN download d ON d.id = j.download_id"
	        " WHERE i.id = $1::bigint AND j.id = i.id"
	        " AND CASE WHEN $2::boolean THEN i.state IN ('waiting', 'blocked')"
	        "  AND NOT EXISTS (SELECT 1 FROM job k WHERE k.dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || i.id::text"
	        "   AND k.state = 'running')"
	        "  ELSE i.state = 'running' END"
	        " AND NOT EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = i.id AND (f.step IS NOT NULL"
	        "  OR f.library_file_id IS NOT NULL OR f.source_state IS NOT NULL))"
	        " RETURNING i.id", 2, params, &rows, error))
		return FALSE;
	*done = axis_db_result_get_n_rows(rows) > 0;
	if (*done)
		g_message("import %" G_GINT64_FORMAT ": the download's files disappeared before they were imported",
			import_id);
	return TRUE;
}

/*
 * Imports that wait or are blocked a long time (storage read-only, a volume
 * offline, a person to decide) and whose download's files something else
 * deleted meanwhile (a cleanup of the download folder by age): looked at
 * through a read guard, which storage read-only allows, and ended so, with
 * nothing tried again.  Only an import of which nothing was placed or
 * moved, and that no job is placing; a volume that cannot be looked at now
 * tells nothing.
 */
static gboolean
sources_gone(
	AxisImport               *self,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) guards = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_volume_guard_release);
	g_autoptr(GHashTable) away = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT i.id, i.source_volume_id, i.source_path FROM import i"
	        " WHERE i.state IN ('waiting', 'blocked') AND i.download_id IS NOT NULL AND i.source_volume_id IS NOT NULL"
	        " AND NOT EXISTS (SELECT 1 FROM import_file f WHERE f.import_id = i.id AND (f.step IS NOT NULL"
	        "  OR f.library_file_id IS NOT NULL OR f.source_state IS NOT NULL))"
	        " AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || i.id::text"
	        "  AND j.state = 'running') ORDER BY i.id LIMIT " G_STRINGIFY(BATCH), 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisVolumeGuard *guard;
		gint64 id = 0;
		gint64 volume = 0;
		gboolean done = FALSE;

		axis_db_result_get_int64(rows, i, 0, &id);
		axis_db_result_get_int64(rows, i, 1, &volume);
		if (g_hash_table_contains(away, &volume))
			continue;
		guard = g_hash_table_lookup(guards, &volume);
		if (guard == NULL) {
			gint64 *key = g_new(gint64, 1);

			*key = volume;
			guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
			if (guard == NULL) {
				g_hash_table_add(away, key);
				continue;
			}
			g_hash_table_insert(guards, key, guard);
		}
		if (axis_import_source_absent(guard, axis_db_result_get_text(rows, i, 2), NULL) != 1)
			continue;
		/* Looked at again through the guard: the volume is still the one. */
		if (!axis_volume_guard_check(guard, NULL)) {
			gint64 *key = g_new(gint64, 1);

			*key = volume;
			g_hash_table_add(away, key);
			continue;
		}
		if (!axis_import_files_gone(self, id, TRUE, &done, error))
			return FALSE;
		if (done && report != NULL)
			report->files_gone++;
	}
	return TRUE;
}

/*
 * A torrent left seeding whose files something else deleted after its
 * import (a cleanup of the download folder by age does not know it seeds):
 * the library's names are separate and stay, and the import says so
 * truthfully, once, on its line about the files at the client
 * (folder_removal "gone").  Looked at a batch at a time, the longest
 * unlooked-at first (seen_at), through a read guard.
 */
static gboolean
seeding_gone(
	AxisImport               *self,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT i.id, i.source_volume_id, i.source_path, r.volume_id, r.rel_path,"
	        " f.dest_dir, f.dest_name, f.dest_inode, f.size"
	        " FROM import i JOIN download d ON d.id = i.download_id LEFT JOIN library_root r ON r.id = i.root_id"
	        " LEFT JOIN LATERAL (SELECT dest_dir, dest_name, dest_inode, size FROM import_file"
	        "  WHERE import_id = i.id AND role IN ('film', 'episode') AND step = 'published' ORDER BY position LIMIT 1)"
	        "  f ON true"
	        " WHERE i.state = 'imported' AND d.protocol = 'torrent' AND i.client_removal = 'kept'"
	        " AND i.folder_removal IS NULL AND i.source_volume_id IS NOT NULL"
	        " ORDER BY coalesce(i.seen_at, i.finished_at) NULLS FIRST, i.id LIMIT " G_STRINGIFY(BATCH), 0, NULL, &rows,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisVolumeGuard) guard = NULL;
		g_autofree gchar *id_text = NULL;
		const gchar *params[2];
		gint64 id = 0;
		gint64 volume = 0;
		gint in_place;
		gint absent;

		axis_db_result_get_int64(rows, i, 0, &id);
		axis_db_result_get_int64(rows, i, 1, &volume);
		id_text = axis_import_int_text(id);
		params[0] = id_text;
		guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
		if (guard == NULL)
			continue;
		absent = axis_import_source_absent(guard, axis_db_result_get_text(rows, i, 2), NULL);
		if (absent == 0) {
			if (!axis_db_pool_exec(self->pool, "UPDATE import SET seen_at = now() WHERE id = $1::bigint", 1, params,
			        NULL, error))
				return FALSE;
			continue;
		}
		if (absent != 1 || !axis_volume_guard_check(guard, NULL))
			continue;
		{
			gint64 root_volume = 0;
			gint64 dest_inode = 0;
			gint64 size = 0;

			axis_db_result_get_int64(rows, i, 3, &root_volume);
			axis_db_result_get_int64(rows, i, 7, &dest_inode);
			axis_db_result_get_int64(rows, i, 8, &size);
			in_place = library_has(self, root_volume, axis_db_result_get_text(rows, i, 4),
				axis_db_result_get_text(rows, i, 5), axis_db_result_get_text(rows, i, 6), dest_inode, size);
		}
		if (in_place < 0)
			continue;
		params[1] = in_place == 1 ?
			"Its files at the client were deleted by something else after the import (a cleanup of the download "
			"folder, a person), so the torrent, still at its client, cannot seed them any more. The library's files "
			"are separate names of them and are in place." :
			"Its files at the client were deleted by something else after the import (a cleanup of the download "
			"folder, a person), so the torrent, still at its client, cannot seed them any more. The library's files "
			"were separate names of them, but the one this import placed is not in place now either.";
		if (!axis_db_pool_exec(self->pool, "UPDATE import SET folder_removal = 'gone', folder_removal_text = $2,"
		        " seen_at = now(), updated_at = now() WHERE id = $1::bigint AND folder_removal IS NULL", 2, params, NULL,
		        error))
			return FALSE;
		if (report != NULL)
			report->files_gone++;
	}
	return TRUE;
}

/* ── The download's files after a move across volumes ───────────── */

/* Whether the library holds, at @root_path/@dest_dir/@dest_name on
 * @volume, the regular file @inode of @size, reached without a link or
 * another filesystem: 1, 0, or -1 when its volume cannot be looked at. */
static gint
library_has(
	AxisImport  *self,
	gint64       volume,
	const gchar *root_path,
	const gchar *dest_dir,
	const gchar *dest_name,
	gint64       inode,
	gint64       size
){
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(GString) dir_bytes = NULL;
	g_autoptr(GString) name_bytes = NULL;
	g_auto(GStrv) parts = NULL;
	struct stat st;
	guint64 dev;
	gint current;
	guint k;

	if (dest_dir == NULL || dest_name == NULL || inode <= 0 || !axis_library_name_decode(dest_dir, &dir_bytes) ||
	    memchr(dir_bytes->str, '\0', dir_bytes->len) != NULL || !axis_library_name_decode(dest_name, &name_bytes) ||
	    memchr(name_bytes->str, '\0', name_bytes->len) != NULL || name_bytes->len == 0)
		return 0;
	reader = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_READ, NULL);
	if (reader == NULL)
		return -1;
	dev = axis_volume_guard_get_dev(reader);
	current = axis_volume_guard_open_dir(reader, root_path != NULL ? root_path : "", NULL);
	parts = g_strsplit(dir_bytes->str, "/", 140);
	for (k = 0; current >= 0 && parts[k] != NULL; k++) {
		gint next;

		if (parts[k][0] == '\0')
			continue;
		next = openat(current, parts[k], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		close(current);
		current = next;
		if (current >= 0 && (fstat(current, &st) != 0 || (guint64)st.st_dev != dev)) {
			close(current);
			current = -1;
		}
	}
	if (current < 0)
		return 0;
	if (fstatat(current, name_bytes->str, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
	    (guint64)st.st_ino == (guint64)inode && (gint64)st.st_size == size && (guint64)st.st_dev == dev) {
		close(current);
		return 1;
	}
	close(current);
	return 0;
}

gboolean
axis_import_moved_sources(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	guint i;

	params[0] = id;
	/* Once the import is recorded, and the client is done with the download
	 * (a torrent let go of it before the copy; a Usenet job's removal is
	 * made, or it never will be). */
	if (!axis_db_pool_exec(self->pool, "SELECT f.id, i.source_volume_id, i.source_path, f.path, f.inode, f.size,"
	        " f.dest_dir, f.dest_name, f.dest_inode, r.volume_id, r.rel_path, f.source_mtime_ns, f.source_ctime_ns"
	        " FROM import_file f JOIN import i ON i.id = f.import_id JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.source_state = 'pending' AND f.step = 'published' AND i.state IN ('imported', 'partial')"
	        " AND coalesce(i.client_removal, '') NOT IN ('pending', 'failed')"
	        " AND ($1::bigint = 0 OR i.id = $1::bigint) ORDER BY f.id LIMIT 500", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisVolumeGuard) guard = NULL;
		g_autoptr(GString) bytes = NULL;
		g_autoptr(GError) local = NULL;
		g_autofree gchar *row_text = NULL;
		g_autofree gchar *parent = NULL;
		g_autofree gchar *name = NULL;
		const gchar *source_path = axis_db_result_get_text(rows, i, 2);
		const gchar *path = axis_db_result_get_text(rows, i, 3);
		const gchar *state = NULL;
		const gchar *note = NULL;
		const gchar *more[3];
		AxisVolumeRecycledOutcome outcome;
		gint64 row = 0;
		gint64 volume = 0;
		gint64 inode = 0;
		gint64 size = 0;
		gint64 dest_inode = 0;
		gint64 dest_volume = 0;
		gint64 mtime = 0;
		gint64 ctime = 0;
		gint present;
		gint dir = -1;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 1, &volume);
		axis_db_result_get_int64(rows, i, 4, &inode);
		axis_db_result_get_int64(rows, i, 5, &size);
		axis_db_result_get_int64(rows, i, 8, &dest_inode);
		axis_db_result_get_int64(rows, i, 9, &dest_volume);
		if (!axis_db_result_is_null(rows, i, 11))
			axis_db_result_get_int64(rows, i, 11, &mtime);
		if (!axis_db_result_is_null(rows, i, 12))
			axis_db_result_get_int64(rows, i, 12, &ctime);
		row_text = axis_import_int_text(row);
		if (source_path == NULL || path == NULL || !axis_library_name_decode(path, &bytes) ||
		    memchr(bytes->str, '\0', bytes->len) != NULL || inode <= 0)
			continue;
		/* The library's copy first: the download's file may be the one copy
		 * left. */
		present = library_has(self, dest_volume, axis_db_result_get_text(rows, i, 10),
			axis_db_result_get_text(rows, i, 6), axis_db_result_get_text(rows, i, 7), dest_inode, size);
		if (present < 0)
			continue;
		if (present == 0) {
			state = "kept";
			note = "The download's file was kept: the verified copy of it is not in the library as recorded, so the "
				"download's file may be the one copy left.";
		} else if (mtime == 0 || ctime == 0) {
			/* No times recorded of the file copied. */
			state = "kept";
			note = "The download's file was kept: Axis recorded nothing of its times when it was copied, so it cannot "
				"tell that it is still the file copied.";
		} else {
			/* The download's file, through a write guard of its volume. */
			guard = axis_volume_guard_acquire(self->storage, volume, AXIS_VOLUME_INTENT_WRITE, NULL);
			if (guard == NULL)
				continue;
			{
				const gchar *slash = strrchr(bytes->str, '/');
				const gchar *last = strrchr(source_path, '/');

				if (bytes->len == 0) {
					parent = last != NULL ? g_strndup(source_path, (gsize)(last - source_path)) : g_strdup("");
					name = g_strdup(last != NULL ? last + 1 : source_path);
					dir = axis_volume_guard_open_dir(guard, parent, &local);
				} else {
					g_autofree gchar *inside = slash != NULL ? g_strndup(bytes->str, (gsize)(slash - bytes->str)) : NULL;

					parent = inside != NULL ? g_strconcat(source_path, "/", inside, NULL) : g_strdup(source_path);
					name = g_strdup(slash != NULL ? slash + 1 : bytes->str);
					dir = axis_volume_guard_open_dir(guard, parent, &local);
				}
			}
			if (dir < 0) {
				if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
				    g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
					state = "gone";
				} else if (local != NULL && local->domain == AXIS_STORAGE_ERROR &&
				           (local->code == AXIS_STORAGE_ERROR_NOT_CONTAINED ||
				            local->code == AXIS_STORAGE_ERROR_INVALID)) {
					state = "kept";
					note = "The download's file was kept: the way to it passes a symbolic link or another filesystem, "
						"which Axis never follows.";
				} else {
					continue;
				}
			} else {
				gboolean ok = axis_volume_write_remove_moved(guard, dir, name, (guint64)inode, (guint64)size, mtime,
					ctime, &outcome, &local);

				close(dir);
				if (!ok)
					continue;
				state = outcome == AXIS_VOLUME_RECYCLED_REMOVED ? "deleted" : outcome == AXIS_VOLUME_RECYCLED_GONE ?
					"gone" : "kept";
				if (outcome == AXIS_VOLUME_RECYCLED_CHANGED)
					note = "The download's file was kept: it is not the file copied, as it was then (another file has "
						"its name now, or it was changed after it was copied).";
			}
		}
		more[0] = row_text;
		more[1] = state;
		more[2] = note;
		if (!axis_db_pool_exec(self->pool, "UPDATE import_file SET source_state = $2, note = coalesce($3, note)"
		        " WHERE id = $1::bigint AND source_state = 'pending'", 3, more, NULL, error))
			return FALSE;
		if (report != NULL && g_strcmp0(state, "deleted") == 0)
			report->moved++;
	}
	return TRUE;
}

/* ── Downloads after their import ───────────────────────────────── */

gboolean
axis_import_client_removals(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[4];
	AxisImportSettings settings;
	guint i;

	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT i.id, i.download_id, d.protocol FROM import i JOIN download d ON d.id = i.download_id"
	        " WHERE i.state = 'imported' AND i.client_removal IN ('pending', 'failed') AND d.state = 'imported'"
	        " AND ($1::bigint = 0 OR i.id = $1::bigint) ORDER BY i.id LIMIT 100", 1, params, &rows, error))
		return FALSE;
	axis_import_settings_read(self, &settings);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autofree gchar *row_text = NULL;
		g_autofree gchar *text = NULL;
		const gchar *outcome = "done";
		const gchar *folder = NULL;
		gboolean torrent = g_strcmp0(axis_db_result_get_text(rows, i, 2), "torrent") == 0;
		gint64 row = 0;
		gint64 download = 0;
		GError *local = NULL;
		g_autofree gchar *left = NULL;
		gboolean removed;

		axis_db_result_get_int64(rows, i, 0, &row);
		axis_db_result_get_int64(rows, i, 1, &download);
		removed = axis_downloads_remove_imported_full(self->downloads, download, settings.torrents_remove, &left,
			&local);
		if (removed && torrent && left != NULL)
			/* Its client said it is in a library folder now (the downloads
			 * layer's place rule): nothing was deleted there. */
			text = g_strdup(left);
		else if (removed && torrent)
			text = g_strdup("The torrent was removed from its client with its downloaded files; the library's files are "
				"separate names and were not touched. To leave torrents seeding, set \"After a torrent is imported\" to "
				"seed.");
		else if (removed)
			/* SABnzbd keeps a completed job's folder: Axis removes it
			 * (axis-import-folder.c), and says so on its own line. */
			text = g_strdup("The job was removed from its client.");
		if (!removed && g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE)) {
			/* Not a failure, never retried: the client said it has no such
			 * download.  What it left on disk is looked at by the folder
			 * step (a torrent's leftover files, as a Usenet job's folder). */
			text = torrent ?
				g_strdup("The torrent was already gone from its client: something else removed it (a person, a plugin "
					"of the client or another program), and the client said it no longer has it, so the client deleted "
					"none of its files now.") :
				g_strdup("The job was already gone from its client: something else removed it, and the client said it "
					"no longer has it.");
			outcome = "gone";
			if (torrent)
				folder = "pending";
		} else if (!removed && g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS)) {
			/* A person moved it to another category or label: theirs now. */
			text = g_strdup_printf("It was left with its client, untouched: %s. Its files there are not Axis's to "
				"delete; the library's files are separate names and were not touched.", local->message);
			outcome = "kept";
		} else if (!removed && (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID))) {
			/* It never will be: left with its client. */
			text = g_strdup_printf("It was left with its client: %s", local->message);
			outcome = "kept";
		} else if (!removed) {
			text = g_strdup_printf("Its client could not remove it yet (tried again every hour): %s", local->message);
			outcome = "failed";
		}
		g_clear_error(&local);
		row_text = axis_import_int_text(row);
		params[0] = row_text;
		params[1] = outcome;
		params[2] = text;
		params[3] = folder;
		if (!axis_db_pool_exec(self->pool, "UPDATE import SET client_removal = $2, client_removal_text = left($3, 1000),"
		        " folder_removal = coalesce($4::text, folder_removal),"
		        " folder_removal_text = CASE WHEN $4::text IS NULL THEN folder_removal_text ELSE NULL END,"
		        " updated_at = now() WHERE id = $1::bigint", 4, params, NULL, error)) {
			axis_import_settings_clear(&settings);
			return FALSE;
		}
		if (removed && report != NULL)
			report->removals++;
	}
	axis_import_settings_clear(&settings);
	/* Then the download's files a move across volumes copied, and what
	 * SABnzbd left of the Usenet jobs it has removed, and what a torrent
	 * whose files were moved, or that is gone from its client, left. */
	return axis_import_moved_sources(self, import_id, report, error) &&
		axis_import_folder_removals(self, import_id, report, error);
}

gboolean
axis_import_run_cleanup(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report_out,
	GError                  **error
){
	AxisImportCleanupReport report;
	gboolean ok;

	memset(&report, 0, sizeof(report));
	/* First the records a removal on the Duplicates page left unfinished, so
	 * that a file it moved gets its time in the recycle folder. */
	ok = (import_id > 0 || axis_import_recover_duplicates(self, DUPLICATE_RECOVER_AFTER_S, &report, error)) &&
		remove_due(self, import_id, &report, error) && (import_id > 0 || dismissed_settle(self, &report, error)) &&
		(import_id > 0 || leftovers(self, &report, error)) &&
		axis_import_client_removals(self, import_id, &report, error) &&
		/* What "Delete this download" could not finish at once (the client did
		 * not answer, the disk could not be written): asked again, before the
		 * leftovers of the downloads removed outside an import are looked at. */
		(import_id > 0 || axis_import_deleted_downloads(self, 0, &report, error)) &&
		(import_id > 0 || axis_import_download_folder_removals(self, 0, &report, error)) &&
		(import_id > 0 || sources_gone(self, &report, error)) &&
		(import_id > 0 || seeding_gone(self, &report, error));
	if (report_out != NULL)
		*report_out = report;
	return ok;
}
