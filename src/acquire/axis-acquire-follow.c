/*
 * axis-acquire-follow.c - after the grab: failures, stalls and downloads
 * that are gone, completions, claims that were never handed over (the
 * module's follow-up job, every minute)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The follow-up works from facts alone, so that a restart, a missed change
 * or two runs at once change nothing: each grab it acts on is re-read with
 * its owner's row locked, and every action checks the grab is still in the
 * state it was found in.
 *
 *  - A download that failed, is gone, or stalled for longer than the
 *    module's give-up setting: its release is put on the owner's blocklist
 *    (by its info-hash when known, and by its name), the grab is closed, its
 *    download is removed from the client with its data, and once nothing of
 *    it is at the client any more the module queues one search for the next
 *    best (none for a release a person chose by hand from a manual search:
 *    the history says to choose again).  A removal that fails is tried again
 *    at every run; the grab counts as in flight meanwhile (the module's
 *    facts say so).
 *  - A download that was removed from its client by something else closes
 *    its grab, with no blocklist entry: it is wanted again.
 *  - A completed download whose files something else deleted or moved
 *    before their import (its import ended "files_gone") closes its grab
 *    the same way, is removed from its client without its data, and the
 *    next best is looked for when the title is monitored.
 *  - A completed download records the completion once.
 *  - A claim that was never handed to a client (the daemon stopped between
 *    the claim and the send) is settled: the download that was sent for it
 *    is adopted if there is one, otherwise it is closed.
 */

#include "acquire/axis-acquire-private.h"

#include <string.h>

#define US AXIS_ACQUIRE_US
#define LIVE_STATES AXIS_ACQUIRE_LIVE_STATES

static gint64
row_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

/* ── Dealing with a failed grab ─────────────────────────────────── */

