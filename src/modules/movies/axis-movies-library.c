/*
 * axis-movies-library.c - the movies on disk that Axis does not manage:
 * their facts and their page, read at query time from identification's
 * links and the file index
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A movie on disk is a title of kind film that identification has linked
 * (in a root for movies) to a file that is present, and that no film row
 * manages.  Nothing is written for it: it is not a film, it is in no job, it
 * has no profile and is not monitored, so the scheduler, the follow-up and
 * the search never see it, and no file of it is renamed, moved, upgraded or
 * touched.  It is one entry whatever the number of its files, folders and
 * disks; its copies are counted and shown.
 *
 * Its life follows its links: a unit linked to it puts it in the list, an
 * undone link or files found gone take it out (a file on a disk that is
 * offline is still present: the scanner marks a file absent only after
 * walking a disk verified online), adding the title as a film makes it a
 * managed film with those files (axis-movies-identify.c), and removing that
 * film leaves it here again while its files are on disk.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* One row per title of @1 (bigint[]) that is a movie on disk; @2 the volume
 * states a disk is usable in. */
static const gchar library_select[] =
	"SELECT t.id, t.title, t.original_title, t.year, t.release_date::text, t.status, t.runtime_minutes,"
	" t.poster_ref, t.provider, " US("d.found_at") ", d.n_files, d.size, d.qualities, d.copies, d.away,"
	" d.away_volume, d.root_id, r.name"
	" FROM (SELECT li.title_id, min(coalesce(li.decided_at, li.created_at)) AS found_at, count(*) AS n_files,"
	"   coalesce(sum(llf.size), 0) AS size,"
	"   string_agg(coalesce(llif.quality, '') || ':' || coalesce(llif.resolution, ''), ',')"
	"     FILTER (WHERE llif.quality IS NOT NULL OR llif.resolution IS NOT NULL) AS qualities,"
	"   count(DISTINCT li.id) AS copies,"
	"   count(*) FILTER (WHERE NOT coalesce(lv.state = ANY ($2::text[]), false)) AS away,"
	"   min(lv.name) FILTER (WHERE NOT coalesce(lv.state = ANY ($2::text[]), false)) AS away_volume,"
	"   min(li.root_id) AS root_id"
	"   FROM" AXIS_MOVIES_ON_DISK_TABLES " LEFT JOIN volume lv ON lv.id = llf.volume_id"
	AXIS_MOVIES_ON_DISK_WHERE " AND li.title_id = ANY ($1::bigint[]) GROUP BY li.title_id) d"
	" JOIN meta_title t ON t.id = d.title_id AND t.kind = 'film'"
	" LEFT JOIN library_root r ON r.id = d.root_id"
	" ORDER BY lower(t.title), t.year, t.id";

enum
{
	L_TITLE_ID, L_TITLE, L_ORIGINAL, L_YEAR, L_RELEASE, L_STATUS, L_RUNTIME, L_POSTER, L_PROVIDER,
	L_FOUND, L_FILES, L_SIZE, L_QUALITIES, L_COPIES, L_AWAY, L_AWAY_VOLUME, L_ROOT_ID, L_ROOT_NAME
};

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

static AxisMovieFacts *
library_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisMovieFacts *f = g_new0(AxisMovieFacts, 1);

	f->library_only = TRUE;
	f->title_id = row_int(result, row, L_TITLE_ID, 0);
	f->title = row_text(result, row, L_TITLE);
	f->original_title = row_text(result, row, L_ORIGINAL);
	f->year = (gint)row_int(result, row, L_YEAR, 0);
	f->release_date = row_text(result, row, L_RELEASE);
	f->status = axis_meta_status_from_string(axis_db_result_get_text(result, row, L_STATUS));
	f->runtime = (gint)row_int(result, row, L_RUNTIME, 0);
	f->poster_ref = row_text(result, row, L_POSTER);
	f->provider = row_text(result, row, L_PROVIDER);
	/* No profile, no destination of its own, not monitored, never searched. */
	f->profile_cutoff = AXIS_QUALITY_UNKNOWN;
	f->root_id = row_int(result, row, L_ROOT_ID, 0);
	f->root_name = row_text(result, row, L_ROOT_NAME);
	f->volume_state = AXIS_VOLUME_ONLINE;
	f->monitored = FALSE;
	f->availability = AXIS_MOVIE_RELEASED;
	f->added_us = row_int(result, row, L_FOUND, 0);
	f->download_size = -1;
	f->download_done = -1;
	f->download_rate = -1;
	f->download_eta = -1;
	f->n_files = (guint)row_int(result, row, L_FILES, 0);
	f->size_bytes = row_int(result, row, L_SIZE, 0);
	f->n_copies = (guint)row_int(result, row, L_COPIES, 0);
	f->n_away = (guint)row_int(result, row, L_AWAY, 0);
	f->away_volume = row_text(result, row, L_AWAY_VOLUME);
	axis_movies_best_on_disk(axis_db_result_get_text(result, row, L_QUALITIES), &f->file_quality,
		&f->file_resolution);
	return f;
}

