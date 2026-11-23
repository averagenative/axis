/*
 * axis-tv-import.c - the series' side of the import pipeline: whose a
 * download is (by its grab), what each video file in it holds (episodes of
 * THIS series, read from the file's own name, never another series'), the
 * files the episodes have now, linking a file to every episode it holds,
 * and the series' history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The pipeline imports a series' download file by file (the owner answers
 * match(): see axis-import-run.c).  A file is imported when what its name
 * says it holds exists in the series' data and at least one of those
 * episodes is wanted (monitored with no file, below the cutoff, or one the
 * grab was for); a pack often holds more than was asked, and those wanted
 * are imported too.  Every other file is left in the download, listed with
 * its reason.  A file that holds two episodes is linked to both.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

#include "import/axis-import.h"
#include "identify/axis-identify.h"

/* What a series' import holds on to (the target's owner data). */
typedef struct
{
	AxisTvSeries  *series;
	GPtrArray     *episodes;	/* AxisTvEpisode, not gone */
	AxisTvKnown   *known;
	GHashTable    *files;		/* episode id -> GPtrArray of AxisImportExisting */
	GHashTable    *grabbed;		/* episode ids the grab was for */
	AxisMetaTitle *title;		/* the series, with its other names */
	gint           bound_season;	/* a grab by force for a whole season: it; else -1 */
	gboolean       one_file;	/* the person ticked: one file holds the episodes picked */
	/* What the matcher is given of an anime series (add-anime-support, A2);
	 * for any other series only "not anime". */
	AxisTvAnime    anime;
	/* The grab's release states absolute numbers and no season and episode
	 * (an anime series' alone; not a grab by force): the grab claimed the
	 * episodes those numbers map to, so every file of it, however its own
	 * name states its episode, is placed only on an episode the grab claimed,
	 * and the download is placed whole or not at all (the A3 review). */
	gboolean       grab_absolute;
	/* Whether the download's files that are numbered absolutely can all be
	 * placed (lot_refusal()), kept for the files of one decision. */
	guint          lot_key;
	gboolean       lot_judged;
	gchar         *lot_reason;
} ImportData;

static void
import_data_free(ImportData *d)
{
	g_free(d->lot_reason);
	axis_tv_anime_clear(&d->anime);
	axis_tv_series_free(d->series);
	g_clear_pointer(&d->episodes, g_ptr_array_unref);
	g_free(d->known);
	g_clear_pointer(&d->files, g_hash_table_unref);
	g_clear_pointer(&d->grabbed, g_hash_table_unref);
	axis_meta_title_free(d->title);
	g_free(d);
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

static gchar *
row_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static AxisImportExisting *
existing_copy(const AxisImportExisting *e)
{
	AxisImportExisting *copy = g_new0(AxisImportExisting, 1);

	*copy = *e;
	copy->root_path = g_strdup(e->root_path);
	copy->rel_path = g_strdup(e->rel_path);
	copy->release_name = g_strdup(e->release_name);
	return copy;
}

static gboolean
resolve(
	gpointer            data,
	AxisDbConn         *conn,
	gint64              download_id,
	gint64              owner_id,
	AxisImportTarget  **target_out,
	gchar             **problem,
	GError            **error
){
	AxisTv *self = data;
	g_autoptr(AxisDbResult) grab = NULL;
	g_autoptr(AxisDbResult) profile = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisDbResult) grabbed = NULL;
	g_autoptr(AxisDbResult) alts = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autofree gchar *download = axis_tv_int_text(download_id);
	g_autofree gchar *series_text = NULL;
	g_autofree gchar *ids = NULL;
	g_autofree gchar *unusable = NULL;
	g_autofree gchar *grab_release = NULL;
	const gchar *params[1];
	gint64 series_id = owner_id;
	gint64 grab_id = 0;
	gboolean set_aside = FALSE;
	gboolean numbering_ok = FALSE;
	gboolean bound = FALSE;
	gboolean one_file = FALSE;
	gint bound_season = -1;
	g_autofree gchar *ticked = NULL;
	g_autofree gchar *grabbed_under = NULL;
	g_autofree gchar *grab_by = NULL;
	g_autofree gchar *hold = NULL;
	ImportData *d;
	guint i;

	*target_out = NULL;
	*problem = NULL;
	/* Whose it is: the grab that sent it, and what a person set aside and
	 * ticked when they chose it (the files ticked replacing, those still at
	 * the quality the confirmation showed). */
	if (download_id > 0) {
		params[0] = download;
		if (!axis_db_conn_exec(conn, "SELECT g.id, g.series_id, g.release_name, g.set_aside IS NOT NULL, g.numbering_ok,"
		        " (SELECT string_agg(x->>'f', ',') FROM jsonb_array_elements(g.replaces_files) x JOIN series_file sf"
		        "  ON sf.file_id = (x->>'f')::bigint AND sf.series_id = g.series_id WHERE coalesce(x->>'q', '') ="
		        "  coalesce((SELECT coalesce(c.source_quality, '') FROM conversion c WHERE c.file_id = sf.file_id"
		        "  AND c.state = 'done' AND c.outcome = 'replace' ORDER BY c.id DESC LIMIT 1), sf.quality, '')),"
		        " g.source, g.bound, coalesce(g.bound_season, -1), g.grabbed_by,"
		        " EXISTS (SELECT 1 FROM jsonb_array_elements(g.forced) x WHERE x->>'c' = 'one_file')"
		        " FROM series_grab g WHERE g.download_id = $1::bigint ORDER BY g.closed_at IS NULL DESC, g.id DESC"
		        " LIMIT 1", 1, params, &grab, error))
			return FALSE;
		if (axis_db_result_get_n_rows(grab) > 0) {
			gint64 grabbed_for = row_int(grab, 0, 1, 0);

			if (owner_id <= 0 || owner_id == grabbed_for) {
				series_id = grabbed_for;
				grab_id = row_int(grab, 0, 0, 0);
				grab_release = row_text(grab, 0, 2);
				axis_db_result_get_boolean(grab, 0, 3, &set_aside);
				axis_db_result_get_boolean(grab, 0, 4, &numbering_ok);
				ticked = row_text(grab, 0, 5);
				grabbed_under = row_text(grab, 0, 6);
				/* By force: bound to the episodes it claimed (or the season
				 * named): the series' name in a file's name is not judged. */
				axis_db_result_get_boolean(grab, 0, 7, &bound);
				bound_season = (gint)row_int(grab, 0, 8, -1);
				grab_by = row_text(grab, 0, 9);
				axis_db_result_get_boolean(grab, 0, 10, &one_file);
			}
		}
	}
	if (series_id <= 0) {
		*problem = g_strdup("This download was not grabbed for a series Axis manages: import it by hand, choosing the "
			"series.");
		return TRUE;
	}
	d = g_new0(ImportData, 1);
	if (!axis_tv_store_series_one(conn, series_id, &d->series, error)) {
		import_data_free(d);
		return FALSE;
	}
	if (d->series == NULL) {
		import_data_free(d);
		*problem = g_strdup("The series this download was for is not managed any more.");
		return TRUE;
	}
	/* Grabbed under another numbering than the series follows now: its
	 * files' names would be read against the wrong one (add-metadata-tvdb).
	 * Held for a person, never imported by their names: the target says so
	 * (its hold), and the import is blocked with it until a person assigns
	 * the files by hand, naming the episodes as the series numbers them now
	 * (add-force-grab-and-assign, slice B). */
	if (grabbed_under != NULL && g_strcmp0(grabbed_under, d->series->episode_source) != 0)
		hold = g_strdup_printf("This download was grabbed while the series was numbered as %s numbers it; it "
			"follows %s now, so the names of its files would be read against the wrong numbering and land on other "
			"episodes. Nothing was imported. Select \"Assign by hand\" to say which episodes each file holds as the "
			"series is numbered now, or dismiss it and search again for what is still wanted.",
			axis_tv_source_label(grabbed_under), axis_tv_source_label(d->series->episode_source));
	ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);
	series_text = axis_tv_int_text(series_id);
	params[0] = series_text;
	if (!axis_tv_store_episodes(conn, ids, FALSE, &d->episodes, error) ||
	    !axis_db_conn_exec(conn, "SELECT qp.format, qp.definition FROM series s JOIN quality_profile qp"
	        " ON qp.id = s.quality_profile_id WHERE s.id = $1::bigint", 1, params, &profile, error) ||
	    /* The files its episodes have now: what an import replaces. */
	    !axis_db_conn_exec(conn, "SELECT sfe.episode_id, lf.id, lf.root_id, lf.volume_id, r.rel_path, lf.rel_path,"
	        " lf.inode, lf.size, sf.quality, sf.release_name, sf.resolution, sf.kept,"
	        " coalesce(sf.kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "' AND sf.hand_how = 'brought', false)"
	        " FROM series_file sf"
	        " JOIN series_file_episode sfe ON sfe.file_id = sf.file_id JOIN library_file lf ON lf.id = sf.file_id"
	        " JOIN library_root r ON r.id = lf.root_id WHERE sf.series_id = $1::bigint AND lf.state = 'present'"
	        " AND lf.kind = 'video' ORDER BY lf.id", 1, params, &files, error)) {
		import_data_free(d);
		return FALSE;
	}
	if (grab_id > 0) {
		g_autofree gchar *grab_text = axis_tv_int_text(grab_id);

		params[0] = grab_text;
		if (!axis_db_conn_exec(conn, "SELECT episode_id FROM series_grab_episode WHERE grab_id = $1::bigint", 1, params,
		        &grabbed, error)) {
			import_data_free(d);
			return FALSE;
		}
	}
	{
		g_autofree gchar *title = axis_tv_int_text(d->series->title_id);

		params[0] = title;
		if (!axis_db_conn_exec(conn, "SELECT title FROM meta_alt_title WHERE title_id = $1::bigint ORDER BY title"
		        " LIMIT 100", 1, params, &alts, error)) {
			import_data_free(d);
			return FALSE;
		}
	}
	d->known = g_new0(AxisTvKnown, MAX(d->episodes->len, 1));
	for (i = 0; i < d->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, i);

		d->known[i].id = e->id;
		d->known[i].season = e->season;
		d->known[i].number = e->number;
		d->known[i].air_date = e->air_date;
	}
	/* An anime series: its absolute numbers as they are now, at import time,
	 * and its names, for files named by absolute number. */
	{
		AxisTvRules rules;

		axis_tv_get_rules(self, &rules);
		if (!axis_tv_anime_init(&d->anime, conn, d->series, d->episodes, rules.today, error)) {
			import_data_free(d);
			return FALSE;
		}
	}
	/* Whether the release was grabbed by absolute numbers: by what its name
	 * states (nothing but absolute numbers, read by itself or after the
	 * series' name), whatever those numbers map to now. */
	if (d->anime.read.anime && grab_release != NULL && !(set_aside && bound)) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(grab_release);
		AxisReleaseStates states;

		axis_release_states(parsed, &states);
		if (states.kind == AXIS_RELEASE_HOLDS_NOTHING && g_strcmp0(states.code, "absolute_only") == 0 &&
		    !states.with_season) {
			d->grab_absolute = TRUE;
		} else if (states.kind == AXIS_RELEASE_HOLDS_NOTHING && g_strcmp0(states.code, "no_episode") == 0 &&
		           d->anime.read.titles != NULL) {
			AxisReleaseAfter after;

			axis_release_read_after(grab_release, d->anime.read.titles, d->anime.read.n_titles, &after);
			d->grab_absolute = after.read;
		}
	}
	d->files = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		gint64 episode = row_int(files, i, 0, 0);
		GPtrArray *list = g_hash_table_lookup(d->files, &episode);
		AxisImportExisting *existing = g_new0(AxisImportExisting, 1);
		const gchar *quality = axis_db_result_get_text(files, i, 8);

		if (list == NULL) {
			gint64 *key = g_new(gint64, 1);

			*key = episode;
			list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_existing_free);
			g_hash_table_insert(d->files, key, list);
		}
		existing->library_file_id = row_int(files, i, 1, 0);
		existing->root_id = row_int(files, i, 2, 0);
		existing->volume_id = row_int(files, i, 3, 0);
		existing->root_path = row_text(files, i, 4);
		existing->rel_path = row_text(files, i, 5);
		existing->inode = row_int(files, i, 6, 0);
		existing->size = row_int(files, i, 7, 0);
		if (quality == NULL || !axis_quality_from_name(quality, &existing->quality))
			existing->quality = AXIS_QUALITY_UNKNOWN;
		existing->release_name = row_text(files, i, 9);
		axis_db_result_get_boolean(files, i, 11, &existing->kept);
		{
			gboolean brought = FALSE;

			axis_db_result_get_boolean(files, i, 12, &brought);
			existing->kept_from = existing->kept && brought ? "Sonarr" : NULL;
		}
		existing->resolution = AXIS_RELEASE_RESOLUTION_NONE;
		/* A file of an unknown source found on disk: its resolution. */
		if (existing->quality == AXIS_QUALITY_UNKNOWN && axis_db_result_get_text(files, i, 10) != NULL) {
			AxisReleaseResolution r;

			for (r = AXIS_RELEASE_RESOLUTION_480P; r <= AXIS_RELEASE_RESOLUTION_2160P; r++) {
				if (g_strcmp0(axis_release_resolution_name(r), axis_db_result_get_text(files, i, 10)) == 0)
					existing->resolution = r;
			}
		}
		g_ptr_array_add(list, existing);
	}
	d->grabbed = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; grabbed != NULL && i < axis_db_result_get_n_rows(grabbed); i++) {
		gint64 *key = g_new(gint64, 1);

		*key = row_int(grabbed, i, 0, 0);
		g_hash_table_add(d->grabbed, key);
	}
	d->title = axis_meta_title_new(AXIS_META_KIND_SERIES);
	d->title->id = d->series->title_id;
	d->title->title = g_strdup(d->series->title);
	d->title->original_title = g_strdup(d->series->original_title);
	d->title->year = d->series->year;
	if (axis_db_result_get_n_rows(alts) > 0)
		d->title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	for (i = 0; i < axis_db_result_get_n_rows(alts); i++) {
		AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

		alt->title = row_text(alts, i, 0);
		g_ptr_array_add(d->title->alt_titles, alt);
	}

	target = axis_import_target_new();
	target->owner_id = series_id;
	target->grab_id = grab_id;
	target->grab_set_aside = set_aside;
	target->grab_numbering_ok = set_aside && numbering_ok;
	target->grab_ticked = axis_import_file_ids(set_aside ? ticked : NULL);
	target->grab_bound = set_aside && bound;
	target->grab_by = g_steal_pointer(&grab_by);
	target->hold = g_steal_pointer(&hold);
	/* A review of an assignment by hand is of this numbering. */
	target->stamp = g_strdup(d->series->episode_source);
	d->bound_season = target->grab_bound ? bound_season : -1;
	d->one_file = target->grab_bound && one_file;
	target->grab_release = g_steal_pointer(&grab_release);
	target->title_id = d->series->title_id;
	target->title = g_strdup(d->series->title);
	target->original_title = g_strdup(d->series->original_title);
	target->year = d->series->year;
	target->runtime_minutes = d->series->runtime;
	target->root_id = d->series->root_id;
	target->label = axis_tv_series_label(d->series->title, d->series->year);
	if (axis_db_result_get_n_rows(profile) > 0)
		target->profile_ok = axis_quality_stored_check((gint)row_int(profile, 0, 0, 0),
			axis_db_result_get_text(profile, 0, 1), &target->profile, &unusable);
	target->tmdb_id = g_strdup(d->series->tmdb_id);
	target->imdb_id = g_strdup(d->series->imdb_id);
	target->tvdb_id = g_strdup(d->series->tvdb_id);
	target->languages = axis_acquire_setting_languages(self->acquire);
	target->owner_data = d;
	target->owner_free = (GDestroyNotify)import_data_free;
	*target_out = g_steal_pointer(&target);
	return TRUE;
}

