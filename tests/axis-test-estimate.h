/*
 * axis-test-estimate.h - test support for the estimator: probe records and
 * index facts built in memory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A video record has one picture and one 5.1 E-AC-3 track at 640 kbit/s in
 * Matroska; an audio record one track (and no cover).  Both are probed
 * "ok" by the current probe version, progressive, with the stamp
 * axis_test_estimate_index() copies, so that a record is eligible for the
 * built-in targets unless a test changes something.
 */

#ifndef AXIS_TEST_ESTIMATE_H
#define AXIS_TEST_ESTIMATE_H

#include <glib.h>

#include "probe/axis-probe-model.h"
#include "transcode/axis-estimate.h"

G_BEGIN_DECLS

AxisMediaInfo *
axis_test_video_info(
	const gchar   *codec,
	gint           width,
	gint           height,
	gdouble        fps,
	gint64         video_bitrate,
	gdouble        seconds,
	AxisHdrFormat  hdr
);

AxisMediaInfo *
axis_test_audio_info(
	const gchar *codec,
	gint         lossless,
	gint64       bitrate,
	gdouble      seconds,
	gint         channels,
	gint         sample_rate,
	gint         bit_depth
);

/* Appends a stream (its index is the next one) and returns it. */
AxisMediaStream *
axis_test_add_stream(
	AxisMediaInfo  *info,
	AxisStreamKind  kind,
	const gchar    *codec
);

/* The size the streams' bitrates make over the duration, plus 1 %. */
gint64
axis_test_natural_size(const AxisMediaInfo *info);

/* Index facts for @info: present, one link, not pinned, not changed, on a
 * usable volume, the stamp of @info, and @size (also written to the
 * record's stamp); a unit of its own unless @unit is set. */
void
axis_test_estimate_index(
	AxisEstimateIndex   *index,
	AxisMediaInfo       *info,
	const gchar         *kind,
	AxisMediaType        root_type,
	gint64               size
);

G_END_DECLS

#endif /* AXIS_TEST_ESTIMATE_H */
