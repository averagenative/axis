/*
 * axis-user-store.c - accounts in the app_user table
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-user-store.h"

#include <string.h>

G_DEFINE_QUARK(axis-user-error-quark, axis_user_error)

/* The columns every query returning accounts selects, in this order. */
#define USER_COLUMNS \
	"id, username, role, disabled, auth_epoch, " \
	"(extract(epoch FROM created_at) * 1000000)::bigint, theme, add_defaults::text"

const gchar *
axis_user_role_to_string(AxisUserRole role)
{
	return role == AXIS_USER_ROLE_ADMIN ? "admin" : "user";
}

AxisUser *
axis_user_copy(const AxisUser *user)
{
	AxisUser *copy;

	g_return_val_if_fail(user != NULL, NULL);

	copy = g_new0(AxisUser, 1);
	*copy = *user;
	copy->username = g_strdup(user->username);
	copy->theme = g_strdup(user->theme);
	copy->add_defaults = g_strdup(user->add_defaults);

	return copy;
}

void
axis_user_free(AxisUser *user)
{
	if (user == NULL)
		return;

	g_free(user->username);
	g_free(user->theme);
	g_free(user->add_defaults);
	g_free(user);
}

/* The account in @row of @result (USER_COLUMNS first). */
static AxisUser *
user_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisUser *user = g_new0(AxisUser, 1);
	gint64 epoch = 0;

	axis_db_result_get_int64(result, row, 0, &user->id);
	user->username = g_strdup(axis_db_result_get_text(result, row, 1));
	user->role = g_strcmp0(axis_db_result_get_text(result, row, 2), "admin") == 0 ?
		AXIS_USER_ROLE_ADMIN : AXIS_USER_ROLE_USER;
	axis_db_result_get_boolean(result, row, 3, &user->disabled);
	axis_db_result_get_int64(result, row, 4, &epoch);
	user->epoch = (gint)epoch;
	axis_db_result_get_int64(result, row, 5, &user->created_us);
	if (!axis_db_result_is_null(result, row, 6))
		user->theme = g_strdup(axis_db_result_get_text(result, row, 6));
	if (!axis_db_result_is_null(result, row, 7))
		user->add_defaults = g_strdup(axis_db_result_get_text(result, row, 7));

	return user;
}

GPtrArray *
axis_user_store_list(
	AxisDbPool  *pool,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GPtrArray *users;
	guint i;

	if (!axis_db_pool_exec(pool,
	                       "SELECT " USER_COLUMNS " FROM app_user ORDER BY username",
	                       0, NULL, &result, error))
		return NULL;

	users = g_ptr_array_new_with_free_func((GDestroyNotify)axis_user_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(users, user_from_row(result, i));

	return users;
}

static gboolean
find_by(
	AxisDbPool   *pool,
	const gchar  *sql,
	const gchar  *value,
	AxisUser    **user,
	gchar       **password_hash,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	*user = NULL;
	if (password_hash != NULL)
		*password_hash = NULL;

	params[0] = value;
	if (!axis_db_pool_exec(pool, sql, 1, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;

	*user = user_from_row(result, 0);
	if (password_hash != NULL)
		*password_hash = g_strdup(axis_db_result_get_text(result, 0, 8));

	return TRUE;
}

gboolean
axis_user_store_find(
	AxisDbPool   *pool,
	const gchar  *username,
	AxisUser    **user,
	gchar       **password_hash,
	GError      **error
){
	g_return_val_if_fail(username != NULL, FALSE);
	g_return_val_if_fail(user != NULL, FALSE);

	return find_by(pool,
		"SELECT " USER_COLUMNS ", password_hash FROM app_user WHERE username = $1",
		username, user, password_hash, error);
}

gboolean
axis_user_store_get(
	AxisDbPool   *pool,
	gint64        id,
	AxisUser    **user,
	gchar       **password_hash,
	GError      **error
){
	gchar id_text[32];

	g_return_val_if_fail(user != NULL, FALSE);

	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	return find_by(pool,
		"SELECT " USER_COLUMNS ", password_hash FROM app_user WHERE id = $1",
		id_text, user, password_hash, error);
}

AxisUser *
axis_user_store_create_first_admin(
	AxisDbPool   *pool,
	const gchar  *username,
	const gchar  *password_hash,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[2];

	g_return_val_if_fail(username != NULL, NULL);
	g_return_val_if_fail(password_hash != NULL, NULL);

	params[0] = username;
	params[1] = password_hash;
	if (!axis_db_pool_exec(pool,
	                       "INSERT INTO app_user (username, password_hash, role, setup_admin) "
	                       "SELECT $1, $2, 'admin', true "
	                       "WHERE NOT EXISTS (SELECT 1 FROM app_user) "
	                       "RETURNING " USER_COLUMNS,
	                       2, params, &result, &local)) {
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_SETUP_CLOSED,
				"an account already exists");
			return NULL;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}

	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_SETUP_CLOSED,
			"an account already exists");
		return NULL;
	}

	return user_from_row(result, 0);
}

