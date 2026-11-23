/*
 * test-profile.c - conversion profile definitions without a database: the
 * strict validator (every member, every bound, unknown and repeated
 * members), the canonical form and its hash, the lock rule for every
 * member, the quality rule, the built-ins, and the property that no valid
 * definition, however built, makes a file eligible that a fixed rule covers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>

#include "axis-test-estimate.h"
#include "fuzz/fuzz-profile-gen.h"
#include "transcode/axis-estimate.h"
#include "transcode/axis-profile.h"

#define GB G_GINT64_CONSTANT(1000000000)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisProfileDefinition
builtin(const gchar *slug)
{
	AxisProfileDefinition def;
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();

	g_assert_nonnull(axis_profile_builtin(slug));
	if (!axis_profile_definition_parse(axis_profile_builtin(slug)->definition, -1, &def, problems)) {
		const AxisProfileProblem *p = g_ptr_array_index(problems, 0);

		g_error("%s: %s: %s", slug, p->field, p->message);
	}
	return def;
}

/* Whether @def checks; when it does not, that a problem names @field. */
static void
expect_check(
	const AxisProfileDefinition *def,
	gboolean                     valid,
	const gchar                 *field
){
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	g_autofree gchar *text = axis_profile_definition_canonical(def);
	gboolean parsed;

	parsed = axis_profile_definition_parse(text, -1, NULL, problems);
	if (parsed != valid)
		g_error("%s: expected %s, got %s (%s)", text, valid ? "valid" : "refused", parsed ? "valid" : "refused",
			problems->len > 0 ? ((AxisProfileProblem *)g_ptr_array_index(problems, 0))->message : "");
	if (!valid && field != NULL && axis_profile_problems_find(problems, field) == NULL) {
		guint i;

		for (i = 0; i < problems->len; i++)
			g_printerr("  problem: %s: %s\n", ((AxisProfileProblem *)g_ptr_array_index(problems, i))->field,
				((AxisProfileProblem *)g_ptr_array_index(problems, i))->message);
		g_error("%s: refused, but not for %s", text, field);
	}
	/* The struct check agrees with the text's. */
	g_assert_cmpint(axis_profile_definition_check(def, NULL), ==, valid);
}

/* @text refused, with a problem about @field (NULL: any). */
static void
expect_refused(
	const gchar *text,
	const gchar *field
){
	g_autoptr(GPtrArray) problems = axis_profile_problems_new();
	AxisProfileDefinition def;
	guint i;

	if (axis_profile_definition_parse(text, -1, &def, problems))
		g_error("accepted: %s", text);
	g_assert_cmpuint(problems->len, >, 0);
	for (i = 0; i < problems->len; i++) {
		const AxisProfileProblem *p = g_ptr_array_index(problems, i);

		/* No message carries any of the input. */
		g_assert_null(strstr(p->message, "sneeze"));
		g_assert_cmpstr(p->message, !=, "");
	}
	if (field != NULL && axis_profile_problems_find(problems, field) == NULL) {
		for (i = 0; i < problems->len; i++)
			g_printerr("  problem: %s: %s\n", ((AxisProfileProblem *)g_ptr_array_index(problems, i))->field,
				((AxisProfileProblem *)g_ptr_array_index(problems, i))->message);
		g_error("refused, but not for %s: %s", field, text);
	}
	/* Nothing usable is left in the result. */
	g_assert_cmpuint(def.libraries, ==, 0);
}

static gchar *
replaced(
	const gchar *text,
	const gchar *from,
	const gchar *to
){
	GString *s = g_string_new(text);

	if (strstr(text, from) == NULL)
		g_error("'%s' is not in %s", from, text);
	g_string_replace(s, from, to, 1);
	return g_string_free(s, FALSE);
}

static gchar *
hash_of(const gchar *text)
{
	AxisProfileDefinition def;
	g_autofree gchar *canonical = NULL;

	if (!axis_profile_definition_parse(text, -1, &def, NULL))
		g_error("refused: %s", text);
	canonical = axis_profile_definition_canonical(&def);
	return axis_profile_hash(canonical);
}

/* ── The built-ins ──────────────────────────────────────────────── */

static void
test_builtins(void)
{
	const AxisProfileBuiltin *list = axis_profile_builtins();
	const AxisEstimateTarget *targets = axis_estimate_builtin_targets();
	guint i;

	for (i = 0; i < AXIS_PROFILE_N_BUILTINS; i++) {
		AxisProfileDefinition def = builtin(list[i].slug);
		g_autofree gchar *canonical = axis_profile_definition_canonical(&def);

		/* Written in canonical form (migration 0014 holds the same text). */
		g_assert_cmpstr(canonical, ==, list[i].definition);
		g_assert_true(axis_profile_slug_reserved(list[i].slug));
		g_assert_true(axis_profile_slug_valid(list[i].slug));
		g_assert_cmpstr(targets[i].id, ==, list[i].slug);
		g_assert_cmpstr(targets[i].name, ==, list[i].name);
	}
	g_assert_false(axis_profile_slug_reserved("custom_1"));
	g_assert_false(axis_profile_slug_valid("Space"));
	g_assert_false(axis_profile_slug_valid("a-b"));
	g_assert_false(axis_profile_slug_valid(""));

	/* The targets they make are the targets of before profiles (the
	 * figures of the fixture library depend on it), but for Maximum
	 * compatibility's own "already efficient" threshold. */
	{
		const AxisEstimateTarget *t = axis_estimate_builtin_target("space_saver");

		g_assert_cmpint(t->codec, ==, AXIS_TCODEC_HEVC);
		g_assert_cmpint(t->bit_depth, ==, 10);
		g_assert_cmpint(t->hdr_bit_depth, ==, 10);
		g_assert_cmpstr(t->profile, ==, "Main 10");
		g_assert_cmpint(t->quality, ==, 4);
		g_assert_cmpint(t->n_quality_rules, ==, 2);
		g_assert_cmpint(t->audio, ==, AXIS_AUDIO_COPY);
		g_assert_cmpint(t->subtitles, ==, AXIS_SUBTITLES_COPY);
		g_assert_cmpint(t->container, ==, AXIS_CONTAINER_MATROSKA);
		g_assert_cmpint(t->efficient_milli, ==, 150);
		g_assert_cmpfloat_with_epsilon(t->min_saving, 0.15, 1e-9);
		g_assert_null(t->keep_audio_languages);
		t = axis_estimate_builtin_target("balanced");
		g_assert_cmpint(t->bit_depth, ==, 8);
		g_assert_cmpint(t->hdr_bit_depth, ==, 10);
		g_assert_cmpint(t->audio, ==, AXIS_AUDIO_COPY_ADD_STEREO);
		g_assert_cmpint(t->audio_bitrate, ==, 160000);
		t = axis_estimate_builtin_target("max_compat");
		g_assert_cmpint(t->codec, ==, AXIS_TCODEC_H264);
		g_assert_cmpint(t->hdr_bit_depth, ==, 0);
		g_assert_cmpint(t->quality, ==, 5);
		g_assert_cmpint(axis_estimate_quality_value(t->codec, t->encoder, t->quality), ==, 20);
		g_assert_cmpint(t->n_quality_rules, ==, 0);
		g_assert_cmpint(t->container, ==, AXIS_CONTAINER_MP4);
		g_assert_cmpint(t->subtitles, ==, AXIS_SUBTITLES_PLAIN_TEXT);
		g_assert_cmpint(t->audio, ==, AXIS_AUDIO_STEREO_FIRST);
		g_assert_cmpint(t->efficient_milli, ==, 120);
		t = axis_estimate_builtin_target("keep_lossless");
		g_assert_cmpint(t->codec, ==, AXIS_TCODEC_FLAC);
		g_assert_cmpint(t->quality, ==, 4);
		g_assert_cmpint(t->container, ==, AXIS_CONTAINER_FLAC);
		g_assert_false(t->reduce_to_cd);
		g_assert_cmpfloat_with_epsilon(t->min_saving, 0.10, 1e-9);
		t = axis_estimate_builtin_target("music_space_saver");
		g_assert_cmpint(t->audio_bitrate, ==, 128000);
		g_assert_cmpint(t->efficient_milli, ==, 1250);
		g_assert_cmpint(t->root_types, ==, AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC));
		t = axis_estimate_builtin_target("music_compat");
		g_assert_cmpint(t->codec, ==, AXIS_TCODEC_MP3);
		g_assert_cmpint(t->audio_quality, ==, 0);
		g_assert_cmpint(t->audio_bitrate, ==, 245000);
		t = axis_estimate_builtin_target("audiobooks");
		g_assert_cmpint(t->audio_bitrate, ==, 48000);
		g_assert_cmpint(t->audio_bitrate_mono, ==, 32000);
		g_assert_true(t->allow_lossy_source);
		g_assert_cmpint(t->root_types, ==, AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS));
	}
}

