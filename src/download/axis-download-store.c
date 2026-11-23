/*
 * axis-download-store.c - the downloads layer's tables: clients, their
 * secrets and path mappings, and the records of downloads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The secrets table is read in one place only, axis_download_store_secret(),
 * for a connection; a list of clients knows only whether a secret is set.
 * A database error's message can quote a failing row, so a secret is
 * checked before it is written and blanked out of any error that writing
 * it produced.
 */

#include "download/axis-download-private.h"

#include <string.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Reading results ────────────────────────────────────────────── */

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(result, row, column));
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

static gboolean
bool_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── Clients ────────────────────────────────────────────────────── */

static const gchar *const client_sql =
	"SELECT c.id, c.name, c.kind, c.address, c.accept_untrusted, c.category, c.priority, c.enabled,"
	" (s.client_id IS NOT NULL), c.state, c.state_message, c.version,"
	" CASE WHEN c.category_exists IS NULL THEN -1 WHEN c.category_exists THEN 1 ELSE 0 END,"
	" " US("c.last_checked_at") ", " US("c.last_contact_at") ", c.problem,"
	" " US("c.created_at") ", " US("c.updated_at") ","
	" (SELECT count(*) FROM download d WHERE d.client_id = c.id"
	"  AND d.state IN ('sending', 'queued', 'downloading', 'paused')),"
	" (SELECT count(*) FROM download d WHERE d.client_id = c.id"
	"  AND d.state IN ('queued', 'downloading', 'paused') AND d.client_path IS NOT NULL AND d.problem IS NOT NULL),"
	" c.base_path, c.username, coalesce(c.leftover_count, 0)"
	" FROM download_client c LEFT JOIN download_client_secret s ON s.client_id = c.id"
	/* Every client an administrator configured (a built-in kind's client has
	 * no address and is not one of them), or the one asked for. */
	" WHERE CASE WHEN $1::bigint = 0 THEN c.address <> '' ELSE c.id = $1::bigint END"
	" ORDER BY lower(c.name), c.id";

static const gchar *const maps_sql =
	"SELECT m.client_id, m.client_prefix, m.volume_id, v.name, m.rel_path"
	" FROM download_path_map m LEFT JOIN volume v ON v.id = m.volume_id"
	" WHERE ($1::bigint = 0 OR m.client_id = $1::bigint)"
	" ORDER BY m.client_id, m.client_prefix";

gboolean
axis_download_store_clients(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **clients,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) maps = NULL;
	g_autofree gchar *id_text = axis_download_int_text(id);
	const gchar *params[1];
	GPtrArray *out;
	guint n;
	guint i;
	guint m;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, client_sql, 1, params, &result, error) ||
	    !axis_db_conn_exec(conn, maps_sql, 1, params, &maps, error))
		return FALSE;
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_client_info_free);
	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		AxisDownloadClientInfo *client = g_new0(AxisDownloadClientInfo, 1);

		client->id = int_at(result, i, 0, 0);
		client->name = text_at(result, i, 1);
		client->kind = text_at(result, i, 2);
		client->address = text_at(result, i, 3);
		client->accept_untrusted = bool_at(result, i, 4);
		client->category = text_at(result, i, 5);
		if (!axis_download_priority_from_string(axis_db_result_get_text(result, i, 6), &client->priority))
			client->priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
		client->enabled = bool_at(result, i, 7);
		client->secret_set = bool_at(result, i, 8);
		if (!axis_download_client_state_from_string(axis_db_result_get_text(result, i, 9), &client->state))
			client->state = AXIS_DOWNLOAD_CLIENT_UNCHECKED;
		client->state_message = text_at(result, i, 10);
		client->version = text_at(result, i, 11);
		client->category_exists = (gint)int_at(result, i, 12, -1);
		client->last_checked_us = int_at(result, i, 13, 0);
		client->last_contact_us = int_at(result, i, 14, 0);
		client->problem = text_at(result, i, 15);
		client->created_us = int_at(result, i, 16, 0);
		client->updated_us = int_at(result, i, 17, 0);
		client->n_active = (guint)int_at(result, i, 18, 0);
		client->n_waiting = (guint)int_at(result, i, 19, 0);
		client->base_path = text_at(result, i, 20);
		if (client->base_path == NULL)
			client->base_path = g_strdup("");
		client->username = text_at(result, i, 21);
		client->n_leftovers = (guint)int_at(result, i, 22, 0);
		client->plain_http = client->address != NULL && g_str_has_prefix(client->address, "http://");
		client->maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_info_free);
		for (m = 0; m < axis_db_result_get_n_rows(maps); m++) {
			AxisDownloadPathMapInfo *map;

			if (int_at(maps, m, 0, 0) != client->id)
				continue;
			map = g_new0(AxisDownloadPathMapInfo, 1);
			map->client_prefix = text_at(maps, m, 1);
			map->volume_id = int_at(maps, m, 2, 0);
			map->volume_name = text_at(maps, m, 3);
			map->rel_path = text_at(maps, m, 4);
			g_ptr_array_add(client->maps, map);
		}
		g_ptr_array_add(out, client);
	}
	*clients = out;
	return TRUE;
}

