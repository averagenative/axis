/*
 * axis-def-filter.c - filters, dates, sizes and counts for indexer
 * definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/def/axis-def-filter.h"

#include <string.h>

#include "indexer/def/axis-def-html.h"

/* Dates before 1970 and after 2100 are not believed. */
#define DATE_MIN_S G_GINT64_CONSTANT(0)
#define DATE_MAX_S G_GINT64_CONSTANT(4102444800)

/* The most a "5 hours ago" can count back: a hundred years. */
#define AGO_MAX_S (G_GINT64_CONSTANT(100) * 366 * 86400)

static const gchar *const filter_names[] = {
	"replace", "re_replace", "regexp", "split", "trim", "append", "prepend", "tolower", "toupper", "urldecode",
	"urlencode", "querystring", "dateparse", "timeparse", "timeago", "reltime", "fuzzytime", "validfilename",
	"htmldecode", "htmlencode", "diacritics", "base64decode", "base64encode", "strdump", NULL
};

gboolean
axis_def_filter_known(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && filter_names[i] != NULL; i++) {
		if (strcmp(filter_names[i], name) == 0)
			return TRUE;
	}
	return FALSE;
}

static guint
n_args(const gchar *const *args)
{
	guint n = 0;

	while (args != NULL && args[n] != NULL)
		n++;
	return n;
}

static void
set_problem(
	gchar       **problem,
	const gchar  *text
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup(text);
}

/* @text made valid UTF-8 without NUL, and cut to the limit. */
static gchar *
finish(gchar *text)
{
	gchar *valid;
	gsize n;

	if (text == NULL)
		return NULL;
	n = strlen(text);
	if (!g_utf8_validate(text, (gssize)n, NULL)) {
		valid = g_utf8_make_valid(text, (gssize)n);
		g_free(text);
		text = valid;
		n = strlen(text);
	}
	if (n > AXIS_DEF_FILTER_MAX_TEXT) {
		const gchar *end = text + AXIS_DEF_FILTER_MAX_TEXT;

		while (end > text && ((guchar)*end & 0xc0) == 0x80)
			end--;
		text[end - text] = '\0';
	}
	return text;
}

/* ── URLs ───────────────────────────────────────────────────────── */

gchar *
axis_def_url_encode(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const guchar *p;

	for (p = (const guchar *)(text != NULL ? text : ""); *p != '\0'; p++) {
		if (g_ascii_isalnum(*p) || strchr("-_.!*()", *p) != NULL)
			g_string_append_c(out, (gchar)*p);
		else if (*p == ' ')
			g_string_append_c(out, '+');
		else
			g_string_append_printf(out, "%%%02X", *p);
	}
	return g_string_free(out, FALSE);
}

gchar *
axis_def_url_decode(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text != NULL ? text : ""; *p != '\0'; p++) {
		if (*p == '+') {
			g_string_append_c(out, ' ');
		} else if (*p == '%' && g_ascii_isxdigit(p[1]) && g_ascii_isxdigit(p[2])) {
			gchar c = (gchar)(g_ascii_xdigit_value(p[1]) * 16 + g_ascii_xdigit_value(p[2]));

			if (c != '\0')
				g_string_append_c(out, c);
			p += 2;
		} else {
			g_string_append_c(out, *p);
		}
	}
	return finish(g_string_free(out, FALSE));
}

/* The value of query parameter @name in the address @url, decoded; "" when
 * it has none. */
static gchar *
query_value(
	const gchar *url,
	const gchar *name
){
	const gchar *query = strchr(url, '?');
	g_auto(GStrv) pairs = NULL;
	guint i;

	if (query == NULL)
		return g_strdup("");
	{
		g_autofree gchar *copy = g_strdup(query + 1);
		gchar *hash = strchr(copy, '#');

		if (hash != NULL)
			*hash = '\0';
		pairs = g_strsplit(copy, "&", 256);
	}
	for (i = 0; pairs[i] != NULL; i++) {
		gchar *eq = strchr(pairs[i], '=');
		g_autofree gchar *raw = eq != NULL ? g_strndup(pairs[i], (gsize)(eq - pairs[i])) : g_strdup(pairs[i]);
		g_autofree gchar *key = axis_def_url_decode(raw);

		if (strcmp(key, name) == 0)
			return axis_def_url_decode(eq != NULL ? eq + 1 : "");
	}
	return g_strdup("");
}

