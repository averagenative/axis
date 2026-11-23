/*
 * axis-quality.h - quality profiles: the ladder of qualities, the profile
 * (what is acceptable, in what order, at what size, with which preferences)
 * and the decision for a list of releases, with every reason in words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * THE LADDER is fixed: users choose among its qualities and order them, never
 * define one.  A quality is a source and a resolution as the release parser
 * read them (release/axis-release.h), worst first:
 *
 *   Unwanted  workprint, cam, telesync, telecine, screener
 *   SD        DVD, SDTV, WEB 480p, BluRay 480p/576p
 *   720p      HDTV, WEBRip, WEB-DL, BluRay
 *   1080p     HDTV, WEBRip, WEB-DL, BluRay, Remux
 *   2160p     HDTV, WEBRip, WEB-DL, BluRay, Remux
 *   Disc      full disc images and folders
 *
 * The entries that name a resolution need it found in the name: a name
 * whose source or resolution the parser did not find is of UNKNOWN quality
 * ("Film.2020.WEB-DL.x264" is unknown).  DVD, Disc and the recordings name
 * none and are placed by their source alone (so that a cam without a
 * resolution is still a cam: an "unknown" cam would slip past the switch
 * below).  480p and 576p are SD; 1080i is 1080p; a remux at 720p or SD is
 * the BluRay of that resolution (the ladder has no such remux).
 *
 * A PROFILE (AxisQualityProfile, a plain value) holds:
 *  - the allowed qualities in order of preference, best first, in tiers:
 *    the qualities of one tier are equals ("any 1080p web source");
 *  - the cutoff: once the file on disk is at or above it no upgrade is
 *    sought;
 *  - whether releases of unknown quality are allowed (last in the order),
 *    and the separate switch without which a recording or pre-release copy
 *    (the Unwanted class) is never accepted, whatever the order lists;
 *  - per quality, a minimum and maximum in mebibytes (MiB, 2^20 bytes) per
 *    minute of runtime, and an absolute maximum for any release in MiB (the
 *    interface writes every size in binary units, the sentences too; the
 *    stored numbers of a profile saved when they meant decimal megabytes are
 *    kept as they are, and mean mebibytes now: a limit is 4.9% larger than
 *    it was written to be);
 *  - preference rules: a condition on the parsed fields and an effect,
 *    require, reject, prefer (+weight) or avoid (-weight);
 *  - the upgrade rules: whether to upgrade at all, whether a proper or
 *    repack of the same quality counts, whether a better score at the same
 *    quality counts and by how much;
 *  - the players preference: avoid, by a stated weight, what the household's
 *    players would need transcoded (the client compatibility verdicts);
 *    0 is off.
 *
 * THE DECISION is pure: no database, no I/O, no clock (the time is an
 * input).  For each release it says accepted or rejected with EVERY reason
 * as a sentence with its numbers in it ("2.9 GiB is above the 2.4 GiB limit
 * for a 97-minute film at WEB-DL 1080p"), ranks the accepted, and with a
 * file on disk says whether each is an upgrade.  Some rules no profile can
 * relax (AXIS_QUALITY_REASON_SAFETY): a sample, a password-protected
 * release, a name ending in an archive's or a program's extension, a name
 * with no readable title (or nothing but a hash-like string), and a release
 * whose match to the title is not "match" (ambiguous is never a match).
 *
 * DEFINITION FORMAT 1, the stored JSON (RFC 8259, at most
 * %AXIS_QUALITY_MAX_TEXT bytes).  Every member is required where it applies
 * and refused where it does not; any other member, a repeated member, a
 * wrong type or a value out of bounds is an error, and so is a definition of
 * another format, which is not read at all (a newer version's members may
 * mean something else).  A definition that fails is refused whole, never
 * half-applied.
 *
 *   format       1
 *   qualities    array of 1 to 24 tiers, best first; a tier is an array of
 *                quality names ("webdl_1080p"), each quality at most once
 *                in all
 *   cutoff       a quality name among them
 *   unknown      boolean: releases of unknown quality are allowed
 *   unwanted     boolean: recordings and pre-release copies may be accepted
 *                (when the qualities list them too)
 *   sizes        object with one member for each of the 24 qualities of the
 *                ladder: [min, max], whole mebibytes per minute, 0 for no
 *                limit, at most %AXIS_QUALITY_MAX_MB_PER_MIN, max above min
 *                when both are set
 *   max_size_mb  whole mebibytes, 0 (none) to %AXIS_QUALITY_MAX_SIZE_MB: no
 *                release above it, whatever its runtime
 *   rules        array of 0 to %AXIS_QUALITY_MAX_RULES objects:
 *                  if      the condition (see AxisQualityCondition)
 *                  is      array of 1 to %AXIS_QUALITY_MAX_VALUES distinct
 *                          values, for every condition but "3d" and
 *                          "hardsub" (which take none)
 *                  then    "require", "reject", "prefer" or "avoid"
 *                  weight  1 to %AXIS_QUALITY_MAX_WEIGHT, prefer and avoid
 *                          only
 *                the same rule twice is refused
 *   upgrades     object: allowed, revisions, score (booleans), and
 *                min_score_gain (1 to %AXIS_QUALITY_MAX_SCORE_GAIN) only
 *                when score is true; and unknown (boolean, the one optional
 *                member: absent is false, and the canonical form writes it
 *                only when true): a file on disk of an unknown source at a
 *                known resolution may be upgraded
 *   players      integer, 0 (off) to %AXIS_QUALITY_MAX_WEIGHT
 *
 * The canonical form: members in byte order of their names, no white space,
 * a tier's qualities best first, a rule's values sorted (ignoring case, then
 * by bytes), absent members absent.  Parsing the canonical text of a valid
 * profile gives the same profile.
 *
 * GLib only, no I/O: fuzzed by tests/fuzz/fuzz-quality-profile.c; the
 * corpora of the release parser are put on the ladder by
 * tests/test-quality.c.
 */

