/*
 * axis-analysis-name.c - the name normaliser (see the header for the rules)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-analysis-name.h"

#include <string.h>

#define MAX_YEAR_TOKENS 16
#define MAX_TITLE_BYTES 400

/* ── Text ───────────────────────────────────────────────────────── */

gchar *
axis_analysis_normalise(const gchar *text)
{
	gchar *decomposed;
	gchar *folded;
	GString *out;
	const gchar *p;
	gboolean pending = FALSE;

	if (text == NULL || strlen(text) > AXIS_ANALYSIS_NAME_MAX_INPUT || !g_utf8_validate(text, -1, NULL))
		return NULL;

	decomposed = g_utf8_normalize(text, -1, G_NORMALIZE_NFKD);
	if (decomposed == NULL)
		return NULL;
	folded = g_utf8_casefold(decomposed, -1);
	g_free(decomposed);

	out = g_string_new(NULL);
	for (p = folded; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		/*
		 * Case folding alone is not stable in GLib for every script: a
		 * small Cherokee letter folds to its capital (as Unicode says),
		 * and the capital, which has no folding of its own in GLib's
		 * table, is then lower-cased back.  Lower-casing each folded
		 * character settles on one form, so that normalising a
		 * normalised name changes nothing.
		 */
		c = g_unichar_tolower(c);

		switch (g_unichar_type(c)) {
		case G_UNICODE_NON_SPACING_MARK:
		case G_UNICODE_ENCLOSING_MARK:
		case G_UNICODE_SPACING_MARK:
			continue;
		default:
			break;
		}
		if (c == '\'' || c == '`' || c == 0x2018 || c == 0x2019)
			continue;
		if (c == '&') {
			if (out->len > 0)
				g_string_append_c(out, ' ');
			g_string_append(out, "and");
			pending = TRUE;
		} else if (g_unichar_isalnum(c)) {
			if (pending && out->len > 0)
				g_string_append_c(out, ' ');
			pending = FALSE;
			g_string_append_unichar(out, c);
		} else {
			pending = TRUE;
		}
	}
	g_free(folded);
	/* A result that is itself too long to be normalised again (every '&'
	 * becomes three letters) is not comparable either. */
	if (out->len > AXIS_ANALYSIS_NAME_MAX_INPUT) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

/* ── Years ──────────────────────────────────────────────────────── */

/* Part of a word: a letter or digit (any byte of a multi-byte character
 * counts as a letter). */
static gboolean
word_byte(gchar c)
{
	guchar u = (guchar)c;

	return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u >= 0x80;
}

static gboolean
digit_at(
	const gchar *s,
	gsize        i
){
	return s[i] >= '0' && s[i] <= '9';
}

/*
 * The year tokens of @s: four digits from 1900 to 2099 with no letter or
 * digit touching them.  Fills @at and @value (up to MAX_YEAR_TOKENS) in
 * order; returns how many.
 */
static guint
year_tokens(
	const gchar *s,
	gsize        at[MAX_YEAR_TOKENS],
	gint         value[MAX_YEAR_TOKENS]
){
	gsize length = strlen(s);
	guint n = 0;
	gsize i;

	for (i = 0; i + 4 <= length && n < MAX_YEAR_TOKENS; i++) {
		gint year;

		if (!digit_at(s, i) || !digit_at(s, i + 1) || !digit_at(s, i + 2) || !digit_at(s, i + 3))
			continue;
		if (i > 0 && word_byte(s[i - 1]))
			continue;
		if (i + 4 < length && word_byte(s[i + 4]))
			continue;
		year = (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
		if (year < 1900 || year > 2099)
			continue;
		at[n] = i;
		value[n] = year;
		n++;
		i += 3;
	}
	return n;
}

/*
 * Title and year of @s: the last year token that has a title before it.
 */
static gboolean
title_and_year(
	const gchar  *s,
	gchar       **title,
	gint         *year
){
	gsize at[MAX_YEAR_TOKENS];
	gint value[MAX_YEAR_TOKENS];
	guint n = year_tokens(s, at, value);

	while (n > 0) {
		g_autofree gchar *before = g_strndup(s, at[n - 1]);
		gchar *normal = axis_analysis_normalise(before);

		if (normal != NULL && normal[0] != '\0' && strlen(normal) <= MAX_TITLE_BYTES) {
			*title = normal;
			*year = value[n - 1];
			return TRUE;
		}
		g_free(normal);
		n--;
	}
	return FALSE;
}

/* ── Paths ──────────────────────────────────────────────────────── */

static const gchar *
base_of(const gchar *path)
{
	const gchar *slash = strrchr(path, '/');

	return slash != NULL ? slash + 1 : path;
}

/* A file name without its extension (up to five letters or digits). */
static gchar *
stem_of(const gchar *base)
{
	const gchar *dot = strrchr(base, '.');

	if (dot != NULL && dot != base) {
		gsize length = strlen(dot + 1);
		gsize i;
		gboolean plain = length >= 1 && length <= 5;

		for (i = 0; plain && i < length; i++)
			plain = g_ascii_isalnum(dot[1 + i]);
		if (plain)
			return g_strndup(base, (gsize)(dot - base));
	}
	return g_strdup(base);
}

/* The name of the folder a file is in, or %NULL when it is at the top. */
static gchar *
parent_of(const gchar *path)
{
	const gchar *last = strrchr(path, '/');
	const gchar *start;

	if (last == NULL)
		return NULL;
	start = last;
	while (start > path && start[-1] != '/')
		start--;
	return g_strndup(start, (gsize)(last - start));
}

/* The first segment of a path with more than one, else %NULL. */
static gchar *
first_of(const gchar *path)
{
	const gchar *slash = strchr(path, '/');

	return slash != NULL ? g_strndup(path, (gsize)(slash - path)) : NULL;
}

/* ── Films ──────────────────────────────────────────────────────── */

gboolean
axis_analysis_parse_film(
	const gchar  *path,
	gchar       **title,
	gint         *year
){
	g_autofree gchar *stem = NULL;
	g_autofree gchar *parent = NULL;
	gsize at[MAX_YEAR_TOKENS];
	gint value[MAX_YEAR_TOKENS];

	g_return_val_if_fail(title != NULL && year != NULL, FALSE);
	*title = NULL;
	*year = 0;
	if (path == NULL || path[0] == '\0' || strlen(path) > 4 * AXIS_ANALYSIS_NAME_MAX_INPUT)
		return FALSE;

	stem = stem_of(base_of(path));
	if (title_and_year(stem, title, year))
		return TRUE;

	/* Only a name with no year in it at all falls back to its folder. */
	if (year_tokens(stem, at, value) > 0)
		return FALSE;
	parent = parent_of(path);
	if (parent != NULL && title_and_year(parent, title, year))
		return TRUE;
	return FALSE;
}

gchar *
axis_analysis_film_key(const gchar *path)
{
	g_autofree gchar *title = NULL;
	gint year;

	if (!axis_analysis_parse_film(path, &title, &year))
		return NULL;
	return g_strdup_printf("film|%s|%d", title, year);
}

/* ── Episodes ───────────────────────────────────────────────────── */

/* Digits from @i: how many (at most @max + 1 are counted). */
static gsize
count_digits(
	const gchar *s,
	gsize        i,
	gsize        max
){
	gsize n = 0;

	while (n <= max && digit_at(s, i + n))
		n++;
	return n;
}

static gint
number_of(
	const gchar *s,
	gsize        i,
	gsize        n
){
	gint value = 0;
	gsize k;

	for (k = 0; k < n; k++)
		value = value * 10 + (s[i + k] - '0');
	return value;
}

/* After a marker: a second episode ("E02", "-02", "-E02", "&02"). */
static gboolean
more_episodes(
	const gchar *s,
	gsize        end
){
	gsize k = end;
	gsize n;

	while (s[k] == ' ')
		k++;
	if ((s[k] == 'e' || s[k] == 'E') && digit_at(s, k + 1))
		return TRUE;
	if (s[k] == '-' || s[k] == '&' || s[k] == '+') {
		k++;
		while (s[k] == ' ')
			k++;
		if (s[k] == 'e' || s[k] == 'E')
			k++;
		n = count_digits(s, k, 4);
		if (n >= 1 && n <= 4 && !word_byte(s[k + n]))
			return TRUE;
	}
	return FALSE;
}

/*
 * The first episode marker of @s: "S01E02" or "1x02".  *@multi says it is
 * followed by another episode.
 */
static gboolean
find_marker(
	const gchar *s,
	gsize       *start,
	gint        *season,
	gint        *episode,
	gboolean    *multi
){
	gsize length = strlen(s);
	gsize i;

	*multi = FALSE;
	for (i = 0; i < length; i++) {
		gsize a;
		gsize b;
		gsize j;

		if (i > 0 && word_byte(s[i - 1]))
			continue;
		if (s[i] == 'S' || s[i] == 's') {
			a = count_digits(s, i + 1, 3);
			j = i + 1 + a;
			if (a < 1 || a > 3 || (s[j] != 'E' && s[j] != 'e'))
				continue;
			b = count_digits(s, j + 1, 4);
			if (b < 1 || b > 4)
				continue;
			j = j + 1 + b;
			if (word_byte(s[j]) && !((s[j] == 'E' || s[j] == 'e') && digit_at(s, j + 1)))
				continue;
			*start = i;
			*season = number_of(s, i + 1, a);
			*episode = number_of(s, i + 2 + a, b);
			*multi = more_episodes(s, j);
			return TRUE;
		}
		if (digit_at(s, i)) {
			a = count_digits(s, i, 2);
			j = i + a;
			if (a < 1 || a > 2 || (s[j] != 'x' && s[j] != 'X'))
				continue;
			b = count_digits(s, j + 1, 3);
			if (b < 2 || b > 3)
				continue;
			if (word_byte(s[j + 1 + b]))
				continue;
			*start = i;
			*season = number_of(s, i, a);
			*episode = number_of(s, j + 1, b);
			*multi = more_episodes(s, j + 1 + b);
			return TRUE;
		}
	}
	return FALSE;
}

gboolean
axis_analysis_parse_episode(
	const gchar  *path,
	gchar       **show,
	gint         *show_year,
	gint         *season,
	gint         *episode
){
	g_autofree gchar *stem = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *normal = NULL;
	g_autofree gchar *folder = NULL;
	gsize start = 0;
	gboolean multi = FALSE;
	gint year = 0;
	gint found_season = 0;
	gint found_episode = 0;
	gchar *title = NULL;

	g_return_val_if_fail(show != NULL && show_year != NULL && season != NULL && episode != NULL, FALSE);
	*show = NULL;
	*show_year = 0;
	*season = 0;
	*episode = 0;
	if (path == NULL || path[0] == '\0' || strlen(path) > 4 * AXIS_ANALYSIS_NAME_MAX_INPUT)
		return FALSE;

	stem = stem_of(base_of(path));
	if (!find_marker(stem, &start, &found_season, &found_episode, &multi) || multi)
		return FALSE;
	if (found_season > 999 || found_episode > 9999)
		return FALSE;

	folder = first_of(path);
	before = g_strndup(stem, start);
	normal = axis_analysis_normalise(before);
	if (normal == NULL)
		return FALSE;

	/* The outputs are only set when the name is read: from here on every
	 * way out that succeeds sets the season and episode first. */
	if (normal[0] != '\0') {
		if (title_and_year(before, &title, &year)) {
			*show = title;
			*show_year = year;
			*season = found_season;
			*episode = found_episode;
			return TRUE;
		}
		*season = found_season;
		*episode = found_episode;
		*show = g_steal_pointer(&normal);
		/* The year of the show's folder, when it is the same show. */
		if (folder != NULL) {
			gchar *folder_title = NULL;
			gint folder_year = 0;

			if (title_and_year(folder, &folder_title, &folder_year)) {
				if (strcmp(folder_title, *show) == 0)
					*show_year = folder_year;
				g_free(folder_title);
			}
		}
		return TRUE;
	}

	/* No show in the name: the show's folder. */
	if (folder == NULL)
		return FALSE;
	if (title_and_year(folder, &title, &year)) {
		*show = title;
		*show_year = year;
		*season = found_season;
		*episode = found_episode;
		return TRUE;
	}
	title = axis_analysis_normalise(folder);
	if (title == NULL || title[0] == '\0') {
		g_free(title);
		return FALSE;
	}
	*show = title;
	*season = found_season;
	*episode = found_episode;
	return TRUE;
}

gchar *
axis_analysis_episode_key(const gchar *path)
{
	g_autofree gchar *show = NULL;
	gint year;
	gint season;
	gint episode;

	if (!axis_analysis_parse_episode(path, &show, &year, &season, &episode))
		return NULL;
	return g_strdup_printf("episode|%s|%d|%d|%d", show, year, season, episode);
}

/* ── Tracks, series and albums ──────────────────────────────────── */

gchar *
axis_analysis_track_key(
	const gchar *artist,
	const gchar *album,
	const gchar *title
){
	g_autofree gchar *a = axis_analysis_normalise(artist);
	g_autofree gchar *b = axis_analysis_normalise(album);
	g_autofree gchar *t = axis_analysis_normalise(title);

	if (a == NULL || b == NULL || t == NULL || a[0] == '\0' || b[0] == '\0' || t[0] == '\0')
		return NULL;
	return g_strdup_printf("track|%s|%s|%s", a, b, t);
}

gchar *
axis_analysis_show_key(const gchar *folder)
{
	g_autofree gchar *title = NULL;
	gchar *normal;
	gint year = 0;

	if (folder == NULL)
		return NULL;
	if (title_and_year(folder, &title, &year))
		return g_strdup_printf("%s|%d", title, year);
	normal = axis_analysis_normalise(folder);
	if (normal == NULL || normal[0] == '\0') {
		g_free(normal);
		return NULL;
	}
	title = normal;
	return g_strdup_printf("%s|0", title);
}

gchar *
axis_analysis_album_key(
	const gchar *artist_folder,
	const gchar *album_folder
){
	g_autofree gchar *a = axis_analysis_normalise(artist_folder);
	g_autofree gchar *b = axis_analysis_normalise(album_folder);

	if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0')
		return NULL;
	return g_strdup_printf("%s|%s", a, b);
}
