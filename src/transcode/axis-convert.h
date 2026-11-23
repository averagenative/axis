/*
 * axis-convert.h - the arguments of a video conversion: the encoder table,
 * the plan (ffmpeg's argument vector and the output it must produce), the
 * check of an output against its plan, the encoder capabilities, and
 * ffmpeg's progress
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A later change converts files with these arguments and then deletes the
 * originals, so every promise here is about not losing anything silently:
 *
 *  - The plan is built from data alone (a profile definition, the probe's
 *    record of the file, the chosen encoder family, that machine's measured
 *    capabilities) by a pure function: no file is read, nothing is run.
 *    When a fact it needs is missing, or anything is in doubt, there is no
 *    plan, and the reason is one of #AxisConvertRefusal.
 *  - Nothing from the library reaches the argument vector: no file name
 *    (the builder is never given one), no path, no tag, title, language or
 *    other text of the file.  Every argument is a fixed string of the
 *    tables in axis-convert-plan.c or a number formatted there after a
 *    range check; axis_convert_args_valid() is a second, independent
 *    reading of that grammar, which the builder applies to its own result.
 *    Stream titles, languages, attachments' names and the file's tags reach
 *    the output only as ffmpeg copies them (as data, never as arguments).
 *  - ffmpeg reads descriptor 3 and writes descriptor 4 (`-fd 3 -i fd:`,
 *    `-fd 4 fd:`), both with only the `fd` protocol allowed: it is never
 *    told where anything is.
 *  - Streams are mapped one by one (`-map 0:<index>`), in a fixed order:
 *    the picture, audio, subtitles, then attachments and cover pictures.
 *    Every stream of the file is either mapped or listed as dropped with
 *    its reason; a stream the plan can neither carry nor drop by a rule of
 *    the profile means no plan.
 *  - Every option that applies to one stream names it by its output index;
 *    default and forced are set on every output stream (relative to the
 *    stream's other flags, which ffmpeg copies).
 *  - Colour is stated where the source states it, on the frames
 *    (`setparams`, which reaches the bitstream and the container) and in
 *    the encoder's own parameters; where the source does not state a
 *    property it is left unstated and the plan says so: never guessed.
 *    (A range left unstated is limited by definition, and ffmpeg and the
 *    encoders may write it out as limited: the same picture.)
 *    HDR10's static metadata values are given back to the encoder exactly.
 *  - The plan describes the output it must produce, stream by stream
 *    (#AxisConvertStream); axis_convert_plan_check_output() compares a
 *    probe of an output with it.
 *
 * design.md of the add-ffmpeg-arguments change records what was measured
 * on ffmpeg 7.1 and 8.1 to choose each argument.
 *
 * GLib only, no I/O, except the capability check (axis-convert-caps.c),
 * which runs ffmpeg and ffprobe, and its store (the database).  There are
 * fuzz targets (tests/fuzz/fuzz-convert-plan.c, fuzz-convert-progress.c).
 */

#ifndef AXIS_CONVERT_H
#define AXIS_CONVERT_H

#include <glib.h>

#include "probe/axis-probe-model.h"
#include "transcode/axis-estimate-target.h"
#include "transcode/axis-profile.h"

G_BEGIN_DECLS

/* ── The encoder table ──────────────────────────────────────────── */

#define AXIS_CONVERT_N_ENCODERS 6

/**
 * AxisConvertEncoder:
 * @name: ffmpeg's encoder ("libx265")
 * @codec: what it makes
 * @family: software, NVENC or VA-API
 * @ten_bit: whether the table asks it for 10 bits at all (HEVC only)
 * @quality_option: how quality is given ("-crf", "-cq", "-qp")
 * @quality_label: the same for people ("CRF", "CQ", "QP")
 * @hdr10_stated: the arguments state HDR10's static metadata values
 *   themselves; otherwise only ffmpeg's passing on of the source's side
 *   data can carry them, and only the capability check can show it does
 * @hdr10: how HDR10's static metadata reaches the output, in words
 * @colour: how colour reaches the output, in words
 * @tested: whether this encoder has produced a file in Axis's real-encode
 *   tests: all six have, the hardware ones only with ffmpeg 8.1 on the
 *   development machine's GPUs (NVENC on an NVIDIA card, VA-API on the
 *   Intel render node) under `make check-hwenc` (`make check` leaves the
 *   GPU alone); the container image's ffmpeg 7.1 has had no device
 */
