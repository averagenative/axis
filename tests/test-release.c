/*
 * test-release.c - reading release names, and matching them to titles
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The corpora in tests/fixtures/release are read field by field: every
 * field a line labels must be read as labelled, and every field it does
 * not label must be reported not found (nothing inferred), including the
 * words the parser did not understand.  films.json is the 99-name corpus
 * carried over from the earlier project, relabelled for the new fields;
 * episodes.json is new; regressions.json holds the bugs found by the audit
 * of real names in the earlier project and every one found since, each with
 * its reason.
 *
 * Spec scenarios (release-parsing):
 *   A film; An episode; Several episodes and season packs; An ordinary word
 *   that is also a quality word; A title that is a year; No resolution in
 *   the name; Cinema recording; A post's subject (regressions.json);
 *   Hostile input; Another name; Two candidates; Same-named titles of other
 *   years; A same-named title with no year, a year off
 *
 * The parser and the matcher are fuzzed (tests/fuzz/fuzz-release-parse.c,
 * tests/fuzz/fuzz-release-match.c).
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "release/axis-release.h"

#define DIR "tests/fixtures/release/"

/* Years are believed up to this one, so that the corpora read the same
 * whatever the date. */
#define LATEST_YEAR 2027

static AxisRelease *
parse(const gchar *name)
{
	return axis_release_parse_full(name, -1, LATEST_YEAR);
}

/* ── Corpora ────────────────────────────────────────────────────── */

typedef const gchar *(*FlagName)(guint flag);

static const gchar *const label_keys[] = {
	"name", "title", "year", "season", "episodes", "episode_range", "absolute", "date", "complete",
	"version", "resolution", "interlaced", "source", "service", "codec", "bits", "hdr", "audio",
	"atmos", "channels", "editions", "languages", "lang_marks", "revision", "marks", "stop", "group",
	"checksum", "unknown", "note", "reason",
	NULL
};

static void
mismatch(
	GString     *errors,
	const gchar *name,
	const gchar *field,
	const gchar *expected,
	const gchar *got
){
	g_string_append_printf(errors, "  '%s': %s: expected %s, got %s\n", name, field, expected, got);
}

static const gchar *
or_none(const gchar *text)
{
	return text != NULL ? text : "none";
}

static void
check_string(
	GString     *errors,
	const gchar *name,
	JsonObject  *labels,
	const gchar *field,
	const gchar *got
){
	const gchar *expected = json_object_has_member(labels, field) ?
		json_object_get_string_member(labels, field) : NULL;

	if (g_strcmp0(expected, got) != 0)
		mismatch(errors, name, field, or_none(expected), or_none(got));
}

static void
check_int(
	GString     *errors,
	const gchar *name,
	JsonObject  *labels,
	const gchar *field,
	gint64       none,
	gint64       got
){
	gint64 expected = json_object_has_member(labels, field) ? json_object_get_int_member(labels, field) : none;

	if (expected != got) {
		g_autofree gchar *e = expected == none ? g_strdup("none") : g_strdup_printf("%" G_GINT64_FORMAT, expected);
		g_autofree gchar *g = got == none ? g_strdup("none") : g_strdup_printf("%" G_GINT64_FORMAT, got);

		mismatch(errors, name, field, e, g);
	}
}

static void
check_bool(
	GString     *errors,
	const gchar *name,
	JsonObject  *labels,
	const gchar *field,
	gboolean     got
){
	gboolean expected = json_object_has_member(labels, field) && json_object_get_boolean_member(labels, field);

	if (expected != got)
		mismatch(errors, name, field, expected ? "true" : "false", got ? "true" : "false");
}

