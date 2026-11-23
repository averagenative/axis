/*
 * axis-download-track.c - following Axis's downloads at their clients
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One poll of a client is one list call, made while holding the client's
 * call lock (so no add or remove of that client runs in between), then:
 *
 *  - Only the client's items in Axis's category are looked at, and of
 *    those only the ones with a record: anything else is counted as
 *    ignored and never touched.
 *  - A recorded download takes the client's state, figures and message.
 *    Completion needs a location: the client's path, mapped onto a volume
 *    and found there through the volume guard (axis_download_locate()).
 *    Until then the download stays in progress with the problem shown.
 *  - A recorded download missing from the list for downloads.gone_after
 *    polls in a row becomes gone.
 *  - A 'sending' record that this process is not sending now, older than
 *    AXIS_DOWNLOAD_RECONCILE_AFTER_S, is matched to an item by info-hash
 *    (a torrent) or by name, adopting the item's identifier; with no match
 *    it is failed, and nothing is sent again.
 *  - A client that cannot be listed is recorded as such; its downloads are
 *    left exactly as they were.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "download/axis-download-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db/axis-db-conn.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-guard.h"

/* ── Locations ──────────────────────────────────────────────────── */

gboolean
axis_download_locate(
	AxisDownloads    *self,
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	gchar           **problem
){
	return axis_download_locate_full(self, maps, client_path, volume_id, rel_path, NULL, problem);
}

gboolean
axis_download_locate_full(
	AxisDownloads    *self,
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	guint64          *inode,
	gchar           **problem
){
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *parent = NULL;
	const gchar *why = NULL;
	const gchar *last;
	const gchar *slash;
	struct stat st;
	gint64 vid = 0;
	gint fd;
	gint rc;

	*volume_id = 0;
	*rel_path = NULL;
	*problem = NULL;
	if (inode != NULL)
		*inode = 0;
	if (maps == NULL) {
		/* A built-in kind's own path: "/<volume id>/<path on the volume>".
		 * Nothing is mapped; the place is checked like any other below. */
		const gchar *p = client_path != NULL && client_path[0] == '/' ? client_path + 1 : NULL;
		gchar *end = NULL;

		if (p != NULL && g_ascii_isdigit(*p))
			vid = g_ascii_strtoll(p, &end, 10);
		if (end == NULL || *end != '/' || vid <= 0 ||
		    !axis_storage_path_normalize_exact(end + 1, FALSE, &rel, NULL) || rel[0] == '\0') {
			*problem = g_strdup("Axis's own path of it cannot be read");
			return FALSE;
		}
	} else {
		switch (axis_download_path_apply(maps, client_path, &vid, &rel, &why)) {
		case AXIS_DOWNLOAD_PATH_MAPPED:
			break;
		default:
			*problem = g_strdup(why != NULL ? why : "the client's path cannot be used");
			return FALSE;
		}
	}
	volume = axis_storage_get_volume(self->storage, vid);
	guard = axis_volume_guard_acquire(self->storage, vid, AXIS_VOLUME_INTENT_READ, &error);
	if (guard == NULL) {
		if (g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND) || volume == NULL)
			*problem = g_strdup("the path maps to a volume Axis no longer knows");
		else if (g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE))
			*problem = g_strdup_printf("the volume %s, where the path maps, is not online", volume->name);
		else
			*problem = g_strdup_printf("the volume %s could not be checked", volume->name);
		return FALSE;
	}
	slash = strrchr(rel, '/');
	parent = slash != NULL ? g_strndup(rel, (gsize)(slash - rel)) : g_strdup("");
	last = slash != NULL ? slash + 1 : rel;
	fd = axis_volume_guard_open_dir(guard, parent, &error);
	if (fd < 0) {
		if (g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED))
			*problem = g_strdup_printf("the path leads through a symbolic link or onto another filesystem on %s",
				volume->name);
		else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
			*problem = g_strdup_printf("nothing is at the mapped place on %s (check the mapping)", volume->name);
		else
			*problem = g_strdup_printf("the mapped place on %s cannot be opened", volume->name);
		return FALSE;
	}
	rc = fstatat(fd, last, &st, AT_SYMLINK_NOFOLLOW);
	close(fd);
	if (rc != 0) {
		*problem = errno == ENOENT ?
			g_strdup_printf("nothing is at the mapped place on %s (check the mapping)", volume->name) :
			g_strdup_printf("the mapped place on %s cannot be read", volume->name);
		return FALSE;
	}
	if (S_ISLNK(st.st_mode)) {
		*problem = g_strdup_printf("the mapped place on %s is a symbolic link, which is not followed", volume->name);
		return FALSE;
	}
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
		*problem = g_strdup_printf("the mapped place on %s is not a file or a folder", volume->name);
		return FALSE;
	}
	if ((guint64)st.st_dev != axis_volume_guard_get_dev(guard)) {
		*problem = g_strdup_printf("the mapped place is on another filesystem than %s", volume->name);
		return FALSE;
	}
	/* The volume is still the one the guard checked. */
	if (!axis_volume_guard_check(guard, NULL)) {
		*problem = g_strdup_printf("the volume %s went away while it was checked", volume->name);
		return FALSE;
	}
	*volume_id = vid;
	*rel_path = g_steal_pointer(&rel);
	if (inode != NULL)
		*inode = (guint64)st.st_ino;
	return TRUE;
}

