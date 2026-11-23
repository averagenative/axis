/*
 * test-probe-map.c - the JSON reader and the mapper from ffprobe's output
 * to the model: recorded and constructed fixtures, HDR classification,
 * derived facts, and hostile or absurd input
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No database and no ffprobe needed: the recorded fixtures are what ffprobe
 * 8.1.3 printed for files made with ffmpeg's test sources
 * (tests/fixtures/probe/record.sh).  The constructed fixtures are written
 * by hand (tests/fixtures/probe/constructed/README.md): Dolby Vision,
 * HDR10+ and the like cannot be made here, so tests that use them say so.
 */

#include <glib.h>
#include <string.h>

#include "probe/axis-probe-json.h"
#include "probe/axis-probe-model.h"

#define RECORDED    "tests/fixtures/probe/recorded"
#define CONSTRUCTED "tests/fixtures/probe/constructed"

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
read_file(
	const gchar *dir,
	const gchar *name,
	const gchar *suffix,
	gsize       *length
){
	g_autofree gchar *file = g_strdup_printf("%s%s", name, suffix);
	g_autofree gchar *path = g_build_filename(dir, file, NULL);
	g_autoptr(GError) error = NULL;
	gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, length, &error))
		g_error("cannot read fixture %s: %s (run the tests from the source directory)", path, error->message);
	return contents;
}

/* Maps a fixture; frames from NAME.frames.json when @frames. */
static AxisMediaInfo *
map_fixture(
	const gchar *dir,
	const gchar *name,
	gboolean     frames
){
	g_autofree gchar *streams = NULL;
	g_autofree gchar *frame_json = NULL;
	g_autofree gchar *error = NULL;
	AxisMediaInfo *info = NULL;
	gsize streams_length = 0;
	gsize frames_length = 0;

	streams = read_file(dir, name, ".json", &streams_length);
	if (frames)
		frame_json = read_file(dir, name, ".frames.json", &frames_length);
	if (!axis_probe_map(streams, streams_length, frame_json, frames_length, AXIS_PROBE_UNKNOWN, &info, &error))
		g_error("fixture %s did not map: %s", name, error);
	g_assert_nonnull(info);
	return info;
}

static AxisMediaInfo *
map_text(
	const gchar *text,
	const gchar *frames
){
	g_autofree gchar *error = NULL;
	AxisMediaInfo *info = NULL;

	if (!axis_probe_map(text, strlen(text), frames, frames != NULL ? strlen(frames) : 0, AXIS_PROBE_UNKNOWN,
	                    &info, &error))
		g_error("text did not map: %s", error);
	return info;
}

static gchar *
map_error(const gchar *text)
{
	AxisMediaInfo *info = NULL;
	gchar *error = NULL;

	g_assert_false(axis_probe_map(text, strlen(text), NULL, 0, AXIS_PROBE_UNKNOWN, &info, &error));
	g_assert_null(info);
	g_assert_nonnull(error);
	return error;
}

static const AxisMediaStream *
stream_at(
	const AxisMediaInfo *info,
	guint                position
){
	g_assert_cmpuint(position, <, info->streams->len);
	return g_ptr_array_index(info->streams, position);
}

static const AxisMediaStream *
first_of(
	const AxisMediaInfo *info,
	AxisStreamKind       kind
){
	guint i;

	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind == kind)
			return s;
	}
	g_error("no stream of kind %s", axis_stream_kind_to_string(kind));
	return NULL;
}

/* A video stream with the fields the classifier reads. */
static AxisMediaStream *
video_stream(
	gint         bit_depth,
	const gchar *transfer,
	const gchar *primaries
){
	AxisMediaStream *s = g_new0(AxisMediaStream, 1);

	s->kind = AXIS_STREAM_VIDEO;
	s->codec = g_strdup("hevc");
	s->bit_depth = bit_depth;
	s->color_transfer = g_strdup(transfer);
	s->color_primaries = g_strdup(primaries);
	s->width = 3840;
	s->height = 2160;
	axis_hdr_signals_init(&s->signals);
	return s;
}

/* ── The JSON reader ────────────────────────────────────────────── */

static void
test_json_values(void)
{
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *problem = NULL;
	const AxisJsonNode *root;
	gint64 whole = 0;
	gdouble real = 0;
	gboolean flag = FALSE;
	const gchar *text =
		"{\"a\": 1, \"b\": \"2\", \"c\": 3.5, \"d\": \"4.25\", \"e\": true, \"f\": null, \"g\": [1, [2]],"
		" \"h\": \"x\\u00e9\\n\", \"a\": 99, \"i\": 1e2, \"j\": -0, \"k\": \"N/A\", \"l\": 1.5}";

	doc = axis_json_parse(text, strlen(text), &problem);
	g_assert_nonnull(doc);
	g_assert_null(problem);
	root = axis_json_doc_get_root(doc);
	g_assert_cmpint(axis_json_type(root), ==, AXIS_JSON_OBJECT);

	/* Numbers arrive as numbers or as strings. */
	g_assert_true(axis_json_get_integer(axis_json_member(root, "a"), 0, 10, &whole));
	g_assert_cmpint(whole, ==, 1);	/* the first of two members called "a" */
	g_assert_true(axis_json_get_integer(axis_json_member(root, "b"), 0, 10, &whole));
	g_assert_cmpint(whole, ==, 2);
	g_assert_true(axis_json_get_double(axis_json_member(root, "c"), 0, 10, &real));
	g_assert_cmpfloat(real, ==, 3.5);
	g_assert_true(axis_json_get_double(axis_json_member(root, "d"), 0, 10, &real));
	g_assert_cmpfloat(real, ==, 4.25);
	g_assert_true(axis_json_get_integer(axis_json_member(root, "i"), 0, 1000, &whole));
	g_assert_cmpint(whole, ==, 100);
	g_assert_true(axis_json_get_boolean(axis_json_member(root, "e"), &flag));
	g_assert_true(flag);
	g_assert_cmpstr(axis_json_string(axis_json_member(root, "h")), ==, "x\xc3\xa9\n");

	/* What is not a value in range is not one. */
	g_assert_false(axis_json_get_integer(axis_json_member(root, "a"), 5, 10, &whole));
	g_assert_false(axis_json_get_integer(axis_json_member(root, "l"), 0, 10, &whole));	/* fractional */
	g_assert_false(axis_json_get_integer(axis_json_member(root, "k"), 0, 10, &whole));	/* "N/A" */
	g_assert_false(axis_json_get_double(axis_json_member(root, "k"), 0, 10, &real));
	g_assert_false(axis_json_get_integer(axis_json_member(root, "f"), 0, 10, &whole));	/* null */
	g_assert_false(axis_json_get_integer(axis_json_member(root, "missing"), 0, 10, &whole));
	g_assert_cmpuint(axis_json_length(axis_json_member(root, "g")), ==, 2);
	g_assert_null(axis_json_member(axis_json_member(root, "g"), "x"));
	g_assert_null(axis_json_element(root, 0));
}

static void
test_json_refused(void)
{
	static const gchar *const bad[] = {
		"", "   ", "{", "[1,", "{\"a\"}", "{\"a\":}", "{a:1}", "[1 2]", "[1,]", "{\"a\":1,}", "nul", "tru",
		"01", "1.", ".5", "+1", "1e", "-", "\"abc", "\"a\\x\"", "\"a\\u12\"", "\"\x01\"", "[1] x", "{}{}",
		"'a'", "NaN", "Infinity", "[\"a\" \"b\"]", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *problem = NULL;
		AxisJsonDoc *doc = axis_json_parse(bad[i], strlen(bad[i]), &problem);

		if (doc != NULL)
			g_error("accepted: %s", bad[i]);
		g_assert_nonnull(problem);
	}
}

/* Spec: Unexpected tool output (enormous or unexpectedly shaped): the
 * reader's memory is bounded by its limits, whatever the input says. */
