/*
 * axis-release.h - reading a release name, and matching it to a title
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A release is known only by its name
 * ("Mesa.Part.Two.2024.2160p.UHD.BluRay.x265.10bit.HDR.DTS-HD.MA.5.1-GROUP"),
 * written by anyone and following no standard: hostile input.
 * axis_release_parse() reads what a name says and nothing else.  Every
 * field is either found or not found (each says below how "not found"
 * looks); nothing is inferred from anything else (a resolution is never
 * guessed from a source, a source never from a streaming service), and the
 * words after the title that no rule claims are returned in ->unknown.
 * Reading never fails: it only finds less.
 *
 * Limits: at most %AXIS_RELEASE_NAME_MAX bytes of valid UTF-8 with no NUL;
 * anything else (and an empty name) finds nothing.  Time is linear in the
 * length: a tokeniser and fixed tables, no regular expressions.
 *
 * How a name is read:
 *
 *  1. A known file extension is taken off the end (an archive's or a
 *     program's sets a stop mark), and a leading "[www.site.tld]",
 *     "[site.tld]" or "www.site.tld - " off the front.  The tags trackers,
 *     indexes and release collectives write after a name ("[TGx]", " TGx",
 *     "[eztv]", "[EZTVx.to]", "[QxR]", " UTR") are taken off its end, and so
 *     is a last word with no letter or digit (" ⭐").  A Usenet post's
 *     subject for one part of an upload (a part counter such as "[15/93]"
 *     and a file's name in quotes) sets a stop mark.
 *  2. The group: a leading anime-style "[Group]"; else a trailing "[GROUP]"
 *     (YTS style) unless a "-GROUP" comes just before it; else a trailing
 *     "-GROUP".  What follows the hyphen is not a group when it is a
 *     quality word, a number, or the second half of a word such as DTS-HD,
 *     DD-EX, H-264 or WEB-DL; a "-GROUP" also needs a year, an episode mark
 *     or a hard token before it ("DUST-E" has no group).
 *     Before the "-GROUP", the last word of a trailing bracket of quality
 *     words is tried ("(1080p BluRay x265 Tigole)"), unless that word is
 *     known or a file extension; and after the group a reposter's suffix
 *     ("-Obfuscated", "-Chamele0n"), a site's domain ("-Pahe.in"), a
 *     tracker's tag or an extension written as a word is taken off first.
 *  3. Tokens: split at dots, spaces, underscores, brackets, and control,
 *     space and invisible formatting characters.  "5.1", "DDP5.1" and
 *     "H.264" stay whole.  A hyphenated word is split where a part is a
 *     number or a quality word ("Ravenwing-2008-1080p"), and kept whole
 *     otherwise ("Newt-Man").
 *     A digit after a dot that a letter follows starts a word: "K9.5x22"
 *     is "K9" and "5x22".
 *  4. Each token is looked up in fixed tables.  A hard token is only ever a
 *     quality word (1080p, BluRay, x265, DTS); a soft one is also an
 *     ordinary word, or a mark that names put before the year (Web, Opus,
 *     Max, Cam, Extended, Proper, Limited, languages, 3D...).
 *  5. The anchor is the first episode mark, air date or hard token.  The
 *     year is the last year from %AXIS_RELEASE_YEAR_MIN to next year before
 *     the anchor that is not the first token ("1936.2019", "Steel Drifter
 *     2049 2017", "2003 A Comet Voyage 1968").
 *     A number with a dot that has a year after it is a title word, not
 *     channels, and no anchor ("Robo.Pal.2.0.2025").
 *  6. The title is everything before the year (before the anchor when there
 *     is no year).  Edition words, and the marks names put before the year
 *     when written as scene tags (languages, revisions, LIMITED, COMPLETE,
 *     and a recording mark such as CAM: a capital after the first
 *     character, as in GERMAN, PROPER, iNTERNAL), are taken off its end
 *     and read as fields; never when that would leave nothing or only an
 *     article, and a scene tag never when the rest of the title is all
 *     capitals.  A first token that is an episode mark is a title when a
 *     year follows it ("12x12.2018").  A name that is exactly a folder's
 *     "<something> (<year>)" (@folder_form) always has a title: when its
 *     first token is a quality word ("4K (2015)", "x264 Ghosts (2015)"),
 *     everything before the year is the title.
 *  7. Everything after the title is read for the fields.  Soft words count
 *     there, except between an episode mark and the first hard token,
 *     where episode titles are: there a soft word counts only when written
 *     as a scene tag, or when known words alone follow it up to the first
 *     hard token ("German.DL.720p"), but a recording word ("Desk.Cam") only
 *     as a tag.  In a film's name (a year, and no episode mark before the
 *     first hard token) an episode mark among the quality words is a word
 *     not understood ("q22 S94 Joy").
 *
 * GLib only, no I/O: fuzzed by tests/fuzz/fuzz-release-parse.c and
 * tests/fuzz/fuzz-release-match.c; the corpora are in tests/fixtures/release
 * and tests/test-release.c reads them field by field.
 */