static gboolean
enqueue_retry(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean      if_monitored,
	gboolean     *queued,
	GError      **error
){
	g_autofree gchar *grab = axis_acquire_int_text(grab_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "UPDATE %G SET retry_at = now() WHERE id = $1::bigint"
		" AND retry_at IS NULL RETURNING id, coalesce(set_aside = 'hand', false), release_name");
	g_autoptr(AxisDbResult) marked = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	const gchar *params[1];
	gboolean by_hand = FALSE;
	gboolean ok;

	*queued = FALSE;
	if (conn == NULL)
		return FALSE;
	params[0] = grab;
	/* Once per grab. */
	ok = axis_db_conn_begin(conn, error) && axis_db_conn_exec(conn, sql, 1, params, &marked, error);
	if (ok && axis_db_result_get_n_rows(marked) > 0)
		axis_db_result_get_boolean(marked, 0, 1, &by_hand);
	/* A release a person chose by hand from a manual search: the next one is
	 * theirs to choose too, so no search follows it (a search at the manual
	 * search's level would take what the profile says, which they set
	 * aside).  The history says so. */
	if (ok && by_hand) {
		ok = axis_acquire_eventf(self, conn, owner_id, NULL, "skipped", user, 0, grab_id, error, "No search follows "
			"the failed grab by hand of %s: it was chosen from a manual search, so the next release is chosen by "
			"hand too. Use Manual search again, or wait for the scheduled search.", axis_db_result_get_text(marked, 0,
			2) != NULL ? axis_db_result_get_text(marked, 0, 2) : "the release");
	} else if (ok && axis_db_result_get_n_rows(marked) > 0) {
		gboolean monitored = TRUE;

		/* Per its monitoring: one that is not monitored is wanted again, and
		 * looked for when a person asks. */
		if (if_monitored && self->info->monitored != NULL)
			ok = self->info->monitored(self->data, conn, owner_id, grab_id, &monitored, error);
		if (ok && monitored) {
			ok = self->info->retry(self->data, conn, owner_id, grab_id, user, error);
			*queued = ok;
		}
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		*queued = FALSE;
	return ok;
}

/* Asks the client to remove @download_id with its data (without it, with
 * @keep_data), and records how it went on the grab.  *@gone: nothing of it
 * is at the client now.
 *
 * What a removal with its data relies on: the download is one Axis itself
 * sent (the downloads layer removes only a record of its own, by the
 * client's identifier, and a torrent a person put in another category or
 * label is left alone), and a client deletes only that download's own
 * files.  A Usenet client deletes an unfinished job from its own temporary
 * folder.  A torrent's client deletes the torrent wherever it is now, which
 * a person can change in the client.  With @judge_place (a grab a person
 * replaced), the place is looked at first, here, where the client is asked,
 * the first time and every time the follow-up asks again
 * (axis_acquire_removal_place()): a place Axis does not let the client
 * delete, or cannot ask about, leaves the removal failed with the reason, and
 * the client is not asked.  Without @judge_place (every other removal with
 * its data), only a place known to be a library folder or the top of a disk
 * changes anything: the torrent is removed without its data, and that its
 * files were left is recorded on the grab and on the download; a place that
 * cannot be asked about or judged is removed with its data, as before.
 * With @kept (nullable; the follow-up, for a
 * download that finished while its removal waited): a place that is refused
 * ends the removal as "kept" instead, with the words on the grab and on the
 * download (the Downloads page), and *@kept says so.  *@why: (nullable)
 * then, or when the client could not remove it, the words. */
static gboolean
remove_download(
	AxisAcquire             *self,
	gint64                   grab_id,
	gint64                   download_id,
	gboolean                 keep_data,
	gboolean                 judge_place,
	gboolean                *gone,
	gboolean                *kept,
	gchar                  **why,
	AxisAcquireFollowReport *report,
	GError                 **error
){
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autofree gchar *grab = axis_acquire_int_text(grab_id);
	g_autofree gchar *text = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *sql = axis_acquire_sql(self, "UPDATE %G SET removal = $2, removal_text = left($3, 1000)"
		" WHERE id = $1::bigint");
	const gchar *params[3];
	gboolean removed;

	*gone = FALSE;
	if (kept != NULL)
		*kept = FALSE;
	if (why != NULL)
		*why = NULL;
	if (judge_place && !keep_data) {
		AxisAcquirePlace place = AXIS_ACQUIRE_PLACE_UNKNOWN;
		g_autofree gchar *place_why = NULL;

		if (!axis_acquire_removal_place(self, download_id, &place, &place_why, error))
			return FALSE;
		if (place == AXIS_ACQUIRE_PLACE_REFUSED && kept != NULL) {
			/* It finished downloading while its removal waited, where the
			 * client is not asked to delete: nobody imports it (its grab is
			 * closed), and asking again changes nothing.  The removal ends
			 * here, the download stays at its client, and the page says so. */
			text = g_strdup_printf("It was to be replaced, could not be removed, and has finished downloading at its "
				"client, where it still is. Axis does not ask the client to delete it: %s Axis does not import it and "
				"does not ask again: remove it in the client.", place_why != NULL ? place_why :
				"where it is now cannot be judged.");
			g_message("%s: download %" G_GINT64_FORMAT " is left at its client: %s", self->info->module, download_id,
				text);
			params[0] = grab;
			params[1] = "kept";
			params[2] = text;
			if (!axis_db_pool_exec(self->pool, sql, 3, params, NULL, error) ||
			    !axis_downloads_note_left(self->downloads, download_id, text, error))
				return FALSE;
			*kept = TRUE;
			if (why != NULL)
				*why = g_steal_pointer(&text);
			return TRUE;
		}
		if (place == AXIS_ACQUIRE_PLACE_REFUSED || place == AXIS_ACQUIRE_PLACE_UNKNOWN) {
			text = place == AXIS_ACQUIRE_PLACE_REFUSED ?
				g_strdup_printf("Axis does not ask the client to delete it: %s", place_why != NULL ? place_why :
					"where it is now cannot be judged.") :
				g_strdup_printf("The client could not be asked where it is: %s", place_why != NULL ? place_why :
					"no reason");
			if (report != NULL)
				report->removal_failed++;
			g_message("%s: download %" G_GINT64_FORMAT " is not removed from its client: %s", self->info->module,
				download_id, text);
			params[0] = grab;
			params[1] = "failed";
			params[2] = text;
			if (!axis_db_pool_exec(self->pool, sql, 3, params, NULL, error))
				return FALSE;
			if (why != NULL)
				*why = g_steal_pointer(&text);
			return TRUE;
		}
		/* Not listed by its client: asked without its data, which deletes
		 * nothing and tells that it is gone. */
		if (place == AXIS_ACQUIRE_PLACE_WITHOUT_DATA)
			keep_data = TRUE;
	}
	/* Every removal with its data (a failure, a stall, Give up, Block and
	 * search again, and a replace whose place was let through above) has the
	 * positive half of the place rule from the downloads layer, which every
	 * caller of it passes through: a torrent its client says is in a library
	 * folder now (or at the top of a disk) is removed from the client without
	 * its data, its files are left, and @left says so.  A place that cannot
	 * be asked about or judged (the client does not answer, no mapping
	 * reaches it) changes nothing: the removal is with its data, as it always
	 * was, so that a failed download in a client's own folder is still
	 * cleaned up. */
	removed = axis_downloads_remove_full(self->downloads, download_id, !keep_data, &left, &failure);
	if (!removed && (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
	    g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
	    g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE) ||
	    g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS))) {
		/* Removed or gone already, or never at the client, or a person's own
		 * now (moved to another category): nothing of Axis's is left. */
		if (axis_downloads_get(self->downloads, download_id, &info, NULL) && info != NULL &&
		    !axis_download_state_is_active(info->state)) {
			removed = TRUE;
			/* (Nothing was removed by this call: nothing to say of its files.) */
			g_clear_pointer(&left, g_free);
		}
	}
	if (removed) {
		*gone = TRUE;
		if (report != NULL)
			report->removed++;
		/* Its files left in a library folder: on the grab (the downloads layer
		 * recorded it on the download, for the Downloads page). */
		if (left != NULL) {
			g_message("%s: download %" G_GINT64_FORMAT ": %s", self->info->module, download_id, left);
			text = g_steal_pointer(&left);
		}
	} else {
		text = g_strdup_printf("The client could not remove it: %s", failure != NULL ? failure->message : "no reason");
		if (report != NULL)
			report->removal_failed++;
		g_message("%s: download %" G_GINT64_FORMAT " could not be removed from its client: %s", self->info->module,
			download_id, failure != NULL ? failure->message : "no reason");
	}
	params[0] = grab;
	params[1] = removed ? "done" : "failed";
	params[2] = text;
	if (!axis_db_pool_exec(self->pool, sql, 3, params, NULL, error))
		return FALSE;
	if (why != NULL)
		*why = g_steal_pointer(&text);
	return TRUE;
}

