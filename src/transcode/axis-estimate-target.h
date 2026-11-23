/*
 * axis-estimate-target.h - target descriptions: what a file would be
 * converted to
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A target is a full combination: the video codec, its profile and bit
 * depth, the encoder family and quality, what happens to audio and
 * subtitle tracks, the container, and the rules that make a file not worth
 * converting.  Every target is built from a conversion profile's
 * definition by axis_profile_to_target() (axis-profile.h); the built-in
 * targets below are the built-in profiles' definitions (the families of
 * openspec/specs/conversion-profiles/spec.md: video Space saver, Balanced,
 * Maximum compatibility; audio Keep lossless, Space saver, Maximum
 * compatibility, Audiobooks), built through that same function.  The
 * client compatibility report judges each one as a combination.
 *
 * One field comes from a setting: the encoder family the workers have
 * (axis_estimate_target_configure(), within the profile's preference).
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_ESTIMATE_TARGET_H
#define AXIS_ESTIMATE_TARGET_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
	AXIS_TARGET_VIDEO,
	AXIS_TARGET_AUDIO
} AxisTargetMedia;

typedef enum
{
	AXIS_TCODEC_HEVC,
	AXIS_TCODEC_H264,
	AXIS_TCODEC_OPUS,
	AXIS_TCODEC_AAC,
	AXIS_TCODEC_MP3,
	AXIS_TCODEC_FLAC
} AxisTargetCodec;

#define AXIS_TCODEC_COUNT 6

/* "hevc", "h264", "opus", "aac", "mp3", "flac" (ffprobe's names) */
const gchar *
axis_target_codec_to_string(AxisTargetCodec codec);

gboolean
axis_target_codec_from_string(
	const gchar     *text,
	AxisTargetCodec *codec
);

/* "HEVC", "H.264", ... */
const gchar *
axis_target_codec_label(AxisTargetCodec codec);

typedef enum
{
	AXIS_ENCODER_SOFTWARE,	/* libx265, libx264, libopus, ... */
	AXIS_ENCODER_NVENC,	/* NVIDIA's hardware encoders */
	AXIS_ENCODER_VAAPI	/* VA-API (Intel, AMD) */
} AxisEncoderFamily;

#define AXIS_ENCODER_COUNT 3

/* "software", "nvenc", "vaapi" */
const gchar *
axis_encoder_family_to_string(AxisEncoderFamily family);

gboolean
axis_encoder_family_from_string(
	const gchar       *text,
	AxisEncoderFamily *family
);

const gchar *
axis_encoder_family_label(AxisEncoderFamily family);

/* Quality levels: 1 (smallest) to 7 (best).  For x265 level 4 is CRF 26,
 * each level two steps of that number; NVENC and VA-API have tables of
 * their own (priors, see axis-estimate-target.c: two and three steps
 * better than the CRF at each level); axis_estimate_quality_value() gives
 * the encoder's number. */
#define AXIS_QUALITY_MIN 1
#define AXIS_QUALITY_MAX 7

gint
axis_estimate_quality_value(
	AxisTargetCodec   codec,
	AxisEncoderFamily family,
	gint              level
);

/* The level of a video codec's quality number (x265 or x264 CRF); -1 for a
 * number the size model has no level for. */
gint
axis_estimate_quality_level(
	AxisTargetCodec codec,
	gint            crf
);

/**
 * AxisEstimateQualityRule:
 * @resolution: a static resolution class name ("2160p"), or %NULL for any
 * @min_bytes: 0 for none, else the file is at least this large
 * @max_bytes: 0 for none, else the file is smaller than this
 * @level: the quality level
 *
 * One condition of a quality rule: the first that matches a file gives its
 * level, else the target's own.
 */
typedef struct
{
	const gchar *resolution;
	gint64       min_bytes;
	gint64       max_bytes;
	gint         level;
} AxisEstimateQualityRule;

#define AXIS_ESTIMATE_MAX_QUALITY_RULES 8

/* Whether a rule's conditions hold for a file. */
gboolean
axis_estimate_rule_matches(
	const gchar *rule_resolution,
	gint64       min_bytes,
	gint64       max_bytes,
	const gchar *resolution_class,
	gint64       size
);

typedef enum
{
	AXIS_AUDIO_COPY,		/* every audio track copied */
	AXIS_AUDIO_COPY_ADD_STEREO,	/* copied, and a stereo AAC track added */
	AXIS_AUDIO_STEREO_FIRST,	/* a stereo AAC track first, the original kept after it */
	AXIS_AUDIO_CONVERT		/* audio targets: the track converted */
} AxisAudioHandling;

typedef enum
{
	AXIS_SUBTITLES_COPY,		/* every subtitle track copied (Matroska) */
	AXIS_SUBTITLES_PLAIN_TEXT,	/* plain text tracks to MP4 text; any other makes the file not eligible */
	AXIS_SUBTITLES_NONE		/* audio targets: there are none */
} AxisSubtitleHandling;

typedef enum
{
	AXIS_CONTAINER_MATROSKA,
	AXIS_CONTAINER_MP4,
	AXIS_CONTAINER_FLAC,
	AXIS_CONTAINER_OGG,
	AXIS_CONTAINER_MP3
} AxisTargetContainer;

/* "mkv", "mp4", "flac", "ogg", "mp3" */
const gchar *
axis_target_container_to_string(AxisTargetContainer container);

/* Root media types as a bit mask: 1 << AxisMediaType (axis-storage.h). */
#define AXIS_TARGET_ROOT(type) (1u << (type))

