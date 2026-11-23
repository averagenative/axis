/*
 * fuzz-release-parse.c - libFuzzer target for the release-name parser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is the name, as bytes.  Whatever they are, the parser must not
 * crash, read past them or take long, and:
 *
 *  - it returns within a fixed time (the name is capped, and reading is
 *    linear in it);
 *  - bytes that are not valid UTF-8, contain a NUL, or are longer than the
 *    limit find nothing at all;
 *  - the title lies within the input: it is no longer than the input, and
 *    its words, separated by single spaces, are pieces of the input in
 *    order; it is valid UTF-8 with no control or invisible formatting
 *    character; the group, the checksum and every word not understood are
 *    pieces of the input too;
 *  - every number is in range (years believed, episode lists within their
 *    limit, real dates, at most eight languages), and every enumeration
 *    has a name;
 *  - reading the same bytes twice, or as a NUL-terminated string, gives
 *    the same reading, and the title's key is stable;
 *  - the reading after a known title (check_after()) holds to its own
 *    limits on the same bytes.
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

/* Far above what any input takes; a step that went quadratic, or worse,
 * would pass it. */
#define BOUND_US (250 * 1000)

#define FAIL(why) do { fprintf(stderr, "fuzz-release-parse: %s\n", why); abort(); } while (0)

/* Whether @piece occurs in @data at or after *@from; moves *@from past it. */
static gboolean
piece_of(
	const uint8_t *data,
	size_t         size,
	const gchar   *piece,
	size_t        *from
){
	size_t length = strlen(piece);
	size_t i;

	if (length == 0 || length > size)
		return FALSE;
	for (i = *from; i + length <= size; i++) {
		if (memcmp(data + i, piece, length) == 0) {
			*from = i + length;
			return TRUE;
		}
	}
	return FALSE;
}

static gboolean
clean_text(const gchar *text)
{
	const gchar *p;

	if (!g_utf8_validate(text, -1, NULL))
		return FALSE;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c != ' ' && (g_unichar_iscntrl(c) || g_unichar_type(c) == G_UNICODE_FORMAT || g_unichar_isspace(c)))
			return FALSE;
	}
	return TRUE;
}

static void
check_nothing(const AxisRelease *r)
{
	if (r->title != NULL || r->year != 0 || r->season_first != -1 || r->season_last != -1 ||
	    r->episode_count != 0 || r->absolute_count != 0 || r->air_year != 0 || r->complete ||
	    r->version != 0 || r->resolution != 0 || r->source != 0 || r->service != NULL || r->codec != 0 ||
	    r->bit_depth != 0 || r->hdr != 0 || r->audio != 0 || r->atmos || r->channels[0] != '\0' ||
	    r->editions != 0 || r->language_count != 0 || r->language_marks != 0 || r->revision != 0 ||
	    r->marks != 0 || r->stop != 0 || r->group != NULL || r->checksum != NULL || r->unknown->len != 0)
		FAIL("unreadable input found something");
}

