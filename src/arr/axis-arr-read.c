/*
 * axis-arr-read.c - the reading of a person's Radarr or Sonarr into a plan
 * (the job arr.read), the plans, and their clean-up
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The job writes to arr_plan, arr_plan_item (for a Sonarr also
 * arr_plan_episode and arr_plan_file) and arr_source, and to the metadata
 * store as any lookup of a title does.  It writes no movie, series, link,
 * blocklist entry, grab or setting.  It holds no database connection while
 * it waits for the app or for a metadata provider.
 *
 * One job reads either app: the status, the short lists, the download queue,
 * the list of every title, its storing and its matching are one path.  A
 * Sonarr then has the two lists of each matched series read, one series at a
 * time (read_series_lists()), each stored and freed before the next is asked
 * for.  A reading that fails or starts over deletes its items, and with them
 * the episodes and files stored so far.
 *
 * A reading has one attempt: the person who started it is looking at the
 * page, and "Read again" is the retry.  A daemon that stops in the middle
 * hands the job back, and it starts over on the same plan.
 */

#include "arr/axis-arr-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "meta/axis-meta-model.h"

/* Titles stored a statement. */
#define INSERT_BATCH 500

/* Why a Sonarr is not read without a TheTVDB key: the words of the plan, the
 * job and the page. */
#define NO_TVDB_KEY "Axis has no TheTVDB key. Sonarr numbers its series as TheTVDB does: set a TheTVDB key on the " \
	"Metadata page, then read."

void
axis_arr_plan_free(AxisArrPlan *plan)
{
	if (plan == NULL)
		return;
	g_free(plan->state);
	g_free(plan->started_by);
	g_free(plan->problem);
	g_free(plan->progress_message);
	g_free(plan);
}

/* ── Asking for a reading ───────────────────────────────────────── */

gboolean
axis_arr_request_read(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *user,
	gint64       *plan_id,
	GError      **error
){
	g_autoptr(AxisArrSource) source = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *key = g_strdup_printf(AXIS_ARR_READ_KEY_PREFIX "%" G_GINT64_FORMAT, source_id);
	const gchar *params[2];
	AxisJobSpec spec;
	AxisDbConn *conn;
	gboolean created = FALSE;
	gboolean ok = FALSE;
	gint64 plan = 0;

	if (plan_id != NULL)
		*plan_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_arr_store_source(conn, source_id, &source, error))
		goto out;
	if (!source->secret_set) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID, "No API key is stored for this connection.");
		goto out;
	}
	/* Nothing is asked of a Sonarr, and no plan is made, while its series
	 * could not be brought. */
	if (source->app == AXIS_ARR_SONARR && !axis_arr_series_ready(self)) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY, NO_TVDB_KEY);
		goto out;
	}
	if (!axis_db_conn_begin(conn, error))
		goto out;
	params[0] = id_text;
	params[1] = user != NULL && user[0] != '\0' ? user : NULL;
	/* A reading left behind (its job gone without a word) is over, and what
	 * a plan that did not complete kept is no use to anyone. */
	if (!axis_db_conn_exec(conn,
	        "DELETE FROM arr_plan_item WHERE plan_id IN (SELECT id FROM arr_plan WHERE source_id = $1::bigint"
	        " AND state <> 'complete')", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn,
	        "UPDATE arr_plan SET state = 'failed', finished_at = now(), problem = 'The reading did not finish.'"
	        " WHERE source_id = $1::bigint AND state = 'reading'", 1, params, NULL, error) ||
	    !axis_db_conn_exec(conn,
	        "INSERT INTO arr_plan (source_id, started_by) VALUES ($1::bigint, $2) RETURNING id", 2, params, &result,
	        error))
		goto rollback;
	plan = axis_arr_int_at(result, 0, 0, 0);
	/* The payload holds the two numbers and nothing else: never the key. */
	json_object_set_int_member(payload, "source", source_id);
	json_object_set_int_member(payload, "plan", plan);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_ARR_READ_KIND;
	spec.queue = AXIS_META_QUEUE;
	spec.payload = payload;
	spec.dedupe_key = key;
	spec.max_attempts = 1;
	if (!axis_job_enqueue(conn, &spec, NULL, &created, error))
		goto rollback;
	if (!created) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_BUSY, "A reading of this connection is already under way.");
		goto rollback;
	}
	ok = axis_db_conn_commit(conn, error);
	if (ok && plan_id != NULL)
		*plan_id = plan;
	goto out;

rollback:
	axis_db_conn_rollback(conn, NULL);
out:
	axis_db_pool_release(self->pool, conn);
	return ok;
}

/* ── The plans of a connection ──────────────────────────────────── */

static const gchar *const plan_sql =
	"SELECT p.id, p.source_id, p.state, " AXIS_ARR_US("p.started_at") ", " AXIS_ARR_US("p.finished_at") ","
	" p.started_by, p.problem, p.n_items, (p.started_at < now() - make_interval(secs => $2::integer)),"
	" j.id, j.progress, j.progress_message"
	" FROM arr_plan p"
	" LEFT JOIN LATERAL (SELECT j.id, j.progress, j.progress_message FROM job j"
	"   WHERE j.dedupe_key = '" AXIS_ARR_READ_KEY_PREFIX "' || p.source_id::text"
	"   AND j.state IN ('queued', 'running') AND j.payload ->> 'plan' = p.id::text LIMIT 1) j ON true"
	" WHERE p.source_id = $1::bigint";

static AxisArrPlan *
plan_of(
	AxisDbResult *result,
	guint         row
){
	AxisArrPlan *plan = g_new0(AxisArrPlan, 1);
	const gchar *progress = axis_db_result_get_text(result, row, 10);

	plan->id = axis_arr_int_at(result, row, 0, 0);
	plan->source_id = axis_arr_int_at(result, row, 1, 0);
	plan->state = axis_arr_text_at(result, row, 2);
	plan->started_us = axis_arr_int_at(result, row, 3, 0);
	plan->finished_us = axis_arr_int_at(result, row, 4, 0);
	plan->started_by = axis_arr_text_at(result, row, 5);
	plan->problem = axis_arr_text_at(result, row, 6);
	plan->n_items = (guint)axis_arr_int_at(result, row, 7, 0);
	plan->expired = axis_arr_bool_at(result, row, 8);
	plan->job_id = axis_arr_int_at(result, row, 9, 0);
	plan->job_live = plan->job_id > 0;
	plan->progress = progress != NULL ? CLAMP(g_ascii_strtod(progress, NULL), 0.0, 1.0) : -1.0;
	plan->progress_message = axis_arr_text_at(result, row, 11);
	return plan;
}

