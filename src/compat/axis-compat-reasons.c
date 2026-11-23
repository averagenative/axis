/*
 * axis-compat-reasons.c - the table of stated reasons
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "compat/axis-compat-reasons.h"

#include <string.h>

#include "compat/axis-compat-model.h"

#define CODEC AXIS_EFFECT_CONVERTED_CODEC
#define LIMIT AXIS_EFFECT_CONVERTED_LIMIT
#define UNCLEAR AXIS_EFFECT_CONVERTED_UNCLEAR

/* Jellyfin 10.11.8, TranscodeReason, in the server's order. */
static const AxisCompatReason reasons[] = {
	{ "ContainerNotSupported", AXIS_REASON_CONTAINER, CODEC, AXIS_PROPERTY_CONTAINER },
	{ "VideoCodecNotSupported", AXIS_REASON_VIDEO, CODEC, AXIS_PROPERTY_NONE },
	{ "AudioCodecNotSupported", AXIS_REASON_AUDIO, CODEC, AXIS_PROPERTY_NONE },
	{ "SubtitleCodecNotSupported", AXIS_REASON_SUBTITLE, CODEC, AXIS_PROPERTY_SUBTITLE_FORMAT },
	{ "AudioIsExternal", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "SecondaryAudioNotSupported", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoProfileNotSupported", AXIS_REASON_VIDEO, CODEC, AXIS_PROPERTY_PROFILE },
	{ "VideoLevelNotSupported", AXIS_REASON_VIDEO, CODEC, AXIS_PROPERTY_LEVEL },
	{ "VideoResolutionNotSupported", AXIS_REASON_VIDEO, LIMIT, AXIS_PROPERTY_NONE },
	{ "VideoBitDepthNotSupported", AXIS_REASON_VIDEO, CODEC, AXIS_PROPERTY_BIT_DEPTH },
	{ "VideoFramerateNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "RefFramesNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AnamorphicVideoNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "InterlacedVideoNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioChannelsNotSupported", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioProfileNotSupported", AXIS_REASON_AUDIO, CODEC, AXIS_PROPERTY_PROFILE },
	{ "AudioSampleRateNotSupported", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "AudioBitDepthNotSupported", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "ContainerBitrateExceedsLimit", AXIS_REASON_ANY, LIMIT, AXIS_PROPERTY_NONE },
	{ "VideoBitrateNotSupported", AXIS_REASON_VIDEO, LIMIT, AXIS_PROPERTY_NONE },
	{ "AudioBitrateNotSupported", AXIS_REASON_AUDIO, LIMIT, AXIS_PROPERTY_NONE },
	{ "UnknownVideoStreamInfo", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "UnknownAudioStreamInfo", AXIS_REASON_AUDIO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "DirectPlayError", AXIS_REASON_ANY, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoRangeTypeNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "VideoCodecTagNotSupported", AXIS_REASON_VIDEO, UNCLEAR, AXIS_PROPERTY_NONE },
	{ "StreamCountExceedsLimit", AXIS_REASON_ANY, UNCLEAR, AXIS_PROPERTY_NONE }
};

const AxisCompatReason *
axis_compat_reason_lookup(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && i < G_N_ELEMENTS(reasons); i++) {
		if (strcmp(reasons[i].name, name) == 0)
			return &reasons[i];
	}
	return NULL;
}

guint
axis_compat_reason_count(void)
{
	return G_N_ELEMENTS(reasons);
}

const AxisCompatReason *
axis_compat_reason_at(guint index)
{
	return index < G_N_ELEMENTS(reasons) ? &reasons[index] : NULL;
}

gboolean
axis_compat_reason_applies(
	const AxisCompatReason *reason,
	gint                    kind
){
	if (reason == NULL)
		return TRUE;
	switch (reason->scope) {
	case AXIS_REASON_VIDEO:
		return kind == AXIS_COMPAT_STREAM_VIDEO;
	case AXIS_REASON_AUDIO:
		return kind == AXIS_COMPAT_STREAM_AUDIO;
	case AXIS_REASON_SUBTITLE:
		return kind == AXIS_COMPAT_STREAM_SUBTITLE;
	case AXIS_REASON_ANY:
		return TRUE;
	default:
		return FALSE;
	}
}
