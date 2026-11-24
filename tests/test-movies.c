/*
 * test-movies.c - the movies module against the metadata test provider, the
 * fake Prowlarr and the fixture download client together, on a real
 * database and a real volume, with real jobs where the task says so
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-movies-module:
 *   4.1 end to end: add, search, the best acceptable release grabbed,
 *       progress, completion marks it to import (the job queue running the
 *       module's own jobs)
 *   4.2 the states from real rows; not available before the release date
 *   4.3 a release of another film, an ambiguous one, one the indexer names
 *       another film's, and a blocklisted one are never grabbed; every
 *       verdict and reason is stored
 *   4.4 the job run twice at once grabs once (and the database's own guard)
 *   4.5 failure, stall and gone take the next best; none left: attention
 *   4.6 the scheduler: pacing, the floor grows and resets, nothing searched
 *       while a grab is in flight or before the film is available
 *   4.7 no usable client, no indexer, the destination offline: attention,
 *       nothing sent
 *   4.8 a manual grab: allowed for a profile's rejection, refused for a
 *       fixed rule's, recorded with the user
 *   4.9 (half) every action is in the film's history with the user
 * Also: an uncertain send counts as in flight; a film follows its title
 * through a metadata merge.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-newznab.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "core/axis-data-dir.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "indexer/axis-indexer-sites.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "modules/movies/axis-movies.h"
#include "quality/axis-quality-store.h"

#define KEY "prowlarr-key-for-films-0c4d7e21"
#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

#define NO_CLIENT    1
#define NO_SOURCE    2
/* add-indexer-newznab: a native Newznab indexer, and no Prowlarr. */
#define NATIVE_ONLY  4
/* The real TMDb provider as well, pointed at a fake TMDb. */
#define WITH_TMDB    8
#define TMDB_TOKEN   "eyJhbGciOiJIUzI1NiJ9.dGVzdC10b2tlbi1ub3QtcmVhbA.c2lnbmF0dXJlLW5vdC1yZWFs"
#define NZB_KEY      "nzb-key-for-films-5a9e01c3"

/* The Mazework (1999) as the indexers and the fixture provider number it. */
#define MAZEWORK_IMDB  600003
#define RELOADED_IMDB 680000

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisTestMetaFixture  *fa;
	AxisTestPlex         *tmdb;		/* the fake TMDb, with WITH_TMDB */
	gchar                *tmdb_origin;
	AxisMetaProviderInfo  tmdb_info;	/* the real provider, pointed at it */
	AxisMeta             *meta;
	AxisTestProwlarr     *prowlarr;
	AxisTestNewznab      *nzb;
	AxisIndexers         *indexers;
	gint64                source_id;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisMovies           *movies;
	AxisJobRegistry      *registry;
	AxisJobQueue         *queue;
} Fixture;

/* ── Setting up ─────────────────────────────────────────────────── */

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, 200, "application/json", body, NULL);
}

static gint64
add_source(Fixture *fx)
{
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr";
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
save_client(
	Fixture     *fx,
	gint64       id,
	const gchar *name,
	const gchar *kind,
	gboolean     enabled
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 saved = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = kind;
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = enabled;
	spec.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, id, &spec, &saved, &error));
	g_assert_no_error(error);
	return saved;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	if ((flags & WITH_TMDB) != 0) {
		fx->tmdb = axis_test_plex_new();
		g_assert_nonnull(fx->tmdb);
		fx->tmdb_origin = axis_test_plex_base(fx->tmdb);
		fx->tmdb_info = *axis_meta_tmdb_get_info();
		fx->tmdb_info.api_origin = fx->tmdb_origin;
		fx->tmdb_info.image_origin = fx->fa->image_origin;
		g_assert_true(axis_meta_add_provider(fx->meta, &fx->tmdb_info, &error));
	}
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);

	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &indexer_tuning);

	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->downloads, &download_tuning);

	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_QUALITY_MODULE_ID, axis_quality_store_settings(), &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_MOVIES_MODULE_ID, axis_movies_get_module()->settings,
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_no_error(error);
	if (fx->tmdb != NULL) {
		g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, TMDB_TOKEN, &error));
		g_assert_no_error(error);
	}

	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);

	/* A second film called The Mazework, so that a name without a year is
	 * ambiguous; and a film still to come.  (The fake answers with the
	 * route added last.) */
	axis_test_meta_fixture_standard(fx->fa);
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"101\",\"kind\":\"film\",\"title\":\"The Mazework\","
		"\"year\":1999,\"ids\":{\"imdb\":\"tt0600003\"}},{\"id\":\"102\",\"kind\":\"film\",\"title\":\"The Mazework "
		"Reloaded\",\"year\":2003,\"ids\":{\"imdb\":\"tt0680000\"}},{\"id\":\"106\",\"kind\":\"film\",\"title\":"
		"\"The Mazework\",\"year\":2021}]}");
	set_route(fx, "/title/film/106", "{\"id\":\"106\",\"kind\":\"film\",\"title\":\"The Mazework\",\"year\":2021,"
		"\"runtime\":120,\"status\":\"released\",\"release_date\":\"2021-12-22\"}");
	set_route(fx, "/title/film/105", "{\"id\":\"105\",\"kind\":\"film\",\"title\":\"A Film To Come\",\"year\":2031,"
		"\"runtime\":100,\"status\":\"upcoming\",\"release_date\":\"2031-06-01\"}");

	fx->prowlarr = axis_test_prowlarr_new(KEY, "");
	if ((flags & NATIVE_ONLY) != 0) {
		AxisIndexerSourceSpec spec;
		g_autofree gchar *base = NULL;

		fx->nzb = axis_test_newznab_new(NZB_KEY, FALSE, "");
		base = axis_test_newznab_base(fx->nzb);
		memset(&spec, 0, sizeof(spec));
		spec.name = "NewsOwl";
		spec.kind = AXIS_INDEXER_NEWZNAB_NAME;
		spec.address = base;
		spec.enabled = TRUE;
		spec.secret = NZB_KEY;
		g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &fx->source_id, &error));
		g_assert_no_error(error);
	} else if ((flags & NO_SOURCE) == 0) {
		fx->source_id = add_source(fx);
	}
	fx->fake = axis_test_download_fake_new(PASSWORD);
	if ((flags & NO_CLIENT) == 0)
		fx->client_id = save_client(fx, 0, "Usenet", "fixturenzb", TRUE);
}

