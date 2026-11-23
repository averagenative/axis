/*
 * test-indexer-definitions.c - the readers and the engine of indexer
 * definitions, without a network or a database: the YAML subset, the page
 * reader and its selectors, templates, filters and their regular
 * expressions, the judging of a definition, and the engine's requests and
 * releases from hand-written fixtures
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The fixtures (tests/fixtures/definitions and its README) are hand-written
 * in the style of the community's format; none is copied, none is a real
 * site.  Nothing here makes a request.
 *
 * add-indexer-definitions, tasks 1.2 to 1.5 and 4.1 (with hand-written
 * fixtures), 4.2 (unsupported features name the feature), 4.3 (the engine
 * level: what a definition or a page cannot make Axis ask), 4.4 (the
 * regular expressions' bounds).
 */

#include <glib.h>
#include <string.h>

#include "core/axis-http-origin.h"
#include "download/axis-download-model.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"
#include "indexer/def/axis-def-filter.h"
#include "indexer/def/axis-def-html.h"
#include "indexer/def/axis-def-regex.h"
#include "indexer/def/axis-def-select.h"
#include "indexer/def/axis-def-template.h"
#include "indexer/def/axis-def-yaml.h"

#define FIXTURES "tests/fixtures/definitions/"

/* 2023-11-14 22:13:20 UTC */
#define NOW_US (G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC)

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, &error));
	g_assert_no_error(error);
	return data;
}

/* @text with every @from replaced by @to. */
static gchar *
replaced(
	const gchar *text,
	const gchar *from,
	const gchar *to
){
	g_auto(GStrv) parts = g_strsplit(text, from, -1);

	return g_strjoinv(to, parts);
}

static AxisDef *
load_ok(const gchar *text)
{
	g_autofree gchar *problem = NULL;
	AxisDef *def = axis_def_load(text, strlen(text), &problem);

	if (def == NULL)
		g_error("not a definition: %s", problem);
	return def;
}

static AxisDef *
load_fixture(const gchar *name)
{
	g_autofree gchar *text = fixture(name);

	return load_ok(text);
}

static void
assert_usable(const AxisDef *def)
{
	g_autofree gchar *problem = axis_def_problem(def);

	if (!axis_def_usable(def))
		g_error("%s is not usable: %s", def->id, problem);
}

/* ── YAML ───────────────────────────────────────────────────────── */

static void
test_yaml_reads(void)
{
	static const gchar text[] =
		"---\n"
		"# a comment\n"
		"id: x1   # after a value\n"
		"plain: a plain value, a colon:x and http://x\n"
		"folded: first line\n"
		"  second line\n"
		"\n"
		"  after an empty line\n"
		"single: 'it''s # not a comment'\n"
		"double: \"tab\\there \\\"q\\\" \\\\ \\/ \\x41 \\u00e9 \\U0001F600\"\n"
		"multi: \"one\n"
		"  two \\\n"
		"  three\"\n"
		"empty:\n"
		"tilde: ~\n"
		"yes: yes\n"
		"list:\n"
		"  - a\n"
		"  - {id: 1, cat: TV/HD, desc: \"x, y\"}\n"
		"  - [q, season, ep]\n"
		"  - - nested\n"
		"    - list\n"
		"same_column:\n"
		"- one\n"
		"- name: two\n"
		"  type: text\n"
		"flow: [a, \"b\", 'c',\n"
		"  d]\n"
		"\"quoted key\": 1\n"
		"$raw: \"&x=1\"\n"
		"...\n";
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisYamlNode) root = axis_yaml_parse(text, strlen(text), &problem);
	const AxisYamlNode *list;
	const AxisYamlNode *item;
	gboolean truth = TRUE;

	g_assert_null(problem);
	g_assert_nonnull(root);
	g_assert_cmpint(root->kind, ==, AXIS_YAML_MAP);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "id")), ==, "x1");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "plain")), ==, "a plain value, a colon:x and http://x");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "folded")), ==, "first line second line\nafter an empty line");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "single")), ==, "it's # not a comment");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "double")), ==,
		"tab\there \"q\" \\ / A \xc3\xa9 \xf0\x9f\x98\x80");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "multi")), ==, "one two three");
	g_assert_true(axis_yaml_is_null(axis_yaml_get(root, "empty")));
	g_assert_true(axis_yaml_is_null(axis_yaml_get(root, "tilde")));
	/* Text as written: no YAML 1.1 booleans. */
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "yes")), ==, "yes");
	g_assert_false(axis_yaml_get_bool(axis_yaml_get(root, "yes"), &truth));
	list = axis_yaml_get(root, "list");
	g_assert_cmpuint(axis_yaml_length(list), ==, 4);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_at(list, 0)), ==, "a");
	item = axis_yaml_at(list, 1);
	g_assert_cmpint(item->kind, ==, AXIS_YAML_MAP);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(item, "cat")), ==, "TV/HD");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(item, "desc")), ==, "x, y");
	g_assert_cmpuint(axis_yaml_length(axis_yaml_at(list, 2)), ==, 3);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_at(axis_yaml_at(list, 3), 1)), ==, "list");
	list = axis_yaml_get(root, "same_column");
	g_assert_cmpuint(axis_yaml_length(list), ==, 2);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(axis_yaml_at(list, 1), "type")), ==, "text");
	g_assert_cmpuint(axis_yaml_length(axis_yaml_get(root, "flow")), ==, 4);
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "quoted key")), ==, "1");
	g_assert_cmpstr(axis_yaml_text(axis_yaml_get(root, "$raw")), ==, "&x=1");
}

static void
test_yaml_refuses(void)
{
	static const struct { const gchar *text; const gchar *problem; } cases[] = {
		{ "a: &x 1\nb: *x\n", "anchors are not supported" },
		{ "a: *x\n", "aliases are not supported" },
		{ "a: !!str 1\n", "tags are not supported" },
		{ "a: |\n  text\n", "block scalars" },
		{ "a: >\n  text\n", "block scalars" },
		{ "%YAML 1.2\n---\na: 1\n", "directives" },
		{ "a: 1\n---\nb: 2\n", "more than one document" },
		{ "a: 1\na: 2\n", "a key written twice" },
		{ "a:\n\t- 1\n", "a tab in the indentation" },
		{ "a: \"\\0\"\n", "a NUL character" },
		{ "a: \"\\x00\"\n", "an escape that is not a character" },
		{ "a: \"open\n", "does not end" },
		{ "a: [1, 2\n", "does not end" },
		{ "a: b: c\n", "\": \" inside a value" },
		{ "a: 1\n  b: 2\n", "inside a value" },
		{ "a:\n  b:\n    - x\n   c: 1\n", "indented more" },
		{ "a: \"\\q\"\n", "an escape that YAML does not have" },
		{ "- a\nb: 1\n", "text after the document" },
		{ "a: 1\n... \nb: 2\n", "text after the end" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		AxisYamlNode *root = axis_yaml_parse(cases[i].text, strlen(cases[i].text), &problem);

		if (root != NULL || problem == NULL || strstr(problem, cases[i].problem) == NULL)
			g_error("case %u: expected \"%s\", got %s", i, cases[i].problem, problem != NULL ? problem : "a tree");
		g_assert_true(g_str_has_prefix(problem, "line "));
	}
	/* Not UTF-8, a NUL, empty. */
	{
		g_autofree gchar *problem = NULL;

		g_assert_null(axis_yaml_parse("a: \xff\n", 6, &problem));
		g_assert_nonnull(strstr(problem, "not UTF-8"));
		g_clear_pointer(&problem, g_free);
		g_assert_null(axis_yaml_parse("a: b\0c\n", 7, &problem));
		g_clear_pointer(&problem, g_free);
		g_assert_null(axis_yaml_parse("", 0, &problem));
	}
}

static void
test_yaml_limits(void)
{
	GString *deep = g_string_new("a: ");
	GString *wide = g_string_new("a: [");
	g_autofree gchar *big = g_malloc(AXIS_YAML_MAX_BYTES + 2);
	g_autofree gchar *problem = NULL;
	guint i;

	for (i = 0; i < AXIS_YAML_MAX_DEPTH + 4; i++)
		g_string_append_c(deep, '[');
	g_string_append(deep, "1");
	for (i = 0; i < AXIS_YAML_MAX_DEPTH + 4; i++)
		g_string_append_c(deep, ']');
	g_assert_null(axis_yaml_parse(deep->str, deep->len, &problem));
	g_assert_nonnull(strstr(problem, "deeper than the limit"));
	g_clear_pointer(&problem, g_free);
	for (i = 0; i < AXIS_YAML_MAX_NODES + 1; i++)
		g_string_append(wide, "1,");
	g_string_append(wide, "1]\n");
	g_assert_null(axis_yaml_parse(wide->str, wide->len, &problem));
	g_assert_nonnull(strstr(problem, "more values than the limit"));
	g_clear_pointer(&problem, g_free);
	memset(big, 'a', AXIS_YAML_MAX_BYTES + 1);
	big[AXIS_YAML_MAX_BYTES + 1] = '\0';
	g_assert_null(axis_yaml_parse(big, AXIS_YAML_MAX_BYTES + 1, &problem));
	g_assert_nonnull(strstr(problem, "larger than the limit"));
	g_string_free(deep, TRUE);
	g_string_free(wide, TRUE);
}

/* ── Pages and selectors ────────────────────────────────────────── */

static const AxisHtmlNode *
first_of(
	AxisHtmlDoc *doc,
	const gchar *selector_text
){
	g_autoptr(AxisSelector) selector = axis_selector_parse(selector_text, NULL, NULL);
	AxisSelectBudget budget = { 0, 1000000, FALSE };

	g_assert_nonnull(selector);
	return axis_selector_first(selector, axis_html_doc_root(doc), &budget);
}

static guint
count_of(
	AxisHtmlDoc *doc,
	const gchar *selector_text
){
	g_autoptr(AxisSelector) selector = NULL;
	g_autoptr(GPtrArray) all = NULL;
	g_autofree gchar *problem = NULL;
	AxisSelectBudget budget = { 0, 1000000, FALSE };

	selector = axis_selector_parse(selector_text, &problem, NULL);
	if (selector == NULL)
		g_error("%s: %s", selector_text, problem);
	all = axis_selector_all(selector, axis_html_doc_root(doc), 1000, &budget);
	return all->len;
}

static gchar *
text_of(
	AxisHtmlDoc *doc,
	const gchar *selector_text
){
	const AxisHtmlNode *node = first_of(doc, selector_text);

	return node != NULL ? axis_html_text(node, 4096) : NULL;
}

static void
test_html_tree(void)
{
	static const gchar page[] =
		"<!DOCTYPE html><html><head><title>A &amp; B &lt;&#x41;&#66;&copy;&bogus; &amp</title>"
		"<script>if (a < b) document.write('<tr><td>no</td></tr>');</script></head>"
		"<body><p>one<p>two<div id=d1 class='x  y'>three<br>four<img src=\"a.png\" alt=yes></div>"
		"<table class=t><tr><td>1<td>2<tr><td>3</table>"
		"<ul><li>a<li>b</ul><a href=\"/x?a=1&amp;b=2\" data-x='q\"'>link</a><!-- <a>gone</a> -->"
		"</body></html>";
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisHtmlDoc) doc = axis_html_parse(page, strlen(page), FALSE, &problem);
	g_autofree gchar *title = NULL;
	g_autofree gchar *div = NULL;
	const AxisHtmlNode *a;

	g_assert_null(problem);
	g_assert_nonnull(doc);
	title = text_of(doc, "title");
	g_assert_cmpstr(title, ==, "A & B <AB\xc2\xa9&bogus; &amp");
	/* The script's text is text. */
	g_assert_cmpuint(count_of(doc, "td"), ==, 3);
	/* <p> ends <p>; <div> ends <p>. */
	g_assert_cmpuint(count_of(doc, "body > p"), ==, 2);
	g_assert_cmpuint(count_of(doc, "p div"), ==, 0);
	div = text_of(doc, "#d1");
	g_assert_cmpstr(div, ==, "threefour");
	g_assert_cmpuint(count_of(doc, "div.x.y"), ==, 1);
	g_assert_cmpuint(count_of(doc, "div > img[alt=yes]"), ==, 1);
	/* The rows a browser makes: a tbody, a row ended by the next. */
	g_assert_cmpuint(count_of(doc, "table.t > tbody > tr"), ==, 2);
	g_assert_cmpuint(count_of(doc, "tr:first-child > td"), ==, 2);
	g_assert_cmpuint(count_of(doc, "ul > li"), ==, 2);
	a = first_of(doc, "a[data-x]");
	g_assert_nonnull(a);
	g_assert_cmpstr(axis_html_attr(a, "href"), ==, "/x?a=1&b=2");
	g_assert_cmpstr(axis_html_attr(a, "data-x"), ==, "q\"");
	g_assert_cmpuint(count_of(doc, "a"), ==, 1);
}

