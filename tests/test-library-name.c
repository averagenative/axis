/*
 * test-library-name.c - file names as bytes, kinds by extension, ignored
 * names and patterns
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"

static gchar *
encode(
	const gchar *bytes,
	gboolean    *valid
){
	GString *out = g_string_new(NULL);
	gboolean ok = axis_library_name_encode(out, bytes, strlen(bytes));

	if (valid != NULL)
		*valid = ok;
	return g_string_free(out, FALSE);
}

static void
assert_round_trip(const gchar *bytes)
{
	g_autofree gchar *stored = encode(bytes, NULL);
	GString *back = NULL;

	g_assert_true(g_utf8_validate(stored, -1, NULL));
	g_assert_true(axis_library_name_decode(stored, &back));
	g_assert_cmpuint(back->len, ==, strlen(bytes));
	g_assert_cmpmem(back->str, back->len, bytes, strlen(bytes));
	g_string_free(back, TRUE);
}

/* Spec: Non-UTF-8 name; Hostile-looking name (stored verbatim) */
static void
test_round_trip(void)
{
	static const gchar *const names[] = {
		"Film (2001).mkv",
		"with space",
		"new\nline",
		"tab\there",
		"quote\"s 'and' ;semi;colons;",
		"<script>alert(1)</script>.mkv",
		"-leading-dash",
		"--",
		"back\\slash",
		"\\xFF",			/* text that looks like an escape */
		"\\\\",
		"\xff",				/* a byte that is not UTF-8 */
		"caf\xe9.mkv",			/* Latin-1 */
		"\x80lone continuation",
		"over\xc0\xaflong",
		"surrogate\xed\xa0\x80",
		"cut\xe2\x82",
		"above\xf4\x90\x80\x80max",
		"caf\xc3\xa9",			/* é, NFC */
		"cafe\xcc\x81",			/* é, NFD */
		"\xe2\x80\xae" "fdp.exe",	/* right-to-left override */
		"\x7f\x01\x1b[31m",
		"\xc2\x85next line",
		NULL
	};
	guint i;

	for (i = 0; names[i] != NULL; i++)
		assert_round_trip(names[i]);
}

static void
test_canonical_form(void)
{
	g_autofree gchar *plain = NULL;
	g_autofree gchar *nfc = NULL;
	g_autofree gchar *nfd = NULL;
	g_autofree gchar *upper = NULL;
	g_autofree gchar *lower = NULL;
	g_autofree gchar *literal = NULL;
	g_autofree gchar *byte = NULL;
	g_autofree gchar *slash = NULL;
	gboolean valid = FALSE;

	/* Valid UTF-8 without a backslash is stored as it is. */
	plain = encode("Film (2001)/caf\xc3\xa9 \n.mkv", &valid);
	g_assert_true(valid);
	g_assert_cmpstr(plain, ==, "Film (2001)/caf\xc3\xa9 \n.mkv");

	/* Never normalised, never folded. */
	nfc = encode("caf\xc3\xa9", NULL);
	nfd = encode("cafe\xcc\x81", NULL);
	g_assert_cmpstr(nfc, !=, nfd);
	upper = encode("Film.MKV", NULL);
	lower = encode("film.mkv", NULL);
	g_assert_cmpstr(upper, !=, lower);

	/* The text "\xFF" and the byte 0xff are different names. */
	literal = encode("\\xFF", &valid);
	g_assert_true(valid);
	g_assert_cmpstr(literal, ==, "\\\\xFF");
	byte = encode("\xff", &valid);
	g_assert_false(valid);
	g_assert_cmpstr(byte, ==, "\\xFF");
	g_assert_cmpstr(literal, !=, byte);

	/* '/' is kept: a path's form is its segments' forms joined. */
	slash = encode("a\xff/b\\c", NULL);
	g_assert_cmpstr(slash, ==, "a\\xFF/b\\\\c");

	/* Only the canonical form decodes. */
	g_assert_false(axis_library_name_decode("\\xff", NULL));
	g_assert_false(axis_library_name_decode("\\x41", NULL));
	g_assert_false(axis_library_name_decode("\\q", NULL));
	g_assert_false(axis_library_name_decode("end\\", NULL));
	g_assert_false(axis_library_name_decode("\\x00", NULL));
	g_assert_false(axis_library_name_decode("\\xC3\\xA9", NULL));
	g_assert_false(axis_library_name_decode("raw\xff", NULL));
	g_assert_false(axis_library_name_decode(NULL, NULL));
	g_assert_true(axis_library_name_decode("\\xC3", NULL));
	g_assert_true(axis_library_name_decode("", NULL));

	g_assert_true(axis_library_name_is_invalid("\\xFF"));
	g_assert_false(axis_library_name_is_invalid("\\\\xFF"));
	g_assert_false(axis_library_name_is_invalid("plain"));
}

