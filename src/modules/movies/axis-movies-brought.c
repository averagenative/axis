/*
 * axis-movies-brought.c - a movie migrated from another app (Radarr):
 * adding it through the code a person's Add uses, linking the file it has on
 * disk, and the hold on its searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/arr-migration/spec.md.  The bring-over layer
 * (src/arr/) decides which movies and which files; what a movie, a link and
 * a hold are is here.
 *
 *  - Only rows change.  No file or folder is opened, moved, renamed or
 *    deleted: a file is known by its row of Axis's own index (library_file).
 *  - A link made here is neither an import's nor identification's: it has no
 *    import_id and no identity_id, and is stamped hand_how "brought" with
 *    who ran the bringing over and when.  So identification never replaces,
 *    moves or removes it (its rule for a row with hand_at), and, having no
 *    import, it is no evidence that draws the other files of its folder to
 *    the movie.  It is kept (AXIS_MOVIE_BROUGHT_KEPT, with kept_how
 *    "brought"): no automatic search and no Search now replaces it, and an
 *    import refuses to replace it without the tick that names it, until a
 *    person lets it go, for the one file on the movie's page or for all of a
 *    connection's (axis_movies_release_brought()).  Let go, upgrades judge it
 *    as any file, and an upgrade retires it as any file a movie has (resolve()
 *    in axis-movies-import.c lists every linked file, wherever it lies and
 *    whoever named its folder).
 *  - A file that is linked already stays as it is, whoever linked it and to
 *    whatever movie; a file a person unlinked by hand stays unlinked.
 *  - The hold (movie.search_hold) stops every automatic search of the movie
 *    at one gate (load() in axis-movies-search.c).  It ends when a person
 *    starts searching, for the movies of a connection or for this one, or
 *    searches for this movie themselves.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "library/axis-library-name.h"
#include "release/axis-release.h"

gboolean
axis_movie_hold_stops(const gchar *hold)
{
	return hold != NULL && hold[0] != '\0';
}

const gchar *
axis_movie_hold_sentence(const gchar *hold)
{
	if (!axis_movie_hold_stops(hold))
		return NULL;
	if (strcmp(hold, AXIS_MOVIE_HOLD_FILE_UNSEEN) == 0)
		return "Migrated from Radarr, which has a file for it that Axis does not see: Axis does not search for it "
			"until that file is found or you start searching for this movie.";
	if (strcmp(hold, AXIS_MOVIE_HOLD_DOWNLOADING) == 0)
		return "Migrated from Radarr, which was downloading it when searching was started: Axis does not search "
			"for it until you start searching again.";
	return "Migrated from Radarr: Axis does not search for it until you start searching.";
}

const gchar *
axis_movie_hold_short(const gchar *hold)
{
	return axis_movie_hold_stops(hold) ? "Held" : NULL;
}

void
axis_movie_brought_clear(AxisMovieBrought *brought)
{
	if (brought == NULL)
		return;
	g_clear_pointer(&brought->other, g_free);
}

void
axis_movies_search_pace(
	AxisMovies *self,
	guint      *per_run,
	guint      *interval_s
){
	*per_run = (guint)CLAMP(axis_movies_setting_integer(self, AXIS_MOVIES_SETTING_PER_RUN, 5), 1, 100000);
	*interval_s = AXIS_MOVIES_SCHEDULE_INTERVAL_S;
}

/* ── The hold ───────────────────────────────────────────────────── */

static gboolean
set_hold(
	AxisDbConn   *conn,
	gint64        movie_id,
	const gchar  *hold,
	GError      **error
){
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	const gchar *params[2];

	params[0] = movie;
	params[1] = hold;
	return axis_db_conn_exec(conn, "UPDATE movie SET search_hold = $2, updated_at = now() WHERE id = $1::bigint"
		" AND search_hold IS DISTINCT FROM $2", 2, params, NULL, error);
}

