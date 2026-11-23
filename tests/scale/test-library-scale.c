/*
 * test-library-scale.c - the Library pages' queries on an index of two
 * million files
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not part of `make check` (it builds a large generated index and takes
 * minutes): `make check-scale`.  AXIS_SCALE_ROWS changes the size (default
 * 2000000).  The rows are made by SQL (tests/axis-test-browser.h), not by
 * files.
 *
 * It prints, as TAP comments, what building the index cost, the sizes of
 * the tables and indexes, what the folder totals cost to compute, and for
 * each query of the pages its timing and its plan from EXPLAIN (ANALYZE,
 * BUFFERS).  It fails when a query that a page runs, other than the
 * ones listed as known limits, takes more than 200 ms (the median of five
 * runs, count included).  Then the Analysis refresh, and the Savings
 * refresh and its file lists, timed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "axis-test-browser.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "library/axis-analysis.h"
#include "library/axis-library-browse.h"
#include "library/axis-library-browse-private.h"
#include "library/axis-library-private.h"
#include "library/axis-library-filter.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-estimator-load.h"
#include "transcode/axis-estimator-private.h"

#define LIMIT_US   (200 * 1000)
#define RUNS       5
#define DIRS       2000

typedef struct
{
	AxisTestLibrary *t;
	gchar           *disk;
	gint64           volume;
	gint64           root;
	guint            rows;
} Fixture;

static guint
env_rows(void)
{
	const gchar *text = g_getenv("AXIS_SCALE_ROWS");

	if (text != NULL && g_ascii_strtoull(text, NULL, 10) >= 1000)
		return (guint)g_ascii_strtoull(text, NULL, 10);
	return 2000000;
}

static gdouble
now_s(void)
{
	return (gdouble)g_get_monotonic_time() / G_USEC_PER_SEC;
}

/* The plan of @sql with @values, as text. */
static gchar *
plan_of(
	Fixture             *fx,
	const gchar         *sql,
	const gchar *const  *values,
	guint                n
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *explain = g_strconcat("EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) ", sql, NULL);
	GString *plan = g_string_new(NULL);
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	guint i;

	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '120s'", 0, NULL, NULL, &error));
	g_assert_true(axis_db_conn_exec(conn, explain, n, values, &result, &error));
	g_assert_true(axis_db_conn_commit(conn, &error));
	axis_db_pool_release(fx->t->pool, conn);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_string_append_printf(plan, "#     %s\n", axis_db_result_get_text(result, i, 0));
	return g_string_free(plan, FALSE);
}

/* What the four statements of the folder totals do on @root, in a
 * transaction that is rolled back. */
static void
profile_dirs(
	Fixture *fx,
	gint64   root
){
	static const gchar *const names[] = { "build", "write", "delete", "parents" };
	const gchar *statements[4];
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, root);
	const gchar *params[1];
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	guint s;

	statements[0] = axis_library_sql_dirs_build;
	statements[1] = axis_library_sql_dirs_write;
	statements[2] = axis_library_sql_dirs_delete;
	statements[3] = axis_library_sql_dirs_parents;
	params[0] = id;
	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '600s'", 0, NULL, NULL, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL enable_nestloop = off", 0, NULL, NULL, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL work_mem = '" AXIS_LIBRARY_DIRS_WORK_MEM "'", 0, NULL, NULL,
		&error));
	for (s = 0; s < 4; s++) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *explain = g_strconcat("EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) ", statements[s], NULL);
		guint i;

		g_assert_true(axis_db_conn_exec(conn, explain, 1, params, &result, &error));
		g_print("#   folder totals, %s:\n", names[s]);
		for (i = 0; i < axis_db_result_get_n_rows(result); i++)
			g_print("#     %s\n", axis_db_result_get_text(result, i, 0));
		if (s == 0)
			g_assert_true(axis_db_conn_exec(conn, "ANALYZE library_scan_dirs", 0, NULL, NULL, &error));
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(fx->t->pool, conn);
}

static gint
compare_int64(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y;
}

static gint64
median(gint64 *times)
{
	qsort(times, RUNS, sizeof(gint64), compare_int64);
	return times[RUNS / 2];
}

