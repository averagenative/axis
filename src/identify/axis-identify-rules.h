/*
 * axis-identify-rules.h - what identification reads from names, how a
 * folder's videos make units, how evidence becomes a confidence, and the
 * quality of a file found in the library
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Pure: no database, no I/O, no clock.  The job (axis-identify-run.c) gathers
 * the evidence (names, identifiers, a sidecar .nfo, the metadata layer's
 * answers, the probe's duration) and these functions decide.
 *
 * THE CONFIDENCE is one of three, and only "sure" is linked without a
 * person.  Sure is exactly one of:
 *
 *  1. a file of the unit was placed by an import of Axis (that link wins);
 *  2. an identifier in the folder or a file name names one film, and the
 *     names do not clearly disagree with it (a different title AND a year
 *     more than one off);
 *  3. an identifier in the sidecar .nfo names one film, no name carries an
 *     identifier, the names do not clearly disagree with it, and at least
 *     one name (the folder's or the file's) reads as a title that fits it
 *     (its title, original title or one of its other names, by comparison
 *     key): an .nfo whose names give nothing to check it against (a generic
 *     folder, "movie.mkv", a hash) is at most likely;
 *  4. the names give a title and a year, read from the folder and the file
 *     agreeing (or from a file standing alone: at the top of the root, or one
 *     of several films in one folder; or from one name alone, the folder's or
 *     the file's, when the unit is one video, and from a series' folder whose
 *     episode files name no series), and of the provider's titles of that
 *     name (title, original title or other name, by comparison key, each
 *     judged by its full record) exactly one has the year exactly and a
 *     runtime that fits the main video's probed duration within the
 *     tolerance; no other title of the name has the year exactly with a
 *     runtime that fits or is not known; every title of the name a year off
 *     has a runtime known that does not fit (by the tolerance, never the
 *     wider band: a release's year may be its festival's); and at most
 *     %AXIS_IDENTIFY_DETAILS titles have that name (all of them read in
 *     full).  The year is exact by its first release, or by its theatrical
 *     release in the region (within two years of the first) only when no
 *     other title of the name was first released within a year of the
 *     names' year; else that is as good as a year off.  A title with no
 *     year known does not stand in the way.  A title that has the name only
 *     as another name, found among the answers read in full for their other
 *     names when not every answer was, is at most likely.  A film that is
 *     the only title of its name among the answers, with the year of its
 *     first release, named so by the folder and the file agreeing (or a file
 *     standing alone), is sure with its runtime within the wider band too
 *     (axis_identify_runtime_loose(): a different cut, or a runtime the
 *     provider has wrong).
 *
 * A series' folder whose names give no year (series are named so: a year
 * only tells two of one name apart) is sure by its episodes instead of the
 * year (rule 5):
 *
 *  5. the names give a title, read from the folder (its files agreeing, or
 *     giving no title of their own), and the provider's series whose title,
 *     original title or other name has that comparison key are at most
 *     %AXIS_IDENTIFY_SERIES_COMPARED; of them exactly one fits the episodes
 *     on disk (axis_identify_shape_fit(): at least
 *     %AXIS_IDENTIFY_SHAPE_MIN_EPISODES numbered episodes in as many files,
 *     none in a season it does not have, and at least
 *     %AXIS_IDENTIFY_SHAPE_SHARE percent of them episodes it has) without
 *     the main video's probed duration contradicting its episode's runtime;
 *     no other of them is one the episodes cannot be checked against (the
 *     provider lists no seasons, or no episode counts, for it); and the main
 *     video is probed.  Two that fit are never chosen between.  The series
 *     compared are those with the name as their title or original title and
 *     those of the first %AXIS_IDENTIFY_OTHER_NAME_LOOKS answers that have it
 *     as another name, together.
 *
 * Everything else is likely (a single candidate with something missing or
 * off: no year, a year one or two off (the nearest, by its full record: no
 * title of the name has the year or one a year off), the runtime off or not
 * known, no probe yet,
 * only the folder or only the file reads as a title in a unit of several
 * videos, identifiers in conflict or naming nothing, names that disagree
 * with an identifier; two titles of the name that the year and the runtime
 * do not tell apart; a series' folder with too few numbered episodes, or
 * that fits none or several series of its name, or one of whose series
 * cannot be checked) or unknown (no candidate, or nothing reads as a title;
 * a film's name whose title holds an episode's or a special's number says
 * it looks like part of a series).
 * A unit whose decision needs a full record the provider did not answer for
 * is none of these: it waits ("could not tell yet", the verdict's
 * @waiting), and nothing final is stored for it.  A unit of a folder with
 * more videos than are read says so, and the folder's own unit is at most
 * likely.  The reasons are kept, in words; every verdict of a series' unit
 * ends with what its files hold (the reason "files").  Popularity and the
 * order of the provider's answers never decide.
 *
 * GLib only: the name readers are fuzzed by tests/fuzz/fuzz-identify-name.c.
 */

