/*
 * test-quality.c - quality profiles: the ladder, the profile and its strict
 * format, the built-ins, and the decision with its reasons, ranking and
 * upgrades
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (quality-profiles):
 *   Order; Unknown; Not allowed; Required field; Several reasons; Ambiguous
 *   match; Cinema recording; Quality first; At the cutoff; A file Axis
 *   converted
 *
 * The page's scenarios (Try a name, Built-in) are in test-web-quality.c, the
 * store's in test-quality-db.c; the profile reader is fuzzed by
 * tests/fuzz/fuzz-quality-profile.c.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "quality/axis-quality.h"

#define RELEASE_DIR "tests/fixtures/release/"
#define LATEST_YEAR 2027

static AxisRelease *
parse(const gchar *name)
{
	return axis_release_parse_full(name, -1, LATEST_YEAR);
}

/* ── Helpers ────────────────────────────────────────────────────── */

typedef struct
{
	AxisRelease          *release;
	AxisQualityCandidate  c;
} Cand;

static void
cand_init(
	Cand        *cand,
	const gchar *name,
	gint64       size,
	AxisDownloadProtocol protocol,
	gint         seeders
){
	memset(cand, 0, sizeof(*cand));
	cand->release = parse(name);
	cand->c.release = cand->release;
	cand->c.name = name;
	cand->c.match = AXIS_RELEASE_MATCH_YES;
	cand->c.size_bytes = size;
	cand->c.protocol = protocol;
	cand->c.seeders = seeders;
	cand->c.published_us = 0;
}

static void
cand_clear(Cand *cand)
{
	axis_release_free(cand->release);
}

static void
context_init(
	AxisQualityContext       *ctx,
	AxisQualityTitle         *title,
	const AxisQualityProfile *profile,
	gint                      runtime
){
	memset(ctx, 0, sizeof(*ctx));
	memset(title, 0, sizeof(*title));
	title->noun = "film";
	title->runtime_minutes = runtime;
	ctx->profile = profile;
	ctx->title = title;
	ctx->now_us = G_GINT64_CONSTANT(1800000000) * G_USEC_PER_SEC;
}

static AxisQualityProfile
builtin(const gchar *slug)
{
	AxisQualityProfile p;

	g_assert_true(axis_quality_builtin_profile(slug, &p));
	return p;
}

/* A profile that allows everything, unwanted included, with no limits. */
static AxisQualityProfile
permissive(void)
{
	AxisQualityProfile p;
	guint q;

	axis_quality_profile_init(&p);
	for (q = AXIS_QUALITY_N_LADDER; q >= 1; q--) {
		p.allowed[p.n_allowed] = (AxisQuality)q;
		p.tier[p.n_allowed] = p.n_allowed;
		p.n_allowed++;
		p.sizes[q].min = 0;
		p.sizes[q].max = 0;
	}
	p.cutoff = AXIS_QUALITY_DISC;
	p.allow_unknown = TRUE;
	p.allow_unwanted = TRUE;
	g_assert_true(axis_quality_profile_check(&p, NULL));
	return p;
}

static void
add_rule(
	AxisQualityProfile   *p,
	AxisQualityCondition  condition,
	AxisQualityEffect     effect,
	gint                  weight,
	const gchar          *value
){
	AxisQualityRule *r = &p->rules[p->n_rules++];

	memset(r, 0, sizeof(*r));
	r->condition = condition;
	r->effect = effect;
	r->weight = weight;
	if (value != NULL) {
		g_strlcpy(r->values[0], value, sizeof(r->values[0]));
		r->n_values = 1;
	}
}

static gboolean
has_reason(
	const AxisQualityVerdict *v,
	const gchar              *code
){
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisQualityReason *r = g_ptr_array_index(v->reasons, i);

		if (strcmp(r->code, code) == 0)
			return TRUE;
	}
	return FALSE;
}

static const gchar *
reason_text(
	const AxisQualityVerdict *v,
	const gchar              *code
){
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisQualityReason *r = g_ptr_array_index(v->reasons, i);

		if (strcmp(r->code, code) == 0)
			return r->text;
	}
	return NULL;
}

static gchar *
reasons_of(const AxisQualityVerdict *v)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisQualityReason *r = g_ptr_array_index(v->reasons, i);

		g_string_append_printf(out, "[%s] %s; ", r->code, r->text);
	}
	return g_string_free(out, FALSE);
}

static AxisQualityVerdict *
decide_name(
	const AxisQualityContext *ctx,
	const gchar              *name,
	gint64                    size
){
	Cand cand;
	AxisQualityVerdict *v;

	cand_init(&cand, name, size, AXIS_DOWNLOAD_USENET, -1);
	v = axis_quality_decide_one(ctx, &cand.c);
	cand_clear(&cand);
	return v;
}

/* ── The ladder ─────────────────────────────────────────────────── */

static void
test_ladder_order(void)
{
	static const AxisQuality order[] = {
		AXIS_QUALITY_WORKPRINT, AXIS_QUALITY_CAM, AXIS_QUALITY_TELESYNC, AXIS_QUALITY_TELECINE, AXIS_QUALITY_SCREENER,
		AXIS_QUALITY_DVD, AXIS_QUALITY_SDTV, AXIS_QUALITY_WEB_480P, AXIS_QUALITY_BLURAY_480P,
		AXIS_QUALITY_HDTV_720P, AXIS_QUALITY_WEBRIP_720P, AXIS_QUALITY_WEBDL_720P, AXIS_QUALITY_BLURAY_720P,
		AXIS_QUALITY_HDTV_1080P, AXIS_QUALITY_WEBRIP_1080P, AXIS_QUALITY_WEBDL_1080P, AXIS_QUALITY_BLURAY_1080P,
		AXIS_QUALITY_REMUX_1080P, AXIS_QUALITY_HDTV_2160P, AXIS_QUALITY_WEBRIP_2160P, AXIS_QUALITY_WEBDL_2160P,
		AXIS_QUALITY_BLURAY_2160P, AXIS_QUALITY_REMUX_2160P, AXIS_QUALITY_DISC
	};
	static const AxisQualityClass classes[] = {
		AXIS_QUALITY_CLASS_UNWANTED, AXIS_QUALITY_CLASS_UNWANTED, AXIS_QUALITY_CLASS_UNWANTED,
		AXIS_QUALITY_CLASS_UNWANTED, AXIS_QUALITY_CLASS_UNWANTED, AXIS_QUALITY_CLASS_SD, AXIS_QUALITY_CLASS_SD,
		AXIS_QUALITY_CLASS_SD, AXIS_QUALITY_CLASS_SD, AXIS_QUALITY_CLASS_720P, AXIS_QUALITY_CLASS_720P,
		AXIS_QUALITY_CLASS_720P, AXIS_QUALITY_CLASS_720P, AXIS_QUALITY_CLASS_1080P, AXIS_QUALITY_CLASS_1080P,
		AXIS_QUALITY_CLASS_1080P, AXIS_QUALITY_CLASS_1080P, AXIS_QUALITY_CLASS_1080P, AXIS_QUALITY_CLASS_2160P,
		AXIS_QUALITY_CLASS_2160P, AXIS_QUALITY_CLASS_2160P, AXIS_QUALITY_CLASS_2160P, AXIS_QUALITY_CLASS_2160P,
		AXIS_QUALITY_CLASS_DISC
	};
	guint i;

	G_STATIC_ASSERT(G_N_ELEMENTS(order) == AXIS_QUALITY_N_LADDER);
	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		AxisQuality back;

		g_assert_cmpint(axis_quality_class(order[i]), ==, classes[i]);
		g_assert_cmpint(axis_quality_compare(AXIS_QUALITY_UNKNOWN, order[i]), <, 0);
		g_assert_cmpint(axis_quality_compare(order[i], order[i]), ==, 0);
		if (i > 0) {
			g_assert_cmpint(axis_quality_compare(order[i - 1], order[i]), <, 0);
			g_assert_cmpint(axis_quality_compare(order[i], order[i - 1]), >, 0);
		}
		g_assert_true(axis_quality_from_name(axis_quality_name(order[i]), &back));
		g_assert_cmpint(back, ==, order[i]);
		g_assert_cmpint(axis_quality_is_unwanted(order[i]), ==, classes[i] == AXIS_QUALITY_CLASS_UNWANTED);
	}
	/* Unknown is not a name of the ladder. */
	{
		AxisQuality any = AXIS_QUALITY_UNKNOWN;

		g_assert_false(axis_quality_from_name("unknown", &any));
		g_assert_false(axis_quality_from_name("WEBDL_1080P", &any));
		g_assert_false(axis_quality_from_name(NULL, &any));
	}
	g_assert_cmpstr(axis_quality_label(AXIS_QUALITY_WEBDL_1080P), ==, "WEB-DL 1080p");
	g_assert_cmpstr(axis_quality_label(AXIS_QUALITY_REMUX_1080P), ==, "Remux 1080p");
	g_assert_cmpstr(axis_quality_name((AxisQuality)99), ==, "unknown");

	/* Scenario: Order.  A WEB-DL 1080p and a BluRay 1080p: the BluRay is the
	 * higher. */
	{
		g_autoptr(AxisRelease) web = parse("Some.Film.2021.1080p.WEB-DL.DDP5.1.H.264-GRP");
		g_autoptr(AxisRelease) bluray = parse("Some.Film.2021.1080p.BluRay.x264-GRP");

		g_assert_cmpint(axis_quality_of_release(web), ==, AXIS_QUALITY_WEBDL_1080P);
		g_assert_cmpint(axis_quality_of_release(bluray), ==, AXIS_QUALITY_BLURAY_1080P);
		g_assert_cmpint(axis_quality_compare(axis_quality_of_release(bluray), axis_quality_of_release(web)), >, 0);
	}
}

