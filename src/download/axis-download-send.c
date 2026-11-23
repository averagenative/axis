/*
 * axis-download-send.c - handing a release to a client once, removing a
 * download Axis added, testing a client
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Sending, step by step (download/axis-download.h states the promise):
 *
 *   1. The request is checked; a .torrent's info-hash is read (bencode).
 *   2. The record is written, 'sending', and this process notes that it is
 *      sending it (tracking leaves it alone meanwhile).
 *   3. The kind hands the release to the client.
 *   4. The client's identifier is recorded ('queued').  When step 3 failed:
 *      if nothing can have reached the client, or the client refused, the
 *      record is failed; otherwise it stays 'sending' and tracking matches
 *      it against the client's list later.  It is never sent again here.
 *
 * A daemon that stops between 2 and 3 leaves a 'sending' record the client
 * never saw; one that stops between 3 and 4 leaves one the client has.
 * Tracking tells them apart by the client's list.  The test faults stop
 * exactly there.
 */

#include "download/axis-download-private.h"

#include <string.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-schedule.h"
#include "download/axis-download-bencode.h"
#include "download/axis-download-path.h"
#include "storage/axis-storage-path.h"

static gboolean
invalid(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, message);
	return FALSE;
}

/* ── The client to send to ──────────────────────────────────────── */

static gboolean
choose_client(
	AxisDownloads               *self,
	const AxisDownloadRequest   *request,
	AxisDownloadClientInfo     **chosen,
	GError                     **error
){
	g_autoptr(GPtrArray) clients = NULL;
	AxisDownloadClientInfo *best = NULL;
	guint i;

	*chosen = NULL;
	/* A protocol only a built-in kind takes goes to that kind's one client. */
	for (i = 0; i < self->kinds->len; i++) {
		const AxisDownloadKindInfo *kind = g_ptr_array_index(self->kinds, i);
		AxisDbConn *conn;
		gint64 id = 0;
		gboolean ok;

		if (!kind->builtin || kind->protocol != request->protocol)
			continue;
		if (!axis_downloads_builtin_client(self, kind->name, &id, error))
			return FALSE;
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL)
			return FALSE;
		ok = axis_download_store_clients(conn, id, &clients, error);
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			return FALSE;
		if (clients->len == 0) {
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT,
				"The built-in download client is not there.");
			return FALSE;
		}
		*chosen = g_ptr_array_steal_index(clients, 0);
		return TRUE;
	}
	if (request->protocol != AXIS_DOWNLOAD_USENET && request->protocol != AXIS_DOWNLOAD_TORRENT) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT,
			"This version of Axis has nothing that takes this kind of release.");
		return FALSE;
	}
	if (!axis_downloads_list_clients(self, &clients, error))
		return FALSE;
	for (i = 0; i < clients->len; i++) {
		AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self, client->kind);

		if (request->client_id != 0 && client->id != request->client_id)
			continue;
		if (request->client_id != 0) {
			if (kind == NULL || !client->enabled)
				break;
			if (kind->protocol != request->protocol)
				return invalid(error, "That client takes the other kind of release.");
			best = client;
			break;
		}
		if (kind == NULL || !client->enabled || kind->protocol != request->protocol)
			continue;
		if (best == NULL || client->id < best->id)
			best = client;
	}
	if (best == NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NO_CLIENT,
			request->client_id != 0 ? "That download client does not exist or is switched off." :
			"No download client that is switched on takes this kind of release.");
		return FALSE;
	}
	for (i = 0; i < clients->len; i++) {
		if (g_ptr_array_index(clients, i) == best) {
			*chosen = g_ptr_array_steal_index(clients, i);
			break;
		}
	}
	return TRUE;
}

/* ── Adding ─────────────────────────────────────────────────────── */

typedef struct
{
	const AxisDownloadRelease *release;
	gchar                     *client_ref;
	guint                      last_status;
} AddCall;

static gboolean
add_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	AddCall *call = data;
	gboolean ok;

	ok = kind->add(kind, conn, call->release, target->info->category, target->info->priority, &call->client_ref,
		error);
	call->last_status = axis_download_conn_get_last_status(conn);
	if (ok && !axis_download_ref_valid(call->client_ref)) {
		g_clear_pointer(&call->client_ref, g_free);
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNREADABLE,
			"the client took the release but gave no identifier Axis can use");
		return FALSE;
	}
	return ok;
}

/* Whether the client certainly did not take the release. */
static gboolean
certainly_not_sent(
	const GError *error,
	gboolean      sent,
	guint         last_status
){
	if (g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH) ||
	    g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) ||
	    g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
	    g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
		return TRUE;
	if (!sent)
		return TRUE;
	/* A client error status: it read the request and said no. */
	return g_error_matches(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_FAILED) && last_status >= 400 &&
		last_status <= 499;
}

static gboolean
record_sending(
	AxisDownloads                *self,
	const AxisDownloadClientInfo *client,
	const AxisDownloadRequest    *request,
	const gchar                  *release_name,
	const gchar                  *info_hash,
	const gchar                  *title_label,
	gint64                       *id,
	GError                      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *client_text = axis_download_int_text(client->id);
	g_autofree gchar *title_text = request->title_id > 0 ? axis_download_int_text(request->title_id) : NULL;
	const gchar *params[9];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	params[0] = client_text;
	params[1] = info_hash;
	ok = axis_db_conn_begin(conn, error);
	/* The same torrent twice at one client is refused here, not by it. */
	if (ok && info_hash != NULL) {
		ok = axis_db_conn_exec(conn,
			"SELECT 1 FROM download WHERE client_id = $1::bigint AND info_hash = $2::text"
			" AND state IN ('sending', 'queued', 'downloading', 'paused', 'completed') FOR UPDATE", 2, params,
			&result, error);
		if (ok && axis_db_result_get_n_rows(result) > 0) {
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS,
				"That release is at that client already.");
			ok = FALSE;
		}
		g_clear_pointer(&result, axis_db_result_free);
	}
	params[2] = client->name;
	params[3] = axis_download_protocol_to_string(request->protocol);
	params[4] = request->file != NULL ? "file" : request->magnet != NULL ? "magnet" : "address";
	params[5] = release_name;
	params[6] = title_text;
	params[7] = title_label;
	params[8] = request->module;
	ok = ok && axis_db_conn_exec(conn,
		"INSERT INTO download (client_id, info_hash, client_name, protocol, source, release_name, title_id,"
		" title_label, module, state)"
		" VALUES ($1::bigint, $2::text, $3::text, $4::text, $5::text, $6::text,"
		" (SELECT id FROM meta_title WHERE id = $7::bigint), $8::text, $9::text, 'sending') RETURNING id",
		9, params, &result, error);
	ok = ok && axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	else
		axis_db_result_get_int64(result, 0, 0, id);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* The record of @id after the client was called. */
