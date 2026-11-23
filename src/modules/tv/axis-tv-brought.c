/*
 * axis-tv-brought.c - a series migrated from another app (Sonarr): adding
 * it through the code a person's Add uses, under the numbering the app's
 * seasons and episodes are in; copying its monitoring; linking the episode
 * files it has on disk; and the hold on its searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/arr-migration/spec.md.  The bring-over layer
 * (src/arr/) decides which series and which files, and maps the app's paths
 * to rows of Axis's index; what a series, an episode's match, a link and a
 * hold are is here.
 *
 *  - Only rows change.  No file or folder is opened, moved, renamed or
 *    deleted: a file is known by its row of Axis's own index (library_file).
 *  - An episode of the app's is an episode of the series when it is at the
 *    same season and number of the numbering the series follows and, where
 *    both give TheTVDB's id of the episode, the ids agree
 *    (AXIS_TV_BROUGHT_EPISODE_MATCH, the one rule).  Anything else is no
 *    match: it is counted, and its file is not linked.  Nothing is guessed
 *    from a title, a date or a file's name.
 *  - Monitoring and the hold of an episode whose file Axis does not see go
 *    by place alone, not by that rule: the episode of Axis's at a place the
 *    app lists is monitored as the app monitors that place, and held when
 *    the app has a file there and Axis has none on it, whether or not the
 *    ids agree.  Getting those two wrong the other way would fetch a second
 *    copy of a file that is on the disk; getting a link wrong would put a
 *    file on an episode it is not.  So the link is strict and these are not.
 *  - A link made here is neither an import's nor identification's: it has no
 *    import_id and no identity_id, and is stamped hand_how "brought" with who
 *    ran the bringing over and when, and read_source the numbering the series
 *    follows, since the episodes it is on are the app's, which are in that
 *    numbering.  What that means, read in the code that reads the stamps:
 *      identification  never replaces, moves or removes a row with hand_at
 *                      (link_placed(), on_link(), on_unlink() in
 *                      axis-tv-identify.c), whatever the file's name says;
 *      Place by name   never lists nor moves a link with hand_at
 *                      (model_read() in axis-tv-place.c), so a file the app
 *                      named under other numbers than its episode's is not
 *                      flagged as misplaced;
 *      a change of numbering  keeps a hand link on its row and lists it among
 *                      the links a person made, never placed by the tick box
 *                      (axis-tv-numbering.c);
 *      upgrades        never replace it by themselves: it is kept
 *                      (AXIS_TV_BROUGHT_KEPT, kept_how "brought"), so its
 *                      episode is not upgradable, no search takes a release
 *                      that holds its episode (a season pack over several
 *                      such files is left whole), and an import refuses to
 *                      replace it without its tick.  A person lets it go, for
 *                      the one file on the series' page or for all of a
 *                      connection's (axis_tv_release_brought()); then upgrades
 *                      judge it as any file, and an upgrade retires it as
 *                      any file an episode has (resolve() in axis-tv-import.c
 *                      lists every linked file, wherever it lies);
 *      the links page  shows it as "Migrated from Sonarr by NAME on DAY",
 *                      with what its name says beside it, and can move it,
 *                      unlink it or hand it back as any link a person made;
 *      duplicates      count it a person's: never recommended for removal.
 *  - A file that is linked already stays as it is, whoever linked it and to
 *    whatever series or episode; a file a person unlinked by hand stays
 *    unlinked.  One exception: a link identification made by the file's name
 *    (no person's and no import's) on other episodes of this series than the
 *    app has the file on is moved to the app's episodes and stamped brought.
 *  - The hold (series.search_hold, and series_episode.search_hold for an
 *    episode whose file Axis does not see) stops every automatic search at
 *    one gate, axis_tv_held().  A person's own search of an episode or a
 *    season passes it by its cause and ends nothing of the series' hold.  The
 *    series' hold ends when a person starts searching, for the series of a
 *    connection or for this one, or presses Search now for the whole series.
 *    An episode's own hold ends when a person presses Search now for that
 *    episode, ticks the box of "Let Axis search for it", or the episode gets
 *    a file (a later run, identification, an import, a link by hand).
 */

#include "modules/tv/axis-tv-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "library/axis-library-name.h"
#include "release/axis-release.h"

/* ── The hold ───────────────────────────────────────────────────── */

gboolean
axis_tv_hold_stops(const gchar *hold)
{
	return hold != NULL && hold[0] != '\0';
}

gboolean
axis_tv_cause_is_person(const gchar *cause)
{
	return g_strcmp0(cause, "manual") == 0 || g_strcmp0(cause, "list") == 0;
}

gboolean
axis_tv_held(
	const AxisTvSeries  *series,
	const AxisTvEpisode *episode
){
	return (series != NULL && axis_tv_hold_stops(series->search_hold)) ||
		(episode != NULL && axis_tv_hold_stops(episode->search_hold));
}

const gchar *
axis_tv_hold_sentence(const gchar *hold)
{
	if (!axis_tv_hold_stops(hold))
		return NULL;
	if (strcmp(hold, AXIS_TV_HOLD_FILE_UNSEEN) == 0)
		return "Migrated from Sonarr, which has a file for it that Axis does not see: Axis does not search for it "
			"until that file is found or you search for it yourself.";
	if (strcmp(hold, AXIS_TV_HOLD_DOWNLOADING) == 0)
		return "Migrated from Sonarr, which was downloading for this series when searching was started: Axis does "
			"not search for it until you start searching again.";
	return "Migrated from Sonarr: Axis does not search for it until you start searching.";
}

const gchar *
axis_tv_hold_short(const gchar *hold)
{
	return axis_tv_hold_stops(hold) ? "Held" : NULL;
}

void
axis_tv_search_pace(
	AxisTv *self,
	guint  *per_run,
	guint  *interval_s
){
	*per_run = (guint)CLAMP(axis_tv_setting_integer(self, AXIS_TV_SETTING_PER_RUN, 5), 1, 100000);
	*interval_s = AXIS_TV_SCHEDULE_INTERVAL_S;
}

void
axis_tv_brought_refresh(AxisTv *self)
{
	axis_tv_note_followed(self);
}

