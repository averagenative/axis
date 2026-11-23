/*
 * axis-release-match.c - matching a release name to a known title (see
 * axis-release.h): title keys, and match, no match or ambiguous
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Plain structures in, an answer out: nothing here touches the database.
 */

#include "release/axis-release.h"

#include <string.h>

/* Longer text is not compared. */
#define KEY_MAX_INPUT 4096

/* At most this many titles are named in an ambiguous answer. */
#define REASON_NAMES 3

/* ── Keys ───────────────────────────────────────────────────────── */

/* A word of i, v and x that is a roman numeral from 1 to 39 in its usual
 * form: its value, else 0. */
static gint
roman_value(
	const gchar *w,
	gsize        length
){
	gsize i = 0;
	gint value = 0;
	guint count = 0;

	while (i < length && w[i] == 'x' && count < 3) {
		value += 10;
		i++;
		count++;
	}
	if (i + 2 == length && w[i] == 'i' && w[i + 1] == 'x')
		return value + 9;
	if (i + 2 == length && w[i] == 'i' && w[i + 1] == 'v')
		return value + 4;
	if (i < length && w[i] == 'v') {
		value += 5;
		i++;
	}
	count = 0;
	while (i < length && w[i] == 'i' && count < 3) {
		value++;
		i++;
		count++;
	}
	if (i != length)
		return 0;
	return value;
}

static void
flush_word(
	GString *out,
	GString *word
){
	gint roman;

	if (word->len == 0)
		return;
	roman = roman_value(word->str, word->len);
	if (roman > 0)
		g_string_append_printf(out, "%d", roman);
	else
		g_string_append_len(out, word->str, (gssize)word->len);
	g_string_truncate(word, 0);
}

static gboolean
apostrophe(gunichar c)
{
	return c == '\'' || c == '`' || c == 0x2018 || c == 0x2019 || c == 0x02bc;
}

/*
 * The plain letters a letter with a stroke, a ligature or a letter of its
 * own stands for, as a name written without it spells them ("Søren" is
 * "Soren", "Æ" is "AE", "Þ" is "TH"): these do not decompose, so the marks
 * removed after NFKD leave them as they are.  Lower case only (the key is
 * folded first); %NULL for any other character.
 */
static const gchar *
plain_letters(gunichar c)
{
	switch (c) {
	case 0x00f8:	/* ø */
		return "o";
	case 0x0111:	/* đ */
	case 0x00f0:	/* ð */
		return "d";
	case 0x0142:	/* ł */
		return "l";
	case 0x00df:	/* ß, should folding leave it */
		return "ss";
	case 0x00e6:	/* æ */
		return "ae";
	case 0x0153:	/* œ */
		return "oe";
	case 0x00fe:	/* þ */
		return "th";
	case 0x0127:	/* ħ */
		return "h";
	case 0x0131:	/* ı, the dotless i */
		return "i";
	case 0x0167:	/* ŧ */
		return "t";
	default:
		return NULL;
	}
}

gchar *
axis_release_title_key(const gchar *text)
{
	gchar *decomposed;
	gchar *folded;
	gchar *again;
	GString *out;
	GString *word;
	const gchar *p;
	gsize length;

	if (text == NULL)
		return NULL;
	length = strlen(text);
	if (length > KEY_MAX_INPUT || !g_utf8_validate(text, (gssize)length, NULL))
		return NULL;
	decomposed = g_utf8_normalize(text, -1, G_NORMALIZE_NFKD);
	if (decomposed == NULL)
		return NULL;
	folded = g_utf8_casefold(decomposed, -1);
	g_free(decomposed);
	/* Folding can make characters that decompose again. */
	again = g_utf8_normalize(folded, -1, G_NORMALIZE_NFKD);
	g_free(folded);
	if (again == NULL)
		return NULL;

	out = g_string_new(NULL);
	word = g_string_new(NULL);
	for (p = again; *p != '\0'; p = g_utf8_next_char(p)) {
		/*
		 * Folding alone is not stable in GLib for every script (a small
		 * Cherokee letter folds to its capital, which GLib's table does not
		 * fold back): lower-casing each folded character settles on one
		 * form, so that the key of a key is the key.
		 */
		gunichar c = g_unichar_tolower(g_utf8_get_char(p));
		const gchar *plain = plain_letters(c);

		if (plain != NULL) {
			g_string_append(word, plain);
			continue;
		}
		switch (g_unichar_type(c)) {
		case G_UNICODE_NON_SPACING_MARK:
		case G_UNICODE_ENCLOSING_MARK:
		case G_UNICODE_SPACING_MARK:
			continue;
		default:
			break;
		}
		if (apostrophe(c))
			continue;
		if (c == '&') {
			/* "&amp;", as a feed that escaped a name twice writes "&". */
			while (strncmp(p + 1, "amp;", 4) == 0)
				p += 4;
			flush_word(out, word);
			g_string_append(out, "and");
		} else if (g_unichar_isalnum(c)) {
			g_string_append_unichar(word, c);
		} else {
			flush_word(out, word);
		}
	}
	flush_word(out, word);
	g_string_free(word, TRUE);
	g_free(again);
	return g_string_free(out, FALSE);
}