typedef struct
{
	const gchar       *name;
	AxisTargetCodec    codec;
	AxisEncoderFamily  family;
	gboolean           ten_bit;
	const gchar       *quality_option;
	const gchar       *quality_label;
	gboolean           hdr10_stated;
	const gchar       *hdr10;
	const gchar       *colour;
	gboolean           tested;
} AxisConvertEncoder;

/* The table, AXIS_CONVERT_N_ENCODERS rows: libx265, libx264, hevc_nvenc,
 * h264_nvenc, hevc_vaapi, h264_vaapi. */
const AxisConvertEncoder *
axis_convert_encoders(void);

/* The row for a video codec and family, or %NULL. */
const AxisConvertEncoder *
axis_convert_encoder_for(
	AxisTargetCodec   codec,
	AxisEncoderFamily family
);

/* The row of an encoder by ffmpeg's name, or %NULL. */
const AxisConvertEncoder *
axis_convert_encoder_named(const gchar *name);

/* Its position in the table (0 to AXIS_CONVERT_N_ENCODERS - 1). */
guint
axis_convert_encoder_index(const AxisConvertEncoder *encoder);

/* ── Capabilities ───────────────────────────────────────────────── */

/**
 * AxisEncoderCapability:
 * @measured: a measurement of this encoder exists
 * @usable: an 8-bit test encode came out exactly as its plan said
 * @ten_bit: a 10-bit one did too (HLG-signalled: transfer and primaries
 *   carried); HEVC only
 * @hdr10: an HDR10 one came back with the same static metadata values, in
 *   the bitstream
 * @reason: (nullable): when not all of them, why (a sentence; the tool's
 *   own words in it are cleaned and cut)
 */
typedef struct
{
	gboolean  measured;
	gboolean  usable;
	gboolean  ten_bit;
	gboolean  hdr10;
	gchar    *reason;
} AxisEncoderCapability;

/**
 * AxisEncoderCapabilities:
 *
 * What one ffmpeg build on one machine was shown to do.  @tool_digest is
 * the SHA-256 (hex) of everything `ffmpeg -version` printed; capabilities
 * are only ever used with the build they were measured for.
 */
typedef struct
{
	gchar                 *tool_version;	/* "8.1.3" */
	gchar                 *tool_digest;
	gint64                 measured_at_us;
	AxisEncoderCapability  encoders[AXIS_CONVERT_N_ENCODERS];
	/* The encoders the plans use beside the picture, as listed by
	 * `ffmpeg -encoders`. */
	gboolean               aac;
	gboolean               mov_text;
	gboolean               subrip;
} AxisEncoderCapabilities;

AxisEncoderCapabilities *
axis_encoder_capabilities_new(void);

void
axis_encoder_capabilities_free(AxisEncoderCapabilities *caps);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisEncoderCapabilities, axis_encoder_capabilities_free)

AxisEncoderCapabilities *
axis_encoder_capabilities_copy(const AxisEncoderCapabilities *caps);

/* The capability of @encoder. */
const AxisEncoderCapability *
axis_encoder_capabilities_get(
	const AxisEncoderCapabilities *caps,
	const AxisConvertEncoder      *encoder
);

/* Sets @encoder's capability (copies @reason). */
void
axis_encoder_capabilities_set(
	AxisEncoderCapabilities *caps,
	const AxisConvertEncoder *encoder,
	gboolean                 usable,
	gboolean                 ten_bit,
	gboolean                 hdr10,
	const gchar             *reason
);

/* The version word of `ffmpeg -version` ("8.1.3", "7.1.5-0+deb13u1"), or
 * %NULL when the text is not that output. */
gchar *
axis_convert_parse_version(
	const gchar *text,
	gsize        length
);