/* ── Names and words ────────────────────────────────────────────── */

static const gchar *const months[] = {
	"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november",
	"december", NULL
};

/* The month (1 to 12) a word at @s names, in full or as its first three
 * letters (and "sept"); the letters it took in @took. */
static gint
month_of(
	const gchar *s,
	gsize       *took
){
	gsize n = 0;
	guint i;

	while (g_ascii_isalpha(s[n]))
		n++;
	*took = n;
	if (n < 3)
		return 0;
	for (i = 0; months[i] != NULL; i++) {
		if (n == strlen(months[i]) && g_ascii_strncasecmp(s, months[i], n) == 0)
			return (gint)i + 1;
		if (n == 3 && g_ascii_strncasecmp(s, months[i], 3) == 0)
			return (gint)i + 1;
	}
	if (n == 4 && g_ascii_strncasecmp(s, "sept", 4) == 0)
		return 9;
	return 0;
}

/* ── Dates ──────────────────────────────────────────────────────── */

typedef struct
{
	gint     year;	/* 0: not read */
	gint     month;
	gint     day;
	gint     hour;
	gint     minute;
	gint     second;
	gboolean pm_set;
	gboolean pm;
	gboolean twelve;	/* the hour was 12-hour */
	gboolean zone_set;
	gint     offset_s;	/* east of UTC */
	gboolean time_set;
} Parts;

/* Up to @max digits at @s (at least @min): their value (of the first nine
 * digits: the fractions of a second can be more), -1 for none. */
static gint
read_digits(
	const gchar **s,
	guint         min,
	guint         max
){
	guint n = 0;
	gint value = 0;

	while (n < max && g_ascii_isdigit((*s)[n])) {
		if (n < 9)
			value = value * 10 + ((*s)[n] - '0');
		n++;
	}
	if (n < min)
		return -1;
	*s += n;
	return value;
}

/* A zone at @s: "+03:00", "+0300", "+03", "Z", "GMT", "UTC". */
static gboolean
read_zone(
	const gchar **s,
	gint         *offset_s
){
	const gchar *p = *s;
	gint sign;
	gint hours;
	gint minutes = 0;

	if (*p == 'Z' || *p == 'z') {
		*offset_s = 0;
		*s = p + 1;
		return TRUE;
	}
	if (g_ascii_strncasecmp(p, "GMT", 3) == 0 || g_ascii_strncasecmp(p, "UTC", 3) == 0) {
		p += 3;
		if (*p != '+' && *p != '-') {
			*offset_s = 0;
			*s = p;
			return TRUE;
		}
	}
	if (*p != '+' && *p != '-')
		return FALSE;
	sign = *p == '-' ? -1 : 1;
	p++;
	hours = read_digits(&p, 1, 2);
	if (hours < 0 || hours > 14)
		return FALSE;
	if (*p == ':')
		p++;
	if (g_ascii_isdigit(*p)) {
		minutes = read_digits(&p, 2, 2);
		if (minutes < 0 || minutes > 59)
			return FALSE;
	}
	*offset_s = sign * (hours * 3600 + minutes * 60);
	*s = p;
	return TRUE;
}

/* The parts as microseconds since 1970, or 0; a missing year is this
 * year, a missing date today. */
static gint64
parts_time(
	Parts  *parts,
	gint64  now_us
){
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_utc(now_us / G_USEC_PER_SEC);
	g_autoptr(GDateTime) when = NULL;
	gint64 seconds;

	if (now == NULL)
		return 0;
	if (parts->twelve) {
		if (parts->hour < 1 || parts->hour > 12)
			return 0;
		if (parts->pm_set)
			parts->hour = parts->hour % 12 + (parts->pm ? 12 : 0);
	} else if (parts->pm_set && parts->pm && parts->hour < 12) {
		parts->hour += 12;
	}
	if (parts->year == 0 && parts->month == 0 && parts->day == 0) {
		parts->year = g_date_time_get_year(now);
		parts->month = g_date_time_get_month(now);
		parts->day = g_date_time_get_day_of_month(now);
	}
	if (parts->year == 0)
		parts->year = g_date_time_get_year(now);
	if (parts->month == 0)
		parts->month = 1;
	if (parts->day == 0)
		parts->day = 1;
	if (parts->year < 1970 || parts->year > 2100 || parts->month < 1 || parts->month > 12 || parts->day < 1 ||
	    parts->day > 31 || parts->hour < 0 || parts->hour > 23 || parts->minute < 0 || parts->minute > 59 ||
	    parts->second < 0 || parts->second > 60)
		return 0;
	when = g_date_time_new_utc(parts->year, parts->month, parts->day, parts->hour, parts->minute,
		MIN(parts->second, 59));
	if (when == NULL)
		return 0;
	seconds = g_date_time_to_unix(when) - (parts->zone_set ? parts->offset_s : 0);
	if (seconds < DATE_MIN_S || seconds > DATE_MAX_S)
		return 0;
	return seconds * G_USEC_PER_SEC;
}