static void
test_json_limits(void)
{
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autofree gchar *problem = NULL;
	const AxisJsonNode *root;
	guint i;

	/* Nesting. */
	for (i = 0; i <= AXIS_JSON_MAX_DEPTH + 1; i++)
		g_string_append_c(text, '[');
	for (i = 0; i <= AXIS_JSON_MAX_DEPTH + 1; i++)
		g_string_append_c(text, ']');
	g_assert_null(axis_json_parse(text->str, text->len, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);

	g_string_set_size(text, 0);
	for (i = 0; i < AXIS_JSON_MAX_DEPTH; i++)
		g_string_append_c(text, '[');
	for (i = 0; i < AXIS_JSON_MAX_DEPTH; i++)
		g_string_append_c(text, ']');
	doc = axis_json_parse(text->str, text->len, NULL);
	g_assert_nonnull(doc);
	g_clear_pointer(&doc, axis_json_doc_free);

	/* Count of values: one more than the limit is refused whole. */
	g_string_set_size(text, 0);
	g_string_append_c(text, '[');
	for (i = 0; i < AXIS_JSON_MAX_NODES; i++)
		g_string_append(text, i == 0 ? "0" : ",0");
	g_string_append_c(text, ']');
	g_assert_null(axis_json_parse(text->str, text->len, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);

	/* A long string keeps its start, cut at a character boundary. */
	g_string_set_size(text, 0);
	g_string_append(text, "[\"");
	for (i = 0; i < AXIS_JSON_MAX_STRING; i++)
		g_string_append(text, "\xc3\xa9");	/* two bytes each */
	g_string_append(text, "\"]");
	doc = axis_json_parse(text->str, text->len, NULL);
	g_assert_nonnull(doc);
	root = axis_json_doc_get_root(doc);
	g_assert_cmpuint(strlen(axis_json_string(axis_json_element(root, 0))), <=, AXIS_JSON_MAX_STRING);
	g_assert_true(g_utf8_validate(axis_json_string(axis_json_element(root, 0)), -1, NULL));
	g_clear_pointer(&doc, axis_json_doc_free);

	/* A number may not be arbitrarily long. */
	g_string_set_size(text, 0);
	g_string_append(text, "[1");
	for (i = 0; i < AXIS_JSON_MAX_NUMBER; i++)
		g_string_append_c(text, '0');
	g_string_append_c(text, ']');
	g_assert_null(axis_json_parse(text->str, text->len, NULL));
}

static void
test_json_text(void)
{
	/* Bytes that are not UTF-8, a lone surrogate, NUL: replaced. */
	static const gchar text[] = "[\"a\xff" "b\", \"\\ud800x\", \"\\u0000\", \"\\ud83d\\ude00\", \"\\udc00\"]";
	g_autoptr(AxisJsonDoc) doc = axis_json_parse(text, sizeof(text) - 1, NULL);
	const AxisJsonNode *root;

	g_assert_nonnull(doc);
	root = axis_json_doc_get_root(doc);
	g_assert_cmpstr(axis_json_string(axis_json_element(root, 0)), ==, "a\xef\xbf\xbd" "b");
	g_assert_cmpstr(axis_json_string(axis_json_element(root, 1)), ==, "\xef\xbf\xbd" "x");
	g_assert_cmpstr(axis_json_string(axis_json_element(root, 2)), ==, "\xef\xbf\xbd");
	g_assert_cmpstr(axis_json_string(axis_json_element(root, 3)), ==, "\xf0\x9f\x98\x80");
	g_assert_cmpstr(axis_json_string(axis_json_element(root, 4)), ==, "\xef\xbf\xbd");
}

/* ── Recorded fixtures: video ───────────────────────────────────── */

/* Spec: Video file (one video stream, two audio, two subtitle). */
static void
test_video_file(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "h264-sdr", TRUE);
	const AxisMediaStream *video = stream_at(info, 0);
	const AxisMediaStream *audio0 = stream_at(info, 1);
	const AxisMediaStream *audio1 = stream_at(info, 2);
	const AxisMediaStream *sub0 = stream_at(info, 3);
	const AxisMediaStream *sub1 = stream_at(info, 4);

	g_assert_cmpstr(info->container, ==, "matroska,webm");
	g_assert_cmpint(info->duration_ms, ==, 1023);
	g_assert_cmpint(info->stream_count, ==, 5);
	g_assert_cmpint(info->video_streams, ==, 1);
	g_assert_cmpint(info->audio_streams, ==, 2);
	g_assert_cmpint(info->subtitle_streams, ==, 2);
	g_assert_cmpint(info->chapter_count, ==, 0);

	g_assert_cmpint(video->kind, ==, AXIS_STREAM_VIDEO);
	g_assert_cmpstr(video->codec, ==, "h264");
	g_assert_cmpstr(video->profile, ==, "High");
	g_assert_cmpint(video->width, ==, 320);
	g_assert_cmpint(video->height, ==, 240);
	g_assert_cmpint(video->bit_depth, ==, 8);
	g_assert_cmpfloat_with_epsilon(video->frame_rate, 24.0, 0.001);
	g_assert_cmpstr(video->pixel_format, ==, "yuv420p");

	g_assert_cmpint(audio0->kind, ==, AXIS_STREAM_AUDIO);
	g_assert_cmpstr(audio0->codec, ==, "aac");
	g_assert_cmpstr(audio0->channel_layout, ==, "mono");
	g_assert_cmpint(audio0->channels, ==, 1);
	g_assert_cmpint(audio0->sample_rate, ==, 44100);
	g_assert_cmpstr(audio0->language, ==, "eng");
	g_assert_cmpstr(audio1->language, ==, "fra");
	/* ffmpeg marked none of them default: the first is the main one. */
	g_assert_false(audio0->is_default);
	g_assert_false(audio1->is_default);
	g_assert_cmpint(audio0->lossless, ==, AXIS_TRI_NO);

	g_assert_cmpint(sub0->kind, ==, AXIS_STREAM_SUBTITLE);
	g_assert_cmpstr(sub0->codec, ==, "subrip");
	g_assert_cmpint(sub0->subtitle_kind, ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpstr(sub0->language, ==, "eng");
	g_assert_false(sub0->is_forced);
	g_assert_cmpstr(sub1->language, ==, "fra");
	g_assert_true(sub1->is_forced);

	/* The file-level summary. */
	g_assert_cmpint(info->main_video, ==, 0);
	g_assert_cmpint(info->main_audio, ==, 1);
	g_assert_cmpstr(info->video_codec, ==, "h264");
	g_assert_cmpstr(info->resolution_class, ==, "sd");
	g_assert_cmpstr(info->audio_codec, ==, "aac");
	g_assert_cmpint(info->interlaced, ==, AXIS_TRI_NO);
	g_assert_cmpint(info->vfr_suspected, ==, AXIS_TRI_NO);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_SDR);
	g_assert_false(info->hdr_uncertain);
	g_assert_cmpint(video->hdr_format, ==, AXIS_HDR_SDR);
}

/* Spec: Missing bitrate is estimated.  Matroska states none of the
 * streams' bitrates, so all are derived from the container's. */
static void
test_missing_bitrate_estimated(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "h264-sdr", TRUE);
	gint64 total = 0;
	guint i;

	g_assert_cmpint(info->bitrate, >, 0);
	g_assert_false(info->bitrate_estimated);	/* the container said it */
	for (i = 0; i < info->streams->len; i++) {
		const AxisMediaStream *s = g_ptr_array_index(info->streams, i);

		if (s->kind != AXIS_STREAM_VIDEO && s->kind != AXIS_STREAM_AUDIO)
			continue;
		g_assert_cmpint(s->bitrate, >, 0);
		g_assert_true(s->bitrate_estimated);
		total += s->bitrate;
	}
	/* Together they never claim more than the container holds. */
	g_assert_cmpint(total, <=, info->bitrate);
	g_assert_true(info->bits_per_pixel > 0);
	g_assert_true(info->bits_per_pixel_estimated);
	g_assert_cmpint(info->audio_bitrate_total, >, 0);
	g_assert_true(info->audio_bitrate_total_estimated);
}

