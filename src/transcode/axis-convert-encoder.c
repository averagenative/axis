/*
 * axis-convert-encoder.c - the encoder table, the colour names the plans
 * may state, the refusals, and the pure parts of the capability check
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Where each entry comes from (design.md of add-ffmpeg-arguments has the
 * experiments):
 *
 *  - libx265, libx264: their options as `ffmpeg -h encoder=...` prints
 *    them in 7.1.5 and 8.1.3; colour and HDR10 as measured on both: every
 *    property reaches the output when the frames carry it (`setparams`
 *    puts the source's values on them) and the encoder's own parameters
 *    (`x265-params`, `x264-params`) state it in the bitstream; x265 is
 *    given the mastering display and content light values themselves
 *    (`master-display`, `max-cll`, or `cll=0` when there are none, since
 *    with `hdr10=1` it otherwise writes zeros).
 *  - hevc_nvenc, h264_nvenc: their options as both versions print them.
 *    Neither version documents any option for mastering display or content
 *    light data, so the arguments cannot state HDR10's values: only
 *    ffmpeg's passing on of the frames' side data (the decoder's export of
 *    the source's) can carry them, and only the capability check on the
 *    machine shows whether it does.
 *  - hevc_vaapi, h264_vaapi: likewise; `-sei hdr` (on by default in both
 *    versions) documents that HDR metadata from the frames is written, with
 *    no option to give values.
 *
 * Measured with ffmpeg 8.1.3 on the development machine only (an NVIDIA GPU
 * of the Ada generation for NVENC; Intel iHD 26.1.5 through libva 1.23 on
 * /dev/dri/renderD128 for VA-API), the plans' own options, one-second and
 * three-second test clips (design.md has the readings): both HEVC encoders
 * write Main 10, 10 bits, the colour stated on the frames in the VUI, and
 * the mastering display and content light values of the frames exactly
 * (odd values too), as SEI at every keyframe but not in the stream header
 * (x265 puts them in both); no light levels in, none out (not zeros); side data
 * removed from the frames, nothing out (the values come from the frames
 * alone); Matroska and MP4 get the values in the container too.  Never run
 * with 7.1 (no device reaches the container image), nor on a Turing
 * generation GPU.  The tests run them only under `make check-hwenc`.
 */

#include "transcode/axis-convert.h"

#include <string.h>

/* ── The table ──────────────────────────────────────────────────── */

static const AxisConvertEncoder encoders[AXIS_CONVERT_N_ENCODERS] = {
	{
		"libx265", AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE, TRUE, "-crf", "CRF", TRUE,
		"stated: x265-params master-display and max-cll (or cll=0), hdr10=1",
		"setparams on the frames, and x265-params colorprim, transfer, colormatrix, range",
		TRUE
	},
	{
		"libx264", AXIS_TCODEC_H264, AXIS_ENCODER_SOFTWARE, FALSE, "-crf", "CRF", FALSE,
		"none: H.264 targets are SDR only",
		"setparams on the frames, and x264-params colorprim, transfer, colormatrix, range",
		TRUE
	},
	{
		"hevc_nvenc", AXIS_TCODEC_HEVC, AXIS_ENCODER_NVENC, TRUE, "-cq", "CQ", FALSE,
		"passed on, not stated: no option in 7.1 or 8.1; ffmpeg passes the frames' side data on, which reached the "
		"bitstream exactly on 8.1.3 with an NVIDIA GPU of the Ada generation; unproven elsewhere: the capability check "
		"decides",
		"setparams on the frames (ffmpeg gives the frames' colour to the encoder)",
		TRUE
	},
	{
		"h264_nvenc", AXIS_TCODEC_H264, AXIS_ENCODER_NVENC, FALSE, "-cq", "CQ", FALSE,
		"none: H.264 targets are SDR only",
		"setparams on the frames (ffmpeg gives the frames' colour to the encoder)",
		TRUE
	},
	{
		"hevc_vaapi", AXIS_TCODEC_HEVC, AXIS_ENCODER_VAAPI, TRUE, "-qp", "QP", FALSE,
		"passed on, not stated: -sei hdr (on by default) writes what the frames carry, which reached the bitstream "
		"exactly on 8.1.3 with Intel iHD 26.1.5; no option to give values; unproven elsewhere: the capability check "
		"decides",
		"setparams on the frames before they are uploaded",
		TRUE
	},
	{
		"h264_vaapi", AXIS_TCODEC_H264, AXIS_ENCODER_VAAPI, FALSE, "-qp", "QP", FALSE,
		"none: H.264 targets are SDR only",
		"setparams on the frames before they are uploaded",
		TRUE
	}
};

const AxisConvertEncoder *
axis_convert_encoders(void)
{
	return encoders;
}

const AxisConvertEncoder *
axis_convert_encoder_for(
	AxisTargetCodec   codec,
	AxisEncoderFamily family
){
	guint i;

	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		if (encoders[i].codec == codec && encoders[i].family == family)
			return &encoders[i];
	}
	return NULL;
}

