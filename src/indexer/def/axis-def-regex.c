/*
 * axis-def-regex.c - bounded regular expressions for indexer definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/def/axis-def-regex.h"

#include <string.h>

struct _AxisDefRegexCache
{
	GHashTable *compiled;	/* pattern -> GRegex * (or none: NULL value for one refused) */
	gint64      budget_us;
	gint64      spent_us;
	gboolean    exhausted;
};

AxisDefRegexCache *
axis_def_regex_cache_new(gint64 budget_us)
{
	AxisDefRegexCache *cache = g_new0(AxisDefRegexCache, 1);

	cache->compiled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_regex_unref);
	cache->budget_us = budget_us > 0 ? budget_us : AXIS_DEF_REGEX_BUDGET_US;
	return cache;
}

void
axis_def_regex_cache_free(AxisDefRegexCache *cache)
{
	if (cache == NULL)
		return;
	g_hash_table_unref(cache->compiled);
	g_free(cache);
}

gboolean
axis_def_regex_cache_exhausted(const AxisDefRegexCache *cache)
{
	return cache->exhausted;
}

gint64
axis_def_regex_cache_spent_us(const AxisDefRegexCache *cache)
{
	return cache->spent_us;
}

static void
set_problem(
	gchar       **problem,
	const gchar  *text
){
	if (problem != NULL && *problem == NULL)
		*problem = g_strdup(text);
}

/*
 * The two forms of .NET's that PCRE2 writes otherwise: "\uXXXX" is
 * "\x{XXXX}", and a Unicode block "\p{IsCyrillic}" is the script
 * "\p{Cyrillic}" (CJK ideographs are the script Han).
 */
static gchar *
translate_pattern(const gchar *pattern)
{
	GString *out = g_string_new(NULL);
	const gchar *p = pattern;

	while (*p != '\0') {
		if (p[0] != '\\' || p[1] == '\0') {
			g_string_append_c(out, *p++);
			continue;
		}
		if (p[1] == 'u' && g_ascii_isxdigit(p[2]) && g_ascii_isxdigit(p[3]) && g_ascii_isxdigit(p[4]) &&
		    g_ascii_isxdigit(p[5])) {
			g_string_append_printf(out, "\\x{%.4s}", p + 2);
			p += 6;
			continue;
		}
		if ((p[1] == 'p' || p[1] == 'P') && strncmp(p + 2, "{Is", 3) == 0) {
			const gchar *close = strchr(p + 5, '}');

			if (close != NULL) {
				g_autofree gchar *block = g_strndup(p + 5, (gsize)(close - p - 5));

				if (g_str_has_prefix(block, "CJK"))
					g_string_append_printf(out, "\\%c{Han}", p[1]);
				else if (g_str_has_prefix(block, "Hangul"))
					g_string_append_printf(out, "\\%c{Hangul}", p[1]);
				else if (g_str_has_prefix(block, "GreekandCoptic"))
					g_string_append_printf(out, "\\%c{Greek}", p[1]);
				else
					g_string_append_printf(out, "\\%c{%s}", p[1], block);
				p = close + 1;
				continue;
			}
		}
		g_string_append_c(out, p[0]);
		g_string_append_c(out, p[1]);
		p += 2;
	}
	return g_string_free(out, FALSE);
}

