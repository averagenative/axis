/*
 * axis-test-requests.c - test support for requests (see the header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-requests.h"

#include <string.h>

#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-plex.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "storage/axis-storage.h"

static void
route(
	AxisTestRequests *r,
	const gchar      *path,
	const gchar      *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(r->fa->api, path, 200, "application/json", body, NULL);
}

static void
film_route(
	AxisTestRequests *r,
	const gchar      *id,
	const gchar      *title,
	gint              year
){
	g_autofree gchar *path = g_strdup_printf("/title/film/%s", id);
	g_autofree gchar *json = g_strdup_printf("{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"%d-03-01\",\"overview\":\"A made-up film.\"}", id,
		title, year, year);

	route(r, path, json);
}

/* A series whose seasons @first to @last have @episodes aired episodes each
 * (a season 0 has one). */
static void
series_route(
	AxisTestRequests *r,
	const gchar      *id,
	const gchar      *title,
	gint              year,
	gint              first,
	gint              last,
	guint             episodes
){
	g_autofree gchar *path = g_strdup_printf("/title/series/%s", id);
	g_autoptr(GString) json = g_string_new(NULL);
	gint season;
	guint e;

	g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"series\",\"title\":\"%s\",\"year\":%d,"
		"\"status\":\"ended\",\"overview\":\"A made-up series.\",\"seasons\":[", id, title, year);
	for (season = first; season <= last; season++)
		g_string_append_printf(json, "%s{\"number\":%d,\"name\":\"Season %d\",\"air_date\":\"%d-02-01\","
			"\"episodes\":%u}", season > first ? "," : "", season, season, year + season, season == 0 ? 1 : episodes);
	g_string_append(json, "]}");
	route(r, path, json->str);
	for (season = first; season <= last; season++) {
		g_autofree gchar *season_path = g_strdup_printf("/title/series/%s/season/%d", id, season);
		g_autoptr(GString) body = g_string_new(NULL);
		guint n = season == 0 ? 1 : episodes;

		g_string_append_printf(body, "{\"number\":%d,\"name\":\"Season %d\",\"episodes\":[", season, season);
		for (e = 1; e <= n; e++)
			g_string_append_printf(body, "%s{\"number\":%u,\"id\":\"%s%02d%02u\",\"name\":\"Part %u\","
				"\"air_date\":\"%d-02-%02u\"}", e > 1 ? "," : "", e, id, season, e, e, year + season, e);
		g_string_append(body, "]}");
		route(r, season_path, body->str);
	}
}

