/*
 * fuzz-release-match.c - libFuzzer target for matching a release name to
 * titles, and for title keys
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is split at newlines: the first line is a release name, each
 * further line (up to 16) a candidate title, its fields split at '|':
 * kind ("s" for a series, anything else a film), year, title, original
 * title, then other names (up to 8).  Whatever they are, the matcher must
 * not crash or take long, and:
 *
 *  - only a match names a candidate, and ambiguous is never a match;
 *  - the answer agrees with a plain count done here, the other way round.
 *    A candidate has the name when one of its names has the title's key
 *    and its kind can be the name's.  For a name without a year: a match
 *    is exactly one candidate with the name, ambiguous two or more.  For a
 *    name with a year: a match is the one candidate with the name whose
 *    year is the very year, whatever others are a year off or have no
 *    year; else the one candidate a year off, when no candidate with the
 *    name has no year; ambiguous is two of the very year, two a year off
 *    with none of the year, one a year off beside one with no year, or one
 *    with no year alone (or several).  No match is none of these;
 *  - a title key is the key of itself, holds no separator, punctuation or
 *    capital, and is the same twice;
 *  - the reading after a known title, against the first candidate's names,
 *    takes for the series' name only what has the key of one of them, and
 *    reads names made of the title itself as their rule says (check_after()).
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "release/axis-release.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define LATEST 2027
#define MAX_CANDIDATES 16
#define MAX_OTHER 8
#define BOUND_US (500 * 1000)

#define FAIL(why) do { fprintf(stderr, "fuzz-release-match: %s\n", why); abort(); } while (0)

static void
check_key(const gchar *text)
{
	gchar *key = axis_release_title_key(text);
	gchar *again;
	gchar *twice;
	const gchar *p;

	if (key == NULL)
		return;
	if (!g_utf8_validate(key, -1, NULL))
		FAIL("a key that is not UTF-8");
	for (p = key; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (!g_unichar_isalnum(c) || g_unichar_isupper(c))
			FAIL("a key with a separator, punctuation or a capital");
	}
	again = axis_release_title_key(key);
	twice = axis_release_title_key(text);
	if (g_strcmp0(again, key) != 0) {
		fprintf(stderr, "'%s' became '%s'\n", key, again != NULL ? again : "(null)");
		FAIL("the key of a key is not the key");
	}
	if (g_strcmp0(twice, key) != 0)
		FAIL("a key is not the same twice");
	g_free(again);
	g_free(twice);
	g_free(key);
}

static gboolean
has_key(
	const gchar *name,
	const gchar *key
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

/*
 * The reading after a known title (axis_release_read_after(), add-anime-support
 * A2) against the names of the first candidate, checked the other way round.
 * Its rule for the title is the matcher's own: a name is the series' when its
 * title key equals the key of one of the series' names, and in no other case.
 *
 *  - Whatever it takes for the series' name in the release name has the key
 *    of one of the candidate's names (the very test has_key() makes for the
 *    matcher above): no part of a word, nothing alike.
 *  - A name made here of the candidate's title, when that is plain words, a
 *    lone hyphen and a two-digit number reads as that number; without the
 *    hyphen too; with one more letter on the title it is another title's; and
 *    the title followed by a year, by half an episode, by 0 or by two numbers
 *    is not read, each for its own reason.
 */