#ifndef AXIS_QUALITY_H
#define AXIS_QUALITY_H

#include <glib.h>

#include "compat/axis-compat-verdict.h"
#include "download/axis-download-model.h"
#include "release/axis-release.h"

G_BEGIN_DECLS

#define AXIS_QUALITY_FORMAT           1
#define AXIS_QUALITY_MAX_TEXT         16384
#define AXIS_QUALITY_MAX_RULES        32
#define AXIS_QUALITY_MAX_VALUES       16
#define AXIS_QUALITY_VALUE_MAX        32	/* bytes of one value */
#define AXIS_QUALITY_MAX_WEIGHT       1000
#define AXIS_QUALITY_MAX_SCORE_GAIN   10000
#define AXIS_QUALITY_MAX_MB_PER_MIN   2000
#define AXIS_QUALITY_MAX_SIZE_MB      1024000	/* 1000 GiB */

/* ── The ladder ─────────────────────────────────────────────────── */

/**
 * AxisQuality:
 *
 * The qualities of the ladder, worst first; %AXIS_QUALITY_UNKNOWN is not on
 * it.  The values are stable only within a build: what is stored is the
 * name (axis_quality_name()).
 */
typedef enum
{
	AXIS_QUALITY_UNKNOWN = 0,
	AXIS_QUALITY_WORKPRINT,
	AXIS_QUALITY_CAM,
	AXIS_QUALITY_TELESYNC,
	AXIS_QUALITY_TELECINE,
	AXIS_QUALITY_SCREENER,
	AXIS_QUALITY_DVD,
	AXIS_QUALITY_SDTV,
	AXIS_QUALITY_WEB_480P,
	AXIS_QUALITY_BLURAY_480P,
	AXIS_QUALITY_HDTV_720P,
	AXIS_QUALITY_WEBRIP_720P,
	AXIS_QUALITY_WEBDL_720P,
	AXIS_QUALITY_BLURAY_720P,
	AXIS_QUALITY_HDTV_1080P,
	AXIS_QUALITY_WEBRIP_1080P,
	AXIS_QUALITY_WEBDL_1080P,
	AXIS_QUALITY_BLURAY_1080P,
	AXIS_QUALITY_REMUX_1080P,
	AXIS_QUALITY_HDTV_2160P,
	AXIS_QUALITY_WEBRIP_2160P,
	AXIS_QUALITY_WEBDL_2160P,
	AXIS_QUALITY_BLURAY_2160P,
	AXIS_QUALITY_REMUX_2160P,
	AXIS_QUALITY_DISC
} AxisQuality;

/* How many qualities the ladder has (unknown not counted); they are 1 to
 * this. */
#define AXIS_QUALITY_N_LADDER 24

typedef enum
{
	AXIS_QUALITY_CLASS_UNKNOWN = 0,
	AXIS_QUALITY_CLASS_UNWANTED,
	AXIS_QUALITY_CLASS_SD,
	AXIS_QUALITY_CLASS_720P,
	AXIS_QUALITY_CLASS_1080P,
	AXIS_QUALITY_CLASS_2160P,
	AXIS_QUALITY_CLASS_DISC
} AxisQualityClass;

/* The quality of a source and a resolution, as the ladder places them. */
AxisQuality
axis_quality_of(
	AxisReleaseSource     source,
	AxisReleaseResolution resolution
);

/* The quality of what the parser read from a name. */
AxisQuality
axis_quality_of_release(const AxisRelease *release);

/* "webdl_1080p", ..., "unknown": stable identifiers (stored). */
const gchar *
axis_quality_name(AxisQuality quality);