/* ── The validator ──────────────────────────────────────────────── */

/* Spec: Out of bounds; Minimum saving floor.  Every member's bounds, on
 * either side, for each kind and codec. */
static void
test_bounds(void)
{
	AxisProfileDefinition base = builtin("space_saver");
	AxisProfileDefinition d;
	guint i;

	expect_check(&base, TRUE, NULL);

	/* Minimum saving: the floor and the top. */
	d = base; d.min_saving_percent = 4; expect_check(&d, FALSE, "min_saving_percent");
	d = base; d.min_saving_percent = 5; expect_check(&d, TRUE, NULL);
	d = base; d.min_saving_percent = 0; expect_check(&d, FALSE, "min_saving_percent");
	d = base; d.min_saving_percent = -20; expect_check(&d, FALSE, "min_saving_percent");
	d = base; d.min_saving_percent = 95; expect_check(&d, TRUE, NULL);
	d = base; d.min_saving_percent = 96; expect_check(&d, FALSE, "min_saving_percent");
	d = base; d.min_saving_bytes = -1; expect_check(&d, FALSE, "min_saving_bytes");
	d = base; d.min_saving_bytes = AXIS_PROFILE_MAX_SAVING_BYTES; expect_check(&d, TRUE, NULL);
	d = base; d.min_saving_bytes = AXIS_PROFILE_MAX_SAVING_BYTES + 1; expect_check(&d, FALSE, "min_saving_bytes");

	/* Libraries. */
	d = base; d.libraries = 0; expect_check(&d, FALSE, "libraries");
	d = base; d.libraries = AXIS_TARGET_ROOT(AXIS_MEDIA_TV); expect_check(&d, TRUE, NULL);
	d = base; d.libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC); expect_check(&d, FALSE, "libraries");
	d = base; d.libraries = AXIS_TARGET_ROOT(AXIS_MEDIA_DOWNLOADS); expect_check(&d, FALSE, "libraries");

	/* Bit depth and HDR per codec. */
	d = base; d.bit_depth = 8; expect_check(&d, TRUE, NULL);
	d = base; d.bit_depth = 9; expect_check(&d, FALSE, "bit_depth");
	d = base; d.bit_depth = 12; expect_check(&d, FALSE, "bit_depth");
	d = builtin("max_compat"); d.bit_depth = 10; expect_check(&d, FALSE, "bit_depth");
	d = builtin("max_compat"); d.keep_hdr = TRUE; expect_check(&d, FALSE, "hdr");

	/* Encoders. */
	d = base; d.n_encoders = 1; expect_check(&d, TRUE, NULL);
	d = base; d.n_encoders = 0; expect_check(&d, FALSE, "encoders");
	d = base; d.encoders[1] = AXIS_ENCODER_SOFTWARE; expect_check(&d, FALSE, "encoders");

	/* Quality numbers: HEVC even 20 to 32, H.264 the seven the model has. */
	{
		static const gint hevc_ok[] = { 20, 22, 24, 26, 28, 30, 32 };
		static const gint hevc_bad[] = { 0, 16, 18, 19, 21, 27, 33, 34, 51, -26 };
		static const gint h264_ok[] = { 18, 19, 20, 22, 24, 26, 28 };
		static const gint h264_bad[] = { 16, 17, 21, 23, 30, 32 };

		for (i = 0; i < G_N_ELEMENTS(hevc_ok); i++) {
			d = base; d.crf = hevc_ok[i]; expect_check(&d, TRUE, NULL);
			d = base; d.rules[0].crf = hevc_ok[i]; expect_check(&d, TRUE, NULL);
		}
		for (i = 0; i < G_N_ELEMENTS(hevc_bad); i++) {
			d = base; d.crf = hevc_bad[i]; expect_check(&d, FALSE, "quality.default");
			d = base; d.rules[0].crf = hevc_bad[i]; expect_check(&d, FALSE, NULL);
		}
		for (i = 0; i < G_N_ELEMENTS(h264_ok); i++) {
			d = builtin("max_compat"); d.crf = h264_ok[i]; expect_check(&d, TRUE, NULL);
		}
		for (i = 0; i < G_N_ELEMENTS(h264_bad); i++) {
			d = builtin("max_compat"); d.crf = h264_bad[i]; expect_check(&d, FALSE, "quality.default");
		}
	}

	/* Rules: conditions, sizes, duplicates, how many. */
	d = base; d.rules[0].resolution = 0; d.rules[0].min_bytes = 0; d.rules[0].max_bytes = 0;
	expect_check(&d, FALSE, NULL);
	d = base; d.rules[0].resolution = 6; expect_check(&d, FALSE, NULL);
	d = base; d.rules[1].min_bytes = 1; d.rules[1].max_bytes = 1; expect_check(&d, FALSE, NULL);
	d = base; d.rules[1].min_bytes = AXIS_PROFILE_MAX_RULE_BYTES + 1; expect_check(&d, FALSE, NULL);
	d = base; d.rules[1].min_bytes = -5; expect_check(&d, FALSE, NULL);
	d = base; d.rules[1] = d.rules[0]; expect_check(&d, FALSE, NULL);
	d = base;
	for (i = 0; i < AXIS_PROFILE_MAX_RULES; i++) {
		d.rules[i].resolution = 1 + (gint)(i % 5);
		d.rules[i].min_bytes = (gint64)(i + 1) * GB;
		d.rules[i].max_bytes = 0;
		d.rules[i].crf = 24;
	}
	d.n_rules = AXIS_PROFILE_MAX_RULES;
	expect_check(&d, TRUE, NULL);

	/* Audio handling and the stereo track. */
	d = base; d.audio = AXIS_AUDIO_COPY_ADD_STEREO; d.stereo_bitrate = 96000; expect_check(&d, TRUE, NULL);
	d = base; d.audio = AXIS_AUDIO_COPY_ADD_STEREO; d.stereo_bitrate = 95999; expect_check(&d, FALSE, "stereo_bitrate");
	d = base; d.audio = AXIS_AUDIO_STEREO_FIRST; d.stereo_bitrate = 320000; expect_check(&d, TRUE, NULL);
	d = base; d.audio = AXIS_AUDIO_STEREO_FIRST; d.stereo_bitrate = 320001; expect_check(&d, FALSE, "stereo_bitrate");
	d = base; d.audio = AXIS_AUDIO_CONVERT; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.stereo_bitrate = 128000; g_assert_false(axis_profile_definition_check(&d, NULL));

	/* Languages. */
	d = base;
	for (i = 0; i < AXIS_PROFILE_MAX_LANGUAGES; i++) {
		d.languages[i][0] = 'a' + (gchar)(i % 26);
		d.languages[i][1] = 'a' + (gchar)(i / 26);
		d.languages[i][2] = 'x';
		d.languages[i][3] = '\0';
	}
	d.n_languages = AXIS_PROFILE_MAX_LANGUAGES;
	expect_check(&d, TRUE, NULL);
	d = base; memcpy(d.languages[0], "und", 4); d.n_languages = 1; expect_check(&d, FALSE, "keep_languages");
	d = base; memcpy(d.languages[0], "EN", 3); d.n_languages = 1; expect_check(&d, FALSE, "keep_languages");
	d = base; memcpy(d.languages[0], "en1", 4); d.n_languages = 1; expect_check(&d, FALSE, "keep_languages");
	d = base; memcpy(d.languages[0], "eng", 4); memcpy(d.languages[1], "eng", 4); d.n_languages = 2;
	expect_check(&d, FALSE, "keep_languages");

	/* Subtitles and container. */
	d = base; d.container = AXIS_CONTAINER_MP4; expect_check(&d, FALSE, "subtitles");
	d = base; d.container = AXIS_CONTAINER_MP4; d.subtitles = AXIS_SUBTITLES_PLAIN_TEXT; expect_check(&d, TRUE, NULL);
	d = base; d.subtitles = AXIS_SUBTITLES_PLAIN_TEXT; expect_check(&d, TRUE, NULL);
	d = base; d.subtitles = AXIS_SUBTITLES_NONE; expect_check(&d, FALSE, "subtitles");
	d = base; d.container = AXIS_CONTAINER_OGG; expect_check(&d, FALSE, "container");

	/* Already efficient. */
	d = base; d.efficient_milli = 9; expect_check(&d, FALSE, "efficient_bpp_milli");
	d = base; d.efficient_milli = 10; expect_check(&d, TRUE, NULL);
	d = base; d.efficient_milli = 1000; expect_check(&d, TRUE, NULL);
	d = base; d.efficient_milli = 1001; expect_check(&d, FALSE, "efficient_bpp_milli");

	/* A video definition holding audio settings. */
	d = base; d.bitrate = 128000; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.vbr_quality = 0; g_assert_false(axis_profile_definition_check(&d, NULL));

	/* Audio. */
	base = builtin("music_space_saver");
	d = base; d.bitrate = 15999; expect_check(&d, FALSE, "bitrate");
	d = base; d.bitrate = 16000; expect_check(&d, TRUE, NULL);
	d = base; d.bitrate = 256000; expect_check(&d, TRUE, NULL);
	d = base; d.bitrate = 256001; expect_check(&d, FALSE, "bitrate");
	d = base; d.codec = AXIS_TCODEC_AAC; d.bitrate = 31999; expect_check(&d, FALSE, "bitrate");
	d = base; d.codec = AXIS_TCODEC_AAC; d.bitrate = 32000; expect_check(&d, TRUE, NULL);
	d = base; d.codec = AXIS_TCODEC_AAC; d.bitrate = 320000; expect_check(&d, TRUE, NULL);
	d = base; d.codec = AXIS_TCODEC_AAC; d.bitrate = 320001; expect_check(&d, FALSE, "bitrate");
	d = base; d.mono_bitrate = 64000; expect_check(&d, TRUE, NULL);
	d = base; d.mono_bitrate = 128000; expect_check(&d, FALSE, "mono_bitrate");
	d = base; d.mono_bitrate = 8000; expect_check(&d, FALSE, "mono_bitrate");
	d = base; d.efficient_percent = 99; expect_check(&d, FALSE, "efficient_percent");
	d = base; d.efficient_percent = 100; expect_check(&d, TRUE, NULL);
	d = base; d.efficient_percent = 400; expect_check(&d, TRUE, NULL);
	d = base; d.efficient_percent = 401; expect_check(&d, FALSE, "efficient_percent");
	d = base; d.reduce_to_cd = TRUE; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.libraries = AXIS_TARGET_ROOT(AXIS_MEDIA_MOVIES); expect_check(&d, FALSE, "libraries");
	d = base; d.libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_AUDIOBOOKS); expect_check(&d, TRUE, NULL);
	d = base; d.crf = 26; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.codec = AXIS_TCODEC_HEVC; expect_check(&d, FALSE, "codec");

	base = builtin("music_compat");
	for (i = 0; i <= 4; i++) {
		d = base; d.vbr_quality = (gint)i; expect_check(&d, TRUE, NULL);
	}
	d = base; d.vbr_quality = 5; expect_check(&d, FALSE, "vbr_quality");
	d = base; d.vbr_quality = -2; expect_check(&d, FALSE, "vbr_quality");

	base = builtin("keep_lossless");
	d = base; d.reduce_to_cd = TRUE; expect_check(&d, TRUE, NULL);
	d = base; d.allow_lossy = TRUE; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.efficient_percent = 125; g_assert_false(axis_profile_definition_check(&d, NULL));
	d = base; d.bitrate = 128000; g_assert_false(axis_profile_definition_check(&d, NULL));
}

