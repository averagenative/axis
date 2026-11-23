/*
 * test-web-tv-scale.c - the lists and the dashboard where the library is
 * large (speed-up-lists-and-dashboard)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two things are held here.
 *
 * What the lists say is what every episode's facts say
 * (/web/tv-scale/counted-as-derived): a few hundred generated series whose
 * episodes are in every state a series' page can show (wanted, searched for,
 * downloading, to import, in the library, to upgrade, needing attention for
 * each of its reasons, held for a person, not aired, not monitored, gone,
 * specials, anime with absolute numbers, kept by hand, brought over), counted
 * twice: by the database, as the lists, the dashboard and Wanted read them
 * (axis_tv_list_counted()), and from the facts of every episode, as they
 * were read before (axis_tv_list()).  The two must agree in every count,
 * state, sentence and episode that needs attention.
 *
 * And the pages stay quick and read a page's worth
 * (/web/tv-scale/pages): a library of 1,500 series and 36,000 episodes with
 * files, searches, releases found, grabs, searches in flight and a job table
 * of 160,000 finished jobs, as a library of that size has after some days.
 * Every page is timed and bounded (generously: the tests run under the
 * sanitizers), and none may read more episodes than a page shows; the
 * dashboard, once started, answers from memory with no statement at all.
 * The times are told with g_test_message().
 *
 * Every title here is made up ("Generated Series 12").
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

/* The targets of the change, and how far a run under the sanitizers may be
 * from them before the test fails. */
#define TARGET_DASHBOARD_MS 100
#define TARGET_LIST_MS      300
#define TARGET_OTHER_MS     500
#define SLACK               3

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
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
	AxisMovies           *movies;
	AxisTv               *tv;
	AxisModuleRegistry   *registry;
	AxisWebHub           *hub;
	AxisWebJobs          *jobs;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
} Fixture;

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
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
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
	fx->session = soup_session_new_with_options("timeout", 120, "idle-timeout", 120, NULL);

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
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
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
	axis_module_registry_free(fx->registry);
	axis_tv_unref(fx->tv);
	axis_movies_unref(fx->movies);
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
	g_free(fx->disk);
	g_free(fx->admin);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_page(
	Fixture     *fx,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, fx->admin, NULL, NULL);
}

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

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (strstr(haystack, needle) == NULL) {
		g_test_message("missing: %s", needle);
		g_assert_not_reached();
	}
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (strstr(haystack, needle) != NULL) {
		g_test_message("unexpected: %s", needle);
		g_assert_not_reached();
	}
}

/* The number in the tab called @label of @html. */
static guint
tab_count(
	const gchar *html,
	const gchar *label
){
	g_autofree gchar *needle = g_strdup_printf(">%s</span><span class=\"tab-count\">", label);
	const gchar *at = strstr(html, needle);

	if (at == NULL) {
		g_test_message("no tab called %s", label);
		g_assert_not_reached();
	}
	return (guint)g_ascii_strtoull(at + strlen(needle), NULL, 10);
}

/* The library's new files for the episodes @where chooses, each linked to
 * its episode: one statement. */
static void
add_files(
	Fixture     *fx,
	const gchar *where,
	const gchar *quality,
	const gchar *extra_columns,
	const gchar *extra_values,
	const gchar *tag
){
	sql(fx, "WITH lf AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'generated/%s/' || e.id || '.mkv', 'video', 700000000 + e.id, 1, 0, 1, 0,"
		" (SELECT coalesce(max(inode), 5000000) FROM library_file) + row_number() OVER (), 1, false, false, false, 1, 1, 1"
		" FROM series_episode e WHERE %s RETURNING id, rel_path),"
		" m AS (SELECT lf.id AS file_id, split_part(split_part(lf.rel_path, '/', 3), '.', 1)::bigint AS episode_id FROM lf),"
		" sf AS (INSERT INTO series_file (file_id, series_id, quality, release_name%s) SELECT m.file_id, e.series_id,"
		" %s, 'Generated.Series.S01E01.1080p.WEB-DL-GRP'%s FROM m JOIN series_episode e ON e.id = m.episode_id"
		" RETURNING file_id)"
		" INSERT INTO series_file_episode (file_id, episode_id) SELECT m.file_id, m.episode_id FROM m", fx->root_id,
		fx->volume_id, tag, where, extra_columns, quality, extra_values);
}

/* A download at the client in @state and its open grab of the episodes
 * @where chooses, one grab an episode (completed when @completed). */
static void
add_grabs(
	Fixture     *fx,
	const gchar *where,
	const gchar *state,
	gboolean     completed,
	gboolean     closed_with_removal
){
	sql(fx, "WITH d AS (INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) SELECT %" G_GINT64_FORMAT ", 'SABnzbd',"
		" 'ref-%s-' || e.id, 'usenet', 'file', 'Generated.Series.E' || e.id || '.1080p.WEB-DL-GRP', s.title_id,"
		" 'Generated Series', 'tv', '%s', 1000, 400, '/data/downloads/generated-' || e.id, %" G_GINT64_FORMAT ","
		" 'downloads/generated-' || e.id, now(), %s FROM series_episode e JOIN series s ON s.id = e.series_id WHERE %s"
		" RETURNING id, client_ref),"
		" g AS (INSERT INTO series_grab (series_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at, scope, episode_id%s) SELECT e.series_id, 'Generated.Series.E' || e.id || '.1080p.WEB-DL-GRP',"
		" 'usenet', 'generated-' || e.id || '-%s', d.id, now(), %s, 'episode', e.id%s FROM d JOIN series_episode e"
		" ON e.id = split_part(d.client_ref, '-', 3)::bigint RETURNING id, episode_id)"
		" INSERT INTO series_grab_episode (grab_id, episode_id, open) SELECT g.id, g.episode_id, %s FROM g",
		fx->client_id, state, state, fx->volume_id, completed ? "now()" : "NULL", where,
		closed_with_removal ? ", closed_at, close_reason, removal" : "", state,
		completed ? "now()" : "NULL", closed_with_removal ? ", now(), 'given_up', 'pending'" : "",
		closed_with_removal ? "false" : "true");
}