static void
stop_queue(Fixture *fx)
{
	if (fx->queue == NULL)
		return;
	axis_job_queue_stop(fx->queue, 5000);
	g_clear_object(&fx->queue);
	g_clear_pointer(&fx->registry, axis_job_registry_unref);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	stop_queue(fx);
	axis_movies_set_claim_hook(fx->movies, NULL, NULL);
	axis_movies_unref(fx->movies);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_downloads_unref(fx->downloads);
	axis_indexers_unref(fx->indexers);
	axis_meta_unref(fx->meta);
	axis_quality_store_unref(fx->quality);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_plex_free(fx->tmdb);
	g_free(fx->tmdb_origin);
	axis_test_prowlarr_free(fx->prowlarr);
	axis_test_newznab_free(fx->nzb);
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* The module's own jobs on a real queue. */
static void
start_queue(Fixture *fx)
{
	const AxisJobHandlerInfo *handler;
	g_autoptr(GError) error = NULL;

	fx->registry = axis_job_registry_new();
	for (handler = axis_movies_get_module()->job_handlers; handler->kind != NULL; handler++)
		g_assert_true(axis_job_registry_add(fx->registry, handler, fx->movies, &error));
	fx->queue = axis_job_queue_new(fx->t->pool, fx->registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->t->db, sql);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

static void
exec(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
exec(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	axis_test_db_exec(fx->t->db, sql);
}

static void
assert_qf(
	Fixture     *fx,
	const gchar *expected,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
assert_qf(
	Fixture     *fx,
	const gchar *expected,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	assert_q(fx, expected, sql);
}

/* The first column of the first row of @sql holds @needle. */
static void
assert_q_has(
	Fixture     *fx,
	const gchar *needle,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	assert_contains(value, needle);
}

static gint64
title_of(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 id;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_FILM, provider_id, 5000, &answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

static gint64
add_film_full(
	Fixture               *fx,
	const gchar           *provider_id,
	AxisMovieAvailability  availability,
	gboolean               search_now
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = availability;
	spec.search_now = search_now;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_film(
	Fixture     *fx,
	const gchar *provider_id
){
	return add_film_full(fx, provider_id, AXIS_MOVIE_RELEASED, FALSE);
}

typedef struct
{
	const gchar *title;
	const gchar *protocol;
	gint64       size;
	gint64       imdb;
	const gchar *file;
} Rel;

/* What the fake Prowlarr answers to every search from now on. */
static void
set_releases(
	Fixture   *fx,
	const Rel *rels,
	guint      n
){
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	guint i;

	for (i = 0; i < n; i++) {
		const Rel *r = &rels[i];
		gboolean torrent = g_strcmp0(r->protocol, "torrent") == 0;

		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/%u\",\"title\":\"%s\",\"size\":%"
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"NewsOwl\",\"publishDate\":\"2026-11-01T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=%s\",\"magnetUrl\":null,\"protocol\":\"%s\","
			"\"imdbId\":%" G_GINT64_FORMAT ",\"seeders\":%s,\"leechers\":%s,\"categories\":[{\"id\":2040}]}",
			i > 0 ? "," : "", i, r->title, r->size, origin, KEY, i, r->file != NULL ? r->file : (torrent ?
			"hearth-torrent" : "hearth-nzb"), r->protocol, r->imdb, torrent ? "40" : "null", torrent ? "2" : "null");
	}
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	/* A new answer: what the layer kept of the old one is forgotten. */
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

static AxisMovieSearchReport
search(
	Fixture     *fx,
	gint64       movie,
	const gchar *cause
){
	AxisMovieSearchReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_movies_run_search(fx->movies, movie, cause, "root", NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

static AxisMovieState
state_of(
	Fixture  *fx,
	gint64    movie,
	gchar   **reason
){
	g_autoptr(AxisMovieFacts) facts = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;

	g_assert_true(axis_movies_get(fx->movies, movie, &facts, &error));
	g_assert_no_error(error);
	axis_movies_get_rules(fx->movies, &rules);
	return axis_movie_derive(facts, &rules, reason);
}

/*
 * The list works every film's state out in SQL (axis-movies-list.c) for its
 * counts and its tabs; it must agree with axis_movie_derive() for every film,
 * in every state these tests put films in.
 */
static void
assert_list_agrees(Fixture *fx)
{
	g_autoptr(GHashTable) in_sql = NULL;
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;
	guint i;

	g_assert_true(axis_movies_list_states(fx->movies, &in_sql, &error));
	g_assert_no_error(error);
	g_assert_true(axis_movies_list(fx->movies, &films, &error));
	g_assert_no_error(error);
	axis_movies_get_rules(fx->movies, &rules);
	g_assert_cmpuint(g_hash_table_size(in_sql), ==, films->len);
	for (i = 0; i < films->len; i++) {
		const AxisMovieFacts *f = g_ptr_array_index(films, i);
		g_autofree gchar *reason = NULL;
		AxisMovieState derived = axis_movie_derive(f, &rules, &reason);
		AxisMovieState listed = (AxisMovieState)GPOINTER_TO_INT(g_hash_table_lookup(in_sql, &f->id));

		if (derived != listed)
			g_error("film %" G_GINT64_FORMAT ": derived %s (%s), but the list's SQL says %s", f->id,
				axis_movie_state_name(derived), reason, axis_movie_state_name(listed));
	}
}

static void
assert_state(
	Fixture        *fx,
	gint64          movie,
	AxisMovieState  expected,
	const gchar    *in_reason
){
	g_autofree gchar *reason = NULL;
	AxisMovieState state = state_of(fx, movie, &reason);

	if (state != expected)
		g_error("film %" G_GINT64_FORMAT ": expected %s, got %s (%s)", movie, axis_movie_state_name(expected),
			axis_movie_state_name(state), reason != NULL ? reason : "");
	if (in_reason != NULL)
		assert_contains(reason, in_reason);
	assert_list_agrees(fx);
}

static void
poll(Fixture *fx)
{
	AxisDownloadPollReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, fx->client_id, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_true(report.reached);
}

static AxisMovieFollowReport
follow(Fixture *fx)
{
	AxisMovieFollowReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_no_error(error);
	return report;
}

static guint
n_adds(Fixture *fx)
{
	return axis_test_download_fake_count(fx->fake, "add", NULL);
}

/* The fake client's id of the film's open grab's download. */
static gchar *
client_ref_of(
	Fixture *fx,
	gint64   movie
){
	return q(fx, "SELECT d.client_ref FROM movie_grab g JOIN download d ON d.id = g.download_id"
		" WHERE g.movie_id = %" G_GINT64_FORMAT " AND g.closed_at IS NULL", movie);
}

static gchar *
grabbed_name(
	Fixture *fx,
	gint64   movie
){
	return q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		movie);
}

#define BLURAY "The.Mazework.1999.1080p.BluRay.x264-GRP"
#define WEBDL "The.Mazework.1999.1080p.WEB-DL.DDP5.1.H.264-WEB"
#define WEBRIP "The.Mazework.1999.1080p.WEBRip.x264-RIP"
#define UHD "The.Mazework.1999.2160p.UHD.BluRay.x265-TRK"
#define RELOADED "The.Mazework.Reloaded.2003.1080p.BluRay.x264-GRP"

static const Rel standard[] = {
	{ WEBDL, "usenet", 6000000000, MAZEWORK_IMDB, NULL },
	{ BLURAY, "usenet", 12000000000, MAZEWORK_IMDB, NULL },
	{ RELOADED, "usenet", 12000000000, RELOADED_IMDB, NULL },
	{ UHD, "usenet", 60000000000, MAZEWORK_IMDB, NULL }
};

/* ── 1.2 Adding ─────────────────────────────────────────────────── */

/* Spec: Add; Twice */
static void
test_add(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) uses = NULL;
	g_autoptr(GPtrArray) films = NULL;
	AxisMovieSpec spec;
	gint64 other_root;
	gint64 movie = add_film(fx, "101");
	gint64 id = 0;

	g_assert_cmpint(movie, >, 0);
	g_assert_true(axis_movies_list(fx->movies, &films, &error));
	g_assert_cmpuint(films->len, ==, 1);
	g_assert_cmpstr(((AxisMovieFacts *)g_ptr_array_index(films, 0))->profile_slug, ==, "balanced");
	assert_state(fx, movie, AXIS_MOVIE_WANTED, NULL);
	assert_q(fx, "added", "SELECT kind FROM movie_event ORDER BY id LIMIT 1");
	assert_q(fx, "root", "SELECT by_user FROM movie_event ORDER BY id LIMIT 1");
	/* Its profile is in use now, and cannot be deleted under it. */
	g_assert_true(axis_quality_store_uses(fx->quality, "balanced", &uses, &error));
	g_assert_cmpuint(uses->len, >=, 1);

	/* Twice: refused. */
	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, "101");
	spec.root_id = fx->root_id;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_false(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_EXISTS);
	g_clear_error(&error);
	/* Not a library root for films: refused. */
	other_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "shows", AXIS_MEDIA_TV);
	spec.title_id = title_of(fx, "102");
	spec.root_id = other_root;
	g_assert_false(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID);
	g_clear_error(&error);
	/* Not a film: refused. */
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES ('series', "
		"'A Series', 'fixture', '999')");
	{
		g_autofree gchar *series = q(fx, "SELECT id FROM meta_title WHERE provider_id = '999'");

		spec.title_id = g_ascii_strtoll(series, NULL, 10);
		spec.root_id = fx->root_id;
		g_assert_false(axis_movies_add(fx->movies, &spec, &id, &error));
		g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_INVALID);
		g_clear_error(&error);
	}
	assert_q(fx, "1", "SELECT count(*) FROM movie");
}

/* ── 4.1 End to end, on the job queue ───────────────────────────── */

static void
test_end_to_end(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", BLURAY, NULL);
	g_autofree gchar *file = g_build_filename(place, "the.mazework.mkv", NULL);
	g_autofree gchar *films_dir = g_build_filename(fx->disk, "films", NULL);
	g_autofree gchar *ref = NULL;
	g_autofree gchar *grabbed = NULL;
	g_autofree gchar *reason = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(AxisMovieDetail) detail = NULL;
	gint64 movie;
	gint64 job = 0;
	guint i;

	set_releases(fx, standard, G_N_ELEMENTS(standard));
	start_queue(fx);
	/* Added with "search now": the search runs as a job, and grabs. */
	movie = add_film_full(fx, "101", AXIS_MOVIE_RELEASED, TRUE);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM movie_grab WHERE download_id IS NOT NULL", "1",
		20000));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND state ="
		" 'succeeded'", "1", 10000));
	/* The best acceptable: the BluRay, above the WEB-DL; not the other film,
	 * not the 2160p the profile does not allow. */
	grabbed = grabbed_name(fx, movie);
	g_assert_cmpstr(grabbed, ==, BLURAY);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_q(fx, BLURAY, "SELECT release_name FROM download");
	assert_q(fx, "movies", "SELECT module FROM download");
	assert_q(fx, "4|2", "SELECT n_found || '|' || n_accepted FROM movie_search");
	assert_q(fx, "grabbed", "SELECT outcome FROM movie_search");
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "Usenet");

	/* Progress. */
	ref = client_ref_of(fx, movie);
	g_assert_nonnull(ref);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	poll(fx);
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "40%");

	/* Completed: to import, where it is. */
	axis_test_storage_mkdir(place);
	axis_test_storage_write(file, "film");
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *client_path = g_strdup_printf("/data/downloads/complete/%s", BLURAY);

		axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, client_path, NULL);
	}
	poll(fx);
	assert_state(fx, movie, AXIS_MOVIE_TO_IMPORT, "downloads/complete/" BLURAY);
	/* The follow-up job records the arrival, once. */
	g_assert_true(axis_job_queue_enqueue(fx->queue, AXIS_MOVIES_FOLLOW_KIND, NULL, NULL, &job, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM movie_event WHERE kind = 'completed'", "1",
		10000));
	assert_state(fx, movie, AXIS_MOVIE_TO_IMPORT, "nothing has been moved");

	/* Nothing was moved, and nothing was written to the library root. */
	g_assert_true(g_file_test(file, G_FILE_TEST_IS_REGULAR));
	dir = g_dir_open(films_dir, 0, &error);
	g_assert_no_error(error);
	g_assert_null(g_dir_read_name(dir));

	/* The history tells it. */
	assert_q(fx, "added,searched,grabbed,completed", "SELECT string_agg(kind, ',' ORDER BY id) FROM movie_event");
	g_assert_true(axis_movies_get_detail(fx->movies, movie, 30, &detail, &error));
	g_assert_cmpuint(detail->candidates->len, ==, 4);
	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);

		if (strcmp(c->name, BLURAY) == 0) {
			g_assert_true(c->accepted);
			g_assert_cmpuint(c->rank, ==, 1);
			g_assert_cmpstr(c->note, ==, "Grabbed.");
		} else if (strcmp(c->name, WEBDL) == 0) {
			g_assert_true(c->accepted);
			g_assert_cmpuint(c->rank, ==, 2);
		} else if (strcmp(c->name, RELOADED) == 0) {
			g_assert_false(c->accepted);
			g_assert_true(c->safety);
			g_assert_cmpint(c->match, ==, AXIS_RELEASE_MATCH_NONE);
			assert_contains(((AxisMovieReason *)g_ptr_array_index(c->reasons, 0))->text, "another movie");
		} else {
			g_assert_cmpstr(c->name, ==, UHD);
			g_assert_false(c->accepted);
			g_assert_false(c->safety);
			g_assert_cmpstr(((AxisMovieReason *)g_ptr_array_index(c->reasons, 0))->code, ==, "not_allowed");
		}
	}
	(void)reason;
}

/* What the native indexer answers to every search from now on: the same
 * releases as a Newznab feed, each file by t=get on the indexer. */
static void
set_native_releases(
	Fixture   *fx,
	const Rel *rels,
	guint      n
){
	g_autoptr(GString) items = g_string_new(NULL);
	g_autofree gchar *feed = NULL;
	guint i;

	for (i = 0; i < n; i++) {
		const Rel *r = &rels[i];

		g_string_append_printf(items, "<item><title>%s</title><guid>@ORIGIN@/details/r%u</guid>"
			"<pubDate>Sun, 01 Nov 2026 10:00:00 +0000</pubDate>"
			"<enclosure url=\"@FILES@@BASE@/api?t=get&amp;id=nzb-r%u&amp;apikey=@KEY@\" length=\"%" G_GINT64_FORMAT
			"\" type=\"application/x-nzb\"/><newznab:attr name=\"category\" value=\"2040\"/>"
			"<newznab:attr name=\"size\" value=\"%" G_GINT64_FORMAT "\"/><newznab:attr name=\"imdb\" value=\"%07"
			G_GINT64_FORMAT "\"/></item>", r->title, i, i, r->size, r->size, r->imdb);
	}
	feed = axis_test_newznab_feed(items->str, 40, 5000, 2, 100);
	axis_test_newznab_set_search(fx->nzb, NULL, feed);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

/* add-indexer-newznab task 4.7; spec: Without Prowlarr: a native Usenet
 * indexer alone finds the film by its identifier, and the best release is
 * fetched from it and sent */
static void
test_end_to_end_native(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *grabbed = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *how = NULL;
	AxisMovieSearchReport report;
	AxisIndexerSourceSpec eztv;
	g_autoptr(GError) error = NULL;
	gint64 movie;

	/* A TV site is also switched on: it has no movies, and is not asked. */
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_eztv_kind(), &error));
	memset(&eztv, 0, sizeof(eztv));
	eztv.name = "EZTV";
	eztv.kind = AXIS_INDEXER_EZTV_NAME;
	eztv.address = "https://eztv.example";
	eztv.priority = 20;
	eztv.enabled = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &eztv, NULL, &error));
	g_assert_no_error(error);

	set_native_releases(fx, standard, G_N_ELEMENTS(standard));
	movie = add_film(fx, "101");
	report = search(fx, movie, "manual");
	g_assert_true(report.searched);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	grabbed = grabbed_name(fx, movie);
	g_assert_cmpstr(grabbed, ==, BLURAY);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_q(fx, "4|2", "SELECT n_found || '|' || n_accepted FROM movie_search");
	/* By its IMDb identifier (its capabilities were read first), in its
	 * movie categories, the key in the query and only there; the file from
	 * the indexer itself; Prowlarr asked nothing. */
	query = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(query, ==, "t=movie&imdbid=0600003&cat=2000&extended=1&limit=100&offset=0&apikey=" NZB_KEY);
	g_assert_cmpuint(axis_test_newznab_n_caps(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->prowlarr), ==, 0);
	how = q(fx, "SELECT how FROM movie_search");
	assert_contains(how, "NewsOwl by the IMDb identifier: 4 releases");
	/* The truth about the site that was not asked, and nothing like "0
	 * releases" for it. */
	assert_contains(how, "EZTV not asked, it has no movies");
	g_assert_null(strstr(how, "EZTV by"));
	assert_q(fx, "0", "SELECT count(*) FROM movie_search WHERE how LIKE '%" NZB_KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM movie_event WHERE coalesce(text, '') LIKE '%" NZB_KEY "%'"
		" OR coalesce(text, '') LIKE '%apikey%'");
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "Usenet");
}

/* ── 4.2 States from real rows ──────────────────────────────────── */

