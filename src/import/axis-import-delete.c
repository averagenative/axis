/*
 * axis-import-delete.c - "Delete this download" on an import that waits for
 * a person (add-replace-grab-and-delete-download, slice B)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An import that is blocked, waiting or partly imported could only be
 * dismissed, which leaves its download on the disk and at its client.  An
 * administrator can delete the download instead, after a confirmation that
 * shows what goes.
 *
 * This file deletes nothing itself and has no rule of its own about what may
 * be deleted.  Everything Axis deletes goes through the removal of a
 * download's leftover (axis-import-folder.c: folder_decide(), then
 * folder_delete() through the volume's write guard), and the confirmation
 * shows that removal's own decision (axis_import_download_folder_look(),
 * which runs the deciding half and writes nothing).  So what is never
 * deleted is what that file never deletes: nothing in a library folder,
 * nothing that is or holds a library root, never the client's folder for
 * Axis itself, never through a symbolic link or into another filesystem,
 * only the place whose inode was recorded when the download completed, and
 * never while the volume is offline or cannot be written.
 *
 * In order:
 *
 *  1. The look, again (the post never trusts the page): a refusal, or a look
 *     that differs from the confirmation's digest, writes nothing.
 *  2. One transaction (axis_import_close_deleted() in axis-import.c): the
 *     import is closed as dismissed with the reason download_deleted and the
 *     person's name, its client removal pending; the action is recorded; the
 *     owner closes the grab.  The request is recorded before anything is
 *     asked of a client, so that a restart finishes it, and because the
 *     folder rules refuse a place an open import is in.
 *  3. The step (axis_import_deleted_downloads()), now and every hour for
 *     what could not be finished, one step for an import at a time (a claim:
 *     step_claim()): the place is judged again; then the client is asked to
 *     remove the download; then what the client kept (a Usenet job's folder),
 *     or left when it no longer had the download, is deleted by
 *     axis_import_download_folder_removals() for this one download.
 *
 * The client is asked to remove WITH ITS DATA under one rule, enforced where
 * it is asked (ask_client(), the one call of axis_downloads_remove() here)
 * and not by the order of the checks: at that moment the client lists the
 * download, something is at the place recorded when it completed, that place
 * passes every rule of the removal, and a torrent's client says the torrent
 * is at that place (axis_import_delete_with_data()).  A torrent's client
 * deletes a torrent wherever it is, so a torrent it still lists that is not
 * at the recorded place (moved in the client: into a library folder, for
 * one) is refused, on the page and in the step, and nothing is asked.  A
 * download its client does not list is asked for without its data, which
 * tells that it is gone and deletes nothing.
 *
 * A dismissed import of any other reason is never taken by the step: an
 * ordinary Dismiss leaves the download where it is.
 */

#include "import/axis-import-private.h"

#include <string.h>

#include "library/axis-library-name.h"

/* ── The confirmation ───────────────────────────────────────────── */

void
axis_import_delete_preview_free(AxisImportDeletePreview *preview)
{
	if (preview == NULL)
		return;
	g_free(preview->title_label);
	g_free(preview->release_name);
	g_free(preview->module);
	g_free(preview->state);
	g_free(preview->refusal);
	g_free(preview->volume_name);
	g_free(preview->path);
	g_free(preview->client_path);
	g_clear_pointer(&preview->placed, g_ptr_array_unref);
	g_free(preview->digest);
	g_free(preview);
}

static gint64
int_of(
	AxisDbResult *rows,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (axis_db_result_is_null(rows, row, column) || !axis_db_result_get_int64(rows, row, column, &value))
		return 0;
	return value;
}

/* What the confirmation showed, as one value: the import's state, the
 * download's state and identifier at its client, the place, what is there,
 * who deletes, and the count and size. */
