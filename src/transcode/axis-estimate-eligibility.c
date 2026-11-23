/*
 * axis-estimate-eligibility.c - the reasons, in their order, and the one
 * decision every estimate, plan and conversion uses
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A file is tried against each reason in the order of #AxisEstimateReason
 * and gets the first that applies.  Every check fails closed: a fact that
 * is missing, unknown or out of range is a reason, never a pass.
 */

#include "transcode/axis-estimate-private.h"

#include <math.h>
#include <string.h>

/* ── Reasons ────────────────────────────────────────────────────── */

static const struct
{
	const gchar *id;
	const gchar *label;
	const gchar *help;
} reasons[AXIS_ESTIMATE_N_REASONS] = {
	{ "eligible", "Would be converted",
	  "Every fact the decision needs is known and allows it, and the estimated saving is at least the minimum." },
	{ "not_applicable", "Not for this conversion profile",
	  "The conversion profile is for another kind of file (video or audio) or another media type." },
	{ "not_present", "Not found by the last scan",
	  "The last complete scan did not find the file; nothing is decided about a file that may be gone." },
	{ "pinned", "Pinned",
	  "It has more than one hard link, or is under a downloads folder: converting it would change what the other "
	  "link, or the download, sees." },
	{ "not_probed", "File not read yet",
	  "Nothing is known yet of what is inside it. It is probed by itself; until then it is not converted." },
	{ "probe_failed", "File could not be read",
	  "ffprobe could not read it, or did not finish. It is probed again when it changes or an administrator asks." },
	{ "probe_stale", "Changed since the file was read",
	  "Its content changed since it was probed, or it was probed by an older version of the probe, so what is "
	  "known may describe older content. It is probed again by itself." },
	{ "hdr_uncertain", "HDR uncertain",
	  "Its HDR signalling is incomplete or conflicting, or could not be classified: converting it could lose or "
	  "misread the HDR, so it is not converted." },
	{ "dolby_vision", "Dolby Vision",
	  "Dolby Vision is never converted: the Dolby Vision layer would be lost." },
	{ "hdr10plus", "HDR10+",
	  "HDR10+ is not converted until it is verified that its dynamic metadata survives." },
	{ "hdr_to_sdr", "HDR source, SDR-only profile",
	  "The profile is SDR only; tone-mapping HDR to SDR changes the picture and is not done." },
	{ "interlaced", "Interlaced",
	  "Interlaced video is not converted until deinterlacing is handled." },
	{ "field_order_unknown", "Not known to be progressive",
	  "The probe could not tell whether the picture is interlaced, so it is treated as if it might be." },
	{ "layout", "Streams the profile cannot carry",
	  "The container, or a stream (a second picture, a data stream, an attachment, image or styled subtitles, an "
	  "audio codec the output container cannot hold), is one the profile cannot carry over unchanged." },
	{ "codec_unknown", "Codec without figures",
	  "The source codec, or whether its audio is lossless, is not one the estimator has figures for." },
	{ "lossy_source", "Lossy source",
	  "Already lossy: converting lossy to lossy loses quality for little saving, and the target does not allow it." },
	{ "facts_missing", "Facts missing",
	  "The duration, picture size, frame rate or a bitrate the estimate needs is not known." },
	{ "facts_inconsistent", "Facts do not add up",
	  "The bitrates the probe recorded do not fit the file's size, so no estimate made from them can be trusted." },
	{ "better_codec", "Already in a more efficient codec",
	  "It is already in a codec that saves more space than the profile's (HEVC, AV1 or VP9 to H.264; AV1 or VP9 "
	  "to HEVC); converting would make it larger or worse." },
	{ "already_efficient", "Already efficient",
	  "It is already in the profile's codec at or below the threshold (for video, the \"large for its class\" "
	  "setting of the Disk usage pages), so converting again would cost quality for little." },
	{ "album", "Album not eligible as a whole",
	  "Albums are converted whole, and another file of its album is not eligible." },
	{ "would_not_shrink", "Would not shrink enough",
	  "The estimated saving is below the minimum saving the profile asks for." },
	{ "offline", "Disk offline",
	  "It would be converted, but its disk is offline now; it is counted from what Axis last saw." }
};