static gboolean
record_outcome(
	AxisDownloads  *self,
	gint64          id,
	const gchar    *client_ref,
	const gchar    *failed,
	const gchar    *uncertain,
	GError        **error
){
	g_autofree gchar *id_text = axis_download_int_text(id);
	g_autofree gchar *clean = axis_download_text_clean(failed != NULL ? failed : uncertain,
		AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	const gchar *params[2];

	params[0] = id_text;
	params[1] = client_ref != NULL ? client_ref : clean;
	if (client_ref != NULL)
		return axis_db_pool_exec(self->pool,
			"UPDATE download SET client_ref = $2::text, state = 'queued', sent_at = now(), updated_at = now(),"
			" problem = NULL, progress_at = now() WHERE id = $1::bigint AND state = 'sending'", 2, params, NULL, error);
	if (failed != NULL)
		return axis_db_pool_exec(self->pool,
			"UPDATE download SET state = 'failed', problem = $2::text, updated_at = now(), finished_at = now()"
			" WHERE id = $1::bigint AND state = 'sending'", 2, params, NULL, error);
	return axis_db_pool_exec(self->pool,
		"UPDATE download SET problem = $2::text, updated_at = now() WHERE id = $1::bigint AND state = 'sending'", 2,
		params, NULL, error);
}

/* Has the tracking job look at @client_id within seconds, not at its next
 * scheduled run: a run in progress is told the client is due now, and one is
 * queued under the schedule's own de-duplication key, so that at most one is
 * queued or running.  Best effort: the schedule is the fallback. */
static void
request_tracking(
	AxisDownloads *self,
	gint64         client_id
){
	AxisDownloadSlot *slot = axis_download_slot_get(self, client_id);
	g_autofree gchar *dedupe = g_strconcat(AXIS_JOB_SCHEDULE_DEDUPE_PREFIX, AXIS_DOWNLOAD_TRACK_SCHEDULE, NULL);
	g_autoptr(GError) error = NULL;
	AxisJobSpec spec;
	AxisDbConn *conn;

	g_mutex_lock(&self->lock);
	slot->next_poll_us = 0;
	g_mutex_unlock(&self->lock);
	axis_download_slot_unref(slot);

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_DOWNLOAD_TRACK_KIND;
	spec.queue = AXIS_DOWNLOAD_QUEUE;
	spec.dedupe_key = dedupe;
	spec.max_attempts = 1;
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn != NULL) {
		axis_job_enqueue(conn, &spec, NULL, NULL, &error);
		axis_db_pool_release(self->pool, conn);
	}
	if (error != NULL)
		g_warning("downloads: tracking could not be started for a sent download: %s", error->message);
}

static void
notify_one(
	AxisDownloads *self,
	gint64         id
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));

	g_array_append_val(ids, id);
	axis_download_notify(self, ids, TRUE);
}

gboolean
axis_downloads_add(
	AxisDownloads              *self,
	const AxisDownloadRequest  *request,
	gint64                     *download_id,
	GError                    **error
){
	g_autoptr(AxisDownloadClientInfo) client = NULL;
	g_autofree gchar *info_hash = NULL;
	g_autofree gchar *torrent_name = NULL;
	g_autofree gchar *release_name = NULL;
	g_autofree gchar *title_label = NULL;
	g_autofree gchar *file_name = NULL;
	g_autoptr(GError) local = NULL;
	AxisDownloadRelease release;
	AddCall call;
	gint64 id = 0;
	gboolean sent = FALSE;
	gboolean certainly_failed = FALSE;
	gboolean ok;

	g_return_val_if_fail(self != NULL && request != NULL, FALSE);

	if (download_id != NULL)
		*download_id = 0;
	if (axis_download_protocol_to_string(request->protocol) == NULL)
		return invalid(error, "A release is for Usenet or for torrents.");
	if (request->protocol == AXIS_DOWNLOAD_YOUTUBE) {
		/* An address and nothing else; which addresses are taken is the
		 * kind's own check, in its add(). */
		if (request->file != NULL || request->magnet != NULL || !axis_download_address_valid(request->address))
			return invalid(error, "A YouTube download is a video's https address.");
	} else if (request->address != NULL) {
		return invalid(error, "An address is not a Usenet or torrent release.");
	} else if ((request->file == NULL) == (request->magnet == NULL)) {
		return invalid(error, "A release is a file or a magnet link, one of the two.");
	}
	if (request->address == NULL && request->magnet != NULL) {
		const gchar *problem = NULL;

		if (request->protocol != AXIS_DOWNLOAD_TORRENT)
			return invalid(error, "A magnet link is a torrent.");
		if (!axis_download_magnet_parse(request->magnet, &info_hash, &problem)) {
			g_autofree gchar *text = g_strdup_printf("The magnet link is refused: %s.", problem);

			return invalid(error, text);
		}
	} else if (request->address == NULL) {
		gsize size = g_bytes_get_size(request->file);

		if (size == 0)
			return invalid(error, "The release file is empty.");
		if (size > AXIS_DOWNLOAD_MAX_RELEASE_BYTES)
			return invalid(error, "The release file is larger than Axis uploads.");
		if (request->protocol == AXIS_DOWNLOAD_TORRENT) {
			const gchar *problem = NULL;

			if (!axis_download_torrent_read(g_bytes_get_data(request->file, NULL), size, &info_hash,
			        &torrent_name, &problem)) {
				g_autofree gchar *text = g_strdup_printf("The file is not a torrent: %s.", problem);

				return invalid(error, text);
			}
		}
	}
	release_name = axis_download_text_clean(request->release_name, AXIS_DOWNLOAD_NAME_MAX_CHARS);
	if (release_name == NULL)
		release_name = g_strdup(torrent_name);
	if (release_name == NULL)
		return invalid(error, "A release needs a name.");
	title_label = axis_download_text_clean(request->title_label, AXIS_DOWNLOAD_NAME_MAX_CHARS);
	if (request->module != NULL && !axis_download_module_valid(request->module))
		return invalid(error, "That is not a module's name.");
	if (!choose_client(self, request, &client, error))
		return FALSE;

	if (!record_sending(self, client, request, release_name, info_hash, title_label, &id, error))
		return FALSE;
	if (download_id != NULL)
		*download_id = id;
	axis_download_mark_sending(self, id, TRUE);
	notify_one(self, id);

	if (g_atomic_int_get(&self->fault) == AXIS_DOWNLOAD_FAULT_AFTER_RECORD) {
		axis_download_mark_sending(self, id, FALSE);
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_FAILED,
			"stopped by a test fault after recording");
		return FALSE;
	}

	memset(&release, 0, sizeof(release));
	release.protocol = request->protocol;
	release.file = request->file;
	file_name = request->file != NULL ? g_strdup_printf("%s.%s", release_name,
		request->protocol == AXIS_DOWNLOAD_TORRENT ? "torrent" : "nzb") : NULL;
	release.file_name = file_name;
	release.magnet = request->magnet;
	release.name = release_name;
	release.info_hash = info_hash;
	release.address = request->address;
	memset(&call, 0, sizeof(call));
	call.release = &release;
	ok = axis_download_with_client(self, client->id, NULL, add_call, &call, &sent, &local);

	if (ok && g_atomic_int_get(&self->fault) == AXIS_DOWNLOAD_FAULT_AFTER_SEND) {
		axis_download_mark_sending(self, id, FALSE);
		g_free(call.client_ref);
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_FAILED,
			"stopped by a test fault after sending");
		return FALSE;
	}

	if (ok) {
		g_autoptr(GError) written = NULL;

		if (!record_outcome(self, id, call.client_ref, NULL, NULL, &written)) {
			/* The client has it: tracking adopts it by its hash or name. */
			g_warning("downloads: download %" G_GINT64_FORMAT " was sent but its identifier could not be recorded: %s",
				id, written->message);
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN,
				"The client took the release, but Axis could not record it yet; it will be matched later.");
			ok = FALSE;
		}
		g_free(call.client_ref);
	} else if (certainly_not_sent(local, sent, call.last_status)) {
		g_autofree gchar *text = g_strdup_printf("Not sent: %s", local->message);
		g_autoptr(GError) written = NULL;

		if (!record_outcome(self, id, NULL, text, NULL, &written))
			g_warning("downloads: download %" G_GINT64_FORMAT " could not be marked failed: %s", id, written->message);
		g_propagate_error(error, g_steal_pointer(&local));
		certainly_failed = TRUE;
	} else {
		g_autofree gchar *text = g_strdup_printf("Handed to the client, but its answer was lost (%s); it will be "
			"matched against the client's list, never sent again", local->message);
		g_autoptr(GError) written = NULL;

		if (!record_outcome(self, id, NULL, NULL, text, &written))
			g_warning("downloads: download %" G_GINT64_FORMAT " could not be updated: %s", id, written->message);
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_UNCERTAIN,
			"It is not known whether the client took the release (%s); it will be matched later.", local->message);
	}
	/* Sent, or possibly sent: have it looked at now (a certain failure needs no look). */
	if (!certainly_failed)
		request_tracking(self, client->id);
	axis_download_mark_sending(self, id, FALSE);
	notify_one(self, id);
	return ok;
}

