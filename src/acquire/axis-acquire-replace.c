/*
 * axis-acquire-replace.c - "Replace the download in flight with this one":
 * what stands in the way of a grab by hand, whether it can be replaced,
 * where a download is before its client is asked to delete it, and the end
 * of the grab that is replaced
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A grab by hand or by force of a release is refused while a grab of the
 * same thing is in flight (axis-acquire-hand.c, axis-acquire-search.c).
 * That was a dead end on the confirmation.  Here the confirmation is told
 * which grabs stand in the way, and when it is exactly one, still open, with
 * a download at its client that has not completed and has no import, it may
 * offer to replace it (add-replace-grab-and-delete-download, slice A).
 *
 * Nothing here deletes a file.  The grab that is replaced is ended through
 * axis_acquire_handle_failure() (axis-acquire-follow.c), which asks the
 * download's client to remove it with its data.  A torrent's client deletes a
 * torrent wherever it is at that moment, and a person can move a torrent in
 * its client, an unfinished one too, into a library folder for one.  So the
 * client is asked where the torrent is first, and the place is judged
 * (axis_acquire_place_judge()): the rule is enforced where the client is
 * asked (remove_download()), the first time and each time the follow-up asks
 * again, and once more before the grab is ended, so that a place that is
 * refused leaves the download in flight as it was.
 */

#include "acquire/axis-acquire-private.h"

#include <string.h>

#include "storage/axis-storage-path.h"

#define LIVE_STATES AXIS_ACQUIRE_LIVE_STATES

AxisAcquireStanding *
axis_acquire_standing_new(
	gint64       grab_id,
	const gchar *holds,
	const gchar *others
){
	AxisAcquireStanding *standing = g_new0(AxisAcquireStanding, 1);

	standing->grab_id = grab_id;
	standing->holds = g_strdup(holds);
	standing->others = others != NULL && others[0] != '\0' ? g_strdup(others) : NULL;
	standing->size_bytes = -1;
	standing->done_bytes = -1;
	return standing;
}

void
axis_acquire_standing_free(AxisAcquireStanding *standing)
{
	if (standing == NULL)
		return;
	g_free(standing->release);
	g_free(standing->client);
	g_free(standing->state);
	g_free(standing->holds);
	g_free(standing->others);
	g_free(standing);
}

/* ── What stands in the way ─────────────────────────────────────── */

/* An owner with no parts: its open grabs, and its grabs whose download is
 * still being removed from its client. */
static gboolean
standing_of_owner(
	AxisAcquire      *self,
	AxisDbConn       *conn,
	gint64            owner_id,
	GPtrArray        *standing,
	GError          **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id FROM %G g LEFT JOIN download d ON d.id = g.download_id"
		" WHERE g.%O = $1::bigint AND (g.closed_at IS NULL OR (g.removal IN ('pending', 'failed')"
		" AND d.state IN " LIVE_STATES ")) ORDER BY g.id LIMIT 20");
	const gchar *params[1];
	guint i;

	params[0] = owner;
	if (!axis_db_conn_exec(conn, sql, 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 id = 0;

		axis_db_result_get_int64(rows, i, 0, &id);
		g_ptr_array_add(standing, axis_acquire_standing_new(id, NULL, NULL));
	}
	return TRUE;
}