static gint
compare_ints(
	gconstpointer a,
	gconstpointer b
){
	gint x = *(const gint *)a;
	gint y = *(const gint *)b;

	return x < y ? -1 : x > y;
}

/* Whether every episode of @indices (into the known episodes) is one the
 * grab claimed. */
static gboolean
all_grabbed(
	const ImportData *d,
	const GArray     *indices
){
	guint i;

	for (i = 0; i < indices->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, g_array_index(indices, guint, i));

		if (!g_hash_table_contains(d->grabbed, &e->id))
			return FALSE;
	}
	return TRUE;
}

/* The same for episode ids. */
static gboolean
grabbed_ids(
	const ImportData *d,
	const GArray     *ids
){
	guint i;

	for (i = 0; i < ids->len; i++) {
		if (!g_hash_table_contains(d->grabbed, &g_array_index(ids, gint64, i)))
			return FALSE;
	}
	return ids->len > 0;
}

/* The index (into the episodes) of the claimed episode @number of @season;
 * -1 when the grab claimed no such episode. */
static gint
claimed_index(
	const ImportData *d,
	gint              season,
	gint              number
){
	guint i;

	for (i = 0; i < d->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, i);

		if (e->season == season && e->number == number && g_hash_table_contains(d->grabbed, &e->id))
			return (gint)i;
	}
	return -1;
}

/* Whether the series has episodes of @season under the numbering followed. */
static gboolean
season_exists(
	const ImportData *d,
	gint              season
){
	guint i;

	for (i = 0; i < d->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, i);

		if (e->season == season)
			return TRUE;
	}
	return FALSE;
}

/* Whether any video file of the download states @season in its own name
 * (then the pack is numbered as the series is). */
static gboolean
download_states_season(
	const AxisImportTarget *target,
	gint                    season
){
	guint i;

	if (target->grab_names == NULL)
		return FALSE;
	for (i = 0; i < target->grab_names->len; i++) {
		const gchar *name = g_ptr_array_index(target->grab_names, i);
		g_autofree gchar *ext = axis_import_extension(name, strlen(name));
		g_autofree gchar *stem = ext != NULL ? g_strndup(name, strlen(name) - strlen(ext)) : g_strdup(name);
		g_autoptr(AxisRelease) release = axis_release_parse(stem);

		if (release->season_first == season ||
		    (release->season_first >= 0 && release->season_first <= season && season <= release->season_last))
			return TRUE;
	}
	return FALSE;
}

