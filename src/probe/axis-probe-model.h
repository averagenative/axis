/*
 * axis-probe-model.h - what the probe knows about a file, and how it is
 * worked out from ffprobe's output
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * AxisMediaInfo is one media_info row with its media_stream rows
 * (data/migrations/0007_media_probe.sql, whose comment states the rules
 * that matter to whoever decides from it).  The functions here turn
 * ffprobe's JSON into one, and never trust it:
 *
 *  - every field is optional; a missing, malformed or absurd one is
 *    unknown (%AXIS_PROBE_UNKNOWN for numbers, %NULL for text, "unknown"
 *    for the three-valued flags), never a default that looks like a fact;
 *  - numbers are range-checked against the %AXIS_PROBE_MAX_* limits below;
 *    text is cut to a bounded length and cleaned (valid UTF-8, no control
 *    characters);
 *  - at most %AXIS_PROBE_MAX_STREAMS streams are kept (the rest are
 *    counted), so the model's size is bounded whatever the input;
 *  - HDR classification is conservative (axis-probe-hdr.c): a signal that
 *    is missing, conflicting, or looks like Dolby Vision or dynamic HDR
 *    metadata without matching the expected shape exactly makes the stream
 *    "uncertain" rather than classified by guess.
 *
 * Mapping runs in three steps, because the second ffprobe call (one frame
 * of the main video stream, for the side data that only frames carry) is
 * only needed for some streams:
 *
 *   axis_probe_map_streams()         the stream and format JSON
 *   axis_media_info_wants_frames()   whether, and for which stream
 *   axis_probe_map_frames()          the frame JSON, or none if it could
 *                                    not be had (then the streams that
 *                                    needed it are uncertain)
 *   axis_media_info_finish()         classification and derived facts
 *
 * axis_probe_map() does all four.  GLib only, no I/O: there is a fuzz
 * target (tests/fuzz/fuzz-probe-map.c).
 */

#ifndef AXIS_PROBE_MODEL_H
#define AXIS_PROBE_MODEL_H

#include <glib.h>

#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/* Bump when the mapping changes: files recorded with an older number are
 * probed again (at low priority).  Version 2 stores the HDR10 static
 * metadata values (AxisHdr10Metadata) and makes a stream whose values are
 * missing, out of range or contradictory uncertain; migration 0015 marks
 * every row that cannot be affected by that (no stream with mastering
 * display or content light data) as version 2, so that only those files
 * are probed again. */
#define AXIS_PROBE_VERSION        2

#define AXIS_PROBE_UNKNOWN        (-1)

#define AXIS_PROBE_MAX_STREAMS    512
#define AXIS_PROBE_MAX_FRAMES     8
#define AXIS_PROBE_MAX_CHAPTERS   1000000
#define AXIS_PROBE_MAX_DURATION_MS G_GINT64_CONSTANT(34560000000)	/* 400 days */
#define AXIS_PROBE_MAX_BITRATE    G_GINT64_CONSTANT(1000000000000)
#define AXIS_PROBE_MAX_DIMENSION  32768
#define AXIS_PROBE_MAX_CHANNELS   256
#define AXIS_PROBE_MAX_SAMPLE_RATE 100000000
#define AXIS_PROBE_MAX_FRAME_RATE 1000000.0

/* Longest text kept, in characters. */
#define AXIS_PROBE_CODEC_CHARS    64
#define AXIS_PROBE_NAME_CHARS     128
#define AXIS_PROBE_TAG_CHARS      256
#define AXIS_PROBE_NOTE_CHARS     200
#define AXIS_PROBE_ERROR_CHARS    500

typedef enum
{
	AXIS_STREAM_VIDEO,
	AXIS_STREAM_AUDIO,
	AXIS_STREAM_SUBTITLE,
	AXIS_STREAM_ATTACHMENT,
	AXIS_STREAM_DATA,
	AXIS_STREAM_UNKNOWN
} AxisStreamKind;

/* "video", "audio", "subtitle", "attachment", "data", "unknown" (stored) */
const gchar *
axis_stream_kind_to_string(AxisStreamKind kind);

gboolean
axis_stream_kind_from_string(
	const gchar    *text,
	AxisStreamKind *kind
);

/* In order of how restrictive they are for conversion.  %AXIS_HDR_NONE is
 * "not classified" (the stream is uncertain, or not a picture). */
