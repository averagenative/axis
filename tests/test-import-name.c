/*
 * test-import-name.c - the import pipeline's pure parts: the names it gives
 * (scheme, character rule, byte budget, editions, renaming off), the format
 * and its check, what it reads from found names (subtitles, samples, parts,
 * extras), and the decision for every file found
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-import-pipeline:
 *   6.5 (the decisions) samples, extras folders, several films, an archive,
 *       an unreadable file, a wrong-film name and a runtime far off each give
 *       their named reason; a film titled "Sample" imports
 *   6.6 names: the scheme; hostile and over-long titles; the byte budget at
 *       255 with multi-byte text; '/' in a title; editions (the preview's
 *       equality with an import is in test-import.c)
 */

#include <glib.h>
#include <string.h>

#include "import/axis-import-private.h"

/* ── Helpers ────────────────────────────────────────────────────── */

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

static AxisImportName
build(
	const gchar *title,
	gint         year,
	const gchar *tmdb,
	const gchar *quality,
	const gchar *group,
	const gchar *edition,
	gsize        budget
){
	AxisImportNameInput input;
	AxisImportName name;
	const gchar *problem = NULL;

	memset(&input, 0, sizeof(input));
	input.title = title;
	input.year = year;
	input.tmdb_id = tmdb;
	input.quality = quality;
	input.group = group;
	input.edition = edition;
	input.extension = ".mkv";
	input.budget = budget;
	if (!axis_import_name_build(&input, &name, &problem))
		g_error("cannot name '%s': %s", title, problem);
	return name;
}

/* Every promise a name keeps. */
static void
assert_name_sound(
	const AxisImportName *name,
	gsize                 budget
){
	if (!axis_import_name_valid(name->folder))
		g_error("folder not valid: '%s'", name->folder);
	if (!axis_import_name_valid(name->file))
		g_error("file not valid: '%s'", name->file);
	g_assert_cmpuint(strlen(name->folder), <=, budget);
	g_assert_cmpuint(strlen(name->file), <=, budget);
	g_assert_true(g_utf8_validate(name->folder, -1, NULL));
	g_assert_true(g_utf8_validate(name->file, -1, NULL));
	/* Jellyfin: the file begins with the folder's text. */
	g_assert_true(g_str_has_prefix(name->file, name->folder));
	g_assert_true(g_str_has_suffix(name->file, ".mkv"));
	g_assert_true(g_str_has_prefix(name->file, name->stem));
	/* Within the default budget, the temporary name fits beside it in 255. */
	if (budget <= AXIS_IMPORT_NAME_BUDGET)
		g_assert_cmpuint(strlen(name->file) + AXIS_IMPORT_TEMP_MAX, <=, 255);
	g_assert_cmpuint(strlen(name->file), <=, 255);
}

/* ── 6.6 Names ──────────────────────────────────────────────────── */

static void
test_scheme(void)
{
	AxisImportNameInput input;
	AxisImportName name;

	name = build("Hearth", 1995, "6001", "BluRay 1080p", "QXR", NULL, 0);
	g_assert_cmpstr(name.folder, ==, "Hearth (1995) {tmdb-6001}");
	g_assert_cmpstr(name.file, ==, "Hearth (1995) {tmdb-6001} [BluRay 1080p]-QXR.mkv");
	assert_name_sound(&name, AXIS_IMPORT_NAME_BUDGET);
	axis_import_name_clear(&name);

	/* Every token, the default format. */
	memset(&input, 0, sizeof(input));
	input.title = "Aurélie";
	input.year = 2001;
	input.tmdb_id = "6022";
	input.edition = "Director's Cut";
	input.quality = "BluRay 2160p";
	input.hdr = "HDR10";
	input.audio = "DTS-HD MA 5.1";
	input.codec = "x265";
	input.group = "ZPLNKT";
	input.extension = ".MKV";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Aurélie (2001) {tmdb-6022}");
	g_assert_cmpstr(name.file, ==, "Aurélie (2001) {tmdb-6022} {edition-Director's Cut} [BluRay 2160p][HDR10]"
		"[DTS-HD MA 5.1][x265]-ZPLNKT.mkv");
	assert_name_sound(&name, AXIS_IMPORT_NAME_BUDGET);
	axis_import_name_clear(&name);

	/* The other layouts, and an IMDb identifier when there is no TMDb one. */
	input.edition = NULL;
	input.hdr = input.audio = input.codec = NULL;
	input.layout = AXIS_IMPORT_LAYOUT_JELLYFIN;
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Aurélie (2001) [tmdbid-6022]");
	axis_import_name_clear(&name);
	input.layout = AXIS_IMPORT_LAYOUT_PLAIN;
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Aurélie (2001)");
	g_assert_cmpstr(name.file, ==, "Aurélie (2001) [BluRay 2160p]-ZPLNKT.mkv");
	axis_import_name_clear(&name);
	input.layout = AXIS_IMPORT_LAYOUT_TMDB;
	input.tmdb_id = NULL;
	input.imdb_id = "tt0690000";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Aurélie (2001) {imdb-tt0690000}");
	axis_import_name_clear(&name);
	/* Malformed identifiers are left out, not trusted. */
	input.tmdb_id = "94/9";
	input.imdb_id = "nm123";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Aurélie (2001)");
	axis_import_name_clear(&name);

	/* Absent fields are left out with what surrounds them. */
	name = build("Hearth", 1995, "6001", NULL, NULL, NULL, 0);
	g_assert_cmpstr(name.file, ==, "Hearth (1995) {tmdb-6001}.mkv");
	axis_import_name_clear(&name);
	name = build("Hearth", 0, NULL, NULL, "GRP", NULL, 0);
	g_assert_cmpstr(name.folder, ==, "Hearth");
	g_assert_cmpstr(name.file, ==, "Hearth-GRP.mkv");
	axis_import_name_clear(&name);
}

static void
test_editions(void)
{
	AxisImportName name = build("Steel Drifter", 1982, "58", "BluRay 1080p", NULL, "Final Cut", 0);
	g_autofree gchar *long_edition = g_strnfill(60, 'E');
	gchar *at;

	g_assert_cmpstr(name.file, ==, "Steel Drifter (1982) {tmdb-58} {edition-Final Cut} [BluRay 1080p].mkv");
	axis_import_name_clear(&name);
	/* Plex reads at most 32 characters; braces would end its mark early. */
	name = build("Steel Drifter", 1982, "58", NULL, NULL, long_edition, 0);
	at = strstr(name.file, "{edition-");
	g_assert_nonnull(at);
	g_assert_cmpuint(strcspn(at + 9, "}"), ==, 32);
	axis_import_name_clear(&name);
	name = build("Steel Drifter", 1982, "58", NULL, NULL, "Cut {x}", 0);
	assert_contains(name.file, "{edition-Cut x}");
	assert_name_sound(&name, AXIS_IMPORT_NAME_BUDGET);
	axis_import_name_clear(&name);
	/* The edition words a release states, in the one form. */
	{
		g_autofree gchar *text = axis_import_edition_text(AXIS_RELEASE_EDITION_DIRECTORS | AXIS_RELEASE_EDITION_IMAX);

		g_assert_cmpstr(text, ==, "Director's Cut IMAX");
	}
	g_assert_null(axis_import_edition_text(0));
}