/* Whether `ffmpeg -encoders` (@text) lists an encoder of that name. */
gboolean
axis_convert_encoders_lists(
	const gchar *text,
	gsize        length,
	const gchar *name
);

/* ── Refusals ───────────────────────────────────────────────────── */

/**
 * AxisConvertRefusal:
 *
 * Why there is no plan: the first that applies.
 */
typedef enum
{
	AXIS_CONVERT_OK = 0,
	AXIS_CONVERT_REFUSE_NOT_VIDEO,	/* not a video profile (audio conversion is a later change) */
	AXIS_CONVERT_REFUSE_DEFINITION,	/* the definition does not pass its check */
	AXIS_CONVERT_REFUSE_OPTIONS,	/* a preset, device or thread count not in the table */
	AXIS_CONVERT_REFUSE_NOT_PROBED,	/* no usable probe record, streams left out of it, or two
					 * records of one stream index */
	AXIS_CONVERT_REFUSE_UNKNOWN_STREAM,	/* a data stream or a stream of unknown kind */
	AXIS_CONVERT_REFUSE_PICTURE,	/* not exactly one picture, or its codec or size unknown */
	AXIS_CONVERT_REFUSE_HDR_UNCERTAIN,
	AXIS_CONVERT_REFUSE_DOLBY_VISION,
	AXIS_CONVERT_REFUSE_HDR10PLUS,
	AXIS_CONVERT_REFUSE_HDR_TO_SDR,
	AXIS_CONVERT_REFUSE_HDR10_METADATA,	/* HDR10 without valid static metadata values */
	AXIS_CONVERT_REFUSE_INTERLACED,	/* not known to be progressive */
	AXIS_CONVERT_REFUSE_PIXEL_FORMAT,	/* not 4:2:0, or more than 12 bits, or not known */
	AXIS_CONVERT_REFUSE_COLOUR,	/* a colour property the encoders have no name for */
	AXIS_CONVERT_REFUSE_COVER_IN_MATROSKA,	/* ffmpeg writes a cover into Matroska as a video track */
	AXIS_CONVERT_REFUSE_COVER_CODEC,	/* a cover picture MP4 cannot hold */
	AXIS_CONVERT_REFUSE_ATTACHMENT_IN_MP4,
	AXIS_CONVERT_REFUSE_SUBTITLE,	/* a subtitle track the target cannot carry */
	AXIS_CONVERT_REFUSE_AUDIO,	/* an audio track the container cannot hold */
	AXIS_CONVERT_REFUSE_NO_AUDIO_LEFT,	/* the language rule would leave the film without sound */
	AXIS_CONVERT_REFUSE_ENCODER_NOT_IN_PROFILE,
	AXIS_CONVERT_REFUSE_NOT_MEASURED,	/* no capabilities for this machine's ffmpeg */
	AXIS_CONVERT_REFUSE_NOT_CAPABLE,	/* the encoder was not shown able to produce this output */
	AXIS_CONVERT_REFUSE_HELPER_ENCODER,	/* AAC, mov_text or SubRip is not there */
	AXIS_CONVERT_REFUSE_TOO_MANY_STREAMS,
	AXIS_CONVERT_REFUSE_INTERNAL,	/* the builder's own vector failed its check: a bug */
	AXIS_CONVERT_N_REFUSALS
} AxisConvertRefusal;

/* "not_video", ... */
const gchar *
axis_convert_refusal_to_string(AxisConvertRefusal refusal);

/* A sentence for people. */
const gchar *
axis_convert_refusal_describe(AxisConvertRefusal refusal);

/* ── The plan ───────────────────────────────────────────────────── */

typedef enum
{
	AXIS_CONVERT_COPY,	/* copied as it is */
	AXIS_CONVERT_ENCODE,	/* the picture, encoded anew */
	AXIS_CONVERT_STEREO,	/* a stereo AAC track made from the source's track */
	AXIS_CONVERT_TO_TEXT,	/* a text subtitle in the container's own text format */
	AXIS_CONVERT_MUXER	/* not mapped: made by the muxer (MP4's chapter track) */
} AxisConvertAction;

