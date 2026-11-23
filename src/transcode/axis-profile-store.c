/*
 * axis-profile-store.c - conversion profiles in the database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Statements are constants; values reach the database as parameters.
 * Every write runs in one transaction that locks the profile's row first,
 * so two administrators saving the same profile at once make two numbered
 * revisions in order, or one fails with a conflict.
 */

#include "transcode/axis-profile-store.h"

#include <string.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"

G_DEFINE_QUARK(axis-profile-error-quark, axis_profile_error)

/* ── Revisions ──────────────────────────────────────────────────── */

AxisProfileRevision *
axis_profile_revision_ref(AxisProfileRevision *revision)
{
	g_return_val_if_fail(revision != NULL, NULL);

	g_atomic_int_inc(&revision->refs);
	return revision;
}

void
axis_profile_revision_unref(AxisProfileRevision *revision)
{
	if (revision == NULL || !g_atomic_int_dec_and_test(&revision->refs))
		return;
	g_free(revision->slug);
	g_free(revision->name);
	g_free(revision->note);
	g_free(revision->text);
	g_free(revision->hash);
	g_free(revision->origin);
	g_free(revision->created_by);
	g_free(revision->unusable);
	g_free(revision->proof_method);
	g_free(revision);
}

gboolean
axis_profile_revision_is_unlocked(const AxisProfileRevision *revision)
{
	return revision != NULL && revision->usable && revision->unlocked;
}

gboolean
axis_profile_revision_target(
	const AxisProfileRevision *revision,
	AxisEstimateTarget        *target
){
	const AxisProfileBuiltin *builtin;
	AxisProfileIdentity identity;

	memset(target, 0, sizeof(*target));
	if (revision == NULL || !revision->usable)
		return FALSE;
	builtin = revision->built_in ? axis_profile_builtin(revision->slug) : NULL;
	memset(&identity, 0, sizeof(identity));
	identity.slug = revision->slug;
	identity.name = revision->name;
	identity.family = builtin != NULL ? builtin->family : NULL;
	identity.summary = builtin != NULL ? builtin->summary : revision->note;
	identity.revision_id = revision->id;
	identity.revision = revision->number;
	return axis_profile_to_target(&revision->def, &identity, target);
}

/* Whether the stored text is a definition this version uses, as it is:
 * valid, in canonical form, with its hash and of its profile's kind. */
static void
verify(AxisProfileRevision *rev)
{
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *hash = NULL;

	rev->usable = FALSE;
	if (!axis_profile_definition_parse(rev->text, -1, &rev->def, problems)) {
		const AxisProfileProblem *p = problems->len > 0 ? g_ptr_array_index(problems, 0) : NULL;

		rev->unusable = g_strdup_printf("Its stored definition does not pass the checks of this Axis release (%s%s%s), so "
			"it is not used for anything: no estimate, no conversion, no copy. It is kept as it is.",
			p != NULL && p->field[0] != '\0' ? p->field : "", p != NULL && p->field[0] != '\0' ? ": " : "",
			p != NULL ? p->message : "refused");
		memset(&rev->def, 0, sizeof(rev->def));
		return;
	}
	canonical = axis_profile_definition_canonical(&rev->def);
	hash = axis_profile_hash(rev->text);
	if (strcmp(canonical, rev->text) != 0) {
		rev->unusable = g_strdup("Its stored definition is not in the form this version writes, so it is not "
			"used for anything. It is kept as it is.");
	} else if (g_strcmp0(hash, rev->hash) != 0) {
		rev->unusable = g_strdup("Its stored definition does not match its stored hash, so it is not used for "
			"anything. It is kept as it is.");
	} else if (rev->def.kind != rev->kind) {
		rev->unusable = g_strdup("Its stored definition is for another kind of file than its profile, so it is "
			"not used for anything. It is kept as it is.");
	} else {
		rev->usable = TRUE;
		return;
	}
	memset(&rev->def, 0, sizeof(rev->def));
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

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(result, row, column) ? NULL : g_strdup(axis_db_result_get_text(result, row, column));
}