gboolean
axis_acquire_handle_failure(
	AxisAcquire               *self,
	gint64                     owner_id,
	gint64                     grab_id,
	const AxisAcquireFailure  *failure,
	const gchar               *user,
	gboolean                   completed_too,
	gboolean                  *handled,
	AxisAcquireFollowReport   *report,
	GError                   **error
){
	return axis_acquire_handle_failure_full(self, owner_id, grab_id, failure, user, completed_too, handled, NULL, NULL,
		report, error);
}

gboolean
axis_acquire_handle_failure_full(
	AxisAcquire               *self,
	gint64                     owner_id,
	gint64                     grab_id,
	const AxisAcquireFailure  *failure,
	const gchar               *user,
	gboolean                   completed_too,
	gboolean                  *handled,
	gboolean                  *gone_out,
	gchar                    **why_out,
	AxisAcquireFollowReport   *report,
	GError                   **error
){
	g_autoptr(AxisDbResult) grab = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *grab_text = axis_acquire_int_text(grab_id);
	g_autofree gchar *name_key = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *release = NULL;
	g_autofree gchar *indexer = NULL;
	g_autofree gchar *whose = g_strdup_printf("%s%s", self->info->noun,
		g_str_has_suffix(self->info->noun, "s") ? "'" : "'s");
	g_autofree gchar *select_sql = axis_acquire_sql(self, "SELECT g.name_key, coalesce(g.info_hash, d.info_hash),"
		" g.release_name, g.indexer, g.download_id, d.state, d.client_ref IS NOT NULL,"
		/* It has arrived: completed, or an import of its download exists. */
		" g.completed_at IS NOT NULL OR coalesce(d.state IN ('completed', 'imported'), false)"
		"  OR EXISTS (SELECT 1 FROM import i WHERE i.download_id = g.download_id),"
		" coalesce(d.protocol = 'torrent', false) FROM %G g"
		" LEFT JOIN download d ON d.id = g.download_id"
		" WHERE g.id = $1::bigint AND g.%O = $2::bigint AND g.closed_at IS NULL"
		" AND (g.completed_at IS NULL OR $3::boolean)");
	g_autofree gchar *close_sql = axis_acquire_sql(self, "UPDATE %G SET closed_at = now(), close_reason = $2,"
		" close_text = $3, removal = $4 WHERE id = $1::bigint AND closed_at IS NULL");
	const gchar *download_state;
	gint64 download_id = 0;
	gboolean found = FALSE;
	gboolean remove = FALSE;
	gboolean live = FALSE;
	gboolean gone = FALSE;
	gboolean queued = FALSE;
	gboolean torrent = FALSE;
	const gchar *params[5];
	AxisDbConn *conn;
	gboolean ok;

	*handled = FALSE;
	if (gone_out != NULL)
		*gone_out = FALSE;
	if (why_out != NULL)
		*why_out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_acquire_store_lock(self, conn, owner_id, &found, error);
	if (!ok || !found)
		goto done;
	params[0] = grab_text;
	params[1] = owner;
	params[2] = completed_too ? "true" : "false";
	ok = axis_db_conn_exec(conn, select_sql, 3, params, &grab, error);
	if (!ok || axis_db_result_get_n_rows(grab) == 0)
		goto done;
	/* A replace takes only a download that is still on its way: one that has
	 * arrived is its import's, and is not ended here (under the owner's lock,
	 * so that a completion recorded a moment ago is seen). */
	if (failure->still_downloading) {
		gboolean arrived = FALSE;

		axis_db_result_get_boolean(grab, 0, 7, &arrived);
		if (arrived)
			goto done;
	}
	name_key = g_strdup(axis_db_result_get_text(grab, 0, 0));
	hash = g_strdup(axis_db_result_get_text(grab, 0, 1));
	release = g_strdup(axis_db_result_get_text(grab, 0, 2));
	indexer = g_strdup(axis_db_result_get_text(grab, 0, 3));
	download_id = row_int(grab, 0, 4);
	download_state = axis_db_result_get_text(grab, 0, 5);
	axis_db_result_get_boolean(grab, 0, 8, &torrent);
	live = download_state != NULL && (strcmp(download_state, "sending") == 0 || strcmp(download_state, "queued") == 0 ||
		strcmp(download_state, "downloading") == 0 || strcmp(download_state, "paused") == 0 ||
		strcmp(download_state, "completed") == 0);
	{
		gboolean has_ref = FALSE;

		axis_db_result_get_boolean(grab, 0, 6, &has_ref);
		/* Only what the client has can be removed from it. */
		remove = failure->remove && download_id > 0 && has_ref &&
			(live || g_strcmp0(download_state, "failed") == 0);
	}
	if (failure->block_reason != NULL)
		ok = axis_acquire_store_block(self, conn, owner_id, name_key, hash, release, indexer, failure->block_reason,
			failure->text, grab_id, user, error);
	params[0] = grab_text;
	params[1] = failure->close_reason;
	params[2] = failure->text;
	params[3] = remove ? "pending" : NULL;
	ok = ok && axis_db_conn_exec(conn, close_sql, 4, params, NULL, error);
	/* An import of it that waits, is blocked or is partly done is not wanted
	 * any more (one that is placing is left to finish: its files are the
	 * owner's; the files a partial one placed stay: this writes only its
	 * state, as a person's Dismiss does). */
	if (ok && download_id > 0) {
		g_autofree gchar *download = axis_acquire_int_text(download_id);
		const gchar *more[1];

		more[0] = download;
		ok = axis_db_conn_exec(conn, "UPDATE import SET state = 'dismissed', finished_at = now(), updated_at = now(),"
			" reason = 'grab_closed', reason_text = 'Its grab was closed (the download failed or was given up).'"
			" WHERE download_id = $1::bigint AND state IN ('waiting', 'blocked', 'partial')", 1, more, NULL, error);
	}
	ok = ok && axis_acquire_eventf(self, conn, owner_id, NULL, strcmp(failure->close_reason, "given_up") == 0 ||
		strcmp(failure->close_reason, "replaced") == 0 ?
		"abandoned" : strcmp(failure->close_reason, "removed") == 0 || strcmp(failure->close_reason, "files_gone") == 0 ?
		"removal" : "failed", user, 0, grab_id, error,
		"%s: %s%s%s%s%s", release, failure->text, failure->block_reason != NULL ? " It is on the " : "",
		failure->block_reason != NULL ? whose : "", failure->block_reason != NULL ? " blocklist now." : "",
		/* Written before the client is asked, so it says what holds either way:
		 * a torrent its client says is in a library folder goes without its
		 * data (remove_download(), which records what happened on the grab). */
		remove ? (failure->keep_data ? " Axis asks the client to remove it, without its data." :
		torrent ? " Axis asks the client to remove it, with its data unless the client says its files are in a library "
		"folder." : " Axis asks the client to remove it with its data.") : "");
	ok = ok && axis_db_conn_commit(conn, error);
	if (ok) {
		*handled = TRUE;
		if (report != NULL)
			report->failed++;
	}
done:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok || !*handled)
		return ok;

	/* Outside the transaction: the client, then the next best. */
	if (remove) {
		if (!remove_download(self, grab_id, download_id, failure->keep_data, failure->judge_place, &gone, NULL, why_out,
		        report, error))
			return FALSE;
	} else {
		gone = !live || download_id == 0;
	}
	if (gone_out != NULL)
		*gone_out = gone;
	/* (Files gone before their import: the next best is looked for whether
	 * or not the client could be asked, as nothing of it is to import.) */
	if (failure->retry && (gone || failure->keep_data)) {
		if (!enqueue_retry(self, owner_id, grab_id, user, failure->retry_if_monitored, &queued, error))
			return FALSE;
		if (queued && report != NULL)
			report->retries++;
	}
	return TRUE;
}

