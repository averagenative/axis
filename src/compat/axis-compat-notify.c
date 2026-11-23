/*
 * axis-compat-notify.c - telling the media servers which folder changed:
 * what is recorded, the batches the job sends, and what the page shows
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-notify.h"

#include <string.h>

#include "compat/axis-compat-private.h"
#include "db/axis-db-error.h"
#include "jobs/axis-job-error.h"
#include "library/axis-library-name.h"

/* ── What happened ──────────────────────────────────────────────── */

static const gchar *const change_names[] = { "created", "modified", "deleted" };

const gchar *
axis_compat_change_name(AxisCompatChange change)
{
	return (guint)change < G_N_ELEMENTS(change_names) ? change_names[change] : "modified";
}

AxisCompatChange
axis_compat_change_merge(
	AxisCompatChange earlier,
	AxisCompatChange later
){
	if (later == AXIS_COMPAT_CHANGE_DELETED)
		return AXIS_COMPAT_CHANGE_DELETED;
	if (earlier == AXIS_COMPAT_CHANGE_DELETED)
		return AXIS_COMPAT_CHANGE_MODIFIED;
	if (earlier == AXIS_COMPAT_CHANGE_CREATED || later == AXIS_COMPAT_CHANGE_CREATED)
		return AXIS_COMPAT_CHANGE_CREATED;
	return AXIS_COMPAT_CHANGE_MODIFIED;
}

/* ── Recording ──────────────────────────────────────────────────── */

static gboolean
declared(
	AxisSettings *settings,
	const gchar  *key
){
	return settings != NULL && key != NULL && axis_settings_lookup(settings, key, NULL) != NULL;
}

/* Whether a kind of server is set up (an address and a token) and not
 * switched off. */
static gboolean
kind_wanted(
	AxisSettings         *settings,
	const AxisCompatKind *kind
){
	g_autofree gchar *address = NULL;
	g_autofree gchar *token = NULL;

	if (!declared(settings, kind->address_setting) || !declared(settings, kind->token_setting))
		return FALSE;
	address = axis_settings_get_string(settings, kind->address_setting);
	token = axis_settings_get_string(settings, kind->token_setting);
	if (address == NULL || address[0] == '\0' || token == NULL || token[0] == '\0')
		return FALSE;
	if (token != NULL)
		memset(token, 0, strlen(token));
	return !declared(settings, kind->notify_setting) || axis_settings_get_boolean(settings, kind->notify_setting);
}

