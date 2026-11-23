/*
 * axis-test-tvdb.c - test support: a stand-in TheTVDB on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-tvdb.h"

#include <string.h>

#include "axis-test-meta-fixture.h"
#include "meta/axis-meta-tvdb.h"

#define DIR "tests/fixtures/tvdb/"
#define POSTER "https://artworks.thetvdb.com/banners/posters/81001-1.jpg"

struct _AxisTestTvdb
{
	AxisTestPlex        *api;
	AxisTestPlex        *images;
	gchar               *api_origin;
	gchar               *image_origin;
	AxisMetaProviderInfo info;

	GMutex               lock;
	guint                logins;	/* (lock) */
	guint                refused;	/* (lock) */
	gboolean             refuse;	/* (lock) */
	gchar               *valid;	/* (lock) the token GETs are taken with */
	guint                expire;	/* (lock) */
	gchar               *poster;	/* (lock) nullable */
	gchar               *tmdb;	/* (lock) nullable */
	gboolean             empty;	/* (lock) */
	GHashTable          *answers;	/* (lock) "path?query" or "path" -> JSON */
	guint                gets;	/* (lock) */
};

static gchar *
answer_key(
	const gchar *path,
	const gchar *query
){
	return query != NULL ? g_strconcat(path, "?", query, NULL) : g_strdup(path);
}

void
axis_test_tvdb_set_answer(
	AxisTestTvdb *self,
	const gchar  *path,
	const gchar  *query,
	const gchar  *json
){
	g_mutex_lock(&self->lock);
	if (json != NULL)
		g_hash_table_replace(self->answers, answer_key(path, query), g_strdup(json));
	else {
		g_autofree gchar *key = answer_key(path, query);

		g_hash_table_remove(self->answers, key);
	}
	g_mutex_unlock(&self->lock);
}

guint
axis_test_tvdb_gets(AxisTestTvdb *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->gets;
	g_mutex_unlock(&self->lock);
	return n;
}

void
axis_test_tvdb_empty_order(
	AxisTestTvdb *self,
	gboolean      empty
){
	g_mutex_lock(&self->lock);
	self->empty = empty;
	g_mutex_unlock(&self->lock);
}

static GBytes *
json_bytes(gchar *text)
{
	return g_bytes_new_take(text, strlen(text));
}

/* The fixture a GET of @path (and @query) is answered with, or %NULL. */
static const gchar *
fixture_for(
	const gchar *path,
	const gchar *query
){
	if (strcmp(path, "/v4/search") == 0)
		return "search-series.json";
	if (strcmp(path, "/v4/series/81001/extended") == 0)
		return "series-81001.json";
	if (strcmp(path, "/v4/search/remoteid/tt9000081") == 0)
		return "remoteid-imdb.json";
	if (strcmp(path, "/v4/seasons/types") == 0)
		return "seasons-types.json";
	if (strcmp(path, "/v4/series/81001/episodes/official/eng") == 0 && query != NULL) {
		if (strcmp(query, "page=0") == 0)
			return "episodes-81001-official-0.json";
		if (strcmp(query, "page=1") == 0)
			return "episodes-81001-official-1.json";
		if (strcmp(query, "page=2") == 0)
			return "episodes-81001-official-2.json";
	}
	return NULL;
}

static gchar *
replace(
	gchar       *text,
	const gchar *old,
	const gchar *new_text
){
	gchar **parts = g_strsplit(text, old, -1);
	gchar *joined = g_strjoinv(new_text, parts);

	g_strfreev(parts);
	g_free(text);
	return joined;
}

