/*
 * axis-indexer-store.c - the indexer layer's tables: sources, their keys and
 * indexers, the searches and their releases
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two tables hold secrets and are read in one place each: a source's key
 * (indexer_source_secret, by axis_indexer_store_secret()) and a release's
 * fetch reference and magnet link (indexer_release_secret, by
 * axis_indexer_store_load_fetch()).  Every listing selects around them: a
 * release has only whether it has a magnet link, as a boolean.  A database
 * error's message can quote a failing row, so what is written to a secrets
 * table is checked first and the message of an error from writing it is
 * replaced by a fixed one.
 */

#include "indexer/axis-indexer-private.h"

#include <stdlib.h>
#include <string.h>

#include "db/axis-db-error.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Reading results ────────────────────────────────────────────── */

static gchar *
text_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	return g_strdup(axis_db_result_get_text(result, row, column));
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
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

/* A PostgreSQL text[] written as @separator-joined text by the query (chr(31)
 * never occurs in cleaned text): a vector, %NULL when empty. */
static gchar **
split_at(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	if (text == NULL || text[0] == '\0')
		return NULL;
	return g_strsplit(text, "\x1f", -1);
}

/* The text form of a PostgreSQL text[] holding @items. */
static gchar *
sql_text_array(const gchar *const *items)
{
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; items != NULL && items[i] != NULL; i++) {
		const gchar *p;

		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, '"');
		for (p = items[i]; *p != '\0'; p++) {
			if (*p == '"' || *p == '\\')
				g_string_append_c(out, '\\');
			g_string_append_c(out, *p);
		}
		g_string_append_c(out, '"');
	}
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* ── Sources ────────────────────────────────────────────────────── */

/* A native indexer's allowances counted from a report more than a day old
 * are of another day: what was used is read as nothing (the most an indexer
 * counts is a day's). */
static const gchar *const source_sql =
	"SELECT s.id, s.name, s.kind, s.address, s.base_path, s.accept_untrusted, s.enabled, (k.source_id IS NOT NULL),"
	" s.state, s.state_message, s.version, " US("s.last_checked_at") ", " US("s.last_contact_at") ","
	" " US("s.indexers_at") ", " US("s.created_at") ", " US("s.updated_at") ","
	" (SELECT count(*) FROM indexer_search x WHERE x.source_id = s.id AND x.expires_at > now()),"
	" s.follow_redirects, s.protocol, s.priority, array_to_string(s.movie_categories, ','),"
	" array_to_string(s.tv_categories, ','), s.files_origin, s.files_origin_seen, " US("s.caps_at") ","
	" s.caps_problem, s.caps_modes, s.caps_movie_ids, s.caps_tv_ids, s.caps_movie_text, s.caps_tv_text,"
	" s.caps_tv_numbers, s.caps_limit_max, s.caps_limit_default, array_to_string(s.caps_categories, chr(31)),"
	" CASE WHEN s.limits_at < now() - interval '1 day' THEN 0 ELSE s.api_used END, s.api_max,"
	" CASE WHEN s.limits_at < now() - interval '1 day' THEN 0 ELSE s.grab_used END, s.grab_max,"
	" " US("s.limits_at") ", " US("s.held_until") ", " US("s.grabs_held_until") ", s.hold_message,"
	" s.definition_id, (SELECT d.name FROM indexer_definition d WHERE d.id = s.definition_id)"
	" FROM indexer_source s LEFT JOIN indexer_source_secret k ON k.source_id = s.id"
	" WHERE ($1::bigint = 0 OR s.id = $1::bigint)"
	" ORDER BY lower(s.name), s.id";

static const gchar *const indexers_sql =
	"SELECT source_id, indexer_id, name, protocol, enabled, failing, movie_ids, tv_ids, redirect, host"
	" FROM indexer_source_indexer WHERE ($1::bigint = 0 OR source_id = $1::bigint)"
	" ORDER BY source_id, lower(name), indexer_id";

/* "2000,2040" into @values (at most %AXIS_INDEXER_MAX_CATEGORIES). */
static guint
read_int_list(
	const gchar *text,
	gint        *values
){
	gchar **parts;
	guint n = 0;
	guint i;

	if (text == NULL || text[0] == '\0')
		return 0;
	parts = g_strsplit(text, ",", -1);
	for (i = 0; parts[i] != NULL && n < AXIS_INDEXER_MAX_CATEGORIES; i++)
		values[n++] = atoi(parts[i]);
	g_strfreev(parts);
	return n;
}

static gint
count_or_none(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = -1;

	if (!axis_db_result_get_int64(result, row, column, &value) || value < 0 || value > G_MAXINT)
		return -1;
	return (gint)value;
}

/* The native indexer's own columns of @source's row. */
static void
read_native(
	AxisDbResult          *result,
	guint                  row,
	AxisIndexerSourceInfo *source
){
	const gchar *protocol = axis_db_result_get_text(result, row, 18);

	source->priority = (gint)int_at(result, row, 19, AXIS_INDEXER_DEFAULT_PRIORITY);
	source->native = protocol != NULL && axis_download_protocol_from_string(protocol, &source->protocol);
	source->n_movie_categories = read_int_list(axis_db_result_get_text(result, row, 20), source->movie_categories);
	source->n_tv_categories = read_int_list(axis_db_result_get_text(result, row, 21), source->tv_categories);
	source->files_origin = text_at(result, row, 22);
	source->files_origin_seen = text_at(result, row, 23);
	source->caps.read_us = int_at(result, row, 24, 0);
	source->caps.problem = text_at(result, row, 25);
	source->caps.modes = (guint)int_at(result, row, 26, 0) & 7;
	source->caps.movie_ids = (guint)int_at(result, row, 27, 0) & 7;
	source->caps.tv_ids = (guint)int_at(result, row, 28, 0) & 7;
	source->caps.movie_text = bool_at(result, row, 29);
	source->caps.tv_text = bool_at(result, row, 30);
	source->caps.tv_numbers = bool_at(result, row, 31);
	source->caps.limit_max = (gint)int_at(result, row, 32, 0);
	source->caps.limit_default = (gint)int_at(result, row, 33, 0);
	source->caps.categories = split_at(result, row, 34);
	source->allowance.api_used = count_or_none(result, row, 35);
	source->allowance.api_max = count_or_none(result, row, 36);
	source->allowance.grab_used = count_or_none(result, row, 37);
	source->allowance.grab_max = count_or_none(result, row, 38);
	source->allowance.reported_us = int_at(result, row, 39, 0);
	source->held_until_us = int_at(result, row, 40, 0);
	source->grabs_held_until_us = int_at(result, row, 41, 0);
	source->hold_message = text_at(result, row, 42);
	if (source->native) {
		AxisHttpOrigin origin;

		if (axis_http_origin_parse(source->address, &origin, NULL)) {
			source->host = axis_indexer_host_key(origin.host);
			axis_http_origin_clear(&origin);
		}
	}
}

