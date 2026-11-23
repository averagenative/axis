/*
 * axis-test-sonarr.c - test support: a stand-in for a person's Sonarr
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-sonarr.h"

#include <string.h>

#include "arr/axis-arr-assumed.h"

#define DIR "tests/fixtures/arr/"

struct _AxisTestSonarr
{
	AxisTestPlex *server;
	GMutex        lock;
	gchar        *key;
	gchar        *prefix;
	gchar        *app_name;
	gchar        *status;
	gchar        *series;
	gchar        *profiles;
	gchar        *folders;
	GHashTable   *episodes;	/* gint64 * (a series) to JSON */
	GHashTable   *files;	/* gint64 * (a series) to JSON */
	GArray       *queue;	/* gint64 */
	guint         gen_series;
	guint         gen_per;
	guint         non_get;
	gint64        fail_series;	/* its lists answer fail_status, fail_times times (-1: always) */
	guint         fail_status;
	gint          fail_times;
};

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(DIR, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	return text;
}

static void
answer_json(
	AxisTestPlexAnswer *answer,
	guint               status,
	const gchar        *json
){
	answer->status = status;
	answer->content_type = g_strdup("application/json; charset=utf-8");
	answer->body = g_bytes_new(json, strlen(json));
}

static guint
query_number(
	GHashTable  *query,
	const gchar *name,
	guint        fallback
){
	const gchar *text = query != NULL ? g_hash_table_lookup(query, name) : NULL;

	if (text == NULL || !g_ascii_isdigit(text[0]))
		return fallback;
	return (guint)g_ascii_strtoull(text, NULL, 10);
}

/* The episodes (or, with @files, the episode files) of generated series
 * @series. */