static gchar *
numbers_text(
	const gint *numbers,
	guint       count
){
	GString *out = g_string_new("[");
	guint i;

	for (i = 0; i < count; i++)
		g_string_append_printf(out, "%s%d", i > 0 ? "," : "", numbers[i]);
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

static void
check_numbers(
	GString     *errors,
	const gchar *name,
	JsonObject  *labels,
	const gchar *field,
	const gchar *range_field,
	const gint  *got,
	guint        count
){
	GArray *expected = g_array_new(FALSE, FALSE, sizeof(gint));
	gboolean equal;
	guint i;

	if (json_object_has_member(labels, field)) {
		JsonArray *list = json_object_get_array_member(labels, field);

		for (i = 0; i < json_array_get_length(list); i++) {
			gint v = (gint)json_array_get_int_element(list, i);

			g_array_append_val(expected, v);
		}
	}
	if (range_field != NULL && json_object_has_member(labels, range_field)) {
		JsonArray *range = json_object_get_array_member(labels, range_field);
		gint v;

		for (v = (gint)json_array_get_int_element(range, 0); v <= (gint)json_array_get_int_element(range, 1); v++)
			g_array_append_val(expected, v);
	}
	equal = expected->len == count;
	for (i = 0; equal && i < count; i++)
		equal = g_array_index(expected, gint, i) == got[i];
	if (!equal) {
		g_autofree gchar *e = numbers_text((const gint *)(gpointer)expected->data, expected->len);
		g_autofree gchar *g = numbers_text(got, count);

		mismatch(errors, name, field, e, g);
	}
	g_array_unref(expected);
}

/* A list of flag names as the flags they are. */
static guint
flags_of(
	JsonObject  *labels,
	const gchar *field,
	FlagName     flag_name,
	const gchar *name
){
	JsonArray *list;
	guint flags = 0;
	guint i;

	if (!json_object_has_member(labels, field))
		return 0;
	list = json_object_get_array_member(labels, field);
	for (i = 0; i < json_array_get_length(list); i++) {
		const gchar *label = json_array_get_string_element(list, i);
		guint bit;
		gboolean known = FALSE;

		for (bit = 0; bit < 32; bit++) {
			if (g_strcmp0(flag_name(1u << bit), label) == 0) {
				flags |= 1u << bit;
				known = TRUE;
			}
		}
		if (!known)
			g_error("'%s': %s: no such value '%s'", name, field, label);
	}
	return flags;
}

static gchar *
flags_text(
	guint    flags,
	FlagName flag_name
){
	GString *out = g_string_new("[");
	guint bit;
	gboolean first = TRUE;

	for (bit = 0; bit < 32; bit++) {
		if (flags & (1u << bit)) {
			g_string_append_printf(out, "%s%s", first ? "" : ",", or_none(flag_name(1u << bit)));
			first = FALSE;
		}
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

static void
check_flags(
	GString     *errors,
	const gchar *name,
	JsonObject  *labels,
	const gchar *field,
	FlagName     flag_name,
	guint        got
){
	guint expected = flags_of(labels, field, flag_name, name);

	if (expected != got) {
		g_autofree gchar *e = flags_text(expected, flag_name);
		g_autofree gchar *g = flags_text(got, flag_name);

		mismatch(errors, name, field, e, g);
	}
}

static void
check_strings(
	GString             *errors,
	const gchar         *name,
	JsonObject          *labels,
	const gchar         *field,
	const gchar *const  *got,
	guint                count
){
	GString *e = g_string_new("[");
	GString *g = g_string_new("[");
	guint i;

	if (json_object_has_member(labels, field)) {
		JsonArray *list = json_object_get_array_member(labels, field);

		for (i = 0; i < json_array_get_length(list); i++)
			g_string_append_printf(e, "%s'%s'", i > 0 ? "," : "", json_array_get_string_element(list, i));
	}
	for (i = 0; i < count; i++)
		g_string_append_printf(g, "%s'%s'", i > 0 ? "," : "", got[i]);
	g_string_append_c(e, ']');
	g_string_append_c(g, ']');
	if (strcmp(e->str, g->str) != 0)
		mismatch(errors, name, field, e->str, g->str);
	g_string_free(e, TRUE);
	g_string_free(g, TRUE);
}

/* Checks one labelled line; returns how many fields it checked. */
static guint
check_line(
	GString    *errors,
	JsonObject *labels
){
	const gchar *name = json_object_get_string_member(labels, "name");
	g_autoptr(AxisRelease) r = parse(name);
	g_autoptr(GList) members = json_object_get_members(labels);
	gsize before = errors->len;
	gint expected_first = -1;
	gint expected_last = -1;
	GList *l;

	for (l = members; l != NULL; l = l->next) {
		if (!g_strv_contains(label_keys, l->data))
			g_error("'%s': unknown label '%s'", name, (const gchar *)l->data);
	}

	check_string(errors, name, labels, "title", r->title);
	check_int(errors, name, labels, "year", 0, r->year);

	if (json_object_has_member(labels, "season")) {
		JsonNode *node = json_object_get_member(labels, "season");

		if (JSON_NODE_HOLDS_ARRAY(node)) {
			expected_first = (gint)json_array_get_int_element(json_node_get_array(node), 0);
			expected_last = (gint)json_array_get_int_element(json_node_get_array(node), 1);
		} else {
			expected_first = (gint)json_node_get_int(node);
			expected_last = expected_first;
		}
	}
	if (expected_first != r->season_first || expected_last != r->season_last) {
		g_autofree gchar *e = g_strdup_printf("%d-%d", expected_first, expected_last);
		g_autofree gchar *g = g_strdup_printf("%d-%d", r->season_first, r->season_last);

		mismatch(errors, name, "season", e, g);
	}
	check_numbers(errors, name, labels, "episodes", "episode_range", r->episodes, r->episode_count);
	check_numbers(errors, name, labels, "absolute", NULL, r->absolute, r->absolute_count);
	{
		g_autofree gchar *date = r->air_year > 0 ?
			g_strdup_printf("%04d-%02d-%02d", r->air_year, r->air_month, r->air_day) : NULL;

		check_string(errors, name, labels, "date", date);
	}
	check_bool(errors, name, labels, "complete", r->complete);
	check_int(errors, name, labels, "version", 0, r->version);
	check_string(errors, name, labels, "resolution", axis_release_resolution_name(r->resolution));
	check_bool(errors, name, labels, "interlaced", r->interlaced);
	check_string(errors, name, labels, "source", axis_release_source_name(r->source));
	check_string(errors, name, labels, "service", r->service);
	check_string(errors, name, labels, "codec", axis_release_codec_name(r->codec));
	check_int(errors, name, labels, "bits", 0, r->bit_depth);
	check_flags(errors, name, labels, "hdr", axis_release_hdr_name, r->hdr);
	check_flags(errors, name, labels, "audio", axis_release_audio_name, r->audio);
	check_bool(errors, name, labels, "atmos", r->atmos);
	check_string(errors, name, labels, "channels", r->channels[0] != '\0' ? r->channels : NULL);
	check_flags(errors, name, labels, "editions", axis_release_edition_name, r->editions);
	check_strings(errors, name, labels, "languages", r->languages, r->language_count);
	check_flags(errors, name, labels, "lang_marks", axis_release_lang_mark_name, r->language_marks);
	check_flags(errors, name, labels, "revision", axis_release_revision_name, r->revision);
	check_flags(errors, name, labels, "marks", axis_release_mark_name, r->marks);
	check_flags(errors, name, labels, "stop", axis_release_stop_name, r->stop);
	check_string(errors, name, labels, "group", r->group);
	check_string(errors, name, labels, "checksum", r->checksum);
	check_strings(errors, name, labels, "unknown", (const gchar *const *)r->unknown->pdata, r->unknown->len);

	if (errors->len > before) {
		g_autofree gchar *line = axis_release_describe(r);

		g_string_append_printf(errors, "    read as: %s\n", line);
	}
	/* Every field of the structure is checked for every line. */
	return 28;
}

static void
check_corpus(
	const gchar *file,
	guint        at_least
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_strconcat(DIR, file, NULL);
	GString *errors = g_string_new(NULL);
	JsonArray *lines;
	guint failed = 0;
	guint fields = 0;
	guint i;

	if (!json_parser_load_from_file(parser, path, &error))
		g_error("cannot read %s: %s", path, error->message);
	lines = json_node_get_array(json_parser_get_root(parser));
	g_assert_cmpuint(json_array_get_length(lines), >=, at_least);
	for (i = 0; i < json_array_get_length(lines); i++) {
		gsize before = errors->len;

		fields += check_line(errors, json_array_get_object_element(lines, i));
		if (errors->len > before)
			failed++;
	}
	g_test_message("%s: %u names, %u fields checked, %u names wrong", file, json_array_get_length(lines),
		fields, failed);
	if (failed > 0)
		g_error("%s: %u of %u names are not read as labelled:\n%s", file, failed,
			json_array_get_length(lines), errors->str);
	g_string_free(errors, TRUE);
}

/* Spec: A film; An episode; ... (and the carried-over corpus). */
static void
test_films(void)
{
	check_corpus("films.json", 99);
}

static void
test_episodes(void)
{
	check_corpus("episodes.json", 150);
}

static void
test_regressions(void)
{
	check_corpus("regressions.json", 1);
}

/* The checker itself: a wrong label, a field found but not labelled, and
 * a word not understood but not labelled are each caught. */
static void
test_checker(void)
{
	static const gchar *const lines[] = {
		"{\"name\":\"Film.2020.1080p.x264-GRP\",\"title\":\"Film\",\"year\":2020,\"resolution\":\"1080p\","
			"\"codec\":\"h264\",\"group\":\"GRP\"}",
		"{\"name\":\"Film.2020.1080p.x264-GRP\",\"title\":\"Film\",\"year\":2021,\"resolution\":\"1080p\","
			"\"codec\":\"h264\",\"group\":\"GRP\"}",
		"{\"name\":\"Film.2020.1080p.x264-GRP\",\"title\":\"Film\",\"year\":2020,\"codec\":\"h264\","
			"\"group\":\"GRP\"}",
		"{\"name\":\"Film.2020.Blarg.1080p.x264-GRP\",\"title\":\"Film\",\"year\":2020,\"resolution\":\"1080p\","
			"\"codec\":\"h264\",\"group\":\"GRP\"}",
		"{\"name\":\"Show.S01E01E02.720p\",\"title\":\"Show\",\"season\":1,\"episodes\":[1],\"resolution\":\"720p\"}",
		"{\"name\":\"Show.S01E01E02.720p\",\"title\":\"Show\",\"season\":1,\"episode_range\":[1,2],"
			"\"resolution\":\"720p\"}",
	};
	static const gboolean wrong[] = { FALSE, TRUE, TRUE, TRUE, TRUE, FALSE };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(lines); i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		GString *errors = g_string_new(NULL);

		g_assert_true(json_parser_load_from_data(parser, lines[i], -1, NULL));
		check_line(errors, json_node_get_object(json_parser_get_root(parser)));
		if ((errors->len > 0) != wrong[i])
			g_error("line %u: expected %s, got '%s'", i, wrong[i] ? "a mismatch" : "none", errors->str);
		g_string_free(errors, TRUE);
	}
}

/* ── The spec's scenarios, by name ──────────────────────────────── */

static void
test_scenarios(void)
{
	{
		g_autoptr(AxisRelease) r = parse("Mesa.Part.Two.2024.2160p.UHD.BluRay.x265.10bit.HDR.DTS-HD.MA.5.1-GROUP");

		g_assert_cmpstr(r->title, ==, "Mesa Part Two");
		g_assert_cmpint(r->year, ==, 2024);
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_2160P);
		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_BLURAY);
		g_assert_cmpint(r->codec, ==, AXIS_RELEASE_CODEC_H265);
		g_assert_cmpstr(r->group, ==, "GROUP");
	}
	{
		g_autoptr(AxisRelease) r = parse("Show.Name.S02E05.1080p.WEB-DL.DDP5.1.H.264-GROUP");

		g_assert_cmpstr(r->title, ==, "Show Name");
		g_assert_cmpint(r->season_first, ==, 2);
		g_assert_cmpuint(r->episode_count, ==, 1);
		g_assert_cmpint(r->episodes[0], ==, 5);
		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_WEBDL);
		g_assert_true(axis_release_is_episode(r));
	}
	{
		g_autoptr(AxisRelease) several = parse("Show.Name.S01E02E03.720p.HDTV.x264-GROUP");
		g_autoptr(AxisRelease) pack = parse("Show.Name.S01.720p.HDTV.x264-GROUP");

		g_assert_cmpint(several->season_first, ==, 1);
		g_assert_cmpuint(several->episode_count, ==, 2);
		g_assert_cmpint(several->episodes[0], ==, 2);
		g_assert_cmpint(several->episodes[1], ==, 3);
		g_assert_cmpint(pack->season_first, ==, 1);
		g_assert_cmpint(pack->season_last, ==, 1);
		g_assert_cmpuint(pack->episode_count, ==, 0);
	}
	{
		g_autoptr(AxisRelease) r = parse("Auntie.Web.2024.1080p.BluRay.x264-GROUP");

		g_assert_cmpstr(r->title, ==, "Auntie Web");
		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_BLURAY);
	}
	{
		g_autoptr(AxisRelease) r = parse("1936.2019.1080p.BluRay.x264-GROUP");

		g_assert_cmpstr(r->title, ==, "1936");
		g_assert_cmpint(r->year, ==, 2019);
	}
	/* Spec: No resolution in the name. */
	{
		g_autoptr(AxisRelease) r = parse("Some.Film.2020.BluRay.x264-GROUP");

		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_BLURAY);
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_NONE);
	}
	/* Spec: Cinema recording. */
	{
		g_autoptr(AxisRelease) cam = parse("Some.Film.2020.CAM.x264-GROUP");
		g_autoptr(AxisRelease) ts = parse("Some.Film.2020.TS.XviD-GROUP");
		g_autoptr(AxisRelease) both = parse("Some.Film.2020.1080p.WEB.HDCAM.x264-GROUP");

		g_assert_cmpint(cam->source, ==, AXIS_RELEASE_SOURCE_CAM);
		g_assert_cmpint(ts->source, ==, AXIS_RELEASE_SOURCE_TELESYNC);
		/* A recording mark always shows, whatever else the name says. */
		g_assert_cmpint(both->source, ==, AXIS_RELEASE_SOURCE_CAM);
	}
}