gboolean
axis_indexer_store_sources(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **sources,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) indexers = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];
	GPtrArray *out;
	guint n;
	guint i;
	guint m;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, source_sql, 1, params, &result, error) ||
	    !axis_db_conn_exec(conn, indexers_sql, 1, params, &indexers, error))
		return FALSE;
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_source_info_free);
	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		AxisIndexerSourceInfo *source = g_new0(AxisIndexerSourceInfo, 1);

		source->id = int_at(result, i, 0, 0);
		source->name = text_at(result, i, 1);
		source->kind = text_at(result, i, 2);
		source->address = text_at(result, i, 3);
		source->base_path = text_at(result, i, 4);
		if (source->base_path == NULL)
			source->base_path = g_strdup("");
		source->accept_untrusted = bool_at(result, i, 5);
		source->enabled = bool_at(result, i, 6);
		source->key_set = bool_at(result, i, 7);
		if (!axis_indexer_source_state_from_string(axis_db_result_get_text(result, i, 8), &source->state))
			source->state = AXIS_INDEXER_SOURCE_UNCHECKED;
		source->state_message = text_at(result, i, 9);
		source->version = text_at(result, i, 10);
		source->last_checked_us = int_at(result, i, 11, 0);
		source->last_contact_us = int_at(result, i, 12, 0);
		source->indexers_us = int_at(result, i, 13, 0);
		source->created_us = int_at(result, i, 14, 0);
		source->updated_us = int_at(result, i, 15, 0);
		source->n_searches = (guint)int_at(result, i, 16, 0);
		source->follow_redirects = bool_at(result, i, 17);
		read_native(result, i, source);
		source->definition_id = int_at(result, i, 43, 0);
		source->definition_name = text_at(result, i, 44);
		source->plain_http = source->address != NULL && g_str_has_prefix(source->address, "http://");
		source->indexers = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_info_free);
		for (m = 0; m < axis_db_result_get_n_rows(indexers); m++) {
			AxisIndexerInfo *info;

			if (int_at(indexers, m, 0, 0) != source->id)
				continue;
			info = axis_indexer_info_new();
			info->id = int_at(indexers, m, 1, 0);
			info->name = text_at(indexers, m, 2);
			if (!axis_download_protocol_from_string(axis_db_result_get_text(indexers, m, 3), &info->protocol))
				info->protocol = AXIS_DOWNLOAD_USENET;
			info->enabled = bool_at(indexers, m, 4);
			info->failing = bool_at(indexers, m, 5);
			info->movie_ids = (guint)int_at(indexers, m, 6, 0);
			info->tv_ids = (guint)int_at(indexers, m, 7, 0);
			info->redirect = bool_at(indexers, m, 8);
			info->host = text_at(indexers, m, 9);
			g_ptr_array_add(source->indexers, info);
		}
		g_ptr_array_add(out, source);
	}
	*sources = out;
	return TRUE;
}

gboolean
axis_indexer_store_definition_body(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **body,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];

	*body = NULL;
	params[0] = id_text;
	/* A built-in one's override is read with it: which is run is decided
	 * now. */
	if (!axis_db_conn_exec(conn, "SELECT d.def_id, d.body, o.body FROM indexer_definition d"
	        " LEFT JOIN indexer_definition o ON o.def_id = d.def_id || '.override' WHERE d.id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*body = axis_indexer_definition_in_use(axis_db_result_get_text(result, 0, 0),
			axis_db_result_get_text(result, 0, 1), axis_db_result_get_text(result, 0, 2));
	return TRUE;
}

gboolean
axis_indexer_store_secret(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **secret,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];

	*secret = NULL;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT secret FROM indexer_source_secret WHERE source_id = $1::bigint", 1, params,
	        &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*secret = text_at(result, 0, 0);
	return TRUE;
}

gboolean
axis_indexer_store_source_state(
	AxisDbConn              *conn,
	gint64                   id,
	AxisIndexerSourceState   state,
	const gchar             *message,
	const gchar             *version,
	gboolean                 contact,
	GError                 **error
){
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	g_autofree gchar *clean = axis_indexer_text_clean(message, 300);
	g_autofree gchar *version_clean = axis_indexer_text_clean(version, 64);
	const gchar *params[5];

	params[0] = id_text;
	params[1] = axis_indexer_source_state_to_string(state);
	params[2] = clean;
	params[3] = version_clean;
	params[4] = contact ? "true" : "false";
	/* Since when it has not been working: begun by the contact that first
	 * finds it so, kept while it stays so whatever the reason becomes, and
	 * cleared by one that works (the right-hand sides read the row as it
	 * was).  A hold whose time has passed is forgotten by the contact that
	 * works: until then its words stay, and say that the indexer has not
	 * answered properly since it was held (the page "Needs attention" counts
	 * it as held for as long; a hold still running is not touched). */
	return axis_db_conn_exec(conn,
		"UPDATE indexer_source SET state = $2::text, state_message = $3::text,"
		" not_working_since = CASE WHEN $2::text IN ('unreachable', 'refused', 'failing')"
		"  THEN coalesce(not_working_since, now()) END,"
		" hold_message = CASE WHEN $2::text = 'ok' AND held_until IS NOT NULL AND held_until <= now()"
		"  AND (grabs_held_until IS NULL OR grabs_held_until <= now()) THEN NULL ELSE hold_message END,"
		" held_until = CASE WHEN $2::text = 'ok' AND held_until <= now() THEN NULL ELSE held_until END,"
		" version = coalesce($4::text, version), last_checked_at = now(),"
		" last_contact_at = CASE WHEN $5::boolean THEN now() ELSE last_contact_at END"
		" WHERE id = $1::bigint", 5, params, NULL, error);
}

gboolean
axis_indexer_store_lift_hold(
	AxisDbConn  *conn,
	gint64       id,
	gint64       definition_id,
	GError     **error
){
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	g_autofree gchar *definition_text = axis_indexer_int_text(definition_id);
	const gchar *params[4];

	params[0] = id_text;
	params[1] = definition_text;
	params[2] = AXIS_INDEXER_HOLD_DRIFT_WORDS;
	params[3] = AXIS_INDEXER_HOLD_CHALLENGE_WORDS;
	/* Only one that runs a definition, and only Axis's own hold, for what its
	 * answers were (axis_indexer_hold_cause()): a Newznab indexer's hold is
	 * what the indexer said of its own limit, and a site that asked Axis to
	 * wait is waited for. */
	return axis_db_conn_exec(conn,
		"UPDATE indexer_source SET held_until = NULL, hold_message = NULL"
		" WHERE definition_id IS NOT NULL AND held_until IS NOT NULL"
		" AND (right(hold_message, length($3::text)) = $3::text OR right(hold_message, length($4::text)) = $4::text)"
		" AND (($1::bigint > 0 AND id = $1::bigint) OR ($2::bigint > 0 AND definition_id = $2::bigint))", 4, params,
		NULL, error);
}

gboolean
axis_indexer_store_replace_indexers(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray   *indexers,
	GError     **error
){
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	GString *ids = g_string_new("{");
	GString *enabled = g_string_new("{");
	GString *failing = g_string_new("{");
	GString *movie = g_string_new("{");
	GString *tv = g_string_new("{");
	GString *redirect = g_string_new("{");
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) protocols = g_ptr_array_new();
	GString *hosts = g_string_new("{");
	g_autofree gchar *name_array = NULL;
	g_autofree gchar *protocol_array = NULL;
	const gchar *params[10];
	gboolean ok;
	guint i;

	for (i = 0; i < indexers->len && i < AXIS_INDEXER_MAX_INDEXERS; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(indexers, i);
		const gchar *comma = i > 0 ? "," : "";
		g_autofree gchar *host = axis_indexer_host_key(info->host);

		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, comma, info->id);
		g_string_append_printf(enabled, "%s%s", comma, info->enabled ? "true" : "false");
		g_string_append_printf(failing, "%s%s", comma, info->failing ? "true" : "false");
		g_string_append_printf(movie, "%s%u", comma, info->movie_ids & 7);
		g_string_append_printf(tv, "%s%u", comma, info->tv_ids & 7);
		g_string_append_printf(redirect, "%s%s", comma, info->redirect ? "true" : "false");
		/* A host is letters, digits and . - : [ ] (axis_indexer_host_key()):
		 * nothing to quote. */
		if (host != NULL)
			g_string_append_printf(hosts, "%s\"%s\"", comma, host);
		else
			g_string_append_printf(hosts, "%sNULL", comma);
		g_ptr_array_add(names, g_strdup(info->name));
		g_ptr_array_add(protocols, (gpointer)axis_download_protocol_to_string(info->protocol));
	}
	g_string_append_c(hosts, '}');
	g_ptr_array_add(names, NULL);
	g_ptr_array_add(protocols, NULL);
	g_string_append_c(ids, '}');
	g_string_append_c(enabled, '}');
	g_string_append_c(failing, '}');
	g_string_append_c(movie, '}');
	g_string_append_c(tv, '}');
	g_string_append_c(redirect, '}');
	name_array = sql_text_array((const gchar *const *)names->pdata);
	protocol_array = sql_text_array((const gchar *const *)protocols->pdata);

	params[0] = id_text;
	params[1] = ids->str;
	params[2] = name_array;
	params[3] = protocol_array;
	params[4] = enabled->str;
	params[5] = failing->str;
	params[6] = movie->str;
	params[7] = tv->str;
	params[8] = redirect->str;
	params[9] = hosts->str;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "DELETE FROM indexer_source_indexer WHERE source_id = $1::bigint", 1, params, NULL,
			error) &&
		axis_db_conn_exec(conn,
			"INSERT INTO indexer_source_indexer (source_id, indexer_id, name, protocol, enabled, failing, movie_ids,"
			" tv_ids, redirect, host)"
			" SELECT $1::bigint, * FROM unnest($2::bigint[], $3::text[], $4::text[], $5::boolean[], $6::boolean[],"
			" $7::smallint[], $8::smallint[], $9::boolean[], $10::text[])", 10, params, NULL, error) &&
		axis_db_conn_exec(conn, "UPDATE indexer_source SET indexers_at = now() WHERE id = $1::bigint", 1, params, NULL,
			error) &&
		axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	g_string_free(ids, TRUE);
	g_string_free(enabled, TRUE);
	g_string_free(failing, TRUE);
	g_string_free(movie, TRUE);
	g_string_free(tv, TRUE);
	g_string_free(redirect, TRUE);
	g_string_free(hosts, TRUE);
	return ok;
}

