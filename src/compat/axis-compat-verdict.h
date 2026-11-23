/*
 * axis-compat-verdict.h - the "harder than" rules, the verdict for a
 * device, a library and a recommendation
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Originals are deleted after conversion, so a wrong "this device can play
 * that" is a permanent loss.  Everything here is built on one rule:
 * unknown is never safe.
 *
 * A *combination* is what a device must be able to play directly: a kind
 * of stream, a codec, a profile, a bit depth, a resolution class, whether
 * the picture is HDR, and the container.  Evidence (axis-compat-evidence.h)
 * has the same fields.
 *
 * What a device is known to be able to play (%axis_compat_covers): the
 * device has been seen playing combination H directly and H covers W when
 *
 *   - the kind and the codec are the same, and the container is the same
 *     (when W names one: container evidence comes only from plays with no
 *     conversion at all, so a stream that was only passed through covers
 *     nothing that names a container);
 *   - the profile is the same when W names one;
 *   - H's bit depth is at least W's, and H's resolution class is at least
 *     W's (sd < 720p < 1080p < 2160p; "other" is unordered and so never
 *     covers, nor is covered), both known;
 *   - H is HDR exactly when W is, when W says (an SDR play shows nothing
 *     about HDR and the other way round).
 *
 * Nothing else implies anything: a different codec, a different container,
 * more channels, a lower bit depth or resolution cover nothing.
 *
 * What a device is known to fail (%axis_compat_fails): the server was seen
 * converting a stream because the device did not take its codec
 * (%AXIS_EFFECT_CONVERTED_CODEC), and the combination W is at least as
 * hard as that stream: the same kind and codec, with W's bit depth and
 * resolution at least those of the failure where the failure states them,
 * and W HDR if the failure was.  The container and the profile are not
 * compared: the device did not take the codec.
 *
 * A server that states why (Jellyfin) may blame a property instead of the
 * codec: the container, the profile, the bit depth, the level or the
 * subtitle format.  Such a failure shows only that the device does not take
 * the codec *with that property*, and so fails W only when W names the same
 * container or profile, or a bit depth at least as great; a level is named
 * by no combination and fails none; a subtitle format fails the same
 * format.  It never counts against the codec elsewhere (another container,
 * another profile, a lower bit depth).  A failure that does not name the
 * value it is about (no container, no profile, no bit depth) fails nothing.
 *
 * The verdict for a device and one combination: *risky* if it is known to
 * fail it (even if it has also been seen playing a related combination:
 * the more cautious reading wins), *safe* if it is known to play it, and
 * *unknown* otherwise.  Conversions explained by a bandwidth or quality
 * limit, conversions of unclear cause and passed-through streams are never
 * evidence of failure, and only the last can ever support a codec, never a
 * container.
 *
 * The verdict for a library and a target: the worst of its devices
 * (risky, then unknown, then safe) over every combination the target needs
 * for the library; no devices at all is unknown.
 *
 * GLib only, no I/O: tested as tables (tests/test-compat-unit.c).
 */

#ifndef AXIS_COMPAT_VERDICT_H
#define AXIS_COMPAT_VERDICT_H

#include <glib.h>

#include "compat/axis-compat-evidence.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_VERDICT_UNKNOWN,
	AXIS_VERDICT_SAFE,
	AXIS_VERDICT_RISKY
} AxisCompatVerdict;

/* "unknown", "safe", "risky" */
const gchar *
axis_compat_verdict_to_string(AxisCompatVerdict verdict);

/* "Unknown", "Safe", "Risky" */
const gchar *
axis_compat_verdict_label(AxisCompatVerdict verdict);

/* The worse of two: risky, then unknown, then safe. */
AxisCompatVerdict
axis_compat_verdict_worse(
	AxisCompatVerdict a,
	AxisCompatVerdict b
);

/**
 * AxisCompatCombo:
 * @kind: AXIS_COMPAT_STREAM_VIDEO, _AUDIO or _SUBTITLE
 * @codec: lower case
 * @profile: lower case, or %NULL for any
 * @bit_depth: 0 for not stated
 * @res_class: "sd", "720p", "1080p", "2160p", "other", or %NULL
 * @hdr: -1 not stated, 0 SDR, 1 HDR
 * @container: "mkv", "mp4", ..., or %NULL for not stated
 */
typedef struct
{
	gint         kind;
	const gchar *codec;
	const gchar *profile;
	gint         bit_depth;
	const gchar *res_class;
	gint         hdr;
	const gchar *container;
} AxisCompatCombo;

/* sd 1, 720p 2, 1080p 3, 2160p 4; 0 for "other", NULL or anything else */
gint
axis_compat_resolution_rank(const gchar *res_class);

/* The combination @evidence describes. */
void
axis_compat_evidence_combo(
	const AxisCompatEvidence *evidence,
	AxisCompatCombo          *combo
);

