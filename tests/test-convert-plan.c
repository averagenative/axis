/*
 * test-convert-plan.c - conversion plans from data alone: golden argument
 * vectors for representative files and every encoder family, nothing of
 * the file in the vector, refusals, dropped and added streams, the choice
 * of encoder, the output check, the vector's grammar, the progress parser
 * and the capability parsers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No ffmpeg and no database: the files are probe records built by hand
 * (tests/axis-test-convert.h).  test-convert-real.c runs the plans.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-convert.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-estimate.h"
#include "transcode/axis-profile.h"

#define GB G_GINT64_CONSTANT(1000000000)

/* ── Helpers ────────────────────────────────────────────────────── */

static void
builtin(
	const gchar           *slug,
	AxisProfileDefinition *def
){
	const AxisProfileBuiltin *b = axis_profile_builtin(slug);

	g_assert_nonnull(b);
	g_assert_true(axis_profile_definition_parse(b->definition, -1, def, NULL));
}

/* The vector as one line, the format whitelist written WL. */
static gchar *
joined(const AxisConvertPlan *plan)
{
	g_autofree gchar *line = g_strjoinv(" ", plan->args);
	g_auto(GStrv) parts = g_strsplit(line, axis_probe_format_whitelist(), -1);

	return g_strjoinv("WL", parts);
}

static gchar *
stats(guint k)
{
	static const gchar *const tags[] = {
		"BPS", "BPS-eng", "NUMBER_OF_FRAMES", "NUMBER_OF_FRAMES-eng", "NUMBER_OF_BYTES", "NUMBER_OF_BYTES-eng",
		"_STATISTICS_WRITING_APP", "_STATISTICS_WRITING_APP-eng", "_STATISTICS_WRITING_DATE_UTC",
		"_STATISTICS_WRITING_DATE_UTC-eng", "_STATISTICS_TAGS", "_STATISTICS_TAGS-eng", NULL
	};
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; tags[i] != NULL; i++)
		g_string_append_printf(out, "%s-metadata:s:%u %s=", i > 0 ? " " : "", k, tags[i]);
	return g_string_free(out, FALSE);
}

static void
assert_args(
	const AxisConvertPlan *plan,
	const gchar           *expected
){
	g_autofree gchar *got = joined(plan);

	if (g_strcmp0(got, expected) != 0)
		g_error("argument vector differs:\n got: %s\nwant: %s", got, expected);
}

static AxisConvertPlan *
build(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         size,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps
){
	AxisConvertPlan *plan = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;

	if (!axis_convert_plan_build(def, info, size, family, caps, NULL, &plan, &refusal))
		g_error("no plan: %s", axis_convert_refusal_to_string(refusal));
	g_assert_cmpint(refusal, ==, AXIS_CONVERT_OK);
	g_assert_true(axis_convert_args_valid((const gchar *const *)plan->args, info, NULL));
	return plan;
}

static AxisConvertRefusal
refusal_of(
	const AxisProfileDefinition   *def,
	const AxisMediaInfo           *info,
	gint64                         size,
	AxisEncoderFamily              family,
	const AxisEncoderCapabilities *caps
){
	AxisConvertPlan *plan = NULL;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;

	g_assert_false(axis_convert_plan_build(def, info, size, family, caps, NULL, &plan, &refusal));
	g_assert_null(plan);
	g_assert_cmpint(refusal, !=, AXIS_CONVERT_OK);
	return refusal;
}

static const AxisConvertStream *
out_stream(
	const AxisConvertPlan *plan,
	guint                  position
){
	g_assert_cmpuint(position, <, plan->streams->len);
	return g_ptr_array_index(plan->streams, position);
}

/* A film: H.264 1080p BT.709; E-AC-3 5.1 English (default), AC-3 stereo
 * French; SubRip English, PGS French (forced); a font; 12 chapters. */
static AxisMediaInfo *
film(void)
{
	AxisMediaInfo *info = axis_test_convert_info("matroska,webm");

	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_audio(info, 1, "eac3", 6, "eng", TRUE);
	axis_test_convert_audio(info, 2, "ac3", 2, "fra", FALSE);
	axis_test_convert_subtitle(info, 3, "subrip", "eng", FALSE, FALSE);
	axis_test_convert_subtitle(info, 4, "hdmv_pgs_subtitle", "fra", FALSE, TRUE);
	axis_test_convert_attachment(info, 5, "ttf");
	info->chapter_count = 12;
	axis_test_convert_finish(info);
	return info;
}

/* HDR10: HEVC 2160p 10-bit PQ with static metadata; TrueHD 7.1 English. */
static AxisMediaInfo *
hdr_film(void)
{
	AxisMediaInfo *info = axis_test_convert_info("matroska,webm");

	axis_test_convert_video(info, 0, "hevc", 3840, 2160, "yuv420p10le", 10, "bt2020", "smpte2084", "bt2020nc", "tv",
		AXIS_HDR_HDR10);
	axis_test_convert_audio(info, 1, "truehd", 8, "eng", TRUE);
	axis_test_convert_finish(info);
	return info;
}

/* For MP4: H.264 1080p; AAC 5.1 English (default); SubRip English. */
static AxisMediaInfo *
mp4_source(void)
{
	AxisMediaInfo *info = axis_test_convert_info("matroska,webm");

	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_audio(info, 1, "aac", 6, "eng", TRUE);
	axis_test_convert_subtitle(info, 2, "subrip", "eng", FALSE, FALSE);
	axis_test_convert_finish(info);
	return info;
}

#define HEAD "-nostdin -hide_banner -loglevel error -nostats -progress pipe:1 "
#define INPUT "-protocol_whitelist fd -format_whitelist WL -fd 3 -i fd: "
#define MKV_TAIL "-default_mode passthrough -f matroska -protocol_whitelist fd -fd 4 fd:"
#define MP4_TAIL "-f mp4 -protocol_whitelist fd -fd 4 fd:"
#define BT709 "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv"
#define BT2020PQ "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv"

/* ── Golden vectors ─────────────────────────────────────────────── */

/* Space saver, the film, each family: the same streams, each family's own
 * encoder, quality and colour arguments. */
static void
test_golden_space_saver(void)
{
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autofree gchar *s0 = stats(0);
	g_autofree gchar *rest = g_strdup("-c:1 copy -disposition:1 +default-forced -c:2 copy -disposition:2 -default-forced "
		"-c:3 copy -disposition:3 -default-forced -c:4 copy -disposition:4 -default+forced "
		"-c:5 copy -disposition:5 -default-forced " MKV_TAIL);
	const gchar *maps = "-map 0:0 -map 0:1 -map 0:2 -map 0:3 -map 0:4 -map 0:5 -map_chapters 0 -map_metadata 0 ";
	AxisProfileDefinition def;

	builtin("space_saver", &def);
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_autofree gchar *want = g_strconcat(HEAD, INPUT, maps,
			"-c:0 libx265 -filter:0 " BT709 " -pix_fmt:0 yuv420p10le -preset:0 medium -crf:0 26 -x265-params:0 "
			"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:log-level=error -a53cc:0 1 ", s0,
			" -disposition:0 +default-forced ", rest, NULL);

		assert_args(plan, want);
		g_assert_cmpstr(plan->encoder->name, ==, "libx265");
		g_assert_cmpint(plan->quality, ==, 26);
		g_assert_cmpint(plan->bit_depth, ==, 10);
	}
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, info, 8 * GB, AXIS_ENCODER_NVENC, caps);
		g_autofree gchar *want = g_strconcat(HEAD, INPUT, maps,
			"-c:0 hevc_nvenc -filter:0 " BT709 " -pix_fmt:0 p010le -preset:0 p5 -rc:0 vbr -cq:0 24 -b:0 0 ", s0,
			" -disposition:0 +default-forced ", rest, NULL);

		assert_args(plan, want);
	}
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, info, 8 * GB, AXIS_ENCODER_VAAPI, caps);
		g_autofree gchar *want = g_strconcat(HEAD, "-vaapi_device /dev/dri/renderD128 ", INPUT, maps,
			"-c:0 hevc_vaapi -filter:0 " BT709 ",format=p010le,hwupload -rc_mode:0 CQP -qp:0 23 ", s0,
			" -disposition:0 +default-forced ", rest, NULL);

		assert_args(plan, want);
	}
}

/* HDR10: x265 is given the values; NVENC takes none (and the plan says the
 * output check is what shows whether they arrive). */
static void
test_golden_hdr10(void)
{
	g_autoptr(AxisMediaInfo) info = hdr_film();
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autofree gchar *s0 = stats(0);
	AxisProfileDefinition def;
	guint i;
	gboolean noted = FALSE;

	builtin("space_saver", &def);
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_autofree gchar *want = g_strconcat(HEAD, INPUT, "-map 0:0 -map 0:1 -map_chapters 0 -map_metadata 0 "
			"-c:0 libx265 -filter:0 " BT2020PQ " -pix_fmt:0 yuv420p10le -preset:0 medium -crf:0 26 -x265-params:0 "
			"colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:hdr10=1:"
			"master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50):"
			"max-cll=1000,400:log-level=error -a53cc:0 1 ", s0, " -disposition:0 +default-forced "
			"-c:1 copy -disposition:1 +default-forced " MKV_TAIL, NULL);

		assert_args(plan, want);
		g_assert_cmpint(plan->hdr_format, ==, AXIS_HDR_HDR10);
		g_assert_true(out_stream(plan, 0)->hdr10.mastering);
		g_assert_cmpint(out_stream(plan, 0)->hdr10.max_luminance, ==, 10000000);
	}
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, info, 40 * GB, AXIS_ENCODER_NVENC, caps);
		g_autofree gchar *want = g_strconcat(HEAD, INPUT, "-map 0:0 -map 0:1 -map_chapters 0 -map_metadata 0 "
			"-c:0 hevc_nvenc -filter:0 " BT2020PQ " -pix_fmt:0 p010le -preset:0 p5 -rc:0 vbr -cq:0 24 -b:0 0 ", s0,
			" -disposition:0 +default-forced -c:1 copy -disposition:1 +default-forced " MKV_TAIL, NULL);

		assert_args(plan, want);
		for (i = 0; i < plan->notes->len; i++) {
			if (strstr(g_ptr_array_index(plan->notes, i), "takes no HDR10 values") != NULL)
				noted = TRUE;
		}
		g_assert_true(noted);
		/* What the output must carry does not depend on the family. */
		g_assert_true(out_stream(plan, 0)->hdr10.mastering);
	}
	/* The 4K quality rule: a 2160p film of 60 GB is CRF 28, of 10 GB 24. */
	{
		g_autoptr(AxisConvertPlan) big = build(&def, info, 60 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_autoptr(AxisConvertPlan) small = build(&def, info, 10 * GB, AXIS_ENCODER_SOFTWARE, caps);

		g_assert_cmpint(big->quality, ==, 28);
		g_assert_cmpint(small->quality, ==, 24);
	}
}

