/*
 * axis-identify-store.c - what the Identify pages read, a person's actions on
 * a unit, and adding every identified film of a root
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every action runs in one transaction with the unit's row locked: its new
 * state, the owner's links and the record of who did it go together.
 */

#include "identify/axis-identify-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-lifecycle.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Freeing ────────────────────────────────────────────────────── */

void
axis_identify_root_info_free(AxisIdentifyRootInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->name);
	g_free(info->volume_name);
	g_free(info->last_run_text);
	g_free(info);
}

static void
file_info_free(AxisIdentifyFileInfo *file)
{
	if (file == NULL)
		return;
	g_free(file->rel_path);
	g_free(file->quality);
	g_free(file->resolution);
	g_free(file->quality_text);
	g_free(file->episode_code);
	g_free(file->episode_text);
	g_free(file);
}

static void
candidate_info_free(AxisIdentifyCandidateInfo *candidate)
{
	if (candidate == NULL)
		return;
	axis_meta_title_free(candidate->title);
	g_free(candidate->why);
	g_free(candidate->poster);
	g_free(candidate);
}

static void
action_info_free(AxisIdentifyActionInfo *action)
{
	if (action == NULL)
		return;
	g_free(action->action);
	g_free(action->by_user);
	g_free(action->text);
	g_free(action);
}

void
axis_identify_unit_info_free(AxisIdentifyUnitInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->root_name);
	g_free(info->unit_kind);
	g_free(info->rel_path);
	g_free(info->state);
	g_free(info->confidence);
	g_free(info->title_label);
	g_free(info->linked_how);
	g_free(info->decided_by);
	g_clear_pointer(&info->reasons, g_ptr_array_unref);
	g_clear_pointer(&info->candidates, g_ptr_array_unref);
	g_free(info->read_text);
	g_free(info->read_title);
	g_free(info->problem);
	g_free(info->doubt);
	g_clear_pointer(&info->files, g_ptr_array_unref);
	g_clear_pointer(&info->actions, g_ptr_array_unref);
	g_free(info);
}

static gchar *
title_label(
	const gchar *title,
	gint         year
){
	if (title == NULL)
		return NULL;
	return year > 0 ? g_strdup_printf("%s (%d)", title, year) : g_strdup(title);
}

/* ── Roots ──────────────────────────────────────────────────────── */

static const gchar roots_select[] =
	"SELECT r.id, r.name, v.name, v.state, coalesce(s.complete_generation, 0) > 0,"
	" coalesce(ir.scopes, 0), coalesce(ir.pending, 0),"
	" count(i.id) FILTER (WHERE i.state = 'linked' AND i.linked_how IN ('auto', 'imported')),"
	" count(i.id) FILTER (WHERE i.state = 'linked' AND i.linked_how IN ('confirmed', 'chosen')),"
	" count(i.id) FILTER (WHERE i.state = 'review' AND i.confidence IN ('sure', 'likely')),"
	" count(i.id) FILTER (WHERE i.state = 'review' AND coalesce(i.confidence, 'unknown') = 'unknown'),"
	" count(i.id) FILTER (WHERE i.state = 'not_film'),"
	" count(i.id) FILTER (WHERE i.state = 'waiting'),"
	" count(DISTINCT i.title_id) FILTER (WHERE i.state = 'linked'),"
	" " US("ir.last_run_at") ", ir.last_run_text, " US("ir.retry_after") ","
	" EXISTS (SELECT 1 FROM job j WHERE j.dedupe_key = '" AXIS_IDENTIFY_KEY_PREFIX "' || r.id::text"
	"   AND j.state IN ('queued', 'running')),"
	" r.media_type, coalesce(ir.seasons_pending, 0),"
	" count(i.id) FILTER (WHERE i.state = 'linked' AND i.doubt IS NOT NULL)"
	" FROM library_root r JOIN volume v ON v.id = r.volume_id"
	" LEFT JOIN library_root_state s ON s.root_id = r.id"
	" LEFT JOIN library_identity_root ir ON ir.root_id = r.id"
	" LEFT JOIN library_identity i ON i.root_id = r.id"
	" WHERE r.media_type IN ('movies', 'tv')";

static const gchar roots_group[] =
	" GROUP BY r.id, r.name, v.name, v.state, s.complete_generation, ir.scopes, ir.pending, ir.last_run_at,"
	" ir.last_run_text, ir.retry_after, ir.seasons_pending ORDER BY r.media_type = 'tv', lower(r.name), r.id";

