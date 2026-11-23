/*
 * axis-compat-evidence.h - what one observed session proves about the
 * device, stream by stream
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The media server states what it did to each stream (played it as it is,
 * passed it through, converted it).  Plex never says why, so the reason is
 * inferred as described below; Jellyfin states reasons, and where a session
 * has any they are the evidence instead (axis-compat-reasons.h: a stated
 * reason decides which stream it concerns, whether it proves the device
 * did not take the codec, a limit, or nothing clear, and the property it
 * blames if not the codec).  This turns one session into evidence,
 * conservatively:
 *
 *  - A session with no conversion in progress and a part decision of
 *    "directplay" played every stream it states as it is: %AXIS_EFFECT_DIRECT,
 *    which is evidence for the codec and for the container.  Anything less
 *    sure (no part decision, a stream decision that contradicts the
 *    session) is %AXIS_EFFECT_UNKNOWN.
 *  - A stream that was copied into another container (the server's "copy")
 *    shows that the device takes the codec and says nothing about the
 *    container: %AXIS_EFFECT_PASSTHROUGH.
 *  - A stream that was converted is explained in this order:
 *      1. a remote session whose bandwidth is below the source's bitrate:
 *         a bandwidth or quality limit (%AXIS_EFFECT_CONVERTED_LIMIT);
 *      2. a video or audio stream whose output codec differs from its
 *         source codec: the device did not take the source codec
 *         (%AXIS_EFFECT_CONVERTED_CODEC);
 *      3. a video stream converted to the same codec at a lower
 *         resolution: a quality limit (%AXIS_EFFECT_CONVERTED_LIMIT);
 *      4. anything else: %AXIS_EFFECT_CONVERTED_UNCLEAR, which counts as
 *         unknown, not as a failure.
 *    A converted subtitle is always cause unclear.
 *  - A decision word that is not known is %AXIS_EFFECT_UNKNOWN.
 *
 * Only streams whose use is certain give evidence: the one stream of a
 * kind, or else the one the server marks as selected (in a conversion,
 * the one it states a decision for).  Which of several audio tracks
 * played, when the server does not say, is not guessed.
 *
 * When the session was matched to a file Axis has probed, the file's
 * facts (which Axis trusts more than the server's summary) describe what
 * was played; see AxisCompatFileFacts.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-compat-plex.c).
 */

#ifndef AXIS_COMPAT_EVIDENCE_H
#define AXIS_COMPAT_EVIDENCE_H

#include <glib.h>

#include "compat/axis-compat-model.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_EFFECT_UNKNOWN,
	AXIS_EFFECT_DIRECT,
	AXIS_EFFECT_PASSTHROUGH,
	AXIS_EFFECT_CONVERTED_CODEC,
	AXIS_EFFECT_CONVERTED_LIMIT,
	AXIS_EFFECT_CONVERTED_UNCLEAR
} AxisCompatEffect;

/* "unknown", "direct", "passthrough", "converted_codec",
 * "converted_limit", "converted_unclear" (as stored) */
const gchar *
axis_compat_effect_to_string(AxisCompatEffect effect);

gboolean
axis_compat_effect_from_string(
	const gchar      *text,
	AxisCompatEffect *effect
);

/* A phrase for the pages: "played directly", ... */
const gchar *
axis_compat_effect_label(AxisCompatEffect effect);

/**
 * AxisCompatProperty:
 * @AXIS_PROPERTY_NONE: the codec itself (or nothing is blamed)
 * @AXIS_PROPERTY_CONTAINER: the container the stream was in
 * @AXIS_PROPERTY_PROFILE: the codec's profile
 * @AXIS_PROPERTY_BIT_DEPTH: the bit depth
 * @AXIS_PROPERTY_LEVEL: the codec's level
 * @AXIS_PROPERTY_SUBTITLE_FORMAT: the subtitle format
 *
 * What a server that states the cause of a conversion says failed.  A
 * conversion blamed on a property says nothing against the codec in another
 * container, profile or bit depth.  Only %AXIS_EFFECT_CONVERTED_CODEC
 * names a property.
 */
typedef enum
{
	AXIS_PROPERTY_NONE,
	AXIS_PROPERTY_CONTAINER,
	AXIS_PROPERTY_PROFILE,
	AXIS_PROPERTY_BIT_DEPTH,
	AXIS_PROPERTY_LEVEL,
	AXIS_PROPERTY_SUBTITLE_FORMAT
} AxisCompatProperty;