AxisUser *
axis_user_store_create(
	AxisDbPool   *pool,
	const gchar  *username,
	const gchar  *password_hash,
	AxisUserRole  role,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[3];

	g_return_val_if_fail(username != NULL, NULL);
	g_return_val_if_fail(password_hash != NULL, NULL);

	params[0] = username;
	params[1] = password_hash;
	params[2] = axis_user_role_to_string(role);
	if (!axis_db_pool_exec(pool,
	                       "INSERT INTO app_user (username, password_hash, role) "
	                       "VALUES ($1, $2, $3) RETURNING " USER_COLUMNS,
	                       3, params, &result, &local)) {
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_EXISTS,
				"an account with that name exists");
			return NULL;
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}

	return user_from_row(result, 0);
}

/*
 * Runs @body in a transaction on one connection, committing if it
 * succeeds and rolling back otherwise.
 */
typedef gboolean (*TxnBody)(AxisDbConn *conn, gpointer data, GError **error);

static gboolean
in_transaction(
	AxisDbPool  *pool,
	TxnBody      body,
	gpointer     data,
	GError     **error
){
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;

	ok = axis_db_conn_begin(conn, error) &&
	     body(conn, data, error) &&
	     axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);

	axis_db_pool_release(pool, conn);
	return ok;
}

typedef struct
{
	const gchar *id;
	const gchar *hash;
	const gchar *keep;
	gint         epoch;
} SetPassword;

static gboolean
set_password_body(
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	SetPassword *op = data;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];
	gint64 epoch = 0;
	gchar epoch_text[32];

	params[0] = op->id;
	params[1] = op->hash;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE app_user SET password_hash = $2, auth_epoch = auth_epoch + 1, "
	                       "updated_at = now() WHERE id = $1 RETURNING auth_epoch",
	                       2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
			"the account no longer exists");
		return FALSE;
	}
	axis_db_result_get_int64(result, 0, 0, &epoch);
	op->epoch = (gint)epoch;

	params[1] = op->keep;
	if (!axis_db_conn_exec(conn,
	                       "DELETE FROM web_session WHERE user_id = $1 "
	                       "AND token_hash IS DISTINCT FROM $2",
	                       2, params, NULL, error))
		return FALSE;

	if (op->keep != NULL) {
		g_snprintf(epoch_text, sizeof(epoch_text), "%d", op->epoch);
		params[1] = op->keep;
		params[2] = epoch_text;
		if (!axis_db_conn_exec(conn,
		                       "UPDATE web_session SET user_epoch = $3 "
		                       "WHERE user_id = $1 AND token_hash = $2",
		                       3, params, NULL, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
axis_user_store_set_password(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *password_hash,
	const gchar  *keep_token_hash,
	gint         *epoch,
	GError      **error
){
	gchar id_text[32];
	SetPassword op;

	g_return_val_if_fail(password_hash != NULL, FALSE);

	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	memset(&op, 0, sizeof(op));
	op.id = id_text;
	op.hash = password_hash;
	op.keep = keep_token_hash;

	if (!in_transaction(pool, set_password_body, &op, error))
		return FALSE;

	if (epoch != NULL)
		*epoch = op.epoch;
	return TRUE;
}

gboolean
axis_user_store_replace_hash(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *old_hash,
	const gchar  *new_hash,
	GError      **error
){
	gchar id_text[32];
	const gchar *params[3];

	g_return_val_if_fail(old_hash != NULL, FALSE);
	g_return_val_if_fail(new_hash != NULL, FALSE);

	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	params[0] = id_text;
	params[1] = old_hash;
	params[2] = new_hash;

	return axis_db_pool_exec(pool,
		"UPDATE app_user SET password_hash = $3, updated_at = now() "
		"WHERE id = $1 AND password_hash = $2",
		3, params, NULL, error);
}

typedef enum
{
	CHANGE_DISABLE,
	CHANGE_ENABLE,
	CHANGE_MAKE_ADMIN,
	CHANGE_MAKE_USER,
	CHANGE_DELETE
} Change;

typedef struct
{
	const gchar *id;
	Change       change;
	AxisUser    *result;	/* out */
} UserChange;

/*
 * Whether the change takes away an enabled administrator: then another
 * one must remain.  Called with the advisory lock held, so no other such
 * change can run between this check and the update.
 */
static gboolean
check_not_last_admin(
	AxisDbConn  *conn,
	UserChange  *op,
	GError     **error
){
	g_autoptr(AxisDbResult) target = NULL;
	g_autoptr(AxisDbResult) others = NULL;
	const gchar *params[1];
	gboolean disabled = FALSE;
	gint64 count = 0;

	params[0] = op->id;
	if (!axis_db_conn_exec(conn, "SELECT role, disabled FROM app_user WHERE id = $1",
	                       1, params, &target, error))
		return FALSE;
	if (axis_db_result_get_n_rows(target) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
			"the account no longer exists");
		return FALSE;
	}

	axis_db_result_get_boolean(target, 0, 1, &disabled);
	if (g_strcmp0(axis_db_result_get_text(target, 0, 0), "admin") != 0 || disabled)
		return TRUE;	/* not an enabled administrator: nothing is lost */
	if (op->change == CHANGE_ENABLE || op->change == CHANGE_MAKE_ADMIN)
		return TRUE;

	if (!axis_db_conn_exec(conn,
	                       "SELECT count(*) FROM app_user "
	                       "WHERE role = 'admin' AND NOT disabled AND id <> $1",
	                       1, params, &others, error))
		return FALSE;
	axis_db_result_get_int64(others, 0, 0, &count);
	if (count == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_LAST_ADMIN,
			"it is the only enabled administrator");
		return FALSE;
	}

	return TRUE;
}

static gboolean
user_change_body(
	AxisDbConn  *conn,
	gpointer     data,
	GError     **error
){
	UserChange *op = data;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	const gchar *sql = NULL;

	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_xact_lock(" AXIS_USER_LOCK_KEY ")",
	                       0, NULL, NULL, error))
		return FALSE;
	if (!check_not_last_admin(conn, op, error))
		return FALSE;

	params[0] = op->id;
	switch (op->change) {
	case CHANGE_DISABLE:
		sql = "UPDATE app_user SET disabled = true, auth_epoch = auth_epoch + 1, "
		      "updated_at = now() WHERE id = $1 RETURNING " USER_COLUMNS;
		break;
	case CHANGE_ENABLE:
		sql = "UPDATE app_user SET disabled = false, updated_at = now() "
		      "WHERE id = $1 RETURNING " USER_COLUMNS;
		break;
	case CHANGE_MAKE_ADMIN:
		sql = "UPDATE app_user SET role = 'admin', updated_at = now() "
		      "WHERE id = $1 RETURNING " USER_COLUMNS;
		break;
	case CHANGE_MAKE_USER:
		sql = "UPDATE app_user SET role = 'user', updated_at = now() "
		      "WHERE id = $1 RETURNING " USER_COLUMNS;
		break;
	case CHANGE_DELETE:
		sql = "DELETE FROM app_user WHERE id = $1 RETURNING " USER_COLUMNS;
		break;
	}

	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
			"the account no longer exists");
		return FALSE;
	}
	op->result = user_from_row(result, 0);

	if (op->change == CHANGE_DISABLE &&
	    !axis_db_conn_exec(conn, "DELETE FROM web_session WHERE user_id = $1",
	                       1, params, NULL, error))
		return FALSE;

	return TRUE;
}