/* Spec: Unknown member.  The text itself: unknown and repeated members,
 * missing ones, wrong types, another format, not JSON at all. */
static void
test_strict_text(void)
{
	const gchar *video = axis_profile_builtin("balanced")->definition;
	const gchar *audio = axis_profile_builtin("audiobooks")->definition;
	static const struct
	{
		const gchar *from;
		const gchar *to;
		const gchar *field;
	} video_cases[] = {
		/* Unknown members, at every level. */
		{ "{\"audio\"", "{\"sneeze\":1,\"audio\"", "" },
		{ "{\"default\"", "{\"sneeze\":1,\"default\"", "" },
		{ "{\"crf\":24", "{\"sneeze\":1,\"crf\":24", "" },
		{ "\"format\":1", "\"format\":1,\"tonemap\":true", "" },
		{ "\"format\":1", "\"format\":1,\"allow_dolby_vision\":true", "" },
		{ "\"format\":1", "\"format\":1,\"skip_pinned\":false", "" },
		{ "\"format\":1", "\"format\":1,\"min_saving_floor\":0", "" },
		/* An audio profile's member in a video one. */
		{ "\"format\":1", "\"format\":1,\"bitrate\":128000", "" },
		/* Repeated. */
		{ "\"codec\":\"hevc\"", "\"codec\":\"hevc\",\"codec\":\"h264\"", "codec" },
		{ "\"crf\":24", "\"crf\":24,\"crf\":26", "quality.rules.1.crf" },
		/* Missing. */
		{ "\"bit_depth\":8,", "", "bit_depth" },
		{ "\"hdr\":\"keep\",", "", "hdr" },
		{ "\"encoders\":[\"software\",\"nvenc\",\"vaapi\"],", "", "encoders" },
		{ "\"keep_languages\":[],", "", "keep_languages" },
		{ "\"min_saving_percent\":15,", "", "min_saving_percent" },
		{ "\"min_saving_bytes\":0,", "", "min_saving_bytes" },
		{ "\"stereo_bitrate\":160000,", "", "stereo_bitrate" },
		{ "\"efficient_bpp_milli\":150,", "", "efficient_bpp_milli" },
		{ "\"default\":26,", "", "quality.default" },
		{ "\"crf\":24,", "", "quality.rules.1.crf" },
		/* Wrong types. */
		{ "\"bit_depth\":8", "\"bit_depth\":\"8\"", "bit_depth" },
		{ "\"bit_depth\":8", "\"bit_depth\":8.5", "bit_depth" },
		{ "\"bit_depth\":8", "\"bit_depth\":true", "bit_depth" },
		{ "\"bit_depth\":8", "\"bit_depth\":null", "bit_depth" },
		{ "\"bit_depth\":8", "\"bit_depth\":[8]", "bit_depth" },
		{ "\"codec\":\"hevc\"", "\"codec\":1", "codec" },
		{ "\"codec\":\"hevc\"", "\"codec\":\"HEVC\"", "codec" },
		{ "\"codec\":\"hevc\"", "\"codec\":\"av1\"", "codec" },
		{ "\"hdr\":\"keep\"", "\"hdr\":\"tonemap\"", "hdr" },
		{ "\"libraries\":[\"movies\",\"tv\",\"other\"]", "\"libraries\":\"movies\"", "libraries" },
		{ "\"libraries\":[\"movies\",\"tv\",\"other\"]", "\"libraries\":[\"movies\",\"movies\"]", "libraries" },
		{ "\"libraries\":[\"movies\",\"tv\",\"other\"]", "\"libraries\":[\"downloads\"]", "libraries" },
		{ "\"encoders\":[\"software\",\"nvenc\",\"vaapi\"]", "\"encoders\":[\"software\",\"software\"]", "encoders" },
		{ "\"encoders\":[\"software\",\"nvenc\",\"vaapi\"]", "\"encoders\":[]", "encoders" },
		{ "\"keep_languages\":[]", "\"keep_languages\":[\"en\"]", "keep_languages" },
		{ "\"keep_languages\":[]", "\"keep_languages\":[\"und\"]", "keep_languages" },
		{ "\"quality\":{", "\"quality\":7,\"q\":{", "quality" },
		{ "\"rules\":[", "\"rules\":[7,", "quality.rules.1" },
		{ "\"resolution\":\"2160p\"}", "\"resolution\":\"4320p\"}", "quality.rules.1.resolution" },
		{ "\"max_bytes\":20000000000", "\"max_bytes\":0", "quality.rules.1.max_bytes" },
		{ "\"stereo_bitrate\":160000", "\"stereo_bitrate\":\"160k\"", "stereo_bitrate" },
		{ "\"min_saving_percent\":15", "\"min_saving_percent\":4", "min_saving_percent" },
		{ "\"min_saving_percent\":15", "\"min_saving_percent\":4.9", "min_saving_percent" },
		{ "\"min_saving_percent\":15", "\"min_saving_percent\":1e400", "min_saving_percent" },
		{ "\"min_saving_percent\":15", "\"min_saving_percent\":9999999999999", "min_saving_percent" },
		/* Another format, kind. */
		{ "\"format\":1", "\"format\":2", "format" },
		{ "\"format\":1", "\"format\":\"1\"", "format" },
		{ "\"kind\":\"video\"", "\"kind\":\"picture\"", "kind" },
		/* Members that do not apply. */
		{ "\"audio\":\"copy_add_stereo\"", "\"audio\":\"copy\"", "stereo_bitrate" },
		{ "\"subtitles\":\"copy\"", "\"subtitles\":\"none\"", "subtitles" },
		{ "\"container\":\"mkv\"", "\"container\":\"mp4\"", "subtitles" }
	};
	static const struct
	{
		const gchar *from;
		const gchar *to;
		const gchar *field;
	} audio_cases[] = {
		{ "\"format\":1", "\"format\":1,\"bit_depth\":10", "" },
		{ "\"format\":1", "\"format\":1,\"vbr_quality\":0", "vbr_quality" },
		{ "\"format\":1", "\"format\":1,\"reduce_to_cd\":false", "reduce_to_cd" },
		{ "\"allow_lossy_source\":true", "\"allow_lossy_source\":1", "allow_lossy_source" },
		{ "\"allow_lossy_source\":true", "\"allow_lossy_source\":\"true\"", "allow_lossy_source" },
		{ "\"allow_lossy_source\":true,", "", "allow_lossy_source" },
		{ "\"bitrate\":48000,", "", "bitrate" },
		{ "\"mono_bitrate\":32000", "\"mono_bitrate\":48000", "mono_bitrate" },
		{ "\"codec\":\"opus\"", "\"codec\":\"vorbis\"", "codec" },
		{ "\"libraries\":[\"audiobooks\"]", "\"libraries\":[\"tv\"]", "libraries" },
		{ "\"efficient_percent\":125", "\"efficient_percent\":50", "efficient_percent" }
	};
	g_autofree gchar *long_text = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(video_cases); i++) {
		g_autofree gchar *text = replaced(video, video_cases[i].from, video_cases[i].to);

		expect_refused(text, video_cases[i].field);
	}
	for (i = 0; i < G_N_ELEMENTS(audio_cases); i++) {
		g_autofree gchar *text = replaced(audio, audio_cases[i].from, audio_cases[i].to);

		expect_refused(text, audio_cases[i].field);
	}
	/* Nine conditions. */
	{
		GString *rules = g_string_new("[");

		for (i = 0; i < 9; i++)
			g_string_append_printf(rules, "%s{\"crf\":24,\"min_bytes\":%u}", i > 0 ? "," : "", i + 1);
		g_string_append_c(rules, ']');
		{
			g_autofree gchar *text = replaced(video, "[{\"crf\":24,\"max_bytes\":20000000000,\"resolution\":\"2160p\"},"
				"{\"crf\":28,\"min_bytes\":50000000000,\"resolution\":\"2160p\"}]", rules->str);

			expect_refused(text, "quality.rules");
		}
		g_string_free(rules, TRUE);
	}
	/* Not a definition at all. */
	expect_refused("", "");
	expect_refused("[]", "");
	expect_refused("{}", "format");
	expect_refused("null", "");
	expect_refused("{\"format\":1}", "kind");
	expect_refused("{\"format\":1,\"kind\":\"video\"", "");
	{
		g_autofree gchar *text = g_strconcat(video, " x", NULL);

		expect_refused(text, "");
	}
	{
		g_autoptr(GPtrArray) problems = axis_profile_problems_new();

		g_assert_false(axis_profile_definition_parse(NULL, -1, NULL, problems));
		g_assert_cmpuint(problems->len, ==, 1);
	}
	/* Longer than the format allows, even with white space only. */
	long_text = g_strnfill(AXIS_PROFILE_MAX_TEXT + 1, ' ');
	memcpy(long_text, video, strlen(video));
	expect_refused(long_text, "");
}