static void
check_reading(
	const AxisRelease *r,
	const uint8_t     *data,
	size_t             size
){
	size_t from = 0;
	guint i;

	if (r->title != NULL) {
		gchar **words;

		if (r->title[0] == '\0' || strlen(r->title) > size || !clean_text(r->title))
			FAIL("a title that is empty, too long or not clean");
		if (r->title[0] == ' ' || r->title[strlen(r->title) - 1] == ' ' || strstr(r->title, "  ") != NULL)
			FAIL("a title with stray spaces");
		words = g_strsplit(r->title, " ", -1);
		for (i = 0; words[i] != NULL; i++) {
			if (!piece_of(data, size, words[i], &from)) {
				fprintf(stderr, "word '%s' of '%s'\n", words[i], r->title);
				FAIL("a title word that is not in the input, in order");
			}
		}
		g_strfreev(words);
		{
			gchar *key = axis_release_title_key(r->title);
			gchar *again = key != NULL ? axis_release_title_key(key) : NULL;

			if (key == NULL || g_strcmp0(key, again) != 0)
				FAIL("the title's key is not stable");
			g_free(key);
			g_free(again);
		}
	}
	if (r->group != NULL) {
		size_t at = 0;

		if (strlen(r->group) > 48 || !g_utf8_validate(r->group, -1, NULL) || !piece_of(data, size, r->group, &at))
			FAIL("a group that is not a piece of the input");
	}
	if (r->checksum != NULL) {
		size_t at = 0;

		if (strlen(r->checksum) != 8 || !piece_of(data, size, r->checksum, &at))
			FAIL("a checksum that is not eight characters of the input");
		for (i = 0; i < 8; i++) {
			if (!g_ascii_isxdigit(r->checksum[i]))
				FAIL("a checksum that is not hex");
		}
	}
	for (i = 0; i < r->unknown->len; i++) {
		const gchar *word = g_ptr_array_index(r->unknown, i);
		size_t at = 0;

		if (word == NULL || word[0] == '\0' || !clean_text(word) || strchr(word, ' ') != NULL ||
		    !piece_of(data, size, word, &at))
			FAIL("a word not understood that is not a piece of the input");
	}

	if (r->year != 0 && (r->year < AXIS_RELEASE_YEAR_MIN || r->year > LATEST))
		FAIL("a year out of range");
	if ((r->season_first < 0) != (r->season_last < 0) || r->season_last < r->season_first ||
	    r->season_last - r->season_first + 1 > AXIS_RELEASE_MAX_NUMBERS || r->season_last > 9999)
		FAIL("seasons out of range");
	if (r->episode_count > AXIS_RELEASE_MAX_NUMBERS || r->absolute_count > AXIS_RELEASE_MAX_NUMBERS)
		FAIL("too many episodes");
	for (i = 0; i < r->episode_count; i++) {
		if (r->episodes[i] < 0 || r->episodes[i] > 9999)
			FAIL("an episode out of range");
	}
	for (i = 0; i < r->absolute_count; i++) {
		if (r->absolute[i] < 0 || r->absolute[i] > 9999)
			FAIL("an absolute number out of range");
	}
	if (r->air_year != 0 &&
	    (r->air_year < AXIS_RELEASE_YEAR_MIN || r->air_year > LATEST ||
	     !g_date_valid_dmy((GDateDay)r->air_day, (GDateMonth)r->air_month, (GDateYear)r->air_year)))
		FAIL("an air date that is not a date");
	if (r->air_year == 0 && (r->air_month != 0 || r->air_day != 0))
		FAIL("half an air date");
	if (r->language_count > AXIS_RELEASE_MAX_LANGUAGES)
		FAIL("too many languages");
	for (i = 0; i < r->language_count; i++) {
		if (r->languages[i] == NULL || strlen(r->languages[i]) != 2)
			FAIL("a language that is not a code");
	}
	if ((r->resolution != 0 && axis_release_resolution_name(r->resolution) == NULL) ||
	    (r->source != 0 && axis_release_source_name(r->source) == NULL) ||
	    (r->codec != 0 && axis_release_codec_name(r->codec) == NULL))
		FAIL("a value without a name");
	if (r->interlaced && r->resolution == 0)
		FAIL("interlaced without a resolution");
	if (r->bit_depth != 0 && r->bit_depth != 8 && r->bit_depth != 10 && r->bit_depth != 12)
		FAIL("a bit depth out of range");
	if (r->channels[0] != '\0') {
		size_t at = 0;

		if (!piece_of(data, size, r->channels, &at) && !(strlen(r->channels) == 3 && r->channels[1] == '.'))
			FAIL("channels that are not in the input");
	}
	if (r->version < 0 || r->version > 99)
		FAIL("a version out of range");
	if ((r->hdr >> 6) != 0 || (r->audio >> 12) != 0 || (r->editions >> 18) != 0 || (r->language_marks >> 5) != 0 ||
	    (r->revision >> 4) != 0 || (r->marks >> 5) != 0 || (r->stop >> 7) != 0)
		FAIL("an unknown flag");
}

/*
 * The reading after a known title (axis_release_read_after(), add-anime-support
 * A2), which is a second reader of the same hostile names: against the title
 * the parser itself read from these bytes, and against a fixed one.  It must
 * return in time, read nothing from bytes the parser would not read, and
 * whatever it reads is one number or one range within its limits, never a
 * year, with the name's own title before it.
 */