const AxisConvertEncoder *
axis_convert_encoder_named(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && i < AXIS_CONVERT_N_ENCODERS; i++) {
		if (strcmp(encoders[i].name, name) == 0)
			return &encoders[i];
	}
	return NULL;
}

guint
axis_convert_encoder_index(const AxisConvertEncoder *encoder)
{
	g_return_val_if_fail(encoder >= encoders && encoder < encoders + AXIS_CONVERT_N_ENCODERS, 0);

	return (guint)(encoder - encoders);
}

/* ── Colour names ───────────────────────────────────────────────── */

/*
 * The names a plan states, as ffprobe prints them.  Each one was put
 * through setparams, x265-params and x264-params on 7.1.5 and 8.1.3 and
 * came back under the same name in the container and the bitstream
 * (design.md); setparams, x265 and x264 take these very spellings (ffmpeg's
 * own -color_trc calls two of the transfers gamma22 and gamma28; ffprobe,
 * setparams and the encoders say bt470m and bt470bg).  Exotic matrices
 * (gbr, ycgco, ictcp, chroma-derived, ...) and primaries the encoders do
 * not name (ebu3213) are not here: a source that states one gets no plan.
 */
static const gchar *const primaries_names[] = {
	"bt709", "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020", "smpte428", "smpte431", "smpte432",
	NULL
};
static const gchar *const transfer_names[] = {
	"bt709", "bt470m", "bt470bg", "smpte170m", "smpte240m", "linear", "log100", "log316", "iec61966-2-4",
	"bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12", "smpte2084", "smpte428", "arib-std-b67", NULL
};
static const gchar *const matrix_names[] = {
	"bt709", "fcc", "bt470bg", "smpte170m", "smpte240m", "bt2020nc", "bt2020c", NULL
};
static const gchar *const range_names[] = { "tv", "pc", NULL };

const gchar *const *
axis_convert_colour_names(const gchar *property)
{
	if (g_strcmp0(property, "primaries") == 0)
		return primaries_names;
	if (g_strcmp0(property, "transfer") == 0)
		return transfer_names;
	if (g_strcmp0(property, "matrix") == 0)
		return matrix_names;
	if (g_strcmp0(property, "range") == 0)
		return range_names;
	return NULL;
}

/* ── Refusals ───────────────────────────────────────────────────── */

static const struct
{
	const gchar *id;
	const gchar *text;
} refusals[AXIS_CONVERT_N_REFUSALS] = {
	{ "ok", "There is a plan." },
	{ "not_video", "The profile is not a video profile; audio conversion is not built yet." },
	{ "definition", "The profile's definition does not pass its check." },
	{ "options", "A preset, device or thread count of this machine's settings is not one Axis knows." },
	{ "not_probed", "There is no usable probe of the file, streams were left out of it, or its stream numbers are not "
		"ones ffmpeg gives." },
	{ "unknown_stream", "The file has a data stream or a stream of unknown kind, which a conversion would lose." },
	{ "picture", "The file does not have exactly one picture, or its codec or size is not known." },
	{ "hdr_uncertain", "Its HDR signalling is uncertain or not classified." },
	{ "dolby_vision", "Dolby Vision is never converted." },
	{ "hdr10plus", "HDR10+ is not converted until it is verified that its dynamic metadata survives." },
	{ "hdr_to_sdr", "The source is HDR and the profile would not keep it." },
	{ "hdr10_metadata", "The HDR10 source's mastering display values are not known, so they cannot be carried over." },
	{ "interlaced", "The picture is not known to be progressive." },
	{ "pixel_format", "The picture is not 4:2:0, or deeper than 12 bits, or its format is not known." },
	{ "colour", "The source states a colour property the encoders have no name for." },
	{ "cover_in_matroska", "The file has a cover picture: ffmpeg would write it into Matroska as a video track." },
	{ "cover_codec", "The file has a cover picture in a format MP4 cannot hold." },
	{ "attachment_in_mp4", "The file has attachments, which MP4 cannot hold." },
	{ "subtitle", "The file has a subtitle track the profile's container or subtitle rule cannot carry." },
	{ "audio", "The file has an audio track the profile's container cannot hold." },
	{ "no_audio_left", "The profile's languages would leave the file without sound." },
	{ "encoder_not_in_profile", "The profile does not allow this encoder." },
	{ "not_measured", "This machine's ffmpeg has not been measured." },
	{ "not_capable", "This machine's encoder was not shown able to produce this output." },
	{ "helper_encoder", "This machine's ffmpeg lacks the AAC, mov_text or SubRip encoder the plan needs." },
	{ "too_many_streams", "The file has more streams than a plan carries." },
	{ "internal", "The arguments failed their own check (a bug in Axis): nothing is converted." }
};

const gchar *
axis_convert_refusal_to_string(AxisConvertRefusal refusal)
{
	return (guint)refusal < AXIS_CONVERT_N_REFUSALS ? refusals[refusal].id : "unknown";
}

const gchar *
axis_convert_refusal_describe(AxisConvertRefusal refusal)
{
	return (guint)refusal < AXIS_CONVERT_N_REFUSALS ? refusals[refusal].text : "Unknown.";
}