/* The container's bitrate is derived from the size when it states none. */
static void
test_container_bitrate_from_size(void)
{
	g_autofree gchar *error = NULL;
	AxisMediaInfo *info = NULL;
	const gchar *json =
		"{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"h264\",\"width\":1920,"
		"\"height\":1080,\"pix_fmt\":\"yuv420p\",\"avg_frame_rate\":\"25/1\",\"r_frame_rate\":\"25/1\"}],"
		"\"format\":{\"format_name\":\"mpegts\",\"duration\":\"10.0\"}}";

	g_assert_true(axis_probe_map(json, strlen(json), NULL, 0, 12500000, &info, &error));
	g_assert_cmpint(info->bitrate, ==, 10000000);
	g_assert_true(info->bitrate_estimated);
	g_assert_cmpint(stream_at(info, 0)->bitrate, >, 9000000);
	g_assert_true(stream_at(info, 0)->bitrate_estimated);
	axis_media_info_free(info);

	/* Without the size there is nothing to derive from. */
	g_assert_true(axis_probe_map(json, strlen(json), NULL, 0, AXIS_PROBE_UNKNOWN, &info, &error));
	g_assert_cmpint(info->bitrate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(stream_at(info, 0)->bitrate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpfloat(info->bits_per_pixel, <, 0);
	axis_media_info_free(info);
}

static void
test_interlaced_and_vfr(void)
{
	g_autoptr(AxisMediaInfo) interlaced = map_fixture(RECORDED, "h264-interlaced", TRUE);
	g_autoptr(AxisMediaInfo) vfr = map_fixture(RECORDED, "h264-vfr", TRUE);
	g_autoptr(AxisMediaInfo) plain = map_fixture(RECORDED, "h264-sdr", TRUE);

	g_assert_cmpint(interlaced->interlaced, ==, AXIS_TRI_YES);
	g_assert_cmpstr(stream_at(interlaced, 0)->field_order, ==, "tb");
	g_assert_cmpint(plain->interlaced, ==, AXIS_TRI_NO);

	g_assert_cmpint(vfr->vfr_suspected, ==, AXIS_TRI_YES);
	g_assert_cmpint(plain->vfr_suspected, ==, AXIS_TRI_NO);
}

static void
test_subtitles_text_in_mp4(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "h264-mp4-subs", TRUE);
	const AxisMediaStream *sub = first_of(info, AXIS_STREAM_SUBTITLE);

	g_assert_cmpstr(info->container, ==, "mov,mp4,m4a,3gp,3g2,mj2");
	g_assert_cmpstr(sub->codec, ==, "mov_text");
	g_assert_cmpint(sub->subtitle_kind, ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpstr(sub->language, ==, "eng");
}

static void
test_multi_audio(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "multi-audio", TRUE);
	const AxisMediaStream *s;
	static const struct
	{
		const gchar *codec;
		gint         lossless;
	} expected[] = {
		{ "aac", AXIS_TRI_NO }, { "ac3", AXIS_TRI_NO }, { "eac3", AXIS_TRI_NO },
		{ "flac", AXIS_TRI_YES }, { "truehd", AXIS_TRI_YES }, { "opus", AXIS_TRI_NO }
	};
	guint i;

	g_assert_cmpint(info->audio_streams, ==, 6);
	for (i = 0; i < G_N_ELEMENTS(expected); i++) {
		s = stream_at(info, i + 1);
		g_assert_cmpstr(s->codec, ==, expected[i].codec);
		g_assert_cmpint(s->lossless, ==, expected[i].lossless);
	}
	/* The one marked default is the main audio stream. */
	g_assert_cmpint(info->main_audio, ==, 2);
	g_assert_cmpstr(info->audio_codec, ==, "ac3");
	g_assert_cmpint(info->audio_lossless, ==, AXIS_TRI_NO);
}

/* ── Recorded fixtures: HDR ─────────────────────────────────────── */

/* Spec: HDR10 (PQ with mastering display metadata, 10 bits recorded). */
static void
test_hdr10(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hdr10", TRUE);
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(s->hdr_uncertain);
	g_assert_cmpint(s->bit_depth, ==, 10);
	g_assert_true(s->signals.has_mastering);
	g_assert_true(s->signals.has_content_light);
	g_assert_cmpstr(s->color_transfer, ==, "smpte2084");
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(info->hdr_uncertain);
	g_assert_cmpint(info->bit_depth, ==, 10);
	g_assert_cmpstr(info->video_codec, ==, "hevc");
}

/* The mastering display metadata of this file is only in the bitstream: if
 * the frame could not be read, the file is uncertain, not HDR10 and not
 * SDR. */
static void
test_hdr10_without_the_frame(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hdr10", FALSE);

	g_assert_true(info->hdr_uncertain);
	g_assert_true(stream_at(info, 0)->hdr_uncertain);
	g_assert_cmpint(info->hdr_format, !=, AXIS_HDR_SDR);
}

static void
test_hlg(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hlg", TRUE);

	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_HLG);
	g_assert_false(info->hdr_uncertain);
}

/* Spec: Conflicting signals (incomplete). */
static void
test_pq_without_metadata_is_uncertain(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-pq-bare", TRUE);
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_NONE);
	g_assert_true(s->hdr_uncertain);
	g_assert_nonnull(s->hdr_note);
	g_assert_true(info->hdr_uncertain);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_NONE);
}

/* A 10-bit stream with no colour signalling at all looks like a Dolby
 * Vision profile 5 stream whose record was not reported: not SDR. */
static void
test_tenbit_unsignalled_is_uncertain(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-10bit-unsignalled", TRUE);

	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_NONE);
	g_assert_true(info->hdr_uncertain);
}

static void
test_tenbit_bt709_is_sdr(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-10bit-bt709", TRUE);

	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_SDR);
	g_assert_false(info->hdr_uncertain);
	g_assert_cmpint(info->bit_depth, ==, 10);
}

/* ── Constructed fixtures (see their README: not real files) ────── */

static void
check_dolby_vision(
	const gchar *name,
	gint         profile,
	gint         compat_id,
	gint         hdr10_compatible
){
	g_autoptr(AxisMediaInfo) info = map_fixture(CONSTRUCTED, name, FALSE);
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_false(s->hdr_uncertain);
	g_assert_cmpint(s->dv_profile, ==, profile);
	g_assert_cmpint(s->dv_bl_compat_id, ==, compat_id);
	g_assert_cmpint(s->dv_hdr10_compatible, ==, hdr10_compatible);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_false(info->hdr_uncertain);
}

/* Spec: Dolby Vision (profile, and whether the base layer is HDR10
 * compatible).  Constructed fixtures. */
static void
test_dolby_vision_profiles(void)
{
	check_dolby_vision("constructed-dv-p5", 5, 0, AXIS_TRI_NO);
	check_dolby_vision("constructed-dv-p81", 8, 1, AXIS_TRI_YES);
	check_dolby_vision("constructed-dv-p82", 8, 2, AXIS_TRI_NO);
	check_dolby_vision("constructed-dv-p84", 8, 4, AXIS_TRI_NO);
	/* Profile 7's compatibility value is not one Axis interprets. */
	check_dolby_vision("constructed-dv-p7", 7, 6, AXIS_TRI_UNKNOWN);
}

/* The Dolby Vision profile 5 fixture has no colour signalling: it must
 * not come out as SDR, but as Dolby Vision. */
static void
test_dolby_vision_p5_is_not_sdr(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(CONSTRUCTED, "constructed-dv-p5", FALSE);

	g_assert_null(stream_at(info, 0)->color_transfer);
	g_assert_cmpint(info->hdr_format, !=, AXIS_HDR_SDR);
	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
}

/* Anything that looks like Dolby Vision and is not exactly the expected
 * shape is uncertain. */
static void
test_dolby_vision_odd_shapes(void)
{
	g_autoptr(AxisMediaInfo) bad = map_fixture(CONSTRUCTED, "constructed-dv-bad-record", FALSE);
	g_autoptr(AxisMediaInfo) rpu = map_fixture(CONSTRUCTED, "constructed-dv-rpu-only", TRUE);
	g_autoptr(AxisMediaInfo) both = map_fixture(CONSTRUCTED, "constructed-dv-and-hdr10plus", TRUE);

	g_assert_cmpint(bad->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_true(bad->hdr_uncertain);
	g_assert_nonnull(bad->hdr_note);

	g_assert_cmpint(rpu->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_true(rpu->hdr_uncertain);

	g_assert_cmpint(both->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_true(both->hdr_uncertain);
}

static void
test_hdr10plus(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(CONSTRUCTED, "constructed-hdr10plus", TRUE);
	g_autoptr(AxisMediaInfo) without = map_fixture(CONSTRUCTED, "constructed-hdr10plus", FALSE);
	g_autoptr(AxisMediaInfo) other = map_fixture(CONSTRUCTED, "constructed-dynamic-other", TRUE);

	g_assert_cmpint(info->hdr_format, ==, AXIS_HDR_HDR10PLUS);
	g_assert_false(info->hdr_uncertain);

	/* Without the frame the dynamic metadata cannot be seen: the file is
	 * PQ with no static metadata at all, and so uncertain. */
	g_assert_true(without->hdr_uncertain);
	g_assert_cmpint(without->hdr_format, !=, AXIS_HDR_SDR);

	/* Dynamic metadata of a kind not known: not classified, uncertain. */
	g_assert_cmpint(other->hdr_format, ==, AXIS_HDR_NONE);
	g_assert_true(other->hdr_uncertain);
}

/* Static metadata in the container shows as stream side data.  (Version 2
 * of the probe wants every value: the entry here is complete.) */
static void
test_hdr10_in_the_container(void)
{
	g_autoptr(AxisMediaInfo) with_frame = map_text(
		"{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"hevc\",\"width\":3840,"
		"\"height\":2160,\"pix_fmt\":\"yuv420p10le\",\"color_space\":\"bt2020nc\","
		"\"color_transfer\":\"smpte2084\",\"color_primaries\":\"bt2020\",\"bits_per_raw_sample\":\"10\","
		"\"side_data_list\":[{\"side_data_type\":\"Mastering display metadata\","
		"\"red_x\":\"34000/50000\",\"red_y\":\"16000/50000\",\"green_x\":\"13250/50000\",\"green_y\":\"34500/50000\","
		"\"blue_x\":\"7500/50000\",\"blue_y\":\"3000/50000\",\"white_point_x\":\"15635/50000\","
		"\"white_point_y\":\"16450/50000\",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"}]}],\"format\":{\"format_name\":\"matroska,webm\"}}",
		"{\"frames\":[{\"pix_fmt\":\"yuv420p10le\"}]}");

	g_assert_cmpint(with_frame->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(with_frame->hdr_uncertain);
}

/* ── HDR10 static metadata values (probe version 2) ─────────────── */