gboolean
axis_download_store_secret(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **secret,
	gint64      *set_us,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_download_int_text(id);
	const gchar *params[1];

	*secret = NULL;
	*set_us = 0;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT secret, " US("set_at") " FROM download_client_secret WHERE client_id = $1::bigint",
	        1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0) {
		*secret = text_at(result, 0, 0);
		*set_us = int_at(result, 0, 1, 0);
	}
	return TRUE;
}

gboolean
axis_download_store_client_state(
	AxisDbConn              *conn,
	gint64                   id,
	AxisDownloadClientState  state,
	const gchar             *message,
	const gchar             *version,
	gint                     category_exists,
	gboolean                 contact,
	GError                 **error
){
	g_autofree gchar *id_text = axis_download_int_text(id);
	g_autofree gchar *clean = axis_download_text_clean(message, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	g_autofree gchar *version_clean = axis_download_text_clean(version, 64);
	g_autofree gchar *exists = g_strdup_printf("%d", category_exists);
	const gchar *params[6];

	params[0] = id_text;
	params[1] = axis_download_client_state_to_string(state);
	params[2] = clean;
	params[3] = version_clean;
	params[4] = exists;
	params[5] = contact ? "true" : "false";
	/* Since when it has not been working: begun by the contact that first
	 * finds it so, kept while it stays so whatever the reason becomes, and
	 * cleared by one that works (the right-hand sides read the row as it
	 * was). */
	return axis_db_conn_exec(conn,
		"UPDATE download_client SET state = $2::text, state_message = $3::text,"
		" not_working_since = CASE WHEN $2::text IN ('unreachable', 'refused', 'failing')"
		"  THEN coalesce(not_working_since, now()) END,"
		" version = coalesce($4::text, version),"
		" category_exists = CASE WHEN $5::integer < 0 THEN category_exists ELSE $5::integer = 1 END,"
		" last_checked_at = now(),"
		" last_contact_at = CASE WHEN $6::boolean THEN now() ELSE last_contact_at END"
		" WHERE id = $1::bigint", 6, params, NULL, error);
}

gboolean
axis_download_store_client_problem(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *problem,
	GError      **error
){
	g_autofree gchar *id_text = axis_download_int_text(id);
	g_autofree gchar *clean = axis_download_text_clean(problem, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
	const gchar *params[2];

	params[0] = id_text;
	params[1] = clean;
	return axis_db_conn_exec(conn,
		"UPDATE download_client SET problem = $2::text WHERE id = $1::bigint AND problem IS DISTINCT FROM $2::text",
		2, params, NULL, error);
}

gboolean
axis_download_store_maps(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **maps,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_download_int_text(id);
	const gchar *params[1];
	guint i;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn,
	        "SELECT client_prefix, volume_id, rel_path FROM download_path_map WHERE client_id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	*maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*maps, axis_download_path_map_new(axis_db_result_get_text(result, i, 0),
			int_at(result, i, 1, 0), axis_db_result_get_text(result, i, 2)));
	return TRUE;
}

/* ── Records ────────────────────────────────────────────────────── */

