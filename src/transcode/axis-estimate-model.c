/*
 * axis-estimate-model.c - the size model: priors, calibration and the
 * table the estimates read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * PRIORS.  Every number in the tables below is a prior: an informed guess
 * made before any file of this library was converted, from the author's
 * reasoning about how the encoders typically behave on films and series.
 * None of them was measured for this project, and none comes from a
 * published table.  They are why the pages say "uncalibrated" and why the
 * ranges are wide; the calibration (real conversions, and the measurement
 * job's excerpts) replaces them cell by cell.
 *
 * The video model.  The bitrate of the output picture is
 *
 *   pixels per second x target bits per pixel x complexity
 *
 * where the target bits per pixel come from a table keyed by the output
 * codec, the encoder family, the quality level and the picture size, and
 * the complexity is a damped ratio of the source's own bits per pixel to
 * what is typical for its size.  The table is given at four picture sizes
 * (anchors) and interpolated between them on a log-log scale, so that a
 * picture of any size has a figure and the figure changes smoothly with it.
 * Each table cell carries a spread; the estimate is a low, middle and high
 * figure.  After calibration the table is made monotonic: a higher quality
 * level never has fewer bits per pixel, and a larger anchor never has
 * fewer bits per frame.
 *
 * GLib only, no I/O.
 */

#include "transcode/axis-estimate-private.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Anchors ────────────────────────────────────────────────────── */

/* SD (720x480), 720p, 1080p, 2160p: pixels per frame. */
static const gdouble anchor_pixels[AXIS_ESTIMATE_N_ANCHORS] = { 345600.0, 921600.0, 2073600.0, 8294400.0 };
static const gchar *const anchor_names[AXIS_ESTIMATE_N_ANCHORS] = { "sd", "720p", "1080p", "2160p" };

/* ── Video priors ───────────────────────────────────────────────── */

/*
 * Target bits per pixel at quality level 4, software encoder, film and
 * series content.  Group: x265 (preset medium), CRF 26, 10-bit.  The
 * author's reasoning: x265 at CRF 26 typically lands near 2 to 3 Mbit/s
 * for 1080p24 films, 5 to 9 Mbit/s for 2160p24, about 1.2 Mbit/s for
 * 720p24 and under 1 Mbit/s for SD; bits per pixel fall as the picture
 * grows because larger pictures are more predictable.  The four figures
 * were chosen so that bits per frame still grow with the picture.
 */
static const gdouble prior_hevc[AXIS_ESTIMATE_N_ANCHORS] = { 0.080, 0.054, 0.045, 0.035 };

/*
 * Group: x264 (preset medium), CRF 22, 8-bit High profile.  The author's
 * reasoning: x264 needs roughly half again to twice the bits of x265 for a
 * similar picture; CRF 22 lands near 4 to 5 Mbit/s for 1080p24 films.
 */
static const gdouble prior_h264[AXIS_ESTIMATE_N_ANCHORS] = { 0.150, 0.105, 0.090, 0.070 };

/* The CRF (or CQ, QP) the figures above are given at. */
#define PRIOR_REF_HEVC 26.0
#define PRIOR_REF_H264 22.0

/*
 * Group: quality levels.  The rule of thumb for x264 and x265 that six CRF
 * steps double or halve the bitrate (the author's reasoning, not
 * measured); the hardware encoders are given the same slope for their CQ
 * and QP numbers.
 */
#define CRF_DOUBLING_STEPS 6.0

/*
 * Group: encoder families.  Hardware encoders make larger files than the
 * software ones at a similar picture: the author's reasoning puts NVENC
 * (of the Turing generation) about 1.3 times x265 and 1.25 times x264,
 * VA-API about 1.4 times; the spreads are wider because these depend on
 * the generation of the hardware.
 */
static const gdouble family_factor[2][AXIS_ENCODER_COUNT] = {
	{ 1.00, 1.30, 1.40 },	/* HEVC */
	{ 1.00, 1.25, 1.35 }	/* H.264 */
};
static const gdouble family_sigma[AXIS_ENCODER_COUNT] = {
	0.47,	/* ln 1.6: low and high are the middle divided and multiplied by 1.6 */
	0.53,	/* ln 1.7 */
	0.59	/* ln 1.8 */
};

/*
 * Group: bit depth.  An 8-bit HEVC encode is given 5 % more bits than a
 * 10-bit one at the same CRF (the author's reasoning: 10-bit encoding is
 * commonly reported as slightly more efficient).
 */
#define EIGHT_BIT_HEVC_FACTOR 1.05

/*
 * Group: the complexity factor.  The source's bits per pixel, counted in
 * H.264 bits (efficiency table below), over what is typical for a library
 * file of that picture size (reference below); square-rooted, because the
 * source bitrate is a weak proxy for how hard a picture is to encode (an
 * over-compressed source looks simple, a remux looks complex), and kept
 * within 0.6 to 1.6.  The reference figures are the author's guesses at
 * typical library files (web downloads and rips more than remuxes).
 */
static const gdouble reference_bpp[AXIS_ESTIMATE_N_ANCHORS] = { 0.25, 0.18, 0.15, 0.10 };
#define COMPLEXITY_MIN 0.6
#define COMPLEXITY_MAX 1.6
/* The extra spread a complexity factor away from 1 brings. */
#define COMPLEXITY_SIGMA_BASE 0.10
#define COMPLEXITY_SIGMA_SLOPE 0.5