static void
test_html_xml(void)
{
	static const gchar feed[] =
		"<?xml version=\"1.0\"?><rss><channel><item><title><![CDATA[A <b> & c]]></title>"
		"<link>https://x.invalid/1</link><torznab:attr name=\"size\" value=\"10\"/><Enclosure url=\"u\"/>"
		"<guid>g</guid></item></channel></rss>";
	g_autoptr(AxisHtmlDoc) doc = axis_html_parse(feed, strlen(feed), TRUE, NULL);
	g_autofree gchar *title = NULL;
	g_autofree gchar *link = NULL;

	g_assert_nonnull(doc);
	title = text_of(doc, "rss > channel > item > title");
	g_assert_cmpstr(title, ==, "A <b> & c");
	/* <link> holds text in XML. */
	link = text_of(doc, "item link");
	g_assert_cmpstr(link, ==, "https://x.invalid/1");
	g_assert_cmpstr(axis_html_attr(first_of(doc, "[name=size]"), "value"), ==, "10");
	/* Empty elements end there; the guid is the item's, not the attr's. */
	g_assert_cmpuint(count_of(doc, "item > guid"), ==, 1);
	g_assert_cmpuint(count_of(doc, "enclosure"), ==, 1);
}

static void
test_html_limits(void)
{
	GString *many = g_string_new(NULL);
	GString *deep = g_string_new(NULL);
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisHtmlDoc) doc = NULL;
	guint i;

	for (i = 0; i < AXIS_HTML_MAX_NODES + 1; i++)
		g_string_append(many, "<b>");
	g_assert_null(axis_html_parse(many->str, many->len, FALSE, &problem));
	g_assert_nonnull(strstr(problem, "more elements than the limit"));
	/* Deeper than the limit: the rest beside the deepest, not refused. */
	for (i = 0; i < AXIS_HTML_MAX_DEPTH * 3; i++)
		g_string_append(deep, "<div>");
	g_string_append(deep, "<span>end</span>");
	doc = axis_html_parse(deep->str, deep->len, FALSE, NULL);
	g_assert_nonnull(doc);
	g_assert_cmpuint(count_of(doc, "span"), ==, 1);
	/* Bytes that are not UTF-8, and a NUL, become U+FFFD. */
	{
		static const gchar bad[] = "<p>a\xff" "b\0c</p>";
		g_autoptr(AxisHtmlDoc) odd = axis_html_parse(bad, sizeof(bad) - 1, FALSE, NULL);
		g_autofree gchar *text = text_of(odd, "p");

		g_assert_cmpstr(text, ==, "a\xef\xbf\xbd" "b\xef\xbf\xbd" "c");
	}
	g_string_free(many, TRUE);
	g_string_free(deep, TRUE);
}

static void
test_select_features(void)
{
	static const gchar page[] =
		"<html><body><div id=top class=\"box main\" lang=en-US>"
		"<table><tr class=r><td>a</td><td class=n>1</td><td><a href=\"/torrent/1\" title=x>T1</a></td></tr>"
		"<tr class=r><td>b</td><td class=n></td><td><a href=\"magnet:?xt=1\">M</a></td></tr>"
		"<tr class=\"r ad\"><td>c</td><td class=n>3</td><td><a href=\"/torrent/3.torrent\">T3</a></td></tr></table>"
		"<p>x</p><p>y</p><span>s</span><p>z</p></div></body></html>";
	g_autoptr(AxisHtmlDoc) doc = axis_html_parse(page, strlen(page), FALSE, NULL);
	static const struct { const gchar *selector; guint count; } cases[] = {
		{ "tr", 3 }, { "*", 24 }, { ".r", 3 }, { "#top", 1 }, { "div#top.box.main", 1 },
		{ "a[title]", 1 }, { "a[href=\"/torrent/1\"]", 1 }, { "a[href^=\"/torrent/\"]", 2 },
		{ "a[href$=\".torrent\"]", 1 }, { "a[href*=torrent]", 2 }, { "tr[class~=ad]", 1 },
		{ "div[lang|=en]", 1 }, { "a[href!=\"/torrent/1\"]", 2 }, { "A[HREF^=\"MAGNET:\" i]", 1 },
		{ "table > tbody > tr > td", 9 }, { "div td", 9 }, { "p + p", 1 }, { "p ~ p", 2 }, { "span + p", 1 },
		{ "td, p", 12 }, { "tr:has(a[href^=\"/torrent/\"])", 2 }, { "tr:has(> td.n:empty)", 1 },
		{ "tr:not(.ad)", 2 }, { "tr:not(:has(a[href^=magnet]))", 2 }, { "td:contains(\"T\")", 2 },
		{ "td:contains(T1)", 1 }, { "td:nth-child(2)", 3 }, { "tr:nth-child(odd)", 2 },
		{ "tr:nth-child(2n)", 1 }, { "tr:nth-child(-n+2)", 2 }, { "td:nth-last-child(1)", 3 },
		{ "p:nth-of-type(2)", 1 }, { "p:nth-last-of-type(1)", 1 }, { "td:first-child", 3 },
		{ "td:last-child a", 3 }, { "p:first-of-type", 1 }, { "p:last-of-type", 1 }, { "span:only-of-type", 1 },
		{ "a:only-child", 3 }, { "td:empty", 1 }, { ":root", 1 }, { "html:root > body", 1 },
		{ "p:has(+ span)", 1 }, { "p:has(~ span)", 2 }, { "td.n:not(:empty)", 2 },
	};
	guint i;

	g_assert_nonnull(doc);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		guint got = count_of(doc, cases[i].selector);

		if (got != cases[i].count)
			g_error("%s: expected %u, got %u", cases[i].selector, cases[i].count, got);
	}
	/* :scope is the element a field is read in. */
	{
		g_autoptr(AxisSelector) scope = axis_selector_parse(":scope > td.n", NULL, NULL);
		const AxisHtmlNode *row = first_of(doc, "tr.ad");
		AxisSelectBudget budget = { 0, 100000, FALSE };
		const AxisHtmlNode *cell = axis_selector_first(scope, row, &budget);
		g_autofree gchar *text = axis_html_text(cell, 100);

		g_assert_cmpstr(text, ==, "3");
	}
}

static void
test_select_refuses(void)
{
	static const struct { const gchar *text; gboolean unsupported; const gchar *problem; } cases[] = {
		{ "td:eq(1)", TRUE, ":eq()" }, { "tr:first", TRUE, ":first" }, { "a::before", TRUE, "pseudo-elements" },
		{ "td:nth-col(1)", TRUE, ":nth-col()" }, { "td:matches(x)", TRUE, ":matches()" },
		{ "a[href", FALSE, "does not end" }, { "td:nth-child(x)", FALSE, "an+b" }, { ">", FALSE, "not a selector" },
		{ "a,,b", FALSE, "not a selector" }, { "", FALSE, "empty" }, { "td:has()", FALSE, "not a selector" },
		{ ":contains(\"x)", FALSE, "does not end" },
	};
	guint i;
	GString *nested = g_string_new("a");

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		gboolean unsupported = !cases[i].unsupported;
		AxisSelector *selector = axis_selector_parse(cases[i].text, &problem, &unsupported);

		if (selector != NULL || problem == NULL || strstr(problem, cases[i].problem) == NULL ||
		    unsupported != cases[i].unsupported)
			g_error("%s: expected %s, got %s", cases[i].text, cases[i].problem, problem != NULL ? problem : "a selector");
	}
	for (i = 0; i < AXIS_SELECT_MAX_NESTING + 2; i++)
		g_string_prepend(nested, "a:not(");
	for (i = 0; i < AXIS_SELECT_MAX_NESTING + 2; i++)
		g_string_append(nested, ")");
	{
		g_autofree gchar *problem = NULL;

		g_assert_null(axis_selector_parse(nested->str, &problem, NULL));
		g_assert_nonnull(strstr(problem, "nested deeper than the limit"));
	}
	g_string_free(nested, TRUE);
}

/* A selector that would take long on a large page stops at its budget. */
static void
test_select_budget(void)
{
	GString *page = g_string_new("<html><body>");
	g_autoptr(AxisHtmlDoc) doc = NULL;
	g_autoptr(AxisSelector) selector = NULL;
	g_autoptr(GPtrArray) all = NULL;
	AxisSelectBudget budget = { 0, 2000000, FALSE };
	gint64 started;
	guint i;

	for (i = 0; i < 3000; i++)
		g_string_append(page, "<div>");
	for (i = 0; i < 3000; i++)
		g_string_append(page, "</div>");
	g_string_append(page, "</body></html>");
	doc = axis_html_parse(page->str, page->len, FALSE, NULL);
	g_assert_nonnull(doc);
	/* Each div looks through all below it, and each of those through all
	 * below it: cubic in the page. */
	selector = axis_selector_parse("div:has(div:has(div:has(p)))", NULL, NULL);
	g_assert_nonnull(selector);
	started = g_get_monotonic_time();
	all = axis_selector_all(selector, axis_html_doc_root(doc), 100, &budget);
	g_assert_true(budget.exhausted);
	g_assert_cmpuint(all->len, ==, 0);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
	g_string_free(page, TRUE);
}

/* ── Templates ──────────────────────────────────────────────────── */

static gchar *
upper_escape(const gchar *text)
{
	return g_ascii_strup(text, -1);
}

