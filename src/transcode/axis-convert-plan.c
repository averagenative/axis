/*
 * axis-convert-plan.c - the plan of a video conversion: which streams, in
 * what order, made how, and the argument vector that makes them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The promises are in axis-convert.h.  How the vector is laid out:
 *
 *   -nostdin -hide_banner -loglevel error -nostats -progress pipe:1
 *   [-vaapi_device DEVICE]
 *   -protocol_whitelist fd -format_whitelist LIST -fd 3 -i fd:
 *   -map 0:N ...                        one per output stream, in order
 *   -map_chapters 0 -map_metadata 0
 *   -c:K ... -disposition:K ...         every option per output stream K
 *   [-default_mode passthrough] -f matroska|mp4
 *   -protocol_whitelist fd -fd 4 fd:
 *
 * Why each global option is there (and why others are not) is in design.md:
 * each one was measured on ffmpeg 7.1 and 8.1.  In short: `-nostdin`, never
 * the terminal; errors only; progress as key=value lines on standard output;
 * explicit dispositions on every stream, because without any ffmpeg marks
 * the first audio and subtitle track default by itself; Matroska's
 * `-default_mode passthrough` (its default today) so that a flag is written
 * as given; no `-y` (the output is a descriptor); no `-movflags +faststart`
 * (with a descriptor it writes a broken MP4 and exits 0, on both versions);
 * no `-max_muxing_queue_size` (a late first subtitle muxes without it on
 * both versions, and a stream that starts late enough to overflow the queue
 * fails the conversion loudly, which keeps the original).
 */

#include "transcode/axis-convert.h"

#include <string.h>

#define MAX_OUTPUT_STREAMS     AXIS_PROBE_MAX_STREAMS
/* The largest stream index the probe records (axis-probe-map.c) and the
 * grammar accepts in `-map 0:<index>`. */
#define MAX_STREAM_INDEX       100000
#define DEFAULT_SOFTWARE_PRESET "medium"
#define DEFAULT_NVENC_PRESET    "p5"
#define DEFAULT_VAAPI_DEVICE    "/dev/dri/renderD128"

/* ── Tables ─────────────────────────────────────────────────────── */

static const gchar *const software_presets[] = {
	"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", NULL
};
static const gchar *const nvenc_presets[] = { "p1", "p2", "p3", "p4", "p5", "p6", "p7", NULL };

/* The statistics tags mkvmerge writes on every track (with and without a
 * language suffix): copied onto a stream encoded anew they would describe
 * the old stream, and the probe reads BPS as the stream's bitrate.  Blanked
 * on every stream the plan encodes (measured: without this the HEVC stream
 * kept the H.264 source's BPS and NUMBER_OF_FRAMES).  DURATION is not here:
 * the Matroska muxer never copies it and writes its own. */
static const gchar *const statistics_tags[] = {
	"BPS", "BPS-eng", "NUMBER_OF_FRAMES", "NUMBER_OF_FRAMES-eng", "NUMBER_OF_BYTES", "NUMBER_OF_BYTES-eng",
	"_STATISTICS_WRITING_APP", "_STATISTICS_WRITING_APP-eng", "_STATISTICS_WRITING_DATE_UTC",
	"_STATISTICS_WRITING_DATE_UTC-eng", "_STATISTICS_TAGS", "_STATISTICS_TAGS-eng", NULL
};

/* What MP4 can hold as it is (the eligibility's lists). */
static const gchar *const mp4_audio[] = { "aac", "ac3", "eac3", "mp3", "alac", "opus", "flac", NULL };
static const gchar *const mp4_text[] = { "subrip", "srt", "mov_text", "webvtt", "text", NULL };
static const gchar *const mp4_covers[] = { "mjpeg", "png", NULL };
/* What Matroska holds as it is, and the text formats it needs converted. */
static const gchar *const mkv_subtitles[] = {
	"subrip", "srt", "ass", "ssa", "webvtt", "hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", NULL
};
static const gchar *const mkv_plain_text[] = { "subrip", "srt", "webvtt", NULL };
static const gchar *const to_subrip[] = { "mov_text", "text", NULL };

/* 4:2:0 pictures (the only chroma layout the targets' profiles have). */
static const gchar *const pixel_formats[] = {
	"yuv420p", "yuvj420p", "yuv420p10le", "yuv420p10be", "yuv420p12le", "yuv420p12be", "nv12", "nv21", "p010le",
	"p010be", NULL
};

#define NOTE_UNSTATED_PRIMARIES "The source does not state its colour primaries: the output leaves them unstated."
#define NOTE_UNSTATED_TRANSFER  "The source does not state its transfer function: the output leaves it unstated."
#define NOTE_UNSTATED_MATRIX    "The source does not state its colour matrix: the output leaves it unstated."
/* Measured on 7.1 and 8.1: ffmpeg hands the encoder a limited range for
 * frames that state none, and x265 and NVENC write it out. */
#define NOTE_UNSTATED_RANGE     "The source does not state its colour range, which means limited range: the output " \
	"is limited range too, whether or not the encoder writes that out."
#define NOTE_DEEP_UNSIGNALLED   "A picture of more than 8 bits with no transfer function stated is classed " \
	"\"HDR uncertain\" by the probe."
#define NOTE_HDR10_PASSED_ON    "This encoder takes no HDR10 values: they reach the output only if ffmpeg passes " \
	"the source's on, which the output check verifies."
