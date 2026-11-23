/*
 * axis-arr-apply.c - bringing over what a preview showed (the job
 * arr.apply), what a connection brought, and starting the searches of the
 * movies migrated
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/arr-migration/spec.md: the run that brings Radarr's movies over.
 *
 * The run decides nothing of its own: it takes what the preview decides
 * (axis_arr_preview_decide(), computed again when the job starts, under the
 * plan and the choices its confirmation was of) and hands each movie to the
 * movies module (axis_movies_bring()), one transaction a movie, with the row
 * of arr_brought that records it in the same transaction.  So:
 *
 *  - a movie is whole or not there, and a run that stops is continued by
 *    running again: what was brought is "Already in Axis" to the next look;
 *  - a second run brings only what is new, and for a movie this connection
 *    brought before does one thing only: it links the file Axis did not see
 *    then and holds now (axis_movies_bring_file()).  It reads and writes
 *    nothing else of a movie that is in Axis, so what a person changed since
 *    (monitoring, profile, folder, links, a file unlinked by hand) stands;
 *  - a movie this connection brought and a person removed is not brought
 *    again (the preview skips it);
 *  - the run asks Radarr nothing, and opens no file or folder on any disk.
 *
 * Starting the searches is the one place here that asks Radarr anything: its
 * download queue, by GET, to leave out what it is downloading.  A run whose
 * confirmation was ticked "Start searching for these as soon as they are
 * migrated" does that at its end, by the same code (start_at_end()); a
 * run without the tick asks Radarr nothing.
 *
 * A file migrated is kept (Axis does not replace it by itself); "Let Axis
 * upgrade these files" lets go of a connection's, here (axis_arr_release_kept()).
 */

#include "arr/axis-arr-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"

/* The movies left out of a start that the page names. */
#define NAMED_IN_QUEUE 50

gboolean (*axis_arr_apply_hook)(guint index, gpointer data, GError **error) = NULL;
gpointer axis_arr_apply_hook_data = NULL;

static gchar *
origin_of(const AxisArrSource *source)
{
	return g_strdup_printf("%s (%s)", axis_arr_app_label(source->app), source->name);
}

/* The files of what connection $1 brought that are kept because they were
 * migrated, and by no person's choice since: what "Let Axis upgrade these
 * files" lets go.  @b names the row of arr_brought. */
#define KEPT_MOVIE_FILES(b) \
	" FROM arr_brought " b " JOIN movie_file mf ON mf.movie_id = " b ".movie_id" \
	" JOIN library_file klf ON klf.id = mf.file_id AND klf.state = 'present' WHERE " b ".source_id = $1::bigint" \
	" AND mf.hand_how = 'brought' AND mf.kept AND mf.kept_how = '" AXIS_MOVIE_KEPT_HOW_BROUGHT "'"
#define KEPT_SERIES_FILES(b) \
	" FROM arr_brought " b " JOIN series_file sf ON sf.series_id = " b ".series_id" \
	" JOIN library_file klf ON klf.id = sf.file_id AND klf.state = 'present' WHERE " b ".source_id = $1::bigint" \
	" AND sf.hand_how = 'brought' AND sf.kept AND sf.kept_how = '" AXIS_TV_KEPT_HOW_BROUGHT "'"

/* A run's own start of the searches, with nobody at the page (the tick of
 * its confirmation). */
typedef struct
{
	const gchar *job;	/* the run's job: only its titles are started */
	const gchar *tail;	/* (nullable) what the run could not bring, for the note */
} Unattended;

static gboolean
set_run_note(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *note,
	GError      **error
);

static gboolean
start_searching(
	AxisArr           *self,
	gint64             source_id,
	gboolean           stopped,
	const Unattended  *unattended,
	const gchar       *user,
	AxisArrStart     **out,
	GError           **error
);

/* ── Asking for a run ───────────────────────────────────────────── */

gchar *
axis_arr_run_digest(
	const gchar *digest,
	gboolean     start
){
	g_autofree gchar *text = NULL;

	if (digest == NULL)
		return NULL;
	if (!start)
		return g_strdup(digest);
	text = g_strdup_printf("%s|start-searching-at-the-end", digest);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

gboolean
axis_arr_request_apply(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *digest,
	const gchar  *start,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *key = g_strdup_printf(AXIS_ARR_APPLY_KEY_PREFIX "%" G_GINT64_FORMAT, source_id);
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean created = FALSE;
	gboolean start_ticked = FALSE;
	gboolean ok;

	g_autoptr(AxisArrSource) source = NULL;

	if (!axis_arr_get_source(self, source_id, &source, error))
		return FALSE;
	/* A run brings a Radarr's movies through the Movies module and a Sonarr's
	 * series through the TV module: the connection's app decides, here and
	 * again when the job starts, and nothing a post carries does. */
	if (source->app == AXIS_ARR_RADARR && self->movies == NULL) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, "Movies cannot be migrated: the Movies "
			"module is not there.");
		return FALSE;
	}
	if (source->app == AXIS_ARR_SONARR && self->tv == NULL) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, "Series cannot be migrated: the TV "
			"module is not there. Nothing was migrated.");
		return FALSE;
	}
	if (source->app != AXIS_ARR_RADARR && source->app != AXIS_ARR_SONARR) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "Nothing can be migrated from this app.");
		return FALSE;
	}
	/* A series is added under TheTVDB's aired order, which Axis can follow
	 * only while it has a TheTVDB key. */
	if (source->app == AXIS_ARR_SONARR && !axis_arr_series_ready(self)) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY, "Sonarr numbers its series as TheTVDB "
			"does, and Axis has no TheTVDB key now. Nothing was migrated.");
		return FALSE;
	}
	if (!axis_arr_preview(self, source_id, AXIS_ARR_SKIP_NONE, 0, 0, &preview, error))
		return FALSE;
	/* The page that would be drawn now is not the one that was confirmed. */
	if (preview->plan == NULL || preview->digest == NULL || digest == NULL || strcmp(preview->digest, digest) != 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_STALE, "The preview changed after this page was "
			"made. Nothing was migrated: look at it again, then confirm.");
		return FALSE;
	}
	/* The tick to start searching at the end is part of what was confirmed:
	 * its value is the digest of this very run with the tick.  Any other
	 * value (a page made before something changed, or a made-up post) is
	 * refused, and nothing is brought. */
	if (start != NULL && start[0] != '\0') {
		if (preview->digest_start == NULL || strcmp(preview->digest_start, start) != 0) {
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_STALE, "The preview changed after this page was "
				"made. Nothing was migrated and no search was started: look at it again, then confirm.");
			return FALSE;
		}
		start_ticked = TRUE;
	}
	if (preview->counts[AXIS_ARR_SKIP_NONE] + preview->n_relink == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "There is nothing to migrate.");
		return FALSE;
	}
	/* The payload holds numbers, a digest and a user's name: never the key,
	 * and no title or path. */
	json_object_set_int_member(payload, "source", source_id);
	json_object_set_int_member(payload, "plan", preview->plan->id);
	json_object_set_string_member(payload, "choices", preview->choices);
	/* The tick, stored with the job: the run starts the searches at its end
	 * only when this says so. */
	json_object_set_boolean_member(payload, "start", start_ticked);
	if (user != NULL && user[0] != '\0')
		json_object_set_string_member(payload, "user", user);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_ARR_APPLY_KIND;
	spec.queue = AXIS_META_QUEUE;
	spec.payload = payload;
	spec.dedupe_key = key;
	/* A run interrupted by a database error goes on by itself. */
	spec.max_attempts = 3;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* What an earlier run said about starting the searches is not this
	 * run's.  Cleared here, once, with the job (so before it can start), and
	 * not when the job starts: a job that is run again after it started the
	 * searches must find its own note. */
	{
		g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
		const gchar *params[1];

		params[0] = source_text;
		ok = axis_db_conn_begin(conn, error) && axis_job_enqueue(conn, &spec, NULL, &created, error) &&
			(!created || axis_db_conn_exec(conn, "UPDATE arr_source SET run_note = NULL WHERE id = $1::bigint", 1, params,
				NULL, error)) &&
			axis_db_conn_commit(conn, error);
		axis_db_conn_rollback(conn, NULL);
	}
	axis_db_pool_release(self->pool, conn);
	if (ok && !created) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_BUSY, "A run of this connection is already under way.");
		return FALSE;
	}
	return ok;
}

/* ── The job ────────────────────────────────────────────────────── */

typedef struct
{
	AxisArr        *self;
	AxisJobContext *context;
	gint64          source_id;
	gint64          plan_id;
	const gchar    *user;		/* nullable */
	const gchar    *job;		/* the job's number, as text: arr_brought.run_job */
	gchar          *origin;		/* "Radarr (NAME)" */
	/* The hold a movie gets when the file it was held for is found: none
	 * once searching was started for the connection's movies, else the hold
	 * the others still have. */
	const gchar    *hold_after;
	guint           brought;
	guint           linked;
	guint           already;	/* managed meanwhile */
	guint           refused;	/* the movies (or the TV) module refused it */
	gchar          *refusal;	/* the first reason */
	guint           unmatched;	/* episodes Sonarr has that Axis's numbering does not, of the series brought */
} Run;

/* arr_brought.file for what the preview found and the link came to. */
static const gchar *
file_word(
	const AxisArrDecision  *decision,
	const AxisMovieBrought *brought
){
	if (!decision->has_file)
		return "none";
	switch (decision->file) {
	case AXIS_ARR_FILE_FOUND:
		switch (brought->file) {
		case AXIS_MOVIE_BROUGHT_LINKED:
			return "linked";
		case AXIS_MOVIE_BROUGHT_ALREADY:
			return "already";
		case AXIS_MOVIE_BROUGHT_ELSEWHERE:
			return "elsewhere";
		case AXIS_MOVIE_BROUGHT_UNLINKED:
			return "unlinked";
		default:
			return "not_indexed";
		}
	case AXIS_ARR_FILE_OTHER_SIZE:
		return "other_size";
	case AXIS_ARR_FILE_LINKED_ELSEWHERE:
		return "elsewhere";
	case AXIS_ARR_FILE_UNMAPPABLE:
		return "unmappable";
	case AXIS_ARR_FILE_NO_PATH:
		return "no_path";
	default:
		return "not_indexed";
	}
}