/* ── Hostile input ──────────────────────────────────────────────── */

static void
assert_nothing(const AxisRelease *r)
{
	g_assert_null(r->title);
	g_assert_cmpint(r->year, ==, 0);
	g_assert_cmpint(r->season_first, ==, -1);
	g_assert_cmpint(r->season_last, ==, -1);
	g_assert_cmpuint(r->episode_count, ==, 0);
	g_assert_cmpuint(r->absolute_count, ==, 0);
	g_assert_cmpint(r->air_year, ==, 0);
	g_assert_false(r->complete);
	g_assert_cmpint(r->version, ==, 0);
	g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_NONE);
	g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_NONE);
	g_assert_null(r->service);
	g_assert_cmpint(r->codec, ==, AXIS_RELEASE_CODEC_NONE);
	g_assert_cmpint(r->bit_depth, ==, 0);
	g_assert_cmpuint(r->hdr | r->audio | r->editions | r->language_marks | r->revision | r->marks | r->stop, ==, 0);
	g_assert_false(r->atmos);
	g_assert_cmpstr(r->channels, ==, "");
	g_assert_cmpuint(r->language_count, ==, 0);
	g_assert_null(r->group);
	g_assert_null(r->checksum);
	g_assert_nonnull(r->unknown);
	g_assert_cmpuint(r->unknown->len, ==, 0);
	g_assert_false(axis_release_is_episode(r));
}

/* Spec: Hostile input. */
static void
test_hostile(void)
{
	static const gchar *const invalid[] = {
		"\xff\xfe",
		"Movie.2020.1080p.BluRay\xc3",
		"Movie.\xc0\xaf.2020.1080p",
		"Movie.\xed\xa0\x80.2020.1080p",
		"Movie.\xf4\x90\x80\x80.2020.1080p",
		"",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(invalid); i++) {
		g_autoptr(AxisRelease) r = parse(invalid[i]);

		assert_nothing(r);
	}
	{
		g_autoptr(AxisRelease) r = parse(NULL);
		g_autoptr(AxisRelease) now = axis_release_parse(NULL);

		assert_nothing(r);
		assert_nothing(now);
	}
	/* A NUL inside the bytes given. */
	{
		static const gchar bytes[] = "Movie.2020\0.1080p.BluRay.x264-GROUP";
		g_autoptr(AxisRelease) r = axis_release_parse_full(bytes, sizeof(bytes) - 1, LATEST_YEAR);
		g_autoptr(AxisRelease) prefix = axis_release_parse_full(bytes, 10, LATEST_YEAR);

		assert_nothing(r);
		g_assert_cmpstr(prefix->title, ==, "Movie");
		g_assert_cmpint(prefix->year, ==, 2020);
	}
	/* The limit: 512 bytes are read, 513 are not. */
	{
		GString *name = g_string_new("Some.Film.2020.1080p.BluRay.x264-GROUP");
		AxisRelease *r;

		while (name->len < AXIS_RELEASE_NAME_MAX)
			g_string_prepend_c(name, 'a');
		g_assert_cmpuint(name->len, ==, AXIS_RELEASE_NAME_MAX);
		r = parse(name->str);
		g_assert_nonnull(r->title);
		g_assert_cmpint(r->year, ==, 2020);
		axis_release_free(r);
		g_string_prepend_c(name, 'a');
		r = parse(name->str);
		assert_nothing(r);
		axis_release_free(r);
		g_string_free(name, TRUE);
	}
	/* Control and invisible formatting characters never reach a title. */
	{
		g_autoptr(AxisRelease) r = parse("Some\tFilm\xe2\x80\xae.Name\xe2\x80\x8b.2020.1080p");

		g_assert_cmpstr(r->title, ==, "Some Film Name");
		g_assert_cmpint(r->year, ==, 2020);
	}
	/* Nothing but separators, or nothing but a hash. */
	{
		g_autoptr(AxisRelease) dots = parse("....----____    ");
		g_autoptr(AxisRelease) hash = parse("550e8400-e29b-41d4-a716-446655440000");

		g_assert_null(dots->title);
		g_assert_null(hash->title);
		g_assert_cmpuint(hash->stop, ==, AXIS_RELEASE_STOP_OBFUSCATED);
	}
	axis_release_free(NULL);
}

/* The worst names at the limit still read quickly (the fuzz target holds
 * every input to a bound; this is the same promise, loosely, under the
 * sanitizers of make check). */
static void
test_bounded(void)
{
	static const gchar *const units[] = { "a.", "S01E01-", "- 01 ", "1x01-", "a-1-", "[x]", "5.1.", "Season 1 " };
	gint64 start = g_get_monotonic_time();
	guint u;
	guint round;

	for (round = 0; round < 20; round++) {
		for (u = 0; u < G_N_ELEMENTS(units); u++) {
			GString *name = g_string_new(NULL);
			AxisRelease *r;

			while (name->len + strlen(units[u]) <= AXIS_RELEASE_NAME_MAX)
				g_string_append(name, units[u]);
			r = parse(name->str);
			axis_release_free(r);
			g_string_free(name, TRUE);
		}
	}
	g_assert_cmpint(g_get_monotonic_time() - start, <, 20 * G_USEC_PER_SEC);
}

/* ── Rules that the corpora show, checked one by one ────────────── */

/* Group guards: DTS-HD, DD-EX, H-264, WEB-DL and the like are not groups. */
static void
test_group_guards(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *group;
	} cases[] = {
		{ "Film.2020.1080p.BluRay.DTS-HD", NULL },
		{ "Film.2020.1080p.BluRay.DD-EX", NULL },
		{ "Film.2020.1080p.BluRay.H-264", NULL },
		{ "Film.2020.1080p.WEB-DL", NULL },
		{ "Film.2020.1080p.BluRay.x-264", NULL },
		{ "Film.2020.1080p.BluRay.E-AC-3", NULL },
		{ "Film.2020.1080p.BluRay.Blu-Ray", NULL },
		{ "Film.2020.1080p-1080p", NULL },
		{ "Film.2020.1080p.x264-1", NULL },
		{ "Film.2020.1080p.x264-2020", NULL },
		{ "Film.2020.1080p.x264-WEB", NULL },
		{ "Film.2020.1080p.x264-GERMAN", NULL },
		{ "DUST-E", NULL },
		{ "Q-Men", NULL },
		{ "The.Real-McGraw", NULL },
		{ "Film.2020.1080p.BluRay.x264-GROUP", "GROUP" },
		{ "Film.2020.1080p.BluRay.DTS-HD.MA.5.1-GROUP", "GROUP" },
		{ "Film.2020.1080p.WEB-DL-GROUP", "GROUP" },
		{ "Film.2020-GROUP", "GROUP" },
		{ "Film.2020.1080p.x264-GROUP[rarbg]", "GROUP" },
		{ "Film.2020.1080p.x264-GROUP [TGx]", "GROUP" },
		{ "Film.2020.1080p.x264-GROUP-xpost", "GROUP" },
		{ "Film (2020) [1080p] [YTS.MX]", "YTS.MX" },
		{ "Film (2020) [1080p]", NULL },
		{ "[Group] Film (2020) [1080p][ABCD1234]", "Group" },
		{ "[www.example.com] Film.2020.1080p.x264-GROUP", "GROUP" },
		{ "www.example.com - Film.2020.1080p.x264-GROUP", "GROUP" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(AxisRelease) r = parse(cases[i].name);

		if (g_strcmp0(r->group, cases[i].group) != 0)
			g_error("group of '%s': expected %s, got %s", cases[i].name, or_none(cases[i].group),
				or_none(r->group));
	}
	{
		g_autoptr(AxisRelease) site = parse("[www.example.com] Film.2020.1080p.x264-GROUP");
		g_autoptr(AxisRelease) prefix = parse("www.example.com - Film.2020.1080p.x264-GROUP");

		g_assert_cmpstr(site->title, ==, "Film");
		g_assert_cmpstr(prefix->title, ==, "Film");
	}
}