void
axis_tv_bring_prepare(
	AxisTv      *self,
	gint64       title_id,
	const gchar *source
){
	AxisMetaTitleAnswer answer;

	/* As axis_tv_add(): outside any transaction, since the provider may be
	 * slow.  The order comes whole, and only when it is not fresh. */
	memset(&answer, 0, sizeof(answer));
	if (axis_meta_get_title(self->meta, title_id, AXIS_TV_META_WAIT_MS, &answer, NULL) && answer.title != NULL &&
	    answer.title->kind == AXIS_META_KIND_SERIES)
		axis_tv_fetch_seasons(self, title_id, source, AXIS_TV_META_WAIT_MS);
	axis_meta_title_answer_clear(&answer);
}

static gboolean
count_of(
	AxisDbResult *result,
	guint         column,
	guint        *out
){
	gint64 n = 0;

	if (result == NULL || axis_db_result_get_n_rows(result) == 0 || !axis_db_result_get_int64(result, 0, column, &n))
		return FALSE;
	*out = (guint)MAX(n, 0);
	return TRUE;
}

gboolean
axis_tv_store_end_hold(
	AxisDbConn    *conn,
	AxisTvSeries  *series,
	AxisTvScope    scope,
	gint64         item,
	const gchar   *user,
	const gchar   *what,
	GError       **error
){
	g_autoptr(AxisDbResult) freed = NULL;
	g_autofree gchar *id = axis_tv_int_text(series->id);
	g_autofree gchar *item_text = axis_tv_int_text(item);
	g_autofree gchar *stay = NULL;
	const gchar *params[2];
	guint n = 0;
	gboolean was = axis_tv_hold_stops(series->search_hold);

	params[0] = id;
	params[1] = item_text;
	/* A season: the person's search runs for what of it is not held apart, and
	 * ends nothing. */
	if (scope == AXIS_TV_SCOPE_SEASON)
		return TRUE;
	if (scope == AXIS_TV_SCOPE_EPISODE) {
		/* The one episode the person asked to be searched for: its own hold.
		 * The series' hold stands. */
		if (!axis_db_conn_exec(conn, "UPDATE series_episode SET search_hold = NULL WHERE series_id = $1::bigint"
		        " AND id = $2::bigint AND search_hold IS NOT NULL RETURNING 1", 2, params, &freed, error))
			return FALSE;
		if (axis_db_result_get_n_rows(freed) == 0)
			return TRUE;
		return axis_tv_store_eventf(conn, series, item, "changed", user, error, "Sonarr has a file for it that Axis does "
			"not see, and it was held for that: %s%s%s ends that for this episode.%s", what, user != NULL ? " by " : "",
			user != NULL ? user : "", was ? " The series itself stays held: Axis searches for nothing else of it until "
			"searching is started." : " Axis searches for it by itself from now on.");
	}
	/* The whole series: its own hold.  Never the episodes Sonarr has files for
	 * that Axis does not see: a second copy of each would be fetched. */
	if (!was)
		return TRUE;
	if (!axis_db_conn_exec(conn, "WITH s AS (UPDATE series SET search_hold = NULL WHERE id = $1::bigint"
	        "   AND search_hold IS NOT NULL RETURNING 1)"
	        " SELECT (SELECT count(*) FROM s), (SELECT count(*) FROM series_episode e WHERE e.series_id = $1::bigint"
	        "   AND e.gone_at IS NULL AND " AXIS_TV_EPISODE_HELD_SQL("e") ")", 1, params, &freed, error))
		return FALSE;
	count_of(freed, 1, &n);
	g_clear_pointer(&series->search_hold, g_free);
	if (n > 0)
		stay = g_strdup_printf(" %u %s Sonarr has a file for that Axis does not see %s held: nothing is searched for "
			"%s.", n, n == 1 ? "episode" : "episodes", n == 1 ? "stays" : "stay", n == 1 ? "it" : "them");
	return axis_tv_store_eventf(conn, series, 0, "changed", user, error, "It was migrated and held: %s%s%s for the "
		"whole series ends that, and Axis searches for it by itself from now on.%s", what, user != NULL ? " by " : "",
		user != NULL ? user : "", stay != NULL ? stay : "");
}

