/*
 * axis-sample-args.c - the sample measurement's encoder command for one
 * excerpt
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-sample-args.h"

static void
add(
	GPtrArray   *argv,
	const gchar *arg
){
	g_ptr_array_add(argv, g_strdup(arg));
}

static void
add_seconds(
	GPtrArray *argv,
	gdouble    seconds
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	g_ascii_formatd(buffer, sizeof(buffer), "%.3f", MAX(seconds, 0.0));
	add(argv, buffer);
}

/* The output of the picture encoders: software, NVENC, VA-API.  Only
 * numbers and fixed names are written: nothing from the file. */
static void
video_encoder(
	GPtrArray                *argv,
	const AxisEstimateTarget *target,
	gint                      quality,
	gint                      bit_depth
){
	gint value = axis_estimate_quality_value(target->codec, target->encoder, quality);
	g_autofree gchar *number = g_strdup_printf("%d", value);
	gboolean hevc = target->codec == AXIS_TCODEC_HEVC;
	gboolean ten = hevc && bit_depth >= 10;

	switch (target->encoder) {
	case AXIS_ENCODER_NVENC:
		add(argv, "-c:v");
		add(argv, hevc ? "hevc_nvenc" : "h264_nvenc");
		add(argv, "-preset");
		add(argv, "p5");
		add(argv, "-rc");
		add(argv, "vbr");
		add(argv, "-cq");
		add(argv, number);
		add(argv, "-b:v");
		add(argv, "0");
		add(argv, "-pix_fmt");
		add(argv, ten ? "p010le" : "yuv420p");
		break;
	case AXIS_ENCODER_VAAPI:
		add(argv, "-vf");
		add(argv, ten ? "format=p010,hwupload" : "format=nv12,hwupload");
		add(argv, "-c:v");
		add(argv, hevc ? "hevc_vaapi" : "h264_vaapi");
		add(argv, "-rc_mode");
		add(argv, "CQP");
		add(argv, "-qp");
		add(argv, number);
		break;
	default:
		add(argv, "-c:v");
		add(argv, hevc ? "libx265" : "libx264");
		add(argv, "-preset");
		add(argv, "medium");
		add(argv, "-crf");
		add(argv, number);
		add(argv, "-pix_fmt");
		add(argv, ten ? "yuv420p10le" : "yuv420p");
		if (hevc) {
			add(argv, "-x265-params");
			add(argv, "log-level=error:pools=" AXIS_SAMPLE_ENCODER_THREADS);
		} else {
			add(argv, "-threads");
			add(argv, AXIS_SAMPLE_ENCODER_THREADS);
		}
		break;
	}
	add(argv, "-f");
	add(argv, hevc ? "hevc" : "h264");
}

static void
audio_encoder(
	GPtrArray                *argv,
	const AxisEstimateTarget *target,
	const AxisMediaStream    *audio
){
	gint rate = target->audio_bitrate_mono > 0 && audio->channels == 1 ? target->audio_bitrate_mono :
		target->audio_bitrate;
	g_autofree gchar *bitrate = g_strdup_printf("%d", rate);
	g_autofree gchar *level = g_strdup_printf("%d", axis_estimate_quality_value(target->codec, target->encoder,
		target->quality));

	switch (target->codec) {
	case AXIS_TCODEC_OPUS:
		add(argv, "-c:a");
		add(argv, "libopus");
		add(argv, "-b:a");
		add(argv, bitrate);
		if (rate <= 96000) {
			add(argv, "-application");
			add(argv, "voip");
		}
		add(argv, "-f");
		add(argv, "ogg");
		break;
	case AXIS_TCODEC_MP3: {
		g_autofree gchar *vbr = g_strdup_printf("%d", CLAMP(target->audio_quality, 0, 9));

		add(argv, "-c:a");
		add(argv, "libmp3lame");
		add(argv, "-q:a");
		add(argv, vbr);
		add(argv, "-f");
		add(argv, "mp3");
		break;
	}
	case AXIS_TCODEC_AAC:
		add(argv, "-c:a");
		add(argv, "aac");
		add(argv, "-b:a");
		add(argv, bitrate);
		add(argv, "-f");
		add(argv, "adts");
		break;
	default:
		add(argv, "-c:a");
		add(argv, "flac");
		add(argv, "-compression_level");
		add(argv, level);
		if (target->reduce_to_cd && (audio->bit_depth_audio > 16 || audio->sample_rate > 48000)) {
			add(argv, "-sample_fmt");
			add(argv, "s16");
			if (audio->sample_rate > 48000) {
				add(argv, "-ar");
				add(argv, "44100");
			}
		}
		add(argv, "-f");
		add(argv, "flac");
		break;
	}
}

gchar **
axis_estimate_sample_argv(
	const gchar *const       *prefix,
	const AxisEstimateTarget *target,
	const AxisMediaStream    *stream,
	gint                      quality,
	gint                      bit_depth,
	gdouble                   start,
	gdouble                   seconds
){
	GPtrArray *argv = g_ptr_array_new();
	g_autofree gchar *map = g_strdup_printf("0:%d", stream->index);
	guint i;

	for (i = 0; prefix[i] != NULL; i++)
		add(argv, prefix[i]);
	add(argv, "-nostdin");
	add(argv, "-hide_banner");
	add(argv, "-loglevel");
	add(argv, "error");
	if (target->media == AXIS_TARGET_VIDEO && target->encoder == AXIS_ENCODER_VAAPI) {
		add(argv, "-vaapi_device");
		add(argv, "/dev/dri/renderD128");
	}
	add(argv, "-threads");
	add(argv, AXIS_SAMPLE_ENCODER_THREADS);
	/* The one file given, nothing it may name. */
	add(argv, "-protocol_whitelist");
	add(argv, "fd");
	add(argv, "-format_whitelist");
	add(argv, axis_probe_format_whitelist());
	add(argv, "-fd");
	add(argv, "3");
	add(argv, "-ss");
	add_seconds(argv, start);
	add(argv, "-t");
	add_seconds(argv, seconds);
	add(argv, "-i");
	add(argv, "fd:");
	add(argv, "-map");
	add(argv, map);
	add(argv, "-sn");
	add(argv, "-dn");
	if (target->media == AXIS_TARGET_VIDEO) {
		add(argv, "-an");
		video_encoder(argv, target, quality, bit_depth);
	} else {
		add(argv, "-vn");
		audio_encoder(argv, target, stream);
	}
	add(argv, "pipe:1");
	g_ptr_array_add(argv, NULL);
	return (gchar **)g_ptr_array_free(argv, FALSE);
}
