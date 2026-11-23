/*
 * test-tv-unit.c - the TV module's pure parts: names, what a release holds
 * (never guessed), and each episode's state from its facts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-tv-module:
 *   2.2 the states and their reasons, by derivation from facts
 *   3.1 what a release holds: an episode, a double episode, a season pack,
 *       several seasons, Complete, a date; refused: an absolute number
 *       alone, a number the series' data does not have, a season it does
 *       not have, a date no episode aired on or two did, no episode named
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "modules/tv/axis-tv.h"
#include "release/axis-release.h"

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

/* ── Names ──────────────────────────────────────────────────────── */

static void
test_names(void)
{
	AxisTvMonitor monitor;
	AxisTvState state;
	AxisTvScope scope;
	guint i;
	g_autofree gchar *label = axis_tv_episode_label(1, 2);
	g_autofree gchar *special = axis_tv_episode_label(0, 11);
	g_autofree gchar *series = axis_tv_series_label("Harbour Lights", 2024);
	g_autofree gchar *undated = axis_tv_series_label("Harbour Lights", 0);

	for (i = AXIS_TV_MONITOR_ALL; i <= AXIS_TV_MONITOR_NONE; i++) {
		g_assert_true(axis_tv_monitor_from_name(axis_tv_monitor_name((AxisTvMonitor)i), &monitor));
		g_assert_cmpint(monitor, ==, (gint)i);
	}
	g_assert_false(axis_tv_monitor_from_name("everything", &monitor));
	g_assert_false(axis_tv_monitor_from_name(NULL, &monitor));
	g_assert_null(axis_tv_monitor_name((AxisTvMonitor)99));
	for (i = 0; i < AXIS_TV_N_STATES; i++) {
		g_assert_true(axis_tv_state_from_name(axis_tv_state_name((AxisTvState)i), &state));
		g_assert_cmpint(state, ==, (gint)i);
		g_assert_cmpstr(axis_tv_state_label((AxisTvState)i), !=, "Unknown");
	}
	g_assert_false(axis_tv_state_from_name("missing", &state));
	g_assert_cmpstr(axis_tv_state_name(AXIS_TV_NOT_AIRED), ==, "not_aired");
	g_assert_cmpstr(axis_tv_state_name(AXIS_TV_UNMONITORED), ==, "unmonitored");
	for (i = AXIS_TV_SCOPE_EPISODE; i <= AXIS_TV_SCOPE_SERIES; i++) {
		g_assert_true(axis_tv_scope_from_name(axis_tv_scope_name((AxisTvScope)i), &scope));
		g_assert_cmpint(scope, ==, (gint)i);
	}
	g_assert_false(axis_tv_scope_from_name("film", &scope));
	g_assert_cmpstr(label, ==, "S01E02");
	g_assert_cmpstr(special, ==, "S00E11");
	g_assert_cmpstr(series, ==, "Harbour Lights (2024)");
	g_assert_cmpstr(undated, ==, "Harbour Lights");
	/* A series' own state is its worst: attention first, then wanted. */
	g_assert_cmpuint(axis_tv_state_severity(AXIS_TV_ATTENTION), >, axis_tv_state_severity(AXIS_TV_WANTED));
	g_assert_cmpuint(axis_tv_state_severity(AXIS_TV_WANTED), >, axis_tv_state_severity(AXIS_TV_TO_IMPORT));
	g_assert_cmpuint(axis_tv_state_severity(AXIS_TV_DOWNLOADING), >, axis_tv_state_severity(AXIS_TV_NOT_AIRED));
	g_assert_cmpuint(axis_tv_state_severity(AXIS_TV_IN_LIBRARY), ==, 0);
	g_assert_true(axis_tv_outcome_needs_attention("none_acceptable"));
	g_assert_true(axis_tv_outcome_needs_attention("exhausted"));
	g_assert_false(axis_tv_outcome_needs_attention("grabbed"));
	g_assert_false(axis_tv_outcome_needs_attention("nothing_found"));
	g_assert_false(axis_tv_outcome_needs_attention(NULL));
}

/* ── What a release holds ───────────────────────────────────────── */

/* The fixture series as the matcher sees it: specials, two seasons, a
 * two-part episode aired on one day, a future episode, one with no date. */
static const AxisTvKnown harbour[] = {
	{ 1, 0, 1, "2025-01-01" },
	{ 2, 1, 1, "2024-03-01" },
	{ 3, 1, 2, "2024-03-08" },
	{ 4, 1, 3, "2024-03-15" },
	{ 5, 1, 4, "2024-03-15" },
	{ 6, 2, 1, "2026-11-06" },
	{ 7, 2, 2, "2026-11-13" },
	{ 8, 2, 3, "2026-11-20" },
	{ 9, 2, 4, NULL }
};

typedef struct
{
	const gchar *name;
	gboolean     ok;
	const gchar *code;	/* when not ok */
	const gchar *label;	/* when ok */
	gboolean     pack;
	const gchar *ids;	/* when ok: the known ids it holds, "4,5" */
} MatchCase;

static const MatchCase match_cases[] = {
	{ "Harbour.Lights.S01E02.1080p.WEB-DL.DDP5.1.H.264-GRP", TRUE, NULL, "S01E02", FALSE, "3" },
	{ "Harbour Lights - S02E01 - Return [WEBDL-1080p]", TRUE, NULL, "S02E01", FALSE, "6" },
	/* A double episode: both. */
	{ "Harbour.Lights.S01E03E04.1080p.WEB-DL.DDP5.1.H.264-GRP", TRUE, NULL, "S01E03-E04", FALSE, "4,5" },
	{ "Harbour.Lights.S01E03-E04.720p.HDTV.x264-GRP", TRUE, NULL, "S01E03-E04", FALSE, "4,5" },
	/* A season pack: every episode of the season. */
	{ "Harbour.Lights.S01.1080p.BluRay.x264-GRP", TRUE, NULL, "S01", TRUE, "2,3,4,5" },
	{ "Harbour.Lights.Season.2.1080p.WEB-DL.DDP5.1.H.264-GRP", TRUE, NULL, "S02", TRUE, "6,7,8,9" },
	/* Several seasons; Complete is every season but the specials. */
	{ "Harbour.Lights.S01-S02.1080p.BluRay.x264-GRP", TRUE, NULL, "S01-S02", TRUE, "2,3,4,5,6,7,8,9" },
	{ "Harbour.Lights.COMPLETE.1080p.BluRay.x264-GRP", TRUE, NULL, "S01-S02", TRUE, "2,3,4,5,6,7,8,9" },
	/* The specials are a season of their own. */
	{ "Harbour.Lights.S00E01.Behind.the.Harbour.1080p.WEB-DL.DDP5.1.H.264-GRP", TRUE, NULL, "S00E01", FALSE, "1" },
	/* A daily show's date: the one episode aired that day. */
	{ "Harbour.Lights.2026.11.13.1080p.WEB-DL.DDP5.1.H.264-GRP", TRUE, NULL, "S02E02", FALSE, "7" },
	/* Never guessed. */
	{ "Harbour.Lights.2024.03.15.1080p.WEB-DL.DDP5.1.H.264-GRP", FALSE, "ambiguous_date", NULL, FALSE, NULL },
	{ "Harbour.Lights.2026.10.01.1080p.WEB-DL.DDP5.1.H.264-GRP", FALSE, "no_such_date", NULL, FALSE, NULL },
	{ "Harbour.Lights.S01E09.1080p.WEB-DL.DDP5.1.H.264-GRP", FALSE, "no_such_episode", NULL, FALSE, NULL },
	{ "Harbour.Lights.S01E04E05.1080p.WEB-DL.DDP5.1.H.264-GRP", FALSE, "no_such_episode", NULL, FALSE, NULL },
	{ "Harbour.Lights.S05.1080p.BluRay.x264-GRP", FALSE, "no_such_season", NULL, FALSE, NULL },
	{ "[SubGroup] Harbour Lights - 105 [1080p].mkv", FALSE, "absolute_only", NULL, FALSE, NULL },
	{ "Harbour.Lights.1080p.WEB-DL.DDP5.1.H.264-GRP", FALSE, "no_episode", NULL, FALSE, NULL },
	{ NULL, FALSE, NULL, NULL, FALSE, NULL }
};

static void
test_match(void)
{
	guint i;

	for (i = 0; match_cases[i].name != NULL; i++) {
		const MatchCase *c = &match_cases[i];
		g_autoptr(AxisRelease) parsed = axis_release_parse(c->name);
		g_autoptr(GString) ids = g_string_new(NULL);
		AxisTvHolds holds;
		guint k;

		g_assert_nonnull(parsed);
		axis_tv_match_release(parsed, harbour, G_N_ELEMENTS(harbour), &holds);
		if (holds.ok != c->ok)
			g_error("%s: expected %s, got %s (%s: %s)", c->name, c->ok ? "a match" : "a refusal",
				holds.ok ? "a match" : "a refusal", holds.code != NULL ? holds.code : "-",
				holds.text != NULL ? holds.text : "");
		g_assert_nonnull(holds.text);
		if (!c->ok) {
			g_assert_cmpstr(holds.code, ==, c->code);
			g_assert_cmpuint(holds.indices->len, ==, 0);
			g_assert_false(holds.pack);
			axis_tv_holds_clear(&holds);
			continue;
		}
		g_assert_null(holds.code);
		g_assert_cmpstr(holds.label, ==, c->label);
		g_assert_cmpint(holds.pack, ==, c->pack);
		for (k = 0; k < holds.indices->len; k++)
			g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, k > 0 ? "," : "",
				harbour[g_array_index(holds.indices, guint, k)].id);
		if (strcmp(ids->str, c->ids) != 0)
			g_error("%s: expected episodes %s, got %s", c->name, c->ids, ids->str);
		axis_tv_holds_clear(&holds);
	}

	/* A series with no data yet: nothing is matched. */
	{
		g_autoptr(AxisRelease) parsed = axis_release_parse("Harbour.Lights.S01E02.1080p.WEB-DL.DDP5.1.H.264-GRP");
		AxisTvHolds holds;

		axis_tv_match_release(parsed, harbour, 0, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "no_such_episode");
		assert_contains(holds.text, "no such episode in the episode list");
		axis_tv_holds_clear(&holds);
	}
}

/* add-manual-search: a release numbered beyond the data in part is still
 * refused, and what it names that the data has is kept apart (a person may
 * grab it by hand for those), with what is missing; one none of whose
 * numbers the data has keeps nothing. */
static void
test_match_partial(void)
{
	g_autoptr(AxisRelease) partial = axis_release_parse("Harbour.Lights.S01E04E05.1080p.WEB-DL.DDP5.1.H.264-GRP");
	g_autoptr(AxisRelease) beyond = axis_release_parse("Harbour.Lights.S01E09.1080p.WEB-DL.DDP5.1.H.264-GRP");
	AxisTvHolds holds;

	axis_tv_match_release(partial, harbour, G_N_ELEMENTS(harbour), &holds);
	g_assert_false(holds.ok);
	g_assert_cmpstr(holds.code, ==, "no_such_episode");
	g_assert_cmpuint(holds.indices->len, ==, 0);
	g_assert_nonnull(holds.found);
	g_assert_cmpuint(holds.found->len, ==, 1);
	g_assert_cmpint(harbour[g_array_index(holds.found, guint, 0)].id, ==, 5);
	g_assert_cmpstr(holds.found_label, ==, "S01E04");
	g_assert_cmpstr(holds.missing, ==, "S01E05");
	axis_tv_holds_clear(&holds);

	axis_tv_match_release(beyond, harbour, G_N_ELEMENTS(harbour), &holds);
	g_assert_false(holds.ok);
	g_assert_cmpstr(holds.code, ==, "no_such_episode");
	g_assert_null(holds.found);
	g_assert_null(holds.missing);
	axis_tv_holds_clear(&holds);
}

/* ── A name numbered absolutely, for an anime series (A2) ───────── */

/* An anime series as the matcher sees it: a special that TheTVDB gives a
 * number (never matched by it), two seasons numbered 1 to 9, one episode that
 * airs in the future, one with no air date, one with no number. */
static const AxisTvKnown rangers[] = {
	{ 10, 0, 1, "2020-01-01" },
	{ 11, 1, 1, "2021-01-01" },
	{ 12, 1, 2, "2021-01-08" },
	{ 13, 1, 3, "2021-01-15" },
	{ 14, 1, 4, "2021-01-22" },
	{ 15, 2, 1, "2022-01-01" },
	{ 16, 2, 2, "2022-01-08" },
	{ 17, 2, 3, "2022-01-15" },
	{ 18, 2, 4, "2099-01-01" },
	{ 19, 2, 5, NULL },
	{ 20, 3, 1, "2023-01-01" }
};

