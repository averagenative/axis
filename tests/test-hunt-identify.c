/*
 * test-hunt-identify.c - the stage 2 bug hunt of identification: the cases
 * that showed its findings, ordinary tests since fix-identify-hunt-findings
 * fixed them (every title here is made up)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each case failed on the code before the fix; the finding it showed is
 * named beside it.  The pure cases decide on evidence built by hand, as the
 * run builds it.  The database cases run identification of a scratch root
 * against a fake provider on loopback (the "fixture" provider) with its own
 * answers below.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "core/axis-data-dir.h"
#include "identify/axis-identify.h"
#include "identify/axis-identify-rules.h"
#include "meta/axis-meta.h"
#include "release/axis-release.h"

/* ── Pure helpers ───────────────────────────────────────────────── */

static AxisIdentifyName *
read_name(const gchar *name)
{
	return axis_identify_name_read(name, 2026);
}

static AxisMetaTitle *
film(
	gint64       id,
	const gchar *title,
	gint         year,
	gint         runtime
){
	AxisMetaTitle *t = axis_meta_title_new(AXIS_META_KIND_FILM);

	t->id = id;
	t->provider = g_strdup("fixture");
	t->provider_id = g_strdup_printf("%" G_GINT64_FORMAT, id);
	t->title = g_strdup(title);
	t->year = year;
	t->runtime = runtime;
	return t;
}

/* A series' full record: seasons 1 to @n with @counts episodes each (NULL
 * @counts: no seasons listed at all). */
static AxisMetaTitle *
series_record(
	gint64       id,
	const gchar *title,
	gint         year,
	gint         runtime,
	const gint  *counts,
	guint        n
){
	AxisMetaTitle *t = axis_meta_title_new(AXIS_META_KIND_SERIES);
	guint i;

	t->id = id;
	t->provider = g_strdup("fixture");
	t->provider_id = g_strdup_printf("%" G_GINT64_FORMAT, id);
	t->title = g_strdup(title);
	t->year = year;
	t->runtime = runtime;
	t->detail = TRUE;
	if (counts == NULL)
		return t;
	t->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	for (i = 0; i < n; i++) {
		AxisMetaSeason *s = axis_meta_season_new((gint)i + 1);

		s->episode_count = counts[i];
		g_ptr_array_add(t->seasons, s);
	}
	return t;
}

static gboolean
has_reason(
	const AxisIdentifyVerdict *v,
	const gchar               *code
){
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		if (g_strcmp0(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, i))->code, code) == 0)
			return TRUE;
	}
	return FALSE;
}

static gchar *
reasons_of(const AxisIdentifyVerdict *v)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisIdentifyReason *r = g_ptr_array_index(v->reasons, i);

		g_string_append_printf(out, "%s[%s] %s", i > 0 ? "; " : "", r->code, r->text);
	}
	return g_string_free(out, FALSE);
}

static void
assert_confidence(
	const AxisIdentifyVerdict *v,
	AxisIdentifyConfidence     expected,
	const gchar               *what
){
	if (v->confidence != expected) {
		g_autofree gchar *why = reasons_of(v);

		g_error("%s: expected %s, got %s (%s)", what, axis_identify_confidence_name(expected),
			axis_identify_confidence_name(v->confidence), why);
	}
}

/* What @files hold, each read in the season folder of its own season. */
static void
shape_of(
	const gchar *const *files,
	AxisIdentifyShape  *shape
){
	guint i;

	axis_identify_shape_init(shape);
	for (i = 0; files[i] != NULL; i++) {
		g_autoptr(AxisIdentifyName) name = read_name(files[i]);
		AxisIdentifyEpisodes episodes;

		axis_identify_episodes_read(name->release, -1, &episodes);
		axis_identify_shape_add(shape, &episodes);
		axis_identify_episodes_clear(&episodes);
	}
	axis_identify_shape_finish(shape);
}

/* ── Films: the year is the search answer's, not the full record's ─ */

/*
 * F2.  The run matched the search's titles and then fetched the full record
 * of the one matched (for its runtime), but the year compared was the
 * search answer's (weigh_names: title->year, not detail->year).  When the
 * two differ, a folder named with the full record's year was held as "year
 * one off".  The year is the full record's now.
 */
static void
test_year_from_search_answer(void)
{
	g_autoptr(AxisMetaTitle) brief = film(7, "Tinder Hollow", 2015, 0);
	g_autoptr(AxisMetaTitle) detail = film(7, "Tinder Hollow", 2014, 98);
	g_autoptr(AxisIdentifyName) folder = read_name("Tinder Hollow (2014)");
	g_autoptr(AxisIdentifyName) file = read_name("Tinder Hollow (2014) Bluray-1080p.mkv");
	g_autoptr(AxisIdentifyVerdict) v = NULL;
	const AxisMetaTitle *found[1];
	AxisIdentifyEvidence e;

	detail->detail = TRUE;
	memset(&e, 0, sizeof(e));
	e.kind = AXIS_META_KIND_FILM;
	e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
	e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
	axis_identify_read(folder, file, &e.reading);
	found[0] = brief;
	e.found = found;
	e.n_found = 1;
	e.detail = detail;
	e.duration_ms = 98 * 60000;
	v = axis_identify_decide(&e);
	/* The full record of the one match says 2014, as the names do. */
	assert_confidence(v, AXIS_IDENTIFY_SURE, "Tinder Hollow (2014), its full record of 2014");
	g_assert_false(has_reason(v, "year_off"));
}

/* ── Series: a dated folder whose files name no series ──────────── */

/*
 * T1.  "Ashgrove Lane (2015)/Season 01/S01E01 - Pilot.mkv": the folder's
 * name gives the title and the year, one series has them, the episode's
 * runtime agrees.  The spec's "Name, year and the episode's runtime" makes
 * that sure; weigh_names() held it as "folder_only", while the same folder
 * without its year was linked as sure by its episodes (weigh_series()
 * accepts the folder's name alone).  Both are sure now.
 */
static void
test_series_dated_folder_bare_files(void)
{
	static const gint counts[] = { 8 };
	static const gchar *const files[] = { "S01E01 - Pilot.mkv", "S01E02 - The Gate.mkv", "S01E03 - Low Tide.mkv",
		"S01E04 - Lanterns.mkv", NULL };
	g_autoptr(AxisMetaTitle) ashgrove = series_record(11, "Ashgrove Lane", 2015, 45, counts, 1);
	g_autoptr(AxisIdentifyName) dated = read_name("Ashgrove Lane (2015)");
	g_autoptr(AxisIdentifyName) undated = read_name("Ashgrove Lane");
	g_auto(AxisIdentifyShape) shape = { 0 };
	AxisIdentifySeriesCandidate candidates[1];
	const AxisMetaTitle *found[1];
	AxisIdentifyName *names[G_N_ELEMENTS(files) - 1];
	AxisIdentifyEvidence e;
	guint i;

	/* No file names a series: the folder is read by itself. */
	for (i = 0; files[i] != NULL; i++)
		names[i] = read_name(files[i]);
	g_assert_cmpint(axis_identify_series_name((const AxisIdentifyName *const *)names, G_N_ELEMENTS(names)), ==, -1);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		axis_identify_name_free(names[i]);
	shape_of(files, &shape);
	found[0] = ashgrove;
	memset(candidates, 0, sizeof(candidates));
	candidates[0].title = ashgrove;
	candidates[0].episode_runtime = 45;

	/* Without the year: sure, by its episodes. */
	memset(&e, 0, sizeof(e));
	e.kind = AXIS_META_KIND_SERIES;
	e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
	e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
	e.shape = &shape;
	e.found = found;
	e.n_found = 1;
	e.series = candidates;
	e.n_series = 1;
	e.detail = ashgrove;
	e.episode_runtime = 45;
	e.duration_ms = 45 * 60000;
	axis_identify_read(undated, NULL, &e.reading);
	g_assert_cmpint(e.reading.kind, ==, AXIS_IDENTIFY_READ_FOLDER);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&e);

		assert_confidence(v, AXIS_IDENTIFY_SURE, "Ashgrove Lane (no year), four episodes that fit");
	}

	/* With the year: the title, the year and the episode's runtime agree. */
	e.series = NULL;
	e.n_series = 0;
	axis_identify_read(dated, NULL, &e.reading);
	g_assert_cmpint(e.reading.kind, ==, AXIS_IDENTIFY_READ_FOLDER);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&e);

		assert_confidence(v, AXIS_IDENTIFY_SURE, "Ashgrove Lane (2015), the same files");
		g_assert_false(has_reason(v, "folder_only"));
		g_assert_true(has_reason(v, "runtime_ok"));
	}
}