/* ── Removing ───────────────────────────────────────────────────── */

/*
 * The place rule of every removal with its data.  A torrent's client, asked
 * to remove a torrent with its data, deletes it wherever it is at that
 * moment, and a person can move a torrent in its client (into a library
 * folder, for one).  So the one function here that asks a client to remove
 * (remove_ask()) asks first, on the same connection and with the client's
 * turn held, where the torrent is: a place known to be the top of a disk, a
 * library root, inside one or to hold one is never deleted at.  The torrent
 * is then removed WITHOUT its data and the caller is told why.  A place that
 * is not known to be one of those (the client does not answer or does not
 * list it, no path mapping reaches the place) changes nothing: the removal
 * is with its data, as it was asked.  Every caller passes here
 * (axis_downloads_remove(), axis_downloads_remove_imported()), so none can
 * leave the rule out.
 */

gboolean
axis_download_place_in_library(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
){
	g_autofree gchar *normal = NULL;
	guint i;

	*why = NULL;
	if (place == NULL || place[0] == '\0' || !mapped || rel == NULL ||
	    !axis_storage_path_normalize_exact(place, TRUE, &normal, NULL))
		return FALSE;
	if (rel[0] == '\0') {
		*why = g_strdup_printf("its client says the torrent's files are at %s, which is the top of a disk.", normal);
		return TRUE;
	}
	for (i = 0; roots != NULL && i + 1 < roots->len; i += 2) {
		const gchar *root = g_ptr_array_index(roots, i);
		gboolean downloads = g_strcmp0(g_ptr_array_index(roots, i + 1), "d") == 0;

		if (root == NULL)
			continue;
		if (downloads && strcmp(root, rel) == 0)
			continue;	/* the client's own save path: not a library folder */
		if (strcmp(root, rel) == 0) {
			*why = g_strdup_printf("its client says the torrent's files are at %s, which is a library folder itself. "
				"It was probably moved in the client.", normal);
			return TRUE;
		}
		if (!downloads && axis_storage_path_contains(root, rel)) {
			*why = g_strdup_printf("its client says the torrent's files are at %s, which is inside a library folder. "
				"It was probably moved in the client.", normal);
			return TRUE;
		}
		if (axis_storage_path_contains(rel, root)) {
			*why = g_strdup_printf("its client says the torrent's files are at %s, which holds a library folder.",
				normal);
			return TRUE;
		}
	}
	return FALSE;
}

/* Whether the client, asked now on @conn, says the torrent @client_ref is at
 * a place a client must not delete at; *@why then.  A client that does not
 * answer or does not list it, and a place no mapping reaches, are %FALSE in
 * *@in_library (not known).  Fails only for a database error. */
