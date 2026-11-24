/*
 * axis-movies-store.c - the movies module's SQL: THE facts query, history,
 * queueing a film's search, and a film's page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * axis_movies_store_facts() is the one statement a film's state is derived
 * from (axis_movie_derive()): the Films pages, the title page, the
 * dashboard, the scheduler and the grab-once check inside the claim
 * transaction all read it.  It reads the downloads layer's table (the open
 * grab's download, and the title's other downloads still at a client) and
 * the job queue's (the film's search job, by its de-duplication key); it
 * writes neither.  The blocklist, the stored searches and their candidates
 * are read and written through the acquisition layer.
 */

#include "modules/movies/axis-movies-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library-name.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

/* ── Small helpers ──────────────────────────────────────────────── */

gchar *
axis_movies_title_label(
	const gchar *title,
	gint         year
){
	g_autofree gchar *cut = axis_movies_text_cut(title, 500);

	if (year > 0)
		return g_strdup_printf("%s (%d)", cut != NULL ? cut : "Untitled", year);
	return g_strdup(cut != NULL ? cut : "Untitled");
}

static gchar *
get_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
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

static gboolean
get_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── The facts ──────────────────────────────────────────────────── */

enum
{
	F_ID, F_TITLE_ID, F_TITLE, F_ORIGINAL, F_YEAR, F_RELEASE, F_STATUS, F_RUNTIME, F_POSTER, F_PROVIDER,
	F_PROFILE_ID, F_PROFILE_NAME, F_PROFILE_SLUG, F_PROFILE_FORMAT, F_PROFILE_DEFINITION,
	F_ROOT_ID, F_ROOT_NAME, F_VOLUME_ID, F_VOLUME_NAME, F_VOLUME_STATE,
	F_MONITORED, F_AVAILABILITY, F_ADDED_BY, F_ADDED, F_LAST_SEARCH, F_NEXT_SEARCH, F_FLOOR,
	F_JOB,
	F_GRAB_ID, F_GRAB_RELEASE, F_GRAB_QUALITY, F_GRAB_MANUAL, F_GRAB_BY, F_GRAB_AT, F_GRAB_COMPLETED,
	F_DOWNLOAD_ID, F_D_STATE, F_D_SIZE, F_D_DONE, F_D_RATE, F_D_ETA, F_D_STALLED, F_D_PROGRESS, F_D_PROBLEM,
	F_D_MESSAGE, F_D_CLIENT, F_D_VOLUME, F_D_REL, F_D_CLIENT_PATH,
	F_OTHER_LIVE, F_FILES, F_FILE_QUALITIES,
	F_SEARCH_ID, F_SEARCH_AT, F_SEARCH_OUTCOME, F_SEARCH_TEXT, F_SEARCH_FOUND, F_SEARCH_ACCEPTED,
	F_IMPORT_ID, F_IMPORT_STATE, F_IMPORT_REASON, F_IMPORT_TEXT,
	F_OWN_IMPORT_ID, F_OWN_IMPORT_STATE, F_OWN_IMPORT_REASON, F_OWN_IMPORT_TEXT,
	F_SIZE, F_COPIES, F_KEPT_BY, F_HELD, F_SEARCH_HOLD, F_KEPT_BROUGHT
};

/* The downloads layer's states of a download still at its client. */
#define LIVE_STATES "('sending', 'queued', 'downloading', 'paused')"