/* Unknown is not a name this accepts: only the 24 of the ladder. */
gboolean
axis_quality_from_name(
	const gchar *name,
	AxisQuality *quality
);

/* "WEB-DL 1080p", "BluRay 480p/576p", "Cam", "Unknown". */
const gchar *
axis_quality_label(AxisQuality quality);

AxisQualityClass
axis_quality_class(AxisQuality quality);

/* "Unwanted", "SD", "720p", "1080p", "2160p", "Disc", "Unknown". */
const gchar *
axis_quality_class_label(AxisQualityClass klass);

/* The Unwanted class: cinema recordings and pre-release copies. */
gboolean
axis_quality_is_unwanted(AxisQuality quality);

/* Negative when @a is lower on the ladder than @b, 0 when the same, positive
 * when higher.  Unknown is below every quality of the ladder. */
gint
axis_quality_compare(
	AxisQuality a,
	AxisQuality b
);

/**
 * axis_quality_explain_place:
 *
 * Where @release is on the ladder, and why when it is not plain: "WEB-DL
 * 1080p", "Cam, by its source alone, whatever its resolution", "BluRay
 * 720p, as the ladder has no remux below 1080p", "unknown: no resolution
 * (720p, 1080p, ...) was found in the name".
 *
 * Returns: (transfer full): the text
 */
gchar *
axis_quality_explain_place(const AxisRelease *release);

/* A source as the pages name it ("WEB-DL", "Cam (a recording in a
 * cinema)"); %NULL for none. */
const gchar *
axis_quality_source_label(AxisReleaseSource source);

/*
 * AN UNKNOWN SOURCE AT A RESOLUTION.  A file found in the library whose name
 * states no source but whose resolution is known (from its name or its
 * probe) is of unknown quality on the ladder; it is placed just below every
 * stated source of its resolution (an unknown source at 1080p is below HDTV
 * 1080p and above every 720p quality).  It is upgraded only when its
 * profile says so (AxisQualityProfile.upgrade_unknown), and then by a
 * release of its resolution or higher that states its source.
 */

/* "an unknown source at 1080p"; "an unknown quality" for none. */
const gchar *
axis_quality_unknown_source_label(AxisReleaseResolution resolution);

/* Whether @quality is above a file of unknown source at @resolution (of
 * that resolution or higher, and not a recording); for NONE, whether
 * @quality is a quality at all. */
gboolean
axis_quality_above_unknown(
	AxisQuality           quality,
	AxisReleaseResolution resolution
);

/* The number axis_quality_compare_on_disk() orders by: twice the quality's
 * rung for a known quality (even), one less than twice the rung of the
 * lowest stated source of @resolution for an unknown one (odd), 0 when
 * nothing is known.  A list that orders or counts files in SQL uses it. */
guint
axis_quality_on_disk_rank(
	AxisQuality           quality,
	AxisReleaseResolution resolution
);

/* As axis_quality_compare() for two files on disk, each a quality, or an
 * unknown quality with the resolution known for it (NONE: nothing known,
 * below everything). */
gint
axis_quality_compare_on_disk(
	AxisQuality           a,
	AxisReleaseResolution a_resolution,
	AxisQuality           b,
	AxisReleaseResolution b_resolution
);

/* ── Preference rules ───────────────────────────────────────────── */

/**
 * AxisQualityCondition:
 * @AXIS_QUALITY_IF_CODEC: the video codec ("h264", "h265", "av1", "vc1",
 *   "mpeg2", "xvid", "vp9": the parser's names)
 * @AXIS_QUALITY_IF_BIT_DEPTH: "8", "10", "12"
 * @AXIS_QUALITY_IF_HDR: "hdr", "hdr10", "hdr10plus", "dv", "hlg", "sdr", and
 *   "any" (any kind of HDR stated) and "dv_only" (Dolby Vision without an
 *   HDR10 fallback: Dolby Vision stated, and none of HDR, HDR10, HDR10+)
 * @AXIS_QUALITY_IF_AUDIO: the parser's audio names ("truehd", "dtshdma",
 *   "eac3", ...) and "atmos"
 * @AXIS_QUALITY_IF_CHANNELS: "2.0", "5.1", "7.1", ... as written
 * @AXIS_QUALITY_IF_LANGUAGE: two-letter ISO 639-1 codes ("de", "fr")
 * @AXIS_QUALITY_IF_LANGUAGE_MARK: "multi", "dual", "dubbed", "subbed"
 * @AXIS_QUALITY_IF_EDITION: the parser's edition names ("extended",
 *   "directors", "imax", ...)
 * @AXIS_QUALITY_IF_SERVICE: streaming service ids ("amazon", "netflix")
 * @AXIS_QUALITY_IF_GROUP: release groups, compared ignoring case (letters,
 *   digits, '.', '_', '+', '-')
 * @AXIS_QUALITY_IF_WORD: a word of the name (letters and digits), compared
 *   ignoring case; the title's own words do not count
 * @AXIS_QUALITY_IF_3D: the name says 3D (no values)
 * @AXIS_QUALITY_IF_HARDSUB: hard-coded subtitles (no values)
 * @AXIS_QUALITY_IF_REVISION: "proper", "repack", "rerip", "real"
 *
 * A condition holds when the release has any of the rule's values.  A field
 * the name does not state holds no value.
 */
