/*
 * axis-profile.h - conversion profile definitions: the strict format, its
 * canonical form and hash, the lock rule, the quality rule, and the target
 * the estimator (and later the conversions) work from
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A profile is data; the rules are code.  A definition holds only what may
 * vary.  What may never vary (pinned files, Dolby Vision, uncertain or
 * unclassified HDR, HDR10+, interlaced or possibly interlaced video, an HDR
 * source to an SDR target, facts missing or inconsistent, a file or album
 * not estimated strictly smaller, albums converted whole, the minimum saving
 * floor) has no member here, so there is nothing to mis-set:
 * axis-estimate-eligibility.c applies those rules whatever the target says.
 *
 * DEFINITION FORMAT 1.  A JSON object (RFC 8259, at most
 * AXIS_PROFILE_MAX_TEXT bytes).  Every member below is required where it
 * applies and refused where it does not; any other member, a repeated
 * member, a wrong type (a string where a number goes, a number with a
 * fraction, true for 1) or a value out of bounds is an error.  Numbers are
 * whole; "26", "26.0" and "2.6e1" are the same number.
 *
 *  Both kinds:
 *   format              integer, 1
 *   kind                "video" or "audio"
 *   libraries           array of distinct strings, at least one: video
 *                       "movies", "tv", "other"; audio "music",
 *                       "audiobooks" (the roots it is for)
 *   codec               video "hevc" or "h264"; audio "flac", "opus",
 *                       "aac" or "mp3"
 *   min_saving_percent  integer, AXIS_PROFILE_MIN_SAVING_FLOOR (5) to 95:
 *                       the smallest saving worth converting a file (an
 *                       album) for, of its size
 *   min_saving_bytes    integer, 0 (none) to 10^12
 *
 *  Video only:
 *   bit_depth           integer: hevc 8 or 10, h264 8
 *   hdr                 "keep" (HDR10 and HLG sources become 10-bit HEVC
 *                       with their metadata; hevc only) or "skip" (an SDR
 *                       target: HDR sources are not converted).  There is
 *                       no value that tone-maps.
 *   encoders            array of 1 to 3 distinct of "software", "nvenc",
 *                       "vaapi", in order of preference
 *   quality             object: "default" (a quality number) and "rules"
 *                       (an array of 0 to AXIS_PROFILE_MAX_RULES objects,
 *                       first match wins).  A rule has "crf" and at least
 *                       one condition of "resolution" ("sd", "720p",
 *                       "1080p", "2160p", "other"), "min_bytes" (the file
 *                       is at least that large, 1 to 10^15) and
 *                       "max_bytes" (it is smaller, 1 to 10^15, above
 *                       min_bytes); the same rule twice is refused.
 *                       Quality numbers are x265/x264 CRF values the size
 *                       model has figures for: hevc 20, 22, ... 32; h264
 *                       18, 19, 20, 22, 24, 26, 28
 *   audio               "copy", "copy_add_stereo" or "stereo_first"
 *   stereo_bitrate      integer, 96000 to 320000 (the added stereo AAC
 *                       track): only with copy_add_stereo and stereo_first
 *   keep_languages      array of 0 to 16 distinct ISO 639-2 codes (three
 *                       lower-case letters, not "und"): audio tracks in
 *                       other known languages are dropped; empty keeps all.
 *                       A track of unknown language is always kept, and a
 *                       file that would be left without sound is not
 *                       converted.
 *   subtitles           "copy" or "text_only" (mp4 needs text_only)
 *   container           "mkv" or "mp4"
 *   efficient_bpp_milli integer, 10 to 1000: a source already in the
 *                       target's codec at or below this many thousandths of
 *                       a bit per pixel is already efficient
 *
 *  Audio only:
 *   bitrate             opus 16000 to 256000, aac 32000 to 320000; opus
 *                       and aac only
 *   mono_bitrate        optional, opus and aac only: for a mono source,
 *                       below bitrate, in the same bounds
 *   vbr_quality         mp3 only: LAME V0 to V4 (0 to 4)
 *   reduce_to_cd        boolean, flac only: more than 16 bits or 48 kHz to
 *                       16 bits at 44.1 kHz
 *   allow_lossy_source  boolean, opus, aac and mp3 only
 *   efficient_percent   integer, 100 to 400, opus, aac and mp3 only: a
 *                       source in the target's codec at or below this
 *                       share of the target's bitrate is already efficient
 *
 * CANONICAL FORM.  Members in byte order of their names, no white space,
 * whole numbers in decimal, the sets (libraries, keep_languages) in their
 * fixed order, the quality rules in a normal order (adjacent rules that
 * cannot both match a file, or that give the same number, are put in a
 * fixed order: they mean the same in either), absent members absent.  The
 * hash is the SHA-256 of the canonical text, in lower-case hex.  So member
 * order, white space, number spelling and the order of sets and of
 * interchangeable rules do not change the hash.
 *
 * GLib only, no I/O: there are fuzz targets (tests/fuzz/fuzz-profile.c,
 * tests/fuzz/fuzz-estimate.c).
 */

