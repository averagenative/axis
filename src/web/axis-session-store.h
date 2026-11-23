/*
 * axis-session-store.h - the in-memory accounts and sessions of the web
 * interface, written through to PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Answering "who is this request from" must not touch the database, so
 * every account (without its password hash) and every live session is
 * held here, loaded when the store is created.  Changes are made here
 * first, so they take effect at once, and are then written to the
 * database by one writer thread, in the order they were made: a session
 * created and revoked a moment later is inserted and then deleted, never
 * the other way round.  The writer retries a write that failed because
 * the database was unreachable; one the database refused is dropped with
 * a warning.  last_seen_at is written in batches by
 * axis_session_store_flush().
 *
 * A session is valid while it is less than %AXIS_SESSION_IDLE_US past its
 * last use and %AXIS_SESSION_LIFETIME_US past its creation, its account
 * exists and is enabled, and the account's epoch is the one the session
 * was made under.
 *
 * The cookie's token is never kept: a session is known by the SHA-256 of
 * its token, which is all the database holds too.
 *
 * Everything except creation and stop is for the main loop's thread only
 * (the writer thread touches only its own queue).  Creation loads from the
 * database and so blocks; call it before the main loop runs.  Without a
 * pool (tests) nothing is loaded or written.
 */

#ifndef AXIS_SESSION_STORE_H
#define AXIS_SESSION_STORE_H

#include <glib.h>

#include "db/axis-db-pool.h"
#include "web/axis-user-store.h"

G_BEGIN_DECLS

/* The default idle limit; the setting "web.session_idle" changes it at run
 * time (see axis_session_store_set_idle_seconds()). */
#define AXIS_SESSION_IDLE_US      (G_GINT64_CONSTANT(30) * 24 * 3600 * G_USEC_PER_SEC)
#define AXIS_SESSION_LIFETIME_US  (G_GINT64_CONSTANT(90) * 24 * 3600 * G_USEC_PER_SEC)

/**
 * axis_session_store_set_idle_seconds:
 * @seconds: how long a session may go unused before it is no longer valid,
 *   1 hour to 90 days
 *
 * Sets the idle limit for every store of the process (it is a policy, not
 * a property of one store).  Thread-safe.  A session already idle for
 * longer than the new limit is ended the next time it is looked up or
 * swept.  Out-of-range values are ignored with a critical.
 */
void
axis_session_store_set_idle_seconds(guint seconds);

/* The idle limit in effect, in seconds. */
guint
axis_session_store_get_idle_seconds(void);

/* last_seen is moved on at most this often per session. */
#define AXIS_SESSION_TOUCH_US     (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

/* An account keeps at most this many sessions; a new one beyond that
 * ends the least recently used. */
#define AXIS_SESSION_MAX_PER_USER 64

typedef struct
{
	gchar  *token_hash;	/* SHA-256 of the token, hex */
	gchar  *public_id;	/* names the session on the account page */
	gint64  user_id;
	gint    epoch;
	gint64  created_us;	/* microseconds since 1970 */
	gint64  last_seen_us;
	gchar  *client_address;
	gchar  *user_agent;
} AxisSession;

typedef struct _AxisSessionStore AxisSessionStore;

/**
 * axis_session_store_new:
 * @pool: (nullable): the database; %NULL keeps everything in memory
 * @now_us: the current time, microseconds since 1970
 * @error: (nullable): return location for an error
 *
 * Loads every account and every valid session, and starts the writer,
 * which first deletes the sessions that are no longer valid.
 *
 * Returns: (transfer full) (nullable): the store, %NULL if loading failed
 */
AxisSessionStore *
axis_session_store_new(
	AxisDbPool  *pool,
	gint64       now_us,
	GError     **error
);

/**
 * axis_session_store_stop:
 *
 * Writes the pending last-use times, lets the writer finish everything
 * queued (giving up on writes that keep failing after a few tries) and
 * joins it.  Blocks.  Nothing is written after this.
 */
void
axis_session_store_stop(AxisSessionStore *self);

/* Stops the store if that has not been done, and frees it. */
void
axis_session_store_free(AxisSessionStore *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSessionStore, axis_session_store_free)

/* ── Accounts ─────────────────────────────────────────────────── */

/* Returns: (transfer none) (nullable): the account with @id */
const AxisUser *
axis_session_store_get_user(
	AxisSessionStore *self,
	gint64            id
);

/* Returns: (transfer none) (nullable): the account called @username
 * (normalised) */
const AxisUser *
axis_session_store_find_user(
	AxisSessionStore *self,
	const gchar      *username
);

guint
axis_session_store_count_users(AxisSessionStore *self);

/* Returns: (transfer container) (element-type AxisUser): every account,
 * by name */
GPtrArray *
axis_session_store_list_users(AxisSessionStore *self);

/**
 * axis_session_store_put_user:
 *
 * Adds @user, or replaces the account with its id, after the database
 * has changed it.  Sessions that are no longer valid under the new
 * record (disabled, or another epoch) are dropped from memory at once;
 * the database change already deleted their rows.
 */
