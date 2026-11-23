/*
 * fuzz-convert-plan.c - libFuzzer target for the conversion plan builder,
 * the argument vector's grammar and the check of an output against its plan
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The bytes become a profile definition (fuzz-profile-gen.h), a probe
 * record of any shape whose text (titles, languages, codec names) is of any
 * kind and marked, the machine's capabilities and settings, and an encoder
 * family.  Whatever they are:
 *
 *  - a file the fixed rules refuse gets no plan, and no refusal is ever
 *    "internal" (the builder's own vector failing its grammar);
 *  - a plan's vector passes axis_convert_args_valid() and carries nothing
 *    of the file (no marked text) and no character outside the grammar's
 *    alphabet; its colour and HDR10 arguments are the source's, exactly;
 *  - every stream of the file is mapped or dropped with a reason, never
 *    both, and mapped once (twice only as the source of an added stereo
 *    track); the maps are in the order of the output's streams, which is
 *    the fixed one, each with exactly one codec and one disposition;
 *  - the plan is one the capabilities allow, for the build measured;
 *  - choosing gives the plan of the first family that has one;
 *  - an output made exactly as the plan says passes the output check and
 *    the picture check, also when it says so in other words that mean the
 *    same (a range left unstated said as limited, "unknown" for unstated,
 *    an SDR picture of 10 bits with no transfer "not classified"), and one
 *    with any single property changed fails the output check, and the
 *    picture check too when it is the picture's;
 *  - a vector changed anywhere that the grammar still accepts is still
 *    safe: one input and one output, both descriptors with only `fd`
 *    allowed, every map a stream of the file, only options the builder
 *    uses, nothing of the file and no path.
 *
 * The checks are written from the specification, apart from the builder;
 * they share its colour name table and format whitelist only.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz-profile-gen.h"
#include "probe/axis-probe-model.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-estimate-target.h"
#include "transcode/axis-profile.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* In every text of the record that comes from the file: no argument may
 * contain it. */
#define MARK "zzlibrarytextzz"

static G_GNUC_NORETURN void
broken(const gchar *what)
{
	g_printerr("conversion plan property: %s\n", what);
	abort();
}