static gchar *
digest_of(
	const gchar                 *import_state,
	const AxisImportFolderLook  *look,
	AxisImportDeleteBy           by
){
	g_autofree gchar *text = g_strdup_printf("%s\n%s\n%s\n%s\n%" G_GINT64_FORMAT "\n%s\n%s\n%" G_GUINT64_FORMAT
		"\n%d\n%d\n%u\n%" G_GUINT64_FORMAT, import_state != NULL ? import_state : "", look->download_state != NULL ?
		look->download_state : "", look->client_ref != NULL ? look->client_ref : "", look->state != NULL ? look->state :
		"", look->volume, look->path != NULL ? look->path : "", look->client_path != NULL ? look->client_path : "",
		look->inode, (gint)by, look->file ? 1 : 0, look->n_files, look->bytes);

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

gboolean
axis_import_delete_preview(
	AxisImport                *self,
	gint64                     import_id,
	AxisImportDeletePreview  **preview_out,
	GError                   **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisDbResult) job = NULL;
	g_autoptr(AxisDbResult) placed = NULL;
	g_autoptr(AxisDbResult) volume = NULL;
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_auto(AxisImportFolderLook) look = AXIS_IMPORT_FOLDER_LOOK_INIT;
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *volume_text = NULL;
	const gchar *state;
	const gchar *params[1];
	gint64 download_id;
	guint i;

	g_return_val_if_fail(self != NULL && preview_out != NULL, FALSE);

	*preview_out = NULL;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT i.state, i.module, i.download_id, i.title_label, i.release_name,"
	        " i.owner_id FROM import i WHERE i.id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_NOT_FOUND, "There is no such import.");
		return FALSE;
	}
	state = axis_db_result_get_text(row, 0, 0);
	download_id = int_of(row, 0, 2);
	/* The states a dismissal takes, and none a job is placing. */
	if (g_strcmp0(state, "running") == 0) {
		if (!axis_db_pool_exec(self->pool, "SELECT 1 FROM job WHERE dedupe_key = '" AXIS_IMPORT_KEY_PREFIX "' || $1"
		        " AND state = 'running'", 1, params, &job, error))
			return FALSE;
		if (axis_db_result_get_n_rows(job) > 0) {
			g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
				"A job is placing that import's files now; wait for it to finish.");
			return FALSE;
		}
		state = "waiting";
	}
	if (g_strcmp0(state, "blocked") != 0 && g_strcmp0(state, "waiting") != 0 && g_strcmp0(state, "partial") != 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED,
			"That import is not waiting for a decision.");
		return FALSE;
	}
	if (download_id <= 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED, "No download of Axis's brought that "
			"import's files (it was opened by hand for a folder), so there is no download to delete. A folder in a "
			"download client's folder for Axis that nothing accounts for can be deleted from the Leftovers page.");
		return FALSE;
	}
	if (!axis_import_download_folder_look(self, download_id, import_id, &look, error))
		return FALSE;

	preview = g_new0(AxisImportDeletePreview, 1);
	preview->import_id = import_id;
	preview->state = g_strdup(state);
	preview->module = g_strdup(axis_db_result_get_text(row, 0, 1));
	preview->title_label = g_strdup(axis_db_result_get_text(row, 0, 3));
	preview->release_name = g_strdup(axis_db_result_get_text(row, 0, 4));
	{
		gint64 grab_id = 0;

		if (!axis_import_grab_of(self, preview->module, download_id, int_of(row, 0, 5), &grab_id, error))
			return FALSE;
		preview->has_grab = grab_id > 0;
	}
	preview->by = look.client_deletes ? AXIS_IMPORT_DELETE_BY_CLIENT : look.at_client ?
		AXIS_IMPORT_DELETE_BY_AXIS_KEPT : AXIS_IMPORT_DELETE_BY_AXIS_GONE;
	preview->gone = g_strcmp0(look.state, "gone") == 0;
	/* "Nothing is at its place" lets the import be closed only for a download
	 * whose client will not be asked to delete anything: never for a torrent
	 * its client still lists, which the look refuses ("left") already. */
	preview->allowed = look.state == NULL || (preview->gone && !look.client_deletes);
	if (preview->gone && !preview->allowed) {
		g_free(look.text);
		look.text = g_strdup("the torrent is no longer where Axis recorded it, and its client still lists it. It was "
			"probably moved in the client. Axis will not delete it: remove it in the client, or dismiss the import");
		g_clear_pointer(&look.client_path, g_free);
	}
	preview->waits = g_strcmp0(look.state, "waiting") == 0;
	preview->file = look.file;
	preview->n_files = look.n_files;
	preview->bytes = look.bytes;
	preview->path = look.path != NULL ? axis_library_name_display(look.path) : NULL;
	preview->client_path = g_strdup(look.client_path);
	if (!preview->allowed) {
		g_autofree gchar *where = look.client_path != NULL ?
			g_strdup_printf(" The files are at %s at the client.", look.client_path) : g_strdup("");
		const gchar *why = look.text != NULL ? look.text : "Axis's rules do not let it be deleted";
		gsize n = strlen(why);

		preview->refusal = preview->waits ?
			g_strdup_printf("It cannot be deleted now: %s%s Nothing was changed. Try again once that has passed.", why,
				n > 0 && why[n - 1] == '.' ? "" : ".") :
			g_strdup_printf("Axis does not delete this download: %s%s%s Nothing was changed. \"Dismiss this import\" "
				"on the import's page closes the import and leaves the files where they are.", why,
				n > 0 && why[n - 1] == '.' ? "" : ".", where);
	}
	if (look.volume > 0) {
		const gchar *more[1];

		volume_text = axis_import_int_text(look.volume);
		more[0] = volume_text;
		if (!axis_db_pool_exec(self->pool, "SELECT name FROM volume WHERE id = $1::bigint", 1, more, &volume, error))
			return FALSE;
		if (axis_db_result_get_n_rows(volume) > 0)
			preview->volume_name = g_strdup(axis_db_result_get_text(volume, 0, 0));
	}
	/* What this import already placed in the library: it stays.  A file
	 * placed by a hard link is another name of a file of the download: the
	 * download's name goes, the library's name and the content stay. */
	preview->placed = g_ptr_array_new_with_free_func(g_free);
	if (!axis_db_pool_exec(self->pool, "SELECT f.dest_dir, f.dest_name, f.mode, r.name FROM import_file f"
	        " JOIN import i ON i.id = f.import_id LEFT JOIN library_root r ON r.id = i.root_id"
	        " WHERE f.import_id = $1::bigint AND f.library_file_id IS NOT NULL AND f.dest_name IS NOT NULL"
	        " ORDER BY f.position LIMIT 200", 1, params, &placed, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(placed); i++) {
		const gchar *dir = axis_db_result_get_text(placed, i, 0);
		g_autofree gchar *stored = dir != NULL && dir[0] != '\0' ?
			g_strconcat(dir, "/", axis_db_result_get_text(placed, i, 1), NULL) :
			g_strdup(axis_db_result_get_text(placed, i, 1));
		g_autofree gchar *shown = axis_library_name_display(stored);
		const gchar *root = axis_db_result_get_text(placed, i, 3);

		g_ptr_array_add(preview->placed, root != NULL ? g_strdup_printf("%s, in %s", shown, root) : g_strdup(shown));
		if (g_strcmp0(axis_db_result_get_text(placed, i, 2), "hardlink") == 0)
			preview->n_linked++;
	}
	preview->digest = digest_of(state, &look, preview->by);
	*preview_out = g_steal_pointer(&preview);
	return TRUE;
}