static void
test_states(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 future = add_film(fx, "105");
	gint64 mazework = add_film(fx, "101");
	gint64 reloaded = add_film(fx, "102");
	g_autofree gchar *file_id = NULL;
	gboolean created = FALSE;

	/* Spec: Before release: not available, and not searched for. */
	assert_state(fx, future, AXIS_MOVIE_NOT_AVAILABLE, "2031-08-30");
	g_assert_false(axis_movies_request_search(fx->movies, future, "manual", "root", &created, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_AVAILABLE);
	g_clear_error(&error);
	g_assert_cmpstr(search(fx, future, "scheduled").skipped, ==, "it is not available yet");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, 0);

	assert_state(fx, mazework, AXIS_MOVIE_WANTED, "Not searched for yet");
	/* Searching: its job is queued. */
	g_assert_true(axis_movies_request_search(fx->movies, mazework, "manual", "root", &created, &error));
	g_assert_true(created);
	assert_state(fx, mazework, AXIS_MOVIE_SEARCHING, "queued");
	/* Asked again while queued: the same job. */
	g_assert_true(axis_movies_request_search(fx->movies, mazework, "manual", "root", &created, &error));
	g_assert_false(created);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");

	/* In library: a linked file that is present (only the import pipeline
	 * links files); upgrade wanted below the cutoff. */
	axis_test_db_exec(fx->t->db, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
		" first_seen_generation, last_seen_generation, changed_generation) SELECT id, volume_id,"
		" 'Reloaded (2003)/Reloaded.mkv', 'video', 1000, 1, 0, 1, 0, 1, 1, false, false, false, 1, 1, 1"
		" FROM library_root WHERE media_type = 'movies'");
	file_id = q(fx, "SELECT id FROM library_file");
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality) VALUES (%s, %" G_GINT64_FORMAT ", 'webdl_1080p')",
		file_id, reloaded);
	assert_state(fx, reloaded, AXIS_MOVIE_UPGRADE_WANTED, "below the cutoff of its quality profile");
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET quality = 'bluray_1080p'");
	assert_state(fx, reloaded, AXIS_MOVIE_IN_LIBRARY, NULL);
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET state = 'missing', missing_at = now(), suspected_at = now(),"
		" suspected_scan_id = 1");
	assert_state(fx, reloaded, AXIS_MOVIE_WANTED, NULL);

	/* The destination offline: needs attention, whatever else. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline*");
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	assert_state(fx, mazework, AXIS_MOVIE_ATTENTION, "offline");
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	assert_state(fx, mazework, AXIS_MOVIE_WANTED, NULL);
}

/* ── 4.3 Never grabbed ──────────────────────────────────────────── */

/* Spec: Wrong film; Why not */
static void
test_never_grabbed(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ RELOADED, "usenet", 12000000000, RELOADED_IMDB, NULL },
		{ "The.Mazework.1080p.BluRay.x264-NOYEAR", "usenet", 12000000000, MAZEWORK_IMDB, NULL },
		{ "The.Mazework.1999.1080p.BluRay.x264-BLOCKED", "usenet", 12000000000, MAZEWORK_IMDB, NULL },
		{ "The.Mazework.1999.1080p.BluRay.x264-OTHERID", "usenet", 12000000000, 9999999, NULL }
	};
	g_autoptr(AxisMovieDetail) detail = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = axis_movie_name_key(fx->source_id, 3, "NewsOwl", rels[2].title);
	AxisMovieSearchReport report;
	gint64 movie = add_film(fx, "101");
	guint i;

	exec(fx, "INSERT INTO movie_blocklist (movie_id, key, name_key, release_name, reason) VALUES (%" G_GINT64_FORMAT
		", 'name:%s', '%s', '%s', 'failed')", movie, key, key, rels[2].title);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, movie, "manual");
	g_assert_true(report.searched);
	if (g_strcmp0(report.outcome, "exhausted") != 0) {
		g_autofree gchar *dump = q(fx, "SELECT string_agg(name || ' ' || match || ' ' || accepted || ' ' || blocklisted"
			" || ' ' || reasons::text || ' ' || name_key, E'\\n') FROM movie_candidate");
		g_autofree gchar *keys = q(fx, "SELECT string_agg(key, ',') FROM movie_blocklist");
		g_autofree gchar *searches = q(fx, "SELECT string_agg(lookup || ':' || query || ':' || result_ids::text, ' ; ')"
			" FROM meta_search");
		g_autofree gchar *titles = q(fx, "SELECT string_agg(id || ':' || title || ':' || coalesce(year, 0), ' ; ')"
			" FROM meta_title");

		g_printerr("candidates:\n%s\nblocklist: %s\nsearches: %s\ntitles: %s\n", dump, keys, searches, titles);
	}
	g_assert_cmpstr(report.outcome, ==, "exhausted");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM movie_grab");
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "has failed before");

	/* Every one, with its verdict and reasons, stored and read back. */
	g_assert_true(axis_movies_get_detail(fx->movies, movie, 30, &detail, &error));
	g_assert_cmpuint(detail->candidates->len, ==, 4);
	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);
		const AxisMovieReason *first = c->reasons->len > 0 ? g_ptr_array_index(c->reasons, 0) : NULL;

		g_assert_false(c->accepted);
		g_assert_nonnull(first);
		if (strcmp(c->name, RELOADED) == 0) {
			g_assert_true(c->safety);
			g_assert_cmpstr(first->code, ==, "no_match");
			assert_contains(first->text, "another movie");
		} else if (g_str_has_suffix(c->name, "NOYEAR")) {
			g_assert_true(c->safety);
			g_assert_cmpint(c->match, ==, AXIS_RELEASE_MATCH_AMBIGUOUS);
			g_assert_cmpstr(first->code, ==, "ambiguous");
			assert_contains(first->text, "2 titles fit");
		} else if (g_str_has_suffix(c->name, "BLOCKED")) {
			g_assert_false(c->safety);
			g_assert_true(c->blocklisted);
			g_assert_cmpstr(first->code, ==, "blocklisted");
		} else {
			g_assert_true(c->safety);
			g_assert_cmpstr(first->code, ==, "no_match");
			assert_contains(first->text, "tt9999999");
		}
	}
	assert_contains(detail->search->outcome_text, "on its blocklist");
}

/*
 * Spec (release-parsing): Same-named titles of other years; A same-named
 * title with no year, a year off; A post's subject.  Found on a real search: a film,
 * one other film of its name and one with no year known, the last two only
 * in the store because a title search listed them; every release of the
 * film was ambiguous.  The year picks the film; the search records carry
 * their years into the verdicts; a name without a year stays ambiguous; a
 * post's subject is ruled out as one.
 */
static void
test_same_name(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ "The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", "usenet", 12000000000, 0, NULL },
		{ "The.Quiet.Harbor.1990.1080p.BluRay.x264-OTHER", "usenet", 12000000000, 0, NULL },
		{ "The.Quiet.Harbor.1969.1080p.BluRay.x264-NEAR", "usenet", 12000000000, 0, NULL },
		{ "The.Quiet.Harbor.1080p.BluRay.x264-NOYEAR", "usenet", 12000000000, 0, NULL },
		{ "(101/171) \\\"The Quiet Harbor (1968) Uncut - AVCHD 1080p mixed by Someone.part099.rar\\\" - 7,66 GB",
		  "usenet", 12000000000, 0, NULL }
	};
	g_autoptr(AxisMovieDetail) detail = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *grabbed = NULL;
	AxisMovieSearchReport report;
	gint64 movie;
	guint seen = 0;
	guint i;

	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"201\",\"kind\":\"film\",\"title\":\"The Quiet Harbor\","
		"\"year\":1968},{\"id\":\"202\",\"kind\":\"film\",\"title\":\"The Quiet Harbor\",\"year\":1990},"
		"{\"id\":\"203\",\"kind\":\"film\",\"title\":\"The Quiet Harbor\"}]}");
	set_route(fx, "/title/film/201", "{\"id\":\"201\",\"kind\":\"film\",\"title\":\"The Quiet Harbor\",\"year\":1968,"
		"\"runtime\":110,\"status\":\"released\",\"release_date\":\"1968-05-01\"}");
	movie = add_film(fx, "201");
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, movie, "manual");
	g_assert_true(report.searched);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	grabbed = grabbed_name(fx, movie);
	g_assert_cmpstr(grabbed, ==, rels[0].title);

	g_assert_true(axis_movies_get_detail(fx->movies, movie, 30, &detail, &error));
	g_assert_no_error(error);
	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);
		const AxisMovieReason *first = c->reasons->len > 0 ? g_ptr_array_index(c->reasons, 0) : NULL;

		if (g_str_has_suffix(c->name, "-GRP")) {
			g_assert_cmpint(c->match, ==, AXIS_RELEASE_MATCH_YES);
			seen |= 1;
		} else if (g_str_has_suffix(c->name, "-OTHER")) {
			/* The search record's year is in the verdict. */
			g_assert_nonnull(first);
			g_assert_cmpstr(first->code, ==, "no_match");
			assert_contains(first->text, "another movie");
			assert_contains(first->text, "'The Quiet Harbor' (1990)");
			seen |= 2;
		} else if (g_str_has_suffix(c->name, "-NEAR")) {
			g_assert_nonnull(first);
			g_assert_cmpstr(first->code, ==, "ambiguous");
			assert_contains(first->text, "'The Quiet Harbor' (1968), 'The Quiet Harbor' (no year known)");
			seen |= 4;
		} else if (g_str_has_suffix(c->name, "-NOYEAR")) {
			g_assert_nonnull(first);
			g_assert_cmpstr(first->code, ==, "ambiguous");
			assert_contains(first->text, "3 titles fit: 'The Quiet Harbor' (1968), 'The Quiet Harbor' (1990), "
				"'The Quiet Harbor' (no year known)");
			seen |= 8;
		} else {
			g_assert_nonnull(first);
			g_assert_cmpstr(first->code, ==, "post_subject");
			assert_contains(first->text, "one part of a multi-part upload");
			seen |= 16;
		}
	}
	g_assert_cmpuint(seen, ==, 31);
}

/* A film of a TMDb search's results, as TMDb gives it. */
#define TMDB_RESULT(id, date) "{\"adult\":false,\"id\":" id ",\"original_language\":\"en\",\"original_title\":" \
	"\"The Quiet Harbor\",\"overview\":\"A made-up film.\",\"release_date\":\"" date "\",\"title\":\"The Quiet Harbor\"}"

/*
 * The same, with the records made as the daemon makes them, by the real
 * TMDb provider against a fake TMDb on loopback (answers written by hand,
 * made-up films): a title search on the Add page keeps every film of the
 * name (never opened, no detail, the year from TMDb's date), the film is
 * added by its record, and the search job reads the search kept.  The years
 * of the records the search made are in the verdicts, and a film of the
 * name that TMDb gives no date (no year known) does not make the film's own
 * release ambiguous.  (With the matcher before the same-name rule, that one
 * yearless record made every release of the film ambiguous: "2 titles fit:
 * 'The Quiet Harbor' (1968), 'The Quiet Harbor'".)
 */