static const gchar *const record_columns =
	"SELECT d.id, coalesce(d.client_id, 0), d.client_name, coalesce(c.state, 'unchecked'),"
	" " US("c.last_contact_at") ", d.client_ref, d.info_hash, d.protocol, d.release_name, coalesce(d.title_id, 0),"
	" d.title_label, d.module, d.state, d.size_bytes, d.done_bytes, d.rate_bps, d.eta_s, d.client_path,"
	" coalesce(d.volume_id, 0), v.name, d.rel_path, d.message, d.problem, d.stalled, " US("d.seen_at") ","
	" " US("d.created_at") ", " US("d.sent_at") ", " US("d.updated_at") ", " US("d.completed_at") ","
	" " US("d.finished_at") ", d.folder_removal, d.folder_removal_text"
	" FROM download d LEFT JOIN download_client c ON c.id = d.client_id LEFT JOIN volume v ON v.id = d.volume_id";

gboolean
axis_download_store_records(
	AxisDbConn   *conn,
	const gchar  *where,
	const gchar  *param,
	const gchar  *order,
	guint         limit,
	GPtrArray   **downloads,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = g_strdup_printf("%s WHERE %s ORDER BY %s LIMIT %u", record_columns, where, order,
		limit > 0 ? limit : 1000);
	const gchar *params[1];
	GPtrArray *out;
	guint i;

	params[0] = param;
	if (!axis_db_conn_exec(conn, sql, param != NULL ? 1 : 0, param != NULL ? params : NULL, &result, error))
		return FALSE;
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisDownloadInfo *d = g_new0(AxisDownloadInfo, 1);

		d->id = int_at(result, i, 0, 0);
		d->client_id = int_at(result, i, 1, 0);
		d->client_name = text_at(result, i, 2);
		if (!axis_download_client_state_from_string(axis_db_result_get_text(result, i, 3), &d->client_state))
			d->client_state = AXIS_DOWNLOAD_CLIENT_UNCHECKED;
		d->client_contact_us = int_at(result, i, 4, 0);
		d->client_ref = text_at(result, i, 5);
		d->info_hash = text_at(result, i, 6);
		if (!axis_download_protocol_from_string(axis_db_result_get_text(result, i, 7), &d->protocol))
			d->protocol = AXIS_DOWNLOAD_USENET;
		d->release_name = text_at(result, i, 8);
		d->title_id = int_at(result, i, 9, 0);
		d->title_label = text_at(result, i, 10);
		d->module = text_at(result, i, 11);
		if (!axis_download_state_from_string(axis_db_result_get_text(result, i, 12), &d->state))
			d->state = AXIS_DOWNLOAD_FAILED;
		d->size_bytes = int_at(result, i, 13, -1);
		d->done_bytes = int_at(result, i, 14, -1);
		d->rate_bps = int_at(result, i, 15, -1);
		d->eta_s = int_at(result, i, 16, -1);
		d->client_path = text_at(result, i, 17);
		d->volume_id = int_at(result, i, 18, 0);
		d->volume_name = text_at(result, i, 19);
		d->rel_path = text_at(result, i, 20);
		d->message = text_at(result, i, 21);
		d->problem = text_at(result, i, 22);
		d->stalled = bool_at(result, i, 23);
		d->seen_us = int_at(result, i, 24, 0);
		d->created_us = int_at(result, i, 25, 0);
		d->sent_us = int_at(result, i, 26, 0);
		d->updated_us = int_at(result, i, 27, 0);
		d->completed_us = int_at(result, i, 28, 0);
		d->finished_us = int_at(result, i, 29, 0);
		d->folder_removal = text_at(result, i, 30);
		d->folder_removal_text = text_at(result, i, 31);
		g_ptr_array_add(out, d);
	}
	*downloads = out;
	return TRUE;
}

/* ── The public reads ───────────────────────────────────────────── */

gboolean
axis_downloads_list_clients(
	AxisDownloads  *self,
	GPtrArray     **clients,
	GError        **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;
	guint i;

	if (conn == NULL)
		return FALSE;
	ok = axis_download_store_clients(conn, 0, clients, error);
	axis_db_pool_release(self->pool, conn);
	/* What this process found of each client's list, kept in memory. */
	for (i = 0; ok && i < (*clients)->len; i++) {
		AxisDownloadClientInfo *client = g_ptr_array_index(*clients, i);

		client->partial_since_us = axis_download_partial_since(self, client->id);
	}
	return ok;
}

