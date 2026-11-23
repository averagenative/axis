/*
 * axis-movies-import.c - the films' side of the import pipeline: whose a
 * download is (by its grab), what the film is called and how long it runs,
 * whether a file's name is the film's, linking the file to the film, and
 * the film's history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The pipeline's core (src/import/) knows nothing of films; this is what
 * the movies module answers for its titles.  Whose download it is comes
 * from the grab that sent it, never from the download's name.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "import/axis-import.h"
#include "library/axis-library-hand.h"

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

/* The film's title as the matcher reads it, from the store (no provider is
 * asked). */
static AxisMetaTitle *
stored_title(
	AxisDbConn   *conn,
	gint64        title_id,
	AxisDbResult *film,
	GError      **error
){
	g_autoptr(AxisDbResult) alts = NULL;
	g_autofree gchar *id = axis_movies_int_text(title_id);
	AxisMetaTitle *title = axis_meta_title_new(AXIS_META_KIND_FILM);
	const gchar *params[1];
	guint i;

	title->id = title_id;
	title->title = row_text(film, 0, 2);
	title->original_title = row_text(film, 0, 3);
	title->year = (gint)row_int(film, 0, 4, 0);
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT title FROM meta_alt_title WHERE title_id = $1::bigint ORDER BY title LIMIT 100",
	        1, params, &alts, error)) {
		axis_meta_title_free(title);
		return NULL;
	}
	if (axis_db_result_get_n_rows(alts) > 0)
		title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	for (i = 0; i < axis_db_result_get_n_rows(alts); i++) {
		AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

		alt->title = row_text(alts, i, 0);
		g_ptr_array_add(title->alt_titles, alt);
	}
	return title;
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
	AxisMovies *self = data;
	g_autoptr(AxisDbResult) grab = NULL;
	g_autoptr(AxisDbResult) film = NULL;
	g_autoptr(AxisDbResult) files = NULL;
	g_autoptr(AxisImportTarget) target = NULL;
	g_autofree gchar *download = axis_movies_int_text(download_id);
	g_autofree gchar *movie_text = NULL;
	g_autofree gchar *unusable = NULL;
	const gchar *params[2];
	gint64 movie_id = owner_id;
	gint64 grab_id = 0;
	gchar *grab_release = NULL;
	g_autofree gchar *ticked = NULL;
	g_autofree gchar *grab_by = NULL;
	gboolean set_aside = FALSE;
	gboolean bound = FALSE;
	guint i;

	*target_out = NULL;
	*problem = NULL;
	/* Whose it is: the grab that sent it, and whether a person set the
	 * profile aside for it; the files they ticked replacing, those still
	 * at the quality the confirmation showed. */
	if (download_id > 0) {
		params[0] = download;
		if (!axis_db_conn_exec(conn, "SELECT g.id, g.movie_id, g.release_name, g.set_aside IS NOT NULL,"
		        " (SELECT string_agg(x->>'f', ',') FROM jsonb_array_elements(g.replaces_files) x JOIN movie_file mf"
		        "  ON mf.file_id = (x->>'f')::bigint AND mf.movie_id = g.movie_id WHERE coalesce(x->>'q', '') ="
		        "  coalesce((SELECT coalesce(c.source_quality, '') FROM conversion c WHERE c.file_id = mf.file_id"
		        "  AND c.state = 'done' AND c.outcome = 'replace' ORDER BY c.id DESC LIMIT 1), mf.quality, '')),"
		        " g.bound, g.grabbed_by"
		        " FROM movie_grab g WHERE g.download_id = $1::bigint ORDER BY g.closed_at IS NULL DESC, g.id DESC"
		        " LIMIT 1", 1, params, &grab, error))
			return FALSE;
		if (axis_db_result_get_n_rows(grab) > 0) {
			gint64 grabbed_for = row_int(grab, 0, 1, 0);

			if (owner_id <= 0 || owner_id == grabbed_for) {
				movie_id = grabbed_for;
				grab_id = row_int(grab, 0, 0, 0);
				grab_release = row_text(grab, 0, 2);
				axis_db_result_get_boolean(grab, 0, 3, &set_aside);
				ticked = row_text(grab, 0, 4);
				/* By force: bound to this movie (the name is not judged
				 * again), for the person who made it. */
				axis_db_result_get_boolean(grab, 0, 5, &bound);
				grab_by = row_text(grab, 0, 6);
			}
		}
	}
	if (movie_id <= 0) {
		*problem = g_strdup("This download was not grabbed for a movie Axis manages: import it by hand, choosing the "
			"movie.");
		return TRUE;
	}
	movie_text = axis_movies_int_text(movie_id);
	params[0] = movie_text;
	if (!axis_db_conn_exec(conn, "SELECT m.id, m.title_id, t.title, t.original_title, t.year, t.runtime_minutes,"
	        " m.root_id, qp.format, qp.definition, t.provider, t.provider_id,"
	        " (SELECT e.value FROM meta_external_id e WHERE e.title_id = t.id AND e.scheme = 'tmdb' LIMIT 1),"
	        " (SELECT e.value FROM meta_external_id e WHERE e.title_id = t.id AND e.scheme = 'imdb' LIMIT 1)"
	        " FROM movie m JOIN meta_title t ON t.id = m.title_id JOIN quality_profile qp ON qp.id = m.quality_profile_id"
	        " WHERE m.id = $1::bigint", 1, params, &film, error)) {
		g_free(grab_release);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(film) == 0) {
		g_free(grab_release);
		*problem = g_strdup("The movie this download was for is not managed any more.");
		return TRUE;
	}
	target = axis_import_target_new();
	target->owner_id = movie_id;
	target->grab_id = grab_id;
	target->grab_set_aside = set_aside;
	target->grab_ticked = axis_import_file_ids(set_aside ? ticked : NULL);
	target->grab_bound = set_aside && bound;
	target->grab_by = g_steal_pointer(&grab_by);
	target->grab_release = grab_release;
	target->title_id = row_int(film, 0, 1, 0);
	target->title = row_text(film, 0, 2);
	target->original_title = row_text(film, 0, 3);
	target->year = (gint)row_int(film, 0, 4, 0);
	target->runtime_minutes = (gint)row_int(film, 0, 5, 0);
	target->root_id = row_int(film, 0, 6, 0);
	target->label = axis_movies_title_label(target->title, target->year);
	target->profile_ok = axis_quality_stored_check((gint)row_int(film, 0, 7, 0), axis_db_result_get_text(film, 0, 8),
		&target->profile, &unusable);
	target->tmdb_id = row_text(film, 0, 11);
	if (target->tmdb_id == NULL && g_strcmp0(axis_db_result_get_text(film, 0, 9), "tmdb") == 0)
		target->tmdb_id = row_text(film, 0, 10);
	target->imdb_id = row_text(film, 0, 12);
	target->languages = axis_movies_setting_languages(self);
	target->owner_data = stored_title(conn, target->title_id, film, error);
	target->owner_free = (GDestroyNotify)axis_meta_title_free;
	if (target->owner_data == NULL)
		return FALSE;

	/* Its files in the library now: what an import replaces. */
	if (!axis_db_conn_exec(conn, "SELECT lf.id, lf.root_id, lf.volume_id, r.rel_path, lf.rel_path, lf.inode, lf.size,"
	        " mf.quality, mf.release_name, mf.resolution, mf.kept,"
	        " coalesce(mf.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' AND mf.hand_how = 'brought', false)"
	        " FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
	        " JOIN library_root r ON r.id = lf.root_id WHERE mf.movie_id = $1::bigint AND lf.state = 'present'"
	        " AND lf.kind = 'video' ORDER BY lf.id", 1, params, &files, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(files); i++) {
		AxisImportExisting *existing = g_new0(AxisImportExisting, 1);
		const gchar *quality = axis_db_result_get_text(files, i, 7);

		existing->library_file_id = row_int(files, i, 0, 0);
		existing->root_id = row_int(files, i, 1, 0);
		existing->volume_id = row_int(files, i, 2, 0);
		existing->root_path = row_text(files, i, 3);
		existing->rel_path = row_text(files, i, 4);
		existing->inode = row_int(files, i, 5, 0);
		existing->size = row_int(files, i, 6, 0);
		if (quality == NULL || !axis_quality_from_name(quality, &existing->quality))
			existing->quality = AXIS_QUALITY_UNKNOWN;
		existing->release_name = row_text(files, i, 8);
		existing->resolution = existing->quality == AXIS_QUALITY_UNKNOWN ?
			axis_identify_resolution_from_name(axis_db_result_get_text(files, i, 9)) : AXIS_RELEASE_RESOLUTION_NONE;
		axis_db_result_get_boolean(files, i, 10, &existing->kept);
		{
			gboolean brought = FALSE;

			axis_db_result_get_boolean(files, i, 11, &brought);
			existing->kept_from = existing->kept && brought ? "Radarr" : NULL;
		}
		g_ptr_array_add(target->existing, existing);
	}
	*target_out = g_steal_pointer(&target);
	return TRUE;
}