/* ── Series: one of the name cannot be checked ──────────────────── */

/*
 * T3.  Two series have the name; the provider lists the seasons of one and
 * none of the other's (or no episode counts).  compare_series() said the
 * other "cannot be checked" and weigh_series() counted that as "does not
 * fit", so the first was linked as sure although the files may be the
 * other's.  It is held now, the reason naming the one not checked.
 */
static void
test_series_unchecked_candidate(void)
{
	static const gint counts[] = { 6 };
	static const gint unknown[] = { -1, -1 };
	static const gchar *const files[] = { "Brindle Cove - S01E01.mkv", "Brindle Cove - S01E02.mkv",
		"Brindle Cove - S01E03.mkv", "Brindle Cove - S01E04.mkv", NULL };
	g_autoptr(AxisMetaTitle) listed = series_record(21, "Brindle Cove", 2010, 45, counts, 1);
	g_autoptr(AxisMetaTitle) bare = series_record(22, "Brindle Cove", 1998, 45, NULL, 0);
	g_autoptr(AxisMetaTitle) uncounted = series_record(23, "Brindle Cove", 1998, 45, unknown, 2);
	g_autoptr(AxisIdentifyName) folder = read_name("Brindle Cove");
	g_autoptr(AxisIdentifyName) file = read_name(files[0]);
	g_auto(AxisIdentifyShape) shape = { 0 };
	AxisIdentifySeriesCandidate candidates[2];
	const AxisMetaTitle *found[2];
	AxisIdentifyEvidence e;
	guint round;

	shape_of(files, &shape);
	for (round = 0; round < 2; round++) {
		const AxisMetaTitle *other = round == 0 ? bare : uncounted;

		memset(&e, 0, sizeof(e));
		memset(candidates, 0, sizeof(candidates));
		e.kind = AXIS_META_KIND_SERIES;
		e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
		e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
		e.shape = &shape;
		found[0] = listed;
		found[1] = other;
		e.found = found;
		e.n_found = 2;
		candidates[0].title = listed;
		candidates[0].episode_runtime = 45;
		candidates[1].title = other;
		candidates[1].episode_runtime = 45;
		e.series = candidates;
		e.n_series = 2;
		e.duration_ms = 45 * 60000;
		axis_identify_read(folder, file, &e.reading);
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&e);

			/* Neither can be ruled out: held for a person. */
			assert_confidence(v, AXIS_IDENTIFY_LIKELY, round == 0 ? "Brindle Cove, the other lists no seasons" :
				"Brindle Cove, the other gives no episode counts");
			g_assert_true(has_reason(v, "series_unchecked"));
			g_assert_true(has_reason(v, round == 0 ? "no_seasons" : "episodes_unknown"));
			g_assert_cmpuint(v->candidates->len, ==, 2);
			g_assert_true(g_ptr_array_index(v->candidates, 0) == listed);
			g_assert_true(g_ptr_array_index(v->candidates, 1) == other);
		}
	}
	/* Alone, the one that fits is sure. */
	memset(&e, 0, sizeof(e));
	memset(candidates, 0, sizeof(candidates));
	e.kind = AXIS_META_KIND_SERIES;
	e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
	e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
	e.shape = &shape;
	found[0] = listed;
	e.found = found;
	e.n_found = 1;
	candidates[0].title = listed;
	candidates[0].episode_runtime = 45;
	e.series = candidates;
	e.n_series = 1;
	e.duration_ms = 45 * 60000;
	axis_identify_read(folder, file, &e.reading);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&e);

		assert_confidence(v, AXIS_IDENTIFY_SURE, "Brindle Cove, the one series of the name");
	}
}

/* ── Names: letters with strokes ────────────────────────────────── */

/*
 * N1 (item I2).  The comparison key folded accents (NFKD drops the marks:
 * "é" is "e"), but not letters that do not decompose: "ø", "æ", "ł", "đ".  A
 * folder written without them did not read as the provider's title.  They
 * are written as the plain letters they stand for now.
 */