/**
 * AxisEstimateTarget:
 * @id: the profile's slug: lower-case letters, digits and '_'; in links
 * @name: "Space saver"
 * @family_name: "Video" or "Music" or "Audiobooks", for grouping
 * @summary: one sentence for the pages
 * @revision_id: the profile revision it was built from (conversion_profile_revision.id),
 *   0 for one built in code only
 * @revision: that revision's number within its profile, 0 likewise
 * @media: video or audio
 * @root_types: the libraries it is for (AXIS_TARGET_ROOT bits)
 * @codec: the output codec
 * @profile: the codec profile ("Main 10", "High"), for the compatibility
 *   report; %NULL for audio
 * @bit_depth: of the output picture; 0 for audio
 * @hdr_bit_depth: of the output picture for an HDR source (0: HDR sources
 *   are not converted, the target is SDR only)
 * @encoder: the encoder family the estimate assumes (the setting, within
 *   @encoders)
 * @encoders: (array length=n_encoders): the profile's preference
 * @quality: the quality level, AXIS_QUALITY_MIN to AXIS_QUALITY_MAX, for a
 *   file no rule matches
 * @quality_rules: (array length=n_quality_rules): the quality rule
 * @audio: what happens to audio tracks
 * @audio_bitrate: the bitrate of a converted or added track, bit/s
 * @audio_bitrate_mono: for a mono source (0: as @audio_bitrate)
 * @audio_quality: an encoder quality instead of a bitrate (MP3 V0 is 0),
 *   -1 for none
 * @keep_audio_languages: (nullable): audio tracks in other (known)
 *   languages are dropped; %NULL keeps every track
 * @subtitles: what happens to subtitle tracks
 * @container: the output container
 * @allow_lossy_source: audio: a lossy source may be converted (to a lower
 *   bitrate); off for music, on for audiobooks
 * @reduce_to_cd: lossless audio of more than 16 bits or 48 kHz goes to 16
 *   bits at 44.1 kHz (off in the built-in targets)
 * @min_saving: the smallest saving worth converting for, as a fraction of
 *   the file's (or album's) size; the decision never uses less than
 *   AXIS_ESTIMATE_MIN_SAVING_FLOOR whatever this says
 * @min_saving_bytes: and in bytes (0: none)
 * @efficient_milli: video: a source already in @codec at or below this
 *   many thousandths of a bit per pixel is already efficient; audio: a
 *   source in @codec at or below @audio_bitrate times this over 1000 is
 */
typedef struct
{
	const gchar             *id;
	const gchar             *name;
	const gchar             *family_name;
	const gchar             *summary;
	gint64                   revision_id;
	gint                     revision;
	AxisTargetMedia          media;
	guint                    root_types;
	AxisTargetCodec          codec;
	const gchar             *profile;
	gint                     bit_depth;
	gint                     hdr_bit_depth;
	AxisEncoderFamily        encoder;
	AxisEncoderFamily        encoders[AXIS_ENCODER_COUNT];
	guint                    n_encoders;
	gint                     quality;
	AxisEstimateQualityRule  quality_rules[AXIS_ESTIMATE_MAX_QUALITY_RULES];
	guint                    n_quality_rules;
	AxisAudioHandling     audio;
	gint                  audio_bitrate;
	gint                  audio_bitrate_mono;
	gint                  audio_quality;
	const gchar *const   *keep_audio_languages;
	AxisSubtitleHandling  subtitles;
	AxisTargetContainer   container;
	gboolean              allow_lossy_source;
	gboolean              reduce_to_cd;
	gdouble               min_saving;
	gint64                min_saving_bytes;
	gint64                efficient_milli;
} AxisEstimateTarget;

/* Whatever a target says, a file (or album) that would save less than this
 * share of its size is never converted. */
#define AXIS_ESTIMATE_MIN_SAVING_FLOOR 0.05

#define AXIS_ESTIMATE_N_TARGETS 7

/* The built-in targets, built from the built-in profiles' definitions in
 * code (axis-profile.h) with the default encoder, in page order. */
const AxisEstimateTarget *
axis_estimate_builtin_targets(void);

/* One of them by id, or %NULL. */
const AxisEstimateTarget *
axis_estimate_builtin_target(const gchar *id);

/* Their ids, ended by %NULL. */
const gchar *const *
axis_estimate_target_ids(void);

/**
 * AxisEstimateConfig:
 *
 * The setting that shapes the targets (see axis-estimator.h for its key):
 * the encoder family the workers have.
 */
typedef struct
{
	AxisEncoderFamily encoder;
} AxisEstimateConfig;

/* The default of the setting. */
void
axis_estimate_config_init(AxisEstimateConfig *config);

/* A copy of @builtin with @config applied: a video target assumes the
 * configured encoder when its profile lists it, else its first. */
void
axis_estimate_target_configure(
	const AxisEstimateTarget *builtin,
	const AxisEstimateConfig *config,
	AxisEstimateTarget       *target
);

/* The level a file of @size bytes at @resolution_class gets: the first
 * quality rule that matches, else the target's quality. */
gint
axis_estimate_target_quality_for(
	const AxisEstimateTarget *target,
	const gchar              *resolution_class,
	gint64                    size
);

/* What the target does, as a few sentences for its page (codec, depth,
 * encoder and quality, audio, subtitles, container). */
gchar *
axis_estimate_target_describe(const AxisEstimateTarget *target);

G_END_DECLS

#endif /* AXIS_ESTIMATE_TARGET_H */
