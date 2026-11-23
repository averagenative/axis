/*
 * fuzz-estimate.c - libFuzzer target for the estimator's decision and size
 * model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The estimator parses nothing, but it decides from facts that come from
 * outside (ffprobe's reading of a file): the input is turned into a probe
 * record (streams of any kind, codecs from a list and from the bytes, every
 * number from the whole range, including NaN and infinities), index facts
 * and a target (sometimes with a language rule or the CD reduction), and a
 * unit of up to four files.  Whatever it is:
 *
 *  - the decision is one of the reasons;
 *  - every figure is a whole number, low <= middle <= high, never
 *    negative, and an estimate is never above the file's size;
 *  - a file that converts has an estimate with a middle below its size;
 *  - a total of them keeps low <= middle <= high <= its size;
 *  - half of the time the target is made from an arbitrary valid profile
 *    definition (fuzz-profile-gen.h) instead of a built-in one, and with
 *    any target, a file that a rule no profile can change covers (pinned,
 *    Dolby Vision, uncertain HDR, HDR10+, HDR to SDR, interlaced or
 *    possibly so, facts missing) is never eligible and has that rule's
 *    reason, and what converts saves at least the floor.
 */

#include <glib.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz-profile-gen.h"
#include "transcode/axis-estimate.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct
{
	const uint8_t *data;
	size_t         size;
	size_t         at;
} Input;

static guint8
byte(Input *in)
{
	return in->at < in->size ? in->data[in->at++] : 0;
}

static gint64
number(Input *in)
{
	switch (byte(in) % 8) {
	case 0:
		return -1;
	case 1:
		return 0;
	case 2:
		return G_MAXINT64;
	case 3:
		return AXIS_PROBE_MAX_BITRATE;
	case 4:
		return (gint64)byte(in) << 40;
	default: {
		gint64 value = 0;
		guint i;

		for (i = 0; i < 4; i++)
			value = (value << 8) | byte(in);
		return value;
	}
	}
}

static gdouble
real(Input *in)
{
	switch (byte(in) % 8) {
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
		return (gdouble)byte(in) + (gdouble)byte(in) / 256.0;
	}
}

static gchar *
text(Input *in)
{
	static const gchar *const known[] = {
		"h264", "hevc", "av1", "vp9", "mpeg2video", "prores", "flac", "mp3", "aac", "opus", "eac3", "truehd",
		"pcm_s24le", "alac", "ape", "subrip", "ass", "hdmv_pgs_subtitle", "mov_text", "matroska,webm",
		"mov,mp4,m4a,3gp,3g2,mj2", "flac", "ogg", "progressive", "tt", "eng", "fre", "und", "ok", "failed", NULL
	};
	guint8 pick = byte(in);

	if (pick < 200)
		return g_strdup(known[pick % (G_N_ELEMENTS(known) - 1)]);
	if (pick < 230)
		return NULL;
	{
		guint8 length = byte(in) % 12;
		gchar *out = g_malloc0(length + 1);
		guint i;

		for (i = 0; i < length; i++)
			out[i] = (gchar)(byte(in) | 1);
		return out;
	}
}

static AxisMediaInfo *
record(Input *in)
{
	AxisMediaInfo *info = axis_media_info_new();
	guint n = byte(in) % 7;
	guint i;

