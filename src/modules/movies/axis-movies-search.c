/*
 * axis-movies-search.c - the films' side of the acquisition layer: what a
 * film wants, what its releases are matched against, its claim and its
 * history; the movies.search job's work and an administrator's grab
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The search itself (asking the indexers, reading, matching, deciding,
 * ranking, storing every release, claiming, fetching and sending once) is
 * the acquisition layer's (src/acquire/axis-acquire-search.c).  What a film
 * adds:
 *
 *  - load(): the film's facts (the one facts query); nothing is asked while
 *    a grab of it is in flight or before its minimum availability.
 *  - read_disk(): its best file present in the library (imported or
 *    identified), which a release must be an upgrade over, by the rules its
 *    import would apply.
 *  - prepare(): the film's full record, every other film of its name the
 *    metadata provider knows (a release that matches one of them is not
 *    this film), and the request by its identifiers (the indexer layer falls
 *    back to the title and year).
 *  - judge(): a release whose indexer names another film's IMDb or TMDb
 *    identifier is not this film, by a fixed rule.
 *  - claim_check(): the facts read again under the film's row lock, and
 *    axis_movie_in_flight(); the unique index movie_grab_one_open refuses a
 *    second open grab besides.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "library/axis-library-name.h"

/* What a film's search holds besides the facts. */
typedef struct
{
	AxisMovies          *self;
	AxisMovieFacts      *facts;
	AxisMetaTitleAnswer  title;	/* the film's full record */
	GPtrArray           *others;	/* AxisMetaTitle, owned: same name */
	GPtrArray           *titles;	/* the film, then the others: borrowed */
} Film;

static void
film_free(Film *film)
{
	axis_movie_facts_free(film->facts);
	axis_meta_title_answer_clear(&film->title);
	g_clear_pointer(&film->others, g_ptr_array_unref);
	g_free(film);
}

static gboolean
load(
	gpointer                  data,
	AxisDbConn               *conn,
	const AxisAcquireTarget  *target,
	AxisAcquireWant         **want_out,
	const gchar             **skip,
	GError                  **error
){
	AxisMovies *self = data;
	g_autoptr(AxisMovieFacts) facts = NULL;
	AxisAcquireWant *want;
	AxisMovieRules rules;
	Film *film;

	*want_out = NULL;
	*skip = NULL;
	if (!axis_movies_store_facts_one(conn, target->owner_id, &facts, error))
		return FALSE;
	if (facts == NULL) {
		*skip = "the movie was removed";
		return TRUE;
	}
	axis_movies_get_rules(self, &rules);
	want = axis_acquire_want_new();
	want->owner_id = facts->id;
	want->title_id = facts->title_id;
	want->label = axis_movies_title_label(facts->title, facts->year);
	want->in_flight = axis_movie_in_flight(facts);
	want->profile_id = facts->profile_id;
	want->profile_name = g_strdup(facts->profile_name);
	want->profile_usable = facts->profile_usable;
	want->root_id = facts->root_id;
	want->root_name = g_strdup(facts->root_name);
	want->volume_id = facts->volume_id;
	want->volume_name = g_strdup(facts->volume_name);
	want->runtime_minutes = facts->runtime;
	g_array_append_val(want->pace_ids, facts->id);
	/* A person's own look or grab: whatever the automatic rules say now (a
	 * grab by hand is still refused while one is in flight). */
	if (!target->by_hand && want->in_flight)
		*skip = "a grab of it is in flight";
	else if (!target->by_hand && !axis_movie_available(facts, &rules, NULL))
		*skip = "it is not available yet";
	/* A file a person keeps by hand is replaced only by a person (a manual
	 * search and its grab): neither the scheduled search nor Search now. */
	/* So is a file kept because it was migrated (import-from-arr), until a
	 * person lets it go.  (Both sentences hold "its file is kept": the grab of
	 * one click reads it, axis_acquire_grab().) */
	else if (!target->by_hand && facts->kept_by != NULL && facts->n_files > 0)
		*skip = facts->kept_brought ? "its file is kept as it was migrated from Radarr" : "its file is kept by hand";
	/* A release grabbed by force that no file could be taken from: its
	 * download waits for a person, so the movie is not searched for meanwhile. */
	else if (!target->by_hand && facts->held_forced != NULL)
		*skip = "a download grabbed by force is held for a person";
	/* Brought over, and its searches not started (0068_arr_brought.sql): THE
	 * gate every automatic search of a movie passes, whatever queued it (the
	 * scheduler, a change of profile, the follow-up after a failed grab).  A
	 * person's Search now and manual search end the hold before they queue
	 * (axis_movies_request_search(), axis_movies_request_manual_search()). */
	else if (!target->by_hand && axis_movie_hold_stops(facts->search_hold))
		*skip = "it was migrated, and searching for it has not been started";
	film = g_new0(Film, 1);
	film->self = self;
	film->facts = g_steal_pointer(&facts);
	want->data = film;
	want->data_free = (GDestroyNotify)film_free;
	*want_out = want;
	return TRUE;
}

