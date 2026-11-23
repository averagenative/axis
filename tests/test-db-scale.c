/*
 * test-db-scale.c - the database's statements where the library and its
 * history are large (scale-database-first-batch)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One generated database at the design scale: 5,000 series of 60 episodes,
 * 20,000 movies, 400,000 library files, 1,000,000 stored search results,
 * 250,000 grabs, 300,000 imports with 3,000,000 files, 3,000,000 history
 * events and 2,000,000 jobs of which 300 wait or run.  Every row is made by
 * the database (generate_series), none by a file.
 *
 *   AXIS_DB_SCALE   the share of that scale, in percent (1 to 100).  The
 *                   default is 10, which `make check` runs; 100 is the
 *                   design scale, for a run by hand:
 *                     make DEBUG=1 build/gcc-debug/tests/test-db-scale
 *                     AXIS_DB_SCALE=100 tests/with-postgres.sh \
 *                       build/gcc-debug/tests/test-db-scale
 *
 * What is measured is what ships: the daemon's own functions are called,
 * the statements they send are told to the test by the pool
 * (axis_db_pool_set_statement_observer()), and each is then explained with
 * the values it was sent with (EXPLAIN (ANALYZE, BUFFERS)).  A statement
 * that changes rows is explained in a transaction that is rolled back.
 *
 * Twice: before (the indexes of 0080_scale_indexes.sql dropped, and the
 * statements this change rewrote as they were), then the indexes are
 * created one at a time (each timed: what the migration costs at an
 * upgrade), and after.  The table is printed at the end; `-p` picks a case:
 *
 *   /db/scale/fixes     what this change fixed, before and after, with the
 *                       bounds `make check` holds
 *   /db/scale/report    what is only measured (the next batch); at 2 % of
 *                       the scale unless AXIS_DB_SCALE says otherwise
 *
 * Every title here is made up ("Generated Series 12").
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "axis-test-browser.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-web.h"
#include "api/axis-worker-proto.h"
#include "arr/axis-arr.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-analysis.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "transcode/axis-estimator.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-jobs-data.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"
#include "youtube/axis-youtube.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

/* The design scale. */
#define FULL_SERIES        5000
#define SEASONS            3
#define EPISODES_A_SEASON  20
#define FULL_MOVIES        20000
#define FULL_OTHER_FILES   135000
#define FULL_JOBS          2000000
#define FULL_MANUAL        50000
#define LIVE_JOBS          300
#define REMOVED_TITLES     200

/* What `make check` holds, at a tenth of the scale and under the
 * sanitizers: generous, and of the database's own time (the plan's
 * "Execution Time"), which the sanitizers do not slow. */
#define BOUND_LIVE_JOBS_MS   50.0
#define BOUND_DELETE_MS      5000.0
#define BOUND_LIST_MS        100.0

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	gint64                films_root_id;
	gint64                other_root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisProbe            *probe;
	AxisImport           *import;
	AxisMovies           *movies;
	AxisTv               *tv;
	AxisModuleRegistry   *registry;
	AxisJobRegistry      *kinds;
	AxisWebHub           *hub;
	AxisWebJobs          *jobs;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
	guint                 percent;
	guint                 n_delete;
	GPtrArray            *rows;	/* Row: the table printed at the end */
} Fixture;

/* ── What the pool's connections send ───────────────────────────── */

typedef struct
{
	gchar     *sql;
	GPtrArray *params;	/* gchar *, NULL for SQL NULL */
} Sent;

static GMutex sent_lock;
static GPtrArray *sent_now;	/* NULL: not listening */

static void
sent_free(Sent *sent)
{
	guint i;

	for (i = 0; i < sent->params->len; i++)
		g_free(g_ptr_array_index(sent->params, i));
	g_ptr_array_free(sent->params, TRUE);
	g_free(sent->sql);
	g_free(sent);
}

static void
on_statement(
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	gpointer            data
){
	guint i;

	g_mutex_lock(&sent_lock);
	if (sent_now != NULL) {
		Sent *sent = g_new0(Sent, 1);

		sent->sql = g_strdup(sql);
		sent->params = g_ptr_array_new();
		for (i = 0; i < n_params; i++)
			g_ptr_array_add(sent->params, g_strdup(params[i]));
		g_ptr_array_add(sent_now, sent);
	}
	g_mutex_unlock(&sent_lock);
}

static void
listen_begin(void)
{
	g_mutex_lock(&sent_lock);
	g_clear_pointer(&sent_now, g_ptr_array_unref);
	sent_now = g_ptr_array_new_with_free_func((GDestroyNotify)sent_free);
	g_mutex_unlock(&sent_lock);
}

/* What was sent since listen_begin(). */
static GPtrArray *
listen_end(void)
{
	GPtrArray *all;

	g_mutex_lock(&sent_lock);
	all = g_steal_pointer(&sent_now);
	g_mutex_unlock(&sent_lock);
	return all;
}

/* The first statement of @all that contains @part, or NULL. */
static const Sent *
sent_find(
	GPtrArray   *all,
	const gchar *part
){
	guint i;

	for (i = 0; i < all->len; i++) {
		const Sent *sent = g_ptr_array_index(all, i);

		if (strstr(sent->sql, part) != NULL)
			return sent;
	}
	return NULL;
}

/* @sent with its values written in: $n as a literal (the highest first, so
 * that $1 is not taken out of $10). */
static gchar *
sent_literal(const Sent *sent)
{
	gchar *text = g_strdup(sent->sql);
	guint i;

	for (i = sent->params->len; i > 0; i--) {
		const gchar *value = g_ptr_array_index(sent->params, i - 1);
		g_autofree gchar *name = g_strdup_printf("$%u", i);
		g_auto(GStrv) parts = g_strsplit(text, name, -1);
		g_autofree gchar *literal = NULL;

		if (value == NULL) {
			literal = g_strdup("NULL");
		} else {
			g_auto(GStrv) quotes = g_strsplit(value, "'", -1);
			g_autofree gchar *doubled = g_strjoinv("''", quotes);

			literal = g_strdup_printf("'%s'", doubled);
		}
		g_free(text);
		text = g_strjoinv(literal, parts);
	}
	return text;
}

/* ── The fixture ────────────────────────────────────────────────── */

static guint
env_percent(void)
{
	const gchar *text = g_getenv("AXIS_DB_SCALE");
	guint64 value = text != NULL ? g_ascii_strtoull(text, NULL, 10) : 0;

	return value >= 1 && value <= 100 ? (guint)value : 10;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *base = NULL;
	AxisDownloadClientSpec client;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->percent = env_percent();
	fx->n_delete = fx->percent == 100 ? 1000 : 100;
	fx->rows = g_ptr_array_new();
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	fx->films_root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->other_root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "other", AXIS_MEDIA_OTHER);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->downloads, fx->probe);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_tv_set_clock(fx->tv, NOW_US);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), fx->tv, &error));
	fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(fx->t->pool, fx->hub);
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	/* The kinds of jobs as the daemon knows them: which are upkeep. */
	fx->kinds = axis_job_registry_new();
	g_assert_true(axis_job_registry_add_modules(fx->kinds, fx->registry, &error));
	g_assert_no_error(error);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 600, "idle-timeout", 600, NULL);

	fx->fake = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(fx->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &client, &fx->client_id, &error));
	g_assert_no_error(error);
	axis_db_pool_set_statement_observer(fx->t->pool, on_statement, NULL);
	/* The helper's own session explains: as a pooled session plans. */
	axis_test_db_exec(fx->t->db, "SET jit = off");
	axis_test_db_exec(fx->t->db, "SET TimeZone = 'UTC'");
}

typedef struct
{
	gchar   *name;
	gchar   *phase;
	gdouble  wall_ms;	/* of the daemon's function; < 0: not timed */
	gdouble  exec_ms;	/* the plan's "Execution Time" */
	gchar   *lines;		/* the plan's lines that matter */
} Row;

static void
row_free(Row *row)
{
	g_free(row->name);
	g_free(row->phase);
	g_free(row->lines);
	g_free(row);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_db_pool_set_statement_observer(fx->t->pool, NULL, NULL);
	g_mutex_lock(&sent_lock);
	g_clear_pointer(&sent_now, g_ptr_array_unref);
	g_mutex_unlock(&sent_lock);
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_jobs_stop(fx->jobs);
	axis_tv_stop(fx->tv);
	axis_movies_stop(fx->movies);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_job_registry_unref(fx->kinds);
	axis_module_registry_free(fx->registry);
	axis_tv_unref(fx->tv);
	axis_movies_unref(fx->movies);
	axis_import_unref(fx->import);
	axis_probe_unref(fx->probe);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_downloads_free(fx->downloads_ui);
	axis_downloads_unref(fx->downloads);
	axis_web_indexers_free(fx->indexers_ui);
	axis_indexers_unref(fx->indexers);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_settings_free(fx->web_settings);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_ptr_array_set_free_func(fx->rows, (GDestroyNotify)row_free);
	g_ptr_array_unref(fx->rows);
	g_free(fx->disk);
	g_free(fx->admin);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
sql(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	axis_test_db_exec(fx->t->db, text);
}

static gint64
qi(
	Fixture     *fx,
	const gchar *text
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, text);

	return value != NULL ? g_ascii_strtoll(value, NULL, 10) : 0;
}