gboolean
axis_acquire_grab_deleted(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *release,
	const gchar  *user,
	gboolean      block,
	GError      **error
){
	g_autoptr(AxisDbResult) grab = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *grab_text = axis_acquire_int_text(grab_id);
	g_autofree gchar *whose = g_strdup_printf("%s%s", self->info->noun,
		g_str_has_suffix(self->info->noun, "s") ? "'" : "'s");
	g_autofree gchar *select_sql = axis_acquire_sql(self, "SELECT g.name_key, coalesce(g.info_hash, d.info_hash),"
		" g.release_name, g.indexer FROM %G g LEFT JOIN download d ON d.id = g.download_id"
		" WHERE g.id = $1::bigint AND g.%O = $2::bigint");
	/* No removal of the grab's own: the follow-up asks a client to remove the
	 * download of a grab whose removal is pending or failed, wherever its
	 * files are, and this download's place is judged first, by the import
	 * module (axis-import-delete.c). */
	g_autofree gchar *close_sql = axis_acquire_sql(self, "UPDATE %G SET closed_at = now(), close_reason = 'deleted',"
		" close_text = left($2, 1000) WHERE id = $1::bigint AND closed_at IS NULL");
	g_autofree gchar *text = g_strdup_printf("Its download was deleted by %s from its import's page.", user != NULL ?
		user : "an administrator");
	const gchar *name = release;
	const gchar *params[2];
	gboolean blocked = FALSE;

	if (grab_id > 0) {
		params[0] = grab_text;
		params[1] = owner;
		if (!axis_db_conn_exec(conn, select_sql, 2, params, &grab, error))
			return FALSE;
	}
	if (grab != NULL && axis_db_result_get_n_rows(grab) > 0) {
		if (axis_db_result_get_text(grab, 0, 2) != NULL)
			name = axis_db_result_get_text(grab, 0, 2);
		params[0] = grab_text;
		params[1] = text;
		if (!axis_db_conn_exec(conn, close_sql, 2, params, NULL, error))
			return FALSE;
		if (block) {
			g_autofree gchar *why = g_strdup_printf("Blocked by %s: its download was deleted from its import's page.",
				user != NULL ? user : "an administrator");

			if (!axis_acquire_store_block(self, conn, owner_id, axis_db_result_get_text(grab, 0, 0),
			        axis_db_result_get_text(grab, 0, 1), name, axis_db_result_get_text(grab, 0, 3),
			        AXIS_ACQUIRE_BLOCK_PERSON, why, grab_id, user, error))
				return FALSE;
			blocked = TRUE;
		}
	} else {
		grab_id = 0;
	}
	return axis_acquire_eventf(self, conn, owner_id, NULL, "removal", user, 0, grab_id, error,
		"%s: its download was deleted by %s from the import's page. The %s is wanted again; no search was started "
		"for it.%s%s%s", name != NULL ? name : "A download", user != NULL ? user : "an administrator", self->info->noun,
		blocked ? " The release is on the " : "", blocked ? whose : "", blocked ? " blocklist now." : "");
}