/* ── The step ───────────────────────────────────────────────────── */

static gboolean
set_removal(
	AxisImport   *self,
	gint64        import_id,
	const gchar  *state,
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[3];

	params[0] = id;
	params[1] = state;
	params[2] = text;
	return axis_db_pool_exec(self->pool, "UPDATE import SET client_removal = $2, client_removal_text = left($3, 1000),"
		" updated_at = now() WHERE id = $1::bigint AND state = 'dismissed' AND reason = 'download_deleted'", 3, params,
		NULL, error);
}

gboolean
axis_import_delete_with_data(const AxisImportFolderLook *look)
{
	/* Listed by its client now; something is at the recorded place; no rule
	 * refuses and nothing waits. */
	if (look == NULL || look->state != NULL || !look->at_client || look->inode == 0)
		return FALSE;
	/* A torrent's client deletes it wherever it is: only when it says it is
	 * at the recorded place. */
	if (look->torrent)
		return look->client_deletes && look->client_agrees;
	return TRUE;
}

/*
 * The one place in this file where a client is asked to remove a download:
 * with its data only under axis_import_delete_with_data(), read from @look,
 * which was made just before; without its data otherwise.  The first post,
 * the hourly retry and whatever calls the step later all pass here.
 */
static gboolean
ask_client(
	AxisImport                  *self,
	gint64                       download_id,
	const AxisImportFolderLook  *look,
	gboolean                    *with_data,
	gchar                      **left,
	GError                     **error
){
	*with_data = axis_import_delete_with_data(look);
	/* (The downloads layer's own place rule holds beneath this one: *@left
	 * when it removed a torrent without its data after all.) */
	return axis_downloads_remove_full(self->downloads, download_id, *with_data, left, error);
}

