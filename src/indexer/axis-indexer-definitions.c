/*
 * axis-indexer-definitions.c - indexer definitions: the built-in ones
 * written at start, installing a file, an override of a built-in one,
 * listing them judged now, removing one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A definition's file is stored whole and judged again (indexer/def/
 * axis-def.h) whenever it is listed or run, so that what the pages say and
 * what an indexer does are this version of the engine's verdict, not the
 * one of the Axis that installed it.  A built-in one (indexer/axis-indexer-
 * shipped.h) is a row like any, written at every start; its override is a
 * row of its own ("<id>.override"), never listed by itself, and which of the
 * two is used is decided here whenever one is read.
 */

#include "indexer/axis-indexer-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "indexer/axis-indexer-shipped.h"
#include "indexer/def/axis-def.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

void
axis_indexer_definition_info_free(AxisIndexerDefinitionInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->def_id);
	g_free(info->name);
	g_free(info->description);
	g_free(info->language);
	g_free(info->site_type);
	g_strfreev(info->links);
	g_free(info->problem);
	g_free(info->sha256);
	g_free(info->override_note);
	g_free(info->origin);
	g_free(info->origin_file);
	g_free(info->origin_commit);
	g_free(info);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return 0;
	return value;
}

gchar *
axis_indexer_definition_in_use(
	const gchar *def_id,
	const gchar *body,
	const gchar *override_body
){
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find(def_id);

	if (shipped == NULL)
		return g_strdup(body);
	/* The copy this daemon ships, whatever the row holds (it is written at
	 * every start), unless the override is newer and usable. */
	if (axis_indexer_shipped_choose(shipped, override_body, NULL, NULL) == AXIS_INDEXER_SHIPPED_OVERRIDDEN)
		return g_strdup(override_body);
	return g_strdup(shipped->body);
}

/* A stored row, judged now. */
static AxisIndexerDefinitionInfo *
info_of(
	AxisDbResult *result,
	guint         row
){
	AxisIndexerDefinitionInfo *info = g_new0(AxisIndexerDefinitionInfo, 1);
	const gchar *stored = axis_db_result_get_text(result, row, 3);
	const gchar *override_body = axis_db_result_get_text(result, row, 9);
	const AxisIndexerShipped *shipped = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *why = NULL;
	g_autoptr(AxisDef) def = NULL;
	GPtrArray *links = g_ptr_array_new();
	guint i;

	info->id = int_at(result, row, 0);
	info->def_id = g_strdup(axis_db_result_get_text(result, row, 1));
	info->name = g_strdup(axis_db_result_get_text(result, row, 2));
	info->sha256 = g_strdup(axis_db_result_get_text(result, row, 4));
	info->size_bytes = int_at(result, row, 5);
	info->installed_us = int_at(result, row, 6);
	info->updated_us = int_at(result, row, 7);
	info->n_indexers = (guint)int_at(result, row, 8);
	shipped = axis_indexer_shipped_find(info->def_id);
	/* Where it came from (add-definition-fetch); a built-in one has none. */
	if (shipped == NULL && axis_db_result_get_text(result, row, 11) != NULL) {
		info->origin = g_strdup(axis_db_result_get_text(result, row, 11));
		info->origin_file = g_strdup(axis_db_result_get_text(result, row, 12));
		info->origin_commit = g_strdup(axis_db_result_get_text(result, row, 13));
		info->origin_us = int_at(result, row, 14);
		info->edited = int_at(result, row, 15) != 0;
		info->newer_upstream = int_at(result, row, 17) != 0;
		info->gone_upstream = int_at(result, row, 18) != 0;
	}
	info->has_previous = shipped == NULL && int_at(result, row, 16) != 0;
	if (info->has_previous) {
		info->previous_from_jackett = axis_db_result_get_text(result, row, 19) != NULL;
		info->previous_us = int_at(result, row, 20);
	}
	if (shipped != NULL) {
		AxisIndexerShippedChoice choice = axis_indexer_shipped_choose(shipped, override_body, &info->override_version,
			&info->override_note);

		info->built_in = TRUE;
		info->built_in_version = shipped->version;
		info->overridden = choice == AXIS_INDEXER_SHIPPED_OVERRIDDEN;
		info->override_ignored = choice == AXIS_INDEXER_SHIPPED_IGNORED;
		info->override_us = override_body != NULL ? int_at(result, row, 10) : 0;
		body = g_strdup(info->overridden ? override_body : shipped->body);
		/* The file in use. */
		g_free(info->sha256);
		info->sha256 = g_compute_checksum_for_string(G_CHECKSUM_SHA256, body, -1);
		info->size_bytes = (gint64)strlen(body);
	} else {
		body = g_strdup(stored);
	}
	def = body != NULL ? axis_def_load(body, strlen(body), &why) : NULL;
	if (def == NULL) {
		/* A file an older Axis took that this one does not read at all. */
		info->problem = g_strdup_printf("This version of Axis does not read it as a definition: %s",
			why != NULL ? why : "it cannot be read.");
		g_ptr_array_add(links, NULL);
		info->links = (gchar **)g_ptr_array_free(links, FALSE);
		return info;
	}
	info->version = def->version;
	info->description = g_strdup(def->description);
	info->language = g_strdup(def->language);
	info->site_type = g_strdup(def->site_type);
	info->usable = axis_def_usable(def);
	info->problem = axis_def_problem(def);
	info->movies = def->covers_movies;
	info->tv = def->covers_tv;
	info->general = def->general;
	for (i = 0; i < def->links->len; i++)
		g_ptr_array_add(links, g_strdup(g_ptr_array_index(def->links, i)));
	for (i = 0; i < def->legacy_links->len; i++)
		g_ptr_array_add(links, g_strdup(g_ptr_array_index(def->legacy_links, i)));
	g_ptr_array_add(links, NULL);
	info->links = (gchar **)g_ptr_array_free(links, FALSE);
	return info;
}

