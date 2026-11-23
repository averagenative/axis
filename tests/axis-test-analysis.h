/*
 * axis-test-analysis.h - test support for the Analysis reports: a library
 * with known answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The scenario is two volumes ("Disk A", "Disk B") with seven roots, files
 * written to scratch directories and scanned by the real scanner, and the
 * probe's rows written by SQL (the probe's own tests cover ffprobe).  The
 * answers each report must give are worked out by hand in
 * tests/test-analysis.c.  The files, as "root: path (size, what the probe
 * found)":
 *
 *  Films (A, movies)
 *   Alder (1979)/Alder (1979) 2160p.mkv     4000  hevc 2160p hdr10, 0.200 bits/pixel, eac3
 *   Alder (1979)/Alder (1979) 2160p.en.srt    10  (beside its video)
 *   Alder (1979)/Sample/alder-sample.mkv      50  not probed (a sample)
 *   Alder (1979)/Extras/making-of.mp4         30  probe failed (an extra)
 *   Hearth (1995)/Hearth (1995).mkv             1000  h264 1080p sdr, 0.350
 *   Hearth (1995)/Hearth (1995) copy.mkv        1000  hard link of the one above
 *   Hearth (1995)/Other Movie.srt               12  (no video of that name: orphaned)
 *   Hearth (1986)/Hearth (1986).mkv             1100  h264 1080p sdr, 0.100
 *   Brannock (1985)/Brannock (1985).mkv          777  h264 1080p sdr, 0.100, 8000 s
 *   Dunmore (2021)/Dunmore (2021) 720p.mkv         500  h264 720p sdr, 0.080
 *   Total Rewind (1990)/Total Rewind (1990).mkv 600  h264 sd sdr (HDR uncertain), 0.120
 *   The Mazework (1999)/The Mazework (1999).mkv  650  h264 1080p sdr, 0.120
 *   The Mazework Reloaded (2003)/The Mazework Reloaded (2003).mkv 660  h264 1080p sdr, 0.120
 *   Unknown Film.mkv                         400  not probed (no year in the name)
 *   Thumbs.db                                  5  (junk)
 *   Example.mkv, Sampler.mkv               40, 41  not probed (not samples)
 *  Shows (A, tv)
 *   Show X (2010)/Season 1/Show X - S01E01.mkv  300  h264 1080p, 0.05
 *   Show X (2010)/Season 1/Show X - S01E02.mkv  310
 *   Show Y/Season 1/Show Y - S01E01.mkv         250
 *  Music (A, music), tagged, flac (lossless)
 *   Artist One/Album One/01 Track.flac 200, 02 Track Two.flac 210
 *   Artist Two/Album Two/01 Solo.flac 220
 *  Downloads (A, downloads)
 *   Hearth.1995.1080p.mkv                     1000  hard link of Hearth (1995).mkv
 *  Films2 (B, movies)
 *   Brannock (1985)/Brannock (1985).mkv          777  the same as on A
 *   Dunmore (2021)/Dunmore.2021.2160p.mkv         3000  hevc 2160p sdr, 0.100
 *   Total Rewind (2012)/Total Rewind (2012).mkv 610  h264 1080p sdr, 0.120
 *   Unknown Film.mkv                         400  not probed
 *  Shows2 (B, tv)
 *   Show X (2010)/Season 1/Show X - S01E03.mkv  320
 *   Show X (2010)/Season 2/Show X - S02E01.mkv  330
 *   Show X (2010)/Season 1/Show.X.2010.S01E01.720p.mkv  150  h264 720p
 *  Music2 (B, music)
 *   Artist One/Album One/01 Track.mp3         50  mp3, lossy, the same tags as the flac
 *
 * 32 present paths (24 on A, 8 on B); 17002 bytes with the hard links
 * counted once (11365 on A, 5637 on B).
 */

#ifndef AXIS_TEST_ANALYSIS_H
#define AXIS_TEST_ANALYSIS_H

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
	gint64  down_a;
	gint64  films_b;
	gint64  shows_b;
	gint64  music_b;
} AxisTestScenario;

#define AXIS_TEST_SCENARIO_PATHS   32
#define AXIS_TEST_SCENARIO_BYTES   17002
#define AXIS_TEST_SCENARIO_PATHS_A 24
#define AXIS_TEST_SCENARIO_BYTES_A 11365
#define AXIS_TEST_SCENARIO_PATHS_B 8
#define AXIS_TEST_SCENARIO_BYTES_B 5637

/* Makes the volumes, roots and files, scans every root, writes the probe
 * rows and clears the files' "changed" flags (as the probe does). */
void
axis_test_scenario_build(
	AxisTestLibrary  *t,
	AxisTestScenario *s
);

void
axis_test_scenario_clear(AxisTestScenario *s);

/* A file's id by root and path in the root; asserts it exists. */
gint64
axis_test_scenario_file(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel
);

/* Writes @size bytes to @rel under the root's directory on @disk. */
void
axis_test_scenario_put(
	const gchar *disk,
	const gchar *root_rel,
	const gchar *rel,
	gsize        size
);

/* A probe row of a video file; @bpp is text ("0.350"), @hdr_uncertain marks
 * the HDR class as uncertain. */
void
axis_test_probe_video(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel,
	const gchar     *container,
	const gchar     *vcodec,
	const gchar     *res,
	const gchar     *hdr,
	const gchar     *bpp,
	gint64           duration_ms,
	const gchar     *acodec,
	gboolean         hdr_uncertain
);

void
axis_test_probe_audio(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel,
	const gchar     *container,
	const gchar     *acodec,
	gboolean         lossless,
	gint64           duration_ms,
	const gchar     *artist,
	const gchar     *album,
	const gchar     *title
);

void
axis_test_probe_failed(
	AxisTestLibrary *t,
	gint64           root,
	const gchar     *rel
);

G_END_DECLS

#endif /* AXIS_TEST_ANALYSIS_H */