static void
assert_hdr10_values(
	const AxisMediaStream *s,
	gint64                 min_luminance,
	gint64                 max_luminance,
	gboolean               light,
	gint                   max_content,
	gint                   max_average
){
	g_assert_true(s->hdr10.mastering);
	g_assert_cmpint(s->hdr10.red_x, ==, 34000);
	g_assert_cmpint(s->hdr10.red_y, ==, 16000);
	g_assert_cmpint(s->hdr10.green_x, ==, 13250);
	g_assert_cmpint(s->hdr10.green_y, ==, 34500);
	g_assert_cmpint(s->hdr10.blue_x, ==, 7500);
	g_assert_cmpint(s->hdr10.blue_y, ==, 3000);
	g_assert_cmpint(s->hdr10.white_x, ==, 15635);
	g_assert_cmpint(s->hdr10.white_y, ==, 16450);
	g_assert_cmpint(s->hdr10.min_luminance, ==, min_luminance);
	g_assert_cmpint(s->hdr10.max_luminance, ==, max_luminance);
	g_assert_cmpint(s->hdr10.light, ==, light);
	g_assert_cmpint(s->hdr10.max_content, ==, max_content);
	g_assert_cmpint(s->hdr10.max_average, ==, max_average);
}

/* Spec: Values stored.  From the bitstream (the frame). */
static void
test_hdr10_values_from_the_bitstream(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hdr10", TRUE);
	const AxisMediaStream *s = stream_at(info, 0);

	assert_hdr10_values(s, 1, 10000000, TRUE, 1000, 400);
	g_assert_true(s->signals.mastering_in_frames);
	g_assert_true(s->signals.light_in_frames);
	g_assert_false(s->signals.hdr10_bad);
	g_assert_false(s->signals.hdr10_conflict);
}

/* The container stores floats, which ffprobe prints as reduced rationals
 * ("17/25", "3127/10000", "1000/1"): rounded to the bitstream's units they
 * are the same values, so the two agree (recorded from a file ffmpeg 8.1
 * wrote both into). */
static void
test_hdr10_values_container_and_frame_agree(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hdr10-reencoded", TRUE);
	g_autoptr(AxisMediaInfo) container_only = map_fixture(RECORDED, "hevc-hdr10-reencoded", FALSE);
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(s->hdr_uncertain);
	assert_hdr10_values(s, 1, 10000000, TRUE, 1000, 400);
	g_assert_false(s->signals.hdr10_conflict);
	/* Without the frame the values are the container's alone (still
	 * uncertain: the frame was wanted and not read). */
	assert_hdr10_values(stream_at(container_only, 0), 1, 10000000, TRUE, 1000, 400);
	g_assert_false(stream_at(container_only, 0)->signals.mastering_in_frames);
	g_assert_true(container_only->hdr_uncertain);
}

/* No content light levels: HDR10 all the same, no levels stored. */
static void
test_hdr10_values_without_light_levels(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "hevc-hdr10-nocll", TRUE);
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_HDR10);
	g_assert_false(s->hdr_uncertain);
	assert_hdr10_values(s, 50, 40000000, FALSE, 0, 0);
	g_assert_false(s->signals.has_content_light);
}

/* A PQ stream with a mastering display entry made of @entry, in the
 * container; and a frame that says nothing more. */
static AxisMediaInfo *
pq_with(const gchar *entries)
{
	g_autofree gchar *json = g_strdup_printf(
		"{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"hevc\",\"width\":3840,"
		"\"height\":2160,\"pix_fmt\":\"yuv420p10le\",\"color_space\":\"bt2020nc\","
		"\"color_transfer\":\"smpte2084\",\"color_primaries\":\"bt2020\",\"bits_per_raw_sample\":\"10\","
		"\"side_data_list\":[%s]}],\"format\":{\"format_name\":\"matroska,webm\"}}", entries);

	return map_text(json, "{\"frames\":[{\"pix_fmt\":\"yuv420p10le\"}]}");
}

#define PRIMARIES "\"red_x\":\"34000/50000\",\"red_y\":\"16000/50000\",\"green_x\":\"13250/50000\"," \
	"\"green_y\":\"34500/50000\",\"blue_x\":\"7500/50000\",\"blue_y\":\"3000/50000\"," \
	"\"white_point_x\":\"15635/50000\",\"white_point_y\":\"16450/50000\""
#define MASTERING(rest) "{\"side_data_type\":\"Mastering display metadata\"," rest "}"
#define LIGHT(rest) "{\"side_data_type\":\"Content light level metadata\"," rest "}"

static void
assert_uncertain_without_values(
	const gchar *entries,
	const gchar *what
){
	g_autoptr(AxisMediaInfo) info = pq_with(entries);
	const AxisMediaStream *s = stream_at(info, 0);

	if (!s->hdr_uncertain)
		g_error("%s: not uncertain", what);
	g_assert_true(info->hdr_uncertain);
	g_assert_nonnull(s->hdr_note);
	g_assert_false(s->hdr10.mastering);
	g_assert_false(s->hdr10.light);
}

/* Spec: Out of range (and missing, and contradictory): uncertain, and
 * nothing stored. */
static void
test_hdr10_values_out_of_range(void)
{
	/* The same entry, complete and in range: HDR10, certain. */
	{
		g_autoptr(AxisMediaInfo) good = pq_with(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
			"\"max_luminance\":\"10000000/10000\"") "," LIGHT("\"max_content\":1000,\"max_average\":400"));

		g_assert_cmpint(good->hdr_format, ==, AXIS_HDR_HDR10);
		g_assert_false(good->hdr_uncertain);
		assert_hdr10_values(stream_at(good, 0), 50, 10000000, TRUE, 1000, 400);
	}
	/* Missing values. */
	assert_uncertain_without_values(MASTERING("\"max_luminance\":\"10000000/10000\""), "luminance only");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"max_luminance\":\"10000000/10000\""),
		"no minimum luminance");
	assert_uncertain_without_values(MASTERING("\"red_x\":\"34000/50000\",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\""), "one primary only");
	/* Chromaticity above 1. */
	assert_uncertain_without_values(MASTERING("\"red_x\":\"50001/50000\",\"red_y\":\"16000/50000\","
		"\"green_x\":\"13250/50000\",\"green_y\":\"34500/50000\",\"blue_x\":\"7500/50000\","
		"\"blue_y\":\"3000/50000\",\"white_point_x\":\"15635/50000\",\"white_point_y\":\"16450/50000\","
		"\"min_luminance\":\"50/10000\",\"max_luminance\":\"10000000/10000\""), "chromaticity above 1");
	/* Negative, malformed. */
	assert_uncertain_without_values(MASTERING("\"red_x\":\"-1/50000\",\"red_y\":\"16000/50000\","
		"\"green_x\":\"13250/50000\",\"green_y\":\"34500/50000\",\"blue_x\":\"7500/50000\","
		"\"blue_y\":\"3000/50000\",\"white_point_x\":\"15635/50000\",\"white_point_y\":\"16450/50000\","
		"\"min_luminance\":\"50/10000\",\"max_luminance\":\"10000000/10000\""), "negative coordinate");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/0\","
		"\"max_luminance\":\"10000000/10000\""), "zero denominator");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"N/A\","
		"\"max_luminance\":\"10000000/10000\""), "not a number");
	/* Maximum luminance under 5 cd/m2 or above 10000; minimum above 5 or
	 * not below the maximum. */
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"1/10000\","
		"\"max_luminance\":\"4/1\""), "maximum luminance too low");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"1/10000\","
		"\"max_luminance\":\"10001/1\""), "maximum luminance too high");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"6/1\","
		"\"max_luminance\":\"1000/1\""), "minimum luminance too high");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"5/1\","
		"\"max_luminance\":\"5/1\""), "minimum equal to the maximum");
	/* Light levels: beyond 16 bits, negative, missing, the average above
	 * the maximum. */
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"") "," LIGHT("\"max_content\":65536,\"max_average\":400"),
		"light level above 65535");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"") "," LIGHT("\"max_content\":-1,\"max_average\":400"),
		"negative light level");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"") "," LIGHT("\"max_content\":1000"), "no average light level");
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"") "," LIGHT("\"max_content\":400,\"max_average\":1000"),
		"average above the maximum");
	/* Two entries that disagree. */
	assert_uncertain_without_values(MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\"") "," MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"40000000/10000\""), "two mastering displays");
	/* Zero light levels are "not known", and valid. */
	{
		g_autoptr(AxisMediaInfo) zero = pq_with(MASTERING(PRIMARIES ",\"min_luminance\":\"0/1\","
			"\"max_luminance\":\"1000/1\"") "," LIGHT("\"max_content\":0,\"max_average\":0"));

		g_assert_false(zero->hdr_uncertain);
		assert_hdr10_values(stream_at(zero, 0), 0, 10000000, TRUE, 0, 0);
	}
}