gboolean
axis_downloads_get_client(
	AxisDownloads           *self,
	gint64                   id,
	AxisDownloadClientInfo **client,
	GError                 **error
){
	g_autoptr(GPtrArray) clients = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*client = NULL;
	if (id <= 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND, "there is no such download client");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_download_store_clients(conn, id, &clients, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	/* A built-in kind's client is not one an administrator looks at or changes. */
	if (clients->len == 0 || ((const AxisDownloadClientInfo *)g_ptr_array_index(clients, 0))->address[0] == '\0') {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND, "there is no such download client");
		return FALSE;
	}
	*client = g_ptr_array_steal_index(clients, 0);
	(*client)->partial_since_us = axis_download_partial_since(self, id);
	return TRUE;
}

gboolean
axis_downloads_builtin_client(
	AxisDownloads  *self,
	const gchar    *kind_name,
	gint64         *client_id,
	GError        **error
){
	const AxisDownloadKindInfo *kind = axis_downloads_find_kind(self, kind_name);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[2];

	*client_id = 0;
	if (kind == NULL || !kind->builtin) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, "that is not a built-in download client");
		return FALSE;
	}
	params[0] = kind->name;
	params[1] = kind->label;
	/* Made once; two callers at once both find the one row (the name is
	 * unique, and the second insert does nothing). */
	if (!axis_db_pool_exec(self->pool,
	        "INSERT INTO download_client (name, kind, address) SELECT $2::text, $1::text, ''"
	        " WHERE NOT EXISTS (SELECT 1 FROM download_client WHERE kind = $1::text AND address = '')"
	        " ON CONFLICT (name) DO NOTHING", 2, params, NULL, &local) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT id FROM download_client WHERE kind = $1::text AND address = '' ORDER BY id LIMIT 1", 1, params,
	        &result, &local)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS,
			"A download client is called %s already: rename it, and the built-in one can be made.", kind->label);
		return FALSE;
	}
	*client_id = int_at(result, 0, 0, 0);
	return TRUE;
}

gboolean
axis_downloads_count_clients(
	AxisDownloads  *self,
	guint          *count,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*count = 0;
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM download_client WHERE address <> ''", 0, NULL, &result,
	        error))
		return FALSE;
	*count = (guint)int_at(result, 0, 0, 0);
	return TRUE;
}

gboolean
axis_downloads_get(
	AxisDownloads     *self,
	gint64             id,
	AxisDownloadInfo **info,
	GError           **error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *id_text = axis_download_int_text(id);
	AxisDbConn *conn;
	gboolean ok;

	*info = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_download_store_records(conn, "d.id = $1::bigint", id_text, "d.id", 1, &rows, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (rows->len == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND, "there is no such download");
		return FALSE;
	}
	*info = g_ptr_array_steal_index(rows, 0);
	return TRUE;
}

gboolean
axis_downloads_list(
	AxisDownloads     *self,
	AxisDownloadList   which,
	guint              limit,
	GPtrArray        **downloads,
	GError           **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	switch (which) {
	case AXIS_DOWNLOAD_LIST_ACTIVE:
		ok = axis_download_store_records(conn, "d.state IN ('sending', 'queued', 'downloading', 'paused')", NULL,
			"d.created_at, d.id", limit, downloads, error);
		break;
	case AXIS_DOWNLOAD_LIST_ARRIVED:
		ok = axis_download_store_records(conn, "d.state IN ('completed', 'imported')", NULL,
			"coalesce(d.completed_at, d.updated_at) DESC, d.id DESC", limit, downloads, error);
		break;
	default:
		ok = axis_download_store_records(conn, "d.state NOT IN ('sending', 'queued', 'downloading', 'paused')",
			NULL, "d.updated_at DESC, d.id DESC", limit, downloads, error);
		break;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_downloads_get_many(
	AxisDownloads  *self,
	GArray         *ids,
	GPtrArray     **downloads,
	GError        **error
){
	GString *list = g_string_new("{");
	g_autoptr(GPtrArray) rows = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint i;
	guint j;

	for (i = 0; i < ids->len; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(list, '}');
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		g_string_free(list, TRUE);
		return FALSE;
	}
	ok = axis_download_store_records(conn, "d.id = ANY($1::bigint[])", list->str, "d.id", ids->len + 1, &rows,
		error);
	axis_db_pool_release(self->pool, conn);
	g_string_free(list, TRUE);
	if (!ok)
		return FALSE;
	*downloads = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_info_free);
	for (i = 0; i < ids->len; i++) {
		for (j = 0; j < rows->len; j++) {
			AxisDownloadInfo *d = g_ptr_array_index(rows, j);

			if (d != NULL && d->id == g_array_index(ids, gint64, i)) {
				g_ptr_array_add(*downloads, d);
				rows->pdata[j] = NULL;
				break;
			}
		}
	}
	return TRUE;
}

/* ── Changing clients ───────────────────────────────────────────── */

static gboolean
invalid(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID, message);
	return FALSE;
}