/* @n of the design scale at this run's share of it. */
static guint
scaled(
	Fixture *fx,
	guint    n
){
	return MAX((guint)((guint64)n * fx->percent / 100), 1);
}

/* The library's new files for the episodes @where chooses, each linked to
 * its episode: one statement. */
static void
add_files(
	Fixture     *fx,
	const gchar *where,
	const gchar *quality,
	const gchar *tag
){
	sql(fx, "WITH lf AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'generated/%s/' || e.id || '.mkv', 'video', 700000000 + e.id, 1, 0, 1, 0,"
		" (SELECT coalesce(max(inode), 5000000) FROM library_file) + row_number() OVER (), 1, false, false, false, 1, 1, 1"
		" FROM series_episode e WHERE %s RETURNING id, rel_path),"
		" m AS (SELECT lf.id AS file_id, split_part(split_part(lf.rel_path, '/', 3), '.', 1)::bigint AS episode_id FROM lf),"
		" sf AS (INSERT INTO series_file (file_id, series_id, quality, release_name) SELECT m.file_id, e.series_id,"
		" %s, 'Generated.Series.S01E01.1080p.WEB-DL-GRP' FROM m JOIN series_episode e ON e.id = m.episode_id"
		" RETURNING file_id)"
		" INSERT INTO series_file_episode (file_id, episode_id) SELECT m.file_id, m.episode_id FROM m", fx->root_id,
		fx->volume_id, tag, where, quality);
}

/* ARRAY['a', 'b'] of @names, or of @fallback when there is none. */
static gchar *
sql_array(
	GStrv        names,
	const gchar *fallback
){
	GString *text = g_string_new("ARRAY[");
	guint i;

	for (i = 0; names != NULL && names[i] != NULL; i++)
		g_string_append_printf(text, "%s'%s'", i > 0 ? ", " : "", names[i]);
	if (i == 0)
		g_string_append_printf(text, "'%s'", fallback);
	g_string_append_c(text, ']');
	return g_string_free(text, FALSE);
}

/*
 * The database at this run's share of the design scale, as a library has it
 * after some months: everything imported long ago, a little in flight.
 */