/* The container says one thing, the bitstream another: uncertain. */
static void
test_hdr10_values_container_and_frame_disagree(void)
{
	g_autoptr(AxisMediaInfo) info = map_text(
		"{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"hevc\",\"width\":3840,"
		"\"height\":2160,\"pix_fmt\":\"yuv420p10le\",\"color_space\":\"bt2020nc\","
		"\"color_transfer\":\"smpte2084\",\"color_primaries\":\"bt2020\",\"bits_per_raw_sample\":\"10\","
		"\"side_data_list\":[" LIGHT("\"max_content\":1000,\"max_average\":400") "]}],"
		"\"format\":{\"format_name\":\"matroska,webm\"}}",
		"{\"frames\":[{\"pix_fmt\":\"yuv420p10le\",\"side_data_list\":["
		MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\",\"max_luminance\":\"10000000/10000\"") ","
		LIGHT("\"max_content\":2000,\"max_average\":400") "]}]}");
	const AxisMediaStream *s = stream_at(info, 0);

	g_assert_true(s->signals.hdr10_conflict);
	g_assert_true(s->hdr_uncertain);
	g_assert_false(s->hdr10.light);
	g_assert_false(s->hdr10.mastering);
}

/* HLG with static metadata: carried when valid, uncertain when not. */
static void
test_hlg_with_static_metadata(void)
{
	static const gchar head[] =
		"{\"streams\":[{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"hevc\",\"width\":3840,"
		"\"height\":2160,\"pix_fmt\":\"yuv420p10le\",\"color_space\":\"bt2020nc\","
		"\"color_transfer\":\"arib-std-b67\",\"color_primaries\":\"bt2020\",\"bits_per_raw_sample\":\"10\","
		"\"side_data_list\":[";
	static const gchar tail[] = "]}],\"format\":{\"format_name\":\"matroska,webm\"}}";
	g_autofree gchar *good_json = g_strconcat(head, MASTERING(PRIMARIES ",\"min_luminance\":\"50/10000\","
		"\"max_luminance\":\"10000000/10000\""), tail, NULL);
	g_autofree gchar *bad_json = g_strconcat(head, MASTERING("\"max_luminance\":\"10000000/10000\""), tail, NULL);
	g_autoptr(AxisMediaInfo) good = map_text(good_json, "{\"frames\":[{\"pix_fmt\":\"yuv420p10le\"}]}");
	g_autoptr(AxisMediaInfo) bad = map_text(bad_json, "{\"frames\":[{\"pix_fmt\":\"yuv420p10le\"}]}");

	g_assert_cmpint(good->hdr_format, ==, AXIS_HDR_HLG);
	g_assert_false(good->hdr_uncertain);
	assert_hdr10_values(stream_at(good, 0), 50, 10000000, FALSE, 0, 0);
	g_assert_cmpint(bad->hdr_format, ==, AXIS_HDR_HLG);
	g_assert_true(bad->hdr_uncertain);
}

/* The equality and range helpers. */
static void
test_hdr10_metadata_helpers(void)
{
	AxisHdr10Metadata a;
	AxisHdr10Metadata b;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	g_assert_true(axis_hdr10_metadata_equal(&a, &b));
	g_assert_true(axis_hdr10_metadata_valid(&a));
	a.mastering = TRUE;
	a.max_luminance = 10000000;
	a.min_luminance = 50;
	g_assert_true(axis_hdr10_metadata_valid(&a));
	g_assert_false(axis_hdr10_metadata_equal(&a, &b));
	b = a;
	g_assert_true(axis_hdr10_metadata_equal(&a, &b));
	b.white_y = 1;
	g_assert_false(axis_hdr10_metadata_equal(&a, &b));
	/* Values not flagged do not count. */
	b = a;
	b.max_content = 5;
	g_assert_true(axis_hdr10_metadata_equal(&a, &b));
	a.red_x = 50001;
	g_assert_false(axis_hdr10_metadata_valid(&a));
}

static void
test_remux(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(CONSTRUCTED, "constructed-remux", FALSE);

	g_assert_cmpint(info->video_streams, ==, 1);
	g_assert_cmpint(info->audio_streams, ==, 3);
	g_assert_cmpint(info->subtitle_streams, ==, 3);
	g_assert_cmpint(info->chapter_count, ==, 2);
	/* TrueHD, DTS-HD MA: lossless; plain DTS: lossy. */
	g_assert_cmpint(stream_at(info, 1)->lossless, ==, AXIS_TRI_YES);
	g_assert_cmpint(stream_at(info, 2)->lossless, ==, AXIS_TRI_YES);
	g_assert_cmpint(stream_at(info, 3)->lossless, ==, AXIS_TRI_NO);
	g_assert_cmpint(stream_at(info, 1)->bit_depth_audio, ==, 24);
	/* PGS subtitles are pictures, SubRip is text. */
	g_assert_cmpint(stream_at(info, 4)->subtitle_kind, ==, AXIS_SUBTITLE_IMAGE);
	g_assert_cmpint(stream_at(info, 5)->subtitle_kind, ==, AXIS_SUBTITLE_IMAGE);
	g_assert_true(stream_at(info, 5)->is_forced);
	g_assert_cmpint(stream_at(info, 6)->subtitle_kind, ==, AXIS_SUBTITLE_TEXT);
	/* The bitrates the muxer tagged are taken as stated. */
	g_assert_cmpint(stream_at(info, 0)->bitrate, ==, 58000000);
	g_assert_false(stream_at(info, 0)->bitrate_estimated);
	/* No frame was given, so even this one is not certain. */
	g_assert_true(info->hdr_uncertain);
}

/* ── The classifier, rule by rule ───────────────────────────────── */

typedef struct
{
	gint         bits;
	const gchar *transfer;
	const gchar *primaries;
	gboolean     mastering;
	gboolean     content_light;
	gboolean     hdr10plus;
	gboolean     dynamic_other;
	gboolean     frames_read;
	AxisHdrFormat format;
	gboolean     uncertain;
} Case;

static void
check_case(
	guint       line,
	const Case *c
){
	AxisMediaStream *s = video_stream(c->bits, c->transfer, c->primaries);

	s->signals.has_mastering = c->mastering;
	s->signals.has_content_light = c->content_light;
	s->signals.hdr10plus = c->hdr10plus;
	s->signals.dynamic_other = c->dynamic_other;
	axis_probe_hdr_classify(s, c->frames_read);
	if (s->hdr_format != c->format || s->hdr_uncertain != c->uncertain)
		g_error("case on line %u: got %s%s, expected %s%s", line, axis_hdr_format_label(s->hdr_format),
			s->hdr_uncertain ? " (uncertain)" : "", axis_hdr_format_label(c->format),
			c->uncertain ? " (uncertain)" : "");
	/* Whatever the verdict, it is never "no class and no doubt". */
	g_assert_true(s->hdr_format != AXIS_HDR_NONE || s->hdr_uncertain);
	axis_media_stream_free(s);
}

#define CASE(...) check_case(__LINE__, &(Case)__VA_ARGS__)

