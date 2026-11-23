/*
 * axis-tv-library.c - the series on disk that Axis does not manage: their
 * facts, their counts and their page, read at query time from
 * identification's links, the file index and the metadata
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A series on disk is a title of kind series that identification has
 * linked (in a root for TV) to a folder holding videos that are present,
 * and that no series row manages.  Nothing is written for it: it has no
 * profile and no episodes of its own and is not monitored, so the
 * scheduler, the follow-up, the search and the calendar never see it, and
 * no file of it is renamed, moved, upgraded or touched.  It is one entry
 * whatever the number of its folders and disks.
 *
 * Which episodes are on disk is read when shown: each file's own numbers
 * (or its date), as identification read them, against the series' episodes
 * as the metadata has them now (the view library_episode_file), so a season
 * fetched later places its files at once.  A file that cannot be placed is
 * listed with its reason, never dropped.
 */

#include "modules/tv/axis-tv-private.h"

#include <stdio.h>
#include <string.h>

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* The volume states a disk is usable in, as a text[] ("{online,...}"). */
static gchar *
usable_states(void)
{
	GString *states = g_string_new("{");
	guint i;

	for (i = AXIS_VOLUME_ONLINE; i <= AXIS_VOLUME_MISMATCH; i++) {
		if (axis_volume_state_is_usable((AxisVolumeState)i))
			g_string_append_printf(states, "%s%s", states->len > 1 ? "," : "",
				axis_volume_state_to_string((AxisVolumeState)i));
	}
	g_string_append_c(states, '}');
	return g_string_free(states, FALSE);
}