static gboolean
in_list(
	const gchar        *value,
	const gchar *const *list
){
	guint i;

	for (i = 0; value != NULL && list[i] != NULL; i++) {
		if (strcmp(value, list[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* ── The file ───────────────────────────────────────────────────── */

static gchar *
pick_text(
	AxisGenInput       *in,
	const gchar *const *table,
	guint               n
){
	guint k = axis_gen_pick(in, n + 1);

	return k < n ? g_strdup(table[k]) : NULL;
}

static const gchar *const languages[] = {
	"eng", "fre", "ger", "jpn", "spa", "ita", "und", "", "fra", MARK, "e\ng" MARK, "-i " MARK
};
static const gchar *const titles[] = {
	MARK, "Commentary " MARK, "-i /etc/passwd " MARK, "$(" MARK ")", "`" MARK "`", "; reboot " MARK,
	"-map 0:9 " MARK, "fd:5 " MARK, "file:/etc/shadow " MARK, "line\nbreak " MARK, "quote\"s " MARK,
	"x265-params=colorprim=" MARK, "Caf\xc3\xa9 " MARK
};
static const gchar *const video_codecs[] = { "h264", "hevc", "mpeg2video", "vc1", "av1", MARK };
static const gchar *const audio_codecs[] = {
	"aac", "aac", "ac3", "eac3", "truehd", "dts", "flac", "opus", "mp3", "alac", "pcm_s24le", "pcm_bluray", MARK
};
static const gchar *const subtitle_codecs[] = {
	"subrip", "srt", "ass", "ssa", "webvtt", "mov_text", "text", "hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle",
	"eia_608", MARK
};
static const gchar *const attachment_codecs[] = { "ttf", "otf", MARK };
static const gchar *const cover_codecs[] = { "mjpeg", "png", "bmp", MARK };
static const gchar *const pixel_formats[] = {
	"yuv420p", "yuvj420p", "yuv420p10le", "yuv420p10be", "yuv420p12le", "nv12", "p010le", "yuv422p10le", "yuv444p",
	"gray", MARK
};
static const gchar *const field_orders[] = { "progressive", "progressive", "progressive", "tt", "bb", "tb", "unknown" };

/* The 4:2:0 layouts of 8 to 12 bits the targets take (the specification's
 * list, written again here). */
static const gchar *const four_two_zero[] = {
	"yuv420p", "yuvj420p", "yuv420p10le", "yuv420p10be", "yuv420p12le", "yuv420p12be", "nv12", "nv21", "p010le",
	"p010be", NULL
};

static gchar *
gen_colour(
	AxisGenInput *in,
	const gchar  *property
){
	static const gchar *const odd[] = { "unknown", "unspecified", "reserved", "", "jedec-p22", "ycgco", MARK };
	const gchar *const *names = axis_convert_colour_names(property);
	guint8 b = axis_gen_byte(in);
	guint n = 0;

	while (names != NULL && names[n] != NULL)
		n++;
	if (n == 0)
		broken("a colour property without names");
	if (b < 200)
		return g_strdup(names[b % n]);
	if (b < 240)
		return NULL;
	return g_strdup(odd[b % G_N_ELEMENTS(odd)]);
}

/* HDR10's values: mostly those of a real film, sometimes one of them off
 * (in range or out of it), sometimes none, sometimes the light levels only. */
static void
gen_hdr10(
	AxisGenInput      *in,
	AxisHdr10Metadata *m
){
	guint8 b = axis_gen_byte(in);

	memset(m, 0, sizeof(*m));
	if (b % 8 == 0)
		return;
	m->mastering = b % 8 != 1;
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
	m->light = (b & 0x08) == 0;
	m->max_content = 1000;
	m->max_average = 400;
	switch (axis_gen_byte(in) % 8) {
	case 0:
		m->red_x = (gint)axis_gen_byte(in) * 200;
		break;
	case 1:
		m->min_luminance = (gint64)axis_gen_byte(in) * 250;
		break;
	case 2:
		m->max_luminance = (gint64)axis_gen_byte(in) << 20;
		break;
	case 3:
		m->max_content = (gint)axis_gen_byte(in) * 300;
		m->max_average = (gint)axis_gen_byte(in) * 300;
		break;
	case 4:
		m->white_y = -1;
		break;
	default:
		break;
	}
}

static AxisMediaStream *
stream_new(
	AxisStreamKind kind,
	gint           index
){
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);

	s->index = index;
	s->kind = kind;
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
	return s;
}

static void
gen_picture(
	AxisGenInput    *in,
	AxisMediaStream *s
){
	static const gint sizes[][2] = {
		{ 1920, 1080 }, { 3840, 2160 }, { 1280, 720 }, { 720, 576 }, { 320, 240 }, { -1, -1 }, { 0, 0 }, { 1920, -1 }
	};
	static const gint odd_depths[] = { -1, 7, 16, 10, 8 };
	guint size = axis_gen_pick(in, G_N_ELEMENTS(sizes));
	guint8 b;

	s->codec = pick_text(in, video_codecs, G_N_ELEMENTS(video_codecs));
	s->width = sizes[size][0];
	s->height = sizes[size][1];
	s->pixel_format = g_strdup(pixel_formats[axis_gen_pick(in, G_N_ELEMENTS(pixel_formats))]);
	if (strstr(s->pixel_format, "12") != NULL)
		s->bit_depth = 12;
	else if (strstr(s->pixel_format, "10") != NULL)
		s->bit_depth = 10;
	else
		s->bit_depth = 8;
	if (axis_gen_pick(in, 8) == 0)
		s->bit_depth = odd_depths[axis_gen_pick(in, G_N_ELEMENTS(odd_depths))];
	s->frame_rate = axis_gen_pick(in, 8) == 0 ? -1.0 : 24000.0 / 1001.0;
	s->real_frame_rate = s->frame_rate;
	s->field_order = pick_text(in, field_orders, G_N_ELEMENTS(field_orders));
	s->color_primaries = gen_colour(in, "primaries");
	s->color_transfer = gen_colour(in, "transfer");
	s->color_space = gen_colour(in, "matrix");
	s->color_range = gen_colour(in, "range");
	b = axis_gen_byte(in);
	if (b == 255)
		s->hdr_format = (AxisHdrFormat)9;
	else if (b % 16 < 7)
		s->hdr_format = AXIS_HDR_SDR;
	else if (b % 16 < 11)
		s->hdr_format = AXIS_HDR_HDR10;
	else if (b % 16 < 13)
		s->hdr_format = AXIS_HDR_HLG;
	else if (b % 16 == 13)
		s->hdr_format = AXIS_HDR_NONE;
	else if (b % 16 == 14)
		s->hdr_format = AXIS_HDR_DOLBY_VISION;
	else
		s->hdr_format = AXIS_HDR_HDR10PLUS;
	s->hdr_uncertain = axis_gen_pick(in, 16) == 0;
	gen_hdr10(in, &s->hdr10);
	s->signals.hdr10 = s->hdr10;
	s->signals.has_mastering = s->hdr10.mastering;
	s->signals.has_content_light = s->hdr10.light;
}

typedef enum
{
	ROLE_PICTURE,
	ROLE_AUDIO,
	ROLE_SUBTITLE,
	ROLE_ATTACHMENT,
	ROLE_COVER,
	ROLE_DATA,
	ROLE_UNKNOWN
} Role;

static Role
gen_role(
	AxisGenInput *in,
	guint         position
){
	guint8 b = axis_gen_byte(in) % 32;

	if (position == 0)
		return b == 0 ? ROLE_AUDIO : ROLE_PICTURE;
	if (b < 12 || b == 31)
		return ROLE_AUDIO;
	if (b < 22)
		return ROLE_SUBTITLE;
	if (b < 25)
		return ROLE_ATTACHMENT;
	if (b < 28)
		return ROLE_COVER;
	if (b == 28)
		return ROLE_PICTURE;
	return b == 29 ? ROLE_DATA : ROLE_UNKNOWN;
}

/* The main picture, as the specification says: the largest video stream
 * that is not a cover; and how many such there are. */
static const AxisMediaStream *
find_picture(
	const AxisMediaInfo *info,
	guint               *pictures
){
	const AxisMediaStream *best = NULL;
	gint64 best_pixels = -1;
	guint i;

	*pictures = 0;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		gint64 pixels;

		if (s->kind != AXIS_STREAM_VIDEO || s->attached_pic)
			continue;
		(*pictures)++;
		pixels = s->width > 0 && s->height > 0 ? (gint64)s->width * s->height : 0;
		if (pixels > best_pixels) {
			best_pixels = pixels;
			best = s;
		}
	}
	return best;
}

static AxisMediaInfo *
gen_info(AxisGenInput *in)
{
	static const gint chapters[] = { -1, 0, 0, 3, 12, 255 };
	static const gint64 durations[] = { -1, 1000, 5400000 };
	static const gint interlaced[] = {
		AXIS_TRI_NO, AXIS_TRI_NO, AXIS_TRI_NO, AXIS_TRI_NO, AXIS_TRI_NO, AXIS_TRI_NO, AXIS_TRI_UNKNOWN, AXIS_TRI_YES
	};
	AxisMediaInfo *info = axis_media_info_new();
	const AxisMediaStream *picture;
	guint pictures = 0;
	guint n = 1 + axis_gen_pick(in, 9);
	guint i;

	info->status = g_strdup(axis_gen_pick(in, 16) != 0 ? "ok" : "failed");
	info->container = g_strdup("matroska,webm");
	info->streams_omitted = axis_gen_pick(in, 16) == 0;
	info->interlaced = interlaced[axis_gen_pick(in, G_N_ELEMENTS(interlaced))];
	info->chapter_count = chapters[axis_gen_pick(in, G_N_ELEMENTS(chapters))];
	info->duration_ms = durations[axis_gen_pick(in, G_N_ELEMENTS(durations))];
	info->frames_unavailable = FALSE;

	for (i = 0; i < n; i++) {
		Role role = gen_role(in, i);
		guint8 b = axis_gen_byte(in);
		gint index = (gint)i;
		AxisMediaStream *s;

		/* Sometimes an index the probe would not write: another stream's,
		 * negative, at the limit, past it. */
		if (b % 32 == 0 && i > 0)
			index = (gint)i - 1;
		else if (b % 32 == 1)
			index = -1;
		else if (b % 64 == 2)
			index = 100000;
		else if (b % 64 == 3)
			index = 100001;

		switch (role) {
		case ROLE_PICTURE:
			s = stream_new(AXIS_STREAM_VIDEO, index);
			gen_picture(in, s);
			break;
		case ROLE_AUDIO: {
			static const gint channels[] = { -1, 1, 2, 2, 6, 8 };

			s = stream_new(AXIS_STREAM_AUDIO, index);
			s->codec = pick_text(in, audio_codecs, G_N_ELEMENTS(audio_codecs));
			s->channels = channels[axis_gen_pick(in, G_N_ELEMENTS(channels))];
			s->sample_rate = 48000;
			break;
		}
		case ROLE_SUBTITLE:
			s = stream_new(AXIS_STREAM_SUBTITLE, index);
			s->codec = pick_text(in, subtitle_codecs, G_N_ELEMENTS(subtitle_codecs));
			s->subtitle_kind = axis_gen_pick(in, 16) != 0 ? axis_probe_subtitle_kind(s->codec) :
				(AxisSubtitleKind)axis_gen_pick(in, 3);
			break;
		case ROLE_ATTACHMENT:
			s = stream_new(AXIS_STREAM_ATTACHMENT, index);
			s->codec = pick_text(in, attachment_codecs, G_N_ELEMENTS(attachment_codecs));
			break;
		case ROLE_COVER:
			s = stream_new(AXIS_STREAM_VIDEO, index);
			s->codec = pick_text(in, cover_codecs, G_N_ELEMENTS(cover_codecs));
			s->attached_pic = TRUE;
			s->width = 600;
			s->height = 900;
			break;
		case ROLE_DATA:
			s = stream_new(AXIS_STREAM_DATA, index);
			s->codec = g_strdup("bin_data");
			break;
		default:
			s = stream_new(AXIS_STREAM_UNKNOWN, index);
			break;
		}
		s->language = pick_text(in, languages, G_N_ELEMENTS(languages));
		s->title = axis_gen_pick(in, 2) ? pick_text(in, titles, G_N_ELEMENTS(titles)) : NULL;
		s->is_default = axis_gen_pick(in, 2);
		s->is_forced = axis_gen_pick(in, 4) == 0;
		g_ptr_array_add(info->streams, s);
	}

	/* The file's facts, as the mapper derives them, or (rarely) not. */
	info->stream_count = (gint)info->streams->len;
	info->hdr_format = AXIS_HDR_NONE;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind == AXIS_STREAM_VIDEO && !s->attached_pic && s->hdr_format > info->hdr_format)
			info->hdr_format = s->hdr_format;
	}
	if (axis_gen_pick(in, 16) == 0)
		info->hdr_format = (AxisHdrFormat)axis_gen_pick(in, 8);
	info->hdr_uncertain = axis_gen_pick(in, 16) == 0;
	picture = find_picture(info, &pictures);
	if (picture != NULL && picture->width > 0 && picture->height > 0) {
		info->width = picture->width;
		info->height = picture->height;
		info->resolution_class = axis_probe_resolution_class(picture->width, picture->height);
	}
	return info;
}

/* ── The machine ────────────────────────────────────────────────── */

static AxisEncoderCapabilities *
gen_caps(AxisGenInput *in)
{
	const AxisConvertEncoder *table = axis_convert_encoders();
	AxisEncoderCapabilities *caps;
	guint i;

	if (axis_gen_pick(in, 16) == 0)
		return NULL;
	caps = axis_encoder_capabilities_new();
	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = axis_gen_pick(in, 16) != 0 ?
		g_strdup("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") : NULL;
	caps->aac = axis_gen_pick(in, 8) != 0;
	caps->mov_text = axis_gen_pick(in, 8) != 0;
	caps->subrip = axis_gen_pick(in, 8) != 0;
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		guint8 b = axis_gen_byte(in);
		gboolean usable = (b & 7) != 1;
		gboolean ten = (b & 0x18) != 0x08;
		gboolean hdr10 = (b & 0x60) != 0x20;

		if ((b & 7) == 0)
			continue;
		axis_encoder_capabilities_set(caps, &table[i], usable, ten, hdr10,
			usable && ten && hdr10 ? NULL : "Its test encode came out otherwise.");
	}
	return caps;
}