static void
test_same_name_searched(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ "The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", "usenet", 12000000000, 0, NULL },
		{ "The.Quiet.Harbor.1990.1080p.BluRay.x264-OTHER", "usenet", 12000000000, 0, NULL }
	};
	g_autoptr(AxisMovieDetail) detail = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *grabbed = NULL;
	AxisMetaSearchAnswer found;
	AxisMovieSearchReport report;
	AxisMovieSpec spec;
	gint64 film = 0;
	gint64 movie = 0;
	guint seen = 0;
	guint i;

	/* The last has no date at TMDb (TMDb gives an empty one): no year. */
	static const gchar search_json[] = "{\"page\":1,\"results\":[" TMDB_RESULT("7001", "1968-05-01") ","
		TMDB_RESULT("7002", "1990-03-09") "," TMDB_RESULT("7003", "2026-02-13") "," TMDB_RESULT("7004", "")
		"],\"total_pages\":1,\"total_results\":4}";
	static const gchar film_json[] = "{\"adult\":false,\"id\":7001,\"imdb_id\":\"tt9900001\",\"original_language\":"
		"\"en\",\"original_title\":\"The Quiet Harbor\",\"overview\":\"A made-up film.\",\"release_date\":"
		"\"1968-05-01\",\"runtime\":110,\"status\":\"Released\",\"title\":\"The Quiet Harbor\",\"external_ids\":"
		"{\"imdb_id\":\"tt9900001\"},\"release_dates\":{\"results\":[{\"iso_3166_1\":\"US\",\"release_dates\":"
		"[{\"certification\":\"\",\"release_date\":\"1968-05-01T00:00:00.000Z\",\"type\":3}]}]}}";
	g_autoptr(GBytes) search_body = g_bytes_new_static(search_json, strlen(search_json));
	g_autoptr(GBytes) film_body = g_bytes_new_static(film_json, strlen(film_json));

	g_assert_nonnull(fx->tmdb);
	axis_test_plex_route_full(fx->tmdb, "/3/search/movie", 200, "application/json", search_body, NULL);
	axis_test_plex_route_full(fx->tmdb, "/3/movie/7001", 200, "application/json", film_body, NULL);

	/* The Add page's search. */
	memset(&found, 0, sizeof(found));
	g_assert_true(axis_meta_search(fx->meta, "tmdb", AXIS_META_KIND_FILM, "the quiet harbor", 0, 5000, &found,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(found.titles->len, ==, 4);
	for (i = 0; i < found.titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(found.titles, i);

		if (title->year == 1968)
			film = title->id;
	}
	axis_meta_search_answer_clear(&found);
	g_assert_cmpint(film, >, 0);
	assert_q(fx, "1968 1990 2026 -", "SELECT string_agg(coalesce(year::text, '-'), ' ' ORDER BY provider_id)"
		" FROM meta_title WHERE title = 'The Quiet Harbor' AND detail_fetched_at IS NULL AND viewed_at IS NULL");

	/* Added by its record. */
	memset(&spec, 0, sizeof(spec));
	spec.title_id = film;
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &movie, &error));
	g_assert_no_error(error);

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	report = search(fx, movie, "scheduled");
	g_assert_true(report.searched);
	/* The job opened the film's record only. */
	assert_q(fx, "7002 7003 7004", "SELECT string_agg(provider_id, ' ' ORDER BY provider_id) FROM meta_title"
		" WHERE title = 'The Quiet Harbor' AND detail_fetched_at IS NULL");
	g_assert_true(axis_movies_get_detail(fx->movies, movie, 30, &detail, &error));
	g_assert_no_error(error);
	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);
		const AxisMovieReason *first = c->reasons->len > 0 ? g_ptr_array_index(c->reasons, 0) : NULL;

		g_test_message("%s: %s", c->name, first != NULL ? first->text : c->match_text);
	}
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	grabbed = grabbed_name(fx, movie);
	g_assert_cmpstr(grabbed, ==, rels[0].title);

	for (i = 0; i < detail->candidates->len; i++) {
		const AxisMovieCandidate *c = g_ptr_array_index(detail->candidates, i);
		const AxisMovieReason *first = c->reasons->len > 0 ? g_ptr_array_index(c->reasons, 0) : NULL;

		if (g_str_has_suffix(c->name, "-GRP")) {
			/* The film, though a film of its name has no year known. */
			g_assert_cmpint(c->match, ==, AXIS_RELEASE_MATCH_YES);
			assert_contains(c->match_text, "1 other title of that name has no year known");
			seen |= 1;
		} else {
			/* The year the search gave the other film's record. */
			g_assert_nonnull(first);
			g_assert_cmpstr(first->code, ==, "no_match");
			assert_contains(first->text, "another movie: it matches 'The Quiet Harbor' (1990), the same year");
			seen |= 2;
		}
	}
	g_assert_cmpuint(seen, ==, 3);
}

/* ── 4.4 Grabbed once ───────────────────────────────────────────── */

typedef struct
{
	GMutex lock;
	GCond  cond;
	guint  arrived;
} Barrier;

/* Both searches wait for each other just before the claim, so that the two
 * claim transactions really race. */
static void
claim_barrier(
	gint64   movie_id,
	gpointer data
){
	Barrier *barrier = data;
	gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	g_mutex_lock(&barrier->lock);
	barrier->arrived++;
	g_cond_broadcast(&barrier->cond);
	while (barrier->arrived < 2 && g_cond_wait_until(&barrier->cond, &barrier->lock, end))
		;
	g_mutex_unlock(&barrier->lock);
}

typedef struct
{
	Fixture               *fx;
	gint64                 movie;
	AxisMovieSearchReport  report;
	gboolean               ok;
} Racer;

static gpointer
race(gpointer data)
{
	Racer *racer = data;
	g_autoptr(GError) error = NULL;

	racer->ok = axis_movies_run_search(racer->fx->movies, racer->movie, "manual", "root", NULL, &racer->report, &error);
	if (error != NULL)
		g_printerr("race: %s\n", error->message);
	return NULL;
}

/* Spec: Searched twice at once */
static void
test_grab_once(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *movie_text = NULL;
	g_autoptr(GError) error = NULL;
	Barrier barrier;
	Racer a;
	Racer b;
	GThread *ta;
	GThread *tb;
	gint64 movie = add_film(fx, "101");
	const gchar *params[1];

	set_releases(fx, standard, G_N_ELEMENTS(standard));
	memset(&barrier, 0, sizeof(barrier));
	g_mutex_init(&barrier.lock);
	g_cond_init(&barrier.cond);
	axis_movies_set_claim_hook(fx->movies, claim_barrier, &barrier);
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	a.fx = b.fx = fx;
	a.movie = b.movie = movie;
	ta = g_thread_new("racer-a", race, &a);
	tb = g_thread_new("racer-b", race, &b);
	g_thread_join(ta);
	g_thread_join(tb);
	axis_movies_set_claim_hook(fx->movies, NULL, NULL);
	g_assert_true(a.ok);
	g_assert_true(b.ok);
	g_assert_cmpuint(barrier.arrived, ==, 2);
	/* One grab, one download, one release at the client. */
	assert_q(fx, "1", "SELECT count(*) FROM movie_grab");
	assert_q(fx, "1", "SELECT count(*) FROM download");
	g_assert_cmpuint(n_adds(fx), ==, 1);
	g_assert_true(g_strcmp0(a.report.outcome, "grabbed") == 0 || g_strcmp0(b.report.outcome, "grabbed") == 0);
	g_assert_true(g_strcmp0(a.report.outcome, "in_flight") == 0 || g_strcmp0(b.report.outcome, "in_flight") == 0);
	g_mutex_clear(&barrier.lock);
	g_cond_clear(&barrier.cond);

	/* A third, later: nothing (a grab is in flight), and no indexer asked. */
	{
		guint searches = axis_test_prowlarr_n_searches(fx->prowlarr);

		g_assert_cmpstr(search(fx, movie, "manual").skipped, ==, "a grab of it is in flight");
		g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, searches);
	}
	/* The database's own guard: a second open grab is refused. */
	movie_text = g_strdup_printf("%" G_GINT64_FORMAT, movie);
	params[0] = movie_text;
	g_assert_false(axis_db_pool_exec(fx->t->pool, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key)"
		" VALUES ($1::bigint, 'x', 'usenet', 'x')", 1, params, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION);
}

/* ── 4.5 Failure takes the next best ────────────────────────────── */

static void
wait_for_grab(
	Fixture     *fx,
	gint64       movie,
	guint        n
){
	g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT
		" AND download_id IS NOT NULL", movie);
	g_autofree gchar *want = g_strdup_printf("%u", n);

	g_assert_true(axis_test_wait_query(fx->t->db, sql, want, 20000));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND state IN"
		" ('queued', 'running')", "0", 20000));
}

/* Spec: Failure; None left */
static void
test_failure(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ WEBDL, "usenet", 6000000000, MAZEWORK_IMDB, NULL },
		{ BLURAY, "usenet", 12000000000, MAZEWORK_IMDB, NULL },
		{ WEBRIP, "usenet", 4000000000, MAZEWORK_IMDB, NULL }
	};
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autofree gchar *ref = NULL;
	AxisMovieFollowReport report;
	gint64 movie;
	guint i;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	start_queue(fx);
	movie = add_film_full(fx, "101", AXIS_MOVIE_RELEASED, TRUE);
	wait_for_grab(fx, movie, 1);
	first = grabbed_name(fx, movie);
	g_assert_cmpstr(first, ==, BLURAY);

	/* It fails at the client: blocklisted, removed with its data, the next. */
	ref = client_ref_of(fx, movie);
	axis_test_download_fake_set(fx->fake, ref, "failed", 100, NULL, "Out of disk space");
	poll(fx);
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "blocklist");
	report = follow(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(report.removed, ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	assert_q(fx, "failed", "SELECT reason FROM movie_blocklist");
	assert_q(fx, BLURAY, "SELECT release_name FROM movie_blocklist");
	/* Once: a second run changes nothing. */
	report = follow(fx);
	g_assert_cmpuint(report.failed, ==, 0);
	wait_for_grab(fx, movie, 2);
	second = grabbed_name(fx, movie);
	g_assert_nonnull(second);
	g_assert_cmpstr(second, !=, BLURAY);
	assert_q(fx, "retry", "SELECT cause FROM movie_search ORDER BY id DESC LIMIT 1");

	/* It stalls beyond the limit: the same. */
	axis_test_db_exec(fx->t->db, "UPDATE download SET stalled = true, progress_at = now() - interval '7 hours'"
		" WHERE state IN ('queued', 'downloading')");
	g_clear_pointer(&ref, g_free);
	ref = client_ref_of(fx, movie);
	report = follow(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	assert_q(fx, "stalled", "SELECT reason FROM movie_blocklist ORDER BY id DESC LIMIT 1");
	wait_for_grab(fx, movie, 3);
	third = grabbed_name(fx, movie);
	g_assert_nonnull(third);
	g_assert_cmpstr(third, !=, first);
	g_assert_cmpstr(third, !=, second);

	/* It disappears from the client: gone, the same (nothing to remove). */
	g_clear_pointer(&ref, g_free);
	ref = client_ref_of(fx, movie);
	axis_test_download_fake_drop(fx->fake, ref);
	for (i = 0; i < 3; i++)
		poll(fx);
	assert_q(fx, "gone", "SELECT d.state FROM download d JOIN movie_grab g ON g.download_id = d.id"
		" WHERE g.closed_at IS NULL");
	report = follow(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	assert_q(fx, "gone", "SELECT reason FROM movie_blocklist ORDER BY id DESC LIMIT 1");
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT outcome FROM movie_search ORDER BY id DESC LIMIT 1",
		"exhausted", 20000));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND state IN"
		" ('queued', 'running')", "0", 20000));

	/* None left: it needs attention, and says why. */
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "Every acceptable release has failed before: 3 are on its blocklist");
	g_assert_cmpuint(n_adds(fx), ==, 3);
	assert_q(fx, "3", "SELECT count(*) FROM movie_grab WHERE closed_at IS NOT NULL");
}

/* ── 4.6 The scheduler ──────────────────────────────────────────── */

static AxisMovieScheduleReport
schedule(Fixture *fx)
{
	AxisMovieScheduleReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_movies_run_schedule(fx->movies, &report, &error));
	g_assert_no_error(error);
	return report;
}

static gchar *
queued_films(Fixture *fx)
{
	return q(fx, "SELECT string_agg((payload->>'movie'), ',' ORDER BY (payload->>'movie')::bigint) FROM job"
		" WHERE kind = 'movies.search' AND state = 'queued'");
}