static void
make_database(Fixture *fx)
{
	gint64 began = g_get_monotonic_time();
	guint series = scaled(fx, FULL_SERIES);
	guint movies = scaled(fx, FULL_MOVIES);
	g_auto(GStrv) upkeep_kinds = axis_job_registry_list_class(fx->kinds, AXIS_JOB_CLASS_UPKEEP);
	g_auto(GStrv) work_kinds = axis_job_registry_list_class(fx->kinds, AXIS_JOB_CLASS_WORK);
	g_autofree gchar *upkeep = sql_array(upkeep_kinds, "download.poll");
	g_autofree gchar *work = sql_array(work_kinds, "tv.search");

	/* The series: three seasons of twenty episodes, aired and monitored but
	 * for one series in ten whose last season airs now. */
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2019, 'ended', 'fixture', 'z' || i FROM generate_series(1, %u) i"
		" RETURNING id), s AS (INSERT INTO series (title_id, quality_profile_id, root_id, synced_at) SELECT id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT ", now() FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, g, true FROM s,"
		" generate_series(1, %d) g RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT s.id, g, e, 'Part ' || e, CASE WHEN g = %d AND s.id %% 10 = 0 THEN DATE '2026-11-10' + e"
		"  ELSE DATE '2019-03-04' + e END, true FROM s, generate_series(1, %d) g, generate_series(1, %d) e", series,
		fx->root_id, SEASONS, SEASONS, SEASONS, EPISODES_A_SEASON);
	/* Five episodes in six have a file, one in seven of them below the
	 * profile's cutoff. */
	add_files(fx, "e.id % 6 <> 0 AND e.air_date < DATE '2026-01-01'",
		"CASE WHEN e.id % 7 = 0 THEN 'hdtv_720p' ELSE 'webdl_1080p' END", "z");
	/* The others were searched for twice and nothing was found; one episode
	 * in fifteen was searched for twice with twenty releases found each
	 * time, which are kept. */
	sql(fx, "INSERT INTO series_search (series_id, cause, started_at, finished_at, outcome, outcome_text, scope,"
		" episode_id) SELECT e.series_id, 'scheduled', now() - interval '2 hours', now() - interval '2 hours',"
		" 'nothing_found', 'As the test made it.', 'episode', e.id FROM series_episode e, generate_series(1, 2) k"
		" WHERE e.id %% 6 = 0 AND e.air_date < DATE '2026-01-01'");
	sql(fx, "INSERT INTO series_search (series_id, cause, started_at, finished_at, outcome, outcome_text, scope,"
		" episode_id, n_found) SELECT e.series_id, 'scheduled', now() - interval '3 hours', now() - interval '3 hours',"
		" 'grabbed', 'As the test made it.', 'episode', e.id, 20 FROM series_episode e, generate_series(1, 2) k"
		" WHERE e.id %% 15 = 1 AND e.air_date < DATE '2026-01-01'");
	sql(fx, "INSERT INTO series_candidate (search_id, position, release_id, source_id, name, protocol, name_key,"
		" quality, match, match_text, accepted, safety, mismatched) SELECT ss.id, p, ss.id * 100 + p, 1,"
		" 'Generated.Series.E' || ss.id || '.' || p || '.1080p.WEB-DL-GRP', 'usenet', 'generated-' || ss.id || '-' || p,"
		" 'webdl_1080p', CASE WHEN p %% 5 = 0 THEN 'none' ELSE 'match' END, 'As the test made it.', p = 1, false,"
		" p %% 9 = 0 FROM series_search ss, generate_series(1, 20) p WHERE ss.outcome = 'grabbed'");
	/* Every file came from a grab that is closed; the grabs of the searches
	 * kept still name their search and the release they took. */
	sql(fx, "WITH g AS (INSERT INTO series_grab (series_id, release_name, protocol, name_key, sent_at, completed_at,"
		" closed_at, close_reason, scope, episode_id) SELECT e.series_id, 'Generated.Series.E' || e.id ||"
		" '.1080p.WEB-DL-GRP', 'usenet', 'generated-' || e.id, now() - interval '30 days', now() - interval '30 days',"
		" now() - interval '30 days', 'imported', 'episode', e.id FROM series_episode e JOIN series_file_episode sfe"
		" ON sfe.episode_id = e.id RETURNING id, episode_id)"
		" INSERT INTO series_grab_episode (grab_id, episode_id, open) SELECT g.id, g.episode_id, false FROM g");
	sql(fx, "UPDATE series_grab g SET search_id = x.search_id, candidate_id = x.id FROM (SELECT DISTINCT ON"
		" (ss.episode_id) ss.episode_id, c.search_id, c.id FROM series_candidate c JOIN series_search ss"
		" ON ss.id = c.search_id WHERE c.position = 1 ORDER BY ss.episode_id, ss.id DESC) x"
		" WHERE x.episode_id = g.episode_id");

	/* The movies: three in four have a file; one in ten needs attention, a
	 * few are being searched for; half were searched with releases kept. */
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, release_date, provider, provider_id)"
		" SELECT 'film', 'Generated Film ' || i, 1950 + i %% 75, 'released',"
		" CASE WHEN i %% 20 = 0 THEN DATE '2099-01-01' ELSE DATE '2000-01-01' + i %% 7000 END, 'fixture', 'gen' || i"
		" FROM generate_series(1, %u) i RETURNING id, provider_id) INSERT INTO movie (title_id, quality_profile_id,"
		" root_id, added_at) SELECT t.id, (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT
		", now() - make_interval(secs => substr(t.provider_id, 4)::int) FROM t", movies, fx->films_root_id);
	sql(fx, "CREATE TEMP TABLE gen AS SELECT m.id AS movie_id, m.title_id, substr(t.provider_id, 4)::int AS i"
		" FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE t.provider_id LIKE 'gen%%'");
	sql(fx, "WITH f AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
		" first_seen_generation, last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", 'gen/' || i || '.mkv', 'video', i::bigint * 1048576, 1, 0, 1, 0, 1000000 + i, 1, false,"
		" false, false, 1, 1, 1 FROM gen WHERE i %% 4 <> 0 RETURNING id, rel_path)"
		" INSERT INTO movie_file (file_id, movie_id, quality) SELECT f.id, g.movie_id,"
		" CASE WHEN g.i %% 20 = 5 THEN 'webdl_720p' ELSE 'remux_2160p' END"
		" FROM f JOIN gen g ON f.rel_path = 'gen/' || g.i || '.mkv'", fx->films_root_id, fx->volume_id);
	sql(fx, "INSERT INTO movie_search (movie_id, cause, finished_at, outcome, outcome_text)"
		" SELECT movie_id, 'scheduled', now(), 'no_indexer', 'No indexer source is set up.' FROM gen WHERE i %% 40 = 0");
	sql(fx, "INSERT INTO movie_search (movie_id, cause, started_at, finished_at, outcome, outcome_text, n_found)"
		" SELECT movie_id, 'scheduled', now() - interval '3 hours', now() - interval '3 hours', 'grabbed',"
		" 'As the test made it.', 20 FROM gen WHERE i %% 2 = 1");
	sql(fx, "INSERT INTO movie_candidate (search_id, position, release_id, source_id, name, protocol, name_key,"
		" quality, match, match_text, accepted, safety) SELECT ms.id, p, ms.id * 100 + p, 1,"
		" 'Generated.Film.' || ms.id || '.' || p || '.1080p.WEB-DL-GRP', 'usenet', 'generated-' || ms.id || '-' || p,"
		" 'webdl_1080p', 'match', 'As the test made it.', p = 1, false FROM movie_search ms, generate_series(1, 20) p"
		" WHERE ms.outcome = 'grabbed'");
	sql(fx, "INSERT INTO movie_grab (movie_id, search_id, candidate_id, release_name, protocol, name_key, sent_at,"
		" completed_at, closed_at, close_reason) SELECT mf.movie_id, c.search_id, c.id, 'Generated.Film.' || mf.movie_id"
		" || '.1080p.WEB-DL-GRP', 'usenet', 'generated-' || mf.movie_id, now() - interval '30 days', now() - interval"
		" '30 days', now() - interval '30 days', 'imported' FROM movie_file mf LEFT JOIN movie_search ms"
		" ON ms.movie_id = mf.movie_id AND ms.outcome = 'grabbed' LEFT JOIN movie_candidate c ON c.search_id = ms.id"
		" AND c.position = 1");
	/* The files nothing manages, with what a probe says of them. */
	axis_test_browser_generate(fx->t, fx->volume_id, fx->other_root_id, 1, scaled(fx, FULL_OTHER_FILES), 2000);

	/* Every episode's file came by a download that an import took, ten
	 * files a download; further imports were dismissed by a person; a few
	 * wait or are blocked. */
	sql(fx, "CREATE TEMP TABLE gen_ep AS SELECT e.id AS episode_id, e.series_id, s.title_id, sfe.file_id"
		" FROM series_episode e JOIN series s ON s.id = e.series_id JOIN series_file_episode sfe ON sfe.episode_id = e.id");
	sql(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, title_id,"
		" title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at, completed_at,"
		" finished_at) SELECT %" G_GINT64_FORMAT ", 'SABnzbd', 'gen-' || g.episode_id, 'usenet', 'file',"
		" 'Generated.Series.E' || g.episode_id || '.1080p.WEB-DL-GRP', g.title_id, 'Generated Series', 'tv', 'imported',"
		" 1000, 1000, '/data/downloads/generated-' || g.episode_id, %" G_GINT64_FORMAT ", 'downloads/generated-' ||"
		" g.episode_id, now() - interval '30 days', now() - interval '30 days', now() - interval '30 days' FROM gen_ep g",
		fx->client_id, fx->volume_id);
	sql(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, title_id,"
		" title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at)"
		" SELECT %" G_GINT64_FORMAT ", 'SABnzbd', 'live-' || i, 'usenet', 'file', 'Generated.Film.' || i ||"
		" '.1080p.WEB-DL-GRP', NULL, 'Generated Film', 'movies', 'downloading', 1000, 400, '/data/downloads/live-' || i,"
		" %" G_GINT64_FORMAT ", 'downloads/live-' || i, now() FROM generate_series(1, 40) i", fx->client_id,
		fx->volume_id);
	sql(fx, "INSERT INTO import (module, owner_id, title_id, title_label, source_path, source_volume_id, state,"
		" download_id, release_name, protocol, created_at, updated_at, finished_at) SELECT 'tv', g.series_id,"
		" g.title_id, 'Generated Series', d.rel_path, d.volume_id, 'imported', d.id, d.release_name, 'usenet', x.t, x.t,"
		" x.t FROM download d JOIN gen_ep g ON d.client_ref = 'gen-' || g.episode_id, LATERAL (SELECT now() -"
		" (g.episode_id %% 40000) * interval '1 minute' AS t) x");
	sql(fx, "INSERT INTO import (module, title_label, source_path, source_volume_id, state, kind, created_at,"
		" updated_at, finished_at) SELECT 'movies', 'Generated Film', 'manual/generated-' || i, %" G_GINT64_FORMAT ","
		" 'dismissed', 'manual', x.t, x.t, x.t FROM generate_series(1, %u) i, LATERAL (SELECT now() - (i %% 40000) *"
		" interval '1 minute' AS t) x", fx->volume_id, scaled(fx, FULL_MANUAL));
	sql(fx, "INSERT INTO import (module, owner_id, title_label, source_path, source_volume_id, state, reason_text)"
		" SELECT 'tv', s.id, 'Generated Series', 'downloads/open-' || s.id, %" G_GINT64_FORMAT ","
		" CASE WHEN s.id %% 3 = 0 THEN 'waiting' ELSE 'blocked' END, 'As the test made it.' FROM series s"
		" ORDER BY s.id LIMIT 60", fx->volume_id);
	sql(fx, "INSERT INTO import_file (import_id, position, path, kind, size, accepted, library_file_id)"
		" SELECT i.id, p, 'file' || p || '.mkv', CASE WHEN p = 1 THEN 'video' ELSE 'other' END, 1000, p = 1,"
		" CASE WHEN p = 1 THEN g.file_id END FROM import i LEFT JOIN download d ON d.id = i.download_id"
		" LEFT JOIN gen_ep g ON d.client_ref = 'gen-' || g.episode_id, generate_series(1, 10) p");

	/* What happened: nine events an episode, fifteen a movie, and the
	 * history of titles that were removed since (their record is left). */
	sql(fx, "INSERT INTO series_event (series_id, title_id, title_label, episode_id, kind, text, created_at)"
		" SELECT e.series_id, s.title_id, 'Generated Series', e.id, (ARRAY['searched', 'grabbed', 'completed',"
		" 'imported'])[1 + k %% 4], 'As the test made it.', now() - (e.id %% 40000) * interval '1 minute'"
		" FROM series_episode e JOIN series s ON s.id = e.series_id, generate_series(1, 9) k");
	sql(fx, "INSERT INTO movie_event (movie_id, title_id, title_label, kind, text, created_at) SELECT g.movie_id,"
		" g.title_id, 'Generated Film', (ARRAY['searched', 'grabbed', 'completed'])[1 + k %% 3], 'As the test made it.',"
		" now() - (g.i %% 40000) * interval '1 minute' FROM gen g, generate_series(1, 15) k");
	sql(fx, "INSERT INTO meta_title (kind, title, year, status, provider, provider_id) SELECT 'film',"
		" 'Generated Removed ' || i, 2001, 'released', 'fixture', 'u' || i FROM generate_series(1, %d) i",
		REMOVED_TITLES);
	sql(fx, "INSERT INTO movie_event (title_id, title_label, kind, text) SELECT t.id, t.title, 'removed',"
		" 'As the test made it.' FROM meta_title t, generate_series(1, 20) k WHERE t.provider_id LIKE 'u%%'");
	sql(fx, "INSERT INTO series_event (title_id, title_label, kind, text) SELECT t.id, t.title, 'removed',"
		" 'As the test made it.' FROM meta_title t, generate_series(1, 20) k WHERE t.provider_id LIKE 'u%%'");
	sql(fx, "INSERT INTO import (module, title_id, title_label, source_path, state, kind, finished_at) SELECT 'movies',"
		" t.id, t.title, 'manual/removed-' || t.id || '-' || k, 'dismissed', 'manual', now() FROM meta_title t,"
		" generate_series(1, 2) k WHERE t.provider_id LIKE 'u%%'");

	/* The jobs: what some weeks leave behind (half of it upkeep, a fiftieth
	 * failed), a few old probes, and what waits or runs now. */
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, last_error, created_at, started_at,"
		" finished_at) SELECT CASE WHEN i %% 2 = 0 THEN (%s)[1 + (i / 2) %% %u] ELSE (%s)[1 + (i / 2) %% %u] END,"
		" 'queue' || i %% 6, (CASE WHEN i %% 50 = 7 THEN 'failed' WHEN i %% 100 = 9 THEN 'cancelled' ELSE 'succeeded'"
		" END)::job_state, 2, 1, CASE WHEN i %% 50 = 7 THEN 'As the test made it.' END, x.t, x.t, x.t"
		" FROM generate_series(1, %u) i, LATERAL (SELECT now() - (i %% 20000) * interval '1 minute' AS t) x"
		" WHERE (%s)[1 + (i / 2) %% %u] <> 'probe.file' OR i %% 2 = 0", upkeep, g_strv_length(upkeep_kinds) > 0 ?
		g_strv_length(upkeep_kinds) : 1, work, g_strv_length(work_kinds) > 0 ? g_strv_length(work_kinds) : 1,
		scaled(fx, FULL_JOBS), work, g_strv_length(work_kinds) > 0 ? g_strv_length(work_kinds) : 1);
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, created_at, started_at, finished_at)"
		" SELECT 'probe.file', 'probes', 'succeeded', 2, 1, x.t, x.t, x.t FROM generate_series(1, 50) i, LATERAL"
		" (SELECT now() - interval '13 days' - i * interval '1 minute' AS t) x");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) SELECT CASE WHEN i %% 5 = 0 THEN"
		" (%s)[1 + i %% %u] ELSE (%s)[1 + i %% %u] END, 'queue' || i %% 6, 'queued', 2, 'generated:' || i"
		" FROM generate_series(1, %d) i WHERE i %% 6 <> 0", upkeep, g_strv_length(upkeep_kinds) > 0 ?
		g_strv_length(upkeep_kinds) : 1, work, g_strv_length(work_kinds) > 0 ? g_strv_length(work_kinds) : 1, LIVE_JOBS);
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, lease_token, lease_expires_at, started_at)"
		" SELECT (%s)[1 + i %% %u], 'queue' || i %% 6, 'running', 2, 1, gen_random_uuid(), now() + interval '5 minutes',"
		" now() FROM generate_series(1, %d) i WHERE i %% 6 = 0", work, g_strv_length(work_kinds) > 0 ?
		g_strv_length(work_kinds) : 1, LIVE_JOBS);

	sql(fx, "VACUUM ANALYZE");
	g_print("the database at %u %% of the design scale took %.1f s to make:\n", fx->percent,
		(g_get_monotonic_time() - began) / 1000000.0);
	g_print("  %" G_GINT64_FORMAT " series, %" G_GINT64_FORMAT " episodes, %" G_GINT64_FORMAT " episode files, %"
		G_GINT64_FORMAT " movies, %" G_GINT64_FORMAT " library files\n", qi(fx, "SELECT count(*) FROM series"),
		qi(fx, "SELECT count(*) FROM series_episode"), qi(fx, "SELECT count(*) FROM series_file"),
		qi(fx, "SELECT count(*) FROM movie"), qi(fx, "SELECT count(*) FROM library_file"));
	g_print("  %" G_GINT64_FORMAT " searches with %" G_GINT64_FORMAT " stored results, %" G_GINT64_FORMAT " grabs, %"
		G_GINT64_FORMAT " downloads\n",
		qi(fx, "SELECT (SELECT count(*) FROM series_search) + (SELECT count(*) FROM movie_search)"),
		qi(fx, "SELECT (SELECT count(*) FROM series_candidate) + (SELECT count(*) FROM movie_candidate)"),
		qi(fx, "SELECT (SELECT count(*) FROM series_grab) + (SELECT count(*) FROM movie_grab)"),
		qi(fx, "SELECT count(*) FROM download"));
	g_print("  %" G_GINT64_FORMAT " imports with %" G_GINT64_FORMAT " files, %" G_GINT64_FORMAT " history events, %"
		G_GINT64_FORMAT " jobs of which %" G_GINT64_FORMAT " wait or run\n", qi(fx, "SELECT count(*) FROM import"),
		qi(fx, "SELECT count(*) FROM import_file"),
		qi(fx, "SELECT (SELECT count(*) FROM series_event) + (SELECT count(*) FROM movie_event)"),
		qi(fx, "SELECT count(*) FROM job"), qi(fx, "SELECT count(*) FROM job WHERE state IN ('queued', 'running')"));
}