static void
test_ladder_placement(void)
{
	/* Every source with every resolution, written out: N is none. */
	static const struct
	{
		AxisReleaseSource     source;
		AxisReleaseResolution resolution;
		AxisQuality           expected;
	} cases[] = {
#define N AXIS_RELEASE_RESOLUTION_NONE
#define R480 AXIS_RELEASE_RESOLUTION_480P
#define R576 AXIS_RELEASE_RESOLUTION_576P
#define R720 AXIS_RELEASE_RESOLUTION_720P
#define R1080 AXIS_RELEASE_RESOLUTION_1080P
#define R2160 AXIS_RELEASE_RESOLUTION_2160P
		{ AXIS_RELEASE_SOURCE_NONE, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_NONE, R1080, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_NONE, R2160, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_CAM, N, AXIS_QUALITY_CAM },
		{ AXIS_RELEASE_SOURCE_CAM, R1080, AXIS_QUALITY_CAM },
		{ AXIS_RELEASE_SOURCE_TELESYNC, R720, AXIS_QUALITY_TELESYNC },
		{ AXIS_RELEASE_SOURCE_TELECINE, N, AXIS_QUALITY_TELECINE },
		{ AXIS_RELEASE_SOURCE_SCREENER, R480, AXIS_QUALITY_SCREENER },
		{ AXIS_RELEASE_SOURCE_WORKPRINT, N, AXIS_QUALITY_WORKPRINT },
		{ AXIS_RELEASE_SOURCE_DVD, N, AXIS_QUALITY_DVD },
		{ AXIS_RELEASE_SOURCE_DVD, R576, AXIS_QUALITY_DVD },
		{ AXIS_RELEASE_SOURCE_DVD, R1080, AXIS_QUALITY_DVD },
		{ AXIS_RELEASE_SOURCE_HDTV, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_HDTV, R480, AXIS_QUALITY_SDTV },
		{ AXIS_RELEASE_SOURCE_HDTV, R576, AXIS_QUALITY_SDTV },
		{ AXIS_RELEASE_SOURCE_HDTV, R720, AXIS_QUALITY_HDTV_720P },
		{ AXIS_RELEASE_SOURCE_HDTV, R1080, AXIS_QUALITY_HDTV_1080P },
		{ AXIS_RELEASE_SOURCE_HDTV, R2160, AXIS_QUALITY_HDTV_2160P },
		{ AXIS_RELEASE_SOURCE_WEBRIP, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_WEBRIP, R480, AXIS_QUALITY_WEB_480P },
		{ AXIS_RELEASE_SOURCE_WEBRIP, R720, AXIS_QUALITY_WEBRIP_720P },
		{ AXIS_RELEASE_SOURCE_WEBRIP, R1080, AXIS_QUALITY_WEBRIP_1080P },
		{ AXIS_RELEASE_SOURCE_WEBRIP, R2160, AXIS_QUALITY_WEBRIP_2160P },
		{ AXIS_RELEASE_SOURCE_WEBDL, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_WEBDL, R576, AXIS_QUALITY_WEB_480P },
		{ AXIS_RELEASE_SOURCE_WEBDL, R720, AXIS_QUALITY_WEBDL_720P },
		{ AXIS_RELEASE_SOURCE_WEBDL, R1080, AXIS_QUALITY_WEBDL_1080P },
		{ AXIS_RELEASE_SOURCE_WEBDL, R2160, AXIS_QUALITY_WEBDL_2160P },
		{ AXIS_RELEASE_SOURCE_BLURAY, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_BLURAY, R480, AXIS_QUALITY_BLURAY_480P },
		{ AXIS_RELEASE_SOURCE_BLURAY, R576, AXIS_QUALITY_BLURAY_480P },
		{ AXIS_RELEASE_SOURCE_BLURAY, R720, AXIS_QUALITY_BLURAY_720P },
		{ AXIS_RELEASE_SOURCE_BLURAY, R1080, AXIS_QUALITY_BLURAY_1080P },
		{ AXIS_RELEASE_SOURCE_BLURAY, R2160, AXIS_QUALITY_BLURAY_2160P },
		{ AXIS_RELEASE_SOURCE_REMUX, N, AXIS_QUALITY_UNKNOWN },
		{ AXIS_RELEASE_SOURCE_REMUX, R576, AXIS_QUALITY_BLURAY_480P },
		{ AXIS_RELEASE_SOURCE_REMUX, R720, AXIS_QUALITY_BLURAY_720P },
		{ AXIS_RELEASE_SOURCE_REMUX, R1080, AXIS_QUALITY_REMUX_1080P },
		{ AXIS_RELEASE_SOURCE_REMUX, R2160, AXIS_QUALITY_REMUX_2160P },
		{ AXIS_RELEASE_SOURCE_DISC, N, AXIS_QUALITY_DISC },
		{ AXIS_RELEASE_SOURCE_DISC, R2160, AXIS_QUALITY_DISC },
#undef N
#undef R480
#undef R576
#undef R720
#undef R1080
#undef R2160
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisQuality got = axis_quality_of(cases[i].source, cases[i].resolution);

		if (got != cases[i].expected)
			g_error("source %s at %s: %s, expected %s", axis_release_source_name(cases[i].source),
				axis_release_resolution_name(cases[i].resolution), axis_quality_name(got),
				axis_quality_name(cases[i].expected));
	}
}

/* Scenario: Unknown.  A name that states no resolution is of unknown
 * quality, and says why. */
static void
test_ladder_unknown(void)
{
	g_autoptr(AxisRelease) web = parse("Some.Film.2020.WEB-DL.x264-GRP");
	g_autoptr(AxisRelease) none = parse("Some.Film.2020.x264-GRP");
	g_autoptr(AxisRelease) cam = parse("Some.Film.2024.CAM.x264-GRP");
	g_autofree gchar *why_web = axis_quality_explain_place(web);
	g_autofree gchar *why_none = axis_quality_explain_place(none);
	g_autofree gchar *why_cam = axis_quality_explain_place(cam);

	g_assert_cmpint(axis_quality_of_release(web), ==, AXIS_QUALITY_UNKNOWN);
	g_assert_nonnull(strstr(why_web, "no resolution"));
	g_assert_cmpint(axis_quality_of_release(none), ==, AXIS_QUALITY_UNKNOWN);
	g_assert_nonnull(strstr(why_none, "neither"));
	/* A recording is a recording whatever it does not say. */
	g_assert_cmpint(axis_quality_of_release(cam), ==, AXIS_QUALITY_CAM);
	g_assert_nonnull(strstr(why_cam, "source alone"));
	g_assert_cmpint(axis_quality_of_release(NULL), ==, AXIS_QUALITY_UNKNOWN);
}

/* The labels of a corpus line, as the ladder would place them. */
static AxisQuality
labelled_quality(JsonObject *labels)
{
	const gchar *source = json_object_has_member(labels, "source") ? json_object_get_string_member(labels, "source") :
		NULL;
	const gchar *resolution = json_object_has_member(labels, "resolution") ?
		json_object_get_string_member(labels, "resolution") : NULL;
	AxisReleaseSource s = AXIS_RELEASE_SOURCE_NONE;
	AxisReleaseResolution r = AXIS_RELEASE_RESOLUTION_NONE;
	guint i;

	for (i = 1; source != NULL && i <= AXIS_RELEASE_SOURCE_DISC; i++) {
		if (g_strcmp0(axis_release_source_name((AxisReleaseSource)i), source) == 0)
			s = (AxisReleaseSource)i;
	}
	g_assert_true(source == NULL || s != AXIS_RELEASE_SOURCE_NONE);
	for (i = 1; resolution != NULL && i <= AXIS_RELEASE_RESOLUTION_2160P; i++) {
		if (g_strcmp0(axis_release_resolution_name((AxisReleaseResolution)i), resolution) == 0)
			r = (AxisReleaseResolution)i;
	}
	g_assert_true(resolution == NULL || r != AXIS_RELEASE_RESOLUTION_NONE);
	return axis_quality_of(s, r);
}

/* Every name of the parser's corpora lands on the quality its labels say:
 * the labels were written by hand, independently of the parser. */
static void
test_ladder_corpus(void)
{
	static const gchar *const files[] = { "films.json", "episodes.json", "regressions.json", NULL };
	guint counts[AXIS_QUALITY_N_LADDER + 1];
	g_autoptr(GString) wrong = g_string_new(NULL);
	guint total = 0;
	guint f;
	guint i;

	memset(counts, 0, sizeof(counts));
	for (f = 0; files[f] != NULL; f++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autofree gchar *path = g_strconcat(RELEASE_DIR, files[f], NULL);
		g_autoptr(GError) error = NULL;
		JsonArray *lines;

		if (!json_parser_load_from_file(parser, path, &error))
			g_error("%s: %s", path, error->message);
		lines = json_node_get_array(json_parser_get_root(parser));
		for (i = 0; i < json_array_get_length(lines); i++) {
			JsonObject *labels = json_array_get_object_element(lines, i);
			const gchar *name = json_object_get_string_member(labels, "name");
			g_autoptr(AxisRelease) release = parse(name);
			AxisQuality expected = labelled_quality(labels);
			AxisQuality got = axis_quality_of_release(release);

			total++;
			counts[got]++;
			if (got != expected)
				g_string_append_printf(wrong, "\n  %s: %s, expected %s", name, axis_quality_name(got),
					axis_quality_name(expected));
		}
	}
	if (wrong->len > 0)
		g_error("names on the wrong quality:%s", wrong->str);
	g_assert_cmpuint(total, >=, 300);
	for (i = 0; i <= AXIS_QUALITY_N_LADDER; i++) {
		if (counts[i] > 0)
			g_test_message("%-14s %u", axis_quality_name((AxisQuality)i), counts[i]);
	}
	/* The corpora reach most of the ladder. */
	g_assert_cmpuint(counts[AXIS_QUALITY_WEBDL_1080P], >, 0);
	g_assert_cmpuint(counts[AXIS_QUALITY_REMUX_2160P], >, 0);
	g_assert_cmpuint(counts[AXIS_QUALITY_CAM], >, 0);
	g_assert_cmpuint(counts[AXIS_QUALITY_UNKNOWN], >, 0);
}

/* ── Profiles ───────────────────────────────────────────────────── */

static void
test_profile_builtins(void)
{
	const AxisQualityBuiltin *b = axis_quality_builtins();
	AxisQualityProfile p;
	guint i;

	for (i = 0; i < AXIS_QUALITY_N_BUILTINS; i++) {
		g_autoptr(GPtrArray) problems = axis_quality_problems_new();
		g_autofree gchar *first = NULL;
		g_autofree gchar *second = NULL;
		AxisQualityProfile again;

		g_assert_true(axis_quality_builtin_profile(b[i].slug, &p));
		g_assert_true(axis_quality_profile_check(&p, problems));
		g_assert_cmpuint(problems->len, ==, 0);
		g_assert_true(axis_quality_slug_valid(b[i].slug));
		g_assert_true(axis_quality_slug_reserved(b[i].slug));
		g_assert_true(axis_quality_builtin(b[i].slug) == &b[i]);
		/* No built-in takes a recording, or a disc image (but Anything, last). */
		g_assert_false(p.allow_unwanted);
		g_assert_cmpint(p.players, ==, 0);
		first = axis_quality_profile_canonical(&p);
		/* What migration 0022 writes for it (test-quality-db.c checks they
		 * agree). */
		g_test_message("%s %s", b[i].slug, first);
		g_assert_true(axis_quality_profile_parse(first, -1, &again, NULL));
		second = axis_quality_profile_canonical(&again);
		g_assert_cmpstr(first, ==, second);
		g_assert_true(memcmp(&p, &again, sizeof(p)) == 0);
		g_assert_cmpuint(strlen(first), <=, AXIS_QUALITY_MAX_TEXT);
	}
	g_assert_false(axis_quality_builtin_profile("custom_1", &p));
	g_assert_false(axis_quality_slug_reserved("custom_1"));
	g_assert_nonnull(axis_quality_builtin(AXIS_QUALITY_DEFAULT_SLUG));

	p = builtin("space_saver");
	g_assert_cmpint(p.cutoff, ==, AXIS_QUALITY_WEBDL_1080P);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_WEBRIP_1080P), ==, 0);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_WEBDL_1080P), ==, 0);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_BLURAY_1080P), ==, -1);
	p = builtin("balanced");
	g_assert_cmpint(p.cutoff, ==, AXIS_QUALITY_BLURAY_1080P);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_REMUX_1080P), ==, -1);
	p = builtin("best_1080p");
	g_assert_cmpint(p.cutoff, ==, AXIS_QUALITY_REMUX_1080P);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_REMUX_1080P), ==, 0);
	p = builtin("uhd");
	g_assert_cmpint(p.cutoff, ==, AXIS_QUALITY_WEBDL_2160P);
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_WEBDL_1080P), ==, -1);
	p = builtin("anything");
	g_assert_true(p.allow_unknown);
	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++)
		g_assert_cmpint(axis_quality_profile_tier(&p, (AxisQuality)i) >= 0, ==, !axis_quality_is_unwanted((AxisQuality)i));
	g_assert_cmpint(axis_quality_profile_tier(&p, AXIS_QUALITY_DISC), ==, (gint)axis_quality_profile_n_tiers(&p) - 1);
}