#ifndef AXIS_IDENTIFY_RULES_H
#define AXIS_IDENTIFY_RULES_H

#include <glib.h>

#include "identify/axis-identify-ids.h"
#include "meta/axis-meta-model.h"
#include "quality/axis-quality.h"
#include "release/axis-release.h"

G_BEGIN_DECLS

/* At most this many candidates are kept for a unit. */
#define AXIS_IDENTIFY_MAX_CANDIDATES 8

/* The runtime tolerance by default: the larger of these. */
#define AXIS_IDENTIFY_DEFAULT_PERCENT 10
#define AXIS_IDENTIFY_DEFAULT_MINUTES 5

/*
 * The version of these rules, stored with every verdict
 * (library_identity.rules): a unit no person decided, held for review or
 * linked by rule 5, whose verdict an older version made, is looked at again
 * once.  Raise it with every change that can change a verdict.
 *   1  before fix-identify-hunt-findings
 *   2  same-name titles judged by their full records, the year by the full
 *      record, a full record not answered waits, other names of series
 *      compared together, a series that cannot be checked holds, one name
 *      enough for one video, the wider runtime band
 *   3  fix-identify-regional-years: a title of the name read in full
 *      whatever year its search answer gives, one two years off likely, a
 *      folder's "<something> (<year>)" always a title (units held for review
 *      only: a link made by rule 5 was looked at again for version 2)
 */
#define AXIS_IDENTIFY_RULES_VERSION 3

/* Units linked by rule 5 under rules older than this are looked at again
 * (once); the versions after it did not change rule 5. */
#define AXIS_IDENTIFY_RULES_SERIES_FIT 2

/* At most this many titles of the name read are read in full for a unit
 * (their years and runtimes); a name more have is not sure. */
#define AXIS_IDENTIFY_DETAILS 5

/* When no answer of a search has the name read as its title, at most this
 * many of a page's first answers are read in full for their other names. */
#define AXIS_IDENTIFY_OTHER_NAME_LOOKS 3

/* A page of the provider's search that holds this many answers is full:
 * only then, and when nothing on it has the name, is the second asked for. */
#define AXIS_IDENTIFY_SEARCH_PAGE 20

/*
 * The wider runtime band (axis_identify_runtime_loose()): the larger of
 * these either way; with an edition word in the names (Extended, Director's
 * Cut, Unrated...), up to AXIS_IDENTIFY_EDITION_PERCENT longer.
 */
#define AXIS_IDENTIFY_LOOSE_PERCENT   25
#define AXIS_IDENTIFY_LOOSE_MINUTES   15
#define AXIS_IDENTIFY_EDITION_PERCENT 60

/* The edition words that say a cut runs longer than the film as released
 * (#AxisReleaseEdition flags): Extended, Director's Cut, Unrated, Uncut,
 * Final Cut, Ultimate, Special Edition and Alternate.  Not the others: a
 * theatrical cut, a remaster, a restoration, IMAX, open matte, Criterion, a
 * collector's or anniversary edition run as the film does, and a bare
 * "Edition", "Cut", "Version" or "Collection" says nothing of its length. */
#define AXIS_IDENTIFY_EDITION_LONGER (AXIS_RELEASE_EDITION_EXTENDED | AXIS_RELEASE_EDITION_DIRECTORS | \
	AXIS_RELEASE_EDITION_UNRATED | AXIS_RELEASE_EDITION_UNCUT | AXIS_RELEASE_EDITION_FINAL_CUT | \
	AXIS_RELEASE_EDITION_ULTIMATE | AXIS_RELEASE_EDITION_SPECIAL | AXIS_RELEASE_EDITION_ALTERNATE)