static void
test_key_letters_with_strokes(void)
{
	static const gchar *const pairs[][2] = {
		{ "Élodie's Garden", "Elodies Garden" },
		{ "Mister Pell & the Night", "Mister Pell and the Night" },
		{ "Ashes of Velmor: Part II", "Ashes of Velmor - Part 2" },
		{ NULL, NULL }
	};
	static const gchar *const strokes[][2] = {
		{ "Søren's Winter", "Sorens Winter" },
		{ "Ærø Tide", "Aero Tide" },
		{ "Łąka Nights", "Laka Nights" },
		{ "Đura's Coat", "Duras Coat" },
		{ "Œuvre of Ðel", "Oeuvre of Del" },
		{ "Þórvik Strand", "Thorvik Strand" },
		{ "Straße der Flöße", "Strasse der Flosse" },
		{ "Ħamrun Lights", "Hamrun Lights" },
		{ "Yıldız Pass", "Yildiz Pass" },
		{ "Ŧorn Ridge", "Torn Ridge" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; pairs[i][0] != NULL; i++) {
		g_autofree gchar *a = axis_release_title_key(pairs[i][0]);
		g_autofree gchar *b = axis_release_title_key(pairs[i][1]);

		g_assert_cmpstr(a, ==, b);
	}
	for (i = 0; strokes[i][0] != NULL; i++) {
		g_autofree gchar *a = axis_release_title_key(strokes[i][0]);
		g_autofree gchar *b = axis_release_title_key(strokes[i][1]);
		g_autofree gchar *again = axis_release_title_key(a);

		if (g_strcmp0(a, b) != 0)
			g_error("'%s' and '%s' read as different titles ('%s', '%s')", strokes[i][0], strokes[i][1], a, b);
		/* The key of a key is the key. */
		g_assert_cmpstr(again, ==, a);
	}
	/* And a folder written without them reads as the title. */
	{
		g_autoptr(AxisMetaTitle) soren = film(31, "Søren's Winter", 2012, 101);
		g_autoptr(AxisIdentifyName) name = read_name("Sorens Winter (2012)");

		g_assert_true(axis_identify_title_fits(name, soren));
	}
}

/* ── Hostile names ──────────────────────────────────────────────── */

/* Long, broken, nested and repetitive names: read in bounded time, with
 * nothing that is not valid UTF-8 coming out. */
static void
test_hostile_names(void)
{
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	gint64 began = g_get_monotonic_time();
	guint i;

	g_ptr_array_add(names, g_strnfill(200000, 'a'));
	g_ptr_array_add(names, g_strnfill(5000, '('));
	g_ptr_array_add(names, g_strnfill(5000, '['));
	{
		GString *s = g_string_new(NULL);

		for (i = 0; i < 2000; i++)
			g_string_append(s, "([{");
		g_string_append(s, "Velmor (2011)");
		for (i = 0; i < 2000; i++)
			g_string_append(s, "}])");
		g_ptr_array_add(names, g_string_free(s, FALSE));
	}
	{
		GString *s = g_string_new("Velmor ");

		for (i = 0; i < 3000; i++)
			g_string_append(s, "S01E01-E99 1x01 2011.03.04 ");
		g_string_append(s, ".mkv");
		g_ptr_array_add(names, g_string_free(s, FALSE));
	}
	{
		GString *s = g_string_new(NULL);

		for (i = 0; i < 20000; i++)
			g_string_append(s, "1080.");
		g_ptr_array_add(names, g_string_free(s, FALSE));
	}
	g_ptr_array_add(names, g_strdup("Velmor \xff\xfe\xc3 (2011) \xe2\x82.mkv"));
	g_ptr_array_add(names, g_strdup("\xc0\xaf\xc0\xaf (1999)"));
	g_ptr_array_add(names, g_strdup("Velmor S01E01-E9999 S00E00 E0000 2011-02-30 0x00.mkv"));
	g_ptr_array_add(names, g_strdup("Velmor {tmdb-1}{tmdb-2}{imdb-tt0000001}[tmdbid-3](2011)[[[]]]{{{}}}.mkv"));
	for (i = 0; i < names->len; i++) {
		g_autoptr(AxisIdentifyName) name = read_name(g_ptr_array_index(names, i));
		AxisIdentifyEpisodes episodes;

		if (name->key != NULL)
			g_assert_true(g_utf8_validate(name->key, -1, NULL));
		if (name->release->title != NULL)
			g_assert_true(g_utf8_validate(name->release->title, -1, NULL));
		axis_identify_episodes_read(name->release, -1, &episodes);
		g_assert_cmpuint(episodes.n, <=, AXIS_IDENTIFY_FILE_EPISODES);
		axis_identify_episodes_clear(&episodes);
	}
	/* Seconds, not minutes, even under the sanitizers. */
	g_assert_cmpint(g_get_monotonic_time() - began, <, 60 * G_USEC_PER_SEC);
}

/* ── The database cases: a fake provider of made-up titles ──────── */

typedef struct
{
	const gchar *id;
	const gchar *title;
	gint         year;
	gint         runtime;
	const gchar *other;	/* (nullable) another name */
	gint         seasons[2];	/* a series': episodes in seasons 1 and 2 */
	gint         search_year;	/* the year its search answer gives, as a
					 * search with a region gave the region's date
					 * (0: its own year) */
} Made;

static const Made made_films[] = {
	/* Known as "Silver Orchard" too: TMDb's search finds it by that name. */
	{ "1001", "Quiet Orchard", 2011, 104, "Silver Orchard", { 0, 0 }, 0 },
	{ "1002", "Tinder Hollow", 2014, 98, NULL, { 0, 0 }, 0 },
	/* Namesakes: one a year off, whose runtime clearly does not fit a file
	 * of 1002; one many years off, whose full record always fails. */
	{ "1003", "Tinder Hollow", 2015, 140, NULL, { 0, 0 }, 0 },
	{ "1004", "Tinder Hollow", 1990, 98, NULL, { 0, 0 }, 0 },
	/* fix-identify-regional-years: first released abroad in 1988, its
	 * search answer the region's 2005. */
	{ "1005", "Paper Lantern Sky", 1988, 86, NULL, { 0, 0 }, 2005 },
	/* First shown in 2020 (a festival), the provider's year 2022. */
	{ "1006", "Copper Wharf", 2022, 101, NULL, { 0, 0 }, 0 },
	/* First released in 1989, its search answer 2019; its full record fails
	 * while fail_far is set. */
	{ "1007", "Velvet Gorge", 1989, 92, NULL, { 0, 0 }, 2019 },
	/* Seven of one name, a decade apart. */
	{ "1010", "Ember Quay", 1950, 90, NULL, { 0, 0 }, 0 },
	{ "1011", "Ember Quay", 1960, 90, NULL, { 0, 0 }, 0 },
	{ "1012", "Ember Quay", 1970, 90, NULL, { 0, 0 }, 0 },
	{ "1013", "Ember Quay", 1980, 90, NULL, { 0, 0 }, 0 },
	{ "1014", "Ember Quay", 1990, 90, NULL, { 0, 0 }, 0 },
	{ "1015", "Ember Quay", 2000, 90, NULL, { 0, 0 }, 0 },
	{ "1016", "Ember Quay", 2010, 90, NULL, { 0, 0 }, 0 },
	{ NULL, NULL, 0, 0, NULL, { 0, 0 }, 0 }
};

/* While set, the full record of film 1003 answers 500. */
static gint fail_namesake;

/* While set, the full record of film 1007 answers 500. */
static gint fail_far;

static const Made made_series[] = {
	{ "2001", "Pellmoor Reach", 2019, 45, NULL, { 8, 0 }, 0 },
	/* Another series, known as "Pellmoor Reach" in another country. */
	{ "2002", "Grey Tide", 2005, 45, "Pellmoor Reach", { 10, 10 }, 0 },
	{ NULL, NULL, 0, 0, NULL, { 0, 0 }, 0 }
};

/* While set, the full record of film 1002 answers 500. */
static gint fail_detail;

static gboolean
made_fits(
	const Made  *m,
	const gchar *want
){
	g_autofree gchar *title = g_utf8_strdown(m->title, -1);
	g_autofree gchar *other = m->other != NULL ? g_utf8_strdown(m->other, -1) : NULL;

	return want[0] != '\0' && (strstr(title, want) != NULL || (other != NULL && strstr(other, want) != NULL));
}

static void
append_made(
	GString     *json,
	const Made  *m,
	gboolean     series,
	gboolean     full
){
	guint i;

	g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"%s\",\"title\":\"%s\",\"year\":%d", m->id,
		series ? "series" : "film", m->title, !full && m->search_year > 0 ? m->search_year : m->year);
	if (!full && !series && m->search_year > 0)
		g_string_append_printf(json, ",\"release_date\":\"%d-03-04\"", m->search_year);
	if (full) {
		g_string_append_printf(json, ",\"runtime\":%d,\"status\":\"ended\",\"overview\":\"Made up.\"", m->runtime);
		if (m->other != NULL)
			g_string_append_printf(json, ",\"alt_titles\":[{\"title\":\"%s\"}]", m->other);
		if (series) {
			g_string_append_printf(json, ",\"first_air_date\":\"%d-01-05\",\"seasons\":[", m->year);
			for (i = 0; i < G_N_ELEMENTS(m->seasons) && m->seasons[i] > 0; i++)
				g_string_append_printf(json, "%s{\"number\":%u,\"name\":\"Season %u\",\"episodes\":%d}",
					i > 0 ? "," : "", i + 1, i + 1, m->seasons[i]);
			g_string_append_c(json, ']');
		} else {
			g_string_append_printf(json, ",\"release_date\":\"%d-06-01\"", m->year);
		}
	}
	g_string_append_c(json, '}');
}

static void
answer_json(
	AxisTestPlexAnswer *answer,
	GString            *json
){
	answer->status = 200;
	answer->content_type = g_strdup("application/json");
	answer->body = g_bytes_new(json->str, json->len);
}