static void
test_character_rule(void)
{
	static const struct
	{
		const gchar *in;
		const gchar *out;
	} cases[] = {
		{ "QR/ST: Let There Be Noise", "QR-ST - Let There Be Noise" },
		{ "Back\\Slash", "Back-Slash" },
		{ "Mission: Improbable", "Mission - Improbable" },
		{ "Trailing dot.", "Trailing dot" },
		{ "Trailing spaces and dots . . ", "Trailing spaces and dots" },
		{ ".hidden", "hidden" },
		{ "...", "" },
		{ "..", "" },
		{ ".", "" },
		{ "Tab\tand\nnewline\x7f", "Tabandnewline" },
		{ "Right\xe2\x80\xaeto\xe2\x80\x8fleft\xe2\x81\xa6marks", "Righttoleftmarks" },
		{ "-Leading dash", "-Leading dash" },
		{ "?!*<>|\"", "?!*<>|\"" },
		{ "Café Bébé", "Café Bébé" },
		{ "Bad \xff byte", "Bad \xef\xbf\xbd byte" },
		{ "C1\xc2\x85" "control", "C1control" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *clean = axis_import_name_clean(cases[i].in, -1);

		if (strcmp(clean, cases[i].out) != 0)
			g_error("clean('%s') = '%s', expected '%s'", cases[i].in, clean, cases[i].out);
	}
	/* A NUL in the middle is dropped, not an end. */
	{
		g_autofree gchar *clean = axis_import_name_clean("a\0b", 3);

		g_assert_cmpstr(clean, ==, "ab");
	}
}

static void
test_hostile_titles(void)
{
	static const gchar *const titles[] = {
		"../../etc/passwd", "/", "\\", "..", ".", "...", "-rf", "--", " ", "\t\n", "\xe2\x80\xae", "?",
		"A/B/C", "\xff\xfe", "Title.", "Title ", ".Title", "AC/DC: Back In Black", "{tmdb-1}", "[x]"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(titles); i++) {
		AxisImportName name = build(titles[i], 2000, "1", "WEB-DL 1080p", "G/R:P", "Edi/tion", 0);

		assert_name_sound(&name, AXIS_IMPORT_NAME_BUDGET);
		g_assert_null(strchr(name.folder, '/'));
		g_assert_null(strchr(name.file, '/'));
		g_assert_cmpstr(name.folder, !=, "..");
		g_assert_true(name.folder[0] != '.');
		axis_import_name_clear(&name);
	}
	/* Nothing left of the title: a name all the same. */
	{
		AxisImportName name = build("...", 2000, NULL, NULL, NULL, NULL, 0);

		g_assert_cmpstr(name.folder, ==, "Untitled (2000)");
		axis_import_name_clear(&name);
	}
	{
		AxisImportName name = build("QR/ST: Let There Be Noise", 1980, "1", NULL, NULL, NULL, 0);

		g_assert_cmpstr(name.folder, ==, "QR-ST - Let There Be Noise (1980) {tmdb-1}");
		axis_import_name_clear(&name);
	}
}

static void
test_budget(void)
{
	g_autoptr(GString) title = g_string_new(NULL);
	AxisImportName name;
	guint i;

	/* A title too long by itself: cut, never to nothing, the year and
	 * the identifier kept. */
	for (i = 0; i < 400; i++)
		g_string_append_c(title, 'a' + (gchar)(i % 26));
	name = build(title->str, 2001, "123456", "WEB-DL 1080p", "GROUP", "Extended", 0);
	assert_name_sound(&name, AXIS_IMPORT_NAME_BUDGET);
	g_assert_true(name.shortened);
	g_assert_true(g_str_has_suffix(name.folder, " (2001) {tmdb-123456}"));
	/* The bracketed part went before the title was cut. */
	g_assert_null(strstr(name.file, "[WEB-DL"));
	g_assert_null(strstr(name.file, "edition"));
	axis_import_name_clear(&name);

	/* Multi-byte text: never cut inside a character, at any budget. */
	g_string_truncate(title, 0);
	for (i = 0; i < 120; i++)
		g_string_append(title, i % 3 == 0 ? "東" : i % 3 == 1 ? "é" : "\xf0\x9f\x8e\xac");
	for (i = 60; i <= AXIS_IMPORT_COMPONENT_MAX; i++) {
		name = build(title->str, 1999, "42", "BluRay 1080p", "GRP", NULL, i);
		assert_name_sound(&name, i);
		axis_import_name_clear(&name);
	}
	/* Combining marks are not cut off their letter. */
	g_string_truncate(title, 0);
	for (i = 0; i < 100; i++)
		g_string_append(title, "e\xcc\x81");
	for (i = 60; i < 120; i++) {
		gsize length;

		name = build(title->str, 1999, NULL, NULL, NULL, NULL, i);
		assert_name_sound(&name, i);
		length = strlen(name.folder) - strlen(" (1999)");
		/* Whole "e" + U+0301 pairs only. */
		g_assert_cmpuint(length % 3, ==, 0);
		axis_import_name_clear(&name);
	}

	/* The bracketed part first: the edition is the last of the tokens. */
	name = build("A title of a decent length for the test", 2010, "77", "WEB-DL 1080p", "GROUP", "Extended", 90);
	assert_name_sound(&name, 90);
	assert_contains(name.file, "{edition-Extended}");
	g_assert_null(strstr(name.file, "-GROUP"));
	axis_import_name_clear(&name);

	/* A budget too small for the year and identifier with one character. */
	{
		AxisImportNameInput input;
		const gchar *problem = NULL;

		memset(&input, 0, sizeof(input));
		input.title = "Hearth";
		input.year = 1995;
		input.tmdb_id = "6001";
		input.extension = ".mkv";
		input.budget = 20;
		g_assert_false(axis_import_name_build(&input, &name, &problem));
		g_assert_nonnull(problem);
	}
}

static void
test_keep_and_extension(void)
{
	AxisImportNameInput input;
	AxisImportName name;
	const gchar *problem = NULL;

	memset(&input, 0, sizeof(input));
	input.title = "Hearth";
	input.year = 1995;
	input.tmdb_id = "6001";
	input.extension = ".mkv";
	input.keep = "hearth.1995.1080p.bluray.x264-grp.MKV";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Hearth (1995) {tmdb-6001}");
	g_assert_cmpstr(name.file, ==, "hearth.1995.1080p.bluray.x264-grp.mkv");
	axis_import_name_clear(&name);
	/* A kept name with nothing left of it is named after the folder. */
	input.keep = "...mkv";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_true(axis_import_name_valid(name.file));
	axis_import_name_clear(&name);
	/* An extension that is not one is refused. */
	input.keep = NULL;
	input.extension = ".m/v";
	g_assert_false(axis_import_name_build(&input, &name, &problem));
	input.extension = "";
	g_assert_false(axis_import_name_build(&input, &name, &problem));
}

static void
test_format(void)
{
	static const struct
	{
		const gchar *format;
		gboolean     ok;
	} cases[] = {
		{ AXIS_IMPORT_DEFAULT_FORMAT, TRUE },
		{ "", TRUE },
		{ " - {quality}", TRUE },
		{ "{[quality]}{(group)}", TRUE },
		{ "{nope}", FALSE },
		{ "{quality", FALSE },
		{ "quality}", FALSE },
		{ "{{quality}}", FALSE },
		{ "{quality}/x", FALSE },
		{ "a\\b", FALSE },
		{ "\x01", FALSE },
		{ "\xff", FALSE },
		{ "{[quality]x}", FALSE },
		{ "{Quality}", FALSE }
	};
	g_autofree gchar *long_format = g_strnfill(AXIS_IMPORT_FORMAT_MAX + 1, 'x');
	AxisImportNameInput input;
	AxisImportName name;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *reason = NULL;
		gboolean ok = axis_import_format_check(cases[i].format, &reason);

		if (ok != cases[i].ok)
			g_error("format '%s': expected %s", cases[i].format, cases[i].ok ? "valid" : "refused");
		g_assert_true(ok || reason != NULL);
	}
	g_assert_false(axis_import_format_check(long_format, NULL));

	memset(&input, 0, sizeof(input));
	input.title = "Hearth";
	input.year = 1995;
	input.extension = ".mkv";
	input.quality = "WEB-DL 1080p";
	input.format = " - {quality}{ (group)}";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.file, ==, "Hearth (1995) - WEB-DL 1080p.mkv");
	axis_import_name_clear(&name);
	input.format = "";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.file, ==, "Hearth (1995).mkv");
	axis_import_name_clear(&name);
}