static const gchar facts_select[] =
	"SELECT m.id, m.title_id, t.title, t.original_title, t.year, t.release_date::text, t.status,"
	" t.runtime_minutes, t.poster_ref, t.provider,"
	" m.quality_profile_id, qp.name, qp.slug, qp.format, qp.definition,"
	" m.root_id, lr.name, lr.volume_id, v.name, v.state,"
	" m.monitored, m.min_availability, m.added_by, " US("m.added_at") ", " US("m.last_search_at") ", "
	US("m.next_search_at") ", m.search_floor_s,"
	" sj.state,"
	" g.id, g.release_name, g.quality, g.manual, g.grabbed_by, " US("g.created_at") ", " US("g.completed_at") ","
	" g.download_id, d.state, d.size_bytes, d.done_bytes, d.rate_bps, d.eta_s, d.stalled,"
	" " US("coalesce(d.progress_at, d.sent_at, d.created_at)") ", d.problem, d.message, d.client_name, dv.name,"
	" d.rel_path, d.client_path,"
	" (SELECT count(*) FROM download od WHERE od.module = '" AXIS_MOVIES_MODULE_ID "' AND od.title_id = m.title_id"
	"   AND od.state IN " LIVE_STATES " AND od.id IS DISTINCT FROM g.download_id),"
	" (SELECT count(*) FROM movie_file f JOIN library_file lf ON lf.id = f.file_id"
	"   WHERE f.movie_id = m.id AND lf.state = 'present'),"
	" (SELECT string_agg(coalesce(f.quality, '') || ':' || coalesce(f.resolution, ''), ',') FROM movie_file f"
	"   JOIN library_file lf ON lf.id = f.file_id WHERE f.movie_id = m.id AND lf.state = 'present'"
	"   AND (f.quality IS NOT NULL OR f.resolution IS NOT NULL)),"
	" s.id, " US("s.finished_at") ", s.outcome, s.outcome_text, s.n_found, s.n_accepted,"
	" idl.id, idl.state, idl.reason, idl.reason_text, iow.id, iow.state, iow.reason, iow.reason_text,"
	" (SELECT coalesce(sum(lf.size), 0) FROM movie_file f JOIN library_file lf ON lf.id = f.file_id"
	"   WHERE f.movie_id = m.id AND lf.state = 'present'),"
	/* Its copies: a unit identification made of a folder or file each (an
	 * import's file not looked at yet is one of its own). */
	" (SELECT count(DISTINCT coalesce(cif.identity_id, f.identity_id, -f.file_id)) FROM movie_file f"
	"   JOIN library_file lf ON lf.id = f.file_id LEFT JOIN library_identity_file cif ON cif.file_id = f.file_id"
	"   WHERE f.movie_id = m.id AND lf.state = 'present'),"
	/* Who keeps a present file of it by hand (0062_force_grab.sql). */
	" (SELECT min(coalesce(f.hand_by, 'a person')) FROM movie_file f JOIN library_file lf ON lf.id = f.file_id"
	"   WHERE f.movie_id = m.id AND lf.state = 'present' AND f.kept),"
	/* A grab by force let go (not_imported) whose download is still held for
	 * a person: its import is blocked.  Its release name. */
	" (SELECT hg.release_name FROM movie_grab hg JOIN import hi ON hi.download_id = hg.download_id"
	"   WHERE hg.movie_id = m.id AND hg.bound AND hg.close_reason = 'not_imported' AND hi.state = 'blocked'"
	"   ORDER BY hg.id DESC LIMIT 1),"
	/* Why no automatic search runs for it (0068_arr_brought.sql). */
	" m.search_hold,"
	/* Every kept present file of it is kept because it was migrated, by
	 * no person's choice (0073_arr_brought_kept.sql).  Both marks are asked:
	 * a link a person made by hand since has another hand_how. */
	" (SELECT coalesce(bool_and(f.kept_how IS NOT DISTINCT FROM '" AXIS_MOVIE_KEPT_HOW_BROUGHT "'"
	"   AND f.hand_how IS NOT DISTINCT FROM 'brought'), false)"
	"   FROM movie_file f JOIN library_file lf ON lf.id = f.file_id"
	"   WHERE f.movie_id = m.id AND lf.state = 'present' AND f.kept)"
	" FROM movie m"
	" JOIN meta_title t ON t.id = m.title_id"
	" JOIN quality_profile qp ON qp.id = m.quality_profile_id"
	" LEFT JOIN library_root lr ON lr.id = m.root_id"
	" LEFT JOIN volume v ON v.id = lr.volume_id"
	" LEFT JOIN LATERAL (SELECT * FROM movie_grab gg WHERE gg.movie_id = m.id AND gg.closed_at IS NULL"
	"   ORDER BY gg.id DESC LIMIT 1) g ON true"
	" LEFT JOIN download d ON d.id = g.download_id"
	" LEFT JOIN volume dv ON dv.id = d.volume_id"
	/* The last automatic search: a manual one (cause "list") is a person's
	 * look, and changes no state. */
	" LEFT JOIN LATERAL (SELECT * FROM movie_search ss WHERE ss.movie_id = m.id AND ss.finished_at IS NOT NULL"
	"   AND ss.cause <> 'list' ORDER BY ss.id DESC LIMIT 1) s ON true"
	/* The import of the open grab's download, and the film's own latest. */
	" LEFT JOIN LATERAL (SELECT ii.id, ii.state, ii.reason, ii.reason_text FROM import ii"
	"   WHERE g.download_id IS NOT NULL AND ii.download_id = g.download_id ORDER BY ii.id DESC LIMIT 1) idl ON true"
	" LEFT JOIN LATERAL (SELECT ii.id, ii.state, ii.reason, ii.reason_text FROM import ii"
	"   WHERE ii.module = '" AXIS_MOVIES_MODULE_ID "' AND ii.owner_id = m.id ORDER BY ii.id DESC LIMIT 1) iow ON true"
	/*
	 * The searches queued or running, read once and joined (a key has one
	 * such job at most: job_dedupe_key_idx).  Looked up once a film, the
	 * planner chose job_live_idx for it and read every job that waits or
	 * runs for each film: 0.06 ms a film with 300 such jobs, 1.2 s of the
	 * scheduler's statement for 20,000 films.
	 */
	" LEFT JOIN (SELECT jj.dedupe_key, min(jj.state::text) AS state FROM job jj WHERE jj.dedupe_key IS NOT NULL"
	"   AND jj.state IN ('queued', 'running') AND jj.dedupe_key LIKE '" AXIS_MOVIES_SEARCH_KEY_PREFIX "%'"
	"   GROUP BY jj.dedupe_key) sj ON sj.dedupe_key = '" AXIS_MOVIES_SEARCH_KEY_PREFIX "' || m.id::text";