static gboolean
name_matches(
	gpointer                data,
	const AxisImportTarget *target,
	const AxisRelease      *parsed,
	gchar                 **reason
){
	const AxisMetaTitle *film = target->owner_data;
	AxisReleaseMatch match;
	gint matched = -1;

	*reason = NULL;
	if (film == NULL)
		return TRUE;
	match = axis_release_match(parsed, &film, 1, &matched, NULL);
	/* Ambiguous here only means its year cannot be compared: it has the
	 * film's name. */
	if (match != AXIS_RELEASE_MATCH_NONE)
		return TRUE;
	if (parsed->year > 0)
		*reason = g_strdup_printf("it says \"%s\" (%d), not %s", parsed->title, parsed->year, target->label);
	else
		*reason = g_strdup_printf("it says \"%s\", not %s", parsed->title, target->label);
	return FALSE;
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
	g_autofree gchar *cut = axis_movies_text_cut(text, 4000);

	return axis_movies_store_event(conn, target->owner_id, target->title_id, target->label, kind,
		cut != NULL ? cut : kind, user, 0, target->grab_id, error);
}

static gboolean
link_file(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const AxisImportLinked *linked,
	GError                **error
){
	g_autofree gchar *file = axis_movies_int_text(linked->library_file_id);
	g_autofree gchar *movie = axis_movies_int_text(target->owner_id);
	g_autofree gchar *import = axis_movies_int_text(linked->import_id);
	g_autofree gchar *release = axis_movies_text_cut(linked->release_name, 300);
	g_autofree gchar *quality_text = axis_movies_text_cut(linked->quality_text, 1000);
	g_autofree gchar *grab = target->grab_id > 0 ? axis_movies_int_text(target->grab_id) : NULL;
	/* A file a person assigned by hand is theirs ("assigned"); else the file
	 * of a grab by force is the person's who forced it. */
	g_autofree gchar *by = axis_movies_text_cut(linked->hand_how != NULL && linked->hand_by != NULL ? linked->hand_by :
		target->grab_by, 200);
	const gchar *how = linked->hand_how != NULL ? linked->hand_how : target->grab_bound ? "forced" : NULL;
	const gchar *params[9];

	params[0] = file;
	params[1] = movie;
	params[2] = linked->quality != AXIS_QUALITY_UNKNOWN ? axis_quality_name(linked->quality) : NULL;
	params[3] = linked->user;
	params[4] = import;
	params[5] = release;
	params[6] = quality_text;
	/* The file of a grab by force is linked by the person who forced it, and
	 * kept by hand: no automatic search replaces it until a person lets it
	 * go (0062_force_grab.sql); so is a file a person assigned by hand.  Any
	 * other import's link is the import's. */
	params[7] = how;
	params[8] = how != NULL ? by : NULL;
	/* An import's link wins over identification's (which made the row, if
	 * any, from an identity: that is taken off it, and the identity is looked
	 * at again on the next run, where the import is its evidence). */
	if (!axis_db_conn_exec(conn, "INSERT INTO movie_file (file_id, movie_id, quality, linked_by, import_id, release_name,"
	        " quality_text, hand_how, hand_by, hand_at, kept) VALUES ($1::bigint, $2::bigint, $3, $4, $5::bigint, $6,"
	        " $7, $8::text, $9::text, CASE WHEN $8::text IS NOT NULL THEN now() END, $8::text IS NOT NULL)"
	        " ON CONFLICT (file_id) DO UPDATE SET movie_id = EXCLUDED.movie_id, quality = EXCLUDED.quality,"
	        " linked_at = now(), linked_by = EXCLUDED.linked_by, import_id = EXCLUDED.import_id,"
	        " release_name = EXCLUDED.release_name, quality_text = EXCLUDED.quality_text, identity_id = NULL,"
	        " resolution = NULL,"
	        /* As the TV import: what a person did, or the bringing over, to the
	         * file stays (a re-link by an import is not theirs; the import's
	         * kept check stands before this); only a link by force or by
	         * assigning replaces it.  Kept because it was brought over only
	         * while the stamp stays the bringing over's. */
	        " hand_how = coalesce(EXCLUDED.hand_how, movie_file.hand_how),"
	        " hand_by = CASE WHEN EXCLUDED.hand_how IS NOT NULL THEN EXCLUDED.hand_by ELSE movie_file.hand_by END,"
	        " hand_at = coalesce(EXCLUDED.hand_at, movie_file.hand_at),"
	        " kept = EXCLUDED.kept OR movie_file.kept,"
	        " kept_how = CASE WHEN EXCLUDED.hand_how IS NULL THEN movie_file.kept_how END", 9, params, NULL, error) ||
	    /* An import by Axis of a file at a path a person unlinked by hand is a
	     * new file: that decision does not hold for it. */
	    !axis_library_hand_unlink_clear(conn, linked->library_file_id, error) ||
	    !axis_db_conn_exec(conn, "UPDATE library_identity_scope s SET fingerprint = '00000000000000000000000000000000'"
	        " FROM library_identity i JOIN library_identity_file lif ON lif.identity_id = i.id"
	        " WHERE lif.file_id = $1::bigint AND s.root_id = i.root_id AND s.path_hash = i.scope_hash", 1, params, NULL,
	        error) ||
	    /* ... and its root's scheduler sees there is something to look at. */
	    !axis_db_conn_exec(conn, "UPDATE library_identity_root ir SET pending = pending + 1 FROM library_identity i"
	        " JOIN library_identity_file lif ON lif.identity_id = i.id WHERE lif.file_id = $1::bigint"
	        " AND ir.root_id = i.root_id", 1, params, NULL, error))
		return FALSE;
	if (grab == NULL)
		return TRUE;
	/* The grab is done with. */
	params[0] = grab;
	return axis_db_conn_exec(conn, "UPDATE movie_grab SET closed_at = now(), close_reason = 'imported',"
		" close_text = 'Imported into the library.', completed_at = coalesce(completed_at, now())"
		" WHERE id = $1::bigint AND closed_at IS NULL", 1, params, NULL, error);
}