/* A finished search of every episode @where chooses. */
static void
add_searches(
	Fixture     *fx,
	const gchar *where,
	const gchar *outcome,
	const gchar *cause,
	guint        person_only
){
	sql(fx, "INSERT INTO series_search (series_id, cause, started_at, finished_at, outcome, outcome_text, scope,"
		" episode_id, n_person_only) SELECT e.series_id, '%s', now() - interval '2 hours', now() - interval '2 hours',"
		" '%s', 'As the test made it: %s.', 'episode', e.id, %u FROM series_episode e WHERE %s", cause, outcome,
		outcome, person_only, where);
}

/* ── The same counts as from every episode's facts ──────────────── */

#define VARIED_SERIES 300

/*
 * 300 series of three seasons (the specials among them) of ten episodes,
 * each episode in a state its number and its series' decide, so that every
 * state and every reason meets every kind of series: one without a
 * destination, one marked as anime with absolute numbers, one brought over
 * and held, one of another profile.
 */
static void
make_varied(Fixture *fx)
{
	/* e.id % 40 decides an episode's facts; "k" is that. */
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2000 + i %% 25, 'ended', 'fixture', 'v' || i"
		" FROM generate_series(1, %d) i RETURNING id, provider_id),"
		" s AS (INSERT INTO series (title_id, quality_profile_id, root_id, kind, search_hold) SELECT t.id,"
		"  (SELECT id FROM quality_profile ORDER BY (slug = 'balanced') DESC, id"
		"   OFFSET (CASE WHEN substr(t.provider_id, 2)::int %% 5 = 0 THEN 1 ELSE 0 END) LIMIT 1),"
		"  CASE WHEN substr(t.provider_id, 2)::int %% 11 = 0 THEN NULL ELSE %" G_GINT64_FORMAT " END,"
		"  CASE WHEN substr(t.provider_id, 2)::int %% 7 = 0 THEN 'anime' ELSE 'standard' END,"
		"  CASE WHEN substr(t.provider_id, 2)::int %% 13 = 0 THEN 'brought' END FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, g, g > 0 FROM s,"
		"  generate_series(0, 2) g RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, absolute_number, title, air_date, monitored)"
		" SELECT s.id, g, e, CASE WHEN g > 0 THEN (g - 1) * 10 + e END, 'Part ' || e,"
		"  CASE WHEN g = 2 AND e = 10 THEN NULL WHEN g = 2 AND e = 9 THEN DATE '2031-05-06'"
		"   WHEN g = 2 AND e = 8 THEN DATE '2026-11-14' WHEN g = 2 AND e = 7 THEN DATE '2026-11-15'"
		"   WHEN g = 0 AND e = 1 THEN DATE '1969-07-01' ELSE DATE '2019-03-04' + e END,"
		"  NOT (g = 0 AND e > 5) AND NOT (g = 1 AND e = 10) FROM s, generate_series(0, 2) g, generate_series(1, 10) e",
		VARIED_SERIES, fx->root_id);
	sql(fx, "UPDATE series_episode SET gone_at = now() WHERE id %% 40 = 39");
	sql(fx, "UPDATE series_episode SET search_hold = 'file_unseen' WHERE id %% 40 = 38");

	/* Files: at the cutoff, below it, of no known source with and without a
	 * resolution, kept by hand, brought over, and two files of one episode. */
	add_files(fx, "e.id % 40 IN (0, 1, 2, 3, 4, 5, 20, 21, 30)", "'webdl_1080p'", "", "", "a");
	add_files(fx, "e.id % 40 IN (6, 7, 22)", "'hdtv_720p'", "", "", "b");
	add_files(fx, "e.id % 40 = 8", "NULL", ", resolution", ", '720p'", "c");
	add_files(fx, "e.id % 40 = 9", "NULL", "", "", "d");
	add_files(fx, "e.id % 40 = 10", "'hdtv_720p'", ", kept, hand_how, hand_by, hand_at", ", true, 'assigned', 'root', now()",
		"e");
	add_files(fx, "e.id % 40 = 11", "'hdtv_720p'", ", kept, kept_how, hand_how, hand_at",
		", true, 'brought', 'brought', now()", "f");
	add_files(fx, "e.id % 40 IN (6, 30)", "'bluray_1080p'", "", "", "g");

	/* Grabs: downloading, being handed over, arrived, arrived and blocked,
	 * partly imported with and without a file, and given up but still at the
	 * client. */
	add_grabs(fx, "e.id % 40 = 12 AND e.series_id % 12 <> 0", "downloading", FALSE, FALSE);
	add_grabs(fx, "e.id % 40 = 13 AND e.series_id % 12 <> 0", "completed", TRUE, FALSE);
	add_grabs(fx, "e.id % 40 IN (14, 20) AND e.series_id % 3 <> 0", "completed", TRUE, FALSE);
	add_grabs(fx, "e.id % 40 IN (15, 21) AND e.series_id % 3 <> 0", "completed", TRUE, FALSE);
	add_grabs(fx, "e.id % 40 IN (16, 22) AND e.series_id % 3 <> 0", "paused", FALSE, TRUE);
	/* One series in three has nothing that needs attention; one in six of
	 * them nothing wanted either: every episode not in a grab has a file. */
	add_files(fx, "e.series_id % 6 = 0 AND NOT EXISTS (SELECT 1 FROM series_file_episode x WHERE x.episode_id = e.id)"
		" AND NOT EXISTS (SELECT 1 FROM series_grab_episode y WHERE y.episode_id = e.id AND y.open)", "'webdl_1080p'",
		"", "", "h");
	sql(fx, "UPDATE series_grab SET download_id = NULL WHERE episode_id %% 80 = 12");
	sql(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id, reason_text,"
		" finished_at) SELECT 'tv', g.series_id, 'Generated Series', 'downloads/generated-' || g.id,"
		" CASE WHEN g.episode_id %% 40 IN (14, 20) THEN 'blocked' ELSE 'partial' END, g.download_id,"
		" 'As the test made it.', CASE WHEN g.episode_id %% 40 IN (15, 21) THEN now() END FROM series_grab g"
		" WHERE g.episode_id %% 40 IN (14, 20, 15, 21)");

	/* Searches: found nothing; the profile rejected what was found, once
	 * with a release only a person can grab; the other reasons; and later
	 * rows that are not the last search (a list, one not finished). */
	add_searches(fx, "e.id % 40 IN (23, 24, 31, 32, 33, 34, 35, 36)", "nothing_found", "scheduled", 0);
	add_searches(fx, "e.id % 40 = 24 AND e.series_id % 3 <> 0", "none_acceptable", "scheduled", 0);
	add_searches(fx, "e.id % 40 = 25 AND e.series_id % 3 <> 0", "none_acceptable", "scheduled", 2);
	add_searches(fx, "e.id % 40 = 26 AND e.series_id % 3 <> 0", "exhausted", "retry", 0);
	add_searches(fx, "e.id % 40 = 27 AND e.series_id % 3 <> 0", "no_indexer", "scheduled", 0);
	add_searches(fx, "e.id % 40 = 28 AND e.series_id % 3 <> 0", "client_error", "manual", 0);
	add_searches(fx, "e.id % 40 = 29 AND e.series_id % 3 <> 0", "failed", "scheduled", 0);
	add_searches(fx, "e.id % 40 = 31", "none_acceptable", "list", 3);
	add_searches(fx, "e.id % 40 IN (0, 6)", "grabbed", "scheduled", 0);
	sql(fx, "INSERT INTO series_search (series_id, cause, scope, episode_id) SELECT e.series_id, 'scheduled', 'episode',"
		" e.id FROM series_episode e WHERE e.id %% 40 = 32");
	sql(fx, "UPDATE series_episode SET last_search_at = now() - interval '2 hours', next_search_at = now() +"
		" interval '4 hours' WHERE id %% 40 IN (23, 24)");

	/* Searches in flight: of an episode (queued, and running), of a season,
	 * of a whole series; and keys that hold nothing (a manual search's list,
	 * a finished job). */
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) SELECT 'tv.search', 'searches', 'queued', 2,"
		" 'tv.search:' || e.series_id || ':episode:' || e.id FROM series_episode e WHERE e.id %% 40 IN (33, 1, 7)"
		" AND e.series_id %% 24 <> 0");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, lease_token, lease_expires_at,"
		" started_at) SELECT 'tv.search', 'searches', 'running', 2, 1, 'tv.search:' || e.series_id || ':episode:' || e.id,"
		" gen_random_uuid(), now() + interval '5 minutes', now() FROM series_episode e WHERE e.id %% 40 = 34");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) SELECT 'tv.search', 'searches', 'queued', 2,"
		" 'tv.search:' || s.id || ':season:1' FROM series s WHERE s.id %% 17 = 0");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, lease_token, lease_expires_at,"
		" started_at) SELECT 'tv.search', 'searches', 'running', 2, 1, 'tv.search:' || s.id || ':series',"
		" gen_random_uuid(), now() + interval '5 minutes', now() FROM series s WHERE s.id %% 19 = 0");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) SELECT 'tv.search', 'searches', 'queued', 2,"
		" 'tv.search:' || e.series_id || ':list:episode:' || e.id FROM series_episode e WHERE e.id %% 40 = 35");
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, finished_at) SELECT 'tv.search',"
		" 'searches', 'succeeded', 2, 1, 'tv.search:' || e.series_id || ':episode:' || e.id, now()"
		" FROM series_episode e WHERE e.id %% 40 = 36");
}

