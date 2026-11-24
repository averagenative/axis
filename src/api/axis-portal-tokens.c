/*
 * axis-portal-tokens.c - the request portal's tokens, by their hashes only
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-portal-tokens.h"

#include <string.h>

#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"

/* A token's use is written down at most this often. */
#define NOTE_EVERY_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

typedef struct
{
	gint64    id;
	gchar    *hash;
	gboolean  revoked;
	gint64    noted_us;	/* when its use was last written down (monotonic); 0: never */
	gchar    *address;	/* where it was last used from */
} Entry;

struct _AxisPortalTokens
{
	GMutex     lock;
	GPtrArray *entries;	/* (lock) of Entry */
};

static void
entry_free(gpointer data)
{
	Entry *entry = data;

	g_free(entry->hash);
	g_free(entry->address);
	g_free(entry);
}

AxisPortalTokens *
axis_portal_tokens_new(void)
{
	AxisPortalTokens *self = g_new0(AxisPortalTokens, 1);

	g_mutex_init(&self->lock);
	self->entries = g_ptr_array_new_with_free_func(entry_free);
	return self;
}

void
axis_portal_tokens_free(AxisPortalTokens *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->entries);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_portal_tokens_put(
	AxisPortalTokens *self,
	gint64            id,
	const gchar      *token_hash,
	gboolean          revoked
){
	Entry *entry;
	guint i;

	g_return_if_fail(self != NULL && token_hash != NULL);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->entries->len; i++) {
		entry = g_ptr_array_index(self->entries, i);
		if (entry->id == id) {
			g_ptr_array_remove_index(self->entries, i);
			break;
		}
	}
	entry = g_new0(Entry, 1);
	entry->id = id;
	entry->hash = g_strdup(token_hash);
	entry->revoked = revoked;
	g_ptr_array_add(self->entries, entry);
	g_mutex_unlock(&self->lock);
}

void
axis_portal_tokens_revoke(
	AxisPortalTokens *self,
	gint64            id
){
	guint i;

	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->entries->len; i++) {
		Entry *entry = g_ptr_array_index(self->entries, i);

		if (entry->id == id)
			entry->revoked = TRUE;
	}
	g_mutex_unlock(&self->lock);
}

guint
axis_portal_tokens_count_active(AxisPortalTokens *self)
{
	guint n = 0;
	guint i;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->entries->len; i++) {
		if (!((const Entry *)g_ptr_array_index(self->entries, i))->revoked)
			n++;
	}
	g_mutex_unlock(&self->lock);
	return n;
}

gboolean
axis_portal_tokens_lookup(
	AxisPortalTokens *self,
	const gchar      *token,
	gint64           *id
){
	g_autofree gchar *hash = NULL;
	const Entry *found = NULL;
	guint i;

	*id = 0;
	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_auth_token_is_wellformed(token, AXIS_TOKEN_LEN))
		return FALSE;
	hash = axis_token_hash(token);

	g_mutex_lock(&self->lock);
	/* Every entry is compared, in constant time each, so that the time
	 * taken says nothing of which (if any) matched. */
	for (i = 0; i < self->entries->len; i++) {
		const Entry *entry = g_ptr_array_index(self->entries, i);

		if (axis_secret_equal(hash, entry->hash) && found == NULL)
			found = entry;
	}
	if (found != NULL && !found->revoked)
		*id = found->id;
	g_mutex_unlock(&self->lock);

	return *id != 0;
}

gboolean
axis_portal_tokens_note_use(
	AxisPortalTokens *self,
	gint64            id,
	const gchar      *address,
	gint64            now_us
){
	gboolean write = FALSE;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->entries->len; i++) {
		Entry *entry = g_ptr_array_index(self->entries, i);

		if (entry->id != id)
			continue;
		if (entry->noted_us == 0 || now_us - entry->noted_us >= NOTE_EVERY_US ||
		    g_strcmp0(entry->address, address) != 0) {
			entry->noted_us = now_us != 0 ? now_us : 1;
			g_free(entry->address);
			entry->address = g_strdup(address);
			write = TRUE;
		}
	}
	g_mutex_unlock(&self->lock);
	return write;
}