/* ── Matching ───────────────────────────────────────────────────── */

typedef enum
{
	FIT_NONE = 0,
	FIT_YEAR,		/* the title, and the same year */
	FIT_NEAR_YEAR,		/* the title, the year one off */
	FIT_NO_YEAR,		/* the title; the name has no year */
	FIT_UNCOMPARABLE	/* the title; the candidate has no year */
} Fit;

static gboolean
key_equal(
	const gchar *key,
	const gchar *name
){
	gchar *other;
	gboolean equal;

	if (name == NULL)
		return FALSE;
	other = axis_release_title_key(name);
	equal = other != NULL && other[0] != '\0' && strcmp(other, key) == 0;
	g_free(other);
	return equal;
}

/* The name of @title that has @key, or %NULL. */
static const gchar *
fitting_name(
	const AxisMetaTitle *title,
	const gchar         *key
){
	guint i;

	if (key_equal(key, title->title))
		return title->title;
	if (key_equal(key, title->original_title))
		return title->original_title;
	if (title->alt_titles != NULL) {
		for (i = 0; i < title->alt_titles->len; i++) {
			const AxisMetaAltTitle *alt = g_ptr_array_index(title->alt_titles, i);

			if (alt != NULL && key_equal(key, alt->title))
				return alt->title;
		}
	}
	return NULL;
}

static const gchar *
shown(const AxisMetaTitle *title)
{
	if (title->title != NULL)
		return title->title;
	if (title->original_title != NULL)
		return title->original_title;
	return "(untitled)";
}

/* A name in a reason: valid UTF-8 with no control or invisible formatting
 * character, whatever the candidate held (the store's titles are clean,
 * but this does not rely on it). */
static void
append_name(
	GString     *out,
	const gchar *name
){
	gchar *valid = g_utf8_make_valid(name, -1);
	const gchar *p;

	g_string_append_c(out, '\'');
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_iscntrl(c) || g_unichar_type(c) == G_UNICODE_FORMAT)
			g_string_append_c(out, ' ');
		else
			g_string_append_unichar(out, c);
	}
	g_string_append_c(out, '\'');
	g_free(valid);
}

/* A title in a reason, with its year, or saying it has none. */
static void
append_title(
	GString             *out,
	const AxisMetaTitle *title
){
	append_name(out, shown(title));
	if (title->year > 0)
		g_string_append_printf(out, " (%d)", title->year);
	else
		g_string_append(out, " (no year known)");
}

/* "N titles fit: 'A' (1999), 'B' (no year known), ...": the @count
 * candidates that fit, in their order. */
static void
append_fits(
	GString                    *out,
	const AxisMetaTitle *const *candidates,
	const Fit                  *fits,
	guint                       n,
	guint                       count
){
	guint named = 0;
	guint i;

	g_string_append_printf(out, "%u titles fit: ", count);
	for (i = 0; i < n && named < REASON_NAMES; i++) {
		if (fits[i] == FIT_NONE)
			continue;
		if (named > 0)
			g_string_append(out, ", ");
		append_title(out, candidates[i]);
		named++;
	}
	if (count > named)
		g_string_append(out, ", ...");
}