static gboolean
step_one(
	AxisImport               *self,
	gint64                    import_id,
	gint64                    download_id,
	AxisImportCleanupReport  *report,
	GError                  **error
){
	g_auto(AxisImportFolderLook) look = AXIS_IMPORT_FOLDER_LOOK_INIT;
	g_autoptr(AxisDbResult) after = NULL;
	g_autofree gchar *download = axis_import_int_text(download_id);
	g_autofree gchar *id = axis_import_int_text(import_id);
	g_autofree gchar *text = NULL;
	const gchar *outcome = "done";
	const gchar *params[3];
	const gchar *folder;
	gboolean axis_deletes;

	if (download_id <= 0)
		return set_removal(self, import_id, "kept", "Nothing was deleted: Axis no longer has the record of this "
			"import's download.", error);
	/* The place, judged again just before anything is asked: by the removal's
	 * own deciding half, which writes nothing. */
	if (!axis_import_download_folder_look(self, download_id, 0, &look, error))
		return FALSE;
	if (g_strcmp0(look.state, "left") == 0) {
		text = g_strdup_printf("Nothing was deleted, and the client was not asked: %s.%s%s%s", look.text != NULL ?
			look.text : "Axis's rules do not let it be deleted", look.client_path != NULL ? " The files are at " : "",
			look.client_path != NULL ? look.client_path : "", look.client_path != NULL ? " at the client." : "");
		return set_removal(self, import_id, "kept", text, error);
	}
	if (g_strcmp0(look.state, "waiting") == 0) {
		gsize n = look.text != NULL ? strlen(look.text) : 0;

		text = g_strdup_printf("Not deleted yet, and the client was not asked: %s%s Axis looks again every hour.",
			look.text != NULL ? look.text : "it cannot be looked at now", n > 0 && look.text[n - 1] == '.' ? "" : ".");
		return set_removal(self, import_id, "failed", text, error);
	}

	/* A torrent its client lists that may not go with its data (its place went
	 * between the two halves of the look, which says "left" for it already):
	 * never removed without its data either, which would leave its files with
	 * nothing that knows them. */
	if (look.torrent && look.at_client && !axis_import_delete_with_data(&look))
		return set_removal(self, import_id, "kept", "Nothing was deleted, and the client was not asked: the torrent "
			"is no longer where Axis recorded it, and its client still lists it. It was probably moved in the client. "
			"Remove it in the client.", error);

	/* The client: with its data under the one rule, and without otherwise. */
	if (g_strcmp0(look.download_state, "removed") != 0 && g_strcmp0(look.download_state, "gone") != 0) {
		g_autoptr(GError) local = NULL;
		g_autofree gchar *left = NULL;
		gboolean with_data = FALSE;

		if (ask_client(self, download_id, &look, &with_data, &left, &local)) {
			if (left != NULL) {
				/* In a library folder when the client was asked (moved there
				 * since the look): its files stay. */
				text = g_strdup_printf("The torrent was removed from its client, and nothing was deleted. %s", left);
				return set_removal(self, import_id, "kept", text, error);
			}
			if (look.torrent && !with_data) {
				/* It did not list the torrent at the look, and had it when asked. */
				return set_removal(self, import_id, "kept", "The torrent was removed from its client, which kept its "
					"files: the client did not list it when Axis looked, so Axis did not ask for its files to be "
					"deleted. They are where the client had them, and can be deleted there by hand.", error);
			}
			text = g_strdup(look.torrent ? "The torrent was removed from its client with its downloaded files." :
				"The job was removed from its client.");
			if (report != NULL)
				report->removals++;
		} else if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE)) {
			outcome = "gone";
			text = g_strdup("It was already gone from its client: the client said it no longer has it, and deleted "
				"none of its files.");
		} else if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS)) {
			text = g_strdup_printf("It was left with its client, untouched: %s. Its files there are not Axis's to "
				"delete.", local->message);
			return set_removal(self, import_id, "kept", text, error);
		} else if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID)) {
			g_autoptr(AxisDownloadInfo) info = NULL;

			/* Removed or gone in the same moment, by something else. */
			if (axis_downloads_get(self->downloads, download_id, &info, NULL) && info != NULL &&
			    (info->state == AXIS_DOWNLOAD_REMOVED || info->state == AXIS_DOWNLOAD_GONE)) {
				outcome = "gone";
				text = g_strdup("It was already gone from its client.");
			} else {
				text = g_strdup_printf("It was left with its client: %s", local->message);
				return set_removal(self, import_id, "kept", text, error);
			}
		} else {
			text = g_strdup_printf("Its client could not remove it yet (asked again every hour): %s", local->message);
			return set_removal(self, import_id, "failed", text, error);
		}
	} else {
		outcome = "gone";
		text = g_strdup("It was already gone from its client, which deleted none of its files.");
	}
	if (!set_removal(self, import_id, outcome, text, error))
		return FALSE;

	/* What the client kept (a Usenet job's folder), or left when it no longer
	 * had the download, is Axis's to delete, by the removal of a download's
	 * leftover and under its rules; what a torrent's client deleted itself is
	 * not looked at. */
	axis_deletes = !look.torrent || strcmp(outcome, "gone") == 0;
	params[0] = download;
	if (axis_deletes) {
		if (!axis_db_pool_exec(self->pool, "UPDATE download SET folder_removal = 'pending', folder_removal_text = NULL,"
		        " updated_at = now() WHERE id = $1::bigint AND state IN ('removed', 'gone') AND folder_removal IS NULL",
		        1, params, NULL, error) ||
		    !axis_import_download_folder_removals(self, download_id, report, error))
			return FALSE;
	}
	/* The download's files a move across volumes copied and had still to
	 * delete (import_file.source_state pending): no run looks at a dismissed
	 * import again, so what became of them is recorded here. */
	if (!axis_db_pool_exec(self->pool, "SELECT folder_removal FROM download WHERE id = $1::bigint", 1, params, &after,
	        error))
		return FALSE;
	folder = axis_db_result_get_n_rows(after) > 0 ? axis_db_result_get_text(after, 0, 0) : NULL;
	params[0] = id;
	if (!axis_deletes || g_strcmp0(folder, "done") == 0 || g_strcmp0(folder, "gone") == 0)
		return axis_db_pool_exec(self->pool, "UPDATE import_file SET source_state = 'gone', note = left(coalesce(note"
			" || ' ', '') || 'Its file in the download went with the download, which a person deleted.', 1000)"
			" WHERE import_id = $1::bigint AND source_state = 'pending'", 1, params, NULL, error);
	if (g_strcmp0(folder, "left") == 0)
		return axis_db_pool_exec(self->pool, "UPDATE import_file SET source_state = 'kept', note = left(coalesce(note"
			" || ' ', '') || 'Its file in the download was left with the download, which a rule kept.', 1000)"
			" WHERE import_id = $1::bigint AND source_state = 'pending'", 1, params, NULL, error);
	return TRUE;
}