/* Whether the download may be re-read within the bound season at all: decided
 * once for every video file, not per file.  Of the files that would be re-read
 * (the season they state is not the series', or they state none), all must
 * state one and the same season, or none a season; else none is re-read. */
static gboolean
reread_agreed(
	const ImportData       *d,
	const AxisImportTarget *target
){
	gint seen = -3;		/* -3: nothing yet; -1: no season; -2: a range */
	guint i;

	if (target->grab_names == NULL)
		return TRUE;
	for (i = 0; i < target->grab_names->len; i++) {
		const gchar *name = g_ptr_array_index(target->grab_names, i);
		g_autofree gchar *ext = axis_import_extension(name, strlen(name));
		g_autofree gchar *stem = ext != NULL ? g_strndup(name, strlen(name) - strlen(ext)) : g_strdup(name);
		g_autoptr(AxisRelease) release = axis_release_parse(stem);
		gint key = release->season_first < 0 ? -1 : release->season_last > release->season_first ? -2 :
			release->season_first;

		/* A file that states no episode is never re-read, so it votes for
		 * nothing; nor is one of a season the series has. */
		if (release->episode_count == 0 || (key >= 0 && season_exists(d, key)))
			continue;
		if (seen == -3)
			seen = key;
		else if (seen != key || key == -2)
			return FALSE;
	}
	return TRUE;
}

static gint
compare_uints(
	gconstpointer a,
	gconstpointer b
){
	guint x = *(const guint *)a;
	guint y = *(const guint *)b;

	return x < y ? -1 : x > y;
}

static AxisImportUnit *unit_by_hand(const ImportData *d, GArray *indices, const gchar *note);

/*
 * A file of a grab by force (add-force-grab-and-assign, F2).  The person
 * named what the release is (the episodes the grab claimed, or a whole
 * season), so the series' name in the file's name is not judged, and nothing
 * is guessed:
 *
 *  - The download's one video file, the grab made for episodes named one by
 *    one (of one season, as many as a file can hold): it is those episodes,
 *    and its name is not read.
 *  - Otherwise the file's own name is read for its episodes.  When the season
 *    and numbers it states are all episodes the grab claimed, it is those.
 *    Else, when everything claimed is in one season (a season named, or
 *    episodes of one season), the episode numbers it states are read within
 *    that season, whatever season the name says (the release's own numbering
 *    is what was set aside), and it is those when the grab claimed each.
 *  - Any other file is left in the download, to be assigned by hand.
 */
static gboolean
bound_match(
	const ImportData        *d,
	const AxisImportTarget  *target,
	const AxisRelease       *parsed,
	const AxisTvAnimeRead   *read,
	AxisImportUnit         **unit_out,
	gchar                  **reason
){
	g_autoptr(GArray) indices = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autofree gchar *by_absolute = NULL;
	gint season = d->bound_season;
	gboolean one_season = TRUE;
	guint n_claimed = 0;
	guint i;

	/* The season everything claimed is in, if it is one. */
	for (i = 0; i < d->episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, i);

		if (!g_hash_table_contains(d->grabbed, &e->id))
			continue;
		n_claimed++;
		if (season < 0)
			season = e->season;
		else if (e->season != season)
			one_season = FALSE;
	}
	if (d->bound_season < 0 && target->grab_videos == 1 && one_season && n_claimed > 0 &&
	    (n_claimed == 1 || d->one_file) && n_claimed <= AXIS_IMPORT_EPISODES_MAX) {
		/* The one file is what was named: the one episode, or the several the
		 * person ticked one file holds.  Several picked and no such tick: the
		 * file's own name must state them (below), else it waits for assigning
		 * (a sample, or a file gone on a rerun, must not make a double
		 * episode). */
		for (i = 0; i < d->episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(d->episodes, i);

			if (g_hash_table_contains(d->grabbed, &e->id))
				g_array_append_val(indices, i);
		}
	} else {
		AxisTvHolds holds;

		/* (An anime series' file named by its absolute number is the episode
		 * that has the number, when the grab claimed it.) */
		axis_tv_match_release_anime(parsed, d->known, d->episodes->len, read, &holds);
		if (holds.ok && !holds.pack && holds.indices->len <= AXIS_IMPORT_EPISODES_MAX &&
		    all_grabbed(d, holds.indices)) {
			g_array_append_vals(indices, holds.indices->data, holds.indices->len);
			if (holds.by_absolute)
				by_absolute = g_strdup_printf(AXIS_TV_ABSOLUTE_NOTE ": %s (TheTVDB's numbering as of %s). The release was "
					"grabbed by force for it: the series' name in the file's name was not judged.", holds.absolute_text,
					read != NULL && read->today != NULL ? read->today : "today");
		} else if (one_season && season >= 0 && parsed->episode_count > 0 &&
		           parsed->episode_count <= AXIS_IMPORT_EPISODES_MAX && parsed->season_last <= parsed->season_first &&
		           (parsed->season_first < 0 || !season_exists(d, parsed->season_first)) &&
		           !download_states_season(target, season) && reread_agreed(d, target)) {
			/* Its episode numbers, within the season named. */
			for (i = 0; i < parsed->episode_count; i++) {
				gint at = claimed_index(d, season, parsed->episodes[i]);
				guint index = (guint)at;

				if (at < 0) {
					g_array_set_size(indices, 0);
					break;
				}
				g_array_append_val(indices, index);
			}
		}
		axis_tv_holds_clear(&holds);
	}
	if (indices->len == 0) {
		*reason = g_strdup("its name does not say which of the episodes this release was grabbed for by force it is, "
			"and nothing is guessed: assign it by hand");
		return FALSE;
	}
	*unit_out = unit_by_hand(d, indices, by_absolute != NULL ? by_absolute : "The release was grabbed by force for it: "
		"the series' name in the file's name was not judged.");
	return TRUE;
}

/* The unit of the episodes at @indices (into the episodes; sorted here) that
 * a person named, by a grab by force or by assigning the file by hand: its
 * episodes of one season, the files they have now, and @note. */
static AxisImportUnit *
unit_by_hand(
	const ImportData *d,
	GArray           *indices,
	const gchar      *note
){
	g_autoptr(GHashTable) seen = g_hash_table_new(g_int64_hash, g_int64_equal);
	AxisImportUnit *unit;
	GArray *ids;
	guint i;

	g_array_sort(indices, compare_uints);
	unit = axis_import_unit_new();
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < indices->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, g_array_index(indices, guint, i));
		GPtrArray *files = g_hash_table_lookup(d->files, &e->id);
		guint k;

		g_array_append_val(ids, e->id);
		unit->season = e->season;
		unit->episodes[unit->n_episodes++] = e->number;
		unit->runtime_minutes += e->runtime > 0 ? e->runtime : d->series->runtime;
		if (unit->episode_title == NULL && e->title != NULL)
			unit->episode_title = g_strdup(e->title);
		if (unit->air_date == NULL && e->air_date != NULL)
			unit->air_date = g_strdup(e->air_date);
		for (k = 0; files != NULL && k < files->len; k++) {
			const AxisImportExisting *existing = g_ptr_array_index(files, k);

			if (g_hash_table_contains(seen, &existing->library_file_id))
				continue;
			g_hash_table_add(seen, (gpointer)&existing->library_file_id);
			g_ptr_array_add(unit->existing, existing_copy(existing));
		}
	}
	{
		g_autofree gchar *first = axis_tv_episode_label(unit->season, unit->episodes[0]);

		unit->label = unit->n_episodes > 1 ? g_strdup_printf("%s-E%02d", first, unit->episodes[unit->n_episodes - 1]) :
			g_strdup(first);
	}
	unit->by_hand = TRUE;
	unit->note = g_strdup(note);
	unit->owner_data = ids;
	unit->owner_free = (GDestroyNotify)g_array_unref;
	return unit;
}

/* ── Assigned by hand (add-force-grab-and-assign, slice B) ──────── */

/* "S01E03 S01E04", as a person writes what a file holds: episodes of the
 * series in the numbering it follows now, of one season, as many as one file
 * can hold.  Their ids, in the order of the data. */