/* TheTVDB's absolute number of each, in the same order. */
static const gint rangers_absolute[G_N_ELEMENTS(rangers)] = { 500, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };

static const gchar *const rangers_names[] = { "Tenkuu Rangers", "Sora no Senshi" };

typedef struct
{
	const gchar *name;
	gboolean     ok;
	const gchar *code;	/* when not ok */
	const gchar *label;	/* when ok */
	const gchar *ids;	/* when ok */
	gboolean     by_absolute;
	const gchar *says;	/* (nullable) a part of the text */
} AnimeCase;

static const AnimeCase anime_cases[] = {
	/* Accepted: each form maps to the episode that has the number. */
	{ "[Tsuki] Tenkuu Rangers - 05 [1080p]", TRUE, NULL, "S02E01", "15", TRUE, "it holds S02E01 (absolute 5 is S02E01 by "
	  "TheTVDB)" },
	{ "Tenkuu.Rangers.E06.1080p.WEB.H264-GRP", TRUE, NULL, "S02E02", "16", TRUE, "6 is S02E02 by TheTVDB" },
	{ "[Tsuki] Tenkuu Rangers - Episode 07 [1080p]", TRUE, NULL, "S02E03", "17", TRUE, NULL },
	{ "[Tsuki] Tenkuu Rangers 06 [1080p]", TRUE, NULL, "S02E02", "16", TRUE, "6 is S02E02 by TheTVDB" },
	{ "Tenkuu.Rangers.06.1080p.WEB", TRUE, NULL, "S02E02", "16", TRUE, NULL },
	{ "[Tsuki] Sora no Senshi 06 [1080p]", TRUE, NULL, "S02E02", "16", TRUE, NULL },
	{ "Tenkuu Rangers - 7 [1080p]", TRUE, NULL, "S02E03", "17", TRUE, NULL },
	/* A version is a revision (the parser's field), a checksum is ignored. */
	{ "[Tsuki] Tenkuu Rangers - 06v2 [1080p]", TRUE, NULL, "S02E02", "16", TRUE, NULL },
	{ "[Tsuki] Tenkuu Rangers - 05 [1080p][A1B2C3D4]", TRUE, NULL, "S02E01", "15", TRUE, NULL },
	/* A season and an episode: by them alone, as any series. */
	{ "Tenkuu.Rangers.S02E01.1080p.WEB.H264-GRP", TRUE, NULL, "S02E01", "15", FALSE, "it holds S02E01" },
	{ "Tenkuu.Rangers.S00E01.1080p.WEB.H264-GRP", TRUE, NULL, "S00E01", "10", FALSE, NULL },
	{ "Tenkuu.Rangers.S01.1080p.BluRay.x264-GRP", TRUE, NULL, "S01", "11,12,13,14", FALSE, NULL },
	/* Both numberings, and they agree. */
	{ "[NekoFansub] Tenkuu Rangers - S02E01 - 005 [720p]", TRUE, NULL, "S02E01", "15", FALSE, NULL },
	/* A range that is exactly a whole season is its pack. */
	{ "[KumoSubs] Tenkuu Rangers (01-04) [1080p] [Batch]", TRUE, NULL, "S01", "11,12,13,14", TRUE, "it is the whole of "
	  "season 1 (absolute 1 to 4 are S01E01 to S01E04 by TheTVDB)" },
	{ "Tenkuu Rangers 01-04 [BD 1080p]", TRUE, NULL, "S01", "11,12,13,14", TRUE, NULL },
	/* Refused, each with its reason. */
	{ "[NekoFansub] Tenkuu Rangers - S02E01 - 006 [720p]", FALSE, "numberings_differ", NULL, NULL, FALSE, "it says S02E01 "
	  "and absolute 6, which is S02E02 by TheTVDB" },
	{ "[NekoFansub] Tenkuu Rangers - S02E01 - 300 [720p]", FALSE, "numberings_differ", NULL, NULL, FALSE, "which no "
	  "episode of the series has" },
	{ "[Tsuki] Tenkuu Rangers S2 - 05 [1080p]", FALSE, "season_and_absolute", NULL, NULL, FALSE, "it is not read as "
	  "S02E05" },
	{ "Tenkuu.Rangers.S01E137.1080p.WEB.H264-GRP", FALSE, "no_such_episode", NULL, NULL, FALSE, "it is numbered S01E137" },
	{ "[Tsuki] Tenkuu Rangers - 300 [1080p]", FALSE, "absolute_unknown", NULL, NULL, FALSE, "no episode of the series has "
	  "the absolute number 300 (the highest TheTVDB gives is 9)" },
	/* A special's number is never matched, whatever TheTVDB gives it. */
	{ "[Tsuki] Tenkuu Rangers - 500 [1080p]", FALSE, "absolute_unknown", NULL, NULL, FALSE, NULL },
	{ "[Tsuki] Tenkuu Rangers - 08 [1080p]", FALSE, "absolute_unaired", NULL, NULL, FALSE, "absolute 8 is S02E04, which has "
	  "not aired" },
	{ "[Tsuki] Tenkuu Rangers - 09 [1080p]", FALSE, "absolute_unaired", NULL, NULL, FALSE, "it has no air date" },
	{ "[Tsuki] Tenkuu Rangers - 00 [1080p]", FALSE, "absolute_zero", NULL, NULL, FALSE, "0 is not an episode number" },
	{ "[Tsuki] Tenkuu Rangers - 05.5 [1080p]", FALSE, "absolute_decimal", NULL, NULL, FALSE, "5.5 is not an episode number" },
	/* (Read as a file's name, a file on disk or a grab by force reads it: a
	 * batch is the episodes it maps to only when a search judges a release,
	 * /tv/unit/match-anime-batch.) */
	{ "[Tsuki] Tenkuu Rangers - 01-03 [1080p]", FALSE, "absolute_batch", NULL, NULL, FALSE, "a batch of part of a season "
	  "(absolute 1 to 3, which are S01E01 to S01E03 by TheTVDB): only a search takes a batch, as the episodes it holds" },
	{ "[Tsuki] Tenkuu Rangers - 01-07 [1080p]", FALSE, "absolute_batch", NULL, NULL, FALSE, NULL },
	/* A range one number of which maps to nothing names it. */
	{ "[Tsuki] Tenkuu Rangers - 01-12 [1080p]", FALSE, "absolute_unaired", NULL, NULL, FALSE, "absolute 8 is S02E04" },
	{ "[Tsuki] Tenkuu Rangers - OVA 2 [1080p]", FALSE, "special", NULL, NULL, FALSE, "assign it by hand" },
	{ "[Tsuki] Tenkuu Rangers - Movie [1080p]", FALSE, "special", NULL, NULL, FALSE, "assign it by hand" },
	{ "Tenkuu Rangers 2 05 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow the series' name" },
	{ "Tenkuu Rangers 5 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "a single digit" },
	{ "Tenkuu Rangers 05 The Long Night [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "words that are not "
	  "understood" },
	{ "Tenkuu.Rangers.1080p.WEB-DL.H.264-GRP", FALSE, "no_episode", NULL, NULL, FALSE, NULL },
	/* A second number after the one reading by itself keeps (the review's
	 * L1): neither is taken. */
	{ "[Tsuki] Tenkuu Rangers - 05 - 06 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow the "
	  "series' name" },
	{ "[Tsuki] Tenkuu Rangers - 05 [06] [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow" },
	/* A picture's height in a bracket is no second number. */
	{ "[Tsuki] Tenkuu Rangers - 05 [1080][A1B2C3D4]", TRUE, NULL, "S02E01", "15", TRUE, NULL },
	/* A season and a bare number with no hyphen (L2): refused as "S2 - 05"
	 * is, never the whole of season 2. */
	{ "[Tsuki] Tenkuu Rangers S2 05 [1080p]", FALSE, "season_and_absolute", NULL, NULL, FALSE, "(season 2, 5), which is not "
	  "matched" },
	{ "[Tsuki] Tenkuu Rangers S02 03v2 [1080p]", FALSE, "season_and_absolute", NULL, NULL, FALSE, "it is not read as S02E03" },
	{ "[Tsuki] Tenkuu Rangers S2 [1080p]", TRUE, NULL, "S02", "15,16,17,18,19", FALSE, NULL },
	/* (Reading the name by itself gives the whole of season 5 here.) */
	{ "Tenkuu.Rangers.2nd.Season.05.1080p.WEB", FALSE, "season_and_absolute", NULL, NULL, FALSE, "(season 2, 5), which is "
	  "not matched" },
	{ "Tenkuu Rangers Season 2 03 [1080p]", FALSE, "season_and_absolute", NULL, NULL, FALSE, "it is not read as S02E03" },
	/* A number alone in a bracket, and a word that says the number counts
	 * something larger than an episode, after a bare number (B2). */
	{ "Tenkuu Rangers 06 [05][1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow" },
	{ "Tenkuu Rangers 06 (05)", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow" },
	{ "[Tsuki] Tenkuu Rangers 02 Complete [BD]", FALSE, "absolute_not_read", NULL, NULL, FALSE, NULL },
	{ "Tenkuu Rangers 03 BD 1080p", FALSE, "absolute_not_read", NULL, NULL, FALSE, NULL },
	/* The second review's lesser faults.  A year after the number is held to
	 * the rule for a name's year (the series is of 2023). */
	{ "[Tsuki] Tenkuu Rangers - 05 (2011) [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "the year in its name is "
	  "2011 and the series' is 2023" },
	{ "[Tsuki] Tenkuu Rangers - 05 (2023) [1080p]", TRUE, NULL, "S02E01", "15", TRUE, NULL },
	{ "[Tsuki] Tenkuu Rangers (2011) - 05 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "the year in its name" },
	/* A word after the number that says it is not one episode; an episode's
	 * title after it is read past. */
	{ "[Tsuki] Tenkuu Rangers - 02 Movie [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "not one episode" },
	{ "[Tsuki] Tenkuu Rangers - 02 BD [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "not one episode" },
	{ "[Tsuki] Tenkuu Rangers - 05 PV [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, NULL },
	{ "[Tsuki] Tenkuu Rangers - 05 of 12 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, NULL },
	{ "[Tsuki] Tenkuu Rangers - 05 Complete [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "not one episode" },
	{ "[Tsuki] Tenkuu Rangers - 05 - The Long Night [1080p]", TRUE, NULL, "S02E01", "15", TRUE, NULL },
	{ "[Tsuki] Tenkuu Rangers - 05 - Part of the Team [1080p]", TRUE, NULL, "S02E01", "15", TRUE, NULL },
	/* Two numbers joined by a word, and a number alone in a later bracket. */
	{ "[Tsuki] Tenkuu Rangers - 05 & 06 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow" },
	{ "[Tsuki] Tenkuu Rangers - 05 [1080p][06]", FALSE, "absolute_not_read", NULL, NULL, FALSE, "two numbers follow" },
	/* "S2 #05"; and a count of episodes after a season is no bare number. */
	{ "[Tsuki] Tenkuu Rangers S2 #05 [1080p]", FALSE, "season_and_absolute", NULL, NULL, FALSE, "(season 2, 5)" },
	{ "Tenkuu Rangers S02 5 Episodes 1080p", TRUE, NULL, "S02", "15,16,17,18,19", FALSE, NULL },
	{ NULL, FALSE, NULL, NULL, NULL, FALSE, NULL }
};

static void
anime_read(
	AxisTvAnimeRead     *read,
	gboolean             anime,
	AxisTvAbsoluteState  state,
	const gchar         *name
){
	memset(read, 0, sizeof(*read));
	read->anime = anime;
	read->state = state;
	read->absolute = rangers_absolute;
	read->today = "2026-11-21";
	read->name = name;
	read->titles = rangers_names;
	read->n_titles = G_N_ELEMENTS(rangers_names);
	read->n_own = 1;
	read->year = 2023;
}

static gchar *
held_ids(
	const AxisTvKnown *known,
	const AxisTvHolds *holds
){
	GString *ids = g_string_new(NULL);
	guint k;

	for (k = 0; k < holds->indices->len; k++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, k > 0 ? "," : "",
			known[g_array_index(holds->indices, guint, k)].id);
	return g_string_free(ids, FALSE);
}

/*
 * add-anime-support, A2 (design.md section 5): each accepted form maps to
 * the episode that has the number, and each doubtful form is refused with its
 * own code and sentence.  Spec (tv delta): Mapped; Both numberings disagree; A
 * season and a bare number; Not aired; A special.
 */
static void
test_match_anime(void)
{
	guint i;

	for (i = 0; anime_cases[i].name != NULL; i++) {
		const AnimeCase *c = &anime_cases[i];
		g_autoptr(AxisRelease) parsed = axis_release_parse(c->name);
		g_autofree gchar *ids = NULL;
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, c->name);
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		if (holds.ok != c->ok || (!c->ok && g_strcmp0(holds.code, c->code) != 0))
			g_error("%s: expected %s, got %s (%s: %s)", c->name, c->ok ? "a match" : c->code,
				holds.ok ? "a match" : "a refusal", holds.code != NULL ? holds.code : "-",
				holds.text != NULL ? holds.text : "");
		g_assert_nonnull(holds.text);
		if (c->says != NULL)
			assert_contains(holds.text, c->says);
		if (!c->ok) {
			/* A refusal holds nothing and is never "by an absolute number". */
			g_assert_cmpuint(holds.indices->len, ==, 0);
			g_assert_false(holds.pack);
			g_assert_false(holds.by_absolute);
			g_assert_null(holds.absolute_text);
			axis_tv_holds_clear(&holds);
			continue;
		}
		g_assert_cmpstr(holds.label, ==, c->label);
		ids = held_ids(rangers, &holds);
		if (strcmp(ids, c->ids) != 0)
			g_error("%s: expected episodes %s, got %s", c->name, c->ids, ids);
		g_assert_cmpint(holds.by_absolute, ==, c->by_absolute);
		g_assert_true((holds.absolute_text != NULL) == c->by_absolute);
		axis_tv_holds_clear(&holds);
	}
	/* "v2" is a revision as the parser reports it (the quality rules rank it
	 * as they rank a PROPER); the matcher only maps the number. */
	{
		g_autoptr(AxisRelease) parsed = axis_release_parse("[Tsuki] Tenkuu Rangers - 06v2 [1080p]");

		g_assert_cmpint(parsed->version, ==, 2);
	}
}

/*
 * A series that is not marked as anime: every name of the anime table, and
 * every name of the standard table, gives exactly what axis_tv_match_release()
 * gives: the same verdict, code, text, label and episodes.  The same name
 * that maps for an anime series is refused as it always was.
 * Spec (tv delta): Not an anime series; An absolute number for a standard
 * series.
 */
static void
test_match_standard_unchanged(void)
{
	guint pass;
	guint i;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; pass == 0 ? anime_cases[i].name != NULL : match_cases[i].name != NULL; i++) {
			const gchar *name = pass == 0 ? anime_cases[i].name : match_cases[i].name;
			const AxisTvKnown *known = pass == 0 ? rangers : harbour;
			guint n = pass == 0 ? G_N_ELEMENTS(rangers) : G_N_ELEMENTS(harbour);
			g_autoptr(AxisRelease) parsed = axis_release_parse(name);
			g_autofree gchar *ids = NULL;
			g_autofree gchar *ids_plain = NULL;
			AxisTvAnimeRead read;
			AxisTvHolds holds;
			AxisTvHolds plain;

			/* Not marked: whatever state its numbers are in. */
			anime_read(&read, FALSE, AXIS_TV_ABSOLUTE_USABLE, name);
			axis_tv_match_release_anime(parsed, known, n, &read, &holds);
			axis_tv_match_release(parsed, known, n, &plain);
			ids = held_ids(known, &holds);
			ids_plain = held_ids(known, &plain);
			g_assert_cmpint(holds.ok, ==, plain.ok);
			g_assert_cmpstr(holds.code, ==, plain.code);
			g_assert_cmpstr(holds.text, ==, plain.text);
			g_assert_cmpstr(holds.label, ==, plain.label);
			g_assert_cmpstr(ids, ==, ids_plain);
			g_assert_cmpint(holds.pack, ==, plain.pack);
			g_assert_false(holds.by_absolute);
			g_assert_null(holds.absolute_text);
			axis_tv_holds_clear(&holds);
			axis_tv_holds_clear(&plain);
			/* No context at all is the same. */
			axis_tv_match_release_anime(parsed, known, n, NULL, &holds);
			axis_tv_match_release(parsed, known, n, &plain);
			g_assert_cmpint(holds.ok, ==, plain.ok);
			g_assert_cmpstr(holds.text, ==, plain.text);
			axis_tv_holds_clear(&holds);
			axis_tv_holds_clear(&plain);
		}
	}
	{
		g_autoptr(AxisRelease) parsed = axis_release_parse("[Tsuki] Tenkuu Rangers - 05 [1080p]");
		g_autoptr(AxisRelease) bare = axis_release_parse("[Tsuki] Tenkuu Rangers 06 [1080p]");
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, FALSE, AXIS_TV_ABSOLUTE_USABLE, "[Tsuki] Tenkuu Rangers - 05 [1080p]");
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_only");
		g_assert_cmpstr(holds.text, ==, "it states only an absolute episode number (5), which is not matched to a season "
			"and an episode");
		axis_tv_holds_clear(&holds);
		/* The reading after a title is never made for it. */
		anime_read(&read, FALSE, AXIS_TV_ABSOLUTE_USABLE, "[Tsuki] Tenkuu Rangers 06 [1080p]");
		axis_tv_match_release_anime(bare, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "no_episode");
		g_assert_false(axis_tv_anime_claims(bare, &read, NULL, NULL));
		axis_tv_holds_clear(&holds);
	}
}

/*
 * An anime series whose numbers are not usable matches nothing through them:
 * no source, none given, not usable; and a state said to be usable over
 * episodes whose numbers are not (two share one, one is out of order) matches
 * nothing either.  A name that states a season and an episode still does.
 */
static void
test_match_anime_unusable(void)
{
	static const AxisTvAbsoluteState states[] = {
		AXIS_TV_ABSOLUTE_NO_SOURCE, AXIS_TV_ABSOLUTE_NONE_GIVEN, AXIS_TV_ABSOLUTE_NOT_USABLE
	};
	static const gchar *const names[] = {
		"[Tsuki] Tenkuu Rangers - 05 [1080p]", "[Tsuki] Tenkuu Rangers 06 [1080p]",
		"[KumoSubs] Tenkuu Rangers (01-04) [1080p] [Batch]", NULL
	};
	gint twice[G_N_ELEMENTS(rangers)];
	gint disorder[G_N_ELEMENTS(rangers)];
	AxisTvAnimeRead read;
	AxisTvHolds holds;
	guint s;
	guint i;

	for (s = 0; s < G_N_ELEMENTS(states); s++) {
		for (i = 0; names[i] != NULL; i++) {
			g_autoptr(AxisRelease) parsed = axis_release_parse(names[i]);

			anime_read(&read, TRUE, states[s], names[i]);
			axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
			g_assert_false(holds.ok);
			g_assert_cmpstr(holds.code, ==, "absolute_unusable");
			assert_contains(holds.text, "cannot be used");
			g_assert_cmpuint(holds.indices->len, ==, 0);
			axis_tv_holds_clear(&holds);
			g_assert_false(axis_tv_anime_claims(parsed, &read, NULL, NULL));
		}
		{
			g_autoptr(AxisRelease) parsed = axis_release_parse("Tenkuu.Rangers.S02E01.1080p.WEB.H264-GRP");

			anime_read(&read, TRUE, states[s], "Tenkuu.Rangers.S02E01.1080p.WEB.H264-GRP");
			axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
			g_assert_true(holds.ok);
			g_assert_cmpstr(holds.label, ==, "S02E01");
			axis_tv_holds_clear(&holds);
		}
	}
	/* The state passed says usable; the episodes do not bear it out. */
	memcpy(twice, rangers_absolute, sizeof(rangers_absolute));
	twice[6] = 5;
	memcpy(disorder, rangers_absolute, sizeof(rangers_absolute));
	disorder[2] = 4;
	disorder[4] = 2;
	for (i = 0; names[i] != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(names[i]);

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, names[i]);
		read.absolute = twice;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_unusable");
		axis_tv_holds_clear(&holds);
		read.absolute = disorder;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_unusable");
		axis_tv_holds_clear(&holds);
		/* No numbers at all. */
		read.absolute = NULL;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_unknown");
		axis_tv_holds_clear(&holds);
	}
}

/*
 * A long series (six seasons of 24, numbered 1 to 144): a number of three
 * digits is the number its digits say, never a season and an episode worked
 * out from it; and a bare one that is also a season and an episode the series
 * has is refused, where a hyphen or an "E" settles it.
 */
static void
test_match_anime_long(void)
{
	static const struct
	{
		const gchar *name;
		gboolean     ok;
		const gchar *code;
		const gchar *label;
	} cases[] = {
		{ "[Tsuki] Tenkuu Rangers - 137 [1080p]", TRUE, NULL, "S06E17" },
		{ "[Tsuki] Tenkuu Rangers 137 [1080p]", TRUE, NULL, "S06E17" },
		{ "Tenkuu.Rangers.137.1080p.WEB", TRUE, NULL, "S06E17" },
		{ "[Tsuki] Tenkuu Rangers 101 [1080p]", FALSE, "absolute_ambiguous", NULL },
		{ "[Tsuki] Tenkuu Rangers 124 [1080p]", FALSE, "absolute_ambiguous", NULL },
		{ "[Tsuki] Tenkuu Rangers - 101 [1080p]", TRUE, NULL, "S05E05" },
		{ "Tenkuu.Rangers.E101.1080p.WEB.H264-GRP", TRUE, NULL, "S05E05" },
		{ "[Tsuki] Tenkuu Rangers 125 [1080p]", TRUE, NULL, "S06E05" },
		{ "[Tsuki] Tenkuu Rangers 1047 [1080p]", FALSE, "absolute_unknown", NULL },
		{ "[Tsuki] Tenkuu Rangers - 145 [1080p]", FALSE, "absolute_unknown", NULL },
		{ "Tenkuu Rangers 001-024 [BD 1080p]", TRUE, NULL, "S01" },
		{ "Tenkuu Rangers 001-048 [BD 1080p]", TRUE, NULL, "S01-S02" },
		{ "Tenkuu Rangers 001-030 [BD 1080p]", FALSE, "absolute_batch", NULL },
		{ "Tenkuu Rangers 013-036 [BD 1080p]", FALSE, "absolute_batch", NULL },
		{ NULL, FALSE, NULL, NULL }
	};
	AxisTvKnown known[144];
	gint absolute[144];
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known); i++) {
		known[i].id = 100 + i;
		known[i].season = (gint)(i / 24) + 1;
		known[i].number = (gint)(i % 24) + 1;
		known[i].air_date = "2020-01-01";
		absolute[i] = (gint)i + 1;
	}
	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		read.absolute = absolute;
		axis_tv_match_release_anime(parsed, known, G_N_ELEMENTS(known), &read, &holds);
		if (holds.ok != cases[i].ok || (!holds.ok && g_strcmp0(holds.code, cases[i].code) != 0) ||
		    (holds.ok && g_strcmp0(holds.label, cases[i].label) != 0))
			g_error("%s: got %s %s (%s)", cases[i].name, holds.ok ? "a match" : holds.code,
				holds.label != NULL ? holds.label : "", holds.text);
		axis_tv_holds_clear(&holds);
	}
}