gboolean
axis_compat_notify_record(
	AxisDbPool        *pool,
	AxisSettings      *settings,
	const gchar       *folder,
	AxisCompatChange   change,
	GError           **error
){
	g_autofree gchar *clean = axis_compat_path_normalize(folder, NULL);
	g_autofree gchar *quiet = NULL;
	g_autofree gchar *max_wait = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_MAX_WAIT_SECONDS);
	guint i;

	if (settings == NULL || clean == NULL || strcmp(clean, "/") == 0)
		return TRUE;
	quiet = g_strdup_printf("%" G_GINT64_FORMAT, declared(settings, AXIS_COMPAT_SETTING_NOTIFY_QUIET) ?
		axis_settings_get_duration(settings, AXIS_COMPAT_SETTING_NOTIFY_QUIET) : (gint64)30);
	for (i = 0; axis_compat_kind_at(i) != NULL; i++) {
		const AxisCompatKind *kind = axis_compat_kind_at(i);
		const gchar *params[5];

		if (!kind_wanted(settings, kind))
			continue;
		params[0] = kind->name;
		params[1] = clean;
		params[2] = axis_compat_change_name(change);
		params[3] = quiet;
		params[4] = max_wait;
		/* A row still waiting for this server and folder takes the event:
		 * what happened merges (axis_compat_change_merge()), its tries start
		 * again, and it is due after the quiet time, or at the latest the
		 * maximum wait after the folder first changed. */
		if (!axis_db_pool_exec(pool,
		        "INSERT INTO compat_notify (server_kind, folder, change, due_at)"
		        " VALUES ($1::text, $2::text, $3::text, now() + $4::double precision * interval '1 second')"
		        " ON CONFLICT (server_kind, folder) WHERE state = 'pending' DO UPDATE SET"
		        " change = CASE"
		        "  WHEN EXCLUDED.change = 'deleted' THEN 'deleted'"
		        "  WHEN compat_notify.change = 'deleted' THEN 'modified'"
		        "  WHEN compat_notify.change = 'created' OR EXCLUDED.change = 'created' THEN 'created'"
		        "  ELSE 'modified' END,"
		        " events = compat_notify.events + 1, attempts = 0, last_at = now(), note = NULL,"
		        " due_at = least(now() + $4::double precision * interval '1 second',"
		        "  compat_notify.first_at + $5::double precision * interval '1 second')",
		        5, params, NULL, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_compat_notify_record_import(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	gint64         import_id,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) bytes = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, import_id);
	g_autofree gchar *folder = NULL;
	const gchar *params[1];
	const gchar *root;
	const gchar *stored;

	if (settings == NULL)
		return TRUE;
	params[0] = id;
	if (!axis_db_pool_exec(pool,
	        "SELECT CASE WHEN r.rel_path = '' THEN v.path ELSE v.path || '/' || r.rel_path END, i.dest_dir, i.upgrade"
	        " FROM import i JOIN library_root r ON r.id = i.root_id JOIN volume v ON v.id = r.volume_id"
	        " WHERE i.id = $1::bigint AND i.dest_dir IS NOT NULL",
	        1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	root = axis_db_result_get_text(result, 0, 0);
	stored = axis_db_result_get_text(result, 0, 1);
	if (root == NULL || stored == NULL || !axis_library_name_decode(stored, &bytes) ||
	    memchr(bytes->str, '\0', bytes->len) != NULL || bytes->len == 0)
		return TRUE;
	folder = g_strconcat(root, "/", bytes->str, NULL);
	return axis_compat_notify_record(pool, settings, folder, g_strcmp0(axis_db_result_get_text(result, 0, 2), "t") == 0 ?
		AXIS_COMPAT_CHANGE_MODIFIED : AXIS_COMPAT_CHANGE_CREATED, error);
}

/* ── Items ──────────────────────────────────────────────────────── */

void
axis_compat_notify_item_free(AxisCompatNotifyItem *item)
{
	if (item == NULL)
		return;
	g_free(item->folder);
	g_free(item->server_folder);
	g_free(item->note);
	g_free(item);
}

/* ── Sending ────────────────────────────────────────────────────── */

typedef struct
{
	gint64  id;
	gchar  *last_at;	/* as the database wrote it, to tell that nothing changed since */
	gint    attempts;
	gboolean too_old;	/* first change more than an hour ago */
} Row;

static void
row_free(Row *row)
{
	g_free(row->last_at);
	g_free(row);
}

static gboolean
finish_row(
	AxisCompat                *self,
	const Row                 *row,
	const AxisCompatNotifyItem *item,
	const gchar               *token,
	AxisCompatNotifySummary   *summary,
	GError                   **error
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, row->id);
	g_autofree gchar *note = item->note != NULL ? axis_http_scrub(item->note, token, 400) : NULL;
	const gchar *params[6];

	params[0] = id;
	params[1] = row->last_at;
	switch (item->result) {
	case AXIS_COMPAT_NOTIFY_SENT:
		params[2] = item->server_folder;
		summary->sent++;
		return axis_db_pool_exec(self->pool,
			"UPDATE compat_notify SET state = 'sent', done_at = now(), attempts = attempts + 1,"
			" sent_folder = $3::text, note = NULL WHERE id = $1::bigint AND state = 'pending'"
			" AND last_at = $2::timestamptz", 3, params, NULL, error);
	case AXIS_COMPAT_NOTIFY_UNMAPPED:
		params[2] = note;
		summary->unmapped++;
		return axis_db_pool_exec(self->pool,
			"UPDATE compat_notify SET state = 'unmapped', done_at = now(), note = $3::text"
			" WHERE id = $1::bigint AND state = 'pending' AND last_at = $2::timestamptz", 3, params, NULL, error);
	default: {
		/* Tried again after a minute, then twice as long each time; dropped
		 * when the tries run out or the first change is over an hour old. */
		gboolean drop = row->attempts + 1 >= AXIS_COMPAT_NOTIFY_MAX_ATTEMPTS || row->too_old;
		g_autofree gchar *delay = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_FIRST_RETRY_SECONDS <<
			MIN(row->attempts, 10));
		g_autofree gchar *text = drop ? g_strdup_printf("Given up after %d tries: %s", row->attempts + 1,
			note != NULL ? note : "the server did not take it") : g_strdup(note != NULL ? note :
			"the server did not take it");

		params[2] = text;
		params[3] = drop ? "true" : "false";
		params[4] = delay;
		if (drop)
			summary->dropped++;
		else
			summary->failed++;
		return axis_db_pool_exec(self->pool,
			"UPDATE compat_notify SET attempts = attempts + 1, note = left($3::text, 1000),"
			" state = CASE WHEN $4::boolean THEN 'dropped' ELSE 'pending' END,"
			" done_at = CASE WHEN $4::boolean THEN now() END,"
			" due_at = now() + $5::double precision * interval '1 second'"
			" WHERE id = $1::bigint AND state = 'pending' AND last_at = $2::timestamptz",
			5, params, NULL, error);
	}
	}
	return TRUE;
}

/* Marks every due row of @kind as skipped, for @why. */
static gboolean
skip_due(
	AxisCompat              *self,
	const AxisCompatKind    *kind,
	gboolean                 force,
	const gchar             *why,
	AxisCompatNotifySummary *summary,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];

	params[0] = kind->name;
	params[1] = why;
	params[2] = force ? "true" : "false";
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE compat_notify SET state = 'skipped', done_at = now(), note = $2::text"
	        " WHERE server_kind = $1::text AND state = 'pending' AND ($3::boolean OR due_at <= now())", 3, params, &result,
	        error))
		return FALSE;
	summary->skipped += (guint)axis_db_result_get_affected_rows(result);
	return TRUE;
}