static gboolean
assign_read(
	gpointer                 data,
	const AxisImportTarget  *target,
	const gchar             *said,
	GArray                 **ids_out,
	gchar                  **problem
){
	const ImportData *d = target->owner_data;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_auto(GStrv) tokens = NULL;
	gint season = -1;
	guint i;
	guint k;

	*ids_out = NULL;
	*problem = NULL;
	if (d == NULL || said == NULL || strlen(said) > 400) {
		*problem = g_strdup("That is too long to be a list of episodes");
		return FALSE;
	}
	tokens = g_strsplit_set(said, " ,;\t\r\n", -1);
	for (i = 0; tokens[i] != NULL; i++) {
		const gchar *token = tokens[i];
		gchar *end = NULL;
		gint64 s;
		gint64 n;
		gint64 id = 0;

		if (token[0] == '\0')
			continue;
		if (strlen(token) > 16 || (token[0] != 's' && token[0] != 'S') || !g_ascii_isdigit(token[1]))
			goto bad;
		s = g_ascii_strtoll(token + 1, &end, 10);
		if (end == NULL || (*end != 'e' && *end != 'E') || !g_ascii_isdigit(end[1]))
			goto bad;
		n = g_ascii_strtoll(end + 1, &end, 10);
		if (end == NULL || *end != '\0' || s < 0 || s > 100000 || n < 0 || n > 100000)
			goto bad;
		for (k = 0; k < d->episodes->len && id == 0; k++) {
			const AxisTvEpisode *e = g_ptr_array_index(d->episodes, k);

			if (e->season == s && e->number == n)
				id = e->id;
		}
		if (id == 0) {
			g_autofree gchar *label = axis_tv_episode_label((gint)s, (gint)n);

			*problem = g_strdup_printf("The series has no episode %s in the numbering it follows", label);
			return FALSE;
		}
		if (season >= 0 && season != s) {
			*problem = g_strdup("One file holds episodes of one season: these are of two");
			return FALSE;
		}
		season = (gint)s;
		for (k = 0; k < ids->len; k++) {
			if (g_array_index(ids, gint64, k) == id)
				break;
		}
		if (k == ids->len)
			g_array_append_val(ids, id);
		if (ids->len > AXIS_IMPORT_EPISODES_MAX) {
			*problem = g_strdup_printf("One file can hold at most %d episodes", AXIS_IMPORT_EPISODES_MAX);
			return FALSE;
		}
		continue;
	bad:
		*problem = g_strdup_printf("\"%s\" is not an episode: write each as S01E03, with a space between two", token);
		return FALSE;
	}
	if (ids->len == 0) {
		*problem = g_strdup("Say which episodes it holds, each as S01E03, with a space between two");
		return FALSE;
	}
	*ids_out = g_steal_pointer(&ids);
	return TRUE;
}

/* The unit of the episodes a person assigned a file to: its name is not
 * read, and whether they are wanted is not asked. */
static gboolean
assigned(
	gpointer                 data,
	const AxisImportTarget  *target,
	const GArray            *ids,
	AxisImportUnit         **unit_out,
	gchar                  **reason
){
	const ImportData *d = target->owner_data;
	g_autoptr(GArray) indices = g_array_new(FALSE, FALSE, sizeof(guint));
	gint season = -1;
	guint i;
	guint k;

	*unit_out = NULL;
	*reason = NULL;
	for (i = 0; d != NULL && ids != NULL && i < ids->len; i++) {
		gboolean found = FALSE;

		for (k = 0; k < d->episodes->len && !found; k++) {
			const AxisTvEpisode *e = g_ptr_array_index(d->episodes, k);

			if (e->id != g_array_index(ids, gint64, i))
				continue;
			found = season < 0 || season == e->season;
			season = e->season;
			if (found)
				g_array_append_val(indices, k);
		}
		if (!found) {
			*reason = g_strdup("an episode it was assigned to is not in the series' data any more (the numbering or "
				"the data changed since)");
			return FALSE;
		}
	}
	if (indices->len == 0 || indices->len > AXIS_IMPORT_EPISODES_MAX) {
		*reason = g_strdup("it was assigned to no episode");
		return FALSE;
	}
	*unit_out = unit_by_hand(d, indices, "Assigned by hand: its name was not read.");
	return TRUE;
}

/* The series is locked while its files are assigned, as it is for every
 * change of its links. */
static gboolean
assign_lock(
	gpointer     data,
	AxisDbConn  *conn,
	gint64       owner_id,
	GError     **error
){
	gboolean found = FALSE;

	return axis_tv_store_lock(conn, owner_id, &found, error);
}

/* An episode another grab holds is that grab's: its download is on the way
 * for it.  (The import's own grab is expected.) */