#define NOTE_MP4_TITLES         "MP4 keeps no track titles: the tracks' titles are not carried over."
#define NOTE_MP4_DEFAULT        "MP4 marks the first track of a kind as default when none of that kind is."
#define NOTE_MP4_CHAPTERS       "In MP4 the chapters add a chapter track (a data stream) after the others."
#define NOTE_MP4_TAGS           "MP4 keeps only the file tags it has names for."
#define NOTE_STEREO_TITLE       "The added stereo track has no title (the original's would describe the original)."
#define NOTE_CAPTIONS           "Closed captions in the picture's bitstream are passed on by the encoder but are " \
	"not recorded by the probe, so they are not checked."

#define REASON_LANGUAGE "Audio in a known language the profile does not keep."

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

/* A stated colour value as the table spells it (a static string), %NULL
 * for "not stated"; @bad is set for a value the table has no name for. */
static const gchar *
colour_value(
	const gchar        *value,
	const gchar *const *names,
	gboolean           *bad
){
	guint i;

	if (value == NULL || value[0] == '\0' || strcmp(value, "unknown") == 0 || strcmp(value, "unspecified") == 0 ||
	    strcmp(value, "reserved") == 0)
		return NULL;
	for (i = 0; names[i] != NULL; i++) {
		if (strcmp(value, names[i]) == 0)
			return names[i];
	}
	*bad = TRUE;
	return NULL;
}

static gboolean
field_based(const gchar *order)
{
	return g_strcmp0(order, "tt") == 0 || g_strcmp0(order, "bb") == 0 || g_strcmp0(order, "tb") == 0 ||
		g_strcmp0(order, "bt") == 0;
}

static gboolean
stereo_aac(const AxisMediaStream *s)
{
	return g_strcmp0(s->codec, "aac") == 0 && s->channels >= 1 && s->channels <= 2;
}

/* The eligibility's language rule: only a track of a known language not on
 * the profile's list is dropped. */
static gboolean
language_dropped(
	const AxisProfileDefinition *def,
	const AxisMediaStream       *s
){
	guint i;

	if (def->n_languages == 0 || s->kind != AXIS_STREAM_AUDIO)
		return FALSE;
	if (s->language == NULL || s->language[0] == '\0' || strcmp(s->language, "und") == 0)
		return FALSE;
	for (i = 0; i < def->n_languages && i < AXIS_PROFILE_MAX_LANGUAGES; i++) {
		if (strcmp(s->language, def->languages[i]) == 0)
			return FALSE;
	}
	return TRUE;
}

/* ── Plan lifetime ──────────────────────────────────────────────── */

static void
stream_free(gpointer data)
{
	AxisConvertStream *s = data;

	if (s == NULL)
		return;
	g_free(s->codec);
	g_free(s->language);
	g_free(s->title);
	g_free(s);
}

void
axis_convert_plan_free(AxisConvertPlan *plan)
{
	if (plan == NULL)
		return;
	g_strfreev(plan->args);
	if (plan->streams != NULL)
		g_ptr_array_unref(plan->streams);
	if (plan->dropped != NULL)
		g_ptr_array_unref(plan->dropped);
	if (plan->notes != NULL)
		g_ptr_array_unref(plan->notes);
	g_free(plan->tool_digest);
	g_free(plan);
}

static AxisConvertPlan *
plan_new(void)
{
	AxisConvertPlan *plan = g_new0(AxisConvertPlan, 1);

	plan->streams = g_ptr_array_new_with_free_func(stream_free);
	plan->dropped = g_ptr_array_new_with_free_func(g_free);
	plan->notes = g_ptr_array_new();
	plan->chapters = -1;
	plan->duration_ms = -1;
	plan->frame_rate = -1.0;
	return plan;
}

static void
note(
	AxisConvertPlan *plan,
	const gchar     *text
){
	g_ptr_array_add(plan->notes, (gpointer)text);
}

/* An output stream made from @source (copies its language and title, which
 * ffmpeg copies too). */
static AxisConvertStream *
add_stream(
	AxisConvertPlan       *plan,
	const AxisMediaStream *source,
	AxisConvertAction      action,
	const gchar           *codec
){
	AxisConvertStream *s = g_new0(AxisConvertStream, 1);

	s->position = plan->streams->len;
	s->source = source != NULL ? source->index : -1;
	s->kind = source != NULL ? source->kind : AXIS_STREAM_DATA;
	s->action = action;
	s->codec = g_strdup(codec);
	s->channels = -1;
	s->width = AXIS_PROBE_UNKNOWN;
	s->height = AXIS_PROBE_UNKNOWN;
	s->bit_depth = AXIS_PROBE_UNKNOWN;
	if (source != NULL) {
		s->is_default = source->is_default;
		s->is_forced = source->is_forced;
		s->attached_pic = source->attached_pic;
		s->language = g_strdup(source->language);
		s->title = g_strdup(source->title);
	}
	s->check_title = TRUE;
	g_ptr_array_add(plan->streams, s);
	return s;
}

/* ── The arguments ──────────────────────────────────────────────── */

static void
arg(
	GPtrArray   *args,
	const gchar *text
){
	g_ptr_array_add(args, g_strdup(text));
}

