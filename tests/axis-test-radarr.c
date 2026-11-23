/*
 * axis-test-radarr.c - test support: a stand-in for a person's Radarr
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-radarr.h"

#include <string.h>

#include "arr/axis-arr-assumed.h"

#define DIR "tests/fixtures/arr/"

struct _AxisTestRadarr
{
	AxisTestPlex *server;
	GMutex        lock;
	gchar        *key;
	gchar        *prefix;
	gchar        *app_name;
	gchar        *status;
	gchar        *movies;
	gchar        *profiles;
	gchar        *folders;
	GArray       *queue;	/* gint64 */
	guint         non_get;
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

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestRadarr *self = user_data;
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	const gchar *given = axis_test_plex_request_header(request, AXIS_ARR_KEY_HEADER);
	const gchar *path = request->path;

	g_mutex_lock(&self->lock);
	/* Axis only reads from Radarr: anything else is counted, and refused. */
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
		answer->body = g_bytes_new_static("<!doctype html><title>Radarr</title>", 36);
		goto out;
	}
	if (given == NULL || strcmp(given, self->key) != 0) {
		answer_json(answer, 401, "{\"error\":\"Unauthorized\"}");
		goto out;
	}
	if (strcmp(path, AXIS_ARR_STATUS_PATH) == 0) {
		g_autofree gchar *named = self->app_name != NULL ? g_strdup_printf("{\"" AXIS_ARR_STATUS_APP "\":\"%s\","
			"\"" AXIS_ARR_STATUS_VERSION "\":\"5.2.6.8376\",\"isDocker\":true}", self->app_name) : NULL;

		answer_json(answer, 200, named != NULL ? named : self->status);
	} else if (strcmp(path, AXIS_ARR_MOVIES_PATH) == 0) {
		answer_json(answer, 200, self->movies);
	} else if (strcmp(path, AXIS_ARR_PROFILES_PATH) == 0) {
		answer_json(answer, 200, self->profiles);
	} else if (strcmp(path, AXIS_ARR_FOLDERS_PATH) == 0) {
		answer_json(answer, 200, self->folders);
	} else if (strcmp(path, AXIS_ARR_API_PREFIX "/queue") == 0) {
		guint page = query_number(query, "page", 1);
		guint size = query_number(query, "pageSize", 10);
		g_autoptr(GString) json = g_string_new(NULL);
		guint first = page > 0 ? (page - 1) * size : 0;
		guint i;

		g_string_append_printf(json, "{\"page\":%u,\"pageSize\":%u,\"" AXIS_ARR_PAGE_TOTAL "\":%u,\""
			AXIS_ARR_PAGE_RECORDS "\":[", page, size, self->queue->len);
		for (i = first; i < self->queue->len && i < first + size; i++)
			g_string_append_printf(json, "%s{\"" AXIS_ARR_QUEUE_MOVIE "\":%" G_GINT64_FORMAT ",\"status\":"
				"\"downloading\",\"id\":%u}", i > first ? "," : "", g_array_index(self->queue, gint64, i), 9000 + i);
		g_string_append(json, "]}");
		answer_json(answer, 200, json->str);
	} else {
		answer_json(answer, 404, "{\"message\":\"NotFound\"}");
	}
out:
	g_mutex_unlock(&self->lock);
}

AxisTestRadarr *
axis_test_radarr_new(const gchar *key)
{
	AxisTestRadarr *self = g_new0(AxisTestRadarr, 1);
	static const gint64 queued[] = { 2 };

	g_mutex_init(&self->lock);
	self->key = g_strdup(key);
	self->prefix = g_strdup("");
	self->status = fixture("radarr-status.json");
	self->movies = fixture("radarr-movies.json");
	self->profiles = fixture("radarr-qualityprofile.json");
	self->folders = fixture("radarr-rootfolder.json");
	self->queue = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_array_append_vals(self->queue, queued, G_N_ELEMENTS(queued));
	self->server = axis_test_plex_new();
	axis_test_plex_set_handler(self->server, handle, self);
	return self;
}

void
axis_test_radarr_free(AxisTestRadarr *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->server);
	g_mutex_clear(&self->lock);
	g_free(self->key);
	g_free(self->prefix);
	g_free(self->app_name);
	g_free(self->status);
	g_free(self->movies);
	g_free(self->profiles);
	g_free(self->folders);
	g_array_unref(self->queue);
	g_free(self);
}

gchar *
axis_test_radarr_base(AxisTestRadarr *self)
{
	return axis_test_plex_base(self->server);
}

AxisTestPlex *
axis_test_radarr_server(AxisTestRadarr *self)
{
	return self->server;
}

static void
replace(
	AxisTestRadarr  *self,
	gchar          **field,
	const gchar     *value
){
	g_mutex_lock(&self->lock);
	g_free(*field);
	*field = g_strdup(value);
	g_mutex_unlock(&self->lock);
}

void
axis_test_radarr_set_prefix(
	AxisTestRadarr *self,
	const gchar    *prefix
){
	replace(self, &self->prefix, prefix != NULL ? prefix : "");
}

void
axis_test_radarr_set_app(
	AxisTestRadarr *self,
	const gchar    *app_name
){
	replace(self, &self->app_name, app_name);
}

void
axis_test_radarr_set_movies(
	AxisTestRadarr *self,
	const gchar    *json
){
	replace(self, &self->movies, json);
}

void
axis_test_radarr_set_profiles(
	AxisTestRadarr *self,
	const gchar    *json
){
	replace(self, &self->profiles, json);
}

void
axis_test_radarr_set_folders(
	AxisTestRadarr *self,
	const gchar    *json
){
	replace(self, &self->folders, json);
}

void
axis_test_radarr_set_queue(
	AxisTestRadarr *self,
	const gint64   *movie_ids,
	guint           n
){
	g_mutex_lock(&self->lock);
	g_array_set_size(self->queue, 0);
	if (n > 0)
		g_array_append_vals(self->queue, movie_ids, n);
	g_mutex_unlock(&self->lock);
}

guint
axis_test_radarr_n_non_get(AxisTestRadarr *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->non_get;
	g_mutex_unlock(&self->lock);
	return n;
}

guint
axis_test_radarr_count(
	AxisTestRadarr *self,
	const gchar    *path
){
	g_autofree gchar *full = NULL;

	g_mutex_lock(&self->lock);
	full = g_strconcat(self->prefix, path, NULL);
	g_mutex_unlock(&self->lock);
	return axis_test_plex_count(self->server, full);
}

void
axis_test_radarr_assert_read_only(AxisTestRadarr *self)
{
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(self->server);
	guint i;

	g_assert_cmpuint(axis_test_radarr_n_non_get(self), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(self->server));
	for (i = 0; i < requests->len; i++) {
		const gchar *text = g_ptr_array_index(requests, i);
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		guint l;

		/* The request line: the method, and the path with its query. */
		if (!g_str_has_prefix(lines[0], "GET "))
			g_error("a request to Radarr was not a GET: %s", lines[0]);
		if (strstr(lines[0], self->key) != NULL || strstr(lines[0], "apikey") != NULL)
			g_error("a request to Radarr carried the key in its address: %s", lines[0]);
		for (l = 1; lines[l] != NULL; l++) {
			if (strstr(lines[l], self->key) != NULL && !g_str_has_prefix(lines[l], AXIS_ARR_KEY_HEADER ": ") &&
			    g_ascii_strncasecmp(lines[l], AXIS_ARR_KEY_HEADER ": ", strlen(AXIS_ARR_KEY_HEADER) + 2) != 0)
				g_error("a request to Radarr carried the key outside its header: %s", lines[l]);
		}
	}
}