static gboolean
torrent_in_library(
	AxisDownloads              *self,
	gint64                      client_id,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	const gchar                *client_ref,
	gboolean                   *in_library,
	gchar                     **why,
	GError                    **error
){
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) roots = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *normal = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *volume_text = NULL;
	const gchar *said = NULL;
	const gchar *params[1];
	gint64 volume = 0;
	guint i;

	*in_library = FALSE;
	*why = NULL;
	if (kind->list == NULL || client_ref == NULL)
		return TRUE;
	conn->list_partial = FALSE;
	/* Only Axis's category: a torrent outside it is refused as not ours by
	 * both client kinds, so listing the rest changes no outcome. */
	if (!kind->list(kind, conn, axis_download_conn_get_category(conn), &items, &local) || items == NULL)
		return TRUE;
	for (i = 0; i < items->len; i++) {
		AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (item == NULL || item->unreadable != NULL || !axis_download_item_check(item))
			continue;
		if (g_strcmp0(item->client_ref, client_ref) == 0)
			said = item->place != NULL ? item->place : item->path;
	}
	if (said == NULL || !axis_storage_path_normalize_exact(said, TRUE, &normal, NULL))
		return TRUE;
	if (!axis_downloads_map_client_path(self, client_id, normal, &volume, &rel, NULL, &problem, &local)) {
		if (local != NULL) {
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		return TRUE;
	}
	volume_text = axis_download_int_text(volume);
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
	*in_library = axis_download_place_in_library(normal, TRUE, rel, roots, why);
	return TRUE;
}

typedef struct
{
	gint64       client_id;
	const gchar *client_ref;
	gboolean     delete_data;
	gboolean     torrent;	/* its place is asked before it goes with its data */
	gboolean     look_only;	/* ask where it is, and remove nothing */
	gboolean     note_place;	/* without its data: ask where it is all the same */
	const gchar *id_text;	/* a person's removal: looked at again; else NULL */
	gboolean     already;	/* removed or gone by the time its turn came */
	gchar       *left;	/* removed without its data after all: why (owned) */
} RemoveCall;

/* The one place where a client is asked to remove a download. */
static gboolean
remove_ask(
	AxisDownloads              *self,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	RemoveCall                 *call,
	GError                    **error
){
	gboolean with_data = call->delete_data;

	g_clear_pointer(&call->left, g_free);
	if ((with_data || call->look_only || call->note_place) && call->torrent) {
		gboolean in_library = FALSE;

		if (!torrent_in_library(self, call->client_id, kind, conn, call->client_ref, &in_library, &call->left, error))
			return FALSE;
		if (in_library)
			with_data = FALSE;
		else
			g_clear_pointer(&call->left, g_free);
	}
	if (call->look_only)
		return TRUE;
	return kind->remove(kind, conn, call->client_ref, with_data, error);
}

static gboolean
remove_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	RemoveCall *call = data;
	g_autoptr(AxisDbResult) row = NULL;
	const gchar *params[1];

	/* Looked at again with the client's turn held: a click while another
	 * removal of it was asking the client asks nothing (add-activity-
	 * feedback). */
	if (call->id_text == NULL)
		return remove_ask(self, kind, conn, call, error);
	params[0] = call->id_text;
	if (!axis_db_pool_exec(self->pool, "SELECT 1 FROM download WHERE id = $1::bigint AND state IN ('removed', 'gone')",
	        1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) > 0) {
		call->already = TRUE;
		return TRUE;
	}
	return remove_ask(self, kind, conn, call, error);
}

/* What is recorded and said of a torrent removed without its data. */
static gchar *
left_words(const gchar *why)
{
	return g_strdup_printf("Removed from its client without its data. Its files were left where they are: %s",
		why != NULL ? why : "they are in a library folder.");
}

gboolean
axis_downloads_removal_leaves(
	AxisDownloads  *self,
	gint64          download_id,
	gchar         **why,
	GError        **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autoptr(GError) local = NULL;
	RemoveCall call;

	*why = NULL;
	if (!axis_downloads_get(self, download_id, &info, error))
		return FALSE;
	if (info->protocol != AXIS_DOWNLOAD_TORRENT || info->client_ref == NULL || info->client_id == 0 ||
	    info->state == AXIS_DOWNLOAD_REMOVED || info->state == AXIS_DOWNLOAD_GONE)
		return TRUE;
	memset(&call, 0, sizeof(call));
	call.client_id = info->client_id;
	call.client_ref = info->client_ref;
	call.torrent = TRUE;
	call.look_only = TRUE;
	/* A client that cannot be asked: not known, as at the removal itself. */
	if (!axis_download_with_client(self, info->client_id, NULL, remove_call, &call, NULL, &local)) {
		g_free(call.left);
		return TRUE;
	}
	*why = call.left;
	return TRUE;
}

gboolean
axis_downloads_remove(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	GError        **error
){
	return axis_downloads_remove_full(self, download_id, delete_data, NULL, error);
}

static gboolean
remove_recorded(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	gboolean        note_place,
	gchar         **left,
	GError        **error
);

gboolean
axis_downloads_remove_full(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	gchar         **left,
	GError        **error
){
	return remove_recorded(self, download_id, delete_data, FALSE, left, error);
}

gboolean
axis_downloads_remove_leaving(
	AxisDownloads  *self,
	gint64          download_id,
	gchar         **left,
	GError        **error
){
	return remove_recorded(self, download_id, FALSE, TRUE, left, error);
}