/*
 * Group: source codec efficiency, in H.264 bits per bit (the author's
 * reasoning from the generations of the codecs: MPEG-2 needs about twice
 * H.264's bitrate, HEVC and VP9 about 0.6 of it, AV1 about half).  A codec
 * not here has no figure: such a file is "codec unknown".
 */
static const struct
{
	const gchar *codec;
	gdouble      efficiency;
} codec_efficiency[] = {
	{ "h264", 1.0 }, { "hevc", 1.6 }, { "vp9", 1.5 }, { "av1", 2.0 }, { "vvc", 2.4 },
	{ "mpeg2video", 0.5 }, { "mpeg1video", 0.4 }, { "vc1", 0.85 }, { "wmv3", 0.75 },
	{ "mpeg4", 0.65 }, { "msmpeg4v3", 0.55 }, { "msmpeg4v2", 0.5 }, { "vp8", 0.9 },
	{ NULL, 0.0 }
};

/* ── Audio priors ───────────────────────────────────────────────── */

/*
 * Audio cells.  For the lossy targets the middle is the target bitrate
 * times the factor below (VBR encoders wander around it); for FLAC it is
 * the source's size times the factor.  The author's reasoning: Opus and
 * AAC VBR land within about a tenth of the asked bitrate; LAME V0 averages
 * about 245 kbit/s on pop and rock (less on quiet music); FLAC at level 8
 * saves one or two percent over the usual level 5; WAV and other PCM
 * compress to about 60 %; ALAC and TTA are about as good as FLAC, APE and
 * TAK slightly better (so FLAC would be larger); reducing 24-bit or
 * high-rate audio to 16 bits at 44.1 kHz shrinks it about in proportion to
 * the samples dropped.
 */
typedef enum
{
	AUDIO_OPUS_MUSIC,
	AUDIO_OPUS_SPEECH,
	AUDIO_AAC_MUSIC,
	AUDIO_AAC_SPEECH,
	AUDIO_MP3,
	AUDIO_FLAC_PCM,
	AUDIO_FLAC_FLAC,
	AUDIO_FLAC_ALAC,
	AUDIO_FLAC_APE,
	AUDIO_FLAC_TTA,
	AUDIO_FLAC_TAK,
	AUDIO_FLAC_REDUCE,
	AUDIO_N_CELLS
} AudioCell;

static const struct
{
	const gchar *name;
	gdouble      factor;
	gdouble      sigma;
} audio_priors[AUDIO_N_CELLS] = {
	{ "opus/music",  1.00, 0.113 },	/* ln 1.12 */
	{ "opus/speech", 1.00, 0.095 },	/* ln 1.10 */
	{ "aac/music",   1.00, 0.095 },
	{ "aac/speech",  1.00, 0.095 },
	{ "mp3/v0",      1.00, 0.140 },	/* ln 1.15, around the target's 245 kbit/s */
	{ "flac/pcm",    0.60, 0.223 },	/* ln 1.25 */
	{ "flac/flac",   0.985, 0.030 },
	{ "flac/alac",   0.98, 0.049 },
	{ "flac/ape",    1.05, 0.049 },
	{ "flac/tta",    1.00, 0.049 },
	{ "flac/tak",    1.04, 0.049 },
	{ "flac/reduce", 0.95, 0.223 }	/* times the share of samples kept */
};

/* Opus and AAC targets at or below this are speech. */
#define SPEECH_BITRATE 96000

/*
 * Group: what an audio file holds besides the audio.  Cover art is
 * guessed from its picture size at 0.35 bytes per pixel (a JPEG; 0.1 to 3
 * for the range, PNG being the large end), 300 KB when the size is not
 * known; tags and the container at 8 KB.  The author's guesses.
 */
#define ART_BYTES_PER_PIXEL 0.35
#define ART_LOW_PER_PIXEL   0.10
#define ART_HIGH_PER_PIXEL  3.0
#define ART_UNKNOWN_BYTES   300000.0
#define TAG_BYTES           8192.0

/* ── Cells ──────────────────────────────────────────────────────── */

#define N_VIDEO_CODECS 2
#define N_LEVELS (AXIS_QUALITY_MAX - AXIS_QUALITY_MIN + 1)
#define N_VIDEO_CELLS (N_VIDEO_CODECS * AXIS_ENCODER_COUNT * N_LEVELS * AXIS_ESTIMATE_N_ANCHORS)
#define N_CELLS (N_VIDEO_CELLS + AUDIO_N_CELLS)

/* Below this many samples the observed spread is not trusted. */
#define MIN_SAMPLES_FOR_SPREAD 5
#define MIN_SIGMA 0.05

typedef struct
{
	gchar                *name;
	gdouble               prior_mid;	/* bits per pixel (video) or factor (audio) */
	gdouble               prior_sigma;
	GArray               *log_ratios;	/* gdouble: observed over predicted, logged */
	/* After finish. */
	gdouble               low;
	gdouble               mid;
	gdouble               high;
	gdouble               factor;
	AxisCalibrationState  state;
	guint                 group;
} Cell;

struct _AxisEstimateModel
{
	gint     refs;	/* atomic */
	Cell     cells[N_CELLS];
	gboolean finished;
};

