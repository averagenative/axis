/*
 * test-identify-unit.c - identification's pure parts: identifiers in names
 * and .nfo bytes, what names read as, the units of a folder, the decision
 * and its confidence, the runtime tolerance, a found file's quality, and the
 * unknown source on disk in the quality layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-library-identification:
 *   4.1 each kind of evidence gives its confidence; an identifier beats a
 *       name; a disagreeing .nfo goes to review
 *   4.2 title and year with the runtime in tolerance is sure; no year, the
 *       runtime off or two candidates is likely
 *   4.3 several films in a folder are separate units; copies of one film are
 *       one unit
 *   4.5 an unknown source is not upgraded by default
 *   4.7 (half) the .nfo scanner is bounded and hostile bytes are harmless
 */

#include <glib.h>
#include <string.h>

#include "identify/axis-identify-ids.h"
#include "identify/axis-identify-rules.h"
#include "quality/axis-quality.h"

/* ── Identifiers in names ───────────────────────────────────────── */

static void
check_name(
	const gchar *name,
	const gchar *tmdb,
	const gchar *imdb,
	const gchar *rest
){
	AxisIdentifyIds ids;
	g_autofree gchar *left = axis_identify_name_ids(name, -1, &ids);

	g_assert_cmpstr(ids.tmdb, ==, tmdb);
	g_assert_cmpstr(ids.imdb, ==, imdb);
	if (rest != NULL)
		g_assert_cmpstr(left, ==, rest);
}

static void
test_name_ids(void)
{
	AxisIdentifyIds ids;
	g_autofree gchar *conflict = NULL;
	g_autofree gchar *tvdb = NULL;
	g_autofree gchar *nul = NULL;

	check_name("Hearth (1995) {tmdb-6001}", "6001", "", "Hearth (1995)");
	check_name("Hearth (1995) [tmdbid-6001]", "6001", "", "Hearth (1995)");
	check_name("Hearth (1995) [tmdbid=6001]", "6001", "", "Hearth (1995)");
	check_name("Hearth (1995) {TMDB-6001}", "6001", "", "Hearth (1995)");
	check_name("Hearth (1995) {imdb-tt0600001}", "", "tt0600001", "Hearth (1995)");
	check_name("Hearth (1995) [imdbid-TT0600001] 1080p", "", "tt0600001", "Hearth (1995) 1080p");
	check_name("Hearth {tmdb-6001} {imdb-tt0600001}", "6001", "tt0600001", "Hearth");
	/* Not tokens: a wrong closing bracket, a leading zero, no digits, too
	 * many, letters among them, an IMDb id without "tt". */
	check_name("Hearth {tmdb-6001]", "", "", "Hearth {tmdb-6001]");
	check_name("Hearth {tmdb-06001}", "", "", NULL);
	check_name("Hearth {tmdb-}", "", "", NULL);
	check_name("Hearth {tmdb-1234567890123}", "", "", NULL);
	check_name("Hearth {tmdb-94a9}", "", "", NULL);
	check_name("Hearth {imdb-0600001}", "", "", NULL);
	check_name("Hearth {imdb-tt060000}", "", "", NULL);
	check_name("Hearth {imdb-tt0600001123}", "", "", NULL);
	check_name("Hearth [tmdbid 6001]", "", "", NULL);

	/* A series' identifier is read and taken out; a film's evidence does not
	 * count it. */
	tvdb = axis_identify_name_ids("Show {tvdb-12345}", -1, &ids);
	g_assert_cmpstr(ids.tvdb, ==, "12345");
	g_assert_false(ids.tvdb_conflict);
	g_assert_false(axis_identify_ids_any(&ids));
	g_assert_cmpstr(tvdb, ==, "Show");
	g_clear_pointer(&tvdb, g_free);
	tvdb = axis_identify_name_ids("Lantern Row (2015) [tvdbid-80001] {tvdb-80002}", -1, &ids);
	g_assert_cmpstr(ids.tvdb, ==, "");
	g_assert_true(ids.tvdb_conflict);
	g_assert_false(axis_identify_ids_conflict(&ids));
	g_assert_cmpstr(tvdb, ==, "Lantern Row (2015)");
	g_clear_pointer(&tvdb, g_free);
	tvdb = axis_identify_name_ids("Lantern Row {tvdb-080001}", -1, &ids);
	g_assert_cmpstr(ids.tvdb, ==, "");

	/* Two different ones of a scheme: neither is believed. */
	conflict = axis_identify_name_ids("Hearth {tmdb-6001} [tmdbid-6024]", -1, &ids);
	g_assert_true(ids.tmdb_conflict);
	g_assert_cmpstr(ids.tmdb, ==, "");
	g_assert_true(axis_identify_ids_conflict(&ids));
	g_assert_cmpstr(conflict, ==, "Hearth");
	check_name("Hearth {tmdb-6001} [tmdbid-6001]", "6001", "", "Hearth");

	/* A NUL inside, or a name too long, finds nothing. */
	nul = axis_identify_name_ids("Hearth {tmdb-6001}\0{tmdb-1}", 28, &ids);
	g_assert_false(axis_identify_ids_any(&ids));
	{
		g_autofree gchar *longer = g_strnfill(AXIS_IDENTIFY_NAME_MAX + 10, 'a');
		g_autofree gchar *with = g_strconcat(longer, " {tmdb-6001}", NULL);
		g_autofree gchar *left = axis_identify_name_ids(with, -1, &ids);

		g_assert_false(axis_identify_ids_any(&ids));
		g_assert_cmpstr(left, ==, with);
	}
	{
		g_autofree gchar *empty = axis_identify_name_ids(NULL, -1, &ids);

		g_assert_cmpstr(empty, ==, "");
	}
}

/* ── Identifiers in .nfo bytes ──────────────────────────────────── */

static void
scan_text(
	const gchar     *text,
	AxisIdentifyIds *ids
){
	axis_identify_nfo_scan((const guint8 *)text, strlen(text), ids);
}

static void
test_nfo_scan(void)
{
	AxisIdentifyIds ids;
	g_autofree guint8 *big = NULL;
	gsize i;

	scan_text("<movie><title>Hearth</title><id>tt0600001</id></movie>", &ids);
	g_assert_cmpstr(ids.imdb, ==, "tt0600001");
	g_assert_cmpstr(ids.tmdb, ==, "");
	scan_text("https://www.imdb.com/title/tt0600001/", &ids);
	g_assert_cmpstr(ids.imdb, ==, "tt0600001");
	scan_text("https://www.themoviedb.org/movie/6001-hearth", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "6001");
	scan_text("https://www.themoviedb.org/tv/6001-hearth", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "");
	scan_text("<tmdbid> 6001 </tmdbid>", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "6001");
	scan_text("<uniqueid type=\"tmdb\" default=\"true\">6001</uniqueid>", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "6001");
	scan_text("<uniqueid default='false' type='TMDB'>6001</uniqueid>", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "6001");
	scan_text("<uniqueid type=\"imdb\">tt0600001</uniqueid>", &ids);
	g_assert_cmpstr(ids.imdb, ==, "tt0600001");
	g_assert_cmpstr(ids.tmdb, ==, "");
	scan_text("<uniqueid subtype=\"tmdb\">6001</uniqueid>", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "");

	/* Word bounds and digit counts. */
	scan_text("xtt0600001", &ids);
	g_assert_cmpstr(ids.imdb, ==, "");
	scan_text("tt0600001x", &ids);
	g_assert_cmpstr(ids.imdb, ==, "");
	scan_text("tt060000", &ids);
	g_assert_cmpstr(ids.imdb, ==, "");
	scan_text("tt0600001123", &ids);
	g_assert_cmpstr(ids.imdb, ==, "");
	scan_text("tt06000011", &ids);
	g_assert_cmpstr(ids.imdb, ==, "tt06000011");
	scan_text("<tmdbid>06001</tmdbid>", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "");

	/* Two films named: a conflict, neither believed; the same twice is fine. */
	scan_text("tt0600001 and tt0600003", &ids);
	g_assert_true(ids.imdb_conflict);
	g_assert_cmpstr(ids.imdb, ==, "");
	scan_text("tt0600001 tt0600001 themoviedb.org/movie/6001", &ids);
	g_assert_false(axis_identify_ids_conflict(&ids));
	g_assert_cmpstr(ids.imdb, ==, "tt0600001");
	g_assert_cmpstr(ids.tmdb, ==, "6001");

	/* Hostile bytes: NULs, high bytes, markup, a lone "<", ends cut short. */
	{
		static const guint8 hostile[] = {
			0x00, 0xff, '<', 's', 'c', 'r', 'i', 'p', 't', '>', 0x00, 't', 't', '0', '6', '0', '0', '0', '0', '1',
			0xc3, 0x28, '<', 'u', 'n', 'i', 'q', 'u', 'e', 'i', 'd', ' ', 't', 'y', 'p', 'e', '=', '"', 't', 'm'
		};

		axis_identify_nfo_scan(hostile, sizeof(hostile), &ids);
		g_assert_cmpstr(ids.imdb, ==, "tt0600001");
		g_assert_cmpstr(ids.tmdb, ==, "");
	}
	axis_identify_nfo_scan(NULL, 100, &ids);
	g_assert_false(axis_identify_ids_any(&ids));
	scan_text("<uniqueid type=\"tmdb\">", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "");
	scan_text("themoviedb.org/movie/", &ids);
	g_assert_cmpstr(ids.tmdb, ==, "");

	/* Bounded: an identifier past the first 64 KiB is never seen. */
	big = g_malloc(AXIS_IDENTIFY_NFO_MAX + 64);
	for (i = 0; i < AXIS_IDENTIFY_NFO_MAX + 64; i++)
		big[i] = 'x';
	big[AXIS_IDENTIFY_NFO_MAX] = ' ';
	memcpy(big + AXIS_IDENTIFY_NFO_MAX + 1, "tt0600001 ", 10);
	axis_identify_nfo_scan(big, AXIS_IDENTIFY_NFO_MAX + 64, &ids);
	g_assert_false(axis_identify_ids_any(&ids));
	memcpy(big + 100, " tt0600001 ", 11);
	axis_identify_nfo_scan(big, AXIS_IDENTIFY_NFO_MAX + 64, &ids);
	g_assert_cmpstr(ids.imdb, ==, "tt0600001");
}

/* ── Names ──────────────────────────────────────────────────────── */

static AxisIdentifyName *
read_name(const gchar *name)
{
	return axis_identify_name_read(name, 2026);
}

static void
test_names(void)
{
	g_autoptr(AxisIdentifyName) folder = read_name("Hearth (1995) {tmdb-6001}");
	g_autoptr(AxisIdentifyName) file = read_name("Hearth.1995.1080p.BluRay.x264-GRP.mkv");
	g_autoptr(AxisIdentifyName) generic = read_name("movie.mkv");
	g_autoptr(AxisIdentifyName) cd = read_name("CD1.avi");
	g_autoptr(AxisIdentifyName) digits = read_name("01.mkv");
	g_autoptr(AxisIdentifyName) film1917 = read_name("1917.2019.1080p.WEB-DL.mkv");
	g_autoptr(AxisIdentifyName) hashed = read_name("a8f5f167f44f4964e6c998dee827110c.mkv");
	g_autoptr(AxisIdentifyName) markup = read_name("<b>Hearth</b> (1995) & \"more\".mkv");
	g_autofree gchar *words = axis_identify_name_describe(folder);

	g_assert_nonnull(folder->key);
	g_assert_cmpint(folder->year, ==, 1995);
	g_assert_cmpstr(folder->ids.tmdb, ==, "6001");
	g_assert_cmpstr(words, ==, "Hearth (1995)");
	g_assert_cmpstr(file->key, ==, folder->key);
	g_assert_cmpint(file->year, ==, 1995);
	g_assert_null(generic->key);
	g_assert_null(cd->key);
	g_assert_null(digits->key);
	g_assert_nonnull(film1917->key);
	g_assert_cmpint(film1917->year, ==, 2019);
	g_assert_null(hashed->key);
	g_assert_cmpstr(markup->shown, ==, "<b>Hearth</b> (1995) & \"more\".mkv");
}

/* A name that is exactly "<something> (<year>)", a folder's or a video's
 * in it: the something is the title, whatever it is made of
 * (fix-identify-regional-years, B). */
static guint expect_misses;

static void
expect_title(
	const gchar *name,
	const gchar *title,
	gint         year
){
	g_autoptr(AxisIdentifyName) read = read_name(name);

	if (title == NULL ? read->key == NULL : read->key != NULL && g_strcmp0(read->release->title, title) == 0 &&
	    read->year == year)
		return;
	g_printerr("%s: read as \"%s\" (%d), key %s; expected \"%s\" (%d)\n", name,
		read->release->title != NULL ? read->release->title : "(none)", read->year,
		read->key != NULL ? read->key : "(none)", title != NULL ? title : "(none)", year);
	expect_misses++;
}

