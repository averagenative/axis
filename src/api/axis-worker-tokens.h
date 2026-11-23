/*
 * axis-worker-tokens.h - the workers' tokens, as the daemon knows them: by
 * their hashes only
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A worker's token is 32 random bytes (AXIS_TOKEN_LEN characters of
 * URL-safe base64, web/axis-password.h), shown to the administrator who
 * created the worker once and never again.  What is kept, here and in the
 * worker table, is its SHA-256.  The auth middleware asks this store on
 * every request to the worker API (web/axis-web-auth.h):
 *
 *  - the presented token is hashed, and the hash is compared with every
 *    known worker's in time that does not depend on where, or whether, it
 *    matches (each comparison is axis_secret_equal(), and all are made);
 *  - a revoked worker's token matches nothing from the moment
 *    axis_worker_tokens_revoke() returns, which the Workers page calls
 *    before it writes the revocation to the database, so the very next
 *    request with it is refused.
 *
 * Loaded once from the database at start-up; changed afterwards only by
 * the daemon itself.  A worker changed in the database from outside is not
 * seen until a restart (as accounts are not).  Thread-safe.
 */

#ifndef AXIS_WORKER_TOKENS_H
#define AXIS_WORKER_TOKENS_H

#include <glib.h>

#include "db/axis-db-conn.h"

G_BEGIN_DECLS

typedef struct _AxisWorkerTokens AxisWorkerTokens;

AxisWorkerTokens *
axis_worker_tokens_new(void);

void
axis_worker_tokens_free(AxisWorkerTokens *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWorkerTokens, axis_worker_tokens_free)

/* Every worker of the worker table, revoked ones included. */
gboolean
axis_worker_tokens_load(
	AxisWorkerTokens  *self,
	AxisDbConn        *conn,
	GError           **error
);

/* Adds a worker (or replaces the one of that id). */
void
axis_worker_tokens_put(
	AxisWorkerTokens *self,
	gint64            id,
	const gchar      *name,
	const gchar      *token_hash,
	gboolean          revoked
);

/* From now on the worker's token matches nothing. */
void
axis_worker_tokens_revoke(
	AxisWorkerTokens *self,
	gint64            id
);

/**
 * axis_worker_tokens_lookup:
 * @token: what was presented (any text)
 * @id: (out): the worker, 0 for none
 * @name: (out) (transfer full) (optional): its name
 *
 * Returns: %TRUE if @token is a well-formed token of a worker that is not
 *   revoked
 */
gboolean
axis_worker_tokens_lookup(
	AxisWorkerTokens  *self,
	const gchar       *token,
	gint64            *id,
	gchar            **name
);

/* Whether the worker @id is known and not revoked. */
gboolean
axis_worker_tokens_is_active(
	AxisWorkerTokens *self,
	gint64            id
);

G_END_DECLS

#endif /* AXIS_WORKER_TOKENS_H */
