/*
 * axis-acquire-block.c - a release a person puts on an owner's blocklist
 * (add-block-release): Block this release on a manual search, Block and
 * search again on a download, and the blocklist as it is now shown on
 * releases a search stored
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A person's entry has the reason AXIS_ACQUIRE_BLOCK_PERSON, with who and
 * when.  Nothing here touches a file, a link or an import: a download a
 * person does not want is given up on by the failure path
 * (axis_acquire_handle_failure(), as "Give up on this download"), and a
 * release that is in the library already is only blocked for the future.
 */

#include "acquire/axis-acquire-private.h"

#include <string.h>

static gchar *
whose_of(AxisAcquire *self)
{
	const gchar *noun = self->info->noun != NULL ? self->info->noun : "title";

	return g_strdup_printf("%s%s", noun, g_str_has_suffix(noun, "s") ? "'" : "'s");
}

/* ── The blocklist as it is now, on stored releases ─────────────── */

gboolean
axis_acquire_candidates_mark_blocked(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray    *candidates,
	GError      **error
){
	g_autoptr(GPtrArray) keys = NULL;
	g_autofree gchar *whose = whose_of(self);
	guint i;

	if (candidates == NULL || candidates->len == 0)
		return TRUE;
	if (!axis_acquire_store_block_keys(self, conn, owner_id, &keys, error))
		return FALSE;
	for (i = 0; i < candidates->len; i++) {
		AxisAcquireCandidate *c = g_ptr_array_index(candidates, i);
		const AxisAcquireBlockKey *key = axis_acquire_blocked_by(keys, c->name_key, c->info_hash);
		gboolean was = c->blocklisted;
		guint k;

		if (key == NULL && !was)
			continue;
		/* What the search said of the blocklist is said again, as it is now. */
		for (k = c->reasons->len; k > 0; k--) {
			const AxisAcquireReason *r = g_ptr_array_index(c->reasons, k - 1);

			if (g_strcmp0(r->kind, "blocklist") == 0)
				g_ptr_array_remove_index(c->reasons, k - 1);
		}
		c->blocklisted = key != NULL;
		g_clear_pointer(&c->blocked_by, g_free);
		c->blocked_by_person = FALSE;
		if (key != NULL) {
			g_autofree gchar *why = axis_acquire_block_words(key->reason, key->created_by);
			g_autofree gchar *text = g_strdup_printf("It is on this %s blocklist: %s.", whose, why);

			c->accepted = FALSE;
			c->blocked_by_person = g_strcmp0(key->reason, AXIS_ACQUIRE_BLOCK_PERSON) == 0;
			c->blocked_by = g_strdup(key->created_by);
			g_ptr_array_insert(c->reasons, 0, axis_acquire_reason_new("blocklist", "blocklisted", text));
		} else {
			g_ptr_array_add(c->reasons, axis_acquire_reason_new("note", "unblocked", "It was on the blocklist when "
				"this search ran, and has been taken off it since. Search again to see where it stands now."));
		}
	}
	return TRUE;
}

/* ── Block this release ─────────────────────────────────────────── */

gboolean
axis_acquire_block_candidate(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        candidate_id,
	const gchar  *user,
	gchar       **name,
	GError      **error
){
	AxisAcquireCandidate *candidate = NULL;
	g_autofree gchar *whose = whose_of(self);
	g_autofree gchar *text = g_strdup_printf("Blocked by %s from a manual search.", user != NULL ? user :
		"an administrator");
	gboolean found = FALSE;
	gboolean written = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	if (name != NULL)
		*name = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_acquire_store_lock(self, conn, owner_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "it was removed");
		ok = FALSE;
	}
	ok = ok && axis_acquire_store_candidate(self, conn, owner_id, candidate_id, &candidate, error);
	if (ok && candidate == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "that release is not one of its searches'");
		ok = FALSE;
	}
	/* (A second post of the same block writes no second line of history.) */
	ok = ok && axis_acquire_store_block_written(self, conn, owner_id, candidate->name_key, candidate->info_hash,
			candidate->name, candidate->indexer, AXIS_ACQUIRE_BLOCK_PERSON, text, 0, user, &written, error) &&
		(!written || axis_acquire_eventf(self, conn, owner_id, NULL, "blocklisted", user, candidate->search_id, 0, error,
			"%s: blocked by %s from a manual search. It is on the %s blocklist now: no search takes it, from any "
			"indexer, until it is taken off.", candidate->name, user != NULL ? user : "an administrator", whose)) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && name != NULL)
		*name = g_strdup(candidate->name);
	axis_acquire_candidate_free(candidate);
	return ok;
}