#ifndef AXIS_PROFILE_H
#define AXIS_PROFILE_H

#include <glib.h>

#include "storage/axis-storage.h"
#include "transcode/axis-estimate-target.h"

G_BEGIN_DECLS

#define AXIS_PROFILE_FORMAT            1
#define AXIS_PROFILE_MAX_TEXT          8192
#define AXIS_PROFILE_MAX_RULES         AXIS_ESTIMATE_MAX_QUALITY_RULES
#define AXIS_PROFILE_MAX_LANGUAGES     16
#define AXIS_PROFILE_MIN_SAVING_FLOOR  5
#define AXIS_PROFILE_MAX_SAVING        95
#define AXIS_PROFILE_MAX_SAVING_BYTES  G_GINT64_CONSTANT(1000000000000)
#define AXIS_PROFILE_MAX_RULE_BYTES    G_GINT64_CONSTANT(1000000000000000)
#define AXIS_PROFILE_HASH_LENGTH       64

typedef enum
{
	AXIS_PROFILE_KIND_VIDEO,
	AXIS_PROFILE_KIND_AUDIO
} AxisProfileKind;

/* "video", "audio" */
const gchar *
axis_profile_kind_to_string(AxisProfileKind kind);

gboolean
axis_profile_kind_from_string(
	const gchar     *text,
	AxisProfileKind *kind
);

/**
 * AxisProfileRule:
 * @resolution: 0 for any, else 1 + the index of the class in
 *   axis_profile_resolutions()
 * @min_bytes: 0 for none, else the file is at least this large
 * @max_bytes: 0 for none, else the file is smaller than this
 * @crf: the quality number
 */
typedef struct
{
	gint   resolution;
	gint64 min_bytes;
	gint64 max_bytes;
	gint   crf;
} AxisProfileRule;

/**
 * AxisProfileDefinition:
 *
 * A definition, as parsed.  A plain value: copy it with =.  Members that do
 * not apply to the kind and codec are zero (vbr_quality -1).
 */
typedef struct
{
	AxisProfileKind      kind;
	guint                libraries;	/* AXIS_TARGET_ROOT bits */
	AxisTargetCodec      codec;
	/* Video. */
	gint                 bit_depth;
	gboolean             keep_hdr;
	AxisEncoderFamily    encoders[AXIS_ENCODER_COUNT];
	guint                n_encoders;
	gint                 crf;	/* the default quality number */
	AxisProfileRule      rules[AXIS_PROFILE_MAX_RULES];
	guint                n_rules;
	AxisAudioHandling    audio;
	gint                 stereo_bitrate;
	gchar                languages[AXIS_PROFILE_MAX_LANGUAGES][4];
	guint                n_languages;
	AxisSubtitleHandling subtitles;
	AxisTargetContainer  container;
	gint                 efficient_milli;
	/* Audio. */
	gint                 bitrate;
	gint                 mono_bitrate;	/* 0 for none */
	gint                 vbr_quality;	/* mp3: 0 to 4, else -1 */
	gboolean             reduce_to_cd;
	gboolean             allow_lossy;
	gint                 efficient_percent;
	/* Both. */
	gint                 min_saving_percent;
	gint64               min_saving_bytes;
} AxisProfileDefinition;