AxisProfileRevision *
axis_profile_revision_from_row(
	AxisDbResult *result,
	guint         row,
	guint         c
){
	AxisProfileRevision *rev = g_new0(AxisProfileRevision, 1);

	rev->refs = 1;
	rev->id = int_at(result, row, c);
	rev->profile_id = int_at(result, row, c + 1);
	rev->number = (gint)int_at(result, row, c + 2);
	rev->slug = text_at(result, row, c + 3);
	if (!axis_profile_kind_from_string(axis_db_result_get_text(result, row, c + 4), &rev->kind))
		rev->kind = (AxisProfileKind)-1;
	rev->built_in = bool_at(result, row, c + 5);
	rev->archived = bool_at(result, row, c + 6);
	rev->current = bool_at(result, row, c + 7);
	rev->name = text_at(result, row, c + 8);
	rev->note = text_at(result, row, c + 9);
	rev->text = text_at(result, row, c + 10);
	rev->hash = text_at(result, row, c + 11);
	rev->origin = text_at(result, row, c + 12);
	rev->created_by = text_at(result, row, c + 13);
	rev->created_us = int_at(result, row, c + 14);
	rev->proof_method = text_at(result, row, c + 15);
	rev->proven_us = int_at(result, row, c + 16);
	rev->unlocked = rev->proof_method != NULL;
	if (rev->slug == NULL)
		rev->slug = g_strdup("");
	if (rev->name == NULL)
		rev->name = g_strdup("");
	if (rev->note == NULL)
		rev->note = g_strdup("");
	verify(rev);
	return rev;
}

/* ── Reading ────────────────────────────────────────────────────── */

static gboolean
read_one(
	AxisDbConn           *conn,
	const gchar          *sql,
	guint                 n_params,
	const gchar *const   *params,
	AxisProfileRevision **revision,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*revision = NULL;
	if (!axis_db_conn_exec(conn, sql, n_params, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*revision = axis_profile_revision_from_row(result, 0, 0);
	return TRUE;
}

gboolean
axis_profile_store_current(
	AxisDbConn           *conn,
	const gchar          *slug,
	AxisProfileRevision **revision,
	GError              **error
){
	return axis_profile_store_revision(conn, slug, 0, revision, error);
}

gboolean
axis_profile_store_revision(
	AxisDbConn           *conn,
	const gchar          *slug,
	gint                  number,
	AxisProfileRevision **revision,
	GError              **error
){
	g_autofree gchar *n = g_strdup_printf("%d", number);
	const gchar *params[2];

	*revision = NULL;
	if (!axis_profile_slug_valid(slug) || number < 0)
		return TRUE;
	params[0] = slug;
	params[1] = n;
	return read_one(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
		" WHERE p.slug = $1::text AND (CASE WHEN $2::integer = 0 THEN r.id = p.current_revision_id"
		" ELSE r.number = $2::integer END)", 2, params, revision, error);
}

gboolean
axis_profile_store_revision_by_id(
	AxisDbConn           *conn,
	gint64                id,
	AxisProfileRevision **revision,
	GError              **error
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];

	params[0] = text;
	return read_one(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
		" WHERE r.id = $1::bigint", 1, params, revision, error);
}

gboolean
axis_profile_store_history(
	AxisDbConn   *conn,
	const gchar  *slug,
	GPtrArray   **revisions,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	*revisions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_revision_unref);
	params[0] = slug;
	if (!axis_db_conn_exec(conn, "SELECT " AXIS_PROFILE_REVISION_COLUMNS AXIS_PROFILE_REVISION_FROM
	        " WHERE p.slug = $1::text ORDER BY r.number DESC", 1, params, &result, error)) {
		g_clear_pointer(revisions, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*revisions, axis_profile_revision_from_row(result, i, 0));
	return TRUE;
}

void
axis_profile_assignment_free(AxisProfileAssignment *a)
{
	if (a == NULL)
		return;
	g_free(a->root_name);
	g_free(a->media_type);
	g_free(a->slug);
	g_free(a->verdict);
	g_free(a->assigned_by);
	g_free(a);
}

gboolean
axis_profile_store_assignments(
	AxisDbConn   *conn,
	GPtrArray   **assignments,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	*assignments = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_assignment_free);
	if (!axis_db_conn_exec(conn,
	        "SELECT a.root_id, ro.name, ro.media_type, a.kind, a.profile_id, p.slug, a.revision_id,"
	        " p.current_revision_id, a.verdict, a.confirmed, a.assigned_by,"
	        " (extract(epoch FROM a.assigned_at) * 1000000)::bigint"
	        " FROM conversion_profile_assignment a JOIN conversion_profile p ON p.id = a.profile_id"
	        " JOIN library_root ro ON ro.id = a.root_id ORDER BY ro.name, ro.id, a.kind DESC",
	        0, NULL, &result, error)) {
		g_clear_pointer(assignments, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisProfileAssignment *a = g_new0(AxisProfileAssignment, 1);

		a->root_id = int_at(result, i, 0);
		a->root_name = text_at(result, i, 1);
		a->media_type = text_at(result, i, 2);
		axis_profile_kind_from_string(axis_db_result_get_text(result, i, 3), &a->kind);
		a->profile_id = int_at(result, i, 4);
		a->slug = text_at(result, i, 5);
		a->revision_id = int_at(result, i, 6);
		a->current_revision_id = int_at(result, i, 7);
		a->verdict = text_at(result, i, 8);
		a->confirmed = bool_at(result, i, 9);
		a->assigned_by = text_at(result, i, 10);
		a->assigned_us = int_at(result, i, 11);
		g_ptr_array_add(*assignments, a);
	}
	return TRUE;
}

void
axis_profile_info_free(AxisProfileInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->slug);
	g_free(info->copied_from);
	axis_profile_revision_unref(info->current);
	g_free(info);
}

