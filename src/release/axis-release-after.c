/*
 * axis-release-after.c - reading a name after a known title (see the
 * header): the one number, or the one range, that follows a series' name
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * add-anime-support, slice A2.  Reading a name by itself (axis-release-parse.c)
 * knows no series, so a number with nothing before it that marks it as an
 * episode ("Tenkuu Rangers 137 [1080p]") stays in the title.  Here the title
 * is given, so what follows it can be read.  Everything doubtful is "not
 * read", with a code: nothing here is a guess, and nothing here changes what
 * reading a name by itself reports.
 *
 * GLib only, no I/O.  Bounded: a name is at most AXIS_RELEASE_NAME_MAX bytes,
 * its words are found in one pass, and the title is looked for at each word's
 * end (at most one key per word, each linear in the name).
 */

#include "release/axis-release-private.h"

#include <string.h>

/* More names than this are not compared. */
#define MAX_TITLES 128

/* A name has at most this many words before its first bracket (half its
 * bytes: a word is at least one byte and one separator). */
#define MAX_WORDS (AXIS_RELEASE_NAME_MAX / 2 + 1)

/* Leading "[Group]" and "[site]" tags taken off, at most. */
#define MAX_LEADING_TAGS 4

typedef struct
{
	gsize start;
	gsize end;
} Word;

static gboolean
separator(gchar c)
{
	return c == ' ' || c == '.' || c == '_' || c == '\t' || (guchar)c < 0x20 || c == 0x7f;
}

static gboolean
bracket(gchar c)
{
	return c == '[' || c == ']' || c == '(' || c == ')' || c == '{' || c == '}';
}