#ifndef AXIS_RELEASE_H
#define AXIS_RELEASE_H

#include <glib.h>

#include "meta/axis-meta-model.h"

G_BEGIN_DECLS

/* Longer names find nothing. */
#define AXIS_RELEASE_NAME_MAX       512

/* The earliest year believed. */
#define AXIS_RELEASE_YEAR_MIN       1888

/* At most this many episodes (or absolute numbers) in one name; a range
 * wider than this, or running backwards, is not believed. */
#define AXIS_RELEASE_MAX_NUMBERS    200

/* At most this many languages are kept. */
#define AXIS_RELEASE_MAX_LANGUAGES  8

typedef enum
{
	AXIS_RELEASE_RESOLUTION_NONE = 0,
	AXIS_RELEASE_RESOLUTION_480P,
	AXIS_RELEASE_RESOLUTION_576P,
	AXIS_RELEASE_RESOLUTION_720P,
	AXIS_RELEASE_RESOLUTION_1080P,
	AXIS_RELEASE_RESOLUTION_2160P
} AxisReleaseResolution;

/* Cam to workprint are recordings or pre-release copies: not wanted by
 * default (the quality profiles decide).  WEB alone is WEB-DL; a full disc
 * is an ISO or BDMV-style copy of the disc itself. */
typedef enum
{
	AXIS_RELEASE_SOURCE_NONE = 0,
	AXIS_RELEASE_SOURCE_CAM,
	AXIS_RELEASE_SOURCE_TELESYNC,
	AXIS_RELEASE_SOURCE_TELECINE,
	AXIS_RELEASE_SOURCE_SCREENER,
	AXIS_RELEASE_SOURCE_WORKPRINT,
	AXIS_RELEASE_SOURCE_DVD,
	AXIS_RELEASE_SOURCE_HDTV,
	AXIS_RELEASE_SOURCE_WEBRIP,
	AXIS_RELEASE_SOURCE_WEBDL,
	AXIS_RELEASE_SOURCE_BLURAY,
	AXIS_RELEASE_SOURCE_REMUX,
	AXIS_RELEASE_SOURCE_DISC
} AxisReleaseSource;

typedef enum
{
	AXIS_RELEASE_CODEC_NONE = 0,
	AXIS_RELEASE_CODEC_H264,	/* x264, H.264, AVC */
	AXIS_RELEASE_CODEC_H265,	/* x265, H.265, HEVC */
	AXIS_RELEASE_CODEC_AV1,
	AXIS_RELEASE_CODEC_VC1,
	AXIS_RELEASE_CODEC_MPEG2,
	AXIS_RELEASE_CODEC_XVID,	/* XviD, DivX */
	AXIS_RELEASE_CODEC_VP9
} AxisReleaseCodec;

/* Flags: a name can state several ("DV.HDR"). */
typedef enum
{
	AXIS_RELEASE_HDR_HDR          = 1 << 0,	/* "HDR", the kind not stated */
	AXIS_RELEASE_HDR_HDR10        = 1 << 1,
	AXIS_RELEASE_HDR_HDR10PLUS    = 1 << 2,
	AXIS_RELEASE_HDR_DOLBY_VISION = 1 << 3,	/* DV, DoVi, Dolby Vision */
	AXIS_RELEASE_HDR_HLG          = 1 << 4,
	AXIS_RELEASE_HDR_SDR          = 1 << 5
} AxisReleaseHdr;