static void
test_template_renders(void)
{
	static const struct { const gchar *text; const gchar *expected; } cases[] = {
		{ "plain", "plain" },
		{ "{{ .Keywords }}", "hearth 1995" },
		{ "{{ if .Keywords }}k={{ .Keywords }}{{ else }}latest{{ end }}", "k=hearth 1995" },
		{ "{{ if .Config.empty }}a{{ else if .Config.sort }}b{{ else }}c{{ end }}", "b" },
		{ "{{ if and .Keywords (eq .Config.sort \"added\") }}yes{{ end }}", "yes" },
		{ "{{ if or .Config.empty .Config.none }}yes{{ else }}no{{ end }}", "no" },
		{ "{{ or .Config.empty .Config.sort }}", "added" },
		{ "{{ and .Keywords .Config.sort }}", "added" },
		{ "{{ if not .Config.empty }}not{{ end }}", "not" },
		{ "{{ if ne .Config.sort \"seeds\" }}ne{{ end }}", "ne" },
		{ "{{ if eq .Config.sort \"x\" \"y\" \"added\" }}any{{ end }}", "any" },
		{ "{{ if eq .Config.box .False }}off{{ else }}on{{ end }}", "off" },
		{ "{{ if .True }}t{{ end }}{{ if .False }}f{{ end }}", "t" },
		{ "{{ join .Categories \",\" }}", "1,2" },
		{ "{{ range .Categories }}c[]={{ . }}&{{ end }}", "c[]=1&c[]=2&" },
		{ "{{ range .Config.empty }}x{{ else }}none{{ end }}", "none" },
		{ "{{ re_replace .Keywords \"(\\d+)\" \"[$1]\" }}", "hearth [1995]" },
		{ "{{ re_replace .Keywords \"\\b(\\d)\" \"\\\"$1\" }}", "hearth \"1995" },
		{ "{{ .Query.Missing }}|{{ .Result.title }}", "|T" },
		{ "a {{- \" b \" -}} c", "a b c" },
		{ "{{ .Today.Year }}", "2023" },
		{ "{{/* a comment */}}x", "x" },
	};
	g_autoptr(GHashTable) values = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GPtrArray) categories = g_ptr_array_new();
	g_autoptr(AxisDefRegexCache) regex = axis_def_regex_cache_new(0);
	AxisTemplateVars vars;
	guint i;

	g_hash_table_insert(values, "Keywords", "hearth 1995");
	g_hash_table_insert(values, "Config.sort", "added");
	g_hash_table_insert(values, "Config.empty", "");
	g_hash_table_insert(values, "Config.box", "");
	g_hash_table_insert(values, "Result.title", "T");
	g_hash_table_insert(values, "Today.Year", "2023");
	g_ptr_array_add(categories, "1");
	g_ptr_array_add(categories, "2");
	vars.values = values;
	vars.categories = categories;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		g_autofree gchar *out = axis_template_expand(cases[i].text, &vars, NULL, regex, &problem);

		if (g_strcmp0(out, cases[i].expected) != 0)
			g_error("%s: expected %s, got %s (%s)", cases[i].text, cases[i].expected, out != NULL ? out : "nothing",
				problem != NULL ? problem : "");
	}
	/* The escape is applied to what actions put in, not to the text. */
	{
		g_autofree gchar *out = axis_template_expand("search/{{ .Keywords }}/x", &vars, upper_escape, regex, NULL);

		g_assert_cmpstr(out, ==, "search/HEARTH 1995/x");
	}
}

static void
test_template_refuses(void)
{
	static const struct { const gchar *text; gboolean unsupported; const gchar *problem; } cases[] = {
		{ "{{ $x := 1 }}", TRUE, "template variables" }, { "{{ .Keywords | len }}", TRUE, "pipes" },
		{ "{{ len .Keywords }}", TRUE, "function len" }, { "{{ with .Keywords }}x{{ end }}", TRUE, "keyword with" },
		{ "{{ .DownloadUri.AbsolutePath }}", TRUE, "variable .DownloadUri" },
		{ "{{ printf \"%s\" .Keywords }}", TRUE, "function printf" }, { "{{ if .Keywords }}x", FALSE, "without its end" },
		{ "{{ end }}", FALSE, "end without its if" }, { "{{ else }}", FALSE, "else without its if" },
		{ "{{ .Keywords", FALSE, "does not end" }, { "{{ eq .Keywords }}", FALSE, "wrong number" },
		{ "{{ \"open }}", FALSE, "does not end" }, { "{{ (.Keywords }}", FALSE, "does not end" },
		{ "{{ .Keywords .Keywords }}", FALSE, "no function" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		gboolean unsupported = !cases[i].unsupported;
		AxisTemplate *t = axis_template_parse(cases[i].text, &problem, &unsupported);

		if (t != NULL || problem == NULL || strstr(problem, cases[i].problem) == NULL ||
		    unsupported != cases[i].unsupported)
			g_error("%s: expected %s, got %s", cases[i].text, cases[i].problem, problem != NULL ? problem : "a template");
	}
}

/* ── Filters ────────────────────────────────────────────────────── */

static void
test_filters(void)
{
	static const struct {
		const gchar *name;
		const gchar *args[3];
		const gchar *value;
		const gchar *expected;
	} cases[] = {
		{ "replace", { "_", " ", NULL }, "a_b_c", "a b c" },
		{ "re_replace", { "(\\w+)-(\\w+)", "$2+$1", NULL }, "ab-cd ef-gh", "cd+ab gh+ef" },
		{ "re_replace", { "x", "${1}$$\\", NULL }, "axb", "a${1}$\\b" },
		{ "re_replace", { "(?<y>\\d{4})", "[${y}]", NULL }, "Hearth 1995", "Hearth [1995]" },
		{ "re_replace", { "(\\d)", "$0$1$10", NULL }, "a1", "a111" "0" },
		{ "re_replace", { "\\p{IsCyrillic}+", "", NULL }, "\xd0\x96\xd0\xb0\xd1\x80\xd0\xb0 1995", " 1995" },
		{ "re_replace", { "[\\s\\u00A0]+", " ", NULL }, "a\xc2\xa0\xc2\xa0" "b", "a b" },
		{ "regexp", { "(\\d+)p", NULL }, "Hearth 1080p", "1080" },
		{ "regexp", { "nothing", NULL }, "Hearth", "" },
		{ "split", { "/", "2", NULL }, "/a/b/c", "b" },
		{ "split", { "=", "-1", NULL }, "x?cat=5", "5" },
		{ "split", { "/", "9", NULL }, "a/b", "" },
		{ "trim", { NULL }, "  a b  ", "a b" },
		{ "trim", { "/", NULL }, "/a/", "a" },
		{ "append", { " +03:00", NULL }, "x", "x +03:00" },
		{ "prepend", { "/dl?", NULL }, "x", "/dl?x" },
		{ "tolower", { NULL }, "\xc3\x89T\xc3\x89", "\xc3\xa9t\xc3\xa9" },
		{ "toupper", { NULL }, "abc", "ABC" },
		{ "urldecode", { NULL }, "a+b%2Fc%C3%A9", "a b/c\xc3\xa9" },
		{ "urlencode", { NULL }, "a b/c\xc3\xa9", "a+b%2Fc%C3%A9" },
		{ "querystring", { "id", NULL }, "/details.php?x=1&id=42&y", "42" },
		{ "querystring", { "zz", NULL }, "/details.php?x=1", "" },
		{ "dateparse", { "yyyy-MM-dd HH:mm zzz", NULL }, "2023-11-01 20:15 +02:00", "2023-11-01T18:15:00Z" },
		{ "dateparse", { "ddd, dd MMM yyyy HH:mm:ss zzz", NULL }, "Wed, 01 Nov 2023 12:00:00 +0000",
		  "2023-11-01T12:00:00Z" },
		{ "dateparse", { "MMM d, yyyy, h:mm tt zzz", NULL }, "Nov 5, 2023, 7:45 pm -05:00", "2023-11-06T00:45:00Z" },
		{ "dateparse", { "dd.MM.yyyy", NULL }, "31.12.2020", "2020-12-31T00:00:00Z" },
		{ "dateparse", { "htt MMM. d", NULL }, "7am Sep. 14", "2023-09-14T07:00:00Z" },
		{ "dateparse", { "MMM. d yy", NULL }, "Apr. 18 11", "2011-04-18T00:00:00Z" },
		{ "dateparse", { "d MMMM yyyy", NULL }, "3 March 2021", "2021-03-03T00:00:00Z" },
		{ "dateparse", { "yyyy-MM-dd", NULL }, "not a date", "" },
		{ "timeago", { NULL }, "2 hours ago", "2023-11-14T20:13:20Z" },
		{ "timeago", { NULL }, "1 day, 3 hours ago", "2023-11-13T19:13:20Z" },
		{ "timeago", { NULL }, "an hour ago", "2023-11-14T21:13:20Z" },
		{ "reltime", { NULL }, "yesterday", "2023-11-13T22:13:20Z" },
		{ "fuzzytime", { NULL }, "Today 12:25am", "2023-11-14T00:25:00Z" },
		{ "fuzzytime", { NULL }, "Yesterday 10:30 pm", "2023-11-13T22:30:00Z" },
		{ "fuzzytime", { NULL }, "5 mins ago", "2023-11-14T22:08:20Z" },
		{ "fuzzytime", { NULL }, "2020-01-02", "2020-01-02T00:00:00Z" },
		{ "validfilename", { NULL }, "a/b:c*d", "a_b_c_d" },
		{ "htmldecode", { NULL }, "a &amp; b &#233;", "a & b \xc3\xa9" },
		{ "htmlencode", { NULL }, "<a & 'b'>", "&lt;a &amp; &#39;b&#39;&gt;" },
		{ "diacritics", { NULL }, "Aur\xc3\xa9lie", "Aurelie" },
		{ "base64decode", { NULL }, "SGVhcnRo", "Hearth" },
		{ "base64encode", { NULL }, "Hearth", "SGVhcnRo" },
		{ "strdump", { NULL }, "same", "same" },
	};
	g_autoptr(AxisDefRegexCache) regex = axis_def_regex_cache_new(0);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		g_autofree gchar *out = axis_def_filter_apply(cases[i].name, cases[i].args, cases[i].value, NOW_US, regex,
			&problem);

		if (g_strcmp0(out, cases[i].expected) != 0)
			g_error("%s(%s): expected %s, got %s (%s)", cases[i].name, cases[i].value, cases[i].expected,
				out != NULL ? out : "nothing", problem != NULL ? problem : "");
	}
	/* Unknown filters, and a filter without its arguments, fail. */
	{
		const gchar *none[] = { NULL };
		g_autofree gchar *problem = NULL;

		g_assert_null(axis_def_filter_apply("jsonjoinarray", none, "x", NOW_US, regex, &problem));
		g_assert_nonnull(problem);
		g_clear_pointer(&problem, g_free);
		g_assert_null(axis_def_filter_apply("replace", none, "x", NOW_US, regex, &problem));
		g_assert_nonnull(problem);
		g_assert_false(axis_def_filter_known("hexdump"));
	}
}