/* The pattern as compiled: the limits in front of it. */
static GRegex *
compile(
	const gchar  *pattern,
	gchar       **problem
){
	g_autofree gchar *translated = NULL;
	g_autofree gchar *bounded = NULL;
	g_autoptr(GError) error = NULL;
	GRegex *regex;

	if (pattern == NULL || strlen(pattern) > AXIS_DEF_REGEX_MAX_PATTERN) {
		set_problem(problem, "a regular expression longer than the limit");
		return NULL;
	}
	if (strstr(pattern, "(*") != NULL || strstr(pattern, "\\C") != NULL) {
		set_problem(problem, "a regular expression with (* or \\C, which Axis does not run");
		return NULL;
	}
	if (!g_utf8_validate(pattern, -1, NULL)) {
		set_problem(problem, "a regular expression that is not UTF-8");
		return NULL;
	}
	translated = translate_pattern(pattern);
	bounded = g_strdup_printf("(*LIMIT_MATCH=%d)(*LIMIT_DEPTH=%d)(*LIMIT_HEAP=%d)%s", AXIS_DEF_REGEX_MATCH_LIMIT,
		AXIS_DEF_REGEX_DEPTH_LIMIT, AXIS_DEF_REGEX_HEAP_KIB, translated);
	regex = g_regex_new(bounded, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, &error);
	if (regex == NULL)
		set_problem(problem, "a regular expression that Axis cannot read");
	return regex;
}

gboolean
axis_def_regex_check(
	const gchar  *pattern,
	gchar       **problem
){
	GRegex *regex = compile(pattern, problem);

	if (regex == NULL)
		return FALSE;
	g_regex_unref(regex);
	return TRUE;
}

/* The compiled pattern from the cache, compiled now if it is not there. */
static GRegex *
get(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	gchar             **problem
){
	GRegex *regex;

	if (cache->exhausted) {
		set_problem(problem, "the definition's regular expressions took longer than the limit on this answer");
		return NULL;
	}
	regex = pattern != NULL ? g_hash_table_lookup(cache->compiled, pattern) : NULL;
	if (regex != NULL)
		return regex;
	regex = compile(pattern, problem);
	if (regex == NULL)
		return NULL;
	if (g_hash_table_size(cache->compiled) >= AXIS_DEF_REGEX_MAX_CACHED)
		g_hash_table_remove_all(cache->compiled);
	g_hash_table_insert(cache->compiled, g_strdup(pattern), regex);
	return regex;
}

/* @subject cut to the limit, at a character's boundary. */
static gchar *
cut_subject(const gchar *subject)
{
	gsize n = subject != NULL ? strlen(subject) : 0;
	const gchar *end;

	if (n <= AXIS_DEF_REGEX_MAX_SUBJECT)
		return g_strdup(subject != NULL ? subject : "");
	end = subject + AXIS_DEF_REGEX_MAX_SUBJECT;
	while (end > subject && ((guchar)*end & 0xc0) == 0x80)
		end--;
	return g_strndup(subject, (gsize)(end - subject));
}

static void
charge(
	AxisDefRegexCache *cache,
	gint64             started
){
	cache->spent_us += g_get_monotonic_time() - started;
	if (cache->spent_us > cache->budget_us)
		cache->exhausted = TRUE;
}

/* A group number or name of @regex for a .NET reference, or -1. */
static gint
group_of(
	GRegex      *regex,
	const gchar *name
){
	gchar *end = NULL;
	guint64 value;

	if (name[0] != '\0' && strspn(name, "0123456789") == strlen(name)) {
		value = g_ascii_strtoull(name, &end, 10);
		return value <= (guint64)g_regex_get_capture_count(regex) ? (gint)value : -1;
	}
	return g_regex_get_string_number(regex, name);
}