static gboolean
all_digits(
	const gchar *text,
	gsize        length
){
	gsize i;

	if (length == 0)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

/* Whether valid UTF-8 @text (@length bytes) holds a letter or a digit. */
static gboolean
has_alnum(
	const gchar *text,
	gsize        length
){
	const gchar *p;

	for (p = text; p < text + length; p = g_utf8_next_char(p)) {
		if (g_unichar_isalnum(g_utf8_get_char(p)))
			return TRUE;
	}
	return FALSE;
}

/* One to four digits at @text: their value and how many, else 0 digits. */
static gsize
number_at(
	const gchar *text,
	gsize        length,
	gint        *value
){
	gsize n = 0;
	gint v = 0;

	while (n < length && g_ascii_isdigit(text[n])) {
		if (n == 4)
			return 0;
		v = v * 10 + (text[n] - '0');
		n++;
	}
	*value = v;
	return n;
}

static gboolean
word_is(
	const gchar *text,
	gsize        length,
	const gchar *word
){
	return strlen(word) == length && g_ascii_strncasecmp(text, word, length) == 0;
}

/* A word by which a release names a special, an extra or a film of a series:
 * never an episode's number. */
gboolean
axis_release_special_word(
	const gchar *text,
	gsize        length
){
	static const gchar *const words[] = {
		"ova", "oad", "ona", "ncop", "nced", "pv", "sp", "special", "specials", "movie", "gekijouban", "op", "ed",
		NULL
	};
	static const gchar *const numbered[] = { "ova", "oad", "ona", "ncop", "nced", "pv", "sp", "op", "ed", NULL };
	guint i;

	if (text == NULL || length == 0 || length > 16)
		return FALSE;
	for (i = 0; words[i] != NULL; i++) {
		if (word_is(text, length, words[i]))
			return TRUE;
	}
	/* "SP01", "OVA2", "NCOP1", "NCED2a". */
	for (i = 0; numbered[i] != NULL; i++) {
		gsize stem = strlen(numbered[i]);
		gsize k;

		if (length <= stem || g_ascii_strncasecmp(text, numbered[i], stem) != 0 || !g_ascii_isdigit(text[stem]))
			continue;
		for (k = stem; k < length && g_ascii_isdigit(text[k]); k++)
			;
		if (k == length || (k + 1 == length && g_ascii_isalpha(text[k])))
			return TRUE;
	}
	return FALSE;
}

static void
not_read(
	AxisReleaseAfter *after,
	const gchar      *code
){
	after->read = FALSE;
	after->code = code;
	after->first = 0;
	after->last = 0;
	after->range = FALSE;
	after->version = 0;
}

static gboolean
could_be_year(
	gint  value,
	gsize digits,
	gint  latest_year
){
	return digits == 4 && value >= AXIS_RELEASE_YEAR_MIN && value <= latest_year;
}

/* A number a name writes for the height of its picture. */
static gboolean
is_resolution(gint value)
{
	return value == 480 || value == 540 || value == 576 || value == 720 || value == 1080 || value == 2160;
}

/* A word after a number that says the number counts something larger than an
 * episode ("02 Complete": the second season, whole; "03 BD": the third disc). */
static gboolean
collection_word(
	const gchar *text,
	gsize        length
){
	static const gchar *const words[] = { "complete", "batch", "bd", "vol", "volume", NULL };
	guint i;

	for (i = 0; words[i] != NULL; i++) {
		if (word_is(text, length, words[i]))
			return TRUE;
	}
	return FALSE;
}

/* Whether the bracket that opens at @open holds a number and nothing else
 * ("[05]", "(05)"): a second number, as one outside a bracket is.  A year and
 * a picture's height are not (the name's own, read as such elsewhere). */
static gboolean
bracket_number(
	const gchar *text,
	gsize        open,
	gsize        end,
	gint         latest_year
){
	gsize at = open + 1;
	gsize digits;
	gint value = 0;

	while (at < end && separator(text[at]))
		at++;
	digits = number_at(text + at, end - at, &value);
	if (digits == 0)
		return FALSE;
	at += digits;
	while (at < end && separator(text[at]))
		at++;
	if (at >= end || !bracket(text[at]))
		return FALSE;
	return !could_be_year(value, digits, latest_year) && !is_resolution(value);
}

void
axis_release_read_after_full(
	const gchar        *name,
	gssize              length,
	const gchar *const *titles,
	guint               n_titles,
	gint                latest_year,
	AxisReleaseAfter   *after
){
	g_autofree gchar *text = NULL;
	g_autofree Word *words = NULL;
	gchar *keys[MAX_TITLES];
	guint n_keys = 0;
	guint n_words = 0;
	guint best = 0;
	gsize size;
	gsize end;
	gsize pos = 0;
	guint tags = 0;
	guint i;
	guint k;
	const Word *w;
	const Word *next;
	gsize digits;
	gsize used;
	gint a = 0;
	gint b = 0;
	gsize b_digits = 0;
	gboolean range = FALSE;
	gint version = 0;
	gsize words_end;

	memset(after, 0, sizeof(*after));
	after->code = "unreadable";
	if (name == NULL)
		return;
	size = length < 0 ? strlen(name) : (gsize)length;
	if (size == 0 || size > AXIS_RELEASE_NAME_MAX || memchr(name, '\0', size) != NULL ||
	    !g_utf8_validate(name, (gssize)size, NULL))
		return;
	text = g_strndup(name, size);
	end = size;

	/* A known file extension is taken off the end. */
	for (i = 1; i <= 8 && i < end; i++) {
		guint stop = 0;

		if (text[end - i] != '.')
			continue;
		if (i > 1 && axis_release_extension(text + end - i + 1, i - 1, &stop))
			end -= i;
		break;
	}

	/* A leading "[Group]", "[site]", or "www.site.tld - ". */
	for (;;) {
		gsize close;

		while (pos < end && separator(text[pos]))
			pos++;
		if (pos >= end || text[pos] != '[' || tags >= MAX_LEADING_TAGS)
			break;
		for (close = pos + 1; close < end && text[close] != ']' && text[close] != '['; close++)
			;
		if (close >= end || text[close] != ']')
			break;
		pos = close + 1;
		tags++;
	}
	if (end - pos > 4 && g_ascii_strncasecmp(text + pos, "www.", 4) == 0) {
		gsize at;

		for (at = pos + 4; at + 2 < end && at < pos + 80 && !bracket(text[at]); at++) {
			if (text[at] == ' ' && text[at + 1] == '-' && text[at + 2] == ' ') {
				pos = at + 3;
				break;
			}
		}
		while (pos < end && separator(text[pos]))
			pos++;
	}

	/* The words up to the first bracket: a title, and the number after it,
	 * stand before any bracket. */
	words = g_new0(Word, MAX_WORDS + 1);
	while (pos < end && !bracket(text[pos]) && n_words < MAX_WORDS) {
		gsize start = pos;

		while (pos < end && !separator(text[pos]) && !bracket(text[pos]))
			pos++;
		words[n_words].start = start;
		words[n_words].end = pos;
		n_words++;
		while (pos < end && separator(text[pos]))
			pos++;
	}
	words_end = pos;

	/* The longest of the names that the name begins with, at a word's end:
	 * compared by title key, as the matcher compares a title. */
	after->code = "other_title";
	for (i = 0; i < n_titles && titles != NULL && n_keys < MAX_TITLES; i++) {
		gchar *key = titles[i] != NULL ? axis_release_title_key(titles[i]) : NULL;

		if (key == NULL || key[0] == '\0') {
			g_free(key);
			continue;
		}
		keys[n_keys++] = key;
	}
	for (i = 0; i < n_words && n_keys > 0; i++) {
		gchar saved = text[words[i].end];
		gchar *key;

		/* A name ends at a word that has a letter or a digit: a lone hyphen
		 * after it is not part of it (its key would be the same). */
		if (!has_alnum(text + words[i].start, words[i].end - words[i].start))
			continue;
		text[words[i].end] = '\0';
		key = axis_release_title_key(text + words[0].start);
		text[words[i].end] = saved;
		for (k = 0; key != NULL && k < n_keys; k++) {
			if (strcmp(key, keys[k]) == 0) {
				best = i + 1;
				break;
			}
		}
		g_free(key);
	}
	for (k = 0; k < n_keys; k++)
		g_free(keys[k]);
	if (best == 0)
		return;
	after->titled = TRUE;
	after->title_at = words[0].start;
	after->title_end = words[best - 1].end;

	/* What follows: an optional lone hyphen, then one number or one range. */
	i = best;
	if (i < n_words && words[i].end - words[i].start == 1 && text[words[i].start] == '-') {
		after->hyphen = TRUE;
		i++;
	}
	if (i >= n_words) {
		not_read(after, "no_number");
		return;
	}
	w = &words[i];
	next = i + 1 < n_words ? &words[i + 1] : NULL;
	if (axis_release_special_word(text + w->start, w->end - w->start)) {
		not_read(after, "special");
		return;
	}
	digits = number_at(text + w->start, w->end - w->start, &a);
	if (digits == 0) {
		not_read(after, "no_number");
		return;
	}
	used = digits;
	if (w->start + used < w->end && (text[w->start + used] == '-' || text[w->start + used] == '~')) {
		/* "01-12", "01~12": a range in one word. */
		b_digits = number_at(text + w->start + used + 1, w->end - w->start - used - 1, &b);
		if (b_digits == 0 || w->start + used + 1 + b_digits != w->end) {
			not_read(after, "no_number");
			return;
		}
		range = TRUE;
	} else if (w->start + used < w->end && (text[w->start + used] == 'v' || text[w->start + used] == 'V')) {
		/* "12v2": a version, one digit from 1 to 9. */
		if (w->start + used + 2 != w->end || text[w->start + used + 1] < '1' || text[w->start + used + 1] > '9') {
			not_read(after, "no_number");
			return;
		}
		version = text[w->start + used + 1] - '0';
	} else if (w->start + used != w->end) {
		not_read(after, "no_number");
		return;
	} else if (next != NULL && next->end - next->start == 1 && text[next->start] == '~' && i + 2 < n_words) {
		/* "01 ~ 12": a range in three words. */
		const Word *last = &words[i + 2];

		b_digits = number_at(text + last->start, last->end - last->start, &b);
		if (b_digits == 0 || last->start + b_digits != last->end) {
			not_read(after, "no_number");
			return;
		}
		range = TRUE;
		i += 2;
		w = last;
		next = i + 1 < n_words ? &words[i + 1] : NULL;
	}
	after->number_end = w->end;

	/* What follows the number: the end of the name, a bracket, or a word the
	 * tables know.  A second number is never read past. */
	if (next != NULL) {
		g_autofree gchar *word = g_strndup(text + next->start, next->end - next->start);
		const Word *third = i + 2 < n_words ? &words[i + 2] : NULL;

		if (all_digits(word, strlen(word))) {
			/* "12.5": half an episode, written with a dot. */
			not_read(after, !range && version == 0 && next->start == w->end + 1 && text[w->end] == '.' &&
				strlen(word) <= 2 ? "decimal" : "two_numbers");
			return;
		}
		if (strcmp(word, "-") == 0 && third != NULL) {
			/* "2 - 05", "2 - 05v2", "2 - 05-06": the number may be the title's,
			 * or a season.  ("2 - 1080p" is no second number: it is words not
			 * understood, below.) */
			gint second = 0;
			gsize wide = third->end - third->start;
			gsize n = number_at(text + third->start, wide, &second);
			gchar then = n > 0 && n < wide ? text[third->start + n] : '\0';

			if (n > 0 && (n == wide || then == 'v' || then == 'V' || then == '-' || then == '~')) {
				not_read(after, "two_numbers");
				return;
			}
		}
		/* "05 & 06", "05 + 06", "05 and 06": two episodes, or two things. */
		if ((strcmp(word, "&") == 0 || strcmp(word, "+") == 0 || g_ascii_strcasecmp(word, "and") == 0) &&
		    third != NULL && g_ascii_isdigit(text[third->start])) {
			not_read(after, "two_numbers");
			return;
		}
		/* "02 Complete", "03 BD", "02 Vol", "02 Movie", "05 PV", "05 Preview",
		 * "02 Part 1", "05 of 12": the word says the number is not one
		 * episode's, though the tables know some of these as words of a
		 * release. */
		if (collection_word(word, strlen(word)) || axis_release_special_word(word, strlen(word)) ||
		    g_ascii_strcasecmp(word, "preview") == 0 ||
		    ((g_ascii_strcasecmp(word, "part") == 0 || g_ascii_strcasecmp(word, "of") == 0) && third != NULL &&
		     g_ascii_isdigit(text[third->start]))) {
			not_read(after, "not_episode");
			return;
		}
		if (axis_release_rule_word(word) == NULL && !axis_release_site_tag(word, strlen(word))) {
			not_read(after, "trailing");
			return;
		}
	} else {
		/* "10 [05]", "10 (05)", "10 [1080p][05]": a number alone in a bracket
		 * after the number may be the episode, and the first the title's. */
		gsize open;

		for (open = words_end; open < end; open++) {
			if ((text[open] == '[' || text[open] == '(' || text[open] == '{') &&
			    bracket_number(text, open, end, latest_year)) {
				not_read(after, "two_numbers");
				return;
			}
		}
	}

	/* The number itself. */
	if (could_be_year(a, digits, latest_year) || (range && could_be_year(b, b_digits, latest_year))) {
		not_read(after, "year");
		return;
	}
	if (a == 0) {
		not_read(after, "zero");
		return;
	}
	if (range) {
		if (b <= a) {
			not_read(after, "backwards");
			return;
		}
		if (b - a + 1 > AXIS_RELEASE_MAX_NUMBERS) {
			not_read(after, "too_wide");
			return;
		}
	} else if (!after->hyphen && digits == 1) {
		/* "Title 2": a sequel or a season far more often than an episode,
		 * which is written with two digits or after a hyphen. */
		not_read(after, "single_digit");
		return;
	}
	after->read = TRUE;
	after->code = NULL;
	after->first = a;
	after->last = range ? b : a;
	after->range = range;
	after->digits = (guint)digits;
	after->version = version;
}

void
axis_release_read_after(
	const gchar        *name,
	const gchar *const *titles,
	guint               n_titles,
	AxisReleaseAfter   *after
){
	GDateTime *now = g_date_time_new_now_local();
	gint year = now != NULL ? g_date_time_get_year(now) + 1 : 2100;

	if (now != NULL)
		g_date_time_unref(now);
	axis_release_read_after_full(name, -1, titles, n_titles, year, after);
}

const gchar *
axis_release_after_words(const gchar *code)
{
	if (code == NULL)
		return NULL;
	if (strcmp(code, "no_number") == 0)
		return "what follows the series' name is not an episode number";
	if (strcmp(code, "special") == 0)
		return "it names a special, an extra or a film by a word (such as OVA), not an episode by its number: assign it "
			"by hand";
	if (strcmp(code, "two_numbers") == 0)
		return "two numbers follow the series' name, so the first may belong to another title or be a season: it is not "
			"read as an episode";
	if (strcmp(code, "decimal") == 0)
		return "the number after the series' name has a decimal part, which is not an episode number";
	if (strcmp(code, "year") == 0)
		return "the number after the series' name can be a year, and a year is never read as an episode number";
	if (strcmp(code, "zero") == 0)
		return "0 is not an episode number";
	if (strcmp(code, "single_digit") == 0)
		return "a single digit follows the series' name with no hyphen before it, which may be a sequel or a season: it "
			"is not read as an episode";
	if (strcmp(code, "backwards") == 0)
		return "the range after the series' name runs backwards";
	if (strcmp(code, "too_wide") == 0)
		return "the range after the series' name is wider than a name is believed to state";
	if (strcmp(code, "not_episode") == 0)
		return "a word after the number says that it is not one episode (a film, a disc, a volume, a part, a preview "
			"or a count of episodes): it is not read as an episode";
	if (strcmp(code, "trailing") == 0)
		return "words that are not understood follow the number after the series' name, so the number may be part of "
			"something else: it is not read as an episode";
	return NULL;
}
