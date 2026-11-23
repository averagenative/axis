/*
 * axis-quality-store.c - quality profiles in the database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "quality/axis-quality-store.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "quality/axis-quality-private.h"

G_DEFINE_QUARK(axis-quality-error-quark, axis_quality_error)

typedef struct
{
	gchar *one;
	gchar *many;
	gchar *sql;
} User;

struct _AxisQualityStore
{
	gint          refs;	/* atomic */
	AxisDbPool   *pool;
	AxisSettings *settings;	/* owned reference, nullable */
	GPtrArray    *users;	/* User; fixed once the store is used */
};

static void
user_free(User *user)
{
	g_free(user->one);
	g_free(user->many);
	g_free(user->sql);
	g_free(user);
}

AxisQualityStore *
axis_quality_store_new(
	AxisDbPool   *pool,
	AxisSettings *settings
){
	AxisQualityStore *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisQualityStore, 1);
	self->refs = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->users = g_ptr_array_new_with_free_func((GDestroyNotify)user_free);
	return self;
}

AxisQualityStore *
axis_quality_store_ref(AxisQualityStore *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->refs);
	return self;
}

void
axis_quality_store_unref(AxisQualityStore *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->refs))
		return;
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_ptr_array_unref(self->users);
	g_free(self);
}

AxisDbPool *
axis_quality_store_get_pool(AxisQualityStore *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->pool;
}

gboolean
axis_quality_store_add_user(
	AxisQualityStore *self,
	const gchar      *one,
	const gchar      *many,
	const gchar      *sql
){
	User *user;

	g_return_val_if_fail(self != NULL && one != NULL && many != NULL, FALSE);

	if (sql == NULL || !g_str_has_prefix(sql, "SELECT ") || strchr(sql, ';') != NULL || strlen(sql) > 2000) {
		g_critical("the quality profile user %s is not one SELECT statement", one);
		return FALSE;
	}
	user = g_new0(User, 1);
	user->one = g_strdup(one);
	user->many = g_strdup(many);
	user->sql = g_strdup(sql);
	g_ptr_array_add(self->users, user);
	return TRUE;
}

/* ── The setting ────────────────────────────────────────────────── */

static gboolean
default_validate(
	const gchar  *text,
	gchar       **reason
){
	if (!axis_quality_slug_valid(text)) {
		*reason = g_strdup("must be a profile's short name as the Quality profiles page shows it, such as balanced or "
			"custom_7");
		return FALSE;
	}
	return TRUE;
}