/* Whether a name that reading by itself gives another title is the series'
 * all the same (what the search and the import ask before they judge it). */
static void
test_anime_claims(void)
{
	static const struct
	{
		const gchar *name;
		gboolean     claims;
		gint         version;
		const gchar *why_not;
	} cases[] = {
		{ "[Tsuki] Tenkuu Rangers 06 [1080p]", TRUE, 0, NULL },
		{ "[Tsuki] Tenkuu Rangers 06v2 [1080p]", TRUE, 2, NULL },
		{ "Tenkuu Rangers - 7 [1080p]", TRUE, 0, NULL },
		{ "Tenkuu Rangers 01-04 [BD 1080p]", TRUE, 0, NULL },
		{ "[Tsuki] Tenkuu Rangers 2 05 [1080p]", FALSE, 0, "two numbers follow" },
		{ "[Tsuki] Tenkuu Rangers - Movie [1080p]", FALSE, 0, "assign it by hand" },
		{ "[Tsuki] Hoshi no Kagami 06 [1080p]", FALSE, 0, NULL },
		/* Reading by itself already states an episode: never claimed here. */
		{ "[Tsuki] Tenkuu Rangers - 06 [1080p]", FALSE, 0, NULL },
		{ "Tenkuu.Rangers.S01E02.1080p.WEB.H264-GRP", FALSE, 0, NULL },
		/* A year that is not the series' (2023): another title's.  The
		 * series' year, and a year one off, are the series'. */
		{ "[Tsuki] Tenkuu Rangers 06 (2011) [1080p]", FALSE, 0, "the year in its name is 2011 and the series' is 2023" },
		{ "Tenkuu.Rangers.06.2011.1080p.WEB", FALSE, 0, "two numbers follow" },
		{ "[Tsuki] Tenkuu Rangers 06 (2023) [1080p]", TRUE, 0, NULL },
		{ "[Tsuki] Tenkuu Rangers 06 (2024) [1080p]", TRUE, 0, NULL },
		/* A number alone in a bracket; a word after the number. */
		{ "Tenkuu Rangers 06 [05][1080p]", FALSE, 0, "two numbers follow" },
		{ "[Tsuki] Tenkuu Rangers 02 Complete [BD]", FALSE, 0, "not one episode" },
		{ NULL, FALSE, 0, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		g_autofree gchar *why_not = NULL;
		AxisTvAnimeRead read;
		gint version = -1;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		if (axis_tv_anime_claims(parsed, &read, &version, &why_not) != cases[i].claims)
			g_error("%s: expected %s", cases[i].name, cases[i].claims ? "claimed" : "not claimed");
		g_assert_cmpint(version, ==, cases[i].version);
		if (cases[i].why_not != NULL)
			assert_contains(why_not, cases[i].why_not);
		else
			g_assert_null(why_not);
	}
}

/*
 * The review's blocker B1.  TMDb and TheTVDB list a season's and a sequel's
 * name among a series' other names ("Tenkuu Rangers 2nd Season", "Tenkuu
 * Rangers II", "Tenkuu Rangers Rising").  A number under such a name counts
 * that part's episodes, so it is never the series' absolute number: refused
 * with a code of its own, whether reading the name by itself found the number
 * or the reading after the series' name did.  Under the series' own title and
 * original title, and under another name that is none of the series' names
 * with more after it, the number is read as before.
 * Spec (tv delta): A name of part of the series.
 */
static void
test_match_anime_part_name(void)
{
	static const gchar *const names[] = {
		"Tenkuu Rangers", "Sora no Senshi", "Tenkuu Rangers 2nd Season", "Tenkuu Rangers II", "Tenkuu Rangers Rising",
		"Zoku Rangers", "Zoku Tenkuu Rangers", "Shin Sora no Senshi Kai"
	};
	static const gchar *const long_original[] = { "Tenkuu Rangers", "Tenkuu Rangers: Sora no Senshi" };
	static const struct
	{
		const gchar *name;
		gboolean     ok;
		const gchar *says;	/* refused: the name it is numbered under; taken: its label */
	} cases[] = {
		/* Read by itself as an absolute number, the title matched by the
		 * other name: was "it holds S02E01 (absolute 5 ...)". */
		{ "[Tsuki] Tenkuu Rangers 2nd Season - 05 [1080p]", FALSE, "under the name \"Tenkuu Rangers 2nd Season\"" },
		{ "[Tsuki] Tenkuu Rangers Rising - 05 [1080p]", FALSE, "under the name \"Tenkuu Rangers Rising\"" },
		{ "[Tsuki] Tenkuu Rangers II - 05 [1080p]", FALSE, "under the name \"Tenkuu Rangers II\"" },
		{ "Tenkuu.Rangers.Rising.E05.1080p.WEB.H264-GRP", FALSE, "under the name \"Tenkuu Rangers Rising\"" },
		{ "[Tsuki] Tenkuu Rangers II - 01-04 [1080p]", FALSE, "under the name \"Tenkuu Rangers II\"" },
		/* Read after the series' name, the longest name taken. */
		{ "[Tsuki] Tenkuu Rangers II 05 [1080p]", FALSE, "under the name \"Tenkuu Rangers II\"" },
		{ "Tenkuu Rangers Rising 05 [1080p]", FALSE, "under the name \"Tenkuu Rangers Rising\"" },
		/* A word before one of the series' names, and words on both sides
		 * (the second review: these were read as S02E01). */
		{ "[Tsuki] Zoku Tenkuu Rangers - 05 [1080p]", FALSE, "under the name \"Zoku Tenkuu Rangers\"" },
		{ "[Tsuki] Zoku Tenkuu Rangers 05 [1080p]", FALSE, "under the name \"Zoku Tenkuu Rangers\"" },
		{ "[Tsuki] Shin Sora no Senshi Kai - 05 [1080p]", FALSE, "under the name \"Shin Sora no Senshi Kai\"" },
		/* The series' own names, and another name that is not one of them
		 * with more after it. */
		{ "[Tsuki] Tenkuu Rangers - 05 [1080p]", TRUE, "S02E01" },
		{ "[Tsuki] Tenkuu Rangers 05 [1080p]", TRUE, "S02E01" },
		{ "[Tsuki] Sora no Senshi - 05 [1080p]", TRUE, "S02E01" },
		{ "[Tsuki] Zoku Rangers - 05 [1080p]", TRUE, "S02E01" },
		{ "[Tsuki] Zoku Rangers 05 [1080p]", TRUE, "S02E01" },
		/* A season and an episode are matched by them alone, under any name. */
		{ "Tenkuu.Rangers.Rising.S02E01.1080p.WEB.H264-GRP", TRUE, "S02E01" },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		read.titles = names;
		read.n_titles = G_N_ELEMENTS(names);
		read.n_own = 2;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		if (holds.ok != cases[i].ok || (!holds.ok && g_strcmp0(holds.code, "absolute_part_name") != 0) ||
		    (holds.ok && g_strcmp0(holds.label, cases[i].says) != 0))
			g_error("%s: got %s %s (%s)", cases[i].name, holds.ok ? "a match" : holds.code,
				holds.label != NULL ? holds.label : "", holds.text);
		if (!holds.ok) {
			assert_contains(holds.text, cases[i].says);
			assert_contains(holds.text, "a part of the series");
			g_assert_cmpuint(holds.indices->len, ==, 0);
			g_assert_false(holds.by_absolute);
			g_assert_false(holds.pack);
		}
		axis_tv_holds_clear(&holds);
	}
	/* An original title that is the title with more after it is the series'
	 * own name; the same name among the other names is a part's. */
	{
		const gchar *name = "[Tsuki] Tenkuu Rangers Sora no Senshi - 05 [1080p]";
		g_autoptr(AxisRelease) parsed = axis_release_parse(name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, name);
		read.titles = long_original;
		read.n_titles = G_N_ELEMENTS(long_original);
		read.n_own = 2;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_true(holds.ok);
		g_assert_cmpstr(holds.label, ==, "S02E01");
		axis_tv_holds_clear(&holds);
		read.n_own = 1;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_part_name");
		axis_tv_holds_clear(&holds);
	}
}

/*
 * The second review's blocker: the name read by itself and the name read
 * after the series' names must say the same number under the same name.  With
 * "Tenkuu Rangers 3" among the other names, "Tenkuu Rangers - 3 - 05" is
 * absolute 3 by itself and 5 of "Tenkuu Rangers 3" after a title: neither is
 * taken (it was "it holds S01E03").
 */
static void
test_match_anime_two_readings(void)
{
	static const gchar *const names[] = { "Tenkuu Rangers", "Sora no Senshi", "Tenkuu Rangers 3", "Sora no Senshi II" };
	static const struct
	{
		const gchar *name;
		gboolean     ok;
		const gchar *code;
	} cases[] = {
		{ "[Tsuki] Tenkuu Rangers - 3 - 05 [1080p]", FALSE, "absolute_not_read" },
		{ "[Tsuki] Sora no Senshi - 2 - 05 [1080p]", FALSE, "absolute_not_read" },
		/* The two readings agree. */
		{ "[Tsuki] Tenkuu Rangers - 05 [1080p]", TRUE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 05v2 [1080p][A1B2C3D4]", TRUE, NULL },
		{ "Tenkuu.Rangers.E05.1080p.WEB.H264-GRP", TRUE, NULL },
		{ "[Tsuki] Tenkuu Rangers - Episode 05 [1080p]", TRUE, NULL },
		{ "[KumoSubs] Tenkuu Rangers (01-04) [1080p] [Batch]", TRUE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 05 - The Long Night [1080p]", TRUE, NULL },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		read.titles = names;
		read.n_titles = G_N_ELEMENTS(names);
		read.n_own = 1;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		if (holds.ok != cases[i].ok || (!holds.ok && g_strcmp0(holds.code, cases[i].code) != 0))
			g_error("%s: got %s %s (%s)", cases[i].name, holds.ok ? "a match" : holds.code,
				holds.label != NULL ? holds.label : "", holds.text);
		if (!holds.ok)
			assert_contains(holds.text, "can be read two ways");
		axis_tv_holds_clear(&holds);
	}
}

/*
 * The lead's rule after the second review.  A number under a name that is
 * not the series' title or original title (a romanised name, or the name of a
 * part that no rule of names can tell from one: "Rising Storm - 05") is a
 * search's to take only when it cannot be a part's own count: the series has
 * one season, or the number is higher than the episode count of every season
 * after the first.  Otherwise it is matched and left to a person
 * (AxisTvHolds.person_only).  A name with punctuation the title does not have
 * ("Tenkuu Rangers!!") is not the title for this.  And whatever was read only
 * because it follows the series' name is a person's too.
 * (The series here: season 1 of 4, season 2 of 5, season 3 of 1.)
 */
static void
test_match_anime_other_name(void)
{
	static const gchar *const names[] = { "Tenkuu Rangers: Sky", "Tenkuu Rangers", "Sora no Senshi", "Rising Storm" };
	static const struct
	{
		const gchar *name;
		const gchar *label;
		gboolean     person_only;
	} cases[] = {
		/* The title and the original title: a search's, whatever the number. */
		{ "[Tsuki] Tenkuu Rangers - 05 [1080p]", "S02E01", FALSE },
		{ "[Tsuki] Tenkuu Rangers Sky - 05 [1080p]", "S02E01", FALSE },
		{ "[Tsuki] Tenkuu Rangers - Sky - 05 [1080p]", "S02E01", FALSE },
		{ "Tenkuu.Rangers.E02.1080p.WEB.H264-GRP", "S01E02", FALSE },
		/* Another name: 5 can be the fifth of season 2 (which has 5). */
		{ "[Tsuki] Sora no Senshi - 05 [1080p]", "S02E01", TRUE },
		{ "[Tsuki] Rising Storm - 05 [1080p]", "S02E01", TRUE },
		{ "[Tsuki] Rising Storm - 01 [1080p]", "S01E01", TRUE },
		{ "Sora.no.Senshi.E03.1080p.WEB.H264-GRP", "S01E03", TRUE },
		/* 6 and 7 are more than any later season has. */
		{ "[Tsuki] Sora no Senshi - 06 [1080p]", "S02E02", FALSE },
		{ "[Tsuki] Rising Storm - 07 [1080p]", "S02E03", FALSE },
		/* Punctuation the title does not have. */
		{ "[Tsuki] Tenkuu Rangers!! - 05 [1080p]", "S02E01", TRUE },
		{ "[Tsuki] Tenkuu Rangers' - 05 [1080p]", "S02E01", TRUE },
		{ "[Tsuki] Tenkuu Rangers!! - 06 [1080p]", "S02E02", FALSE },
		/* Read only because it follows the series' name: always a person's. */
		{ "[Tsuki] Tenkuu Rangers 06 [1080p]", "S02E02", TRUE },
		{ "[Tsuki] Sora no Senshi 06 [1080p]", "S02E02", TRUE },
		{ "Tenkuu Rangers - 7 [1080p]", "S02E03", TRUE },
		/* A season and an episode: never. */
		{ "Sora.no.Senshi.S02E01.1080p.WEB.H264-GRP", "S02E01", FALSE },
		{ NULL, NULL, FALSE }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		read.titles = names;
		read.n_titles = G_N_ELEMENTS(names);
		read.n_own = 2;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		if (!holds.ok || g_strcmp0(holds.label, cases[i].label) != 0 || holds.person_only != cases[i].person_only)
			g_error("%s: got %s %s, a person's only: %d (%s)", cases[i].name, holds.ok ? "a match" : holds.code,
				holds.label != NULL ? holds.label : "", holds.person_only, holds.text);
		g_assert_true((holds.person_why != NULL) == holds.person_only);
		axis_tv_holds_clear(&holds);
		/* A series not marked: never set. */
		read.anime = FALSE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.person_only);
		g_assert_null(holds.person_why);
		axis_tv_holds_clear(&holds);
	}
	/* One season (and its specials): no later season whose count the number
	 * could be. */
	{
		const gchar *name = "[Tsuki] Sora no Senshi - 02 [1080p]";
		g_autoptr(AxisRelease) parsed = axis_release_parse(name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, name);
		read.titles = names;
		read.n_titles = G_N_ELEMENTS(names);
		read.n_own = 2;
		axis_tv_match_release_anime(parsed, rangers, 5, &read, &holds);
		g_assert_true(holds.ok);
		g_assert_cmpstr(holds.label, ==, "S01E02");
		g_assert_false(holds.person_only);
		axis_tv_holds_clear(&holds);
	}
}

/*
 * add-anime-support, A3 (design.md section 6, Batches): for a caller that
 * reads batches (a search judging a release), a range of absolute numbers
 * that is not whole seasons holds exactly the episodes its numbers map to, in
 * one season or across two, and is never a pack.  Every refusal of slice A2
 * still refuses: a number that maps to nothing or to an episode not aired, a
 * part's name, a season with a range, a word after the range, a range
 * backwards.  What A2 left to a person stays a person's: a range read only
 * because it follows the series' name, and a low range under a name that is
 * not the title.  A caller that does not read batches is refused as before,
 * and a series not marked is matched as any series whatever is set.
 * Spec (tv delta): A batch across seasons; A number that does not map.
 * (The series: season 1 of 4, numbered 1 to 4; season 2 of 5, numbered 5 to
 * 9, of which 8 has not aired and 9 has no date.)
 */
static void
test_match_anime_batch(void)
{
	static const gchar *const names[] = {
		"Tenkuu Rangers", "Sora no Senshi", "Tenkuu Rangers 2nd Season"
	};
	static const struct
	{
		const gchar *name;
		gboolean     ok;
		const gchar *code;	/* when not ok */
		const gchar *label;	/* when ok */
		const gchar *ids;
		gboolean     pack;
		gboolean     person_only;
		const gchar *says;
	} cases[] = {
		{ "[Tsuki] Tenkuu Rangers - 01-03 [1080p]", TRUE, NULL, "S01E01-S01E03", "11,12,13", FALSE, FALSE, "it holds S01E01 "
		  "to S01E03 (absolute 1 to 3 are S01E01 to S01E03 by TheTVDB): a batch of 3 episodes" },
		/* Across two seasons: each number its own episode, by a lookup. */
		{ "[Hikari-Raws] Tenkuu Rangers - 03-06 [BD 1080p]", TRUE, NULL, "S01E03-S02E02", "13,14,15,16", FALSE, FALSE,
		  "it holds S01E03 to S02E02 (absolute 3 to 6 are S01E03 to S02E02 by TheTVDB): a batch of 4 episodes" },
		{ "[Tsuki] Tenkuu Rangers - 01-07 [1080p]", TRUE, NULL, "S01E01-S02E03", "11,12,13,14,15,16,17", FALSE, FALSE,
		  NULL },
		{ "[Tsuki] Tenkuu Rangers - 02 ~ 03 [1080p]", TRUE, NULL, "S01E02-S01E03", "12,13", FALSE, FALSE, NULL },
		/* Exactly a whole season is its pack, as in A2. */
		{ "[KumoSubs] Tenkuu Rangers (01-04) [1080p] [Batch]", TRUE, NULL, "S01", "11,12,13,14", TRUE, FALSE, NULL },
		/* One number that maps to an episode not aired, or to none, refuses
		 * the whole batch and names it. */
		{ "[Tsuki] Tenkuu Rangers - 01-12 [1080p]", FALSE, "absolute_unaired", NULL, NULL, FALSE, FALSE, "absolute 8 is "
		  "S02E04, which has not aired" },
		{ "[Tsuki] Tenkuu Rangers - 05-08 [1080p]", FALSE, "absolute_unaired", NULL, NULL, FALSE, FALSE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 300-302 [1080p]", FALSE, "absolute_unknown", NULL, NULL, FALSE, FALSE, "no episode of "
		  "the series has the absolute number 300 (the highest TheTVDB gives is 9)" },
		/* A2's refusals, for a range as for one number. */
		{ "[Tsuki] Tenkuu Rangers 2nd Season - 01-03 [1080p]", FALSE, "absolute_part_name", NULL, NULL, FALSE, FALSE,
		  "the name of a part of the series" },
		{ "[Tsuki] Tenkuu Rangers S2 - 01-03 [1080p]", FALSE, NULL, NULL, NULL, FALSE, FALSE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 01-03 Batch 1080p", FALSE, "absolute_not_read", NULL, NULL, FALSE, FALSE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 01-03 - 04 [1080p]", FALSE, NULL, NULL, NULL, FALSE, FALSE, NULL },
		{ "[Tsuki] Tenkuu Rangers - 01-03 (2011) [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, FALSE, "the year "
		  "in its name is 2011" },
		{ "Tenkuu Rangers 03-01 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, FALSE, NULL },
		{ "Tenkuu Rangers 2 01-03 [1080p]", FALSE, "absolute_not_read", NULL, NULL, FALSE, FALSE, "it is not read as an "
		  "episode" },
		/* A person's only, as A2 left it: read only because it follows the
		 * series' name; and under a name that is not the title, a range that
		 * begins low enough to be a later season's own count. */
		{ "Tenkuu Rangers 01-03 [1080p]", TRUE, NULL, "S01E01-S01E03", "11,12,13", FALSE, TRUE, NULL },
		{ "[Tsuki] Sora no Senshi - 01-03 [1080p]", TRUE, NULL, "S01E01-S01E03", "11,12,13", FALSE, TRUE, NULL },
		{ "[Tsuki] Sora no Senshi - 05-07 [1080p]", TRUE, NULL, "S02E01-S02E03", "15,16,17", FALSE, TRUE, NULL },
		{ "[Tsuki] Sora no Senshi - 06-07 [1080p]", TRUE, NULL, "S02E02-S02E03", "16,17", FALSE, FALSE, NULL },
		{ NULL, FALSE, NULL, NULL, NULL, FALSE, FALSE, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(cases[i].name);
		g_autofree gchar *ids = NULL;
		AxisTvAnimeRead read;
		AxisTvHolds holds;
		AxisTvHolds plain;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		read.titles = names;
		read.n_titles = G_N_ELEMENTS(names);
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		if (holds.ok != cases[i].ok || (!holds.ok && cases[i].code != NULL && g_strcmp0(holds.code, cases[i].code) != 0))
			g_error("%s: expected %s, got %s %s (%s)", cases[i].name, cases[i].ok ? "a match" : cases[i].code != NULL ?
				cases[i].code : "a refusal", holds.ok ? "a match" : holds.code, holds.label != NULL ? holds.label : "",
				holds.text != NULL ? holds.text : "");
		if (cases[i].says != NULL)
			assert_contains(holds.text, cases[i].says);
		if (holds.ok) {
			ids = held_ids(rangers, &holds);
			if (g_strcmp0(holds.label, cases[i].label) != 0 || strcmp(ids, cases[i].ids) != 0 ||
			    holds.pack != cases[i].pack || holds.person_only != cases[i].person_only)
				g_error("%s: got %s, episodes %s, a pack: %d, a person's only: %d", cases[i].name, holds.label, ids,
					holds.pack, holds.person_only);
			g_assert_true(holds.batch);
			g_assert_true(holds.by_absolute);
			g_assert_nonnull(holds.absolute_text);
			g_assert_true((holds.person_why != NULL) == holds.person_only);
			if (!holds.pack) {
				/* The seasons of its first and last episodes; never a season
				 * as a whole. */
				g_assert_cmpint(holds.season_first, ==, rangers[g_array_index(holds.indices, guint, 0)].season);
				g_assert_cmpint(holds.season_last, ==,
					rangers[g_array_index(holds.indices, guint, holds.indices->len - 1)].season);
			}
		} else {
			g_assert_cmpuint(holds.indices->len, ==, 0);
			g_assert_false(holds.by_absolute);
			g_assert_null(holds.absolute_text);
			g_assert_false(holds.person_only);
		}
		/* A caller that does not read batches (a file's name, a grab by
		 * force): a batch that would be taken is refused as before A3, and
		 * everything else is the same answer. */
		read.batches = FALSE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &plain);
		if (holds.ok && !holds.pack) {
			g_assert_false(plain.ok);
			g_assert_cmpstr(plain.code, ==, "absolute_batch");
			g_assert_cmpuint(plain.indices->len, ==, 0);
			g_assert_false(plain.by_absolute);
		} else {
			g_assert_cmpint(plain.ok, ==, holds.ok);
			g_assert_cmpstr(plain.code, ==, holds.code);
			g_assert_cmpstr(plain.text, ==, holds.text);
		}
		axis_tv_holds_clear(&holds);
		axis_tv_holds_clear(&plain);
		/* A series that is not marked: as any series, whatever is set. */
		read.anime = FALSE;
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		axis_tv_match_release(parsed, rangers, G_N_ELEMENTS(rangers), &plain);
		g_assert_cmpint(holds.ok, ==, plain.ok);
		g_assert_cmpstr(holds.code, ==, plain.code);
		g_assert_cmpstr(holds.text, ==, plain.text);
		g_assert_false(holds.by_absolute);
		g_assert_false(holds.batch);
		axis_tv_holds_clear(&holds);
		axis_tv_holds_clear(&plain);
	}
	/* Numbers that are not usable: no batch either. */
	{
		const gchar *name = "[Tsuki] Tenkuu Rangers - 01-03 [1080p]";
		g_autoptr(AxisRelease) parsed = axis_release_parse(name);
		gint shared[G_N_ELEMENTS(rangers)];
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		memcpy(shared, rangers_absolute, sizeof(shared));
		shared[3] = 2;
		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, name);
		read.absolute = shared;
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_unusable");
		axis_tv_holds_clear(&holds);
		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_NO_SOURCE, name);
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, rangers, G_N_ELEMENTS(rangers), &read, &holds);
		g_assert_false(holds.ok);
		g_assert_cmpstr(holds.code, ==, "absolute_unusable");
		axis_tv_holds_clear(&holds);
	}
	/* A long series (six seasons of 24): the spec's batch of 28. */
	{
		static const struct
		{
			const gchar *name;
			gboolean     ok;
			const gchar *label;
			guint        n;
			gboolean     person_only;
			const gchar *says;
		} longs[] = {
			{ "[Hikari-Raws] Tenkuu Rangers - 01-28 [BD 1080p]", TRUE, "S01E01-S02E04", 28, FALSE, "it holds S01E01 to "
			  "S02E04 (absolute 1 to 28 are S01E01 to S02E04 by TheTVDB): a batch of 28 episodes" },
			{ "Tenkuu Rangers 013-036 [BD 1080p]", TRUE, "S01E13-S02E12", 24, TRUE, NULL },
			{ "[Tsuki] Tenkuu Rangers - 01-48 [1080p]", TRUE, "S01-S02", 48, FALSE, NULL },
			{ "[Tsuki] Tenkuu Rangers - 100-150 [1080p]", FALSE, NULL, 0, FALSE, "no episode of the series has the absolute "
			  "number 145 (the highest TheTVDB gives is 144)" },
			{ NULL, FALSE, NULL, 0, FALSE, NULL }
		};
		AxisTvKnown known[144];
		gint absolute[144];

		for (i = 0; i < G_N_ELEMENTS(known); i++) {
			known[i].id = 100 + i;
			known[i].season = (gint)(i / 24) + 1;
			known[i].number = (gint)(i % 24) + 1;
			known[i].air_date = "2020-01-01";
			absolute[i] = (gint)i + 1;
		}
		for (i = 0; longs[i].name != NULL; i++) {
			g_autoptr(AxisRelease) parsed = axis_release_parse(longs[i].name);
			AxisTvAnimeRead read;
			AxisTvHolds holds;
			guint k;

			anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, longs[i].name);
			read.absolute = absolute;
			read.batches = TRUE;
			axis_tv_match_release_anime(parsed, known, G_N_ELEMENTS(known), &read, &holds);
			if (holds.ok != longs[i].ok || (holds.ok && (g_strcmp0(holds.label, longs[i].label) != 0 ||
			    holds.indices->len != longs[i].n || holds.person_only != longs[i].person_only)))
				g_error("%s: got %s %s, %u episodes, a person's only: %d (%s)", longs[i].name, holds.ok ? "a match" :
					holds.code, holds.label != NULL ? holds.label : "", holds.indices->len, holds.person_only, holds.text);
			if (longs[i].says != NULL)
				assert_contains(holds.text, longs[i].says);
			/* Each episode held is the one that has the next number. */
			for (k = 0; holds.ok && !holds.pack && k < holds.indices->len; k++)
				g_assert_cmpint(absolute[g_array_index(holds.indices, guint, k)], ==,
					absolute[g_array_index(holds.indices, guint, 0)] + (gint)k);
			axis_tv_holds_clear(&holds);
		}
	}
}

