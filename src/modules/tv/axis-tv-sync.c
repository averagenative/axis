/*
 * axis-tv-sync.c - a series' seasons and episodes following the metadata
 * store
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An episode here is the metadata episode it is: the provider's own id
 * (meta_episode.provider_id), or, when the provider gives none, its place
 * (season and number).  On every sync:
 *
 *  - a metadata episode already known (by its id, else by its place) brings
 *    its row up to date: title, air date, running time, and its place when
 *    the provider numbered it anew; the row keeps its id, so its grabs, its
 *    files and its history stay with it;
 *  - one not known yet is added, monitored as its season is (as chosen when
 *    the series is being added);
 *  - a row the metadata no longer lists is marked gone (never deleted),
 *    unless its season's episodes were never fetched; it comes back with its
 *    links if the metadata lists it again.
 *
 * An id listed twice (an episode that moved between two seasons, listed in
 * both until the second is fetched again) is taken from the season fetched
 * last.  A season the metadata announces gets a row, monitored when the
 * series was added with all or future episodes.
 *
 * Only the source of numbering the series follows (series.episode_source,
 * add-metadata-tvdb) is read: its seasons and episodes, and the rows known
 * by that source.  A row of another source (kept, gone, by a change of the
 * series' numbering) is left as it is.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

/* An episode's row takes what the metadata says of it now ($2 to $8), and
 * is listed again if it was gone: written only where one of them differs. */
#define AXIS_TV_SYNC_SQL_EPISODE \
	"UPDATE series_episode SET provider_id = coalesce($2, provider_id)," \
	" season_number = $3::integer, number = $4::integer, absolute_number = $5::integer," \
	" title = left($6, 500), air_date = $7::date, runtime_minutes = $8::integer, gone_at = NULL" \
	" WHERE id = $9::bigint AND series_id = $1::bigint" \
	" AND (provider_id IS DISTINCT FROM coalesce($2, provider_id)" \
	"  OR season_number IS DISTINCT FROM $3::integer OR number IS DISTINCT FROM $4::integer" \
	"  OR absolute_number IS DISTINCT FROM $5::integer OR title IS DISTINCT FROM left($6, 500)" \
	"  OR air_date IS DISTINCT FROM $7::date OR runtime_minutes IS DISTINCT FROM $8::integer" \
	"  OR gone_at IS NOT NULL)"

typedef struct
{
	gint64   id;
	gchar   *provider_id;
	gint     season;
	gint     number;
	gboolean gone;
	gboolean seen;
} Row;

static void
row_free(Row *row)
{
	g_free(row->provider_id);
	g_free(row);
}