static gboolean
dismissed(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *user,
	GError                **error
){
	g_autofree gchar *grab = target->grab_id > 0 ? axis_movies_int_text(target->grab_id) : NULL;
	const gchar *params[1];

	if (grab == NULL)
		return TRUE;
	params[0] = grab;
	return axis_db_conn_exec(conn, "UPDATE movie_grab SET closed_at = now(), close_reason = 'dismissed',"
		" close_text = 'Its import was dismissed by a person; the download was left with its client.'"
		" WHERE id = $1::bigint AND closed_at IS NULL", 1, params, NULL, error);
}

/* "Delete this download" on the import's page: the grab closed as deleted,
 * the movie's history with who, the person's block when ticked. */
static gboolean
download_deleted(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *user,
	gboolean                block,
	GError                **error
){
	AxisMovies *self = data;

	return axis_acquire_grab_deleted(self->acquire, conn, target->owner_id, target->grab_id, target->grab_release, user,
		block, error);
}

static gboolean
list_films(
	gpointer     data,
	AxisDbConn  *conn,
	GPtrArray  **choices,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	guint i;

	*choices = NULL;
	if (!axis_db_conn_exec(conn, "SELECT m.id, t.title, t.year FROM movie m JOIN meta_title t ON t.id = m.title_id"
	        " ORDER BY lower(t.title), t.year, m.id LIMIT 5000", 0, NULL, &rows, error))
		return FALSE;
	*choices = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_choice_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportChoice *choice = g_new0(AxisImportChoice, 1);

		choice->owner_id = row_int(rows, i, 0, 0);
		choice->label = axis_movies_title_label(axis_db_result_get_text(rows, i, 1), (gint)row_int(rows, i, 2, 0));
		g_ptr_array_add(*choices, choice);
	}
	return TRUE;
}