/* "copy", "encode", "stereo", "to_text", "muxer" */
const gchar *
axis_convert_action_to_string(AxisConvertAction action);

/**
 * AxisConvertStream:
 * @position: its index in the output
 * @source: the source stream's index (ffprobe's), or -1 for a stream the
 *   muxer makes
 * @kind: what it is
 * @action: what is done to make it
 * @codec: (nullable): the codec the output stream must have (ffprobe's
 *   name); %NULL where the source had none (an attachment)
 * @is_default, @is_forced, @attached_pic: its flags in the output
 * @channels: audio: how many, or -1 when not checked
 * @language: (nullable): what the output must say (copied by ffmpeg from
 *   the source: data, never part of the argument vector)
 * @title: (nullable): likewise
 * @check_title: whether the container keeps a track title (MP4 does not)
 * @width, @height, @bit_depth: the picture's (encoded picture only)
 * @color_range, @color_space, @color_transfer, @color_primaries: what the
 *   picture must state (static strings, ffprobe's names), %NULL for a
 *   property the source leaves unstated, which the output must leave
 *   unstated too; a range left unstated is limited, which the output may
 *   state (axis_convert_colour_same())
 * @hdr_format: %AXIS_HDR_SDR, %AXIS_HDR_HLG or %AXIS_HDR_HDR10
 * @hdr10: the static metadata values the output's bitstream must carry,
 *   exactly (flags off: it must carry none)
 */
typedef struct
{
	guint              position;
	gint               source;
	AxisStreamKind     kind;
	AxisConvertAction  action;
	gchar             *codec;
	gboolean           is_default;
	gboolean           is_forced;
	gboolean           attached_pic;
	gint               channels;
	gchar             *language;
	gchar             *title;
	gboolean           check_title;
	gint               width;
	gint               height;
	gint               bit_depth;
	const gchar       *color_range;
	const gchar       *color_space;
	const gchar       *color_transfer;
	const gchar       *color_primaries;
	AxisHdrFormat      hdr_format;
	AxisHdr10Metadata  hdr10;
} AxisConvertStream;

/**
 * AxisConvertDropped:
 * @source: the source stream's index
 * @kind: what it was
 * @reason: a fixed sentence
 */
typedef struct
{
	gint            source;
	AxisStreamKind  kind;
	const gchar    *reason;
} AxisConvertDropped;

/**
 * AxisConvertPlan:
 * @args: ffmpeg's arguments without the program, ended by %NULL; the
 *   source is descriptor 3, the output descriptor 4, the progress
 *   (`-progress pipe:1`) standard output
 * @encoder: the encoder's row
 * @container: the output container
 * @crf: the profile's quality number for this file (its quality rule)
 * @level: that number's quality level
 * @quality: the encoder's own number for the level (CRF, CQ or QP)
 * @bit_depth: of the output picture
 * @hdr_format: of the output picture
 * @streams: (element-type AxisConvertStream): the output, in order
 * @dropped: (element-type AxisConvertDropped): the source's streams that
 *   are not carried, each with its reason
 * @notes: (element-type utf8): fixed sentences about what the output
 *   leaves unstated or what its container does by itself
 * @chapters: how many chapters the output must have, -1 when not known
 * @duration_ms: the source's duration (progress), -1 when not known
 * @frame_rate: the picture's average rate (progress), -1 when not known
 * @tool_digest: the ffmpeg build the capabilities were measured for: the
 *   plan is for that build only
 */
typedef struct
{
	gchar                    **args;
	const AxisConvertEncoder  *encoder;
	AxisTargetContainer        container;
	gint                       crf;
	gint                       level;
	gint                       quality;
	gint                       bit_depth;
	AxisHdrFormat              hdr_format;
	GPtrArray                 *streams;
	GPtrArray                 *dropped;
	GPtrArray                 *notes;
	gint                       chapters;
	gint64                     duration_ms;
	gdouble                    frame_rate;
	gchar                     *tool_digest;
} AxisConvertPlan;

void
axis_convert_plan_free(AxisConvertPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisConvertPlan, axis_convert_plan_free)