/*
 * The review of slice A3.  What stands after the number, in a bracket or
 * bare, can say that the number is not the series' absolute number or not one
 * episode's: a season or a part written as an ordinal or a word ("[2nd
 * Season]", "Second Season", "(Part 2)", "Cour 2", "[Season 1+2]"), and a word
 * for something that is no episode (a soundtrack, a manga, a trailer, a
 * sample, a recap).  And a range that reading a name by itself cuts short
 * ("01-12v2", "E01-E12", "01-12, 14") is not its first number alone.  None is
 * what a search may take, for one number or for a range.  (A long series: six
 * seasons of 24, numbered 1 to 144.)
 */
static void
test_match_anime_after_number(void)
{
	static const gchar *const refused[] = {
		/* A season or a part after the number. */
		"[Tsuki] Tenkuu Rangers - 01-12 [2nd Season] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 2nd Season [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 (2nd Season) [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [Second Season] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 Second Season [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [3rd Season] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [Season Two] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [Season 2] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [BD] [Season 2]",
		"[Tsuki] Tenkuu Rangers - 01-12 [S2] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 [Season 1+2]",
		"[Tsuki] Tenkuu Rangers - 01-12 [Part 2] [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 (Part 2) [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-12 Cour 2 [1080p]",
		"[Tsuki] Tenkuu Rangers - 13-24 (S2 01-12) [1080p]",
		"[Tsuki] Tenkuu Rangers - 05 [2nd Season] [1080p]",
		"[Tsuki] Tenkuu Rangers - 05 [Part 2] [1080p]",
		"[Tsuki] Tenkuu Rangers - 05 Cour 2 [1080p]",
		"[Tsuki] Tenkuu Rangers - 05 Second Season [1080p]",
		"[Tsuki] Tenkuu Rangers - 137 (3rd Season) [1080p]",
		"[Tsuki] Tenkuu Rangers 05 [2nd Season] [1080p]",
		"Tenkuu Rangers 001-030 [Part 2] [BD 1080p]",
		/* A range cut short by reading the name by itself. */
		"[Tsuki] Tenkuu Rangers - 01-12v2 [1080p]",
		"Tenkuu.Rangers.E01-E12.1080p.WEB.H264-GRP",
		"Tenkuu.Rangers.E01-12.1080p.WEB.H264-GRP",
		"[Tsuki] Tenkuu Rangers - 01-12, 14 [1080p]",
		"[Tsuki] Tenkuu Rangers - 05, 07 [1080p]",
		/* Something that is no episode. */
		"[Tsuki] Tenkuu Rangers - 01-13 OST [FLAC]",
		"[Tsuki] Tenkuu Rangers - 01-13 Manga [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-13 [Manga] [Digital]",
		"[Tsuki] Tenkuu Rangers - 01-13 Trailer [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-13 Sample [1080p]",
		"[Tsuki] Tenkuu Rangers - 01-13 Recap [1080p]",
		"[Tsuki] Tenkuu Rangers - 05 OST [FLAC]",
		"[Tsuki] Tenkuu Rangers - 05 [Soundtrack]",
		NULL
	};
	static const struct
	{
		const gchar *name;
		const gchar *label;
	} taken[] = {
		/* What none of that touches. */
		{ "[Tsuki] Tenkuu Rangers - 01-12 [1080p]", "S01E01-S01E12" },
		{ "[Tsuki] Tenkuu Rangers - 01-24 [BD 1080p]", "S01" },
		{ "[Tsuki] Tenkuu Rangers - 137 [1080p][A1B2C3D4]", "S06E17" },
		{ "[Tsuki] Tenkuu Rangers - 05 - Part of the Team [1080p]", "S01E05" },
		{ "[Tsuki] Tenkuu Rangers - 05 - The Long Night [1080p]", "S01E05" },
		{ "[Tsuki] Tenkuu Rangers - 05v2 [1080p]", "S01E05" },
		{ "[Tsuki] Tenkuu Rangers - 05 [720p-1080p]", "S01E05" },
		{ "Tenkuu.Rangers.E101.1080p.WEB.H264-GRP", "S05E05" },
		{ "Tenkuu.Rangers.S02E01.Part.2.1080p.WEB.H264-GRP", "S02E01" },
		{ NULL, NULL }
	};
	AxisTvKnown known[144];
	gint absolute[144];
	guint pass;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(known); i++) {
		known[i].id = 100 + i;
		known[i].season = (gint)(i / 24) + 1;
		known[i].number = (gint)(i % 24) + 1;
		known[i].air_date = "2020-01-01";
		absolute[i] = (gint)i + 1;
	}
	/* As a search judges a release (batches read), and as a file's name is
	 * read (not): refused both ways. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; refused[i] != NULL; i++) {
			g_autoptr(AxisRelease) parsed = axis_release_parse(refused[i]);
			AxisTvAnimeRead read;
			AxisTvHolds holds;

			anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, refused[i]);
			read.absolute = absolute;
			read.batches = pass == 0;
			axis_tv_match_release_anime(parsed, known, G_N_ELEMENTS(known), &read, &holds);
			if (holds.ok)
				g_error("%s: got a match %s (%s), a person's only: %d", refused[i], holds.label, holds.text,
					holds.person_only);
			g_assert_nonnull(holds.text);
			g_assert_cmpuint(holds.indices->len, ==, 0);
			g_assert_false(holds.by_absolute);
			axis_tv_holds_clear(&holds);
		}
	}
	for (i = 0; taken[i].name != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(taken[i].name);
		AxisTvAnimeRead read;
		AxisTvHolds holds;

		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, taken[i].name);
		read.absolute = absolute;
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, known, G_N_ELEMENTS(known), &read, &holds);
		if (!holds.ok || g_strcmp0(holds.label, taken[i].label) != 0 || holds.person_only)
			g_error("%s: got %s %s (%s), a person's only: %d", taken[i].name, holds.ok ? "a match" : holds.code,
				holds.label != NULL ? holds.label : "", holds.text, holds.person_only);
		axis_tv_holds_clear(&holds);
	}
	/* A series that is not marked: none of this is looked at. */
	for (i = 0; refused[i] != NULL; i++) {
		g_autoptr(AxisRelease) parsed = axis_release_parse(refused[i]);
		AxisTvAnimeRead read;
		AxisTvHolds holds;
		AxisTvHolds plain;

		anime_read(&read, FALSE, AXIS_TV_ABSOLUTE_USABLE, refused[i]);
		read.absolute = absolute;
		read.batches = TRUE;
		axis_tv_match_release_anime(parsed, known, G_N_ELEMENTS(known), &read, &holds);
		axis_tv_match_release(parsed, known, G_N_ELEMENTS(known), &plain);
		g_assert_cmpint(holds.ok, ==, plain.ok);
		g_assert_cmpstr(holds.code, ==, plain.code);
		g_assert_cmpstr(holds.text, ==, plain.text);
		axis_tv_holds_clear(&holds);
		axis_tv_holds_clear(&plain);
	}
}