gboolean
axis_tv_start_searching(
	AxisTv       *self,
	gint64        series_id,
	gboolean      unseen_too,
	const gchar  *user,
	gboolean     *was_held,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisDbResult) freed = NULL;
	g_autofree gchar *id = axis_tv_int_text(series_id);
	const gchar *params[2];
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean found = FALSE;
	gboolean ok;
	guint n_series = 0;
	guint n_episodes = 0;
	guint n_stay = 0;

	if (was_held != NULL)
		*was_held = FALSE;
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = unseen_too ? "true" : "false";
	ok = axis_db_conn_begin(conn, error) && axis_tv_store_lock(conn, series_id, &found, error);
	if (ok && found)
		ok = axis_tv_store_series_one(conn, series_id, &series, error);
	if (ok && series == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		ok = FALSE;
	}
	/* The series' own hold; its episodes' holds only with the person's tick.
	 * (Counted from before the change: what was held apart, and of that what
	 * the tick let go.) */
	ok = ok && axis_db_conn_exec(conn, "WITH s AS (UPDATE series SET search_hold = NULL WHERE id = $1::bigint"
		"   AND search_hold IS NOT NULL RETURNING 1),"
		" e AS (UPDATE series_episode SET search_hold = NULL WHERE series_id = $1::bigint AND search_hold IS NOT NULL"
		"   AND $2::boolean RETURNING 1)"
		" SELECT (SELECT count(*) FROM s), (SELECT count(*) FROM e), (SELECT count(*) FROM series_episode x"
		"   WHERE x.series_id = $1::bigint AND x.gone_at IS NULL AND NOT $2::boolean AND "
		AXIS_TV_EPISODE_HELD_SQL("x") ")", 2, params, &freed, error);
	if (ok) {
		count_of(freed, 0, &n_series);
		count_of(freed, 1, &n_episodes);
		count_of(freed, 2, &n_stay);
	}
	if (ok && n_series + n_episodes > 0) {
		g_autofree gchar *also = n_episodes > 0 ? g_strdup_printf(" %u %s Sonarr has a file for that Axis does not see "
			"%s searched for too, as ticked: a second copy may be fetched.", n_episodes, n_episodes == 1 ? "episode" :
			"episodes", n_episodes == 1 ? "is" : "are") : n_stay > 0 ? g_strdup_printf(" %u %s Sonarr has a file for "
			"that Axis does not see %s held: nothing is searched for %s.", n_stay, n_stay == 1 ? "episode" : "episodes",
			n_stay == 1 ? "stays" : "stay", n_stay == 1 ? "it" : "them") : NULL;

		if (was_held != NULL)
			*was_held = TRUE;
		/* Nothing is searched at once: the scheduler takes it at its pace. */
		ok = axis_tv_store_eventf(conn, series, 0, "changed", user, error, "Searching for it was started%s%s: it was "
			"migrated and held until now. Axis searches for it at the scheduler's ordinary pace.%s",
			user != NULL ? " by " : "", user != NULL ? user : "", also != NULL ? also : "");
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

static gchar *
id_array(const GArray *ids)
{
	GString *text = g_string_new("{");
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(text, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(text, '}');
	return g_string_free(text, FALSE);
}

gboolean
axis_tv_start_brought(
	AxisTv        *self,
	AxisDbConn    *conn,
	const GArray  *start,
	const GArray  *downloading,
	const gchar   *origin,
	const gchar   *user,
	guint         *started,
	GError       **error
){
	g_autoptr(AxisDbResult) freed = NULL;
	g_autofree gchar *start_ids = id_array(start);
	g_autofree gchar *held_ids = id_array(downloading);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	g_autofree gchar *text = g_strdup_printf("Searching was started for the series migrated from %s%s%s. Axis "
		"searches for it at the scheduler's ordinary pace.", origin, by != NULL ? " by " : "", by != NULL ? by : "");
	g_autofree gchar *waits = g_strdup_printf("Searching was started for the series migrated from %s, and this one "
		"was left out: that app's download queue holds a download for it. Start searching again once it has left the "
		"queue.", origin);
	const gchar *params[3];

	*started = 0;
	/* The series the other app is downloading for: held for that, said once. */
	params[0] = held_ids;
	params[1] = waits;
	params[2] = by;
	if (!axis_db_conn_exec(conn,
	        "WITH held AS (UPDATE series SET search_hold = '" AXIS_TV_HOLD_DOWNLOADING "'"
	        "   WHERE id = ANY ($1::bigint[]) AND search_hold = '" AXIS_TV_HOLD_BROUGHT "' RETURNING id, title_id)"
	        " INSERT INTO series_event (series_id, title_id, title_label, kind, text, by_user)"
	        " SELECT h.id, h.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        " 'changed', $2, $3 FROM held h JOIN meta_title t ON t.id = h.title_id", 3, params, NULL, error))
		return FALSE;
	/* The others: free.  An episode whose file Axis cannot see has a hold of
	 * its own, which nothing here touches. */
	params[0] = start_ids;
	params[1] = text;
	if (!axis_db_conn_exec(conn,
	        "WITH freed AS (UPDATE series SET search_hold = NULL"
	        "   WHERE id = ANY ($1::bigint[]) AND search_hold IN ('" AXIS_TV_HOLD_BROUGHT "', '"
	        AXIS_TV_HOLD_DOWNLOADING "') RETURNING id, title_id),"
	        " said AS (INSERT INTO series_event (series_id, title_id, title_label, kind, text, by_user)"
	        "   SELECT f.id, f.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        "   'changed', $2, $3 FROM freed f JOIN meta_title t ON t.id = f.title_id RETURNING 1)"
	        " SELECT (SELECT count(*) FROM freed), (SELECT count(*) FROM said)", 3, params, &freed, error))
		return FALSE;
	count_of(freed, 0, started);
	return TRUE;
}

gboolean
axis_tv_release_brought(
	AxisTv        *self,
	AxisDbConn    *conn,
	const GArray  *series,
	const gchar   *origin,
	const gchar   *user,
	guint         *released,
	GError       **error
){
	g_autoptr(AxisDbResult) freed = NULL;
	g_autofree gchar *ids = id_array(series);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	g_autofree gchar *text = g_strdup_printf("The episode files migrated from %s were let go%s%s: Axis may replace "
		"this series' files again. A file below the cutoff of the quality profile is replaced at the scheduler's "
		"ordinary pace, and the old one goes to the recycle folder.", origin, by != NULL ? " by " : "",
		by != NULL ? by : "");
	const gchar *params[3];

	*released = 0;
	params[0] = ids;
	params[1] = text;
	params[2] = by;
	/* Only what the bringing over keeps: a link a person kept or let go on
	 * the series' page has no kept_how, and one linked by hand since has
	 * another hand_how. */
	if (!axis_db_conn_exec(conn,
	        "WITH freed AS (UPDATE series_file SET kept = false, kept_how = NULL"
	        "   WHERE series_id = ANY ($1::bigint[]) AND hand_how = 'brought' AND kept"
	        "   AND kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "' RETURNING series_id),"
	        " said AS (INSERT INTO series_event (series_id, title_id, title_label, kind, text, by_user)"
	        "   SELECT s.id, s.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        "   'hand_link', $2, $3 FROM (SELECT DISTINCT series_id FROM freed) f JOIN series s ON s.id = f.series_id"
	        "   JOIN meta_title t ON t.id = s.title_id RETURNING 1)"
	        " SELECT (SELECT count(*) FROM freed), (SELECT count(*) FROM said)", 3, params, &freed, error))
		return FALSE;
	count_of(freed, 0, released);
	return TRUE;
}

/* ── Matching and linking ───────────────────────────────────────── */

/* What Axis reads of a file's quality from its own name, when the other app
 * recorded none Axis knows: a ladder quality, else a resolution, else
 * nothing. */
static void
quality_of_name(
	const gchar  *stored_base,
	gchar       **quality,
	gchar       **resolution
){
	g_autofree gchar *shown = axis_library_name_display(stored_base != NULL ? stored_base : "");
	g_autoptr(AxisRelease) release = axis_release_parse(shown);
	AxisQuality read = axis_quality_of_release(release);

	*quality = NULL;
	*resolution = NULL;
	if (read != AXIS_QUALITY_UNKNOWN)
		*quality = g_strdup(axis_quality_name(read));
	else if (release != NULL && release->resolution != AXIS_RELEASE_RESOLUTION_NONE)
		*resolution = g_strdup(axis_release_resolution_name(release->resolution));
}

/* The app's episodes as JSON for one statement: [{"o": 0, "s": 1, "n": 2,
 * "t": 12345, "m": true, "f": true}]; "f": the app has a file for it; "x":
 * only its place could be read. */
static gchar *
episodes_json(
	const AxisTvBringEpisode *episodes,
	guint                     n
){
	g_autoptr(JsonArray) rows = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(JsonGenerator) generator = json_generator_new();
	guint i;

	for (i = 0; i < n; i++) {
		JsonObject *row = json_object_new();

		json_object_set_int_member(row, "o", i);
		json_object_set_int_member(row, "s", episodes[i].season);
		json_object_set_int_member(row, "n", episodes[i].number);
		if (episodes[i].tvdb_id > 0)
			json_object_set_int_member(row, "t", episodes[i].tvdb_id);
		json_object_set_boolean_member(row, "m", episodes[i].monitored && !episodes[i].odd);
		json_object_set_boolean_member(row, "f", episodes[i].file_ref > 0 || episodes[i].odd);
		json_object_set_boolean_member(row, "x", episodes[i].odd);
		json_array_add_object_element(rows, row);
	}
	json_node_set_array(node, rows);
	json_generator_set_root(generator, node);
	return json_generator_to_data(generator, NULL);
}

#define EPISODES_AS_Q "jsonb_to_recordset($2::jsonb) AS q(o integer, s integer, n integer, t bigint, m boolean," \
	" f boolean, x boolean)"

/* The places the app lists, one row each (SQL, over $2): "m", every entry at
 * the place is monitored; "f", one of them has a file (or could not be
 * read). */
#define PLACES_AS_P "(SELECT q.s, q.n, bool_and(q.m) AS m, bool_or(q.f) AS f FROM " EPISODES_AS_Q " GROUP BY q.s, q.n)" \
	" AS p"

/*
 * Each of the app's episodes matched to the series' own
 * (AXIS_TV_BROUGHT_EPISODE_MATCH): *@matched holds, for each of @episodes in
 * order, the series_episode id or 0.  A place the app lists twice matches
 * nothing: which of the two is meant cannot be told.  An entry of which only
 * the place could be read matches nothing either (and counts at its place),
 * and is not counted among the unmatched: it is no episode.
 */
static gboolean
match_episodes(
	AxisDbConn                *conn,
	gint64                     series_id,
	const AxisTvBringEpisode  *episodes,
	const gchar               *json,
	guint                      n,
	GArray                   **matched,
	guint                     *unmatched,
	GError                   **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_tv_int_text(series_id);
	const gchar *params[2];
	guint i;

	*matched = g_array_sized_new(FALSE, TRUE, sizeof(gint64), n);
	g_array_set_size(*matched, n);
	*unmatched = 0;
	for (i = 0; i < n; i++)
		*unmatched += episodes[i].odd ? 0 : 1;
	if (n == 0)
		return TRUE;
	params[0] = id;
	params[1] = json;
	if (!axis_db_conn_exec(conn, "SELECT q.o, e.id FROM " EPISODES_AS_Q " JOIN series s ON s.id = $1::bigint"
	        " JOIN series_episode e ON " AXIS_TV_BROUGHT_EPISODE_MATCH("e", "s", "q.s", "q.n", "q.t")
	        " WHERE NOT q.x"
	        " AND (SELECT count(*) FROM " EPISODES_AS_Q " WHERE q.s = e.season_number AND q.n = e.number) = 1", 2,
	        params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 ord = -1;
		gint64 episode = 0;

		axis_db_result_get_int64(rows, i, 0, &ord);
		axis_db_result_get_int64(rows, i, 1, &episode);
		if (ord < 0 || ord >= n || episode <= 0 || g_array_index(*matched, gint64, (guint)ord) != 0)
			continue;
		g_array_index(*matched, gint64, (guint)ord) = episode;
		(*unmatched)--;
	}
	return TRUE;
}

/*
 * The app's monitoring onto the series: each season the app lists as it has
 * it; a season it does not list as seasons that come later are (never the
 * specials); each episode as its season, then each episode at a place the
 * app lists as the app monitors that place.  When the app does not monitor
 * the series at all, nothing is monitored.
 */
static gboolean
copy_monitoring(
	AxisDbConn         *conn,
	gint64              series_id,
	const AxisTvBring  *bring,
	const gchar        *json,
	GError            **error
){
	g_autoptr(GString) seasons = g_string_new("[");
	g_autofree gchar *id = axis_tv_int_text(series_id);
	gboolean later = bring->monitored && bring->monitor_new;
	const gchar *params[3];
	guint i;

	for (i = 0; i < bring->n_seasons; i++)
		g_string_append_printf(seasons, "%s{\"n\":%d,\"m\":%s}", i > 0 ? "," : "", bring->seasons[i].number,
			bring->monitored && bring->seasons[i].monitored ? "true" : "false");
	g_string_append_c(seasons, ']');
	params[0] = id;
	params[1] = later ? "true" : "false";
	if (!axis_db_conn_exec(conn, "UPDATE series SET monitor_new = $2::boolean WHERE id = $1::bigint", 2, params, NULL,
	        error))
		return FALSE;
	params[1] = seasons->str;
	params[2] = later ? "true" : "false";
	if (!axis_db_conn_exec(conn, "UPDATE series_season ss SET monitored = coalesce((SELECT bool_or(q.m)"
	        " FROM jsonb_to_recordset($2::jsonb) AS q(n integer, m boolean) WHERE q.n = ss.number),"
	        " $3::boolean AND ss.number > 0) WHERE ss.series_id = $1::bigint", 3, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "UPDATE series_episode e SET monitored = ss.monitored FROM series_season ss"
	        " WHERE e.series_id = $1::bigint AND ss.series_id = e.series_id AND ss.number = e.season_number", 1, params,
	        NULL, error))
		return FALSE;
	if (!bring->monitored || bring->n_episodes == 0)
		return TRUE;
	params[1] = json;
	/* By place alone: the episode of the series at a place the app lists, as
	 * the app monitors that place, whatever the ids say.  (An episode the app
	 * does not monitor must not become wanted here because its id differs.) */
	return axis_db_conn_exec(conn, "UPDATE series_episode e SET monitored = p.m FROM " PLACES_AS_P
		" JOIN series s ON s.id = $1::bigint WHERE e.series_id = s.id AND e.gone_at IS NULL"
		" AND e.source = s.episode_source AND e.season_number = p.s AND e.number = p.n", 2, params, NULL, error);
}

typedef enum
{
	LINK_GONE,
	LINK_LINKED,
	LINK_ALREADY,
	LINK_DIFFERS,	/* already, on other episodes than the app says */
	LINK_MOVED,	/* identification's link by name, moved to the app's episodes */
	LINK_ELSEWHERE,
	LINK_UNLINKED
} Linked;

/*
 * Links @file to the episodes @episode_ids ("{1,2}") of the series as brought
 * over, when it is linked to nothing and no person unlinked it.  The file's
 * row of the index is locked first, so that an import, identification or the
 * links page deciding about the same file at this moment is seen, or waits.
 */
static gboolean
link_file(
	AxisDbConn             *conn,
	const AxisTvSeries     *series,
	const AxisTvBringFile  *file,
	const gchar            *episode_ids,
	const gchar            *user,
	Linked                 *out,
	GError                **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autoptr(AxisDbResult) made = NULL;
	g_autofree gchar *file_text = axis_tv_int_text(file->file_id);
	g_autofree gchar *series_text = axis_tv_int_text(series->id);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	g_autofree gchar *from = axis_tv_text_cut(file->release, 300);
	g_autofree gchar *read_quality = NULL;
	g_autofree gchar *read_resolution = NULL;
	const gchar *params[9];
	gint64 linked_to = 0;
	gboolean unlinked = FALSE;
	gboolean same = FALSE;
	gboolean by_name = FALSE;

	*out = LINK_GONE;
	params[0] = file_text;
	params[1] = episode_ids;
	if (!axis_db_conn_exec(conn, "SELECT lf.state, lf.kind, lf.base_name, coalesce(sf.series_id, 0),"
	        " EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = lf.id),"
	        " coalesce((SELECT array_agg(x.episode_id ORDER BY x.episode_id) FROM series_file_episode x"
	        "   WHERE x.file_id = lf.id), '{}') = (SELECT coalesce(array_agg(u ORDER BY u), '{}')"
	        "   FROM unnest($2::bigint[]) AS u),"
	        /* Identification's own link: by the file's name, no person's and
	         * no import's, not kept. */
	        " coalesce(sf.hand_at IS NULL AND sf.import_id IS NULL AND NOT sf.kept, false)"
	        " FROM library_file lf LEFT JOIN series_file sf ON sf.file_id = lf.id"
	        " WHERE lf.id = $1::bigint FOR UPDATE OF lf", 2, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0 || g_strcmp0(axis_db_result_get_text(row, 0, 0), "present") != 0 ||
	    g_strcmp0(axis_db_result_get_text(row, 0, 1), "video") != 0)
		return TRUE;
	axis_db_result_get_int64(row, 0, 3, &linked_to);
	axis_db_result_get_boolean(row, 0, 4, &unlinked);
	axis_db_result_get_boolean(row, 0, 5, &same);
	axis_db_result_get_boolean(row, 0, 6, &by_name);
	if (linked_to == series->id && !same && by_name) {
		/* Identification put it on the episodes its name states, a moment ago
		 * (Add links what identification holds) or before; the app has it on
		 * others, and every one of those matched.  The app's word is the one
		 * a person relied on for years: the link is moved there and stamped
		 * as migrated, which identification never moves again (it leaves
		 * every row with hand_at).  The episodes it leaves are as any other:
		 * monitored as copied, and held when the app has another file there
		 * that Axis does not see (link_files(), after every file). */
		params[1] = episode_ids;
		params[2] = by;
		params[3] = series->episode_source;
		params[4] = file->quality;
		params[5] = from;
		/* Stamped as any brought link: kept, as one linked here is. */
		params[6] = AXIS_TV_BROUGHT_KEPT ? "true" : "false";
		if (!axis_db_conn_exec(conn, "WITH gone AS (DELETE FROM series_file_episode WHERE file_id = $1::bigint"
		        "   AND NOT (episode_id = ANY ($2::bigint[])) RETURNING 1),"
		        " made AS (INSERT INTO series_file_episode (file_id, episode_id) SELECT $1::bigint, u"
		        "   FROM unnest($2::bigint[]) AS u ON CONFLICT DO NOTHING RETURNING 1)"
		        " UPDATE series_file SET hand_how = 'brought', hand_by = $3, hand_at = now(), identity_id = NULL,"
		        " read_source = $4, quality = coalesce($5, quality), release_name = coalesce($6, release_name),"
		        " quality_text = CASE WHEN $5 IS NOT NULL THEN 'Its quality is the one Sonarr recorded for it.'"
		        "   ELSE quality_text END, kept = $7::boolean,"
		        " kept_how = CASE WHEN $7::boolean THEN '" AXIS_TV_KEPT_HOW_BROUGHT "' END"
		        " WHERE file_id = $1::bigint", 7, params, NULL, error))
			return FALSE;
		*out = LINK_MOVED;
		return TRUE;
	}
	if (linked_to == series->id && same && by_name) {
		/* Identification linked it to the very episodes the app names, a
		 * moment ago (Add links what identification holds) or before.  It is
		 * the app's file all the same, and a brought file is kept: left as
		 * identification's (not kept, no hand stamp) it would be upgraded by
		 * Axis itself after "Start searching", on a server whose library was
		 * identified before the run most files.  So it is stamped as any
		 * brought link, and counted as linked. */
		params[1] = by;
		params[2] = series->episode_source;
		params[3] = file->quality;
		params[4] = from;
		params[5] = AXIS_TV_BROUGHT_KEPT ? "true" : "false";
		if (!axis_db_conn_exec(conn, "UPDATE series_file SET hand_how = 'brought', hand_by = $2, hand_at = now(),"
		        " identity_id = NULL, read_source = $3, quality = coalesce($4, quality),"
		        " release_name = coalesce($5, release_name),"
		        " quality_text = CASE WHEN $4 IS NOT NULL THEN 'Its quality is the one Sonarr recorded for it.'"
		        "   ELSE quality_text END, kept = $6::boolean,"
		        " kept_how = CASE WHEN $6::boolean THEN '" AXIS_TV_KEPT_HOW_BROUGHT "' END"
		        " WHERE file_id = $1::bigint", 6, params, NULL, error))
			return FALSE;
		*out = LINK_LINKED;
		return TRUE;
	}
	if (linked_to == series->id) {
		/* An import's or a person's: it stays as it is, and is not stamped
		 * again. */
		*out = same ? LINK_ALREADY : LINK_DIFFERS;
		return TRUE;
	}
	if (linked_to > 0) {
		*out = LINK_ELSEWHERE;
		return TRUE;
	}
	if (unlinked) {
		*out = LINK_UNLINKED;
		return TRUE;
	}
	if (file->quality == NULL)
		quality_of_name(axis_db_result_get_text(row, 0, 2), &read_quality, &read_resolution);
	params[1] = series_text;
	params[2] = file->quality != NULL ? file->quality : read_quality;
	params[3] = read_resolution;
	params[4] = from;
	params[5] = file->quality != NULL ? "Its quality is the one Sonarr recorded for it." : read_quality != NULL ||
		read_resolution != NULL ? "Its quality was read from its own name: Sonarr recorded none Axis knows." : NULL;
	params[6] = by;
	params[7] = AXIS_TV_BROUGHT_KEPT ? "true" : "false";
	/* The numbering its episodes are in: the one the series follows, which is
	 * the app's.  (Its name may state other numbers; nothing reads it.) */
	params[8] = series->episode_source;
	/* No import and no identity; ON CONFLICT: a link made since the row was
	 * read (it was locked, so by nothing that takes that lock) stands. */
	if (!axis_db_conn_exec(conn, "INSERT INTO series_file (file_id, series_id, quality, resolution, release_name,"
	        " quality_text, linked_by, hand_how, hand_by, hand_at, kept, kept_how, read_source) VALUES ($1::bigint,"
	        " $2::bigint, $3, $4, $5, $6, $7, 'brought', $7, now(), $8::boolean,"
	        " CASE WHEN $8::boolean THEN '" AXIS_TV_KEPT_HOW_BROUGHT "' END, $9)"
	        " ON CONFLICT (file_id) DO NOTHING RETURNING file_id", 9, params, &made, error))
		return FALSE;
	if (axis_db_result_get_n_rows(made) == 0) {
		*out = LINK_ELSEWHERE;
		return TRUE;
	}
	params[1] = episode_ids;
	if (!axis_db_conn_exec(conn, "INSERT INTO series_file_episode (file_id, episode_id) SELECT $1::bigint, u"
	        " FROM unnest($2::bigint[]) AS u ON CONFLICT DO NOTHING", 2, params, NULL, error))
		return FALSE;
	*out = LINK_LINKED;
	return TRUE;
}

/*
 * Whether @file_id (a row of the index, 0 for none) is linked to @series on
 * other episodes than @episode_ids ("{1,2}"; %NULL: on any episode, since the
 * app's are not all known).  Read only.
 */
static gboolean
linked_on_other(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	gint64               file_id,
	const gchar         *episode_ids,
	gboolean            *other,
	GError             **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *file_text = axis_tv_int_text(file_id);
	g_autofree gchar *series_text = axis_tv_int_text(series->id);
	const gchar *params[3];

	*other = FALSE;
	if (file_id <= 0)
		return TRUE;
	params[0] = file_text;
	params[1] = series_text;
	params[2] = episode_ids;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM series_file sf WHERE sf.file_id = $1::bigint AND sf.series_id = $2::bigint"
	        " AND EXISTS (SELECT 1 FROM series_file_episode x WHERE x.file_id = sf.file_id)"
	        " AND ($3::bigint[] IS NULL OR (SELECT array_agg(x.episode_id ORDER BY x.episode_id) FROM series_file_episode x"
	        "   WHERE x.file_id = sf.file_id) IS DISTINCT FROM (SELECT array_agg(u ORDER BY u)"
	        "   FROM unnest($3::bigint[]) AS u))", 3, params, &row, error))
		return FALSE;
	*other = axis_db_result_get_n_rows(row) > 0;
	return TRUE;
}

/*
 * Each file linked to the episodes the app says it holds, when every one of
 * them matched; then the hold of each episode at a place the app has a file
 * for (by place alone, @json: also where the ids disagree, where the place is
 * listed twice, and where the entry could not be read): let go where the
 * episode has a present file in Axis, and, with @hold (the run that brings
 * the series), set where it has none.  A later run never sets one: a hold a
 * person ended since stays ended.
 */
static gboolean
link_files(
	AxisDbConn                *conn,
	const AxisTvSeries        *series,
	const AxisTvBringEpisode  *episodes,
	const gchar               *json,
	guint                      n_episodes,
	const GArray              *matched,
	const AxisTvBringFile     *files,
	guint                      n_files,
	const gchar               *user,
	gboolean                   hold,
	AxisTvBrought             *brought,
	GError                   **error
){
	g_autoptr(GHashTable) by_ref = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
		(GDestroyNotify)g_array_unref);
	g_autoptr(AxisDbResult) held = NULL;
	g_autofree gchar *id = axis_tv_int_text(series->id);
	const gchar *params[3];
	guint i;

	/* The episodes of each file, by the app's number of the file. */
	for (i = 0; i < n_episodes; i++) {
		gint64 episode = g_array_index(matched, gint64, i);
		GArray *of;

		if (episodes[i].file_ref <= 0 || episodes[i].odd)
			continue;
		of = g_hash_table_lookup(by_ref, &episodes[i].file_ref);
		if (of == NULL) {
			of = g_array_new(FALSE, FALSE, sizeof(gint64));
			g_hash_table_insert(by_ref, (gpointer)&episodes[i].file_ref, of);
		}
		g_array_append_val(of, episode);
	}
	for (i = 0; i < n_files; i++) {
		const GArray *of = g_hash_table_lookup(by_ref, &files[i].file_ref);
		g_autoptr(GString) ids = g_string_new("{");
		gboolean all = of != NULL && of->len > 0;
		Linked linked = LINK_GONE;
		guint k;

		for (k = 0; of != NULL && k < of->len; k++) {
			gint64 episode = g_array_index(of, gint64, k);

			if (episode <= 0)
				all = FALSE;
			else
				g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, ids->len > 1 ? "," : "", episode);
		}
		g_string_append_c(ids, '}');
		if (!all) {
			gboolean other = FALSE;

			/* On an episode Axis's numbering does not have (or on none the
			 * app names): which episodes it holds is not known here, so it is
			 * linked to none, not to the part that matched.  One that is
			 * linked to the series all the same (identification, by its name)
			 * stays there, since where it belongs cannot be told, and is
			 * counted as on other episodes than the app says, not as
			 * unlinked. */
			if (!linked_on_other(conn, series, files[i].file_id > 0 ? files[i].file_id : files[i].other_id, NULL, &other,
			        error))
				return FALSE;
			if (other)
				brought->n_differs++;
			else
				brought->n_unmatched_files++;
			continue;
		}
		if (files[i].file_id <= 0) {
			gboolean other = FALSE;

			/* Of another size than the app recorded: it may be another file
			 * than the app's, so its link is not moved.  Linked on other
			 * episodes than the app says, it is counted so. */
			if (!linked_on_other(conn, series, files[i].other_id, ids->str, &other, error))
				return FALSE;
			if (other)
				brought->n_differs++;
			else
				brought->n_gone++;
			continue;
		}
		if (!link_file(conn, series, &files[i], ids->str, user, &linked, error))
			return FALSE;
		switch (linked) {
		case LINK_LINKED:
			brought->n_linked++;
			break;
		case LINK_MOVED:
			brought->n_moved++;
			break;
		case LINK_DIFFERS:
			brought->n_differs++;
			break;
		case LINK_ALREADY:
			brought->n_already++;
			break;
		case LINK_ELSEWHERE:
			brought->n_elsewhere++;
			break;
		case LINK_UNLINKED:
			brought->n_unlinked++;
			break;
		default:
			brought->n_gone++;
			break;
		}
	}
	/* The app has a file for the episode: held where Axis has none on it (it
	 * would count it missing and fetch a second copy), free where it has. */
	params[0] = id;
	params[1] = json;
	params[2] = hold ? "true" : "false";
	if (!axis_db_conn_exec(conn, "WITH has AS (SELECT e.id, EXISTS (SELECT 1 FROM series_file_episode sfe"
	        "   JOIN library_file lf ON lf.id = sfe.file_id AND lf.state = 'present'"
		        "   JOIN series_file sf ON sf.file_id = sfe.file_id"
		        "   WHERE sfe.episode_id = e.id AND (sf.kept OR sf.hand_at IS NOT NULL OR sf.import_id IS NOT NULL)) AS present"
	        "   FROM series_episode e JOIN series s ON s.id = e.series_id JOIN " PLACES_AS_P
	        "   ON p.s = e.season_number AND p.n = e.number AND p.f"
	        "   WHERE e.series_id = $1::bigint AND e.gone_at IS NULL AND e.source = s.episode_source),"
	        " changed AS (UPDATE series_episode e SET search_hold = CASE WHEN h.present THEN NULL ELSE '"
	        AXIS_TV_HOLD_FILE_UNSEEN "' END FROM has h WHERE e.id = h.id AND (h.present OR $3::boolean)"
	        "   AND e.search_hold IS DISTINCT FROM CASE WHEN h.present THEN NULL ELSE '" AXIS_TV_HOLD_FILE_UNSEEN "' END"
	        "   RETURNING 1)"
	        " SELECT (SELECT count(*) FROM series_episode e WHERE e.series_id = $1::bigint AND e.search_hold = '"
	        AXIS_TV_HOLD_FILE_UNSEEN "'), (SELECT count(*) FROM changed)", 3, params, &held, error))
		return FALSE;
	count_of(held, 1, &brought->n_held_episodes);
	/* (The count of the statement's own snapshot is from before the change:
	 * the episodes held now are counted after it.) */
	{
		g_autoptr(AxisDbResult) now = NULL;

		if (!axis_db_conn_exec(conn, "SELECT count(*) FROM series_episode WHERE series_id = $1::bigint"
		        " AND search_hold = '" AXIS_TV_HOLD_FILE_UNSEEN "'", 1, params, &now, error))
			return FALSE;
		count_of(now, 0, &brought->n_held_episodes);
	}
	return TRUE;
}

/* The history line of the links moved from the episodes their files' names
 * state to the episodes the app has them on; nothing for none. */
static gboolean
moved_event(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const gchar         *origin,
	const gchar         *user,
	guint                n,
	GError             **error
){
	if (n == 0)
		return TRUE;
	return axis_tv_store_eventf(conn, series, 0, "hand_link", user, error, "Migrated from %s%s%s: %u episode %s "
		"linked by %s to another episode than Sonarr has %s on, and %s moved to the episode Sonarr has it on. No file on "
		"disk was touched.", origin, user != NULL ? " by " : "", user != NULL ? user : "", n, n == 1 ? "file was" :
		"files were", n == 1 ? "its name" : "their names", n == 1 ? "it" : "them", n == 1 ? "was" : "each was");
}

/*
 * The app's own absolute number of each episode beside the one TheTVDB gives
 * the episode it was matched to (@matched: the series_episode id of each of
 * @bring's episodes, 0 for none), for a series brought as anime: counted,
 * kept on the series (series.brought_absolute) and said in its history.
 *
 * A REPORT ONLY.  It runs after the monitoring was copied, the files were
 * linked and the holds were set, reads the rows they left, and writes nothing
 * but its own column and one history line: no episode is matched, monitored,
 * linked or held differently because the numbers disagree.  Seasons 1 and up
 * only: a special is never compared.
 */
static gboolean
compare_absolute_run(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvBring   *bring,
	const GArray        *matched,
	AxisTvBrought       *brought,
	GError             **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) theirs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *id = axis_tv_int_text(series->id);
	g_autofree gchar *json = NULL;
	g_autofree gchar *sentence = NULL;
	AxisTvAbsoluteCompare compare;
	const gchar *params[2];
	gboolean ok;
	guint i;

	for (i = 0; i < bring->n_episodes && i < matched->len; i++) {
		gint64 episode = g_array_index(matched, gint64, i);
		gint64 *key;

		if (episode <= 0 || bring->episodes[i].odd)
			continue;
		key = g_new(gint64, 1);
		*key = episode;
		/* (The app's number + 1, so that "none" is told from "not matched".) */
		g_hash_table_insert(theirs, key, GINT_TO_POINTER(MAX(bring->episodes[i].absolute, 0) + 1));
	}
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT id, season_number, number, coalesce(absolute_number, 0) FROM series_episode"
	        " WHERE series_id = $1::bigint AND gone_at IS NULL AND season_number > 0 ORDER BY season_number, number, id",
	        1, params, &rows, error))
		return FALSE;
	axis_tv_absolute_compare_init(&compare);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 episode = 0;
		gint64 season = 0;
		gint64 number = 0;
		gint64 tvdb = 0;
		gint found;

		axis_db_result_get_int64(rows, i, 0, &episode);
		axis_db_result_get_int64(rows, i, 1, &season);
		axis_db_result_get_int64(rows, i, 2, &number);
		axis_db_result_get_int64(rows, i, 3, &tvdb);
		found = GPOINTER_TO_INT(g_hash_table_lookup(theirs, &episode));
		/* Only an episode both list: one the app does not have, or that did
		 * not match, is no comparison. */
		if (found <= 0)
			continue;
		axis_tv_absolute_compare_add(&compare, (gint)season, (gint)number, found - 1, (gint)tvdb);
	}
	json = axis_tv_absolute_compare_json(&compare);
	sentence = axis_tv_absolute_compare_sentence(&compare);
	brought->compared = TRUE;
	brought->n_absolute_agree = compare.agree;
	brought->n_absolute_differ = compare.differ;
	brought->n_absolute_one_side = compare.one_side;
	axis_tv_absolute_compare_clear(&compare);
	params[1] = json;
	ok = axis_db_conn_exec(conn, "UPDATE series SET brought_absolute = $2::jsonb WHERE id = $1::bigint", 2, params, NULL,
			error) &&
		axis_tv_store_eventf(conn, series, 0, "changed", bring->spec.user, error, "Migrated from %s as anime. %s Axis "
			"numbers it by season, and takes its absolute numbers from TheTVDB alone.", bring->spec.brought_from,
			sentence);
	return ok;
}