/* How many times @c repeats at the start of @s. */
static guint
run_of(
	const gchar *s,
	gchar        c
){
	guint n = 0;

	while (s[n] == c)
		n++;
	return n;
}

gint64
axis_def_parse_layout(
	const gchar *layout,
	const gchar *text,
	gint64       now_us
){
	const gchar *l = layout;
	const gchar *t = text;
	Parts parts;

	if (layout == NULL || text == NULL || strlen(layout) > 128 || strlen(text) > 256)
		return 0;
	memset(&parts, 0, sizeof(parts));
	while (*t == ' ')
		t++;
	while (*l != '\0') {
		gchar c = *l;
		guint n = run_of(l, c);
		gint v;
		gsize took;

		if (c == ' ' || c == '\t') {
			/* A blank matches any run of blanks (a no-break space too), or
			 * none. */
			while (*t == ' ' || *t == '\t' || ((guchar)t[0] == 0xc2 && (guchar)t[1] == 0xa0))
				t += *t == ' ' || *t == '\t' ? 1 : 2;
			l += n;
			continue;
		}
		if (c == '\'' || c == '"') {
			const gchar *end = strchr(l + 1, c);
			gsize len = end != NULL ? (gsize)(end - l - 1) : strlen(l + 1);

			if (g_ascii_strncasecmp(t, l + 1, len) != 0)
				return 0;
			t += len;
			l += len + 1 + (end != NULL ? 1 : 0);
			continue;
		}
		if (c == '\\' && l[1] != '\0') {
			if (g_ascii_tolower(*t) != g_ascii_tolower(l[1]))
				return 0;
			t++;
			l += 2;
			continue;
		}
		switch (c) {
		case 'y':
			v = n >= 3 ? read_digits(&t, 4, 4) : read_digits(&t, n == 1 ? 1 : 2, 2);
			if (v < 0)
				return 0;
			parts.year = n >= 3 ? v : (v <= 49 ? 2000 + v : 1900 + v);
			break;
		case 'M':
			if (n >= 3) {
				v = month_of(t, &took);
				if (v == 0)
					return 0;
				t += took;
			} else {
				v = read_digits(&t, n == 1 ? 1 : 2, 2);
				if (v < 1 || v > 12)
					return 0;
			}
			parts.month = v;
			break;
		case 'd':
			if (n >= 3) {
				/* A weekday's name: its letters, not checked. */
				gsize k = 0;

				while (g_ascii_isalpha(t[k]))
					k++;
				if (k < 2)
					return 0;
				t += k;
			} else {
				v = read_digits(&t, n == 1 ? 1 : 2, 2);
				if (v < 1 || v > 31)
					return 0;
				parts.day = v;
			}
			break;
		case 'H':
		case 'h':
			v = read_digits(&t, n == 1 ? 1 : 2, 2);
			if (v < 0)
				return 0;
			parts.hour = v;
			parts.twelve = c == 'h';
			parts.time_set = TRUE;
			break;
		case 'm':
			v = read_digits(&t, n == 1 ? 1 : 2, 2);
			if (v < 0)
				return 0;
			parts.minute = v;
			break;
		case 's':
			v = read_digits(&t, n == 1 ? 1 : 2, 2);
			if (v < 0)
				return 0;
			parts.second = v;
			break;
		case 'f':
		case 'F':
			if (read_digits(&t, c == 'f' ? n : 0, n) < 0)
				return 0;
			break;
		case 't':
			if (g_ascii_tolower(*t) != 'a' && g_ascii_tolower(*t) != 'p')
				return 0;
			parts.pm_set = TRUE;
			parts.pm = g_ascii_tolower(*t) == 'p';
			t++;
			if (n >= 2) {
				if (g_ascii_tolower(*t) != 'm')
					return 0;
				t++;
			} else if (g_ascii_tolower(*t) == 'm') {
				t++;
			}
			break;
		case 'z':
		case 'K':
			if (!read_zone(&t, &parts.offset_s)) {
				if (c == 'K')
					break;
				return 0;
			}
			parts.zone_set = TRUE;
			break;
		default:
			/* A literal character, letters without regard to case. */
			if (g_ascii_tolower(*t) != g_ascii_tolower(c))
				return 0;
			t++;
			l++;
			continue;
		}
		l += n;
	}
	while (*t == ' ')
		t++;
	if (*t != '\0')
		return 0;
	return parts_time(&parts, now_us);
}