static void
test_names_any_title(void)
{
	expect_misses = 0;
	/* Numbers, letters that look like a tag, a quality word, one or two
	 * characters. */
	expect_title("412 (2011)", "412", 2011);
	expect_title("412 (2011).mkv", "412", 2011);
	expect_title("7 (2014)", "7", 2014);
	expect_title("7 (2014).mkv", "7", 2014);
	expect_title("88 (2019)", "88", 2019);
	expect_title("zQz (2003)", "zQz", 2003);
	expect_title("XQX (2003).mkv", "XQX", 2003);
	/* Letters that read as a Roman numeral: the key is the number. */
	expect_title("xIv (2003)", "xIv", 2003);
	expect_title("XIV (2003).mkv", "XIV", 2003);
	expect_title("XIV.2003.1080p.BluRay.x264-GRP.mkv", "XIV", 2003);
	expect_title("Q (2016)", "Q", 2016);
	expect_title("Ka (2012)", "Ka", 2012);
	expect_title("HEVC (2010)", "HEVC", 2010);
	expect_title("4K (2015)", "4K", 2015);
	expect_title("4K (2015).mkv", "4K", 2015);
	expect_title("x264 (2015)", "x264", 2015);
	expect_title("1080p (2015)", "1080p", 2015);
	expect_title("CD1 (2007)", "CD1", 2007);
	expect_title("01 (2007)", "01", 2007);
	/* A number followed by words; one that looks like a resolution or an
	 * episode; one that looks like a year (the bracketed year is the year). */
	expect_title("412 Lanterns (2011)", "412 Lanterns", 2011);
	expect_title("4K Ghosts (2015)", "4K Ghosts", 2015);
	expect_title("720 (2015)", "720", 2015);
	expect_title("S01 (2008)", "S01", 2008);
	expect_title("2x3 (2008)", "2x3", 2008);
	expect_title("1983 (1961)", "1983", 1961);
	expect_title("1983 (1961).mkv", "1983", 1961);
	/* Release names keep their reading: the number is the title, the year
	 * the year. */
	expect_title("412.2011.1080p.BluRay.x264-GROUP.mkv", "412", 2011);
	expect_title("7.2014.720p.WEB-DL.DD5.1.H264-GRP", "7", 2014);
	expect_title("1983.1961.1080p.BluRay.x264-GRP", "1983", 1961);
	/* Not that form: generic names stay no title. */
	expect_title("CD1.avi", NULL, 0);
	expect_title("01.mkv", NULL, 0);
	expect_title("412.mkv", NULL, 0);
	expect_title("movie.mkv", NULL, 0);
	expect_title("- (2015)", NULL, 0);
	g_assert_cmpuint(expect_misses, ==, 0);
}

/* ── Units of a folder ──────────────────────────────────────────── */

typedef struct
{
	AxisIdentifyName  *folder;
	AxisIdentifyName **files;
	guint              n;
	GPtrArray         *groups;
} Grouped;

static void
grouped_clear(Grouped *g)
{
	guint i;

	axis_identify_name_free(g->folder);
	for (i = 0; i < g->n; i++)
		axis_identify_name_free(g->files[i]);
	g_free(g->files);
	g_ptr_array_unref(g->groups);
}

static void
group_of(
	Grouped            *g,
	const gchar        *folder,
	const gchar *const *files
){
	guint i;

	memset(g, 0, sizeof(*g));
	g->folder = folder != NULL ? read_name(folder) : NULL;
	g->n = g_strv_length((gchar **)files);
	g->files = g_new0(AxisIdentifyName *, g->n);
	for (i = 0; i < g->n; i++)
		g->files[i] = read_name(files[i]);
	g->groups = axis_identify_group(g->folder, (const AxisIdentifyName *const *)g->files, g->n);
}

static guint
members(
	const Grouped *g,
	guint          group
){
	return ((AxisIdentifyGroup *)g_ptr_array_index(g->groups, group))->members->len;
}

static gboolean
is_folder(
	const Grouped *g,
	guint          group
){
	return ((AxisIdentifyGroup *)g_ptr_array_index(g->groups, group))->folder;
}

static void
test_groups(void)
{
	static const gchar *const one[] = { "Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL };
	static const gchar *const copies[] = { "Hearth.1995.1080p.BluRay.x264-GRP.mkv", "Hearth.1995.2160p.UHD.BluRay.x265-GRP.mkv",
		NULL };
	static const gchar *const two[] = { "Hearth.1995.1080p.BluRay.mkv", "Rokan.1998.1080p.BluRay.mkv", NULL };
	static const gchar *const remakes[] = { "Dunmore.1984.1080p.mkv", "Dunmore.2021.2160p.mkv", NULL };
	static const gchar *const nameless[] = { "Hearth.1995.1080p.mkv", "abcdef0123456789abcdef0123456789.mkv", NULL };
	static const gchar *const no_year[] = { "Hearth.1080p.mkv", "Hearth.1995.2160p.mkv", NULL };
	static const gchar *const mixed[] = { "Hearth.1995.1080p.mkv", "Hearth.1995.720p.mkv", "Rokan.1998.mkv", NULL };
	static const gchar *const loose[] = { "Hearth.1995.mkv", "Rokan.1998.mkv", NULL };
	Grouped g;

	group_of(&g, "Hearth (1995)", one);
	g_assert_cmpuint(g.groups->len, ==, 1);
	g_assert_true(is_folder(&g, 0));
	grouped_clear(&g);

	/* Copies of one film: one unit, the folder's. */
	group_of(&g, "Hearth (1995)", copies);
	g_assert_cmpuint(g.groups->len, ==, 1);
	g_assert_cmpuint(members(&g, 0), ==, 2);
	g_assert_true(is_folder(&g, 0));
	grouped_clear(&g);

	/* Two films: two units, neither the folder's. */
	group_of(&g, "Rowan Marsh", two);
	g_assert_cmpuint(g.groups->len, ==, 2);
	g_assert_false(is_folder(&g, 0));
	g_assert_false(is_folder(&g, 1));
	grouped_clear(&g);

	/* Same title, years far apart: two films. */
	group_of(&g, "Dunmore", remakes);
	g_assert_cmpuint(g.groups->len, ==, 2);
	grouped_clear(&g);

	/* A video reading as nothing stays with the folder's film. */
	group_of(&g, "Hearth (1995)", nameless);
	g_assert_cmpuint(g.groups->len, ==, 1);
	g_assert_cmpuint(members(&g, 0), ==, 2);
	grouped_clear(&g);

	/* No year joins the one group of its title when the folder's name gives
	 * that title and its year (fix-identify-hunt-findings, item I4)... */
	group_of(&g, "Hearth (1995)", no_year);
	g_assert_cmpuint(g.groups->len, ==, 1);
	g_assert_true(is_folder(&g, 0));
	grouped_clear(&g);
	{
		static const gchar *const hollow[] = { "Tinder.Hollow.mkv", "Tinder Hollow (1984).mkv", NULL };

		group_of(&g, "Tinder Hollow (1985)", hollow);
		g_assert_cmpuint(g.groups->len, ==, 1);
		g_assert_cmpuint(members(&g, 0), ==, 2);
		grouped_clear(&g);

		/* ...and stays apart otherwise: in a folder without a year it may be
		 * another film of the name. */
		group_of(&g, "Tinder Hollow", hollow);
		g_assert_cmpuint(g.groups->len, ==, 2);
		g_assert_false(is_folder(&g, 0));
		g_assert_false(is_folder(&g, 1));
		g_assert_cmpuint(members(&g, 0), ==, 1);
		g_assert_cmpuint(members(&g, 1), ==, 1);
		grouped_clear(&g);
		group_of(&g, "Ember Reels", hollow);
		g_assert_cmpuint(g.groups->len, ==, 2);
		grouped_clear(&g);
		/* A folder of another year: the one without a year is the folder's. */
		group_of(&g, "Tinder Hollow (2015)", hollow);
		g_assert_cmpuint(g.groups->len, ==, 2);
		g_assert_true(is_folder(&g, 0));
		g_assert_cmpuint(members(&g, 0), ==, 1);
		grouped_clear(&g);
	}

	/* The folder's own film (two copies) and another. */
	group_of(&g, "Hearth (1995)", mixed);
	g_assert_cmpuint(g.groups->len, ==, 2);
	g_assert_true(is_folder(&g, 0));
	g_assert_cmpuint(members(&g, 0), ==, 2);
	g_assert_false(is_folder(&g, 1));
	g_assert_cmpuint(members(&g, 1), ==, 1);
	grouped_clear(&g);

	/* At the top of a root, each file is its own. */
	group_of(&g, NULL, loose);
	g_assert_cmpuint(g.groups->len, ==, 2);
	grouped_clear(&g);
}

/* ── Readings ───────────────────────────────────────────────────── */

static void
test_readings(void)
{
	g_autoptr(AxisIdentifyName) folder = read_name("Hearth (1995)");
	g_autoptr(AxisIdentifyName) file = read_name("Hearth.1995.1080p.mkv");
	g_autoptr(AxisIdentifyName) no_year = read_name("Hearth.1080p.mkv");
	g_autoptr(AxisIdentifyName) other = read_name("Rokan.1998.mkv");
	g_autoptr(AxisIdentifyName) generic = read_name("movie.mkv");
	g_autoptr(AxisIdentifyName) bare = read_name("Hearth");
	AxisIdentifyReading r;

	axis_identify_read(folder, file, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_AGREE);
	g_assert_cmpint(r.year, ==, 1995);
	axis_identify_read(bare, file, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_AGREE);
	g_assert_true(r.primary == file);
	g_assert_cmpint(r.year, ==, 1995);
	axis_identify_read(folder, no_year, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_AGREE);
	g_assert_true(r.primary == folder);
	axis_identify_read(folder, other, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_DISAGREE);
	axis_identify_read(folder, generic, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_FOLDER);
	axis_identify_read(NULL, file, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_ALONE);
	axis_identify_read(NULL, generic, &r);
	g_assert_cmpint(r.kind, ==, AXIS_IDENTIFY_READ_NONE);
}

/* ── The decision ───────────────────────────────────────────────── */

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

typedef struct
{
	AxisIdentifyName     *folder;
	AxisIdentifyName     *file;
	AxisIdentifyEvidence  e;
} Case;

static void
case_init(
	Case        *c,
	const gchar *folder,
	const gchar *file
){
	memset(c, 0, sizeof(*c));
	c->folder = folder != NULL ? read_name(folder) : NULL;
	c->file = read_name(file);
	axis_identify_read(c->folder, c->file, &c->e.reading);
	c->e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
	c->e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
}

static void
case_clear(Case *c)
{
	axis_identify_name_free(c->folder);
	axis_identify_name_free(c->file);
}

static void
test_decide_identifiers(void)
{
	g_autoptr(AxisMetaTitle) hearth = film(1, "Hearth", 1995, 170);
	g_autoptr(AxisMetaTitle) mazework = film(2, "The Mazework", 1999, 136);
	const AxisMetaTitle *found[1];
	Case c;

	/* An identifier in the folder's name: sure, without looking further. */
	case_init(&c, "Hearth (1995) {tmdb-6001}", "Hearth.1995.1080p.mkv");
	c.e.name_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.name_id.where = "the folder name";
	c.e.name_id.token = "tmdb 6001";
	c.e.name_id.title = hearth;
	g_assert_true(axis_identify_id_decides(&c.e.name_id, &c.e.reading));
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == hearth);
		g_assert_true(has_reason(v, "id_name"));
	}
	case_clear(&c);

	/* An identifier beats a name that gives no year (or reads as nothing). */
	case_init(&c, "Some Old Rip {tmdb-6001}", "movie.mkv");
	c.e.name_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.name_id.title = hearth;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == hearth);
	}
	case_clear(&c);

	/* An identifier the names clearly disagree with (another title, a year
	 * far off): to review, with both. */
	case_init(&c, "The Mazework (1999) {tmdb-6001}", "The.Mazework.1999.1080p.mkv");
	c.e.name_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.name_id.title = hearth;
	g_assert_false(axis_identify_id_decides(&c.e.name_id, &c.e.reading));
	found[0] = mazework;
	c.e.found = found;
	c.e.n_found = 1;
	c.e.duration_ms = 136 * 60000;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "id_disagrees"));
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}
	case_clear(&c);

	/* An .nfo the names agree with: sure. */
	case_init(&c, "Hearth (1995)", "Hearth.1995.1080p.mkv");
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.nfo_id.where = "the .nfo file";
	c.e.nfo_id.title = hearth;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(has_reason(v, "id_nfo"));
	}
	case_clear(&c);

	/* An .nfo whose names give nothing to check it against (a hash for a
	 * folder, "movie.mkv"): likely, saying so. */
	case_init(&c, "a8f5f167f44f4964e6c998dee827110c", "movie.mkv");
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.nfo_id.where = "the .nfo file";
	c.e.nfo_id.token = "tmdb 6001";
	c.e.nfo_id.title = hearth;
	g_assert_cmpint(c.e.reading.kind, ==, AXIS_IDENTIFY_READ_NONE);
	g_assert_true(axis_identify_id_decides(&c.e.nfo_id, &c.e.reading));
	g_assert_false(axis_identify_nfo_decides(&c.e.nfo_id, &c.e.reading));
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == hearth);
		g_assert_true(has_reason(v, "id_uncorroborated"));
		g_assert_nonnull(strstr(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, 0))->text,
			"the names give nothing to check it against"));
	}
	case_clear(&c);

	/* A stray .nfo of another film, in a folder whose names give no year (so
	 * they do not clearly disagree): likely, not sure. */
	case_init(&c, "Hearth", "hearth.mkv");
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.nfo_id.title = mazework;
	found[0] = hearth;
	c.e.found = found;
	c.e.n_found = 1;
	c.e.detail = hearth;
	c.e.duration_ms = 170 * 60000;
	g_assert_false(axis_identify_nfo_decides(&c.e.nfo_id, &c.e.reading));
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "id_uncorroborated"));
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}
	case_clear(&c);

	/* Its other names corroborate it too ("Die Kielwacht" for "The Keel Watch"). */
	{
		g_autoptr(AxisMetaTitle) boot = film(9, "Die Kielwacht", 1981, 149);
		AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

		alt->title = g_strdup("The Keel Watch");
		boot->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
		g_ptr_array_add(boot->alt_titles, alt);
		case_init(&c, "The Keel Watch (1981)", "movie.mkv");
		c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
		c.e.nfo_id.title = boot;
		g_assert_true(axis_identify_nfo_decides(&c.e.nfo_id, &c.e.reading));
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		}
		case_clear(&c);
	}

	/* An .nfo naming another film than the folder: review, with both. */
	case_init(&c, "The Mazework (1999)", "The.Mazework.1999.1080p.mkv");
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.nfo_id.title = hearth;
	found[0] = mazework;
	c.e.found = found;
	c.e.n_found = 1;
	c.e.duration_ms = 136 * 60000;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "id_disagrees"));
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}
	case_clear(&c);

	/* A name's identifier beats the .nfo's. */
	case_init(&c, "Hearth (1995) {tmdb-6001}", "Hearth.1995.mkv");
	c.e.name_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.name_id.title = hearth;
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_FOUND;
	c.e.nfo_id.title = mazework;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == hearth);
	}
	case_clear(&c);

	/* Identifiers in conflict, or naming nothing: never sure. */
	case_init(&c, "Hearth (1995)", "Hearth.1995.1080p.mkv");
	c.e.name_id.state = AXIS_IDENTIFY_ID_CONFLICT;
	c.e.name_id.where = "the file name";
	found[0] = hearth;
	c.e.found = found;
	c.e.n_found = 1;
	c.e.detail = hearth;
	c.e.duration_ms = 170 * 60000;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "id_conflict"));
	}
	c.e.name_id.state = AXIS_IDENTIFY_ID_NOT_FOUND;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "id_unknown"));
	}
	/* An .nfo that names nothing is no evidence against the names. */
	c.e.name_id.state = AXIS_IDENTIFY_ID_NONE;
	c.e.nfo_id.state = AXIS_IDENTIFY_ID_NOT_FOUND;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}
	/* An import of Axis wins over everything. */
	c.e.imported = mazework;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == mazework);
		g_assert_true(has_reason(v, "imported"));
	}
	case_clear(&c);
}