/* Spec: Pacing */
static void
test_scheduler(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *queued = NULL;
	g_autofree gchar *expected = NULL;
	AxisMovieScheduleReport report;
	gint64 mazework;
	gint64 reloaded;
	gint64 remake;
	gint64 future;

	g_assert_true(axis_settings_set(fx->settings, AXIS_MOVIES_SETTING_PER_RUN, "2", &error));
	set_releases(fx, NULL, 0);
	mazework = add_film(fx, "101");
	reloaded = add_film(fx, "102");
	remake = add_film(fx, "106");
	future = add_film(fx, "105");

	/* At most two a run, never searched first, the earliest added first; the
	 * film to come is not due. */
	report = schedule(fx);
	g_assert_cmpuint(report.considered, ==, 4);
	g_assert_cmpuint(report.due, ==, 3);
	g_assert_cmpuint(report.queued, ==, 2);
	queued = queued_films(fx);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT, mazework, reloaded);
	g_assert_cmpstr(queued, ==, expected);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	g_assert_cmpstr(search(fx, future, "scheduled").skipped, ==, "it is not available yet");

	/* Nothing found: the floor (6 h) before the next, and it doubles. */
	g_assert_cmpstr(search(fx, mazework, "scheduled").outcome, ==, "nothing_found");
	assert_qf(fx, "43200", "SELECT search_floor_s FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);
	assert_qf(fx, "t", "SELECT next_search_at BETWEEN now() + interval '5 hours 59 minutes' AND"
		" now() + interval '6 hours 1 minute' FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);
	g_assert_cmpstr(search(fx, mazework, "scheduled").outcome, ==, "nothing_found");
	assert_qf(fx, "86400", "SELECT search_floor_s FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);
	assert_qf(fx, "t", "SELECT next_search_at BETWEEN now() + interval '11 hours 59 minutes' AND"
		" now() + interval '12 hours 1 minute' FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);
	/* It stops at seven days. */
	exec(fx, "UPDATE movie SET search_floor_s = 500000 WHERE id = %" G_GINT64_FORMAT, mazework);
	g_assert_cmpstr(search(fx, mazework, "scheduled").outcome, ==, "nothing_found");
	assert_qf(fx, "604800", "SELECT search_floor_s FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);

	/* Not due before its next time: the scheduler leaves it. */
	report = schedule(fx);
	assert_qf(fx, "0", "SELECT count(*) FROM job WHERE kind = 'movies.search' AND (payload->>'movie')::bigint = %"
		G_GINT64_FORMAT, mazework);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");

	/* Its availability changes (the release date moved): the floor starts
	 * over and it is due at once. */
	exec(fx, "UPDATE movie SET seen_release_date = '1998-01-01' WHERE id = %" G_GINT64_FORMAT, mazework);
	exec(fx, "UPDATE movie SET next_search_at = now() + interval '1 day' WHERE id <> %" G_GINT64_FORMAT, mazework);
	report = schedule(fx);
	g_assert_cmpuint(report.became_available, ==, 1);
	assert_qf(fx, "21600", "SELECT search_floor_s FROM movie WHERE id = %" G_GINT64_FORMAT, mazework);
	g_clear_pointer(&queued, g_free);
	g_clear_pointer(&expected, g_free);
	queued = queued_films(fx);
	expected = g_strdup_printf("%" G_GINT64_FORMAT, mazework);
	g_assert_cmpstr(queued, ==, expected);
	assert_q(fx, "available", "SELECT kind FROM movie_event ORDER BY id DESC LIMIT 1");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");

	/* Nothing is searched while a grab is in flight. */
	axis_test_db_exec(fx->t->db, "UPDATE movie SET next_search_at = NULL");
	exec(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key) VALUES (%" G_GINT64_FORMAT ", 'x',"
		" 'usenet', 'x'), (%" G_GINT64_FORMAT ", 'y', 'usenet', 'y')", mazework, reloaded);
	report = schedule(fx);
	g_clear_pointer(&queued, g_free);
	g_clear_pointer(&expected, g_free);
	queued = queued_films(fx);
	expected = g_strdup_printf("%" G_GINT64_FORMAT, remake);
	g_assert_cmpstr(queued, ==, expected);
	g_assert_cmpstr(search(fx, mazework, "scheduled").skipped, ==, "a grab of it is in flight");
}

/* ── 4.7 Nothing usable: attention, nothing sent ────────────────── */

static void
test_unusable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 movie = add_film(fx, "101");
	guint searches;

	set_releases(fx, standard, G_N_ELEMENTS(standard));

	/* No client switched on: not even the indexers are asked. */
	save_client(fx, fx->client_id, "Usenet", "fixturenzb", FALSE);
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "no_client");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, 0);
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "No download client");

	/* A client for torrents only, and Usenet releases: searched (by the
	 * identifier, and the one indexer that takes none by the title), not
	 * sent. */
	save_client(fx, 0, "Torrents", "fixture", TRUE);
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "no_client");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, 2);
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "no usable download client takes them");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	save_client(fx, fx->client_id, "Usenet", "fixturenzb", TRUE);

	/* No indexer. */
	g_assert_true(axis_indexers_delete_source(fx->indexers, fx->source_id, &error));
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "no_indexer");
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "No indexer is set up");
	fx->source_id = add_source(fx);

	/* The destination offline: nothing searched, nothing sent. */
	searches = axis_test_prowlarr_n_searches(fx->prowlarr);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline*");
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "root_offline");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, searches);
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "offline");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM movie_grab");

	/* Back: grabbed. */
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "grabbed");
	g_assert_cmpuint(n_adds(fx), ==, 1);
}

/* ── 4.8 By hand ────────────────────────────────────────────────── */

static gint64
candidate_id(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *id = q(fx, "SELECT max(id) FROM movie_candidate WHERE name = '%s'", name);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static void
test_manual_grab(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ RELOADED, "usenet", 12000000000, RELOADED_IMDB, NULL },
		{ UHD, "usenet", 60000000000, MAZEWORK_IMDB, NULL }
	};
	g_autoptr(GError) error = NULL;
	gint64 movie = add_film(fx, "101");
	gint64 grab = 0;

	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "none_acceptable");
	assert_state(fx, movie, AXIS_MOVIE_ATTENTION, "rejected them all");

	/* A fixed rule's rejection: no one may grab it. */
	g_assert_false(axis_movies_grab(fx->movies, movie, candidate_id(fx, RELOADED), "root", &grab, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_REFUSED);
	assert_contains(error->message, "another movie");
	g_clear_error(&error);
	g_assert_cmpuint(n_adds(fx), ==, 0);

	/* The profile's rejection: an administrator may. */
	g_assert_true(axis_movies_grab(fx->movies, movie, candidate_id(fx, UHD), "root", &grab, &error));
	g_assert_no_error(error);
	g_assert_cmpint(grab, >, 0);
	g_assert_cmpuint(n_adds(fx), ==, 1);
	assert_q(fx, "true|root", "SELECT manual || '|' || grabbed_by FROM movie_grab");
	/* (Its import sets the profile aside: the person did.) */
	assert_q(fx, "anyway", "SELECT set_aside FROM movie_grab");
	assert_q(fx, "root", "SELECT by_user FROM movie_event WHERE kind = 'grabbed'");
	assert_q_has(fx, "Grabbed by hand", "SELECT text FROM movie_event WHERE kind = 'grabbed'");
	assert_q_has(fx, "although the profile rejected it", "SELECT text FROM movie_event WHERE kind = 'grabbed'");
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, NULL);

	/* Once in flight, nothing else. */
	g_assert_false(axis_movies_grab(fx->movies, movie, candidate_id(fx, UHD), "root", &grab, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT);
	g_clear_error(&error);

	/* Given up on: blocklisted, and then refused by hand too. */
	g_assert_true(axis_movies_give_up(fx->movies, movie, "root", &error));
	g_assert_no_error(error);
	assert_q(fx, "given_up|root", "SELECT reason || '|' || created_by FROM movie_blocklist");
	g_assert_false(axis_movies_grab(fx->movies, movie, candidate_id(fx, UHD), "root", &grab, &error));
	g_assert_error(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_BLOCKLISTED);
	g_clear_error(&error);
	/* Off the blocklist again: allowed. */
	g_assert_true(axis_movies_clear_blocklist(fx->movies, movie, 0, "root", NULL, &error));
	g_assert_true(axis_movies_grab(fx->movies, movie, candidate_id(fx, UHD), "root", &grab, &error));
	g_assert_no_error(error);
}

/* ── 4.9 Every action in the history, with the user ─────────────── */

static void
test_actions_logged(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMovieSpec spec;
	gint64 movie = add_film(fx, "101");
	gint64 title = title_of(fx, "101");
	gboolean created = FALSE;

	set_releases(fx, standard, G_N_ELEMENTS(standard));
	g_assert_true(axis_movies_request_search(fx->movies, movie, "manual", "alice", &created, &error));
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	memset(&spec, 0, sizeof(spec));
	spec.root_id = fx->root_id;
	spec.monitored = FALSE;
	spec.availability = AXIS_MOVIE_ANNOUNCED;
	spec.user = "alice";
	g_assert_true(axis_movies_edit(fx->movies, movie, &spec, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "grabbed");
	g_assert_true(axis_movies_give_up(fx->movies, movie, "alice", &error));
	g_assert_true(axis_movies_clear_blocklist(fx->movies, movie, 0, "alice", NULL, &error));
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	g_assert_true(axis_movies_remove(fx->movies, movie, "alice", &error));
	g_assert_no_error(error);
	assert_q(fx, "0", "SELECT count(*) FROM movie");
	/* The history outlives the film, with who did what. */
	assert_q(fx, "added:root,searched:alice,changed:alice,searched:root,grabbed:root,abandoned:alice,unblocked:alice,"
		"removed:alice", "SELECT string_agg(kind || ':' || coalesce(by_user, '-'), ',' ORDER BY id) FROM movie_event"
		" WHERE kind <> 'removal'");
	assert_qf(fx, "8", "SELECT count(*) FROM movie_event WHERE title_id = %" G_GINT64_FORMAT " AND movie_id IS NULL",
		title);
	assert_q_has(fx, "not monitored", "SELECT text FROM movie_event WHERE kind = 'changed'");
	assert_q_has(fx, "No file was touched", "SELECT text FROM movie_event WHERE kind = 'removed'");
}

/* ── An uncertain send ──────────────────────────────────────────── */

static void
test_uncertain(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "101");
	guint searches;

	set_releases(fx, standard, G_N_ELEMENTS(standard));
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_ADD_HANGS);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	g_assert_cmpstr(search(fx, movie, "manual").outcome, ==, "grabbed");
	assert_q(fx, "sending", "SELECT state FROM download");
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "waiting for the client to confirm");
	assert_q_has(fx, "not known yet", "SELECT text FROM movie_event WHERE kind = 'grabbed'");
	/* Never searched again while it is unsettled. */
	searches = axis_test_prowlarr_n_searches(fx->prowlarr);
	g_assert_cmpstr(search(fx, movie, "scheduled").skipped, ==, "a grab of it is in flight");
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, searches);
	/* The downloads layer settles it: the client has it. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	axis_test_db_exec(fx->t->db, "UPDATE download SET created_at = now() - interval '1 hour' WHERE state = 'sending'");
	poll(fx);
	assert_q(fx, "queued", "SELECT state FROM download");
	assert_state(fx, movie, AXIS_MOVIE_DOWNLOADING, "Queued at");
	g_assert_cmpuint(n_adds(fx), ==, 1);
}

/* ── Old searches go ────────────────────────────────────────────── */

static void
test_prune(Fixture *fx, gconstpointer data)
{
	gint64 movie = add_film(fx, "101");
	gint64 other = add_film(fx, "102");
	guint i;

	for (i = 0; i < 8; i++)
		exec(fx, "INSERT INTO movie_search (movie_id, cause, started_at, finished_at, outcome) VALUES (%"
			G_GINT64_FORMAT ", 'scheduled', now() - interval '10 days', now() - interval '10 days', 'nothing_found')",
			movie);
	exec(fx, "INSERT INTO movie_search (movie_id, cause, finished_at, outcome) VALUES (%" G_GINT64_FORMAT
		", 'manual', now(), 'nothing_found')", other);
	exec(fx, "INSERT INTO movie_candidate (search_id, position, release_id, source_id, name, protocol, name_key,"
		" quality, match, accepted, safety) SELECT id, 0, 1, 1, 'x', 'usenet', 'x', 'unknown', 'none', false, false"
		" FROM movie_search");
	follow(fx);
	/* The latest five of a film stay; nothing of a week is touched. */
	assert_qf(fx, "5", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, other);
	assert_q(fx, "6", "SELECT count(*) FROM movie_candidate");
}

/* Not at every follow-up, and not all at once: once an hour (and at the
 * first follow-up after a start), a bounded number of searches a time. */
