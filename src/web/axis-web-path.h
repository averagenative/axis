/*
 * axis-web-path.h - strict parser for static asset URLs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Depends on GLib only, so that the fuzz target can build it alone.
 */

#ifndef AXIS_WEB_PATH_H
#define AXIS_WEB_PATH_H

#include <glib.h>

G_BEGIN_DECLS

/* Every asset URL starts with this. */
#define AXIS_WEB_STATIC_PREFIX "/static/"

/* The version segment is this many lower-case hexadecimal digits. */
#define AXIS_WEB_VERSION_LEN 12

/* Longest accepted asset path, and the most segments in it. */
#define AXIS_WEB_ASSET_PATH_MAX     200
#define AXIS_WEB_ASSET_SEGMENTS_MAX 6

/**
 * axis_web_static_path_parse:
 * @path: a request path, exactly as received (not decoded)
 * @version: (out) (optional) (transfer full): the version segment
 * @asset: (out) (optional) (transfer full): the asset path after it,
 *   such as "vendor/htmx.min.js"
 *
 * Accepts only "/static/" + version + "/" + asset, where the version is
 * AXIS_WEB_VERSION_LEN lower-case hexadecimal digits and the asset is
 * one to AXIS_WEB_ASSET_SEGMENTS_MAX segments of letters, digits, '-',
 * '_' and '.', none empty, none starting with '.', joined by single '/'.
 *
 * Everything else is refused, and that includes any '%' (so an encoded
 * "..", in any number of layers, never reaches a lookup), any '\\', any
 * control character, a trailing '/', and "//".  The outputs are set only
 * on success.
 *
 * Returns: %TRUE if @path is acceptable
 */
gboolean
axis_web_static_path_parse(
	const gchar  *path,
	gchar       **version,
	gchar       **asset
);

G_END_DECLS

#endif /* AXIS_WEB_PATH_H */