static AxisIdentifyConfidence
names_case(
	const gchar         *folder,
	const gchar         *file,
	const AxisMetaTitle *const *found,
	guint                n,
	const AxisMetaTitle *detail,
	gint64               duration_ms,
	const gchar         *reason_code
){
	AxisIdentifyConfidence confidence;
	AxisIdentifyVerdict *v;
	Case c;

	case_init(&c, folder, file);
	c.e.found = found;
	c.e.n_found = n;
	c.e.detail = detail;
	c.e.duration_ms = duration_ms;
	v = axis_identify_decide(&c.e);
	confidence = v->confidence;
	if (reason_code != NULL && !has_reason(v, reason_code))
		g_error("%s / %s: no reason %s", folder != NULL ? folder : "-", file, reason_code);
	if (confidence != AXIS_IDENTIFY_UNKNOWN)
		g_assert_nonnull(v->title);
	axis_identify_verdict_free(v);
	case_clear(&c);
	return confidence;
}

static void
test_decide_names(void)
{
	g_autoptr(AxisMetaTitle) hearth = film(1, "Hearth", 1995, 170);
	g_autoptr(AxisMetaTitle) hearth86 = film(3, "Hearth", 1986, 101);
	g_autoptr(AxisMetaTitle) hearth95b = film(4, "Hearth", 1995, 168);
	g_autoptr(AxisMetaTitle) no_runtime = film(5, "Hearth", 1995, 0);
	const AxisMetaTitle *one[1];
	const AxisMetaTitle *two[2];
	const AxisMetaTitle *same_year[2];
	gint64 runs = 170 * 60000;

	one[0] = hearth;
	two[0] = hearth;
	two[1] = hearth86;
	same_year[0] = hearth;
	same_year[1] = hearth95b;

	/* Title and year from the folder and the file, one film, runtime in
	 * tolerance: sure. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.1080p.BluRay.x264-GRP.mkv", one, 1, hearth, runs, "runtime_ok"),
		==, AXIS_IDENTIFY_SURE);
	/* The year tells two films of a title apart. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", two, 2, hearth, runs, NULL), ==, AXIS_IDENTIFY_SURE);
	/* A release-named file at the top of a root: sure too. */
	g_assert_cmpint(names_case(NULL, "Hearth.1995.1080p.BluRay.x264-GRP.mkv", one, 1, hearth, runs, NULL), ==,
		AXIS_IDENTIFY_SURE);
	/* Within 10%: 155 minutes for 170 is 15 off, 17 allowed. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", one, 1, hearth, 155 * 60000, NULL), ==,
		AXIS_IDENTIFY_SURE);

	/* No year: likely. */
	g_assert_cmpint(names_case("Hearth", "Hearth.1080p.mkv", one, 1, hearth, runs, "no_year"), ==, AXIS_IDENTIFY_LIKELY);
	/* Runtime off. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", one, 1, hearth, 100 * 60000, "runtime_off"), ==,
		AXIS_IDENTIFY_LIKELY);
	/* Not probed yet. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", one, 1, hearth, 0, "no_probe"), ==, AXIS_IDENTIFY_LIKELY);
	/* No runtime known. */
	one[0] = no_runtime;
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", one, 1, no_runtime, runs, "no_runtime"), ==,
		AXIS_IDENTIFY_LIKELY);
	one[0] = hearth;
	/* Two candidates of that title and year whose runtimes both fit. */
	g_assert_cmpint(names_case("Hearth (1995)", "Hearth.1995.mkv", same_year, 2, NULL, runs, "ambiguous"), ==,
		AXIS_IDENTIFY_LIKELY);
	/* A year one off. */
	g_assert_cmpint(names_case("Hearth (1996)", "Hearth.1996.mkv", one, 1, hearth, runs, "year_off"), ==,
		AXIS_IDENTIFY_LIKELY);
	/* Only the folder reads as a title. */
	g_assert_cmpint(names_case("Hearth (1995)", "movie.mkv", one, 1, hearth, runs, "folder_only"), ==,
		AXIS_IDENTIFY_LIKELY);
	/* The folder and the file disagree. */
	g_assert_cmpint(names_case("Hearth (1995)", "Rokan.1998.mkv", one, 1, hearth, runs, "disagree"), ==,
		AXIS_IDENTIFY_LIKELY);
	/* No film of that name. */
	g_assert_cmpint(names_case("Rokan (1998)", "Rokan.1998.mkv", one, 1, NULL, runs, "no_match"), ==,
		AXIS_IDENTIFY_UNKNOWN);
	/* Nothing reads as a title. */
	g_assert_cmpint(names_case(NULL, "movie.mkv", one, 1, NULL, runs, "no_title"), ==, AXIS_IDENTIFY_UNKNOWN);
}

/* ── fix-identify-hunt-findings: titles of one name by their records ─ */

/* A full record: its year (its first release), its release date (in the
 * region), its runtime. */
static AxisMetaTitle *
record(
	gint64       id,
	const gchar *title,
	gint         year,
	const gchar *release_date,
	gint         runtime
){
	AxisMetaTitle *t = film(id, title, year, runtime);

	t->release_date = g_strdup(release_date);
	t->detail = TRUE;
	return t;
}

/* The search's record of @full: no runtime, the year the search gave. */
static AxisMetaTitle *
brief_of(
	const AxisMetaTitle *full,
	gint                 year
){
	return film(full->id, full->title, year, 0);
}

typedef struct
{
	const gchar                *folder;
	const gchar                *file;
	const AxisMetaTitle *const *found;
	guint                       n_found;
	const AxisMetaTitle *const *details;
	guint                       n_details;
	gint64                      minutes;
	guint                       n_videos;
	gboolean                    more;
} Named;

/* The verdict on @n's evidence, its reasons checked to hold @code. */
static AxisIdentifyVerdict *
named_case(
	const Named *n,
	const gchar *code
){
	AxisIdentifyVerdict *v;
	Case c;

	case_init(&c, n->folder, n->file);
	c.e.found = n->found;
	c.e.n_found = n->n_found;
	c.e.details = n->details;
	c.e.n_details = n->n_details;
	c.e.details_more = n->more;
	c.e.duration_ms = n->minutes * 60000;
	c.e.n_videos = n->n_videos;
	c.e.editions = c.file->release->editions | (c.folder != NULL ? c.folder->release->editions : 0);
	v = axis_identify_decide(&c.e);
	if (code != NULL && !has_reason(v, code)) {
		GString *all = g_string_new(NULL);
		guint i;

		for (i = 0; i < v->reasons->len; i++) {
			const AxisIdentifyReason *r = g_ptr_array_index(v->reasons, i);

			g_string_append_printf(all, " [%s] %s;", r->code, r->text);
		}
		g_error("%s / %s: no reason %s (%s )", n->folder != NULL ? n->folder : "-", n->file, code, all->str);
	}
	case_clear(&c);
	return v;
}

static const gchar *
reason_of(
	const AxisIdentifyVerdict *v,
	const gchar               *code
){
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisIdentifyReason *r = g_ptr_array_index(v->reasons, i);

		if (g_strcmp0(r->code, code) == 0)
			return r->text;
	}
	return NULL;
}

/*
 * Item A: titles of one name within a year of each other are told apart by
 * their full records' years and runtimes, never by the provider's order;
 * what does not tell them apart stays ambiguous, naming each.
 */
