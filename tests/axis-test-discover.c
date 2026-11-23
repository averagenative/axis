/*
 * axis-test-discover.c - test support: a fake TMDb for the Discover tests
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-discover.h"

#include <string.h>

#include "axis-test-meta-fixture.h"
#include "meta/axis-meta-tmdb.h"

#define FIXTURES "tests/fixtures/discover/"

/* The page a query asks for ("...&page=2&..."), 1 when it names none. */
static guint
page_of(const gchar *query)
{
	g_auto(GStrv) parts = NULL;
	guint i;

	if (query == NULL)
		return 1;
	parts = g_strsplit(query, "&", -1);
	for (i = 0; parts[i] != NULL; i++) {
		if (g_str_has_prefix(parts[i], "page="))
			return (guint)g_ascii_strtoull(parts[i] + 5, NULL, 10);
	}
	return 1;
}

static gchar *
key_of(
	const gchar *path,
	guint        page
){
	return page == 0 ? g_strdup(path) : g_strdup_printf("%s?page=%u", path, page);
}

/* "/3/movie/popular", page 2: "tests/fixtures/discover/movie-popular-page2.json" */
static gchar *
file_of(
	const gchar *path,
	guint        page
){
	g_autofree gchar *name = g_strdup(g_str_has_prefix(path, "/3/") ? path + 3 : path);
	gchar *p;

	for (p = name; *p != '\0'; p++) {
		if (*p == '/')
			*p = '-';
		else if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			return NULL;
	}
	if (page > 1)
		return g_strdup_printf(FIXTURES "%s-page%u.json", name, page);
	return g_strdup_printf(FIXTURES "%s.json", name);
}

static void
answer_api(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestDiscoverTmdb *self = user_data;
	guint page = page_of(request->query);
	g_autofree gchar *with_page = key_of(request->path, page);
	g_autofree gchar *file = NULL;
	gchar *data = NULL;
	gsize length = 0;
	gpointer status;
	GBytes *body;

	g_mutex_lock(&self->lock);
	status = g_hash_table_lookup(self->status, with_page);
	if (status == NULL)
		status = g_hash_table_lookup(self->status, request->path);
	body = g_hash_table_lookup(self->bodies, with_page);
	if (body == NULL)
		body = g_hash_table_lookup(self->bodies, request->path);
	if (body != NULL)
		body = g_bytes_ref(body);
	g_mutex_unlock(&self->lock);

	if (status != NULL) {
		answer->status = GPOINTER_TO_UINT(status);
		answer->content_type = g_strdup("application/json");
		answer->body = g_bytes_new_static("{}", 2);
		if (answer->status == 429) {
			answer->headers = g_new0(gchar *, 3);
			answer->headers[0] = g_strdup("Retry-After");
			answer->headers[1] = g_strdup("30");
		}
		if (body != NULL)
			g_bytes_unref(body);
		return;
	}
	if (body != NULL) {
		answer->status = 200;
		answer->content_type = g_strdup("application/json;charset=utf-8");
		answer->body = body;
		return;
	}
	file = file_of(request->path, page);
	if (file == NULL || !g_file_get_contents(file, &data, &length, NULL)) {
		answer->status = 404;
		answer->content_type = g_strdup("application/json");
		answer->body = g_bytes_new_static("{\"status_code\":34}", 18);
		return;
	}
	answer->status = 200;
	answer->content_type = g_strdup("application/json;charset=utf-8");
	answer->body = g_bytes_new_take(data, length);
}

static void
answer_image(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	AxisTestDiscoverTmdb *self = user_data;

	answer->status = 200;
	answer->content_type = g_strdup("image/png");
	answer->body = g_bytes_ref(self->png);
}

AxisTestDiscoverTmdb *
axis_test_discover_tmdb_new(void)
{
	AxisTestDiscoverTmdb *self = g_new0(AxisTestDiscoverTmdb, 1);

	g_mutex_init(&self->lock);
	self->status = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
	self->png = axis_test_meta_png();
	self->api = axis_test_plex_new();
	self->images = axis_test_plex_new();
	g_assert_nonnull(self->api);
	g_assert_nonnull(self->images);
	axis_test_plex_set_handler(self->api, answer_api, self);
	axis_test_plex_set_handler(self->images, answer_image, self);
	self->api_origin = axis_test_plex_base(self->api);
	self->image_origin = axis_test_plex_base(self->images);
	self->info = *axis_meta_tmdb_get_info();
	self->info.api_origin = self->api_origin;
	self->info.image_origin = self->image_origin;
	return self;
}

void
axis_test_discover_tmdb_free(AxisTestDiscoverTmdb *self)
{
	if (self == NULL)
		return;
	/* The servers first: their threads read the tables. */
	axis_test_plex_free(self->api);
	axis_test_plex_free(self->images);
	g_hash_table_unref(self->status);
	g_hash_table_unref(self->bodies);
	g_bytes_unref(self->png);
	g_free(self->api_origin);
	g_free(self->image_origin);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_test_discover_tmdb_set_status(
	AxisTestDiscoverTmdb *self,
	const gchar          *path,
	guint                 page,
	guint                 status
){
	g_autofree gchar *key = key_of(path, page);

	g_mutex_lock(&self->lock);
	if (status == 0)
		g_hash_table_remove(self->status, key);
	else
		g_hash_table_replace(self->status, g_steal_pointer(&key), GUINT_TO_POINTER(status));
	g_mutex_unlock(&self->lock);
}

void
axis_test_discover_tmdb_set_body(
	AxisTestDiscoverTmdb *self,
	const gchar          *path,
	guint                 page,
	const gchar          *json
){
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->bodies, key_of(path, page), g_bytes_new(json, strlen(json)));
	g_mutex_unlock(&self->lock);
}

guint
axis_test_discover_tmdb_requests(AxisTestDiscoverTmdb *self)
{
	return axis_test_plex_n_requests(self->api);
}

guint
axis_test_discover_tmdb_count(
	AxisTestDiscoverTmdb *self,
	const gchar          *path
){
	return axis_test_plex_count(self->api, path);
}