static gboolean
run_kind(
	AxisCompat              *self,
	const AxisCompatKind    *kind,
	gboolean                 force,
	GCancellable            *cancellable,
	AxisCompatNotifySummary *summary,
	GError                 **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GPtrArray) records = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_notify_item_free);
	g_autoptr(GPtrArray) sendable = g_ptr_array_new();
	g_autoptr(GPtrArray) map = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_BATCH);
	AxisCompatConnection connection;
	AxisDbConn *conn;
	const gchar *params[3];
	gboolean ok = TRUE;
	guint i;

	if (self->settings != NULL && !kind_wanted(self->settings, kind)) {
		const gchar *why = self->settings != NULL && declared(self->settings, kind->notify_setting) &&
			!axis_settings_get_boolean(self->settings, kind->notify_setting) ?
			"Telling this server which folder changed is switched off." :
			"This server is no longer set up, so it was not told.";

		return skip_due(self, kind, force, why, summary, error);
	}
	params[0] = kind->name;
	params[1] = force ? "true" : "false";
	params[2] = limit;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id, folder, change, last_at::text, attempts, first_at < now() - interval '1 hour'"
	        " FROM compat_notify WHERE server_kind = $1::text AND state = 'pending' AND ($2::boolean OR due_at <= now())"
	        " ORDER BY id LIMIT $3::integer", 3, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	if (!axis_compat_connection_load(self, kind, &connection, &local)) {
		axis_compat_connection_clear(&connection);
		return skip_due(self, kind, force, "This server is no longer set up, so it was not told.", summary, error);
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_compat_connection_clear(&connection);
		return FALSE;
	}
	ok = axis_compat_load_path_map(conn, kind->name, &map, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_compat_connection_clear(&connection);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Row *row = g_new0(Row, 1);
		AxisCompatNotifyItem *item = g_new0(AxisCompatNotifyItem, 1);
		const gchar *change = axis_db_result_get_text(rows, i, 2);
		guint c;

		axis_db_result_get_int64(rows, i, 0, &row->id);
		row->last_at = g_strdup(axis_db_result_get_text(rows, i, 3));
		{
			gint64 attempts = 0;

			axis_db_result_get_int64(rows, i, 4, &attempts);
			row->attempts = (gint)attempts;
		}
		row->too_old = g_strcmp0(axis_db_result_get_text(rows, i, 5), "t") == 0;
		item->id = row->id;
		item->folder = g_strdup(axis_db_result_get_text(rows, i, 1));
		item->change = AXIS_COMPAT_CHANGE_MODIFIED;
		for (c = 0; c < G_N_ELEMENTS(change_names); c++) {
			if (g_strcmp0(change, change_names[c]) == 0)
				item->change = (gint)c;
		}
		item->server_folder = axis_compat_path_map_reverse(map, item->folder);
		if (item->server_folder == NULL) {
			item->result = AXIS_COMPAT_NOTIFY_UNMAPPED;
			item->note = g_strdup("No path mapping reaches this folder (or its name there would be too long), so the "
				"server was not asked. Map it under Libraries and paths.");
		} else {
			g_ptr_array_add(sendable, item);
		}
		g_ptr_array_add(records, row);
		g_ptr_array_add(items, item);
	}
	if (sendable->len > 0 && !kind->notify(self, &connection, cancellable, sendable, &local)) {
		/* Cancelled: nothing is decided, the rows wait. */
		axis_compat_connection_clear(&connection);
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	for (i = 0; ok && i < items->len; i++)
		ok = finish_row(self, g_ptr_array_index(records, i), g_ptr_array_index(items, i), connection.token, summary,
			error);
	axis_compat_connection_clear(&connection);
	return ok;
}