typedef enum
{
	AXIS_HDR_NONE,
	AXIS_HDR_SDR,
	AXIS_HDR_HLG,
	AXIS_HDR_HDR10,
	AXIS_HDR_HDR10PLUS,
	AXIS_HDR_DOLBY_VISION
} AxisHdrFormat;

/* "sdr", "hlg", "hdr10", "hdr10plus", "dolby_vision" (stored); %NULL for
 * %AXIS_HDR_NONE */
const gchar *
axis_hdr_format_to_string(AxisHdrFormat format);

gboolean
axis_hdr_format_from_string(
	const gchar   *text,
	AxisHdrFormat *format
);

/* "SDR", "HLG", "HDR10", "HDR10+", "Dolby Vision"; "Not classified" */
const gchar *
axis_hdr_format_label(AxisHdrFormat format);

typedef enum
{
	AXIS_SUBTITLE_UNKNOWN,
	AXIS_SUBTITLE_TEXT,
	AXIS_SUBTITLE_IMAGE
} AxisSubtitleKind;

/* Three-valued flags. */
#define AXIS_TRI_UNKNOWN (-1)
#define AXIS_TRI_NO      0
#define AXIS_TRI_YES     1

/*
 * HDR10 static metadata, in the units the HEVC and AV1 bitstreams carry it
 * (SMPTE ST 2086 mastering display colour volume, CTA-861.3 content light
 * level): chromaticity coordinates in steps of 0.00002 (0 to 50000 is 0 to
 * 1), luminance in steps of 0.0001 cd/m2, light levels in cd/m2.  ffprobe
 * prints the values as rationals ("34000/50000" from a bitstream, "17/25"
 * from a Matroska container, which stores floats); they are rounded to
 * these units, which is what an encoder can be given back.
 *
 * The ranges a value must be in to be accepted (anything else makes the
 * stream uncertain): every chromaticity coordinate 0 to 50000; the maximum
 * luminance 5 to 10000 cd/m2 and the minimum 0 to 5 cd/m2 (the ranges
 * H.265 D.3.28 gives), the minimum below the maximum; each light level 0
 * to 65535 (16 bits; 0 is "not known"), and the frame-average level not
 * above the maximum when both are known.
 */
#define AXIS_HDR10_CHROMA_MAX            50000
#define AXIS_HDR10_MAX_LUMINANCE_LOWEST  G_GINT64_CONSTANT(50000)
#define AXIS_HDR10_MAX_LUMINANCE_HIGHEST G_GINT64_CONSTANT(100000000)
#define AXIS_HDR10_MIN_LUMINANCE_HIGHEST G_GINT64_CONSTANT(50000)
#define AXIS_HDR10_LIGHT_MAX             65535

/**
 * AxisHdr10Metadata:
 * @mastering: the mastering display values are known and valid
 * @red_x, @red_y, @green_x, @green_y, @blue_x, @blue_y, @white_x, @white_y:
 *   the display's primaries and white point, in 0.00002
 * @min_luminance, @max_luminance: its luminance, in 0.0001 cd/m2
 * @light: the content light levels are known and valid
 * @max_content: MaxCLL, cd/m2
 * @max_average: MaxFALL, cd/m2
 *
 * Members are 0 when their flag is not set.
 */
typedef struct
{
	gboolean mastering;
	gint     red_x;
	gint     red_y;
	gint     green_x;
	gint     green_y;
	gint     blue_x;
	gint     blue_y;
	gint     white_x;
	gint     white_y;
	gint64   min_luminance;
	gint64   max_luminance;
	gboolean light;
	gint     max_content;
	gint     max_average;
} AxisHdr10Metadata;

/* Whether two sets of values are the same (each flag, and the values it
 * covers). */
gboolean
axis_hdr10_metadata_equal(
	const AxisHdr10Metadata *a,
	const AxisHdr10Metadata *b
);

/* Whether the values that are flagged are in the ranges above. */
gboolean
axis_hdr10_metadata_valid(const AxisHdr10Metadata *metadata);