/* Flags: a name can state several audio codecs. */
typedef enum
{
	AXIS_RELEASE_AUDIO_TRUEHD    = 1 << 0,
	AXIS_RELEASE_AUDIO_DTS_X     = 1 << 1,
	AXIS_RELEASE_AUDIO_DTS_HD_MA = 1 << 2,
	AXIS_RELEASE_AUDIO_DTS_HD    = 1 << 3,	/* DTS-HD, or High Resolution */
	AXIS_RELEASE_AUDIO_DTS       = 1 << 4,
	AXIS_RELEASE_AUDIO_EAC3      = 1 << 5,	/* E-AC-3, DD+, DDP */
	AXIS_RELEASE_AUDIO_AC3       = 1 << 6,	/* AC-3, DD */
	AXIS_RELEASE_AUDIO_AAC       = 1 << 7,
	AXIS_RELEASE_AUDIO_FLAC      = 1 << 8,
	AXIS_RELEASE_AUDIO_OPUS      = 1 << 9,
	AXIS_RELEASE_AUDIO_MP3       = 1 << 10,
	AXIS_RELEASE_AUDIO_PCM       = 1 << 11
} AxisReleaseAudio;

/* Flags.  OTHER is a bare "Edition", "Cut" or "Version" with no known word
 * before it ("Rust.and.Chrome.Edition": the other words are unknown). */
typedef enum
{
	AXIS_RELEASE_EDITION_EXTENDED    = 1 << 0,
	AXIS_RELEASE_EDITION_DIRECTORS   = 1 << 1,
	AXIS_RELEASE_EDITION_THEATRICAL  = 1 << 2,
	AXIS_RELEASE_EDITION_UNRATED     = 1 << 3,
	AXIS_RELEASE_EDITION_UNCUT       = 1 << 4,
	AXIS_RELEASE_EDITION_REMASTERED  = 1 << 5,
	AXIS_RELEASE_EDITION_RESTORED    = 1 << 6,
	AXIS_RELEASE_EDITION_IMAX        = 1 << 7,
	AXIS_RELEASE_EDITION_CRITERION   = 1 << 8,
	AXIS_RELEASE_EDITION_FINAL_CUT   = 1 << 9,
	AXIS_RELEASE_EDITION_OPEN_MATTE  = 1 << 10,
	AXIS_RELEASE_EDITION_SPECIAL     = 1 << 11,
	AXIS_RELEASE_EDITION_COLLECTORS  = 1 << 12,
	AXIS_RELEASE_EDITION_ANNIVERSARY = 1 << 13,
	AXIS_RELEASE_EDITION_ULTIMATE    = 1 << 14,
	AXIS_RELEASE_EDITION_DEFINITIVE  = 1 << 15,
	AXIS_RELEASE_EDITION_ALTERNATE   = 1 << 16,
	AXIS_RELEASE_EDITION_OTHER       = 1 << 17
} AxisReleaseEdition;

/* Flags: what a name says about its languages, beside the languages. */
typedef enum
{
	AXIS_RELEASE_LANG_MULTI   = 1 << 0,	/* MULTi */
	AXIS_RELEASE_LANG_DUAL    = 1 << 1,	/* DUAL, Dual Audio, DL */
	AXIS_RELEASE_LANG_DUBBED  = 1 << 2,
	AXIS_RELEASE_LANG_SUBBED  = 1 << 3,	/* SUBBED, Subs, VOSTFR */
	AXIS_RELEASE_LANG_HARDSUB = 1 << 4	/* hard-coded subtitles: HC, HardSub, KORSUB */
} AxisReleaseLangMark;

/* Flags. */
typedef enum
{
	AXIS_RELEASE_REVISION_PROPER = 1 << 0,
	AXIS_RELEASE_REVISION_REPACK = 1 << 1,
	AXIS_RELEASE_REVISION_RERIP  = 1 << 2,
	AXIS_RELEASE_REVISION_REAL   = 1 << 3
} AxisReleaseRevision;

/* Flags: other marks a name can carry. */
typedef enum
{
	AXIS_RELEASE_MARK_LIMITED  = 1 << 0,
	AXIS_RELEASE_MARK_INTERNAL = 1 << 1,
	AXIS_RELEASE_MARK_FIX      = 1 << 2,	/* NFOFIX, DIRFIX, SUBFIX... */
	AXIS_RELEASE_MARK_READNFO  = 1 << 3,
	AXIS_RELEASE_MARK_HYBRID   = 1 << 4
} AxisReleaseMark;

