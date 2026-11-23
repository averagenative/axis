/*
 * axis-indexer-shipped.c - the site definitions Axis ships
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-shipped.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "axis-definitions-resource.h"
#include "core/axis-http-origin.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"

#define RESOURCE_DIR "/axis/definitions/"

static GPtrArray *shipped;	/* AxisIndexerShipped, sorted by id */

static gint
by_id(
	gconstpointer a,
	gconstpointer b
){
	const AxisIndexerShipped *x = *(const AxisIndexerShipped *const *)a;
	const AxisIndexerShipped *y = *(const AxisIndexerShipped *const *)b;

	return strcmp(x->def_id, y->def_id);
}

/* Every file of the resource that reads as a usable definition with a
 * version and the id its name gives. */
static GPtrArray *
load_all(void)
{
	GPtrArray *all = g_ptr_array_new();
	GResource *resource;
	g_auto(GStrv) children = NULL;
	guint i;

	axis_definitions_register_resource();
	resource = axis_definitions_get_resource();
	children = g_resource_enumerate_children(resource, RESOURCE_DIR, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	for (i = 0; children != NULL && children[i] != NULL; i++) {
		g_autofree gchar *path = g_strconcat(RESOURCE_DIR, children[i], NULL);
		g_autoptr(GBytes) bytes = g_resource_lookup_data(resource, path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
		g_autofree gchar *expected = g_strndup(children[i], strlen(children[i]) > 4 ? strlen(children[i]) - 4 : 0);
		g_autoptr(AxisDef) def = NULL;
		g_autofree gchar *problem = NULL;
		AxisIndexerShipped *one;
		gconstpointer data;
		gsize length = 0;

		if (bytes == NULL)
			continue;
		data = g_bytes_get_data(bytes, &length);
		def = axis_def_load(data, length, &problem);
		if (def == NULL || !axis_def_usable(def) || def->version < 1 || strcmp(def->id, expected) != 0) {
			g_autofree gchar *why = def != NULL ? axis_def_problem(def) : NULL;

			g_warning("indexers: the built-in definition %s is left out: %s", children[i],
				def == NULL ? problem : !axis_def_usable(def) ? why : def->version < 1 ? "it states no version" :
				"its id is not its file's name");
			continue;
		}
		one = g_new0(AxisIndexerShipped, 1);
		one->def_id = g_strdup(def->id);
		one->name = g_strdup(def->name);
		one->description = g_strdup(def->description);
		one->version = def->version;
		one->body = g_strndup(data, length);
		one->length = length;
		one->sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
		g_ptr_array_add(all, one);
	}
	g_ptr_array_sort(all, by_id);
	return all;
}

static GPtrArray *
get_all(void)
{
	static gsize loaded = 0;

	if (g_once_init_enter(&loaded)) {
		shipped = load_all();
		g_once_init_leave(&loaded, 1);
	}
	return shipped;
}

guint
axis_indexer_shipped_count(void)
{
	return get_all()->len;
}

const AxisIndexerShipped *
axis_indexer_shipped_at(guint index)
{
	GPtrArray *all = get_all();

	return index < all->len ? g_ptr_array_index(all, index) : NULL;
}

const AxisIndexerShipped *
axis_indexer_shipped_find(const gchar *def_id)
{
	GPtrArray *all = get_all();
	guint i;

	for (i = 0; def_id != NULL && i < all->len; i++) {
		const AxisIndexerShipped *one = g_ptr_array_index(all, i);

		if (strcmp(one->def_id, def_id) == 0)
			return one;
	}
	return NULL;
}

gboolean
axis_indexer_shipped_reserved(const gchar *def_id)
{
	return def_id != NULL && g_ascii_strncasecmp(def_id, AXIS_INDEXER_SHIPPED_PREFIX,
		strlen(AXIS_INDEXER_SHIPPED_PREFIX)) == 0;
}

gchar *
axis_indexer_shipped_override_id(const gchar *def_id)
{
	return g_strconcat(def_id, AXIS_INDEXER_OVERRIDE_SUFFIX, NULL);
}

AxisIndexerShippedChoice
axis_indexer_shipped_choose(
	const AxisIndexerShipped  *shipped_def,
	const gchar               *override,
	gint                      *override_version,
	gchar                    **why
){
	g_autoptr(AxisDef) def = NULL;

	if (override_version != NULL)
		*override_version = 0;
	if (why != NULL)
		*why = NULL;
	if (override == NULL)
		return AXIS_INDEXER_SHIPPED_BUILT_IN;
	def = axis_def_load(override, strlen(override), NULL);
	if (def != NULL && override_version != NULL)
		*override_version = def->version;
	if (def == NULL || !axis_def_usable(def)) {
		if (why != NULL)
			*why = g_strdup("This version of Axis cannot run it.");
		return AXIS_INDEXER_SHIPPED_IGNORED;
	}
	if (def->version <= shipped_def->version) {
		if (why != NULL)
			*why = g_strdup_printf("It is version %d, and this version of Axis has version %d built in, which is "
				"as new or newer.", def->version, shipped_def->version);
		return AXIS_INDEXER_SHIPPED_IGNORED;
	}
	return AXIS_INDEXER_SHIPPED_OVERRIDDEN;
}

/* Whether the engine builds the requests of a movie, an episode and a test
 * against @def's first link (asking nothing). */
static gboolean
builds_requests(
	const AxisDef  *def,
	gchar         **problem
){
	g_autoptr(GUri) uri = g_uri_parse(g_ptr_array_index(def->links, 0), G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *origin_text = NULL;
	g_autofree gchar *base_path = NULL;
	AxisIndexerRequest requests[3];
	AxisHttpOrigin origin;
	AxisDefSite site;
	guint i;

	if (uri == NULL || g_uri_get_host(uri) == NULL) {
		*problem = g_strdup("Its first address cannot be read.");
		return FALSE;
	}
	origin_text = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri),
		g_uri_get_port(uri)) : g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	if (!axis_http_origin_parse(origin_text, &origin, NULL)) {
		*problem = g_strdup("Its first address is not one Axis can ask.");
		return FALSE;
	}
	base_path = g_strdup(g_uri_get_path(uri));
	while (base_path[0] != '\0' && base_path[strlen(base_path) - 1] == '/')
		base_path[strlen(base_path) - 1] = '\0';
	memset(&site, 0, sizeof(site));
	site.def = def;
	site.origin = &origin;
	site.base_path = base_path;
	site.now_us = g_get_real_time();
	axis_indexer_request_init(&requests[0], AXIS_INDEXER_QUERY_FILM);
	requests[0].title = "Made Up Film";
	requests[0].year = 2001;
	requests[0].imdb_id = "tt0000123";
	axis_indexer_request_init(&requests[1], AXIS_INDEXER_QUERY_EPISODE);
	requests[1].title = "Made Up Show";
	requests[1].season = 1;
	requests[1].episode = 2;
	axis_indexer_request_init(&requests[2], AXIS_INDEXER_QUERY_TEXT);
	for (i = 0; i < G_N_ELEMENTS(requests); i++) {
		g_autofree gchar *why = NULL;
		AxisDefSearch *search;

		if ((i == 0 && !def->covers_movies && !def->general) || (i == 1 && !def->covers_tv && !def->general))
			continue;
		search = axis_def_search_new(&site, &requests[i], &why);
		if (search == NULL) {
			*problem = g_strdup_printf("Its %s cannot be built: %s.", i == 0 ? "movie search" : i == 1 ?
				"episode search" : "test", why != NULL ? why : "it builds no request");
			axis_http_origin_clear(&origin);
			return FALSE;
		}
		axis_def_search_free(search);
	}
	axis_http_origin_clear(&origin);
	return TRUE;
}

gboolean
axis_indexer_shipped_check_override(
	const AxisIndexerShipped  *shipped_def,
	const gchar               *data,
	gsize                      length,
	gchar                    **problem
){
	g_autoptr(AxisDef) def = NULL;
	g_autofree gchar *why = NULL;

	if (problem != NULL)
		*problem = NULL;
	def = axis_def_load(data, length, &why);
	if (def == NULL) {
		if (problem != NULL)
			*problem = g_strdup_printf("It is not a definition: %s", why != NULL ? why : "it cannot be read.");
		return FALSE;
	}
	if (strcmp(def->id, shipped_def->def_id) != 0) {
		if (problem != NULL)
			*problem = g_strdup_printf("Its id is not %s.", shipped_def->def_id);
		return FALSE;
	}
	if (def->version <= shipped_def->version) {
		if (problem != NULL)
			*problem = g_strdup_printf("This version of Axis has version %d of %s built in: an override must say a "
				"later version, such as \"version: %d\" at its top.", shipped_def->version, shipped_def->name,
				shipped_def->version + 1);
		return FALSE;
	}
	if (!axis_def_usable(def)) {
		g_autofree gchar *needs = axis_def_problem(def);

		if (problem != NULL)
			*problem = g_strdup_printf("This version of Axis cannot run it: %s", needs);
		return FALSE;
	}
	g_clear_pointer(&why, g_free);
	if (!builds_requests(def, &why)) {
		if (problem != NULL)
			*problem = g_steal_pointer(&why);
		return FALSE;
	}
	return TRUE;
}

gchar *
axis_indexer_read_file_at_most(
	const gchar  *path,
	gsize         max_bytes,
	gsize        *length,
	GError      **error
){
	struct stat st;
	gchar *data;
	gsize got = 0;
	gint fd;

	*length = 0;
	fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY, 0);
	if (fd < 0) {
		gint saved = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved), "%s cannot be opened: %s", path,
			g_strerror(saved));
		return NULL;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s is not a file", path);
		close(fd);
		return NULL;
	}
	if (st.st_size < 0 || (guint64)st.st_size > max_bytes) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s is larger than %" G_GSIZE_FORMAT " KiB", path,
			max_bytes / 1024);
		close(fd);
		return NULL;
	}
	/* Its size, and one byte more: a file that grew is seen. */
	max_bytes = (gsize)st.st_size;
	data = g_malloc(max_bytes + 2);
	while (got <= max_bytes) {
		gssize n = read(fd, data + got, max_bytes + 1 - got);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			gint saved = errno;

			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved), "%s cannot be read: %s", path,
				g_strerror(saved));
			g_free(data);
			close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		got += (gsize)n;
	}
	close(fd);
	if (got > max_bytes) {
		g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s changed while it was read", path);
		g_free(data);
		return NULL;
	}
	data[got] = '\0';
	*length = got;
	return data;
}