typedef enum
{
	AXIS_QUALITY_IF_CODEC = 0,
	AXIS_QUALITY_IF_BIT_DEPTH,
	AXIS_QUALITY_IF_HDR,
	AXIS_QUALITY_IF_AUDIO,
	AXIS_QUALITY_IF_CHANNELS,
	AXIS_QUALITY_IF_LANGUAGE,
	AXIS_QUALITY_IF_LANGUAGE_MARK,
	AXIS_QUALITY_IF_EDITION,
	AXIS_QUALITY_IF_SERVICE,
	AXIS_QUALITY_IF_GROUP,
	AXIS_QUALITY_IF_WORD,
	AXIS_QUALITY_IF_3D,
	AXIS_QUALITY_IF_HARDSUB,
	AXIS_QUALITY_IF_REVISION
} AxisQualityCondition;

#define AXIS_QUALITY_N_CONDITIONS 14

typedef enum
{
	AXIS_QUALITY_REQUIRE = 0,
	AXIS_QUALITY_REJECT,
	AXIS_QUALITY_PREFER,
	AXIS_QUALITY_AVOID
} AxisQualityEffect;

#define AXIS_QUALITY_N_EFFECTS 4

/* "codec", "bit_depth", "hdr", "audio", "channels", "language",
 * "language_mark", "edition", "service", "group", "word", "3d", "hardsub",
 * "revision"; %NULL for none. */
const gchar *
axis_quality_condition_name(AxisQualityCondition condition);

gboolean
axis_quality_condition_from_name(
	const gchar          *name,
	AxisQualityCondition *condition
);

/* "Video codec", "Release group", ... */
const gchar *
axis_quality_condition_label(AxisQualityCondition condition);

/* Whether the condition takes values (all but 3d and hardsub). */
gboolean
axis_quality_condition_has_values(AxisQualityCondition condition);

/* The values a condition of a fixed set takes, ended by %NULL; %NULL for
 * the open ones (channels, language, service, group, word). */
const gchar *const *
axis_quality_condition_values(AxisQualityCondition condition);

/* Whether @value is one @condition takes (exactly as it is stored: the
 * fixed sets and codes in lower case). */
gboolean
axis_quality_value_valid(
	AxisQualityCondition  condition,
	const gchar          *value
);

/* "HEVC (x265)", "Dolby Vision without an HDR10 fallback", "German": a
 * value in words (the value itself when it has no other name). */
const gchar *
axis_quality_value_label(
	AxisQualityCondition  condition,
	const gchar          *value
);

/* "require", "reject", "prefer", "avoid". */
const gchar *
axis_quality_effect_name(AxisQualityEffect effect);

gboolean
axis_quality_effect_from_name(
	const gchar       *name,
	AxisQualityEffect *effect
);

typedef struct
{
	AxisQualityCondition condition;
	AxisQualityEffect    effect;
	gint                 weight;	/* prefer, avoid: 1 to 1000; else 0 */
	guint                n_values;
	gchar                values[AXIS_QUALITY_MAX_VALUES][AXIS_QUALITY_VALUE_MAX + 1];
} AxisQualityRule;

/* Whether @rule's condition holds for @release (@name: the name it was read
 * from, for the word condition; nullable). */
gboolean
axis_quality_rule_holds(
	const AxisQualityRule *rule,
	const AxisRelease     *release,
	const gchar           *name
);

/* The rule in words: "Prefer HEVC (x265), by 100". */
gchar *
axis_quality_rule_describe(const AxisQualityRule *rule);

/* ── Profiles ───────────────────────────────────────────────────── */

typedef struct
{
	gint min;	/* MiB per minute; 0: no minimum */
	gint max;	/* MiB per minute; 0: no maximum */
} AxisQualitySize;

/**
 * AxisQualityProfile:
 * @allowed: the allowed qualities, best first
 * @tier: @tier[i] is the tier of @allowed[i]: 0 for the first, rising by 0
 *   (an equal of the one before) or 1
 * @sizes: by AxisQuality (index 0, unknown, is unused: a release of unknown
 *   quality has only the absolute limit)
 *
 * A profile, as parsed.  A plain value: copy it with =.
 */