#define SETTLED_SERIES 5

/*
 * Five more series with nothing under way, each of the specials (three
 * episodes, never monitored, no file) and two seasons of four aired
 * episodes: every episode of them is in the library or not monitored, the
 * two states that are no worse than each other, so the series' own state is
 * decided by what of it is monitored and not by the episode counted first
 * (series-monitoring-control).
 *
 *   1  both seasons monitored and in the library, new seasons monitored:
 *      the series that read "Not monitored"
 *   2  nothing monitored, new seasons neither, its files in the library
 *   3  season 1 left out, season 2 monitored, all of both in the library
 *   4  nothing monitored and no file, new seasons monitored
 *   5  nothing monitored and no file, new seasons neither
 */
static void
make_settled(Fixture *fx)
{
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Settled Series ' || i, 2010 + i, 'ended', 'fixture', 'w' || i"
		" FROM generate_series(1, %d) i RETURNING id, provider_id),"
		" s AS (INSERT INTO series (title_id, quality_profile_id, root_id, monitor_new) SELECT t.id,"
		"  (SELECT id FROM quality_profile ORDER BY (slug = 'balanced') DESC, id LIMIT 1), %" G_GINT64_FORMAT ","
		"  substr(t.provider_id, 2)::int IN (1, 4) FROM t RETURNING id, title_id),"
		" k AS (SELECT s.id, substr(t.provider_id, 2)::int AS i FROM s JOIN t ON t.id = s.title_id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT k.id, g,"
		"  (k.i = 1 AND g > 0) OR (k.i = 3 AND g = 2) FROM k, generate_series(0, 2) g RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT k.id, g, e, 'Part ' || e, DATE '2018-02-05' + e, (k.i = 1 AND g > 0) OR (k.i = 3 AND g = 2)"
		" FROM k, generate_series(0, 2) g, generate_series(1, 4) e WHERE g > 0 OR e < 4", SETTLED_SERIES, fx->root_id);
	add_files(fx, "e.season_number > 0 AND e.series_id IN (SELECT s.id FROM series s JOIN meta_title t ON t.id ="
		" s.title_id WHERE t.provider_id IN ('w1', 'w2', 'w3'))", "'bluray_1080p'", "", "", "s");
}

/* The state of the settled series @i (make_settled()), which @counted and
 * @derived both give. */
static AxisTvState
settled_state(
	const GPtrArray *counted,
	const GPtrArray *derived,
	guint            i,
	const gchar    **reason
){
	g_autofree gchar *title = g_strdup_printf("Settled Series %u", i);
	guint k;

	for (k = 0; k < derived->len; k++) {
		const AxisTvSummary *d = g_ptr_array_index(derived, k);
		const AxisTvSummary *c = g_ptr_array_index(counted, k);

		if (g_strcmp0(d->series->title, title) != 0)
			continue;
		g_assert_cmpint(c->state, ==, d->state);
		g_assert_cmpuint(c->monitored, ==, d->monitored);
		g_assert_cmpuint(c->series->n_seasons, ==, 2);
		g_assert_true(c->series->has_specials);
		g_assert_false(c->series->specials_monitored);
		g_assert_cmpuint(c->series->n_seasons_monitored, ==, d->series->n_seasons_monitored);
		if (reason != NULL)
			*reason = d->reason;
		return d->state;
	}
	g_error("no series called %s", title);
	return AXIS_TV_ATTENTION;
}

