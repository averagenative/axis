/*
 * axis-meta-store.c - the metadata tables: what is written and read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-private.h"

#include <string.h>

#include "db/axis-db-result.h"
#include "release/axis-release.h"

/* Microseconds since 1970 of a timestamptz column, NULL as NULL. */
#define EPOCH_US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Helpers ────────────────────────────────────────────────────── */

gchar *
axis_meta_sql_text_array(GPtrArray *items)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; items != NULL && i < items->len; i++) {
		const gchar *item = g_ptr_array_index(items, i);
		const gchar *p;

		if (i > 0)
			g_string_append_c(out, ',');
		if (item == NULL) {
			g_string_append(out, "NULL");
			continue;
		}
		g_string_append_c(out, '"');
		for (p = item; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

gchar *
axis_meta_sql_int_array(GArray *ids)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* "{1,2,3}" (a bigint[] as PostgreSQL writes it) as numbers. */
static GArray *
parse_int_array(const gchar *text)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_auto(GStrv) parts = NULL;
	gsize n;
	guint i;

	if (text == NULL)
		return ids;
	n = strlen(text);
	if (n < 2 || text[0] != '{' || text[n - 1] != '}')
		return ids;
	{
		g_autofree gchar *inner = g_strndup(text + 1, n - 2);

		parts = g_strsplit(inner, ",", -1);
	}
	for (i = 0; parts[i] != NULL; i++) {
		gint64 value;
		gchar *end = NULL;

		if (parts[i][0] == '\0')
			continue;
		value = g_ascii_strtoll(parts[i], &end, 10);
		if (end != NULL && *end == '\0' && value > 0)
			g_array_append_val(ids, value);
	}
	return ids;
}

void
axis_meta_set_failed(
	GError      **error,
	const GError *cause
){
	g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_FAILED,
		cause != NULL ? cause->message : "the store could not be used");
}

static gchar *
int_text(gint64 value)
{
	return value != 0 ? g_strdup_printf("%" G_GINT64_FORMAT, value) : NULL;
}

static gboolean
exec(
	AxisDbConn          *conn,
	const gchar         *sql,
	GPtrArray           *params,
	AxisDbResult       **result,
	GError             **error
){
	return axis_db_conn_exec(conn, sql, params != NULL ? params->len : 0,
		params != NULL ? (const gchar *const *)params->pdata : NULL, result, error);
}

static gint64
result_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return 0;
	return value;
}

static gchar *
result_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

/* ── Name keys ──────────────────────────────────────────────────── */

gchar *
axis_meta_name_keys(
	const gchar        *title,
	const gchar        *original_title,
	const gchar *const *other_names
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GString *keys = g_string_new(NULL);
	const gchar *names[2];
	guint i;

	names[0] = title;
	names[1] = original_title;
	for (i = 0; i < 2 + (other_names != NULL ? g_strv_length((gchar **)other_names) : 0); i++) {
		const gchar *name = i < 2 ? names[i] : other_names[i - 2];
		gchar *key = name != NULL ? axis_release_title_key(name) : NULL;

		/* Nothing a key is made of, nor one already there. */
		if (key == NULL || key[0] == '\0' || g_hash_table_contains(seen, key)) {
			g_free(key);
			continue;
		}
		g_string_append_printf(keys, "%s%s", keys->len > 0 ? "|" : "", key);
		g_hash_table_add(seen, key);
	}
	return g_string_free(keys, FALSE);
}

/* The title @id_text's name keys from what is stored of it now (its names
 * and other names), written on @conn. */
static gboolean
store_name_keys(
	AxisDbConn   *conn,
	const gchar  *id_text,
	GError      **error
){
	g_autoptr(AxisDbResult) names = NULL;
	g_autoptr(GPtrArray) others = g_ptr_array_new();
	g_autofree gchar *keys = NULL;
	const gchar *params[2];
	guint i;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT t.title, t.original_title, a.title FROM meta_title t"
	        " LEFT JOIN meta_alt_title a ON a.title_id = t.id WHERE t.id = $1::bigint ORDER BY a.title",
	        1, params, &names, error))
		return FALSE;
	if (axis_db_result_get_n_rows(names) == 0)
		return TRUE;
	for (i = 0; i < axis_db_result_get_n_rows(names); i++) {
		if (!axis_db_result_is_null(names, i, 2))
			g_ptr_array_add(others, (gpointer)axis_db_result_get_text(names, i, 2));
	}
	g_ptr_array_add(others, NULL);
	keys = axis_meta_name_keys(axis_db_result_get_text(names, 0, 0), axis_db_result_get_text(names, 0, 1),
		(const gchar *const *)others->pdata);
	params[1] = keys;
	return axis_db_conn_exec(conn, "UPDATE meta_title SET name_keys = $2 WHERE id = $1::bigint", 2, params, NULL,
		error);
}