/*
 * The film's best file present in the library (by the on-disk order: a
 * ladder quality, or an unknown source at a resolution), with the name it
 * is read by: the release it was imported from, else its own name.
 */
static gboolean
read_disk(
	gpointer          data,
	AxisDbConn       *conn,
	AxisAcquireWant  *want,
	GError          **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *movie = axis_movies_int_text(want->owner_id);
	g_autofree gchar *name = NULL;
	const gchar *params[1];
	gint best = -1;
	AxisQuality best_quality = AXIS_QUALITY_UNKNOWN;
	AxisQuality best_source = AXIS_QUALITY_UNKNOWN;
	AxisReleaseResolution best_resolution = AXIS_RELEASE_RESOLUTION_NONE;
	gboolean best_converted = FALSE;
	guint i;

	params[0] = movie;
	/* A file Axis converted (its done replacement) counts as the quality of
	 * the file it was made from. */
	if (!axis_db_conn_exec(conn, "SELECT mf.quality, mf.resolution, mf.release_name, lf.base_name, cv.source_quality,"
	        " cv.id IS NOT NULL FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id"
	        " LEFT JOIN LATERAL (SELECT c.id, c.source_quality FROM conversion c WHERE c.file_id = lf.id"
	        "  AND c.state = 'done' AND c.outcome = 'replace' ORDER BY c.id DESC LIMIT 1) cv ON true"
	        " WHERE mf.movie_id = $1::bigint AND lf.state = 'present' ORDER BY lf.id LIMIT 50", 1, params, &rows, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *quality_name = axis_db_result_get_text(rows, i, 0);
		const gchar *source_name = axis_db_result_get_text(rows, i, 4);
		AxisQuality quality = AXIS_QUALITY_UNKNOWN;
		AxisQuality source = AXIS_QUALITY_UNKNOWN;
		AxisReleaseResolution resolution = AXIS_RELEASE_RESOLUTION_NONE;
		gboolean converted = FALSE;

		axis_db_result_get_boolean(rows, i, 5, &converted);
		if (converted) {
			if (source_name == NULL || !axis_quality_from_name(source_name, &source))
				source = AXIS_QUALITY_UNKNOWN;
			quality = source;
		} else if (quality_name == NULL || !axis_quality_from_name(quality_name, &quality)) {
			resolution = axis_identify_resolution_from_name(axis_db_result_get_text(rows, i, 1));
		}
		if (best < 0 || axis_quality_compare_on_disk(quality, resolution, best_quality, best_resolution) > 0) {
			best = (gint)i;
			best_quality = quality;
			best_resolution = resolution;
			best_converted = converted;
			best_source = source;
		}
	}
	if (best < 0)
		return TRUE;
	if (axis_db_result_get_text(rows, (guint)best, 2) != NULL)
		name = g_strdup(axis_db_result_get_text(rows, (guint)best, 2));
	else if (axis_db_result_get_text(rows, (guint)best, 3) != NULL)
		name = axis_library_name_display(axis_db_result_get_text(rows, (guint)best, 3));
	want->on_disk = TRUE;
	axis_acquire_disk_set(&want->disk, &want->disk_fields, &want->disk_name, best_quality, best_resolution, name);
	want->disk.converted = best_converted;
	want->disk.source_quality = best_source;
	return TRUE;
}

static gboolean
prepare(
	gpointer             data,
	AxisAcquireWant     *want,
	GCancellable        *cancellable,
	AxisIndexerRequest  *request,
	GPtrArray          **titles,
	gchar              **problem,
	GError             **error
){
	AxisMovies *self = data;
	Film *film = want->data;
	g_autoptr(GError) local = NULL;
	AxisMetaSearchAnswer same;
	const AxisMetaTitle *record;
	const gchar *tmdb;
	const gchar *imdb;
	guint i;

	*titles = NULL;
	*problem = NULL;
	memset(&same, 0, sizeof(same));
	/* The film's full record, and every film of its name. */
	if (!axis_meta_get_title(self->meta, film->facts->title_id, AXIS_MOVIES_META_WAIT_MS, &film->title, &local) ||
	    film->title.title == NULL) {
		*problem = g_strdup_printf("Its details could not be read%s%s",
			local != NULL ? ": " : ".", local != NULL ? local->message : "");
		return TRUE;
	}
	g_clear_error(&local);
	record = film->title.title;
	film->others = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	if (axis_meta_search(self->meta, record->provider, AXIS_META_KIND_FILM, record->title, 0,
	        AXIS_MOVIES_META_WAIT_MS, &same, &local)) {
		for (i = 0; same.titles != NULL && i < same.titles->len; i++) {
			AxisMetaTitle *other = g_ptr_array_index(same.titles, i);

			if (other->id == record->id)
				continue;
			same.titles->pdata[i] = NULL;
			g_ptr_array_add(film->others, other);
		}
	} else {
		g_debug("films: the titles of that name could not be looked up: %s", local->message);
	}
	axis_meta_search_answer_clear(&same);
	*titles = g_ptr_array_new();
	g_ptr_array_add(*titles, (gpointer)record);
	for (i = 0; i < film->others->len; i++)
		g_ptr_array_add(*titles, g_ptr_array_index(film->others, i));

	tmdb = axis_meta_title_get_id(record, "tmdb");
	imdb = axis_meta_title_get_id(record, "imdb");
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = record->title;
	request->year = record->year > 0 ? record->year : film->facts->year;
	request->imdb_id = imdb != NULL && axis_indexer_imdb_valid(imdb) ? imdb : NULL;
	request->tmdb_id = tmdb != NULL ? g_ascii_strtoll(tmdb, NULL, 10) : 0;
	want->runtime_minutes = record->runtime > 0 ? record->runtime : film->facts->runtime;
	return TRUE;
}

/* A release whose indexer states an identifier of another film. */
static gchar *
id_mismatch(
	const AxisIndexerRelease *release,
	const AxisMetaTitle      *title
){
	const gchar *imdb = axis_meta_title_get_id(title, "imdb");
	const gchar *tmdb = axis_meta_title_get_id(title, "tmdb");

	if (release->imdb_id != NULL && imdb != NULL && g_ascii_strcasecmp(release->imdb_id, imdb) != 0)
		return g_strdup_printf("its indexer says it is %s, another movie than this one (%s)", release->imdb_id, imdb);
	if (release->tmdb_id > 0 && tmdb != NULL && g_ascii_strtoll(tmdb, NULL, 10) != release->tmdb_id)
		return g_strdup_printf("its indexer says it is TMDb %" G_GINT64_FORMAT ", another movie than this one (TMDb %s)",
			release->tmdb_id, tmdb);
	return NULL;
}

static void
judge(
	gpointer               data,
	AxisAcquireWant       *want,
	AxisAcquireConsidered *c
){
	Film *film = want->data;
	gchar *other;

	/* Read again without its record (another profile tried on a stored
	 * release): the match the search stored stands. */
	if (film->title.title == NULL)
		return;
	other = id_mismatch(c->release, film->title.title);
	if (other == NULL)
		return;
	c->match = AXIS_RELEASE_MATCH_NONE;
	g_free(c->match_text);
	c->match_text = other;
}

/* In the claim's transaction, the film's row locked: the one facts query
 * again, and the grab-once check. */
static gboolean
claim_check(
	gpointer                   data,
	AxisDbConn                *conn,
	AxisAcquireWant           *want,
	const AxisAcquireGrabSpec *grab,
	gboolean                  *gone,
	gboolean                  *in_flight,
	GError                   **error
){
	g_autoptr(AxisMovieFacts) now = NULL;

	*gone = FALSE;
	*in_flight = FALSE;
	if (!axis_movies_store_facts_one(conn, want->owner_id, &now, error))
		return FALSE;
	if (now == NULL)
		*gone = TRUE;
	else
		*in_flight = axis_movie_in_flight(now);
	return TRUE;
}

static gboolean
event(
	gpointer               data,
	AxisDbConn            *conn,
	gint64                 owner_id,
	const AxisAcquireWant *want,
	const gchar           *kind,
	const gchar           *user,
	gint64                 search_id,
	gint64                 grab_id,
	const gchar           *text,
	GError               **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *movie = NULL;
	g_autofree gchar *label = NULL;
	const gchar *params[1];
	gint64 title_id = 0;

	if (want != NULL) {
		const Film *film = want->data;

		return axis_movies_store_event(conn, film->facts->id, film->facts->title_id, want->label, kind, text, user,
			search_id, grab_id, error);
	}
	movie = axis_movies_int_text(owner_id);
	params[0] = movie;
	if (!axis_db_conn_exec(conn, "SELECT m.title_id, t.title, t.year FROM movie m JOIN meta_title t ON t.id = m.title_id"
	        " WHERE m.id = $1::bigint", 1, params, &row, error))
		return FALSE;
	if (axis_db_result_get_n_rows(row) == 0)
		return TRUE;
	{
		gint64 year = 0;

		axis_db_result_get_int64(row, 0, 0, &title_id);
		axis_db_result_get_int64(row, 0, 2, &year);
		label = axis_movies_title_label(axis_db_result_get_text(row, 0, 1), (gint)year);
	}
	return axis_movies_store_event(conn, owner_id, title_id, label, kind, text, user, search_id, grab_id, error);
}

static gboolean
retry(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
){
	return axis_movies_store_enqueue_search(conn, owner_id, "retry", user, NULL, NULL, error);
}

static gboolean
follow_more(
	gpointer                  data,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	return axis_movies_request_imports(data, report, error);
}

/* Whether the film is monitored (automatic searches look for it). */
static gboolean
monitored(
	gpointer      data,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	gboolean     *is_monitored,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *id = axis_movies_int_text(owner_id);
	const gchar *params[1];

	*is_monitored = FALSE;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT monitored FROM movie WHERE id = $1::bigint", 1, params, &row, error))
		return FALSE;
	*is_monitored = axis_db_result_get_n_rows(row) > 0 && g_strcmp0(axis_db_result_get_text(row, 0, 0), "t") == 0;
	return TRUE;
}

/* What a grab by hand of a stored release would replace: the film's files
 * in the library, each by its quality (a converted one by its source's). */
static gboolean
hand_preview(
	gpointer                    data,
	AxisDbConn                 *conn,
	AxisAcquireWant            *want,
	const AxisAcquireCandidate *candidate,
	AxisAcquireHandPreview     *preview,
	GError                    **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *movie = axis_movies_int_text(want->owner_id);
	const gchar *params[1];
	guint i;

	params[0] = movie;
	if (!axis_db_conn_exec(conn, "SELECT mf.quality, mf.resolution, lf.base_name, cv.source_quality, cv.id IS NOT NULL,"
	        " lf.id, mf.kept, coalesce(mf.hand_by, 'a person'),"
	        " mf.kept_how IS NOT DISTINCT FROM '" AXIS_MOVIE_KEPT_HOW_BROUGHT "'"
	        " AND mf.hand_how IS NOT DISTINCT FROM 'brought' FROM movie_file mf"
	        " JOIN library_file lf ON lf.id = mf.file_id"
	        " LEFT JOIN LATERAL (SELECT c.id, c.source_quality FROM conversion c WHERE c.file_id = lf.id"
	        "  AND c.state = 'done' AND c.outcome = 'replace' ORDER BY c.id DESC LIMIT 1) cv ON true"
	        " WHERE mf.movie_id = $1::bigint AND lf.state = 'present' AND lf.kind = 'video' ORDER BY lf.id LIMIT 50", 1,
	        params, &rows, error))
		return FALSE;
	g_ptr_array_add(preview->fills, g_strdup("the movie"));
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		const gchar *name = axis_db_result_get_text(rows, i, 2);
		const gchar *quality_name = axis_db_result_get_text(rows, i, 0);
		AxisQuality quality = AXIS_QUALITY_UNKNOWN;
		AxisReleaseResolution resolution = AXIS_RELEASE_RESOLUTION_NONE;
		gboolean converted = FALSE;

		axis_db_result_get_boolean(rows, i, 4, &converted);
		if (converted)
			quality_name = axis_db_result_get_text(rows, i, 3);
		if (quality_name == NULL || !axis_quality_from_name(quality_name, &quality)) {
			quality = AXIS_QUALITY_UNKNOWN;
			if (!converted)
				resolution = axis_identify_resolution_from_name(axis_db_result_get_text(rows, i, 1));
		}
		{
			g_autofree gchar *label = name != NULL ? g_strdup_printf("the file %s", name) : g_strdup("its file");
			AxisAcquireReplaced *replaced;
			gboolean kept = FALSE;
			gint64 file_id = 0;

			axis_db_result_get_int64(rows, i, 5, &file_id);
			axis_db_result_get_boolean(rows, i, 6, &kept);
			replaced = axis_acquire_replaced_new(file_id, label, quality, resolution);
			/* Kept by hand: replaced only with the tick that names it. */
			if (kept) {
				gboolean brought = FALSE;

				axis_db_result_get_boolean(rows, i, 8, &brought);
				replaced->kept_by = g_strdup(axis_db_result_get_text(rows, i, 7));
				/* Kept because it was migrated: no person is named. */
				replaced->kept_from = brought ? "Radarr" : NULL;
			}
			g_ptr_array_add(preview->replaces, replaced);
		}
	}
	return TRUE;
}