/* @counted: by the database alone, no episode read; @derived: from every
 * episode's facts.  Every number and the state; the sentence, unless the
 * series needs attention (its episodes are then read before it is shown). */
static void
assert_same_counts(
	const AxisTvSummary *counted,
	const AxisTvSummary *derived
){
	guint i;

	g_assert_cmpint(counted->series->id, ==, derived->series->id);
	for (i = 0; i < AXIS_TV_N_STATES; i++) {
		if (counted->counts[i] != derived->counts[i])
			g_test_message("%s: %u counted as %s, %u derived", derived->series->title, counted->counts[i],
				axis_tv_state_name((AxisTvState)i), derived->counts[i]);
		g_assert_cmpuint(counted->counts[i], ==, derived->counts[i]);
	}
	g_assert_cmpuint(counted->episodes, ==, derived->episodes);
	g_assert_cmpuint(counted->in_library, ==, derived->in_library);
	g_assert_cmpuint(counted->aired_monitored, ==, derived->aired_monitored);
	g_assert_cmpint(counted->state, ==, derived->state);
	g_assert_cmpstr(counted->next_air_date, ==, derived->next_air_date);
	g_assert_cmpuint(counted->attention->len, ==, 0);
	g_assert_cmpuint(counted->counts[AXIS_TV_ATTENTION], ==, derived->attention->len);
	if (derived->attention->len == 0)
		g_assert_cmpstr(counted->reason, ==, derived->reason);
	else
		g_assert_null(counted->reason);
}

static void
assert_same_summary(
	const AxisTvSummary *counted,
	const AxisTvSummary *derived
){
	guint i;

	g_assert_cmpint(counted->series->id, ==, derived->series->id);
	g_assert_cmpstr(counted->series->title, ==, derived->series->title);
	for (i = 0; i < AXIS_TV_N_STATES; i++) {
		if (counted->counts[i] != derived->counts[i])
			g_test_message("%s: %u counted as %s, %u derived", derived->series->title, counted->counts[i],
				axis_tv_state_name((AxisTvState)i), derived->counts[i]);
		g_assert_cmpuint(counted->counts[i], ==, derived->counts[i]);
	}
	g_assert_cmpuint(counted->episodes, ==, derived->episodes);
	g_assert_cmpuint(counted->in_library, ==, derived->in_library);
	g_assert_cmpuint(counted->aired_monitored, ==, derived->aired_monitored);
	g_assert_cmpint(counted->state, ==, derived->state);
	g_assert_cmpstr(counted->next_air_date, ==, derived->next_air_date);
	g_assert_cmpstr(counted->reason, ==, derived->reason);
	g_assert_cmpuint(counted->attention->len, ==, derived->attention->len);
	for (i = 0; i < derived->attention->len; i++) {
		const AxisTvAttentionItem *a = g_ptr_array_index(counted->attention, i);
		const AxisTvAttentionItem *b = g_ptr_array_index(derived->attention, i);

		g_assert_cmpint(a->episode_id, ==, b->episode_id);
		g_assert_cmpint(a->kind, ==, b->kind);
		g_assert_cmpstr(a->reason, ==, b->reason);
		g_assert_cmpint(a->search_id, ==, b->search_id);
		g_assert_cmpint(a->import_id, ==, b->import_id);
	}
}