/* ── Canonical form and hash ────────────────────────────────────── */

static void
test_canonical(void)
{
	const gchar *space = axis_profile_builtin("space_saver")->definition;
	g_autofree gchar *reference = hash_of(space);
	static const gchar *const same[] = {
		/* White space, member order, number spelling, set order, the order
		 * of rules that cannot both match. */
		"{ \"format\" : 1 , \"kind\" : \"video\", \"libraries\": [\"other\", \"tv\", \"movies\"], \"codec\": \"hevc\",\n"
		"  \"bit_depth\": 1e1, \"hdr\": \"keep\", \"encoders\": [\"software\", \"nvenc\", \"vaapi\"],\n"
		"  \"quality\": {\"rules\": [{\"resolution\": \"2160p\", \"min_bytes\": 5e10, \"crf\": 28},\n"
		"    {\"max_bytes\": 20000000000.0, \"crf\": 24, \"resolution\": \"2160p\"}], \"default\": 26.0},\n"
		"  \"audio\": \"copy\", \"keep_languages\": [], \"subtitles\": \"copy\", \"container\": \"mkv\",\n"
		"  \"efficient_bpp_milli\": 150, \"min_saving_percent\": 15, \"min_saving_bytes\": 0 }\n",
		"{\"subtitles\":\"copy\",\"quality\":{\"default\":2.6E1,\"rules\":[{\"crf\":24,\"max_bytes\":20000000000,"
		"\"resolution\":\"2160p\"},{\"crf\":28,\"min_bytes\":50000000000,\"resolution\":\"2160p\"}]},"
		"\"min_saving_percent\":15,\"min_saving_bytes\":-0,\"libraries\":[\"tv\",\"movies\",\"other\"],\"kind\":\"video\","
		"\"keep_languages\":[],\"hdr\":\"keep\",\"format\":1,\"encoders\":[\"software\",\"nvenc\",\"vaapi\"],"
		"\"efficient_bpp_milli\":150,\"container\":\"mkv\",\"codec\":\"hevc\",\"bit_depth\":10,\"audio\":\"copy\"}",
		NULL
	};
	AxisProfileDefinition def;
	guint i;

	g_assert_cmpuint(strlen(reference), ==, AXIS_PROFILE_HASH_LENGTH);
	for (i = 0; same[i] != NULL; i++) {
		g_autofree gchar *hash = hash_of(same[i]);

		g_assert_cmpstr(hash, ==, reference);
	}

	/* Equal in meaning: languages in any order. */
	{
		g_autofree gchar *a = replaced(space, "\"keep_languages\":[]", "\"keep_languages\":[\"eng\",\"fre\",\"jpn\"]");
		g_autofree gchar *b = replaced(space, "\"keep_languages\":[]", "\"keep_languages\":[\"jpn\",\"eng\",\"fre\"]");
		g_autofree gchar *ha = hash_of(a);
		g_autofree gchar *hb = hash_of(b);

		g_assert_cmpstr(ha, ==, hb);
		g_assert_cmpstr(ha, !=, reference);
	}
	/* Overlapping rules with different numbers mean different things in a
	 * different order; with the same number they do not. */
	{
		const gchar *rules = "[{\"crf\":24,\"max_bytes\":20000000000,\"resolution\":\"2160p\"},"
			"{\"crf\":28,\"min_bytes\":50000000000,\"resolution\":\"2160p\"}]";
		g_autofree gchar *a = replaced(space, rules, "[{\"crf\":28,\"resolution\":\"2160p\"},{\"crf\":24,\"min_bytes\":1}]");
		g_autofree gchar *b = replaced(space, rules, "[{\"crf\":24,\"min_bytes\":1},{\"crf\":28,\"resolution\":\"2160p\"}]");
		g_autofree gchar *c = replaced(space, rules, "[{\"crf\":24,\"resolution\":\"2160p\"},{\"crf\":24,\"min_bytes\":1}]");
		g_autofree gchar *d = replaced(space, rules, "[{\"crf\":24,\"min_bytes\":1},{\"crf\":24,\"resolution\":\"2160p\"}]");
		g_autofree gchar *ha = hash_of(a);
		g_autofree gchar *hb = hash_of(b);
		g_autofree gchar *hc = hash_of(c);
		g_autofree gchar *hd = hash_of(d);
		AxisProfileDefinition da;
		AxisProfileDefinition db;

		g_assert_cmpstr(ha, !=, hb);
		g_assert_cmpstr(hc, ==, hd);
		g_assert_true(axis_profile_definition_parse(a, -1, &da, NULL));
		g_assert_true(axis_profile_definition_parse(b, -1, &db, NULL));
		/* They do differ: a 2160p file of 30 GB. */
		g_assert_cmpint(axis_profile_quality_for(&da, "2160p", 30 * GB), ==, 28);
		g_assert_cmpint(axis_profile_quality_for(&db, "2160p", 30 * GB), ==, 24);
	}

	/* Parse, canonicalise, parse again: the same; and the canonical text
	 * of every built-in is a fixed point. */
	for (i = 0; i < AXIS_PROFILE_N_BUILTINS; i++) {
		const gchar *text = axis_profile_builtins()[i].definition;
		AxisProfileDefinition again;
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;

		g_assert_true(axis_profile_definition_parse(text, -1, &def, NULL));
		first = axis_profile_definition_canonical(&def);
		g_assert_true(axis_profile_definition_parse(first, -1, &again, NULL));
		second = axis_profile_definition_canonical(&again);
		g_assert_cmpstr(first, ==, second);
		g_assert_true(memcmp(&def, &again, sizeof(def)) == 0);
		g_assert_true(axis_profile_definition_equal(&def, &again));
	}
	/* And the same for many generated ones. */
	{
		g_autoptr(GRand) rand = g_rand_new_with_seed(20261113);

		for (i = 0; i < 2000; i++) {
			AxisGenInput in = { NULL, 0, 0, rand };
			AxisProfileDefinition again;
			g_autofree gchar *first = NULL;
			g_autofree gchar *second = NULL;

			axis_gen_definition(&in, &def);
			first = axis_profile_definition_canonical(&def);
			if (!axis_profile_definition_parse(first, -1, &again, NULL))
				g_error("a valid definition's canonical text is refused: %s", first);
			second = axis_profile_definition_canonical(&again);
			g_assert_cmpstr(first, ==, second);
		}
	}
}

