/*
 * axis-test-media.h - test support: small media files made with ffmpeg's
 * test sources, once per test program
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The clips are a second or two long and live in a scratch directory that
 * is removed when the program exits.  Without ffmpeg and ffprobe
 * installed, axis_test_media_available() says so and a test skips.
 *
 * The clips: sdr.mkv (H.264 and AAC), multi.mkv (H.264, two AAC tracks, two
 * SubRip tracks), hdr10.mkv (HEVC, PQ with mastering display metadata),
 * pq-bare.mkv (PQ without it), unsig10.mkv (10-bit with no colour
 * signalling), tone1.flac and tone2.flac (tagged, one with cover art),
 * tone.mp3, short.mp4, other.ts (AAC at 12000 Hz, distinct from the rest)
 * and cover.png.
 */

#ifndef AXIS_TEST_MEDIA_H
#define AXIS_TEST_MEDIA_H

#include <glib.h>

G_BEGIN_DECLS

/* Makes the clips on first use. */
gboolean
axis_test_media_available(void);

/* Returns: (transfer full): the path of a clip; only after
 * axis_test_media_available() said yes */
gchar *
axis_test_media_path(const gchar *name);

G_END_DECLS

#endif /* AXIS_TEST_MEDIA_H */