/* Each grab as it is now; one that is no longer the owner's is dropped. */
static gboolean
standing_read(
	AxisAcquire                *self,
	AxisDbConn                 *conn,
	gint64                      owner_id,
	const AxisAcquireCandidate *candidate,
	GPtrArray                  *standing,
	GError                    **error
){
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *chosen = axis_acquire_int_text(candidate->id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.release_name, g.closed_at IS NULL, g.download_id,"
		" d.client_ref IS NOT NULL, d.client_name, d.state, d.size_bytes, d.done_bytes,"
		" g.completed_at IS NOT NULL OR coalesce(d.state IN ('completed', 'imported'), false),"
		" (SELECT max(i.id) FROM import i WHERE i.download_id = g.download_id),"
		" coalesce(g.candidate_id = $3::bigint, false) OR (g.release_name = $4 AND g.indexer IS NOT DISTINCT FROM $5),"
		/* The same torrent, whatever indexer each came from: both hashes known. */
		" coalesce(lower(coalesce(g.info_hash, d.info_hash)) = lower($6), false)"
		" FROM %G g LEFT JOIN download d ON d.id = g.download_id WHERE g.id = $1::bigint AND g.%O = $2::bigint");
	guint i = 0;

	while (i < standing->len) {
		AxisAcquireStanding *s = g_ptr_array_index(standing, i);
		g_autoptr(AxisDbResult) row = NULL;
		g_autofree gchar *grab = axis_acquire_int_text(s->grab_id);
		const gchar *params[6];

		params[0] = grab;
		params[1] = owner;
		params[2] = chosen;
		params[3] = candidate->name;
		params[4] = candidate->indexer;
		params[5] = candidate->info_hash != NULL && axis_download_hash_valid(candidate->info_hash) ?
			candidate->info_hash : NULL;
		if (!axis_db_conn_exec(conn, sql, 6, params, &row, error))
			return FALSE;
		if (axis_db_result_get_n_rows(row) == 0) {
			g_ptr_array_remove_index(standing, i);
			continue;
		}
		g_free(s->release);
		s->release = g_strdup(axis_db_result_get_text(row, 0, 0));
		axis_db_result_get_boolean(row, 0, 1, &s->open);
		if (!axis_db_result_is_null(row, 0, 2))
			axis_db_result_get_int64(row, 0, 2, &s->download_id);
		if (!axis_db_result_is_null(row, 0, 3))
			axis_db_result_get_boolean(row, 0, 3, &s->at_client);
		g_free(s->client);
		s->client = g_strdup(axis_db_result_get_text(row, 0, 4));
		g_free(s->state);
		s->state = g_strdup(axis_db_result_get_text(row, 0, 5));
		if (!axis_db_result_is_null(row, 0, 6))
			axis_db_result_get_int64(row, 0, 6, &s->size_bytes);
		if (!axis_db_result_is_null(row, 0, 7))
			axis_db_result_get_int64(row, 0, 7, &s->done_bytes);
		axis_db_result_get_boolean(row, 0, 8, &s->completed);
		if (!axis_db_result_is_null(row, 0, 9))
			axis_db_result_get_int64(row, 0, 9, &s->import_id);
		if (!axis_db_result_is_null(row, 0, 10))
			axis_db_result_get_boolean(row, 0, 10, &s->same);
		axis_db_result_get_boolean(row, 0, 11, &s->same_torrent);
		i++;
	}
	return TRUE;
}

/* An owner with no parts: a download of its title that is still at a client
 * and that none of the grabs in the way follows (the module counts it as in
 * flight: its claim refuses while it is there).  *@stray: (nullable) its
 * release and client, in words. */
static gboolean
stray_download(
	AxisAcquire      *self,
	AxisDbConn       *conn,
	AxisAcquireWant  *want,
	gchar           **stray,
	GError          **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(want->owner_id);
	g_autofree gchar *title = axis_acquire_int_text(want->title_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT d.release_name, d.client_name FROM download d"
		" WHERE d.module = $1 AND d.title_id = $2::bigint AND d.state IN " LIVE_STATES
		" AND NOT EXISTS (SELECT 1 FROM %G g WHERE g.download_id = d.id AND g.%O = $3::bigint"
		"  AND (g.closed_at IS NULL OR g.removal IN ('pending', 'failed'))) ORDER BY d.id LIMIT 1");
	const gchar *params[3];

	*stray = NULL;
	if (want->title_id <= 0 || self->info->module == NULL)
		return TRUE;
	params[0] = self->info->module;
	params[1] = title;
	params[2] = owner;
	if (!axis_db_conn_exec(conn, sql, 3, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) > 0) {
		const gchar *release = axis_db_result_get_text(row, 0, 0);
		const gchar *client = axis_db_result_get_text(row, 0, 1);

		*stray = g_strdup_printf("%s%s%s", release != NULL ? release : "one with no name", client != NULL ? ", at " : "",
			client != NULL ? client : "");
	}
	return TRUE;
}

static const gchar *
release_of(const AxisAcquireStanding *s)
{
	return s->release != NULL ? s->release : "a release";
}

/* The digest of a replace's offer: the release chosen, the grab and its
 * download, what it holds and what of that stops downloading too. */