/* ── The follow-up ──────────────────────────────────────────────── */

static gboolean
finish_stale_searches(
	AxisAcquire  *self,
	GError      **error
){
	g_autofree gchar *after = axis_acquire_int_text(AXIS_ACQUIRE_SEARCH_STALE_S);
	g_autofree gchar *sql = axis_acquire_sql(self, "UPDATE %S s SET finished_at = now(), outcome = 'failed',"
		" outcome_text = 'The search did not finish: the daemon stopped, or the search failed.'"
		" WHERE s.finished_at IS NULL AND s.started_at < now() - $1::integer * interval '1 second'"
		" AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = %J AND j.state = 'running')");
	const gchar *params[1];

	params[0] = after;
	return axis_db_pool_exec(self->pool, sql, 1, params, NULL, error);
}

/* Older searches go, with their releases (grabs and history keep what they
 * said).  The latest automatic ones and the latest manual ones are kept
 * apart: a person's looks never push out what explains a title's state. */
/*
 * Not at every follow-up (every minute), and not all at once: once in
 * AXIS_ACQUIRE_PRUNE_EVERY_S (and at the first follow-up after a start), at
 * most AXIS_ACQUIRE_PRUNE_BATCH searches a statement, each statement its own
 * transaction, at most AXIS_ACQUIRE_PRUNE_BATCHES of them.  One statement
 * for everything old enough took a search's stored releases with it for
 * every search in one transaction: after a long stop that was 110,000
 * searches and their 280,000 releases in 4.5 s, with the rows locked until
 * it ended.  What a time leaves waits for the next.
 */
gboolean
axis_acquire_prune_searches(
	AxisAcquire  *self,
	gboolean      now,
	guint        *deleted_out,
	GError      **error
){
	g_autofree gchar *kept = axis_acquire_int_text(AXIS_ACQUIRE_SEARCHES_KEPT);
	g_autofree gchar *after = axis_acquire_int_text(AXIS_ACQUIRE_SEARCH_KEEP_S);
	g_autofree gchar *batch = axis_acquire_int_text(AXIS_ACQUIRE_PRUNE_BATCH);
	g_autofree gchar *sql = axis_acquire_sql(self, "WITH gone AS (DELETE FROM %S WHERE id IN (SELECT s.id FROM %S s"
		" WHERE s.finished_at < now() - $2::integer * interval '1 second' AND s.id NOT IN (SELECT k.id FROM %S k"
		" WHERE k.%O = s.%O AND (k.cause = 'list') = (s.cause = 'list') ORDER BY k.id DESC LIMIT $1::integer)"
		" LIMIT $3::integer) RETURNING 1) SELECT count(*) FROM gone");
	const gchar *params[3];
	gint64 at = g_get_monotonic_time();
	guint deleted = 0;
	guint i;

	if (deleted_out != NULL)
		*deleted_out = 0;
	g_mutex_lock(&self->hook_lock);
	if (!now && self->prune_after_us != 0 && at < self->prune_after_us) {
		g_mutex_unlock(&self->hook_lock);
		return TRUE;
	}
	g_mutex_unlock(&self->hook_lock);

	params[0] = kept;
	params[1] = after;
	params[2] = batch;
	for (i = 0; i < AXIS_ACQUIRE_PRUNE_BATCHES; i++) {
		g_autoptr(AxisDbResult) result = NULL;
		gint64 n = 0;

		if (!axis_db_pool_exec(self->pool, sql, 3, params, &result, error))
			return FALSE;
		axis_db_result_get_int64(result, 0, 0, &n);
		deleted += (guint)MAX(n, 0);
		if (n < AXIS_ACQUIRE_PRUNE_BATCH)
			break;
	}
	/* The hour is spent only by a time that worked: after a failure the next
	 * follow-up tries again. */
	g_mutex_lock(&self->hook_lock);
	self->prune_after_us = at + (gint64)AXIS_ACQUIRE_PRUNE_EVERY_S * G_USEC_PER_SEC;
	g_mutex_unlock(&self->hook_lock);
	if (deleted_out != NULL)
		*deleted_out = deleted;
	return TRUE;
}

/* A claim the daemon never handed over: adopt the download it sent, if
 * any (the downloads layer records before it sends), or close it. */