static void
test_counted_as_derived(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) alone = NULL;
	g_autoptr(GPtrArray) counted = NULL;
	g_autoptr(GPtrArray) derived = NULL;
	g_autoptr(GError) error = NULL;
	guint totals[AXIS_TV_N_STATES] = { 0 };
	guint kinds[AXIS_ACQUIRE_ATTENTION_FAILED + 1] = { 0 };
	guint series_states[AXIS_TV_N_STATES] = { 0 };
	guint held = 0;
	guint attention = 0;
	guint read;
	AxisTestReply reply;
	guint i;
	guint k;

	make_varied(fx);
	/* (After the varied ones: what their episodes' ids decide is not theirs.) */
	make_settled(fx);
	g_assert_true(axis_tv_list(fx->tv, &derived, &error));
	g_assert_no_error(error);
	/* By the database alone: not one episode read. */
	read = axis_tv_episodes_read();
	g_assert_true(axis_tv_list_counted(fx->tv, FALSE, &alone, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_tv_episodes_read(), ==, read);
	g_assert_cmpuint(alone->len, ==, VARIED_SERIES + SETTLED_SERIES);
	g_assert_true(axis_tv_list_counted(fx->tv, TRUE, &counted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(counted->len, ==, VARIED_SERIES + SETTLED_SERIES);
	g_assert_cmpuint(derived->len, ==, VARIED_SERIES + SETTLED_SERIES);
	/* Spec (tv, series-monitoring-control): A series is "Not monitored" only
	 * when nothing of it is monitored.  Counted or derived, the same. */
	{
		const gchar *reason = NULL;

		g_assert_cmpint(settled_state(alone, derived, 1, &reason), ==, AXIS_TV_IN_LIBRARY);
		assert_contains(reason, "Every monitored episode that has aired is in the library.");
		assert_contains(reason, "New seasons and episodes are monitored as they are announced.");
		g_assert_cmpint(settled_state(alone, derived, 2, &reason), ==, AXIS_TV_UNMONITORED);
		assert_contains(reason, "No season is monitored, and new seasons are not");
		assert_contains(reason, "8 episodes are in the library.");
		g_assert_cmpint(settled_state(alone, derived, 3, &reason), ==, AXIS_TV_IN_LIBRARY);
		assert_contains(reason, "Every monitored episode that has aired is in the library.");
		assert_lacks(reason, "New seasons and episodes");
		g_assert_cmpint(settled_state(alone, derived, 4, &reason), ==, AXIS_TV_NOT_AIRED);
		assert_contains(reason, "No monitored episode has aired. New seasons and episodes are monitored");
		g_assert_cmpint(settled_state(alone, derived, 5, &reason), ==, AXIS_TV_UNMONITORED);
		g_assert_cmpstr(reason, ==, "No season is monitored, and new seasons are not: Axis searches for nothing of it.");
		g_assert_cmpint(settled_state(counted, derived, 1, NULL), ==, AXIS_TV_IN_LIBRARY);
		g_assert_cmpint(settled_state(counted, derived, 2, NULL), ==, AXIS_TV_UNMONITORED);
	}
	for (i = 0; i < derived->len; i++) {
		const AxisTvSummary *d = g_ptr_array_index(derived, i);

		assert_same_counts(g_ptr_array_index(alone, i), d);
		assert_same_summary(g_ptr_array_index(counted, i), d);
		series_states[d->state]++;
		attention += d->counts[AXIS_TV_ATTENTION];
		for (k = 0; k < AXIS_TV_N_STATES; k++)
			totals[k] += d->counts[k];
		for (k = 0; k < d->attention->len; k++) {
			const AxisTvAttentionItem *item = g_ptr_array_index(d->attention, k);

			kinds[item->kind]++;
		}
		held += ((const AxisTvSummary *)g_ptr_array_index(counted, i))->held;
	}
	/* The fixture is what it claims: every state of an episode is there, and
	 * the reasons a search, an import, a client and a series give. */
	for (k = 0; k < AXIS_TV_N_STATES; k++) {
		g_test_message("%u episodes %s", totals[k], axis_tv_state_name((AxisTvState)k));
		g_assert_cmpuint(totals[k], >, 0);
	}
	g_assert_cmpuint(kinds[AXIS_ACQUIRE_ATTENTION_IMPORT], >, 0);
	g_assert_cmpuint(kinds[AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT], >, 0);
	g_assert_cmpuint(kinds[AXIS_ACQUIRE_ATTENTION_NO_ROOT], >, 0);
	g_assert_cmpuint(kinds[AXIS_ACQUIRE_ATTENTION_REJECTED], >, 0);
	g_assert_cmpuint(kinds[AXIS_ACQUIRE_ATTENTION_FAILED], >, 0);
	g_assert_cmpuint(held, >, 0);
	g_assert_cmpuint(held, <, kinds[AXIS_ACQUIRE_ATTENTION_REJECTED]);
	/* Series in several states, so that the sentence of one that does not
	 * need attention was compared too; and only the series that need
	 * attention had their episodes read. */
	for (k = 0; k < AXIS_TV_N_STATES; k++)
		g_test_message("%u series %s", series_states[k], axis_tv_state_name((AxisTvState)k));
	g_assert_cmpuint(series_states[AXIS_TV_ATTENTION], >, 0);
	g_assert_cmpuint(series_states[AXIS_TV_ATTENTION], <, VARIED_SERIES);
	g_assert_cmpuint(series_states[AXIS_TV_WANTED], >, 0);
	g_assert_cmpuint(series_states[AXIS_TV_TO_IMPORT], >, 0);
	g_assert_cmpuint(series_states[AXIS_TV_SEARCHING], >, 0);
	g_test_message("%u of %u series need attention; reading what is wrong with them read %u episodes' facts",
		series_states[AXIS_TV_ATTENTION], VARIED_SERIES, axis_tv_episodes_read() - read);

	/* The pages say the same.  The list's tabs: the series by their state. */
	reply = get_page(fx, "/series");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(tab_count(reply.body, "All"), ==, VARIED_SERIES + SETTLED_SERIES);
	g_assert_cmpuint(series_states[AXIS_TV_UNMONITORED], >=, 2);
	for (k = 0; k < AXIS_TV_N_STATES; k++) {
		if (series_states[k] > 0)
			g_assert_cmpuint(tab_count(reply.body, axis_tv_state_label((AxisTvState)k)), ==, series_states[k]);
	}
	axis_test_reply_clear(&reply);
	/* Wanted's tabs: the episodes by their state, and the ones that wait for
	 * a person. */
	reply = get_page(fx, "/series/wanted");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(tab_count(reply.body, "All"), ==, totals[AXIS_TV_WANTED] + totals[AXIS_TV_ATTENTION] +
		totals[AXIS_TV_SEARCHING]);
	g_assert_cmpuint(tab_count(reply.body, "Needs attention"), ==, attention);
	g_assert_cmpuint(tab_count(reply.body, "Upgrade wanted"), ==, totals[AXIS_TV_UPGRADE_WANTED]);
	axis_test_reply_clear(&reply);
	reply = get_page(fx, "/series/wanted?state=held");
	g_assert_cmpuint(tab_count(reply.body, "Waiting for Grab this one"), ==, held);
	axis_test_reply_clear(&reply);
}

/* Wanted in pages: every wanted episode once, in order, a hundred a page. */
static void
test_wanted_pages(Fixture *fx, gconstpointer data)
{
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree gchar *path = g_strdup("/series/wanted");
	guint total = 0;
	guint pages = 0;

	make_varied(fx);
	while (path != NULL) {
		AxisTestReply reply = get_page(fx, path);
		const gchar *at;
		const gchar *next;
		guint rows = 0;

		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_cmpuint(pages, <, 200);
		if (pages == 0)
			total = tab_count(reply.body, "All");
		for (at = strstr(reply.body, "class=\"episode-line\""); at != NULL;
		     at = strstr(at + 1, "class=\"episode-line\"")) {
			/* A row is told by its series' link and its episode's line. */
			const gchar *row = g_strrstr_len(reply.body, at - reply.body, "<tr");
			const gchar *end = strstr(at, "</td>");
			gchar *key = g_strndup(row, end - row);

			g_assert_true(g_hash_table_add(seen, key));
			rows++;
		}
		g_assert_cmpuint(rows, <=, 100);
		g_assert_cmpuint(rows, >, 0);
		pages++;
		g_clear_pointer(&path, g_free);
		next = strstr(reply.body, ">Next page</a>");
		if (next != NULL) {
			const gchar *href = g_strrstr_len(reply.body, next - reply.body, "href=\"");
			const gchar *close = strchr(href + 6, '"');
			g_autofree gchar *escaped = g_strndup(href + 6, close - href - 6);
			g_auto(GStrv) parts = g_strsplit(escaped, "&amp;", -1);

			path = g_strjoinv("&", parts);
			g_assert_cmpuint(rows, ==, 100);
			assert_contains(reply.body, "100 of ");
		}
		axis_test_reply_clear(&reply);
	}
	g_test_message("%u wanted episodes in %u pages", total, pages);
	g_assert_cmpuint(g_hash_table_size(seen), ==, total);
	g_assert_cmpuint(pages, ==, (total + 99) / 100);
}

/* ── The pages where the library is large ───────────────────────── */

#define SCALE_SERIES 1500
#define SCALE_JOBS   160000

static void
make_library(Fixture *fx)
{
	gint64 began = g_get_monotonic_time();

	/* 1,500 series of two seasons of twelve episodes: aired and monitored,
	 * but for one series in ten whose second season airs later and one in
	 * fifteen with a season that is not monitored. */
	sql(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, provider, provider_id)"
		" SELECT 'series', 'Generated Series ' || i, 2019, 'ended', 'fixture', 'z' || i FROM generate_series(1, %d) i"
		" RETURNING id), s AS (INSERT INTO series (title_id, quality_profile_id, root_id) SELECT id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT " FROM t RETURNING id),"
		" n AS (INSERT INTO series_season (series_id, number, monitored) SELECT s.id, g, true FROM s,"
		" generate_series(1, 2) g RETURNING 1)"
		" INSERT INTO series_episode (series_id, season_number, number, title, air_date, monitored)"
		" SELECT s.id, g, e, 'Part ' || e, CASE WHEN g = 2 AND s.id %% 10 = 0 THEN DATE '2026-11-10' + e"
		"  ELSE DATE '2019-03-04' + e END, NOT (g = 1 AND s.id %% 15 = 0) FROM s, generate_series(1, 2) g,"
		" generate_series(1, 12) e", SCALE_SERIES, fx->root_id);
	/* Two episodes in three have a file, one in seven of them below the
	 * profile's cutoff. */
	add_files(fx, "e.id % 20 < 13 AND e.air_date < DATE '2026-01-01'",
		"CASE WHEN e.id % 7 = 0 THEN 'hdtv_720p' ELSE 'webdl_1080p' END", "", "", "z");
	/* The others were searched for, twice each; one in twenty of them found
	 * only what the profile rejects, with the releases found kept. */
	add_searches(fx, "e.id % 20 >= 13 AND e.air_date < DATE '2026-01-01'", "nothing_found", "scheduled", 0);
	add_searches(fx, "e.id % 20 >= 14 AND e.air_date < DATE '2026-01-01'", "nothing_found", "scheduled", 0);
	add_searches(fx, "e.id % 20 = 13 AND e.air_date < DATE '2026-01-01'", "none_acceptable", "scheduled", 0);
	sql(fx, "INSERT INTO series_candidate (search_id, position, release_id, source_id, name, protocol, name_key,"
		" quality, match, match_text, accepted, safety, mismatched) SELECT ss.id, p, ss.id * 100 + p, 1,"
		" 'Generated.Series.E' || ss.id || '.' || p || '.480p.SDTV-GRP', 'usenet', 'generated-' || ss.id || '-' || p,"
		" 'sdtv', CASE WHEN p %% 5 = 0 THEN 'none' ELSE 'match' END, 'As the test made it.', false, false,"
		" p %% 9 = 0 FROM series_search ss, generate_series(1, 20) p WHERE ss.outcome = 'none_acceptable'");
	/* Downloads under way and arrived, and searches in flight. */
	add_grabs(fx, "e.id % 20 = 19 AND e.id % 180 = 19 AND e.air_date < DATE '2026-01-01'", "downloading", FALSE, FALSE);
	add_grabs(fx, "e.id % 20 = 18 AND e.id % 360 = 18 AND e.air_date < DATE '2026-01-01'", "completed", TRUE, FALSE);
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, dedupe_key) SELECT 'tv.search', 'searches', 'queued', 2,"
		" 'tv.search:' || e.series_id || ':episode:' || e.id FROM series_episode e WHERE e.id %% 20 = 17"
		" AND e.id %% 100 = 17 AND e.air_date < DATE '2026-01-01'");
	/* The finished jobs a few days leave behind. */
	sql(fx, "INSERT INTO job (kind, queue, state, max_attempts, attempts, dedupe_key, finished_at) SELECT"
		" CASE WHEN i %% 3 = 0 THEN 'tv.search' ELSE 'download.poll' END, 'searches', 'succeeded', 2, 1,"
		" 'tv.search:' || (i %% %d) || ':episode:' || i, now() - (i %% 7000) * interval '1 minute'"
		" FROM generate_series(1, %d) i", SCALE_SERIES, SCALE_JOBS);
	sql(fx, "ANALYZE");
	g_test_message("the library (%d series, %" G_GINT64_FORMAT " episodes, %" G_GINT64_FORMAT " files, %" G_GINT64_FORMAT
		" searches, %" G_GINT64_FORMAT " releases found, %" G_GINT64_FORMAT " open grabs, %" G_GINT64_FORMAT
		" jobs of which %" G_GINT64_FORMAT " in flight) took %" G_GINT64_FORMAT " ms to make", SCALE_SERIES,
		qi(fx, "SELECT count(*) FROM series_episode"), qi(fx, "SELECT count(*) FROM series_file"),
		qi(fx, "SELECT count(*) FROM series_search"), qi(fx, "SELECT count(*) FROM series_candidate"),
		qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), qi(fx, "SELECT count(*) FROM job"),
		qi(fx, "SELECT count(*) FROM job WHERE state IN ('queued', 'running')"),
		(g_get_monotonic_time() - began) / 1000);
}

