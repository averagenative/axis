/*
 * axis-import-name.h - the names Axis gives what it imports, and what it
 * reads from the names it finds: the film's folder and file, the character
 * rule, the byte budget, subtitles' languages and marks, and which files and
 * folders are extras, samples or nothing to import
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ONE SCHEME, read by Plex and by Jellyfin (openspec/specs/import/spec.md;
 * both servers document these names):
 *
 *   folder  Title (Year) {tmdb-12345}
 *   file    Title (Year) {tmdb-12345} {edition-Director's Cut} [WEB-DL 1080p][HDR10][DTS-HD MA 5.1][x265]-GROUP.mkv
 *
 * The file always begins with the folder's text (Jellyfin needs that), and
 * what follows it is the file format (a setting, AXIS_IMPORT_DEFAULT_FORMAT):
 * literal text and tokens, a token being "{" optional prefix, a name, optional
 * suffix "}" ("{[quality]}", "{-group}", "{ edition}").  A token whose value is
 * absent is left out with its prefix and suffix.  The tokens are edition,
 * quality, hdr, audio, codec and group.  Runs of spaces in what the format
 * gave are made one and it is trimmed at its end.  The layouts: "tmdb" (the
 * one above; an IMDb identifier, "{imdb-tt...}", when there is no TMDb one),
 * "jellyfin" ("[tmdbid-12345]", "[imdbid-tt...]") and "plain" (no
 * identifier).
 *
 * THE CHARACTER RULE, the same for every name and every value put in one:
 * '/' and '\' become '-'; ':' becomes " -"; control characters and the
 * invisible direction controls (U+061C, U+200E, U+200F, U+202A to U+202E,
 * U+2066 to U+2069) are dropped; text that is not valid UTF-8 has its bad
 * bytes replaced by U+FFFD; leading spaces and dots are removed (a name that
 * begins with a dot is hidden from both media servers) and so are trailing
 * dots and spaces.  Everything else is kept as the title has it: the
 * filesystems here are Linux.  A title with nothing left is "Untitled".
 *
 * THE BUDGET: every component is at most AXIS_IMPORT_NAME_BUDGET bytes, 255
 * less the longest temporary name (".axis-partial-" and a 20-digit number),
 * so that the temporary name the file is written under before it gets its
 * final one fits in 255 too.  What is cut to fit, in order: the format's
 * tokens from its last to its first (the bracketed part first, the edition
 * last of them), then the title, on a character boundary, never to nothing.
 * The year, the identifier and the extension are never cut.
 *
 * EPISODES (a series' file: @episode set in the input) go one level deeper,
 * in a season's folder, and their file names state the episodes:
 *
 *   folder  Series Title (Year) {tvdb-350001}          ({tmdb-N} without TVDB)
 *   season  Season 01                                  (specials: Season 00)
 *   file    Series Title (Year) - S01E02 - Episode Title [WEB-DL 1080p]-GROUP.mkv
 *
 * A file holding several episodes names them as a range when they follow
 * one another ("S01E02-E03"), else each ("S01E02E05").  The file begins
 * with the series' title and year (not its identifier: that is the
 * folder's).  The format is the same setting; its "airdate" token is the
 * episode's air date ("2024-03-15"), for daily shows.  The same character
 * rule applies, and the same budget, cut in this order: the episode title
 * first (to nothing, and then its " - " with it), then the format's tokens
 * from the last, then the series' title (never to nothing).  The episode
 * mark, the year and the extension are never cut.
 *
 * GLib only, no I/O: tests/fuzz/fuzz-import-name.c and
 * tests/fuzz/fuzz-import-subtitle.c fuzz it; the Naming page's preview calls
 * axis_import_name_build() exactly as an import does.
 */

#ifndef AXIS_IMPORT_NAME_H
#define AXIS_IMPORT_NAME_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_IMPORT_TEMP_PREFIX     ".axis-partial-"
#define AXIS_IMPORT_TEMP_MAX        (sizeof(AXIS_IMPORT_TEMP_PREFIX) - 1 + 20)
#define AXIS_IMPORT_COMPONENT_MAX   255
#define AXIS_IMPORT_NAME_BUDGET     (AXIS_IMPORT_COMPONENT_MAX - AXIS_IMPORT_TEMP_MAX)

/* The recycle folder at the top of every volume. */
#define AXIS_IMPORT_RECYCLE_DIR     ".axis-recycle"

/* The longest file format, in bytes; an edition's longest text (Plex reads
 * at most 32 characters). */
#define AXIS_IMPORT_FORMAT_MAX      200
#define AXIS_IMPORT_EDITION_CHARS   32

#define AXIS_IMPORT_DEFAULT_FORMAT  "{ edition}{ [quality]}{[hdr]}{[audio]}{[codec]}{-group}"

typedef enum
{
	AXIS_IMPORT_LAYOUT_TMDB,
	AXIS_IMPORT_LAYOUT_JELLYFIN,
	AXIS_IMPORT_LAYOUT_PLAIN
} AxisImportLayout;

/* "tmdb", "jellyfin", "plain" (the setting's choices). */
const gchar *
axis_import_layout_name(AxisImportLayout layout);