static guint
video_cell(
	guint             codec,
	AxisEncoderFamily family,
	gint              level,
	guint             anchor
){
	return ((codec * AXIS_ENCODER_COUNT + (guint)family) * N_LEVELS + (guint)(level - AXIS_QUALITY_MIN)) *
		AXIS_ESTIMATE_N_ANCHORS + anchor;
}

static gdouble
crf_ref(guint codec)
{
	return codec == 0 ? PRIOR_REF_HEVC : PRIOR_REF_H264;
}

/* 0 for HEVC, 1 for H.264, -1 for an audio codec. */
static gint
video_codec_index(AxisTargetCodec codec)
{
	return codec == AXIS_TCODEC_HEVC ? 0 : codec == AXIS_TCODEC_H264 ? 1 : -1;
}

AxisEstimateModel *
axis_estimate_model_new(void)
{
	AxisEstimateModel *model = g_new0(AxisEstimateModel, 1);
	guint codec;
	guint family;
	gint level;
	guint anchor;
	guint i;

	model->refs = 1;
	for (codec = 0; codec < N_VIDEO_CODECS; codec++) {
		for (family = 0; family < AXIS_ENCODER_COUNT; family++) {
			for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
				AxisTargetCodec tcodec = codec == 0 ? AXIS_TCODEC_HEVC : AXIS_TCODEC_H264;
				/* A level is the same picture in every family: its slope
				 * is the software CRF's; the family factor is what a
				 * hardware encoder costs for that picture (the hardware
				 * encoders' own numbers, axis-estimate-target.c, are
				 * chosen to give it). */
				gdouble crf = axis_estimate_quality_value(tcodec, AXIS_ENCODER_SOFTWARE, level);
				gdouble slope = pow(2.0, (crf_ref(codec) - crf) / CRF_DOUBLING_STEPS);

				for (anchor = 0; anchor < AXIS_ESTIMATE_N_ANCHORS; anchor++) {
					Cell *cell = &model->cells[video_cell(codec, (AxisEncoderFamily)family, level, anchor)];
					gdouble base = codec == 0 ? prior_hevc[anchor] : prior_h264[anchor];

					cell->name = g_strdup_printf("%s/%s/q%d/%s", codec == 0 ? "hevc" : "h264",
						axis_encoder_family_to_string((AxisEncoderFamily)family), level, anchor_names[anchor]);
					cell->prior_mid = base * family_factor[codec][family] * slope;
					cell->prior_sigma = family_sigma[family];
				}
			}
		}
	}
	for (i = 0; i < AUDIO_N_CELLS; i++) {
		Cell *cell = &model->cells[N_VIDEO_CELLS + i];

		cell->name = g_strdup(audio_priors[i].name);
		cell->prior_mid = audio_priors[i].factor;
		cell->prior_sigma = audio_priors[i].sigma;
	}
	for (i = 0; i < N_CELLS; i++)
		model->cells[i].log_ratios = g_array_new(FALSE, FALSE, sizeof(gdouble));
	axis_estimate_model_finish(model);
	return model;
}

AxisEstimateModel *
axis_estimate_model_ref(AxisEstimateModel *model)
{
	g_return_val_if_fail(model != NULL, NULL);

	g_atomic_int_inc(&model->refs);
	return model;
}

void
axis_estimate_model_free(AxisEstimateModel *model)
{
	guint i;

	if (model == NULL || !g_atomic_int_dec_and_test(&model->refs))
		return;
	for (i = 0; i < N_CELLS; i++) {
		g_free(model->cells[i].name);
		g_array_unref(model->cells[i].log_ratios);
	}
	g_free(model);
}

guint
axis_estimate_model_n_cells(const AxisEstimateModel *model)
{
	return N_CELLS;
}

/* ── Shapes ─────────────────────────────────────────────────────── */

gdouble
axis_estimate_codec_efficiency(const gchar *codec)
{
	guint i;

	for (i = 0; codec != NULL && codec_efficiency[i].codec != NULL; i++) {
		if (strcmp(codec, codec_efficiency[i].codec) == 0)
			return codec_efficiency[i].efficiency;
	}
	return 0.0;
}

gint
axis_estimate_lossless_class(const gchar *codec)
{
	if (codec == NULL)
		return -1;
	if (g_str_has_prefix(codec, "pcm_") && strcmp(codec, "pcm_alaw") != 0 && strcmp(codec, "pcm_mulaw") != 0 &&
	    strcmp(codec, "pcm_vidc") != 0)
		return AUDIO_FLAC_PCM;
	if (strcmp(codec, "flac") == 0)
		return AUDIO_FLAC_FLAC;
	if (strcmp(codec, "alac") == 0)
		return AUDIO_FLAC_ALAC;
	if (strcmp(codec, "ape") == 0)
		return AUDIO_FLAC_APE;
	if (strcmp(codec, "tta") == 0)
		return AUDIO_FLAC_TTA;
	if (strcmp(codec, "tak") == 0)
		return AUDIO_FLAC_TAK;
	return -1;
}

gboolean
axis_estimate_lossy_known(const gchar *codec)
{
	static const gchar *const known[] = {
		"mp3", "aac", "opus", "vorbis", "wmav2", "ac3", "eac3", "mp2", NULL
	};
	guint i;

	for (i = 0; codec != NULL && known[i] != NULL; i++) {
		if (strcmp(codec, known[i]) == 0)
			return TRUE;
	}
	return FALSE;
}