gboolean
axis_compat_notify_run(
	AxisCompat              *self,
	gboolean                 force,
	GCancellable            *cancellable,
	AxisCompatNotifySummary *summary,
	GError                 **error
){
	AxisCompatNotifySummary local_summary;
	g_autoptr(AxisDbResult) removed = NULL;
	g_autofree gchar *keep = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_KEEP_SECONDS);
	const gchar *params[1];
	guint i;

	if (summary == NULL)
		summary = &local_summary;
	memset(summary, 0, sizeof(*summary));
	for (i = 0; axis_compat_kind_at(i) != NULL; i++) {
		if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
			return FALSE;
		}
		if (!run_kind(self, axis_compat_kind_at(i), force, cancellable, summary, error))
			return FALSE;
	}
	params[0] = keep;
	if (!axis_db_pool_exec(self->pool,
	        "DELETE FROM compat_notify WHERE state <> 'pending' AND done_at < now() - $1::double precision * interval '1 second'",
	        1, params, &removed, error))
		return FALSE;
	summary->removed = (guint)axis_db_result_get_affected_rows(removed);
	return TRUE;
}

gboolean
axis_compat_run_notify_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	gboolean force = payload != NULL && json_object_get_boolean_member_with_default(payload, "force", FALSE);

	return axis_compat_notify_run(self, force, axis_job_context_get_cancellable(context), NULL, error);
}

/* ── Asking again ───────────────────────────────────────────────── */

/* Row @id waits again, unless a request for its server and folder waits
 * already (then *@already).  @states: the states it may be in. */
static gboolean
wait_again(
	AxisCompat   *self,
	const gchar  *id,
	const gchar  *states,
	gboolean     *changed,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[2];

	*changed = FALSE;
	params[0] = id;
	params[1] = states;
	/* As if its folder had just changed: due now, its tries from nothing,
	 * and its hour of tries from now. */
	if (!axis_db_pool_exec(self->pool,
	        "UPDATE compat_notify n SET state = 'pending', done_at = NULL, attempts = 0, note = NULL, due_at = now(),"
	        " first_at = now(), last_at = now()"
	        " WHERE n.id = $1::bigint AND n.state = ANY ($2::text[])"
	        " AND NOT EXISTS (SELECT 1 FROM compat_notify p WHERE p.server_kind = n.server_kind"
	        "  AND p.folder = n.folder AND p.state = 'pending') RETURNING n.id", 2, params, &result, &local)) {
		/* A request for the folder was recorded meanwhile: that one is sent. */
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION))
			return TRUE;
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	*changed = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

