/*
 * axis-tv-identify.c - the series' side of identification: a managed
 * series' links to the episode files identified as it, made when a folder
 * is linked, when the series is added and when its episodes follow the
 * metadata, dropped when the link is undone; and adding every identified
 * series of a root at once
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A link made from an identity is a series_file row with its identity_id
 * and a series_file_episode row for each episode of the series it holds:
 * the file's own numbers (or its date) put against the series' episodes by
 * library_episode_file (0045_identify_series.sql), as the pages read them.
 * An import's link of the same file (import_id) always wins: identification
 * never replaces it, and the import pipeline replaces an identification's.
 * An episode with a linked file that is present is in the library, so it is
 * not wanted; adding series here never queues a search.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

#include "library/axis-library-name.h"

/* The series of @title_id, when it is managed: its id and label. */
static gboolean
series_of_title(
	AxisDbConn  *conn,
	gint64       title_id,
	gint64      *series_id,
	gchar      **label,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *title = axis_tv_int_text(title_id);
	const gchar *params[1];
	gint64 year = 0;

	*series_id = 0;
	*label = NULL;
	params[0] = title;
	if (!axis_db_conn_exec(conn, "SELECT s.id, t.title, t.year FROM series s JOIN meta_title t ON t.id = s.title_id"
	        " WHERE s.title_id = $1::bigint", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	axis_db_result_get_int64(rows, 0, 0, series_id);
	axis_db_result_get_int64(rows, 0, 2, &year);
	*label = axis_tv_series_label(axis_db_result_get_text(rows, 0, 1), (gint)year);
	return TRUE;
}

/*
 * The links of the series @series_ids (a bigint[]) to their files placed on
 * their episodes, of the identity @identity_id alone (0: of every one); with
 * @replace, the links made from those identities before go first, but for a
 * file its name places on no episode now whose identity is still linked and
 * which is present: that one stays on the episode it was on (a change of
 * numbering, or a renumbering, that loses its name's episode never takes the
 * file from its episode, add-metadata-tvdb).  A link whose file's name was
 * read under another numbering than the series follows now (it changed its
 * numbering since, series_file.read_source) is never moved or removed: the
 * name is in the old numbering, and the change of numbering moved the
 * episode the file is on.  With @only_unlinked, only files no link holds
 * are placed (what a change of numbering does).  Three statements whatever
 * the number of files.  *@files: files linked.
 */
static gboolean
link_placed(
	AxisDbConn   *conn,
	const gchar  *series_ids,
	gint64        identity_id,
	gboolean      replace,
	gboolean      only_unlinked,
	guint        *files,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *identity = axis_tv_int_text(identity_id);
	const gchar *params[3];
	gint64 n = 0;

	*files = 0;
	params[0] = series_ids;
	params[1] = identity;
	params[2] = only_unlinked ? "true" : "false";
	if (replace && !axis_db_conn_exec(conn, "DELETE FROM series_file sf WHERE sf.series_id = ANY ($1::bigint[])"
	        " AND sf.identity_id IS NOT NULL AND sf.hand_at IS NULL AND ($2::bigint = 0 OR sf.identity_id = $2::bigint)"
	        " AND sf.read_source IS NOT DISTINCT FROM (SELECT rs.episode_source FROM series rs WHERE rs.id = sf.series_id)"
	        " AND (EXISTS (SELECT 1 FROM series s JOIN library_episode_file v ON v.title_id = s.title_id"
	        "   AND v.identity_id = sf.identity_id JOIN series_episode e ON e.series_id = s.id AND e.gone_at IS NULL"
	        "   AND e.season_number = v.season_number AND e.number = v.number WHERE s.id = sf.series_id"
	        "   AND v.file_id = sf.file_id)"
	        " OR NOT EXISTS (SELECT 1 FROM library_identity li JOIN library_identity_file lif ON lif.identity_id = li.id"
	        "   JOIN library_file lf ON lf.id = lif.file_id AND lf.state = 'present' JOIN series s ON s.id = sf.series_id"
	        "   AND s.title_id = li.title_id WHERE li.id = sf.identity_id AND li.state = 'linked'"
	        "   AND lif.file_id = sf.file_id))", 2, params, NULL, error))
		return FALSE;
	if (!axis_db_conn_exec(conn, "WITH placed AS (SELECT DISTINCT v.file_id, v.identity_id, s.id AS series_id,"
	        "   e.id AS episode_id FROM series s JOIN library_episode_file v ON v.title_id = s.title_id"
	        "   JOIN series_episode e ON e.series_id = s.id AND e.gone_at IS NULL AND e.season_number = v.season_number"
	        "   AND e.number = v.number"
	        "   WHERE s.id = ANY ($1::bigint[]) AND ($2::bigint = 0 OR v.identity_id = $2::bigint)"
	        "   AND NOT EXISTS (SELECT 1 FROM series_file k WHERE k.file_id = v.file_id AND ($3::boolean"
	        "   OR k.read_source IS DISTINCT FROM s.episode_source OR k.hand_at IS NOT NULL))"
	        /* What a person linked by hand stands, whatever series it is
	         * linked to, and what a person unlinked by hand stays unlinked
	         * (add-force-grab-and-assign, 0063_hand_unlink.sql). */
	        "   AND NOT EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = v.file_id)),"
	        " files AS (INSERT INTO series_file (file_id, series_id, quality, resolution, quality_text, identity_id,"
	        "   linked_by) SELECT DISTINCT ON (p.file_id) p.file_id, p.series_id, lif.quality, lif.resolution,"
	        "   lif.quality_text, p.identity_id, li.decided_by FROM placed p"
	        "   JOIN library_identity_file lif ON lif.file_id = p.file_id JOIN library_identity li ON li.id = p.identity_id"
	        "   ORDER BY p.file_id"
	        "   ON CONFLICT (file_id) DO UPDATE SET series_id = EXCLUDED.series_id, quality = EXCLUDED.quality,"
	        "   resolution = EXCLUDED.resolution, quality_text = EXCLUDED.quality_text, identity_id = EXCLUDED.identity_id,"
	        "   linked_by = EXCLUDED.linked_by, linked_at = now() WHERE series_file.import_id IS NULL"
	        "   AND series_file.hand_at IS NULL RETURNING file_id),"
	        " links AS (INSERT INTO series_file_episode (file_id, episode_id) SELECT p.file_id, p.episode_id"
	        "   FROM placed p JOIN files f ON f.file_id = p.file_id ON CONFLICT DO NOTHING RETURNING 1)"
	        " SELECT (SELECT count(*) FROM files), (SELECT count(*) FROM links)", 3, params, &rows, error))
		return FALSE;
	axis_db_result_get_int64(rows, 0, 0, &n);
	*files = (guint)MAX(n, 0);
	return TRUE;
}

gboolean
axis_tv_link_identified(
	AxisDbConn   *conn,
	gint64        series_id,
	gint64        title_id,
	guint        *linked,
	GError      **error
){
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);

	(void)title_id;
	return link_placed(conn, ids, 0, TRUE, FALSE, linked, error);
}

gboolean
axis_tv_link_unlinked(
	AxisDbConn   *conn,
	gint64        series_id,
	guint        *linked,
	GError      **error
){
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);

	return link_placed(conn, ids, 0, FALSE, TRUE, linked, error);
}

