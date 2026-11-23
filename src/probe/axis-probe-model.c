/*
 * axis-probe-model.c - the model's types, names and fixed tables
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "probe/axis-probe-model.h"

#include <string.h>

/* ── What the tools may read ────────────────────────────────────── */

/* The containers and bare streams ffprobe and ffmpeg may be asked to read.
 * It excludes everything that names other files or addresses (concat, hls,
 * dash, sdp, rtsp, image2, ffmetadata, the playlist and subtitle-list
 * formats) and the demuxers that run other software (vapoursynth, lavfi,
 * the module and game music players).  A file of any other format is
 * refused with "Format not on whitelist": it is recorded as failed. */
static const gchar format_whitelist[] =
	"matroska,webm,mov,mp4,m4a,3gp,3g2,mj2,avi,mpegts,mpegtsraw,mpeg,mpegvideo,asf,asf_o,flv,"
	"ogg,flac,mp3,wav,w64,aiff,aac,ac3,eac3,ac4,dts,dtshd,truehd,mlp,ape,wv,tta,tak,caf,amr,"
	"amrnb,amrwb,au,dsf,dss,dv,ivf,h264,hevc,vvc,m4v,av1,obu,vc1,vc1test,nut,rm,mxf,gxf,wtv,"
	"nsv,mpc,mpc8,swf,sox,voc,shn,osq,oma,aa,aax,iff,gsm,sbc,dfpwm,ircam,mmf,msf,wsd,kux,"
	"dnxhd,yuv4mpegpipe";

const gchar *
axis_probe_format_whitelist(void)
{
	return format_whitelist;
}

