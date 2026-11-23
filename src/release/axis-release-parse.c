/*
 * axis-release-parse.c - reading a release name (see axis-release.h for the
 * rules)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is a single pass, or a fixed number of passes, over at
 * most AXIS_RELEASE_NAME_MAX bytes and the tokens made of them; each look
 * at a token compares it with the fixed tables, and no step looks further
 * ahead than a few tokens.  Memory is one fixed-size work structure.
 */

#include "release/axis-release-private.h"

#include <string.h>

#define MAX_TOKENS 512
#define MAX_PARTS  (AXIS_RELEASE_NAME_MAX / 2 + 2)
#define GROUP_MAX  48

enum
{
	TOK_BRACKET      = 1 << 0,	/* inside brackets */
	TOK_AFTER_HYPHEN = 1 << 1,	/* glued by a hyphen to the token before */
	TOK_HYPHEN       = 1 << 2	/* a lone run of hyphens: a separator */
};

typedef struct
{
	gchar *text;	/* in Parse.buffer */
	gsize  offset;	/* in the name */
	gsize  length;
	guint  flags;
} Token;

/* What an episode mark says, before it is applied. */
typedef struct
{
	gint     season_first;
	gint     season_last;
	guint    n_episodes;
	gint     episodes[AXIS_RELEASE_MAX_NUMBERS];
	guint    n_absolute;
	gint     absolute[AXIS_RELEASE_MAX_NUMBERS];
	gint     year;
	gint     month;
	gint     day;
	gboolean complete;
	gint     version;
	gboolean unbelievable;
} Marker;

/* Where a word of the tail stands. */
enum
{
	ZONE_FILM,	/* after a year: soft words count */
	ZONE_EPISODE,	/* after an episode mark: soft words count as scene tags */
	ZONE_QUALITY	/* after the first hard token: everything counts */
};

typedef enum
{
	FORM_NONE = 0,
	FORM_CHANNELS,		/* 5.1 */
	FORM_AUDIO_CHANNELS,	/* DDP5.1 */
	FORM_AUDIO_SPLIT,	/* DDP5 1: a feed that turned the dot into a space */
	FORM_CHANNELS_SPLIT,	/* AAC 5 1: the same, after an audio word */
	FORM_DIMENSIONS,	/* 1920x1080 */
	FORM_SOURCE_RES,	/* br720p, Bluray1080p: a source glued to a resolution */
	FORM_VERSION,		/* v2 */
	FORM_CHECKSUM		/* [ABCD1234] */
} Form;

typedef struct
{
	gchar       *name;		/* a NUL-terminated copy */
	gsize        length;
	gint         latest_year;
	gboolean     leading_group;
	Token        tokens[MAX_TOKENS];
	const gchar *texts[MAX_TOKENS];
	gboolean     claimed[MAX_TOKENS];
	gboolean     year_ahead[MAX_TOKENS + 1];
	guint        n;
	gchar        buffer[AXIS_RELEASE_NAME_MAX * 2 + MAX_TOKENS + 8];
	gsize        used;
	guint        res_rank;
	guint        source_rank;
	AxisRelease *r;
	Marker       marker;
} Parse;

/* ── Small helpers ──────────────────────────────────────────────── */

static gboolean
is_word(
	const gchar *token,
	const gchar *word
){
	return g_ascii_strcasecmp(token, word) == 0;
}

static gboolean
all_digits(const gchar *s)
{
	if (*s == '\0')
		return FALSE;
	for (; *s != '\0'; s++) {
		if (!g_ascii_isdigit(*s))
			return FALSE;
	}
	return TRUE;
}

/* 1 to @max digits at *@p (fewer than @min fails); moves *@p past them. */
static gboolean
read_number(
	const gchar **p,
	guint         min,
	guint         max,
	gint         *value
){
	const gchar *s = *p;
	guint n = 0;
	gint v = 0;

	while (n <= max && g_ascii_isdigit(s[n])) {
		v = v * 10 + (s[n] - '0');
		n++;
	}
	if (n < min || n > max)
		return FALSE;
	*value = v;
	*p = s + n;
	return TRUE;
}

static gboolean
plausible_year(
	const Parse *p,
	gint         year
){
	return year >= AXIS_RELEASE_YEAR_MIN && year <= p->latest_year;
}

/* A token that is four digits and a year believed. */
static gboolean
year_token(
	const Parse *p,
	const gchar *t
){
	gint value;
	const gchar *q = t;

	if (strlen(t) != 4 || !read_number(&q, 4, 4, &value))
		return FALSE;
	return plausible_year(p, value);
}

/*
 * A capital after the first character: how names write their tags
 * (GERMAN, PROPER, iNTERNAL, 3D), and not how they write title words
 * (German, Proper).
 */
static gboolean
scene_cased(const gchar *t)
{
	const gchar *p;

	if (t[0] == '\0')
		return FALSE;
	for (p = t + 1; *p != '\0'; p++) {
		if (g_ascii_isupper(*p))
			return TRUE;
	}
	return FALSE;
}

/* "1st", "70th", "25TH": one to three digits and an ordinal's ending. */
static gboolean
ordinal_text(const gchar *t)
{
	gsize n = 0;

	while (n < 3 && g_ascii_isdigit(t[n]))
		n++;
	if (n == 0 || strlen(t) != n + 2)
		return FALSE;
	return g_ascii_strcasecmp(t + n, "st") == 0 || g_ascii_strcasecmp(t + n, "nd") == 0 ||
		g_ascii_strcasecmp(t + n, "rd") == 0 || g_ascii_strcasecmp(t + n, "th") == 0;
}

/* Letters, none of them lower case. */
static gboolean
all_capitals(const gchar *t)
{
	const gchar *p;
	gboolean letter = FALSE;

	for (p = t; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_islower(c))
			return FALSE;
		if (g_unichar_isalpha(c))
			letter = TRUE;
	}
	return letter;
}

static gboolean
has_letter(const gchar *t)
{
	const gchar *p;

	for (p = t; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_isalpha(g_utf8_get_char(p)))
			return TRUE;
	}
	return FALSE;
}

/* Characters that never belong to a word: they part tokens and are never
 * part of a title.  Returns the length of the one at @i, 0 for none. */
static gsize
separator_at(
	const gchar *s,
	gsize        i
){
	guchar c = (guchar)s[i];

	if (c == ' ' || c == '.' || c == '_' || c == '[' || c == ']' || c == '(' || c == ')' || c == '{' ||
	    c == '}' || c < 0x20 || c == 0x7f)
		return 1;
	if (c >= 0x80) {
		gunichar u = g_utf8_get_char(s + i);

		if (g_unichar_isspace(u) || g_unichar_iscntrl(u) || g_unichar_type(u) == G_UNICODE_FORMAT ||
		    g_unichar_type(u) == G_UNICODE_LINE_SEPARATOR || g_unichar_type(u) == G_UNICODE_PARAGRAPH_SEPARATOR ||
		    g_unichar_type(u) == G_UNICODE_SPACE_SEPARATOR)
			return (gsize)g_utf8_skip[c];
	}
	return 0;
}

/* ── Single-token forms ─────────────────────────────────────────── */

/* "5.1", "2.0", "7.1.4". */
static gboolean
channels_text(const gchar *t)
{
	if (!g_ascii_isdigit(t[0]) || t[0] == '0' || t[1] != '.' || !g_ascii_isdigit(t[2]))
		return FALSE;
	if (t[3] == '\0')
		return TRUE;
	return t[3] == '.' && g_ascii_isdigit(t[4]) && t[5] == '\0';
}

/* An audio codec glued to its channels: "DDP5.1", "AAC2.0", "EAC3.5.1". */
static const AxisReleaseRule *
audio_channels(
	const gchar  *t,
	const gchar **channels
){
	gsize length = 0;
	const AxisReleaseRule *rule = axis_release_rule_audio_prefix(t, &length);
	const gchar *rest;

	if (rule == NULL)
		return NULL;
	rest = t + length;
	if (*rest == '.')
		rest++;
	if (!channels_text(rest))
		return NULL;
	*channels = rest;
	return rule;
}

/* "1920x1080", "1280x720": the resolution they are, by height or width. */
static AxisReleaseResolution
dimensions(const gchar *t)
{
	const gchar *p = t;
	gint width;
	gint height;

	if (!read_number(&p, 3, 4, &width) || (*p != 'x' && *p != 'X'))
		return AXIS_RELEASE_RESOLUTION_NONE;
	p++;
	if (!read_number(&p, 3, 4, &height) || *p != '\0')
		return AXIS_RELEASE_RESOLUTION_NONE;
	switch (height) {
	case 2160: return AXIS_RELEASE_RESOLUTION_2160P;
	case 1080: return AXIS_RELEASE_RESOLUTION_1080P;
	case 720:  return AXIS_RELEASE_RESOLUTION_720P;
	case 576:  return AXIS_RELEASE_RESOLUTION_576P;
	case 480:  return AXIS_RELEASE_RESOLUTION_480P;
	default:   break;
	}
	switch (width) {
	case 3840:
	case 4096: return AXIS_RELEASE_RESOLUTION_2160P;
	case 1920: return AXIS_RELEASE_RESOLUTION_1080P;
	case 1280: return AXIS_RELEASE_RESOLUTION_720P;
	default:   return AXIS_RELEASE_RESOLUTION_NONE;
	}
}

/* A source word glued to a resolution word: "br720p", "Bluray1080p",
 * "WEB1080p".  Both words must be in the tables. */
static gboolean
source_resolution(
	const gchar            *t,
	const AxisReleaseRule **source,
	const AxisReleaseRule **resolution
){
	gsize length = strlen(t);
	gsize cut;

	/* Cheaply first: a resolution word ends in a digit and p or i. */
	if (length < 6 || !g_ascii_isdigit(t[length - 2]) || g_ascii_isdigit(t[0]) ||
	    (t[length - 1] != 'p' && t[length - 1] != 'P' && t[length - 1] != 'i' && t[length - 1] != 'I'))
		return FALSE;
	for (cut = 4; cut <= 5 && cut + 2 <= length; cut++) {
		const AxisReleaseRule *res = axis_release_rule_word(t + length - cut);
		const AxisReleaseRule *src;
		gchar prefix[16];

		if (res == NULL || res->field != AXIS_RULE_RESOLUTION || length - cut >= sizeof(prefix))
			continue;
		memcpy(prefix, t, length - cut);
		prefix[length - cut] = '\0';
		src = axis_release_rule_word(prefix);
		if (src == NULL || src->field != AXIS_RULE_SOURCE)
			continue;
		*source = src;
		*resolution = res;
		return TRUE;
	}
	return FALSE;
}

static gboolean
version_text(
	const gchar *t,
	gint        *version
){
	const gchar *p = t + 1;

	if (t[0] != 'v' && t[0] != 'V')
		return FALSE;
	return read_number(&p, 1, 2, version) && *p == '\0' && *version >= 1;
}