static gchar *
row_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static gint64
row_int(
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
row_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── Freeing and words ──────────────────────────────────────────── */

void
axis_tv_on_disk_free(AxisTvOnDisk *disk)
{
	if (disk == NULL)
		return;
	g_free(disk->title);
	g_free(disk->original_title);
	g_free(disk->poster_ref);
	g_free(disk->provider);
	g_free(disk->away_volume);
	g_free(disk->root_name);
	g_free(disk);
}

gchar *
axis_tv_on_disk_episodes_text(const AxisTvOnDisk *disk)
{
	if (!disk->counted)
		return g_strdup_printf("%u %s on disk", disk->n_files, disk->n_files == 1 ? "file" : "files");
	if (disk->episodes == 0 && disk->on_disk == 0)
		return g_strdup(disk->seasons_unknown > 0 ? "Episodes not known yet" : "No episode known yet");
	if (disk->seasons_unknown > 0)
		return g_strdup_printf("%u of at least %u aired episodes on disk", disk->on_disk, disk->episodes);
	return g_strdup_printf("%u of %u aired %s on disk", disk->on_disk, disk->episodes,
		disk->episodes == 1 ? "episode" : "episodes");
}

void
axis_tv_disk_file_free(AxisTvDiskFile *file)
{
	if (file == NULL)
		return;
	g_free(file->rel_path);
	g_free(file->root_name);
	g_free(file->volume_name);
	g_free(file->quality);
	g_free(file->resolution);
	g_free(file->quality_text);
	g_free(file->holds);
	g_free(file->reason);
	g_free(file);
}

const gchar *
axis_tv_disk_file_quality(const AxisTvDiskFile *file)
{
	AxisQuality quality = AXIS_QUALITY_UNKNOWN;
	AxisReleaseResolution r;

	if (file->quality != NULL && axis_quality_from_name(file->quality, &quality) && quality != AXIS_QUALITY_UNKNOWN)
		return axis_quality_label(quality);
	for (r = AXIS_RELEASE_RESOLUTION_480P; file->resolution != NULL && r <= AXIS_RELEASE_RESOLUTION_2160P; r++) {
		if (g_strcmp0(axis_release_resolution_name(r), file->resolution) == 0)
			return axis_quality_unknown_source_label(r);
	}
	return "Unknown";
}

void
axis_tv_disk_episode_free(AxisTvDiskEpisode *episode)
{
	if (episode == NULL)
		return;
	g_free(episode->title);
	g_free(episode->air_date);
	g_clear_pointer(&episode->files, g_ptr_array_unref);
	g_free(episode);
}

void
axis_tv_disk_folder_free(AxisTvDiskFolder *folder)
{
	if (folder == NULL)
		return;
	g_free(folder->root_name);
	g_free(folder->rel_path);
	g_free(folder->volume_name);
	g_free(folder->linked_how);
	g_free(folder);
}

void
axis_tv_disk_detail_free(AxisTvDiskDetail *detail)
{
	if (detail == NULL)
		return;
	axis_tv_on_disk_free(detail->facts);
	g_clear_pointer(&detail->episodes, g_ptr_array_unref);
	g_clear_pointer(&detail->unplaced, g_ptr_array_unref);
	g_clear_pointer(&detail->files, g_ptr_array_unref);
	g_clear_pointer(&detail->folders, g_ptr_array_unref);
	g_clear_pointer(&detail->seasons_unknown, g_array_unref);
	g_free(detail);
}

/* ── The series on disk ─────────────────────────────────────────── */

enum
{
	D_TITLE_ID, D_TITLE, D_ORIGINAL, D_YEAR, D_STATUS, D_POSTER, D_PROVIDER, D_FOUND, D_FOLDERS, D_FILES, D_SIZE,
	D_AWAY, D_AWAY_VOLUME, D_ROOT_ID, D_ROOT_NAME
};

/* %s: the condition on the units' titles (or nothing). */
#define ON_DISK_SELECT \
	"SELECT t.id, t.title, t.original_title, t.year, t.status, t.poster_ref, t.provider, " US("d.found_at") "," \
	" d.folders, d.files, d.size, d.away, d.away_volume, d.root_id, r.name" \
	" FROM (SELECT li.title_id, min(coalesce(li.decided_at, li.created_at)) AS found_at," \
	"   count(DISTINCT li.id) AS folders, count(*) AS files, coalesce(sum(llf.size), 0) AS size," \
	"   count(*) FILTER (WHERE NOT coalesce(lv.state = ANY ($2::text[]), false)) AS away," \
	"   min(lv.name) FILTER (WHERE NOT coalesce(lv.state = ANY ($2::text[]), false)) AS away_volume," \
	"   min(li.root_id) AS root_id" \
	"   FROM" AXIS_TV_ON_DISK_TABLES " LEFT JOIN volume lv ON lv.id = llf.volume_id" AXIS_TV_ON_DISK_WHERE "%s" \
	"   GROUP BY li.title_id) d" \
	" JOIN meta_title t ON t.id = d.title_id AND t.kind = 'series'" \
	" LEFT JOIN library_root r ON r.id = d.root_id" \
	" WHERE ($3::text IS NULL OR t.name_keys LIKE '%%' || $3::text || '%%')" \
	" ORDER BY lower(t.title), t.year, t.id"

static AxisTvOnDisk *
on_disk_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisTvOnDisk *d = g_new0(AxisTvOnDisk, 1);

	d->title_id = row_int(result, row, D_TITLE_ID, 0);
	d->title = row_text(result, row, D_TITLE);
	d->original_title = row_text(result, row, D_ORIGINAL);
	d->year = (gint)row_int(result, row, D_YEAR, 0);
	d->status = axis_meta_status_from_string(axis_db_result_get_text(result, row, D_STATUS));
	d->poster_ref = row_text(result, row, D_POSTER);
	d->provider = row_text(result, row, D_PROVIDER);
	d->found_us = row_int(result, row, D_FOUND, 0);
	d->n_folders = (guint)row_int(result, row, D_FOLDERS, 0);
	d->n_files = (guint)row_int(result, row, D_FILES, 0);
	d->size = row_int(result, row, D_SIZE, 0);
	d->n_away = (guint)row_int(result, row, D_AWAY, 0);
	d->away_volume = row_text(result, row, D_AWAY_VOLUME);
	d->root_id = row_int(result, row, D_ROOT_ID, 0);
	d->root_name = row_text(result, row, D_ROOT_NAME);
	return d;
}