gboolean
axis_import_layout_from_name(
	const gchar      *name,
	AxisImportLayout *layout
);

/**
 * axis_import_name_clean:
 * @text: (nullable): a title, a value or a name; bytes, of any form
 * @length: its length, or -1 when it ends at a NUL
 *
 * The character rule above.
 *
 * Returns: (transfer full): the text, possibly "" (never %NULL)
 */
gchar *
axis_import_name_clean(
	const gchar *text,
	gssize       length
);

/**
 * axis_import_format_check:
 * @format: (nullable): a file format as typed
 * @reason: (out) (optional) (transfer full): why it is refused, a sentence
 *   that does not repeat it
 *
 * A format is valid UTF-8 of at most %AXIS_IMPORT_FORMAT_MAX bytes, without
 * control characters, '/' or '\', with braces only around tokens (not
 * nested, each closed); a token is "{", a prefix and a suffix of spaces and
 * "-_.[]()" around one of the token names, "}".  Empty is valid (the file
 * is then the folder's text and the extension).  This is the setting's
 * validator.
 *
 * Returns: %TRUE if @format is acceptable
 */
gboolean
axis_import_format_check(
	const gchar  *format,
	gchar       **reason
);

/**
 * AxisImportNameInput:
 * @title: the title's name (as the metadata has it)
 * @year: 0 for none
 * @tmdb_id: (nullable): digits
 * @imdb_id: (nullable): "tt" and digits
 * @layout: the folder's form
 * @format: (nullable): the file format; %NULL is AXIS_IMPORT_DEFAULT_FORMAT
 * @edition, @quality, @hdr, @audio, @codec, @group: (nullable): the tokens'
 *   values
 * @extension: the file's extension with its dot (".mkv"); lower-cased here
 * @keep: (nullable): with renaming off, the release's own file name, kept
 *   (cleaned and fitted to the budget) instead of the format
 * @budget: 0 for AXIS_IMPORT_NAME_BUDGET (tests make it smaller)
 * @episode: a series' episode file (see EPISODES above)
 * @tvdb_id: (nullable): digits: a series' TVDB identifier (its folder's,
 *   before @tmdb_id)
 * @season: the season's number (0: specials), with @episode
 * @episodes: (array length=n_episodes): the episodes' numbers in the season,
 *   1 to %AXIS_IMPORT_EPISODES_MAX of them, with @episode
 * @episode_title: (nullable): the (first) episode's title
 * @air_date: (nullable): "YYYY-MM-DD", the "airdate" token's value
 */
typedef struct
{
	const gchar      *title;
	gint              year;
	const gchar      *tmdb_id;
	const gchar      *imdb_id;
	AxisImportLayout  layout;
	const gchar      *format;
	const gchar      *edition;
	const gchar      *quality;
	const gchar      *hdr;
	const gchar      *audio;
	const gchar      *codec;
	const gchar      *group;
	const gchar      *extension;
	const gchar      *keep;
	gsize             budget;
	gboolean          episode;
	const gchar      *tvdb_id;
	gint              season;
	const gint       *episodes;
	guint             n_episodes;
	const gchar      *episode_title;
	const gchar      *air_date;
} AxisImportNameInput;

/* At most this many episodes are named in one file's name. */
#define AXIS_IMPORT_EPISODES_MAX  20

/**
 * AxisImportName:
 * @folder: the title's folder
 * @season_folder: (nullable): an episode's season folder ("Season 01"), in
 *   @folder; %NULL for a film
 * @file: its file, with the extension
 * @stem: @file without the extension (what sidecars are named after)
 * @shortened: something was cut to fit the budget
 */
typedef struct
{
	gchar    *folder;
	gchar    *season_folder;
	gchar    *file;
	gchar    *stem;
	gboolean  shortened;
} AxisImportName;

void
axis_import_name_clear(AxisImportName *name);

/**
 * axis_import_name_build:
 * @problem: (out) (optional) (transfer none): why not, when it fails
 *
 * The names of one title's folder and file.  Fails only for an input that
 * cannot be named at all (an extension that is not one, a format that does
 * not pass axis_import_format_check(), a budget too small for the year, the
 * identifier and the extension with a one-character title).
 *
 * Returns: %TRUE with @name filled
 */
gboolean
axis_import_name_build(
	const AxisImportNameInput  *input,
	AxisImportName             *name,
	const gchar               **problem
);

/* Whether @name (bytes) is a name Axis may give: 1 to 255 bytes of valid
 * UTF-8, no '/', no control or direction character, not "." or "..", not
 * beginning with a dot, not ending with a dot or a space. */
gboolean
axis_import_name_valid(const gchar *name);

/* ── What a found name is ───────────────────────────────────────── */

/**
 * AxisImportFolder:
 * @AXIS_IMPORT_FOLDER_PLAIN: an ordinary folder: walked
 * @AXIS_IMPORT_FOLDER_IGNORED: samples, proofs, screens, generic extras, a
 *   dot-folder, NAS housekeeping: not walked
 * @AXIS_IMPORT_FOLDER_EXTRA: one of the standard extras folders ("Trailers",
 *   "Featurettes", ...): its videos are extras
 * @AXIS_IMPORT_FOLDER_SUBS: "Subs", "Subtitles": its subtitle files are the
 *   film's
 */