void
axis_estimate_shape_init(
	AxisEstimateShape   *shape,
	const AxisMediaInfo *info
){
	gint64 best_pixels = -1;
	guint i;

	memset(shape, 0, sizeof(*shape));
	shape->seconds = -1.0;
	shape->fps = -1.0;
	shape->pixels = -1;
	shape->video_bitrate = -1;
	shape->source_bpp = -1.0;
	if (info == NULL || info->streams == NULL)
		return;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		switch (s->kind) {
		case AXIS_STREAM_VIDEO:
			if (s->attached_pic) {
				shape->n_cover++;
				break;
			}
			shape->n_video++;
			{
				gint64 pixels = s->width > 0 && s->height > 0 ? (gint64)s->width * s->height : 0;

				/* The largest picture, as the probe picks it. */
				if (pixels > best_pixels) {
					best_pixels = pixels;
					shape->video = s;
				}
			}
			break;
		case AXIS_STREAM_AUDIO:
			shape->n_audio++;
			/* The first default track, else the first, as the probe does. */
			if (shape->audio == NULL || (s->is_default && !shape->audio->is_default))
				shape->audio = s;
			break;
		case AXIS_STREAM_SUBTITLE:
			shape->n_subtitle++;
			break;
		case AXIS_STREAM_ATTACHMENT:
			shape->n_attachment++;
			break;
		default:
			shape->n_other++;
			break;
		}
	}

	if (info->duration_ms > 0 && info->duration_ms <= AXIS_PROBE_MAX_DURATION_MS)
		shape->seconds = (gdouble)info->duration_ms / 1000.0;
	if (shape->video != NULL) {
		const AxisMediaStream *v = shape->video;

		if (shape->seconds <= 0 && v->duration_ms > 0 && v->duration_ms <= AXIS_PROBE_MAX_DURATION_MS)
			shape->seconds = (gdouble)v->duration_ms / 1000.0;
		if (v->frame_rate > 0 && v->frame_rate <= 1000.0 && isfinite(v->frame_rate))
			shape->fps = v->frame_rate;
		else if (v->real_frame_rate > 0 && v->real_frame_rate <= 240.0 && isfinite(v->real_frame_rate))
			shape->fps = v->real_frame_rate;
		if (v->width > 0 && v->height > 0 && v->width <= AXIS_PROBE_MAX_DIMENSION &&
		    v->height <= AXIS_PROBE_MAX_DIMENSION)
			shape->pixels = (gint64)v->width * v->height;
		if (v->bitrate > 0 && v->bitrate <= AXIS_PROBE_MAX_BITRATE)
			shape->video_bitrate = v->bitrate;
		if (shape->pixels > 0 && shape->fps > 0 && shape->video_bitrate > 0)
			shape->source_bpp = (gdouble)shape->video_bitrate / ((gdouble)shape->pixels * shape->fps);
	} else if (shape->audio != NULL && shape->seconds <= 0 && shape->audio->duration_ms > 0 &&
	           shape->audio->duration_ms <= AXIS_PROBE_MAX_DURATION_MS) {
		shape->seconds = (gdouble)shape->audio->duration_ms / 1000.0;
	}
}

/* ── Interpolation ──────────────────────────────────────────────── */

/* Where @pixels falls: the two anchors and how far between them (log
 * scale); below the first or above the last both are that one. */
static void
locate(
	gdouble  pixels,
	guint   *a0,
	guint   *a1,
	gdouble *t
){
	guint i;

	if (pixels <= anchor_pixels[0]) {
		*a0 = *a1 = 0;
		*t = 0.0;
		return;
	}
	for (i = 0; i + 1 < AXIS_ESTIMATE_N_ANCHORS; i++) {
		if (pixels <= anchor_pixels[i + 1]) {
			*a0 = i;
			*a1 = i + 1;
			*t = (log(pixels) - log(anchor_pixels[i])) / (log(anchor_pixels[i + 1]) - log(anchor_pixels[i]));
			*t = CLAMP(*t, 0.0, 1.0);
			return;
		}
	}
	*a0 = *a1 = AXIS_ESTIMATE_N_ANCHORS - 1;
	*t = 0.0;
}

static gdouble
lerp_log(
	gdouble x0,
	gdouble x1,
	gdouble t
){
	if (x0 <= 0 || x1 <= 0)
		return MAX(x0, 0.0);
	return exp(log(x0) + (log(x1) - log(x0)) * t);
}

/* The nearest anchor, for the cell a file counts against. */
static guint
nearest(gdouble pixels)
{
	guint a0;
	guint a1;
	gdouble t;

	locate(pixels, &a0, &a1, &t);
	return t < 0.5 ? a0 : a1;
}

static gdouble
reference_at(gdouble pixels)
{
	guint a0;
	guint a1;
	gdouble t;

	locate(pixels, &a0, &a1, &t);
	return lerp_log(reference_bpp[a0], reference_bpp[a1], t);
}

