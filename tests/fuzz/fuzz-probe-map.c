/*
 * fuzz-probe-map.c - libFuzzer target for the JSON reader and for the
 * mapper from ffprobe's output to the model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is ffprobe's report on a file (the output of the container and
 * streams call), optionally followed by the line "@@FRAMES@@" and the
 * output of the frame call.  Whatever it is, the reader and the mapper
 * must return without crashing or allocating without bound, and what they
 * return must keep the promises of axis-probe-model.h:
 *
 *  - every string is valid UTF-8 without control characters;
 *  - every number is unknown or within its limits, and at most
 *    AXIS_PROBE_MAX_STREAMS streams are kept;
 *  - a picture that is not a cover has an HDR class or is uncertain, and a
 *    file with any stream in doubt is in doubt;
 *  - a stream that carries a Dolby Vision record or Dolby Vision frame
 *    metadata is never classified as anything else, and the file is
 *    never SDR because of it.
 */

#include <glib.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "probe/axis-probe-json.h"
#include "probe/axis-probe-model.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define MARKER "\n@@FRAMES@@\n"

static void
check_text(const gchar *text)
{
	const guchar *p;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL))
		abort();
	for (p = (const guchar *)text; *p != '\0'; p++) {
		if (*p < 0x20 || *p == 0x7f)
			abort();
	}
}

static void
check_stream(const AxisMediaStream *s)
{
	if (s->index < 0 || s->index > 100000)
		abort();
	check_text(s->codec);
	check_text(s->profile);
	check_text(s->language);
	check_text(s->title);
	check_text(s->pixel_format);
	check_text(s->field_order);
	check_text(s->color_range);
	check_text(s->color_space);
	check_text(s->color_transfer);
	check_text(s->color_primaries);
	check_text(s->channel_layout);
	check_text(s->hdr_note);
	if (s->codec != NULL && g_utf8_strlen(s->codec, -1) > AXIS_PROBE_CODEC_CHARS)
		abort();
	if (s->title != NULL && g_utf8_strlen(s->title, -1) > AXIS_PROBE_TAG_CHARS)
		abort();

	if (s->width != AXIS_PROBE_UNKNOWN && (s->width < 1 || s->width > AXIS_PROBE_MAX_DIMENSION))
		abort();
	if (s->height != AXIS_PROBE_UNKNOWN && (s->height < 1 || s->height > AXIS_PROBE_MAX_DIMENSION))
		abort();
	if ((s->width == AXIS_PROBE_UNKNOWN) != (s->height == AXIS_PROBE_UNKNOWN))
		abort();
	if (s->bitrate != AXIS_PROBE_UNKNOWN && (s->bitrate < 0 || s->bitrate > AXIS_PROBE_MAX_BITRATE))
		abort();
	if (s->duration_ms != AXIS_PROBE_UNKNOWN && (s->duration_ms < 0 || s->duration_ms > AXIS_PROBE_MAX_DURATION_MS))
		abort();
	if (s->bit_depth != AXIS_PROBE_UNKNOWN && (s->bit_depth < 1 || s->bit_depth > 64))
		abort();
	if (s->channels != AXIS_PROBE_UNKNOWN && (s->channels < 1 || s->channels > AXIS_PROBE_MAX_CHANNELS))
		abort();
	if (s->sample_rate != AXIS_PROBE_UNKNOWN && (s->sample_rate < 1 || s->sample_rate > AXIS_PROBE_MAX_SAMPLE_RATE))
		abort();
	if (s->frame_rate >= 0 && (!isfinite(s->frame_rate) || s->frame_rate <= 0 || s->frame_rate > AXIS_PROBE_MAX_FRAME_RATE))
		abort();
	if (s->real_frame_rate >= 0 &&
	    (!isfinite(s->real_frame_rate) || s->real_frame_rate <= 0 || s->real_frame_rate > AXIS_PROBE_MAX_FRAME_RATE))
		abort();
	if (s->dv_profile != AXIS_PROBE_UNKNOWN && (s->dv_profile < 0 || s->dv_profile > 255))
		abort();

	if (s->kind != AXIS_STREAM_VIDEO) {
		if (s->hdr_format != AXIS_HDR_NONE || s->hdr_uncertain)
			abort();
		return;
	}
	if (!s->attached_pic) {
		/* Never "no class and no doubt". */
		if (s->hdr_format == AXIS_HDR_NONE && !s->hdr_uncertain)
			abort();
		/* Dolby Vision, however it is signalled, is Dolby Vision. */
		if ((s->signals.dovi_record || s->signals.dovi_metadata) && s->hdr_format != AXIS_HDR_DOLBY_VISION)
			abort();
		/* HDR10+ metadata is never plain SDR. */
		if (s->signals.hdr10plus && s->hdr_format == AXIS_HDR_SDR)
			abort();
		/* Something that looks like dynamic metadata is doubt at least. */
		if (s->signals.dynamic_other && !s->hdr_uncertain)
			abort();
		if (s->hdr_uncertain && s->hdr_note == NULL)
			abort();
		/* HDR10 static metadata: what is stored is always in range;
		 * values that were incomplete, out of range or contradictory are
		 * never stored, and never certain HDR10, HDR10+ or HLG; a certain
		 * HDR10 stream has its values. */
		if (!axis_hdr10_metadata_valid(&s->hdr10))
			abort();
		if ((s->signals.hdr10_bad || s->signals.hdr10_conflict) && (s->hdr10.mastering || s->hdr10.light))
			abort();
		if ((s->signals.hdr10_bad || s->signals.hdr10_conflict) && !s->hdr_uncertain &&
		    (s->hdr_format == AXIS_HDR_HDR10 || s->hdr_format == AXIS_HDR_HDR10PLUS ||
		     (s->hdr_format == AXIS_HDR_HLG && (s->signals.has_mastering || s->signals.has_content_light))))
			abort();
		if (s->hdr_format == AXIS_HDR_HDR10 && !s->hdr_uncertain && !s->hdr10.mastering)
			abort();
	} else if (s->hdr10.mastering || s->hdr10.light) {
		/* A cover picture is not classified and carries no values. */
		abort();
	}
}