/* A page fetched three times: the fastest in milliseconds, how many
 * statements one view sent and how many episodes' facts it read. */
typedef struct
{
	gint64 ms;
	guint  statements;
	guint  episodes;
	gsize  bytes;
} Cost;

static Cost
cost_of(
	Fixture     *fx,
	const gchar *path,
	const gchar *contains
){
	Cost cost = { G_MAXINT64, 0, 0, 0 };
	guint i;

	for (i = 0; i < 3; i++) {
		guint statements = axis_db_pool_get_statements_sent(fx->t->pool);
		guint episodes = axis_tv_episodes_read();
		gint64 began = g_get_monotonic_time();
		AxisTestReply reply = get_page(fx, path);

		cost.ms = MIN(cost.ms, (g_get_monotonic_time() - began) / 1000);
		g_assert_cmpuint(reply.status, ==, 200);
		if (contains != NULL)
			assert_contains(reply.body, contains);
		cost.statements = axis_db_pool_get_statements_sent(fx->t->pool) - statements;
		cost.episodes = axis_tv_episodes_read() - episodes;
		cost.bytes = reply.length;
		axis_test_reply_clear(&reply);
	}
	g_test_message("%s: %" G_GINT64_FORMAT " ms, %u statements, %u episodes read, %" G_GSIZE_FORMAT " bytes", path,
		cost.ms, cost.statements, cost.episodes, cost.bytes);
	return cost;
}