/* Every definition but the overrides, which are shown on their built-in
 * one (their row's id is "axis-...override"). */
static const gchar *const definitions_sql =
	"SELECT d.id, d.def_id, d.name, d.body, d.sha256, octet_length(d.body), " US("d.installed_at") ", "
	US("d.updated_at") ", (SELECT count(*) FROM indexer_source s WHERE s.definition_id = d.id),"
	" o.body, " US("o.updated_at") ","
	" d.origin, d.origin_file, d.origin_commit, " US("d.origin_at") ","
	" (d.origin IS NOT NULL AND d.sha256 IS DISTINCT FROM d.origin_sha256)::int, (d.previous_body IS NOT NULL)::int,"
	" (SELECT (f.blob <> d.origin_blob)::int FROM definition_listing_file f"
	"   WHERE f.origin = d.origin AND f.name = d.origin_file),"
	/* A list was read since, and it does not hold the name. */
	" (d.origin IS NOT NULL AND EXISTS (SELECT 1 FROM definition_listing l WHERE l.origin = d.origin)"
	"   AND NOT EXISTS (SELECT 1 FROM definition_listing_file f WHERE f.origin = d.origin AND f.name = d.origin_file))::int,"
	/* The kept earlier file is "as fetched" only when its content is the one fetched. */
	" CASE WHEN d.previous_origin IS NOT NULL"
	"   AND encode(sha256(convert_to(d.previous_body, 'UTF8')), 'hex') IS NOT DISTINCT FROM d.previous_sha256"
	"   THEN d.previous_origin END, " US("d.previous_at")
	" FROM indexer_definition d LEFT JOIN indexer_definition o ON o.def_id = d.def_id || '" AXIS_INDEXER_OVERRIDE_SUFFIX "'"
	" WHERE ($1::bigint = 0 OR d.id = $1::bigint) AND d.def_id NOT LIKE '" AXIS_INDEXER_SHIPPED_PREFIX "%"
	AXIS_INDEXER_OVERRIDE_SUFFIX "'"
	" ORDER BY lower(d.name), d.id";

static gboolean
read_definitions(
	AxisIndexers  *self,
	gint64         id,
	GPtrArray    **definitions,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];
	guint i;

	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, definitions_sql, 1, params, &result, error))
		return FALSE;
	*definitions = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_definition_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*definitions, info_of(result, i));
	return TRUE;
}

gboolean
axis_indexers_list_definitions(
	AxisIndexers  *self,
	GPtrArray    **definitions,
	GError       **error
){
	return read_definitions(self, 0, definitions, error);
}

gboolean
axis_indexers_get_definition(
	AxisIndexers               *self,
	gint64                      id,
	AxisIndexerDefinitionInfo **definition,
	GError                    **error
){
	g_autoptr(GPtrArray) all = NULL;

	*definition = NULL;
	if (id <= 0 || !read_definitions(self, id, &all, error)) {
		if (id <= 0)
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such definition");
		return FALSE;
	}
	if (all->len == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such definition");
		return FALSE;
	}
	*definition = g_ptr_array_steal_index(all, 0);
	return TRUE;
}