/* "-OPTION:K" for output stream @k. */
static void
arg_stream(
	GPtrArray   *args,
	const gchar *option,
	guint        k,
	const gchar *value
){
	g_ptr_array_add(args, g_strdup_printf("%s:%u", option, k));
	g_ptr_array_add(args, g_strdup(value));
}

static void
arg_stream_number(
	GPtrArray   *args,
	const gchar *option,
	guint        k,
	gint64       value
){
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

	arg_stream(args, option, k, text);
}

static void
blank_statistics(
	GPtrArray *args,
	guint      k
){
	guint i;

	for (i = 0; statistics_tags[i] != NULL; i++) {
		g_ptr_array_add(args, g_strdup_printf("-metadata:s:%u", k));
		g_ptr_array_add(args, g_strdup_printf("%s=", statistics_tags[i]));
	}
}

/* The colour the plan states, in the three spellings it goes out in. */
typedef struct
{
	const gchar *primaries;
	const gchar *transfer;
	const gchar *matrix;
	const gchar *range;	/* "tv" or "pc" */
} Colour;

static gchar *
setparams_filter(const Colour *c)
{
	GString *out = g_string_new(NULL);

	if (c->primaries != NULL)
		g_string_append_printf(out, "%scolor_primaries=%s", out->len > 0 ? ":" : "", c->primaries);
	if (c->transfer != NULL)
		g_string_append_printf(out, "%scolor_trc=%s", out->len > 0 ? ":" : "", c->transfer);
	if (c->matrix != NULL)
		g_string_append_printf(out, "%scolorspace=%s", out->len > 0 ? ":" : "", c->matrix);
	if (c->range != NULL)
		g_string_append_printf(out, "%srange=%s", out->len > 0 ? ":" : "", c->range);
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	g_string_prepend(out, "setparams=");
	return g_string_free(out, FALSE);
}

/* x265's and x264's own colour parameters ("colorprim=...:transfer=..."). */
static void
encoder_colour(
	GString      *out,
	const Colour *c,
	gboolean      x265
){
	if (c->primaries != NULL)
		g_string_append_printf(out, "%scolorprim=%s", out->len > 0 ? ":" : "", c->primaries);
	if (c->transfer != NULL)
		g_string_append_printf(out, "%stransfer=%s", out->len > 0 ? ":" : "", c->transfer);
	if (c->matrix != NULL)
		g_string_append_printf(out, "%scolormatrix=%s", out->len > 0 ? ":" : "", c->matrix);
	if (c->range != NULL)
		g_string_append_printf(out, "%srange=%s", out->len > 0 ? ":" : "",
			x265 ? (strcmp(c->range, "pc") == 0 ? "full" : "limited") : c->range);
}

/* "+default-forced" and the like: relative, so that the stream's other
 * flags (which ffmpeg copies: comment, hearing_impaired, attached_pic ...)
 * are kept. */
static void
disposition(
	GPtrArray               *args,
	const AxisConvertStream *s
){
	g_autofree gchar *value = g_strdup_printf("%cdefault%cforced", s->is_default ? '+' : '-',
		s->is_forced ? '+' : '-');

	arg_stream(args, "-disposition", s->position, value);
}

/* ── Building ───────────────────────────────────────────────────── */

typedef struct
{
	const gchar *software_preset;
	const gchar *nvenc_preset;
	const gchar *vaapi_device;
	guint        threads;
} Options;

static gboolean
vaapi_device_valid(const gchar *device)
{
	static const gchar head[] = "/dev/dri/renderD";
	gsize i;

	if (device == NULL || strlen(device) != sizeof(head) - 1 + 3 || strncmp(device, head, sizeof(head) - 1) != 0)
		return FALSE;
	for (i = sizeof(head) - 1; device[i] != '\0'; i++) {
		if (!g_ascii_isdigit(device[i]))
			return FALSE;
	}
	return TRUE;
}

static gboolean
resolve_options(
	const AxisConvertOptions *given,
	Options                  *out
){
	out->software_preset = DEFAULT_SOFTWARE_PRESET;
	out->nvenc_preset = DEFAULT_NVENC_PRESET;
	out->vaapi_device = DEFAULT_VAAPI_DEVICE;
	out->threads = 0;
	if (given == NULL)
		return TRUE;
	if (given->software_preset != NULL) {
		guint i;

		out->software_preset = NULL;
		for (i = 0; software_presets[i] != NULL; i++) {
			if (strcmp(given->software_preset, software_presets[i]) == 0)
				out->software_preset = software_presets[i];
		}
		if (out->software_preset == NULL)
			return FALSE;
	}
	if (given->nvenc_preset != NULL) {
		guint i;

		out->nvenc_preset = NULL;
		for (i = 0; nvenc_presets[i] != NULL; i++) {
			if (strcmp(given->nvenc_preset, nvenc_presets[i]) == 0)
				out->nvenc_preset = nvenc_presets[i];
		}
		if (out->nvenc_preset == NULL)
			return FALSE;
	}
	if (given->vaapi_device != NULL) {
		if (!vaapi_device_valid(given->vaapi_device))
			return FALSE;
		/* Not a table string: kept as given, after the check above
		 * (letters of a fixed prefix and three digits). */
		out->vaapi_device = given->vaapi_device;
	}
	if (given->threads > 64)
		return FALSE;
	out->threads = given->threads;
	return TRUE;
}

/* The main picture: the largest video stream that is not a cover; and how
 * many such there are. */