static gboolean
read_roots(
	AxisIdentify  *self,
	gint64         root_id,
	GPtrArray    **roots,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = g_strconcat(roots_select, root_id > 0 ? " AND r.id = $1::bigint" : "", roots_group, NULL);
	g_autofree gchar *root = axis_identify_int_text(root_id);
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	params[0] = root;
	if (!axis_db_pool_exec(self->pool, sql, root_id > 0 ? 1 : 0, params, &rows, error))
		return FALSE;
	*roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_root_info_free);
	conn = axis_db_result_get_n_rows(rows) > 0 ? axis_db_pool_acquire(self->pool, error) : NULL;
	if (axis_db_result_get_n_rows(rows) > 0 && conn == NULL) {
		g_clear_pointer(roots, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		AxisIdentifyRootInfo *info = g_new0(AxisIdentifyRootInfo, 1);
		const gchar *state = axis_db_result_get_text(rows, i, 3);
		gpointer data = NULL;
		const AxisIdentifyOwnerInfo *owner;

		info->kind = axis_identify_root_kind(axis_db_result_get_text(rows, i, 18));
		info->seasons_pending = (guint)axis_identify_row_int(rows, i, 19, 0);
		owner = axis_identify_owner(self, info->kind, &data);
		info->root_id = axis_identify_row_int(rows, i, 0, 0);
		info->name = axis_identify_row_text(rows, i, 1);
		info->volume_name = axis_identify_row_text(rows, i, 2);
		info->volume_state = AXIS_VOLUME_OFFLINE;
		if (state != NULL)
			axis_volume_state_from_string(state, &info->volume_state);
		axis_db_result_get_boolean(rows, i, 4, &info->scanned);
		info->scopes = (guint)axis_identify_row_int(rows, i, 5, 0);
		info->pending = (guint)axis_identify_row_int(rows, i, 6, 0);
		info->sure = (guint)axis_identify_row_int(rows, i, 7, 0);
		info->confirmed = (guint)axis_identify_row_int(rows, i, 8, 0);
		info->likely = (guint)axis_identify_row_int(rows, i, 9, 0);
		info->unknown = (guint)axis_identify_row_int(rows, i, 10, 0);
		info->not_film = (guint)axis_identify_row_int(rows, i, 11, 0);
		info->waiting = (guint)axis_identify_row_int(rows, i, 12, 0);
		info->doubted = (guint)axis_identify_row_int(rows, i, 20, 0);
		info->titles = (guint)axis_identify_row_int(rows, i, 13, 0);
		info->last_run_us = axis_identify_row_int(rows, i, 14, 0);
		info->last_run_text = axis_identify_row_text(rows, i, 15);
		info->retry_after_us = axis_identify_row_int(rows, i, 16, 0);
		axis_db_result_get_boolean(rows, i, 17, &info->running);
		if (conn != NULL && owner != NULL && owner->count_managed != NULL)
			ok = owner->count_managed(data, conn, info->root_id, &info->managed, error);
		g_ptr_array_add(*roots, info);
	}
	if (conn != NULL)
		axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(roots, g_ptr_array_unref);
	return ok;
}

gboolean
axis_identify_list_roots(
	AxisIdentify  *self,
	GPtrArray    **roots,
	GError       **error
){
	return read_roots(self, 0, roots, error);
}

gboolean
axis_identify_get_root(
	AxisIdentify          *self,
	gint64                 root_id,
	AxisIdentifyRootInfo **info,
	GError               **error
){
	g_autoptr(GPtrArray) roots = NULL;

	*info = NULL;
	if (root_id <= 0 || !read_roots(self, root_id, &roots, error)) {
		if (root_id <= 0)
			g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such root of "
				"movies or for TV");
		return FALSE;
	}
	if (roots->len == 0) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such root of movies "
			"or for TV");
		return FALSE;
	}
	*info = g_ptr_array_steal_index(roots, 0);
	return TRUE;
}

/* ── Units ──────────────────────────────────────────────────────── */

static const gchar units_select[] =
	"SELECT i.id, i.root_id, r.name, i.unit_kind, i.rel_path, i.state, i.confidence, i.title_id, t.title, t.year,"
	" i.linked_how, i.decided_by, " US("i.decided_at") ", i.held, i.reasons::text, i.candidates::text, i.read_text,"
	" i.read_title, i.read_year, i.duration_ms, i.problem, " US("i.examined_at") ", count(*) OVER (), r.media_type,"
	" i.doubt"
	" FROM library_identity i JOIN library_root r ON r.id = i.root_id LEFT JOIN meta_title t ON t.id = i.title_id";

static AxisIdentifyUnitInfo *
unit_from_row(
	AxisDbResult *rows,
	guint         row
){
	AxisIdentifyUnitInfo *info = g_new0(AxisIdentifyUnitInfo, 1);

	info->id = axis_identify_row_int(rows, row, 0, 0);
	info->root_id = axis_identify_row_int(rows, row, 1, 0);
	info->kind = axis_identify_root_kind(axis_db_result_get_text(rows, row, 23));
	info->root_name = axis_identify_row_text(rows, row, 2);
	info->unit_kind = axis_identify_row_text(rows, row, 3);
	info->rel_path = axis_identify_row_text(rows, row, 4);
	info->state = axis_identify_row_text(rows, row, 5);
	info->confidence = axis_identify_row_text(rows, row, 6);
	info->title_id = axis_identify_row_int(rows, row, 7, 0);
	info->title_label = title_label(axis_db_result_get_text(rows, row, 8), (gint)axis_identify_row_int(rows, row, 9, 0));
	info->linked_how = axis_identify_row_text(rows, row, 10);
	info->decided_by = axis_identify_row_text(rows, row, 11);
	info->decided_us = axis_identify_row_int(rows, row, 12, 0);
	axis_db_result_get_boolean(rows, row, 13, &info->held);
	info->reasons = axis_identify_reasons_from_json(axis_db_result_get_text(rows, row, 14));
	info->read_text = axis_identify_row_text(rows, row, 16);
	info->read_title = axis_identify_row_text(rows, row, 17);
	info->read_year = (gint)axis_identify_row_int(rows, row, 18, 0);
	info->duration_ms = axis_identify_row_int(rows, row, 19, 0);
	info->problem = axis_identify_row_text(rows, row, 20);
	info->examined_us = axis_identify_row_int(rows, row, 21, 0);
	info->doubt = axis_identify_row_text(rows, row, 24);
	info->files = g_ptr_array_new_with_free_func((GDestroyNotify)file_info_free);
	info->candidates = g_ptr_array_new_with_free_func((GDestroyNotify)candidate_info_free);
	info->actions = g_ptr_array_new_with_free_func((GDestroyNotify)action_info_free);
	return info;
}