static const gchar facts_order[] = " ORDER BY lower(t.title), t.year, m.id";

typedef struct
{
	gboolean    usable;
	AxisQuality cutoff;
	gboolean    upgrades;
	gboolean    upgrade_unknown;
} ProfileFacts;

gchar *
axis_movies_usable_states(void)
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

/* The best of the files' "quality:resolution" pairs: a ladder quality, or an
 * unknown source at a resolution (axis_quality_compare_on_disk()). */
void
axis_movies_best_on_disk(
	const gchar           *list,
	AxisQuality           *quality,
	AxisReleaseResolution *resolution
){
	g_auto(GStrv) pairs = list != NULL ? g_strsplit(list, ",", 64) : NULL;
	guint i;

	*quality = AXIS_QUALITY_UNKNOWN;
	*resolution = AXIS_RELEASE_RESOLUTION_NONE;
	for (i = 0; pairs != NULL && pairs[i] != NULL; i++) {
		gchar *colon = strchr(pairs[i], ':');
		AxisQuality q = AXIS_QUALITY_UNKNOWN;
		AxisReleaseResolution r = AXIS_RELEASE_RESOLUTION_NONE;

		if (colon != NULL) {
			*colon = '\0';
			r = axis_identify_resolution_from_name(colon + 1);
		}
		if (pairs[i][0] != '\0' && axis_quality_from_name(pairs[i], &q))
			r = AXIS_RELEASE_RESOLUTION_NONE;
		else
			q = AXIS_QUALITY_UNKNOWN;
		if (axis_quality_compare_on_disk(q, r, *quality, *resolution) > 0) {
			*quality = q;
			*resolution = r;
		}
	}
}