static gchar *
generated(
	AxisTestSonarr *self,
	guint           series,
	gboolean        files
){
	GString *json = g_string_new("[");
	guint i;

	for (i = 0; i < self->gen_per; i++) {
		guint season = i / 20 + 1;
		guint number = i % 20 + 1;
		guint id = series * 1000 + i + 1;

		if (files)
			g_string_append_printf(json, "%s{\"" AXIS_ARR_EPISODE_FILE_ID "\":%u,\"seriesId\":%u,\"" AXIS_ARR_FILE_PATH
				"\":\"/tv/Made Up Series %u/Season %u/Made Up Series %u - S%02uE%02u.mkv\",\"" AXIS_ARR_FILE_SIZE
				"\":%u,\"" AXIS_ARR_FILE_QUALITY "\":{\"" AXIS_ARR_FILE_QUALITY_INNER "\":{\"" AXIS_ARR_FILE_QUALITY_NAME
				"\":\"WEBDL-1080p\"}},\"mediaInfo\":{\"videoCodec\":\"h264\",\"audioChannels\":5.1}}", i > 0 ? "," : "",
				id, series, series, season, series, season, number, 1000 + i);
		else
			g_string_append_printf(json, "%s{\"" AXIS_ARR_EPISODE_ID "\":%u,\"seriesId\":%u,\"" AXIS_ARR_EPISODE_TVDB
				"\":%u,\"" AXIS_ARR_EPISODE_SEASON "\":%u,\"" AXIS_ARR_EPISODE_NUMBER "\":%u,\"" AXIS_ARR_EPISODE_AIR_DATE
				"\":\"2021-03-%02u\",\"title\":\"Made Up Episode %u\",\"overview\":\"Nothing much happens.\",\""
				AXIS_ARR_EPISODE_MONITORED "\":%s,\"hasFile\":true,\"" AXIS_ARR_EPISODE_FILE "\":%u}", i > 0 ? "," : "",
				id, series, 90000000 + id, season, number, number, i + 1, i % 5 == 4 ? "false" : "true", id);
	}
	g_string_append_c(json, ']');
	return g_string_free(json, FALSE);
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestSonarr *self = user_data;
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	const gchar *given = axis_test_plex_request_header(request, AXIS_ARR_KEY_HEADER);
	const gchar *path = request->path;

	g_mutex_lock(&self->lock);
	/* Axis only reads from Sonarr: anything else is counted, and refused. */
	if (strcmp(request->method, "GET") != 0) {
		self->non_get++;
		answer_json(answer, 405, "{\"message\":\"MethodNotAllowed\"}");
		goto out;
	}
	if (self->prefix[0] != '\0') {
		if (!g_str_has_prefix(path, self->prefix)) {
			answer_json(answer, 404, "{\"message\":\"NotFound\"}");
			goto out;
		}
		path += strlen(self->prefix);
	}
	if (!g_str_has_prefix(path, AXIS_ARR_API_PREFIX "/")) {
		answer->status = 200;
		answer->content_type = g_strdup("text/html");
		answer->body = g_bytes_new_static("<!doctype html><title>Sonarr</title>", 36);
		goto out;
	}
	if (given == NULL || strcmp(given, self->key) != 0) {
		answer_json(answer, 401, "{\"error\":\"Unauthorized\"}");
		goto out;
	}
	if (strcmp(path, AXIS_ARR_STATUS_PATH) == 0) {
		g_autofree gchar *named = self->app_name != NULL ? g_strdup_printf("{\"" AXIS_ARR_STATUS_APP "\":\"%s\","
			"\"" AXIS_ARR_STATUS_VERSION "\":\"4.0.9.2244\",\"isDocker\":true}", self->app_name) : NULL;

		answer_json(answer, 200, named != NULL ? named : self->status);
	} else if (strcmp(path, AXIS_ARR_SERIES_PATH) == 0) {
		answer_json(answer, 200, self->series);
	} else if (strcmp(path, AXIS_ARR_PROFILES_PATH) == 0) {
		answer_json(answer, 200, self->profiles);
	} else if (strcmp(path, AXIS_ARR_FOLDERS_PATH) == 0) {
		answer_json(answer, 200, self->folders);
	} else if (strcmp(path, AXIS_ARR_API_PREFIX "/episode") == 0 || strcmp(path, AXIS_ARR_API_PREFIX "/episodefile") == 0) {
		gboolean files = strcmp(path, AXIS_ARR_API_PREFIX "/episodefile") == 0;
		gint64 series = query_number(query, "seriesId", 0);
		const gchar *set = g_hash_table_lookup(files ? self->files : self->episodes, &series);

		if (series == self->fail_series && self->fail_times != 0) {
			if (self->fail_times > 0)
				self->fail_times--;
			answer_json(answer, self->fail_status, "{\"message\":\"Unavailable\"}");
		} else if (set != NULL) {
			answer_json(answer, 200, set);
		} else if (series >= 1 && series <= self->gen_series) {
			g_autofree gchar *made = generated(self, (guint)series, files);

			answer_json(answer, 200, made);
		} else {
			answer_json(answer, 200, "[]");
		}
	} else if (strcmp(path, AXIS_ARR_API_PREFIX "/queue") == 0) {
		guint page = query_number(query, "page", 1);
		guint size = query_number(query, "pageSize", 10);
		g_autoptr(GString) json = g_string_new(NULL);
		guint first = page > 0 ? (page - 1) * size : 0;
		guint i;

		g_string_append_printf(json, "{\"page\":%u,\"pageSize\":%u,\"" AXIS_ARR_PAGE_TOTAL "\":%u,\""
			AXIS_ARR_PAGE_RECORDS "\":[", page, size, self->queue->len);
		for (i = first; i < self->queue->len && i < first + size; i++)
			g_string_append_printf(json, "%s{\"" AXIS_ARR_QUEUE_SERIES "\":%" G_GINT64_FORMAT ",\"episodeId\":%u,"
				"\"status\":\"downloading\",\"id\":%u}", i > first ? "," : "", g_array_index(self->queue, gint64, i),
				700 + i, 9000 + i);
		g_string_append(json, "]}");
		answer_json(answer, 200, json->str);
	} else {
		answer_json(answer, 404, "{\"message\":\"NotFound\"}");
	}
out:
	g_mutex_unlock(&self->lock);
}