/* The stored candidates' ids, best first. */
static GArray *
candidate_ids(const gchar *json)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonArray *array;
	guint i;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return ids;
	array = json_node_get_array(json_parser_get_root(parser));
	for (i = 0; i < json_array_get_length(array) && ids->len < AXIS_IDENTIFY_MAX_CANDIDATES; i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
		JsonNode *id = object != NULL ? json_object_get_member(object, "id") : NULL;

		if (id != NULL && JSON_NODE_HOLDS_VALUE(id) && json_node_get_value_type(id) == G_TYPE_INT64) {
			gint64 value = json_node_get_int(id);

			if (value > 0)
				g_array_append_val(ids, value);
		}
	}
	return ids;
}

/* The stored brief records of @ids, in their order (missing ones left out). */
static gboolean
read_titles(
	AxisIdentify  *self,
	GArray        *ids,
	GPtrArray     *into,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) list = g_string_new("{");
	const gchar *params[1];
	guint i;
	guint k;

	if (ids->len == 0)
		return TRUE;
	for (i = 0; i < ids->len; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(list, '}');
	params[0] = list->str;
	/* With what tells candidates apart at a glance: the runtime and a
	 * series' seasons. */
	if (!axis_db_pool_exec(self->pool, "SELECT t.id, t.kind, t.provider, t.provider_id, t.title, t.original_title, t.year,"
	        " t.overview, t.runtime_minutes, t.poster_ref, (SELECT count(*) FROM meta_season_followed s"
	        " WHERE s.title_id = t.id AND s.number > 0) FROM meta_title t WHERE t.id = ANY($1::bigint[])", 1, params, &rows,
	        error))
		return FALSE;
	for (i = 0; i < ids->len; i++) {
		gint64 want = g_array_index(ids, gint64, i);

		for (k = 0; k < axis_db_result_get_n_rows(rows); k++) {
			AxisIdentifyCandidateInfo *c;
			AxisMetaKind kind = AXIS_META_KIND_FILM;

			if (axis_identify_row_int(rows, k, 0, 0) != want)
				continue;
			if (!axis_meta_kind_from_string(axis_db_result_get_text(rows, k, 1), &kind))
				break;
			c = g_new0(AxisIdentifyCandidateInfo, 1);
			c->title = axis_meta_title_new(kind);
			c->title->id = want;
			c->title->provider = axis_identify_row_text(rows, k, 2);
			c->title->provider_id = axis_identify_row_text(rows, k, 3);
			c->title->title = axis_identify_row_text(rows, k, 4);
			c->title->original_title = axis_identify_row_text(rows, k, 5);
			c->title->year = (gint)axis_identify_row_int(rows, k, 6, 0);
			c->title->overview = axis_identify_row_text(rows, k, 7);
			c->title->runtime = (gint)axis_identify_row_int(rows, k, 8, 0);
			c->title->poster_ref = axis_identify_row_text(rows, k, 9);
			c->seasons = (guint)CLAMP(axis_identify_row_int(rows, k, 10, 0), 0, 10000);
			/* Only looks: a poster not kept is fetched when its card's
			 * placeholder is on screen and asks for it (/search/poster),
			 * not for every candidate of a list. */
			if (c->title->provider != NULL && c->title->poster_ref != NULL)
				c->poster = axis_meta_artwork_find(self->meta, c->title->provider, c->title->poster_ref,
					AXIS_META_ART_POSTER_SMALL, FALSE);
			g_ptr_array_add(into, c);
			break;
		}
	}
	return TRUE;
}