static const AxisMediaStream *
main_picture(
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

/* Everything that would refuse whatever the encoder; fills @colour. */
static AxisConvertRefusal
check_file(
	const AxisProfileDefinition *def,
	const AxisMediaInfo         *info,
	const AxisMediaStream      **main_out,
	Colour                      *colour
){
	const AxisMediaStream *main;
	guint pictures = 0;
	gboolean bad = FALSE;
	gboolean has_audio = FALSE;
	gboolean kept_audio = FALSE;
	guint i;
	guint j;

	if (info == NULL || g_strcmp0(info->status, "ok") != 0 || info->streams == NULL || info->streams->len == 0 ||
	    info->streams_omitted != 0)
		return AXIS_CONVERT_REFUSE_NOT_PROBED;
	if (info->streams->len >= MAX_OUTPUT_STREAMS)
		return AXIS_CONVERT_REFUSE_TOO_MANY_STREAMS;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_VIDEO && s->kind != AXIS_STREAM_AUDIO && s->kind != AXIS_STREAM_SUBTITLE &&
		    s->kind != AXIS_STREAM_ATTACHMENT)
			return AXIS_CONVERT_REFUSE_UNKNOWN_STREAM;
		if (s->index < 0 || s->index > MAX_STREAM_INDEX)
			return AXIS_CONVERT_REFUSE_NOT_PROBED;
		/* `-map 0:<index>` names ffmpeg's stream: two records of one index
		 * would map one stream twice and lose the other. */
		for (j = 0; j < i; j++) {
			if (((const AxisMediaStream *)g_ptr_array_index(info->streams, j))->index == s->index)
				return AXIS_CONVERT_REFUSE_NOT_PROBED;
		}
	}
	main = main_picture(info, &pictures);
	if (pictures != 1 || main == NULL || main->codec == NULL || main->width <= 0 || main->height <= 0)
		return AXIS_CONVERT_REFUSE_PICTURE;

	/* HDR, as the eligibility decides it, and then what the output needs. */
	if (info->hdr_uncertain || main->hdr_uncertain || main->hdr_format == AXIS_HDR_NONE ||
	    info->hdr_format == AXIS_HDR_NONE || (guint)main->hdr_format > AXIS_HDR_DOLBY_VISION)
		return AXIS_CONVERT_REFUSE_HDR_UNCERTAIN;
	if (main->hdr_format == AXIS_HDR_DOLBY_VISION || info->hdr_format == AXIS_HDR_DOLBY_VISION)
		return AXIS_CONVERT_REFUSE_DOLBY_VISION;
	if (main->hdr_format == AXIS_HDR_HDR10PLUS || info->hdr_format == AXIS_HDR_HDR10PLUS)
		return AXIS_CONVERT_REFUSE_HDR10PLUS;
	if (info->hdr_format != main->hdr_format)
		return AXIS_CONVERT_REFUSE_HDR_UNCERTAIN;
	if ((main->hdr_format == AXIS_HDR_HDR10 || main->hdr_format == AXIS_HDR_HLG) &&
	    (!def->keep_hdr || def->codec != AXIS_TCODEC_HEVC))
		return AXIS_CONVERT_REFUSE_HDR_TO_SDR;
	if (main->hdr_format == AXIS_HDR_HDR10 && (!main->hdr10.mastering || !axis_hdr10_metadata_valid(&main->hdr10)))
		return AXIS_CONVERT_REFUSE_HDR10_METADATA;
	if (!axis_hdr10_metadata_valid(&main->hdr10))
		return AXIS_CONVERT_REFUSE_HDR10_METADATA;

	if (info->interlaced == AXIS_TRI_YES || field_based(main->field_order))
		return AXIS_CONVERT_REFUSE_INTERLACED;
	if (info->interlaced != AXIS_TRI_NO && g_strcmp0(main->field_order, "progressive") != 0)
		return AXIS_CONVERT_REFUSE_INTERLACED;

	if (!in_list(main->pixel_format, pixel_formats) || main->bit_depth < 8 || main->bit_depth > 12)
		return AXIS_CONVERT_REFUSE_PIXEL_FORMAT;

	colour->primaries = colour_value(main->color_primaries, axis_convert_colour_names("primaries"), &bad);
	colour->transfer = colour_value(main->color_transfer, axis_convert_colour_names("transfer"), &bad);
	colour->matrix = colour_value(main->color_space, axis_convert_colour_names("matrix"), &bad);
	colour->range = colour_value(main->color_range, axis_convert_colour_names("range"), &bad);
	if (bad)
		return AXIS_CONVERT_REFUSE_COLOUR;

	/* What the container and the profile's rules can carry. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		gboolean mp4 = def->container == AXIS_CONTAINER_MP4;

		switch (s->kind) {
		case AXIS_STREAM_VIDEO:
			if (!s->attached_pic)
				break;
			if (!mp4)
				return AXIS_CONVERT_REFUSE_COVER_IN_MATROSKA;
			if (!in_list(s->codec, mp4_covers))
				return AXIS_CONVERT_REFUSE_COVER_CODEC;
			break;
		case AXIS_STREAM_ATTACHMENT:
			if (mp4)
				return AXIS_CONVERT_REFUSE_ATTACHMENT_IN_MP4;
			break;
		case AXIS_STREAM_SUBTITLE:
			if (s->subtitle_kind == AXIS_SUBTITLE_UNKNOWN)
				return AXIS_CONVERT_REFUSE_SUBTITLE;
			if (mp4 && (def->subtitles != AXIS_SUBTITLES_PLAIN_TEXT || !in_list(s->codec, mp4_text)))
				return AXIS_CONVERT_REFUSE_SUBTITLE;
			if (!mp4 && def->subtitles == AXIS_SUBTITLES_PLAIN_TEXT && !in_list(s->codec, mkv_plain_text) &&
			    !in_list(s->codec, to_subrip))
				return AXIS_CONVERT_REFUSE_SUBTITLE;
			if (!mp4 && def->subtitles == AXIS_SUBTITLES_COPY && !in_list(s->codec, mkv_subtitles) &&
			    !in_list(s->codec, to_subrip))
				return AXIS_CONVERT_REFUSE_SUBTITLE;
			break;
		case AXIS_STREAM_AUDIO:
			has_audio = TRUE;
			if (language_dropped(def, s))
				break;
			kept_audio = TRUE;
			if (s->codec == NULL || (mp4 && !in_list(s->codec, mp4_audio)))
				return AXIS_CONVERT_REFUSE_AUDIO;
			break;
		default:
			break;
		}
	}
	if (has_audio && !kept_audio)
		return AXIS_CONVERT_REFUSE_NO_AUDIO_LEFT;

	*main_out = main;
	return AXIS_CONVERT_OK;
}

/* The picture's arguments for its encoder. */
static void
picture_arguments(
	GPtrArray                *args,
	const AxisConvertEncoder *encoder,
	const Options            *options,
	const Colour             *colour,
	const AxisHdr10Metadata  *hdr10,
	gint                      bit_depth,
	gint                      quality
){
	g_autofree gchar *setparams = setparams_filter(colour);
	gboolean ten = bit_depth >= 10;

	arg_stream(args, "-c", 0, encoder->name);
	if (encoder->family == AXIS_ENCODER_VAAPI) {
		g_autofree gchar *filter = g_strdup_printf("%s%sformat=%s,hwupload", setparams != NULL ? setparams : "",
			setparams != NULL ? "," : "", ten ? "p010le" : "nv12");

		arg_stream(args, "-filter", 0, filter);
	} else {
		if (setparams != NULL)
			arg_stream(args, "-filter", 0, setparams);
		arg_stream(args, "-pix_fmt", 0, encoder->family == AXIS_ENCODER_NVENC ? (ten ? "p010le" : "yuv420p") :
			(ten ? "yuv420p10le" : "yuv420p"));
	}

	switch (encoder->family) {
	case AXIS_ENCODER_NVENC:
		arg_stream(args, "-preset", 0, options->nvenc_preset);
		arg_stream(args, "-rc", 0, "vbr");
		arg_stream_number(args, "-cq", 0, quality);
		arg_stream(args, "-b", 0, "0");
		break;
	case AXIS_ENCODER_VAAPI:
		arg_stream(args, "-rc_mode", 0, "CQP");
		arg_stream_number(args, "-qp", 0, quality);
		break;
	default:
		arg_stream(args, "-preset", 0, options->software_preset);
		arg_stream_number(args, "-crf", 0, quality);
		if (encoder->codec == AXIS_TCODEC_HEVC) {
			GString *params = g_string_new(NULL);

			encoder_colour(params, colour, TRUE);
			if (hdr10->mastering) {
				g_string_append_printf(params, "%shdr10=1:master-display=G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%"
					G_GINT64_FORMAT ",%" G_GINT64_FORMAT ")", params->len > 0 ? ":" : "", hdr10->green_x,
					hdr10->green_y, hdr10->blue_x, hdr10->blue_y, hdr10->red_x, hdr10->red_y, hdr10->white_x,
					hdr10->white_y, hdr10->max_luminance, hdr10->min_luminance);
				/* x265 writes zero light levels with hdr10=1 unless
				 * told not to write them at all. */
				if (hdr10->light)
					g_string_append_printf(params, ":max-cll=%d,%d", hdr10->max_content, hdr10->max_average);
				else
					g_string_append(params, ":cll=0");
			} else if (hdr10->light) {
				g_string_append_printf(params, "%smax-cll=%d,%d", params->len > 0 ? ":" : "", hdr10->max_content,
					hdr10->max_average);
			}
			if (options->threads > 0)
				g_string_append_printf(params, "%spools=%u", params->len > 0 ? ":" : "", options->threads);
			g_string_append_printf(params, "%slog-level=error", params->len > 0 ? ":" : "");
			arg_stream(args, "-x265-params", 0, params->str);
			g_string_free(params, TRUE);
			/* x265 drops A/53 closed captions unless asked (x264, NVENC
			 * and VA-API pass them on by default). */
			arg_stream(args, "-a53cc", 0, "1");
		} else {
			GString *params = g_string_new(NULL);

			encoder_colour(params, colour, FALSE);
			if (params->len > 0)
				arg_stream(args, "-x264-params", 0, params->str);
			g_string_free(params, TRUE);
			if (options->threads > 0)
				arg_stream_number(args, "-threads", 0, options->threads);
		}
		break;
	}
	blank_statistics(args, 0);
}