gboolean
axis_profile_store_list(
	AxisDbConn   *conn,
	gboolean      with_archived,
	GPtrArray   **profiles,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	guint i;

	*profiles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_profile_info_free);
	params[0] = with_archived ? "true" : "false";
	if (!axis_db_conn_exec(conn,
	        "SELECT p.id, p.slug, p.kind, p.built_in, p.archived, c.slug,"
	        " (SELECT count(*) FROM conversion_profile_revision x WHERE x.profile_id = p.id),"
	        " r.id IS NOT NULL, " AXIS_PROFILE_REVISION_COLUMNS
	        " FROM conversion_profile p LEFT JOIN conversion_profile c ON c.id = p.copied_from"
	        " LEFT JOIN conversion_profile_revision r ON r.id = p.current_revision_id"
	        " LEFT JOIN conversion_profile_proof pr ON pr.revision_id = r.id"
	        " WHERE $1::boolean OR NOT p.archived"
	        " ORDER BY p.built_in DESC, p.id",
	        1, params, &result, error)) {
		g_clear_pointer(profiles, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisProfileInfo *info = g_new0(AxisProfileInfo, 1);

		info->id = int_at(result, i, 0);
		info->slug = text_at(result, i, 1);
		axis_profile_kind_from_string(axis_db_result_get_text(result, i, 2), &info->kind);
		info->built_in = bool_at(result, i, 3);
		info->archived = bool_at(result, i, 4);
		info->copied_from = text_at(result, i, 5);
		info->revisions = (guint)int_at(result, i, 6);
		if (bool_at(result, i, 7))
			info->current = axis_profile_revision_from_row(result, i, 8);
		g_ptr_array_add(*profiles, info);
	}
	return TRUE;
}

/* ── Writing ────────────────────────────────────────────────────── */

static gboolean
fail(
	GError      **error,
	gint          code,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_PROFILE_ERROR, code, message);
	return FALSE;
}

/* Inserts revision @number of @profile_id and makes it current; its id in
 * @id. */
