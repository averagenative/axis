/*
 * axis-estimate-target.c - the built-in targets and their names
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "transcode/axis-estimate-target.h"

#include <stdlib.h>
#include <string.h>

#include "storage/axis-storage.h"
#include "transcode/axis-profile.h"

/* ── Names ──────────────────────────────────────────────────────── */

static const gchar *const codec_names[AXIS_TCODEC_COUNT] = {
	"hevc", "h264", "opus", "aac", "mp3", "flac"
};
static const gchar *const codec_labels[AXIS_TCODEC_COUNT] = {
	"HEVC", "H.264", "Opus", "AAC", "MP3", "FLAC"
};
static const gchar *const family_names[AXIS_ENCODER_COUNT] = { "software", "nvenc", "vaapi" };
static const gchar *const family_labels[AXIS_ENCODER_COUNT] = {
	"software (x265, x264, libopus, LAME)", "NVIDIA NVENC", "VA-API"
};

const gchar *
axis_target_codec_to_string(AxisTargetCodec codec)
{
	return (guint)codec < AXIS_TCODEC_COUNT ? codec_names[codec] : "unknown";
}

gboolean
axis_target_codec_from_string(
	const gchar     *text,
	AxisTargetCodec *codec
){
	guint i;

	for (i = 0; text != NULL && i < AXIS_TCODEC_COUNT; i++) {
		if (strcmp(text, codec_names[i]) == 0) {
			*codec = (AxisTargetCodec)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_target_codec_label(AxisTargetCodec codec)
{
	return (guint)codec < AXIS_TCODEC_COUNT ? codec_labels[codec] : "unknown";
}

const gchar *
axis_encoder_family_to_string(AxisEncoderFamily family)
{
	return (guint)family < AXIS_ENCODER_COUNT ? family_names[family] : "software";
}

gboolean
axis_encoder_family_from_string(
	const gchar       *text,
	AxisEncoderFamily *family
){
	guint i;

	for (i = 0; text != NULL && i < AXIS_ENCODER_COUNT; i++) {
		if (strcmp(text, family_names[i]) == 0) {
			*family = (AxisEncoderFamily)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_encoder_family_label(AxisEncoderFamily family)
{
	return (guint)family < AXIS_ENCODER_COUNT ? family_labels[family] : family_labels[0];
}

const gchar *
axis_target_container_to_string(AxisTargetContainer container)
{
	switch (container) {
	case AXIS_CONTAINER_MATROSKA:
		return "mkv";
	case AXIS_CONTAINER_MP4:
		return "mp4";
	case AXIS_CONTAINER_FLAC:
		return "flac";
	case AXIS_CONTAINER_OGG:
		return "ogg";
	case AXIS_CONTAINER_MP3:
		return "mp3";
	default:
		return "unknown";
	}
}

/* ── Quality ────────────────────────────────────────────────────── */

/*
 * The encoders' own numbers for each quality level (1 smallest, 7 best),
 * by codec and family.  Software: the x265 and x264 CRF the profiles
 * name.  NVENC (constant quality: -rc vbr -cq N -b:v 0) and VA-API
 * (constant QP: -rc_mode CQP -qp N) have their own tables, not the CRF
 * numbers reused: a hardware encoder at the same QP-like number makes a
 * worse picture than x264 or x265, so each table asks for a better number
 * by the step that buys the extra bits the size model already assumes
 * these encoders need for a similar picture (axis-estimate-model.c:
 * NVENC 1.30 and 1.25 times x265 and x264, VA-API 1.40 and 1.35), under
 * the model's own rule that six steps double the bits: 6 log2(1.30) = 2.3
 * and 6 log2(1.25) = 1.9, so NVENC is 2 below; 6 log2(1.40) = 2.9 and
 * 6 log2(1.35) = 2.6, so VA-API is 3 below.  These are PRIORS, the
 * author's reasoning and nothing measured (the hardware encoders have only
 * been run on one-second test pictures, to show what they carry, never to
 * compare a picture's quality): the canary step checks them for quality
 * and the calibration for size, as for every other prior.  The size model reads a
 * level's slope from the software number (a level is the same picture in
 * every family), so these tables do not move its priors.
 */
static const gint hevc_software[AXIS_QUALITY_MAX] = { 32, 30, 28, 26, 24, 22, 20 };
static const gint hevc_nvenc[AXIS_QUALITY_MAX]    = { 30, 28, 26, 24, 22, 20, 18 };
static const gint hevc_vaapi[AXIS_QUALITY_MAX]    = { 29, 27, 25, 23, 21, 19, 17 };
/* x264's useful range is narrower and lower than x265's. */
static const gint h264_software[AXIS_QUALITY_MAX] = { 28, 26, 24, 22, 20, 19, 18 };
static const gint h264_nvenc[AXIS_QUALITY_MAX]    = { 26, 24, 22, 20, 18, 17, 16 };
static const gint h264_vaapi[AXIS_QUALITY_MAX]    = { 25, 23, 21, 19, 17, 16, 15 };

gint
axis_estimate_quality_value(
	AxisTargetCodec   codec,
	AxisEncoderFamily family,
	gint              level
){
	/* FLAC: the compression level (higher is smaller, the sound the same). */
	static const gint flac[AXIS_QUALITY_MAX] = { 5, 6, 6, 8, 8, 8, 8 };

	level = CLAMP(level, AXIS_QUALITY_MIN, AXIS_QUALITY_MAX);
	switch (codec) {
	case AXIS_TCODEC_HEVC:
		return family == AXIS_ENCODER_NVENC ? hevc_nvenc[level - 1] :
			family == AXIS_ENCODER_VAAPI ? hevc_vaapi[level - 1] : hevc_software[level - 1];
	case AXIS_TCODEC_H264:
		return family == AXIS_ENCODER_NVENC ? h264_nvenc[level - 1] :
			family == AXIS_ENCODER_VAAPI ? h264_vaapi[level - 1] : h264_software[level - 1];
	case AXIS_TCODEC_FLAC:
		return flac[level - 1];
	default:
		return level;
	}
}

gint
axis_estimate_quality_level(
	AxisTargetCodec codec,
	gint            crf
){
	gint level;

	if (codec != AXIS_TCODEC_HEVC && codec != AXIS_TCODEC_H264)
		return -1;
	for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
		if (axis_estimate_quality_value(codec, AXIS_ENCODER_SOFTWARE, level) == crf)
			return level;
	}
	return -1;
}

gboolean
axis_estimate_rule_matches(
	const gchar *rule_resolution,
	gint64       min_bytes,
	gint64       max_bytes,
	const gchar *resolution_class,
	gint64       size
){
	if (rule_resolution != NULL && g_strcmp0(rule_resolution, resolution_class) != 0)
		return FALSE;
	if (min_bytes > 0 && size < min_bytes)
		return FALSE;
	if (max_bytes > 0 && size >= max_bytes)
		return FALSE;
	return TRUE;
}

/* ── The built-in targets ───────────────────────────────────────── */

/* Built once from the built-in profiles' definitions, through the same
 * function every other profile's target is made by.  A definition in code
 * that does not validate is a programming error: the program stops rather
 * than estimate with something else. */
static AxisEstimateTarget builtins[AXIS_ESTIMATE_N_TARGETS];
static const gchar *target_ids[AXIS_ESTIMATE_N_TARGETS + 1];

static void
build_builtins(void)
{
	static gsize ready = 0;

	if (g_once_init_enter(&ready)) {
		const AxisProfileBuiltin *list = axis_profile_builtins();
		guint i;

		for (i = 0; i < AXIS_ESTIMATE_N_TARGETS; i++) {
			g_autoptr(GPtrArray) problems = axis_profile_problems_new();
			AxisProfileDefinition def;
			AxisProfileIdentity identity;

			if (!axis_profile_definition_parse(list[i].definition, -1, &def, problems)) {
				const AxisProfileProblem *p = problems->len > 0 ? g_ptr_array_index(problems, 0) : NULL;

				g_printerr("axis: the built-in profile %s does not validate: %s: %s\n", list[i].slug,
					p != NULL ? p->field : "?", p != NULL ? p->message : "?");
				abort();
			}
			memset(&identity, 0, sizeof(identity));
			identity.slug = list[i].slug;
			identity.name = list[i].name;
			identity.family = list[i].family;
			identity.summary = list[i].summary;
			if (!axis_profile_to_target(&def, &identity, &builtins[i]))
				abort();
			target_ids[i] = builtins[i].id;
		}
		target_ids[AXIS_ESTIMATE_N_TARGETS] = NULL;
		g_once_init_leave(&ready, 1);
	}
}

const AxisEstimateTarget *
axis_estimate_builtin_targets(void)
{
	build_builtins();
	return builtins;
}

const AxisEstimateTarget *
axis_estimate_builtin_target(const gchar *id)
{
	guint i;

	build_builtins();
	for (i = 0; id != NULL && i < AXIS_ESTIMATE_N_TARGETS; i++) {
		if (strcmp(builtins[i].id, id) == 0)
			return &builtins[i];
	}
	return NULL;
}

const gchar *const *
axis_estimate_target_ids(void)
{
	build_builtins();
	return target_ids;
}

void
axis_estimate_config_init(AxisEstimateConfig *config)
{
	memset(config, 0, sizeof(*config));
	config->encoder = AXIS_ENCODER_SOFTWARE;
}

void
axis_estimate_target_configure(
	const AxisEstimateTarget *builtin,
	const AxisEstimateConfig *config,
	AxisEstimateTarget       *target
){
	guint i;

	*target = *builtin;
	if (config == NULL || builtin->media != AXIS_TARGET_VIDEO)
		return;
	/* Audio is always encoded in software: the families are about
	 * pictures.  A video target assumes the encoder the workers have when
	 * its profile allows that one, else the profile's first choice. */
	if (builtin->n_encoders == 0) {
		target->encoder = AXIS_ENCODER_SOFTWARE;
		return;
	}
	target->encoder = builtin->encoders[0];
	for (i = 0; i < builtin->n_encoders && i < AXIS_ENCODER_COUNT; i++) {
		if (builtin->encoders[i] == config->encoder)
			target->encoder = config->encoder;
	}
}

gint
axis_estimate_target_quality_for(
	const AxisEstimateTarget *target,
	const gchar              *resolution_class,
	gint64                    size
){
	gint level = target->quality;
	guint i;

	for (i = 0; i < target->n_quality_rules && i < AXIS_ESTIMATE_MAX_QUALITY_RULES; i++) {
		const AxisEstimateQualityRule *rule = &target->quality_rules[i];

		if (axis_estimate_rule_matches(rule->resolution, rule->min_bytes, rule->max_bytes, resolution_class, size)) {
			level = rule->level;
			break;
		}
	}
	return CLAMP(level, AXIS_QUALITY_MIN, AXIS_QUALITY_MAX);
}

/* "50 GB" for a whole number of gigabytes, else to one decimal. */
static gchar *
gigabytes(gint64 bytes)
{
	if (bytes % 1000000000 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " GB", bytes / 1000000000);
	return g_strdup_printf("%.1f GB", (gdouble)bytes / 1e9);
}

static void
describe_rules(
	GString                  *text,
	const AxisEstimateTarget *target
){
	guint i;

	for (i = 0; i < target->n_quality_rules && i < AXIS_ESTIMATE_MAX_QUALITY_RULES; i++) {
		const AxisEstimateQualityRule *rule = &target->quality_rules[i];
		gint value = axis_estimate_quality_value(target->codec, AXIS_ENCODER_SOFTWARE, rule->level);

		g_string_append_printf(text, "%s %d for ", i == 0 ? "; then the first that applies of:" : ";", value);
		if (rule->resolution != NULL)
			g_string_append_printf(text, "%s ", rule->resolution);
		g_string_append(text, "sources");
		if (rule->min_bytes > 0) {
			g_autofree gchar *size = gigabytes(rule->min_bytes);

			g_string_append_printf(text, " of %s or more", size);
		}
		if (rule->max_bytes > 0) {
			g_autofree gchar *size = gigabytes(rule->max_bytes);

			g_string_append_printf(text, "%s under %s", rule->min_bytes > 0 ? " and" : "", size);
		}
	}
}

gchar *
axis_estimate_target_describe(const AxisEstimateTarget *target)
{
	GString *text = g_string_new(NULL);

	if (target->media == AXIS_TARGET_VIDEO) {
		g_string_append_printf(text, "Video: %s %s, %d-bit", axis_target_codec_label(target->codec),
			target->profile != NULL ? target->profile : "", target->bit_depth);
		if (target->hdr_bit_depth > 0 && target->hdr_bit_depth != target->bit_depth)
			g_string_append_printf(text, " (%d-bit for HDR10 and HLG sources, whose metadata is kept)",
				target->hdr_bit_depth);
		else if (target->hdr_bit_depth > 0)
			g_string_append(text, " (HDR10 and HLG metadata kept)");
		else
			g_string_append(text, ", SDR only (HDR sources are skipped)");
		g_string_append_printf(text, ", encoded with %s at %s %d (quality level %d of %d)",
			axis_encoder_family_label(target->encoder),
			target->encoder == AXIS_ENCODER_SOFTWARE ? "CRF" :
			target->encoder == AXIS_ENCODER_NVENC ? "CQ" : "QP",
			axis_estimate_quality_value(target->codec, target->encoder, target->quality), target->quality,
			AXIS_QUALITY_MAX);
		describe_rules(text, target);
		g_string_append(text, ". ");
		switch (target->audio) {
		case AXIS_AUDIO_COPY_ADD_STEREO:
			g_string_append_printf(text, "Audio: every track copied, and a stereo AAC track at %d kbit/s "
				"added unless there is one. ", target->audio_bitrate / 1000);
			break;
		case AXIS_AUDIO_STEREO_FIRST:
			g_string_append_printf(text, "Audio: a stereo AAC track at %d kbit/s first (unless the first "
				"track is one), the original kept after it. ", target->audio_bitrate / 1000);
			break;
		default:
			g_string_append(text, "Audio: every track copied, never re-encoded. ");
			break;
		}
		if (target->keep_audio_languages != NULL)
			g_string_append(text, "Audio tracks in other known languages are dropped. ");
		g_string_append(text, target->subtitles == AXIS_SUBTITLES_PLAIN_TEXT ?
			"Subtitles: plain text tracks kept as MP4 text; a file with image or styled subtitles is "
			"skipped. " : "Subtitles: every track copied. ");
		g_string_append_printf(text, "Container: %s.", target->container == AXIS_CONTAINER_MP4 ?
			"MP4" : "Matroska");
	} else {
		if (target->codec == AXIS_TCODEC_FLAC)
			g_string_append_printf(text, "Audio: FLAC at compression level %d%s.",
				axis_estimate_quality_value(target->codec, target->encoder, target->quality),
				target->reduce_to_cd ? ", reduced to 16 bits at 44.1 kHz" : ", same bits and rate");
		else if (target->audio_quality >= 0)
			g_string_append_printf(text, "Audio: %s VBR quality %d (about %d kbit/s).",
				axis_target_codec_label(target->codec), target->audio_quality, target->audio_bitrate / 1000);
		else
			g_string_append_printf(text, "Audio: %s at %d kbit/s%s.", axis_target_codec_label(target->codec),
				target->audio_bitrate / 1000, target->audio_bitrate_mono > 0 ? " (less for mono)" : "");
		g_string_append(text, target->allow_lossy_source ? " Lossy sources are converted when they are "
			"well above that bitrate." : " Only lossless sources are converted: never lossy to lossy.");
		g_string_append(text, " Albums are converted whole; tags and cover art are carried over.");
	}
	g_string_append_printf(text, " A file (an album, for music) is skipped when it would save less than "
		"%.0f%% of its size.", MAX(target->min_saving, AXIS_ESTIMATE_MIN_SAVING_FLOOR) * 100.0);
	return g_string_free(text, FALSE);
}