/* An optional count as SQL text: %NULL for none. */
static gchar *
count_text(gint value)
{
	return value >= 0 ? g_strdup_printf("%d", value) : NULL;
}

gboolean
axis_indexer_store_learned(
	AxisDbConn               *conn,
	gint64                    id,
	const AxisIndexerLearned *learned,
	GError                  **error
){
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	g_autofree gchar *problem = axis_indexer_text_clean(learned->caps.problem, 300);
	g_autofree gchar *modes = g_strdup_printf("%u", learned->caps.modes & 7);
	g_autofree gchar *movie_ids = g_strdup_printf("%u", learned->caps.movie_ids & 7);
	g_autofree gchar *tv_ids = g_strdup_printf("%u", learned->caps.tv_ids & 7);
	g_autofree gchar *limit_max = learned->caps.limit_max > 0 ? g_strdup_printf("%d", learned->caps.limit_max) : NULL;
	g_autofree gchar *limit_default = learned->caps.limit_default > 0 ?
		g_strdup_printf("%d", learned->caps.limit_default) : NULL;
	g_autofree gchar *categories = NULL;
	g_autofree gchar *api_used = count_text(learned->allowance.api_used);
	g_autofree gchar *api_max = count_text(learned->allowance.api_max);
	g_autofree gchar *grab_used = count_text(learned->allowance.grab_used);
	g_autofree gchar *grab_max = count_text(learned->allowance.grab_max);
	g_autofree gchar *calls = g_strdup_printf("%u", learned->n_calls);
	g_autofree gchar *grabs = g_strdup_printf("%u", learned->n_grabs);
	g_autofree gchar *held = axis_indexer_int_text(learned->held_until_us);
	g_autofree gchar *grabs_held = axis_indexer_int_text(learned->grabs_held_until_us);
	g_autofree gchar *message = axis_indexer_text_clean(learned->hold_message, 300);
	g_autoptr(GPtrArray) kept = g_ptr_array_new();
	const gchar *params[23];
	guint i;

	if (!learned->caps_learned && !learned->allowance_learned && learned->n_calls == 0 && learned->n_grabs == 0 &&
	    learned->other_origin == NULL && learned->held_until_us == 0 && learned->grabs_held_until_us == 0)
		return TRUE;
	for (i = 0; learned->caps.categories != NULL && learned->caps.categories[i] != NULL &&
	     kept->len < AXIS_INDEXER_MAX_CAPS_CATEGORIES; i++)
		g_ptr_array_add(kept, learned->caps.categories[i]);
	g_ptr_array_add(kept, NULL);
	categories = sql_text_array((const gchar *const *)kept->pdata);

	params[0] = id_text;
	params[1] = learned->caps_learned ? "true" : "false";
	params[2] = problem;
	params[3] = modes;
	params[4] = movie_ids;
	params[5] = tv_ids;
	params[6] = learned->caps.movie_text ? "true" : "false";
	params[7] = learned->caps.tv_text ? "true" : "false";
	params[8] = learned->caps.tv_numbers ? "true" : "false";
	params[9] = limit_max;
	params[10] = limit_default;
	params[11] = categories;
	params[12] = learned->allowance_learned ? "true" : "false";
	params[13] = api_used;
	params[14] = api_max;
	params[15] = grab_used;
	params[16] = grab_max;
	params[17] = calls;
	params[18] = grabs;
	params[19] = learned->other_origin;
	params[20] = held;
	params[21] = grabs_held;
	params[22] = message;
	/* Every expression reads the row as it was before this update: a count
	 * since a report a day old starts again from this call's. */
	return axis_db_conn_exec(conn,
		"UPDATE indexer_source SET"
		" caps_at = CASE WHEN $2::boolean THEN now() ELSE caps_at END,"
		" caps_problem = CASE WHEN $2::boolean THEN $3::text ELSE caps_problem END,"
		" caps_modes = CASE WHEN $2::boolean THEN $4::smallint ELSE caps_modes END,"
		" caps_movie_ids = CASE WHEN $2::boolean THEN $5::smallint ELSE caps_movie_ids END,"
		" caps_tv_ids = CASE WHEN $2::boolean THEN $6::smallint ELSE caps_tv_ids END,"
		" caps_movie_text = CASE WHEN $2::boolean THEN $7::boolean ELSE caps_movie_text END,"
		" caps_tv_text = CASE WHEN $2::boolean THEN $8::boolean ELSE caps_tv_text END,"
		" caps_tv_numbers = CASE WHEN $2::boolean THEN $9::boolean ELSE caps_tv_numbers END,"
		" caps_limit_max = CASE WHEN $2::boolean THEN $10::integer ELSE caps_limit_max END,"
		" caps_limit_default = CASE WHEN $2::boolean THEN $11::integer ELSE caps_limit_default END,"
		" caps_categories = CASE WHEN $2::boolean THEN $12::text[] ELSE caps_categories END,"
		" api_used = CASE WHEN $13::boolean THEN $14::integer + $18::integer"
		"   WHEN api_max IS NULL THEN api_used"
		"   WHEN limits_at IS NULL OR limits_at < now() - interval '1 day' THEN $18::integer"
		"   ELSE coalesce(api_used, 0) + $18::integer END,"
		" api_max = CASE WHEN $13::boolean THEN $15::integer ELSE api_max END,"
		" grab_used = CASE WHEN $13::boolean THEN $16::integer + $19::integer"
		"   WHEN grab_max IS NULL THEN grab_used"
		"   WHEN limits_at IS NULL OR limits_at < now() - interval '1 day' THEN $19::integer"
		"   ELSE coalesce(grab_used, 0) + $19::integer END,"
		" grab_max = CASE WHEN $13::boolean THEN $17::integer ELSE grab_max END,"
		" limits_at = CASE WHEN $13::boolean THEN now()"
		"   WHEN (api_max IS NOT NULL OR grab_max IS NOT NULL) AND ($18::integer > 0 OR $19::integer > 0)"
		"    AND (limits_at IS NULL OR limits_at < now() - interval '1 day') THEN now()"
		"   ELSE limits_at END,"
		" files_origin_seen = CASE WHEN $20::text IS NOT NULL AND $20::text IS DISTINCT FROM files_origin"
		"   THEN $20::text ELSE files_origin_seen END,"
		" held_until = CASE WHEN $21::bigint > 0 THEN to_timestamp($21::bigint / 1000000.0) ELSE held_until END,"
		" grabs_held_until = CASE WHEN $22::bigint > 0 THEN to_timestamp($22::bigint / 1000000.0)"
		"   ELSE grabs_held_until END,"
		" hold_message = CASE WHEN $21::bigint > 0 OR $22::bigint > 0 THEN $23::text ELSE hold_message END"
		" WHERE id = $1::bigint AND protocol IS NOT NULL", 23, params, NULL, error);
}