static gchar *
replace_digest(
	const AxisAcquireCandidate *candidate,
	const AxisAcquireStanding  *s
){
	g_autofree gchar *text = g_strdup_printf("replace|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT
		"|%s|holds:%s|others:%s", candidate->id, s->grab_id, s->download_id, release_of(s),
		s->holds != NULL ? s->holds : "", s->others != NULL ? s->others : "");

	return g_compute_checksum_for_string(G_CHECKSUM_MD5, text, -1);
}

gboolean
axis_acquire_standing_fill(
	AxisAcquire                *self,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
){
	g_autoptr(GPtrArray) standing = g_ptr_array_new_with_free_func((GDestroyNotify)axis_acquire_standing_free);
	g_autofree gchar *stray = NULL;
	const gchar *noun = self->info->noun != NULL ? self->info->noun : "title";
	const AxisAcquireStanding *s;

	preview->in_flight = FALSE;
	preview->replaceable = FALSE;
	/* Another obstacle: that is what the page says, and nothing is offered. */
	if (preview->refused != NULL)
		return TRUE;
	if (self->info->standing != NULL) {
		if (!self->info->standing(self->data, conn, want, candidate, preview, standing, error))
			return FALSE;
	} else if (!standing_of_owner(self, conn, want->owner_id, standing, error)) {
		return FALSE;
	}
	if (!standing_read(self, conn, want->owner_id, candidate, standing, error))
		return FALSE;
	/* (An owner with parts says what is in flight itself: its standing().) */
	if (self->info->standing == NULL && standing->len == 1 && !stray_download(self, conn, want, &stray, error))
		return FALSE;
	if (standing->len == 0 && !want->in_flight && preview->flying == NULL)
		return TRUE;

	preview->in_flight = TRUE;
	if (standing->len == 0) {
		/* In flight by the module's facts, and no grab to name: a download of
		 * it that no grab follows any more is still at a client. */
		preview->standing_text = preview->flying != NULL ? g_strdup(preview->flying) :
			g_strdup_printf("A grab of the %s is in flight already.", noun);
		preview->standing_downloads = TRUE;
	} else if (standing->len > 1) {
		g_autoptr(GString) names = g_string_new(NULL);
		guint i;

		for (i = 0; i < standing->len; i++) {
			const AxisAcquireStanding *each = g_ptr_array_index(standing, i);

			g_string_append_printf(names, "%s%s", i == 0 ? "" : i + 1 == standing->len ? " and " : ", ",
				release_of(each));
			if (each->holds != NULL)
				g_string_append_printf(names, " (%s)", each->holds);
		}
		preview->standing_text = g_strdup_printf("%u downloads are in the way: %s. Axis does not replace more than one "
			"in one step. Give up on each of them on the Downloads page first, then grab this release.", standing->len,
			names->str);
		preview->standing_downloads = TRUE;
	} else {
		s = g_ptr_array_index(standing, 0);
		if (!s->open) {
			preview->standing_text = g_strdup_printf("Axis is still removing an earlier download (%s) from the client. "
				"Nothing is grabbed until it is gone.", release_of(s));
			preview->standing_downloads = TRUE;
		} else if (s->completed || s->import_id > 0) {
			preview->standing_text = s->import_id > 0 ?
				g_strdup_printf("%s has arrived and waits for its import. Resolve the import first.", release_of(s)) :
				g_strdup_printf("%s has arrived, and its import is about to start. It is not replaced: resolve its "
					"import first.", release_of(s));
			preview->standing_import = s->import_id;
			preview->standing_downloads = s->import_id == 0;
		} else if (s->same) {
			preview->standing_text = g_strdup_printf("This very release is the one in flight: %s is downloading%s%s. "
				"There is nothing to replace it with.", release_of(s), s->client != NULL ? " at " : "",
				s->client != NULL ? s->client : "");
			preview->standing_downloads = TRUE;
		} else if (s->same_torrent) {
			preview->standing_text = g_strdup_printf("This release is the same torrent as the one in flight (%s%s%s), "
				"from another indexer. There is nothing to replace it with.", release_of(s),
				s->client != NULL ? ", downloading at " : "", s->client != NULL ? s->client : "");
			preview->standing_downloads = TRUE;
		} else if (stray != NULL) {
			/* The module's claim refuses while that download is at its client:
			 * a replace would end this grab and then send nothing. */
			preview->standing_text = g_strdup_printf("%s is downloading%s%s, and another download of the %s is still at "
				"a download client (%s), which no grab follows any more. Axis does not replace a download while that one "
				"is there. Remove it on the Downloads page first, then grab this release.", release_of(s),
				s->client != NULL ? " at " : "", s->client != NULL ? s->client : "", noun, stray);
			preview->standing_downloads = TRUE;
		} else if (s->download_id == 0 || !s->at_client) {
			preview->standing_text = g_strdup_printf("A release (%s) is being sent to the client right now. Try again "
				"in a moment.", release_of(s));
		} else if (g_strcmp0(s->state, "queued") != 0 && g_strcmp0(s->state, "downloading") != 0 &&
		           g_strcmp0(s->state, "paused") != 0) {
			preview->standing_text = g_strdup_printf("The download of %s has failed or has left its client, and Axis is "
				"dealing with it. Try again in a minute.", release_of(s));
			preview->standing_downloads = TRUE;
		} else {
			preview->standing_text = g_strdup_printf("%s is downloading%s%s%s%s%s.", release_of(s),
				s->client != NULL ? " at " : "", s->client != NULL ? s->client : "",
				s->holds != NULL ? ", for " : "", s->holds != NULL ? s->holds : "",
				s->others != NULL ? ", more than this release holds" : "");
			preview->replaceable = TRUE;
			preview->replace_digest = replace_digest(candidate, s);
		}
	}
	preview->standing = g_steal_pointer(&standing);
	if (!preview->replaceable)
		preview->refused = g_strdup(preview->standing_text);
	return TRUE;
}