static gboolean
remove_recorded(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	gboolean        note_place,
	gchar         **left,
	GError        **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *left_why = NULL;
	const gchar *params[2];
	RemoveCall call;

	if (left != NULL)
		*left = NULL;
	if (!axis_downloads_get(self, download_id, &info, error))
		return FALSE;
	if (info->client_ref == NULL || info->client_id == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
			"That download has no identifier at its client, so there is nothing to remove there.");
		return FALSE;
	}
	if (info->state == AXIS_DOWNLOAD_REMOVED || info->state == AXIS_DOWNLOAD_GONE) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"That download is no longer at its client.");
		return FALSE;
	}
	id_text = axis_download_int_text(download_id);
	memset(&call, 0, sizeof(call));
	call.client_id = info->client_id;
	call.client_ref = info->client_ref;
	call.delete_data = delete_data;
	call.note_place = note_place;
	call.torrent = info->protocol == AXIS_DOWNLOAD_TORRENT;
	call.id_text = id_text;
	params[0] = id_text;
	{
		GError *local = NULL;
		gboolean asked = axis_download_with_client(self, info->client_id, NULL, remove_call, &call, NULL, &local);

		left_why = g_steal_pointer(&call.left);
		if (!asked) {
			/* The client no longer has it: it is gone, as tracking would
			 * find in a few polls; said so, and recorded now.  Asked to
			 * delete its data, the client deleted nothing: what it left is
			 * the import module's cleanup's to look at. */
			if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE)) {
				params[1] = delete_data && info->state != AXIS_DOWNLOAD_IMPORTED ? "pending" : NULL;
				if (!axis_db_pool_exec(self->pool, "UPDATE download SET state = 'gone', updated_at = now(),"
				        " finished_at = coalesce(finished_at, now()), rate_bps = NULL, eta_s = NULL,"
				        " folder_removal = coalesce($2::text, folder_removal),"
				        " folder_removal_text = CASE WHEN $2::text IS NULL THEN folder_removal_text ELSE NULL END"
				        " WHERE id = $1::bigint AND state NOT IN ('removed', 'gone', 'imported')", 2, params, NULL,
				        error)) {
					g_error_free(local);
					return FALSE;
				}
				notify_one(self, download_id);
			}
			/* In another category or label now (a person moved it there): not
			 * Axis's any more.  Nothing was asked of the client, and nothing of
			 * its files is Axis's to look at; it is gone from Axis's downloads. */
			if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_OURS)) {
				if (!axis_db_pool_exec(self->pool, "UPDATE download SET state = 'gone', updated_at = now(),"
				        " finished_at = coalesce(finished_at, now()), rate_bps = NULL, eta_s = NULL"
				        " WHERE id = $1::bigint AND state NOT IN ('removed', 'gone', 'imported')", 1, params, NULL,
				        error)) {
					g_error_free(local);
					return FALSE;
				}
				notify_one(self, download_id);
			}
			g_propagate_error(error, local);
			return FALSE;
		}
	}
	if (call.already) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"That download is no longer at its client.");
		return FALSE;
	}
	/* SABnzbd's delete with its files keeps a completed job's folder: what a
	 * Usenet download asked to go with its data left is the import module's
	 * cleanup's to look at.  A torrent's client deletes its files itself.  An
	 * imported download's leftover is its import's (axis-import-folder.c). */
	params[1] = delete_data && info->protocol == AXIS_DOWNLOAD_USENET && info->state != AXIS_DOWNLOAD_IMPORTED ?
		"pending" : NULL;
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE download SET state = 'removed', updated_at = now(), finished_at = coalesce(finished_at, now()),"
	        " rate_bps = NULL, eta_s = NULL, folder_removal = coalesce($2::text, folder_removal),"
	        " folder_removal_text = CASE WHEN $2::text IS NULL THEN folder_removal_text ELSE NULL END"
	        " WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	/* A torrent in a library folder: removed without its data after all.  It
	 * is recorded here, for the Downloads page, whoever asked. */
	if (left_why != NULL) {
		g_autofree gchar *text = left_words(left_why);

		g_message("downloads: download %" G_GINT64_FORMAT ": %s", download_id, text);
		if (!axis_downloads_note_left(self, download_id, text, error))
			return FALSE;
		if (left != NULL)
			*left = g_steal_pointer(&text);
		return TRUE;
	}
	notify_one(self, download_id);
	return TRUE;
}

gboolean
axis_downloads_note_left(
	AxisDownloads  *self,
	gint64          download_id,
	const gchar    *text,
	GError        **error
){
	g_autofree gchar *id_text = axis_download_int_text(download_id);
	const gchar *params[2];

	params[0] = id_text;
	params[1] = text;
	if (!axis_db_pool_exec(self->pool, "UPDATE download SET folder_removal = 'left', folder_removal_text = left($2, 1000),"
	        " updated_at = now() WHERE id = $1::bigint", 2, params, NULL, error))
		return FALSE;
	notify_one(self, download_id);
	return TRUE;
}

gboolean
axis_downloads_mark_imported(
	AxisDbConn  *conn,
	gint64       download_id,
	gboolean    *marked,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_download_int_text(download_id);
	const gchar *params[1];

	*marked = FALSE;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "UPDATE download SET state = 'imported', updated_at = now(),"
	        " finished_at = coalesce(finished_at, now()), rate_bps = NULL, eta_s = NULL"
	        " WHERE id = $1::bigint AND state IN ('completed', 'imported') RETURNING id", 1, params, &result, error))
		return FALSE;
	*marked = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

void
axis_downloads_notify_changed(
	AxisDownloads *self,
	gint64         download_id
){
	notify_one(self, download_id);
}

gboolean
axis_downloads_remove_imported(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        torrents,
	GError        **error
){
	return axis_downloads_remove_imported_full(self, download_id, torrents, NULL, error);
}

gboolean
axis_downloads_remove_imported_full(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        torrents,
	gchar         **left,
	GError        **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	RemoveCall call;

	if (left != NULL)
		*left = NULL;
	if (!axis_downloads_get(self, download_id, &info, error))
		return FALSE;
	if (info->state != AXIS_DOWNLOAD_IMPORTED) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"Only a download that has been imported is removed this way.");
		return FALSE;
	}
	if (info->protocol != AXIS_DOWNLOAD_USENET && !torrents) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"A torrent is left with its client to seed.");
		return FALSE;
	}
	if (info->client_ref == NULL || info->client_id == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
			"That download has no identifier at its client, so there is nothing to remove there.");
		return FALSE;
	}
	memset(&call, 0, sizeof(call));
	call.client_id = info->client_id;
	call.client_ref = info->client_ref;
	call.delete_data = TRUE;
	call.torrent = info->protocol == AXIS_DOWNLOAD_TORRENT;
	if (!axis_download_with_client(self, info->client_id, NULL, remove_call, &call, NULL, error)) {
		g_free(call.left);
		return FALSE;
	}
	/* (The record stays imported; its import says what became of the files.) */
	if (call.left != NULL) {
		g_autofree gchar *text = left_words(call.left);

		g_free(call.left);
		g_message("downloads: download %" G_GINT64_FORMAT ": %s", download_id, text);
		if (left != NULL)
			*left = g_steal_pointer(&text);
	}
	notify_one(self, download_id);
	return TRUE;
}

/* How many times a torrent's removal (without its data) is asked before the
 * client must say it no longer has it. */
#define RELEASE_TRIES 3