gboolean
axis_indexers_get_definition_text(
	AxisIndexers  *self,
	gint64         id,
	gchar        **text,
	GError       **error
){
	AxisDbConn *conn;
	gboolean ok;

	*text = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_definition_body(conn, id, text, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && *text == NULL) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such definition");
		return FALSE;
	}
	return ok;
}

/* ── Writing rows ───────────────────────────────────────────────── */

/* After the definition @id_text changed: its indexers' kept searches were
 * read by the old one, and a hold for what its answers were is over (not a
 * site's own request to wait: axis_indexer_store_lift_hold()). */
gboolean
axis_indexer_definition_changed(
	AxisDbConn   *conn,
	const gchar  *id_text,
	GError      **error
){
	const gchar *params[1];
	gint64 id = 0;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE source_id IN"
	        " (SELECT id FROM indexer_source WHERE definition_id = $1::bigint)", 1, params, NULL, error))
		return FALSE;
	id = g_ascii_strtoll(id_text, NULL, 10);
	return axis_indexer_store_lift_hold(conn, 0, id, error);
}

/*
 * Writes the row of @def_id with @name and @body in a transaction of its
 * own: inserted when missing (counted against the most when @counted), its
 * file replaced when it differs.  @id (out): the row; @changed (out): it was
 * inserted or replaced; @existed (out) and @was_usable (out): what was there.
 */
static gboolean
write_row(
	AxisDbConn   *conn,
	const gchar  *def_id,
	const gchar  *name,
	const gchar  *body,
	gboolean      counted,
	gint64       *id,
	gboolean     *changed,
	gboolean     *existed,
	gboolean     *was_usable,
	GError      **error
){
	g_autoptr(AxisDbResult) existing = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, body, -1);
	g_autofree gchar *id_text = NULL;
	GError *local = NULL;
	const gchar *params[4];
	gboolean ok;

	*id = 0;
	*changed = FALSE;
	if (existed != NULL)
		*existed = FALSE;
	if (was_usable != NULL)
		*was_usable = FALSE;
	params[0] = def_id;
	ok = axis_db_conn_begin(conn, &local) &&
		axis_db_conn_exec(conn, "SELECT id, sha256, body FROM indexer_definition WHERE def_id = $1::text FOR UPDATE",
			1, params, &existing, &local);
	if (ok && axis_db_result_get_n_rows(existing) > 0) {
		const gchar *old_body = axis_db_result_get_text(existing, 0, 2);
		g_autoptr(AxisDef) old = old_body != NULL ? axis_def_load(old_body, strlen(old_body), NULL) : NULL;

		*id = int_at(existing, 0, 0);
		if (existed != NULL)
			*existed = TRUE;
		if (was_usable != NULL)
			*was_usable = axis_def_usable(old);
		if (g_strcmp0(axis_db_result_get_text(existing, 0, 1), sha) != 0) {
			id_text = axis_indexer_int_text(*id);
			params[0] = id_text;
			params[1] = name;
			params[2] = body;
			params[3] = sha;
			*changed = TRUE;
			ok = axis_db_conn_exec(conn, "UPDATE indexer_definition SET name = $2::text, body = $3::text,"
				" sha256 = $4::text, updated_at = now() WHERE id = $1::bigint", 4, params, NULL, &local) &&
				axis_indexer_definition_changed(conn, id_text, &local);
		}
	} else if (ok) {
		if (counted) {
			g_autoptr(AxisDbResult) count = NULL;

			ok = axis_db_conn_exec(conn, "SELECT count(*) FROM indexer_definition WHERE def_id NOT LIKE '"
				AXIS_INDEXER_SHIPPED_PREFIX "%'", 0, NULL, &count, &local);
			if (ok && int_at(count, 0, 0) >= AXIS_INDEXER_MAX_DEFINITIONS) {
				ok = FALSE;
				g_set_error(&local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
					"At most %d definitions can be installed.", AXIS_INDEXER_MAX_DEFINITIONS);
			}
		}
		params[0] = def_id;
		params[1] = name;
		params[2] = body;
		params[3] = sha;
		ok = ok && axis_db_conn_exec(conn, "INSERT INTO indexer_definition (def_id, name, body, sha256)"
			" VALUES ($1::text, $2::text, $3::text, $4::text) RETURNING id", 4, params, &result, &local);
		if (ok) {
			*id = int_at(result, 0, 0);
			*changed = TRUE;
		}
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok) {
		axis_db_conn_rollback(conn, NULL);
		g_propagate_error(error, local);
		return FALSE;
	}
	return TRUE;
}