gboolean
axis_arr_plans(
	AxisArr      *self,
	gint64        source_id,
	AxisArrPlan **latest,
	AxisArrPlan **usable,
	GError      **error
){
	g_autoptr(AxisDbResult) newest = NULL;
	g_autoptr(AxisDbResult) complete = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, source_id);
	g_autofree gchar *newest_sql = g_strconcat(plan_sql, " ORDER BY p.id DESC LIMIT 1", NULL);
	g_autofree gchar *complete_sql = g_strconcat(plan_sql, " AND p.state = 'complete'"
		" AND p.started_at >= now() - make_interval(secs => $2::integer) ORDER BY p.id DESC LIMIT 1", NULL);
	g_autofree gchar *keep = g_strdup_printf("%d", AXIS_ARR_PLAN_KEEP_S);
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean ok;

	*latest = NULL;
	if (usable != NULL)
		*usable = NULL;
	params[0] = id_text;
	params[1] = keep;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_db_conn_exec(conn, newest_sql, 2, params, &newest, error) &&
		(usable == NULL || axis_db_conn_exec(conn, complete_sql, 2, params, &complete, error));
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	if (axis_db_result_get_n_rows(newest) > 0)
		*latest = plan_of(newest, 0);
	if (usable != NULL && axis_db_result_get_n_rows(complete) > 0)
		*usable = plan_of(complete, 0);
	return TRUE;
}

gboolean
axis_arr_run_cleanup(
	AxisArr  *self,
	GError  **error
){
	g_autofree gchar *keep = g_strdup_printf("%d", AXIS_ARR_PLAN_KEEP_S);
	const gchar *params[1];

	params[0] = keep;
	/* A reading still at work is its job's; one a day old and more, with no
	 * job left, was abandoned. */
	return axis_db_pool_exec(self->pool,
		"DELETE FROM arr_plan p WHERE p.started_at < now() - make_interval(secs => $1::integer)"
		" AND NOT (p.state = 'reading' AND EXISTS (SELECT 1 FROM job j"
		"   WHERE j.dedupe_key = '" AXIS_ARR_READ_KEY_PREFIX "' || p.source_id::text"
		"   AND j.state IN ('queued', 'running')))", 1, params, NULL, error);
}

/* ── The job ────────────────────────────────────────────────────── */

typedef struct
{
	AxisArr        *self;
	AxisJobContext *context;
	gint64          source_id;
	gint64          plan_id;
	AxisArrSource  *source;
	AxisArrClient  *client;
	AxisArrTuning   tuning;
	const gchar    *app;	/* its label */
	gchar          *version;
	GPtrArray      *profiles;	/* AxisArrProfile */
	GPtrArray      *folders;	/* AxisArrFolder */
	gboolean        sonarr;	/* the titles are series */
	GPtrArray      *titles;	/* AxisArrMovie, or AxisArrSeries */
	guint           n_titles;
	GHashTable     *queued;	/* gint64 * of the titles the app is downloading (something of) */
	/* Sonarr's lists for each series. */
	gint64          last_request_us;
	guint64         series_bytes;
	guint           n_episodes;
	guint           n_files;
	guint           n_gave_up;	/* series in a row that ended incomplete for want of an answer */
	gint64          started_us;	/* the reading's one deadline runs from here */
} Reading;

static void
reading_clear(Reading *reading)
{
	g_clear_pointer(&reading->source, axis_arr_source_free);
	g_clear_pointer(&reading->client, axis_arr_client_free);
	g_clear_pointer(&reading->version, g_free);
	g_clear_pointer(&reading->profiles, g_ptr_array_unref);
	g_clear_pointer(&reading->folders, g_ptr_array_unref);
	g_clear_pointer(&reading->titles, g_ptr_array_unref);
	g_clear_pointer(&reading->queued, g_hash_table_unref);
}

static gboolean
stopped(
	Reading  *reading,
	GError  **error
){
	if (axis_job_context_is_cancelled(reading->context)) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_CANCELLED, "The reading was stopped.");
		return TRUE;
	}
	/* One deadline for the whole reading, matching included: the lookups share
	 * the metadata queue with other jobs. */
	if (reading->started_us > 0 &&
	    g_get_monotonic_time() - reading->started_us > (gint64)reading->tuning.series_deadline_ms * 1000) {
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED, "%s took too long to read: the whole reading passed its "
			"time limit. Read again.", reading->app);
		return TRUE;
	}
	return FALSE;
}

static gboolean
unreadable(
	Reading      *reading,
	const gchar  *what,
	const gchar  *why,
	GError      **error
){
	/* @why is one of the parser's fixed fragments. */
	g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_UNREADABLE, "%s's %s %s.", reading->app, what,
		why != NULL ? why : "cannot be read");
	return FALSE;
}

/* Whether the connection is still there, before a request to the app: a
 * connection forgotten while the reading runs ends it. */
static gboolean
connection_kept(
	Reading  *reading,
	GError  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->source_id);
	const gchar *params[1];

	params[0] = source_text;
	if (!axis_db_pool_exec(reading->self->pool, "SELECT 1 FROM arr_source WHERE id = $1::bigint", 1, params, &result,
	        error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "The connection was forgotten.");
		return FALSE;
	}
	return TRUE;
}

/* The connection and its key; the plan emptied for this run. */
static gboolean
reading_begin(
	Reading  *reading,
	GError  **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->source_id);
	const gchar *params[2];
	AxisDbConn *conn;
	gchar *secret = NULL;
	gboolean ok;

	params[0] = plan_text;
	params[1] = source_text;
	conn = axis_db_pool_acquire(reading->self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_arr_store_source_secret(conn, reading->source_id, &reading->source, &secret, error) &&
		axis_db_conn_exec(conn, "DELETE FROM arr_plan_item WHERE plan_id = $1::bigint", 1, params, NULL, error) &&
		axis_db_conn_exec(conn,
			"UPDATE arr_plan SET state = 'reading', finished_at = NULL, problem = NULL, n_items = 0"
			" WHERE id = $1::bigint AND source_id = $2::bigint", 2, params, &result, error);
	axis_db_pool_release(reading->self->pool, conn);
	if (ok && axis_db_result_get_affected_rows(result) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "The reading's plan is gone.");
		ok = FALSE;
	}
	if (ok) {
		reading->app = axis_arr_app_label(reading->source->app);
		reading->sonarr = reading->source->app == AXIS_ARR_SONARR;
		/* The key went after the reading was asked for: Sonarr is not asked. */
		if (reading->sonarr && !axis_arr_series_ready(reading->self)) {
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY, NO_TVDB_KEY);
			ok = FALSE;
		}
	}
	if (ok) {
		reading->client = axis_arr_client_new(reading->self, reading->source, secret,
			axis_job_context_get_cancellable(reading->context), error);
		ok = reading->client != NULL;
	}
	axis_arr_secret_free(secret);
	return ok;
}

