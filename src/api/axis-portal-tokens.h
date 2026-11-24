/*
 * axis-portal-tokens.h - the request portal's tokens, as the daemon knows
 * them: by their hashes only
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2.)  A portal token is 32 random bytes
 * (AXIS_TOKEN_LEN characters of URL-safe base64, web/axis-password.h), shown
 * once to the administrator who made it on the page "Request portal" and
 * never again.  What is kept, here and in the portal_token table, is its
 * SHA-256.  The portal API's gate (api/axis-portal-api.h) asks this store on
 * every request, and nothing else in the daemon does: a portal token opens
 * the portal API and nothing more, and by itself it acts as no one.
 *
 *  - The presented token is hashed, and the hash compared with every known
 *    token's in time that does not depend on where, or whether, it matches.
 *  - A revoked token matches nothing from the moment
 *    axis_portal_tokens_revoke() returns, which the page calls before it
 *    writes the revocation to the database.
 *  - At most %AXIS_PORTAL_TOKENS_MAX_ACTIVE tokens are active at once: enough
 *    to replace one without an interruption, and no more.
 *
 * It is a class of its own beside the workers' tokens
 * (api/axis-worker-tokens.h): a worker's token is not found here, and a
 * portal token is not found there.
 *
 * Loaded once from the database at start-up; changed afterwards only by the
 * daemon itself.  Thread-safe.
 */

#ifndef AXIS_PORTAL_TOKENS_H
#define AXIS_PORTAL_TOKENS_H

#include <glib.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

#define AXIS_PORTAL_TOKENS_MAX_ACTIVE 4

typedef struct _AxisPortalTokens AxisPortalTokens;

AxisPortalTokens *
axis_portal_tokens_new(void);

void
axis_portal_tokens_free(AxisPortalTokens *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalTokens, axis_portal_tokens_free)

/* Every token of the portal_token table, revoked ones included. */
gboolean
axis_portal_tokens_load(
	AxisPortalTokens  *self,
	AxisDbConn        *conn,
	GError           **error
);

/* Adds a token (or replaces the one of that id). */
void
axis_portal_tokens_put(
	AxisPortalTokens *self,
	gint64            id,
	const gchar      *token_hash,
	gboolean          revoked
);

/* From now on the token matches nothing. */
void
axis_portal_tokens_revoke(
	AxisPortalTokens *self,
	gint64            id
);

/* How many tokens are not revoked. */
guint
axis_portal_tokens_count_active(AxisPortalTokens *self);

/**
 * axis_portal_tokens_lookup:
 * @token: what was presented (any text)
 * @id: (out): the token, 0 for none
 *
 * Returns: %TRUE if @token is a well-formed portal token that is not revoked
 */
gboolean
axis_portal_tokens_lookup(
	AxisPortalTokens *self,
	const gchar      *token,
	gint64           *id
);

/**
 * axis_portal_tokens_note_use:
 * @address: the address the request came from
 * @now_us: monotonic time
 *
 * Remembers that the token @id was used now, from @address.
 *
 * Returns: %TRUE when that should be written to the database (at most once a
 *   minute for a token, and when the address changed)
 */
gboolean
axis_portal_tokens_note_use(
	AxisPortalTokens *self,
	gint64            id,
	const gchar      *address,
	gint64            now_us
);

/* ── With the database (the page "Request portal"; worker threads) ── */

#define AXIS_PORTAL_TOKENS_ERROR (axis_portal_tokens_error_quark())

typedef enum
{
	AXIS_PORTAL_TOKENS_ERROR_FULL,		/* as many tokens are active as may be */
	AXIS_PORTAL_TOKENS_ERROR_NOT_FOUND
} AxisPortalTokensError;

GQuark
axis_portal_tokens_error_quark(void);

/**
 * axis_portal_tokens_create:
 * @by: the administrator who makes it
 * @token: (out) (transfer full): the token, to show once; free it with
 *   axis_secret_free().  Only its hash is stored
 * @id: (out): its row
 *
 * Makes a token: stored, then known to @self.  Fails with
 * %AXIS_PORTAL_TOKENS_ERROR_FULL while %AXIS_PORTAL_TOKENS_MAX_ACTIVE are
 * active (counted with the insert, under a lock, so two at once cannot both
 * take the last place).  Blocks.
 */
gboolean
axis_portal_tokens_create(
	AxisPortalTokens  *self,
	AxisDbPool        *pool,
	const gchar       *by,
	gchar            **token,
	gint64            *id,
	GError           **error
);

/**
 * axis_portal_tokens_revoke_stored:
 * @changed: (out): it was active until now
 *
 * Revokes the token @id: in @self first, so that the very next request with
 * it is refused, then in the database.  Blocks.
 */
gboolean
axis_portal_tokens_revoke_stored(
	AxisPortalTokens  *self,
	AxisDbPool        *pool,
	gint64             id,
	const gchar       *by,
	gboolean          *changed,
	GError           **error
);

G_END_DECLS

#endif /* AXIS_PORTAL_TOKENS_H */