gboolean
axis_tv_store_on_disk(
	AxisDbConn   *conn,
	const gchar  *titles,
	const gchar  *name_key,
	GPtrArray   **series,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *usable = usable_states();
	g_autofree gchar *sql = g_strdup_printf(ON_DISK_SELECT, titles != NULL ? " AND li.title_id = ANY ($1::bigint[])" :
		" AND $1::bigint[] IS NULL");
	const gchar *params[3];
	guint i;

	params[0] = titles;
	params[1] = usable;
	params[2] = name_key != NULL && name_key[0] != '\0' ? name_key : NULL;
	if (!axis_db_conn_exec(conn, sql, 3, params, &result, error))
		return FALSE;
	*series = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_on_disk_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*series, on_disk_from_row(result, i));
	return TRUE;
}

/*
 * For each series of $1: its regular episodes aired by $2 (a date), its
 * episodes on disk (regular ones, specials, those with more than one file,
 * and the regular ones not aired yet or with no date), its seasons not
 * fetched yet, and its files that hold no episode of its data.
 */
static const gchar counts_select[] =
	"SELECT t.id,"
	" (SELECT count(*) FROM meta_episode_followed me WHERE me.title_id = t.id AND me.season_number > 0"
	"   AND me.air_date <= $2::date),"
	" coalesce(p.on_disk, 0), coalesce(p.specials, 0), coalesce(p.copies, 0), coalesce(p.beyond, 0),"
	" (SELECT count(*) FROM meta_season_followed ms WHERE ms.title_id = t.id AND ms.number > 0"
	"   AND ms.episodes_fetched_at IS NULL),"
	" (SELECT count(*) FROM" AXIS_TV_ON_DISK_TABLES " WHERE li.state = 'linked' AND li.title_id = t.id"
	"   AND NOT EXISTS (SELECT 1 FROM library_episode_file v WHERE v.file_id = llf.id AND v.title_id = t.id))"
	" FROM unnest ($1::bigint[]) AS t (id)"
	" LEFT JOIN LATERAL (SELECT count(*) FILTER (WHERE x.season_number > 0) AS on_disk,"
	"   count(*) FILTER (WHERE x.season_number = 0) AS specials, count(*) FILTER (WHERE x.n > 1) AS copies,"
	"   count(*) FILTER (WHERE x.season_number > 0 AND NOT x.aired) AS beyond"
	"   FROM (SELECT v.season_number, v.number, count(DISTINCT v.file_id) AS n,"
	"     bool_or(coalesce(me.air_date <= $2::date, false)) AS aired"
	"     FROM library_episode_file v JOIN meta_episode_followed me ON me.title_id = v.title_id"
	"     AND me.season_number = v.season_number AND me.number = v.number"
	"     WHERE v.title_id = t.id GROUP BY v.season_number, v.number) x) p ON true";

gboolean
axis_tv_store_on_disk_counts(
	AxisDbConn   *conn,
	GPtrArray    *series,
	const gchar  *today,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) ids = g_string_new("{");
	const gchar *params[2];
	guint i;
	guint k;

	if (series->len == 0)
		return TRUE;
	for (i = 0; i < series->len; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			((AxisTvOnDisk *)g_ptr_array_index(series, i))->title_id);
	g_string_append_c(ids, '}');
	params[0] = ids->str;
	params[1] = today;
	if (!axis_db_conn_exec(conn, counts_select, 2, params, &result, error))
		return FALSE;
	for (k = 0; k < axis_db_result_get_n_rows(result); k++) {
		gint64 title = row_int(result, k, 0, 0);

		for (i = 0; i < series->len; i++) {
			AxisTvOnDisk *d = g_ptr_array_index(series, i);

			if (d->title_id != title)
				continue;
			d->counted = TRUE;
			d->on_disk = (guint)row_int(result, k, 2, 0);
			d->episodes = (guint)row_int(result, k, 1, 0) + (guint)row_int(result, k, 5, 0);
			d->specials = (guint)row_int(result, k, 3, 0);
			d->copies = (guint)row_int(result, k, 4, 0);
			d->seasons_unknown = (guint)row_int(result, k, 6, 0);
			d->unplaced = (guint)row_int(result, k, 7, 0);
		}
	}
	return TRUE;
}