static AxisMovieFacts *
facts_from_row(
	AxisDbResult *result,
	guint         row,
	GHashTable   *profiles
){
	AxisMovieFacts *f = g_new0(AxisMovieFacts, 1);
	const gchar *text;
	ProfileFacts *profile;
	gint64 profile_id;

	f->id = get_int(result, row, F_ID, 0);
	f->title_id = get_int(result, row, F_TITLE_ID, 0);
	f->title = get_text(result, row, F_TITLE);
	f->original_title = get_text(result, row, F_ORIGINAL);
	f->year = (gint)get_int(result, row, F_YEAR, 0);
	f->release_date = get_text(result, row, F_RELEASE);
	f->status = axis_meta_status_from_string(axis_db_result_get_text(result, row, F_STATUS));
	f->runtime = (gint)get_int(result, row, F_RUNTIME, 0);
	f->poster_ref = get_text(result, row, F_POSTER);
	f->provider = get_text(result, row, F_PROVIDER);

	profile_id = get_int(result, row, F_PROFILE_ID, 0);
	f->profile_id = profile_id;
	f->profile_name = get_text(result, row, F_PROFILE_NAME);
	f->profile_slug = get_text(result, row, F_PROFILE_SLUG);
	profile = g_hash_table_lookup(profiles, &profile_id);
	if (profile == NULL) {
		AxisQualityProfile parsed;
		g_autofree gchar *unusable = NULL;
		gint64 *key = g_new(gint64, 1);

		*key = profile_id;
		profile = g_new0(ProfileFacts, 1);
		memset(&parsed, 0, sizeof(parsed));
		profile->usable = axis_quality_stored_check((gint)get_int(result, row, F_PROFILE_FORMAT, 0),
			axis_db_result_get_text(result, row, F_PROFILE_DEFINITION), &parsed, &unusable);
		profile->cutoff = profile->usable ? parsed.cutoff : AXIS_QUALITY_UNKNOWN;
		profile->upgrades = profile->usable && parsed.upgrades;
		profile->upgrade_unknown = profile->usable && parsed.upgrade_unknown;
		g_hash_table_insert(profiles, key, profile);
	}
	f->profile_usable = profile->usable;
	f->profile_cutoff = profile->cutoff;
	f->profile_upgrades = profile->upgrades;
	f->profile_upgrade_unknown = profile->upgrade_unknown;

	f->root_id = get_int(result, row, F_ROOT_ID, 0);
	f->root_name = get_text(result, row, F_ROOT_NAME);
	f->volume_id = get_int(result, row, F_VOLUME_ID, 0);
	f->volume_name = get_text(result, row, F_VOLUME_NAME);
	f->volume_state = AXIS_VOLUME_OFFLINE;
	text = axis_db_result_get_text(result, row, F_VOLUME_STATE);
	if (text != NULL)
		axis_volume_state_from_string(text, &f->volume_state);

	f->monitored = get_bool(result, row, F_MONITORED);
	f->availability = AXIS_MOVIE_RELEASED;
	axis_movie_availability_from_name(axis_db_result_get_text(result, row, F_AVAILABILITY), &f->availability);
	f->added_by = get_text(result, row, F_ADDED_BY);
	f->added_us = get_int(result, row, F_ADDED, 0);
	f->last_search_us = get_int(result, row, F_LAST_SEARCH, 0);
	f->next_search_us = get_int(result, row, F_NEXT_SEARCH, 0);
	f->floor_s = get_int(result, row, F_FLOOR, 21600);

	text = axis_db_result_get_text(result, row, F_JOB);
	f->search_running = g_strcmp0(text, "running") == 0;
	f->search_queued = g_strcmp0(text, "queued") == 0;

	f->grab_id = get_int(result, row, F_GRAB_ID, 0);
	f->grab_release = get_text(result, row, F_GRAB_RELEASE);
	f->grab_quality = get_text(result, row, F_GRAB_QUALITY);
	f->grab_manual = get_bool(result, row, F_GRAB_MANUAL);
	f->grab_by = get_text(result, row, F_GRAB_BY);
	f->grab_us = get_int(result, row, F_GRAB_AT, 0);
	f->grab_completed_us = get_int(result, row, F_GRAB_COMPLETED, 0);
	f->download_id = get_int(result, row, F_DOWNLOAD_ID, 0);
	text = axis_db_result_get_text(result, row, F_D_STATE);
	f->download_known = text != NULL && axis_download_state_from_string(text, &f->download_state);
	f->download_size = get_int(result, row, F_D_SIZE, -1);
	f->download_done = get_int(result, row, F_D_DONE, -1);
	f->download_rate = get_int(result, row, F_D_RATE, -1);
	f->download_eta = get_int(result, row, F_D_ETA, -1);
	f->download_stalled = get_bool(result, row, F_D_STALLED);
	f->download_progress_us = get_int(result, row, F_D_PROGRESS, 0);
	f->download_problem = get_text(result, row, F_D_PROBLEM);
	f->download_message = get_text(result, row, F_D_MESSAGE);
	f->client_name = get_text(result, row, F_D_CLIENT);
	f->download_volume = get_text(result, row, F_D_VOLUME);
	f->download_rel_path = get_text(result, row, F_D_REL);
	f->download_client_path = get_text(result, row, F_D_CLIENT_PATH);

	f->other_live = (guint)get_int(result, row, F_OTHER_LIVE, 0);
	f->n_files = (guint)get_int(result, row, F_FILES, 0);
	f->size_bytes = get_int(result, row, F_SIZE, 0);
	f->n_copies = (guint)get_int(result, row, F_COPIES, 0);
	f->kept_by = get_text(result, row, F_KEPT_BY);
	f->held_forced = get_text(result, row, F_HELD);
	f->search_hold = get_text(result, row, F_SEARCH_HOLD);
	f->kept_brought = f->kept_by != NULL && get_bool(result, row, F_KEPT_BROUGHT);
	axis_movies_best_on_disk(axis_db_result_get_text(result, row, F_FILE_QUALITIES), &f->file_quality,
		&f->file_resolution);

	f->search_id = get_int(result, row, F_SEARCH_ID, 0);
	f->search_us = get_int(result, row, F_SEARCH_AT, 0);
	f->search_outcome = get_text(result, row, F_SEARCH_OUTCOME);
	f->search_text = get_text(result, row, F_SEARCH_TEXT);
	f->search_found = (guint)get_int(result, row, F_SEARCH_FOUND, 0);
	f->search_accepted = (guint)get_int(result, row, F_SEARCH_ACCEPTED, 0);
	/* The open grab's import when there is one; else the film's own (a
	 * manual import). */
	if (!axis_db_result_is_null(result, row, F_IMPORT_ID)) {
		f->import_id = get_int(result, row, F_IMPORT_ID, 0);
		f->import_state = get_text(result, row, F_IMPORT_STATE);
		f->import_reason = get_text(result, row, F_IMPORT_REASON);
		f->import_text = get_text(result, row, F_IMPORT_TEXT);
	} else if (!axis_db_result_is_null(result, row, F_OWN_IMPORT_ID)) {
		f->import_id = get_int(result, row, F_OWN_IMPORT_ID, 0);
		f->import_state = get_text(result, row, F_OWN_IMPORT_STATE);
		f->import_reason = get_text(result, row, F_OWN_IMPORT_REASON);
		f->import_text = get_text(result, row, F_OWN_IMPORT_TEXT);
	}
	return f;
}