static gboolean
assign_claimed(
	gpointer                 data,
	AxisDbConn              *conn,
	const AxisImportTarget  *target,
	const GArray            *ids,
	gchar                  **problem,
	GError                 **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) array = g_string_new("{");
	g_autofree gchar *grab = axis_tv_int_text(target->grab_id);
	const gchar *params[2];
	guint i;

	*problem = NULL;
	for (i = 0; ids != NULL && i < ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(array, '}');
	params[0] = array->str;
	params[1] = grab;
	if (!axis_db_conn_exec(conn, "SELECT 'S' || lpad(e.season_number::text, 2, '0') || 'E' || lpad(e.number::text, 2, '0'),"
	        " g.release_name FROM series_grab_episode ge JOIN series_grab g ON g.id = ge.grab_id"
	        " JOIN series_episode e ON e.id = ge.episode_id WHERE ge.episode_id = ANY ($1::bigint[])"
	        " AND g.closed_at IS NULL AND g.id <> $2::bigint ORDER BY e.season_number, e.number LIMIT 1", 2, params,
	        &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0)
		*problem = g_strdup_printf("%s is being downloaded by another grab (%s). Wait for it, or remove that download "
			"on the Downloads page, then assign again.", axis_db_result_get_text(rows, 0, 0),
			axis_db_result_get_text(rows, 0, 1) != NULL ? axis_db_result_get_text(rows, 0, 1) : "a release");
	return TRUE;
}

/*
 * A download of an anime series whose files are numbered absolutely is placed
 * whole or not at all (add-anime-support, A2).  One file whose number maps to
 * no episode, to an episode not aired, or to an episode the grab did not
 * claim is the first sign of a mapping that is off by some episodes, and then
 * every other file's place is in doubt too.  So when any video file of the
 * download is numbered absolutely and cannot be placed, none that is read by
 * an absolute number is: they wait for "Assign by hand".  A file that states
 * a season and an episode, a special named by a word and a file that names no
 * episode at all stand in nobody's way.
 *
 * Returns: (nullable) (transfer none): why the lot is refused, naming the
 * first file at fault; %NULL when it is not.
 */
static const gchar *
lot_refusal(
	ImportData              *d,
	const AxisImportTarget  *target
){
	g_autoptr(GHashTable) covered = g_hash_table_new(g_int64_hash, g_int64_equal);
	const gchar *stray = NULL;
	guint key = 17;
	guint i;

	if (!d->anime.read.anime || target->grab_names == NULL)
		return NULL;
	for (i = 0; i < target->grab_names->len; i++)
		key = key * 31 + g_str_hash(g_ptr_array_index(target->grab_names, i));
	if (d->lot_judged && d->lot_key == key)
		return d->lot_reason;
	g_clear_pointer(&d->lot_reason, g_free);
	d->lot_judged = TRUE;
	d->lot_key = key;
	for (i = 0; i < target->grab_names->len && d->lot_reason == NULL; i++) {
		const gchar *name = g_ptr_array_index(target->grab_names, i);
		g_autofree gchar *ext = axis_import_extension(name, strlen(name));
		g_autofree gchar *stem = ext != NULL ? g_strndup(name, strlen(name) - strlen(ext)) : g_strdup(name);
		g_autoptr(AxisRelease) release = axis_release_parse(stem);
		AxisTvAnimeRead read = d->anime.read;
		AxisTvHolds holds;

		read.name = name;
		/* Another title's file is not this series', as in match(). */
		if (release->title != NULL) {
			const AxisMetaTitle *candidates[1];

			candidates[0] = d->title;
			if (axis_release_match(release, candidates, 1, NULL, NULL) == AXIS_RELEASE_MATCH_NONE &&
			    !axis_tv_anime_claims(release, &read, NULL, NULL)) {
				/* (Unless it begins with the series' name and what follows is
				 * a doubtful number: that is a file numbered absolutely, and
				 * match() leaves it for that reason.  So does the lot: the
				 * matcher is not asked about a name match() never gives it.) */
				AxisReleaseAfter after;

				axis_release_read_after(name, read.titles, read.n_titles, &after);
				if (after.titled && after.code != NULL && strcmp(after.code, "no_number") != 0 &&
				    strcmp(after.code, "special") != 0 && axis_release_after_words(after.code) != NULL)
					d->lot_reason = g_strdup_printf("another file of this download, %s, begins with the series' name "
						"and is not read: %s", name, axis_release_after_words(after.code));
				else if (stray == NULL)
					stray = name;
				continue;
			}
		}
		axis_tv_match_release_anime(release, d->known, d->episodes->len, &read, &holds);
		if (holds.ok && holds.by_absolute && !all_grabbed(d, holds.indices))
			d->lot_reason = g_strdup_printf("another file of this download, %s, gives %s by its absolute number (%s), "
				"which this download was not grabbed for", name, holds.label, holds.absolute_text);
		else if (!holds.ok && holds.code != NULL && (g_str_has_prefix(holds.code, "absolute_") ||
		         strcmp(holds.code, "season_and_absolute") == 0 || strcmp(holds.code, "numberings_differ") == 0))
			d->lot_reason = g_strdup_printf("another file of this download, %s, is numbered absolutely and cannot be "
				"placed: %s", name, holds.text);
		/* A download grabbed by absolute numbers: a file that states a season
		 * and an episode the grab did not claim is outside the claim too. */
		else if (d->grab_absolute && holds.ok && !holds.pack && !all_grabbed(d, holds.indices))
			d->lot_reason = g_strdup_printf("another file of this download, %s, gives %s, which this download was not "
				"grabbed for", name, holds.label);
		else if (holds.ok && !holds.pack) {
			guint k;

			for (k = 0; k < holds.indices->len; k++) {
				const AxisTvEpisode *e = g_ptr_array_index(d->episodes, g_array_index(holds.indices, guint, k));

				g_hash_table_add(covered, (gpointer)&e->id);
			}
		} else if (stray == NULL) {
			stray = name;
		}
		axis_tv_holds_clear(&holds);
	}
	/* A download grabbed by absolute numbers whose files' names do not account
	 * for every episode it was grabbed for, while one of its files does not
	 * say which episode it is: that file may be the missing one.  Placing the
	 * others would end the import as done, remove the download and leave the
	 * episode to be searched for again (and this very release grabbed again):
	 * nothing is placed, and a person assigns the files. */
	if (d->grab_absolute && d->lot_reason == NULL && stray != NULL &&
	    g_hash_table_size(covered) < g_hash_table_size(d->grabbed))
		d->lot_reason = g_strdup_printf("another file of this download, %s, does not say which episode it is, and the "
			"names of the files account for %u of the %u episodes the download was grabbed for", stray,
			g_hash_table_size(covered), g_hash_table_size(d->grabbed));
	return d->lot_reason;
}

static gboolean
match(
	gpointer                 data,
	const AxisImportTarget  *target,
	const AxisRelease       *parsed,
	const gchar             *file_name,
	AxisImportUnit         **unit_out,
	gchar                  **reason
){
	ImportData *d = target->owner_data;
	g_autoptr(GHashTable) seen = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GArray) numbers = g_array_new(FALSE, FALSE, sizeof(gint));
	g_autofree gchar *note = NULL;
	AxisTvAnimeRead read;
	AxisTvHolds holds;
	AxisImportUnit *unit;
	GArray *ids;
	gboolean wanted = FALSE;
	guint i;

	*unit_out = NULL;
	*reason = NULL;
	if (d == NULL || parsed == NULL)
		return FALSE;
	/* An anime series: the file's own name, for a name numbered absolutely.
	 * For any other series this says "not anime" and nothing more. */
	read = d->anime.read;
	read.name = read.anime ? file_name : NULL;
	/* By force: bound to what the person named. */
	if (target->grab_bound)
		return bound_match(d, target, parsed, &read, unit_out, reason);
	/* A name that states a title states this series' (with its other
	 * names), or it is not this series' file. */
	if (parsed->title != NULL) {
		const AxisMetaTitle *candidates[1];
		gint matched = -1;

		candidates[0] = d->title;
		if (axis_release_match(parsed, candidates, 1, &matched, NULL) == AXIS_RELEASE_MATCH_NONE) {
			g_autofree gchar *why_not = NULL;

			/* An anime series' name with a bare number after it reads, by
			 * itself, as another title ("Tenkuu Rangers 137"). */
			if (!axis_tv_anime_claims(parsed, &read, NULL, &why_not)) {
				*reason = why_not != NULL ?
					g_strdup_printf("its name begins with the series' name, but %s", why_not) :
					g_strdup_printf("its name says \"%s\", not %s", parsed->title, target->label);
				return FALSE;
			}
		}
	}
	axis_tv_match_release_anime(parsed, d->known, d->episodes->len, &read, &holds);
	/* A release a person took although it numbers its episodes otherwise
	 * than the data, as its confirmation said: a file that holds episodes
	 * the grab claimed and a number the data does not have is placed for
	 * the claimed ones alone, the other number noted.  Never otherwise. */
	if (!holds.ok && target->grab_numbering_ok && holds.found != NULL && holds.found->len > 0 &&
	    all_grabbed(d, holds.found)) {
		guint first = g_array_index(holds.found, guint, 0);

		note = g_strdup_printf("Its name also says %s, which the series' data does not have: it is placed for %s "
			"alone.", holds.missing, holds.found_label);
		g_array_set_size(holds.indices, 0);
		g_array_append_vals(holds.indices, holds.found->data, holds.found->len);
		g_free(holds.label);
		holds.label = g_strdup(holds.found_label);
		holds.season_first = d->known[first].season;
		holds.season_last = d->known[first].season;
		holds.ok = TRUE;
	}
	if (!holds.ok) {
		*reason = g_steal_pointer(&holds.text);
		axis_tv_holds_clear(&holds);
		return FALSE;
	}
	if (holds.pack) {
		*reason = g_strdup_printf("its name says it is a whole season (%s), not an episode", holds.label);
		axis_tv_holds_clear(&holds);
		return FALSE;
	}
	if (holds.indices->len > AXIS_IMPORT_EPISODES_MAX) {
		*reason = g_strdup_printf("it holds more episodes (%u) than a name can state", holds.indices->len);
		axis_tv_holds_clear(&holds);
		return FALSE;
	}
	/* Read by an absolute number: placed only on an episode its grab
	 * claimed, whatever else is wanted.  The claim was made by episode id, so
	 * a number TheTVDB moved since the grab places nothing on an episode
	 * nobody decided on. */
	if (holds.by_absolute) {
		const gchar *lot;

		if (!all_grabbed(d, holds.indices)) {
			*reason = g_strdup_printf("its name gives %s by its absolute number (%s), and this download was not "
				"grabbed for that episode: a file read by an absolute number is placed only on an episode its grab "
				"claimed. Assign it by hand", holds.label, holds.absolute_text);
			axis_tv_holds_clear(&holds);
			return FALSE;
		}
		/* Whole or not at all. */
		lot = lot_refusal(d, target);
		if (lot != NULL) {
			*reason = g_strdup_printf("its name gives %s by its absolute number (%s), but %s. A download whose files "
				"are numbered absolutely is placed whole or not at all, since one number that does not fit puts the "
				"others in doubt. Assign the files by hand", holds.label, holds.absolute_text, lot);
			axis_tv_holds_clear(&holds);
			return FALSE;
		}
		note = g_strdup_printf(AXIS_TV_ABSOLUTE_NOTE ": %s (TheTVDB's numbering as of %s).", holds.absolute_text,
			read.today != NULL ? read.today : "today");
	} else if (d->grab_absolute) {
		/* The download was grabbed by absolute numbers, for the episodes those
		 * map to and no other.  A file of it that states a season and an
		 * episode is held to the same claim (it was placed on whatever wanted
		 * episode its name said: the A3 review's blocker), and to the lot. */
		const gchar *lot;

		if (!all_grabbed(d, holds.indices)) {
			*reason = g_strdup_printf("it holds %s, and this download was grabbed by absolute numbers for other "
				"episodes: a file of such a download is placed only on an episode its grab claimed. Assign it by hand",
				holds.label);
			axis_tv_holds_clear(&holds);
			return FALSE;
		}
		lot = lot_refusal(d, target);
		if (lot != NULL) {
			*reason = g_strdup_printf("it holds %s, but %s. A download that was grabbed by absolute numbers is placed "
				"whole or not at all, since one file that does not fit puts the others in doubt. Assign the files by "
				"hand", holds.label, lot);
			axis_tv_holds_clear(&holds);
			return FALSE;
		}
	}
	unit = axis_import_unit_new();
	ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	for (i = 0; i < holds.indices->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(d->episodes, g_array_index(holds.indices, guint, i));
		GPtrArray *files = g_hash_table_lookup(d->files, &e->id);
		guint k;

		g_array_append_val(ids, e->id);
		g_array_append_val(numbers, e->number);
		if (e->monitored && (e->n_files == 0 || axis_tv_episode_upgradable(e, d->series)))
			wanted = TRUE;
		if (g_hash_table_contains(d->grabbed, &e->id))
			wanted = TRUE;
		unit->runtime_minutes += e->runtime > 0 ? e->runtime : d->series->runtime;
		if (unit->episode_title == NULL && e->title != NULL)
			unit->episode_title = g_strdup(e->title);
		if (unit->air_date == NULL && e->air_date != NULL)
			unit->air_date = g_strdup(e->air_date);
		/* The files these episodes have now, each once (a double episode's
		 * file is both's). */
		for (k = 0; files != NULL && k < files->len; k++) {
			const AxisImportExisting *existing = g_ptr_array_index(files, k);

			if (g_hash_table_contains(seen, &existing->library_file_id))
				continue;
			g_hash_table_add(seen, (gpointer)&existing->library_file_id);
			g_ptr_array_add(unit->existing, existing_copy(existing));
		}
	}
	if (!wanted) {
		*reason = g_strdup_printf("it holds %s, which is not wanted (in the library already, or not monitored)",
			holds.label);
		axis_import_unit_free(unit);
		g_array_unref(ids);
		axis_tv_holds_clear(&holds);
		return FALSE;
	}
	g_array_sort(numbers, compare_ints);
	unit->season = holds.season_first;
	unit->n_episodes = numbers->len;
	for (i = 0; i < numbers->len; i++)
		unit->episodes[i] = g_array_index(numbers, gint, i);
	unit->label = g_strdup(holds.label);
	/* What a person set aside is for the episodes their grab claimed: a
	 * file of any other is judged as any import's. */
	unit->by_hand = target->grab_set_aside && grabbed_ids(d, ids);
	unit->note = g_steal_pointer(&note);
	unit->owner_data = ids;
	unit->owner_free = (GDestroyNotify)g_array_unref;
	axis_tv_holds_clear(&holds);
	*unit_out = unit;
	return TRUE;
}