/* A stored profile that is invalid, or from a newer version, is refused
 * whole: nothing of it is returned. */
static void
test_profile_strict(void)
{
	AxisQualityProfile good = builtin("balanced");
	g_autofree gchar *text = axis_quality_profile_canonical(&good);
	static const struct
	{
		const gchar *from;
		const gchar *to;
		const gchar *field;
	} edits[] = {
		{ "\"format\":1", "\"format\":2", "format" },
		{ "\"format\":1", "\"format\":0", "format" },
		{ "\"format\":1", "\"format\":\"1\"", "format" },
		{ "\"cutoff\":\"bluray_1080p\"", "\"cutoff\":\"remux_1080p\"", "cutoff" },
		{ "\"cutoff\":\"bluray_1080p\"", "\"cutoff\":\"unknown\"", "cutoff" },
		{ "\"players\":0", "\"players\":1001", "players" },
		{ "\"players\":0", "\"players\":0.5", "players" },
		{ "\"players\":0", "\"players\":0,\"players\":0", "players" },
		{ "\"players\":0", "\"players\":0,\"extra\":1", "" },
		{ "\"unknown\":false", "\"unknown\":0", "unknown" },
		{ "\"unknown\":false,", "", "unknown" },
		{ "\"max_size_mb\":24576", "\"max_size_mb\":-1", "max_size_mb" },
		{ "\"max_size_mb\":24576", "\"max_size_mb\":1024001", "max_size_mb" },
		{ "\"bluray_1080p\":[6,200]", "\"bluray_1080p\":[200,6]", "sizes.bluray_1080p" },
		{ "\"bluray_1080p\":[6,200]", "\"bluray_1080p\":[6,2001]", "sizes.bluray_1080p" },
		{ "\"bluray_1080p\":[6,200],", "", "sizes.bluray_1080p" },
		{ "\"bluray_1080p\":[6,200]", "\"bluray_1080p\":[6]", "sizes.bluray_1080p" },
		{ "[[\"bluray_1080p\"],", "[[\"bluray_1080p\"],[\"bluray_1080p\"],", "qualities" },
		{ "[[\"bluray_1080p\"],", "[[\"bluray_1080p\"],[],", "qualities" },
		{ "[[\"bluray_1080p\"],", "[[\"bluray_1080p\",\"cam2\"],", "qualities" },
		{ "{\"if\":\"3d\",\"then\":\"reject\"}", "{\"if\":\"3d\",\"is\":[\"x\"],\"then\":\"reject\"}", "rules.2.is" },
		{ "{\"if\":\"3d\",\"then\":\"reject\"}", "{\"if\":\"3d\",\"then\":\"reject\",\"weight\":5}", "rules.2.weight" },
		{ "{\"if\":\"3d\",\"then\":\"reject\"}", "{\"if\":\"4d\",\"then\":\"reject\"}", "rules.2.if" },
		{ "{\"if\":\"3d\",\"then\":\"reject\"}", "{\"if\":\"3d\",\"then\":\"ban\"}", "rules.2.then" },
		{ "{\"if\":\"3d\",\"then\":\"reject\"}", "{\"if\":\"3d\",\"then\":\"reject\"},{\"if\":\"3d\",\"then\":\"reject\"}",
		  "rules.3" },
		{ "\"is\":[\"h265\"]", "\"is\":[\"x265\"]", "rules.1.is" },
		{ "\"is\":[\"h265\"]", "\"is\":[\"h265\",\"h265\"]", "rules.1.is" },
		{ "\"is\":[\"h265\"]", "\"is\":[]", "rules.1.is" },
		{ "\"weight\":25", "\"weight\":0", "rules.1.weight" },
		{ "\"weight\":25", "\"weight\":1001", "rules.1.weight" },
		{ "\"score\":false", "\"score\":false,\"min_score_gain\":5", "upgrades.min_score_gain" },
		{ "\"score\":false", "\"score\":true", "upgrades.min_score_gain" },
		{ "\"revisions\":true,", "", "upgrades.revisions" }
	};
	guint i;

	g_assert_true(axis_quality_profile_parse(text, -1, NULL, NULL));
	for (i = 0; i < G_N_ELEMENTS(edits); i++) {
		g_autoptr(GPtrArray) problems = axis_quality_problems_new();
		AxisQualityProfile out;
		AxisQualityProfile zero;
		gchar *at = strstr(text, edits[i].from);
		g_autofree gchar *edited = NULL;
		guint k;

		if (at == NULL)
			g_error("edit %u: '%s' is not in %s", i, edits[i].from, text);
		edited = g_strdup_printf("%.*s%s%s", (gint)(at - text), text, edits[i].to, at + strlen(edits[i].from));
		memset(&out, 0x5a, sizeof(out));
		if (axis_quality_profile_parse(edited, -1, &out, problems))
			g_error("edit %u was accepted: %s", i, edited);
		memset(&zero, 0, sizeof(zero));
		g_assert_true(memcmp(&out, &zero, sizeof(out)) == 0);
		g_assert_cmpuint(problems->len, >, 0);
		for (k = 0; k < problems->len; k++) {
			const AxisQualityProblem *p = g_ptr_array_index(problems, k);

			g_assert_nonnull(p->message);
			g_assert_cmpuint(strlen(p->message), >, 0);
		}
		if (axis_quality_problems_find(problems, edits[i].field) == NULL) {
			GString *all = g_string_new(NULL);

			for (k = 0; k < problems->len; k++) {
				const AxisQualityProblem *p = g_ptr_array_index(problems, k);

				g_string_append_printf(all, " [%s] %s", p->field, p->message);
			}
			g_error("edit %u: no problem about '%s':%s", i, edits[i].field, all->str);
		}
	}

	/* A newer version's definition says so, and its format is read. */
	{
		g_autoptr(GPtrArray) problems = axis_quality_problems_new();
		const gchar *newer = "{\"format\":2,\"qualities\":\"whatever a later version means\"}";

		g_assert_false(axis_quality_profile_parse(newer, -1, NULL, problems));
		g_assert_nonnull(strstr(axis_quality_problems_find(problems, "format"), "newer version"));
		g_assert_cmpuint(problems->len, ==, 1);
		g_assert_cmpint(axis_quality_profile_format_of(newer, -1), ==, 2);
		g_assert_cmpint(axis_quality_profile_format_of(text, -1), ==, 1);
		g_assert_cmpint(axis_quality_profile_format_of("[1]", -1), ==, 0);
	}
	/* Not JSON, too long, nothing. */
	{
		g_autofree gchar *longer = g_strnfill(AXIS_QUALITY_MAX_TEXT + 1, ' ');

		g_assert_false(axis_quality_profile_parse("{", -1, NULL, NULL));
		g_assert_false(axis_quality_profile_parse(longer, -1, NULL, NULL));
		g_assert_false(axis_quality_profile_parse(NULL, -1, NULL, NULL));
		g_assert_false(axis_quality_profile_parse("[]", -1, NULL, NULL));
	}
}

/* Member order, white space and the order of a rule's values do not change
 * what a profile is. */
static void
test_profile_canonical(void)
{
	AxisQualityProfile p;
	AxisQualityProfile q;
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *hand = NULL;
	g_autoptr(GString) sizes = g_string_new(NULL);
	guint i;

	for (i = 1; i <= AXIS_QUALITY_N_LADDER; i++)
		g_string_append_printf(sizes, "%s \"%s\" : [ 0 , %d ]", i > 1 ? "," : "",
			axis_quality_name((AxisQuality)(AXIS_QUALITY_N_LADDER + 1 - i)), 100 + (gint)i);
	hand = g_strdup_printf("{ \"upgrades\": {\"score\": true, \"min_score_gain\": 50, \"revisions\": false, "
		"\"allowed\": true},\n \"players\": 30, \"rules\": [ {\"then\": \"prefer\", \"weight\": 10, \"if\": \"group\", "
		"\"is\": [\"NTb\", \"flux\", \"CMRG\"]} ], \"qualities\": [[\"webrip_1080p\", \"webdl_1080p\"], "
		"[\"bluray_720p\"]], \"cutoff\": \"webdl_1080p\", \"format\": 1, \"unknown\": true, \"unwanted\": false, "
		"\"max_size_mb\": 9000, \"sizes\": {%s} }", sizes->str);
	g_assert_true(axis_quality_profile_parse(hand, -1, &p, NULL));
	/* A tier best first, the values sorted ignoring case. */
	g_assert_cmpint(p.allowed[0], ==, AXIS_QUALITY_WEBDL_1080P);
	g_assert_cmpint(p.allowed[1], ==, AXIS_QUALITY_WEBRIP_1080P);
	g_assert_cmpstr(p.rules[0].values[0], ==, "CMRG");
	g_assert_cmpstr(p.rules[0].values[1], ==, "flux");
	g_assert_cmpstr(p.rules[0].values[2], ==, "NTb");
	canonical = axis_quality_profile_canonical(&p);
	g_assert_true(g_str_has_prefix(canonical, "{\"cutoff\":\"webdl_1080p\",\"format\":1,\"max_size_mb\":9000,"
		"\"players\":30,\"qualities\":[[\"webdl_1080p\",\"webrip_1080p\"],[\"bluray_720p\"]],\"rules\":[{\"if\":"
		"\"group\",\"is\":[\"CMRG\",\"flux\",\"NTb\"],\"then\":\"prefer\",\"weight\":10}],\"sizes\":{"
		"\"bluray_1080p\":[0,"));
	g_assert_true(g_str_has_suffix(canonical, "\"unknown\":true,\"unwanted\":false,\"upgrades\":{\"allowed\":true,"
		"\"min_score_gain\":50,\"revisions\":false,\"score\":true}}"));
	g_assert_null(strchr(canonical, ' '));
	g_assert_true(axis_quality_profile_parse(canonical, -1, &q, NULL));
	g_assert_true(memcmp(&p, &q, sizeof(p)) == 0);
	g_assert_true(axis_quality_profile_equal(&p, &q));

	/* The writer never turns a broken profile into a valid one. */
	q = p;
	q.tier[0] = 1;
	g_free(canonical);
	canonical = axis_quality_profile_canonical(&q);
	g_assert_false(axis_quality_profile_parse(canonical, -1, NULL, NULL));
	q = p;
	q.cutoff = AXIS_QUALITY_UNKNOWN;
	g_free(canonical);
	canonical = axis_quality_profile_canonical(&q);
	g_assert_false(axis_quality_profile_parse(canonical, -1, NULL, NULL));
	q = p;
	q.rules[0].condition = (AxisQualityCondition)77;
	g_free(canonical);
	canonical = axis_quality_profile_canonical(&q);
	g_assert_false(axis_quality_profile_parse(canonical, -1, NULL, NULL));
}

/* ── Reasons ────────────────────────────────────────────────────── */