/*
 * A title of the name whose stored year (its full record's when one was
 * read, else its search answer's) is further than this from the names' year
 * is out of reach: its first release and its theatrical one in the region
 * are within two years of each other, so by that year it can be neither the
 * one nor in its way.  A search answer's year alone does not settle it
 * (fix-identify-regional-years): a title whose own title has the name is
 * read in full all the same, after those within reach and within the
 * AXIS_IDENTIFY_DETAILS read, and judged by its record; only one left
 * unread, or whose record is not answered while another title of the name
 * is within reach, is judged as stored, and the folder does not wait for
 * it.  Out of reach, a title is never read for its other names.
 */
#define AXIS_IDENTIFY_YEAR_REACH 3

typedef enum
{
	AXIS_IDENTIFY_SURE = 0,
	AXIS_IDENTIFY_LIKELY,
	AXIS_IDENTIFY_UNKNOWN
} AxisIdentifyConfidence;

/* "sure", "likely", "unknown" (stored). */
const gchar *
axis_identify_confidence_name(AxisIdentifyConfidence confidence);

gboolean
axis_identify_confidence_from_name(
	const gchar            *name,
	AxisIdentifyConfidence *confidence
);

/* "Sure", "Likely", "Unknown". */
const gchar *
axis_identify_confidence_label(AxisIdentifyConfidence confidence);

/* ── What a name says ───────────────────────────────────────────── */

/**
 * AxisIdentifyName:
 * @shown: the name as read (its display form)
 * @release: what the release parser read from it, its identifiers taken out
 * @key: the title's comparison key (axis_release_title_key()), or %NULL
 *   when the name gives no title to look for (none, a hash-like string, a
 *   generic word such as "movie" or "CD1")
 * @year: the year, 0 for none
 * @ids: the identifiers in it
 */
typedef struct
{
	gchar           *shown;
	AxisRelease     *release;
	gchar           *key;
	gint             year;
	AxisIdentifyIds  ids;
} AxisIdentifyName;

/**
 * axis_identify_name_read:
 * @name: (nullable): a folder's or a file's name (one segment), shown form
 * @latest_year: the latest year believed (0: next year)
 *
 * Returns: (transfer full): what it says; never %NULL
 */
AxisIdentifyName *
axis_identify_name_read(
	const gchar *name,
	gint         latest_year
);

void
axis_identify_name_free(AxisIdentifyName *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIdentifyName, axis_identify_name_free)

/* "Hearth (1995)", "Hearth, no year", "nothing that reads as a title". */
gchar *
axis_identify_name_describe(const AxisIdentifyName *name);

/* ── Units of a folder ──────────────────────────────────────────── */

/**
 * AxisIdentifyGroup:
 * @folder: the group is the folder's (the folder's name is evidence for it)
 * @members: (element-type guint): indices into the files given, in order
 */
typedef struct
{
	gboolean  folder;
	GArray   *members;
} AxisIdentifyGroup;

void
axis_identify_group_free(AxisIdentifyGroup *group);

/**
 * axis_identify_group:
 * @folder: (nullable): the folder's name, %NULL for files at the top of a
 *   root (each is then a group of its own)
 * @files: (array length=n): the names of the folder's videos (samples and
 *   extras left out)
 * @n: how many
 *
 * The units of a folder.  One video, or videos that all read as one title
 * (or as nothing), are one unit: the folder's.  Videos that read as
 * different titles (a different title, or the same title with years more
 * than one apart) are a unit each, per title; the ones that read as the
 * folder's own title, with those that read as nothing, stay the folder's.
 * A video reading as no year joins the one group of its title, when there
 * is just one and the folder's name gives that title with its year (one
 * off allowed); otherwise it is a unit of its own ("Title.mkv" beside
 * "Title (1984).mkv" in a folder without a year may be another film).
 *
 * Returns: (transfer full) (element-type AxisIdentifyGroup): the groups, the
 *   folder's first
 */
GPtrArray *
axis_identify_group(
	const AxisIdentifyName        *folder,
	const AxisIdentifyName *const *files,
	guint                          n
);

/* ── What a unit's names read as ────────────────────────────────── */

typedef enum
{
	AXIS_IDENTIFY_READ_NONE = 0,	/* nothing reads as a title */
	AXIS_IDENTIFY_READ_AGREE,	/* the folder and the file agree */
	AXIS_IDENTIFY_READ_FOLDER,	/* only the folder reads as a title */
	AXIS_IDENTIFY_READ_FILE_ONLY,	/* a folder's: only the file reads as one */
	AXIS_IDENTIFY_READ_ALONE,	/* a file standing alone */
	AXIS_IDENTIFY_READ_DISAGREE	/* the folder and the file read differently */
} AxisIdentifyReadKind;