static void
test_values(void)
{
	static const struct { const gchar *text; gint64 bytes; } sizes[] = {
		{ "1.5 GB", 1610612736 }, { "700 MB", 734003200 }, { "1,234.5 KB", 1264128 }, { "1,5 GB", 1610612736 },
		{ "1,500 MB", 1572864000 }, { "1.2 GiB", 1288490189 }, { "123456", 123456 }, { "512 B", 512 },
		{ "2 TB", G_GINT64_CONSTANT(2199023255552) }, { "8.5\xc2\xa0GB", 9126805504 },
		{ "1,5 \xd0\x93\xd0\x91", 1610612736 }, { "big", -1 }, { "5 parsecs", -1 }, { "", -1 },
	};
	static const struct { const gchar *text; gint64 count; } counts[] = {
		{ "1,234", 1234 }, { "12", 12 }, { " 7 ", 7 }, { "-5", -1 }, { "none", -1 }, { "1 234", 1234 },
	};
	static const struct { const gchar *text; gint64 seconds; } dates[] = {
		{ "1690000000", 1690000000 }, { "1690000000123", 1690000000 }, { "2023-11-01T12:00:00Z", 1698840000 },
		{ "2023-11-01T14:00:00+02:00", 1698840000 }, { "Wed, 01 Nov 2023 12:00:00 GMT", 1698840000 },
		{ "2023-11-01 12:00:00", 1698840000 }, { "2023-11-01", 1698796800 }, { "01.11.2023", 1698796800 },
		{ "now", 1700000000 }, { "2 hours ago", 1699992800 }, { "yesterday", 1699913600 },
		{ "not a date", 0 }, { "99999999999", 0 }, { "", 0 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(sizes); i++) {
		if (axis_def_parse_size(sizes[i].text) != sizes[i].bytes)
			g_error("size %s: expected %" G_GINT64_FORMAT ", got %" G_GINT64_FORMAT, sizes[i].text, sizes[i].bytes,
				axis_def_parse_size(sizes[i].text));
	}
	for (i = 0; i < G_N_ELEMENTS(counts); i++)
		g_assert_cmpint(axis_def_parse_count(counts[i].text), ==, counts[i].count);
	for (i = 0; i < G_N_ELEMENTS(dates); i++) {
		if (axis_def_parse_date(dates[i].text, NOW_US) != dates[i].seconds * G_USEC_PER_SEC)
			g_error("date %s: expected %" G_GINT64_FORMAT ", got %" G_GINT64_FORMAT, dates[i].text,
				dates[i].seconds, axis_def_parse_date(dates[i].text, NOW_US) / G_USEC_PER_SEC);
	}
}

/* ── Regular expressions ────────────────────────────────────────── */

/*
 * Spec: what bounds a pathological pattern.  "(a+)+$" against a long run
 * of "a" and a "!" backtracks exponentially; each attempt stops at
 * PCRE2's match limit set in front of the pattern, so the call ends with a
 * failure, soon.  Then the answer's time budget: once spent, every match
 * fails at once.
 */
static void
test_regex_bounds(void)
{
	g_autoptr(AxisDefRegexCache) regex = axis_def_regex_cache_new(0);
	g_autoptr(AxisDefRegexCache) small = axis_def_regex_cache_new(1);
	GString *subject = g_string_new(NULL);
	g_autofree gchar *problem = NULL;
	gchar *out;
	gint64 started;
	gint64 took;
	guint i;

	for (i = 0; i < 4000; i++)
		g_string_append_c(subject, 'a');
	g_string_append_c(subject, '!');
	started = g_get_monotonic_time();
	out = axis_def_regex_replace(regex, "(a+)+$", subject->str, "x", &problem);
	took = g_get_monotonic_time() - started;
	g_test_message("a pathological pattern on %" G_GSIZE_FORMAT " bytes ended in %" G_GINT64_FORMAT " ms: %s",
		subject->len, took / 1000, problem != NULL ? problem : "matched");
	g_assert_null(out);
	g_assert_nonnull(strstr(problem, "past its limits"));
	g_assert_cmpint(took, <, 5 * G_USEC_PER_SEC);
	g_clear_pointer(&problem, g_free);
	/* A subject is cut to the limit first. */
	g_string_truncate(subject, 0);
	for (i = 0; i < AXIS_DEF_REGEX_MAX_SUBJECT + 100; i++)
		g_string_append_c(subject, 'b');
	out = axis_def_regex_replace(regex, "b", subject->str, "c", NULL);
	g_assert_cmpuint(strlen(out), ==, AXIS_DEF_REGEX_MAX_SUBJECT);
	g_free(out);
	/* The budget: once spent, nothing more runs. */
	g_string_truncate(subject, 0);
	for (i = 0; i < 3000; i++)
		g_string_append_c(subject, 'a');
	g_string_append_c(subject, '!');
	g_assert_null(axis_def_regex_replace(small, "(a+)+$", subject->str, "x", NULL));
	g_assert_true(axis_def_regex_cache_exhausted(small));
	g_assert_null(axis_def_regex_replace(small, "a", "a", "b", &problem));
	g_assert_nonnull(strstr(problem, "longer than the limit"));
	g_clear_pointer(&problem, g_free);
	/* Verbs and options of PCRE2's own, and \C, are refused. */
	g_assert_false(axis_def_regex_check("(*LIMIT_MATCH=100000000)a", &problem));
	g_clear_pointer(&problem, g_free);
	g_assert_false(axis_def_regex_check("a(*SKIP)b", NULL));
	g_assert_false(axis_def_regex_check("a\\Cb", NULL));
	g_assert_false(axis_def_regex_check("(unclosed", NULL));
	g_assert_true(axis_def_regex_check("(?i)(?<![\\]\\)])\\b(S\\d{2})(?=E)", NULL));
	g_string_free(subject, TRUE);
}

/* ── Judging definitions ────────────────────────────────────────── */

static void
test_fixtures_usable(void)
{
	static const gchar *const names[] = { "example-html.yml", "example-json.yml", "example-rss.yml",
		"example-after.yml", "hostile-links.yml", NULL };
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		g_autoptr(AxisDef) def = load_fixture(names[i]);

		assert_usable(def);
	}
	{
		g_autoptr(AxisDef) def = load_fixture("example-html.yml");

		g_assert_cmpstr(def->id, ==, "example-html");
		g_assert_cmpstr(def->name, ==, "Example HTML");
		g_assert_true(def->covers_movies);
		g_assert_true(def->covers_tv);
		g_assert_false(def->general);
		g_assert_cmpuint(def->links->len, ==, 1);
		g_assert_cmpuint(def->legacy_links->len, ==, 1);
		g_assert_cmpfloat(def->request_delay, ==, 1.0);
		g_assert_true(def->rows_andmatch);
		g_assert_true(axis_def_download_is_page(def));
		g_assert_cmpstr(axis_def_setting(def, "magnets")->value, ==, "True");
		g_assert_cmpstr(axis_def_setting(def, "sort")->value, ==, "added");
		g_assert_true(axis_def_has_link(def, "https://old.example-html.invalid/x"));
		g_assert_false(axis_def_has_link(def, "https://example-html.invalid:8443/"));
		g_assert_cmpint(axis_def_newznab_category("Movies/HD"), ==, 2040);
		g_assert_cmpint(axis_def_newznab_category("TV/Anime"), ==, 5070);
		g_assert_cmpint(axis_def_newznab_category("Nonsense"), ==, 0);
	}
}

/* A small usable definition, to change one thing in at a time. */
static const gchar base_def[] =
	"---\n"
	"id: base\n"
	"name: Base\n"
	"description: a test\n"
	"language: en-US\n"
	"type: public\n"
	"encoding: UTF-8\n"
	"links:\n"
	"  - https://base.example.invalid/\n"
	"caps:\n"
	"  categorymappings:\n"
	"    - {id: 1, cat: Movies, desc: Movies}\n"
	"  modes:\n"
	"    search: [q]\n"
	"@SETTINGS@"
	"search:\n"
	"  paths:\n"
	"    - path: @PATH@\n"
	"@SEARCH@"
	"  rows:\n"
	"    selector: @ROWS@\n"
	"  fields:\n"
	"    category:\n"
	"      text: 1\n"
	"    title:\n"
	"      selector: td\n"
	"@FIELDS@"
	"    download:\n"
	"      selector: a\n"
	"      attribute: href\n"
	"    size:\n"
	"      text: 1 GB\n"
	"    seeders:\n"
	"      text: 1\n"
	"@ROOT@";

static gchar *
variant(
	const gchar *what,
	const gchar *text
){
	static const gchar *const marks[] = { "@SETTINGS@", "@PATH@", "@SEARCH@", "@ROWS@", "@FIELDS@", "@ROOT@", NULL };
	static const gchar *const defaults[] = { "", "/search", "", "tr", "", "", NULL };
	gchar *out = g_strdup(base_def);
	guint i;

	for (i = 0; marks[i] != NULL; i++) {
		gchar *next = replaced(out, marks[i], strcmp(marks[i], what) == 0 ? text : defaults[i]);

		g_free(out);
		out = next;
	}
	return out;
}

/* Spec: Unsupported feature: the definition is not usable, naming it;
 * task 4.2 */