/**
 * AxisHdrSignals:
 *
 * What a video stream (and its first frames) said about HDR, before
 * anything is concluded from it.  The integers are %AXIS_PROBE_UNKNOWN
 * when absent or malformed.
 *
 * @has_mastering: mastering display metadata claiming a luminance (an
 *   entry of zeros is what a writer that had nothing puts there: not a
 *   claim)
 * @has_content_light: content light level metadata
 * @hdr10: the values of the first such entries that were complete and in
 *   range
 * @hdr10_bad: an entry was incomplete or out of range
 * @hdr10_conflict: two entries (the container's and a frame's, or two
 *   frames') gave different values
 * @mastering_in_frames, @light_in_frames: seen on a frame, that is in the
 *   bitstream itself (a decoder of the stream sees them), not only in the
 *   container
 * @dovi_record: a "DOVI configuration record" was present
 * @dovi_shape_bad: it was, but is not exactly the shape expected
 * @dovi_metadata: Dolby Vision metadata (RPU) was seen on a frame
 * @hdr10plus: "HDR Dynamic Metadata SMPTE2094-40 (HDR10+)" was present
 * @dynamic_other: side data whose name looks like dynamic HDR or Dolby
 *   Vision, but is none of the recognised ones
 */
typedef struct
{
	gboolean has_mastering;
	gboolean has_content_light;
	AxisHdr10Metadata hdr10;
	gboolean hdr10_bad;
	gboolean hdr10_conflict;
	gboolean mastering_in_frames;
	gboolean light_in_frames;
	gboolean dovi_record;
	gboolean dovi_shape_bad;
	gboolean dovi_metadata;
	gboolean hdr10plus;
	gboolean dynamic_other;
	gint     dv_version_major;
	gint     dv_version_minor;
	gint     dv_profile;
	gint     dv_level;
	gint     dv_rpu_present;
	gint     dv_el_present;
	gint     dv_bl_present;
	gint     dv_bl_compat_id;
} AxisHdrSignals;

/**
 * AxisMediaStream:
 *
 * One media_stream row.  Numbers are %AXIS_PROBE_UNKNOWN when unknown;
 * @frame_rate and @real_frame_rate are -1.0 then; the flags that can be
 * unknown are %AXIS_TRI_UNKNOWN.  The last members are the mapper's
 * working state and are not stored.
 */
typedef struct
{
	gint              index;
	AxisStreamKind    kind;
	gchar            *codec;
	gchar            *profile;
	gint64            duration_ms;
	gint64            bitrate;
	gboolean          bitrate_estimated;
	gchar            *language;
	gchar            *title;
	gboolean          is_default;
	gboolean          is_forced;
	gboolean          attached_pic;

	gint              width;
	gint              height;
	gchar            *pixel_format;
	gint              bit_depth;
	gdouble           frame_rate;
	gdouble           real_frame_rate;
	gchar            *field_order;
	gchar            *color_range;
	gchar            *color_space;
	gchar            *color_transfer;
	gchar            *color_primaries;
	AxisHdrFormat     hdr_format;
	gboolean          hdr_uncertain;
	const gchar      *hdr_note;	/* a fixed sentence, or %NULL */
	/* HDR10 static metadata (stored): the values the stream carries, set
	 * by the classification only when every entry read was complete, in
	 * range and the same as the others; cleared otherwise. */
	AxisHdr10Metadata hdr10;
	gint              dv_profile;
	gint              dv_level;
	gint              dv_bl_compat_id;
	gint              dv_hdr10_compatible;	/* tri */

	gint              channels;
	gchar            *channel_layout;
	gint              sample_rate;
	gint              bit_depth_audio;
	gint              lossless;	/* tri */

	AxisSubtitleKind  subtitle_kind;

	/* Working state. */
	AxisHdrSignals    signals;
	gboolean          frame_color_conflict;
	gint64            tag_bitrate;	/* a bitrate tag some muxers write, or -1 */
	gchar            *sample_format;
} AxisMediaStream;

void
axis_media_stream_free(AxisMediaStream *stream);

/**
 * AxisMediaInfo:
 *
 * One media_info row and its streams.  @status, @error, @probed_at_us,
 * @probe_ms, @tool_version and the file's stamp are filled by whoever
 * stores or loads it, not by the mapper.
 */