static void
test_same_name_titles(void)
{
	g_autoptr(AxisMetaTitle) cove14 = record(41, "Lantern Cove", 2014, "2014-05-02", 98);
	g_autoptr(AxisMetaTitle) cove15 = record(42, "Lantern Cove", 2015, "2015-03-20", 97);
	g_autoptr(AxisMetaTitle) cove14b = record(43, "Lantern Cove", 2014, "2014-10-10", 131);
	g_autoptr(AxisMetaTitle) cove14c = record(44, "Lantern Cove", 2014, "2014-11-01", 100);
	g_autoptr(AxisMetaTitle) cove14d = record(45, "Lantern Cove", 2014, "2014-11-01", 0);
	g_autoptr(AxisMetaTitle) b14 = brief_of(cove14, 2014);
	g_autoptr(AxisMetaTitle) b15 = brief_of(cove15, 2015);
	g_autoptr(AxisMetaTitle) b14b = brief_of(cove14b, 2014);
	g_autoptr(AxisMetaTitle) b14c = brief_of(cove14c, 2014);
	g_autoptr(AxisMetaTitle) b14d = brief_of(cove14d, 2014);
	const AxisMetaTitle *found[2];
	const AxisMetaTitle *details[2];
	Named n;

	memset(&n, 0, sizeof(n));
	n.folder = "Lantern Cove (2014)";
	n.file = "Lantern.Cove.2014.1080p.BluRay.mkv";
	n.found = found;
	n.n_found = 2;
	n.details = details;
	n.n_details = 2;
	n.minutes = 98;

	/* The one of the year whose runtime fits, beside one a year off whose
	 * runtime fits too: held, both named (review item 5: a release's year
	 * may be its festival's), in either order of the answers. */
	found[0] = b15;
	found[1] = b14;
	details[0] = cove15;
	details[1] = cove14;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_nonnull(strstr(reason_of(v, "ambiguous"), "Lantern Cove (2015, 97 minutes)"));
		g_assert_nonnull(strstr(reason_of(v, "ambiguous"), "Lantern Cove (2014, 98 minutes)"));
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}
	/* One a year off whose runtime is known and does not fit, by the
	 * tolerance itself: the one of the year is sure, in either order. */
	{
		g_autoptr(AxisMetaTitle) long15 = record(48, "Lantern Cove", 2015, "2015-03-20", 112);
		g_autoptr(AxisMetaTitle) blong15 = brief_of(long15, 2015);

		found[0] = blong15;
		found[1] = b14;
		details[0] = long15;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "others_ruled_out");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
			g_assert_true(v->title == cove14);
			g_assert_true(has_reason(v, "runtime_ok"));
		}
		found[0] = b14;
		found[1] = blong15;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, NULL);

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
			g_assert_true(v->title == cove14);
		}
	}
	/* A case met in use: a documentary (2012, 95 minutes) and a drama
	 * (2013, 100 minutes) of one name; the drama's folder named with 2012,
	 * the file 100 minutes: held, with both named. */
	{
		g_autoptr(AxisMetaTitle) doc = record(91, "Salt Lantern", 2012, "2012-04-01", 95);
		g_autoptr(AxisMetaTitle) drama = record(92, "Salt Lantern", 2013, "2013-03-01", 100);
		g_autoptr(AxisMetaTitle) bdoc = brief_of(doc, 2012);
		g_autoptr(AxisMetaTitle) bdrama = brief_of(drama, 2013);
		const AxisMetaTitle *both[2];
		const AxisMetaTitle *both_details[2];
		Named festival;

		both[0] = bdoc;
		both[1] = bdrama;
		both_details[0] = doc;
		both_details[1] = drama;
		memset(&festival, 0, sizeof(festival));
		festival.folder = "Salt Lantern (2012)";
		festival.file = "Salt.Lantern.2012.1080p.WEB-DL.mkv";
		festival.found = both;
		festival.n_found = 2;
		festival.details = both_details;
		festival.n_details = 2;
		festival.minutes = 100;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&festival, "ambiguous");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
			g_assert_nonnull(strstr(reason_of(v, "ambiguous"), "Salt Lantern (2012, 95 minutes)"));
			g_assert_nonnull(strstr(reason_of(v, "ambiguous"), "Salt Lantern (2013, 100 minutes)"));
			g_assert_cmpuint(v->candidates->len, ==, 2);
		}
	}
	found[0] = b14;
	found[1] = b15;
	details[0] = cove15;

	/* Two of the year, the other's runtime off: sure. */
	found[1] = b14b;
	details[0] = cove14b;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "others_ruled_out");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == cove14);
	}

	/* Two of the year that both fit: ambiguous, naming both with their
	 * years and runtimes. */
	found[1] = b14c;
	details[0] = cove14c;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");
		const gchar *text = reason_of(v, "ambiguous");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_nonnull(strstr(text, "Lantern Cove (2014, 98 minutes)"));
		g_assert_nonnull(strstr(text, "Lantern Cove (2014, 100 minutes)"));
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}

	/* One of the year with no runtime known cannot be ruled out. */
	found[1] = b14d;
	details[0] = cove14d;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_nonnull(strstr(reason_of(v, "ambiguous"), "no runtime known"));
	}

	/* Not probed yet: only the year separates them, likely (and looked at
	 * again when the probe changes the folder). */
	found[1] = b15;
	details[0] = cove15;
	n.minutes = 0;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "no_probe");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == cove14);
	}
	n.minutes = 98;

	/* The year's one runs off, the one a year off fits: ambiguous. */
	n.minutes = 131;
	{
		g_autoptr(AxisMetaTitle) long15 = record(46, "Lantern Cove", 2015, "2015-03-20", 130);
		g_autoptr(AxisMetaTitle) blong15 = brief_of(long15, 2015);

		found[1] = blong15;
		details[0] = long15;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		}
	}
	n.minutes = 98;

	/* Two a year off: ambiguous. */
	n.folder = "Lantern Cove (2013)";
	n.file = "Lantern.Cove.2013.mkv";
	found[1] = b15;
	details[0] = cove15;
	{
		g_autoptr(AxisMetaTitle) cove12 = record(47, "Lantern Cove", 2012, "2012-01-01", 98);
		g_autoptr(AxisMetaTitle) b12 = brief_of(cove12, 2012);

		found[1] = b12;
		details[0] = cove12;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		}
	}
	n.folder = "Lantern Cove (2014)";
	n.file = "Lantern.Cove.2014.1080p.BluRay.mkv";

	/* More of the name than were read in full: likely. */
	found[1] = b14b;
	details[0] = cove14b;
	n.more = TRUE;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "more_titles");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == cove14);
	}
	n.more = FALSE;

	/* Records not read: the search's records alone, no runtime known,
	 * cannot be told apart. */
	found[1] = b14c;
	n.details = NULL;
	n.n_details = 0;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
}

/*
 * Item B: the year is the full record's, its first release or its release
 * in the region; the search's year never decides.
 */
static void
test_year_by_full_record(void)
{
	/* First released in 2013, in cinemas in the region in 2014; the search
	 * said 2015. */
	g_autoptr(AxisMetaTitle) full = record(51, "Copper Wake", 2013, "2014-02-07", 104);
	g_autoptr(AxisMetaTitle) brief = brief_of(full, 2015);
	const AxisMetaTitle *found[1];
	const AxisMetaTitle *details[1];
	Named n;

	full->regional_year = 2014;
	found[0] = brief;
	details[0] = full;
	memset(&n, 0, sizeof(n));
	n.found = found;
	n.n_found = 1;
	n.details = details;
	n.n_details = 1;
	n.minutes = 104;

	n.folder = "Copper Wake (2014)";
	n.file = "Copper.Wake.2014.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "year_regional");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_false(has_reason(v, "year_off"));
	}
	n.folder = "Copper Wake (2013)";
	n.file = "Copper.Wake.2013.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_ok");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_false(has_reason(v, "year_regional"));
	}
	/* The search's year alone is one off the full record's 2014: likely. */
	n.folder = "Copper Wake (2015)";
	n.file = "Copper.Wake.2015.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "year_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	{
		gint years[3];

		g_assert_cmpuint(axis_identify_title_years(full, years), ==, 2);
		g_assert_cmpint(years[0], ==, 2013);
		g_assert_cmpint(years[1], ==, 2014);
		g_assert_cmpuint(axis_identify_title_years(brief, years), ==, 1);
		g_assert_cmpuint(axis_identify_title_years(NULL, years), ==, 0);
	}
}

/*
 * fix-identify-regional-years, A: a year two off the full record's (a
 * festival's, a first showing elsewhere) is likely, naming both years; it
 * was "no movie fits" before.  It never stands in the way of a title of the
 * year, nor of one a year off.
 */
static void
test_two_years_off(void)
{
	g_autoptr(AxisMetaTitle) wharf = record(71, "Copper Wharf", 2022, "2022-05-06", 101);
	g_autoptr(AxisMetaTitle) wharf_brief = brief_of(wharf, 2022);
	g_autoptr(AxisMetaTitle) exact = record(72, "Copper Wharf", 2020, "2020-03-01", 101);
	g_autoptr(AxisMetaTitle) exact_brief = brief_of(exact, 2020);
	g_autoptr(AxisMetaTitle) near = record(73, "Copper Wharf", 2021, "2021-03-01", 101);
	g_autoptr(AxisMetaTitle) near_brief = brief_of(near, 2021);
	const AxisMetaTitle *found[2];
	const AxisMetaTitle *details[2];
	Named n;

	found[0] = wharf_brief;
	details[0] = wharf;
	memset(&n, 0, sizeof(n));
	n.found = found;
	n.n_found = 1;
	n.details = details;
	n.n_details = 1;
	n.minutes = 101;
	n.folder = "Copper Wharf (2020)";
	n.file = "Copper.Wharf.2020.1080p.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "year_off");
		const AxisIdentifyReason *r = NULL;
		guint i;

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == wharf);
		for (i = 0; i < v->reasons->len; i++) {
			if (g_strcmp0(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, i))->code, "year_off") == 0)
				r = g_ptr_array_index(v->reasons, i);
		}
		g_assert_nonnull(strstr(r->text, "two off: 2020 in the names, 2022 for the movie"));
	}
	/* Three off is as before: no movie fits. */
	n.folder = "Copper Wharf (2019)";
	n.file = "Copper.Wharf.2019.1080p.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "no_match");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_UNKNOWN);
	}
	/* Beside a title of the year, the one two off does not stand in its
	 * way: sure. */
	n.folder = "Copper Wharf (2020)";
	n.file = "Copper.Wharf.2020.1080p.mkv";
	found[1] = exact_brief;
	details[1] = exact;
	n.n_found = 2;
	n.n_details = 2;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "others_ruled_out");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == exact);
	}
	/* Beside one a year off, that one is taken, likely, as the nearest. */
	found[1] = near_brief;
	details[1] = near;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "others_ruled_out");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == near);
		g_assert_true(has_reason(v, "year_off"));
	}
}

/*
 * fix-identify-regional-years, C: a film's name whose title holds an
 * episode's or a special's number ("S05M01") says it looks like part of a
 * series, and the search starts from the words after the mark.
 */
static void
test_series_like(void)
{
	g_autofree gchar *mark = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *words = NULL;
	Named n;

	g_assert_true(axis_identify_series_mark("Copper Lantern Show S05M01 Night Harbour", &mark, &after));
	g_assert_cmpstr(mark, ==, "S05M01");
	g_assert_cmpstr(after, ==, "Night Harbour");
	g_clear_pointer(&mark, g_free);
	g_clear_pointer(&after, g_free);
	g_assert_true(axis_identify_series_mark("Copper Lantern Show 5x01", &mark, &after));
	g_assert_cmpstr(mark, ==, "5x01");
	g_assert_cmpstr(after, ==, "");
	g_assert_true(axis_identify_series_mark("Lantern s1e2 Pilot", NULL, NULL));
	/* The first word is a title's ("S01 (2008)"), and words that only look
	 * alike are not marks. */
	g_assert_false(axis_identify_series_mark("S05M01 Night Harbour", NULL, NULL));
	g_assert_false(axis_identify_series_mark("Paper Lantern Sky", NULL, NULL));
	g_assert_false(axis_identify_series_mark("Copper SM Harbour", NULL, NULL));
	g_assert_false(axis_identify_series_mark("Copper S5 Harbour", NULL, NULL));
	g_assert_false(axis_identify_series_mark("Copper 1080x Harbour", NULL, NULL));
	g_assert_false(axis_identify_series_mark(NULL, NULL, NULL));

	words = axis_identify_search_words(AXIS_META_KIND_FILM, "Copper Lantern Show S05M01 Night Harbour");
	g_assert_cmpstr(words, ==, "Night Harbour");
	g_clear_pointer(&words, g_free);
	words = axis_identify_search_words(AXIS_META_KIND_FILM, "Copper Lantern Show 5x01");
	g_assert_cmpstr(words, ==, "Copper Lantern Show 5x01");
	g_clear_pointer(&words, g_free);
	words = axis_identify_search_words(AXIS_META_KIND_FILM, "Paper Lantern Sky");
	g_assert_cmpstr(words, ==, "Paper Lantern Sky");
	g_clear_pointer(&words, g_free);
	/* A series' name comes before its numbers: its search is as read. */
	words = axis_identify_search_words(AXIS_META_KIND_SERIES, "Copper Lantern Show S05M01 Night Harbour");
	g_assert_cmpstr(words, ==, "Copper Lantern Show S05M01 Night Harbour");
	g_assert_null(axis_identify_search_words(AXIS_META_KIND_FILM, NULL));

	/* The unknown verdict says so, first, and what settles it. */
	memset(&n, 0, sizeof(n));
	n.file = "Copper Lantern Show S05M01 Night Harbour (2007 360p re-rip).mp4";
	n.minutes = 90;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "series_like");
		const AxisIdentifyReason *first = g_ptr_array_index(v->reasons, 0);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_UNKNOWN);
		g_assert_cmpstr(first->code, ==, "series_like");
		g_assert_nonnull(strstr(first->text, "the name looks like part of a series: S05M01"));
		g_assert_nonnull(strstr(first->text, "\"Night Harbour\""));
		g_assert_true(has_reason(v, "no_match"));
		g_assert_nonnull(strstr(axis_identify_settle_text(AXIS_META_KIND_FILM, v->reasons), "part of a series"));
	}
	/* An ordinary name that fits nothing says nothing of a series. */
	n.file = "Copper Lantern Harbour (2007).mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "no_match");

		g_assert_false(has_reason(v, "series_like"));
	}
}

/*
 * Item H: a folder "Title (Year)" holding one video whose name carries no
 * title is sure with the year and the runtime; with several videos it is
 * held.  The wider runtime band for the one film of its name and year.
 */