/* The short lists and the download queue. */
static gboolean
read_lists(
	Reading  *reading,
	GError  **error
){
	g_autoptr(GBytes) profiles = NULL;
	g_autoptr(GBytes) folders = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	const gchar *why = NULL;
	const gchar *data;
	gint64 started;
	gsize length = 0;
	guint seen = 0;
	guint page;
	guint i;

	if (!connection_kept(reading, error) ||
	    !axis_arr_client_get(reading->client, AXIS_ARR_PROFILES_PATH, AXIS_ARR_GET_SMALL, &profiles, error))
		return FALSE;
	data = g_bytes_get_data(profiles, &length);
	if (!axis_arr_parse_profiles(data, length, &reading->profiles, &why))
		return unreadable(reading, "list of quality profiles", why, error);
	if (stopped(reading, error) || !connection_kept(reading, error) ||
	    !axis_arr_client_get(reading->client, AXIS_ARR_FOLDERS_PATH, AXIS_ARR_GET_SMALL, &folders, error))
		return FALSE;
	data = g_bytes_get_data(folders, &length);
	if (!axis_arr_parse_folders(data, length, &reading->folders, &why))
		return unreadable(reading, "list of root folders", why, error);

	/* What the app is downloading now, page by page, up to the count it
	 * states. */
	started = g_get_monotonic_time();
	for (page = 1; page <= AXIS_ARR_MAX_PAGES; page++) {
		g_autofree gchar *path = axis_arr_queue_path(reading->source->app, page, reading->tuning.page_size);
		g_autoptr(GBytes) body = NULL;
		guint total = 0;
		guint n = 0;

		if (stopped(reading, error))
			return FALSE;
		/* One deadline for all the pages: an app that answers each page just
		 * in time does not hold the reading for hours. */
		if (g_get_monotonic_time() - started > (gint64)reading->tuning.queue_deadline_ms * 1000) {
			g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_ANSWER, "%s took too long to list what it is downloading.",
				reading->app);
			return FALSE;
		}
		if (!connection_kept(reading, error) ||
		    !axis_arr_client_get(reading->client, path, AXIS_ARR_GET_SMALL, &body, error))
			return FALSE;
		data = g_bytes_get_data(body, &length);
		if (!axis_arr_parse_queue_page(data, length, reading->source->app, ids, &total, &n, &why))
			return unreadable(reading, "download queue", why, error);
		seen += n;
		if (n == 0 || seen >= total)
			break;
	}
	reading->queued = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < ids->len; i++) {
		gint64 *id = g_new(gint64, 1);

		*id = g_array_index(ids, gint64, i);
		g_hash_table_add(reading->queued, id);
	}
	return TRUE;
}

/* The list of every movie, or of every series. */
static gboolean
read_titles(
	Reading  *reading,
	GError  **error
){
	g_autoptr(GBytes) body = NULL;
	const gchar *why = NULL;
	const gchar *data;
	gsize length = 0;

	if (!connection_kept(reading, error) ||
	    !axis_arr_client_get(reading->client, reading->sonarr ? AXIS_ARR_SERIES_PATH : AXIS_ARR_MOVIES_PATH,
	        AXIS_ARR_GET_LIBRARY, &body, error))
		return FALSE;
	data = g_bytes_get_data(body, &length);
	/* Read once, reduced to what is kept, and the answer freed with @body. */
	if (reading->sonarr) {
		if (!axis_arr_parse_series(data, length, AXIS_ARR_MAX_TITLES, &reading->titles, &why))
			return unreadable(reading, "list of series", why, error);
	} else if (!axis_arr_parse_movies(data, length, AXIS_ARR_MAX_TITLES, &reading->titles, &why)) {
		return unreadable(reading, "list of movies", why, error);
	}
	reading->n_titles = reading->titles->len;
	return TRUE;
}

static void
set_text(
	JsonObject  *object,
	const gchar *name,
	const gchar *value
){
	if (value != NULL)
		json_object_set_string_member(object, name, value);
}

static gchar *
object_to_text(JsonNode *node)
{
	g_autoptr(JsonGenerator) generator = json_generator_new();

	json_generator_set_root(generator, node);
	return json_generator_to_data(generator, NULL);
}

/* One statement stores a movie or a series: what only one of them has is
 * absent from the other's rows. */
static const gchar *const insert_sql =
	"INSERT INTO arr_plan_item (plan_id, seq, arr_id, title, year, tmdb_id, imdb_id, monitored, min_availability,"
	" profile_id, path, root_path, has_file, file_path, file_size, file_quality, file_scene, in_queue, match,"
	" tvdb_id, series_type, monitor_new, seasons)"
	" SELECT $1::bigint, r.seq, r.arr_id, r.title, r.year, r.tmdb_id, r.imdb_id, coalesce(r.monitored, false),"
	" r.min_availability, r.profile_id, r.path, r.root_path, coalesce(r.has_file, false), r.file_path, r.file_size,"
	" r.file_quality, r.file_scene, coalesce(r.in_queue, false), r.match, r.tvdb_id, r.series_type, r.monitor_new,"
	" r.seasons"
	" FROM jsonb_to_recordset($2::jsonb) AS r(seq integer, arr_id bigint, title text, year integer, tmdb_id bigint,"
	" imdb_id text, monitored boolean, min_availability text, profile_id bigint, path text, root_path text,"
	" has_file boolean, file_path text, file_size bigint, file_quality text, file_scene text, in_queue boolean,"
	" match text, tvdb_id bigint, series_type text, monitor_new boolean, seasons jsonb)";

static void
movie_row(
	Reading            *reading,
	const AxisArrMovie *movie,
	JsonObject         *row
){
	if (movie->arr_id > 0)
		json_object_set_int_member(row, "arr_id", movie->arr_id);
	set_text(row, "title", movie->title);
	if (movie->year > 0)
		json_object_set_int_member(row, "year", movie->year);
	if (movie->tmdb_id > 0)
		json_object_set_int_member(row, "tmdb_id", movie->tmdb_id);
	set_text(row, "imdb_id", movie->imdb_id);
	json_object_set_boolean_member(row, "monitored", movie->monitored);
	set_text(row, "min_availability", movie->min_availability);
	if (movie->profile_id > 0)
		json_object_set_int_member(row, "profile_id", movie->profile_id);
	set_text(row, "path", movie->path);
	set_text(row, "root_path", movie->root_path);
	json_object_set_boolean_member(row, "has_file", movie->has_file);
	set_text(row, "file_path", movie->file_path);
	if (movie->file_size >= 0)
		json_object_set_int_member(row, "file_size", movie->file_size);
	set_text(row, "file_quality", movie->file_quality);
	set_text(row, "file_scene", movie->file_scene);
	json_object_set_boolean_member(row, "in_queue", movie->arr_id > 0 &&
		g_hash_table_contains(reading->queued, &movie->arr_id));
	json_object_set_string_member(row, "match", !movie->usable ? "incomplete" :
		movie->tmdb_id <= 0 ? "no_id" : "pending");
}