/* How long @sql takes in the database, and its plan's lines that say what
 * was scanned how often (told, not asserted). */
static void
explain(
	Fixture     *fx,
	const gchar *what,
	const gchar *statement,
	const gchar *p1,
	const gchar *p2,
	const gchar *p3
){
	g_autofree gchar *with1 = NULL;
	g_autofree gchar *with2 = NULL;
	g_autofree gchar *with3 = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *plan = NULL;
	g_auto(GStrv) parts = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	/* The parameters as literals: the plan of the statement as it is sent. */
	parts = g_strsplit(statement, "$1", -1);
	with1 = g_strjoinv(p1, parts);
	g_clear_pointer(&parts, g_strfreev);
	parts = g_strsplit(with1, "$2", -1);
	with2 = g_strjoinv(p2, parts);
	g_clear_pointer(&parts, g_strfreev);
	parts = g_strsplit(with2, "$3", -1);
	with3 = g_strjoinv(p3, parts);
	text = g_strdup_printf("DO $do$ DECLARE r record; BEGIN CREATE TEMP TABLE IF NOT EXISTS plan_lines (n serial, l text);"
		" TRUNCATE plan_lines; FOR r IN EXPLAIN (ANALYZE, BUFFERS) %s LOOP INSERT INTO plan_lines (l) VALUES"
		" (r.\"QUERY PLAN\"); END LOOP; END $do$", with3);
	axis_test_db_exec(fx->t->db, text);
	plan = axis_test_db_query(fx->t->db, "SELECT string_agg(l, E'\\n' ORDER BY n) FROM plan_lines");
	lines = g_strsplit(plan != NULL ? plan : "", "\n", -1);
	g_test_message("plan of %s:", what);
	/* The lines that matter: the whole, what ran ten thousand times and
	 * more, the scans of the job table, and the compilation. */
	for (i = 0; lines[i] != NULL; i++) {
		const gchar *loops = strstr(lines[i], "loops=");

		if (i == 0 || strstr(lines[i], "Execution Time") != NULL || strstr(lines[i], "Planning Time") != NULL ||
		    strstr(lines[i], "JIT") != NULL || strstr(lines[i], "Timing: ") != NULL || strstr(lines[i], " job ") != NULL ||
		    (loops != NULL && g_ascii_strtoull(loops + 6, NULL, 10) >= 10000))
			g_test_message("  %s", g_strstrip(lines[i]));
	}
}

static void
pump_until_dashboard_read(
	Fixture *fx,
	guint    before
){
	gint64 until = g_get_monotonic_time() + 120 * G_USEC_PER_SEC;

	while (axis_web_dashboard_get_readings(fx->web) <= before) {
		g_assert_cmpint(g_get_monotonic_time(), <, until);
		g_main_context_iteration(NULL, FALSE);
		g_usleep(200);
	}
}