static void
test_preview(void)
{
	g_autoptr(GPtrArray) previews = NULL;
	g_autofree gchar *problem = NULL;
	const AxisImportPreview *hearth;
	guint i;

	g_assert_true(axis_import_preview(TRUE, "tmdb", AXIS_IMPORT_DEFAULT_FORMAT, &previews, &problem));
	g_assert_cmpuint(previews->len, ==, 3);
	hearth = g_ptr_array_index(previews, 0);
	g_assert_cmpstr(hearth->folder, ==, "Hearth (1995) {tmdb-6001}");
	g_assert_cmpstr(hearth->file, ==, "Hearth (1995) {tmdb-6001} {edition-Remastered} [BluRay 1080p][DTS-HD MA 5.1][x264]"
		"-QXR.mkv");
	for (i = 0; i < previews->len; i++) {
		const AxisImportPreview *p = g_ptr_array_index(previews, i);

		g_assert_true(axis_import_name_valid(p->folder));
		g_assert_true(axis_import_name_valid(p->file));
		g_assert_true(g_str_has_prefix(p->file, p->folder));
	}
	/* ':' in the third sample's title. */
	assert_contains(((AxisImportPreview *)g_ptr_array_index(previews, 2))->folder,
		"Mission - Improbable - Cold Reckoning Part One (2023)");
	g_clear_pointer(&previews, g_ptr_array_unref);
	g_assert_false(axis_import_preview(TRUE, "tmdb", "{nope}", &previews, &problem));
	g_assert_nonnull(problem);
	g_clear_pointer(&problem, g_free);
	/* Renaming off keeps the release's file name in the folder. */
	g_assert_true(axis_import_preview(FALSE, "plain", AXIS_IMPORT_DEFAULT_FORMAT, &previews, &problem));
	hearth = g_ptr_array_index(previews, 0);
	g_assert_cmpstr(hearth->folder, ==, "Hearth (1995)");
	g_assert_cmpstr(hearth->file, ==, "hearth.1995.remastered.1080p.bluray.x264-qxr.mkv");
}

/* ── What a found name is ───────────────────────────────────────── */