typedef struct
{
	gint64       file_id;
	gchar       *status;	/* "ok", "failed" or "timeout" */
	gchar       *error;
	gint         probe_version;
	gint64       probed_at_us;
	gint         probe_ms;
	gchar       *tool_version;

	gint64       file_size;
	gint64       mtime_sec;
	gint         mtime_nsec;
	gint64       ctime_sec;
	gint         ctime_nsec;
	gint64       inode;

	gchar       *container;
	gint64       duration_ms;
	gint64       bitrate;
	gboolean     bitrate_estimated;
	gint         chapter_count;
	gint         stream_count;
	gint         streams_omitted;
	gint         video_streams;
	gint         audio_streams;
	gint         subtitle_streams;
	GPtrArray   *streams;	/* (element-type AxisMediaStream) */

	gint         main_video;	/* index into @streams, or -1 */
	gint         main_audio;
	gchar       *video_codec;
	gint         width;
	gint         height;
	const gchar *resolution_class;
	gint         bit_depth;
	gdouble      frame_rate;
	AxisHdrFormat hdr_format;
	gboolean     hdr_uncertain;
	const gchar *hdr_note;
	gdouble      bits_per_pixel;	/* -1.0 when unknown */
	gboolean     bits_per_pixel_estimated;
	gint         interlaced;	/* tri */
	gint         vfr_suspected;	/* tri */

	gchar       *audio_codec;
	gint         audio_channels;
	gint         audio_sample_rate;
	gint         audio_bit_depth;
	gint         audio_lossless;	/* tri */
	gint64       audio_bitrate_total;
	gboolean     audio_bitrate_total_estimated;

	gchar       *tag_title;
	gchar       *tag_artist;
	gchar       *tag_album;
	gboolean     has_track;
	gboolean     has_disc;
	gboolean     has_musicbrainz;
	gboolean     has_replaygain;
	gboolean     has_embedded_art;
	gint         art_width;
	gint         art_height;
	gchar       *album_key;

	/* Working state. */
	gboolean     frames_unavailable;
	gchar       *hdr_note_owned;	/* what @hdr_note points to, for a loaded row */
} AxisMediaInfo;

AxisMediaInfo *
axis_media_info_new(void);

void
axis_media_info_free(AxisMediaInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMediaInfo, axis_media_info_free)

/* ── Mapping ────────────────────────────────────────────────────── */

/**
 * axis_probe_map_streams:
 * @json: the output of `ffprobe -print_format json -show_format
 *   -show_streams -show_chapters -show_error`
 * @length: its length in bytes
 * @info: (out) (transfer full): the streams and container, not yet
 *   classified
 * @error: (out) (transfer full): when %FALSE, why, as a sentence safe to
 *   store, show and log (the file's own text is cleaned and cut in it)
 *
 * Fails for output that is not JSON within axis-probe-json.h's limits, for
 * ffprobe's own error report, and for output that has no container or no
 * stream.  Never fails for a field that is merely odd.
 *
 * Returns: %TRUE if there is something to build on
 */
gboolean
axis_probe_map_streams(
	const gchar    *json,
	gsize           length,
	AxisMediaInfo **info,
	gchar         **error
);

/**
 * axis_media_info_wants_frames:
 * @index: (out) (optional): the stream index to read a frame of
 *
 * Whether the second call is needed: the main video stream's colour
 * signalling or bit depth leaves room for HDR or Dolby Vision that only
 * frame side data would show.  A plain 8-bit stream signalled as SDR (or
 * not signalled at all, without wide-gamut primaries) does not.
 */
gboolean
axis_media_info_wants_frames(
	const AxisMediaInfo *info,
	gint                *index
);

/**
 * axis_probe_map_frames:
 * @info: the model
 * @json: the output of the frame call (`-show_frames -read_intervals %+#1`
 *   with `-show_entries frame=...side_data_list`), or %NULL when it could
 *   not be had
 * @length: its length
 *
 * Merges the side data and colour fields of the first frames into the main
 * video stream.  Unparseable output counts as none.
 */
void
axis_probe_map_frames(
	AxisMediaInfo *info,
	const gchar   *json,
	gsize          length
);

/**
 * axis_media_info_finish:
 * @info: the model
 * @file_size: the file's size, for the container bitrate when the
 *   container has none; or %AXIS_PROBE_UNKNOWN
 *
 * Classifies every video stream, estimates the bitrates that are missing,
 * picks the main streams and computes the derived facts.  Call once, last.
 */
void
axis_media_info_finish(
	AxisMediaInfo *info,
	gint64         file_size
);