/* The damped complexity factor, and the spread it adds. */
static gdouble
complexity(
	gdouble  source_bpp,
	gdouble  efficiency,
	gdouble  pixels,
	gdouble *sigma
){
	gdouble ratio = source_bpp * efficiency / reference_at(pixels);
	gdouble c;

	if (!isfinite(ratio) || ratio <= 0)
		ratio = 1.0;
	c = CLAMP(sqrt(ratio), COMPLEXITY_MIN, COMPLEXITY_MAX);
	*sigma = COMPLEXITY_SIGMA_BASE + COMPLEXITY_SIGMA_SLOPE * fabs(log(c));
	return c;
}

/* ── Calibration ────────────────────────────────────────────────── */

gboolean
axis_estimate_sample_is_valid(const AxisEstimateSample *sample)
{
	if (sample == NULL || sample->target == NULL || sample->part == NULL || sample->source_codec == NULL)
		return FALSE;
	if ((guint)sample->codec >= AXIS_TCODEC_COUNT || (guint)sample->family >= AXIS_ENCODER_COUNT)
		return FALSE;
	if (sample->quality < AXIS_QUALITY_MIN || sample->quality > AXIS_QUALITY_MAX)
		return FALSE;
	if (!isfinite(sample->seconds) || sample->seconds <= 0 || sample->seconds > 86400.0)
		return FALSE;
	if (sample->source_bitrate < 1 || sample->source_bitrate > AXIS_PROBE_MAX_BITRATE ||
	    sample->output_bitrate < 1 || sample->output_bitrate > AXIS_PROBE_MAX_BITRATE)
		return FALSE;
	if (strcmp(sample->part, "video") == 0) {
		if (video_codec_index(sample->codec) < 0)
			return FALSE;
		if (sample->width < 1 || sample->width > AXIS_PROBE_MAX_DIMENSION || sample->height < 1 ||
		    sample->height > AXIS_PROBE_MAX_DIMENSION)
			return FALSE;
		if (!isfinite(sample->frame_rate) || sample->frame_rate <= 0 || sample->frame_rate > 1000.0)
			return FALSE;
		return TRUE;
	}
	if (strcmp(sample->part, "audio") == 0)
		return video_codec_index(sample->codec) < 0;
	return FALSE;
}

/* The audio cell of a target and source. */
static gint
audio_cell_for(
	const AxisEstimateTarget *target,
	AxisTargetCodec           codec,
	gint                      nominal_bitrate,
	const gchar              *source_codec,
	gint                      sample_rate,
	gint                      bit_depth
){
	switch (codec) {
	case AXIS_TCODEC_OPUS:
		return nominal_bitrate <= SPEECH_BITRATE ? AUDIO_OPUS_SPEECH : AUDIO_OPUS_MUSIC;
	case AXIS_TCODEC_AAC:
		return nominal_bitrate <= SPEECH_BITRATE ? AUDIO_AAC_SPEECH : AUDIO_AAC_MUSIC;
	case AXIS_TCODEC_MP3:
		return AUDIO_MP3;
	case AXIS_TCODEC_FLAC:
		if (target != NULL && target->reduce_to_cd && (bit_depth > 16 || sample_rate > 48000))
			return AUDIO_FLAC_REDUCE;
		return axis_estimate_lossless_class(source_codec);
	default:
		return -1;
	}
}

/* The share of samples a reduction to 16 bits (and 44.1 kHz above 48 kHz)
 * keeps. */
static gdouble
reduce_ratio(
	gint sample_rate,
	gint bit_depth
){
	gdouble rate = sample_rate > 48000 ? 44100.0 / sample_rate : 1.0;
	gdouble bits = bit_depth > 16 ? 16.0 / bit_depth : 1.0;

	return rate * bits;
}

/* What the uncalibrated model predicts for a sample, as a bitrate, and the
 * cell it falls in; FALSE for one it has no cell for. */
static gboolean
predict_sample(
	const AxisEstimateModel  *model,
	const AxisEstimateSample *sample,
	guint                    *cell_out,
	gdouble                  *predicted
){
	const AxisEstimateTarget *builtin = sample->has_output_facts ? NULL : axis_estimate_builtin_target(sample->target);
	AxisEstimateTarget facts;
	const AxisEstimateTarget *target = NULL;
	gint vcodec = video_codec_index(sample->codec);

	/* The output facts as a target, whichever way they were given. */
	if (sample->has_output_facts) {
		memset(&facts, 0, sizeof(facts));
		facts.codec = sample->codec;
		facts.bit_depth = sample->out_bit_depth;
		facts.audio_bitrate = sample->nominal_bitrate;
		facts.reduce_to_cd = sample->reduce_to_cd;
		target = &facts;
	} else {
		target = builtin;
	}

	if (vcodec >= 0) {
		gdouble pixels = (gdouble)sample->width * sample->height;
		gdouble efficiency = axis_estimate_codec_efficiency(sample->source_codec);
		gdouble source_bpp = (gdouble)sample->source_bitrate / (pixels * sample->frame_rate);
		gdouble sigma;
		gdouble c;
		guint a0;
		guint a1;
		gdouble t;
		gdouble bpp;
		guint anchor = nearest(pixels);

		if (efficiency <= 0)
			return FALSE;
		locate(pixels, &a0, &a1, &t);
		bpp = lerp_log(model->cells[video_cell((guint)vcodec, sample->family, sample->quality, a0)].prior_mid,
			model->cells[video_cell((guint)vcodec, sample->family, sample->quality, a1)].prior_mid, t);
		if (vcodec == 0 && target != NULL && target->bit_depth == 8)
			bpp *= EIGHT_BIT_HEVC_FACTOR;
		c = complexity(source_bpp, efficiency, pixels, &sigma);
		*predicted = bpp * c * pixels * sample->frame_rate;
		*cell_out = video_cell((guint)vcodec, sample->family, sample->quality, anchor);
		return isfinite(*predicted) && *predicted > 0;
	} else {
		gint nominal = target != NULL ? target->audio_bitrate : 0;
		gint cell;

		if (target != NULL && target->audio_bitrate_mono > 0 && sample->channels == 1)
			nominal = target->audio_bitrate_mono;
		cell = audio_cell_for(target, sample->codec, nominal, sample->source_codec, sample->sample_rate,
			sample->bit_depth);
		if (cell < 0)
			return FALSE;
		if (sample->codec == AXIS_TCODEC_FLAC) {
			gdouble base = (gdouble)sample->source_bitrate;

			if (cell == AUDIO_FLAC_REDUCE)
				base *= reduce_ratio(sample->sample_rate, sample->bit_depth);
			*predicted = base * audio_priors[cell].factor;
		} else {
			if (nominal <= 0)
				return FALSE;
			*predicted = (gdouble)nominal * audio_priors[cell].factor;
		}
		*cell_out = N_VIDEO_CELLS + (guint)cell;
		return isfinite(*predicted) && *predicted > 0;
	}
}