/* Maximum compatibility: H.264 in MP4, stereo AAC first, the original
 * after it, SubRip as mov_text. */
static void
test_golden_max_compat(void)
{
	g_autoptr(AxisMediaInfo) info = mp4_source();
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autofree gchar *s0 = stats(0);
	g_autofree gchar *s1 = stats(1);
	g_autofree gchar *want = NULL;
	AxisProfileDefinition def;

	builtin("max_compat", &def);
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	want = g_strconcat(HEAD, INPUT, "-map 0:0 -map 0:1 -map 0:1 -map 0:2 -map_chapters 0 -map_metadata 0 "
		"-c:0 libx264 -filter:0 " BT709 " -pix_fmt:0 yuv420p -preset:0 medium -crf:0 20 -x264-params:0 "
		"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv ", s0, " -disposition:0 +default-forced "
		"-c:1 aac -b:1 160000 -ac:1 2 -metadata:s:1 title= ", s1, " -disposition:1 +default-forced "
		"-c:2 copy -disposition:2 -default-forced -c:3 mov_text -disposition:3 -default-forced " MP4_TAIL, NULL);
	assert_args(plan, want);

	/* What the output must be: the stereo track first and default, the
	 * original after it, the subtitle made default by MP4 itself (it marks
	 * the first track of a kind when none is). */
	g_assert_cmpuint(plan->streams->len, ==, 4);
	g_assert_cmpint(out_stream(plan, 1)->action, ==, AXIS_CONVERT_STEREO);
	g_assert_cmpint(out_stream(plan, 1)->channels, ==, 2);
	g_assert_true(out_stream(plan, 1)->is_default);
	g_assert_null(out_stream(plan, 1)->title);
	g_assert_cmpstr(out_stream(plan, 1)->language, ==, "eng");
	g_assert_cmpint(out_stream(plan, 2)->action, ==, AXIS_CONVERT_COPY);
	g_assert_cmpint(out_stream(plan, 2)->channels, ==, 6);
	g_assert_false(out_stream(plan, 2)->is_default);
	g_assert_cmpstr(out_stream(plan, 3)->codec, ==, "mov_text");
	g_assert_true(out_stream(plan, 3)->is_default);
	g_assert_false(out_stream(plan, 3)->check_title);
}

/* Balanced: HEVC 8-bit, the original audio, a stereo AAC track after it. */
static void
test_golden_balanced(void)
{
	g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autofree gchar *s0 = stats(0);
	g_autofree gchar *s2 = stats(2);
	g_autofree gchar *want = NULL;
	AxisProfileDefinition def;

	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_audio(info, 1, "dts", 6, "eng", TRUE);
	axis_test_convert_finish(info);
	builtin("balanced", &def);
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	want = g_strconcat(HEAD, INPUT, "-map 0:0 -map 0:1 -map 0:1 -map_chapters 0 -map_metadata 0 "
		"-c:0 libx265 -filter:0 " BT709 " -pix_fmt:0 yuv420p -preset:0 medium -crf:0 26 -x265-params:0 "
		"colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited:log-level=error -a53cc:0 1 ", s0,
		" -disposition:0 +default-forced -c:1 copy -disposition:1 +default-forced "
		"-c:2 aac -b:2 160000 -ac:2 2 -metadata:s:2 title= ", s2, " -disposition:2 -default-forced " MKV_TAIL, NULL);
	assert_args(plan, want);
	g_assert_cmpint(plan->bit_depth, ==, 8);
}

/* ── Nothing of the file in the vector ──────────────────────────── */

static void
assert_nothing_of(
	const AxisConvertPlan *plan,
	const gchar *const    *texts
){
	guint i;
	guint j;

	for (i = 0; plan->args[i] != NULL; i++) {
		const gchar *a = plan->args[i];

		g_assert_cmpstr(a, !=, "");
		g_assert_cmpstr(a, !=, "-y");
		for (j = 0; texts[j] != NULL; j++) {
			if (strstr(a, texts[j]) != NULL)
				g_error("argument %u (%s) has text of the file in it: %s", i, a, texts[j]);
		}
		/* The only path is the VA-API render device. */
		if (strchr(a, '/') != NULL && !g_str_has_prefix(a, "/dev/dri/renderD"))
			g_error("argument %u is a path: %s", i, a);
	}
}

/* Spec: Hostile names and tags.  Every text field of the record (titles,
 * languages, tags, a cover's and an attachment's codec, the container's
 * name) holds option-like and shell-like text, and paths under a library
 * root (Spec: No library path): none of it reaches the vector, for any
 * family. */
static void
test_hostile_text(void)
{
	const gchar *const *texts = axis_test_convert_hostile_texts();
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	guint t;
	guint family;

	for (t = 0; texts[t] != NULL; t++) {
		g_autoptr(AxisMediaInfo) info = film();
		AxisProfileDefinition def;
		guint i;

		g_free(info->tag_title);
		info->tag_title = g_strdup(texts[t]);
		g_free(info->tag_artist);
		info->tag_artist = g_strdup(texts[t]);
		g_free(info->tag_album);
		info->tag_album = g_strdup(texts[t]);
		for (i = 0; i < info->streams->len; i++) {
			AxisMediaStream *s = g_ptr_array_index(info->streams, i);

			g_free(s->title);
			s->title = g_strdup(texts[t]);
			g_free(s->channel_layout);
			s->channel_layout = g_strdup(texts[t]);
			g_free(s->profile);
			s->profile = g_strdup(texts[t]);
			/* A language the profile keeps nothing of is still kept
			 * (unknown languages always are): the text goes to the
			 * plan's expectation, never to the vector. */
			if (s->kind == AXIS_STREAM_SUBTITLE || s->kind == AXIS_STREAM_ATTACHMENT) {
				g_free(s->language);
				s->language = g_strdup(texts[t]);
			}
		}
		builtin("space_saver", &def);
		for (family = 0; family < AXIS_ENCODER_COUNT; family++) {
			g_autoptr(AxisConvertPlan) plan = build(&def, info, 8 * GB, (AxisEncoderFamily)family, caps);

			assert_nothing_of(plan, texts);
			/* The text is data the plan expects ffmpeg to copy. */
			g_assert_cmpstr(out_stream(plan, 1)->title, ==, texts[t]);
		}
	}
}

/* A language from the file is never an argument, even one of the
 * alphabet ("-map"): the rule only decides which tracks are mapped. */
static void
test_hostile_language(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	AxisMediaStream *a = g_ptr_array_index(info->streams, 2);
	guint i;

	g_free(a->language);
	a->language = g_strdup("-map");
	builtin("space_saver", &def);
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	for (i = 0; plan->args[i] != NULL; i++)
		g_assert_cmpstr(plan->args[i], !=, "-map0");
	/* -map appears only as the option itself, each followed by "0:N". */
	for (i = 0; plan->args[i] != NULL; i++) {
		if (strcmp(plan->args[i], "-map") == 0)
			g_assert_true(g_str_has_prefix(plan->args[i + 1], "0:"));
	}
}

/* ── Streams: dropped, added, kept ──────────────────────────────── */

/* Spec: Dropped stream is explained. */
static void
test_dropped_language(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	const AxisConvertDropped *d;
	guint i;

	builtin("space_saver", &def);
	memcpy(def.languages[0], "eng", 4);
	def.n_languages = 1;
	g_assert_true(axis_profile_definition_check(&def, NULL));
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	g_assert_cmpuint(plan->dropped->len, ==, 1);
	d = g_ptr_array_index(plan->dropped, 0);
	g_assert_cmpint(d->source, ==, 2);
	g_assert_cmpint(d->kind, ==, AXIS_STREAM_AUDIO);
	g_assert_nonnull(strstr(d->reason, "language"));
	/* It is not mapped; everything else is, once each, in order. */
	for (i = 0; plan->args[i] != NULL; i++)
		g_assert_cmpstr(plan->args[i], !=, "0:2");
	g_assert_cmpuint(plan->streams->len, ==, 5);
	g_assert_cmpint(out_stream(plan, 1)->source, ==, 1);
	g_assert_cmpint(out_stream(plan, 2)->source, ==, 3);

	/* Every stream of the file is either in the output or dropped. */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);
		gboolean found = FALSE;
		guint j;

		for (j = 0; j < plan->streams->len; j++) {
			if (out_stream(plan, j)->source == s->index)
				found = TRUE;
		}
		for (j = 0; j < plan->dropped->len; j++) {
			if (((const AxisConvertDropped *)g_ptr_array_index(plan->dropped, j))->source == s->index)
				found = TRUE;
		}
		g_assert_true(found);
	}

	/* A track of unknown language is kept; a rule that would leave no
	 * sound refuses. */
	memcpy(def.languages[0], "jpn", 4);
	g_assert_cmpint(refusal_of(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
		AXIS_CONVERT_REFUSE_NO_AUDIO_LEFT);
	g_free(((AxisMediaStream *)g_ptr_array_index(info->streams, 2))->language);
	((AxisMediaStream *)g_ptr_array_index(info->streams, 2))->language = NULL;
	{
		g_autoptr(AxisConvertPlan) kept = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);

		g_assert_cmpuint(kept->dropped->len, ==, 1);
		g_assert_cmpint(out_stream(kept, 1)->source, ==, 2);
	}
}

