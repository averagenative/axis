/*
 * axis-worker-tokens.c - the workers' tokens, by their hashes only
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "api/axis-worker-tokens.h"

#include <string.h>

#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"

typedef struct
{
	gint64    id;
	gchar    *name;
	gchar    *hash;
	gboolean  revoked;
} Entry;

struct _AxisWorkerTokens
{
	GMutex     lock;
	GPtrArray *entries;	/* (lock) of Entry */
};

static void
entry_free(gpointer data)
{
	Entry *entry = data;

	g_free(entry->name);
	g_free(entry->hash);
	g_free(entry);
}

AxisWorkerTokens *
axis_worker_tokens_new(void)
{
	AxisWorkerTokens *self = g_new0(AxisWorkerTokens, 1);

	g_mutex_init(&self->lock);
	self->entries = g_ptr_array_new_with_free_func(entry_free);
	return self;
}

void
axis_worker_tokens_free(AxisWorkerTokens *self)
{
	if (self == NULL)
		return;
	g_ptr_array_unref(self->entries);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_worker_tokens_put(
	AxisWorkerTokens *self,
	gint64            id,
	const gchar      *name,
	const gchar      *token_hash,
	gboolean          revoked
){
	Entry *entry;
	guint i;

	g_return_if_fail(self != NULL && name != NULL && token_hash != NULL);

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
	entry->name = g_strdup(name);
	entry->hash = g_strdup(token_hash);
	entry->revoked = revoked;
	g_ptr_array_add(self->entries, entry);
	g_mutex_unlock(&self->lock);
}

void
axis_worker_tokens_revoke(
	AxisWorkerTokens *self,
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

gboolean
axis_worker_tokens_lookup(
	AxisWorkerTokens  *self,
	const gchar       *token,
	gint64            *id,
	gchar            **name
){
	g_autofree gchar *hash = NULL;
	const Entry *found = NULL;
	guint i;

	*id = 0;
	if (name != NULL)
		*name = NULL;
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
	if (found != NULL && !found->revoked) {
		*id = found->id;
		if (name != NULL)
			*name = g_strdup(found->name);
	}
	g_mutex_unlock(&self->lock);

	return *id != 0;
}

gboolean
axis_worker_tokens_is_active(
	AxisWorkerTokens *self,
	gint64            id
){
	gboolean active = FALSE;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	g_mutex_lock(&self->lock);
	for (i = 0; i < self->entries->len; i++) {
		const Entry *entry = g_ptr_array_index(self->entries, i);

		if (entry->id == id)
			active = !entry->revoked;
	}
	g_mutex_unlock(&self->lock);
	return active;
}

gboolean
axis_worker_tokens_load(
	AxisWorkerTokens  *self,
	AxisDbConn        *conn,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	g_return_val_if_fail(self != NULL && conn != NULL, FALSE);

	if (!axis_db_conn_exec(conn, "SELECT id, name, token_hash, revoked_at IS NOT NULL FROM worker ORDER BY id", 0,
	                       NULL, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;
		gboolean revoked = TRUE;

		if (!axis_db_result_get_int64(result, i, 0, &id) || !axis_db_result_get_boolean(result, i, 3, &revoked))
			continue;
		axis_worker_tokens_put(self, id, axis_db_result_get_text(result, i, 1), axis_db_result_get_text(result, i, 2),
			revoked);
	}
	return TRUE;
}
