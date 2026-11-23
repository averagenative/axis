/*
 * axis-movies-identify.c - the films' side of identification: a managed
 * film's links to the files identified as it, made when a unit is linked or
 * the film is added, dropped when the link is undone; and adding every
 * identified film of a root at once
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A link made from an identity is a movie_file row with its identity_id.  An
 * import's link of the same file (import_id) always wins: identification
 * never replaces it, and the import pipeline replaces an identification's.
 * A film with a linked file that is present is in the library, so nothing is
 * searched for it; adding films here never queues a search.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "identify/axis-identify.h"
#include "library/axis-library-name.h"

/* The film of @title_id, when it is managed: its id and label. */
static gboolean
film_of_title(
	AxisDbConn  *conn,
	gint64       title_id,
	gint64      *movie_id,
	gchar      **label,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *title = axis_movies_int_text(title_id);
	const gchar *params[1];

	*movie_id = 0;
	*label = NULL;
	params[0] = title;
	if (!axis_db_conn_exec(conn, "SELECT m.id, t.title, t.year FROM movie m JOIN meta_title t ON t.id = m.title_id"
	        " WHERE m.title_id = $1::bigint", 1, params, &rows, error))
		return FALSE;
	if (axis_db_result_get_n_rows(rows) == 0)
		return TRUE;
	axis_db_result_get_int64(rows, 0, 0, movie_id);
	{
		gint64 year = 0;

		axis_db_result_get_int64(rows, 0, 2, &year);
		*label = axis_movies_title_label(axis_db_result_get_text(rows, 0, 1), (gint)year);
	}
	return TRUE;
}

/* The links of one identity's files to @movie_id; how many were made.  An
 * import's link stands; an identification's is the newest identity's. */
static gboolean
link_files(
	AxisDbConn   *conn,
	gint64        identity_id,
	gint64        movie_id,
	const gchar  *user,
	guint        *made,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *identity = axis_movies_int_text(identity_id);
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	const gchar *params[3];

	params[0] = identity;
	params[1] = movie;
	params[2] = by;
	if (!axis_db_conn_exec(conn, "INSERT INTO movie_file (file_id, movie_id, quality, resolution, quality_text, linked_by,"
	        " identity_id) SELECT lif.file_id, $2::bigint, lif.quality, lif.resolution, lif.quality_text, $3, lif.identity_id"
	        " FROM library_identity_file lif WHERE lif.identity_id = $1::bigint"
	        /* What a person unlinked by hand stays unlinked, and what a person
	         * linked by hand stands, whatever movie it is linked to
	         * (add-force-grab-and-assign, 0063_hand_unlink.sql). */
	        " AND NOT EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = lif.file_id)"
	        " ON CONFLICT (file_id) DO UPDATE SET movie_id = EXCLUDED.movie_id, quality = EXCLUDED.quality,"
	        " resolution = EXCLUDED.resolution, quality_text = EXCLUDED.quality_text, linked_by = EXCLUDED.linked_by,"
	        " identity_id = EXCLUDED.identity_id, linked_at = now() WHERE movie_file.import_id IS NULL"
	        " AND movie_file.hand_at IS NULL RETURNING file_id", 3, params, &rows, error))
		return FALSE;
	*made = axis_db_result_get_n_rows(rows);
	return TRUE;
}