static void
test_not_usable(void)
{
	static const struct { const gchar *where; const gchar *text; const gchar *need; } cases[] = {
		{ "@ROOT@", "login:\n  method: form\n  path: /login\n", "a login" },
		{ "@ROOT@", "certificates:\n  - abc\n", "certificates of its own" },
		{ "@ROOT@", "download:\n  before:\n    path: /x\n", "download.before" },
		{ "@ROOT@", "download:\n  method: post\n", "downloads sent as a form" },
		{ "@ROOT@", "newthing: 1\n", "the definition key newthing" },
		{ "@ROOT@", "encoding2: x\n", "the definition key encoding2" },
		{ "@PATH@", "https://elsewhere.invalid/search", "not one of its site's" },
		{ "@PATH@", "\"https://{{ .Config.apiurl }}/search\"", "builds from a setting" },
		{ "@PATH@", "/s\n      method: post", "searches sent as a form (POST)" },
		{ "@PATH@", "/s\n      method: \"{{ if .Keywords }}post{{ else }}get{{ end }}\"", "POST" },
		{ "@PATH@", "/s\n      response:\n        type: csv", "answers of the type csv" },
		{ "@PATH@", "/s\n      response:\n        type: json\n        noResultsMessage: x", "noResultsMessage" },
		{ "@ROWS@", "tr\n    dateheaders:\n      selector: td", "dateheaders" },
		{ "@ROWS@", "tr\n    multiple: true\n    attribute: torrents", "rows.multiple" },
		{ "@ROWS@", "tr\n    filters:\n      - name: strangefilter", "the row filter strangefilter" },
		{ "@ROWS@", "\"tr:eq(1)\"", "the selector :eq()" },
		{ "@ROWS@", "\"tr::before\"", "pseudo-elements" },
		{ "@FIELDS@", "    date:\n      selector: td\n      filters:\n        - name: jsonjoinarray\n",
		  "the filter jsonjoinarray" },
		{ "@FIELDS@", "    date:\n      text: \"{{ len .Keywords }}\"\n", "the template function len" },
		{ "@FIELDS@", "    date:\n      text: \"{{ .DownloadUri.Query }}\"\n", "the template variable .DownloadUri" },
		{ "@FIELDS@", "    date:\n      text: \"{{ .Keywords | x }}\"\n", "template pipes" },
		{ "@FIELDS@", "    date:\n      selector: td\n      filters:\n        - name: re_replace\n"
		  "          args: [\"(unclosed\", \"\"]\n", "regular expressions Axis can run" },
		{ "@FIELDS@", "    title|append:\n      text: x\n", "field modifiers" },
		{ "@FIELDS@", "    date:\n      selector: td\n      weird: 1\n", "the definition key weird in a field" },
		{ "@SEARCH@", "  preprocessingfilters:\n    - name: replace\n", "preprocessingfilters" },
		{ "@SEARCH@", "  headers:\n    X-Forwarded-For: [\"1.2.3.4\"]\n", "the request header X-Forwarded-For" },
		{ "@SETTINGS@", "settings:\n  - name: apikey\n    type: text\n    label: API Key\n", "typed in" },
		{ "@SETTINGS@", "settings:\n  - name: secret\n    type: password\n    label: Password\n", "typed in" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *text = variant(cases[i].where, cases[i].text);
		g_autoptr(AxisDef) def = load_ok(text);
		g_autofree gchar *problem = axis_def_problem(def);

		if (axis_def_usable(def) || problem == NULL || strstr(problem, cases[i].need) == NULL)
			g_error("case %u: expected a need of \"%s\", got %s", i, cases[i].need,
				problem != NULL ? problem : "usable");
	}
	/* The base itself is usable; one private, one for music only, are not. */
	{
		g_autofree gchar *text = variant("", "");
		g_autoptr(AxisDef) def = load_ok(text);
		g_autofree gchar *private_text = replaced(text, "type: public", "type: private");
		g_autoptr(AxisDef) private_def = load_ok(private_text);
		g_autofree gchar *music = replaced(text, "cat: Movies", "cat: Audio/MP3");
		g_autoptr(AxisDef) music_def = load_ok(music);
		g_autofree gchar *general = replaced(text, "cat: Movies", "cat: Other");
		g_autoptr(AxisDef) general_def = load_ok(general);
		g_autofree gchar *problem = NULL;

		assert_usable(def);
		g_assert_false(axis_def_usable(private_def));
		problem = axis_def_problem(private_def);
		g_assert_nonnull(strstr(problem, "an account"));
		g_clear_pointer(&problem, g_free);
		g_assert_false(axis_def_usable(music_def));
		problem = axis_def_problem(music_def);
		g_assert_nonnull(strstr(problem, "nothing Axis would search it for"));
		assert_usable(general_def);
		g_assert_true(general_def->general);
	}
}

static void
test_not_definitions(void)
{
	static const struct { const gchar *text; const gchar *problem; } cases[] = {
		{ "id: x\nname: x\nlinks: [https://a.invalid/]\nsearch:\n  a: &y 1\n", "anchors" },
		{ "- a list\n", "not an indexer definition" },
		{ "name: no id\nlinks: [https://a.invalid/]\nsearch: {}\n", "no id" },
		{ "id: \"bad id!\"\nname: x\nlinks: [https://a.invalid/]\nsearch: {}\n", "no id" },
		{ "id: x\nname: x\nsearch: {}\n", "links" },
		{ "id: x\nname: x\nlinks: [ftp://a.invalid/]\nsearch: {}\n", "links" },
		{ "id: x\nname: x\nlinks: [\"https://user:pw@a.invalid/\"]\nsearch: {}\n", "links" },
		{ "id: x\nname: x\nlinks: [https://a.invalid/]\n", "no search" },
		{ "id: x\nname: x\nlinks: [https://a.invalid/]\nsearch: {}\n---\nid: y\n", "more than one document" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *problem = NULL;
		AxisDef *def = axis_def_load(cases[i].text, strlen(cases[i].text), &problem);

		if (def != NULL || problem == NULL || strstr(problem, cases[i].problem) == NULL)
			g_error("case %u: expected %s, got %s", i, cases[i].problem, problem != NULL ? problem : "a definition");
	}
}

/* ── The engine ─────────────────────────────────────────────────── */

typedef struct
{
	AxisDef        *def;
	AxisHttpOrigin  origin;
	AxisDefSite     site;
} Site;

static void
site_open(
	Site        *site,
	AxisDef     *def,
	const gchar *address
){
	memset(site, 0, sizeof(*site));
	site->def = def;
	g_assert_true(axis_http_origin_parse(address, &site->origin, NULL));
	site->site.def = def;
	site->site.origin = &site->origin;
	site->site.base_path = "";
	site->site.now_us = NOW_US;
}

static void
site_close(Site *site)
{
	axis_http_origin_clear(&site->origin);
	axis_def_free(site->def);
}

static AxisDefSearch *
search_ok(
	Site                     *site,
	const AxisIndexerRequest *request
){
	g_autofree gchar *problem = NULL;
	AxisDefSearch *search = axis_def_search_new(&site->site, request, &problem);

	if (search == NULL)
		g_error("no search: %s", problem);
	return search;
}

static void
read_ok(
	AxisDefSearch      *search,
	const gchar        *body,
	gsize               length,
	AxisIndexerOutcome *outcome
){
	g_autofree gchar *problem = NULL;

	axis_indexer_outcome_init(outcome);
	if (!axis_def_search_read(search, 0, body, length, NULL, outcome, &problem))
		g_error("not read: %s", problem);
}

static const AxisIndexerFound *
found_named(
	const AxisIndexerOutcome *outcome,
	const gchar              *name
){
	guint i;

	for (i = 0; i < outcome->items->len; i++) {
		const AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		if (g_strcmp0(found->release->name, name) == 0)
			return found;
	}
	g_error("no release named %s", name);
	return NULL;
}

static void
movie_request(
	AxisIndexerRequest *request,
	const gchar        *title,
	gint                year,
	const gchar        *imdb
){
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = title;
	request->year = year;
	request->imdb_id = imdb;
}

/* Task 4.1, with hand-written fixtures: an HTML site's request, its
 * releases and its release page. */
static void
test_engine_html(void)
{
	g_autofree gchar *page = fixture("example-html-search.html");
	g_autofree gchar *details = fixture("example-html-details.html");
	g_autoptr(AxisDefSearch) search = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *problem = NULL;
	const AxisDefRequest *asked;
	const AxisIndexerFound *found;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	site_open(&site, load_fixture("example-html.yml"), "https://example-html.invalid");
	movie_request(&request, "Hearth", 1995, NULL);
	search = search_ok(&site, &request);
	g_assert_cmpuint(axis_def_search_n_requests(search), ==, 1);
	asked = axis_def_search_request(search, 0);
	g_assert_cmpstr(asked->path, ==, "/search/hearth+1995/added/1/?cat=1%2C2");
	g_assert_cmpint(asked->response, ==, AXIS_DEF_HTML);
	g_assert_cmpstr(asked->headers[0], ==, "Referer");
	g_assert_cmpstr(asked->headers[1], ==, "https://example-html.invalid/");
	g_assert_null(asked->headers[2]);
	g_assert_cmpint(axis_def_search_mode(search), ==, AXIS_INDEXER_BY_TEXT);

	read_ok(search, page, strlen(page), &outcome);
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_skipped, ==, 1);
	found = found_named(&outcome, "Hearth 1995 1080p BluRay x264-GROUP");
	g_assert_cmpstr(found->fetch_path, ==, "/torrent/101/hearth-1995");
	g_assert_null(found->magnet);
	g_assert_cmpint(found->release->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(9126805504));
	g_assert_cmpint(found->release->seeders, ==, 1234);
	g_assert_cmpint(found->release->leechers, ==, 56);
	g_assert_cmpint(found->release->grabs, ==, 77);
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1698862500) * G_USEC_PER_SEC);
	g_assert_cmpuint(found->release->n_categories, ==, 1);
	g_assert_cmpint(found->release->categories[0], ==, 2040);
	g_assert_nonnull(found->merge_key);
	found = found_named(&outcome, "Hearth 1995 DVDRip");
	g_assert_cmpstr(found->fetch_path, ==, "/torrent/102/hearth-1995-dvdrip");
	g_assert_cmpint(found->release->size_bytes, ==, 734003200);
	g_assert_cmpint(found->release->seeders, ==, 12);
	g_assert_cmpint(found->release->leechers, ==, 3);
	g_assert_cmpint(found->release->grabs, ==, 0);
	g_assert_cmpint(found->release->categories[0], ==, 2030);
	axis_indexer_outcome_clear(&outcome);

	/* The release's page: the first selector names another site, the
	 * second gives the magnet link. */
	g_assert_true(axis_def_read_download(&site.site, "/torrent/101/hearth-1995", details, strlen(details), &magnet,
		&file, &problem));
	g_assert_null(file);
	g_assert_true(g_str_has_prefix(magnet, "magnet:?xt=urn:btih:0123456789ABCDEF"));
	site_close(&site);
}

/*
 * add-anime-support, A3: the words a search asks for an anime series (its
 * name and an absolute number) are asked of a site in its TV categories, its
 * anime one among them (5000 counts 5070 in it); an episode's and a season's
 * request are what they were, and words not asked for an anime series go to
 * every category, as before.
 * Spec (indexers delta): An anime search includes the anime category; A
 * standard series.
 */
static void
test_engine_anime(void)
{
	g_autoptr(AxisDefSearch) words = NULL;
	g_autoptr(AxisDefSearch) plain = NULL;
	g_autoptr(AxisDefSearch) episode = NULL;
	g_autoptr(AxisDefSearch) episode_anime = NULL;
	AxisIndexerRequest request;
	Site site;

	site_open(&site, load_fixture("example-html.yml"), "https://example-html.invalid");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = "Tenkuu Rangers 137";
	plain = search_ok(&site, &request);
	g_assert_cmpuint(axis_def_search_n_requests(plain), ==, 1);
	/* (No category of its own: the site's path names none.) */
	g_assert_cmpstr(axis_def_search_request(plain, 0)->path, ==, "/search/tenkuu+rangers+137/added/1/");
	request.anime = TRUE;
	words = search_ok(&site, &request);
	g_assert_cmpuint(axis_def_search_n_requests(words), ==, 1);
	g_assert_cmpstr(axis_def_search_request(words, 0)->path, ==, "/search/tenkuu+rangers+137/added/1/?cat=3");

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "Tenkuu Rangers";
	request.season = 6;
	request.episode = 12;
	episode = search_ok(&site, &request);
	request.anime = TRUE;
	episode_anime = search_ok(&site, &request);
	g_assert_cmpstr(axis_def_search_request(episode, 0)->path, ==, axis_def_search_request(episode_anime, 0)->path);
	g_assert_true(g_str_has_suffix(axis_def_search_request(episode, 0)->path, "?cat=3"));
	site_close(&site);
}

static void
test_engine_json(void)
{
	g_autofree gchar *answer = fixture("example-json.json");
	g_autoptr(AxisDefSearch) search = NULL;
	g_autofree gchar *hash = NULL;
	const AxisIndexerFound *found;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	site_open(&site, load_fixture("example-json.yml"), "https://api.example-json.invalid");
	movie_request(&request, "Hearth", 1995, "tt0600001");
	search = search_ok(&site, &request);
	g_assert_cmpstr(axis_def_search_request(search, 0)->path, ==, "/api/v1/search?query=tt0600001&type=movies&limit=50");
	g_assert_cmpint(axis_def_search_mode(search), ==, AXIS_INDEXER_BY_IMDB);
	read_ok(search, answer, strlen(answer), &outcome);
	/* The row without its hash (a field that is not optional) is lost, as
	 * is the one that is not an object. */
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_unusable, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, 2);
	found = found_named(&outcome, "Hearth 1995 2160p WEB-DL DDP5.1 HEVC");
	g_assert_cmpstr(found->release->info_hash, ==, "abcdefabcdefabcdefabcdefabcdefabcdefabcd");
	g_assert_true(found->release->has_magnet);
	g_assert_true(axis_download_magnet_parse(found->magnet, &hash, NULL));
	g_assert_cmpstr(hash, ==, "abcdefabcdefabcdefabcdefabcdefabcdefabcd");
	g_assert_null(found->fetch_path);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(12345678901));
	g_assert_cmpint(found->release->seeders, ==, 10);
	g_assert_cmpint(found->release->leechers, ==, 2);
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1690000000) * G_USEC_PER_SEC);
	g_assert_cmpstr(found->release->imdb_id, ==, "tt0600001");
	g_assert_cmpint(found->release->categories[0], ==, 2000);
	/* The odd one: what cannot be believed is left unstated. */
	found = g_ptr_array_index(outcome.items, 1);
	g_assert_true(g_utf8_validate(found->release->name, -1, NULL));
	g_assert_null(strchr(found->release->name, '\n'));
	g_assert_cmpint(found->release->size_bytes, ==, -1);
	g_assert_cmpint(found->release->seeders, ==, -1);
	g_assert_cmpint(found->release->published_us, ==, 0);
	g_assert_null(found->release->imdb_id);
	g_assert_cmpint(found->release->categories[0], ==, 5000);
	axis_indexer_outcome_clear(&outcome);
	site_close(&site);
}

static void
test_engine_rss(void)
{
	g_autofree gchar *feed = fixture("example-rss.xml");
	g_autoptr(AxisDefSearch) search = NULL;
	const AxisIndexerFound *found;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	site_open(&site, load_fixture("example-rss.yml"), "https://feed.example-rss.invalid");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.season = 1;
	request.episode = 2;
	search = search_ok(&site, &request);
	g_assert_cmpstr(axis_def_search_request(search, 0)->path, ==,
		"/rss/search.xml?q=The+Show+S01E02&c[]=tv&c[]=tvhd");
	g_assert_cmpint(axis_def_search_request(search, 0)->response, ==, AXIS_DEF_XML);
	read_ok(search, feed, strlen(feed), &outcome);
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_off_origin, ==, 1);
	found = found_named(&outcome, "The Show S01E02 720p HDTV x264 & more");
	g_assert_cmpstr(found->fetch_path, ==, "/get/1.torrent");
	g_assert_cmpint(found->release->size_bytes, ==, 1234567890);
	g_assert_cmpint(found->release->seeders, ==, 42);
	g_assert_cmpint(found->release->leechers, ==, 7);
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1698840000) * G_USEC_PER_SEC);
	g_assert_cmpint(found->release->categories[0], ==, 5040);
	found = found_named(&outcome, "The Show S01E02 SD");
	g_assert_cmpstr(found->fetch_path, ==, "/get/3.torrent");
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1698931800) * G_USEC_PER_SEC);
	g_assert_cmpint(found->release->categories[0], ==, 5000);
	axis_indexer_outcome_clear(&outcome);
	site_close(&site);
}

