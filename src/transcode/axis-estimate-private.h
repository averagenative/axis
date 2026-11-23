/*
 * axis-estimate-private.h - shared by the files of the estimator's pure part
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_ESTIMATE_PRIVATE_H
#define AXIS_ESTIMATE_PRIVATE_H

#include "transcode/axis-estimate.h"

G_BEGIN_DECLS

/* The picture sizes the video priors are given at (pixels per frame). */
#define AXIS_ESTIMATE_N_ANCHORS 4

/* What the size model needs of a file, worked out once from its facts. */
typedef struct
{
	const AxisMediaStream *video;	/* the main picture (largest, not a cover) */
	const AxisMediaStream *audio;	/* the main audio stream */
	guint                  n_video;	/* pictures that are not covers */
	guint                  n_audio;
	guint                  n_subtitle;
	guint                  n_attachment;
	guint                  n_other;	/* data and unknown */
	guint                  n_cover;
	gdouble                seconds;	/* -1 when unknown */
	gdouble                fps;	/* -1 when unknown */
	gint64                 pixels;	/* per frame, -1 when unknown */
	gint64                 video_bitrate;	/* -1 when unknown */
	gdouble                source_bpp;	/* -1 when unknown */
} AxisEstimateShape;

void
axis_estimate_shape_init(
	AxisEstimateShape   *shape,
	const AxisMediaInfo *info
);

/* How many H.264 bits a codec's bit is worth (H.264 is 1); 0 for a codec
 * the estimator has no figure for. */
gdouble
axis_estimate_codec_efficiency(const gchar *codec);

/* Audio: whether the estimator knows the codec, and what the FLAC prior is
 * for it (the index of its cell); -1 for an unknown lossless codec. */
gint
axis_estimate_lossless_class(const gchar *codec);

gboolean
axis_estimate_lossy_known(const gchar *codec);

/**
 * AxisEstimatePart:
 *
 * One estimated stream: the size of what replaces it, as three figures
 * (bytes, not rounded), and the cell they come from.
 */
typedef struct
{
	gdouble low;
	gdouble mid;
	gdouble high;
	guint   cell;
} AxisEstimatePart;

/* The video stream of a file at @quality: bits per pixel from the table,
 * the complexity factor, and the bytes over @seconds.  FALSE when a fact is
 * missing. */
gboolean
axis_estimate_model_video(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	const AxisEstimateShape  *shape,
	const gchar              *source_codec,
	gboolean                  hdr_source,
	gint                      quality,
	AxisEstimatePart         *part
);

/* What an audio file holds besides its audio (cover art and tags), as a
 * range: what is kept as it is when its audio is converted. */
void
axis_estimate_other_bytes(
	const AxisMediaInfo *info,
	gdouble             *low,
	gdouble             *mid,
	gdouble             *high
);

/* The converted audio stream of an audio file of @source_bytes. */
gboolean
axis_estimate_model_audio(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	const AxisEstimateShape  *shape,
	gdouble                   source_bytes,
	AxisEstimatePart         *part
);

G_END_DECLS

#endif /* AXIS_ESTIMATE_PRIVATE_H */