/**
 * AxisIdentifyReading:
 * @primary: (nullable): the name to look for (the one with a year when both
 *   agree); for a disagreement, the folder's
 * @secondary: (nullable): for a disagreement, the file's
 * @year: the year read, 0 for none
 */
typedef struct
{
	AxisIdentifyReadKind    kind;
	const AxisIdentifyName *primary;
	const AxisIdentifyName *secondary;
	gint                    year;
} AxisIdentifyReading;

/* @folder: (nullable) for a file that stands alone. */
void
axis_identify_read(
	const AxisIdentifyName *folder,
	const AxisIdentifyName *file,
	AxisIdentifyReading    *reading
);

/* "title and year from the folder and the file", ... */
const gchar *
axis_identify_read_label(AxisIdentifyReadKind kind);

/**
 * axis_identify_title_fits:
 *
 * Whether @name's title is @title's (its title, original title or one of its
 * other names, by comparison key), whatever the year.
 */
gboolean
axis_identify_title_fits(
	const AxisIdentifyName *name,
	const AxisMetaTitle    *title
);

/**
 * axis_identify_named_order:
 * @name: what the names read as
 * @found: (array length=n): the provider's answers (stored records)
 * @n: how many
 * @order: (out caller-allocates) (array length=n): indices into @found
 *
 * The answers that have @name's title (axis_identify_title_fits()), the
 * nearest to @name's year first (by the nearest of each one's years, one
 * with no year known last, the provider's order among equals): the order
 * in which they are read in full, at most %AXIS_IDENTIFY_DETAILS of them.
 * The order chooses what is read, never what is taken.
 *
 * Returns: how many
 */
guint
axis_identify_named_order(
	const AxisIdentifyName     *name,
	const AxisMetaTitle *const *found,
	guint                       n,
	guint                      *order
);

/**
 * axis_identify_clearly_disagrees:
 *
 * Whether the names clearly say another film than @title: every name that
 * reads as a title has another title than @title's and a year more than one
 * away from @title's.  Names that read as nothing never disagree.
 */
gboolean
axis_identify_clearly_disagrees(
	const AxisIdentifyReading *reading,
	const AxisMetaTitle       *title
);

/* ── Runtime ────────────────────────────────────────────────────── */

/**
 * axis_identify_runtime_fits:
 * @duration_ms: the file's probed duration
 * @runtime_minutes: the title's runtime
 * @percent: the tolerance in percent of the runtime
 * @minutes: the tolerance in minutes; the larger of the two counts
 * @off_minutes: (out) (optional): how far off, in whole minutes, rounded
 *
 * Returns: %TRUE when both are known and the difference is within the
 *   tolerance
 */
gboolean
axis_identify_runtime_fits(
	gint64  duration_ms,
	gint    runtime_minutes,
	gint    percent,
	gint    minutes,
	gint   *off_minutes
);

/**
 * axis_identify_runtime_loose:
 * @duration_ms: the file's probed duration
 * @runtime_minutes: the title's runtime
 * @edition: the names state an edition that runs longer
 *   (%AXIS_IDENTIFY_EDITION_LONGER)
 *
 * The wider band: within %AXIS_IDENTIFY_LOOSE_PERCENT percent of the
 * runtime, or %AXIS_IDENTIFY_LOOSE_MINUTES minutes, whichever is larger,
 * either way; with @edition, up to %AXIS_IDENTIFY_EDITION_PERCENT percent
 * longer.
 *
 * Returns: %TRUE when both are known and the file is within it
 */
gboolean
axis_identify_runtime_loose(
	gint64   duration_ms,
	gint     runtime_minutes,
	gboolean edition
);

/**
 * axis_identify_title_years:
 * @title: (nullable): a title's record
 * @years: (out caller-allocates) (array fixed-size=3): its release years
 *
 * The years @title's record gives, each once: first its year (its first
 * release; the year of its release or first air date when it has none),
 * then the year of its theatrical release in the region when its full
 * record gives one (@regional_year: within two years of the first).  A
 * disc or a stream in the region is no year of it.
 *
 * Returns: how many (0 to 2)
 */
guint
axis_identify_title_years(
	const AxisMetaTitle *title,
	gint                 years[3]
);