/* Spec: Added stereo track, in the order the profile states. */
static void
test_added_stereo(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	AxisProfileDefinition def;

	builtin("balanced", &def);
	/* Surround only: the original, then the stereo track. */
	{
		g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
		g_autoptr(AxisConvertPlan) plan = NULL;

		axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv",
			AXIS_HDR_SDR);
		axis_test_convert_audio(info, 1, "truehd", 8, "eng", TRUE);
		axis_test_convert_audio(info, 2, "ac3", 6, "fra", FALSE);
		axis_test_convert_finish(info);
		plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_assert_cmpuint(plan->streams->len, ==, 4);
		g_assert_cmpint(out_stream(plan, 1)->source, ==, 1);
		g_assert_cmpint(out_stream(plan, 1)->action, ==, AXIS_CONVERT_COPY);
		g_assert_cmpint(out_stream(plan, 2)->source, ==, 2);
		/* Made from the default track, after the originals, not
		 * default. */
		g_assert_cmpint(out_stream(plan, 3)->source, ==, 1);
		g_assert_cmpint(out_stream(plan, 3)->action, ==, AXIS_CONVERT_STEREO);
		g_assert_false(out_stream(plan, 3)->is_default);
		g_assert_true(out_stream(plan, 1)->is_default);

		/* Stereo first: the stereo track, then the originals; the first
		 * audio track is the default one. */
		def.audio = AXIS_AUDIO_STEREO_FIRST;
		{
			g_autoptr(AxisConvertPlan) first = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);

			g_assert_cmpint(out_stream(first, 1)->action, ==, AXIS_CONVERT_STEREO);
			g_assert_cmpint(out_stream(first, 1)->source, ==, 1);
			g_assert_true(out_stream(first, 1)->is_default);
			g_assert_cmpint(out_stream(first, 2)->source, ==, 1);
			g_assert_false(out_stream(first, 2)->is_default);
			g_assert_cmpint(out_stream(first, 3)->source, ==, 2);
		}
		def.audio = AXIS_AUDIO_COPY_ADD_STEREO;
	}
	/* A stereo AAC track already there: nothing is added; for stereo
	 * first, the main track (if it is the one) goes first as it is. */
	{
		g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
		g_autoptr(AxisConvertPlan) plan = NULL;

		axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv",
			AXIS_HDR_SDR);
		axis_test_convert_audio(info, 1, "dts", 6, "eng", FALSE);
		axis_test_convert_audio(info, 2, "aac", 2, "eng", TRUE);
		axis_test_convert_finish(info);
		plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_assert_cmpuint(plan->streams->len, ==, 3);
		def.audio = AXIS_AUDIO_STEREO_FIRST;
		{
			g_autoptr(AxisConvertPlan) first = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);

			g_assert_cmpuint(first->streams->len, ==, 3);
			g_assert_cmpint(out_stream(first, 1)->source, ==, 2);
			g_assert_cmpint(out_stream(first, 1)->action, ==, AXIS_CONVERT_COPY);
			g_assert_true(out_stream(first, 1)->is_default);
			g_assert_cmpint(out_stream(first, 2)->source, ==, 1);
			g_assert_false(out_stream(first, 2)->is_default);
		}
	}
}

/* Spec (design): every kept stream mapped, in order, each option by its
 * output index; attachments after subtitles; a cover in MP4. */
static void
test_mapping_order(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;

	/* Streams in an odd source order. */
	axis_test_convert_attachment(info, 0, "ttf");
	axis_test_convert_subtitle(info, 1, "ass", "eng", TRUE, FALSE);
	axis_test_convert_audio(info, 2, "flac", 2, "jpn", FALSE);
	axis_test_convert_video(info, 3, "h264", 1280, 720, "yuv420p", 8, "bt709", "bt709", "bt709", "tv", AXIS_HDR_SDR);
	axis_test_convert_audio(info, 4, "aac", 2, "eng", TRUE);
	axis_test_convert_subtitle(info, 5, "mov_text", "jpn", FALSE, TRUE);
	axis_test_convert_finish(info);
	builtin("space_saver", &def);
	plan = build(&def, info, 2 * GB, AXIS_ENCODER_SOFTWARE, caps);
	g_assert_cmpuint(plan->streams->len, ==, 6);
	g_assert_cmpint(out_stream(plan, 0)->source, ==, 3);
	g_assert_cmpint(out_stream(plan, 1)->source, ==, 2);
	g_assert_cmpint(out_stream(plan, 2)->source, ==, 4);
	g_assert_cmpint(out_stream(plan, 3)->source, ==, 1);
	/* mov_text cannot go into Matroska as it is: SubRip. */
	g_assert_cmpint(out_stream(plan, 4)->source, ==, 5);
	g_assert_cmpint(out_stream(plan, 4)->action, ==, AXIS_CONVERT_TO_TEXT);
	g_assert_cmpstr(out_stream(plan, 4)->codec, ==, "subrip");
	g_assert_true(out_stream(plan, 4)->is_forced);
	g_assert_cmpint(out_stream(plan, 5)->source, ==, 0);
	g_assert_cmpint(out_stream(plan, 5)->kind, ==, AXIS_STREAM_ATTACHMENT);
	{
		g_autofree gchar *line = joined(plan);

		g_assert_nonnull(strstr(line, "-map 0:3 -map 0:2 -map 0:4 -map 0:1 -map 0:5 -map 0:0 "));
		g_assert_nonnull(strstr(line, "-c:4 srt -disposition:4 -default+forced"));
		g_assert_nonnull(strstr(line, "-c:5 copy -disposition:5 -default-forced"));
	}

	/* MP4: a cover picture is carried, after the subtitles; chapters add
	 * the muxer's chapter track at the end. */
	{
		g_autoptr(AxisMediaInfo) mp4 = mp4_source();
		g_autoptr(AxisConvertPlan) p = NULL;
		AxisProfileDefinition compat;

		axis_test_convert_cover(mp4, 3, "mjpeg");
		mp4->chapter_count = 3;
		axis_test_convert_finish(mp4);
		builtin("max_compat", &compat);
		p = build(&compat, mp4, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_assert_cmpuint(p->streams->len, ==, 6);
		g_assert_true(out_stream(p, 4)->attached_pic);
		g_assert_cmpint(out_stream(p, 4)->source, ==, 3);
		g_assert_cmpint(out_stream(p, 5)->action, ==, AXIS_CONVERT_MUXER);
		g_assert_cmpint(out_stream(p, 5)->kind, ==, AXIS_STREAM_DATA);
		g_assert_cmpint(out_stream(p, 5)->source, ==, -1);
		g_assert_cmpint(p->chapters, ==, 3);
	}
}

/* Unknown colour is left unstated and said; never guessed. */
static void
test_unstated_colour(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autofree gchar *line = NULL;
	AxisProfileDefinition def;
	guint notes = 0;
	guint i;

	axis_test_convert_video(info, 0, "mpeg4", 720, 480, "yuv420p", 8, "unknown", NULL, "bt470bg", "unknown",
		AXIS_HDR_SDR);
	axis_test_convert_finish(info);
	builtin("space_saver", &def);
	plan = build(&def, info, GB, AXIS_ENCODER_SOFTWARE, caps);
	line = joined(plan);
	/* Only the matrix is stated. */
	g_assert_nonnull(strstr(line, "-filter:0 setparams=colorspace=bt470bg -pix_fmt:0"));
	g_assert_nonnull(strstr(line, "-x265-params:0 colormatrix=bt470bg:log-level=error"));
	g_assert_null(strstr(line, "color_primaries"));
	g_assert_null(strstr(line, "bt709"));
	g_assert_null(out_stream(plan, 0)->color_primaries);
	g_assert_null(out_stream(plan, 0)->color_transfer);
	g_assert_null(out_stream(plan, 0)->color_range);
	g_assert_cmpstr(out_stream(plan, 0)->color_space, ==, "bt470bg");
	for (i = 0; i < plan->notes->len; i++) {
		if (strstr(g_ptr_array_index(plan->notes, i), "does not state") != NULL)
			notes++;
	}
	g_assert_cmpuint(notes, ==, 3);

	/* Nothing stated at all: no setparams, no x264 colour. */
	{
		g_autoptr(AxisMediaInfo) bare = axis_test_convert_info("matroska,webm");
		g_autoptr(AxisConvertPlan) p = NULL;
		g_autofree gchar *l = NULL;
		AxisProfileDefinition compat;

		axis_test_convert_video(bare, 0, "mpeg2video", 720, 576, "yuv420p", 8, NULL, NULL, NULL, NULL, AXIS_HDR_SDR);
		axis_test_convert_audio(bare, 1, "aac", 2, NULL, TRUE);
		axis_test_convert_finish(bare);
		builtin("max_compat", &compat);
		p = build(&compat, bare, GB, AXIS_ENCODER_SOFTWARE, caps);
		l = joined(p);
		g_assert_null(strstr(l, "setparams"));
		g_assert_null(strstr(l, "x264-params"));
	}
}

/* HLG: carried by its transfer and primaries, 10 bits. */
static void
test_hlg(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autofree gchar *line = NULL;
	AxisProfileDefinition def;

	axis_test_convert_video(info, 0, "hevc", 3840, 2160, "yuv420p10le", 10, "bt2020", "arib-std-b67", "bt2020nc",
		"tv", AXIS_HDR_HLG);
	axis_test_convert_finish(info);
	builtin("balanced", &def);
	plan = build(&def, info, 30 * GB, AXIS_ENCODER_SOFTWARE, caps);
	line = joined(plan);
	g_assert_nonnull(strstr(line, "transfer=arib-std-b67"));
	g_assert_nonnull(strstr(line, "color_trc=arib-std-b67"));
	g_assert_nonnull(strstr(line, "-pix_fmt:0 yuv420p10le"));
	g_assert_null(strstr(line, "master-display"));
	g_assert_cmpint(plan->bit_depth, ==, 10);
	g_assert_cmpint(out_stream(plan, 0)->hdr_format, ==, AXIS_HDR_HLG);
}

/* ── Refusals ───────────────────────────────────────────────────── */

/* Spec: Missing fact; and the other refusals, each for its reason. */
static void
test_refusals(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	AxisProfileDefinition space;
	AxisProfileDefinition compat;
	AxisProfileDefinition music;

	builtin("space_saver", &space);
	builtin("max_compat", &compat);
	builtin("music_space_saver", &music);

	/* HDR10 whose mastering display values are not known. */
	{
		g_autoptr(AxisMediaInfo) info = hdr_film();
		AxisMediaStream *v = g_ptr_array_index(info->streams, 0);

		memset(&v->hdr10, 0, sizeof(v->hdr10));
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR10_METADATA);
		/* Out of range values that got into the model somehow. */
		axis_test_convert_hdr10_values(&v->hdr10);
		v->hdr10.min_luminance = v->hdr10.max_luminance;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR10_METADATA);
	}
	/* An unknown stream; a data stream. */
	{
		g_autoptr(AxisMediaInfo) info = film();
		AxisMediaStream *s = g_ptr_array_index(info->streams, 5);

		s->kind = AXIS_STREAM_UNKNOWN;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_UNKNOWN_STREAM);
		s->kind = AXIS_STREAM_DATA;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_UNKNOWN_STREAM);
	}
	/* HDR: uncertain, Dolby Vision, HDR10+, to an SDR target. */
	{
		g_autoptr(AxisMediaInfo) info = hdr_film();
		AxisMediaStream *v = g_ptr_array_index(info->streams, 0);

		v->hdr_uncertain = TRUE;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR_UNCERTAIN);
		v->hdr_uncertain = FALSE;
		info->hdr_uncertain = TRUE;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR_UNCERTAIN);
		info->hdr_uncertain = FALSE;
		v->hdr_format = info->hdr_format = AXIS_HDR_DOLBY_VISION;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_DOLBY_VISION);
		v->hdr_format = info->hdr_format = AXIS_HDR_HDR10PLUS;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR10PLUS);
		v->hdr_format = info->hdr_format = AXIS_HDR_NONE;
		g_assert_cmpint(refusal_of(&space, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR_UNCERTAIN);
		v->hdr_format = info->hdr_format = AXIS_HDR_HDR10;
		g_assert_cmpint(refusal_of(&compat, info, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_HDR_TO_SDR);
	}
	/* Interlaced, or not known to be progressive; 4:2:2; colour without a
	 * name; two pictures; no picture size. */
	{
		g_autoptr(AxisMediaInfo) info = film();
		AxisMediaStream *v = g_ptr_array_index(info->streams, 0);

		g_free(v->field_order);
		v->field_order = g_strdup("tt");
		info->interlaced = AXIS_TRI_YES;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_INTERLACED);
		g_free(v->field_order);
		v->field_order = NULL;
		info->interlaced = AXIS_TRI_UNKNOWN;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_INTERLACED);
		v->field_order = g_strdup("progressive");
		info->interlaced = AXIS_TRI_NO;
		g_free(v->pixel_format);
		v->pixel_format = g_strdup("yuv422p10le");
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_PIXEL_FORMAT);
		g_free(v->pixel_format);
		v->pixel_format = g_strdup("yuv420p");
		g_free(v->color_space);
		v->color_space = g_strdup("ycgco");
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_COLOUR);
		g_free(v->color_space);
		v->color_space = g_strdup("bt709");
		g_free(v->color_primaries);
		v->color_primaries = g_strdup("jedec-p22");
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_COLOUR);
		g_free(v->color_primaries);
		v->color_primaries = g_strdup("bt709");
		v->width = AXIS_PROBE_UNKNOWN;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_PICTURE);
		v->width = 1920;
		axis_test_convert_video(info, 6, "h264", 640, 360, "yuv420p", 8, "bt709", "bt709", "bt709", "tv",
			AXIS_HDR_SDR);
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_PICTURE);
	}
	/* Containers: a cover into Matroska, attachments and image subtitles
	 * into MP4, an audio codec MP4 cannot hold. */
	{
		g_autoptr(AxisMediaInfo) info = film();

		axis_test_convert_cover(info, 6, "mjpeg");
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_COVER_IN_MATROSKA);
	}
	{
		g_autoptr(AxisMediaInfo) info = mp4_source();

		axis_test_convert_attachment(info, 3, "ttf");
		g_assert_cmpint(refusal_of(&compat, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_ATTACHMENT_IN_MP4);
	}
	{
		g_autoptr(AxisMediaInfo) info = mp4_source();

		axis_test_convert_subtitle(info, 3, "hdmv_pgs_subtitle", "eng", FALSE, FALSE);
		g_assert_cmpint(refusal_of(&compat, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_SUBTITLE);
	}
	{
		g_autoptr(AxisMediaInfo) info = mp4_source();

		axis_test_convert_subtitle(info, 3, "ass", "eng", FALSE, FALSE);
		g_assert_cmpint(refusal_of(&compat, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_SUBTITLE);
	}
	{
		g_autoptr(AxisMediaInfo) info = mp4_source();

		axis_test_convert_audio(info, 3, "truehd", 8, "eng", FALSE);
		g_assert_cmpint(refusal_of(&compat, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_AUDIO);
	}
	{
		g_autoptr(AxisMediaInfo) info = film();
		AxisMediaStream *s = g_ptr_array_index(info->streams, 3);

		s->subtitle_kind = AXIS_SUBTITLE_UNKNOWN;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_SUBTITLE);
	}
	/* Not a video profile; a definition that does not check; a family the
	 * profile does not list; streams left out of the probe. */
	{
		g_autoptr(AxisMediaInfo) info = film();
		AxisProfileDefinition bad = space;

		g_assert_cmpint(refusal_of(&music, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_NOT_VIDEO);
		bad.crf = 27;
		g_assert_cmpint(refusal_of(&bad, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_DEFINITION);
		bad = space;
		bad.n_encoders = 1;
		g_assert_cmpint(refusal_of(&bad, info, 8 * GB, AXIS_ENCODER_NVENC, caps), ==,
			AXIS_CONVERT_REFUSE_ENCODER_NOT_IN_PROFILE);
		info->streams_omitted = 1;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_NOT_PROBED);
		info->streams_omitted = 0;
		/* Two records of one stream index: `-map 0:2` would take that
		 * stream twice and the other never. */
		((AxisMediaStream *)g_ptr_array_index(info->streams, 3))->index = 2;
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_NOT_PROBED);
		((AxisMediaStream *)g_ptr_array_index(info->streams, 3))->index = 3;
		g_free(info->status);
		info->status = g_strdup("failed");
		g_assert_cmpint(refusal_of(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==,
			AXIS_CONVERT_REFUSE_NOT_PROBED);
	}
	/* Settings outside the table. */
	{
		g_autoptr(AxisMediaInfo) info = film();
		AxisConvertOptions options;
		AxisConvertRefusal refusal = AXIS_CONVERT_OK;
		AxisConvertPlan *plan = NULL;

		memset(&options, 0, sizeof(options));
		options.software_preset = "placebo";
		g_assert_false(axis_convert_plan_build(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps, &options, &plan,
			&refusal));
		g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_OPTIONS);
		options.software_preset = NULL;
		options.vaapi_device = "/dev/sda";
		g_assert_false(axis_convert_plan_build(&space, info, 8 * GB, AXIS_ENCODER_VAAPI, caps, &options, &plan,
			&refusal));
		g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_OPTIONS);
		options.vaapi_device = "/dev/dri/renderD129";
		g_assert_true(axis_convert_plan_build(&space, info, 8 * GB, AXIS_ENCODER_VAAPI, caps, &options, &plan,
			&refusal));
		g_assert_true(g_strv_contains((const gchar *const *)plan->args, "/dev/dri/renderD129"));
		axis_convert_plan_free(plan);
		options.vaapi_device = NULL;
		options.threads = 65;
		g_assert_false(axis_convert_plan_build(&space, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps, &options, &plan,
			&refusal));
		g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_OPTIONS);
	}
}