static void
test_classification_table(void)
{
	/*     bits transfer        primaries  mast   cll    h10p   dyn    frames  format              uncertain */
	CASE({ 8, "bt709",        "bt709",    FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         FALSE });
	CASE({ 8, NULL,           NULL,       FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         FALSE });
	CASE({ 8, "unknown",      "unknown",  FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         FALSE });
	CASE({ 8, "smpte170m",    "smpte170m",FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         FALSE });
	CASE({ 10, "bt709",       "bt709",    FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         FALSE });
	/* SDR transfer with HDR metadata, or wide gamut: doubt. */
	CASE({ 10, "bt709",       "bt709",    TRUE,  FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         TRUE });
	CASE({ 10, "bt709",       "bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_SDR,         TRUE });
	/* No transfer signalled. */
	CASE({ 10, NULL,          NULL,       FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 10, "unspecified", "bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 8, NULL,           "bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 8, NULL,           NULL,       TRUE,  FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ AXIS_PROBE_UNKNOWN, NULL, NULL, FALSE, FALSE, FALSE, FALSE, TRUE, AXIS_HDR_NONE,        TRUE });
	/* PQ. */
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  TRUE,  FALSE, FALSE, TRUE,  AXIS_HDR_HDR10,       FALSE });
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HDR10,       FALSE });
	CASE({ 10, "smpte2084",   "bt2020",   FALSE, TRUE,  FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 10, "smpte2084",   "bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 10, "smpte2084",   "smpte432", TRUE,  FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HDR10,       TRUE });
	CASE({ 8, "smpte2084",    "bt2020",   TRUE,  FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HDR10,       TRUE });
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  FALSE, FALSE, FALSE, FALSE, AXIS_HDR_HDR10,       TRUE });
	/* HLG. */
	CASE({ 10, "arib-std-b67","bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HLG,         FALSE });
	CASE({ 10, "arib-std-b67","bt709",    FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HLG,         TRUE });
	CASE({ 8, "arib-std-b67", "bt2020",   FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_HLG,         TRUE });
	/* HDR10+. */
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  FALSE, TRUE,  FALSE, TRUE,  AXIS_HDR_HDR10PLUS,   FALSE });
	CASE({ 10, "smpte2084",   "bt2020",   FALSE, FALSE, TRUE,  FALSE, TRUE,  AXIS_HDR_HDR10PLUS,   TRUE });
	CASE({ 10, "bt709",       "bt709",    TRUE,  FALSE, TRUE,  FALSE, TRUE,  AXIS_HDR_HDR10PLUS,   TRUE });
	/* Dynamic metadata of an unknown kind. */
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  FALSE, FALSE, TRUE,  TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 10, "smpte2084",   "bt2020",   TRUE,  FALSE, TRUE,  TRUE,  TRUE,  AXIS_HDR_HDR10PLUS,   TRUE });
	/* Unusual transfer functions. */
	CASE({ 10, "linear",      "bt709",    FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	CASE({ 10, "smpte428",    "bt709",    FALSE, FALSE, FALSE, FALSE, TRUE,  AXIS_HDR_NONE,        TRUE });
	/* A frame wanted and not read. */
	CASE({ 10, "bt709",       "bt709",    FALSE, FALSE, FALSE, FALSE, FALSE, AXIS_HDR_SDR,         TRUE });
	CASE({ 8, "bt709",        "bt709",    FALSE, FALSE, FALSE, FALSE, FALSE, AXIS_HDR_SDR,         FALSE });
}

static void
test_classification_dolby_vision(void)
{
	AxisMediaStream *s = video_stream(10, "bt709", "bt709");

	/* A record, however the colour fields look. */
	s->signals.dovi_record = TRUE;
	s->signals.dv_profile = 8;
	s->signals.dv_bl_compat_id = 2;
	axis_probe_hdr_classify(s, FALSE);
	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_false(s->hdr_uncertain);
	g_assert_cmpint(s->dv_profile, ==, 8);
	g_assert_cmpint(s->dv_hdr10_compatible, ==, AXIS_TRI_NO);

	/* A malformed one. */
	s->signals.dovi_shape_bad = TRUE;
	axis_probe_hdr_classify(s, TRUE);
	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_true(s->hdr_uncertain);

	/* Frame metadata alone. */
	s->signals.dovi_record = FALSE;
	s->signals.dovi_shape_bad = FALSE;
	s->signals.dovi_metadata = TRUE;
	axis_probe_hdr_classify(s, TRUE);
	g_assert_cmpint(s->hdr_format, ==, AXIS_HDR_DOLBY_VISION);
	g_assert_true(s->hdr_uncertain);
	g_assert_cmpint(s->dv_profile, ==, AXIS_PROBE_UNKNOWN);

	axis_media_stream_free(s);
}

/* A cover picture is not a video: it needs no class. */
static void
test_cover_picture_not_classified(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "album-track", FALSE);
	const AxisMediaStream *cover = stream_at(info, 1);

	g_assert_cmpint(cover->kind, ==, AXIS_STREAM_VIDEO);
	g_assert_true(cover->attached_pic);
	g_assert_cmpint(cover->hdr_format, ==, AXIS_HDR_NONE);
	g_assert_false(cover->hdr_uncertain);
	g_assert_cmpint(info->video_streams, ==, 0);
	g_assert_false(info->hdr_uncertain);
	g_assert_cmpint(info->main_video, ==, -1);
}

/* ── Recorded fixtures: audio ───────────────────────────────────── */

/* Spec: Audio file. */
static void
test_audio_flac(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "album-track", FALSE);
	const AxisMediaStream *audio = stream_at(info, 0);

	g_assert_cmpstr(info->container, ==, "flac");
	g_assert_cmpstr(info->audio_codec, ==, "flac");
	g_assert_cmpint(info->audio_sample_rate, ==, 44100);
	g_assert_cmpint(info->audio_bit_depth, ==, 16);
	g_assert_cmpint(info->audio_channels, ==, 1);
	g_assert_cmpint(info->audio_lossless, ==, AXIS_TRI_YES);
	g_assert_cmpint(audio->lossless, ==, AXIS_TRI_YES);

	/* Tags, ReplayGain, MusicBrainz and the cover. */
	g_assert_cmpstr(info->tag_title, ==, "Track One");
	g_assert_cmpstr(info->tag_artist, ==, "The Band");
	g_assert_cmpstr(info->tag_album, ==, "The Album");
	g_assert_true(info->has_track);
	g_assert_true(info->has_disc);
	g_assert_true(info->has_musicbrainz);
	g_assert_true(info->has_replaygain);
	g_assert_true(info->has_embedded_art);
	g_assert_cmpint(info->art_width, ==, 64);
	g_assert_cmpint(info->art_height, ==, 64);

	/* FLAC states no bitrate: it is derived. */
	g_assert_cmpint(audio->bitrate, >, 0);
	g_assert_true(audio->bitrate_estimated);
}

static void
test_audio_mp3(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "album-track-mp3", FALSE);
	const AxisMediaStream *audio = first_of(info, AXIS_STREAM_AUDIO);

	g_assert_cmpstr(info->container, ==, "mp3");
	g_assert_cmpint(info->audio_lossless, ==, AXIS_TRI_NO);
	g_assert_cmpint(audio->bitrate, ==, 192000);
	g_assert_false(audio->bitrate_estimated);
	g_assert_cmpstr(info->tag_title, ==, "Track Two");
	g_assert_true(info->has_track);
	g_assert_true(info->has_embedded_art);
	g_assert_false(info->has_replaygain);
	g_assert_false(info->has_musicbrainz);
}

static void
test_audio_opus(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "voice-opus", FALSE);

	g_assert_cmpstr(info->container, ==, "ogg");
	g_assert_cmpstr(info->audio_codec, ==, "opus");
	g_assert_cmpint(info->audio_lossless, ==, AXIS_TRI_NO);
	g_assert_cmpstr(info->tag_title, ==, "A Voice");
	g_assert_cmpstr(info->tag_artist, ==, "Someone");
	g_assert_false(info->has_embedded_art);
}

static void
test_audio_m4b_chapters(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "book-m4b", FALSE);

	g_assert_cmpint(info->chapter_count, ==, 3);
	g_assert_cmpstr(info->tag_album, ==, "The Book");
	g_assert_cmpstr(info->tag_artist, ==, "An Author");
	g_assert_cmpstr(info->audio_codec, ==, "aac");
}

/* A PCM stream's bitrate follows from its parameters. */
static void
test_audio_pcm(void)
{
	g_autoptr(AxisMediaInfo) info = map_fixture(RECORDED, "pcm24-wav", FALSE);
	const AxisMediaStream *audio = stream_at(info, 0);

	g_assert_cmpint(info->audio_lossless, ==, AXIS_TRI_YES);
	g_assert_cmpint(info->audio_bit_depth, ==, 24);
	g_assert_cmpint(info->audio_sample_rate, ==, 48000);
	g_assert_cmpint(audio->bitrate, ==, 48000 * 1 * 24);
}

/* ── Failures ───────────────────────────────────────────────────── */

/* Spec: Corrupt file. */
static void
test_corrupt_files(void)
{
	static const gchar *const names[] = { "corrupt-random", "corrupt-truncated-mp4", "corrupt-empty", NULL };
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *json = read_file(RECORDED, names[i], ".json", NULL);
		g_autofree gchar *error = map_error(json);

		g_assert_nonnull(strstr(error, "Invalid data found when processing input"));
	}
}

static void
test_unexpected_shapes(void)
{
	static const gchar *const bad[] = {
		"[]", "5", "\"x\"", "null", "{}", "{\"streams\":[]}", "{\"format\":{\"format_name\":\"x\"}}",
		"{\"streams\":{},\"format\":{\"format_name\":\"x\"}}",
		"{\"streams\":[1,2,3],\"format\":{\"format_name\":\"x\"}}",
		"{\"streams\":[{}],\"format\":5}", "{\"streams\":[{}],\"format\":{\"format_name\":null}}",
		"{\"error\":{\"code\":-1}}", "{\"error\":5}", NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *error = map_error(bad[i]);

		g_assert_true(g_utf8_validate(error, -1, NULL));
	}
}

/* ── Hostile and absurd input ───────────────────────────────────── */

