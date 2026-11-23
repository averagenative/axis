/*
 * fuzz-profile-gen.h - arbitrary valid profile definitions, and the rules
 * no definition may get past, for the property test (tests/test-profile.c)
 * and the fuzz targets (fuzz-profile.c, fuzz-estimate.c)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Header only (static functions), so that the fuzz targets, which are
 * built from a single source each, can share it.
 *
 * axis_gen_definition() turns bytes into a definition that passes the
 * strict check: every member takes any value its bounds allow, so the
 * definitions it makes cover the format.  axis_gen_fixed_rule() works out,
 * from a file's facts alone and without the estimator's code, the first of
 * the rules no profile can change that applies to the file;
 * axis_gen_check_decision() then asserts that the estimator agrees, and
 * that whatever converts is estimated strictly smaller by at least the
 * floor.  A broken property is printed with its figures, then the program
 * aborts.
 */

#ifndef AXIS_FUZZ_PROFILE_GEN_H
#define AXIS_FUZZ_PROFILE_GEN_H

#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "transcode/axis-estimate.h"
#include "transcode/axis-profile.h"

typedef struct
{
	const guint8 *data;
	gsize         size;
	gsize         at;
	GRand        *rand;	/* used once the bytes run out, if set */
} AxisGenInput;

static G_GNUC_UNUSED guint8
axis_gen_byte(AxisGenInput *in)
{
	if (in->at < in->size)
		return in->data[in->at++];
	return in->rand != NULL ? (guint8)g_rand_int_range(in->rand, 0, 256) : 0;
}

static G_GNUC_UNUSED guint
axis_gen_pick(
	AxisGenInput *in,
	guint         n
){
	return n > 0 ? axis_gen_byte(in) % n : 0;
}

static const gint64 axis_gen_sizes[] = {
	0, 1, 1000000000, G_GINT64_CONSTANT(20000000000), G_GINT64_CONSTANT(50000000000),
	G_GINT64_CONSTANT(100000000000), G_GINT64_CONSTANT(10000000000000), AXIS_PROFILE_MAX_RULE_BYTES
};