/* axis_meta_fill_name_keys() on @pool. */
static gboolean
fill_name_keys(
	AxisDbPool  *pool,
	guint        limit,
	guint       *filled,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *count = g_strdup_printf("%u", CLAMP(limit, 1, 5000));
	g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *id_array = NULL;
	g_autofree gchar *key_array = NULL;
	const gchar *params[2];
	guint i;

	if (filled != NULL)
		*filled = 0;
	params[0] = count;
	/* Each title with its other names, gathered in one row and parted by a
	 * control character (stored text holds none). */
	if (!axis_db_pool_exec(pool, "SELECT t.id, t.title, t.original_title,"
	        " string_agg(a.title, chr(31) ORDER BY a.title)"
	        " FROM (SELECT id, title, original_title FROM meta_title WHERE name_keys IS NULL ORDER BY id"
	        "  LIMIT $1::integer) t LEFT JOIN meta_alt_title a ON a.title_id = t.id"
	        " GROUP BY t.id, t.title, t.original_title ORDER BY t.id", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *joined = axis_db_result_get_text(rows, i, 3);
		g_auto(GStrv) others = joined != NULL ? g_strsplit(joined, "\x1f", -1) : NULL;

		g_ptr_array_add(ids, g_strdup(axis_db_result_get_text(rows, i, 0)));
		g_ptr_array_add(keys, axis_meta_name_keys(axis_db_result_get_text(rows, i, 1),
			axis_db_result_get_text(rows, i, 2), (const gchar *const *)others));
	}
	if (ids->len == 0)
		return TRUE;
	id_array = axis_meta_sql_text_array(ids);
	key_array = axis_meta_sql_text_array(keys);
	params[0] = id_array;
	params[1] = key_array;
	if (!axis_db_pool_exec(pool, "UPDATE meta_title t SET name_keys = u.k"
	        " FROM unnest($1::bigint[], $2::text[]) AS u (id, k) WHERE t.id = u.id AND t.name_keys IS NULL",
	        2, params, NULL, error))
		return FALSE;
	if (filled != NULL)
		*filled = ids->len;
	return TRUE;
}

gboolean
axis_meta_fill_name_keys(
	AxisMeta  *self,
	guint      limit,
	guint     *filled,
	GError   **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	return fill_name_keys(axis_meta_get_pool(self), limit, filled, error);
}

gboolean
axis_meta_fill_every_name_key(
	AxisDbPool  *pool,
	GError     **error
){
	guint filled = 0;

	g_return_val_if_fail(pool != NULL, FALSE);

	do {
		if (!fill_name_keys(pool, 2000, &filled, error))
			return FALSE;
	} while (filled > 0);
	return TRUE;
}

/* ── Writing a title ────────────────────────────────────────────── */

/* The other record @other of a title becomes part of @keep. */
static gboolean
merge(
	AxisDbConn  *conn,
	gint64       keep,
	gint64       other,
	GError     **error
){
	static const gchar *const statements[] = {
		"UPDATE meta_external_id SET title_id = $1::bigint WHERE title_id = $2::bigint",
		"INSERT INTO meta_alt_title (title_id, title, country, language)"
		" SELECT $1::bigint, title, country, language FROM meta_alt_title WHERE title_id = $2::bigint"
		" ON CONFLICT DO NOTHING",
		"UPDATE meta_season SET title_id = $1::bigint WHERE title_id = $2::bigint"
		" AND (source, number) NOT IN (SELECT source, number FROM meta_season WHERE title_id = $1::bigint)",
		/* A claim between the two is settled by the merge. */
		"DELETE FROM meta_id_claim WHERE (title_id = $1::bigint AND held_by = $2::bigint)"
		" OR (title_id = $2::bigint AND held_by = $1::bigint)",
		"UPDATE meta_id_claim SET held_by = $1::bigint WHERE held_by = $2::bigint",
		"UPDATE meta_title k SET viewed_at = CASE WHEN k.viewed_at IS NULL THEN o.viewed_at"
		"   WHEN o.viewed_at IS NULL THEN k.viewed_at ELSE greatest(k.viewed_at, o.viewed_at) END,"
		" created_at = least(k.created_at, o.created_at), adult = k.adult OR o.adult"
		" FROM meta_title o WHERE k.id = $1::bigint AND o.id = $2::bigint",
		"UPDATE meta_search SET result_ids = array_replace(result_ids, $2::bigint, $1::bigint)"
		" WHERE $2::bigint = ANY (result_ids)",
		/* What a connection brought over follows the title; a connection
		 * that brought both keeps the kept title's record (the other's
		 * goes with the title). */
		"UPDATE arr_brought SET title_id = $1::bigint WHERE title_id = $2::bigint"
		" AND source_id NOT IN (SELECT source_id FROM arr_brought WHERE title_id = $1::bigint)",
		"DELETE FROM meta_title WHERE id = $2::bigint AND id <> $1::bigint",
		NULL
	};
	g_autofree gchar *a = g_strdup_printf("%" G_GINT64_FORMAT, keep);
	g_autofree gchar *b = g_strdup_printf("%" G_GINT64_FORMAT, other);
	g_autoptr(AxisDbResult) doubled = NULL;
	g_autoptr(GPtrArray) schemes = g_ptr_array_new();
	g_autoptr(GPtrArray) values = g_ptr_array_new();
	g_autofree gchar *scheme_array = NULL;
	g_autofree gchar *value_array = NULL;
	const gchar *params[4];
	guint i;

	params[0] = a;
	params[1] = b;
	/* The other record's identifiers in schemes the kept one holds one of:
	 * moved with the rest (what follows a title follows them), then dropped,
	 * so that the title holds one identifier a scheme (review of V1/V2,
	 * blocker 2); the answer being stored then sets those it names. */
	if (!axis_db_conn_exec(conn, "SELECT o.scheme, o.value FROM meta_external_id o WHERE o.title_id = $2::bigint"
	        " AND EXISTS (SELECT 1 FROM meta_external_id k WHERE k.title_id = $1::bigint AND k.kind = o.kind"
	        " AND k.scheme = o.scheme)", 2, params, &doubled, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(doubled); i++) {
		g_ptr_array_add(schemes, (gpointer)axis_db_result_get_text(doubled, i, 0));
		g_ptr_array_add(values, (gpointer)axis_db_result_get_text(doubled, i, 1));
	}
	for (i = 0; statements[i] != NULL; i++) {
		if (!axis_db_conn_exec(conn, statements[i], 2, params, NULL, error))
			return FALSE;
	}
	if (schemes->len == 0)
		return TRUE;
	scheme_array = axis_meta_sql_text_array(schemes);
	value_array = axis_meta_sql_text_array(values);
	params[1] = scheme_array;
	params[2] = value_array;
	return axis_db_conn_exec(conn, "DELETE FROM meta_external_id WHERE title_id = $1::bigint AND (scheme, value) IN"
		" (SELECT s, v FROM unnest($2::text[], $3::text[]) AS u (s, v))", 3, params, NULL, error);
}

/*
 * Which stored title an answer is, and which of the titles its identifiers
 * name become part of it (add-metadata-tvdb, the cross-id rule): the title
 * holding the answer's own identifier (its provider's record, or one that
 * names that identifier), else one of the same provider's records holding
 * another of its identifiers, else none (a new one).  Of the others its
 * identifiers name, those of the same provider are merged into it, as two
 * records of one provider that turn out to be one title always were; a
 * title that holds another provider's record is never merged by a claim.
 */
static gboolean
choose_title(
	AxisDbResult  *found,
	const gchar   *provider,
	gint64        *keep,
	gchar        **keep_provider,
	GArray        *merged
){
	gint64 own = 0;
	gint64 same = 0;
	gboolean own_other = FALSE;
	guint i;

	*keep = 0;
	*keep_provider = NULL;
	for (i = 0; i < axis_db_result_get_n_rows(found); i++) {
		gint64 id = result_int(found, i, 0);
		gboolean holds_own = FALSE;
		gboolean mine = g_strcmp0(axis_db_result_get_text(found, i, 1), provider) == 0;

		axis_db_result_get_boolean(found, i, 2, &holds_own);
		if (holds_own && own == 0) {
			own = id;
			own_other = !mine;
		}
		/* Rows come in id order: the oldest of the provider's records. */
		if (same == 0 && mine)
			same = id;
	}
	/* Another provider's record that names the answer's own id is the
	 * title; among the provider's own records the oldest is kept, as ever. */
	*keep = own != 0 && own_other ? own : same;
	for (i = 0; i < axis_db_result_get_n_rows(found); i++) {
		gint64 id = result_int(found, i, 0);

		if (id == *keep)
			*keep_provider = result_text(found, i, 1);
		else if (*keep != 0 && g_strcmp0(axis_db_result_get_text(found, i, 1), provider) == 0)
			g_array_append_val(merged, id);
	}
	return *keep != 0;
}

static gboolean
store_title_in_transaction(
	AxisDbConn           *conn,
	const AxisMetaTitle  *title,
	gboolean              secondary,
	gint64               *id_out,
	GError              **error
){
	const gchar *kind = axis_meta_kind_to_string(title->kind);
	g_autoptr(GPtrArray) schemes = g_ptr_array_new();
	g_autoptr(GPtrArray) values = g_ptr_array_new();
	g_autofree gchar *scheme_array = NULL;
	g_autofree gchar *value_array = NULL;
	g_autofree gchar *lock = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_META_TITLE_LOCK);
	g_autofree gchar *id_text = NULL;
	g_autofree gchar *keep_provider = NULL;
	g_autoptr(AxisDbResult) found = NULL;
	g_autoptr(GPtrArray) params = NULL;
	g_autoptr(GArray) merged = g_array_new(FALSE, FALSE, sizeof(gint64));
	gboolean record;
	gint64 keep = 0;
	guint i;

	for (i = 0; i < title->ids->len; i++) {
		const AxisMetaExternalId *id = g_ptr_array_index(title->ids, i);

		g_ptr_array_add(schemes, id->scheme);
		g_ptr_array_add(values, id->value);
	}
	scheme_array = axis_meta_sql_text_array(schemes);
	value_array = axis_meta_sql_text_array(values);

	params = g_ptr_array_new();
	g_ptr_array_add(params, lock);
	if (!exec(conn, "SELECT pg_advisory_xact_lock($1::bigint)", params, NULL, error))
		return FALSE;

	g_ptr_array_set_size(params, 0);
	g_ptr_array_add(params, (gpointer)kind);
	g_ptr_array_add(params, scheme_array);
	g_ptr_array_add(params, value_array);
	g_ptr_array_add(params, title->provider);
	g_ptr_array_add(params, title->provider_id);
	if (!exec(conn, "SELECT e.title_id, t.provider, bool_or(e.scheme = $4 AND e.value = $5) FROM meta_external_id e"
	        " JOIN meta_title t ON t.id = e.title_id"
	        " WHERE e.kind = $1 AND (e.scheme, e.value) IN (SELECT s, v FROM unnest($2::text[], $3::text[]) AS u (s, v))"
	        " GROUP BY e.title_id, t.provider ORDER BY e.title_id", params, &found, error))
		return FALSE;

	if (!choose_title(found, title->provider, &keep, &keep_provider, merged)) {
		g_autoptr(AxisDbResult) inserted = NULL;

		/* A secondary provider (TheTVDB) adds to a title Axis holds and
		 * never makes one of its own (review of V1/V2, blocker 1: until a
		 * series can follow it): nothing is stored. */
		if (secondary) {
			*id_out = 0;
			return TRUE;
		}
		g_ptr_array_set_size(params, 0);
		g_ptr_array_add(params, (gpointer)kind);
		g_ptr_array_add(params, title->title);
		g_ptr_array_add(params, title->provider);
		g_ptr_array_add(params, title->provider_id);
		if (!exec(conn, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES ($1, $2, $3, $4)"
		        " RETURNING id", params, &inserted, error))
			return FALSE;
		keep = result_int(inserted, 0, 0);
		keep_provider = g_strdup(title->provider);
	} else {
		for (i = 0; i < merged->len; i++) {
			if (!merge(conn, keep, g_array_index(merged, gint64, i), error))
				return FALSE;
		}
	}
	id_text = g_strdup_printf("%" G_GINT64_FORMAT, keep);
	/* The record provider keeps the title's own fields: a secondary
	 * provider's answer about a title another provider's record made adds
	 * identifiers, other names and its own seasons only (TMDb whenever TMDb
	 * knows it). */
	record = !secondary || g_strcmp0(keep_provider, title->provider) == 0;

	if (record) {
		g_autofree gchar *year = int_text(title->year);
		g_autofree gchar *runtime = int_text(title->runtime);
		g_autofree gchar *genres = title->genres != NULL ? axis_meta_sql_text_array(title->genres) : NULL;
		g_autofree gchar *regional = int_text(title->regional_year);

		g_ptr_array_set_size(params, 0);
		g_ptr_array_add(params, id_text);
		g_ptr_array_add(params, title->title);
		g_ptr_array_add(params, title->original_title);
		g_ptr_array_add(params, year);
		g_ptr_array_add(params, title->overview);
		g_ptr_array_add(params, runtime);
		g_ptr_array_add(params, (gpointer)axis_meta_status_to_string(title->status));
		g_ptr_array_add(params, genres);
		g_ptr_array_add(params, title->original_language);
		g_ptr_array_add(params, title->release_date);
		g_ptr_array_add(params, title->first_air_date);
		g_ptr_array_add(params, title->last_air_date);
		g_ptr_array_add(params, title->poster_ref);
		g_ptr_array_add(params, title->backdrop_ref);
		g_ptr_array_add(params, title->provider);
		g_ptr_array_add(params, title->provider_id);
		g_ptr_array_add(params, (gpointer)(title->detail ? "true" : "false"));
		g_ptr_array_add(params, regional);
		g_ptr_array_add(params, (gpointer)(!title->adult_known ? NULL : title->adult ? "true" : "false"));
		/* The adult mark: a title that is not from a provider's answer says
		 * nothing of it and leaves it as it is; an answer makes a mark that
		 * was not known known, and a mark once set is cleared by none (a
		 * brief record need not say).  A search's answer never takes a full
		 * record's year and dates: the
		 * full record's own release date (its first release) and its date in
		 * the region stand, whatever date a search gives; the year of its
		 * theatrical release in the region is a full record's alone. */
		if (!exec(conn, "UPDATE meta_title SET title = $2,"
		        " original_title = coalesce($3, original_title),"
		        " year = CASE WHEN $17::boolean OR detail_fetched_at IS NULL THEN coalesce($4::integer, year)"
		        "  ELSE year END,"
		        " overview = coalesce($5, overview),"
		        " runtime_minutes = coalesce($6::integer, runtime_minutes),"
		        " status = coalesce($7, status),"
		        " genres = coalesce($8::text[], genres),"
		        " original_language = coalesce($9, original_language),"
		        " release_date = CASE WHEN $17::boolean OR detail_fetched_at IS NULL THEN coalesce($10::date, release_date)"
		        "  ELSE release_date END,"
		        " first_air_date = CASE WHEN $17::boolean OR detail_fetched_at IS NULL"
		        "  THEN coalesce($11::date, first_air_date) ELSE first_air_date END,"
		        " last_air_date = coalesce($12::date, last_air_date),"
		        " poster_ref = coalesce($13, poster_ref),"
		        " backdrop_ref = coalesce($14, backdrop_ref),"
		        " provider = $15, provider_id = $16, fetched_at = now(),"
		        " detail_fetched_at = CASE WHEN $17::boolean THEN now() ELSE detail_fetched_at END,"
		        " regional_year = CASE WHEN $17::boolean THEN $18::integer ELSE regional_year END,"
		        " adult = CASE WHEN $19::boolean IS NULL THEN adult ELSE coalesce(adult, false) OR $19::boolean END"
		        " WHERE id = $1::bigint", params, NULL, error))
			return FALSE;
	}

	/* The answer's identifiers.  From the record provider, an identifier it
	 * gives in a scheme replaces the one stored in that scheme; another
	 * provider's adds those of schemes the title has none in.  Neither takes
	 * an identifier another title holds (a title merged above holds none
	 * now): that claim is kept, apart, and shown (meta_id_claim). */
	g_ptr_array_set_size(params, 0);
	g_ptr_array_add(params, id_text);
	g_ptr_array_add(params, (gpointer)kind);
	g_ptr_array_add(params, scheme_array);
	g_ptr_array_add(params, value_array);
	{
		/* A record another provider made before, taken over now, keeps that
		 * provider's own identifier of it, whatever this answer says of
		 * that scheme; a record of the answering provider's own has no such
		 * scheme (review of V1/V2, blocker 2). */
		const gchar *exempt = g_strcmp0(keep_provider, title->provider) != 0 ? keep_provider : "";
		const gchar *ids[6];

		ids[0] = id_text;
		ids[1] = kind;
		ids[2] = scheme_array;
		ids[3] = value_array;
		ids[4] = exempt;
		ids[5] = record ? "true" : "false";
		if (record && !axis_db_conn_exec(conn, "DELETE FROM meta_external_id WHERE title_id = $1::bigint"
		        " AND scheme = ANY ($3::text[]) AND scheme <> $5"
		        " AND (scheme, value) NOT IN (SELECT s, v FROM unnest($3::text[], $4::text[]) AS u (s, v))"
		        " AND $2 = kind AND $6::boolean", 6, ids, NULL, error))
			return FALSE;
		/* One identifier a scheme: the record's replace those stored (but
		 * in the exempt scheme); another provider's only fill a scheme the
		 * title has none in. */
		if (!axis_db_conn_exec(conn, "INSERT INTO meta_external_id (title_id, kind, scheme, value)"
		        " SELECT $1::bigint, $2::text, s, v FROM unnest($3::text[], $4::text[]) AS u (s, v)"
		        " WHERE ($6::boolean AND s <> $5)"
		        "  OR NOT EXISTS (SELECT 1 FROM meta_external_id x WHERE x.title_id = $1::bigint AND x.scheme = s)"
		        " ON CONFLICT (kind, scheme, value) DO NOTHING", 6, ids, NULL, error))
			return FALSE;
	}
	g_ptr_array_add(params, title->provider);
	/* A full record says what the provider claims now; a brief one (a
	 * search's) adds what it names. */
	if (title->detail) {
		const gchar *drop[2];

		drop[0] = id_text;
		drop[1] = title->provider;
		if (!axis_db_conn_exec(conn, "DELETE FROM meta_id_claim WHERE title_id = $1::bigint AND provider = $2", 2,
		        drop, NULL, error))
			return FALSE;
	}
	if (!exec(conn, "INSERT INTO meta_id_claim (title_id, provider, scheme, value, held_by)"
	        " SELECT $1::bigint, $5, s, v, x.title_id FROM unnest($3::text[], $4::text[]) AS u (s, v)"
	        " JOIN meta_external_id x ON x.kind = $2 AND x.scheme = s AND x.value = v"
	        " WHERE x.title_id <> $1::bigint AND s <> $5"
	        " ON CONFLICT (title_id, provider, scheme) DO UPDATE SET value = EXCLUDED.value,"
	        " held_by = EXCLUDED.held_by, noted_at = now()", params, NULL, error))
		return FALSE;

	if (title->alt_titles != NULL) {
		g_autoptr(GPtrArray) names = g_ptr_array_new();
		g_autoptr(GPtrArray) countries = g_ptr_array_new();
		g_autoptr(GPtrArray) languages = g_ptr_array_new();
		g_autofree gchar *a = NULL;
		g_autofree gchar *b = NULL;
		g_autofree gchar *c = NULL;

		for (i = 0; i < title->alt_titles->len; i++) {
			const AxisMetaAltTitle *alt = g_ptr_array_index(title->alt_titles, i);

			g_ptr_array_add(names, alt->title);
			g_ptr_array_add(countries, (gpointer)(alt->country != NULL ? alt->country : ""));
			g_ptr_array_add(languages, (gpointer)(alt->language != NULL ? alt->language : ""));
		}
		a = axis_meta_sql_text_array(names);
		b = axis_meta_sql_text_array(countries);
		c = axis_meta_sql_text_array(languages);
		g_ptr_array_set_size(params, 0);
		g_ptr_array_add(params, id_text);
		/* The record provider's names replace those stored; another
		 * provider's are added to them. */
		if (record && !exec(conn, "DELETE FROM meta_alt_title WHERE title_id = $1::bigint", params, NULL, error))
			return FALSE;
		g_ptr_array_add(params, a);
		g_ptr_array_add(params, b);
		g_ptr_array_add(params, c);
		if (!exec(conn, "INSERT INTO meta_alt_title (title_id, title, country, language)"
		        " SELECT $1::bigint, t, c, l FROM unnest($2::text[], $3::text[], $4::text[]) AS u (t, c, l)"
		        " ON CONFLICT DO NOTHING", params, NULL, error))
			return FALSE;
	}

	/* What a list's name filter compares: its names as they are now. */
	if (!store_name_keys(conn, id_text, error))
		return FALSE;

	if (title->seasons != NULL && title->seasons->len > 0) {
		g_autoptr(GPtrArray) numbers = g_ptr_array_new_with_free_func(g_free);
		g_autoptr(GPtrArray) names = g_ptr_array_new();
		g_autoptr(GPtrArray) dates = g_ptr_array_new();
		g_autoptr(GPtrArray) overviews = g_ptr_array_new();
		g_autoptr(GPtrArray) counts = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *a = NULL;
		g_autofree gchar *b = NULL;
		g_autofree gchar *c = NULL;
		g_autofree gchar *d = NULL;
		g_autofree gchar *e = NULL;

		for (i = 0; i < title->seasons->len; i++) {
			const AxisMetaSeason *season = g_ptr_array_index(title->seasons, i);

			g_ptr_array_add(numbers, g_strdup_printf("%d", season->number));
			g_ptr_array_add(names, season->name);
			g_ptr_array_add(dates, season->air_date);
			g_ptr_array_add(overviews, season->overview);
			g_ptr_array_add(counts, season->episode_count >= 0 ? g_strdup_printf("%d", season->episode_count) : NULL);
		}
		a = axis_meta_sql_text_array(numbers);
		b = axis_meta_sql_text_array(names);
		c = axis_meta_sql_text_array(dates);
		d = axis_meta_sql_text_array(overviews);
		e = axis_meta_sql_text_array(counts);
		g_ptr_array_set_size(params, 0);
		g_ptr_array_add(params, id_text);
		g_ptr_array_add(params, a);
		g_ptr_array_add(params, b);
		g_ptr_array_add(params, c);
		g_ptr_array_add(params, d);
		g_ptr_array_add(params, e);
		g_ptr_array_add(params, title->provider);
		/* A record's seasons are its provider's own source of numbering. */
		if (!exec(conn, "INSERT INTO meta_season (title_id, source, number, name, air_date, overview, episode_count)"
		        " SELECT $1::bigint, $7, n::integer, nm, d::date, o, c::integer"
		        " FROM unnest($2::text[], $3::text[], $4::text[], $5::text[], $6::text[]) AS u (n, nm, d, o, c)"
		        " ON CONFLICT (title_id, source, number) DO UPDATE SET"
		        " name = coalesce(EXCLUDED.name, meta_season.name),"
		        " air_date = coalesce(EXCLUDED.air_date, meta_season.air_date),"
		        " overview = coalesce(EXCLUDED.overview, meta_season.overview),"
		        " episode_count = coalesce(EXCLUDED.episode_count, meta_season.episode_count),"
		        " fetched_at = now()", params, NULL, error))
			return FALSE;
	}

	*id_out = keep;
	return TRUE;
}

gboolean
axis_meta_store_title(
	AxisDbConn           *conn,
	const AxisMetaTitle  *title,
	gint64               *id,
	GError              **error
){
	return axis_meta_store_title_from(conn, title, FALSE, id, error);
}

gboolean
axis_meta_store_title_from(
	AxisDbConn           *conn,
	const AxisMetaTitle  *title,
	gboolean              secondary,
	gint64               *id,
	GError              **error
){
	gint64 keep = 0;

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!store_title_in_transaction(conn, title, secondary, &keep, error)) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	if (!axis_db_conn_commit(conn, error))
		return FALSE;
	if (id != NULL)
		*id = keep;
	return TRUE;
}