/* The page's real query (and its count), timed, then its plan. */
static void
check_list(
	Fixture     *fx,
	const gchar *label,
	const gchar *query,
	gboolean     known_limit
){
	AxisLibraryFilter filter;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileList) list = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *count_sql = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *count_plan = NULL;
	g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
	gint64 times[RUNS];
	guint n_base = 0;
	guint i;

	g_assert_true(axis_library_filter_parse(query, &filter, &error));
	for (i = 0; i < RUNS; i++) {
		gint64 started = g_get_monotonic_time();

		g_clear_pointer(&list, axis_file_list_free);
		if (!axis_library_list_files(fx->t->library, &filter, &list, &error)) {
			g_print("# %-46s  FAILED: %s\n", label, error->message);
			g_clear_error(&error);
			g_assert_true(known_limit);
			axis_library_filter_clear(&filter);
			return;
		}
		times[i] = g_get_monotonic_time() - started;
	}

	/* The plans: the cursor's key as the page reads it. */
	if (filter.after > 0) {
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *id_params[1];

		after = g_strdup_printf("%" G_GINT64_FORMAT, filter.after);
		id_params[0] = after;
		g_assert_true(axis_db_pool_exec(fx->t->pool, axis_library_browse_key_sql(filter.sort), 1, id_params,
			&result, &error));
		if (axis_db_result_get_n_rows(result) == 1 && !axis_db_result_is_null(result, 0, 0))
			key = g_strdup(axis_db_result_get_text(result, 0, 0));
	}
	sql = axis_library_browse_list_sql(&filter, key, after, values, &n_base, &count_sql);
	plan = plan_of(fx, sql, (const gchar *const *)values->pdata, values->len);
	count_plan = plan_of(fx, count_sql, (const gchar *const *)values->pdata, n_base);

	{
		gint64 m = median(times);

		g_print("# %-46s %7.1f ms  %u rows%s%s%s\n", label, m / 1000.0, list->files->len,
			list->count_capped ? ", count capped" : "", list->count_unknown ? ", COUNT TIMED OUT" : "",
			m > LIMIT_US ? (known_limit ? "  [known limit]" : "  TOO SLOW") : "");
		g_print("#   list:\n%s#   count:\n%s", plan, count_plan);
		if (!known_limit)
			g_assert_cmpint(m, <=, LIMIT_US);
	}
	axis_library_filter_clear(&filter);
}

static void
check_statement(
	Fixture             *fx,
	const gchar         *label,
	const gchar         *sql,
	const gchar *const  *values,
	guint                n
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *plan = NULL;
	gint64 times[RUNS];
	guint i;

	for (i = 0; i < RUNS; i++) {
		g_autoptr(AxisDbResult) result = NULL;
		gint64 started = g_get_monotonic_time();

		g_assert_true(axis_db_pool_exec(fx->t->pool, sql, n, values, &result, &error));
		times[i] = g_get_monotonic_time() - started;
	}
	plan = plan_of(fx, sql, values, n);
	{
		gint64 m = median(times);

		g_print("# %-46s %7.1f ms\n#   plan:\n%s", label, m / 1000.0, plan);
		g_assert_cmpint(m, <=, LIMIT_US);
	}
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();

	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Big", AXIS_MEDIA_MOVIES);
	fx->rows = env_rows();
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_library_free(fx->t);
	g_free(fx->disk);
	axis_test_log_reset();
}

static void
print_sizes(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	g_assert_true(axis_db_pool_exec(fx->t->pool,
		"SELECT c.relname, pg_size_pretty(pg_relation_size(c.oid)), c.relkind"
		" FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace"
		" WHERE n.nspname = 'public' AND c.relname IN ('library_file', 'media_info', 'library_dir')"
		"   OR (n.nspname = 'public' AND c.relname LIKE 'library_file_%idx')"
		"   OR (n.nspname = 'public' AND c.relname LIKE 'media_info_%idx')"
		"   OR (n.nspname = 'public' AND c.relname LIKE 'library_dir_%idx')"
		" ORDER BY c.relkind DESC, pg_relation_size(c.oid) DESC", 0, NULL, &result, &error));
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_print("#   %-34s %10s  %s\n", axis_db_result_get_text(result, i, 0),
			axis_db_result_get_text(result, i, 1),
			g_strcmp0(axis_db_result_get_text(result, i, 2), "r") == 0 ? "table" : "index");
}

/* ── The Analysis refresh ───────────────────────────────────────── */

typedef struct
{
	Fixture *fx;
	gdouble  last;
	gdouble  write_seconds;
	gboolean wrote;
} Stages;

/* Prints how long each stage of the refresh took.  In the middle of it a
 * writer updates twenty thousand rows of the index it reads: if the refresh
 * held a lock that blocks writers (a long transaction in the way a scan's
 * would be), this would wait for the end of it. */
static void
after_stage(
	const gchar *stage,
	gpointer     data
){
	Stages *stages = data;
	gdouble now = now_s();

	g_print("#   %-34s %7.1f s\n", stage, now - stages->last);
	stages->last = now_s();
	if (!stages->wrote && g_strcmp0(stage, "space by root") == 0) {
		gdouble started = now_s();

		stages->wrote = TRUE;
		axis_test_db_exec(stages->fx->t->db,
			"UPDATE library_file SET last_seen_generation = last_seen_generation WHERE id BETWEEN 1 AND 20000");
		stages->write_seconds = now_s() - started;
		stages->last = now_s();
	}
}