typedef struct
{
	guint            n_allowed;
	AxisQuality      allowed[AXIS_QUALITY_N_LADDER];
	guint            tier[AXIS_QUALITY_N_LADDER];
	AxisQuality      cutoff;
	gboolean         allow_unknown;
	gboolean         allow_unwanted;
	AxisQualitySize  sizes[AXIS_QUALITY_N_LADDER + 1];
	gint64           max_size_mb;
	guint            n_rules;
	AxisQualityRule  rules[AXIS_QUALITY_MAX_RULES];
	gboolean         upgrades;
	gboolean         upgrade_revisions;
	gboolean         upgrade_score;
	gint             min_score_gain;	/* with upgrade_score, else 0 */
	gint             players;	/* weight; 0: off */
	gboolean         upgrade_unknown;	/* an unknown source on disk may be upgraded */
} AxisQualityProfile;

/* An empty profile: nothing allowed, no rules, sizes the defaults, upgrades
 * on (repacks, not score).  Not valid until a quality is allowed. */
void
axis_quality_profile_init(AxisQualityProfile *profile);

/* The ladder's default size limits for @quality (MiB per minute). */
AxisQualitySize
axis_quality_default_size(AxisQuality quality);

/* The tier of @quality in @profile (0 is the most preferred), or -1 when it
 * is not allowed. */
gint
axis_quality_profile_tier(
	const AxisQualityProfile *profile,
	AxisQuality               quality
);

/* How many tiers the profile has. */
guint
axis_quality_profile_n_tiers(const AxisQualityProfile *profile);

/**
 * AxisQualityProblem:
 * @field: what it is about ("cutoff", "sizes.webdl_1080p", "rules.3.is", ""
 *   for the definition as a whole)
 * @message: a sentence for the person, never with any of the input in it
 */
typedef struct
{
	gchar *field;
	gchar *message;
} AxisQualityProblem;

/* A new array of problems (freed with the array). */
GPtrArray *
axis_quality_problems_new(void);

/* The message of the first problem about @field, or %NULL. */
const gchar *
axis_quality_problems_find(
	const GPtrArray *problems,
	const gchar     *field
);

/* The checks of the values, for a profile built in code (a form): parse()
 * applies them too.  Every problem is appended to @problems (nullable). */
gboolean
axis_quality_profile_check(
	const AxisQualityProfile *profile,
	GPtrArray                *problems
);

/**
 * axis_quality_profile_parse:
 * @text: (nullable): the definition
 * @length: its length in bytes (-1: up to the first NUL)
 * @profile: (out caller-allocates) (optional): the result; zero (and not to
 *   be used) when it is refused
 * @problems: (nullable): every problem found is appended
 *
 * The only way in: every stored definition is read through it, and a
 * refused one is not used at all.  Pure.
 *
 * Returns: %TRUE when the definition is valid
 */
gboolean
axis_quality_profile_parse(
	const gchar        *text,
	gssize              length,
	AxisQualityProfile *profile,
	GPtrArray          *problems
);

/* The format a definition says it is in (its "format" member), 0 when it
 * says none or is not a JSON object: for telling a newer version's
 * definition from a broken one. */
gint
axis_quality_profile_format_of(
	const gchar *text,
	gssize       length
);

/* The canonical text (what parse() reads back; an invalid profile writes
 * what parse() refuses). */
gchar *
axis_quality_profile_canonical(const AxisQualityProfile *profile);

/* Whether two profiles mean the same (their canonical texts are equal). */
gboolean
axis_quality_profile_equal(
	const AxisQualityProfile *a,
	const AxisQualityProfile *b
);

/**
 * AxisQualityFact:
 *
 * One line of a profile in words: @label ("Qualities") and @text.
 */
typedef struct
{
	const gchar *label;
	gchar       *text;
} AxisQualityFact;

/* The profile in words, a fact per member (AxisQualityFact, freed with the
 * array). */
GPtrArray *
axis_quality_profile_describe(const AxisQualityProfile *profile);

/* What the parser read from a name, in words: its title (always), then a
 * fact for every field it found, its quality on the ladder with why, and
 * the words it did not understand (AxisQualityFact, freed with the array). */
GPtrArray *
axis_quality_release_describe(const AxisRelease *release);

/* ── Built-in profiles ──────────────────────────────────────────── */

typedef struct
{
	const gchar *slug;	/* reserved: no other profile may have it */
	const gchar *name;
	const gchar *summary;
} AxisQualityBuiltin;

#define AXIS_QUALITY_N_BUILTINS 5

/* The default profile for a new title, until a setting says another. */
#define AXIS_QUALITY_DEFAULT_SLUG "balanced"

/* Space saver, Balanced, Best 1080p, 4K, Anything: in page order. */
const AxisQualityBuiltin *
axis_quality_builtins(void);