gboolean
axis_movies_store_end_hold(
	AxisDbConn            *conn,
	const AxisMovieFacts  *facts,
	const gchar           *user,
	const gchar           *what,
	GError               **error
){
	if (facts == NULL || !axis_movie_hold_stops(facts->search_hold))
		return TRUE;
	return set_hold(conn, facts->id, NULL, error) &&
		axis_movies_store_eventf(conn, facts, "changed", user, 0, 0, error, "It was migrated and held: %s%s%s "
			"ends that, and Axis searches for it by itself from now on.", what, user != NULL ? " by " : "",
			user != NULL ? user : "");
}

gboolean
axis_movies_start_searching(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	gboolean     *was_held,
	GError      **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean found = FALSE;
	gboolean ok;

	if (was_held != NULL)
		*was_held = FALSE;
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_begin(conn, error) && axis_movies_store_lock(conn, movie_id, &found, error);
	if (ok && found)
		ok = axis_movies_store_facts_one(conn, movie_id, &facts, error);
	if (ok && facts == NULL) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	if (ok && axis_movie_hold_stops(facts->search_hold)) {
		if (was_held != NULL)
			*was_held = TRUE;
		/* Nothing is searched at once: the scheduler takes it at its pace. */
		ok = set_hold(conn, movie_id, NULL, error) &&
			axis_movies_store_eventf(conn, facts, "changed", user, 0, 0, error, "Searching for it was started%s%s: it "
				"was migrated and held until now. Axis searches for it at the scheduler's ordinary pace.",
				user != NULL ? " by " : "", user != NULL ? user : "");
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
axis_movies_start_brought(
	AxisMovies    *self,
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
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	g_autofree gchar *text = g_strdup_printf("Searching was started for the movies migrated from %s%s%s. Axis "
		"searches for it at the scheduler's ordinary pace.", origin, by != NULL ? " by " : "", by != NULL ? by : "");
	g_autofree gchar *waits = g_strdup_printf("Searching was started for the movies migrated from %s, and this one "
		"was left out: it is in that app's download queue. Start searching again once it has left the queue.", origin);
	const gchar *params[3];

	*started = 0;
	/* The movies the other app is downloading: held for that, said once. */
	params[0] = held_ids;
	params[1] = waits;
	params[2] = by;
	if (!axis_db_conn_exec(conn,
	        "WITH held AS (UPDATE movie SET search_hold = '" AXIS_MOVIE_HOLD_DOWNLOADING "', updated_at = now()"
	        "   WHERE id = ANY ($1::bigint[]) AND search_hold = '" AXIS_MOVIE_HOLD_BROUGHT "' RETURNING id, title_id)"
	        " INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user)"
	        " SELECT h.id, h.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        " 'changed', $2, $3 FROM held h JOIN meta_title t ON t.id = h.title_id", 3, params, NULL, error))
		return FALSE;
	/* The others: free.  A movie whose file Axis cannot see is not among the
	 * kinds named here, so it stays held whoever lists it. */
	params[0] = start_ids;
	params[1] = text;
	if (!axis_db_conn_exec(conn,
	        "WITH freed AS (UPDATE movie SET search_hold = NULL, updated_at = now()"
	        "   WHERE id = ANY ($1::bigint[]) AND search_hold IN ('" AXIS_MOVIE_HOLD_BROUGHT "', '"
	        AXIS_MOVIE_HOLD_DOWNLOADING "') RETURNING id, title_id),"
	        " said AS (INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user)"
	        "   SELECT f.id, f.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        "   'changed', $2, $3 FROM freed f JOIN meta_title t ON t.id = f.title_id RETURNING 1)"
	        " SELECT (SELECT count(*) FROM freed), (SELECT count(*) FROM said)", 3, params, &freed, error))
		return FALSE;
	{
		gint64 n = 0;

		axis_db_result_get_int64(freed, 0, 0, &n);
		*started = (guint)MAX(n, 0);
	}
	return TRUE;
}

gboolean
axis_movies_release_brought(
	AxisMovies    *self,
	AxisDbConn    *conn,
	const GArray  *movies,
	const gchar   *origin,
	const gchar   *user,
	guint         *released,
	GError       **error
){
	g_autoptr(AxisDbResult) freed = NULL;
	g_autofree gchar *ids = id_array(movies);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	g_autofree gchar *text = g_strdup_printf("The files migrated from %s were let go%s%s: Axis may replace this "
		"movie's file again. A file below the cutoff of its quality profile is replaced at the scheduler's ordinary "
		"pace, and the old one goes to the recycle folder.", origin, by != NULL ? " by " : "", by != NULL ? by : "");
	const gchar *params[3];
	gint64 n = 0;

	*released = 0;
	params[0] = ids;
	params[1] = text;
	params[2] = by;
	/* Only what the bringing over keeps: a link a person kept or let go on
	 * the movie's page has no kept_how, and one linked by hand since has
	 * another hand_how. */
	if (!axis_db_conn_exec(conn,
	        "WITH freed AS (UPDATE movie_file SET kept = false, kept_how = NULL"
	        "   WHERE movie_id = ANY ($1::bigint[]) AND hand_how = 'brought' AND kept"
	        "   AND kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' RETURNING movie_id),"
	        " said AS (INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user)"
	        "   SELECT m.id, m.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
	        "   'hand_link', $2, $3 FROM (SELECT DISTINCT movie_id FROM freed) f JOIN movie m ON m.id = f.movie_id"
	        "   JOIN meta_title t ON t.id = m.title_id RETURNING 1)"
	        " SELECT (SELECT count(*) FROM freed), (SELECT count(*) FROM said)", 3, params, &freed, error))
		return FALSE;
	axis_db_result_get_int64(freed, 0, 0, &n);
	*released = (guint)MAX(n, 0);
	return TRUE;
}

/* ── Linking the file ───────────────────────────────────────────── */

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

/*
 * Links @file_id to @movie_id as migrated, when it is linked to nothing
 * and no person unlinked it.  The file's row of the index is locked first,
 * so that an import, identification or the links page deciding about the
 * same file at this moment is seen, or waits.
 */
static gboolean
link_file(
	AxisDbConn            *conn,
	gint64                 movie_id,
	gint64                 file_id,
	const gchar           *quality,
	const gchar           *release,
	const gchar           *user,
	AxisMovieBroughtFile  *out,
	gchar                **other,
	gchar                **path,
	gboolean              *was_identified,
	GError               **error
){
	g_autoptr(AxisDbResult) file = NULL;
	g_autoptr(AxisDbResult) made = NULL;
	g_autofree gchar *file_text = axis_movies_int_text(file_id);
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	g_autofree gchar *from = axis_movies_text_cut(release, 300);
	g_autofree gchar *read_quality = NULL;
	g_autofree gchar *read_resolution = NULL;
	const gchar *params[8];
	gint64 linked_to = 0;
	gboolean unlinked = FALSE;
	gboolean identified = FALSE;

	*out = AXIS_MOVIE_BROUGHT_GONE;
	*other = NULL;
	if (was_identified != NULL)
		*was_identified = FALSE;
	if (path != NULL)
		*path = NULL;
	params[0] = file_text;
	if (!axis_db_conn_exec(conn, "SELECT lf.state, lf.kind, lf.base_name, lf.rel_path, coalesce(mf.movie_id, 0),"
	        " t.title, t.year, EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = lf.id),"
	        /* Identification's own link: no person's and no import's, not kept. */
	        " coalesce(mf.hand_at IS NULL AND mf.import_id IS NULL AND NOT mf.kept, false)"
	        " FROM library_file lf LEFT JOIN movie_file mf ON mf.file_id = lf.id"
	        " LEFT JOIN movie m ON m.id = mf.movie_id LEFT JOIN meta_title t ON t.id = m.title_id"
	        " WHERE lf.id = $1::bigint FOR UPDATE OF lf", 1, params, &file, error))
		return FALSE;
	if (axis_db_result_get_n_rows(file) == 0 || g_strcmp0(axis_db_result_get_text(file, 0, 0), "present") != 0 ||
	    g_strcmp0(axis_db_result_get_text(file, 0, 1), "video") != 0)
		return TRUE;
	if (path != NULL)
		*path = axis_library_name_display(axis_db_result_get_text(file, 0, 3));
	axis_db_result_get_int64(file, 0, 4, &linked_to);
	axis_db_result_get_boolean(file, 0, 7, &unlinked);
	axis_db_result_get_boolean(file, 0, 8, &identified);
	if (linked_to == movie_id && identified) {
		/* Identification linked it to this very movie, a moment ago (Add links
		 * what identification holds for the title) or before.  It is the
		 * app's file all the same, and a brought file is kept: left as
		 * identification's (not kept, no hand stamp) it would be upgraded by
		 * Axis itself after "Start searching", on a server whose library was
		 * identified before the run most files.  So it is stamped as any
		 * brought link, and counted as linked. */
		params[1] = movie;
		params[2] = quality;
		params[3] = from;
		params[4] = by;
		params[5] = AXIS_MOVIE_BROUGHT_KEPT ? "true" : "false";
		if (!axis_db_conn_exec(conn, "UPDATE movie_file SET hand_how = 'brought', hand_by = $5, hand_at = now(),"
		        " identity_id = NULL, quality = coalesce($3, quality), release_name = coalesce($4, release_name),"
		        " quality_text = CASE WHEN $3 IS NOT NULL THEN 'Its quality is the one Radarr recorded for it.'"
		        "   ELSE quality_text END, kept = $6::boolean,"
		        " kept_how = CASE WHEN $6::boolean THEN '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' END"
		        " WHERE file_id = $1::bigint AND movie_id = $2::bigint", 6, params, NULL, error))
			return FALSE;
		if (was_identified != NULL)
			*was_identified = TRUE;
		*out = AXIS_MOVIE_BROUGHT_LINKED;
		return TRUE;
	}
	if (linked_to == movie_id) {
		/* An import's or a person's: it stays as it is, and is not stamped
		 * again. */
		*out = AXIS_MOVIE_BROUGHT_ALREADY;
		return TRUE;
	}
	if (linked_to > 0) {
		gint64 year = 0;

		axis_db_result_get_int64(file, 0, 6, &year);
		*out = AXIS_MOVIE_BROUGHT_ELSEWHERE;
		*other = axis_movies_title_label(axis_db_result_get_text(file, 0, 5), (gint)year);
		return TRUE;
	}
	if (unlinked) {
		*out = AXIS_MOVIE_BROUGHT_UNLINKED;
		return TRUE;
	}
	if (quality == NULL)
		quality_of_name(axis_db_result_get_text(file, 0, 2), &read_quality, &read_resolution);
	params[1] = movie;
	params[2] = quality != NULL ? quality : read_quality;
	params[3] = read_resolution;
	params[4] = from;
	params[5] = quality != NULL ? "Its quality is the one Radarr recorded for it." : read_quality != NULL ||
		read_resolution != NULL ? "Its quality was read from its own name: Radarr recorded none Axis knows." : NULL;
	params[6] = by;
	params[7] = AXIS_MOVIE_BROUGHT_KEPT ? "true" : "false";
	/* No import and no identity; ON CONFLICT: a link made since the row was
	 * read (it was locked, so by nothing that takes that lock) stands. */
	if (!axis_db_conn_exec(conn, "INSERT INTO movie_file (file_id, movie_id, quality, resolution, release_name,"
	        " quality_text, linked_by, hand_how, hand_by, hand_at, kept, kept_how) VALUES ($1::bigint, $2::bigint, $3, $4,"
	        " $5, $6, $7, 'brought', $7, now(), $8::boolean, CASE WHEN $8::boolean THEN '" AXIS_MOVIE_KEPT_HOW_BROUGHT "'"
	        " END) ON CONFLICT (file_id) DO NOTHING RETURNING file_id", 8, params, &made, error))
		return FALSE;
	*out = axis_db_result_get_n_rows(made) > 0 ? AXIS_MOVIE_BROUGHT_LINKED : AXIS_MOVIE_BROUGHT_ELSEWHERE;
	return TRUE;
}

/* Whether the movie has a file of the library that is present. */
static gboolean
has_present_file(
	AxisDbConn  *conn,
	gint64       movie_id,
	gboolean    *has,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	const gchar *params[1];

	*has = FALSE;
	params[0] = movie;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
	        " WHERE mf.movie_id = $1::bigint AND lf.state = 'present' LIMIT 1", 1, params, &rows, error))
		return FALSE;
	*has = axis_db_result_get_n_rows(rows) > 0;
	return TRUE;
}

gboolean
axis_movies_bring(
	AxisMovies            *self,
	AxisDbConn            *conn,
	const AxisMovieBring  *bring,
	AxisMovieBrought      *brought,
	GError               **error
){
	AxisMovieSpec spec = bring->spec;
	gboolean has = FALSE;

	memset(brought, 0, sizeof(*brought));
	g_return_val_if_fail(spec.brought_from != NULL, FALSE);

	/* Never searched by the run, whatever the caller left in the spec. */
	spec.search_now = FALSE;
	if (!axis_movies_add_in_transaction(self, conn, &spec, &brought->movie_id, error))
		return FALSE;
	if (bring->file_id > 0) {
		if (!link_file(conn, brought->movie_id, bring->file_id, bring->quality, bring->release, spec.user,
		        &brought->file, &brought->other, NULL, NULL, error))
			return FALSE;
	} else {
		brought->file = AXIS_MOVIE_BROUGHT_NO_FILE;
	}
	if (!has_present_file(conn, brought->movie_id, &has, error))
		return FALSE;
	/* The other app has a file and Axis sees none for the movie: held apart,
	 * or Axis would count it missing and fetch a second copy. */
	brought->hold = bring->has_file && !has ? AXIS_MOVIE_HOLD_FILE_UNSEEN : AXIS_MOVIE_HOLD_BROUGHT;
	return set_hold(conn, brought->movie_id, brought->hold, error);
}

gboolean
axis_movies_bring_file(
	AxisMovies            *self,
	AxisDbConn            *conn,
	gint64                 movie_id,
	gint64                 file_id,
	const gchar           *quality,
	const gchar           *release,
	const gchar           *origin,
	const gchar           *user,
	const gchar           *hold_after,
	AxisMovieBrought      *brought,
	GError               **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autofree gchar *path = NULL;
	gboolean found = FALSE;
	gboolean has = FALSE;
	gboolean was_identified = FALSE;

	memset(brought, 0, sizeof(*brought));
	brought->movie_id = movie_id;
	brought->file = AXIS_MOVIE_BROUGHT_GONE;
	if (!axis_movies_store_lock(conn, movie_id, &found, error))
		return FALSE;
	if (!found || !axis_movies_store_facts_one(conn, movie_id, &facts, error) || facts == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	brought->hold = facts->search_hold != NULL ? g_intern_string(facts->search_hold) : NULL;
	if (!link_file(conn, movie_id, file_id, quality, release, user, &brought->file, &brought->other, &path,
	        &was_identified, error))
		return FALSE;
	if (brought->file != AXIS_MOVIE_BROUGHT_LINKED && brought->file != AXIS_MOVIE_BROUGHT_ALREADY)
		return TRUE;
	/* The file it was held for is there now: only that kind of hold goes,
	 * and only to what the caller says the others of its connection have. */
	if (g_strcmp0(facts->search_hold, AXIS_MOVIE_HOLD_FILE_UNSEEN) == 0) {
		if (!has_present_file(conn, movie_id, &has, error))
			return FALSE;
		if (has) {
			if (!set_hold(conn, movie_id, hold_after, error))
				return FALSE;
			brought->hold = hold_after != NULL ? g_intern_string(hold_after) : NULL;
		}
	}
	if (brought->file != AXIS_MOVIE_BROUGHT_LINKED)
		return TRUE;
	return axis_movies_store_eventf(conn, facts, "hand_link", user, 0, 0, error, "Migrated from %s%s%s: its file "
		"%s, which %s, is linked as migrated, and kept. The file on disk was not touched.", origin,
		user != NULL ? " by " : "", user != NULL ? user : "", path != NULL ? path : "on disk",
		was_identified ? "identification had linked since" : "Axis did not see before");
}
