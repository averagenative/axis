/*
 * test-analysis-name.c - the name normaliser of the same-title duplicate
 * tier, the ranking of copies, and the leftover rules' data
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tables of names with the key each must have, and pairs that must have
 * different keys: a wrong group is worse than a missed one, so the
 * near-misses are as important as the positives.  The normaliser has a fuzz
 * target (tests/fuzz/fuzz-analysis-name.c).
 */

#include <glib.h>
#include <string.h>

#include "library/axis-analysis-name.h"
#include "library/axis-analysis-rank.h"
#include "library/axis-analysis-rules.h"

/* A name is read as a film in a movies root and as an episode in a TV root:
 * each is checked on its own. */
static void
assert_key(
	const gchar *name,
	const gchar *film,
	const gchar *episode
){
	if (film != NULL) {
		g_autofree gchar *f = axis_analysis_film_key(name);

		if (g_strcmp0(f, film) != 0)
			g_error("film key of '%s': expected %s, got %s", name, film, f != NULL ? f : "none");
	}
	if (episode != NULL) {
		g_autofree gchar *e = axis_analysis_episode_key(name);

		if (g_strcmp0(e, episode) != 0)
			g_error("episode key of '%s': expected %s, got %s", name, episode, e != NULL ? e : "none");
	}
}

/* ── Text ───────────────────────────────────────────────────────── */