/* Each kind of rejection states its reason, in words with the numbers. */
static void
test_reasons_each(void)
{
	AxisQualityProfile space = builtin("space_saver");
	AxisQualityProfile balanced = builtin("balanced");
	AxisQualityProfile required = builtin("balanced");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	static const struct
	{
		const gchar *name;
		const gchar *code;
		const gchar *words;
	} safety[] = {
		{ "Some.Film.2020.1080p.BluRay.x264-GROUP-sample", "sample", "a sample" },
		{ "Some.Film.2020.PASSWORDED.1080p.BluRay.x264-GROUP", "password", "password-protected" },
		{ "Some.Film.2020.1080p.BluRay.x264-GROUP.rar", "archive", "archive" },
		{ "Some.Film.2020.1080p.WEB-DL.x264-GROUP.exe", "executable", "program" },
		{ "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6.mkv", "no_title", "hash-like" },
		{ "1080p.BluRay.x264-GROUP", "no_title", "no title" }
	};
	guint i;

	context_init(&ctx, &title, &balanced, 97);
	for (i = 0; i < G_N_ELEMENTS(safety); i++) {
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, safety[i].name, 5000000000);
		g_autofree gchar *all = reasons_of(v);

		if (!has_reason(v, safety[i].code) || strstr(reason_text(v, safety[i].code), safety[i].words) == NULL)
			g_error("%s: expected %s (%s), got %s", safety[i].name, safety[i].code, safety[i].words, all);
		g_assert_false(v->accepted);
		g_assert_true(v->safety);
	}

	/* The match. */
	{
		Cand cand;
		g_autoptr(AxisQualityVerdict) none = NULL;
		g_autoptr(AxisQualityVerdict) ambiguous = NULL;

		cand_init(&cand, "Some.Film.2020.1080p.BluRay.x264-GROUP", 8000000000, AXIS_DOWNLOAD_USENET, -1);
		cand.c.match = AXIS_RELEASE_MATCH_NONE;
		cand.c.match_reason = "the title is another";
		none = axis_quality_decide_one(&ctx, &cand.c);
		g_assert_true(has_reason(none, "no_match"));
		g_assert_nonnull(strstr(reason_text(none, "no_match"), "the title is another"));
		/* Scenario: Ambiguous match. */
		cand.c.match = AXIS_RELEASE_MATCH_AMBIGUOUS;
		cand.c.match_reason = NULL;
		ambiguous = axis_quality_decide_one(&ctx, &cand.c);
		g_assert_true(has_reason(ambiguous, "ambiguous"));
		g_assert_false(ambiguous->accepted);
		g_assert_true(ambiguous->safety);
		cand_clear(&cand);
	}

	/* Scenario: Not allowed. */
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, "Some.Film.2020.720p.WEB-DL.DDP5.1.H.264-GRP", 3000000000);

		g_assert_true(has_reason(v, "not_allowed"));
		g_assert_cmpstr(reason_text(v, "not_allowed"), ==, "WEB-DL 720p is not one of this profile's qualities");
		g_assert_false(v->safety);
	}
	/* Unknown quality. */
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, "Some.Film.2020.WEB-DL.x264-GRP", 3000000000);

		g_assert_true(has_reason(v, "unknown_quality"));
		g_assert_nonnull(strstr(reason_text(v, "unknown_quality"), "no resolution was found"));
	}
	/* Sizes, with the numbers (the example of the design). */
	{
		g_autoptr(AxisQualityVerdict) big = NULL;
		g_autoptr(AxisQualityVerdict) small = NULL;
		g_autoptr(AxisQualityVerdict) huge = NULL;

		ctx.profile = &space;
		big = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 3100000000);
		g_assert_cmpstr(reason_text(big, "too_large"), ==,
			"2.9\302\240GiB is above the 2.4\302\240GiB limit for a 97-minute film at WEB-DL 1080p");
		small = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 200000000);
		g_assert_cmpstr(reason_text(small, "too_small"), ==,
			"190.7\302\240MiB is below the 291\302\240MiB a 97-minute film at WEB-DL 1080p should be at least");
		ctx.profile = &balanced;
		huge = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-GRP", 26000000000);
		g_assert_cmpstr(reason_text(huge, "too_large"), ==,
			"24.2\302\240GiB is above the 18.9\302\240GiB limit for a 97-minute film at BluRay 1080p");
		g_assert_cmpstr(reason_text(huge, "over_limit"), ==,
			"24.2\302\240GiB is above this profile's 24\302\240GiB limit for any release");
	}
	/* Two sizes that differ never read the same. */
	{
		g_autoptr(AxisQualityVerdict) close = NULL;

		ctx.profile = &space;
		close = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 2566000000);
		g_assert_cmpstr(reason_text(close, "too_large"), ==,
			"2.39\302\240GiB is above the 2.37\302\240GiB limit for a 97-minute film at WEB-DL 1080p");
		ctx.profile = &balanced;
	}
	/* Scenario: Required field. */
	{
		g_autoptr(AxisQualityVerdict) v = NULL;
		g_autoptr(AxisQualityVerdict) none = NULL;
		g_autoptr(AxisQualityVerdict) ok = NULL;

		add_rule(&required, AXIS_QUALITY_IF_CODEC, AXIS_QUALITY_REQUIRE, 0, "h265");
		g_assert_true(axis_quality_profile_check(&required, NULL));
		ctx.profile = &required;
		v = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-GRP", 8000000000);
		g_assert_cmpstr(reason_text(v, "rule_require"), ==, "H.264 (x264) where the profile requires HEVC (x265)");
		none = decide_name(&ctx, "Some.Film.2020.1080p.BluRay-GRP", 8000000000);
		g_assert_cmpstr(reason_text(none, "rule_require"), ==, "no video codec stated where the profile requires "
			"HEVC (x265)");
		ok = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x265-GRP", 8000000000);
		g_assert_true(ok->accepted);
		ctx.profile = &balanced;
	}
	/* A rule that rejects. */
	{
		AxisQualityProfile p = builtin("balanced");
		g_autoptr(AxisQualityVerdict) v = NULL;

		add_rule(&p, AXIS_QUALITY_IF_GROUP, AXIS_QUALITY_REJECT, 0, "yify");
		ctx.profile = &p;
		v = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-YIFY", 2000000000);
		g_assert_cmpstr(reason_text(v, "rule_reject"), ==, "the group YIFY, which this profile rejects");
		ctx.profile = &balanced;
	}
	/* 3D: the built-ins reject it. */
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, "Lumenor.2009.3D.HSBS.1080p.BluRay.x264-GROUP", 9000000000);

		g_assert_cmpstr(reason_text(v, "rule_reject"), ==, "3D, which this profile rejects");
	}
	/* The language. */
	{
		static const gchar *const english[] = { "en", NULL };
		g_autoptr(AxisQualityVerdict) german = NULL;
		g_autoptr(AxisQualityVerdict) multi = NULL;
		g_autoptr(AxisQualityVerdict) unstated = NULL;

		title.languages = english;
		german = decide_name(&ctx, "Some.Film.2020.GERMAN.1080p.BluRay.x264-GRP", 9000000000);
		g_assert_cmpstr(reason_text(german, "language"), ==, "German, where English is wanted");
		multi = decide_name(&ctx, "Some.Film.2020.GERMAN.MULTi.1080p.BluRay.x264-GRP", 9000000000);
		g_assert_false(has_reason(multi, "language"));
		unstated = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-GRP", 9000000000);
		g_assert_false(has_reason(unstated, "language"));
		title.languages = NULL;
	}
	/* Retention. */
	{
		Cand cand;
		g_autoptr(AxisQualityVerdict) v = NULL;

		cand_init(&cand, "Some.Film.2020.1080p.BluRay.x264-GRP", 9000000000, AXIS_DOWNLOAD_USENET, -1);
		cand.c.published_us = ctx.now_us - (gint64)4000 * 86400 * G_USEC_PER_SEC;
		ctx.retention_days = 3000;
		v = axis_quality_decide_one(&ctx, &cand.c);
		g_assert_cmpstr(reason_text(v, "retention"), ==, "it was posted 4000 days ago, beyond the 3000 days the "
			"Usenet provider keeps");
		ctx.retention_days = 0;
		cand_clear(&cand);
	}
}

/* Scenario: Several reasons.  Too large and of a rejected codec: both are
 * stated; and a release with many faults states every one. */
static void
test_reasons_several(void)
{
	AxisQualityProfile p = builtin("space_saver");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	g_autoptr(AxisQualityVerdict) two = NULL;
	g_autoptr(AxisQualityVerdict) many = NULL;
	g_autofree gchar *all = NULL;
	Cand cand;

	add_rule(&p, AXIS_QUALITY_IF_CODEC, AXIS_QUALITY_REJECT, 0, "h264");
	context_init(&ctx, &title, &p, 97);
	two = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-GRP", 3100000000);
	g_assert_cmpuint(two->reasons->len, ==, 2);
	g_assert_true(has_reason(two, "too_large"));
	g_assert_cmpstr(reason_text(two, "rule_reject"), ==, "H.264 (x264), which this profile rejects");

	cand_init(&cand, "Some.Film.2024.CAM.x264-GRP-sample", 9000000000, AXIS_DOWNLOAD_USENET, -1);
	cand.c.match = AXIS_RELEASE_MATCH_AMBIGUOUS;
	many = axis_quality_decide_one(&ctx, &cand.c);
	all = reasons_of(many);
	g_test_message("%s", all);
	g_assert_true(has_reason(many, "sample"));
	g_assert_true(has_reason(many, "ambiguous"));
	g_assert_true(has_reason(many, "unwanted"));
	g_assert_true(has_reason(many, "not_allowed"));
	g_assert_true(has_reason(many, "over_limit"));
	g_assert_true(has_reason(many, "rule_reject"));
	/* The fixed rules first. */
	g_assert_cmpstr(((AxisQualityReason *)g_ptr_array_index(many->reasons, 0))->code, ==, "sample");
	g_assert_cmpint(((AxisQualityReason *)g_ptr_array_index(many->reasons, 0))->kind, ==, AXIS_QUALITY_REASON_SAFETY);
	cand_clear(&cand);
}

/* ── Fixed rules ────────────────────────────────────────────────── */

/* No profile relaxes the fixed rules; the Unwanted class only comes in
 * through its switch. */
