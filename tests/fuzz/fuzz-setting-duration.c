/*
 * fuzz-setting-duration.c - libFuzzer target for the duration parser of the
 * Settings page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Besides memory safety it checks what makes the parser safe to build on: a
 * duration it accepts is never negative, is written back in a form it
 * reads as the same number of seconds, and that form is itself stable.
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
	gint64 seconds = -1;
	gint64 again = -1;
	gchar *written;
	gchar *rewritten;

	if (!axis_setting_parse_duration(text, &seconds))
		return 0;

	if (seconds < 0)
		abort();

	written = axis_setting_format_duration(seconds);
	if (!axis_setting_parse_duration(written, &again) || again != seconds)
		abort();
	rewritten = axis_setting_format_duration(again);
	if (strcmp(written, rewritten) != 0)
		abort();

	g_free(written);
	g_free(rewritten);
	return 0;
}