static G_GNUC_UNUSED void
axis_gen_definition(
	AxisGenInput          *in,
	AxisProfileDefinition *def
){
	static const AxisTargetCodec audio_codecs[] = {
		AXIS_TCODEC_FLAC, AXIS_TCODEC_OPUS, AXIS_TCODEC_AAC, AXIS_TCODEC_MP3
	};
	static const gchar *const languages[] = { "eng", "fre", "ger", "jpn", "spa", "ita" };
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	const gint *crfs;
	guint n_crfs = 0;
	guint i;

	memset(def, 0, sizeof(*def));
	def->vbr_quality = -1;
	def->kind = axis_gen_pick(in, 2) == 0 ? AXIS_PROFILE_KIND_VIDEO : AXIS_PROFILE_KIND_AUDIO;
	def->min_saving_percent = AXIS_PROFILE_MIN_SAVING_FLOOR +
		(gint)axis_gen_pick(in, AXIS_PROFILE_MAX_SAVING - AXIS_PROFILE_MIN_SAVING_FLOOR + 1);
	def->min_saving_bytes = axis_gen_pick(in, 3) == 0 ? (gint64)axis_gen_byte(in) * 100000000 : 0;
	if (def->kind == AXIS_PROFILE_KIND_VIDEO) {
		guint mask = 0;

		while (mask == 0) {
			if (axis_gen_pick(in, 2))
				mask |= AXIS_TARGET_ROOT(AXIS_MEDIA_MOVIES);
			if (axis_gen_pick(in, 2))
				mask |= AXIS_TARGET_ROOT(AXIS_MEDIA_TV);
			if (axis_gen_pick(in, 2))
				mask |= AXIS_TARGET_ROOT(AXIS_MEDIA_OTHER);
			if (mask == 0 && in->at >= in->size && in->rand == NULL)
				mask = AXIS_TARGET_ROOT(AXIS_MEDIA_MOVIES);
		}
		def->libraries = mask;
		def->codec = axis_gen_pick(in, 2) == 0 ? AXIS_TCODEC_HEVC : AXIS_TCODEC_H264;
		def->bit_depth = def->codec == AXIS_TCODEC_HEVC && axis_gen_pick(in, 2) ? 10 : 8;
		def->keep_hdr = def->codec == AXIS_TCODEC_HEVC && axis_gen_pick(in, 2);
		def->n_encoders = 1 + axis_gen_pick(in, AXIS_ENCODER_COUNT);
		{
			AxisEncoderFamily pool[AXIS_ENCODER_COUNT] = {
				AXIS_ENCODER_SOFTWARE, AXIS_ENCODER_NVENC, AXIS_ENCODER_VAAPI
			};
			guint left = AXIS_ENCODER_COUNT;

			for (i = 0; i < def->n_encoders; i++) {
				guint at = axis_gen_pick(in, left);

				def->encoders[i] = pool[at];
				pool[at] = pool[left - 1];
				left--;
			}
		}
		crfs = axis_profile_crf_values(def->codec);
		while (crfs[n_crfs] != 0)
			n_crfs++;
		def->crf = crfs[axis_gen_pick(in, n_crfs)];
		{
			guint wanted = axis_gen_pick(in, AXIS_PROFILE_MAX_RULES + 1);

			for (i = 0; i < wanted; i++) {
				AxisProfileRule rule;
				guint j;
				gboolean twice = FALSE;

				rule.resolution = (gint)axis_gen_pick(in, 6);
				rule.min_bytes = axis_gen_sizes[axis_gen_pick(in, G_N_ELEMENTS(axis_gen_sizes))];
				rule.max_bytes = axis_gen_sizes[axis_gen_pick(in, G_N_ELEMENTS(axis_gen_sizes))];
				if (rule.min_bytes > 0 && rule.max_bytes > 0 && rule.min_bytes >= rule.max_bytes)
					rule.max_bytes = 0;
				if (rule.resolution == 0 && rule.min_bytes == 0 && rule.max_bytes == 0)
					rule.resolution = 4;
				rule.crf = crfs[axis_gen_pick(in, n_crfs)];
				for (j = 0; j < def->n_rules; j++) {
					if (def->rules[j].resolution == rule.resolution && def->rules[j].min_bytes == rule.min_bytes &&
					    def->rules[j].max_bytes == rule.max_bytes && def->rules[j].crf == rule.crf)
						twice = TRUE;
				}
				if (!twice)
					def->rules[def->n_rules++] = rule;
			}
		}
		def->audio = (AxisAudioHandling)axis_gen_pick(in, 3);
		if (def->audio != AXIS_AUDIO_COPY)
			def->stereo_bitrate = 96000 + 1000 * (gint)axis_gen_pick(in, 225);
		{
			guint wanted = axis_gen_pick(in, 4);
			guint used = 0;

			for (i = 0; i < wanted; i++) {
				guint at = axis_gen_pick(in, G_N_ELEMENTS(languages));

				if ((used & (1u << at)) != 0)
					continue;
				used |= 1u << at;
				memcpy(def->languages[def->n_languages++], languages[at], 4);
			}
		}
		def->container = axis_gen_pick(in, 2) == 0 ? AXIS_CONTAINER_MATROSKA : AXIS_CONTAINER_MP4;
		def->subtitles = def->container == AXIS_CONTAINER_MP4 || axis_gen_pick(in, 2) ? AXIS_SUBTITLES_PLAIN_TEXT :
			AXIS_SUBTITLES_COPY;
		def->efficient_milli = 10 + (gint)(axis_gen_byte(in) * 4 + axis_gen_pick(in, 4)) % 991;
	} else {
		guint mask = 0;

		while (mask == 0) {
			if (axis_gen_pick(in, 2))
				mask |= AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC);
			if (axis_gen_pick(in, 2))
				mask |= AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS);
			if (mask == 0 && in->at >= in->size && in->rand == NULL)
				mask = AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC);
		}
		def->libraries = mask;
		def->codec = audio_codecs[axis_gen_pick(in, G_N_ELEMENTS(audio_codecs))];
		switch (def->codec) {
		case AXIS_TCODEC_FLAC:
			def->reduce_to_cd = axis_gen_pick(in, 2);
			break;
		case AXIS_TCODEC_MP3:
			def->vbr_quality = (gint)axis_gen_pick(in, 5);
			break;
		default: {
			gint low = def->codec == AXIS_TCODEC_OPUS ? 16000 : 32000;
			gint high = def->codec == AXIS_TCODEC_OPUS ? 256000 : 320000;

			def->bitrate = low + 1000 * (gint)(((guint)axis_gen_byte(in) << 8 | axis_gen_byte(in)) %
				(guint)((high - low) / 1000 + 1));
			if (axis_gen_pick(in, 2) && def->bitrate > low)
				def->mono_bitrate = low + (gint)(((guint)axis_gen_byte(in) << 8 | axis_gen_byte(in)) %
					(guint)(def->bitrate - low));
			break;
		}
		}
		if (def->codec != AXIS_TCODEC_FLAC) {
			def->allow_lossy = axis_gen_pick(in, 2);
			def->efficient_percent = 100 + (gint)(((guint)axis_gen_byte(in) << 8 | axis_gen_byte(in)) % 301);
		}
	}
	if (!axis_profile_definition_check(def, problems)) {
		const AxisProfileProblem *p = problems->len > 0 ? g_ptr_array_index(problems, 0) : NULL;

		g_printerr("profile generator: made an invalid definition: %s: %s\n", p != NULL ? p->field : "?",
			p != NULL ? p->message : "?");
		abort();
	}
}