void
axis_test_sonarr_fail_series(
	AxisTestSonarr *self,
	gint64          series_id,
	guint           status,
	gint            times
){
	g_mutex_lock(&self->lock);
	self->fail_series = series_id;
	self->fail_status = status;
	self->fail_times = times;
	g_mutex_unlock(&self->lock);
}

AxisTestSonarr *
axis_test_sonarr_new(const gchar *key)
{
	AxisTestSonarr *self = g_new0(AxisTestSonarr, 1);
	g_autofree gchar *episodes = fixture("sonarr-episode.json");
	g_autofree gchar *files = fixture("sonarr-episodefile.json");

	g_mutex_init(&self->lock);
	self->key = g_strdup(key);
	self->prefix = g_strdup("");
	self->status = fixture("sonarr-status.json");
	self->series = fixture("sonarr-series.json");
	self->profiles = g_strdup("[{\"id\":4,\"name\":\"HD-1080p\"},{\"id\":6,\"name\":\"Ultra-HD\"}]");
	self->folders = g_strdup("[{\"id\":1,\"path\":\"/tv\",\"accessible\":true,\"freeSpace\":1000}]");
	self->episodes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	self->files = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	self->queue = g_array_new(FALSE, FALSE, sizeof(gint64));
	self->server = axis_test_plex_new();
	axis_test_plex_set_handler(self->server, handle, self);
	axis_test_sonarr_set_episodes(self, 1, episodes);
	axis_test_sonarr_set_episode_files(self, 1, files);
	return self;
}

void
axis_test_sonarr_free(AxisTestSonarr *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_mutex_clear(&self->lock);
	g_free(self->key);
	g_free(self->prefix);
	g_free(self->app_name);
	g_free(self->status);
	g_free(self->series);
	g_free(self->profiles);
	g_free(self->folders);
	g_hash_table_unref(self->episodes);
	g_hash_table_unref(self->files);
	g_array_unref(self->queue);
	g_free(self);
}