/* ── Explaining ─────────────────────────────────────────────────── */

/* The plan of @statement (its values written in), from EXPLAIN (ANALYZE,
 * BUFFERS) on the helper's session; @undo: in a transaction rolled back,
 * after @first (nullable) in it. */
static gchar *
plan_of(
	Fixture     *fx,
	const gchar *statement,
	gboolean     undo,
	const gchar *first
){
	g_autofree gchar *text = g_strdup_printf("DO $do$ DECLARE plan_row record; BEGIN CREATE TEMP TABLE IF NOT EXISTS"
		" plan_lines (n serial, l text); TRUNCATE plan_lines; FOR plan_row IN EXPLAIN (ANALYZE, BUFFERS) %s LOOP"
		" INSERT INTO plan_lines (l) VALUES (plan_row.\"QUERY PLAN\"); END LOOP; END $do$", statement);
	gchar *plan;

	if (undo)
		axis_test_db_exec(fx->t->db, "BEGIN");
	if (first != NULL)
		axis_test_db_exec(fx->t->db, first);
	axis_test_db_exec(fx->t->db, text);
	plan = axis_test_db_query(fx->t->db, "SELECT string_agg(l, E'\\n' ORDER BY n) FROM plan_lines");
	if (undo)
		axis_test_db_exec(fx->t->db, "ROLLBACK");
	return plan != NULL ? plan : g_strdup("");
}

/* The plan's "Execution Time", in milliseconds. */
static gdouble
plan_ms(const gchar *plan)
{
	const gchar *at = strstr(plan, "Execution Time: ");

	return at != NULL ? g_ascii_strtod(at + strlen("Execution Time: "), NULL) : -1;
}

/* The lines of @plan that contain one of @needles ('|' between them),
 * trimmed, at most eight, " ;; " between them.  Of the foreign keys'
 * triggers, only the ones that took a millisecond and more. */
static gchar *
plan_lines(
	const gchar *plan,
	const gchar *needles
){
	g_auto(GStrv) lines = g_strsplit(plan, "\n", -1);
	g_auto(GStrv) wanted = g_strsplit(needles, "|", -1);
	GString *out = g_string_new(NULL);
	guint taken = 0;
	guint i;
	guint k;

	for (i = 0; lines[i] != NULL && taken < 8; i++) {
		const gchar *trigger = strstr(lines[i], "Trigger for constraint");
		const gchar *time = trigger != NULL ? strstr(trigger, "time=") : NULL;

		if (time != NULL && g_ascii_strtod(time + 5, NULL) < 1.0)
			continue;
		for (k = 0; wanted[k] != NULL; k++) {
			if (strstr(lines[i], wanted[k]) != NULL) {
				g_string_append_printf(out, "%s%s", taken > 0 ? " ;; " : "", g_strstrip(lines[i]));
				taken++;
				break;
			}
		}
	}
	return g_string_free(out, FALSE);
}

/* Where the time of @plan went: of the nodes that took a third of the whole
 * and more (their children's time included), the three deepest. */
static gchar *
plan_heavy(const gchar *plan)
{
	g_auto(GStrv) lines = g_strsplit(plan, "\n", -1);
	gdouble whole = plan_ms(plan);
	const gchar *picked[3] = { NULL, NULL, NULL };
	guint depth[3] = { 0, 0, 0 };
	GString *out = g_string_new(NULL);
	guint i;
	guint k;

	for (i = 1; lines[i] != NULL; i++) {
		const gchar *actual = strstr(lines[i], "actual time=");
		const gchar *upto = actual != NULL ? strstr(actual, "..") : NULL;
		const gchar *loops = actual != NULL ? strstr(actual, "loops=") : NULL;
		guint indent = 0;
		guint least = 0;

		if (upto == NULL || loops == NULL)
			continue;
		if (g_ascii_strtod(upto + 2, NULL) * g_ascii_strtod(loops + 6, NULL) < whole / 3.0)
			continue;
		while (lines[i][indent] == ' ')
			indent++;
		/* An empty place, else the shallowest of the three. */
		for (k = 0; k < 3; k++) {
			if (picked[k] == NULL) {
				least = k;
				break;
			}
			if (depth[k] < depth[least])
				least = k;
		}
		if (picked[least] == NULL || indent > depth[least]) {
			picked[least] = lines[i];
			depth[least] = indent;
		}
	}
	for (k = 0; k < 3; k++) {
		if (picked[k] != NULL) {
			g_autofree gchar *line = g_strstrip(g_strdup(picked[k]));

			g_string_append_printf(out, "%s%s", out->len > 0 ? " ;; " : "", line);
		}
	}
	return g_string_free(out, FALSE);
}