/* ── Block and search again ─────────────────────────────────────── */

void
axis_acquire_block_grab_free(AxisAcquireBlockGrab *grab)
{
	if (grab == NULL)
		return;
	g_free(grab->release_name);
	g_free(grab->indexer);
	g_free(grab->close_reason);
	g_free(grab);
}

static gint64
row_int(
	AxisDbResult *result,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, 0, column, &value);
	return value;
}

static gboolean
row_bool(
	AxisDbResult *result,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, 0, column, &value);
	return value;
}

gboolean
axis_acquire_block_grab_find(
	AxisAcquire            *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab_out,
	GError                **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(GPtrArray) keys = NULL;
	g_autoptr(AxisAcquireBlockGrab) grab = NULL;
	g_autofree gchar *by_grab = axis_acquire_int_text(grab_id > 0 ? grab_id : 0);
	g_autofree gchar *by_download = axis_acquire_int_text(grab_id > 0 || download_id <= 0 ? 0 : download_id);
	g_autofree gchar *by_import = axis_acquire_int_text(grab_id > 0 || download_id > 0 || import_id <= 0 ? 0 :
		import_id);
	/* The latest grab that sent the download (a download is sent for one). */
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.id, g.%O, g.release_name, g.indexer, g.closed_at IS NULL,"
		" g.close_reason, d.client_ref IS NOT NULL AND d.state IN ('sending', 'queued', 'downloading', 'paused',"
		" 'completed', 'failed'), g.name_key, coalesce(g.info_hash, d.info_hash),"
		" coalesce(g.set_aside = 'hand', false)"
		" FROM %G g LEFT JOIN download d ON d.id = g.download_id"
		" WHERE ($1::bigint > 0 AND g.id = $1::bigint)"
		" OR ($2::bigint > 0 AND g.download_id = $2::bigint)"
		" OR ($3::bigint > 0 AND g.download_id = (SELECT i.download_id FROM import i WHERE i.id = $3::bigint))"
		" ORDER BY g.closed_at IS NULL DESC, g.id DESC LIMIT 1");
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean ok;

	*grab_out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = by_grab;
	params[1] = by_download;
	params[2] = by_import;
	ok = axis_db_conn_exec(conn, sql, 3, params, &row, error);
	if (ok && axis_db_result_get_n_rows(row) > 0) {
		grab = g_new0(AxisAcquireBlockGrab, 1);
		grab->grab_id = row_int(row, 0);
		grab->owner_id = row_int(row, 1);
		grab->release_name = g_strdup(axis_db_result_get_text(row, 0, 2));
		grab->indexer = g_strdup(axis_db_result_get_text(row, 0, 3));
		grab->open = row_bool(row, 4);
		grab->close_reason = g_strdup(axis_db_result_get_text(row, 0, 5));
		grab->at_client = grab->open && row_bool(row, 6);
		ok = axis_acquire_store_block_keys(self, conn, grab->owner_id, &keys, error);
		if (ok)
			grab->blocked = axis_acquire_blocked(keys, axis_db_result_get_text(row, 0, 7),
				axis_db_result_get_text(row, 0, 8));
		if (ok && grab->open && !row_bool(row, 9)) {
			/* The failure path's search follows a block only for a monitored
			 * owner (one that is not is looked for when a person asks). */
			grab->searches = TRUE;
			if (self->info->monitored != NULL)
				ok = self->info->monitored(self->data, conn, grab->owner_id, grab->grab_id, &grab->searches, error);
		}
	}
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*grab_out = g_steal_pointer(&grab);
	return ok;
}

