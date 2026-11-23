/*
 * fuzz-setting-byte-size.c - libFuzzer target for the byte size parser of
 * the Settings page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Besides memory safety it checks that a size the parser accepts is never
 * negative, is written back in a form it reads as the same number of
 * bytes, and that form is itself stable.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/axis-setting-units.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = g_strndup((const gchar *)data, size);
	gint64 bytes = -1;
	gint64 again = -1;
	gchar *written;
	gchar *rewritten;

	if (!axis_setting_parse_byte_size(text, &bytes))
		return 0;

	if (bytes < 0)
		abort();

	written = axis_setting_format_byte_size(bytes);
	if (!axis_setting_parse_byte_size(written, &again) || again != bytes)
		abort();
	rewritten = axis_setting_format_byte_size(again);
	if (strcmp(written, rewritten) != 0)
		abort();

	g_free(written);
	g_free(rewritten);
	return 0;
}