/* ── Reading titles ─────────────────────────────────────────────── */

#define TITLE_COLUMNS \
	"id, kind, title, original_title, year, overview, runtime_minutes, status," \
	" array_to_string(genres, E'\\x1f'), cardinality(genres), original_language, release_date::text," \
	" first_air_date::text, last_air_date::text, poster_ref, backdrop_ref, provider, provider_id, " \
	EPOCH_US("fetched_at") ", " EPOCH_US("detail_fetched_at") ", " EPOCH_US("viewed_at") ", coalesce(regional_year, 0), adult"

static AxisMetaTitle *
title_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisMetaKind kind = AXIS_META_KIND_FILM;
	AxisMetaTitle *title;
	const gchar *genres;

	axis_meta_kind_from_string(axis_db_result_get_text(result, row, 1), &kind);
	title = axis_meta_title_new(kind);
	title->id = result_int(result, row, 0);
	title->title = result_text(result, row, 2);
	title->original_title = result_text(result, row, 3);
	title->year = (gint)result_int(result, row, 4);
	title->overview = result_text(result, row, 5);
	title->runtime = (gint)result_int(result, row, 6);
	title->status = axis_meta_status_from_string(axis_db_result_get_text(result, row, 7));
	title->genres = g_ptr_array_new_with_free_func(g_free);
	genres = axis_db_result_get_text(result, row, 8);
	if (genres != NULL && result_int(result, row, 9) > 0) {
		g_auto(GStrv) parts = g_strsplit(genres, "\x1f", -1);
		guint i;

		for (i = 0; parts[i] != NULL; i++)
			g_ptr_array_add(title->genres, g_strdup(parts[i]));
	}
	title->original_language = result_text(result, row, 10);
	title->release_date = result_text(result, row, 11);
	title->first_air_date = result_text(result, row, 12);
	title->last_air_date = result_text(result, row, 13);
	title->poster_ref = result_text(result, row, 14);
	title->backdrop_ref = result_text(result, row, 15);
	title->provider = result_text(result, row, 16);
	title->provider_id = result_text(result, row, 17);
	title->fetched_us = result_int(result, row, 18);
	title->detail_fetched_us = result_int(result, row, 19);
	title->viewed_us = result_int(result, row, 20);
	title->regional_year = (gint)result_int(result, row, 21);
	/* NULL: no provider's answer has said yet (since migration 0084). */
	title->adult_known = !axis_db_result_is_null(result, row, 22);
	if (title->adult_known)
		axis_db_result_get_boolean(result, row, 22, &title->adult);
	title->detail = title->detail_fetched_us > 0;
	return title;
}