const AxisSettingInfo *
axis_quality_store_settings(void)
{
	static const AxisSettingInfo settings[] = {
		{ AXIS_QUALITY_SETTING_DEFAULT, AXIS_SETTING_STRING, "Default quality profile",
		  "The quality profile a title gets when it is added, by the short name the Quality profiles page shows for it"
		  "(balanced, space_saver, best_1080p, uhd, anything, or custom_ and a number for a copy). A profile that "
		  "is the default cannot be deleted.", 0, AXIS_QUALITY_DEFAULT_SLUG, 1, 41, 0, 0, NULL },
		{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
	};

	axis_setting_set_validator(AXIS_QUALITY_SETTING_DEFAULT, default_validate);
	return settings;
}

gchar *
axis_quality_store_default_slug(AxisQualityStore *self)
{
	g_autofree gchar *slug = NULL;

	g_return_val_if_fail(self != NULL, g_strdup(AXIS_QUALITY_DEFAULT_SLUG));

	if (self->settings != NULL && axis_settings_lookup(self->settings, AXIS_QUALITY_SETTING_DEFAULT, NULL) != NULL)
		slug = axis_settings_get_string(self->settings, AXIS_QUALITY_SETTING_DEFAULT);
	if (!axis_quality_slug_valid(slug))
		return g_strdup(AXIS_QUALITY_DEFAULT_SLUG);
	return g_steal_pointer(&slug);
}

/* ── Rows ───────────────────────────────────────────────────────── */

#define COLUMNS \
	"p.id, p.slug, p.name, p.built_in, p.format, p.definition, p.revision, c.slug," \
	" (extract(epoch FROM p.created_at) * 1000000)::bigint, (extract(epoch FROM p.updated_at) * 1000000)::bigint," \
	" p.updated_by"
#define FROM " FROM quality_profile p LEFT JOIN quality_profile c ON c.id = p.copied_from"

/* The built-ins first, in their order, then the others by age. */
#define ORDER \
	" ORDER BY p.built_in DESC, array_position(ARRAY['space_saver', 'balanced', 'best_1080p', 'uhd', 'anything']," \
	" p.slug), p.id"

void
axis_quality_stored_free(AxisQualityStored *stored)
{
	if (stored == NULL)
		return;
	g_free(stored->slug);
	g_free(stored->name);
	g_free(stored->text);
	g_free(stored->copied_from);
	g_free(stored->updated_by);
	g_free(stored->unusable);
	g_free(stored);
}

gboolean
axis_quality_stored_check(
	gint                format,
	const gchar        *text,
	AxisQualityProfile *profile,
	gchar             **unusable
){
	g_autoptr(GPtrArray) problems = axis_quality_problems_new();
	gint stated = axis_quality_profile_format_of(text, -1);

	*unusable = NULL;
	if (format > AXIS_QUALITY_FORMAT || stated > AXIS_QUALITY_FORMAT) {
		*unusable = g_strdup_printf("It was written by a newer version of Axis, in format %d; this version reads "
			"format %d. Nothing of it is used.", MAX(format, stated), AXIS_QUALITY_FORMAT);
		memset(profile, 0, sizeof(*profile));
		return FALSE;
	}
	if (format != AXIS_QUALITY_FORMAT || stated != format) {
		*unusable = g_strdup_printf("Its stored definition is not of format %d. Nothing of it is used.",
			AXIS_QUALITY_FORMAT);
		memset(profile, 0, sizeof(*profile));
		return FALSE;
	}
	if (!axis_quality_profile_parse(text, -1, profile, problems)) {
		const AxisQualityProblem *first = problems->len > 0 ? g_ptr_array_index(problems, 0) : NULL;

		/* The first problem says enough; its message is a sentence. */
		if (first == NULL)
			*unusable = g_strdup("Its stored definition is not valid. Nothing of it is used.");
		else if (first->field[0] != '\0')
			*unusable = g_strdup_printf("Its stored definition is not valid (%s): %s Nothing of it is used.",
				first->field, first->message);
		else
			*unusable = g_strdup_printf("Its stored definition is not valid: %s Nothing of it is used.",
				first->message);
		return FALSE;
	}
	return TRUE;
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static AxisQualityStored *
stored_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisQualityStored *s = g_new0(AxisQualityStored, 1);
	const gchar *copied = axis_db_result_get_text(result, row, 7);
	const gchar *by = axis_db_result_get_text(result, row, 10);

	s->id = int_at(result, row, 0);
	s->slug = g_strdup(axis_db_result_get_text(result, row, 1));
	s->name = g_strdup(axis_db_result_get_text(result, row, 2));
	axis_db_result_get_boolean(result, row, 3, &s->built_in);
	s->format = (gint)int_at(result, row, 4);
	s->text = g_strdup(axis_db_result_get_text(result, row, 5));
	s->revision = (gint)int_at(result, row, 6);
	s->copied_from = copied != NULL ? g_strdup(copied) : NULL;
	s->created_us = int_at(result, row, 8);
	s->updated_us = int_at(result, row, 9);
	s->updated_by = by != NULL ? g_strdup(by) : NULL;
	s->usable = axis_quality_stored_check(s->format, s->text, &s->profile, &s->unusable);
	return s;
}

static gboolean
read_one(
	AxisDbConn         *conn,
	const gchar        *where,
	const gchar        *param,
	gboolean            lock,
	AxisQualityStored **stored,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = g_strdup_printf("SELECT " COLUMNS FROM " WHERE %s%s", where, lock ? " FOR UPDATE OF p" : "");
	const gchar *params[1];

	*stored = NULL;
	params[0] = param;
	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*stored = stored_from_row(result, 0);
	return TRUE;
}

gboolean
axis_quality_store_read(
	AxisDbConn         *conn,
	const gchar        *slug,
	AxisQualityStored **stored,
	GError            **error
){
	g_return_val_if_fail(conn != NULL && stored != NULL, FALSE);

	if (!axis_quality_slug_valid(slug)) {
		*stored = NULL;
		return TRUE;
	}
	return read_one(conn, "p.slug = $1", slug, FALSE, stored, error);
}

gboolean
axis_quality_store_read_id(
	AxisDbConn         *conn,
	gint64              id,
	AxisQualityStored **stored,
	GError            **error
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);

	g_return_val_if_fail(conn != NULL && stored != NULL, FALSE);

	return read_one(conn, "p.id = $1::bigint", text, FALSE, stored, error);
}