/* ── One poll ───────────────────────────────────────────────────── */

/* What is to be written to one record. */
typedef struct
{
	AxisDownloadInfo  *before;	/* the record as it was */
	AxisDownloadState  state;
	gchar             *client_ref;	/* adopted (a 'sending' record) */
	gint64             size_bytes;
	gint64             done_bytes;
	gint64             rate_bps;
	gint64             eta_s;
	gchar             *client_path;
	gint64             volume_id;
	gchar             *rel_path;
	guint64            location_inode;	/* what is at the place, when completed now */
	gchar             *message;
	gchar             *problem;
	gchar             *reason;	/* why a finished download is not taken */
	gboolean           stalled;
	gboolean           seen;
	gboolean           grew;
	gint               missing_polls;
	gboolean           completed_now;
	gboolean           finished_now;
} Change;

static void
change_free(gpointer data)
{
	Change *change = data;

	g_free(change->client_ref);
	g_free(change->client_path);
	g_free(change->rel_path);
	g_free(change->message);
	g_free(change->problem);
	g_free(change->reason);
	g_free(change);
}

typedef struct
{
	GCancellable           *cancellable;
	AxisDownloadPollReport *report;
	GArray                 *changed;	/* gint64 */
	gboolean                set_changed;
	gint64                  client_id;
} Poll;

static gboolean
same_text(
	const gchar *a,
	const gchar *b
){
	return g_strcmp0(a, b) == 0;
}

static gint64
figure(gint64 value)
{
	return value >= 0 ? value : -1;
}

static gchar *
figure_text(gint64 value)
{
	return value >= 0 ? axis_download_int_text(value) : NULL;
}

/* The record @change describes differs from what it was. */
static gboolean
differs(const Change *change)
{
	const AxisDownloadInfo *b = change->before;

	return change->state != b->state || change->client_ref != NULL || change->size_bytes != b->size_bytes ||
		change->done_bytes != b->done_bytes || change->rate_bps != b->rate_bps || change->eta_s != b->eta_s ||
		!same_text(change->client_path, b->client_path) || change->volume_id != b->volume_id ||
		!same_text(change->rel_path, b->rel_path) || !same_text(change->message, b->message) ||
		!same_text(change->problem, b->problem) || change->stalled != b->stalled;
}

static Change *
change_new(AxisDownloadInfo *before)
{
	Change *change = g_new0(Change, 1);

	change->before = before;
	change->state = before->state;
	change->size_bytes = before->size_bytes;
	change->done_bytes = before->done_bytes;
	change->rate_bps = before->rate_bps;
	change->eta_s = before->eta_s;
	change->client_path = g_strdup(before->client_path);
	change->volume_id = before->volume_id;
	change->rel_path = g_strdup(before->rel_path);
	change->message = g_strdup(before->message);
	change->problem = g_strdup(before->problem);
	change->stalled = before->stalled;
	return change;
}