static void
test_pages(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) derived = NULL;
	g_autoptr(GPtrArray) counted = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *series_page = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *mark = NULL;
	g_autofree gchar *cards = NULL;
	g_autoptr(AxisTestStream) stream = NULL;
	AxisTestReply reply;
	const gchar *have;
	gint64 began;
	gint64 before_ms;
	gint64 after_ms;
	guint episodes;
	guint statements;
	guint readings;
	Cost cost;
	guint i;

	make_library(fx);

	/* Before and after, where it is one function each: every series counted
	 * from every episode's facts (what the dashboard, the list and Wanted
	 * each did at every view), and counted by the database. */
	episodes = axis_tv_episodes_read();
	began = g_get_monotonic_time();
	g_assert_true(axis_tv_list(fx->tv, &derived, &error));
	g_assert_no_error(error);
	before_ms = (g_get_monotonic_time() - began) / 1000;
	g_assert_cmpuint(axis_tv_episodes_read() - episodes, ==, SCALE_SERIES * 24);
	episodes = axis_tv_episodes_read();
	began = g_get_monotonic_time();
	g_assert_true(axis_tv_list_counted(fx->tv, FALSE, &counted, &error));
	g_assert_no_error(error);
	after_ms = (g_get_monotonic_time() - began) / 1000;
	g_test_message("every series from every episode's facts: %" G_GINT64_FORMAT " ms, %d episodes read; counted by the "
		"database: %" G_GINT64_FORMAT " ms, %u episodes read", before_ms, SCALE_SERIES * 24, after_ms,
		axis_tv_episodes_read() - episodes);
	g_assert_cmpuint(counted->len, ==, derived->len);
	for (i = 0; i < derived->len; i++)
		assert_same_counts(g_ptr_array_index(counted, i), g_ptr_array_index(derived, i));

	{
		g_autofree gchar *classes = axis_tv_store_statement("classes");
		g_autofree gchar *list = axis_tv_store_statement("series-list");
		g_autofree gchar *all = axis_tv_store_statement("series-all");
		g_autofree gchar *facts = axis_tv_store_statement("episodes");

		explain(fx, "the episodes counted by class", classes, "NULL", "'2026-11-14'", "'none'");
		explain(fx, "the series of a list", list, "NULL", "", "");
		explain(fx, "every series with what its own page reads (as every list read them before)", all, "", "", "");
		explain(fx, "the facts of every episode (as every list read them before, the searches in flight joined)",
			facts, "(SELECT array_agg(id) FROM series)", "false", "");
	}
	explain(fx, "the search in flight of every episode as it was looked up before, a row at a time",
		"SELECT count((SELECT j.state::text FROM job j WHERE j.dedupe_key IN ('tv.search:' || e.series_id || ':episode:'"
		" || e.id, 'tv.search:' || e.series_id || ':season:' || e.season_number, 'tv.search:' || e.series_id ||"
		" ':series') AND j.state IN ('queued', 'running') ORDER BY j.state = 'running' DESC LIMIT 1))"
		" FROM series_episode e", "", "", "");

	/* The list: the series by state, a page of them. */
	cost = cost_of(fx, "/series", "Generated Series");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);
	g_assert_cmpuint(cost.episodes, <, 100 * 24 + 1);
	g_assert_cmpuint(cost.statements, <, 16);
	cost = cost_of(fx, "/series?state=attention&sort=title", "Generated Series");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);
	g_assert_cmpuint(cost.episodes, <, 100 * 24 + 1);
	cost = cost_of(fx, "/series?q=Series+12&view=table", "Generated Series 12");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);

	/* Wanted: a page of a hundred, whatever is wanted. */
	cost = cost_of(fx, "/series/wanted", "100 of ");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);
	g_assert_cmpuint(cost.episodes, ==, 100);
	g_assert_cmpuint(cost.statements, <, 8);
	g_assert_cmpuint(cost.bytes, <, 120 * 1024);
	cost = cost_of(fx, "/series/wanted?state=attention", "Episodes that need attention");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);
	g_assert_cmpuint(cost.episodes, <=, 100);
	cost = cost_of(fx, "/series/wanted?state=upgrade_wanted", "Episodes wanted for an upgrade");
	g_assert_cmpint(cost.ms, <, TARGET_LIST_MS * SLACK);
	g_assert_cmpuint(cost.episodes, <=, 100);

	/* The calendar reads its three weeks; a series' page its own episodes. */
	cost = cost_of(fx, "/series/calendar", "TV calendar");
	g_assert_cmpint(cost.ms, <, TARGET_OTHER_MS * SLACK);
	g_assert_cmpuint(cost.episodes, <, 3000);
	one = axis_test_db_query(fx->t->db, "SELECT min(id)::text FROM series");
	series_page = g_strdup_printf("/series/%s", one);
	cost = cost_of(fx, series_page, "Generated Series");
	g_assert_cmpint(cost.ms, <, TARGET_OTHER_MS * SLACK);
	g_assert_cmpuint(cost.episodes, ==, 24);
	cost = cost_of(fx, "/series/history", NULL);
	g_assert_cmpint(cost.ms, <, TARGET_OTHER_MS * SLACK);
	cost = cost_of(fx, "/films", NULL);
	g_assert_cmpint(cost.ms, <, TARGET_OTHER_MS * SLACK);

	/* The dashboard as it was read at every view (a shell not started). */
	cost = cost_of(fx, "/", "Generated Series");
	g_test_message("the dashboard read at the view (not started): %" G_GINT64_FORMAT " ms", cost.ms);
	g_assert_cmpuint(cost.episodes, ==, 0);

	/* Started: read in the background, and a view is drawn from memory. */
	axis_web_dashboard_set_tuning(fx->web, 50, 3600 * 1000);
	began = g_get_monotonic_time();
	axis_web_dashboard_start(fx->web);
	reply = get_page(fx, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	g_test_message("the dashboard before its first reading: %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - began) /
		1000);
	axis_test_reply_clear(&reply);
	pump_until_dashboard_read(fx, 0);
	g_test_message("the first reading of the dashboard's figures took %" G_GINT64_FORMAT " ms in the background",
		(g_get_monotonic_time() - began) / 1000);
	pump_until_dashboard_read(fx, 1);
	statements = axis_db_pool_get_statements_sent(fx->t->pool);
	episodes = axis_tv_episodes_read();
	began = g_get_monotonic_time();
	reply = get_page(fx, "/?part=cards");
	after_ms = (g_get_monotonic_time() - began) / 1000;
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "1500 series");
	assert_contains(reply.body, "episodes wanted");
	assert_lacks(reply.body, "Counting...");
	cards = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	g_test_message("the dashboard's cards from memory: %" G_GINT64_FORMAT " ms", after_ms);
	g_assert_cmpint(after_ms, <, TARGET_DASHBOARD_MS * SLACK);
	/* No statement, and no episode read, on the view. */
	g_assert_cmpuint(axis_db_pool_get_statements_sent(fx->t->pool), ==, statements);
	g_assert_cmpuint(axis_tv_episodes_read(), ==, episodes);

	/* An open dashboard: told after a reading, it asks for its cards with the
	 * mark of the ones it has, and is answered with nothing when they are
	 * the same. */
	stream = axis_test_stream_open(fx->port, "/events?topic=" AXIS_WEB_DASHBOARD_TOPIC, fx->admin, 0);
	g_assert_true(axis_test_stream_wait_for(stream, "\r\n\r\n", 5000));
	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	readings = axis_web_dashboard_get_readings(fx->web);
	began = g_get_monotonic_time();
	cost = cost_of(fx, "/", "id=\"dashboard-cards\"");
	g_assert_cmpint(cost.ms, <, TARGET_DASHBOARD_MS * SLACK);
	pump_until_dashboard_read(fx, readings);
	g_assert_true(axis_test_stream_wait_for(stream, "event: " AXIS_WEB_DASHBOARD_EVENT, 5000));
	/* (A reading between the two requests gives other cards: asked again.) */
	for (i = 0; i < 20; i++) {
		g_autofree gchar *path = NULL;
		guint status;

		readings = axis_web_dashboard_get_readings(fx->web);
		reply = get_page(fx, "/?part=cards&have=0123456789abcdef");
		g_assert_cmpuint(reply.status, ==, 200);
		have = strstr(reply.body, "hx-get=\"/?part=cards&amp;have=");
		g_assert_nonnull(have);
		assert_contains(reply.body, "hx-trigger=\"sse:" AXIS_WEB_DASHBOARD_EVENT "\"");
		g_clear_pointer(&mark, g_free);
		mark = g_strndup(have + strlen("hx-get=\"/?part=cards&amp;have="), 16);
		axis_test_reply_clear(&reply);
		path = g_strdup_printf("/?part=cards&have=%s", mark);
		reply = get_page(fx, path);
		status = reply.status;
		if (status == 204)
			g_assert_cmpuint(reply.length, ==, 0);
		axis_test_reply_clear(&reply);
		if (axis_web_dashboard_get_readings(fx->web) != readings)
			continue;
		g_assert_cmpuint(status, ==, 204);
		break;
	}
	g_assert_cmpuint(i, <, 20);
	g_assert_nonnull(cards);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/tv-scale/counted-as-derived", Fixture, NULL, test_counted_as_derived);
	AXIS_TEST_ADD_DB_CASE("/web/tv-scale/wanted-pages", Fixture, NULL, test_wanted_pages);
	AXIS_TEST_ADD_DB_CASE("/web/tv-scale/pages", Fixture, NULL, test_pages);

	return g_test_run();
}