gboolean
axis_quality_store_load(
	AxisDbConn         *conn,
	gint64              id,
	AxisQualityProfile *profile,
	GError            **error
){
	g_autoptr(AxisQualityStored) stored = NULL;

	g_return_val_if_fail(profile != NULL, FALSE);

	memset(profile, 0, sizeof(*profile));
	if (!axis_quality_store_read_id(conn, id, &stored, error))
		return FALSE;
	if (stored == NULL) {
		g_set_error_literal(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND, "There is no such quality "
			"profile.");
		return FALSE;
	}
	if (!stored->usable) {
		g_set_error(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_UNUSABLE, "The quality profile %s is not usable: "
			"%s", stored->slug, stored->unusable);
		return FALSE;
	}
	*profile = stored->profile;
	return TRUE;
}

/* ── Through the pool ───────────────────────────────────────────── */

gboolean
axis_quality_store_list(
	AxisQualityStore  *self,
	GPtrArray        **profiles,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && profiles != NULL, FALSE);

	*profiles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_quality_stored_free);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_exec(conn, "SELECT " COLUMNS FROM ORDER, 0, NULL, &result, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*profiles, stored_from_row(result, i));
	return TRUE;
}

gboolean
axis_quality_store_get(
	AxisQualityStore   *self,
	const gchar        *slug,
	AxisQualityStored **stored,
	GError            **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && stored != NULL, FALSE);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		*stored = NULL;
		return FALSE;
	}
	ok = axis_quality_store_read(conn, slug, stored, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* What uses profile @id (named @slug), in words, on @conn. */
static gboolean
uses_of(
	AxisQualityStore  *self,
	AxisDbConn        *conn,
	gint64             id,
	const gchar       *slug,
	GPtrArray        **uses,
	GError           **error
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autofree gchar *default_slug = axis_quality_store_default_slug(self);
	const gchar *params[1];
	guint i;

	*uses = g_ptr_array_new_with_free_func(g_free);
	params[0] = text;
	for (i = 0; i < self->users->len; i++) {
		const User *user = g_ptr_array_index(self->users, i);
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM (%s) AS used (profile_id)"
			" WHERE used.profile_id = $1::bigint", user->sql);
		gint64 count = 0;

		if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
			return FALSE;
		count = int_at(result, 0, 0);
		if (count > 0)
			g_ptr_array_add(*uses, g_strdup_printf("%" G_GINT64_FORMAT " %s", count, count == 1 ? user->one :
				user->many));
	}
	if (g_strcmp0(default_slug, slug) == 0)
		g_ptr_array_add(*uses, g_strdup("the default for new titles (a setting)"));
	return TRUE;
}

gboolean
axis_quality_store_uses(
	AxisQualityStore  *self,
	const gchar       *slug,
	GPtrArray        **uses,
	GError           **error
){
	g_autoptr(AxisQualityStored) stored = NULL;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && uses != NULL, FALSE);

	*uses = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_quality_store_read(conn, slug, &stored, error);
	if (ok && stored == NULL) {
		g_set_error_literal(error, AXIS_QUALITY_ERROR, AXIS_QUALITY_ERROR_NOT_FOUND, "There is no such quality "
			"profile.");
		ok = FALSE;
	}
	if (ok)
		ok = uses_of(self, conn, stored->id, stored->slug, uses, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(uses, g_ptr_array_unref);
	return ok;
}

/* ── Writing ────────────────────────────────────────────────────── */

static gboolean
fail(
	GError           **error,
	AxisQualityError   code,
	const gchar       *message
){
	g_set_error_literal(error, AXIS_QUALITY_ERROR, code, message);
	return FALSE;
}