static void
made_provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	g_autoptr(GString) json = g_string_new(NULL);
	gboolean series = g_str_has_prefix(request->path, "/search/series") ||
		g_str_has_prefix(request->path, "/title/series/") || g_str_has_prefix(request->path, "/find/series");
	const Made *table = series ? made_series : made_films;
	guint i;

	if (g_str_has_prefix(request->path, "/search/")) {
		const gchar *q = query != NULL ? g_hash_table_lookup(query, "q") : NULL;
		g_autofree gchar *want = q != NULL ? g_utf8_strdown(q, -1) : g_strdup("");
		guint n = 0;

		g_string_append(json, "{\"results\":[");
		for (i = 0; table[i].id != NULL; i++) {
			if (!made_fits(&table[i], want))
				continue;
			if (n++ > 0)
				g_string_append_c(json, ',');
			append_made(json, &table[i], series, FALSE);
		}
		g_string_append(json, "]}");
		answer_json(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/find/")) {
		g_string_append(json, "{\"results\":[]}");
		answer_json(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/title/")) {
		const gchar *rest = request->path + strlen(series ? "/title/series/" : "/title/film/");
		const gchar *slash = strchr(rest, '/');
		g_autofree gchar *id = slash != NULL ? g_strndup(rest, (gsize)(slash - rest)) : g_strdup(rest);
		guint64 number = 0;
		guint e;

		for (i = 0; table[i].id != NULL && strcmp(table[i].id, id) != 0; i++)
			;
		if (table[i].id == NULL) {
			answer->status = 404;
			return;
		}
		if (slash == NULL) {
			if (!series && strcmp(id, "1002") == 0 && g_atomic_int_get(&fail_detail) == 2) {
				answer->status = 429;
				answer->headers = g_new0(gchar *, 3);
				answer->headers[0] = g_strdup("Retry-After");
				answer->headers[1] = g_strdup("120");
				return;
			}
			if (!series && strcmp(id, "1002") == 0 && g_atomic_int_get(&fail_detail)) {
				answer->status = 500;
				return;
			}
			if (!series && ((strcmp(id, "1003") == 0 && g_atomic_int_get(&fail_namesake)) || strcmp(id, "1004") == 0 ||
			    (strcmp(id, "1007") == 0 && g_atomic_int_get(&fail_far)))) {
				answer->status = 500;
				return;
			}
			append_made(json, &table[i], series, TRUE);
			answer_json(answer, json);
			return;
		}
		if (!series || !g_str_has_prefix(slash, "/season/") ||
		    !g_ascii_string_to_unsigned(slash + strlen("/season/"), 10, 1, 2, &number, NULL) ||
		    table[i].seasons[number - 1] <= 0) {
			answer->status = 404;
			return;
		}
		g_string_append_printf(json, "{\"number\":%u,\"name\":\"Season %u\",\"episodes\":[", (guint)number,
			(guint)number);
		for (e = 1; e <= (guint)table[i].seasons[number - 1]; e++)
			g_string_append_printf(json, "%s{\"number\":%u,\"id\":\"%s%02u%03u\",\"name\":\"Part %u\","
				"\"air_date\":\"%d-02-%02u\",\"runtime\":%d}", e > 1 ? "," : "", e, table[i].id, (guint)number, e, e,
				table[i].year, MIN(e, 28u), table[i].runtime);
		g_string_append(json, "]}");
		answer_json(answer, json);
		return;
	}
	answer->status = 404;
}

typedef struct
{
	AxisTestLibrary     *t;
	gchar               *disk;
	gchar               *films_dir;
	gchar               *series_dir;
	gint64               volume_id;
	gint64               films;
	gint64               series;
	AxisDataDir         *data;
	AxisSettings        *settings;
	AxisTestMetaFixture *fa;
	AxisMeta            *meta;
	AxisIdentify        *identify;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIdentifyTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	g_atomic_int_set(&fail_detail, 0);
	g_atomic_int_set(&fail_namesake, 0);
	g_atomic_int_set(&fail_far, 0);
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->films = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->series = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	fx->films_dir = g_build_filename(fx->disk, "films", NULL);
	fx->series_dir = g_build_filename(fx->disk, "series", NULL);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, made_provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IDENTIFY_MODULE_ID, axis_identify_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
	g_assert_no_error(error);

	fx->identify = axis_identify_new(fx->t->pool, fx->settings, fx->t->storage, fx->meta);
	memset(&tuning, 0, sizeof(tuning));
	tuning.wait_ms = 6000;
	tuning.budget_ms = 120000;
	tuning.latest_year = 2026;
	axis_identify_set_tuning(fx->identify, &tuning);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_identify_unref(fx->identify);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_meta_unref(fx->meta);
	axis_test_meta_fixture_free(fx->fa);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->films_dir);
	g_free(fx->series_dir);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* A file of @size bytes at @dir/@rel. */
static void
put(
	const gchar *dir,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(dir, rel, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autofree gchar *content = g_malloc(MAX(size, 1));
	gsize i;

	g_assert_cmpint(g_mkdir_with_parents(parent, 0755), ==, 0);
	for (i = 0; i < size; i++)
		content[i] = (gchar)('a' + (i * 7 + size) % 26);
	g_assert_true(g_file_set_contents(path, content, (gssize)size, NULL));
}

/* @rels under @dir written, the root scanned, each probed to run @minutes. */
static void
make_tree(
	Fixture            *fx,
	const gchar        *dir,
	gint64              root,
	const gchar *const *rels,
	gint                minutes
){
	g_autoptr(AxisScanResult) result = NULL;
	guint i;

	for (i = 0; rels[i] != NULL; i++)
		put(dir, rels[i], 1000 + i * 10);
	result = axis_test_library_scan_expect(fx->t, root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	g_assert_nonnull(result);
	for (i = 0; rels[i] != NULL; i++)
		axis_test_probe_video(fx->t, root, rels[i], "matroska", "h264", "1080p", "sdr", "0.1",
			(gint64)minutes * 60000, "aac", FALSE);
}

static AxisIdentifyReport
run(
	Fixture *fx,
	gint64   root
){
	AxisIdentifyReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_identify_run_root(fx->identify, root, NULL, &report, &error));
	g_assert_no_error(error);
	return report;
}

/* The unit at @rel of @root: "state confidence title". */
static gchar *
unit_of(
	Fixture     *fx,
	gint64       root,
	const gchar *rel
){
	return axis_test_library_q(fx->t, "SELECT i.state || ' ' || coalesce(i.confidence, '-') || ' ' ||"
		" coalesce(t.title, '-') FROM library_identity i LEFT JOIN meta_title t ON t.id = i.title_id"
		" WHERE i.root_id = %" G_GINT64_FORMAT " AND i.rel_path = '%s'", root, rel);
}

static gchar *
reasons_at(
	Fixture     *fx,
	gint64       root,
	const gchar *rel
){
	return axis_test_library_q(fx->t, "SELECT reasons::text FROM library_identity WHERE root_id = %" G_GINT64_FORMAT
		" AND rel_path = '%s'", root, rel);
}

static void
assert_unit(
	Fixture     *fx,
	gint64       root,
	const gchar *rel,
	const gchar *expected
){
	g_autofree gchar *got = unit_of(fx, root, rel);

	if (g_strcmp0(got, expected) != 0) {
		g_autofree gchar *reasons = reasons_at(fx, root, rel);

		g_error("%s: expected '%s', got '%s' (%s)", rel, expected, got != NULL ? got : "no unit",
			reasons != NULL ? reasons : "");
	}
}

/*
 * F3 (item G).  A film folder named by one of the film's other names.  The
 * provider's search finds the film by that name, but its search answer
 * carries no other names, and for a film the run fetched a full record only
 * of a title the search answer already matched: nothing fitted, the folder
 * was "unknown".  The first answers are read in full for their other names
 * now, when none has the name as its title.
 */
static void
test_film_other_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Silver Orchard (2011)/Silver Orchard (2011) Bluray-1080p.mkv", NULL };

	make_tree(fx, fx->films_dir, fx->films, rels, 104);
	(void)run(fx, fx->films);
	/* The full record says "Silver Orchard" is its name too, of 2011, 104
	 * minutes: the films' rules make that sure. */
	assert_unit(fx, fx->films, "Silver Orchard (2011)", "linked sure Quiet Orchard");
}

/*
 * F1 (item C).  The full record of the one match is not answered (the
 * provider fails for that request alone).  The run did not stop as it does
 * when a search is not answered: it decided on the search answer, which has
 * no runtime, stored "likely: the provider gives no runtime", and recorded
 * the folder's fingerprint, so the folder was never looked at again once the
 * provider answered.  Now nothing final is stored: the folder waits, saying
 * the provider did not answer, and is looked at again once its wait is over
 * and the provider answers.
 */
static void
test_film_detail_not_answered(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Tinder Hollow (2014)/Tinder Hollow (2014) Bluray-1080p.mkv", NULL };
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;
	g_autofree gchar *reasons = NULL;
	g_autofree gchar *wait = NULL;
	guint queued = 0;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	make_tree(fx, fx->films_dir, fx->films, rels, 98);
	g_atomic_int_set(&fail_detail, 1);
	report = run(fx, fx->films);
	g_assert_false(report.provider_down);
	g_assert_cmpuint(report.examined, ==, 1);
	g_assert_cmpuint(report.waiting, ==, 1);
	g_assert_cmpuint(report.review, ==, 0);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "waiting unknown -");
	reasons = reasons_at(fx, fx->films, "Tinder Hollow (2014)");
	g_assert_nonnull(strstr(reasons, "provider_waiting"));
	g_assert_nonnull(strstr(reasons, "did not answer"));
	/* Fifteen minutes first; nothing to look at before. */
	wait = axis_test_library_q(fx->t, "SELECT tries || ' ' || (retry_at > now() + interval '14 minutes'"
		" AND retry_at < now() + interval '16 minutes') FROM library_identity WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(wait, ==, "1 true");
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 0);

	/* "Look again now" ends the wait; still failing, it waits again, twice
	 * as long.  Two failures in a row put the provider in its back-off: the
	 * run stops after this folder, and the root waits (review item 3c). */
	g_assert_true(axis_identify_request(fx->identify, fx->films, NULL, &error));
	g_assert_no_error(error);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	g_assert_true(report.provider_down);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "waiting unknown -");
	g_free(wait);
	wait = axis_test_library_q(fx->t, "SELECT tries || ' ' || (retry_at > now() + interval '29 minutes'"
		" AND retry_at < now() + interval '31 minutes') FROM library_identity WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(wait, ==, "2 true");
	g_free(wait);
	wait = axis_test_library_q(fx->t, "SELECT retry_after > now() FROM library_identity_root WHERE root_id = %"
		G_GINT64_FORMAT, fx->films);
	g_assert_cmpstr(wait, ==, "t");

	/* The provider answers again; once the waits are over, the scheduler
	 * queues the root and the run links it. */
	g_atomic_int_set(&fail_detail, 0);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET retry_at = now() - interval '1 second'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET retry_after = now() - interval '1 second'");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	/* (Nothing else of the root could have changed since its last run.) */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET run_started_at = now() + interval '1 minute'");
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, >=, 1);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	g_free(wait);
	wait = axis_test_library_q(fx->t, "SELECT tries || ' ' || (retry_at IS NULL) FROM library_identity"
		" WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(wait, ==, "0 true");
}