/* All four steps; @frames_json may be %NULL. */
gboolean
axis_probe_map(
	const gchar    *streams_json,
	gsize           streams_length,
	const gchar    *frames_json,
	gsize           frames_length,
	gint64          file_size,
	AxisMediaInfo **info,
	gchar         **error
);

/* ── Derived facts (axis-probe-model.c) ─────────────────────────── */

/* "sd", "720p", "1080p", "2160p" or "other"; %NULL when the size is not
 * known */
const gchar *
axis_probe_resolution_class(
	gint width,
	gint height
);

/* "SD", "720p", ... for pages */
const gchar *
axis_probe_resolution_label(const gchar *resolution_class);

/* Whether an audio codec keeps every sample: %AXIS_TRI_YES for FLAC, ALAC,
 * PCM, TrueHD, DTS-HD MA ...; %AXIS_TRI_NO for MP3, AAC, Opus, AC-3, DTS
 * core ...; %AXIS_TRI_UNKNOWN for a codec not in the table (and for those
 * that come in both kinds, like WavPack). */
gint
axis_probe_audio_lossless(
	const gchar *codec,
	const gchar *profile
);

AxisSubtitleKind
axis_probe_subtitle_kind(const gchar *codec);

/* Pixel format "yuv420p10le" -> 10, "yuv420p" -> 8; %AXIS_PROBE_UNKNOWN
 * when it cannot be told. */
gint
axis_probe_pixel_format_depth(const gchar *pixel_format);

/* The containers and bare streams a media tool may be asked to read
 * (ffmpeg's and ffprobe's -format_whitelist): none that names other files
 * or addresses, or runs other software.  The probe, the sample measurement
 * and conversions use it. */
const gchar *
axis_probe_format_whitelist(void);

/* What ffprobe may write (per call) and use. */
#define AXIS_PROBE_OUTPUT_CAP          (4 * 1024 * 1024)
#define AXIS_PROBE_STDERR_CAP          (64 * 1024)
#define AXIS_PROBE_MEMORY_LIMIT        (G_GUINT64_CONSTANT(4) * 1024 * 1024 * 1024)

/**
 * axis_probe_tool_argv:
 * @prefix: (nullable): the program and any arguments before the probe's
 *   own, ended by %NULL
 * @frames: the call that reads one frame of stream @stream_index, rather
 *   than the one that lists the container and streams
 *
 * The probe's two ffprobe calls, the only form in which ffprobe is run:
 * the file is descriptor 3 (`-protocol_whitelist fd -fd 3 -i fd:`), never
 * a name.  tests/fixtures/probe/record.sh records with the same arguments.
 *
 * Returns: (transfer full): the command, ended by %NULL
 */
gchar **
axis_probe_tool_argv(
	const gchar *const *prefix,
	gboolean            frames,
	gint                stream_index
);

/* ── HDR (axis-probe-hdr.c) ─────────────────────────────────────── */

/* Fills @signals from a "side_data_list" array (adds to what is there):
 * a stream's own list (the container's). */
void
axis_probe_hdr_read_side_data(
	const AxisJsonNode *side_data_list,
	AxisHdrSignals     *signals
);

/* The same for a list that is a frame's (@from_frame) or a stream's. */
void
axis_probe_hdr_read_side_data_from(
	const AxisJsonNode *side_data_list,
	AxisHdrSignals     *signals,
	gboolean            from_frame
);

void
axis_hdr_signals_init(AxisHdrSignals *signals);

/* Whether the signals warrant a look at a frame. */
gboolean
axis_probe_hdr_wants_frames(const AxisMediaStream *stream);

/**
 * axis_probe_hdr_classify:
 * @stream: a video stream that is not a cover picture, with its colour
 *   fields, bit depth, codec and signals filled
 * @frames_read: whether frame side data was read for it; if it was wanted
 *   and not read, the stream is uncertain
 *
 * Sets hdr_format, hdr_uncertain, hdr_note and the Dolby Vision fields.
 * The rules are in the file's header comment; the short form: only
 * positive, consistent evidence classifies; anything else is uncertain.
 */
void
axis_probe_hdr_classify(
	AxisMediaStream *stream,
	gboolean         frames_read
);

G_END_DECLS

#endif /* AXIS_PROBE_MODEL_H */