/* Eight hex digits that are not a date. */
static gboolean
checksum_text(const gchar *t)
{
	guint i;
	gint year;
	gint month;
	gint day;

	for (i = 0; i < 8; i++) {
		if (!g_ascii_isxdigit(t[i]))
			return FALSE;
	}
	if (t[8] != '\0')
		return FALSE;
	if (all_digits(t)) {
		year = (t[0] - '0') * 1000 + (t[1] - '0') * 100 + (t[2] - '0') * 10 + (t[3] - '0');
		month = (t[4] - '0') * 10 + (t[5] - '0');
		day = (t[6] - '0') * 10 + (t[7] - '0');
		if (g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
			return FALSE;
	}
	return TRUE;
}

/* ── Episode marks ──────────────────────────────────────────────── */

static void
marker_reset(Marker *m)
{
	m->season_first = -1;
	m->season_last = -1;
	m->n_episodes = 0;
	m->n_absolute = 0;
	m->year = 0;
	m->month = 0;
	m->day = 0;
	m->complete = FALSE;
	m->version = 0;
	m->unbelievable = FALSE;
}

static void
push_number(
	Marker *m,
	gint   *list,
	guint  *count,
	gint    value
){
	if (*count >= AXIS_RELEASE_MAX_NUMBERS) {
		m->unbelievable = TRUE;
		return;
	}
	list[(*count)++] = value;
}

/* Writes out a range from the last number in @list to @last. */
static void
push_range(
	Marker *m,
	gint   *list,
	guint  *count,
	gint    last
){
	gint first;
	gint v;

	if (*count == 0) {
		m->unbelievable = TRUE;
		return;
	}
	first = list[*count - 1];
	/* "S03E12-12": a range of one. */
	if (last == first)
		return;
	if (last < first || last - first + 1 > AXIS_RELEASE_MAX_NUMBERS ||
	    *count + (guint)(last - first) > AXIS_RELEASE_MAX_NUMBERS) {
		m->unbelievable = TRUE;
		return;
	}
	for (v = first + 1; v <= last; v++)
		list[(*count)++] = v;
}

/* "S01E02", "S01E02E03", "S01E02-E04", "S01E02-04", "S01E01-S01E03",
 * "S01", "S01-S03", "S01-03", "S01E02v2". */
static gboolean
token_season_episode(
	const gchar *t,
	Marker      *m
){
	const gchar *p = t;
	gint season;
	gint value;

	if (*p != 'S' && *p != 's')
		return FALSE;
	p++;
	if (!read_number(&p, 1, 4, &season))
		return FALSE;
	marker_reset(m);
	m->season_first = season;
	m->season_last = season;
	if (*p == '\0')
		return TRUE;
	if (*p == '-') {
		p++;
		if (*p == 'S' || *p == 's')
			p++;
		if (!read_number(&p, 1, 4, &value) || *p != '\0')
			return FALSE;
		m->season_last = value;
		if (value <= season || value - season + 1 > AXIS_RELEASE_MAX_NUMBERS)
			m->unbelievable = TRUE;
		return TRUE;
	}
	if (*p != 'E' && *p != 'e')
		return FALSE;
	while (*p == 'E' || *p == 'e') {
		p++;
		if (!read_number(&p, 1, 4, &value))
			return FALSE;
		push_number(m, m->episodes, &m->n_episodes, value);
		if (*p == '-') {
			const gchar *q = p + 1;
			gboolean bare = TRUE;

			if (*q == 'S' || *q == 's') {
				gint other;

				q++;
				if (!read_number(&q, 1, 4, &other) || (*q != 'E' && *q != 'e'))
					return FALSE;
				/* Across seasons: what lies between is not known. */
				if (other != season)
					m->unbelievable = TRUE;
				bare = FALSE;
			}
			if (*q == 'E' || *q == 'e') {
				q++;
				bare = FALSE;
			}
			if (!read_number(&q, 1, 4, &value))
				return FALSE;
			/* "S04E14-2.Cooks.Remain": a number below the episode after
			 * a hyphen is not the end of a range but the first word of
			 * the episode's title. */
			if (bare && value < m->episodes[m->n_episodes - 1])
				return FALSE;
			push_range(m, m->episodes, &m->n_episodes, value);
			p = q;
		}
	}
	if (*p == 'v' || *p == 'V') {
		p++;
		if (!read_number(&p, 1, 2, &value))
			return FALSE;
		m->version = value;
	}
	return *p == '\0';
}

/* "E05" after an "S01E04": one more episode. */
static gboolean
token_more_episode(
	const gchar *t,
	gint        *value
){
	const gchar *p = t + 1;

	if (t[0] != 'E' && t[0] != 'e')
		return FALSE;
	return read_number(&p, 1, 4, value) && *p == '\0';
}

/* "1x02", "1x02x03", "1x02-03", "1x02-1x03". */
static gboolean
token_cross(
	const gchar *t,
	Marker      *m
){
	const gchar *p = t;
	gint season;
	gint value;

	if (!read_number(&p, 1, 2, &season) || (*p != 'x' && *p != 'X'))
		return FALSE;
	p++;
	if (!read_number(&p, 2, 3, &value))
		return FALSE;
	marker_reset(m);
	m->season_first = season;
	m->season_last = season;
	push_number(m, m->episodes, &m->n_episodes, value);
	for (;;) {
		if (*p == 'x' || *p == 'X') {
			p++;
			if (!read_number(&p, 2, 3, &value))
				return FALSE;
			push_number(m, m->episodes, &m->n_episodes, value);
		} else if (*p == '-') {
			const gchar *q = p + 1;
			const gchar *save = q;
			gint other;

			if (read_number(&q, 1, 2, &other) && (*q == 'x' || *q == 'X')) {
				if (other != season)
					m->unbelievable = TRUE;
				q++;
			} else {
				q = save;
			}
			if (!read_number(&q, 1, 3, &value))
				return FALSE;
			push_range(m, m->episodes, &m->n_episodes, value);
			p = q;
		} else {
			break;
		}
	}
	if (*p == 'v' || *p == 'V') {
		p++;
		if (!read_number(&p, 1, 2, &value))
			return FALSE;
		m->version = value;
	}
	return *p == '\0';
}

static gboolean
set_date(
	const Parse *p,
	Marker      *m,
	gint         year,
	gint         month,
	gint         day
){
	if (!plausible_year(p, year) || month < 1 || month > 12 || day < 1 || day > 31 ||
	    !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
		return FALSE;
	marker_reset(m);
	m->year = year;
	m->month = month;
	m->day = day;
	return TRUE;
}

/* "2024-03-15". */
static gboolean
token_date(
	const Parse *p,
	const gchar *t,
	Marker      *m
){
	const gchar *q = t;
	gint year;
	gint month;
	gint day;

	if (!read_number(&q, 4, 4, &year) || *q != '-')
		return FALSE;
	q++;
	if (!read_number(&q, 1, 2, &month) || *q != '-')
		return FALSE;
	q++;
	if (!read_number(&q, 1, 2, &day) || *q != '\0')
		return FALSE;
	return set_date(p, m, year, month, day);
}

/* "123", "123v2": a number of 1 to 4 digits; *@digits says how many. */
static gboolean
token_number(
	const gchar *t,
	gint        *value,
	gint        *version,
	guint       *digits
){
	const gchar *p = t;

	*version = 0;
	if (!read_number(&p, 1, 4, value))
		return FALSE;
	*digits = (guint)(p - t);
	if (*p == 'v' || *p == 'V') {
		p++;
		if (!read_number(&p, 1, 2, version))
			return FALSE;
	}
	return *p == '\0';
}

/* "01-12", "01~12". */
static gboolean
token_range(
	const gchar *t,
	gint        *first,
	gint        *last
){
	const gchar *p = t;

	if (!read_number(&p, 1, 4, first) || (*p != '-' && *p != '~'))
		return FALSE;
	p++;
	return read_number(&p, 1, 4, last) && *p == '\0';
}

/* "E123", "EP123", "E123v2": an absolute number. */
static gboolean
token_absolute(
	const gchar *t,
	Marker      *m
){
	const gchar *p = t;
	gint value;
	gint version = 0;

	if (*p != 'E' && *p != 'e')
		return FALSE;
	p++;
	if (*p == 'P' || *p == 'p')
		p++;
	if (!read_number(&p, 1, 4, &value))
		return FALSE;
	if (*p == 'v' || *p == 'V') {
		p++;
		if (!read_number(&p, 1, 2, &version))
			return FALSE;
	}
	if (*p != '\0')
		return FALSE;
	marker_reset(m);
	push_number(m, m->absolute, &m->n_absolute, value);
	m->version = version;
	return TRUE;
}

/* "SP01", "OVA", "OVA2", "OAD", "ONA": specials, season 0.  Returns how
 * many digits followed (-1: not a special). */
static gint
token_special(
	const gchar *t,
	Marker      *m
){
	const gchar *p;
	gint value;

	if (g_ascii_strncasecmp(t, "sp", 2) == 0 && g_ascii_isdigit(t[2])) {
		p = t + 2;
		if (!read_number(&p, 1, 3, &value) || *p != '\0')
			return -1;
		marker_reset(m);
		m->season_first = 0;
		m->season_last = 0;
		push_number(m, m->episodes, &m->n_episodes, value);
		return 1;
	}
	if (g_ascii_strncasecmp(t, "ova", 3) != 0 && g_ascii_strncasecmp(t, "oad", 3) != 0 &&
	    g_ascii_strncasecmp(t, "ona", 3) != 0)
		return -1;
	p = t + 3;
	marker_reset(m);
	m->season_first = 0;
	m->season_last = 0;
	if (*p == '\0')
		return 0;
	if (!read_number(&p, 1, 3, &value) || *p != '\0')
		return -1;
	push_number(m, m->episodes, &m->n_episodes, value);
	return 1;
}

/* Whether a single token is any episode mark (for splitting hyphenated
 * words: "S01E01-GROUP"). */
static gboolean
token_is_mark(
	const Parse *p,
	const gchar *t
){
	Marker *m = (Marker *)&p->marker;
	gint a;
	gint b;

	return token_season_episode(t, m) || token_cross(t, m) || token_date(p, t, m) ||
		token_range(t, &a, &b) || token_absolute(t, m) || token_special(t, m) >= 0;
}

/* ── Forms over tokens ──────────────────────────────────────────── */

static Form
form_of(
	const gchar *t,
	gboolean     bracketed
){
	const gchar *channels;
	const AxisReleaseRule *source;
	const AxisReleaseRule *resolution;
	gint version;

	if (channels_text(t))
		return FORM_CHANNELS;
	if (audio_channels(t, &channels) != NULL)
		return FORM_AUDIO_CHANNELS;
	if (dimensions(t) != AXIS_RELEASE_RESOLUTION_NONE)
		return FORM_DIMENSIONS;
	if (source_resolution(t, &source, &resolution))
		return FORM_SOURCE_RES;
	if (version_text(t, &version))
		return FORM_VERSION;
	if (bracketed && checksum_text(t))
		return FORM_CHECKSUM;
	return FORM_NONE;
}

/* An audio word ending in a digit, then a token of one digit: "DDP5 1". */
static gboolean
audio_split(
	const Parse *p,
	guint        i
){
	gsize length = 0;
	const gchar *t = p->tokens[i].text;
	const gchar *next;

	if (i + 1 >= p->n || axis_release_rule_audio_prefix(t, &length) == NULL)
		return FALSE;
	if (!g_ascii_isdigit(t[length]) || t[length] == '0' || t[length + 1] != '\0')
		return FALSE;
	next = p->tokens[i + 1].text;
	return g_ascii_isdigit(next[0]) && next[1] == '\0' && !(p->tokens[i + 1].flags & TOK_HYPHEN);
}

/* A digit, then a token of one digit, after a word that is only ever an
 * audio word: "AAC 5 1", "TrueHD 7 1", "Atmos 7 1". */
static gboolean
channels_split(
	const Parse *p,
	guint        i
){
	const gchar *t = p->tokens[i].text;
	const gchar *next;
	const AxisReleaseRule *before;

	if (i == 0 || i + 1 >= p->n || !g_ascii_isdigit(t[0]) || t[0] == '0' || t[1] != '\0' ||
	    (p->tokens[i - 1].flags & TOK_HYPHEN) || (p->tokens[i + 1].flags & TOK_HYPHEN))
		return FALSE;
	next = p->tokens[i + 1].text;
	if (!g_ascii_isdigit(next[0]) || next[1] != '\0')
		return FALSE;
	before = axis_release_rule_word(p->tokens[i - 1].text);
	return before != NULL && before->hard && (before->field == AXIS_RULE_AUDIO || before->field == AXIS_RULE_ATMOS);
}

static Form
form_at(
	const Parse *p,
	guint        i,
	guint       *span
){
	Form form;

	*span = 1;
	if (p->tokens[i].flags & TOK_HYPHEN)
		return FORM_NONE;
	form = form_of(p->tokens[i].text, (p->tokens[i].flags & TOK_BRACKET) != 0);
	if (form != FORM_NONE)
		return form;
	if (audio_split(p, i)) {
		*span = 2;
		return FORM_AUDIO_SPLIT;
	}
	if (channels_split(p, i)) {
		*span = 2;
		return FORM_CHANNELS_SPLIT;
	}
	return FORM_NONE;
}

static gboolean
form_hard(Form form)
{
	return form == FORM_CHANNELS || form == FORM_AUDIO_CHANNELS || form == FORM_AUDIO_SPLIT ||
		form == FORM_CHANNELS_SPLIT || form == FORM_DIMENSIONS || form == FORM_SOURCE_RES;
}

/* Whether a word, as one token, is something the tables or the forms
 * know: such a part of a hyphenated word is split off. */
static gboolean
word_known(
	const Parse *p,
	const gchar *t
){
	return axis_release_rule_word(t) != NULL || all_digits(t) || form_of(t, FALSE) != FORM_NONE ||
		token_is_mark(p, t);
}

static const AxisReleaseRule *
rule_at(
	const Parse *p,
	guint        i,
	guint       *span
){
	guint k;

	*span = 0;
	if (i >= p->n || (p->tokens[i].flags & TOK_HYPHEN))
		return NULL;
	/* A phrase never runs across a lone hyphen. */
	for (k = i; k < p->n && k < i + 4; k++) {
		if (p->tokens[k].flags & TOK_HYPHEN)
			break;
	}
	return axis_release_rule_at(p->texts, k, i, span);
}

static gboolean
hard_at(
	const Parse *p,
	guint        i
){
	guint span;
	const AxisReleaseRule *rule = rule_at(p, i, &span);

	if (rule != NULL)
		return rule->hard != 0;
	return form_hard(form_at(p, i, &span));
}

/* ── Episode marks over tokens ──────────────────────────────────── */

/* After a word form ("Season 2", "Episode 4"), a year before any hard
 * token: "Quiet.Season.2.2008" is a film, not a season. */
static gboolean
word_mark_believed(
	const Parse *p,
	guint        next
){
	return !p->year_ahead[next];
}

/* "- 123", "- 123v2", "- 01-12", "- 01 ~ 12" at the lone hyphen @i. */
static guint
absolute_after_hyphen(
	const Parse *p,
	guint        i,
	Marker      *m
){
	const gchar *next;
	gint value;
	gint version;
	gint last;
	guint digits;
	guint span;

	if (i == 0 || i + 1 >= p->n || (p->tokens[i + 1].flags & TOK_HYPHEN))
		return 0;
	/* "- 10 bit" is not episode 10. */
	if (rule_at(p, i + 1, &span) != NULL)
		return 0;
	next = p->tokens[i + 1].text;
	if (token_number(next, &value, &version, &digits)) {
		if (!p->leading_group && digits < 2)
			return 0;
		if (digits == 4 && plausible_year(p, value))
			return 0;
		marker_reset(m);
		push_number(m, m->absolute, &m->n_absolute, value);
		m->version = version;
		if (i + 3 < p->n && strcmp(p->tokens[i + 2].text, "~") == 0 &&
		    token_number(p->tokens[i + 3].text, &last, &version, &digits)) {
			push_range(m, m->absolute, &m->n_absolute, last);
			return 4;
		}
		return 2;
	}
	if (token_range(next, &value, &last)) {
		marker_reset(m);
		push_number(m, m->absolute, &m->n_absolute, value);
		push_range(m, m->absolute, &m->n_absolute, last);
		return 2;
	}
	return 0;
}

/* "Season 2", "Season 1-3", "Seasons 1-3", "Season 2 Episode 5",
 * "Season 12 Episodes 1-3", "Season 3 Complete", "Season01". */
static guint
season_words(
	const Parse *p,
	guint        i,
	Marker      *m
){
	const gchar *t = p->tokens[i].text;
	gboolean plural = is_word(t, "seasons");
	guint j = i + 1;
	gint first;
	gint last;
	gint version;
	guint digits;

	if (g_ascii_strncasecmp(t, "season", 6) == 0 && all_digits(t + 6)) {
		const gchar *q = t + 6;

		if (!read_number(&q, 1, 3, &first) || *q != '\0')
			return 0;
		marker_reset(m);
		m->season_first = first;
		m->season_last = first;
	} else if (is_word(t, "season") || plural) {
		const gchar *next;

		if (j >= p->n || (p->tokens[j].flags & TOK_HYPHEN))
			return 0;
		next = p->tokens[j].text;
		if (!plural && token_number(next, &first, &version, &digits) && version == 0 && digits <= 3) {
			marker_reset(m);
			m->season_first = first;
			m->season_last = first;
		} else if (token_range(next, &first, &last)) {
			marker_reset(m);
			m->season_first = first;
			m->season_last = last;
			if (last <= first || last - first + 1 > AXIS_RELEASE_MAX_NUMBERS)
				m->unbelievable = TRUE;
		} else {
			return 0;
		}
		j++;
	} else {
		return 0;
	}
	if (!word_mark_believed(p, j))
		return 0;
	if (j < p->n && is_word(p->tokens[j].text, "complete")) {
		m->complete = TRUE;
		j++;
	}
	if (j + 1 < p->n && (is_word(p->tokens[j].text, "episode") || is_word(p->tokens[j].text, "episodes") ||
	    is_word(p->tokens[j].text, "ep"))) {
		const gchar *next = p->tokens[j + 1].text;

		if (token_number(next, &first, &version, &digits) && digits <= 4) {
			push_number(m, m->episodes, &m->n_episodes, first);
			if (version > 0)
				m->version = version;
			j += 2;
		} else if (token_range(next, &first, &last)) {
			push_number(m, m->episodes, &m->n_episodes, first);
			push_range(m, m->episodes, &m->n_episodes, last);
			j += 2;
		}
	}
	return j - i;
}

/*
 * The episode mark that starts at token @i, read into @m.  Returns how
 * many tokens it covers, 0 when there is none.
 */
static guint
mark_at(
	const Parse *p,
	guint        i,
	Marker      *m
){
	const Token *tok = &p->tokens[i];
	const gchar *t = tok->text;
	guint span;
	gint value;
	gint last;
	gint version;
	guint digits;
	gint special;

	if (tok->flags & TOK_HYPHEN)
		return absolute_after_hyphen(p, i, m);

	/* "The Complete Series", "Complete Series", "Complete Seasons". */
	if (is_word(t, "the") && i + 2 < p->n && is_word(p->tokens[i + 1].text, "complete") &&
	    (is_word(p->tokens[i + 2].text, "series") || is_word(p->tokens[i + 2].text, "seasons"))) {
		marker_reset(m);
		m->complete = TRUE;
		return 3;
	}
	if (is_word(t, "complete") && i + 1 < p->n) {
		const gchar *next = p->tokens[i + 1].text;

		if (is_word(next, "series") || is_word(next, "seasons")) {
			marker_reset(m);
			m->complete = TRUE;
			return 2;
		}
		/* "Complete Season 2", "Complete S02": the mark, complete. */
		if (!is_word(next, "complete") && !(p->tokens[i + 1].flags & TOK_HYPHEN)) {
			span = season_words(p, i + 1, m);
			if (span == 0 && token_season_episode(next, m) && m->n_episodes == 0)
				span = 1;
			if (span > 0 && m->n_episodes == 0) {
				m->complete = TRUE;
				return span + 1;
			}
		}
		return 0;
	}

	if (token_season_episode(t, m)) {
		span = 1;
		/* "S01 E02", "S01.E02". */
		if (m->n_episodes == 0 && m->season_first == m->season_last && i + 1 < p->n &&
		    token_more_episode(p->tokens[i + 1].text, &value)) {
			push_number(m, m->episodes, &m->n_episodes, value);
			span = 2;
		}
		/* "S01E09.E10". */
		while (m->n_episodes > 0 && i + span < p->n && token_more_episode(p->tokens[i + span].text, &value)) {
			if (m->n_episodes > 0 && value <= m->episodes[m->n_episodes - 1])
				break;
			push_number(m, m->episodes, &m->n_episodes, value);
			span++;
		}
		/* "S02.COMPLETE". */
		if (m->n_episodes == 0 && i + span < p->n && is_word(p->tokens[i + span].text, "complete")) {
			m->complete = TRUE;
			span++;
		}
		return span;
	}
	if (token_cross(t, m))
		return 1;
	if (token_date(p, t, m))
		return 1;
	/* "2024.03.15", "2024 03 15". */
	if (i + 2 < p->n && strlen(t) == 4 && all_digits(t)) {
		const gchar *mo = p->tokens[i + 1].text;
		const gchar *da = p->tokens[i + 2].text;
		gint month;
		gint day;
		const gchar *q;

		if (strlen(mo) <= 2 && all_digits(mo) && strlen(da) <= 2 && all_digits(da) &&
		    !(p->tokens[i + 1].flags & TOK_HYPHEN) && !(p->tokens[i + 2].flags & TOK_HYPHEN)) {
			q = t;
			read_number(&q, 4, 4, &value);
			q = mo;
			read_number(&q, 1, 2, &month);
			q = da;
			read_number(&q, 1, 2, &day);
			if (set_date(p, m, value, month, day))
				return 3;
		}
	}
	span = season_words(p, i, m);
	if (span > 0)
		return span;
	if (token_absolute(t, m))
		return 1;
	/* "Episode 7", "Ep 2". */
	if ((is_word(t, "episode") || is_word(t, "ep")) && i + 1 < p->n &&
	    !(p->tokens[i + 1].flags & TOK_HYPHEN)) {
		const gchar *next = p->tokens[i + 1].text;

		if (token_number(next, &value, &version, &digits) && word_mark_believed(p, i + 2)) {
			marker_reset(m);
			push_number(m, m->absolute, &m->n_absolute, value);
			m->version = version;
			return 2;
		}
		if (token_range(next, &value, &last) && word_mark_believed(p, i + 2)) {
			marker_reset(m);
			push_number(m, m->absolute, &m->n_absolute, value);
			push_range(m, m->absolute, &m->n_absolute, last);
			return 2;
		}
	}
	special = token_special(t, m);
	if (special >= 0) {
		/* "OVA 2". */
		if (special == 0 && i + 1 < p->n && token_number(p->tokens[i + 1].text, &value, &version, &digits) &&
		    digits <= 3 && !(p->tokens[i + 1].flags & TOK_HYPHEN)) {
			push_number(m, m->episodes, &m->n_episodes, value);
			return 2;
		}
		return 1;
	}
	/* A bracketed batch: "(01-12)". */
	if ((tok->flags & TOK_BRACKET) && token_range(t, &value, &last)) {
		marker_reset(m);
		push_number(m, m->absolute, &m->n_absolute, value);
		push_range(m, m->absolute, &m->n_absolute, last);
		return 1;
	}
	return 0;
}

/* ── Tokens ─────────────────────────────────────────────────────── */

static void
push_token(
	Parse *p,
	gsize  offset,
	gsize  length,
	guint  flags
){
	Token *t;

	if (p->n >= MAX_TOKENS || length == 0 || p->used + length + 1 > sizeof(p->buffer))
		return;
	t = &p->tokens[p->n];
	memcpy(p->buffer + p->used, p->name + offset, length);
	p->buffer[p->used + length] = '\0';
	t->text = p->buffer + p->used;
	t->offset = offset;
	t->length = length;
	t->flags = flags;
	p->texts[p->n] = t->text;
	p->used += length + 1;
	p->n++;
}

/* Whether the @length bytes at @t are a known word. */
static gboolean
known_text(
	const Parse *p,
	const gchar *t,
	gsize        length
){
	gchar word[AXIS_RELEASE_NAME_MAX + 1];

	if (length == 0 || length > AXIS_RELEASE_NAME_MAX)
		return FALSE;
	memcpy(word, t, length);
	word[length] = '\0';
	return word_known(p, word);
}

static gboolean
span_known(
	const Parse *p,
	gsize        offset,
	gsize        length
){
	return known_text(p, p->name + offset, length);
}

/*
 * One raw token (no separator in it).  Hyphens at its ends are dropped
 * (a leading one glues it to the token before); a hyphenated word is
 * split where a part, or two or three parts together ("DTS-HD",
 * "E-AC-3"), is a known word, and kept whole otherwise.
 */
static void
add_raw(
	Parse    *p,
	gsize     start,
	gsize     end,
	gboolean  bracketed
){
	const gchar *s = p->name;
	guint base = bracketed ? TOK_BRACKET : 0;
	guint first_flags;
	gsize a = start;
	gsize b = end;
	gsize part_start[MAX_PARTS];
	gsize part_end[MAX_PARTS];
	guint unit_parts[MAX_PARTS];
	gboolean unit_known[MAX_PARTS];
	guint parts = 0;
	guint units = 0;
	gboolean any_known = FALSE;
	guint k;
	gsize i;

	while (a < b && s[a] == '-')
		a++;
	while (b > a && s[b - 1] == '-')
		b--;
	if (a == b) {
		push_token(p, start, end - start, base | TOK_HYPHEN);
		return;
	}
	first_flags = base | (a > start ? TOK_AFTER_HYPHEN : 0);
	if (memchr(s + a, '-', b - a) == NULL || span_known(p, a, b - a)) {
		push_token(p, a, b - a, first_flags);
		return;
	}

	/* The parts between hyphens. */
	i = a;
	while (i < b && parts < MAX_PARTS) {
		gsize j = i;

		while (j < b && s[j] != '-')
			j++;
		if (j > i) {
			part_start[parts] = i;
			part_end[parts] = j;
			parts++;
		}
		i = j + 1;
	}

	/* Units: known compounds of three or two parts, known parts, and the
	 * other parts one by one. */
	k = 0;
	while (k < parts) {
		guint take = 1;
		gboolean known = FALSE;

		if (k + 2 < parts && span_known(p, part_start[k], part_end[k + 2] - part_start[k])) {
			take = 3;
			known = TRUE;
		} else if (k + 1 < parts && span_known(p, part_start[k], part_end[k + 1] - part_start[k])) {
			take = 2;
			known = TRUE;
		} else if (span_known(p, part_start[k], part_end[k] - part_start[k])) {
			known = TRUE;
		}
		unit_parts[units] = take;
		unit_known[units] = known;
		any_known = any_known || known;
		units++;
		k += take;
	}
	if (!any_known) {
		push_token(p, a, b - a, first_flags);
		return;
	}

	/* Unknown units next to each other stay one word ("Newt-Man-2002"
	 * is "Newt-Man" and "2002"). */
	k = 0;
	{
		guint u = 0;
		gboolean first = TRUE;

		while (u < units) {
			guint from = k;
			guint to;

			if (unit_known[u]) {
				to = k + unit_parts[u] - 1;
				k += unit_parts[u];
				u++;
			} else {
				while (u < units && !unit_known[u]) {
					k += unit_parts[u];
					u++;
				}
				to = k - 1;
			}
			push_token(p, part_start[from], part_end[to] - part_start[from],
				first ? first_flags : (base | TOK_AFTER_HYPHEN));
			first = FALSE;
		}
	}
}

/* "5.1", "DDP5.1", "7.1.4", "H.264": a dot that does not part tokens.  A
 * digit after the dot that a letter follows starts a word of its own
 * ("K9.5x22" is "K9" and "5x22"). */
static gboolean
keep_dot(
	const gchar *s,
	gsize        start,
	gsize        j,
	gsize        end
){
	if (j > start && g_ascii_isdigit(s[j - 1]) && (j - 1 == start || !g_ascii_isdigit(s[j - 2])) &&
	    j + 1 < end && g_ascii_isdigit(s[j + 1]) && (j + 2 >= end || !g_ascii_isalnum(s[j + 2])))
		return TRUE;
	if (j == start + 1 && (s[start] == 'H' || s[start] == 'h') && j + 3 < end &&
	    s[j + 1] == '2' && s[j + 2] == '6' && (s[j + 3] == '4' || s[j + 3] == '5') &&
	    (j + 4 >= end || !g_ascii_isdigit(s[j + 4])))
		return TRUE;
	return FALSE;
}

static void
tokenise(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	gsize i = start;
	gint depth = 0;

	while (i < end) {
		gsize n = separator_at(s, i);
		gsize j;

		if (n > 0) {
			if (s[i] == '[' || s[i] == '(' || s[i] == '{')
				depth++;
			else if ((s[i] == ']' || s[i] == ')' || s[i] == '}') && depth > 0)
				depth--;
			i += n;
			continue;
		}
		j = i;
		while (j < end) {
			if (s[j] == '.' && keep_dot(s, i, j, end)) {
				j++;
				continue;
			}
			if (separator_at(s, j) > 0)
				break;
			j += (gsize)g_utf8_skip[(guchar)s[j]];
		}
		if (j > end)
			j = end;
		add_raw(p, i, j, depth > 0);
		i = j;
	}
}

/* ── The ends of the name: extension, site, group ───────────────── */

static gsize
strip_extensions(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	gboolean archive = FALSE;
	guint round;

	for (round = 0; round < 3; round++) {
		gsize dot = end;
		gsize length;
		gsize i;
		guint stop = 0;

		while (dot > start && s[dot - 1] != '.' && end - dot < 8)
			dot--;
		if (dot <= start + 1 || s[dot - 1] != '.')
			break;
		length = end - dot;
		if (length == 0)
			break;
		for (i = dot; i < end; i++) {
			if (!g_ascii_isalnum(s[i]))
				return end;
		}
		if (axis_release_extension(s + dot, length, &stop)) {
			p->r->stop |= stop;
			archive = archive || stop == AXIS_RELEASE_STOP_ARCHIVE;
			end = dot - 1;
			continue;
		}
		/* "name.mkv.2": the number a client adds to a second copy, after an
		 * extension. */
		if (round == 0 && length <= 2 && g_ascii_isdigit(s[dot]) && (length == 1 || g_ascii_isdigit(s[dot + 1]))) {
			gsize before = dot - 1;

			while (before > start && s[before - 1] != '.' && dot - 1 - before < 8)
				before--;
			if (before > start + 1 && s[before - 1] == '.' &&
			    axis_release_extension(s + before, dot - 1 - before, &stop)) {
				end = dot - 1;
				continue;
			}
		}
		/* "name.part01.rar". */
		if (archive && length >= 5 && length <= 7 && g_ascii_strncasecmp(s + dot, "part", 4) == 0) {
			for (i = dot + 4; i < end && g_ascii_isdigit(s[i]); i++)
				;
			if (i == end) {
				end = dot - 1;
				continue;
			}
		}
		break;
	}
	return end;
}

/* Nothing but a hash: hex digits (and hyphens), or a long jumble of
 * letters of both cases and digits. */
static gboolean
obfuscated(
	const gchar *s,
	gsize        start,
	gsize        end
){
	gsize length = end - start;
	guint digits = 0;
	guint letters = 0;
	guint lower = 0;
	guint upper = 0;
	guint hyphens = 0;
	guint transitions = 0;
	gboolean hex = TRUE;
	gint previous = -1;
	gsize i;

	if (length < 16)
		return FALSE;
	for (i = start; i < end; i++) {
		gchar c = s[i];
		gint kind;

		if (c == '-') {
			hyphens++;
			continue;
		}
		if (g_ascii_isdigit(c)) {
			digits++;
			kind = 1;
		} else if (g_ascii_isalpha(c)) {
			letters++;
			if (g_ascii_islower(c))
				lower++;
			else
				upper++;
			kind = 0;
		} else {
			return FALSE;
		}
		if (!g_ascii_isxdigit(c))
			hex = FALSE;
		if (previous >= 0 && kind != previous)
			transitions++;
		previous = kind;
	}
	if (hex && digits + letters >= 16 && digits > 0 && letters > 0)
		return TRUE;
	return hyphens == 0 && length >= 20 && lower > 0 && upper > 0 && digits * 5 >= length && transitions >= 6;
}

/* Whether the text of a bracket or a group (@length bytes at @t) holds a
 * known word: then it is not a group ("[1080p]", "[WEBDL-1080p]"). */
static gboolean
holds_known_word(
	const Parse *p,
	const gchar *t,
	gsize        length
){
	gsize i = 0;

	if (known_text(p, t, length))
		return TRUE;
	while (i < length) {
		gsize j = i;

		while (j < length && t[j] != '-' && t[j] != '.' && t[j] != ' ')
			j++;
		if (j > i && known_text(p, t + i, j - i))
			return TRUE;
		/* Two parts together: "DTS-HD". */
		if (j < length && t[j] == '-') {
			gsize k = j + 1;

			while (k < length && t[k] != '-' && t[k] != '.' && t[k] != ' ')
				k++;
			if (k > j + 1 && known_text(p, t + i, k - i))
				return TRUE;
		}
		i = j + 1;
	}
	return FALSE;
}

/* What a group may be: a short word with a letter, no separator, nothing
 * the tables know, not a number, a year or a checksum. */
static gboolean
group_text_ok(
	const Parse *p,
	const gchar *t,
	gsize        length,
	gboolean     spaces
){
	gsize i;
	gboolean letter = FALSE;
	gchar word[GROUP_MAX + 1];

	if (length == 0 || length > GROUP_MAX)
		return FALSE;
	for (i = 0; i < length;) {
		gsize n = separator_at(t, i);

		if (n > 0 && !(t[i] == '.' || (spaces && t[i] == ' ')))
			return FALSE;
		if (g_ascii_isalpha(t[i]) || (guchar)t[i] >= 0x80)
			letter = TRUE;
		i += (guchar)t[i] >= 0x80 ? (gsize)g_utf8_skip[(guchar)t[i]] : 1;
	}
	if (!letter)
		return FALSE;
	memcpy(word, t, length);
	word[length] = '\0';
	if (checksum_text(word) || year_token(p, word) || !has_letter(word))
		return FALSE;
	return !holds_known_word(p, t, length);
}

static void
set_group(
	Parse       *p,
	const gchar *t,
	gsize        length
){
	g_free(p->r->group);
	p->r->group = g_strndup(t, length);
}

/* Something a site's address looks like: "www.site.tld", "site.tld". */
static gboolean
site_like(
	const gchar *t,
	gsize        length
){
	gsize last_dot = length;
	gsize i;

	if (length < 4 || length > 64)
		return FALSE;
	for (i = 0; i < length; i++) {
		if (t[i] == '.')
			last_dot = i;
		else if (!g_ascii_isalnum(t[i]) && t[i] != '-')
			return FALSE;
	}
	if (g_ascii_strncasecmp(t, "www.", 4) == 0)
		return TRUE;
	return last_dot < length && last_dot > 0 && axis_release_tld(t + last_dot + 1, length - last_dot - 1);
}

static gsize
skip_after_site(
	const gchar *s,
	gsize        i,
	gsize        end
){
	while (i < end && (s[i] == ' ' || s[i] == '-' || s[i] == '_' || s[i] == '.'))
		i++;
	return i;
}

/* Trims spaces off [*a, *b). */
static void
trim(
	const gchar *s,
	gsize       *a,
	gsize       *b
){
	while (*a < *b && s[*a] == ' ')
		(*a)++;
	while (*b > *a && s[*b - 1] == ' ')
		(*b)--;
}

/* A leading "www.site.tld - ", "[www.site.tld]" or "[site.tld]", and a
 * leading anime-style "[Group]". */
static gsize
take_front(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	guint round;

	if (end - start > 4 && g_ascii_strncasecmp(s + start, "www.", 4) == 0) {
		gsize i = start + 4;
		guint labels = 0;

		while (labels < 5) {
			gsize j = i;

			while (j < end && (g_ascii_isalnum(s[j]) || s[j] == '-'))
				j++;
			if (j == i)
				break;
			labels++;
			if (labels >= 2 && axis_release_tld(s + i, j - i) && (j == end || s[j] == ' ' || s[j] == '.' ||
			    s[j] == '-' || s[j] == '_' || s[j] == '[')) {
				start = skip_after_site(s, j, end);
				break;
			}
			if (j >= end || s[j] != '.')
				break;
			i = j + 1;
		}
	}
	for (round = 0; round < 2 && start < end && s[start] == '['; round++) {
		gsize close = start + 1;
		gsize a;
		gsize b;

		while (close < end && s[close] != ']' && s[close] != '[')
			close++;
		if (close >= end || s[close] != ']')
			break;
		a = start + 1;
		b = close;
		trim(s, &a, &b);
		if (site_like(s + a, b - a)) {
			start = skip_after_site(s, close + 1, end);
			continue;
		}
		if (p->r->group == NULL && group_text_ok(p, s + a, b - a, TRUE)) {
			set_group(p, s + a, b - a);
			p->leading_group = TRUE;
			start = close + 1;
		}
		break;
	}
	return start;
}

/* A tracker's, index's or collective's tag (axis_release_site_tag()),
 * alone or as its site's address ("EZTVx.to"). */
static gboolean
tag_text(
	const gchar *t,
	gsize        length
){
	gsize dot = 0;

	if (axis_release_site_tag(t, length))
		return TRUE;
	while (dot < length && t[dot] != '.')
		dot++;
	return dot < length && site_like(t, length) && axis_release_site_tag(t, dot);
}

/* Whether the @length bytes at @t hold a letter or a digit of any script. */
static gboolean
has_alnum(
	const gchar *t,
	gsize        length
){
	const gchar *p;

	for (p = t; p < t + length; p = g_utf8_next_char(p)) {
		if (g_unichar_isalnum(g_utf8_get_char(p)))
			return TRUE;
	}
	return FALSE;
}

/*
 * Takes off the tags written after a name's end: "[TGx]", " TGx",
 * "[eztv]", "[EZTVx.to]", "[QxR]", ") UTR", a last word with no letter or
 * digit in it (" ⭐", " |"), and an extension written as a word after a
 * bracket ("] mkv").  What is left ends where the group is looked for.
 */
static gsize
strip_trailing_tags(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	guint round;

	for (round = 0; round < 3; round++) {
		gsize e = end;
		gsize a;
		gsize b;

		while (e > start && s[e - 1] == ' ')
			e--;
		if (e == start)
			return end;
		if (s[e - 1] == ']' || s[e - 1] == ')') {
			gchar opener = s[e - 1] == ']' ? '[' : '(';
			gsize open = e - 1;

			while (open > start && s[open - 1] != opener && s[open - 1] != '[' && s[open - 1] != ']' &&
			       s[open - 1] != '(' && s[open - 1] != ')')
				open--;
			if (open <= start + 1 || s[open - 1] != opener)
				return end;
			a = open;
			b = e - 1;
			trim(s, &a, &b);
			if (b == a || !tag_text(s + a, b - a))
				return end;
			end = open - 1;
			continue;
		}
		a = e;
		while (a > start && s[a - 1] != ' ')
			a--;
		if (a == start)
			return end;
		if (!tag_text(s + a, e - a) && has_alnum(s + a, e - a)) {
			/* "[1080p x265 Joy] mkv": an extension written as a word
			 * after a bracket. */
			gsize before = a;
			guint stop;

			while (before > start && s[before - 1] == ' ')
				before--;
			if (before == start || (s[before - 1] != ')' && s[before - 1] != ']') ||
			    !axis_release_extension(s + a, e - a, &stop) || stop != 0)
				return end;
		}
		end = a;
	}
	return end;
}

/* The word just before @open (back to a space or a bracket) ends in a
 * "-GROUP". */
static gboolean
hyphen_group_before(
	const Parse *p,
	gsize        start,
	gsize        open
){
	const gchar *s = p->name;
	gsize e = open;
	gsize b;
	gsize h;

	while (e > start && s[e - 1] == ' ')
		e--;
	b = e;
	while (b > start && s[b - 1] != ' ' && s[b - 1] != ']' && s[b - 1] != ')' && s[b - 1] != '[' &&
	       s[b - 1] != '(' && s[b - 1] != '_')
		b--;
	h = e;
	while (h > b && s[h - 1] != '-')
		h--;
	if (h <= b || h >= e)
		return FALSE;
	if (memchr(s + h, '.', e - h) != NULL)
		return FALSE;
	return group_text_ok(p, s + h, e - h, FALSE);
}

/* A trailing "[GROUP]" (YTS style), unless a "-GROUP" comes just before it
 * or the name began with an anime-style group. */
static gsize
take_bracket_group(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	gsize e = end;
	gsize open;
	gsize a;
	gsize b;

	if (p->r->group != NULL)
		return end;
	while (e > start && s[e - 1] == ' ')
		e--;
	if (e == start || s[e - 1] != ']')
		return end;
	open = e - 1;
	while (open > start && s[open - 1] != '[' && s[open - 1] != ']')
		open--;
	if (open == start || s[open - 1] != '[')
		return end;
	open--;
	a = open + 1;
	b = e - 1;
	trim(s, &a, &b);
	if (!group_text_ok(p, s + a, b - a, FALSE) || axis_release_site_tag(s + a, b - a))
		return end;
	if (hyphen_group_before(p, start, open))
		return end;
	set_group(p, s + a, b - a);
	return open;
}

/* Whether the @length bytes at @t, split at spaces, dots and underscores,
 * hold a word that is only ever a quality word. */
static gboolean
holds_hard_word(
	const gchar *t,
	gsize        length
){
	gsize i = 0;

	while (i < length) {
		gsize j = i;

		while (j < length && t[j] != ' ' && t[j] != '.' && t[j] != '_')
			j++;
		if (j > i && j - i <= 32) {
			gchar word[33];
			const AxisReleaseRule *rule;

			memcpy(word, t + i, j - i);
			word[j - i] = '\0';
			rule = axis_release_rule_word(word);
			if ((rule != NULL && rule->hard) || form_hard(form_of(word, FALSE)))
				return TRUE;
		}
		i = j + 1;
	}
	return FALSE;
}

/*
 * A trailing bracket of quality words whose last word is not one: that
 * word is the group, as P2P encoders write it ("(2160p BluRay x265 10bit
 * HDR Tigole)", "[1080p x265 q22 FS97 Joy]", "(1080p BluRay x265 RZeroX)"),
 * unless it is a tracker's tag or a file's extension ("[H264 mp4]").
 */
static gsize
take_quality_bracket_group(
	Parse *p,
	gsize  start,
	gsize  end
){
	const gchar *s = p->name;
	gsize e = end;
	gsize open;
	gsize w;
	gsize b;
	guint stop;
	gchar opener;

	if (p->r->group != NULL)
		return end;
	while (e > start && s[e - 1] == ' ')
		e--;
	if (e == start || (s[e - 1] != ')' && s[e - 1] != ']'))
		return end;
	opener = s[e - 1] == ')' ? '(' : '[';
	open = e - 1;
	while (open > start && s[open - 1] != opener) {
		gchar c = s[open - 1];

		if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
			return end;
		open--;
	}
	if (open == start)
		return end;
	open--;
	b = e - 1;
	while (b > open + 1 && s[b - 1] == ' ')
		b--;
	w = b;
	while (w > open + 1 && s[w - 1] != ' ' && s[w - 1] != '.' && s[w - 1] != '_')
		w--;
	if (w <= open + 1 || w == b)
		return end;
	if (!group_text_ok(p, s + w, b - w, FALSE) || axis_release_site_tag(s + w, b - w) ||
	    axis_release_extension(s + w, b - w, &stop) || !holds_hard_word(s + open + 1, w - open - 1))
		return end;
	set_group(p, s + w, b - w);
	return w;
}

static void
remove_token(
	Parse *p,
	guint  i
){
	if (i >= p->n)
		return;
	memmove(&p->tokens[i], &p->tokens[i + 1], (p->n - i - 1) * sizeof(Token));
	memmove(&p->texts[i], &p->texts[i + 1], (p->n - i - 1) * sizeof(const gchar *));
	p->n--;
}

static void
drop_last_token(Parse *p)
{
	if (p->n > 0)
		p->n--;
}

/* Whether before token @last (and within its own first @prefix bytes)
 * there is a hard token, an episode mark or a year: a "-GROUP" is only
 * believed after one. */
static gboolean
evidence_before(
	Parse *p,
	guint  last,
	gsize  prefix
){
	guint i;

	if (prefix > 0) {
		gchar word[AXIS_RELEASE_NAME_MAX + 1];
		const AxisReleaseRule *rule;

		memcpy(word, p->tokens[last].text, prefix);
		word[prefix] = '\0';
		rule = axis_release_rule_word(word);
		if ((rule != NULL && rule->hard) || form_hard(form_of(word, FALSE)) || token_is_mark(p, word) ||
		    (last > 0 && year_token(p, word)))
			return TRUE;
	}
	for (i = 0; i < last; i++) {
		if (p->tokens[i].flags & TOK_HYPHEN)
			continue;
		if (hard_at(p, i) || mark_at(p, i, &p->marker) > 0 || (i > 0 && year_token(p, p->tokens[i].text)))
			return TRUE;
	}
	return FALSE;
}

/* Whether token @c, after a word that ends in a "-GROUP", is something
 * written after the group that is not part of it: a site's domain
 * ("-Pahe.in", "-MkvCage.ws"), a tracker's tag ("-NAISU TGx"), a file
 * extension written as a word ("-HazMatt mkv"), or a token with no letter
 * or digit in it ("-GalaxyRG ⭐"). */
static gboolean
after_group(
	const Parse *p,
	guint        c
){
	const Token *t = &p->tokens[c];
	const Token *before;
	guint stop;
	gboolean alnum = FALSE;
	const gchar *q;

	if (c == 0 || (t->flags & (TOK_HYPHEN | TOK_BRACKET | TOK_AFTER_HYPHEN)) || strchr(t->text, '-') != NULL)
		return FALSE;
	before = &p->tokens[c - 1];
	if ((before->flags & (TOK_HYPHEN | TOK_BRACKET)) ||
	    (!(before->flags & TOK_AFTER_HYPHEN) && strchr(before->text, '-') == NULL))
		return FALSE;
	for (q = t->text; *q != '\0'; q = g_utf8_next_char(q)) {
		if (g_unichar_isalnum(g_utf8_get_char(q)))
			alnum = TRUE;
	}
	if (!alnum)
		return TRUE;
	if (t->offset > 0 && p->name[t->offset - 1] == '.' && axis_release_tld(t->text, t->length))
		return TRUE;
	return axis_release_site_tag(t->text, t->length) || axis_release_extension(t->text, t->length, &stop);
}

/*
 * A trailing "-GROUP", after taking off a reposter's suffix, "-sample" and
 * what after_group() says follows a group; a bracket after it
 * ("-GROUP[rarbg]") is left where it is.
 */
static void
take_hyphen_group(Parse *p)
{
	guint round;

	for (round = 0; round < 4 && p->n > 0; round++) {
		guint c = p->n - 1;
		Token *t;
		const gchar *h;
		const gchar *candidate;
		gsize prefix = 0;
		gboolean whole;

		while (c > 0 && (p->tokens[c].flags & TOK_BRACKET))
			c--;
		if (after_group(p, c)) {
			remove_token(p, c);
			continue;
		}
		t = &p->tokens[c];
		if (t->flags & (TOK_HYPHEN | TOK_BRACKET))
			return;
		h = strrchr(t->text, '-');
		if (h != NULL && h != t->text) {
			candidate = h + 1;
			prefix = (gsize)(h - t->text);
			whole = FALSE;
		} else if ((t->flags & TOK_AFTER_HYPHEN) && c > 0) {
			candidate = t->text;
			whole = TRUE;
		} else {
			return;
		}
		if (*candidate == '\0')
			return;
		if (axis_release_repost_suffix(candidate) || is_word(candidate, "sample")) {
			if (is_word(candidate, "sample"))
				p->r->stop |= AXIS_RELEASE_STOP_SAMPLE;
			if (whole) {
				remove_token(p, c);
			} else {
				t->length = prefix;
				t->text[prefix] = '\0';
			}
			continue;
		}
		if (!group_text_ok(p, candidate, strlen(candidate), FALSE))
			return;
		if (!whole) {
			/* "DTS-HD", "WEB-DL": the word is the whole. */
			const gchar *before = t->text + prefix;

			while (before > t->text && before[-1] != '-')
				before--;
			if (known_text(p, before, (gsize)(candidate - before) + strlen(candidate)))
				return;
		}
		if (!evidence_before(p, c, whole ? 0 : prefix))
			return;
		set_group(p, candidate, strlen(candidate));
		if (whole) {
			remove_token(p, c);
		} else {
			t->length = prefix;
			t->text[prefix] = '\0';
		}
		return;
	}
}

/* ── Fields ─────────────────────────────────────────────────────── */

static void
add_language(
	AxisRelease *r,
	const gchar *code
){
	guint i;

	if (code == NULL)
		return;
	for (i = 0; i < r->language_count; i++) {
		if (strcmp(r->languages[i], code) == 0)
			return;
	}
	if (r->language_count < AXIS_RELEASE_MAX_LANGUAGES)
		r->languages[r->language_count++] = code;
}

static void
set_resolution(
	Parse                 *p,
	AxisReleaseResolution  resolution,
	guint                  rank,
	gboolean               interlaced
){
	if (p->r->resolution == AXIS_RELEASE_RESOLUTION_NONE || rank > p->res_rank) {
		p->r->resolution = resolution;
		p->r->interlaced = interlaced;
		p->res_rank = rank;
	}
}

static void
set_channels(
	AxisRelease *r,
	const gchar *channels,
	gsize        length
){
	if (r->channels[0] != '\0' || length >= sizeof(r->channels))
		return;
	memcpy(r->channels, channels, length);
	r->channels[length] = '\0';
}

static void
apply_rule(
	Parse                 *p,
	const AxisReleaseRule *rule
){
	AxisRelease *r = p->r;

	switch (rule->field) {
	case AXIS_RULE_RESOLUTION:
		set_resolution(p, (AxisReleaseResolution)(rule->value & 0xff), rule->rank,
			(rule->value & AXIS_RULE_INTERLACED) != 0);
		break;
	case AXIS_RULE_SOURCE: {
		/* Of two words of the same precedence, one that is only ever a
		 * source wins over one that is also an ordinary word:
		 * "The.Glass.Web.1080p.BluRay" is a Blu-ray. */
		guint rank = (guint)rule->rank * 2 + (rule->hard ? 1 : 0);

		if (r->source == AXIS_RELEASE_SOURCE_NONE || rank > p->source_rank) {
			r->source = (AxisReleaseSource)rule->value;
			p->source_rank = rank;
		}
		break;
	}
	case AXIS_RULE_SERVICE:
		if (r->service == NULL)
			r->service = rule->code;
		break;
	case AXIS_RULE_CODEC:
		if (r->codec == AXIS_RELEASE_CODEC_NONE)
			r->codec = (AxisReleaseCodec)rule->value;
		break;
	case AXIS_RULE_BITS:
		if (r->bit_depth == 0)
			r->bit_depth = (gint)rule->value;
		break;
	case AXIS_RULE_HDR:
		r->hdr |= rule->value;
		break;
	case AXIS_RULE_AUDIO:
		r->audio |= rule->value;
		if (rule->rank & 1)
			r->atmos = TRUE;
		break;
	case AXIS_RULE_ATMOS:
		r->atmos = TRUE;
		break;
	case AXIS_RULE_EDITION:
		r->editions |= rule->value;
		break;
	case AXIS_RULE_LANGUAGE:
		add_language(r, rule->code);
		r->language_marks |= rule->value;
		break;
	case AXIS_RULE_LANG_MARK:
		r->language_marks |= rule->value;
		break;
	case AXIS_RULE_REVISION:
		r->revision |= rule->value;
		break;
	case AXIS_RULE_MARK:
		r->marks |= rule->value;
		break;
	case AXIS_RULE_STOP:
		r->stop |= rule->value;
		break;
	case AXIS_RULE_COMPLETE:
		r->complete = TRUE;
		break;
	default:
		break;
	}
}

static void
apply_form(
	Parse *p,
	Form   form,
	guint  i
){
	AxisRelease *r = p->r;
	const gchar *t = p->tokens[i].text;
	const gchar *channels = NULL;
	const AxisReleaseRule *rule;
	gsize length = 0;
	gint version;

	switch (form) {
	case FORM_CHANNELS:
		set_channels(r, t, strlen(t));
		break;
	case FORM_AUDIO_CHANNELS:
		rule = audio_channels(t, &channels);
		if (rule == NULL || channels == NULL)
			break;
		apply_rule(p, rule);
		set_channels(r, channels, strlen(channels));
		break;
	case FORM_AUDIO_SPLIT: {
		gchar text[4];

		rule = axis_release_rule_audio_prefix(t, &length);
		if (rule == NULL)
			break;
		apply_rule(p, rule);
		text[0] = t[length];
		text[1] = '.';
		text[2] = p->tokens[i + 1].text[0];
		text[3] = '\0';
		set_channels(r, text, 3);
		break;
	}
	case FORM_CHANNELS_SPLIT: {
		gchar text[4];

		text[0] = t[0];
		text[1] = '.';
		text[2] = p->tokens[i + 1].text[0];
		text[3] = '\0';
		set_channels(r, text, 3);
		break;
	}
	case FORM_DIMENSIONS:
		set_resolution(p, dimensions(t), 2, FALSE);
		break;
	case FORM_SOURCE_RES: {
		const AxisReleaseRule *source;
		const AxisReleaseRule *resolution;

		if (source_resolution(t, &source, &resolution)) {
			apply_rule(p, source);
			apply_rule(p, resolution);
		}
		break;
	}
	case FORM_VERSION:
		if (version_text(t, &version) && r->version == 0)
			r->version = version;
		break;
	case FORM_CHECKSUM:
		if (r->checksum == NULL)
			r->checksum = g_strdup(t);
		break;
	default:
		break;
	}
}

static void
unknown(
	Parse *p,
	guint  from,
	guint  to
){
	guint i;

	for (i = from; i < to && i < p->n; i++) {
		if (!(p->tokens[i].flags & TOK_HYPHEN))
			g_ptr_array_add(p->r->unknown, g_strdup(p->tokens[i].text));
	}
}

static gboolean
has_episode_info(const AxisRelease *r)
{
	return r->season_first >= 0 || r->episode_count > 0 || r->absolute_count > 0 || r->air_year > 0;
}

/* The mark @m read from tokens @i to @i + @span: the first sets the
 * episodes; a later one adds only what does not contradict them. */
static void
apply_mark(
	Parse        *p,
	const Marker *m,
	guint         i,
	guint         span
){
	AxisRelease *r = p->r;
	gboolean season_only = m->season_first >= 0 && m->n_episodes == 0 && m->n_absolute == 0 && m->year == 0;
	gboolean absolute_only = m->season_first < 0 && m->n_episodes == 0 && m->n_absolute > 0 && m->year == 0;
	gboolean complete_only = m->season_first < 0 && m->n_episodes == 0 && m->n_absolute == 0 && m->year == 0;
	guint k;

	if (m->unbelievable) {
		unknown(p, i, i + span);
		return;
	}
	if (!has_episode_info(r)) {
		r->season_first = m->season_first;
		r->season_last = m->season_last;
		r->episode_count = m->n_episodes;
		for (k = 0; k < m->n_episodes; k++)
			r->episodes[k] = m->episodes[k];
		r->absolute_count = m->n_absolute;
		for (k = 0; k < m->n_absolute; k++)
			r->absolute[k] = m->absolute[k];
		r->air_year = m->year;
		r->air_month = m->month;
		r->air_day = m->day;
		r->complete = r->complete || m->complete;
		if (m->version > 0 && r->version == 0)
			r->version = m->version;
		return;
	}
	if (season_only && r->season_first == m->season_first && r->season_last == m->season_last) {
		r->complete = r->complete || m->complete;
		return;
	}
	if (season_only && r->season_first < 0 && r->episode_count == 0 && r->air_year == 0) {
		r->season_first = m->season_first;
		r->season_last = m->season_last;
		r->complete = r->complete || m->complete;
		return;
	}
	if (absolute_only && r->absolute_count == 0 && r->air_year == 0) {
		r->absolute_count = m->n_absolute;
		for (k = 0; k < m->n_absolute; k++)
			r->absolute[k] = m->absolute[k];
		if (m->version > 0 && r->version == 0)
			r->version = m->version;
		return;
	}
	if (complete_only && m->complete) {
		r->complete = TRUE;
		return;
	}
	unknown(p, i, i + span);
}

/* ── The title ──────────────────────────────────────────────────── */

/* A soft word that says the release is a recording: Cam, TS, TC, Screener,
 * WP. */
static gboolean
recording_rule(const AxisReleaseRule *rule)
{
	return rule != NULL && !rule->hard && rule->field == AXIS_RULE_SOURCE &&
		rule->value >= (guint)AXIS_RELEASE_SOURCE_CAM && rule->value <= (guint)AXIS_RELEASE_SOURCE_WORKPRINT;
}

static gboolean
edition_field(const AxisReleaseRule *rule)
{
	return rule != NULL && (rule->field == AXIS_RULE_EDITION || rule->field == AXIS_RULE_EDITION_WORD ||
		rule->field == AXIS_RULE_EDITION_END);
}

/* What is left of the title before @end: something, and not only an
 * article. */
static gboolean
title_left(
	const Parse *p,
	guint        end
){
	guint words = 0;
	guint last = 0;
	guint i;

	for (i = 0; i < end; i++) {
		if (!(p->tokens[i].flags & TOK_HYPHEN)) {
			words++;
			last = i;
		}
	}
	return words > 1 || (words == 1 && !axis_release_article(p->tokens[last].text));
}

/* Whether some word before @end has a lower-case letter. */
static gboolean
title_has_lower(
	const Parse *p,
	guint        end
){
	guint i;

	for (i = 0; i < end; i++) {
		if (!(p->tokens[i].flags & TOK_HYPHEN) && !all_capitals(p->tokens[i].text) &&
		    has_letter(p->tokens[i].text))
			return TRUE;
	}
	return FALSE;
}

/* An edition at the end of the title, before @end: the words it covers
 * start at the returned index (@end when there is none); *@flags gets
 * what it says. */
static guint
edition_tail(
	const Parse *p,
	guint        end,
	guint       *flags
){
	guint k = end;
	guint standalone = 0;
	guint words = 0;
	gboolean has_end = FALSE;
	guint i;

	*flags = 0;
	while (k > 0) {
		const AxisReleaseRule *rule = axis_release_rule_word(p->tokens[k - 1].text);

		if (edition_field(rule)) {
			k--;
			continue;
		}
		/* "Open Matte". */
		if (k >= 2 && is_word(p->tokens[k - 1].text, "matte") && is_word(p->tokens[k - 2].text, "open")) {
			k -= 2;
			continue;
		}
		break;
	}
	if (k == end)
		return end;
	/* Without Cut, Edition or Version, only the words that are editions by
	 * themselves can go: start after the last of the others. */
	for (i = k; i < end; i++) {
		const AxisReleaseRule *rule = axis_release_rule_word(p->tokens[i].text);

		if (rule != NULL && rule->field == AXIS_RULE_EDITION_END)
			has_end = TRUE;
	}
	if (!has_end) {
		for (i = end; i > k; i--) {
			const AxisReleaseRule *rule = axis_release_rule_word(p->tokens[i - 1].text);

			if (rule != NULL && rule->field == AXIS_RULE_EDITION_WORD)
				break;
		}
		k = i;
	}
	/* "70th Anniversary Edition": the ordinal is the edition's. */
	if (k > 0 && k < end && ordinal_text(p->tokens[k - 1].text)) {
		const AxisReleaseRule *rule = axis_release_rule_word(p->tokens[k].text);

		if (rule != NULL && rule->value == AXIS_RELEASE_EDITION_ANNIVERSARY && edition_field(rule))
			k--;
	}
	for (i = k; i < end; i++) {
		const AxisReleaseRule *rule = axis_release_rule_word(p->tokens[i].text);

		if (rule == NULL) {
			if (is_word(p->tokens[i].text, "open"))
				standalone |= AXIS_RELEASE_EDITION_OPEN_MATTE;
			continue;
		}
		if (rule->field == AXIS_RULE_EDITION)
			standalone |= rule->value;
		else if (rule->field == AXIS_RULE_EDITION_WORD)
			words |= rule->value;
	}
	*flags = standalone | (has_end ? words : 0);
	if (*flags == 0)
		return end;
	/* "Loud, Fast and Uncut": a tag never follows "and". */
	if (k >= 1 && (is_word(p->tokens[k - 1].text, "and") || strcmp(p->tokens[k - 1].text, "&") == 0)) {
		*flags = 0;
		return end;
	}
	if (k >= 1 && is_word(p->tokens[k - 1].text, "the"))
		k--;
	return k;
}

/*
 * The soft fields a title may end with, when written as scene tags: the
 * marks names put before the year (languages, revisions, LIMITED,
 * COMPLETE), and a recording mark (CAM, TS), which must never be lost.
 * Services, audio and HDR words never come before the year, so a title
 * that ends in one keeps it ("Rapture.NOW").
 */
static gboolean
tail_tag_field(guint field)
{
	return field == AXIS_RULE_LANGUAGE || field == AXIS_RULE_LANG_MARK || field == AXIS_RULE_REVISION ||
		field == AXIS_RULE_MARK || field == AXIS_RULE_COMPLETE || field == AXIS_RULE_SOURCE;
}

/* Takes edition words and scene tags off the end of the title, reading
 * them as fields; returns where the title now ends. */
static guint
strip_title_tail(
	Parse *p,
	guint  end
){
	for (;;) {
		guint last = end;
		guint flags;
		guint k;
		guint span;
		const AxisReleaseRule *rule;

		while (last > 0 && (p->tokens[last - 1].flags & TOK_HYPHEN))
			last--;
		if (last == 0)
			return end;
		k = edition_tail(p, last, &flags);
		if (k < last && title_left(p, k)) {
			p->r->editions |= flags;
			end = k;
			continue;
		}
		rule = axis_release_rule_at(p->texts, last, last - 1, &span);
		if (rule != NULL && span == 1 && !rule->hard && tail_tag_field(rule->field) &&
		    scene_cased(p->tokens[last - 1].text) && title_left(p, last - 1) && title_has_lower(p, last - 1)) {
			apply_rule(p, rule);
			end = last - 1;
			continue;
		}
		return end;
	}
}

static gchar *
title_text(
	const Parse *p,
	guint        end
){
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < end; i++) {
		if (p->tokens[i].flags & TOK_HYPHEN)
			continue;
		if (out->len > 0)
			g_string_append_c(out, ' ');
		g_string_append(out, p->tokens[i].text);
	}
	if (out->len == 0) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}

/* ── The tail ───────────────────────────────────────────────────── */

/* An edition run from @k in the tail: all its words when it has Cut,
 * Edition or Version, else only the editions by themselves. */
static guint
read_edition(
	Parse    *p,
	guint     k,
	gboolean  counts
){
	guint j = k;
	guint standalone = 0;
	guint words = 0;
	gboolean has_end = FALSE;
	guint i;

	while (j < p->n) {
		guint span;
		const AxisReleaseRule *rule = rule_at(p, j, &span);

		if (!edition_field(rule))
			break;
		if (rule->field == AXIS_RULE_EDITION)
			standalone |= rule->value;
		else if (rule->field == AXIS_RULE_EDITION_WORD)
			words |= rule->value;
		else
			has_end = TRUE;
		j += span;
	}
	if (!counts) {
		unknown(p, k, j);
		return j;
	}
	if (has_end) {
		p->r->editions |= standalone | words;
		if ((standalone | words) == 0)
			p->r->editions |= AXIS_RELEASE_EDITION_OTHER;
		return j;
	}
	p->r->editions |= standalone;
	for (i = k; i < j;) {
		guint span;
		const AxisReleaseRule *rule = rule_at(p, i, &span);

		if (rule->field == AXIS_RULE_EDITION_WORD)
			unknown(p, i, i + span);
		i += span;
	}
	return j;
}

static gboolean
all_scene_cased(
	const Parse *p,
	guint        i,
	guint        span
){
	guint k;

	for (k = i; k < i + span && k < p->n; k++) {
		if (!scene_cased(p->tokens[k].text))
			return FALSE;
	}
	return TRUE;
}

/*
 * The start of the unbroken run of known words that leads up to the first
 * hard token after @from (or to the end): "German.DL.720p" counts
 * "German" there, where "German.Engineering.720p" does not.
 */
static guint
run_before_quality(
	Parse *p,
	guint  from
){
	guint run = from;
	guint k = from;

	while (k < p->n) {
		guint span;
		const AxisReleaseRule *rule;
		Form form;

		if (p->claimed[k]) {
			k++;
			continue;
		}
		span = mark_at(p, k, &p->marker);
		if (span > 0) {
			k += span;
			run = k;
			continue;
		}
		if (p->tokens[k].flags & TOK_HYPHEN) {
			k++;
			continue;
		}
		rule = rule_at(p, k, &span);
		if (rule != NULL) {
			if (rule->hard)
				return run;
			k += span;
			continue;
		}
		form = form_at(p, k, &span);
		if (form != FORM_NONE) {
			if (form_hard(form))
				return run;
			k += span;
			continue;
		}
		k++;
		run = k;
	}
	return run;
}

static void
read_tail(
	Parse *p,
	guint  from,
	guint  zone
){
	guint run = run_before_quality(p, from);
	guint k = from;

	while (k < p->n) {
		guint span;
		const AxisReleaseRule *rule;
		Form form;

		if (p->claimed[k]) {
			k++;
			continue;
		}
		span = mark_at(p, k, &p->marker);
		if (span > 0) {
			/*
			 * A film's name (a year before the anchor, no episode mark
			 * before the quality words) does not number episodes among
			 * its quality words: "(1080p x265 q22 S94 Joy)", "[S143]"
			 * and "Sp33dy94" are tags.
			 */
			if (zone == ZONE_QUALITY && p->r->year > 0 && !has_episode_info(p->r) && !p->r->complete)
				unknown(p, k, k + span);
			else
				apply_mark(p, &p->marker, k, span);
			if (zone != ZONE_QUALITY)
				zone = ZONE_EPISODE;
			k += span;
			continue;
		}
		if (p->tokens[k].flags & TOK_HYPHEN) {
			k++;
			continue;
		}
		rule = rule_at(p, k, &span);
		if (rule != NULL) {
			gboolean counts;

			if (rule->hard)
				zone = ZONE_QUALITY;
			/* Before the first hard word after an episode mark a soft word
			 * counts when it is written as a scene tag, or when only known
			 * words follow it up to the quality words ("German.DL.720p");
			 * a recording mark only when written as a tag ("Desk.Cam.1080p"
			 * is an episode's title). */
			counts = rule->hard || zone != ZONE_EPISODE || (k >= run && !recording_rule(rule)) ||
				all_scene_cased(p, k, span);
			if (edition_field(rule)) {
				k = read_edition(p, k, counts);
				continue;
			}
			if (counts)
				apply_rule(p, rule);
			else
				unknown(p, k, k + span);
			k += span;
			continue;
		}
		form = form_at(p, k, &span);
		if (form != FORM_NONE) {
			if (form_hard(form))
				zone = ZONE_QUALITY;
			apply_form(p, form, k);
			k += span;
			continue;
		}
		unknown(p, k, k + 1);
		k++;
	}
}

/*
 * Whether the @length bytes at @s are a Usenet post's subject for one part
 * of a multi-part upload, as some indexers list them: a part counter in
 * brackets ("[15/93]", "(101/171)") and a file's name in double quotes
 * that ends in an extension ("...part099.rar", "...vol07+08.par2").
 */
static gboolean
post_subject(
	const gchar *s,
	gsize        length
){
	gboolean counter = FALSE;
	const gchar *open;
	const gchar *close;
	const gchar *dot;
	guint stop;
	gsize i;

	for (i = 0; i < length && !counter; i++) {
		gsize j = i + 1;
		guint a = 0;
		guint b = 0;

		if (s[i] != '[' && s[i] != '(')
			continue;
		while (j < length && g_ascii_isdigit(s[j]) && a < 5) {
			j++;
			a++;
		}
		if (a == 0 || j >= length || s[j] != '/')
			continue;
		j++;
		while (j < length && g_ascii_isdigit(s[j]) && b < 5) {
			j++;
			b++;
		}
		counter = b > 0 && j < length && (s[j] == ']' || s[j] == ')');
	}
	if (!counter)
		return FALSE;
	open = memchr(s, '"', length);
	if (open == NULL)
		return FALSE;
	close = memchr(open + 1, '"', length - (gsize)(open + 1 - s));
	if (close == NULL)
		return FALSE;
	dot = close;
	while (dot > open + 1 && dot[-1] != '.' && close - dot < 8)
		dot--;
	return dot > open + 2 && dot[-1] == '.' && axis_release_extension(dot, (gsize)(close - dot), &stop);
}

/* ── Reading ────────────────────────────────────────────────────── */

/*
 * Whether @s[@start, @end) is a folder's name as libraries write it:
 * "<something> (<yyyy>)", a space before the bracket, nothing after it, no
 * other bracket, and something with a letter or a digit in it ("412
 * (2011)", "4K (2015)").  Whether the four digits are a year believed is
 * the caller's to check.
 */
static gboolean
folder_name_form(
	const gchar *s,
	gsize        start,
	gsize        end
){
	gboolean any = FALSE;
	gsize i;

	if (end < start + 8 || s[end - 1] != ')' || s[end - 6] != '(' || s[end - 7] != ' ')
		return FALSE;
	for (i = end - 5; i < end - 1; i++) {
		if (!g_ascii_isdigit(s[i]))
			return FALSE;
	}
	for (i = start; i < end - 7; i++) {
		gchar c = s[i];

		if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
			return FALSE;
		if (g_ascii_isalnum(c) || (guchar)c >= 0x80)
			any = TRUE;
	}
	return any;
}

/* The anchor at or after @from: the first episode mark or hard token (see
 * read_name()), *@mark saying it is a mark; p->n for none. */
static guint
anchor_from(
	Parse    *p,
	guint     from,
	gboolean *mark
){
	guint i;

	*mark = FALSE;
	for (i = from; i < p->n; i++) {
		guint span = mark_at(p, i, &p->marker);

		if (span > 0 && !(i == 0 && p->year_ahead[span])) {
			*mark = TRUE;
			return i;
		}
		if (i > 0 && form_at(p, i, &span) == FORM_CHANNELS && rule_at(p, i, &span) == NULL &&
		    p->year_ahead[i + 1])
			continue;
		if (hard_at(p, i))
			return i;
	}
	return p->n;
}

/*
 * Whether the hard word at @i is a word of the title: a word of letters,
 * outside brackets, with a year ahead of it before any other hard token,
 * that the title would be nothing without (nothing, or only an article,
 * before it: "Atmos (2018)", "Hdr Ghosts (2019)", "The Remux Brothers
 * (2010) Remux-1080p"), or that is written as a title word (not in
 * capitals) in a name that is a folder's "Title (Year)": the year in
 * brackets and nothing after it ("Dark Remux Harbour (2011)").  In a
 * release's name with quality after the year, a quality word before it
 * stays quality.  (Edition and language words are not hard: those before
 * the year are read as before, by strip_title_tail().)
 */
static gboolean
title_word(
	const Parse *p,
	guint        i
){
	const gchar *t;
	const gchar *c;
	guint j;

	if (i >= p->n || (p->tokens[i].flags & (TOK_BRACKET | TOK_HYPHEN)) || !p->year_ahead[i + 1])
		return FALSE;
	t = p->tokens[i].text;
	for (c = t; *c != '\0'; c++) {
		if (g_ascii_isdigit(*c))
			return FALSE;
	}
	if (!has_letter(t))
		return FALSE;
	if (!title_left(p, i))
		return TRUE;
	if (all_capitals(t))
		return FALSE;
	for (j = i + 1; j < p->n && !year_token(p, p->tokens[j].text); j++)
		;
	return j + 1 == p->n && (p->tokens[j].flags & TOK_BRACKET) != 0;
}

static void
read_name(Parse *p)
{
	AxisRelease *r = p->r;
	gsize start = 0;
	gsize end = p->length;
	guint anchor;
	gint year_at = -1;
	guint title_end;
	guint i;
	gboolean anchor_mark = FALSE;
	gboolean folder = FALSE;

	if (post_subject(p->name, p->length))
		r->stop |= AXIS_RELEASE_STOP_SUBJECT;
	end = strip_extensions(p, start, end);
	while (start < end && separator_at(p->name, start) > 0 && p->name[start] != '[')
		start += separator_at(p->name, start);
	if (obfuscated(p->name, start, end)) {
		r->stop |= AXIS_RELEASE_STOP_OBFUSCATED;
		return;
	}
	start = take_front(p, start, end);
	end = strip_trailing_tags(p, start, end);
	end = take_bracket_group(p, start, end);
	end = take_quality_bracket_group(p, start, end);
	folder = folder_name_form(p->name, start, end);

	tokenise(p, start, end);

	/* A trailing "sample" word, then the group. */
	if (p->n > 1 && is_word(p->tokens[p->n - 1].text, "sample") &&
	    !(p->tokens[p->n - 1].flags & TOK_AFTER_HYPHEN)) {
		r->stop |= AXIS_RELEASE_STOP_SAMPLE;
		drop_last_token(p);
	}
	if (r->group == NULL)
		take_hyphen_group(p);

	/* Where a year lies ahead before any hard token (for "Season 2"). */
	p->year_ahead[p->n] = FALSE;
	for (i = p->n; i > 0; i--) {
		if (hard_at(p, i - 1))
			p->year_ahead[i - 1] = FALSE;
		else if (year_token(p, p->tokens[i - 1].text))
			p->year_ahead[i - 1] = TRUE;
		else
			p->year_ahead[i - 1] = p->year_ahead[i];
	}

	/* The anchor: the first episode mark or hard token.  A mark that is the
	 * first token, with a year ahead of it, is a title ("12x12.2018"), and
	 * so is a number with a dot that has a year ahead of it ("Robo Pal 2.0
	 * 2025", "The Night Line 1.2.3 2009": channels are never written
	 * before the year).  A hard word that is a word of the title
	 * (title_word()) is passed over. */
	anchor = anchor_from(p, 0, &anchor_mark);
	while (!anchor_mark && title_word(p, anchor))
		anchor = anchor_from(p, anchor + 1, &anchor_mark);

	/* A folder's "<something> (<year>)" says nothing but a title and its
	 * year: a first word that is a quality word ("4K (2015)", "x264
	 * (2015)", "4K Ghosts (2015)") is the title's, not an anchor.  Only then
	 * is the reading changed: an edition or a tag after a title is still
	 * read as before ("Copper Sky Extended (2012)"). */
	r->folder_form = folder && p->n >= 2 && (p->tokens[p->n - 1].flags & TOK_BRACKET) != 0 &&
		year_token(p, p->tokens[p->n - 1].text);
	if (r->folder_form && anchor == 0) {
		anchor = p->n;
		anchor_mark = FALSE;
	}

	/* The year: the last one before the anchor that is not the first token. */
	for (i = 1; i < anchor; i++) {
		if (year_token(p, p->tokens[i].text))
			year_at = (gint)i;
	}
	if (year_at > 0) {
		const gchar *q = p->tokens[year_at].text;
		gint value;

		read_number(&q, 4, 4, &value);
		r->year = value;
		p->claimed[year_at] = TRUE;
		title_end = (guint)year_at;
	} else {
		title_end = anchor;
	}

	title_end = strip_title_tail(p, title_end);
	r->title = title_text(p, title_end);

	if (year_at > 0)
		read_tail(p, (guint)year_at + 1, ZONE_FILM);
	else
		read_tail(p, anchor, anchor_mark ? ZONE_EPISODE : ZONE_QUALITY);
}

AxisRelease *
axis_release_parse_full(
	const gchar *name,
	gssize       length,
	gint         latest_year
){
	AxisRelease *r = g_new0(AxisRelease, 1);
	Parse *p;
	gsize len;

	r->season_first = -1;
	r->season_last = -1;
	r->unknown = g_ptr_array_new_with_free_func(g_free);
	if (name == NULL)
		return r;
	if (length < 0) {
		len = strlen(name);
	} else {
		len = (gsize)length;
		if (len > 0 && memchr(name, '\0', MIN(len, (gsize)AXIS_RELEASE_NAME_MAX + 1)) != NULL)
			return r;
	}
	if (len == 0 || len > AXIS_RELEASE_NAME_MAX || !g_utf8_validate(name, (gssize)len, NULL))
		return r;

	p = g_new0(Parse, 1);
	p->name = g_strndup(name, len);
	p->length = len;
	p->latest_year = latest_year;
	p->r = r;
	read_name(p);
	g_free(p->name);
	g_free(p);
	return r;
}

AxisRelease *
axis_release_parse(const gchar *name)
{
	GDateTime *now = g_date_time_new_now_local();
	gint year = now != NULL ? g_date_time_get_year(now) + 1 : 2100;

	if (now != NULL)
		g_date_time_unref(now);
	return axis_release_parse_full(name, -1, year);
}