static const AxisAcquireOwnerInfo acquire_info = {
	AXIS_MOVIES_MODULE_ID,
	"movie",
	"movie",
	"movie_id",
	"movie_search",
	"movie_candidate",
	"movie_grab",
	"movie_blocklist",
	"movie",
	/* A manual search's job has a key of its own. */
	"'" AXIS_MOVIES_SEARCH_KEY_PREFIX "' || s.movie_id::text || CASE WHEN s.cause = 'list' THEN ':list' ELSE '' END",
	"'" AXIS_MOVIES_SEARCH_KEY_PREFIX "' || g.movie_id::text",
	AXIS_MOVIES_SETTING_FLOOR,
	AXIS_MOVIES_SETTING_FLOOR_MAX,
	AXIS_MOVIES_SETTING_GIVE_UP,
	AXIS_MOVIES_SETTING_PROTOCOL,
	AXIS_MOVIES_SETTING_RETENTION,
	AXIS_MOVIES_SETTING_LANGUAGES,
	FALSE,
	load,
	read_disk,
	prepare,
	judge,
	NULL,
	NULL,
	claim_check,
	NULL,
	event,
	retry,
	follow_more,
	monitored,
	hand_preview,
	TRUE,	/* force: Grab it by force is offered */
	NULL,	/* unmatched: a film's name is read by itself alone */
	NULL	/* standing: a film has no parts; its own open grab stands in the way */
};