/*
 * Item C and review item 3c, a provider that tells Axis to wait (429): the
 * folder that met it waits, and the run stops after it (the root waits),
 * rather than make every folder after it wait on its own; the folders after
 * it are looked at once the root's wait is over.
 */
static void
test_film_detail_told_to_wait(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = {
		"Tinder Hollow (2014)/Tinder Hollow (2014) Bluray-1080p.mkv",
		"Quiet Orchard (2011)/Quiet Orchard (2011) Bluray-1080p.mkv",
		NULL
	};
	AxisIdentifyReport report;
	g_autofree gchar *unit = NULL;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	make_tree(fx, fx->films_dir, fx->films, rels, 98);
	/* Tinder Hollow is looked at first (the newest first, then by path:
	 * both are as new; "Q" before "T" by path, so make Tinder newer). */
	axis_test_db_exec(fx->t->db, "UPDATE library_file SET created_at = now() + interval '1 hour'"
		" WHERE rel_path LIKE 'Tinder Hollow%'");
	g_atomic_int_set(&fail_detail, 2);
	report = run(fx, fx->films);
	g_assert_true(report.provider_down);
	g_assert_cmpuint(report.examined, ==, 1);
	g_assert_cmpuint(report.pending, ==, 1);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "waiting unknown -");
	unit = unit_of(fx, fx->films, "Quiet Orchard (2011)");
	g_assert_null(unit);
	g_free(unit);
	unit = axis_test_library_q(fx->t, "SELECT retry_after > now() FROM library_identity_root WHERE root_id = %"
		G_GINT64_FORMAT, fx->films);
	g_assert_cmpstr(unit, ==, "t");
}

/*
 * T2 (item D).  A series folder with no year; the provider has a series with
 * that title and another known by that name in another country.  The spec
 * counts "the series with that title, original title or other name"; the
 * run (series_candidates) read other names only when no series had the name
 * as its title, so the second was never compared and the first was linked
 * as sure, although the episodes on disk fit both.  Both are compared now.
 */
static void
test_series_other_name_not_compared(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = {
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E01.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E02.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E03.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E04.mkv",
		NULL
	};

	make_tree(fx, fx->series_dir, fx->series, rels, 45);
	(void)run(fx, fx->series);
	/* Both fit four episodes of a first season of 45 minutes: held. */
	{
		g_autofree gchar *got = unit_of(fx, fx->series, "Pellmoor Reach");
		g_autofree gchar *reasons = reasons_at(fx, fx->series, "Pellmoor Reach");

		if (got == NULL || !g_str_has_prefix(got, "review likely"))
			g_error("Pellmoor Reach: expected 'review likely', got '%s' (%s)", got != NULL ? got : "no unit",
				reasons != NULL ? reasons : "");
		g_assert_nonnull(strstr(reasons, "series_ambiguous"));
	}
}

/*
 * Item J.  Units decided by older rules than the build's are looked at
 * again once, without a person asking: one held for review, and a series'
 * folder linked by itself by its episodes (rule 5, whose comparison of the
 * series of a name was wrong before).  A person's decision is not.
 */
static void
test_older_rules_looked_at_again(Fixture *fx, gconstpointer data)
{
	static const gchar *const films[] = {
		"Tinder Hollow (2014)/Tinder Hollow (2014) Bluray-1080p.mkv",
		"Quiet Orchard (2011)/Quiet Orchard (2011) Bluray-1080p.mkv",
		NULL
	};
	static const gchar *const series[] = {
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E01.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E02.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E03.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E04.mkv",
		NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *orchard = NULL;
	AxisIdentifyReport report;
	guint queued = 0;

	make_tree(fx, fx->films_dir, fx->films, films, 98);
	make_tree(fx, fx->series_dir, fx->series, series, 45);
	(void)run(fx, fx->films);
	(void)run(fx, fx->series);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	/* Quiet Orchard runs 104 minutes: 98 is within the tolerance. */
	assert_unit(fx, fx->films, "Quiet Orchard (2011)", "linked sure Quiet Orchard");
	assert_unit(fx, fx->series, "Pellmoor Reach", "review likely -");

	/* As version 1 of the rules left them: Tinder Hollow held for review,
	 * Pellmoor Reach linked by itself to the series of its title, Quiet
	 * Orchard held and undone by a person. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'review', confidence = 'likely', title_id = NULL,"
		" linked_how = NULL, reasons = '[{\"c\": \"year_off\", \"t\": \"the year is one off\"}]', rules = 1"
		" WHERE rel_path = 'Tinder Hollow (2014)'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'linked', confidence = 'sure',"
		" title_id = (SELECT id FROM meta_title WHERE title = 'Pellmoor Reach'), linked_how = 'auto',"
		" reasons = '[{\"c\": \"series_fits\", \"t\": \"the one series of that name\"}]', rules = 1"
		" WHERE rel_path = 'Pellmoor Reach'");
	orchard = axis_test_library_q(fx->t, "SELECT id FROM library_identity WHERE rel_path = 'Quiet Orchard (2011)'");
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(orchard, NULL, 10), AXIS_IDENTIFY_UNDO, 0, "alice",
		&error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET rules = 1 WHERE rel_path = 'Quiet Orchard (2011)'");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET run_started_at = now() + interval '1 minute'");

	/* Nothing changed on disk; the scheduler queues both roots. */
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 2);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	/* The person's undoing stands. */
	g_free(orchard);
	orchard = axis_test_library_q(fx->t, "SELECT state || ' ' || held || ' ' || rules FROM library_identity"
		" WHERE rel_path = 'Quiet Orchard (2011)'");
	g_assert_cmpstr(orchard, ==, "review true 1");
	/* The series' folder that both series of its name fit is no longer
	 * sure: its link stands (only a person changes a link), and it is
	 * listed for a person, with both as candidates (review item 4). */
	report = run(fx, fx->series);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->series, "Pellmoor Reach", "linked likely Pellmoor Reach");
	g_free(orchard);
	orchard = axis_test_library_q(fx->t, "SELECT linked_how || ' ' || rules || ' ' ||"
		" (doubt LIKE 'It was linked by itself under earlier rules; looked at again, it is now uncertain (%%') || ' ' ||"
		" jsonb_array_length(candidates) || ' ' ||"
		" (reasons::text LIKE '%%series_ambiguous%%') FROM library_identity WHERE rel_path = 'Pellmoor Reach'");
	g_assert_cmpstr(orchard, ==, "auto " G_STRINGIFY(AXIS_IDENTIFY_RULES_VERSION) " true 2 true");

	/* Once: nothing more to look at, nothing queued. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET run_started_at = now() + interval '1 minute'");
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 0);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 0);
	report = run(fx, fx->series);
	g_assert_cmpuint(report.examined, ==, 0);
}

/*
 * Item G: a film folder named by another name that the first page's
 * answers do not hold: the second page is asked for only when the first
 * was full (20 answers) and none had the name.
 */