static void
test_engine_after_and_encoding(void)
{
	g_autofree gchar *utf8 = fixture("example-after.html");
	g_autofree gchar *page = NULL;
	g_autoptr(AxisDefSearch) search = NULL;
	gsize length = 0;
	const AxisIndexerFound *found;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	page = g_convert(utf8, -1, "WINDOWS-1251", "UTF-8", NULL, &length, NULL);
	g_assert_nonnull(page);
	site_open(&site, load_fixture("example-after.yml"), "https://two.example-after.invalid");
	movie_request(&request, "\xd0\x96\xd0\xb0\xd1\x80\xd0\xb0", 1995, NULL);
	search = search_ok(&site, &request);
	g_assert_cmpstr(axis_def_search_request(search, 0)->path, ==, "/browse.php?search=%C6%E0%F0%E0+1995");
	read_ok(search, page, length, &outcome);
	g_assert_cmpuint(outcome.items->len, ==, 2);
	found = found_named(&outcome, "\xd0\x96\xd0\xb0\xd1\x80\xd0\xb0 1995 1080p");
	g_assert_cmpstr(found->fetch_path, ==, "/dl.php?id=1");
	g_assert_cmpint(found->release->size_bytes, ==, 1610612736);
	g_assert_cmpint(found->release->seeders, ==, 7);
	g_assert_cmpint(found->release->leechers, ==, 1);
	g_assert_cmpint(found->release->published_us, ==, NOW_US - G_GINT64_CONSTANT(2) * 3600 * G_USEC_PER_SEC);
	found = found_named(&outcome, "\xd0\x96\xd0\xb0\xd1\x80\xd0\xb0 1995 720p");
	g_assert_cmpstr(found->fetch_path, ==, "/dl.php?id=2");
	g_assert_cmpint(found->release->size_bytes, ==, 734003200);
	g_assert_cmpint(found->release->leechers, ==, -1);
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1699914600) * G_USEC_PER_SEC);
	axis_indexer_outcome_clear(&outcome);
	site_close(&site);
}

/* Spec: Elsewhere: what a definition or a page names off the site is not
 * asked; task 4.3 (the engine; the requests themselves are
 * test-indexer-cardigann) */
static void
test_engine_elsewhere(void)
{
	g_autofree gchar *hostile = fixture("hostile-path.yml");
	g_autofree gchar *text = replaced(hostile, "@OTHER@", "https://evil.example.invalid");
	g_autofree gchar *links_page = fixture("hostile-links-search.html");
	g_autofree gchar *release_page = fixture("hostile-links-release.html");
	g_autofree gchar *page_a = replaced(links_page, "@OTHER@", "https://evil.example.invalid");
	g_autofree gchar *page = replaced(page_a, "@SELF@", "https://links.example.invalid");
	g_autofree gchar *release = replaced(release_page, "@OTHER@", "https://evil.example.invalid");
	g_autofree gchar *problem = NULL;
	g_autofree gchar *magnet = NULL;
	g_autofree gchar *file = NULL;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	/* Usable as written: the address is only in a setting's default. */
	site_open(&site, load_ok(text), "https://hostile.example.invalid");
	assert_usable(site.def);
	movie_request(&request, "Hearth", 1995, NULL);
	g_assert_null(axis_def_search_new(&site.site, &request, &problem));
	g_assert_nonnull(strstr(problem, "not its site's"));
	g_clear_pointer(&problem, g_free);
	site_close(&site);

	site_open(&site, load_fixture("hostile-links.yml"), "https://links.example.invalid");
	{
		g_autoptr(AxisDefSearch) search = search_ok(&site, &request);

		read_ok(search, page, strlen(page), &outcome);
	}
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_off_origin, ==, 2);
	g_assert_cmpuint(outcome.n_unusable, ==, 1);
	g_assert_cmpstr(found_named(&outcome, "Hearth 1995 Own Page")->fetch_path, ==, "/release/1");
	g_assert_cmpstr(found_named(&outcome, "Hearth 1995 Relative Page")->fetch_path, ==, "/release/2");
	axis_indexer_outcome_clear(&outcome);
	/* The release page's only file is elsewhere: nothing is given. */
	g_assert_false(axis_def_read_download(&site.site, "/release/1", release, strlen(release), &magnet, &file,
		&problem));
	g_assert_null(magnet);
	g_assert_null(file);
	g_assert_nonnull(strstr(problem, "another address"));
	site_close(&site);
}

/* An answer that is not one is refused whole; the site's own error is
 * said. */
static void
test_engine_refuses(void)
{
	g_autofree gchar *text = variant("@SEARCH@", "  error:\n    - selector: div.error\n      message:\n"
		"        selector: div.error\n");
	g_autoptr(AxisDefSearch) search = NULL;
	g_autofree gchar *problem = NULL;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;

	site_open(&site, load_ok(text), "https://base.example.invalid");
	movie_request(&request, "Hearth", 1995, NULL);
	search = search_ok(&site, &request);
	axis_indexer_outcome_init(&outcome);
	g_assert_false(axis_def_search_read(search, 0, "<div class=error>Too many requests</div><tr><td>x</td></tr>", 59,
		NULL, &outcome, &problem));
	g_assert_nonnull(strstr(problem, "Too many requests"));
	g_assert_null(outcome.items);
	g_clear_pointer(&problem, g_free);
	/* A browser challenge in place of the page: not usable yet, saying
	 * what it needs. */
	{
		static const gchar challenge[] = "<html><head><title>Just a moment...</title></head><body><script>"
			"window._cf_chl_opt={}</script><tr><td>x</td></tr></body></html>";

		g_assert_true(axis_def_is_challenge(challenge));
		g_assert_false(axis_def_is_challenge("<html><title>Just a movie</title></html>"));
		/* add-builtin-torrent-indexers: Cloudflare's detection script in an
		 * ordinary page is no challenge; its challenge's own script is. */
		g_assert_false(axis_def_is_challenge("<html><body><table></table><script>(function(){var a=document."
			"createElement('script');a.src='/cdn-cgi/challenge-platform/scripts/jsd/main.js';})();</script>"
			"</body></html>"));
		g_assert_true(axis_def_is_challenge("<script src=\"/cdn-cgi/challenge-platform/h/b/orchestrate/chl_page/v1"
			"?ray=0\"></script>"));
		g_assert_true(axis_def_is_challenge("<script src='/cdn-cgi/challenge-platform/h/g/orchestrate/managed/v1"
			"?ray=0'></script>"));
		/* Review item 3: the detection script in its other form, under .../h/
		 * too, is no challenge; nor a page that links to DDoS-Guard. */
		g_assert_false(axis_def_is_challenge("<html><body><table></table><script>(function(){function c(){var b=a."
			"contentDocument||a.contentWindow.document;var d=b.createElement('script');d.nonce='';d.innerHTML=\""
			"window.__CF$cv$params={r:'0',t:'0'};var a=document.createElement('script');a.src='/cdn-cgi/"
			"challenge-platform/h/b/scripts/jsd/000000000000/main.js';document.getElementsByTagName('head')[0]."
			"appendChild(a);\";b.getElementsByTagName('head')[0].appendChild(d)}})();</script></body></html>"));
		g_assert_false(axis_def_is_challenge("<html><body><p>Protected by <a href=\"https://ddos-guard.net/\">"
			"DDoS-Guard</a></p><table></table></body></html>"));
		g_assert_true(axis_def_is_challenge("<html><head><title>DDoS-Guard</title></head><body></body></html>"));
		g_assert_false(axis_def_search_read(search, 0, challenge, strlen(challenge), NULL, &outcome, &problem));
		g_assert_nonnull(strstr(problem, "needs a browser helper"));
		g_assert_null(outcome.items);
	}
	site_close(&site);
}

/* ── add-builtin-torrent-indexers ───────────────────────────────── */