static gboolean
settle_claim(
	AxisAcquire              *self,
	gint64                    owner_id,
	gint64                    grab_id,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) grab = NULL;
	g_autoptr(AxisDbResult) download = NULL;
	g_autofree gchar *grab_text = axis_acquire_int_text(grab_id);
	g_autofree gchar *grab_sql = axis_acquire_sql(self, "SELECT g.release_name, o.title_id, " US("g.created_at")
		" FROM %G g JOIN %T o ON o.id = g.%O WHERE g.id = $1::bigint AND g.closed_at IS NULL"
		" AND g.download_id IS NULL");
	g_autofree gchar *download_sql = axis_acquire_sql(self, "SELECT id FROM download WHERE module = '%M'"
		" AND title_id = $1::bigint AND release_name = $2"
		" AND created_at >= to_timestamp($3::bigint / 1000000.0) - interval '1 minute' ORDER BY id LIMIT 1");
	g_autofree gchar *adopt_sql = axis_acquire_sql(self, "UPDATE %G SET download_id = $2::bigint, sent_at = now()"
		" WHERE id = $1::bigint AND download_id IS NULL");
	g_autofree gchar *close_sql = axis_acquire_sql(self, "UPDATE %G SET closed_at = now(), close_reason = 'abandoned',"
		" close_text = 'It was never handed to a download client (the daemon stopped while it was being sent).'"
		" WHERE id = $1::bigint AND closed_at IS NULL");
	const gchar *params[4];
	gboolean found = FALSE;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_acquire_store_lock(self, conn, owner_id, &found, error);
	if (ok && found) {
		params[0] = grab_text;
		ok = axis_db_conn_exec(conn, grab_sql, 1, params, &grab, error);
	}
	if (ok && found && axis_db_result_get_n_rows(grab) > 0) {
		g_autofree gchar *title = axis_acquire_int_text(row_int(grab, 0, 1));
		g_autofree gchar *created = axis_acquire_int_text(row_int(grab, 0, 2));

		params[0] = title;
		params[1] = axis_db_result_get_text(grab, 0, 0);
		params[2] = created;
		ok = axis_db_conn_exec(conn, download_sql, 3, params, &download, error);
		if (ok && axis_db_result_get_n_rows(download) > 0) {
			params[0] = grab_text;
			params[1] = axis_db_result_get_text(download, 0, 0);
			ok = axis_db_conn_exec(conn, adopt_sql, 2, params, NULL, error) &&
				axis_acquire_eventf(self, conn, owner_id, NULL, "grabbed", NULL, 0, grab_id, error, "%s: the daemon "
					"stopped while it was being sent; the download that was sent is followed.",
					axis_db_result_get_text(grab, 0, 0));
		} else if (ok) {
			params[0] = grab_text;
			ok = axis_db_conn_exec(conn, close_sql, 1, params, NULL, error) &&
				axis_acquire_eventf(self, conn, owner_id, NULL, "not_sent", NULL, 0, grab_id, error, "%s was never "
					"handed to a download client (the daemon stopped while it was being sent); the %s is wanted again.",
					axis_db_result_get_text(grab, 0, 0), self->info->noun);
		}
		if (ok && report != NULL)
			report->settled++;
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gboolean
settle_claims(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) claims = NULL;
	g_autofree gchar *after = axis_acquire_int_text(AXIS_ACQUIRE_CLAIM_STALE_S);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O FROM %G g WHERE g.closed_at IS NULL"
		" AND g.download_id IS NULL AND g.created_at < now() - $1::integer * interval '1 second'"
		" AND NOT EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = %K AND j.state = 'running')"
		" ORDER BY g.id LIMIT 100");
	const gchar *params[1];
	guint i;

	params[0] = after;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &claims, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(claims); i++) {
		if (!settle_claim(self, row_int(claims, i, 1), row_int(claims, i, 0), report, error))
			return FALSE;
	}
	return TRUE;
}

static gchar *
when_text(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%Y-%m-%d %H:%M UTC") : g_strdup("an unknown time");
}