/* The comparison, as a report that cannot fail what it reports on: inside a
 * savepoint, so a statement of it that fails leaves the series' transaction
 * usable.  A failure is logged and said in the history, and the migration goes
 * on. */
static gboolean
compare_absolute(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvBring   *bring,
	const GArray        *matched,
	AxisTvBrought       *brought,
	GError             **error
){
	g_autoptr(GError) failure = NULL;

	if (!axis_db_conn_exec(conn, "SAVEPOINT axis_absolute_compare", 0, NULL, NULL, error))
		return FALSE;
	if (!compare_absolute_run(conn, series, bring, matched, brought, &failure)) {
		g_message("series: the absolute numbers of a migrated series could not be compared: %s",
			failure != NULL ? failure->message : "unknown error");
		if (!axis_db_conn_exec(conn, "ROLLBACK TO SAVEPOINT axis_absolute_compare", 0, NULL, NULL, error))
			return FALSE;
		brought->compared = FALSE;
		brought->n_absolute_agree = 0;
		brought->n_absolute_differ = 0;
		brought->n_absolute_one_side = 0;
		axis_tv_store_eventf(conn, series, 0, "changed", bring->spec.user, NULL, "Migrated from %s as anime. Its "
			"absolute numbers could not be compared with TheTVDB's. Nothing was migrated differently because of it.",
			bring->spec.brought_from);
	}
	return axis_db_conn_exec(conn, "RELEASE SAVEPOINT axis_absolute_compare", 0, NULL, NULL, error);
}