/* Flags: marks that should stop a grab (the caller decides; 3D only when
 * 3D is not wanted). */
typedef enum
{
	AXIS_RELEASE_STOP_SAMPLE     = 1 << 0,
	AXIS_RELEASE_STOP_PASSWORD   = 1 << 1,
	AXIS_RELEASE_STOP_3D         = 1 << 2,
	AXIS_RELEASE_STOP_OBFUSCATED = 1 << 3,	/* nothing but a hash-like string */
	AXIS_RELEASE_STOP_ARCHIVE    = 1 << 4,	/* the name ends in .rar, .zip... */
	AXIS_RELEASE_STOP_EXECUTABLE = 1 << 5,	/* the name ends in .exe, .scr... */
	AXIS_RELEASE_STOP_SUBJECT    = 1 << 6	/* a Usenet post's subject for one part of a
						 * multi-part upload: a part counter
						 * ("[15/93]", "(101/171)") and a file's
						 * name in quotes, not a release's name */
} AxisReleaseStop;

/**
 * AxisRelease:
 * @title: (nullable): the title, its words as they are written in the name,
 *   joined by single spaces; %NULL: not found
 * @year: 0: not found
 * @season_first: -1: not found; 0 is specials; a pack of several seasons
 *   runs to @season_last
 * @season_last: equal to @season_first for one season; -1 with it
 * @episode_count: how many of @episodes are set; 0 with a season found is a
 *   whole season
 * @episodes: the episodes within the season, in the order stated, ranges
 *   written out
 * @absolute_count: how many of @absolute are set (0: none)
 * @absolute: absolute episode numbers (anime "- 123", "E123"), ranges
 *   written out
 * @air_year: a daily show's date ("2024.03.15"); 0: not found
 * @complete: "Complete" (a whole series or several seasons)
 * @version: "v2" and the like; 0: not found
 * @interlaced: "1080i"
 * @service: (nullable): the streaming service, an id such as "amazon",
 *   "netflix", "disney" (from AMZN, NF, DSNP); %NULL: not found
 * @bit_depth: 8, 10 or 12; 0: not found
 * @hdr: #AxisReleaseHdr flags; 0: none stated
 * @audio: #AxisReleaseAudio flags; 0: none stated
 * @channels: "5.1", "7.1", "2.0", "7.1.4"; empty: not found
 * @editions: #AxisReleaseEdition flags
 * @languages: (array length=language_count): ISO 639-1 codes ("de",
 *   "fr"), in the order stated, each once; static strings
 * @language_marks: #AxisReleaseLangMark flags
 * @revision: #AxisReleaseRevision flags
 * @marks: #AxisReleaseMark flags
 * @stop: #AxisReleaseStop flags
 * @group: (nullable): the release group
 * @checksum: (nullable): an anime-style "[ABCD1234]", as written
 * @unknown: (element-type utf8): the words after the title that nothing
 *   claimed, in order, as written
 * @folder_form: the name (its extension and trackers' tags taken off) is
 *   exactly "<something> (<year>)", a space before the bracket and no
 *   other bracket: the form a library's folders, and the videos in them,
 *   are named in.  The something is then always the title (rule 6)
 */
typedef struct
{
	gchar                 *title;
	gint                   year;

	gint                   season_first;
	gint                   season_last;
	guint                  episode_count;
	gint                   episodes[AXIS_RELEASE_MAX_NUMBERS];
	guint                  absolute_count;
	gint                   absolute[AXIS_RELEASE_MAX_NUMBERS];
	gint                   air_year;
	gint                   air_month;
	gint                   air_day;
	gboolean               complete;
	gint                   version;

	AxisReleaseResolution  resolution;
	gboolean               interlaced;
	AxisReleaseSource      source;
	const gchar           *service;
	AxisReleaseCodec       codec;
	gint                   bit_depth;
	guint                  hdr;
	guint                  audio;
	gboolean               atmos;
	gchar                  channels[8];
	guint                  editions;
	guint                  language_count;
	const gchar           *languages[AXIS_RELEASE_MAX_LANGUAGES];
	guint                  language_marks;
	guint                  revision;
	guint                  marks;
	guint                  stop;
	gchar                 *group;
	gchar                 *checksum;
	GPtrArray             *unknown;
	gboolean               folder_form;
} AxisRelease;