/* One movie, in a transaction of its own.  *@gone: the plan's item is not
 * there any more (the plan was replaced or forgotten). */
static gboolean
apply_one(
	Run                    *run,
	AxisDbConn             *conn,
	const AxisArrDecision  *decision,
	gboolean               *gone,
	GError                **error
){
	g_autoptr(AxisDbResult) item = NULL;
	g_autoptr(AxisDbResult) before = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *plan = g_strdup_printf("%" G_GINT64_FORMAT, run->plan_id);
	g_autofree gchar *seq = g_strdup_printf("%u", decision->seq);
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, run->source_id);
	g_autofree gchar *title = g_strdup_printf("%" G_GINT64_FORMAT, decision->title_id);
	g_autofree gchar *movie = NULL;
	g_autofree gchar *arr_id = decision->arr_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, decision->arr_id) : NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *detail = NULL;
	const gchar *params[10];
	const gchar *quality;
	const gchar *word;
	AxisMovieBrought brought;
	gboolean ok;

	*gone = FALSE;
	memset(&brought, 0, sizeof(brought));
	params[0] = plan;
	params[1] = seq;
	params[2] = title;
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	/* The item as the reading stored it; it must still be the title the
	 * decision was made for. */
	if (!axis_db_conn_exec(conn, "SELECT coalesce(i.title, t.title), coalesce(i.year, t.year, 0), i.monitored,"
	        " i.min_availability, i.file_quality, i.file_scene FROM arr_plan_item i JOIN meta_title t ON t.id = i.title_id"
	        " WHERE i.plan_id = $1::bigint AND i.seq = $2::integer AND i.title_id = $3::bigint", 3, params, &item, error))
		goto fail;
	if (axis_db_result_get_n_rows(item) == 0) {
		*gone = TRUE;
		axis_db_conn_rollback(conn, NULL);
		return TRUE;
	}
	{
		gint64 year = axis_arr_int_at(item, 0, 1, 0);

		label = year > 0 ? g_strdup_printf("%s (%" G_GINT64_FORMAT ")", axis_db_result_get_text(item, 0, 0), year) :
			axis_arr_text_at(item, 0, 0);
	}
	/* The quality Radarr recorded, where Axis knows its name (A8); else the
	 * movies module reads the file's own name. */
	quality = axis_arr_quality_to_axis(axis_db_result_get_text(item, 0, 4));
	if (decision->relink) {
		gint64 movie_id;

		params[0] = source;
		params[1] = title;
		if (!axis_db_conn_exec(conn, "SELECT movie_id FROM arr_brought WHERE source_id = $1::bigint AND title_id ="
		        " $2::bigint AND movie_id IS NOT NULL", 2, params, &before, error))
			goto fail;
		movie_id = axis_db_result_get_n_rows(before) > 0 ? axis_arr_int_at(before, 0, 0, 0) : 0;
		if (movie_id <= 0) {
			/* Removed since the job looked: nothing to link to. */
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		ok = axis_movies_bring_file(run->self->movies, conn, movie_id, decision->file_id, quality,
			axis_db_result_get_text(item, 0, 5), run->origin, run->user, run->hold_after, &brought, &local);
		if (!ok && g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND)) {
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&local));
			goto fail;
		}
		movie = g_strdup_printf("%" G_GINT64_FORMAT, movie_id);
	} else {
		AxisMovieBring bring;

		memset(&bring, 0, sizeof(bring));
		bring.spec.title_id = decision->title_id;
		bring.spec.profile_id = decision->profile_id;
		bring.spec.root_id = decision->root_id;
		bring.spec.monitored = axis_arr_bool_at(item, 0, 2);
		/* As in Radarr (A7); a value Axis does not know is the latest. */
		if (!axis_movie_availability_from_name(axis_db_result_get_text(item, 0, 3), &bring.spec.availability))
			bring.spec.availability = AXIS_MOVIE_RELEASED;
		bring.spec.user = run->user;
		bring.spec.brought_from = run->origin;
		bring.file_id = decision->file_id;
		bring.quality = quality;
		bring.release = axis_db_result_get_text(item, 0, 5);
		bring.has_file = decision->has_file;
		ok = axis_movies_bring(run->self->movies, conn, &bring, &brought, &local);
		if (!ok && g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_EXISTS)) {
			/* Managed since the job looked: already in Axis, and left so. */
			run->already++;
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		if (!ok && g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID)) {
			/* Its profile or its library folder went since: it is not brought,
			 * and the others are. */
			run->refused++;
			if (run->refusal == NULL)
				run->refusal = g_strdup(local->message);
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&local));
			goto fail;
		}
		movie = g_strdup_printf("%" G_GINT64_FORMAT, brought.movie_id);
	}
	word = file_word(decision, &brought);
	detail = brought.other != NULL ? g_strdup(brought.other) : g_strdup(decision->other);
	params[0] = source;
	params[1] = title;
	params[2] = movie;
	params[3] = arr_id;
	params[4] = label;
	params[5] = word;
	params[6] = strcmp(word, "elsewhere") == 0 ? detail : NULL;
	params[7] = run->user;
	/* The run that brought it, or linked a file of it, last: what a ticked
	 * run starts the searches of. */
	params[8] = run->job;
	/* A later run names itself only when it linked the file (a relink that
	 * linked nothing leaves the run that last did). */
	params[9] = !decision->relink || strcmp(word, "linked") == 0 ? "true" : "false";
	ok = axis_db_conn_exec(conn, "INSERT INTO arr_brought (source_id, title_id, movie_id, arr_id, label, file,"
		" file_detail, brought_by, run_job) VALUES ($1::bigint, $2::bigint, $3::bigint, $4::bigint, left($5, 320), $6,"
		" left($7, 320), left($8, 200), $9::bigint) ON CONFLICT (source_id, title_id) DO UPDATE"
		" SET movie_id = EXCLUDED.movie_id,"
		" run_job = CASE WHEN $10::boolean THEN EXCLUDED.run_job ELSE arr_brought.run_job END,"
		" arr_id = coalesce(EXCLUDED.arr_id, arr_brought.arr_id), file = EXCLUDED.file,"
		" file_detail = EXCLUDED.file_detail, updated_at = now()", 10, params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	if (!ok)
		goto fail;
	if (!decision->relink)
		run->brought++;
	if (brought.file == AXIS_MOVIE_BROUGHT_LINKED)
		run->linked++;
	axis_movie_brought_clear(&brought);
	return TRUE;

fail:
	axis_movie_brought_clear(&brought);
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

/* ── One series ─────────────────────────────────────────────────── */

/* Whether Sonarr monitors the highest season of [{"n": 1, "m": true}]: what a
 * Sonarr 3, which says nothing of new seasons, is taken to do with them
 * (A13). */
static gboolean
highest_season_monitored(
	const AxisTvBringSeason *seasons,
	guint                    n
){
	gboolean monitored = FALSE;
	gint highest = -1;
	guint i;

	for (i = 0; i < n; i++) {
		if (seasons[i].number > highest) {
			highest = seasons[i].number;
			monitored = seasons[i].monitored;
		}
	}
	return highest > 0 && monitored;
}

static GArray *
seasons_of(const gchar *json)
{
	GArray *seasons = g_array_new(FALSE, TRUE, sizeof(AxisTvBringSeason));
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonArray *array;
	guint i;

	if (json == NULL || !json_parser_load_from_data(parser, json, -1, NULL) || json_parser_get_root(parser) == NULL ||
	    !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser)))
		return seasons;
	array = json_node_get_array(json_parser_get_root(parser));
	for (i = 0; i < json_array_get_length(array) && i < AXIS_ARR_MAX_SEASONS; i++) {
		JsonNode *entry = json_array_get_element(array, i);
		JsonNode *number;
		JsonNode *flag;
		AxisTvBringSeason season;

		if (entry == NULL || !JSON_NODE_HOLDS_OBJECT(entry))
			continue;
		number = json_object_get_member(json_node_get_object(entry), "n");
		flag = json_object_get_member(json_node_get_object(entry), "m");
		if (number == NULL || !JSON_NODE_HOLDS_VALUE(number) || json_node_get_value_type(number) != G_TYPE_INT64 ||
		    json_node_get_int(number) < 0 || json_node_get_int(number) > 10000)
			continue;
		season.number = (gint)json_node_get_int(number);
		season.monitored = flag != NULL && JSON_NODE_HOLDS_VALUE(flag) &&
			json_node_get_value_type(flag) == G_TYPE_BOOLEAN && json_node_get_boolean(flag);
		g_array_append_val(seasons, season);
	}
	return seasons;
}

/* arr_brought.counts of a series: what the preview found of each file and
 * what the TV module made of them. */
static gchar *
counts_json(
	const AxisArrDecision *decision,
	const AxisTvBrought   *brought,
	guint                  n_files
){
	guint by_state[AXIS_ARR_FILE_N] = { 0 };
	guint i;

	for (i = 0; decision->files != NULL && i < decision->files->len; i++) {
		AxisArrFileState state = g_array_index(decision->files, AxisArrFileDecision, i).state;

		if ((guint)state < AXIS_ARR_FILE_N)
			by_state[state]++;
	}
	return g_strdup_printf("{\"files\":%u,\"linked\":%u,\"already\":%u,\"differs\":%u,\"moved\":%u,\"elsewhere\":%u,"
		"\"unlinked\":%u,\"not_indexed\":%u,\"other_size\":%u,\"unmappable\":%u,\"no_path\":%u,\"unmatched_files\":%u,"
		"\"unmatched_episodes\":%u,\"held_episodes\":%u}", n_files, brought->n_linked, brought->n_already,
		brought->n_differs, brought->n_moved, brought->n_elsewhere + by_state[AXIS_ARR_FILE_LINKED_ELSEWHERE],
		brought->n_unlinked,
		by_state[AXIS_ARR_FILE_NOT_INDEXED], by_state[AXIS_ARR_FILE_OTHER_SIZE], by_state[AXIS_ARR_FILE_UNMAPPABLE],
		by_state[AXIS_ARR_FILE_NO_PATH], brought->n_unmatched_files, brought->n_unmatched_episodes,
		brought->n_held_episodes);
}