/*
 * The refresh on everything the fixture holds (about two and a half million
 * files), with the time of each stage.  It must finish within a minute, must
 * leave no work table behind, and must not hold up a writer.
 */
static void
test_analysis(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisAnalysis) analysis = axis_analysis_new(fx->t->pool, NULL);
	AxisAnalysisTuning tuning;
	Stages stages;
	gdouble started;
	gdouble seconds;
	gint64 snapshot = 0;

	memset(&stages, 0, sizeof(stages));
	stages.fx = fx;
	memset(&tuning, 0, sizeof(tuning));
	tuning.after_stage = after_stage;
	tuning.data = &stages;
	axis_analysis_set_tuning(analysis, &tuning);

	axis_test_db_exec(fx->t->db, "ANALYZE library_file");
	axis_test_db_exec(fx->t->db, "ANALYZE media_info");
	g_print("# the analysis refresh, stage by stage:\n");
	started = now_s();
	stages.last = started;
	g_assert_true(axis_analysis_refresh(analysis, "direct", 0, NULL, &snapshot, &error));
	g_assert_no_error(error);
	seconds = now_s() - started;
	g_print("# the analysis refresh, %" G_GINT64_FORMAT " files in the index: %.1f s (the limit is 60 s)\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM library_file"), seconds);
	g_print("# a writer updating 20000 rows of the index meanwhile waited %.2f s\n", stages.write_seconds);
	g_print("# the snapshot holds %" G_GINT64_FORMAT " facts\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_fact WHERE snapshot_id = %" G_GINT64_FORMAT,
			snapshot));
	g_assert_cmpfloat(seconds, <=, 60.0);
	g_assert_cmpfloat(stages.write_seconds, <=, 10.0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM pg_class WHERE relname = 'analysis_work'"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM analysis_fact WHERE snapshot_id = %"
		G_GINT64_FORMAT, snapshot), <, 200);
	{
		g_autofree gchar *sizes = axis_test_library_q(fx->t,
			"SELECT pg_size_pretty(pg_total_relation_size('analysis_fact')) || ' of facts, '"
			" || pg_size_pretty(pg_total_relation_size('analysis_snapshot')) || ' of snapshots'");

		g_print("# analysis table sizes: %s\n", sizes);
	}
}

/* ── The Savings refresh and its lists ──────────────────────────── */

/* A fact's profile: facts name the revision they were computed for. */
#define FACT_PROFILE " JOIN conversion_profile_revision r ON r.id = f.revision_id" \
	" JOIN conversion_profile p ON p.id = r.profile_id"

/* One estimator list, timed once (the page has its own deadlines: a page
 * that does not fill in time is an error the page shows, not a failure
 * here). */
static void
estimate_list(
	AxisEstimator *estimator,
	const gchar   *label,
	const gchar   *query
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisFileList) list = NULL;
	AxisLibraryFilter filter;
	gdouble started;

	g_assert_true(axis_library_filter_parse(query, &filter, &error));
	started = now_s();
	if (!axis_estimator_list_files(estimator, &filter, &list, &error)) {
		g_print("# %-46s %7.1f ms  NO PAGE: %s\n", label, (now_s() - started) * 1000.0, error->message);
	} else {
		g_print("# %-46s %7.1f ms  %u rows, count %" G_GUINT64_FORMAT "%s%s\n", label, (now_s() - started) * 1000.0,
			list->files->len, list->count, list->count_capped ? " (capped)" : "",
			list->count_unknown ? ", COUNT TIMED OUT" : "");
	}
	axis_library_filter_clear(&filter);
}

/* The plan of @sql as the estimator runs it (JIT off), or with JIT left on
 * when @jit (with its summary: plan_of's COSTS OFF hides it). */
static gchar *
plan_estimate(
	Fixture             *fx,
	const gchar         *sql,
	const gchar *const  *values,
	guint                n,
	gboolean             jit
){
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *explain = g_strconcat(jit ? "EXPLAIN (ANALYZE, SUMMARY) " :
		"EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) ", sql, NULL);
	GString *plan = g_string_new(NULL);
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	guint i;

	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "SET LOCAL statement_timeout = '120s'", 0, NULL, NULL, &error));
	if (!jit)
		g_assert_true(axis_db_conn_exec(conn, AXIS_ESTIMATE_SQL_NO_JIT, 0, NULL, NULL, &error));
	g_assert_true(axis_db_conn_exec(conn, explain, n, values, &result, &error));
	g_assert_true(axis_db_conn_commit(conn, &error));
	axis_db_pool_release(fx->t->pool, conn);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *line = axis_db_result_get_text(result, i, 0);

		/* With JIT, only its summary and the total. */
		if (!jit || strstr(line, "JIT") != NULL || strstr(line, "Functions:") != NULL ||
		    strstr(line, "Timing:") != NULL || strstr(line, "Execution Time") != NULL)
			g_string_append_printf(plan, "#     %s\n", line);
	}
	return g_string_free(plan, FALSE);
}