/* ── Where a download is, before its client deletes it ──────────── */

/* The positive half of the place rule is the downloads layer's, which
 * enforces it in every removal with its data (axis_downloads_remove()):
 * there is one copy of it. */
gboolean
axis_acquire_place_in_library(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
){
	return axis_download_place_in_library(place, mapped, rel, roots, why);
}

gboolean
axis_acquire_place_judge(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
){
	g_autofree gchar *normal = NULL;
	guint i;

	*why = NULL;
	if (place == NULL || place[0] == '\0') {
		*why = g_strdup("its client does not say where the torrent's files are now.");
		return FALSE;
	}
	/* A path with a "." or ".." part is refused, never resolved. */
	if (!axis_storage_path_normalize_exact(place, TRUE, &normal, NULL)) {
		*why = g_strdup_printf("the place its client gives for the torrent's files (%s) is not a plain full path.",
			place);
		return FALSE;
	}
	if (!mapped || rel == NULL) {
		*why = g_strdup_printf("its client says the torrent's files are at %s, which none of the client's path "
			"mappings reaches, so Axis cannot tell that the place is not a library folder. Add a path mapping for that "
			"folder to the client, on the client's page.", normal);
		return FALSE;
	}
	/* A replace is stricter than the downloads layer: the downloads root
	 * itself is the client's whole folder, never one download's own place. */
	for (i = 0; roots != NULL && i + 1 < roots->len; i += 2) {
		const gchar *root = g_ptr_array_index(roots, i);

		if (root != NULL && strcmp(root, rel) == 0) {
			*why = g_strdup_printf("its client says the torrent's files are at %s, which is a library folder itself. "
				"It was probably moved in the client.", normal);
			return FALSE;
		}
	}
	return !axis_download_place_in_library(normal, TRUE, rel, roots, why);
}

gboolean
axis_acquire_removal_place(
	AxisAcquire       *self,
	gint64             download_id,
	AxisAcquirePlace  *place,
	gchar            **why,
	GError           **error
){
	return axis_acquire_removal_place_full(self, download_id, place, NULL, why, error);
}