/* 3.1: a definition's version, and what every answer has */
static void
test_version_and_expect(void)
{
	static const struct { const gchar *text; gint version; gboolean usable; } versions[] = {
		{ "version: 3\n", 3, TRUE }, { "version: 1\n", 1, TRUE }, { "version: 1000000\n", 1000000, TRUE },
		{ "", 0, TRUE }, { "version: 0\n", 0, FALSE }, { "version: x\n", 0, FALSE }, { "version: 1.5\n", 0, FALSE },
		{ "version: -2\n", 0, FALSE }, { "version: 10000000\n", 0, FALSE }, { "version: [1]\n", 0, FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(versions); i++) {
		g_autofree gchar *text = variant("@ROOT@", versions[i].text);
		g_autoptr(AxisDef) def = load_ok(text);

		if (def->version != versions[i].version || axis_def_usable(def) != versions[i].usable)
			g_error("version case %u: got %d, %s", i, def->version, axis_def_usable(def) ? "usable" : "not usable");
	}
	{
		g_autofree gchar *expect = variant("@ROWS@", "tr\n    expect: table.results");
		g_autofree gchar *empty = variant("@ROWS@", "tr\n    expect: \"\"");
		g_autofree gchar *bad = variant("@ROWS@", "tr\n    expect: \"table:eq(1)\"");
		g_autoptr(AxisDef) with = load_ok(expect);
		g_autoptr(AxisDef) none = load_ok(empty);
		g_autoptr(AxisDef) unreadable = load_ok(bad);

		assert_usable(with);
		g_assert_cmpstr(with->rows_expect, ==, "table.results");
		g_assert_false(axis_def_usable(none));
		g_assert_false(axis_def_usable(unreadable));
	}
}

/* The verdict on @page for @text's definition. */
static AxisDefVerdict
verdict_of(
	const gchar         *text,
	const gchar         *page,
	AxisDefAnswerInfo   *info,
	AxisIndexerOutcome  *outcome,
	gchar              **problem
){
	g_autoptr(AxisDefSearch) search = NULL;
	AxisIndexerRequest request;
	AxisDefAnswerInfo own;
	Site site;

	if (info == NULL)
		info = &own;
	site_open(&site, load_ok(text), "https://base.example.invalid");
	movie_request(&request, "Made Up Film", 2001, NULL);
	search = search_ok(&site, &request);
	axis_indexer_outcome_init(outcome);
	axis_def_search_read_full(search, 0, page, strlen(page), NULL, outcome, info, problem);
	g_clear_pointer(&search, axis_def_search_free);
	site_close(&site);
	return info->verdict;
}

/* Spec: Changed layout; Nothing found; 3.2: drift told apart from an empty
 * answer */
static void
test_drift(void)
{
	g_autofree gchar *expect = variant("@ROWS@", "tr\n    expect: table.results");
	g_autofree gchar *plain = variant("", "");
	AxisIndexerOutcome outcome;
	AxisDefAnswerInfo info;
	gchar *problem = NULL;

	/* The part every answer has, and no row: nothing was found. */
	g_assert_cmpint(verdict_of(expect, "<table class=results></table>", &info, &outcome, &problem), ==,
		AXIS_DEF_ANSWER_READ);
	g_assert_null(problem);
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(info.rows_matched, ==, 0);
	axis_indexer_outcome_clear(&outcome);

	/* Without it: the site's page changed (or its address is someone
	 * else's). */
	g_assert_cmpint(verdict_of(expect, "<html><body><h1>This domain is for sale</h1></body></html>", &info, &outcome,
		&problem), ==, AXIS_DEF_ANSWER_DRIFT);
	g_assert_true(g_str_has_prefix(problem, AXIS_DEF_DRIFT_PROBLEM ": "));
	g_assert_nonnull(strstr(problem, "the part every answer of the site has is missing"));
	g_assert_null(outcome.items);
	g_clear_pointer(&problem, g_free);
	/* A definition that names no such part cannot tell: no results. */
	g_assert_cmpint(verdict_of(plain, "<html><body><h1>This domain is for sale</h1></body></html>", &info, &outcome,
		&problem), ==, AXIS_DEF_ANSWER_READ);
	g_assert_cmpuint(outcome.items->len, ==, 0);
	axis_indexer_outcome_clear(&outcome);

	/* Most rows lack what every result must have (a title): drift, for any
	 * definition, saying what they lack. */
	g_assert_cmpint(verdict_of(plain, "<table><tr><th>a</th></tr><tr><th>b</th></tr><tr><th>c</th></tr>"
		"<tr><td>Made Up Film 2001 1080p</td><td><a href=/d/1>get</a></td></tr></table>", &info, &outcome, &problem),
		==, AXIS_DEF_ANSWER_DRIFT);
	g_assert_cmpuint(info.rows_matched, ==, 4);
	g_assert_cmpuint(info.lost, ==, 3);
	g_assert_nonnull(strstr(problem, "3 of the 4 results it listed lack the field title"));
	g_clear_pointer(&problem, g_free);
	/* Half, or one alone, is not most. */
	g_assert_cmpint(verdict_of(plain, "<table><tr><th>a</th></tr><tr><td>Made Up Film 2001 1080p</td>"
		"<td><a href=/d/1>get</a></td></tr></table>", &info, &outcome, &problem), ==, AXIS_DEF_ANSWER_READ);
	g_assert_cmpuint(outcome.items->len, ==, 1);
	axis_indexer_outcome_clear(&outcome);
	g_assert_cmpint(verdict_of(plain, "<table><tr><th>a</th></tr></table>", &info, &outcome, &problem), ==,
		AXIS_DEF_ANSWER_READ);
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, 1);
	axis_indexer_outcome_clear(&outcome);
	/* Rows without their download (a field that must be there) are lost
	 * too. */
	g_assert_cmpint(verdict_of(plain, "<table><tr><td>One</td></tr><tr><td>Two</td></tr></table>", &info, &outcome,
		&problem), ==, AXIS_DEF_ANSWER_DRIFT);
	g_assert_nonnull(strstr(problem, "2 of the 2 results it listed lack the field download"));
	g_clear_pointer(&problem, g_free);
	/* And rows whose download is only elsewhere are left out, not lost. */
	g_assert_cmpint(verdict_of(plain, "<table><tr><td>One</td><td><a href=https://elsewhere.invalid/1>x</a></td></tr>"
		"<tr><td>Two</td><td><a href=https://elsewhere.invalid/2>x</a></td></tr></table>", &info, &outcome, &problem),
		==, AXIS_DEF_ANSWER_READ);
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(outcome.n_off_origin, ==, 2);
	g_assert_cmpuint(info.lost, ==, 0);
	axis_indexer_outcome_clear(&outcome);

	/* A challenge's page is a challenge, whatever the definition. */
	g_assert_cmpint(verdict_of(expect, "<html><head><title>Just a moment...</title></head></html>", &info, &outcome,
		&problem), ==, AXIS_DEF_ANSWER_CHALLENGE);
	g_assert_cmpstr(problem, ==, AXIS_DEF_CHALLENGE_PROBLEM);
	g_clear_pointer(&problem, g_free);

	/* A JSON API that answers a page: drift when the definition names what
	 * every answer has, unreadable when it does not. */
	{
		g_autofree gchar *json = variant("@PATH@", "/api\n      response:\n        type: json");
		g_autofree gchar *json_rows = replaced(json, "selector: tr\n", "selector: items\n    expect: items\n");
		g_autofree gchar *json_fields = replaced(json_rows, "      selector: td\n", "      selector: name\n");
		g_autofree gchar *json_plain = replaced(json, "selector: tr\n", "selector: items\n");

		g_assert_cmpint(verdict_of(json_fields, "<html>Not found</html>", &info, &outcome, &problem), ==,
			AXIS_DEF_ANSWER_DRIFT);
		g_assert_nonnull(strstr(problem, "is not what its definition reads"));
		g_clear_pointer(&problem, g_free);
		g_assert_cmpint(verdict_of(json_plain, "<html>Not found</html>", &info, &outcome, &problem), ==,
			AXIS_DEF_ANSWER_REFUSED);
		g_clear_pointer(&problem, g_free);
		g_assert_cmpint(verdict_of(json_fields, "{\"items\":[]}", &info, &outcome, &problem), ==,
			AXIS_DEF_ANSWER_READ);
		axis_indexer_outcome_clear(&outcome);
		g_assert_cmpint(verdict_of(json_fields, "{\"error\":\"down\"}", &info, &outcome, &problem), ==,
			AXIS_DEF_ANSWER_DRIFT);
		g_clear_pointer(&problem, g_free);
		/* A challenge's page in place of the JSON is a challenge; JSON that
		 * names one is JSON. */
		g_assert_cmpint(verdict_of(json_fields, "\n<!DOCTYPE html><html><head><title>Just a moment...</title></head>"
			"</html>", &info, &outcome, &problem), ==, AXIS_DEF_ANSWER_CHALLENGE);
		g_clear_pointer(&problem, g_free);
		g_assert_cmpint(verdict_of(json_fields, "{\"items\":[{\"name\":\"<title>Just a moment...</title>\"}]}", &info,
			&outcome, &problem), ==, AXIS_DEF_ANSWER_READ);
		axis_indexer_outcome_clear(&outcome);
	}
}

/* Spec: Music in a feed; 3.2: results whose category is not video are left
 * out */
static void
test_not_video(void)
{
	g_autofree gchar *base = variant("", "");
	g_autofree gchar *mapped = replaced(base, "    - {id: 1, cat: Movies, desc: Movies}\n",
		"    - {id: 1, cat: Movies, desc: Movies}\n    - {id: 2, cat: Audio, desc: Music}\n"
		"    - {id: 3, cat: PC/Games, desc: Games}\n    - {id: 4, cat: XXX, desc: Adult}\n"
		"    - {id: 5, cat: Books/EBook, desc: Books}\n    - {id: 6, cat: Other, desc: Other}\n");
	g_autofree gchar *text = replaced(mapped, "    category:\n      text: 1\n", "    category:\n      selector: td.c\n");
	AxisIndexerOutcome outcome;
	AxisDefAnswerInfo info;
	gchar *problem = NULL;
	static const gchar page[] =
		"<table>"
		"<tr><td>Made Up Film 2001 1080p</td><td class=c>1</td><td><a href=/d/1>get</a></td></tr>"
		"<tr><td>Made Up Film Soundtrack</td><td class=c>2</td><td><a href=/d/2>get</a></td></tr>"
		"<tr><td>Made Up Film The Game</td><td class=c>3</td><td><a href=/d/3>get</a></td></tr>"
		"<tr><td>Made Up Adult Film</td><td class=c>4</td><td><a href=/d/4>get</a></td></tr>"
		"<tr><td>Made Up Film The Book</td><td class=c>5</td><td><a href=/d/5>get</a></td></tr>"
		"<tr><td>Made Up Film Extras</td><td class=c>6</td><td><a href=/d/6>get</a></td></tr>"
		"<tr><td>Made Up Film Unmapped</td><td class=c>9</td><td><a href=/d/7>get</a></td></tr>"
		"</table>";

	g_assert_cmpint(verdict_of(text, page, &info, &outcome, &problem), ==, AXIS_DEF_ANSWER_READ);
	g_assert_cmpuint(info.not_video, ==, 3);
	g_assert_cmpuint(outcome.items->len, ==, 4);
	found_named(&outcome, "Made Up Film 2001 1080p");
	found_named(&outcome, "Made Up Adult Film");
	found_named(&outcome, "Made Up Film Extras");
	found_named(&outcome, "Made Up Film Unmapped");
	axis_indexer_outcome_clear(&outcome);
}

/* The names among @names that @words keep through andmatch, joined by "|". */
static gchar *
kept_by_andmatch(
	const gchar        *words,
	const gchar *const *names
){
	g_autofree gchar *text = variant("@ROWS@", "tr\n    filters:\n      - name: andmatch");
	g_autoptr(AxisDefSearch) search = NULL;
	g_autoptr(GString) page = g_string_new("<table>");
	g_autoptr(GString) kept = g_string_new(NULL);
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	Site site;
	guint i;

	for (i = 0; names[i] != NULL; i++)
		g_string_append_printf(page, "<tr><td>%s</td><td><a href=/d/%u>get</a></td></tr>", names[i], i);
	g_string_append(page, "</table>");
	site_open(&site, load_ok(text), "https://base.example.invalid");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = words;
	search = search_ok(&site, &request);
	read_ok(search, page->str, page->len, &outcome);
	for (i = 0; names[i] != NULL; i++) {
		guint k;

		for (k = 0; outcome.items != NULL && k < outcome.items->len; k++) {
			const AxisIndexerFound *found = g_ptr_array_index(outcome.items, k);

			if (strcmp(found->release->name, names[i]) == 0) {
				g_string_append_printf(kept, "%s%s", kept->len > 0 ? "|" : "", names[i]);
				break;
			}
		}
	}
	axis_indexer_outcome_clear(&outcome);
	g_clear_pointer(&search, axis_def_search_free);
	site_close(&site);
	return g_string_free(g_steal_pointer(&kept), FALSE);
}

/* Review item 6: andmatch folds as Axis compares titles (accents, strokes,
 * apostrophes, "&" and "and") both ways, holds whole words, and leaves out
 * a single letter that is a leftover of punctuation */
static void
test_and_match(void)
{
	static const gchar *const cafe[] = {
		"Cafe Made Up 2001 1080p", "Caf\xc3\xa9.Made.Up.2001.720p", "Cafe Made Up 2002", NULL
	};
	static const gchar *const soren[] = { "S\xc3\xb8ren Made Up 2001", "Soren.Made.Up.2001", "Sorensen Made Up", NULL };
	static const gchar *const apostrophe[] = { "Made Ups Film 2001", "Made.Up's.Film.2001", "Made Up Film 2001", NULL };
	static const gchar *const and_sign[] = { "Made and Up 2001", "Made & Up 2001", "Made Up 2001", NULL };
	static const gchar *const whole[] = { "Made Up 2160p WEB", "Made Up 2 1080p", "Upside Made 2", NULL };
	static const gchar *const leftover[] = { "Made Film 2001", "X-Made Film 2001", "Made 2001", NULL };
	static const gchar *const dotted[] = { "SWAT Made 2001", "S.W.A.T.Made.2001", "S W Made 2001", NULL };
	g_autofree gchar *got = NULL;

	got = kept_by_andmatch("Caf\xc3\xa9 Made Up 2001", cafe);
	g_assert_cmpstr(got, ==, "Cafe Made Up 2001 1080p|Caf\xc3\xa9.Made.Up.2001.720p");
	g_free(got);
	got = kept_by_andmatch("Cafe Made Up 2001", cafe);
	g_assert_cmpstr(got, ==, "Cafe Made Up 2001 1080p|Caf\xc3\xa9.Made.Up.2001.720p");
	g_free(got);
	got = kept_by_andmatch("Soren Made Up", soren);
	g_assert_cmpstr(got, ==, "S\xc3\xb8ren Made Up 2001|Soren.Made.Up.2001");
	g_free(got);
	got = kept_by_andmatch("S\xc3\xb8ren Made Up", soren);
	g_assert_cmpstr(got, ==, "S\xc3\xb8ren Made Up 2001|Soren.Made.Up.2001");
	g_free(got);
	/* "Made Up's" is "Made Ups", as the release names write it, never a
	 * word "s" that every name has. */
	got = kept_by_andmatch("Made Up's Film", apostrophe);
	g_assert_cmpstr(got, ==, "Made Ups Film 2001|Made.Up's.Film.2001");
	g_free(got);
	got = kept_by_andmatch("Made & Up 2001", and_sign);
	g_assert_cmpstr(got, ==, "Made and Up 2001|Made & Up 2001");
	g_free(got);
	got = kept_by_andmatch("Made and Up", and_sign);
	g_assert_cmpstr(got, ==, "Made and Up 2001|Made & Up 2001");
	g_free(got);
	/* Whole words: "2" is not in "2160p", "Up" not in "Upside". */
	got = kept_by_andmatch("Made Up 2", whole);
	g_assert_cmpstr(got, ==, "Made Up 2 1080p");
	g_free(got);
	/* The "X" of "X-Made" is a leftover of its punctuation. */
	got = kept_by_andmatch("X-Made Film", leftover);
	g_assert_cmpstr(got, ==, "Made Film 2001|X-Made Film 2001");
	g_free(got);
	got = kept_by_andmatch("S.W.A.T. Made", dotted);
	g_assert_cmpstr(got, ==, "SWAT Made 2001|S.W.A.T.Made.2001");
}

