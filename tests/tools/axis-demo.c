/*
 * axis-demo.c - a demo instance of Axis filled with made-up titles, for
 * looking at the pages and taking their screenshots
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   tests/with-postgres.sh build/<cc>-<type>/tests/tools/axis-demo --port 2NNNN \
 *       [--scene full|empty|setup] [--ready-file FILE] [--pages-file FILE]
 *       [--attention N]
 *
 * --attention N adds N made-up notices to the page "Needs attention" (and to
 * the bell's count in the header), beside the scene's own: 3 or 140 to look
 * at the header with a count and with "99+".
 *
 * tests/tools/axis-demo-shots.sh runs it and takes the screenshots of the
 * pages it lists in --pages-file (docs/SCREENSHOTS.md says how).
 *
 * Run from the top of the repository (the fake probe and the fixtures are
 * read from there).  It makes a database of its own on the server
 * tests/with-postgres.sh starts, disks and library folders in a scratch
 * directory, and serves the real pages on 127.0.0.1 at the port given, with
 * every module the daemon has.  What would be outside Axis is a stand-in
 * on loopback: TMDb (answered from the catalogue below, with posters drawn
 * here as flat colours with the title on them), Prowlarr and the download
 * clients (the tests' doubles), and ffprobe (tests/fixtures/import's fake,
 * which reads a file's first line).  Every title, overview, name and path
 * is made up; nothing is fetched from the internet.
 *
 * The scenes:
 *   full   two accounts (an administrator and a viewer), three disks (one
 *          offline), movies and series in every state there is to show,
 *          downloads, imports, held folders to identify, Discover's lists,
 *          jobs and history
 *   empty  the two accounts and one disk, nothing else set up
 *   setup  no account at all: the setup page
 *
 * Once it serves it prints the address and the accounts (and writes them to
 * --ready-file as "key=value" lines); it stops on SIGINT or SIGTERM and
 * drops its database and scratch directory.  The job queue does not run:
 * what is shown stays as it was made (a search asked for from a page stays
 * queued).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-plex.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "acquire/axis-acquire.h"
#include "api/axis-worker-api.h"
#include "api/axis-worker-tokens.h"
#include "compat/axis-compat.h"
#include "core/axis-data-dir.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-settings.h"
#include "discover/axis-discover.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "jobs/axis-history.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "meta/axis-meta-tmdb-parse.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage.h"
#include "transcode/axis-conversion.h"
#include "transcode/axis-estimator.h"
#include "web/axis-password.h"
#include "web/axis-user-store.h"
#include "web/axis-web.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-compat.h"
#include "web/axis-web-conversions.h"
#include "web/axis-web-discover.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-library.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-profiles.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-ready.h"
#include "web/axis-web-savings.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-setup.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-workers.h"

/* The accounts (made up, for a demo on loopback only). */
#define ADMIN_NAME      "admin"
#define ADMIN_PASSWORD  "demo-admin-password"
#define VIEWER_NAME     "viewer"
#define VIEWER_PASSWORD "demo-viewer-password"

/* What the stand-ins take: no real key of any service. */
#define TMDB_TOKEN      "eyJdemo.made-up-token-for-the-demo.not-a-key-4c1d"
#define PROWLARR_KEY    "demo-prowlarr-key-5a7e3c19"
#define CLIENT_PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

#define FAKE_PROBE      "tests/fixtures/import/fake-ffprobe.sh"

/* A film's file as the fake probe reads it: about two hours. */
#define FILM_LINE "AXISFAKE duration=6900 width=1920 height=800 vcodec=h264 acodec=eac3 channels=6"
#define EPISODE_LINE "AXISFAKE duration=2700 width=1920 height=1080 vcodec=hevc acodec=eac3 channels=6"

/* ── The catalogue ──────────────────────────────────────────────── */

/* The lists a title is on (Discover). */
#define LIST_POPULAR   (1u << 0)
#define LIST_TRENDING  (1u << 1)
#define LIST_NOW       (1u << 2)
#define LIST_UPCOMING  (1u << 3)
#define LIST_TOP       (1u << 4)

typedef struct
{
	gint         id;
	const gchar *title;
	const gchar *date;	/* first release, "YYYY-MM-DD"; NULL: @ahead days from today */
	gint         ahead;
	gint         runtime;
	const gchar *genres;	/* TMDb's numbers, "18,9648" */
	gdouble      rating;
	guint        lists;
	const gchar *overview;
} DemoFilm;

static const DemoFilm films[] = {
	{ 1001, "The Lantern Keepers", "2023-04-14", 0, 118, "18,9648", 7.4, LIST_POPULAR | LIST_TOP,
	  "Two sisters inherit a lighthouse on a coast where the lamps have not been lit in forty years, and learn why the "
	  "village wants them dark." },
	{ 1002, "Tidewater Hollow", "2020-09-18", 0, 104, "18,53", 6.9, LIST_TOP,
	  "A ferry pilot takes one last crossing in a storm and comes back with a passenger nobody saw board." },
	{ 1003, "Wren Crossing", "2018-05-04", 0, 97, "35,18", 7.1, LIST_POPULAR,
	  "A small-town postmaster delivers letters that were never sent, one each morning, to the people they were "
	  "written for." },
	{ 1004, "Ember Quay", "2012-11-02", 0, 126, "28,53", 6.6, 0,
	  "Dock workers on a burning waterfront hold the line for one night while the city decides whether to save them." },
	{ 1005, "The Brass Orchard", "2024-03-08", 0, 111, "18,9648", 7.3, LIST_POPULAR | LIST_TRENDING,
	  "A clockmaker's apprentice inherits an orchard where the trees keep time." },
	{ 1006, "Salt and Cinder", "2021-06-25", 0, 102, "10749,18", 6.8, LIST_TRENDING,
	  "A glassblower and a lifeguard spend one summer arguing about the sea." },
	{ 1007, "A Map of Small Rivers", "2019-02-15", 0, 93, "99", 7.8, LIST_TOP,
	  "A year spent walking every stream that feeds one river, from the hills to the estuary." },
	{ 1008, "Northbound Static", "2022-10-07", 0, 109, "878,53", 6.5, LIST_POPULAR,
	  "A night-shift radio host hears her own voice reading tomorrow's news." },
	{ 1009, "The Glasshouse Year", "2017-08-11", 0, 121, "18", 7.0, 0,
	  "Three generations of a family of gardeners spend a year under one leaking roof of glass." },
	{ 1010, "Paper Moons Over Ostend", "2015-04-03", 0, 99, "35,10749", 6.4, 0,
	  "A forger of travel posters falls for the inspector sent to catch him." },
	{ 1011, "The Ninth Lighthouse", "2016-01-22", 0, 114, "12,18", 6.7, 0,
	  "A cartographer sets out to visit every lighthouse on a coast and finds one more than the charts show." },
	{ 1012, "Copper Sky Motel", "2023-07-21", 0, 101, "53,9648", 6.2, LIST_TRENDING,
	  "Eleven guests, one storm, and a night manager who knows more than he says." },
	{ 1013, "The Quiet Engine", "2014-03-14", 0, 116, "878,18", 7.2, 0,
	  "An engineer builds a train that makes no sound, and the town it runs through starts to forget it is there." },
	{ 1014, "Harrow Lane", "2011-10-28", 0, 95, "27,53", 5.9, 0,
	  "The last house on a closed road keeps its porch light on for someone." },
	{ 1015, "Under the Kestrel Moon", "2020-12-04", 0, 107, "16,12", 7.6, 0,
	  "A young falconer and an old bird cross a valley in winter." },
	{ 1016, "The Folding City", NULL, 75, 0, "878,12", 0.0, LIST_UPCOMING,
	  "Architects of a city that rearranges itself every night race to finish the last district." },
	{ 1017, "Fennimore's Last Ride", "2013-06-07", 0, 103, "37,18", 6.3, 0,
	  "A retired stagecoach driver agrees to one final run across a desert that has since become a motorway." },
	{ 1018, "Saltmarsh Radio", "2019-09-13", 0, 98, "35", 6.9, 0,
	  "The volunteers of a tiny coastal radio station fight to stay on the air." },
	{ 1019, "Orchard of Bells", "2010-05-21", 0, 112, "18,10749", 6.1, 0,
	  "A bell founder's daughter returns home to cast the bell her father never finished." },
	{ 1020, "The Cartographer's Daughter", "2018-03-02", 0, 109, "18,12", 6.8, 0,
	  "A girl inherits her mother's unfinished maps and the debts that came with them." },
	{ 1021, "The Cartographer's Daughter", "2018-11-16", 0, 88, "99", 7.0, 0,
	  "A documentary about the women who drew the first survey maps of a northern coast." },
	{ 1022, "Lamplight Assembly", "2022-02-11", 0, 106, "35,18", 7.1, LIST_POPULAR | LIST_NOW,
	  "A school's caretakers stage the end-of-year play when the teachers walk out." },
	{ 1023, "Gale Force Gentle", "2024-10-18", 0, 94, "10749,35", 6.6, LIST_TRENDING | LIST_NOW,
	  "Two rival weather presenters are snowed in at the same mountain station." },
	{ 1024, "Midwinter Signal", NULL, 30, 0, "878,9648", 0.0, LIST_UPCOMING,
	  "A research base at the pole receives a message timed for the longest night of the year." },
	{ 1025, "The Patient Cartwright", "2016-09-09", 0, 131, "18", 7.9, LIST_TOP,
	  "A wheelwright spends a lifetime repairing the same wagon for a family that never pays him." },
	{ 1026, "Dovetail", "2021-03-19", 0, 92, "35", 6.0, LIST_POPULAR,
	  "A carpenter and a carpentry critic share a workshop for a week." },
	{ 1027, "The Weathervane Society", "2025-05-02", 0, 113, "9648,12", 7.2, LIST_TRENDING | LIST_POPULAR,
	  "A club of retired sailors reads the weather from rooftops and solves a disappearance doing it." },
	{ 1028, "Lowlight", "2025-09-26", 0, 99, "53", 6.4, LIST_NOW,
	  "A lighting technician on a night shoot sees something in the dark of every take." },
	{ 1029, "Fogbound", "2009-01-16", 0, 96, "27", 5.7, 0,
	  "A fishing crew is lost in fog for three days that last three weeks ashore." },
	{ 1030, "Fogbound", "2016-11-04", 0, 118, "18", 6.9, 0,
	  "A lighthouse keeper's family waits out a winter of fog on an island with no ferry." },
	{ 1031, "The Iron Tide Almanac", NULL, 140, 0, "12,14", 0.0, LIST_UPCOMING,
	  "An almanac that predicts the tides a year ahead starts to be right about everything else." }
};

typedef struct
{
	gint         id;
	const gchar *title;
	gint         seasons;
	gint         episodes;		/* in each season */
	gint         first_days_ago;	/* when its first episode aired */
	gint         episode_gap;	/* days between episodes */
	gint         season_gap;	/* days between seasons' first episodes */
	gboolean     ended;
	gint         runtime;
	const gchar *genres;
	gdouble      rating;
	guint        lists;
	const gchar *overview;
} DemoSeries;

static const DemoSeries series[] = {
	{ 2001, "Harbour Lights", 3, 8, 760, 7, 365, FALSE, 46, "18,9648", 7.8, LIST_POPULAR | LIST_TOP,
	  "A harbour master and the town's only detective keep the lights of a fishing port burning through three hard "
	  "winters." },
	{ 2002, "Night Ferry", 1, 8, 15, 7, 0, FALSE, 42, "80,18", 7.2, LIST_TRENDING | LIST_NOW,
	  "The last boat out, every night: a crew, a captain and the passengers who should not be aboard." },
	{ 2003, "Quarry Bells", 2, 6, 1500, 7, 365, TRUE, 38, "35", 7.0, LIST_TOP,
	  "A brass band in a quarry town rehearses for the contest that could save the works." },
	{ 2004, "The Long Thaw", 2, 6, 900, 7, 400, TRUE, 52, "10765,18", 6.9, 0,
	  "When a glacier gives up a village it buried a century ago, the villagers start coming home." },
	{ 2005, "Kestrel Point", 1, 10, 200, 7, 0, TRUE, 44, "9648,80", 7.4, LIST_POPULAR,
	  "A coastguard station on a headland, a missing yacht, and ten days of weather." },
	{ 2006, "Static Garden", 2, 6, 1200, 7, 365, TRUE, 30, "35,10765", 6.7, 0,
	  "Two radio engineers tend a garden of antennas on a hill nobody else wants." },
	{ 2007, "Velvet Signal", 1, 8, 60, 7, 0, FALSE, 50, "18,80", 7.5, LIST_TRENDING | LIST_POPULAR,
	  "A late-night jazz station is the cover for a smuggling ring, until the new presenter starts listening." },
	{ 2008, "The Ropewalk", 3, 6, 2000, 7, 365, TRUE, 55, "18", 8.1, LIST_TOP,
	  "Three generations of a family run the longest building in the docks, where rope is made by walking." },
	{ 2009, "Margin of Tides", 1, 6, 40, 7, 0, FALSE, 47, "99", 7.9, LIST_NOW,
	  "A documentary series following one estuary through a year of tides." }
};