gboolean
axis_tv_bring(
	AxisTv             *self,
	AxisDbConn         *conn,
	const AxisTvBring  *bring,
	AxisTvBrought      *brought,
	GError            **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GArray) matched = NULL;
	g_autofree gchar *json = episodes_json(bring->episodes, bring->n_episodes);
	g_autofree gchar *id = NULL;
	AxisTvSpec spec = bring->spec;
	const gchar *params[1];

	memset(brought, 0, sizeof(*brought));
	g_return_val_if_fail(spec.brought_from != NULL, FALSE);

	/* Never searched by the run, and monitored as the app has it, whatever
	 * the caller left in the spec. */
	spec.search_now = FALSE;
	spec.monitor = AXIS_TV_MONITOR_BROUGHT;
	spec.specials = FALSE;
	if (!axis_tv_add_in_transaction(self, conn, &spec, &brought->series_id, error) ||
	    !axis_tv_store_series_one(conn, brought->series_id, &series, error))
		return FALSE;
	if (series == NULL) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	if (!copy_monitoring(conn, series->id, bring, json, error) ||
	    !match_episodes(conn, series->id, bring->episodes, json, bring->n_episodes, &matched,
	        &brought->n_unmatched_episodes, error) ||
	    !link_files(conn, series, bring->episodes, json, bring->n_episodes, matched, bring->files, bring->n_files,
	        spec.user, TRUE, brought, error))
		return FALSE;
	if (!moved_event(conn, series, spec.brought_from, spec.user, brought->n_moved, error))
		return FALSE;
	/* An anime series: the app's absolute numbers beside TheTVDB's, after
	 * everything above was decided, and changing none of it. */
	if (spec.anime && !compare_absolute(conn, series, bring, matched, brought, error))
		return FALSE;
	/* Held, whatever it has: nothing is searched for until a person starts. */
	id = axis_tv_int_text(series->id);
	params[0] = id;
	brought->hold = AXIS_TV_HOLD_BROUGHT;
	return axis_db_conn_exec(conn, "UPDATE series SET search_hold = '" AXIS_TV_HOLD_BROUGHT "' WHERE id = $1::bigint", 1,
		params, NULL, error);
}