gboolean
axis_downloads_release(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean       *was_gone,
	GError        **error
){
	g_autoptr(AxisDownloadInfo) info = NULL;
	RemoveCall call;
	guint tries;

	*was_gone = FALSE;
	if (!axis_downloads_get(self, download_id, &info, error))
		return FALSE;
	if (info->protocol != AXIS_DOWNLOAD_TORRENT) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"Only a torrent is let go of by its client this way.");
		return FALSE;
	}
	if (info->state != AXIS_DOWNLOAD_COMPLETED && info->state != AXIS_DOWNLOAD_IMPORTED) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"Only a torrent that has completed is let go of this way.");
		return FALSE;
	}
	if (info->client_ref == NULL || info->client_id == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
			"That download has no identifier at its client, so its client cannot be asked to let go of it.");
		return FALSE;
	}
	memset(&call, 0, sizeof(call));
	call.client_id = info->client_id;
	call.client_ref = info->client_ref;
	/* Removed without its data, then asked again: only the client's own
	 * answer that it does not have the torrent (gone) confirms that it has
	 * let go of it. */
	for (tries = 0; tries < RELEASE_TRIES; tries++) {
		GError *local = NULL;

		if (axis_download_with_client(self, info->client_id, NULL, remove_call, &call, NULL, &local))
			continue;
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_GONE)) {
			g_error_free(local);
			*was_gone = tries == 0;
			notify_one(self, download_id);
			return TRUE;
		}
		g_propagate_error(error, local);
		return FALSE;
	}
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED,
		"the client still has the torrent after it was asked to remove it");
	return FALSE;
}

/* ── The other clients around a place ───────────────────────────── */

void
axis_download_neighbour_free(AxisDownloadNeighbour *neighbour)
{
	if (neighbour == NULL)
		return;
	g_free(neighbour->name);
	g_free(neighbour->folder);
	g_free(neighbour->folder_failure);
	g_free(neighbour->folder_none);
	g_clear_pointer(&neighbour->items, g_ptr_array_unref);
	g_free(neighbour->unreadable_why);
	g_free(neighbour->failure);
	g_free(neighbour);
}

gboolean
axis_downloads_neighbours(
	AxisDownloads  *self,
	gint64          client_id,
	gint64          volume,
	const gchar    *rel,
	GPtrArray     **out,
	GError        **error
){
	g_autoptr(GPtrArray) clients = NULL;
	guint i;

	*out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_neighbour_free);
	if (rel == NULL)
		return TRUE;
	if (!axis_downloads_list_clients(self, &clients, error))
		return FALSE;
	for (i = 0; i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);
		AxisDownloadNeighbour *neighbour;
		GError *local = NULL;
		gboolean reaches = FALSE;
		guint k;

		/* Every other client configured: one switched off in Axis is still
		 * a program that may keep files there. */
		if (client->id == client_id)
			continue;
		/* Only a client that could put files in or around the place, as Axis
		 * sees it: one of its mappings holds the place or lies inside it. */
		for (k = 0; client->maps != NULL && k < client->maps->len; k++) {
			const AxisDownloadPathMapInfo *map = g_ptr_array_index(client->maps, k);
			const gchar *map_rel = map->rel_path != NULL ? map->rel_path : "";

			if (map->volume_id == volume && (axis_storage_path_contains(map_rel, rel) ||
			    axis_storage_path_contains(rel, map_rel)))
				reaches = TRUE;
		}
		if (!reaches)
			continue;
		neighbour = g_new0(AxisDownloadNeighbour, 1);
		neighbour->client_id = client->id;
		neighbour->name = g_strdup(client->name);
		/* Its folder for Axis: one it cannot say is not known not to be this
		 * one either. */
		if (!axis_downloads_category_folder(self, client->id, &neighbour->folder, &local)) {
			if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) ||
			    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
			    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
				neighbour->folder_none = g_strdup(local->message);
			else
				neighbour->folder_failure = g_strdup(local->message);
			g_clear_error(&local);
		}
		if (!axis_downloads_client_items_full(self, client->id, &neighbour->items, &neighbour->unreadable,
		        &neighbour->unreadable_why, &local)) {
			neighbour->failure = g_strdup(local->message);
			g_clear_error(&local);
		}
		g_ptr_array_add(*out, neighbour);
	}
	return TRUE;
}

/* ── A torrent in any category ──────────────────────────────────── */

typedef struct
{
	const gchar      *client_ref;
	AxisDownloadItem *item;
	gchar            *category;
} FindCall;

static gboolean
find_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	FindCall *call = data;

	if (kind->find == NULL) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "%s cannot look a torrent up by its hash.",
			kind->label);
		return FALSE;
	}
	call->category = g_strdup(target->info->category);
	return kind->find(kind, conn, call->client_ref, &call->item, error);
}

gboolean
axis_downloads_find_torrent(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_ref,
	gboolean       *found,
	gboolean       *ours,
	gchar         **category,
	GError        **error
){
	FindCall call;
	gboolean ok;

	*found = FALSE;
	*ours = FALSE;
	if (category != NULL)
		*category = NULL;
	if (!axis_download_hash_valid(client_ref)) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "That is not a torrent's hash.");
		return FALSE;
	}
	memset(&call, 0, sizeof(call));
	call.client_ref = client_ref;
	ok = axis_download_with_client(self, client_id, NULL, find_call, &call, NULL, error);
	if (ok && call.item != NULL) {
		*found = TRUE;
		*ours = call.item->category != NULL && call.category != NULL &&
			g_ascii_strcasecmp(call.item->category, call.category) == 0;
		if (category != NULL)
			*category = g_strdup(call.item->category != NULL ? call.item->category : "");
	}
	axis_download_item_free(call.item);
	g_free(call.category);
	return ok;
}

/* ── What the client has now ────────────────────────────────────── */

typedef struct
{
	GPtrArray *items;
	gboolean   partial;
} ItemsCall;

static gboolean
items_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	ItemsCall *call = data;
	gboolean ok;

	/* Everything it has, in every category or label: whether anything of
	 * the client's is at a path does not depend on the category it is in. */
	conn->list_partial = FALSE;
	ok = kind->list(kind, conn, NULL, &call->items, error);
	call->partial = conn->list_partial;
	return ok;
}

/* An entry of a client's list, for a person: its identifier and its name, as
 * far as they can be read. */
static gchar *
entry_words(const AxisDownloadItem *item)
{
	g_autofree gchar *name = item != NULL ? axis_download_text_clean(item->name != NULL ? item->name : item->own_name,
		AXIS_DOWNLOAD_NAME_MAX_CHARS) : NULL;

	if (item == NULL || !axis_download_ref_valid(item->client_ref))
		return g_strdup("an entry whose identifier Axis cannot read");
	return name != NULL ? g_strdup_printf("the entry %s (%s)", item->client_ref, name) :
		g_strdup_printf("the entry %s", item->client_ref);
}

