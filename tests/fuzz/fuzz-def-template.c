/*
 * fuzz-def-template.c - libFuzzer target for the template evaluator of
 * indexer definitions (indexer/def/axis-def-template.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first line is a template; the lines after it are the values of
 * .Keywords, .Config.a, .Config.b, .Result.c and the items of .Categories.
 * Whatever they are, the code returns without crashing or allocating
 * without bound, and:
 *
 *  - the template is read or refused with a reason, never both;
 *  - rendering gives text or a reason; text is at most
 *    AXIS_TEMPLATE_MAX_OUTPUT bytes and valid UTF-8 (the values are), and
 *    rendering again gives the same text;
 *  - a template without an action renders as itself;
 *  - with an escape, the template's own text is unchanged: what the
 *    escape marks appears only around what actions put in.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/def/axis-def-regex.h"
#include "indexer/def/axis-def-template.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
fail(int line)
{
	fprintf(stderr, "fuzz-def-template: the check at line %d failed\n", line);
	abort();
}

static gchar *
bracket(const gchar *text)
{
	return g_strconcat("\x01", text, "\x02", NULL);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const gchar *const names[] = { "Keywords", "Config.a", "Config.b", "Result.c", NULL };
	gchar *valid;
	gchar **lines;
	gchar *problem = NULL;
	GHashTable *values;
	GPtrArray *categories;
	AxisTemplateVars vars;
	AxisDefRegexCache *regex;
	AxisTemplate *template_;
	guint n;
	guint i;

	if (size > 64 * 1024)
		return 0;
	valid = g_utf8_make_valid((const gchar *)data, (gssize)size);
	lines = g_strsplit(valid, "\n", 40);
	n = g_strv_length(lines);
	values = g_hash_table_new(g_str_hash, g_str_equal);
	categories = g_ptr_array_new();
	for (i = 0; names[i] != NULL && i + 1 < n; i++)
		g_hash_table_insert(values, (gpointer)names[i], lines[i + 1]);
	for (i = 5; i < n; i++)
		g_ptr_array_add(categories, lines[i]);
	vars.values = values;
	vars.categories = categories;
	regex = axis_def_regex_cache_new(0);
	template_ = n > 0 ? axis_template_parse(lines[0], &problem, NULL) : NULL;
	if (n > 0 && template_ == NULL && problem == NULL)
		fail(__LINE__);
	if (template_ != NULL) {
		gchar *first;
		gchar *second;
		gchar *escaped;

		if (problem != NULL)
			fail(__LINE__);
		first = axis_template_render(template_, &vars, NULL, regex, &problem);
		if (first != NULL) {
			if (problem != NULL || strlen(first) > AXIS_TEMPLATE_MAX_OUTPUT || !g_utf8_validate(first, -1, NULL))
				fail(__LINE__);
			if (!axis_template_has_actions(lines[0]) && strcmp(first, lines[0]) != 0)
				fail(__LINE__);
			second = axis_template_render(template_, &vars, NULL, regex, NULL);
			if (second != NULL && strcmp(first, second) != 0 && !axis_def_regex_cache_exhausted(regex))
				fail(__LINE__);
			g_free(second);
		} else if (problem == NULL) {
			fail(__LINE__);
		}
		g_clear_pointer(&problem, g_free);
		/* With an escape: the brackets come only from it. */
		escaped = axis_template_render(template_, &vars, bracket, regex, NULL);
		if (escaped != NULL && strchr(lines[0], '\x01') == NULL && strchr(lines[0], '\x02') == NULL && first != NULL &&
		    !axis_def_regex_cache_exhausted(regex)) {
			GString *plain = g_string_new(NULL);
			const gchar *p;

			for (p = escaped; *p != '\0'; p++) {
				if (*p != '\x01' && *p != '\x02')
					g_string_append_c(plain, *p);
			}
			/* Values may hold the brackets themselves; compare only when
			 * none does. */
			for (i = 1; i < n; i++) {
				if (strchr(lines[i], '\x01') != NULL || strchr(lines[i], '\x02') != NULL)
					break;
			}
			if (i == n && strcmp(plain->str, first) != 0 && strlen(first) < AXIS_TEMPLATE_MAX_OUTPUT / 2)
				fail(__LINE__);
			g_string_free(plain, TRUE);
		}
		g_free(escaped);
		g_free(first);
		axis_template_free(template_);
	}
	g_free(problem);
	axis_def_regex_cache_free(regex);
	g_ptr_array_unref(categories);
	g_hash_table_unref(values);
	g_strfreev(lines);
	g_free(valid);
	return 0;
}