G_DEFINE_QUARK(axis-portal-tokens-error-quark, axis_portal_tokens_error)

gboolean
axis_portal_tokens_create(
	AxisPortalTokens  *self,
	AxisDbPool        *pool,
	const gchar       *by,
	gchar            **token,
	gint64            *id,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *hash = NULL;
	gchar *made = axis_random_token();
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && pool != NULL && by != NULL && token != NULL && id != NULL, FALSE);

	*token = NULL;
	*id = 0;
	hash = axis_token_hash(made);
	params[0] = hash;
	params[1] = by;
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL) {
		axis_secret_free(made);
		return FALSE;
	}
	ok = axis_db_conn_begin(conn, error) &&
	     axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(hashtext('axis.portal.token'), 0)", 0, NULL, NULL,
	                       error) &&
	     axis_db_conn_exec(conn, "INSERT INTO portal_token (token_hash, created_by) SELECT $1, $2"
	                       " WHERE (SELECT count(*) FROM portal_token WHERE revoked_at IS NULL) < "
	                       G_STRINGIFY(AXIS_PORTAL_TOKENS_MAX_ACTIVE) " RETURNING id", 2, params, &result, error);
	if (ok && axis_db_result_get_n_rows(result) == 0) {
		g_set_error(error, AXIS_PORTAL_TOKENS_ERROR, AXIS_PORTAL_TOKENS_ERROR_FULL,
			"%d portal tokens are active already; revoke one first", AXIS_PORTAL_TOKENS_MAX_ACTIVE);
		ok = FALSE;
	}
	if (ok)
		axis_db_result_get_int64(result, 0, 0, id);
	ok = ok && axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(pool, conn);
	if (!ok) {
		axis_secret_free(made);
		*id = 0;
		return FALSE;
	}
	axis_portal_tokens_put(self, *id, hash, FALSE);
	*token = made;
	return TRUE;
}

gboolean
axis_portal_tokens_revoke_stored(
	AxisPortalTokens  *self,
	AxisDbPool        *pool,
	gint64             id,
	const gchar       *by,
	gboolean          *changed,
	GError           **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) exists = NULL;
	const gchar *params[] = { id_text, by };

	g_return_val_if_fail(self != NULL && pool != NULL && by != NULL && changed != NULL, FALSE);

	*changed = FALSE;
	/* In memory first: refused from the very next request. */
	axis_portal_tokens_revoke(self, id);
	if (!axis_db_pool_exec(pool, "UPDATE portal_token SET revoked_at = now(), revoked_by = $2 WHERE id = $1::bigint"
	        " AND revoked_at IS NULL RETURNING id", 2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0) {
		*changed = TRUE;
		return TRUE;
	}
	if (!axis_db_pool_exec(pool, "SELECT 1 FROM portal_token WHERE id = $1::bigint", 1, params, &exists, error))
		return FALSE;
	if (axis_db_result_get_n_rows(exists) == 0) {
		g_set_error_literal(error, AXIS_PORTAL_TOKENS_ERROR, AXIS_PORTAL_TOKENS_ERROR_NOT_FOUND, "there is no such token");
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_portal_tokens_load(
	AxisPortalTokens  *self,
	AxisDbConn        *conn,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	g_return_val_if_fail(self != NULL && conn != NULL, FALSE);

	if (!axis_db_conn_exec(conn, "SELECT id, token_hash, revoked_at IS NOT NULL FROM portal_token ORDER BY id", 0, NULL,
	                       &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;
		gboolean revoked = TRUE;

		if (!axis_db_result_get_int64(result, i, 0, &id) || !axis_db_result_get_boolean(result, i, 2, &revoked))
			continue;
		axis_portal_tokens_put(self, id, axis_db_result_get_text(result, i, 1), revoked);
	}
	return TRUE;
}