/* The row of a built-in definition, written now if it is missing. */
static gboolean
shipped_row(
	AxisDbConn                *conn,
	const AxisIndexerShipped  *shipped,
	gint64                    *id,
	GError                   **error
){
	gboolean changed = FALSE;

	return write_row(conn, shipped->def_id, shipped->name, shipped->body, FALSE, id, &changed, NULL, NULL, error);
}

/* Stores @data as @shipped's override, after its checks. */
static gboolean
install_override(
	AxisDbConn                      *conn,
	const AxisIndexerShipped        *shipped,
	const AxisDef                   *def,
	const gchar                     *data,
	gsize                            length,
	AxisIndexerDefinitionInstalled  *installed,
	GError                         **error
){
	g_autofree gchar *problem = NULL;
	g_autofree gchar *override_id = axis_indexer_shipped_override_id(shipped->def_id);
	g_autofree gchar *body = g_strndup(data, length);
	g_autofree gchar *id_text = NULL;
	gint64 override_row = 0;
	gboolean changed = FALSE;
	gboolean existed = FALSE;

	if (!axis_indexer_shipped_check_override(shipped, data, length, &problem)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "That file was not taken as an override "
			"of the built-in %s. %s", shipped->name, problem);
		return FALSE;
	}
	if (!shipped_row(conn, shipped, &installed->id, error) ||
	    !write_row(conn, override_id, def->name, body, FALSE, &override_row, &changed, &existed, &installed->was_usable,
	        error))
		return FALSE;
	installed->override = TRUE;
	installed->replaced = existed;
	installed->unchanged = !changed;
	/* The built-in one's indexers run the override from now. */
	id_text = axis_indexer_int_text(installed->id);
	return !changed || axis_indexer_definition_changed(conn, id_text, error);
}

gboolean
axis_indexers_install_definition(
	AxisIndexers                    *self,
	const gchar                     *data,
	gsize                            length,
	AxisIndexerDefinitionInstalled  *installed,
	GError                         **error
){
	g_autoptr(AxisDef) def = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *body = NULL;
	const AxisIndexerShipped *shipped;
	AxisDbConn *conn;
	gboolean changed = FALSE;
	gboolean ok;

	memset(installed, 0, sizeof(*installed));
	if (data == NULL || length == 0 || length > AXIS_INDEXER_DEFINITION_MAX_BYTES) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"A definition file is 1 byte to %d KiB.", AXIS_INDEXER_DEFINITION_MAX_BYTES / 1024);
		return FALSE;
	}
	def = axis_def_load(data, length, &problem);
	if (def == NULL) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "That file was not installed. %s", problem);
		return FALSE;
	}
	shipped = axis_indexer_shipped_find(def->id);
	if (shipped == NULL && axis_indexer_shipped_reserved(def->id)) {
		g_set_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, "That file was not installed: ids that "
			"begin with \"%s\" are Axis's own built-in definitions, and this version of Axis has none called %.64s.",
			AXIS_INDEXER_SHIPPED_PREFIX, def->id);
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (shipped != NULL) {
		ok = install_override(conn, shipped, def, data, length, installed, error);
		axis_db_pool_release(self->pool, conn);
		return ok;
	}
	/* The reader took it whole: valid UTF-8 without NUL. */
	body = g_strndup(data, length);
	ok = write_row(conn, def->id, def->name, body, TRUE, &installed->id, &changed, &installed->replaced,
		&installed->was_usable, error);
	installed->unchanged = installed->replaced && !changed;
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_indexers_remove_definition(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];

	if (!axis_indexers_get_definition(self, id, &info, error))
		return FALSE;
	if (info->built_in) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"That definition is built into Axis and is not removed; an indexer that runs it is removed on its own.");
		return FALSE;
	}
	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, "DELETE FROM indexer_definition d WHERE d.id = $1::bigint AND NOT EXISTS"
	        " (SELECT 1 FROM indexer_source s WHERE s.definition_id = d.id)", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"That definition is run by an indexer: remove the indexer first.");
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_indexers_remove_definition_override(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
){
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *override_id = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];
	GError *local = NULL;
	AxisDbConn *conn;
	gboolean ok;

	if (!axis_indexers_get_definition(self, id, &info, error))
		return FALSE;
	if (!info->built_in || (!info->overridden && !info->override_ignored)) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"That definition has no override to remove.");
		return FALSE;
	}
	override_id = axis_indexer_shipped_override_id(info->def_id);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = override_id;
	ok = axis_db_conn_begin(conn, &local) &&
		axis_db_conn_exec(conn, "DELETE FROM indexer_definition WHERE def_id = $1::text", 1, params, &result, &local) &&
		axis_indexer_definition_changed(conn, id_text, &local) &&
		axis_db_conn_commit(conn, &local);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		g_propagate_error(error, local);
		return FALSE;
	}
	return TRUE;
}