/* The plans of what an estimator list runs: one batch of the scan (the
 * ordinary list's order, narrowed), the loader on a batch of films and on
 * one of tracks, and one album's members. */
static void
estimate_plans(
	Fixture *fx,
	gint64   music
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *narrowing = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *tracks = NULL;
	g_autofree gchar *track = NULL;
	AxisLibraryFilter filter;
	const gchar *params[1];

	g_assert_true(axis_library_filter_parse("target=space_saver&outcome=eligible", &filter, &error));
	narrowing = axis_estimator_list_narrowing(axis_estimate_builtin_target("space_saver"), "eligible");
	sql = axis_library_browse_scan_sql(&filter, narrowing, NULL, NULL, AXIS_ESTIMATE_LIST_BATCH, values);
	plan = plan_estimate(fx, sql, (const gchar *const *)values->pdata, values->len, FALSE);
	g_print("#   savings list, one batch of the scan (Space saver, eligible, by name):\n%s", plan);
	axis_library_filter_clear(&filter);

	films = axis_test_library_q(fx->t, "SELECT '{' || string_agg(id::text, ',') || '}' FROM (SELECT id FROM"
		" library_file WHERE root_id = %" G_GINT64_FORMAT " AND kind = 'video' ORDER BY id LIMIT %d) x", fx->root,
		AXIS_ESTIMATE_LIST_BATCH);
	params[0] = films;
	g_clear_pointer(&plan, g_free);
	plan = plan_estimate(fx, AXIS_ESTIMATE_SQL_FILES, params, 1, FALSE);
	g_print("#   savings list, the loader on a batch of films:\n%s", plan);
	g_clear_pointer(&plan, g_free);
	plan = plan_estimate(fx, AXIS_ESTIMATE_SQL_FILES, params, 1, TRUE);
	g_print("#   the same with PostgreSQL's JIT left on (what AXIS_ESTIMATE_SQL_NO_JIT avoids):\n%s", plan);

	tracks = axis_test_library_q(fx->t, "SELECT '{' || string_agg(id::text, ',') || '}' FROM (SELECT id FROM"
		" library_file WHERE root_id = %" G_GINT64_FORMAT " AND kind = 'audio' ORDER BY id LIMIT %d) x", music,
		AXIS_ESTIMATE_LIST_BATCH);
	params[0] = tracks;
	g_clear_pointer(&plan, g_free);
	plan = plan_estimate(fx, AXIS_ESTIMATE_SQL_FILES, params, 1, FALSE);
	g_print("#   savings list, the loader on a batch of tracks:\n%s", plan);

	track = axis_test_library_q(fx->t, "SELECT '{' || id || '}' FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND kind = 'audio' ORDER BY id LIMIT 1", music);
	params[0] = track;
	g_clear_pointer(&plan, g_free);
	plan = plan_estimate(fx, AXIS_ESTIMATE_SQL_UNIT_MEMBERS, params, 1, FALSE);
	g_print("#   savings list, one album's members:\n%s", plan);

	/* As a batch asks: the albums of five hundred tracks at once. */
	params[0] = tracks;
	g_clear_pointer(&plan, g_free);
	plan = plan_estimate(fx, AXIS_ESTIMATE_SQL_UNIT_MEMBERS, params, 1, FALSE);
	g_print("#   savings list, the albums of a batch of tracks:\n%s", plan);
}

/*
 * The estimates over the same index plus a music root of albums (the
 * folder two levels down: about three tracks each).  The generated probe
 * records have no streams and sizes unrelated to their bitrates; here they
 * get consistent facts (the bitrate from the size and the duration, a
 * picture of the size class's dimensions and a sound track per video, one
 * FLAC track per audio file), so that the decisions reach the size model
 * and every reason of the order is possible.  Prints how long the refresh
 * takes and what the estimator's lists cost; fails only when the refresh
 * takes more than ten minutes.
 */