static void
test_found_names(void)
{
	const gchar *extra = NULL;
	const gchar *type = NULL;
	gint part = 0;

	g_assert_cmpint(axis_import_folder_kind("Sample", 6, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("SAMPLES", 7, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("Proof", 5, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("Screens", 7, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("Extras", 6, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind(".hidden", 7, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("_UNPACK_Hearth", 14, NULL), ==, AXIS_IMPORT_FOLDER_IGNORED);
	g_assert_cmpint(axis_import_folder_kind("behind the scenes", 17, &extra), ==, AXIS_IMPORT_FOLDER_EXTRA);
	g_assert_cmpstr(extra, ==, "Behind The Scenes");
	g_assert_cmpint(axis_import_folder_kind("Trailers", 8, &extra), ==, AXIS_IMPORT_FOLDER_EXTRA);
	g_assert_cmpstr(extra, ==, "Trailers");
	g_assert_cmpint(axis_import_folder_kind("Subs", 4, NULL), ==, AXIS_IMPORT_FOLDER_SUBS);
	g_assert_cmpint(axis_import_folder_kind("Hearth.1995.1080p", 17, NULL), ==, AXIS_IMPORT_FOLDER_PLAIN);

	g_assert_cmpint(axis_import_file_kind("a.MKV", 5), ==, AXIS_IMPORT_FILE_VIDEO);
	g_assert_cmpint(axis_import_file_kind("a.srt", 5), ==, AXIS_IMPORT_FILE_SUBTITLE);
	g_assert_cmpint(axis_import_file_kind("a.rar", 5), ==, AXIS_IMPORT_FILE_ARCHIVE);
	g_assert_cmpint(axis_import_file_kind("a.r07", 5), ==, AXIS_IMPORT_FILE_ARCHIVE);
	g_assert_cmpint(axis_import_file_kind("a.002", 5), ==, AXIS_IMPORT_FILE_ARCHIVE);
	g_assert_cmpint(axis_import_file_kind("a.nfo", 5), ==, AXIS_IMPORT_FILE_OTHER);
	g_assert_cmpint(axis_import_file_kind("._a.mkv", 7), ==, AXIS_IMPORT_FILE_IGNORED);
	g_assert_cmpint(axis_import_file_kind("Thumbs.db", 9), ==, AXIS_IMPORT_FILE_IGNORED);
	g_assert_cmpint(axis_import_file_kind("noextension", 11), ==, AXIS_IMPORT_FILE_OTHER);

	g_assert_true(axis_import_extra_suffix("Hearth-trailer.mkv", 18, &type));
	g_assert_cmpstr(type, ==, "trailer");
	g_assert_true(axis_import_extra_suffix("Making Of-Featurette.mp4", 24, &type));
	g_assert_cmpstr(type, ==, "featurette");
	g_assert_false(axis_import_extra_suffix("Trailer Yard Lads.mkv", 21, &type));
	g_assert_false(axis_import_extra_suffix("-trailer.mkv", 12, &type));

	/* A sample by its name, but not by the title's own words. */
	g_assert_true(axis_import_sample_marked("sample.mkv", 10, "Hearth"));
	g_assert_true(axis_import_sample_marked("hearth.1995.1080p-sample.mkv", 28, "Hearth"));
	g_assert_true(axis_import_sample_marked("Hearth.SAMPLE.mkv", 17, "Hearth"));
	g_assert_false(axis_import_sample_marked("Sample.2019.1080p.WEB-DL.mkv", 28, "Sample"));
	g_assert_true(axis_import_sample_marked("Sample.2019.1080p.WEB-DL-sample.mkv", 35, "Sample"));
	g_assert_false(axis_import_sample_marked("The.Sampler.2020.mkv", 20, "Hearth"));
	g_assert_false(axis_import_sample_marked("Hearth.1995.mkv", 15, NULL));

	g_assert_true(axis_import_part_marked("Film.CD1.avi", 12, &part));
	g_assert_cmpint(part, ==, 1);
	g_assert_true(axis_import_part_marked("Film Part 2.avi", 15, &part));
	g_assert_cmpint(part, ==, 2);
	g_assert_true(axis_import_part_marked("film-disc3.mkv", 14, &part));
	g_assert_false(axis_import_part_marked("Partwell.1999.mkv", 17, &part));
	g_assert_false(axis_import_part_marked("Dunmore.Part.Two.2024.mkv", 25, &part));
}

static void
test_subtitles(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *language;
		gboolean     forced;
		gboolean     sdh;
		const gchar *extension;
	} cases[] = {
		{ "Hearth.1995.en.srt", "en", FALSE, FALSE, ".srt" },
		{ "Hearth.1995.eng.forced.srt", "en", TRUE, FALSE, ".srt" },
		{ "Hearth.1995.English.SDH.ass", "en", FALSE, TRUE, ".ass" },
		{ "Hearth.1995.forced.fre.srt", "fr", TRUE, FALSE, ".srt" },
		{ "Hearth.1995.hi.srt", "hi", FALSE, FALSE, ".srt" },
		{ "Hearth.1995.en.hi.srt", "en", FALSE, TRUE, ".srt" },
		{ "Hearth.1995.pt-BR.srt", "pt", FALSE, FALSE, ".srt" },
		{ "English.srt", "en", FALSE, FALSE, ".srt" },
		{ "Hearth.1995.CC.en.srt", "en", FALSE, TRUE, ".srt" },
		{ "Hearth.1995.xyz.srt", NULL, FALSE, FALSE, ".srt" },
		{ "Hearth.1995.idx", NULL, FALSE, FALSE, ".idx" },
		{ "Hearth 1995 [ger] default.sup", "de", FALSE, FALSE, ".sup" },
		{ "x.vtt", NULL, FALSE, FALSE, ".vtt" }
	};
	AxisImportSubtitle subtitle;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_assert_true(axis_import_subtitle_read(cases[i].name, strlen(cases[i].name), &subtitle));
		if (g_strcmp0(subtitle.language, cases[i].language) != 0 || subtitle.forced != cases[i].forced ||
		    subtitle.sdh != cases[i].sdh || g_strcmp0(subtitle.extension, cases[i].extension) != 0)
			g_error("subtitle '%s': got %s forced=%d sdh=%d %s", cases[i].name, subtitle.language,
				subtitle.forced, subtitle.sdh, subtitle.extension);
	}
	g_assert_false(axis_import_subtitle_read("Hearth.mkv", 10, &subtitle));
	g_assert_false(axis_import_subtitle_read("srt", 3, &subtitle));

	{
		AxisImportSubtitle s = { "en", TRUE, TRUE, ".srt" };
		AxisImportSubtitle u = { NULL, FALSE, FALSE, ".ass" };
		g_autofree gchar *a = axis_import_subtitle_name("Hearth (1995) {tmdb-6001}", &s, 0);
		g_autofree gchar *b = axis_import_subtitle_name("Hearth (1995) {tmdb-6001}", &u, 0);
		g_autofree gchar *c = axis_import_subtitle_name("Hearth (1995) {tmdb-6001}", &u, 2);
		g_autofree gchar *long_stem = g_strnfill(250, 'x');
		g_autofree gchar *d = axis_import_subtitle_name(long_stem, &s, 7);

		g_assert_cmpstr(a, ==, "Hearth (1995) {tmdb-6001}.en.forced.sdh.srt");
		g_assert_cmpstr(b, ==, "Hearth (1995) {tmdb-6001}.und.1.ass");
		g_assert_cmpstr(c, ==, "Hearth (1995) {tmdb-6001}.und.2.ass");
		g_assert_cmpuint(strlen(d), <=, 255);
		g_assert_true(g_str_has_suffix(d, ".en.forced.sdh.7.srt"));
	}
	{
		g_autofree gchar *e = axis_import_extra_name("Hearth: Making Of-featurette.mkv", 32);

		g_assert_cmpstr(e, ==, "Hearth - Making Of-featurette.mkv");
		g_assert_null(axis_import_extra_name("not an extra.mkv", 16));
	}
}

/* ── 6.5 The decisions ──────────────────────────────────────────── */

typedef struct
{
	GPtrArray          *found;
	AxisImportTarget   *target;
	AxisImportSettings  settings;
	AxisImportExpect    expect;
	AxisImportVerdict   verdict;
	AxisRelease        *grab;
	gint                next;
} Case;

static gboolean
fake_name_matches(
	gpointer                data,
	const AxisImportTarget *target,
	const AxisRelease      *parsed,
	gchar                 **reason
){
	g_autofree gchar *a = axis_release_title_key(parsed->title);
	g_autofree gchar *b = axis_release_title_key(target->title);

	if (g_strcmp0(a, b) == 0)
		return TRUE;
	*reason = g_strdup_printf("it says \"%s\"", parsed->title);
	return FALSE;
}

static const AxisImportOwnerInfo fake_owner = {
	"movies", "film", NULL, fake_name_matches, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static void
case_init(
	Case        *c,
	const gchar *title,
	gint         year,
	gint         runtime,
	const gchar *grab
){
	memset(c, 0, sizeof(*c));
	c->found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_import_found_free);
	c->target = axis_import_target_new();
	c->target->title = g_strdup(title);
	c->target->year = year;
	c->target->runtime_minutes = runtime;
	c->target->label = g_strdup_printf("%s (%d)", title, year);
	c->target->grab_release = g_strdup(grab);
	c->target->profile_ok = axis_quality_builtin_profile("anything", &c->target->profile);
	c->settings.subtitles = TRUE;
	c->settings.extras = TRUE;
	c->settings.runtime_tolerance = 25;
	c->grab = grab != NULL ? axis_release_parse(grab) : NULL;
	c->expect.target = c->target;
	c->expect.owner = &fake_owner;
	c->expect.settings = &c->settings;
	c->expect.grab = c->grab;
	c->expect.protocol = AXIS_DOWNLOAD_TORRENT;
	c->expect.chosen = -1;
	c->expect.now_us = g_get_real_time();
}

static AxisImportFound *
add(
	Case                *c,
	AxisImportFoundType  type,
	const gchar         *dir,
	const gchar         *name,
	guint64              size,
	gint                 minutes
){
	AxisImportFound *f = g_new0(AxisImportFound, 1);
	AxisImportFolder folder = AXIS_IMPORT_FOLDER_PLAIN;
	const gchar *extra = NULL;

	if (dir[0] != '\0')
		folder = axis_import_folder_kind(dir, strlen(dir), &extra);
	f->position = c->next++;
	f->type = type;
	f->kind = axis_import_file_kind(name, strlen(name));
	f->dir = g_strdup(dir);
	f->name = g_strdup(name);
	f->path = dir[0] != '\0' ? g_strconcat(dir, "/", name, NULL) : g_strdup(name);
	f->size = size;
	f->inode = 1000 + (guint64)f->position;
	f->folder = folder;
	f->extra_folder = extra;
	if (type == AXIS_IMPORT_FOUND_FILE && f->kind == AXIS_IMPORT_FILE_VIDEO) {
		f->probed = TRUE;
		f->probe_ok = minutes >= 0;
		if (f->probe_ok) {
			f->duration_ms = (gint64)minutes * 60000;
			f->video_streams = 1;
			f->width = 1920;
			f->height = 800;
			f->video_codec = g_strdup("h264");
		} else {
			f->probe_text = g_strdup("ffprobe could not read the file: Invalid data.");
		}
	}
	g_ptr_array_add(c->found, f);
	return f;
}

static void
decide(
	Case *c,
	gint  attempts
){
	axis_import_verdict_clear(&c->verdict);
	axis_import_decide(&c->expect, c->found, attempts, &c->verdict);
}

static void
case_clear(Case *c)
{
	axis_import_verdict_clear(&c->verdict);
	g_ptr_array_unref(c->found);
	axis_import_target_free(c->target);
	axis_release_free(c->grab);
}

static gboolean
has_reason(
	const AxisImportFound *f,
	const gchar           *code
){
	guint i;

	for (i = 0; f->reasons != NULL && i < f->reasons->len; i++) {
		if (g_strcmp0(((AxisImportReason *)g_ptr_array_index(f->reasons, i))->code, code) == 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean
blocked_by(
	const AxisImportVerdict *verdict,
	const gchar             *code
){
	guint i;

	for (i = 0; i < verdict->blockers->len; i++) {
		if (g_strcmp0(((AxisImportReason *)g_ptr_array_index(verdict->blockers, i))->code, code) == 0)
			return TRUE;
	}
	return FALSE;
}

#define GIB (G_GUINT64_CONSTANT(1) << 30)

static void
test_decide_film_and_sample(void)
{
	Case c;
	AxisImportFound *film;
	AxisImportFound *sample;
	AxisImportFound *folder;
	AxisImportFound *sub;
	AxisImportFound *nfo;

	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	film = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", 12 * GIB, 171);
	sample = add(&c, AXIS_IMPORT_FOUND_FILE, "", "hearth.1995.1080p-sample.mkv", 50000000, 1);
	folder = add(&c, AXIS_IMPORT_FOUND_FOLDER, "", "Sample", 0, 0);
	folder->folder_note = "a folder Axis leaves out (samples, proofs, screens, generic extras): not looked into";
	sub = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.BluRay.x264-GRP.en.forced.srt", 50000, 0);
	nfo = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.nfo", 100, 0);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(c.verdict.film == film);
	g_assert_true(film->accepted);
	g_assert_cmpstr(film->role, ==, "film");
	g_assert_true(has_reason(sample, "sample"));
	g_assert_false(sample->accepted);
	g_assert_true(has_reason(folder, "folder"));
	g_assert_cmpstr(sub->role, ==, "subtitle");
	g_assert_true(sub->accepted);
	g_assert_true(has_reason(nfo, "other"));
	g_assert_false(nfo->accepted);
	g_assert_cmpint(c.verdict.quality, ==, AXIS_QUALITY_BLURAY_1080P);
	assert_contains(c.verdict.quality_text, "the resolution from the picture (1920x800)");
	case_clear(&c);

	/* A film called Sample is not a sample, short or not. */
	case_init(&c, "Sample", 2019, 9, "Sample.2019.1080p.WEB-DL.H264-GRP");
	film = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Sample.2019.1080p.WEB-DL.H264-GRP.mkv", 400000000, 9);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(c.verdict.film == film);
	g_assert_false(has_reason(film, "sample"));
	case_clear(&c);

	/* A file the probe cannot read: tried again, then a person asks
	 * (overridable). */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	film = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", 12 * GIB, -1);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(c.verdict.retry_probe);
	g_assert_cmpstr(c.verdict.code, ==, "probe_retry");
	g_assert_false(film->accepted);
	decide(&c, AXIS_IMPORT_PROBE_ATTEMPTS);
	g_assert_true(c.verdict.blocked);
	g_assert_true(blocked_by(&c.verdict, "unreadable"));
	g_assert_true(((AxisImportReason *)g_ptr_array_index(c.verdict.blockers, 0))->overridable);
	case_clear(&c);
}

static void
test_decide_blocks(void)
{
	Case c;
	AxisImportFound *a;
	AxisImportFound *b;
	gchar *overrides[] = { (gchar *)"runtime", (gchar *)"wrong_name", NULL };

	/* Several films. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	a = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.mkv", 12 * GIB, 170);
	b = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Rokan.1998.1080p.mkv", 11 * GIB, 122);
	decide(&c, 0);
	g_assert_true(c.verdict.blocked);
	g_assert_true(blocked_by(&c.verdict, "several"));
	g_assert_true(has_reason(a, "several") && has_reason(b, "several"));
	g_assert_null(c.verdict.film);
	/* A person chooses: that file is the film (and its name is checked). */
	c.expect.chosen = a->position;
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(c.verdict.film == a);
	case_clear(&c);

	/* An archive left packed. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "hearth.rar", 50000000, 0);
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "hearth.r00", 50000000, 0);
	add(&c, AXIS_IMPORT_FOUND_FOLDER, "", "Sample", 0, 0);
	decide(&c, 0);
	g_assert_true(blocked_by(&c.verdict, "archive"));
	g_assert_false(((AxisImportReason *)g_ptr_array_index(c.verdict.blockers, 0))->overridable);
	case_clear(&c);

	/* Nothing at all. */
	case_init(&c, "Hearth", 1995, 170, NULL);
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "readme.txt", 10, 0);
	decide(&c, 0);
	g_assert_true(blocked_by(&c.verdict, "no_video"));
	case_clear(&c);

	/* A name that reads as another film, and a runtime far off. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	a = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Rokan.1998.1080p.BluRay.x264-GRP.mkv", 12 * GIB, 52);
	decide(&c, 0);
	g_assert_true(c.verdict.blocked);
	g_assert_true(blocked_by(&c.verdict, "wrong_name"));
	g_assert_true(blocked_by(&c.verdict, "runtime"));
	g_assert_true(has_reason(a, "wrong_name") && has_reason(a, "runtime"));
	g_assert_cmpstr(c.verdict.code, ==, "several_reasons");
	/* Import anyway overrides both. */
	c.expect.overrides = overrides;
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(a->accepted);
	case_clear(&c);

	/* A name with no year, or an obfuscated one, says nothing. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "a8f3c2e91b0d4f6a7c5e3b2d1f0a9c8e.mkv", 12 * GIB, 171);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	case_clear(&c);

	/* A film in two parts. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.DVDRip.XviD-GRP");
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "hearth.cd1.avi", 700000000, 85);
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "hearth.cd2.avi", 700000000, 85);
	decide(&c, 0);
	g_assert_true(blocked_by(&c.verdict, "multi_part"));
	case_clear(&c);
}

static void
test_decide_extras(void)
{
	Case c;
	AxisImportFound *film;
	AxisImportFound *trailer;
	AxisImportFound *featurette;
	AxisImportFound *intro;
	AxisImportFound *extra_sub;

	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	film = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", 12 * GIB, 171);
	trailer = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth-trailer.mkv", 90000000, 2);
	featurette = add(&c, AXIS_IMPORT_FOUND_FILE, "Featurettes", "Making Hearth.mkv", 900000000, 25);
	intro = add(&c, AXIS_IMPORT_FOUND_FILE, "", "intro.mkv", 30000000, 2);
	extra_sub = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth-trailer.en.srt", 1000, 0);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_true(c.verdict.film == film);
	g_assert_cmpstr(trailer->role, ==, "extra");
	g_assert_true(trailer->accepted);
	g_assert_cmpstr(trailer->dest_name, ==, "Hearth-trailer.mkv");
	g_assert_null(trailer->dest_subdir);
	g_assert_cmpstr(featurette->role, ==, "extra");
	g_assert_cmpstr(featurette->dest_subdir, ==, "Featurettes");
	g_assert_cmpstr(featurette->dest_name, ==, "Making Hearth.mkv");
	g_assert_true(has_reason(intro, "short"));
	g_assert_false(intro->accepted);
	g_assert_null(extra_sub->role);
	case_clear(&c);

	/* Extras off: not imported, and said so. */
	case_init(&c, "Hearth", 1995, 170, "Hearth.1995.1080p.BluRay.x264-GRP");
	c.settings.extras = FALSE;
	film = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", 12 * GIB, 171);
	trailer = add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth-trailer.mkv", 90000000, 2);
	decide(&c, 0);
	g_assert_false(c.verdict.blocked);
	g_assert_false(trailer->accepted);
	g_assert_true(has_reason(trailer, "extra"));
	case_clear(&c);
}

static void
test_decide_quality(void)
{
	Case c;
	AxisImportExisting *existing;

	/* Not allowed by the profile. */
	case_init(&c, "Hearth", 1995, 0, "Hearth.1995.720p.HDTV.x264-GRP");
	axis_quality_builtin_profile("balanced", &c.target->profile);
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.720p.HDTV.x264-GRP.mkv", 3 * GIB, 170);
	((AxisImportFound *)g_ptr_array_index(c.found, 0))->width = 1280;
	((AxisImportFound *)g_ptr_array_index(c.found, 0))->height = 720;
	decide(&c, 0);
	g_assert_true(blocked_by(&c.verdict, "quality"));
	case_clear(&c);

	/* Not better than what is in the library. */
	case_init(&c, "Hearth", 1995, 0, "Hearth.1995.1080p.WEB-DL.x264-GRP");
	existing = g_new0(AxisImportExisting, 1);
	existing->quality = AXIS_QUALITY_BLURAY_1080P;
	existing->release_name = g_strdup("Hearth.1995.1080p.BluRay.x264-OLD");
	existing->rel_path = g_strdup("Hearth (1995)/Hearth.mkv");
	g_ptr_array_add(c.target->existing, existing);
	add(&c, AXIS_IMPORT_FOUND_FILE, "", "Hearth.1995.1080p.WEB-DL.x264-GRP.mkv", 6 * GIB, 170);
	decide(&c, 0);
	g_assert_true(blocked_by(&c.verdict, "not_upgrade"));
	g_assert_true(c.verdict.upgrade);
	case_clear(&c);
}

/* ── Episodes (add-tv-module 4.2, 6.8) ──────────────────────────── */

static gboolean
episode_name(
	const gchar     *title,
	const gchar     *tvdb,
	const gchar     *tmdb,
	gint             season,
	const gint      *episodes,
	guint            n,
	const gchar     *episode_title,
	gsize            budget,
	AxisImportName  *name
){
	AxisImportNameInput input;
	const gchar *problem = NULL;

	memset(&input, 0, sizeof(input));
	input.title = title;
	input.year = 2021;
	input.tvdb_id = tvdb;
	input.tmdb_id = tmdb;
	input.quality = "WEB-DL 1080p";
	input.group = "GRP";
	input.extension = ".mkv";
	input.budget = budget;
	input.episode = TRUE;
	input.season = season;
	input.episodes = episodes;
	input.n_episodes = n;
	input.episode_title = episode_title;
	return axis_import_name_build(&input, name, &problem);
}

static void
test_episode_names(void)
{
	static const gint one[] = { 2 };
	static const gint two[] = { 2, 3 };
	static const gint apart[] = { 2, 5 };
	static const gint special[] = { 1 };
	AxisImportName name;
	AxisImportNameInput input;
	g_autoptr(GString) long_title = g_string_new(NULL);
	guint i;

	/* The scheme. */
	g_assert_true(episode_name("Fixture Street", "350001", "201", 1, one, 1, "Number Two", 0, &name));
	g_assert_cmpstr(name.folder, ==, "Fixture Street (2021) {tvdb-350001}");
	g_assert_cmpstr(name.season_folder, ==, "Season 01");
	g_assert_cmpstr(name.file, ==, "Fixture Street (2021) - S01E02 - Number Two [WEB-DL 1080p]-GRP.mkv");
	g_assert_cmpstr(name.stem, ==, "Fixture Street (2021) - S01E02 - Number Two [WEB-DL 1080p]-GRP");
	g_assert_false(name.shortened);
	axis_import_name_clear(&name);

	/* No TVDB identifier: TMDb's. */
	g_assert_true(episode_name("Fixture Street", NULL, "201", 1, one, 1, "Number Two", 0, &name));
	g_assert_cmpstr(name.folder, ==, "Fixture Street (2021) {tmdb-201}");
	axis_import_name_clear(&name);

	/* A double episode, and two that do not follow one another. */
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, two, 2, "Part One", 0, &name));
	g_assert_cmpstr(name.file, ==, "Fixture Street (2021) - S01E02-E03 - Part One [WEB-DL 1080p]-GRP.mkv");
	axis_import_name_clear(&name);
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, apart, 2, NULL, 0, &name));
	g_assert_cmpstr(name.file, ==, "Fixture Street (2021) - S01E02E05 [WEB-DL 1080p]-GRP.mkv");
	axis_import_name_clear(&name);

	/* Specials. */
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 0, special, 1, "Christmas", 0, &name));
	g_assert_cmpstr(name.season_folder, ==, "Season 00");
	assert_contains(name.file, " - S00E01 - Christmas ");
	axis_import_name_clear(&name);

	/* The character rule on the episode's title. */
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, one, 1, "Who/What: Where?", 0, &name));
	assert_contains(name.file, " - S01E02 - Who-What - Where? [");
	g_assert_null(strchr(name.file, '/'));
	axis_import_name_clear(&name);

	/* A long episode title is cut first, on a character boundary, and the
	 * rest stays; the budget holds with the temporary name. */
	for (i = 0; i < 60; i++)
		g_string_append(long_title, "Ünïcödé ");
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, one, 1, long_title->str, 0, &name));
	g_assert_true(name.shortened);
	g_assert_cmpuint(strlen(name.file), <=, AXIS_IMPORT_NAME_BUDGET);
	g_assert_cmpuint(strlen(name.file) + AXIS_IMPORT_TEMP_MAX, <=, AXIS_IMPORT_COMPONENT_MAX);
	g_assert_true(g_utf8_validate(name.file, -1, NULL));
	g_assert_true(g_str_has_prefix(name.file, "Fixture Street (2021) - S01E02 - Ünïcödé"));
	g_assert_true(g_str_has_suffix(name.file, " [WEB-DL 1080p]-GRP.mkv"));
	g_assert_true(axis_import_name_valid(name.file));
	axis_import_name_clear(&name);

	/* A budget too small for the episode's title: it goes, then the tokens;
	 * the mark, the year and the extension stay. */
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, two, 2, "Part One", 45, &name));
	g_assert_cmpstr(name.file, ==, "Fixture Street (2021) - S01E02-E03.mkv");
	axis_import_name_clear(&name);
	g_assert_true(episode_name("Fixture Street", "350001", NULL, 1, two, 2, "Part One", 32, &name));
	g_assert_cmpstr(name.folder, ==, "Fixture (2021) {tvdb-350001}");
	g_assert_cmpstr(name.file, ==, "Fixture (2021) - S01E02-E03.mkv");
	axis_import_name_clear(&name);
	g_assert_true(episode_name("Fixture Street", NULL, NULL, 1, two, 2, "Part One", 28, &name));
	g_assert_cmpstr(name.folder, ==, "Fixture Street (2021)");
	g_assert_cmpstr(name.file, ==, "Fixt (2021) - S01E02-E03.mkv");
	axis_import_name_clear(&name);
	g_assert_false(episode_name("Fixture Street", "350001", NULL, 1, two, 2, "Part One", 20, &name));

	/* The Jellyfin layout, the air date token, and renaming off. */
	memset(&input, 0, sizeof(input));
	input.title = "Daily Thing";
	input.year = 2024;
	input.tvdb_id = "77";
	input.layout = AXIS_IMPORT_LAYOUT_JELLYFIN;
	input.format = "{ (airdate)}{ [quality]}";
	input.quality = "HDTV 720p";
	input.air_date = "2024-03-15";
	input.extension = ".MKV";
	input.episode = TRUE;
	input.season = 2024;
	input.episodes = one;
	input.n_episodes = 1;
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.folder, ==, "Daily Thing (2024) [tvdbid-77]");
	g_assert_cmpstr(name.season_folder, ==, "Season 2024");
	g_assert_cmpstr(name.file, ==, "Daily Thing (2024) - S2024E02 (2024-03-15) [HDTV 720p].mkv");
	axis_import_name_clear(&name);
	input.keep = "daily.thing.2024.03.15.720p.hdtv-grp.mkv";
	g_assert_true(axis_import_name_build(&input, &name, NULL));
	g_assert_cmpstr(name.season_folder, ==, "Season 2024");
	g_assert_cmpstr(name.file, ==, "daily.thing.2024.03.15.720p.hdtv-grp.mkv");
	axis_import_name_clear(&name);

	/* Episodes a name cannot state. */
	input.keep = NULL;
	input.n_episodes = 0;
	g_assert_false(axis_import_name_build(&input, &name, NULL));
}

