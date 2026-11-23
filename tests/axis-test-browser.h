/*
 * axis-test-browser.h - test support for the Library pages: an index of
 * generated rows (SQL generate_series, no files), and the folder totals
 * brought up to date from it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The rows are made so that a test can know what a filter should find
 * without a second implementation of it: for the file numbered i (1 to n,
 * also its inode):
 *
 *   path      d<i % dirs>/s<(i / dirs) % 10>/f<i, 8 digits><ext>
 *   kind      i % 5 = 0: text (.txt); i % 5 = 1: audio (.flac); else video (.mkv)
 *   size      i * 1000
 *   mtime     1600000000 + (i * 7919) % 100003 (many equal, past 100003 files)
 *   pinned    i % 7 = 0 (two hard links)
 *   state     i % 97 = 0: suspected; else present
 *   probe     video and audio, but not i % 11 = 0 (not probed yet); i % 13 = 0
 *             failed ("boom"); the rest ok:
 *     container     matroska,webm (flac for audio)
 *     video codec   i % 3 = 0: hevc; else h264 (audio: none)
 *     resolution    sd, 720p, 1080p, 2160p for i % 4 = 0 to 3 (audio: none)
 *     HDR           i % 49 = 0: dolby_vision and uncertain; i % 9 = 0: hdr10; else sdr
 *     audio codec   i % 2 = 0: eac3; else aac
 *     duration      60 s + (i % 7200) s
 *     bit rate      1000000 + (i * 37) % 8000000
 */

#ifndef AXIS_TEST_BROWSER_H
#define AXIS_TEST_BROWSER_H

#include <glib.h>

#include "axis-test-library.h"

G_BEGIN_DECLS

/* Inserts files numbered @first to @last of the pattern above into @root. */
void
axis_test_browser_generate(
	AxisTestLibrary *t,
	gint64           volume_id,
	gint64           root_id,
	guint            first,
	guint            last,
	guint            dirs
);

/* Brings the root's library_dir rows up to date, as a complete scan does
 * (on one pooled connection, in one transaction); returns the seconds it
 * took. */
gdouble
axis_test_browser_update_dirs(
	AxisTestLibrary *t,
	gint64           root_id
);

G_END_DECLS

#endif /* AXIS_TEST_BROWSER_H */