/* Spec: Hostile-looking name (displayed escaped) */
static void
test_display(void)
{
	g_autofree gchar *line = axis_library_name_display("a\nb\tc\rd");
	g_autofree gchar *bad = axis_library_name_display("caf\\xE9");
	g_autofree gchar *bidi = axis_library_name_display("\xe2\x80\xae" "fdp.exe");
	g_autofree gchar *controls = axis_library_name_display("\x1b[31m\x7f\xc2\x85");
	g_autofree gchar *slash = axis_library_name_display("back\\\\slash");
	g_autofree gchar *broken = axis_library_name_display("\\x41");
	g_autofree gchar *none = axis_library_name_display(NULL);
	g_autofree gchar *exact = axis_library_name_exact("caf\\xE9\n\\\\ \xe2\x80\xae" "x");
	const gchar *p;

	g_assert_cmpstr(line, ==, "a\\nb\\tc\\rd");
	g_assert_cmpstr(bad, ==, "caf\xef\xbf\xbd");
	g_assert_cmpstr(bidi, ==, "\\u{202E}fdp.exe");
	g_assert_cmpstr(controls, ==, "\\x1B[31m\\x7F\\u{0085}");
	g_assert_cmpstr(slash, ==, "back\\slash");
	g_assert_cmpstr(broken, ==, "(unreadable)");
	g_assert_cmpstr(none, ==, "");
	g_assert_cmpstr(exact, ==, "caf\\xE9\\x0A\\\\ \\xE2\\x80\\xAEx");

	for (p = line; *p != '\0'; p++)
		g_assert_cmpint((guchar)*p, >=, 0x20);
}

static void
test_json_and_clean(void)
{
	g_autoptr(GString) json = g_string_new(NULL);
	g_autofree gchar *clean = axis_library_text_clean("one\ntwo\x1b\xff\xe2\x80\xae end", 100);
	g_autofree gchar *cut = axis_library_text_clean("abcdef", 3);

	axis_library_json_append_string(json, "q\"b\\s/\n\t\x01\x7f\xc3\xa9");
	g_assert_cmpstr(json->str, ==, "\"q\\\"b\\\\s/\\n\\t\\u0001\x7f\xc3\xa9\"");
	g_assert_cmpstr(clean, ==, "one two \xef\xbf\xbd  end");
	g_assert_cmpstr(cut, ==, "abc");
	g_assert_null(axis_library_text_clean(NULL, 10));
}

/* Spec: Kinds */
static void
test_kinds(void)
{
	static const struct
	{
		const gchar  *name;
		AxisFileKind  kind;
	} cases[] = {
		{ "Film (2001).mkv", AXIS_FILE_KIND_VIDEO },
		{ "FILM.MKV",        AXIS_FILE_KIND_VIDEO },
		{ "clip.Mp4",        AXIS_FILE_KIND_VIDEO },
		{ "a.b.c.m2ts",      AXIS_FILE_KIND_VIDEO },
		{ "01 Song.FLAC",    AXIS_FILE_KIND_AUDIO },
		{ "book.m4b",        AXIS_FILE_KIND_AUDIO },
		{ "film.en.srt",     AXIS_FILE_KIND_SUBTITLE },
		{ "film.SUP",        AXIS_FILE_KIND_SUBTITLE },
		{ "poster.JpG",      AXIS_FILE_KIND_IMAGE },
		{ "fanart.webp",     AXIS_FILE_KIND_IMAGE },
		{ "movie.nfo",       AXIS_FILE_KIND_TEXT },
		{ "album.cue",       AXIS_FILE_KIND_TEXT },
		{ "archive.rar",     AXIS_FILE_KIND_OTHER },
		{ "noextension",     AXIS_FILE_KIND_OTHER },
		{ ".mkv",            AXIS_FILE_KIND_OTHER },
		{ "trailing.",       AXIS_FILE_KIND_OTHER },
		{ "film.mk\xc3\xa9", AXIS_FILE_KIND_OTHER },
		{ "film.verylongext", AXIS_FILE_KIND_OTHER },
		{ NULL, 0 }
	};
	AxisFileKind kind;
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		AxisFileKind got = axis_file_kind_from_name(cases[i].name, strlen(cases[i].name));

		if (got != cases[i].kind)
			g_error("%s: expected %s, got %s", cases[i].name, axis_file_kind_to_string(cases[i].kind),
				axis_file_kind_to_string(got));
	}

	for (i = 0; i < AXIS_FILE_KIND_COUNT; i++) {
		g_assert_true(axis_file_kind_from_string(axis_file_kind_to_string((AxisFileKind)i), &kind));
		g_assert_cmpint(kind, ==, (AxisFileKind)i);
	}
	g_assert_false(axis_file_kind_from_string("movie", &kind));
}

#define IGNORED(name, dir) axis_library_ignore_builtin(name, strlen(name), dir)

