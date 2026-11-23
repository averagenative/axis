/*
 * test-web-text.c - the names that break at their separators
 * (axis_web_breakable()) and the counts that read right in the singular
 * (axis_web_count())
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios of web-shell:
 *   Long names break at their separators / Release name in a narrow column
 *     (a break opportunity after a dot, hyphen, underscore or slash)
 *   Long names break at their separators / Copying
 *     (the text is the name exactly; markup in a name is escaped)
 */

#include <glib.h>
#include <htmx-glib.h>
#include <string.h>

#include "web/axis-web-text.h"

/* The HTML of @text made breakable. */
static gchar *
breakable_html(const gchar *text)
{
	HtmxNode *node = axis_web_breakable(text);
	gchar *html = htmx_element_render(HTMX_ELEMENT(node));

	g_object_unref(node);
	return html;
}

/* Everything but the break opportunities: what a person copying the name
 * from the page gets (before the browser unescapes it). */
static gchar *
without_breaks(const gchar *html)
{
	g_auto(GStrv) pieces = g_strsplit(html, "<wbr />", -1);

	return g_strjoinv("", pieces);
}

static void
assert_html(
	const gchar *text,
	const gchar *want
){
	g_autofree gchar *html = breakable_html(text);

	g_assert_cmpstr(html, ==, want);
}

/* A break opportunity after each run of separators that has more of the word
 * after it, and nowhere else. */
static void
test_breaks_after_separators(void)
{
	assert_html("Hearth.1995.1080p.BluRay.x264-GRP",
		"Hearth.<wbr />1995.<wbr />1080p.<wbr />BluRay.<wbr />x264-<wbr />GRP");
	assert_html("a_b-c.d/e", "a_<wbr />b-<wbr />c.<wbr />d/<wbr />e");
	/* A run of separators is one break, after the run. */
	assert_html("a..b--c__d//e", "a..<wbr />b--<wbr />c__<wbr />d//<wbr />e");
	assert_html("downloads/complete/Hearth.1995-GRP", "downloads/<wbr />complete/<wbr />Hearth.<wbr />1995-<wbr />GRP");
}

/* None where it would do nothing, or break a word. */
static void
test_no_break_where_none_is_needed(void)
{
	assert_html("", "");
	assert_html(NULL, "");
	assert_html("Hearth", "Hearth");
	assert_html("trailing.", "trailing.");
	assert_html("trailing...", "trailing...");
	assert_html(".hidden", ".hidden");
	assert_html("...", "...");
	/* A sentence's full stop is not a separator of a name. */
	assert_html("Grabbed it. Then x.y", "Grabbed it. Then x.<wbr />y");
	assert_html("a - b", "a - b");
	/* Multi-byte text is never cut inside a character. */
	assert_html("Am\303\251lie.2001", "Am\303\251lie.<wbr />2001");
}

/* The text is the name: nothing added, nothing dropped, in the order given. */
static void
test_text_is_unchanged(void)
{
	static const gchar *const names[] = {
		"The.Mazework.1999.1080p.WEB-DL.DDP5.1.H.264-WEB",
		"Some_Film_2020_1080p_BluRay_x264-GRP",
		"/data/downloads/complete/Hearth.1995.1080p.BluRay.x264-GRP/hearth.mkv",
		"a.-_/b",
		"one two.three four-five six_seven",
		"..a..", "-", ".", "x"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		g_autofree gchar *html = breakable_html(names[i]);
		g_autofree gchar *copied = without_breaks(html);

		g_assert_cmpstr(copied, ==, names[i]);
		/* Break opportunities and text, nothing else. */
		g_assert_null(strstr(html, "<span"));
		g_assert_null(strstr(html, "<bdi"));
	}
}

/* Markup in a name is text, as everywhere else. */
static void
test_markup_is_escaped(void)
{
	g_autofree gchar *html = breakable_html("<script>alert(1)</script>.Film-<b>2020</b>&x.y");
	g_autofree gchar *copied = without_breaks(html);

	g_assert_null(strstr(html, "<script"));
	g_assert_null(strstr(html, "<b>"));
	g_assert_null(strstr(html, "</b>"));
	g_assert_cmpstr(copied, ==, "&lt;script&gt;alert(1)&lt;/script&gt;.Film-&lt;b&gt;2020&lt;/b&gt;&amp;x.y");
	/* Its break opportunities are the same as for any name (the slashes of
	 * the tags among them: they are text). */
	g_assert_cmpstr(html, ==, "&lt;script&gt;alert(1)&lt;/<wbr />script&gt;.<wbr />Film-<wbr />&lt;b&gt;2020&lt;/<wbr />"
		"b&gt;&amp;x.<wbr />y");
}

static void
test_counts(void)
{
	g_autofree gchar *none = axis_web_count(0, "release", "releases");
	g_autofree gchar *one = axis_web_count(1, "release", "releases");
	g_autofree gchar *two = axis_web_count(2, "release", "releases");
	g_autofree gchar *many = axis_web_count(1200, "indexer", "indexers");
	g_autofree gchar *words = axis_web_count(1, "result was", "results were");

	g_assert_cmpstr(none, ==, "0 releases");
	g_assert_cmpstr(one, ==, "1 release");
	g_assert_cmpstr(two, ==, "2 releases");
	/* A comma between thousands, never localised (refine-indexer-followups
	 * 3.1). */
	g_assert_cmpstr(many, ==, "1,200 indexers");
	g_assert_cmpstr(words, ==, "1 result was");
	{
		g_autofree gchar *a = axis_web_format_count(0);
		g_autofree gchar *b = axis_web_format_count(999);
		g_autofree gchar *c = axis_web_format_count(1000);
		g_autofree gchar *d = axis_web_format_count(1234567);

		g_assert_cmpstr(a, ==, "0");
		g_assert_cmpstr(b, ==, "999");
		g_assert_cmpstr(c, ==, "1,000");
		g_assert_cmpstr(d, ==, "1,234,567");
	}
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-text/breaks-after-separators", test_breaks_after_separators);
	g_test_add_func("/web-text/no-break-where-none-is-needed", test_no_break_where_none_is_needed);
	g_test_add_func("/web-text/text-is-unchanged", test_text_is_unchanged);
	g_test_add_func("/web-text/markup-is-escaped", test_markup_is_escaped);
	g_test_add_func("/web-text/counts", test_counts);

	return g_test_run();
}