static void
series_row(
	Reading             *reading,
	const AxisArrSeries *series,
	JsonObject          *row
){
	JsonArray *seasons = json_array_new();
	guint i;

	if (series->arr_id > 0)
		json_object_set_int_member(row, "arr_id", series->arr_id);
	set_text(row, "title", series->title);
	if (series->year > 0)
		json_object_set_int_member(row, "year", series->year);
	if (series->tvdb_id > 0)
		json_object_set_int_member(row, "tvdb_id", series->tvdb_id);
	set_text(row, "imdb_id", series->imdb_id);
	json_object_set_boolean_member(row, "monitored", series->monitored);
	if (series->profile_id > 0)
		json_object_set_int_member(row, "profile_id", series->profile_id);
	set_text(row, "path", series->path);
	set_text(row, "root_path", series->root_path);
	json_object_set_boolean_member(row, "in_queue", series->arr_id > 0 &&
		g_hash_table_contains(reading->queued, &series->arr_id));
	json_object_set_string_member(row, "match", !series->usable ? "incomplete" :
		series->tvdb_id <= 0 ? "no_id" : "pending");
	set_text(row, "series_type", series->series_type);
	if (series->monitor_new >= 0)
		json_object_set_boolean_member(row, "monitor_new", series->monitor_new > 0);
	/* Each season's monitoring, as Sonarr has it. */
	for (i = 0; i < series->seasons->len; i++) {
		const AxisArrSeason *season = &g_array_index(series->seasons, AxisArrSeason, i);
		JsonObject *entry = json_object_new();

		json_object_set_int_member(entry, "n", season->number);
		json_object_set_boolean_member(entry, "m", season->monitored);
		json_array_add_object_element(seasons, entry);
	}
	json_object_set_array_member(row, "seasons", seasons);
}

static gboolean
store_titles(
	Reading  *reading,
	GError  **error
){
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	guint done = 0;

	while (done < reading->titles->len) {
		g_autoptr(JsonArray) rows = json_array_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
		g_autofree gchar *text = NULL;
		const gchar *params[2];
		guint end = MIN(done + INSERT_BATCH, reading->titles->len);
		guint i;

		for (i = done; i < end; i++) {
			JsonObject *row = json_object_new();

			json_object_set_int_member(row, "seq", (gint64)i + 1);
			if (reading->sonarr)
				series_row(reading, g_ptr_array_index(reading->titles, i), row);
			else
				movie_row(reading, g_ptr_array_index(reading->titles, i), row);
			json_array_add_object_element(rows, row);
		}
		json_node_set_array(node, rows);
		text = object_to_text(node);
		params[0] = plan_text;
		params[1] = text;
		if (stopped(reading, error) ||
		    !axis_db_pool_exec(reading->self->pool, insert_sql, 2, params, NULL, error))
			return FALSE;
		done = end;
		axis_job_context_set_progress(reading->context, 0.15 + 0.15 * done / MAX(reading->titles->len, 1),
			"Storing what was read");
	}
	/* What was read is in the plan: the list itself is not needed again. */
	g_ptr_array_set_size(reading->titles, 0);
	return TRUE;
}

typedef enum
{
	LOOK_MATCHED,
	LOOK_UNKNOWN,		/* the provider does not know the identifier */
	LOOK_NOT_ANSWERED
} Look;

/* The stored title of TMDb identifier @tmdb_id, asking the provider. */
static Look
look_up(
	Reading *reading,
	gint64   tmdb_id,
	gint64  *title_id
){
	g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, tmdb_id);
	g_autoptr(GError) local = NULL;
	const gchar *provider = reading->tuning.provider;
	Look look = LOOK_UNKNOWN;
	guint i;

	*title_id = 0;
	if (!axis_meta_id_valid("tmdb", value))
		return LOOK_UNKNOWN;
	if (strcmp(provider, "tmdb") == 0) {
		AxisMetaTitleAnswer answer;

		memset(&answer, 0, sizeof(answer));
		if (!axis_meta_lookup(reading->self->meta, provider, AXIS_META_KIND_FILM, value, reading->tuning.lookup_wait_ms,
		        &answer, &local))
			return g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND) ? LOOK_UNKNOWN :
				LOOK_NOT_ANSWERED;
		if (answer.title != NULL && answer.title->kind == AXIS_META_KIND_FILM && answer.title->id > 0) {
			*title_id = answer.title->id;
			look = LOOK_MATCHED;
		} else if (!answer.not_found && answer.saved && answer.fetched_us == 0) {
			look = LOOK_NOT_ANSWERED;
		}
		axis_meta_title_answer_clear(&answer);
		return look;
	}
	{
		AxisMetaSearchAnswer answer;

		memset(&answer, 0, sizeof(answer));
		if (!axis_meta_find(reading->self->meta, provider, AXIS_META_KIND_FILM, "tmdb", value,
		        reading->tuning.lookup_wait_ms, &answer, &local))
			return LOOK_NOT_ANSWERED;
		if (answer.saved && answer.fetched_us == 0)
			look = LOOK_NOT_ANSWERED;
		for (i = 0; answer.titles != NULL && i < answer.titles->len; i++) {
			const AxisMetaTitle *found = g_ptr_array_index(answer.titles, i);

			if (found->kind == AXIS_META_KIND_FILM && found->id > 0) {
				*title_id = found->id;
				look = LOOK_MATCHED;
				break;
			}
		}
		axis_meta_search_answer_clear(&answer);
	}
	return look;
}

/* The stored series the details provider knows under TheTVDB identifier
 * @tvdb_id: the title Axis keeps the series under (design.md, section 6).
 * TheTVDB itself is never asked for a title: it makes none of its own. */
static Look
look_up_series(
	Reading *reading,
	gint64   tvdb_id,
	gint64  *title_id
){
	g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, tvdb_id);
	g_autoptr(GError) local = NULL;
	AxisMetaSearchAnswer answer;
	Look look = LOOK_UNKNOWN;
	guint i;

	*title_id = 0;
	if (!axis_meta_id_valid("tvdb", value))
		return LOOK_UNKNOWN;
	memset(&answer, 0, sizeof(answer));
	if (!axis_meta_find(reading->self->meta, reading->tuning.provider, AXIS_META_KIND_SERIES, "tvdb", value,
	        reading->tuning.lookup_wait_ms, &answer, &local))
		return LOOK_NOT_ANSWERED;
	if (answer.saved && answer.fetched_us == 0)
		look = LOOK_NOT_ANSWERED;
	for (i = 0; answer.titles != NULL && i < answer.titles->len; i++) {
		const AxisMetaTitle *found = g_ptr_array_index(answer.titles, i);

		if (found->kind == AXIS_META_KIND_SERIES && found->id > 0) {
			*title_id = found->id;
			look = LOOK_MATCHED;
			break;
		}
	}
	axis_meta_search_answer_clear(&answer);
	return look;
}

/* Whether TheTVDB's aired order, which a series from Sonarr follows, lists
 * the stored series @title_id: asks TheTVDB for the order when the store has
 * none of it (the whole order is fetched and kept, as adding the series
 * would).  %LOOK_UNKNOWN: it lists nothing for it. */