/* ── Arbitrary files ────────────────────────────────────────────── */

static G_GNUC_UNUSED gint64
axis_gen_number(AxisGenInput *in)
{
	switch (axis_gen_byte(in) % 8) {
	case 0:
		return -1;
	case 1:
		return 0;
	case 2:
		return G_MAXINT64;
	case 3:
		return AXIS_PROBE_MAX_BITRATE;
	case 4:
		return (gint64)axis_gen_byte(in) << 40;
	default: {
		gint64 value = 0;
		guint i;

		for (i = 0; i < 4; i++)
			value = (value << 8) | axis_gen_byte(in);
		return value;
	}
	}
}

static G_GNUC_UNUSED gdouble
axis_gen_real(AxisGenInput *in)
{
	switch (axis_gen_byte(in) % 8) {
	case 0:
		return -1.0;
	case 1:
		return NAN;
	case 2:
		return INFINITY;
	case 3:
		return 0.0;
	case 4:
		return 1e300;
	default:
		return (gdouble)axis_gen_byte(in) + (gdouble)axis_gen_byte(in) / 256.0;
	}
}

static G_GNUC_UNUSED gchar *
axis_gen_text(AxisGenInput *in)
{
	static const gchar *const known[] = {
		"h264", "hevc", "av1", "vp9", "mpeg2video", "flac", "mp3", "aac", "opus", "eac3", "truehd", "pcm_s24le",
		"subrip", "hdmv_pgs_subtitle", "mov_text", "matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2", "ogg",
		"progressive", "tt", "bb", "eng", "fre", "und", "ok", "failed", NULL
	};
	guint8 pick = axis_gen_byte(in);

	if (pick < 220)
		return g_strdup(known[pick % (G_N_ELEMENTS(known) - 1)]);
	if (pick < 240)
		return NULL;
	return g_strdup("sneeze");
}

/* A probe record with streams of any kind and any facts (the fuzz-estimate
 * generator's), and the index facts beside it; mostly "fresh" so that
 * decisions get past the probe's checks. */