static AxisMetaTitle *
record_of(
	const gchar *title,
	const gchar *original
){
	AxisMetaTitle *record = axis_meta_title_new(AXIS_META_KIND_SERIES);

	record->title = g_strdup(title);
	record->original_title = g_strdup(original);
	record->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	return record;
}

static void
record_alt(
	AxisMetaTitle *record,
	const gchar   *title,
	const gchar   *language,
	const gchar   *country
){
	AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

	alt->title = g_strdup(title);
	alt->language = g_strdup(language);
	alt->country = g_strdup(country);
	g_ptr_array_add(record->alt_titles, alt);
}

/*
 * add-anime-support, A3 (design.md section 6, The request): the words a
 * search asks for an absolute number, padded to the series' width; and the one
 * other name asked by, by each branch of its rule: a Latin original title; a
 * romanised other name marked as Japanese, the first by its text; none.
 * Spec (indexers delta): The words; Padded.
 */
static void
test_anime_search_words(void)
{
	static const gint short_series[] = { 0, 1, 2, 99 };
	static const gint long_series[] = { 0, 1, 2, 100 };
	g_autofree gchar *two = axis_tv_absolute_words("Tenkuu Rangers", 5, 2);
	g_autofree gchar *three = axis_tv_absolute_words("Tenkuu Rangers", 5, 3);
	g_autofree gchar *wide = axis_tv_absolute_words(" Tenkuu Rangers ", 137, 2);
	g_autofree gchar *four = axis_tv_absolute_words("Tenkuu Rangers", 1047, 3);

	g_assert_cmpstr(two, ==, "Tenkuu Rangers 05");
	g_assert_cmpstr(three, ==, "Tenkuu Rangers 005");
	g_assert_cmpstr(wide, ==, "Tenkuu Rangers 137");
	g_assert_cmpstr(four, ==, "Tenkuu Rangers 1047");
	g_assert_null(axis_tv_absolute_words("Tenkuu Rangers", 0, 2));
	g_assert_null(axis_tv_absolute_words("Tenkuu Rangers", 10000, 2));
	g_assert_null(axis_tv_absolute_words("", 5, 2));
	g_assert_null(axis_tv_absolute_words(NULL, 5, 2));
	g_assert_cmpuint(axis_tv_absolute_digits(short_series, G_N_ELEMENTS(short_series)), ==, 2);
	g_assert_cmpuint(axis_tv_absolute_digits(long_series, G_N_ELEMENTS(long_series)), ==, 3);
	g_assert_cmpuint(axis_tv_absolute_digits(NULL, 0), ==, 2);

	g_assert_true(axis_tv_name_is_latin("Sora no Senshi"));
	g_assert_true(axis_tv_name_is_latin("Tenk\xc5\xab Rangers 2!"));
	g_assert_false(axis_tv_name_is_latin("\xe5\xa4\xa9\xe7\xa9\xba Rangers"));
	g_assert_false(axis_tv_name_is_latin("\xd0\xa0\xd0\xb5\xd0\xb9\xd0\xbd\xd0\xb4\xd0\xb6"));
	g_assert_false(axis_tv_name_is_latin("137"));
	g_assert_false(axis_tv_name_is_latin(""));
	g_assert_false(axis_tv_name_is_latin(NULL));
	g_assert_false(axis_tv_name_is_latin("\xff\xfe"));

	/* The original title, in Latin letters. */
	{
		g_autoptr(AxisMetaTitle) record = record_of("Sky Rangers", "Tenkuu Rangers");

		record_alt(record, "Aozora Rangers", "ja", "JP");
		g_assert_cmpstr(axis_tv_anime_second_name(record), ==, "Tenkuu Rangers");
	}
	/* An original title not in Latin letters: the other name marked as
	 * Japanese that is, the first by its text; one marked otherwise, and one
	 * not in Latin letters, are passed over. */
	{
		g_autoptr(AxisMetaTitle) record = record_of("Sky Rangers", "\xe5\xa4\xa9\xe7\xa9\xba\xe3\x83\xac\xe3\x83\xb3");

		record_alt(record, "Himmelsranger", "de", "DE");
		record_alt(record, "Tenkuu Rangers", "ja", NULL);
		record_alt(record, "Sora no Rangers", NULL, "JP");
		record_alt(record, "\xe5\xa4\xa9\xe7\xa9\xba", "ja", "JP");
		g_assert_cmpstr(axis_tv_anime_second_name(record), ==, "Sora no Rangers");
	}
	/* An original title that is the title again: the other names. */
	{
		g_autoptr(AxisMetaTitle) record = record_of("Sky Rangers", "Sky Rangers!");

		record_alt(record, "Tenkuu Rangers", "ja", "JP");
		g_assert_cmpstr(axis_tv_anime_second_name(record), ==, "Tenkuu Rangers");
	}
	/* A part's name (the title with more after it), under which no absolute
	 * number is read, is not asked by; nor the title again. */
	{
		g_autoptr(AxisMetaTitle) record = record_of("Tenkuu Rangers", NULL);

		record_alt(record, "Tenkuu Rangers 2nd Season", "ja", "JP");
		record_alt(record, "TENKUU RANGERS", "ja", "JP");
		g_assert_null(axis_tv_anime_second_name(record));
	}
	/* None. */
	{
		g_autoptr(AxisMetaTitle) record = record_of("Sky Rangers", NULL);
		g_autoptr(AxisMetaTitle) bare = axis_meta_title_new(AXIS_META_KIND_SERIES);

		record_alt(record, "Himmelsranger", "de", "DE");
		g_assert_null(axis_tv_anime_second_name(record));
		g_assert_null(axis_tv_anime_second_name(bare));
		g_assert_null(axis_tv_anime_second_name(NULL));
	}
}