/* Ends the transaction: commits when @ok, else rolls back. */
static gboolean
finish(
	AxisDbConn  *conn,
	gboolean     ok,
	GError     **error
){
	if (ok)
		return axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

/* A transaction on a connection of the pool: begun, or NULL. */
static AxisDbConn *
begin(
	AxisDbPool  *pool,
	GError     **error
){
	AxisDbConn *conn = axis_db_pool_acquire(pool, error);

	if (conn == NULL)
		return NULL;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return NULL;
	}
	return conn;
}

/* The profile @slug, locked; an error when there is none. */
static gboolean
lock_profile(
	AxisDbConn         *conn,
	const gchar        *slug,
	AxisQualityStored **stored,
	GError            **error
){
	if (!axis_quality_slug_valid(slug))
		return fail(error, AXIS_QUALITY_ERROR_NOT_FOUND, "There is no such quality profile.");
	if (!read_one(conn, "p.slug = $1", slug, TRUE, stored, error))
		return FALSE;
	if (*stored == NULL)
		return fail(error, AXIS_QUALITY_ERROR_NOT_FOUND, "There is no such quality profile.");
	return TRUE;
}

gboolean
axis_quality_store_sync_builtins(
	AxisDbPool  *pool,
	guint       *changed,
	GError     **error
){
	const AxisQualityBuiltin *builtins = axis_quality_builtins();
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint written = 0;
	guint i;

	g_return_val_if_fail(pool != NULL, FALSE);

	conn = begin(pool, error);
	if (conn == NULL)
		return FALSE;
	for (i = 0; ok && i < AXIS_QUALITY_N_BUILTINS; i++) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *format = g_strdup_printf("%d", AXIS_QUALITY_FORMAT);
		AxisQualityProfile profile;
		const gchar *params[4];

		if (!axis_quality_builtin_profile(builtins[i].slug, &profile)) {
			ok = fail(error, AXIS_QUALITY_ERROR_INVALID, "A built-in quality profile of the code does not validate.");
			break;
		}
		text = axis_quality_profile_canonical(&profile);
		params[0] = builtins[i].slug;
		params[1] = builtins[i].name;
		params[2] = format;
		params[3] = text;
		ok = axis_db_conn_exec(conn,
			"INSERT INTO quality_profile (slug, name, built_in, format, definition)"
			" VALUES ($1, $2, true, $3::integer, $4)"
			" ON CONFLICT (slug) DO UPDATE SET name = EXCLUDED.name, format = EXCLUDED.format,"
			" definition = EXCLUDED.definition, revision = quality_profile.revision + 1, updated_at = now(),"
			" updated_by = NULL"
			" WHERE quality_profile.built_in AND (quality_profile.name <> EXCLUDED.name"
			" OR quality_profile.format <> EXCLUDED.format OR quality_profile.definition <> EXCLUDED.definition)"
			" RETURNING id",
			4, params, &result, error);
		if (ok)
			written += axis_db_result_get_n_rows(result);
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	if (ok && changed != NULL)
		*changed = written;
	if (ok && written > 0)
		g_message("quality profiles: %u built-in profile%s written from the code", written, written == 1 ? "" : "s");
	return ok;
}