const AxisQualityBuiltin *
axis_quality_builtin(const gchar *slug);

/* The built-in's definition, valid; FALSE for a slug that is not one. */
gboolean
axis_quality_builtin_profile(
	const gchar        *slug,
	AxisQualityProfile *profile
);

/* Whether @slug is a well-formed profile slug ([a-z][a-z0-9_]{0,40}). */
gboolean
axis_quality_slug_valid(const gchar *slug);

/* Whether @slug is one a user's profile may never take (a built-in's, or
 * anything but custom_<digits>... is checked by the store). */
gboolean
axis_quality_slug_reserved(const gchar *slug);

#define AXIS_QUALITY_NAME_MAX_CHARS 80

/* A profile name as stored: control and invisible characters dropped or
 * made spaces, runs of spaces one, none at the ends, at most 80 characters;
 * %NULL for text that is not UTF-8 or of which nothing is left. */
gchar *
axis_quality_clean_name(const gchar *text);

/* ── The decision ───────────────────────────────────────────────── */

/**
 * AxisQualityTitle:
 * @noun: what a release of it holds, for the sentences ("film",
 *   "episode", "season"); %NULL is "film"
 * @runtime_minutes: the runtime of what a release should hold (a film's;
 *   for episodes, the caller's sum), 0 when not known: then only the
 *   absolute size limit applies
 * @languages: (nullable) (array zero-terminated=1): the languages wanted,
 *   ISO 639-1 codes; empty or %NULL for no check.  A release that states
 *   languages, none of them wanted, and no multi or dual audio is rejected;
 *   one that states none is taken to be in the original language.
 */
typedef struct
{
	const gchar        *noun;
	gint                runtime_minutes;
	const gchar *const *languages;
} AxisQualityTitle;

/**
 * AxisQualityOnDisk:
 * @quality: the file's quality as recorded for it (from the name it came
 *   in with)
 * @fields: (nullable): what was read from its name, for its score and its
 *   revision (none: score 0, the original)
 * @name: (nullable): that name, for the word condition
 * @converted: Axis converted the file (its conversion record says so)
 * @source_quality: for a converted file, the ladder quality recorded for
 *   the file it was made from; unknown when that was not recorded
 * @resolution: for a file of unknown @quality whose resolution is known (a
 *   file found in the library whose name states no source): that
 *   resolution, and the file is "an unknown source at that resolution"
 *   (see axis_quality_unknown_source_label()); NONE otherwise
 *
 * A file Axis converted counts as its source's quality, so a remux shrunk
 * to HEVC is still "Remux 1080p" and a BluRay encode is not an upgrade over
 * it.  When its source's quality was not recorded, nothing is taken to be
 * better than it.  A file of an unknown source at a resolution is not
 * upgraded at all unless the profile's upgrade_unknown says so; a file of
 * unknown quality with no resolution known is replaced by any acceptable
 * known quality (as before).
 */
typedef struct
{
	AxisQuality            quality;
	const AxisRelease     *fields;
	const gchar           *name;
	gboolean               converted;
	AxisQuality            source_quality;
	AxisReleaseResolution  resolution;
} AxisQualityOnDisk;

/**
 * AxisQualityCandidate:
 * @release: what the parser read from @name
 * @name: (nullable): the name, for the word condition
 * @match: the matcher's verdict for the title asked for
 * @match_reason: (nullable): the matcher's words for it, quoted in the
 *   reason when it is not a match
 * @size_bytes: -1 when not known
 * @protocol: usenet or torrent
 * @seeders: -1 when not known (always for Usenet)
 * @published_us: microseconds since 1970, 0 when not known
 */
typedef struct
{
	const AxisRelease    *release;
	const gchar          *name;
	AxisReleaseMatch      match;
	const gchar          *match_reason;
	gint64                size_bytes;
	AxisDownloadProtocol  protocol;
	gint                  seeders;
	gint64                published_us;
} AxisQualityCandidate;

typedef enum
{
	AXIS_QUALITY_PROTOCOL_ANY = 0,
	AXIS_QUALITY_PROTOCOL_USENET,
	AXIS_QUALITY_PROTOCOL_TORRENT
} AxisQualityProtocolPreference;

/**
 * AxisQualityPlayersFunc:
 * @want: a combination the release would need played directly (a video
 *   stream, or one audio codec)
 * @user_data: (closure)
 *
 * The household's verdict for @want (axis-compat-verdict.h): risky when a
 * player is known to need it converted.  Pure: it must not block.
 */
typedef AxisCompatVerdict (*AxisQualityPlayersFunc)(
	const AxisCompatCombo *want,
	gpointer               user_data
);