/*
 * The review's blocker B2, the part that is a rule of names: a name the
 * series would claim by the number after its own name is not claimed when
 * another title the search looked up could own it: the same name (the series'
 * other name is another series' title), a longer name that takes the number
 * in, or the series' name followed by that very number, however it is written
 * ("Tenkuu Rangers 2" for a name that says "Tenkuu Rangers 02": their title
 * keys differ, so comparing keys never saw it).
 */
static void
test_anime_other_title(void)
{
	static const struct
	{
		const gchar *name;
		const gchar *other;
		const gchar *says;	/* NULL: still claimed */
	} cases[] = {
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers 2", "the number 2 may be part of that name" },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers II", "\"Tenkuu Rangers II\"" },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers 2nd Season", "the number 2 may be part of that name" },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers: Season 2", "the number 2 may be part of that name" },
		{ "Tenkuu.Rangers.02-04.1080p.WEB", "Tenkuu Rangers 2", "the number 2 may be part of that name" },
		{ "[Tsuki] Sora no Senshi 06 [1080p]", "Sora no Senshi", "the name begins with that too" },
		{ "[Tsuki] Sora no Senshi 06 [1080p]", "SORA NO SENSHI!", "the name begins with that too" },
		{ "Tenkuu.Rangers.13.BluRay.1080p.x264-GRP", "Tenkuu Rangers 13", "the name begins with that too" },
		/* Another number, another name, a part of a word: still the series'. */
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers 3", NULL },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers 20", NULL },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu Rangers Rising", NULL },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Hoshi no Kagami", NULL },
		{ "[Tsuki] Tenkuu Rangers 02 [1080p]", "Tenkuu", NULL },
		/* Not a name the series claims: nothing to say. */
		{ "[Tsuki] Tenkuu Rangers - Movie [1080p]", "Tenkuu Rangers 2", NULL },
		{ NULL, NULL, NULL }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		const gchar *others[2];
		g_autofree gchar *why = NULL;
		AxisTvAnimeRead read;

		others[0] = "Kaze no Machi";
		others[1] = cases[i].other;
		anime_read(&read, TRUE, AXIS_TV_ABSOLUTE_USABLE, cases[i].name);
		why = axis_tv_anime_other_title(&read, others, G_N_ELEMENTS(others));
		if ((why != NULL) != (cases[i].says != NULL))
			g_error("%s against %s: expected %s, got %s", cases[i].name, cases[i].other,
				cases[i].says != NULL ? cases[i].says : "claimed", why != NULL ? why : "claimed");
		if (why != NULL)
			assert_contains(why, cases[i].says);
		/* No other title, and a series not marked: nothing. */
		g_assert_null(axis_tv_anime_other_title(&read, NULL, 0));
		read.anime = FALSE;
		g_assert_null(axis_tv_anime_other_title(&read, others, G_N_ELEMENTS(others)));
	}
}

