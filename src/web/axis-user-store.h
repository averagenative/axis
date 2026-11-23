/*
 * axis-user-store.h - accounts in the app_user table
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every function blocks on the database: call them from worker threads
 * only.  Each acquires one pooled connection and releases it before it
 * returns, on the calling thread, and never holds two.
 *
 * The rule that there is always an enabled administrator is kept here,
 * inside the transaction that would break it: disabling, deleting or
 * demoting takes a transaction-scoped advisory lock first, so two such
 * changes cannot both see "another administrator is left" and both go
 * ahead.
 */

#ifndef AXIS_USER_STORE_H
#define AXIS_USER_STORE_H

#include <glib.h>

#include "db/axis-db-pool.h"

G_BEGIN_DECLS

#define AXIS_USER_ERROR (axis_user_error_quark())

/**
 * AxisUserError:
 * @AXIS_USER_ERROR_NOT_FOUND: there is no such account
 * @AXIS_USER_ERROR_EXISTS: an account with that name exists
 * @AXIS_USER_ERROR_LAST_ADMIN: the change would leave no enabled
 *   administrator
 * @AXIS_USER_ERROR_SETUP_CLOSED: an account exists, so first-run setup
 *   cannot create one
 */
typedef enum
{
	AXIS_USER_ERROR_NOT_FOUND,
	AXIS_USER_ERROR_EXISTS,
	AXIS_USER_ERROR_LAST_ADMIN,
	AXIS_USER_ERROR_SETUP_CLOSED
} AxisUserError;

GQuark
axis_user_error_quark(void);

/* The transaction advisory lock for changes that could remove the last
 * administrator (0x617869735f757372: its bytes spell "axis_usr"). */
#define AXIS_USER_LOCK_KEY "7023479563127976818"

typedef enum
{
	AXIS_USER_ROLE_USER,
	AXIS_USER_ROLE_ADMIN
} AxisUserRole;

/* "admin" or "user", as stored. */
const gchar *
axis_user_role_to_string(AxisUserRole role);

/**
 * AxisUser:
 * @id: the row id
 * @username: the stored (lower-case) name
 * @role: the role
 * @disabled: whether the account may log in
 * @epoch: the auth_epoch; a session is valid only under the current one
 * @created_us: when the account was created, microseconds since 1970
 * @theme: (nullable): the id of the theme the account chose, %NULL for
 *   none (the default theme is used); the id may name a theme this
 *   version no longer has
 *
 * An account as the rest of the daemon sees it: never the password hash.
 */
typedef struct
{
	gint64        id;
	gchar        *username;
	AxisUserRole  role;
	gboolean      disabled;
	gint          epoch;
	gint64        created_us;
	gchar        *theme;
	gchar        *add_defaults;	/* a JSON object: where Adds from a list go, per kind */
} AxisUser;

AxisUser *
axis_user_copy(const AxisUser *user);

void
axis_user_free(AxisUser *user);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisUser, axis_user_free)

/**
 * axis_user_store_list:
 *
 * Returns: (transfer full) (element-type AxisUser) (nullable): every
 *   account, by name; %NULL on error
 */
GPtrArray *
axis_user_store_list(
	AxisDbPool  *pool,
	GError     **error
);

/**
 * axis_user_store_find:
 * @pool: the pool
 * @username: a normalised username
 * @user: (out) (transfer full) (nullable): the account, %NULL if none
 * @password_hash: (out) (transfer full) (nullable) (optional): its
 *   password hash
 * @error: (nullable): return location for an error
 *
 * Returns: %TRUE unless the database failed (no account is not a failure)
 */
gboolean
axis_user_store_find(
	AxisDbPool   *pool,
	const gchar  *username,
	AxisUser    **user,
	gchar       **password_hash,
	GError      **error
);

/* As axis_user_store_find(), by id. */
gboolean
axis_user_store_get(
	AxisDbPool   *pool,
	gint64        id,
	AxisUser    **user,
	gchar       **password_hash,
	GError      **error
);

/**
 * axis_user_store_create_first_admin:
 *
 * Creates an administrator with one statement that only inserts while
 * the table is empty, marked as the setup account (at most one row may
 * be).  Fails with %AXIS_USER_ERROR_SETUP_CLOSED when any account exists
 * or another setup won a race.
 *
 * Returns: (transfer full) (nullable): the account
 */