/* ── The lock ───────────────────────────────────────────────────── */

typedef void (*Change)(AxisProfileDefinition *def);

static void ch_codec(AxisProfileDefinition *d) { d->codec = AXIS_TCODEC_H264; d->bit_depth = 8; d->keep_hdr = FALSE; d->crf = 20; d->n_rules = 0; }
static void ch_depth(AxisProfileDefinition *d) { d->bit_depth = 8; }
static void ch_hdr(AxisProfileDefinition *d) { d->keep_hdr = FALSE; }
static void ch_encoder_order(AxisProfileDefinition *d) { d->encoders[0] = AXIS_ENCODER_NVENC; d->encoders[1] = AXIS_ENCODER_SOFTWARE; }
static void ch_encoder_fewer(AxisProfileDefinition *d) { d->n_encoders = 1; }
static void ch_default(AxisProfileDefinition *d) { d->crf = 28; }
static void ch_rule(AxisProfileDefinition *d) { d->rules[0].crf = 22; }
static void ch_rule_added(AxisProfileDefinition *d) { d->rules[d->n_rules].resolution = 3; d->rules[d->n_rules].crf = 24; d->n_rules++; }
static void ch_rules_reordered(AxisProfileDefinition *d) { AxisProfileRule r = d->rules[0]; d->rules[0] = d->rules[1]; d->rules[1] = r; }
static void ch_audio(AxisProfileDefinition *d) { d->audio = AXIS_AUDIO_STEREO_FIRST; d->stereo_bitrate = 160000; }
static void ch_stereo(AxisProfileDefinition *d) { d->audio = AXIS_AUDIO_COPY_ADD_STEREO; d->stereo_bitrate = 192000; }
static void ch_lang_add(AxisProfileDefinition *d) { memcpy(d->languages[d->n_languages++], "jpn", 4); }
static void ch_lang_drop(AxisProfileDefinition *d) { d->n_languages--; }
static void ch_lang_order(AxisProfileDefinition *d) { gchar t[4]; memcpy(t, d->languages[0], 4); memcpy(d->languages[0], d->languages[1], 4); memcpy(d->languages[1], t, 4); }
static void ch_subtitles(AxisProfileDefinition *d) { d->subtitles = AXIS_SUBTITLES_PLAIN_TEXT; }
static void ch_container(AxisProfileDefinition *d) { d->container = AXIS_CONTAINER_MP4; d->subtitles = AXIS_SUBTITLES_PLAIN_TEXT; }
static void ch_efficient(AxisProfileDefinition *d) { d->efficient_milli = 200; }
static void ch_saving(AxisProfileDefinition *d) { d->min_saving_percent = 25; }
static void ch_saving_bytes(AxisProfileDefinition *d) { d->min_saving_bytes = 500000000; }
static void ch_libraries_fewer(AxisProfileDefinition *d) { d->libraries &= ~AXIS_TARGET_ROOT(AXIS_MEDIA_TV); }
static void ch_libraries_more(AxisProfileDefinition *d) { d->libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_TV); }
static void ch_nothing(AxisProfileDefinition *d) { (void)d; }
static void ch_bitrate(AxisProfileDefinition *d) { d->bitrate = 96000; }
static void ch_mono(AxisProfileDefinition *d) { d->mono_bitrate = 24000; }
static void ch_mono_added(AxisProfileDefinition *d) { d->mono_bitrate = 64000; }
static void ch_audio_codec(AxisProfileDefinition *d) { d->codec = AXIS_TCODEC_AAC; d->bitrate = 128000; }
static void ch_vbr(AxisProfileDefinition *d) { d->vbr_quality = 2; }
static void ch_reduce(AxisProfileDefinition *d) { d->reduce_to_cd = TRUE; }
static void ch_lossy_on(AxisProfileDefinition *d) { d->allow_lossy = TRUE; }
static void ch_lossy_off(AxisProfileDefinition *d) { d->allow_lossy = FALSE; }
static void ch_efficient_percent(AxisProfileDefinition *d) { d->efficient_percent = 150; }
static void ch_audio_libraries_more(AxisProfileDefinition *d) { d->libraries |= AXIS_TARGET_ROOT(AXIS_MEDIA_MUSIC); }

/* Spec: Output-affecting change locks; Other change keeps the proof.  Every
 * member of the format, one change each (both directions where the class
 * depends on it). */