static gboolean
retry_kind(
	AxisCompat           *self,
	const AxisCompatKind *kind,
	guint                *count,
	GError              **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GPtrArray) map = NULL;
	g_autofree gchar *window = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_RETRY_UNMAPPED_SECONDS);
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_COMPAT_NOTIFY_BATCH * 5);
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_compat_load_path_map(conn, kind->name, &map, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	params[0] = kind->name;
	params[1] = window;
	params[2] = limit;
	/* Per folder, its latest request of the window, when that one is
	 * unmapped: an older one whose folder was told since (or waits to be) is
	 * not asked again.  A later request changed later, so it is in the window
	 * too. */
	if (!axis_db_pool_exec(self->pool,
	        "SELECT latest.id, latest.folder FROM (SELECT DISTINCT ON (n.folder) n.id, n.folder, n.state"
	        "  FROM compat_notify n WHERE n.server_kind = $1::text"
	        "  AND n.last_at >= now() - $2::double precision * interval '1 second'"
	        "  ORDER BY n.folder, n.id DESC) latest"
	        " WHERE latest.state = 'unmapped' ORDER BY latest.id LIMIT $3::integer", 3, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		g_autofree gchar *reached = axis_compat_path_map_reverse(map, axis_db_result_get_text(rows, i, 1));
		gboolean changed = FALSE;

		if (reached == NULL)
			continue;
		if (!wait_again(self, axis_db_result_get_text(rows, i, 0), "{unmapped}", &changed, error))
			return FALSE;
		if (changed && count != NULL)
			(*count)++;
	}
	return TRUE;
}

gboolean
axis_compat_notify_retry_unmapped(
	AxisCompat   *self,
	const gchar  *kind,
	guint        *count,
	GError      **error
){
	guint i;

	if (count != NULL)
		*count = 0;
	for (i = 0; axis_compat_kind_at(i) != NULL; i++) {
		const AxisCompatKind *k = axis_compat_kind_at(i);

		if (kind != NULL && g_strcmp0(kind, k->name) != 0)
			continue;
		if (!retry_kind(self, k, count, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_compat_notify_ask_again(
	AxisCompat          *self,
	gint64               id,
	AxisCompatAskAgain  *outcome,
	GError             **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	const gchar *state;
	gboolean changed = FALSE;

	*outcome = AXIS_COMPAT_ASK_AGAIN_ALREADY;
	params[0] = text;
	if (!axis_db_pool_exec(self->pool, "SELECT state FROM compat_notify WHERE id = $1::bigint", 1, params, &found,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(found) == 0) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_NOT_FOUND, "There is no such request.");
		return FALSE;
	}
	state = axis_db_result_get_text(found, 0, 0);
	if (g_strcmp0(state, "unmapped") != 0 && g_strcmp0(state, "dropped") != 0 && g_strcmp0(state, "skipped") != 0) {
		g_set_error_literal(error, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_INVALID, g_strcmp0(state, "sent") == 0 ?
			"That request was sent; there is nothing to ask again." :
			"That request is waiting to be sent already.");
		return FALSE;
	}
	if (!wait_again(self, text, "{unmapped,dropped,skipped}", &changed, error))
		return FALSE;
	*outcome = changed ? AXIS_COMPAT_ASK_AGAIN_WAITING : AXIS_COMPAT_ASK_AGAIN_ALREADY;
	return TRUE;
}

/* ── What the page shows ────────────────────────────────────────── */

void
axis_compat_notify_info_free(AxisCompatNotifyInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->server_kind);
	g_free(info->folder);
	g_free(info->change);
	g_free(info->state);
	g_free(info->sent_folder);
	g_free(info->note);
	g_free(info);
}

gboolean
axis_compat_notify_list(
	AxisCompat  *self,
	guint        limit,
	GPtrArray  **rows,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *count = g_strdup_printf("%u", limit);
	const gchar *params[1];
	guint n;
	guint i;

	*rows = NULL;
	params[0] = count;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id, server_kind, folder, change, state, events, attempts,"
	        " (extract(epoch from first_at) * 1000000)::bigint, (extract(epoch from last_at) * 1000000)::bigint,"
	        " (extract(epoch from due_at) * 1000000)::bigint, (extract(epoch from done_at) * 1000000)::bigint,"
	        " sent_folder, note FROM compat_notify ORDER BY id DESC LIMIT $1::integer", 1, params, &result, error))
		return FALSE;
	*rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_notify_info_free);
	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		AxisCompatNotifyInfo *info = g_new0(AxisCompatNotifyInfo, 1);
		gint64 number = 0;

		axis_db_result_get_int64(result, i, 0, &info->id);
		info->server_kind = g_strdup(axis_db_result_get_text(result, i, 1));
		info->folder = g_strdup(axis_db_result_get_text(result, i, 2));
		info->change = g_strdup(axis_db_result_get_text(result, i, 3));
		info->state = g_strdup(axis_db_result_get_text(result, i, 4));
		axis_db_result_get_int64(result, i, 5, &number);
		info->events = (gint)number;
		axis_db_result_get_int64(result, i, 6, &number);
		info->attempts = (gint)number;
		axis_db_result_get_int64(result, i, 7, &info->first_us);
		axis_db_result_get_int64(result, i, 8, &info->last_us);
		axis_db_result_get_int64(result, i, 9, &info->due_us);
		if (!axis_db_result_is_null(result, i, 10))
			axis_db_result_get_int64(result, i, 10, &info->done_us);
		info->sent_folder = g_strdup(axis_db_result_get_text(result, i, 11));
		info->note = g_strdup(axis_db_result_get_text(result, i, 12));
		g_ptr_array_add(*rows, info);
	}
	return TRUE;
}

