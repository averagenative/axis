/*
 * axis-youtube-output.h - reading what yt-dlp writes: its progress lines, the
 * record of a video, and why it failed
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything yt-dlp prints carries what a video's uploader wrote (titles,
 * channel names, descriptions) and what YouTube answered: data from outside.
 * It is read within fixed limits and never trusted for more than showing:
 *
 *  - Progress comes as lines Axis asked for with a template of its own
 *    (AXIS_YOUTUBE_PROGRESS_TEMPLATE): the word "AXIS", a status and five
 *    figures, each a number or yt-dlp's word for "not known".  A line of any
 *    other form is ignored, a figure that is not a number within range is
 *    "not known", and a line longer than %AXIS_YOUTUBE_LINE_MAX is dropped.
 *  - A video's record is the JSON of `--dump-single-json`, read with the
 *    bounded reader (probe/axis-probe-json.h).  Only a handful of members are
 *    taken; every text is cleaned (no control characters) and cut, and a
 *    record without an id of the right form or a title is refused.
 *  - Why yt-dlp failed is its last line that begins "ERROR:", cleaned and
 *    cut, with the few failures a person can act on told apart.
 *
 * GLib only, no I/O: tests/fuzz/fuzz-youtube-output.c fuzzes it.
 */

#ifndef AXIS_YOUTUBE_OUTPUT_H
#define AXIS_YOUTUBE_OUTPUT_H

#include <glib.h>

G_BEGIN_DECLS

/* The progress line yt-dlp is asked to print (its --progress-template). */
#define AXIS_YOUTUBE_PROGRESS_TEMPLATE \
	"download:AXIS %(progress.status)s %(progress.downloaded_bytes)s %(progress.total_bytes)s " \
	"%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s"

#define AXIS_YOUTUBE_LINE_MAX      512
#define AXIS_YOUTUBE_TITLE_CHARS   300
#define AXIS_YOUTUBE_CHANNEL_CHARS 200
#define AXIS_YOUTUBE_REASON_CHARS  200

/* The largest figure taken from yt-dlp (bytes, bytes a second): 2^50. */
#define AXIS_YOUTUBE_MAX_BYTES     (G_GINT64_CONSTANT(1) << 50)

/**
 * AxisYoutubeProgress:
 * @finished: this stream is done (a video and its audio are two)
 * @done_bytes: of this stream, or -1
 * @total_bytes: of this stream (an estimate when yt-dlp has no more), or -1
 * @rate_bps: bytes a second, or -1
 * @eta_s: seconds left of this stream, or -1
 */
typedef struct
{
	gboolean finished;
	gint64   done_bytes;
	gint64   total_bytes;
	gint64   rate_bps;
	gint64   eta_s;
} AxisYoutubeProgress;

/* Whether @line (@length bytes, no line end) is a progress line; fills
 * @progress. */
gboolean
axis_youtube_progress_parse(
	const gchar         *line,
	gsize                length,
	AxisYoutubeProgress *progress
);

/**
 * axis_youtube_progress_add:
 * @base: (inout): the bytes of the streams that are done (0 before the
 *   first line); a finished stream is added to it
 * @progress: a progress line, read
 *
 * A video and its audio are two streams, one after the other: this adds a
 * line to what came before.  yt-dlp's lines are data from outside, as many
 * as it cares to print: @base and the result stay within 0 and
 * %AXIS_YOUTUBE_MAX_BYTES whatever they say.
 *
 * Returns: the bytes done of the whole download so far
 */
gint64
axis_youtube_progress_add(
	gint64                    *base,
	const AxisYoutubeProgress *progress
);

/**
 * AxisYoutubeLines:
 *
 * Standard output as it arrives, cut into lines.  It keeps at most one line
 * being read (%AXIS_YOUTUBE_LINE_MAX bytes: a longer one is dropped whole).
 */