const gchar *
axis_convert_action_to_string(AxisConvertAction action)
{
	switch (action) {
	case AXIS_CONVERT_COPY:
		return "copy";
	case AXIS_CONVERT_ENCODE:
		return "encode";
	case AXIS_CONVERT_STEREO:
		return "stereo";
	case AXIS_CONVERT_TO_TEXT:
		return "to_text";
	case AXIS_CONVERT_MUXER:
		return "muxer";
	default:
		return "unknown";
	}
}

/* ── Capabilities ───────────────────────────────────────────────── */

AxisEncoderCapabilities *
axis_encoder_capabilities_new(void)
{
	return g_new0(AxisEncoderCapabilities, 1);
}

void
axis_encoder_capabilities_free(AxisEncoderCapabilities *caps)
{
	guint i;

	if (caps == NULL)
		return;
	g_free(caps->tool_version);
	g_free(caps->tool_digest);
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++)
		g_free(caps->encoders[i].reason);
	g_free(caps);
}

AxisEncoderCapabilities *
axis_encoder_capabilities_copy(const AxisEncoderCapabilities *caps)
{
	AxisEncoderCapabilities *copy;
	guint i;

	if (caps == NULL)
		return NULL;
	copy = g_new0(AxisEncoderCapabilities, 1);
	*copy = *caps;
	copy->tool_version = g_strdup(caps->tool_version);
	copy->tool_digest = g_strdup(caps->tool_digest);
	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++)
		copy->encoders[i].reason = g_strdup(caps->encoders[i].reason);
	return copy;
}

const AxisEncoderCapability *
axis_encoder_capabilities_get(
	const AxisEncoderCapabilities *caps,
	const AxisConvertEncoder      *encoder
){
	g_return_val_if_fail(caps != NULL && encoder != NULL, NULL);

	return &caps->encoders[axis_convert_encoder_index(encoder)];
}

void
axis_encoder_capabilities_set(
	AxisEncoderCapabilities  *caps,
	const AxisConvertEncoder *encoder,
	gboolean                  usable,
	gboolean                  ten_bit,
	gboolean                  hdr10,
	const gchar              *reason
){
	AxisEncoderCapability *cap;

	g_return_if_fail(caps != NULL && encoder != NULL);

	cap = &caps->encoders[axis_convert_encoder_index(encoder)];
	g_free(cap->reason);
	cap->measured = TRUE;
	/* Each needs the one before it. */
	cap->usable = usable;
	cap->ten_bit = usable && ten_bit && encoder->ten_bit;
	cap->hdr10 = cap->ten_bit && hdr10;
	cap->reason = g_strdup(reason);
}

/* ── What ffmpeg says about itself ──────────────────────────────── */

gchar *
axis_convert_parse_version(
	const gchar *text,
	gsize        length
){
	static const gchar head[] = "ffmpeg version ";
	gsize at;
	gsize end;

	if (text == NULL || length < sizeof(head) || memcmp(text, head, sizeof(head) - 1) != 0)
		return NULL;
	at = sizeof(head) - 1;
	end = at;
	/* A version is short and of a plain alphabet; anything else is not
	 * one ffmpeg wrote. */
	while (end < length && end - at < 64 && (g_ascii_isalnum(text[end]) || text[end] == '.' || text[end] == '-' ||
	       text[end] == '+' || text[end] == '_' || text[end] == '~'))
		end++;
	if (end == at || end - at >= 64 || (end < length && text[end] != ' ' && text[end] != '\n'))
		return NULL;
	return g_strndup(text + at, end - at);
}

gboolean
axis_convert_encoders_lists(
	const gchar *text,
	gsize        length,
	const gchar *name
){
	gsize name_length;
	gsize at = 0;
	gboolean listing = FALSE;

	if (text == NULL || name == NULL || name[0] == '\0')
		return FALSE;
	name_length = strlen(name);
	/* After the legend and its " ------" line, lines of
	 * " V....D libx265              libx265 H.265 / HEVC ...": the name is
	 * the second word, after six flag characters.  Without the separator
	 * line nothing counts as listed. */
	while (at < length) {
		gsize line_end = at;
		gsize p;
		gsize word;

		while (line_end < length && text[line_end] != '\n')
			line_end++;
		p = at;
		while (p < line_end && text[p] == ' ')
			p++;
		if (!listing) {
			gsize q = p;

			while (q < line_end && text[q] == '-')
				q++;
			listing = q == line_end && line_end - p >= 6;
			at = line_end + 1;
			continue;
		}
		/* The flags: six of "VASFXBD." */
		word = p;
		while (p < line_end && text[p] != ' ')
			p++;
		if (p - word == 6) {
			while (p < line_end && text[p] == ' ')
				p++;
			if (line_end - p >= name_length && memcmp(text + p, name, name_length) == 0 &&
			    (p + name_length == line_end || text[p + name_length] == ' '))
				return TRUE;
		}
		at = line_end + 1;
	}
	return FALSE;
}
