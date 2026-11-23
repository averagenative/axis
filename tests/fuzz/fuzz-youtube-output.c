/*
 * fuzz-youtube-output.c - libFuzzer target for the reading of what yt-dlp
 * writes (youtube/axis-youtube-output.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' standard output as it arrives,
 * cut into lines and each read as a progress line; '1' the record of a video;
 * '2' standard error, for why it failed; '3' the answer to --version); the
 * rest is the input.  Whatever it is, the reader returns without crashing or
 * allocating without bound, and:
 *
 *  - a line handed over is 1 to AXIS_YOUTUBE_LINE_MAX bytes with no line end
 *    in it; a progress line's figures are -1 or within their limits, and no
 *    more is done than the whole; the sum of the lines so far (a video and
 *    its audio are two streams) stays within the limit however many lines
 *    say the largest figure;
 *  - a record that is taken has a title and a channel that are clean (valid
 *    UTF-8, no control character, within their limits), a video id of the
 *    right form unless it is refused, a channel id of letters, digits, '-'
 *    and '_', a date that is one or none, and figures that are -1 or not
 *    negative; one that is not taken gives a reason;
 *  - a failure always has one clean sentence of bounded length;
 *  - a version is digits and dots, at most 32 characters.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "youtube/axis-youtube-address.h"
#include "youtube/axis-youtube-output.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_clean(
	const gchar *text,
	glong        max_chars
){
	const gchar *p;

	if (text == NULL || text[0] == '\0' || !g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > max_chars)
		abort();
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			abort();
	}
}

typedef struct
{
	guint  count;
	gint64 base;	/* the streams that are done, summed */
} Seen;

static void
on_line(
	const gchar *line,
	gsize        length,
	gpointer     user_data
){
	AxisYoutubeProgress progress;
	Seen *seen = user_data;
	guint *count = &seen->count;
	gint64 done;

	(*count)++;
	if (length == 0 || length > AXIS_YOUTUBE_LINE_MAX || memchr(line, '\n', length) != NULL ||
	    memchr(line, '\r', length) != NULL)
		abort();
	if (!axis_youtube_progress_parse(line, length, &progress))
		return;
	if (progress.done_bytes < -1 || progress.done_bytes > AXIS_YOUTUBE_MAX_BYTES || progress.total_bytes < -1 ||
	    progress.total_bytes > AXIS_YOUTUBE_MAX_BYTES || progress.rate_bps < -1 ||
	    progress.rate_bps > AXIS_YOUTUBE_MAX_BYTES || progress.eta_s < -1)
		abort();
	if (progress.total_bytes >= 0 && progress.done_bytes > progress.total_bytes)
		abort();
	/* The sum over every line so far, as the fetch job keeps it. */
	done = axis_youtube_progress_add(&seen->base, &progress);
	if (done < 0 || done > AXIS_YOUTUBE_MAX_BYTES || seen->base < 0 || seen->base > AXIS_YOUTUBE_MAX_BYTES ||
	    done < (progress.finished ? seen->base : 0))
		abort();
}

static void
check_lines(
	const uint8_t *data,
	size_t         size
){
	AxisYoutubeLines lines;
	Seen seen;
	size_t at = 0;

	memset(&lines, 0, sizeof(lines));
	memset(&seen, 0, sizeof(seen));
	/* In pieces of uneven sizes, as a pipe hands them over. */
	while (at < size) {
		size_t piece = MIN(size - at, (size_t)(1 + (at * 7) % 97));

		axis_youtube_lines_feed(&lines, data + at, piece, on_line, &seen);
		at += piece;
	}
	if (seen.count > size)
		abort();
}

static void
check_record(
	const gchar *text,
	gsize        length
){
	gchar *problem = NULL;
	AxisYoutubeInfo *info = axis_youtube_info_parse(text, length, &problem);

	if (info == NULL) {
		if (problem == NULL)
			abort();
		g_free(problem);
		return;
	}
	if (problem != NULL)
		abort();
	check_clean(info->title, AXIS_YOUTUBE_TITLE_CHARS);
	check_clean(info->channel, AXIS_YOUTUBE_CHANNEL_CHARS);
	if (info->video_id == NULL ? info->refusal == NULL : !axis_youtube_video_id_valid(info->video_id))
		abort();
	if (info->channel_id != NULL) {
		const gchar *p;

		if (info->channel_id[0] == '\0' || strlen(info->channel_id) > 64)
			abort();
		for (p = info->channel_id; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
				abort();
		}
	}
	if (info->year != 0 && !g_date_valid_dmy((GDateDay)info->day, (GDateMonth)info->month, (GDateYear)info->year))
		abort();
	if (info->year == 0 && (info->month != 0 || info->day != 0))
		abort();
	if (info->duration_s < -1 || info->duration_s > 1000000 || info->expected_bytes < -1 ||
	    info->expected_bytes > AXIS_YOUTUBE_MAX_BYTES)
		abort();
	if (info->format_note != NULL)
		check_clean(info->format_note, 64);
	axis_youtube_info_free(info);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text;
	gsize length;

	if (size < 1 || size > 1 << 18)
		return 0;
	text = (const gchar *)data + 1;
	length = size - 1;
	switch (data[0]) {
	case '0':
		check_lines(data + 1, length);
		break;
	case '1':
		check_record(text, length);
		break;
	case '2': {
		gchar *reason = NULL;
		AxisYoutubeFailure kind = axis_youtube_failure_read(text, length, &reason);

		if ((guint)kind > AXIS_YOUTUBE_FAILURE_PLACE)
			abort();
		check_clean(reason, AXIS_YOUTUBE_REASON_CHARS + 16);
		g_free(reason);
		break;
	}
	case '3': {
		gchar *version = axis_youtube_version_read(text, length);

		if (version != NULL && (strlen(version) < 3 || strlen(version) > 32 ||
		    strspn(version, "0123456789.") != strlen(version)))
			abort();
		g_free(version);
		break;
	}
	default:
		break;
	}
	return 0;
}