/* "5 hours ago", "1 day 2 hours ago", "yesterday", "now": now less that, or
 * 0 when @text is not such. */
static gint64
parse_ago(
	const gchar *text,
	gint64       now_us
){
	g_autofree gchar *lower = g_ascii_strdown(text, -1);
	const gchar *p;
	gint64 seconds = 0;
	gboolean any = FALSE;

	g_strstrip(lower);
	if (strcmp(lower, "now") == 0 || strcmp(lower, "just now") == 0 || strcmp(lower, "today") == 0)
		return now_us;
	if (strcmp(lower, "yesterday") == 0)
		return now_us - G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC;
	p = lower;
	while (*p != '\0') {
		gint64 count;
		gsize n = 0;
		gint64 unit = 0;

		while (*p == ' ' || *p == ',' || g_str_has_prefix(p, "and "))
			p += *p == 'a' ? 4 : 1;
		if (g_str_has_prefix(p, "ago") && (p[3] == '\0' || p[3] == ' '))
			break;
		if (*p == '\0')
			break;
		if (g_str_has_prefix(p, "an ") || g_str_has_prefix(p, "a ")) {
			count = 1;
			p += p[1] == 'n' ? 3 : 2;
		} else if (g_ascii_isdigit(*p)) {
			gchar *end = NULL;
			guint64 value = g_ascii_strtoull(p, &end, 10);

			if (end == NULL || value > 100000)
				return 0;
			count = (gint64)value;
			p = end;
			if (*p == '.' && g_ascii_isdigit(p[1])) {
				p++;
				while (g_ascii_isdigit(*p))
					p++;
			}
		} else {
			return 0;
		}
		while (*p == ' ')
			p++;
		while (g_ascii_isalpha(p[n]))
			n++;
		if (n == 0)
			return 0;
#define UNIT(word, value) else if (n == strlen(word) && strncmp(p, word, n) == 0) unit = value
		if (FALSE) {}
		UNIT("s", 1); UNIT("sec", 1); UNIT("secs", 1); UNIT("second", 1); UNIT("seconds", 1);
		UNIT("m", 60); UNIT("min", 60); UNIT("mins", 60); UNIT("minute", 60); UNIT("minutes", 60);
		UNIT("h", 3600); UNIT("hr", 3600); UNIT("hrs", 3600); UNIT("hour", 3600); UNIT("hours", 3600);
		UNIT("d", 86400); UNIT("day", 86400); UNIT("days", 86400);
		UNIT("w", 604800); UNIT("wk", 604800); UNIT("week", 604800); UNIT("weeks", 604800);
		UNIT("mo", 2592000); UNIT("month", 2592000); UNIT("months", 2592000);
		UNIT("y", 31536000); UNIT("yr", 31536000); UNIT("year", 31536000); UNIT("years", 31536000);
#undef UNIT
		if (unit == 0)
			return 0;
		seconds += count * unit;
		if (seconds > AGO_MAX_S)
			return 0;
		any = TRUE;
		p += n;
	}
	if (!any || seconds * G_USEC_PER_SEC > now_us)
		return 0;
	return now_us - seconds * G_USEC_PER_SEC;
}

/* A time of day at @s ("12:25am", "10:30", "10:30 pm", "7am"): seconds
 * since midnight, -1 when there is none. */