/**
 * axis_identify_series_mark:
 * @title: (nullable): a title as the names read it (words joined by single
 *   spaces)
 * @mark: (out) (optional) (transfer full): the mark, as written
 * @after: (out) (optional) (transfer full): the words after it, "" for none
 *
 * Whether a word of @title after its first is an episode's or a special's
 * number ("S05E01", "S05M01": a season, any letter and a number; "5x01"),
 * which the release parser left in the title: the name looks like part of
 * a series.  Nothing is read from the mark; it is only said.
 *
 * Returns: %TRUE when there is one (*@mark and *@after are then set)
 */
gboolean
axis_identify_series_mark(
	const gchar  *title,
	gchar       **mark,
	gchar       **after
);

/**
 * axis_identify_search_words:
 * @kind: what the unit is looked for as
 * @read_title: (nullable): the title a unit's names read as
 *
 * What a person's search for the unit starts from: for a film, the words
 * after a series' mark (axis_identify_series_mark()) when there are any;
 * else, and for a series (whose name comes before its numbers),
 * @read_title.
 *
 * Returns: (transfer full) (nullable): the words
 */
gchar *
axis_identify_search_words(
	AxisMetaKind  kind,
	const gchar  *read_title
);

/* ── A series' folder and its episode files ─────────────────────── */

/**
 * axis_identify_season_folder:
 * @name: (nullable): a folder's name (one segment, shown form)
 *
 * The season a season folder is for: "Season 1", "Season 01", "Season1",
 * "S01", "S1", "Series 2" (each ignoring case), "Specials" and "Special"
 * (season 0); spaces around are ignored.  Anything else is no season folder.
 *
 * Returns: the season (0 to 10000), or -1
 */
gint
axis_identify_season_folder(const gchar *name);

/* At most this many episodes are kept of one file's name. */
#define AXIS_IDENTIFY_FILE_EPISODES 200

/**
 * AxisIdentifyEpisodes:
 * @code: %NULL when the name holds episodes (by numbers, or by a date);
 *   else why it holds none: "no_episode" (no season and episode),
 *   "absolute_only" (an absolute number alone, or a season and one),
 *   "cross_season" (episodes numbered across seasons), "season_pack" (a
 *   whole season or several, or Complete), "season_folder" (a season other
 *   than its season folder's)
 * @text: what it holds ("S01E02-E03", "dated 2024-03-15") or the reason, a
 *   sentence
 * @date: a daily show's air date ("YYYY-MM-DD"), "" for numbers
 * @season: the season of @numbers
 * @numbers: (array length=n): the episodes, in the order stated, each once
 */
typedef struct
{
	const gchar *code;
	gchar       *text;
	gchar        date[11];
	gint         season;
	guint        n;
	gint         numbers[AXIS_IDENTIFY_FILE_EPISODES];
} AxisIdentifyEpisodes;

void
axis_identify_episodes_clear(AxisIdentifyEpisodes *episodes);

/**
 * axis_identify_episodes_read:
 * @file: what the release parser read from the file's own name
 * @folder_season: the season of the season folder the file is in
 *   (axis_identify_season_folder()), -1 for none (the series' folder itself,
 *   or a folder that is no season's)
 * @out: (out caller-allocates): what it holds
 *
 * What an episode file's name says it holds, by axis_release_states() (the
 * reading the TV module's matcher uses), and nothing guessed: a date is a
 * date; episodes are those of one season; a season folder that names
 * another season than the file is a disagreement, never resolved.  Which
 * episode of the series' data that is, is read against the metadata when
 * it is shown.
 */
void
axis_identify_episodes_read(
	const AxisRelease    *file,
	gint                  folder_season,
	AxisIdentifyEpisodes *out
);

/**
 * axis_identify_series_name:
 * @files: (array length=n): what each video's name says
 *
 * Which file's name stands for a series' episode files beside its folder's
 * name: of the names that read as episodes and as a title, one with the
 * title most of them share (the first such, by order).
 *
 * Returns: its index, or -1 when none reads as an episode of a title
 */
gint
axis_identify_series_name(
	const AxisIdentifyName *const *files,
	guint                          n
);

/* ── A series' episodes on disk against a series' seasons ────────── */

/*
 * A series' folder with no year in its names is sure by its episodes only
 * when at least this many numbered episodes of regular seasons are on disk,
 * in as many files (fewer say little: any series has a season 1 with a
 * first episode), and at least this share of them, in percent, are episodes
 * the series has (a tenth may follow another numbering, as a TVDB-named
 * file against TMDb's data often does; below ten episodes that is none).
 * At most this many series of one name are compared; a folder whose name
 * more have is held for a person.
 */