/* ── States ─────────────────────────────────────────────────────── */

static AxisTvSeries *
series_new(void)
{
	AxisTvSeries *s = g_new0(AxisTvSeries, 1);

	s->id = 1;
	s->title = g_strdup("Harbour Lights");
	s->year = 2024;
	s->profile_id = 1;
	s->profile_name = g_strdup("Balanced");
	s->profile_usable = TRUE;
	s->profile_cutoff = AXIS_QUALITY_WEBDL_1080P;
	s->profile_upgrades = TRUE;
	s->root_id = 3;
	s->root_name = g_strdup("Series");
	s->volume_id = 2;
	s->volume_name = g_strdup("Disk one");
	s->volume_state = AXIS_VOLUME_ONLINE;
	return s;
}

static AxisTvEpisode *
episode_new(const gchar *air_date)
{
	AxisTvEpisode *e = g_new0(AxisTvEpisode, 1);

	e->id = 7;
	e->series_id = 1;
	e->season = 2;
	e->number = 2;
	e->title = g_strdup("Storm");
	e->air_date = g_strdup(air_date);
	e->monitored = TRUE;
	e->season_monitored = TRUE;
	e->file_quality = AXIS_QUALITY_UNKNOWN;
	e->worst_quality = AXIS_QUALITY_UNKNOWN;
	return e;
}

static AxisTvState
derive(
	const AxisTvEpisode  *e,
	const AxisTvSeries   *s,
	gchar               **reason
){
	AxisTvRules rules;

	axis_tv_rules_init(&rules, NOW_US);
	return axis_tv_episode_derive(e, s, &rules, reason);
}

static void
assert_derived(
	const AxisTvEpisode *e,
	const AxisTvSeries  *s,
	AxisTvState          expected,
	const gchar         *in_reason
){
	g_autofree gchar *reason = NULL;
	AxisTvState state = derive(e, s, &reason);

	if (state != expected)
		g_error("expected %s, got %s (%s)", axis_tv_state_name(expected), axis_tv_state_name(state),
			reason != NULL ? reason : "");
	g_assert_nonnull(reason);
	if (in_reason != NULL)
		assert_contains(reason, in_reason);
}

static void
test_aired(void)
{
	g_autoptr(AxisTvSeries) s = series_new();
	AxisTvEpisode *e = episode_new("2026-11-13");
	AxisTvRules rules;
	gint64 from = 0;

	axis_tv_rules_init(&rules, NOW_US);
	g_assert_cmpstr(rules.today, ==, "2026-11-14");
	/* Aired yesterday: searched for. */
	g_assert_true(axis_tv_episode_aired(e, &rules, &from));
	g_assert_cmpint(from, ==, (NOW_US - 12 * 3600 * (gint64)G_USEC_PER_SEC) - 24 * 3600 * (gint64)G_USEC_PER_SEC +
		3600 * (gint64)G_USEC_PER_SEC);
	assert_derived(e, s, AXIS_TV_WANTED, "Aired on 2026-11-13");

	/* Airs today: not before the delay has passed (the day's start, UTC). */
	g_free(e->air_date);
	e->air_date = g_strdup("2026-11-14");
	rules.air_delay_s = 13 * 3600;
	g_assert_false(axis_tv_episode_aired(e, &rules, &from));
	rules.air_delay_s = 3600;
	g_assert_true(axis_tv_episode_aired(e, &rules, &from));
	{
		g_autofree gchar *reason = NULL;

		axis_tv_rules_init(&rules, NOW_US);
		rules.air_delay_s = 13 * 3600;
		g_assert_cmpint(axis_tv_episode_derive(e, s, &rules, &reason), ==, AXIS_TV_NOT_AIRED);
		assert_contains(reason, "searched for from 2026-11-14 13:00 UTC");
	}

	/* Airs next week. */
	g_free(e->air_date);
	e->air_date = g_strdup("2026-11-20");
	assert_derived(e, s, AXIS_TV_NOT_AIRED, "Airs on 2026-11-20.");
	/* No air date: never searched for by itself. */
	g_clear_pointer(&e->air_date, g_free);
	g_assert_false(axis_tv_episode_aired(e, &rules, &from));
	g_assert_cmpint(from, ==, 0);
	assert_derived(e, s, AXIS_TV_NOT_AIRED, "No air date is known");
	/* Not monitored comes first. */
	e->monitored = FALSE;
	assert_derived(e, s, AXIS_TV_UNMONITORED, "Not monitored");
	axis_tv_episode_free(e);
}

static void
test_states(void)
{
	g_autoptr(AxisTvSeries) s = series_new();
	AxisTvEpisode *e = episode_new("2026-11-06");

	assert_derived(e, s, AXIS_TV_WANTED, NULL);

	/* Searching: a search that holds it is queued, or running. */
	e->search_queued = TRUE;
	assert_derived(e, s, AXIS_TV_SEARCHING, "queued");
	e->search_running = TRUE;
	assert_derived(e, s, AXIS_TV_SEARCHING, "running");
	e->search_queued = FALSE;
	e->search_running = FALSE;

	/* The last search's outcome. */
	e->search_outcome = g_strdup("none_acceptable");
	e->search_text = g_strdup("Nothing acceptable was found.");
	assert_derived(e, s, AXIS_TV_ATTENTION, "Nothing acceptable");
	g_clear_pointer(&e->search_outcome, g_free);
	g_clear_pointer(&e->search_text, g_free);

	/* Downloading, with the share done. */
	e->grab_id = 40;
	e->grab_release = g_strdup("Harbour.Lights.S02E01.1080p.WEB-DL.DDP5.1.H.264-GRP");
	e->download_id = 50;
	e->download_known = TRUE;
	e->download_size = 1000;
	e->download_done = 400;
	e->client_name = g_strdup("Usenet");
	assert_derived(e, s, AXIS_TV_DOWNLOADING, "40% at Usenet");
	/* Handed over, not yet at the client. */
	e->download_id = 0;
	assert_derived(e, s, AXIS_TV_DOWNLOADING, "being handed");
	e->download_id = 50;

	/* Arrived: to import; an import that needs a person: attention. */
	e->grab_completed = TRUE;
	assert_derived(e, s, AXIS_TV_TO_IMPORT, "waits for its import");
	e->import_state = g_strdup("partial");
	e->import_text = g_strdup("Partly imported: 1 of 2 files are in the library.");
	assert_derived(e, s, AXIS_TV_ATTENTION, "Partly imported");
	/* Partly imported, an episode that has its file is in the library (the
	 * grab stays open for the others). */
	e->n_files = 1;
	e->file_quality = AXIS_QUALITY_BLURAY_1080P;
	e->worst_quality = AXIS_QUALITY_BLURAY_1080P;
	assert_derived(e, s, AXIS_TV_IN_LIBRARY, NULL);
	g_assert_true(axis_tv_episode_in_flight(e));
	e->n_files = 0;
	e->file_quality = AXIS_QUALITY_UNKNOWN;
	e->worst_quality = AXIS_QUALITY_UNKNOWN;
	g_free(e->import_state);
	e->import_state = g_strdup("blocked");
	assert_derived(e, s, AXIS_TV_ATTENTION, "needs a person");
	g_clear_pointer(&e->import_state, g_free);
	g_clear_pointer(&e->import_text, g_free);

	/* A download given up on and still at its client. */
	e->grab_id = 0;
	e->grab_completed = FALSE;
	e->removal_pending = TRUE;
	g_assert_true(axis_tv_episode_in_flight(e));
	assert_derived(e, s, AXIS_TV_ATTENTION, "still at its download client");
	e->removal_pending = FALSE;
	g_assert_false(axis_tv_episode_in_flight(e));

	/* In the library: at the cutoff, below it, of unknown quality. */
	e->n_files = 1;
	e->file_quality = AXIS_QUALITY_WEBDL_1080P;
	e->worst_quality = AXIS_QUALITY_WEBDL_1080P;
	assert_derived(e, s, AXIS_TV_IN_LIBRARY, "In the library as");
	g_assert_false(axis_tv_episode_upgradable(e, s));
	e->file_quality = AXIS_QUALITY_HDTV_720P;
	e->worst_quality = AXIS_QUALITY_HDTV_720P;
	g_assert_true(axis_tv_episode_upgradable(e, s));
	assert_derived(e, s, AXIS_TV_UPGRADE_WANTED, "below the cutoff of its quality profile");
	/* Not monitored: no upgrade sought. */
	e->monitored = FALSE;
	assert_derived(e, s, AXIS_TV_IN_LIBRARY, NULL);
	e->monitored = TRUE;
	/* A profile that does not upgrade. */
	s->profile_upgrades = FALSE;
	assert_derived(e, s, AXIS_TV_IN_LIBRARY, NULL);
	s->profile_upgrades = TRUE;
	/* Nothing says a release would be better than a file of unknown
	 * quality: it is never replaced by itself. */
	e->file_quality = AXIS_QUALITY_UNKNOWN;
	e->worst_quality = AXIS_QUALITY_UNKNOWN;
	g_assert_false(axis_tv_episode_upgradable(e, s));
	assert_derived(e, s, AXIS_TV_IN_LIBRARY, NULL);
	e->n_files = 0;

	/* The series' own troubles. */
	s->volume_state = AXIS_VOLUME_OFFLINE;
	assert_derived(e, s, AXIS_TV_ATTENTION, "offline");
	s->volume_state = AXIS_VOLUME_ONLINE;
	s->profile_usable = FALSE;
	assert_derived(e, s, AXIS_TV_ATTENTION, "cannot be used");
	s->profile_usable = TRUE;
	s->root_id = 0;
	assert_derived(e, s, AXIS_TV_ATTENTION, "destination library folder was removed");
	s->root_id = 3;
	/* ...which do not concern an episode not aired yet. */
	g_free(e->air_date);
	e->air_date = g_strdup("2026-11-20");
	s->volume_state = AXIS_VOLUME_OFFLINE;
	assert_derived(e, s, AXIS_TV_NOT_AIRED, NULL);
	s->volume_state = AXIS_VOLUME_ONLINE;

	/* Gone from the metadata: kept, not monitored, said so. */
	e->gone = TRUE;
	assert_derived(e, s, AXIS_TV_UNMONITORED, "no longer in TMDb's episode list");
	axis_tv_episode_free(e);
}