static void
test_one_name_and_wider_band(void)
{
	g_autoptr(AxisMetaTitle) ember = record(61, "Ember Rows", 2016, "2016-09-09", 98);
	g_autoptr(AxisMetaTitle) brief = brief_of(ember, 2016);
	g_autoptr(AxisMetaTitle) other = record(62, "Ember Rows", 2017, "2017-01-01", 0);
	g_autoptr(AxisMetaTitle) other_brief = brief_of(other, 2017);
	const AxisMetaTitle *found[2];
	const AxisMetaTitle *details[2];
	Named n;

	found[0] = brief;
	details[0] = ember;
	memset(&n, 0, sizeof(n));
	n.found = found;
	n.n_found = 1;
	n.details = details;
	n.n_details = 1;
	n.minutes = 99;

	/* One name alone, one video: sure; several: held. */
	n.folder = "Ember Rows (2016)";
	n.file = "movie.mkv";
	n.n_videos = 1;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_ok");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_false(has_reason(v, "folder_only"));
	}
	n.file = "a8f5f167f44f4964e6c998dee827110c.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, NULL);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}
	n.n_videos = 2;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "folder_only");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	/* The file alone in a folder that reads as nothing, one video. */
	n.folder = "a8f5f167f44f4964e6c998dee827110c";
	n.file = "Ember.Rows.2016.1080p.mkv";
	n.n_videos = 1;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, NULL);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}

	/* The wider band: 20 minutes off 98 is beyond 10 % and 5 minutes,
	 * within 25 %. */
	n.folder = "Ember Rows (2016)";
	n.file = "Ember.Rows.2016.1080p.mkv";
	n.minutes = 118;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_loose");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}
	n.minutes = 76;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_loose");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}
	/* Beyond it: held. */
	n.minutes = 128;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	/* An edition word explains a longer cut, up to 60 % longer. */
	n.file = "Ember.Rows.2016.Extended.Cut.1080p.mkv";
	n.minutes = 150;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_loose");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_nonnull(strstr(reason_of(v, "runtime_loose"), "another cut"));
	}
	n.minutes = 160;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	/* Not when another title has the name within a year. */
	n.file = "Ember.Rows.2016.1080p.mkv";
	n.minutes = 118;
	found[1] = other_brief;
	details[1] = other;
	n.n_found = 2;
	n.n_details = 2;
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	n.n_found = 1;
	n.n_details = 1;
	/* Nor on one name alone. */
	n.file = "movie.mkv";
	{
		g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}

	g_assert_true(axis_identify_runtime_loose(118 * 60000, 98, FALSE));
	g_assert_false(axis_identify_runtime_loose(124 * 60000, 98, FALSE));
	g_assert_true(axis_identify_runtime_loose(156 * 60000, 98, TRUE));
	g_assert_false(axis_identify_runtime_loose(157 * 60000, 98, TRUE));
	g_assert_true(axis_identify_runtime_loose(74 * 60000, 98, TRUE));
	g_assert_false(axis_identify_runtime_loose(73 * 60000, 98, TRUE));
	/* 15 minutes count for a short film. */
	g_assert_true(axis_identify_runtime_loose(55 * 60000, 40, FALSE));
	g_assert_false(axis_identify_runtime_loose(0, 98, TRUE));
	g_assert_false(axis_identify_runtime_loose(98 * 60000, 0, TRUE));
}

/* Items C and I1: a verdict that waits, and a folder with too many videos. */
static void
test_waiting_and_cut(void)
{
	g_autoptr(AxisMetaTitle) ember = record(61, "Ember Rows", 2016, "2016-09-09", 98);
	g_autoptr(AxisMetaTitle) brief = brief_of(ember, 2016);
	const AxisMetaTitle *found[1];
	Case c;

	found[0] = brief;
	case_init(&c, "Ember Rows (2016)", "Ember.Rows.2016.mkv");
	c.e.found = found;
	c.e.n_found = 1;
	c.e.duration_ms = 98 * 60000;
	c.e.untold = brief;
	c.e.untold_problem = "Fixture answered 500";
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_true(v->waiting);
		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_UNKNOWN);
		g_assert_null(v->title);
		g_assert_true(has_reason(v, "provider_waiting"));
		g_assert_nonnull(strstr(reason_of(v, "provider_waiting"), "Fixture answered 500"));
		g_assert_nonnull(strstr(axis_identify_settle_text(AXIS_META_KIND_FILM, v->reasons), "did not answer"));
	}
	/* Review item 8: the candidates known so far, not only the title
	 * awaited (which may be no title of the name at all). */
	{
		g_autoptr(AxisMetaTitle) other = record(63, "Ember Rows", 2017, "2017-01-01", 90);
		g_autoptr(AxisMetaTitle) stranger = film(64, "Glass Wren", 2016, 0);
		const AxisMetaTitle *three[3];
		AxisIdentifyEvidence e = c.e;

		three[0] = stranger;
		three[1] = brief;
		three[2] = other;
		e.found = three;
		e.n_found = 3;
		e.untold = stranger;
		e.untold_problem = NULL;
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&e);

			g_assert_true(v->waiting);
			g_assert_cmpuint(v->candidates->len, ==, 2);
			g_assert_true(g_ptr_array_index(v->candidates, 0) == brief);
			g_assert_true(g_ptr_array_index(v->candidates, 1) == other);
			g_assert_nonnull(strstr(reason_of(v, "provider_waiting"), "Glass Wren (2016)"));
		}
	}

	/* More videos than are read: the folder's unit is held, saying so. */
	c.e.untold = NULL;
	c.e.untold_problem = NULL;
	c.e.detail = ember;
	c.e.not_read = 3;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_false(v->waiting);
		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(v->title == ember);
		g_assert_true(has_reason(v, "too_many_videos"));
		g_assert_nonnull(strstr(axis_identify_settle_text(AXIS_META_KIND_FILM, v->reasons), "split it"));
	}
	case_clear(&c);
	/* A film of its own in such a folder stays sure, saying so. */
	case_init(&c, NULL, "Ember.Rows.2016.mkv");
	c.e.found = found;
	c.e.n_found = 1;
	c.e.duration_ms = 98 * 60000;
	c.e.detail = ember;
	c.e.not_read = 3;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(has_reason(v, "too_many_videos"));
	}
	case_clear(&c);
}

/*
 * The review of fix-identify-hunt-findings: items 1 (a year in the region),
 * 2 (a title found only by another name), 7 (the words that widen the
 * runtime band) and the wider band beside a namesake of another year.
 */
static void
test_review_rules(void)
{
	Named n;

	memset(&n, 0, sizeof(n));
	/* Item 1: a disc in the region decades later is no year of the film.
	 * "Iron Orchard" (1975, 104 minutes) came out on disc in the region in
	 * 2009 (its stored release date); "Iron Orchard" (2009) runs 85 minutes.
	 * A folder "Iron Orchard (2009)" with a file of 101 minutes is not linked
	 * to the 1975 film, nor taken as the 2009 one by the wider band. */
	{
		g_autoptr(AxisMetaTitle) old = record(101, "Iron Orchard", 1975, "2009-05-05", 104);
		g_autoptr(AxisMetaTitle) young = record(102, "Iron Orchard", 2009, "2009-08-01", 85);
		const AxisMetaTitle *found[2];

		found[0] = old;
		found[1] = young;
		n.folder = "Iron Orchard (2009)";
		n.file = "Iron.Orchard.2009.1080p.BluRay.mkv";
		n.found = found;
		n.n_found = 2;
		n.minutes = 101;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
			g_assert_true(v->title == young);
		}
	}
	/* Item 1: a theatrical year in the region is no better than a year off
	 * when another title of the name was first released within a year of
	 * the folder's: held. */
	{
		g_autoptr(AxisMetaTitle) wake13 = record(103, "Copper Wake", 2013, "2014-02-07", 104);
		g_autoptr(AxisMetaTitle) wake14 = record(104, "Copper Wake", 2014, "2014-06-06", 80);
		const AxisMetaTitle *found[2];

		wake13->regional_year = 2014;
		found[0] = wake13;
		found[1] = wake14;
		n.folder = "Copper Wake (2014)";
		n.file = "Copper.Wake.2014.mkv";
		n.found = found;
		n.n_found = 2;
		n.minutes = 104;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "ambiguous");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		}
		/* Alone, the year in the region counts. */
		n.n_found = 1;
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "year_regional");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
			g_assert_true(v->title == wake13);
		}
	}
	/* Item 2: a title that has the name only as another name, found among
	 * the first answers read for their other names when not all were: at
	 * most likely; sure when every answer was read. */
	{
		g_autoptr(AxisMetaTitle) orchard = record(105, "Quiet Orchard", 2011, "2011-06-01", 104);
		g_autoptr(AxisMetaTitle) brief = brief_of(orchard, 2011);
		AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);
		const AxisMetaTitle *found[1];
		const AxisMetaTitle *details[1];
		Case c;

		alt->title = g_strdup("Silver Orchard");
		orchard->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
		g_ptr_array_add(orchard->alt_titles, alt);
		found[0] = brief;
		details[0] = orchard;
		case_init(&c, "Silver Orchard (2011)", "Silver.Orchard.2011.mkv");
		c.e.found = found;
		c.e.n_found = 1;
		c.e.details = details;
		c.e.n_details = 1;
		c.e.duration_ms = 104 * 60000;
		c.e.others_partial = TRUE;
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
			g_assert_true(has_reason(v, "other_name_partial"));
			g_assert_true(v->title == orchard);
		}
		c.e.others_partial = FALSE;
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		}
		/* The title itself is never "only another name". */
		case_clear(&c);
		case_init(&c, "Quiet Orchard (2011)", "Quiet.Orchard.2011.mkv");
		c.e.found = found;
		c.e.n_found = 1;
		c.e.details = details;
		c.e.n_details = 1;
		c.e.duration_ms = 104 * 60000;
		c.e.others_partial = TRUE;
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		}
		case_clear(&c);
	}
	/* Item 7: "Collection", "Version" or "Cut" alone widen nothing. */
	{
		g_autoptr(AxisMetaTitle) ember = record(106, "Ember Rows", 2016, "2016-09-09", 98);
		const AxisMetaTitle *found[1];
		static const gchar *const plain[] = { "Ember.Rows.2016.Collection.1080p.mkv", "Ember.Rows.2016.Version.1080p.mkv",
			"Ember.Rows.2016.Cut.1080p.mkv", NULL };
		guint i;

		found[0] = ember;
		n.folder = "Ember Rows (2016)";
		n.found = found;
		n.n_found = 1;
		n.minutes = 150;
		for (i = 0; plain[i] != NULL; i++) {
			g_autoptr(AxisIdentifyVerdict) v = NULL;

			n.file = plain[i];
			v = named_case(&n, "runtime_off");
			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		}
		n.file = "Ember.Rows.2016.Directors.Cut.1080p.mkv";
		{
			g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_loose");

			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		}
		/* The wider band holds only for the one title of its name among
		 * the answers: a namesake of another year (here 1980, 120 minutes)
		 * is not ruled out by a file that runs as it does. */
		{
			g_autoptr(AxisMetaTitle) old = record(107, "Ember Rows", 1980, "1980-01-01", 120);
			const AxisMetaTitle *two[2];

			two[0] = ember;
			two[1] = old;
			n.file = "Ember.Rows.2016.1080p.mkv";
			n.found = two;
			n.n_found = 2;
			n.minutes = 118;
			{
				g_autoptr(AxisIdentifyVerdict) v = named_case(&n, "runtime_off");

				g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
			}
		}
	}
	g_assert_cmpuint(AXIS_IDENTIFY_EDITION_LONGER & AXIS_RELEASE_EDITION_OTHER, ==, 0);
	g_assert_cmpuint(AXIS_IDENTIFY_EDITION_LONGER & AXIS_RELEASE_EDITION_EXTENDED, !=, 0);
}

/* The order in which titles of a name are read in full: by their years'
 * nearness, the provider's order among equals; others left out. */
static void
test_named_order(void)
{
	g_autoptr(AxisMetaTitle) a = film(71, "Glass Wren", 1999, 0);
	g_autoptr(AxisMetaTitle) b = film(72, "Glass Wren", 2015, 0);
	g_autoptr(AxisMetaTitle) c = film(73, "Glass Wren", 0, 0);
	g_autoptr(AxisMetaTitle) d = film(74, "Glass Wren Returns", 2014, 0);
	g_autoptr(AxisMetaTitle) e = film(75, "Glass Wren", 2014, 0);
	g_autoptr(AxisIdentifyName) name = read_name("Glass Wren (2014)");
	const AxisMetaTitle *found[5];
	guint order[5];

	found[0] = a;
	found[1] = b;
	found[2] = c;
	found[3] = d;
	found[4] = e;
	g_assert_cmpuint(axis_identify_named_order(name, found, 5, order), ==, 4);
	g_assert_cmpuint(order[0], ==, 4);
	g_assert_cmpuint(order[1], ==, 1);
	g_assert_cmpuint(order[2], ==, 0);
	g_assert_cmpuint(order[3], ==, 2);
}

static void
test_runtime(void)
{
	gint off = 0;

	/* 10% of 120 is 12: 131 is off by 11, fits; 133 by 13, does not. */
	g_assert_true(axis_identify_runtime_fits(131 * 60000, 120, 10, 5, &off));
	g_assert_cmpint(off, ==, 11);
	g_assert_false(axis_identify_runtime_fits(133 * 60000, 120, 10, 5, &off));
	/* 10% of 30 is 3: the 5 minutes count. */
	g_assert_true(axis_identify_runtime_fits(35 * 60000, 30, 10, 5, NULL));
	g_assert_false(axis_identify_runtime_fits(36 * 60000, 30, 10, 5, NULL));
	g_assert_false(axis_identify_runtime_fits(0, 120, 10, 5, NULL));
	g_assert_false(axis_identify_runtime_fits(120 * 60000, 0, 10, 5, NULL));
	g_assert_false(axis_identify_runtime_fits(G_MAXINT64, 120, 10, 5, &off));
}

/* ── A found file's quality ─────────────────────────────────────── */

static void
check_quality(
	const gchar           *file,
	const gchar           *folder,
	const gchar           *probe,
	AxisQuality            quality,
	AxisReleaseResolution  resolution
){
	g_autoptr(AxisRelease) f = axis_release_parse_full(file, -1, 2026);
	g_autoptr(AxisRelease) d = folder != NULL ? axis_release_parse_full(folder, -1, 2026) : NULL;
	AxisIdentifyQuality q;

	axis_identify_file_quality(f, d, probe, &q);
	if (q.quality != quality || q.resolution != resolution)
		g_error("%s: %s at %d (%s)", file, axis_quality_name(q.quality), q.resolution, q.text);
	g_assert_nonnull(q.text);
	axis_identify_quality_clear(&q);
}