gboolean
axis_estimate_model_add_sample(
	AxisEstimateModel        *model,
	const AxisEstimateSample *sample
){
	gdouble predicted = 0;
	gdouble ratio;
	guint cell = 0;

	g_return_val_if_fail(model != NULL, FALSE);

	if (!axis_estimate_sample_is_valid(sample) || !predict_sample(model, sample, &cell, &predicted))
		return FALSE;
	ratio = log((gdouble)sample->output_bitrate / predicted);
	if (!isfinite(ratio))
		return FALSE;
	/* A result more than a hundred times off its prediction is not a
	 * correction, it is a broken record. */
	ratio = CLAMP(ratio, -4.6, 4.6);
	g_array_append_val(model->cells[cell].log_ratios, ratio);
	model->finished = FALSE;
	return TRUE;
}

static gint
compare_doubles(
	gconstpointer a,
	gconstpointer b
){
	gdouble x = *(const gdouble *)a;
	gdouble y = *(const gdouble *)b;

	return x < y ? -1 : x > y;
}

/* The value at fraction @q of a sorted array (linear between points). */
static gdouble
quantile(
	const GArray *sorted,
	gdouble       q
){
	gdouble position = q * (sorted->len - 1);
	guint below = (guint)floor(position);
	guint above = MIN(below + 1, sorted->len - 1);
	gdouble t = position - below;

	return g_array_index(sorted, gdouble, below) * (1 - t) + g_array_index(sorted, gdouble, above) * t;
}

/* Calibrates one cell: blends the prior with the observations below
 * AXIS_ESTIMATE_CALIBRATED_SAMPLES, replaces it at or above. */
static void
calibrate(Cell *cell)
{
	guint n = cell->log_ratios->len;
	gdouble shift = 0.0;
	gdouble sigma = cell->prior_sigma;

	cell->state = n == 0 ? AXIS_CALIBRATION_NONE : n < AXIS_ESTIMATE_CALIBRATED_SAMPLES ?
		AXIS_CALIBRATION_PARTLY : AXIS_CALIBRATION_FULL;
	if (n > 0) {
		g_autoptr(GArray) sorted = g_array_sized_new(FALSE, FALSE, sizeof(gdouble), n);
		gdouble weight = MIN(1.0, (gdouble)n / AXIS_ESTIMATE_CALIBRATED_SAMPLES);
		gdouble median;
		gdouble observed = cell->prior_sigma;

		g_array_append_vals(sorted, cell->log_ratios->data, n);
		g_array_sort(sorted, compare_doubles);
		median = quantile(sorted, 0.5);
		if (n >= MIN_SAMPLES_FOR_SPREAD)
			/* Low and high at the 10th and 90th percentiles of what was
			 * seen, around the median. */
			observed = MAX(MAX(median - quantile(sorted, 0.1), quantile(sorted, 0.9) - median), MIN_SIGMA);
		shift = weight * median;
		sigma = (1.0 - weight) * cell->prior_sigma + weight * observed;
	}
	cell->factor = exp(shift);
	cell->mid = cell->prior_mid * cell->factor;
	cell->low = cell->mid * exp(-sigma);
	cell->high = cell->mid * exp(sigma);
}

