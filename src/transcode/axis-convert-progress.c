/*
 * axis-convert-progress.c - ffmpeg's `-progress` output: key=value lines
 * in blocks that end with "progress=continue" or "progress=end"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What the two versions print was recorded (design.md): "frame", "fps",
 * "stream_0_0_q", "bitrate", "total_size", "out_time_us", "out_time_ms"
 * (also microseconds), "out_time", "dup_frames", "drop_frames", "speed",
 * "progress", in that order, the same keys in 7.1 and 8.1.  "out_time_us"
 * is "N/A" while a mapped attachment or sparse stream has no time yet, and
 * goes back at the end when a subtitle stream ends early; "speed" is "N/A"
 * or padded with spaces ("   2x"); "total_size" stays put while the muxer
 * holds packets to interleave.  So every value is optional, only the frame
 * count is taken as it comes, the time only ever moves forward, and
 * nothing is divided by a duration that is not known.
 */

#include "transcode/axis-convert.h"

#include <math.h>
#include <string.h>

#define MAX_FRAMES       G_GINT64_CONSTANT(100000000000)
#define MAX_TIME_US      (AXIS_PROBE_MAX_DURATION_MS * 1000)
#define MAX_SIZE         G_GINT64_CONSTANT(1000000000000000)
#define MAX_SPEED        1000000.0

static void
reset_pending(AxisConvertProgress *p)
{
	p->pending_frame = -1;
	p->pending_time_us = -1;
	p->pending_size = -1;
	p->pending_speed = -1.0;
}

void
axis_convert_progress_init(
	AxisConvertProgress *p,
	gint64               duration_ms,
	gdouble              frame_rate
){
	memset(p, 0, sizeof(*p));
	p->expected_frames = -1;
	p->duration_us = -1;
	p->frame = -1;
	p->out_time_us = -1;
	p->total_size = -1;
	p->speed = -1.0;
	reset_pending(p);
	if (duration_ms > 0 && duration_ms <= AXIS_PROBE_MAX_DURATION_MS) {
		p->duration_us = duration_ms * 1000;
		if (isfinite(frame_rate) && frame_rate > 0.0 && frame_rate <= 1000.0) {
			gdouble frames = (gdouble)duration_ms / 1000.0 * frame_rate;

			if (frames >= 1.0 && frames <= (gdouble)MAX_FRAMES)
				p->expected_frames = (gint64)(frames + 0.5);
		}
	}
}

/* A whole number of decimal digits only, at most @maximum; -1 otherwise
 * ("N/A", a sign, anything else). */
static gint64
whole(
	const gchar *text,
	gint64       maximum
){
	gint64 value = 0;
	const gchar *p;

	if (text[0] == '\0')
		return -1;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return -1;
		if (value > (maximum - (*p - '0')) / 10)
			return -1;
		value = value * 10 + (*p - '0');
	}
	return value;
}

/* "5.29x", "   2x": the number before the x; -1 otherwise. */
static gdouble
speed_of(const gchar *text)
{
	gchar buffer[32];
	gchar *end = NULL;
	gsize length;
	gdouble value;
	gsize i;

	while (*text == ' ')
		text++;
	length = strlen(text);
	if (length < 2 || length >= sizeof(buffer) || text[length - 1] != 'x')
		return -1.0;
	for (i = 0; i + 1 < length; i++) {
		if (!g_ascii_isdigit(text[i]) && text[i] != '.')
			return -1.0;
	}
	memcpy(buffer, text, length - 1);
	buffer[length - 1] = '\0';
	value = g_ascii_strtod(buffer, &end);
	if (end == buffer || *end != '\0' || !isfinite(value) || value < 0.0 || value > MAX_SPEED)
		return -1.0;
	return value;
}

static void
read_line(
	AxisConvertProgress *p,
	gchar               *line
){
	gchar *eq = strchr(line, '=');
	const gchar *value;

	if (eq == NULL)
		return;
	*eq = '\0';
	value = eq + 1;
	if (strcmp(line, "frame") == 0) {
		p->pending_frame = whole(value, MAX_FRAMES);
	} else if (strcmp(line, "out_time_us") == 0) {
		p->pending_time_us = whole(value, MAX_TIME_US);
	} else if (strcmp(line, "total_size") == 0) {
		p->pending_size = whole(value, MAX_SIZE);
	} else if (strcmp(line, "speed") == 0) {
		p->pending_speed = speed_of(value);
	} else if (strcmp(line, "progress") == 0) {
		/* The block is complete: what it said is taken, what it did not
		 * say stays as it was. */
		if (p->pending_frame >= 0)
			p->frame = p->pending_frame;
		if (p->pending_time_us >= 0 && p->pending_time_us > p->out_time_us)
			p->out_time_us = p->pending_time_us;
		if (p->pending_size >= 0)
			p->total_size = p->pending_size;
		if (p->pending_speed >= 0.0)
			p->speed = p->pending_speed;
		if (p->blocks < G_MAXUINT)
			p->blocks++;
		if (strcmp(value, "end") == 0)
			p->ended = TRUE;
		reset_pending(p);
	}
}

void
axis_convert_progress_feed(
	AxisConvertProgress *p,
	const gchar         *data,
	gsize                length
){
	gsize i;

	g_return_if_fail(p != NULL);

	for (i = 0; data != NULL && i < length; i++) {
		gchar c = data[i];

		if (c == '\n') {
			if (!p->line_too_long) {
				/* A line may end in "\r\n". */
				if (p->line_length > 0 && p->line[p->line_length - 1] == '\r')
					p->line_length--;
				p->line[p->line_length] = '\0';
				read_line(p, p->line);
			}
			p->line_length = 0;
			p->line_too_long = FALSE;
			continue;
		}
		if (p->line_too_long)
			continue;
		/* A NUL or a line that runs on is not a line of ffmpeg's. */
		if (c == '\0' || p->line_length >= AXIS_CONVERT_PROGRESS_LINE_MAX) {
			p->line_too_long = TRUE;
			continue;
		}
		p->line[p->line_length++] = c;
	}
}

gboolean
axis_convert_progress_fraction(
	const AxisConvertProgress *p,
	gdouble                   *fraction
){
	gdouble value;

	g_return_val_if_fail(p != NULL && fraction != NULL, FALSE);

	if (p->expected_frames > 0 && p->frame >= 0)
		value = (gdouble)p->frame / (gdouble)p->expected_frames;
	else if (p->duration_us > 0 && p->out_time_us >= 0)
		value = (gdouble)p->out_time_us / (gdouble)p->duration_us;
	else
		return FALSE;
	if (!isfinite(value))
		return FALSE;
	*fraction = CLAMP(value, 0.0, 1.0);
	return TRUE;
}