/* Whether having played @have directly shows the device plays @want
 * (see the rules above).  @have must have been played directly. */
gboolean
axis_compat_covers(
	const AxisCompatCombo *have,
	const AxisCompatCombo *want
);

/* As above for a stream that was only passed through: the container
 * cannot be shown, so @want must not name one. */
gboolean
axis_compat_covers_codec(
	const AxisCompatCombo *have,
	const AxisCompatCombo *want
);

/* Whether a failure on @failed shows the device fails @want. */
gboolean
axis_compat_fails(
	const AxisCompatCombo *failed,
	const AxisCompatCombo *want
);

/* As axis_compat_fails() for a conversion the server blamed on @property
 * (see above); %AXIS_PROPERTY_NONE is axis_compat_fails(). */
gboolean
axis_compat_fails_property(
	const AxisCompatCombo *failed,
	AxisCompatProperty     property,
	const AxisCompatCombo *want
);

/**
 * AxisCompatWhy:
 * @AXIS_WHY_NO_EVIDENCE: nothing about the codec was ever seen
 * @AXIS_WHY_DIRECT: seen playing a combination that covers it
 * @AXIS_WHY_CONVERTED: seen converting the codec, which is at least as hard
 * @AXIS_WHY_EASIER_ONLY: seen playing the codec directly, but only easier
 *   combinations or another container
 * @AXIS_WHY_PASSTHROUGH_ONLY: the codec was seen passed through, which
 *   says nothing of the container
 * @AXIS_WHY_LIMITED_ONLY: conversions seen were bandwidth or quality limits
 * @AXIS_WHY_UNCLEAR_ONLY: conversions seen had no clear cause
 * @AXIS_WHY_PROPERTY_ONLY: conversions seen were blamed on another property
 *   (the container, a profile, a bit depth, a level, a subtitle format) and
 *   say nothing here
 */
typedef enum
{
	AXIS_WHY_NO_EVIDENCE,
	AXIS_WHY_DIRECT,
	AXIS_WHY_CONVERTED,
	AXIS_WHY_EASIER_ONLY,
	AXIS_WHY_PASSTHROUGH_ONLY,
	AXIS_WHY_LIMITED_ONLY,
	AXIS_WHY_UNCLEAR_ONLY,
	AXIS_WHY_PROPERTY_ONLY
} AxisCompatWhy;

/* A sentence fragment for the pages. */
const gchar *
axis_compat_why_label(AxisCompatWhy why);

/**
 * AxisCompatObservation:
 * @combo: the evidence's combination
 * @effect: what the session proved
 * @plays: sessions behind it, and how many of them were remote
 * @remote_plays: how many of those were not local
 *
 * One row of a device's evidence: identical observations are merged and
 * counted.
 */
typedef struct
{
	AxisCompatCombo    combo;
	AxisCompatEffect   effect;
	guint              plays;
	guint              remote_plays;
	AxisCompatProperty property;	/* what the server blamed, for a conversion of the codec */
} AxisCompatObservation;

/**
 * axis_compat_device_verdict:
 * @want: the combination
 * @obs: (array length=n): the device's evidence
 * @n: how many
 * @why: (out) (optional): the reason
 * @deciding: (out) (optional): the index of the observation that decided
 *   (the first that supports the verdict), or -1
 *
 * Returns: the verdict for one device and one combination
 */
AxisCompatVerdict
axis_compat_device_verdict(
	const AxisCompatCombo       *want,
	const AxisCompatObservation *obs,
	guint                        n,
	AxisCompatWhy               *why,
	gint                        *deciding
);

/* ── Recommendation ─────────────────────────────────────────────── */

/**
 * AxisCompatChoice:
 * @verdict: the target's verdict for the library
 * @saving: the estimated saving in bytes (middle of the range), or a
 *   negative number when there is no estimate
 *
 * One target of one library, for axis_compat_recommend().
 */
typedef struct
{
	AxisCompatVerdict verdict;
	gint64            saving;
} AxisCompatChoice;

/**
 * axis_compat_recommend:
 * @choices: (array length=n): the targets that apply to the library
 * @n: how many
 * @best: (out): the index of the target with the largest saving of all
 *   (the one a blocked recommendation is about), or -1 when none has a
 *   saving above zero
 * @next: (out): the index of the target with the next largest saving, or -1
 *
 * Returns: the index of the safe target with the largest estimated saving
 *   (the lowest index on a tie), or -1 when none is safe or none saves
 *   anything: an unknown or risky target is never recommended
 */
gint
axis_compat_recommend(
	const AxisCompatChoice *choices,
	guint                   n,
	gint                   *best,
	gint                   *next
);

G_END_DECLS

#endif /* AXIS_COMPAT_VERDICT_H */