/* No file of the download can be taken (no video, an archive its client
 * did not unpack) and nothing can be imported anyway: a grab a person made by
 * force is closed (not_imported), and the movie's history says why.  The
 * import and the download stay, for a person, and the movie is not searched
 * for automatically while that import is blocked (held_forced).  A grab by
 * hand that is not forced, and a grab Axis made by itself, are left open, as
 * before. */
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
	g_autofree gchar *grab = target->grab_id > 0 ? axis_movies_int_text(target->grab_id) : NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *event = NULL;
	const gchar *params[2];

	(void)code;
	/* Only a grab by force: a grab by hand that is not forced stays open as it
	 * did, so the movie is not searched for again while a person unpacks. */
	if (grab == NULL || !target->grab_bound)
		return TRUE;
	why = g_strdup_printf("Grabbed by force, but its import could take no file of the download: %s", text);
	params[0] = grab;
	params[1] = why;
	if (!axis_db_conn_exec(conn, "UPDATE movie_grab SET closed_at = now(), close_reason = 'not_imported',"
	        " close_text = left($2, 1000) WHERE id = $1::bigint AND closed_at IS NULL AND set_aside = 'hand'"
	        " AND bound RETURNING id", 2, params, &closed, error))
		return FALSE;
	if (axis_db_result_get_n_rows(closed) == 0)
		return TRUE;
	event = g_strdup_printf("The grab by force of %s is let go: its import could take no file of the download. %s The "
		"movie is not searched for automatically while that download is held for you: open its import, or dismiss "
		"it and Axis searches again. The download is left with its client.", target->grab_release != NULL ?
		target->grab_release : "a release", text);
	return note_event(data, conn, target, "abandoned", event, NULL, error);
}