/* Spec: an encoder without the measured capability gets no plan; HDR not
 * carried means never an HDR file; not measured means nothing. */
static void
test_capabilities_decide(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) sdr = film();
	g_autoptr(AxisMediaInfo) hdr = hdr_film();
	const AxisConvertEncoder *x265 = axis_convert_encoder_named("libx265");
	AxisProfileDefinition def;

	builtin("space_saver", &def);
	g_assert_cmpint(refusal_of(&def, sdr, 8 * GB, AXIS_ENCODER_SOFTWARE, NULL), ==, AXIS_CONVERT_REFUSE_NOT_MEASURED);

	/* Usable and 10 bits, HDR10 not shown: SDR is planned, HDR10 is not. */
	axis_encoder_capabilities_set(caps, x265, TRUE, TRUE, FALSE, "the values came back altered");
	{
		g_autoptr(AxisConvertPlan) plan = build(&def, sdr, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);

		g_assert_nonnull(plan);
	}
	g_assert_cmpint(refusal_of(&def, hdr, 40 * GB, AXIS_ENCODER_SOFTWARE, caps), ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);

	/* 8 bits only: a 10-bit target is refused, an 8-bit one is not. */
	axis_encoder_capabilities_set(caps, x265, TRUE, FALSE, FALSE, "it made 8 bits when 10 were asked");
	g_assert_cmpint(refusal_of(&def, sdr, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
	{
		AxisProfileDefinition balanced;
		g_autoptr(AxisConvertPlan) plan = NULL;

		builtin("balanced", &balanced);
		plan = build(&balanced, sdr, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_assert_cmpint(plan->bit_depth, ==, 8);
	}
	/* Not usable at all. */
	axis_encoder_capabilities_set(caps, x265, FALSE, FALSE, FALSE, "no such encoder");
	g_assert_cmpint(refusal_of(&def, sdr, 8 * GB, AXIS_ENCODER_SOFTWARE, caps), ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
	/* The helpers: no AAC, no stereo track. */
	{
		AxisProfileDefinition balanced;
		g_autoptr(AxisEncoderCapabilities) no_aac = axis_test_convert_caps_all();
		g_autoptr(AxisMediaInfo) info = axis_test_convert_info("matroska,webm");

		axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, "bt709", "bt709", "bt709", "tv",
			AXIS_HDR_SDR);
		axis_test_convert_audio(info, 1, "dts", 6, "eng", TRUE);
		axis_test_convert_finish(info);
		no_aac->aac = FALSE;
		builtin("balanced", &balanced);
		g_assert_cmpint(refusal_of(&balanced, info, 8 * GB, AXIS_ENCODER_SOFTWARE, no_aac), ==,
			AXIS_CONVERT_REFUSE_HELPER_ENCODER);
	}
}

/* Spec: Encoder missing: the next in the profile's preference is used. */
static void
test_choose_next_encoder(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisMediaInfo) hdr = hdr_film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	AxisConvertRefusal refusal = AXIS_CONVERT_OK;
	AxisConvertRefusal tried[AXIS_ENCODER_COUNT];

	builtin("space_saver", &def);
	def.encoders[0] = AXIS_ENCODER_NVENC;
	def.encoders[1] = AXIS_ENCODER_SOFTWARE;
	def.encoders[2] = AXIS_ENCODER_VAAPI;
	g_assert_true(axis_profile_definition_check(&def, NULL));
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("hevc_nvenc"), FALSE, FALSE, FALSE,
		"No NVENC capable devices found");
	g_assert_true(axis_convert_plan_choose(&def, info, 8 * GB, caps, NULL, &plan, &refusal, tried));
	g_assert_cmpstr(plan->encoder->name, ==, "libx265");
	g_assert_cmpint(tried[0], ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
	g_assert_cmpint(tried[1], ==, AXIS_CONVERT_OK);
	g_clear_pointer(&plan, axis_convert_plan_free);

	/* HDR10 not shown for x265 either: VA-API, which was. */
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), TRUE, TRUE, FALSE, "altered");
	g_assert_true(axis_convert_plan_choose(&def, hdr, 40 * GB, caps, NULL, &plan, &refusal, tried));
	g_assert_cmpstr(plan->encoder->name, ==, "hevc_vaapi");
	g_clear_pointer(&plan, axis_convert_plan_free);
	/* And none: no plan, the first family's reason. */
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("hevc_vaapi"), TRUE, TRUE, FALSE, "altered");
	g_assert_false(axis_convert_plan_choose(&def, hdr, 40 * GB, caps, NULL, &plan, &refusal, tried));
	g_assert_null(plan);
	g_assert_cmpint(refusal, ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
	g_assert_cmpint(tried[2], ==, AXIS_CONVERT_REFUSE_NOT_CAPABLE);
}