/*
 * One series, in a transaction of its own: the series through the TV
 * module's own Add under TheTVDB's aired order, its monitoring, its links,
 * its holds and its row of arr_brought, whole or not at all.  The plan gives
 * what Sonarr said (its seasons, its episodes, its files); the decision gives
 * where each file stands in Axis's index.  *@gone: the plan's item is not
 * there any more.
 */
static gboolean
apply_series(
	Run                    *run,
	AxisDbConn             *conn,
	const AxisArrDecision  *decision,
	gboolean               *gone,
	GError                **error
){
	g_autoptr(AxisDbResult) item = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(AxisDbResult) listed = NULL;
	g_autoptr(AxisDbResult) before = NULL;
	g_autoptr(GArray) seasons = NULL;
	g_autoptr(GArray) episodes = g_array_new(FALSE, TRUE, sizeof(AxisTvBringEpisode));
	g_autoptr(GArray) files = g_array_new(FALSE, TRUE, sizeof(AxisTvBringFile));
	g_autoptr(GHashTable) at = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GError) local = NULL;
	g_autofree gchar *plan = g_strdup_printf("%" G_GINT64_FORMAT, run->plan_id);
	g_autofree gchar *seq = g_strdup_printf("%u", decision->seq);
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, run->source_id);
	g_autofree gchar *title = g_strdup_printf("%" G_GINT64_FORMAT, decision->title_id);
	g_autofree gchar *arr_id = decision->arr_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, decision->arr_id) : NULL;
	g_autofree gchar *series_text = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *counts = NULL;
	g_autofree gchar *n_linked = NULL;
	g_autofree gchar *n_held = NULL;
	g_autofree gchar *n_moved = NULL;
	const gchar *params[9];
	const gchar *word;
	AxisTvBrought brought;
	guint n_files = 0;
	gboolean ok;
	guint i;

	*gone = FALSE;
	memset(&brought, 0, sizeof(brought));
	/* As a person's Add does before its transaction: the series' record and
	 * its aired order, asked for only when they are not fresh.  A series
	 * linked later needs neither. */
	if (!decision->relink)
		axis_tv_bring_prepare(run->self->tv, decision->title_id, AXIS_ARR_SERIES_SOURCE);
	params[0] = plan;
	params[1] = seq;
	params[2] = title;
	if (!axis_db_conn_begin(conn, error))
		return FALSE;
	/* The item as the reading stored it; it must still be the title the
	 * decision was made for. */
	if (!axis_db_conn_exec(conn, "SELECT coalesce(i.title, t.title), coalesce(i.year, t.year, 0), i.monitored,"
	        " i.monitor_new, i.seasons::text, coalesce(i.series_type = '" AXIS_ARR_SERIES_TYPE_ANIME "', false)"
	        " FROM arr_plan_item i JOIN meta_title t ON t.id = i.title_id"
	        " WHERE i.plan_id = $1::bigint AND i.seq = $2::integer AND i.title_id = $3::bigint", 3, params, &item, error))
		goto fail;
	if (axis_db_result_get_n_rows(item) == 0) {
		*gone = TRUE;
		axis_db_conn_rollback(conn, NULL);
		return TRUE;
	}
	{
		gint64 year = axis_arr_int_at(item, 0, 1, 0);

		label = year > 0 ? g_strdup_printf("%s (%" G_GINT64_FORMAT ")", axis_db_result_get_text(item, 0, 0), year) :
			axis_arr_text_at(item, 0, 0);
	}
	/* Its episodes and its files as Sonarr listed them. */
	if (!axis_db_conn_exec(conn, "SELECT season, episode, coalesce(tvdb_id, 0), monitored, coalesce(file_arr_id, 0), odd,"
	        " coalesce(absolute_number, 0)"
	        " FROM arr_plan_episode WHERE plan_id = $1::bigint AND seq = $2::integer ORDER BY ord", 2, params, &rows,
	        error) ||
	    !axis_db_conn_exec(conn, "SELECT arr_id, quality, scene FROM arr_plan_file WHERE plan_id = $1::bigint"
	        " AND seq = $2::integer ORDER BY ord", 2, params, &listed, error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisTvBringEpisode episode;

		memset(&episode, 0, sizeof(episode));
		episode.season = (gint)axis_arr_int_at(rows, i, 0, 0);
		episode.number = (gint)axis_arr_int_at(rows, i, 1, 0);
		episode.tvdb_id = axis_arr_int_at(rows, i, 2, 0);
		episode.monitored = axis_arr_bool_at(rows, i, 3);
		episode.file_ref = axis_arr_int_at(rows, i, 4, 0);
		/* A place whose entry could not be read: the TV module leaves the
		 * episode there unmonitored and held. */
		episode.odd = axis_arr_bool_at(rows, i, 5);
		/* Sonarr's own absolute number: the TV module compares it with
		 * TheTVDB's for an anime series, and uses it for nothing. */
		episode.absolute = (gint)axis_arr_int_at(rows, i, 6, 0);
		g_array_append_val(episodes, episode);
	}
	/* Where each file stands in the index, by Sonarr's number of it: only a
	 * file the preview found at its path and of its size has a row to link. */
	for (i = 0; decision->files != NULL && i < decision->files->len; i++) {
		AxisArrFileDecision *file = &g_array_index(decision->files, AxisArrFileDecision, i);

		g_hash_table_insert(at, &file->arr_id, file);
	}
	n_files = axis_db_result_get_n_rows(listed);
	for (i = 0; i < n_files; i++) {
		gint64 number = axis_arr_int_at(listed, i, 0, 0);
		const AxisArrFileDecision *found = g_hash_table_lookup(at, &number);
		AxisTvBringFile file;

		/* A later run is handed only the files it links. */
		if (decision->relink && found == NULL)
			continue;
		memset(&file, 0, sizeof(file));
		file.file_ref = number;
		file.file_id = found != NULL && found->state == AXIS_ARR_FILE_FOUND ? found->file_id : 0;
		file.other_id = found != NULL && found->state == AXIS_ARR_FILE_OTHER_SIZE ? found->other_size_id : 0;
		/* The quality Sonarr recorded, where Axis knows its name (A8); else
		 * the TV module reads the file's own name. */
		file.quality = axis_arr_quality_to_axis(axis_db_result_get_text(listed, i, 1));
		file.release = axis_db_result_get_text(listed, i, 2);
		g_array_append_val(files, file);
	}
	if (decision->relink) {
		gint64 series_id;

		params[0] = source;
		params[1] = title;
		if (!axis_db_conn_exec(conn, "SELECT series_id FROM arr_brought WHERE source_id = $1::bigint AND title_id ="
		        " $2::bigint AND series_id IS NOT NULL", 2, params, &before, error))
			goto fail;
		series_id = axis_db_result_get_n_rows(before) > 0 ? axis_arr_int_at(before, 0, 0, 0) : 0;
		if (series_id <= 0) {
			/* Removed since the job looked: nothing to link to. */
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		ok = axis_tv_bring_files(run->self->tv, conn, series_id, AXIS_ARR_SERIES_SOURCE,
			(const AxisTvBringEpisode *)episodes->data, episodes->len, (const AxisTvBringFile *)files->data, files->len,
			run->origin, run->user, &brought, &local);
		if (!ok && g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND)) {
			axis_db_conn_rollback(conn, NULL);
			return TRUE;
		}
		if (!ok) {
			g_propagate_error(error, g_steal_pointer(&local));
			goto fail;
		}
		if (brought.other_numbering) {
			/* A person changed its numbering between the preview and now (the
			 * preview offers no file of such a series): nothing was linked,
			 * and its row says why. */
			if (!axis_db_conn_exec(conn, "UPDATE arr_brought SET counts = coalesce(counts, '{}'::jsonb)"
			        " || jsonb_build_object('other_numbering', 1), updated_at = now()"
			        " WHERE source_id = $1::bigint AND title_id = $2::bigint", 2, params, NULL, error) ||
			    !axis_db_conn_commit(conn, error))
				goto fail;
			return TRUE;
		}
		/* Its row says what stands now: the files linked since are added to
		 * those linked then. */
		n_linked = g_strdup_printf("%u", brought.n_linked);
		n_held = g_strdup_printf("%u", brought.n_held_episodes);
		n_moved = g_strdup_printf("%u", brought.n_moved);
		params[2] = n_linked;
		params[3] = n_held;
		params[4] = n_moved;
		params[5] = run->job;
		/* (A link moved now was identification's, made after the first run:
		 * it is added to those moved then.) */
		/* (The run is named only when it linked a file: a relink that linked
		 * nothing leaves the run that last did.) */
		if (!axis_db_conn_exec(conn, "UPDATE arr_brought SET run_job = CASE WHEN $3::integer + $5::integer > 0"
		        " THEN $6::bigint ELSE run_job END, counts = coalesce(counts, '{}'::jsonb)"
		        " || jsonb_build_object('linked', coalesce((counts ->> 'linked')::integer, 0) + $3::integer,"
		        " 'moved', coalesce((counts ->> 'moved')::integer, 0) + $5::integer,"
		        " 'held_episodes', $4::integer) - 'other_numbering',"
		        " file = CASE WHEN coalesce((counts ->> 'linked')::integer, 0) + $3::integer"
		        "   + coalesce((counts ->> 'already')::integer, 0) + coalesce((counts ->> 'moved')::integer, 0)"
		        "   + $5::integer >= coalesce((counts ->> 'files')::integer, 0)"
		        "   THEN 'linked' ELSE 'partly' END, updated_at = now()"
		        " WHERE source_id = $1::bigint AND title_id = $2::bigint", 6, params, NULL, error) ||
		    !axis_db_conn_commit(conn, error))
			goto fail;
		run->linked += brought.n_linked;
		return TRUE;
	}
	seasons = seasons_of(axis_db_result_get_text(item, 0, 4));
	{
		AxisTvBring bring;

		memset(&bring, 0, sizeof(bring));
		bring.spec.title_id = decision->title_id;
		bring.spec.profile_id = decision->profile_id;
		bring.spec.root_id = decision->root_id;
		bring.spec.user = run->user;
		bring.spec.brought_from = run->origin;
		/* TheTVDB's aired order, from the start: the numbering Sonarr's
		 * seasons, episodes and monitoring are in. */
		bring.spec.source = AXIS_ARR_SERIES_SOURCE;
		/* Its kind, as Sonarr has it: an anime series is brought exactly as
		 * any other (the same match, links, kept files and hold), marked as
		 * anime in Axis, with its absolute numbers compared and reported. */
		bring.spec.anime = axis_arr_bool_at(item, 0, 5);
		bring.monitored = axis_arr_bool_at(item, 0, 2);
		/* Seasons that come later: as Sonarr 4 says; a Sonarr 3 says nothing,
		 * and is taken to monitor them when it monitors the highest season. */
		bring.monitor_new = axis_db_result_get_text(item, 0, 3) != NULL ? axis_arr_bool_at(item, 0, 3) :
			highest_season_monitored((const AxisTvBringSeason *)seasons->data, seasons->len);
		bring.seasons = (const AxisTvBringSeason *)seasons->data;
		bring.n_seasons = seasons->len;
		bring.episodes = (const AxisTvBringEpisode *)episodes->data;
		bring.n_episodes = episodes->len;
		bring.files = (const AxisTvBringFile *)files->data;
		bring.n_files = files->len;
		ok = axis_tv_bring(run->self->tv, conn, &bring, &brought, &local);
	}
	if (!ok && g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_EXISTS)) {
		/* Managed since the job looked: already in Axis, and left so. */
		run->already++;
		axis_db_conn_rollback(conn, NULL);
		return TRUE;
	}
	if (!ok && g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID)) {
		/* Its profile or its library folder went since, or its aired order is
		 * not held: it is not brought, and the others are. */
		run->refused++;
		if (run->refusal == NULL)
			run->refusal = g_strdup(local->message);
		axis_db_conn_rollback(conn, NULL);
		return TRUE;
	}
	if (!ok) {
		g_propagate_error(error, g_steal_pointer(&local));
		goto fail;
	}
	series_text = g_strdup_printf("%" G_GINT64_FORMAT, brought.series_id);
	counts = counts_json(decision, &brought, n_files);
	word = n_files == 0 ? "none" : brought.n_linked + brought.n_already + brought.n_moved >= n_files ? "linked" :
		"partly";
	params[0] = source;
	params[1] = title;
	params[2] = series_text;
	params[3] = arr_id;
	params[4] = label;
	params[5] = word;
	params[6] = counts;
	params[7] = run->user;
	params[8] = run->job;
	ok = axis_db_conn_exec(conn, "INSERT INTO arr_brought (source_id, title_id, series_id, arr_id, label, file, counts,"
		" brought_by, run_job) VALUES ($1::bigint, $2::bigint, $3::bigint, $4::bigint, left($5, 320), $6,"
		/* With what the TV module found of an anime series' absolute numbers
		 * (series.brought_absolute), for the list of what was migrated. */
		" $7::jsonb || coalesce((SELECT jsonb_build_object('absolute', s.brought_absolute) FROM series s"
		"   WHERE s.id = $3::bigint AND s.brought_absolute IS NOT NULL), '{}'::jsonb),"
		" left($8, 200), $9::bigint) ON CONFLICT (source_id, title_id) DO UPDATE SET series_id = EXCLUDED.series_id,"
		" run_job = EXCLUDED.run_job,"
		" arr_id = coalesce(EXCLUDED.arr_id, arr_brought.arr_id), file = EXCLUDED.file, counts = EXCLUDED.counts,"
		" updated_at = now()", 9, params, NULL, error) &&
		axis_db_conn_commit(conn, error);
	if (!ok)
		goto fail;
	run->brought++;
	run->linked += brought.n_linked;
	run->unmatched += brought.n_unmatched_episodes;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	return FALSE;
}