gchar **
axis_probe_tool_argv(
	const gchar *const *prefix,
	gboolean            frames,
	gint                stream_index
){
	GPtrArray *argv = g_ptr_array_new();
	guint i;

	for (i = 0; prefix != NULL && prefix[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(prefix[i]));

	/* Quiet; only the one protocol (the descriptor given to the child)
	 * and only containers that cannot name other files; the file is
	 * descriptor 3, never a name. */
	g_ptr_array_add(argv, g_strdup("-v"));
	g_ptr_array_add(argv, g_strdup("error"));
	g_ptr_array_add(argv, g_strdup("-hide_banner"));
	g_ptr_array_add(argv, g_strdup("-protocol_whitelist"));
	g_ptr_array_add(argv, g_strdup("fd"));
	g_ptr_array_add(argv, g_strdup("-format_whitelist"));
	g_ptr_array_add(argv, g_strdup(format_whitelist));
	g_ptr_array_add(argv, g_strdup("-fd"));
	g_ptr_array_add(argv, g_strdup("3"));
	g_ptr_array_add(argv, g_strdup("-threads"));
	g_ptr_array_add(argv, g_strdup("1"));
	g_ptr_array_add(argv, g_strdup("-print_format"));
	g_ptr_array_add(argv, g_strdup("json"));
	g_ptr_array_add(argv, g_strdup("-show_error"));
	if (frames) {
		g_ptr_array_add(argv, g_strdup("-select_streams"));
		g_ptr_array_add(argv, g_strdup_printf("%d", stream_index));
		g_ptr_array_add(argv, g_strdup("-show_frames"));
		g_ptr_array_add(argv, g_strdup("-read_intervals"));
		g_ptr_array_add(argv, g_strdup("%+#1"));
		g_ptr_array_add(argv, g_strdup("-show_entries"));
		g_ptr_array_add(argv, g_strdup("frame=pix_fmt,color_space,color_primaries,color_transfer,"
			"side_data_list"));
	} else {
		g_ptr_array_add(argv, g_strdup("-show_format"));
		g_ptr_array_add(argv, g_strdup("-show_streams"));
		g_ptr_array_add(argv, g_strdup("-show_chapters"));
	}
	g_ptr_array_add(argv, g_strdup("-i"));
	g_ptr_array_add(argv, g_strdup("fd:"));
	g_ptr_array_add(argv, NULL);

	return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* ── HDR10 static metadata ──────────────────────────────────────── */

gboolean
axis_hdr10_metadata_equal(
	const AxisHdr10Metadata *a,
	const AxisHdr10Metadata *b
){
	if ((a->mastering != FALSE) != (b->mastering != FALSE) || (a->light != FALSE) != (b->light != FALSE))
		return FALSE;
	if (a->mastering && (a->red_x != b->red_x || a->red_y != b->red_y || a->green_x != b->green_x ||
	    a->green_y != b->green_y || a->blue_x != b->blue_x || a->blue_y != b->blue_y || a->white_x != b->white_x ||
	    a->white_y != b->white_y || a->min_luminance != b->min_luminance || a->max_luminance != b->max_luminance))
		return FALSE;
	if (a->light && (a->max_content != b->max_content || a->max_average != b->max_average))
		return FALSE;
	return TRUE;
}

static gboolean
chroma_ok(gint value)
{
	return value >= 0 && value <= AXIS_HDR10_CHROMA_MAX;
}

gboolean
axis_hdr10_metadata_valid(const AxisHdr10Metadata *m)
{
	if (m->mastering) {
		if (!chroma_ok(m->red_x) || !chroma_ok(m->red_y) || !chroma_ok(m->green_x) || !chroma_ok(m->green_y) ||
		    !chroma_ok(m->blue_x) || !chroma_ok(m->blue_y) || !chroma_ok(m->white_x) || !chroma_ok(m->white_y))
			return FALSE;
		if (m->max_luminance < AXIS_HDR10_MAX_LUMINANCE_LOWEST || m->max_luminance > AXIS_HDR10_MAX_LUMINANCE_HIGHEST)
			return FALSE;
		if (m->min_luminance < 0 || m->min_luminance > AXIS_HDR10_MIN_LUMINANCE_HIGHEST ||
		    m->min_luminance >= m->max_luminance)
			return FALSE;
	}
	if (m->light) {
		if (m->max_content < 0 || m->max_content > AXIS_HDR10_LIGHT_MAX || m->max_average < 0 ||
		    m->max_average > AXIS_HDR10_LIGHT_MAX)
			return FALSE;
		if (m->max_content > 0 && m->max_average > 0 && m->max_average > m->max_content)
			return FALSE;
	}
	return TRUE;
}

/* ── Stream kinds ───────────────────────────────────────────────── */

static const gchar *const kind_names[] = {
	"video", "audio", "subtitle", "attachment", "data", "unknown"
};

const gchar *
axis_stream_kind_to_string(AxisStreamKind kind)
{
	return (guint)kind < G_N_ELEMENTS(kind_names) ? kind_names[kind] : "unknown";
}

gboolean
axis_stream_kind_from_string(
	const gchar    *text,
	AxisStreamKind *kind
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(kind_names); i++) {
		if (strcmp(text, kind_names[i]) == 0) {
			*kind = (AxisStreamKind)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

void
axis_media_stream_free(AxisMediaStream *stream)
{
	if (stream == NULL)
		return;

	g_free(stream->codec);
	g_free(stream->profile);
	g_free(stream->language);
	g_free(stream->title);
	g_free(stream->pixel_format);
	g_free(stream->field_order);
	g_free(stream->color_range);
	g_free(stream->color_space);
	g_free(stream->color_transfer);
	g_free(stream->color_primaries);
	g_free(stream->channel_layout);
	g_free(stream->sample_format);
	g_free(stream);
}

AxisMediaInfo *
axis_media_info_new(void)
{
	AxisMediaInfo *info = g_new0(AxisMediaInfo, 1);

	info->probe_version = AXIS_PROBE_VERSION;
	info->probe_ms = AXIS_PROBE_UNKNOWN;
	info->duration_ms = AXIS_PROBE_UNKNOWN;
	info->bitrate = AXIS_PROBE_UNKNOWN;
	info->chapter_count = AXIS_PROBE_UNKNOWN;
	info->stream_count = AXIS_PROBE_UNKNOWN;
	info->streams = g_ptr_array_new_with_free_func((GDestroyNotify)axis_media_stream_free);
	info->main_video = -1;
	info->main_audio = -1;
	info->width = AXIS_PROBE_UNKNOWN;
	info->height = AXIS_PROBE_UNKNOWN;
	info->bit_depth = AXIS_PROBE_UNKNOWN;
	info->frame_rate = -1.0;
	info->bits_per_pixel = -1.0;
	info->interlaced = AXIS_TRI_UNKNOWN;
	info->vfr_suspected = AXIS_TRI_UNKNOWN;
	info->audio_channels = AXIS_PROBE_UNKNOWN;
	info->audio_sample_rate = AXIS_PROBE_UNKNOWN;
	info->audio_bit_depth = AXIS_PROBE_UNKNOWN;
	info->audio_lossless = AXIS_TRI_UNKNOWN;
	info->audio_bitrate_total = AXIS_PROBE_UNKNOWN;
	info->art_width = AXIS_PROBE_UNKNOWN;
	info->art_height = AXIS_PROBE_UNKNOWN;
	/* Until axis_probe_map_frames() reads one. */
	info->frames_unavailable = TRUE;

	return info;
}

void
axis_media_info_free(AxisMediaInfo *info)
{
	if (info == NULL)
		return;

	g_free(info->status);
	g_free(info->error);
	g_free(info->tool_version);
	g_free(info->container);
	if (info->streams != NULL)
		g_ptr_array_unref(info->streams);
	g_free(info->video_codec);
	g_free(info->audio_codec);
	g_free(info->tag_title);
	g_free(info->tag_artist);
	g_free(info->tag_album);
	g_free(info->album_key);
	g_free(info->hdr_note_owned);
	g_free(info);
}

/* ── Resolution ─────────────────────────────────────────────────── */

const gchar *
axis_probe_resolution_class(
	gint width,
	gint height
){
	gint w;
	gint h;

	if (width <= 0 || height <= 0)
		return NULL;
	/* Pictures are taken as landscape: a portrait one is the same class
	 * turned. */
	w = MAX(width, height);
	h = MIN(width, height);

	if (w < 160 || h < 90)
		return "other";
	if (w >= 3400 && w <= 4400)
		return "2160p";
	if (w >= 1700 && w <= 2100)
		return "1080p";
	if (w >= 1200 && w <= 1400)
		return "720p";
	if (w <= 1100)
		return "sd";
	/* Anamorphic HD (1440x1080) and the like are named by their height. */
	if (h >= 1000 && h <= 1100 && w <= 2100)
		return "1080p";
	if (h >= 680 && h <= 760 && w <= 1700)
		return "720p";
	return "other";
}

const gchar *
axis_probe_resolution_label(const gchar *resolution_class)
{
	if (g_strcmp0(resolution_class, "sd") == 0)
		return "SD";
	if (g_strcmp0(resolution_class, "720p") == 0)
		return "720p";
	if (g_strcmp0(resolution_class, "1080p") == 0)
		return "1080p";
	if (g_strcmp0(resolution_class, "2160p") == 0)
		return "2160p (4K)";
	if (g_strcmp0(resolution_class, "other") == 0)
		return "Other size";
	return "Unknown size";
}

/* ── Audio: lossless or lossy ───────────────────────────────────── */

gint
axis_probe_audio_lossless(
	const gchar *codec,
	const gchar *profile
){
	static const gchar *const lossless[] = {
		"flac", "alac", "ape", "tta", "tak", "truehd", "mlp", "wmalossless", "shorten", "mp4als",
		"als", "dsd_lsbf", "dsd_msbf", "dsd_lsbf_planar", "dsd_msbf_planar", NULL
	};
	static const gchar *const lossy[] = {
		"mp3", "mp3float", "mp3adu", "mp3on4", "mp2", "mp1", "aac", "aac_latm", "vorbis", "opus", "ac3",
		"eac3", "wmav1", "wmav2", "wmapro", "wmavoice", "amr_nb", "amr_wb", "speex", "atrac1",
		"atrac3", "atrac3p", "atrac9", "cook", "ra_144", "ra_288", "sipr", "qcelp", "evrc", "gsm",
		"gsm_ms", "g723_1", "g729", "nellymoser", "musepack7", "musepack8", "ilbc", "truespeech",
		"pcm_alaw", "pcm_mulaw", "pcm_vidc", NULL
	};
	guint i;

	if (codec == NULL)
		return AXIS_TRI_UNKNOWN;

	for (i = 0; lossless[i] != NULL; i++) {
		if (strcmp(codec, lossless[i]) == 0)
			return AXIS_TRI_YES;
	}
	for (i = 0; lossy[i] != NULL; i++) {
		if (strcmp(codec, lossy[i]) == 0)
			return AXIS_TRI_NO;
	}
	/* Companded 8-bit PCM is lossy; the rest of PCM is not. */
	if (g_str_has_prefix(codec, "pcm_"))
		return AXIS_TRI_YES;
	/* ADPCM codecs are 4:1 compression. */
	if (g_str_has_prefix(codec, "adpcm_"))
		return AXIS_TRI_NO;
	if (strcmp(codec, "dts") == 0) {
		if (profile == NULL)
			return AXIS_TRI_UNKNOWN;
		if (g_str_has_prefix(profile, "DTS-HD MA"))
			return AXIS_TRI_YES;
		if (strcmp(profile, "DTS") == 0 || strcmp(profile, "DTS-ES") == 0 ||
		    strcmp(profile, "DTS 96/24") == 0 || g_str_has_prefix(profile, "DTS-HD HRA") ||
		    strcmp(profile, "DTS Express") == 0)
			return AXIS_TRI_NO;
		return AXIS_TRI_UNKNOWN;
	}
	/* WavPack may be a lossy hybrid; anything else is not in the table. */
	return AXIS_TRI_UNKNOWN;
}

/* ── Subtitles ──────────────────────────────────────────────────── */

AxisSubtitleKind
axis_probe_subtitle_kind(const gchar *codec)
{
	static const gchar *const text[] = {
		"subrip", "srt", "ass", "ssa", "webvtt", "mov_text", "text", "subviewer", "subviewer1",
		"microdvd", "mpl2", "jacosub", "sami", "realtext", "stl", "vplayer", "pjs", "ttml",
		"hdmv_text_subtitle", NULL
	};
	static const gchar *const image[] = {
		"hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", "xsub", NULL
	};
	guint i;

	if (codec == NULL)
		return AXIS_SUBTITLE_UNKNOWN;
	for (i = 0; text[i] != NULL; i++) {
		if (strcmp(codec, text[i]) == 0)
			return AXIS_SUBTITLE_TEXT;
	}
	for (i = 0; image[i] != NULL; i++) {
		if (strcmp(codec, image[i]) == 0)
			return AXIS_SUBTITLE_IMAGE;
	}
	return AXIS_SUBTITLE_UNKNOWN;
}

/* ── Pixel formats ──────────────────────────────────────────────── */

gint
axis_probe_pixel_format_depth(const gchar *pixel_format)
{
	g_autofree gchar *name = NULL;
	gsize length;
	gsize start;
	gint depth;

	if (pixel_format == NULL || pixel_format[0] == '\0' || strlen(pixel_format) > 32)
		return AXIS_PROBE_UNKNOWN;

	name = g_ascii_strdown(pixel_format, -1);
	length = strlen(name);
	if (length > 2 && (g_str_has_suffix(name, "le") || g_str_has_suffix(name, "be"))) {
		length -= 2;
		name[length] = '\0';
	}

	/* nv12, nv16, nv21, nv24, nv42: 8 bits. */
	if (g_str_has_prefix(name, "nv"))
		return 8;
	/* p010, p016, p210, p216, p410, p416. */
	if (length == 4 && name[0] == 'p' && g_ascii_isdigit(name[1]) && g_ascii_isdigit(name[2]) &&
	    g_ascii_isdigit(name[3])) {
		depth = (name[2] - '0') * 10 + (name[3] - '0');
		return depth == 10 || depth == 16 ? depth : AXIS_PROBE_UNKNOWN;
	}
	if (!g_str_has_prefix(name, "yuv") && !g_str_has_prefix(name, "gray") &&
	    !g_str_has_prefix(name, "gbr") && !g_str_has_prefix(name, "ya"))
		return AXIS_PROBE_UNKNOWN;

	/* The digits at the end, if any: "yuv420p10" -> 10, "yuv420p" -> none. */
	start = length;
	while (start > 0 && g_ascii_isdigit(name[start - 1]))
		start--;
	if (start == length)
		return 8;
	/* "yuv420" would be all digits after the prefix; require a letter
	 * before the run. */
	if (start == 0 || !g_ascii_isalpha(name[start - 1]))
		return AXIS_PROBE_UNKNOWN;
	if (length - start > 2)
		return AXIS_PROBE_UNKNOWN;
	depth = (gint)g_ascii_strtoll(name + start, NULL, 10);
	if (depth == 8 || depth == 9 || depth == 10 || depth == 12 || depth == 14 || depth == 16)
		return depth;
	return AXIS_PROBE_UNKNOWN;
}
