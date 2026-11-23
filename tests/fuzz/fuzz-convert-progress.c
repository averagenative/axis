/*
 * fuzz-convert-progress.c - libFuzzer target for the reader of ffmpeg's
 * `-progress` output and the readers of `ffmpeg -version` and
 * `ffmpeg -encoders`
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the source's duration and frame rate, the second
 * how the rest is cut into pieces; the rest is what ffmpeg wrote.
 * Whatever it is:
 *
 *  - read in pieces of any size it gives what reading it whole gives;
 *  - after every piece each value is "not said" (-1) or within its limits,
 *    the time never goes back, the blocks only count up, a line kept is
 *    never longer than the limit, and a fraction is a number from 0 to 1;
 *  - read as `ffmpeg -version`, the version is %NULL or a short word of the
 *    plain alphabet, the one the text starts with;
 *  - read as `ffmpeg -encoders`, an encoder is listed only when its name is
 *    in the text.
 *
 * The text is read from a buffer of exactly its size, with no terminating
 * zero, as the capability check reads a pipe.
 */

#include <glib.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "transcode/axis-convert.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MAX_TIME_US (AXIS_PROBE_MAX_DURATION_MS * 1000)

static void
check_state(
	const AxisConvertProgress *p,
	gint64                     last_time,
	guint                      last_blocks
){
	gdouble fraction = -2.0;

	if (p->line_length > AXIS_CONVERT_PROGRESS_LINE_MAX)
		abort();
	if (p->frame < -1 || p->out_time_us < -1 || p->total_size < -1 || p->expected_frames < -1 || p->duration_us < -1)
		abort();
	if (p->out_time_us > MAX_TIME_US)
		abort();
	if (!(p->speed == -1.0 || (isfinite(p->speed) && p->speed >= 0.0 && p->speed <= 1000000.0)))
		abort();
	if (p->out_time_us < last_time || p->blocks < last_blocks)
		abort();
	if (axis_convert_progress_fraction(p, &fraction) && (!isfinite(fraction) || fraction < 0.0 || fraction > 1.0))
		abort();
}

static gboolean
same_state(
	const AxisConvertProgress *a,
	const AxisConvertProgress *b
){
	return a->expected_frames == b->expected_frames && a->duration_us == b->duration_us && a->frame == b->frame &&
		a->out_time_us == b->out_time_us && a->total_size == b->total_size &&
		memcmp(&a->speed, &b->speed, sizeof(a->speed)) == 0 && a->ended == b->ended && a->blocks == b->blocks &&
		a->line_length == b->line_length && a->line_too_long == b->line_too_long &&
		memcmp(a->line, b->line, a->line_length) == 0;
}

static void
check_tool_output(
	const gchar *text,
	gsize        length
){
	static const gchar *const names[] = {
		"libx265", "libx264", "hevc_nvenc", "h264_nvenc", "hevc_vaapi", "h264_vaapi", "aac", "mov_text", "srt",
		"subrip", "x", NULL
	};
	gchar *version = axis_convert_parse_version(text, length);
	guint i;

	if (version != NULL) {
		static const gchar head[] = "ffmpeg version ";
		gsize n = strlen(version);
		const gchar *p;

		if (n == 0 || n >= 64 || length < sizeof(head) - 1 + n || memcmp(text, head, sizeof(head) - 1) != 0 ||
		    memcmp(text + sizeof(head) - 1, version, n) != 0)
			abort();
		for (p = version; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && strchr(".-+_~", *p) == NULL)
				abort();
		}
		g_free(version);
	}
	for (i = 0; names[i] != NULL; i++) {
		gsize n = strlen(names[i]);
		gboolean found = FALSE;
		gsize at;

		if (!axis_convert_encoders_lists(text, length, names[i]))
			continue;
		for (at = 0; !found && at + n <= length; at++)
			found = memcmp(text + at, names[i], n) == 0;
		if (!found)
			abort();
	}
	if (axis_convert_encoders_lists(text, length, "") || axis_convert_encoders_lists(text, length, NULL) ||
	    axis_convert_encoders_lists(NULL, length, "libx265"))
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const gint64 durations[] = { -1, 0, 1, 1000, 5400000, AXIS_PROBE_MAX_DURATION_MS,
		AXIS_PROBE_MAX_DURATION_MS + 1, G_MAXINT64 };
	static const gdouble rates[] = { -1.0, 0.0, 24000.0 / 1001.0, 25.0, 60.0, 1e-9, 1000.0, 1000.5 };
	AxisConvertProgress whole;
	AxisConvertProgress pieces;
	gchar *text;
	gsize length;
	gsize at;
	guint8 choice;
	guint8 cut;
	gint64 last_time;
	guint last_blocks;

	if (size < 2 || size > 64 * 1024)
		return 0;
	choice = data[0];
	cut = data[1];
	length = size - 2;
	/* Exactly the bytes, no terminating zero. */
	text = g_malloc(length > 0 ? length : 1);
	memcpy(text, data + 2, length);

	axis_convert_progress_init(&whole, durations[choice % G_N_ELEMENTS(durations)],
		rates[(choice / G_N_ELEMENTS(durations)) % G_N_ELEMENTS(rates)]);
	pieces = whole;
	check_state(&whole, -1, 0);

	axis_convert_progress_feed(&whole, text, length);
	check_state(&whole, -1, 0);

	/* The same text in pieces of 1 to 32 bytes, the sizes drawn from the
	 * text itself. */
	last_time = pieces.out_time_us;
	last_blocks = pieces.blocks;
	for (at = 0; at < length;) {
		gsize piece = 1 + (gsize)((cut ^ (guint8)text[at]) % 32);

		if (piece > length - at)
			piece = length - at;
		axis_convert_progress_feed(&pieces, text + at, piece);
		check_state(&pieces, last_time, last_blocks);
		last_time = pieces.out_time_us;
		last_blocks = pieces.blocks;
		at += piece;
	}
	axis_convert_progress_feed(&pieces, text, 0);
	axis_convert_progress_feed(&pieces, NULL, 0);
	if (!same_state(&whole, &pieces))
		abort();

	check_tool_output(text, length);
	g_free(text);
	return 0;
}
