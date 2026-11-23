/*
 * axis-test-savings.h - test support for the Savings estimates: a library
 * whose probe records fit the files' sizes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two volumes ("Disk A", "Disk B"), files written to scratch directories at
 * the size their probe records imply (the bitrates over a duration of one
 * second, plus 1 %), scanned by the real scanner, probe rows written by SQL.
 * What each built-in target decides for each file is worked out in
 * tests/test-estimate-db.c; the files, "root: path (what the probe found)":
 *
 *  Films (A, movies)
 *   Big (2001)/Big (2001).mkv            h264 1080p 8 Mbit/s, eac3 640k
 *   Remux (2002)/Remux (2002).mkv        h264 1080p 25 Mbit/s, truehd 3 Mbit/s
 *   Small (2003)/Small (2003).mkv        h264 720p 0.6 Mbit/s, aac 128k
 *   HEVC (2004)/HEVC (2004).mkv          hevc 1080p 2 Mbit/s, aac 128k
 *   DV (2005)/DV (2005).mkv              hevc 2160p Dolby Vision 20 Mbit/s
 *   Uncertain (2006)/Uncertain (2006).mkv h264 1080p, HDR uncertain
 *   Unprobed.mkv                         not probed
 *   Broken.mkv                           probe failed
 *   Linked (2007)/Linked (2007).mkv      h264 1080p 8 Mbit/s, and a hard
 *   Linked (2007)/Linked copy.mkv        link of it beside it
 *   HDR10 (2008)/HDR10 (2008).mkv        hevc 2160p HDR10 40 Mbit/s
 *  Shows (A, tv)
 *   Show (2010)/Season 1/Show - S01E01.mkv  h264 720p 4 Mbit/s, aac 128k
 *   Show (2010)/Season 1/Show - S01E02.mkv  the same, interlaced
 *  Music (A, music)
 *   Band/Album/01 One.flac, 02 Two.flac  flac 900k
 *   Band/Mixed/01 A.flac                 flac 900k
 *   Band/Mixed/02 B.mp3                  mp3 320k (lossy: the album is not
 *                                        converted)
 *   Loose.wav                            pcm_s16le 1411k at the top
 *  Books (A, audiobooks)
 *   Author/Book/Part 1.mp3, Part 2.mp3   mp3 128k
 *  Downloads (A, downloads)
 *   Linked.mkv                           another hard link of Linked (2007)
 *  Films2 (B, movies)
 *   Other (2011)/Other (2011).mkv        h264 1080p 10 Mbit/s, aac 192k
 */

#ifndef AXIS_TEST_SAVINGS_H
#define AXIS_TEST_SAVINGS_H

#include <glib.h>

#include "axis-test-library.h"

G_BEGIN_DECLS

typedef struct
{
	gchar  *disk_a;
	gchar  *disk_b;
	gint64  vol_a;
	gint64  vol_b;
	gint64  films_a;
	gint64  shows_a;
	gint64  music_a;
	gint64  books_a;
	gint64  down_a;
	gint64  films_b;
} AxisTestSavings;

/**
 * AxisTestProbeStream:
 *
 * One stream of a probe record.  @lossless is "true", "false" or %NULL;
 * @hdr is a stored HDR class or %NULL; @field_order %NULL for not known.
 */
typedef struct
{
	const gchar *kind;
	const gchar *codec;
	gint64       bitrate;
	gint         width;
	gint         height;
	gdouble      fps;
	const gchar *hdr;
	gboolean     hdr_uncertain;
	gint         channels;
	gint         sample_rate;
	gint         bits;
	const gchar *lossless;
	const gchar *field_order;
	const gchar *language;
} AxisTestProbeStream;

/* The size a record of @streams (ended by a NULL kind) implies over
 * @seconds. */
gsize
axis_test_savings_size(
	const AxisTestProbeStream *streams,
	gdouble               seconds
);

/* Writes the probe rows of a file (its stamp from the index) and clears its
 * "changed" flag, as the probe does; @interlaced is "true", "false" or
 * %NULL. */
void
axis_test_savings_probe(
	AxisTestLibrary      *t,
	gint64                file_id,
	const gchar          *container,
	gdouble               seconds,
	const AxisTestProbeStream *streams,
	const gchar          *interlaced
);

void
axis_test_savings_build(
	AxisTestLibrary *t,
	AxisTestSavings *s
);

void
axis_test_savings_clear(AxisTestSavings *s);

G_END_DECLS

#endif /* AXIS_TEST_SAVINGS_H */