static G_GNUC_UNUSED AxisMediaInfo *
axis_gen_file(
	AxisGenInput     *in,
	AxisEstimateFile *file
){
	AxisMediaInfo *info = axis_gen_byte(in) % 8 == 0 ? NULL : axis_media_info_new();
	AxisEstimateIndex *idx = &file->index;
	guint i;

	memset(file, 0, sizeof(*file));
	if (info != NULL) {
		guint n = axis_gen_byte(in) % 6;

		info->status = axis_gen_byte(in) % 8 == 0 ? g_strdup("failed") : g_strdup("ok");
		info->probe_version = axis_gen_byte(in) % 8 == 0 ? AXIS_PROBE_VERSION - 1 : AXIS_PROBE_VERSION;
		info->container = axis_gen_text(in);
		info->duration_ms = axis_gen_number(in);
		info->bitrate = axis_gen_number(in);
		info->hdr_format = (AxisHdrFormat)(axis_gen_byte(in) % 8);
		info->hdr_uncertain = axis_gen_byte(in) % 4 == 0;
		info->interlaced = (gint)(axis_gen_byte(in) % 3) - 1;
		info->bits_per_pixel = axis_gen_real(in);
		info->streams_omitted = axis_gen_byte(in) % 8 == 0;
		info->resolution_class = axis_gen_byte(in) % 2 ? "2160p" : "1080p";
		info->video_codec = axis_gen_text(in);
		for (i = 0; i < n; i++) {
			AxisMediaStream *s = g_new0(AxisMediaStream, 1);

			s->index = (gint)i;
			s->kind = (AxisStreamKind)(axis_gen_byte(in) % 6);
			s->codec = axis_gen_text(in);
			s->bitrate = axis_gen_number(in);
			s->bitrate_estimated = axis_gen_byte(in) % 2;
			s->duration_ms = axis_gen_number(in);
			s->width = (gint)axis_gen_number(in);
			s->height = (gint)axis_gen_number(in);
			s->frame_rate = axis_gen_real(in);
			s->real_frame_rate = axis_gen_real(in);
			s->hdr_format = (AxisHdrFormat)(axis_gen_byte(in) % 8);
			s->hdr_uncertain = axis_gen_byte(in) % 4 == 0;
			s->attached_pic = axis_gen_byte(in) % 4 == 0;
			s->channels = (gint)axis_gen_number(in);
			s->sample_rate = (gint)axis_gen_number(in);
			s->bit_depth_audio = (gint)axis_gen_number(in);
			s->lossless = (gint)(axis_gen_byte(in) % 3) - 1;
			s->is_default = axis_gen_byte(in) % 2;
			s->subtitle_kind = (AxisSubtitleKind)(axis_gen_byte(in) % 3);
			s->field_order = axis_gen_text(in);
			s->language = axis_gen_text(in);
			g_ptr_array_add(info->streams, s);
		}
	}
	idx->file_id = 1;
	idx->root_id = 1;
	idx->kind = axis_gen_byte(in) % 2 ? "video" : "audio";
	idx->root_type = (AxisMediaType)(axis_gen_byte(in) % (AXIS_MEDIA_TYPE_COUNT + 1));
	idx->state = axis_gen_byte(in) % 8 == 0 ? "missing" : "present";
	idx->size = MAX(axis_gen_number(in), 0);
	idx->nlink = axis_gen_byte(in) % 8 == 0 ? 2 : 1;
	idx->pinned = axis_gen_byte(in) % 8 == 0;
	idx->changed = axis_gen_byte(in) % 8 == 0;
	idx->volume_usable = axis_gen_byte(in) % 4 != 0;
	idx->unit = "Artist/Album";
	idx->unit_files = 1;
	if (info != NULL) {
		/* Sometimes a probe of another size: one byte off, either way, never
		 * past the largest number. */
		info->file_size = axis_gen_byte(in) % 8 != 0 ? idx->size : idx->size < G_MAXINT64 ? idx->size + 1 :
			idx->size - 1;
		info->mtime_sec = idx->mtime_sec = 1700000000;
		info->inode = idx->inode = 7;
	}
	file->info = info;
	return info;
}

/* ── The rules no definition may get past ───────────────────────── */

static G_GNUC_UNUSED gboolean
axis_gen_field_based(const gchar *order)
{
	return g_strcmp0(order, "tt") == 0 || g_strcmp0(order, "bb") == 0 || g_strcmp0(order, "tb") == 0 ||
		g_strcmp0(order, "bt") == 0;
}

/*
 * The first fixed rule that covers @file for @target, worked out from the
 * facts without the estimator: %AXIS_ESTIMATE_ELIGIBLE when none does, and
 * %AXIS_ESTIMATE_NOT_APPLICABLE or %AXIS_ESTIMATE_NOT_PRESENT when the
 * target never gets as far as the rules.
 */