/* Spec: Partial download is not indexed (the rule) */
static void
test_builtin_ignores(void)
{
	g_assert_cmpint(IGNORED(".axis-volume", FALSE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED(".axis-tmp-1234", FALSE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED("lost+found", TRUE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED("@eaDir", TRUE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED(".Trash-1000", TRUE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED("$RECYCLE.BIN", TRUE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED(".DS_Store", FALSE), ==, AXIS_IGNORE_SYSTEM);

	g_assert_cmpint(IGNORED("Film.mkv.part", FALSE), ==, AXIS_IGNORE_PARTIAL);
	g_assert_cmpint(IGNORED("Film.mkv.PART", FALSE), ==, AXIS_IGNORE_PARTIAL);
	g_assert_cmpint(IGNORED("Film.mkv.!qB", FALSE), ==, AXIS_IGNORE_PARTIAL);
	g_assert_cmpint(IGNORED("setup.crdownload", FALSE), ==, AXIS_IGNORE_PARTIAL);
	g_assert_cmpint(IGNORED("v.f137.mp4.part-Frag12", FALSE), ==, AXIS_IGNORE_PARTIAL);
	g_assert_cmpint(IGNORED("_UNPACK_Film.2001", TRUE), ==, AXIS_IGNORE_PARTIAL);

	g_assert_cmpint(IGNORED("_UNPACK_Film.2001", FALSE), ==, AXIS_IGNORE_NONE);
	g_assert_cmpint(IGNORED("Film.mkv", FALSE), ==, AXIS_IGNORE_NONE);
	g_assert_cmpint(IGNORED("partial", FALSE), ==, AXIS_IGNORE_NONE);
	g_assert_cmpint(IGNORED(".part", FALSE), ==, AXIS_IGNORE_NONE);
	g_assert_cmpint(IGNORED("Film.part.mkv", FALSE), ==, AXIS_IGNORE_NONE);
	g_assert_cmpint(IGNORED("lost+found", FALSE), ==, AXIS_IGNORE_SYSTEM);
	g_assert_cmpint(IGNORED("Lost+Found", TRUE), ==, AXIS_IGNORE_NONE);
}

#define MATCH(pattern, name) axis_library_glob_match(pattern, name, strlen(name))

static void
test_patterns(void)
{
	const gchar *problem = NULL;

	g_assert_true(MATCH("*.nfo", "movie.nfo"));
	g_assert_false(MATCH("*.nfo", "movie.nfoo"));
	g_assert_false(MATCH("*.nfo", "movie.NFO"));
	g_assert_true(MATCH("sample*", "sample.mkv"));
	g_assert_true(MATCH("sample*", "sample"));
	g_assert_false(MATCH("sample*", "Sample.mkv"));
	g_assert_true(MATCH("a*b*c", "aXXbYYc"));
	g_assert_false(MATCH("a*b*c", "aXXbYY"));
	g_assert_true(MATCH("*", ""));
	g_assert_true(MATCH("caf?", "caf\xc3\xa9"));
	g_assert_true(MATCH("caf?", "caf\xff"));
	g_assert_false(MATCH("caf?", "caf\xc3\xa9x"));
	g_assert_true(MATCH("\\*.mkv", "*.mkv"));
	g_assert_false(MATCH("\\*.mkv", "a.mkv"));
	g_assert_true(MATCH("x\\", "x\\"));
	g_assert_true(MATCH("**a", "bba"));
	g_assert_false(MATCH("", "a"));

	g_assert_true(axis_library_pattern_check("*.nfo", NULL));
	g_assert_true(axis_library_pattern_check("Extras", NULL));
	g_assert_false(axis_library_pattern_check("", &problem));
	g_assert_nonnull(problem);
	g_assert_false(axis_library_pattern_check(NULL, NULL));
	g_assert_false(axis_library_pattern_check("a/b", NULL));
	g_assert_false(axis_library_pattern_check("*", NULL));
	g_assert_false(axis_library_pattern_check("***", NULL));
	g_assert_false(axis_library_pattern_check(" x", NULL));
	g_assert_false(axis_library_pattern_check("x ", NULL));
	g_assert_false(axis_library_pattern_check("a\nb", NULL));
	g_assert_false(axis_library_pattern_check("a\xc2\x85", NULL));
	g_assert_false(axis_library_pattern_check("bad\xff", NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/library/name/round-trip", test_round_trip);
	g_test_add_func("/library/name/canonical", test_canonical_form);
	g_test_add_func("/library/name/display", test_display);
	g_test_add_func("/library/name/json-and-clean", test_json_and_clean);
	g_test_add_func("/library/kind/by-extension", test_kinds);
	g_test_add_func("/library/kind/builtin-ignores", test_builtin_ignores);
	g_test_add_func("/library/kind/patterns", test_patterns);

	return g_test_run();
}