static gboolean
identity_where(
	AxisDbConn  *conn,
	gint64       identity_id,
	gchar      **where,
	GError     **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *identity = axis_movies_int_text(identity_id);
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
	g_autofree gchar *identity = axis_movies_int_text(identity_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];
	gint64 movie_id = 0;
	guint made = 0;

	/* What this identity linked before goes first (it may have been another
	 * film's); a link a person made or keeps by hand stays (one made by hand
	 * has no identity; one only kept has, and follows the identity in
	 * link_files() with its mark). */
	params[0] = identity;
	if (!axis_db_conn_exec(conn, "DELETE FROM movie_file WHERE identity_id = $1::bigint AND hand_at IS NULL AND NOT kept",
	        1, params, NULL, error) ||
	    !film_of_title(conn, title_id, &movie_id, &label, error))
		return FALSE;
	if (movie_id <= 0) {
		/* The title is no managed movie: a link only kept (an identity's, no
		 * hand mark) must not stay on the movie it was on. */
		return axis_db_conn_exec(conn, "DELETE FROM movie_file WHERE identity_id = $1::bigint AND hand_at IS NULL", 1,
			params, NULL, error);
	}
	if (!link_files(conn, identity_id, movie_id, user, &made, error))
		return FALSE;
	/* Every file had an import's link already: nothing new to say. */
	if (made == 0)
		return TRUE;
	if (!identity_where(conn, identity_id, &where, error))
		return FALSE;
	text = g_strdup_printf("Found on disk%s%s%s%s: %s.", user != NULL ? ", confirmed by " : " (sure)",
		user != NULL ? user : "", where != NULL ? ", at " : "", where != NULL ? where : "",
		made == 1 ? "its file is linked" : "its files are linked");
	return axis_movies_store_event(conn, movie_id, title_id, label, "identified", text, user, 0, 0, error);
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
	g_autofree gchar *identity = axis_movies_int_text(identity_id);
	g_autofree gchar *label = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[1];
	gint64 movie_id = 0;
	gint64 title_id = 0;

	params[0] = identity;
	if (!axis_db_conn_exec(conn, "DELETE FROM movie_file mf USING movie m WHERE mf.identity_id = $1::bigint"
	        " AND mf.hand_at IS NULL AND m.id = mf.movie_id RETURNING m.id, m.title_id", 1, params, &gone, error))
		return FALSE;
	if (axis_db_result_get_n_rows(gone) == 0)
		return TRUE;
	axis_db_result_get_int64(gone, 0, 1, &title_id);
	if (!film_of_title(conn, title_id, &movie_id, &label, error) || !identity_where(conn, identity_id, &where, error))
		return FALSE;
	if (movie_id <= 0)
		return TRUE;
	text = g_strdup_printf("The link to the folder on disk%s%s was undone%s%s: the movie no longer counts %s.",
		where != NULL ? " at " : "", where != NULL ? where : "", user != NULL ? " by " : "", user != NULL ? user : "",
		axis_db_result_get_n_rows(gone) == 1 ? "that file" : "those files");
	return axis_movies_store_event(conn, movie_id, title_id, label, "unidentified", text, user, 0, 0, error);
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
	gint64 movie_id = 0;

	*managed = FALSE;
	if (!film_of_title(conn, title_id, &movie_id, &label, error))
		return FALSE;
	*managed = movie_id > 0;
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
	g_autofree gchar *root = axis_movies_int_text(root_id);
	const gchar *params[1];
	gint64 n = 0;

	*managed = 0;
	params[0] = root;
	if (!axis_db_conn_exec(conn, "SELECT count(DISTINCT i.title_id) FROM library_identity i JOIN movie m ON"
	        " m.title_id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked'", 1, params, &rows, error))
		return FALSE;
	axis_db_result_get_int64(rows, 0, 0, &n);
	*managed = (guint)MAX(n, 0);
	return TRUE;
}

gboolean
axis_movies_link_identified(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	guint        *linked,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *title = axis_movies_int_text(title_id);
	const gchar *params[2];

	*linked = 0;
	params[0] = movie;
	params[1] = title;
	if (!axis_db_conn_exec(conn, "INSERT INTO movie_file (file_id, movie_id, quality, resolution, quality_text, linked_by,"
	        " identity_id) SELECT lif.file_id, $1::bigint, lif.quality, lif.resolution, lif.quality_text, i.decided_by, i.id"
	        " FROM library_identity i JOIN library_identity_file lif ON lif.identity_id = i.id"
	        " WHERE i.title_id = $2::bigint AND i.state = 'linked'"
	        " AND NOT EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = lif.file_id)"
	        " ON CONFLICT (file_id) DO NOTHING RETURNING file_id", 2, params, &rows, error))
		return FALSE;
	*linked = axis_db_result_get_n_rows(rows);
	return TRUE;
}