static G_GNUC_UNUSED AxisEstimateReason
axis_gen_fixed_rule(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target
){
	const AxisEstimateIndex *idx = &file->index;
	const AxisMediaInfo *info = file->info;
	gboolean video = target->media == AXIS_TARGET_VIDEO;
	const AxisMediaStream *main = NULL;
	gint64 best = -1;
	AxisHdrFormat worst;
	gboolean uncertain;
	guint i;

	if (g_strcmp0(idx->kind, video ? "video" : "audio") != 0 || (guint)idx->root_type >= AXIS_MEDIA_TYPE_COUNT ||
	    (target->root_types & AXIS_TARGET_ROOT(idx->root_type)) == 0)
		return AXIS_ESTIMATE_NOT_APPLICABLE;
	if (g_strcmp0(idx->state, "present") != 0)
		return AXIS_ESTIMATE_NOT_PRESENT;
	/* Pinned: hard-linked or under a downloads root. */
	if (idx->pinned || idx->nlink != 1)
		return AXIS_ESTIMATE_PINNED;
	/* Facts missing: never probed, failed, or describing other content. */
	if (info == NULL)
		return AXIS_ESTIMATE_NOT_PROBED;
	if (g_strcmp0(info->status, "ok") != 0)
		return AXIS_ESTIMATE_PROBE_FAILED;
	if (idx->changed || info->probe_version < AXIS_PROBE_VERSION || info->streams == NULL ||
	    info->file_size != idx->size || info->mtime_sec != idx->mtime_sec || info->mtime_nsec != idx->mtime_nsec ||
	    info->ctime_sec != idx->ctime_sec || info->ctime_nsec != idx->ctime_nsec || info->inode != idx->inode)
		return AXIS_ESTIMATE_PROBE_STALE;
	if (!video)
		return AXIS_ESTIMATE_ELIGIBLE;

	worst = info->hdr_format;
	uncertain = info->hdr_uncertain || info->hdr_format == AXIS_HDR_NONE;
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		gint64 pixels;

		if (s->kind != AXIS_STREAM_VIDEO || s->attached_pic)
			continue;
		if (s->hdr_uncertain || s->hdr_format == AXIS_HDR_NONE)
			uncertain = TRUE;
		if (s->hdr_format > worst)
			worst = s->hdr_format;
		pixels = s->width > 0 && s->height > 0 ? (gint64)s->width * s->height : 0;
		if (pixels > best) {
			best = pixels;
			main = s;
		}
	}
	/* A file without a picture is refused later (its layout). */
	if (main == NULL)
		return AXIS_ESTIMATE_ELIGIBLE;
	/* Uncertain or unclassified HDR, Dolby Vision, HDR10+. */
	if (uncertain || (guint)worst > AXIS_HDR_DOLBY_VISION)
		return AXIS_ESTIMATE_HDR_UNCERTAIN;
	if (worst == AXIS_HDR_DOLBY_VISION)
		return AXIS_ESTIMATE_DOLBY_VISION;
	if (worst == AXIS_HDR_HDR10PLUS)
		return AXIS_ESTIMATE_HDR10PLUS;
	/* An HDR source is never made SDR: only 10-bit HEVC that keeps it. */
	if ((worst == AXIS_HDR_HDR10 || worst == AXIS_HDR_HLG) &&
	    !(target->codec == AXIS_TCODEC_HEVC && target->hdr_bit_depth >= 10))
		return AXIS_ESTIMATE_HDR_TO_SDR;
	/* Interlaced, or not known to be progressive. */
	if (info->interlaced == AXIS_TRI_YES || axis_gen_field_based(main->field_order))
		return AXIS_ESTIMATE_INTERLACED;
	if (info->interlaced != AXIS_TRI_NO && g_strcmp0(main->field_order, "progressive") != 0)
		return AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN;
	return AXIS_ESTIMATE_ELIGIBLE;
}

static G_GNUC_NORETURN void
axis_gen_broken(
	const gchar              *what,
	const AxisEstimateTarget *target,
	const AxisEstimateResult *r
){
	g_printerr("profile property: %s: target=%s codec=%s hdr_bit_depth=%d min_saving=%.3f reason=%s "
		"now=%" G_GINT64_FORMAT " mid=%" G_GINT64_FORMAT "\n", what, target->id,
		axis_target_codec_to_string(target->codec), target->hdr_bit_depth, target->min_saving,
		axis_estimate_reason_to_string(r->reason), r->now, r->mid);
	abort();
}

/*
 * The decision @r for @file agrees with the fixed rules: when one covers
 * the file it is that rule's reason (or an earlier one: not applicable,
 * not present), never "would be converted"; a file that converts alone is
 * estimated strictly smaller, by at least the floor, from known facts.
 * @member: @r is one file of an album of several (whose saving is the
 * album's, checked by axis_gen_check_album()).
 */
