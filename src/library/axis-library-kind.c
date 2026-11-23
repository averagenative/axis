/*
 * axis-library-kind.c - what a file is, from its name, and which names the
 * scanner leaves out
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-library-kind.h"
#include "library/axis-library-name.h"

#include <string.h>

static const gchar *const kind_names[] = {
	"video", "audio", "subtitle", "image", "text", "other", "symlink"
};

static const gchar *const kind_labels[] = {
	"Video", "Audio", "Subtitles", "Images", "Text and metadata", "Other", "Symbolic links"
};

G_STATIC_ASSERT(G_N_ELEMENTS(kind_names) == AXIS_FILE_KIND_COUNT);
G_STATIC_ASSERT(G_N_ELEMENTS(kind_labels) == AXIS_FILE_KIND_COUNT);

const gchar *
axis_file_kind_to_string(AxisFileKind kind)
{
	return (guint)kind < AXIS_FILE_KIND_COUNT ? kind_names[kind] : "other";
}

gboolean
axis_file_kind_from_string(
	const gchar  *text,
	AxisFileKind *kind
){
	guint i;

	for (i = 0; text != NULL && i < AXIS_FILE_KIND_COUNT; i++) {
		if (strcmp(text, kind_names[i]) == 0) {
			*kind = (AxisFileKind)i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
axis_file_kind_label(AxisFileKind kind)
{
	return (guint)kind < AXIS_FILE_KIND_COUNT ? kind_labels[kind] : "Other";
}

static const gchar *const kind_labels_one[] = {
	"Video", "Audio", "Subtitle", "Image", "Text or info file", "Other", "Symbolic link"
};

G_STATIC_ASSERT(G_N_ELEMENTS(kind_labels_one) == AXIS_FILE_KIND_COUNT);

const gchar *
axis_file_kind_label_one(AxisFileKind kind)
{
	return (guint)kind < AXIS_FILE_KIND_COUNT ? kind_labels_one[kind] : "Other";
}

/* Extensions, lower case, by kind. */
static const struct
{
	const gchar  *extension;
	AxisFileKind  kind;
} extensions[] = {
	{ "3gp",  AXIS_FILE_KIND_VIDEO },
	{ "avi",  AXIS_FILE_KIND_VIDEO },
	{ "divx", AXIS_FILE_KIND_VIDEO },
	{ "flv",  AXIS_FILE_KIND_VIDEO },
	{ "m2ts", AXIS_FILE_KIND_VIDEO },
	{ "m4v",  AXIS_FILE_KIND_VIDEO },
	{ "mkv",  AXIS_FILE_KIND_VIDEO },
	{ "mov",  AXIS_FILE_KIND_VIDEO },
	{ "mp4",  AXIS_FILE_KIND_VIDEO },
	{ "mpeg", AXIS_FILE_KIND_VIDEO },
	{ "mpg",  AXIS_FILE_KIND_VIDEO },
	{ "mts",  AXIS_FILE_KIND_VIDEO },
	{ "ogv",  AXIS_FILE_KIND_VIDEO },
	{ "rmvb", AXIS_FILE_KIND_VIDEO },
	{ "ts",   AXIS_FILE_KIND_VIDEO },
	{ "vob",  AXIS_FILE_KIND_VIDEO },
	{ "webm", AXIS_FILE_KIND_VIDEO },
	{ "wmv",  AXIS_FILE_KIND_VIDEO },

	{ "aac",  AXIS_FILE_KIND_AUDIO },
	{ "ac3",  AXIS_FILE_KIND_AUDIO },
	{ "aif",  AXIS_FILE_KIND_AUDIO },
	{ "aiff", AXIS_FILE_KIND_AUDIO },
	{ "alac", AXIS_FILE_KIND_AUDIO },
	{ "ape",  AXIS_FILE_KIND_AUDIO },
	{ "dff",  AXIS_FILE_KIND_AUDIO },
	{ "dsf",  AXIS_FILE_KIND_AUDIO },
	{ "dts",  AXIS_FILE_KIND_AUDIO },
	{ "flac", AXIS_FILE_KIND_AUDIO },
	{ "m4a",  AXIS_FILE_KIND_AUDIO },
	{ "m4b",  AXIS_FILE_KIND_AUDIO },
	{ "mka",  AXIS_FILE_KIND_AUDIO },
	{ "mp2",  AXIS_FILE_KIND_AUDIO },
	{ "mp3",  AXIS_FILE_KIND_AUDIO },
	{ "oga",  AXIS_FILE_KIND_AUDIO },
	{ "ogg",  AXIS_FILE_KIND_AUDIO },
	{ "opus", AXIS_FILE_KIND_AUDIO },
	{ "wav",  AXIS_FILE_KIND_AUDIO },
	{ "wma",  AXIS_FILE_KIND_AUDIO },
	{ "wv",   AXIS_FILE_KIND_AUDIO },

	{ "ass",  AXIS_FILE_KIND_SUBTITLE },
	{ "idx",  AXIS_FILE_KIND_SUBTITLE },
	{ "smi",  AXIS_FILE_KIND_SUBTITLE },
	{ "srt",  AXIS_FILE_KIND_SUBTITLE },
	{ "ssa",  AXIS_FILE_KIND_SUBTITLE },
	{ "sub",  AXIS_FILE_KIND_SUBTITLE },
	{ "sup",  AXIS_FILE_KIND_SUBTITLE },
	{ "ttml", AXIS_FILE_KIND_SUBTITLE },
	{ "vtt",  AXIS_FILE_KIND_SUBTITLE },

	{ "avif", AXIS_FILE_KIND_IMAGE },
	{ "bmp",  AXIS_FILE_KIND_IMAGE },
	{ "gif",  AXIS_FILE_KIND_IMAGE },
	{ "heic", AXIS_FILE_KIND_IMAGE },
	{ "jpeg", AXIS_FILE_KIND_IMAGE },
	{ "jpg",  AXIS_FILE_KIND_IMAGE },
	{ "png",  AXIS_FILE_KIND_IMAGE },
	{ "tif",  AXIS_FILE_KIND_IMAGE },
	{ "tiff", AXIS_FILE_KIND_IMAGE },
	{ "webp", AXIS_FILE_KIND_IMAGE },

	{ "cue",  AXIS_FILE_KIND_TEXT },
	{ "json", AXIS_FILE_KIND_TEXT },
	{ "log",  AXIS_FILE_KIND_TEXT },
	{ "m3u",  AXIS_FILE_KIND_TEXT },
	{ "m3u8", AXIS_FILE_KIND_TEXT },
	{ "md5",  AXIS_FILE_KIND_TEXT },
	{ "nfo",  AXIS_FILE_KIND_TEXT },
	{ "opf",  AXIS_FILE_KIND_TEXT },
	{ "sfv",  AXIS_FILE_KIND_TEXT },
	{ "txt",  AXIS_FILE_KIND_TEXT },
	{ "xml",  AXIS_FILE_KIND_TEXT },
	{ NULL, 0 }
};