gboolean
axis_movies_store_facts(
	AxisDbConn             *conn,
	AxisMoviesFactsFilter   filter,
	const gchar            *param,
	GPtrArray             **films,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) profiles = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	g_autofree gchar *sql = NULL;
	const gchar *params[1];
	guint n_params = 0;
	guint i;

	switch (filter) {
	case AXIS_MOVIES_FACTS_ONE:
		sql = g_strconcat(facts_select, " WHERE m.id = $1::bigint", facts_order, NULL);
		break;
	case AXIS_MOVIES_FACTS_TITLES:
		sql = g_strconcat(facts_select, " WHERE m.title_id = ANY ($1::bigint[])", facts_order, NULL);
		break;
	case AXIS_MOVIES_FACTS_MONITORED:
		sql = g_strconcat(facts_select, " WHERE m.monitored", facts_order, NULL);
		break;
	case AXIS_MOVIES_FACTS_IDS:
		sql = g_strconcat(facts_select, " WHERE m.id = ANY ($1::bigint[])", facts_order, NULL);
		break;
	default:
		sql = g_strconcat(facts_select, facts_order, NULL);
		break;
	}
	if (filter == AXIS_MOVIES_FACTS_ONE || filter == AXIS_MOVIES_FACTS_TITLES || filter == AXIS_MOVIES_FACTS_IDS) {
		params[0] = param;
		n_params = 1;
	}
	if (!axis_db_conn_exec(conn, sql, n_params, params, &result, error))
		return FALSE;
	*films = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_facts_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*films, facts_from_row(result, i, profiles));
	return TRUE;
}

gboolean
axis_movies_store_facts_one(
	AxisDbConn      *conn,
	gint64           movie_id,
	AxisMovieFacts **facts,
	GError         **error
){
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autoptr(GPtrArray) films = NULL;

	*facts = NULL;
	if (!axis_movies_store_facts(conn, AXIS_MOVIES_FACTS_ONE, id, &films, error))
		return FALSE;
	if (films->len > 0)
		*facts = g_ptr_array_steal_index(films, 0);
	return TRUE;
}

gboolean
axis_movies_store_lock(
	AxisDbConn  *conn,
	gint64       movie_id,
	gboolean    *found,
	GError     **error
){
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT id FROM movie WHERE id = $1::bigint FOR UPDATE", 1, params, &result,
	        error))
		return FALSE;
	*found = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

/* ── History ────────────────────────────────────────────────────── */

gboolean
axis_movies_store_event(
	AxisDbConn   *conn,
	gint64        movie_id,
	gint64        title_id,
	const gchar  *title_label,
	const gchar  *kind,
	const gchar  *text,
	const gchar  *user,
	gint64        search_id,
	gint64        grab_id,
	GError      **error
){
	g_autofree gchar *movie = movie_id > 0 ? axis_movies_int_text(movie_id) : NULL;
	g_autofree gchar *title = title_id > 0 ? axis_movies_int_text(title_id) : NULL;
	g_autofree gchar *search = search_id > 0 ? axis_movies_int_text(search_id) : NULL;
	g_autofree gchar *grab = grab_id > 0 ? axis_movies_int_text(grab_id) : NULL;
	g_autofree gchar *label = axis_movies_text_cut(title_label, 600);
	g_autofree gchar *body = axis_movies_text_cut(text, 4000);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	const gchar *params[8];

	params[0] = movie;
	params[1] = title;
	params[2] = label != NULL ? label : "Untitled";
	params[3] = kind;
	params[4] = body != NULL ? body : kind;
	params[5] = by;
	params[6] = search;
	params[7] = grab;
	return axis_db_conn_exec(conn, "INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, by_user,"
		" search_id, grab_id) VALUES ($1::bigint, $2::bigint, $3, $4, $5, $6, $7::bigint, $8::bigint)", 8, params,
		NULL, error);
}

gboolean
axis_movies_store_eventf(
	AxisDbConn           *conn,
	const AxisMovieFacts *facts,
	const gchar          *kind,
	const gchar          *user,
	gint64                search_id,
	gint64                grab_id,
	GError              **error,
	const gchar          *format,
	...
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *label = axis_movies_title_label(facts->title, facts->year);
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_movies_store_event(conn, facts->id, facts->title_id, label, kind, text, user, search_id, grab_id,
		error);
}

/* ── The search job ─────────────────────────────────────────────── */

gboolean
axis_movies_store_enqueue_search(
	AxisDbConn   *conn,
	gint64        movie_id,
	const gchar  *cause,
	const gchar  *user,
	gint64       *job_id,
	gboolean     *created,
	GError      **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	/* A manual search's job is never merged with an automatic one. */
	g_autofree gchar *key = g_strdup_printf(AXIS_MOVIES_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT "%s", movie_id,
		g_strcmp0(cause, "list") == 0 ? ":list" : "");
	gboolean made = FALSE;
	AxisJobSpec spec;

	json_object_set_int_member(payload, "movie", movie_id);
	json_object_set_string_member(payload, "cause", cause);
	if (user != NULL)
		json_object_set_string_member(payload, "user", user);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_MOVIES_SEARCH_KIND;
	spec.queue = AXIS_ACQUIRE_SEARCH_QUEUE;
	spec.payload = payload;
	spec.priority = g_strcmp0(cause, "scheduled") == 0 ? 0 : 10;
	spec.dedupe_key = key;
	spec.max_attempts = 2;
	if (!axis_job_enqueue(conn, &spec, job_id, &made, error))
		return FALSE;
	if (created != NULL)
		*created = made;
	/* The same search queued already, by the scheduler (a click between a
	 * look and this): raised to this one's priority, not left behind the
	 * scheduler's backlog. */
	if (!made && spec.priority > 0) {
		g_autofree gchar *priority = g_strdup_printf("%d", spec.priority);
		const gchar *params[2];

		params[0] = key;
		params[1] = priority;
		return axis_db_conn_exec(conn, "UPDATE job SET priority = $2::integer WHERE dedupe_key = $1"
			" AND state = 'queued' AND priority < $2::integer", 2, params, NULL, error);
	}
	return TRUE;
}