/**
 * AxisConvertOptions:
 * @software_preset: (nullable): x264's and x265's preset, one of
 *   "ultrafast" ... "veryslow" (%NULL: "medium")
 * @nvenc_preset: (nullable): "p1" ... "p7" (%NULL: "p5")
 * @vaapi_device: (nullable): /dev/dri/renderD followed by three digits
 *   (%NULL: /dev/dri/renderD128)
 * @threads: the software encoders' threads, 1 to 64; 0 is theirs to
 *   choose
 *
 * Settings of the machine that encodes.  Anything not in the table is
 * refused (%AXIS_CONVERT_REFUSE_OPTIONS), never passed on.
 */
typedef struct
{
	const gchar *software_preset;
	const gchar *nvenc_preset;
	const gchar *vaapi_device;
	guint        threads;
} AxisConvertOptions;

/**
 * axis_convert_plan_build:
 * @def: a profile definition
 * @info: the probe's record of the file, with its streams
 * @file_size: the file's size (its quality rule)
 * @family: the encoder family to use
 * @caps: (nullable): the machine's measured capabilities; %NULL refuses
 * @options: (nullable): the machine's settings (%NULL: the defaults)
 * @plan: (out) (transfer full) (optional): the plan, or %NULL
 * @refusal: (out) (optional): why not, %AXIS_CONVERT_OK with a plan
 *
 * Pure.  Returns: %TRUE with a plan
 */
gboolean
axis_convert_plan_build(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         file_size,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps,
	const AxisConvertOptions      *options,
	AxisConvertPlan              **plan,
	AxisConvertRefusal            *refusal
);

/**
 * axis_convert_plan_choose:
 * @refusals: (out caller-allocates) (array fixed-size=3) (optional): the
 *   refusal of each family of the profile's preference that was tried, in
 *   that order (%AXIS_CONVERT_OK where none was tried)
 *
 * The profile's encoder families in order of preference: the plan of the
 * first that can make one.  An encoder the machine cannot use (or that was
 * not shown to carry what the file needs, such as HDR10) is passed over for
 * the next.  @refusal is the first family's refusal when none can.
 */
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
);

/**
 * axis_convert_args_valid:
 * @args: an argument vector, ended by %NULL
 * @info: the file it is for
 * @problem: (out) (optional) (transfer full): what is wrong
 *
 * Whether @args is one the builder can make: every argument from the
 * builder's alphabet, never empty; an option the builder uses followed by
 * a value of the form it gives that option; every `-map` naming a stream
 * of @info; every per-stream option naming an output stream that exists;
 * the input and the output the two descriptors.  Written separately from
 * the builder, as a check of it.
 */
gboolean
axis_convert_args_valid(
	const gchar *const  *args,
	const AxisMediaInfo *info,
	gchar              **problem
);

/**
 * axis_convert_plan_check_output:
 * @plan: a plan
 * @output: a probe of the file the plan's arguments produced, made with
 *   its frames read (axis_probe_map(): the check looks at what the
 *   bitstream itself carries)
 * @mismatches: (nullable) (element-type utf8): a sentence per difference
 *   is appended
 *
 * Whether the output is what the plan says, stream by stream: kinds,
 * order, codecs, flags, channels, languages and titles, the picture's
 * size, depth, colour and HDR (static metadata exactly, in the bitstream),
 * and the chapters.  Extra or missing streams are differences.  Colour and
 * the HDR class are compared by what they mean, by the same rules as
 * axis_convert_plan_check_picture().
 */
gboolean
axis_convert_plan_check_output(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *output,
	GPtrArray             *mismatches
);

/**
 * axis_convert_plan_check_picture:
 * @plan: a plan
 * @output: a probe of its output, made with its frames read
 * @why: (out) (optional) (transfer full): what differs, one sentence per
 *   difference naming the property, what the plan says and what the
 *   output has; %NULL when nothing does
 *
 * Whether the output's picture is the one the plan encodes (check 4 of a
 * conversion): an encoded picture at the planned position, of the planned
 * size and bit depth; each colour property meaning what the plan's means
 * (axis_convert_colour_same()); of the planned HDR class; and with the
 * planned HDR10 values exactly.  The HDR class is the plan's for HDR10
 * and HLG (and not uncertain); for SDR it is SDR, or "not classified" in
 * the one case where that is all the probe can say of an SDR picture:
 * deeper than 8 bits, no transfer function stated (where the plan states
 * none either), not BT.2020, its frames read, and no HDR or Dolby Vision
 * signal of any kind.  Whether HDR10's values are in the bitstream itself
 * is axis_convert_check_bitstream()'s to say.
 */