const AxisAcquireOwnerInfo *
axis_movies_acquire_info(void)
{
	return &acquire_info;
}

/* ── The job's work ─────────────────────────────────────────────── */

gboolean
axis_movies_run_search(
	AxisMovies             *self,
	gint64                  movie_id,
	const gchar            *cause,
	const gchar            *user,
	GCancellable           *cancellable,
	AxisMovieSearchReport  *report,
	GError                **error
){
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	return axis_acquire_run_search(self->acquire, &target, cause, user, cancellable, report, error);
}

/* ── Another profile, tried on what a search found ──────────────── */

gboolean
axis_movies_try_profiles(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *candidates,
	const GArray     *profile_ids,
	GPtrArray       **tries,
	GError          **error
){
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_try_profiles(self->acquire, &target, candidates, profile_ids, tries, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		}
		return FALSE;
	}
	return TRUE;
}

/* ── An administrator's grab ────────────────────────────────────── */

static gboolean
grab_failed(
	AxisAcquireGrabResult   result,
	const gchar            *problem,
	GError                **error
);

/* A replace of the download in flight that sent nothing: the layer's own
 * words (which download, why, and what happens next), as a conflict.  %TRUE
 * with *@error set when @result is one of those. */
static gboolean
replace_failed(
	AxisAcquireGrabResult           result,
	const gchar                    *problem,
	const AxisAcquireReplaceTicks  *asked,
	GError                        **error
){
	if (problem == NULL || asked == NULL || asked->grab_id <= 0)
		return FALSE;
	if (result != AXIS_ACQUIRE_GRAB_REPLACE_STUCK && result != AXIS_ACQUIRE_GRAB_IN_FLIGHT)
		return FALSE;
	g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "%s", problem);
	return TRUE;
}