/* Spec: the mapper never trusts a number. */
static void
test_absurd_numbers_are_unknown(void)
{
	g_autoptr(AxisMediaInfo) info = map_text(
		"{\"streams\":["
		"{\"index\":0,\"codec_type\":\"video\",\"codec_name\":\"h264\",\"width\":70000,\"height\":-5,"
		"\"bit_rate\":\"99999999999999999999\",\"avg_frame_rate\":\"1/0\",\"r_frame_rate\":\"1e400\","
		"\"bits_per_raw_sample\":\"9999\",\"pix_fmt\":\"nonsense\"},"
		"{\"index\":1,\"codec_type\":\"audio\",\"codec_name\":\"aac\",\"channels\":100000,"
		"\"sample_rate\":\"-1\",\"bit_rate\":-12,\"bits_per_sample\":999},"
		"{\"index\":\"x\",\"codec_type\":7,\"codec_name\":[1],\"profile\":{},\"tags\":5}"
		"],\"chapters\":\"many\",\"format\":{\"format_name\":\"mkv\",\"duration\":\"1e12\","
		"\"bit_rate\":\"-1\",\"tags\":[1]}}", NULL);
	const AxisMediaStream *video = stream_at(info, 0);
	const AxisMediaStream *audio = stream_at(info, 1);
	const AxisMediaStream *odd = stream_at(info, 2);

	g_assert_cmpint(info->duration_ms, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(info->bitrate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(info->chapter_count, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(video->width, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(video->height, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(video->bitrate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(video->bit_depth, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpfloat(video->frame_rate, <, 0);
	g_assert_cmpfloat(video->real_frame_rate, <, 0);
	g_assert_cmpint(audio->channels, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(audio->sample_rate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(audio->bitrate, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(audio->bit_depth_audio, ==, AXIS_PROBE_UNKNOWN);
	g_assert_cmpint(odd->kind, ==, AXIS_STREAM_UNKNOWN);
	g_assert_cmpint(odd->index, ==, 2);	/* its place when its index is unreadable */
	g_assert_null(odd->codec);
	g_assert_null(odd->profile);
	/* And a video stream that is not classified says so. */
	g_assert_true(video->hdr_format != AXIS_HDR_NONE || video->hdr_uncertain);
}

/* Numbers as strings are read, as ffprobe prints most of them so. */
static void
test_numbers_as_strings(void)
{
	g_autoptr(AxisMediaInfo) info = map_text(
		"{\"streams\":[{\"index\":\"0\",\"codec_type\":\"video\",\"codec_name\":\"h264\",\"width\":\"1920\","
		"\"height\":1080,\"pix_fmt\":\"yuv420p\",\"avg_frame_rate\":\"30000/1001\",\"bit_rate\":\"8000000\"}],"
		"\"format\":{\"format_name\":\"mov,mp4\",\"duration\":\"3600.5\",\"bit_rate\":8100000}}", NULL);
	const AxisMediaStream *video = stream_at(info, 0);

	g_assert_cmpint(video->width, ==, 1920);
	g_assert_cmpint(video->height, ==, 1080);
	g_assert_cmpint(video->bitrate, ==, 8000000);
	g_assert_false(video->bitrate_estimated);
	g_assert_cmpfloat_with_epsilon(video->frame_rate, 29.97, 0.001);
	g_assert_cmpint(info->duration_ms, ==, 3600500);
	g_assert_cmpint(info->bitrate, ==, 8100000);
	g_assert_cmpstr(info->resolution_class, ==, "1080p");
}

/* No more than the limit of streams is kept, the rest is counted. */
static void
test_many_streams_are_capped(void)
{
	g_autoptr(GString) json = g_string_new("{\"streams\":[");
	g_autoptr(AxisMediaInfo) info = NULL;
	guint i;

	for (i = 0; i < 700; i++)
		g_string_append_printf(json, "%s{\"index\":%u,\"codec_type\":\"subtitle\",\"codec_name\":\"subrip\"}",
			i > 0 ? "," : "", i);
	g_string_append(json, "],\"format\":{\"format_name\":\"matroska,webm\"}}");
	info = map_text(json->str, NULL);

	g_assert_cmpuint(info->streams->len, ==, AXIS_PROBE_MAX_STREAMS);
	g_assert_cmpint(info->stream_count, ==, 700);
	g_assert_cmpint(info->streams_omitted, ==, 700 - AXIS_PROBE_MAX_STREAMS);
	g_assert_cmpint(info->subtitle_streams, ==, AXIS_PROBE_MAX_STREAMS);
}

/* Text from a file: cut, cleaned, and never a control or direction
 * character. */
static void
test_text_is_cleaned(void)
{
	g_autoptr(GString) json = g_string_new(NULL);
	g_autoptr(AxisMediaInfo) info = NULL;
	const gchar *title;
	const gchar *p;
	guint i;

	g_string_append(json, "{\"streams\":[{\"index\":0,\"codec_type\":\"audio\",\"codec_name\":\"mp3\","
		"\"tags\":{\"language\":\"en<script>\",\"title\":\"");
	for (i = 0; i < 5000; i++)
		g_string_append(json, "ab");
	g_string_append(json, "\"}}],\"format\":{\"format_name\":\"mp3\",\"tags\":{\"title\":"
		"\"<b>bold</b>\\r\\n\\u202eevil\\u0007\\u0085x\",\"ALBUM\":\"A\\nB\",\"Artist\":\"  spaced  \"}}}");
	info = map_text(json->str, NULL);

	g_assert_null(stream_at(info, 0)->language);
	g_assert_cmpuint(g_utf8_strlen(stream_at(info, 0)->title, -1), <=, AXIS_PROBE_TAG_CHARS);

	title = info->tag_title;
	g_assert_nonnull(title);
	g_assert_nonnull(strstr(title, "<b>bold</b>"));	/* markup is text: the page escapes it */
	for (p = title; *p != '\0'; p++) {
		g_assert_true((guchar)*p >= 0x20 && *p != 0x7f);
		g_assert_false((guchar)p[0] == 0xe2 && (guchar)p[1] == 0x80 && (guchar)p[2] == 0xae);
		g_assert_false((guchar)p[0] == 0xc2 && (guchar)p[1] == 0x85);
	}
	g_assert_cmpstr(info->tag_album, ==, "A B");
	g_assert_cmpstr(info->tag_artist, ==, "spaced");
}

/* Tags kept with an audio stream name the file only when the file is
 * audio: the title of one track of a film is not the film's. */
static void
test_stream_tags_name_only_audio_files(void)
{
	static const gchar *const audio_tags =
		"\"codec_type\":\"audio\",\"codec_name\":\"opus\",\"tags\":{\"title\":\"Track\",\"artist\":\"Band\","
		"\"REPLAYGAIN_TRACK_GAIN\":\"-3 dB\"}";
	g_autofree gchar *music = g_strdup_printf("{\"streams\":[{%s}],\"format\":{\"format_name\":\"ogg\"}}",
		audio_tags);
	g_autofree gchar *film = g_strdup_printf("{\"streams\":[{\"codec_type\":\"video\",\"codec_name\":\"h264\","
		"\"width\":640,\"height\":360,\"pix_fmt\":\"yuv420p\"},{%s}],\"format\":{\"format_name\":\"matroska,webm\","
		"\"tags\":{\"title\":\"The Film\"}}}", audio_tags);
	g_autoptr(AxisMediaInfo) audio_file = map_text(music, NULL);
	g_autoptr(AxisMediaInfo) video_file = map_text(film, NULL);

	g_assert_cmpstr(audio_file->tag_title, ==, "Track");
	g_assert_cmpstr(audio_file->tag_artist, ==, "Band");
	g_assert_true(audio_file->has_replaygain);
	g_assert_cmpstr(video_file->tag_title, ==, "The Film");
	g_assert_null(video_file->tag_artist);
	g_assert_false(video_file->has_replaygain);
}

/* The same file, probed twice, gives the same model. */
static void
test_mapping_is_deterministic(void)
{
	g_autofree gchar *json = read_file(RECORDED, "multi-audio", ".json", NULL);
	g_autoptr(AxisMediaInfo) a = map_text(json, NULL);
	g_autoptr(AxisMediaInfo) b = map_text(json, NULL);
	guint i;

	g_assert_cmpuint(a->streams->len, ==, b->streams->len);
	for (i = 0; i < a->streams->len; i++) {
		const AxisMediaStream *x = stream_at(a, i);
		const AxisMediaStream *y = stream_at(b, i);

		g_assert_cmpint(x->bitrate, ==, y->bitrate);
		g_assert_cmpstr(x->codec, ==, y->codec);
	}
}

/* ── Derived facts ──────────────────────────────────────────────── */

static void
test_resolution_classes(void)
{
	static const struct
	{
		gint         width;
		gint         height;
		const gchar *expected;
	} cases[] = {
		{ 720, 480, "sd" }, { 720, 576, "sd" }, { 1024, 576, "sd" }, { 640, 360, "sd" },
		{ 1280, 720, "720p" }, { 1280, 544, "720p" }, { 1366, 768, "720p" },
		{ 1920, 1080, "1080p" }, { 1920, 800, "1080p" }, { 1440, 1080, "1080p" },
		{ 3840, 2160, "2160p" }, { 3840, 1600, "2160p" }, { 4096, 2160, "2160p" },
		{ 2560, 1440, "other" }, { 7680, 4320, "other" }, { 100, 100, "other" },
		{ 1080, 1920, "1080p" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
		g_assert_cmpstr(axis_probe_resolution_class(cases[i].width, cases[i].height), ==, cases[i].expected);
	g_assert_null(axis_probe_resolution_class(0, 100));
	g_assert_null(axis_probe_resolution_class(-1, -1));
}

static void
test_pixel_format_depths(void)
{
	static const struct
	{
		const gchar *format;
		gint         depth;
	} cases[] = {
		{ "yuv420p", 8 }, { "yuvj420p", 8 }, { "yuv420p10le", 10 }, { "yuv420p10be", 10 },
		{ "yuv444p12le", 12 }, { "yuv422p16le", 16 }, { "yuv420p9le", 9 }, { "gray", 8 },
		{ "gray10le", 10 }, { "gbrp", 8 }, { "gbrap12le", 12 }, { "nv12", 8 }, { "p010le", 10 },
		{ "p016le", 16 }, { "ya8", 8 }, { "rgb24", AXIS_PROBE_UNKNOWN },
		{ "rgb48le", AXIS_PROBE_UNKNOWN }, { "", AXIS_PROBE_UNKNOWN }, { "yuv420p99", AXIS_PROBE_UNKNOWN },
		{ "gbrpf32le", AXIS_PROBE_UNKNOWN }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
		g_assert_cmpint(axis_probe_pixel_format_depth(cases[i].format), ==, cases[i].depth);
	g_assert_cmpint(axis_probe_pixel_format_depth(NULL), ==, AXIS_PROBE_UNKNOWN);
}

static void
test_lossless_table(void)
{
	g_assert_cmpint(axis_probe_audio_lossless("flac", NULL), ==, AXIS_TRI_YES);
	g_assert_cmpint(axis_probe_audio_lossless("alac", NULL), ==, AXIS_TRI_YES);
	g_assert_cmpint(axis_probe_audio_lossless("pcm_s24le", NULL), ==, AXIS_TRI_YES);
	g_assert_cmpint(axis_probe_audio_lossless("pcm_alaw", NULL), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("truehd", "Dolby TrueHD + Dolby Atmos"), ==, AXIS_TRI_YES);
	g_assert_cmpint(axis_probe_audio_lossless("dts", "DTS-HD MA"), ==, AXIS_TRI_YES);
	g_assert_cmpint(axis_probe_audio_lossless("dts", "DTS"), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("dts", "DTS-HD HRA"), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("dts", NULL), ==, AXIS_TRI_UNKNOWN);
	g_assert_cmpint(axis_probe_audio_lossless("mp3", NULL), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("aac", "LC"), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("opus", NULL), ==, AXIS_TRI_NO);
	g_assert_cmpint(axis_probe_audio_lossless("adpcm_ms", NULL), ==, AXIS_TRI_NO);
	/* Not in the table, or either kind: not guessed. */
	g_assert_cmpint(axis_probe_audio_lossless("wavpack", NULL), ==, AXIS_TRI_UNKNOWN);
	g_assert_cmpint(axis_probe_audio_lossless("somethingnew", NULL), ==, AXIS_TRI_UNKNOWN);
	g_assert_cmpint(axis_probe_audio_lossless(NULL, NULL), ==, AXIS_TRI_UNKNOWN);
}

static void
test_subtitle_kinds(void)
{
	g_assert_cmpint(axis_probe_subtitle_kind("subrip"), ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpint(axis_probe_subtitle_kind("ass"), ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpint(axis_probe_subtitle_kind("mov_text"), ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpint(axis_probe_subtitle_kind("webvtt"), ==, AXIS_SUBTITLE_TEXT);
	g_assert_cmpint(axis_probe_subtitle_kind("hdmv_pgs_subtitle"), ==, AXIS_SUBTITLE_IMAGE);
	g_assert_cmpint(axis_probe_subtitle_kind("dvd_subtitle"), ==, AXIS_SUBTITLE_IMAGE);
	g_assert_cmpint(axis_probe_subtitle_kind("dvb_subtitle"), ==, AXIS_SUBTITLE_IMAGE);
	g_assert_cmpint(axis_probe_subtitle_kind("eia_608"), ==, AXIS_SUBTITLE_UNKNOWN);
	g_assert_cmpint(axis_probe_subtitle_kind(NULL), ==, AXIS_SUBTITLE_UNKNOWN);
}

static void
test_names(void)
{
	AxisHdrFormat format;
	AxisStreamKind kind;
	guint i;

	for (i = AXIS_HDR_SDR; i <= AXIS_HDR_DOLBY_VISION; i++) {
		g_assert_true(axis_hdr_format_from_string(axis_hdr_format_to_string((AxisHdrFormat)i), &format));
		g_assert_cmpint(format, ==, i);
	}
	g_assert_null(axis_hdr_format_to_string(AXIS_HDR_NONE));
	g_assert_false(axis_hdr_format_from_string("hdr", &format));
	g_assert_false(axis_hdr_format_from_string(NULL, &format));
	for (i = 0; i <= AXIS_STREAM_UNKNOWN; i++) {
		g_assert_true(axis_stream_kind_from_string(axis_stream_kind_to_string((AxisStreamKind)i), &kind));
		g_assert_cmpint(kind, ==, i);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/probe/json/values", test_json_values);
	g_test_add_func("/probe/json/refused", test_json_refused);
	g_test_add_func("/probe/json/limits", test_json_limits);
	g_test_add_func("/probe/json/text", test_json_text);

	g_test_add_func("/probe/map/video-file", test_video_file);
	g_test_add_func("/probe/map/missing-bitrate-estimated", test_missing_bitrate_estimated);
	g_test_add_func("/probe/map/container-bitrate-from-size", test_container_bitrate_from_size);
	g_test_add_func("/probe/map/interlaced-and-vfr", test_interlaced_and_vfr);
	g_test_add_func("/probe/map/subtitles-in-mp4", test_subtitles_text_in_mp4);
	g_test_add_func("/probe/map/multi-audio", test_multi_audio);

	g_test_add_func("/probe/hdr/hdr10", test_hdr10);
	g_test_add_func("/probe/hdr/hdr10-without-the-frame", test_hdr10_without_the_frame);
	g_test_add_func("/probe/hdr/hlg", test_hlg);
	g_test_add_func("/probe/hdr/pq-without-metadata", test_pq_without_metadata_is_uncertain);
	g_test_add_func("/probe/hdr/10bit-unsignalled", test_tenbit_unsignalled_is_uncertain);
	g_test_add_func("/probe/hdr/10bit-bt709", test_tenbit_bt709_is_sdr);
	g_test_add_func("/probe/hdr/constructed/dolby-vision-profiles", test_dolby_vision_profiles);
	g_test_add_func("/probe/hdr/constructed/dolby-vision-p5-not-sdr", test_dolby_vision_p5_is_not_sdr);
	g_test_add_func("/probe/hdr/constructed/dolby-vision-odd-shapes", test_dolby_vision_odd_shapes);
	g_test_add_func("/probe/hdr/constructed/hdr10plus", test_hdr10plus);
	g_test_add_func("/probe/hdr/constructed/hdr10-in-the-container", test_hdr10_in_the_container);
	g_test_add_func("/probe/hdr/constructed/remux", test_remux);
	g_test_add_func("/probe/hdr/values/bitstream", test_hdr10_values_from_the_bitstream);
	g_test_add_func("/probe/hdr/values/container-and-frame-agree", test_hdr10_values_container_and_frame_agree);
	g_test_add_func("/probe/hdr/values/without-light-levels", test_hdr10_values_without_light_levels);
	g_test_add_func("/probe/hdr/values/out-of-range", test_hdr10_values_out_of_range);
	g_test_add_func("/probe/hdr/values/container-and-frame-disagree", test_hdr10_values_container_and_frame_disagree);
	g_test_add_func("/probe/hdr/values/hlg", test_hlg_with_static_metadata);
	g_test_add_func("/probe/hdr/values/helpers", test_hdr10_metadata_helpers);
	g_test_add_func("/probe/hdr/table", test_classification_table);
	g_test_add_func("/probe/hdr/dolby-vision-rules", test_classification_dolby_vision);
	g_test_add_func("/probe/hdr/cover-picture", test_cover_picture_not_classified);

	g_test_add_func("/probe/audio/flac", test_audio_flac);
	g_test_add_func("/probe/audio/mp3", test_audio_mp3);
	g_test_add_func("/probe/audio/opus", test_audio_opus);
	g_test_add_func("/probe/audio/m4b-chapters", test_audio_m4b_chapters);
	g_test_add_func("/probe/audio/pcm", test_audio_pcm);

	g_test_add_func("/probe/failures/corrupt-files", test_corrupt_files);
	g_test_add_func("/probe/failures/unexpected-shapes", test_unexpected_shapes);

	g_test_add_func("/probe/hostile/absurd-numbers", test_absurd_numbers_are_unknown);
	g_test_add_func("/probe/hostile/numbers-as-strings", test_numbers_as_strings);
	g_test_add_func("/probe/hostile/many-streams", test_many_streams_are_capped);
	g_test_add_func("/probe/hostile/text-is-cleaned", test_text_is_cleaned);
	g_test_add_func("/probe/hostile/deterministic", test_mapping_is_deterministic);
	g_test_add_func("/probe/audio/stream-tags", test_stream_tags_name_only_audio_files);

	g_test_add_func("/probe/derived/resolution-classes", test_resolution_classes);
	g_test_add_func("/probe/derived/pixel-formats", test_pixel_format_depths);
	g_test_add_func("/probe/derived/lossless", test_lossless_table);
	g_test_add_func("/probe/derived/subtitle-kinds", test_subtitle_kinds);
	g_test_add_func("/probe/derived/names", test_names);

	return g_test_run();
}