/**
 * axis_release_parse:
 * @name: (nullable): a release name (not a path)
 *
 * Reads @name, believing years up to next year.
 *
 * Returns: (transfer full): what was read; never %NULL
 */
AxisRelease *
axis_release_parse(const gchar *name);

/**
 * axis_release_parse_full:
 * @name: (nullable): a release name
 * @length: its length in bytes, or -1 when it ends at a NUL
 * @latest_year: the latest year believed (a test or a fuzz target fixes
 *   it; axis_release_parse() uses next year)
 *
 * Returns: (transfer full): what was read; never %NULL
 */
AxisRelease *
axis_release_parse_full(
	const gchar *name,
	gssize       length,
	gint         latest_year
);

void
axis_release_free(AxisRelease *release);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRelease, axis_release_free)

/* Whether @release names episodes: a season, episodes, absolute numbers,
 * an air date, or "Complete". */
gboolean
axis_release_is_episode(const AxisRelease *release);

/* ── What a name says it holds of a series ──────────────────────── */

/**
 * AxisReleaseHoldsKind:
 * @AXIS_RELEASE_HOLDS_NOTHING: no episode it can name (@code says why)
 * @AXIS_RELEASE_HOLDS_DATE: the episode aired on a day (a daily show)
 * @AXIS_RELEASE_HOLDS_EPISODES: episodes of one season
 * @AXIS_RELEASE_HOLDS_SEASONS: whole seasons, @season_first to @season_last
 * @AXIS_RELEASE_HOLDS_COMPLETE: "Complete" with no season: every season
 */
typedef enum
{
	AXIS_RELEASE_HOLDS_NOTHING = 0,
	AXIS_RELEASE_HOLDS_DATE,
	AXIS_RELEASE_HOLDS_EPISODES,
	AXIS_RELEASE_HOLDS_SEASONS,
	AXIS_RELEASE_HOLDS_COMPLETE
} AxisReleaseHoldsKind;

/**
 * AxisReleaseStates:
 * @code: for %AXIS_RELEASE_HOLDS_NOTHING: "absolute_only" (an absolute
 *   number, with a season or not: @absolute is the first, @with_season
 *   whether a season was stated), "cross_season" (episodes numbered across
 *   seasons) or "no_episode" (no season and no episode); %NULL otherwise
 * @date: for %AXIS_RELEASE_HOLDS_DATE, "YYYY-MM-DD"
 * @season_first, @season_last: the season of the episodes, or the seasons
 * @episodes: (array length=n_episodes): the episodes within the season, as
 *   the parser read them (borrowed from the release)
 */
typedef struct
{
	AxisReleaseHoldsKind  kind;
	const gchar          *code;
	gint                  absolute;
	gboolean              with_season;
	gchar                 date[11];
	gint                  season_first;
	gint                  season_last;
	guint                 n_episodes;
	const gint           *episodes;
} AxisReleaseStates;

/**
 * axis_release_states:
 * @release: what was read from a name
 * @states: (out caller-allocates): what it says it holds
 *
 * What a name says it holds of a series, by its own words alone, in this
 * order: an air date; with no season, "Complete" (every season), an
 * absolute number or nothing; a season with no episode named, an absolute
 * number ("S02 - 05", not matched) or the whole season (or seasons); then
 * episodes, of one season only.  The one reading of a name both a release's
 * matcher (the TV module) and identification's file reading use.  Pure.
 */
void
axis_release_states(
	const AxisRelease *release,
	AxisReleaseStates *states
);

/* ── Reading a name after a known title ─────────────────────────── */

/**
 * AxisReleaseAfter:
 * @read: one number, or one range, follows one of the names
 * @code: (nullable): when not read, why (static): "unreadable" (not a name
 *   the parser would read), "other_title" (the name does not begin with one
 *   of the names), "no_number", "special" (a word such as OVA follows),
 *   "two_numbers", "decimal" ("12.5"), "year", "zero", "single_digit" (one
 *   digit with no hyphen before it), "backwards", "too_wide" (a range of more
 *   than %AXIS_RELEASE_MAX_NUMBERS), "trailing" (words not understood follow
 *   the number), "not_episode" (a word after the number says it is not one
 *   episode: "Complete", "BD", "Vol", "Movie", "PV", "Part 2", "of 12");
 *   %NULL when read
 * @titled: the name begins with one of the names (whatever follows)
 * @first, @last: the number (both), or the range's ends; 1 to 9999
 * @range: a range was written ("01-12", "01 ~ 12")
 * @hyphen: a lone hyphen stands between the name and the number
 * @digits: how many digits the (first) number is written with
 * @version: "12v2": 2; 0: none
 * @title_at, @title_end: where in the name the series' name stands (bytes)
 * @number_end: where the number, or the range, ends
 */