AxisFileKind
axis_file_kind_from_name(
	const gchar *name,
	gsize        length
){
	gchar lower[8];
	gsize dot = length;
	gsize n;
	gsize i;

	/* The last '.', not the first character (".hidden" has none). */
	for (i = length; i > 1; i--) {
		if (name[i - 1] == '.') {
			dot = i - 1;
			break;
		}
	}
	if (dot == length || dot == 0)
		return AXIS_FILE_KIND_OTHER;

	n = length - dot - 1;
	if (n == 0 || n >= sizeof(lower))
		return AXIS_FILE_KIND_OTHER;
	for (i = 0; i < n; i++) {
		guchar c = (guchar)name[dot + 1 + i];

		if (c >= 0x80)
			return AXIS_FILE_KIND_OTHER;
		lower[i] = g_ascii_tolower((gchar)c);
	}
	lower[n] = '\0';

	for (i = 0; extensions[i].extension != NULL; i++) {
		if (strcmp(lower, extensions[i].extension) == 0)
			return extensions[i].kind;
	}

	return AXIS_FILE_KIND_OTHER;
}

/* ── Ignored names ──────────────────────────────────────────────── */

static gboolean
is(
	const gchar *name,
	gsize        length,
	const gchar *literal
){
	gsize n = strlen(literal);

	return length == n && memcmp(name, literal, n) == 0;
}

static gboolean
starts(
	const gchar *name,
	gsize        length,
	const gchar *prefix
){
	gsize n = strlen(prefix);

	return length >= n && memcmp(name, prefix, n) == 0;
}

/* A suffix, compared case-insensitively in ASCII; not the whole name. */
static gboolean
ends_nocase(
	const gchar *name,
	gsize        length,
	const gchar *suffix
){
	gsize n = strlen(suffix);

	return length > n && g_ascii_strncasecmp(name + length - n, suffix, n) == 0;
}

static gboolean
contains(
	const gchar *name,
	gsize        length,
	const gchar *needle
){
	gsize n = strlen(needle);
	gsize i;

	for (i = 0; n <= length && i <= length - n; i++) {
		if (memcmp(name + i, needle, n) == 0)
			return TRUE;
	}

	return FALSE;
}