static void
test_lock_rule(void)
{
	static const struct
	{
		const gchar *base;
		const gchar *member;
		Change       change;
		gboolean     carries;
	} cases[] = {
		{ "space_saver", "codec", ch_codec, FALSE },
		{ "space_saver", "bit_depth", ch_depth, FALSE },
		{ "space_saver", "hdr", ch_hdr, FALSE },
		{ "space_saver", "encoders", ch_encoder_order, FALSE },
		{ "space_saver", "encoders", ch_encoder_fewer, FALSE },
		{ "space_saver", "quality", ch_default, FALSE },
		{ "space_saver", "quality", ch_rule, FALSE },
		{ "space_saver", "quality", ch_rule_added, FALSE },
		{ "space_saver", NULL, ch_rules_reordered, TRUE },
		{ "space_saver", "audio", ch_audio, FALSE },
		{ "balanced", "stereo_bitrate", ch_stereo, FALSE },
		{ "space_saver", "keep_languages", ch_lang_add, FALSE },
		{ "space_saver", "subtitles", ch_subtitles, FALSE },
		{ "space_saver", "container", ch_container, FALSE },
		{ "space_saver", "efficient_bpp_milli", ch_efficient, TRUE },
		{ "space_saver", "min_saving_percent", ch_saving, TRUE },
		{ "space_saver", "min_saving_bytes", ch_saving_bytes, TRUE },
		{ "space_saver", "libraries", ch_libraries_fewer, TRUE },
		{ "space_saver", NULL, ch_nothing, TRUE },
		{ "music_space_saver", "bitrate", ch_bitrate, FALSE },
		{ "audiobooks", "mono_bitrate", ch_mono, FALSE },
		{ "music_space_saver", "mono_bitrate", ch_mono_added, FALSE },
		{ "music_space_saver", "codec", ch_audio_codec, FALSE },
		{ "music_compat", "vbr_quality", ch_vbr, FALSE },
		{ "keep_lossless", "reduce_to_cd", ch_reduce, FALSE },
		{ "music_space_saver", "allow_lossy_source", ch_lossy_on, FALSE },
		{ "audiobooks", "allow_lossy_source", ch_lossy_off, TRUE },
		{ "music_space_saver", "efficient_percent", ch_efficient_percent, TRUE },
		{ "audiobooks", "libraries", ch_audio_libraries_more, FALSE },
		{ "music_space_saver", "min_saving_percent", ch_saving, TRUE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisProfileDefinition before = builtin(cases[i].base);
		AxisProfileDefinition after = before;
		g_autoptr(GPtrArray) changed = g_ptr_array_new();
		gboolean carries;

		cases[i].change(&after);
		g_assert_true(axis_profile_definition_check(&after, NULL));
		carries = axis_profile_carries_proof(&before, &after, changed);
		if (carries != cases[i].carries)
			g_error("%s, %s: expected %s", cases[i].base, cases[i].member != NULL ? cases[i].member : "nothing",
				cases[i].carries ? "the proof carried" : "a lock");
		if (cases[i].member != NULL) {
			g_assert_cmpuint(changed->len, >, 0);
			if (!g_ptr_array_find_with_equal_func(changed, cases[i].member, g_str_equal, NULL))
				g_error("%s: %s not named", cases[i].base, cases[i].member);
		} else {
			g_assert_cmpuint(changed->len, ==, 0);
		}
	}

	/* The other direction of the narrowing members, and languages either
	 * way (dropping a track changes what a device is offered). */
	{
		AxisProfileDefinition a = builtin("space_saver");
		AxisProfileDefinition b = a;

		/* More libraries than before locks. */
		ch_libraries_fewer(&b);
		g_assert_false(axis_profile_carries_proof(&b, &a, NULL));
		a = b;
		ch_libraries_more(&a);
		g_assert_false(axis_profile_carries_proof(&b, &a, NULL));
		a = builtin("space_saver");
		/* Kept languages eng and fre: the same set in another order carries,
		 * one fewer or one more locks. */
		memcpy(a.languages[0], "eng", 4);
		memcpy(a.languages[1], "fre", 4);
		a.n_languages = 2;
		b = a;
		ch_lang_order(&b);
		g_assert_true(axis_profile_carries_proof(&a, &b, NULL));
		b = a;
		ch_lang_drop(&b);
		g_assert_false(axis_profile_carries_proof(&a, &b, NULL));
		b = a;
		ch_lang_add(&b);
		g_assert_false(axis_profile_carries_proof(&a, &b, NULL));
		/* Back to keeping every track locks too. */
		b = a;
		b.n_languages = 0;
		g_assert_false(axis_profile_carries_proof(&a, &b, NULL));
	}
	/* Another kind locks. */
	{
		AxisProfileDefinition a = builtin("space_saver");
		AxisProfileDefinition b = builtin("music_space_saver");

		g_assert_false(axis_profile_carries_proof(&a, &b, NULL));
	}
	/* Every member of the format has a class, and only "cannot change the
	 * bytes" members carry. */
	{
		static const gchar *const format[] = {
			"format", "kind", "libraries", "codec", "bit_depth", "hdr", "encoders", "quality", "audio",
			"stereo_bitrate", "keep_languages", "subtitles", "container", "efficient_bpp_milli",
			"min_saving_percent", "min_saving_bytes", "bitrate", "mono_bitrate", "vbr_quality", "reduce_to_cd",
			"allow_lossy_source", "efficient_percent", NULL
		};
		static const gchar *const carrying[] = {
			"efficient_bpp_milli", "efficient_percent", "min_saving_percent", "min_saving_bytes", NULL
		};
		const AxisProfileMember *members = axis_profile_members();
		guint n = 0;

		for (i = 0; format[i] != NULL; i++) {
			guint j;
			const AxisProfileMember *m = NULL;

			for (j = 0; members[j].name != NULL; j++) {
				if (strcmp(members[j].name, format[i]) == 0)
					m = &members[j];
			}
			if (m == NULL)
				g_error("%s has no class", format[i]);
			if (m->klass == AXIS_PROFILE_MEMBER_SELECTION && !g_strv_contains(carrying, m->name))
				g_error("%s carries the proof", m->name);
		}
		while (members[n].name != NULL)
			n++;
		g_assert_cmpuint(n, ==, g_strv_length((gchar **)format));
	}
}

/* ── Quality rule ───────────────────────────────────────────────── */

/* Spec: Large 4K; Default. */
static void
test_quality_rule(void)
{
	AxisProfileDefinition space = builtin("space_saver");
	AxisProfileDefinition d;
	const AxisEstimateTarget *target = axis_estimate_builtin_target("space_saver");

	/* Large 4K: the larger the file, the higher the quality number. */
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 60 * GB), ==, 28);
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 50 * GB), ==, 28);
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 50 * GB - 1), ==, 26);
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 20 * GB), ==, 26);
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 20 * GB - 1), ==, 24);
	g_assert_cmpint(axis_profile_quality_for(&space, "2160p", 1), ==, 24);
	/* Default: no condition matches. */
	g_assert_cmpint(axis_profile_quality_for(&space, "1080p", 60 * GB), ==, 26);
	g_assert_cmpint(axis_profile_quality_for(&space, NULL, 10 * GB), ==, 26);
	/* The target's levels say the same. */
	g_assert_cmpint(axis_estimate_target_quality_for(target, "2160p", 60 * GB), ==, 3);
	g_assert_cmpint(axis_estimate_target_quality_for(target, "2160p", 30 * GB), ==, 4);
	g_assert_cmpint(axis_estimate_target_quality_for(target, "2160p", 10 * GB), ==, 5);
	g_assert_cmpint(axis_estimate_target_quality_for(target, "1080p", 60 * GB), ==, 4);

	/* First match wins among conditions that overlap. */
	d = space;
	d.n_rules = 2;
	d.rules[0].resolution = 4;
	d.rules[0].min_bytes = 0;
	d.rules[0].max_bytes = 0;
	d.rules[0].crf = 30;
	d.rules[1].resolution = 0;
	d.rules[1].min_bytes = 40 * GB;
	d.rules[1].max_bytes = 0;
	d.rules[1].crf = 22;
	g_assert_true(axis_profile_definition_check(&d, NULL));
	g_assert_cmpint(axis_profile_quality_for(&d, "2160p", 60 * GB), ==, 30);
	g_assert_cmpint(axis_profile_quality_for(&d, "1080p", 60 * GB), ==, 22);
	g_assert_cmpint(axis_profile_quality_for(&d, "1080p", 10 * GB), ==, 26);
	/* ... and the target built from it decides the same. */
	{
		AxisEstimateTarget t;
		AxisProfileIdentity id = { "custom_1", "Mine", NULL, NULL, 0, 0 };

		g_assert_true(axis_profile_to_target(&d, &id, &t));
		g_assert_cmpint(axis_estimate_quality_value(t.codec, t.encoder,
			axis_estimate_target_quality_for(&t, "2160p", 60 * GB)), ==, 30);
		g_assert_cmpint(axis_estimate_quality_value(t.codec, t.encoder,
			axis_estimate_target_quality_for(&t, "1080p", 60 * GB)), ==, 22);
		g_assert_cmpint(axis_estimate_quality_value(t.codec, t.encoder,
			axis_estimate_target_quality_for(&t, "720p", 1)), ==, 26);
	}
}

/* ── Targets ────────────────────────────────────────────────────── */

static void
test_targets(void)
{
	guint i;

	/* A copy of a built-in makes the same target, but for its name. */
	for (i = 0; i < AXIS_PROFILE_N_BUILTINS; i++) {
		const AxisEstimateTarget *original = &axis_estimate_builtin_targets()[i];
		AxisProfileDefinition def = builtin(axis_profile_builtins()[i].slug);
		AxisProfileIdentity id = { "custom_7", "My copy", NULL, NULL, 12, 1 };
		AxisEstimateTarget copy;

		g_assert_true(axis_profile_to_target(&def, &id, &copy));
		g_assert_cmpstr(copy.id, ==, "custom_7");
		g_assert_cmpint(copy.revision_id, ==, 12);
		copy.id = original->id;
		copy.name = original->name;
		copy.family_name = original->family_name;
		copy.summary = original->summary;
		copy.revision_id = original->revision_id;
		copy.revision = original->revision;
		g_assert_true(memcmp(&copy, original, sizeof(copy)) == 0);
	}
	/* Nothing is made of a definition that does not check. */
	{
		AxisProfileDefinition def = builtin("space_saver");
		AxisEstimateTarget t;

		def.min_saving_percent = 1;
		memset(&t, 0xab, sizeof(t));
		g_assert_false(axis_profile_to_target(&def, NULL, &t));
		g_assert_null(t.id);
		g_assert_cmpint(t.min_saving, ==, 0);
	}
	/* The encoder the workers have, within the profile's preference. */
	{
		AxisProfileDefinition def = builtin("space_saver");
		AxisEstimateConfig config;
		AxisEstimateTarget t;
		AxisEstimateTarget configured;

		axis_estimate_config_init(&config);
		config.encoder = AXIS_ENCODER_NVENC;
		g_assert_true(axis_profile_to_target(&def, NULL, &t));
		axis_estimate_target_configure(&t, &config, &configured);
		g_assert_cmpint(configured.encoder, ==, AXIS_ENCODER_NVENC);
		def.n_encoders = 1;
		g_assert_true(axis_profile_to_target(&def, NULL, &t));
		axis_estimate_target_configure(&t, &config, &configured);
		g_assert_cmpint(configured.encoder, ==, AXIS_ENCODER_SOFTWARE);
		/* Audio is always software. */
		axis_estimate_target_configure(axis_estimate_builtin_target("music_space_saver"), &config, &configured);
		g_assert_cmpint(configured.encoder, ==, AXIS_ENCODER_SOFTWARE);
	}
	/* The definition in words. */
	{
		AxisProfileDefinition def = builtin("space_saver");
		g_autoptr(GPtrArray) facts = axis_profile_describe(&def);
		gboolean quality = FALSE;

		for (i = 0; i < facts->len; i++) {
			const AxisProfileFact *f = g_ptr_array_index(facts, i);

			if (strcmp(f->label, "Quality") == 0) {
				g_assert_nonnull(strstr(f->text, "CRF 26"));
				g_assert_nonnull(strstr(f->text, "CRF 28 for 2160p sources of 50 GB or more"));
				g_assert_nonnull(strstr(f->text, "CRF 24 for 2160p sources under 20 GB"));
				quality = TRUE;
			}
		}
		g_assert_true(quality);
	}
}

/* ── The rules no profile can change ────────────────────────────── */