static Look
look_up_order(
	Reading *reading,
	gint64   title_id
){
	g_autoptr(AxisDbResult) held = NULL;
	g_autofree gchar *title_text = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autoptr(GError) local = NULL;
	g_autoptr(GError) ignored = NULL;
	AxisMetaSeasonAnswer answer;
	const gchar *params[1];
	gboolean silent;

	memset(&answer, 0, sizeof(answer));
	if (!axis_meta_get_source_season(reading->self->meta, title_id, AXIS_ARR_SERIES_SOURCE, 1,
	        reading->tuning.lookup_wait_ms, &answer, &local))
		return g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND) ? LOOK_UNKNOWN : LOOK_NOT_ANSWERED;
	silent = answer.saved && !answer.not_found;
	axis_meta_season_answer_clear(&answer);
	/* Whatever season was asked for: the order lists the series when the
	 * store holds a season of it. */
	params[0] = title_text;
	if (!axis_db_pool_exec(reading->self->pool,
	        "SELECT 1 FROM meta_season WHERE title_id = $1::bigint AND source = '" AXIS_ARR_SERIES_SOURCE "' LIMIT 1", 1,
	        params, &held, &ignored))
		return LOOK_NOT_ANSWERED;
	if (axis_db_result_get_n_rows(held) > 0)
		return LOOK_MATCHED;
	return silent ? LOOK_NOT_ANSWERED : LOOK_UNKNOWN;
}

/* What the store has, in one go.  A movie: its stored title by its TMDb
 * identifier.  A series: its stored title by its TheTVDB identifier (the
 * title of the details provider, never one TheTVDB alone made), and matched
 * at once when the store holds TheTVDB's aired order of it; one with its
 * title and no order yet stays pending, with its title, for the question to
 * TheTVDB.  A series Sonarr has as anime is matched, asked about and read as
 * any other (add-anime-support, A1).  One of a kind Axis does not know (its
 * type is not said) is asked about nowhere: it is not brought. */
static gboolean
match_from_store(
	Reading  *reading,
	GError  **error
){
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	const gchar *params[2];

	params[0] = plan_text;
	params[1] = reading->tuning.provider;
	if (!reading->sonarr)
		return axis_db_pool_exec(reading->self->pool,
			"UPDATE arr_plan_item i SET title_id = e.title_id, match = 'matched' FROM meta_external_id e"
			" WHERE i.plan_id = $1::bigint AND i.match = 'pending' AND e.kind = 'film' AND e.scheme = 'tmdb'"
			" AND e.value = i.tmdb_id::text", 1, params, NULL, error);
	return axis_db_pool_exec(reading->self->pool,
			"UPDATE arr_plan_item SET match = 'not_looked_up' WHERE plan_id = $1::bigint AND match = 'pending'"
			" AND series_type IS NULL", 1, params, NULL, error) &&
		axis_db_pool_exec(reading->self->pool,
			"UPDATE arr_plan_item i SET title_id = e.title_id FROM meta_external_id e"
			" JOIN meta_title t ON t.id = e.title_id AND t.kind = 'series' AND t.provider = $2"
			" WHERE i.plan_id = $1::bigint AND i.match = 'pending' AND e.kind = 'series' AND e.scheme = 'tvdb'"
			" AND e.value = i.tvdb_id::text", 2, params, NULL, error) &&
		axis_db_pool_exec(reading->self->pool,
			"UPDATE arr_plan_item i SET match = 'matched' WHERE i.plan_id = $1::bigint AND i.match = 'pending'"
			" AND i.title_id IS NOT NULL AND EXISTS (SELECT 1 FROM meta_season s WHERE s.title_id = i.title_id"
			" AND s.source = '" AXIS_ARR_SERIES_SOURCE "')", 1, params, NULL, error);
}

/* Each title to its stored title: from the store in one go, then the
 * providers for the rest, one at a time within their limits; the job's
 * progress runs from @from over @span. */
static gboolean
match_titles(
	Reading  *reading,
	gdouble   from,
	gdouble   span,
	GError  **error
){
	g_autoptr(AxisDbResult) pending = NULL;
	g_autoptr(AxisDbResult) updated = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	g_autofree gchar *matching = g_strdup_printf("Matching the %s to titles Axis has",
		axis_arr_app_noun(reading->source->app, TRUE));
	const gchar *noun = axis_arr_app_noun(reading->source->app, FALSE);
	const gchar *params[4];
	guint misses = 0;
	guint asked = 0;
	guint n;
	guint i;

	params[0] = plan_text;
	axis_job_context_set_progress(reading->context, from, matching);
	if (!match_from_store(reading, error) ||
	    !axis_db_pool_exec(reading->self->pool, reading->sonarr ?
	        "SELECT seq, tvdb_id, title_id FROM arr_plan_item WHERE plan_id = $1::bigint AND match = 'pending' ORDER BY seq" :
	        "SELECT seq, tmdb_id, NULL::bigint FROM arr_plan_item WHERE plan_id = $1::bigint AND match = 'pending'"
	        " ORDER BY seq", 1, params, &pending, error))
		return FALSE;
	n = axis_db_result_get_n_rows(pending);
	/* At most so many questions are asked of the providers in one reading,
	 * whether they know the titles or not; the rest wait for the next
	 * reading. */
	for (i = 0; i < n && misses < reading->tuning.lookup_give_up && asked < reading->tuning.lookup_max; i++) {
		g_autofree gchar *seq_text = axis_arr_text_at(pending, i, 0);
		g_autofree gchar *title_text = NULL;
		g_autofree gchar *message = g_strdup_printf("Looking up %s %u of %u", noun, i + 1, n);
		gint64 title_id = axis_arr_int_at(pending, i, 2, 0);
		const gchar *match;
		Look look = LOOK_MATCHED;

		if (stopped(reading, error))
			return FALSE;
		axis_job_context_set_progress(reading->context, from + 0.03 + (span - 0.03) * i / n, message);
		if (!reading->sonarr) {
			asked++;
			look = look_up(reading, axis_arr_int_at(pending, i, 1, 0), &title_id);
		} else if (title_id <= 0) {
			asked++;
			look = look_up_series(reading, axis_arr_int_at(pending, i, 1, 0), &title_id);
		}
		match = look == LOOK_MATCHED ? "matched" : "unknown";
		/* A series whose title is known: does the numbering it would follow
		 * list it? */
		if (reading->sonarr && look == LOOK_MATCHED) {
			asked++;
			look = look_up_order(reading, title_id);
			match = look == LOOK_MATCHED ? "matched" : "not_listed";
		}
		if (look == LOOK_NOT_ANSWERED) {
			misses++;
			continue;
		}
		misses = 0;
		title_text = title_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, title_id) : NULL;
		params[1] = seq_text;
		params[2] = title_text;
		params[3] = match;
		if (!axis_db_pool_exec(reading->self->pool,
		        "UPDATE arr_plan_item SET title_id = $3::bigint, match = $4"
		        " WHERE plan_id = $1::bigint AND seq = $2::integer", 4, params, &updated, error))
			return FALSE;
		/* No row: the plan went (the connection was forgotten) and nothing
		 * is left to read for. */
		if (axis_db_result_get_affected_rows(updated) == 0) {
			g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "The reading's plan is gone.");
			return FALSE;
		}
		g_clear_pointer(&updated, axis_db_result_free);
	}
	/* Whatever was not answered (and what was not asked once the provider
	 * fell silent): read again later, and the store has them by then. */
	return axis_db_pool_exec(reading->self->pool,
		"UPDATE arr_plan_item SET match = 'not_looked_up' WHERE plan_id = $1::bigint AND match = 'pending'",
		1, params, NULL, error);
}

/* ── Sonarr: the two lists of each series ───────────────────────── */