static void
handle(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestTvdb *self = user_data;
	const gchar *name;
	gchar *text;

	answer->content_type = g_strdup("application/json");
	g_mutex_lock(&self->lock);
	if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/v4/login") == 0) {
		if (self->refuse) {
			self->refused++;
			answer->status = 401;
			answer->body = json_bytes(g_strdup("{\"status\": \"failure\", \"message\": \"Unauthorized\", \"data\": null}"));
		} else {
			self->logins++;
			g_free(self->valid);
			self->valid = g_strdup_printf(AXIS_TEST_TVDB_TOKEN_PREFIX "%u", self->logins);
			answer->status = 200;
			answer->body = json_bytes(g_strdup_printf("{\"status\": \"success\", \"data\": {\"token\": \"%s\"}}",
				self->valid));
		}
		g_mutex_unlock(&self->lock);
		return;
	}
	if (strcmp(request->method, "GET") != 0) {
		g_mutex_unlock(&self->lock);
		answer->status = 405;
		return;
	}
	{
		const gchar *auth = axis_test_plex_request_header(request, "Authorization");
		g_autofree gchar *expected = self->valid != NULL ? g_strconcat("Bearer ", self->valid, NULL) : NULL;

		if (expected == NULL || g_strcmp0(auth, expected) != 0 || self->expire > 0) {
			if (self->expire > 0)
				self->expire--;
			g_mutex_unlock(&self->lock);
			answer->status = 401;
			answer->body = json_bytes(g_strdup("{\"status\": \"failure\", \"message\": \"Unauthorized\"}"));
			return;
		}
	}
	self->gets++;
	{
		g_autofree gchar *exact = answer_key(request->path, request->query);
		const gchar *json = g_hash_table_lookup(self->answers, exact);

		if (json == NULL)
			json = g_hash_table_lookup(self->answers, request->path);
		if (json != NULL) {
			text = g_strdup(json);
			g_mutex_unlock(&self->lock);
			answer->status = 200;
			answer->body = json_bytes(text);
			return;
		}
	}
	name = fixture_for(request->path, request->query);
	if (name != NULL && self->empty && g_str_has_prefix(name, "episodes-")) {
		g_mutex_unlock(&self->lock);
		answer->status = 200;
		answer->body = json_bytes(g_strdup("{\"status\": \"success\", \"data\": {\"series\": {\"id\": 81001},"
			" \"episodes\": []}, \"links\": {\"next\": null}}"));
		return;
	}
	if (name == NULL) {
		g_mutex_unlock(&self->lock);
		answer->status = 404;
		answer->body = json_bytes(g_strdup("{\"status\": \"failure\", \"message\": \"NotFound\", \"data\": null}"));
		return;
	}
	{
		g_autofree gchar *path = g_strconcat(DIR, name, NULL);

		if (!g_file_get_contents(path, &text, NULL, NULL))
			g_error("cannot read %s", path);
	}
	if (strcmp(name, "series-81001.json") == 0) {
		if (self->poster != NULL)
			text = replace(text, POSTER, self->poster);
		if (self->tmdb != NULL)
			text = replace(text, "\"7051\"", self->tmdb);
	}
	g_mutex_unlock(&self->lock);
	answer->status = 200;
	answer->body = json_bytes(text);
}

AxisTestTvdb *
axis_test_tvdb_new(void)
{
	AxisTestTvdb *self = g_new0(AxisTestTvdb, 1);
	g_autoptr(GBytes) png = axis_test_meta_png();

	g_mutex_init(&self->lock);
	self->answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->api = axis_test_plex_new();
	self->images = axis_test_plex_new();
	g_assert_nonnull(self->api);
	g_assert_nonnull(self->images);
	self->api_origin = axis_test_plex_base(self->api);
	self->image_origin = axis_test_plex_base(self->images);
	self->info = *axis_meta_tvdb_get_info();
	self->info.api_origin = self->api_origin;
	self->info.image_origin = self->image_origin;
	axis_test_plex_set_handler(self->api, handle, self);
	axis_test_plex_route_full(self->images, "/banners/posters/81001-1.jpg", 200, "image/png", png, NULL);
	return self;
}

void
axis_test_tvdb_free(AxisTestTvdb *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->api);
	axis_test_plex_free(self->images);
	g_free(self->api_origin);
	g_free(self->image_origin);
	g_free(self->valid);
	g_free(self->poster);
	g_free(self->tmdb);
	g_hash_table_unref(self->answers);
	g_mutex_clear(&self->lock);
	g_free(self);
}

const AxisMetaProviderInfo *
axis_test_tvdb_info(AxisTestTvdb *self)
{
	return &self->info;
}

AxisTestPlex *
axis_test_tvdb_api(AxisTestTvdb *self)
{
	return self->api;
}

AxisTestPlex *
axis_test_tvdb_images(AxisTestTvdb *self)
{
	return self->images;
}

guint
axis_test_tvdb_logins(AxisTestTvdb *self)
{
	guint n;

	g_mutex_lock(&self->lock);
	n = self->logins + self->refused;
	g_mutex_unlock(&self->lock);
	return n;
}

void
axis_test_tvdb_refuse(
	AxisTestTvdb *self,
	gboolean      refuse
){
	g_mutex_lock(&self->lock);
	self->refuse = refuse;
	g_mutex_unlock(&self->lock);
}

void
axis_test_tvdb_expire(
	AxisTestTvdb *self,
	guint         n
){
	g_mutex_lock(&self->lock);
	self->expire = n;
	g_mutex_unlock(&self->lock);
}

void
axis_test_tvdb_set_record(
	AxisTestTvdb *self,
	const gchar  *address,
	const gchar  *tmdb
){
	g_mutex_lock(&self->lock);
	g_free(self->poster);
	g_free(self->tmdb);
	self->poster = g_strdup(address);
	self->tmdb = tmdb != NULL ? g_strdup_printf("\"%s\"", tmdb) : NULL;
	g_mutex_unlock(&self->lock);
}