/* Spec: Quality mapping differs by family: each its own option and
 * number for the same level; and the estimator agrees about the level. */
static void
test_quality_by_family(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	AxisProfileDefinition def;
	gint level;

	builtin("space_saver", &def);
	{
		g_autoptr(AxisConvertPlan) sw = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_autoptr(AxisConvertPlan) nv = build(&def, info, 8 * GB, AXIS_ENCODER_NVENC, caps);
		g_autoptr(AxisConvertPlan) va = build(&def, info, 8 * GB, AXIS_ENCODER_VAAPI, caps);

		g_assert_true(g_strv_contains((const gchar *const *)sw->args, "-crf:0"));
		g_assert_true(g_strv_contains((const gchar *const *)nv->args, "-cq:0"));
		g_assert_true(g_strv_contains((const gchar *const *)va->args, "-qp:0"));
		g_assert_cmpint(sw->level, ==, nv->level);
		g_assert_cmpint(sw->level, ==, va->level);
		g_assert_cmpint(sw->quality, ==, 26);
		g_assert_cmpint(nv->quality, ==, 24);
		g_assert_cmpint(va->quality, ==, 23);
	}
	/* The tables, every level: hardware a fixed number of steps better,
	 * never 0 (NVENC's "automatic"), always within the encoders' ranges. */
	for (level = AXIS_QUALITY_MIN; level <= AXIS_QUALITY_MAX; level++) {
		gint hevc = axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE, level);
		gint h264 = axis_estimate_quality_value(AXIS_TCODEC_H264, AXIS_ENCODER_SOFTWARE, level);

		g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_NVENC, level), ==, hevc - 2);
		g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_HEVC, AXIS_ENCODER_VAAPI, level), ==, hevc - 3);
		g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_H264, AXIS_ENCODER_NVENC, level), ==, h264 - 2);
		g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_H264, AXIS_ENCODER_VAAPI, level), ==, h264 - 3);
		g_assert_cmpint(axis_estimate_quality_value(AXIS_TCODEC_H264, AXIS_ENCODER_VAAPI, level), >=, 1);
		g_assert_cmpint(axis_estimate_quality_level(AXIS_TCODEC_HEVC, hevc), ==, level);
	}
	/* The encoder table agrees with the families. */
	g_assert_cmpstr(axis_convert_encoder_for(AXIS_TCODEC_HEVC, AXIS_ENCODER_NVENC)->name, ==, "hevc_nvenc");
	g_assert_cmpstr(axis_convert_encoder_for(AXIS_TCODEC_H264, AXIS_ENCODER_VAAPI)->name, ==, "h264_vaapi");
	g_assert_false(axis_convert_encoder_for(AXIS_TCODEC_HEVC, AXIS_ENCODER_NVENC)->hdr10_stated);
	g_assert_true(axis_convert_encoder_for(AXIS_TCODEC_HEVC, AXIS_ENCODER_SOFTWARE)->hdr10_stated);
}

/* ── The output check ───────────────────────────────────────────── */

/* A probe of the output, as the plan says it must be. */
static AxisMediaInfo *
perfect_output(const AxisConvertPlan *plan)
{
	AxisMediaInfo *out = axis_test_convert_info(plan->container == AXIS_CONTAINER_MP4 ? "mov,mp4,m4a,3gp,3g2,mj2" :
		"matroska,webm");
	guint i;

	out->chapter_count = plan->chapters;
	for (i = 0; i < plan->streams->len; i++) {
		const AxisConvertStream *e = g_ptr_array_index(plan->streams, i);
		AxisMediaStream *s;

		if (e->action == AXIS_CONVERT_ENCODE) {
			s = axis_test_convert_video(out, (gint)i, e->codec, e->width, e->height, "yuv420p10le", e->bit_depth,
				e->color_primaries, e->color_transfer, e->color_space, e->color_range, e->hdr_format);
			s->hdr10 = e->hdr10;
			s->signals.has_mastering = e->hdr10.mastering;
			s->signals.has_content_light = e->hdr10.light;
			s->signals.mastering_in_frames = e->hdr10.mastering;
			s->signals.light_in_frames = e->hdr10.light;
		} else {
			s = g_new0(AxisMediaStream, 1);
			s->index = (gint)i;
			s->kind = e->kind;
			s->codec = g_strdup(e->codec);
			s->channels = e->channels;
			axis_hdr_signals_init(&s->signals);
			g_ptr_array_add(out->streams, s);
		}
		s->is_default = e->is_default;
		s->is_forced = e->is_forced;
		s->attached_pic = e->attached_pic;
		g_free(s->language);
		s->language = g_strdup(e->language);
		g_free(s->title);
		s->title = g_strdup(e->title);
	}
	axis_test_convert_finish(out);
	return out;
}

static void
expect_mismatch(
	const AxisConvertPlan *plan,
	AxisMediaInfo         *out,
	const gchar           *what
){
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);

	if (axis_convert_plan_check_output(plan, out, mismatches))
		g_error("not noticed: %s", what);
	g_assert_cmpuint(mismatches->len, >, 0);
}

static void
test_output_check(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisMediaInfo) hdr = hdr_film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisConvertPlan) hdr_plan = NULL;
	AxisProfileDefinition def;

	builtin("space_saver", &def);
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	hdr_plan = build(&def, hdr, 40 * GB, AXIS_ENCODER_SOFTWARE, caps);
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		g_autoptr(AxisMediaInfo) hdr_out = perfect_output(hdr_plan);

		g_assert_true(axis_convert_plan_check_output(plan, out, NULL));
		g_assert_true(axis_convert_plan_check_output(hdr_plan, hdr_out, NULL));
	}
	/* Each difference is one. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		((AxisMediaStream *)g_ptr_array_index(out->streams, 2))->is_default = TRUE;
		expect_mismatch(plan, out, "a default flag");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		((AxisMediaStream *)g_ptr_array_index(out->streams, 4))->is_forced = FALSE;
		expect_mismatch(plan, out, "a forced flag");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		g_ptr_array_remove_index(out->streams, 5);
		expect_mismatch(plan, out, "a missing attachment");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		axis_test_convert_audio(out, 6, "aac", 2, "eng", FALSE);
		expect_mismatch(plan, out, "an extra stream");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *a = g_ptr_array_index(out->streams, 1);
		AxisMediaStream *b = g_ptr_array_index(out->streams, 2);

		/* Two tracks swapped. */
		g_ptr_array_index(out->streams, 1) = b;
		g_ptr_array_index(out->streams, 2) = a;
		expect_mismatch(plan, out, "the order");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		g_free(v->color_primaries);
		v->color_primaries = g_strdup("unknown");
		expect_mismatch(plan, out, "lost primaries");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->bit_depth = 8;
		expect_mismatch(plan, out, "8 bits");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		out->chapter_count = 0;
		expect_mismatch(plan, out, "lost chapters");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *a = g_ptr_array_index(out->streams, 1);

		g_free(a->language);
		a->language = g_strdup("ger");
		expect_mismatch(plan, out, "a language");
	}
	/* HDR10: a value altered, light levels lost, values only in the
	 * container, HDR10 made uncertain. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(hdr_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr10.white_x++;
		expect_mismatch(hdr_plan, out, "an altered white point");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(hdr_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr10.light = FALSE;
		expect_mismatch(hdr_plan, out, "lost light levels");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(hdr_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->signals.mastering_in_frames = FALSE;
		expect_mismatch(hdr_plan, out, "values only in the container");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(hdr_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr_uncertain = TRUE;
		expect_mismatch(hdr_plan, out, "uncertain HDR");
	}
	/* An SDR output that suddenly carries HDR10 values. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		axis_test_convert_hdr10_values(&v->hdr10);
		v->signals.has_mastering = TRUE;
		expect_mismatch(plan, out, "HDR10 values on SDR");
	}
	/* No output at all; another container. */
	expect_mismatch(plan, NULL, "no probe");
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		g_free(out->container);
		out->container = g_strdup("mov,mp4,m4a,3gp,3g2,mj2");
		expect_mismatch(plan, out, "the container");
	}
}

/* ── Colour by what it means (checks 3 and 4) ───────────────────── */

/* The probe of an untagged source (a Blu-ray encode of the older kind):
 * H.264 1080p 8-bit stating none of its four colour properties, SDR. */
static AxisMediaInfo *
untagged_film(void)
{
	AxisMediaInfo *info = axis_test_convert_info("matroska,webm");

	axis_test_convert_video(info, 0, "h264", 1920, 1080, "yuv420p", 8, NULL, NULL, NULL, NULL, AXIS_HDR_SDR);
	axis_test_convert_audio(info, 1, "ac3", 6, "eng", TRUE);
	axis_test_convert_finish(info);
	return info;
}

/* Whether the picture check passes (and says nothing), or fails saying
 * exactly @expected. */
static void
expect_picture(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *out,
	const gchar           *expected
){
	g_autofree gchar *why = NULL;
	gboolean ok = axis_convert_plan_check_picture(plan, out, &why);

	if (expected == NULL) {
		if (!ok)
			g_error("the picture check failed: %s", why);
		g_assert_null(why);
		return;
	}
	if (ok)
		g_error("the picture check passed; expected: %s", expected);
	g_assert_cmpstr(why, ==, expected);
}

/* The output check (3) fails, and one of its sentences is @expected. */
static void
expect_output_says(
	const AxisConvertPlan *plan,
	const AxisMediaInfo   *out,
	const gchar           *expected
){
	g_autoptr(GPtrArray) mismatches = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_assert_false(axis_convert_plan_check_output(plan, out, mismatches));
	for (i = 0; i < mismatches->len; i++) {
		if (strcmp(g_ptr_array_index(mismatches, i), expected) == 0)
			return;
	}
	g_error("the output check did not say \"%s\"; it said \"%s\"", expected,
		mismatches->len > 0 ? (const gchar *)g_ptr_array_index(mismatches, 0) : "nothing");
}

static void
set_colour(
	AxisMediaStream *v,
	const gchar     *primaries,
	const gchar     *transfer,
	const gchar     *matrix,
	const gchar     *range
){
	g_free(v->color_primaries);
	g_free(v->color_transfer);
	g_free(v->color_space);
	g_free(v->color_range);
	v->color_primaries = g_strdup(primaries);
	v->color_transfer = g_strdup(transfer);
	v->color_space = g_strdup(matrix);
	v->color_range = g_strdup(range);
}