static gint
time_of_day(const gchar *s)
{
	const gchar *p = s;
	gint hour;
	gint minute = 0;
	gint second = 0;

	while (*p == ' ')
		p++;
	hour = read_digits(&p, 1, 2);
	if (hour < 0)
		return -1;
	if (*p == ':') {
		p++;
		minute = read_digits(&p, 2, 2);
		if (minute < 0)
			return -1;
		if (*p == ':') {
			p++;
			second = read_digits(&p, 2, 2);
			if (second < 0)
				return -1;
		}
	}
	while (*p == ' ')
		p++;
	if (g_ascii_tolower(p[0]) == 'a' || g_ascii_tolower(p[0]) == 'p') {
		gboolean pm = g_ascii_tolower(p[0]) == 'p';

		if (hour < 1 || hour > 12)
			return -1;
		hour = hour % 12 + (pm ? 12 : 0);
		p++;
		if (g_ascii_tolower(*p) == 'm')
			p++;
	}
	while (*p == ' ')
		p++;
	if (*p != '\0' || hour > 23 || minute > 59 || second > 59)
		return -1;
	return hour * 3600 + minute * 60 + second;
}

/* "Today 12:25am", "Yesterday 10:00", or what parse_ago() reads. */
static gint64
parse_fuzzy(
	const gchar *text,
	gint64       now_us
){
	g_autofree gchar *lower = g_strstrip(g_ascii_strdown(text, -1));
	gint64 day_us = 0;
	const gchar *rest = NULL;

	if (g_str_has_prefix(lower, "today"))
		rest = lower + 5;
	else if (g_str_has_prefix(lower, "yesterday")) {
		rest = lower + 9;
		day_us = G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC;
	}
	if (rest != NULL) {
		gint64 midnight = (now_us / G_USEC_PER_SEC / 86400) * 86400 * G_USEC_PER_SEC;
		gint seconds;

		while (*rest == ' ' || *rest == ',' || *rest == 'a' || *rest == 't')
			rest++;
		if (now_us < day_us + G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC)
			return 0;
		if (*rest == '\0')
			return now_us - day_us;
		seconds = time_of_day(rest);
		if (seconds < 0)
			return 0;
		return midnight - day_us + (gint64)seconds * G_USEC_PER_SEC;
	}
	return parse_ago(text, now_us);
}

/* The ISO 8601 text of @us ("2024-03-01T12:00:00Z"). */
static gchar *
iso_text(gint64 us)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);

	return when != NULL ? g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ") : g_strdup("");
}

gint64
axis_def_parse_date(
	const gchar *text,
	gint64       now_us
){
	static const gchar *const layouts[] = {
		"ddd, d MMM yyyy H:m:s zzz", "ddd, d MMM yyyy H:m:s", "d MMM yyyy H:m:s zzz", "yyyy-MM-dd HH:mm:ss zzz",
		"yyyy-MM-dd HH:mm zzz", "yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd HH:mm", "yyyy-MM-dd", "yyyy/MM/dd HH:mm:ss",
		"yyyy/MM/dd HH:mm", "yyyy/MM/dd", "yyyy.MM.dd", "dd.MM.yyyy HH:mm:ss", "dd.MM.yyyy HH:mm", "dd.MM.yyyy",
		"dd/MM/yyyy HH:mm:ss", "dd/MM/yyyy HH:mm", "dd/MM/yyyy", "d MMM yyyy HH:mm", "d MMM yyyy", "MMM d, yyyy",
		"MMM d yyyy", "d MMMM yyyy", "MMMM d, yyyy", NULL
	};
	g_autofree gchar *t = NULL;
	gint64 us;
	guint i;

	if (text == NULL)
		return 0;
	t = g_strstrip(g_strdup(text));
	if (t[0] == '\0' || strlen(t) > 256)
		return 0;
	if (strspn(t, "0123456789") == strlen(t)) {
		guint64 value = g_ascii_strtoull(t, NULL, 10);

		if (strlen(t) >= 12)
			value /= 1000;
		if (strlen(t) < 9 || strlen(t) > 13 || value > (guint64)DATE_MAX_S)
			return 0;
		return (gint64)value * G_USEC_PER_SEC;
	}
	{
		g_autoptr(GTimeZone) utc = g_time_zone_new_utc();
		g_autoptr(GDateTime) when = NULL;

		if (strlen(t) >= 16 && g_ascii_isdigit(t[0]) && t[4] == '-' && (t[10] == 'T' || t[10] == 't' || t[10] == ' '))
			when = g_date_time_new_from_iso8601(t, utc);
		if (when != NULL) {
			us = g_date_time_to_unix(when);
			return us >= DATE_MIN_S && us <= DATE_MAX_S ? us * G_USEC_PER_SEC : 0;
		}
	}
	for (i = 0; layouts[i] != NULL; i++) {
		us = axis_def_parse_layout(layouts[i], t, now_us);
		if (us != 0)
			return us;
	}
	return parse_fuzzy(t, now_us);
}