static gboolean
link_file(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const AxisImportLinked *linked,
	GError                **error
){
	const GArray *ids = linked->unit != NULL ? linked->unit->owner_data : NULL;
	g_autoptr(GString) array = g_string_new("{");
	g_autofree gchar *file = axis_tv_int_text(linked->library_file_id);
	g_autofree gchar *series = axis_tv_int_text(target->owner_id);
	g_autofree gchar *import = axis_tv_int_text(linked->import_id);
	g_autofree gchar *release = axis_tv_text_cut(linked->release_name, 300);
	g_autofree gchar *quality_text = axis_tv_text_cut(linked->quality_text, 1000);
	/* A file a person assigned by hand is theirs; else the file of a grab by
	 * force is the person's who forced it. */
	g_autofree gchar *by = axis_tv_text_cut(linked->hand_how != NULL && linked->hand_by != NULL ? linked->hand_by :
		target->grab_by != NULL ? target->grab_by : linked->user, 200);
	gboolean forced = target->grab_bound && linked->unit != NULL && linked->unit->by_hand;
	const gchar *how = linked->hand_how != NULL ? linked->hand_how : forced ? "forced" : NULL;
	const gchar *params[9];
	guint i;

	if (ids == NULL || ids->len == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "a series' file must hold an episode");
		return FALSE;
	}
	for (i = 0; i < ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(array, '}');
	params[0] = file;
	params[1] = series;
	params[2] = linked->quality != AXIS_QUALITY_UNKNOWN ? axis_quality_name(linked->quality) : NULL;
	params[3] = release;
	params[4] = quality_text;
	params[5] = import;
	params[6] = linked->user;
	/* The file of a grab by force is linked by the person who forced it, and
	 * kept by hand: no automatic search replaces it until a person lets it
	 * go (0062_force_grab.sql); so is a file a person assigned by hand
	 * ("assigned").  Any other import's link is the import's. */
	params[7] = how;
	params[8] = how != NULL ? by : NULL;
	if (!axis_db_conn_exec(conn, "INSERT INTO series_file (file_id, series_id, quality, release_name, quality_text,"
	        " import_id, linked_by, hand_how, hand_by, hand_at, kept) VALUES ($1::bigint, $2::bigint, $3, $4, $5,"
	        " $6::bigint, $7, $8, $9, CASE WHEN $8::text IS NOT NULL THEN now() END, $8::text IS NOT NULL)"
	        " ON CONFLICT (file_id) DO UPDATE SET series_id = EXCLUDED.series_id, quality = EXCLUDED.quality,"
	        " release_name = EXCLUDED.release_name, quality_text = EXCLUDED.quality_text, import_id = EXCLUDED.import_id,"
	        " identity_id = NULL, resolution = NULL, linked_by = EXCLUDED.linked_by, linked_at = now(),"
	        /* What a person did to the file stays (a re-link by an import is
	         * not theirs); only a link by force replaces it. */
	        " hand_how = coalesce(EXCLUDED.hand_how, series_file.hand_how),"
	        " hand_by = CASE WHEN EXCLUDED.hand_how IS NOT NULL THEN EXCLUDED.hand_by ELSE series_file.hand_by END,"
	        " hand_at = coalesce(EXCLUDED.hand_at, series_file.hand_at), kept = EXCLUDED.kept OR series_file.kept,"
	        /* Kept because it was brought over only while the stamp stays the
	         * bringing over's (0073_arr_brought_kept.sql). */
	        " kept_how = CASE WHEN EXCLUDED.hand_how IS NULL THEN series_file.kept_how END", 9,
	        params, NULL, error))
		return FALSE;
	params[1] = array->str;
	/* An import by Axis of a file at a path a person unlinked by hand is a new
	 * file: that decision does not hold for it. */
	if (!axis_db_conn_exec(conn, "DELETE FROM library_hand_unlink WHERE file_id = $1::bigint", 1, params, NULL, error))
		return FALSE;
	/* Every episode it holds (a double episode's file is both's). */
	if (!axis_db_conn_exec(conn, "DELETE FROM series_file_episode WHERE file_id = $1::bigint", 1, params, NULL,
		    error) ||
	    !axis_db_conn_exec(conn, "INSERT INTO series_file_episode (file_id, episode_id) SELECT $1::bigint, e.id"
		    " FROM series_episode e WHERE e.id = ANY ($2::bigint[])", 2, params, NULL, error))
		return FALSE;
	/* Placed through an absolute number: the episode's history says so, with
	 * both numbers and the day (add-anime-support, A2). */
	if (linked->unit->note != NULL && g_str_has_prefix(linked->unit->note, AXIS_TV_ABSOLUTE_NOTE)) {
		g_autofree gchar *text = g_strdup_printf("The file %s was placed on %s. %s", linked->release_name != NULL ?
			linked->release_name : "of this import", linked->unit->label != NULL ? linked->unit->label : "its episode",
			linked->unit->note);

		return axis_tv_store_event(conn, target->owner_id, target->title_id, target->label, g_array_index(ids, gint64,
			0), "changed", text, linked->user, 0, target->grab_id, error);
	}
	return TRUE;
}

static gboolean
note_event(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *kind,
	const gchar            *text,
	const gchar            *user,
	GError                **error
){
	return axis_tv_store_event(conn, target->owner_id, target->title_id, target->label, 0, kind, text, user, 0,
		target->grab_id, error);
}

static gboolean
dismissed(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *user,
	GError                **error
){
	g_autofree gchar *grab = target->grab_id > 0 ? axis_tv_int_text(target->grab_id) : NULL;
	const gchar *params[1];

	if (grab == NULL)
		return TRUE;
	params[0] = grab;
	return axis_db_conn_exec(conn, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed',"
		" close_text = 'Its import was dismissed by a person; the download was left with its client.'"
		" WHERE id = $1::bigint AND closed_at IS NULL", 1, params, NULL, error);
}

/* "Delete this download" on the import's page: the grab closed as deleted
 * (its episodes are free again; those a partial import placed keep their
 * files and links), the series' history with who, the person's block when
 * ticked. */
static gboolean
download_deleted(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *user,
	gboolean                block,
	GError                **error
){
	AxisTv *self = data;

	return axis_acquire_grab_deleted(self->acquire, conn, target->owner_id, target->grab_id, target->grab_release, user,
		block, error);
}

/* Every file it was to import is in the library: the grab is done with
 * (its episodes are free again for upgrades).  Partly imported, it stays
 * open: what failed is still that grab's. */
static gboolean
finished(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	gboolean                complete,
	GError                **error
){
	const ImportData *d = target->owner_data;
	g_autoptr(AxisDbResult) holes = NULL;
	g_autofree gchar *grab = target->grab_id > 0 ? axis_tv_int_text(target->grab_id) : NULL;
	g_autofree gchar *text = NULL;
	const gchar *left;
	const gchar *params[2];

	if (grab == NULL || !complete)
		return TRUE;
	params[0] = grab;
	/* A download grabbed by absolute numbers that had no file for one of the
	 * episodes it was grabbed for (every file it had is placed: a file that
	 * could have been the missing one stops the import before this): the
	 * grab's record and the series' history say which, and no search grabs
	 * the same release again for it (load() in axis-tv-search.c reads this
	 * record by how it begins). */
	if (d != NULL && d->grab_absolute) {
		if (!axis_db_conn_exec(conn, "SELECT string_agg(x.label, ', ' ORDER BY x.season_number, x.number) FROM (SELECT"
		        " e.season_number, e.number, 'S' || lpad(e.season_number::text, 2, '0') || 'E' ||"
		        " lpad(e.number::text, 2, '0') AS label FROM series_grab_episode ge JOIN series_episode e"
		        " ON e.id = ge.episode_id WHERE ge.grab_id = $1::bigint AND e.gone_at IS NULL AND NOT EXISTS (SELECT 1"
		        " FROM series_file_episode sfe WHERE sfe.episode_id = e.id)) x", 1, params, &holes, error))
			return FALSE;
		left = axis_db_result_get_n_rows(holes) > 0 ? axis_db_result_get_text(holes, 0, 0) : NULL;
		if (left != NULL && left[0] != '\0') {
			g_autofree gchar *event = g_strdup_printf("The download of %s was imported, and %s, which it was grabbed "
				"for, got no file from it: none of its files is named for that. No search grabs this release again for "
				"it; another release is searched for.", target->grab_release != NULL ? target->grab_release :
				"a release", left);

			text = g_strdup_printf(AXIS_TV_GRAB_LEFT_HOLES " %s", left);
			if (!axis_tv_store_event(conn, target->owner_id, target->title_id, target->label, 0, "changed", event, NULL,
			        0, target->grab_id, error))
				return FALSE;
		}
	}
	params[1] = text != NULL ? text : "Imported into the library.";
	return axis_db_conn_exec(conn, "UPDATE series_grab SET closed_at = now(), close_reason = 'imported',"
		" close_text = left($2, 2000), completed_at = coalesce(completed_at, now())"
		" WHERE id = $1::bigint AND closed_at IS NULL", 2, params, NULL, error);
}

/* No file of the download can be taken: a grab a person chose by hand lets
 * go of its episodes, so they are wanted again, and the series' history
 * says why.  The import and the download stay, for a person. */