static gboolean
handle_problems(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *give_up = axis_acquire_int_text(axis_acquire_setting_duration(self, self->info->setting_give_up,
		6 * 3600));
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O, d.state, d.client_ref IS NOT NULL,"
		" d.message, d.client_name, " US("coalesce(d.progress_at, d.sent_at, d.created_at)") " FROM %G g"
		" JOIN download d ON d.id = g.download_id"
		" WHERE g.closed_at IS NULL AND g.completed_at IS NULL"
		" AND (d.state IN ('failed', 'gone', 'removed') OR (d.state IN " LIVE_STATES " AND d.stalled"
		"  AND coalesce(d.progress_at, d.sent_at, d.created_at) < now() - $1::integer * interval '1 second'))"
		" ORDER BY g.id LIMIT 200");
	const gchar *params[1];
	guint i;

	params[0] = give_up;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *state = axis_db_result_get_text(rows, i, 2);
		const gchar *message = axis_db_result_get_text(rows, i, 4);
		const gchar *client = axis_db_result_get_text(rows, i, 5);
		AxisAcquireFailure failure;
		gboolean has_ref = FALSE;
		gboolean handled = FALSE;
		gboolean ok;

		if (client == NULL)
			client = "the download client";
		axis_db_result_get_boolean(rows, i, 3, &has_ref);
		memset(&failure, 0, sizeof(failure));
		if (g_strcmp0(state, "failed") == 0 && !has_ref) {
			failure.close_reason = "not_taken";
			failure.block_reason = "not_taken";
			failure.text = g_strdup_printf("Axis could not tell whether %s took it, and the client does not have it.",
				client);
			failure.retry = TRUE;
		} else if (g_strcmp0(state, "failed") == 0) {
			failure.close_reason = "failed";
			failure.block_reason = "failed";
			failure.text = g_strdup_printf("The download failed at %s%s%s%s", client, message != NULL ? ": " : "",
				message != NULL ? message : "", message != NULL && g_str_has_suffix(message, ".") ? "" : ".");
			failure.remove = TRUE;
			failure.retry = TRUE;
		} else if (g_strcmp0(state, "gone") == 0) {
			failure.close_reason = "gone";
			failure.block_reason = "gone";
			failure.text = g_strdup_printf("%s no longer has the download.", client);
			failure.retry = TRUE;
		} else if (g_strcmp0(state, "removed") == 0) {
			failure.close_reason = "removed";
			failure.text = g_strdup_printf("The download was removed from %s outside the %s%s page; the %s is "
				"wanted again.", client, self->info->noun, g_str_has_suffix(self->info->noun, "s") ? "'" : "'s",
				self->info->noun);
		} else {
			g_autofree gchar *since = when_text(row_int(rows, i, 6));

			failure.close_reason = "stalled";
			failure.block_reason = "stalled";
			failure.text = g_strdup_printf("The download made no progress at %s since %s; Axis gave up on it.", client,
				since);
			failure.remove = TRUE;
			failure.retry = TRUE;
		}
		ok = axis_acquire_handle_failure(self, row_int(rows, i, 1), row_int(rows, i, 0), &failure, NULL, FALSE,
			&handled, report, error);
		g_free(failure.text);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

/*
 * A completed download whose files something else deleted or moved before
 * they were imported (a cleanup job of the download folder, a person): its
 * import ended so (dismissed, files_gone, by the import module).  The grab
 * closes with no blocklist entry (the release was not at fault), the
 * download is removed from its client without its data (there is none of it
 * to keep or delete, and nothing else of the client's is touched), and the
 * title is wanted again: a search for the next best is queued when it is
 * monitored.  Once: the grab is closed by it.
 */