gboolean
axis_tv_bring_files(
	AxisTv                    *self,
	AxisDbConn                *conn,
	gint64                     series_id,
	const gchar               *source,
	const AxisTvBringEpisode  *episodes,
	guint                      n_episodes,
	const AxisTvBringFile     *files,
	guint                      n_files,
	const gchar               *origin,
	const gchar               *user,
	AxisTvBrought             *brought,
	GError                   **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GArray) matched = NULL;
	g_autofree gchar *json = episodes_json(episodes, n_episodes);
	gboolean found = FALSE;

	memset(brought, 0, sizeof(*brought));
	brought->series_id = series_id;
	if (!axis_tv_store_lock(conn, series_id, &found, error))
		return FALSE;
	if (!found || !axis_tv_store_series_one(conn, series_id, &series, error) || series == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	brought->hold = series->search_hold != NULL ? g_intern_string(series->search_hold) : NULL;
	/* A person changed its numbering since it was brought: the app's places
	 * are @source's, and the series' rows are another numbering's.  The same
	 * season and number there may be another episode, so nothing is linked. */
	if (g_strcmp0(series->episode_source, source) != 0) {
		brought->other_numbering = TRUE;
		return TRUE;
	}
	/* Only links, and the holds of the episodes that get their file: the
	 * series' monitoring, numbering and own hold are a person's by now. */
	if (!match_episodes(conn, series_id, episodes, json, n_episodes, &matched, &brought->n_unmatched_episodes, error) ||
	    !link_files(conn, series, episodes, json, n_episodes, matched, files, n_files, user, FALSE, brought, error))
		return FALSE;
	/* A link moved is said whether or not another file was linked. */
	if (!moved_event(conn, series, origin, user, brought->n_moved, error))
		return FALSE;
	if (brought->n_linked == 0)
		return TRUE;
	return axis_tv_store_eventf(conn, series, 0, "hand_link", user, error, "Migrated from %s%s%s: %u episode %s "
		"Axis did not see before, or that identification had linked since, %s linked as migrated, and kept. No "
		"file on disk was touched.", origin, user != NULL ? " by " : "",
		user != NULL ? user : "", brought->n_linked, brought->n_linked == 1 ? "file" : "files",
		brought->n_linked == 1 ? "is" : "are");
}