static gboolean
identity_where(
	AxisDbConn  *conn,
	gint64       identity_id,
	gchar      **where,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *identity = axis_tv_int_text(identity_id);
	const gchar *params[1];

	*where = NULL;
	params[0] = identity;
	if (!axis_db_conn_exec(conn, "SELECT r.name, i.rel_path FROM library_identity i JOIN library_root r ON r.id ="
	        " i.root_id WHERE i.id = $1::bigint", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0) {
		g_autofree gchar *shown = axis_library_name_display(axis_db_result_get_text(rows, 0, 1));

		*where = g_strdup_printf("%s: %s", axis_db_result_get_text(rows, 0, 0), shown);
	}
	return TRUE;
}

static gboolean
on_link(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        identity_id,
	gint64        title_id,
	const gchar  *user,
	GError      **error
){
	g_autofree gchar *identity = axis_tv_int_text(identity_id);
	g_autofree gchar *ids = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];
	gint64 series_id = 0;
	guint made = 0;

	/* What this identity linked before goes first (it may have been another
	 * series'), but for a link whose file's name was read under another
	 * numbering than its series follows now: that stays as it is. */
	params[0] = identity;
	if (!axis_db_conn_exec(conn, "DELETE FROM series_file sf WHERE sf.identity_id = $1::bigint AND sf.hand_at IS NULL"
	        " AND (NOT EXISTS"
	        " (SELECT 1 FROM series s JOIN library_identity li ON li.id = sf.identity_id WHERE s.id = sf.series_id"
	        " AND s.title_id = li.title_id) OR sf.read_source IS NOT DISTINCT FROM (SELECT rs.episode_source FROM series rs"
	        " WHERE rs.id = sf.series_id))", 1, params, NULL, error) ||
	    !series_of_title(conn, title_id, &series_id, &label, error))
		return FALSE;
	if (series_id <= 0)
		return TRUE;
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);
	if (!link_placed(conn, ids, identity_id, FALSE, FALSE, &made, error))
		return FALSE;
	/* Every file had an import's link already, or none holds an episode of
	 * the series' data: nothing new to say. */
	if (made == 0)
		return TRUE;
	if (!identity_where(conn, identity_id, &where, error))
		return FALSE;
	text = g_strdup_printf("Found on disk%s%s%s%s: %u episode %s linked, and those episodes are in the library.",
		user != NULL ? ", confirmed by " : " (sure)", user != NULL ? user : "", where != NULL ? ", at " : "",
		where != NULL ? where : "", made, made == 1 ? "file is" : "files are");
	return axis_tv_store_event(conn, series_id, title_id, label, 0, "identified", text, user, 0, 0, error);
}