gboolean
axis_downloads_client_items_full(
	AxisDownloads  *self,
	gint64          client_id,
	GPtrArray     **items,
	guint          *unreadable,
	gchar         **why,
	GError        **error
){
	g_autofree gchar *first = NULL;
	ItemsCall call;
	guint n = 0;
	guint i;

	*items = NULL;
	if (unreadable != NULL)
		*unreadable = 0;
	if (why != NULL)
		*why = NULL;
	memset(&call, 0, sizeof(call));
	if (!axis_download_with_client(self, client_id, NULL, items_call, &call, NULL, error)) {
		g_clear_pointer(&call.items, g_ptr_array_unref);
		return FALSE;
	}
	if (call.items == NULL)
		call.items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	/* What the layer would not use is not used here either; but it is
	 * counted, and the first is said: the client has something Axis could not
	 * read (or could not read the place of), and that is not nothing. */
	for (i = 0; i < call.items->len; i++) {
		AxisDownloadItem *item = g_ptr_array_index(call.items, i);
		gboolean had_place = item != NULL && (item->place != NULL || item->path != NULL);
		g_autofree gchar *reason = item != NULL && item->unreadable != NULL ? g_strdup(item->unreadable) : NULL;
		g_autofree gchar *words = NULL;

		if (!axis_download_item_check(item)) {
			n++;
			if (first == NULL) {
				words = entry_words(item);
				first = g_strdup_printf("%s: %s", words, reason != NULL ? reason : "it is not one Axis can use");
			}
			axis_download_item_free(item);
			call.items->pdata[i] = NULL;
		} else if (had_place && item->place == NULL && item->path == NULL) {
			n++;
			if (first == NULL) {
				words = entry_words(item);
				first = g_strdup_printf("%s: its place cannot be read exactly", words);
			}
		}
	}
	for (i = call.items->len; i > 0; i--) {
		if (g_ptr_array_index(call.items, i - 1) == NULL)
			g_ptr_array_remove_index(call.items, i - 1);
	}
	if (call.partial) {
		n++;
		if (first == NULL)
			first = g_strdup("its list was cut at the most Axis reads, or held an entry whose identifier Axis cannot "
				"read");
	}
	if (unreadable != NULL)
		*unreadable = n;
	if (why != NULL)
		*why = g_steal_pointer(&first);
	*items = call.items;
	return TRUE;
}

gboolean
axis_downloads_client_items(
	AxisDownloads  *self,
	gint64          client_id,
	GPtrArray     **items,
	guint          *unreadable,
	GError        **error
){
	return axis_downloads_client_items_full(self, client_id, items, unreadable, NULL, error);
}

/* ── Where the client keeps Axis's downloads ────────────────────── */

typedef struct
{
	gchar *folder;
} FolderCall;

static gboolean
folder_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	FolderCall *call = data;

	if (kind->category_folder == NULL) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"%s cannot say where it keeps Axis's downloads.", kind->label);
		return FALSE;
	}
	return kind->category_folder(kind, conn, target->info->category, &call->folder, error);
}

gboolean
axis_downloads_category_folder(
	AxisDownloads  *self,
	gint64          client_id,
	gchar         **folder,
	GError        **error
){
	g_autofree gchar *normal = NULL;
	FolderCall call;

	*folder = NULL;
	memset(&call, 0, sizeof(call));
	if (!axis_download_with_client(self, client_id, NULL, folder_call, &call, NULL, error)) {
		g_free(call.folder);
		return FALSE;
	}
	/* What the client said is checked here too: absolute, no "." or "..",
	 * nothing trimmed. */
	if (call.folder == NULL || !axis_storage_path_normalize_exact(call.folder, TRUE, &normal, NULL)) {
		g_free(call.folder);
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED,
			"the client named a folder for Axis's category that cannot be used");
		return FALSE;
	}
	g_free(call.folder);
	*folder = g_steal_pointer(&normal);
	return TRUE;
}

gboolean
axis_downloads_map_client_path(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_path,
	gint64         *volume_id,
	gchar         **rel_path,
	gchar         **prefix,
	gchar         **problem,
	GError        **error
){
	g_autoptr(GPtrArray) maps = NULL;
	const gchar *why = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*volume_id = 0;
	*rel_path = NULL;
	*problem = NULL;
	if (prefix != NULL)
		*prefix = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_download_store_maps(conn, client_id, &maps, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (axis_download_path_apply_full(maps, client_path, volume_id, rel_path, prefix, &why) ==
	    AXIS_DOWNLOAD_PATH_MAPPED)
		return TRUE;
	*problem = g_strdup(why != NULL ? why : "the client's path cannot be mapped");
	return FALSE;
}

gboolean
axis_downloads_remove_ref(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_ref,
	gboolean        delete_data,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *client_text = axis_download_int_text(client_id);
	const gchar *params[2];
	gint64 id = 0;

	if (!axis_download_ref_valid(client_ref)) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
			"Axis has no record of that download.");
		return FALSE;
	}
	params[0] = client_text;
	params[1] = client_ref;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id FROM download WHERE client_id = $1::bigint AND client_ref = $2::text"
	        " AND state NOT IN ('removed', 'gone') ORDER BY id DESC LIMIT 1", 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0 || !axis_db_result_get_int64(result, 0, 0, &id)) {
		/* Not Axis's: it is left alone, and the client is not asked. */
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
			"Axis has no record of that download, so it did not add it and will not remove it.");
		return FALSE;
	}
	return axis_downloads_remove(self, id, delete_data, error);
}

/* ── Testing ────────────────────────────────────────────────────── */

typedef struct
{
	AxisDownloadTestResult result;
	gboolean               builtin;
} TestCall;

static gboolean
test_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	TestCall *call = data;

	call->builtin = kind->builtin;
	if (!kind->builtin && axis_download_conn_get_secret(conn) == NULL) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_AUTH, "no secret is set");
		return FALSE;
	}
	return kind->test(kind, conn, target->info->category, &call->result, error);
}

/* The latest recorded path of the client's, checked against its mappings. */
static gchar *
mapping_note(
	AxisDownloads *self,
	gint64         client_id
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) maps = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *client_text = axis_download_int_text(client_id);
	g_autofree gchar *problem = NULL;
	g_autofree gchar *rel = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gchar *path = NULL;
	gint64 volume_id = 0;

	params[0] = client_text;
	conn = axis_db_pool_acquire(self->pool, &error);
	if (conn == NULL)
		return NULL;
	if (axis_db_conn_exec(conn,
	        "SELECT client_path FROM download WHERE client_id = $1::bigint AND client_path IS NOT NULL"
	        " ORDER BY updated_at DESC LIMIT 1", 1, params, &result, &error) &&
	    axis_db_result_get_n_rows(result) > 0)
		path = g_strdup(axis_db_result_get_text(result, 0, 0));
	if (path != NULL && !axis_download_store_maps(conn, client_id, &maps, &error))
		g_clear_pointer(&path, g_free);
	axis_db_pool_release(self->pool, conn);
	if (path == NULL)
		return g_strdup("No download of this client has finished yet, so its path mappings could not be checked.");
	if (axis_download_locate(self, maps, path, &volume_id, &rel, &problem)) {
		g_free(path);
		return g_strdup("The path of its latest finished download maps to a volume.");
	}
	g_free(path);
	return g_strdup_printf("The path of its latest finished download does not map: %s.", problem);
}