/* ── Anime: absolute numbers (add-anime-support, A1) ────────────── */

/* Episodes from "S:E:ABSOLUTE" entries, space between; a trailing "x" marks
 * one the numbering no longer lists. */
static GPtrArray *
numbered(const gchar *places)
{
	GPtrArray *episodes = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) each = g_strsplit(places, " ", -1);
	guint i;

	for (i = 0; each[i] != NULL; i++) {
		AxisTvEpisode *e = g_new0(AxisTvEpisode, 1);

		g_assert_cmpint(sscanf(each[i], "%d:%d:%d", &e->season, &e->number, &e->absolute), ==, 3);
		e->id = i + 1;
		e->gone = g_str_has_suffix(each[i], "x");
		g_ptr_array_add(episodes, e);
	}
	return episodes;
}

static gchar *
state_of(
	const gchar         *source,
	const gchar         *places,
	gboolean             tvdb_key,
	AxisTvAbsoluteState  expected
){
	g_autoptr(GPtrArray) episodes = numbered(places);
	AxisTvSeries *s = series_new();
	AxisTvAbsolute absolute;
	gchar *sentence;

	s->episode_source = g_strdup(source);
	axis_tv_absolute_state(s, episodes, &absolute);
	g_assert_cmpint(absolute.state, ==, expected);
	sentence = axis_tv_absolute_sentence(s, &absolute, tvdb_key);
	axis_tv_series_free(s);
	return sentence;
}

/*
 * Spec (tv): "An anime series' absolute numbers are TheTVDB's, shown, and
 * used only when they can be trusted": TMDb's numbering; A number on two
 * episodes; Numbers out of order; Usable.  Each of the four states from an
 * episode list, with the sentence the series' page says.
 */
static void
test_absolute_states(void)
{
	g_autofree gchar *text = NULL;

	/* No source: the series follows TMDb's numbering, whatever its rows
	 * hold; with a key, how to change it; without one, the key first. */
	text = state_of("tmdb", "1:1:1 1:2:2", TRUE, AXIS_TV_ABSOLUTE_NO_SOURCE);
	assert_contains(text, "Absolute numbers come from TheTVDB's aired order, and this series is numbered as TMDb numbers "
		"it, which has none.");
	assert_contains(text, "change its numbering to TheTVDB's aired order");
	g_clear_pointer(&text, g_free);
	text = state_of("tmdb", "1:1:0 1:2:0", FALSE, AXIS_TV_ABSOLUTE_NO_SOURCE);
	assert_contains(text, "Set a TheTVDB key on the Movie and TV details page first");
	g_clear_pointer(&text, g_free);

	/* None given: TheTVDB's aired order, and no episode of seasons 1 and up
	 * has a number.  A special's number counts for nothing. */
	text = state_of("tvdb", "0:1:7 1:1:0 1:2:0", TRUE, AXIS_TV_ABSOLUTE_NONE_GIVEN);
	g_assert_cmpstr(text, ==, "TheTVDB gives no absolute numbers for this series.");
	g_clear_pointer(&text, g_free);

	/* Usable, whole and in order, across two seasons and given in no order;
	 * the special's number (the same as an episode's) and an episode the
	 * numbering no longer lists are never counted. */
	text = state_of("tvdb", "2:1:4 1:1:1 1:3:3 0:1:2 1:2:2 2:2:5 3:9:5x", TRUE, AXIS_TV_ABSOLUTE_USABLE);
	g_assert_cmpstr(text, ==, "TheTVDB gives absolute numbers for 5 of 5 episodes. They equal a plain count of the "
		"episodes in aired order.");
	g_clear_pointer(&text, g_free);

	/* Usable, and not a plain count: TheTVDB counts one more from S02E01 on;
	 * one episode has no number, which is not a fault. */
	text = state_of("tvdb", "1:1:1 1:2:2 2:1:4 2:2:0 2:3:6", TRUE, AXIS_TV_ABSOLUTE_USABLE);
	assert_contains(text, "TheTVDB gives absolute numbers for 4 of 5 episodes.");
	assert_contains(text, "From S02E01 on they differ from a plain count of the episodes in aired order (S02E01 is 4 at "
		"TheTVDB and 3 in the count).");
	g_clear_pointer(&text, g_free);

	/* Not usable: a number on two episodes, both named. */
	text = state_of("tvdb", "1:1:1 1:2:2 3:2:30 3:3:30 3:4:31", TRUE, AXIS_TV_ABSOLUTE_NOT_USABLE);
	assert_contains(text, "the same absolute number (S03E02 and S03E03 are both 30)");
	assert_contains(text, "Axis does not use this series' absolute numbers.");
	g_clear_pointer(&text, g_free);
	/* (Two that are not neighbours.) */
	text = state_of("tvdb", "1:1:5 1:2:6 1:3:7 1:4:5", TRUE, AXIS_TV_ABSOLUTE_NOT_USABLE);
	assert_contains(text, "(S01E01 and S01E04 are both 5)");
	g_clear_pointer(&text, g_free);

	/* Not usable: numbers out of order, the two episodes named. */
	text = state_of("tvdb", "3:1:28 3:2:30 3:3:29 3:4:31", TRUE, AXIS_TV_ABSOLUTE_NOT_USABLE);
	assert_contains(text, "TheTVDB's absolute numbers for this series are not in order (S03E02 is 30 and S03E03 is 29). "
		"Axis does not use them.");
	g_clear_pointer(&text, g_free);

	/* No episodes at all, under TheTVDB's order: none given. */
	text = state_of("tvdb", "0:1:1", TRUE, AXIS_TV_ABSOLUTE_NONE_GIVEN);
}

/*
 * Spec (tv): "An anime series is brought from Sonarr like any other", The
 * numbers differ.  Sonarr's absolute numbers beside TheTVDB's: three counts,
 * the episodes that differ named with both numbers, and the stored form read
 * back as it was written.
 */
static void
test_absolute_compare(void)
{
	AxisTvAbsoluteCompare compare;
	AxisTvAbsoluteCompare read;
	g_autofree gchar *json = NULL;
	g_autofree gchar *sentence = NULL;
	g_autofree gchar *list = NULL;
	guint i;

	axis_tv_absolute_compare_init(&compare);
	sentence = axis_tv_absolute_compare_sentence(&compare);
	assert_contains(sentence, "nothing to compare");
	g_clear_pointer(&sentence, g_free);
	axis_tv_absolute_compare_add(&compare, 1, 1, 1, 1);
	axis_tv_absolute_compare_add(&compare, 1, 2, 2, 2);
	/* Neither side: counted nowhere. */
	axis_tv_absolute_compare_add(&compare, 1, 3, 0, 0);
	sentence = axis_tv_absolute_compare_sentence(&compare);
	g_assert_cmpstr(sentence, ==, "Absolute numbers: Sonarr and TheTVDB agree for 2 of 2 episodes.");
	g_assert_null(axis_tv_absolute_compare_list(&compare));
	g_clear_pointer(&sentence, g_free);

	axis_tv_absolute_compare_add(&compare, 3, 2, 29, 30);
	axis_tv_absolute_compare_add(&compare, 3, 3, 0, 31);
	axis_tv_absolute_compare_add(&compare, 3, 4, 32, 0);
	g_assert_cmpuint(compare.agree, ==, 2);
	g_assert_cmpuint(compare.differ, ==, 1);
	g_assert_cmpuint(compare.one_side, ==, 2);
	sentence = axis_tv_absolute_compare_sentence(&compare);
	g_assert_cmpstr(sentence, ==, "Absolute numbers: Sonarr and TheTVDB differ for 1 of 5 episodes, the first S03E02 "
		"(Sonarr 29, TheTVDB 30), and agree for 2. For 2 only one of the two gives a number. Nothing was migrated "
		"differently because of it: every file is on the episode Sonarr had it on.");
	list = axis_tv_absolute_compare_list(&compare);
	g_assert_cmpstr(list, ==, "S03E02 (Sonarr 29, TheTVDB 30)");
	json = axis_tv_absolute_compare_json(&compare);
	g_assert_cmpstr(json, ==, "{\"agree\":2,\"differ\":1,\"one_side\":2,\"list\":[[3,2,29,30]]}");
	g_assert_true(axis_tv_absolute_compare_parse(json, &read));
	g_assert_cmpuint(read.agree, ==, 2);
	g_assert_cmpuint(read.differ, ==, 1);
	g_assert_cmpuint(read.one_side, ==, 2);
	g_assert_cmpuint(read.differs->len, ==, 1);
	g_assert_cmpint(g_array_index(read.differs, AxisTvAbsoluteDiffer, 0).theirs, ==, 29);
	axis_tv_absolute_compare_clear(&read);
	g_clear_pointer(&list, g_free);

	/* Only so many are kept and listed; all are counted. */
	for (i = 0; i < 60; i++)
		axis_tv_absolute_compare_add(&compare, 4, (gint)i + 1, 100 + (gint)i, 200 + (gint)i);
	g_assert_cmpuint(compare.differ, ==, 61);
	g_assert_cmpuint(compare.differs->len, ==, AXIS_TV_ABSOLUTE_LISTED);
	list = axis_tv_absolute_compare_list(&compare);
	assert_contains(list, "; and 11 more");
	axis_tv_absolute_compare_clear(&compare);

	/* What is not that form is nothing, and leaves an empty comparison. */
	g_assert_false(axis_tv_absolute_compare_parse("[1,2]", &read));
	g_assert_cmpuint(read.agree + read.differ + read.one_side, ==, 0);
	axis_tv_absolute_compare_clear(&read);
	g_assert_false(axis_tv_absolute_compare_parse("{\"agree\":\"many\"}", &read));
	axis_tv_absolute_compare_clear(&read);
	g_assert_false(axis_tv_absolute_compare_parse(NULL, &read));
	axis_tv_absolute_compare_clear(&read);
}

/* Spec (tv): "A series has a kind", Never by itself: what the details say is
 * a hint beside the box and nothing more; this is the whole of the hint. */
static void
test_anime_hint(void)
{
	g_autoptr(GPtrArray) genres = g_ptr_array_new();

	g_assert_false(axis_tv_title_hints_anime(NULL, "ja"));
	g_ptr_array_add(genres, (gpointer)"Drama");
	g_assert_false(axis_tv_title_hints_anime(genres, "ja"));
	g_ptr_array_add(genres, (gpointer)"Animation");
	g_assert_true(axis_tv_title_hints_anime(genres, "ja"));
	g_assert_true(axis_tv_title_hints_anime(genres, "JA"));
	g_assert_false(axis_tv_title_hints_anime(genres, "en"));
	g_assert_false(axis_tv_title_hints_anime(genres, NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/tv/unit/absolute-states", test_absolute_states);
	g_test_add_func("/tv/unit/absolute-compare", test_absolute_compare);
	g_test_add_func("/tv/unit/anime-hint", test_anime_hint);
	g_test_add_func("/tv/unit/names", test_names);
	g_test_add_func("/tv/unit/match", test_match);
	g_test_add_func("/tv/unit/match-partial", test_match_partial);
	g_test_add_func("/tv/unit/match-anime", test_match_anime);
	g_test_add_func("/tv/unit/match-standard-unchanged", test_match_standard_unchanged);
	g_test_add_func("/tv/unit/match-anime-unusable", test_match_anime_unusable);
	g_test_add_func("/tv/unit/match-anime-long", test_match_anime_long);
	g_test_add_func("/tv/unit/anime-claims", test_anime_claims);
	g_test_add_func("/tv/unit/match-anime-part-name", test_match_anime_part_name);
	g_test_add_func("/tv/unit/anime-other-title", test_anime_other_title);
	g_test_add_func("/tv/unit/match-anime-two-readings", test_match_anime_two_readings);
	g_test_add_func("/tv/unit/match-anime-other-name", test_match_anime_other_name);
	g_test_add_func("/tv/unit/match-anime-batch", test_match_anime_batch);
	g_test_add_func("/tv/unit/match-anime-after-number", test_match_anime_after_number);
	g_test_add_func("/tv/unit/anime-search-words", test_anime_search_words);
	g_test_add_func("/tv/unit/aired", test_aired);
	g_test_add_func("/tv/unit/states", test_states);
	return g_test_run();
}