/* Each unit's videos, in one query. */
static gboolean
read_files(
	AxisIdentify  *self,
	GPtrArray     *units,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) list = g_string_new("{");
	const gchar *params[1];
	guint i;
	guint k;

	if (units->len == 0)
		return TRUE;
	for (i = 0; i < units->len; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			((AxisIdentifyUnitInfo *)g_ptr_array_index(units, i))->id);
	g_string_append_c(list, '}');
	params[0] = list->str;
	/* At most 200 files of a unit (a series' folder holds hundreds): the
	 * series' own pages show them all, by episode. */
	if (!axis_db_pool_exec(self->pool, "SELECT x.identity_id, x.id, x.rel_path, x.size, x.duration_ms, x.best,"
	        " x.quality, x.resolution, x.quality_text, x.episode_code, x.episode_text FROM (SELECT lif.identity_id, lf.id,"
	        " lf.rel_path, lf.size, mi.duration_ms, lif.best, lif.quality, lif.resolution, lif.quality_text,"
	        " lif.episode_code, lif.episode_text, row_number() OVER (PARTITION BY lif.identity_id"
	        "  ORDER BY lif.best DESC, lf.rel_path) AS n"
	        " FROM library_identity_file lif"
	        " JOIN library_file lf ON lf.id = lif.file_id LEFT JOIN media_info mi ON mi.file_id = lf.id AND mi.status = 'ok'"
	        " WHERE lif.identity_id = ANY($1::bigint[])) x WHERE x.n <= 200 ORDER BY x.identity_id, x.n", 1, params,
	        &rows, error))
		return FALSE;
	for (k = 0; k < axis_db_result_get_n_rows(rows); k++) {
		gint64 owner = axis_identify_row_int(rows, k, 0, 0);

		for (i = 0; i < units->len; i++) {
			AxisIdentifyUnitInfo *unit = g_ptr_array_index(units, i);
			AxisIdentifyFileInfo *file;

			if (unit->id != owner)
				continue;
			file = g_new0(AxisIdentifyFileInfo, 1);
			file->file_id = axis_identify_row_int(rows, k, 1, 0);
			file->rel_path = axis_identify_row_text(rows, k, 2);
			file->size = axis_identify_row_int(rows, k, 3, 0);
			file->duration_ms = MAX(axis_identify_row_int(rows, k, 4, 0), 0);
			axis_db_result_get_boolean(rows, k, 5, &file->best);
			file->quality = axis_identify_row_text(rows, k, 6);
			file->resolution = axis_identify_row_text(rows, k, 7);
			file->quality_text = axis_identify_row_text(rows, k, 8);
			file->episode_code = axis_identify_row_text(rows, k, 9);
			file->episode_text = axis_identify_row_text(rows, k, 10);
			g_ptr_array_add(unit->files, file);
			break;
		}
	}
	return TRUE;
}

static gboolean
fill_candidates(
	AxisIdentify          *self,
	AxisIdentifyUnitInfo  *unit,
	const gchar           *json,
	GError               **error
){
	g_autoptr(GArray) ids = candidate_ids(json);

	return read_titles(self, ids, unit->candidates, error);
}

/* Which units of a root (as "i") @list holds; *@latest_first: ordered by the
 * latest decision first, else by path. */
static const gchar *
list_condition(
	AxisIdentifyList  list,
	gboolean         *latest_first
){
	*latest_first = FALSE;
	switch (list) {
	case AXIS_IDENTIFY_LIST_LIKELY:
		return "i.state = 'review' AND i.confidence IN ('sure', 'likely')";
	case AXIS_IDENTIFY_LIST_UNKNOWN:
		return "i.state = 'review' AND coalesce(i.confidence, 'unknown') = 'unknown'";
	case AXIS_IDENTIFY_LIST_LINKED:
		*latest_first = TRUE;
		return "i.state = 'linked'";
	case AXIS_IDENTIFY_LIST_NOT_FILM:
		return "i.state = 'not_film'";
	case AXIS_IDENTIFY_LIST_DOUBT:
		return "i.state = 'linked' AND i.doubt IS NOT NULL";
	default:
		return "i.state = 'waiting'";
	}
}

gboolean
axis_identify_list_neighbours(
	AxisIdentify      *self,
	gint64             unit_id,
	AxisIdentifyList   list,
	gint64            *next_id,
	gint64            *previous_id,
	guint             *position,
	GError           **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *id = axis_identify_int_text(unit_id);
	gboolean latest_first;
	const gchar *condition = list_condition(list, &latest_first);
	/* The list's order as a row of i, the same row of m (the unit), and
	 * the comparison that says "after it" in that order. */
	const gchar *key = latest_first ? "(coalesce(i.decided_at, i.examined_at), i.id)" : "(i.rel_path, i.id)";
	const gchar *own = latest_first ? "(coalesce(m.decided_at, m.examined_at), m.id)" : "(m.rel_path, m.id)";
	const gchar *after = latest_first ? "<" : ">";
	const gchar *before = latest_first ? ">" : "<";
	const gchar *forward = latest_first ? "coalesce(i.decided_at, i.examined_at) DESC, i.id DESC" : "i.rel_path, i.id";
	const gchar *back = latest_first ? "coalesce(i.decided_at, i.examined_at), i.id" : "i.rel_path DESC, i.id DESC";
	const gchar *params[1];

	*next_id = 0;
	*previous_id = 0;
	*position = 0;
	params[0] = id;
	sql = g_strdup_printf("SELECT"
		" (SELECT i.id FROM library_identity i WHERE i.root_id = m.root_id AND %s AND %s %s %s ORDER BY %s LIMIT 1),"
		" (SELECT i.id FROM library_identity i WHERE i.root_id = m.root_id AND %s AND %s %s %s ORDER BY %s LIMIT 1),"
		" (SELECT count(*) FROM library_identity i WHERE i.root_id = m.root_id AND %s AND %s %s %s)"
		" FROM library_identity m WHERE m.id = $1::bigint",
		condition, key, after, own, forward,
		condition, key, before, own, back,
		condition, key, before, own);
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	*next_id = axis_identify_row_int(rows, 0, 0, 0);
	*previous_id = axis_identify_row_int(rows, 0, 1, 0);
	*position = (guint)CLAMP(axis_identify_row_int(rows, 0, 2, 0), 0, G_MAXINT);
	return TRUE;
}