/* Whether another client keeps Axis's downloads in the client's folder for
 * Axis, or in one holding it or inside it, as Axis sees them on a volume:
 * then that folder is not the client's alone, and Axis deletes nothing there
 * (the Leftovers page lists nothing, a download's folder is left).  %NULL when
 * no other does (or the client has no folder of its own). */
static gchar *
folder_note(
	AxisDownloads *self,
	gint64         client_id
){
	g_autoptr(GPtrArray) neighbours = NULL;
	g_autoptr(GPtrArray) clients = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *folder = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *problem = NULL;
	gboolean others = FALSE;
	gint64 volume = 0;
	guint i;

	/* Nothing is asked when no other client is configured (switched off in
	 * Axis or not: the program may still run). */
	if (!axis_downloads_list_clients(self, &clients, NULL))
		return NULL;
	for (i = 0; i < clients->len; i++) {
		const AxisDownloadClientInfo *client = g_ptr_array_index(clients, i);

		if (client->id != client_id)
			others = TRUE;
	}
	if (!others) {
		axis_downloads_set_folder_warning(self, client_id, NULL);
		return NULL;
	}
	/* Its own folder for Axis: none (or none mapped) is said by the
	 * Leftovers page, and nothing is shared then; one that cannot be asked
	 * now leaves what was found before. */
	if (!axis_downloads_category_folder(self, client_id, &folder, &local)) {
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
			axis_downloads_set_folder_warning(self, client_id, NULL);
		return NULL;
	}
	if (!axis_downloads_map_client_path(self, client_id, folder, &volume, &rel, NULL, &problem, &local)) {
		if (local == NULL)
			axis_downloads_set_folder_warning(self, client_id, NULL);
		return NULL;
	}
	if (!axis_downloads_neighbours(self, client_id, volume, rel, &neighbours, &local))
		return NULL;
	for (i = 0; i < neighbours->len; i++) {
		const AxisDownloadNeighbour *n = g_ptr_array_index(neighbours, i);
		g_autofree gchar *theirs = NULL;
		g_autofree gchar *why = NULL;
		gint64 where = 0;

		/* One whose folder for Axis cannot be worked out may keep Axis's
		 * downloads in this one: nothing is deleted there either. */
		if (n->folder == NULL && n->folder_none != NULL) {
			gchar *note = g_strdup_printf("Its folder for Axis (%s) may not be its own: the download client %s, whose "
				"path mappings reach it, keeps Axis's downloads in no folder Axis can work out (%s). Axis deletes nothing "
				"in it (the Leftovers page lists nothing there) until that client keeps Axis's downloads in a folder of "
				"its own, or its path mappings no longer reach this one.", folder, n->name, n->folder_none);

			axis_downloads_set_folder_warning(self, client_id, note);
			return note;
		}
		if (n->folder == NULL ||
		    !axis_downloads_map_client_path(self, n->client_id, n->folder, &where, &theirs, NULL, &why, NULL))
			continue;
		if (where == volume && (axis_storage_path_contains(theirs, rel) || axis_storage_path_contains(rel, theirs))) {
			gchar *note = g_strdup_printf("Its folder for Axis (%s) is not its own: the download client %s keeps Axis's "
				"downloads in %s, which is the same folder, or holds it, or is inside it. Axis deletes nothing in it (the "
				"Leftovers page lists nothing there, and what a download leaves there stays) until each client keeps "
				"Axis's downloads in a folder of its own.", folder, n->name, n->folder);

			axis_downloads_set_folder_warning(self, client_id, note);
			return note;
		}
	}
	axis_downloads_set_folder_warning(self, client_id, NULL);
	return NULL;
}

gboolean
axis_downloads_test_client(
	AxisDownloads           *self,
	gint64                   id,
	AxisDownloadTestResult  *result,
	GError                 **error
){
	g_autoptr(GError) local = NULL;
	g_autofree gchar *message = NULL;
	AxisDownloadClientState state = AXIS_DOWNLOAD_CLIENT_OK;
	TestCall call;
	AxisDbConn *conn;
	gboolean ok;

	if (result != NULL) {
		memset(result, 0, sizeof(*result));
		result->category_exists = -1;
	}
	memset(&call, 0, sizeof(call));
	call.result.category_exists = -1;
	ok = axis_download_with_client(self, id, NULL, test_call, &call, NULL, &local);
	if (!ok) {
		if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) ||
		    g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED) ||
		    local->domain != AXIS_DOWNLOAD_ERROR) {
			axis_download_test_result_clear(&call.result);
			g_propagate_error(error, g_steal_pointer(&local));
			return FALSE;
		}
		message = axis_download_contact_problem(local, &state);
	} else if (call.builtin) {
		/* No path is mapped and no folder is a client's own. */
		message = g_strdup(call.result.note);
	} else {
		g_autofree gchar *note = mapping_note(self, id);
		g_autofree gchar *shared = folder_note(self, id);
		g_autofree gchar *first = note != NULL ? g_strdup_printf("%s%s%s", call.result.note != NULL ?
			call.result.note : "", call.result.note != NULL ? " " : "", note) : g_strdup(call.result.note);

		message = shared != NULL ? g_strdup_printf("%s%s%s", first != NULL ? first : "", first != NULL ? " " : "",
			shared) : g_steal_pointer(&first);
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_download_test_result_clear(&call.result);
		return FALSE;
	}
	ok = axis_download_store_client_state(conn, id, state, message, call.result.version,
		state == AXIS_DOWNLOAD_CLIENT_OK ? call.result.category_exists : -1, state == AXIS_DOWNLOAD_CLIENT_OK, error);
	axis_db_pool_release(self->pool, conn);
	if (result != NULL) {
		*result = call.result;
		g_free(result->note);
		result->note = g_strdup(message);
	} else {
		axis_download_test_result_clear(&call.result);
	}
	return ok;
}