/* ── At start ───────────────────────────────────────────────────── */

/* The version of @shipped's stored override when it is one this engine can
 * run, else 0; @same (out): its body is @data. */
static gint
stored_override_version(
	AxisDbConn                *conn,
	const AxisIndexerShipped  *shipped,
	const gchar               *data,
	gboolean                  *same
){
	g_autofree gchar *override_id = axis_indexer_shipped_override_id(shipped->def_id);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDef) def = NULL;
	const gchar *params[1];
	const gchar *body;

	*same = FALSE;
	params[0] = override_id;
	if (!axis_db_conn_exec(conn, "SELECT body FROM indexer_definition WHERE def_id = $1::text", 1, params, &result,
	        NULL) || axis_db_result_get_n_rows(result) == 0)
		return 0;
	body = axis_db_result_get_text(result, 0, 0);
	if (body == NULL)
		return 0;
	*same = strcmp(body, data) == 0;
	def = axis_def_load(body, strlen(body), NULL);
	return def != NULL && axis_def_usable(def) ? def->version : 0;
}

/*
 * The override an administrator placed in @dir for @shipped: taken when it
 * passes the checks and is newer than the stored override (one pasted or
 * uploaded on the definition's page may be newer, and is then kept), or
 * the stored one cannot be run; else said why in the log.  Its size is
 * looked at before it is read, and no more than the most is read.
 */
static void
override_from_file(
	AxisDbConn                *conn,
	const gchar               *dir,
	const AxisIndexerShipped  *shipped
){
	g_autofree gchar *name = g_strconcat(shipped->def_id, ".yml", NULL);
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	g_autofree gchar *data = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(AxisDef) def = NULL;
	AxisIndexerDefinitionInstalled installed;
	gboolean same = FALSE;
	gsize length = 0;
	gint stored;

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		return;
	data = axis_indexer_read_file_at_most(path, AXIS_INDEXER_DEFINITION_MAX_BYTES, &length, &local);
	if (data == NULL) {
		g_warning("indexers: the override %s is not taken: %s (a definition file is 1 byte to %d KiB)", path,
			local->message, AXIS_INDEXER_DEFINITION_MAX_BYTES / 1024);
		return;
	}
	if (length == 0) {
		g_warning("indexers: the override %s is not taken: it is empty", path);
		return;
	}
	def = axis_def_load(data, length, NULL);
	if (def == NULL || strcmp(def->id, shipped->def_id) != 0) {
		g_warning("indexers: the override %s is not taken: it is not a definition with the id %s", path,
			shipped->def_id);
		return;
	}
	stored = stored_override_version(conn, shipped, data, &same);
	if (same)
		return;
	if (stored >= def->version) {
		g_message("indexers: the override %s (version %d) of the built-in %s is not taken: the override stored "
			"(pasted or uploaded on its page, or an earlier file), version %d, is as new or newer", path,
			def->version, shipped->name, stored);
		return;
	}
	memset(&installed, 0, sizeof(installed));
	if (!install_override(conn, shipped, def, data, length, &installed, &local)) {
		g_warning("indexers: the override %s is not taken: %s", path, local->message);
		return;
	}
	if (!installed.unchanged)
		g_message("indexers: the override %s (version %d) of the built-in %s is in use", path, def->version,
			shipped->name);
}

gboolean
axis_indexers_sync_shipped(
	AxisIndexers  *self,
	const gchar   *definitions_dir,
	GError       **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	guint i;

	if (conn == NULL)
		return FALSE;
	for (i = 0; i < axis_indexer_shipped_count(); i++) {
		const AxisIndexerShipped *shipped = axis_indexer_shipped_at(i);
		gboolean changed = FALSE;
		gint64 id = 0;

		if (!write_row(conn, shipped->def_id, shipped->name, shipped->body, FALSE, &id, &changed, NULL, NULL, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
		if (changed)
			g_message("indexers: the built-in definition %s is version %d", shipped->def_id, shipped->version);
		if (definitions_dir != NULL)
			override_from_file(conn, definitions_dir, shipped);
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}