static void
test_file_quality(void)
{
	check_quality("Hearth.1995.1080p.BluRay.x264-GRP.mkv", NULL, "1080p", AXIS_QUALITY_BLURAY_1080P,
		AXIS_RELEASE_RESOLUTION_NONE);
	/* The probe completes a missing resolution. */
	check_quality("Hearth.1995.BluRay.x264-GRP.mkv", NULL, "1080p", AXIS_QUALITY_BLURAY_1080P,
		AXIS_RELEASE_RESOLUTION_NONE);
	/* The folder's name completes the file's. */
	check_quality("hearth.mkv", "Hearth.1995.720p.WEB-DL.DDP5.1-GRP", "1080p", AXIS_QUALITY_WEBDL_720P,
		AXIS_RELEASE_RESOLUTION_NONE);
	/* No source: an unknown source at the resolution known. */
	check_quality("Hearth (1995).mkv", NULL, "1080p", AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_1080P);
	check_quality("Hearth.1995.2160p.mkv", NULL, NULL, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_2160P);
	check_quality("Hearth (1995).mkv", NULL, NULL, AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_NONE);
	/* DVD and recordings by their source alone. */
	check_quality("Hearth.1995.DVDRip.XviD.avi", NULL, "sd", AXIS_QUALITY_DVD, AXIS_RELEASE_RESOLUTION_NONE);
}

/* ── An unknown source on disk ──────────────────────────────────── */

static AxisQualityProfile
builtin(const gchar *slug)
{
	AxisQualityProfile p;

	g_assert_true(axis_quality_builtin_profile(slug, &p));
	return p;
}

static AxisQualityVerdict *
against_unknown(
	const AxisQualityProfile *p,
	AxisReleaseResolution     resolution,
	const gchar              *name
){
	g_autoptr(AxisRelease) release = axis_release_parse_full(name, -1, 2026);
	AxisQualityOnDisk disk;
	AxisQualityContext ctx;
	AxisQualityTitle title;
	AxisQualityCandidate c;

	memset(&disk, 0, sizeof(disk));
	disk.quality = AXIS_QUALITY_UNKNOWN;
	disk.resolution = resolution;
	memset(&title, 0, sizeof(title));
	title.noun = "film";
	memset(&ctx, 0, sizeof(ctx));
	ctx.profile = p;
	ctx.title = &title;
	ctx.disk = &disk;
	memset(&c, 0, sizeof(c));
	c.release = release;
	c.name = name;
	c.match = AXIS_RELEASE_MATCH_YES;
	c.size_bytes = -1;
	c.protocol = AXIS_DOWNLOAD_USENET;
	c.seeders = -1;
	return axis_quality_decide_one(&ctx, &c);
}

static void
test_unknown_source(void)
{
	AxisQualityProfile p = builtin("anything");
	g_autofree gchar *canonical = NULL;
	AxisQualityProfile back;

	/* The order: below every stated source of its resolution, above the
	 * resolution below. */
	g_assert_cmpint(axis_quality_compare_on_disk(AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_1080P,
		AXIS_QUALITY_HDTV_1080P, AXIS_RELEASE_RESOLUTION_NONE), <, 0);
	g_assert_cmpint(axis_quality_compare_on_disk(AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_1080P,
		AXIS_QUALITY_BLURAY_720P, AXIS_RELEASE_RESOLUTION_NONE), >, 0);
	g_assert_cmpint(axis_quality_compare_on_disk(AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_2160P,
		AXIS_QUALITY_REMUX_1080P, AXIS_RELEASE_RESOLUTION_NONE), >, 0);
	g_assert_cmpint(axis_quality_compare_on_disk(AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_NONE,
		AXIS_QUALITY_CAM, AXIS_RELEASE_RESOLUTION_NONE), <, 0);
	g_assert_true(axis_quality_above_unknown(AXIS_QUALITY_HDTV_1080P, AXIS_RELEASE_RESOLUTION_1080P));
	g_assert_false(axis_quality_above_unknown(AXIS_QUALITY_BLURAY_720P, AXIS_RELEASE_RESOLUTION_1080P));
	g_assert_false(axis_quality_above_unknown(AXIS_QUALITY_CAM, AXIS_RELEASE_RESOLUTION_480P));
	g_assert_false(axis_quality_above_unknown(AXIS_QUALITY_UNKNOWN, AXIS_RELEASE_RESOLUTION_480P));

	/* By default (every built-in profile): never upgraded, and said so. */
	g_assert_false(p.upgrade_unknown);
	{
		g_autoptr(AxisQualityVerdict) v = against_unknown(&p, AXIS_RELEASE_RESOLUTION_1080P,
			"Some.Film.2020.2160p.BluRay.REMUX.HEVC.TrueHD.7.1-GRP");

		g_assert_false(v->accepted);
		g_assert_true(v->acceptable);
		g_assert_nonnull(strstr(((AxisQualityReason *)g_ptr_array_index(v->reasons, 0))->text,
			"does not upgrade a file of unknown source"));
	}
	/* When the profile says so: a stated source of its resolution or higher. */
	p.upgrade_unknown = TRUE;
	{
		g_autoptr(AxisQualityVerdict) better = against_unknown(&p, AXIS_RELEASE_RESOLUTION_1080P,
			"Some.Film.2020.1080p.BluRay.x264-GRP");
		g_autoptr(AxisQualityVerdict) lower = against_unknown(&p, AXIS_RELEASE_RESOLUTION_1080P,
			"Some.Film.2020.720p.BluRay.x264-GRP");
		g_autoptr(AxisQualityVerdict) unknown = against_unknown(&p, AXIS_RELEASE_RESOLUTION_1080P,
			"Some.Film.2020.x264-GRP");

		g_assert_true(better->accepted);
		g_assert_true(better->upgrade);
		g_assert_false(lower->accepted);
		g_assert_false(unknown->accepted);
	}
	/* The switch is stored only when on, and read back. */
	canonical = axis_quality_profile_canonical(&p);
	g_assert_nonnull(strstr(canonical, "\"unknown\":true}}"));
	g_assert_true(axis_quality_profile_parse(canonical, -1, &back, NULL));
	g_assert_true(back.upgrade_unknown);
	p.upgrade_unknown = FALSE;
	g_clear_pointer(&canonical, g_free);
	canonical = axis_quality_profile_canonical(&p);
	g_assert_null(strstr(canonical, "\"unknown\":true}"));
	g_assert_true(axis_quality_profile_parse(canonical, -1, &back, NULL));
	g_assert_false(back.upgrade_unknown);
	/* Absent, false, or not a boolean. */
	{
		g_autofree gchar *with_false = NULL;
		g_autofree gchar *wrong = NULL;
		gchar *at = strstr(canonical, "\"score\":");

		g_assert_nonnull(at);
		with_false = g_strdup(canonical);
		at = strstr(with_false, "}}");
		g_assert_nonnull(at);
		wrong = g_strdup_printf("%.*s,\"unknown\":1}}", (gint)(at - with_false), with_false);
		g_free(with_false);
		with_false = g_strdup_printf("%.*s,\"unknown\":false}}", (gint)(strstr(canonical, "}}") - canonical),
			canonical);
		g_assert_true(axis_quality_profile_parse(with_false, -1, &back, NULL));
		g_assert_false(back.upgrade_unknown);
		g_assert_false(axis_quality_profile_parse(wrong, -1, &back, NULL));
	}
}

/* ── Read-only ──────────────────────────────────────────────────── */

/* No file of the identification layer reaches for a way to change a file:
 * not the write surface, not a write guard, not a call that creates,
 * renames, links or removes, not a descriptor opened for writing. */
static void
test_read_only_source(void)
{
	static const gchar *const forbidden[] = {
		"axis-volume-write", "AXIS_VOLUME_INTENT_WRITE", "unlinkat", "unlink(", "renameat", "rename(", "linkat",
		"symlinkat", "mkdirat", "mkdir(", "O_WRONLY", "O_RDWR", "O_CREAT", "O_TRUNC", "fopen(", "g_file_set_contents",
		"ftruncate", "write(", "fchmod", "fchown", "utimensat", "futimens", "g_mkdir", "g_remove", "g_rename", NULL
	};
	static const gchar *const files[] = {
		"src/identify/axis-identify.c", "src/identify/axis-identify-ids.c", "src/identify/axis-identify-rules.c",
		"src/identify/axis-identify-run.c", "src/identify/axis-identify-store.c",
		"src/modules/movies/axis-movies-identify.c", "src/web/axis-web-identify.c",
		"src/web/axis-web-identify-pages.c", NULL
	};
	guint i;
	guint k;

	for (i = 0; files[i] != NULL; i++) {
		g_autofree gchar *text = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_true(g_file_get_contents(files[i], &text, NULL, &error));
		g_assert_no_error(error);
		for (k = 0; forbidden[k] != NULL; k++) {
			const gchar *at = text;

			/* As a word of its own, not a member (owner_unlink() and
			 * owner->unlink() are the owner's links in the database). */
			while ((at = strstr(at, forbidden[k])) != NULL) {
				if (at == text || !(g_ascii_isalnum(at[-1]) || at[-1] == '_' || at[-1] == '>' || at[-1] == '.'))
					g_error("%s uses %s", files[i], forbidden[k]);
				at++;
			}
		}
	}
	/* The one file it opens, it opens to read, through the guard. */
	{
		g_autofree gchar *run = NULL;

		g_assert_true(g_file_get_contents("src/identify/axis-identify-run.c", &run, NULL, NULL));
		g_assert_nonnull(strstr(run, "AXIS_VOLUME_INTENT_READ"));
		g_assert_nonnull(strstr(run, "axis_storage_open_file_beneath"));
	}
}

/* ── A series' folders and episode files ────────────────────────── */

static void
test_season_folders(void)
{
	static const struct
	{
		const gchar *name;
		gint         season;
	} cases[] = {
		{ "Season 1", 1 }, { "Season 01", 1 }, { "season 12", 12 }, { "Season1", 1 }, { "SEASON 3", 3 },
		{ "S01", 1 }, { "s2", 2 }, { "Series 4", 4 }, { "Specials", 0 }, { "special", 0 }, { "Season 00", 0 },
		{ " Season 2 ", 2 }, { "Season 2024", 2024 },
		/* Not a season folder: the file's own season stands. */
		{ "Season", -1 }, { "Seasons", -1 }, { "Season 1 (2010)", -1 }, { "S01E01", -1 }, { "Series", -1 },
		{ "Seriesfour", -1 }, { "Extras", -1 }, { "", -1 }, { "Season 123456", -1 }, { "Season -1", -1 },
		{ NULL, 0 }
	};
	guint i;

	for (i = 0; cases[i].name != NULL; i++) {
		gint got = axis_identify_season_folder(cases[i].name);

		if (got != cases[i].season)
			g_error("\"%s\": expected season %d, got %d", cases[i].name, cases[i].season, got);
	}
	g_assert_cmpint(axis_identify_season_folder(NULL), ==, -1);
}

/* What @file's name holds in a season folder of @folder_season: "code" or
 * the words of what it holds. */
static gchar *
holds_of(
	const gchar *file,
	gint         folder_season
){
	g_autoptr(AxisIdentifyName) name = axis_identify_name_read(file, 2026);
	AxisIdentifyEpisodes out;
	gchar *text;

	axis_identify_episodes_read(name->release, folder_season, &out);
	text = out.code != NULL ? g_strdup_printf("%s: %s", out.code, out.text) : g_strdup(out.text);
	axis_identify_episodes_clear(&out);
	return text;
}

static void
assert_holds(
	const gchar *file,
	gint         folder_season,
	const gchar *expected
){
	g_autofree gchar *got = holds_of(file, folder_season);

	if (!g_str_has_prefix(got, expected))
		g_error("%s (season folder %d): expected '%s', got '%s'", file, folder_season, expected, got);
}

/*
 * Spec (tv): Episode files are placed by what their names say.  One
 * episode, a double one written either way, 1x02, a range, a special, a
 * daily show's date; held, with the reason: no number, an absolute number
 * alone, a whole season, episodes across seasons, a season other than the
 * season folder's.
 */