static G_GNUC_UNUSED void
axis_gen_check_decision(
	const AxisEstimateFile   *file,
	const AxisEstimateTarget *target,
	const AxisEstimateResult *r,
	gboolean                  member
){
	AxisEstimateReason rule = axis_gen_fixed_rule(file, target);

	if (rule != AXIS_ESTIMATE_ELIGIBLE) {
		if (axis_estimate_result_converts(r))
			axis_gen_broken("a file a fixed rule covers would be converted", target, r);
		if (r->reason != rule)
			axis_gen_broken(rule == AXIS_ESTIMATE_PINNED ? "a pinned file did not get its reason" :
				rule == AXIS_ESTIMATE_DOLBY_VISION ? "Dolby Vision did not get its reason" :
				rule == AXIS_ESTIMATE_HDR_UNCERTAIN ? "uncertain HDR did not get its reason" :
				rule == AXIS_ESTIMATE_HDR10PLUS ? "HDR10+ did not get its reason" :
				rule == AXIS_ESTIMATE_HDR_TO_SDR ? "an HDR source for an SDR target did not get its reason" :
				rule == AXIS_ESTIMATE_INTERLACED ? "interlaced video did not get its reason" :
				rule == AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN ? "possibly interlaced video did not get its reason" :
				"a file whose facts are missing did not get the reason", target, r);
	}
	if (!axis_estimate_result_converts(r))
		return;
	if (!r->sized || r->now <= 0)
		axis_gen_broken("would be converted without an estimate", target, r);
	if (!member) {
		if (r->mid >= r->now)
			axis_gen_broken("would be converted without being estimated strictly smaller", target, r);
		if ((gdouble)r->now - (gdouble)r->mid < AXIS_ESTIMATE_MIN_SAVING_FLOOR * (gdouble)r->now)
			axis_gen_broken("would be converted for less than the floor's saving", target, r);
	}
	/* The facts the estimate is made from are known, and add up. */
	if (target->media == AXIS_TARGET_VIDEO && file->info != NULL) {
		const AxisMediaInfo *info = file->info;
		const AxisMediaStream *main = NULL;
		gint64 best = -1;
		gdouble seconds = info->duration_ms > 0 && info->duration_ms <= AXIS_PROBE_MAX_DURATION_MS ?
			(gdouble)info->duration_ms / 1000.0 : -1.0;
		guint i;

		for (i = 0; info->streams != NULL && i < info->streams->len; i++) {
			const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
			gint64 pixels = s->width > 0 && s->height > 0 ? (gint64)s->width * s->height : 0;

			if (s->kind == AXIS_STREAM_VIDEO && !s->attached_pic && pixels > best) {
				best = pixels;
				main = s;
			}
		}
		if (main == NULL)
			axis_gen_broken("a file without a picture would be converted", target, r);
		if (seconds <= 0 && main->duration_ms > 0 && main->duration_ms <= AXIS_PROBE_MAX_DURATION_MS)
			seconds = (gdouble)main->duration_ms / 1000.0;
		if (seconds <= 0 || main->width <= 0 || main->height <= 0 || main->bitrate <= 0 ||
		    main->bitrate > AXIS_PROBE_MAX_BITRATE)
			axis_gen_broken("a file with facts missing would be converted", target, r);
		if ((gdouble)main->bitrate * seconds / 8.0 > (gdouble)file->index.size * 1.05)
			axis_gen_broken("a file whose facts do not add up would be converted", target, r);
	}
}

/* An album that converts is estimated strictly smaller as a whole, by at
 * least the floor; and it converts whole or not at all. */
static G_GNUC_UNUSED void
axis_gen_check_album(
	const AxisEstimateTarget *target,
	const AxisEstimateResult *results,
	guint                     n
){
	gdouble now = 0;
	gdouble mid = 0;
	guint converting = 0;
	guint usable = 0;
	guint i;

	for (i = 0; i < n; i++) {
		if (axis_estimate_result_converts(&results[i])) {
			converting++;
			now += (gdouble)results[i].now;
			mid += (gdouble)results[i].mid;
		}
		if (results[i].reason == AXIS_ESTIMATE_ELIGIBLE || results[i].reason == AXIS_ESTIMATE_OFFLINE ||
		    results[i].reason == AXIS_ESTIMATE_ALBUM)
			usable++;
	}
	if (converting == 0)
		return;
	if (converting != n)
		axis_gen_broken("an album would be converted in part", target, &results[0]);
	if (mid >= now)
		axis_gen_broken("an album would be converted without being estimated strictly smaller", target,
			&results[0]);
	if (now - mid < AXIS_ESTIMATE_MIN_SAVING_FLOOR * now)
		axis_gen_broken("an album would be converted for less than the floor's saving", target, &results[0]);
	(void)usable;
}

#endif /* AXIS_FUZZ_PROFILE_GEN_H */
