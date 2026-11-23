/*
 * axis-sample-args.h - the sample measurement's encoder command for one
 * excerpt
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The daemon's own measurement (axis-estimate-sample.c) and a worker's
 * (clients/worker) build the command with this one function, so an excerpt
 * is encoded the same way wherever it runs.  Only fixed names and numbers
 * reach the vector: nothing from the file, and nothing a daemon sent a
 * worker but the enumerations and bounded integers of the target and the
 * stream (see axis-worker-proto.h).
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_SAMPLE_ARGS_H
#define AXIS_SAMPLE_ARGS_H

#include <glib.h>

#include "probe/axis-probe-model.h"
#include "transcode/axis-estimate-target.h"

G_BEGIN_DECLS

/* The encoder's threads in a measurement, and the most bytes one excerpt
 * may write to its pipe. */
#define AXIS_SAMPLE_ENCODER_THREADS "4"
#define AXIS_SAMPLE_OUTPUT_CAP      (G_GUINT64_CONSTANT(64) * 1024 * 1024 * 1024)

/**
 * axis_estimate_sample_argv:
 *
 * The measurement's command for one excerpt: @prefix (the program, or a
 * test's tool and its arguments), then the input as descriptor 3 from
 * @start for @seconds, the stream @stream only, encoded for @target at
 * @quality (and @bit_depth for a picture) to a raw stream on standard
 * output.  Of @target it reads the media, codec, encoder family, audio
 * bitrates, audio quality and reduce_to_cd; of @stream the index, channel
 * count, sample rate and audio bit depth.  Only fixed names and numbers:
 * nothing from the file.
 */
gchar **
axis_estimate_sample_argv(
	const gchar *const       *prefix,
	const AxisEstimateTarget *target,
	const AxisMediaStream    *stream,
	gint                      quality,
	gint                      bit_depth,
	gdouble                   start,
	gdouble                   seconds
);

G_END_DECLS

#endif /* AXIS_SAMPLE_ARGS_H */
