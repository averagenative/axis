/*
 * axis-test-convert.c - test support for conversion plans
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-convert.h"

#include <string.h>

AxisMediaInfo *
axis_test_convert_info(const gchar *container)
{
	AxisMediaInfo *info = axis_media_info_new();

	info->status = g_strdup("ok");
	info->container = g_strdup(container);
	info->duration_ms = 5400000;
	info->chapter_count = 0;
	info->interlaced = AXIS_TRI_NO;
	info->frames_unavailable = FALSE;
	return info;
}

static AxisMediaStream *
stream_new(
	AxisMediaInfo  *info,
	gint            index,
	AxisStreamKind  kind,
	const gchar    *codec
){
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);

	s->index = index;
	s->kind = kind;
	s->codec = g_strdup(codec);
	s->duration_ms = AXIS_PROBE_UNKNOWN;
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
	axis_hdr_signals_init(&s->signals);
	g_ptr_array_add(info->streams, s);
	return s;
}

AxisMediaStream *
axis_test_convert_video(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	gint           width,
	gint           height,
	const gchar   *pixel_format,
	gint           bit_depth,
	const gchar   *primaries,
	const gchar   *transfer,
	const gchar   *matrix,
	const gchar   *range,
	AxisHdrFormat  hdr
){
	AxisMediaStream *s = stream_new(info, index, AXIS_STREAM_VIDEO, codec);

	s->width = width;
	s->height = height;
	s->pixel_format = g_strdup(pixel_format);
	s->bit_depth = bit_depth;
	s->frame_rate = 24000.0 / 1001.0;
	s->real_frame_rate = s->frame_rate;
	s->field_order = g_strdup("progressive");
	s->color_primaries = g_strdup(primaries);
	s->color_transfer = g_strdup(transfer);
	s->color_space = g_strdup(matrix);
	s->color_range = g_strdup(range);
	s->hdr_format = hdr;
	s->is_default = TRUE;
	if (hdr == AXIS_HDR_HDR10) {
		axis_test_convert_hdr10_values(&s->hdr10);
		s->signals.has_mastering = TRUE;
		s->signals.has_content_light = TRUE;
		s->signals.hdr10 = s->hdr10;
		s->signals.mastering_in_frames = TRUE;
		s->signals.light_in_frames = TRUE;
	}
	return s;
}

void
axis_test_convert_hdr10_values(AxisHdr10Metadata *m)
{
	memset(m, 0, sizeof(*m));
	m->mastering = TRUE;
	m->red_x = 34000;
	m->red_y = 16000;
	m->green_x = 13250;
	m->green_y = 34500;
	m->blue_x = 7500;
	m->blue_y = 3000;
	m->white_x = 15635;
	m->white_y = 16450;
	m->min_luminance = 50;
	m->max_luminance = 10000000;
	m->light = TRUE;
	m->max_content = 1000;
	m->max_average = 400;
}

AxisMediaStream *
axis_test_convert_audio(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	gint           channels,
	const gchar   *language,
	gboolean       is_default
){
	AxisMediaStream *s = stream_new(info, index, AXIS_STREAM_AUDIO, codec);

	s->channels = channels;
	s->sample_rate = 48000;
	s->language = g_strdup(language);
	s->is_default = is_default;
	s->lossless = axis_probe_audio_lossless(codec, NULL);
	return s;
}

AxisMediaStream *
axis_test_convert_subtitle(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec,
	const gchar   *language,
	gboolean       is_default,
	gboolean       is_forced
){
	AxisMediaStream *s = stream_new(info, index, AXIS_STREAM_SUBTITLE, codec);

	s->language = g_strdup(language);
	s->is_default = is_default;
	s->is_forced = is_forced;
	s->subtitle_kind = axis_probe_subtitle_kind(codec);
	return s;
}

AxisMediaStream *
axis_test_convert_attachment(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec
){
	return stream_new(info, index, AXIS_STREAM_ATTACHMENT, codec);
}

AxisMediaStream *
axis_test_convert_cover(
	AxisMediaInfo *info,
	gint           index,
	const gchar   *codec
){
	AxisMediaStream *s = stream_new(info, index, AXIS_STREAM_VIDEO, codec);

	s->attached_pic = TRUE;
	s->width = 600;
	s->height = 900;
	return s;
}

void
axis_test_convert_finish(AxisMediaInfo *info)
{
	guint i;

	info->stream_count = (gint)info->streams->len;
	info->hdr_format = AXIS_HDR_NONE;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind == AXIS_STREAM_VIDEO && !s->attached_pic) {
			if (s->hdr_format > info->hdr_format)
				info->hdr_format = s->hdr_format;
			if (info->main_video < 0) {
				info->main_video = (gint)i;
				info->width = s->width;
				info->height = s->height;
				info->resolution_class = axis_probe_resolution_class(s->width, s->height);
				info->bit_depth = s->bit_depth;
			}
			info->video_streams++;
		} else if (s->kind == AXIS_STREAM_AUDIO) {
			info->audio_streams++;
		} else if (s->kind == AXIS_STREAM_SUBTITLE) {
			info->subtitle_streams++;
		}
	}
}

AxisEncoderCapabilities *
axis_test_convert_caps_all(void)
{
	AxisEncoderCapabilities *caps = axis_encoder_capabilities_new();
	const AxisConvertEncoder *table = axis_convert_encoders();
	guint i;

	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0000000000000000000000000000000000000000000000000000000000000000");
	caps->aac = caps->mov_text = caps->subrip = TRUE;
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++)
		axis_encoder_capabilities_set(caps, &table[i], TRUE, TRUE, TRUE, NULL);
	return caps;
}

const gchar *const *
axis_test_convert_hostile_texts(void)
{
	/* None of them is an argument the builder makes, whole or in part:
	 * a test may assert that none appears anywhere in a vector. */
	static const gchar *const texts[] = {
		"-i /etc/passwd", "$(rm -rf ~)", "`id`", "; reboot", "| nc evil 1", "-map 0:9", "-f null -",
		"../../media/Films/x.mkv", "/media/disk1/Films/Film (2001).mkv", "fd:5", "file:/etc/shadow",
		"concat:a|b", "line\nbreak", "tab\there", "quote\"s", "it's", "x265-params=colorprim=evil",
		"-hostile-option", "evil-title", "Ünïcødé", NULL
	};

	return texts;
}