/* One series' episodes, its files and what they came to, in one statement:
 * all of it or none.  Nothing is written when the item is gone. */
static const gchar *const parts_sql =
	"WITH e AS (INSERT INTO arr_plan_episode (plan_id, seq, ord, arr_id, tvdb_id, season, episode, air_date, monitored,"
	"   file_arr_id, odd, absolute_number)"
	"  SELECT $1::bigint, $2::integer, r.o, r.a, r.t, r.s, r.e, r.d, r.m, r.f, coalesce(r.x, false), r.b"
	"  FROM jsonb_to_recordset($3::jsonb) AS r(o integer, a bigint, t bigint, s integer, e integer, d date, m boolean,"
	"   f bigint, x boolean, b integer)"
	"  WHERE EXISTS (SELECT 1 FROM arr_plan_item i WHERE i.plan_id = $1::bigint AND i.seq = $2::integer)),"
	" f AS (INSERT INTO arr_plan_file (plan_id, seq, ord, arr_id, path, size, quality, scene)"
	"  SELECT $1::bigint, $2::integer, r.o, r.a, r.p, r.z, r.q, r.n"
	"  FROM jsonb_to_recordset($4::jsonb) AS r(o integer, a bigint, p text, z bigint, q text, n text)"
	"  WHERE EXISTS (SELECT 1 FROM arr_plan_item i WHERE i.plan_id = $1::bigint AND i.seq = $2::integer))"
	" UPDATE arr_plan_item SET n_episodes = $5::integer, n_episodes_monitored = $6::integer,"
	" n_episodes_with_file = $7::integer, n_files = $8::integer, n_odd = $9::integer"
	" WHERE plan_id = $1::bigint AND seq = $2::integer";

/* One of the two lists of a series: not sooner after the last than the pace
 * allows, and counted against the reading's bytes. */
static gboolean
get_series_list(
	Reading      *reading,
	const gchar  *path,
	GBytes      **body,
	GError      **error
){
	gint64 gap = G_USEC_PER_SEC / MAX(reading->tuning.requests_per_second, 1);
	gint64 now = g_get_monotonic_time();

	if (reading->last_request_us > 0 && now - reading->last_request_us < gap)
		g_usleep((gulong)(gap - (now - reading->last_request_us)));
	if (stopped(reading, error))
		return FALSE;
	reading->last_request_us = g_get_monotonic_time();
	if (!axis_arr_client_get(reading->client, path, AXIS_ARR_GET_SERIES, body, error))
		return FALSE;
	reading->series_bytes += g_bytes_get_size(*body);
	if (reading->series_bytes > reading->tuning.series_total_bytes) {
		g_clear_pointer(body, g_bytes_unref);
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_TOO_LARGE, "%s's library is too large for this version of "
			"Axis to migrate (its lists of episodes are over %u MB in all).", reading->app,
			(guint)(reading->tuning.series_total_bytes / (1024 * 1024)));
		return FALSE;
	}
	return TRUE;
}

/* get_series_list() for one series' list: a list that does not answer is asked
 * for once more after a pause, and if it still does not, @gave_up is set and
 * the call succeeds with no body (that one series is incomplete).  Anything
 * else that fails (stopped, connection gone, too large, key refused, not the
 * app) fails the reading. */