/* Titles that are years, years at the edge of belief. */
static void
test_years(void)
{
	{
		g_autoptr(AxisRelease) in = axis_release_parse_full("Film.2027.1080p", -1, 2027);
		g_autoptr(AxisRelease) out = axis_release_parse_full("Film.2027.1080p", -1, 2026);

		g_assert_cmpint(in->year, ==, 2027);
		g_assert_cmpstr(in->title, ==, "Film");
		g_assert_cmpint(out->year, ==, 0);
		g_assert_cmpstr(out->title, ==, "Film 2027");
	}
	{
		g_autoptr(AxisRelease) first = parse("Film.1888.DVDRip");
		g_autoptr(AxisRelease) before = parse("Film.1887.DVDRip");

		g_assert_cmpint(first->year, ==, 1888);
		g_assert_cmpint(before->year, ==, 0);
		g_assert_cmpstr(before->title, ==, "Film 1887");
	}
	/* A year after the anchor is not the year. */
	{
		g_autoptr(AxisRelease) r = parse("Film.1080p.2019");

		g_assert_cmpint(r->year, ==, 0);
		g_assert_cmpstr(r->title, ==, "Film");
		g_assert_cmpuint(r->unknown->len, ==, 1);
	}
	/* "Season 2" followed by a year is a film's title. */
	{
		g_autoptr(AxisRelease) r = parse("Quiet.Season.2.2008.1080p.BluRay.x264-GROUP");

		g_assert_cmpstr(r->title, ==, "Quiet Season 2");
		g_assert_cmpint(r->year, ==, 2008);
		g_assert_false(axis_release_is_episode(r));
	}
	/* axis_release_parse() believes up to next year. */
	{
		g_autoptr(GDateTime) now = g_date_time_new_now_local();
		g_autofree gchar *name = g_strdup_printf("Film.%d.1080p", g_date_time_get_year(now) + 1);
		g_autofree gchar *later = g_strdup_printf("Film.%d.1080p", g_date_time_get_year(now) + 2);
		g_autoptr(AxisRelease) r = axis_release_parse(name);
		g_autoptr(AxisRelease) l = axis_release_parse(later);

		g_assert_cmpint(r->year, ==, g_date_time_get_year(now) + 1);
		g_assert_cmpint(l->year, ==, 0);
	}
}

/*
 * fix-identify-hunt-findings, item I3: a quality word is a title word when
 * the title would be nothing without it, or when it is written as a title
 * word before a year in brackets ("Title (Year)", as folders are named).
 * Each of these read as "The" or as no title before.  (Made-up titles.)
 */
static void
test_quality_words_in_titles(void)
{
	static const struct {
		const gchar *name;
		const gchar *title;
		gint         year;
	} cases[] = {
		{ "The Remux Brothers (2010)", "The Remux Brothers", 2010 },
		{ "Atmos (2018)", "Atmos", 2018 },
		{ "Hdr Ghosts (2019)", "Hdr Ghosts", 2019 },
		{ "Dark Remux Harbour (2011)", "Dark Remux Harbour", 2011 },
		{ "Atmos.2018.1080p.WEB-DL.mkv", "Atmos", 2018 },
		{ "The Remux Brothers (2010) Remux-1080p.mkv", "The Remux Brothers", 2010 },
		{ NULL, NULL, 0 }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) r = axis_release_parse_full(cases[i].name, -1, 2026);

		if (g_strcmp0(r->title, cases[i].title) != 0 || r->year != cases[i].year)
			g_error("'%s': title '%s' year %d, expected '%s' %d", cases[i].name, r->title != NULL ? r->title : "(none)",
				r->year, cases[i].title, cases[i].year);
	}
	/* What follows the year is still quality. */
	{
		g_autoptr(AxisRelease) r = axis_release_parse_full("The Remux Brothers (2010) Remux-1080p.mkv", -1, 2026);

		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_REMUX);
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_1080P);
	}
	/* A tag in capitals after a title, the year not in brackets, and a
	 * quality word after the year are as they were. */
	{
		g_autoptr(AxisRelease) tag = axis_release_parse_full("Lantern.Cove.REMUX.(2010)", -1, 2026);
		g_autoptr(AxisRelease) dots = axis_release_parse_full("Lantern.Cove.Remux.2010.1080p", -1, 2026);
		g_autoptr(AxisRelease) after = axis_release_parse_full("Lantern.Cove.2010.Remux.1080p", -1, 2026);

		g_assert_cmpstr(tag->title, ==, "Lantern Cove");
		g_assert_cmpstr(dots->title, ==, "Lantern Cove");
		g_assert_cmpstr(after->title, ==, "Lantern Cove");
		g_assert_cmpint(after->year, ==, 2010);
		g_assert_cmpint(after->source, ==, AXIS_RELEASE_SOURCE_REMUX);
	}
}

/*
 * fix-identify-regional-years, B: a name that is exactly a folder's
 * "<something> (<year>)" says so (folder_form), and its something is the
 * title even when it starts with a quality word.  "4K (2015)" and "x264
 * Ghosts (2015)" read as no title before.  Release names, and a folder
 * name with more after the year, are read as before.  (Made-up titles.)
 */
static void
test_folder_form(void)
{
	static const struct {
		const gchar *name;
		gboolean     form;
		const gchar *title;
		gint         year;
	} cases[] = {
		{ "412 (2011)", TRUE, "412", 2011 },
		{ "412 (2011).mkv", TRUE, "412", 2011 },
		{ "7 (2014)", TRUE, "7", 2014 },
		{ "Q (2016)", TRUE, "Q", 2016 },
		{ "1983 (1961)", TRUE, "1983", 1961 },
		{ "4K (2015)", TRUE, "4K", 2015 },
		{ "x264 Ghosts (2015)", TRUE, "x264 Ghosts", 2015 },
		{ "Lantern Cove (2010)", TRUE, "Lantern Cove", 2010 },
		{ "Lantern Cove Extended (2010)", TRUE, "Lantern Cove", 2010 },
		{ "Lantern Cove (2010) [tmdb-12]", FALSE, "Lantern Cove", 2010 },
		{ "Lantern Cove (2010) 1080p", FALSE, "Lantern Cove", 2010 },
		{ "Lantern.Cove.(2010)", FALSE, "Lantern Cove", 2010 },
		{ "Lantern Cove (Director's Cut) (2010)", FALSE, NULL, 0 },
		{ "Lantern Cove (2031)", FALSE, NULL, 0 },
		{ "(2010)", FALSE, NULL, 0 },
		{ "412.2011.1080p.BluRay.x264-GROUP", FALSE, "412", 2011 },
		{ NULL, FALSE, NULL, 0 }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) r = axis_release_parse_full(cases[i].name, -1, 2026);

		if (r->folder_form != cases[i].form || (cases[i].title != NULL && (g_strcmp0(r->title, cases[i].title) != 0 ||
		    r->year != cases[i].year)))
			g_error("'%s': form %d title '%s' year %d, expected %d '%s' %d", cases[i].name, r->folder_form,
				r->title != NULL ? r->title : "(none)", r->year, cases[i].form,
				cases[i].title != NULL ? cases[i].title : "(any)", cases[i].year);
	}
	/* The quality word is the title's, not a field. */
	{
		g_autoptr(AxisRelease) r = axis_release_parse_full("4K (2015)", -1, 2026);

		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_NONE);
	}
}

/* Nothing inferred: each of these states one field and nothing else. */
static void
test_nothing_inferred(void)
{
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.AMZN");

		g_assert_cmpstr(r->service, ==, "amazon");
		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_NONE);
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_NONE);
	}
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.UHD");

		/* UHD states the resolution, not the source. */
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_2160P);
		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_NONE);
	}
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.REMUX");

		g_assert_cmpint(r->source, ==, AXIS_RELEASE_SOURCE_REMUX);
		g_assert_cmpint(r->resolution, ==, AXIS_RELEASE_RESOLUTION_NONE);
		g_assert_cmpint(r->codec, ==, AXIS_RELEASE_CODEC_NONE);
	}
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.Hi10P");

		g_assert_cmpint(r->bit_depth, ==, 10);
		g_assert_cmpint(r->codec, ==, AXIS_RELEASE_CODEC_NONE);
	}
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.Atmos");

		g_assert_true(r->atmos);
		g_assert_cmpuint(r->audio, ==, 0);
		g_assert_cmpstr(r->channels, ==, "");
	}
	/* Words after the title that nothing claims are returned, in order. */
	{
		g_autoptr(AxisRelease) r = parse("Film.2020.Zorgle.1080p.Blarg.x264-GROUP");

		g_assert_cmpuint(r->unknown->len, ==, 2);
		g_assert_cmpstr(g_ptr_array_index(r->unknown, 0), ==, "Zorgle");
		g_assert_cmpstr(g_ptr_array_index(r->unknown, 1), ==, "Blarg");
	}
}

/* ── Title keys ─────────────────────────────────────────────────── */