/* The settings, and whether every one of them is in the table. */
static gboolean
gen_options(
	AxisGenInput       *in,
	AxisConvertOptions *options,
	gboolean           *given
){
	static const gchar *const software[] = { "medium", "ultrafast", "veryslow", "placebo", MARK };
	static const gchar *const nvenc[] = { "p1", "p7", "p8", "slow" };
	static const gchar *const devices[] = {
		"/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/sda", "/dev/dri/renderD12", "/dev/dri/renderD1234",
		"/dev/dri/renderDabc", "/dev/dri/../sda1"
	};
	static const guint threads[] = { 0, 1, 8, 64, 65 };
	guint k;
	gboolean ok = TRUE;

	memset(options, 0, sizeof(*options));
	*given = axis_gen_pick(in, 2) == 0;
	if (!*given)
		return TRUE;
	k = axis_gen_pick(in, G_N_ELEMENTS(software) + 1);
	if (k < G_N_ELEMENTS(software)) {
		options->software_preset = software[k];
		ok = ok && k < 3;
	}
	k = axis_gen_pick(in, G_N_ELEMENTS(nvenc) + 1);
	if (k < G_N_ELEMENTS(nvenc)) {
		options->nvenc_preset = nvenc[k];
		ok = ok && k < 2;
	}
	k = axis_gen_pick(in, G_N_ELEMENTS(devices) + 1);
	if (k < G_N_ELEMENTS(devices)) {
		options->vaapi_device = devices[k];
		ok = ok && k < 2;
	}
	options->threads = threads[axis_gen_pick(in, G_N_ELEMENTS(threads))];
	return ok && options->threads <= 64;
}

/* ── The rules ──────────────────────────────────────────────────── */

static gboolean
unstated(const gchar *value)
{
	return value == NULL || value[0] == '\0' || strcmp(value, "unknown") == 0 || strcmp(value, "unspecified") == 0 ||
		strcmp(value, "reserved") == 0;
}

/* The language rule: a track of a known language the profile does not
 * list is dropped; one of no language, or "und", never. */
static gboolean
dropped_by_language(
	const AxisProfileDefinition *def,
	const AxisMediaStream       *s
){
	guint i;

	if (s->kind != AXIS_STREAM_AUDIO || def->n_languages == 0 || s->language == NULL || s->language[0] == '\0' ||
	    strcmp(s->language, "und") == 0)
		return FALSE;
	for (i = 0; i < def->n_languages; i++) {
		if (strcmp(s->language, def->languages[i]) == 0)
			return FALSE;
	}
	return TRUE;
}

static gboolean
stereo_aac(const AxisMediaStream *s)
{
	return g_strcmp0(s->codec, "aac") == 0 && s->channels >= 1 && s->channels <= 2;
}

/* Why the file can have no plan whatever the encoder, or %NULL. */
static const gchar *
must_refuse(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	const AxisEncoderCapabilities *caps,
	gboolean                       options_ok
){
	gboolean mp4 = def->container == AXIS_CONTAINER_MP4;
	gboolean has_audio = FALSE;
	gboolean kept_audio = FALSE;
	const AxisMediaStream *picture;
	guint pictures = 0;
	guint i;
	guint j;

	if (def->kind != AXIS_PROFILE_KIND_VIDEO)
		return "an audio profile";
	if (!options_ok)
		return "settings outside the table";
	if (g_strcmp0(info->status, "ok") != 0 || info->streams_omitted != 0)
		return "a failed probe, or one that left streams out";
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_VIDEO && s->kind != AXIS_STREAM_AUDIO && s->kind != AXIS_STREAM_SUBTITLE &&
		    s->kind != AXIS_STREAM_ATTACHMENT)
			return "a data stream, or one of unknown kind";
		if (s->index < 0 || s->index > 100000)
			return "an index the probe does not write";
		for (j = 0; j < i; j++) {
			if (((const AxisMediaStream *)g_ptr_array_index(info->streams, j))->index == s->index)
				return "two records of one stream index";
		}
		if (s->kind == AXIS_STREAM_AUDIO) {
			has_audio = TRUE;
			if (!dropped_by_language(def, s))
				kept_audio = TRUE;
		}
		if (s->kind == AXIS_STREAM_VIDEO && s->attached_pic && !mp4)
			return "a cover into Matroska";
		if (s->kind == AXIS_STREAM_ATTACHMENT && mp4)
			return "an attachment into MP4";
	}
	if (has_audio && !kept_audio)
		return "the language rule would leave no sound";
	picture = find_picture(info, &pictures);
	if (pictures != 1 || picture->codec == NULL || picture->width <= 0 || picture->height <= 0)
		return "not exactly one picture, of known codec and size";
	if (picture->hdr_uncertain || info->hdr_uncertain)
		return "HDR in doubt";
	if (picture->hdr_format != AXIS_HDR_SDR && picture->hdr_format != AXIS_HDR_HLG &&
	    picture->hdr_format != AXIS_HDR_HDR10)
		return "a picture that is not SDR, HLG or HDR10";
	if (info->hdr_format != picture->hdr_format)
		return "a file whose HDR class is not its picture's";
	if (picture->hdr_format != AXIS_HDR_SDR && (!def->keep_hdr || def->codec != AXIS_TCODEC_HEVC))
		return "HDR made SDR";
	if (picture->hdr_format == AXIS_HDR_HDR10 && !picture->hdr10.mastering)
		return "HDR10 without its values";
	if (!axis_hdr10_metadata_valid(&picture->hdr10))
		return "HDR10 values out of range";
	if (info->interlaced == AXIS_TRI_YES || axis_gen_field_based(picture->field_order))
		return "interlaced";
	if (info->interlaced != AXIS_TRI_NO && g_strcmp0(picture->field_order, "progressive") != 0)
		return "not known to be progressive";
	if (!in_list(picture->pixel_format, four_two_zero) || picture->bit_depth < 8 || picture->bit_depth > 12)
		return "not 4:2:0 of 8 to 12 bits";
	if (caps == NULL || caps->tool_digest == NULL)
		return "no capabilities measured";
	return NULL;
}

/* ── The plan ───────────────────────────────────────────────────── */

/* The value after the only @option in @args; %NULL when it is not there,
 * and broken when it is there twice. */
static const gchar *
value_of(
	const gchar *const *args,
	const gchar        *option
){
	const gchar *value = NULL;
	guint i;

	for (i = 0; args[i] != NULL; i++) {
		if (strcmp(args[i], option) != 0)
			continue;
		if (value != NULL || args[i + 1] == NULL)
			broken("an option of one output stream given twice, or without its value");
		value = args[i + 1];
	}
	return value;
}