/*
 * The claim on an import's step: a session advisory lock of PostgreSQL's,
 * keyed by the import, held on a connection of its own for as long as the
 * step runs (no transaction is left open while a client is asked).  *@claim
 * is %NULL when another step holds it.  A daemon that dies lets go of it with
 * its connection.
 */
static gboolean
step_claim(
	AxisImport   *self,
	gint64        import_id,
	AxisDbConn  **claim,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	AxisDbConn *conn;

	*claim = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock(hashtext('" AXIS_IMPORT_DELETE_CLAIM "'),"
	        " ($1::bigint % 2147483647)::integer)", 1, params, &row, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(row) == 0 || g_strcmp0(axis_db_result_get_text(row, 0, 0), "t") != 0) {
		axis_db_pool_release(self->pool, conn);
		return TRUE;
	}
	*claim = conn;
	return TRUE;
}

static void
step_release(
	AxisImport  *self,
	AxisDbConn  *claim,
	gint64       import_id
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];

	params[0] = id;
	/* A session's lock outlives a statement: let go of it before the
	 * connection goes back to the pool. */
	if (!axis_db_conn_exec(claim, "SELECT pg_advisory_unlock(hashtext('" AXIS_IMPORT_DELETE_CLAIM "'),"
	        " ($1::bigint % 2147483647)::integer)", 1, params, NULL, &local))
		g_warning("import %" G_GINT64_FORMAT ": the claim on its download's deletion could not be let go: %s",
			import_id, local->message);
	axis_db_pool_release(self->pool, claim);
}