/**
 * AxisQualityContext:
 * @profile: a valid profile
 * @title: (nullable): what is searched for (%NULL: a film of unknown
 *   runtime, any language)
 * @disk: (nullable): the file on disk, %NULL for none
 * @protocol: the protocol preference (a setting)
 * @retention_days: the Usenet provider's retention, 0 when not known (no
 *   release is too old)
 * @now_us: the time (microseconds since 1970), for ages
 * @players: (nullable): the household's verdicts, for the players
 *   preference (%NULL: none known)
 * @players_data: (closure players)
 * @no_title: only for trying a name with no title to match it against (the
 *   Quality page): the match is not checked, and every verdict says so.
 *   Searches never set it.
 */
typedef struct
{
	const AxisQualityProfile      *profile;
	const AxisQualityTitle        *title;
	const AxisQualityOnDisk       *disk;
	AxisQualityProtocolPreference  protocol;
	gint                           retention_days;
	gint64                         now_us;
	AxisQualityPlayersFunc         players;
	gpointer                       players_data;
	gboolean                       no_title;
} AxisQualityContext;

/**
 * AxisQualityReasonKind:
 * @AXIS_QUALITY_REASON_SAFETY: a fixed rule: no profile relaxes it, and no
 *   one may grab such a release by hand either
 * @AXIS_QUALITY_REASON_PROFILE: the profile (qualities, sizes, rules,
 *   languages) or the release's availability
 * @AXIS_QUALITY_REASON_UPGRADE: not better than the file on disk
 */
typedef enum
{
	AXIS_QUALITY_REASON_SAFETY,
	AXIS_QUALITY_REASON_PROFILE,
	AXIS_QUALITY_REASON_UPGRADE
} AxisQualityReasonKind;

/**
 * AxisQualityReason:
 * @code: a stable identifier: "sample", "password", "archive",
 *   "executable", "post_subject", "no_title", "no_match", "ambiguous", "unwanted",
 *   "not_allowed", "unknown_quality", "too_large", "too_small",
 *   "over_limit", "rule_require", "rule_reject", "language",
 *   "retention", "profile_invalid", "upgrades_off", "cutoff", "not_better"
 * @text: the sentence
 */
typedef struct
{
	AxisQualityReasonKind  kind;
	const gchar           *code;
	gchar                 *text;
} AxisQualityReason;

/* One part of a score: "+100: HEVC (x265) is preferred". */
typedef struct
{
	gint   points;
	gchar *text;
} AxisQualityScoreLine;

/**
 * AxisQualityKey:
 *
 * The ranking's keys, in the order they are compared.
 */
typedef enum
{
	AXIS_QUALITY_KEY_TIER = 0,
	AXIS_QUALITY_KEY_SCORE,
	AXIS_QUALITY_KEY_REVISION,
	AXIS_QUALITY_KEY_PROTOCOL,
	AXIS_QUALITY_KEY_AVAILABILITY,
	AXIS_QUALITY_KEY_SIZE,
	AXIS_QUALITY_KEY_NONE
} AxisQualityKey;

/**
 * AxisQualityKeys:
 * @tier: the release's tier in the profile (0 is the first; unknown is
 *   after the last; a quality not allowed is after that)
 * @score: the preference score
 * @revision: 0 the original; 1 a proper, repack or rerip (or v2); 2 a second
 *   one (REAL.PROPER, v3) and so on
 * @preferred: of the preferred protocol (always %FALSE without one)
 * @protocol: compared within a protocol only
 * @availability: torrent: the seeders' band (0: none, 1: 1 to 4, 2: 5 to
 *   19, 3: 20 to 99, 4: 100 or more; -1 not known), higher first; Usenet:
 *   the age in whole days (-1 not known), younger first
 * @size: bytes, -1 not known; smaller first, the unknown last
 * @index: the position in the input: equals keep their order
 */
typedef struct
{
	guint                tier;
	gint                 score;
	gint                 revision;
	gboolean             preferred;
	AxisDownloadProtocol protocol;
	gint                 availability;
	gint64               size;
	guint                index;
} AxisQualityKeys;

/**
 * AxisQualityVerdict:
 * @quality: the release's quality on the ladder
 * @accepted: acceptable and (with a file on disk) an upgrade
 * @safety: rejected by a fixed rule (see AXIS_QUALITY_REASON_SAFETY)
 * @acceptable: nothing but the file on disk stands against it
 * @upgrade: with a file on disk: better than it by the profile's rules
 * @upgrade_text: (nullable): why it is an upgrade
 * @reasons: (element-type AxisQualityReason): every reason it is rejected,
 *   in a fixed order (fixed rules, then quality, size, rules, language,
 *   availability, the file on disk); empty when accepted
 * @lines: (element-type AxisQualityScoreLine): how @score was made
 * @notes: (element-type utf8): what was not checked, and why
 * @rank: 1 for the best of the accepted; 0 when rejected
 */