static gboolean
add_revision(
	AxisDbConn   *conn,
	gint64        profile_id,
	gint          number,
	const gchar  *name,
	const gchar  *note,
	const gchar  *definition,
	const gchar  *origin,
	const gchar  *user,
	gint64       *id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *profile = g_strdup_printf("%" G_GINT64_FORMAT, profile_id);
	g_autofree gchar *n = g_strdup_printf("%d", number);
	g_autofree gchar *hash = axis_profile_hash(definition);
	g_autofree gchar *revision = NULL;
	const gchar *params[8];

	params[0] = profile;
	params[1] = n;
	params[2] = name;
	params[3] = note != NULL ? note : "";
	params[4] = definition;
	params[5] = hash;
	params[6] = origin;
	params[7] = user;
	if (!axis_db_conn_exec(conn,
	        "INSERT INTO conversion_profile_revision (profile_id, number, name, note, definition, hash, origin,"
	        " created_by) VALUES ($1::bigint, $2::integer, $3::text, $4::text, $5::text, $6::text, $7::text, $8::text)"
	        " RETURNING id", 8, params, &result, error))
		return FALSE;
	*id = int_at(result, 0, 0);
	revision = g_strdup_printf("%" G_GINT64_FORMAT, *id);
	params[0] = profile;
	params[1] = revision;
	return axis_db_conn_exec(conn, "UPDATE conversion_profile SET current_revision_id = $2::bigint"
		" WHERE id = $1::bigint", 2, params, NULL, error);
}

static gboolean
finish(
	AxisDbConn  *conn,
	gboolean     ok,
	GError     **error
){
	if (!ok) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	return axis_db_conn_commit(conn, error);
}

gboolean
axis_profile_store_sync_builtins(
	AxisDbPool  *pool,
	guint       *added,
	GError     **error
){
	const AxisProfileBuiltin *list = axis_profile_builtins();
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	if (added != NULL)
		*added = 0;
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return FALSE;
	}
	for (i = 0; ok && i < AXIS_PROFILE_N_BUILTINS; i++) {
		g_autoptr(GPtrArray) problems = axis_profile_problems_new();
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *canonical = NULL;
		g_autofree gchar *hash = NULL;
		AxisProfileDefinition def;
		const gchar *params[2];
		gint64 profile_id = 0;
		gint64 revision_id = 0;
		gint number = 0;

		/* The code's own definition must pass, in canonical form. */
		if (!axis_profile_definition_parse(list[i].definition, -1, &def, problems)) {
			g_set_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_INVALID,
				"the built-in profile %s does not validate", list[i].slug);
			ok = FALSE;
			break;
		}
		canonical = axis_profile_definition_canonical(&def);
		hash = axis_profile_hash(canonical);
		params[0] = list[i].slug;
		if (!axis_db_conn_exec(conn,
		        "SELECT p.id, p.built_in, p.kind, r.hash, r.name, coalesce((SELECT max(number)"
		        " FROM conversion_profile_revision x WHERE x.profile_id = p.id), 0)"
		        " FROM conversion_profile p LEFT JOIN conversion_profile_revision r ON r.id = p.current_revision_id"
		        " WHERE p.slug = $1::text FOR UPDATE OF p", 1, params, &result, error)) {
			ok = FALSE;
			break;
		}
		if (axis_db_result_get_n_rows(result) == 0) {
			g_autoptr(AxisDbResult) inserted = NULL;

			params[1] = axis_profile_kind_to_string(def.kind);
			if (!axis_db_conn_exec(conn, "INSERT INTO conversion_profile (slug, kind, built_in)"
			        " VALUES ($1::text, $2::text, true) RETURNING id", 2, params, &inserted, error)) {
				ok = FALSE;
				break;
			}
			profile_id = int_at(inserted, 0, 0);
			number = 0;
		} else {
			profile_id = int_at(result, 0, 0);
			if (!bool_at(result, 0, 1) || g_strcmp0(axis_db_result_get_text(result, 0, 2),
			        axis_profile_kind_to_string(def.kind)) != 0) {
				g_set_error(error, AXIS_PROFILE_ERROR, AXIS_PROFILE_ERROR_INVALID,
					"the profile %s in the database is not the built-in of that name", list[i].slug);
				ok = FALSE;
				break;
			}
			if (g_strcmp0(axis_db_result_get_text(result, 0, 3), hash) == 0 &&
			    g_strcmp0(axis_db_result_get_text(result, 0, 4), list[i].name) == 0)
				continue;
			number = (gint)int_at(result, 0, 5);
		}
		if (!add_revision(conn, profile_id, number + 1, list[i].name, "", canonical, "code", NULL, &revision_id,
		        error)) {
			ok = FALSE;
			break;
		}
		g_message("profiles: the built-in profile %s is now revision %d, from this version's code", list[i].slug,
			number + 1);
		if (added != NULL)
			(*added)++;
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