gboolean
axis_movies_grab(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        candidate_id,
	const gchar  *user,
	gint64       *grab_id,
	GError      **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_grab(self->acquire, &target, candidate_id, user, grab_id, &result, &problem, error))
		return FALSE;
	return grab_failed(result, problem, error);
}

gboolean
axis_movies_grab_by_hand(
	AxisMovies                  *self,
	gint64                       movie_id,
	gint64                       candidate_id,
	const AxisAcquireHandTicks  *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result_out,
	GError                     **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_grab_by_hand(self->acquire, &target, candidate_id, user, ticks, grabbed, &result, &problem,
	        error))
		return FALSE;
	if (result_out != NULL)
		*result_out = result;
	if (replace_failed(result, problem, ticks != NULL ? ticks->in_flight : NULL, error))
		return FALSE;
	return grab_failed(result, problem, error);
}

gboolean
axis_movies_hand_preview(
	AxisMovies               *self,
	gint64                    movie_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
){
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_hand_preview(self->acquire, &target, candidate_id, preview, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that release is not one of the "
				"movie's searches'");
		}
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_movies_force_preview(
	AxisMovies               *self,
	gint64                    movie_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
){
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_force_preview(self->acquire, &target, candidate_id, preview, error)) {
		if (error != NULL && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
			g_clear_error(error);
			g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that release is not one of the "
				"movie's searches'");
		}
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_movies_grab_by_force(
	AxisMovies                   *self,
	gint64                        movie_id,
	gint64                        candidate_id,
	const AxisAcquireForceTicks  *ticks,
	const gchar                  *user,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result_out,
	GError                      **error
){
	g_autofree gchar *problem = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireTarget target;

	memset(&target, 0, sizeof(target));
	target.owner_id = movie_id;
	if (!axis_acquire_grab_by_force(self->acquire, &target, candidate_id, user, ticks, grabbed, &result, &problem,
	        error))
		return FALSE;
	if (result_out != NULL)
		*result_out = result;
	/* By force, a refusal is a rule that protects the system (or no fixed
	 * rule at all): its own words. */
	if (result == AXIS_ACQUIRE_GRAB_REFUSED) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_REFUSED, "this release cannot be grabbed by force: %s",
			problem != NULL ? problem : "a fixed rule that protects the system rules it out");
		return FALSE;
	}
	if (replace_failed(result, problem, ticks != NULL ? ticks->in_flight : NULL, error))
		return FALSE;
	return grab_failed(result, problem, error);
}