/* Every title of @titles gets its identifiers. */
static gboolean
load_ids(
	AxisDbConn  *conn,
	GPtrArray   *titles,
	GError     **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *array = NULL;
	const gchar *params[1];
	guint i;
	guint j;

	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		g_array_append_val(ids, title->id);
	}
	if (ids->len == 0)
		return TRUE;
	array = axis_meta_sql_int_array(ids);
	params[0] = array;
	if (!axis_db_conn_exec(conn, "SELECT title_id, scheme, value FROM meta_external_id"
	        " WHERE title_id = ANY ($1::bigint[]) ORDER BY title_id, scheme", 1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = result_int(result, i, 0);

		for (j = 0; j < titles->len; j++) {
			AxisMetaTitle *title = g_ptr_array_index(titles, j);

			if (title->id == id) {
				AxisMetaExternalId *external = g_new0(AxisMetaExternalId, 1);

				external->scheme = result_text(result, i, 1);
				external->value = result_text(result, i, 2);
				g_ptr_array_add(title->ids, external);
				break;
			}
		}
	}
	return TRUE;
}

gboolean
axis_meta_store_load_title(
	AxisDbConn     *conn,
	gint64          id,
	gboolean        full,
	AxisMetaTitle **title_out,
	GError        **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) one = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	const gchar *params[1];
	AxisMetaTitle *title;
	guint i;

	*title_out = NULL;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT " TITLE_COLUMNS " FROM meta_title WHERE id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	title = title_from_row(result, 0);
	g_ptr_array_add(one, title);
	if (!load_ids(conn, one, error))
		return FALSE;
	if (full) {
		g_autoptr(AxisDbResult) alts = NULL;
		g_autoptr(AxisDbResult) seasons = NULL;

		if (!axis_db_conn_exec(conn, "SELECT title, nullif(country, ''), nullif(language, '') FROM meta_alt_title"
		        " WHERE title_id = $1::bigint ORDER BY country, language, title", 1, params, &alts, error))
			return FALSE;
		title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
		for (i = 0; i < axis_db_result_get_n_rows(alts); i++) {
			AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

			alt->title = result_text(alts, i, 0);
			alt->country = result_text(alts, i, 1);
			alt->language = result_text(alts, i, 2);
			g_ptr_array_add(title->alt_titles, alt);
		}
		/* The record provider's own seasons. */
		if (!axis_db_conn_exec(conn, "SELECT s.number, s.name, s.air_date::text, s.overview, coalesce(s.episode_count, -1), "
		        EPOCH_US("s.fetched_at") ", " EPOCH_US("s.episodes_fetched_at")
		        " FROM meta_season s JOIN meta_title t ON t.id = s.title_id AND t.provider = s.source"
		        " WHERE s.title_id = $1::bigint ORDER BY s.number", 1, params, &seasons, error))
			return FALSE;
		if (title->kind == AXIS_META_KIND_SERIES)
			title->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
		for (i = 0; title->seasons != NULL && i < axis_db_result_get_n_rows(seasons); i++) {
			AxisMetaSeason *season = axis_meta_season_new((gint)result_int(seasons, i, 0));

			season->name = result_text(seasons, i, 1);
			season->air_date = result_text(seasons, i, 2);
			season->overview = result_text(seasons, i, 3);
			season->episode_count = (gint)result_int(seasons, i, 4);
			season->fetched_us = result_int(seasons, i, 5);
			season->episodes_fetched_us = result_int(seasons, i, 6);
			g_ptr_array_add(title->seasons, season);
		}
	}
	*title_out = g_ptr_array_steal_index(one, 0);
	return TRUE;
}

