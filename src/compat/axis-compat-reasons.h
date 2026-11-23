/*
 * axis-compat-reasons.h - the reasons a media server states for converting a
 * stream, and what each proves
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Jellyfin says why it is converting (the "transcode reasons" of a session);
 * Plex never does.  Where a server states reasons they are the evidence, and
 * the inference of axis-compat-evidence.h is not used.  One table maps each
 * reason to what it proves about the device:
 *
 *  - the stream it is about (video, audio, subtitle, or the container, or
 *    any converted stream: a bitrate limit applies to all of them);
 *  - the effect: the device did not take the codec
 *    (%AXIS_EFFECT_CONVERTED_CODEC), a bandwidth or quality limit
 *    (%AXIS_EFFECT_CONVERTED_LIMIT), or cause unclear
 *    (%AXIS_EFFECT_CONVERTED_UNCLEAR, which counts as unknown);
 *  - for a "did not take" reason that is not about the codec itself, the
 *    property that failed (the container, a profile, a bit depth, a level,
 *    a subtitle format).  Such a failure does not count against the codec
 *    elsewhere.
 *
 * A reason the table does not contain is cause unclear: never safe, never
 * a failure.  The table was made from the server's own API description (the
 * enumeration TranscodeReason of Jellyfin 10.11.8, in
 * tests/fixtures/compat/jellyfin/recorded/api-description.json); the
 * reasons seen on a real server are listed in tests/fixtures/compat/jellyfin.
 *
 * GLib only, no I/O.
 */

#ifndef AXIS_COMPAT_REASONS_H
#define AXIS_COMPAT_REASONS_H

#include <glib.h>

#include "compat/axis-compat-evidence.h"

G_BEGIN_DECLS

typedef enum
{
	AXIS_REASON_VIDEO,	/* about the video stream */
	AXIS_REASON_AUDIO,	/* about the audio stream */
	AXIS_REASON_SUBTITLE,	/* about the subtitle stream */
	AXIS_REASON_CONTAINER,	/* about the container (shown against the main stream) */
	AXIS_REASON_ANY		/* about the conversion as a whole */
} AxisCompatReasonScope;

typedef struct
{
	const gchar          *name;
	AxisCompatReasonScope scope;
	AxisCompatEffect      effect;	/* a converted_* effect */
	AxisCompatProperty    property;
} AxisCompatReason;

/* The reason called @name (exactly as the server spells it), or %NULL when
 * the table does not contain it. */
const AxisCompatReason *
axis_compat_reason_lookup(const gchar *name);

/* The table, for tests: @index below axis_compat_reason_count(). */
guint
axis_compat_reason_count(void);

const AxisCompatReason *
axis_compat_reason_at(guint index);

/**
 * axis_compat_reason_applies:
 * @reason: (nullable): from the table, or %NULL for a reason the table does
 *   not contain (which could be about any stream)
 * @kind: the stream, AXIS_COMPAT_STREAM_VIDEO, _AUDIO or _SUBTITLE
 *
 * Whether the reason explains why the stream of @kind was converted.  A
 * container reason is dealt with separately (it explains no conversion).
 */
gboolean
axis_compat_reason_applies(
	const AxisCompatReason *reason,
	gint                    kind
);

G_END_DECLS

#endif /* AXIS_COMPAT_REASONS_H */