/* The rule itself: an unstated range is limited, the rest exactly. */
static void
test_colour_same(void)
{
	static const struct
	{
		const gchar *property;
		const gchar *planned;
		const gchar *got;
		gboolean     same;
	} cases[] = {
		{ "range", NULL, NULL, TRUE },
		{ "range", NULL, "tv", TRUE },
		{ "range", NULL, "unknown", TRUE },
		{ "range", "tv", NULL, TRUE },
		{ "range", "tv", "tv", TRUE },
		{ "range", "pc", "pc", TRUE },
		{ "range", NULL, "pc", FALSE },
		{ "range", "tv", "pc", FALSE },
		{ "range", "pc", "tv", FALSE },
		{ "range", "pc", NULL, FALSE },
		{ "range", "pc", "unknown", FALSE },
		{ "matrix", NULL, NULL, TRUE },
		{ "matrix", NULL, "unknown", TRUE },
		{ "matrix", NULL, "bt709", FALSE },
		{ "matrix", "bt709", "bt709", TRUE },
		{ "matrix", "bt709", NULL, FALSE },
		{ "matrix", "bt709", "bt470bg", FALSE },
		{ "transfer", NULL, "unspecified", TRUE },
		{ "transfer", NULL, "bt709", FALSE },
		{ "transfer", "bt709", "smpte170m", FALSE },
		{ "primaries", NULL, "reserved", TRUE },
		{ "primaries", NULL, "bt709", FALSE },
		{ "primaries", "bt709", "unknown", FALSE },
		/* "tv" means limited for the range alone. */
		{ "primaries", NULL, "tv", FALSE },
		{ "matrix", "tv", NULL, FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		if (axis_convert_colour_same(cases[i].property, cases[i].planned, cases[i].got) != cases[i].same)
			g_error("%s: planned %s, got %s: expected %s", cases[i].property, cases[i].planned, cases[i].got,
				cases[i].same ? "the same" : "different");
	}
	g_assert_cmpstr(axis_convert_colour_said("range", NULL), ==, "unstated (limited)");
	g_assert_cmpstr(axis_convert_colour_said("range", "unknown"), ==, "unstated (limited)");
	g_assert_cmpstr(axis_convert_colour_said("range", "tv"), ==, "limited (tv)");
	g_assert_cmpstr(axis_convert_colour_said("range", "pc"), ==, "full (pc)");
	g_assert_cmpstr(axis_convert_colour_said("matrix", NULL), ==, "unstated");
	g_assert_cmpstr(axis_convert_colour_said("transfer", "bt709"), ==, "bt709");
}

/*
 * Spec: an untagged source is converted, the output states a limited range
 * (as ffmpeg's encoders write it) and is accepted by both checks; a full
 * range, against an unstated or a limited one, is rejected, and so is any
 * change of matrix, transfer or primaries, each saying which property, what
 * the plan says and what the output has.
 */
static void
test_colour_meaning(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) untagged = untagged_film();
	g_autoptr(AxisMediaInfo) tagged = film();
	g_autoptr(AxisMediaInfo) full = film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisConvertPlan) tagged_plan = NULL;
	g_autoptr(AxisConvertPlan) full_plan = NULL;
	AxisProfileDefinition def;
	const AxisConvertStream *e;
	guint notes = 0;
	guint i;

	builtin("space_saver", &def);
	plan = build(&def, untagged, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	tagged_plan = build(&def, tagged, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	set_colour(g_ptr_array_index(full->streams, 0), "bt709", "bt709", "bt709", "pc");
	full_plan = build(&def, full, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);

	/* The plan of an untagged source states nothing, and says the range it
	 * leaves unstated is limited. */
	e = out_stream(plan, 0);
	g_assert_null(e->color_range);
	g_assert_null(e->color_space);
	g_assert_null(e->color_transfer);
	g_assert_null(e->color_primaries);
	g_assert_cmpint(e->bit_depth, ==, 10);
	{
		g_autofree gchar *line = joined(plan);

		g_assert_null(strstr(line, "setparams"));
		g_assert_null(strstr(line, "range="));
	}
	for (i = 0; i < plan->notes->len; i++) {
		const gchar *n = g_ptr_array_index(plan->notes, i);

		if (strstr(n, "does not state") != NULL)
			notes++;
		if (strstr(n, "colour range") != NULL)
			g_assert_nonnull(strstr(n, "which means limited range"));
	}
	g_assert_cmpuint(notes, ==, 4);

	/* What x265 and NVENC make of it on ffmpeg 7.1 and 8.1: a limited
	 * range stated, the rest unstated, and 10 bits without a transfer
	 * function, which the probe does not classify. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		set_colour(v, NULL, NULL, NULL, "tv");
		v->hdr_format = AXIS_HDR_NONE;
		v->hdr_uncertain = TRUE;
		g_assert_true(axis_convert_plan_check_output(plan, out, NULL));
		expect_picture(plan, out, NULL);
		/* Or with nothing stated at all, or "unknown" said. */
		set_colour(v, "unknown", "unspecified", NULL, NULL);
		g_assert_true(axis_convert_plan_check_output(plan, out, NULL));
		expect_picture(plan, out, NULL);
		/* The same picture classified SDR is fine too. */
		v->hdr_format = AXIS_HDR_SDR;
		v->hdr_uncertain = FALSE;
		expect_picture(plan, out, NULL);
	}
	/* A source tagged limited, an output that states it, or does not. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		g_assert_cmpstr(out_stream(tagged_plan, 0)->color_range, ==, "tv");
		g_assert_true(axis_convert_plan_check_output(tagged_plan, out, NULL));
		expect_picture(tagged_plan, out, NULL);
		set_colour(v, "bt709", "bt709", "bt709", NULL);
		g_assert_true(axis_convert_plan_check_output(tagged_plan, out, NULL));
		expect_picture(tagged_plan, out, NULL);
	}
	/* Full range against unstated, limited against full, unstated against
	 * full. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		set_colour(g_ptr_array_index(out->streams, 0), NULL, NULL, NULL, "pc");
		expect_picture(plan, out, "The picture's colour range is not the planned one: the plan says unstated "
			"(limited), the output has full (pc).");
		expect_output_says(plan, out, "stream 0: the colour range is full (pc), the plan says unstated (limited)");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);

		set_colour(g_ptr_array_index(out->streams, 0), "bt709", "bt709", "bt709", "pc");
		expect_picture(tagged_plan, out, "The picture's colour range is not the planned one: the plan says limited "
			"(tv), the output has full (pc).");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(full_plan);

		g_assert_cmpstr(out_stream(full_plan, 0)->color_range, ==, "pc");
		expect_picture(full_plan, out, NULL);
		set_colour(g_ptr_array_index(out->streams, 0), "bt709", "bt709", "bt709", "tv");
		expect_picture(full_plan, out, "The picture's colour range is not the planned one: the plan says full (pc), "
			"the output has limited (tv).");
		expect_output_says(full_plan, out, "stream 0: the colour range is limited (tv), the plan says full (pc)");
		set_colour(g_ptr_array_index(out->streams, 0), "bt709", "bt709", "bt709", NULL);
		expect_picture(full_plan, out, "The picture's colour range is not the planned one: the plan says full (pc), "
			"the output has unstated (limited).");
	}
	/* Matrix, transfer and primaries: exactly, and unstated stays so. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);

		set_colour(g_ptr_array_index(out->streams, 0), "bt709", "bt709", "bt470bg", "tv");
		expect_picture(tagged_plan, out, "The picture's colour matrix is not the planned one: the plan says bt709, "
			"the output has bt470bg.");
		expect_output_says(tagged_plan, out, "stream 0: the colour matrix is bt470bg, the plan says bt709");
		set_colour(g_ptr_array_index(out->streams, 0), "bt709", "smpte170m", "bt709", "tv");
		expect_picture(tagged_plan, out, "The picture's transfer function is not the planned one: the plan says "
			"bt709, the output has smpte170m.");
		set_colour(g_ptr_array_index(out->streams, 0), "smpte170m", "bt709", "bt709", "tv");
		expect_picture(tagged_plan, out, "The picture's colour primaries are not the planned ones: the plan says "
			"bt709, the output has smpte170m.");
		set_colour(g_ptr_array_index(out->streams, 0), NULL, "bt709", "bt709", "tv");
		expect_picture(tagged_plan, out, "The picture's colour primaries are not the planned ones: the plan says "
			"bt709, the output has unstated.");
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);

		set_colour(g_ptr_array_index(out->streams, 0), "bt709", NULL, NULL, "tv");
		expect_picture(plan, out, "The picture's colour primaries are not the planned ones: the plan says unstated, "
			"the output has bt709.");
		expect_output_says(plan, out, "stream 0: the colour primaries are bt709, the plan says unstated");
		/* Two at once: each is said. */
		set_colour(g_ptr_array_index(out->streams, 0), NULL, "bt709", "bt709", "pc");
		expect_picture(plan, out, "The picture's colour range is not the planned one: the plan says unstated "
			"(limited), the output has full (pc). The picture's colour matrix is not the planned one: the plan says "
			"unstated, the output has bt709. The picture's transfer function is not the planned one: the plan says "
			"unstated, the output has bt709.");
	}
}

/* Spec: the HDR class and HDR10's values stay as strict.  "Not classified"
 * passes for SDR only where nothing else could have made it. */