static void
check_after(
	const uint8_t *data,
	size_t         size,
	const gchar   *title,
	gboolean       readable
){
	const gchar *titles[3];
	AxisReleaseAfter after;
	AxisReleaseAfter again;
	gint64 start = g_get_monotonic_time();
	gint64 took;

	titles[0] = title;
	titles[1] = "Tenkuu Rangers";
	titles[2] = NULL;
	axis_release_read_after_full((const gchar *)data, (gssize)size, titles, 3, LATEST, &after);
	took = g_get_monotonic_time() - start;
	if (took > BOUND_US)
		FAIL("reading after a title took too long");
	axis_release_read_after_full((const gchar *)data, (gssize)size, titles, 3, LATEST, &again);
	if (after.read != again.read || after.code != again.code || after.first != again.first || after.last != again.last ||
	    after.range != again.range || after.hyphen != again.hyphen || after.digits != again.digits ||
	    after.version != again.version || after.title_at != again.title_at || after.title_end != again.title_end ||
	    after.number_end != again.number_end || after.titled != again.titled)
		FAIL("two readings after a title differ");
	if (after.read != (after.code == NULL))
		FAIL("read without a code, or a code with a reading");
	if (!readable && (after.read || after.titled || strcmp(after.code, "unreadable") != 0))
		FAIL("unreadable input was read after a title");
	if (after.titled && (after.title_at >= after.title_end || after.title_end > size))
		FAIL("a title outside the name");
	if (!after.read) {
		if (after.first != 0 || after.last != 0 || after.range || after.version != 0)
			FAIL("a name not read that gives a number");
		if (after.titled && strcmp(after.code, "other_title") == 0)
			FAIL("the series' title found and not found");
		return;
	}
	if (!after.titled)
		FAIL("a number read after no title");
	if (after.first < 1 || after.last < after.first || after.last > 9999)
		FAIL("a number read after a title that is out of range");
	if (after.range != (after.last > after.first) || after.last - after.first + 1 > AXIS_RELEASE_MAX_NUMBERS)
		FAIL("a range read after a title that is no range, or too wide");
	if (after.digits < 1 || after.digits > 4 || after.version < 0 || after.version > 9)
		FAIL("digits or a version out of range");
	if (after.digits == 4 && after.first >= AXIS_RELEASE_YEAR_MIN && after.first <= LATEST)
		FAIL("a year read as an episode number");
	if (!after.range && !after.hyphen && after.digits < 2)
		FAIL("a bare single digit read as an episode number");
	if (after.number_end <= after.title_end || after.number_end > size)
		FAIL("a number that does not follow the title within the name");
	/* The number's digits stand in the name, after the title. */
	{
		size_t i;
		gboolean digit = FALSE;

		for (i = after.title_end; i < after.number_end; i++)
			digit = digit || (data[i] >= '0' && data[i] <= '9');
		if (!digit)
			FAIL("a number read where the name has no digit");
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gint64 start = g_get_monotonic_time();
	AxisRelease *r = axis_release_parse_full((const gchar *)data, (gssize)size, LATEST);
	gint64 took = g_get_monotonic_time() - start;
	gboolean readable = size > 0 && size <= AXIS_RELEASE_NAME_MAX && memchr(data, '\0', size) == NULL &&
		g_utf8_validate((const gchar *)data, (gssize)size, NULL);
	gchar *line;

	if (took > BOUND_US) {
		fprintf(stderr, "took %" G_GINT64_FORMAT " us\n", took);
		FAIL("reading took too long");
	}
	if (r == NULL || r->unknown == NULL)
		FAIL("no reading");
	if (!readable)
		check_nothing(r);
	else
		check_reading(r, data, size);
	check_after(data, size, r->title, readable);

	line = axis_release_describe(r);
	{
		AxisRelease *again = axis_release_parse_full((const gchar *)data, (gssize)size, LATEST);
		gchar *line2 = axis_release_describe(again);

		if (strcmp(line, line2) != 0)
			FAIL("two readings of the same bytes differ");
		g_free(line2);
		axis_release_free(again);
	}
	if (memchr(data, '\0', size) == NULL) {
		gchar *text = g_strndup((const gchar *)data, size);
		AxisRelease *terminated = axis_release_parse_full(text, -1, LATEST);
		gchar *line3 = axis_release_describe(terminated);

		if (strcmp(line, line3) != 0)
			FAIL("reading a NUL-terminated copy differs");
		g_free(line3);
		axis_release_free(terminated);
		g_free(text);
	}
	g_free(line);
	axis_release_free(r);
	return 0;
}
