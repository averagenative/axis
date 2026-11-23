/*
 * axis-analysis-rank.h - which copy of a title looks best
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The copies in a "same title" group are ranked by what the probe found:
 * a probed copy before one never probed (or whose probe failed), then the
 * higher resolution, the richer HDR class (Dolby Vision, HDR10+, HDR10,
 * HLG, then SDR; a class that is uncertain or unknown counts as none), a
 * lossless audio copy before a lossy one, the higher video bits per pixel
 * (else the higher bit rate), the larger file, and last the lower file id,
 * so that the order never depends on the order rows were read.  The ranking
 * only orders; nothing is decided from it.
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_ANALYSIS_RANK_H
#define AXIS_ANALYSIS_RANK_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AxisAnalysisCopy:
 * @id: the file
 * @probed: its probe succeeded
 * @resolution: (nullable): "sd", "720p", "1080p", "2160p" or "other"
 * @hdr: (nullable): "sdr", "hlg", "hdr10", "hdr10plus" or "dolby_vision"
 * @hdr_uncertain: the probe could not be sure of @hdr
 * @lossless: 1 lossless audio, 0 lossy, -1 not known
 * @bits_per_pixel: video bits per pixel and frame, negative when unknown
 * @bitrate: bits per second, negative when unknown
 * @size: bytes
 */
typedef struct
{
	gint64       id;
	gboolean     probed;
	const gchar *resolution;
	const gchar *hdr;
	gboolean     hdr_uncertain;
	gint         lossless;
	gdouble      bits_per_pixel;
	gint64       bitrate;
	gint64       size;
} AxisAnalysisCopy;

/* A rank number of a resolution class (0 for none or unknown, higher is
 * better) and of an HDR class. */
gint
axis_analysis_resolution_rank(const gchar *resolution);

gint
axis_analysis_hdr_rank(
	const gchar *hdr,
	gboolean     uncertain
);

/* Negative when @a ranks before (is better than) @b, positive when after,
 * and never 0 for two different ids. */
gint
axis_analysis_copy_compare(
	const AxisAnalysisCopy *a,
	const AxisAnalysisCopy *b
);

G_END_DECLS

#endif /* AXIS_ANALYSIS_RANK_H */