static void
test_estimate(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisSettings *settings = axis_settings_new();
	AxisEstimator *estimator;
	gint64 music;
	gint64 snapshot = 0;
	gdouble started;
	gdouble seconds;

	music = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Music", AXIS_MEDIA_MUSIC);
	axis_test_browser_generate(fx->t, fx->volume, music, fx->rows + 600001, fx->rows + 600000 + fx->rows / 4, DIRS);

	started = now_s();
	axis_test_db_exec(fx->t->db,
		"UPDATE media_info m SET"
		" duration_ms = 60000 + (m.inode % 7200) * 1000,"
		" bitrate = GREATEST(f.size * 8000 / (60000 + (m.inode % 7200) * 1000), 64000),"
		" bitrate_estimated = false, frame_rate = CASE WHEN f.kind = 'video' THEN 24 END,"
		" width = CASE WHEN f.kind <> 'video' THEN NULL WHEN m.resolution_class = 'sd' THEN 720"
		"   WHEN m.resolution_class = '720p' THEN 1280 WHEN m.resolution_class = '1080p' THEN 1920 ELSE 3840 END,"
		" height = CASE WHEN f.kind <> 'video' THEN NULL WHEN m.resolution_class = 'sd' THEN 480"
		"   WHEN m.resolution_class = '720p' THEN 720 WHEN m.resolution_class = '1080p' THEN 1080 ELSE 2160 END,"
		" bit_depth = CASE WHEN f.kind <> 'video' THEN NULL WHEN m.hdr_format = 'sdr' THEN 8 ELSE 10 END,"
		" interlaced = CASE WHEN f.kind = 'video' THEN false END,"
		" audio_codec = CASE WHEN f.kind = 'audio' THEN 'flac' ELSE m.audio_codec END,"
		" audio_channels = CASE WHEN f.kind = 'audio' THEN 2 ELSE 6 END,"
		" audio_sample_rate = CASE WHEN f.kind = 'audio' THEN 44100 ELSE 48000 END,"
		" audio_bit_depth = CASE WHEN f.kind = 'audio' THEN 16 END,"
		" audio_lossless = f.kind = 'audio'"
		" FROM library_file f WHERE f.id = m.file_id AND m.status = 'ok'");
	axis_test_db_exec(fx->t->db,
		"UPDATE media_info SET bits_per_pixel = bitrate * 0.9 / (width * height * 24.0)"
		" WHERE status = 'ok' AND width IS NOT NULL");
	axis_test_db_exec(fx->t->db,
		"INSERT INTO media_stream (file_id, idx, kind, codec, duration_ms, bitrate, width, height, bit_depth,"
		" frame_rate, real_frame_rate, field_order, hdr_format, hdr_uncertain, is_default)"
		" SELECT m.file_id, 0, 'video', m.video_codec, m.duration_ms, (m.bitrate * 0.9)::bigint, m.width, m.height,"
		" m.bit_depth, 24, 24, 'progressive', m.hdr_format, m.hdr_uncertain, true"
		" FROM media_info m JOIN library_file f ON f.id = m.file_id WHERE m.status = 'ok' AND f.kind = 'video'");
	axis_test_db_exec(fx->t->db,
		"INSERT INTO media_stream (file_id, idx, kind, codec, duration_ms, bitrate, channels, sample_rate,"
		" bit_depth_audio, lossless, language, is_default)"
		" SELECT m.file_id, CASE WHEN f.kind = 'video' THEN 1 ELSE 0 END, 'audio', m.audio_codec, m.duration_ms,"
		" CASE WHEN f.kind = 'video' THEN (m.bitrate * 0.1)::bigint ELSE m.bitrate END, m.audio_channels,"
		" m.audio_sample_rate, m.audio_bit_depth, f.kind = 'audio', 'eng', true"
		" FROM media_info m JOIN library_file f ON f.id = m.file_id WHERE m.status = 'ok'");
	axis_test_db_exec(fx->t->db, "ANALYZE library_file");
	axis_test_db_exec(fx->t->db, "ANALYZE media_info");
	axis_test_db_exec(fx->t->db, "ANALYZE media_stream");
	g_print("# the savings: streams and consistent facts for the probe records: %.1f s\n", now_s() - started);

	g_assert_true(axis_settings_add_table(settings, AXIS_ESTIMATE_MODULE_ID, axis_estimator_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(settings, AXIS_ANALYSIS_MODULE_ID, axis_analysis_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(settings, NULL, &error));
	estimator = axis_estimator_new(fx->t->pool, fx->t->storage, settings);

	started = now_s();
	g_assert_true(axis_estimator_refresh(estimator, "direct", 0, NULL, &snapshot, &error));
	g_assert_no_error(error);
	seconds = now_s() - started;
	g_print("# the savings refresh, %" G_GINT64_FORMAT " present video and audio files (%" G_GINT64_FORMAT
		" with streams), %d targets: %.1f s\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE state = 'present'"
			" AND kind IN ('video', 'audio')"),
		axis_test_library_n(fx->t, "SELECT count(DISTINCT file_id) FROM media_stream"), AXIS_ESTIMATE_N_TARGETS,
		seconds);
	g_print("# the snapshot holds %" G_GINT64_FORMAT " facts; Space saver would convert %" G_GINT64_FORMAT
		" files, Music space saver %" G_GINT64_FORMAT "\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM estimate_fact WHERE snapshot_id = %" G_GINT64_FORMAT,
			snapshot),
		axis_test_library_n(fx->t, "SELECT f.paths FROM estimate_fact f" FACT_PROFILE " WHERE f.snapshot_id = %"
			G_GINT64_FORMAT " AND p.slug = 'space_saver' AND f.report = 'total'", snapshot),
		axis_test_library_n(fx->t, "SELECT f.paths FROM estimate_fact f" FACT_PROFILE " WHERE f.snapshot_id = %"
			G_GINT64_FORMAT " AND p.slug = 'music_space_saver' AND f.report = 'total'", snapshot));
	{
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, snapshot);
		const gchar *params[1];
		guint i;

		params[0] = id;
		g_assert_true(axis_db_pool_exec(fx->t->pool,
			"SELECT p.slug, f.dim, f.paths FROM estimate_fact f" FACT_PROFILE " WHERE f.snapshot_id = $1::bigint"
			" AND f.report = 'skip' AND p.slug IN ('space_saver', 'music_space_saver') ORDER BY p.slug, f.paths DESC",
			1, params, &result, &error));
		for (i = 0; i < axis_db_result_get_n_rows(result); i++)
			g_print("#   skipped by %s: %-24s %s\n", axis_db_result_get_text(result, i, 0),
				axis_db_result_get_text(result, i, 1), axis_db_result_get_text(result, i, 2));
	}
	g_assert_cmpfloat(seconds, <=, 600.0);

	estimate_list(estimator, "savings list: eligible, by name", "target=space_saver&outcome=eligible");
	estimate_list(estimator, "savings list: eligible, by size", "target=space_saver&outcome=eligible&sort=size");
	estimate_list(estimator, "savings list: would not shrink", "target=space_saver&outcome=would_not_shrink");
	estimate_list(estimator, "savings list: Dolby Vision (2 %)", "target=space_saver&outcome=dolby_vision");
	estimate_list(estimator, "savings list: HDR10+ (nothing)", "target=space_saver&outcome=hdr10plus");
	estimate_list(estimator, "savings list: pinned, by size", "target=space_saver&outcome=pinned&sort=size");
	estimate_list(estimator, "savings list: music, eligible", "target=music_space_saver&outcome=eligible");
	estimate_list(estimator, "savings list: music, album", "target=music_space_saver&outcome=album");
	estimate_plans(fx, music);

	axis_estimator_unref(estimator);
	axis_settings_unref(settings);
}