void
axis_session_store_put_user(
	AxisSessionStore *self,
	const AxisUser   *user
);

/* Forgets the account with @id and its sessions (it was deleted). */
void
axis_session_store_remove_user(
	AxisSessionStore *self,
	gint64            id
);

/* ── Sessions ─────────────────────────────────────────────────── */

/**
 * axis_session_store_lookup:
 * @self: the store
 * @token: (nullable): the token from a cookie
 * @now_us: the current time
 * @user: (out) (transfer none) (optional): the session's account
 *
 * Finds the valid session for @token and marks it used.  An expired
 * session found on the way is ended.
 *
 * Returns: (transfer none) (nullable): the session; valid until the next
 *   call that changes the store
 */
const AxisSession *
axis_session_store_lookup(
	AxisSessionStore  *self,
	const gchar       *token,
	gint64             now_us,
	const AxisUser   **user
);

/**
 * axis_session_store_create:
 * @self: the store
 * @user_id: an account the store holds
 * @epoch: the epoch the login read for the account
 * @client_address: (nullable): shown on the account page
 * @user_agent: (nullable): likewise (cleaned and shortened here)
 * @now_us: the current time
 * @token: (out) (transfer full): the new session's token, for the
 *   cookie; free it with axis_secret_free()
 *
 * Makes a new session with a fresh random token.  If @epoch is not the
 * account's current one (it changed while the login was being checked),
 * the session is still made but is never valid.
 *
 * Returns: (transfer none): the session
 */
const AxisSession *
axis_session_store_create(
	AxisSessionStore  *self,
	gint64             user_id,
	gint               epoch,
	const gchar       *client_address,
	const gchar       *user_agent,
	gint64             now_us,
	gchar            **token
);

/* Ends the session whose token hashes to @token_hash.  Returns: %TRUE if
 * there was one. */
gboolean
axis_session_store_revoke(
	AxisSessionStore *self,
	const gchar      *token_hash
);

/* Ends @user_id's session called @public_id.  Returns: %TRUE if there was
 * one. */
gboolean
axis_session_store_revoke_public(
	AxisSessionStore *self,
	gint64            user_id,
	const gchar      *public_id
);

/**
 * axis_session_store_set_epoch:
 *
 * Moves the session @token_hash to @epoch (the one session a password
 * change keeps).  Call before axis_session_store_put_user() with the
 * account's new epoch, or it is dropped.
 */
void
axis_session_store_set_epoch(
	AxisSessionStore *self,
	const gchar      *token_hash,
	gint              epoch
);

/* Returns: (transfer container) (element-type AxisSession): @user_id's
 * valid sessions, most recently used first */
GPtrArray *
axis_session_store_list_for_user(
	AxisSessionStore *self,
	gint64            user_id,
	gint64            now_us
);

/**
 * AxisSessionEndFunc:
 * @token_hash: the session that ended
 * @user_data: (closure): as given to axis_session_store_set_end_func()
 *
 * Called on the main loop's thread whenever a session stops existing:
 * logout, revocation, an account that was disabled, deleted or had its
 * password changed, expiry, or the cap on a user's sessions.  It must not
 * call back into the store.
 */
typedef void (*AxisSessionEndFunc)(
	const gchar *token_hash,
	gpointer     user_data
);

/**
 * axis_session_store_set_end_func:
 * @func: (nullable): the one handler; %NULL removes it
 *
 * The web hub uses this to close a session's event streams.  Clear it
 * before @user_data goes away.
 */
void
axis_session_store_set_end_func(
	AxisSessionStore   *self,
	AxisSessionEndFunc  func,
	gpointer            user_data
);

/**
 * AxisSessionRoleFunc:
 * @user_id: the account whose role changed
 * @user_data: (closure): as given to axis_session_store_set_role_func()
 *
 * Called on the main loop's thread when axis_session_store_put_user() gives
 * an account another role.  Its sessions stay (a request reads the role
 * afresh each time); what was opened under the old role must not outlive
 * it.  It must not call back into the store.
 */
typedef void (*AxisSessionRoleFunc)(
	gint64   user_id,
	gpointer user_data
);

/* The web hub uses this to close an account's event streams, which then
 * reconnect under the new role.  Clear it before @user_data goes away. */
void
axis_session_store_set_role_func(
	AxisSessionStore    *self,
	AxisSessionRoleFunc  func,
	gpointer             user_data
);

/* How many sessions are held (valid or not yet swept). */
guint
axis_session_store_count(AxisSessionStore *self);

/* Queues the last-use times that changed since the previous flush. */
void
axis_session_store_flush(AxisSessionStore *self);

/* Drops expired sessions from memory and queues their deletion. */
void
axis_session_store_sweep(
	AxisSessionStore *self,
	gint64            now_us
);

/**
 * axis_session_store_wait_idle:
 *
 * For tests: blocks until the writer has finished everything queued so
 * far.  Returns at once without a pool.
 */
void
axis_session_store_wait_idle(AxisSessionStore *self);

G_END_DECLS

#endif /* AXIS_SESSION_STORE_H */