typedef enum
{
	AXIS_IMPORT_FOLDER_PLAIN,
	AXIS_IMPORT_FOLDER_IGNORED,
	AXIS_IMPORT_FOLDER_EXTRA,
	AXIS_IMPORT_FOLDER_SUBS
} AxisImportFolder;

/* What the folder called @name (bytes) is; @extra: for an extras folder,
 * the name both servers read it by ("Trailers", "Behind The Scenes"). */
AxisImportFolder
axis_import_folder_kind(
	const gchar  *name,
	gsize         length,
	const gchar **extra
);

/**
 * AxisImportFileKind:
 * @AXIS_IMPORT_FILE_IGNORED: a dot-file, "._" file, NAS or OS housekeeping
 * @AXIS_IMPORT_FILE_VIDEO: a video by its extension
 * @AXIS_IMPORT_FILE_SUBTITLE: .srt .ass .ssa .sub .idx .sup .vtt
 * @AXIS_IMPORT_FILE_ARCHIVE: .rar .r00 .zip .7z and the like (not unpacked)
 * @AXIS_IMPORT_FILE_OTHER: anything else (.nfo, images, text): not imported
 */
typedef enum
{
	AXIS_IMPORT_FILE_IGNORED,
	AXIS_IMPORT_FILE_VIDEO,
	AXIS_IMPORT_FILE_SUBTITLE,
	AXIS_IMPORT_FILE_ARCHIVE,
	AXIS_IMPORT_FILE_OTHER
} AxisImportFileKind;

AxisImportFileKind
axis_import_file_kind(
	const gchar *name,
	gsize        length
);

/* The extension of @name (bytes) with its dot, lower-cased, at most 16
 * bytes of ASCII letters and digits after the dot; %NULL when it has none. */
gchar *
axis_import_extension(
	const gchar *name,
	gsize        length
);

/* Whether a video called @name is an extra by its suffix ("-trailer",
 * "-featurette", ...), and @type the suffix's word ("trailer"). */
gboolean
axis_import_extra_suffix(
	const gchar  *name,
	gsize         length,
	const gchar **type
);

/**
 * axis_import_sample_marked:
 * @name: a file's name (bytes)
 * @title_words: (nullable): the title's own name, so that a film called
 *   "Sample" is not marked by its own title
 *
 * Whether the name marks the file as a sample: a word "sample" (letters and
 * digits between separators, case ignored) that is not one of the title's
 * own words, or "sample" right before the extension after a '-' or '.'.
 */
gboolean
axis_import_sample_marked(
	const gchar *name,
	gsize        length,
	const gchar *title_words
);

/* Whether the name reads as one part of a film split into several ("cd1",
 * "part2", "disc 1", "pt.3"), with @part the number (1 based). */
gboolean
axis_import_part_marked(
	const gchar *name,
	gsize        length,
	gint        *part
);

/* ── Subtitles ──────────────────────────────────────────────────── */

/**
 * AxisImportSubtitle:
 * @language: (nullable): ISO 639-1 ("en"), static; %NULL when it could not
 *   be read
 * @forced: marked forced (or foreign)
 * @sdh: marked SDH, CC or hearing impaired
 * @extension: ".srt" and the like, static
 */
typedef struct
{
	const gchar *language;
	gboolean     forced;
	gboolean     sdh;
	const gchar *extension;
} AxisImportSubtitle;

/**
 * axis_import_subtitle_read:
 * @name: a subtitle file's name (bytes)
 * @subtitle: (out): what it says
 *
 * Reads the words before the extension from the end: a language (a
 * two-letter ISO 639-1 code, a three-letter ISO 639-2 code, or the
 * language's English name), "forced"/"foreign", "sdh"/"cc"/"hi" (only beside
 * a language: alone "hi" is Hindi), "default" (ignored).  The first word
 * that is none of these ends the reading.
 *
 * Returns: %TRUE when @name is a subtitle file (by its extension)
 */
gboolean
axis_import_subtitle_read(
	const gchar        *name,
	gsize               length,
	AxisImportSubtitle *subtitle
);

/**
 * axis_import_subtitle_name:
 * @stem: the film's file without its extension
 * @subtitle: what was read
 * @number: 0 for none, else a number to tell apart subtitles that would
 *   have the same name (an unknown language always has one: ".und.1")
 *
 * "<stem>.<language>[.forced][.sdh][.<number>]<extension>", the stem cut
 * on a character boundary to fit the budget.
 *
 * Returns: (transfer full): the name
 */
gchar *
axis_import_subtitle_name(
	const gchar              *stem,
	const AxisImportSubtitle *subtitle,
	guint                     number
);

/* The name of an extra by suffix in the film's folder: its own name,
 * cleaned and fitted (the suffix kept). */
gchar *
axis_import_extra_name(
	const gchar *name,
	gsize        length
);

G_END_DECLS

#endif /* AXIS_IMPORT_NAME_H */