typedef struct
{
	AxisQuality      quality;
	gboolean         accepted;
	gboolean         safety;
	gboolean         acceptable;
	gboolean         upgrade;
	gchar           *upgrade_text;
	GPtrArray       *reasons;
	gint             score;
	GPtrArray       *lines;
	GPtrArray       *notes;
	AxisQualityKeys  keys;
	guint            rank;
} AxisQualityVerdict;

void
axis_quality_verdict_free(AxisQualityVerdict *verdict);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisQualityVerdict, axis_quality_verdict_free)

/**
 * AxisQualityDecision:
 * @verdicts: (element-type AxisQualityVerdict): one per release, in the
 *   order given
 * @order: indices into @verdicts: the accepted best first, then the
 *   rejected in the order given
 * @n_accepted: how many were accepted
 * @cutoff_reached: the file on disk is at or above the cutoff (or upgrades
 *   are off, or it is a converted file of unrecorded source): no release is
 *   an upgrade
 * @disk_text: (nullable): the file on disk in words, with a file
 */
typedef struct
{
	GPtrArray *verdicts;
	guint     *order;
	guint      n_accepted;
	gboolean   cutoff_reached;
	gchar     *disk_text;
} AxisQualityDecision;

void
axis_quality_decision_free(AxisQualityDecision *decision);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisQualityDecision, axis_quality_decision_free)

/**
 * axis_quality_decide:
 * @context: the profile and what is around the releases
 * @candidates: (array length=n): the releases
 * @n: how many
 *
 * Every release accepted or rejected with every reason, the accepted ranked
 * (axis_quality_keys_compare()), each said to be an upgrade or not when
 * there is a file on disk.  A profile that does not pass
 * axis_quality_profile_check() accepts nothing (every release is rejected
 * with that reason): fail closed.
 *
 * Returns: (transfer full): the decision
 */
AxisQualityDecision *
axis_quality_decide(
	const AxisQualityContext   *context,
	const AxisQualityCandidate *candidates,
	guint                       n
);

/* One release alone (rank 1 when accepted). */
AxisQualityVerdict *
axis_quality_decide_one(
	const AxisQualityContext   *context,
	const AxisQualityCandidate *candidate
);

/**
 * axis_quality_keys_compare:
 * @deciding: (out) (optional): the key that decided (%AXIS_QUALITY_KEY_NONE
 *   for equals, kept in their order)
 *
 * The ranking: quality position in the profile, then the preference score,
 * then the revision (repack or proper over the original), then the
 * preferred protocol, then within one protocol the seeders' band (torrents)
 * or the age (Usenet, younger first), then the smaller size; equals keep
 * the order given.
 *
 * Returns: negative when @a ranks above @b
 */
gint
axis_quality_keys_compare(
	const AxisQualityKeys *a,
	const AxisQualityKeys *b,
	AxisQualityKey        *deciding
);

/**
 * axis_quality_explain_rank:
 *
 * Why @a ranks above @b, in words ("a higher score: 120 against 20"), for
 * the pages.
 *
 * Returns: (transfer full): the text
 */
gchar *
axis_quality_explain_rank(
	const AxisQualityProfile *profile,
	const AxisQualityVerdict *a,
	const AxisQualityVerdict *b
);

/* "2.9 GiB", "850 MiB", "25 GiB": binary units, as every page writes a size
 * (axis_size_format()); "an unknown size" for a negative one. */
gchar *
axis_quality_format_size(gint64 bytes);

/* ── The players preference ─────────────────────────────────────── */

/**
 * AxisQualityDevice:
 *
 * One player's evidence (the rows of its capability table), for
 * axis_quality_players_verdict().
 */
typedef struct
{
	const AxisCompatObservation *obs;
	guint                        n_obs;
} AxisQualityDevice;

typedef struct
{
	const AxisQualityDevice *devices;
	guint                    n_devices;
} AxisQualityPlayers;

/**
 * axis_quality_players_verdict:
 * @want: a combination
 * @user_data: an #AxisQualityPlayers
 *
 * An #AxisQualityPlayersFunc over a precomputed table of the household's
 * players: the worst of the devices' verdicts (risky, then unknown, then
 * safe); unknown for no device.
 */
AxisCompatVerdict
axis_quality_players_verdict(
	const AxisCompatCombo *want,
	gpointer               user_data
);

/* What a release needs played: its video (index 0, when it states a codec)
 * and each audio codec it states, as compatibility combinations whose
 * strings are static.  Returns how many were written (at most @max). */
guint
axis_quality_release_combos(
	const AxisRelease *release,
	AxisCompatCombo   *combos,
	guint              max
);

G_END_DECLS

#endif /* AXIS_QUALITY_H */