/* ── Sizes and counts ───────────────────────────────────────────── */

/* The number at the start of @s with its separators read as sites write
 * them; its end in @end.  -1 for none. */
static gdouble
read_number(
	const gchar  *s,
	const gchar **end
){
	GString *digits = g_string_new(NULL);
	const gchar *p = s;
	const gchar *last_comma = NULL;
	const gchar *last_dot = NULL;
	const gchar *q;
	gdouble value;
	gchar decimal = 0;

	while (g_ascii_isdigit(*p) || *p == ',' || *p == '.' || (*p == ' ' && g_ascii_isdigit(p[1]) &&
	       p > s && g_ascii_isdigit(p[-1]))) {
		if (*p == ',')
			last_comma = p;
		if (*p == '.')
			last_dot = p;
		p++;
	}
	*end = p;
	if (p == s) {
		g_string_free(digits, TRUE);
		return -1;
	}
	/* The separator that is last is the decimal one when both are there;
	 * a lone comma is one only when other than three digits follow it. */
	if (last_comma != NULL && last_dot != NULL)
		decimal = last_comma > last_dot ? ',' : '.';
	else if (last_dot != NULL)
		decimal = strchr(s, '.') == last_dot ? '.' : 0;
	else if (last_comma != NULL)
		decimal = (strchr(s, ',') == last_comma && (p - last_comma - 1) != 3) ? ',' : 0;
	for (q = s; q < p; q++) {
		if (g_ascii_isdigit(*q))
			g_string_append_c(digits, *q);
		else if (*q == decimal && q == (decimal == ',' ? last_comma : last_dot))
			g_string_append_c(digits, '.');
	}
	value = digits->len > 0 && digits->len < 40 ? g_ascii_strtod(digits->str, NULL) : -1;
	g_string_free(digits, TRUE);
	return value;
}

gint64
axis_def_parse_size(const gchar *text)
{
	static const struct { const gchar *unit; gint power; } units[] = {
		{ "b", 0 }, { "byte", 0 }, { "bytes", 0 }, { "k", 1 }, { "kb", 1 }, { "kib", 1 }, { "m", 2 }, { "mb", 2 },
		{ "mib", 2 }, { "g", 3 }, { "gb", 3 }, { "gib", 3 }, { "t", 4 }, { "tb", 4 }, { "tib", 4 }, { "p", 5 },
		{ "pb", 5 }, { "pib", 5 }, { "\xd0\xb1", 0 }, { "\xd0\xba\xd0\xb1", 1 }, { "\xd0\xbc\xd0\xb1", 2 },
		{ "\xd0\xb3\xd0\xb1", 3 }, { "\xd1\x82\xd0\xb1", 4 }
	};
	const gchar *p = text;
	const gchar *end = NULL;
	g_autofree gchar *unit = NULL;
	gdouble value;
	guint i;

	if (text == NULL || strlen(text) > 1024 || !g_utf8_validate(text, -1, NULL))
		return -1;
	while (*p != '\0' && !g_ascii_isdigit(*p))
		p++;
	value = read_number(p, &end);
	if (value < 0)
		return -1;
	while (*end == ' ' || ((guchar)end[0] == 0xc2 && (guchar)end[1] == 0xa0))
		end += *end == ' ' ? 1 : 2;
	{
		const gchar *u = end;

		while (*u != '\0' && *u != ' ' && *u != ')' && *u != ']' && *u != ',' && *u != '/')
			u++;
		unit = g_utf8_strdown(end, u - end);
	}
	if (unit[0] == '\0')
		return value <= 1e18 ? (gint64)value : -1;
	for (i = 0; i < G_N_ELEMENTS(units); i++) {
		if (strcmp(unit, units[i].unit) == 0) {
			gint k;

			for (k = 0; k < units[i].power; k++)
				value *= 1024.0;
			return value <= 1e18 ? (gint64)(value + 0.5) : -1;
		}
	}
	return -1;
}