static void
test_safety_fixed(void)
{
	static const gchar *const unsafe[] = {
		"Some.Film.2020.1080p.BluRay.x264-GROUP-sample",
		"Some.Film.2020.PASSWORDED.1080p.BluRay.x264-GROUP",
		"Some.Film.2020.1080p.BluRay.x264-GROUP.rar",
		"Some.Film.2020.1080p.WEB-DL.x264-GROUP.exe",
		"a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6.mkv",
		"Xq7Lm2Rt9Vb4Kp8Wz3Nc"
	};
	AxisQualityProfile profiles[AXIS_QUALITY_N_BUILTINS + 1];
	const AxisQualityBuiltin *b = axis_quality_builtins();
	guint i;
	guint k;

	for (i = 0; i < AXIS_QUALITY_N_BUILTINS; i++)
		profiles[i] = builtin(b[i].slug);
	profiles[AXIS_QUALITY_N_BUILTINS] = permissive();
	for (k = 0; k < G_N_ELEMENTS(profiles); k++) {
		AxisQualityContext ctx;
		AxisQualityTitle title;
		Cand cand;

		context_init(&ctx, &title, &profiles[k], 0);
		for (i = 0; i < G_N_ELEMENTS(unsafe); i++) {
			g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, unsafe[i], 1000000000);

			g_assert_false(v->accepted);
			g_assert_true(v->safety);
		}
		/* A good release that is not the title, or might not be. */
		cand_init(&cand, "Some.Film.2020.2160p.UHD.BluRay.REMUX.HDR.HEVC.TrueHD.7.1-GRP", 50000000000,
			AXIS_DOWNLOAD_TORRENT, 50);
		cand.c.match = AXIS_RELEASE_MATCH_NONE;
		{
			g_autoptr(AxisQualityVerdict) v = axis_quality_decide_one(&ctx, &cand.c);

			g_assert_false(v->accepted);
			g_assert_true(v->safety);
		}
		cand.c.match = AXIS_RELEASE_MATCH_AMBIGUOUS;
		{
			g_autoptr(AxisQualityVerdict) v = axis_quality_decide_one(&ctx, &cand.c);

			g_assert_false(v->accepted);
			g_assert_true(v->safety);
		}
		cand_clear(&cand);
	}

	/* Scenario: Cinema recording.  Every quality listed, the switch off: a
	 * cam is rejected.  With the switch on, accepted; on but not listed,
	 * rejected. */
	{
		AxisQualityProfile all = permissive();
		AxisQualityContext ctx;
		AxisQualityTitle title;
		g_autoptr(AxisQualityVerdict) off = NULL;
		g_autoptr(AxisQualityVerdict) on = NULL;
		g_autoptr(AxisQualityVerdict) unlisted = NULL;
		guint n = 0;

		all.allow_unwanted = FALSE;
		context_init(&ctx, &title, &all, 0);
		off = decide_name(&ctx, "Some.Film.2024.HDCAM.x264-GRP", 1000000000);
		g_assert_false(off->accepted);
		g_assert_cmpstr(reason_text(off, "unwanted"), ==, "Cam is a cinema recording or a pre-release copy, and this "
			"profile's switch for those is off");
		g_assert_false(off->safety);
		all.allow_unwanted = TRUE;
		on = decide_name(&ctx, "Some.Film.2024.HDCAM.x264-GRP", 1000000000);
		g_assert_true(on->accepted);
		/* Listed nowhere: the switch alone is not enough. */
		for (i = 0; i < all.n_allowed; i++) {
			if (!axis_quality_is_unwanted(all.allowed[i])) {
				all.allowed[n] = all.allowed[i];
				all.tier[n] = n;
				n++;
			}
		}
		all.n_allowed = n;
		g_assert_true(axis_quality_profile_check(&all, NULL));
		unlisted = decide_name(&ctx, "Some.Film.2024.HDCAM.x264-GRP", 1000000000);
		g_assert_false(unlisted->accepted);
		g_assert_true(has_reason(unlisted, "not_allowed"));
	}

	/* Trying a name with no title skips the match only, and says so. */
	{
		AxisQualityProfile all = permissive();
		AxisQualityContext ctx;
		AxisQualityTitle title;
		Cand cand;
		g_autoptr(AxisQualityVerdict) v = NULL;
		g_autoptr(AxisQualityVerdict) sample = NULL;

		context_init(&ctx, &title, &all, 0);
		ctx.no_title = TRUE;
		cand_init(&cand, "Some.Film.2020.1080p.BluRay.x264-GROUP", 8000000000, AXIS_DOWNLOAD_USENET, -1);
		cand.c.match = AXIS_RELEASE_MATCH_NONE;
		v = axis_quality_decide_one(&ctx, &cand.c);
		g_assert_true(v->accepted);
		g_assert_cmpuint(v->notes->len, >, 0);
		g_assert_nonnull(strstr(g_ptr_array_index(v->notes, 0), "Not matched against a title"));
		sample = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-GROUP-sample", 100000000);
		g_assert_false(sample->accepted);
		cand_clear(&cand);
	}

	/* An invalid profile accepts nothing. */
	{
		AxisQualityProfile broken = builtin("balanced");
		AxisQualityContext ctx;
		AxisQualityTitle title;
		g_autoptr(AxisQualityVerdict) v = NULL;

		broken.cutoff = AXIS_QUALITY_CAM;
		context_init(&ctx, &title, &broken, 97);
		v = decide_name(&ctx, "Some.Film.2020.1080p.BluRay.x264-GRP", 8000000000);
		g_assert_false(v->accepted);
		g_assert_true(has_reason(v, "profile_invalid"));
	}
}

/* ── Ranking ────────────────────────────────────────────────────── */

typedef struct
{
	const gchar          *name;
	gint64                size;
	AxisDownloadProtocol  protocol;
	gint                  seeders;
	gint                  age_days;
} Spec;

static AxisQualityDecision *
decide_specs(
	const AxisQualityContext *ctx,
	const Spec               *specs,
	guint                     n,
	Cand                     *cands
){
	AxisQualityCandidate list[16];
	guint i;

	g_assert_cmpuint(n, <=, G_N_ELEMENTS(list));
	for (i = 0; i < n; i++) {
		cand_init(&cands[i], specs[i].name, specs[i].size, specs[i].protocol, specs[i].seeders);
		if (specs[i].age_days >= 0)
			cands[i].c.published_us = ctx->now_us - (gint64)specs[i].age_days * 86400 * G_USEC_PER_SEC;
		list[i] = cands[i].c;
	}
	return axis_quality_decide(ctx, list, n);
}

static void
assert_order(
	const AxisQualityDecision *d,
	const guint               *expected,
	guint                      n
){
	guint i;

	g_assert_cmpuint(d->n_accepted, ==, n);
	for (i = 0; i < n; i++) {
		const AxisQualityVerdict *v = g_ptr_array_index(d->verdicts, d->order[i]);

		if (d->order[i] != expected[i])
			g_error("rank %u is release %u, expected %u", i + 1, d->order[i], expected[i]);
		g_assert_cmpuint(v->rank, ==, i + 1);
	}
}

/* Quality over score over revision over protocol over seeders and age over
 * size; stable for equals. */
static void
test_rank_order(void)
{
	AxisQualityProfile p = builtin("balanced");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	Cand cands[16];
	guint i;

	add_rule(&p, AXIS_QUALITY_IF_GROUP, AXIS_QUALITY_PREFER, 500, "GOOD");
	context_init(&ctx, &title, &p, 0);

	/* Scenario: Quality first.  A higher quality beats a higher score. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GOOD", 3000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_USENET, -1, -1 }
		};
		static const guint expected[] = { 1, 0 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, 2, cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, 2);
		g_assert_cmpint(((AxisQualityVerdict *)g_ptr_array_index(d->verdicts, 0))->score, >,
			((AxisQualityVerdict *)g_ptr_array_index(d->verdicts, 1))->score);
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 1), g_ptr_array_index(d->verdicts, 0));
		g_assert_cmpstr(why, ==, "higher in this profile's order: BluRay 1080p against WEB-DL 1080p");
		for (i = 0; i < 2; i++)
			cand_clear(&cands[i]);
	}
	/* Score over revision. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.REPACK.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.1080p.BluRay.x265-OTHER", 9000000000, AXIS_DOWNLOAD_USENET, -1, -1 }
		};
		static const guint expected[] = { 1, 0 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, 2, cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, 2);
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 1), g_ptr_array_index(d->verdicts, 0));
		g_assert_cmpstr(why, ==, "the same quality and a higher score: 25 against 0");
		for (i = 0; i < 2; i++)
			cand_clear(&cands[i]);
	}
	/* Revision over protocol. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_TORRENT, 500, -1 },
			{ "Some.Film.2020.PROPER.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_USENET, -1, 3 }
		};
		static const guint expected[] = { 1, 0 };
		g_autoptr(AxisQualityDecision) d = NULL;

		ctx.protocol = AXIS_QUALITY_PROTOCOL_TORRENT;
		d = decide_specs(&ctx, specs, 2, cands);
		assert_order(d, expected, 2);
		for (i = 0; i < 2; i++)
			cand_clear(&cands[i]);
	}
	/* Protocol over seeders, age and size. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 2000000000, AXIS_DOWNLOAD_USENET, -1, 0 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_TORRENT, 2, -1 }
		};
		static const guint expected[] = { 1, 0 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, 2, cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, 2);
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 1), g_ptr_array_index(d->verdicts, 0));
		g_assert_cmpstr(why, ==, "otherwise equal, and torrent is the protocol preferred");
		ctx.protocol = AXIS_QUALITY_PROTOCOL_ANY;
		for (i = 0; i < 2; i++)
			cand_clear(&cands[i]);
	}
	/* Seeders in bands over size; within a band, size; Usenet by age, with
	 * no protocol preferred: a protocol is compared with itself only. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 4000000000, AXIS_DOWNLOAD_TORRENT, 7, -1 },	/* band 2 */
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_TORRENT, 150, -1 },	/* band 4 */
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 8000000000, AXIS_DOWNLOAD_TORRENT, 101, -1 },	/* band 4 */
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 3000000000, AXIS_DOWNLOAD_TORRENT, 0, -1 },	/* band 0 */
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 3000000000, AXIS_DOWNLOAD_TORRENT, 19, -1 },	/* band 2 */
		};
		static const guint expected[] = { 2, 1, 4, 0, 3 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, G_N_ELEMENTS(specs), cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, G_N_ELEMENTS(specs));
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 1), g_ptr_array_index(d->verdicts, 0));
		g_assert_cmpstr(why, ==, "otherwise equal, and more seeders: 100 seeders or more against 5 to 19 seeders");
		g_free(why);
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 2), g_ptr_array_index(d->verdicts, 1));
		g_assert_cmpstr(why, ==, "otherwise equal, and smaller: 7.5\302\240GiB against 8.4\302\240GiB");
		for (i = 0; i < G_N_ELEMENTS(specs); i++)
			cand_clear(&cands[i]);
	}
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 4000000000, AXIS_DOWNLOAD_USENET, -1, 400 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 9000000000, AXIS_DOWNLOAD_USENET, -1, 2 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 1000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 8000000000, AXIS_DOWNLOAD_USENET, -1, 2 }
		};
		static const guint expected[] = { 3, 1, 0, 2 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, G_N_ELEMENTS(specs), cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, G_N_ELEMENTS(specs));
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 1), g_ptr_array_index(d->verdicts, 0));
		g_assert_cmpstr(why, ==, "otherwise equal, and younger: 2 days against 400");
		for (i = 0; i < G_N_ELEMENTS(specs); i++)
			cand_clear(&cands[i]);
	}
	/* Equals keep the order found; the rejected come after the accepted. */
	{
		static const Spec specs[] = {
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 5000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.720p.BluRay.x264-OTHER", 5000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 5000000000, AXIS_DOWNLOAD_USENET, -1, -1 },
			{ "Some.Film.2020.1080p.BluRay.x264-OTHER", 5000000000, AXIS_DOWNLOAD_USENET, -1, -1 }
		};
		static const guint expected[] = { 0, 2, 3 };
		g_autoptr(AxisQualityDecision) d = decide_specs(&ctx, specs, G_N_ELEMENTS(specs), cands);
		g_autofree gchar *why = NULL;

		assert_order(d, expected, 3);
		g_assert_cmpuint(d->order[3], ==, 1);
		g_assert_cmpuint(((AxisQualityVerdict *)g_ptr_array_index(d->verdicts, 1))->rank, ==, 0);
		why = axis_quality_explain_rank(&p, g_ptr_array_index(d->verdicts, 0), g_ptr_array_index(d->verdicts, 2));
		g_assert_cmpstr(why, ==, "equal on every key: kept in the order found");
		for (i = 0; i < G_N_ELEMENTS(specs); i++)
			cand_clear(&cands[i]);
	}
	/* The keys themselves. */
	{
		AxisQualityKeys a;
		AxisQualityKeys b;
		AxisQualityKey key;

		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		b.index = 1;
		g_assert_cmpint(axis_quality_keys_compare(&a, &b, &key), <, 0);
		g_assert_cmpint(key, ==, AXIS_QUALITY_KEY_NONE);
		b.size = 5;
		a.size = -1;
		g_assert_cmpint(axis_quality_keys_compare(&a, &b, &key), >, 0);
		g_assert_cmpint(key, ==, AXIS_QUALITY_KEY_SIZE);
		a.tier = 1;
		b.score = -1000;
		g_assert_cmpint(axis_quality_keys_compare(&a, &b, &key), >, 0);
		g_assert_cmpint(key, ==, AXIS_QUALITY_KEY_TIER);
	}
}