static const AxisMediaStream *
source_of(
	const AxisMediaInfo *info,
	gint                 index
){
	guint i;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->index == index)
			return s;
	}
	return NULL;
}

static void
colour_kept(
	const gchar *source,
	const gchar *planned
){
	if (unstated(source) ? planned != NULL : g_strcmp0(source, planned) != 0)
		broken("a colour property not as the source states it");
}

static void
append_colour(
	GString     *out,
	const gchar *key,
	const gchar *value
){
	if (value == NULL)
		return;
	g_string_append_printf(out, "%s%s=%s", out->len > 0 ? ":" : "", key, value);
}

static void
check_plan(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps,
	const AxisConvertPlan         *plan
){
	static const gchar *const mp4_audio[] = { "aac", "ac3", "eac3", "mp3", "alac", "opus", "flac", NULL };
	static const gchar *const mp4_text[] = { "subrip", "srt", "mov_text", "webvtt", "text", NULL };
	static const gchar *const mp4_covers[] = { "mjpeg", "png", NULL };
	static const gchar *const text_subtitles[] = { "subrip", "srt", "webvtt", "mov_text", "text", NULL };
	const gchar *const *args = (const gchar *const *)plan->args;
	const AxisConvertEncoder *encoder = axis_convert_encoder_for(def->codec, family);
	gboolean mp4 = def->container == AXIS_CONTAINER_MP4;
	gboolean hdr = FALSE;
	const AxisEncoderCapability *cap;
	const AxisMediaStream *picture;
	const AxisMediaStream *stereo_source = NULL;
	const AxisMediaStream *first_kept = NULL;
	const AxisConvertStream *video;
	gchar *problem = NULL;
	guint pictures = 0;
	guint maps = 0;
	guint encodes = 0;
	guint stereos = 0;
	guint muxers = 0;
	guint audio_seen = 0;
	gint last_rank = 0;
	gboolean listed = FALSE;
	gboolean any_stereo_aac = FALSE;
	guint i;
	guint j;

	/* The vector. */
	if (args == NULL)
		broken("a plan without a vector");
	if (!axis_convert_args_valid(args, info, &problem)) {
		g_printerr("%s\n", problem != NULL ? problem : "?");
		broken("a plan whose vector fails the grammar");
	}
	for (i = 0; args[i] != NULL; i++) {
		const gchar *p;

		if (strstr(args[i], MARK) != NULL)
			broken("text of the file in the vector");
		if (args[i][0] == '\0')
			broken("an empty argument");
		for (p = args[i]; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && strchr(":=+-_.,/()", *p) == NULL)
				broken("a character outside the alphabet");
		}
		if (strcmp(args[i], "-map") == 0) {
			const AxisConvertStream *s;
			g_autofree gchar *want = NULL;

			if (maps >= plan->streams->len)
				broken("more maps than output streams");
			s = g_ptr_array_index(plan->streams, maps);
			want = g_strdup_printf("0:%d", s->source);
			if (s->action == AXIS_CONVERT_MUXER || g_strcmp0(args[i + 1], want) != 0)
				broken("a map that is not the output stream of its position");
			maps++;
		}
	}

	/* The encoder and what the machine was shown to do. */
	for (i = 0; i < def->n_encoders; i++) {
		if (def->encoders[i] == family)
			listed = TRUE;
	}
	if (!listed || encoder == NULL || plan->encoder != encoder)
		broken("not the profile's encoder of its codec and this family");
	cap = axis_encoder_capabilities_get(caps, encoder);
	if (!cap->measured || !cap->usable)
		broken("an encoder not shown usable");
	if (plan->bit_depth >= 10 && (!encoder->ten_bit || !cap->ten_bit))
		broken("10 bits from an encoder not shown to make them");
	if (plan->hdr_format == AXIS_HDR_HDR10 && !cap->hdr10)
		broken("HDR10 from an encoder not shown to carry it");
	if (g_strcmp0(plan->tool_digest, caps->tool_digest) != 0)
		broken("a plan for another ffmpeg build than the one measured");
	if (plan->container != def->container)
		broken("not the profile's container");

	/* The picture. */
	picture = find_picture(info, &pictures);
	hdr = picture->hdr_format == AXIS_HDR_HDR10 || picture->hdr_format == AXIS_HDR_HLG;
	if (plan->hdr_format != picture->hdr_format)
		broken("the output's HDR class is not the source's");
	if (plan->bit_depth != (hdr ? 10 : def->bit_depth))
		broken("the output's depth");
	if (plan->quality < 1 || plan->quality > 51 ||
	    plan->quality != axis_estimate_quality_value(def->codec, family, plan->level))
		broken("the encoder's number is not its table's for the level");
	if (plan->chapters != info->chapter_count)
		broken("the chapters");
	if (plan->streams->len == 0)
		broken("a plan without streams");
	video = g_ptr_array_index(plan->streams, 0);
	if (video->action != AXIS_CONVERT_ENCODE || video->source != picture->index)
		broken("the first output stream is not the picture, encoded");
	colour_kept(picture->color_primaries, video->color_primaries);
	colour_kept(picture->color_transfer, video->color_transfer);
	colour_kept(picture->color_space, video->color_space);
	colour_kept(picture->color_range, video->color_range);
	{
		/* The frames are told the source's colour (setparams), before the
		 * upload for VA-API. */
		GString *want = g_string_new(NULL);
		const gchar *filter = value_of(args, "-filter:0");
		gboolean vaapi = encoder->family == AXIS_ENCODER_VAAPI;

		append_colour(want, "color_primaries", video->color_primaries);
		append_colour(want, "color_trc", video->color_transfer);
		append_colour(want, "colorspace", video->color_space);
		append_colour(want, "range", video->color_range);
		if (want->len > 0)
			g_string_prepend(want, "setparams=");
		if (vaapi)
			g_string_append_printf(want, "%sformat=%s,hwupload", want->len > 0 ? "," : "",
				plan->bit_depth >= 10 ? "p010le" : "nv12");
		if (want->len == 0 ? filter != NULL : g_strcmp0(filter, want->str) != 0)
			broken("the picture's filter is not the source's colour");
		g_string_free(want, TRUE);
	}
	if (hdr) {
		if (!axis_hdr10_metadata_equal(&video->hdr10, &picture->hdr10))
			broken("HDR10's values are not the source's");
		if (picture->hdr_format == AXIS_HDR_HDR10 && !video->hdr10.mastering)
			broken("HDR10 without its mastering display values");
	} else if (video->hdr10.mastering || video->hdr10.light) {
		broken("HDR10 values for an SDR picture");
	}
	if (encoder->hdr10_stated && video->hdr10.mastering) {
		const gchar *params = value_of(args, "-x265-params:0");
		const AxisHdr10Metadata *m = &video->hdr10;
		g_autofree gchar *display = g_strdup_printf("master-display=G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%"
			G_GINT64_FORMAT ",%" G_GINT64_FORMAT ")", m->green_x, m->green_y, m->blue_x, m->blue_y, m->red_x,
			m->red_y, m->white_x, m->white_y, m->max_luminance, m->min_luminance);
		g_autofree gchar *light = m->light ? g_strdup_printf("max-cll=%d,%d", m->max_content, m->max_average) :
			g_strdup("cll=0");

		if (params == NULL || strstr(params, display) == NULL || strstr(params, light) == NULL)
			broken("the encoder is not given HDR10's values exactly");
	}

	/* The output's streams. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_AUDIO || dropped_by_language(def, s))
			continue;
		if (first_kept == NULL)
			first_kept = s;
		if (stereo_source == NULL && s->is_default)
			stereo_source = s;
		if (stereo_aac(s))
			any_stereo_aac = TRUE;
	}
	if (stereo_source == NULL)
		stereo_source = first_kept;
	for (i = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);
		const AxisMediaStream *source;
		g_autofree gchar *codec_option = g_strdup_printf("-c:%u", i);
		g_autofree gchar *disposition_option = g_strdup_printf("-disposition:%u", i);
		const gchar *codec;
		const gchar *disposition;
		const gchar *want = NULL;
		gint rank;

		if (s->position != i)
			broken("a stream not at its position");
		if (s->action == AXIS_CONVERT_MUXER) {
			if (!mp4 || info->chapter_count <= 0 || i + 1 != plan->streams->len || s->source != -1 ||
			    g_strcmp0(s->codec, "bin_data") != 0)
				broken("a stream of the muxer's where there is none, or not last");
			muxers++;
			continue;
		}
		source = source_of(info, s->source);
		if (source == NULL || source->kind != s->kind)
			broken("an output stream of no stream of the file, or of another kind");
		switch (s->action) {
		case AXIS_CONVERT_ENCODE:
			encodes++;
			if (i != 0 || source != picture || g_strcmp0(s->codec, axis_target_codec_to_string(def->codec)) != 0)
				broken("an encoded stream that is not the picture");
			want = encoder->name;
			break;
		case AXIS_CONVERT_STEREO:
			stereos++;
			if (s->kind != AXIS_STREAM_AUDIO || g_strcmp0(s->codec, "aac") != 0 || s->channels != 2 ||
			    s->title != NULL || def->audio == AXIS_AUDIO_COPY || source != stereo_source)
				broken("a stereo track that is not one");
			want = "aac";
			break;
		case AXIS_CONVERT_TO_TEXT:
			if (s->kind != AXIS_STREAM_SUBTITLE || g_strcmp0(s->codec, mp4 ? "mov_text" : "subrip") != 0 ||
			    (!mp4 && g_strcmp0(source->codec, "mov_text") != 0 && g_strcmp0(source->codec, "text") != 0))
				broken("a subtitle converted that need not be");
			want = mp4 ? "mov_text" : "srt";
			break;
		case AXIS_CONVERT_COPY:
			if (g_strcmp0(s->codec, source->codec) != 0 || (s->kind == AXIS_STREAM_VIDEO && !source->attached_pic))
				broken("a copy that is not the source's codec, or the picture copied");
			want = "copy";
			break;
		default:
			broken("an action that does not exist");
		}
		rank = s->kind == AXIS_STREAM_VIDEO && !s->attached_pic ? 0 : s->kind == AXIS_STREAM_AUDIO ? 1 :
			s->kind == AXIS_STREAM_SUBTITLE ? 2 : 3;
		if (rank < last_rank)
			broken("the output's streams out of their order");
		last_rank = rank;

		/* What the container holds and the profile keeps. */
		if (mp4 && s->kind == AXIS_STREAM_AUDIO && s->action == AXIS_CONVERT_COPY && !in_list(s->codec, mp4_audio))
			broken("audio MP4 cannot hold");
		if (mp4 && s->kind == AXIS_STREAM_SUBTITLE &&
		    (!in_list(source->codec, mp4_text) || def->subtitles != AXIS_SUBTITLES_PLAIN_TEXT))
			broken("a subtitle MP4 cannot hold as text");
		if (mp4 && s->attached_pic && !in_list(s->codec, mp4_covers))
			broken("a cover MP4 cannot hold");
		if (!mp4 && s->kind == AXIS_STREAM_SUBTITLE && def->subtitles == AXIS_SUBTITLES_PLAIN_TEXT &&
		    !in_list(source->codec, text_subtitles))
			broken("a picture subtitle kept by a plain-text profile");
		if (s->kind == AXIS_STREAM_AUDIO && dropped_by_language(def, source))
			broken("audio the language rule drops, carried");

		/* Its codec and its flags, once each. */
		codec = value_of(args, codec_option);
		disposition = value_of(args, disposition_option);
		if (g_strcmp0(codec, want) != 0)
			broken("an output stream's codec argument");
		if (disposition == NULL)
			broken("an output stream without its flags");
		if (!mp4) {
			g_autofree gchar *flags = g_strdup_printf("%cdefault%cforced", s->is_default ? '+' : '-',
				s->is_forced ? '+' : '-');

			if (strcmp(disposition, flags) != 0)
				broken("the flags given are not the plan's");
		}
		if (s->kind == AXIS_STREAM_AUDIO) {
			/* Stereo first: the first audio track is stereo and the
			 * default, the others not. */
			if (def->audio == AXIS_AUDIO_STEREO_FIRST) {
				if (audio_seen == 0 && s->action != AXIS_CONVERT_STEREO &&
				    !(s->action == AXIS_CONVERT_COPY && source == stereo_source && stereo_aac(source)))
					broken("stereo first, and the first audio track not stereo");
				if ((s->is_default != FALSE) != (audio_seen == 0))
					broken("stereo first, and not the first audio track the default");
			}
			audio_seen++;
		}
	}
	if (encodes != 1 || stereos > 1)
		broken("not one encoded picture, or two stereo tracks");
	if (muxers != (mp4 && info->chapter_count > 0 ? 1u : 0u))
		broken("MP4's chapter track not where it is made");
	if (maps + muxers != plan->streams->len)
		broken("not every output stream mapped");
	if (def->audio == AXIS_AUDIO_COPY_ADD_STEREO && stereos != (first_kept != NULL && !any_stereo_aac ? 1u : 0u))
		broken("a stereo track added where there is one, or not where there is none");
	if (def->audio == AXIS_AUDIO_STEREO_FIRST &&
	    stereos != (stereo_source != NULL && !stereo_aac(stereo_source) ? 1u : 0u))
		broken("stereo first, and a stereo track made or not made");

	/* Every stream of the file mapped or dropped, never both, once. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		guint out = 0;
		guint stereo = 0;
		guint dropped = 0;

		for (j = 0; j < plan->streams->len; j++) {
			const AxisConvertStream *e = g_ptr_array_index(plan->streams, j);

			if (e->source == s->index) {
				out++;
				if (e->action == AXIS_CONVERT_STEREO)
					stereo++;
			}
		}
		for (j = 0; j < plan->dropped->len; j++) {
			const AxisConvertDropped *d = g_ptr_array_index(plan->dropped, j);

			if (d->source == s->index) {
				dropped++;
				if (d->reason == NULL || d->kind != s->kind)
					broken("a stream dropped without its reason");
			}
		}
		if (dropped > 1 || (dropped == 1 && out > 0))
			broken("a stream both dropped and carried, or dropped twice");
		if (out + dropped == 0)
			broken("a stream neither carried nor dropped");
		if (out > 2 || (out == 2 && stereo != 1))
			broken("a stream mapped twice");
		if ((dropped == 1) != dropped_by_language(def, s))
			broken("a stream dropped that the language rule keeps");
	}
	for (j = 0; j < plan->dropped->len; j++) {
		if (source_of(info, ((const AxisConvertDropped *)g_ptr_array_index(plan->dropped, j))->source) == NULL)
			broken("a dropped stream the file does not have");
	}
	for (j = 0; j < plan->notes->len; j++) {
		if (g_ptr_array_index(plan->notes, j) == NULL)
			broken("a note without its sentence");
	}
}

/* ── The output check ───────────────────────────────────────────── */