gboolean
axis_convert_plan_build(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         file_size,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps,
	const AxisConvertOptions      *given,
	AxisConvertPlan              **plan_out,
	AxisConvertRefusal            *refusal_out
){
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(GPtrArray) args = NULL;
	const AxisConvertEncoder *encoder;
	const AxisEncoderCapability *cap;
	const AxisMediaStream *main = NULL;
	const AxisMediaStream *stereo_source = NULL;
	AxisConvertRefusal refusal;
	AxisConvertStream *video;
	Options options;
	Colour colour;
	gboolean mp4;
	gboolean stereo_moved = FALSE;
	gboolean needs_aac = FALSE;
	gboolean needs_mov_text = FALSE;
	gboolean needs_subrip = FALSE;
	gint bit_depth;
	gint crf;
	gint level;
	gint quality;
	guint i;
	guint maps;

	if (plan_out != NULL)
		*plan_out = NULL;
	memset(&colour, 0, sizeof(colour));

	/* ── Refusals, in their order ── */
	if (def == NULL || def->kind != AXIS_PROFILE_KIND_VIDEO) {
		refusal = AXIS_CONVERT_REFUSE_NOT_VIDEO;
		goto refuse;
	}
	if (!axis_profile_definition_check(def, NULL)) {
		refusal = AXIS_CONVERT_REFUSE_DEFINITION;
		goto refuse;
	}
	if (!resolve_options(given, &options)) {
		refusal = AXIS_CONVERT_REFUSE_OPTIONS;
		goto refuse;
	}
	refusal = check_file(def, info, &main, &colour);
	if (refusal != AXIS_CONVERT_OK)
		goto refuse;
	mp4 = def->container == AXIS_CONTAINER_MP4;

	/* The encoder: the profile's, in the table, measured, capable. */
	{
		gboolean listed = FALSE;

		for (i = 0; i < def->n_encoders && i < AXIS_ENCODER_COUNT; i++) {
			if (def->encoders[i] == family)
				listed = TRUE;
		}
		if (!listed) {
			refusal = AXIS_CONVERT_REFUSE_ENCODER_NOT_IN_PROFILE;
			goto refuse;
		}
	}
	encoder = axis_convert_encoder_for(def->codec, family);
	if (encoder == NULL) {
		refusal = AXIS_CONVERT_REFUSE_NOT_CAPABLE;
		goto refuse;
	}
	bit_depth = main->hdr_format == AXIS_HDR_HDR10 || main->hdr_format == AXIS_HDR_HLG ? 10 : def->bit_depth;
	if (caps == NULL || caps->tool_digest == NULL) {
		refusal = AXIS_CONVERT_REFUSE_NOT_MEASURED;
		goto refuse;
	}
	cap = axis_encoder_capabilities_get(caps, encoder);
	if (!cap->measured) {
		refusal = AXIS_CONVERT_REFUSE_NOT_MEASURED;
		goto refuse;
	}
	if (!cap->usable || (bit_depth >= 10 && (!encoder->ten_bit || !cap->ten_bit)) ||
	    (main->hdr_format == AXIS_HDR_HDR10 && !cap->hdr10)) {
		refusal = AXIS_CONVERT_REFUSE_NOT_CAPABLE;
		goto refuse;
	}

	/* Quality: the profile's number for this file, the encoder's own
	 * number for its level. */
	crf = axis_profile_quality_for(def, info->resolution_class, file_size);
	level = axis_estimate_quality_level(def->codec, crf);
	if (level < AXIS_QUALITY_MIN) {
		refusal = AXIS_CONVERT_REFUSE_DEFINITION;
		goto refuse;
	}
	quality = axis_estimate_quality_value(def->codec, family, level);
	if (quality < 1 || quality > 51) {
		refusal = AXIS_CONVERT_REFUSE_INTERNAL;
		goto refuse;
	}

	/* ── The output, stream by stream ── */
	plan = plan_new();
	plan->encoder = encoder;
	plan->container = def->container;
	plan->crf = crf;
	plan->level = level;
	plan->quality = quality;
	plan->bit_depth = bit_depth;
	plan->hdr_format = main->hdr_format;
	plan->chapters = info->chapter_count;
	plan->duration_ms = info->duration_ms;
	plan->frame_rate = main->frame_rate > 0 ? main->frame_rate : -1.0;
	plan->tool_digest = g_strdup(caps->tool_digest);

	/* The picture. */
	video = add_stream(plan, main, AXIS_CONVERT_ENCODE, axis_target_codec_to_string(def->codec));
	video->width = main->width;
	video->height = main->height;
	video->bit_depth = bit_depth;
	video->color_primaries = colour.primaries;
	video->color_transfer = colour.transfer;
	video->color_space = colour.matrix;
	video->color_range = colour.range;
	video->hdr_format = main->hdr_format;
	if (main->hdr_format == AXIS_HDR_HDR10 || main->hdr_format == AXIS_HDR_HLG)
		video->hdr10 = main->hdr10;
	if (colour.primaries == NULL)
		note(plan, NOTE_UNSTATED_PRIMARIES);
	if (colour.transfer == NULL)
		note(plan, NOTE_UNSTATED_TRANSFER);
	if (colour.matrix == NULL)
		note(plan, NOTE_UNSTATED_MATRIX);
	if (colour.range == NULL)
		note(plan, NOTE_UNSTATED_RANGE);
	if (colour.transfer == NULL && bit_depth > 8)
		note(plan, NOTE_DEEP_UNSIGNALLED);
	if ((video->hdr10.mastering || video->hdr10.light) && !encoder->hdr10_stated)
		note(plan, NOTE_HDR10_PASSED_ON);
	note(plan, NOTE_CAPTIONS);

	/* Audio: kept tracks in source order, with a stereo track where the
	 * profile adds one. */
	{
		const AxisMediaStream *first_kept = NULL;
		const AxisMediaStream *default_kept = NULL;
		gboolean has_stereo = FALSE;

		for (i = 0; i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

			if (s->kind != AXIS_STREAM_AUDIO)
				continue;
			if (language_dropped(def, s)) {
				AxisConvertDropped *d = g_new0(AxisConvertDropped, 1);

				d->source = s->index;
				d->kind = s->kind;
				d->reason = REASON_LANGUAGE;
				g_ptr_array_add(plan->dropped, d);
				continue;
			}
			if (first_kept == NULL)
				first_kept = s;
			if (default_kept == NULL && s->is_default)
				default_kept = s;
			if (stereo_aac(s))
				has_stereo = TRUE;
		}
		stereo_source = default_kept != NULL ? default_kept : first_kept;
		if (stereo_source != NULL && def->audio == AXIS_AUDIO_STEREO_FIRST) {
			if (stereo_aac(stereo_source)) {
				/* Already stereo AAC: it goes first as it is. */
				AxisConvertStream *s = add_stream(plan, stereo_source, AXIS_CONVERT_COPY, stereo_source->codec);

				s->channels = stereo_source->channels;
				stereo_moved = TRUE;
			} else {
				AxisConvertStream *s = add_stream(plan, stereo_source, AXIS_CONVERT_STEREO, "aac");

				s->channels = 2;
				g_clear_pointer(&s->title, g_free);
				needs_aac = TRUE;
				note(plan, NOTE_STEREO_TITLE);
			}
		}
		for (i = 0; i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
			AxisConvertStream *out;

			if (s->kind != AXIS_STREAM_AUDIO || language_dropped(def, s))
				continue;
			if (stereo_moved && s == stereo_source)
				continue;
			out = add_stream(plan, s, AXIS_CONVERT_COPY, s->codec);
			out->channels = s->channels > 0 ? s->channels : -1;
		}
		if (stereo_source != NULL && def->audio == AXIS_AUDIO_COPY_ADD_STEREO && !has_stereo) {
			AxisConvertStream *s = add_stream(plan, stereo_source, AXIS_CONVERT_STEREO, "aac");

			s->channels = 2;
			s->is_default = FALSE;
			s->is_forced = FALSE;
			g_clear_pointer(&s->title, g_free);
			needs_aac = TRUE;
			note(plan, NOTE_STEREO_TITLE);
		}
		/* Stereo first: the first audio track is the default one. */
		if (stereo_source != NULL && def->audio == AXIS_AUDIO_STEREO_FIRST) {
			gboolean first = TRUE;

			for (i = 0; i < plan->streams->len; i++) {
				AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

				if (s->kind != AXIS_STREAM_AUDIO)
					continue;
				s->is_default = first;
				first = FALSE;
			}
		}
	}

	/* Subtitles, in source order. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_SUBTITLE)
			continue;
		if (mp4) {
			if (g_strcmp0(s->codec, "mov_text") == 0) {
				add_stream(plan, s, AXIS_CONVERT_COPY, s->codec);
			} else {
				add_stream(plan, s, AXIS_CONVERT_TO_TEXT, "mov_text");
				needs_mov_text = TRUE;
			}
		} else if (in_list(s->codec, to_subrip)) {
			add_stream(plan, s, AXIS_CONVERT_TO_TEXT, "subrip");
			needs_subrip = TRUE;
		} else {
			add_stream(plan, s, AXIS_CONVERT_COPY, s->codec);
		}
	}

	/* Attachments (Matroska) and cover pictures (MP4), in source order. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind == AXIS_STREAM_ATTACHMENT || (s->kind == AXIS_STREAM_VIDEO && s->attached_pic)) {
			AxisConvertStream *out = add_stream(plan, s, AXIS_CONVERT_COPY, s->codec);

			if (s->kind == AXIS_STREAM_ATTACHMENT) {
				out->is_default = FALSE;
				out->is_forced = FALSE;
			}
		}
	}

	if ((needs_aac && !caps->aac) || (needs_mov_text && !caps->mov_text) || (needs_subrip && !caps->subrip)) {
		refusal = AXIS_CONVERT_REFUSE_HELPER_ENCODER;
		goto refuse;
	}
	maps = plan->streams->len;
	if (maps >= MAX_OUTPUT_STREAMS) {
		refusal = AXIS_CONVERT_REFUSE_TOO_MANY_STREAMS;
		goto refuse;
	}

	/* ── The arguments ── */
	args = g_ptr_array_new_with_free_func(g_free);
	arg(args, "-nostdin");
	arg(args, "-hide_banner");
	arg(args, "-loglevel");
	arg(args, "error");
	arg(args, "-nostats");
	arg(args, "-progress");
	arg(args, "pipe:1");
	if (encoder->family == AXIS_ENCODER_VAAPI) {
		arg(args, "-vaapi_device");
		arg(args, options.vaapi_device);
	}
	/* The one file given, nothing it may name. */
	arg(args, "-protocol_whitelist");
	arg(args, "fd");
	arg(args, "-format_whitelist");
	arg(args, axis_probe_format_whitelist());
	arg(args, "-fd");
	arg(args, "3");
	arg(args, "-i");
	arg(args, "fd:");
	for (i = 0; i < maps; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

		g_ptr_array_add(args, g_strdup("-map"));
		g_ptr_array_add(args, g_strdup_printf("0:%d", s->source));
	}
	arg(args, "-map_chapters");
	arg(args, "0");
	arg(args, "-map_metadata");
	arg(args, "0");
	for (i = 0; i < maps; i++) {
		const AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

		switch (s->action) {
		case AXIS_CONVERT_ENCODE:
			picture_arguments(args, encoder, &options, &colour, &s->hdr10, bit_depth, quality);
			break;
		case AXIS_CONVERT_STEREO:
			arg_stream(args, "-c", s->position, "aac");
			arg_stream_number(args, "-b", s->position, def->stereo_bitrate);
			arg_stream(args, "-ac", s->position, "2");
			g_ptr_array_add(args, g_strdup_printf("-metadata:s:%u", s->position));
			g_ptr_array_add(args, g_strdup("title="));
			blank_statistics(args, s->position);
			break;
		case AXIS_CONVERT_TO_TEXT:
			arg_stream(args, "-c", s->position, mp4 ? "mov_text" : "srt");
			break;
		default:
			arg_stream(args, "-c", s->position, "copy");
			break;
		}
		disposition(args, s);
	}
	if (mp4) {
		arg(args, "-f");
		arg(args, "mp4");
	} else {
		arg(args, "-default_mode");
		arg(args, "passthrough");
		arg(args, "-f");
		arg(args, "matroska");
	}
	arg(args, "-protocol_whitelist");
	arg(args, "fd");
	arg(args, "-fd");
	arg(args, "4");
	arg(args, "fd:");
	g_ptr_array_add(args, NULL);
	plan->args = (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);

	/* What the container does by itself, as the output will show it. */
	if (mp4) {
		AxisStreamKind kinds[3] = { AXIS_STREAM_VIDEO, AXIS_STREAM_AUDIO, AXIS_STREAM_SUBTITLE };
		guint k;

		note(plan, NOTE_MP4_TITLES);
		note(plan, NOTE_MP4_TAGS);
		for (i = 0; i < plan->streams->len; i++)
			((AxisConvertStream *)g_ptr_array_index(plan->streams, i))->check_title = FALSE;
		for (k = 0; k < G_N_ELEMENTS(kinds); k++) {
			AxisConvertStream *first = NULL;
			gboolean any = FALSE;

			for (i = 0; i < plan->streams->len; i++) {
				AxisConvertStream *s = g_ptr_array_index(plan->streams, i);

				if (s->kind != kinds[k] || s->attached_pic)
					continue;
				if (first == NULL)
					first = s;
				if (s->is_default)
					any = TRUE;
			}
			if (first != NULL && !any) {
				first->is_default = TRUE;
				note(plan, NOTE_MP4_DEFAULT);
			}
		}
		if (info->chapter_count > 0) {
			AxisConvertStream *chapters = add_stream(plan, NULL, AXIS_CONVERT_MUXER, "bin_data");

			chapters->check_title = FALSE;
			note(plan, NOTE_MP4_CHAPTERS);
		}
	}

	/* The vector checks against the grammar, read separately. */
	if (!axis_convert_args_valid((const gchar *const *)plan->args, info, NULL)) {
		refusal = AXIS_CONVERT_REFUSE_INTERNAL;
		goto refuse;
	}

	if (refusal_out != NULL)
		*refusal_out = AXIS_CONVERT_OK;
	if (plan_out != NULL)
		*plan_out = g_steal_pointer(&plan);
	return TRUE;

refuse:
	if (refusal_out != NULL)
		*refusal_out = refusal;
	return FALSE;
}