/* ── Upgrades ───────────────────────────────────────────────────── */

static AxisQualityVerdict *
against_disk(
	const AxisQualityProfile *profile,
	const AxisQualityOnDisk  *disk,
	const gchar              *name
){
	AxisQualityContext ctx;
	AxisQualityTitle title;

	context_init(&ctx, &title, profile, 0);
	ctx.disk = disk;
	return decide_name(&ctx, name, 8000000000);
}

static void
test_upgrades(void)
{
	AxisQualityProfile balanced = builtin("balanced");
	AxisQualityOnDisk disk;
	g_autoptr(AxisRelease) web_fields = parse("Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-GRP");
	g_autoptr(AxisRelease) web_proper = parse("Some.Film.2020.PROPER.1080p.WEB-DL.DDP5.1.H.264-GRP");

	memset(&disk, 0, sizeof(disk));

	/* Below the cutoff: a better quality is an upgrade. */
	disk.quality = AXIS_QUALITY_WEBDL_1080P;
	disk.fields = web_fields;
	{
		g_autoptr(AxisQualityVerdict) v = against_disk(&balanced, &disk, "Some.Film.2020.1080p.BluRay.x264-GRP");
		g_autoptr(AxisQualityVerdict) worse = against_disk(&balanced, &disk, "Some.Film.2020.1080p.HDTV.x264-GRP");

		g_assert_true(v->accepted);
		g_assert_true(v->upgrade);
		g_assert_cmpstr(v->upgrade_text, ==, "BluRay 1080p is above the WEB-DL 1080p on disk in this profile's order");
		g_assert_false(worse->accepted);
		g_assert_cmpstr(reason_text(worse, "not_better"), ==, "not better than the file on disk: HDTV 1080p is below "
			"its WEB-DL 1080p in this profile's order");
		g_assert_true(worse->acceptable);
	}
	/* Scenario: At the cutoff: nothing is an upgrade, not even a repack. */
	disk.quality = AXIS_QUALITY_BLURAY_1080P;
	{
		g_autoptr(AxisQualityVerdict) v = against_disk(&balanced, &disk, "Some.Film.2020.REPACK.1080p.BluRay.x264-GRP");
		AxisQualityContext ctx;
		AxisQualityTitle title;
		Cand cand;
		g_autoptr(AxisQualityDecision) d = NULL;

		g_assert_false(v->accepted);
		g_assert_cmpstr(reason_text(v, "cutoff"), ==, "the file on disk, BluRay 1080p, is at or above this profile's "
			"cutoff (BluRay 1080p): no upgrade is sought");
		context_init(&ctx, &title, &balanced, 0);
		ctx.disk = &disk;
		cand_init(&cand, "Some.Film.2020.1080p.BluRay.x264-GRP", 9000000000, AXIS_DOWNLOAD_USENET, -1);
		d = axis_quality_decide(&ctx, &cand.c, 1);
		g_assert_true(d->cutoff_reached);
		g_assert_cmpuint(d->n_accepted, ==, 0);
		cand_clear(&cand);
	}
	/* Above it on the ladder, and not one of the profile's qualities. */
	disk.quality = AXIS_QUALITY_REMUX_2160P;
	{
		g_autoptr(AxisQualityVerdict) v = against_disk(&balanced, &disk, "Some.Film.2020.1080p.BluRay.x264-GRP");

		g_assert_false(v->accepted);
		g_assert_nonnull(strstr(reason_text(v, "cutoff"), "not one of this profile's qualities"));
	}
	/* Below it, and not listed: compared on the ladder. */
	disk.quality = AXIS_QUALITY_HDTV_720P;
	{
		g_autoptr(AxisQualityVerdict) v = against_disk(&balanced, &disk, "Some.Film.2020.1080p.HDTV.x264-GRP");

		g_assert_true(v->accepted);
		g_assert_nonnull(strstr(v->upgrade_text, "in the standard order of qualities"));
	}

	/* The repack rule. */
	disk.quality = AXIS_QUALITY_WEBDL_1080P;
	disk.fields = web_fields;
	{
		AxisQualityProfile off = balanced;
		g_autoptr(AxisQualityVerdict) on = against_disk(&balanced, &disk,
			"Some.Film.2020.PROPER.1080p.WEB-DL.DDP5.1.H.264-GRP");
		g_autoptr(AxisQualityVerdict) same = against_disk(&balanced, &disk,
			"Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-OTHER");
		g_autoptr(AxisQualityVerdict) refused = NULL;
		g_autoptr(AxisQualityVerdict) again = NULL;
		/* WEBRip is WEB-DL's equal in Balanced: the same quality. */
		g_autoptr(AxisQualityVerdict) equal = against_disk(&balanced, &disk,
			"Some.Film.2020.REPACK.1080p.WEBRip.DDP5.1.H.264-GRP");

		g_assert_true(on->accepted);
		g_assert_cmpstr(on->upgrade_text, ==, "a proper or repack of the same quality, where the file on disk is the "
			"original");
		g_assert_true(equal->accepted);
		g_assert_false(same->accepted);
		g_assert_cmpstr(reason_text(same, "not_better"), ==, "not better than the file on disk (the same quality and "
			"the same score, 0)");
		off.upgrade_revisions = FALSE;
		refused = against_disk(&off, &disk, "Some.Film.2020.PROPER.1080p.WEB-DL.DDP5.1.H.264-GRP");
		g_assert_false(refused->accepted);
		/* A proper over a proper is not one. */
		disk.fields = web_proper;
		again = against_disk(&balanced, &disk, "Some.Film.2020.PROPER.1080p.WEB-DL.DDP5.1.H.264-GRP");
		g_assert_false(again->accepted);
		disk.fields = web_fields;
	}

	/* The same quality with a better score, by enough. */
	{
		AxisQualityProfile p = balanced;
		g_autoptr(AxisQualityVerdict) enough = NULL;
		g_autoptr(AxisQualityVerdict) short_of = NULL;
		g_autoptr(AxisQualityVerdict) lower = NULL;
		g_autoptr(AxisQualityVerdict) off = NULL;

		p.upgrade_score = TRUE;
		p.min_score_gain = 50;
		add_rule(&p, AXIS_QUALITY_IF_GROUP, AXIS_QUALITY_PREFER, 60, "GOOD");
		add_rule(&p, AXIS_QUALITY_IF_GROUP, AXIS_QUALITY_AVOID, 40, "BAD");
		g_assert_true(axis_quality_profile_check(&p, NULL));
		/* The file on disk scores 0; x265 is +25, GOOD +60. */
		enough = against_disk(&p, &disk, "Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-GOOD");
		g_assert_true(enough->accepted);
		g_assert_cmpstr(enough->upgrade_text, ==, "the same quality with a score of 60 against the file on disk's 0 "
			"(this profile upgrades for 50 more)");
		short_of = against_disk(&p, &disk, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-OTHER");
		g_assert_false(short_of->accepted);
		g_assert_cmpstr(reason_text(short_of, "not_better"), ==, "not better than the file on disk by enough (the same "
			"quality, a score of 25 against 0, where this profile upgrades for 50 more)");
		lower = against_disk(&p, &disk, "Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-BAD");
		g_assert_cmpstr(reason_text(lower, "not_better"), ==, "not better than the file on disk (the same quality, a "
			"lower score: -40 against 0)");
		p.upgrade_score = FALSE;
		p.min_score_gain = 0;
		off = against_disk(&p, &disk, "Some.Film.2020.1080p.WEB-DL.DDP5.1.H.264-GOOD");
		g_assert_false(off->accepted);
	}

	/* Upgrades off. */
	{
		AxisQualityProfile p = balanced;
		g_autoptr(AxisQualityVerdict) v = NULL;

		p.upgrades = FALSE;
		v = against_disk(&p, &disk, "Some.Film.2020.1080p.BluRay.x264-GRP");
		g_assert_false(v->accepted);
		g_assert_true(has_reason(v, "upgrades_off"));
	}

	/* Scenario: A file Axis converted.  Converted from a Remux, it counts as
	 * a Remux: a BluRay encode is not an upgrade, nor its own source
	 * quality; something better is. */
	{
		AxisQualityProfile best = builtin("best_1080p");
		/* Everything, upgraded up to a 2160p remux. */
		AxisQualityProfile wide = builtin("anything");
		g_autoptr(AxisRelease) hevc = parse("Some.Film.2020.1080p.BluRay.x265-AXIS");
		const gchar *remux_name = "Some.Film.2020.1080p.BluRay.REMUX.AVC.DTS-HD.MA.5.1-GRP";
		g_autoptr(AxisQualityVerdict) bluray = NULL;
		g_autoptr(AxisQualityVerdict) remux = NULL;
		g_autoptr(AxisQualityVerdict) at_cutoff = NULL;
		g_autoptr(AxisQualityVerdict) better = NULL;
		g_autoptr(AxisQualityVerdict) plain = NULL;

		wide.cutoff = AXIS_QUALITY_REMUX_2160P;
		g_assert_true(axis_quality_profile_check(&wide, NULL));
		memset(&disk, 0, sizeof(disk));
		/* What the file looks like now (an HEVC encode), and what it was. */
		disk.quality = AXIS_QUALITY_BLURAY_1080P;
		disk.fields = hevc;
		disk.converted = TRUE;
		disk.source_quality = AXIS_QUALITY_REMUX_1080P;
		bluray = against_disk(&wide, &disk, "Some.Film.2020.1080p.BluRay.x264-GRP");
		g_assert_false(bluray->accepted);
		g_assert_nonnull(strstr(reason_text(bluray, "not_better"), "below its Remux 1080p"));
		/* Not its own source quality either. */
		remux = against_disk(&wide, &disk, remux_name);
		g_assert_false(remux->accepted);
		g_assert_true(has_reason(remux, "not_better"));
		at_cutoff = against_disk(&best, &disk, remux_name);
		g_assert_false(at_cutoff->accepted);
		g_assert_true(has_reason(at_cutoff, "cutoff"));
		better = against_disk(&wide, &disk, "Some.Film.2020.2160p.WEB-DL.DDP5.1.HDR.H.265-GRP");
		g_assert_true(better->accepted);
		/* The same file, had Axis not converted it, would be replaced by the
		 * remux. */
		disk.converted = FALSE;
		plain = against_disk(&wide, &disk, remux_name);
		g_assert_true(plain->accepted);
	}
	/* Converted, of a source whose quality was not recorded: nothing is
	 * taken to be better. */
	{
		AxisQualityProfile anything = builtin("anything");
		g_autoptr(AxisQualityVerdict) v = NULL;

		memset(&disk, 0, sizeof(disk));
		disk.quality = AXIS_QUALITY_UNKNOWN;
		disk.converted = TRUE;
		disk.source_quality = AXIS_QUALITY_UNKNOWN;
		v = against_disk(&anything, &disk, "Some.Film.2020.2160p.BluRay.REMUX.HEVC.TrueHD.7.1-GRP");
		g_assert_false(v->accepted);
		g_assert_nonnull(strstr(reason_text(v, "cutoff"), "was not recorded"));
	}
	/* A file of unknown quality (not converted) is replaced by any
	 * acceptable known quality; an unknown release replaces nothing. */
	{
		AxisQualityProfile anything = builtin("anything");
		g_autoptr(AxisQualityVerdict) known = NULL;
		g_autoptr(AxisQualityVerdict) unknown = NULL;

		memset(&disk, 0, sizeof(disk));
		disk.quality = AXIS_QUALITY_UNKNOWN;
		known = against_disk(&anything, &disk, "Some.Film.2020.720p.HDTV.x264-GRP");
		g_assert_true(known->accepted);
		unknown = against_disk(&anything, &disk, "Some.Film.2020.x264-GRP");
		g_assert_false(unknown->accepted);
		g_assert_true(unknown->acceptable);
	}
}

/* ── Sizes ──────────────────────────────────────────────────────── */

static void
test_sizes(void)
{
	AxisQualityProfile p = builtin("space_saver");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	g_autoptr(AxisQualityVerdict) with = NULL;
	g_autoptr(AxisQualityVerdict) without = NULL;
	g_autoptr(AxisQualityVerdict) over = NULL;
	g_autoptr(AxisQualityVerdict) unknown_size = NULL;
	g_autoptr(AxisQualityVerdict) longer = NULL;
	Cand cand;

	/* With a runtime: per minute. */
	context_init(&ctx, &title, &p, 97);
	with = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 4000000000);
	g_assert_true(has_reason(with, "too_large"));
	/* A longer film may be larger. */
	title.runtime_minutes = 170;
	longer = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 4000000000);
	g_assert_true(longer->accepted);
	/* Without one: only the absolute limit (6 GiB), and it says so. */
	title.runtime_minutes = 0;
	without = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 4000000000);
	g_assert_true(without->accepted);
	g_assert_cmpuint(without->notes->len, ==, 1);
	g_assert_nonnull(strstr(g_ptr_array_index(without->notes, 0), "runtime is not known"));
	over = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6500000000);
	g_assert_false(over->accepted);
	g_assert_cmpstr(reason_text(over, "over_limit"), ==,
		"6.1\302\240GiB is above this profile's 6\302\240GiB limit for any release");
	g_assert_false(has_reason(over, "too_large"));
	/* An unknown size checks nothing, and says so. */
	cand_init(&cand, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", -1, AXIS_DOWNLOAD_USENET, -1);
	title.runtime_minutes = 97;
	unknown_size = axis_quality_decide_one(&ctx, &cand.c);
	g_assert_true(unknown_size->accepted);
	g_assert_nonnull(strstr(g_ptr_array_index(unknown_size->notes, 0), "size is not known"));
	cand_clear(&cand);
	/* Episodes say so. */
	{
		g_autoptr(AxisQualityVerdict) episode = NULL;

		title.noun = "episode";
		title.runtime_minutes = 45;
		episode = decide_name(&ctx, "Show.S01E01.1080p.WEB-DL.DDP5.1.x265-GRP", 2000000000);
		g_assert_cmpstr(reason_text(episode, "too_large"), ==, "1.9\302\240GiB is above the 1.1\302\240GiB limit for a "
			"45-minute episode at WEB-DL 1080p");
	}
	{
		g_autofree gchar *mb = axis_quality_format_size(850 * G_GINT64_CONSTANT(1048576));
		g_autofree gchar *small = axis_quality_format_size(4404019);
		g_autofree gchar *large = axis_quality_format_size(123 * G_GINT64_CONSTANT(1073741824));
		g_autofree gchar *whole = axis_quality_format_size(25 * G_GINT64_CONSTANT(1073741824));
		g_autofree gchar *none = axis_quality_format_size(-1);

		g_assert_cmpstr(mb, ==, "850\302\240MiB");
		g_assert_cmpstr(small, ==, "4.2\302\240MiB");
		g_assert_cmpstr(large, ==, "123\302\240GiB");
		g_assert_cmpstr(whole, ==, "25\302\240GiB");
		g_assert_cmpstr(none, ==, "an unknown size");
	}
}