/* The movie is locked while one of a download's files is assigned to it by
 * hand (add-force-grab-and-assign, slice B). */
static gboolean
assign_lock(
	gpointer     data,
	AxisDbConn  *conn,
	gint64       owner_id,
	GError     **error
){
	g_autofree gchar *movie = axis_movies_int_text(owner_id);
	const gchar *params[1];

	params[0] = movie;
	return axis_db_conn_exec(conn, "SELECT 1 FROM movie WHERE id = $1::bigint FOR UPDATE", 1, params, NULL, error);
}

/* Another grab that is open for the movie has a download on the way for it.
 * (The import's own grab is expected.) */
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
	g_autofree gchar *movie = axis_movies_int_text(target->owner_id);
	g_autofree gchar *grab = axis_movies_int_text(target->grab_id);
	const gchar *params[2];

	*problem = NULL;
	params[0] = movie;
	params[1] = grab;
	if (!axis_db_conn_exec(conn, "SELECT release_name FROM movie_grab WHERE movie_id = $1::bigint AND closed_at IS NULL"
	        " AND id <> $2::bigint LIMIT 1", 2, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0)
		*problem = g_strdup_printf("%s is being downloaded by another grab (%s). Wait for it, or remove that download "
			"on the Downloads page, then assign again.", target->label, axis_db_result_get_text(rows, 0, 0) != NULL ?
			axis_db_result_get_text(rows, 0, 0) : "a release");
	return TRUE;
}