/* "sd", "720p", "1080p", "2160p", "other", ended by %NULL. */
const gchar *const *
axis_profile_resolutions(void);

/* The library names of a kind ("movies", "tv", "other"; "music",
 * "audiobooks"), ended by %NULL, and the root type of each. */
const gchar *const *
axis_profile_library_names(AxisProfileKind kind);

gboolean
axis_profile_library_type(
	const gchar   *name,
	AxisMediaType *type
);

/* The quality numbers a codec accepts, ascending, ended by 0. */
const gint *
axis_profile_crf_values(AxisTargetCodec codec);

/* Whether @crf is one of them. */
gboolean
axis_profile_crf_valid(
	AxisTargetCodec codec,
	gint            crf
);

/* The nominal bitrate of LAME V@quality (0 to 4), from LAME's own table. */
gint
axis_profile_mp3_bitrate(gint quality);

/* ── Problems ───────────────────────────────────────────────────── */

/**
 * AxisProfileProblem:
 * @field: the member it is about ("codec", "quality.rules.2.crf", "" for
 *   the definition as a whole)
 * @message: a sentence for the person, never with any of the input in it
 */
typedef struct
{
	gchar *field;
	gchar *message;
} AxisProfileProblem;

void
axis_profile_problem_free(AxisProfileProblem *problem);

/* A new array of problems (freed with the array). */
GPtrArray *
axis_profile_problems_new(void);

/* The message of the first problem about @field, or %NULL. */
const gchar *
axis_profile_problems_find(
	const GPtrArray *problems,
	const gchar     *field
);

/* ── The one strict function ────────────────────────────────────── */

/**
 * axis_profile_definition_parse:
 * @text: (nullable): the definition
 * @length: its length in bytes (-1: up to the first NUL)
 * @def: (out caller-allocates) (optional): the result, in normal order;
 *   zero (and not to be used) when it is refused
 * @problems: (nullable): every problem found is appended
 *
 * The only way in: every write and every read of a stored definition goes
 * through it.  Pure.
 *
 * Returns: %TRUE when the definition is valid
 */
gboolean
axis_profile_definition_parse(
	const gchar           *text,
	gssize                 length,
	AxisProfileDefinition *def,
	GPtrArray             *problems
);

/* The checks of the values (bounds and what goes with what), for a
 * definition built in code (a form, a test): parse() applies them too. */
gboolean
axis_profile_definition_check(
	const AxisProfileDefinition *def,
	GPtrArray                   *problems
);

/* The canonical text of @def (which need not be valid: what it writes is
 * what parse() would read back, so an invalid one is refused there). */
gchar *
axis_profile_definition_canonical(const AxisProfileDefinition *def);

/* The SHA-256 of @canonical, lower-case hex. */
gchar *
axis_profile_hash(const gchar *canonical);

/* Whether two definitions mean the same (their canonical texts are equal). */
gboolean
axis_profile_definition_equal(
	const AxisProfileDefinition *a,
	const AxisProfileDefinition *b
);

/* The quality number for a file of @size bytes at @resolution_class: the
 * first rule that matches, else the default. */
gint
axis_profile_quality_for(
	const AxisProfileDefinition *def,
	const gchar                 *resolution_class,
	gint64                       size
);

/* ── The lock ───────────────────────────────────────────────────── */

/**
 * AxisProfileMemberClass:
 * @AXIS_PROFILE_MEMBER_OUTPUT: a change can alter the bytes produced: the
 *   new revision is locked
 * @AXIS_PROFILE_MEMBER_SELECTION: a change alters only which files are
 *   converted, never what is made of one: the proof carries forward
 * @AXIS_PROFILE_MEMBER_NARROWING: a change that only narrows (fewer
 *   libraries, lossy sources no longer allowed) carries the proof; any
 *   other locks
 */
typedef enum
{
	AXIS_PROFILE_MEMBER_OUTPUT,
	AXIS_PROFILE_MEMBER_SELECTION,
	AXIS_PROFILE_MEMBER_NARROWING
} AxisProfileMemberClass;