gboolean
axis_convert_plan_choose(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         file_size,
	const AxisEncoderCapabilities *caps,
	const AxisConvertOptions      *options,
	AxisConvertPlan              **plan,
	AxisConvertRefusal            *refusal,
	AxisConvertRefusal            *refusals
){
	AxisConvertRefusal first = AXIS_CONVERT_REFUSE_NOT_VIDEO;
	guint i;

	if (plan != NULL)
		*plan = NULL;
	if (refusals != NULL) {
		for (i = 0; i < AXIS_ENCODER_COUNT; i++)
			refusals[i] = AXIS_CONVERT_OK;
	}
	if (def == NULL || def->kind != AXIS_PROFILE_KIND_VIDEO || def->n_encoders == 0) {
		if (refusal != NULL)
			*refusal = def != NULL && def->kind == AXIS_PROFILE_KIND_VIDEO ? AXIS_CONVERT_REFUSE_DEFINITION :
				AXIS_CONVERT_REFUSE_NOT_VIDEO;
		return FALSE;
	}
	for (i = 0; i < def->n_encoders && i < AXIS_ENCODER_COUNT; i++) {
		AxisConvertRefusal one = AXIS_CONVERT_OK;

		if (axis_convert_plan_build(def, info, file_size, def->encoders[i], caps, options, plan, &one)) {
			if (refusals != NULL)
				refusals[i] = AXIS_CONVERT_OK;
			if (refusal != NULL)
				*refusal = AXIS_CONVERT_OK;
			return TRUE;
		}
		if (refusals != NULL)
			refusals[i] = one;
		if (i == 0)
			first = one;
	}
	if (refusal != NULL)
		*refusal = first;
	return FALSE;
}