gboolean
axis_convert_plan_check_picture(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *output,
	gchar                **why
);

/**
 * axis_convert_colour_same:
 * @property: "primaries", "transfer", "matrix" or "range"
 * @planned: (nullable): the plan's value (%NULL: the source states none)
 * @got: (nullable): the output's, as ffprobe names it
 *
 * Whether the output's colour property means what the plan's does.  Not
 * stated is %NULL, empty, "unknown", "unspecified" or "reserved".  The
 * primaries, the transfer function and the matrix must be the plan's where
 * it states one and not stated where it does not.  A range that is not
 * stated is limited: H.264 and H.265 infer video_full_range_flag 0 when
 * the bitstream leaves it out (Annex E of both), and Matroska's Range and
 * MP4's colour box are read the same way, so limited ("tv") and not stated
 * are one range, either standing for the other; full ("pc") is the same
 * only as full.
 */
gboolean
axis_convert_colour_same(
	const gchar *property,
	const gchar *planned,
	const gchar *got
);

/**
 * axis_convert_colour_said:
 * @property: as for axis_convert_colour_same()
 * @value: (nullable): a value of it
 *
 * How a check's sentence says @value: a range as "limited (tv)", "full
 * (pc)" or "unstated (limited)"; the others as ffprobe names them, or
 * "unstated".  Returns a static string, or @value itself.
 */
const gchar *
axis_convert_colour_said(
	const gchar *property,
	const gchar *value
);

/* The colour names the plans can state (ffprobe's), ended by %NULL: for
 * "primaries", "transfer", "matrix" or "range". */
const gchar *const *
axis_convert_colour_names(const gchar *property);

/* ── Progress ───────────────────────────────────────────────────── */

/* A line of `-progress` output longer than this is ignored whole. */
#define AXIS_CONVERT_PROGRESS_LINE_MAX 256

/**
 * AxisConvertProgress:
 *
 * What ffmpeg's `-progress` output said so far.  Every value is optional:
 * -1 (-1.0) is "not said, or not readable".  Values are taken at the end
 * of each block (the "progress=" line).  @out_time_us is the largest seen
 * (ffmpeg's value can go back, or be "N/A", when a sparse stream such as a
 * subtitle is mapped: measured on 7.1 and 8.1).
 */
typedef struct
{
	gint64   expected_frames;
	gint64   duration_us;
	gint64   frame;
	gint64   out_time_us;
	gint64   total_size;
	gdouble  speed;
	gboolean ended;
	guint    blocks;
	/* The block being read. */
	gint64   pending_frame;
	gint64   pending_time_us;
	gint64   pending_size;
	gdouble  pending_speed;
	gchar    line[AXIS_CONVERT_PROGRESS_LINE_MAX + 1];
	gsize    line_length;
	gboolean line_too_long;
} AxisConvertProgress;

/* @duration_ms and @frame_rate of the source, either unknown (-1). */
void
axis_convert_progress_init(
	AxisConvertProgress *progress,
	gint64               duration_ms,
	gdouble              frame_rate
);

/* Reads more of the output (any split, any bytes). */
void
axis_convert_progress_feed(
	AxisConvertProgress *progress,
	const gchar         *data,
	gsize                length
);

/**
 * axis_convert_progress_fraction:
 * @fraction: (out): 0 to 1
 *
 * How far it is: frames written against the frames expected when both are
 * known, else the output time against the duration.  FALSE (and nothing
 * divided) when neither can be worked out.
 */
gboolean
axis_convert_progress_fraction(
	const AxisConvertProgress *progress,
	gdouble                   *fraction
);

G_END_DECLS

#endif /* AXIS_CONVERT_H */