/*
 * A profile saved before sizes were written in binary units (a built-in's
 * definition as the migration wrote it, the Space saver of that day) is read
 * as it is: its numbers are kept as stored, and they mean mebibytes now (a
 * limit of 6000 is 6000 MiB where it was 6000 decimal megabytes: 4.9% more).
 * What it decides is what its numbers say, in the units the sentences use,
 * and a release that is not within 4.9% of a limit is accepted or refused as
 * it always was.
 */
static void
test_stored_profile_keeps_its_numbers(void)
{
	static const gchar before[] =
		"{\"cutoff\":\"webdl_1080p\",\"format\":1,\"max_size_mb\":6000,\"players\":0,\"qualities\":[[\"webdl_1080p\","
		"\"webrip_1080p\"]],\"rules\":[{\"if\":\"codec\",\"is\":[\"h265\"],\"then\":\"prefer\",\"weight\":100},"
		"{\"if\":\"3d\",\"then\":\"reject\"},{\"if\":\"hardsub\",\"then\":\"avoid\",\"weight\":100}],\"sizes\":"
		"{\"bluray_1080p\":[6,200],\"bluray_2160p\":[20,500],\"bluray_480p\":[2,70],\"bluray_720p\":[4,100],"
		"\"cam\":[1,60],\"disc\":[20,1000],\"dvd\":[2,70],\"hdtv_1080p\":[4,130],\"hdtv_2160p\":[10,300],"
		"\"hdtv_720p\":[3,70],\"remux_1080p\":[60,400],\"remux_2160p\":[150,1000],\"screener\":[1,60],"
		"\"sdtv\":[1,40],\"telecine\":[1,60],\"telesync\":[1,60],\"web_480p\":[1,40],\"webdl_1080p\":[3,25],"
		"\"webdl_2160p\":[10,300],\"webdl_720p\":[3,70],\"webrip_1080p\":[3,25],\"webrip_2160p\":[10,300],"
		"\"webrip_720p\":[3,70],\"workprint\":[1,60]},\"unknown\":false,\"unwanted\":false,\"upgrades\":"
		"{\"allowed\":true,\"revisions\":true,\"score\":false}}";
	static const gint64 mib = G_GINT64_CONSTANT(1048576);
	AxisQualityProfile p;
	AxisQualityContext ctx;
	AxisQualityTitle title;
	g_autofree gchar *again = NULL;
	g_autoptr(AxisQualityVerdict) inside = NULL;
	g_autoptr(AxisQualityVerdict) above = NULL;
	g_autoptr(AxisQualityVerdict) at_limit = NULL;
	g_autoptr(AxisQualityVerdict) over_limit = NULL;
	g_autoptr(AxisQualityVerdict) band = NULL;
	g_autoptr(AxisQualityVerdict) far_over = NULL;
	g_autoptr(AxisQualityVerdict) worded = NULL;
	g_autoptr(AxisQualityVerdict) too_small = NULL;

	/* Read and written back as it was, numbers and all. */
	g_assert_true(axis_quality_profile_parse(before, -1, &p, NULL));
	again = axis_quality_profile_canonical(&p);
	g_assert_cmpstr(again, ==, before);
	g_assert_cmpint(p.max_size_mb, ==, 6000);
	g_assert_cmpint(p.sizes[AXIS_QUALITY_WEBDL_1080P].min, ==, 3);
	g_assert_cmpint(p.sizes[AXIS_QUALITY_WEBDL_1080P].max, ==, 25);

	context_init(&ctx, &title, &p, 100);
	/* Per minute: 100 minutes at 25 a minute is 2500 MiB. */
	inside = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 2400 * mib);
	g_assert_true(inside->accepted);
	above = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 2600 * mib);
	g_assert_false(above->accepted);
	g_assert_cmpstr(reason_text(above, "too_large"), ==,
		"2.5\302\240GiB is above the 2.4\302\240GiB limit for a 100-minute film at WEB-DL 1080p");
	too_small = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 299 * mib);
	g_assert_true(has_reason(too_small, "too_small"));

	/* The limit for any release: 6000 stored is 6000 MiB, to the byte. */
	title.runtime_minutes = 0;
	at_limit = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6000 * mib);
	g_assert_true(at_limit->accepted);
	over_limit = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6000 * mib + 1);
	g_assert_false(over_limit->accepted);
	g_assert_true(has_reason(over_limit, "over_limit"));
	worded = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6100 * mib);
	g_assert_cmpstr(reason_text(worded, "over_limit"), ==,
		"6\302\240GiB is above this profile's 5.9\302\240GiB limit for any release");
	/* Inside the 4.9% between the decimal and the binary reading it is the
	 * binary one that holds: 6.1 decimal GB was over, and is not now. */
	band = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6100000000);
	g_assert_true(band->accepted);
	/* Outside it, as always. */
	far_over = decide_name(&ctx, "Some.Film.2020.1080p.WEB-DL.DDP5.1.x265-GRP", 6400000000);
	g_assert_false(far_over->accepted);
}

/* ── Rules ──────────────────────────────────────────────────────── */

static gboolean
holds(
	AxisQualityCondition  condition,
	const gchar          *value,
	const gchar          *name
){
	AxisQualityRule rule;
	g_autoptr(AxisRelease) release = parse(name);

	memset(&rule, 0, sizeof(rule));
	rule.condition = condition;
	rule.effect = AXIS_QUALITY_REJECT;
	if (value != NULL) {
		g_strlcpy(rule.values[0], value, sizeof(rule.values[0]));
		rule.n_values = 1;
	}
	return axis_quality_rule_holds(&rule, release, name);
}