void
axis_estimate_model_finish(AxisEstimateModel *model)
{
	guint codec;
	guint family;
	gint level;
	guint anchor;
	guint i;

	g_return_if_fail(model != NULL);

	for (i = 0; i < N_CELLS; i++)
		calibrate(&model->cells[i]);

	/* A video cell without samples of its own takes the pooled correction
	 * of the cells of its codec and encoder family that have some (each
	 * weighted by its samples): a prior that is wrong is likely wrong in the
	 * same direction at every level and size.  It keeps its prior's wide
	 * spread, and its state stays "none". */
	for (codec = 0; codec < N_VIDEO_CODECS; codec++) {
		for (family = 0; family < AXIS_ENCODER_COUNT; family++) {
			gdouble weighted = 0;
			gdouble samples = 0;

			for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
				for (anchor = 0; anchor < AXIS_ESTIMATE_N_ANCHORS; anchor++) {
					const Cell *cell = &model->cells[video_cell(codec, (AxisEncoderFamily)family, level, anchor)];

					weighted += cell->log_ratios->len * log(cell->factor);
					samples += cell->log_ratios->len;
				}
			}
			if (samples <= 0)
				continue;
			for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
				for (anchor = 0; anchor < AXIS_ESTIMATE_N_ANCHORS; anchor++) {
					Cell *cell = &model->cells[video_cell(codec, (AxisEncoderFamily)family, level, anchor)];

					if (cell->log_ratios->len > 0)
						continue;
					cell->factor = exp(weighted / samples);
					cell->mid = cell->prior_mid * cell->factor;
					cell->low = cell->mid * exp(-cell->prior_sigma);
					cell->high = cell->mid * exp(cell->prior_sigma);
				}
			}
		}
	}

	/* Monotonic: per codec and family, a cell is at least the cell of the
	 * level below (bits per pixel) and of the anchor below (bits per
	 * frame), for each of low, middle and high.  One pass in this order
	 * suffices: both neighbours are final when a cell is reached. */
	for (codec = 0; codec < N_VIDEO_CODECS; codec++) {
		for (family = 0; family < AXIS_ENCODER_COUNT; family++) {
			for (anchor = 0; anchor < AXIS_ESTIMATE_N_ANCHORS; anchor++) {
				for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
					Cell *cell = &model->cells[video_cell(codec, (AxisEncoderFamily)family, level, anchor)];

					if (level > AXIS_QUALITY_MIN) {
						const Cell *lower = &model->cells[video_cell(codec, (AxisEncoderFamily)family,
							level - 1, anchor)];

						cell->low = MAX(cell->low, lower->low);
						cell->mid = MAX(cell->mid, lower->mid);
						cell->high = MAX(cell->high, lower->high);
					}
					if (anchor > 0) {
						const Cell *smaller = &model->cells[video_cell(codec, (AxisEncoderFamily)family,
							level, anchor - 1)];
						gdouble scale = anchor_pixels[anchor - 1] / anchor_pixels[anchor];

						cell->low = MAX(cell->low, smaller->low * scale);
						cell->mid = MAX(cell->mid, smaller->mid * scale);
						cell->high = MAX(cell->high, smaller->high * scale);
					}
				}
			}
		}
	}

	/* Correlation groups: every uncalibrated cell of one codec and family
	 * is one group (their priors come from the same reasoning, so they are
	 * wrong together); a calibrated cell is a group of its own. */
	for (i = 0; i < N_CELLS; i++) {
		Cell *cell = &model->cells[i];

		if (cell->state != AXIS_CALIBRATION_NONE)
			cell->group = 1000 + i;
		else if (i < N_VIDEO_CELLS)
			cell->group = i / (N_LEVELS * AXIS_ESTIMATE_N_ANCHORS);
		else
			cell->group = 100;
	}
	model->finished = TRUE;
}

const gchar *
axis_calibration_state_label(AxisCalibrationState state)
{
	switch (state) {
	case AXIS_CALIBRATION_FULL:
		return "Based on measurements";
	case AXIS_CALIBRATION_PARTLY:
		return "Partly based on measurements";
	default:
		return "Informed guess";
	}
}

const gchar *
axis_calibration_state_to_string(AxisCalibrationState state)
{
	switch (state) {
	case AXIS_CALIBRATION_FULL:
		return "full";
	case AXIS_CALIBRATION_PARTLY:
		return "partly";
	default:
		return "none";
	}
}

void
axis_estimate_model_cell_info(
	const AxisEstimateModel *model,
	guint                    cell,
	AxisEstimateCellInfo    *info
){
	const Cell *c;

	memset(info, 0, sizeof(*info));
	if (model == NULL || cell >= N_CELLS)
		return;
	c = &model->cells[cell];
	info->name = c->name;
	info->samples = c->log_ratios->len;
	info->state = c->state;
	info->factor = c->factor;
	info->group = c->group;
}

AxisCalibrationState
axis_estimate_model_state_of(
	const AxisEstimateModel *model,
	const guint             *cells,
	guint                    n_cells
){
	gboolean any = FALSE;
	gboolean all = n_cells > 0;
	guint i;

	for (i = 0; i < n_cells; i++) {
		AxisCalibrationState state = cells[i] < N_CELLS ? model->cells[cells[i]].state : AXIS_CALIBRATION_NONE;

		if (state != AXIS_CALIBRATION_NONE)
			any = TRUE;
		if (state != AXIS_CALIBRATION_FULL)
			all = FALSE;
	}
	if (all)
		return AXIS_CALIBRATION_FULL;
	return any ? AXIS_CALIBRATION_PARTLY : AXIS_CALIBRATION_NONE;
}