static gboolean
handle_files_gone(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O, i.reason_text FROM %G g"
		" JOIN download d ON d.id = g.download_id"
		" JOIN LATERAL (SELECT state, reason, reason_text FROM import WHERE download_id = d.id ORDER BY id DESC LIMIT 1)"
		"  i ON true"
		" WHERE g.closed_at IS NULL AND g.completed_at IS NOT NULL AND i.state = 'dismissed'"
		" AND i.reason = 'files_gone' ORDER BY g.id LIMIT 200");
	guint i;

	if (!axis_db_pool_exec(self->pool, sql, 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisAcquireFailure failure;
		gboolean handled = FALSE;
		gboolean ok;

		memset(&failure, 0, sizeof(failure));
		failure.close_reason = "files_gone";
		failure.text = g_strdup_printf("Its files disappeared from the download folder before they were imported: %s",
			axis_db_result_get_text(rows, i, 2) != NULL ? axis_db_result_get_text(rows, i, 2) :
			"something else deleted or moved them.");
		failure.remove = TRUE;
		failure.keep_data = TRUE;
		failure.retry = TRUE;
		failure.retry_if_monitored = TRUE;
		ok = axis_acquire_handle_failure(self, row_int(rows, i, 1), row_int(rows, i, 0), &failure, NULL, TRUE, &handled,
			report, error);
		g_free(failure.text);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

/* Grabs given up on whose download is still at the client: the removal
 * again; and the next best once nothing is left there.  A download that is
 * not live any more needs no removal (it failed, was removed or is gone), or
 * has completed: for a grab a person replaced, that one is still asked for
 * (nothing imports it), and where its place is refused it is left at its
 * client for good, said on the Downloads page and in the owner's history.
 * (For the other reasons a completed one is marked done without asking, as
 * before: it arrived after Axis gave up on it, and stays at its client.) */
static gboolean
retry_removals(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O, g.download_id, d.state IN " LIVE_STATES ","
		" g.close_reason IN ('failed', 'stalled', 'given_up', 'gone', 'not_taken') AND g.retry_at IS NULL,"
		" g.close_reason = 'files_gone',"
		/* A grab a person replaced: the place is judged before the client is
		 * asked, each time (remove_download()).  No search follows it: the
		 * reason is not one of those above. */
		" g.close_reason = 'replaced',"
		/* It finished downloading while its removal waited: for a replace it
		 * is still asked for (nobody imports it: its grab is closed), and a
		 * place that is refused ends the removal as kept. */
		" g.close_reason = 'replaced' AND d.state = 'completed' AND d.client_ref IS NOT NULL, g.release_name"
		" FROM %G g JOIN download d ON d.id = g.download_id"
		" WHERE g.closed_at IS NOT NULL AND g.removal IN ('pending', 'failed') ORDER BY g.id LIMIT 100");
	g_autofree gchar *done_sql = axis_acquire_sql(self, "UPDATE %G SET removal = 'done' WHERE id = $1::bigint");
	guint i;

	if (!axis_db_pool_exec(self->pool, sql, 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 grab_id = row_int(rows, i, 0);
		gint64 owner_id = row_int(rows, i, 1);
		gboolean live = FALSE;
		gboolean retry = FALSE;
		gboolean keep_data = FALSE;
		gboolean judge_place = FALSE;
		gboolean arrived = FALSE;
		gboolean gone = FALSE;
		gboolean kept = FALSE;
		gboolean queued = FALSE;

		axis_db_result_get_boolean(rows, i, 3, &live);
		axis_db_result_get_boolean(rows, i, 4, &retry);
		axis_db_result_get_boolean(rows, i, 5, &keep_data);
		axis_db_result_get_boolean(rows, i, 6, &judge_place);
		axis_db_result_get_boolean(rows, i, 7, &arrived);
		if (live || arrived) {
			g_autofree gchar *why = NULL;

			if (!remove_download(self, grab_id, row_int(rows, i, 2), keep_data, judge_place, &gone,
			        arrived ? &kept : NULL, &why, report, error))
				return FALSE;
			/* Left at its client for good: said in the owner's history, once. */
			if (kept) {
				AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
				gboolean ok;

				if (conn == NULL)
					return FALSE;
				ok = axis_db_conn_begin(conn, error) &&
					axis_acquire_eventf(self, conn, owner_id, NULL, "removal", NULL, 0, grab_id, error,
						"%s: %s The %s is not in flight any more: it can be grabbed again.",
						axis_db_result_get_text(rows, i, 8) != NULL ? axis_db_result_get_text(rows, i, 8) : "A download",
						why != NULL ? why : "It is left at its client.", self->info->noun) &&
					axis_db_conn_commit(conn, error);
				axis_db_conn_rollback(conn, NULL);
				axis_db_pool_release(self->pool, conn);
				if (!ok)
					return FALSE;
			}
		} else {
			g_autofree gchar *grab = axis_acquire_int_text(grab_id);
			const gchar *params[1];

			params[0] = grab;
			gone = TRUE;
			if (!axis_db_pool_exec(self->pool, done_sql, 1, params, NULL, error))
				return FALSE;
		}
		if (gone && retry) {
			if (!enqueue_retry(self, owner_id, grab_id, NULL, FALSE, &queued, error))
				return FALSE;
			if (queued && report != NULL)
				report->retries++;
		}
	}
	return TRUE;
}

static gboolean
record_completions(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O FROM %G g JOIN download d ON d.id = g.download_id"
		" WHERE g.closed_at IS NULL AND g.completed_at IS NULL AND d.state = 'completed' ORDER BY g.id LIMIT 500");
	g_autofree gchar *mark_sql = axis_acquire_sql(self, "UPDATE %G g SET completed_at = coalesce(d.completed_at, now())"
		" FROM download d LEFT JOIN volume dv ON dv.id = d.volume_id WHERE g.id = $1::bigint"
		" AND d.id = g.download_id AND g.completed_at IS NULL AND d.state = 'completed'"
		" RETURNING g.release_name, dv.name, d.rel_path");
	guint i;

	if (!axis_db_pool_exec(self->pool, sql, 0, NULL, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autoptr(AxisDbResult) marked = NULL;
		g_autofree gchar *grab = axis_acquire_int_text(row_int(rows, i, 0));
		const gchar *params[1];
		AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
		gboolean ok;

		if (conn == NULL)
			return FALSE;
		params[0] = grab;
		ok = axis_db_conn_begin(conn, error) && axis_db_conn_exec(conn, mark_sql, 1, params, &marked, error);
		if (ok && axis_db_result_get_n_rows(marked) > 0) {
			const gchar *volume = axis_db_result_get_text(marked, 0, 1);
			const gchar *rel_path = axis_db_result_get_text(marked, 0, 2);

			ok = axis_acquire_eventf(self, conn, row_int(rows, i, 1), NULL, "completed", NULL, 0, row_int(rows, i, 0),
				error, "%s has arrived%s%s%s%s. It waits there for its import; nothing was moved.",
				axis_db_result_get_text(marked, 0, 0), volume != NULL ? ": on " : "", volume != NULL ? volume : "",
				rel_path != NULL ? ", at " : "", rel_path != NULL ? rel_path : "");
			if (ok && report != NULL)
				report->completed++;
		}
		ok = ok && axis_db_conn_commit(conn, error);
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_acquire_run_follow(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report_out,
	GError                  **error
){
	AxisAcquireFollowReport report;
	gboolean ok;

	memset(&report, 0, sizeof(report));
	ok = finish_stale_searches(self, error) && settle_claims(self, &report, error) &&
		handle_problems(self, &report, error) && handle_files_gone(self, &report, error) &&
		retry_removals(self, &report, error) &&
		record_completions(self, &report, error) &&
		(self->info->follow_more == NULL || self->info->follow_more(self->data, &report, error)) &&
		axis_acquire_prune_searches(self, FALSE, NULL, error);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}