static gint filler_page;

static void
filler_provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	const gchar *page = query != NULL ? g_hash_table_lookup(query, "page") : NULL;
	g_autoptr(GString) json = g_string_new(NULL);
	guint i;

	if (g_str_has_prefix(request->path, "/search/film")) {
		guint n = g_atomic_int_get(&filler_page);

		g_string_append(json, "{\"results\":[");
		if (page == NULL) {
			for (i = 0; i < n; i++)
				g_string_append_printf(json, "%s{\"id\":\"%u\",\"kind\":\"film\",\"title\":\"Wren Filler %u\","
					"\"year\":%u}", i > 0 ? "," : "", 3000 + i, i, 1990 + i);
		} else {
			g_string_append(json, "{\"id\":\"1001\",\"kind\":\"film\",\"title\":\"Quiet Orchard\",\"year\":2011}");
		}
		g_string_append(json, "]}");
		answer_json(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/title/film/3")) {
		const gchar *id = request->path + strlen("/title/film/");

		g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"Wren Filler %s\",\"year\":1990,"
			"\"runtime\":90,\"release_date\":\"1990-01-01\"}", id, id);
		answer_json(answer, json);
		return;
	}
	made_provider(request, answer, user_data);
}

static void
test_film_second_page(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Silver Orchard (2011)/Silver Orchard (2011) Bluray-1080p.mkv", NULL };

	axis_test_plex_set_handler(fx->fa->api, filler_provider, NULL);
	/* A full first page without the name: the second is asked for. */
	g_atomic_int_set(&filler_page, AXIS_IDENTIFY_SEARCH_PAGE);
	make_tree(fx, fx->films_dir, fx->films, rels, 104);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Silver Orchard (2011)", "linked sure Quiet Orchard");
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 2);
	/* Of the first page's answers, only those within reach of 2011 by their
	 * year (2008 and 2009) were read in full for their other names: the rest
	 * could change nothing (review item 3b).  Every answer within reach was
	 * read, so the match by another name can be sure (review item 2). */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/3018"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/3019"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/3000"), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/3017"), ==, 0);
}

/*
 * Review item 2: a match found only by another name, among the first
 * answers read for their other names while more within reach were not,
 * is at most likely.
 */
static void
crowd_provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	g_autoptr(GString) json = g_string_new(NULL);
	guint i;

	if (g_str_has_prefix(request->path, "/search/film")) {
		g_string_append(json, "{\"results\":[");
		for (i = 0; i < 8; i++)
			g_string_append_printf(json, "%s{\"id\":\"%u\",\"kind\":\"film\",\"title\":\"Crowd %u\",\"year\":2011}",
				i > 0 ? "," : "", 4000 + i, i);
		g_string_append(json, "]}");
		answer_json(answer, json);
		return;
	}
	if (g_str_has_prefix(request->path, "/title/film/4")) {
		const gchar *id = request->path + strlen("/title/film/");

		g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"Crowd %s\",\"year\":2011,"
			"\"runtime\":104,\"release_date\":\"2011-01-01\"%s}", id, id, strcmp(id, "4000") == 0 ?
			",\"alt_titles\":[{\"title\":\"Silver Orchard\"}]" : "");
		answer_json(answer, json);
		return;
	}
	made_provider(request, answer, user_data);
}

static void
test_film_other_name_partial(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Silver Orchard (2011)/Silver Orchard (2011) Bluray-1080p.mkv", NULL };
	g_autofree gchar *reasons = NULL;

	axis_test_plex_set_handler(fx->fa->api, crowd_provider, NULL);
	make_tree(fx, fx->films_dir, fx->films, rels, 104);
	(void)run(fx, fx->films);
	/* "Crowd 0" has the name as another name; five answers of the year
	 * were not read for theirs. */
	assert_unit(fx, fx->films, "Silver Orchard (2011)", "review likely -");
	reasons = reasons_at(fx, fx->films, "Silver Orchard (2011)");
	g_assert_nonnull(strstr(reasons, "other_name_partial"));
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/4003"), ==, 0);
}

/*
 * Review item 3: a provider hiccup never removes a link.  (a) A linked
 * folder looked at again while a full record its decision needs is not
 * answered keeps its link, and waits only to be looked at again; (b) a
 * namesake many years off is not waited for.  It was not read in full
 * either; since fix-identify-regional-years it is (a search answer's year
 * alone rules no title out), once a look, and its record failing changes
 * nothing while another title of the name is within reach.
 */
