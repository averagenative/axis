/*
 * fuzz-web-static-path.c - libFuzzer target for the static asset path
 * parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Built with clang -fsanitize=fuzzer,address,undefined by `make fuzz`.
 * Besides memory safety, it checks what the parser promises about every
 * path it accepts: a version of exactly twelve lower-case hex digits; an
 * asset of one to six non-empty segments of letters, digits, '-', '_' and
 * '.', none starting with '.', so it can hold no "..", no '%' and no
 * '\\'; and putting the pieces back together gives the input exactly.
 * Whatever it refuses leaves the outputs alone.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web/axis-web-path.h"

static void
check(const gchar *text)
{
	gchar *version = NULL;
	gchar *asset = NULL;
	gchar *again;
	const gchar *p;
	guint segments = 1;
	guint segment_len = 0;
	guint i;

	if (!axis_web_static_path_parse(text, &version, &asset)) {
		if (version != NULL || asset != NULL)
			abort();
		return;
	}

	if (version == NULL || asset == NULL)
		abort();
	if (strlen(version) != AXIS_WEB_VERSION_LEN)
		abort();
	for (i = 0; version[i] != '\0'; i++) {
		if (!((version[i] >= '0' && version[i] <= '9') ||
		      (version[i] >= 'a' && version[i] <= 'f')))
			abort();
	}

	if (asset[0] == '\0' || strlen(asset) > AXIS_WEB_ASSET_PATH_MAX)
		abort();
	for (p = asset; ; p++) {
		if (*p == '/' || *p == '\0') {
			if (segment_len == 0)
				abort();	/* empty segment */
			segment_len = 0;
			if (*p == '\0')
				break;
			segments++;
			continue;
		}
		if (!(g_ascii_isalnum(*p) || *p == '-' || *p == '_' || *p == '.'))
			abort();
		if (segment_len == 0 && *p == '.')
			abort();	/* ".", ".." or a hidden file */
		segment_len++;
	}
	if (segments > AXIS_WEB_ASSET_SEGMENTS_MAX)
		abort();
	if (strstr(asset, "/.") != NULL || asset[0] == '.')
		abort();
	if (strchr(asset, '%') != NULL || strchr(asset, '\\') != NULL)
		abort();

	again = g_strconcat(AXIS_WEB_STATIC_PREFIX, version, "/", asset, NULL);
	if (strcmp(again, text) != 0)
		abort();

	g_free(again);
	g_free(version);
	g_free(asset);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* The parser takes a C string; an embedded NUL ends the input. */
	g_autofree gchar *text = g_strndup((const gchar *)data, size);

	check(text);

	return 0;
}