static gboolean
stuck(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *code,
	const gchar            *text,
	GError                **error
){
	g_autoptr(AxisDbResult) closed = NULL;
	g_autofree gchar *grab = target->grab_id > 0 ? axis_tv_int_text(target->grab_id) : NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *event = NULL;
	const gchar *params[2];

	(void)code;
	if (grab == NULL)
		return TRUE;
	why = g_strdup_printf("Grabbed by hand, but its import could take no file of the download: %s", text);
	params[0] = grab;
	params[1] = why;
	if (!axis_db_conn_exec(conn, "UPDATE series_grab SET closed_at = now(), close_reason = 'not_imported',"
	        " close_text = left($2, 2000) WHERE id = $1::bigint AND closed_at IS NULL AND set_aside = 'hand'"
	        " RETURNING (SELECT string_agg(x.label, ', ' ORDER BY x.season_number, x.number) FROM (SELECT"
	        " e.season_number, e.number, 'S' || lpad(e.season_number::text, 2, '0') || 'E' ||"
	        " lpad(e.number::text, 2, '0') AS label FROM series_grab_episode ge JOIN series_episode e"
	        " ON e.id = ge.episode_id WHERE ge.grab_id = $1::bigint) x)", 2, params, &closed, error))
		return FALSE;
	if (axis_db_result_get_n_rows(closed) == 0)
		return TRUE;
	event = g_strdup_printf("The grab by hand of %s is let go: its import could take no file of the download. %s %s "
		"may be searched for again; the download is left with its client.", target->grab_release != NULL ?
		target->grab_release : "a release", text, axis_db_result_get_text(closed, 0, 0) != NULL ?
		axis_db_result_get_text(closed, 0, 0) : "Its episodes");
	return axis_tv_store_event(conn, target->owner_id, target->title_id, target->label, 0, "abandoned", event, NULL, 0,
		target->grab_id, error);
}

static gboolean
list_series(
	gpointer     data,
	AxisDbConn  *conn,
	GPtrArray  **choices,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	*choices = NULL;
	if (!axis_db_conn_exec(conn, "SELECT s.id, t.title, t.year FROM series s JOIN meta_title t ON t.id = s.title_id"
	        " ORDER BY lower(t.title), t.year, s.id LIMIT 5000", 0, NULL, &rows, error))
		return FALSE;
	*choices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_choice_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportChoice *choice = g_new0(AxisImportChoice, 1);

		choice->owner_id = row_int(rows, i, 0, 0);
		choice->label = axis_tv_series_label(axis_db_result_get_text(rows, i, 1), (gint)row_int(rows, i, 2, 0));
		g_ptr_array_add(*choices, choice);
	}
	return TRUE;
}

/* ── Duplicates (add-force-grab-and-assign, slice D) ────────────── */

#define EPISODE_CODE(e) "'S' || lpad(" e ".season_number::text, 2, '0') || 'E' || lpad(" e ".number::text, 2, '0')"

/* The episodes (never one the numbering lacks) that hold more than one
 * linked video file that is present, each with its files.  A file that
 * holds two episodes is one file on each: it is listed only where another
 * file is too, and is held (never offered for removal) while it is the only
 * file of another episode. */