gboolean
axis_import_deleted_downloads(
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
	/* Only the requests of "Delete this download": never a dismissed import
	 * of another reason, whose download stays where it is. */
	if (!axis_db_pool_exec(self->pool, "SELECT i.id, i.download_id FROM import i WHERE i.state = 'dismissed'"
	        " AND i.reason = 'download_deleted' AND i.client_removal IN ('pending', 'failed')"
	        " AND ($1::bigint = 0 OR i.id = $1::bigint) ORDER BY i.id LIMIT 100", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisDbResult) still = NULL;
		g_autofree gchar *one = axis_import_int_text(int_of(rows, i, 0));
		AxisDbConn *claim = NULL;
		gboolean ok;

		/* One step for an import at a time: the post's and the hourly cleanup's
		 * can come for the same import together, and the second must not ask
		 * the client, or look at the disk, beside the first.  What is claimed
		 * elsewhere is left to its claimant (and, should that die, to the next
		 * hour). */
		if (!step_claim(self, int_of(rows, i, 0), &claim, error))
			return FALSE;
		if (claim == NULL)
			continue;
		/* Read again under the claim: the other step may have just finished. */
		params[0] = one;
		ok = axis_db_pool_exec(self->pool, "SELECT i.download_id FROM import i WHERE i.id = $1::bigint"
			" AND i.state = 'dismissed' AND i.reason = 'download_deleted' AND i.client_removal IN ('pending', 'failed')",
			1, params, &still, error);
		if (ok && axis_db_result_get_n_rows(still) > 0)
			ok = step_one(self, int_of(rows, i, 0), int_of(still, 0, 0), report, error);
		step_release(self, claim, int_of(rows, i, 0));
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

/* ── The request ────────────────────────────────────────────────── */

gboolean
axis_import_delete_download(
	AxisImport               *self,
	gint64                    import_id,
	const gchar              *digest,
	gboolean                  block,
	const gchar              *user,
	AxisImportDeleteOutcome  *outcome,
	GError                  **error
){
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_import_int_text(import_id);
	const gchar *params[1];
	const gchar *client;
	const gchar *folder;
	gboolean torrent;

	g_return_val_if_fail(self != NULL && user != NULL && outcome != NULL, FALSE);

	*outcome = AXIS_IMPORT_DELETE_WAITING;
	/* The decision, made again: the post never trusts the page. */
	if (!axis_import_delete_preview(self, import_id, &preview, error))
		return FALSE;
	if (!preview->allowed) {
		g_set_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED, "%s", preview->refusal);
		return FALSE;
	}
	if (digest == NULL || strcmp(digest, preview->digest) != 0) {
		g_set_error_literal(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_CHANGED, "The download, its place or the "
			"import changed since this page was made. Nothing was deleted. Read what is so now, and confirm again.");
		return FALSE;
	}
	/* The request, recorded; then the client and the disk. */
	if (!axis_import_close_deleted(self, import_id, preview->state, user, block, NULL, error) ||
	    !axis_import_deleted_downloads(self, import_id, NULL, error))
		return FALSE;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, "SELECT i.client_removal, d.folder_removal, d.protocol = 'torrent' FROM import i"
	        " LEFT JOIN download d ON d.id = i.download_id WHERE i.id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0)
		return TRUE;
	client = axis_db_result_get_text(row, 0, 0);
	folder = axis_db_result_get_text(row, 0, 1);
	torrent = g_strcmp0(axis_db_result_get_text(row, 0, 2), "t") == 0;
	if (g_strcmp0(client, "kept") == 0 || g_strcmp0(folder, "left") == 0)
		*outcome = AXIS_IMPORT_DELETE_LEFT;
	else if (g_strcmp0(client, "pending") == 0 || g_strcmp0(client, "failed") == 0 ||
	    g_strcmp0(folder, "pending") == 0 || g_strcmp0(folder, "waiting") == 0)
		*outcome = AXIS_IMPORT_DELETE_WAITING;
	else if (g_strcmp0(folder, "gone") == 0)
		*outcome = AXIS_IMPORT_DELETE_GONE;
	else if (g_strcmp0(folder, "done") == 0 || (torrent && g_strcmp0(client, "done") == 0))
		*outcome = AXIS_IMPORT_DELETE_DELETED;
	else
		*outcome = AXIS_IMPORT_DELETE_WAITING;
	return TRUE;
}