typedef struct
{
	gchar    buffer[AXIS_YOUTUBE_LINE_MAX];
	gsize    used;
	gboolean overlong;
} AxisYoutubeLines;

typedef void (*AxisYoutubeLineFunc)(
	const gchar *line,
	gsize        length,
	gpointer     user_data
);

/* Feeds @data to @lines; @func is called for each whole line (a line end is
 * '\n' or '\r'; empty lines are skipped). */
void
axis_youtube_lines_feed(
	AxisYoutubeLines    *lines,
	const guint8        *data,
	gsize                length,
	AxisYoutubeLineFunc  func,
	gpointer             user_data
);

/**
 * AxisYoutubeInfo:
 * @video_id: the video's id (valid)
 * @title: its title, cleaned
 * @channel: its channel's name, cleaned ("Unknown channel" for none)
 * @channel_id: (nullable): the channel's id (letters, digits, '-', '_')
 * @year, @month, @day: its upload date, 0 when not said
 * @duration_s: -1 when not said
 * @expected_bytes: the size of what would be taken, -1 when not said
 * @format_note: (nullable): what would be taken ("1080p, avc1 with mp4a")
 * @refusal: (nullable): why Axis does not fetch it (a live stream, one not
 *   shown yet, one that needs an account): a whole sentence
 */
typedef struct
{
	gchar  *video_id;
	gchar  *title;
	gchar  *channel;
	gchar  *channel_id;
	gint    year;
	gint    month;
	gint    day;
	gint64  duration_s;
	gint64  expected_bytes;
	gchar  *format_note;
	gchar  *refusal;
} AxisYoutubeInfo;

void
axis_youtube_info_free(AxisYoutubeInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisYoutubeInfo, axis_youtube_info_free)

/**
 * axis_youtube_info_parse:
 * @data: yt-dlp's standard output for `--dump-single-json`
 * @length: its length
 * @problem: (out) (optional) (transfer full): why it cannot be used, a
 *   sentence with none of the input in it
 *
 * Returns: (transfer full) (nullable): the record
 */
AxisYoutubeInfo *
axis_youtube_info_parse(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/**
 * AxisYoutubeFailure:
 * @AXIS_YOUTUBE_FAILURE_OTHER: anything else
 * @AXIS_YOUTUBE_FAILURE_REFUSED: YouTube refused this server for now (too
 *   many requests, or it asked to confirm that the visitor is not a bot):
 *   YouTube work pauses
 * @AXIS_YOUTUBE_FAILURE_ACCOUNT: the video needs an account (private, for
 *   members, age restricted): Axis signs in nowhere
 * @AXIS_YOUTUBE_FAILURE_GONE: YouTube says the video is not available
 * @AXIS_YOUTUBE_FAILURE_PLACE: not available from this server's location
 */
typedef enum
{
	AXIS_YOUTUBE_FAILURE_OTHER,
	AXIS_YOUTUBE_FAILURE_REFUSED,
	AXIS_YOUTUBE_FAILURE_ACCOUNT,
	AXIS_YOUTUBE_FAILURE_GONE,
	AXIS_YOUTUBE_FAILURE_PLACE
} AxisYoutubeFailure;

/**
 * axis_youtube_failure_read:
 * @errors: yt-dlp's standard error (need not end in NUL)
 * @length: its length
 * @reason: (out) (transfer full): one plain sentence for a person, at most
 *   %AXIS_YOUTUBE_REASON_CHARS characters of yt-dlp's own words in it
 *
 * Returns: what kind of failure it was
 */
AxisYoutubeFailure
axis_youtube_failure_read(
	const gchar  *errors,
	gsize         length,
	gchar       **reason
);

/* The first line of `yt-dlp --version` when it is a version (digits and
 * dots, as "2026.11.05", at most 32 characters); %NULL otherwise. */
gchar *
axis_youtube_version_read(
	const gchar *output,
	gsize        length
);

G_END_DECLS

#endif /* AXIS_YOUTUBE_OUTPUT_H */