gboolean
axis_acquire_removal_place_full(
	AxisAcquire       *self,
	gint64             download_id,
	AxisAcquirePlace  *place,
	gboolean          *in_library,
	gchar            **why,
	GError           **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) roots = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *normal = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *volume_text = NULL;
	g_autofree gchar *unreadable_why = NULL;
	guint unreadable = 0;
	const AxisDownloadItem *own = NULL;
	const gchar *said;
	const gchar *params[1];
	gint64 volume = 0;
	gboolean mapped = FALSE;
	guint i;

	*place = AXIS_ACQUIRE_PLACE_UNKNOWN;
	*why = NULL;
	if (in_library != NULL)
		*in_library = FALSE;
	if (!axis_downloads_get(self->downloads, download_id, &info, &local) || info == NULL) {
		*why = g_strdup_printf("its record could not be read: %s", local != NULL ? local->message : "no reason");
		return TRUE;
	}
	/* A Usenet job, or a download with nothing at a client to name: the
	 * removal is its client's, or asks nothing. */
	if (info->protocol != AXIS_DOWNLOAD_TORRENT || info->client_ref == NULL || info->client_id == 0) {
		*place = AXIS_ACQUIRE_PLACE_WITH_DATA;
		return TRUE;
	}
	if (!axis_downloads_client_items_full(self->downloads, info->client_id, &items, &unreadable, &unreadable_why,
	        &local)) {
		*why = g_strdup_printf("%s%s", local != NULL ? local->message : "its client did not answer",
			local != NULL && g_str_has_suffix(local->message, ".") ? "" : ".");
		return TRUE;
	}
	for (i = 0; items != NULL && i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (g_strcmp0(item->client_ref, info->client_ref) == 0)
			own = item;
	}
	if (own == NULL) {
		/* Not listed, and part of the list could not be read or was cut: it
		 * may be there all the same, wherever it is. */
		if (unreadable > 0) {
			*why = g_strdup_printf("its client does not list it, but part of the client's list could not be read%s%s%s, "
				"so Axis cannot tell that it is gone.", unreadable_why != NULL ? " (" : "",
				unreadable_why != NULL ? unreadable_why : "", unreadable_why != NULL ? ")" : "");
			return TRUE;
		}
		*place = AXIS_ACQUIRE_PLACE_WITHOUT_DATA;
		return TRUE;
	}
	said = own->place != NULL ? own->place : own->path;
	if (said != NULL && axis_storage_path_normalize_exact(said, TRUE, &normal, NULL)) {
		if (axis_downloads_map_client_path(self->downloads, info->client_id, normal, &volume, &rel, NULL, &problem,
		        &local)) {
			mapped = TRUE;
		} else if (local != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
	}
	if (mapped) {
		volume_text = axis_acquire_int_text(volume);
		params[0] = volume_text;
		if (!axis_db_pool_exec(self->pool, "SELECT rel_path, media_type = 'downloads' FROM library_root"
		        " WHERE volume_id = $1::bigint ORDER BY id", 1, params, &rows, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
			gboolean downloads = FALSE;

			axis_db_result_get_boolean(rows, i, 1, &downloads);
			g_ptr_array_add(roots, g_strdup(axis_db_result_get_text(rows, i, 0)));
			g_ptr_array_add(roots, g_strdup(downloads ? "d" : ""));
		}
	}
	*place = axis_acquire_place_judge(said, mapped, rel, roots, why) ? AXIS_ACQUIRE_PLACE_WITH_DATA :
		AXIS_ACQUIRE_PLACE_REFUSED;
	if (in_library != NULL && *place == AXIS_ACQUIRE_PLACE_REFUSED) {
		g_autofree gchar *again = NULL;

		*in_library = axis_acquire_place_in_library(said, mapped, rel, roots, &again);
	}
	return TRUE;
}

/* ── The end of the grab that is replaced ───────────────────────── */

gboolean
axis_acquire_replace_end(
	AxisAcquire                *self,
	gint64                      owner_id,
	const AxisAcquireStanding  *old,
	const gchar                *new_release,
	const gchar                *user,
	gboolean                   *ended,
	gboolean                   *gone,
	gchar                     **why,
	GError                    **error
){
	AxisAcquireFailure failure;
	gboolean ok;

	*ended = FALSE;
	*gone = FALSE;
	*why = NULL;
	memset(&failure, 0, sizeof(failure));
	failure.close_reason = "replaced";
	/* No block: the release was not bad, only not wanted now.  No search: the
	 * person's own grab follows. */
	failure.text = g_strdup_printf("Replaced by %s with %s.", user != NULL ? user : "an administrator",
		new_release != NULL ? new_release : "another release");
	failure.remove = TRUE;
	failure.still_downloading = TRUE;
	failure.judge_place = TRUE;
	ok = axis_acquire_handle_failure_full(self, owner_id, old->grab_id, &failure, user, FALSE, ended, gone, why, NULL,
		error);
	g_free(failure.text);
	return ok;
}