/* ── The film's page and the history ────────────────────────────── */

void
axis_movie_candidate_free(AxisMovieCandidate *candidate)
{
	axis_acquire_candidate_free(candidate);
}

void
axis_movie_block_free(AxisMovieBlock *block)
{
	axis_acquire_block_free(block);
}

void
axis_movie_event_free(AxisMovieEvent *event)
{
	if (event == NULL)
		return;
	g_free(event->title_label);
	g_free(event->kind);
	g_free(event->text);
	g_free(event->by_user);
	g_free(event);
}

void
axis_movie_search_info_free(AxisMovieSearchInfo *search)
{
	axis_acquire_search_info_free(search);
}

void
axis_movie_detail_free(AxisMovieDetail *detail)
{
	if (detail == NULL)
		return;
	axis_movie_facts_free(detail->facts);
	axis_movie_search_info_free(detail->search);
	g_clear_pointer(&detail->candidates, g_ptr_array_unref);
	g_clear_pointer(&detail->blocklist, g_ptr_array_unref);
	g_clear_pointer(&detail->events, g_ptr_array_unref);
	g_clear_pointer(&detail->files, g_ptr_array_unref);
	g_free(detail);
}

void
axis_movie_file_free(AxisMovieFile *file)
{
	if (file == NULL)
		return;
	g_free(file->root_name);
	g_free(file->volume_name);
	g_free(file->rel_path);
	g_free(file->state);
	g_free(file->quality);
	g_free(file->quality_text);
	g_free(file->mode);
	g_free(file->resolution);
	g_free(file->identity_how);
	g_free(file->hand_how);
	g_free(file->hand_by);
	g_free(file);
}

static AxisMovieEvent *
event_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisMovieEvent *event = g_new0(AxisMovieEvent, 1);

	event->id = get_int(result, row, 0, 0);
	event->movie_id = get_int(result, row, 1, 0);
	event->title_id = get_int(result, row, 2, 0);
	event->title_label = get_text(result, row, 3);
	event->kind = get_text(result, row, 4);
	event->text = get_text(result, row, 5);
	event->by_user = get_text(result, row, 6);
	event->created_us = get_int(result, row, 7, 0);
	return event;
}

#define EVENT_COLUMNS "id, movie_id, title_id, title_label, kind, text, by_user, " US("created_at")