gboolean
axis_identify_list_units(
	AxisIdentify      *self,
	gint64             root_id,
	AxisIdentifyList   list,
	guint              offset,
	guint              limit,
	gboolean           with_candidates,
	GPtrArray        **units,
	guint             *total,
	GError           **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *root = axis_identify_int_text(root_id);
	g_autofree gchar *from = g_strdup_printf("%u", offset);
	g_autofree gchar *count = g_strdup_printf("%u", MIN(limit, 500u));
	gboolean latest_first;
	const gchar *condition = list_condition(list, &latest_first);
	const gchar *order = latest_first ? " ORDER BY coalesce(i.decided_at, i.examined_at) DESC, i.id DESC" :
		" ORDER BY i.rel_path, i.id";
	const gchar *params[3];
	guint i;

	*units = NULL;
	if (total != NULL)
		*total = 0;
	params[0] = root;
	params[1] = from;
	params[2] = count;
	sql = g_strdup_printf("%s WHERE i.root_id = $1::bigint AND %s%s OFFSET $2::integer LIMIT $3::integer", units_select,
		condition, order);
	if (!axis_db_pool_exec(self->pool, sql, 3, params, &rows, error))
		return FALSE;
	*units = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_unit_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisIdentifyUnitInfo *unit = unit_from_row(rows, i);

		g_ptr_array_add(*units, unit);
		if (total != NULL && i == 0)
			*total = (guint)axis_identify_row_int(rows, i, 22, 0);
		if (with_candidates && !fill_candidates(self, unit, axis_db_result_get_text(rows, i, 15), error)) {
			g_clear_pointer(units, g_ptr_array_unref);
			return FALSE;
		}
	}
	if (!read_files(self, *units, error)) {
		g_clear_pointer(units, g_ptr_array_unref);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_identify_get_unit(
	AxisIdentify          *self,
	gint64                 unit_id,
	AxisIdentifyUnitInfo **info,
	GError               **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisDbResult) actions = NULL;
	g_autoptr(GPtrArray) one = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_unit_info_free);
	g_autofree gchar *sql = g_strconcat(units_select, " WHERE i.id = $1::bigint", NULL);
	g_autofree gchar *id = axis_identify_int_text(unit_id);
	AxisIdentifyUnitInfo *unit;
	const gchar *params[1];
	guint i;

	*info = NULL;
	params[0] = id;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such unit");
		return FALSE;
	}
	unit = unit_from_row(rows, 0);
	g_ptr_array_add(one, unit);
	if (!fill_candidates(self, unit, axis_db_result_get_text(rows, 0, 15), error) || !read_files(self, one, error))
		return FALSE;
	if (!axis_db_pool_exec(self->pool, "SELECT action, by_user, text, " US("created_at") " FROM library_identity_action"
	        " WHERE identity_id = $1::bigint ORDER BY id DESC LIMIT 50", 1, params, &actions, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(actions); i++) {
		AxisIdentifyActionInfo *action = g_new0(AxisIdentifyActionInfo, 1);

		action->action = axis_identify_row_text(actions, i, 0);
		action->by_user = axis_identify_row_text(actions, i, 1);
		action->text = axis_identify_row_text(actions, i, 2);
		action->created_us = axis_identify_row_int(actions, i, 3, 0);
		g_ptr_array_add(unit->actions, action);
	}
	{
		gpointer data = NULL;
		const AxisIdentifyOwnerInfo *owner = axis_identify_owner(self, unit->kind, &data);

		if (owner != NULL && owner->managed != NULL && unit->title_id > 0) {
			AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
			gboolean ok;

			if (conn == NULL)
				return FALSE;
			ok = owner->managed(data, conn, unit->title_id, &unit->managed, error);
			axis_db_pool_release(self->pool, conn);
			if (!ok)
				return FALSE;
		}
	}
	*info = g_ptr_array_steal_index(one, 0);
	return TRUE;
}

gboolean
axis_identify_units_of_title(
	AxisIdentify  *self,
	gint64         title_id,
	GPtrArray    **units,
	gboolean      *managed,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *sql = g_strconcat(units_select, " WHERE i.title_id = $1::bigint AND i.state = 'linked'"
		" ORDER BY r.name, i.rel_path LIMIT 50", NULL);
	g_autofree gchar *id = axis_identify_int_text(title_id);
	const gchar *params[1];
	guint i;

	params[0] = id;
	*units = NULL;
	*managed = FALSE;
	if (!axis_db_pool_exec(self->pool, sql, 1, params, &rows, error))
		return FALSE;
	*units = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_unit_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(*units, unit_from_row(rows, i));
	if (!read_files(self, *units, error)) {
		g_clear_pointer(units, g_ptr_array_unref);
		return FALSE;
	}
	if ((*units)->len > 0) {
		gpointer data = NULL;
		const AxisIdentifyOwnerInfo *owner = axis_identify_owner(self,
			((AxisIdentifyUnitInfo *)g_ptr_array_index(*units, 0))->kind, &data);
		AxisDbConn *conn;
		gboolean ok;

		if (owner == NULL || owner->managed == NULL)
			return TRUE;
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL) {
			g_clear_pointer(units, g_ptr_array_unref);
			return FALSE;
		}
		ok = owner->managed(data, conn, title_id, managed, error);
		axis_db_pool_release(self->pool, conn);
		if (!ok) {
			g_clear_pointer(units, g_ptr_array_unref);
			return FALSE;
		}
	}
	return TRUE;
}