/* .NET's replacement syntax as GLib's. */
static gchar *
translate_replacement(
	GRegex      *regex,
	const gchar *replacement
){
	GString *out = g_string_new(NULL);
	const gchar *p = replacement != NULL ? replacement : "";
	gint captures = g_regex_get_capture_count(regex);

	while (*p != '\0') {
		if (*p == '\\') {
			g_string_append(out, "\\\\");
			p++;
			continue;
		}
		if (*p != '$') {
			g_string_append_c(out, *p++);
			continue;
		}
		if (p[1] == '$') {
			g_string_append_c(out, '$');
			p += 2;
		} else if (p[1] == '&') {
			g_string_append(out, "\\g<0>");
			p += 2;
		} else if (p[1] == '{') {
			const gchar *close = strchr(p + 2, '}');
			g_autofree gchar *name = close != NULL ? g_strndup(p + 2, (gsize)(close - p - 2)) : NULL;
			gint group = name != NULL ? group_of(regex, name) : -1;

			if (group >= 0) {
				g_string_append_printf(out, "\\g<%d>", group);
				p = close + 1;
			} else {
				g_string_append_c(out, '$');
				p++;
			}
		} else if (g_ascii_isdigit(p[1])) {
			gsize digits = strspn(p + 1, "0123456789");
			gint value = -1;

			/* The longest number that is a group, as .NET reads it. */
			while (digits > 0) {
				g_autofree gchar *number = g_strndup(p + 1, MIN(digits, (gsize)9));

				value = (gint)g_ascii_strtoll(number, NULL, 10);
				if (digits <= 9 && value <= captures)
					break;
				digits--;
				value = -1;
			}
			if (value >= 0) {
				g_string_append_printf(out, "\\g<%d>", value);
				p += 1 + digits;
			} else {
				g_string_append_c(out, '$');
				p++;
			}
		} else {
			g_string_append_c(out, '$');
			p++;
		}
	}
	return g_string_free(out, FALSE);
}

gchar *
axis_def_regex_replace(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	const gchar        *replacement,
	gchar             **problem
){
	GRegex *regex = get(cache, pattern, problem);
	g_autofree gchar *cut = NULL;
	g_autofree gchar *glib_replacement = NULL;
	g_autoptr(GError) error = NULL;
	gint64 started;
	gchar *result;

	if (regex == NULL)
		return NULL;
	cut = cut_subject(subject);
	glib_replacement = translate_replacement(regex, replacement);
	started = g_get_monotonic_time();
	result = g_regex_replace(regex, cut, -1, 0, glib_replacement, G_REGEX_MATCH_DEFAULT, &error);
	charge(cache, started);
	if (result == NULL) {
		set_problem(problem, "a regular expression went past its limits");
		return NULL;
	}
	if (!g_utf8_validate(result, -1, NULL)) {
		gchar *valid = g_utf8_make_valid(result, -1);

		g_free(result);
		result = valid;
	}
	return result;
}

gchar *
axis_def_regex_group(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	gint                group,
	gchar             **problem
){
	GRegex *regex = get(cache, pattern, problem);
	g_autofree gchar *cut = NULL;
	g_autoptr(GMatchInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	gint64 started;
	gboolean matched;
	gchar *text;

	if (regex == NULL)
		return NULL;
	cut = cut_subject(subject);
	started = g_get_monotonic_time();
	matched = g_regex_match_full(regex, cut, -1, 0, G_REGEX_MATCH_DEFAULT, &info, &error);
	charge(cache, started);
	if (error != NULL) {
		set_problem(problem, "a regular expression went past its limits");
		return NULL;
	}
	if (!matched || group < 0 || group > g_regex_get_capture_count(regex))
		return g_strdup("");
	text = g_match_info_fetch(info, group);
	return text != NULL ? text : g_strdup("");
}

gboolean
axis_def_regex_matches(
	AxisDefRegexCache  *cache,
	const gchar        *pattern,
	const gchar        *subject,
	gchar             **problem
){
	GRegex *regex = get(cache, pattern, problem);
	g_autofree gchar *cut = NULL;
	g_autoptr(GError) error = NULL;
	gint64 started;
	gboolean matched;

	if (regex == NULL)
		return FALSE;
	cut = cut_subject(subject);
	started = g_get_monotonic_time();
	matched = g_regex_match_full(regex, cut, -1, 0, G_REGEX_MATCH_DEFAULT, NULL, &error);
	charge(cache, started);
	if (error != NULL) {
		set_problem(problem, "a regular expression went past its limits");
		return FALSE;
	}
	return matched;
}