gboolean
axis_meta_store_load_titles(
	AxisDbConn  *conn,
	GArray      *ids,
	GPtrArray  **titles_out,
	GError     **error
){
	g_autoptr(GPtrArray) loaded = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *array = axis_meta_sql_int_array(ids);
	const gchar *params[1];
	GPtrArray *ordered;
	guint i;
	guint j;

	*titles_out = NULL;
	params[0] = array;
	if (ids != NULL && ids->len > 0) {
		if (!axis_db_conn_exec(conn, "SELECT " TITLE_COLUMNS " FROM meta_title WHERE id = ANY ($1::bigint[])", 1,
		        params, &result, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++)
			g_ptr_array_add(loaded, title_from_row(result, i));
		if (!load_ids(conn, loaded, error))
			return FALSE;
	}
	ordered = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	for (i = 0; ids != NULL && i < ids->len; i++) {
		gint64 id = g_array_index(ids, gint64, i);

		for (j = 0; j < loaded->len; j++) {
			AxisMetaTitle *title = g_ptr_array_index(loaded, j);

			/* Taken once: a merge can leave an id twice in a list. */
			if (title != NULL && title->id == id) {
				g_ptr_array_add(ordered, title);
				loaded->pdata[j] = NULL;
				break;
			}
		}
	}
	*titles_out = ordered;
	return TRUE;
}

gboolean
axis_meta_store_find_id(
	AxisDbConn   *conn,
	AxisMetaKind  kind,
	const gchar  *scheme,
	const gchar  *value,
	gint64       *id,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];

	*id = 0;
	params[0] = axis_meta_kind_to_string(kind);
	params[1] = scheme;
	params[2] = value;
	if (!axis_db_conn_exec(conn, "SELECT title_id FROM meta_external_id WHERE kind = $1 AND scheme = $2 AND value = $3",
	        3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*id = result_int(result, 0, 0);
	return TRUE;
}

/* ── Lookups ────────────────────────────────────────────────────── */

gboolean
axis_meta_store_get_lookup(
	AxisDbConn   *conn,
	const gchar  *provider,
	AxisMetaKind  kind,
	const gchar  *lookup,
	const gchar  *query,
	gint          year,
	gboolean     *found,
	GArray      **ids,
	gint64       *fetched_us,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *year_text = g_strdup_printf("%d", year);
	const gchar *params[5];

	*found = FALSE;
	*ids = NULL;
	*fetched_us = 0;
	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = lookup;
	params[3] = query;
	params[4] = year_text;
	if (!axis_db_conn_exec(conn, "SELECT result_ids::text, " EPOCH_US("fetched_at") " FROM meta_search"
	        " WHERE provider = $1 AND kind = $2 AND lookup = $3 AND query = $4 AND year = $5::integer",
	        5, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		*ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		return TRUE;
	}
	*found = TRUE;
	*ids = parse_int_array(axis_db_result_get_text(result, 0, 0));
	*fetched_us = result_int(result, 0, 1);
	return TRUE;
}

gboolean
axis_meta_store_put_lookup(
	AxisDbConn   *conn,
	const gchar  *provider,
	AxisMetaKind  kind,
	const gchar  *lookup,
	const gchar  *query,
	gint          year,
	GArray       *ids,
	GError      **error
){
	g_autofree gchar *year_text = g_strdup_printf("%d", year);
	g_autofree gchar *array = axis_meta_sql_int_array(ids);
	const gchar *params[6];

	params[0] = provider;
	params[1] = axis_meta_kind_to_string(kind);
	params[2] = lookup;
	params[3] = query;
	params[4] = year_text;
	params[5] = array;
	return axis_db_conn_exec(conn, "INSERT INTO meta_search (provider, kind, lookup, query, year, result_ids)"
		" VALUES ($1, $2, $3, $4, $5::integer, $6::bigint[])"
		" ON CONFLICT (provider, kind, lookup, query, year)"
		" DO UPDATE SET result_ids = EXCLUDED.result_ids, fetched_at = now()", 6, params, NULL, error);
}

/* ── Seasons ────────────────────────────────────────────────────── */

static gboolean
store_season_in_transaction(
	AxisDbConn            *conn,
	gint64                 title_id,
	const gchar           *source,
	const AxisMetaSeason  *season,
	GError               **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autofree gchar *number = g_strdup_printf("%d", season->number);
	g_autofree gchar *count = season->episode_count >= 0 ? g_strdup_printf("%d", season->episode_count) :
		g_strdup_printf("%u", season->episodes->len);
	g_autoptr(GPtrArray) params = g_ptr_array_new();
	g_autoptr(AxisDbResult) owner = NULL;
	g_autoptr(GPtrArray) numbers = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) absolutes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_autoptr(GPtrArray) dates = g_ptr_array_new();
	g_autoptr(GPtrArray) overviews = g_ptr_array_new();
	g_autoptr(GPtrArray) runtimes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) ids = g_ptr_array_new();
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *c = NULL;
	g_autofree gchar *d = NULL;
	g_autofree gchar *e = NULL;
	g_autofree gchar *f = NULL;
	g_autofree gchar *g = NULL;
	guint i;

	g_ptr_array_add(params, id_text);
	if (!exec(conn, "SELECT kind FROM meta_title WHERE id = $1::bigint FOR UPDATE", params, &owner, error))
		return FALSE;
	if (axis_db_result_get_n_rows(owner) == 0 || g_strcmp0(axis_db_result_get_text(owner, 0, 0), "series") != 0) {
		g_set_error_literal(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	g_ptr_array_add(params, number);
	g_ptr_array_add(params, (gpointer)source);
	g_ptr_array_add(params, season->name);
	g_ptr_array_add(params, season->air_date);
	g_ptr_array_add(params, season->overview);
	g_ptr_array_add(params, count);
	if (!exec(conn, "INSERT INTO meta_season (title_id, number, source, name, air_date, overview, episode_count,"
	        " episodes_fetched_at) VALUES ($1::bigint, $2::integer, $3, $4, $5::date, $6, $7::integer, now())"
	        " ON CONFLICT (title_id, source, number) DO UPDATE SET name = coalesce(EXCLUDED.name, meta_season.name),"
	        " air_date = coalesce(EXCLUDED.air_date, meta_season.air_date),"
	        " overview = coalesce(EXCLUDED.overview, meta_season.overview),"
	        " episode_count = EXCLUDED.episode_count, fetched_at = now(), episodes_fetched_at = now(),"
	        " empty_answers = 0, empty_since = NULL",
	        params, NULL, error))
		return FALSE;
	g_ptr_array_set_size(params, 3);
	if (!exec(conn, "DELETE FROM meta_episode WHERE title_id = $1::bigint AND season_number = $2::integer"
	        " AND source = $3", params, NULL, error))
		return FALSE;
	if (season->episodes->len == 0)
		return TRUE;
	for (i = 0; i < season->episodes->len; i++) {
		const AxisMetaEpisode *episode = g_ptr_array_index(season->episodes, i);

		g_ptr_array_add(numbers, g_strdup_printf("%d", episode->number));
		g_ptr_array_add(absolutes, episode->absolute_number > 0 ? g_strdup_printf("%d", episode->absolute_number) :
			NULL);
		g_ptr_array_add(names, episode->name);
		g_ptr_array_add(dates, episode->air_date);
		g_ptr_array_add(overviews, episode->overview);
		g_ptr_array_add(runtimes, episode->runtime > 0 ? g_strdup_printf("%d", episode->runtime) : NULL);
		g_ptr_array_add(ids, axis_meta_episode_id_valid(episode->provider_id) ? episode->provider_id : NULL);
	}
	a = axis_meta_sql_text_array(numbers);
	b = axis_meta_sql_text_array(absolutes);
	c = axis_meta_sql_text_array(names);
	d = axis_meta_sql_text_array(dates);
	e = axis_meta_sql_text_array(overviews);
	f = axis_meta_sql_text_array(runtimes);
	g = axis_meta_sql_text_array(ids);
	g_ptr_array_add(params, a);
	g_ptr_array_add(params, b);
	g_ptr_array_add(params, c);
	g_ptr_array_add(params, d);
	g_ptr_array_add(params, e);
	g_ptr_array_add(params, f);
	g_ptr_array_add(params, g);
	/* The provider's own id of each episode goes with it, so that what
	 * refers to an episode can follow it when it is numbered anew. */
	return exec(conn, "INSERT INTO meta_episode (title_id, season_number, source, number, absolute_number, name,"
		" air_date, overview, runtime_minutes, provider_id) SELECT $1::bigint, $2::integer, $3, n::integer, a::integer,"
		" nm, d::date, o, r::integer, pid FROM unnest($4::text[], $5::text[], $6::text[], $7::text[], $8::text[],"
		" $9::text[], $10::text[]) AS u (n, a, nm, d, o, r, pid)", params, NULL, error);
}

gboolean
axis_meta_store_season(
	AxisDbConn            *conn,
	gint64                 title_id,
	const gchar           *source,
	const AxisMetaSeason  *season,
	GError               **error
){
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	if (!store_season_in_transaction(conn, title_id, source, season, error)) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	return axis_db_conn_commit(conn, error);
}

gboolean
axis_meta_store_order(
	AxisDbConn       *conn,
	gint64            title_id,
	const gchar      *source,
	const GPtrArray  *seasons,
	GError          **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autoptr(GPtrArray) numbers = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *kept = NULL;
	const gchar *params[3];
	guint i;

	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	for (i = 0; i < seasons->len; i++) {
		const AxisMetaSeason *season = g_ptr_array_index(seasons, i);

		g_ptr_array_add(numbers, g_strdup_printf("%d", season->number));
		if (!store_season_in_transaction(conn, title_id, source, season, error)) {
			axis_db_conn_rollback(conn, NULL);
			return FALSE;
		}
	}
	/* The order is the whole answer: a season it no longer has goes (its
	 * episodes with it), and no other source's season is touched. */
	kept = axis_meta_sql_text_array(numbers);
	params[0] = id_text;
	params[1] = source;
	params[2] = kept;
	if (!axis_db_conn_exec(conn, "DELETE FROM meta_season WHERE title_id = $1::bigint AND source = $2"
	        " AND NOT (number = ANY ($3::integer[]))", 3, params, NULL, error)) {
		axis_db_conn_rollback(conn, NULL);
		return FALSE;
	}
	return axis_db_conn_commit(conn, error);
}

gboolean
axis_meta_store_load_season(
	AxisDbConn      *conn,
	gint64           title_id,
	const gchar     *source,
	gint             number,
	AxisMetaSeason **season_out,
	gboolean        *is_current,
	GError         **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autofree gchar *number_text = g_strdup_printf("%d", number);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) episodes = NULL;
	const gchar *params[3];
	AxisMetaSeason *season;
	guint i;

	*season_out = NULL;
	if (is_current != NULL)
		*is_current = FALSE;
	params[0] = id_text;
	params[1] = number_text;
	params[2] = source;
	if (!axis_db_conn_exec(conn, "SELECT name, air_date::text, overview, coalesce(episode_count, -1), "
	        EPOCH_US("fetched_at") ", " EPOCH_US("episodes_fetched_at") ","
	        " number = (SELECT max(number) FROM meta_season WHERE title_id = $1::bigint AND source = $3)"
	        " FROM meta_season WHERE title_id = $1::bigint AND number = $2::integer AND source = $3", 3, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	season = axis_meta_season_new(number);
	season->name = result_text(result, 0, 0);
	season->air_date = result_text(result, 0, 1);
	season->overview = result_text(result, 0, 2);
	season->episode_count = (gint)result_int(result, 0, 3);
	season->fetched_us = result_int(result, 0, 4);
	season->episodes_fetched_us = result_int(result, 0, 5);
	if (is_current != NULL)
		axis_db_result_get_boolean(result, 0, 6, is_current);
	season->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_episode_free);
	if (!axis_db_conn_exec(conn, "SELECT number, coalesce(absolute_number, 0), name, air_date::text, overview,"
	        " coalesce(runtime_minutes, 0), provider_id FROM meta_episode WHERE title_id = $1::bigint"
	        " AND season_number = $2::integer AND source = $3 ORDER BY number", 3, params, &episodes, error)) {
		axis_meta_season_free(season);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(episodes); i++) {
		AxisMetaEpisode *episode = axis_meta_episode_new();

		episode->season_number = number;
		episode->number = (gint)result_int(episodes, i, 0);
		episode->absolute_number = (gint)result_int(episodes, i, 1);
		episode->name = result_text(episodes, i, 2);
		episode->air_date = result_text(episodes, i, 3);
		episode->overview = result_text(episodes, i, 4);
		episode->runtime = (gint)result_int(episodes, i, 5);
		episode->provider_id = result_text(episodes, i, 6);
		g_ptr_array_add(season->episodes, episode);
	}
	*season_out = season;
	return TRUE;
}

gboolean
axis_meta_store_mark_viewed(
	AxisDbConn  *conn,
	gint64       id,
	GError     **error
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];

	params[0] = id_text;
	return axis_db_conn_exec(conn, "UPDATE meta_title SET viewed_at = now() WHERE id = $1::bigint"
		" AND (viewed_at IS NULL OR viewed_at < now() - interval '1 hour')", 1, params, NULL, error);
}

gchar *
axis_meta_store_today(
	AxisDbConn  *conn,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;

	if (!axis_db_conn_exec(conn, "SELECT current_date::text", 0, NULL, &result, error))
		return NULL;
	return result_text(result, 0, 0);
}