/* The probe of an output exactly as the plan says. */
static AxisMediaInfo *
output_of(const AxisConvertPlan *plan)
{
	AxisMediaInfo *out = axis_media_info_new();
	guint i;

	out->status = g_strdup("ok");
	out->container = g_strdup(plan->container == AXIS_CONTAINER_MP4 ? "mov,mp4,m4a,3gp,3g2,mj2" : "matroska,webm");
	out->chapter_count = plan->chapters >= 0 ? plan->chapters : 0;
	for (i = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *e = g_ptr_array_index(plan->streams, i);
		AxisMediaStream *o = stream_new(e->kind, (gint)i);

		o->codec = g_strdup(e->codec);
		o->is_default = e->is_default;
		o->is_forced = e->is_forced;
		o->attached_pic = e->attached_pic;
		o->channels = e->channels > 0 ? e->channels : AXIS_PROBE_UNKNOWN;
		o->language = g_strdup(e->language);
		o->title = g_strdup(e->title);
		if (e->action == AXIS_CONVERT_ENCODE) {
			o->width = e->width;
			o->height = e->height;
			o->bit_depth = e->bit_depth;
			o->color_primaries = g_strdup(e->color_primaries);
			o->color_transfer = g_strdup(e->color_transfer);
			o->color_space = g_strdup(e->color_space);
			o->color_range = g_strdup(e->color_range);
			o->hdr_format = e->hdr_format;
			o->hdr10 = e->hdr10;
			o->signals.has_mastering = o->signals.mastering_in_frames = e->hdr10.mastering;
			o->signals.has_content_light = o->signals.light_in_frames = e->hdr10.light;
		}
		g_ptr_array_add(out->streams, o);
	}
	out->stream_count = (gint)out->streams->len;
	return out;
}