/* A secret goes into a header or a body: any text, but no control
 * character and not too long. */
static gboolean
secret_usable(const gchar *secret)
{
	const gchar *p;

	if (strlen(secret) > 1024 || !g_utf8_validate(secret, -1, NULL))
		return FALSE;
	for (p = secret; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* A user name goes into a login body: any text of at most 128 characters
 * (empty is "none"), no control character. */
static gboolean
username_usable(const gchar *name)
{
	const gchar *p;

	if (!g_utf8_validate(name, -1, NULL) || g_utf8_strlen(name, -1) > 128)
		return FALSE;
	for (p = name; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* @error, with @secret blanked out of its message. */
static void
scrub_error(
	GError      *error,
	const gchar *secret
){
	if (error == NULL || secret == NULL || secret[0] == '\0' || strstr(error->message, secret) == NULL)
		return;
	{
		gchar **parts = g_strsplit(error->message, secret, -1);
		gchar *joined = g_strjoinv("***", parts);

		g_strfreev(parts);
		g_free(error->message);
		error->message = joined;
	}
}

/* Whether @name is the label of a built-in kind, whatever its case: the
 * name of the client Axis makes for that kind. */
static gboolean
name_is_builtin(
	AxisDownloads *self,
	const gchar   *name
){
	g_autofree gchar *folded = g_utf8_casefold(name, -1);
	guint i;

	for (i = 0; i < self->kinds->len; i++) {
		const AxisDownloadKindInfo *kind = g_ptr_array_index(self->kinds, i);
		g_autofree gchar *label = kind->builtin ? g_utf8_casefold(kind->label, -1) : NULL;

		if (label != NULL && strcmp(label, folded) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean
axis_downloads_save_client(
	AxisDownloads                 *self,
	gint64                         id,
	const AxisDownloadClientSpec  *spec,
	gint64                        *id_out,
	GError                       **error
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *address = NULL;
	g_autofree gchar *category = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *base_path = NULL;
	g_autofree gchar *username = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	AxisHttpOrigin origin;
	GError *local = NULL;
	const gchar *base_problem = NULL;
	const gchar *secret = spec->secret != NULL && spec->secret[0] != '\0' ? spec->secret : NULL;
	const gchar *params[9];
	AxisDbConn *conn;
	gboolean ok;

	name = axis_download_text_clean(spec->name, 64);
	if (name == NULL || spec->name == NULL || g_utf8_strlen(spec->name, -1) > 64)
		return invalid(error, "A client's name is 1 to 64 characters.");
	if (id == 0 && (axis_downloads_find_kind(self, spec->kind) == NULL ||
	    axis_downloads_find_kind(self, spec->kind)->builtin))
		return invalid(error, "Choose one of the kinds of download client this version of Axis has.");
	/* A built-in kind's client is made by Axis under its kind's label, when
	 * it is first needed: a client of that name would keep it from being
	 * made (names are unique), so the name is not one to give. */
	if (name_is_builtin(self, name)) {
		g_autofree gchar *text = g_strdup_printf("The name %s is the one Axis gives its own built-in download client. "
			"Choose another name.", name);

		return invalid(error, text);
	}
	address = g_strstrip(g_strdup(spec->address != NULL ? spec->address : ""));
	if (!axis_http_origin_parse(address, &origin, &problem)) {
		g_autofree gchar *text = g_strdup_printf("The address %s. It is the client's own address, such as "
			"http://sabnzbd:8080, with nothing after the port (a path goes in the base path).", problem != NULL ? problem : "is not valid");

		return invalid(error, text);
	}
	if (!axis_download_base_path_normalize(spec->base_path, &base_path, &base_problem)) {
		g_autofree gchar *text = g_strdup_printf("The base path %s. It is the path the client is served under, such "
			"as /sabnzbd: a slash, then names of letters, digits, '.', '_', '~' and '-' separated by slashes, with "
			"no '..', no query and no spaces. Leave it empty when the client is at the top of its address.",
			base_problem != NULL ? base_problem : "is not valid");

		axis_http_origin_clear(&origin);
		return invalid(error, text);
	}
	category = g_strdup(spec->category != NULL && spec->category[0] != '\0' ? spec->category :
		AXIS_DOWNLOAD_DEFAULT_CATEGORY);
	g_strstrip(category);
	if (!axis_download_category_valid(category)) {
		axis_http_origin_clear(&origin);
		return invalid(error, "A category is 1 to 64 letters, digits, '.', '_' and '-', starting with a letter "
			"or a digit.");
	}
	if (axis_download_priority_to_string(spec->priority) == NULL) {
		axis_http_origin_clear(&origin);
		return invalid(error, "Choose a priority.");
	}
	if (secret != NULL && !secret_usable(secret)) {
		axis_http_origin_clear(&origin);
		return invalid(error, "The secret has a character it cannot be sent with, or is longer than 1024 bytes.");
	}
	/* NULL keeps the stored user name, "" removes it. */
	if (spec->username != NULL) {
		username = g_strstrip(g_strdup(spec->username));
		if (!username_usable(username)) {
			axis_http_origin_clear(&origin);
			return invalid(error, "The user name is at most 128 characters, with no control character.");
		}
	}

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_http_origin_clear(&origin);
		return FALSE;
	}
	params[0] = name;
	params[1] = spec->kind;
	params[2] = origin.base;
	params[3] = spec->accept_untrusted ? "true" : "false";
	params[4] = category;
	params[5] = axis_download_priority_to_string(spec->priority);
	params[6] = spec->enabled ? "true" : "false";
	params[7] = base_path;
	params[8] = username;
	ok = axis_db_conn_begin(conn, &local);
	if (ok && id == 0) {
		g_autoptr(AxisDbResult) counted = NULL;

		ok = axis_db_conn_exec(conn, "SELECT count(*) FROM download_client WHERE address <> ''", 0, NULL, &counted,
			&local);
		if (ok && int_at(counted, 0, 0, 0) >= AXIS_DOWNLOAD_MAX_CLIENTS) {
			ok = FALSE;
			g_set_error(&local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
				"At most %d download clients can be configured.", AXIS_DOWNLOAD_MAX_CLIENTS);
		}
		ok = ok && axis_db_conn_exec(conn,
			"INSERT INTO download_client (name, kind, address, accept_untrusted, category, priority, enabled,"
			" base_path, username)"
			" VALUES ($1::text, $2::text, $3::text, $4::boolean, $5::text, $6::text, $7::boolean, $8::text,"
			" nullif($9::text, ''))"
			" RETURNING id", 9, params, &result, &local);
		if (ok)
			id = int_at(result, 0, 0, 0);
	} else if (ok) {
		id_text = axis_download_int_text(id);
		params[1] = id_text;
		ok = axis_db_conn_exec(conn,
			"UPDATE download_client SET name = $1::text, address = $3::text, accept_untrusted = $4::boolean,"
			" category = $5::text, priority = $6::text, enabled = $7::boolean, updated_at = now(),"
			" state = CASE WHEN address <> $3::text OR base_path <> $8::text THEN 'unchecked' ELSE state END,"
			/* Somewhere else: not checked there.  Switched on again: it has
			 * not been failing all the while it was off, so the time begins
			 * again, for the page "Needs attention". */
			" not_working_since = CASE WHEN address <> $3::text OR base_path <> $8::text THEN NULL"
			"  WHEN $7::boolean AND NOT enabled AND not_working_since IS NOT NULL THEN now()"
			"  ELSE not_working_since END,"
			" base_path = $8::text,"
			" username = CASE WHEN $9::text IS NULL THEN username ELSE nullif($9::text, '') END"
			/* Never a built-in kind's client (it has no address). */
			" WHERE id = $2::bigint AND address <> ''", 9, params, &result, &local);
		if (ok && axis_db_result_get_affected_rows(result) == 0) {
			ok = FALSE;
			g_set_error_literal(&local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND,
				"there is no such download client");
		}
	}
	if (ok && secret != NULL) {
		g_autofree gchar *client_text = axis_download_int_text(id);
		const gchar *secret_params[2];

		secret_params[0] = client_text;
		secret_params[1] = secret;
		ok = axis_db_conn_exec(conn,
			"INSERT INTO download_client_secret (client_id, secret) VALUES ($1::bigint, $2::text)"
			" ON CONFLICT (client_id) DO UPDATE SET secret = excluded.secret, set_at = now()", 2, secret_params, NULL,
			&local);
		if (ok)
			ok = axis_db_conn_exec(conn, "UPDATE download_client SET updated_at = now() WHERE id = $1::bigint", 1,
				secret_params, NULL, &local);
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	axis_http_origin_clear(&origin);
	if (!ok) {
		scrub_error(local, secret);
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS,
				"There is a download client of that name already.");
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
		}
		return FALSE;
	}
	axis_download_forget_client(self, id);
	if (id_out != NULL)
		*id_out = id;
	return TRUE;
}

gboolean
axis_downloads_delete_client(
	AxisDownloads  *self,
	gint64          id,
	GError        **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_download_int_text(id);
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok;
	gint64 active = 0;

	params[0] = id_text;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT id FROM download_client WHERE id = $1::bigint AND address <> '' FOR UPDATE", 1,
			params, &result, error);
	if (ok && axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_NOT_FOUND, "there is no such download client");
		ok = FALSE;
	}
	if (ok) {
		g_clear_pointer(&result, axis_db_result_free);
		ok = axis_db_conn_exec(conn,
			"SELECT count(*) FROM download WHERE client_id = $1::bigint"
			" AND state IN ('sending', 'queued', 'downloading', 'paused')", 1, params, &result, error);
		active = ok ? int_at(result, 0, 0, 0) : 0;
		if (ok && active > 0) {
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_BUSY,
				"The client has downloads in progress; remove them or let them finish first.");
			ok = FALSE;
		}
	}
	ok = ok && axis_db_conn_exec(conn, "DELETE FROM download_client WHERE id = $1::bigint", 1, params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		axis_download_forget_client(self, id);
	return ok;
}

gboolean
axis_downloads_add_path_map(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_prefix,
	gint64          volume_id,
	const gchar    *rel_path,
	GError        **error
){
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *client_text = axis_download_int_text(client_id);
	g_autofree gchar *volume_text = axis_download_int_text(volume_id);
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *problem = NULL;
	const gchar *params[4];
	AxisDbConn *conn;
	GError *local = NULL;
	gboolean ok;

	if (!axis_download_path_map_normalize(client_prefix, rel_path, &prefix, &rel, &problem)) {
		g_autofree gchar *text = g_strdup_printf("The mapping cannot be used: %s.", problem);

		return invalid(error, text);
	}
	volume = axis_storage_get_volume(self->storage, volume_id);
	if (volume == NULL)
		return invalid(error, "Choose one of the volumes.");
	params[0] = client_text;
	params[1] = prefix;
	params[2] = volume_text;
	params[3] = rel;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_exec(conn, "SELECT count(*) FROM download_path_map WHERE client_id = $1::bigint", 1, params,
		&result, &local);
	if (ok && int_at(result, 0, 0, 0) >= AXIS_DOWNLOAD_MAX_PATH_MAPS) {
		g_set_error(&local, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID,
			"A client has at most %d path mappings.", AXIS_DOWNLOAD_MAX_PATH_MAPS);
		ok = FALSE;
	}
	ok = ok && axis_db_conn_exec(conn,
		"INSERT INTO download_path_map (client_id, client_prefix, volume_id, rel_path)"
		" SELECT $1::bigint, $2::text, $3::bigint, $4::text WHERE EXISTS (SELECT 1 FROM download_client"
		" WHERE id = $1::bigint)", 4, params, NULL, &local);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_EXISTS,
				"That path of the client is mapped already.");
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
		}
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_downloads_remove_path_map(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_prefix,
	GError        **error
){
	g_autofree gchar *client_text = axis_download_int_text(client_id);
	const gchar *params[2];

	params[0] = client_text;
	params[1] = client_prefix;
	if (client_prefix == NULL)
		return invalid(error, "There is no such mapping.");
	return axis_db_pool_exec(self->pool,
		"DELETE FROM download_path_map WHERE client_id = $1::bigint AND client_prefix = $2::text", 2, params, NULL,
		error);
}