/* ── Duplicates (add-force-grab-and-assign, slice D) ────────────── */

/* The movies that hold more than one linked video file that is present,
 * each with its files (the Duplicates page). */
static gboolean
dup_list(
	gpointer     data,
	AxisDbConn  *conn,
	gint64       owner_id,
	GPtrArray   *groups,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *movie = axis_movies_int_text(owner_id);
	AxisImportDupGroup *group = NULL;
	const gchar *params[1];
	guint i;

	params[0] = movie;
	if (!axis_db_conn_exec(conn, "SELECT m.id, m.title_id, t.title, t.year, qp.format, qp.definition,"
	        " lf.id, lf.root_id, lf.volume_id, r.rel_path, lf.rel_path, lf.inode, lf.size, mf.quality, mf.release_name,"
	        " mf.resolution, mf.kept, mf.hand_how, mf.hand_by, mf.import_id IS NOT NULL, lf.pinned_links,"
	        " lf.pinned_downloads, mi.status, mi.duration_ms, (mi.inode = lf.inode AND mi.file_size = lf.size),"
	        " coalesce(mf.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' AND mf.hand_how = 'brought', false)"
	        " FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id JOIN library_root r ON r.id = lf.root_id"
	        " JOIN movie m ON m.id = mf.movie_id JOIN meta_title t ON t.id = m.title_id"
	        " JOIN quality_profile qp ON qp.id = m.quality_profile_id LEFT JOIN media_info mi ON mi.file_id = lf.id"
	        " WHERE lf.state = 'present' AND lf.kind = 'video' AND ($1::bigint = 0 OR m.id = $1::bigint)"
	        " AND (SELECT count(*) FROM movie_file o JOIN library_file ol ON ol.id = o.file_id"
	        "  WHERE o.movie_id = m.id AND ol.state = 'present' AND ol.kind = 'video') > 1"
	        " ORDER BY lower(t.title), m.id, lf.id LIMIT 2000", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisImportDupFile *f = axis_import_dup_file_new();
		const gchar *quality = axis_db_result_get_text(rows, i, 13);
		gint64 id = row_int(rows, i, 0, 0);
		gboolean current = FALSE;

		if (group == NULL || group->owner_id != id) {
			g_autofree gchar *unusable = NULL;

			group = axis_import_dup_group_new();
			group->owner_id = id;
			group->title_id = row_int(rows, i, 1, 0);
			group->title_label = axis_movies_title_label(axis_db_result_get_text(rows, i, 2), (gint)row_int(rows, i, 3, 0));
			group->label = g_strdup(group->title_label);
			group->href = g_strdup_printf("/films/%" G_GINT64_FORMAT, id);
			group->profile_ok = axis_quality_stored_check((gint)row_int(rows, i, 4, 0), axis_db_result_get_text(rows, i, 5),
				&group->profile, &unusable);
			g_ptr_array_add(groups, group);
		}
		f->file->library_file_id = row_int(rows, i, 6, 0);
		f->file->root_id = row_int(rows, i, 7, 0);
		f->file->volume_id = row_int(rows, i, 8, 0);
		f->file->root_path = row_text(rows, i, 9);
		f->file->rel_path = row_text(rows, i, 10);
		f->file->inode = row_int(rows, i, 11, 0);
		f->file->size = row_int(rows, i, 12, 0);
		if (quality == NULL || !axis_quality_from_name(quality, &f->file->quality))
			f->file->quality = AXIS_QUALITY_UNKNOWN;
		f->file->release_name = row_text(rows, i, 14);
		f->file->resolution = f->file->quality == AXIS_QUALITY_UNKNOWN ?
			axis_identify_resolution_from_name(axis_db_result_get_text(rows, i, 15)) : AXIS_RELEASE_RESOLUTION_NONE;
		axis_db_result_get_boolean(rows, i, 16, &f->file->kept);
		{
			gboolean brought = FALSE;

			axis_db_result_get_boolean(rows, i, 25, &brought);
			f->file->kept_from = f->file->kept && brought ? "Radarr" : NULL;
		}
		f->hand_how = row_text(rows, i, 17);
		f->hand_by = row_text(rows, i, 18);
		axis_db_result_get_boolean(rows, i, 19, &f->by_import);
		axis_db_result_get_boolean(rows, i, 20, &f->pinned_links);
		axis_db_result_get_boolean(rows, i, 21, &f->pinned_downloads);
		axis_db_result_get_boolean(rows, i, 24, &current);
		axis_import_dup_file_set_probe(f, axis_db_result_get_text(rows, i, 22), row_int(rows, i, 23, 0),
			!axis_db_result_is_null(rows, i, 24) && current);
		g_ptr_array_add(group->files, f);
	}
	return TRUE;
}

/* The movie is locked while its duplicates are judged and removed; a grab
 * that is open for it has a download on the way whose import may replace
 * these very files.  With @remove: every link of the movie is locked (none
 * is linked elsewhere, or unlinked, between the look and the removal), a
 * named file that is no longer the movie's refuses the request, and the
 * movie is appended to @others
 * with the present video files that would still be linked to it, read under
 * that lock.  Whether one of those can stay is the layer's to say. */
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
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisDbResult) left = NULL;
	g_autoptr(AxisDbResult) gone = NULL;
	g_autoptr(GString) array = g_string_new("{");
	g_autofree gchar *movie = axis_movies_int_text(owner_id);
	AxisImportDupGroup *other;
	const gchar *params[2];
	guint i;

	*refusal = NULL;
	params[0] = movie;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM movie WHERE id = $1::bigint FOR UPDATE", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT release_name FROM movie_grab WHERE movie_id = $1::bigint AND closed_at IS NULL"
	        " LIMIT 1", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) > 0) {
		*refusal = g_strdup_printf("A download for this movie is on the way (%s), and its import may replace these very "
			"files. Wait for it, or remove that download on the Downloads page, then look again.",
			axis_db_result_get_text(rows, 0, 0) != NULL ? axis_db_result_get_text(rows, 0, 0) : "a release");
		return TRUE;
	}
	if (remove == NULL || remove->len == 0 || others == NULL)
		return TRUE;
	for (i = 0; i < remove->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(remove, gint64, i));
	g_string_append_c(array, '}');
	params[1] = array->str;
	/* Every link of the movie is locked, the named files' too (their
	 * retirement deletes those on this connection).  A named file that is no
	 * longer the movie's, read under the lock, may be another movie's by now:
	 * nothing is removed. */
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM movie_file WHERE movie_id = $1::bigint ORDER BY file_id FOR UPDATE", 1,
	        params, NULL, error) ||
	    !axis_db_conn_exec(conn, "SELECT u.id FROM unnest($2::bigint[]) u(id) WHERE NOT EXISTS (SELECT 1 FROM movie_file"
	        " mf WHERE mf.file_id = u.id AND mf.movie_id = $1::bigint) LIMIT 1", 2, params, &gone, error))
		return FALSE;
	if (axis_db_result_get_n_rows(gone) > 0) {
		*refusal = g_strdup("A ticked file is no longer linked to this movie: its link changed while the request was "
			"checked, and it may be another movie's file now. Nothing was removed: look again.");
		return TRUE;
	}
	if (!axis_db_conn_exec(conn, "SELECT lf.id, lf.root_id, lf.volume_id, r.rel_path, lf.rel_path, lf.inode, lf.size,"
	        " mi.status, mi.duration_ms, (mi.inode = lf.inode AND mi.file_size = lf.size)"
	        " FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id JOIN library_root r ON r.id = lf.root_id"
	        " LEFT JOIN media_info mi ON mi.file_id = lf.id"
	        " WHERE mf.movie_id = $1::bigint AND NOT (mf.file_id = ANY ($2::bigint[])) AND lf.state = 'present'"
	        " AND lf.kind = 'video' ORDER BY lf.id LIMIT 2000", 2, params, &left, error))
		return FALSE;
	other = axis_import_dup_group_new();
	other->owner_id = owner_id;
	other->label = g_strdup("the movie");
	g_ptr_array_add(others, other);
	for (i = 0; i < axis_db_result_get_n_rows(left); i++) {
		AxisImportDupFile *f = axis_import_dup_file_new();
		gboolean current = FALSE;

		f->file->library_file_id = row_int(left, i, 0, 0);
		f->file->root_id = row_int(left, i, 1, 0);
		f->file->volume_id = row_int(left, i, 2, 0);
		f->file->root_path = row_text(left, i, 3);
		f->file->rel_path = row_text(left, i, 4);
		f->file->inode = row_int(left, i, 5, 0);
		f->file->size = row_int(left, i, 6, 0);
		axis_db_result_get_boolean(left, i, 9, &current);
		axis_import_dup_file_set_probe(f, axis_db_result_get_text(left, i, 7), row_int(left, i, 8, 0),
			!axis_db_result_is_null(left, i, 9) && current);
		g_ptr_array_add(other->files, f);
	}
	return TRUE;
}