void
axis_compat_notify_root_free(AxisCompatNotifyRoot *root)
{
	if (root == NULL)
		return;
	g_free(root->server_kind);
	g_free(root->root_name);
	g_free(root->root_path);
	g_free(root->media_type);
	g_free(root->server_path);
	g_free(root->library);
	g_free(root);
}

gboolean
axis_compat_notify_roots(
	AxisCompat  *self,
	GPtrArray  **roots,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;
	guint k;

	*roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_notify_root_free);
	if (!axis_db_pool_exec(self->pool,
	        "SELECT r.id, r.name, r.media_type, CASE WHEN r.rel_path = '' THEN v.path ELSE v.path || '/' || r.rel_path END"
	        " FROM library_root r JOIN volume v ON v.id = r.volume_id WHERE r.media_type IN ('movies', 'tv')"
	        " ORDER BY r.id", 0, NULL, &rows, error))
		return FALSE;
	for (k = 0; axis_compat_kind_at(k) != NULL; k++) {
		const AxisCompatKind *kind = axis_compat_kind_at(k);
		g_autoptr(AxisDbResult) libraries = NULL;
		g_autoptr(GPtrArray) map = NULL;
		AxisDbConn *conn;
		const gchar *params[1];
		gboolean ok;

		if (!axis_compat_is_configured_kind(self, kind->name))
			continue;
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL)
			return FALSE;
		ok = axis_compat_load_path_map(conn, kind->name, &map, error);
		params[0] = kind->name;
		ok = ok && axis_db_conn_exec(conn,
			"SELECT l.title, f.folder FROM compat_library l LEFT JOIN LATERAL unnest(l.folders) AS f(folder) ON true"
			" WHERE l.server_kind = $1::text ORDER BY l.key, f.folder", 1, params, &libraries, error);
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
			AxisCompatNotifyRoot *root = g_new0(AxisCompatNotifyRoot, 1);
			guint n;

			axis_db_result_get_int64(rows, i, 0, &root->root_id);
			root->server_kind = g_strdup(kind->name);
			root->root_name = g_strdup(axis_db_result_get_text(rows, i, 1));
			root->media_type = g_strdup(axis_db_result_get_text(rows, i, 2));
			root->root_path = g_strdup(axis_db_result_get_text(rows, i, 3));
			root->server_path = axis_compat_path_map_reverse(map, root->root_path);
			root->reach = 0;
			if (root->server_path != NULL) {
				root->reach = 1;
				for (n = 0; n < axis_db_result_get_n_rows(libraries); n++) {
					const gchar *folder = axis_db_result_get_text(libraries, n, 1);

					/* The folder holds the root, or (a library of a part of the
					 * root) lies inside it. */
					if (folder != NULL && (axis_compat_path_holds(folder, root->server_path) ||
					    axis_compat_path_holds(root->server_path, folder))) {
						root->reach = 2;
						root->library = g_strdup(axis_db_result_get_text(libraries, n, 0));
						break;
					}
				}
			}
			g_ptr_array_add(*roots, root);
		}
	}
	return TRUE;
}