gboolean
axis_tv_explain_on_disk(
	AxisTv       *self,
	const gchar  *titles,
	gchar       **plan,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *usable = usable_states();
	g_autofree gchar *sql = NULL;
	AxisTvRules rules;
	const gchar *params[3];

	*plan = NULL;
	axis_tv_get_rules(self, &rules);
	if (titles == NULL) {
		g_autofree gchar *list = g_strdup_printf(ON_DISK_SELECT, " AND $1::bigint[] IS NULL");

		sql = g_strconcat("EXPLAIN (ANALYZE, FORMAT JSON) ", list, NULL);
		params[0] = NULL;
		params[1] = usable;
		params[2] = NULL;
		if (!axis_db_pool_exec(self->pool, sql, 3, params, &result, error))
			return FALSE;
	} else {
		sql = g_strconcat("EXPLAIN (ANALYZE, FORMAT JSON) ", counts_select, NULL);
		params[0] = titles;
		params[1] = rules.today;
		if (!axis_db_pool_exec(self->pool, sql, 2, params, &result, error))
			return FALSE;
	}
	*plan = row_text(result, 0, 0);
	return TRUE;
}

gboolean
axis_tv_store_held(
	AxisDbConn  *conn,
	guint       *review,
	guint       *waiting,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;

	*review = 0;
	*waiting = 0;
	if (!axis_db_conn_exec(conn, "SELECT count(*) FILTER (WHERE i.state = 'review'),"
	        " count(*) FILTER (WHERE i.state = 'waiting') FROM library_identity i JOIN library_root r ON r.id = i.root_id"
	        " WHERE r.media_type = 'tv' AND i.state IN ('review', 'waiting')", 0, NULL, &result, error))
		return FALSE;
	*review = (guint)MAX(row_int(result, 0, 0, 0), 0);
	*waiting = (guint)MAX(row_int(result, 0, 1, 0), 0);
	return TRUE;
}

gboolean
axis_tv_store_is_on_disk(
	AxisDbConn  *conn,
	gint64       title_id,
	gboolean    *on_disk,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *title = axis_tv_int_text(title_id);
	const gchar *params[1];

	*on_disk = FALSE;
	params[0] = title;
	if (!axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM" AXIS_TV_ON_DISK_TABLES AXIS_TV_ON_DISK_WHERE
	        " AND li.title_id = $1::bigint)", 1, params, &result, error))
		return FALSE;
	*on_disk = row_bool(result, 0, 0);
	return TRUE;
}

gboolean
axis_tv_get_on_disk_by_titles(
	AxisTv      *self,
	GArray      *title_ids,
	GPtrArray  **series,
	GError     **error
){
	g_autoptr(GString) array = g_string_new("{");
	AxisTvRules rules;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && title_ids != NULL && series != NULL, FALSE);

	*series = NULL;
	for (i = 0; i < title_ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(title_ids, gint64, i));
	g_string_append_c(array, '}');
	axis_tv_get_rules(self, &rules);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_tv_store_on_disk(conn, array->str, NULL, series, error) &&
		axis_tv_store_on_disk_counts(conn, *series, rules.today, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		g_clear_pointer(series, g_ptr_array_unref);
	return ok;
}