	info->status = text(in);
	info->probe_version = (gint)(byte(in) % 3);
	info->container = text(in);
	info->duration_ms = number(in);
	info->bitrate = number(in);
	info->file_size = number(in);
	info->hdr_format = (AxisHdrFormat)(byte(in) % 8);
	info->hdr_uncertain = byte(in) % 2;
	info->interlaced = (gint)(byte(in) % 3) - 1;
	info->bits_per_pixel = real(in);
	info->streams_omitted = byte(in) % 4 == 0;
	info->has_embedded_art = byte(in) % 2;
	info->art_width = (gint)number(in);
	info->art_height = (gint)number(in);
	info->resolution_class = byte(in) % 2 ? "2160p" : NULL;
	info->video_codec = text(in);
	for (i = 0; i < n; i++) {
		AxisMediaStream *s = g_new0(AxisMediaStream, 1);

		s->index = (gint)i;
		s->kind = (AxisStreamKind)(byte(in) % 7);
		s->codec = text(in);
		s->bitrate = number(in);
		s->bitrate_estimated = byte(in) % 2;
		s->duration_ms = number(in);
		s->width = (gint)number(in);
		s->height = (gint)number(in);
		s->frame_rate = real(in);
		s->real_frame_rate = real(in);
		s->hdr_format = (AxisHdrFormat)(byte(in) % 8);
		s->hdr_uncertain = byte(in) % 2;
		s->attached_pic = byte(in) % 4 == 0;
		s->channels = (gint)number(in);
		s->sample_rate = (gint)number(in);
		s->bit_depth_audio = (gint)number(in);
		s->lossless = (gint)(byte(in) % 3) - 1;
		s->is_default = byte(in) % 2;
		s->subtitle_kind = (AxisSubtitleKind)(byte(in) % 4);
		s->field_order = text(in);
		s->language = text(in);
		g_ptr_array_add(info->streams, s);
	}
	return info;
}

/*
 * @member: the result is one file of a unit of several (an album).  Such
 * a file may be estimated at its own size, since an album is never left
 * half converted; it is the unit as a whole that must be smaller, which
 * the caller checks.
 */
