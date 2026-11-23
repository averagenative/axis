/*
 * axis-test-estimate.c - test support for the estimator
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-estimate.h"

#include <string.h>

AxisMediaStream *
axis_test_add_stream(
	AxisMediaInfo  *info,
	AxisStreamKind  kind,
	const gchar    *codec
){
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);

	s->index = (gint)info->streams->len;
	s->kind = kind;
	s->codec = g_strdup(codec);
	s->duration_ms = info->duration_ms;
	s->bitrate = AXIS_PROBE_UNKNOWN;
	s->width = AXIS_PROBE_UNKNOWN;
	s->height = AXIS_PROBE_UNKNOWN;
	s->bit_depth = AXIS_PROBE_UNKNOWN;
	s->frame_rate = -1.0;
	s->real_frame_rate = -1.0;
	s->dv_profile = AXIS_PROBE_UNKNOWN;
	s->dv_level = AXIS_PROBE_UNKNOWN;
	s->dv_bl_compat_id = AXIS_PROBE_UNKNOWN;
	s->dv_hdr10_compatible = AXIS_TRI_UNKNOWN;
	s->channels = AXIS_PROBE_UNKNOWN;
	s->sample_rate = AXIS_PROBE_UNKNOWN;
	s->bit_depth_audio = AXIS_PROBE_UNKNOWN;
	s->lossless = AXIS_TRI_UNKNOWN;
	s->tag_bitrate = AXIS_PROBE_UNKNOWN;
	if (kind == AXIS_STREAM_AUDIO)
		s->lossless = axis_probe_audio_lossless(codec, NULL);
	if (kind == AXIS_STREAM_SUBTITLE)
		s->subtitle_kind = axis_probe_subtitle_kind(codec);
	g_ptr_array_add(info->streams, s);
	return s;
}

static AxisMediaInfo *
base_info(
	const gchar *container,
	gdouble      seconds
){
	AxisMediaInfo *info = axis_media_info_new();

	info->status = g_strdup("ok");
	info->probe_version = AXIS_PROBE_VERSION;
	info->container = g_strdup(container);
	info->duration_ms = (gint64)(seconds * 1000.0);
	info->mtime_sec = 1700000000;
	info->mtime_nsec = 5;
	info->ctime_sec = 1700000001;
	info->ctime_nsec = 6;
	info->inode = 4242;
	return info;
}

AxisMediaInfo *
axis_test_video_info(
	const gchar   *codec,
	gint           width,
	gint           height,
	gdouble        fps,
	gint64         video_bitrate,
	gdouble        seconds,
	AxisHdrFormat  hdr
){
	AxisMediaInfo *info = base_info("matroska,webm", seconds);
	AxisMediaStream *v = axis_test_add_stream(info, AXIS_STREAM_VIDEO, codec);
	AxisMediaStream *a = axis_test_add_stream(info, AXIS_STREAM_AUDIO, "eac3");

	v->width = width;
	v->height = height;
	v->frame_rate = fps;
	v->real_frame_rate = fps;
	v->bitrate = video_bitrate;
	v->bit_depth = hdr == AXIS_HDR_SDR ? 8 : 10;
	v->field_order = g_strdup("progressive");
	v->hdr_format = hdr;
	v->is_default = TRUE;
	a->bitrate = 640000;
	a->channels = 6;
	a->sample_rate = 48000;
	a->is_default = TRUE;
	a->language = g_strdup("eng");

	info->video_streams = 1;
	info->audio_streams = 1;
	info->video_codec = g_strdup(codec);
	info->width = width;
	info->height = height;
	info->resolution_class = axis_probe_resolution_class(width, height);
	info->bit_depth = v->bit_depth;
	info->frame_rate = fps;
	info->hdr_format = hdr;
	info->interlaced = AXIS_TRI_NO;
	info->vfr_suspected = AXIS_TRI_NO;
	if (width > 0 && height > 0 && fps > 0 && video_bitrate > 0)
		info->bits_per_pixel = (gdouble)video_bitrate / ((gdouble)width * height * fps);
	info->audio_codec = g_strdup("eac3");
	info->audio_channels = 6;
	info->audio_lossless = AXIS_TRI_NO;
	info->bitrate = video_bitrate + 640000;
	info->stream_count = 2;
	return info;
}

AxisMediaInfo *
axis_test_audio_info(
	const gchar *codec,
	gint         lossless,
	gint64       bitrate,
	gdouble      seconds,
	gint         channels,
	gint         sample_rate,
	gint         bit_depth
){
	AxisMediaInfo *info = base_info(g_strcmp0(codec, "flac") == 0 ? "flac" :
		g_strcmp0(codec, "mp3") == 0 ? "mp3" : "ogg", seconds);
	AxisMediaStream *a = axis_test_add_stream(info, AXIS_STREAM_AUDIO, codec);

	a->bitrate = bitrate;
	a->channels = channels;
	a->sample_rate = sample_rate;
	a->bit_depth_audio = bit_depth;
	a->lossless = lossless;
	a->is_default = TRUE;
	info->audio_streams = 1;
	info->audio_codec = g_strdup(codec);
	info->audio_channels = channels;
	info->audio_sample_rate = sample_rate;
	info->audio_bit_depth = bit_depth;
	info->audio_lossless = lossless;
	info->bitrate = bitrate;
	info->stream_count = 1;
	info->hdr_format = AXIS_HDR_NONE;
	return info;
}

gint64
axis_test_natural_size(const AxisMediaInfo *info)
{
	gdouble seconds = (gdouble)info->duration_ms / 1000.0;
	gdouble bits = 0;
	guint i;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->bitrate > 0)
			bits += (gdouble)s->bitrate;
	}
	return (gint64)(bits * seconds / 8.0 * 1.01) + 1;
}

void
axis_test_estimate_index(
	AxisEstimateIndex   *index,
	AxisMediaInfo       *info,
	const gchar         *kind,
	AxisMediaType        root_type,
	gint64               size
){
	memset(index, 0, sizeof(*index));
	index->file_id = 1;
	index->root_id = 1;
	index->volume_id = 1;
	index->kind = kind;
	index->root_type = root_type;
	index->state = "present";
	index->size = size;
	index->nlink = 1;
	index->volume_usable = TRUE;
	index->counted = TRUE;
	index->rel_path = "Film/Film.mkv";
	if (info != NULL) {
		info->file_size = size;
		index->mtime_sec = info->mtime_sec;
		index->mtime_nsec = info->mtime_nsec;
		index->ctime_sec = info->ctime_sec;
		index->ctime_nsec = info->ctime_nsec;
		index->inode = info->inode;
	}
}