/* ── One series' page ───────────────────────────────────────────── */

/* Every present video of the series' folders on disk, with what its name
 * holds as read ("season:number:date", in its order), the episodes of the
 * series' data it is placed on ("season:number"), and whether the title its
 * own name states (if any) is one of the series' names. */
static const gchar files_select[] =
	"SELECT lf.id, li.id, lf.rel_path, r.name, v.name, NOT coalesce(v.state = ANY ($2::text[]), false), lf.size,"
	" llif.quality, llif.resolution, llif.quality_text, llif.episode_code, llif.episode_text, llif.name_title,"
	" (llif.title_key IS NULL OR position('|' || llif.title_key || '|' IN '|' || coalesce(t.name_keys, '') || '|') > 0),"
	" p.placed,"
	" (SELECT string_agg(coalesce(e.season_number::text, '') || ':' || coalesce(e.number::text, '') || ':'"
	"   || coalesce(e.air_date::text, ''), ',' ORDER BY e.position) FROM library_identity_episode e"
	"   WHERE e.file_id = lf.id)"
	" FROM library_identity li JOIN library_root r ON r.id = li.root_id AND r.media_type = 'tv'"
	" JOIN meta_title t ON t.id = li.title_id"
	" JOIN library_identity_file llif ON llif.identity_id = li.id"
	" JOIN library_file lf ON lf.id = llif.file_id AND lf.state = 'present'"
	" JOIN volume v ON v.id = lf.volume_id"
	" LEFT JOIN (SELECT pv.file_id, string_agg(pv.season_number || ':' || pv.number, ',') AS placed"
	"   FROM library_episode_file pv WHERE pv.title_id = $1::bigint GROUP BY pv.file_id) p ON p.file_id = lf.id"
	" WHERE li.state = 'linked' AND li.title_id = $1::bigint ORDER BY lf.rel_path, lf.id LIMIT 20000";

static const gchar folders_select[] =
	"SELECT li.id, r.name, li.rel_path, v.name, NOT coalesce(v.state = ANY ($2::text[]), false), li.linked_how,"
	" (SELECT count(*) FROM library_identity_file lif JOIN library_file lf ON lf.id = lif.file_id"
	"   AND lf.state = 'present' WHERE lif.identity_id = li.id)"
	" FROM library_identity li JOIN library_root r ON r.id = li.root_id AND r.media_type = 'tv'"
	" JOIN volume v ON v.id = r.volume_id"
	" WHERE li.title_id = $1::bigint AND li.state = 'linked' ORDER BY lower(r.name), li.rel_path, li.id";

static AxisTvDiskEpisode *
episode_at(
	GPtrArray *episodes,
	gint       season,
	gint       number
){
	guint i;

	for (i = 0; i < episodes->len; i++) {
		AxisTvDiskEpisode *e = g_ptr_array_index(episodes, i);

		if (e->season == season && e->number == number)
			return e;
	}
	return NULL;
}

static gboolean
season_known(
	const GArray *unknown,
	gint          season
){
	guint i;

	for (i = 0; i < unknown->len; i++) {
		if (g_array_index(unknown, gint, i) == season)
			return FALSE;
	}
	return TRUE;
}

/*
 * Why a file that holds episodes by its name is on none of the series' data:
 * the season's episodes are not fetched yet, the data has no such episode,
 * or (a date) no episode or several aired that day.  Never a guess.
 */