/* SENT: %TRUE; anything else in the film's words. */
static gboolean
grab_failed(
	AxisAcquireGrabResult   result,
	const gchar            *problem,
	GError                **error
){
	switch (result) {
	case AXIS_ACQUIRE_GRAB_SENT:
		return TRUE;
	case AXIS_ACQUIRE_GRAB_NOT_FOUND:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		break;
	case AXIS_ACQUIRE_GRAB_NO_CANDIDATE:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that release is not one of the "
			"movie's searches'");
		break;
	case AXIS_ACQUIRE_GRAB_REFUSED:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_REFUSED, "a fixed safety rule rejects this release, "
			"and no one may grab it: %s", problem != NULL ? problem : "it is not certainly this movie.");
		break;
	case AXIS_ACQUIRE_GRAB_BLOCKLISTED:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_BLOCKLISTED, "this release is on the movie's "
			"blocklist: take it off the blocklist first");
		break;
	case AXIS_ACQUIRE_GRAB_IN_FLIGHT:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "a grab of the movie is in flight "
			"already");
		break;
	case AXIS_ACQUIRE_GRAB_NEEDS_TICK:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_REFUSED, "%s", problem != NULL ? problem : "it needs "
			"a tick of its own. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_CHANGED:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_REFUSED, "%s", problem != NULL ? problem : "what "
			"its confirmation showed has changed. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_REPLACE_STUCK:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT, "%s", problem != NULL ? problem : "the "
			"download in flight could not be replaced. Nothing was sent");
		break;
	case AXIS_ACQUIRE_GRAB_OFFLINE:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_UNUSABLE, "the movie's destination is not "
			"online: nothing is sent until it is back");
		break;
	case AXIS_ACQUIRE_GRAB_EXPIRED:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_EXPIRED, "the search that found it has "
			"expired: search again");
		break;
	case AXIS_ACQUIRE_GRAB_GONE:
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "the movie was removed meanwhile");
		break;
	case AXIS_ACQUIRE_GRAB_NO_CLIENT:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_UNUSABLE, "%s", problem != NULL ? problem :
			"no usable download client takes it");
		break;
	default:
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_FAILED, "%s", problem != NULL ? problem :
			"it could not be sent");
		break;
	}
	return FALSE;
}