static void
replace(
	gchar       **slot,
	const gchar  *value
){
	g_free(*slot);
	*slot = g_strdup(value);
}

/* Another name of @property than @not. */
static const gchar *
other_colour(
	const gchar *property,
	const gchar *not
){
	const gchar *const *names = axis_convert_colour_names(property);

	return g_strcmp0(names[0], not) != 0 ? names[0] : names[1];
}

/*
 * Says the planned picture of @out in other words that mean the same: a
 * range the plan leaves unstated or states limited as "tv", as nothing or
 * as "unknown"; another unstated property as "unknown" or "unspecified";
 * and an SDR picture of more than 8 bits that states no transfer function
 * "not classified", uncertain, as the probe has it (its frames read).
 */
static void
same_meaning(
	AxisGenInput          *in,
	const AxisConvertPlan *plan,
	AxisMediaInfo         *out
){
	const AxisConvertStream *e0 = g_ptr_array_index(plan->streams, 0);
	AxisMediaStream *o0 = g_ptr_array_index(out->streams, 0);

	if (e0->action != AXIS_CONVERT_ENCODE)
		return;
	if (e0->color_range == NULL || strcmp(e0->color_range, "tv") == 0) {
		switch (axis_gen_pick(in, 4)) {
		case 0:
			replace(&o0->color_range, "tv");
			break;
		case 1:
			replace(&o0->color_range, NULL);
			break;
		case 2:
			replace(&o0->color_range, "unknown");
			break;
		default:
			break;
		}
	}
	if (e0->color_primaries == NULL && axis_gen_pick(in, 2))
		replace(&o0->color_primaries, "unknown");
	if (e0->color_transfer == NULL && axis_gen_pick(in, 2))
		replace(&o0->color_transfer, "unspecified");
	if (e0->color_space == NULL && axis_gen_pick(in, 2))
		replace(&o0->color_space, "unknown");
	if (e0->hdr_format == AXIS_HDR_SDR && e0->color_transfer == NULL && e0->bit_depth > 8 &&
	    g_strcmp0(e0->color_primaries, "bt2020") != 0 && axis_gen_pick(in, 2)) {
		o0->hdr_format = AXIS_HDR_NONE;
		o0->hdr_uncertain = TRUE;
		out->main_video = 0;
		out->frames_unavailable = FALSE;
	}
}

/* Changes one property the check compares; FALSE when this one does not
 * apply to the plan.  *@picture: whether it is the picture's own (its
 * size, depth, colour, HDR class or HDR10 values). */
static gboolean
mutate_output(
	AxisGenInput          *in,
	const AxisConvertPlan *plan,
	AxisMediaInfo         *out,
	gboolean              *picture
){
	guint choice = axis_gen_pick(in, 18);
	guint at = axis_gen_pick(in, out->streams->len);
	const AxisConvertStream *e = g_ptr_array_index(plan->streams, at);
	AxisMediaStream *o = g_ptr_array_index(out->streams, at);
	const AxisConvertStream *e0 = g_ptr_array_index(plan->streams, 0);
	AxisMediaStream *o0 = g_ptr_array_index(out->streams, 0);
	gboolean muxer = e->action == AXIS_CONVERT_MUXER;

	*picture = choice >= 12;
	switch (choice) {
	case 0:
		replace(&out->container, "ogg");
		return TRUE;
	case 1:
		g_ptr_array_remove_index(out->streams, out->streams->len - 1);
		return TRUE;
	case 2: {
		AxisMediaStream *extra = stream_new(AXIS_STREAM_SUBTITLE, (gint)out->streams->len);

		extra->codec = g_strdup("subrip");
		g_ptr_array_add(out->streams, extra);
		return TRUE;
	}
	case 3:
		if (plan->chapters < 0)
			return FALSE;
		out->chapter_count++;
		return TRUE;
	case 4:
		o->kind = o->kind == AXIS_STREAM_AUDIO ? AXIS_STREAM_SUBTITLE : AXIS_STREAM_AUDIO;
		return TRUE;
	case 5:
		replace(&o->codec, "zz");
		return TRUE;
	case 6:
		if (muxer)
			return FALSE;
		o->is_default = !o->is_default;
		return TRUE;
	case 7:
		if (muxer)
			return FALSE;
		o->is_forced = !o->is_forced;
		return TRUE;
	case 8:
		if (muxer)
			return FALSE;
		o->attached_pic = !o->attached_pic;
		return TRUE;
	case 9:
		if (muxer || e->channels <= 0)
			return FALSE;
		o->channels = e->channels + 1;
		return TRUE;
	case 10:
		if (muxer || e->kind == AXIS_STREAM_ATTACHMENT)
			return FALSE;
		replace(&o->language, "qqq");
		return TRUE;
	case 11:
		if (muxer || !e->check_title)
			return FALSE;
		replace(&o->title, e->title == NULL ? "qqq" : NULL);
		return TRUE;
	case 12:
		o0->width++;
		return TRUE;
	case 13:
		o0->bit_depth = o0->bit_depth == 8 ? 10 : 8;
		return TRUE;
	case 14:
		switch (axis_gen_pick(in, 4)) {
		case 0:
			replace(&o0->color_range, e0->color_range == NULL || strcmp(e0->color_range, "tv") == 0 ? "pc" : "tv");
			break;
		case 1:
			replace(&o0->color_space, other_colour("matrix", e0->color_space));
			break;
		case 2:
			replace(&o0->color_transfer, other_colour("transfer", e0->color_transfer));
			break;
		default:
			replace(&o0->color_primaries, other_colour("primaries", e0->color_primaries));
			break;
		}
		return TRUE;
	case 15:
		if (e0->hdr_format == AXIS_HDR_HDR10 || e0->hdr_format == AXIS_HDR_HLG)
			o0->hdr_uncertain = TRUE;
		else
			o0->hdr_format = AXIS_HDR_HLG;
		return TRUE;
	/* Values only in the container are the bitstream check's to find
	 * (check 4 runs it apart), not the picture check's. */
	case 16:
		if (!e0->hdr10.mastering) {
			o0->signals.has_mastering = TRUE;
		} else if (axis_gen_pick(in, 2)) {
			o0->hdr10.red_x ^= 1;
		} else {
			o0->signals.mastering_in_frames = FALSE;
			*picture = FALSE;
		}
		return TRUE;
	default:
		if (!e0->hdr10.light) {
			o0->signals.has_content_light = TRUE;
		} else if (axis_gen_pick(in, 2)) {
			o0->hdr10.max_content ^= 1;
		} else {
			o0->signals.light_in_frames = FALSE;
			*picture = FALSE;
		}
		return TRUE;
	}
}