static gint64
payload_id(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

static const gchar *
payload_text(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

static gboolean
permanent(
	GError      **error,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, message);
	return FALSE;
}

static gboolean
payload_yes(
	JsonObject  *payload,
	const gchar *name
){
	JsonNode *node = payload != NULL ? json_object_get_member(payload, name) : NULL;

	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
		json_node_get_boolean(node);
}

/* What the last run said about starting the searches at its end (nullable:
 * nothing to say), for the connection's page. */
static gboolean
set_run_note(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *note,
	GError      **error
){
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	const gchar *params[2];

	params[0] = source;
	params[1] = note;
	return axis_db_pool_exec(self->pool, "UPDATE arr_source SET run_note = left($2, 1000) WHERE id = $1::bigint", 2,
		params, NULL, error);
}

/*
 * The tick of the run's confirmation, "Start searching for these as soon as
 * they are migrated": the run does at its end what "Start searching" does
 * for the connection, by the same code (start_searching()): the app's
 * download queue is read again, a title in it stays held as "the app is
 * downloading it", the others are started, and a hold for a file Axis does
 * not see is never touched.  Nobody is there to tick "the app is stopped":
 * when the queue cannot be read (no answer, or an answer that does not say
 * which titles it is downloading) nothing is started, everything stays held,
 * and the connection's page says so.
 */
/* What the connection's page says after a ticked run started its searches:
 * how many, how many were left out, and (@tail, nullable) what the run could
 * not bring. */
static gchar *
started_note(
	const AxisArrStart *start,
	const gchar        *tail
){
	gboolean series = start->source->app == AXIS_ARR_SONARR;
	const gchar *app = axis_arr_app_label(start->source->app);
	g_autofree gchar *left = start->n_in_queue == 0 ? g_strdup("") : g_strdup_printf(" %u %s left out and "
		"stay%s held: %s is downloading %s. Start searching again once %s has finished.", start->n_in_queue,
		start->n_in_queue == 1 ? "was" : "were", start->n_in_queue == 1 ? "s" : "", app,
		series ? (start->n_in_queue == 1 ? "for it" : "for them") : start->n_in_queue == 1 ? "it" : "them", app);

	g_autofree gchar *others = start->n_other_held == 0 ? g_strdup("") : g_strdup_printf(" %u %s migrated by an earlier "
		"run %s still held; press Start searching for %s.", start->n_other_held, series ? "series" : "titles",
		start->n_other_held == 1 ? "is" : "are", start->n_other_held == 1 ? "it" : "them");

	return g_strdup_printf("Searching was started at the end of the last run, as ticked on its confirmation: for "
		"%u %s of that run.%s%s%s%s", start->started, series ? "series" : start->started == 1 ? "movie" : "movies",
		left, others, tail != NULL ? " " : "", tail != NULL ? tail : "");
}

/*
 * @job: the run's own job: only the titles it brought, or linked a file of
 * (arr_brought.run_job), are started; what earlier runs brought stays held
 * until "Start searching".  @tail: (nullable) what the run could not bring,
 * said after the note.
 */
static gboolean
start_at_end(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *job,
	const gchar  *tail,
	const gchar  *user,
	GError      **error
){
	g_autoptr(AxisArrStart) start = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *note = NULL;
	Unattended un;

	un.job = job;
	un.tail = tail;
	/* Started: the note was written with the holds, in one transaction, so
	 * that a job that is run again after that commit finds its note there
	 * (it then finds nothing held, below, and leaves the note alone). */
	if (start_searching(self, source_id, FALSE, &un, user, &start, &local)) {
		g_info("migrate: connection %" G_GINT64_FORMAT ": searching started at the end of the run for %u (%u left "
			"out)", source_id, start->started, start->n_in_queue);
		return TRUE;
	}
	if (g_error_matches(local, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NEEDS_TICK) && start != NULL) {
		const gchar *app = axis_arr_app_label(start->source->app);

		note = start->queue == AXIS_ARR_QUEUE_NO_ANSWER ?
			g_strdup_printf("Searching was not started at the end of the last run, though its confirmation was ticked: "
				"%s did not answer, so Axis could not check what it is downloading. Everything migrated is held. "
				"Start searching from this page.%s%s", app, tail != NULL ? " " : "", tail != NULL ? tail : "") :
			g_strdup_printf("Searching was not started at the end of the last run, though its confirmation was ticked: "
				"%s answered, but Axis could not read which titles it is downloading. Everything migrated is held. "
				"Start searching from this page.%s%s", app, tail != NULL ? " " : "", tail != NULL ? tail : "");
		g_info("migrate: connection %" G_GINT64_FORMAT ": searching not started at the end of the run: the queue "
			"could not be read", source_id);
		return set_run_note(self, source_id, note, error);
	}
	if (g_error_matches(local, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
		g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
		const gchar *params[2];

		/* Nothing this run brought is held: nothing to start, and the page
		 * says so (a ticked run with no note would look as if it had not
		 * been asked).  A note an earlier attempt of this job wrote stands: the
		 * note was cleared when the job was made, so an empty one is not
		 * this job's. */
		note = g_strdup_printf("Nothing of this run was waiting to be searched for.%s%s", tail != NULL ? " " : "",
			tail != NULL ? tail : "");
		params[0] = source;
		params[1] = note;
		return axis_db_pool_exec(self->pool, "UPDATE arr_source SET run_note = left($2, 1000) WHERE id = $1::bigint"
			" AND run_note IS NULL", 2, params, NULL, error);
	}
	if (g_error_matches(local, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		/* The connection was forgotten: nothing to start. */
		return TRUE;
	g_propagate_error(error, g_steal_pointer(&local));
	return FALSE;
}

gboolean
axis_arr_run_apply(
	AxisArr         *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(GPtrArray) decisions = NULL;
	g_autoptr(AxisDbResult) started = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *source_text = NULL;
	const gchar *choices = payload_text(payload, "choices");
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean ok = TRUE;
	gboolean gone = FALSE;
	gboolean series;
	gchar job[32];
	Run run;
	guint i;

	memset(&run, 0, sizeof(run));
	run.self = self;
	run.context = context;
	run.source_id = payload_id(payload, "source");
	run.plan_id = payload_id(payload, "plan");
	run.user = payload_text(payload, "user");
	if (run.source_id <= 0 || run.plan_id <= 0 || choices == NULL)
		return permanent(error, "The job names no connection.");
	axis_job_context_set_progress(context, 0.0, "Looking at what to migrate");
	/* What the preview decides now, under the plan and the choices that
	 * were confirmed: each movie is checked again here, and nothing the
	 * preview did not show is brought. */
	if (!axis_arr_preview_decide(self, run.source_id, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &decisions, &local)) {
		if (g_error_matches(local, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
			return permanent(error, "The connection was forgotten.");
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	/* The connection's app, as stored, decides what is brought and through
	 * which module: a Radarr's movies, a Sonarr's series.  The decisions are
	 * of that app's plan, so neither can reach the other's module. */
	series = preview->source->app == AXIS_ARR_SONARR;
	if (!series && preview->source->app != AXIS_ARR_RADARR)
		return permanent(error, "Nothing can be migrated from this app.");
	if (!series && self->movies == NULL)
		return permanent(error, "Movies cannot be migrated: the Movies module is not there.");
	if (series && self->tv == NULL)
		return permanent(error, "Series cannot be migrated: the TV module is not there. Nothing was migrated.");
	if (series && !preview->series_ready)
		return permanent(error, "Axis has no TheTVDB key now, and a series from Sonarr follows TheTVDB's aired order. "
			"Nothing more was migrated.");
	if (preview->plan == NULL || decisions == NULL || preview->plan->id != run.plan_id || preview->choices == NULL ||
	    strcmp(preview->choices, choices) != 0)
		return permanent(error, "The reading or the choices changed after this run was confirmed. Open the preview "
			"and confirm again: what was migrated so far stays.");
	g_snprintf(job, sizeof(job), "%" G_GINT64_FORMAT, axis_job_context_get_id(context));
	run.job = job;
	run.origin = origin_of(preview->source);
	source_text = g_strdup_printf("%" G_GINT64_FORMAT, run.source_id);
	params[0] = source_text;
	if (!axis_db_pool_exec(self->pool, "SELECT search_started_at IS NOT NULL FROM arr_source WHERE id = $1::bigint", 1,
	        params, &started, error)) {
		g_free(run.origin);
		return FALSE;
	}
	run.hold_after = axis_db_result_get_n_rows(started) > 0 && axis_arr_bool_at(started, 0, 0) ? NULL :
		AXIS_MOVIE_HOLD_BROUGHT;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL) {
		g_free(run.origin);
		return FALSE;
	}
	for (i = 0; ok && !gone && i < decisions->len; i++) {
		if (axis_job_context_is_cancelled(context))
			break;
		/* A seam for tests: a run interrupted after some titles. */
		if (axis_arr_apply_hook != NULL && !axis_arr_apply_hook(i, axis_arr_apply_hook_data, error)) {
			ok = FALSE;
			break;
		}
		ok = series ? apply_series(&run, conn, g_ptr_array_index(decisions, i), &gone, error) :
			apply_one(&run, conn, g_ptr_array_index(decisions, i), &gone, error);
		if (ok && (i % 20 == 0 || i + 1 == decisions->len)) {
			g_autofree gchar *message = g_strdup_printf("%s %u of %u", series ? "Series" : "Movie", i + 1,
				decisions->len);

			axis_job_context_set_progress(context, (gdouble)(i + 1) / decisions->len, message);
		}
	}
	axis_db_pool_release(self->pool, conn);
	/* The series brought follow TheTVDB's aired order: the metadata layer is
	 * told, so that it keeps that order fresh. */
	if (series && run.brought > 0)
		axis_tv_brought_refresh(self->tv);
	g_info("migrate: connection %" G_GINT64_FORMAT ": %u %s brought, %u files linked, %u already there, %u refused, "
		"%u episodes not in Axis's numbering", run.source_id, run.brought, series ? "series" : "movies", run.linked,
		run.already, run.refused, run.unmatched);
	g_free(run.origin);
	if (!ok) {
		g_free(run.refusal);
		return FALSE;
	}
	if (axis_job_context_is_cancelled(context)) {
		AxisJobStopReason stop = axis_job_context_get_stop_reason(context);

		g_free(run.refusal);
		/* What was brought stays; a run interrupted by a restart goes on by
		 * itself, one a person cancelled is continued from the preview. */
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED,
			stop == AXIS_JOB_STOP_SHUTDOWN || stop == AXIS_JOB_STOP_LEASE_LOST ? "The run was interrupted." :
			"The run was cancelled. What it migrated stays: confirm again on the preview to continue.");
		return FALSE;
	}
	if (gone) {
		g_free(run.refusal);
		return permanent(error, "The reading was replaced or forgotten while the run worked. What it migrated stays: "
			"open the preview and confirm again to continue.");
	}
	/* Every title was taken: with the tick of the confirmation, the searches
	 * are started as "Start searching" starts them.  A run that stopped
	 * before its end starts none. */
	{
		/* The movies module's words name a profile or a library folder of
		 * Axis's own: nothing of Radarr's. */
		g_autofree gchar *message = run.refused == 0 ? NULL : g_strdup_printf("%u %s not migrated: %s. The others "
			"were. Look at the preview again.", run.refused, series ? (run.refused == 1 ? "series was" : "series were") :
			run.refused == 1 ? "movie was" : "movies were", run.refusal);

		g_free(run.refusal);
		/* Of this run's own titles only, and with what it could not bring
		 * said in the same note. */
		if (payload_yes(payload, "start") && !start_at_end(self, run.source_id, run.job, message, run.user, error))
			return FALSE;
		if (message != NULL)
			return permanent(error, message);
	}
	return TRUE;
}

/* ── What a connection brought ──────────────────────────────────── */

void
axis_arr_run_free(AxisArrRun *run)
{
	if (run == NULL)
		return;
	g_free(run->progress_message);
	g_free(run->last_state);
	g_free(run->last_error);
	g_free(run->run_note);
	g_free(run);
}

gboolean
axis_arr_run_state(
	AxisArr     *self,
	gint64       source_id,
	AxisArrRun **out,
	GError     **error
){
	g_autoptr(AxisDbResult) job = NULL;
	g_autoptr(AxisDbResult) last = NULL;
	g_autoptr(AxisDbResult) counts = NULL;
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *key = g_strdup_printf(AXIS_ARR_APPLY_KEY_PREFIX "%" G_GINT64_FORMAT, source_id);
	const gchar *params[1];
	AxisArrRun *run;
	AxisDbConn *conn;
	gboolean ok;

	*out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = key;
	ok = axis_db_conn_exec(conn, "SELECT progress, progress_message FROM job WHERE dedupe_key = $1"
		" AND state IN ('queued', 'running') LIMIT 1", 1, params, &job, error);
	params[0] = source;
	ok = ok && axis_db_conn_exec(conn, "SELECT state::text, last_error FROM job WHERE kind = '" AXIS_ARR_APPLY_KIND "'"
		" AND payload ->> 'source' = $1 AND state NOT IN ('queued', 'running') ORDER BY id DESC LIMIT 1", 1, params,
		&last, error);
	ok = ok && axis_db_conn_exec(conn,
		/* A row is a movie's or a series' (the connection's app): the one
		 * statement counts either.  A series' files linked are in its counts;
		 * its episodes held apart are counted by themselves. */
		"SELECT count(*), count(*) FILTER (WHERE b.movie_id IS NULL AND b.series_id IS NULL),"
		" coalesce(sum(CASE WHEN b.counts IS NOT NULL THEN coalesce((b.counts ->> 'linked')::integer, 0)"
		"   WHEN b.file = 'linked' THEN 1 ELSE 0 END), 0),"
		" count(*) FILTER (WHERE coalesce(m.search_hold, sr.search_hold) IS NOT NULL),"
		" count(*) FILTER (WHERE m.search_hold = '" AXIS_MOVIE_HOLD_FILE_UNSEEN "'),"
		" count(*) FILTER (WHERE coalesce(m.search_hold, sr.search_hold) = '" AXIS_MOVIE_HOLD_DOWNLOADING "'),"
		" (SELECT " AXIS_ARR_US("s.search_started_at") " FROM arr_source s WHERE s.id = $1::bigint),"
		" (SELECT count(*) FROM arr_brought b2 JOIN series_episode e ON e.series_id = b2.series_id"
		"   WHERE b2.source_id = $1::bigint AND " AXIS_TV_EPISODE_HELD_SQL("e") " AND e.gone_at IS NULL),"
		" (SELECT count(*)" KEPT_MOVIE_FILES("b3") ") + (SELECT count(*)" KEPT_SERIES_FILES("b4") "),"
		" (SELECT s.run_note FROM arr_source s WHERE s.id = $1::bigint),"
		/* Files the run did not see, linked by identification since and not
		 * kept: a later run keeps them. */
		" (SELECT count(*) FROM arr_brought b5 JOIN movie_file mf ON mf.movie_id = b5.movie_id"
		"   JOIN library_file ulf ON ulf.id = mf.file_id AND ulf.state = 'present' AND ulf.kind = 'video'"
		"   WHERE b5.source_id = $1::bigint AND b5.file IN ('not_indexed', 'other_size')"
		"   AND NOT mf.kept AND mf.hand_at IS NULL AND mf.import_id IS NULL)"
		" + (SELECT count(*) FROM arr_brought b6 JOIN series_file sf ON sf.series_id = b6.series_id"
		"   JOIN library_file ulf ON ulf.id = sf.file_id AND ulf.state = 'present' AND ulf.kind = 'video'"
		"   WHERE b6.source_id = $1::bigint AND (coalesce((b6.counts ->> 'not_indexed')::integer, 0) > 0"
		"   OR coalesce((b6.counts ->> 'other_size')::integer, 0) > 0)"
		"   AND NOT sf.kept AND sf.hand_at IS NULL AND sf.import_id IS NULL),"
		/* Of the titles held as brought: those "Start searching" would have
		 * searched for, and those it changes nothing for now. */
		" count(*) FILTER (WHERE " AXIS_ARR_MOVIE_WAITS_SQL("m") " OR " AXIS_ARR_SERIES_WAITS_SQL("sr") "),"
		" count(*) FILTER (WHERE (m.search_hold = '" AXIS_MOVIE_HOLD_BROUGHT "' AND " AXIS_ARR_MOVIE_HAS_FILE_SQL("m") ")"
		"   OR (sr.search_hold = '" AXIS_TV_HOLD_BROUGHT "' AND NOT " AXIS_ARR_SERIES_WAITS_SQL("sr") "))"
		" FROM arr_brought b LEFT JOIN movie m ON m.id = b.movie_id LEFT JOIN series sr ON sr.id = b.series_id"
		" WHERE b.source_id = $1::bigint", 1, params, &counts, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	run = g_new0(AxisArrRun, 1);
	run->progress = -1.0;
	if (axis_db_result_get_n_rows(job) > 0) {
		const gchar *progress = axis_db_result_get_text(job, 0, 0);

		run->job_live = TRUE;
		run->progress = progress != NULL ? CLAMP(g_ascii_strtod(progress, NULL), 0.0, 1.0) : -1.0;
		run->progress_message = axis_arr_text_at(job, 0, 1);
	}
	if (axis_db_result_get_n_rows(last) > 0) {
		run->last_state = axis_arr_text_at(last, 0, 0);
		run->last_error = axis_arr_text_at(last, 0, 1);
	}
	run->n_brought = (guint)axis_arr_int_at(counts, 0, 0, 0);
	run->n_gone = (guint)axis_arr_int_at(counts, 0, 1, 0);
	run->n_linked = (guint)axis_arr_int_at(counts, 0, 2, 0);
	run->n_held = (guint)axis_arr_int_at(counts, 0, 3, 0);
	run->n_unseen = (guint)axis_arr_int_at(counts, 0, 4, 0);
	run->n_downloading = (guint)axis_arr_int_at(counts, 0, 5, 0);
	run->started_us = axis_arr_int_at(counts, 0, 6, 0);
	run->n_unseen_episodes = (guint)axis_arr_int_at(counts, 0, 7, 0);
	run->n_kept = (guint)axis_arr_int_at(counts, 0, 8, 0);
	run->run_note = axis_arr_text_at(counts, 0, 9);
	run->n_unkept = (guint)axis_arr_int_at(counts, 0, 10, 0);
	run->n_waiting = (guint)axis_arr_int_at(counts, 0, 11, 0);
	run->n_with_file = (guint)axis_arr_int_at(counts, 0, 12, 0);
	*out = run;
	return TRUE;
}

gchar *
axis_arr_brought_file_label(
	const gchar *file,
	const gchar *detail,
	AxisArrApp   app
){
	const gchar *label = axis_arr_app_label(app);

	if (g_strcmp0(file, "linked") == 0)
		return g_strdup("Linked");
	if (g_strcmp0(file, "already") == 0)
		return g_strdup("Linked in Axis already: left as it was");
	if (g_strcmp0(file, "not_indexed") == 0)
		return g_strdup("Not in Axis's index at that path");
	if (g_strcmp0(file, "other_size") == 0)
		return g_strdup_printf("Another size than %s recorded", label);
	if (g_strcmp0(file, "elsewhere") == 0)
		return detail != NULL ? g_strdup_printf("Linked in Axis to another movie: %s. Left there", detail) :
			g_strdup("Linked in Axis to another movie: left there");
	if (g_strcmp0(file, "unmappable") == 0)
		return g_strdup("Path cannot be mapped");
	if (g_strcmp0(file, "no_path") == 0)
		return g_strdup_printf("%s gives no path or size for it", label);
	if (g_strcmp0(file, "unlinked") == 0)
		return g_strdup("Unlinked by hand in Axis: left so");
	return g_strdup_printf("%s has no file for it", label);
}

static void
brought_row_free(gpointer data)
{
	AxisArrBroughtRow *row = data;

	g_free(row->label);
	g_free(row->file);
	g_free(row->file_detail);
	g_free(row->hold);
	g_free(row->counts);
	g_free(row);
}

static gint64
count_member(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return MAX(json_node_get_int(node), 0);
}

gchar *
axis_arr_brought_counts_label(
	const gchar *counts,
	AxisArrApp   app
){
	static const struct
	{
		const gchar *member;
		const gchar *one;
		const gchar *many;
	} reasons[] = {
		{ "already", "linked in Axis already: left as it was", "linked in Axis already: left as they were" },
		{ "differs", "linked in Axis to another episode than the app has it on: left there, since a person or an import "
		  "put it there, or it cannot be checked against the app (an episode the app has it on is not in Axis's "
		  "numbering, or its size is not the one the app recorded)",
		  "linked in Axis to other episodes than the app has them on: left there, since a person or an import put them "
		  "there, or they cannot be checked against the app (an episode the app has them on is not in Axis's numbering, "
		  "or their size is not the one the app recorded)" },
		{ "moved", "linked by its name to another episode than the app has it on: moved to the episode the app has it on",
		  "linked by their names to other episodes than the app has them on: moved to the episodes the app has them on" },
		{ "elsewhere", "linked in Axis to another series: left there", "linked in Axis to another series: left there" },
		{ "unlinked", "unlinked by hand in Axis: left so", "unlinked by hand in Axis: left so" },
		{ "not_indexed", "not in Axis's index at its path", "not in Axis's index at their paths" },
		{ "other_size", "of another size than the app recorded", "of another size than the app recorded" },
		{ "unmappable", "at a path that cannot be mapped", "at paths that cannot be mapped" },
		{ "no_path", "without a path or a size", "without a path or a size" },
		{ "unmatched_files", "on an episode Axis's numbering does not have: not linked",
		  "on episodes Axis's numbering does not have: not linked" }
	};
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GString) text = g_string_new(NULL);
	const gchar *label = axis_arr_app_label(app);
	JsonObject *object;
	gint64 files;
	gint64 n;
	guint i;

	if (counts == NULL || !json_parser_load_from_data(parser, counts, -1, NULL) || json_parser_get_root(parser) == NULL ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
		return g_strdup_printf("%s has no file for it", label);
	object = json_node_get_object(json_parser_get_root(parser));
	files = count_member(object, "files");
	if (files == 0)
		g_string_append_printf(text, "%s has no file for it.", label);
	else
		g_string_append_printf(text, "%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " %s linked.",
			count_member(object, "linked"), files, files == 1 ? "file" : "files");
	for (i = 0; i < G_N_ELEMENTS(reasons); i++) {
		n = count_member(object, reasons[i].member);
		if (n > 0)
			g_string_append_printf(text, " %" G_GINT64_FORMAT " %s.", n, n == 1 ? reasons[i].one : reasons[i].many);
	}
	n = count_member(object, "unmatched_episodes");
	if (n > 0)
		g_string_append_printf(text, " %" G_GINT64_FORMAT " %s %s has that Axis's numbering does not: left as they "
			"are, and nothing was made of them.", n, n == 1 ? "episode" : "episodes", label);
	if (count_member(object, "other_numbering") > 0)
		g_string_append(text, " The series no longer follows TheTVDB's numbering in Axis: a later run linked nothing "
			"more of it.");
	/* A page names the app, never "the app". */
	g_string_replace(text, "the app", label, 0);
	/* A series migrated as anime: what came of comparing Sonarr's absolute
	 * numbers with TheTVDB's, and every episode that differs.  A report: no
	 * count above depends on it. */
	if (json_object_has_member(object, "absolute")) {
		g_autofree gchar *stored = json_to_string(json_object_get_member(object, "absolute"), FALSE);
		AxisTvAbsoluteCompare compare;

		if (axis_tv_absolute_compare_parse(stored, &compare)) {
			g_autofree gchar *sentence = axis_tv_absolute_compare_sentence(&compare);
			g_autofree gchar *list = axis_tv_absolute_compare_list(&compare);

			g_string_append_printf(text, " Migrated as anime. %s", sentence);
			if (list != NULL && compare.differ > 1)
				g_string_append_printf(text, " They differ for: %s.", list);
		}
		axis_tv_absolute_compare_clear(&compare);
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

gboolean
axis_arr_list_brought(
	AxisArr    *self,
	gint64      source_id,
	gboolean    only_unlinked,
	guint       page,
	GPtrArray **rows,
	guint      *total,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *offset = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)page * AXIS_ARR_PAGE_ROWS);
	const gchar *params[3];
	guint i;

	*rows = NULL;
	*total = 0;
	params[0] = source;
	params[1] = offset;
	params[2] = only_unlinked ? "true" : "false";
	if (!axis_db_pool_exec(self->pool,
	        "SELECT b.title_id, coalesce(b.movie_id, b.series_id, 0), b.label, b.file, b.file_detail,"
	        " coalesce(m.search_hold, sr.search_hold), " AXIS_ARR_US("b.brought_at") ", count(*) OVER (), b.counts::text,"
	        " (SELECT count(*) FROM series_episode e WHERE e.series_id = b.series_id AND "
	        AXIS_TV_EPISODE_HELD_SQL("e") " AND e.gone_at IS NULL)"
	        " FROM arr_brought b LEFT JOIN movie m ON m.id = b.movie_id LEFT JOIN series sr ON sr.id = b.series_id"
	        " WHERE b.source_id = $1::bigint"
	        " AND (NOT $3::boolean OR b.file NOT IN ('none', 'linked', 'already'))"
	        " ORDER BY lower(b.label), b.id LIMIT " G_STRINGIFY(AXIS_ARR_PAGE_ROWS) " OFFSET $2::bigint", 3, params,
	        &result, error))
		return FALSE;
	*rows = g_ptr_array_new_with_free_func(brought_row_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisArrBroughtRow *row = g_new0(AxisArrBroughtRow, 1);

		row->title_id = axis_arr_int_at(result, i, 0, 0);
		row->movie_id = axis_arr_int_at(result, i, 1, 0);
		row->label = axis_arr_text_at(result, i, 2);
		row->file = axis_arr_text_at(result, i, 3);
		row->file_detail = axis_arr_text_at(result, i, 4);
		row->hold = axis_arr_text_at(result, i, 5);
		row->brought_us = axis_arr_int_at(result, i, 6, 0);
		*total = (guint)axis_arr_int_at(result, i, 7, 0);
		row->counts = axis_arr_text_at(result, i, 8);
		row->held_episodes = (guint)axis_arr_int_at(result, i, 9, 0);
		g_ptr_array_add(*rows, row);
	}
	return TRUE;
}

/* ── Starting the searches ──────────────────────────────────────── */

void
axis_arr_start_free(AxisArrStart *start)
{
	if (start == NULL)
		return;
	g_clear_pointer(&start->source, axis_arr_source_free);
	g_clear_pointer(&start->in_queue, g_ptr_array_unref);
	g_free(start);
}

/*
 * Radarr's download queue, now: the app's ids of the movies in it.  A9: a
 * record's "movieId" was never seen on a running Radarr (its queue was
 * empty).  So an answer that cannot be read as assumed, or that lists
 * downloads without one movie Axis can name, is %AXIS_ARR_QUEUE_UNCLEAR: said
 * in words on the page, and nothing is held back for it.  Only an app that
 * does not answer (or a connection without its key) needs the person's tick.
 */
static AxisArrQueueCheck
read_queue(
	AxisArr     *self,
	gint64       source_id,
	GHashTable  *ids,
	guint       *listed
){
	g_autoptr(AxisArrSource) source = NULL;
	g_autoptr(GArray) found = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GError) local = NULL;
	g_autofree gchar *version = NULL;
	AxisArrQueueCheck check = AXIS_ARR_QUEUE_READ;
	AxisArrClient *client = NULL;
	AxisArrTuning tuning;
	AxisDbConn *conn;
	gchar *secret = NULL;
	gint64 began = g_get_monotonic_time();
	gboolean ok;
	guint seen = 0;
	guint page;
	guint i;

	*listed = 0;
	axis_arr_get_tuning(self, &tuning);
	conn = axis_db_pool_acquire(self->pool, &local);
	if (conn == NULL)
		return AXIS_ARR_QUEUE_NO_ANSWER;
	ok = axis_arr_store_source_secret(conn, source_id, &source, &secret, &local);
	axis_db_pool_release(self->pool, conn);
	if (!ok || secret == NULL) {
		axis_arr_secret_free(secret);
		return AXIS_ARR_QUEUE_NO_ANSWER;
	}
	client = axis_arr_client_new(self, source, secret, self->cancel, &local);
	axis_arr_secret_free(secret);
	/* Nothing is asked of the address until its answer has named the app. */
	if (client == NULL || !axis_arr_client_status(client, &version, &local)) {
		g_clear_pointer(&client, axis_arr_client_free);
		return AXIS_ARR_QUEUE_NO_ANSWER;
	}
	for (page = 1; page <= AXIS_ARR_MAX_PAGES; page++) {
		g_autofree gchar *path = axis_arr_queue_path(source->app, page, tuning.page_size);
		g_autoptr(GBytes) body = NULL;
		const gchar *why = NULL;
		const gchar *data;
		gsize length = 0;
		guint total = 0;
		guint n = 0;

		g_clear_error(&local);
		if (g_get_monotonic_time() - began > (gint64)tuning.queue_deadline_ms * 1000 ||
		    !axis_arr_client_get(client, path, AXIS_ARR_GET_SMALL, &body, &local)) {
			/* It named itself and then fell silent, or refused the request:
			 * the queue is not known. */
			check = local != NULL && local->code == AXIS_ARR_ERROR_UNREADABLE ? AXIS_ARR_QUEUE_UNCLEAR :
				AXIS_ARR_QUEUE_NO_ANSWER;
			break;
		}
		data = g_bytes_get_data(body, &length);
		if (!axis_arr_parse_queue_page(data, length, source->app, found, &total, &n, &why)) {
			check = AXIS_ARR_QUEUE_UNCLEAR;
			break;
		}
		*listed = MAX(*listed, total);
		seen += n;
		if (n == 0 || seen >= total)
			break;
	}
	axis_arr_client_free(client);
	if (check != AXIS_ARR_QUEUE_READ)
		return check;
	for (i = 0; i < found->len; i++) {
		gint64 *id = g_new(gint64, 1);

		*id = g_array_index(found, gint64, i);
		g_hash_table_add(ids, id);
	}
	/* Downloads are listed, and none names a movie: the shape is not the one
	 * assumed. */
	if (*listed > 0 && g_hash_table_size(ids) == 0)
		return AXIS_ARR_QUEUE_UNCLEAR;
	return AXIS_ARR_QUEUE_READ;
}

/* The review, and with @start_ids and @held_ids (nullable) the movies to
 * start and to hold as downloading.  @only_job: (nullable) only the titles
 * that run brought or linked a file of (arr_brought.run_job). */
static gboolean
start_look(
	AxisArr       *self,
	gint64         source_id,
	const gchar   *only_job,
	AxisArrStart **out,
	GArray        *start_ids,
	GArray        *held_ids,
	GError       **error
){
	g_autoptr(AxisArrStart) start = g_new0(AxisArrStart, 1);
	g_autoptr(GHashTable) queued = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(AxisDbResult) held = NULL;
	g_autoptr(AxisDbResult) unseen = NULL;
	g_autoptr(AxisDbResult) monitored = NULL;
	g_autoptr(GString) titles = g_string_new("{");
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *keep = g_strdup_printf("%d", AXIS_ARR_PLAN_KEEP_S);
	const gchar *params[3];
	gboolean series;
	guint i;

	*out = NULL;
	if (!axis_arr_get_source(self, source_id, &start->source, error))
		return FALSE;
	start->in_queue = g_ptr_array_new_with_free_func(g_free);
	start->n_monitored = -1;
	series = start->source->app == AXIS_ARR_SONARR;
	if (!series && self->movies != NULL)
		axis_movies_search_pace(self->movies, &start->per_run, &start->interval_s);
	if (series && self->tv != NULL)
		axis_tv_search_pace(self->tv, &start->per_run, &start->interval_s);
	params[0] = source;
	params[1] = only_job;
	/* The held titles of the connection's app, each with whether something of
	 * it is missing and monitored (a series: one of its episodes, not held
	 * apart) and whether it has a file. */
	if (!axis_db_pool_exec(self->pool, series ?
	        "SELECT s.id, coalesce(b.arr_id, 0), b.label,"
	        " (SELECT count(*) FROM series_episode e WHERE e.series_id = s.id AND e.gone_at IS NULL AND e.monitored"
	        "   AND e.search_hold IS NULL AND NOT EXISTS (SELECT 1 FROM series_file_episode sfe JOIN library_file lf"
	        "   ON lf.id = sfe.file_id AND lf.state = 'present' WHERE sfe.episode_id = e.id)) > 0, b.title_id,"
	        " EXISTS (SELECT 1 FROM series_file sf JOIN library_file lf ON lf.id = sf.file_id WHERE sf.series_id = s.id"
	        "   AND lf.state = 'present'),"
	        " (SELECT count(*) FROM series_episode e WHERE e.series_id = s.id AND e.gone_at IS NULL AND e.monitored"
	        "   AND e.search_hold IS NULL AND NOT EXISTS (SELECT 1 FROM series_file_episode sfe JOIN library_file lf"
	        "   ON lf.id = sfe.file_id AND lf.state = 'present' WHERE sfe.episode_id = e.id))"
	        " FROM arr_brought b JOIN series s ON s.id = b.series_id WHERE b.source_id = $1::bigint"
	        " AND s.search_hold IN ('" AXIS_TV_HOLD_BROUGHT "', '" AXIS_TV_HOLD_DOWNLOADING "')"
	        " AND ($2::bigint IS NULL OR b.run_job = $2::bigint)"
	        " ORDER BY lower(b.label), s.id" :
	        "SELECT m.id, coalesce(b.arr_id, 0), b.label, m.monitored, b.title_id,"
	        " EXISTS (SELECT 1 FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id WHERE mf.movie_id = m.id"
	        "   AND lf.state = 'present'), 0"
	        " FROM arr_brought b JOIN movie m ON m.id = b.movie_id WHERE b.source_id = $1::bigint"
	        " AND m.search_hold IN ('" AXIS_MOVIE_HOLD_BROUGHT "', '" AXIS_MOVIE_HOLD_DOWNLOADING "')"
	        " AND ($2::bigint IS NULL OR b.run_job = $2::bigint)"
	        " ORDER BY lower(b.label), m.id", 2, params, &held, error) ||
	    !axis_db_pool_exec(self->pool,
	        "SELECT (SELECT count(*) FROM arr_brought b JOIN movie m ON m.id = b.movie_id WHERE b.source_id = $1::bigint"
	        " AND m.search_hold = '" AXIS_MOVIE_HOLD_FILE_UNSEEN "'),"
	        " (SELECT count(*) FROM arr_brought b JOIN series_episode e ON e.series_id = b.series_id"
	        " WHERE b.source_id = $1::bigint AND " AXIS_TV_EPISODE_HELD_SQL("e") " AND e.gone_at IS NULL),"
	        /* Titles of other runs that are still held (only for a run's own
	         * start: a person's start takes them all). */
	        " (SELECT count(*) FROM arr_brought b JOIN movie m ON m.id = b.movie_id WHERE b.source_id = $1::bigint"
	        "   AND m.search_hold IN ('" AXIS_MOVIE_HOLD_BROUGHT "', '" AXIS_MOVIE_HOLD_DOWNLOADING "')"
	        "   AND $2::bigint IS NOT NULL AND b.run_job IS DISTINCT FROM $2::bigint)"
	        " + (SELECT count(*) FROM arr_brought b JOIN series s ON s.id = b.series_id WHERE b.source_id = $1::bigint"
	        "   AND s.search_hold IN ('" AXIS_TV_HOLD_BROUGHT "', '" AXIS_TV_HOLD_DOWNLOADING "')"
	        "   AND $2::bigint IS NOT NULL AND b.run_job IS DISTINCT FROM $2::bigint)", 2, params,
	        &unseen,
	        error))
		return FALSE;
	start->n_other_held = (guint)axis_arr_int_at(unseen, 0, 2, 0);
	start->n_unseen = (guint)axis_arr_int_at(unseen, 0, 0, 0);
	start->n_unseen_episodes = (guint)axis_arr_int_at(unseen, 0, 1, 0);
	/* Nothing to start: Radarr is not asked. */
	if (axis_db_result_get_n_rows(held) > 0)
		start->queue = read_queue(self, source_id, queued, &start->n_queue);
	for (i = 0; i < axis_db_result_get_n_rows(held); i++) {
		gint64 movie_id = axis_arr_int_at(held, i, 0, 0);
		gint64 arr_id = axis_arr_int_at(held, i, 1, 0);

		if (start->queue == AXIS_ARR_QUEUE_READ && arr_id > 0 && g_hash_table_contains(queued, &arr_id)) {
			/* Radarr is downloading it: left out, and held for that. */
			start->n_in_queue++;
			if (start->in_queue->len < NAMED_IN_QUEUE)
				g_ptr_array_add(start->in_queue, axis_arr_text_at(held, i, 2));
			if (held_ids != NULL)
				g_array_append_val(held_ids, movie_id);
			continue;
		}
		start->n_held++;
		if (series) {
			/* A series: missing when a monitored episode of it has no file. */
			if (axis_arr_bool_at(held, i, 3))
				start->n_missing++;
			else if (axis_arr_bool_at(held, i, 5))
				start->n_with_file++;
			start->n_missing_episodes += (guint)axis_arr_int_at(held, i, 6, 0);
		} else if (axis_arr_bool_at(held, i, 5)) {
			start->n_with_file++;
		} else if (axis_arr_bool_at(held, i, 3)) {
			start->n_missing++;
		}
		g_string_append_printf(titles, "%s%" G_GINT64_FORMAT, titles->len > 1 ? "," : "",
			axis_arr_int_at(held, i, 4, 0));
		if (start_ids != NULL)
			g_array_append_val(start_ids, movie_id);
	}
	g_string_append_c(titles, '}');
	/* How many of them Radarr still monitored when it was last read. */
	params[1] = titles->str;
	params[2] = keep;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT p.id, (SELECT count(*) FROM arr_plan_item i WHERE i.plan_id = p.id AND i.monitored"
	        "   AND i.title_id = ANY ($2::bigint[]))"
	        " FROM arr_plan p WHERE p.source_id = $1::bigint AND p.state = 'complete'"
	        " AND p.started_at >= now() - make_interval(secs => $3::integer) ORDER BY p.id DESC LIMIT 1", 3, params,
	        &monitored, error))
		return FALSE;
	if (axis_db_result_get_n_rows(monitored) > 0)
		start->n_monitored = (gint)axis_arr_int_at(monitored, 0, 1, 0);
	*out = g_steal_pointer(&start);
	return TRUE;
}

gboolean
axis_arr_start_review(
	AxisArr       *self,
	gint64         source_id,
	AxisArrStart **start,
	GError       **error
){
	return start_look(self, source_id, NULL, start, NULL, NULL, error);
}

/*
 * "Start searching", for a person's confirmation and for the run's tick
 * alike.  @unattended: nobody is looking at the page (the run's end), so
 * nothing is started unless the app's queue was read: an app that does not
 * answer cannot be vouched for by a tick, and an answer that does not say
 * which titles it is downloading, which a person is warned of before they
 * confirm, has nobody to warn.  Fails then as %AXIS_ARR_ERROR_NEEDS_TICK,
 * with the review.
 */
static gboolean
start_searching(
	AxisArr       *self,
	gint64         source_id,
	gboolean           stopped,
	const Unattended  *unattended,
	const gchar       *user,
	AxisArrStart     **out,
	GError           **error
){
	g_autoptr(AxisArrStart) start = NULL;
	g_autoptr(GArray) start_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) held_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *origin = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean series;
	gboolean ok;

	*out = NULL;
	if (!start_look(self, source_id, unattended != NULL ? unattended->job : NULL, &start, start_ids, held_ids, error))
		return FALSE;
	series = start->source->app == AXIS_ARR_SONARR;
	if (series ? self->tv == NULL : self->movies == NULL) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, series ? "The TV module is not there." :
			"The Movies module is not there.");
		return FALSE;
	}
	if (start->n_held + start->n_in_queue == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, series ? "No series this connection migrated "
			"is held." : "No movie this connection migrated is held.");
		return FALSE;
	}
	if (unattended != NULL && start->queue != AXIS_ARR_QUEUE_READ) {
		*out = g_steal_pointer(&start);
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NEEDS_TICK, "Axis could not read what %s is downloading. "
			"Nothing was started.", axis_arr_app_label((*out)->source->app));
		return FALSE;
	}
	if (start->queue == AXIS_ARR_QUEUE_NO_ANSWER && !stopped) {
		*out = g_steal_pointer(&start);
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NEEDS_TICK, "%s did not answer, so Axis could not check what "
			"it is downloading. Tick the box to start anyway.", axis_arr_app_label((*out)->source->app));
		return FALSE;
	}
	origin = origin_of(start->source);
	params[0] = source;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The holds of the connection's app's titles: a Radarr's movies, a
	 * Sonarr's series. */
	ok = axis_db_conn_begin(conn, error) &&
		(series ? axis_tv_start_brought(self->tv, conn, start_ids, held_ids, origin, user, &start->started, error) :
		axis_movies_start_brought(self->movies, conn, start_ids, held_ids, origin, user, &start->started, error)) &&
		axis_db_conn_exec(conn, "UPDATE arr_source SET search_started_at = now() WHERE id = $1::bigint", 1, params, NULL,
			error);
	if (ok && unattended != NULL) {
		/* The run's note, with the holds it speaks of: both or neither. */
		g_autofree gchar *note = started_note(start, unattended->tail);

		params[1] = note;
		ok = axis_db_conn_exec(conn, "UPDATE arr_source SET run_note = left($2, 1000) WHERE id = $1::bigint", 2, params,
			NULL, error);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*out = g_steal_pointer(&start);
	return ok;
}