/* "" for none, else "container", "profile", "bit_depth", "level",
 * "subtitle_format" (as stored) */
const gchar *
axis_compat_property_to_string(AxisCompatProperty property);

gboolean
axis_compat_property_from_string(
	const gchar        *text,
	AxisCompatProperty *property
);

/* "the container", "the profile", ...; "" for none */
const gchar *
axis_compat_property_label(AxisCompatProperty property);

/**
 * AxisCompatEvidence:
 * @kind: AXIS_COMPAT_STREAM_VIDEO, _AUDIO or _SUBTITLE
 * @codec: lower case, or %NULL (a stream without a known codec proves
 *   nothing and is not recorded)
 * @profile: lower case or %NULL
 * @bit_depth: 0 when not known
 * @res_class: "sd", "720p", "1080p", "2160p", "other", or %NULL (video)
 * @hdr: -1 not known, 0 SDR, 1 HDR (video)
 * @channels: 0 when not known (audio)
 * @container: lower case, or %NULL; used by %AXIS_EFFECT_DIRECT, and by a
 *   conversion blamed on the container (the container that was not taken)
 * @effect: what the session proves
 * @out_codec: what the server converted the stream to, or %NULL
 * @stated_reason: the reason(s) the server itself states for this
 *   conversion, as it sent them (cleaned, at most 64 characters), or %NULL
 *   (Plex states none; the inferred @effect then stands alone)
 * @property: what the server blames, for %AXIS_EFFECT_CONVERTED_CODEC
 * @remote: the session was not on the local network (or not known to be)
 */
typedef struct
{
	gint               kind;
	gchar             *codec;
	gchar             *profile;
	gint               bit_depth;
	gchar             *res_class;
	gint               hdr;
	gint               channels;
	gchar             *container;
	AxisCompatEffect   effect;
	gchar             *out_codec;
	gchar             *stated_reason;
	AxisCompatProperty property;
	gboolean           remote;
} AxisCompatEvidence;

void
axis_compat_evidence_free(AxisCompatEvidence *evidence);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisCompatEvidence, axis_compat_evidence_free)

/**
 * AxisCompatFileFacts:
 *
 * What Axis's own probe found in the file that was played; any field may be
 * unknown (%NULL, 0, -1).  @hdr is -1 unknown, 0 SDR, 1 HDR.
 */
typedef struct
{
	const gchar *video_codec;
	const gchar *video_profile;
	gint         bit_depth;
	const gchar *res_class;
	gint         hdr;
	const gchar *audio_codec;
	gint         audio_channels;
	const gchar *container;
	gint64       bitrate_kbps;
} AxisCompatFileFacts;

/**
 * axis_compat_container_family:
 * @name: a container as ffprobe or the server names it ("matroska,webm",
 *   "mov,mp4,m4a,3gp,3g2,mj2", "mkv", "mp4", "mp3", "flac", "ogg")
 *
 * Returns: (transfer full) (nullable): "mkv", "mp4", "mp3", "flac" or
 *   "ogg" for those families, for any other name its lower case (letters,
 *   digits and "_.-", at most 16 characters), %NULL for no usable name
 */
gchar *
axis_compat_container_family(const gchar *name);

/* A codec's name in one spelling ("avc" and "h264" are "h264", "h265" and
 * "hvc1" "hevc", "dca" "dts", ...); lower case.  Returns: (transfer full)
 * (nullable) */
gchar *
axis_compat_codec_name(const gchar *name);

/**
 * axis_compat_classify_session:
 * @session: one session as one poll saw it
 * @facts: (nullable): the played file's probed facts
 *
 * Returns: (transfer container) (element-type AxisCompatEvidence): the
 *   evidence, at most one entry per kind of stream and property; empty when
 *   the session proves nothing.  When the server states why the session is
 *   converted (session->transcode.reasons), those reasons decide, through
 *   the table of axis-compat-reasons.h, and the inference above is not used.
 */
GPtrArray *
axis_compat_classify_session(
	const AxisCompatSession   *session,
	const AxisCompatFileFacts *facts
);

G_END_DECLS

#endif /* AXIS_COMPAT_EVIDENCE_H */