static void
test_episode_files(void)
{
	AxisIdentifyEpisodes out;
	g_autoptr(AxisIdentifyName) double_name = NULL;

	assert_holds("Lantern Row (2015) - S01E02 - Chapter 2 [WEBDL-1080p].mkv", 1, "S01E02");
	assert_holds("Lantern Row (2015) - S01E02 - Chapter 2 [WEBDL-1080p].mkv", -1, "S01E02");
	assert_holds("Lantern.Row.S01E02E03.720p.HDTV.x264-GRP.mkv", 1, "S01E02-E03");
	assert_holds("Lantern.Row.S01E02-E04.720p.HDTV.x264-GRP.mkv", 1, "S01E02-E04");
	assert_holds("Lantern.Row.S01E02E05.720p.HDTV.x264-GRP.mkv", 1, "S01E02E05");
	assert_holds("Lantern Row - 1x02.mkv", 1, "S01E02");
	assert_holds("Lantern Row (2015) - S00E01 - Winter Special.mkv", 0, "S00E01");
	assert_holds("The.Night.Ledger.2024.03.11.Guest.Name.720p.WEB.h264-GRP.mkv", -1, "dated 2024-03-11");
	/* Held. */
	assert_holds("Lantern Row (2015) - Pilot Party.mkv", 1, "no_episode: its name gives no season and episode");
	assert_holds("01.mkv", 1, "no_episode");
	assert_holds("Glass Harbor - 05 [1080p].mkv", -1, "absolute_only: its name gives only an absolute episode number "
		"(5)");
	assert_holds("Lantern.Row.S02.1080p.BluRay.x264-GRP.mkv", 2, "season_pack: its name says it is the whole of season "
		"2");
	assert_holds("Lantern.Row.S01-S03.1080p.BluRay.x264-GRP.mkv", -1, "season_pack: its name says it holds seasons 1 "
		"to 3");
	assert_holds("Lantern.Row.Complete.Series.1080p.BluRay.x264-GRP.mkv", -1, "season_pack: its name says it is the "
		"complete series");
	assert_holds("Lantern Row (2015) - S02E01 - Misfiled.mkv", 1, "season_folder: its name says season 2, but it is in "
		"the folder of season 1");
	assert_holds("Lantern Row (2015) - S02E01 - Misfiled.mkv", 0, "season_folder: its name says season 2, but it is in "
		"the specials' folder");

	/* The numbers themselves, each once. */
	double_name = axis_identify_name_read("Lantern.Row.S01E02E02E03.720p.HDTV.x264-GRP.mkv", 2026);
	axis_identify_episodes_read(double_name->release, -1, &out);
	g_assert_null(out.code);
	g_assert_cmpint(out.season, ==, 1);
	g_assert_cmpuint(out.n, ==, 2);
	g_assert_cmpint(out.numbers[0], ==, 2);
	g_assert_cmpint(out.numbers[1], ==, 3);
	g_assert_cmpstr(out.date, ==, "");
	axis_identify_episodes_clear(&out);
}

/* The name a series' folder is read with: the title most episode files
 * share, never a file that is no episode. */
static void
test_series_name(void)
{
	static const gchar *const files[] = {
		"Lantern Row - Pilot Party.mkv",
		"Quiet Lake - S01E03.mkv",
		"Lantern Row (2015) - S01E01 - Chapter 1.mkv",
		"S01E04.mkv",
		"Lantern Row (2015) - S01E02 - Chapter 2.mkv",
		NULL
	};
	AxisIdentifyName *names[G_N_ELEMENTS(files)];
	guint n = 0;
	guint i;

	for (i = 0; files[i] != NULL; i++)
		names[n++] = axis_identify_name_read(files[i], 2026);
	g_assert_cmpint(axis_identify_series_name((const AxisIdentifyName *const *)names, n), ==, 2);
	g_assert_cmpint(axis_identify_series_name((const AxisIdentifyName *const *)names, 1), ==, -1);
	g_assert_cmpint(axis_identify_series_name((const AxisIdentifyName *const *)names + 3, 1), ==, -1);
	for (i = 0; i < n; i++)
		axis_identify_name_free(names[i]);
}

/* The rules are the films'; a series' reasons name a series, and its runtime
 * is the episode's. */
static void
test_series_decide(void)
{
	g_autoptr(AxisMetaTitle) row = axis_meta_title_new(AXIS_META_KIND_SERIES);
	g_autoptr(AxisMetaTitle) other = axis_meta_title_new(AXIS_META_KIND_SERIES);
	g_autoptr(AxisIdentifyVerdict) v = NULL;
	const AxisMetaTitle *found[2];
	Case c;

	row->id = 1;
	row->title = g_strdup("Copper Valley");
	row->year = 2019;
	row->runtime = 0;
	other->id = 2;
	other->title = g_strdup("Copper Valley");
	other->year = 2004;
	found[0] = row;
	found[1] = other;

	/* With its year and the episode's runtime: sure. */
	case_init(&c, "Copper Valley (2019)", "Copper.Valley.S01E01.720p.HDTV.x264-GRP.mkv");
	c.e.kind = AXIS_META_KIND_SERIES;
	c.e.found = found;
	c.e.n_found = 2;
	c.e.duration_ms = 30 * 60000;
	c.e.episode_runtime = 31;
	v = axis_identify_decide(&c.e);
	g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	g_assert_true(v->title == row);
	g_assert_true(has_reason(v, "runtime_ok"));
	g_clear_pointer(&v, axis_identify_verdict_free);
	/* No runtime known for the series' episodes: likely, in a series' words. */
	c.e.episode_runtime = 0;
	v = axis_identify_decide(&c.e);
	g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	g_assert_true(has_reason(v, "no_runtime"));
	g_clear_pointer(&v, axis_identify_verdict_free);
	case_clear(&c);

	/* No year: two series of the name, held. */
	case_init(&c, "Copper Valley", "Copper.Valley.S01E01.720p.HDTV.x264-GRP.mkv");
	c.e.kind = AXIS_META_KIND_SERIES;
	c.e.found = found;
	c.e.n_found = 2;
	c.e.duration_ms = 30 * 60000;
	c.e.episode_runtime = 30;
	v = axis_identify_decide(&c.e);
	g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	g_assert_true(has_reason(v, "ambiguous"));
	g_assert_nonnull(strstr(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, 0))->text, "2 series fit"));
	case_clear(&c);
}

/* ── A series' folder with no year: its episodes ────────────────── */

/* A series' full record: seasons 1 to @n with @counts episodes each (-1:
 * not given), its episodes running @runtime minutes (0: not given). */
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
	t->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	for (i = 0; i < n; i++) {
		AxisMetaSeason *s = axis_meta_season_new((gint)i + 1);

		s->episode_count = counts[i];
		g_ptr_array_add(t->seasons, s);
	}
	return t;
}

/* What @files hold (each "S01E02" or a name), read in season folders of
 * their own seasons. */
static void
shape_of(
	const gchar *const *files,
	AxisIdentifyShape  *shape
){
	guint i;

	axis_identify_shape_init(shape);
	for (i = 0; files[i] != NULL; i++) {
		g_autoptr(AxisIdentifyName) name = axis_identify_name_read(files[i], 2026);
		AxisIdentifyEpisodes episodes;

		axis_identify_episodes_read(name->release, -1, &episodes);
		axis_identify_shape_add(shape, &episodes);
		axis_identify_episodes_clear(&episodes);
	}
	axis_identify_shape_finish(shape);
}

static void
test_shape(void)
{
	static const gchar *const files[] = {
		"Lantern Row - S01E01.mkv", "Lantern Row - S01E02.mkv", "Lantern Row - S01E02 [HDTV-720p].mkv",
		"Lantern Row - S02E01E02.mkv", "Lantern Row - S00E01 - Winter Special.mkv",
		"The.Night.Ledger.2024.03.11.720p.WEB.h264-GRP.mkv", "Lantern Row - Pilot Party.mkv", NULL
	};
	g_auto(AxisIdentifyShape) shape = { 0 };
	g_autofree gchar *words = NULL;

	shape_of(files, &shape);
	g_assert_cmpuint(shape.n_files, ==, 7);
	g_assert_cmpuint(shape.n_numbered, ==, 4);
	g_assert_cmpuint(shape.n_specials, ==, 1);
	g_assert_cmpuint(shape.n_dated, ==, 1);
	g_assert_cmpuint(shape.n_none, ==, 1);
	/* A copy counts once; a double episode twice. */
	g_assert_cmpuint(shape.episodes->len, ==, 4);
	words = axis_identify_shape_describe(&shape);
	g_assert_cmpstr(words, ==, "4 numbered episodes of seasons 1 to 2; 1 more file is specials; 1 file is dated, as a "
		"daily show's; 1 file holds no episode");
}

/*
 * The thresholds: three numbered episodes are the least checked; at least
 * nine in ten must be the series' (below ten, every one); a season the
 * series does not have rules it out; a season of unknown size is not held
 * against it, nor counted for it; specials count neither way.
 */
static void
test_shape_fit(void)
{
	static const gint counts[] = { 10, 10, -1 };
	g_autoptr(AxisMetaTitle) row = series_record(1, "Lantern Row", 2015, 45, counts, G_N_ELEMENTS(counts));
	static const gchar *const three[] = { "S01E01", "S01E02", "S01E03", "S00E04", NULL };
	static const gchar *const two[] = { "S01E01", "S01E02", "S00E01", "S00E02", NULL };
	static const gchar *const ten_one_off[] = { "S01E01", "S01E02", "S01E03", "S01E04", "S01E05", "S01E06", "S01E07",
		"S01E08", "S01E09", "S01E11", NULL };
	static const gchar *const nine_one_off[] = { "S01E01", "S01E02", "S01E03", "S01E04", "S01E05", "S01E06",
		"S01E07", "S01E08", "S01E11", NULL };
	static const gchar *const other_season[] = { "S01E01", "S01E02", "S01E03", "S04E01", NULL };
	static const gchar *const unknown_size[] = { "S01E01", "S03E01", "S03E02", "S03E03", NULL };
	AxisIdentifyFit fit;

	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(three, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_true(fit.fits);
		g_assert_cmpuint(fit.checked, ==, 3);
	}
	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(two, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_false(fit.fits);
		g_assert_cmpuint(fit.checked, ==, 2);
	}
	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(ten_one_off, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_true(fit.fits);
		g_assert_cmpuint(fit.landed, ==, 9);
		g_assert_cmpint(fit.off_season, ==, 1);
		g_assert_cmpint(fit.off_number, ==, 11);
	}
	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(nine_one_off, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_false(fit.fits);
	}
	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(other_season, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_false(fit.fits);
		g_assert_cmpuint(fit.missing, ==, 1);
		g_assert_cmpint(fit.missing_season, ==, 4);
	}
	{
		g_auto(AxisIdentifyShape) shape = { 0 };

		shape_of(unknown_size, &shape);
		axis_identify_shape_fit(&shape, row, &fit);
		g_assert_false(fit.fits);
		g_assert_cmpuint(fit.unchecked, ==, 3);
		g_assert_cmpuint(fit.checked, ==, 1);
	}
	/* A record with no seasons checks nothing. */
	{
		g_auto(AxisIdentifyShape) shape = { 0 };
		g_autoptr(AxisMetaTitle) bare = axis_meta_title_new(AXIS_META_KIND_SERIES);

		shape_of(three, &shape);
		axis_identify_shape_fit(&shape, bare, &fit);
		g_assert_false(fit.listed);
		g_assert_false(fit.fits);
	}
}

typedef struct
{
	AxisIdentifyName            *folder;
	AxisIdentifyName            *file;
	AxisIdentifyShape            shape;
	AxisIdentifySeriesCandidate  series[AXIS_IDENTIFY_SERIES_COMPARED];
	AxisIdentifyEvidence         e;
} SeriesCase;

/* A series' folder @folder holding @files (its title read with theirs as the
 * job reads it), the main video probed at @minutes (0: not probed), with the
 * series of its name @titles (@n of them, each with @runtime as its main
 * episode's runtime). */
static void
series_case_init(
	SeriesCase                 *c,
	const gchar                *folder,
	const gchar *const         *files,
	gint                        minutes,
	const AxisMetaTitle *const *titles,
	guint                       n
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_name_free);
	gint named;
	guint i;

	memset(c, 0, sizeof(*c));
	c->folder = read_name(folder);
	/* The file its title is read from, as the job picks it. */
	for (i = 0; files[i] != NULL; i++)
		g_ptr_array_add(names, read_name(files[i]));
	named = axis_identify_series_name((const AxisIdentifyName *const *)names->pdata, names->len);
	c->file = named >= 0 ? read_name(files[named]) : NULL;
	axis_identify_read(c->folder, c->file, &c->e.reading);
	shape_of(files, &c->shape);
	c->e.kind = AXIS_META_KIND_SERIES;
	c->e.shape = &c->shape;
	c->e.percent = AXIS_IDENTIFY_DEFAULT_PERCENT;
	c->e.minutes = AXIS_IDENTIFY_DEFAULT_MINUTES;
	c->e.duration_ms = (gint64)minutes * 60000;
	for (i = 0; i < n && i < AXIS_IDENTIFY_SERIES_COMPARED; i++) {
		c->series[i].title = titles[i];
		c->series[i].episode_runtime = titles[i]->runtime;
	}
	c->e.series = c->series;
	c->e.n_series = MIN(n, AXIS_IDENTIFY_SERIES_COMPARED);
	c->e.found = titles;
	c->e.n_found = n;
}

static void
series_case_clear(SeriesCase *c)
{
	axis_identify_name_free(c->folder);
	axis_identify_name_free(c->file);
	axis_identify_shape_clear(&c->shape);
}

static gchar *
reason_text(
	const AxisIdentifyVerdict *v,
	const gchar               *code
){
	guint i;

	for (i = 0; i < v->reasons->len; i++) {
		const AxisIdentifyReason *r = g_ptr_array_index(v->reasons, i);

		if (g_strcmp0(r->code, code) == 0)
			return g_strdup(r->text);
	}
	return NULL;
}

/*
 * Spec (library-identification): A series' folder without a year is linked
 * by its episodes.  One series of the name whose seasons the files fit is
 * sure; a season it does not have, a runtime that contradicts, too few
 * numbered episodes, or a generic folder name hold it; of two of one name,
 * the one the files fit is sure, and two that fit are held; a name that is
 * one of a series' other names counts as its own; a series with no runtime
 * is checked by its episodes alone.
 */
