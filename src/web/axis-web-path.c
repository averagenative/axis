/*
 * axis-web-path.c - strict parser for static asset URLs
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-path.h"

#include <string.h>

static gboolean
is_hex_digit(gchar c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static gboolean
is_segment_char(gchar c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}

gboolean
axis_web_static_path_parse(
	const gchar  *path,
	gchar       **version,
	gchar       **asset
){
	const gchar *p;
	const gchar *asset_start;
	guint segments = 0;
	guint segment_len = 0;
	guint i;

	g_return_val_if_fail(path != NULL, FALSE);

	if (!g_str_has_prefix(path, AXIS_WEB_STATIC_PREFIX))
		return FALSE;
	p = path + strlen(AXIS_WEB_STATIC_PREFIX);

	for (i = 0; i < AXIS_WEB_VERSION_LEN; i++) {
		if (!is_hex_digit(p[i]))
			return FALSE;
	}
	if (p[AXIS_WEB_VERSION_LEN] != '/')
		return FALSE;

	asset_start = p + AXIS_WEB_VERSION_LEN + 1;
	if (*asset_start == '\0' || strlen(asset_start) > AXIS_WEB_ASSET_PATH_MAX)
		return FALSE;

	for (p = asset_start; ; p++) {
		if (*p == '/' || *p == '\0') {
			if (segment_len == 0)
				return FALSE;	/* empty: "//" or a trailing "/" */
			segments++;
			segment_len = 0;
			if (*p == '\0')
				break;
			continue;
		}

		if (!is_segment_char(*p))
			return FALSE;
		/* No hidden files, ".", or "..": a segment never starts with '.'. */
		if (segment_len == 0 && *p == '.')
			return FALSE;
		segment_len++;
	}

	if (segments > AXIS_WEB_ASSET_SEGMENTS_MAX)
		return FALSE;

	if (version != NULL)
		*version = g_strndup(path + strlen(AXIS_WEB_STATIC_PREFIX), AXIS_WEB_VERSION_LEN);
	if (asset != NULL)
		*asset = g_strdup(asset_start);

	return TRUE;
}
