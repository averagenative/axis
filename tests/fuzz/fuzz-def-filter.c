/*
 * fuzz-def-filter.c - libFuzzer target for the filters of indexer
 * definitions, their regular expressions, and the readers of dates, sizes
 * and counts (indexer/def/axis-def-filter.h, indexer/def/axis-def-regex.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses the filter (in the order of the list below); the
 * lines after it are the value and up to three arguments.  Whatever they
 * are, the code returns without crashing or allocating without bound, and:
 *
 *  - a filter gives text or a reason, never both; text is valid UTF-8 of at
 *    most AXIS_DEF_FILTER_MAX_TEXT bytes;
 *  - a date read is 0 or within 1970 to 2100; dateparse's, timeago's and
 *    fuzzytime's results are empty or ISO 8601 text that the date reader
 *    reads back;
 *  - a size and a count are -1 or not negative;
 *  - the regular expressions' time budget, once spent, refuses at once.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/def/axis-def-filter.h"
#include "indexer/def/axis-def-regex.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define NOW_US (G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC)

static void
fail(int line)
{
	fprintf(stderr, "fuzz-def-filter: the check at line %d failed\n", line);
	abort();
}

static void
check_date(gint64 us)
{
	if (us != 0 && (us < 0 || us > G_GINT64_CONSTANT(4102444800) * G_USEC_PER_SEC))
		fail(__LINE__);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const gchar *const filters[] = {
		"replace", "re_replace", "regexp", "split", "trim", "append", "prepend", "tolower", "toupper",
		"urldecode", "urlencode", "querystring", "dateparse", "timeparse", "timeago", "reltime", "fuzzytime",
		"validfilename", "htmldecode", "htmlencode", "diacritics", "base64decode", "base64encode", "strdump",
		"nonsense"
	};
	const gchar *name;
	gchar *valid;
	gchar **lines;
	const gchar *args[4] = { NULL, NULL, NULL, NULL };
	gchar *problem = NULL;
	gchar *out;
	AxisDefRegexCache *regex;
	guint n;
	guint i;

	if (size < 1 || size > 64 * 1024)
		return 0;
	name = filters[data[0] % G_N_ELEMENTS(filters)];
	valid = g_utf8_make_valid((const gchar *)data + 1, (gssize)(size - 1));
	lines = g_strsplit(valid, "\n", 5);
	n = g_strv_length(lines);
	for (i = 1; i < n && i < 4; i++)
		args[i - 1] = lines[i];
	regex = axis_def_regex_cache_new(0);
	out = axis_def_filter_apply(name, args, n > 0 ? lines[0] : "", NOW_US, regex, &problem);
	if (out != NULL) {
		if (problem != NULL || !g_utf8_validate(out, -1, NULL) || strlen(out) > AXIS_DEF_FILTER_MAX_TEXT)
			fail(__LINE__);
		if ((strcmp(name, "dateparse") == 0 || strcmp(name, "timeago") == 0 || strcmp(name, "fuzzytime") == 0) &&
		    out[0] != '\0' && axis_def_parse_date(out, NOW_US) == 0)
			fail(__LINE__);
	} else if (problem == NULL) {
		fail(__LINE__);
	}
	g_free(out);
	g_clear_pointer(&problem, g_free);
	for (i = 0; i < n; i++) {
		gint64 size_bytes = axis_def_parse_size(lines[i]);
		gint64 count = axis_def_parse_count(lines[i]);

		check_date(axis_def_parse_date(lines[i], NOW_US));
		if (i + 1 < n)
			check_date(axis_def_parse_layout(lines[i], lines[i + 1], NOW_US));
		if (size_bytes < -1 || count < -1)
			fail(__LINE__);
	}
	/* A spent budget refuses at once. */
	if (axis_def_regex_cache_exhausted(regex)) {
		gchar *again = axis_def_regex_replace(regex, "a", "a", "b", &problem);

		if (again != NULL || problem == NULL)
			fail(__LINE__);
		g_free(problem);
	}
	axis_def_regex_cache_free(regex);
	g_strfreev(lines);
	g_free(valid);
	return 0;
}