typedef struct
{
	gboolean     read;
	const gchar *code;
	gboolean     titled;
	gint         first;
	gint         last;
	gboolean     range;
	gboolean     hyphen;
	guint        digits;
	gint         version;
	gsize        title_at;
	gsize        title_end;
	gsize        number_end;
} AxisReleaseAfter;

/**
 * axis_release_read_after:
 * @name: (nullable): a release's or a file's name (not a path)
 * @titles: (array length=n_titles): the names of ONE series: its title, its
 *   original title, its other names (%NULL entries are skipped)
 * @after: (out caller-allocates): what was read
 *
 * A second reading, for the forms that only a known title makes safe: a bare
 * number ("Tenkuu Rangers 137 [1080p]"), a range with no brackets ("Tenkuu
 * Rangers 001-052"), a single digit after a hyphen with no group.  A known
 * file extension, a leading "[Group]" and site tags are taken off.  The name
 * must then begin with one of @titles, compared by axis_release_title_key()
 * at the end of a word (exactly as axis_release_match() compares a title; no
 * prefix of a word, nothing fuzzy), the longest that fits being taken.  What
 * follows is read only when it is exactly one whole number of one to four
 * digits with an optional version ("12v2"), or one range ("01-12", "01~12",
 * "01 ~ 12"), with or without a lone hyphen before it, followed by the end of
 * the name, a bracket, or a word the parser's tables know.  A four-digit
 * number that can be a year, a number with a decimal part, two numbers in a
 * row, 0, a single digit with no hyphen, and a special's word are not read,
 * each with its code.  A number alone in the bracket that follows ("10 [05]")
 * is a second number, unless it can be a year or is a picture's height; and
 * "Complete", "Batch", "BD", "Vol" and "Volume" after the number are words
 * not understood here, whatever the tables know of them ("02 Complete" is a
 * season or a volume, not an episode).  Nothing is counted or inferred.
 *
 * What is read here is the weakest reading there is: the name of another
 * title can be the very same ("Tenkuu Rangers 13", a film).  The caller
 * decides what it is good for (the TV module: a person's grab, never a
 * search's own).
 *
 * Never fails, bounded as axis_release_parse() is, and independent of it: it
 * changes nothing of what reading a name by itself reports.  Which series it
 * is used for is the caller's rule (the TV module: a series marked as anime
 * whose absolute numbers are usable).
 */
void
axis_release_read_after(
	const gchar        *name,
	const gchar *const *titles,
	guint               n_titles,
	AxisReleaseAfter   *after
);

/* The same, for @length bytes (-1: to a NUL), believing years up to
 * @latest_year (a test or a fuzz target fixes it). */
void
axis_release_read_after_full(
	const gchar        *name,
	gssize              length,
	const gchar *const *titles,
	guint               n_titles,
	gint                latest_year,
	AxisReleaseAfter   *after
);

/* Why a name that begins with the series' name was not read, as a clause a
 * page shows ("two numbers follow the series' name, ..."); %NULL for a code
 * that is not about what follows the name (static). */
const gchar *
axis_release_after_words(const gchar *code);

/* Whether @text (@length bytes) is a word by which a release names a
 * special, an extra or a film of a series ("OVA", "OAD", "ONA", "NCOP",
 * "NCED", "PV", "SP01", "Special", "Movie", "Gekijouban"). */
gboolean
axis_release_special_word(
	const gchar *text,
	gsize        length
);

/* ── Names of the values (stable identifiers, lower case) ───────── */

/* "480p", "576p", "720p", "1080p", "2160p"; %NULL for none. */
const gchar *
axis_release_resolution_name(AxisReleaseResolution resolution);

/* "cam", "telesync", "telecine", "screener", "workprint", "dvd", "hdtv",
 * "webrip", "webdl", "bluray", "remux", "disc"; %NULL for none. */