#define AXIS_IDENTIFY_SHAPE_MIN_EPISODES 3
#define AXIS_IDENTIFY_SHAPE_SHARE        90
#define AXIS_IDENTIFY_SERIES_COMPARED    4

/**
 * AxisIdentifyShape:
 * @n_files: the videos read
 * @n_numbered: of those, the ones that hold episodes of a regular season
 *   (1 or more) by their numbers
 * @n_specials: the ones that hold specials (season 0)
 * @n_dated: the ones that hold a daily show's date
 * @n_none: the ones that hold no episode
 * @episodes: (element-type gint64): the regular episodes they hold, each
 *   once, as season * %AXIS_IDENTIFY_SHAPE_PACK + number, ascending (after
 *   axis_identify_shape_finish())
 *
 * What a series' episode files hold, all together.
 */
#define AXIS_IDENTIFY_SHAPE_PACK G_GINT64_CONSTANT(1000000)

typedef struct
{
	guint   n_files;
	guint   n_numbered;
	guint   n_specials;
	guint   n_dated;
	guint   n_none;
	GArray *episodes;
} AxisIdentifyShape;

void
axis_identify_shape_init(AxisIdentifyShape *shape);

/* One video's reading (axis_identify_episodes_read()), added. */
void
axis_identify_shape_add(
	AxisIdentifyShape          *shape,
	const AxisIdentifyEpisodes *episodes
);

/* Sorted, each episode once: call after the last add. */
void
axis_identify_shape_finish(AxisIdentifyShape *shape);

void
axis_identify_shape_clear(AxisIdentifyShape *shape);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(AxisIdentifyShape, axis_identify_shape_clear)

/* "38 numbered episodes of seasons 1 to 3; 2 more files are specials; 1
 * holds no episode", "no numbered episode". */
gchar *
axis_identify_shape_describe(const AxisIdentifyShape *shape);

/**
 * AxisIdentifyFit:
 * @listed: the series lists its seasons (else nothing is checked)
 * @checked: the episodes on disk in a season it has whose episode count is
 *   known
 * @landed: of those, the ones numbered within that count
 * @unchecked: the ones in a season it has with no episode count known
 * @missing: the ones in a season it does not have
 * @missing_season: the first such season, -1 for none
 * @off_season: @off_number: the first episode checked that it does not
 *   have, -1 for none
 * @fits: @listed, nothing @missing, @checked at least
 *   %AXIS_IDENTIFY_SHAPE_MIN_EPISODES, and @landed at least
 *   %AXIS_IDENTIFY_SHAPE_SHARE percent of @checked
 */
typedef struct
{
	gboolean listed;
	guint    checked;
	guint    landed;
	guint    unchecked;
	guint    missing;
	gint     missing_season;
	gint     off_season;
	gint     off_number;
	gboolean fits;
} AxisIdentifyFit;

/**
 * axis_identify_shape_fit:
 * @shape: the episodes on disk (finished)
 * @series: a series' full record, with its seasons and their episode counts
 * @fit: (out caller-allocates)
 *
 * Whether the episodes on disk fit @series' seasons.  Specials and dated
 * files count neither way (a provider's specials are numbered as it likes).
 */
void
axis_identify_shape_fit(
	const AxisIdentifyShape *shape,
	const AxisMetaTitle     *series,
	AxisIdentifyFit         *fit
);

/**
 * AxisIdentifySeriesCandidate:
 * @title: a series of the name read, its full record (with its seasons)
 * @episode_runtime: the runtime of the episode the main video's name holds,
 *   as that series' data has it (0: not known)
 * @other_name: the name read is one of its other names, not its title
 */
typedef struct
{
	const AxisMetaTitle *title;
	gint                 episode_runtime;
	gboolean             other_name;
} AxisIdentifySeriesCandidate;

/* ── The decision ───────────────────────────────────────────────── */

typedef enum
{
	AXIS_IDENTIFY_ID_NONE = 0,	/* no identifier */
	AXIS_IDENTIFY_ID_FOUND,		/* it names @title */
	AXIS_IDENTIFY_ID_NOT_FOUND,	/* the provider knows no film of it */
	AXIS_IDENTIFY_ID_CONFLICT	/* two different identifiers */
} AxisIdentifyIdState;