static Row *
add_row(
	Fixture     *fx,
	const gchar *name,
	const gchar *phase,
	gdouble      wall_ms,
	const gchar *plan,
	const gchar *needles
){
	Row *row = g_new0(Row, 1);

	row->name = g_strdup(name);
	row->phase = g_strdup(phase);
	row->wall_ms = wall_ms;
	row->exec_ms = plan != NULL ? plan_ms(plan) : -1;
	row->lines = plan == NULL ? g_strdup("") : needles != NULL ? plan_lines(plan, needles) : plan_heavy(plan);
	g_ptr_array_add(fx->rows, row);
	/* (Progress; the table at the end has the plans' lines.) */
	g_print("%-7s %-62s %10.1f ms\n", phase, name, row->exec_ms);
	return row;
}

/* A statement of the test's own text (a statement as it was before this
 * change, or one that no function of the daemon sends alone). */
static Row *
measure_sql(
	Fixture     *fx,
	const gchar *name,
	const gchar *phase,
	const gchar *statement,
	gboolean     undo,
	const gchar *needles
){
	g_autofree gchar *plan = plan_of(fx, statement, undo, NULL);

	return add_row(fx, name, phase, -1, plan, needles);
}

typedef void (*Call)(Fixture *fx);

/* A function of the daemon: called three times (the fastest counts), then
 * once more while the pool tells what it sends; the statement that contains
 * @part is explained as it was sent. */
static Row *
measure_call(
	Fixture     *fx,
	const gchar *name,
	const gchar *phase,
	Call         call,
	const gchar *part,
	const gchar *needles
){
	g_autoptr(GPtrArray) all = NULL;
	g_autofree gchar *literal = NULL;
	g_autofree gchar *plan = NULL;
	const Sent *sent;
	gint64 best = G_MAXINT64;
	guint i;

	for (i = 0; i < 3; i++) {
		gint64 began = g_get_monotonic_time();

		call(fx);
		best = MIN(best, g_get_monotonic_time() - began);
	}
	listen_begin();
	call(fx);
	all = listen_end();
	sent = sent_find(all, part);
	if (sent == NULL) {
		g_print("%s: no statement sent contains \"%s\"\n", name, part);
		g_assert_not_reached();
	}
	literal = sent_literal(sent);
	plan = plan_of(fx, literal, TRUE, NULL);
	return add_row(fx, name, phase, best / 1000.0, plan, needles);
}

/* ── The indexes of 0080_scale_indexes.sql ──────────────────────── */

static const gchar *const new_indexes[] = {
	"job_live_idx", "job_probe_ended_idx", "series_grab_candidate", "series_grab_search", "series_grab_for_episode", "movie_grab_candidate",
	"movie_grab_search", "import_file_library_file", "estimate_sample_file", "series_event_episode",
	"series_event_title", "movie_event_title", "import_title", "download_title", "library_identity_action_title",
	"import_ended"
};

/* Drops them, keeping each one's definition in @defs. */
static void
drop_new_indexes(
	Fixture  *fx,
	gchar   **defs
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(new_indexes); i++) {
		g_autofree gchar *ask = g_strdup_printf("SELECT indexdef FROM pg_indexes WHERE schemaname = 'public'"
			" AND indexname = '%s'", new_indexes[i]);

		defs[i] = axis_test_db_query(fx->t->db, ask);
		if (defs[i] == NULL) {
			g_print("the migration has no index %s\n", new_indexes[i]);
			g_assert_not_reached();
		}
		sql(fx, "DROP INDEX %s", new_indexes[i]);
	}
}

/* Creates them again, as the migration does, and tells what each cost. */
static void
create_new_indexes(
	Fixture  *fx,
	gchar   **defs
){
	gdouble total = 0;
	guint i;

	g_print("the indexes of 0080_scale_indexes.sql, each built on the database above:\n");
	for (i = 0; i < G_N_ELEMENTS(new_indexes); i++) {
		g_autofree gchar *size = NULL;
		g_autofree gchar *ask = g_strdup_printf("SELECT pg_size_pretty(pg_relation_size('%s'::regclass))",
			new_indexes[i]);
		gint64 began = g_get_monotonic_time();
		gdouble ms;

		axis_test_db_exec(fx->t->db, defs[i]);
		ms = (g_get_monotonic_time() - began) / 1000.0;
		total += ms;
		size = axis_test_db_query(fx->t->db, ask);
		g_print("  %-32s %8.0f ms  %s\n", new_indexes[i], ms, size);
		g_clear_pointer(&defs[i], g_free);
	}
	g_print("  %-32s %8.0f ms\n", "all of them", total);
	sql(fx, "ANALYZE");
}

/* ── What this change fixed ─────────────────────────────────────── */

static void
call_activity(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisJobsActivity activity;

	g_assert_true(axis_web_jobs_count_activity(fx->t->pool, fx->kinds, &activity, &error));
	g_assert_no_error(error);
	g_assert_cmpint(activity.running + activity.queued, >, 0);
	g_assert_cmpint(activity.running + activity.queued, <=, LIVE_JOBS + 10);
	g_assert_cmpint(activity.downloads, ==, 40);
}

static void
call_running(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint running = 0;

	g_assert_true(axis_web_jobs_count_running(fx->t->pool, fx->kinds, &running, &error));
	g_assert_no_error(error);
	g_assert_cmpint(running, ==, LIVE_JOBS / 6);
}