static void
test_title_key(void)
{
	static const struct
	{
		const gchar *a;
		const gchar *b;
	} same[] = {
		{ "Dusty II", "Dusty 2" },
		{ "Sky Battles Episode IV", "Sky.Battles.Episode.4" },
		{ "Newt-Man", "Newt Man" },
		{ "Newt-Man", "NewtMan" },
		{ "Swift & Fierce", "Swift and Fierce" },
		{ "Swift&Fierce", "swift and fierce" },
		{ "Aur\xc3\xa9lie", "Aurelie" },
		{ "Marlow's Nine", "Marlows Nine" },
		{ "Marlow\xe2\x80\x99s Nine", "Marlows Nine" },
		{ "Bram's Hollow", "Bram s Hollow" },
		{ "I'm Thinking of Painting Walls", "Im Thinking of Painting Walls" },
		{ "Stra\xc3\x9f" "e", "Strasse" },
		{ "\xef\xbc\xa1\xef\xbd\x82\xef\xbd\x83", "abc" },
		{ "La casa \xc3\xa8 calda", "La.Casa.e.Calda" },
		{ "Part XXXIX", "Part 39" },
		{ "Harriet X", "Harriet 10" },
		/* A name a feed escaped twice (audit of real names). */
		{ "Sea Pals Near &amp; Far", "Sea Pals: Near & Far" },
		{ "Salt &amp;amp; Thunder", "Salt and Thunder" },
		{ "The Night Line 1.2.3", "The Night Line 1 2 3" },
		/* Cherokee capitals and small letters (U+13E3 U+13B3 U+13A9). */
		{ "\xe1\x8f\xa3\xe1\x8e\xb3\xe1\x8e\xa9", "\xea\xae\xb3\xea\xae\x83\xea\xad\xb9" },
	};
	static const struct
	{
		const gchar *a;
		const gchar *b;
	} different[] = {
		{ "The Lattice", "Lattice" },
		{ "Xenoform", "Xenoforms" },
		{ "Ei8ht", "Eight" },
		{ "Dusty II", "Dusty 3" },
		{ "Part XL", "Part 40" },
		{ "Drizzle", "Drizzle 2" },
		{ "Fork &amp Spoon", "Fork and Spoon" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(same); i++) {
		g_autofree gchar *a = axis_release_title_key(same[i].a);
		g_autofree gchar *b = axis_release_title_key(same[i].b);
		g_autofree gchar *again = axis_release_title_key(a);

		if (a == NULL || b == NULL || strcmp(a, b) != 0)
			g_error("keys of '%s' and '%s' differ: '%s', '%s'", same[i].a, same[i].b, or_none(a), or_none(b));
		g_assert_cmpstr(again, ==, a);
	}
	for (i = 0; i < G_N_ELEMENTS(different); i++) {
		g_autofree gchar *a = axis_release_title_key(different[i].a);
		g_autofree gchar *b = axis_release_title_key(different[i].b);

		g_assert_nonnull(a);
		g_assert_nonnull(b);
		if (strcmp(a, b) == 0)
			g_error("'%s' and '%s' have the same key '%s'", different[i].a, different[i].b, a);
	}
	{
		g_autofree gchar *key = axis_release_title_key("Dusty II: The Return!");
		g_autofree gchar *empty = axis_release_title_key("...!!!");
		g_autofree gchar *long_text = g_strnfill(4097, 'a');
		g_autofree gchar *limit = g_strnfill(4096, 'a');
		g_autofree gchar *kept = axis_release_title_key(limit);

		g_assert_cmpstr(key, ==, "dusty2thereturn");
		g_assert_cmpstr(empty, ==, "");
		g_assert_null(axis_release_title_key(NULL));
		g_assert_null(axis_release_title_key("\xff"));
		g_assert_null(axis_release_title_key(long_text));
		g_assert_nonnull(kept);
	}
}

/* ── Matching ───────────────────────────────────────────────────── */

static AxisMetaTitle *
title_new(
	AxisMetaKind  kind,
	const gchar  *name,
	gint          year,
	const gchar  *original,
	const gchar  *other,
	const gchar  *country
){
	AxisMetaTitle *title = axis_meta_title_new(kind);

	title->title = g_strdup(name);
	title->year = year;
	title->original_title = g_strdup(original);
	if (other != NULL) {
		AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

		alt->title = g_strdup(other);
		alt->country = g_strdup(country);
		title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
		g_ptr_array_add(title->alt_titles, alt);
	}
	return title;
}

static AxisReleaseMatch
match_name(
	const gchar   *name,
	GPtrArray     *titles,
	gint          *matched
){
	g_autoptr(AxisRelease) r = parse(name);
	g_autofree gchar *reason = NULL;
	AxisReleaseMatch result = axis_release_match(r, (const AxisMetaTitle *const *)titles->pdata, titles->len,
		matched, &reason);

	g_assert_nonnull(reason);
	g_test_message("%s: %s (%s)", name, axis_release_match_name(result), reason);
	/* Ambiguous is never a match. */
	if (result != AXIS_RELEASE_MATCH_YES)
		g_assert_cmpint(*matched, ==, -1);
	else
		g_assert_cmpint(*matched, >=, 0);
	return result;
}

static GPtrArray *
titles_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
}