gboolean
axis_arr_start_searching(
	AxisArr       *self,
	gint64         source_id,
	gboolean       stopped,
	const gchar   *user,
	AxisArrStart **out,
	GError       **error
){
	return start_searching(self, source_id, stopped, NULL, user, out, error);
}

/* ── Letting the brought files go ───────────────────────────────── */

void
axis_arr_release_free(AxisArrRelease *release)
{
	if (release == NULL)
		return;
	g_clear_pointer(&release->source, axis_arr_source_free);
	g_free(release->digest);
	g_free(release);
}

gboolean
axis_arr_release_review(
	AxisArr         *self,
	gint64           source_id,
	AxisArrRelease **out,
	GError         **error
){
	g_autoptr(AxisArrRelease) release = g_new0(AxisArrRelease, 1);
	g_autoptr(AxisDbResult) counts = NULL;
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *text = NULL;
	const gchar *params[1];

	*out = NULL;
	if (!axis_arr_get_source(self, source_id, &release->source, error))
		return FALSE;
	params[0] = source;
	if (!axis_db_pool_exec(self->pool, release->source->app == AXIS_ARR_SONARR ?
	        "SELECT count(*), count(DISTINCT sf.series_id)" KEPT_SERIES_FILES("b") :
	        "SELECT count(*), count(DISTINCT mf.movie_id)" KEPT_MOVIE_FILES("b"), 1, params, &counts, error))
		return FALSE;
	release->n_files = (guint)axis_arr_int_at(counts, 0, 0, 0);
	release->n_titles = (guint)axis_arr_int_at(counts, 0, 1, 0);
	text = g_strdup_printf("release|%" G_GINT64_FORMAT "|%u|%u", source_id, release->n_files, release->n_titles);
	release->digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
	*out = g_steal_pointer(&release);
	return TRUE;
}