void
axis_identify_folder_free(AxisIdentifyFolder *folder)
{
	if (folder == NULL)
		return;
	g_free(folder->state);
	axis_meta_title_free(folder->title);
	g_free(folder->poster);
	g_free(folder);
}

gboolean
axis_identify_folders(
	AxisIdentify  *self,
	const gint64  *dir_ids,
	guint          n_dirs,
	GPtrArray    **folders,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) list = g_string_new("{");
	const gchar *params[1];
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(folders != NULL, FALSE);
	g_return_val_if_fail(n_dirs <= AXIS_IDENTIFY_FOLDERS_MAX, FALSE);

	*folders = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_folder_free);
	if (n_dirs == 0)
		return TRUE;
	for (i = 0; i < n_dirs; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", dir_ids[i]);
	g_string_append_c(list, '}');
	params[0] = list->str;
	/* A folder unit's path is its folder's: the same hash, under the unique
	 * index of each. */
	if (!axis_db_pool_exec(self->pool, "SELECT d.id, i.id, i.state, t.id, t.kind, t.provider, t.provider_id, t.title,"
	        " t.year, t.poster_ref FROM library_dir d"
	        " JOIN library_identity i ON i.root_id = d.root_id AND i.path_hash = d.path_hash AND i.unit_kind = 'folder'"
	        " LEFT JOIN meta_title t ON t.id = i.title_id AND i.state = 'linked'"
	        " WHERE d.id = ANY($1::bigint[]) AND d.path <> ''", 1, params, &rows, error)) {
		g_clear_pointer(folders, g_ptr_array_unref);
		return FALSE;
	}
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisIdentifyFolder *folder = g_new0(AxisIdentifyFolder, 1);
		AxisMetaKind kind = AXIS_META_KIND_FILM;

		folder->dir_id = axis_identify_row_int(rows, i, 0, 0);
		folder->unit_id = axis_identify_row_int(rows, i, 1, 0);
		folder->state = axis_identify_row_text(rows, i, 2);
		if (axis_identify_row_int(rows, i, 3, 0) > 0 &&
		    axis_meta_kind_from_string(axis_db_result_get_text(rows, i, 4), &kind)) {
			AxisMetaTitle *title = axis_meta_title_new(kind);

			title->id = axis_identify_row_int(rows, i, 3, 0);
			title->provider = axis_identify_row_text(rows, i, 5);
			title->provider_id = axis_identify_row_text(rows, i, 6);
			title->title = axis_identify_row_text(rows, i, 7);
			title->year = (gint)axis_identify_row_int(rows, i, 8, 0);
			title->poster_ref = axis_identify_row_text(rows, i, 9);
			if (title->provider != NULL && title->poster_ref != NULL)
				folder->poster = axis_meta_artwork_find(self->meta, title->provider, title->poster_ref,
					AXIS_META_ART_POSTER_SMALL, TRUE);
			folder->title = title;
		}
		g_ptr_array_add(*folders, folder);
	}
	return TRUE;
}

gboolean
axis_identify_count_review(
	AxisIdentify  *self,
	guint         *count,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	gint64 n = 0;

	*count = 0;
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM library_identity WHERE state = 'review'", 0, NULL, &rows,
	        error))
		return FALSE;
	axis_db_result_get_int64(rows, 0, 0, &n);
	*count = (guint)MAX(n, 0);
	return TRUE;
}

gboolean
axis_identify_count_review_by_kind(
	AxisIdentify  *self,
	guint         *films,
	guint         *series,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;

	*films = 0;
	*series = 0;
	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FILTER (WHERE r.media_type = 'movies'),"
	        " count(*) FILTER (WHERE r.media_type = 'tv') FROM library_identity i JOIN library_root r ON r.id = i.root_id"
	        " WHERE i.state = 'review'", 0, NULL, &rows, error))
		return FALSE;
	*films = (guint)MAX(axis_identify_row_int(rows, 0, 0, 0), 0);
	*series = (guint)MAX(axis_identify_row_int(rows, 0, 1, 0), 0);
	return TRUE;
}

/* ── Acting ─────────────────────────────────────────────────────── */

static const gchar *const action_names[] = { "confirm", "choose", "not_film", "undo" };

const gchar *
axis_identify_action_name(AxisIdentifyAction action)
{
	return (guint)action < G_N_ELEMENTS(action_names) ? action_names[action] : "undo";
}