/* A closed grab's release on the blocklist, and nothing else. */
static gboolean
block_closed(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *owner = axis_acquire_int_text(owner_id);
	g_autofree gchar *grab = axis_acquire_int_text(grab_id);
	g_autofree gchar *whose = whose_of(self);
	g_autofree gchar *text = g_strdup_printf("Blocked by %s.", user != NULL ? user : "an administrator");
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT g.name_key, coalesce(g.info_hash, d.info_hash),"
		" g.release_name, g.indexer, g.close_reason FROM %G g LEFT JOIN download d ON d.id = g.download_id"
		" WHERE g.id = $1::bigint AND g.%O = $2::bigint");
	const gchar *params[2];
	gboolean found = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = grab;
	params[1] = owner;
	ok = axis_db_conn_begin(conn, error) && axis_acquire_store_lock(self, conn, owner_id, &found, error) &&
		(!found || axis_db_conn_exec(conn, sql, 2, params, &row, error));
	if (ok && (!found || axis_db_result_get_n_rows(row) == 0)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "that download is not one Axis grabbed for it");
		ok = FALSE;
	}
	if (ok) {
		const gchar *release = axis_db_result_get_text(row, 0, 2);
		gboolean imported = g_strcmp0(axis_db_result_get_text(row, 0, 4), "imported") == 0;

		gboolean written = FALSE;

		ok = axis_acquire_store_block_written(self, conn, owner_id, axis_db_result_get_text(row, 0, 0),
				axis_db_result_get_text(row, 0, 1), release, axis_db_result_get_text(row, 0, 3),
				AXIS_ACQUIRE_BLOCK_PERSON, text, grab_id, user, &written, error) &&
			(!written || axis_acquire_eventf(self, conn, owner_id, NULL, "blocklisted", user, 0, grab_id, error,
				"%s: blocked by %s. It is on the %s blocklist now, for the future: no search takes it again.%s",
				release != NULL ? release : "The release", user != NULL ? user : "an administrator", whose, imported ?
				" It was imported already: its file in the library stays as it is." : ""));
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* An import of the grab's download is placing its files now. */
static gboolean
import_running(
	AxisAcquire  *self,
	gint64        grab_id,
	gboolean     *running,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *grab = axis_acquire_int_text(grab_id);
	g_autofree gchar *sql = axis_acquire_sql(self, "SELECT 1 FROM import i JOIN %G g ON g.download_id = i.download_id"
		" WHERE g.id = $1::bigint AND i.state = 'running' LIMIT 1");
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;

	*running = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = grab;
	ok = axis_db_conn_exec(conn, sql, 1, params, &row, error);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*running = axis_db_result_get_n_rows(row) > 0;
	return ok;
}

gboolean
axis_acquire_block_grab(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
){
	g_autoptr(AxisAcquireBlockGrab) grab = NULL;
	AxisAcquireFailure failure;
	gboolean handled = FALSE;
	gboolean running = FALSE;
	gboolean ok;

	*cancelled = FALSE;
	if (!axis_acquire_block_grab_find(self, grab_id, 0, 0, &grab, error))
		return FALSE;
	if (grab == NULL || grab->owner_id != owner_id) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "that download is not one Axis grabbed for it");
		return FALSE;
	}
	if (import_running(self, grab_id, &running, error)) {
		if (running) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Axis is importing this download right now. Try "
				"again when it has finished.");
			return FALSE;
		}
	} else {
		return FALSE;
	}
	if (grab->open) {
		/* The failure path, as a person giving up on the download: the
		 * blocklist, the grab closed, the download removed from its client,
		 * the next best looked for. */
		memset(&failure, 0, sizeof(failure));
		failure.close_reason = "given_up";
		failure.block_reason = AXIS_ACQUIRE_BLOCK_PERSON;
		failure.text = g_strdup_printf("Blocked by %s: the download was not wanted.", user != NULL ? user :
			"an administrator");
		failure.remove = TRUE;
		failure.retry = TRUE;
		/* Only for a monitored owner: one that is not is looked for when a
		 * person asks. */
		failure.retry_if_monitored = TRUE;
		ok =axis_acquire_handle_failure(self, owner_id, grab_id, &failure, user, TRUE, &handled, NULL, error);
		g_free(failure.text);
		if (!ok)
			return FALSE;
		if (handled) {
			*cancelled = TRUE;
			return TRUE;
		}
		/* It was closed meanwhile (imported, failed): blocked alone. */
	}
	return block_closed(self, owner_id, grab_id, user, error);
}