typedef struct
{
	const gchar            *name;	/* the member, as in the format */
	const gchar            *label;	/* "Quality rule" */
	AxisProfileMemberClass  klass;
} AxisProfileMember;

/* Every member of the format with its class, ended by a %NULL name. */
const AxisProfileMember *
axis_profile_members(void);

/**
 * axis_profile_carries_proof:
 * @before: the earlier revision's definition
 * @after: the new one
 * @changed: (nullable): the members that differ are appended (static
 *   strings), with a locking member first
 *
 * The lock rule, as one pure function: whether a revision made from
 * @before by changing it into @after keeps @before's proof.  Only when
 * every difference is of a member that cannot change the bytes produced (a
 * different kind, or anything in doubt, locks).  Equal definitions carry
 * it.
 */
gboolean
axis_profile_carries_proof(
	const AxisProfileDefinition *before,
	const AxisProfileDefinition *after,
	GPtrArray                   *changed
);

/* ── Targets ────────────────────────────────────────────────────── */

/**
 * AxisProfileIdentity:
 *
 * What a target built from a definition is called: @slug, @name, @family
 * and @summary are copied into the string pool (g_intern_string()), so the
 * target needs nothing to outlive it.
 */
typedef struct
{
	const gchar *slug;
	const gchar *name;
	const gchar *family;	/* %NULL: from the kind and libraries */
	const gchar *summary;	/* %NULL: none */
	gint64       revision_id;
	gint         revision;
} AxisProfileIdentity;

/**
 * axis_profile_to_target:
 * @def: a valid definition
 *
 * The one way a target is made: the built-in targets go through it too.
 * Returns FALSE (and leaves @target zero) for a definition that does not
 * pass axis_profile_definition_check(): fail closed.
 */
gboolean
axis_profile_to_target(
	const AxisProfileDefinition *def,
	const AxisProfileIdentity   *identity,
	AxisEstimateTarget          *target
);

/* "Space saver"'s family for a definition: "Video", "Music" or
 * "Audiobooks". */
const gchar *
axis_profile_family(const AxisProfileDefinition *def);

/**
 * AxisProfileFact:
 *
 * One line of a definition in words: @label ("Quality") and @text ("CRF 26;
 * CRF 28 for 2160p of 50 GB or more").
 */
typedef struct
{
	const gchar *label;
	gchar       *text;
} AxisProfileFact;

/* The definition in words, a fact per member (AxisProfileFact, freed with
 * the array). */
GPtrArray *
axis_profile_describe(const AxisProfileDefinition *def);

/* ── Built-ins ──────────────────────────────────────────────────── */

typedef struct
{
	const gchar *slug;	/* reserved: no other profile may have it */
	const gchar *name;
	const gchar *family;
	const gchar *summary;
	const gchar *definition;	/* canonical text */
} AxisProfileBuiltin;

#define AXIS_PROFILE_N_BUILTINS AXIS_ESTIMATE_N_TARGETS

/* The built-ins, in page order. */
const AxisProfileBuiltin *
axis_profile_builtins(void);

const AxisProfileBuiltin *
axis_profile_builtin(const gchar *slug);

/* Whether @slug is one a profile of a user may never take. */
gboolean
axis_profile_slug_reserved(const gchar *slug);

/* Whether @slug is a well-formed profile slug ([a-z][a-z0-9_]{0,40}). */
gboolean
axis_profile_slug_valid(const gchar *slug);

/* ── Names ──────────────────────────────────────────────────────── */

#define AXIS_PROFILE_NAME_MAX_CHARS 80
#define AXIS_PROFILE_NOTE_MAX_CHARS 500

/**
 * axis_profile_clean_name:
 *
 * A profile name or note as it is stored: control characters and line
 * breaks become spaces (a note keeps its line breaks), runs of spaces one,
 * no space at either end, at most @max_chars characters.  %NULL for text
 * that is not UTF-8 or nothing is left of.
 */
gchar *
axis_profile_clean_text(
	const gchar *text,
	guint        max_chars,
	gboolean     multiline
);

G_END_DECLS

#endif /* AXIS_PROFILE_H */