gboolean
axis_identify_action_from_name(
	const gchar        *name,
	AxisIdentifyAction *action
){
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(action_names); i++) {
		if (strcmp(name, action_names[i]) == 0) {
			*action = (AxisIdentifyAction)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* A stored title's label, of @kind; %NULL when there is none. */
static gboolean
film_label(
	AxisDbConn    *conn,
	gint64         title_id,
	AxisMetaKind   kind,
	gchar        **label,
	GError       **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_identify_int_text(title_id);
	const gchar *params[2];

	*label = NULL;
	params[0] = id;
	params[1] = axis_meta_kind_to_string(kind == AXIS_META_KIND_SERIES ? AXIS_META_KIND_SERIES : AXIS_META_KIND_FILM);
	if (!axis_db_conn_exec(conn, "SELECT title, year FROM meta_title WHERE id = $1::bigint AND kind = $2", 2,
	        params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0)
		*label = title_label(axis_db_result_get_text(rows, 0, 0), (gint)axis_identify_row_int(rows, 0, 1, 0));
	return TRUE;
}

static gboolean
record_action(
	AxisDbConn   *conn,
	gint64        identity_id,
	gint64        root_id,
	const gchar  *rel_path,
	const gchar  *action,
	gint64        title_id,
	const gchar  *user,
	const gchar  *text,
	GError      **error
){
	g_autofree gchar *identity = identity_id > 0 ? axis_identify_int_text(identity_id) : NULL;
	g_autofree gchar *root = axis_identify_int_text(root_id);
	g_autofree gchar *title = title_id > 0 ? axis_identify_int_text(title_id) : NULL;
	g_autofree gchar *cut = axis_identify_text_cut(text, 2000);
	const gchar *params[7];

	params[0] = identity;
	params[1] = root;
	params[2] = rel_path;
	params[3] = action;
	params[4] = title;
	params[5] = user;
	params[6] = cut;
	return axis_db_conn_exec(conn, "INSERT INTO library_identity_action (identity_id, root_id, rel_path, action,"
		" title_id, by_user, text) VALUES ($1::bigint, $2::bigint, $3, $4, $5::bigint, $6, $7)", 7, params, NULL, error);
}

static gboolean
act_in_transaction(
	AxisIdentify        *self,
	AxisDbConn          *conn,
	gint64               unit_id,
	AxisIdentifyAction   action,
	gint64               title_id,
	const gchar         *user,
	GError             **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GArray) candidates = NULL;
	g_autofree gchar *id = axis_identify_int_text(unit_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *target_text = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *how = NULL;
	g_autofree gchar *rel_path = NULL;
	const gchar *params[4];
	AxisMetaKind kind;
	const gchar *noun;
	gint64 current = 0;
	gint64 root_id;
	gint64 target = 0;
	gboolean doubted = FALSE;
	guint i;

	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT i.state, i.title_id, i.candidates::text, i.rel_path, i.root_id, i.linked_how,"
	        " r.media_type, i.doubt IS NOT NULL FROM library_identity i JOIN library_root r ON r.id = i.root_id"
	        " WHERE i.id = $1::bigint FOR UPDATE OF i", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such unit");
		return FALSE;
	}
	state = axis_identify_row_text(rows, 0, 0);
	current = axis_identify_row_int(rows, 0, 1, 0);
	rel_path = axis_identify_row_text(rows, 0, 3);
	root_id = axis_identify_row_int(rows, 0, 4, 0);
	how = axis_identify_row_text(rows, 0, 5);
	kind = axis_identify_root_kind(axis_db_result_get_text(rows, 0, 6));
	axis_db_result_get_boolean(rows, 0, 7, &doubted);
	noun = axis_identify_noun(kind);
	if (g_strcmp0(how, "imported") == 0) {
		g_set_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID, "an import of Axis placed this file for "
			"its %s, and that link stands", noun);
		return FALSE;
	}

	switch (action) {
	case AXIS_IDENTIFY_CONFIRM:
	case AXIS_IDENTIFY_CHOOSE:
		if (action == AXIS_IDENTIFY_CONFIRM) {
			candidates = candidate_ids(axis_db_result_get_text(rows, 0, 2));
			for (i = 0; i < candidates->len && target == 0; i++) {
				if (!film_label(conn, g_array_index(candidates, gint64, i), kind, &label, error))
					return FALSE;
				if (label != NULL)
					target = g_array_index(candidates, gint64, i);
			}
			if (target == 0) {
				g_set_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID, "there is no candidate to confirm: "
					"choose or search for the %s", noun);
				return FALSE;
			}
		} else {
			if (title_id <= 0 || !film_label(conn, title_id, kind, &label, error) || label == NULL) {
				if (error != NULL && *error == NULL)
					g_set_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such %s", noun);
				return FALSE;
			}
			target = title_id;
		}
		/* A link a later look did not confirm can be kept: the person's
		 * choice of the title it has. */
		if (g_strcmp0(state, "linked") == 0 && current == target && !doubted) {
			g_set_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID, "it is linked to that %s already", noun);
			return FALSE;
		}
		target_text = axis_identify_int_text(target);
		params[1] = target_text;
		params[2] = action == AXIS_IDENTIFY_CONFIRM ? "confirmed" : "chosen";
		params[3] = user;
		if (!axis_db_conn_exec(conn, "UPDATE library_identity SET state = 'linked', title_id = $2::bigint,"
		        " linked_how = $3, decided_by = $4, decided_at = now(), held = true, problem = NULL, doubt = NULL,"
		        " retry_at = NULL, tries = 0 WHERE id = $1::bigint", 4, params, NULL, error) ||
		    !axis_identify_owner_link(self, kind, conn, unit_id, target, user, error))
			return FALSE;
		/* A series linked by a person: its root's run fetches the seasons
		 * it needs. */
		if (kind == AXIS_META_KIND_SERIES && !axis_identify_enqueue(conn, root_id, 5, NULL, error))
			return FALSE;
		text = g_strdup_printf("%s %s", action == AXIS_IDENTIFY_CONFIRM ? "Confirmed as" : "Linked to", label);
		break;
	case AXIS_IDENTIFY_NOT_FILM:
		if (g_strcmp0(state, "not_film") == 0) {
			g_set_error(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID, "it is marked as not a %s already",
				noun);
			return FALSE;
		}
		params[1] = user;
		if ((g_strcmp0(state, "linked") == 0 && !axis_identify_owner_unlink(self, kind, conn, unit_id, user, error)) ||
		    !axis_db_conn_exec(conn, "UPDATE library_identity SET state = 'not_film', title_id = NULL, linked_how = NULL,"
		        " decided_by = $2, decided_at = now(), held = true, doubt = NULL, retry_at = NULL, tries = 0"
		        " WHERE id = $1::bigint", 2, params, NULL, error))
			return FALSE;
		text = g_strdup_printf("Marked as not a %s", noun);
		break;
	default:
		if (g_strcmp0(state, "linked") != 0 && g_strcmp0(state, "not_film") != 0) {
			g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_INVALID, "there is nothing to undo: it "
				"is neither linked nor marked");
			return FALSE;
		}
		if (current > 0 && !film_label(conn, current, kind, &label, error))
			return FALSE;
		params[1] = user;
		if ((g_strcmp0(state, "linked") == 0 && !axis_identify_owner_unlink(self, kind, conn, unit_id, user, error)) ||
		    !axis_db_conn_exec(conn, "UPDATE library_identity SET state = 'review', title_id = NULL, linked_how = NULL,"
		        " decided_by = $2, decided_at = now(), held = true, doubt = NULL, retry_at = NULL, tries = 0"
		        " WHERE id = $1::bigint", 2, params, NULL, error))
			return FALSE;
		if (g_strcmp0(state, "linked") == 0)
			text = g_strdup_printf("Undid the link to %s; back to review", label != NULL ? label :
				kind == AXIS_META_KIND_SERIES ? "a series" : "a movie");
		else
			text = g_strdup_printf("Undid \"not a %s\"; back to review", noun);
		target = current;
		break;
	}
	return record_action(conn, unit_id, root_id, rel_path, axis_identify_action_name(action), target, user, text,
		error);
}

