/*
 * axis-compat-history.c - the import of the server's play history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The history says who played what and when, and nothing about how it
 * played: it adds rows to compat_play and never to compat_evidence.
 */

#include "compat/axis-compat-private.h"

#include <string.h>

#include "compat/axis-compat-plex.h"
#include "jobs/axis-job-error.h"

typedef struct
{
	AxisCompat           *self;
	const AxisCompatKind *kind;
	GHashTable           *server_devices;	/* plex id (gint64 *) -> AxisCompatDevice */
	GHashTable           *resolved;		/* plex id (gint64 *) -> client id */
} Import;

static guint
int64_hash(gconstpointer key)
{
	return g_int64_hash(key);
}

static gboolean
int64_equal(
	gconstpointer a,
	gconstpointer b
){
	return g_int64_equal(a, b);
}

static gchar *
int_text(gint64 value)
{
	return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

/* The id sessions use for the device the history calls @plex_id, creating
 * the device if it is new. */
static const gchar *
resolve_device(
	Import     *import,
	AxisDbConn *conn,
	gint64      plex_id,
	gint64      viewed_at,
	GError    **error
){
	const AxisCompatDevice *known = g_hash_table_lookup(import->server_devices, &plex_id);
	const gchar *cached = g_hash_table_lookup(import->resolved, &plex_id);
	g_autofree gchar *client_id = NULL;
	g_autofree gchar *id_text = int_text(plex_id);
	g_autofree gchar *when = int_text(viewed_at);
	const gchar *params[6];
	gint64 *key;

	if (cached != NULL)
		return cached;
	if (known != NULL && known->client_id != NULL)
		client_id = g_strdup(known->client_id);
	else
		client_id = g_strdup_printf("plex:%" G_GINT64_FORMAT, plex_id);
	params[0] = client_id;
	params[1] = id_text;
	params[2] = known != NULL ? known->name : NULL;
	params[3] = known != NULL ? known->platform : NULL;
	params[4] = when;
	params[5] = import->kind->name;
	if (!axis_db_conn_exec(conn,
	        "INSERT INTO compat_device (client_id, plex_id, name, platform, last_seen, server_kind) VALUES ($1::text,"
	        " $2::bigint, $3::text, $4::text, to_timestamp($5::double precision), $6::text)"
	        " ON CONFLICT (server_kind, client_id) DO UPDATE SET plex_id = EXCLUDED.plex_id,"
	        " name = coalesce(compat_device.name, EXCLUDED.name),"
	        " platform = coalesce(compat_device.platform, EXCLUDED.platform),"
	        " last_seen = greatest(compat_device.last_seen, EXCLUDED.last_seen)",
	        6, params, NULL, error))
		return NULL;
	key = g_new(gint64, 1);
	*key = plex_id;
	g_hash_table_insert(import->resolved, key, g_steal_pointer(&client_id));
	return g_hash_table_lookup(import->resolved, &plex_id);
}

/* Stores one page of plays; counts the new ones. */
static gboolean
store_page(
	Import                  *import,
	const AxisCompatHistory *history,
	gint64                   horizon,
	guint                   *stored,
	gboolean                *reached_horizon,
	GError                 **error
){
	AxisDbConn *conn = axis_db_pool_acquire(import->self->pool, error);
	GString *sql = g_string_new("INSERT INTO compat_play (device_id, library_key, rating_key, viewed_at, account_id,"
		" server_kind) VALUES ");
	GPtrArray *texts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(AxisDbResult) result = NULL;
	guint n = 0;
	guint i;
	gboolean ok = FALSE;

	*stored = 0;
	if (conn == NULL) {
		g_string_free(sql, TRUE);
		g_ptr_array_unref(texts);
		return FALSE;
	}
	if (!axis_db_conn_begin(conn, error))
		goto out;
	for (i = 0; i < history->plays->len; i++) {
		const AxisCompatPlay *p = g_ptr_array_index(history->plays, i);
		const gchar *client_id;
		gchar *when;

		if (p->viewed_at >= 0 && p->viewed_at < horizon)
			*reached_horizon = TRUE;
		if (p->device_id < 0 || p->library_key == NULL || p->rating_key == NULL || p->viewed_at < 0 ||
		    p->viewed_at < horizon)
			continue;
		client_id = resolve_device(import, conn, p->device_id, p->viewed_at, error);
		if (client_id == NULL)
			goto out;
		when = int_text(p->viewed_at);
		if (n > 0)
			g_string_append_c(sql, ',');
		g_string_append_printf(sql, "($%u::text, $%u::text, $%u::text, to_timestamp($%u::double precision),"
			" $%u::text, $%u::text)", n * 6 + 1, n * 6 + 2, n * 6 + 3, n * 6 + 4, n * 6 + 5, n * 6 + 6);
		g_ptr_array_add(texts, g_strdup(client_id));
		g_ptr_array_add(texts, g_strdup(p->library_key));
		g_ptr_array_add(texts, g_strdup(p->rating_key));
		g_ptr_array_add(texts, when);
		g_ptr_array_add(texts, g_strdup(p->account_id != NULL ? p->account_id : ""));
		g_ptr_array_add(texts, g_strdup(import->kind->name));
		n++;
	}
	if (n > 0) {
		const gchar **params = g_new0(const gchar *, texts->len + 1);
		gboolean exec_ok;

		for (i = 0; i < texts->len; i++)
			params[i] = g_ptr_array_index(texts, i);
		g_string_append(sql, " ON CONFLICT DO NOTHING");
		exec_ok = axis_db_conn_exec(conn, sql->str, texts->len, params, &result, error);
		g_free(params);
		if (!exec_ok)
			goto out;
		*stored = (guint)axis_db_result_get_affected_rows(result);
	}
	if (!axis_db_conn_commit(conn, error))
		goto out;
	ok = TRUE;
out:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(import->self->pool, conn);
	g_string_free(sql, TRUE);
	g_ptr_array_unref(texts);
	return ok;
}

static gboolean
do_import(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	gboolean                    first,
	GCancellable               *cancellable,
	AxisCompatHistorySummary   *summary,
	GError                    **error
){
	AxisCompatIdentity identity;
	Import import;
	g_autoptr(GPtrArray) devices = NULL;
	g_autofree gchar *problem = NULL;
	gint64 horizon;
	gint64 deadline = g_get_monotonic_time() + (gint64)AXIS_COMPAT_HISTORY_DEADLINE_SECONDS * G_USEC_PER_SEC;
	gsize length;
	const gchar *data;
	guint start = 0;
	gboolean ok = FALSE;
	guint i;

	memset(&import, 0, sizeof(import));
	import.self = self;
	import.kind = connection->kind;
	if (!axis_compat_fetch_identity(self, connection, cancellable, &identity, error))
		return FALSE;
	{
		AxisDbConn *c = axis_db_pool_acquire(self->pool, error);

		if (c == NULL) {
			axis_compat_identity_clear(&identity);
			return FALSE;
		}
		if (!axis_db_conn_begin(c, error) || !axis_compat_adopt_server(c, connection->kind->name, &identity, error) ||
		    !axis_db_conn_commit(c, error)) {
			axis_db_conn_rollback(c, NULL);
			axis_db_pool_release(self->pool, c);
			axis_compat_identity_clear(&identity);
			return FALSE;
		}
		axis_db_pool_release(self->pool, c);
	}
	axis_compat_identity_clear(&identity);

	{
		g_autoptr(GError) local = NULL;

		if (!axis_compat_refresh_libraries(self, connection, cancellable, &local)) {
			if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
				g_propagate_error(error, g_steal_pointer(&local));
				return FALSE;
			}
			g_message("compat: the libraries could not be read: %s", local->message);
		}
	}

	if (!connection->kind->fetch_devices(self, connection, cancellable, &devices, error))
		return FALSE;
	import.server_devices = g_hash_table_new_full(int64_hash, int64_equal, NULL, NULL);
	import.resolved = g_hash_table_new_full(int64_hash, int64_equal, g_free, g_free);
	for (i = 0; i < devices->len; i++) {
		AxisCompatDevice *d = g_ptr_array_index(devices, i);

		if (d->plex_id >= 0)
			g_hash_table_insert(import.server_devices, &d->plex_id, d);
	}

	horizon = g_get_real_time() / G_USEC_PER_SEC - (self->settings != NULL ?
		axis_settings_get_duration(self->settings, AXIS_COMPAT_SETTING_RETENTION) : (gint64)400 * 86400);

	while (summary->pages < AXIS_COMPAT_HISTORY_MAX_PAGES) {
		g_autofree gchar *start_text = int_text(start);
		g_autofree gchar *size_text = int_text(AXIS_COMPAT_HISTORY_PAGE);
		const gchar *headers[5];
		g_autoptr(GBytes) page = NULL;
		AxisCompatHistory history;
		gboolean reached = FALSE;
		guint stored = 0;

		if (g_cancellable_is_cancelled(cancellable)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
			goto out;
		}
		if (g_get_monotonic_time() > deadline)
			goto out_partial;
		headers[0] = "X-Plex-Container-Start";
		headers[1] = start_text;
		headers[2] = "X-Plex-Container-Size";
		headers[3] = size_text;
		headers[4] = NULL;
		if (!axis_compat_fetch(self, connection, "/status/sessions/history/all?sort=viewedAt:desc", headers,
		        cancellable, &page, error))
			goto out;
		data = g_bytes_get_data(page, &length);
		memset(&history, 0, sizeof(history));
		g_clear_pointer(&problem, g_free);
		if (!axis_compat_plex_parse_history(data, length, &history, &problem)) {
			axis_compat_set_server_error(error, "the history request", problem);
			goto out;
		}
		summary->pages++;
		summary->seen += history.plays->len;
		if (!store_page(&import, &history, horizon, &stored, &reached, error)) {
			axis_compat_history_clear(&history);
			goto out;
		}
		summary->plays += stored;
		start += history.plays->len;
		/* The end, the retention horizon, or (after the first import)
		 * a page with nothing new: everything newer is in. */
		if (history.plays->len == 0 || reached || (!first && stored == 0) ||
		    (history.total >= 0 ? (gint64)start >= history.total : history.plays->len < AXIS_COMPAT_HISTORY_PAGE)) {
			axis_compat_history_clear(&history);
			summary->complete = TRUE;
			break;
		}
		axis_compat_history_clear(&history);
	}
out_partial:
	ok = TRUE;
out:
	g_hash_table_unref(import.server_devices);
	g_hash_table_unref(import.resolved);
	return ok;
}