gboolean
axis_movies_store_library(
	AxisDbConn   *conn,
	const gchar  *titles,
	GPtrArray   **films,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *usable = axis_movies_usable_states();
	const gchar *params[2];
	guint i;

	params[0] = titles;
	params[1] = usable;
	if (!axis_db_conn_exec(conn, library_select, 2, params, &result, error))
		return FALSE;
	*films = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_facts_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*films, library_from_row(result, i));
	return TRUE;
}

gboolean
axis_movies_store_on_disk(
	AxisDbConn   *conn,
	gint64        title_id,
	gboolean     *on_disk,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *title = axis_movies_int_text(title_id);
	const gchar *params[1];

	*on_disk = FALSE;
	params[0] = title;
	if (!axis_db_conn_exec(conn, "SELECT EXISTS (SELECT 1 FROM" AXIS_MOVIES_ON_DISK_TABLES AXIS_MOVIES_ON_DISK_WHERE
	        " AND li.title_id = $1::bigint)", 1, params, &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, on_disk);
	return TRUE;
}

gboolean
axis_movies_get_library_by_titles(
	AxisMovies  *self,
	GArray      *title_ids,
	GPtrArray  **films,
	GError     **error
){
	g_autoptr(GString) array = g_string_new("{");
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && title_ids != NULL && films != NULL, FALSE);

	for (i = 0; i < title_ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(title_ids, gint64, i));
	g_string_append_c(array, '}');
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_library(conn, array->str, films, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* The files of its units, how each came there: present ones first, the best
 * of a unit first, at most twenty. */
static const gchar library_files[] =
	"SELECT lf.id, r.name, v.name, lf.rel_path, lf.size, lf.state, llif.quality, llif.quality_text, lf.pinned,"
	" NULL::bigint, NULL::text, " US("coalesce(li.decided_at, li.created_at)") ", llif.resolution, li.id,"
	" li.linked_how, llif.best, NOT coalesce(v.state = ANY ($2::text[]), false)"
	" FROM library_identity li JOIN library_root llr ON llr.id = li.root_id AND llr.media_type = 'movies'"
	" JOIN library_identity_file llif ON llif.identity_id = li.id JOIN library_file lf ON lf.id = llif.file_id"
	" JOIN library_root r ON r.id = lf.root_id JOIN volume v ON v.id = lf.volume_id"
	" WHERE li.state = 'linked' AND li.title_id = $1::bigint"
	" AND NOT EXISTS (SELECT 1 FROM movie_file lmf WHERE lmf.file_id = lf.id)"
	" ORDER BY lf.state = 'present' DESC, llif.best DESC, li.id, lf.rel_path LIMIT 20";

gboolean
axis_movies_get_library_detail(
	AxisMovies       *self,
	gint64            title_id,
	guint             n_events,
	AxisMovieDetail **detail_out,
	GError          **error
){
	g_autoptr(AxisMovieDetail) detail = g_new0(AxisMovieDetail, 1);
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *titles = g_strdup_printf("{%" G_GINT64_FORMAT "}", title_id);
	g_autofree gchar *title = axis_movies_int_text(title_id);
	g_autofree gchar *usable = axis_movies_usable_states();
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	g_return_val_if_fail(self != NULL && detail_out != NULL, FALSE);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_library(conn, titles, &films, error);
	if (ok && films->len == 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie on disk");
		ok = FALSE;
	}
	if (ok)
		detail->facts = g_ptr_array_steal_index(films, 0);
	/* Not managed: no search, no release considered, no blocklist; the
	 * history its title kept from a film removed before. */
	detail->candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_candidate_free);
	detail->blocklist = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_block_free);
	ok = ok && axis_movies_store_title_events(conn, title_id, MAX(n_events, 1), &detail->events, error);
	params[0] = title;
	params[1] = usable;
	ok = ok && axis_db_conn_exec(conn, library_files, 2, params, &rows, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	detail->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_file_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(detail->files, axis_movies_file_from_row(rows, i));
	*detail_out = g_steal_pointer(&detail);
	return TRUE;
}