gboolean
axis_identify_act(
	AxisIdentify        *self,
	gint64               unit_id,
	AxisIdentifyAction   action,
	gint64               title_id,
	const gchar         *user,
	GError             **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(user != NULL && user[0] != '\0', FALSE);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && act_in_transaction(self, conn, unit_id, action, title_id, user, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_identify_add_all(
	AxisIdentify  *self,
	gint64         root_id,
	gint64         profile_id,
	gboolean       monitored,
	const gchar   *user,
	guint         *added,
	guint         *skipped,
	GError       **error
){
	g_autoptr(AxisDbResult) found = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *root = axis_identify_int_text(root_id);
	const AxisIdentifyOwnerInfo *owner;
	const gchar *params[1];
	gpointer data = NULL;
	AxisMetaKind kind;
	guint n_added = 0;
	guint n_skipped = 0;
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(user != NULL && user[0] != '\0', FALSE);

	if (added != NULL)
		*added = 0;
	if (skipped != NULL)
		*skipped = 0;
	params[0] = root;
	if (!axis_db_pool_exec(self->pool, "SELECT media_type FROM library_root WHERE id = $1::bigint", 1, params, &found,
	        error))
		return FALSE;
	kind = axis_db_result_get_n_rows(found) > 0 ? axis_identify_root_kind(axis_db_result_get_text(found, 0, 0)) :
		(AxisMetaKind)0;
	if (kind == 0) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND, "there is no such root of movies "
			"or for TV");
		return FALSE;
	}
	owner = axis_identify_owner(self, kind, &data);
	if (owner == NULL || owner->manage == NULL) {
		g_set_error_literal(error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_UNAVAILABLE, kind == AXIS_META_KIND_SERIES ?
			"nothing manages series: the TV module is off" : "nothing manages movies: the Movies module is off");
		return FALSE;
	}
	if (!owner->manage(data, root_id, profile_id, monitored, user, &n_added, &n_skipped, error))
		return FALSE;
	if (kind == AXIS_META_KIND_SERIES)
		text = g_strdup_printf("Added %u identified series (%u managed already), %s; nothing was searched for",
			n_added, n_skipped, monitored ? "monitoring future episodes" : "monitoring nothing yet");
	else
		text = g_strdup_printf("Added %u identified %s (%u managed already), %s; nothing was searched for", n_added,
			n_added == 1 ? "movie" : "movies", n_skipped, monitored ? "monitored for upgrades" : "not monitored");
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = record_action(conn, 0, root_id, NULL, "add_all", 0, user, text, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && added != NULL)
		*added = n_added;
	if (ok && skipped != NULL)
		*skipped = n_skipped;
	return ok;
}