/* ── Duplicates: which file to keep (add-force-grab-and-assign, D) ─ */

static AxisImportDupFile *
dup_file(
	AxisImportDupGroup    *group,
	AxisQuality            quality,
	AxisReleaseResolution  resolution,
	gint64                 size,
	gboolean               kept,
	const gchar           *hand_how
){
	AxisImportDupFile *f = axis_import_dup_file_new();

	f->file->library_file_id = (gint64)group->files->len + 1;
	f->file->rel_path = g_strdup_printf("Hearth (1995)/file-%u.mkv", group->files->len + 1);
	f->file->quality = quality;
	f->file->resolution = resolution;
	f->file->size = size;
	f->file->kept = kept;
	f->hand_how = g_strdup(hand_how);
	f->hand_by = hand_how != NULL ? g_strdup("root") : NULL;
	/* Analysed as it is, an hour and a half; the original, score 0. */
	axis_import_dup_file_set_probe(f, "ok", 90 * 60 * 1000, TRUE);
	f->ranked = TRUE;
	g_ptr_array_add(group->files, f);
	return f;
}

static void
dup_rename(
	AxisImportDupFile *f,
	const gchar       *name
){
	g_free(f->file->rel_path);
	f->file->rel_path = g_strdup_printf("Hearth (1995)/%s", name);
}