gboolean
axis_movies_get_detail(
	AxisMovies       *self,
	gint64            movie_id,
	guint             n_events,
	AxisMovieDetail **detail_out,
	GError          **error
){
	g_autoptr(AxisMovieDetail) detail = g_new0(AxisMovieDetail, 1);
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *limit = axis_movies_int_text(MAX(n_events, 1));
	g_autofree gchar *usable = axis_movies_usable_states();
	g_autoptr(AxisDbResult) rows = NULL;
	AxisDbConn *conn;
	const gchar *params[2];
	gboolean ok;
	guint i;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_facts_one(conn, movie_id, &detail->facts, error);
	if (ok && detail->facts == NULL) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		ok = FALSE;
	}
	/* Its latest search with every release it considered, and its
	 * blocklist. */
	ok = ok && axis_acquire_store_search(self->acquire, conn, movie_id, 0, &detail->search, error);
	if (ok && detail->search != NULL)
		ok = axis_acquire_store_candidates(self->acquire, conn, detail->search->id, &detail->candidates, error);
	if (detail->candidates == NULL)
		detail->candidates = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_candidate_free);
	ok = ok && axis_acquire_store_blocklist(self->acquire, conn, movie_id, &detail->blocklist, error);
	if (detail->blocklist == NULL)
		detail->blocklist = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_block_free);
	params[0] = movie;
	params[1] = limit;
	ok = ok && axis_db_conn_exec(conn, "SELECT " EVENT_COLUMNS " FROM movie_event WHERE movie_id = $1::bigint"
		" ORDER BY id DESC LIMIT $2::integer", 2, params, &rows, error);
	detail->events = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_event_free);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(detail->events, event_from_row(rows, i));
	g_clear_pointer(&rows, axis_db_result_free);
	/* Whether a person has looked by hand (its page is linked then). */
	ok = ok && axis_db_conn_exec(conn, "SELECT 1 FROM movie_search WHERE movie_id = $1::bigint AND cause = 'list'"
		" LIMIT 1", 1, params, &rows, error);
	if (ok)
		detail->manual = axis_db_result_get_n_rows(rows) > 0;
	g_clear_pointer(&rows, axis_db_result_free);
	/* Its files in the library, and how each came. */
	params[0] = movie;
	params[1] = usable;
	ok = ok && axis_db_conn_exec(conn, "SELECT lf.id, r.name, v.name, lf.rel_path, lf.size, lf.state, mf.quality,"
		" mf.quality_text, lf.pinned, mf.import_id, i.mode, " US("mf.linked_at") ", mf.resolution, mf.identity_id,"
		" li.linked_how, coalesce(lif.best, false), NOT coalesce(v.state = ANY ($2::text[]), false), mf.hand_how,"
		" mf.hand_by, mf.kept,"
		" EXISTS (SELECT 1 FROM movie_grab xg WHERE xg.download_id = i.download_id AND xg.closed_at IS NULL),"
		" coalesce(mf.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "' AND mf.hand_how = 'brought', false)"
		" FROM movie_file mf"
		" JOIN library_file lf ON lf.id = mf.file_id JOIN library_root r ON r.id = lf.root_id"
		" JOIN volume v ON v.id = lf.volume_id LEFT JOIN import i ON i.id = mf.import_id"
		" LEFT JOIN library_identity li ON li.id = mf.identity_id"
		" LEFT JOIN library_identity_file lif ON lif.file_id = mf.file_id AND lif.identity_id = mf.identity_id"
		" WHERE mf.movie_id = $1::bigint ORDER BY lf.state = 'present' DESC, coalesce(lif.best, false) DESC,"
		" mf.linked_at DESC LIMIT 20", 2, params, &rows, error);
	detail->files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_file_free);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		AxisMovieFile *file = axis_movies_file_from_row(rows, i);

		/* Made by a person, and kept by hand (a managed movie's link alone). */
		file->hand_how = get_text(rows, i, 17);
		file->hand_by = get_text(rows, i, 18);
		file->kept = get_bool(rows, i, 19);
		file->grab_open = get_bool(rows, i, 20);
		file->kept_brought = file->kept && get_bool(rows, i, 21);
		g_ptr_array_add(detail->files, file);
	}
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*detail_out = g_steal_pointer(&detail);
	return TRUE;
}