static void
call_active(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 running = 0;
	gint64 queued = 0;

	g_assert_true(axis_web_jobs_count_active(fx->t->pool, fx->kinds, "work", &running, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpint(running, ==, LIVE_JOBS / 6);
	g_assert_cmpint(queued, >, 0);
}

/* The page "Now": the work that waits or runs, and what ended this hour. */
static void
call_activity_page(Fixture *fx)
{
	AxisTestReply reply = axis_test_web_request(fx->session, fx->port, "GET", "/activity?part=list", fx->admin, NULL,
		NULL);

	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
}

static void
call_imports_ended(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) imports = NULL;

	g_assert_true(axis_import_list(fx->import, AXIS_IMPORT_LIST_RECENT, 50, &imports, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(imports->len, ==, 50);
}

/* The statements that count or list the jobs that wait or run, each as its
 * source has it (the file and line are in design.md): none may read the
 * whole table. */
typedef struct
{
	const gchar *name;
	const gchar *statement;
} JobStatement;

static const JobStatement job_statements[] = {
	{ "probe: the queue's top-up (axis-probe-job.c)",
	  "SELECT count(*) FROM job WHERE kind = '" AXIS_PROBE_FILE_KIND "' AND state IN ('queued', 'running')" },
	{ "metrics: axis_jobs (axis-web-metrics.c)",
	  "SELECT queue, state::text, count(*) FROM job WHERE state IN ('queued', 'running') GROUP BY 1, 2 ORDER BY 1, 2" },
	{ "youtube: the fetches known (axis-youtube-fetch.c)",
	  "SELECT id FROM job WHERE state IN ('queued', 'running') AND kind = '" AXIS_YOUTUBE_FETCH_KIND "'" },
	{ "estimate: a measurement to cancel (axis-estimate-sample.c)",
	  "SELECT id FROM job WHERE kind IN ('" AXIS_ESTIMATE_SAMPLE_KIND "', '" AXIS_WORKER_KIND_SAMPLE "')"
	  " AND state IN ('queued', 'running')" },
	{ "job queue: the kinds stored, at start-up (axis-job-queue.c)",
	  "SELECT kind, queue, count(*) FILTER (WHERE state = 'queued'), count(*) FILTER (WHERE state = 'running')"
	  " FROM job WHERE state IN ('queued', 'running') GROUP BY kind, queue ORDER BY kind, queue" }
};

/* The statements that ask for the last job of a kind, whatever its state,
 * and the metrics' counts by state: measured, each as its source has it. */
static const JobStatement other_statements[] = {
	{ "meta: the last refresh (axis-meta-refresh.c)",
	  "SELECT state, finished_at, last_error FROM job WHERE kind = '" AXIS_META_REFRESH_KIND "'"
	  " ORDER BY finished_at DESC NULLS FIRST, id DESC LIMIT 1" },
	{ "estimate: the last measurement (axis-estimate-sample.c)",
	  "SELECT j.id, j.state::text FROM job j WHERE j.kind IN ('" AXIS_ESTIMATE_SAMPLE_KIND "', '"
	  AXIS_WORKER_KIND_SAMPLE "') ORDER BY j.finished_at DESC NULLS FIRST, j.id DESC LIMIT 1" },
	{ "arr: the last run of a connection (axis-arr-apply.c)",
	  "SELECT state::text, last_error FROM job WHERE kind = '" AXIS_ARR_APPLY_KIND "' AND payload ->> 'source' = '1'"
	  " AND state NOT IN ('queued', 'running') ORDER BY id DESC LIMIT 1" },
	{ "metrics: axis_downloads (axis-web-metrics.c)",
	  "SELECT state, count(*) FROM download GROUP BY 1 ORDER BY 1" },
	{ "metrics: axis_imports (axis-web-metrics.c)",
	  "SELECT state, count(*) FROM import GROUP BY 1 ORDER BY 1" },
	{ "import: the other downloads of a volume (axis-import-folder.c)",
	  "SELECT rel_path FROM download d WHERE volume_id = 1 AND id <> 0 AND rel_path IS NOT NULL"
	  " AND (state IN ('sending', 'queued', 'downloading', 'paused', 'completed') OR (protocol = 'torrent'"
	  "  AND EXISTS (SELECT 1 FROM import j WHERE j.download_id = d.id AND j.client_removal = 'kept')))"
	  " UNION ALL SELECT source_path FROM import WHERE source_volume_id = 1 AND id <> 0"
	  " AND state IN ('waiting', 'running', 'blocked', 'partial')" }
};

static void
call_analysis_due(Fixture *fx)
{
	g_autoptr(AxisAnalysis) analysis = axis_analysis_new(fx->t->pool, NULL);
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_analysis_enqueue_due(analysis, NULL, &error));
	g_assert_no_error(error);
}

static void
measure_fixes(
	Fixture     *fx,
	const gchar *phase
){
	gboolean after = strcmp(phase, "after") == 0;
	g_autofree gchar *delete_candidates = g_strdup_printf("DELETE FROM series_candidate WHERE id = ANY (ARRAY(SELECT"
		" id FROM series_candidate WHERE position = 1 ORDER BY id LIMIT %u))", fx->n_delete);
	g_autofree gchar *delete_searches = g_strdup_printf("DELETE FROM series_search WHERE id = ANY (ARRAY(SELECT"
		" id FROM series_search WHERE outcome = 'grabbed' ORDER BY id LIMIT %u))", fx->n_delete / 10);
	g_autofree gchar *delete_movie_candidates = g_strdup_printf("DELETE FROM movie_candidate WHERE id = ANY (ARRAY("
		"SELECT id FROM movie_candidate WHERE position = 1 ORDER BY id LIMIT %u))", fx->n_delete);
	g_autofree gchar *delete_files = g_strdup_printf("DELETE FROM library_file WHERE id = ANY (ARRAY(SELECT file_id"
		" FROM series_file ORDER BY file_id LIMIT %u))", fx->n_delete);
	g_autofree gchar *delete_episodes = g_strdup_printf("DELETE FROM series_episode WHERE id = ANY (ARRAY(SELECT id"
		" FROM series_episode ORDER BY id LIMIT %u))", fx->n_delete / 10);
	g_autofree gchar *delete_titles = g_strdup_printf("DELETE FROM meta_title WHERE id = ANY (ARRAY(SELECT id"
		" FROM meta_title WHERE provider_id LIKE 'u%%' ORDER BY id LIMIT %u))", MIN(fx->n_delete / 10, REMOVED_TITLES));
	g_autofree gchar *name = NULL;
	Row *row;
	guint i;

	/* 1. The jobs that wait or run. */
	row = measure_call(fx, "jobs: the header's count (count_activity)", phase, call_activity,
		"count(*) FILTER", " job |on job");
	if (after) {
		g_assert_null(strstr(row->lines, "Seq Scan on job"));
		g_assert_cmpfloat(row->exec_ms, <, BOUND_LIVE_JOBS_MS);
	}
	row = measure_call(fx, "jobs: running now (count_running)", phase, call_running, "FROM job", " job |on job");
	if (after)
		g_assert_null(strstr(row->lines, "Seq Scan on job"));
	row = measure_call(fx, "jobs: waiting and running, of work (count_active)", phase, call_active, "FROM job",
		" job |on job");
	if (after)
		g_assert_null(strstr(row->lines, "Seq Scan on job"));
	row = measure_call(fx, "Now: the work that waits or runs (the page)", phase, call_activity_page,
		"j.state IN ('queued', 'running') AND NOT", " job j|on job j");
	if (after)
		g_assert_null(strstr(row->lines, "Seq Scan on job"));
	row = measure_call(fx, "Now: the work that ended this hour (the page)", phase, call_activity_page,
		"j.state IN ('succeeded', 'failed', 'cancelled') AND NOT", " job j|on job j");
	g_assert_null(strstr(row->lines, "Seq Scan on job"));
	for (i = 0; i < G_N_ELEMENTS(job_statements); i++) {
		row = measure_sql(fx, job_statements[i].name, phase, job_statements[i].statement, FALSE, " job |on job");
		if (after) {
			g_assert_null(strstr(row->lines, "Seq Scan on job"));
			g_assert_cmpfloat(row->exec_ms, <, BOUND_LIVE_JOBS_MS);
		}
	}

	row = measure_call(fx, "analysis: whether a refresh is due (enqueue_due)", phase, call_analysis_due, "WITH s AS",
		" job|on job");
	if (after) {
		g_assert_null(strstr(row->lines, "Seq Scan on job"));
		g_assert_null(strstr(row->lines, "job_finished_idx"));
		g_assert_cmpfloat(row->exec_ms, <, BOUND_LIVE_JOBS_MS);
	}
	for (i = 0; i < G_N_ELEMENTS(other_statements); i++)
		measure_sql(fx, other_statements[i].name, phase, other_statements[i].statement, FALSE,
			" job|on job|on download|on import|Sort Method");

	/* 2. Deleting stored search results, searches and episodes. */
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u stored results of series", fx->n_delete);
	row = measure_sql(fx, name, phase, delete_candidates, TRUE, "Trigger for constraint|Delete on");
	if (after)
		g_assert_cmpfloat(row->exec_ms, <, BOUND_DELETE_MS);
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u stored results of movies", fx->n_delete);
	measure_sql(fx, name, phase, delete_movie_candidates, TRUE, "Trigger for constraint|Delete on");
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u searches of series (20 results each)", fx->n_delete / 10);
	row = measure_sql(fx, name, phase, delete_searches, TRUE, "Trigger for constraint|Delete on");
	if (after)
		g_assert_cmpfloat(row->exec_ms, <, BOUND_DELETE_MS);
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u episodes", fx->n_delete / 10);
	measure_sql(fx, name, phase, delete_episodes, TRUE, "Trigger for constraint|Delete on");

	/* 3. Deleting library files and titles. */
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u library files", fx->n_delete);
	row = measure_sql(fx, name, phase, delete_files, TRUE, "Trigger for constraint|Delete on");
	if (after)
		g_assert_cmpfloat(row->exec_ms, <, BOUND_DELETE_MS);
	g_clear_pointer(&name, g_free);
	name = g_strdup_printf("delete %u titles nothing manages", MIN(fx->n_delete / 10, REMOVED_TITLES));
	row = measure_sql(fx, name, phase, delete_titles, TRUE, "Trigger for constraint|Delete on");
	if (after)
		g_assert_cmpfloat(row->exec_ms, <, BOUND_DELETE_MS);
	measure_sql(fx, "movies: the history of a title removed (axis-movies-store.c)", phase,
		"SELECT id, kind, text FROM movie_event WHERE title_id = (SELECT max(id) FROM meta_title WHERE provider_id"
		" LIKE 'u%') AND movie_id IS NULL ORDER BY id DESC LIMIT 50", FALSE, "movie_event");

	/* 4. The imports that ended. */
	row = measure_call(fx, "imports: the 50 that ended last (axis_import_list)", phase, call_imports_ended,
		"ORDER BY i.finished_at DESC", "import i|on import");
	if (after)
		g_assert_cmpfloat(row->exec_ms, <, BOUND_LIST_MS);
}

static void
print_table(Fixture *fx)
{
	guint i;

	g_print("the table: statement | when | ms in the database | ms the call | the plan's lines\n");
	for (i = 0; i < fx->rows->len; i++) {
		const Row *row = g_ptr_array_index(fx->rows, i);
		g_autofree gchar *wall = row->wall_ms >= 0 ? g_strdup_printf("%.1f", row->wall_ms) : g_strdup("-");

		g_print("%s | %s | %.1f | %s | %s\n", row->name, row->phase, row->exec_ms, wall, row->lines);
	}
}

static void
test_fixes(Fixture *fx, gconstpointer data)
{
	gchar *defs[G_N_ELEMENTS(new_indexes)];

	drop_new_indexes(fx, defs);
	make_database(fx);
	measure_fixes(fx, "before");
	create_new_indexes(fx, defs);
	measure_fixes(fx, "after");
	print_table(fx);
}

/* ── What is only measured ──────────────────────────────────────── */

static const gchar *jobs_cursor;	/* of call_jobs_list() */
static const gchar *jobs_state;

static void
call_jobs_list(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *next = NULL;
	g_auto(GStrv) queues = NULL;
	AxisJobFilter filter;

	memset(&filter, 0, sizeof(filter));
	filter.cursor = jobs_cursor;
	filter.state = jobs_state;
	if (!axis_web_jobs_list(fx->t->pool, fx->kinds, &filter, &rows, &next, &queues, &error))
		g_print("the Jobs list failed: %s\n", error->message);
}

static void
call_movies_page(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisMoviesListQuery query;
	AxisMoviesListPage page;

	memset(&query, 0, sizeof(query));
	memset(&page, 0, sizeof(page));
	query.limit = 60;
	g_assert_true(axis_movies_list_page(fx->movies, &query, &page, &error));
	g_assert_no_error(error);
	axis_movies_list_page_clear(&page);
}

static void
call_movies_attention(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	guint64 reasons[AXIS_MOVIES_ATTENTION_KINDS];
	gint64 highest[AXIS_MOVIES_ATTENTION_KINDS];

	if (!axis_movies_count_attention(fx->movies, 60000, reasons, highest, &error))
		g_print("the movies' attention count failed: %s\n", error->message);
}

static glong
peak_rss_kb(void)
{
	struct rusage usage;

	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_maxrss;
}

/* Explains every statement of @all (in a transaction rolled back) and adds
 * a row for each that took @min_ms and more in the database; tells the
 * longest, which must stay under the session's statement timeout. */
static void
report_sent(
	Fixture     *fx,
	const gchar *name,
	GPtrArray   *all,
	gdouble      min_ms
){
	gdouble longest = 0;
	gdouble sum = 0;
	guint i;

	for (i = 0; i < all->len && i < 60; i++) {
		const Sent *sent = g_ptr_array_index(all, i);
		g_autofree gchar *literal = NULL;
		g_autofree gchar *plan = NULL;
		g_autofree gchar *label = NULL;
		g_autofree gchar *start = NULL;
		gdouble ms;

		if (g_ascii_strncasecmp(sent->sql, "SELECT", 6) != 0 && g_ascii_strncasecmp(sent->sql, "UPDATE", 6) != 0 &&
		    g_ascii_strncasecmp(sent->sql, "DELETE", 6) != 0 && g_ascii_strncasecmp(sent->sql, "WITH", 4) != 0)
			continue;
		literal = sent_literal(sent);
		plan = plan_of(fx, literal, TRUE, NULL);
		ms = plan_ms(plan);
		sum += MAX(ms, 0);
		longest = MAX(longest, ms);
		if (ms < min_ms)
			continue;
		start = g_strndup(sent->sql, 70);
		label = g_strdup_printf("%s: %s...", name, start);
		add_row(fx, label, "now", -1, plan, NULL);
	}
	g_print("%s: %u statements sent, %.0f ms in the database in all, the longest %.0f ms (the session's statement "
		"timeout is %d ms)\n", name, all->len, sum, longest, AXIS_DB_STATEMENT_TIMEOUT_MS);
}

/* A page fetched three times: the fastest. */
static void
report_page(
	Fixture     *fx,
	const gchar *path
){
	gint64 best = G_MAXINT64;
	guint status = 0;
	gsize bytes = 0;
	guint i;

	for (i = 0; i < 3; i++) {
		gint64 began = g_get_monotonic_time();
		AxisTestReply reply = axis_test_web_request(fx->session, fx->port, "GET", path, fx->admin, NULL, NULL);

		best = MIN(best, g_get_monotonic_time() - began);
		status = reply.status;
		bytes = reply.length;
		axis_test_reply_clear(&reply);
	}
	g_print("page %-28s %8.0f ms (status %u, %" G_GSIZE_FORMAT " bytes)\n", path, best / 1000.0, status, bytes);
}

/* Sets jit in every session of the pool (all of them held at once, so that
 * each is reached). */
static void
pool_set_jit(
	Fixture     *fx,
	const gchar *value
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *set = g_strdup_printf("SET jit = %s", value);
	guint n = axis_db_pool_get_size(fx->t->pool);
	AxisDbConn **held = g_new0(AxisDbConn *, n);
	guint i;

	for (i = 0; i < n; i++) {
		held[i] = axis_db_pool_acquire(fx->t->pool, &error);
		g_assert_no_error(error);
		g_assert_true(axis_db_conn_exec(held[i], set, 0, NULL, NULL, &error));
	}
	for (i = 0; i < n; i++)
		axis_db_pool_release(fx->t->pool, held[i]);
	g_free(held);
}

/* Storing one search's thousand results as the search does: one INSERT a
 * row in one transaction (src/acquire/axis-acquire-search.c), rolled back;
 * and, to compare, the same rows in one statement. */
static void
report_store_results(Fixture *fx)
{
	static const gchar one[] = "INSERT INTO series_candidate (search_id, position, release_id, source_id,"
		" name, protocol, size_bytes, published_at, seeders, indexer, indexer_id, info_hash, name_key, quality, match,"
		" match_text, accepted, safety, blocklisted, rank, score, reasons) VALUES ($1::bigint, $2::integer,"
		" $3::bigint, $4::bigint, $5, $6, $7::bigint, to_timestamp($8::bigint / 1000000.0), $9::integer, $10,"
		" $11::bigint, $12, $13, $14, $15, $16, $17::boolean, $18::boolean, $19::boolean, $20::integer,"
		" $21::integer, $22::jsonb) RETURNING id";
	g_autoptr(GError) error = NULL;
	g_autofree gchar *search = axis_test_db_query(fx->t->db, "SELECT max(id)::text FROM series_search"
		" WHERE outcome = 'nothing_found'");
	AxisDbConn *conn = axis_db_pool_acquire(fx->t->pool, &error);
	gint64 began;
	guint i;

	g_assert_nonnull(conn);
	g_assert_true(axis_db_conn_begin(conn, &error));
	began = g_get_monotonic_time();
	for (i = 0; i < 1000; i++) {
		g_autoptr(AxisDbResult) inserted = NULL;
		g_autofree gchar *position = g_strdup_printf("%u", i + 100);
		g_autofree gchar *name = g_strdup_printf("Generated.Series.S01E01.%u.1080p.WEB-DL-GRP", i);
		const gchar *params[22];

		params[0] = search;
		params[1] = position;
		params[2] = position;
		params[3] = "1";
		params[4] = name;
		params[5] = "usenet";
		params[6] = "1000000000";
		params[7] = "1794657600000000";
		params[8] = NULL;
		params[9] = "Generated Indexer";
		params[10] = "1";
		params[11] = NULL;
		params[12] = name;
		params[13] = "webdl_1080p";
		params[14] = "match";
		params[15] = "As the test made it.";
		params[16] = "false";
		params[17] = "false";
		params[18] = "false";
		params[19] = position;
		params[20] = "0";
		params[21] = "[{\"k\": \"profile\", \"c\": \"quality_not_wanted\", \"t\": \"As the test made it.\"}]";
		g_assert_true(axis_db_conn_exec(conn, one, 22, params, &inserted, &error));
	}
	g_print("storing 1,000 results of one search, one INSERT a row: %.0f ms\n",
		(g_get_monotonic_time() - began) / 1000.0);
	axis_db_conn_rollback(conn, NULL);
	g_assert_true(axis_db_conn_begin(conn, &error));
	began = g_get_monotonic_time();
	{
		const gchar *params[1];

		params[0] = search;
		g_assert_true(axis_db_conn_exec(conn, "INSERT INTO series_candidate (search_id, position, release_id,"
			" source_id, name, protocol, name_key, quality, match, match_text, accepted, safety, reasons)"
			" SELECT $1::bigint, p + 100, p, 1, 'Generated.Series.S01E01.' || p || '.1080p.WEB-DL-GRP', 'usenet',"
			" 'generated-' || p, 'webdl_1080p', 'match', 'As the test made it.', false, false,"
			" '[{\"k\": \"profile\", \"c\": \"quality_not_wanted\", \"t\": \"As the test made it.\"}]'::jsonb"
			" FROM generate_series(1, 1000) p", 1, params, NULL, &error));
		g_assert_no_error(error);
	}
	g_print("the same 1,000 rows in one statement: %.0f ms\n", (g_get_monotonic_time() - began) / 1000.0);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(fx->t->pool, conn);
}

static void
test_report(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autofree gchar *cursor = NULL;
	g_autofree gchar *classes = axis_tv_store_statement("classes");
	const Sent *prune;
	gint64 began;
	glong rss;

	/* Nothing is held here, only told: with the other tests a fiftieth of
	 * the scale shows that every part still runs. */
	if (g_getenv("AXIS_DB_SCALE") == NULL)
		fx->percent = 2;
	make_database(fx);

	/* The Jobs list. */
	jobs_cursor = NULL;
	jobs_state = NULL;
	measure_call(fx, "Jobs: the first page", "now", call_jobs_list, "ORDER BY (state IN", " job |on job|Sort Method");
	measure_call(fx, "Jobs: the queues' names", "now", call_jobs_list, "SELECT DISTINCT queue",
		" job |on job|Sort Method");
	cursor = g_strdup_printf("f%" G_GINT64_FORMAT, qi(fx, "SELECT max(id) / 2 FROM job"));
	jobs_cursor = cursor;
	measure_call(fx, "Jobs: a later page (half way)", "now", call_jobs_list, "ORDER BY (state IN",
		" job |on job|Sort Method");
	jobs_cursor = NULL;
	jobs_state = "failed";
	measure_call(fx, "Jobs: the failed ones, first page", "now", call_jobs_list, "ORDER BY (state IN",
		" job |on job|Sort Method");
	jobs_state = NULL;

	/* The TV scheduler, as its job runs it every fifteen minutes. */
	{
		AxisTvScheduleReport report;
		gboolean ok;

		memset(&report, 0, sizeof(report));
		rss = peak_rss_kb();
		listen_begin();
		began = g_get_monotonic_time();
		ok = axis_tv_run_schedule(fx->tv, &report, &error);
		g_print("the TV scheduler: %s in %.1f s; %u episodes considered, %u due, %u searches queued; the process's "
			"peak memory went from %ld MiB to %ld MiB\n", ok ? "ran" : error->message,
			(g_get_monotonic_time() - began) / 1000000.0, report.considered, report.due, report.queued, rss / 1024,
			peak_rss_kb() / 1024);
		g_clear_error(&error);
		all = listen_end();
		report_sent(fx, "TV scheduler", all, 20.0);
		g_clear_pointer(&all, g_ptr_array_unref);
		g_print("the series' metadata is followed 5 series a run, a run every %d s: %" G_GINT64_FORMAT " series take %.1f "
			"days to go round\n", AXIS_TV_SCHEDULE_INTERVAL_S, qi(fx, "SELECT count(*) FROM series"),
			qi(fx, "SELECT count(*) FROM series") / 5.0 * AXIS_TV_SCHEDULE_INTERVAL_S / 86400.0);
	}

	/* The movies. */
	{
		AxisMovieScheduleReport report;
		gboolean ok;

		memset(&report, 0, sizeof(report));
		rss = peak_rss_kb();
		listen_begin();
		began = g_get_monotonic_time();
		ok = axis_movies_run_schedule(fx->movies, &report, &error);
		g_print("the movies' scheduler: %s in %.1f s; %u movies considered, %u due; peak memory from %ld MiB to %ld "
			"MiB\n", ok ? "ran" : error->message, (g_get_monotonic_time() - began) / 1000000.0, report.considered,
			report.due, rss / 1024, peak_rss_kb() / 1024);
		g_clear_error(&error);
		all = listen_end();
		report_sent(fx, "movies' scheduler", all, 20.0);
		g_clear_pointer(&all, g_ptr_array_unref);
	}
	listen_begin();
	began = g_get_monotonic_time();
	call_movies_page(fx);
	g_print("the Movies list's first page: %.0f ms the call\n", (g_get_monotonic_time() - began) / 1000.0);
	all = listen_end();
	report_sent(fx, "Movies list", all, 5.0);
	g_clear_pointer(&all, g_ptr_array_unref);
	listen_begin();
	began = g_get_monotonic_time();
	call_movies_attention(fx);
	g_print("the movies' attention count: %.0f ms the call\n", (g_get_monotonic_time() - began) / 1000.0);
	all = listen_end();
	report_sent(fx, "movies' attention count", all, 0.0);
	g_clear_pointer(&all, g_ptr_array_unref);

	/* The follow-up's prune of old searches, as it is sent every minute:
	 * with nothing old enough to go, and with everything old enough (what
	 * the first run after a long stop finds). */
	{
		AxisAcquireFollowReport report;

		memset(&report, 0, sizeof(report));
		listen_begin();
		began = g_get_monotonic_time();
		if (!axis_tv_run_follow(fx->tv, &report, &error)) {
			g_print("the TV follow-up failed: %s\n", error->message);
			g_clear_error(&error);
		}
		g_print("the TV follow-up: %.0f ms the call\n", (g_get_monotonic_time() - began) / 1000.0);
		all = listen_end();
		report_sent(fx, "TV follow-up", all, 5.0);
		prune = sent_find(all, "DELETE FROM series_search");
		if (prune != NULL) {
			g_autofree gchar *literal = sent_literal(prune);
			g_autofree gchar *steady = plan_of(fx, literal, TRUE, NULL);
			g_autofree gchar *plan = plan_of(fx, literal, TRUE, "UPDATE series_search SET started_at = now() -"
				" interval '30 days', finished_at = now() - interval '30 days'");

			add_row(fx, "prune: no search is old enough to go", "now", -1, steady, NULL);
			add_row(fx, "prune: every search is a month old (all but 5 a series go)", "now", -1, plan,
				"Delete on|Trigger for constraint|Seq Scan|SubPlan");
			sql(fx, "VACUUM ANALYZE series_search");
		} else {
			g_print("the TV follow-up sent no prune of series_search\n");
		}
		g_clear_pointer(&all, g_ptr_array_unref);
	}
	report_store_results(fx);

	/* The pages of speed-up-lists-and-dashboard, at this scale. */
	report_page(fx, "/");
	report_page(fx, "/series");
	report_page(fx, "/series/wanted");
	report_page(fx, "/series/calendar");
	report_page(fx, "/films");
	report_page(fx, "/jobs");

	/* PostgreSQL's JIT: every pooled session has it off (axis-db-pool.c).
	 * What that changes, measured with the pool's sessions as they are and
	 * then with each of them set as they were before (jit on, the server's
	 * default): the lists' counting statement, the lists themselves, the
	 * movies' scheduler and the Analysis refresh. */
	{
		g_autoptr(AxisAnalysis) analysis = axis_analysis_new(fx->t->pool, NULL);
		g_auto(GStrv) one = g_strsplit(classes, "$1", -1);
		g_autofree gchar *with1 = g_strjoinv("NULL", one);
		g_auto(GStrv) two = g_strsplit(with1, "$2", -1);
		g_autofree gchar *with2 = g_strjoinv("'2026-11-14'", two);
		g_auto(GStrv) three = g_strsplit(with2, "$3", -1);
		g_autofree gchar *with3 = g_strjoinv("'none'", three);
		gint64 snapshot = 0;
		guint pass;

		measure_sql(fx, "TV: the episodes counted by class, JIT off", "now", with3, FALSE, "JIT|Timing:|Execution Time");
		axis_test_db_exec(fx->t->db, "SET jit = on");
		measure_sql(fx, "TV: the episodes counted by class, JIT on", "now", with3, FALSE, "JIT|Timing:|Execution Time");
		axis_test_db_exec(fx->t->db, "SET jit = off");
		/* (Once before, so that neither pass reads a cold cache.) */
		if (!axis_analysis_refresh(analysis, "direct", 0, NULL, &snapshot, &error)) {
			g_print("the Analysis refresh failed: %s\n", error->message);
			g_clear_error(&error);
		}
		for (pass = 0; pass < 2; pass++) {
			const gchar *jit = pass == 0 ? "off" : "on";

			pool_set_jit(fx, jit);
			g_print("with jit %s in every pooled session:\n", jit);
			began = g_get_monotonic_time();
			if (!axis_analysis_refresh(analysis, "direct", 0, NULL, &snapshot, &error)) {
				g_print("the Analysis refresh failed: %s\n", error->message);
				g_clear_error(&error);
			}
			g_print("the Analysis refresh of %" G_GINT64_FORMAT " files: %.0f ms\n",
				qi(fx, "SELECT count(*) FROM library_file"), (g_get_monotonic_time() - began) / 1000.0);
			began = g_get_monotonic_time();
			if (!axis_movies_run_schedule(fx->movies, NULL, &error)) {
				g_print("the movies' scheduler failed: %s\n", error->message);
				g_clear_error(&error);
			}
			g_print("the movies' scheduler: %.0f ms\n", (g_get_monotonic_time() - began) / 1000.0);
			report_page(fx, "/series");
			report_page(fx, "/series/wanted");
			report_page(fx, "/films");
			report_page(fx, "/jobs");
		}
		pool_set_jit(fx, "off");
	}
	print_table(fx);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	/* Progress is visible while it runs, also when the output is a file. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	AXIS_TEST_ADD_DB_CASE("/db/scale/fixes", Fixture, NULL, test_fixes);
	AXIS_TEST_ADD_DB_CASE("/db/scale/report", Fixture, NULL, test_report);

	return g_test_run();
}