typedef enum
{
	DEFECT_NONE,
	DEFECT_PINNED,
	DEFECT_LINKED,
	DEFECT_NOT_PROBED,
	DEFECT_PROBE_FAILED,
	DEFECT_CHANGED,
	DEFECT_STALE_SIZE,
	DEFECT_OLD_PROBE,
	DEFECT_HDR_UNCERTAIN,
	DEFECT_HDR_UNCLASSIFIED,
	DEFECT_STREAM_UNCERTAIN,
	DEFECT_DOLBY_VISION,
	DEFECT_STREAM_DOLBY_VISION,
	DEFECT_HDR10PLUS,
	DEFECT_HDR10,
	DEFECT_HLG,
	DEFECT_INTERLACED,
	DEFECT_FIELD_ORDER,
	DEFECT_FIELD_UNKNOWN,
	DEFECT_NO_DURATION,
	DEFECT_INCONSISTENT,
	DEFECT_TINY_SAVING,
	N_DEFECTS
} Defect;

static AxisEstimateReason
expected_reason(Defect defect)
{
	switch (defect) {
	case DEFECT_PINNED:
	case DEFECT_LINKED:
		return AXIS_ESTIMATE_PINNED;
	case DEFECT_NOT_PROBED:
		return AXIS_ESTIMATE_NOT_PROBED;
	case DEFECT_PROBE_FAILED:
		return AXIS_ESTIMATE_PROBE_FAILED;
	case DEFECT_CHANGED:
	case DEFECT_STALE_SIZE:
	case DEFECT_OLD_PROBE:
		return AXIS_ESTIMATE_PROBE_STALE;
	case DEFECT_HDR_UNCERTAIN:
	case DEFECT_HDR_UNCLASSIFIED:
	case DEFECT_STREAM_UNCERTAIN:
		return AXIS_ESTIMATE_HDR_UNCERTAIN;
	case DEFECT_DOLBY_VISION:
	case DEFECT_STREAM_DOLBY_VISION:
		return AXIS_ESTIMATE_DOLBY_VISION;
	case DEFECT_HDR10PLUS:
		return AXIS_ESTIMATE_HDR10PLUS;
	case DEFECT_INTERLACED:
	case DEFECT_FIELD_ORDER:
		return AXIS_ESTIMATE_INTERLACED;
	case DEFECT_FIELD_UNKNOWN:
		return AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN;
	default:
		return AXIS_ESTIMATE_ELIGIBLE;
	}
}

static AxisMediaType
root_for(
	const AxisEstimateTarget *target,
	GRand                    *rand
){
	static const AxisMediaType all[] = {
		AXIS_MEDIA_MOVIES, AXIS_MEDIA_TV, AXIS_MEDIA_OTHER, AXIS_MEDIA_MUSIC, AXIS_MEDIA_AUDIOBOOKS
	};
	AxisMediaType covered[G_N_ELEMENTS(all)];
	guint n = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(all); i++) {
		if ((target->root_types & AXIS_TARGET_ROOT(all[i])) != 0)
			covered[n++] = all[i];
	}
	g_assert_cmpuint(n, >, 0);
	return covered[g_rand_int_range(rand, 0, (gint32)n)];
}

/* A file the target is for, that would get past every fixed rule, then
 * given @defect. */