static gboolean
get_series_list_patiently(
	Reading      *reading,
	const gchar  *path,
	GBytes      **body,
	gboolean     *gave_up,
	GError      **error
){
	g_autoptr(GError) local = NULL;
	guint waited;

	*gave_up = FALSE;
	if (get_series_list(reading, path, body, &local))
		return TRUE;
	if (local->domain != AXIS_ARR_ERROR ||
	    (local->code != AXIS_ARR_ERROR_NO_ANSWER && local->code != AXIS_ARR_ERROR_UNREADABLE)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	g_clear_error(&local);
	/* The pause is taken in slices, so that stopping the reading is not held up. */
	for (waited = 0; waited < reading->tuning.series_retry_ms; waited += 100) {
		if (stopped(reading, error))
			return FALSE;
		g_usleep(100 * 1000);
	}
	if (get_series_list(reading, path, body, &local))
		return TRUE;
	if (local->domain != AXIS_ARR_ERROR ||
	    (local->code != AXIS_ARR_ERROR_NO_ANSWER && local->code != AXIS_ARR_ERROR_UNREADABLE)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	*gave_up = TRUE;
	return TRUE;
}

/* The series' item says its lists are incomplete.  @gave_up: for want of an
 * answer; so many series in a row end the reading (the app is down). */
static gboolean
series_incomplete(
	Reading      *reading,
	const gchar  *plan_text,
	const gchar  *seq_text,
	gboolean      gave_up,
	GError      **error
){
	const gchar *params[2];

	params[0] = plan_text;
	params[1] = seq_text;
	if (!axis_db_pool_exec(reading->self->pool,
	        "UPDATE arr_plan_item SET match = 'incomplete' WHERE plan_id = $1::bigint AND seq = $2::integer", 2, params,
	        NULL, error))
		return FALSE;
	if (gave_up && ++reading->n_gave_up >= AXIS_ARR_SERIES_GIVE_UP) {
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_ANSWER, "%s did not answer for %d series in a row.",
			reading->app, AXIS_ARR_SERIES_GIVE_UP);
		return FALSE;
	}
	return TRUE;
}

/* The episodes and the episode files of the series @arr_id (the plan's item
 * @seq_text): asked for, reduced to what is kept, stored, and freed.  A list
 * that cannot be read makes that one series incomplete. */
static gboolean
read_one_series(
	Reading      *reading,
	const gchar  *seq_text,
	gint64        arr_id,
	GError      **error
){
	g_autofree gchar *episodes_path = g_strdup_printf(AXIS_ARR_EPISODES_PATH, arr_id);
	g_autofree gchar *files_path = g_strdup_printf(AXIS_ARR_EPISODE_FILES_PATH, arr_id);
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	g_autoptr(GArray) episodes = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(JsonArray) episode_rows = json_array_new();
	g_autoptr(JsonArray) file_rows = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(AxisDbResult) updated = NULL;
	g_autofree gchar *episodes_text = NULL;
	g_autofree gchar *files_text = NULL;
	g_autofree gchar *n_episodes = NULL;
	g_autofree gchar *n_monitored = NULL;
	g_autofree gchar *n_with_file = NULL;
	g_autofree gchar *n_files = NULL;
	g_autofree gchar *n_odd = NULL;
	const gchar *params[9];
	const gchar *why = NULL;
	const gchar *data;
	GBytes *body = NULL;
	gboolean read = FALSE;
	gboolean gave_up = FALSE;
	gsize length = 0;
	guint monitored = 0;
	guint with_file = 0;
	guint odd_episodes = 0;
	guint odd_places = 0;
	guint odd_files = 0;
	guint i;

	params[0] = plan_text;
	params[1] = seq_text;
	if (!connection_kept(reading, error) || !get_series_list_patiently(reading, episodes_path, &body, &gave_up, error))
		return FALSE;
	if (gave_up)
		return series_incomplete(reading, plan_text, seq_text, TRUE, error);
	data = g_bytes_get_data(body, &length);
	read = axis_arr_parse_episodes(data, length, AXIS_ARR_MAX_SERIES_EPISODES, &episodes, &odd_episodes, &why);
	g_clear_pointer(&body, g_bytes_unref);
	if (read) {
		if (!connection_kept(reading, error) ||
		    !get_series_list_patiently(reading, files_path, &body, &gave_up, error))
			return FALSE;
		if (gave_up)
			return series_incomplete(reading, plan_text, seq_text, TRUE, error);
		data = g_bytes_get_data(body, &length);
		read = axis_arr_parse_episode_files(data, length, AXIS_ARR_MAX_SERIES_EPISODES, &files, &odd_files, &why);
		g_clear_pointer(&body, g_bytes_unref);
	}
	if (!read)
		/* Sonarr's answer for this series cannot be used: it alone. */
		return series_incomplete(reading, plan_text, seq_text, FALSE, error);

	for (i = 0; i < episodes->len; i++) {
		const AxisArrEpisode *episode = &g_array_index(episodes, AxisArrEpisode, i);
		JsonObject *row = json_object_new();

		json_object_set_int_member(row, "o", i);
		if (episode->odd) {
			/* A place whose entry could not be read: stored as that, and
			 * counted among the odd, not among the episodes. */
			json_object_set_int_member(row, "s", episode->season);
			json_object_set_int_member(row, "e", episode->number);
			json_object_set_boolean_member(row, "m", FALSE);
			json_object_set_boolean_member(row, "x", TRUE);
			json_array_add_object_element(episode_rows, row);
			odd_places++;
			continue;
		}
		if (episode->arr_id > 0)
			json_object_set_int_member(row, "a", episode->arr_id);
		if (episode->tvdb_id > 0)
			json_object_set_int_member(row, "t", episode->tvdb_id);
		json_object_set_int_member(row, "s", episode->season);
		json_object_set_int_member(row, "e", episode->number);
		if (episode->air_date[0] != '\0')
			json_object_set_string_member(row, "d", episode->air_date);
		json_object_set_boolean_member(row, "m", episode->monitored);
		if (episode->file_id > 0) {
			json_object_set_int_member(row, "f", episode->file_id);
			with_file++;
		}
		if (episode->monitored)
			monitored++;
		/* Sonarr's own absolute number: kept to be compared with TheTVDB's
		 * (A20), and for nothing else. */
		if (episode->absolute > 0)
			json_object_set_int_member(row, "b", episode->absolute);
		json_array_add_object_element(episode_rows, row);
	}
	for (i = 0; i < files->len; i++) {
		const AxisArrEpisodeFile *file = g_ptr_array_index(files, i);
		JsonObject *row = json_object_new();

		json_object_set_int_member(row, "o", i);
		json_object_set_int_member(row, "a", file->arr_id);
		set_text(row, "p", file->path);
		if (file->size >= 0)
			json_object_set_int_member(row, "z", file->size);
		set_text(row, "q", file->quality);
		set_text(row, "n", file->scene);
		json_array_add_object_element(file_rows, row);
	}
	reading->n_episodes += episodes->len;
	reading->n_files += files->len;
	if (reading->n_episodes > AXIS_ARR_PLAN_MAX_EPISODES || reading->n_files > AXIS_ARR_PLAN_MAX_FILES) {
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_TOO_LARGE, "%s's library is too large for this version of "
			"Axis to migrate (more than %d episodes or %d episode files).", reading->app, AXIS_ARR_PLAN_MAX_EPISODES,
			AXIS_ARR_PLAN_MAX_FILES);
		return FALSE;
	}
	json_node_set_array(node, episode_rows);
	episodes_text = object_to_text(node);
	json_node_set_array(node, file_rows);
	files_text = object_to_text(node);
	n_episodes = g_strdup_printf("%u", episodes->len - odd_places);
	n_monitored = g_strdup_printf("%u", monitored);
	n_with_file = g_strdup_printf("%u", with_file);
	n_files = g_strdup_printf("%u", files->len);
	n_odd = g_strdup_printf("%u", odd_episodes + odd_places + odd_files);
	params[2] = episodes_text;
	params[3] = files_text;
	params[4] = n_episodes;
	params[5] = n_monitored;
	params[6] = n_with_file;
	params[7] = n_files;
	params[8] = n_odd;
	if (!axis_db_pool_exec(reading->self->pool, parts_sql, 9, params, &updated, error))
		return FALSE;
	if (axis_db_result_get_affected_rows(updated) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "The reading's plan is gone.");
		return FALSE;
	}
	reading->n_gave_up = 0;
	return TRUE;
}

/* For every series that is matched (the first entry of each of Sonarr's
 * numbers): its episodes and its episode files.  Within a count of series,
 * the bytes of all the lists, and one deadline; a reading that passes one
 * fails as a whole and keeps nothing. */