/* The client's view of a recorded download, into @change. */
static void
take_item(
	AxisDownloads          *self,
	const GPtrArray        *maps,
	gboolean                builtin,
	Change                 *change,
	const AxisDownloadItem *item,
	AxisDownloadPollReport *report
){
	const AxisDownloadInfo *before = change->before;

	change->seen = TRUE;
	change->size_bytes = figure(item->size_bytes);
	change->done_bytes = figure(item->done_bytes);
	change->rate_bps = figure(item->rate_bps);
	change->eta_s = figure(item->eta_s);
	change->grew = change->done_bytes > before->done_bytes;
	/* The client's words as they are now; Axis's problem only while one
	 * stands (set again below for a download that waits). */
	g_free(change->message);
	change->message = g_strdup(item->message);
	g_clear_pointer(&change->problem, g_free);
	switch (item->state) {
	case AXIS_DOWNLOAD_ITEM_QUEUED:
		change->state = AXIS_DOWNLOAD_QUEUED;
		break;
	case AXIS_DOWNLOAD_ITEM_PAUSED:
		change->state = AXIS_DOWNLOAD_PAUSED;
		break;
	case AXIS_DOWNLOAD_ITEM_DOWNLOADING:
		change->state = AXIS_DOWNLOAD_DOWNLOADING;
		break;
	case AXIS_DOWNLOAD_ITEM_FAILED:
		change->state = AXIS_DOWNLOAD_FAILED;
		if (change->message == NULL)
			change->message = g_strdup("The client says it failed, without saying why.");
		change->finished_now = TRUE;
		change->rate_bps = -1;
		change->eta_s = -1;
		report->failed++;
		break;
	case AXIS_DOWNLOAD_ITEM_COMPLETED: {
		g_autofree gchar *problem = NULL;
		g_autofree gchar *rel = NULL;
		gint64 volume_id = 0;

		change->rate_bps = -1;
		change->eta_s = -1;
		if (change->size_bytes >= 0)
			change->done_bytes = change->size_bytes;
		g_free(change->client_path);
		change->client_path = g_strdup(item->path);
		if (item->path == NULL) {
			problem = g_strdup("the client says it is complete but gives no path for it");
		} else if (axis_download_locate_full(self, maps, item->path, &volume_id, &rel, &change->location_inode,
		           &problem)) {
			/* A built-in kind's finished download is a file its kind has put
			 * in the library already: imported in the same step, so that it
			 * is never one waiting to be imported (which a manual import
			 * would offer, and the import pipeline has nothing to do with). */
			change->state = builtin ? AXIS_DOWNLOAD_IMPORTED : AXIS_DOWNLOAD_COMPLETED;
			change->volume_id = volume_id;
			g_free(change->rel_path);
			change->rel_path = g_steal_pointer(&rel);
			change->completed_now = TRUE;
			change->finished_now = TRUE;
			report->completed++;
			break;
		}
		/* It waits, in progress, with the reason; nothing is guessed. */
		change->state = AXIS_DOWNLOAD_DOWNLOADING;
		change->volume_id = 0;
		g_clear_pointer(&change->rel_path, g_free);
		change->problem = g_strdup_printf("Finished at the download client, but cannot be imported yet: %s.", problem);
		change->reason = g_steal_pointer(&problem);
		report->waiting++;
		break;
	}
	}
	if (item->state != AXIS_DOWNLOAD_ITEM_COMPLETED)
		g_clear_pointer(&change->client_path, g_free);
	/* Stalled (a flag, not a state) is decided where the time of the last
	 * growth is kept: write_change(). */
}

/* Writes one change (the record must still be in the state it was read
 * in); *@stalled is the flag as written. */