/* A profile that prefers WEB-DL 720p to WEB-DL 1080p and BluRay 1080p (the
 * two an equal tier), and allows nothing else. */
static AxisImportDupGroup *
dup_group(gboolean with_profile)
{
	AxisImportDupGroup *group = axis_import_dup_group_new();

	group->label = g_strdup("Hearth (1995)");
	axis_quality_profile_init(&group->profile);
	group->profile.n_allowed = 3;
	group->profile.allowed[0] = AXIS_QUALITY_WEBDL_720P;
	group->profile.tier[0] = 0;
	group->profile.allowed[1] = AXIS_QUALITY_BLURAY_1080P;
	group->profile.tier[1] = 1;
	group->profile.allowed[2] = AXIS_QUALITY_WEBDL_1080P;
	group->profile.tier[2] = 1;
	group->profile_ok = with_profile;
	return group;
}

#define ADVICE(group, i) (((AxisImportDupFile *)g_ptr_array_index((group)->files, (i)))->advice)
#define TICKED(group, i) (((AxisImportDupFile *)g_ptr_array_index((group)->files, (i)))->ticked)
#define WORDS(group, i)  (((AxisImportDupFile *)g_ptr_array_index((group)->files, (i)))->advice_text)

/*
 * add-force-grab-and-assign, slice D; the import delta's "Duplicates are
 * listed with a recommendation": the profile first, then the ladder, then
 * for the same quality the larger file, then the one kept or linked by hand;
 * nothing recommended when Axis cannot tell or cannot see the better file; a
 * file a person keeps or linked is said so and never ticked; a file that is
 * not offered stays.
 */