static void
test_prune_paced(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 movie = add_film(fx, "101");
	guint all = AXIS_ACQUIRE_PRUNE_BATCH * AXIS_ACQUIRE_PRUNE_BATCHES;
	guint deleted = 99;

	/* The first follow-up after a start prunes; there is nothing yet. */
	follow(fx);
	exec(fx, "INSERT INTO movie_search (movie_id, cause, started_at, finished_at, outcome) SELECT %" G_GINT64_FORMAT
		", 'scheduled', now() - interval '10 days', now() - interval '10 days', 'nothing_found'"
		" FROM generate_series(1, 8)", movie);
	/* The next ones, within the hour, do not. */
	follow(fx);
	follow(fx);
	assert_qf(fx, "8", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
	/* At its time: the latest five stay. */
	g_assert_true(axis_movies_prune_searches(fx->movies, &deleted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(deleted, ==, 3);
	assert_qf(fx, "5", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
	g_assert_true(axis_movies_prune_searches(fx->movies, &deleted, &error));
	g_assert_cmpuint(deleted, ==, 0);

	/* More old searches than one time takes (a long stop): its batches go,
	 * the rest waits for the next time, and the latest five stay. */
	exec(fx, "INSERT INTO movie_search (movie_id, cause, started_at, finished_at, outcome) SELECT %" G_GINT64_FORMAT
		", 'scheduled', now() - interval '10 days', now() - interval '10 days', 'nothing_found'"
		" FROM generate_series(1, %u)", movie, all + 300);
	exec(fx, "INSERT INTO movie_candidate (search_id, position, release_id, source_id, name, protocol, name_key,"
		" quality, match, accepted, safety) SELECT id, 0, 1, 1, 'x', 'usenet', 'x', 'unknown', 'none', false, false"
		" FROM movie_search");
	g_assert_true(axis_movies_prune_searches(fx->movies, &deleted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(deleted, ==, all);
	assert_qf(fx, "305", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_q(fx, "305", "SELECT count(*) FROM movie_candidate");
	g_assert_true(axis_movies_prune_searches(fx->movies, &deleted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(deleted, ==, 300);
	assert_qf(fx, "5", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
	/* The five that stay are the latest. */
	assert_qf(fx, "5", "SELECT count(*) FROM movie_search WHERE id > (SELECT max(id) - 5 FROM movie_search)"
		" AND movie_id = %" G_GINT64_FORMAT, movie);
	assert_q(fx, "5", "SELECT count(*) FROM movie_candidate");
}

/* The hour is spent only by a prune that worked: after a failure the next
 * follow-up tries again, not in an hour. */
static void
test_prune_failed_is_tried_again(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMovieFollowReport report;
	gint64 movie = add_film(fx, "101");

	exec(fx, "INSERT INTO movie_search (movie_id, cause, started_at, finished_at, outcome) SELECT %" G_GINT64_FORMAT
		", 'scheduled', now() - interval '10 days', now() - interval '10 days', 'nothing_found'"
		" FROM generate_series(1, 8)", movie);
	exec(fx, "CREATE FUNCTION made_up_refuse() RETURNS trigger LANGUAGE plpgsql AS"
		" $$ BEGIN RAISE EXCEPTION 'made-up refusal'; END $$");
	exec(fx, "CREATE TRIGGER made_up_refuse BEFORE DELETE ON movie_search FOR EACH ROW EXECUTE FUNCTION"
		" made_up_refuse()");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*made-up refusal*");
	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*made-up refusal*");

	/* The first follow-up after a start prunes, and fails. */
	g_assert_false(axis_movies_run_follow(fx->movies, &report, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	assert_qf(fx, "8", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);

	/* The next one, at once, tries again and goes through. */
	exec(fx, "DROP TRIGGER made_up_refuse ON movie_search");
	follow(fx);
	assert_qf(fx, "5", "SELECT count(*) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT, movie);
}

/* ── A title merge ──────────────────────────────────────────────── */

static void
test_title_merge(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *keep = NULL;
	g_autofree gchar *other = NULL;
	gint64 movie;

	axis_test_db_exec(fx->t->db, "INSERT INTO meta_title (kind, title, year, provider, provider_id) VALUES"
		" ('film', 'Kept', 2000, 'fixture', '500'), ('film', 'Merged', 2000, 'fixtureb', '501')");
	keep = q(fx, "SELECT id FROM meta_title WHERE provider_id = '500'");
	other = q(fx, "SELECT id FROM meta_title WHERE provider_id = '501'");
	exec(fx, "INSERT INTO meta_external_id (title_id, kind, scheme, value) VALUES (%s, 'film', 'fixture', '500'),"
		" (%s, 'film', 'fixtureb', '501'), (%s, 'film', 'imdb', 'tt0000501')", keep, other, other);
	{
		AxisMovieSpec spec;
		g_autoptr(GError) error = NULL;

		memset(&spec, 0, sizeof(spec));
		spec.title_id = g_ascii_strtoll(other, NULL, 10);
		spec.root_id = fx->root_id;
		spec.monitored = TRUE;
		spec.availability = AXIS_MOVIE_RELEASED;
		g_assert_true(axis_movies_add(fx->movies, &spec, &movie, &error));
	}
	/* An identifier moving between two titles that both stay moves no film. */
	exec(fx, "UPDATE meta_external_id SET title_id = %s WHERE scheme = 'imdb'", keep);
	assert_q(fx, other, "SELECT title_id::text FROM movie");
	/* A merge (meta/axis-meta-store.c's statements): the film follows. */
	exec(fx, "UPDATE meta_external_id SET title_id = %s WHERE title_id = %s", keep, other);
	exec(fx, "DELETE FROM meta_title WHERE id = %s", other);
	assert_q(fx, keep, "SELECT title_id::text FROM movie");
}

/* ── A film with a file in the library ──────────────────────────── */

/* A file of the films root, linked to @movie with @quality and @resolution
 * (either nullable), as an import or identification leaves it. */
static void
file_on_disk(
	Fixture     *fx,
	gint64       movie,
	const gchar *rel,
	const gchar *quality,
	const gchar *resolution
){
	g_autofree gchar *path = g_build_filename(fx->disk, "films", rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	AxisScanResult *result;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, "film", -1, NULL));
	result = axis_test_library_scan(fx->t, fx->root_id);
	axis_scan_result_free(result);
	exec(fx, "DELETE FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	exec(fx, "INSERT INTO movie_file (file_id, movie_id, quality, resolution) SELECT id, %" G_GINT64_FORMAT ", %s, %s"
		" FROM library_file WHERE rel_path = '%s'", movie, quality != NULL ? quality : "NULL",
		resolution != NULL ? resolution : "NULL", rel);
}

/* How many releases of the film's latest search have a reason @code. */
static gchar *
with_reason(
	Fixture     *fx,
	gint64       movie,
	const gchar *code
){
	return q(fx, "SELECT count(*) FROM movie_candidate c WHERE c.search_id = (SELECT max(id) FROM movie_search WHERE"
		" movie_id = %" G_GINT64_FORMAT ") AND EXISTS (SELECT 1 FROM jsonb_array_elements(c.reasons) r WHERE"
		" r->>'c' = '%s')", movie, code);
}

static gchar *
reason_text(
	Fixture     *fx,
	gint64       movie,
	const gchar *code
){
	return q(fx, "SELECT r->>'t' FROM movie_candidate c, jsonb_array_elements(c.reasons) r WHERE c.search_id ="
		" (SELECT max(id) FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT ") AND r->>'c' = '%s' LIMIT 1", movie,
		code);
}

#define HDTV "The.Mazework.1999.1080p.HDTV.x264-TV"

/* Spec: a search of a film in the library judges against its file. */
static void
test_on_disk(Fixture *fx, gconstpointer data)
{
	static const Rel mazework[] = {
		{ WEBDL, "usenet", 6000000000, MAZEWORK_IMDB, NULL },
		{ BLURAY, "usenet", 12000000000, MAZEWORK_IMDB, NULL },
		{ HDTV, "usenet", 5000000000, MAZEWORK_IMDB, NULL }
	};
	static const Rel reloaded[] = {
		{ RELOADED, "usenet", 12000000000, RELOADED_IMDB, NULL }
	};
	g_autofree gchar *count = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *definition = NULL;
	g_autofree gchar *sql = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisQualityProfile flagged;
	gint64 movie = add_film(fx, "101");
	gint64 other = add_film(fx, "102");

	set_releases(fx, mazework, G_N_ELEMENTS(mazework));

	/* At the cutoff (Balanced's: BluRay 1080p): nothing is grabbed, and every
	 * release says why. */
	file_on_disk(fx, movie, "The Mazework (1999)/The.Mazework.1999.1080p.BluRay.mkv", "'bluray_1080p'", NULL);
	assert_state(fx, movie, AXIS_MOVIE_IN_LIBRARY, NULL);
	report = search(fx, movie, "manual");
	g_assert_cmpint(report.grab_id, ==, 0);
	g_assert_cmpstr(report.outcome, ==, "none_acceptable");
	g_assert_cmpuint(n_adds(fx), ==, 0);
	count = with_reason(fx, movie, "cutoff");
	g_assert_cmpstr(count, ==, "3");
	text = q(fx, "SELECT outcome_text FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT " ORDER BY id DESC LIMIT 1",
		movie);
	assert_contains(text, "none is wanted over the file in the library, BluRay 1080p");
	assert_q(fx, "0", "SELECT count(*) FROM movie_candidate WHERE accepted");
	assert_state(fx, movie, AXIS_MOVIE_IN_LIBRARY, NULL);

	/* An unknown source at 1080p, and the profile does not upgrade one. */
	file_on_disk(fx, movie, "The Mazework (1999)/The Mazework.mkv", NULL, "'1080p'");
	report = search(fx, movie, "manual");
	g_assert_cmpint(report.grab_id, ==, 0);
	g_assert_cmpuint(n_adds(fx), ==, 0);
	g_free(count);
	count = with_reason(fx, movie, "cutoff");
	g_assert_cmpstr(count, ==, "3");
	g_free(text);
	text = reason_text(fx, movie, "cutoff");
	assert_contains(text, "does not upgrade a file of unknown source");

	/* Below the cutoff (HDTV 1080p): only a better release; one of the same
	 * quality is "not better". */
	file_on_disk(fx, movie, "The Mazework (1999)/The.Mazework.1999.1080p.HDTV.mkv", "'hdtv_1080p'", NULL);
	assert_state(fx, movie, AXIS_MOVIE_UPGRADE_WANTED, NULL);
	report = search(fx, movie, "manual");
	g_assert_cmpint(report.grab_id, >, 0);
	{
		g_autofree gchar *grabbed = grabbed_name(fx, movie);

		g_assert_cmpstr(grabbed, ==, BLURAY);
	}
	g_free(count);
	count = with_reason(fx, movie, "not_better");
	g_assert_cmpstr(count, ==, "1");
	g_free(text);
	text = reason_text(fx, movie, "not_better");
	assert_contains(text, "not better than the file on disk (the same quality");

	/* An unknown source with a profile that upgrades one: a stated source of
	 * its resolution is grabbed. */
	g_assert_true(axis_quality_store_copy(fx->quality, "balanced", "Upgrade unknown", "root", &slug, &error));
	g_assert_no_error(error);
	g_assert_true(axis_quality_builtin_profile("balanced", &flagged));
	flagged.upgrade_unknown = TRUE;
	definition = axis_quality_profile_canonical(&flagged);
	sql = g_strdup_printf("UPDATE quality_profile SET definition = '%s' WHERE slug = '%s'", definition, slug);
	axis_test_db_exec(fx->t->db, sql);
	exec(fx, "UPDATE movie SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug = '%s') WHERE id = %"
		G_GINT64_FORMAT, slug, other);
	file_on_disk(fx, other, "The Mazework Reloaded (2003)/Reloaded.mkv", NULL, "'1080p'");
	assert_state(fx, other, AXIS_MOVIE_UPGRADE_WANTED, "upgrades a file of unknown source");
	set_releases(fx, reloaded, G_N_ELEMENTS(reloaded));
	report = search(fx, other, "manual");
	g_assert_cmpint(report.grab_id, >, 0);
	{
		g_autofree gchar *grabbed = grabbed_name(fx, other);

		g_assert_cmpstr(grabbed, ==, RELOADED);
	}
}

/* ── Replace the download in flight: where a torrent is ─────────── */

/* A replace of the film's grab in flight with @release, ticked, as the
 * confirmation of this moment offers it.  Returns what came of it; *@message
 * the error's words, when nothing was sent. */
static AxisAcquireGrabResult
replace_with(
	Fixture      *fx,
	gint64        movie,
	const gchar  *release,
	gchar       **message
){
	g_autoptr(AxisAcquireHandPreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisAcquireReplaceTicks swap;
	AxisAcquireHandTicks ticks;
	const AxisAcquireStanding *standing;
	gboolean sent;

	g_assert_true(axis_movies_hand_preview(fx->movies, movie, candidate_id(fx, release), &preview, &error));
	g_assert_no_error(error);
	g_assert_true(preview->replaceable);
	g_assert_cmpuint(preview->standing->len, ==, 1);
	standing = g_ptr_array_index(preview->standing, 0);
	memset(&swap, 0, sizeof(swap));
	memset(&ticks, 0, sizeof(ticks));
	swap.grab_id = standing->grab_id;
	swap.ok = TRUE;
	swap.digest = preview->replace_digest;
	ticks.in_flight = &swap;
	sent = axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, release), &ticks, "root", NULL, &result, &error);
	g_assert_true(sent == (result == AXIS_ACQUIRE_GRAB_SENT));
	if (message != NULL)
		*message = error != NULL ? g_strdup(error->message) : NULL;
	return result;
}

/* The replace hook: the client stops answering between the look at where the
 * download is and its removal. */
static void
client_goes_down(gint64 movie, gpointer data)
{
	Fixture *fx = data;

	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
}

/*
 * add-replace-grab-and-delete-download, slice A: what a replace never
 * deletes.  A torrent's client deletes a torrent wherever it is, and a person
 * can move one in the client, an unfinished one too.  The client is asked
 * where the torrent is before it is asked to remove it with its data: inside
 * a library folder, at one, or at a place no path mapping reaches, the
 * replace is refused and nothing is ended; and when the removal is asked
 * again by the follow-up (the client did not answer the first time), the
 * place is judged again, each time.  The fake client deletes, on a real disk,
 * what a removal with data names: a file in the library stays.
 */
static void
test_replace_torrent_place(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ WEBDL, "torrent", 6000000000, MAZEWORK_IMDB, NULL },
		{ BLURAY, "torrent", 12000000000, MAZEWORK_IMDB, NULL }
	};
	g_autofree gchar *library = g_build_filename(fx->disk, "films", NULL);
	g_autofree gchar *in_library = g_build_filename(fx->disk, "films", WEBDL, NULL);
	g_autofree gchar *library_file = g_build_filename(fx->disk, "films", WEBDL, "the-mazework.mkv", NULL);
	g_autofree gchar *incomplete = g_build_filename(fx->disk, "downloads", "incomplete", NULL);
	g_autofree gchar *own = g_build_filename(fx->disk, "downloads", "incomplete", WEBDL, NULL);
	g_autofree gchar *own_file = g_build_filename(fx->disk, "downloads", "incomplete", WEBDL, "part.mkv", NULL);
	g_autofree gchar *moved = g_build_filename(fx->disk, "films", BLURAY, NULL);
	g_autofree gchar *moved_file = g_build_filename(fx->disk, "films", BLURAY, "the-mazework.mkv", NULL);
	guint n_removes = 0;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *why = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireHandTicks none;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	gint64 movie = add_film(fx, "101");

	fx->client_id = save_client(fx, 0, "Torrents", "fixture", TRUE);
	/* The client sees the downloads and, as many do, the library too. */
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
		"downloads", &error));
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/films", fx->volume_id, "films",
		&error));
	g_assert_no_error(error);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_cmpstr(search(fx, movie, "list").outcome, ==, "listed");
	memset(&none, 0, sizeof(none));
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, WEBDL), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	ref = client_ref_of(fx, movie);
	g_assert_nonnull(ref);

	/* A person moved the unfinished torrent into the films' library folder,
	 * where a file of that name is.  The client would delete it there. */
	axis_test_storage_mkdir(in_library);
	axis_test_storage_write(library_file, "the film in the library");
	axis_test_download_fake_set_data_dir(fx->fake, library);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/films/" WEBDL, NULL);
	poll(fx);
	result = replace_with(fx, movie, BLURAY, &why);
	/* (First: the file in the library is still there.) */
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, WEBDL " is not replaced. Axis does not ask the client to delete it");
	assert_contains(why, "/data/films/" WEBDL ", which is inside a library folder");
	assert_contains(why, "Nothing was ended and nothing was sent");
	g_clear_pointer(&why, g_free);
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	assert_q(fx, "1|1", "SELECT count(*)::text || '|' || count(*) FILTER (WHERE closed_at IS NULL) FROM movie_grab");
	g_assert_cmpuint(n_adds(fx), ==, 1);

	/* The library folder itself; and a place no path mapping reaches, of
	 * which Axis cannot tell that it is not a library folder. */
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/films", NULL);
	result = replace_with(fx, movie, BLURAY, &why);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, "which is a library folder itself");
	g_clear_pointer(&why, g_free);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/elsewhere/" WEBDL, NULL);
	result = replace_with(fx, movie, BLURAY, &why);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, "none of the client's path mappings reaches");
	g_clear_pointer(&why, g_free);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/../films/" WEBDL, NULL);
	result = replace_with(fx, movie, BLURAY, &why);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, "is not a plain full path");
	g_clear_pointer(&why, g_free);
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	assert_q(fx, "1|1", "SELECT count(*)::text || '|' || count(*) FILTER (WHERE closed_at IS NULL) FROM movie_grab");

	/* In its own place again, and the client does not answer when the
	 * replace is confirmed: where the torrent is cannot be asked, so nothing
	 * is ended and nothing is sent (the review's S1: the grab was ended). */
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/incomplete/" WEBDL, NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	result = replace_with(fx, movie, BLURAY, &why);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, WEBDL " is not replaced. Its client could not be asked where it is");
	assert_contains(why, "Nothing was ended and nothing was sent. Try again when the client answers");
	g_clear_pointer(&why, g_free);
	assert_q(fx, "1|1", "SELECT count(*)::text || '|' || count(*) FILTER (WHERE closed_at IS NULL) FROM movie_grab");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_cmpuint(n_adds(fx), ==, 1);

	/* The client lists another torrent that Axis cannot read, and not this
	 * one: it may be there all the same, so it is not taken for gone (L2). */
	axis_test_download_fake_set_unreadable(fx->fake, ref, "its name is not text");
	result = replace_with(fx, movie, BLURAY, &why);
	axis_test_download_fake_set_unreadable(fx->fake, ref, NULL);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, "part of the client's list could not be read");
	assert_contains(why, "Nothing was ended and nothing was sent");
	g_clear_pointer(&why, g_free);
	assert_q(fx, "1|1", "SELECT count(*)::text || '|' || count(*) FILTER (WHERE closed_at IS NULL) FROM movie_grab");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* The client answers the look and stops answering before the removal
	 * (the window that is left): the grab is ended, nothing is sent, and the
	 * removal waits for the follow-up. */
	axis_movies_set_replace_hook(fx->movies, client_goes_down, fx);
	result = replace_with(fx, movie, BLURAY, &why);
	axis_movies_set_replace_hook(fx->movies, NULL, NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	assert_contains(why, WEBDL " was ended, but it is still at its client. The client could not be asked where it is");
	assert_contains(why, "Nothing was sent");
	g_clear_pointer(&why, g_free);
	assert_q(fx, "replaced|failed", "SELECT close_reason || '|' || removal FROM movie_grab");
	g_assert_cmpuint(n_adds(fx), ==, 1);

	/* While it waits, the person moves the torrent into the library.  The
	 * follow-up asks the client where it is before it asks it to remove it:
	 * it is not asked, the file in the library stays, and the movie stays in
	 * flight until the torrent is gone from the client. */
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/films/" WEBDL, NULL);
	follow(fx);
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	assert_q(fx, "replaced|failed", "SELECT close_reason || '|' || removal FROM movie_grab");
	assert_q_has(fx, "Axis does not ask the client to delete it", "SELECT removal_text FROM movie_grab");
	assert_q_has(fx, "which is inside a library folder", "SELECT removal_text FROM movie_grab");
	g_assert_cmpstr(search(fx, movie, "scheduled").skipped, ==, "a grab of it is in flight");

	/* Back in the client's own place: the next follow-up has it removed with
	 * its data.  Its own folder goes; the library's file stays.  No search is
	 * queued: a replace is the person's to finish. */
	axis_test_storage_mkdir(own);
	axis_test_storage_write(own_file, "half a film");
	axis_test_download_fake_set_data_dir(fx->fake, incomplete);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/incomplete/" WEBDL, NULL);
	follow(fx);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 1);
	g_assert_false(g_file_test(own_file, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));
	assert_q(fx, "replaced|done", "SELECT close_reason || '|' || removal FROM movie_grab");
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE kind = 'movies.search'");
	g_assert_cmpuint(n_adds(fx), ==, 1);

	/* A torrent in its own place is replaced: removed with its data, and the
	 * release chosen is sent. */
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, BLURAY), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	g_free(ref);
	ref = client_ref_of(fx, movie);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/incomplete/" BLURAY, NULL);
	poll(fx);
	result = replace_with(fx, movie, WEBDL, &why);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	g_assert_null(why);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 2);
	assert_q(fx, WEBDL, "SELECT release_name FROM movie_grab WHERE closed_at IS NULL");
	g_assert_cmpuint(n_adds(fx), ==, 3);

	/* A torrent its client does not list any more: asked without its data,
	 * which deletes nothing; the release chosen is sent. */
	g_free(ref);
	ref = client_ref_of(fx, movie);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/incomplete/" WEBDL, NULL);
	poll(fx);
	axis_test_download_fake_drop(fx->fake, ref);
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	result = replace_with(fx, movie, BLURAY, &why);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_SENT);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 2);
	assert_q(fx, BLURAY, "SELECT release_name FROM movie_grab WHERE closed_at IS NULL");
	g_assert_true(g_file_test(library_file, G_FILE_TEST_EXISTS));

	/* The review's S2.  A replace whose removal waits (the client stopped
	 * answering after the look); the person moves the torrent into the
	 * library, and it finishes there.  The follow-up used to mark the removal
	 * done without asking, and the finished download stayed at its client with
	 * nothing said.  It is judged still: where it is, the client is not asked
	 * to delete; the removal ends as kept, with the words on the grab, on the
	 * download (the Downloads page) and in the movie's history; the file in
	 * the library stays; and the movie is not in flight any more. */
	g_free(ref);
	ref = client_ref_of(fx, movie);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/downloads/incomplete/" BLURAY, NULL);
	poll(fx);
	axis_movies_set_replace_hook(fx->movies, client_goes_down, fx);
	result = replace_with(fx, movie, WEBDL, &why);
	axis_movies_set_replace_hook(fx->movies, NULL, NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_REPLACE_STUCK);
	g_clear_pointer(&why, g_free);
	assert_qf(fx, "replaced|failed", "SELECT close_reason || '|' || removal FROM movie_grab g JOIN download d ON d.id = "
		"g.download_id WHERE d.client_ref = '%s'", ref);
	axis_test_storage_mkdir(moved);
	axis_test_storage_write(moved_file, "the film, finished in the library");
	axis_test_download_fake_set_data_dir(fx->fake, library);
	axis_test_download_fake_set(fx->fake, ref, "completed", 12000000000, "/data/films/" BLURAY, NULL);
	poll(fx);
	assert_qf(fx, "completed", "SELECT state FROM download WHERE client_ref = '%s'", ref);
	n_removes = axis_test_download_fake_count(fx->fake, "remove", NULL);
	follow(fx);
	/* (First: the file in the library is still there.) */
	g_assert_true(g_file_test(moved_file, G_FILE_TEST_EXISTS));
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, n_removes);
	assert_qf(fx, "replaced|kept", "SELECT close_reason || '|' || removal FROM movie_grab g JOIN download d ON d.id = "
		"g.download_id WHERE d.client_ref = '%s'", ref);
	{
		g_autofree gchar *on_grab = q(fx, "SELECT g.removal_text FROM movie_grab g JOIN download d ON d.id = "
			"g.download_id WHERE d.client_ref = '%s'", ref);
		g_autofree gchar *on_download = q(fx, "SELECT folder_removal || '|' || folder_removal_text FROM download"
			" WHERE client_ref = '%s'", ref);
		g_autofree gchar *in_history = q(fx, "SELECT text FROM movie_event WHERE kind = 'removal' ORDER BY id DESC"
			" LIMIT 1");

		assert_contains(on_grab, "It was to be replaced, could not be removed, and has finished downloading at its "
			"client, where it still is");
		assert_contains(on_grab, "/data/films/" BLURAY ", which is inside a library folder");
		assert_contains(on_grab, "remove it in the client");
		assert_contains(on_download, "left|It was to be replaced");
		assert_contains(in_history, BLURAY ": It was to be replaced");
		assert_contains(in_history, "The movie is not in flight any more");
	}
	/* Not asked again; nothing is in flight. */
	follow(fx);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, n_removes);
	g_assert_true(g_file_test(moved_file, G_FILE_TEST_EXISTS));
	assert_q(fx, "0", "SELECT count(*) FROM movie_grab WHERE closed_at IS NULL OR removal IN ('pending', 'failed')");
	{
		g_autoptr(AxisMovieFacts) facts = NULL;

		g_assert_true(axis_movies_get(fx->movies, movie, &facts, &error));
		g_assert_no_error(error);
		g_assert_false(axis_movie_in_flight(facts));
	}
}