gboolean
axis_indexer_store_accept_files_origin(
	AxisDbConn  *conn,
	gint64       id,
	gboolean    *accepted,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];
	gboolean ok;

	*accepted = FALSE;
	params[0] = id_text;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn,
			"UPDATE indexer_source SET files_origin = files_origin_seen, files_origin_seen = NULL, updated_at = now()"
			" WHERE id = $1::bigint AND protocol IS NOT NULL AND files_origin_seen IS NOT NULL RETURNING id", 1,
			params, &result, error);
	if (ok && axis_db_result_get_n_rows(result) > 0) {
		*accepted = TRUE;
		/* Its stored searches left those results out. */
		ok = axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE source_id = $1::bigint", 1, params, NULL,
			error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	return ok;
}

gboolean
axis_indexer_store_alternatives(
	AxisDbConn  *conn,
	gint64       release_id,
	guint        max,
	GArray     **ids,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(release_id);
	g_autofree gchar *max_text = g_strdup_printf("%u", max);
	const gchar *params[2];
	guint i;

	*ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	params[0] = id_text;
	params[1] = max_text;
	if (!axis_db_conn_exec(conn,
	        "SELECT a.id FROM ("
	        " SELECT DISTINCT ON (s2.source_id, r2.indexer_id) r2.id, src.priority, s2.source_id, r2.indexer_id"
	        " FROM indexer_release r"
	        " JOIN indexer_search s ON s.id = r.search_id"
	        " JOIN indexer_release r2 ON r2.group_key = r.group_key AND r2.protocol = r.protocol AND r2.id <> r.id"
	        " JOIN indexer_search s2 ON s2.id = r2.search_id"
	        " JOIN indexer_source src ON src.id = s2.source_id"
	        " WHERE r.id = $1::bigint AND r.group_key IS NOT NULL AND s2.expires_at > now() AND src.enabled"
	        " AND (src.grabs_held_until IS NULL OR src.grabs_held_until <= now())"
	        " AND NOT (s2.source_id = s.source_id AND r2.indexer_id = r.indexer_id)"
	        " ORDER BY s2.source_id, r2.indexer_id, s2.created_at DESC, r2.id"
	        ") a ORDER BY a.priority, a.source_id, a.indexer_id LIMIT $2::integer", 2, params, &result, error)) {
		g_clear_pointer(ids, g_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = int_at(result, i, 0, 0);

		g_array_append_val(*ids, id);
	}
	return TRUE;
}

/* ── Releases ───────────────────────────────────────────────────── */

/* Never the secrets: only whether a magnet link is stored. */
static const gchar *const release_columns =
	"SELECT r.id, r.name, coalesce(r.size_bytes, -1), r.protocol, r.indexer_id, r.indexer_name,"
	" coalesce(" US("r.published_at") ", 0), coalesce(r.seeders, -1), coalesce(r.leechers, -1),"
	" coalesce(r.grabs, -1), array_to_string(r.categories, ','), array_to_string(r.flags, chr(31)),"
	" r.imdb_id, coalesce(r.tmdb_id, 0), coalesce(r.tvdb_id, 0), r.info_hash,"
	" EXISTS (SELECT 1 FROM indexer_release_secret k WHERE k.release_id = r.id AND k.magnet IS NOT NULL),"
	" (SELECT x.source_id FROM indexer_search x WHERE x.id = r.search_id)"
	" FROM indexer_release r";

static AxisIndexerRelease *
read_release(
	AxisDbResult *result,
	guint         row
){
	AxisIndexerRelease *release = axis_indexer_release_new();
	const gchar *categories = axis_db_result_get_text(result, row, 10);

	release->id = int_at(result, row, 0, 0);
	release->name = text_at(result, row, 1);
	release->size_bytes = int_at(result, row, 2, -1);
	if (!axis_download_protocol_from_string(axis_db_result_get_text(result, row, 3), &release->protocol))
		release->protocol = AXIS_DOWNLOAD_USENET;
	release->indexer_id = int_at(result, row, 4, 0);
	release->indexer_name = text_at(result, row, 5);
	release->published_us = int_at(result, row, 6, 0);
	release->seeders = (gint)int_at(result, row, 7, -1);
	release->leechers = (gint)int_at(result, row, 8, -1);
	release->grabs = (gint)int_at(result, row, 9, -1);
	if (categories != NULL && categories[0] != '\0') {
		gchar **parts = g_strsplit(categories, ",", -1);
		guint i;

		for (i = 0; parts[i] != NULL && release->n_categories < AXIS_INDEXER_MAX_CATEGORIES; i++)
			release->categories[release->n_categories++] = atoi(parts[i]);
		g_strfreev(parts);
	}
	release->flags = split_at(result, row, 11);
	release->imdb_id = text_at(result, row, 12);
	release->tmdb_id = int_at(result, row, 13, 0);
	release->tvdb_id = int_at(result, row, 14, 0);
	release->info_hash = text_at(result, row, 15);
	release->has_magnet = bool_at(result, row, 16);
	release->source_id = int_at(result, row, 17, 0);
	return release;
}

gboolean
axis_indexer_store_get_release(
	AxisDbConn           *conn,
	gint64                release_id,
	AxisIndexerRelease  **release,
	GError              **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = g_strdup_printf("%s JOIN indexer_search s ON s.id = r.search_id"
		" WHERE r.id = $1::bigint AND s.expires_at > now()", release_columns);
	g_autofree gchar *id_text = axis_indexer_int_text(release_id);
	const gchar *params[1];

	*release = NULL;
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, sql, 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*release = read_release(result, 0);
	return TRUE;
}

gboolean
axis_indexer_store_get_search(
	AxisDbConn               *conn,
	gint64                    source_id,
	const gchar              *key,
	AxisIndexerSearchResult **found,
	GError                  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *source_text = axis_indexer_int_text(source_id);
	g_autofree gchar *sql = g_strdup_printf("%s WHERE r.search_id = $1::bigint ORDER BY r.position", release_columns);
	g_autofree gchar *search_text = NULL;
	AxisIndexerSearchResult *search;
	const gchar *params[2];
	guint i;

	*found = NULL;
	params[0] = source_text;
	params[1] = key;
	if (!axis_db_conn_exec(conn,
	        "SELECT id, mode, fell_back, n_off_origin, n_unusable, n_skipped, truncated, coalesce(n_indexers, -1),"
	        " array_to_string(failed_names, chr(31)), " US("created_at") ", " US("expires_at") ", also_text"
	        " FROM indexer_search WHERE source_id = $1::bigint AND request_key = $2::text AND expires_at > now()",
	        2, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	search = g_new0(AxisIndexerSearchResult, 1);
	search->search_id = int_at(result, 0, 0, 0);
	search->source_id = source_id;
	if (!axis_indexer_mode_from_string(axis_db_result_get_text(result, 0, 1), &search->mode))
		search->mode = AXIS_INDEXER_BY_TEXT;
	search->fell_back = bool_at(result, 0, 2);
	search->n_off_origin = (guint)int_at(result, 0, 3, 0);
	search->n_unusable = (guint)int_at(result, 0, 4, 0);
	search->n_skipped = (guint)int_at(result, 0, 5, 0);
	search->truncated = bool_at(result, 0, 6);
	search->n_indexers = (gint)int_at(result, 0, 7, -1);
	search->failed = split_at(result, 0, 8);
	search->created_us = int_at(result, 0, 9, 0);
	search->expires_us = int_at(result, 0, 10, 0);
	search->also_text = bool_at(result, 0, 11);
	search->cached = TRUE;
	search->releases = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_release_free);
	search_text = axis_indexer_int_text(search->search_id);
	params[0] = search_text;
	if (!axis_db_conn_exec(conn, sql, 1, params, &rows, error)) {
		axis_indexer_search_result_free(search);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(search->releases, read_release(rows, i));
	*found = search;
	return TRUE;
}

/* "{1,2}" for a bigint[] or integer[]. */
static gchar *
int_array(
	const gint *values,
	guint       n
){
	GString *out = g_string_new("{");
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(out, "%s%d", i > 0 ? "," : "", values[i]);
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

/* The fixed message for a failure of writing a secret: the database's own
 * can quote the row. */
static void
secret_failure(
	GError **local,
	GError **error
){
	gboolean lost = g_error_matches(*local, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);

	g_set_error_literal(error, AXIS_DB_ERROR, lost ? AXIS_DB_ERROR_CONNECTION_LOST : AXIS_DB_ERROR_QUERY,
		"the database could not store a release's fetch reference");
	g_clear_error(local);
}

static gboolean
put_release(
	AxisDbConn             *conn,
	gint64                  search_id,
	guint                   position,
	const AxisIndexerFound *found,
	gint64                 *release_id,
	GError                **error
){
	const AxisIndexerRelease *r = found->release;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *search_text = axis_indexer_int_text(search_id);
	g_autofree gchar *position_text = g_strdup_printf("%u", position);
	g_autofree gchar *size = r->size_bytes >= 0 ? axis_indexer_int_text(r->size_bytes) : NULL;
	g_autofree gchar *indexer_id = axis_indexer_int_text(r->indexer_id);
	g_autofree gchar *published = axis_indexer_int_text(r->published_us);
	g_autofree gchar *seeders = r->seeders >= 0 ? g_strdup_printf("%d", r->seeders) : NULL;
	g_autofree gchar *leechers = r->leechers >= 0 ? g_strdup_printf("%d", r->leechers) : NULL;
	g_autofree gchar *grabs = r->grabs >= 0 ? g_strdup_printf("%d", r->grabs) : NULL;
	g_autofree gchar *categories = int_array(r->categories, r->n_categories);
	g_autofree gchar *flags = sql_text_array((const gchar *const *)r->flags);
	g_autofree gchar *tmdb = r->tmdb_id > 0 ? axis_indexer_int_text(r->tmdb_id) : NULL;
	g_autofree gchar *tvdb = r->tvdb_id > 0 ? axis_indexer_int_text(r->tvdb_id) : NULL;
	g_autofree gchar *group = axis_indexer_release_group_key(r);
	const gchar *params[18];
	GError *local = NULL;

	params[0] = search_text;
	params[1] = position_text;
	params[2] = r->name;
	params[3] = size;
	params[4] = axis_download_protocol_to_string(r->protocol);
	params[5] = indexer_id;
	params[6] = r->indexer_name;
	params[7] = published;
	params[8] = seeders;
	params[9] = leechers;
	params[10] = grabs;
	params[11] = categories;
	params[12] = flags;
	params[13] = r->imdb_id;
	params[14] = tmdb;
	params[15] = tvdb;
	params[16] = r->info_hash;
	params[17] = group;
	if (!axis_db_conn_exec(conn,
	        "INSERT INTO indexer_release (search_id, position, name, size_bytes, protocol, indexer_id, indexer_name,"
	        " published_at, seeders, leechers, grabs, categories, flags, imdb_id, tmdb_id, tvdb_id, info_hash,"
	        " group_key)"
	        " VALUES ($1::bigint, $2::integer, $3::text, $4::bigint, $5::text, $6::bigint, $7::text,"
	        " CASE WHEN $8::bigint > 0 THEN to_timestamp($8::bigint / 1000000.0) END, $9::integer, $10::integer,"
	        " $11::integer, $12::integer[], $13::text[], $14::text, $15::bigint, $16::bigint, $17::text, $18::text)"
	        " RETURNING id", 18, params, &result, error))
		return FALSE;
	*release_id = int_at(result, 0, 0, 0);
	if (found->fetch_path != NULL || found->magnet != NULL) {
		g_autofree gchar *release_text = axis_indexer_int_text(*release_id);
		const gchar *secret_params[4];

		secret_params[0] = release_text;
		secret_params[1] = found->fetch_path;
		secret_params[2] = found->magnet;
		secret_params[3] = found->files_origin && found->fetch_path != NULL ? "true" : "false";
		if (!axis_db_conn_exec(conn,
		        "INSERT INTO indexer_release_secret (release_id, fetch_ref, magnet, files_origin)"
		        " VALUES ($1::bigint, $2::text, $3::text, $4::boolean)", 4, secret_params, NULL, &local)) {
			secret_failure(&local, error);
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
axis_indexer_store_put_search(
	AxisDbConn                *conn,
	gint64                     source_id,
	const gchar               *key,
	const AxisIndexerOutcome  *outcome,
	gint                       n_indexers,
	GPtrArray                 *failed,
	gint64                     life_s,
	AxisIndexerSearchResult  **stored,
	GError                   **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *source_text = axis_indexer_int_text(source_id);
	g_autofree gchar *n_text = g_strdup_printf("%u", outcome->items->len);
	g_autofree gchar *off_text = g_strdup_printf("%u", outcome->n_off_origin);
	g_autofree gchar *unusable_text = g_strdup_printf("%u", outcome->n_unusable);
	g_autofree gchar *skipped_text = g_strdup_printf("%u", outcome->n_skipped);
	g_autofree gchar *indexers_text = n_indexers >= 0 ? g_strdup_printf("%d", n_indexers) : NULL;
	g_autofree gchar *life_text = axis_indexer_int_text(life_s);
	g_autofree gchar *failed_text = NULL;
	AxisIndexerSearchResult *search = NULL;
	GPtrArray *names = g_ptr_array_new();
	const gchar *params[13];
	gboolean ok;
	guint i;

	for (i = 0; failed != NULL && i < failed->len && i < AXIS_INDEXER_MAX_FAILED; i++)
		g_ptr_array_add(names, g_ptr_array_index(failed, i));
	g_ptr_array_add(names, NULL);
	failed_text = sql_text_array((const gchar *const *)names->pdata);
	g_ptr_array_unref(names);

	*stored = NULL;
	params[0] = source_text;
	params[1] = key;
	params[2] = axis_indexer_mode_to_string(outcome->mode);
	params[3] = outcome->fell_back ? "true" : "false";
	params[4] = n_text;
	params[5] = off_text;
	params[6] = unusable_text;
	params[7] = skipped_text;
	params[8] = outcome->truncated ? "true" : "false";
	params[9] = indexers_text;
	params[10] = failed_text;
	params[11] = life_text;
	params[12] = outcome->also_text ? "true" : "false";
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE source_id = $1::bigint AND request_key = $2::text",
			2, params, NULL, error) &&
		axis_db_conn_exec(conn,
			"INSERT INTO indexer_search (source_id, request_key, mode, fell_back, n_releases, n_off_origin,"
			" n_unusable, n_skipped, truncated, n_indexers, failed_names, expires_at, also_text)"
			" VALUES ($1::bigint, $2::text, $3::text, $4::boolean, $5::integer, $6::integer, $7::integer,"
			" $8::integer, $9::boolean, $10::integer, $11::text[], now() + ($12::bigint * interval '1 second'),"
			" $13::boolean)"
			" RETURNING id, " US("created_at") ", " US("expires_at"), 13, params, &result, error);
	if (ok) {
		search = g_new0(AxisIndexerSearchResult, 1);
		search->search_id = int_at(result, 0, 0, 0);
		search->source_id = source_id;
		search->mode = outcome->mode;
		search->fell_back = outcome->fell_back;
		search->also_text = outcome->also_text;
		search->truncated = outcome->truncated;
		search->n_off_origin = outcome->n_off_origin;
		search->n_unusable = outcome->n_unusable;
		search->n_skipped = outcome->n_skipped;
		search->n_indexers = n_indexers;
		search->created_us = int_at(result, 0, 1, 0);
		search->expires_us = int_at(result, 0, 2, 0);
		search->releases = g_ptr_array_new_with_free_func((GDestroyNotify)axis_indexer_release_free);
		if (failed != NULL && failed->len > 0) {
			search->failed = g_new0(gchar *, MIN(failed->len, (guint)AXIS_INDEXER_MAX_FAILED) + 1);
			for (i = 0; i < failed->len && i < AXIS_INDEXER_MAX_FAILED; i++)
				search->failed[i] = g_strdup(g_ptr_array_index(failed, i));
		}
	}
	for (i = 0; ok && i < outcome->items->len; i++) {
		const AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);
		AxisIndexerRelease *copy;
		gint64 release_id = 0;

		ok = put_release(conn, search->search_id, i, found, &release_id, error);
		if (!ok)
			break;
		/* The release as the caller sees it: the same, with its identifier
		 * and no secret. */
		copy = axis_indexer_release_copy(found->release);
		copy->id = release_id;
		copy->source_id = source_id;
		g_ptr_array_add(search->releases, copy);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	if (!ok) {
		axis_db_conn_rollback(conn, NULL);
		axis_indexer_search_result_free(search);
		return FALSE;
	}
	*stored = search;
	return TRUE;
}

gboolean
axis_indexer_store_load_fetch(
	AxisDbConn              *conn,
	gint64                   release_id,
	gboolean                *found,
	AxisIndexerFetchTarget  *target,
	GError                 **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(release_id);
	const gchar *params[1];

	*found = FALSE;
	memset(target, 0, sizeof(*target));
	params[0] = id_text;
	if (!axis_db_conn_exec(conn,
	        "SELECT s.source_id, r.protocol, r.name, r.info_hash, k.fetch_ref, k.magnet, k.files_origin"
	        " FROM indexer_release r JOIN indexer_search s ON s.id = r.search_id"
	        " JOIN indexer_release_secret k ON k.release_id = r.id"
	        " WHERE r.id = $1::bigint AND s.expires_at > now()", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	target->source_id = int_at(result, 0, 0, 0);
	if (!axis_download_protocol_from_string(axis_db_result_get_text(result, 0, 1), &target->protocol))
		target->protocol = AXIS_DOWNLOAD_USENET;
	target->name = text_at(result, 0, 2);
	target->info_hash = text_at(result, 0, 3);
	target->fetch_ref = text_at(result, 0, 4);
	target->magnet = text_at(result, 0, 5);
	target->files_origin = bool_at(result, 0, 6);
	*found = TRUE;
	return TRUE;
}

gboolean
axis_indexer_store_expire(
	AxisDbConn  *conn,
	guint       *n_searches,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;

	if (!axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE expires_at <= now()", 0, NULL, &result, error))
		return FALSE;
	if (n_searches != NULL)
		*n_searches = (guint)axis_db_result_get_affected_rows(result);
	return TRUE;
}

/* ── Changing sources ───────────────────────────────────────────── */

static gboolean
invalid(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID, message);
	return FALSE;
}

/* A key goes into a header: any text, but no control character and not too
 * long. */
static gboolean
secret_usable(const gchar *secret)
{
	const gchar *p;

	if (strlen(secret) > 1024 || !g_utf8_validate(secret, -1, NULL))
		return FALSE;
	for (p = secret; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			return FALSE;
	}
	return TRUE;
}

/* Categories as typed ("2000, 2040"), as an integer[] literal: empty is
 * @fallback alone.  %FALSE for anything but up to 16 numbers. */
static gboolean
categories_text(
	const gchar  *typed,
	gint          fallback,
	gchar       **out
){
	g_auto(GStrv) words = NULL;
	GString *list = g_string_new("{");
	gint seen[AXIS_INDEXER_MAX_CATEGORIES];
	guint n = 0;
	guint i;

	*out = NULL;
	if (typed != NULL && strlen(typed) > 200) {
		g_string_free(list, TRUE);
		return FALSE;
	}
	words = g_strsplit_set(typed != NULL ? typed : "", ", \t", -1);
	for (i = 0; words[i] != NULL; i++) {
		gint64 value = 0;
		gchar *end = NULL;
		guint j;
		gboolean again = FALSE;

		if (words[i][0] == '\0')
			continue;
		if (!g_ascii_isdigit(words[i][0]))
			goto bad;
		value = g_ascii_strtoll(words[i], &end, 10);
		if (end == NULL || *end != '\0' || value < 1 || value > 999999 || n >= AXIS_INDEXER_MAX_CATEGORIES)
			goto bad;
		for (j = 0; j < n; j++)
			again = again || seen[j] == (gint)value;
		if (again)
			continue;
		seen[n++] = (gint)value;
		g_string_append_printf(list, "%s%d", n > 1 ? "," : "", (gint)value);
	}
	if (n == 0)
		g_string_append_printf(list, "%d", fallback);
	g_string_append_c(list, '}');
	*out = g_string_free(list, FALSE);
	return TRUE;
bad:
	g_string_free(list, TRUE);
	return FALSE;
}

/* @error, with @secret blanked out of its message. */
static void
scrub_error(
	GError      *error,
	const gchar *secret
){
	gchar **parts;
	gchar *joined;

	if (error == NULL || secret == NULL || secret[0] == '\0' || strstr(error->message, secret) == NULL)
		return;
	parts = g_strsplit(error->message, secret, -1);
	joined = g_strjoinv("***", parts);
	g_strfreev(parts);
	g_free(error->message);
	error->message = joined;
}

gboolean
axis_indexers_list_sources(
	AxisIndexers  *self,
	GPtrArray    **sources,
	GError       **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_sources(conn, 0, sources, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_indexers_get_source(
	AxisIndexers            *self,
	gint64                   id,
	AxisIndexerSourceInfo  **source,
	GError                 **error
){
	g_autoptr(GPtrArray) sources = NULL;
	AxisDbConn *conn;
	gboolean ok;

	*source = NULL;
	if (id <= 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such indexer");
		return FALSE;
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_indexer_store_sources(conn, id, &sources, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (sources->len == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such indexer");
		return FALSE;
	}
	*source = g_ptr_array_steal_index(sources, 0);
	return TRUE;
}

/* The installed definition @id, which must be usable now: its name and its
 * first link. */
static gboolean
definition_for(
	AxisIndexers  *self,
	gint64         id,
	gchar        **name,
	gchar        **link,
	GError       **error
){
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;
	g_autoptr(GError) local = NULL;

	if (id <= 0)
		return invalid(error, "Choose the definition this indexer runs.");
	if (!axis_indexers_get_definition(self, id, &info, &local)) {
		if (g_error_matches(local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
			return invalid(error, "That definition is not installed.");
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	if (!info->usable) {
		g_autofree gchar *text = g_strdup_printf("That definition is not usable yet: %s", info->problem);

		return invalid(error, text);
	}
	*name = g_strdup(info->name);
	*link = g_strdup(info->links[0]);
	return TRUE;
}

/* The kind of the stored source @id (%NULL when this daemon has none of
 * that name); a source that is not there is left to the update to say. */
static gboolean
kind_of_source(
	AxisIndexers               *self,
	gint64                      id,
	const AxisIndexerKindInfo **kind,
	GError                    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];

	*kind = NULL;
	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, "SELECT kind FROM indexer_source WHERE id = $1::bigint", 1, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) > 0)
		*kind = axis_indexers_find_kind(self, axis_db_result_get_text(result, 0, 0));
	return TRUE;
}

/*
 * An address that may have a path ("https://example.org/forum/") as its
 * origin ("https://example.org") and the path normalized as a base path
 * ("/forum", "" for none).
 */
static gboolean
split_address(
	const gchar  *address,
	gchar       **origin,
	gchar       **path
){
	g_autoptr(GUri) uri = NULL;
	const gchar *base_problem = NULL;
	const gchar *scheme;

	*origin = NULL;
	*path = NULL;
	if (address == NULL || address[0] == '\0' || strlen(address) > 512)
		return FALSE;
	uri = g_uri_parse(address, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_host(uri) == NULL || g_uri_get_host(uri)[0] == '\0' || g_uri_get_query(uri) != NULL ||
	    g_uri_get_fragment(uri) != NULL || g_uri_get_userinfo(uri) != NULL)
		return FALSE;
	scheme = g_uri_get_scheme(uri);
	if (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0)
		return FALSE;
	if (!axis_download_base_path_normalize(g_uri_get_path(uri), path, &base_problem))
		return FALSE;
	if (strchr(g_uri_get_host(uri), ':') != NULL)
		*origin = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://[%s]:%d", scheme, g_uri_get_host(uri),
			g_uri_get_port(uri)) : g_strdup_printf("%s://[%s]", scheme, g_uri_get_host(uri));
	else
		*origin = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", scheme, g_uri_get_host(uri),
			g_uri_get_port(uri)) : g_strdup_printf("%s://%s", scheme, g_uri_get_host(uri));
	return TRUE;
}

gboolean
axis_indexers_save_source(
	AxisIndexers                 *self,
	gint64                        id,
	const AxisIndexerSourceSpec  *spec,
	gint64                       *id_out,
	GError                      **error
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *address = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *base_path = NULL;
	g_autofree gchar *id_text = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	AxisHttpOrigin origin;
	GError *local = NULL;
	const gchar *base_problem = NULL;
	const gchar *secret = spec->secret != NULL && spec->secret[0] != '\0' ? spec->secret : NULL;
	const gchar *follow = spec->refuse_redirects ? "false" : "true";
	const gchar *params[13];
	g_autofree gchar *definition_name = NULL;
	g_autofree gchar *definition_link = NULL;
	g_autofree gchar *definition_text = NULL;
	g_autofree gchar *site_path = NULL;
	g_autofree gchar *priority = NULL;
	g_autofree gchar *movie_categories = NULL;
	g_autofree gchar *tv_categories = NULL;
	g_autofree gchar *files_origin = NULL;
	g_autofree gchar *typed_origin = NULL;
	const AxisIndexerKindInfo *kind = NULL;
	gboolean changed = FALSE;
	gboolean refiltered = FALSE;
	AxisDbConn *conn;
	gboolean ok;

	name = axis_indexer_text_clean(spec->name, 64);
	/* An indexer that runs a definition: the definition, which must be
	 * usable when it is added; its name and first link are the defaults. */
	if (id == 0 && (kind = axis_indexers_find_kind(self, spec->kind)) != NULL && kind->definition) {
		if (!definition_for(self, spec->definition_id, &definition_name, &definition_link, error))
			return FALSE;
		definition_text = axis_indexer_int_text(spec->definition_id);
		if (name == NULL)
			name = axis_indexer_text_clean(definition_name, 64);
	} else if (id != 0 && !kind_of_source(self, id, &kind, error)) {
		return FALSE;
	}
	/* A public site added with no name typed is called what the site is. */
	if (name == NULL && id == 0) {
		const AxisIndexerKindInfo *named = axis_indexers_find_kind(self, spec->kind);

		if (named != NULL && named->site)
			name = g_strdup(named->label);
	}
	if (name == NULL || (spec->name != NULL && g_utf8_strlen(spec->name, -1) > 64))
		return invalid(error, "An indexer's name is 1 to 64 characters.");
	if (id == 0 && (kind = axis_indexers_find_kind(self, spec->kind)) == NULL)
		return invalid(error, "Choose one of the kinds of indexer this version of Axis has.");
	/* A public site takes no key: none is kept, whatever was sent. */
	if (kind != NULL && kind->site)
		secret = NULL;
	if (spec->priority != 0 && (spec->priority < 1 || spec->priority > AXIS_INDEXER_MAX_PRIORITY))
		return invalid(error, "The priority is a whole number from 1 to 50 (lower is asked first).");
	priority = g_strdup_printf("%d", spec->priority != 0 ? spec->priority : AXIS_INDEXER_DEFAULT_PRIORITY);
	if (!categories_text(spec->movie_categories, 2000, &movie_categories) ||
	    !categories_text(spec->tv_categories, 5000, &tv_categories))
		return invalid(error, "The categories are Newznab category numbers (such as 2000 for movies and 5000 for "
			"TV), at most 16, separated by commas or spaces. Leave them empty for those two.");
	typed_origin = g_strstrip(g_strdup(spec->files_origin != NULL ? spec->files_origin : ""));
	if (typed_origin[0] != '\0') {
		AxisHttpOrigin other;
		g_autofree gchar *why = NULL;

		if (!axis_http_origin_parse(typed_origin, &other, &why)) {
			g_autofree gchar *text = g_strdup_printf("The address the indexer's files come from %s. It is an http or "
				"https address with its host and port only, such as https://files.example.",
				why != NULL ? why : "is not valid");

			return invalid(error, text);
		}
		files_origin = g_strdup(other.base);
		axis_http_origin_clear(&other);
	}
	address = g_strstrip(g_strdup(spec->address != NULL ? spec->address : ""));
	/* A site's kind has the address it was known at: used when none is typed. */
	if (address[0] == '\0' && kind != NULL && kind->default_address != NULL) {
		g_free(address);
		address = g_strdup(kind->default_address);
	}
	/* An indexer that runs a definition is at one of its links (the first
	 * when none is typed) or an address typed, which may have a path: that
	 * is its base path. */
	if (kind != NULL && kind->definition) {
		g_autofree gchar *origin_text = NULL;
		g_autofree gchar *path = NULL;

		if (address[0] == '\0' && definition_link != NULL) {
			g_free(address);
			address = g_strdup(definition_link);
		}
		if (!split_address(address, &origin_text, &path)) {
			return invalid(error, "The address is not one Axis can use: an http or https address, such as "
				"https://example.org or https://example.org/forum, without a query.");
		}
		g_free(address);
		address = g_steal_pointer(&origin_text);
		site_path = g_steal_pointer(&path);
	}
	if (!axis_http_origin_parse(address, &origin, &problem)) {
		g_autofree gchar *text = g_strdup_printf("The address %s. It is the indexer's own address, such as "
			"http://prowlarr:9696, with nothing after the port (a path goes in the base path).",
			problem != NULL ? problem : "is not valid");

		return invalid(error, text);
	}
	if (!axis_download_base_path_normalize(spec->base_path, &base_path, &base_problem)) {
		g_autofree gchar *text = g_strdup_printf("The base path %s. It is the path the indexer is served under, such "
			"as /prowlarr: a slash, then names of letters, digits, '.', '_', '~' and '-' separated by slashes, with "
			"no '..', no query and no spaces. Leave it empty when the indexer is at the top of its address.",
			base_problem != NULL ? base_problem : "is not valid");

		axis_http_origin_clear(&origin);
		return invalid(error, text);
	}
	if (secret != NULL && !secret_usable(secret)) {
		axis_http_origin_clear(&origin);
		return invalid(error, "The key has a character it cannot be sent with, or is longer than 1024 bytes.");
	}
	/* A public site is at its address, not under a path; one that runs a
	 * definition is under its address's. */
	if (kind != NULL && kind->site) {
		g_free(base_path);
		base_path = g_strdup(site_path != NULL ? site_path : "");
	}

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		axis_http_origin_clear(&origin);
		return FALSE;
	}
	params[0] = name;
	params[1] = spec->kind;
	params[2] = origin.base;
	params[3] = base_path;
	params[4] = spec->accept_untrusted ? "true" : "false";
	params[5] = spec->enabled ? "true" : "false";
	params[6] = follow;
	params[7] = kind != NULL && kind->native ? axis_download_protocol_to_string(kind->protocol) : NULL;
	params[8] = priority;
	params[9] = movie_categories;
	params[10] = tv_categories;
	params[11] = kind != NULL && kind->native && !kind->site ? files_origin : NULL;
	ok = axis_db_conn_begin(conn, &local);
	if (ok && id == 0) {
		g_autoptr(AxisDbResult) counted = NULL;

		ok = axis_db_conn_exec(conn, "SELECT count(*) FROM indexer_source", 0, NULL, &counted, &local);
		if (ok && int_at(counted, 0, 0, 0) >= AXIS_INDEXER_MAX_SOURCES) {
			ok = FALSE;
			g_set_error(&local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
				"At most %d indexers can be configured.", AXIS_INDEXER_MAX_SOURCES);
		}
		/* A native indexer never follows a download off its origin. */
		if (kind->native)
			params[6] = "false";
		params[12] = definition_text;
		ok = ok && axis_db_conn_exec(conn,
			"INSERT INTO indexer_source (name, kind, address, base_path, accept_untrusted, enabled, follow_redirects,"
			" protocol, priority, movie_categories, tv_categories, files_origin, definition_id)"
			" VALUES ($1::text, $2::text, $3::text, $4::text, $5::boolean, $6::boolean, $7::boolean, $8::text,"
			" $9::integer, $10::integer[], $11::integer[], $12::text, $13::bigint) RETURNING id", 13,
			params, &result, &local);
		if (ok)
			id = int_at(result, 0, 0, 0);
	} else if (ok) {
		g_autoptr(AxisDbResult) old = NULL;
		const gchar *old_params[1];

		id_text = axis_indexer_int_text(id);
		old_params[0] = id_text;
		ok = axis_db_conn_exec(conn, "SELECT address, base_path, protocol IS NOT NULL,"
			" array_to_string(movie_categories, ','), array_to_string(tv_categories, ','), files_origin,"
			" kind FROM indexer_source WHERE id = $1::bigint FOR UPDATE", 1, old_params, &old, &local);
		if (ok && axis_db_result_get_n_rows(old) == 0) {
			ok = FALSE;
			g_set_error_literal(&local, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND,
				"there is no such indexer");
		}
		if (ok) {
			/* A source's kind does not change: its fields but that. */
			gboolean native = bool_at(old, 0, 2);
			g_autofree gchar *old_movie = g_strdup_printf("{%s}", axis_db_result_get_text(old, 0, 3) != NULL ?
				axis_db_result_get_text(old, 0, 3) : "");
			g_autofree gchar *old_tv = g_strdup_printf("{%s}", axis_db_result_get_text(old, 0, 4) != NULL ?
				axis_db_result_get_text(old, 0, 4) : "");
			const gchar *update[11];
			const AxisIndexerKindInfo *old_kind = axis_indexers_find_kind(self, axis_db_result_get_text(old, 0, 6));
			gboolean site = old_kind != NULL && old_kind->site;

			if (site) {
				/* A public site is at its address, not under a path (one
				 * that runs a definition, under its address's). */
				g_free(base_path);
				base_path = g_strdup(old_kind->definition && site_path != NULL ? site_path : "");
			}
			changed = g_strcmp0(axis_db_result_get_text(old, 0, 0), origin.base) != 0 ||
				g_strcmp0(axis_db_result_get_text(old, 0, 1), base_path) != 0;
			/* Other categories or another origin for files: what was
			 * kept was searched or read for the old ones. */
			refiltered = native && (g_strcmp0(old_movie, movie_categories) != 0 ||
				g_strcmp0(old_tv, tv_categories) != 0 ||
				g_strcmp0(axis_db_result_get_text(old, 0, 5), files_origin) != 0);
			update[0] = name;
			update[1] = origin.base;
			update[2] = base_path;
			update[3] = spec->accept_untrusted ? "true" : "false";
			update[4] = spec->enabled ? "true" : "false";
			update[5] = id_text;
			update[6] = native ? "false" : follow;
			update[7] = priority;
			update[8] = movie_categories;
			update[9] = tv_categories;
			update[10] = native && !site ? files_origin : NULL;
			ok = axis_db_conn_exec(conn,
				"UPDATE indexer_source SET name = $1::text, address = $2::text, base_path = $3::text,"
				" accept_untrusted = $4::boolean, enabled = $5::boolean, follow_redirects = $7::boolean,"
				/* Switched on again: it has not been failing all the while it
				 * was off.  The time begins again, for the page "Needs
				 * attention". */
				" not_working_since = CASE WHEN $5::boolean AND NOT enabled AND not_working_since IS NOT NULL"
				"  THEN now() ELSE not_working_since END,"
				" priority = $8::integer, movie_categories = $9::integer[], tv_categories = $10::integer[],"
				" files_origin = $11::text,"
				" files_origin_seen = CASE WHEN files_origin_seen = $11::text THEN NULL ELSE files_origin_seen END,"
				" updated_at = now() WHERE id = $6::bigint", 11, update, NULL, &local);
		}
	}
	if (ok && secret != NULL) {
		g_autofree gchar *source_text = axis_indexer_int_text(id);
		const gchar *secret_params[2];

		changed = TRUE;
		secret_params[0] = source_text;
		secret_params[1] = secret;
		ok = axis_db_conn_exec(conn,
			"INSERT INTO indexer_source_secret (source_id, secret) VALUES ($1::bigint, $2::text)"
			" ON CONFLICT (source_id) DO UPDATE SET secret = excluded.secret, set_at = now()", 2, secret_params, NULL,
			&local);
	}
	/* A source that is somewhere else, or opened with another key, has
	 * other searches and other indexers: what was kept is about the old. */
	if (ok && changed && id_text != NULL) {
		const gchar *id_params[1];

		id_params[0] = id_text;
		ok = axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE source_id = $1::bigint", 1, id_params, NULL,
				&local) &&
			axis_db_conn_exec(conn, "DELETE FROM indexer_source_indexer WHERE source_id = $1::bigint", 1, id_params,
				NULL, &local) &&
			axis_db_conn_exec(conn,
				"UPDATE indexer_source SET state = 'unchecked', state_message = NULL, version = NULL,"
				" not_working_since = NULL,"
				" indexers_at = NULL, last_checked_at = NULL, last_contact_at = NULL,"
				" caps_at = NULL, caps_problem = NULL, caps_modes = 0, caps_movie_ids = 0, caps_tv_ids = 0,"
				" caps_movie_text = false, caps_tv_text = false, caps_tv_numbers = false, caps_limit_max = NULL,"
				" caps_limit_default = NULL, caps_categories = '{}', api_used = NULL, api_max = NULL,"
				" grab_used = NULL, grab_max = NULL, limits_at = NULL, held_until = NULL, grabs_held_until = NULL,"
				" hold_message = NULL, files_origin_seen = NULL WHERE id = $1::bigint", 1,
				id_params, NULL, &local);
	} else if (ok && refiltered && id_text != NULL) {
		const gchar *id_params[1];

		id_params[0] = id_text;
		ok = axis_db_conn_exec(conn, "DELETE FROM indexer_search WHERE source_id = $1::bigint", 1, id_params, NULL,
			&local);
	}
	if (ok)
		ok = axis_db_conn_commit(conn, &local);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	axis_http_origin_clear(&origin);
	if (!ok) {
		scrub_error(local, secret);
		if (g_error_matches(local, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION)) {
			g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_EXISTS,
				"There is an indexer of that name already.");
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
		}
		return FALSE;
	}
	if (changed)
		axis_indexer_forget_source(self, id);
	if (id_out != NULL)
		*id_out = id;
	return TRUE;
}

gboolean
axis_indexers_delete_source(
	AxisIndexers  *self,
	gint64         id,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = axis_indexer_int_text(id);
	const gchar *params[1];

	params[0] = id_text;
	if (!axis_db_pool_exec(self->pool, "DELETE FROM indexer_source WHERE id = $1::bigint", 1, params, &result, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND, "there is no such indexer");
		return FALSE;
	}
	axis_indexer_forget_source(self, id);
	return TRUE;
}