AxisTestRequests *
axis_test_requests_new(void)
{
	AxisTestRequests *r = g_new0(AxisTestRequests, 1);
	g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;

	r->t = axis_test_library_new(16);
	if (r->t == NULL) {
		g_free(r);
		return NULL;
	}
	r->disk = axis_test_library_disk(r->t, "disk1");
	r->volume_id = axis_test_library_add_volume(r->t, r->disk, "Disk one");
	r->films_root = axis_test_library_add_root(r->t, r->volume_id, r->disk, "films", AXIS_MEDIA_MOVIES);
	r->series_root = axis_test_library_add_root(r->t, r->volume_id, r->disk, "series", AXIS_MEDIA_TV);
	path = g_build_filename(r->t->dir, "data", NULL);
	r->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
	/* Writing is allowed in these tests (the default is read-only). */
	g_assert_true(axis_settings_set(r->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;

	r->settings = axis_settings_new();
	r->meta = axis_meta_new(r->t->pool, r->settings, r->data, &error);
	g_assert_no_error(error);
	r->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(r->meta, &r->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(r->meta, &meta_tuning);
	r->indexers = axis_indexers_new(r->t->pool, r->settings);
	g_assert_true(axis_indexers_add_kind(r->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(r->indexers, &indexer_tuning);
	r->downloads = axis_downloads_new(r->t->pool, r->t->storage, r->settings);
	g_assert_true(axis_downloads_add_kind(r->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(r->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(r->downloads, &download_tuning);
	r->quality = axis_quality_store_new(r->t->pool, r->settings);
	r->movies = axis_movies_new(r->t->pool, r->settings, r->meta, r->indexers, r->downloads, r->t->storage, r->quality);
	r->tv = axis_tv_new(r->t->pool, r->settings, r->meta, r->indexers, r->downloads, r->t->storage, r->quality);
	axis_tv_set_clock(r->tv, AXIS_TEST_REQUESTS_NOW_US);
	r->requests = axis_requests_new(r->t->pool, r->settings, r->meta, r->movies, r->tv, r->t->storage, r->quality);
	axis_requests_set_clock(r->requests, AXIS_TEST_REQUESTS_NOW_US);
	axis_requests_set_zone(r->requests, utc);

	axis_test_meta_fixture_standard(r->fa);
	film_route(r, AXIS_TEST_REQUESTS_ORCHARD, "The Lantern Orchard", 2019);
	film_route(r, AXIS_TEST_REQUESTS_LEDGER, "Saltmarsh Ledger", 2015);
	film_route(r, AXIS_TEST_REQUESTS_KILN, "Paper Kiln", 2011);
	film_route(r, AXIS_TEST_REQUESTS_BELLS, "Quarry Bells", 2008);
	film_route(r, AXIS_TEST_REQUESTS_COMPASS, "Tin Compass", 2021);
	film_route(r, AXIS_TEST_REQUESTS_ALMANAC, "Low Tide Almanac", 2017);
	/* One its provider marks as adult: no search of the tests returns it, and
	 * a lookup by its identifier stores it with the mark. */
	route(r, "/title/film/" AXIS_TEST_REQUESTS_ANNEX, "{\"id\":\"" AXIS_TEST_REQUESTS_ANNEX "\",\"kind\":\"film\","
		"\"title\":\"Velvet Annex\",\"year\":2014,\"runtime\":90,\"status\":\"released\",\"release_date\":\"2014-03-01\","
		"\"overview\":\"A made-up film.\",\"adult\":true}");
	series_route(r, AXIS_TEST_REQUESTS_HARBOUR, "Harbour of Glass", 2020, 0, 3, 2);
	series_route(r, AXIS_TEST_REQUESTS_WINTERS, "Nine Winters", 2012, 1, 9, 1);
	series_route(r, AXIS_TEST_REQUESTS_CLOCKWORK, "Tidal Clockwork", 2018, 1, 2, 8);
	return r;
}

void
axis_test_requests_declare(AxisTestRequests *r)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_add_table(r->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(r->meta),
		&error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_QUALITY_MODULE_ID, axis_quality_store_settings(), &error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_MOVIES_MODULE_ID, axis_movies_get_module()->settings,
		&error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_TV_MODULE_ID, axis_tv_get_module()->settings, &error));
	g_assert_true(axis_settings_add_table(r->settings, AXIS_REQUESTS_MODULE_ID, axis_requests_get_module()->settings,
		&error));
	g_assert_no_error(error);
}

gint64
axis_test_requests_person(
	AxisTestRequests *r,
	const gchar      *username,
	gboolean          admin
){
	/* (Not a hash of any password: these accounts never log in.) */
	return axis_test_requests_n(r, "INSERT INTO app_user (username, password_hash, role) VALUES ('%s',"
		" '$argon2id$not-a-password', '%s') RETURNING id", username, admin ? "admin" : "user");
}

void
axis_test_requests_ready(AxisTestRequests *r)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_load(r->settings, r->t->pool, &error));
	g_assert_true(axis_settings_set(r->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_no_error(error);
	r->profile_id = axis_test_requests_n(r, "SELECT id FROM quality_profile ORDER BY id LIMIT 1");
	g_assert_cmpint(r->profile_id, >, 0);
	r->admin = axis_test_requests_person(r, "root", TRUE);
	r->robin = axis_test_requests_person(r, "robin", FALSE);
	r->sam = axis_test_requests_person(r, "sam", FALSE);
}

void
axis_test_requests_free(AxisTestRequests *r)
{
	if (r == NULL)
		return;
	axis_requests_unref(r->requests);
	axis_movies_unref(r->movies);
	axis_tv_unref(r->tv);
	axis_downloads_stop(r->downloads);
	axis_indexers_stop(r->indexers);
	axis_meta_stop(r->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_downloads_unref(r->downloads);
	axis_indexers_unref(r->indexers);
	axis_meta_unref(r->meta);
	axis_quality_store_unref(r->quality);
	axis_test_meta_fixture_free(r->fa);
	axis_settings_unref(r->settings);
	axis_data_dir_unref(r->data);
	g_free(r->disk);
	axis_test_library_free(r->t);
	g_free(r);
}

gint64
axis_test_requests_title(
	AxisTestRequests *r,
	AxisMetaKind      kind,
	const gchar      *provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 id;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(r->meta, "fixture", kind, provider_id, 5000, &answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

void
axis_test_requests_set_defaults(AxisTestRequests *r)
{
	g_autoptr(GError) error = NULL;
	AxisRequestDefaults defaults;

	memset(&defaults, 0, sizeof(defaults));
	defaults.movies_root = r->films_root;
	defaults.movies_profile = r->profile_id;
	defaults.series_root = r->series_root;
	defaults.series_profile = r->profile_id;
	g_assert_true(axis_requests_set_defaults(r->requests, &defaults, &error));
	g_assert_no_error(error);
}

gchar *
axis_test_requests_q(
	AxisTestRequests *r,
	const gchar      *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(r->t->db, sql);
}

gint64
axis_test_requests_n(
	AxisTestRequests *r,
	const gchar      *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *text = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	text = axis_test_db_query(r->t->db, sql);
	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : 0;
}

gint64
axis_test_requests_add_movie(
	AxisTestRequests *r,
	gint64            title_id
){
	g_autoptr(GError) error = NULL;
	AxisMovieSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id;
	spec.root_id = r->films_root;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(r->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

gint64
axis_test_requests_add_series(
	AxisTestRequests *r,
	gint64            title_id
){
	g_autoptr(GError) error = NULL;
	AxisTvSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id;
	spec.root_id = r->series_root;
	spec.monitor = AXIS_TV_MONITOR_NONE;
	spec.user = "root";
	g_assert_true(axis_tv_add(r->tv, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

/* A file of the index, present; its id.  Its path is @stem and a number of
 * its own, so that no two are the same file. */
static gint64
library_file(
	AxisTestRequests *r,
	gint64            root_id,
	const gchar      *stem
){
	static gint64 inode = 5100000;

	inode++;
	return axis_test_requests_n(r, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
		" first_seen_generation, last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", '%s-%" G_GINT64_FORMAT ".mkv', 'video', 2147483648, 1, 0, 1, 0, %" G_GINT64_FORMAT
		", 1, false, false, false, 1, 1, 1) RETURNING id", root_id, r->volume_id, stem, inode, inode);
}

void
axis_test_requests_movie_file(
	AxisTestRequests *r,
	gint64            movie_id
){
	g_autofree gchar *path = g_strdup_printf("Made Up %" G_GINT64_FORMAT "/film", movie_id);
	gint64 file = library_file(r, r->films_root, path);

	axis_test_requests_n(r, "INSERT INTO movie_file (file_id, movie_id, quality) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", 'bluray_1080p') RETURNING file_id", file, movie_id);
}

void
axis_test_requests_episode_files(
	AxisTestRequests *r,
	gint64            series_id,
	gint              season,
	guint             n
){
	guint i;

	for (i = 1; i <= n; i++) {
		g_autofree gchar *path = g_strdup_printf("Made Up %" G_GINT64_FORMAT "/Season %02d/part-%02u", series_id,
			season, i);
		gint64 episode = axis_test_requests_n(r, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
			" AND season_number = %d AND number = %u", series_id, season, i);
		gint64 file;

		g_assert_cmpint(episode, >, 0);
		file = library_file(r, r->series_root, path);
		axis_test_requests_n(r, "INSERT INTO series_file (file_id, series_id) VALUES (%" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ") RETURNING file_id", file, series_id);
		axis_test_requests_n(r, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ") RETURNING file_id", file, episode);
	}
}

gint64
axis_test_requests_movie_grab(
	AxisTestRequests *r,
	gint64            movie_id,
	const gchar      *close_reason
){
	g_autofree gchar *closed = close_reason != NULL ? g_strdup_printf("now(), '%s'", close_reason) : g_strdup("NULL, NULL");

	return axis_test_requests_n(r, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, closed_at,"
		" close_reason) VALUES (%" G_GINT64_FORMAT ", 'Made.Up.Release.1080p.WEB-DL-MADEUP', 'usenet', 'made-up-key', %s)"
		" RETURNING id", movie_id, closed);
}

void
axis_test_requests_import_blocked(
	AxisTestRequests *r,
	gint64            movie_id,
	gint64            grab_id
){
	axis_test_requests_n(r, "UPDATE movie_grab SET completed_at = now() WHERE id = %" G_GINT64_FORMAT " RETURNING id",
		grab_id);
	axis_test_requests_n(r, "INSERT INTO import (module, owner_id, grab_id, title_label, source_path, state, reason,"
		" reason_text) VALUES ('movies', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'A made-up film', 'downloads/made-up',"
		" 'blocked', 'no_video', 'No video file was found in it.') RETURNING id", movie_id, grab_id);
}