guint
axis_estimate_model_target_cells(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	guint                    *cells,
	guint                     max
){
	gint vcodec = video_codec_index(target->codec);
	guint n = 0;
	guint anchor;

	if (vcodec >= 0) {
		for (anchor = 0; anchor < AXIS_ESTIMATE_N_ANCHORS && n < max; anchor++)
			cells[n++] = video_cell((guint)vcodec, target->encoder, target->quality, anchor);
	} else if (target->codec == AXIS_TCODEC_FLAC) {
		guint i;

		for (i = AUDIO_FLAC_PCM; i <= AUDIO_FLAC_TAK && n < max; i++)
			cells[n++] = N_VIDEO_CELLS + i;
	} else {
		gint cell = audio_cell_for(target, target->codec, target->audio_bitrate, NULL, 0, 0);

		if (cell >= 0 && n < max)
			cells[n++] = N_VIDEO_CELLS + (guint)cell;
	}
	return n;
}

/* ── Estimating ─────────────────────────────────────────────────── */

gboolean
axis_estimate_model_video(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	const AxisEstimateShape  *shape,
	const gchar              *source_codec,
	gboolean                  hdr_source,
	gint                      quality,
	AxisEstimatePart         *part
){
	gint vcodec = video_codec_index(target->codec);
	gdouble efficiency = axis_estimate_codec_efficiency(source_codec);
	gdouble pixels;
	gdouble c;
	gdouble sigma_c = 0;
	gdouble frames;
	gdouble depth = 1.0;
	guint a0;
	guint a1;
	gdouble t;
	const Cell *c0;
	const Cell *c1;

	memset(part, 0, sizeof(*part));
	if (vcodec < 0 || efficiency <= 0 || shape->pixels <= 0 || shape->fps <= 0 || shape->seconds <= 0 ||
	    shape->source_bpp <= 0)
		return FALSE;
	quality = CLAMP(quality, AXIS_QUALITY_MIN, AXIS_QUALITY_MAX);
	pixels = (gdouble)shape->pixels;
	locate(pixels, &a0, &a1, &t);
	c0 = &model->cells[video_cell((guint)vcodec, target->encoder, quality, a0)];
	c1 = &model->cells[video_cell((guint)vcodec, target->encoder, quality, a1)];
	c = complexity(shape->source_bpp, efficiency, pixels, &sigma_c);
	/* An 8-bit HEVC output (Balanced, for an SDR source). */
	if (vcodec == 0 && (hdr_source ? target->hdr_bit_depth : target->bit_depth) == 8)
		depth = EIGHT_BIT_HEVC_FACTOR;

	frames = shape->fps * shape->seconds;
	part->low = lerp_log(c0->low, c1->low, t) * depth * c * exp(-sigma_c) * pixels * frames / 8.0;
	part->mid = lerp_log(c0->mid, c1->mid, t) * depth * c * pixels * frames / 8.0;
	part->high = lerp_log(c0->high, c1->high, t) * depth * c * exp(sigma_c) * pixels * frames / 8.0;
	part->cell = video_cell((guint)vcodec, target->encoder, quality, nearest(pixels));
	return isfinite(part->low) && isfinite(part->mid) && isfinite(part->high) && part->low >= 0 &&
		part->low <= part->mid && part->mid <= part->high;
}

gboolean
axis_estimate_model_audio(
	const AxisEstimateModel  *model,
	const AxisEstimateTarget *target,
	const AxisEstimateShape  *shape,
	gdouble                   source_bytes,
	AxisEstimatePart         *part
){
	const AxisMediaStream *a = shape->audio;
	gint nominal = target->audio_bitrate;
	gint cell;
	const Cell *c;
	gdouble base;

	memset(part, 0, sizeof(*part));
	if (a == NULL || shape->seconds <= 0)
		return FALSE;
	if (target->audio_bitrate_mono > 0 && a->channels == 1)
		nominal = target->audio_bitrate_mono;
	cell = audio_cell_for(target, target->codec, nominal, a->codec, a->sample_rate, a->bit_depth_audio);
	if (cell < 0)
		return FALSE;
	c = &model->cells[N_VIDEO_CELLS + (guint)cell];
	if (target->codec == AXIS_TCODEC_FLAC) {
		base = source_bytes;
		if (cell == AUDIO_FLAC_REDUCE) {
			if (a->sample_rate <= 0 || a->bit_depth_audio <= 0)
				return FALSE;
			base *= reduce_ratio(a->sample_rate, a->bit_depth_audio);
		}
	} else {
		if (nominal <= 0)
			return FALSE;
		base = (gdouble)nominal * shape->seconds / 8.0;
	}
	if (!isfinite(base) || base < 0)
		return FALSE;
	part->low = base * c->low;
	part->mid = base * c->mid;
	part->high = base * c->high;
	part->cell = N_VIDEO_CELLS + (guint)cell;
	return isfinite(part->high) && part->low <= part->mid && part->mid <= part->high;
}

void
axis_estimate_other_bytes(
	const AxisMediaInfo *info,
	gdouble             *low,
	gdouble             *mid,
	gdouble             *high
){
	*low = *mid = *high = TAG_BYTES;
	if (info == NULL || !info->has_embedded_art)
		return;
	if (info->art_width > 0 && info->art_height > 0) {
		gdouble pixels = (gdouble)info->art_width * info->art_height;

		*low += pixels * ART_LOW_PER_PIXEL;
		*mid += pixels * ART_BYTES_PER_PIXEL;
		*high += pixels * ART_HIGH_PER_PIXEL;
	} else {
		*low += ART_UNKNOWN_BYTES / 6.0;
		*mid += ART_UNKNOWN_BYTES;
		*high += ART_UNKNOWN_BYTES * 10.0;
	}
}