static AxisUser *
user_change(
	AxisDbPool   *pool,
	gint64        id,
	Change        change,
	GError      **error
){
	gchar id_text[32];
	UserChange op;

	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	memset(&op, 0, sizeof(op));
	op.id = id_text;
	op.change = change;

	if (!in_transaction(pool, user_change_body, &op, error)) {
		axis_user_free(op.result);
		return NULL;
	}

	return op.result;
}

AxisUser *
axis_user_store_set_disabled(
	AxisDbPool   *pool,
	gint64        id,
	gboolean      disabled,
	GError      **error
){
	return user_change(pool, id, disabled ? CHANGE_DISABLE : CHANGE_ENABLE, error);
}

AxisUser *
axis_user_store_set_role(
	AxisDbPool   *pool,
	gint64        id,
	AxisUserRole  role,
	GError      **error
){
	return user_change(pool, id, role == AXIS_USER_ROLE_ADMIN ? CHANGE_MAKE_ADMIN : CHANGE_MAKE_USER,
		error);
}

gboolean
axis_user_store_set_theme(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *theme,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	gchar id_text[32];
	const gchar *params[2];

	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	params[0] = id_text;
	params[1] = theme;
	if (!axis_db_pool_exec(pool,
	                       "UPDATE app_user SET theme = $2, updated_at = now() "
	                       "WHERE id = $1 RETURNING id",
	                       2, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
			"the account no longer exists");
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_user_store_set_add_defaults(
	AxisDbPool   *pool,
	gint64        id,
	const gchar  *kind,
	const gchar  *values,
	gchar       **all,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	gchar id_text[32];
	const gchar *params[3];

	g_return_val_if_fail(kind != NULL && values != NULL, FALSE);

	*all = NULL;
	g_snprintf(id_text, sizeof(id_text), "%" G_GINT64_FORMAT, id);
	params[0] = id_text;
	params[1] = kind;
	params[2] = values;
	if (!axis_db_pool_exec(pool,
	                       "UPDATE app_user SET add_defaults = jsonb_set(add_defaults, ARRAY[$2], $3::jsonb), "
	                       "updated_at = now() WHERE id = $1 RETURNING add_defaults::text",
	                       3, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_USER_ERROR, AXIS_USER_ERROR_NOT_FOUND,
			"the account no longer exists");
		return FALSE;
	}
	*all = g_strdup(axis_db_result_get_text(result, 0, 0));

	return TRUE;
}

gboolean
axis_user_store_delete(
	AxisDbPool   *pool,
	gint64        id,
	GError      **error
){
	AxisUser *deleted = user_change(pool, id, CHANGE_DELETE, error);

	if (deleted == NULL)
		return FALSE;

	axis_user_free(deleted);
	return TRUE;
}