static void
test_rules_conditions(void)
{
	const gchar *dv = "Some.Film.2022.2160p.WEB-DL.DDP5.1.Atmos.DV.H.265-GRP";
	const gchar *dvhdr = "Some.Film.2022.2160p.WEB-DL.DDP5.1.Atmos.DV.HDR.H.265-GRP";
	const gchar *plain = "Some.Film.2022.1080p.BluRay.x264-GRP";
	guint c;

	g_assert_true(holds(AXIS_QUALITY_IF_CODEC, "h265", dv));
	g_assert_false(holds(AXIS_QUALITY_IF_CODEC, "h265", plain));
	g_assert_true(holds(AXIS_QUALITY_IF_HDR, "dv_only", dv));
	g_assert_false(holds(AXIS_QUALITY_IF_HDR, "dv_only", dvhdr));
	g_assert_true(holds(AXIS_QUALITY_IF_HDR, "any", dvhdr));
	g_assert_false(holds(AXIS_QUALITY_IF_HDR, "any", plain));
	g_assert_true(holds(AXIS_QUALITY_IF_AUDIO, "atmos", dv));
	g_assert_true(holds(AXIS_QUALITY_IF_AUDIO, "eac3", dv));
	g_assert_true(holds(AXIS_QUALITY_IF_CHANNELS, "5.1", dv));
	g_assert_false(holds(AXIS_QUALITY_IF_CHANNELS, "7.1", dv));
	g_assert_true(holds(AXIS_QUALITY_IF_GROUP, "grp", dv));
	g_assert_true(holds(AXIS_QUALITY_IF_BIT_DEPTH, "10", "Some.Film.2022.2160p.BluRay.x265.10bit-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_LANGUAGE, "de", "Some.Film.2022.GERMAN.1080p.BluRay.x264-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_LANGUAGE_MARK, "multi", "Some.Film.2022.MULTi.1080p.BluRay.x264-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_EDITION, "extended", "Some.Film.2022.Extended.1080p.BluRay.x264-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_SERVICE, "amazon", "Some.Film.2022.1080p.AMZN.WEB-DL.DDP5.1.H.264-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_3D, NULL, "Lumenor.2009.3D.HSBS.1080p.BluRay.x264-GROUP"));
	g_assert_false(holds(AXIS_QUALITY_IF_3D, NULL, plain));
	g_assert_true(holds(AXIS_QUALITY_IF_HARDSUB, NULL, "Some.Film.2022.1080p.HC.WEBRip.x264-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_REVISION, "repack", "Some.Film.2022.REPACK.1080p.BluRay.x264-GRP"));
	g_assert_false(holds(AXIS_QUALITY_IF_REVISION, "proper", "Some.Film.2022.REPACK.1080p.BluRay.x264-GRP"));
	/* A word of the name counts, the title's own words do not. */
	g_assert_true(holds(AXIS_QUALITY_IF_WORD, "upscaled", "Some.Film.2022.UPSCALED.2160p.WEB-DL.H.265-GRP"));
	g_assert_false(holds(AXIS_QUALITY_IF_WORD, "film", "Some.Film.2022.UPSCALED.2160p.WEB-DL.H.265-GRP"));
	g_assert_true(holds(AXIS_QUALITY_IF_WORD, "x264", plain));

	/* Every condition has a name, a label, and values it takes. */
	for (c = 0; c < AXIS_QUALITY_N_CONDITIONS; c++) {
		AxisQualityCondition back;
		const gchar *const *values = axis_quality_condition_values((AxisQualityCondition)c);
		guint i;

		g_assert_nonnull(axis_quality_condition_name((AxisQualityCondition)c));
		g_assert_true(axis_quality_condition_from_name(axis_quality_condition_name((AxisQualityCondition)c), &back));
		g_assert_cmpint(back, ==, c);
		for (i = 0; values != NULL && values[i] != NULL; i++) {
			g_assert_true(axis_quality_value_valid((AxisQualityCondition)c, values[i]));
			g_assert_nonnull(axis_quality_value_label((AxisQualityCondition)c, values[i]));
		}
	}
	g_assert_true(axis_quality_value_valid(AXIS_QUALITY_IF_GROUP, "D-Z0N3"));
	g_assert_false(axis_quality_value_valid(AXIS_QUALITY_IF_GROUP, "a b"));
	g_assert_false(axis_quality_value_valid(AXIS_QUALITY_IF_WORD, "web-dl"));
	g_assert_true(axis_quality_value_valid(AXIS_QUALITY_IF_CHANNELS, "7.1.4"));
	g_assert_false(axis_quality_value_valid(AXIS_QUALITY_IF_CHANNELS, "7..1"));
	g_assert_false(axis_quality_value_valid(AXIS_QUALITY_IF_LANGUAGE, "eng"));
	g_assert_false(axis_quality_value_valid(AXIS_QUALITY_IF_3D, "yes"));
	{
		AxisQualityProfile p = builtin("uhd");
		g_autofree gchar *first = axis_quality_rule_describe(&p.rules[0]);
		g_autofree gchar *second = axis_quality_rule_describe(&p.rules[1]);

		g_assert_cmpstr(first, ==, "Prefer HDR of any kind, by 50");
		g_assert_cmpstr(second, ==, "Avoid Dolby Vision without an HDR10 fallback, by 100");
	}
}

/* Prefer and avoid make the score, line by line. */
static void
test_rules_score(void)
{
	AxisQualityProfile p = builtin("uhd");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	g_autoptr(AxisQualityVerdict) dv = NULL;
	g_autoptr(AxisQualityVerdict) dvhdr = NULL;

	context_init(&ctx, &title, &p, 0);
	dv = decide_name(&ctx, "Some.Film.2022.2160p.WEB-DL.DDP5.1.Atmos.DV.H.265-GRP", 15000000000);
	g_assert_cmpint(dv->score, ==, -50);
	g_assert_cmpuint(dv->lines->len, ==, 2);
	g_assert_cmpstr(((AxisQualityScoreLine *)g_ptr_array_index(dv->lines, 0))->text, ==,
		"+50 for Dolby Vision, which this profile prefers");
	g_assert_cmpstr(((AxisQualityScoreLine *)g_ptr_array_index(dv->lines, 1))->text, ==,
		"-100 for Dolby Vision without an HDR10 fallback, which this profile avoids");
	dvhdr = decide_name(&ctx, "Some.Film.2022.2160p.WEB-DL.DDP5.1.Atmos.DV.HDR.H.265-GRP", 15000000000);
	g_assert_cmpint(dvhdr->score, ==, 50);
	g_assert_true(dv->accepted);
}

/* ── The players ────────────────────────────────────────────────── */

static AxisCompatVerdict
fixed_players(
	const AxisCompatCombo *want,
	gpointer               user_data
){
	/* The players of this test: HEVC fails, TrueHD unknown, the rest plays. */
	if (want->kind == AXIS_COMPAT_STREAM_VIDEO && g_strcmp0(want->codec, "hevc") == 0)
		return AXIS_VERDICT_RISKY;
	if (want->kind == AXIS_COMPAT_STREAM_AUDIO && g_strcmp0(want->codec, "truehd") == 0)
		return AXIS_VERDICT_UNKNOWN;
	return AXIS_VERDICT_SAFE;
}

static void
test_players(void)
{
	AxisQualityProfile p = builtin("balanced");
	AxisQualityContext ctx;
	AxisQualityTitle title;
	const gchar *hevc = "Some.Film.2022.1080p.BluRay.x265.TrueHD.7.1-GRP";
	const gchar *avc = "Some.Film.2022.1080p.BluRay.x264.DTS-HD.MA.5.1-GRP";

	/* A runtime, so that the sizes leave no note. */
	context_init(&ctx, &title, &p, 120);
	/* Off by default: the verdicts change nothing. */
	ctx.players = fixed_players;
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, hevc, 9000000000);

		g_assert_cmpint(v->score, ==, 25);
		g_assert_cmpuint(v->notes->len, ==, 0);
	}
	/* On: what a player would need transcoded is avoided, said in a line;
	 * what is not known is said in a note. */
	p.players = 80;
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, hevc, 9000000000);
		g_autoptr(AxisQualityVerdict) w = decide_name(&ctx, avc, 9000000000);
		const AxisQualityScoreLine *line;

		g_assert_cmpint(v->score, ==, 25 - 80);
		line = g_ptr_array_index(v->lines, v->lines->len - 1);
		g_assert_cmpint(line->points, ==, -80);
		g_assert_cmpstr(line->text, ==, "-80 because a player of the household is known to need its HEVC (x265) 1080p "
			"video transcoded (the compatibility report)");
		g_assert_nonnull(strstr(g_ptr_array_index(v->notes, 0), "TrueHD audio"));
		g_assert_true(v->accepted);
		g_assert_cmpint(w->score, ==, 0);
		g_assert_cmpuint(w->notes->len, ==, 0);
	}
	/* On without verdicts: nothing, and a note. */
	ctx.players = NULL;
	{
		g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, hevc, 9000000000);

		g_assert_cmpint(v->score, ==, 25);
		g_assert_nonnull(strstr(g_ptr_array_index(v->notes, 0), "no compatibility verdicts"));
	}

	/* The table of a household's players, from their evidence. */
	{
		AxisCompatObservation seen_direct[1];
		AxisCompatObservation seen_failing[1];
		AxisQualityDevice devices[2];
		AxisQualityPlayers players;
		AxisCompatCombo combos[4];
		g_autoptr(AxisRelease) release = parse(hevc);
		guint n;

		memset(seen_direct, 0, sizeof(seen_direct));
		memset(seen_failing, 0, sizeof(seen_failing));
		seen_direct[0].combo.kind = AXIS_COMPAT_STREAM_VIDEO;
		seen_direct[0].combo.codec = "hevc";
		seen_direct[0].combo.bit_depth = 10;
		seen_direct[0].combo.res_class = "2160p";
		seen_direct[0].combo.hdr = 1;
		seen_direct[0].effect = AXIS_EFFECT_DIRECT;
		seen_direct[0].plays = 3;
		seen_failing[0].combo.kind = AXIS_COMPAT_STREAM_VIDEO;
		seen_failing[0].combo.codec = "hevc";
		seen_failing[0].combo.hdr = -1;
		seen_failing[0].effect = AXIS_EFFECT_CONVERTED_CODEC;
		seen_failing[0].plays = 1;
		devices[0].obs = seen_direct;
		devices[0].n_obs = 1;
		devices[1].obs = seen_failing;
		devices[1].n_obs = 1;
		players.devices = devices;
		players.n_devices = 1;

		n = axis_quality_release_combos(release, combos, G_N_ELEMENTS(combos));
		g_assert_cmpuint(n, ==, 2);
		g_assert_cmpstr(combos[0].codec, ==, "hevc");
		g_assert_cmpstr(combos[0].res_class, ==, "1080p");
		g_assert_cmpint(combos[0].hdr, ==, -1);
		g_assert_cmpstr(combos[1].codec, ==, "truehd");
		/* One device that played 10-bit 2160p HDR HEVC directly: safe. */
		g_assert_cmpint(axis_quality_players_verdict(&combos[0], &players), ==, AXIS_VERDICT_SAFE);
		/* Its TrueHD was never seen: unknown. */
		g_assert_cmpint(axis_quality_players_verdict(&combos[1], &players), ==, AXIS_VERDICT_UNKNOWN);
		/* Another device that needed HEVC converted: the worst wins. */
		players.n_devices = 2;
		g_assert_cmpint(axis_quality_players_verdict(&combos[0], &players), ==, AXIS_VERDICT_RISKY);
		players.n_devices = 0;
		g_assert_cmpint(axis_quality_players_verdict(&combos[0], &players), ==, AXIS_VERDICT_UNKNOWN);

		/* Through the decision. */
		players.n_devices = 2;
		ctx.players = axis_quality_players_verdict;
		ctx.players_data = &players;
		{
			g_autoptr(AxisQualityVerdict) v = decide_name(&ctx, hevc, 9000000000);

			g_assert_cmpint(v->score, ==, 25 - 80);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/quality/ladder/order", test_ladder_order);
	g_test_add_func("/quality/ladder/placement", test_ladder_placement);
	g_test_add_func("/quality/ladder/unknown", test_ladder_unknown);
	g_test_add_func("/quality/ladder/corpus", test_ladder_corpus);
	g_test_add_func("/quality/profile/builtins", test_profile_builtins);
	g_test_add_func("/quality/profile/strict", test_profile_strict);
	g_test_add_func("/quality/profile/canonical", test_profile_canonical);
	g_test_add_func("/quality/reasons/each", test_reasons_each);
	g_test_add_func("/quality/reasons/several", test_reasons_several);
	g_test_add_func("/quality/safety/fixed", test_safety_fixed);
	g_test_add_func("/quality/rank/order", test_rank_order);
	g_test_add_func("/quality/upgrades", test_upgrades);
	g_test_add_func("/quality/sizes", test_sizes);
	g_test_add_func("/quality/sizes/stored-profile", test_stored_profile_keeps_its_numbers);
	g_test_add_func("/quality/rules/conditions", test_rules_conditions);
	g_test_add_func("/quality/rules/score", test_rules_score);
	g_test_add_func("/quality/players", test_players);

	return g_test_run();
}