static const AxisImportOwnerInfo owner_info = {
	AXIS_MOVIES_MODULE_ID,
	"movie",
	resolve,
	name_matches,
	link_file,
	note_event,
	dismissed,
	list_films,
	NULL,	/* match: a film's download is one file */
	NULL,
	stuck,
	NULL,	/* assign_read, assigned: one file is the movie */
	NULL,
	assign_lock,
	assign_claimed,
	dup_list,
	dup_guard,
	download_deleted
};

void
axis_movies_set_import(
	AxisMovies *self,
	AxisImport *import
){
	g_return_if_fail(self != NULL);

	self->import = import;
	if (import != NULL)
		axis_import_add_owner(import, &owner_info, self);
}

/* The follow-up's part: an import for every completed grab, and another
 * look at those that wait. */
gboolean
axis_movies_request_imports(
	AxisMovies             *self,
	AxisMovieFollowReport  *report,
	GError                **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	AxisDbConn *conn;
	gboolean ok = TRUE;
	guint i;

	if (self->import == NULL)
		return TRUE;
	if (!axis_db_pool_exec(self->pool, "SELECT g.download_id FROM movie_grab g JOIN download d ON d.id = g.download_id"
	        " WHERE g.closed_at IS NULL AND g.completed_at IS NOT NULL AND d.state = 'completed' ORDER BY g.id LIMIT 200",
	        0, NULL, &rows, error))
		return FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* Imports with no open grab (manual ones) are looked at again too. */
	ok = axis_import_requeue_due(self->import, conn, NULL, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		gboolean created = FALSE;

		ok = axis_import_request(self->import, conn, AXIS_MOVIES_MODULE_ID, row_int(rows, i, 0, 0), NULL, &created,
			error);
		if (ok && created && report != NULL)
			report->imports++;
	}
	axis_db_pool_release(self->pool, conn);
	return ok;
}