/* The profile @slug's row, locked, with its current revision. */
static gboolean
lock_profile(
	AxisDbConn           *conn,
	const gchar          *slug,
	AxisProfileRevision **current,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	*current = NULL;
	if (!axis_profile_slug_valid(slug))
		return fail(error, AXIS_PROFILE_ERROR_NOT_FOUND, "There is no such profile.");
	params[0] = slug;
	if (!axis_db_conn_exec(conn, "SELECT id FROM conversion_profile WHERE slug = $1::text FOR UPDATE", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return fail(error, AXIS_PROFILE_ERROR_NOT_FOUND, "There is no such profile.");
	if (!axis_profile_store_current(conn, slug, current, error))
		return FALSE;
	if (*current == NULL)
		return fail(error, AXIS_PROFILE_ERROR_UNUSABLE, "The profile has no current version.");
	return TRUE;
}

gboolean
axis_profile_store_copy(
	AxisDbPool   *pool,
	const gchar  *source,
	const gchar  *name,
	const gchar  *user,
	gchar       **slug,
	GError      **error
){
	g_autoptr(AxisProfileRevision) from = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *source_id = NULL;
	const gchar *params[3];
	AxisDbConn *conn;
	gint64 profile_id = 0;
	gint64 revision_id = 0;
	gboolean ok;

	*slug = NULL;
	if (name == NULL || name[0] == '\0' || user == NULL)
		return fail(error, AXIS_PROFILE_ERROR_INVALID, "A profile needs a name.");
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return FALSE;
	}
	ok = lock_profile(conn, source, &from, error);
	if (ok && !from->usable)
		ok = fail(error, AXIS_PROFILE_ERROR_UNUSABLE, "Its definition is not usable, so it cannot be copied.");
	if (ok) {
		source_id = g_strdup_printf("%" G_GINT64_FORMAT, from->profile_id);
		params[0] = axis_profile_kind_to_string(from->kind);
		params[1] = source_id;
		ok = axis_db_conn_exec(conn,
			"WITH n AS (SELECT nextval(pg_get_serial_sequence('conversion_profile', 'id')) AS id)"
			" INSERT INTO conversion_profile (id, slug, kind, built_in, copied_from)"
			" SELECT n.id, 'custom_' || n.id, $1::text, false, $2::bigint FROM n RETURNING id, slug",
			2, params, &result, error);
	}
	if (ok) {
		profile_id = int_at(result, 0, 0);
		*slug = g_strdup(axis_db_result_get_text(result, 0, 1));
		ok = add_revision(conn, profile_id, 1, name, "", from->text, "user", user, &revision_id, error);
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	if (!ok)
		g_clear_pointer(slug, g_free);
	return ok;
}

gboolean
axis_profile_store_save(
	AxisDbPool                  *pool,
	const gchar                 *slug,
	gint                         expected,
	const AxisProfileDefinition *def,
	const gchar                 *name,
	const gchar                 *note,
	const gchar                 *user,
	AxisProfileSaved            *saved,
	GPtrArray                   *problems,
	GError                     **error
){
	g_autoptr(AxisProfileRevision) current = NULL;
	g_autofree gchar *canonical = NULL;
	AxisProfileDefinition checked;
	AxisDbConn *conn;
	gboolean ok;

	memset(saved, 0, sizeof(*saved));
	if (name == NULL || name[0] == '\0' || user == NULL)
		return fail(error, AXIS_PROFILE_ERROR_INVALID, "A profile needs a name.");
	/* The one strict function, on the text that would be stored. */
	canonical = axis_profile_definition_canonical(def);
	if (!axis_profile_definition_parse(canonical, -1, &checked, problems))
		return fail(error, AXIS_PROFILE_ERROR_INVALID, "The definition is refused.");

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return FALSE;
	}
	ok = lock_profile(conn, slug, &current, error);
	if (ok && current->built_in)
		ok = fail(error, AXIS_PROFILE_ERROR_BUILT_IN, "A built-in profile cannot be changed; copy it instead.");
	if (ok && current->archived)
		ok = fail(error, AXIS_PROFILE_ERROR_ARCHIVED, "The profile is archived; select Offer this profile again on its page first.");
	if (ok && !current->usable)
		ok = fail(error, AXIS_PROFILE_ERROR_UNUSABLE, "Its current definition is not usable, so it cannot be "
			"edited; make a copy of another profile instead.");
	if (ok && current->number != expected)
		ok = fail(error, AXIS_PROFILE_ERROR_CONFLICT, "The profile was changed by someone else since this page "
			"was opened.");
	if (ok && checked.kind != current->kind)
		ok = fail(error, AXIS_PROFILE_ERROR_INVALID, "A profile keeps its kind.");
	if (ok) {
		saved->number = current->number;
		saved->revision_id = current->id;
		/* Saving without a change adds nothing. */
		if (strcmp(canonical, current->text) == 0 && g_strcmp0(name, current->name) == 0 &&
		    g_strcmp0(note != NULL ? note : "", current->note) == 0) {
			ok = finish(conn, TRUE, error);
			axis_db_pool_release(pool, conn);
			return ok;
		}
		ok = add_revision(conn, current->profile_id, current->number + 1, name, note, canonical, "user", user,
			&saved->revision_id, error);
	}
	/* The proof goes forward only when nothing that makes the bytes
	 * changed; anything in doubt locks. */
	if (ok && axis_profile_revision_is_unlocked(current) && axis_profile_carries_proof(&current->def, &checked, NULL)) {
		g_autofree gchar *to = g_strdup_printf("%" G_GINT64_FORMAT, saved->revision_id);
		g_autofree gchar *from = g_strdup_printf("%" G_GINT64_FORMAT, current->id);
		const gchar *params[3];

		params[0] = to;
		params[1] = from;
		params[2] = user;
		ok = axis_db_conn_exec(conn, "INSERT INTO conversion_profile_proof (revision_id, method, carried_from,"
			" recorded_by) VALUES ($1::bigint, 'carried', $2::bigint, $3::text)", 3, params, NULL, error);
		saved->carried = ok;
	}
	if (ok) {
		saved->created = TRUE;
		saved->number = current->number + 1;
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	if (!ok)
		memset(saved, 0, sizeof(*saved));
	return ok;
}

gboolean
axis_profile_store_set_archived(
	AxisDbPool   *pool,
	const gchar  *slug,
	gboolean      archived,
	GError      **error
){
	g_autoptr(AxisProfileRevision) current = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return FALSE;
	}
	ok = lock_profile(conn, slug, &current, error);
	if (ok && current->built_in)
		ok = fail(error, AXIS_PROFILE_ERROR_BUILT_IN, "A built-in profile is always there.");
	if (ok) {
		id = g_strdup_printf("%" G_GINT64_FORMAT, current->profile_id);
		params[0] = id;
		ok = axis_db_conn_exec(conn, "SELECT count(*) FROM conversion_profile_assignment WHERE profile_id = $1::bigint",
			1, params, &result, error);
		if (ok && archived && int_at(result, 0, 0) > 0)
			ok = fail(error, AXIS_PROFILE_ERROR_ASSIGNED, "The profile is assigned to a library; assign another "
				"one there first.");
	}
	if (ok) {
		params[0] = id;
		params[1] = archived ? "true" : "false";
		ok = axis_db_conn_exec(conn, "UPDATE conversion_profile SET archived = $2::boolean WHERE id = $1::bigint", 2,
			params, NULL, error);
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

gboolean
axis_profile_store_assign(
	AxisDbPool   *pool,
	gint64        root_id,
	const gchar  *slug,
	const gchar  *verdict,
	gboolean      confirmed,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisProfileRevision) current = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, root_id);
	g_autofree gchar *profile = NULL;
	g_autofree gchar *revision = NULL;
	const gchar *params[7];
	AxisMediaType type;
	AxisDbConn *conn;
	gboolean ok;

	if (g_strcmp0(verdict, "safe") != 0 && g_strcmp0(verdict, "risky") != 0 && g_strcmp0(verdict, "unknown") != 0)
		return fail(error, AXIS_PROFILE_ERROR_INVALID, "The verdict is not one of safe, risky and unknown.");
	if (strcmp(verdict, "safe") != 0 && !confirmed)
		return fail(error, AXIS_PROFILE_ERROR_CONFIRM, "The verdict for this library is not safe: confirm to assign.");
	if (user == NULL)
		return fail(error, AXIS_PROFILE_ERROR_INVALID, "An assignment is made by someone.");
	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_db_conn_begin(conn, error)) {
		axis_db_pool_release(pool, conn);
		return FALSE;
	}
	ok = lock_profile(conn, slug, &current, error);
	if (ok && current->archived)
		ok = fail(error, AXIS_PROFILE_ERROR_ARCHIVED, "An archived profile is not assigned.");
	if (ok && !current->usable)
		ok = fail(error, AXIS_PROFILE_ERROR_UNUSABLE, "Its definition is not usable, so it is not assigned.");
	if (ok) {
		params[0] = root;
		ok = axis_db_conn_exec(conn, "SELECT media_type FROM library_root WHERE id = $1::bigint FOR SHARE", 1, params,
			&result, error);
		if (ok && axis_db_result_get_n_rows(result) == 0)
			ok = fail(error, AXIS_PROFILE_ERROR_NOT_FOUND, "There is no such library folder.");
		if (ok && (!axis_media_type_from_string(axis_db_result_get_text(result, 0, 0), &type) ||
		           (current->def.libraries & AXIS_TARGET_ROOT(type)) == 0))
			ok = fail(error, AXIS_PROFILE_ERROR_MISMATCH, "That profile cannot be assigned to that library folder.");
	}
	if (ok) {
		profile = g_strdup_printf("%" G_GINT64_FORMAT, current->profile_id);
		revision = g_strdup_printf("%" G_GINT64_FORMAT, current->id);
		params[0] = root;
		params[1] = axis_profile_kind_to_string(current->kind);
		params[2] = profile;
		params[3] = revision;
		params[4] = verdict;
		params[5] = confirmed ? "true" : "false";
		params[6] = user;
		ok = axis_db_conn_exec(conn,
			"INSERT INTO conversion_profile_assignment (root_id, kind, profile_id, revision_id, verdict, confirmed,"
			" assigned_by) VALUES ($1::bigint, $2::text, $3::bigint, $4::bigint, $5::text, $6::boolean, $7::text)"
			" ON CONFLICT (root_id, kind) DO UPDATE SET profile_id = EXCLUDED.profile_id,"
			" revision_id = EXCLUDED.revision_id, verdict = EXCLUDED.verdict, confirmed = EXCLUDED.confirmed,"
			" assigned_by = EXCLUDED.assigned_by, assigned_at = now()",
			7, params, NULL, error);
	}
	ok = finish(conn, ok, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

gboolean
axis_profile_store_unassign(
	AxisDbPool       *pool,
	gint64            root_id,
	AxisProfileKind   kind,
	gboolean         *removed,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, root_id);
	const gchar *params[2];

	if (removed != NULL)
		*removed = FALSE;
	params[0] = root;
	params[1] = axis_profile_kind_to_string(kind);
	if (!axis_db_pool_exec(pool, "DELETE FROM conversion_profile_assignment WHERE root_id = $1::bigint"
	        " AND kind = $2::text", 2, params, &result, error))
		return FALSE;
	if (removed != NULL)
		*removed = axis_db_result_get_affected_rows(result) > 0;
	return TRUE;
}