static void
check(const AxisEstimateResult *r, gboolean member)
{
	/* Say which property broke and with what figures: a bare abort leaves
	 * nothing to go on. */
	const gchar *broken = NULL;

	if ((guint)r->reason >= AXIS_ESTIMATE_N_REASONS)
		broken = "the reason is not one of the list";
	else if (r->now < 0 || r->low < 0 || r->low > r->mid || r->mid > r->high)
		broken = "not 0 <= low <= mid <= high, or a negative size";
	else if (r->sized && r->high > r->now)
		broken = "high is above the file's size";
	else if (axis_estimate_result_converts(r) && !r->sized)
		broken = "would be converted without an estimate";
	else if (axis_estimate_result_converts(r) && !member && r->mid >= r->now)
		broken = "would be converted without being estimated smaller";

	if (broken != NULL) {
		g_printerr("fuzz-estimate: %s: reason=%d sized=%d now=%" G_GINT64_FORMAT " low=%" G_GINT64_FORMAT
			" mid=%" G_GINT64_FORMAT " high=%" G_GINT64_FORMAT "\n", broken, (gint)r->reason, (gint)r->sized,
			r->now, r->low, r->mid, r->high);
		abort();
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const gchar *const english[] = { "eng", NULL };
	static AxisEstimateModel *model;
	Input in = { data, size, 0 };
	AxisEstimateTarget target;
	AxisMediaInfo *infos[4];
	AxisEstimateFile files[4];
	const AxisEstimateFile *unit[4];
	AxisEstimateResult results[4];
	AxisEstimateTotal *total;
	gint64 n_total = 0;
	gint64 now = 0;
	gint64 low = 0;
	gint64 mid = 0;
	gint64 high = 0;
	guint n;
	guint i;

	if (model == NULL) {
		AxisEstimateSample sample;

		/* A model with some calibration, so that the calibrated table is
		 * exercised too. */
		model = axis_estimate_model_new();
		memset(&sample, 0, sizeof(sample));
		sample.target = "space_saver";
		sample.codec = AXIS_TCODEC_HEVC;
		sample.quality = 4;
		sample.part = "video";
		sample.source_codec = "h264";
		sample.width = 1920;
		sample.height = 1080;
		sample.frame_rate = 24.0;
		sample.source_bitrate = 8000000;
		sample.seconds = 60.0;
		for (i = 0; i < 30; i++) {
			sample.output_bitrate = 500000 + 100000 * (i % 7);
			axis_estimate_model_add_sample(model, &sample);
		}
		axis_estimate_model_finish(model);
	}

	if (byte(&in) % 2 == 0) {
		AxisGenInput gen = { data, size, in.at, NULL };
		AxisProfileDefinition def;
		AxisEstimateTarget built;
		AxisEstimateConfig config;

		axis_gen_definition(&gen, &def);
		in.at = gen.at;
		if (!axis_profile_to_target(&def, NULL, &built)) {
			g_printerr("fuzz-estimate: no target from a valid definition\n");
			abort();
		}
		axis_estimate_config_init(&config);
		config.encoder = (AxisEncoderFamily)(byte(&in) % AXIS_ENCODER_COUNT);
		axis_estimate_target_configure(&built, &config, &target);
	} else {
		target = axis_estimate_builtin_targets()[byte(&in) % AXIS_ESTIMATE_N_TARGETS];
		if (byte(&in) % 4 == 0)
			target.keep_audio_languages = english;
		if (byte(&in) % 4 == 0)
			target.reduce_to_cd = TRUE;
		target.quality = 1 + byte(&in) % 7;
		target.encoder = (AxisEncoderFamily)(byte(&in) % AXIS_ENCODER_COUNT);
	}
	n = 1 + byte(&in) % 4;

	for (i = 0; i < n; i++) {
		AxisEstimateIndex *idx = &files[i].index;

		infos[i] = byte(&in) % 8 == 0 ? NULL : record(&in);
		memset(&files[i], 0, sizeof(files[i]));
		idx->file_id = i + 1;
		idx->root_id = 1;
		idx->kind = byte(&in) % 2 ? "video" : "audio";
		idx->root_type = (AxisMediaType)(byte(&in) % (AXIS_MEDIA_TYPE_COUNT + 1));
		idx->state = byte(&in) % 8 == 0 ? "missing" : "present";
		idx->size = MAX(number(&in), 0);
		idx->nlink = byte(&in) % 8 == 0 ? 2 : 1;
		idx->pinned = byte(&in) % 8 == 0;
		idx->changed = byte(&in) % 8 == 0;
		idx->volume_usable = byte(&in) % 4 != 0;
		idx->unit = byte(&in) % 4 == 0 ? NULL : "Artist/Album";
		idx->unit_files = byte(&in) % 6;
		if (infos[i] != NULL) {
			/* Mostly the record's stamp, so that decisions get past it. */
			if (byte(&in) % 4 != 0)
				infos[i]->file_size = idx->size;
			idx->mtime_sec = infos[i]->mtime_sec;
			idx->mtime_nsec = infos[i]->mtime_nsec;
			idx->ctime_sec = infos[i]->ctime_sec;
			idx->ctime_nsec = infos[i]->ctime_nsec;
			idx->inode = infos[i]->inode;
		}
		files[i].info = infos[i];
		unit[i] = &files[i];
	}

	axis_estimate_evaluate(unit, n, &target, model, results);
	total = axis_estimate_total_new();
	for (i = 0; i < n; i++) {
		check(&results[i], n > 1 && target.media == AXIS_TARGET_AUDIO);
		axis_gen_check_decision(&files[i], &target, &results[i], n > 1 && target.media == AXIS_TARGET_AUDIO);
		if (axis_estimate_result_converts(&results[i]))
			axis_estimate_total_add(total, model, &results[i]);
	}
	if (target.media == AXIS_TARGET_AUDIO)
		axis_gen_check_album(&target, results, n);
	axis_estimate_total_get(total, &n_total, &now, &low, &mid, &high);
	if (low < 0 || low > mid || mid > high || high > now) {
		g_printerr("fuzz-estimate: total out of order: now=%" G_GINT64_FORMAT " low=%" G_GINT64_FORMAT
			" mid=%" G_GINT64_FORMAT " high=%" G_GINT64_FORMAT "\n", now, low, mid, high);
		abort();
	}
	/* Whatever is converted, taken together, is estimated strictly
	 * smaller than it is now. */
	if (n_total > 0 && mid >= now) {
		g_printerr("fuzz-estimate: %" G_GINT64_FORMAT " files would be converted for no saving: now=%"
			G_GINT64_FORMAT " mid=%" G_GINT64_FORMAT "\n", n_total, now, mid);
		abort();
	}
	axis_estimate_total_free(total);

	/* One file alone, as a conversion would ask. */
	{
		AxisEstimateResult one;

		axis_estimate_eligibility(&files[0], &target, model, &one);
		check(&one, FALSE);
		axis_gen_check_decision(&files[0], &target, &one, FALSE);
	}

	for (i = 0; i < n; i++)
		axis_media_info_free(infos[i]);
	return 0;
}