/**
 * AxisIdentifyIdEvidence:
 * @where: "the folder name", "the file name", "the .nfo file"
 * @token: the identifier in words ("tmdb 6003"), for the reasons
 * @title: (nullable): the film it names, its full record when it could be
 *   read (with its other names)
 */
typedef struct
{
	AxisIdentifyIdState  state;
	const gchar         *where;
	const gchar         *token;
	const AxisMetaTitle *title;
} AxisIdentifyIdEvidence;

/**
 * AxisIdentifyEvidence:
 * @kind: what the unit is looked for as: %AXIS_META_KIND_FILM (also 0) in a
 *   root of movies, %AXIS_META_KIND_SERIES in a root for TV.  The rules are
 *   the same; the words name a movie or a series.
 * @imported: (nullable): the film an import of Axis placed a file of this
 *   unit for
 * @name_id: an identifier in the names
 * @nfo_id: an identifier in a sidecar .nfo
 * @reading: what the names read as
 * @found: (array length=n_found): the provider's titles for the reading
 *   (both readings when they disagree), stored records
 * @detail: (nullable): the full record of one title of @found (by its id),
 *   for its years, runtime and other names; @episode_runtime is its
 *   episode's
 * @duration_ms: the main video's probed duration, 0 when not probed
 * @episode_runtime: a series': the runtime of the episode the main video's
 *   name holds, as the series' data has it (0: not known; then the series'
 *   own runtime, when it gives one), of @detail's series (or of every
 *   series without one of @series)
 * @percent: @minutes: the runtime tolerance
 * @shape: (nullable): a series': what its episode files hold (those whose
 *   own names state another title left out); %NULL for a film
 * @series: (array length=n_series): a series' whose names give no year: the
 *   provider's series of the name read (rule 5), at most
 *   %AXIS_IDENTIFY_SERIES_COMPARED, each with its full record; with a year,
 *   the episode runtimes of the series of the name whose year is the name's
 * @series_more: more series have that name than are in @series
 * @details: (array length=n_details): full records of titles of @found (by
 *   id), read for the decision: of the titles of the name read, at most
 *   %AXIS_IDENTIFY_DETAILS, and of the first answers read for their other
 *   names; a title of @found without one is judged by its stored record
 * @details_more: more titles have the name read than were read in full
 * @untold: (nullable): a title whose full record the decision needs and the
 *   provider did not answer for: the verdict then waits
 * @untold_problem: (nullable): why it did not answer, in words
 * @n_videos: the unit's videos (0: not said, as several)
 * @editions: the editions its folder's and main video's names state
 *   (#AxisReleaseEdition flags)
 * @not_read: the videos of its folder beyond the most read of one folder,
 *   which are in no unit
 * @others_partial: the answers were read in full for their other names,
 *   and not every one of them (a title that has the name only as another
 *   name may not be the only one): such a title is at most likely
 */
typedef struct
{
	AxisMetaKind                       kind;
	gint                               episode_runtime;
	const AxisMetaTitle               *imported;
	AxisIdentifyIdEvidence             name_id;
	AxisIdentifyIdEvidence             nfo_id;
	AxisIdentifyReading                reading;
	const AxisMetaTitle *const        *found;
	guint                              n_found;
	const AxisMetaTitle               *detail;
	gint64                             duration_ms;
	gint                               percent;
	gint                               minutes;
	const AxisIdentifyShape           *shape;
	const AxisIdentifySeriesCandidate *series;
	guint                              n_series;
	gboolean                           series_more;
	const AxisMetaTitle *const        *details;
	guint                              n_details;
	gboolean                           details_more;
	const AxisMetaTitle               *untold;
	const gchar                       *untold_problem;
	guint                              n_videos;
	guint                              editions;
	guint                              not_read;
	gboolean                           others_partial;
} AxisIdentifyEvidence;

/**
 * AxisIdentifyReason:
 * @code: a stable identifier ("id_folder", "runtime_off", ...)
 * @text: the sentence
 */
typedef struct
{
	gchar *code;
	gchar *text;
} AxisIdentifyReason;

void
axis_identify_reason_free(AxisIdentifyReason *reason);

/**
 * AxisIdentifyVerdict:
 * @title: (nullable): the title it is (sure) or is most likely (likely);
 *   %NULL for unknown.  Borrowed from the evidence.
 * @candidates: (element-type AxisMetaTitle): the titles to show a person,
 *   best first, borrowed from the evidence, at most
 *   %AXIS_IDENTIFY_MAX_CANDIDATES
 * @reasons: (element-type AxisIdentifyReason): why, in order
 * @waiting: it could not be told yet (a full record it needs was not
 *   answered): nothing final is stored, and the unit is looked at again
 *   when the provider answers
 */