gint64
axis_def_parse_count(const gchar *text)
{
	const gchar *p = text;
	gint64 value = 0;
	guint digits = 0;

	if (text == NULL)
		return -1;
	while (*p != '\0' && !g_ascii_isdigit(*p))
		p++;
	/* A negative count is none. */
	if (p > text && p[-1] == '-')
		return -1;
	for (; *p != '\0'; p++) {
		if (g_ascii_isdigit(*p)) {
			if (++digits > 12)
				return -1;
			value = value * 10 + (*p - '0');
		} else if (*p != ',' && *p != '.' && *p != ' ' && *p != '\'') {
			break;
		}
	}
	return digits > 0 ? value : -1;
}

/* ── The filters ────────────────────────────────────────────────── */

static gchar *
replace_all(
	const gchar *text,
	const gchar *from,
	const gchar *to
){
	GString *out;
	const gchar *p = text;
	gsize n = strlen(from);

	if (n == 0)
		return g_strdup(text);
	out = g_string_new(NULL);
	while (TRUE) {
		const gchar *hit = strstr(p, from);

		if (hit == NULL) {
			g_string_append(out, p);
			break;
		}
		g_string_append_len(out, p, hit - p);
		g_string_append(out, to);
		p = hit + n;
		if (out->len > AXIS_DEF_FILTER_MAX_TEXT)
			break;
	}
	return g_string_free(out, FALSE);
}

/* split: the @index-th part of @text split at the first character of
 * @separator (from the end when negative); "" when there is none. */
static gchar *
split_part(
	const gchar *text,
	const gchar *separator,
	gint64       index
){
	g_autofree gchar *sep = NULL;
	g_auto(GStrv) parts = NULL;
	gint64 n;
	gunichar c;

	if (separator == NULL || separator[0] == '\0')
		return g_strdup(text);
	c = g_utf8_get_char(separator);
	sep = g_ucs4_to_utf8(&c, 1, NULL, NULL, NULL);
	parts = g_strsplit(text, sep, 4096);
	n = (gint64)g_strv_length(parts);
	if (index < 0)
		index += n;
	if (index < 0 || index >= n)
		return g_strdup("");
	return g_strdup(parts[index]);
}

static gchar *
trim_chars(
	const gchar *text,
	const gchar *chars
){
	const gchar *start = text;
	const gchar *end = text + strlen(text);

	if (chars == NULL || chars[0] == '\0') {
		gchar *copy = g_strdup(text);

		return g_strstrip(copy);
	}
	while (start < end && strchr(chars, *start) != NULL)
		start++;
	while (end > start && strchr(chars, end[-1]) != NULL)
		end--;
	return g_strndup(start, (gsize)(end - start));
}

/* validfilename: the characters a file's name cannot hold as '_'. */
static gchar *
valid_file_name(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text; *p != '\0'; p++)
		g_string_append_c(out, strchr("\\/:*?\"<>|", *p) != NULL || ((guchar)*p < 0x20) ? '_' : *p);
	return g_string_free(out, FALSE);
}

/* diacritics: the marks taken off letters ("é" is "e"). */
static gchar *
without_diacritics(const gchar *text)
{
	g_autofree gchar *decomposed = g_utf8_normalize(text, -1, G_NORMALIZE_NFD);
	GString *out = g_string_new(NULL);
	const gchar *p;

	if (decomposed == NULL)
		return g_string_free(out, FALSE);
	for (p = decomposed; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (g_unichar_type(c) != G_UNICODE_NON_SPACING_MARK)
			g_string_append_unichar(out, c);
	}
	return g_string_free(out, FALSE);
}

static gchar *
html_encode(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		switch (*p) {
		case '&': g_string_append(out, "&amp;"); break;
		case '<': g_string_append(out, "&lt;"); break;
		case '>': g_string_append(out, "&gt;"); break;
		case '"': g_string_append(out, "&quot;"); break;
		case '\'': g_string_append(out, "&#39;"); break;
		default: g_string_append_c(out, *p); break;
		}
	}
	return g_string_free(out, FALSE);
}