static void
test_link_kept_while_waiting(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Tinder Hollow (2014)/Tinder Hollow (2014) Bluray-1080p.mkv", NULL };
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisIdentifyReport report;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	make_tree(fx, fx->films_dir, fx->films, rels, 98);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	/* The namesake of 1990 (whose record always fails) was asked for once,
	 * after the two within reach, and not waited for. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/1004"), ==, 1);
	before = reasons_at(fx, fx->films, "Tinder Hollow (2014)");

	/* The namesake a year off is known by its search's record only now, and
	 * its full record fails; the folder is looked at again. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET detail_fetched_at = NULL, runtime_minutes = NULL"
		" WHERE provider_id = '1003'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = repeat('0', 32)");
	g_atomic_int_set(&fail_namesake, 1);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	/* Still linked, as it was; it waits only to be looked at again. */
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	after = reasons_at(fx, fx->films, "Tinder Hollow (2014)");
	g_assert_cmpstr(after, ==, before);
	g_free(after);
	after = axis_test_library_q(fx->t, "SELECT linked_how || ' ' || tries || ' ' || (retry_at > now()) || ' ' ||"
		" (doubt IS NULL) FROM library_identity WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(after, ==, "auto 1 true true");

	/* The provider answers again: looked at once its wait is over, still
	 * the same link, the wait gone. */
	g_atomic_int_set(&fail_namesake, 0);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET retry_at = now() - interval '1 second'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET retry_after = NULL");
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	g_free(after);
	after = axis_test_library_q(fx->t, "SELECT tries || ' ' || (retry_at IS NULL) FROM library_identity"
		" WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(after, ==, "0 true");
	/* Once more by the last look (the one before ended at the record it
	 * waited for). */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/1004"), ==, 2);
}

/*
 * Review item 4, for a film: a folder linked by itself that a later look no
 * longer makes sure keeps its link, listed for a person with what it is
 * now; a person keeps it.
 */
static void
test_film_link_kept_in_doubt(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Quiet Orchard (2011)/Quiet Orchard (2011) Bluray-1080p.mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *row = NULL;
	g_autoptr(GPtrArray) units = NULL;
	guint total = 0;

	make_tree(fx, fx->films_dir, fx->films, rels, 104);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Quiet Orchard (2011)", "linked sure Quiet Orchard");
	/* Probed again, the file now runs an hour: looked at again, the runtime
	 * is far off. */
	axis_test_db_exec(fx->t->db, "UPDATE media_info SET duration_ms = 3600000");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = repeat('0', 32)");
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Quiet Orchard (2011)", "linked likely Quiet Orchard");
	row = axis_test_library_q(fx->t, "SELECT linked_how || ' ' || (reasons::text LIKE '%%runtime_off%%') || ' ' || doubt"
		" FROM library_identity WHERE rel_path = 'Quiet Orchard (2011)'");
	g_assert_cmpstr(row, ==, "auto true It was linked by itself; looked at again, it is now uncertain (runtime 44 minutes"
		" off: 60 minutes against 104), so the link stands until a person decides");
	/* Listed for a person. */
	g_assert_true(axis_identify_list_units(fx->identify, fx->films, AXIS_IDENTIFY_LIST_DOUBT, 0, 10, TRUE, &units,
		&total, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(total, ==, 1);
	g_assert_nonnull(((AxisIdentifyUnitInfo *)g_ptr_array_index(units, 0))->doubt);
	/* Probed again as it was: sure of the same link again, listed no more. */
	axis_test_db_exec(fx->t->db, "UPDATE media_info SET duration_ms = 6240000");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = repeat('0', 32)");
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Quiet Orchard (2011)", "linked sure Quiet Orchard");
	g_free(row);
	row = axis_test_library_q(fx->t, "SELECT (doubt IS NULL)::text FROM library_identity"
		" WHERE rel_path = 'Quiet Orchard (2011)'");
	g_assert_cmpstr(row, ==, "true");
	/* And off again: uncertain once more. */
	axis_test_db_exec(fx->t->db, "UPDATE media_info SET duration_ms = 3600000");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_scope SET fingerprint = repeat('0', 32)");
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Quiet Orchard (2011)", "linked likely Quiet Orchard");
	/* A person keeps the link. */
	g_assert_true(axis_identify_act(fx->identify, ((AxisIdentifyUnitInfo *)g_ptr_array_index(units, 0))->id,
		AXIS_IDENTIFY_CHOOSE, ((AxisIdentifyUnitInfo *)g_ptr_array_index(units, 0))->title_id, "alice", &error));
	g_assert_no_error(error);
	g_free(row);
	row = axis_test_library_q(fx->t, "SELECT state || ' ' || linked_how || ' ' || held || ' ' || (doubt IS NULL)"
		" FROM library_identity WHERE rel_path = 'Quiet Orchard (2011)'");
	g_assert_cmpstr(row, ==, "linked chosen true true");
}

static void
test_film_page_not_full(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Silver Orchard (2011)/Silver Orchard (2011) Bluray-1080p.mkv", NULL };

	axis_test_plex_set_handler(fx->fa->api, filler_provider, NULL);
	/* A first page that is not full: nothing more is asked for. */
	g_atomic_int_set(&filler_page, AXIS_IDENTIFY_SEARCH_PAGE - 1);
	make_tree(fx, fx->films_dir, fx->films, rels, 104);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Silver Orchard (2011)", "review unknown -");
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);
}

/*
 * Item I1: a folder with more videos than are read says so on its entry and
 * in the log, and its own unit is held.
 */
static void
test_folder_too_many_videos(Fixture *fx, gconstpointer data)
{
	GPtrArray *rels = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *reasons = NULL;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	for (i = 0; i < AXIS_IDENTIFY_MAX_VIDEOS + 2; i++)
		g_ptr_array_add(rels, g_strdup_printf("Tinder Hollow (2014)/Tinder Hollow (2014) part %03u.mkv", i));
	g_ptr_array_add(rels, NULL);
	make_tree(fx, fx->films_dir, fx->films, (const gchar *const *)rels->pdata, 98);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "review likely -");
	reasons = reasons_at(fx, fx->films, "Tinder Hollow (2014)");
	g_assert_nonnull(strstr(reasons, "too_many_videos"));
	g_assert_nonnull(strstr(reasons, "2 videos more"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*holds 66 videos; only the first 64 are identified*"), ==,
		1);
	g_ptr_array_unref(rels);
}

/* ── fix-identify-regional-years ────────────────────────────────── */

/*
 * A.  A film first released abroad, the folder named with its first
 * release's year, its search answer giving the year of its release in the
 * region (as TMDb's did with region=US).  The search answer's year was more
 * than three years off, so the title was neither read in full nor taken:
 * "the provider knows no movie that fits Paper Lantern Sky (1988) (Paper
 * Lantern Sky (2005) has that title, but the names say 1988)".  A title
 * whose own title has the name is read in full whatever its search answer
 * says, and judged by its record.
 */
static void
test_film_first_release_abroad(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Paper Lantern Sky (1988)/Paper Lantern Sky (1988) Bluray-1080p.mkv", NULL };
	g_autofree gchar *year = NULL;

	make_tree(fx, fx->films_dir, fx->films, rels, 86);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Paper Lantern Sky (1988)", "linked sure Paper Lantern Sky");
	year = axis_test_library_q(fx->t, "SELECT year FROM meta_title WHERE provider_id = '1005'");
	g_assert_cmpstr(year, ==, "1988");
}

/*
 * A.  The folder's year is two before the provider's (a festival's, a first
 * showing elsewhere): it was "unknown" (two off was as good as another
 * film); it is likely now, the reason naming both years.
 */
static void
test_film_two_years_off(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Copper Wharf (2020)/Copper Wharf (2020) Bluray-1080p.mkv", NULL };
	g_autofree gchar *got = NULL;
	g_autofree gchar *reasons = NULL;

	make_tree(fx, fx->films_dir, fx->films, rels, 101);
	(void)run(fx, fx->films);
	got = unit_of(fx, fx->films, "Copper Wharf (2020)");
	reasons = reasons_at(fx, fx->films, "Copper Wharf (2020)");
	if (got == NULL || !g_str_has_prefix(got, "review likely"))
		g_error("Copper Wharf (2020): expected 'review likely', got '%s' (%s)", got != NULL ? got : "no unit",
			reasons != NULL ? reasons : "");
	g_assert_nonnull(strstr(reasons, "year_off"));
	g_assert_nonnull(strstr(reasons, "two off: 2020 in the names, 2022 for the movie"));
	g_assert_nonnull(strstr(reasons, "Copper Wharf"));
}

/*
 * A.  The one title of the name, out of reach by its search answer's year,
 * whose full record is not answered: the folder waits for it (it is the
 * only title that could be the one), and is linked once it answers.
 * Before, it was not read and the folder was "unknown" for good.
 */
static void
test_film_far_record_waits(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Velvet Gorge (1989)/Velvet Gorge (1989) Bluray-1080p.mkv", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reasons = NULL;
	AxisIdentifyReport report;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	make_tree(fx, fx->films_dir, fx->films, rels, 92);
	g_atomic_int_set(&fail_far, 1);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.waiting, ==, 1);
	assert_unit(fx, fx->films, "Velvet Gorge (1989)", "waiting unknown -");
	reasons = reasons_at(fx, fx->films, "Velvet Gorge (1989)");
	g_assert_nonnull(strstr(reasons, "provider_waiting"));

	g_atomic_int_set(&fail_far, 0);
	g_assert_true(axis_identify_request(fx->identify, fx->films, NULL, &error));
	g_assert_no_error(error);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->films, "Velvet Gorge (1989)", "linked sure Velvet Gorge");
}

/*
 * A, the bound.  Seven titles of the name, every one far from the folder's
 * year: the five nearest are read in full (2010 back to 1970), the other
 * two are not, and nothing fits.  Before, none was read.
 */
static void
test_film_far_namesakes_bounded(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "Ember Quay (2024)/Ember Quay (2024) Bluray-1080p.mkv", NULL };
	static const gchar *const read[] = { "1016", "1015", "1014", "1013", "1012", NULL };
	static const gchar *const left[] = { "1011", "1010", NULL };
	g_autofree gchar *reasons = NULL;
	guint i;

	make_tree(fx, fx->films_dir, fx->films, rels, 90);
	(void)run(fx, fx->films);
	assert_unit(fx, fx->films, "Ember Quay (2024)", "review unknown -");
	reasons = reasons_at(fx, fx->films, "Ember Quay (2024)");
	g_assert_nonnull(strstr(reasons, "no_match"));
	for (i = 0; read[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/title/film/%s", read[i]);

		g_assert_cmpuint(axis_test_plex_count(fx->fa->api, path), ==, 1);
	}
	for (i = 0; left[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/title/film/%s", left[i]);

		g_assert_cmpuint(axis_test_plex_count(fx->fa->api, path), ==, 0);
	}
}

/*
 * B.  A folder named by a number, and its video named so: "nothing in the
 * names reads as a title" before (a key of three digits or fewer was taken
 * for a generic name).
 */
static void
test_film_number_title(Fixture *fx, gconstpointer data)
{
	static const gchar *const rels[] = { "412 (2011)/412 (2011).mkv", NULL };
	g_autofree gchar *reasons = NULL;

	make_tree(fx, fx->films_dir, fx->films, rels, 98);
	(void)run(fx, fx->films);
	reasons = reasons_at(fx, fx->films, "412 (2011)");
	g_assert_nonnull(reasons);
	g_assert_null(strstr(reasons, "no_title"));
	g_assert_nonnull(strstr(reasons, "no_match"));
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);
}

/*
 * The rules' version: units held for review by version 2 (unknown or
 * likely) are looked at again once; a link is not looked at again for it
 * (a series' folder linked by its episodes was, for version 2), and a
 * person's decision never is.
 */
static void
test_regional_years_looked_at_again(Fixture *fx, gconstpointer data)
{
	static const gchar *const abroad[] = { "Paper Lantern Sky (1988)/Paper Lantern Sky (1988) Bluray-1080p.mkv", NULL };
	static const gchar *const films[] = {
		"Tinder Hollow (2014)/Tinder Hollow (2014) Bluray-1080p.mkv",
		"Copper Wharf (2020)/Copper Wharf (2020) Bluray-1080p.mkv",
		NULL
	};
	static const gchar *const series[] = {
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E01.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E02.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E03.mkv",
		"Pellmoor Reach/Season 01/Pellmoor Reach - S01E04.mkv",
		NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *wharf = NULL;
	g_autofree gchar *got = NULL;
	AxisIdentifyReport report;
	guint queued = 0;

	make_tree(fx, fx->films_dir, fx->films, abroad, 86);
	make_tree(fx, fx->films_dir, fx->films, films, 98);
	make_tree(fx, fx->series_dir, fx->series, series, 45);
	(void)run(fx, fx->films);
	(void)run(fx, fx->series);
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");

	/* As version 2 left them: Paper Lantern Sky unknown, Tinder Hollow linked
	 * by itself, Copper Wharf marked by a person as not a movie, Pellmoor
	 * Reach linked by its episodes. */
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'review', confidence = 'unknown', title_id = NULL,"
		" linked_how = NULL, doubt = NULL, reasons = '[{\"c\": \"no_match\", \"t\": \"the provider knows no movie that"
		" fits\"}]', rules = 2 WHERE rel_path = 'Paper Lantern Sky (1988)'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET rules = 2 WHERE rel_path = 'Tinder Hollow (2014)'");
	wharf = axis_test_library_q(fx->t, "SELECT id FROM library_identity WHERE rel_path = 'Copper Wharf (2020)'");
	g_assert_true(axis_identify_act(fx->identify, g_ascii_strtoll(wharf, NULL, 10), AXIS_IDENTIFY_NOT_FILM, 0, "alice",
		&error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET rules = 2 WHERE rel_path = 'Copper Wharf (2020)'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity SET state = 'linked', confidence = 'sure',"
		" title_id = (SELECT id FROM meta_title WHERE title = 'Pellmoor Reach'), linked_how = 'auto', doubt = NULL,"
		" reasons = '[{\"c\": \"series_fits\", \"t\": \"the one series of that name\"}]', rules = 2"
		" WHERE rel_path = 'Pellmoor Reach'");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'identify.root'");
	axis_test_db_exec(fx->t->db, "UPDATE library_identity_root SET run_started_at = now() + interval '1 minute'");

	/* Nothing changed on disk: only the films' root is queued. */
	g_assert_true(axis_identify_run_schedule(fx->identify, &queued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(queued, ==, 1);
	report = run(fx, fx->films);
	g_assert_cmpuint(report.examined, ==, 1);
	assert_unit(fx, fx->films, "Paper Lantern Sky (1988)", "linked sure Paper Lantern Sky");
	got = axis_test_library_q(fx->t, "SELECT rules FROM library_identity WHERE rel_path = 'Paper Lantern Sky (1988)'");
	g_assert_cmpstr(got, ==, G_STRINGIFY(AXIS_IDENTIFY_RULES_VERSION));
	/* The link and the person's decision stand, untouched. */
	assert_unit(fx, fx->films, "Tinder Hollow (2014)", "linked sure Tinder Hollow");
	g_free(got);
	got = axis_test_library_q(fx->t, "SELECT rules FROM library_identity WHERE rel_path = 'Tinder Hollow (2014)'");
	g_assert_cmpstr(got, ==, "2");
	g_free(got);
	got = axis_test_library_q(fx->t, "SELECT state || ' ' || held || ' ' || rules FROM library_identity"
		" WHERE rel_path = 'Copper Wharf (2020)'");
	g_assert_cmpstr(got, ==, "not_film true 2");
	report = run(fx, fx->series);
	g_assert_cmpuint(report.examined, ==, 0);
	assert_unit(fx, fx->series, "Pellmoor Reach", "linked sure Pellmoor Reach");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/hunt-identify/film-year-from-search-answer", test_year_from_search_answer);
	g_test_add_func("/hunt-identify/series-dated-folder-bare-files", test_series_dated_folder_bare_files);
	g_test_add_func("/hunt-identify/series-unchecked-candidate", test_series_unchecked_candidate);
	g_test_add_func("/hunt-identify/key-letters-with-strokes", test_key_letters_with_strokes);
	g_test_add_func("/hunt-identify/hostile-names", test_hostile_names);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-other-name", Fixture, NULL, test_film_other_name);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-detail-not-answered", Fixture, NULL, test_film_detail_not_answered);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-detail-told-to-wait", Fixture, NULL, test_film_detail_told_to_wait);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/series-other-name-not-compared", Fixture, NULL,
		test_series_other_name_not_compared);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/older-rules-looked-at-again", Fixture, NULL,
		test_older_rules_looked_at_again);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-second-page", Fixture, NULL, test_film_second_page);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-page-not-full", Fixture, NULL, test_film_page_not_full);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/folder-too-many-videos", Fixture, NULL, test_folder_too_many_videos);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-other-name-partial", Fixture, NULL, test_film_other_name_partial);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/link-kept-while-waiting", Fixture, NULL, test_link_kept_while_waiting);
	AXIS_TEST_ADD_DB_CASE("/hunt-identify/film-link-kept-in-doubt", Fixture, NULL, test_film_link_kept_in_doubt);
	AXIS_TEST_ADD_DB_CASE("/regional-years/film-first-release-abroad", Fixture, NULL, test_film_first_release_abroad);
	AXIS_TEST_ADD_DB_CASE("/regional-years/film-two-years-off", Fixture, NULL, test_film_two_years_off);
	AXIS_TEST_ADD_DB_CASE("/regional-years/film-far-record-waits", Fixture, NULL, test_film_far_record_waits);
	AXIS_TEST_ADD_DB_CASE("/regional-years/film-far-namesakes-bounded", Fixture, NULL,
		test_film_far_namesakes_bounded);
	AXIS_TEST_ADD_DB_CASE("/regional-years/film-number-title", Fixture, NULL, test_film_number_title);
	AXIS_TEST_ADD_DB_CASE("/regional-years/looked-at-again", Fixture, NULL, test_regional_years_looked_at_again);
	return g_test_run();
}