gboolean
axis_movies_set_kept(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        file_id,
	gboolean      kept,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *movie = axis_movies_int_text(movie_id);
	g_autofree gchar *file = axis_movies_int_text(file_id);
	g_autofree gchar *by = axis_movies_text_cut(user, 200);
	g_autofree gchar *label = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *text = NULL;
	const gchar *params[4];
	AxisDbConn *conn;
	gint64 title_id = 0;
	gint64 year = 0;
	gboolean ok;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = movie;
	params[1] = file;
	params[2] = kept ? "true" : "false";
	params[3] = by;
	/* A file kept by a person who made no link keeps who linked it (its
	 * hand_how stays NULL): only the keeping is theirs, said in the history.
	 * So is the keeping, or the letting go, of a file that was kept because it
	 * was migrated: kept_how goes, and "Let Axis upgrade these files" on
	 * its connection's page never touches it again. */
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn, "UPDATE movie_file mf SET kept = $3::boolean, kept_how = NULL,"
			" hand_by = CASE WHEN $3::boolean"
			" THEN coalesce($4::text, mf.hand_by) ELSE mf.hand_by END FROM movie m JOIN meta_title t ON t.id = m.title_id,"
			/* (How it was kept until now, for the history's words.) */
			" library_file lf, (SELECT o.file_id, o.kept_how FROM movie_file o WHERE o.file_id = $2::bigint) was"
			" WHERE mf.movie_id = $1::bigint AND mf.file_id = $2::bigint AND m.id = mf.movie_id"
			" AND lf.id = mf.file_id AND was.file_id = mf.file_id RETURNING m.title_id, t.title, t.year, lf.base_name,"
			" coalesce(was.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "', false)", 4, params, &row, error);
	if (ok && axis_db_result_get_n_rows(row) == 0) {
		g_set_error_literal(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "that file is not this movie's");
		ok = FALSE;
	}
	if (ok) {
		axis_db_result_get_int64(row, 0, 0, &title_id);
		axis_db_result_get_int64(row, 0, 2, &year);
		label = axis_movies_title_label(axis_db_result_get_text(row, 0, 1), (gint)year);
		shown = axis_db_result_get_text(row, 0, 3) != NULL ?
			axis_library_name_display(axis_db_result_get_text(row, 0, 3)) : g_strdup("its file");
		text = kept ? g_strdup_printf("The file %s is kept by hand%s%s: no automatic search and no Search now "
				"replaces it until a person lets it go.", shown, by != NULL ? " by " : "", by != NULL ? by : "") :
			get_bool(row, 0, 4) ? g_strdup_printf("The file %s, kept since it was migrated, is let go%s%s: it is "
				"upgraded by the ordinary rules again.", shown, by != NULL ? " by " : "", by != NULL ? by : "") :
			g_strdup_printf("The file %s is no longer kept by hand%s%s: it is upgraded by the ordinary rules again.",
				shown, by != NULL ? ", let go by " : "", by != NULL ? by : "");
		ok = axis_movies_store_event(conn, movie_id, title_id, label, "hand_link", text, by, 0, 0, error) &&
			axis_db_conn_commit(conn, error);
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

AxisMovieFile *
axis_movies_file_from_row(
	AxisDbResult *rows,
	guint         i
){
	AxisMovieFile *file = g_new0(AxisMovieFile, 1);

	file->file_id = get_int(rows, i, 0, 0);
	file->root_name = get_text(rows, i, 1);
	file->volume_name = get_text(rows, i, 2);
	file->rel_path = get_text(rows, i, 3);
	file->size = get_int(rows, i, 4, 0);
	file->state = get_text(rows, i, 5);
	file->quality = get_text(rows, i, 6);
	file->quality_text = get_text(rows, i, 7);
	file->pinned = get_bool(rows, i, 8);
	file->import_id = get_int(rows, i, 9, 0);
	file->mode = get_text(rows, i, 10);
	file->linked_us = get_int(rows, i, 11, 0);
	file->resolution = get_text(rows, i, 12);
	file->identity_id = get_int(rows, i, 13, 0);
	file->identity_how = get_text(rows, i, 14);
	file->best = get_bool(rows, i, 15);
	file->away = get_bool(rows, i, 16);
	return file;
}

gboolean
axis_movies_store_title_events(
	AxisDbConn   *conn,
	gint64        title_id,
	guint         limit,
	GPtrArray   **events,
	GError      **error
){
	g_autofree gchar *title = axis_movies_int_text(title_id);
	g_autofree gchar *count = axis_movies_int_text(MAX(limit, 1));
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[2];
	guint i;

	params[0] = title;
	params[1] = count;
	if (!axis_db_conn_exec(conn, "SELECT " EVENT_COLUMNS " FROM movie_event WHERE title_id = $1::bigint"
	        " AND movie_id IS NULL ORDER BY id DESC LIMIT $2::integer", 2, params, &rows, error))
		return FALSE;
	*events = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_event_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(*events, event_from_row(rows, i));
	return TRUE;
}

gboolean
axis_movies_list_events(
	AxisMovies  *self,
	gint64       before_id,
	guint        limit,
	GPtrArray  **events,
	GError     **error
){
	g_autofree gchar *before = axis_movies_int_text(before_id > 0 ? before_id : G_MAXINT64);
	g_autofree gchar *count = axis_movies_int_text(CLAMP(limit, 1, 500));
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[2];
	guint i;

	params[0] = before;
	params[1] = count;
	if (!axis_db_pool_exec(self->pool, "SELECT " EVENT_COLUMNS " FROM movie_event WHERE id < $1::bigint"
	        " ORDER BY id DESC LIMIT $2::integer", 2, params, &rows, error))
		return FALSE;
	*events = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_event_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++)
		g_ptr_array_add(*events, event_from_row(rows, i));
	return TRUE;
}

/* ── Reading through the pool ───────────────────────────────────── */

static gboolean
facts_through_pool(
	AxisMovies             *self,
	AxisMoviesFactsFilter   filter,
	const gchar            *param,
	GPtrArray             **films,
	GError                **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_facts(conn, filter, param, films, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_movies_list(
	AxisMovies  *self,
	GPtrArray  **films,
	GError     **error
){
	return facts_through_pool(self, AXIS_MOVIES_FACTS_ALL, NULL, films, error);
}

gboolean
axis_movies_get(
	AxisMovies      *self,
	gint64           movie_id,
	AxisMovieFacts **facts,
	GError         **error
){
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autoptr(GPtrArray) films = NULL;

	if (!facts_through_pool(self, AXIS_MOVIES_FACTS_ONE, id, &films, error))
		return FALSE;
	if (films->len == 0) {
		g_set_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND, "there is no such movie");
		return FALSE;
	}
	*facts = g_ptr_array_steal_index(films, 0);
	return TRUE;
}

gboolean
axis_movies_get_by_titles(
	AxisMovies  *self,
	GArray      *title_ids,
	GPtrArray  **films,
	GError     **error
){
	g_autoptr(GString) array = g_string_new("{");
	guint i;

	for (i = 0; i < title_ids->len; i++)
		g_string_append_printf(array, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", g_array_index(title_ids, gint64, i));
	g_string_append_c(array, '}');
	return facts_through_pool(self, AXIS_MOVIES_FACTS_TITLES, array->str, films, error);
}