gboolean
axis_arr_release_kept(
	AxisArr         *self,
	gint64           source_id,
	const gchar     *digest,
	const gchar     *user,
	AxisArrRelease **out,
	GError         **error
){
	g_autoptr(AxisArrRelease) release = NULL;
	g_autoptr(AxisDbResult) titles = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *source = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *origin = NULL;
	const gchar *params[1];
	AxisDbConn *conn;
	gboolean series;
	gboolean ok;
	guint i;

	*out = NULL;
	if (!axis_arr_release_review(self, source_id, &release, error))
		return FALSE;
	series = release->source->app == AXIS_ARR_SONARR;
	if (series ? self->tv == NULL : self->movies == NULL) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, series ? "The TV module is not there." :
			"The Movies module is not there.");
		return FALSE;
	}
	/* The page that would be drawn now is not the one that was confirmed. */
	if (digest == NULL || strcmp(release->digest, digest) != 0) {
		*out = g_steal_pointer(&release);
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_STALE, "The files this would let go changed after "
			"this page was made. Nothing was changed: look at it again, then confirm.");
		return FALSE;
	}
	if (release->n_files == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "No file migrated by this connection is kept.");
		return FALSE;
	}
	params[0] = source;
	if (!axis_db_pool_exec(self->pool, series ?
	        "SELECT series_id FROM arr_brought WHERE source_id = $1::bigint AND series_id IS NOT NULL" :
	        "SELECT movie_id FROM arr_brought WHERE source_id = $1::bigint AND movie_id IS NOT NULL", 1, params, &titles,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(titles); i++) {
		gint64 id = axis_arr_int_at(titles, i, 0, 0);

		g_array_append_val(ids, id);
	}
	origin = origin_of(release->source);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The links are the modules': each lets go of its own, and only of those
	 * that are still kept because they were migrated. */
	ok = axis_db_conn_begin(conn, error) &&
		(series ? axis_tv_release_brought(self->tv, conn, ids, origin, user, &release->released, error) :
		axis_movies_release_brought(self->movies, conn, ids, origin, user, &release->released, error)) &&
		axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok)
		*out = g_steal_pointer(&release);
	return ok;
}