AxisReleaseMatch
axis_release_match(
	const AxisRelease          *release,
	const AxisMetaTitle *const *candidates,
	guint                       n,
	gint                       *matched,
	gchar                     **reason
){
	g_autofree gchar *key = NULL;
	g_autofree Fit *fits = NULL;
	g_autofree const gchar **by = NULL;
	gboolean episode;
	guint fitting = 0;
	guint dated = 0;
	guint exact = 0;
	guint unknown = 0;
	guint titled = 0;
	guint films_skipped = 0;
	gint only = -1;
	gint only_dated = -1;
	gint only_exact = -1;
	gint chosen = -1;
	gint year_differs = -1;
	guint i;
	AxisReleaseMatch result;
	GString *why;

	if (matched != NULL)
		*matched = -1;
	if (reason != NULL)
		*reason = NULL;
	if (release != NULL && release->title != NULL)
		key = axis_release_title_key(release->title);
	if (key == NULL || key[0] == '\0') {
		if (reason != NULL)
			*reason = g_strdup("no title was read from the name");
		return AXIS_RELEASE_MATCH_NONE;
	}
	episode = axis_release_is_episode(release);
	fits = g_new0(Fit, MAX(n, 1));
	by = g_new0(const gchar *, MAX(n, 1));

	for (i = 0; i < n; i++) {
		const AxisMetaTitle *title = candidates != NULL ? candidates[i] : NULL;

		if (title == NULL)
			continue;
		if (episode && title->kind != AXIS_META_KIND_SERIES) {
			films_skipped++;
			continue;
		}
		by[i] = fitting_name(title, key);
		if (by[i] == NULL)
			continue;
		titled++;
		if (release->year <= 0)
			fits[i] = FIT_NO_YEAR;
		else if (title->year <= 0)
			fits[i] = FIT_UNCOMPARABLE;
		else if (title->year == release->year)
			fits[i] = FIT_YEAR;
		else if (title->year == release->year + 1 || title->year == release->year - 1)
			fits[i] = FIT_NEAR_YEAR;
		else if (year_differs < 0)
			year_differs = (gint)i;
		if (fits[i] == FIT_YEAR || fits[i] == FIT_NEAR_YEAR) {
			dated++;
			only_dated = (gint)i;
			if (fits[i] == FIT_YEAR) {
				exact++;
				only_exact = (gint)i;
			}
		} else if (fits[i] == FIT_UNCOMPARABLE) {
			unknown++;
		}
		if (fits[i] != FIT_NONE) {
			fitting++;
			only = (gint)i;
		}
	}

	/*
	 * The one candidate a name means.  A name without a year means the one
	 * title of that name, when there is exactly one.  A name with a year
	 * means the one title of that name whose known year is its year or one
	 * off.  A title of that name with no year known stands in the way only
	 * when that year is one off (it could be the name's year as well); when
	 * it is the same year, the title known to have that year is taken, and
	 * the one with no year known cannot be shown to be it.  The one title
	 * whose year is the name's year is taken whatever others are a year off
	 * (a popular name has a film most years, and none could be grabbed);
	 * two of the name's own year, or two a year off with none of the year,
	 * are ambiguous.
	 */
	if (release->year <= 0) {
		if (fitting == 1)
			chosen = only;
	} else if (exact == 1) {
		chosen = only_exact;
	} else if (exact == 0 && dated == 1 && unknown == 0) {
		chosen = only_dated;
	}

	why = g_string_new(NULL);
	if (fitting == 0) {
		result = AXIS_RELEASE_MATCH_NONE;
		if (year_differs >= 0) {
			append_title(why, candidates[year_differs]);
			g_string_append_printf(why, " has that title, but the name says %d", release->year);
		} else if (episode && films_skipped > 0 && titled == 0) {
			g_string_append(why, "the name is of episodes, and no series has that title");
		} else {
			g_string_append(why, "no title of that name");
		}
	} else if (chosen >= 0) {
		result = AXIS_RELEASE_MATCH_YES;
		if (matched != NULL)
			*matched = chosen;
		g_string_append(why, "matches ");
		append_title(why, candidates[chosen]);
		if (by[chosen] != candidates[chosen]->title) {
			g_string_append(why, " by its other name ");
			append_name(why, by[chosen]);
		}
		switch (fits[chosen]) {
		case FIT_YEAR:
			g_string_append(why, ", the same year");
			break;
		case FIT_NEAR_YEAR:
			g_string_append_printf(why, ", the year one off (the name says %d)", release->year);
			break;
		default:
			g_string_append(why, "; the name has no year, and no other title has that name");
			break;
		}
		if (fits[chosen] == FIT_YEAR && dated > 1)
			g_string_append_printf(why, "; %u other title%s of that name %s a year off", dated - 1,
				dated == 2 ? "" : "s", dated == 2 ? "is" : "are");
		if (unknown > 0)
			g_string_append_printf(why, "; %u other title%s of that name ha%s no year known", unknown,
				unknown == 1 ? "" : "s", unknown == 1 ? "s" : "ve");
	} else if (fitting == 1) {
		result = AXIS_RELEASE_MATCH_AMBIGUOUS;
		append_title(why, candidates[only]);
		g_string_append_printf(why, " has that title, but no year to compare with %d", release->year);
	} else {
		result = AXIS_RELEASE_MATCH_AMBIGUOUS;
		append_fits(why, candidates, fits, n, fitting);
		if (release->year <= 0)
			g_string_append(why, "; the name has no year");
		else if (dated == 1)
			g_string_append_printf(why, "; the name says %d, a year off the one known year, and a title with no "
				"year known could be the one", release->year);
	}
	if (reason != NULL)
		*reason = g_string_free(why, FALSE);
	else
		g_string_free(why, TRUE);
	return result;
}