gchar *
axis_test_sonarr_base(AxisTestSonarr *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_sonarr_server(AxisTestSonarr *self)
{
	return self->server;
}

static void
replace(
	AxisTestSonarr  *self,
	gchar          **field,
	const gchar     *value
){
	g_mutex_lock(&self->lock);
	g_free(*field);
	*field = g_strdup(value);
	g_mutex_unlock(&self->lock);
}

void
axis_test_sonarr_set_prefix(
	AxisTestSonarr *self,
	const gchar    *prefix
){
	replace(self, &self->prefix, prefix != NULL ? prefix : "");
}

void
axis_test_sonarr_set_app(
	AxisTestSonarr *self,
	const gchar    *app_name
){
	replace(self, &self->app_name, app_name);
}

void
axis_test_sonarr_set_series(
	AxisTestSonarr *self,
	const gchar    *json
){
	replace(self, &self->series, json);
}

void
axis_test_sonarr_set_profiles(
	AxisTestSonarr *self,
	const gchar    *json
){
	replace(self, &self->profiles, json);
}

void
axis_test_sonarr_set_folders(
	AxisTestSonarr *self,
	const gchar    *json
){
	replace(self, &self->folders, json);
}

static void
set_part(
	AxisTestSonarr *self,
	GHashTable     *table,
	gint64          series_id,
	const gchar    *json
){
	gint64 *key = g_new(gint64, 1);

	*key = series_id;
	g_mutex_lock(&self->lock);
	if (json != NULL) {
		g_hash_table_replace(table, key, g_strdup(json));
	} else {
		g_hash_table_remove(table, key);
		g_free(key);
	}
	g_mutex_unlock(&self->lock);
}

void
axis_test_sonarr_set_episodes(
	AxisTestSonarr *self,
	gint64          series_id,
	const gchar    *json
){
	set_part(self, self->episodes, series_id, json);
}

void
axis_test_sonarr_set_episode_files(
	AxisTestSonarr *self,
	gint64          series_id,
	const gchar    *json
){
	set_part(self, self->files, series_id, json);
}

void
axis_test_sonarr_generate(
	AxisTestSonarr *self,
	guint           n_series,
	guint           per_series
){
	g_autoptr(GString) json = g_string_new("[");
	guint i;

	for (i = 1; i <= n_series; i++)
		g_string_append_printf(json, "%s{\"" AXIS_ARR_SERIES_ID "\":%u,\"" AXIS_ARR_SERIES_TITLE "\":\"Made Up Series %u\","
			"\"" AXIS_ARR_SERIES_YEAR "\":2021,\"" AXIS_ARR_SERIES_TVDB "\":%u,\"" AXIS_ARR_SERIES_MONITORED "\":%s,\""
			AXIS_ARR_SERIES_TYPE "\":\"standard\",\"" AXIS_ARR_SERIES_PROFILE "\":4,\"" AXIS_ARR_SERIES_PATH_MEMBER
			"\":\"/tv/Made Up Series %u\",\"" AXIS_ARR_SERIES_ROOT "\":\"/tv\",\"" AXIS_ARR_SERIES_MONITOR_NEW
			"\":\"all\",\"overview\":\"Made up, at some length, as a real record is.\",\"" AXIS_ARR_SERIES_SEASONS "\":[{\""
			AXIS_ARR_SEASON_NUMBER "\":1,\"" AXIS_ARR_SEASON_MONITORED "\":true},{\"" AXIS_ARR_SEASON_NUMBER "\":2,\""
			AXIS_ARR_SEASON_MONITORED "\":%s}]}", i > 1 ? "," : "", i, i, 700000 + i, i % 10 == 0 ? "false" : "true", i,
			i % 3 == 0 ? "false" : "true");
	g_string_append_c(json, ']');
	g_mutex_lock(&self->lock);
	g_free(self->series);
	self->series = g_strdup(json->str);
	self->gen_series = n_series;
	self->gen_per = per_series;
	g_hash_table_remove_all(self->episodes);
	g_hash_table_remove_all(self->files);
	g_mutex_unlock(&self->lock);
}

void
axis_test_sonarr_set_queue(
	AxisTestSonarr *self,
	const gint64   *series_ids,
	guint           n
){
	g_mutex_lock(&self->lock);
	g_array_set_size(self->queue, 0);
	if (n > 0)
		g_array_append_vals(self->queue, series_ids, n);
	g_mutex_unlock(&self->lock);
}

guint
axis_test_sonarr_n_non_get(AxisTestSonarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->non_get;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_sonarr_count(
	AxisTestSonarr *self,
	const gchar    *path
){
	g_autofree gchar *full = NULL;

	g_mutex_lock(&self->lock);
	full = g_strconcat(self->prefix, path, NULL);
	g_mutex_unlock(&self->lock);
	return axis_test_plex_count(self->server, full);
}

void
axis_test_sonarr_assert_read_only(AxisTestSonarr *self)
{
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(self->server);
	guint i;

	g_assert_cmpuint(axis_test_sonarr_n_non_get(self), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(self->server));
	for (i = 0; i < requests->len; i++) {
		const gchar *text = g_ptr_array_index(requests, i);
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		guint l;

		/* The request line: the method, and the path with its query. */
		if (!g_str_has_prefix(lines[0], "GET "))
			g_error("a request to Sonarr was not a GET: %s", lines[0]);
		if (strstr(lines[0], self->key) != NULL || strstr(lines[0], "apikey") != NULL)
			g_error("a request to Sonarr carried the key in its address: %s", lines[0]);
		for (l = 1; lines[l] != NULL; l++) {
			if (strstr(lines[l], self->key) != NULL &&
			    g_ascii_strncasecmp(lines[l], AXIS_ARR_KEY_HEADER ": ", strlen(AXIS_ARR_KEY_HEADER) + 2) != 0)
				g_error("a request to Sonarr carried the key outside its header: %s", lines[l]);
		}
	}
}