static void
check_after(
	const gchar         *name,
	const AxisMetaTitle *title,
	guint                number
){
	const gchar *names[2 + MAX_OTHER];
	AxisReleaseAfter after;
	guint n = 0;
	guint a;
	gint64 start;

	names[n++] = title->title;
	names[n++] = title->original_title;
	for (a = 0; title->alt_titles != NULL && a < title->alt_titles->len && n < G_N_ELEMENTS(names); a++)
		names[n++] = ((AxisMetaAltTitle *)g_ptr_array_index(title->alt_titles, a))->title;
	start = g_get_monotonic_time();
	axis_release_read_after_full(name, -1, names, n, LATEST, &after);
	if (g_get_monotonic_time() - start > BOUND_US)
		FAIL("reading after a title took too long");
	if (after.read != (after.code == NULL) || (after.read && !after.titled))
		FAIL("a reading after a title that contradicts itself");
	if (after.titled) {
		gchar *part;
		gchar *key;
		gboolean named = FALSE;

		if (after.title_at >= after.title_end || after.title_end > strlen(name))
			FAIL("a title outside the name");
		part = g_strndup(name + after.title_at, after.title_end - after.title_at);
		key = axis_release_title_key(part);
		for (a = 0; key != NULL && key[0] != '\0' && a < n && !named; a++)
			named = has_key(names[a], key);
		if (!named) {
			fprintf(stderr, "'%s' taken for the title in '%s'\n", part, name);
			FAIL("a name taken for the series' that has the key of none of its names");
		}
		g_free(key);
		g_free(part);
	}
	if (after.read && (after.first < 1 || after.last < after.first || after.last > 9999 ||
	    after.last - after.first + 1 > AXIS_RELEASE_MAX_NUMBERS ||
	    (after.digits == 4 && after.first >= AXIS_RELEASE_YEAR_MIN && after.first <= LATEST)))
		FAIL("a number read after a title that is out of range, or a year");

	/* The other way round: names made of the title itself. */
	{
		const gchar *plain = title->title;
		const gchar *only[1];
		gchar *made;
		gsize i;
		gsize length = plain != NULL ? strlen(plain) : 0;
		guint value = number % 99 + 1;

		if (length == 0 || length > 100 || plain[0] == ' ' || plain[length - 1] == ' ')
			return;
		for (i = 0; i < length; i++) {
			if (!g_ascii_isalpha(plain[i]) && !(plain[i] == ' ' && plain[i + 1] != ' '))
				return;
		}
		only[0] = plain;

		made = g_strdup_printf("%s - %02u", plain, value);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (!after.read || after.range || !after.hyphen || after.first != (gint)value || after.last != (gint)value) {
			fprintf(stderr, "'%s'\n", made);
			FAIL("the title, a hyphen and a number were not read as that number");
		}
		g_free(made);

		made = g_strdup_printf("[Group] %s %02u [1080p]", plain, value);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (!after.read || after.range || after.hyphen || after.first != (gint)value) {
			fprintf(stderr, "'%s'\n", made);
			FAIL("the title and a bare number were not read as that number");
		}
		g_free(made);

		made = g_strdup_printf("%sx - %02u", plain, value);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (after.read || after.titled || strcmp(after.code, "other_title") != 0) {
			fprintf(stderr, "'%s'\n", made);
			FAIL("a title with one more letter was taken for the series'");
		}
		g_free(made);

		made = g_strdup_printf("%s - 2019", plain);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (after.read || strcmp(after.code, "year") != 0)
			FAIL("a year after the title was read as an episode");
		g_free(made);

		made = g_strdup_printf("%s - %02u.5", plain, value);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (after.read || strcmp(after.code, "decimal") != 0)
			FAIL("half an episode after the title was read as an episode");
		g_free(made);

		made = g_strdup_printf("%s - 00", plain);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (after.read || strcmp(after.code, "zero") != 0)
			FAIL("0 after the title was read as an episode");
		g_free(made);

		made = g_strdup_printf("%s 2 - %02u", plain, value);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (after.read || strcmp(after.code, "two_numbers") != 0)
			FAIL("two numbers after the title were read as an episode");
		g_free(made);

		made = g_strdup_printf("%s %02u-%02u", plain, value, value + 3);
		axis_release_read_after_full(made, -1, only, 1, LATEST, &after);
		if (!after.read || !after.range || after.first != (gint)value || after.last != (gint)value + 3)
			FAIL("a range after the title was not read as that range");
		g_free(made);
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *text = g_strndup((const gchar *)data, size);
	gchar **lines = g_strsplit(text, "\n", MAX_CANDIDATES + 2);
	AxisMetaTitle *titles[MAX_CANDIDATES];
	AxisRelease *release;
	gchar *key;
	gchar *reason = NULL;
	gint matched = -2;
	guint n = 0;
	guint named_count = 0;
	guint dated = 0;
	guint exact = 0;
	guint uncomparable = 0;
	gint last_named = -1;
	gint last_dated = -1;
	gint last_exact = -1;
	gint expected_match = -1;
	gboolean expected_ambiguous = FALSE;
	gboolean episode;
	AxisReleaseMatch result;
	gint64 start;
	gint64 took;
	guint i;

	if (lines[0] == NULL) {
		g_strfreev(lines);
		g_free(text);
		return 0;
	}
	check_key(lines[0]);
	for (i = 1; lines[i] != NULL && n < MAX_CANDIDATES; i++) {
		gchar **fields = g_strsplit(lines[i], "|", 4 + MAX_OTHER);
		AxisMetaTitle *title;
		guint f;

		title = axis_meta_title_new(fields[0] != NULL && fields[0][0] == 's' ? AXIS_META_KIND_SERIES :
			AXIS_META_KIND_FILM);
		if (fields[0] != NULL && fields[1] != NULL)
			title->year = (gint)g_ascii_strtoll(fields[1], NULL, 10) % 3000;
		if (fields[0] != NULL && fields[1] != NULL && fields[2] != NULL) {
			title->title = g_strdup(fields[2]);
			check_key(fields[2]);
			if (fields[3] != NULL) {
				title->original_title = g_strdup(fields[3]);
				title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
				for (f = 4; fields[f] != NULL; f++) {
					AxisMetaAltTitle *alt = g_new0(AxisMetaAltTitle, 1);

					alt->title = g_strdup(fields[f]);
					g_ptr_array_add(title->alt_titles, alt);
				}
			}
		}
		titles[n++] = title;
		g_strfreev(fields);
	}

	release = axis_release_parse_full(lines[0], -1, LATEST);
	start = g_get_monotonic_time();
	result = axis_release_match(release, (const AxisMetaTitle *const *)titles, n, &matched, &reason);
	took = g_get_monotonic_time() - start;
	if (took > BOUND_US)
		FAIL("matching took too long");
	if (reason == NULL || !g_utf8_validate(reason, -1, NULL))
		FAIL("no reason, or one that is not UTF-8");
	{
		const gchar *p;

		for (p = reason; *p != '\0'; p = g_utf8_next_char(p)) {
			gunichar c = g_utf8_get_char(p);

			if (g_unichar_iscntrl(c) || g_unichar_type(c) == G_UNICODE_FORMAT)
				FAIL("a reason with a control or formatting character");
		}
	}
	if (result != AXIS_RELEASE_MATCH_YES && matched != -1)
		FAIL("a candidate named for an answer that is not a match");
	if (result == AXIS_RELEASE_MATCH_YES && (matched < 0 || (guint)matched >= n))
		FAIL("a match that names no candidate");

	/* The same answer, counted the other way round. */
	key = release->title != NULL ? axis_release_title_key(release->title) : NULL;
	episode = axis_release_is_episode(release);
	for (i = 0; key != NULL && key[0] != '\0' && i < n; i++) {
		const AxisMetaTitle *t = titles[i];
		gboolean named = has_key(t->title, key) || has_key(t->original_title, key);
		guint a;

		for (a = 0; !named && t->alt_titles != NULL && a < t->alt_titles->len; a++)
			named = has_key(((AxisMetaAltTitle *)g_ptr_array_index(t->alt_titles, a))->title, key);
		if (!named || (episode && t->kind != AXIS_META_KIND_SERIES))
			continue;
		named_count++;
		last_named = (gint)i;
		if (release->year <= 0)
			continue;
		if (t->year <= 0) {
			uncomparable++;
		} else if (t->year >= release->year - 1 && t->year <= release->year + 1) {
			dated++;
			last_dated = (gint)i;
			if (t->year == release->year) {
				exact++;
				last_exact = (gint)i;
			}
		}
	}
	if (key != NULL && key[0] != '\0') {
		if (release->year <= 0) {
			if (named_count == 1)
				expected_match = last_named;
			expected_ambiguous = named_count >= 2;
		} else if (exact == 1) {
			expected_match = last_exact;
		} else if (exact == 0 && dated == 1 && uncomparable == 0) {
			expected_match = last_dated;
		} else {
			expected_ambiguous = dated >= 2 || uncomparable > 0;
		}
	}
	switch (result) {
	case AXIS_RELEASE_MATCH_YES:
		if (expected_match < 0 || matched != expected_match)
			FAIL("a match that is not the one candidate the count picks");
		break;
	case AXIS_RELEASE_MATCH_AMBIGUOUS:
		if (!expected_ambiguous)
			FAIL("ambiguous where the count finds no two candidates, nor one that cannot be compared");
		break;
	default:
		if (expected_match >= 0 || expected_ambiguous)
			FAIL("no match although a candidate fits");
		break;
	}

	if (n > 0)
		check_after(lines[0], titles[0], (guint)size);

	g_free(key);
	g_free(reason);
	axis_release_free(release);
	for (i = 0; i < n; i++)
		axis_meta_title_free(titles[i]);
	g_strfreev(lines);
	g_free(text);
	return 0;
}
