/*
 * axis-youtube-address.h - which addresses the YouTube downloader takes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An address is typed or pasted by an administrator and ends up as an
 * argument of yt-dlp, so it is read strictly and never passed on as it was
 * typed: what is accepted is reduced to the video's id (11 characters of
 * letters, digits, '-' and '_'), and what yt-dlp is given is the one address
 * Axis writes from that id, "https://www.youtube.com/watch?v=<id>".
 *
 * Accepted, in at most %AXIS_YOUTUBE_ADDRESS_MAX bytes of printable ASCII
 * (white space around it is ignored):
 *
 *   https://www.youtube.com/watch?v=<id>     (and youtube.com, m.youtube.com,
 *                                             music.youtube.com)
 *   https://youtu.be/<id>
 *   https://www.youtube.com/shorts/<id>      (and /live/<id>, /embed/<id>)
 *
 * with any further query parameters after the id (a "list=" beside a video
 * is ignored: the one video is taken) and an optional fragment.  Refused,
 * each with its own words: anything that is not https, any other host, a
 * host with a user name or a port, a playlist's address and a channel's
 * (this version takes one video), and an address with no video id of the
 * right form.
 *
 * GLib only, no I/O: tests/fuzz/fuzz-youtube-address.c fuzzes it.
 */

#ifndef AXIS_YOUTUBE_ADDRESS_H
#define AXIS_YOUTUBE_ADDRESS_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_YOUTUBE_ADDRESS_MAX 2048
#define AXIS_YOUTUBE_ID_LENGTH   11

/* What yt-dlp is given for a video: this, and the id. */
#define AXIS_YOUTUBE_WATCH_PREFIX "https://www.youtube.com/watch?v="

/* Whether @id is a video id: exactly 11 of A-Z, a-z, 0-9, '-' and '_'. */
gboolean
axis_youtube_video_id_valid(const gchar *id);

/**
 * axis_youtube_address_parse:
 * @text: (nullable): what was typed
 * @video_id: (out) (optional) (transfer full): the video's id
 * @problem: (out) (optional) (transfer none): why it is refused: a whole
 *   sentence that holds none of @text
 *
 * Returns: %TRUE if @text is the address of one YouTube video
 */
gboolean
axis_youtube_address_parse(
	const gchar  *text,
	gchar       **video_id,
	const gchar **problem
);

/* (transfer full): the address yt-dlp is given for @video_id (a valid id). */
gchar *
axis_youtube_address_for(const gchar *video_id);

G_END_DECLS

#endif /* AXIS_YOUTUBE_ADDRESS_H */