static void
test_duplicate_recommendation(void)
{
	g_autoptr(AxisImportDupGroup) g = NULL;
	AxisImportDupFile *f;

	/* The profile prefers the 720p file, though the other is higher on the
	 * ladder and larger. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_KEEP);
	g_assert_cmpint(ADVICE(g, 0), ==, AXIS_IMPORT_DUP_REMOVE);
	g_assert_true(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(g->summary, "Axis would keep file-2.mkv"));
	g_assert_nonnull(strstr(g->summary, "the quality profile prefers its quality"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* A quality the profile lists against one it does not: the standard
	 * ladder, as for an upgrade (check_upgrade() falls back to it when either
	 * side is not listed).  Here the ladder agrees with the list. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "its quality is higher"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* And here it does not: a Remux 1080p the profile does not list is still
	 * the better file than the WEB-DL 720p it lists first.  An upgrade would
	 * replace the WEB-DL with the Remux; the page must not say the opposite. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_REMUX_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "its quality is higher"));
	g_assert_null(strstr(g->summary, "allows its quality"));
	g_assert_true(TICKED(g, 0));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* The profile ranks the two the same: the ladder. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "its quality is higher"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* No usable profile: the ladder. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* The same quality: the larger. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "the same quality and larger"));
	g_assert_true(TICKED(g, 0));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* Scenario: Axis cannot tell.  The same quality and size, neither kept:
	 * nothing is recommended and nothing is ticked. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_assert_cmpint(ADVICE(g, 0), ==, AXIS_IMPORT_DUP_NONE);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_NONE);
	g_assert_false(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(g->summary, "Axis cannot tell which file is better"));
	g_assert_nonnull(strstr(g->summary, "the same quality and the same size"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* The same quality and size: the one a person keeps. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, TRUE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_true(TICKED(g, 0));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* Scenario: Kept by hand.  The worse file is one a person linked and
	 * keeps: Axis says it would remove it, says who linked it, and does not
	 * tick it. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, TRUE, "linked");
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_REMOVE_HAND);
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(WORDS(g, 1), "Axis would remove it"));
	g_assert_nonnull(strstr(WORDS(g, 1), "root linked it by hand, and it is kept by hand"));
	g_assert_nonnull(strstr(WORDS(g, 1), "not ticked for you"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* Kept only (an import's file a person keeps), or forced and let go. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, TRUE, NULL);
	dup_file(g, AXIS_QUALITY_SDTV, AXIS_RELEASE_RESOLUTION_NONE, 300, FALSE, "forced");
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_REMOVE_HAND);
	g_assert_cmpint(ADVICE(g, 2), ==, AXIS_IMPORT_DUP_REMOVE_HAND);
	g_assert_false(TICKED(g, 1));
	g_assert_false(TICKED(g, 2));
	g_assert_nonnull(strstr(WORDS(g, 1), "it is kept by hand"));
	g_assert_nonnull(strstr(WORDS(g, 2), "root grabbed it by force"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* A file of which nothing is known cannot be compared. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_assert_false(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(g->summary, "the quality of one of them is not known"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* An unknown source at a known resolution has a place on the ladder. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_720P, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_true(TICKED(g, 0));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* Two unknown sources at one resolution: size says nothing of them. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_1080P, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_1080P, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* A file that is not offered (pinned, a disk that cannot be written)
	 * stays and is never ticked, whatever Axis would recommend. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f = dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	f->fixed = g_strdup("it has another name on its disk");
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_assert_cmpint(ADVICE(g, 0), ==, AXIS_IMPORT_DUP_KEEP);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_STAYS);
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(WORDS(g, 1), "Stays, whatever Axis would recommend: it has another name on its disk"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* The better file is on a disk Axis cannot see: nothing is recommended
	 * on its strength, and the other is not ticked. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f->unseen = TRUE;
	f->fixed = g_strdup("its disk, Disk two, is offline");
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_assert_cmpint(ADVICE(g, 0), ==, AXIS_IMPORT_DUP_STAYS);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_NONE);
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(g->summary, "Axis cannot see it now, so nothing is recommended"));
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* Three files: the one that wins against both; with the top two tied,
	 * none does. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 800, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_true(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_true(TICKED(g, 2));
	g_clear_pointer(&g, axis_import_dup_group_free);
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_assert_false(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_false(TICKED(g, 2));
}

/* Nothing recommended and nothing ticked in @g, and the summary says @why. */
static void
assert_no_advice(
	AxisImportDupGroup *g,
	const gchar        *why
){
	guint i;

	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	if (strstr(g->summary, why) == NULL)
		g_error("expected \"%s\" in: %s", why, g->summary);
	g_assert_nonnull(strstr(g->summary, "Nothing is recommended and nothing is ticked for you"));
	for (i = 0; i < g->files->len; i++) {
		g_assert_false(TICKED(g, i));
		g_assert_cmpint(ADVICE(g, i), !=, AXIS_IMPORT_DUP_REMOVE);
		g_assert_cmpint(ADVICE(g, i), !=, AXIS_IMPORT_DUP_REMOVE_HAND);
		g_assert_cmpint(ADVICE(g, i), !=, AXIS_IMPORT_DUP_KEEP);
	}
}

/*
 * add-force-grab-and-assign, slice D, the review's blocker B2: two files
 * linked to one title need not be copies of it.  The page recommends, and
 * ticks, only when every file has a readable analysis of the file as it is
 * now, the running times agree, and no name states a part of a set; and
 * within one quality it prefers as an upgrade does (the revision, then the
 * score) before the size (L2).
 */
static void
test_duplicate_not_copies(void)
{
	g_autoptr(AxisImportDupGroup) g = NULL;
	AxisImportDupFile *f;

	/* A movie in two parts, both linked: the same quality, the first larger.
	 * Neither is a duplicate of the other. */
	g = dup_group(TRUE);
	f = dup_file(g, AXIS_QUALITY_DVD, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_rename(f, "Hearth (1995) CD1 [DVD].avi");
	axis_import_dup_file_set_probe(f, "ok", 52 * 60 * 1000, TRUE);
	f = dup_file(g, AXIS_QUALITY_DVD, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	dup_rename(f, "Hearth (1995) CD2 [DVD].avi");
	axis_import_dup_file_set_probe(f, "ok", 41 * 60 * 1000, TRUE);
	assert_no_advice(g, "These files run for different lengths, so they may not be copies of the same thing");
	g_assert_nonnull(strstr(g->summary, "runs 52 min"));
	g_assert_nonnull(strstr(g->summary, "runs 41 min"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* The two parts run as long as each other: the names still say parts. */
	g = dup_group(TRUE);
	f = dup_file(g, AXIS_QUALITY_DVD, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_rename(f, "Hearth (1995) CD1 [DVD].avi");
	f = dup_file(g, AXIS_QUALITY_DVD, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	dup_rename(f, "Hearth (1995) CD2 [DVD].avi");
	assert_no_advice(g, "named as parts of a set");
	g_clear_pointer(&g, axis_import_dup_group_free);
	g = dup_group(TRUE);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_rename(f, "Hearth (1995) - Part 1 [WEB-DL 1080p].mkv");
	f = dup_file(g, AXIS_QUALITY_WEBDL_720P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	dup_rename(f, "Hearth (1995) [WEB-DL 720p].mkv");
	assert_no_advice(g, "named as parts of a set");
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* Both name the same part (a sequel called "Part 2"): copies, as far as
	 * the names go. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	dup_rename(f, "Hearth Part 2 (1995) [WEB-DL 1080p].mkv");
	f = dup_file(g, AXIS_QUALITY_WEBDL_720P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	dup_rename(f, "Hearth Part 2 (1995) [WEB-DL 720p].mkv");
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* Running times a little apart (a different cut of the credits) are
	 * copies; more than 5 % of the shorter are not. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "ok", 1000 * 1000, TRUE);
	f = dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "ok", 1050 * 1000, TRUE);
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 0);
	g_assert_true(TICKED(g, 1));
	axis_import_dup_file_set_probe(f, "ok", 1051 * 1000, TRUE);
	assert_no_advice(g, "These files run for different lengths");
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* The file with the better name was cut short and cannot be read: it
	 * does not win against the complete one, and the complete one is not
	 * ticked. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 12, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "failed", 0, TRUE);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	assert_no_advice(g, "Axis could not read one of these files");
	g_assert_nonnull(strstr(g->summary, "file-1.mkv could not be read as a video"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* The worse file is the unreadable one: nothing is recommended either
	 * (it may be the other that is not what its name says). */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f = dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "timeout", 0, TRUE);
	assert_no_advice(g, "Axis could not read one of these files");
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* Never analysed, or analysed before it changed (not current). */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f = dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_file_set_probe(f, NULL, 0, FALSE);
	assert_no_advice(g, "file-2.mkv has not been analysed since it was added or last changed");
	axis_import_dup_file_set_probe(f, "ok", 90 * 60 * 1000, FALSE);
	g_assert_null(f->probe);
	assert_no_advice(g, "file-2.mkv has not been analysed since it was added or last changed");
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* An empty file with the better name, and a readable analysis with no
	 * running time. */
	g = dup_group(FALSE);
	dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 0, FALSE, NULL);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	assert_no_advice(g, "file-1.mkv is empty (0 bytes)");
	g_clear_pointer(&g, axis_import_dup_group_free);
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "ok", 0, TRUE);
	dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	assert_no_advice(g, "file-1.mkv has no running time");
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* A file that is not offered still says so. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_BLURAY_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f->fixed = g_strdup("it has another name on its disk");
	f = dup_file(g, AXIS_QUALITY_HDTV_720P, AXIS_RELEASE_RESOLUTION_NONE, 400, FALSE, NULL);
	axis_import_dup_file_set_probe(f, "failed", 0, TRUE);
	assert_no_advice(g, "Axis could not read one of these files");
	g_assert_cmpint(ADVICE(g, 0), ==, AXIS_IMPORT_DUP_STAYS);
	g_assert_cmpint(ADVICE(g, 1), ==, AXIS_IMPORT_DUP_NONE);
	g_clear_pointer(&g, axis_import_dup_group_free);

	/* Within one quality, as an upgrade: a proper beats the original though
	 * it is smaller; then the score; then the size. */
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	f->revision = 1;
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "a later revision (a proper or repack)"));
	g_assert_true(TICKED(g, 0));
	g_clear_pointer(&g, axis_import_dup_group_free);
	g = dup_group(TRUE);
	dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	f->score = 100;
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_assert_nonnull(strstr(g->summary, "its release scores higher in the quality profile"));
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* The revision before the score. */
	g = dup_group(TRUE);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f->score = 100;
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	f->revision = 1;
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, 1);
	g_clear_pointer(&g, axis_import_dup_group_free);
	/* The revision and the score are not known (no usable profile) and the
	 * sizes differ: the size alone does not say. */
	g = dup_group(FALSE);
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 900, FALSE, NULL);
	f->ranked = FALSE;
	f = dup_file(g, AXIS_QUALITY_WEBDL_1080P, AXIS_RELEASE_RESOLUTION_NONE, 700, FALSE, NULL);
	f->ranked = FALSE;
	axis_import_dup_recommend(g);
	g_assert_cmpint(g->keep, ==, -1);
	g_assert_false(TICKED(g, 0));
	g_assert_false(TICKED(g, 1));
	g_assert_nonnull(strstr(g->summary, "the size alone does not say which is better"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/import/duplicates/recommend", test_duplicate_recommendation);
	g_test_add_func("/import/duplicates/not-copies", test_duplicate_not_copies);
	g_test_add_func("/import/name/scheme", test_scheme);
	g_test_add_func("/import/name/editions", test_editions);
	g_test_add_func("/import/name/character-rule", test_character_rule);
	g_test_add_func("/import/name/hostile-titles", test_hostile_titles);
	g_test_add_func("/import/name/budget", test_budget);
	g_test_add_func("/import/name/keep-and-extension", test_keep_and_extension);
	g_test_add_func("/import/name/format", test_format);
	g_test_add_func("/import/name/preview", test_preview);
	g_test_add_func("/import/name/found-names", test_found_names);
	g_test_add_func("/import/name/subtitles", test_subtitles);
	g_test_add_func("/import/decide/film-and-sample", test_decide_film_and_sample);
	g_test_add_func("/import/decide/blocks", test_decide_blocks);
	g_test_add_func("/import/decide/extras", test_decide_extras);
	g_test_add_func("/import/decide/quality", test_decide_quality);
	g_test_add_func("/import/name/episodes", test_episode_names);
	return g_test_run();
}