static AxisMediaInfo *
make_file(
	const AxisEstimateTarget *target,
	Defect                    defect,
	GRand                    *rand,
	AxisEstimateFile         *file
){
	static const gchar *const vcodecs[] = { "h264", "h264", "mpeg2video", "hevc", "vc1" };
	static const gint sizes[][2] = { { 720, 480 }, { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
	static const gchar *const acodecs[] = { "flac", "flac", "pcm_s16le", "mp3", "aac", "alac" };
	AxisMediaInfo *info;
	AxisMediaStream *v = NULL;
	gint64 size;

	memset(file, 0, sizeof(*file));
	if (target->media == AXIS_TARGET_VIDEO) {
		guint at = (guint)g_rand_int_range(rand, 0, G_N_ELEMENTS(sizes));

		info = axis_test_video_info(vcodecs[g_rand_int_range(rand, 0, G_N_ELEMENTS(vcodecs))], sizes[at][0], sizes[at][1],
			24.0, g_rand_int_range(rand, 500000, 60000000), 600.0 + g_rand_int_range(rand, 0, 7200), AXIS_HDR_SDR);
		v = g_ptr_array_index(info->streams, 0);
	} else {
		const gchar *codec = acodecs[g_rand_int_range(rand, 0, G_N_ELEMENTS(acodecs))];

		info = axis_test_audio_info(codec, axis_probe_audio_lossless(codec, NULL),
			g_rand_int_range(rand, 64000, 1500000), 60.0 + g_rand_int_range(rand, 0, 3000), g_rand_int_range(rand, 1, 3),
			44100, 16);
	}
	size = axis_test_natural_size(info);
	axis_test_estimate_index(&file->index, info, target->media == AXIS_TARGET_VIDEO ? "video" : "audio",
		root_for(target, rand), size);
	if (target->media == AXIS_TARGET_AUDIO) {
		file->index.rel_path = "Artist/Album/01.flac";
		file->index.unit = "Artist/Album";
		file->index.unit_files = 1;
	}
	file->info = info;

	switch (defect) {
	case DEFECT_PINNED:
		file->index.pinned = TRUE;
		break;
	case DEFECT_LINKED:
		file->index.nlink = 2;
		break;
	case DEFECT_NOT_PROBED:
		file->info = NULL;
		break;
	case DEFECT_PROBE_FAILED:
		g_free(info->status);
		info->status = g_strdup(g_rand_boolean(rand) ? "failed" : "timeout");
		break;
	case DEFECT_CHANGED:
		file->index.changed = TRUE;
		break;
	case DEFECT_STALE_SIZE:
		file->index.size += 1;
		break;
	case DEFECT_OLD_PROBE:
		info->probe_version = AXIS_PROBE_VERSION - 1;
		break;
	case DEFECT_HDR_UNCERTAIN:
		info->hdr_uncertain = TRUE;
		break;
	case DEFECT_HDR_UNCLASSIFIED:
		info->hdr_format = AXIS_HDR_NONE;
		break;
	case DEFECT_STREAM_UNCERTAIN:
		if (v != NULL)
			v->hdr_uncertain = TRUE;
		break;
	case DEFECT_DOLBY_VISION:
		info->hdr_format = AXIS_HDR_DOLBY_VISION;
		if (v != NULL)
			v->hdr_format = AXIS_HDR_DOLBY_VISION;
		break;
	case DEFECT_STREAM_DOLBY_VISION:
		if (v != NULL)
			v->hdr_format = AXIS_HDR_DOLBY_VISION;
		break;
	case DEFECT_HDR10PLUS:
		info->hdr_format = AXIS_HDR_HDR10PLUS;
		if (v != NULL)
			v->hdr_format = AXIS_HDR_HDR10PLUS;
		break;
	case DEFECT_HDR10:
	case DEFECT_HLG:
		info->hdr_format = defect == DEFECT_HDR10 ? AXIS_HDR_HDR10 : AXIS_HDR_HLG;
		if (v != NULL)
			v->hdr_format = info->hdr_format;
		break;
	case DEFECT_INTERLACED:
		info->interlaced = AXIS_TRI_YES;
		break;
	case DEFECT_FIELD_ORDER:
		if (v != NULL) {
			g_free(v->field_order);
			v->field_order = g_strdup(g_rand_boolean(rand) ? "tt" : "bt");
		}
		break;
	case DEFECT_FIELD_UNKNOWN:
		info->interlaced = AXIS_TRI_UNKNOWN;
		if (v != NULL)
			g_clear_pointer(&v->field_order, g_free);
		break;
	case DEFECT_NO_DURATION:
		info->duration_ms = -1;
		if (v != NULL)
			v->duration_ms = -1;
		break;
	case DEFECT_INCONSISTENT:
		if (v != NULL)
			v->bitrate *= 3;
		break;
	case DEFECT_TINY_SAVING:
		if (v != NULL)
			v->bitrate = 300000;
		file->index.size = axis_test_natural_size(info);
		info->file_size = file->index.size;
		break;
	default:
		break;
	}
	return info;
}

/* Spec: No definition overrides the rules.  Arbitrary valid definitions
 * against files that one fixed rule covers each (and against clean ones):
 * the file is never eligible, and has that rule's reason; whatever does
 * convert is estimated strictly smaller by at least the floor. */
static void
test_fixed_rules_property(void)
{
	g_autoptr(GRand) rand = g_rand_new_with_seed(16);
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	guint covered[AXIS_ESTIMATE_N_REASONS];
	guint converted = 0;
	guint iteration;
	guint i;

	memset(covered, 0, sizeof(covered));
	for (iteration = 0; iteration < 600; iteration++) {
		AxisGenInput in = { NULL, 0, 0, rand };
		AxisProfileDefinition def;
		AxisProfileIdentity id = { "custom_1", "Generated", NULL, NULL, 0, 0 };
		AxisEstimateTarget built;
		AxisEstimateTarget target;
		AxisEstimateConfig config;
		guint defect;

		axis_gen_definition(&in, &def);
		g_assert_true(axis_profile_to_target(&def, &id, &built));
		axis_estimate_config_init(&config);
		config.encoder = (AxisEncoderFamily)g_rand_int_range(rand, 0, AXIS_ENCODER_COUNT);
		axis_estimate_target_configure(&built, &config, &target);
		for (defect = 0; defect < N_DEFECTS; defect++) {
			AxisEstimateFile file;
			AxisEstimateResult result;
			AxisMediaInfo *info = make_file(&target, (Defect)defect, rand, &file);
			AxisEstimateReason want = expected_reason((Defect)defect);

			axis_estimate_eligibility(&file, &target, model, &result);
			axis_gen_check_decision(&file, &target, &result, FALSE);
			if (target.media == AXIS_TARGET_AUDIO && want >= AXIS_ESTIMATE_HDR_UNCERTAIN)
				want = AXIS_ESTIMATE_ELIGIBLE;
			if ((defect == DEFECT_HDR10 || defect == DEFECT_HLG) && target.media == AXIS_TARGET_VIDEO &&
			    target.hdr_bit_depth == 0)
				want = AXIS_ESTIMATE_HDR_TO_SDR;
			if (want != AXIS_ESTIMATE_ELIGIBLE) {
				if (result.reason != want)
					g_error("defect %u, %s profile for roots 0x%x, file in a root of type %d: expected %s, got %s",
						defect, axis_target_codec_to_string(target.codec), target.root_types,
						(gint)file.index.root_type, axis_estimate_reason_to_string(want),
						axis_estimate_reason_to_string(result.reason));
				covered[want]++;
			}
			if (defect == DEFECT_NO_DURATION && target.media == AXIS_TARGET_VIDEO)
				g_assert_false(axis_estimate_result_converts(&result));
			if (defect == DEFECT_INCONSISTENT && target.media == AXIS_TARGET_VIDEO)
				g_assert_false(axis_estimate_result_converts(&result));
			if (axis_estimate_result_converts(&result))
				converted++;
			/* An album of two: the one with the defect keeps its reason, the
			 * other does not convert without it. */
			if (target.media == AXIS_TARGET_AUDIO) {
				AxisEstimateFile other;
				AxisMediaInfo *other_info = make_file(&target, DEFECT_NONE, rand, &other);
				const AxisEstimateFile *unit[2];
				AxisEstimateResult results[2];

				file.index.unit_files = 2;
				other.index.unit_files = 2;
				other.index.root_type = file.index.root_type;
				other.index.file_id = 2;
				unit[0] = &file;
				unit[1] = &other;
				axis_estimate_evaluate(unit, 2, &target, model, results);
				axis_gen_check_decision(&file, &target, &results[0], TRUE);
				axis_gen_check_decision(&other, &target, &results[1], TRUE);
				axis_gen_check_album(&target, results, 2);
				if (want != AXIS_ESTIMATE_ELIGIBLE)
					g_assert_false(axis_estimate_result_converts(&results[1]));
				axis_media_info_free(other_info);
			}
			axis_media_info_free(info);
		}
	}
	/* Every rule was exercised, and the profiles do convert something. */
	for (i = AXIS_ESTIMATE_PINNED; i <= AXIS_ESTIMATE_FIELD_ORDER_UNKNOWN; i++) {
		if (covered[i] == 0)
			g_error("%s was never tried", axis_estimate_reason_to_string((AxisEstimateReason)i));
	}
	g_assert_cmpuint(converted, >, 100);
}

/* Spec: Minimum saving floor.  A target that asks for less than the floor
 * (none can be built from a definition) still gets the floor. */
static void
test_floor_in_code(void)
{
	g_autoptr(AxisEstimateModel) model = axis_estimate_model_new();
	AxisEstimateTarget target = *axis_estimate_builtin_target("space_saver");
	AxisMediaInfo *info = axis_test_video_info("h264", 1920, 1080, 24.0, 8000000, 7200.0, AXIS_HDR_SDR);
	AxisEstimateFile file;
	AxisEstimateResult result;
	gdouble values[] = { 0.0, -1.0, NAN, 0.049 };
	guint i;

	axis_test_estimate_index(&file.index, info, "video", AXIS_MEDIA_MOVIES, axis_test_natural_size(info));
	file.info = info;
	for (i = 0; i < G_N_ELEMENTS(values); i++) {
		gdouble saving;

		target.min_saving = values[i];
		axis_estimate_eligibility(&file, &target, model, &result);
		g_assert_true(result.sized);
		saving = (gdouble)(result.now - result.mid) / (gdouble)result.now;
		if (axis_estimate_result_converts(&result))
			g_assert_cmpfloat(saving, >=, AXIS_ESTIMATE_MIN_SAVING_FLOOR);
	}
	/* A file that would save 3 % is never converted, whatever the target
	 * says: the stereo track a target adds makes a small saving. */
	target = *axis_estimate_builtin_target("balanced");
	target.min_saving = 0.0;
	{
		AxisMediaStream *v = g_ptr_array_index(info->streams, 0);
		gint low = 100000;
		gint high = 8000000;

		/* Find a bitrate where the estimate saves between 0 and 5 %. */
		while (high - low > 1000) {
			gint middle = (low + high) / 2;
			gdouble saving;

			v->bitrate = middle;
			file.index.size = axis_test_natural_size(info);
			info->file_size = file.index.size;
			axis_estimate_eligibility(&file, &target, model, &result);
			saving = result.sized ? (gdouble)(result.now - result.mid) / (gdouble)result.now : 1.0;
			if (saving < 0.03)
				low = middle;
			else
				high = middle;
		}
		v->bitrate = low;
		file.index.size = axis_test_natural_size(info);
		info->file_size = file.index.size;
		axis_estimate_eligibility(&file, &target, model, &result);
		g_assert_true(result.sized);
		g_assert_cmpfloat((gdouble)(result.now - result.mid) / (gdouble)result.now, <, 0.05);
		g_assert_cmpint(result.reason, !=, AXIS_ESTIMATE_ELIGIBLE);
	}
	/* An HDR source and a target that claims to keep HDR but is not 10-bit
	 * HEVC: never converted. */
	{
		AxisEstimateTarget odd = *axis_estimate_builtin_target("max_compat");
		AxisMediaInfo *hdr = axis_test_video_info("hevc", 3840, 2160, 24.0, 40000000, 7200.0, AXIS_HDR_HDR10);
		AxisEstimateFile f;

		axis_test_estimate_index(&f.index, hdr, "video", AXIS_MEDIA_MOVIES, axis_test_natural_size(hdr));
		f.info = hdr;
		odd.hdr_bit_depth = 10;
		g_assert_cmpint(axis_estimate_eligibility(&f, &odd, model, &result), ==, AXIS_ESTIMATE_HDR_TO_SDR);
		odd = *axis_estimate_builtin_target("space_saver");
		odd.hdr_bit_depth = 8;
		g_assert_cmpint(axis_estimate_eligibility(&f, &odd, model, &result), ==, AXIS_ESTIMATE_HDR_TO_SDR);
		axis_media_info_free(hdr);
	}
	axis_media_info_free(info);
}

/* ── Names ──────────────────────────────────────────────────────── */

static void
test_clean_text(void)
{
	g_autofree gchar *a = axis_profile_clean_text("  Kids'  TV\t profile \n", 80, FALSE);
	g_autofree gchar *b = axis_profile_clean_text("a\xe2\x80\xae" "b<script>", 80, FALSE);
	g_autofree gchar *c = axis_profile_clean_text("line one\n\n  line two  ", 500, TRUE);
	g_autofree gchar *d = axis_profile_clean_text("abcdefghij", 4, FALSE);
	g_autofree gchar *e = axis_profile_clean_text(" \t\n ", 80, FALSE);
	g_autofree gchar *f = axis_profile_clean_text("\xff\xfe", 80, FALSE);

	g_assert_cmpstr(a, ==, "Kids' TV profile");
	g_assert_cmpstr(b, ==, "ab<script>");
	g_assert_cmpstr(c, ==, "line one\n\nline two");
	g_assert_cmpstr(d, ==, "abcd");
	g_assert_null(e);
	g_assert_null(f);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/profile/builtins", test_builtins);
	g_test_add_func("/profile/bounds", test_bounds);
	g_test_add_func("/profile/strict-text", test_strict_text);
	g_test_add_func("/profile/canonical", test_canonical);
	g_test_add_func("/profile/lock-rule", test_lock_rule);
	g_test_add_func("/profile/quality-rule", test_quality_rule);
	g_test_add_func("/profile/targets", test_targets);
	g_test_add_func("/profile/fixed-rules-property", test_fixed_rules_property);
	g_test_add_func("/profile/floor-in-code", test_floor_in_code);
	g_test_add_func("/profile/clean-text", test_clean_text);
	return g_test_run();
}