static gboolean
read_series_lists(
	Reading  *reading,
	gdouble   from,
	gdouble   span,
	GError  **error
){
	g_autoptr(AxisDbResult) wanted = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GArray) rows = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	const gchar *params[1];
	guint i;

	params[0] = plan_text;
	if (!axis_db_pool_exec(reading->self->pool,
	        "SELECT seq, arr_id FROM arr_plan_item WHERE plan_id = $1::bigint AND match = 'matched' AND arr_id IS NOT NULL"
	        " ORDER BY seq", 1, params, &wanted, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(wanted); i++) {
		gint64 arr_id = axis_arr_int_at(wanted, i, 1, 0);
		gint64 *key;

		if (arr_id <= 0 || g_hash_table_contains(seen, &arr_id))
			continue;
		key = g_new(gint64, 1);
		*key = arr_id;
		g_hash_table_add(seen, key);
		g_array_append_val(rows, i);
	}
	if (rows->len > reading->tuning.series_read_max) {
		g_set_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_TOO_LARGE, "%s's library is too large for this version of "
			"Axis to migrate (more than %u series to read the episodes of).", reading->app,
			reading->tuning.series_read_max);
		return FALSE;
	}
	for (i = 0; i < rows->len; i++) {
		guint row = g_array_index(rows, guint, i);
		g_autofree gchar *seq_text = axis_arr_text_at(wanted, row, 0);
		g_autofree gchar *message = g_strdup_printf("Reading the episodes of series %u of %u", i + 1, rows->len);

		if (stopped(reading, error))
			return FALSE;
		axis_job_context_set_progress(reading->context, from + span * i / rows->len, message);
		if (!read_one_series(reading, seq_text, axis_arr_int_at(wanted, row, 1, 0), error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
complete_plan(
	Reading  *reading,
	GError  **error
){
	g_autoptr(JsonArray) profiles = json_array_new();
	g_autoptr(JsonArray) folders = json_array_new();
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_ARRAY);
	g_autoptr(AxisDbResult) updated = NULL;
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	g_autofree gchar *source_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->source_id);
	g_autofree gchar *count = g_strdup_printf("%u", reading->n_titles);
	g_autofree gchar *profiles_text = NULL;
	g_autofree gchar *folders_text = NULL;
	const gchar *params[5];
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	for (i = 0; i < reading->profiles->len; i++) {
		const AxisArrProfile *profile = g_ptr_array_index(reading->profiles, i);
		JsonObject *row = json_object_new();

		json_object_set_int_member(row, "id", profile->id);
		json_object_set_string_member(row, "name", profile->name);
		json_array_add_object_element(profiles, row);
	}
	json_node_set_array(node, profiles);
	profiles_text = object_to_text(node);
	for (i = 0; i < reading->folders->len; i++) {
		const AxisArrFolder *folder = g_ptr_array_index(reading->folders, i);
		JsonObject *row = json_object_new();

		json_object_set_string_member(row, "path", folder->path);
		json_object_set_boolean_member(row, "accessible", folder->accessible);
		json_array_add_object_element(folders, row);
	}
	json_node_set_array(node, folders);
	folders_text = object_to_text(node);

	params[0] = plan_text;
	params[1] = source_text;
	params[2] = count;
	params[3] = profiles_text;
	params[4] = folders_text;
	conn = axis_db_pool_acquire(reading->self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* The plan is whole, and the connection's one plan: earlier ones go. */
	ok = axis_db_conn_begin(conn, error) &&
		axis_db_conn_exec(conn,
			"UPDATE arr_plan SET state = 'complete', finished_at = now(), problem = NULL, n_items = $3::integer,"
			" profiles = $4::jsonb, folders = $5::jsonb WHERE id = $1::bigint AND source_id = $2::bigint",
			5, params, &updated, error);
	/* No row: the connection was forgotten while the reading ran. */
	if (ok && axis_db_result_get_affected_rows(updated) == 0) {
		g_set_error_literal(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND, "The reading's plan is gone.");
		ok = FALSE;
	}
	ok = ok &&
		axis_db_conn_exec(conn, "DELETE FROM arr_plan WHERE source_id = $2::bigint AND id <> $1::bigint", 2, params,
			NULL, error) &&
		axis_db_conn_commit(conn, error);
	if (!ok)
		axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(reading->self->pool, conn);
	return ok;
}

static gboolean
reading_run(
	Reading  *reading,
	GError  **error
){
	g_autofree gchar *asking = NULL;

	if (!reading_begin(reading, error))
		return FALSE;
	asking = g_strdup_printf("Asking %s what it has", reading->app);
	axis_job_context_set_progress(reading->context, 0.02, asking);
	/* Nothing else is asked until this answer has named the app. */
	if (!axis_arr_client_status(reading->client, &reading->version, error))
		return FALSE;
	if (stopped(reading, error) || !read_lists(reading, error))
		return FALSE;
	axis_job_context_set_progress(reading->context, 0.06, asking);
	if (stopped(reading, error) || !read_titles(reading, error))
		return FALSE;
	axis_job_context_set_progress(reading->context, 0.15, "Storing what was read");
	if (!store_titles(reading, error))
		return FALSE;
	if (reading->sonarr) {
		/* A series that is matched has its two lists read; no other does. */
		if (!match_titles(reading, 0.32, 0.28, error) || stopped(reading, error) ||
		    !read_series_lists(reading, 0.60, 0.37, error))
			return FALSE;
	} else if (!match_titles(reading, 0.32, 0.63, error)) {
		return FALSE;
	}
	if (stopped(reading, error))
		return FALSE;
	axis_job_context_set_progress(reading->context, 0.98, "Finishing");
	return complete_plan(reading, error);
}

/* The plan says why its reading ended; best effort (the job says it too). */
static void
fail_plan(
	Reading     *reading,
	const gchar *problem
){
	g_autofree gchar *plan_text = g_strdup_printf("%" G_GINT64_FORMAT, reading->plan_id);
	g_autoptr(GError) error = NULL;
	const gchar *params[2];

	params[0] = plan_text;
	params[1] = problem;
	if (!axis_db_pool_exec(reading->self->pool,
	        "UPDATE arr_plan SET state = 'failed', finished_at = now(), problem = left($2, 300)"
	        " WHERE id = $1::bigint AND state = 'reading'", 2, params, NULL, &error))
		g_debug("bring over: a failed reading could not be recorded: %s", error->message);
	/* A plan that failed is never previewed: what it stored goes. */
	g_clear_error(&error);
	if (!axis_db_pool_exec(reading->self->pool, "DELETE FROM arr_plan_item WHERE plan_id = $1::bigint", 1, params, NULL,
	        &error))
		g_debug("bring over: what a failed reading stored could not be removed: %s", error->message);
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

gboolean
axis_arr_run_read(
	AxisArr         *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	g_autoptr(GError) local = NULL;
	Reading reading;
	AxisJobStopReason stop;
	gboolean ok;

	memset(&reading, 0, sizeof(reading));
	reading.self = self;
	reading.context = context;
	reading.source_id = payload_id(payload, "source");
	reading.plan_id = payload_id(payload, "plan");
	reading.app = "The app";
	reading.started_us = g_get_monotonic_time();
	axis_arr_get_tuning(self, &reading.tuning);
	if (reading.source_id <= 0 || reading.plan_id <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "The job names no connection.");
		return FALSE;
	}
	ok = reading_run(&reading, &local);
	if (ok) {
		g_autoptr(GError) ignored = NULL;

		/* The reading is also a test that passed. */
		axis_arr_store_test(self, reading.source_id, reading.source->origin, reading.source->path_prefix,
			AXIS_ARR_TEST_OK, reading.version, &ignored);
		g_info("bring over: connection %" G_GINT64_FORMAT " read: %u titles", reading.source_id, reading.n_titles);
		reading_clear(&reading);
		return TRUE;
	}
	stop = axis_job_context_get_stop_reason(context);
	if (stop == AXIS_JOB_STOP_SHUTDOWN || stop == AXIS_JOB_STOP_LEASE_LOST) {
		/* The job runs again, on the same plan. */
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "The reading was interrupted.");
	} else if (stop == AXIS_JOB_STOP_CANCELLED) {
		fail_plan(&reading, "The reading was cancelled.");
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "The reading was cancelled.");
	} else if (local->domain == AXIS_ARR_ERROR) {
		/* Fixed words of this layer: safe for the plan, the job and a page. */
		fail_plan(&reading, local->message);
		if (local->code == AXIS_ARR_ERROR_REFUSED || local->code == AXIS_ARR_ERROR_NOT_APP ||
		    local->code == AXIS_ARR_ERROR_CERTIFICATE || local->code == AXIS_ARR_ERROR_NO_ANSWER) {
			g_autoptr(GError) ignored = NULL;

			if (reading.source != NULL)
				axis_arr_store_test(self, reading.source_id, reading.source->origin, reading.source->path_prefix,
					axis_arr_test_of_error(local), NULL, &ignored);
		}
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, local->message);
	} else {
		/* The database's own words stay in the job; the plan says less. */
		fail_plan(&reading, "The reading failed in Axis. The job on the Jobs page says why.");
		g_propagate_error(error, g_steal_pointer(&local));
	}
	reading_clear(&reading);
	return FALSE;
}