static void
check_output(
	AxisGenInput          *in,
	const AxisConvertPlan *plan
){
	g_autoptr(AxisMediaInfo) out = output_of(plan);
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *why = NULL;
	gboolean picture = FALSE;
	gboolean picture_ok;
	guint i;

	/* Exactly as the plan says, in any words that mean the same. */
	same_meaning(in, plan, out);
	if (!axis_convert_plan_check_output(plan, out, mismatches) || mismatches->len != 0) {
		g_printerr("%s\n", mismatches->len > 0 ? (const gchar *)g_ptr_array_index(mismatches, 0) : "?");
		broken("an output exactly as the plan says fails the check");
	}
	if (!axis_convert_plan_check_picture(plan, out, &why) || why != NULL) {
		g_printerr("%s\n", why != NULL ? why : "?");
		broken("an output exactly as the plan says fails the picture check");
	}
	if (!mutate_output(in, plan, out, &picture))
		return;
	if (axis_convert_plan_check_output(plan, out, mismatches) || mismatches->len == 0)
		broken("an output that differs from its plan passes the check");
	for (i = 0; i < mismatches->len; i++) {
		if (!g_utf8_validate(g_ptr_array_index(mismatches, i), -1, NULL))
			broken("a difference that is not text");
	}
	/* The picture check (check 4) finds every difference of the picture's
	 * own and says what it is. */
	picture_ok = axis_convert_plan_check_picture(plan, out, &why);
	if (picture && picture_ok)
		broken("a picture that differs from its plan passes the picture check");
	if (!picture_ok && (why == NULL || why[0] == '\0' || !g_utf8_validate(why, -1, NULL)))
		broken("a picture check failed without saying why, in text");
	if (picture_ok && why != NULL)
		broken("a picture check passed and said something differs");
}

/* ── Vectors changed ────────────────────────────────────────────── */

static const gchar *const tokens[] = {
	"-i", "fd:", "pipe:0", "pipe:1", "file:/etc/passwd", "/etc/passwd", "-y", "-f", "null", "-", "-map", "0:0", "0:1",
	"0:99", "0:-1", "0:100001", "-c:0", "-c:1", "copy", "-filter_complex", "movie=x", "-fd", "3", "4", "5",
	"-protocol_whitelist", "file,fd", "fd", MARK, "-metadata:s:0", "-metadata:s:1", "title=" MARK, "title=", "BPS=",
	"-disposition:0", "+default+forced", "-x265-params:0", "log-level=error", "log-level=error:" MARK "=1",
	"master-display=G(1,1)B(1,1)R(1,1)WP(1,1)L(100000,1)", "-dump_attachment:t:0", "out.mkv", "-default_mode",
	"passthrough", "matroska", "mp4", "mpegts", "-vaapi_device", "/dev/dri/renderD128", "/dev/dri/../sda", "-progress",
	"-loglevel", "error", "-nostdin", "", "a b", "-attach", "-map_metadata", "-map_chapters", "0", "-b:1", "96000",
	"-ac:1", "2", "-threads:0", "64", "setparams=range=pc", "format=nv12,hwupload", "-filter:0", "-pix_fmt:0",
	"yuv420p", "-format_whitelist", "concat", "-c", "-c:v", "libx265", "-preset:0", "medium"
};

static gboolean
render_device(const gchar *text)
{
	return strlen(text) == 19 && g_str_has_prefix(text, "/dev/dri/renderD") && g_ascii_isdigit(text[16]) &&
		g_ascii_isdigit(text[17]) && g_ascii_isdigit(text[18]);
}

static gboolean
digits(const gchar *text)
{
	const gchar *p;

	if (text[0] == '\0')
		return FALSE;
	for (p = text; *p != '\0'; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
	}
	return TRUE;
}

static G_GNUC_NORETURN void
unsafe(const gchar *what)
{
	g_printerr("conversion plan property: the grammar accepts a vector with %s\n", what);
	abort();
}

/* What makes a vector safe to run, read as ffmpeg reads it: every
 * argument an option and its value but the last, the one output. */
static void
check_safe(
	const gchar *const  *args,
	const AxisMediaInfo *info
){
	static const gchar *const flags[] = { "-nostdin", "-hide_banner", "-nostats", NULL };
	static const gchar *const valued[] = {
		"-loglevel", "-progress", "-vaapi_device", "-protocol_whitelist", "-format_whitelist", "-fd", "-i", "-map",
		"-map_chapters", "-map_metadata", "-default_mode", "-f", NULL
	};
	static const gchar *const per_stream[] = {
		"c", "filter", "pix_fmt", "preset", "crf", "cq", "qp", "rc", "rc_mode", "b", "ac", "x265-params",
		"x264-params", "a53cc", "threads", "metadata:s", "disposition", NULL
	};
	const gchar *fd = NULL;
	gboolean fd_only = FALSE;
	guint inputs = 0;
	guint n;
	guint i;

	for (n = 0; args[n] != NULL; n++) {
		if (strstr(args[n], MARK) != NULL)
			unsafe("text of the file in it");
		if (strchr(args[n], '/') != NULL && !(n > 0 && strcmp(args[n - 1], "-vaapi_device") == 0 &&
		    render_device(args[n])))
			unsafe("a path in it");
	}
	if (n < 3 || strcmp(args[n - 1], "fd:") != 0 || strcmp(args[n - 2], "4") != 0 || strcmp(args[n - 3], "-fd") != 0)
		unsafe("an output that is not descriptor 4, last");
	i = 0;
	while (i + 1 < n) {
		const gchar *a = args[i];
		const gchar *v = args[i + 1];
		const gchar *colon;

		if (in_list(a, flags)) {
			i++;
			continue;
		}
		if (in_list(a, valued)) {
			if (strcmp(a, "-i") == 0) {
				inputs++;
				if (strcmp(v, "fd:") != 0 || g_strcmp0(fd, "3") != 0 || !fd_only)
					unsafe("an input that is not descriptor 3 with only fd allowed");
				fd = NULL;
				fd_only = FALSE;
			} else if (strcmp(a, "-fd") == 0) {
				if (strcmp(v, "3") != 0 && strcmp(v, "4") != 0)
					unsafe("a descriptor other than 3 and 4");
				fd = v;
			} else if (strcmp(a, "-protocol_whitelist") == 0) {
				if (strcmp(v, "fd") != 0)
					unsafe("a protocol other than fd allowed");
				fd_only = TRUE;
			} else if (strcmp(a, "-format_whitelist") == 0) {
				if (strcmp(v, axis_probe_format_whitelist()) != 0)
					unsafe("other input formats allowed");
			} else if (strcmp(a, "-map") == 0) {
				if (!g_str_has_prefix(v, "0:") || !digits(v + 2) || strlen(v + 2) > 6 ||
				    source_of(info, (gint)g_ascii_strtoll(v + 2, NULL, 10)) == NULL)
					unsafe("a map of no stream of the file");
			} else if (strcmp(a, "-progress") == 0) {
				if (strcmp(v, "pipe:1") != 0)
					unsafe("the progress written elsewhere than standard output");
			} else if (strcmp(a, "-f") == 0) {
				if (strcmp(v, "matroska") != 0 && strcmp(v, "mp4") != 0)
					unsafe("another output format");
			}
			i += 2;
			continue;
		}
		if (a[0] == '-' && (colon = strrchr(a, ':')) != NULL && colon > a + 1) {
			g_autofree gchar *name = g_strndup(a + 1, (gsize)(colon - a - 1));

			if (in_list(name, per_stream) && digits(colon + 1)) {
				i += 2;
				continue;
			}
		}
		unsafe("an argument that is no option the builder uses (ffmpeg would take it for an output)");
	}
	if (i != n - 1)
		unsafe("its output taken as an option's value");
	if (inputs != 1)
		unsafe("not exactly one input");
	if (g_strcmp0(fd, "4") != 0 || !fd_only)
		unsafe("an output that is not descriptor 4 with only fd allowed");
}