static void
test_colour_hdr_class(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) untagged = untagged_film();
	g_autoptr(AxisMediaInfo) tagged = film();
	g_autoptr(AxisMediaInfo) hdr = hdr_film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	g_autoptr(AxisConvertPlan) tagged_plan = NULL;
	g_autoptr(AxisConvertPlan) hdr_plan = NULL;
	AxisProfileDefinition def;
	guint k;

	builtin("space_saver", &def);
	plan = build(&def, untagged, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	tagged_plan = build(&def, tagged, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	hdr_plan = build(&def, hdr, 40 * GB, AXIS_ENCODER_SOFTWARE, caps);

	/* Not classified, where the plan states a transfer function: that is
	 * not the SDR planned. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr_format = AXIS_HDR_NONE;
		v->hdr_uncertain = TRUE;
		expect_picture(tagged_plan, out, "The picture's HDR class is not the planned one: the plan says SDR, the output "
			"has not classified, uncertain.");
		expect_output_says(tagged_plan, out, "stream 0 is not classified, uncertain, the plan says SDR");
	}
	/* Not classified for an untagged source, but with a sign of HDR (each
	 * kind), BT.2020, its frames not read, another stream's frames read,
	 * or a transfer function stated. */
	for (k = 0; k < 10; k++) {
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr_format = AXIS_HDR_NONE;
		v->hdr_uncertain = TRUE;
		switch (k) {
		case 0:
			v->signals.dynamic_other = TRUE;
			break;
		case 1:
			v->signals.hdr10plus = TRUE;
			break;
		case 2:
			v->signals.dovi_record = TRUE;
			break;
		case 3:
			v->signals.dovi_metadata = TRUE;
			break;
		case 4:
			v->signals.has_mastering = TRUE;
			break;
		case 5:
			v->signals.has_content_light = TRUE;
			break;
		case 6:
			set_colour(v, "bt2020", NULL, NULL, NULL);
			break;
		case 7:
			out->main_video = 1;
			break;
		case 8:
			out->frames_unavailable = TRUE;
			break;
		default:
			set_colour(v, NULL, "smpte2084", NULL, NULL);
			break;
		}
		g_test_message("case %u", k);
		g_assert_false(axis_convert_plan_check_picture(plan, out, NULL));
		g_assert_false(axis_convert_plan_check_output(plan, out, NULL));
	}
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->hdr_format = AXIS_HDR_NONE;
		v->hdr_uncertain = TRUE;
		v->hdr_note = "Dynamic HDR metadata of an unrecognised kind is present.";
		v->signals.dynamic_other = TRUE;
		expect_picture(plan, out, "The picture's HDR class is not the planned one: the plan says SDR, the output has "
			"not classified, uncertain (the probe says: Dynamic HDR metadata of an unrecognised kind is present.).");
	}
	/* Not classified at 8 bits (Balanced makes 8-bit HEVC): the probe
	 * classifies an 8-bit picture without a transfer function as SDR, so
	 * this is something else. */
	{
		g_autoptr(AxisConvertPlan) eight = NULL;
		g_autoptr(AxisMediaInfo) out = NULL;
		AxisProfileDefinition balanced;
		AxisMediaStream *v;

		builtin("balanced", &balanced);
		eight = build(&balanced, untagged, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
		g_assert_cmpint(out_stream(eight, 0)->bit_depth, ==, 8);
		out = perfect_output(eight);
		v = g_ptr_array_index(out->streams, 0);
		expect_picture(eight, out, NULL);
		v->hdr_format = AXIS_HDR_NONE;
		v->hdr_uncertain = TRUE;
		expect_picture(eight, out, "The picture's HDR class is not the planned one: the plan says SDR, the output has "
			"not classified, uncertain.");
		g_assert_false(axis_convert_plan_check_output(eight, out, NULL));
	}
	/* SDR made HLG. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);

		((AxisMediaStream *)g_ptr_array_index(out->streams, 0))->hdr_format = AXIS_HDR_HLG;
		expect_picture(tagged_plan, out, "The picture's HDR class is not the planned one: the plan says SDR, the output "
			"has HLG.");
	}
	/* HDR10: uncertain, a value altered, the values lost. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(hdr_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		expect_picture(hdr_plan, out, NULL);
		v->hdr_uncertain = TRUE;
		expect_picture(hdr_plan, out, "The picture's HDR class is not the planned one: the plan says HDR10, the output "
			"has HDR10, uncertain.");
		v->hdr_uncertain = FALSE;
		v->hdr10.white_x++;
		expect_picture(hdr_plan, out, "The picture's HDR10 values are not the planned ones: the plan says mastering "
			"display G(13250,34500) B(7500,3000) R(34000,16000) WP(15635,16450) L(10000000,50), content light "
			"1000,400, the output has mastering display G(13250,34500) B(7500,3000) R(34000,16000) WP(15636,16450) "
			"L(10000000,50), content light 1000,400.");
		memset(&v->hdr10, 0, sizeof(v->hdr10));
		v->signals.has_mastering = FALSE;
		v->signals.has_content_light = FALSE;
		expect_picture(hdr_plan, out, "The picture's HDR10 values are not the planned ones: the plan says mastering "
			"display G(13250,34500) B(7500,3000) R(34000,16000) WP(15635,16450) L(10000000,50), content light "
			"1000,400, the output has none.");
	}
	/* HDR10 values on an SDR picture, kept or not by the probe. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);
		AxisMediaStream *v = g_ptr_array_index(out->streams, 0);

		v->signals.has_content_light = TRUE;
		expect_picture(tagged_plan, out, "The picture's HDR10 values are not the planned ones: the plan says none, the "
			"output has none (and values the probe does not keep: out of range, or not the same in every entry).");
	}
	/* No picture where the plan has one. */
	{
		g_autoptr(AxisMediaInfo) out = perfect_output(tagged_plan);

		((AxisMediaStream *)g_ptr_array_index(out->streams, 0))->kind = AXIS_STREAM_AUDIO;
		expect_picture(tagged_plan, out, "Stream 0 of the output is not a picture (its kind is audio), where the plan "
			"has its encoded picture.");
		((AxisMediaStream *)g_ptr_array_index(out->streams, 0))->kind = AXIS_STREAM_VIDEO;
		((AxisMediaStream *)g_ptr_array_index(out->streams, 0))->bit_depth = 8;
		expect_picture(tagged_plan, out, "The picture is 1920x1080 at 8 bits; the plan says 1920x1080 at 10 bits.");
	}
	expect_picture(tagged_plan, NULL, "The output has no stream 0, where the plan has its encoded picture.");
}

/* ── The vector's grammar ───────────────────────────────────────── */

static void
test_args_grammar(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_test_convert_caps_all();
	g_autoptr(AxisMediaInfo) info = film();
	g_autoptr(AxisConvertPlan) plan = NULL;
	AxisProfileDefinition def;
	guint n;
	guint i;

	builtin("space_saver", &def);
	plan = build(&def, info, 8 * GB, AXIS_ENCODER_SOFTWARE, caps);
	n = g_strv_length(plan->args);
	g_assert_true(axis_convert_args_valid((const gchar *const *)plan->args, info, NULL));

	/* Each kind of tampering fails it. */
	{
		static const struct
		{
			const gchar *find;
			const gchar *put;
		} edits[] = {
			{ "0:5", "0:9" },	/* a map to no stream of the file */
			{ "-c:5", "-c:6" },	/* an option of no output stream */
			{ "medium", "placebo" },
			{ "26", "52" },
			{ "fd:", "file:/etc/passwd" },
			{ "fd", "fd,file" },
			{ "3", "0" },
			{ "-disposition:3", "-disposition:x" },
			{ "+default-forced", "default" },
			{ "copy", "rawvideo" },
			{ "error", "debug" },
			{ "pipe:1", "pipe:2" },
			{ "matroska", "hls" },
			{ "passthrough", "infer" },
			{ "BPS=", "BPS=1" },
			{ "-nostdin", "-y" },
			{ "-map_metadata", "-map_metadata:g" }
		};
		guint e;

		for (e = 0; e < G_N_ELEMENTS(edits); e++) {
			g_auto(GStrv) copy = g_strdupv(plan->args);
			gboolean changed = FALSE;
			g_autofree gchar *problem = NULL;

			for (i = 0; i < n && !changed; i++) {
				if (strcmp(copy[i], edits[e].find) == 0) {
					g_free(copy[i]);
					copy[i] = g_strdup(edits[e].put);
					changed = TRUE;
				}
			}
			g_assert_true(changed);
			if (axis_convert_args_valid((const gchar *const *)copy, info, &problem))
				g_error("tampering not caught: %s -> %s", edits[e].find, edits[e].put);
			g_assert_nonnull(problem);
		}
	}
	/* A title, empty, a forbidden character, cut short, something after
	 * the output. */
	{
		g_auto(GStrv) copy = g_strdupv(plan->args);

		g_free(copy[n - 6]);
		copy[n - 6] = g_strdup("evil title");
		g_assert_false(axis_convert_args_valid((const gchar *const *)copy, info, NULL));
		g_free(copy[n - 6]);
		copy[n - 6] = g_strdup("");
		g_assert_false(axis_convert_args_valid((const gchar *const *)copy, info, NULL));
		g_free(copy[n - 6]);
		copy[n - 6] = g_strdup("$x");
		g_assert_false(axis_convert_args_valid((const gchar *const *)copy, info, NULL));
	}
	{
		g_auto(GStrv) copy = g_strdupv(plan->args);

		g_free(copy[n - 1]);
		copy[n - 1] = NULL;
		g_assert_false(axis_convert_args_valid((const gchar *const *)copy, info, NULL));
	}
	{
		gchar **longer = g_new0(gchar *, n + 2);

		for (i = 0; i < n; i++)
			longer[i] = g_strdup(plan->args[i]);
		longer[n] = g_strdup("-y");
		g_assert_false(axis_convert_args_valid((const gchar *const *)longer, info, NULL));
		g_strfreev(longer);
	}
	g_assert_false(axis_convert_args_valid(NULL, info, NULL));
}

/* ── Progress ───────────────────────────────────────────────────── */

static void
feed(
	AxisConvertProgress *p,
	const gchar         *text
){
	axis_convert_progress_feed(p, text, strlen(text));
}

/* Spec: Progress reported (about one half), from the frames and from the
 * time; and what 7.1 and 8.1 printed for a file with a subtitle stream. */
static void
test_progress_half_way(void)
{
	AxisConvertProgress p;
	gdouble fraction = -1;

	/* 6 s at 24 fps: 144 frames. */
	axis_convert_progress_init(&p, 6000, 24.0);
	g_assert_cmpint(p.expected_frames, ==, 144);
	feed(&p, "frame=72\nfps=5.00\nstream_0_0_q=28.0\nbitrate=N/A\ntotal_size=123456\n"
		"out_time_us=3000000\nout_time_ms=3000000\nout_time=00:00:03.000000\ndup_frames=0\ndrop_frames=0\n"
		"speed=2.53x\nprogress=continue\n");
	g_assert_true(axis_convert_progress_fraction(&p, &fraction));
	g_assert_cmpfloat_with_epsilon(fraction, 0.5, 0.01);
	g_assert_cmpint(p.total_size, ==, 123456);
	g_assert_cmpfloat_with_epsilon(p.speed, 2.53, 0.001);
	g_assert_false(p.ended);

	/* Without a frame rate, the time. */
	axis_convert_progress_init(&p, 6000, -1.0);
	feed(&p, "out_time_us=3000000\nprogress=continue\n");
	g_assert_true(axis_convert_progress_fraction(&p, &fraction));
	g_assert_cmpfloat_with_epsilon(fraction, 0.5, 0.01);

	/* Recorded from a file with a subtitle: the time goes back at the end;
	 * an attachment makes it N/A; speed is padded or N/A.  The frames
	 * decide; the time never goes back. */
	{
		static const gchar recorded[] =
			"frame=60\nfps=0.00\nstream_0_0_q=36.0\nbitrate=N/A\ntotal_size=3334\nout_time_us=1000000\n"
			"out_time_ms=1000000\nout_time=00:00:01.000000\ndup_frames=0\ndrop_frames=0\nspeed=   2x\n"
			"progress=continue\n"
			"frame=129\nfps=26.09\nstream_0_0_q=36.0\nbitrate=   5.0kbits/s\ntotal_size=3334\n"
			"out_time_us=5333333\nout_time_ms=5333333\nout_time=00:00:05.333333\ndup_frames=0\ndrop_frames=0\n"
			"speed=5.33x\nprogress=continue\n"
			"frame=144\nfps=27.32\nstream_0_0_q=-1.0\nbitrate=3277.9kbits/s\ntotal_size=409740\n"
			"out_time_us=1000000\nout_time_ms=1000000\nout_time=00:00:01.000000\ndup_frames=0\ndrop_frames=0\n"
			"speed=0.922x\nprogress=end\n";
		gsize i;

		axis_convert_progress_init(&p, 6000, 24.0);
		/* Fed a byte at a time. */
		for (i = 0; recorded[i] != '\0'; i++)
			axis_convert_progress_feed(&p, recorded + i, 1);
		g_assert_true(p.ended);
		g_assert_cmpuint(p.blocks, ==, 3);
		g_assert_cmpint(p.frame, ==, 144);
		g_assert_cmpint(p.out_time_us, ==, 5333333);
		g_assert_cmpint(p.total_size, ==, 409740);
		g_assert_true(axis_convert_progress_fraction(&p, &fraction));
		g_assert_cmpfloat_with_epsilon(fraction, 1.0, 0.001);
	}
}

/* Spec: Unknown duration: no fraction, nothing fails. */
static void
test_progress_unknown_duration(void)
{
	AxisConvertProgress p;
	gdouble fraction = -1;

	axis_convert_progress_init(&p, -1, 24.0);
	feed(&p, "frame=72\nout_time_us=3000000\nprogress=continue\n");
	g_assert_cmpint(p.frame, ==, 72);
	g_assert_false(axis_convert_progress_fraction(&p, &fraction));
	g_assert_cmpfloat(fraction, ==, -1);
	axis_convert_progress_init(&p, 0, 0.0);
	g_assert_false(axis_convert_progress_fraction(&p, &fraction));
	/* Nothing said at all. */
	axis_convert_progress_init(&p, 6000, 24.0);
	feed(&p, "progress=continue\n");
	g_assert_false(axis_convert_progress_fraction(&p, &fraction));
}

/* Everything optional and bounded: N/A, signs, huge numbers, long lines,
 * NUL bytes, CRLF, a block never finished, more than done. */
static void
test_progress_hostile(void)
{
	AxisConvertProgress p;
	gdouble fraction = -1;
	g_autofree gchar *long_line = g_strnfill(10000, 'x');
	g_autofree gchar *huge = g_strdup_printf("frame=%s\nprogress=continue\n", "99999999999999999999999");

	axis_convert_progress_init(&p, 6000, 24.0);
	feed(&p, "frame=N/A\nout_time_us=-500\nspeed=N/A\ntotal_size=-1\nprogress=continue\n");
	g_assert_cmpint(p.frame, ==, -1);
	g_assert_cmpint(p.out_time_us, ==, -1);
	g_assert_cmpfloat(p.speed, ==, -1.0);
	g_assert_false(axis_convert_progress_fraction(&p, &fraction));
	feed(&p, huge);
	g_assert_cmpint(p.frame, ==, -1);
	/* A line that runs on is ignored whole, the next one counts. */
	feed(&p, "frame=");
	feed(&p, long_line);
	feed(&p, "\nframe=12\r\nprogress=continue\r\n");
	g_assert_cmpint(p.frame, ==, 12);
	/* A NUL spoils its line. */
	{
		static const gchar with_nul[] = "frame=1\0" "3\nprogress=continue\n";

		axis_convert_progress_feed(&p, with_nul, sizeof(with_nul) - 1);
	}
	g_assert_cmpint(p.frame, ==, 12);
	/* Values only count when their block ends. */
	feed(&p, "frame=100\n");
	g_assert_cmpint(p.frame, ==, 12);
	/* More frames than expected: still at most 1. */
	feed(&p, "frame=1000\nprogress=end\n");
	g_assert_true(axis_convert_progress_fraction(&p, &fraction));
	g_assert_cmpfloat(fraction, ==, 1.0);
	/* Speeds of odd shapes. */
	axis_convert_progress_init(&p, 6000, 24.0);
	feed(&p, "speed=1e9x\nprogress=continue\nspeed=-2x\nprogress=continue\nspeed=x\n"
		"progress=continue\nspeed=nanx\nprogress=continue\n");
	g_assert_cmpfloat(p.speed, ==, -1.0);
	/* Absurd durations and rates are not known. */
	axis_convert_progress_init(&p, G_MAXINT64, 1e30);
	g_assert_cmpint(p.duration_us, ==, -1);
	g_assert_cmpint(p.expected_frames, ==, -1);
}

/* ── Capability parsers ─────────────────────────────────────────── */

static void
test_capability_parsers(void)
{
	static const gchar listing[] =
		"Encoders:\n V..... = Video\n A..... = Audio\n ------\n V....D libx264              libx264 H.264 (codec h264)\n"
		" V....D libx265              libx265 H.265 / HEVC (codec hevc)\n"
		" V....D hevc_nvenc           NVIDIA NVENC hevc encoder (codec hevc)\n"
		" A....D aac                  AAC (Advanced Audio Coding)\n S..... srt                  SubRip subtitle\n";
	g_autofree gchar *v1 = NULL;
	g_autofree gchar *v2 = NULL;

#define PARSE(text) axis_convert_parse_version(text, strlen(text))
	v1 = PARSE("ffmpeg version 8.1.3 Copyright (c) 2000-2026");
	g_assert_cmpstr(v1, ==, "8.1.3");
	v2 = PARSE("ffmpeg version 7.1.5-0+deb13u1 Copyright");
	g_assert_cmpstr(v2, ==, "7.1.5-0+deb13u1");
	g_assert_null(PARSE("ffprobe version 8.1.3"));
	g_assert_null(PARSE("ffmpeg version $(x) y"));
	g_assert_null(PARSE("ffmpeg version "));
	g_assert_null(axis_convert_parse_version(NULL, 0));
#undef PARSE

	g_assert_true(axis_convert_encoders_lists(listing, strlen(listing), "libx265"));
	g_assert_true(axis_convert_encoders_lists(listing, strlen(listing), "hevc_nvenc"));
	g_assert_true(axis_convert_encoders_lists(listing, strlen(listing), "aac"));
	g_assert_true(axis_convert_encoders_lists(listing, strlen(listing), "srt"));
	g_assert_false(axis_convert_encoders_lists(listing, strlen(listing), "hevc_vaapi"));
	/* Not a prefix, not the description, not the legend. */
	g_assert_false(axis_convert_encoders_lists(listing, strlen(listing), "libx26"));
	g_assert_false(axis_convert_encoders_lists(listing, strlen(listing), "NVIDIA"));
	g_assert_false(axis_convert_encoders_lists(listing, strlen(listing), "="));
	g_assert_false(axis_convert_encoders_lists(listing, strlen(listing), ""));
}

/* The capability record keeps its order: 10 bits needs usable, HDR10
 * needs 10 bits; H.264 has no 10 bits in the table. */
static void
test_capability_order(void)
{
	g_autoptr(AxisEncoderCapabilities) caps = axis_encoder_capabilities_new();
	g_autoptr(AxisEncoderCapabilities) copy = NULL;
	const AxisEncoderCapability *c;

	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), FALSE, TRUE, TRUE, "x");
	c = axis_encoder_capabilities_get(caps, axis_convert_encoder_named("libx265"));
	g_assert_true(c->measured);
	g_assert_false(c->ten_bit);
	g_assert_false(c->hdr10);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, TRUE, TRUE, NULL);
	c = axis_encoder_capabilities_get(caps, axis_convert_encoder_named("libx264"));
	g_assert_true(c->usable);
	g_assert_false(c->ten_bit);
	g_assert_false(axis_encoder_capabilities_get(caps, axis_convert_encoder_named("hevc_vaapi"))->measured);
	copy = axis_encoder_capabilities_copy(caps);
	g_assert_cmpstr(axis_encoder_capabilities_get(copy, axis_convert_encoder_named("libx265"))->reason, ==, "x");
	g_assert_cmpuint(axis_convert_encoder_index(axis_convert_encoder_named("h264_vaapi")), ==, 5);
	g_assert_null(axis_convert_encoder_named("libaom-av1"));
}