static gboolean
on_unlink(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        identity_id,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisDbResult) gone = NULL;
	g_autofree gchar *identity = axis_tv_int_text(identity_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];
	gint64 series_id = 0;
	gint64 title_id = 0;
	gint64 found = 0;

	params[0] = identity;
	if (!axis_db_conn_exec(conn, "DELETE FROM series_file sf USING series s WHERE sf.identity_id = $1::bigint"
	        " AND sf.hand_at IS NULL AND s.id = sf.series_id RETURNING s.id, s.title_id", 1, params, &gone, error))
		return FALSE;
	if (axis_db_result_get_n_rows(gone) == 0)
		return TRUE;
	axis_db_result_get_int64(gone, 0, 1, &title_id);
	if (!series_of_title(conn, title_id, &series_id, &label, error) || !identity_where(conn, identity_id, &where, error))
		return FALSE;
	if (series_id <= 0)
		return TRUE;
	found = (gint64)axis_db_result_get_n_rows(gone);
	text = g_strdup_printf("The link to the folder on disk%s%s was undone%s%s: the series no longer counts %s.",
		where != NULL ? " at " : "", where != NULL ? where : "", user != NULL ? " by " : "", user != NULL ? user : "",
		found == 1 ? "that file" : "those files");
	return axis_tv_store_event(conn, series_id, title_id, label, 0, "unidentified", text, user, 0, 0, error);
}

static gboolean
on_managed(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        title_id,
	gboolean     *managed,
	GError      **error
){
	g_autofree gchar *label = NULL;
	gint64 series_id = 0;

	*managed = FALSE;
	if (!series_of_title(conn, title_id, &series_id, &label, error))
		return FALSE;
	*managed = series_id > 0;
	return TRUE;
}