static void
check_info(
	const AxisMediaInfo *info,
	gint64               file_size
){
	gboolean doubt = FALSE;
	AxisHdrFormat top = AXIS_HDR_NONE;
	guint i;

	if (info->streams->len == 0 || info->streams->len > AXIS_PROBE_MAX_STREAMS)
		abort();
	check_text(info->container);
	check_text(info->tag_title);
	check_text(info->tag_artist);
	check_text(info->tag_album);
	check_text(info->hdr_note);
	if (info->container == NULL)
		abort();
	if (info->duration_ms != AXIS_PROBE_UNKNOWN &&
	    (info->duration_ms < 0 || info->duration_ms > AXIS_PROBE_MAX_DURATION_MS))
		abort();
	if (info->bitrate != AXIS_PROBE_UNKNOWN && (info->bitrate < 1 || info->bitrate > AXIS_PROBE_MAX_BITRATE))
		abort();
	if (info->chapter_count != AXIS_PROBE_UNKNOWN &&
	    (info->chapter_count < 0 || info->chapter_count > AXIS_PROBE_MAX_CHAPTERS))
		abort();
	if (info->streams_omitted < 0 || info->stream_count < (gint)info->streams->len)
		abort();
	if (info->bits_per_pixel >= 0 && (!isfinite(info->bits_per_pixel) || info->bits_per_pixel >= 1000000.0))
		abort();
	if (info->audio_bitrate_total != AXIS_PROBE_UNKNOWN &&
	    (info->audio_bitrate_total < 1 || info->audio_bitrate_total > AXIS_PROBE_MAX_BITRATE))
		abort();
	if (info->main_video >= (gint)info->streams->len || info->main_audio >= (gint)info->streams->len)
		abort();
	(void)file_size;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		check_stream(s);
		if (s->kind == AXIS_STREAM_VIDEO && !s->attached_pic) {
			if (s->hdr_format > top)
				top = s->hdr_format;
			if (s->hdr_format == AXIS_HDR_NONE || s->hdr_uncertain)
				doubt = TRUE;
		}
	}
	/* The file is as restrictive as its most restrictive picture and in
	 * doubt if any is. */
	if (info->hdr_format != top)
		abort();
	if (doubt && !info->hdr_uncertain)
		abort();
	if (info->hdr_uncertain && info->hdr_note == NULL)
		abort();
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text = (const gchar *)data;
	const gchar *marker;
	gsize streams_length = size;
	const gchar *frames = NULL;
	gsize frames_length = 0;
	AxisMediaInfo *info = NULL;
	gchar *error = NULL;
	AxisJsonDoc *doc;
	gchar *problem = NULL;
	gint64 file_size;

	if (size == 0 || size > 1024 * 1024)
		return 0;

	/* The reader on its own. */
	doc = axis_json_parse(text, size, &problem);
	g_free(problem);
	axis_json_doc_free(doc);

	marker = g_strstr_len(text, (gssize)size, MARKER);
	if (marker != NULL) {
		streams_length = (gsize)(marker - text);
		frames = marker + strlen(MARKER);
		frames_length = size - streams_length - strlen(MARKER);
	}
	file_size = (gint64)size * 1000;

	if (axis_probe_map(text, streams_length, frames, frames_length, file_size, &info, &error)) {
		check_info(info, file_size);
		axis_media_info_free(info);
		if (error != NULL)
			abort();
	} else {
		if (info != NULL || error == NULL || !g_utf8_validate(error, -1, NULL))
			abort();
		check_text(error);
		g_free(error);
	}

	return 0;
}