static const gchar *const reason_ids[AXIS_ESTIMATE_N_REASONS] = {
	"not_applicable", "not_present", "pinned", "not_probed", "probe_failed", "probe_stale", "hdr_uncertain",
	"dolby_vision", "hdr10plus", "hdr_to_sdr", "interlaced", "field_order_unknown", "layout", "codec_unknown",
	"lossy_source", "facts_missing", "facts_inconsistent", "better_codec", "already_efficient", "album",
	"would_not_shrink", "offline", NULL
};

const gchar *
axis_estimate_reason_to_string(AxisEstimateReason reason)
{
	return (guint)reason < AXIS_ESTIMATE_N_REASONS ? reasons[reason].id : "unknown";
}

gboolean
axis_estimate_reason_from_string(
	const gchar        *text,
	AxisEstimateReason *reason
){
	guint i;

	for (i = 0; text != NULL && i < AXIS_ESTIMATE_N_REASONS; i++) {
		if (strcmp(text, reasons[i].id) == 0) {
			*reason = (AxisEstimateReason)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_estimate_reason_label(AxisEstimateReason reason)
{
	return (guint)reason < AXIS_ESTIMATE_N_REASONS ? reasons[reason].label : "Unknown";
}

const gchar *
axis_estimate_reason_help(AxisEstimateReason reason)
{
	return (guint)reason < AXIS_ESTIMATE_N_REASONS ? reasons[reason].help : "";
}

const gchar *const *
axis_estimate_reason_ids(void)
{
	return reason_ids;
}

/* ── Tables ─────────────────────────────────────────────────────── */

static gboolean
in_list(
	const gchar        *value,
	const gchar *const *list
){
	guint i;

	for (i = 0; value != NULL && list[i] != NULL; i++) {
		if (strcmp(value, list[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

/* Containers the estimator knows how a conversion reads (ffprobe's names). */
static const gchar *const video_containers[] = {
	"matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2", "avi", "mpegts", NULL
};
static const gchar *const audio_containers[] = {
	"flac", "mp3", "ogg", "wav", "aiff", "mov,mp4,m4a,3gp,3g2,mj2", "ape", "wv", "matroska,webm", "tta", NULL
};

/* What MP4 can carry as it is. */
static const gchar *const mp4_audio[] = { "aac", "ac3", "eac3", "mp3", "alac", "opus", "flac", NULL };
static const gchar *const mp4_text[] = { "subrip", "srt", "mov_text", "webvtt", "text", NULL };

/* Codecs at least as efficient as the target's: converting to it would
 * make the file larger or worse. */
static const gchar *const better_than_hevc[] = { "av1", "vp9", "vvc", NULL };
static const gchar *const better_than_h264[] = { "hevc", "av1", "vp9", "vvc", NULL };

static gboolean
field_based(const gchar *order)
{
	return g_strcmp0(order, "tt") == 0 || g_strcmp0(order, "bb") == 0 || g_strcmp0(order, "tb") == 0 ||
		g_strcmp0(order, "bt") == 0;
}

/* A stream bitrate as bytes over @seconds; -1 when unknown. */
static gdouble
stream_bytes(
	const AxisMediaStream *s,
	gdouble                seconds
){
	if (s->bitrate <= 0 || s->bitrate > AXIS_PROBE_MAX_BITRATE || seconds <= 0)
		return -1.0;
	return (gdouble)s->bitrate * seconds / 8.0;
}

/* Whether an audio track is dropped by the target's language rule: only a
 * track with a known language not on the list. */
static gboolean
dropped(
	const AxisEstimateTarget *target,
	const AxisMediaStream    *s
){
	if (target->keep_audio_languages == NULL || s->kind != AXIS_STREAM_AUDIO)
		return FALSE;
	if (s->language == NULL || s->language[0] == '\0' || strcmp(s->language, "und") == 0)
		return FALSE;
	return !in_list(s->language, target->keep_audio_languages);
}

static gboolean
is_stereo_aac(const AxisMediaStream *s)
{
	return s != NULL && g_strcmp0(s->codec, "aac") == 0 && s->channels >= 1 && s->channels <= 2;
}

/* ── One file ───────────────────────────────────────────────────── */

/* The minimum saving a target asks for, never below the floor: a target
 * (whatever profile it came from) cannot lower it. */
static gdouble
min_saving_of(const AxisEstimateTarget *target)
{
	gdouble asked = target->min_saving;

	if (!isfinite(asked) || asked < AXIS_ESTIMATE_MIN_SAVING_FLOOR)
		return AXIS_ESTIMATE_MIN_SAVING_FLOOR;
	return asked;
}

/* A double as whole bytes, within what a file can be. */
static gint64
whole(gdouble value)
{
	if (!isfinite(value) || value <= 0)
		return 0;
	if (value > 1e15)
		return G_GINT64_CONSTANT(1000000000000000);
	return (gint64)llround(value);
}

static void
set_size(
	AxisEstimateResult *result,
	gdouble             low,
	gdouble             mid,
	gdouble             high,
	guint               cell
){
	result->low = whole(low);
	result->mid = MAX(whole(mid), result->low);
	result->high = MAX(whole(high), result->mid);
	result->cell = cell;
	result->sized = TRUE;
}

/* Everything up to the probe's facts, for any target. */
static AxisEstimateReason
index_reason(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target
){
	const AxisEstimateIndex *idx = &file->index;
	const AxisMediaInfo *info = file->info;

	if (g_strcmp0(idx->kind, target->media == AXIS_TARGET_VIDEO ? "video" : "audio") != 0)
		return AXIS_ESTIMATE_NOT_APPLICABLE;
	if ((guint)idx->root_type >= AXIS_MEDIA_TYPE_COUNT || (target->root_types & AXIS_TARGET_ROOT(idx->root_type)) == 0)
		return AXIS_ESTIMATE_NOT_APPLICABLE;
	if (g_strcmp0(idx->state, "present") != 0)
		return AXIS_ESTIMATE_NOT_PRESENT;
	if (idx->pinned || idx->nlink != 1)
		return AXIS_ESTIMATE_PINNED;
	if (info == NULL)
		return AXIS_ESTIMATE_NOT_PROBED;
	if (g_strcmp0(info->status, "ok") != 0)
		return AXIS_ESTIMATE_PROBE_FAILED;
	if (idx->changed || info->probe_version < AXIS_PROBE_VERSION || info->streams == NULL ||
	    info->file_size != idx->size || info->mtime_sec != idx->mtime_sec || info->mtime_nsec != idx->mtime_nsec ||
	    info->ctime_sec != idx->ctime_sec || info->ctime_nsec != idx->ctime_nsec || info->inode != idx->inode)
		return AXIS_ESTIMATE_PROBE_STALE;
	return AXIS_ESTIMATE_ELIGIBLE;
}

static AxisEstimateReason
video_reason(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *result
){
	const AxisEstimateIndex *idx = &file->index;
	const AxisMediaInfo *info = file->info;
	AxisEstimateShape shape;
	AxisHdrFormat worst = info->hdr_format;
	gboolean kept_audio = FALSE;
	gdouble size = (gdouble)idx->size;
	gdouble video_bytes;
	gdouble dropped_bytes = 0;
	gdouble residual;
	gdouble added_low = 0;
	gdouble added_mid = 0;
	gdouble added_high = 0;
	gdouble bpp;
	AxisEstimatePart part;
	gint quality;
	guint i;

	axis_estimate_shape_init(&shape, info);

	/* HDR, then the scan type: only for a file that has a picture (one
	 * without falls to the layout check). */
	if (shape.n_video > 0) {
		gboolean uncertain = info->hdr_uncertain || info->hdr_format == AXIS_HDR_NONE;

		for (i = 0; i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

			if (s->kind != AXIS_STREAM_VIDEO || s->attached_pic)
				continue;
			if (s->hdr_uncertain || s->hdr_format == AXIS_HDR_NONE)
				uncertain = TRUE;
			if (s->hdr_format > worst)
				worst = s->hdr_format;
		}
		if (uncertain || (guint)worst > AXIS_HDR_DOLBY_VISION)
			return AXIS_ESTIMATE_HDR_UNCERTAIN;
		if (worst == AXIS_HDR_DOLBY_VISION)
			return AXIS_ESTIMATE_DOLBY_VISION;
		if (worst == AXIS_HDR_HDR10PLUS)
			return AXIS_ESTIMATE_HDR10PLUS;
		/* An HDR source keeps its HDR or is not converted: only a target
		 * that writes 10-bit HEVC for it (with the metadata) takes it,
		 * whatever else the target says. */
		if ((worst == AXIS_HDR_HDR10 || worst == AXIS_HDR_HLG) &&
		    (target->hdr_bit_depth < 10 || target->codec != AXIS_TCODEC_HEVC))
			return AXIS_ESTIMATE_HDR_TO_SDR;
		if (info->interlaced == AXIS_TRI_YES || (shape.video != NULL && field_based(shape.video->field_order)))
			return AXIS_ESTIMATE_INTERLACED;
		if (info->interlaced != AXIS_TRI_NO &&
		    (shape.video == NULL || g_strcmp0(shape.video->field_order, "progressive") != 0))
			return AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN;
	}

	/* What the output can carry. */
	if (info->streams_omitted != 0 || shape.n_video != 1 || shape.n_other > 0 ||
	    !in_list(info->container, video_containers))
		return AXIS_ESTIMATE_LAYOUT;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind == AXIS_STREAM_SUBTITLE) {
			if (s->subtitle_kind == AXIS_SUBTITLE_UNKNOWN)
				return AXIS_ESTIMATE_LAYOUT;
			if (target->subtitles == AXIS_SUBTITLES_PLAIN_TEXT &&
			    (s->subtitle_kind != AXIS_SUBTITLE_TEXT || !in_list(s->codec, mp4_text)))
				return AXIS_ESTIMATE_LAYOUT;
		} else if (s->kind == AXIS_STREAM_AUDIO) {
			if (target->container == AXIS_CONTAINER_MP4 && !dropped(target, s) && !in_list(s->codec, mp4_audio))
				return AXIS_ESTIMATE_LAYOUT;
			if (!dropped(target, s))
				kept_audio = TRUE;
		} else if (s->kind == AXIS_STREAM_ATTACHMENT && target->container == AXIS_CONTAINER_MP4) {
			return AXIS_ESTIMATE_LAYOUT;
		}
	}
	/* A language rule never leaves a film without sound. */
	if (shape.n_audio > 0 && !kept_audio)
		return AXIS_ESTIMATE_LAYOUT;

	if (shape.video == NULL || axis_estimate_codec_efficiency(shape.video->codec) <= 0)
		return AXIS_ESTIMATE_CODEC_UNKNOWN;

	if (shape.seconds <= 0 || shape.pixels <= 0 || shape.fps <= 0 || shape.video_bitrate <= 0 ||
	    shape.source_bpp <= 0 || idx->size <= 0)
		return AXIS_ESTIMATE_FACTS_MISSING;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (dropped(target, s)) {
			gdouble bytes = stream_bytes(s, shape.seconds);

			if (bytes < 0)
				return AXIS_ESTIMATE_FACTS_MISSING;
			dropped_bytes += bytes;
		}
	}

	/* The picture is what is replaced; everything copied is the rest of the
	 * file, at its measured size.  A picture the probe says is larger than
	 * the whole file cannot be estimated from. */
	video_bytes = (gdouble)shape.video_bitrate * shape.seconds / 8.0;
	if (!isfinite(video_bytes) || video_bytes + dropped_bytes > size * 1.05)
		return AXIS_ESTIMATE_FACTS_INCONSISTENT;
	residual = MAX(size - video_bytes - dropped_bytes, 0.0);

	if (in_list(shape.video->codec, target->codec == AXIS_TCODEC_H264 ? better_than_h264 : better_than_hevc))
		return AXIS_ESTIMATE_BETTER_CODEC;
	bpp = info->bits_per_pixel >= 0 ? info->bits_per_pixel : shape.source_bpp;
	if (g_strcmp0(shape.video->codec, axis_target_codec_to_string(target->codec)) == 0 &&
	    bpp * 1000.0 <= (gdouble)target->efficient_milli)
		return AXIS_ESTIMATE_ALREADY_EFFICIENT;

	quality = axis_estimate_target_quality_for(target, info->resolution_class, idx->size);
	result->quality = quality;
	if (!axis_estimate_model_video(model, target, &shape, shape.video->codec,
	        worst == AXIS_HDR_HDR10 || worst == AXIS_HDR_HLG, quality, &part))
		return AXIS_ESTIMATE_FACTS_MISSING;
	/* Never more than the source picture: the estimate does not claim a
	 * file will grow. */
	part.low = MIN(part.low, video_bytes * 0.95);
	part.mid = MIN(part.mid, video_bytes * 0.95);
	part.high = MIN(part.high, video_bytes * 0.95);

	/* A stereo AAC track that the target adds. */
	if (shape.n_audio > 0 && target->audio_bitrate > 0 &&
	    (target->audio == AXIS_AUDIO_COPY_ADD_STEREO || target->audio == AXIS_AUDIO_STEREO_FIRST)) {
		gboolean has = FALSE;

		if (target->audio == AXIS_AUDIO_STEREO_FIRST) {
			has = is_stereo_aac(shape.audio);
		} else {
			for (i = 0; i < info->streams->len; i++) {
				const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

				if (s->kind == AXIS_STREAM_AUDIO && !dropped(target, s) && is_stereo_aac(s))
					has = TRUE;
			}
		}
		if (!has) {
			added_mid = (gdouble)target->audio_bitrate * shape.seconds / 8.0;
			added_low = added_mid * 0.95;
			added_high = added_mid * 1.05;
		}
	}

	/* The new container's own overhead: a small fixed fraction of what is
	 * written anew (the source's is in the rest of the file already). */
	set_size(result, part.low + added_low + residual,
		part.mid + added_mid + residual + 0.003 * (part.mid + added_mid),
		part.high + added_high + residual + 0.006 * (part.high + added_high), part.cell);

	/* Whatever the target's minimum, a file that is not estimated strictly
	 * smaller is never converted, nor one that saves less than the floor. */
	if (idx->size - result->mid <= 0 ||
	    (gdouble)(idx->size - result->mid) < min_saving_of(target) * size ||
	    (target->min_saving_bytes > 0 && idx->size - result->mid < target->min_saving_bytes))
		return AXIS_ESTIMATE_WOULD_NOT_SHRINK;
	return AXIS_ESTIMATE_ELIGIBLE;
}

static AxisEstimateReason
audio_reason(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *result
){
	const AxisEstimateIndex *idx = &file->index;
	const AxisMediaInfo *info = file->info;
	AxisEstimateShape shape;
	const AxisMediaStream *a;
	gdouble size = (gdouble)idx->size;
	gdouble source;
	gdouble rest_low;
	gdouble rest_mid;
	gdouble rest_high;
	AxisEstimatePart part;
	gint nominal;
	gint lossless;

	axis_estimate_shape_init(&shape, info);
	a = shape.audio;
	if (info->streams_omitted != 0 || shape.n_audio != 1 || shape.n_video > 0 || shape.n_subtitle > 0 ||
	    shape.n_other > 0 || shape.n_attachment > 0 || a == NULL || !in_list(info->container, audio_containers))
		return AXIS_ESTIMATE_LAYOUT;

	lossless = a->lossless;
	if (lossless == AXIS_TRI_YES && axis_estimate_lossless_class(a->codec) < 0)
		return AXIS_ESTIMATE_CODEC_UNKNOWN;
	if (lossless == AXIS_TRI_NO && !axis_estimate_lossy_known(a->codec))
		return AXIS_ESTIMATE_CODEC_UNKNOWN;
	if (lossless != AXIS_TRI_YES && lossless != AXIS_TRI_NO)
		return AXIS_ESTIMATE_CODEC_UNKNOWN;
	/* FLAC from a lossy source keeps the loss and is larger. */
	if (lossless == AXIS_TRI_NO && (!target->allow_lossy_source || target->codec == AXIS_TCODEC_FLAC))
		return AXIS_ESTIMATE_LOSSY_SOURCE;

	if (shape.seconds <= 0 || idx->size <= 0)
		return AXIS_ESTIMATE_FACTS_MISSING;
	if (lossless == AXIS_TRI_NO && (a->bitrate <= 0 || a->bitrate > AXIS_PROBE_MAX_BITRATE))
		return AXIS_ESTIMATE_FACTS_MISSING;

	/* The audio is what is replaced; cover art and tags are kept.  A
	 * measured bitrate gives the audio's size and so the rest exactly;
	 * otherwise the rest is guessed from the cover's picture size. */
	if (a->bitrate > 0 && !a->bitrate_estimated) {
		source = stream_bytes(a, shape.seconds);
		if (source < 0 || source > size * 1.05)
			return AXIS_ESTIMATE_FACTS_INCONSISTENT;
		rest_low = rest_mid = rest_high = MAX(size - source, 0.0);
	} else {
		axis_estimate_other_bytes(info, &rest_low, &rest_mid, &rest_high);
		source = size - rest_mid;
		if (source <= 0)
			return AXIS_ESTIMATE_FACTS_INCONSISTENT;
		rest_low = MIN(rest_low, size);
		rest_high = MIN(rest_high, size);
	}

	nominal = target->audio_bitrate_mono > 0 && a->channels == 1 ? target->audio_bitrate_mono :
		target->audio_bitrate;
	if (lossless == AXIS_TRI_NO) {
		if (g_strcmp0(a->codec, "opus") == 0 && target->codec != AXIS_TCODEC_OPUS)
			return AXIS_ESTIMATE_BETTER_CODEC;
		if (g_strcmp0(a->codec, axis_target_codec_to_string(target->codec)) == 0 &&
		    (gdouble)a->bitrate * 1000.0 <= (gdouble)nominal * (gdouble)target->efficient_milli)
			return AXIS_ESTIMATE_ALREADY_EFFICIENT;
	}

	result->quality = target->quality;
	if (!axis_estimate_model_audio(model, target, &shape, source, &part))
		return AXIS_ESTIMATE_FACTS_MISSING;
	set_size(result, part.low + rest_low, part.mid + rest_mid, part.high + rest_high, part.cell);
	/* The minimum saving is the album's (axis_estimate_evaluate()). */
	return AXIS_ESTIMATE_ELIGIBLE;
}

static AxisEstimateReason
file_reason(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *result
){
	AxisEstimateReason reason;

	memset(result, 0, sizeof(*result));
	result->now = MAX(file->index.size, 0);
	result->quality = target->quality;
	reason = index_reason(file, target);
	if (reason != AXIS_ESTIMATE_ELIGIBLE)
		return reason;
	if (target->media == AXIS_TARGET_VIDEO)
		return video_reason(file, target, model, result);
	return audio_reason(file, target, model, result);
}

/* ── Units ──────────────────────────────────────────────────────── */

gboolean
axis_estimate_result_converts(const AxisEstimateResult *result)
{
	return result->reason == AXIS_ESTIMATE_ELIGIBLE || result->reason == AXIS_ESTIMATE_OFFLINE;
}

/* What a file is after a conversion is never more than it is now: a
 * conversion keeps the original when the output is not smaller.  So every
 * estimate (not only those of files that would be converted) is capped at
 * the file's size; whether the saving is enough was decided before. */
static void
clamp_to_now(AxisEstimateResult *result)
{
	result->high = MIN(result->high, result->now);
	result->mid = MIN(result->mid, result->high);
	result->low = MIN(result->low, result->mid);
}

void
axis_estimate_evaluate(
	const AxisEstimateFile *const *files,
	guint                          n_files,
	const AxisEstimateTarget      *target,
	const AxisEstimateModel       *model,
	AxisEstimateResult            *results
){
	gboolean units = target->media == AXIS_TARGET_AUDIO;
	guint i;

	g_return_if_fail(n_files == 0 || (files != NULL && results != NULL));
	g_return_if_fail(target != NULL && model != NULL);

	for (i = 0; i < n_files; i++)
		results[i].reason = file_reason(files[i], target, model, &results[i]);

	if (units && n_files > 0) {
		gboolean whole = TRUE;
		gint bad = -1;

		/* The unit must be given whole: as many files as the index counts
		 * in it, all of the same unit. */
		for (i = 0; i < n_files; i++) {
			const AxisEstimateIndex *idx = &files[i]->index;

			if (idx->unit == NULL || idx->unit_files != n_files || idx->root_id != files[0]->index.root_id ||
			    g_strcmp0(idx->unit, files[0]->index.unit) != 0)
				whole = FALSE;
			if (bad < 0 && results[i].reason != AXIS_ESTIMATE_ELIGIBLE)
				bad = (gint)i;
		}
		if (!whole) {
			for (i = 0; i < n_files; i++) {
				if (results[i].reason == AXIS_ESTIMATE_ELIGIBLE) {
					results[i].reason = AXIS_ESTIMATE_ALBUM;
					results[i].album_reason = AXIS_ESTIMATE_ALBUM;
					results[i].album_file = 0;
				}
			}
		} else if (bad >= 0) {
			for (i = 0; i < n_files; i++) {
				if (results[i].reason == AXIS_ESTIMATE_ELIGIBLE) {
					results[i].reason = AXIS_ESTIMATE_ALBUM;
					results[i].album_reason = results[bad].reason;
					results[i].album_file = files[bad]->index.file_id;
				}
			}
		} else {
			/*
			 * Every file is eligible: the album's saving decides, from the
			 * figures before they are capped, so a track estimated to grow
			 * counts against the album in full.  One track of an album
			 * that is converted may therefore be estimated at its own size
			 * (albums are never left half converted); the album as a whole
			 * must be estimated strictly smaller, whatever the target's
			 * minimum.
			 */
			gdouble now = 0;
			gdouble mid = 0;

			for (i = 0; i < n_files; i++) {
				now += (gdouble)results[i].now;
				mid += (gdouble)results[i].mid;
			}
			if (now - mid <= 0 || now - mid < min_saving_of(target) * now ||
			    (target->min_saving_bytes > 0 && now - mid < (gdouble)target->min_saving_bytes)) {
				for (i = 0; i < n_files; i++)
					results[i].reason = AXIS_ESTIMATE_WOULD_NOT_SHRINK;
			}
		}
	}

	for (i = 0; i < n_files; i++) {
		if (results[i].reason == AXIS_ESTIMATE_ELIGIBLE && !files[i]->index.volume_usable)
			results[i].reason = AXIS_ESTIMATE_OFFLINE;
		if (results[i].sized)
			clamp_to_now(&results[i]);
	}
}

AxisEstimateReason
axis_estimate_eligibility(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateModel  *model,
	AxisEstimateResult       *result
){
	AxisEstimateResult local;
	const AxisEstimateFile *unit[1];

	unit[0] = file;
	axis_estimate_evaluate(unit, 1, target, model, result != NULL ? result : &local);
	return (result != NULL ? result : &local)->reason;
}

gchar *
axis_estimate_unit_key(
	const gchar   *kind,
	AxisMediaType  root_type,
	const gchar   *rel_path
){
	const gchar *first;
	const gchar *second;
	const gchar *last;

	if (g_strcmp0(kind, "audio") != 0 || (root_type != AXIS_MEDIA_MUSIC && root_type != AXIS_MEDIA_AUDIOBOOKS) ||
	    rel_path == NULL)
		return NULL;
	first = strchr(rel_path, '/');
	second = first != NULL ? strchr(first + 1, '/') : NULL;
	/* "Artist/Album/..." (both parts not empty) is the album. */
	if (first != NULL && first > rel_path && second != NULL && second > first + 1)
		return g_strndup(rel_path, (gsize)(second - rel_path));
	/* Otherwise the folder the file is in. */
	last = strrchr(rel_path, '/');
	return last != NULL ? g_strndup(rel_path, (gsize)(last - rel_path)) : g_strdup("");
}