const gchar *
axis_release_source_name(AxisReleaseSource source);

/* "h264", "h265", "av1", "vc1", "mpeg2", "xvid", "vp9"; %NULL for none. */
const gchar *
axis_release_codec_name(AxisReleaseCodec codec);

/*
 * The name of one flag of a set ("hdr10", "dtshdma", "extended", "dual",
 * "proper", "limited", "sample"...), %NULL for a value that is not exactly
 * one known flag.  Each set has its own: see the source for the lists.
 */
const gchar *
axis_release_hdr_name(guint flag);

const gchar *
axis_release_audio_name(guint flag);

const gchar *
axis_release_edition_name(guint flag);

const gchar *
axis_release_lang_mark_name(guint flag);

const gchar *
axis_release_revision_name(guint flag);

const gchar *
axis_release_mark_name(guint flag);

const gchar *
axis_release_stop_name(guint flag);

/**
 * axis_release_describe:
 * @release: what was read
 *
 * One line naming every field that was found, for logs, the audit tool and
 * test messages ("title='Mesa Part Two' year=2024 resolution=2160p ...").
 *
 * Returns: (transfer full): the text
 */
gchar *
axis_release_describe(const AxisRelease *release);

/* ── Matching a name to a title ─────────────────────────────────── */

typedef enum
{
	AXIS_RELEASE_MATCH_NONE = 0,
	AXIS_RELEASE_MATCH_YES,
	AXIS_RELEASE_MATCH_AMBIGUOUS
} AxisReleaseMatch;

/* "no match", "match", "ambiguous". */
const gchar *
axis_release_match_name(AxisReleaseMatch match);

/**
 * axis_release_title_key:
 * @text: (nullable): a title
 *
 * The form titles are compared in: decomposed (NFKD), case-folded, marks
 * removed; the letters that do not decompose written as the plain letters
 * they stand for ("ø" as "o", "đ" and "ð" as "d", "ł" as "l", "æ" as "ae",
 * "œ" as "oe", "þ" as "th", "ß" as "ss", and "ħ", "ı", "ŧ"), so that a name
 * written without them reads as the title; apostrophes dropped; "&" (and
 * "&amp;", as a feed that escaped a
 * name twice writes it) read as "and"; a word that is a roman
 * numeral from I to XXXIX written as its number ("Dusty II" and "Dusty 2"
 * are the same); then every other character that is not a letter or a
 * digit dropped, separators included ("Newt-Man", "Newt Man" and
 * "NewtMan" are the same).  Articles are kept: "The Lattice" and "Lattice"
 * differ.
 *
 * Returns: (transfer full) (nullable): the key ("" when nothing is left),
 *   or %NULL for %NULL, text that is not valid UTF-8, or text longer than
 *   4 KiB
 */
gchar *
axis_release_title_key(const gchar *text);

/**
 * axis_release_match:
 * @release: what was read from a name
 * @candidates: (array length=n): the titles it might be: every title the
 *   name could mean, so that "only one has that title" means something
 * @n: how many
 * @matched: (out) (optional): the index of the title matched, set only for
 *   %AXIS_RELEASE_MATCH_YES (-1 otherwise)
 * @reason: (out) (optional) (transfer full): why, in words
 *
 * A candidate fits when the name's title key equals the key of its title,
 * original title or one of its other names, and the year in the name
 * equals its year or differs by one (release dates differ by country).  A
 * name without a year fits every candidate of that title, and matches only
 * when exactly one has it.  A name with a year matches the one candidate of
 * that title whose known year fits; two such candidates make it ambiguous
 * (even when one has the very year).  A candidate of that title with no
 * year known does not stand in the way of one whose year is the name's
 * (it cannot be shown to be the name's), but does of one a year off, and
 * alone it is ambiguous.  Ambiguous is never a match.  A name that reads
 * as episodes is compared with series only.  A reason names each candidate
 * with its year, or says it has none.
 *
 * Returns: match, no match, or ambiguous
 */
AxisReleaseMatch
axis_release_match(
	const AxisRelease          *release,
	const AxisMetaTitle *const *candidates,
	guint                       n,
	gint                       *matched,
	gchar                     **reason
);

G_END_DECLS

#endif /* AXIS_RELEASE_H */