/* What an episode is called: picked by its place, made up. */
static const gchar *const episode_names[] = {
	"Low Tide", "The Signal", "Breakwater", "Night Watch", "Salt Lines", "The Crossing", "Fog Bank",
	"Lantern Hour", "Driftwood", "Undertow", "The Keeper", "Slack Water", "Spring Tide", "Harbour Wall",
	"Moorings", "The Long Swim", "Weather Eye", "Last Light", "Dead Reckoning", "Sea Room"
};

static const struct
{
	gint         id;
	const gchar *name;
} movie_genres[] = {
	{ 12, "Adventure" }, { 14, "Fantasy" }, { 16, "Animation" }, { 18, "Drama" }, { 27, "Horror" },
	{ 28, "Action" }, { 35, "Comedy" }, { 37, "Western" }, { 53, "Thriller" }, { 99, "Documentary" },
	{ 878, "Science Fiction" }, { 9648, "Mystery" }, { 10749, "Romance" }
}, tv_genres[] = {
	{ 18, "Drama" }, { 35, "Comedy" }, { 80, "Crime" }, { 99, "Documentary" }, { 9648, "Mystery" },
	{ 10765, "Sci-Fi & Fantasy" }
};

static const DemoFilm *
film_of(gint id)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(films); i++) {
		if (films[i].id == id)
			return &films[i];
	}
	return NULL;
}

static const DemoSeries *
series_of(gint id)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(series); i++) {
		if (series[i].id == id)
			return &series[i];
	}
	return NULL;
}

/* "YYYY-MM-DD", @days from today (UTC; negative: before). */
static gchar *
date_from_today(gint days)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(GDateTime) then = g_date_time_add_days(now, days);

	return g_date_time_format(then, "%Y-%m-%d");
}

static gchar *
film_date(const DemoFilm *film)
{
	return film->date != NULL ? g_strdup(film->date) : date_from_today(film->ahead);
}

static gint
year_of(const gchar *date)
{
	return date != NULL ? (gint)g_ascii_strtoll(date, NULL, 10) : 0;
}

/* Episode @episode (from 1) of season @season (from 1): when it airs. */
static gchar *
episode_date(
	const DemoSeries *show,
	gint              season,
	gint              episode
){
	return date_from_today(-show->first_days_ago + (season - 1) * show->season_gap + (episode - 1) * show->episode_gap);
}

/* ── Posters: flat colours with the title drawn on them ─────────── */