/* Spec: A field that matches nothing; Markup in an answer; 3.3: the trace */
static void
test_trace(void)
{
	g_autofree gchar *base = variant("@FIELDS@", "    leechers:\n      selector: td.peers\n      optional: true\n"
		"      default: \"0\"\n    grabs:\n      selector: td.done\n");
	g_autofree gchar *text = replaced(base, "rows:\n    selector: tr\n", "rows:\n    selector: tr\n    expect: table\n");
	g_autoptr(AxisDefSearch) search = NULL;
	g_autoptr(AxisDefTrace) trace = axis_def_trace_new(TRUE);
	g_autofree gchar *account = NULL;
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	const AxisDefTraceAnswer *answer;
	const AxisDefTraceRow *row;
	const AxisDefTraceField *field;
	gchar *problem = NULL;
	Site site;
	static const gchar page[] =
		"<table>"
		"<tr><td>Made Up &lt;b&gt;Film&lt;/b&gt; 2001</td><td class=done>12</td><td><a href=/d/1>get</a></td></tr>"
		"<tr><td>Made Up Film 2001 720p</td><td><a href=/d/2>get</a></td></tr>"
		"</table>";

	site_open(&site, load_ok(text), "https://base.example.invalid");
	movie_request(&request, "Made Up Film", 2001, NULL);
	search = search_ok(&site, &request);
	axis_def_search_set_trace(search, trace);
	g_assert_cmpstr(trace->definition, ==, "base (it states no version)");
	g_assert_cmpstr(trace->words, ==, "Made Up Film 2001");
	g_assert_cmpuint(trace->requests->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(trace->requests, 0), ==, "/search");
	axis_indexer_outcome_init(&outcome);
	g_assert_true(axis_def_search_read_full(search, 0, page, strlen(page), NULL, &outcome, NULL, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 1);

	g_assert_cmpuint(trace->answers->len, ==, 1);
	answer = g_ptr_array_index(trace->answers, 0);
	g_assert_cmpstr(answer->path, ==, "/search");
	g_assert_cmpuint(answer->length, ==, strlen(page));
	g_assert_cmpstr(answer->kind, ==, "HTML");
	g_assert_true(answer->expect_found);
	g_assert_cmpuint(answer->rows_matched, ==, 2);
	g_assert_cmpuint(answer->kept, ==, 1);
	g_assert_cmpuint(answer->lost, ==, 1);
	g_assert_nonnull(strstr(answer->verdict, "1 release kept"));
	g_assert_cmpuint(answer->rows->len, ==, 2);
	/* The first row: every field, what it read, the markup a name holds as
	 * text, the optional one's default. */
	row = g_ptr_array_index(answer->rows, 0);
	g_assert_cmpuint(row->number, ==, 1);
	g_assert_cmpstr(row->fate, ==, "kept");
	field = g_ptr_array_index(row->fields, 1);
	g_assert_cmpstr(field->name, ==, "title");
	g_assert_cmpstr(field->how, ==, "selector td");
	g_assert_cmpstr(field->value, ==, "Made Up <b>Film</b> 2001");
	field = g_ptr_array_index(row->fields, 2);
	g_assert_cmpstr(field->name, ==, "leechers");
	g_assert_cmpstr(field->value, ==, "0");
	g_assert_cmpstr(field->note, ==, "matched nothing: optional, so its default");
	g_assert_nonnull(row->excerpt);
	g_assert_true(g_str_has_prefix(row->excerpt, "<tr><td>Made Up &lt;b&gt;Film"));
	/* The second: lost for the field that matched nothing, and said so. */
	row = g_ptr_array_index(answer->rows, 1);
	g_assert_cmpstr(row->fate, ==, "left out: the field grabs matched nothing, and it must be there");
	field = g_ptr_array_index(row->fields, row->fields->len - 1);
	g_assert_cmpstr(field->name, ==, "grabs");
	g_assert_null(field->value);
	g_assert_cmpstr(field->note, ==, "matched nothing");

	account = axis_def_trace_to_text(trace);
	g_assert_nonnull(strstr(account, "Rows: tr: matched 2\n"));
	g_assert_nonnull(strstr(account, "Every answer has: table: found\n"));
	g_assert_nonnull(strstr(account, "  Row 2: left out: the field grabs matched nothing"));
	g_assert_nonnull(strstr(account, "grabs: selector td.done (matched nothing)"));
	axis_indexer_outcome_clear(&outcome);
	g_clear_pointer(&account, g_free);

	/* A page no row was read from has an excerpt of its own, cut. */
	{
		GString *long_page = g_string_new("<html><body><table>");
		AxisDefTraceAnswer *last;
		guint k;

		for (k = 0; k < 400; k++)
			g_string_append(long_page, "<div class=\"x\">text that is not a row</div>");
		g_string_append(long_page, "</table></body></html>");
		axis_indexer_outcome_init(&outcome);
		g_assert_true(axis_def_search_read_full(search, 0, long_page->str, long_page->len, NULL, &outcome, NULL,
			&problem));
		last = axis_def_trace_last(trace);
		g_assert_cmpuint(last->rows_matched, ==, 0);
		g_assert_nonnull(last->excerpt);
		g_assert_true(g_str_has_prefix(last->excerpt, "<body>"));
		g_assert_nonnull(strstr(last->excerpt, "<div class=\"x\">text that is not a row</div>"));
		g_assert_cmpuint(strlen(last->excerpt), <=, AXIS_DEF_TRACE_EXCERPT_BYTES + 3);
		g_assert_true(g_str_has_suffix(last->excerpt, "..."));
		axis_indexer_outcome_clear(&outcome);
		g_string_free(long_page, TRUE);
	}
	g_clear_pointer(&search, axis_def_search_free);
	site_close(&site);
}

/* 3.3: the page reader written back, for excerpts */
static void
test_html_markup(void)
{
	static const gchar page[] = "<div id=\"a\" title='say \"hi\" &amp; go'>One  <br>two\n\n<b>&lt;three&gt;</b>"
		"<img src=x></div><p>after</p>";
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisHtmlDoc) doc = axis_html_parse(page, strlen(page), FALSE, &problem);
	g_autofree gchar *all = NULL;
	g_autofree gchar *cut = NULL;
	g_autofree gchar *div = NULL;
	g_autofree gchar *none = NULL;

	g_assert_nonnull(doc);
	div = axis_html_markup(first_of(doc, "#a"), 4096);
	g_assert_cmpstr(div, ==, "<div id=\"a\" title=\"say &quot;hi&quot; &amp; go\">One <br>two <b>&lt;three&gt;</b>"
		"<img src=\"x\"></div>");
	/* The whole document: what is beside the element too. */
	all = axis_html_markup(axis_html_doc_root(doc), 4096);
	g_assert_nonnull(strstr(all, div));
	g_assert_nonnull(strstr(all, "<p>after</p>"));
	cut = axis_html_markup(first_of(doc, "#a"), 10);
	g_assert_cmpstr(cut, ==, "<div id=\"a");
	none = axis_html_markup(NULL, 10);
	g_assert_cmpstr(none, ==, "");

	/* Review item 7: a text node and an attribute of a megabyte, cut where
	 * they are copied, and between characters. */
	{
		g_autoptr(GString) big = g_string_new("<div id=\"big\" title=\"");
		g_autoptr(AxisHtmlDoc) huge = NULL;
		g_autofree gchar *excerpt = NULL;
		guint i;

		for (i = 0; i < 1024 * 1024 / 2; i++)
			g_string_append(big, "\xc3\xa9");
		g_string_append(big, "\">");
		for (i = 0; i < 1024 * 1024 / 2; i++)
			g_string_append(big, "\xc3\xa9");
		g_string_append(big, "</div>");
		huge = axis_html_parse(big->str, big->len, FALSE, &problem);
		g_assert_nonnull(huge);
		excerpt = axis_html_markup(first_of(huge, "#big"), 101);
		g_assert_cmpuint(strlen(excerpt), <=, 101);
		g_assert_true(g_utf8_validate(excerpt, -1, NULL));
		g_assert_true(g_str_has_prefix(excerpt, "<div id=\"big\" title=\"\xc3\xa9"));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/definitions/yaml/reads", test_yaml_reads);
	g_test_add_func("/definitions/yaml/refuses", test_yaml_refuses);
	g_test_add_func("/definitions/yaml/limits", test_yaml_limits);
	g_test_add_func("/definitions/html/tree", test_html_tree);
	g_test_add_func("/definitions/html/xml", test_html_xml);
	g_test_add_func("/definitions/html/limits", test_html_limits);
	g_test_add_func("/definitions/select/features", test_select_features);
	g_test_add_func("/definitions/select/refuses", test_select_refuses);
	g_test_add_func("/definitions/select/budget", test_select_budget);
	g_test_add_func("/definitions/template/renders", test_template_renders);
	g_test_add_func("/definitions/template/refuses", test_template_refuses);
	g_test_add_func("/definitions/filters", test_filters);
	g_test_add_func("/definitions/values", test_values);
	g_test_add_func("/definitions/regex/bounds", test_regex_bounds);
	g_test_add_func("/definitions/judge/fixtures", test_fixtures_usable);
	g_test_add_func("/definitions/judge/not-usable", test_not_usable);
	g_test_add_func("/definitions/judge/not-definitions", test_not_definitions);
	g_test_add_func("/definitions/engine/html", test_engine_html);
	g_test_add_func("/definitions/engine/anime", test_engine_anime);
	g_test_add_func("/definitions/engine/json", test_engine_json);
	g_test_add_func("/definitions/engine/rss", test_engine_rss);
	g_test_add_func("/definitions/engine/after-and-encoding", test_engine_after_and_encoding);
	g_test_add_func("/definitions/engine/elsewhere", test_engine_elsewhere);
	g_test_add_func("/definitions/engine/refuses", test_engine_refuses);
	g_test_add_func("/definitions/judge/version-and-expect", test_version_and_expect);
	g_test_add_func("/definitions/engine/drift", test_drift);
	g_test_add_func("/definitions/engine/not-video", test_not_video);
	g_test_add_func("/definitions/engine/andmatch", test_and_match);
	g_test_add_func("/definitions/engine/trace", test_trace);
	g_test_add_func("/definitions/html/markup", test_html_markup);
	return g_test_run();
}