/*
 * The review's S3: a second download of the movie that is still at a client
 * and that no grab follows.  The module counts it as in flight, so its claim
 * refuses while it is there: a replace would end the one grab and then send
 * nothing.  No replace is offered; the page names that download and points to
 * the Downloads page; and a post all the same ends nothing.
 */
/*
 * Give up, and an ordinary failure, on a torrent a person moved into a
 * library folder: the client would delete it there when asked to remove it
 * with its data.  Where the client says the torrent is in a library folder,
 * it is removed from the client without its data and the files are left,
 * which the grab and the Downloads page say.  A place no path mapping
 * reaches is not known to be a library folder: removed with its data, as it
 * always was (a failed download in the client's own folder is cleaned up).
 */
static void
test_failure_torrent_place(Fixture *fx, gconstpointer data)
{
	static const Rel rels[] = {
		{ WEBDL, "torrent", 6000000000, MAZEWORK_IMDB, NULL },
		{ BLURAY, "torrent", 12000000000, MAZEWORK_IMDB, NULL },
		{ WEBRIP, "torrent", 4000000000, MAZEWORK_IMDB, NULL }
	};
	g_autofree gchar *library = g_build_filename(fx->disk, "films", NULL);
	g_autofree gchar *given_up_dir = g_build_filename(fx->disk, "films", WEBDL, NULL);
	g_autofree gchar *given_up_file = g_build_filename(fx->disk, "films", WEBDL, "the-mazework.mkv", NULL);
	g_autofree gchar *failed_dir = g_build_filename(fx->disk, "films", BLURAY, NULL);
	g_autofree gchar *failed_file = g_build_filename(fx->disk, "films", BLURAY, "the-mazework.mkv", NULL);
	g_autofree gchar *elsewhere = g_build_filename(fx->disk, "elsewhere", NULL);
	g_autofree gchar *own_dir = g_build_filename(fx->disk, "elsewhere", WEBRIP, NULL);
	g_autofree gchar *own_file = g_build_filename(fx->disk, "elsewhere", WEBRIP, "part.mkv", NULL);
	g_autofree gchar *ref = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireHandTicks none;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	AxisMovieFollowReport report;
	gint64 movie = add_film(fx, "101");

	fx->client_id = save_client(fx, 0, "Torrents", "fixture", TRUE);
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
		"downloads", &error));
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/films", fx->volume_id, "films",
		&error));
	g_assert_no_error(error);
	set_releases(fx, rels, G_N_ELEMENTS(rels));
	g_assert_cmpstr(search(fx, movie, "list").outcome, ==, "listed");
	memset(&none, 0, sizeof(none));

	/* Give up on a torrent moved into the films' library folder. */
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, WEBDL), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	ref = client_ref_of(fx, movie);
	g_assert_nonnull(ref);
	axis_test_storage_mkdir(given_up_dir);
	axis_test_storage_write(given_up_file, "the film in the library");
	axis_test_download_fake_set_data_dir(fx->fake, library);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/data/films/" WEBDL, NULL);
	poll(fx);
	g_assert_true(axis_movies_give_up(fx->movies, movie, "root", &error));
	g_assert_no_error(error);
	/* (First: the file in the library is still there.) */
	g_assert_true(g_file_test(given_up_file, G_FILE_TEST_EXISTS));
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_q(fx, "given_up|done", "SELECT close_reason || '|' || removal FROM movie_grab ORDER BY id DESC LIMIT 1");
	/* The history line written before the client is asked is true of what
	 * followed: it does not say "with its data" of a torrent outright. */
	assert_q_has(fx, "Axis asks the client to remove it, with its data unless the client says its files are in a library "
		"folder.", "SELECT text FROM movie_event WHERE kind = 'abandoned' ORDER BY id DESC LIMIT 1");
	assert_q_has(fx, "Removed from its client without its data. Its files were left where they are: its client says the "
		"torrent's files are at /data/films/" WEBDL ", which is inside a library folder",
		"SELECT removal_text FROM movie_grab ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "removed|left", "SELECT state || '|' || folder_removal FROM download WHERE client_ref = '%s'", ref);
	assert_q_has(fx, "Its files were left where they are", "SELECT folder_removal_text FROM download ORDER BY id DESC"
		" LIMIT 1");

	/* An ordinary failure of a torrent that is in the library folder: the
	 * follow-up blocks it and removes it, and deletes nothing there. */
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, BLURAY), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	g_free(ref);
	ref = client_ref_of(fx, movie);
	axis_test_storage_mkdir(failed_dir);
	axis_test_storage_write(failed_file, "another film in the library");
	axis_test_download_fake_set(fx->fake, ref, "failed", 100, "/data/films/" BLURAY, "Out of disk space");
	poll(fx);
	report = follow(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_true(g_file_test(failed_file, G_FILE_TEST_EXISTS));
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	assert_q(fx, "failed|done", "SELECT close_reason || '|' || removal FROM movie_grab ORDER BY id DESC LIMIT 1");
	assert_q_has(fx, "which is inside a library folder", "SELECT removal_text FROM movie_grab ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "left", "SELECT folder_removal FROM download WHERE client_ref = '%s'", ref);

	/* A place no path mapping reaches (the client's own folder, as far as Axis
	 * knows): removed with its data, as before. */
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, WEBRIP), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	g_free(ref);
	ref = client_ref_of(fx, movie);
	axis_test_storage_mkdir(own_dir);
	axis_test_storage_write(own_file, "half a film");
	axis_test_download_fake_set_data_dir(fx->fake, elsewhere);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, "/elsewhere/" WEBRIP, NULL);
	poll(fx);
	g_assert_true(axis_movies_give_up(fx->movies, movie, "root", &error));
	g_assert_no_error(error);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 1);
	g_assert_false(g_file_test(own_file, G_FILE_TEST_EXISTS));
	assert_q(fx, "given_up|done", "SELECT close_reason || '|' || removal FROM movie_grab ORDER BY id DESC LIMIT 1");
	assert_q(fx, "none", "SELECT coalesce(removal_text, 'none') FROM movie_grab ORDER BY id DESC LIMIT 1");
	assert_qf(fx, "none", "SELECT coalesce(folder_removal, 'none') FROM download WHERE client_ref = '%s'", ref);
	g_assert_true(g_file_test(given_up_file, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(failed_file, G_FILE_TEST_EXISTS));
}