/* A 5 by 7 face for capitals and figures: one byte a row, the low five bits. */
static const guint8 glyphs[36][7] = {
	{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
	{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
	{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
	{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
	{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
	{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
	{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },
	{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
	{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
	{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
	{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
	{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
	{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
	{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
	{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
	{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
	{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
	{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }
};

typedef struct
{
	guint   width;
	guint   height;
	guint8 *rgb;
} Canvas;

static void
canvas_fill(
	Canvas *c,
	guint   x0,
	guint   y0,
	guint   x1,
	guint   y1,
	guint8  r,
	guint8  g,
	guint8  b
){
	guint x;
	guint y;

	x1 = MIN(x1, c->width);
	y1 = MIN(y1, c->height);
	for (y = y0; y < y1; y++) {
		for (x = x0; x < x1; x++) {
			guint8 *p = c->rgb + ((gsize)y * c->width + x) * 3;

			p[0] = r;
			p[1] = g;
			p[2] = b;
		}
	}
}

static gint
glyph_of(gunichar ch)
{
	ch = g_unichar_toupper(ch);
	if (ch >= 'A' && ch <= 'Z')
		return (gint)(ch - 'A');
	if (ch >= '0' && ch <= '9')
		return 26 + (gint)(ch - '0');
	return -1;
}

/* @text centred on the line whose top is @y, each pixel of the face @scale
 * pixels square. */
static void
canvas_text(
	Canvas      *c,
	const gchar *text,
	guint        y,
	guint        scale,
	guint8       r,
	guint8       g,
	guint8       b
){
	guint n = (guint)g_utf8_strlen(text, -1);
	guint cell = 6 * scale;
	guint width = n * cell;
	guint x = width < c->width ? (c->width - width) / 2 : 0;
	const gchar *p;

	for (p = text; *p != '\0'; p = g_utf8_next_char(p), x += cell) {
		gint glyph = glyph_of(g_utf8_get_char(p));
		guint row;
		guint column;

		if (glyph < 0)
			continue;
		for (row = 0; row < 7; row++) {
			for (column = 0; column < 5; column++) {
				if ((glyphs[glyph][row] >> (4 - column)) & 1)
					canvas_fill(c, x + column * scale, y + row * scale, x + (column + 1) * scale,
						y + (row + 1) * scale, r, g, b);
			}
		}
	}
}

/* The words of @title on lines of at most @per_line characters. */
static GPtrArray *
wrap_words(
	const gchar *title,
	guint        per_line
){
	g_auto(GStrv) words = g_strsplit(title, " ", -1);
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
	GString *line = g_string_new(NULL);
	guint i;

	for (i = 0; words[i] != NULL; i++) {
		if (words[i][0] == '\0')
			continue;
		if (line->len > 0 && line->len + 1 + strlen(words[i]) > per_line) {
			g_ptr_array_add(lines, g_string_free(line, FALSE));
			line = g_string_new(NULL);
		}
		if (line->len > 0)
			g_string_append_c(line, ' ');
		g_string_append(line, words[i]);
	}
	if (line->len > 0)
		g_ptr_array_add(lines, g_string_free(line, FALSE));
	else
		g_string_free(line, TRUE);
	return lines;
}

static void
hsv_to_rgb(
	gdouble  h,
	gdouble  s,
	gdouble  v,
	guint8  *r,
	guint8  *g,
	guint8  *b
){
	gdouble c = v * s;
	gdouble hh = h * 6.0;
	gdouble x = c * (1.0 - ABS(fmod(hh, 2.0) - 1.0));
	gdouble m = v - c;
	gdouble rr = 0;
	gdouble gg = 0;
	gdouble bb = 0;

	if (hh < 1) {
		rr = c; gg = x;
	} else if (hh < 2) {
		rr = x; gg = c;
	} else if (hh < 3) {
		gg = c; bb = x;
	} else if (hh < 4) {
		gg = x; bb = c;
	} else if (hh < 5) {
		rr = x; bb = c;
	} else {
		rr = c; bb = x;
	}
	*r = (guint8)((rr + m) * 255.0);
	*g = (guint8)((gg + m) * 255.0);
	*b = (guint8)((bb + m) * 255.0);
}

static guint32 crc_table[256];

static void
crc_init(void)
{
	guint32 n;

	for (n = 0; n < 256; n++) {
		guint32 c = n;
		gint k;

		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static guint32
crc_of(
	const guint8 *data,
	gsize         length,
	guint32       crc
){
	gsize i;

	for (i = 0; i < length; i++)
		crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

static void
put_be32(
	GByteArray *out,
	guint32     value
){
	guint8 bytes[4];

	bytes[0] = (guint8)(value >> 24);
	bytes[1] = (guint8)(value >> 16);
	bytes[2] = (guint8)(value >> 8);
	bytes[3] = (guint8)value;
	g_byte_array_append(out, bytes, 4);
}

static void
put_chunk(
	GByteArray   *out,
	const gchar  *type,
	const guint8 *data,
	gsize         length
){
	guint32 crc;

	put_be32(out, (guint32)length);
	g_byte_array_append(out, (const guint8 *)type, 4);
	if (length > 0)
		g_byte_array_append(out, data, (guint)length);
	crc = crc_of((const guint8 *)type, 4, 0xFFFFFFFFu);
	crc = crc_of(data, length, crc);
	put_be32(out, crc ^ 0xFFFFFFFFu);
}

static GBytes *
deflated(
	const guint8 *data,
	gsize         length
){
	g_autoptr(GZlibCompressor) zlib = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 6);
	g_autoptr(GOutputStream) memory = g_memory_output_stream_new_resizable();
	g_autoptr(GOutputStream) out = g_converter_output_stream_new(memory, G_CONVERTER(zlib));

	if (!g_output_stream_write_all(out, data, length, NULL, NULL, NULL) || !g_output_stream_close(out, NULL, NULL))
		g_error("cannot compress a poster");
	return g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(memory));
}

static GBytes *
canvas_png(Canvas *c)
{
	static const guint8 signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	GByteArray *out = g_byte_array_new();
	g_autoptr(GBytes) idat = NULL;
	guint8 header[13];
	guint8 *raw;
	gsize stride = (gsize)c->width * 3 + 1;
	guint y;

	raw = g_malloc(stride * c->height);
	for (y = 0; y < c->height; y++) {
		raw[y * stride] = 0;
		memcpy(raw + y * stride + 1, c->rgb + (gsize)y * c->width * 3, (gsize)c->width * 3);
	}
	idat = deflated(raw, stride * c->height);
	g_free(raw);

	header[0] = (guint8)(c->width >> 24);
	header[1] = (guint8)(c->width >> 16);
	header[2] = (guint8)(c->width >> 8);
	header[3] = (guint8)c->width;
	header[4] = (guint8)(c->height >> 24);
	header[5] = (guint8)(c->height >> 16);
	header[6] = (guint8)(c->height >> 8);
	header[7] = (guint8)c->height;
	header[8] = 8;	/* bits */
	header[9] = 2;	/* RGB */
	header[10] = 0;
	header[11] = 0;
	header[12] = 0;
	g_byte_array_append(out, signature, sizeof(signature));
	put_chunk(out, "IHDR", header, sizeof(header));
	put_chunk(out, "IDAT", g_bytes_get_data(idat, NULL), g_bytes_get_size(idat));
	put_chunk(out, "IEND", NULL, 0);
	return g_byte_array_free_to_bytes(out);
}

/*
 * A poster (@backdrop FALSE, two by three) or a backdrop (sixteen by nine)
 * @width pixels wide for @title and @year: a colour of its own (from the
 * id), a frame, the title in capitals and the year in a band at the foot.
 */
static GBytes *
draw_art(
	gint         id,
	const gchar *title,
	gint         year,
	gboolean     backdrop,
	guint        width
){
	Canvas c;
	guint8 r, g, b;
	guint8 lr, lg, lb;
	guint8 dr, dg, db;
	guint scale;
	guint per_line;
	guint line_height;
	guint top;
	guint i;
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *upper = g_utf8_strup(title, -1);
	g_autofree gchar *year_text = year > 0 ? g_strdup_printf("%d", year) : g_strdup("");
	GBytes *png;
	gdouble hue = fmod((gdouble)(id * 0.61803398875), 1.0);
	guint frame;

	c.width = CLAMP(width, 64, 1280);
	c.height = backdrop ? c.width * 9 / 16 : c.width * 3 / 2;
	c.rgb = g_malloc((gsize)c.width * c.height * 3);
	hsv_to_rgb(hue, 0.50, 0.46, &r, &g, &b);
	hsv_to_rgb(hue, 0.18, 0.95, &lr, &lg, &lb);
	hsv_to_rgb(hue, 0.55, 0.24, &dr, &dg, &db);
	canvas_fill(&c, 0, 0, c.width, c.height, r, g, b);

	/* A thin frame inside the edge. */
	frame = MAX(c.width / 40, 2);
	canvas_fill(&c, frame * 2, frame * 2, c.width - frame * 2, frame * 3, lr, lg, lb);
	canvas_fill(&c, frame * 2, c.height - frame * 3, c.width - frame * 2, c.height - frame * 2, lr, lg, lb);
	canvas_fill(&c, frame * 2, frame * 2, frame * 3, c.height - frame * 2, lr, lg, lb);
	canvas_fill(&c, c.width - frame * 3, frame * 2, c.width - frame * 2, c.height - frame * 2, lr, lg, lb);

	/* The band at the foot, with the year. */
	canvas_fill(&c, frame * 3, c.height * 4 / 5, c.width - frame * 3, c.height - frame * 3, dr, dg, db);
	scale = MAX(c.width / (backdrop ? 160 : 90), 1);
	canvas_text(&c, year_text, c.height * 4 / 5 + (c.height / 5 - frame * 3 - 7 * scale) / 2, scale, lr, lg, lb);

	/* The title, as large as fits in four lines with its longest word whole. */
	scale = MAX(c.width / (backdrop ? 90 : 60), 1);
	{
		g_auto(GStrv) words = g_strsplit(upper, " ", -1);
		guint longest = 1;

		for (i = 0; words[i] != NULL; i++)
			longest = MAX(longest, (guint)g_utf8_strlen(words[i], -1));
		while (scale > 1 && longest * 6 * scale > c.width - frame * 8)
			scale--;
	}
	per_line = (c.width - frame * 8) / (6 * scale);
	lines = wrap_words(upper, MAX(per_line, 4));
	while (lines->len > 4 && scale > 1) {
		scale--;
		per_line = (c.width - frame * 8) / (6 * scale);
		g_ptr_array_unref(lines);
		lines = wrap_words(upper, MAX(per_line, 4));
	}
	line_height = 10 * scale;
	top = backdrop ? (c.height * 4 / 5 - lines->len * line_height) / 2 : c.height / 6;
	for (i = 0; i < lines->len; i++)
		canvas_text(&c, g_ptr_array_index(lines, i), top + i * line_height, scale, lr, lg, lb);

	png = canvas_png(&c);
	g_free(c.rgb);
	return png;
}

/* ── The stand-in for TMDb ──────────────────────────────────────── */

typedef struct
{
	AxisMetaProviderInfo  info;	/* first: a pointer to this is the provider */
	AxisTestPlex         *api;
	AxisTestPlex         *images;
	gchar                *api_origin;
	gchar                *image_origin;
	GMutex                lock;
	GHashTable           *art;	/* (lock) image path -> GBytes */
} FakeTmdb;

static void
add_genres(
	JsonBuilder *b,
	const gchar *genres,
	gboolean     named,
	gboolean     tv
){
	g_auto(GStrv) ids = g_strsplit(genres != NULL ? genres : "", ",", -1);
	guint i;

	json_builder_set_member_name(b, named ? "genres" : "genre_ids");
	json_builder_begin_array(b);
	for (i = 0; ids[i] != NULL; i++) {
		gint64 id = g_ascii_strtoll(ids[i], NULL, 10);
		const gchar *name = NULL;
		guint k;

		if (id <= 0)
			continue;
		if (!named) {
			json_builder_add_int_value(b, id);
			continue;
		}
		if (tv) {
			for (k = 0; k < G_N_ELEMENTS(tv_genres); k++)
				if (tv_genres[k].id == id)
					name = tv_genres[k].name;
		} else {
			for (k = 0; k < G_N_ELEMENTS(movie_genres); k++)
				if (movie_genres[k].id == id)
					name = movie_genres[k].name;
		}
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id");
		json_builder_add_int_value(b, id);
		json_builder_set_member_name(b, "name");
		json_builder_add_string_value(b, name != NULL ? name : "Other");
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
}

static void
member_string(
	JsonBuilder *b,
	const gchar *name,
	const gchar *value
){
	json_builder_set_member_name(b, name);
	if (value != NULL)
		json_builder_add_string_value(b, value);
	else
		json_builder_add_null_value(b);
}

static void
member_int(
	JsonBuilder *b,
	const gchar *name,
	gint64       value
){
	json_builder_set_member_name(b, name);
	json_builder_add_int_value(b, value);
}

static void
member_double(
	JsonBuilder *b,
	const gchar *name,
	gdouble      value
){
	json_builder_set_member_name(b, name);
	json_builder_add_double_value(b, value);
}

static void
art_members(
	JsonBuilder *b,
	gint         id
){
	g_autofree gchar *poster = g_strdup_printf("/p%d.png", id);
	g_autofree gchar *backdrop = g_strdup_printf("/b%d.png", id);

	member_string(b, "poster_path", poster);
	member_string(b, "backdrop_path", backdrop);
}

/* A film as a search or a list gives it, or (@full) its whole record. */
static void
film_json(
	JsonBuilder    *b,
	const DemoFilm *film,
	gboolean        full
){
	g_autofree gchar *date = film_date(film);

	json_builder_begin_object(b);
	member_int(b, "id", film->id);
	member_string(b, "title", film->title);
	member_string(b, "original_title", film->title);
	member_string(b, "original_language", "en");
	member_string(b, "overview", film->overview);
	member_string(b, "release_date", date);
	art_members(b, film->id);
	member_double(b, "vote_average", film->rating);
	member_int(b, "vote_count", film->rating > 0 ? 100 + film->id % 900 : 0);
	member_double(b, "popularity", 10.0 + film->id % 97);
	json_builder_set_member_name(b, "adult");
	json_builder_add_boolean_value(b, FALSE);
	member_string(b, "media_type", "movie");
	add_genres(b, film->genres, full, FALSE);
	if (full) {
		member_int(b, "runtime", film->runtime);
		member_string(b, "status", film->date != NULL ? "Released" : "Post Production");
		json_builder_set_member_name(b, "alternative_titles");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "titles");
		json_builder_begin_array(b);
		json_builder_end_array(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "external_ids");
		json_builder_begin_object(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "release_dates");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "results");
		json_builder_begin_array(b);
		json_builder_end_array(b);
		json_builder_end_object(b);
	}
	json_builder_end_object(b);
}

static gboolean
series_running(const DemoSeries *show)
{
	return !show->ended;
}

static void
series_json(
	JsonBuilder      *b,
	const DemoSeries *show,
	gboolean          full
){
	g_autofree gchar *first = episode_date(show, 1, 1);
	g_autofree gchar *last = episode_date(show, show->seasons, show->episodes);
	gint s;

	json_builder_begin_object(b);
	member_int(b, "id", show->id);
	member_string(b, "name", show->title);
	member_string(b, "original_name", show->title);
	member_string(b, "original_language", "en");
	member_string(b, "overview", show->overview);
	member_string(b, "first_air_date", first);
	art_members(b, show->id);
	member_double(b, "vote_average", show->rating);
	member_int(b, "vote_count", 80 + show->id % 700);
	member_double(b, "popularity", 10.0 + show->id % 89);
	member_string(b, "media_type", "tv");
	add_genres(b, show->genres, full, TRUE);
	if (full) {
		member_string(b, "last_air_date", last);
		member_string(b, "status", series_running(show) ? "Returning Series" : "Ended");
		json_builder_set_member_name(b, "episode_run_time");
		json_builder_begin_array(b);
		json_builder_add_int_value(b, show->runtime);
		json_builder_end_array(b);
		member_int(b, "number_of_seasons", show->seasons);
		member_int(b, "number_of_episodes", show->seasons * show->episodes);
		json_builder_set_member_name(b, "seasons");
		json_builder_begin_array(b);
		for (s = 1; s <= show->seasons; s++) {
			g_autofree gchar *air = episode_date(show, s, 1);
			g_autofree gchar *name = g_strdup_printf("Season %d", s);

			json_builder_begin_object(b);
			member_int(b, "season_number", s);
			member_string(b, "name", name);
			member_string(b, "air_date", air);
			member_int(b, "episode_count", show->episodes);
			member_string(b, "overview", "");
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_set_member_name(b, "external_ids");
		json_builder_begin_object(b);
		json_builder_end_object(b);
		json_builder_set_member_name(b, "alternative_titles");
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "results");
		json_builder_begin_array(b);
		json_builder_end_array(b);
		json_builder_end_object(b);
	}
	json_builder_end_object(b);
}

static void
season_json(
	JsonBuilder      *b,
	const DemoSeries *show,
	gint              season
){
	g_autofree gchar *air = episode_date(show, season, 1);
	g_autofree gchar *name = g_strdup_printf("Season %d", season);
	gint e;

	json_builder_begin_object(b);
	member_int(b, "season_number", season);
	member_string(b, "name", name);
	member_string(b, "air_date", air);
	member_string(b, "overview", "");
	json_builder_set_member_name(b, "episodes");
	json_builder_begin_array(b);
	for (e = 1; e <= show->episodes; e++) {
		g_autofree gchar *date = episode_date(show, season, e);
		const gchar *title = episode_names[(show->id * 7 + season * 5 + e) % G_N_ELEMENTS(episode_names)];

		json_builder_begin_object(b);
		member_int(b, "episode_number", e);
		member_int(b, "season_number", season);
		member_string(b, "name", title);
		member_string(b, "air_date", date);
		member_string(b, "overview", "Made up for the demo.");
		member_int(b, "runtime", show->runtime);
		member_int(b, "id", show->id * 1000 + season * 100 + e);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
}

/* Whether @query (folded) is in @title (folded). */
static gboolean
title_matches(
	const gchar *title,
	const gchar *query
){
	g_autofree gchar *t = g_utf8_casefold(title, -1);
	g_autofree gchar *q = g_utf8_casefold(query, -1);
	g_auto(GStrv) words = g_strsplit(q, " ", -1);
	guint i;

	for (i = 0; words[i] != NULL; i++) {
		if (words[i][0] != '\0' && strstr(t, words[i]) == NULL)
			return FALSE;
	}
	return q[0] != '\0';
}

/* The value of @name in an urlencoded @query, decoded. */
static gchar *
query_value(
	const gchar *query,
	const gchar *name
){
	g_autoptr(GHashTable) form = NULL;
	const gchar *value;

	if (query == NULL)
		return NULL;
	form = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_WWW_FORM, NULL);
	value = form != NULL ? g_hash_table_lookup(form, name) : NULL;
	return g_strdup(value);
}

static void
list_begin(JsonBuilder *b)
{
	json_builder_begin_object(b);
	member_int(b, "page", 1);
	json_builder_set_member_name(b, "results");
	json_builder_begin_array(b);
}

static void
list_end(
	JsonBuilder *b,
	guint        n
){
	json_builder_end_array(b);
	member_int(b, "total_pages", 1);
	member_int(b, "total_results", n);
	json_builder_end_object(b);
}

static gchar *
answer_text(
	const gchar *path,
	const gchar *query
){
	g_autoptr(JsonBuilder) b = json_builder_new();
	g_autoptr(JsonGenerator) gen = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_auto(GStrv) parts = g_strsplit(path, "/", -1);
	guint n = g_strv_length(parts);
	guint count = 0;
	guint i;

	/* "", "3", ... */
	if (n < 3 || strcmp(parts[1], "3") != 0)
		return NULL;

	if (n == 4 && strcmp(parts[2], "search") == 0) {
		g_autofree gchar *text = query_value(query, "query");
		gboolean tv = strcmp(parts[3], "tv") == 0;

		list_begin(b);
		for (i = 0; !tv && text != NULL && i < G_N_ELEMENTS(films); i++) {
			if (title_matches(films[i].title, text)) {
				film_json(b, &films[i], FALSE);
				count++;
			}
		}
		for (i = 0; tv && text != NULL && i < G_N_ELEMENTS(series); i++) {
			if (title_matches(series[i].title, text)) {
				series_json(b, &series[i], FALSE);
				count++;
			}
		}
		list_end(b, count);
	} else if (n == 5 && strcmp(parts[2], "genre") == 0) {
		gboolean tv = strcmp(parts[3], "tv") == 0;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "genres");
		json_builder_begin_array(b);
		for (i = 0; i < (tv ? G_N_ELEMENTS(tv_genres) : G_N_ELEMENTS(movie_genres)); i++) {
			json_builder_begin_object(b);
			member_int(b, "id", tv ? tv_genres[i].id : movie_genres[i].id);
			member_string(b, "name", tv ? tv_genres[i].name : movie_genres[i].name);
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
		json_builder_end_object(b);
	} else if (n >= 4 && strcmp(parts[2], "find") == 0) {
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "movie_results");
		json_builder_begin_array(b);
		json_builder_end_array(b);
		json_builder_set_member_name(b, "tv_results");
		json_builder_begin_array(b);
		json_builder_end_array(b);
		json_builder_end_object(b);
	} else if (n == 5 && strcmp(parts[2], "trending") == 0) {
		gboolean tv = strcmp(parts[3], "tv") == 0;

		list_begin(b);
		for (i = 0; !tv && i < G_N_ELEMENTS(films); i++) {
			if (films[i].lists & LIST_TRENDING) {
				film_json(b, &films[i], FALSE);
				count++;
			}
		}
		for (i = 0; tv && i < G_N_ELEMENTS(series); i++) {
			if (series[i].lists & LIST_TRENDING) {
				series_json(b, &series[i], FALSE);
				count++;
			}
		}
		list_end(b, count);
	} else if (n == 4 && (strcmp(parts[2], "movie") == 0 || strcmp(parts[2], "tv") == 0) &&
	           !g_ascii_isdigit(parts[3][0])) {
		gboolean tv = strcmp(parts[2], "tv") == 0;
		guint want = 0;

		if (strcmp(parts[3], "popular") == 0)
			want = LIST_POPULAR;
		else if (strcmp(parts[3], "now_playing") == 0 || strcmp(parts[3], "on_the_air") == 0)
			want = LIST_NOW;
		else if (strcmp(parts[3], "upcoming") == 0)
			want = LIST_UPCOMING;
		else if (strcmp(parts[3], "top_rated") == 0)
			want = LIST_TOP;
		if (want == 0)
			return NULL;
		list_begin(b);
		for (i = 0; !tv && i < G_N_ELEMENTS(films); i++) {
			if (films[i].lists & want) {
				film_json(b, &films[i], FALSE);
				count++;
			}
		}
		for (i = 0; tv && i < G_N_ELEMENTS(series); i++) {
			if (series[i].lists & want) {
				series_json(b, &series[i], FALSE);
				count++;
			}
		}
		list_end(b, count);
	} else if (n >= 4 && strcmp(parts[2], "movie") == 0) {
		const DemoFilm *film = film_of((gint)g_ascii_strtoll(parts[3], NULL, 10));

		if (film == NULL)
			return NULL;
		if (n == 5 && strcmp(parts[4], "recommendations") == 0) {
			list_begin(b);
			for (i = 0; i < G_N_ELEMENTS(films) && count < 6; i++) {
				if (films[i].id != film->id && (films[i].id + film->id) % 3 == 0) {
					film_json(b, &films[i], FALSE);
					count++;
				}
			}
			list_end(b, count);
		} else if (n == 4) {
			film_json(b, film, TRUE);
		} else {
			return NULL;
		}
	} else if (n >= 4 && strcmp(parts[2], "tv") == 0) {
		const DemoSeries *show = series_of((gint)g_ascii_strtoll(parts[3], NULL, 10));

		if (show == NULL)
			return NULL;
		if (n == 6 && strcmp(parts[4], "season") == 0) {
			gint season = (gint)g_ascii_strtoll(parts[5], NULL, 10);

			if (season < 1 || season > show->seasons)
				return NULL;
			season_json(b, show, season);
		} else if (n == 5 && strcmp(parts[4], "recommendations") == 0) {
			list_begin(b);
			for (i = 0; i < G_N_ELEMENTS(series) && count < 4; i++) {
				if (series[i].id != show->id && (series[i].id + show->id) % 2 == 1) {
					series_json(b, &series[i], FALSE);
					count++;
				}
			}
			list_end(b, count);
		} else if (n == 4) {
			series_json(b, show, TRUE);
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}

	root = json_builder_get_root(b);
	json_generator_set_root(gen, root);
	return json_generator_to_data(gen, NULL);
}

static void
answer_api(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	gchar *text = answer_text(request->path, request->query);

	answer->content_type = g_strdup("application/json;charset=utf-8");
	if (text == NULL) {
		answer->status = 404;
		answer->body = g_bytes_new_static("{\"status_code\":34}", 18);
		return;
	}
	answer->status = 200;
	answer->body = g_bytes_new_take(text, strlen(text));
}

/* "/t/p/w342/p1001.png": a poster of 1001, 342 pixels wide. */
static void
answer_image(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	FakeTmdb *self = user_data;
	const gchar *name = strrchr(request->path, '/');
	const gchar *size = strstr(request->path, "/t/p/w");
	GBytes *png;

	answer->status = 404;
	if (name == NULL || size == NULL || (name[1] != 'p' && name[1] != 'b'))
		return;

	g_mutex_lock(&self->lock);
	png = g_hash_table_lookup(self->art, request->path);
	if (png == NULL) {
		gint id = (gint)g_ascii_strtoll(name + 2, NULL, 10);
		guint width = (guint)g_ascii_strtoull(size + 6, NULL, 10);
		const DemoFilm *film = film_of(id);
		const DemoSeries *show = series_of(id);
		g_autofree gchar *date = NULL;

		if (film != NULL)
			date = film_date(film);
		else if (show != NULL)
			date = episode_date(show, 1, 1);
		if (film != NULL || show != NULL) {
			png = draw_art(id, film != NULL ? film->title : show->title, year_of(date), name[1] == 'b',
				MIN(width, name[1] == 'b' ? 1280u : 500u));
			g_hash_table_insert(self->art, g_strdup(request->path), png);
		}
	}
	if (png != NULL) {
		answer->status = 200;
		answer->content_type = g_strdup("image/png");
		answer->body = g_bytes_ref(png);
	}
	g_mutex_unlock(&self->lock);
}

static FakeTmdb *
fake_tmdb_new(void)
{
	FakeTmdb *self = g_new0(FakeTmdb, 1);

	g_mutex_init(&self->lock);
	self->art = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
	self->api = axis_test_plex_new();
	self->images = axis_test_plex_new();
	axis_test_plex_set_handler(self->api, answer_api, self);
	axis_test_plex_set_handler(self->images, answer_image, self);
	self->api_origin = axis_test_plex_base(self->api);
	self->image_origin = axis_test_plex_base(self->images);
	self->info = *axis_meta_tmdb_get_info();
	self->info.api_origin = self->api_origin;
	self->info.image_origin = self->image_origin;
	return self;
}

static void
fake_tmdb_free(FakeTmdb *self)
{
	if (self == NULL)
		return;
	axis_test_plex_free(self->api);
	axis_test_plex_free(self->images);
	g_hash_table_unref(self->art);
	g_free(self->api_origin);
	g_free(self->image_origin);
	g_mutex_clear(&self->lock);
	g_free(self);
}

/* ── The instance ───────────────────────────────────────────────── */

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk1;
	gchar                *disk2;
	gchar                *disk3;
	gint64                volume1;
	gint64                volume2;
	gint64                volume3;
	gint64                movies_root;
	gint64                tv_root;
	gint64                downloads_root;
	gint64                movies2_root;
	gint64                archive_root;
	gchar                *downloads_dir;

	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisHistory          *history;
	AxisWebSettings      *web_settings;
	AxisModuleRegistry   *registry;
	AxisJobRegistry      *job_registry;
	AxisWorkerTokens     *worker_tokens;
	AxisWebHub           *hub;
	AxisWebJobs          *jobs_ui;
	AxisWebStorage       *storage_ui;
	AxisWebLibrary       *library_ui;
	AxisProbe            *probe;
	AxisWebProbe         *probe_ui;
	AxisWebBrowser       *browser_ui;
	AxisAnalysis         *analysis;
	AxisWebAnalysis      *analysis_ui;
	AxisEstimator        *estimator;
	AxisWebSavings       *savings_ui;
	AxisCompat           *compat;
	AxisWebCompat        *compat_ui;
	AxisWebProfiles      *profiles_ui;
	AxisWebWorkers       *workers_ui;
	AxisConversions      *conversions;
	AxisWebConversions   *conversions_ui;
	FakeTmdb             *tmdb;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisTestDownloadFake *fake;
	AxisTestDownloadFake *fake_torrent;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	gint64                client_id;
	gint64                torrent_client_id;
	AxisImport           *import;
	AxisWebImports       *imports_ui;
	AxisTestProwlarr     *prowlarr;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisIdentify         *identify;
	AxisWebIdentify      *identify_ui;
	AxisMovies           *movies;
	AxisTv               *tv;
	AxisDiscover         *discover;
	AxisWebDiscover      *discover_ui;
	AxisWebReady         *ready_ui;
	AxisWebSetup         *setup_ui;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	gchar                *tool[3];	/* the fake probe's command, ended by NULL */
	gint                  notices;	/* --attention: made-up notices beside the scene's own */
} Demo;

static gchar *
q(
	Demo        *d,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Demo        *d,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(d->t->db, sql);
}

static gint64
qi(
	Demo        *d,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
qi(
	Demo        *d,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *value = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	value = axis_test_db_query(d->t->db, sql);
	return value != NULL ? g_ascii_strtoll(value, NULL, 10) : 0;
}

static void
must(
	gboolean     ok,
	GError      *error,
	const gchar *what
){
	if (!ok)
		g_error("demo: %s failed: %s", what, error != NULL ? error->message : "no reason given");
}

/* The demo's own module of made-up notices (below). */
static const AxisModuleInfo *
notices_module(void);

/* Every module the daemon has (src/axisd/main.c), with the stand-ins. */
static void
demo_build(Demo *d)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;
	AxisProbeTuning probe_tuning;
	AxisDbConn *conn;

	path = g_build_filename(d->t->dir, "data", NULL);
	d->data = axis_data_dir_open(path, &error);
	must(d->data != NULL, error, "the data directory");

	d->settings = axis_settings_new();
	d->web_settings = axis_web_settings_new(d->settings, d->t->pool);
	d->registry = axis_module_registry_new();
	must(axis_module_registry_register(d->registry, axis_job_builtin_get_module(), d->settings, &error), error,
		"the jobs module");
	d->history = axis_history_new(d->settings);
	must(axis_module_registry_register(d->registry, axis_history_get_module(), d->history, &error), error,
		"the history module");
	must(axis_module_registry_register(d->registry, axis_web_settings_get_module(), d->web_settings, &error), error,
		"the settings pages");
	d->hub = axis_web_hub_new(NULL);
	d->jobs_ui = axis_web_jobs_new(d->t->pool, d->hub);
	must(axis_module_registry_register(d->registry, axis_web_jobs_get_module(), d->jobs_ui, &error), error,
		"the jobs pages");
	d->storage_ui = axis_web_storage_new(d->t->storage, d->hub);
	must(axis_module_registry_register(d->registry, axis_web_storage_get_module(), d->storage_ui, &error), error,
		"the storage pages");
	d->library_ui = axis_web_library_new(d->t->library, d->t->storage);
	must(axis_module_registry_register(d->registry, axis_web_library_get_module(), d->library_ui, &error), error,
		"the library");
	d->probe = axis_probe_new(d->t->pool, d->t->storage, NULL);
	d->tool[0] = g_strdup("/bin/sh");
	d->tool[1] = g_canonicalize_filename(FAKE_PROBE, NULL);
	memset(&probe_tuning, 0, sizeof(probe_tuning));
	probe_tuning.tool_command = (const gchar *const *)d->tool;
	axis_probe_set_tuning(d->probe, &probe_tuning);
	d->probe_ui = axis_web_probe_new(d->probe, d->t->storage);
	must(axis_module_registry_register(d->registry, axis_web_probe_get_module(), d->probe_ui, &error), error,
		"the probe");
	d->browser_ui = axis_web_browser_new(d->t->library, d->probe, d->t->storage, d->hub);
	must(axis_module_registry_register(d->registry, axis_web_browser_get_module(), d->browser_ui, &error), error,
		"the library pages");
	d->analysis = axis_analysis_new(d->t->pool, d->settings);
	d->analysis_ui = axis_web_analysis_new(d->analysis);
	must(axis_module_registry_register(d->registry, axis_web_analysis_get_module(), d->analysis_ui, &error), error,
		"the analysis pages");
	d->estimator = axis_estimator_new(d->t->pool, d->t->storage, d->settings);
	d->savings_ui = axis_web_savings_new(d->estimator);
	must(axis_module_registry_register(d->registry, axis_web_savings_get_module(), d->savings_ui, &error), error,
		"the savings pages");
	axis_web_probe_set_estimator(d->probe_ui, d->estimator);
	axis_web_browser_set_estimator(d->browser_ui, d->estimator);
	d->compat = axis_compat_new(d->t->pool, d->t->storage, d->settings, d->estimator);
	d->compat_ui = axis_web_compat_new(d->compat);
	must(axis_module_registry_register(d->registry, axis_web_compat_get_module(), d->compat_ui, &error), error,
		"the compatibility pages");
	axis_web_savings_set_compat(d->savings_ui, d->compat);
	d->profiles_ui = axis_web_profiles_new(d->estimator);
	axis_web_profiles_set_compat(d->profiles_ui, d->compat);
	must(axis_module_registry_register(d->registry, axis_web_profiles_get_module(), d->profiles_ui, &error), error,
		"the profiles pages");
	d->worker_tokens = axis_worker_tokens_new();
	conn = axis_db_pool_acquire(d->t->pool, &error);
	must(conn != NULL, error, "a connection");
	must(axis_worker_tokens_load(d->worker_tokens, conn, &error), error, "the worker tokens");
	axis_db_pool_release(d->t->pool, conn);
	d->workers_ui = axis_web_workers_new(d->t->pool, d->worker_tokens, d->estimator, 15000);
	must(axis_module_registry_register(d->registry, axis_web_workers_get_module(), d->workers_ui, &error), error,
		"the workers page");
	d->conversions = axis_conversions_new(d->t->pool, d->settings, d->t->storage, d->probe, d->estimator);
	axis_conversions_set_verdict(d->conversions, axis_web_conversions_compat_verdict, d->compat);
	d->conversions_ui = axis_web_conversions_new(d->conversions);
	must(axis_module_registry_register(d->registry, axis_web_conversions_get_module(), d->conversions_ui, &error),
		error, "the conversions pages");
	axis_web_probe_set_file_card(d->probe_ui, axis_web_conversions_file_collect, axis_web_conversions_file_render,
		axis_web_conversions_file_free, d->conversions_ui);
	axis_web_workers_set_conversions(d->workers_ui, d->conversions);

	d->meta = axis_meta_new(d->t->pool, d->settings, d->data, &error);
	must(d->meta != NULL, error, "the metadata layer");
	d->tmdb = fake_tmdb_new();
	must(axis_meta_add_provider(d->meta, &d->tmdb->info, &error), error, "the stand-in for TMDb");
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 5000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(d->meta, &meta_tuning);
	d->meta_ui = axis_web_meta_new(d->meta);
	must(axis_module_registry_register(d->registry, axis_web_meta_get_module(d->meta_ui), d->meta_ui, &error), error,
		"the metadata pages");

	d->downloads = axis_downloads_new(d->t->pool, d->t->storage, d->settings);
	must(axis_downloads_add_kind(d->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error), error,
		"the torrent client kind");
	must(axis_downloads_add_kind(d->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error), error,
		"the Usenet client kind");
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 3000;
	axis_downloads_set_tuning(d->downloads, &download_tuning);
	d->downloads_ui = axis_web_downloads_new(d->downloads, d->t->storage, d->hub);
	must(axis_module_registry_register(d->registry, axis_web_downloads_get_module(), d->downloads_ui, &error), error,
		"the download clients pages");
	d->import = axis_import_new(d->t->pool, d->settings, d->t->storage, d->downloads, d->probe);
	d->imports_ui = axis_web_imports_new(d->import, d->settings);
	must(axis_module_registry_register(d->registry, axis_web_imports_get_module(), d->imports_ui, &error), error,
		"the imports pages");
	d->indexers = axis_indexers_new(d->t->pool, d->settings);
	must(axis_indexers_add_kind(d->indexers, axis_indexer_prowlarr_kind(), &error), error, "the Prowlarr kind");
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(d->indexers, &indexer_tuning);
	d->indexers_ui = axis_web_indexers_new(d->indexers);
	must(axis_module_registry_register(d->registry, axis_web_indexers_get_module(), d->indexers_ui, &error), error,
		"the indexers pages");
	d->quality = axis_quality_store_new(d->t->pool, d->settings);
	d->quality_ui = axis_web_quality_new(d->quality);
	axis_web_quality_set_compat(d->quality_ui, d->compat);
	must(axis_module_registry_register(d->registry, axis_web_quality_get_module(), d->quality_ui, &error), error,
		"the quality pages");
	d->identify = axis_identify_new(d->t->pool, d->settings, d->t->storage, d->meta);
	d->identify_ui = axis_web_identify_new(d->identify, d->quality);
	must(axis_module_registry_register(d->registry, axis_web_identify_get_module(), d->identify_ui, &error), error,
		"the identify pages");
	d->movies = axis_movies_new(d->t->pool, d->settings, d->meta, d->indexers, d->downloads, d->t->storage,
		d->quality);
	axis_movies_set_compat(d->movies, d->compat);
	axis_movies_set_import(d->movies, d->import);
	axis_movies_set_identify(d->movies, d->identify);
	must(axis_module_registry_register(d->registry, axis_movies_get_module(), d->movies, &error), error,
		"the movies module");
	d->tv = axis_tv_new(d->t->pool, d->settings, d->meta, d->indexers, d->downloads, d->t->storage, d->quality);
	axis_tv_set_compat(d->tv, d->compat);
	axis_tv_set_import(d->tv, d->import);
	axis_tv_set_identify(d->tv, d->identify);
	must(axis_module_registry_register(d->registry, axis_tv_get_module(), d->tv, &error), error, "the TV module");
	axis_web_browser_set_folder_titles(d->browser_ui, AXIS_MEDIA_MOVIES, AXIS_IDENTIFY_MODULE_ID,
		axis_web_identify_folder_titles, d->identify);
	axis_web_browser_set_folder_titles(d->browser_ui, AXIS_MEDIA_TV, AXIS_TV_MODULE_ID, axis_tv_folder_titles, d->tv);
	d->discover = axis_discover_new(d->t->pool, d->settings, d->meta);
	d->discover_ui = axis_web_discover_new(d->discover);
	must(axis_module_registry_register(d->registry, axis_web_discover_get_module(), d->discover_ui, &error), error,
		"Discover");
	d->ready_ui = axis_web_ready_new(d->meta, d->indexers, d->downloads, d->t->storage);
	must(axis_module_registry_register(d->registry, axis_web_ready_get_module(), d->ready_ui, &error), error,
		"Getting started");
	{
		AxisWebSetupLayers layers;

		memset(&layers, 0, sizeof(layers));
		layers.settings = d->settings;
		layers.meta = d->meta;
		layers.indexers = d->indexers;
		layers.downloads = d->downloads;
		layers.storage = d->t->storage;
		layers.quality = d->quality;
		layers.compat = d->compat;
		layers.identify = d->identify;
		d->setup_ui = axis_web_setup_new(&layers);
		must(axis_module_registry_register(d->registry, axis_web_setup_get_module(), d->setup_ui, &error), error,
			"the setup guide");
	}
	if (d->notices > 0)
		must(axis_module_registry_register(d->registry, notices_module(), d, &error), error, "the made-up notices");

	must(axis_settings_add_modules(d->settings, d->registry, &error), error, "the settings");
	must(axis_settings_load(d->settings, d->t->pool, &error), error, "loading the settings");
	must(axis_module_state_load(d->registry, d->t->pool, &error), error, "the modules' state");
	axis_web_settings_sync(d->web_settings);

	/* The jobs are named in plain words (the queue itself does not run). */
	d->job_registry = axis_job_registry_new();
	must(axis_job_registry_add_modules(d->job_registry, d->registry, &error), error, "the job kinds");
	must(axis_worker_api_register_kinds(d->job_registry, d->registry, &error), error, "the worker job kinds");
	axis_web_jobs_set_kinds(d->jobs_ui, d->job_registry);
	axis_web_workers_set_kinds(d->workers_ui, d->job_registry);
}

/* ── Made-up notices (--attention) ──────────────────────────────── */

/*
 * A module of the demo's own that gives the page "Needs attention" as many
 * notices as --attention asks for, beside the scene's own: for looking at
 * the bell with 3 or with more than 99, and at the page with every level.
 */
static gboolean
notices_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	static const AxisAttentionLevel levels[] = { AXIS_ATTENTION_BROKEN, AXIS_ATTENTION_WAITING, AXIS_ATTENTION_LOOK };
	const Demo *d = user_data;
	gint i;

	for (i = 0; i < d->notices; i++) {
		g_autofree gchar *subject = g_strdup_printf("%d", i + 1);
		g_autofree gchar *tail = g_strdup_printf(" waits, notice %d of %d made up for the demo.", i + 1, d->notices);
		AxisAttentionItem *item = axis_attention_item_new("demo.notice", subject, levels[i % G_N_ELEMENTS(levels)], 1,
			i + 1);

		axis_attention_item_say(item, "The made-up thing ", i % 2 == 0 ? "Lantern Bay" : "Copper Kettle Lane", tail);
		axis_attention_item_link(item, "/", "Open the dashboard");
		item->since_us = reading->now_us - (gint64)(i + 1) * 600 * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

static void
notices_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_attention_source(web, "the demo's made-up notices", notices_source, user_data);
}

static const AxisModuleInfo *
notices_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = "demo-notices";
		info.name = "Demo notices";
		info.register_routes = notices_routes;
		info.description = "Made-up notices for the page Needs attention (the demo's --attention).";
	}
	return &info;
}

/* The accounts, and the shell with every module's pages. */
static void
demo_serve(
	Demo        *d,
	guint16      port,
	const gchar *scene
){
	g_autoptr(GError) error = NULL;
	gboolean setup = g_strcmp0(scene, "setup") == 0;

	d->server = htmx_server_new();
	must(htmx_server_listen_on(d->server, "127.0.0.1", port, &error), error, "listening");
	d->auth = axis_web_auth_new_with_admin(d->t->pool, "", AXIS_SECURE_COOKIES_AUTO, setup ? NULL : ADMIN_NAME,
		setup ? NULL : ADMIN_PASSWORD, &error);
	must(d->auth != NULL, error, "the accounts");
	axis_web_auth_set_worker_tokens(d->auth, d->worker_tokens);
	if (!setup) {
		g_autofree gchar *hash = axis_password_hash(VIEWER_PASSWORD, &error);
		g_autoptr(AxisUser) viewer = NULL;
		AxisUser *existing = NULL;

		must(hash != NULL, error, "a password hash");
		must(axis_user_store_find(d->t->pool, VIEWER_NAME, &existing, NULL, &error), error, "the viewer");
		if (existing == NULL) {
			viewer = axis_user_store_create(d->t->pool, VIEWER_NAME, hash, AXIS_USER_ROLE_USER, &error);
			must(viewer != NULL, error, "the viewer's account");
		}
		axis_user_free(existing);
		/* The auth loaded the accounts before the viewer was made. */
		axis_web_auth_free(d->auth);
		d->auth = axis_web_auth_new(d->t->pool, "", AXIS_SECURE_COOKIES_AUTO, &error);
		must(d->auth != NULL, error, "the accounts, again");
		axis_web_auth_set_worker_tokens(d->auth, d->worker_tokens);
	}
	d->web = axis_web_new(d->server, d->registry, d->auth);
	axis_web_hub_install(d->hub, d->web, d->auth);
	must(htmx_server_start(d->server, &error), error, "serving");
	must(axis_module_registry_start_all(d->registry, &error), error, "starting the modules");
	/* What needs attention is read as the daemon reads it (the bell in the
	 * header, the page). */
	axis_web_attention_start(d->web, d->t->pool);
}

/* ── Making the scene ───────────────────────────────────────────── */

static gint64
title_id(
	Demo         *d,
	AxisMetaKind  kind,
	gint          provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_strdup_printf("%d", provider_id);
	gint64 result;

	memset(&answer, 0, sizeof(answer));
	must(axis_meta_lookup(d->meta, AXIS_META_TMDB_NAME, kind, id, 8000, &answer, &error) && answer.title != NULL,
		error, "a title from the stand-in for TMDb");
	result = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return result;
}

static gint64
add_film(
	Demo   *d,
	gint    provider_id,
	gint64  root_id
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id(d, AXIS_META_KIND_FILM, provider_id);
	spec.root_id = root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = ADMIN_NAME;
	must(axis_movies_add(d->movies, &spec, &id, &error), error, "adding a film");
	return id;
}

static gint64
add_series(
	Demo          *d,
	gint           provider_id,
	AxisTvMonitor  monitor
){
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id(d, AXIS_META_KIND_SERIES, provider_id);
	spec.root_id = d->tv_root;
	spec.monitor = monitor;
	spec.user = ADMIN_NAME;
	must(axis_tv_add(d->tv, &spec, &id, &error), error, "adding a series");
	return id;
}

/* Prowlarr's answer to every search from now on: @names, each of @size. */
static void
set_releases(
	Demo               *d,
	const gchar *const *names,
	gint64              size
){
	g_autofree gchar *origin = axis_test_prowlarr_base(d->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	static guint serial;
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		serial++;
		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/%u\",\"title\":\"%s\",\"size\":%"
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"Example Usenet\",\"publishDate\":\"2026-11-01T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=heat-nzb\",\"magnetUrl\":null,"
			"\"protocol\":\"usenet\",\"categories\":[{\"id\":2040}]}", i > 0 ? "," : "", serial, names[i],
			size + (gint64)i * 700000000, origin, PROWLARR_KEY, serial);
	}
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(d->prowlarr, NULL, body->str);
	axis_test_db_exec(d->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

static void
search_film(
	Demo   *d,
	gint64  movie
){
	AxisMovieSearchReport report;
	g_autoptr(GError) error = NULL;

	memset(&report, 0, sizeof(report));
	if (!axis_movies_run_search(d->movies, movie, "manual", ADMIN_NAME, NULL, &report, &error))
		g_warning("demo: a film's search failed: %s", error != NULL ? error->message : "no reason given");
}

static void
search_series(
	Demo        *d,
	gint64       show,
	AxisTvScope  scope,
	gint64       item
){
	AxisAcquireSearchReport report;
	g_autoptr(GError) error = NULL;

	memset(&report, 0, sizeof(report));
	if (!axis_tv_run_search(d->tv, show, scope, item, "manual", ADMIN_NAME, NULL, &report, &error))
		g_warning("demo: a series' search failed: %s", error != NULL ? error->message : "no reason given");
}

static void
poll_clients(Demo *d)
{
	g_autoptr(GError) error = NULL;

	if (!axis_downloads_poll_client(d->downloads, d->client_id, NULL, NULL, &error))
		g_warning("demo: polling the client failed: %s", error != NULL ? error->message : "no reason given");
}

static gchar *
movie_ref(
	Demo   *d,
	gint64  movie
){
	return q(d, "SELECT d.client_ref FROM movie_grab g JOIN download d ON d.id = g.download_id"
		" WHERE g.movie_id = %" G_GINT64_FORMAT " AND g.closed_at IS NULL", movie);
}

/* A film grabbed and @percent downloaded. */
static gint64
film_downloading(
	Demo        *d,
	gint         provider_id,
	const gchar *release,
	gint         percent
){
	const gchar *names[] = { release, NULL };
	gint64 movie = add_film(d, provider_id, d->movies_root);
	g_autofree gchar *ref = NULL;

	set_releases(d, names, 6000000000);
	search_film(d, movie);
	ref = movie_ref(d, movie);
	if (ref != NULL) {
		/* The client says how big it is; then how far it has got. */
		gint64 size;

		axis_test_download_fake_set(d->fake, ref, "downloading", 0, NULL, NULL);
		poll_clients(d);
		size = qi(d, "SELECT size_bytes FROM download WHERE client_ref = '%s'", ref);
		axis_test_download_fake_set(d->fake, ref, "downloading", (size > 0 ? size : 6000000000) / 100 * percent, NULL,
			NULL);
		poll_clients(d);
	}
	return movie;
}

/* A file whose first line the fake probe reads, @size bytes long but sparse
 * (it takes no room on the disk; the size is for the checks that read it). */
static void
write_media(
	const gchar *path,
	const gchar *line,
	gint64       size
){
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *head = g_strconcat(line, "\n", NULL);

	g_mkdir_with_parents(dir, 0755);
	if (!g_file_set_contents(path, head, -1, NULL) || truncate(path, (off_t)size) != 0)
		g_error("demo: cannot write %s", path);
}

/* A film grabbed, its download arrived as @files (each starting with
 * @line) and its import run once.  Returns the import. */
static gint64
film_imported(
	Demo               *d,
	gint64              movie,
	const gchar        *release,
	const gchar *const *files,
	const gchar        *line
){
	g_autofree gchar *title = q(d, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);
	g_autofree gchar *download = NULL;
	g_autoptr(GError) error = NULL;
	AxisImportReport report;
	AxisDbConn *conn;
	gint64 import_id = 0;
	guint i;

	for (i = 0; files[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(d->downloads_dir, release, files[i], NULL);

		if (g_str_has_suffix(files[i], ".srt"))
			g_file_set_contents(path, "1\n00:00:01,000 --> 00:00:02,000\nA line.\n", -1, NULL);
		else
			write_media(path, line, G_GINT64_CONSTANT(5800000000));
	}
	download = q(d, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) SELECT %" G_GINT64_FORMAT ", 'SABnzbd', 'ref-%s', 'usenet', 'file', '%s', %s, t.title,"
		" 'movies', 'completed', 6000000000, 6000000000, '/data/downloads/%s', %" G_GINT64_FORMAT ", 'Downloads/%s',"
		" now() - interval '3 hours', now() - interval '2 hours' FROM meta_title t WHERE t.id = %s RETURNING id",
		d->client_id, release, release, title, release, d->volume1, release, title);
	if (download == NULL) {
		g_warning("demo: no download made for %s", release);
		return 0;
	}
	q(d, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at, completed_at,"
		" quality) VALUES (%" G_GINT64_FORMAT ", '%s', 'usenet', lower('1:0:%s'), %s, now() - interval '3 hours',"
		" now() - interval '2 hours', 'webdl_1080p') RETURNING id", movie, release, release, download);
	conn = axis_db_pool_acquire(d->t->pool, &error);
	must(conn != NULL, error, "a connection");
	if (!axis_import_request(d->import, conn, AXIS_MOVIES_MODULE_ID, g_ascii_strtoll(download, NULL, 10), &import_id,
	                         NULL, &error))
		g_warning("demo: the import of %s was not asked for: %s", release, error->message);
	axis_db_pool_release(d->t->pool, conn);
	g_clear_error(&error);
	memset(&report, 0, sizeof(report));
	if (import_id > 0 && !axis_import_run(d->import, import_id, NULL, &report, &error))
		g_warning("demo: the import of %s did not run: %s", release, error != NULL ? error->message : "no reason");
	return import_id;
}

/* A library file holding episode @episode of @show. */
static void
episode_in_library(
	Demo        *d,
	gint64       show,
	const gchar *folder,
	gint         season,
	gint         episode,
	const gchar *quality
){
	gint64 episode_id = qi(d, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND season_number = %d AND number = %d", show, season, episode);
	g_autofree gchar *rel = g_strdup_printf("%s/Season %02d/%s - S%02dE%02d.mkv", folder, season, folder, season,
		episode);
	g_autofree gchar *path = NULL;
	gint64 file;

	if (episode_id == 0)
		return;
	path = g_build_filename(d->disk1, "TV", rel, NULL);
	write_media(path, EPISODE_LINE, G_GINT64_CONSTANT(1800000000));
	file = qi(d, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', 'video',"
		" 1800000000, 1, 0, 1, 0, %" G_GINT64_FORMAT ", 1, false, false, false, 1, 1, 1) RETURNING id", d->tv_root,
		d->volume1, rel, 500000 + episode_id);
	q(d, "INSERT INTO series_file (file_id, series_id, quality, release_name) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", '%s', 'Made.Up.S%02dE%02d.1080p.WEB-DL-DEMO') RETURNING file_id", file, show, quality, season,
		episode);
	q(d, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		") RETURNING file_id", file, episode_id);
}

static gint64
episode_of(
	Demo   *d,
	gint64  show,
	gint    season,
	gint    episode
){
	return qi(d, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT " AND season_number = %d"
		" AND number = %d", show, season, episode);
}

/* Folders of films and series on disk that Axis does not manage, some it
 * will be sure of, some it will hold for a person. */
static void
make_on_disk(Demo *d)
{
	/* The runtimes: Fogbound's and The Cartographer's Daughter's fit both of
	 * their namesakes, so they are held with their candidates. */
	static const struct
	{
		const gchar *folder;
		const gchar *file;
		gint         seconds;
		const gchar *picture;
	} movie_folders[] = {
		{ "Saltmarsh Radio (2019)", "Saltmarsh Radio (2019).mkv", 5880, "width=1920 height=800 vcodec=h264" },
		{ "Orchard of Bells (2010)", "Orchard of Bells (2010) 1080p.mkv", 6720, "width=1920 height=800 vcodec=h264" },
		{ "The Cartographer's Daughter (2018)", "The Cartographer's Daughter (2018).mkv", 5880,
		  "width=1920 height=1040 vcodec=hevc" },
		{ "Fogbound", "Fogbound.mkv", 6420, "width=1280 height=720 vcodec=h264" },
		{ "Holiday Tapes 2009", "Holiday Tapes 2009.mkv", 2400, "width=720 height=576 vcodec=mpeg2video" },
		{ "The Patient Cartwright (2016)", "The Patient Cartwright (2016) 2160p.mkv", 7860,
		  "width=3840 height=1600 vcodec=hevc" }
	};
	static const struct
	{
		gint         id;	/* 0: a series the provider does not know */
		const gchar *name;
		gboolean     dated;
		gint         seasons;
	} series_folders[] = {
		{ 2006, "Static Garden", TRUE, 2 },
		{ 2008, "The Ropewalk", FALSE, 3 },
		{ 0, "Allotment Diaries", FALSE, 1 }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(movie_folders); i++) {
		g_autofree gchar *path = g_build_filename(d->disk2, "Movies", movie_folders[i].folder, movie_folders[i].file,
			NULL);
		g_autofree gchar *line = g_strdup_printf("AXISFAKE duration=%d %s acodec=eac3 channels=6",
			movie_folders[i].seconds, movie_folders[i].picture);

		write_media(path, line, G_GINT64_CONSTANT(9000000000) + i * G_GINT64_CONSTANT(1300000000));
	}
	for (i = 0; i < G_N_ELEMENTS(series_folders); i++) {
		const DemoSeries *show = series_of(series_folders[i].id);
		g_autofree gchar *first = show != NULL ? episode_date(show, 1, 1) : NULL;
		g_autofree gchar *folder = series_folders[i].dated && first != NULL ?
			g_strdup_printf("%s (%d)", series_folders[i].name, year_of(first)) : g_strdup(series_folders[i].name);
		gint s;
		gint e;

		for (s = 1; s <= series_folders[i].seasons; s++) {
			for (e = 1; e <= 6; e++) {
				g_autofree gchar *name = g_strdup_printf("%s S%02dE%02d.mkv", series_folders[i].name, s, e);
				g_autofree gchar *season = g_strdup_printf("Season %d", s);
				g_autofree gchar *path = g_build_filename(d->disk1, "TV", folder, season, name, NULL);
				g_autofree gchar *line = g_strdup_printf("AXISFAKE duration=%d width=1920 height=1080 vcodec=h264 "
					"acodec=aac channels=2", (show != NULL ? show->runtime : 25) * 60);

				write_media(path, line, G_GINT64_CONSTANT(1100000000));
			}
		}
	}
}

/* Every video of @root_id probed (by the fake probe). */
static void
probe_root(
	Demo   *d,
	gint64  root_id
){
	g_autofree gchar *ids = q(d, "SELECT string_agg(id::text, ',') FROM library_file WHERE root_id = %"
		G_GINT64_FORMAT " AND kind = 'video'", root_id);
	g_auto(GStrv) list = g_strsplit(ids != NULL ? ids : "", ",", -1);
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;

		if (list[i][0] == '\0')
			continue;
		if (!axis_probe_file(d->probe, g_ascii_strtoll(list[i], NULL, 10), NULL, &outcome, &error))
			g_warning("demo: a file was not probed: %s", error != NULL ? error->message : "no reason given");
	}
}

static void
scan_and_identify(
	Demo   *d,
	gint64  root_id
){
	g_autoptr(GError) error = NULL;
	AxisIdentifyReport report;

	axis_scan_result_free(axis_test_library_scan(d->t, root_id));
	probe_root(d, root_id);
	memset(&report, 0, sizeof(report));
	if (!axis_identify_run_root(d->identify, root_id, NULL, &report, &error))
		g_warning("demo: identifying a root failed: %s", error != NULL ? error->message : "no reason given");
}

/* A few finished jobs, for the Jobs page and the dashboard. */
static void
make_jobs(Demo *d)
{
	axis_test_db_exec(d->t->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, created_at, started_at, finished_at, progress)"
		" VALUES"
		" ('library.scan', 'scan', 'succeeded', 3, 1, now() - interval '50 minutes', now() - interval '50 minutes',"
		"  now() - interval '48 minutes', 1),"
		" ('identify.root', 'metadata', 'succeeded', 3, 1, now() - interval '47 minutes',"
		"  now() - interval '47 minutes', now() - interval '46 minutes', 1),"
		" ('discover.refresh', 'metadata', 'succeeded', 3, 1, now() - interval '40 minutes',"
		"  now() - interval '40 minutes', now() - interval '39 minutes', 1),"
		" ('movies.search', 'searches', 'succeeded', 3, 1, now() - interval '30 minutes',"
		"  now() - interval '30 minutes', now() - interval '30 minutes', 1)");
	axis_test_db_exec(d->t->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, created_at, started_at, finished_at,"
		" last_error) VALUES ('import.run', 'downloads', 'failed', 3, 3, now() - interval '25 minutes',"
		" now() - interval '25 minutes', now() - interval '20 minutes',"
		" 'the file''s runtime does not match the film''s (made up for the demo)')");
	axis_test_db_exec(d->t->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, created_at, started_at, lease_token,"
		" lease_expires_at, worker_id, progress, progress_message) VALUES ('library.scan', 'scan', 'running', 3, 1,"
		" now() - interval '2 minutes', now() - interval '2 minutes', gen_random_uuid(), now() + interval '1 hour',"
		" 'demo', 0.42, 'Reading the folders of Movies on Disk two')");
}

static void
scene_full(Demo *d)
{
	static const gchar *const rejected[] = {
		"Copper.Sky.Motel.2023.CAM.x264-NOGRP",
		"Copper.Sky.Motel.2023.2160p.UHD.BluRay.REMUX.HDR.HEVC.TrueHD.7.1-HUGE",
		"Copper.Sky.Moteliers.2019.1080p.WEB-DL.x264-OTHER",
		NULL
	};
	static const gchar *const tidewater_files[] = { "Tidewater.Hollow.2020.1080p.WEB-DL.DDP5.1.H.264-DEMO.mkv",
		"Tidewater.Hollow.2020.1080p.WEB-DL.DDP5.1.H.264-DEMO.en.srt", NULL };
	static const gchar *const glasshouse_files[] = { "The.Glasshouse.Year.2017.1080p.BluRay.x264-DEMO.mkv", NULL };
	static const gchar *const paper_files[] = { "Paper.Moons.Over.Ostend.2015.1080p.BluRay.x264-DEMO.mkv", NULL };
	static const gchar *const ninth_files[] = { "The.Ninth.Lighthouse.2016.720p.WEB-DL.x264-DEMO.mkv", NULL };
	static const gchar *const fennimore_files[] = { "Fennimores.Last.Ride.2013.1080p.WEB-DL.x264-DEMO.mkv", NULL };
	static const gchar *const harrow[] = { "Harrow.Lane.2011.1080p.WEB-DL.DDP5.1.H.264-DEMO", NULL };
	static const gchar *const quiet[] = { "The.Quiet.Engine.2014.1080p.BluRay.x264-DEMO", NULL };
	static const gchar *const kestrel[] = { "Under.the.Kestrel.Moon.2020.1080p.WEB-DL.x264-DEMO", NULL };
	static const gchar *const map[] = { "A.Map.of.Small.Rivers.2019.1080p.WEB-DL.x264-DEMO", NULL };
	static const gchar *const harbour_s3[] = { "Harbour.Lights.S03E04.1080p.WEB-DL.DDP5.1.H.264-DEMO", NULL };
	static const gchar *const thaw[] = { "The.Long.Thaw.S01E01.CAM.x264-NOGRP", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gboolean created = FALSE;
	gint64 movie;
	gint64 harbour;
	gint64 ferry;
	gint64 quarry;
	gint64 thaw_id;
	gint s;
	gint e;

	/* Downloading, at two places. */
	film_downloading(d, 1003, "Wren.Crossing.2018.1080p.WEB-DL.DDP5.1.H.264-DEMO", 37);
	film_downloading(d, 1004, "Ember.Quay.2012.1080p.BluRay.x264-DEMO", 82);

	/* In the library, imported. */
	movie = add_film(d, 1002, d->movies_root);
	film_imported(d, movie, "Tidewater.Hollow.2020.1080p.WEB-DL.DDP5.1.H.264-DEMO", tidewater_files,
		"AXISFAKE duration=6240 width=1920 height=800 vcodec=h264 acodec=eac3 channels=6");
	movie = add_film(d, 1009, d->movies_root);
	film_imported(d, movie, "The.Glasshouse.Year.2017.1080p.BluRay.x264-DEMO", glasshouse_files,
		"AXISFAKE duration=7260 width=1920 height=800 vcodec=h264 acodec=dts channels=6");
	movie = add_film(d, 1010, d->movies_root);
	film_imported(d, movie, "Paper.Moons.Over.Ostend.2015.1080p.BluRay.x264-DEMO", paper_files,
		"AXISFAKE duration=5940 width=1920 height=800 vcodec=h264 acodec=aac channels=2");
	/* Below its profile's cutoff: an upgrade is wanted. */
	movie = add_film(d, 1011, d->movies_root);
	film_imported(d, movie, "The.Ninth.Lighthouse.2016.720p.WEB-DL.x264-DEMO", ninth_files,
		"AXISFAKE duration=6840 width=1280 height=534 vcodec=h264 acodec=aac channels=2");
	/* Blocked: the file runs for half an hour, the film for an hour and three quarters. */
	movie = add_film(d, 1017, d->movies_root);
	film_imported(d, movie, "Fennimores.Last.Ride.2013.1080p.WEB-DL.x264-DEMO", fennimore_files,
		"AXISFAKE duration=1800 width=1920 height=800 vcodec=h264 acodec=aac channels=2");

	/* Arrived, waiting for its import. */
	movie = add_film(d, 1007, d->movies_root);
	set_releases(d, map, 4000000000);
	search_film(d, movie);
	ref = movie_ref(d, movie);
	if (ref != NULL) {
		g_autofree gchar *place = g_build_filename(d->downloads_dir, map[0], NULL);
		g_autofree gchar *file = g_build_filename(place, "a.map.of.small.rivers.mkv", NULL);

		write_media(file, "AXISFAKE duration=5580 width=1920 height=1080 vcodec=h264 acodec=aac channels=2",
			G_GINT64_CONSTANT(4000000000));
		axis_test_download_fake_set(d->fake, ref, "completed", 4000000000, "/data/downloads/A.Map.of.Small.Rivers.2019"
			".1080p.WEB-DL.x264-DEMO", NULL);
		poll_clients(d);
		must(axis_movies_run_follow(d->movies, NULL, &error), error, "the follow-up");
	}

	/* Needs attention: every release found was rejected. */
	movie = add_film(d, 1012, d->movies_root);
	set_releases(d, rejected, 3000000000);
	search_film(d, movie);
	/* And a manual search of it, which lists and grabs nothing. */
	{
		static const gchar *const listed[] = {
			"Copper.Sky.Motel.2023.1080p.WEB-DL.DDP5.1.H.264-DEMO",
			"Copper.Sky.Motel.2023.1080p.BluRay.x264-DEMO",
			"Copper.Sky.Motel.2023.720p.WEB-DL.x264-DEMO",
			"Copper.Sky.Motel.2023.CAM.x264-NOGRP",
			"Copper.Sky.Moteliers.2019.1080p.WEB-DL.x264-OTHER",
			NULL
		};
		AxisMovieSearchReport report;

		set_releases(d, listed, 4500000000);
		memset(&report, 0, sizeof(report));
		if (!axis_movies_run_search(d->movies, movie, "list", ADMIN_NAME, NULL, &report, &error))
			g_warning("demo: the manual search failed: %s", error != NULL ? error->message : "no reason given");
		g_clear_error(&error);
	}
	/* Needs attention: the download failed, and nothing else was found. */
	movie = add_film(d, 1014, d->movies_root);
	set_releases(d, harrow, 5000000000);
	search_film(d, movie);
	g_clear_pointer(&ref, g_free);
	ref = movie_ref(d, movie);
	if (ref != NULL) {
		axis_test_download_fake_set(d->fake, ref, "failed", 1000000000, NULL, "Repair failed, not enough repair blocks");
		poll_clients(d);
		axis_movies_run_follow(d->movies, NULL, NULL);
		axis_test_db_exec(d->t->db, "DELETE FROM job WHERE kind = 'movies.search' AND state = 'queued'");
		search_film(d, movie);
	}
	/* Needs attention: the indexer could not be asked. */
	movie = add_film(d, 1013, d->movies_root);
	set_releases(d, quiet, 9000000000);
	axis_test_prowlarr_set_mode(d->prowlarr, AXIS_TEST_PROWLARR_BROKEN);
	search_film(d, movie);
	axis_test_prowlarr_set_mode(d->prowlarr, AXIS_TEST_PROWLARR_NORMAL);
	/* Needs attention: its destination is on a disk that is away. */
	movie = add_film(d, 1015, d->archive_root);
	set_releases(d, kestrel, 5000000000);
	(void)movie;

	/* Wanted, never searched; searching (its search queued); not out yet. */
	add_film(d, 1005, d->movies_root);
	movie = add_film(d, 1006, d->movies_root);
	axis_movies_request_search(d->movies, movie, "manual", ADMIN_NAME, &created, NULL);
	add_film(d, 1016, d->movies_root);
	add_film(d, 1024, d->movies_root);
	/* Just added from its page with the defaults (its search queued): the
	 * page the add journey ends on. */
	movie = add_film(d, 1022, d->movies_root);
	axis_movies_request_search(d->movies, movie, "manual", ADMIN_NAME, &created, NULL);

	/* Harbour Lights: three seasons, the first in the library, the second
	 * half there, the third airing. */
	harbour = add_series(d, 2001, AXIS_TV_MONITOR_ALL);
	for (e = 1; e <= 8; e++)
		episode_in_library(d, harbour, "Harbour Lights (2024)", 1, e, "bluray_1080p");
	for (e = 1; e <= 4; e++)
		episode_in_library(d, harbour, "Harbour Lights (2024)", 2, e, e == 3 ? "webdl_720p" : "webdl_1080p");
	set_releases(d, harbour_s3, 1500000000);
	search_series(d, harbour, AXIS_TV_SCOPE_EPISODE, episode_of(d, harbour, 3, 4));
	g_clear_pointer(&ref, g_free);
	ref = q(d, "SELECT d.client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.series_id = %"
		G_GINT64_FORMAT " AND g.closed_at IS NULL", harbour);
	if (ref != NULL) {
		axis_test_download_fake_set(d->fake, ref, "downloading", 900000000, NULL, NULL);
		poll_clients(d);
	}
	for (e = 1; e <= 3; e++)
		episode_in_library(d, harbour, "Harbour Lights (2024)", 3, e, "webdl_1080p");
	axis_tv_request_search(d->tv, harbour, AXIS_TV_SCOPE_EPISODE, episode_of(d, harbour, 2, 5), "manual", ADMIN_NAME,
		&created, NULL);

	/* Night Ferry: airing now, the aired episodes wanted. */
	ferry = add_series(d, 2002, AXIS_TV_MONITOR_ALL);
	(void)ferry;
	/* Quarry Bells: ended, every episode in the library. */
	quarry = add_series(d, 2003, AXIS_TV_MONITOR_ALL);
	for (s = 1; s <= 2; s++)
		for (e = 1; e <= 6; e++)
			episode_in_library(d, quarry, "Quarry Bells (2022)", s, e, "webdl_1080p");
	/* The Long Thaw: everything found was rejected. */
	thaw_id = add_series(d, 2004, AXIS_TV_MONITOR_FIRST);
	set_releases(d, thaw, 2000000000);
	search_series(d, thaw_id, AXIS_TV_SCOPE_EPISODE, episode_of(d, thaw_id, 1, 1));
	/* Kestrel Point: added, nothing searched yet. */
	add_series(d, 2005, AXIS_TV_MONITOR_ALL);

	/* The disk that is away, after the film was given its folder there. */
	axis_test_library_unplug(d->t, d->disk3, "disk3");
	axis_test_db_exec(d->t->db, "UPDATE volume SET state = 'offline', state_reason = 'unchecked'"
		" WHERE name = 'USB enclosure'");
	movie = qi(d, "SELECT m.id FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE t.title ="
		" 'Under the Kestrel Moon'");
	if (movie > 0)
		search_film(d, movie);

	/* On disk, not managed: some sure, some held for a person. */
	make_on_disk(d);
	scan_and_identify(d, d->movies2_root);
	scan_and_identify(d, d->tv_root);
	axis_scan_result_free(axis_test_library_scan(d->t, d->movies_root));
	probe_root(d, d->movies_root);

	/* Disk usage and Space savings from what is on the disks now. */
	{
		gint64 snapshot = 0;

		if (!axis_analysis_refresh(d->analysis, "direct", 0, NULL, &snapshot, &error))
			g_warning("demo: no disk usage: %s", error != NULL ? error->message : "no reason given");
		g_clear_error(&error);
		if (!axis_estimator_refresh(d->estimator, "direct", 0, NULL, &snapshot, &error))
			g_warning("demo: no space savings: %s", error != NULL ? error->message : "no reason given");
		g_clear_error(&error);
	}

	/* Leftovers in the client's folder: a folder and a file that no
	 * download and no import accounts for. */
	{
		g_autofree gchar *category = g_build_filename(d->downloads_dir, "axis", NULL);
		g_autofree gchar *old_job = g_build_filename(category, "Brindle.Point.2015.1080p.WEB-DL-MADEUP", NULL);
		g_autofree gchar *old_file = g_build_filename(old_job, "Brindle.Point.2015.1080p.WEB-DL-MADEUP.mkv", NULL);
		g_autofree gchar *loose = g_build_filename(category, "Larkspur.Row.2017.nfo", NULL);

		write_media(old_file, FILM_LINE, G_GINT64_CONSTANT(4200000000));
		g_file_set_contents(loose, "a loose nfo, made up", -1, NULL);
		axis_test_download_fake_set_category_dir(d->fake, "/data/downloads/axis");
	}

	/* Discover's lists, with the states above on their cards. */
	{
		AxisDiscoverReport report;

		memset(&report, 0, sizeof(report));
		if (!axis_discover_run_refresh(d->discover, NULL, TRUE, &report, &error))
			g_warning("demo: Discover's lists were not fetched: %s", error != NULL ? error->message : "no reason");
		g_clear_error(&error);
	}
	make_jobs(d);
}

/* Disks, roots, the indexer and the clients. */
static void
scene_storage(
	Demo     *d,
	gboolean  full
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;
	AxisIndexerSourceSpec source;
	AxisDownloadClientSpec client;

	d->disk1 = axis_test_library_disk(d->t, "disk1");
	d->volume1 = axis_test_library_add_volume(d->t, d->disk1, "Disk one");
	d->movies_root = axis_test_library_add_root(d->t, d->volume1, d->disk1, "Movies", AXIS_MEDIA_MOVIES);
	d->tv_root = axis_test_library_add_root(d->t, d->volume1, d->disk1, "TV", AXIS_MEDIA_TV);
	if (!full)
		return;
	d->downloads_root = axis_test_library_add_root(d->t, d->volume1, d->disk1, "Downloads", AXIS_MEDIA_DOWNLOADS);
	d->downloads_dir = g_build_filename(d->disk1, "Downloads", NULL);
	d->disk2 = axis_test_library_disk(d->t, "disk2");
	d->volume2 = axis_test_library_add_volume_beside(d->t, d->disk2, "Disk two");
	d->movies2_root = axis_test_library_add_root(d->t, d->volume2, d->disk2, "Movies", AXIS_MEDIA_MOVIES);
	d->disk3 = axis_test_library_disk(d->t, "disk3");
	d->volume3 = axis_test_library_add_volume_beside(d->t, d->disk3, "USB enclosure");
	d->archive_root = axis_test_library_add_root(d->t, d->volume3, d->disk3, "Archive", AXIS_MEDIA_MOVIES);

	d->prowlarr = axis_test_prowlarr_new(PROWLARR_KEY, "");
	base = axis_test_prowlarr_base(d->prowlarr);
	memset(&source, 0, sizeof(source));
	source.name = "Prowlarr";
	source.kind = AXIS_INDEXER_PROWLARR_NAME;
	source.address = base;
	source.enabled = TRUE;
	source.secret = PROWLARR_KEY;
	must(axis_indexers_save_source(d->indexers, 0, &source, NULL, &error), error, "the indexer");
	g_clear_pointer(&base, g_free);

	d->fake = axis_test_download_fake_new(CLIENT_PASSWORD);
	base = axis_test_download_fake_base(d->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = CLIENT_PASSWORD;
	must(axis_downloads_save_client(d->downloads, 0, &client, &d->client_id, &error), error, "the Usenet client");
	must(axis_downloads_add_path_map(d->downloads, d->client_id, "/data/downloads", d->volume1, "Downloads", &error),
		error, "the client's folder");
	g_clear_pointer(&base, g_free);

	d->fake_torrent = axis_test_download_fake_new(CLIENT_PASSWORD);
	base = axis_test_download_fake_base(d->fake_torrent);
	memset(&client, 0, sizeof(client));
	client.name = "qBittorrent";
	client.kind = "fixture";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = CLIENT_PASSWORD;
	must(axis_downloads_save_client(d->downloads, 0, &client, &d->torrent_client_id, &error), error,
		"the torrent client");
}

/* ── The pages to look at ───────────────────────────────────────── */

static void
page_line(
	GString     *list,
	const gchar *name,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
page_line(
	GString     *list,
	const gchar *name,
	const gchar *format,
	...
){
	va_list ap;

	g_string_append_printf(list, "%-28s ", name);
	va_start(ap, format);
	g_string_append_vprintf(list, format, ap);
	va_end(ap);
	g_string_append_c(list, '\n');
}

/*
 * The list of pages tests/tools/axis-screenshots.py takes ("NAME PATH [JS]"
 * a line; "@logout" and "@login" between them): every page family, the add
 * journey first, and the images docs/SCREENSHOTS.md asks for ("guide-"),
 * with the addresses of this instance's own titles, imports and jobs.
 */
static gchar *
pages_of(
	Demo        *d,
	const gchar *scene
){
	GString *list = g_string_new("# Written by tests/tools/axis-demo.c for this instance.\n");

	if (strcmp(scene, "setup") == 0) {
		page_line(list, "setup", "/setup");
		page_line(list, "guide-01-setup", "/setup document.querySelector('input[name=setup_token]').value = "
			"'demo-setup-token-made-up'; document.querySelector('input[name=username]').value = 'admin'; "
			"document.querySelectorAll('input[type=password]').forEach(function (e) { "
			"e.value = 'a-made-up-password'; })");
		return g_string_free(list, FALSE);
	}

	g_string_append(list, "@logout\n");
	page_line(list, "login", "/login");
	g_string_append(list, "@login\n");
	page_line(list, "dashboard", "/");
	page_line(list, "attention", "/attention");
	if (strcmp(scene, "empty") == 0) {
		page_line(list, "guide-03-indexers", "/indexers");
		page_line(list, "settings", "/admin/settings");
		page_line(list, "setup-guide", "/setup-guide");
		return g_string_free(list, FALSE);
	}

	/* The add journey: find, recognise, add, follow. */
	page_line(list, "search-results", "/search?q=the");
	page_line(list, "search-lantern", "/search?q=lantern");
	page_line(list, "title-lantern", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1001));
	page_line(list, "title-lantern-add", "/title/%" G_GINT64_FORMAT " document.querySelectorAll('details')"
		".forEach(function (e) { e.open = true; })", title_id(d, AXIS_META_KIND_FILM, 1001));
	page_line(list, "title-lamplight-added", "/title/%" G_GINT64_FORMAT "?done=added",
		title_id(d, AXIS_META_KIND_FILM, 1022));
	page_line(list, "title-velvet", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_SERIES, 2007));
	page_line(list, "title-wren-downloading", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1003));
	page_line(list, "title-copper-attention", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1012));
	page_line(list, "title-harrow-attention", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1014));
	page_line(list, "title-kestrel-offline", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1015));
	page_line(list, "title-brass-wanted", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1005));
	page_line(list, "title-tidewater-library", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1002));
	page_line(list, "title-harbour", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_SERIES, 2001));
	page_line(list, "series-harbour", "/series/%" G_GINT64_FORMAT, qi(d, "SELECT s.id FROM series s JOIN meta_title t"
		" ON t.id = s.title_id WHERE t.title = 'Harbour Lights'"));
	page_line(list, "series-thaw", "/series/%" G_GINT64_FORMAT, qi(d, "SELECT s.id FROM series s JOIN meta_title t"
		" ON t.id = s.title_id WHERE t.title = 'The Long Thaw'"));
	/* Acting on a title: its manual search, a release's confirmation. */
	{
		gint64 copper = qi(d, "SELECT m.id FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE t.title ="
			" 'Copper Sky Motel'");
		gint64 harbour = qi(d, "SELECT s.id FROM series s JOIN meta_title t ON t.id = s.title_id WHERE t.title ="
			" 'Harbour Lights'");

		page_line(list, "manual-movie", "/films/%" G_GINT64_FORMAT "/manual?level=movie", copper);
		page_line(list, "manual-movie-confirm", "/films/%" G_GINT64_FORMAT "/manual/grab?level=movie&candidate=%"
			G_GINT64_FORMAT, copper, qi(d, "SELECT c.id FROM movie_candidate c JOIN movie_search s ON s.id ="
			" c.search_id WHERE s.movie_id = %" G_GINT64_FORMAT " ORDER BY c.search_id DESC, c.position LIMIT 1",
			copper));
		page_line(list, "manual-episode", "/series/%" G_GINT64_FORMAT "/manual?level=episode&item=%" G_GINT64_FORMAT,
			harbour, episode_of(d, harbour, 3, 4));
	}
	page_line(list, "discover-movies", "/discover/movies");
	page_line(list, "discover-tv", "/discover/tv");
	page_line(list, "discover-not-interested", "/discover/not-interested");

	/* Where things stand. */
	/* A list remembers its view: posters again last. */
	page_line(list, "films-overview", "/films?view=overview");
	page_line(list, "films-table", "/films?view=table");
	page_line(list, "films", "/films?view=posters");
	page_line(list, "films-wanted", "/films/wanted");
	page_line(list, "films-history", "/films/history");
	page_line(list, "series-overview", "/series?view=overview");
	page_line(list, "series-table", "/series?view=table");
	page_line(list, "series", "/series?view=posters");
	page_line(list, "series-wanted", "/series/wanted");
	page_line(list, "series-calendar", "/series/calendar");
	page_line(list, "downloads", "/downloads");
	page_line(list, "imports", "/imports");
	page_line(list, "import-blocked", "/imports/%" G_GINT64_FORMAT, qi(d, "SELECT id FROM import WHERE state ="
		" 'blocked' ORDER BY id LIMIT 1"));
	page_line(list, "activity", "/activity");
	page_line(list, "jobs", "/jobs");
	page_line(list, "job-failed", "/jobs/%" G_GINT64_FORMAT, qi(d, "SELECT id FROM job WHERE state = 'failed'"
		" ORDER BY id LIMIT 1"));

	/* Everything else. */
	page_line(list, "identify", "/identify");
	page_line(list, "identify-movies", "/identify/roots/%" G_GINT64_FORMAT "?list=likely", d->movies2_root);
	page_line(list, "identify-tv", "/identify/roots/%" G_GINT64_FORMAT, d->tv_root);
	page_line(list, "library", "/library");
	page_line(list, "storage", "/storage");
	page_line(list, "storage-volume", "/storage/volumes/%" G_GINT64_FORMAT, d->volume1);
	page_line(list, "storage-roots", "/storage/roots");
	page_line(list, "quality", "/quality");
	page_line(list, "naming", "/imports/naming");
	page_line(list, "analysis", "/analysis");
	page_line(list, "savings", "/savings");
	page_line(list, "profiles", "/profiles");
	page_line(list, "compat", "/compat");
	page_line(list, "workers", "/workers");
	page_line(list, "conversions", "/conversions");
	page_line(list, "conversion-runs", "/conversions/runs");
	page_line(list, "indexers", "/indexers");
	page_line(list, "download-clients", "/downloads/clients");
	page_line(list, "leftovers", "/downloads/leftovers");
	page_line(list, "metadata", "/metadata");
	page_line(list, "settings", "/admin/settings");
	page_line(list, "modules", "/admin/modules");
	page_line(list, "network", "/admin/network");
	page_line(list, "users", "/admin/users");
	page_line(list, "proxies", "/admin/proxies");
	page_line(list, "account", "/account");
	page_line(list, "setup-guide", "/setup-guide");

	/* The images docs/SCREENSHOTS.md asks for that this scene can show (the
	 * set-up page is the "setup" scene's, the Indexers page with no indexer
	 * the "empty" scene's). */
	page_line(list, "guide-01-disk-usage", "/analysis");
	page_line(list, "guide-03-download-clients", "/downloads/clients");
	page_line(list, "guide-04-title-lantern", "/title/%" G_GINT64_FORMAT, title_id(d, AXIS_META_KIND_FILM, 1001));
	page_line(list, "guide-05-imports", "/imports");
	page_line(list, "guide-05-leftovers", "/downloads/leftovers");
	page_line(list, "guide-06-identify-tv", "/identify/roots/%" G_GINT64_FORMAT "?list=likely", d->tv_root);
	page_line(list, "guide-07-series", "/series/%" G_GINT64_FORMAT, qi(d, "SELECT s.id FROM series s JOIN meta_title t"
		" ON t.id = s.title_id WHERE t.title = 'Harbour Lights'"));
	page_line(list, "guide-08-savings", "/savings");
	page_line(list, "guide-11-workers", "/workers");
	page_line(list, "guide-12-account", "/account");
	page_line(list, "guide-13-proxies", "/admin/proxies");
	return g_string_free(list, FALSE);
}

/* ── Running ────────────────────────────────────────────────────── */

static gboolean
on_signal(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static void
demo_stop(Demo *d)
{
	htmx_server_stop(d->server);
	axis_web_stop(d->web);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_workers_stop(d->workers_ui);
	axis_web_auth_stop(d->auth);
	axis_web_settings_stop(d->web_settings);
	axis_web_jobs_stop(d->jobs_ui);
	axis_web_storage_stop(d->storage_ui);
	axis_web_library_stop(d->library_ui);
	axis_web_probe_stop(d->probe_ui);
	axis_web_browser_stop(d->browser_ui);
	axis_web_analysis_stop(d->analysis_ui);
	axis_web_savings_stop(d->savings_ui);
	axis_web_compat_stop(d->compat_ui);
	axis_web_profiles_stop(d->profiles_ui);
	axis_web_conversions_stop(d->conversions_ui);
	axis_web_meta_stop(d->meta_ui);
	axis_web_downloads_stop(d->downloads_ui);
	axis_web_imports_stop(d->imports_ui);
	axis_web_indexers_stop(d->indexers_ui);
	axis_web_quality_stop(d->quality_ui);
	axis_web_identify_stop(d->identify_ui);
	axis_web_setup_stop(d->setup_ui);
	axis_web_discover_stop(d->discover_ui);
	axis_movies_stop(d->movies);
	axis_tv_stop(d->tv);
	axis_module_registry_stop_all(d->registry);
	axis_meta_stop(d->meta);
	axis_downloads_stop(d->downloads);
	axis_indexers_stop(d->indexers);
	while (g_main_context_iteration(NULL, FALSE))
		;
	/* The rest goes with the process; the database and the scratch
	 * directory are removed. */
	axis_test_library_free(d->t);
}

int
main(int argc, char **argv)
{
	gint port = 0;
	gchar *scene = NULL;
	gchar *ready_file = NULL;
	gchar *pages_file = NULL;
	gint notices = 0;
	GOptionEntry entries[] = {
		{ "port", 0, 0, G_OPTION_ARG_INT, &port, "Serve on 127.0.0.1 at PORT", "PORT" },
		{ "attention", 0, 0, G_OPTION_ARG_INT, &notices,
		  "N made-up notices on the page Needs attention, beside the scene's own (0 to 500)", "N" },
		{ "scene", 0, 0, G_OPTION_ARG_STRING, &scene, "full (the default), empty or setup", "SCENE" },
		{ "ready-file", 0, 0, G_OPTION_ARG_FILENAME, &ready_file, "Write the address and accounts to FILE", "FILE" },
		{ "pages-file", 0, 0, G_OPTION_ARG_FILENAME, &pages_file,
		  "Write the pages to look at, for tests/tools/axis-screenshots.py, to FILE", "FILE" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};
	g_autoptr(GOptionContext) context = g_option_context_new("- a demo instance of Axis with made-up titles");
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *ready = NULL;
	Demo demo;
	Demo *d = &demo;

	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("axis-demo: %s\n", error->message);
		return 2;
	}
	if (port <= 0 || port > 65535) {
		g_printerr("axis-demo: --port is needed (a free port, such as one above 20000)\n");
		return 2;
	}
	if (scene == NULL)
		scene = g_strdup("full");
	if (strcmp(scene, "full") != 0 && strcmp(scene, "empty") != 0 && strcmp(scene, "setup") != 0) {
		g_printerr("axis-demo: --scene is full, empty or setup\n");
		return 2;
	}
	if (g_getenv("AXIS_TEST_DATABASE_URL") == NULL) {
		g_printerr("axis-demo: run it under tests/with-postgres.sh, which starts the database it needs\n");
		return 2;
	}
	if (!g_file_test(FAKE_PROBE, G_FILE_TEST_EXISTS)) {
		g_printerr("axis-demo: run it from the top of the repository (%s is read from there)\n", FAKE_PROBE);
		return 2;
	}

	crc_init();
	if (!axis_password_init()) {
		g_printerr("axis-demo: libsodium cannot be used\n");
		return 1;
	}
	axis_password_use_test_cost();
	memset(d, 0, sizeof(*d));
	d->notices = CLAMP(notices, 0, 500);
	d->t = axis_test_library_new(32);
	if (d->t == NULL) {
		g_printerr("axis-demo: no database (AXIS_SKIP_DB_TESTS is set?)\n");
		return 1;
	}
	axis_settings_set(d->t->settings, "storage.read_only", "false", NULL);
	demo_build(d);
	must(axis_settings_set(d->settings, AXIS_META_TMDB_SETTING_KEY, TMDB_TOKEN, &error), error, "the TMDb token");
	axis_settings_set(d->settings, "storage.read_only", "false", NULL);

	if (strcmp(scene, "setup") != 0)
		scene_storage(d, strcmp(scene, "full") == 0);
	if (strcmp(scene, "full") == 0)
		scene_full(d);
	/* The header counts what the scene left running (the queue that would
	 * count it again does not run here). */
	must(axis_web_jobs_count_now(d->jobs_ui, &error), error, "the header's counts");
	if (pages_file != NULL) {
		g_autofree gchar *pages = pages_of(d, scene);

		if (!g_file_set_contents(pages_file, pages, -1, &error))
			g_printerr("axis-demo: cannot write %s: %s\n", pages_file, error->message);
		g_clear_error(&error);
	}
	demo_serve(d, (guint16)port, scene);

	ready = g_strdup_printf("url=http://127.0.0.1:%d\nscene=%s\nadmin=%s\nadmin_password=%s\nviewer=%s\n"
		"viewer_password=%s\n", port, scene, ADMIN_NAME, ADMIN_PASSWORD, VIEWER_NAME, VIEWER_PASSWORD);
	if (ready_file != NULL && !g_file_set_contents(ready_file, ready, -1, &error))
		g_printerr("axis-demo: cannot write %s: %s\n", ready_file, error->message);
	g_print("axis-demo: serving\n%s", ready);

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_signal, loop);
	g_unix_signal_add(SIGTERM, on_signal, loop);
	g_main_loop_run(loop);

	g_print("axis-demo: stopping\n");
	demo_stop(d);
	fake_tmdb_free(d->tmdb);
	if (d->prowlarr != NULL)
		axis_test_prowlarr_free(d->prowlarr);
	if (d->fake != NULL)
		axis_test_download_fake_free(d->fake);
	if (d->fake_torrent != NULL)
		axis_test_download_fake_free(d->fake_torrent);
	g_free(scene);
	g_free(ready_file);
	g_free(pages_file);
	return 0;
}