static gboolean
on_count_managed(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        root_id,
	guint        *managed,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *root = axis_tv_int_text(root_id);
	const gchar *params[1];
	gint64 n = 0;

	*managed = 0;
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT count(DISTINCT i.title_id) FROM library_identity i JOIN series s ON"
	        " s.title_id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked'", 1, params, &rows, error))
		return FALSE;
	axis_db_result_get_int64(rows, 0, 0, &n);
	*managed = (guint)MAX(n, 0);
	return TRUE;
}

/*
 * Every series linked in @root_id and not managed yet, managed now, set by
 * set: the series, their seasons and episodes as the metadata store has them
 * (what axis_tv_sync_in_transaction() makes when a series is added), the
 * links of their files on disk and their history, in one transaction,
 * whatever their number.  @monitored: future episodes (the seasons with a
 * number, and the episodes of them not aired yet or with no date), else
 * nothing.  Nothing is searched for.
 */
static gboolean
on_manage(
	gpointer      data,
	gint64        root_id,
	gint64        profile_id,
	gboolean      monitored,
	const gchar  *user,
	guint        *added,
	guint        *skipped,
	GError      **error
){
	AxisTv *self = data;
	g_autoptr(AxisDbResult) total = NULL;
	g_autoptr(AxisDbResult) fresh = NULL;
	g_autoptr(AxisDbResult) root_row = NULL;
	g_autoptr(AxisQualityStored) stored = NULL;
	g_autofree gchar *root = axis_tv_int_text(root_id);
	g_autofree gchar *profile = NULL;
	g_autofree gchar *delay = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *floor = axis_tv_int_text(axis_tv_setting_duration(self, AXIS_TV_SETTING_FLOOR, 6 * 3600));
	g_autofree gchar *text = NULL;
	g_autoptr(GString) ids = g_string_new("{");
	AxisQualityProfile checked;
	AxisTvRules rules;
	const gchar *params[9];
	AxisDbConn *conn;
	gint64 n_titles = 0;
	guint linked = 0;
	gboolean ok;
	guint i;

	*added = 0;
	*skipped = 0;
	axis_tv_get_rules(self, &rules);
	delay = axis_tv_int_text(rules.air_delay_s);
	now = axis_tv_int_text(rules.now_us / G_USEC_PER_SEC);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The profile (0: the default) and the root, as for one series. */
	if (profile_id > 0) {
		ok = axis_quality_store_read_id(conn, profile_id, &stored, error);
	} else {
		g_autofree gchar *slug = axis_quality_store_default_slug(self->quality);

		ok = axis_quality_store_read(conn, slug, &stored, error);
	}
	if (ok && (stored == NULL || !axis_quality_store_load(conn, stored->id, &checked, NULL))) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "There is no usable quality "
			"profile of that kind.");
		ok = FALSE;
	}
	params[0] = root;
	ok = ok && axis_db_conn_exec(conn, "SELECT name FROM library_root WHERE id = $1::bigint AND media_type = 'tv'", 1,
		params, &root_row, error);
	if (ok && axis_db_result_get_n_rows(root_row) == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "That is not a library folder for "
			"TV.");
		ok = FALSE;
	}
	if (!ok) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	profile = axis_tv_int_text(stored->id);
	text = g_strdup_printf("Added%s%s with every identified series of %s, with the quality profile %s, monitoring %s. "
		"Its episodes on disk are in the library, so nothing was searched for.", user != NULL ? " by " : "",
		user != NULL ? user : "", axis_db_result_get_text(root_row, 0, 0), stored->name, monitored ?
		"future episodes only" : "nothing yet");
	params[1] = profile;
	params[2] = monitored ? "future" : "none";
	params[3] = monitored ? "true" : "false";
	params[4] = user;
	params[5] = delay;
	params[6] = floor;
	params[7] = text;
	params[8] = now;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT count(DISTINCT i.title_id) FROM library_identity i JOIN meta_title t ON"
			" t.id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked' AND t.kind = 'series'", 1, params,
			&total, error) &&
		axis_db_conn_exec(conn, "WITH titles AS (SELECT DISTINCT i.title_id FROM library_identity i"
			"   JOIN meta_title t ON t.id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked'"
			"   AND t.kind = 'series'),"
			" fresh AS (INSERT INTO series (title_id, quality_profile_id, root_id, monitor_choice, monitor_new, added_by,"
			"   synced_at) SELECT title_id, $2::bigint, $1::bigint, $3, $4::boolean, $5, now() FROM titles"
			"   ORDER BY title_id ON CONFLICT (title_id) DO NOTHING RETURNING id, title_id, episode_source),"
			" seasons AS (INSERT INTO series_season (series_id, number, monitored) SELECT f.id, ms.number,"
			"   $4::boolean AND ms.number > 0 FROM fresh f JOIN meta_season ms ON ms.title_id = f.title_id"
			"   AND ms.source = f.episode_source"
			"   ON CONFLICT (series_id, number) DO NOTHING RETURNING 1),"
			" episodes AS (INSERT INTO series_episode (series_id, source, provider_id, season_number, number,"
			"   absolute_number, title, air_date, runtime_minutes, monitored, search_floor_s)"
			"   SELECT DISTINCT ON (f.id, coalesce(me.provider_id, me.season_number || ':' || me.number)) f.id,"
			"   f.episode_source, me.provider_id, me.season_number, me.number, me.absolute_number, left(me.name, 500),"
			"   me.air_date, me.runtime_minutes, $4::boolean AND me.season_number > 0 AND (me.air_date IS NULL"
			"     OR (me.air_date::timestamp AT TIME ZONE 'UTC') + make_interval(secs => $6::integer)"
			"       > to_timestamp($9::bigint)), $7::integer"
			"   FROM fresh f JOIN meta_episode me ON me.title_id = f.title_id AND me.source = f.episode_source"
			"   JOIN meta_season ms ON ms.title_id = me.title_id AND ms.source = me.source"
			"   AND ms.number = me.season_number"
			"   ORDER BY f.id, coalesce(me.provider_id, me.season_number || ':' || me.number),"
			"   ms.episodes_fetched_at DESC NULLS LAST RETURNING 1),"
			" events AS (INSERT INTO series_event (series_id, title_id, title_label, kind, text, by_user)"
			"   SELECT f.id, f.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
			"   'added', $8, $5 FROM fresh f JOIN meta_title t ON t.id = f.title_id RETURNING 1)"
			" SELECT f.id, (SELECT count(*) FROM seasons), (SELECT count(*) FROM episodes), (SELECT count(*) FROM events)"
			" FROM fresh f", 9, params, &fresh, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(fresh); i++) {
		gint64 id = 0;

		axis_db_result_get_int64(fresh, i, 0, &id);
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", id);
	}
	g_string_append_c(ids, '}');
	ok = ok && (axis_db_result_get_n_rows(fresh) == 0 || link_placed(conn, ids->str, 0, FALSE, FALSE, &linked, error)) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	axis_db_result_get_int64(total, 0, 0, &n_titles);
	*added = axis_db_result_get_n_rows(fresh);
	*skipped = (guint)MAX(n_titles - (gint64)*added, 0);
	return TRUE;
}

static const AxisIdentifyOwnerInfo owner_info = {
	AXIS_TV_MODULE_ID,
	on_link,
	on_unlink,
	on_manage,
	on_managed,
	on_count_managed,
	AXIS_META_KIND_SERIES
};

void
axis_tv_set_identify(
	AxisTv       *self,
	AxisIdentify *identify
){
	g_return_if_fail(self != NULL);

	self->identify = identify;
	if (identify != NULL)
		axis_identify_add_owner(identify, &owner_info, self);
}