static void
test_normalise(void)
{
	static const struct
	{
		const gchar *in;
		const gchar *out;
	} cases[] = {
		{ "The Mazework", "the mazework" },
		{ "The.Mazework_1999", "the mazework 1999" },
		{ "THE MAZEWORK", "the mazework" },
		{ "Aur\xc3\xa9lie", "aurelie" },
		{ "Kessler's Ledger", "kesslers ledger" },
		{ "Kessler\xe2\x80\x99s Ledger", "kesslers ledger" },
		{ "Lore & Ledger", "lore and ledger" },
		{ "Lore&Ledger", "lore and ledger" },
		{ "  A -- B  ", "a b" },
		{ "[Group] Title (2020)", "group title 2020" },
		{ "", "" },
		{ "!!!", "" },
		{ "\xef\xbc\x92\xef\xbc\x90\xef\xbc\x92\xef\xbc\x90", "2020" },
		{ "Stra\xc3\x9f" "e", "strasse" },
		{ "\xe6\x9d\xb1\xe4\xba\xac", "\xe6\x9d\xb1\xe4\xba\xac" },
		{ "a\tb\nc", "a b c" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *out = axis_analysis_normalise(cases[i].in);
		g_autofree gchar *again = NULL;

		g_assert_cmpstr(out, ==, cases[i].out);
		/* Normalising is idempotent. */
		again = axis_analysis_normalise(out);
		g_assert_cmpstr(again, ==, out);
	}

	g_assert_null(axis_analysis_normalise(NULL));
	/* Not UTF-8: not compared. */
	g_assert_null(axis_analysis_normalise("\xff\xfe"));
	g_assert_null(axis_analysis_normalise("a\xc3"));
	{
		g_autofree gchar *long_text = g_strnfill(AXIS_ANALYSIS_NAME_MAX_INPUT + 1, 'a');
		g_autofree gchar *limit = g_strnfill(AXIS_ANALYSIS_NAME_MAX_INPUT, 'a');
		g_autofree gchar *kept = axis_analysis_normalise(limit);

		g_assert_null(axis_analysis_normalise(long_text));
		g_assert_nonnull(kept);
	}
}

/* ── Films ──────────────────────────────────────────────────────── */

/* Spec: Same title, different quality (the positives). */
static void
test_film_positives(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *key;
	} cases[] = {
		{ "Movies/The Mazework (1999)/The Mazework (1999) 1080p.mkv", "film|the mazework|1999" },
		{ "The.Mazework.1999.2160p.BluRay.x265.mkv", "film|the mazework|1999" },
		{ "the_mazework [1999].mp4", "film|the mazework|1999" },
		{ "Steel.Drifter.2049.2017.2160p.mkv", "film|steel drifter 2049|2017" },
		{ "2001 A Comet Voyage (1968).mkv", "film|2001 a comet voyage|1968" },
		{ "1917 (2019).mkv", "film|1917|2019" },
		{ "2012.2009.1080p.mkv", "film|2012|2009" },
		{ "Aur\xc3\xa9lie (2001).mkv", "film|aurelie|2001" },
		{ "Aurelie.2001.720p.mkv", "film|aurelie|2001" },
		{ "Kessler's Ledger (1993).mkv", "film|kesslers ledger|1993" },
		/* No year in the name: the folder it is in. */
		{ "Hearth (1995)/video.mkv", "film|hearth|1995" },
		{ "Hearth (1995)/1080p.mkv", "film|hearth|1995" },
		/* A year in the name beats the folder's. */
		{ "Hearth (1995)/Hearth 1986.mkv", "film|hearth|1986" },
		{ "Movie 2020 Remastered 2160p.mkv", "film|movie|2020" },
		{ "Movie (1900).mkv", "film|movie|1900" },
		{ "Movie (2099).mkv", "film|movie|2099" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
		assert_key(cases[i].name, cases[i].key, NULL);
}

/* Spec: Different titles stay apart (the films that are not comparable). */
static void
test_film_unreadable(void)
{
	static const gchar *const names[] = {
		/* No year anywhere: two films called Hearth must not group. */
		"Hearth.mkv", "Hearth/Hearth.mkv", "Movies/Hearth 1080p.mkv", "notes.txt",
		/* A year, but no title before it. */
		"1999.mkv", "(1999).mkv", "2020/2020.mkv",
		/* Not years. */
		"Movie (1899).mkv", "Movie (2100).mkv", "Movie 12345.mkv", "Movie.19999.mkv", "Movie 1080p.mkv",
		"Movie.1920x1080.mkv",
		"", ".mkv",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *key = axis_analysis_film_key(names[i]);

		if (key != NULL)
			g_error("'%s' was read as a film: %s", names[i], key);
	}
	g_assert_null(axis_analysis_film_key(NULL));
}

/* Spec: Different titles stay apart (near misses). */
static void
test_film_near_misses(void)
{
	static const gchar *const pairs[][2] = {
		/* Same title, different year. */
		{ "Hearth (1995).mkv", "Hearth (1986).mkv" },
		{ "Total Rewind (1990).mkv", "Total Rewind (2012).mkv" },
		{ "Dunmore.1984.mkv", "Dunmore.2021.mkv" },
		/* Similar titles. */
		{ "The Mazework (1999).mkv", "The Mazework Reloaded (2003).mkv" },
		{ "The Mazework (1999).mkv", "Mazework (1999).mkv" },
		{ "Alder (1979).mkv", "Alders (1986).mkv" },
		{ "Alder (1979).mkv", "Alder (1980).mkv" },
		{ "Ei8ht (1995).mkv", "Eight (1995).mkv" },
		{ "Ghast (1990).mkv", "Ghasts (1990).mkv" },
		/* A year inside the title is not the release year. */
		{ "Steel Drifter 2049.2017.mkv", "Steel Drifter (2017).mkv" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(pairs); i++) {
		g_autofree gchar *a = axis_analysis_film_key(pairs[i][0]);
		g_autofree gchar *b = axis_analysis_film_key(pairs[i][1]);

		g_assert_nonnull(a);
		g_assert_nonnull(b);
		if (strcmp(a, b) == 0)
			g_error("'%s' and '%s' have the same key %s", pairs[i][0], pairs[i][1], a);
	}
}

/* ── Episodes ───────────────────────────────────────────────────── */

static void
test_episode_positives(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *key;
	} cases[] = {
		{ "Show Name/Season 01/Show Name - S01E02 - Title.mkv", "episode|show name|0|1|2" },
		{ "Show.Name.S01E02.720p.mkv", "episode|show name|0|1|2" },
		{ "show name s1e2.mkv", "episode|show name|0|1|2" },
		{ "Show Name - 1x02.mkv", "episode|show name|0|1|2" },
		{ "Show Name - S01E102.mkv", "episode|show name|0|1|102" },
		/* The year comes from the show's folder when it is the same show... */
		{ "Show Name (2010)/Season 1/Show Name - S01E02.mkv", "episode|show name|2010|1|2" },
		/* ... or from the name. */
		{ "Show.Name.2010.S01E02.mkv", "episode|show name|2010|1|2" },
		/* No show in the name: the folder. */
		{ "Show Name (2010)/Season 1/S01E02.mkv", "episode|show name|2010|1|2" },
		{ "Show Name/Season 1/S01E02.mkv", "episode|show name|0|1|2" },
		/* Words after the marker are not looked at. */
		{ "Show Name - S01E02 - 1080p.mkv", "episode|show name|0|1|2" },
		{ "Show Name - S01E02-1080p.mkv", "episode|show name|0|1|2" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
		assert_key(cases[i].name, NULL, cases[i].key);
}

static void
test_episode_unreadable(void)
{
	static const gchar *const names[] = {
		/* Two episodes in one file. */
		"Show - S01E01E02.mkv", "Show - S01E01-E02.mkv", "Show - S01E01-02.mkv", "Show - S01E01 & 02.mkv",
		"Show - S01E01+E02.mkv", "Show - 1x01-02.mkv", "Show - S01E01 E02.mkv",
		/* Not an episode marker. */
		"Movie.1920x1080.mkv", "Movie.x264.mkv", "Show - S01E02x264.mkv", "Show - 1x1.mkv", "Show - S1E.mkv",
		"Show - SE01.mkv", "Show.mkv", "S1E1234567.mkv",
		/* Nothing before the marker and no folder. */
		"S01E02.mkv", "",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *key = axis_analysis_episode_key(names[i]);

		if (key != NULL)
			g_error("'%s' was read as an episode: %s", names[i], key);
	}
	g_assert_null(axis_analysis_episode_key(NULL));
}

/* Spec: Different titles stay apart (different episodes). */
static void
test_episode_near_misses(void)
{
	static const gchar *const pairs[][2] = {
		{ "Show - S01E01.mkv", "Show - S01E02.mkv" },
		{ "Show - S01E01.mkv", "Show - S02E01.mkv" },
		{ "Show - S01E01.mkv", "Show 2 - S01E01.mkv" },
		/* A show with a year and the same show without one are not matched. */
		{ "Show (2010)/Show - S01E01.mkv", "Show - S01E01.mkv" },
		/* Two shows of one name, years apart. */
		{ "Professor When (1963)/S01E01.mkv", "Professor When (2005)/S01E01.mkv" },
		{ "Show - S01E10.mkv", "Show - S01E01.mkv" },
		{ "Show - 1x02.mkv", "Show - 1x03.mkv" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(pairs); i++) {
		g_autofree gchar *a = axis_analysis_episode_key(pairs[i][0]);
		g_autofree gchar *b = axis_analysis_episode_key(pairs[i][1]);

		g_assert_nonnull(a);
		g_assert_nonnull(b);
		if (strcmp(a, b) == 0)
			g_error("'%s' and '%s' have the same key %s", pairs[i][0], pairs[i][1], a);
	}
	/* An episode is not a film, and a film is not an episode. */
	g_assert_null(axis_analysis_film_key("Show Name - S01E02.mkv"));
	assert_key("Movie (2020).mkv", "film|movie|2020", NULL);
}

/* ── Tracks, series and albums ──────────────────────────────────── */

static void
test_other_keys(void)
{
	g_autofree gchar *a = axis_analysis_track_key("The Beatles", "Abbey Road", "Something");
	g_autofree gchar *b = axis_analysis_track_key("THE BEATLES", "abbey road", "Something!");
	g_autofree gchar *c = axis_analysis_track_key("The Beatles", "Abbey Road", "Something Else");

	g_assert_cmpstr(a, ==, "track|the beatles|abbey road|something");
	g_assert_cmpstr(a, ==, b);
	g_assert_cmpstr(a, !=, c);
	g_assert_null(axis_analysis_track_key("The Beatles", "Abbey Road", NULL));
	g_assert_null(axis_analysis_track_key("The Beatles", "", "Something"));
	g_assert_null(axis_analysis_track_key("", "Abbey Road", "Something"));
	g_assert_null(axis_analysis_track_key("The Beatles", "Abbey Road", "..."));

	{
		g_autofree gchar *with_year = axis_analysis_show_key("Cracking Slate (2008)");
		g_autofree gchar *dotted = axis_analysis_show_key("Cracking.Slate.2008");
		g_autofree gchar *without = axis_analysis_show_key("Cracking Slate");
		g_autofree gchar *old = axis_analysis_show_key("Professor When (1963)");
		g_autofree gchar *new = axis_analysis_show_key("Professor When (2005)");

		g_assert_cmpstr(with_year, ==, "cracking slate|2008");
		g_assert_cmpstr(dotted, ==, with_year);
		g_assert_cmpstr(without, ==, "cracking slate|0");
		g_assert_cmpstr(old, !=, new);
		g_assert_null(axis_analysis_show_key(NULL));
		g_assert_null(axis_analysis_show_key("---"));
	}
	{
		g_autofree gchar *wall = axis_analysis_album_key("Pink Floyd", "The Wall");
		g_autofree gchar *wall2 = axis_analysis_album_key("pink floyd", "The Wall");
		g_autofree gchar *other = axis_analysis_album_key("Pink Floyd", "The Wall (Live)");

		g_assert_cmpstr(wall, ==, "pink floyd|the wall");
		g_assert_cmpstr(wall, ==, wall2);
		g_assert_cmpstr(wall, !=, other);
		g_assert_null(axis_analysis_album_key("Pink Floyd", ""));
		g_assert_null(axis_analysis_album_key(NULL, "The Wall"));
	}
}

/* ── Ranking ────────────────────────────────────────────────────── */

static AxisAnalysisCopy
copy_of(
	gint64       id,
	gboolean     probed,
	const gchar *res,
	const gchar *hdr,
	gdouble      bpp,
	gint64       size
){
	AxisAnalysisCopy copy;

	memset(&copy, 0, sizeof(copy));
	copy.id = id;
	copy.probed = probed;
	copy.resolution = res;
	copy.hdr = hdr;
	copy.lossless = -1;
	copy.bits_per_pixel = bpp;
	copy.bitrate = -1;
	copy.size = size;
	return copy;
}

/* Spec: the higher-resolution copy is ranked first. */
static void
test_rank(void)
{
	AxisAnalysisCopy uhd = copy_of(2, TRUE, "2160p", "sdr", 0.1, 100);
	AxisAnalysisCopy hd = copy_of(1, TRUE, "1080p", "sdr", 0.5, 900);
	AxisAnalysisCopy sd = copy_of(3, TRUE, "sd", "sdr", 0.9, 9000);
	AxisAnalysisCopy unprobed = copy_of(4, FALSE, NULL, NULL, -1, 99999);
	AxisAnalysisCopy hdr = copy_of(5, TRUE, "1080p", "hdr10", 0.1, 100);
	AxisAnalysisCopy dv = copy_of(6, TRUE, "1080p", "dolby_vision", 0.1, 100);
	AxisAnalysisCopy unsure = copy_of(7, TRUE, "1080p", "dolby_vision", 0.1, 100);
	AxisAnalysisCopy heavy = copy_of(8, TRUE, "1080p", "sdr", 0.7, 100);
	AxisAnalysisCopy big = copy_of(9, TRUE, "1080p", "sdr", 0.5, 2000);
	AxisAnalysisCopy same = copy_of(10, TRUE, "1080p", "sdr", 0.5, 900);

	unsure.hdr_uncertain = TRUE;
	/* Resolution first, whatever the size. */
	g_assert_cmpint(axis_analysis_copy_compare(&uhd, &hd), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&hd, &sd), <, 0);
	/* A probed copy before one never probed. */
	g_assert_cmpint(axis_analysis_copy_compare(&sd, &unprobed), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&unprobed, &sd), >, 0);
	/* HDR class, an uncertain one counting as none. */
	g_assert_cmpint(axis_analysis_copy_compare(&dv, &hdr), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&hdr, &hd), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&hd, &unsure), <, 0);
	/* Then bits per pixel, then size, then the lower id. */
	g_assert_cmpint(axis_analysis_copy_compare(&heavy, &hd), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&big, &hd), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&hd, &same), <, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&same, &hd), >, 0);
	g_assert_cmpint(axis_analysis_copy_compare(&hd, &hd), ==, 0);

	/* Lossless audio before lossy, then bit rate. */
	{
		AxisAnalysisCopy flac = copy_of(20, TRUE, NULL, NULL, -1, 100);
		AxisAnalysisCopy mp3 = copy_of(21, TRUE, NULL, NULL, -1, 900);
		AxisAnalysisCopy mp3_high = copy_of(22, TRUE, NULL, NULL, -1, 100);

		flac.lossless = 1;
		mp3.lossless = 0;
		mp3_high.lossless = 0;
		mp3.bitrate = 128000;
		mp3_high.bitrate = 320000;
		g_assert_cmpint(axis_analysis_copy_compare(&flac, &mp3), <, 0);
		g_assert_cmpint(axis_analysis_copy_compare(&mp3_high, &mp3), <, 0);
	}

	g_assert_cmpint(axis_analysis_resolution_rank("2160p"), >, axis_analysis_resolution_rank("1080p"));
	g_assert_cmpint(axis_analysis_resolution_rank("other"), ==, 0);
	g_assert_cmpint(axis_analysis_resolution_rank(NULL), ==, 0);
	g_assert_cmpint(axis_analysis_hdr_rank("hdr10", TRUE), ==, 0);
	g_assert_cmpint(axis_analysis_hdr_rank("sdr", FALSE), >, 0);
}