gboolean
axis_compat_import_history(
	AxisCompat               *self,
	gboolean                  force,
	GCancellable             *cancellable,
	AxisCompatHistorySummary *summary,
	GError                  **error
){
	AxisCompatHistorySummary local_summary;
	AxisCompatConnection connection;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const AxisCompatKind *kind = axis_compat_kind_find(AXIS_COMPAT_KIND_PLEX);
	const gchar *params[1];
	gboolean first = TRUE;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	/* Plex is the one kind that keeps a history (kind->has_history); a
	 * server with none is learned from only while its clients play. */
	if (summary == NULL)
		summary = &local_summary;
	memset(summary, 0, sizeof(*summary));
	if (!kind->has_history || !axis_compat_connection_load(self, kind, &connection, &local)) {
		summary->skipped = TRUE;
		return TRUE;
	}
	params[0] = kind->name;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT history_at IS NULL, coalesce(extract(epoch FROM now() - history_at), 0)::double precision"
	        " FROM compat_state WHERE server_kind = $1::text", 1, params, &result, error)) {
		axis_compat_connection_clear(&connection);
		return FALSE;
	}
	axis_db_result_get_boolean(result, 0, 0, &first);
	if (!first && !force && g_ascii_strtod(axis_db_result_get_text(result, 0, 1), NULL) <
	    AXIS_COMPAT_HISTORY_REFRESH_SECONDS) {
		summary->skipped = TRUE;
		axis_compat_connection_clear(&connection);
		return TRUE;
	}
	ok = do_import(self, &connection, first, cancellable, summary, &local);
	if (ok) {
		if (summary->complete) {
			g_autoptr(GError) ignored = NULL;

			axis_db_pool_exec(self->pool,
				"UPDATE compat_state SET history_at = now(), history_error = NULL WHERE server_kind = $1::text", 1,
				params, NULL, &ignored);
		}
		axis_compat_apply_retention(self, NULL, NULL, NULL);
	} else if (g_error_matches(local, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED)) {
		g_propagate_error(error, g_steal_pointer(&local));
	} else {
		g_autofree gchar *clean = axis_http_scrub(local->message, connection.token, 500);

		g_clear_error(&local);
		g_set_error_literal(&local, AXIS_COMPAT_ERROR, AXIS_COMPAT_ERROR_SERVER, clean);
		axis_compat_note_failure(self, kind->name, local, TRUE);
		g_propagate_error(error, g_steal_pointer(&local));
	}
	axis_compat_connection_clear(&connection);
	return ok;
}