/* Spec: Another name. */
static void
test_match_other_names(void)
{
	g_autoptr(GPtrArray) titles = titles_new();
	gint matched = -1;

	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Mesa", 2021, NULL, "Mesa: Part One", "US"));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "The House Is Warm", 1997, "La casa \xc3\xa8 calda",
		NULL, NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Swift & Fierce", 2009, NULL, NULL, NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Dusty II", 1979, NULL, NULL, NULL));

	g_assert_cmpint(match_name("Mesa.Part.One.2021.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_assert_cmpint(match_name("Mesa.2021.2160p.WEB-DL-GROUP", titles, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_assert_cmpint(match_name("La.Casa.e.Calda.1997.ITA.1080p.BluRay.x265-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 1);
	g_assert_cmpint(match_name("Swift.and.Fierce.2009.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 2);
	/* Sequel numerals. */
	g_assert_cmpint(match_name("Dusty.2.1979.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 3);
	g_assert_cmpint(match_name("Dusty.II.1979.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(match_name("Dusty.III.1982.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_NONE);
	g_assert_cmpint(match_name("Mesa.Part.Two.2024.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_NONE);
}

static void
test_match_years(void)
{
	g_autoptr(GPtrArray) titles = titles_new();
	g_autoptr(GPtrArray) close = titles_new();
	g_autoptr(GPtrArray) yearless = titles_new();
	gint matched = -1;

	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Drizzle", 1995, NULL, NULL, NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Drizzle", 1986, NULL, NULL, NULL));

	/* The year picks one. */
	g_assert_cmpint(match_name("Drizzle.1995.1080p.BluRay.x264-GROUP", titles, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_assert_cmpint(match_name("Drizzle.1986.DVDRip.XviD-GROUP", titles, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 1);
	/* Off by one (release dates differ by country). */
	g_assert_cmpint(match_name("Drizzle.1996.1080p.BluRay.x264-GROUP", titles, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	/* Off by two is not the same film. */
	g_assert_cmpint(match_name("Drizzle.1997.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_NONE);
	/* Spec: Two candidates. */
	g_assert_cmpint(match_name("Drizzle.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);

	/* Two within a year of the name: the one of the name's own year is
	 * taken; a year between two of them means neither. */
	g_ptr_array_add(close, title_new(AXIS_META_KIND_FILM, "Doubles", 2010, NULL, NULL, NULL));
	g_ptr_array_add(close, title_new(AXIS_META_KIND_FILM, "Doubles", 2011, NULL, NULL, NULL));
	g_assert_cmpint(match_name("Doubles.2010.1080p.BluRay.x264-GROUP", close, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_assert_cmpint(match_name("Doubles.2011.1080p.BluRay.x264-GROUP", close, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 1);
	g_assert_cmpint(match_name("Doubles.2009.1080p.BluRay.x264-GROUP", close, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_ptr_array_add(close, title_new(AXIS_META_KIND_FILM, "Doubles", 2013, NULL, NULL, NULL));
	g_assert_cmpint(match_name("Doubles.2012.1080p.BluRay.x264-GROUP", close, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	g_ptr_array_add(close, title_new(AXIS_META_KIND_FILM, "Doubles", 2010, NULL, NULL, NULL));
	g_assert_cmpint(match_name("Doubles.2010.1080p.BluRay.x264-GROUP", close, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);

	/* No year in the name and one title of that name; a title with no
	 * year to compare. */
	g_ptr_array_add(yearless, title_new(AXIS_META_KIND_FILM, "Lonely Film", 0, NULL, NULL, NULL));
	g_assert_cmpint(match_name("Lonely.Film.1080p.WEB-DL-GROUP", yearless, &matched), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(match_name("Lonely.Film.2001.1080p.WEB-DL-GROUP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
}

/* The reason for @name against @titles holds @expected. */
static void
assert_reason(
	const gchar *name,
	GPtrArray   *titles,
	const gchar *expected
){
	g_autoptr(AxisRelease) r = parse(name);
	g_autofree gchar *reason = NULL;
	gint matched = -1;

	(void)axis_release_match(r, (const AxisMetaTitle *const *)titles->pdata, titles->len, &matched, &reason);
	if (reason == NULL || strstr(reason, expected) == NULL)
		g_error("%s: the reason '%s' does not say '%s'", name, reason != NULL ? reason : "(none)", expected);
}

/*
 * Same-named films of other years, as a film's search compares them (the
 * film itself first, then the provider's other films of its name): found
 * on a real search, where a remake and an announced film of the same name
 * made every release of the film ambiguous.
 */
static void
test_match_same_name(void)
{
	g_autoptr(GPtrArray) dated = titles_new();
	g_autoptr(GPtrArray) yearless = titles_new();
	gint matched = -1;

	g_ptr_array_add(dated, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 1968, NULL, NULL, NULL));
	g_ptr_array_add(dated, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 1990, NULL, NULL, NULL));
	g_ptr_array_add(dated, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 2026, NULL, NULL, NULL));

	/* A name with a year means the film of that year, whatever others of
	 * that name there are. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", dated, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	g_assert_cmpint(match_name("The.Quiet.Harbor.1990.1080p.BluRay.x264-GRP", dated, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 1);
	/* A known year that differs (more than one off) does not fit. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1975.1080p.BluRay.x264-GRP", dated, &matched), ==,
		AXIS_RELEASE_MATCH_NONE);
	assert_reason("The.Quiet.Harbor.1975.1080p.BluRay.x264-GRP", dated, "'The Quiet Harbor' (1968) has that title");
	/* Without a year it could be any of them; the reason names each with
	 * its year. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1080p.BluRay.x264-GRP", dated, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	assert_reason("The.Quiet.Harbor.1080p.BluRay.x264-GRP", dated,
		"3 titles fit: 'The Quiet Harbor' (1968), 'The Quiet Harbor' (1990), 'The Quiet Harbor' (2026)");

	/* Others of that name with no year known (search records without a
	 * date). */
	g_ptr_array_add(yearless, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 1968, NULL, NULL, NULL));
	g_ptr_array_add(yearless, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 0, NULL, NULL, NULL));
	g_ptr_array_add(yearless, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 0, NULL, NULL, NULL));
	/* The film's own year: the film (the others cannot be shown to be it). */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 0);
	assert_reason("The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", yearless,
		"the same year; 2 other titles of that name have no year known");
	/* A year one off: one with no year known could be that year too. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1969.1080p.BluRay.x264-GRP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	assert_reason("The.Quiet.Harbor.1969.1080p.BluRay.x264-GRP", yearless,
		"3 titles fit: 'The Quiet Harbor' (1968), 'The Quiet Harbor' (no year known)");
	/* Another year: only those with no year known could be it. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1990.1080p.BluRay.x264-GRP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	/* No year: the caution stays. */
	g_assert_cmpint(match_name("The.Quiet.Harbor.1080p.BluRay.x264-GRP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	assert_reason("The.Quiet.Harbor.1080p.BluRay.x264-GRP", yearless, "; the name has no year");

	/* The order does not matter: the film with the year is the one taken. */
	g_ptr_array_remove_index(yearless, 0);
	g_ptr_array_add(yearless, title_new(AXIS_META_KIND_FILM, "The Quiet Harbor", 1968, NULL, NULL, NULL));
	g_assert_cmpint(match_name("The.Quiet.Harbor.1968.1080p.BluRay.x264-GRP", yearless, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 2);
}

static void
test_match_series(void)
{
	g_autoptr(GPtrArray) titles = titles_new();
	gint matched = -1;

	g_ptr_array_add(titles, title_new(AXIS_META_KIND_SERIES, "Time Warden", 1963, NULL, NULL, NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_SERIES, "Time Warden", 2005, NULL, "Time Warden (2005)", NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_SERIES, "The Office Hours", 2005, NULL,
		"The Office Hours (US)", "US"));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_SERIES, "The Office Hours", 2001, NULL,
		"The Office Hours (UK)", "GB"));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Harbor Lights", 2019, NULL, NULL, NULL));
	g_ptr_array_add(titles, title_new(AXIS_META_KIND_SERIES, "Kitchen Wars", 2008, NULL, NULL, NULL));

	/* A year in the series' name. */
	g_assert_cmpint(match_name("Time.Warden.2005.S01E01.720p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 1);
	g_assert_cmpint(match_name("Time.Warden.S01E01.720p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	/* A country in the series' name, through its other names. */
	g_assert_cmpint(match_name("The.Office.Hours.US.S01E01.720p.HDTV.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 2);
	g_assert_cmpint(match_name("The.Office.Hours.UK.S01E01.720p.HDTV.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 3);
	g_assert_cmpint(match_name("The.Office.Hours.S01E01.720p.HDTV.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_AMBIGUOUS);
	/* For a series the year is optional. */
	g_assert_cmpint(match_name("Kitchen.Wars.S12E04.480p.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 5);
	/* An episode is never a film. */
	g_assert_cmpint(match_name("Harbor.Lights.S01E01.720p.HDTV.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_NONE);
	g_assert_cmpint(match_name("Harbor.Lights.2019.1080p.BluRay.x264-GROUP", titles, &matched), ==,
		AXIS_RELEASE_MATCH_YES);
	g_assert_cmpint(matched, ==, 4);
}

static void
test_match_edges(void)
{
	g_autoptr(GPtrArray) titles = titles_new();
	g_autoptr(AxisRelease) none = parse("");
	g_autoptr(AxisRelease) film = parse("Drizzle.1995.1080p");
	g_autofree gchar *reason = NULL;
	gint matched = 7;

	g_ptr_array_add(titles, title_new(AXIS_META_KIND_FILM, "Drizzle", 1995, NULL, NULL, NULL));
	g_assert_cmpint(axis_release_match(none, (const AxisMetaTitle *const *)titles->pdata, titles->len, &matched,
		&reason), ==, AXIS_RELEASE_MATCH_NONE);
	g_assert_cmpint(matched, ==, -1);
	g_assert_nonnull(reason);
	g_clear_pointer(&reason, g_free);
	g_assert_cmpint(axis_release_match(film, NULL, 0, &matched, &reason), ==, AXIS_RELEASE_MATCH_NONE);
	g_clear_pointer(&reason, g_free);
	g_assert_cmpint(axis_release_match(NULL, NULL, 0, NULL, NULL), ==, AXIS_RELEASE_MATCH_NONE);
	g_assert_cmpint(axis_release_match(film, (const AxisMetaTitle *const *)titles->pdata, titles->len, NULL,
		NULL), ==, AXIS_RELEASE_MATCH_YES);
	g_assert_cmpstr(axis_release_match_name(AXIS_RELEASE_MATCH_AMBIGUOUS), ==, "ambiguous");

	/* Found by fuzzing: a candidate whose title is not valid text (the
	 * store's never are, but the matcher does not rely on it) still gets a
	 * reason that is, with nothing in it that could break a line. */
	{
		g_autoptr(GPtrArray) odd = titles_new();
		g_autoptr(AxisRelease) drizzle = parse("Drizzle.2001.1080p");

		g_ptr_array_add(odd, title_new(AXIS_META_KIND_FILM, "Dri\xff\xff" "zzle\n\xe2\x80\xae", 1995, NULL, "Drizzle",
			NULL));
		g_assert_cmpint(axis_release_match(drizzle, (const AxisMetaTitle *const *)odd->pdata, odd->len, &matched,
			&reason), ==, AXIS_RELEASE_MATCH_NONE);
		g_assert_true(g_utf8_validate(reason, -1, NULL));
		g_assert_null(strchr(reason, '\n'));
		g_assert_null(strstr(reason, "\xe2\x80\xae"));
		g_clear_pointer(&reason, g_free);
		g_assert_cmpint(axis_release_match(film, (const AxisMetaTitle *const *)odd->pdata, odd->len, &matched,
			&reason), ==, AXIS_RELEASE_MATCH_YES);
		g_assert_true(g_utf8_validate(reason, -1, NULL));
		g_clear_pointer(&reason, g_free);
	}
}

/* ── Names of values ────────────────────────────────────────────── */

static void
test_names(void)
{
	static const FlagName sets[] = {
		axis_release_hdr_name, axis_release_audio_name, axis_release_edition_name,
		axis_release_lang_mark_name, axis_release_revision_name, axis_release_mark_name,
		axis_release_stop_name,
	};
	static const guint sizes[] = { 6, 12, 18, 5, 4, 5, 7 };
	guint s;
	guint i;

	for (s = 0; s < G_N_ELEMENTS(sets); s++) {
		g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

		for (i = 0; i < 32; i++) {
			const gchar *name = sets[s](1u << i);

			if (i < sizes[s]) {
				g_assert_nonnull(name);
				g_assert_false(g_hash_table_contains(seen, name));
				g_hash_table_add(seen, (gpointer)name);
			} else {
				g_assert_null(name);
			}
		}
		g_assert_null(sets[s](0));
		g_assert_null(sets[s](3));
	}
	for (i = AXIS_RELEASE_RESOLUTION_480P; i <= AXIS_RELEASE_RESOLUTION_2160P; i++)
		g_assert_nonnull(axis_release_resolution_name((AxisReleaseResolution)i));
	for (i = AXIS_RELEASE_SOURCE_CAM; i <= AXIS_RELEASE_SOURCE_DISC; i++)
		g_assert_nonnull(axis_release_source_name((AxisReleaseSource)i));
	for (i = AXIS_RELEASE_CODEC_H264; i <= AXIS_RELEASE_CODEC_VP9; i++)
		g_assert_nonnull(axis_release_codec_name((AxisReleaseCodec)i));
	g_assert_null(axis_release_resolution_name(AXIS_RELEASE_RESOLUTION_NONE));
	g_assert_null(axis_release_source_name(AXIS_RELEASE_SOURCE_NONE));
	g_assert_null(axis_release_codec_name(AXIS_RELEASE_CODEC_NONE));
	{
		g_autoptr(AxisRelease) r = parse("Show.S01E01-E03.2160p.WEB-DL.DDP5.1.Atmos.DV.HDR.H.265-GROUP");
		g_autofree gchar *line = axis_release_describe(r);

		g_assert_cmpstr(line, ==, "title='Show' season=1 episodes=1-3 resolution=2160p source=webdl codec=h265 "
			"hdr=hdr,dv audio=eac3 atmos channels=5.1 group='GROUP'");
	}
}

/*
 * What a name says it holds of a series (axis_release_states()): the one
 * reading the TV module's matcher and identification's file reading share.
 */
static void
test_states(void)
{
	static const struct
	{
		const gchar          *name;
		AxisReleaseHoldsKind  kind;
		const gchar          *code;
		gint                  first;
		gint                  last;
		guint                 n;
		const gchar          *date;
	} cases[] = {
		{ "Lantern.Row.S01E02.720p.HDTV.x264-GRP", AXIS_RELEASE_HOLDS_EPISODES, NULL, 1, 1, 1, "" },
		{ "Lantern.Row.S01E02E03.720p.HDTV.x264-GRP", AXIS_RELEASE_HOLDS_EPISODES, NULL, 1, 1, 2, "" },
		{ "Lantern Row - 1x02 - Chapter 2", AXIS_RELEASE_HOLDS_EPISODES, NULL, 1, 1, 1, "" },
		{ "Lantern.Row.S02.1080p.BluRay.x264-GRP", AXIS_RELEASE_HOLDS_SEASONS, NULL, 2, 2, 0, "" },
		{ "Lantern.Row.S01-S03.1080p.BluRay.x264-GRP", AXIS_RELEASE_HOLDS_SEASONS, NULL, 1, 3, 0, "" },
		{ "Lantern.Row.Complete.Series.1080p.BluRay.x264-GRP", AXIS_RELEASE_HOLDS_COMPLETE, NULL, 0, 0, 0, "" },
		{ "The.Night.Ledger.2024.03.11.Guest.Name.720p.WEB.h264-GRP", AXIS_RELEASE_HOLDS_DATE, NULL, 0, 0, 0,
		  "2024-03-11" },
		{ "Glass Harbor - 05 [1080p]", AXIS_RELEASE_HOLDS_NOTHING, "absolute_only", 0, 0, 0, "" },
		{ "Lantern Row - Pilot Party", AXIS_RELEASE_HOLDS_NOTHING, "no_episode", 0, 0, 0, "" },
		{ NULL, 0, NULL, 0, 0, 0, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) r = parse(cases[i].name);
		AxisReleaseStates s;

		axis_release_states(r, &s);
		if (s.kind != cases[i].kind || g_strcmp0(s.code, cases[i].code) != 0 || strcmp(s.date, cases[i].date) != 0)
			g_error("%s: kind %d code %s date '%s'", cases[i].name, s.kind, s.code, s.date);
		if (s.kind == AXIS_RELEASE_HOLDS_EPISODES || s.kind == AXIS_RELEASE_HOLDS_SEASONS) {
			g_assert_cmpint(s.season_first, ==, cases[i].first);
			g_assert_cmpint(s.season_last, ==, cases[i].last);
		}
		g_assert_cmpuint(s.n_episodes, ==, cases[i].n);
	}
	{
		g_autoptr(AxisRelease) r = parse("Glass Harbor - 05 [1080p]");
		AxisReleaseStates s;

		axis_release_states(r, &s);
		g_assert_cmpint(s.absolute, ==, 5);
		g_assert_false(s.with_season);
	}
}

/*
 * add-anime-support, task 2.1: what reading a name by itself makes of the
 * anime forms no corpus line pins.  These record the reading as it was before
 * slice A2 and as it stays: A2 adds a second reading after a known title
 * (axis_release_read_after()) and changes nothing here.
 */
static void
test_anime_forms_today(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *reading;
	} cases[] = {
		/* A bare number with no hyphen stays in the title. */
		{ "[Tsuki] Tenkuu Rangers 137 [1080p]", "title='Tenkuu Rangers 137' resolution=1080p group='Tsuki'" },
		{ "Tenkuu.Rangers.137.1080p.WEB", "title='Tenkuu Rangers 137' resolution=1080p source=webdl" },
		{ "Tenkuu Rangers 05 [1080p]", "title='Tenkuu Rangers 05' resolution=1080p" },
		/* A range with no brackets and no hyphen before it. */
		{ "Tenkuu Rangers 001-052 [BD 1080p]", "title='Tenkuu Rangers 001-052' resolution=1080p source=bluray" },
		/* Half an episode: the whole part is read as the number, and the
		 * half is a word not understood.  (For an anime series the TV module
		 * refuses the name: axis_tv_match_release_anime().) */
		{ "[NekoFansub] Kumo Academy - 12.5 [720p]",
		  "title='Kumo Academy' absolute=12 resolution=720p group='NekoFansub' unknown=['5']" },
		/* A single digit after a hyphen: read with a group, a title word
		 * without one. */
		{ "Kumo Academy - 7 [1080p]", "title='Kumo Academy 7' resolution=1080p" },
		{ "[NekoFansub] Kumo Academy - 7 [1080p]", "title='Kumo Academy' absolute=7 resolution=1080p group='NekoFansub'" },
		/* A title that ends in a number, then a hyphen and a number. */
		{ "[Tsuki] Tenkuu Rangers 2 - 05 [1080p]", "title='Tenkuu Rangers 2' absolute=5 resolution=1080p group='Tsuki'" },
		/* A special by its word. */
		{ "[Tsuki] Tenkuu Rangers - OVA 2 [1080p]",
		  "title='Tenkuu Rangers' season=0 episodes=2 resolution=1080p group='Tsuki'" },
		{ "[Tsuki] Tenkuu Rangers - NCOP 01 [1080p]", "title='Tenkuu Rangers NCOP 01' resolution=1080p group='Tsuki'" },
		{ "[Tsuki] Tenkuu Rangers - Movie [1080p]", "title='Tenkuu Rangers Movie' resolution=1080p group='Tsuki'" },
		/* Zero. */
		{ "[Tsuki] Tenkuu Rangers - 00 [1080p]", "title='Tenkuu Rangers' absolute=0 resolution=1080p group='Tsuki'" },
		/* A year after the hyphen. */
		{ "Kumo Academy - 2023 [1080p]", "title='Kumo Academy' year=2023 resolution=1080p" },
		/* A season and an episode its season may lack. */
		{ "Tenkuu.Rangers.S01E137.1080p.WEB.H264-GRP",
		  "title='Tenkuu Rangers' season=1 episodes=137 resolution=1080p source=webdl codec=h264 group='GRP'" },
		{ NULL, NULL }
	};
	guint bad = 0;
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) r = parse(cases[i].name);
		g_autofree gchar *line = axis_release_describe(r);

		if (strcmp(line, cases[i].reading) != 0) {
			g_printerr("%s\n    reads: %s\n    pinned: %s\n", cases[i].name, line, cases[i].reading);
			bad++;
		}
	}
	g_assert_cmpuint(bad, ==, 0);
}

/*
 * add-anime-support, A2: the reading after a known title
 * (axis_release_read_after()), from its own corpus.  Each line is a name, the
 * names of one series, and what the reading gives or why it is not read.
 * Spec (release-parsing delta): A bare number; A range with no brackets; The
 * title ends in a number; A number that is not the title's; Half an episode;
 * A year; Another series.
 */
static void
test_after_corpus(void)
{
	static const gchar *const keys[] = {
		"name", "titles", "read", "not_read", "range", "hyphen", "digits", "version", "note", NULL
	};
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) errors = g_string_new(NULL);
	JsonArray *lines;
	guint i;

	json_parser_load_from_file(parser, DIR "anime.json", &error);
	g_assert_no_error(error);
	lines = json_node_get_array(json_parser_get_root(parser));
	g_assert_cmpuint(json_array_get_length(lines), >=, 50);
	for (i = 0; i < json_array_get_length(lines); i++) {
		JsonObject *line = json_array_get_object_element(lines, i);
		const gchar *name = json_object_get_string_member(line, "name");
		JsonArray *given = json_object_get_array_member(line, "titles");
		g_autoptr(GList) members = json_object_get_members(line);
		g_autofree const gchar **titles = g_new0(const gchar *, json_array_get_length(given) + 1);
		const gchar *expected_code = json_object_has_member(line, "not_read") ?
			json_object_get_string_member(line, "not_read") : NULL;
		AxisReleaseAfter after;
		AxisReleaseAfter again;
		GList *l;
		guint k;

		for (l = members; l != NULL; l = l->next) {
			if (!g_strv_contains(keys, l->data))
				g_error("'%s': unknown label '%s'", name, (const gchar *)l->data);
		}
		g_assert_true(json_object_has_member(line, "read") != json_object_has_member(line, "not_read"));
		for (k = 0; k < json_array_get_length(given); k++)
			titles[k] = json_array_get_string_element(given, k);
		axis_release_read_after_full(name, -1, titles, json_array_get_length(given), LATEST_YEAR, &after);
		axis_release_read_after_full(name, (gssize)strlen(name), titles, json_array_get_length(given), LATEST_YEAR,
			&again);
		g_assert_cmpint(memcmp(&after, &again, sizeof(after)), ==, 0);
		if (expected_code != NULL) {
			if (after.read || g_strcmp0(after.code, expected_code) != 0)
				g_string_append_printf(errors, "  '%s': expected not read (%s), got %s %d-%d (%s)\n", name,
					expected_code, after.read ? "read" : "not read", after.first, after.last,
					after.code != NULL ? after.code : "no code");
			/* A code about what follows the name has words for a page. */
			if (after.titled != (strcmp(expected_code, "other_title") != 0 && strcmp(expected_code, "unreadable") != 0))
				g_string_append_printf(errors, "  '%s': titled is %d\n", name, after.titled);
			if (after.titled && axis_release_after_words(after.code) == NULL)
				g_string_append_printf(errors, "  '%s': the code %s has no words\n", name, after.code);
			continue;
		}
		{
			JsonArray *read = json_object_get_array_member(line, "read");
			gint first = (gint)json_array_get_int_element(read, 0);
			gint last = (gint)json_array_get_int_element(read, 1);
			gboolean range = json_object_has_member(line, "range") && json_object_get_boolean_member(line, "range");
			gboolean hyphen = json_object_has_member(line, "hyphen") && json_object_get_boolean_member(line, "hyphen");
			gint version = json_object_has_member(line, "version") ? (gint)json_object_get_int_member(line, "version") : 0;
			guint digits = (guint)json_object_get_int_member(line, "digits");

			if (!after.read || after.code != NULL || !after.titled || after.first != first || after.last != last ||
			    after.range != range || after.hyphen != hyphen || after.version != version || after.digits != digits)
				g_string_append_printf(errors, "  '%s': expected %d-%d range %d hyphen %d v%d digits %u, got %s %d-%d "
					"range %d hyphen %d v%d digits %u (%s)\n", name, first, last, range, hyphen, version, digits,
					after.read ? "read" : "not read", after.first, after.last, after.range, after.hyphen,
					after.version, after.digits, after.code != NULL ? after.code : "no code");
		}
	}
	if (errors->len > 0)
		g_error("the reading after a title:\n%s", errors->str);
}

/* The reading after a title never fails and never changes what reading a name
 * by itself reports: every name of the three corpora, read by itself before
 * and after it is read against its own title, describes the same. */
static void
test_after_leaves_parser(void)
{
	static const gchar *const files[] = { DIR "films.json", DIR "episodes.json", DIR "regressions.json", NULL };
	guint f;
	guint n = 0;

	for (f = 0; files[f] != NULL; f++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		JsonArray *lines;
		guint i;

		json_parser_load_from_file(parser, files[f], &error);
		g_assert_no_error(error);
		lines = json_node_get_array(json_parser_get_root(parser));
		for (i = 0; i < json_array_get_length(lines); i++) {
			const gchar *name = json_object_get_string_member(json_array_get_object_element(lines, i), "name");
			g_autoptr(AxisRelease) before = parse(name);
			g_autofree gchar *line = axis_release_describe(before);
			const gchar *titles[2];
			AxisReleaseAfter after;
			g_autoptr(AxisRelease) again = NULL;
			g_autofree gchar *line2 = NULL;

			titles[0] = before->title;
			titles[1] = "Tenkuu Rangers";
			axis_release_read_after_full(name, -1, titles, 2, LATEST_YEAR, &after);
			g_assert_true(after.read == (after.code == NULL));
			if (after.read) {
				g_assert_cmpint(after.first, >=, 1);
				g_assert_cmpint(after.last, >=, after.first);
				g_assert_cmpint(after.last, <=, 9999);
			}
			again = parse(name);
			line2 = axis_release_describe(again);
			g_assert_cmpstr(line, ==, line2);
			n++;
		}
	}
	g_assert_cmpuint(n, >, 300);
}

/* Hostile and oversized input to the reading after a title. */
static void
test_after_hostile(void)
{
	static const gchar *const titles[] = { "Tenkuu Rangers", NULL, "", "!!!" };
	g_autofree gchar *huge = g_strnfill(AXIS_RELEASE_NAME_MAX + 1, 'a');
	g_autofree gchar *many = g_strnfill(AXIS_RELEASE_NAME_MAX, ' ');
	g_autoptr(GString) words = g_string_new("Tenkuu Rangers");
	AxisReleaseAfter after;
	guint i;

	axis_release_read_after_full(NULL, -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_false(after.read);
	g_assert_cmpstr(after.code, ==, "unreadable");
	axis_release_read_after_full(huge, -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_cmpstr(after.code, ==, "unreadable");
	axis_release_read_after_full("Tenkuu Rangers \xff 05", -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_cmpstr(after.code, ==, "unreadable");
	axis_release_read_after_full("Tenkuu Rangers\0 05", 18, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_cmpstr(after.code, ==, "unreadable");
	axis_release_read_after_full(many, -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_cmpstr(after.code, ==, "other_title");
	axis_release_read_after_full("Tenkuu Rangers 05", -1, NULL, 0, LATEST_YEAR, &after);
	g_assert_cmpstr(after.code, ==, "other_title");
	/* As many words as a name can hold. */
	for (i = 0; words->len + 2 <= AXIS_RELEASE_NAME_MAX; i++)
		g_string_append(words, " a");
	g_assert_cmpuint(i, >, 200);
	axis_release_read_after_full(words->str, -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_false(after.read);
	g_assert_true(after.titled);
	/* Only a bracket that closes is a leading tag. */
	axis_release_read_after_full("[[[[[[Tenkuu Rangers 05", -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR, &after);
	g_assert_false(after.read);
	axis_release_read_after_full("[a][b][c][d][e] Tenkuu Rangers 05", -1, titles, G_N_ELEMENTS(titles), LATEST_YEAR,
		&after);
	g_assert_false(after.read);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/release/anime-forms-today", test_anime_forms_today);
	g_test_add_func("/release/after/corpus", test_after_corpus);
	g_test_add_func("/release/after/leaves-parser", test_after_leaves_parser);
	g_test_add_func("/release/after/hostile", test_after_hostile);
	g_test_add_func("/release/corpus/films", test_films);
	g_test_add_func("/release/corpus/episodes", test_episodes);
	g_test_add_func("/release/corpus/regressions", test_regressions);
	g_test_add_func("/release/corpus/checker", test_checker);
	g_test_add_func("/release/scenarios", test_scenarios);
	g_test_add_func("/release/hostile", test_hostile);
	g_test_add_func("/release/bounded", test_bounded);
	g_test_add_func("/release/group-guards", test_group_guards);
	g_test_add_func("/release/years", test_years);
	g_test_add_func("/release/quality-words-in-titles", test_quality_words_in_titles);
	g_test_add_func("/release/folder-form", test_folder_form);
	g_test_add_func("/release/nothing-inferred", test_nothing_inferred);
	g_test_add_func("/release/title-key", test_title_key);
	g_test_add_func("/release/match/other-names", test_match_other_names);
	g_test_add_func("/release/match/years", test_match_years);
	g_test_add_func("/release/match/same-name", test_match_same_name);
	g_test_add_func("/release/match/series", test_match_series);
	g_test_add_func("/release/match/edges", test_match_edges);
	g_test_add_func("/release/names", test_names);
	g_test_add_func("/release/states", test_states);

	return g_test_run();
}