static void
check_changed(
	AxisGenInput          *in,
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *info
){
	GPtrArray *v = g_ptr_array_new_with_free_func(g_free);
	guint rounds = 1 + axis_gen_pick(in, 3);
	guint r;
	guint i;

	for (i = 0; plan->args[i] != NULL; i++)
		g_ptr_array_add(v, g_strdup(plan->args[i]));
	for (r = 0; r < rounds; r++) {
		guint op = axis_gen_pick(in, 5);
		guint at = v->len > 0 ? ((guint)axis_gen_byte(in) << 8 | axis_gen_byte(in)) % v->len : 0;
		const gchar *token = tokens[axis_gen_pick(in, G_N_ELEMENTS(tokens))];

		switch (op) {
		case 0:
			if (v->len > 0) {
				g_free(g_ptr_array_index(v, at));
				g_ptr_array_index(v, at) = g_strdup(token);
			}
			break;
		case 1:
			if (v->len > 0)
				g_ptr_array_remove_index(v, at);
			break;
		case 2:
			g_ptr_array_insert(v, (gint)at, g_strdup(token));
			break;
		case 3:
			if (at + 1 < v->len) {
				gpointer t = g_ptr_array_index(v, at);

				g_ptr_array_index(v, at) = g_ptr_array_index(v, at + 1);
				g_ptr_array_index(v, at + 1) = t;
			}
			break;
		default:
			if (at + 1 < v->len) {
				gchar *a = g_strdup(g_ptr_array_index(v, at));
				gchar *b = g_strdup(g_ptr_array_index(v, at + 1));

				g_ptr_array_insert(v, (gint)(at + 2), b);
				g_ptr_array_insert(v, (gint)(at + 2), a);
			}
			break;
		}
	}
	g_ptr_array_add(v, NULL);
	if (axis_convert_args_valid((const gchar *const *)v->pdata, info, NULL))
		check_safe((const gchar *const *)v->pdata, info);
	g_ptr_array_free(v, TRUE);
}

/* ── The target ─────────────────────────────────────────────────── */

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const AxisEncoderFamily families[] = {
		AXIS_ENCODER_SOFTWARE, AXIS_ENCODER_NVENC, AXIS_ENCODER_VAAPI, (AxisEncoderFamily)7
	};
	AxisGenInput in;
	AxisProfileDefinition def;
	AxisConvertOptions options;
	AxisConvertPlan *plan = NULL;
	AxisConvertPlan *chosen = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisConvertRefusal refusals[AXIS_ENCODER_COUNT];
	AxisConvertRefusal chosen_refusal = AXIS_CONVERT_OK;
	AxisMediaInfo *info;
	AxisEncoderCapabilities *caps;
	AxisEncoderFamily family;
	const gchar *reason;
	gboolean given = FALSE;
	gboolean options_ok;
	gboolean built;
	gint64 file_size;
	guint i;

	if (size > 4096)
		return 0;
	memset(&in, 0, sizeof(in));
	in.data = data;
	in.size = size;

	axis_gen_definition(&in, &def);
	info = gen_info(&in);
	caps = gen_caps(&in);
	options_ok = gen_options(&in, &options, &given);
	family = families[axis_gen_pick(&in, G_N_ELEMENTS(families))];
	file_size = axis_gen_sizes[axis_gen_pick(&in, G_N_ELEMENTS(axis_gen_sizes))];

	built = axis_convert_plan_build(&def, info, file_size, family, caps, given ? &options : NULL, &plan, &refusal);
	if (built != (plan != NULL) || (built != (refusal == AXIS_CONVERT_OK)))
		broken("a plan and its refusal disagree");
	if ((guint)refusal >= AXIS_CONVERT_N_REFUSALS || axis_convert_refusal_to_string(refusal) == NULL ||
	    axis_convert_refusal_describe(refusal) == NULL)
		broken("a refusal that does not exist");
	if (refusal == AXIS_CONVERT_REFUSE_INTERNAL)
		broken("the builder's own vector failed its grammar");
	if (def.kind != AXIS_PROFILE_KIND_VIDEO && refusal != AXIS_CONVERT_REFUSE_NOT_VIDEO)
		broken("an audio profile not refused as such");
	if (def.kind == AXIS_PROFILE_KIND_VIDEO && !options_ok && refusal != AXIS_CONVERT_REFUSE_OPTIONS)
		broken("settings outside the table not refused as such");
	reason = must_refuse(&def, info, caps, options_ok);
	if (plan != NULL) {
		if (reason != NULL)
			broken(reason);
		check_plan(&def, info, family, caps, plan);
		check_output(&in, plan);
		check_changed(&in, plan, info);
	}

	/* Choosing: the plan of the first family of the profile's that has
	 * one, the others' refusals as building gives them. */
	if (axis_convert_plan_choose(&def, info, file_size, caps, given ? &options : NULL, &chosen, &chosen_refusal,
	    refusals)) {
		AxisConvertPlan *again = NULL;
		guint at = def.n_encoders;

		if (chosen == NULL || chosen_refusal != AXIS_CONVERT_OK)
			broken("a choice without its plan");
		for (i = 0; i < def.n_encoders; i++) {
			if (chosen->encoder->family == def.encoders[i]) {
				at = i;
				break;
			}
		}
		if (at == def.n_encoders || refusals[at] != AXIS_CONVERT_OK)
			broken("a choice of a family the profile does not list");
		for (i = 0; i < at; i++) {
			AxisConvertRefusal one = AXIS_CONVERT_OK;

			if (refusals[i] == AXIS_CONVERT_OK ||
			    axis_convert_plan_build(&def, info, file_size, def.encoders[i], caps, given ? &options : NULL, NULL,
			    &one) || one != refusals[i])
				broken("a family passed over that has a plan");
		}
		if (!axis_convert_plan_build(&def, info, file_size, def.encoders[at], caps, given ? &options : NULL, &again,
		    NULL) || g_strv_length(again->args) != g_strv_length(chosen->args))
			broken("the chosen plan is not the family's");
		for (i = 0; again->args[i] != NULL; i++) {
			if (strcmp(again->args[i], chosen->args[i]) != 0)
				broken("the chosen plan is not the family's");
		}
		axis_convert_plan_free(again);
		if (reason != NULL)
			broken(reason);
		axis_convert_plan_free(chosen);
	} else {
		if (chosen != NULL || chosen_refusal == AXIS_CONVERT_OK || chosen_refusal == AXIS_CONVERT_REFUSE_INTERNAL)
			broken("no choice, yet a plan or no reason");
		for (i = 0; i < def.n_encoders && def.kind == AXIS_PROFILE_KIND_VIDEO; i++) {
			if (refusals[i] == AXIS_CONVERT_OK || refusals[i] == AXIS_CONVERT_REFUSE_INTERNAL)
				broken("a family without its refusal");
		}
		if (def.kind == AXIS_PROFILE_KIND_VIDEO && def.n_encoders > 0 && chosen_refusal != refusals[0])
			broken("no choice, and not the first family's refusal");
	}

	axis_convert_plan_free(plan);
	axis_encoder_capabilities_free(caps);
	axis_media_info_free(info);
	return 0;
}