#define SAME_HASH "0123456789abcdef0123456789abcdef01234567"

static void
test_replace_stray(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisAcquireHandPreview) preview = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireHandTicks none;
	AxisAcquireHandTicks ticks;
	AxisAcquireReplaceTicks swap;
	AxisAcquireGrabResult result = AXIS_ACQUIRE_GRAB_SOURCE_ERROR;
	const AxisAcquireStanding *standing;
	gint64 movie = add_film(fx, "101");

	set_releases(fx, standard, G_N_ELEMENTS(standard));
	g_assert_cmpstr(search(fx, movie, "list").outcome, ==, "listed");
	memset(&none, 0, sizeof(none));
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, WEBDL), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);
	/* Its grab is closed with nothing left to remove, as far as Axis knows,
	 * while its download is at the client still; then another is grabbed. */
	exec(fx, "UPDATE movie_grab SET closed_at = now(), close_reason = 'given_up', removal = 'done'");
	exec(fx, "UPDATE download SET state = 'failed'");
	g_assert_true(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, BLURAY), &none, "root", NULL, &result,
		&error));
	g_assert_no_error(error);

	/* (L3) The release chosen is the very torrent in flight, listed by another
	 * indexer under another name: the same info hash.  Nothing to replace it
	 * with; before, it was offered, and the torrent deleted and added again. */
	exec(fx, "UPDATE movie_grab SET info_hash = '%s' WHERE closed_at IS NULL", SAME_HASH);
	exec(fx, "UPDATE movie_candidate SET info_hash = '%s' WHERE name = '%s'", SAME_HASH, UHD);
	{
		g_autoptr(AxisAcquireHandPreview) same = NULL;

		g_assert_true(axis_movies_hand_preview(fx->movies, movie, candidate_id(fx, UHD), &same, &error));
		g_assert_no_error(error);
		g_assert_true(same->in_flight);
		g_assert_false(same->replaceable);
		assert_contains(same->refused, "This release is the same torrent as the one in flight (" BLURAY);
	}
	exec(fx, "UPDATE movie_candidate SET info_hash = NULL");
	exec(fx, "UPDATE movie_grab SET info_hash = NULL");

	exec(fx, "UPDATE download SET state = 'downloading' WHERE release_name = '%s'", WEBDL);
	assert_q(fx, "2|2", "SELECT count(*)::text || '|' || count(*) FILTER (WHERE state IN ('queued', 'downloading'))"
		" FROM download");

	g_assert_true(axis_movies_hand_preview(fx->movies, movie, candidate_id(fx, UHD), &preview, &error));
	g_assert_no_error(error);
	g_assert_true(preview->in_flight);
	/* (First: no replace is offered.) */
	g_assert_false(preview->replaceable);
	g_assert_null(preview->replace_digest);
	assert_contains(preview->refused, BLURAY " is downloading");
	assert_contains(preview->refused, "another download of the movie is still at a download client (" WEBDL);
	assert_contains(preview->refused, "Remove it on the Downloads page first");
	g_assert_true(preview->standing_downloads);
	g_assert_cmpuint(preview->standing->len, ==, 1);
	standing = g_ptr_array_index(preview->standing, 0);

	/* A post that asks for the replace all the same: nothing is ended. */
	memset(&swap, 0, sizeof(swap));
	memset(&ticks, 0, sizeof(ticks));
	swap.grab_id = standing->grab_id;
	swap.ok = TRUE;
	swap.digest = "0123456789abcdef0123456789abcdef";
	ticks.in_flight = &swap;
	g_assert_false(axis_movies_grab_by_hand(fx->movies, movie, candidate_id(fx, UHD), &ticks, "root", NULL, &result,
		&error));
	g_assert_cmpint(result, ==, AXIS_ACQUIRE_GRAB_CHANGED);
	assert_contains(error->message, "Nothing was ended and nothing was sent");
	assert_q(fx, BLURAY, "SELECT release_name FROM movie_grab WHERE closed_at IS NULL");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_cmpuint(n_adds(fx), ==, 2);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	AXIS_TEST_ADD_DB_CASE("/movies/replace-grab/torrent-place", Fixture, GUINT_TO_POINTER(NO_CLIENT),
		test_replace_torrent_place);
	ADD("/movies/replace-grab/stray-download", test_replace_stray);
	AXIS_TEST_ADD_DB_CASE("/movies/failure/torrent-place", Fixture, GUINT_TO_POINTER(NO_CLIENT),
		test_failure_torrent_place);
	ADD("/movies/add", test_add);
	ADD("/movies/end-to-end", test_end_to_end);
	AXIS_TEST_ADD_DB_CASE("/movies/end-to-end-native", Fixture, GUINT_TO_POINTER(NATIVE_ONLY),
		test_end_to_end_native);
	ADD("/movies/states", test_states);
	ADD("/movies/never-grabbed", test_never_grabbed);
	ADD("/movies/same-name", test_same_name);
	AXIS_TEST_ADD_DB_CASE("/movies/same-name-searched", Fixture, GUINT_TO_POINTER(WITH_TMDB), test_same_name_searched);
	ADD("/movies/grab-once", test_grab_once);
	ADD("/movies/failure", test_failure);
	ADD("/movies/scheduler", test_scheduler);
	ADD("/movies/unusable", test_unusable);
	ADD("/movies/manual-grab", test_manual_grab);
	ADD("/movies/actions-logged", test_actions_logged);
	ADD("/movies/uncertain", test_uncertain);
	ADD("/movies/prune", test_prune);
	ADD("/movies/prune-paced", test_prune_paced);
	ADD("/movies/prune-failed-is-tried-again", test_prune_failed_is_tried_again);
	ADD("/movies/title-merge", test_title_merge);
	ADD("/movies/on-disk", test_on_disk);
#undef ADD
	return g_test_run();
}