typedef struct
{
	AxisIdentifyConfidence  confidence;
	const AxisMetaTitle    *title;
	GPtrArray              *candidates;
	GPtrArray              *reasons;
	gboolean                waiting;
} AxisIdentifyVerdict;

void
axis_identify_verdict_free(AxisIdentifyVerdict *verdict);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIdentifyVerdict, axis_identify_verdict_free)

/**
 * axis_identify_decide:
 *
 * The confidence, the title, the candidates and the reasons, from the
 * evidence (see the rules above).
 *
 * Returns: (transfer full): the verdict
 */
AxisIdentifyVerdict *
axis_identify_decide(const AxisIdentifyEvidence *evidence);

/**
 * axis_identify_settle_text:
 * @kind: what the unit is looked for as (film or series)
 * @reasons: (element-type AxisIdentifyReason) (nullable): why it is held
 *
 * What would settle a unit held for a person, in a sentence for the
 * Identify page: what to choose there, or what in the names would let it
 * be linked by itself (from the first of its reasons that says).
 *
 * Returns: a static sentence, never %NULL
 */
const gchar *
axis_identify_settle_text(
	AxisMetaKind     kind,
	const GPtrArray *reasons
);

/**
 * axis_identify_doubt_cause:
 * @reasons: (element-type AxisIdentifyReason) (nullable): a verdict's
 *
 * Why a verdict is not sure, for the line that lists a link a later look
 * did not confirm: the text of the most decisive of its reasons against
 * (in the order axis_identify_settle_text() reads them).
 *
 * Returns: (nullable): that reason's text, owned by @reasons; %NULL when
 *     none of them is a reason against
 */
const gchar *
axis_identify_doubt_cause(const GPtrArray *reasons);

/**
 * axis_identify_id_decides:
 *
 * Whether an identifier's evidence settles the unit without looking at the
 * names any further (found, and the names do not clearly disagree): the job
 * then makes no search.
 */
gboolean
axis_identify_id_decides(
	const AxisIdentifyIdEvidence *id,
	const AxisIdentifyReading    *reading
);

/* Whether a name of @reading (the folder's or the file's) reads as a title
 * that fits @title. */
gboolean
axis_identify_corroborates(
	const AxisIdentifyReading *reading,
	const AxisMetaTitle       *title
);

/* As axis_identify_id_decides(), for an .nfo's identifier: it must also be
 * corroborated by a name (rule 3). */
gboolean
axis_identify_nfo_decides(
	const AxisIdentifyIdEvidence *id,
	const AxisIdentifyReading    *reading
);

/* ── The quality of a file found in the library ─────────────────── */

/**
 * AxisIdentifyQuality:
 * @quality: its ladder quality; %AXIS_QUALITY_UNKNOWN when no source is
 *   stated (or the parts of one the ladder needs are not known)
 * @resolution: with an unknown quality, the resolution known for it (from
 *   its name, its folder's or the probe): "an unknown source at that
 *   resolution"; %AXIS_RELEASE_RESOLUTION_NONE otherwise
 * @text: how it was decided, in words
 */
typedef struct
{
	AxisQuality            quality;
	AxisReleaseResolution  resolution;
	gchar                 *text;
} AxisIdentifyQuality;

void
axis_identify_quality_clear(AxisIdentifyQuality *quality);

/**
 * axis_identify_file_quality:
 * @file: what the file's name says
 * @folder: (nullable): what its folder's name says (a film folder's)
 * @probe_class: (nullable): the probe's resolution class ("sd", "720p",
 *   "1080p", "2160p", "other")
 * @out: (out caller-allocates): the quality
 *
 * The source and resolution from the file's name, each completed by the
 * folder's name, the resolution then by the probe.
 */
void
axis_identify_file_quality(
	const AxisRelease   *file,
	const AxisRelease   *folder,
	const gchar         *probe_class,
	AxisIdentifyQuality *out
);

/* The resolution of a stored name ("1080p"), NONE for anything else. */
AxisReleaseResolution
axis_identify_resolution_from_name(const gchar *name);

G_END_DECLS

#endif /* AXIS_IDENTIFY_RULES_H */