static void
test_scale(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *top_id = NULL;
	g_autofree gchar *leaf_id = NULL;
	g_autofree gchar *middle = NULL;
	g_autofree gchar *query = NULL;
	gdouble started;
	gdouble seconds;
	gint64 small_root;
	gint64 wide_root;
	guint i;

	if (fx->t == NULL)
		return;

	g_print("# %u files in %u top folders\n", fx->rows, DIRS);
	started = now_s();
	axis_test_browser_generate(fx->t, fx->volume, fx->root, 1, fx->rows, DIRS);
	g_print("# generating the rows: %.1f s\n", now_s() - started);
	started = now_s();
	axis_test_db_exec(fx->t->db, "ANALYZE library_file");
	axis_test_db_exec(fx->t->db, "ANALYZE media_info");
	g_print("# analyze: %.1f s\n", now_s() - started);
	g_print("# sizes:\n");
	print_sizes(fx);

	/* What the folder totals cost: the big root, then the same cost for
	 * 300,000 files in few and in very many folders, and again unchanged. */
	seconds = axis_test_browser_update_dirs(fx->t, fx->root);
	g_print("# folder totals, %u files, %" G_GINT64_FORMAT " folders: %.1f s\n", fx->rows,
		axis_test_library_n(fx->t, "SELECT count(*) FROM library_dir"), seconds);
	seconds = axis_test_browser_update_dirs(fx->t, fx->root);
	g_print("# folder totals again, nothing changed: %.1f s\n", seconds);

	small_root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Small", AXIS_MEDIA_OTHER);
	axis_test_browser_generate(fx->t, fx->volume, small_root, fx->rows + 1, fx->rows + 300000, 600);
	seconds = axis_test_browser_update_dirs(fx->t, small_root);
	g_print("# folder totals, 300000 files in %" G_GINT64_FORMAT " folders (like the large-tree test): %.1f s\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM library_dir WHERE root_id = %" G_GINT64_FORMAT,
			small_root), seconds);
	wide_root = axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Wide", AXIS_MEDIA_OTHER);
	axis_test_browser_generate(fx->t, fx->volume, wide_root, fx->rows + 300001, fx->rows + 600000, 30000);
	profile_dirs(fx, wide_root);
	seconds = axis_test_browser_update_dirs(fx->t, wide_root);
	g_print("# folder totals, 300000 files in %" G_GINT64_FORMAT " folders (about one file each): %.1f s\n",
		axis_test_library_n(fx->t, "SELECT count(*) FROM library_dir WHERE root_id = %" G_GINT64_FORMAT,
			wide_root), seconds);
	seconds = axis_test_browser_update_dirs(fx->t, wide_root);
	g_print("# folder totals again, nothing changed: %.1f s\n", seconds);

	axis_test_db_exec(fx->t->db, "ANALYZE library_file");
	axis_test_db_exec(fx->t->db, "ANALYZE library_dir");

	/* The flat list: each sort's first page, with the count. */
	check_list(fx, "name, first page", "", FALSE);
	check_list(fx, "name, descending", "dir=desc", FALSE);
	check_list(fx, "size", "sort=size", FALSE);
	check_list(fx, "size, ascending", "sort=size&dir=asc", FALSE);
	check_list(fx, "modified", "sort=modified", FALSE);
	check_list(fx, "duration", "sort=duration", FALSE);
	check_list(fx, "bit rate, ascending", "sort=bitrate&dir=asc", FALSE);

	/* Deep pages. */
	middle = axis_test_library_q(fx->t, "SELECT id FROM library_file WHERE inode = %u", fx->rows / 2);
	query = g_strdup_printf("after=%s", middle);
	check_list(fx, "name, a page in the middle", query, FALSE);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("sort=size&after=%s", middle);
	check_list(fx, "size, a page in the middle", query, FALSE);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("sort=modified&dir=asc&after=%s", middle);
	check_list(fx, "modified, a page in the middle", query, FALSE);
	g_clear_pointer(&query, g_free);
	query = g_strdup_printf("sort=duration&after=%s", middle);
	check_list(fx, "duration, a page in the middle", query, FALSE);
	g_clear_pointer(&query, g_free);

	/* Filters: broad, narrow, and matching nothing. */
	check_list(fx, "kind=video", "kind=video", FALSE);
	check_list(fx, "kind=video, by size", "kind=video&sort=size", FALSE);
	check_list(fx, "vcodec=hevc&res=2160p", "vcodec=hevc&res=2160p", FALSE);
	check_list(fx, "vcodec=hevc&res=2160p, by size", "vcodec=hevc&res=2160p&sort=size", FALSE);
	check_list(fx, "hdr=hdr10, by duration", "hdr=hdr10&sort=duration", FALSE);
	check_list(fx, "hdr=dolby_vision", "hdr=dolby_vision", FALSE);
	check_list(fx, "hdr=uncertain, by size", "hdr=uncertain&sort=size", FALSE);
	check_list(fx, "acodec=eac3&container=flac", "acodec=eac3&container=flac", FALSE);
	check_list(fx, "container=matroska,webm, by bit rate", "container=matroska%2Cwebm&sort=bitrate", FALSE);
	check_list(fx, "probe=failed", "probe=failed", FALSE);
	check_list(fx, "probe=none, by size", "probe=none&sort=size", FALSE);
	check_list(fx, "pinned=yes", "pinned=yes", FALSE);
	check_list(fx, "pinned=yes, by modified", "pinned=yes&sort=modified", FALSE);
	check_list(fx, "state=suspected", "state=suspected", FALSE);
	/* A known limit: the generated names run in the order of the sizes, so
	 * the planner's choice (walk the name index, filter by size) reads
	 * every file before the range.  Real names are not ordered by size;
	 * the page's 5 s deadline bounds the worst case in any event. */
	check_list(fx, "size 1500 to 1520 MiB, by name", "min_mib=1500&max_mib=1520", TRUE);
	check_list(fx, "size 1500 to 1520 MiB, by size", "min_mib=1500&max_mib=1520&sort=size", FALSE);
	check_list(fx, "size 1500 to 1520 MiB, by modified", "min_mib=1500&max_mib=1520&sort=modified", FALSE);
	check_list(fx, "root only", "root=1", FALSE);
	/* A known limit: five filters, of which the indexes serve one and the
	 * rest are checked row by row (about 100,000 rows, cold cache). */
	check_list(fx, "root, a narrow filter", "root=1&vcodec=hevc&res=2160p&hdr=hdr10&pinned=yes", TRUE);
	check_list(fx, "vcodec=vp9 (nothing matches)", "vcodec=vp9", FALSE);
	check_list(fx, "vcodec=vp9, by size", "vcodec=vp9&sort=size", FALSE);
	check_list(fx, "res=sd&acodec=nothing, by modified", "res=sd&acodec=nothing&sort=modified", FALSE);

	/* Names: the text a name contains. */
	check_list(fx, "name contains (common)", "name=f001", FALSE);
	check_list(fx, "name contains (one file)", "name=f00123456", FALSE);
	check_list(fx, "name contains (nothing)", "name=zzzz", FALSE);
	check_list(fx, "name contains (one file), by size", "name=f00123456&sort=size", FALSE);
	check_list(fx, "name contains .flac, by modified", "name=.flac&sort=modified", FALSE);

	/* Folders. */
	top_id = axis_test_library_q(fx->t, "SELECT id FROM library_dir WHERE root_id = %" G_GINT64_FORMAT
		" AND path = ''", fx->root);
	leaf_id = axis_test_library_q(fx->t, "SELECT id FROM library_dir WHERE root_id = %" G_GINT64_FORMAT
		" AND path = 'd7/s3'", fx->root);
	{
		const gchar *params[5];
		g_autofree gchar *after_dir = axis_test_library_q(fx->t,
			"SELECT id FROM library_dir WHERE parent_id = %s ORDER BY name, id OFFSET 1000 LIMIT 1", top_id);
		g_autofree gchar *after_name = axis_test_library_q(fx->t, "SELECT name FROM library_dir WHERE id = %s",
			after_dir);
		g_autofree gchar *after_file = axis_test_library_q(fx->t,
			"SELECT f.id FROM library_dir d JOIN library_file f ON f.root_id = d.root_id AND f.dir_hash = d.path_hash"
			" WHERE d.id = %s ORDER BY f.base_name, f.id OFFSET 50 LIMIT 1", leaf_id);
		g_autofree gchar *after_file_name = axis_test_library_q(fx->t,
			"SELECT base_name FROM library_file WHERE id = %s", after_file);
		g_autofree gchar *limit = g_strdup_printf("%d", AXIS_LIBRARY_PAGE_SIZE + 1);

		params[0] = top_id;
		params[1] = limit;
		check_statement(fx, "folder: the root's subfolders", AXIS_LIBRARY_SQL_DIRS_FIRST, params, 2);
		params[2] = after_name;
		params[3] = after_dir;
		check_statement(fx, "folder: its subfolders, a later page", AXIS_LIBRARY_SQL_DIRS_AFTER, params, 4);
		/* Filtered by name: the same index, among the folder's children. */
		params[4] = "%1%";
		check_statement(fx, "folder: subfolders by name, a later page", AXIS_LIBRARY_SQL_DIRS_NAMED_AFTER,
			params, 5);
		params[2] = "%no such name%";
		check_statement(fx, "folder: subfolders by a name none has", AXIS_LIBRARY_SQL_DIRS_NAMED_FIRST, params, 3);
		params[0] = leaf_id;
		check_statement(fx, "folder: a leaf's files", AXIS_LIBRARY_SQL_FILES_FIRST, params, 2);
		params[2] = after_file_name;
		params[3] = after_file;
		check_statement(fx, "folder: a leaf's files, a later page", AXIS_LIBRARY_SQL_FILES_AFTER, params, 4);
		params[0] = top_id;
		check_statement(fx, "folder: the root's own files", AXIS_LIBRARY_SQL_FILES_FIRST, params, 2);
	}
	for (i = 0; i < 3; i++) {
		g_autoptr(AxisFolderView) view = NULL;
		const gchar *id = i == 0 ? top_id : leaf_id;
		gint64 s = g_get_monotonic_time();

		g_assert_true(axis_library_get_folder(fx->t->library, g_ascii_strtoll(id, NULL, 10), 0, 0, &view, &error));
		g_print("# a folder's whole page (%s), 5 statements: %.1f ms\n", i == 0 ? "the top" : "a leaf",
			(g_get_monotonic_time() - s) / 1000.0);
		g_assert_cmpint(g_get_monotonic_time() - s, <=, LIMIT_US);
	}

	test_analysis(fx);
	test_estimate(fx);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* Progress is visible while it runs, also when the output is a file. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	g_test_add("/library/scale/queries", Fixture, NULL, fixture_setup, test_scale, fixture_teardown);

	return g_test_run();
}