/* Every film linked in @root_id and not managed yet, managed now: one
 * transaction, three statements, whatever their number. */
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
	AxisMovies *self = data;
	g_autoptr(AxisDbResult) fresh = NULL;
	g_autoptr(AxisDbResult) total = NULL;
	g_autofree gchar *root = axis_movies_int_text(root_id);
	g_autofree gchar *profile = NULL;
	g_autofree gchar *floor = axis_movies_int_text(axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_FLOOR,
		6 * 3600));
	g_autofree gchar *availability = axis_movies_setting_string(self, AXIS_MOVIES_SETTING_AVAILABILITY, "released");
	g_autofree gchar *text = NULL;
	g_autofree gchar *profile_name = NULL;
	g_autofree gchar *root_name = NULL;
	g_autoptr(AxisQualityStored) stored = NULL;
	g_autoptr(AxisDbResult) root_row = NULL;
	AxisMovieAvailability parsed = AXIS_MOVIE_RELEASED;
	AxisQualityProfile checked;
	const gchar *params[7];
	AxisDbConn *conn;
	gint64 n_titles = 0;
	gboolean ok;

	*added = 0;
	*skipped = 0;
	if (!axis_movie_availability_from_name(availability, &parsed)) {
		g_free(availability);
		availability = g_strdup("released");
	}
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The profile (0: the default) and the root, as for one film. */
	if (profile_id > 0) {
		ok = axis_quality_store_read_id(conn, profile_id, &stored, error);
	} else {
		g_autofree gchar *slug = axis_quality_store_default_slug(self->quality);

		ok = axis_quality_store_read(conn, slug, &stored, error);
	}
	if (ok && (stored == NULL || !axis_quality_store_load(conn, stored->id, &checked, NULL))) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "There is no usable quality "
			"profile of that kind.");
		ok = FALSE;
	}
	params[0] = root;
	ok = ok && axis_db_conn_exec(conn, "SELECT name FROM library_root WHERE id = $1::bigint AND media_type = 'movies'", 1,
		params, &root_row, error);
	if (ok && axis_db_result_get_n_rows(root_row) == 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID, "That is not a library folder for "
			"movies.");
		ok = FALSE;
	}
	if (!ok) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	profile = axis_movies_int_text(stored->id);
	profile_name = g_strdup(stored->name);
	root_name = g_strdup(axis_db_result_get_text(root_row, 0, 0));
	text = g_strdup_printf("Added%s%s with every identified movie of %s, with the quality profile %s; %s. Its file is in "
		"the library, so nothing was searched for.", user != NULL ? " by " : "", user != NULL ? user : "", root_name,
		profile_name, monitored ? "monitored for upgrades" : "not monitored");
	params[1] = profile;
	params[2] = monitored ? "true" : "false";
	params[3] = availability;
	params[4] = user;
	params[5] = floor;
	params[6] = text;
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "SELECT count(DISTINCT i.title_id) FROM library_identity i JOIN meta_title t ON"
			" t.id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked' AND t.kind = 'film'", 1, params,
			&total, error) &&
		axis_db_conn_exec(conn, "WITH titles AS (SELECT DISTINCT i.title_id FROM library_identity i"
			"   JOIN meta_title t ON t.id = i.title_id WHERE i.root_id = $1::bigint AND i.state = 'linked'"
			"   AND t.kind = 'film'),"
			" fresh AS (INSERT INTO movie (title_id, quality_profile_id, root_id, monitored, min_availability, added_by,"
			"   search_floor_s) SELECT title_id, $2::bigint, $1::bigint, $3::boolean, $4, $5, $6::integer FROM titles"
			"   ORDER BY title_id ON CONFLICT (title_id) DO NOTHING RETURNING id, title_id),"
			" files AS (INSERT INTO movie_file (file_id, movie_id, quality, resolution, quality_text, linked_by,"
			"   identity_id) SELECT lif.file_id, f.id, lif.quality, lif.resolution, lif.quality_text, i.decided_by, i.id"
			"   FROM fresh f JOIN library_identity i ON i.title_id = f.title_id AND i.state = 'linked'"
			"   JOIN library_identity_file lif ON lif.identity_id = i.id"
			"   WHERE NOT EXISTS (SELECT 1 FROM library_hand_unlink hu WHERE hu.file_id = lif.file_id)"
			"   ON CONFLICT (file_id) DO NOTHING RETURNING 1),"
			" events AS (INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user)"
			"   SELECT f.id, f.title_id, CASE WHEN t.year > 0 THEN t.title || ' (' || t.year || ')' ELSE t.title END,"
			"   'added', $7, $5 FROM fresh f JOIN meta_title t ON t.id = f.title_id RETURNING 1)"
			" SELECT (SELECT count(*) FROM fresh), (SELECT count(*) FROM files), (SELECT count(*) FROM events)", 7, params,
			&fresh, error) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	axis_db_result_get_int64(total, 0, 0, &n_titles);
	{
		gint64 n_added = 0;

		axis_db_result_get_int64(fresh, 0, 0, &n_added);
		*added = (guint)MAX(n_added, 0);
		*skipped = (guint)MAX(n_titles - n_added, 0);
	}
	return TRUE;
}

static const AxisIdentifyOwnerInfo owner_info = {
	AXIS_MOVIES_MODULE_ID,
	on_link,
	on_unlink,
	on_manage,
	on_managed,
	on_count_managed,
	AXIS_META_KIND_FILM
};

void
axis_movies_set_identify(
	AxisMovies   *self,
	AxisIdentify *identify
){
	g_return_if_fail(self != NULL);

	if (identify != NULL)
		axis_identify_add_owner(identify, &owner_info, self);
}
