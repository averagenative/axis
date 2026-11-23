/*
 * axis-identify-ids.c - provider identifiers in names and in .nfo bytes (see
 * the header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "identify/axis-identify-ids.h"

#include <string.h>

void
axis_identify_ids_init(AxisIdentifyIds *ids)
{
	memset(ids, 0, sizeof(*ids));
}

gboolean
axis_identify_ids_any(const AxisIdentifyIds *ids)
{
	return ids->tmdb[0] != '\0' || ids->imdb[0] != '\0';
}

gboolean
axis_identify_ids_conflict(const AxisIdentifyIds *ids)
{
	return ids->tmdb_conflict || ids->imdb_conflict;
}

/* ── Small pieces ───────────────────────────────────────────────── */

static guint8
lower(guint8 c)
{
	return c >= 'A' && c <= 'Z' ? (guint8)(c - 'A' + 'a') : c;
}

static gboolean
is_digit(guint8 c)
{
	return c >= '0' && c <= '9';
}

static gboolean
is_alnum(guint8 c)
{
	c = lower(c);
	return is_digit(c) || (c >= 'a' && c <= 'z');
}

static gboolean
is_space(guint8 c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Whether @word (lower case) is at @p, ignoring case, within @left bytes. */
static gboolean
at_word(
	const guint8 *p,
	gsize         left,
	const gchar  *word
){
	gsize n = strlen(word);
	gsize i;

	if (left < n)
		return FALSE;
	for (i = 0; i < n; i++) {
		if (lower(p[i]) != (guint8)word[i])
			return FALSE;
	}
	return TRUE;
}

/* How many digits start at @p (at most @max + 1, so that "too many" shows). */
static gsize
count_digits(
	const guint8 *p,
	gsize         left,
	gsize         max
){
	gsize n = 0;

	while (n < left && n <= max && is_digit(p[n]))
		n++;
	return n;
}

/* Records @value of @scheme in @ids: a second, different one is a conflict. */
static void
record(
	gchar        slot[16],
	gboolean    *conflict,
	const guint8 *value,
	gsize         length,
	gboolean      tt
){
	gchar text[16];
	gsize at = 0;
	gsize i;

	if (length + (tt ? 2 : 0) >= sizeof(text))
		return;
	if (tt) {
		text[at++] = 't';
		text[at++] = 't';
	}
	for (i = 0; i < length; i++)
		text[at++] = (gchar)value[i];
	text[at] = '\0';
	if (slot[0] == '\0' && !*conflict) {
		memcpy(slot, text, at + 1);
	} else if (strcmp(slot, text) != 0) {
		*conflict = TRUE;
		slot[0] = '\0';
	}
}

/* ── Names ──────────────────────────────────────────────────────── */

typedef enum
{
	SCHEME_NONE,
	SCHEME_TMDB,
	SCHEME_IMDB,
	SCHEME_TVDB
} Scheme;

/*
 * A token at @p ('{' or '['): its length (to its closing bracket, included),
 * 0 when there is none.  Its scheme and the value's place in @p.
 */
static gsize
token_at(
	const guint8 *p,
	gsize         left,
	Scheme       *scheme,
	gsize        *value_at,
	gsize        *value_length
){
	guint8 close = p[0] == '{' ? '}' : ']';
	gsize at = 1;
	gsize digits;

	*scheme = SCHEME_NONE;
	if (at_word(p + at, left - at, "tmdbid")) {
		*scheme = SCHEME_TMDB;
		at += 6;
	} else if (at_word(p + at, left - at, "tmdb")) {
		*scheme = SCHEME_TMDB;
		at += 4;
	} else if (at_word(p + at, left - at, "imdbid")) {
		*scheme = SCHEME_IMDB;
		at += 6;
	} else if (at_word(p + at, left - at, "imdb")) {
		*scheme = SCHEME_IMDB;
		at += 4;
	} else if (at_word(p + at, left - at, "tvdbid")) {
		*scheme = SCHEME_TVDB;
		at += 6;
	} else if (at_word(p + at, left - at, "tvdb")) {
		*scheme = SCHEME_TVDB;
		at += 4;
	} else {
		return 0;
	}
	if (at >= left || (p[at] != '-' && p[at] != '='))
		return 0;
	at++;
	if (*scheme == SCHEME_IMDB) {
		if (!at_word(p + at, left - at, "tt"))
			return 0;
		at += 2;
		digits = count_digits(p + at, left - at, 9);
		if (digits < 7 || digits > 9)
			return 0;
	} else {
		digits = count_digits(p + at, left - at, 12);
		if (digits < 1 || digits > 12 || p[at] == '0')
			return 0;
	}
	*value_at = at;
	*value_length = digits;
	at += digits;
	if (at >= left || p[at] != close)
		return 0;
	return at + 1;
}

gchar *
axis_identify_name_ids(
	const gchar     *name,
	gssize           length,
	AxisIdentifyIds *ids
){
	const guint8 *p = (const guint8 *)name;
	g_autoptr(GString) out = NULL;
	gboolean space = TRUE;
	gsize n;
	gsize i;

	axis_identify_ids_init(ids);
	if (name == NULL)
		return g_strdup("");
	n = length < 0 ? strlen(name) : (gsize)length;
	if (memchr(name, '\0', n) != NULL)
		return g_strdup(name);
	if (n > AXIS_IDENTIFY_NAME_MAX)
		return g_strndup(name, n);
	out = g_string_sized_new(n);
	for (i = 0; i < n; i++) {
		Scheme scheme = SCHEME_NONE;
		gsize value_at = 0;
		gsize value_length = 0;
		gsize token = 0;

		if (p[i] == '{' || p[i] == '[')
			token = token_at(p + i, n - i, &scheme, &value_at, &value_length);
		if (token > 0) {
			if (scheme == SCHEME_TMDB)
				record(ids->tmdb, &ids->tmdb_conflict, p + i + value_at, value_length, FALSE);
			else if (scheme == SCHEME_IMDB)
				record(ids->imdb, &ids->imdb_conflict, p + i + value_at, value_length, TRUE);
			else
				record(ids->tvdb, &ids->tvdb_conflict, p + i + value_at, value_length, FALSE);
			i += token - 1;
			/* The token goes; one space stands where it was. */
			if (!space) {
				g_string_append_c(out, ' ');
				space = TRUE;
			}
			continue;
		}
		if (p[i] == ' ') {
			if (space)
				continue;
			space = TRUE;
		} else {
			space = FALSE;
		}
		g_string_append_c(out, (gchar)p[i]);
	}
	while (out->len > 0 && out->str[out->len - 1] == ' ')
		g_string_truncate(out, out->len - 1);
	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── .nfo bytes ─────────────────────────────────────────────────── */

/* "tt" and 7 to 9 digits, standing alone, at @i. */
static gboolean
imdb_at(
	const guint8 *p,
	gsize         n,
	gsize         i,
	gsize        *digits
){
	gsize d;

	if (i + 2 > n || lower(p[i]) != 't' || lower(p[i + 1]) != 't')
		return FALSE;
	if (i > 0 && is_alnum(p[i - 1]))
		return FALSE;
	d = count_digits(p + i + 2, n - i - 2, 9);
	if (d < 7 || d > 9)
		return FALSE;
	if (i + 2 + d < n && is_alnum(p[i + 2 + d]))
		return FALSE;
	*digits = d;
	return TRUE;
}

/* A TMDb number at @at: 1 to 12 digits, no leading 0, no digit after. */
static gsize
tmdb_number(
	const guint8 *p,
	gsize         n,
	gsize         at
){
	gsize d;

	if (at >= n || p[at] == '0')
		return 0;
	d = count_digits(p + at, n - at, 12);
	if (d < 1 || d > 12)
		return 0;
	return d;
}

/* After a tag that ends at @at: white space, a number, white space, '<'. */
static gsize
tmdb_element(
	const guint8 *p,
	gsize         n,
	gsize         at,
	gsize        *start
){
	gsize d;
	gsize end;

	while (at < n && is_space(p[at]))
		at++;
	d = tmdb_number(p, n, at);
	if (d == 0)
		return 0;
	end = at + d;
	while (end < n && is_space(p[end]))
		end++;
	if (end >= n || p[end] != '<')
		return 0;
	*start = at;
	return d;
}

/* "<uniqueid" whose attributes, before the tag's '>', hold type="tmdb": the
 * number in it. */
static gsize
uniqueid_at(
	const guint8 *p,
	gsize         n,
	gsize         i,
	gsize        *start
){
	gsize at = i + 9;
	gsize limit;
	gboolean tmdb = FALSE;

	if (!at_word(p + i, n - i, "<uniqueid") || at >= n || (!is_space(p[at]) && p[at] != '>'))
		return 0;
	limit = MIN(n, at + 256);
	while (at < limit && p[at] != '>') {
		if (at_word(p + at, limit - at, "type=") && at + 5 < limit && (p[at + 5] == '"' || p[at + 5] == '\'')) {
			guint8 quote = p[at + 5];

			if (at_word(p + at + 6, limit - at - 6, "tmdb") && at + 10 < limit && p[at + 10] == quote &&
			    (at == 0 || !is_alnum(p[at - 1])))
				tmdb = TRUE;
		}
		at++;
	}
	if (at >= limit || !tmdb)
		return 0;
	return tmdb_element(p, n, at + 1, start);
}

void
axis_identify_nfo_scan(
	const guint8    *data,
	gsize            length,
	AxisIdentifyIds *ids
){
	static const gchar site[] = "themoviedb.org/movie/";
	gsize n = MIN(length, (gsize)AXIS_IDENTIFY_NFO_MAX);
	gsize i;

	axis_identify_ids_init(ids);
	if (data == NULL)
		return;
	for (i = 0; i < n; i++) {
		guint8 c = lower(data[i]);
		gsize d = 0;
		gsize start = 0;

		if (c == 't' && imdb_at(data, n, i, &d)) {
			record(ids->imdb, &ids->imdb_conflict, data + i + 2, d, TRUE);
			i += 1 + d;
		} else if (c == 't' && at_word(data + i, n - i, site)) {
			gsize at = i + sizeof(site) - 1;

			d = tmdb_number(data, n, at);
			if (d > 0 && (at + d >= n || !is_digit(data[at + d]))) {
				record(ids->tmdb, &ids->tmdb_conflict, data + at, d, FALSE);
				i = at + d - 1;
			}
		} else if (c == '<' && at_word(data + i, n - i, "<tmdbid>")) {
			d = tmdb_element(data, n, i + 8, &start);
			if (d > 0) {
				record(ids->tmdb, &ids->tmdb_conflict, data + start, d, FALSE);
				i = start + d - 1;
			}
		} else if (c == '<' && at_word(data + i, n - i, "<uniqueid")) {
			d = uniqueid_at(data, n, i, &start);
			if (d > 0) {
				record(ids->tmdb, &ids->tmdb_conflict, data + start, d, FALSE);
				i = start + d - 1;
			}
		}
	}
}