gboolean
axis_quality_store_copy(
	AxisQualityStore  *self,
	const gchar       *source,
	const gchar       *name,
	const gchar       *user,
	gchar            **slug,
	GError           **error
){
	g_autoptr(AxisQualityStored) from = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *source_id = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *format = g_strdup_printf("%d", AXIS_QUALITY_FORMAT);
	const gchar *params[5];
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && slug != NULL, FALSE);

	*slug = NULL;
	if (name == NULL || name[0] == '\0' || user == NULL)
		return fail(error, AXIS_QUALITY_ERROR_INVALID, "A profile needs a name.");
	conn = begin(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = lock_profile(conn, source, &from, error);
	if (ok && !from->usable)
		ok = fail(error, AXIS_QUALITY_ERROR_UNUSABLE, "Its definition is not usable, so it cannot be copied.");
	if (ok) {
		source_id = g_strdup_printf("%" G_GINT64_FORMAT, from->id);
		text = axis_quality_profile_canonical(&from->profile);
		params[0] = name;
		params[1] = format;
		params[2] = text;
		params[3] = source_id;
		params[4] = user;
		ok = axis_db_conn_exec(conn,
			"WITH n AS (SELECT nextval(pg_get_serial_sequence('quality_profile', 'id')) AS id)"
			" INSERT INTO quality_profile (id, slug, name, built_in, format, definition, copied_from, updated_by)"
			" SELECT n.id, 'custom_' || n.id, $1, false, $2::integer, $3, $4::bigint, $5 FROM n RETURNING slug",
			5, params, &result, error);
	}
	if (ok)
		*slug = g_strdup(axis_db_result_get_text(result, 0, 0));
	ok = finish(conn, ok, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(slug, g_free);
	return ok;
}

gboolean
axis_quality_store_save(
	AxisQualityStore         *self,
	const gchar              *slug,
	gint                      expected,
	const gchar              *name,
	const AxisQualityProfile *profile,
	const gchar              *user,
	gboolean                 *changed,
	GPtrArray                *problems,
	GError                  **error
){
	g_autoptr(AxisQualityStored) current = NULL;
	g_autofree gchar *text = NULL;
	AxisQualityProfile checked;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL && profile != NULL, FALSE);

	if (changed != NULL)
		*changed = FALSE;
	if (name == NULL || name[0] == '\0' || user == NULL)
		return fail(error, AXIS_QUALITY_ERROR_INVALID, "A profile needs a name.");
	/* The one strict reader, on the text that would be stored. */
	text = axis_quality_profile_canonical(profile);
	if (!axis_quality_profile_parse(text, -1, &checked, problems))
		return fail(error, AXIS_QUALITY_ERROR_INVALID, "The definition is refused.");

	conn = begin(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = lock_profile(conn, slug, &current, error);
	if (ok && current->built_in)
		ok = fail(error, AXIS_QUALITY_ERROR_BUILT_IN, "A built-in profile cannot be changed; copy it instead.");
	if (ok && !current->usable)
		ok = fail(error, AXIS_QUALITY_ERROR_UNUSABLE, "Its stored definition is not usable, so it cannot be edited.");
	if (ok && current->revision != expected)
		ok = fail(error, AXIS_QUALITY_ERROR_CONFLICT, "The profile was saved by someone else since this form was "
			"opened; open it again to see the change.");
	if (ok && (g_strcmp0(current->text, text) != 0 || g_strcmp0(current->name, name) != 0)) {
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, current->id);
		g_autofree gchar *format = g_strdup_printf("%d", AXIS_QUALITY_FORMAT);
		const gchar *params[5];

		params[0] = id;
		params[1] = name;
		params[2] = format;
		params[3] = text;
		params[4] = user;
		ok = axis_db_conn_exec(conn,
			"UPDATE quality_profile SET name = $2, format = $3::integer, definition = $4, revision = revision + 1,"
			" updated_at = now(), updated_by = $5 WHERE id = $1::bigint AND NOT built_in",
			5, params, NULL, error);
		if (ok && changed != NULL)
			*changed = TRUE;
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok && changed != NULL)
		*changed = FALSE;
	return ok;
}

gboolean
axis_quality_store_delete(
	AxisQualityStore  *self,
	const gchar       *slug,
	GPtrArray        **uses,
	GError           **error
){
	g_autoptr(AxisQualityStored) current = NULL;
	g_autoptr(GPtrArray) found = NULL;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (uses != NULL)
		*uses = NULL;
	conn = begin(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = lock_profile(conn, slug, &current, error);
	if (ok && current->built_in)
		ok = fail(error, AXIS_QUALITY_ERROR_BUILT_IN, "A built-in profile cannot be deleted.");
	if (ok)
		ok = uses_of(self, conn, current->id, current->slug, &found, error);
	if (ok && found->len > 0) {
		g_autofree gchar *what = axis_quality_join(found, " and ");
		g_autofree gchar *message = g_strdup_printf("The profile is in use: %s. Change what uses it first.", what);

		ok = fail(error, AXIS_QUALITY_ERROR_IN_USE, message);
		if (uses != NULL)
			*uses = g_steal_pointer(&found);
	}
	if (ok) {
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, current->id);
		const gchar *params[1];

		params[0] = id;
		ok = axis_db_conn_exec(conn, "DELETE FROM quality_profile WHERE id = $1::bigint AND NOT built_in", 1, params,
			NULL, error);
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