static gboolean
write_change(
	AxisDbConn    *conn,
	const Change  *change,
	gint64         stall_after_s,
	gboolean      *written,
	gboolean      *stalled,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_download_int_text(change->before->id);
	g_autofree gchar *size = figure_text(change->size_bytes);
	g_autofree gchar *done = figure_text(change->done_bytes);
	g_autofree gchar *rate = figure_text(change->rate_bps);
	g_autofree gchar *eta = figure_text(change->eta_s);
	g_autofree gchar *volume = change->volume_id > 0 ? axis_download_int_text(change->volume_id) : NULL;
	g_autofree gchar *missing = g_strdup_printf("%d", change->missing_polls);
	g_autofree gchar *stall = axis_download_int_text(stall_after_s);
	g_autofree gchar *message = axis_download_text_clean(change->message, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	g_autofree gchar *problem = axis_download_text_clean(change->problem, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	g_autofree gchar *inode = change->completed_now && change->location_inode > 0 ?
		g_strdup_printf("%" G_GUINT64_FORMAT, change->location_inode) : NULL;
	const gchar *params[20];

	params[0] = id_text;
	params[1] = axis_download_state_to_string(change->before->state);
	params[2] = axis_download_state_to_string(change->state);
	params[3] = change->client_ref;
	params[4] = size;
	params[5] = done;
	params[6] = rate;
	params[7] = eta;
	params[8] = change->client_path;
	params[9] = volume;
	params[10] = change->rel_path;
	params[11] = message;
	params[12] = problem;
	params[13] = change->seen ? "true" : "false";
	params[14] = change->grew ? "true" : "false";
	params[15] = missing;
	params[16] = change->completed_now ? "true" : "false";
	params[17] = change->finished_now ? "true" : "false";
	params[18] = stall;
	params[19] = inode;
	/* Stalled is decided here, where the time of the last growth is: a
	 * torrent in progress, seen now, that has not grown for stall_after.  The
	 * inode of the place is the one seen when it completed. */
	if (!axis_db_conn_exec(conn,
	        "UPDATE download SET state = $3::text, client_ref = coalesce($4::text, client_ref),"
	        " size_bytes = $5::bigint, done_bytes = $6::bigint, rate_bps = $7::bigint, eta_s = $8::bigint,"
	        " client_path = $9::text, volume_id = $10::bigint, rel_path = $11::text, message = $12::text,"
	        " problem = $13::text, missing_polls = $16::integer,"
	        " location_inode = CASE WHEN $17::boolean THEN $20::bigint WHEN $11::text IS NULL THEN NULL"
	        "  ELSE location_inode END,"
	        " seen_at = CASE WHEN $14::boolean THEN now() ELSE seen_at END,"
	        " progress_at = CASE WHEN $15::boolean OR progress_at IS NULL THEN now() ELSE progress_at END,"
	        " stalled = protocol = 'torrent' AND $3::text = 'downloading' AND $14::boolean AND NOT $15::boolean"
	        "  AND $9::text IS NULL AND progress_at IS NOT NULL AND progress_at < now() - $19::bigint * interval '1 second',"
	        " sent_at = CASE WHEN $4::text IS NOT NULL THEN coalesce(sent_at, now()) ELSE sent_at END,"
	        " completed_at = CASE WHEN $17::boolean THEN now() ELSE completed_at END,"
	        " finished_at = CASE WHEN $18::boolean THEN coalesce(finished_at, now()) ELSE finished_at END,"
	        " updated_at = now()"
	        " WHERE id = $1::bigint AND state = $2::text RETURNING stalled",
	        20, params, &result, error))
		return FALSE;
	*written = axis_db_result_get_n_rows(result) > 0;
	*stalled = FALSE;
	if (*written)
		axis_db_result_get_boolean(result, 0, 0, stalled);
	return TRUE;
}

static AxisDownloadItem *
find_by_ref(
	GPtrArray   *items,
	const gchar *ref
){
	guint i;

	for (i = 0; ref != NULL && i < items->len; i++) {
		AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (item != NULL && strcmp(item->client_ref, ref) == 0)
			return item;
	}
	return NULL;
}

/* An item for a 'sending' record: the same info-hash, or, without one, the
 * same name; never one whose identifier a live record has. */
static AxisDownloadItem *
find_for_sending(
	GPtrArray              *items,
	GHashTable             *taken,
	const AxisDownloadInfo *record
){
	guint i;

	for (i = 0; i < items->len; i++) {
		AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (item == NULL || g_hash_table_contains(taken, item->client_ref))
			continue;
		if (record->info_hash != NULL) {
			if (item->info_hash != NULL && strcmp(item->info_hash, record->info_hash) == 0)
				return item;
			continue;
		}
		/* The name exactly as the client gives it (a cleaned one could be
		 * another download's of a name that differs in its white space). */
		if (item->own_name != NULL && strcmp(item->own_name, record->release_name) == 0)
			return item;
	}
	return NULL;
}

static gboolean
store_reach(
	AxisDownloads            *self,
	gint64                    client_id,
	const GError             *failure,
	GError                  **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	AxisDownloadClientState state = AXIS_DOWNLOAD_CLIENT_OK;
	g_autofree gchar *message = NULL;
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	if (failure != NULL)
		message = axis_download_contact_problem(failure, &state);
	ok = axis_download_store_client_state(conn, client_id, state, message, NULL, -1, failure == NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gboolean
poll_call(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
){
	Poll *poll = data;
	AxisDownloadPollReport *report = poll->report;
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GPtrArray) maps = NULL;
	g_autoptr(GPtrArray) changes = g_ptr_array_new_with_free_func(change_free);
	g_autoptr(GHashTable) taken = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) held = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GError) failure = NULL;
	g_autofree gchar *client_text = axis_download_int_text(target->info->id);
	g_autofree gchar *client_problem = NULL;
	g_autofree gchar *held_why = NULL;
	gint64 now_us = g_get_real_time();
	gint64 stall_after_s = axis_download_setting_duration(self, AXIS_DOWNLOAD_SETTING_STALL_AFTER, 3600);
	gint64 gone_after = axis_download_setting_integer(self, AXIS_DOWNLOAD_SETTING_GONE_AFTER, 3);
	AxisDbConn *db;
	gboolean ok;
	guint i;

	conn->list_partial = FALSE;
	if (!kind->list(kind, conn, target->info->category, &items, &failure)) {
		g_clear_pointer(&items, g_ptr_array_unref);
		if (g_error_matches(failure, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED)) {
			g_propagate_error(error, g_steal_pointer(&failure));
			return FALSE;
		}
		/* Nothing about the downloads is concluded from a client that
		 * did not answer: they keep their state, and the page its age. */
		return store_reach(self, target->info->id, failure, error);
	}
	report->reached = TRUE;
	/* A list the kind says is not whole (cut at the most one call reads, or
	 * holding an entry of Axis's category it could not read): what it does
	 * not show is not concluded to be gone (nor, being sent, failed). */
	report->partial = conn->list_partial;
	if (items == NULL)
		items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_item_free);
	if (!store_reach(self, target->info->id, NULL, error))
		return FALSE;

	/* Only Axis's category, and only items of the right form. */
	for (i = 0; i < items->len; i++) {
		AxisDownloadItem *item = g_ptr_array_index(items, i);

		/* One whose identifier can be read and nothing else, of Axis's
		 * category or of one that cannot be read either: that download is
		 * kept as it was (and said), the others are judged as ever. */
		if (item != NULL && item->unreadable != NULL && axis_download_ref_valid(item->client_ref) &&
		    (item->category == NULL || g_ascii_strcasecmp(item->category, target->info->category) == 0)) {
			if (held_why == NULL)
				held_why = g_strdup_printf("%s: %s", item->client_ref, item->unreadable);
			g_hash_table_add(held, g_strdup(item->client_ref));
			axis_download_item_free(item);
			items->pdata[i] = NULL;
			continue;
		}
		/* One of Axis's category (or of none said) that cannot be used makes
		 * the list not whole: it may be one of Axis's downloads. */
		if ((item == NULL || item->category == NULL ||
		     g_ascii_strcasecmp(item->category, target->info->category) == 0) && !axis_download_item_check(item)) {
			report->partial = TRUE;
			axis_download_item_free(item);
			items->pdata[i] = NULL;
			continue;
		}
		if (item == NULL || g_ascii_strcasecmp(item->category, target->info->category) != 0) {
			axis_download_item_free(item);
			items->pdata[i] = NULL;
			continue;
		}
		report->listed++;
	}
	/* For how long it has not been whole: said on the Downloads page once
	 * it is long. */
	axis_download_note_partial(self, target->info->id, report->partial);

	db = axis_db_pool_acquire(self->pool, error);
	if (db == NULL)
		return FALSE;
	ok = axis_download_store_records(db,
		"d.client_id = $1::bigint AND d.state IN ('sending', 'queued', 'downloading', 'paused')", client_text,
		"d.id", 10000, &records, error) &&
		/* A built-in kind's paths are Axis's own: none is mapped (@maps stays
		 * NULL, which axis_download_locate_full() takes for that). */
		(kind->builtin || axis_download_store_maps(db, target->info->id, &maps, error));
	axis_db_pool_release(self->pool, db);
	if (!ok)
		return FALSE;

	/* Identifiers live records hold, which a 'sending' one cannot adopt. */
	for (i = 0; i < records->len; i++) {
		const AxisDownloadInfo *record = g_ptr_array_index(records, i);

		if (record->client_ref != NULL)
			g_hash_table_add(taken, record->client_ref);
	}

	for (i = 0; i < records->len; i++) {
		AxisDownloadInfo *record = g_ptr_array_index(records, i);
		AxisDownloadItem *item;
		Change *change;

		if (record->state == AXIS_DOWNLOAD_SENDING) {
			if (axis_download_is_sending(self, record->id) ||
			    now_us - record->created_us < (gint64)AXIS_DOWNLOAD_RECONCILE_AFTER_S * G_USEC_PER_SEC)
				continue;
			item = find_for_sending(items, taken, record);
			/* Not in a list that is not whole: it may be in what was not shown. */
			if (item == NULL && report->partial) {
				report->unseen++;
				continue;
			}
			/* Nor while an entry that cannot be read may be it (by its hash, or
			 * any, for one known by its name only). */
			if (item == NULL && g_hash_table_size(held) > 0 &&
			    (record->info_hash == NULL || g_hash_table_contains(held, record->info_hash))) {
				report->held++;
				continue;
			}
			change = change_new(record);
			if (item != NULL) {
				change->client_ref = g_strdup(item->client_ref);
				g_hash_table_add(taken, item->client_ref);
				take_item(self, maps, kind->builtin, change, item, report);
				report->adopted++;
			} else {
				change->state = AXIS_DOWNLOAD_FAILED;
				g_free(change->problem);
				change->problem = g_strdup("Axis could not tell whether the client took this, and the client does "
					"not have it; it was not sent again.");
				change->finished_now = TRUE;
				report->not_found++;
			}
			g_ptr_array_add(changes, change);
			continue;
		}
		item = find_by_ref(items, record->client_ref);
		/* Its entry cannot be read: it keeps its state, not a miss. */
		if (item == NULL && record->client_ref != NULL && g_hash_table_contains(held, record->client_ref)) {
			report->held++;
			continue;
		}
		/* Not in a list that is not whole: not a miss (it may be in what was
		 * not shown); it keeps its state. */
		if (item == NULL && report->partial) {
			report->unseen++;
			continue;
		}
		change = change_new(record);
		if (item != NULL)
			take_item(self, maps, kind->builtin, change, item, report);
		else
			change->missing_polls = -1;	/* one more miss, counted in place below */
		g_ptr_array_add(changes, change);
	}
	for (i = 0; i < items->len; i++) {
		const AxisDownloadItem *item = g_ptr_array_index(items, i);

		if (item != NULL && !g_hash_table_contains(taken, item->client_ref))
			report->ignored++;
	}

	db = axis_db_pool_acquire(self->pool, error);
	if (db == NULL)
		return FALSE;
	ok = axis_db_conn_begin(db, error);
	for (i = 0; ok && i < changes->len; i++) {
		Change *change = g_ptr_array_index(changes, i);
		gboolean written = FALSE;

		if (change->missing_polls == -1) {
			/* Not listed: one more miss; gone after enough of them. */
			g_autoptr(AxisDbResult) result = NULL;
			g_autofree gchar *id_text = axis_download_int_text(change->before->id);
			g_autofree gchar *limit = axis_download_int_text(gone_after);
			const gchar *params[3];
			gboolean gone = FALSE;

			params[0] = id_text;
			params[1] = axis_download_state_to_string(change->before->state);
			params[2] = limit;
			ok = axis_db_conn_exec(db,
				"UPDATE download SET missing_polls = missing_polls + 1, rate_bps = NULL, eta_s = NULL,"
				" state = CASE WHEN missing_polls + 1 >= $3::integer THEN 'gone' ELSE state END,"
				" finished_at = CASE WHEN missing_polls + 1 >= $3::integer THEN now() ELSE finished_at END,"
				" updated_at = CASE WHEN missing_polls + 1 >= $3::integer OR rate_bps IS NOT NULL THEN now()"
				"  ELSE updated_at END"
				" WHERE id = $1::bigint AND state = $2::text RETURNING state = 'gone'",
				3, params, &result, error);
			if (ok && axis_db_result_get_n_rows(result) > 0) {
				axis_db_result_get_boolean(result, 0, 0, &gone);
				if (gone) {
					report->gone++;
					poll->set_changed = TRUE;
					g_array_append_val(poll->changed, change->before->id);
				} else if (change->before->rate_bps >= 0) {
					g_array_append_val(poll->changed, change->before->id);
				}
			}
			continue;
		}
		/* Seen: written for its time, if nothing else. */
		{
			gboolean stalled = FALSE;
			gboolean changed;

			ok = write_change(db, change, stall_after_s, &written, &stalled, error);
			changed = differs(change) || stalled != change->before->stalled;
			if (ok && written && changed) {
				report->updated++;
				g_array_append_val(poll->changed, change->before->id);
				if (axis_download_state_is_active(change->state) !=
				    axis_download_state_is_active(change->before->state) || change->client_ref != NULL)
					poll->set_changed = TRUE;
			}
		}
		if (change->reason != NULL && client_problem == NULL)
			client_problem = g_strdup(change->reason);
	}
	if (ok) {
		g_autofree gchar *line = NULL;

		if (report->waiting > 0)
			line = g_strdup_printf("%u finished %s be imported yet: %s%s.", report->waiting,
				report->waiting == 1 ? "download cannot" : "downloads cannot", client_problem != NULL ? client_problem :
				"see the Downloads page", report->waiting > 1 ? " (the first)" : "");
		else if (report->unseen > 0)
			line = g_strdup_printf("Its list of Axis's downloads is not whole (it holds more than Axis reads in one call, "
				"or an entry whose identifier Axis could not read), and %u %s not in what it showed: %s kept as %s, never "
				"taken for gone.", report->unseen, report->unseen == 1 ? "download Axis follows was" :
				"downloads Axis follows were", report->unseen == 1 ? "it is" : "they are",
				report->unseen == 1 ? "it was" : "they were");
		/* Entries it lists that cannot be read: those downloads kept. */
		if (report->held > 0) {
			g_autofree gchar *held_line = g_strdup_printf("Axis cannot read its entries for %u of the downloads Axis "
				"follows there (the first: %s): %s kept as %s, the others are followed as ever.", report->held,
				held_why != NULL ? held_why : "an entry", report->held == 1 ? "that download is" : "those downloads are",
				report->held == 1 ? "it was" : "they were");
			gchar *both = line != NULL ? g_strdup_printf("%s %s", line, held_line) : g_strdup(held_line);

			g_free(line);
			line = both;
		}
		/* Its folder for Axis shared with another client, as last found. */
		{
			g_autofree gchar *warning = axis_download_folder_warning(self, target->info->id);

			if (warning != NULL) {
				gchar *both = line != NULL ? g_strdup_printf("%s %s", line, warning) : g_strdup(warning);

				g_free(line);
				line = both;
			}
		}
		ok = axis_download_store_client_problem(db, target->info->id, line, error);
	}
	ok = ok && axis_db_conn_commit(db, error);
	if (!ok)
		axis_db_conn_rollback(db, NULL);
	axis_db_pool_release(self->pool, db);
	return ok;
}