static gchar *
base64_decode(const gchar *text)
{
	g_autofree gchar *clean = g_strstrip(g_strdup(text));
	gsize n = 0;
	guchar *bytes;
	gchar *out;
	gsize i;

	if (strspn(clean, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != strlen(clean) ||
	    strlen(clean) > AXIS_DEF_FILTER_MAX_TEXT)
		return g_strdup("");
	bytes = g_base64_decode(clean, &n);
	out = g_new(gchar, n + 1);
	for (i = 0; i < n; i++)
		out[i] = bytes[i] != 0 ? (gchar)bytes[i] : ' ';
	out[n] = '\0';
	g_free(bytes);
	return out;
}

gchar *
axis_def_filter_apply(
	const gchar         *name,
	const gchar *const  *args,
	const gchar         *value,
	gint64               now_us,
	AxisDefRegexCache   *regex,
	gchar              **problem
){
	guint n = n_args(args);
	const gchar *a0 = n > 0 ? args[0] : NULL;
	const gchar *a1 = n > 1 ? args[1] : NULL;
	const gchar *v = value != NULL ? value : "";

	if (problem != NULL)
		*problem = NULL;
	if (name == NULL || !axis_def_filter_known(name)) {
		set_problem(problem, "a filter Axis does not have");
		return NULL;
	}
	if (strcmp(name, "replace") == 0) {
		if (n < 2) {
			set_problem(problem, "the filter replace without its two arguments");
			return NULL;
		}
		return finish(replace_all(v, a0, a1));
	}
	if (strcmp(name, "re_replace") == 0) {
		if (n < 2) {
			set_problem(problem, "the filter re_replace without its two arguments");
			return NULL;
		}
		return finish(axis_def_regex_replace(regex, a0, v, a1, problem));
	}
	if (strcmp(name, "regexp") == 0) {
		if (n < 1) {
			set_problem(problem, "the filter regexp without its pattern");
			return NULL;
		}
		return finish(axis_def_regex_group(regex, a0, v, 1, problem));
	}
	if (strcmp(name, "split") == 0) {
		gint64 index = a1 != NULL ? g_ascii_strtoll(a1, NULL, 10) : 0;

		if (n < 1) {
			set_problem(problem, "the filter split without its separator");
			return NULL;
		}
		return finish(split_part(v, a0, index));
	}
	if (strcmp(name, "trim") == 0)
		return finish(trim_chars(v, a0));
	if (strcmp(name, "append") == 0)
		return finish(g_strconcat(v, a0 != NULL ? a0 : "", NULL));
	if (strcmp(name, "prepend") == 0)
		return finish(g_strconcat(a0 != NULL ? a0 : "", v, NULL));
	if (strcmp(name, "tolower") == 0)
		return finish(g_utf8_strdown(v, -1));
	if (strcmp(name, "toupper") == 0)
		return finish(g_utf8_strup(v, -1));
	if (strcmp(name, "urldecode") == 0)
		return axis_def_url_decode(v);
	if (strcmp(name, "urlencode") == 0)
		return finish(axis_def_url_encode(v));
	if (strcmp(name, "querystring") == 0) {
		if (n < 1) {
			set_problem(problem, "the filter querystring without its parameter's name");
			return NULL;
		}
		return finish(query_value(v, a0));
	}
	if (strcmp(name, "dateparse") == 0 || strcmp(name, "timeparse") == 0) {
		gint64 us;

		if (n < 1) {
			set_problem(problem, "the filter dateparse without its layout");
			return NULL;
		}
		us = axis_def_parse_layout(a0, v, now_us);
		return us != 0 ? iso_text(us) : g_strdup("");
	}
	if (strcmp(name, "timeago") == 0 || strcmp(name, "reltime") == 0) {
		gint64 us = parse_ago(v, now_us);

		return us != 0 ? iso_text(us) : g_strdup("");
	}
	if (strcmp(name, "fuzzytime") == 0) {
		gint64 us = parse_fuzzy(v, now_us);

		if (us == 0)
			us = axis_def_parse_date(v, now_us);
		return us != 0 ? iso_text(us) : g_strdup("");
	}
	if (strcmp(name, "validfilename") == 0)
		return finish(valid_file_name(v));
	if (strcmp(name, "htmldecode") == 0)
		return finish(axis_html_decode_entities(v));
	if (strcmp(name, "htmlencode") == 0)
		return finish(html_encode(v));
	if (strcmp(name, "diacritics") == 0)
		return finish(without_diacritics(v));
	if (strcmp(name, "base64decode") == 0)
		return finish(base64_decode(v));
	if (strcmp(name, "base64encode") == 0)
		return finish(g_base64_encode((const guchar *)v, strlen(v)));
	/* strdump */
	return g_strdup(v);
}
