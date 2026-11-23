/*
 * axis-web-assets.c - static assets compiled into the binary
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * data/web is a GResource (see the Makefile).  Its files are served at
 * /static/<version>/<path>, where <version> is a hash of all of them
 * computed at build time, so a browser can keep them for ever: a changed
 * asset has a new URL.  A request with any other version is sent to the
 * current URL.
 */

#include "web/axis-web-private.h"
#include "web/axis-web-path.h"

#include <string.h>

#include "axis-web-assets-version.h"
#include "axis-web-resource.h"

#define RESOURCE_PREFIX "/axis/web/"
#define ETAG_HEX_LEN    16

G_STATIC_ASSERT(sizeof(AXIS_WEB_ASSET_VERSION) == AXIS_WEB_VERSION_LEN + 1);

static const struct
{
	const gchar *extension;
	const gchar *content_type;
} content_types[] = {
	{ ".js",    "text/javascript; charset=utf-8" },
	{ ".css",   "text/css; charset=utf-8" },
	{ ".svg",   "image/svg+xml" },
	{ ".png",   "image/png" },
	{ ".ico",   "image/x-icon" },
	{ ".woff2", "font/woff2" },
	{ ".webmanifest", "application/manifest+json" },
	{ NULL, NULL }
};

/* Answers only on the main thread, where route handlers run. */
static GHashTable *etags;

const gchar *
axis_web_asset_version(void)
{
	return AXIS_WEB_ASSET_VERSION;
}

gchar *
axis_web_asset_url(const gchar *asset)
{
	g_return_val_if_fail(asset != NULL, NULL);

	return g_strconcat(AXIS_WEB_STATIC_PREFIX, AXIS_WEB_ASSET_VERSION, "/", asset, NULL);
}

static GResource *
resource_get(void)
{
	static gsize registered = 0;

	if (g_once_init_enter(&registered)) {
		axis_web_assets_register_resource();
		g_once_init_leave(&registered, 1);
	}

	return axis_web_assets_get_resource();
}

static const gchar *
content_type_for(const gchar *asset)
{
	guint i;

	for (i = 0; content_types[i].extension != NULL; i++) {
		if (g_str_has_suffix(asset, content_types[i].extension))
			return content_types[i].content_type;
	}

	return NULL;
}

/* The entity tag of @asset: a strong tag from the hash of its content. */
static const gchar *
etag_for(
	const gchar *asset,
	GBytes      *data
){
	const gchar *etag;

	if (etags == NULL)
		etags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	etag = g_hash_table_lookup(etags, asset);
	if (etag == NULL) {
		g_autofree gchar *digest = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);

		digest[ETAG_HEX_LEN] = '\0';
		g_hash_table_insert(etags, g_strdup(asset), g_strdup_printf("\"%s\"", digest));
		etag = g_hash_table_lookup(etags, asset);
	}

	return etag;
}

/* Whether an If-None-Match header value names @etag (or is "*"). */
static gboolean
if_none_match_hits(
	const gchar *header,
	const gchar *etag
){
	g_auto(GStrv) items = NULL;
	guint i;

	if (header == NULL)
		return FALSE;

	items = g_strsplit(header, ",", -1);
	for (i = 0; items[i] != NULL; i++) {
		const gchar *item = g_strstrip(items[i]);

		if (g_str_has_prefix(item, "W/"))
			item += 2;
		if (strcmp(item, "*") == 0 || strcmp(item, etag) == 0)
			return TRUE;
	}

	return FALSE;
}

/* The answer carrying @data, or 304 if the client already has it. */
static HtmxResponse *
asset_response(
	HtmxRequest *request,
	const gchar *asset,
	GBytes      *data,
	const gchar *content_type,
	const gchar *cache_control
){
	HtmxResponse *response = htmx_response_new();
	SoupServerMessage *message;
	const gchar *etag = etag_for(asset, data);
	const gchar *condition = NULL;

	message = htmx_request_get_message(request);
	if (message != NULL)
		condition = soup_message_headers_get_one(
			soup_server_message_get_request_headers(message), "If-None-Match");

	htmx_response_add_header(response, "ETag", etag);
	htmx_response_add_header(response, "Cache-Control", cache_control);
	axis_web_keep_cache_policy(response);

	if (if_none_match_hits(condition, etag)) {
		htmx_response_set_status(response, 304);
	} else {
		htmx_response_set_bytes(response, data);
		htmx_response_set_content_type(response, content_type);
	}

	return response;
}

HtmxResponse *
axis_web_static_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	g_autofree gchar *version = NULL;
	g_autofree gchar *asset = NULL;
	g_autofree gchar *resource_path = NULL;
	g_autoptr(GBytes) data = NULL;
	g_autoptr(HtmxResponse) response = NULL;
	const gchar *content_type;

	if (!axis_web_static_path_parse(htmx_request_get_path(request), &version, &asset))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");

	resource_path = g_strconcat(RESOURCE_PREFIX, asset, NULL);
	data = g_resource_lookup_data(resource_get(), resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	content_type = content_type_for(asset);
	if (data == NULL || content_type == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");

	/* A stale version: the same asset under the current one. */
	if (strcmp(version, AXIS_WEB_ASSET_VERSION) != 0) {
		g_autofree gchar *url = axis_web_asset_url(asset);

		response = htmx_response_new();
		htmx_response_set_status(response, 302);
		htmx_response_add_header(response, "Location", url);
		return (HtmxResponse *)g_steal_pointer(&response);
	}

	return asset_response(request, asset, data, content_type,
		"public, max-age=31536000, immutable");
}

/*
 * The icons browsers and phones ask for at the site root, without a
 * session and without reading any page: the answer is the icon itself
 * (not a redirect, which some clients do not follow for icons).  Their
 * URLs carry no version, so they may be kept for a day only.
 */
static const struct
{
	const gchar *path;
	const gchar *asset;
} well_known_icons[] = {
	{ AXIS_WEB_FAVICON_PATH,              "icons/favicon.ico" },
	{ AXIS_WEB_TOUCH_ICON_PATH,           "icons/apple-touch-icon.png" },
	{ AXIS_WEB_TOUCH_ICON_PRECOMPOSED_PATH, "icons/apple-touch-icon.png" },
	{ NULL, NULL }
};

HtmxResponse *
axis_web_icon_handle(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *path = htmx_request_get_path(request);
	g_autofree gchar *resource_path = NULL;
	g_autoptr(GBytes) data = NULL;
	const gchar *asset = NULL;
	guint i;

	for (i = 0; path != NULL && well_known_icons[i].path != NULL; i++) {
		if (strcmp(path, well_known_icons[i].path) == 0)
			asset = well_known_icons[i].asset;
	}
	if (asset == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");

	resource_path = g_strconcat(RESOURCE_PREFIX, asset, NULL);
	data = g_resource_lookup_data(resource_get(), resource_path, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	if (data == NULL)
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");

	return asset_response(request, asset, data, content_type_for(asset), "public, max-age=86400");
}

void
axis_web_icons_register(AxisWeb *web)
{
	guint i;

	for (i = 0; well_known_icons[i].path != NULL; i++)
		axis_web_route_full(web, HTMX_METHOD_GET, well_known_icons[i].path,
			AXIS_WEB_ROUTE_PUBLIC, axis_web_icon_handle, web);
}