static gboolean
dup_list(
	gpointer     data,
	AxisDbConn  *conn,
	gint64       owner_id,
	GPtrArray   *groups,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *series = axis_tv_int_text(owner_id);
	AxisImportDupGroup *group = NULL;
	const gchar *params[1];
	guint i;

	params[0] = series;
	if (!axis_db_conn_exec(conn, "SELECT s.id, s.title_id, t.title, t.year, qp.format, qp.definition, e.id, "
	        EPISODE_CODE("e") ", lf.id, lf.root_id, lf.volume_id, r.rel_path, lf.rel_path, lf.inode, lf.size, sf.quality,"
	        " sf.release_name, sf.resolution, sf.kept, sf.hand_how, sf.hand_by, sf.import_id IS NOT NULL, lf.pinned_links,"
	        " lf.pinned_downloads,"
	        " (SELECT string_agg(" EPISODE_CODE("o") ", ', ' ORDER BY o.season_number, o.number)"
	        "  FROM series_file_episode x JOIN series_episode o ON o.id = x.episode_id"
	        "  WHERE x.file_id = sf.file_id AND x.episode_id <> e.id),"
	        " (SELECT string_agg(" EPISODE_CODE("o") ", ', ' ORDER BY o.season_number, o.number)"
	        "  FROM series_file_episode x JOIN series_episode o ON o.id = x.episode_id"
	        "  WHERE x.file_id = sf.file_id AND x.episode_id <> e.id AND NOT EXISTS (SELECT 1 FROM series_file_episode y"
	        "   JOIN library_file yl ON yl.id = y.file_id JOIN volume yv ON yv.id = yl.volume_id"
	        "   WHERE y.episode_id = x.episode_id AND y.file_id <> sf.file_id"
	        "   AND yl.state = 'present' AND yl.kind = 'video' AND yv.state IN ('online', 'read_only'))),"
	        " mi.status, mi.duration_ms, (mi.inode = lf.inode AND mi.file_size = lf.size),"
	        " coalesce(sf.kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "' AND sf.hand_how = 'brought', false)"
	        " FROM series_file_episode sfe JOIN series_file sf ON sf.file_id = sfe.file_id"
	        " JOIN library_file lf ON lf.id = sf.file_id JOIN library_root r ON r.id = lf.root_id"
	        " JOIN series_episode e ON e.id = sfe.episode_id JOIN series s ON s.id = sf.series_id"
	        " JOIN meta_title t ON t.id = s.title_id JOIN quality_profile qp ON qp.id = s.quality_profile_id"
	        " LEFT JOIN media_info mi ON mi.file_id = lf.id"
	        " WHERE lf.state = 'present' AND lf.kind = 'video' AND e.gone_at IS NULL AND e.series_id = s.id"
	        " AND ($1::bigint = 0 OR s.id = $1::bigint)"
	        " AND (SELECT count(*) FROM series_file_episode c JOIN library_file cl ON cl.id = c.file_id"
	        "  WHERE c.episode_id = e.id AND cl.state = 'present' AND cl.kind = 'video') > 1"
	        " ORDER BY lower(t.title), s.id, e.season_number, e.number, e.id, lf.id LIMIT 5000", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportDupFile *f = axis_import_dup_file_new();
		const gchar *quality = axis_db_result_get_text(rows, i, 15);
		const gchar *also = axis_db_result_get_text(rows, i, 24);
		const gchar *sole = axis_db_result_get_text(rows, i, 25);
		gint64 episode = row_int(rows, i, 6, 0);
		gboolean current = FALSE;

		if (group == NULL || group->item_id != episode) {
			g_autofree gchar *unusable = NULL;

			group = axis_import_dup_group_new();
			group->owner_id = row_int(rows, i, 0, 0);
			group->item_id = episode;
			group->title_id = row_int(rows, i, 1, 0);
			group->title_label = axis_tv_series_label(axis_db_result_get_text(rows, i, 2), (gint)row_int(rows, i, 3, 0));
			group->label = g_strdup_printf("%s %s", group->title_label, axis_db_result_get_text(rows, i, 7));
			group->href = g_strdup_printf("/series/%" G_GINT64_FORMAT, group->owner_id);
			group->profile_ok = axis_quality_stored_check((gint)row_int(rows, i, 4, 0), axis_db_result_get_text(rows, i, 5),
				&group->profile, &unusable);
			g_ptr_array_add(groups, group);
		}
		f->file->library_file_id = row_int(rows, i, 8, 0);
		f->file->root_id = row_int(rows, i, 9, 0);
		f->file->volume_id = row_int(rows, i, 10, 0);
		f->file->root_path = row_text(rows, i, 11);
		f->file->rel_path = row_text(rows, i, 12);
		f->file->inode = row_int(rows, i, 13, 0);
		f->file->size = row_int(rows, i, 14, 0);
		if (quality == NULL || !axis_quality_from_name(quality, &f->file->quality))
			f->file->quality = AXIS_QUALITY_UNKNOWN;
		f->file->release_name = row_text(rows, i, 16);
		f->file->resolution = AXIS_RELEASE_RESOLUTION_NONE;
		if (f->file->quality == AXIS_QUALITY_UNKNOWN && axis_db_result_get_text(rows, i, 17) != NULL) {
			AxisReleaseResolution r;

			for (r = AXIS_RELEASE_RESOLUTION_480P; r <= AXIS_RELEASE_RESOLUTION_2160P; r++) {
				if (g_strcmp0(axis_release_resolution_name(r), axis_db_result_get_text(rows, i, 17)) == 0)
					f->file->resolution = r;
			}
		}
		axis_db_result_get_boolean(rows, i, 18, &f->file->kept);
		{
			gboolean brought = FALSE;

			axis_db_result_get_boolean(rows, i, 29, &brought);
			f->file->kept_from = f->file->kept && brought ? "Sonarr" : NULL;
		}
		f->hand_how = row_text(rows, i, 19);
		f->hand_by = row_text(rows, i, 20);
		axis_db_result_get_boolean(rows, i, 21, &f->by_import);
		axis_db_result_get_boolean(rows, i, 22, &f->pinned_links);
		axis_db_result_get_boolean(rows, i, 23, &f->pinned_downloads);
		axis_db_result_get_boolean(rows, i, 28, &current);
		axis_import_dup_file_set_probe(f, axis_db_result_get_text(rows, i, 26), row_int(rows, i, 27, 0),
			!axis_db_result_is_null(rows, i, 28) && current);
		if (also != NULL)
			f->also = g_strdup_printf("It also holds %s: removing it takes it from there too.", also);
		if (sole != NULL)
			f->held = g_strdup_printf("it is the only file of %s that is on a disk Axis can read now", sole);
		g_ptr_array_add(group->files, f);
	}
	return TRUE;
}

/* The series is locked while an episode's duplicates are judged and removed.
 * Refused while a grab is open for an episode that one of the episode's
 * files holds (its import may replace these very files).  With @remove: the
 * links of the named files and of the files that would stay are locked, a
 * named file that is no longer the episode's refuses the request, and every
 * episode a named file holds is appended to @others with the present video
 * files that would still be linked to it, read under that lock.
 * Whether one of those is a file that can stay (seen on its disk, readable)
 * is the layer's to say, not the index's: library_file.state is what the
 * last scan saw, which may be long ago. */
static gboolean
dup_guard(
	gpointer       data,
	AxisDbConn    *conn,
	gint64         owner_id,
	gint64         item_id,
	const GArray  *remove,
	GPtrArray     *others,
	gchar        **refusal,
	GError       **error
){
	g_autoptr(AxisDbResult) grabs = NULL;
	g_autoptr(AxisDbResult) left = NULL;
	g_autoptr(AxisDbResult) gone = NULL;
	g_autoptr(GString) array = g_string_new("{");
	g_autofree gchar *episode = axis_tv_int_text(item_id);
	g_autofree gchar *series = axis_tv_int_text(owner_id);
	AxisImportDupGroup *other = NULL;
	const gchar *params[1];
	const gchar *more[3];
	gboolean found = FALSE;
	gint64 last = 0;
	guint i;

	*refusal = NULL;
	if (!axis_tv_store_lock(conn, owner_id, &found, error))
		return FALSE;
	params[0] = episode;
	if (!axis_db_conn_exec(conn, "SELECT " EPISODE_CODE("e") ", g.release_name FROM series_grab_episode ge"
	        " JOIN series_grab g ON g.id = ge.grab_id JOIN series_episode e ON e.id = ge.episode_id"
	        " WHERE g.closed_at IS NULL AND (ge.episode_id = $1::bigint OR ge.episode_id IN (SELECT x.episode_id"
	        "  FROM series_file_episode x WHERE x.file_id IN (SELECT file_id FROM series_file_episode"
	        "  WHERE episode_id = $1::bigint))) ORDER BY e.season_number, e.number LIMIT 1", 1, params, &grabs, error))
		return FALSE;
	if (axis_db_result_get_n_rows(grabs) > 0) {
		*refusal = g_strdup_printf("A download for %s is on the way (%s), and its import may replace these very files. "
			"Wait for it, or remove that download on the Downloads page, then look again.",
			axis_db_result_get_text(grabs, 0, 0), axis_db_result_get_text(grabs, 0, 1) != NULL ?
			axis_db_result_get_text(grabs, 0, 1) : "a release");
		return TRUE;
	}
	if (remove == NULL || remove->len == 0 || others == NULL)
		return TRUE;
	for (i = 0; i < remove->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(remove, gint64, i));
	g_string_append_c(array, '}');
	params[0] = array->str;
	/* First the links of the named files, and of every file of an episode
	 * one of them holds, are locked (in one order, so that two removals wait
	 * for each other and never on each other): none of them is linked
	 * elsewhere, or unlinked, between the look below and the removal.  (The
	 * retirement deletes the named files' links on this connection.) */
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM series_file WHERE file_id = ANY ($1::bigint[])"
	        " OR file_id IN (SELECT y.file_id FROM series_file_episode y WHERE y.episode_id IN (SELECT x.episode_id"
	        "  FROM series_file_episode x WHERE x.file_id = ANY ($1::bigint[]))) ORDER BY file_id FOR UPDATE", 1, params,
	        NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT 1 FROM series_file_episode WHERE file_id = ANY ($1::bigint[])"
	        " OR episode_id IN (SELECT x.episode_id FROM series_file_episode x WHERE x.file_id = ANY ($1::bigint[]))"
	        " ORDER BY file_id, episode_id FOR UPDATE", 1, params, NULL, error))
		return FALSE;
	/* A named file that is no longer a file of this episode of this series,
	 * read under the lock, may be another title's or another episode's only
	 * file by now: nothing is removed. */
	more[0] = array->str;
	more[1] = series;
	more[2] = episode;
	if (!axis_db_conn_exec(conn, "SELECT u.id FROM unnest($1::bigint[]) u(id) WHERE NOT EXISTS (SELECT 1"
	        " FROM series_file sf JOIN series_file_episode x ON x.file_id = sf.file_id WHERE sf.file_id = u.id"
	        " AND sf.series_id = $2::bigint AND x.episode_id = $3::bigint) LIMIT 1", 3, more, &gone, error))
		return FALSE;
	if (axis_db_result_get_n_rows(gone) > 0) {
		*refusal = g_strdup("A ticked file is no longer linked to this episode: its link changed while the request "
			"was checked, and it may be another episode's or another series' file now. Nothing was removed: look "
			"again.");
		return TRUE;
	}
	/* Then every episode a named file holds (the group's own among them),
	 * each with the files that would still be linked to it (a row without a
	 * file: it would have none). */
	if (!axis_db_conn_exec(conn, "SELECT o.id, " EPISODE_CODE("o") ", k.id, k.root_id, k.volume_id, k.root_path,"
	        " k.rel_path, k.inode, k.size, k.status, k.duration_ms, k.current"
	        " FROM (SELECT DISTINCT x.episode_id FROM series_file_episode x WHERE x.file_id = ANY ($1::bigint[])) h"
	        " JOIN series_episode o ON o.id = h.episode_id"
	        " LEFT JOIN LATERAL (SELECT yl.id, yl.root_id, yl.volume_id, r.rel_path AS root_path, yl.rel_path, yl.inode,"
	        "  yl.size, mi.status, mi.duration_ms, (mi.inode = yl.inode AND mi.file_size = yl.size) AS current"
	        "  FROM series_file_episode y JOIN library_file yl ON yl.id = y.file_id"
	        "  JOIN library_root r ON r.id = yl.root_id LEFT JOIN media_info mi ON mi.file_id = yl.id"
	        "  WHERE y.episode_id = o.id AND NOT (y.file_id = ANY ($1::bigint[])) AND yl.state = 'present'"
	        "  AND yl.kind = 'video') k ON true"
	        " ORDER BY o.season_number, o.number, o.id, k.id LIMIT 5000", 1, params, &left, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(left); i++) {
		gint64 id = row_int(left, i, 0, 0);
		gboolean current = FALSE;
		AxisImportDupFile *f;

		if (other == NULL || id != last) {
			other = axis_import_dup_group_new();
			other->owner_id = owner_id;
			other->item_id = id;
			other->label = row_text(left, i, 1);
			g_ptr_array_add(others, other);
			last = id;
		}
		if (axis_db_result_is_null(left, i, 2))
			continue;
		f = axis_import_dup_file_new();
		f->file->library_file_id = row_int(left, i, 2, 0);
		f->file->root_id = row_int(left, i, 3, 0);
		f->file->volume_id = row_int(left, i, 4, 0);
		f->file->root_path = row_text(left, i, 5);
		f->file->rel_path = row_text(left, i, 6);
		f->file->inode = row_int(left, i, 7, 0);
		f->file->size = row_int(left, i, 8, 0);
		axis_db_result_get_boolean(left, i, 11, &current);
		axis_import_dup_file_set_probe(f, axis_db_result_get_text(left, i, 9), row_int(left, i, 10, 0),
			!axis_db_result_is_null(left, i, 11) && current);
		g_ptr_array_add(other->files, f);
	}
	return TRUE;
}

static const AxisImportOwnerInfo owner_info = {
	AXIS_TV_MODULE_ID,
	"series",
	resolve,
	NULL,
	link_file,
	note_event,
	dismissed,
	list_series,
	match,
	finished,
	stuck,
	assign_read,
	assigned,
	assign_lock,
	assign_claimed,
	dup_list,
	dup_guard,
	download_deleted
};

void
axis_tv_set_import(
	AxisTv     *self,
	AxisImport *import
){
	g_return_if_fail(self != NULL);

	self->import = import;
	if (import != NULL)
		axis_import_add_owner(import, &owner_info, self);
}

gboolean
axis_tv_request_imports(
	AxisTv                   *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	if (self->import == NULL)
		return TRUE;
	if (!axis_db_pool_exec(self->pool, "SELECT g.download_id FROM series_grab g JOIN download d ON d.id = g.download_id"
	        " WHERE g.closed_at IS NULL AND g.completed_at IS NOT NULL AND d.state = 'completed' ORDER BY g.id LIMIT 200",
	        0, NULL, &rows, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_import_requeue_due(self->import, conn, NULL, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		gboolean created = FALSE;

		ok = axis_import_request(self->import, conn, AXIS_TV_MODULE_ID, row_int(rows, i, 0, 0), NULL, &created, error);
		if (ok && created && report != NULL)
			report->imports++;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}