static gchar *
unplaced_reason(
	const gchar      *read,
	GPtrArray        *episodes,
	const GArray     *unknown,
	const GHashTable *known_seasons
){
	g_auto(GStrv) places = read != NULL ? g_strsplit(read, ",", 201) : NULL;
	g_autoptr(GString) missing = g_string_new(NULL);
	guint i;
	guint k;

	if (places == NULL || places[0] == NULL)
		return g_strdup("its name holds no episode Axis could place");
	for (i = 0; places[i] != NULL; i++) {
		g_auto(GStrv) parts = g_strsplit(places[i], ":", 3);
		gint64 season = -1;
		gint64 number = -1;

		if (g_strv_length(parts) != 3)
			continue;
		if (parts[2][0] != '\0') {
			guint aired = 0;

			for (k = 0; k < episodes->len; k++) {
				const AxisTvDiskEpisode *e = g_ptr_array_index(episodes, k);

				if (g_strcmp0(e->air_date, parts[2]) == 0)
					aired++;
			}
			if (aired == 0)
				return unknown->len > 0 ? g_strdup_printf("it is dated %s, and no episode of the series known so far "
					"aired that day (some of its seasons are not known yet)", parts[2]) :
					g_strdup_printf("it is dated %s, and no episode of the series aired that day", parts[2]);
			return g_strdup_printf("%u episodes of the series aired on %s: it is not certainly one of them", aired,
				parts[2]);
		}
		if (!g_ascii_string_to_signed(parts[0], 10, 0, 10000, &season, NULL) ||
		    !g_ascii_string_to_signed(parts[1], 10, 0, 100000, &number, NULL))
			continue;
		if (!season_known(unknown, (gint)season) ||
		    !g_hash_table_contains((GHashTable *)known_seasons, GINT_TO_POINTER((gint)season + 1)))
			return g_strdup_printf("season %" G_GINT64_FORMAT "'s episodes are not known yet: it is placed once they "
				"are fetched", season);
		if (episode_at(episodes, (gint)season, (gint)number) == NULL) {
			g_autofree gchar *label = axis_tv_episode_label((gint)season, (gint)number);

			g_string_append_printf(missing, "%s%s", missing->len > 0 ? ", " : "", label);
		}
	}
	if (missing->len > 0)
		return g_strdup_printf("it is numbered %s, and there is no such episode in the episode list (release names "
			"and file names often follow another numbering than TMDb's)", missing->str);
	return g_strdup("its name holds no episode Axis could place");
}