static gint64
get_int(
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

gboolean
axis_tv_monitor_takes_season(
	AxisTvMonitor monitor,
	gboolean      specials,
	gint          number,
	gint          first,
	gint          latest
){
	switch (monitor) {
	case AXIS_TV_MONITOR_ALL:
		return number > 0 || specials;
	case AXIS_TV_MONITOR_FUTURE:
		return number > 0;
	case AXIS_TV_MONITOR_LATEST:
		return number > 0 && number == latest;
	case AXIS_TV_MONITOR_FIRST:
		return number > 0 && number == first;
	default:
		return FALSE;
	}
}

/* Whether a season is monitored when it is first known. */
static gboolean
season_monitored(
	const AxisTvSeries *series,
	const AxisTvSpec   *adding,
	gint                number,
	gint                first,
	gint                latest
){
	if (adding == NULL)
		return series->monitor_new && number > 0;
	return axis_tv_monitor_takes_season(adding->monitor, adding->specials, number, first, latest);
}

gboolean
axis_tv_sync_in_transaction(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvSpec    *adding,
	guint               *changes_out,
	GError             **error
){
	return axis_tv_sync_rows(self, conn, series, adding, FALSE, changes_out, error);
}

gboolean
axis_tv_sync_rows(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvSpec    *adding,
	gboolean             quiet,
	guint               *changes_out,
	GError             **error
){
	g_autoptr(AxisDbResult) meta_seasons = NULL;
	g_autoptr(AxisDbResult) meta_episodes = NULL;
	g_autoptr(AxisDbResult) seasons = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GPtrArray) known = g_ptr_array_new_with_free_func((GDestroyNotify)row_free);
	g_autoptr(GHashTable) season_on = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) unfetched = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) ids_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) listed_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gboolean rekey;
	g_autoptr(GString) added = g_string_new(NULL);
	g_autoptr(GString) moved = g_string_new(NULL);
	g_autoptr(GString) went = g_string_new(NULL);
	g_autofree gchar *title = axis_tv_int_text(series->title_id);
	g_autofree gchar *series_id = axis_tv_int_text(series->id);
	AxisTvRules rules;
	const gchar *params[11];
	guint changes = 0;
	guint n_added = 0;
	gint first = G_MAXINT;
	gint latest = -1;
	guint i;
	guint k;

	if (changes_out != NULL)
		*changes_out = 0;
	axis_tv_get_rules(self, &rules);
	params[0] = title;
	params[1] = series->episode_source;
	if (!axis_db_conn_exec(conn, "SELECT number, episodes_fetched_at IS NOT NULL FROM meta_season"
	        " WHERE title_id = $1::bigint AND source = $2 ORDER BY number", 2, params, &meta_seasons, error) ||
	    !axis_db_conn_exec(conn, "SELECT me.season_number, me.number, me.absolute_number, me.name, me.air_date::text,"
	        " me.runtime_minutes, me.provider_id FROM meta_episode me JOIN meta_season ms ON ms.title_id = me.title_id"
	        " AND ms.source = me.source AND ms.number = me.season_number WHERE me.title_id = $1::bigint"
	        " AND me.source = $2"
	        " ORDER BY ms.episodes_fetched_at DESC NULLS LAST, me.season_number, me.number", 2, params, &meta_episodes,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(meta_seasons); i++) {
		gint number = (gint)get_int(meta_seasons, i, 0, 0);
		gboolean fetched = FALSE;

		axis_db_result_get_boolean(meta_seasons, i, 1, &fetched);
		if (!fetched)
			g_hash_table_add(unfetched, GINT_TO_POINTER(number + 1));
		if (number > 0) {
			first = MIN(first, number);
			latest = MAX(latest, number);
		}
	}

	/* The seasons. */
	params[0] = series_id;
	if (!axis_db_conn_exec(conn, "SELECT number, monitored FROM series_season WHERE series_id = $1::bigint", 1, params,
	        &seasons, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(seasons); i++) {
		gboolean on = FALSE;

		axis_db_result_get_boolean(seasons, i, 1, &on);
		g_hash_table_insert(season_on, GINT_TO_POINTER((gint)get_int(seasons, i, 0, 0) + 1), GINT_TO_POINTER(on ? 2 : 1));
	}
	for (i = 0; i < axis_db_result_get_n_rows(meta_seasons); i++) {
		gint number = (gint)get_int(meta_seasons, i, 0, 0);
		g_autofree gchar *number_text = NULL;
		gboolean on;

		if (g_hash_table_contains(season_on, GINT_TO_POINTER(number + 1)))
			continue;
		on = season_monitored(series, adding, number, first, latest);
		number_text = axis_tv_int_text(number);
		params[0] = series_id;
		params[1] = number_text;
		params[2] = on ? "true" : "false";
		if (!axis_db_conn_exec(conn, "INSERT INTO series_season (series_id, number, monitored) VALUES ($1::bigint,"
		        " $2::integer, $3::boolean) ON CONFLICT (series_id, number) DO NOTHING", 3, params, NULL, error))
			return FALSE;
		g_hash_table_insert(season_on, GINT_TO_POINTER(number + 1), GINT_TO_POINTER(on ? 2 : 1));
		if (adding == NULL)
			changes++;
	}

	/* The title's record changed since the rows last followed it (two of the
	 * provider's records merged): noted once, and the rows are keyed anew by
	 * their place (0059_series_record_id.sql). */
	rekey = series->synced_record_id != NULL && series->record_id != NULL &&
		strcmp(series->synced_record_id, series->record_id) != 0 &&
		g_strcmp0(series->episode_source, series->provider) == 0;
	if (rekey) {
		params[0] = series_id;
		if (!axis_db_conn_exec(conn, "UPDATE series SET record_changed_at = coalesce(record_changed_at, now())"
		        " WHERE id = $1::bigint", 1, params, NULL, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(meta_episodes); i++) {
			const gchar *listed = axis_db_result_get_text(meta_episodes, i, 6);

			if (listed != NULL)
				g_hash_table_add(listed_ids, g_strdup(listed));
		}
	}

	/* The episodes known now, by the source the series follows. */
	params[0] = series_id;
	params[1] = series->episode_source;
	if (!axis_db_conn_exec(conn, "SELECT id, provider_id, season_number, number, gone_at IS NOT NULL"
	        " FROM series_episode WHERE series_id = $1::bigint AND source = $2 ORDER BY id", 2, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		Row *row = g_new0(Row, 1);
		const gchar *id = axis_db_result_get_text(rows, i, 1);

		row->id = get_int(rows, i, 0, 0);
		row->provider_id = id != NULL ? g_strdup(id) : NULL;
		row->season = (gint)get_int(rows, i, 2, 0);
		row->number = (gint)get_int(rows, i, 3, 0);
		axis_db_result_get_boolean(rows, i, 4, &row->gone);
		g_ptr_array_add(known, row);
	}

	for (i = 0; i < axis_db_result_get_n_rows(meta_episodes); i++) {
		gint season = (gint)get_int(meta_episodes, i, 0, 0);
		gint number = (gint)get_int(meta_episodes, i, 1, 0);
		const gchar *provider_id = axis_db_result_get_text(meta_episodes, i, 6);
		g_autofree gchar *season_text = axis_tv_int_text(season);
		g_autofree gchar *number_text = axis_tv_int_text(number);
		Row *row = NULL;

		/* An id listed twice: the season fetched last has it. */
		if (provider_id != NULL) {
			if (g_hash_table_contains(ids_seen, provider_id))
				continue;
			g_hash_table_add(ids_seen, g_strdup(provider_id));
		}
		for (k = 0; k < known->len && row == NULL; k++) {
			Row *candidate = g_ptr_array_index(known, k);

			if (provider_id != NULL && g_strcmp0(candidate->provider_id, provider_id) == 0)
				row = candidate;
		}
		/* Known by its place (no id then, or none before): one listed now
		 * first, then (with no id) one marked gone, which comes back. */
		for (k = 0; k < known->len && row == NULL; k++) {
			Row *candidate = g_ptr_array_index(known, k);

			if (candidate->provider_id == NULL && !candidate->seen && !candidate->gone &&
			    candidate->season == season && candidate->number == number)
				row = candidate;
		}
		for (k = 0; k < known->len && row == NULL && provider_id == NULL; k++) {
			Row *candidate = g_ptr_array_index(known, k);

			if (candidate->provider_id == NULL && !candidate->seen && candidate->gone &&
			    candidate->season == season && candidate->number == number)
				row = candidate;
		}
		/* After a change of the title's record: the row at this place whose
		 * episode the new record does not list is this one, keyed anew. */
		for (k = 0; rekey && k < known->len && row == NULL && provider_id != NULL; k++) {
			Row *candidate = g_ptr_array_index(known, k);

			if (candidate->provider_id != NULL && !candidate->seen && !candidate->gone &&
			    candidate->season == season && candidate->number == number &&
			    !g_hash_table_contains(listed_ids, candidate->provider_id))
				row = candidate;
		}
		params[0] = series_id;
		params[1] = provider_id;
		params[2] = season_text;
		params[3] = number_text;
		params[4] = axis_db_result_get_text(meta_episodes, i, 2);
		params[5] = axis_db_result_get_text(meta_episodes, i, 3);
		params[6] = axis_db_result_get_text(meta_episodes, i, 4);
		params[7] = axis_db_result_get_text(meta_episodes, i, 5);
		if (row != NULL) {
			g_autofree gchar *id = axis_tv_int_text(row->id);

			row->seen = TRUE;
			if (row->season != season || row->number != number) {
				g_autofree gchar *was = axis_tv_episode_label(row->season, row->number);
				g_autofree gchar *now = axis_tv_episode_label(season, number);

				g_string_append_printf(moved, "%s%s is now %s", moved->len > 0 ? "; " : "", was, now);
				changes++;
			} else if (row->gone) {
				g_autofree gchar *now = axis_tv_episode_label(season, number);

				g_string_append_printf(moved, "%s%s is listed again", moved->len > 0 ? "; " : "", now);
				changes++;
			}
			params[8] = id;
			/* (Keyed anew, it takes the new record's id of its episode.)
			 * Only a row that differs is written: an episode the metadata
			 * says nothing new about, which is nearly every one at every
			 * sync, would otherwise leave a dead row behind each time, and
			 * another in series_episode_identity through its trigger. */
			if (!axis_db_conn_exec(conn, AXIS_TV_SYNC_SQL_EPISODE, 9, params, NULL, error))
				return FALSE;
			continue;
		}
		/* New: monitored as its season is (as chosen, when adding). */
		{
			gboolean on = GPOINTER_TO_INT(g_hash_table_lookup(season_on, GINT_TO_POINTER(season + 1))) == 2;
			g_autoptr(AxisDbResult) inserted = NULL;
			g_autofree gchar *floor = axis_tv_int_text(axis_tv_setting_duration(self, AXIS_TV_SETTING_FLOOR,
				6 * 3600));

			if (adding != NULL && adding->monitor == AXIS_TV_MONITOR_FUTURE) {
				const gchar *air = axis_db_result_get_text(meta_episodes, i, 4);
				gint64 start = axis_tv_date_us(air);

				on = season > 0 && (start == 0 || start + rules.air_delay_s * G_USEC_PER_SEC > rules.now_us);
			}
			params[8] = on ? "true" : "false";
			params[9] = floor;
			params[10] = series->episode_source;
			if (!axis_db_conn_exec(conn, "INSERT INTO series_episode (series_id, provider_id, season_number, number,"
			        " absolute_number, title, air_date, runtime_minutes, monitored, search_floor_s, source) VALUES"
			        " ($1::bigint, $2, $3::integer, $4::integer, $5::integer, left($6, 500), $7::date, $8::integer,"
			        " $9::boolean, $10::integer, $11) RETURNING id", 11, params, &inserted, error))
				return FALSE;
			if (adding == NULL) {
				g_autofree gchar *label = axis_tv_episode_label(season, number);

				if (n_added < 20)
					g_string_append_printf(added, "%s%s", added->len > 0 ? ", " : "", label);
				n_added++;
				changes++;
			}
		}
	}
	/* What the metadata no longer lists. */
	for (k = 0; k < known->len; k++) {
		Row *row = g_ptr_array_index(known, k);
		g_autofree gchar *id = NULL;

		if (row->seen || row->gone || g_hash_table_contains(unfetched, GINT_TO_POINTER(row->season + 1)))
			continue;
		id = axis_tv_int_text(row->id);
		params[0] = id;
		if (!axis_db_conn_exec(conn, "UPDATE series_episode SET gone_at = now() WHERE id = $1::bigint", 1, params, NULL,
		        error))
			return FALSE;
		{
			g_autofree gchar *label = axis_tv_episode_label(row->season, row->number);

			g_string_append_printf(went, "%s%s", went->len > 0 ? ", " : "", label);
		}
		changes++;
	}
	params[0] = series_id;
	if (!axis_db_conn_exec(conn, "UPDATE series SET synced_at = now() WHERE id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	/* The rows follow the title's record now: from the start, or once every
	 * season of the source was fetched after its change was seen. */
	params[1] = series->record_id;
	if (series->record_id != NULL && !axis_db_conn_exec(conn, "UPDATE series s SET record_id = $2,"
	        " record_changed_at = NULL WHERE s.id = $1::bigint AND (s.record_id IS NULL OR (s.record_id <> $2"
	        " AND NOT EXISTS (SELECT 1 FROM meta_season ms WHERE ms.title_id = s.title_id"
	        " AND ms.source = s.episode_source AND (ms.episodes_fetched_at IS NULL"
	        " OR ms.episodes_fetched_at < s.record_changed_at))))", 2, params, NULL, error))
		return FALSE;
	/* Episodes that appeared or moved: the files on disk identification
	 * found are placed on them again (adding links them itself; a change of
	 * numbering, quiet, places only the files no link holds). */
	if (adding == NULL && !quiet && changes > 0) {
		guint linked = 0;

		if (!axis_tv_link_identified(conn, series->id, series->title_id, &linked, error))
			return FALSE;
	}
	if (adding == NULL && !quiet && (added->len > 0 || moved->len > 0 || went->len > 0)) {
		g_autoptr(GString) text = g_string_new("The episode list changed:");

		if (added->len > 0)
			g_string_append_printf(text, " %u new %s (%s%s), monitored as their seasons are.", n_added,
				n_added == 1 ? "episode" : "episodes", added->str, n_added > 20 ? ", ..." : "");
		if (moved->len > 0)
			g_string_append_printf(text, " %s: each keeps its grabs, files and history.", moved->str);
		if (went->len > 0)
			g_string_append_printf(text, " No longer listed: %s (kept, never searched for).", went->str);
		if (!axis_tv_store_eventf(conn, series, 0, "episodes", NULL, error, "%s", text->str))
			return FALSE;
	}
	if (changes_out != NULL)
		*changes_out = changes;
	return TRUE;
}

void
axis_tv_fetch_seasons(
	AxisTv      *self,
	gint64       title_id,
	const gchar *source,
	guint        wait_ms
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title = axis_tv_int_text(title_id);
	g_autofree gchar *owner = source != NULL ? axis_meta_source_provider(source) : NULL;
	const AxisMetaProviderInfo *info = owner != NULL ? axis_meta_provider_find(self->meta, owner) : NULL;
	const gchar *params[2];
	guint i;

	/* A source a provider numbers in several orders comes whole: asking for
	 * one of its seasons fetches them all, when they are not fresh. */
	if (info != NULL && info->order != NULL) {
		AxisMetaSeasonAnswer answer;
		g_autoptr(GError) local = NULL;

		memset(&answer, 0, sizeof(answer));
		if (!axis_meta_get_source_season(self->meta, title_id, source, 1, wait_ms, &answer, &local))
			g_debug("series: an order could not be fetched: %s", local->message);
		axis_meta_season_answer_clear(&answer);
		return;
	}
	params[0] = title;
	params[1] = source;
	if (!axis_db_pool_exec(self->pool, "SELECT s.number FROM meta_season s JOIN meta_title t ON t.id = s.title_id"
	        " WHERE s.title_id = $1::bigint AND s.source = coalesce($2, t.provider)"
	        " AND s.episodes_fetched_at IS NULL ORDER BY s.number DESC LIMIT 60", 2, params, &rows, &error)) {
		g_message("series: the seasons to fetch could not be read: %s", error->message);
		return;
	}
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisMetaSeasonAnswer answer;
		g_autoptr(GError) local = NULL;

		memset(&answer, 0, sizeof(answer));
		if (!axis_meta_get_source_season(self->meta, title_id, source, (gint)get_int(rows, i, 0, 0), wait_ms, &answer,
		        &local))
			g_debug("series: a season could not be fetched: %s", local->message);
		axis_meta_season_answer_clear(&answer);
	}
}

gboolean
axis_tv_sync(
	AxisTv   *self,
	gint64    series_id,
	gboolean  fetch,
	guint    *changes,
	GError  **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean ok;

	if (changes != NULL)
		*changes = 0;
	if (fetch) {
		conn = axis_db_pool_acquire(self->pool, error);
		if (conn == NULL)
			return FALSE;
		ok = axis_tv_store_series_one(conn, series_id, &series, error);
		axis_db_pool_release(self->pool, conn);
		if (!ok)
			return FALSE;
		if (series == NULL) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
			return FALSE;
		}
		axis_tv_fetch_seasons(self, series->title_id, series->episode_source, AXIS_TV_META_WAIT_MS);
		g_clear_pointer(&series, axis_tv_series_free);
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	ok = ok && axis_tv_store_series_one(conn, series_id, &series, error) && series != NULL &&
		axis_tv_sync_in_transaction(self, conn, series, NULL, changes, error) && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}