AxisUser *
axis_user_store_create_first_admin(
	AxisDbPool   *pool,
	const gchar  *username,
	const gchar  *password_hash,
	GError      **error
);

/**
 * axis_user_store_create:
 *
 * Fails with %AXIS_USER_ERROR_EXISTS when the name is taken.
 *
 * Returns: (transfer full) (nullable): the account
 */
AxisUser *
axis_user_store_create(
	AxisDbPool   *pool,
	const gchar  *username,
	const gchar  *password_hash,
	AxisUserRole  role,
	GError      **error
);

/**
 * axis_user_store_set_password:
 * @pool: the pool
 * @id: the account
 * @password_hash: the new hash
 * @keep_token_hash: (nullable): the one session of the account to keep
 * @epoch: (out): the account's new epoch
 * @error: (nullable): return location for an error
 *
 * In one transaction: sets the hash, moves the account to a new epoch,
 * deletes every session of the account but @keep_token_hash, and moves
 * that one to the new epoch.
 *
 * Returns: %TRUE on success; %AXIS_USER_ERROR_NOT_FOUND if @id is gone
 */
gboolean
axis_user_store_set_password(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *password_hash,
	const gchar  *keep_token_hash,
	gint         *epoch,
	GError      **error
);

/**
 * axis_user_store_replace_hash:
 *
 * Replaces @old_hash with @new_hash (the same password under new limits)
 * if it is still the account's hash; otherwise does nothing.  Sessions
 * are not touched.
 *
 * Returns: %TRUE unless the database failed
 */
gboolean
axis_user_store_replace_hash(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *old_hash,
	const gchar  *new_hash,
	GError      **error
);

/**
 * axis_user_store_set_disabled:
 *
 * Disabling moves the account to a new epoch and deletes its sessions, in
 * the same transaction.  Disabling the only enabled administrator fails
 * with %AXIS_USER_ERROR_LAST_ADMIN.
 *
 * Returns: (transfer full) (nullable): the account as it now is
 */
AxisUser *
axis_user_store_set_disabled(
	AxisDbPool   *pool,
	gint64        id,
	gboolean      disabled,
	GError      **error
);

/**
 * axis_user_store_set_role:
 *
 * Demoting the only enabled administrator fails with
 * %AXIS_USER_ERROR_LAST_ADMIN.
 *
 * Returns: (transfer full) (nullable): the account as it now is
 */
AxisUser *
axis_user_store_set_role(
	AxisDbPool   *pool,
	gint64        id,
	AxisUserRole  role,
	GError      **error
);

/**
 * axis_user_store_set_theme:
 * @pool: the pool
 * @id: the account
 * @theme: (nullable): the theme id, or %NULL to use the default
 * @error: (nullable): return location for an error
 *
 * Stores the choice.  Sessions and the epoch are not touched.  The id is
 * not checked against the built-in themes here (the caller does).
 *
 * Returns: %TRUE on success; %AXIS_USER_ERROR_NOT_FOUND if @id is gone
 */
gboolean
axis_user_store_set_theme(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *theme,
	GError      **error
);

/**
 * axis_user_store_set_add_defaults:
 * @pool: the pool
 * @id: the account
 * @kind: the media type ("movies", "series")
 * @values: a JSON object: what that type's Adds from a list use
 * @all: (out) (transfer full): every type's, as stored now
 * @error: (nullable): return location for an error
 *
 * Stores the person's choice for @kind (beside the theme: their own, no one
 * else's).  The ids are not checked here (the module does).
 *
 * Returns: %TRUE on success; %AXIS_USER_ERROR_NOT_FOUND if @id is gone
 */
gboolean
axis_user_store_set_add_defaults(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *kind,
	const gchar  *values,
	gchar       **all,
	GError      **error
);

/**
 * axis_user_store_delete:
 *
 * Deletes the account and (by cascade) its sessions.  Deleting the only
 * enabled administrator fails with %AXIS_USER_ERROR_LAST_ADMIN.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_user_store_delete(
	AxisDbPool   *pool,
	gint64        id,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_USER_STORE_H */