gboolean
axis_tv_get_disk_detail(
	AxisTv             *self,
	gint64              title_id,
	AxisTvDiskDetail  **detail_out,
	GError            **error
){
	g_autoptr(AxisTvDiskDetail) detail = g_new0(AxisTvDiskDetail, 1);
	g_autoptr(GPtrArray) facts = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisDbResult) episodes = NULL;
	g_autoptr(AxisDbResult) seasons = NULL;
	g_autoptr(AxisDbResult) folders = NULL;
	g_autoptr(GHashTable) known_seasons = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autofree gchar *titles = g_strdup_printf("{%" G_GINT64_FORMAT "}", title_id);
	g_autofree gchar *title = axis_tv_int_text(title_id);
	g_autofree gchar *usable = usable_states();
	const gchar *params[2];
	AxisTvRules rules;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && detail_out != NULL, FALSE);

	*detail_out = NULL;
	axis_tv_get_rules(self, &rules);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = title;
	params[1] = usable;
	ok = axis_tv_store_on_disk(conn, titles, NULL, &facts, error) &&
		axis_tv_store_on_disk_counts(conn, facts, rules.today, error) &&
		axis_db_conn_exec(conn, files_select, 2, params, &files, error) &&
		axis_db_conn_exec(conn, "SELECT season_number, number, name, air_date::text, coalesce(runtime_minutes, 0)"
			" FROM meta_episode_followed WHERE title_id = $1::bigint ORDER BY season_number, number", 1, params,
			&episodes, error) &&
		axis_db_conn_exec(conn, "SELECT number, episodes_fetched_at IS NOT NULL FROM meta_season_followed"
			" WHERE title_id = $1::bigint ORDER BY number", 1, params, &seasons, error) &&
		axis_db_conn_exec(conn, folders_select, 2, params, &folders, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (facts->len > 0)
		detail->facts = g_ptr_array_steal_index(facts, 0);

	detail->seasons_unknown = g_array_new(FALSE, FALSE, sizeof(gint));
	for (i = 0; i < axis_db_result_get_n_rows(seasons); i++) {
		gint number = (gint)row_int(seasons, i, 0, 0);

		g_hash_table_add(known_seasons, GINT_TO_POINTER(number + 1));
		if (!row_bool(seasons, i, 1))
			g_array_append_val(detail->seasons_unknown, number);
	}
	detail->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_disk_episode_free);
	for (i = 0; i < axis_db_result_get_n_rows(episodes); i++) {
		AxisTvDiskEpisode *e = g_new0(AxisTvDiskEpisode, 1);

		e->season = (gint)row_int(episodes, i, 0, 0);
		e->number = (gint)row_int(episodes, i, 1, 0);
		e->title = row_text(episodes, i, 2);
		e->air_date = row_text(episodes, i, 3);
		e->runtime = (gint)row_int(episodes, i, 4, 0);
		e->files = g_ptr_array_new();
		g_ptr_array_add(detail->episodes, e);
	}
	detail->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_disk_file_free);
	detail->unplaced = g_ptr_array_new();
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		AxisTvDiskFile *f = g_new0(AxisTvDiskFile, 1);
		const gchar *code = axis_db_result_get_text(files, i, 10);
		const gchar *placed = axis_db_result_get_text(files, i, 14);
		gboolean fits = row_bool(files, i, 13);

		f->file_id = row_int(files, i, 0, 0);
		f->identity_id = row_int(files, i, 1, 0);
		f->rel_path = row_text(files, i, 2);
		f->root_name = row_text(files, i, 3);
		f->volume_name = row_text(files, i, 4);
		f->away = row_bool(files, i, 5);
		f->size = row_int(files, i, 6, 0);
		f->quality = row_text(files, i, 7);
		f->resolution = row_text(files, i, 8);
		f->quality_text = row_text(files, i, 9);
		f->holds = code == NULL ? row_text(files, i, 11) : NULL;
		g_ptr_array_add(detail->files, f);
		if (placed != NULL) {
			g_auto(GStrv) places = g_strsplit(placed, ",", 201);
			guint k;

			for (k = 0; places[k] != NULL; k++) {
				gint season = 0;
				gint number = 0;
				AxisTvDiskEpisode *e;

				if (sscanf(places[k], "%d:%d", &season, &number) != 2)
					continue;
				e = episode_at(detail->episodes, season, number);
				if (e != NULL)
					g_ptr_array_add(e->files, f);
			}
			continue;
		}
		/* Not placed: why, in words. */
		if (code != NULL)
			f->reason = row_text(files, i, 11);
		else if (!fits)
			f->reason = g_strdup_printf("its name says \"%s\", which is not one of this series' names",
				axis_db_result_get_text(files, i, 12) != NULL ? axis_db_result_get_text(files, i, 12) : "another title");
		else
			f->reason = unplaced_reason(axis_db_result_get_text(files, i, 15), detail->episodes, detail->seasons_unknown,
				known_seasons);
		g_ptr_array_add(detail->unplaced, f);
	}
	detail->folders = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_disk_folder_free);
	for (i = 0; i < axis_db_result_get_n_rows(folders); i++) {
		AxisTvDiskFolder *folder = g_new0(AxisTvDiskFolder, 1);

		folder->identity_id = row_int(folders, i, 0, 0);
		folder->root_name = row_text(folders, i, 1);
		folder->rel_path = row_text(folders, i, 2);
		folder->volume_name = row_text(folders, i, 3);
		folder->away = row_bool(folders, i, 4);
		folder->linked_how = row_text(folders, i, 5);
		folder->n_files = (guint)row_int(folders, i, 6, 0);
		g_ptr_array_add(detail->folders, folder);
	}
	*detail_out = g_steal_pointer(&detail);
	return TRUE;
}