gboolean
axis_downloads_poll_client(
	AxisDownloads           *self,
	gint64                   client_id,
	GCancellable            *cancellable,
	AxisDownloadPollReport  *report,
	GError                 **error
){
	AxisDownloadPollReport local_report;
	g_autoptr(GError) local = NULL;
	Poll poll;
	gboolean ok;

	memset(&local_report, 0, sizeof(local_report));
	memset(&poll, 0, sizeof(poll));
	poll.cancellable = cancellable;
	poll.report = report != NULL ? report : &local_report;
	memset(poll.report, 0, sizeof(*poll.report));
	poll.changed = g_array_new(FALSE, FALSE, sizeof(gint64));
	poll.client_id = client_id;
	ok = axis_download_with_client(self, client_id, cancellable, poll_call, &poll, NULL, &local);
	if (!ok && local != NULL && local->domain == AXIS_DOWNLOAD_ERROR &&
	    !g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND) &&
	    !g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED)) {
		/* The connection could not be made (its kind is gone, its address
		 * no longer parses): recorded on the client, not a failure. */
		g_autoptr(GError) stored = NULL;

		ok = store_reach(self, client_id, local, &stored);
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&stored));
			g_array_unref(poll.changed);
			return FALSE;
		}
	} else if (!ok) {
		g_propagate_error(error, g_steal_pointer(&local));
	}
	if (ok)
		axis_download_notify(self, poll.changed, poll.set_changed);
	g_array_unref(poll.changed);
	return ok;
}