static void
test_series_without_year(void)
{
	static const gint lantern_counts[] = { 6, 6 };
	static const gint brass_old_counts[] = { 10, 10 };
	static const gint brass_new_counts[] = { 8 };
	static const gint short_counts[] = { 2 };
	static const gchar *const lantern_files[] = { "Lantern Row - S01E01.mkv", "Lantern Row - S01E02.mkv",
		"Lantern Row - S02E01.mkv", "Lantern Row - S02E02.mkv", NULL };
	static const gchar *const third_season[] = { "Lantern Row - S01E01.mkv", "Lantern Row - S01E02.mkv",
		"Lantern Row - S03E01.mkv", NULL };
	static const gchar *const unnumbered[] = { "Lantern Row - Pilot Party.mkv", "Lantern Row - Holiday.mkv",
		"Lantern Row - Reunion.mkv", NULL };
	static const gchar *const two_files[] = { "Lantern Row - S01E01.mkv", "Lantern Row - S01E02E03.mkv", NULL };
	static const gchar *const brass_files[] = { "Brass Meridian - S01E01.mkv", "Brass Meridian - S01E02.mkv",
		"Brass Meridian - S01E03.mkv", "Brass Meridian - S01E04.mkv", "Brass Meridian - S01E05.mkv", NULL };
	static const gchar *const brass_late[] = { "Brass Meridian - S01E07.mkv", "Brass Meridian - S01E08.mkv",
		"Brass Meridian - S01E09.mkv", "Brass Meridian - S02E01.mkv", NULL };
	static const gchar *const bare_files[] = { "S01E01.mkv", "S01E02.mkv", "S01E03.mkv", NULL };
	g_autoptr(AxisMetaTitle) lantern = series_record(1, "Lantern Row", 2015, 45, lantern_counts, 2);
	g_autoptr(AxisMetaTitle) brass_old = series_record(2, "Brass Meridian", 2008, 45, brass_old_counts, 2);
	g_autoptr(AxisMetaTitle) brass_new = series_record(3, "Brass Meridian", 2021, 45, brass_new_counts, 1);
	g_autoptr(AxisMetaTitle) brass_short = series_record(4, "Brass Meridian", 1996, 25, short_counts, 1);
	g_autoptr(AxisMetaTitle) kestrel = series_record(5, "Kestrel Point", 2016, 50, lantern_counts, 1);
	g_autoptr(AxisMetaTitle) juniper = series_record(6, "Lantern Row", 2012, 0, lantern_counts, 2);
	const AxisMetaTitle *titles[AXIS_IDENTIFY_SERIES_COMPARED];
	SeriesCase c;

	/* One series of the name, the files fit, the runtime agrees: sure. */
	titles[0] = lantern;
	series_case_init(&c, "Lantern Row", lantern_files, 44, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *fits = reason_text(v, "episodes_fit");
		g_autofree gchar *files = reason_text(v, "files");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == lantern);
		g_assert_true(has_reason(v, "series_fits"));
		g_assert_false(has_reason(v, "no_year"));
		g_assert_cmpstr(fits, ==, "Lantern Row (2015) fits: 4 of the 4 numbered episodes on disk are its episodes; "
			"the main video runs 44 minutes against 45 for the episode");
		g_assert_cmpstr(files, ==, "its files hold 4 numbered episodes of seasons 1 to 2");
		/* The reason "files" comes last, always. */
		g_assert_cmpstr(((AxisIdentifyReason *)g_ptr_array_index(v->reasons, v->reasons->len - 1))->code, ==,
			"files");
	}
	/* Not probed yet: held until it is. */
	c.e.duration_ms = 0;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "no_probe"));
	}
	/* The runtime contradicts: held. */
	c.e.duration_ms = 90 * 60000;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *off = reason_text(v, "runtime_off");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_cmpstr(off, ==, "the episodes on disk fit Lantern Row (2015), but its episode runs 45 minutes and "
			"the main video 90: 45 minutes off");
		g_assert_true(has_reason(v, "no_fit"));
		g_assert_true(v->title == lantern);
	}
	series_case_clear(&c);

	/* A season the series does not have: held. */
	series_case_init(&c, "Lantern Row", third_season, 45, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *missing = reason_text(v, "season_missing");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_cmpstr(missing, ==, "Lantern Row (2015) has no season 3, and 1 of the episodes on disk is in one it "
			"does not have");
	}
	series_case_clear(&c);

	/* No numbered episode, and two files with three episodes: too few. */
	series_case_init(&c, "Lantern Row", unnumbered, 45, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *few = reason_text(v, "few_episodes");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_cmpstr(few, ==, "no file on disk is a numbered episode, and at least 3, in as many files, are needed "
			"for that check");
		g_assert_true(v->title == lantern);
	}
	series_case_clear(&c);
	series_case_init(&c, "Lantern Row", two_files, 45, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "few_episodes"));
	}
	series_case_clear(&c);

	/* Files that give no title of their own, in a folder that does: sure. */
	series_case_init(&c, "Lantern Row", bare_files, 45, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(c.e.reading.kind, ==, AXIS_IDENTIFY_READ_FOLDER);
		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
	}
	series_case_clear(&c);

	/* A folder whose own name reads as nothing: held. */
	series_case_init(&c, "Season 1", lantern_files, 45, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
	}
	series_case_clear(&c);

	/* Two of one name: the one the files fit (the other's first season has
	 * two episodes, and its episodes run 25 minutes). */
	titles[0] = brass_short;
	titles[1] = brass_old;
	series_case_init(&c, "Brass Meridian", brass_files, 45, titles, 2);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == brass_old);
		g_assert_true(g_ptr_array_index(v->candidates, 0) == brass_old);
		g_assert_true(has_reason(v, "episodes_off"));
	}
	series_case_clear(&c);

	/* Two of one name that both fit: held, never chosen between. */
	titles[0] = brass_old;
	titles[1] = brass_new;
	series_case_init(&c, "Brass Meridian", brass_files, 45, titles, 2);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *both = reason_text(v, "series_ambiguous");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_cmpstr(both, ==, "2 series of that name fit the episodes on disk (Brass Meridian (2008) and Brass "
			"Meridian (2021)), so neither is taken");
		g_assert_cmpuint(v->candidates->len, ==, 2);
	}
	series_case_clear(&c);

	/* The same two, and a second season the newer one does not have: the
	 * older, by its seasons alone. */
	series_case_init(&c, "Brass Meridian", brass_late, 45, titles, 2);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(v->title == brass_old);
		g_assert_true(has_reason(v, "season_missing"));
	}
	/* More of the name than were compared: held. */
	c.e.series_more = TRUE;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_LIKELY);
		g_assert_true(has_reason(v, "more_series"));
	}
	series_case_clear(&c);

	/* One of its other names: sure, and said. */
	titles[0] = kestrel;
	{
		static const gchar *const kestrel_files[] = { "Kestrel Bay - S01E01.mkv", "Kestrel Bay - S01E02.mkv",
			"Kestrel Bay - S01E03.mkv", NULL };
		g_autofree gchar *names = NULL;

		series_case_init(&c, "Kestrel Bay", kestrel_files, 50, titles, 1);
		c.series[0].other_name = TRUE;
		{
			g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

			names = reason_text(v, "names");
			g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
			g_assert_true(v->title == kestrel);
			g_assert_nonnull(strstr(names, "it is one of its other names"));
		}
		series_case_clear(&c);
	}

	/* No runtime given: by the episodes alone, and said. */
	titles[0] = juniper;
	series_case_init(&c, "Lantern Row", lantern_files, 44, titles, 1);
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);
		g_autofree gchar *fits = reason_text(v, "episodes_fit");

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_nonnull(strstr(fits, "the details provider gives no runtime for its episodes"));
	}
	series_case_clear(&c);

	/* A year in the name: the films' rule, as before. */
	titles[0] = lantern;
	series_case_init(&c, "Lantern Row (2015)", lantern_files, 45, titles, 1);
	c.e.series = NULL;
	c.e.n_series = 0;
	c.e.detail = lantern;
	c.e.episode_runtime = 45;
	{
		g_autoptr(AxisIdentifyVerdict) v = axis_identify_decide(&c.e);

		g_assert_cmpint(v->confidence, ==, AXIS_IDENTIFY_SURE);
		g_assert_true(has_reason(v, "runtime_ok"));
		g_assert_false(has_reason(v, "series_fits"));
		g_assert_true(has_reason(v, "files"));
	}
	series_case_clear(&c);
}

/* Every held unit is told what would settle it, by its reasons. */
static void
test_settle(void)
{
	static const struct
	{
		AxisMetaKind  kind;
		const gchar  *code;
		const gchar  *start;
	} cases[] = {
		{ AXIS_META_KIND_SERIES, "series_ambiguous", "Several series have this name" },
		{ AXIS_META_KIND_SERIES, "few_episodes", "Confirm the series here if it is the right one. Axis links it by "
		  "itself once at least 3" },
		{ AXIS_META_KIND_SERIES, "season_missing", "Its episode files do not match the series' seasons" },
		{ AXIS_META_KIND_SERIES, "runtime_off", "Its files run longer or shorter" },
		{ AXIS_META_KIND_SERIES, "no_probe", "Axis looks at it again by itself once its files are read" },
		{ AXIS_META_KIND_SERIES, "no_match", "Search for the series under another name" },
		{ AXIS_META_KIND_FILM, "no_year", "Confirm the movie here if it is the right one, or add its year" },
		{ AXIS_META_KIND_FILM, "ambiguous", "Several movies have this name" },
		{ AXIS_META_KIND_FILM, "id_conflict", "The identifier in its name does not settle it" },
		{ AXIS_META_KIND_FILM, "names", "Confirm the best candidate here" },
		{ 0, NULL, NULL }
	};
	guint i;

	for (i = 0; cases[i].code != NULL; i++) {
		g_autoptr(GPtrArray) reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_reason_free);
		AxisIdentifyReason *r = g_new0(AxisIdentifyReason, 1);
		const gchar *text;

		r->code = g_strdup(cases[i].code);
		r->text = g_strdup("why");
		g_ptr_array_add(reasons, r);
		text = axis_identify_settle_text(cases[i].kind, reasons);
		if (!g_str_has_prefix(text, cases[i].start))
			g_error("%s: '%s'", cases[i].code, text);
	}
	g_assert_nonnull(axis_identify_settle_text(AXIS_META_KIND_SERIES, NULL));
	/* A runtime that contradicts says so, though nothing fits either. */
	{
		g_autoptr(GPtrArray) reasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_reason_free);
		const gchar *const codes[] = { "runtime_off", "no_fit" };

		for (i = 0; i < G_N_ELEMENTS(codes); i++) {
			AxisIdentifyReason *r = g_new0(AxisIdentifyReason, 1);

			r->code = g_strdup(codes[i]);
			r->text = g_strdup("why");
			g_ptr_array_add(reasons, r);
		}
		g_assert_true(g_str_has_prefix(axis_identify_settle_text(AXIS_META_KIND_SERIES, reasons),
			"Its files run longer or shorter"));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/identify/unit/name-ids", test_name_ids);
	g_test_add_func("/identify/unit/nfo-scan", test_nfo_scan);
	g_test_add_func("/identify/unit/names", test_names);
	g_test_add_func("/identify/unit/names-any-title", test_names_any_title);
	g_test_add_func("/identify/unit/groups", test_groups);
	g_test_add_func("/identify/unit/readings", test_readings);
	g_test_add_func("/identify/unit/decide-identifiers", test_decide_identifiers);
	g_test_add_func("/identify/unit/decide-names", test_decide_names);
	g_test_add_func("/identify/unit/same-name-titles", test_same_name_titles);
	g_test_add_func("/identify/unit/year-by-full-record", test_year_by_full_record);
	g_test_add_func("/identify/unit/two-years-off", test_two_years_off);
	g_test_add_func("/identify/unit/series-like", test_series_like);
	g_test_add_func("/identify/unit/one-name-and-wider-band", test_one_name_and_wider_band);
	g_test_add_func("/identify/unit/waiting-and-cut", test_waiting_and_cut);
	g_test_add_func("/identify/unit/named-order", test_named_order);
	g_test_add_func("/identify/unit/review-rules", test_review_rules);
	g_test_add_func("/identify/unit/runtime", test_runtime);
	g_test_add_func("/identify/unit/file-quality", test_file_quality);
	g_test_add_func("/identify/unit/unknown-source", test_unknown_source);
	g_test_add_func("/identify/unit/read-only-source", test_read_only_source);
	g_test_add_func("/identify/unit/season-folders", test_season_folders);
	g_test_add_func("/identify/unit/episode-files", test_episode_files);
	g_test_add_func("/identify/unit/series-name", test_series_name);
	g_test_add_func("/identify/unit/series-decide", test_series_decide);
	g_test_add_func("/identify/unit/shape", test_shape);
	g_test_add_func("/identify/unit/shape-fit", test_shape_fit);
	g_test_add_func("/identify/unit/series-without-year", test_series_without_year);
	g_test_add_func("/identify/unit/settle", test_settle);
	return g_test_run();
}