/* The refusals have names and sentences, none empty. */
static void
test_refusal_names(void)
{
	guint i;

	for (i = 0; i < AXIS_CONVERT_N_REFUSALS; i++) {
		g_assert_cmpstr(axis_convert_refusal_to_string((AxisConvertRefusal)i), !=, "unknown");
		g_assert_cmpuint(strlen(axis_convert_refusal_describe((AxisConvertRefusal)i)), >, 5);
	}
	g_assert_cmpstr(axis_convert_refusal_to_string(AXIS_CONVERT_N_REFUSALS), ==, "unknown");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/convert/plan/golden/space-saver", test_golden_space_saver);
	g_test_add_func("/convert/plan/golden/hdr10", test_golden_hdr10);
	g_test_add_func("/convert/plan/golden/max-compat", test_golden_max_compat);
	g_test_add_func("/convert/plan/golden/balanced", test_golden_balanced);
	g_test_add_func("/convert/plan/hostile/text", test_hostile_text);
	g_test_add_func("/convert/plan/hostile/language", test_hostile_language);
	g_test_add_func("/convert/plan/streams/dropped-language", test_dropped_language);
	g_test_add_func("/convert/plan/streams/added-stereo", test_added_stereo);
	g_test_add_func("/convert/plan/streams/mapping-order", test_mapping_order);
	g_test_add_func("/convert/plan/colour/unstated", test_unstated_colour);
	g_test_add_func("/convert/plan/colour/hlg", test_hlg);
	g_test_add_func("/convert/plan/refusals", test_refusals);
	g_test_add_func("/convert/plan/capabilities-decide", test_capabilities_decide);
	g_test_add_func("/convert/plan/choose-next-encoder", test_choose_next_encoder);
	g_test_add_func("/convert/plan/quality-by-family", test_quality_by_family);
	g_test_add_func("/convert/plan/output-check", test_output_check);
	g_test_add_func("/convert/plan/colour/same", test_colour_same);
	g_test_add_func("/convert/plan/colour/by-meaning", test_colour_meaning);
	g_test_add_func("/convert/plan/colour/hdr-class", test_colour_hdr_class);
	g_test_add_func("/convert/plan/args-grammar", test_args_grammar);
	g_test_add_func("/convert/progress/half-way", test_progress_half_way);
	g_test_add_func("/convert/progress/unknown-duration", test_progress_unknown_duration);
	g_test_add_func("/convert/progress/hostile", test_progress_hostile);
	g_test_add_func("/convert/caps/parsers", test_capability_parsers);
	g_test_add_func("/convert/caps/order", test_capability_order);
	g_test_add_func("/convert/refusal-names", test_refusal_names);

	return g_test_run();
}