AxisIgnoreReason
axis_library_ignore_builtin(
	const gchar *name,
	gsize        length,
	gboolean     is_dir
){
	static const gchar *const system_names[] = {
		"lost+found", "@eaDir", "@Recycle", "#recycle", "$RECYCLE.BIN", ".DS_Store",
		".AppleDouble", ".Trash", "System Volume Information", NULL
	};
	static const gchar *const partial_suffixes[] = {
		".part", ".partial", ".!qb", ".!ut", ".crdownload", ".aria2", ".filepart", ".ytdl", NULL
	};
	guint i;

	/* Axis's own: the volume marker and temporary files. */
	if (starts(name, length, ".axis-"))
		return AXIS_IGNORE_SYSTEM;
	for (i = 0; system_names[i] != NULL; i++) {
		if (is(name, length, system_names[i]))
			return AXIS_IGNORE_SYSTEM;
	}
	if (starts(name, length, ".Trash-"))
		return AXIS_IGNORE_SYSTEM;

	/* SABnzbd unpacks into _UNPACK_<name> before renaming it. */
	if (is_dir && starts(name, length, "_UNPACK_"))
		return AXIS_IGNORE_PARTIAL;
	if (!is_dir) {
		for (i = 0; partial_suffixes[i] != NULL; i++) {
			if (ends_nocase(name, length, partial_suffixes[i]))
				return AXIS_IGNORE_PARTIAL;
		}
		/* yt-dlp's fragments: name.part-Frag12 */
		if (contains(name, length, ".part-Frag"))
			return AXIS_IGNORE_PARTIAL;
	}

	return AXIS_IGNORE_NONE;
}

gboolean
axis_library_pattern_check(
	const gchar  *pattern,
	const gchar **problem
){
	const gchar *p;
	gsize length;
	gboolean only_stars = TRUE;

	if (pattern == NULL || pattern[0] == '\0') {
		if (problem != NULL)
			*problem = "A pattern cannot be empty.";
		return FALSE;
	}
	length = strlen(pattern);
	if (length > AXIS_LIBRARY_PATTERN_MAX) {
		if (problem != NULL)
			*problem = "A pattern can be at most 255 bytes long.";
		return FALSE;
	}
	if (!axis_library_utf8_valid(pattern, length)) {
		if (problem != NULL)
			*problem = "A pattern must be valid UTF-8 text.";
		return FALSE;
	}
	if (g_ascii_isspace(pattern[0]) || g_ascii_isspace(pattern[length - 1])) {
		if (problem != NULL)
			*problem = "A pattern cannot begin or end with white space.";
		return FALSE;
	}
	for (p = pattern; *p != '\0'; p++) {
		guchar c = (guchar)*p;

		if (c == '/') {
			if (problem != NULL)
				*problem = "A pattern matches one name, so it cannot contain '/'.";
			return FALSE;
		}
		if (c < 0x20 || c == 0x7f || (c == 0xc2 && (guchar)p[1] >= 0x80 && (guchar)p[1] <= 0x9f)) {
			if (problem != NULL)
				*problem = "A pattern cannot contain control characters.";
			return FALSE;
		}
		if (c != '*')
			only_stars = FALSE;
	}
	if (only_stars) {
		if (problem != NULL)
			*problem = "A pattern of only '*' would leave out everything.";
		return FALSE;
	}

	return TRUE;
}

/* One character of @p: a valid UTF-8 sequence, else one byte. */
static gsize
char_length(
	const guchar *p,
	gsize         left
){
	gsize n = axis_library_utf8_sequence(p, left);

	return n > 0 ? n : 1;
}

/*
 * The usual matcher with one backtracking point: on a mismatch after a
 * '*', the star takes one more character of the name and matching resumes
 * after it.  Linear in practice, at worst the product of the lengths.
 */
gboolean
axis_library_glob_match(
	const gchar *pattern,
	const gchar *name,
	gsize        length
){
	const guchar *pat = (const guchar *)pattern;
	const guchar *str = (const guchar *)name;
	gsize plen = strlen(pattern);
	gsize pi = 0;
	gsize ni = 0;
	gsize star_pi = G_MAXSIZE;
	gsize star_ni = 0;

	while (ni < length) {
		if (pi < plen && pat[pi] == '*') {
			while (pi < plen && pat[pi] == '*')
				pi++;
			star_pi = pi;
			star_ni = ni;
			continue;
		}
		if (pi < plen) {
			gsize name_len = char_length(str + ni, length - ni);

			if (pat[pi] == '?') {
				pi++;
				ni += name_len;
				continue;
			} else {
				gsize at = pat[pi] == '\\' && pi + 1 < plen ? pi + 1 : pi;
				gsize pat_len = char_length(pat + at, plen - at);

				if (pat_len == name_len && memcmp(pat + at, str + ni, pat_len) == 0) {
					pi = at + pat_len;
					ni += name_len;
					continue;
				}
			}
		}

		/* A mismatch, or the pattern ran out first. */
		if (star_pi == G_MAXSIZE)
			return FALSE;
		star_ni += char_length(str + star_ni, length - star_ni);
		ni = star_ni;
		pi = star_pi;
	}

	while (pi < plen && pat[pi] == '*')
		pi++;
	return pi == plen;
}