/* ── The job ────────────────────────────────────────────────────── */

typedef struct
{
	gint64   id;
	gboolean active;
} Due;

/* The clients to follow: every one switched on, and one switched off while it
 * still has downloads of Axis's in progress.  Switching a client off means
 * that no new download is sent to it (choose_client()); what it has already
 * is followed to the end, completed and imported. */
static gboolean
followed_clients(
	AxisDownloads  *self,
	GArray        **clients,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool,
	        "SELECT c.id, a.active FROM download_client c, LATERAL (SELECT EXISTS (SELECT 1 FROM download d"
	        " WHERE d.client_id = c.id AND d.state IN ('sending', 'queued', 'downloading', 'paused')) AS active) a"
	        /* A built-in kind's client (no address) only while it has work. */
	        " WHERE (c.enabled AND c.address <> '') OR a.active ORDER BY c.id", 0, NULL, &result, error))
		return FALSE;
	*clients = g_array_new(FALSE, TRUE, sizeof(Due));
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		Due due;

		memset(&due, 0, sizeof(due));
		axis_db_result_get_int64(result, i, 0, &due.id);
		axis_db_result_get_boolean(result, i, 1, &due.active);
		g_array_append_val(*clients, due);
	}
	return TRUE;
}

gboolean
axis_downloads_run_tracking(
	AxisDownloads   *self,
	AxisJobContext  *context,
	guint            run_ms,
	GError         **error
){
	gint64 start = g_get_monotonic_time();
	gint64 end = start + (gint64)(run_ms != 0 ? run_ms : AXIS_DOWNLOAD_TRACK_RUN_S * 1000) * 1000;
	GCancellable *cancellable = context != NULL ? axis_job_context_get_cancellable(context) : NULL;

	for (;;) {
		g_autoptr(GArray) clients = NULL;
		gint64 active_us = axis_download_setting_duration(self, AXIS_DOWNLOAD_SETTING_ACTIVE_INTERVAL, 5) *
			G_USEC_PER_SEC;
		gint64 idle_us = axis_download_setting_duration(self, AXIS_DOWNLOAD_SETTING_IDLE_INTERVAL, 60) *
			G_USEC_PER_SEC;
		gint64 now = g_get_monotonic_time();
		gint64 next = end;
		guint i;

		if ((context != NULL && axis_job_context_is_cancelled(context)) || g_atomic_int_get(&self->stopped))
			break;
		if (!followed_clients(self, &clients, error))
			return FALSE;
		/* Nothing to follow: done, rather than holding the queue's place
		 * for the rest of the minute. */
		if (clients->len == 0)
			break;
		for (i = 0; i < clients->len; i++) {
			Due *due = &g_array_index(clients, Due, i);
			AxisDownloadSlot *state = axis_download_slot_get(self, due->id);
			g_autoptr(GError) local = NULL;
			AxisDownloadPollReport report;
			gint64 due_us;

			g_mutex_lock(&self->lock);
			due_us = state->next_poll_us;
			g_mutex_unlock(&self->lock);
			if (context == NULL || due_us <= now) {
				gboolean active;

				if (!axis_downloads_poll_client(self, due->id, cancellable, &report, &local)) {
					axis_download_slot_unref(state);
					if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND))
						continue;
					if (g_error_matches(local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_CANCELLED))
						return TRUE;
					g_propagate_error(error, g_steal_pointer(&local));
					return FALSE;
				}
				/* In progress, or just finished something: look again soon. */
				active = due->active || report.waiting > 0 || report.completed > 0 || report.adopted > 0;
				g_mutex_lock(&self->lock);
				state->next_poll_us = g_get_monotonic_time() + (active ? active_us : idle_us);
				due_us = state->next_poll_us;
				g_mutex_unlock(&self->lock);
			}
			next = MIN(next, due_us);
			axis_download_slot_unref(state);
		}
		if (context == NULL)
			break;
		axis_job_context_set_progress(context, CLAMP((gdouble)(g_get_monotonic_time() - start) /
			(gdouble)(end - start), 0.0, 1.0), clients->len == 1 ? "Following 1 download client" :
			"Following the download clients");
		now = g_get_monotonic_time();
		if (now >= end)
			break;
		/* A new client or a new download is picked up within a second. */
		if (axis_job_context_wait(context, (guint)CLAMP((MIN(next, end) - now) / 1000, 100, 1000)))
			break;
	}
	return TRUE;
}