/* ── The leftover rules ─────────────────────────────────────────── */

static void
test_rule_data(void)
{
	const AxisLeftoverRule *rules = axis_leftover_rules();
	const gchar *const *values = axis_leftover_filter_values();
	guint n = 0;
	guint i;

	for (i = 0; rules[i].id != NULL; i++) {
		g_assert_nonnull(rules[i].label);
		g_assert_nonnull(rules[i].help);
		g_assert_nonnull(rules[i].condition);
		g_assert_true(g_strv_contains(values, rules[i].id));
		g_assert_cmpstr(axis_leftover_condition(rules[i].id), ==, rules[i].condition);
		n++;
	}
	g_assert_cmpuint(n, ==, 4);
	g_assert_true(g_strv_contains(values, "any"));
	g_assert_nonnull(axis_leftover_condition("any"));
	g_assert_null(axis_leftover_condition("bogus"));
	g_assert_null(axis_leftover_condition(NULL));
	g_assert_null(axis_leftover_condition("any; DROP TABLE library_file"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/analysis/name/normalise", test_normalise);
	g_test_add_func("/analysis/name/film", test_film_positives);
	g_test_add_func("/analysis/name/film-unreadable", test_film_unreadable);
	g_test_add_func("/analysis/name/film-near-misses", test_film_near_misses);
	g_test_add_func("/analysis/name/episode", test_episode_positives);
	g_test_add_func("/analysis/name/episode-unreadable", test_episode_unreadable);
	g_test_add_func("/analysis/name/episode-near-misses", test_episode_near_misses);
	g_test_add_func("/analysis/name/other-keys", test_other_keys);
	g_test_add_func("/analysis/rank", test_rank);
	g_test_add_func("/analysis/rules/data", test_rule_data);

	return g_test_run();
}
