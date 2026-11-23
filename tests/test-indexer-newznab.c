/*
 * test-indexer-newznab.c - native indexers (Newznab, Torznab): the bounded
 * XML reader, the answers' mapping, the kinds against stand-ins on loopback,
 * the search of every indexer at once and its merging, on a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The stand-ins (tests/axis-test-newznab.c) answer from hand-written
 * fixtures (tests/fixtures/newznab and its README: written from the
 * specifications and the documented answers; no real indexer was asked).
 *
 * add-indexer-newznab, spec scenarios (indexers): Usenet indexer; Without
 * Prowlarr (the layer's part; the modules' are test-movies and test-tv);
 * Both; One indexer down; Allowance; Entity definitions; Failure message.
 * Tasks 4.1 to 4.6 (4.7 is test-movies' and test-tv's).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-newznab.h"
#include "axis-test-plex.h"
#include "axis-test-prowlarr.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-newznab-parse.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "indexer/axis-indexer-xml.h"

#define KEY       "nzb-api-key-5c1e9a07b3d24f68"
#define OTHER_KEY "nzb-other-key-77a0c3e1d9"
#define FEED_KEY  "jackett-key-2f9b6d41c0e8"
#define PROWLARR_KEY "prowlarr-api-key-7f3a9c21d84e5b06"

#define FIXTURES "tests/fixtures/newznab/"

static gchar *
read_fixture(
	const gchar *name,
	gsize       *length
){
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, length, NULL));
	return text;
}

/* The fixture with the substitutions a stand-in at @origin would make. */
static gchar *
fixture_at(
	const gchar *name,
	const gchar *origin,
	const gchar *files
){
	gchar *text = read_fixture(name, NULL);
	gchar **parts;
	gchar *out;

	parts = g_strsplit(text, "@FILES@", -1);
	out = g_strjoinv(files != NULL ? files : origin, parts);
	g_strfreev(parts);
	g_free(text);
	text = out;
	parts = g_strsplit(text, "@ORIGIN@", -1);
	out = g_strjoinv(origin, parts);
	g_strfreev(parts);
	g_free(text);
	text = out;
	parts = g_strsplit(text, "@BASE@", -1);
	out = g_strjoinv("", parts);
	g_strfreev(parts);
	g_free(text);
	text = out;
	parts = g_strsplit(text, "@KEY@", -1);
	out = g_strjoinv(KEY, parts);
	g_strfreev(parts);
	g_free(text);
	return out;
}

/* ── The XML reader ─────────────────────────────────────────────── */

/* Every event of @text, written out: "S:name[a=v]", "T:text", "E:name". */
static gchar *
events_of(const gchar *text)
{
	g_autoptr(AxisXmlReader) reader = axis_xml_reader_new(text, strlen(text), NULL);
	GString *out = g_string_new(NULL);
	AxisXmlEvent event;

	while ((event = axis_xml_reader_next(reader)) != AXIS_XML_DONE && event != AXIS_XML_ERROR) {
		guint i;

		switch (event) {
		case AXIS_XML_START:
			g_string_append_printf(out, "S%u:%s", axis_xml_reader_depth(reader), axis_xml_reader_name(reader));
			for (i = 0; i < axis_xml_reader_n_attributes(reader); i++)
				g_string_append_printf(out, "[%s=%s]", axis_xml_reader_attribute_name(reader, i),
					axis_xml_reader_attribute_value(reader, i));
			g_string_append_c(out, ' ');
			break;
		case AXIS_XML_END:
			g_string_append_printf(out, "E%u:%s ", axis_xml_reader_depth(reader), axis_xml_reader_name(reader));
			break;
		default:
			g_string_append_printf(out, "T:%s ", axis_xml_reader_text(reader, NULL));
			break;
		}
	}
	if (event == AXIS_XML_ERROR)
		g_string_append_printf(out, "ERROR:%s", axis_xml_reader_error(reader));
	else
		g_string_append(out, "DONE");
	return g_string_free(out, FALSE);
}

static void
test_xml_reads(void)
{
	g_autofree gchar *a = events_of("\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- a comment -->"
		"<?xml-stylesheet href=\"x\"?><rss v='2' x:y=\"&lt;&amp;&gt;&quot;&apos;\"><a>one &amp; two&#33;&#x41;</a>"
		"<b/><n:attr name=\"size\" value=\"1\"/><c><![CDATA[<raw> & ]]></c></rss>\n");
	g_autofree gchar *b = events_of("\n<x>\xc3\xa9t\xc3\xa9</x>");
	g_autoptr(AxisXmlReader) reader = NULL;
	const gchar *doc = "<rss><newznab:attr name=\"grabs\" value=\"3\"/><torznab:attr name=\"seeders\"/></rss>";

	g_assert_cmpstr(a, ==, "S1:rss[v=2][x:y=<&>\"'] S2:a T:one & two!A E2:a S2:b E2:b S2:n:attr[name=size][value=1] "
		"E2:n:attr S2:c T:<raw> &  E2:c E1:rss DONE");
	g_assert_cmpstr(b, ==, "S1:x T:\xc3\xa9t\xc3\xa9 E1:x DONE");
	/* Attributes by local name, whatever the prefix. */
	reader = axis_xml_reader_new(doc, strlen(doc), NULL);
	g_assert_cmpint(axis_xml_reader_next(reader), ==, AXIS_XML_START);
	g_assert_cmpint(axis_xml_reader_next(reader), ==, AXIS_XML_START);
	g_assert_cmpstr(axis_xml_local_name(axis_xml_reader_name(reader)), ==, "attr");
	g_assert_cmpstr(axis_xml_reader_attribute(reader, "value"), ==, "3");
	g_assert_null(axis_xml_reader_attribute(reader, "missing"));
	g_assert_cmpstr(axis_xml_local_name("torznab:attr"), ==, "attr");
	g_assert_cmpstr(axis_xml_local_name("attr"), ==, "attr");
}

/* Spec: Entity definitions; task 4.2: entity definitions, external
 * references, deep nesting, huge documents and truncated input are refused. */
static void
test_xml_refuses(void)
{
	static const struct
	{
		const gchar *text;
		const gchar *why;
	} cases[] = {
		{ "<!DOCTYPE r [<!ENTITY a \"b\">]><r>&a;</r>", "document type declaration" },
		{ "<?xml version=\"1.0\"?><!DOCTYPE r SYSTEM \"http://x.test/a.dtd\"><r/>", "document type declaration" },
		{ "<r><!DOCTYPE x></r>", "document type declaration" },
		{ "<r><!ENTITY a \"b\"></r>", "declaration this reader does not read" },
		{ "<r>&a;</r>", "entity reference" },
		{ "<r>&lt</r>", "reference this reader knows" },
		{ "<r>a & b</r>", "reference this reader knows" },
		{ "<r>&#0;</r>", "does not allow" },
		{ "<r>&#xD800;</r>", "does not allow" },
		{ "<r>&#x110000;</r>", "out of range" },
		{ "<r>&#xZZ;</r>", "not a number" },
		{ "<r a=\"1\" a=\"2\"/>", "same attribute twice" },
		{ "<r a=\"<\"/>", "holds a '<'" },
		{ "<r a=1/>", "not in quotes" },
		{ "<r a/>", "no value" },
		{ "<r a=\"1\"b=\"2\"/>", "not separated" },
		{ "<r><a></b></r>", "does not match" },
		{ "<r></r><s/>", "more than one root" },
		{ "text<r/>", "outside the root" },
		{ "<r/>text", "outside the root" },
		{ "", "no root element" },
		{ "   ", "no root element" },
		{ "<r>", "ends before its root element" },
		{ "<r><a>text", "ends before its root element" },
		{ "<r a=\"1", "ends inside an attribute value" },
		{ "<r", "ends inside a tag" },
		{ "<r><!-- never ends", "comment does not end" },
		{ "<r><![CDATA[ never ends", "CDATA section does not end" },
		{ "<r>]]></r>", "outside a CDATA section" },
		{ "<r>\x01</r>", "not UTF-8 text" },
		{ "<r>\xff</r>", "not UTF-8 text" },
		{ "<r>\xef\xbf\xbf</r>", "not UTF-8 text" },
		{ "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><r/>", "encoding other than UTF-8" },
		{ "<r/><?xml version=\"1.0\"?>", "not at the start" },
		{ "<>", "no name" },
		{ NULL, NULL }
	};
	g_autofree gchar *deep = NULL;
	g_autofree gchar *wide = NULL;
	GString *text;
	const gchar *problem = NULL;
	AxisXmlLimits limits;
	guint i;

	for (i = 0; cases[i].text != NULL; i++) {
		problem = NULL;
		if (axis_xml_check(cases[i].text, strlen(cases[i].text), NULL, &problem))
			g_error("accepted: %s", cases[i].text);
		g_assert_nonnull(problem);
		if (strstr(problem, cases[i].why) == NULL)
			g_error("%s: refused for \"%s\", not \"%s\"", cases[i].text, problem, cases[i].why);
	}
	/* The hostile fixtures. */
	{
		gsize n = 0;
		g_autofree gchar *laughs = read_fixture("hostile-entities.xml", &n);
		g_autofree gchar *external = NULL;

		g_assert_false(axis_xml_check(laughs, n, NULL, &problem));
		g_assert_nonnull(strstr(problem, "document type declaration"));
		external = read_fixture("hostile-external.xml", &n);
		g_assert_false(axis_xml_check(external, n, NULL, &problem));
		g_assert_nonnull(strstr(problem, "document type declaration"));
	}
	/* Nested past the depth limit (32), and at it. */
	text = g_string_new(NULL);
	for (i = 0; i < AXIS_XML_MAX_DEPTH; i++)
		g_string_append(text, "<a>");
	for (i = 0; i < AXIS_XML_MAX_DEPTH; i++)
		g_string_append(text, "</a>");
	g_assert_true(axis_xml_check(text->str, text->len, NULL, NULL));
	g_string_prepend(text, "<b>");
	g_string_append(text, "</b>");
	g_assert_false(axis_xml_check(text->str, text->len, NULL, &problem));
	g_assert_nonnull(strstr(problem, "nested too deeply"));
	deep = g_string_free(text, FALSE);
	/* Too many attributes on one element. */
	text = g_string_new("<r");
	for (i = 0; i <= AXIS_XML_MAX_ATTRIBUTES; i++)
		g_string_append_printf(text, " a%u=\"1\"", i);
	g_string_append(text, "/>");
	g_assert_false(axis_xml_check(text->str, text->len, NULL, &problem));
	g_assert_nonnull(strstr(problem, "too many attributes"));
	wide = g_string_free(text, FALSE);
	/* A document larger than the limit, and too many elements. */
	memset(&limits, 0, sizeof(limits));
	limits.max_bytes = 64;
	g_assert_false(axis_xml_check("<r>0123456789012345678901234567890123456789012345678901234567890</r>", 69, &limits,
		&problem));
	g_assert_nonnull(strstr(problem, "larger than the limit"));
	{
		gsize huge = AXIS_XML_MAX_BYTES + 16;
		gchar *big = g_malloc(huge);

		memset(big, ' ', huge);
		memcpy(big, "<r>", 3);
		memcpy(big + huge - 4, "</r>", 4);
		g_assert_false(axis_xml_check(big, huge, NULL, &problem));
		g_assert_nonnull(strstr(problem, "larger than the limit"));
		g_free(big);
	}
	memset(&limits, 0, sizeof(limits));
	limits.max_elements = 3;
	g_assert_true(axis_xml_check("<r><a/><b/></r>", 15, &limits, NULL));
	g_assert_false(axis_xml_check("<r><a/><b/><c/></r>", 19, &limits, &problem));
	g_assert_nonnull(strstr(problem, "too many elements"));
	/* Truncated anywhere: a real answer cut at every byte is refused (the
	 * last cuts leave only space or nothing after the root, and pass). */
	{
		gsize n = 0;
		g_autofree gchar *feed = read_fixture("search-movie.xml", &n);
		gsize end = n;
		gsize cut;

		g_assert_true(axis_xml_check(feed, n, NULL, NULL));
		while (end > 0 && (feed[end - 1] == '\n' || feed[end - 1] == ' '))
			end--;
		for (cut = 0; cut < end; cut += 1 + cut / 50)
			g_assert_false(axis_xml_check(feed, cut, NULL, NULL));
	}
}

/* The search answer's mapping refuses a hostile answer as a whole: nothing
 * of it is taken. */
static void
test_xml_nothing_of_a_refused_answer(void)
{
	g_autofree gchar *laughs = fixture_at("hostile-entities.xml", "http://127.0.0.1:1", NULL);
	g_autofree gchar *good = fixture_at("search-movie.xml", "http://127.0.0.1:1", NULL);
	g_autofree gchar *spoiled = NULL;
	AxisNewznabContext context;
	AxisIndexerOutcome outcome;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	AxisHttpOrigin origin;
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_http_origin_parse("http://127.0.0.1:1", &origin, NULL));
	memset(&context, 0, sizeof(context));
	context.origin = &origin;
	context.protocol = AXIS_DOWNLOAD_USENET;
	g_assert_cmpint(axis_newznab_parse_search(laughs, strlen(laughs), &context, &outcome, &allowance, &document,
		&problem), ==, AXIS_NEWZNAB_READ_INVALID);
	g_assert_null(outcome.items);
	g_assert_nonnull(strstr(problem, "document type declaration"));
	g_clear_pointer(&problem, g_free);
	/* A good answer with a bad byte at its end: none of its items. */
	spoiled = g_strconcat(good, "<extra>", NULL);
	g_assert_cmpint(axis_newznab_parse_search(spoiled, strlen(spoiled), &context, &outcome, &allowance, &document,
		&problem), ==, AXIS_NEWZNAB_READ_INVALID);
	g_assert_null(outcome.items);
	g_assert_cmpint(allowance.api_max, ==, -1);
	axis_http_origin_clear(&origin);
}

/* ── The answers ────────────────────────────────────────────────── */

/* Task 2.2: caps */
static void
test_parse_caps(void)
{
	gsize n = 0;
	g_autofree gchar *caps_text = read_fixture("caps.xml", &n);
	g_autofree gchar *old_text = NULL;
	g_autofree gchar *version = NULL;
	g_autofree gchar *problem = NULL;
	AxisIndexerCaps caps;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	guint i;
	gboolean hd = FALSE;

	g_assert_cmpint(axis_newznab_parse_caps(caps_text, n, &caps, &allowance, &version, &document, &problem), ==,
		AXIS_NEWZNAB_READ_OK);
	g_assert_cmpstr(version, ==, "Stand-in Newznab 1.0");
	g_assert_cmpuint(caps.modes, ==, AXIS_INDEXER_CAN_SEARCH | AXIS_INDEXER_CAN_MOVIE | AXIS_INDEXER_CAN_TV);
	g_assert_cmpuint(caps.movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpuint(caps.tv_ids, ==, AXIS_INDEXER_IDS_TVDB);
	g_assert_true(caps.movie_text);
	g_assert_true(caps.tv_text);
	g_assert_true(caps.tv_numbers);
	g_assert_cmpint(caps.limit_max, ==, 100);
	g_assert_cmpint(caps.limit_default, ==, 50);
	g_assert_nonnull(caps.categories);
	for (i = 0; caps.categories[i] != NULL; i++)
		hd = hd || strcmp(caps.categories[i], "2040 HD") == 0;
	g_assert_true(hd);
	g_assert_cmpuint(g_strv_length(caps.categories), ==, 13);
	g_assert_cmpint(allowance.api_max, ==, -1);
	axis_indexer_caps_clear(&caps);
	g_clear_pointer(&version, g_free);

	/* No supportedParams: the specification's defaults. */
	old_text = read_fixture("caps-defaults.xml", &n);
	g_assert_cmpint(axis_newznab_parse_caps(old_text, n, &caps, NULL, &version, &document, NULL), ==,
		AXIS_NEWZNAB_READ_OK);
	g_assert_cmpstr(version, ==, "Old Newznab 0.2.3");
	g_assert_cmpuint(caps.movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpuint(caps.tv_ids, ==, 0);
	g_assert_true(caps.tv_numbers);
	axis_indexer_caps_clear(&caps);

	/* An error document, and something else altogether. */
	{
		const gchar *refused = "<?xml version=\"1.0\"?><error code=\"100\" description=\"Incorrect user credentials\"/>";
		const gchar *feed = "<rss><channel/></rss>";

		g_assert_cmpint(axis_newznab_parse_caps(refused, strlen(refused), &caps, NULL, NULL, &document, NULL), ==,
			AXIS_NEWZNAB_READ_ERROR);
		g_assert_cmpint(document.code, ==, 100);
		g_assert_cmpstr(document.description, ==, "Incorrect user credentials");
		g_assert_cmpint(axis_newznab_error_kind(document.code), ==, AXIS_INDEXER_ERROR_AUTH);
		axis_newznab_error_clear(&document);
		g_assert_cmpint(axis_newznab_parse_caps(feed, strlen(feed), &caps, NULL, NULL, &document, &problem), ==,
			AXIS_NEWZNAB_READ_INVALID);
	}
	g_assert_cmpint(axis_newznab_error_kind(500), ==, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpint(axis_newznab_error_kind(501), ==, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpint(axis_newznab_error_kind(201), ==, AXIS_INDEXER_ERROR_REFUSED);
	g_assert_cmpint(axis_newznab_error_kind(910), ==, AXIS_INDEXER_ERROR_AUTH);
	g_assert_cmpint(axis_newznab_error_kind(900), ==, AXIS_INDEXER_ERROR_FAILED);
}

static AxisIndexerFound *
found_named(
	const AxisIndexerOutcome *outcome,
	const gchar              *name
){
	guint i;

	for (i = 0; outcome->items != NULL && i < outcome->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		if (g_strcmp0(found->release->name, name) == 0)
			return found;
	}
	return NULL;
}

/* Task 2.3: the answer mapped to the release model */
static void
test_parse_search(void)
{
	const gchar *base = "https://indexer.test:443";
	g_autofree gchar *text = fixture_at("search-movie.xml", "https://indexer.test", NULL);
	AxisNewznabContext context;
	AxisIndexerOutcome outcome;
	AxisIndexerAllowance allowance;
	AxisNewznabError document;
	AxisHttpOrigin origin;
	AxisIndexerFound *found;

	g_assert_true(axis_http_origin_parse(base, &origin, NULL));
	memset(&context, 0, sizeof(context));
	context.origin = &origin;
	context.protocol = AXIS_DOWNLOAD_USENET;
	context.now_us = g_get_real_time();
	g_assert_cmpint(axis_newznab_parse_search(text, strlen(text), &context, &outcome, &allowance, &document, NULL), ==,
		AXIS_NEWZNAB_READ_OK);
	/* Kept: 1, 2, 7, 8, 9, 10; one guid twice (3) is one; 4 elsewhere; 5 no
	 * title; 6 no file. */
	g_assert_cmpuint(outcome.items->len, ==, 6);
	g_assert_cmpuint(outcome.n_off_origin, ==, 1);
	g_assert_cmpuint(outcome.n_skipped, ==, 1);
	g_assert_cmpuint(outcome.n_unusable, ==, 1);
	g_assert_cmpstr(outcome.other_origin, ==, "https://files.elsewhere.test:443");
	g_assert_cmpint(allowance.api_used, ==, 12);
	g_assert_cmpint(allowance.api_max, ==, 5000);
	g_assert_cmpint(allowance.grab_used, ==, 3);
	g_assert_cmpint(allowance.grab_max, ==, 100);

	found = found_named(&outcome, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(found);
	g_assert_cmpstr(found->fetch_path, ==, "/api?t=get&id=nzb-aaaa1111&apikey=" KEY);
	g_assert_false(found->files_origin);
	g_assert_null(found->magnet);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(8923441152));
	g_assert_cmpint(found->release->grabs, ==, 412);
	g_assert_cmpuint(found->release->n_categories, ==, 2);
	g_assert_cmpint(found->release->categories[1], ==, 2040);
	g_assert_cmpstr(found->release->imdb_id, ==, "tt0600001");
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(found->release->seeders, ==, -1);
	g_assert_null(found->release->flags);
	g_assert_nonnull(found->merge_key);
	g_assert_null(strstr(found->merge_key, "details"));

	found = found_named(&outcome, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	g_assert_nonnull(found);
	g_assert_cmpstr(found->fetch_path, ==, "/getnzb/nzb-bbbb2222.nzb&i=1&r=" KEY);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(61203456789));
	g_assert_cmpstr(found->release->imdb_id, ==, "tt0600001");
	/* Fri, 01 Mar 2024 21:30:00 -0500 is 02:30 UTC the day after. */
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1709346600) * G_USEC_PER_SEC);

	found = found_named(&outcome, "Hearth.1995.<b>bold</b>.1080p-MARK");
	g_assert_nonnull(found);
	found = found_named(&outcome, "Hearth.1995.1080p.BluRay.DTS-PWD");
	g_assert_nonnull(found);
	g_assert_nonnull(found->release->flags);
	g_assert_cmpstr(found->release->flags[0], ==, "passworded");
	found = found_named(&outcome, "Hearth.1995.1080p.WRONG-FIELDS");
	g_assert_nonnull(found);
	g_assert_cmpint(found->release->size_bytes, ==, -1);
	g_assert_cmpint(found->release->grabs, ==, -1);
	g_assert_cmpuint(found->release->n_categories, ==, 0);
	g_assert_null(found->release->imdb_id);
	g_assert_cmpint(found->release->tvdb_id, ==, 0);
	g_assert_cmpint(found->release->published_us, ==, 0);
	found = found_named(&outcome, "Hearth.1995.Director's.Cut-NUM");
	g_assert_nonnull(found);
	g_assert_cmpint(found->release->published_us, ==, G_GINT64_CONSTANT(1709374260) * G_USEC_PER_SEC);
	axis_indexer_outcome_clear(&outcome);

	/* The accepted other origin for files: there, kept and marked. */
	{
		AxisHttpOrigin files;
		g_autofree gchar *elsewhere = fixture_at("search-movie.xml", "https://indexer.test",
			"https://files.indexer.test");

		g_assert_true(axis_http_origin_parse("https://files.indexer.test", &files, NULL));
		g_assert_cmpint(axis_newznab_parse_search(elsewhere, strlen(elsewhere), &context, &outcome, NULL, &document,
			NULL), ==, AXIS_NEWZNAB_READ_OK);
		/* Every item with an address (the guid given twice too: it is
		 * dropped before it could be one). */
		g_assert_cmpuint(outcome.items->len, ==, 0);
		g_assert_cmpuint(outcome.n_off_origin, ==, 8);
		g_assert_cmpstr(outcome.other_origin, ==, "https://files.indexer.test:443");
		axis_indexer_outcome_clear(&outcome);
		context.files_origin = &files;
		g_assert_cmpint(axis_newznab_parse_search(elsewhere, strlen(elsewhere), &context, &outcome, NULL, &document,
			NULL), ==, AXIS_NEWZNAB_READ_OK);
		g_assert_cmpuint(outcome.items->len, ==, 6);
		found = found_named(&outcome, "Hearth.1995.1080p.BluRay.x264-GRP");
		g_assert_true(found->files_origin);
		g_assert_cmpuint(outcome.n_off_origin, ==, 1);
		axis_indexer_outcome_clear(&outcome);
		axis_http_origin_clear(&files);
	}
	/* An error document with status 200. */
	{
		const gchar *limit = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<error code=\"500\" "
			"description=\"Request limit reached\"/>";

		context.files_origin = NULL;
		g_assert_cmpint(axis_newznab_parse_search(limit, strlen(limit), &context, &outcome, NULL, &document, NULL), ==,
			AXIS_NEWZNAB_READ_ERROR);
		g_assert_cmpint(document.code, ==, 500);
		g_assert_null(outcome.items);
		axis_newznab_error_clear(&document);
		g_assert_true(axis_newznab_parse_error_only(limit, strlen(limit), &document));
		g_assert_cmpint(document.code, ==, 500);
		axis_newznab_error_clear(&document);
		g_assert_false(axis_newznab_parse_error_only("<nzb/>", 6, &document));
	}
	axis_http_origin_clear(&origin);
}

static void
test_parse_torznab(void)
{
	g_autofree gchar *text = fixture_at("torznab-search.xml", "http://jackett.test:9117", NULL);
	AxisNewznabContext context;
	AxisIndexerOutcome outcome;
	AxisNewznabError document;
	AxisHttpOrigin origin;
	AxisIndexerFound *found;

	g_assert_true(axis_http_origin_parse("http://jackett.test:9117", &origin, NULL));
	memset(&context, 0, sizeof(context));
	context.origin = &origin;
	context.protocol = AXIS_DOWNLOAD_TORRENT;
	g_assert_cmpint(axis_newznab_parse_search(text, strlen(text), &context, &outcome, NULL, &document, NULL), ==,
		AXIS_NEWZNAB_READ_OK);
	g_assert_cmpuint(outcome.items->len, ==, 3);
	/* A torrent file outside the feed's path, on its origin. */
	found = found_named(&outcome, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(found);
	g_assert_true(g_str_has_prefix(found->fetch_path, "/dl/tidepool/?jackett_apikey=" KEY "&path=abc123"));
	g_assert_null(found->magnet);
	g_assert_cmpint(found->release->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpint(found->release->seeders, ==, 85);
	g_assert_cmpint(found->release->leechers, ==, 4);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(2254857830));
	g_assert_cmpstr(found->release->info_hash, ==, "f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4");
	g_assert_nonnull(found->release->flags);
	g_assert_cmpstr(found->release->flags[0], ==, "freeleech");
	/* A magnet link only, in its link, enclosure and magneturl. */
	found = found_named(&outcome, "Hearth (1995) 720p BRRip x264 -MAG");
	g_assert_nonnull(found);
	g_assert_null(found->fetch_path);
	g_assert_nonnull(found->magnet);
	g_assert_true(found->release->has_magnet);
	g_assert_cmpstr(found->release->info_hash, ==, "e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4");
	g_assert_cmpstr(found->release->flags[0], ==, "partial freeleech");
	g_assert_cmpstr(found->release->flags[1], ==, "double upload");
	g_assert_cmpint(found->release->leechers, ==, 1);
	axis_indexer_outcome_clear(&outcome);
	/* A Newznab indexer's magnet link is not believed. */
	context.protocol = AXIS_DOWNLOAD_USENET;
	g_assert_cmpint(axis_newznab_parse_search(text, strlen(text), &context, &outcome, NULL, &document, NULL), ==,
		AXIS_NEWZNAB_READ_OK);
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_unusable, ==, 1);
	axis_indexer_outcome_clear(&outcome);
	axis_http_origin_clear(&origin);
}

static void
test_parse_dates(void)
{
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 10:11:12 +0000"), ==,
		G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date("02 Mar 2024 10:11:12 GMT"), ==,
		G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date("Sat,  2 Mar 2024 05:11:12 EST"), ==,
		G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 11:41:12 +0130"), ==,
		G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date("2024-03-02T10:11:12Z"), ==, G_GINT64_CONSTANT(1709374272) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 10:11 UTC"), ==,
		G_GINT64_CONSTANT(1709374260) * G_USEC_PER_SEC);
	g_assert_cmpint(axis_newznab_parse_date(""), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date(NULL), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 31 Feb 2024 10:11:12 +0000"), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Foo 2024 10:11:12 +0000"), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 1900 10:11:12 +0000"), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 25:11:12 +0000"), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 10:11:12 +0000 extra"), ==, 0);
	g_assert_cmpint(axis_newznab_parse_date("Sat, 02 Mar 2024 10:11:12 +00"), ==, 0);
}

/* Task 2.3: by identifier or by words, as the capabilities say */
static void
test_search_paths(void)
{
	gsize n = 0;
	g_autofree gchar *caps_text = read_fixture("caps.xml", &n);
	g_autofree gchar *old_text = NULL;
	AxisIndexerCaps caps;
	AxisIndexerCaps old;
	AxisNewznabError document;
	AxisIndexerRequest request;
	AxisIndexerMode modes[3];
	const gint movies[] = { 2000, 2040 };
	const gint tv[] = { 5000 };
	g_autofree gchar *path = NULL;

	g_assert_cmpint(axis_newznab_parse_caps(caps_text, n, &caps, NULL, NULL, &document, NULL), ==,
		AXIS_NEWZNAB_READ_OK);
	caps.read_us = 1;
	old_text = read_fixture("caps-defaults.xml", &n);
	g_assert_cmpint(axis_newznab_parse_caps(old_text, n, &old, NULL, NULL, &document, NULL), ==, AXIS_NEWZNAB_READ_OK);
	old.read_us = 1;

	/* A movie: by its IMDb digits, in the movie categories. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
	request.title = "Hearth";
	request.year = 1995;
	request.imdb_id = "tt0600001";
	request.tmdb_id = 6001;
	g_assert_cmpuint(axis_newznab_identifiers(&request, &caps, modes), ==, 1);
	g_assert_cmpint(modes[0], ==, AXIS_INDEXER_BY_IMDB);
	path = axis_newznab_search_path(&request, modes[0], &caps, movies, 2);
	g_assert_cmpstr(path, ==, "/api?t=movie&imdbid=0600001&cat=2000%2C2040&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, &caps, movies, 1);
	g_assert_cmpstr(path, ==, "/api?t=movie&q=Hearth%201995&cat=2000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* Capabilities never read: words, by t=search, the default limit. */
	g_assert_cmpuint(axis_newznab_identifiers(&request, NULL, modes), ==, 0);
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, NULL, movies, 1);
	g_assert_cmpstr(path, ==, "/api?t=search&q=Hearth%201995&cat=2000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* A TMDb identifier this indexer does not take. */
	request.imdb_id = NULL;
	g_assert_cmpuint(axis_newznab_identifiers(&request, &caps, modes), ==, 0);

	/* An episode: by TVDB with its season and episode. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.tvdb_id = 82002;
	request.season = 1;
	request.episode = 2;
	g_assert_cmpuint(axis_newznab_identifiers(&request, &caps, modes), ==, 1);
	path = axis_newznab_search_path(&request, modes[0], &caps, tv, 1);
	g_assert_cmpstr(path, ==, "/api?t=tvsearch&tvdbid=82002&season=1&ep=2&cat=5000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* The old indexer takes no TVDB identifier: words, with the numbers. */
	g_assert_cmpuint(axis_newznab_identifiers(&request, &old, modes), ==, 0);
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, &old, tv, 1);
	g_assert_cmpstr(path, ==, "/api?t=tvsearch&q=The%20Show&season=1&ep=2&cat=5000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* Without capabilities: one line of words. */
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, NULL, tv, 1);
	g_assert_cmpstr(path, ==, "/api?t=search&q=The%20Show%20S01E02&cat=5000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* A season: no episode number. */
	request.kind = AXIS_INDEXER_QUERY_SEASON;
	request.episode = -1;
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TVDB, &caps, tv, 1);
	g_assert_cmpstr(path, ==, "/api?t=tvsearch&tvdbid=82002&season=1&cat=5000&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* Words as typed: no category unless asked. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = "hearth & dust";
	path = axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, &caps, NULL, 0);
	g_assert_cmpstr(path, ==, "/api?t=search&q=hearth%20%26%20dust&extended=1&limit=100&offset=0");
	g_clear_pointer(&path, g_free);
	/* Nothing to search by. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
	request.tmdb_id = 6001;
	g_assert_null(axis_newznab_search_path(&request, AXIS_INDEXER_BY_TEXT, &caps, movies, 1));
	path = axis_newznab_latest_path(movies, 2, 10);
	g_assert_cmpstr(path, ==, "/api?t=search&cat=2000%2C2040&extended=1&limit=10");
	axis_indexer_caps_clear(&caps);
	axis_indexer_caps_clear(&old);
}

static void
test_group_and_host_keys(void)
{
	g_autoptr(AxisIndexerRelease) a = axis_indexer_release_new();
	g_autoptr(AxisIndexerRelease) b = axis_indexer_release_new();
	g_autofree gchar *ka = NULL;
	g_autofree gchar *kb = NULL;
	g_autofree gchar *host = NULL;

	a->name = g_strdup("Hearth.1995.1080p-GRP");
	a->protocol = AXIS_DOWNLOAD_USENET;
	b->name = g_strdup("Hearth.1995.1080p-GRP");
	b->protocol = AXIS_DOWNLOAD_USENET;
	/* Usenet: no size, no group. */
	g_assert_null(axis_indexer_release_group_key(a));
	a->size_bytes = 100;
	b->size_bytes = 100;
	ka = axis_indexer_release_group_key(a);
	kb = axis_indexer_release_group_key(b);
	g_assert_cmpstr(ka, ==, kb);
	g_clear_pointer(&kb, g_free);
	b->size_bytes = 101;
	kb = axis_indexer_release_group_key(b);
	g_assert_cmpstr(ka, !=, kb);
	/* Torrents: by the hash, not the name. */
	g_clear_pointer(&ka, g_free);
	g_clear_pointer(&kb, g_free);
	a->protocol = AXIS_DOWNLOAD_TORRENT;
	g_assert_null(axis_indexer_release_group_key(a));
	a->info_hash = g_strdup("f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4");
	b->protocol = AXIS_DOWNLOAD_TORRENT;
	g_free(b->name);
	b->name = g_strdup("Another name");
	b->info_hash = g_strdup("f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4");
	ka = axis_indexer_release_group_key(a);
	kb = axis_indexer_release_group_key(b);
	g_assert_cmpstr(ka, ==, kb);

	host = axis_indexer_url_host_key("https://API.NewsOwl.example/");
	g_assert_cmpstr(host, ==, "newsowl.example");
	g_clear_pointer(&host, g_free);
	host = axis_indexer_host_key("www.dozynewt.example");
	g_assert_cmpstr(host, ==, "dozynewt.example");
	g_clear_pointer(&host, g_free);
	host = axis_indexer_host_key("api.example");
	g_assert_cmpstr(host, ==, "api.example");
	g_assert_null(axis_indexer_host_key("bad host"));
	g_assert_null(axis_indexer_url_host_key("not an address"));

	/* The name a site goes by (fix-first-contact 3), and a name made plain. */
	g_clear_pointer(&host, g_free);
	host = axis_indexer_site_name("newsowl.example");
	g_assert_cmpstr(host, ==, "newsowl");
	g_clear_pointer(&host, g_free);
	host = axis_indexer_site_name("dozynewt.example:8443");
	g_assert_cmpstr(host, ==, "dozynewt");
	g_clear_pointer(&host, g_free);
	host = axis_indexer_site_name("some.where.example.org");
	g_assert_cmpstr(host, ==, "example");
	g_clear_pointer(&host, g_free);
	g_assert_null(axis_indexer_site_name("127.0.0.1:7878"));
	g_assert_null(axis_indexer_site_name("[::1]"));
	g_assert_null(axis_indexer_site_name("localhost"));
	g_assert_null(axis_indexer_site_name("geek.co.example"));
	g_assert_null(axis_indexer_site_name(NULL));
	host = axis_indexer_plain_name("News-Owl (2)");
	g_assert_cmpstr(host, ==, "newsowl2");
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb       *db;
	AxisDbPool       *pool;
	AxisSettings     *settings;
	AxisIndexers     *indexers;
	AxisTestNewznab  *nzb;		/* a Newznab indexer */
	gint64            nzb_id;
	AxisTestNewznab  *other;	/* a second one, added by the tests that need it */
	AxisTestNewznab  *feed;		/* a Torznab feed, likewise */
	AxisTestProwlarr *prowlarr;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisIndexerTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 16);
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_no_error(error);
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_torznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	fx->nzb = axis_test_newznab_new(KEY, FALSE, "");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_indexers_unref(fx->indexers);
	axis_test_newznab_free(fx->nzb);
	axis_test_newznab_free(fx->other);
	axis_test_newznab_free(fx->feed);
	axis_test_prowlarr_free(fx->prowlarr);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static gint64
add_native(
	Fixture     *fx,
	const gchar *kind,
	const gchar *name,
	const gchar *address,
	const gchar *base_path,
	const gchar *key,
	gint         priority
){
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = kind;
	spec.address = address;
	spec.base_path = base_path;
	spec.enabled = TRUE;
	spec.secret = key;
	spec.priority = priority;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_nzb(
	Fixture         *fx,
	AxisTestNewznab *stand_in,
	const gchar     *name,
	gint             priority
){
	g_autofree gchar *base = axis_test_newznab_base(stand_in);

	return add_native(fx, AXIS_INDEXER_NEWZNAB_NAME, name, base, "", KEY, priority);
}

static void
film_request(AxisIndexerRequest *request)
{
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = "Hearth";
	request->year = 1995;
	request->imdb_id = "tt0600001";
	request->tmdb_id = 6001;
}

static AxisIndexerSearchResult *
search_ok(
	Fixture                  *fx,
	gint64                    source_id,
	const AxisIndexerRequest *request
){
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_search(fx->indexers, source_id, request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static void
fetch_ok(
	Fixture            *fx,
	gint64              release_id,
	AxisIndexerFetched *fetched
){
	g_autoptr(GError) error = NULL;

	if (!axis_indexers_fetch(fx->indexers, release_id, NULL, fetched, &error))
		g_error("the fetch failed: %s", error->message);
}

static const AxisIndexerRelease *
release_named(
	const AxisIndexerSearchResult *result,
	const gchar                   *name
){
	guint i;

	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (g_strcmp0(release->name, name) == 0)
			return release;
	}
	return NULL;
}

static const AxisIndexerSearchPart *
part_named(
	const AxisIndexerSearchResult *result,
	const gchar                   *name
){
	guint i;

	for (i = 0; result->parts != NULL && i < result->parts->len; i++) {
		const AxisIndexerSearchPart *part = g_ptr_array_index(result->parts, i);

		if (g_strcmp0(part->name, name) == 0)
			return part;
	}
	return NULL;
}

static void
assert_clean(const gchar *text)
{
	g_assert_nonnull(text);
	g_assert_null(strstr(text, KEY));
	g_assert_null(strstr(text, OTHER_KEY));
	g_assert_null(strstr(text, FEED_KEY));
	g_assert_null(strstr(text, "apikey"));
	g_assert_null(strstr(text, "t=get"));
	g_assert_null(strstr(text, "getnzb"));
	g_assert_null(strstr(text, "jackett_apikey"));
}

/* ── A Newznab indexer ──────────────────────────────────────────── */

/* Spec: Usenet indexer (its test); tasks 2.2 and 4.1 */
static void
test_a_test_reads_the_capabilities(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *caps_query = NULL;
	g_autofree gchar *test_query = NULL;
	const AxisIndexerInfo *self_info;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_true(source->native);
	g_assert_cmpint(source->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_null(source->state_message);
	g_assert_cmpstr(source->version, ==, "Stand-in Newznab 1.0");
	g_assert_cmpint(source->priority, ==, AXIS_INDEXER_DEFAULT_PRIORITY);
	g_assert_cmpuint(source->n_movie_categories, ==, 1);
	g_assert_cmpint(source->movie_categories[0], ==, 2000);
	g_assert_cmpint(source->tv_categories[0], ==, 5000);
	g_assert_cmpint(source->caps.read_us, >, 0);
	g_assert_null(source->caps.problem);
	g_assert_cmpuint(source->caps.movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpuint(source->caps.tv_ids, ==, AXIS_INDEXER_IDS_TVDB);
	g_assert_true(source->caps.tv_numbers);
	g_assert_cmpint(source->caps.limit_max, ==, 100);
	g_assert_nonnull(source->caps.categories);
	/* The test's search for what is newest stated the allowances, and one
	 * of its results named another address for its file: offered, as an
	 * origin (no path, no key), not accepted. */
	g_assert_cmpint(source->allowance.api_used, ==, 12);
	g_assert_cmpint(source->allowance.api_max, ==, 5000);
	g_assert_cmpint(source->allowance.grab_max, ==, 100);
	g_assert_cmpint(source->allowance.reported_us, >, 0);
	g_assert_cmpstr(source->files_origin_seen, ==, "https://files.elsewhere.test:443");
	g_assert_null(source->files_origin);
	/* It reports itself, with what it takes, at its host. */
	g_assert_cmpuint(source->indexers->len, ==, 1);
	self_info = g_ptr_array_index(source->indexers, 0);
	g_assert_cmpstr(self_info->name, ==, "NewsOwl");
	g_assert_true(self_info->enabled);
	g_assert_cmpuint(self_info->movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpstr(self_info->host, ==, "127.0.0.1");
	/* Two requests, each with the key in the query and nowhere else. */
	g_assert_cmpuint(axis_test_newznab_n_caps(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);
	caps_query = axis_test_newznab_query(fx->nzb, 0);
	g_assert_cmpstr(caps_query, ==, "t=caps&apikey=" KEY);
	test_query = axis_test_newznab_query(fx->nzb, 1);
	g_assert_cmpstr(test_query, ==, "t=search&cat=2000%2C5000&extended=1&limit=10&apikey=" KEY);
	g_assert_false(axis_test_plex_saw_non_get(axis_test_newznab_server(fx->nzb)));
	assert_q(fx, "f", "SELECT follow_redirects FROM indexer_source");
	assert_q(fx, "usenet", "SELECT protocol FROM indexer_source");
}

/* An indexer that does not answer its capabilities is searched by words,
 * and says so. */
static void
test_without_capabilities_words_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "Old one", 0);
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_NO_CAPS);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_nonnull(source->caps.problem);
	g_assert_nonnull(strstr(source->state_message, "searched by words only"));
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_NORMAL);
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TEXT);
	query = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(query, ==, "t=search&q=Hearth%201995&cat=2000&extended=1&limit=100&offset=0&apikey=" KEY);
	g_clear_pointer(&source, axis_indexer_source_info_free);

	/* An answer that is not XML: the same. */
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_JUNK_CAPS);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_nonnull(strstr(source->caps.problem, "cannot be read"));
}

/* Spec: Failure message; task 4.3 (a refused key says so, without it) */
static void
test_a_wrong_key_is_refused_without_naming_it(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *result = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	axis_test_newznab_set_key(fx->nzb, OTHER_KEY);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_REFUSED);
	g_assert_nonnull(strstr(source->state_message, "NewsOwl refused the key"));
	g_assert_nonnull(strstr(source->state_message, "Incorrect user credentials"));
	assert_clean(source->state_message);
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH);
	g_assert_nonnull(strstr(error->message, "NewsOwl"));
	assert_clean(error->message);
}

/* Task 2.3: the searches as the capabilities say, and the answer mapped */
static void
test_searches_by_identifier_or_words(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) film = NULL;
	g_autoptr(AxisIndexerSearchResult) episode = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = NULL;
	const AxisIndexerRelease *release;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	film_request(&request);
	film = search_ok(fx, 0, &request);
	query = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(query, ==, "t=movie&imdbid=0600001&cat=2000&extended=1&limit=100&offset=0&apikey=" KEY);
	g_assert_cmpint(film->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_false(film->fell_back);
	g_assert_cmpuint(film->releases->len, ==, 6);
	g_assert_cmpuint(film->n_off_origin, ==, 1);
	g_assert_cmpuint(film->n_unusable, ==, 1);
	g_assert_cmpuint(film->n_skipped, ==, 1);
	release = release_named(film, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(release);
	g_assert_cmpstr(release->indexer_name, ==, "NewsOwl");
	g_assert_cmpint(release->indexer_id, ==, 0);
	g_assert_cmpint(release->source_id, ==, fx->nzb_id);
	g_assert_cmpint(release->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpuint(film->parts->len, ==, 1);
	g_assert_cmpint(film->search_id, >, 0);
	g_assert_cmpint(film->source_id, ==, fx->nzb_id);
	/* Nothing in the database but the secrets tables holds the key. */
	assert_q(fx, "6", "SELECT count(*) FROM indexer_release_secret WHERE fetch_ref LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release WHERE name LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE coalesce(state_message, '') LIKE '%" KEY "%'"
		" OR coalesce(files_origin_seen, '') LIKE '%" KEY "%' OR address LIKE '%" KEY "%'");
	/* The allowances the answer stated, and the calls since. */
	assert_q(fx, "12|5000|3|100", "SELECT api_used || '|' || api_max || '|' || grab_used || '|' || grab_max"
		" FROM indexer_source");

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.tvdb_id = 82002;
	request.imdb_id = "tt0700002";
	request.season = 1;
	request.episode = 2;
	episode = search_ok(fx, 0, &request);
	g_clear_pointer(&query, g_free);
	query = axis_test_newznab_last_search(fx->nzb);
	g_assert_cmpstr(query, ==, "t=tvsearch&tvdbid=82002&season=1&ep=2&cat=5000&extended=1&limit=100&offset=0&apikey="
		KEY);
	g_assert_cmpint(episode->mode, ==, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpuint(episode->releases->len, ==, 2);
	release = release_named(episode, "The.Show.S01E02.1080p.WEB-DL.DDP5.1.H.264-GRP");
	g_assert_nonnull(release);
	g_assert_cmpint(release->tvdb_id, ==, 82002);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);
}

/* Task 2.4: fetched from the indexer, checked by its bytes */
static void
test_fetch_from_the_indexer(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	film_request(&request);
	result = search_ok(fx, 0, &request);
	release = release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP");
	fetch_ok(fx, release->id, &fetched);
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_cmpint(axis_indexer_content_kind(g_bytes_get_data(fetched.file, NULL), g_bytes_get_size(fetched.file)), ==,
		AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpstr(fetched.file_name, ==, "Hearth.1995.1080p.BluRay.x264-GRP.nzb");
	g_assert_cmpint(fetched.source_id, ==, fx->nzb_id);
	axis_indexer_fetched_clear(&fetched);
	/* The other address form. */
	release = release_named(result, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	fetch_ok(fx, release->id, &fetched);
	g_assert_nonnull(fetched.file);
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, 2);
	/* A grab is counted until the next report. */
	assert_q(fx, "5", "SELECT grab_used FROM indexer_source");

	/* What is not an NZB is refused; an error document says what it is. */
	{
		g_autofree gchar *items = axis_test_newznab_item(fx->nzb, "Hearth.1995.HTML-PAGE", "p1", 1000, "html-p1");
		g_autofree gchar *more = axis_test_newznab_item(fx->nzb, "Hearth.1995.GONE", "p2", 1001, "gone-p2");
		g_autofree gchar *both = g_strconcat(items, more, NULL);
		g_autofree gchar *feed = axis_test_newznab_feed(both, 0, 0, 0, 0);
		g_autoptr(AxisIndexerSearchResult) other = NULL;

		axis_test_newznab_set_search(fx->nzb, NULL, feed);
		request.year = 1996;
		other = search_ok(fx, 0, &request);
		release = release_named(other, "Hearth.1995.HTML-PAGE");
		g_assert_false(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
		g_assert_nonnull(strstr(error->message, "not an NZB"));
		assert_clean(error->message);
		g_clear_error(&error);
		release = release_named(other, "Hearth.1995.GONE");
		g_assert_false(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
		g_assert_nonnull(strstr(error->message, "error 300: No such item"));
		assert_clean(error->message);
	}
}

/* ── Paging (refine-indexer-followups 1.1) ──────────────────────── */

/* One answer of @n results, from number @first, saying it has @total in all
 * (-1: it says nothing). */
static gchar *
page_feed(
	Fixture *fx,
	guint    first,
	guint    n,
	gint64   total
){
	GString *items = g_string_new(NULL);
	g_autofree gchar *response = total >= 0 ? g_strdup_printf("<newznab:response offset=\"0\" total=\"%"
		G_GINT64_FORMAT "\"/>", total) : g_strdup("");
	guint i;

	for (i = first; i < first + n; i++) {
		g_autofree gchar *title = g_strdup_printf("Hearth.1995.1080p.WEB-P%u", i);
		g_autofree gchar *guid = g_strdup_printf("g%u", i);
		g_autofree gchar *id = g_strdup_printf("nzb-g%u", i);
		g_autofree gchar *item = axis_test_newznab_item(fx->nzb, title, guid, 1000 + i, id);

		g_string_append(items, item);
	}
	{
		gchar *feed = g_strdup_printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\" "
			"xmlns:newznab=\"http://www.newznab.com/DTD/2010/feeds/attributes/\"><channel><title>Stand-in</title>%s%s"
			"</channel></rss>\n", response, items->str);

		g_string_free(items, TRUE);
		return feed;
	}
}

/* The capabilities of the Newznab stand-in with pages of at most @limit
 * results. */
static void
small_pages(
	Fixture *fx,
	guint    limit
){
	g_autofree gchar *caps = axis_test_newznab_fixture(fx->nzb, "caps.xml");
	g_autofree gchar *from = g_strdup_printf("<limits max=\"%u\" default=\"%u\"/>", limit, limit);
	gchar *at = strstr(caps, "<limits max=\"100\" default=\"50\"/>");
	g_autofree gchar *changed = NULL;

	g_assert_nonnull(at);
	*at = '\0';
	changed = g_strconcat(caps, from, at + strlen("<limits max=\"100\" default=\"50\"/>"), NULL);
	axis_test_newznab_set_caps(fx->nzb, changed);
}

static void
set_page(
	Fixture *fx,
	guint    index,
	guint    first,
	guint    n,
	gint64   total
){
	g_autofree gchar *feed = page_feed(fx, first, n, total);

	axis_test_newznab_set_search_page(fx->nzb, index, feed);
}

static gboolean
asked_at_offset(
	Fixture *fx,
	guint    search,
	guint    offset
){
	g_autofree gchar *query = axis_test_newznab_query(fx->nzb, search + 1);
	g_autofree gchar *needle = g_strdup_printf("&offset=%u&", offset);

	return query != NULL && strstr(query, needle) != NULL;
}

/* A search reads on while the indexer says there are more, stepping by the
 * results it returned; a result that appears on two pages is one. */
static void
test_paging_follows_the_total(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	small_pages(fx, 3);
	set_page(fx, 0, 0, 3, 8);
	/* Result 2 is on the second page as well. */
	set_page(fx, 1, 2, 3, 8);	set_page(fx, 2, 5, 3, 8);
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 3);
	g_assert_true(asked_at_offset(fx, 0, 0));
	g_assert_true(asked_at_offset(fx, 1, 3));
	g_assert_true(asked_at_offset(fx, 2, 6));
	/* Results 0 to 7: the one on two pages (2) is one. */
	g_assert_cmpuint(result->releases->len, ==, 8);
	g_assert_nonnull(release_named(result, "Hearth.1995.1080p.WEB-P7"));
	g_assert_false(result->truncated);
}

/* No total stated: until a page comes back short. */
static void
test_paging_stops_at_a_short_page(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	small_pages(fx, 3);
	set_page(fx, 0, 0, 3, -1);
	set_page(fx, 1, 3, 2, -1);
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 2);
	g_assert_cmpuint(result->releases->len, ==, 5);
	g_assert_false(result->truncated);
}

/* The cap on pages: what was read is kept and the result says it is cut. */
static void
test_paging_stops_at_the_cap_of_pages(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;
	guint i;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	small_pages(fx, 3);
	for (i = 0; i < 8; i++)
		set_page(fx, i, i * 3, 3, 1000);
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, AXIS_NEWZNAB_MAX_PAGES);
	g_assert_cmpuint(result->releases->len, ==, 3 * AXIS_NEWZNAB_MAX_PAGES);
	g_assert_true(result->truncated);
}

/* A later page that fails keeps the earlier ones and says it is cut; the
 * first page that fails is the search's failure. */
static void
test_a_later_page_that_fails_keeps_what_was_read(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) none = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	small_pages(fx, 3);
	set_page(fx, 0, 0, 3, 9);
	axis_test_newznab_set_search_page(fx->nzb, 1, "@FAIL@");
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpuint(result->releases->len, ==, 3);
	g_assert_true(result->truncated);

	/* The first page failing is the search failing. */
	axis_test_newznab_set_search_page(fx->nzb, 2, "@FAIL@");
	request.year = 1996;
	g_assert_false(axis_indexers_search(fx->indexers, fx->nzb_id, &request, NULL, &none, &error));
	g_assert_nonnull(error);
}

/* An indexer that ignores the offset answers the same page again: nothing
 * new, so no more pages (and no endless reading). */
static void
test_paging_stops_when_a_page_is_nothing_new(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *same = NULL;
	AxisIndexerRequest request;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	small_pages(fx, 3);
	same = page_feed(fx, 0, 3, -1);
	axis_test_newznab_set_search(fx->nzb, NULL, same);
	film_request(&request);
	result = search_ok(fx, fx->nzb_id, &request);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 2);
	g_assert_cmpuint(result->releases->len, ==, 3);
	g_assert_false(result->truncated);
}

/* A Torznab feed whose API is not at /api: the path in front of /api is the
 * base path; pages follow it, and one torrent under two guids is one. */
static void
test_a_torznab_feed_at_another_path_pages_by_hash(Fixture *fx, gconstpointer data)
{
	static const gchar *const hash = "f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4";
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *caps = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRequest request;
	gchar *at;
	gint64 id;

	fx->feed = axis_test_newznab_new(FEED_KEY, TRUE, "/api/v1");
	base = axis_test_newznab_base(fx->feed);
	caps = axis_test_newznab_fixture(fx->feed, "torznab-caps.xml");
	at = strstr(caps, "<limits default=\"100\" max=\"100\" />");
	g_assert_nonnull(at);
	*at = '\0';
	{
		g_autofree gchar *changed = g_strconcat(caps, "<limits default=\"2\" max=\"2\" />",
			at + strlen("<limits default=\"100\" max=\"100\" />"), NULL);

		axis_test_newznab_set_caps(fx->feed, changed);
	}
	first = g_strdup_printf("<?xml version=\"1.0\"?><rss version=\"2.0\" "
		"xmlns:torznab=\"http://torznab.com/schemas/2015/feed\"><channel><title>x</title>"
		"<torznab:response offset=\"0\" total=\"4\"/>"
		"<item><title>Hearth.1995.1080p.A-GRP</title><guid>a1</guid><pubDate>Sat, 02 Mar 2024 10:11:12 +0000</pubDate>"
		"<size>1000</size><link>magnet:?xt=urn:btih:%s&amp;dn=A</link>"
		"<torznab:attr name=\"seeders\" value=\"5\"/><torznab:attr name=\"infohash\" value=\"%s\"/></item>"
		"<item><title>Hearth.1995.1080p.B-GRP</title><guid>b1</guid><pubDate>Sat, 02 Mar 2024 10:11:12 +0000</pubDate>"
		"<size>1000</size><link>magnet:?xt=urn:btih:e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4&amp;dn=B</link>"
		"<torznab:attr name=\"seeders\" value=\"5\"/>"
		"<torznab:attr name=\"infohash\" value=\"e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4\"/></item>"
		"</channel></rss>", hash, hash);
	/* The same torrent again, under another guid, and a new one. */
	second = g_strdup_printf("<?xml version=\"1.0\"?><rss version=\"2.0\" "
		"xmlns:torznab=\"http://torznab.com/schemas/2015/feed\"><channel><title>x</title>"
		"<torznab:response offset=\"2\" total=\"4\"/>"
		"<item><title>Hearth.1995.1080p.A-GRP</title><guid>a2</guid><pubDate>Sat, 02 Mar 2024 10:11:12 +0000</pubDate>"
		"<size>1000</size><link>magnet:?xt=urn:btih:%s&amp;dn=A</link>"
		"<torznab:attr name=\"seeders\" value=\"5\"/><torznab:attr name=\"infohash\" value=\"%s\"/></item>"
		"<item><title>Hearth.1995.1080p.C-GRP</title><guid>c1</guid><pubDate>Sat, 02 Mar 2024 10:11:12 +0000</pubDate>"
		"<size>1000</size><link>magnet:?xt=urn:btih:3a3b3c3d3a3b3c3d3a3b3c3d3a3b3c3d3a3b3c3d&amp;dn=C</link>"
		"<torznab:attr name=\"seeders\" value=\"5\"/>"
		"<torznab:attr name=\"infohash\" value=\"3a3b3c3d3a3b3c3d3a3b3c3d3a3b3c3d3a3b3c3d\"/></item>"
		"</channel></rss>", hash, hash);
	axis_test_newznab_set_search_page(fx->feed, 0, first);
	axis_test_newznab_set_search_page(fx->feed, 1, second);
	id = add_native(fx, AXIS_INDEXER_TORZNAB_NAME, "Jackett: tidepool", base, "/api/v1", FEED_KEY, 0);
	film_request(&request);
	result = search_ok(fx, id, &request);
	/* Asked under the base path, in front of /api. */
	query = axis_test_newznab_last_search(fx->feed);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->feed), ==, 2);
	g_assert_nonnull(strstr(query, "&offset=2&"));
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->feed), ==, 0);
	g_assert_cmpuint(result->releases->len, ==, 3);
	g_assert_nonnull(release_named(result, "Hearth.1995.1080p.C-GRP"));
	g_assert_false(result->truncated);
}

/* Task 4.6: an enclosure on another origin is not fetched unless accepted
 * for that indexer */
static void
test_files_elsewhere_only_once_accepted(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *files = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(AxisIndexerSearchResult) before = NULL;
	g_autoptr(AxisIndexerSearchResult) after = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *expected = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;

	/* Its results name another stand-in for their files. */
	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	files = axis_test_newznab_base(fx->other);
	axis_test_newznab_set_files(fx->nzb, files);
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	expected = g_strdup(files);
	/* Shown, as an origin (no path, no key), and not accepted. */
	g_assert_cmpstr(source->files_origin_seen, ==, expected);
	g_assert_null(source->files_origin);
	film_request(&request);
	before = search_ok(fx, 0, &request);
	g_assert_cmpuint(before->releases->len, ==, 0);
	g_assert_cmpuint(before->n_off_origin, ==, 8);
	/* Nothing was ever asked of the other place. */
	g_assert_cmpuint(axis_test_newznab_n_requests(fx->other), ==, 0);
	/* A later answer never makes it accepted. */
	g_clear_pointer(&source, axis_indexer_source_info_free);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_null(source->files_origin);

	/* Accepted: the search is made again, and the file fetched there, with
	 * nothing of the indexer's but what the address holds. */
	g_assert_true(axis_indexers_accept_files_origin(fx->indexers, fx->nzb_id, &error));
	g_assert_no_error(error);
	g_assert_false(axis_indexers_accept_files_origin(fx->indexers, fx->nzb_id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_clear_error(&error);
	after = search_ok(fx, 0, &request);
	g_assert_false(after->cached);
	g_assert_cmpuint(after->releases->len, ==, 6);
	release = release_named(after, "Hearth.1995.1080p.BluRay.x264-GRP");
	fetch_ok(fx, release->id, &fetched);
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->other), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, 0);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->other), ==, 0);
	assert_q(fx, "t", "SELECT bool_and(files_origin) FROM indexer_release_secret");

	/* No longer accepted (cleared in its settings): its stored releases
	 * there are not fetched, and nothing is sent. */
	{
		AxisIndexerSourceSpec spec;
		g_autofree gchar *base = axis_test_newznab_base(fx->nzb);
		g_autoptr(AxisIndexerSearchResult) third = NULL;

		memset(&spec, 0, sizeof(spec));
		spec.name = "NewsOwl";
		spec.address = base;
		spec.enabled = TRUE;
		g_assert_true(axis_indexers_save_source(fx->indexers, fx->nzb_id, &spec, NULL, &error));
		g_assert_no_error(error);
		assert_q(fx, NULL, "SELECT files_origin FROM indexer_source");
		/* The changed origin of files forgot the stored searches. */
		assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
		third = search_ok(fx, 0, &request);
		g_assert_cmpuint(third->releases->len, ==, 0);
	}
	/* The accepted origin, typed in the settings, is accepted as well. */
	{
		AxisIndexerSourceSpec spec;
		g_autofree gchar *base = axis_test_newznab_base(fx->nzb);

		memset(&spec, 0, sizeof(spec));
		spec.name = "NewsOwl";
		spec.address = base;
		spec.enabled = TRUE;
		spec.files_origin = "ftp://nope";
		g_assert_false(axis_indexers_save_source(fx->indexers, fx->nzb_id, &spec, NULL, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
		g_clear_error(&error);
		spec.files_origin = files;
		g_assert_true(axis_indexers_save_source(fx->indexers, fx->nzb_id, &spec, NULL, &error));
		assert_q(fx, expected, "SELECT files_origin FROM indexer_source");
	}
}

/* Task 2.4: a Torznab feed's torrent file, magnet link, or download that
 * answers with a magnet link */
static void
test_a_torznab_feed(Fixture *fx, gconstpointer data)
{
	const gchar *prefix = "/api/v2.0/indexers/tidepool/results/torznab";
	g_autofree gchar *base = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 id;

	fx->feed = axis_test_newznab_new(FEED_KEY, TRUE, prefix);
	base = axis_test_newznab_base(fx->feed);
	id = add_native(fx, AXIS_INDEXER_TORZNAB_NAME, "Jackett: tidepool", base, prefix, FEED_KEY, 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_cmpint(source->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpstr(source->version, ==, "Jackett");
	g_assert_cmpuint(source->caps.movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpuint(source->caps.tv_ids, ==, 0);
	/* Its files are on its own origin, outside the feed's path: nothing to
	 * accept. */
	g_assert_null(source->files_origin_seen);
	film_request(&request);
	result = search_ok(fx, id, &request);
	query = axis_test_newznab_last_search(fx->feed);
	g_assert_cmpstr(query, ==, "t=movie&imdbid=0600001&cat=2000&extended=1&limit=100&offset=0&apikey=" FEED_KEY);
	g_assert_cmpuint(result->releases->len, ==, 3);
	release = release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_cmpint(release->seeders, ==, 85);
	g_assert_cmpstr(release->flags[0], ==, "freeleech");
	fetch_ok(fx, release->id, &fetched);
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_null(fetched.magnet);
	axis_indexer_fetched_clear(&fetched);
	release = release_named(result, "Hearth (1995) 720p BRRip x264 -MAG");
	g_assert_true(release->has_magnet);
	fetch_ok(fx, release->id, &fetched);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, "e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4");
	axis_indexer_fetched_clear(&fetched);
	/* A download that answers with its magnet link: read, checked against
	 * the stated hash, never followed. */
	release = release_named(result, "Hearth.1995.REMASTERED.1080p-RDR");
	fetch_ok(fx, release->id, &fetched);
	g_assert_no_error(error);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, "cccccccccccccccccccccccccccccccccccccccc");
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->feed), ==, 0);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->feed), ==, 2);
}

/* ── Limits ─────────────────────────────────────────────────────── */

/* Task 2.5: "limit reached" pauses the indexer, and says so */
static void
test_a_limit_reached_holds_the_indexer(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *none = NULL;
	AxisIndexerRequest request;
	guint before;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	add_nzb(fx, fx->other, "DozyNewt", 0);
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_LIMIT);
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, fx->nzb_id, &request, NULL, &none, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "request limit is reached"));
	g_assert_nonnull(strstr(error->message, "error 500: Request limit reached"));
	assert_clean(error->message);
	g_clear_error(&error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_cmpint(source->held_until_us, >, g_get_real_time() + G_GINT64_CONSTANT(3000) * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(source->hold_message, "request limit is reached"));
	/* A rate limit is not a failure of the indexer's. */
	g_assert_cmpint(source->state, !=, AXIS_INDEXER_SOURCE_FAILING);
	g_assert_true(axis_indexer_source_spent(source, FALSE, g_get_real_time()));

	/* Held: not asked, by a search of them all or of it; the other is. */
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_NORMAL);
	before = axis_test_newznab_n_requests(fx->nzb);
	result = search_ok(fx, 0, &request);
	g_assert_cmpuint(axis_test_newznab_n_requests(fx->nzb), ==, before);
	g_assert_false(part_named(result, "NewsOwl")->asked);
	g_assert_nonnull(strstr(part_named(result, "NewsOwl")->problem, "request limit is reached"));
	g_assert_true(part_named(result, "DozyNewt")->asked);
	g_assert_cmpuint(result->releases->len, ==, 6);
	g_assert_false(axis_indexers_search(fx->indexers, fx->nzb_id, &request, NULL, &none, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpuint(axis_test_newznab_n_requests(fx->nzb), ==, before);
}

/* Spec: Allowance; task 2.5: automatic searches stop short of it, a person's
 * do not; the download limit holds its files */
static void
test_allowances_are_kept_to(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *item = NULL;
	g_autofree gchar *feed = NULL;
	g_autoptr(AxisIndexerSearchResult) first = NULL;
	g_autoptr(AxisIndexerSearchResult) manual = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *none = NULL;
	AxisIndexerFetched fetched;
	AxisIndexerRequest request;
	guint before;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	item = axis_test_newznab_item(fx->nzb, "Hearth.1995.1080p.BluRay.x264-GRP", "a1", 8000000000, "limit-a1");
	/* 4,996 of 5,000 calls used: within the reserve. */
	feed = axis_test_newznab_feed(item, 4996, 5000, 10, 100);
	axis_test_newznab_set_search(fx->nzb, NULL, feed);
	film_request(&request);
	first = search_ok(fx, 0, &request);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->nzb_id, &source, &error));
	g_assert_cmpint(source->allowance.api_used, ==, 4996);
	g_assert_true(axis_indexer_source_spent(source, TRUE, g_get_real_time()));
	g_assert_false(axis_indexer_source_spent(source, FALSE, g_get_real_time()));

	/* An automatic search passes it over, saying why; a person's asks. */
	before = axis_test_newznab_n_requests(fx->nzb);
	request.year = 1996;
	request.automatic = TRUE;
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &none, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "4996 of its 5000 API calls"));
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_newznab_n_requests(fx->nzb), ==, before);
	request.automatic = FALSE;
	manual = search_ok(fx, 0, &request);
	g_assert_cmpuint(axis_test_newznab_n_requests(fx->nzb), ==, before + 1);

	/* The download limit: the file is not given, and no other is asked for
	 * from it until it resets. */
	g_assert_false(axis_indexers_fetch(fx->indexers, ((const AxisIndexerRelease *)g_ptr_array_index(manual->releases,
		0))->id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "download limit is reached"));
	assert_clean(error->message);
	g_clear_error(&error);
	before = axis_test_newznab_n_files(fx->nzb);
	g_assert_false(axis_indexers_fetch(fx->indexers, ((const AxisIndexerRelease *)g_ptr_array_index(first->releases,
		0))->id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, before);
}

/* ── All of them at once ────────────────────────────────────────── */

/* Spec: One indexer down; task 4.4: one slow or failing indexer does not
 * delay or fail the others beyond its deadline */
static void
test_one_down_does_not_hold_the_others(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerTuning tuning;
	AxisIndexerRequest request;
	const AxisIndexerSearchPart *down;
	const AxisIndexerSearchPart *up;
	gint64 start;
	gint64 took;

	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	add_nzb(fx, fx->other, "DozyNewt", 0);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_DOWN);
	film_request(&request);
	start = g_get_monotonic_time();
	result = search_ok(fx, 0, &request);
	took = g_get_monotonic_time() - start;
	/* The one that answered, and the other's failure stated. */
	g_assert_cmpuint(result->releases->len, ==, 6);
	g_assert_cmpuint(result->parts->len, ==, 2);
	down = part_named(result, "NewsOwl");
	up = part_named(result, "DozyNewt");
	g_assert_true(down->asked);
	g_assert_nonnull(down->problem);
	assert_clean(down->problem);
	g_assert_null(up->problem);
	g_assert_cmpuint(up->n_releases, ==, 6);
	g_assert_cmpint(result->search_id, ==, 0);
	/* Within the one that did not answer's own deadline (it is asked for
	 * its capabilities first, never read: two requests), with time to
	 * spare for a loaded machine; never the others' sum. */
	g_assert_cmpint(took, <, G_GINT64_CONSTANT(6000000));
	assert_q(fx, "unreachable", "SELECT state FROM indexer_source WHERE name = 'NewsOwl'");
	assert_q(fx, "ok", "SELECT state FROM indexer_source WHERE name = 'DozyNewt'");
}

/* Task 4.4: they are asked at once, each under its own limiter */
static void
test_they_are_asked_at_once(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 start;
	gint64 took;

	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	add_nzb(fx, fx->other, "DozyNewt", 0);
	/* Their capabilities read first, so that a search is one request. */
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id + 1, NULL, &error));
	axis_test_newznab_set_delay(fx->nzb, 2000);
	axis_test_newznab_set_delay(fx->other, 2000);
	film_request(&request);
	start = g_get_monotonic_time();
	result = search_ok(fx, 0, &request);
	took = g_get_monotonic_time() - start;
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 2);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->other), ==, 2);
	/* One after the other would be 4 s. */
	g_assert_cmpint(took, >=, G_GINT64_CONSTANT(1900000));
	g_assert_cmpint(took, <, G_GINT64_CONSTANT(3700000));
	/* The same releases at both: listed once, the other's its alternative;
	 * but the one that states no size is not known to be the same, and is
	 * listed from each. */
	g_assert_cmpuint(result->releases->len, ==, 7);
	g_assert_cmpuint(result->n_merged, ==, 5);
}

/* Task 4.4: limits hold per indexer: each indexer's own rate, whatever the
 * other does */
static void
test_limits_hold_per_indexer(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) first = NULL;
	g_autoptr(GArray) second = NULL;
	AxisIndexerRequest request;
	gint64 other;
	guint i;

	/* One request a second, one at a time, for each. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_RATE, "1", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_BURST, "1", &error));
	g_assert_no_error(error);
	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	other = add_nzb(fx, fx->other, "DozyNewt", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	g_assert_true(axis_indexers_test_source(fx->indexers, other, NULL, &error));
	axis_test_plex_reset_requests(axis_test_newznab_server(fx->nzb));
	axis_test_plex_reset_requests(axis_test_newznab_server(fx->other));
	film_request(&request);
	for (i = 0; i < 3; i++) {
		AxisIndexerSearchResult *result;

		request.year = 1995 + (gint)i;
		result = search_ok(fx, 0, &request);
		axis_indexer_search_result_free(result);
	}
	first = axis_test_plex_request_times(axis_test_newznab_server(fx->nzb));
	second = axis_test_plex_request_times(axis_test_newznab_server(fx->other));
	g_assert_cmpuint(first->len, ==, 3);
	g_assert_cmpuint(second->len, ==, 3);
	for (i = 1; i < 3; i++) {
		/* Each one's searches a second apart (less the clock's slack)... */
		g_assert_cmpint(g_array_index(first, gint64, i) - g_array_index(first, gint64, i - 1), >=,
			G_GINT64_CONSTANT(850000));
		g_assert_cmpint(g_array_index(second, gint64, i) - g_array_index(second, gint64, i - 1), >=,
			G_GINT64_CONSTANT(850000));
	}
	/* ...and the two asked side by side, not one after the other: three
	 * rounds of both took about two seconds, not five. */
	g_assert_cmpint(MAX(g_array_index(first, gint64, 2), g_array_index(second, gint64, 2)) -
		MIN(g_array_index(first, gint64, 0), g_array_index(second, gint64, 0)), <, G_GINT64_CONSTANT(3500000));
}

/* Task 4.5: de-duplication and alternatives; a release fetched from the
 * second indexer when the first fails */
static void
test_the_same_release_at_two_indexers(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;
	g_autofree gchar *a_only = NULL;
	g_autofree gchar *feed_a = NULL;
	g_autofree gchar *feed_b = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 second;

	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	/* The first (priority 10) has it, but cannot give it; the second (20)
	 * has the same name and size, under its own guid, and can. */
	a = axis_test_newznab_item(fx->nzb, "Hearth.1995.1080p.BluRay.x264-GRP", "at-a", 8912345678, "gone-a");
	a_only = axis_test_newznab_item(fx->nzb, "Hearth.1995.720p.Only.At.A", "only-a", 4000000000, "nzb-only");
	b = axis_test_newznab_item(fx->other, "Hearth.1995.1080p.BluRay.x264-GRP", "at-b", 8912345678, "nzb-b");
	feed_a = axis_test_newznab_feed(a, 0, 0, 0, 0);
	{
		g_autofree gchar *both = g_strconcat(a, a_only, NULL);

		g_free(feed_a);
		feed_a = axis_test_newznab_feed(both, 0, 0, 0, 0);
	}
	feed_b = axis_test_newznab_feed(b, 0, 0, 0, 0);
	axis_test_newznab_set_search(fx->nzb, NULL, feed_a);
	axis_test_newznab_set_search(fx->other, NULL, feed_b);
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 10);
	second = add_nzb(fx, fx->other, "DozyNewt", 20);
	film_request(&request);
	result = search_ok(fx, 0, &request);
	g_assert_cmpuint(result->releases->len, ==, 2);
	g_assert_cmpuint(result->n_merged, ==, 1);
	release = release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_cmpint(release->source_id, ==, fx->nzb_id);
	g_assert_cmpstr(release->indexer_name, ==, "NewsOwl");
	g_assert_cmpuint(release->n_alternatives, ==, 1);
	g_assert_cmpstr(release->also_at[0], ==, "DozyNewt");
	g_assert_null(release->also_at[1]);
	g_assert_cmpstr(((const AxisIndexerSearchPart *)g_ptr_array_index(result->parts, 0))->name, ==, "NewsOwl");

	/* The first says it has no such item: fetched from the second. */
	fetch_ok(fx, release->id, &fetched);
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_cmpint(fetched.source_id, ==, second);
	g_assert_cmpint(fetched.release_id, !=, release->id);
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->nzb), ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_files(fx->other), ==, 1);

	/* Neither can: the first's error, saying the other was tried. */
	axis_test_newznab_set_mode(fx->other, AXIS_TEST_NEWZNAB_BROKEN);
	g_assert_false(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "nor could it be fetched from the 1 other indexer"));
	assert_clean(error->message);
	g_clear_error(&error);
	/* One only at the first is not tried elsewhere. */
	axis_test_newznab_set_mode(fx->other, AXIS_TEST_NEWZNAB_NORMAL);
	release = release_named(result, "Hearth.1995.720p.Only.At.A");
	g_assert_cmpuint(release->n_alternatives, ==, 0);
	fetch_ok(fx, release->id, &fetched);
	axis_indexer_fetched_clear(&fetched);
}

/* Spec: Both; task 1.3: a native indexer and the same one through Prowlarr
 * (the same host) are asked once, natively */
static void
test_the_same_indexer_natively_and_through_prowlarr(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *prowlarr_base = NULL;
	g_autofree gchar *indexers = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *nzb_base = axis_test_newznab_base(fx->nzb);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 prowlarr;
	guint i;

	fx->prowlarr = axis_test_prowlarr_new(PROWLARR_KEY, "");
	prowlarr_base = axis_test_prowlarr_base(fx->prowlarr);
	/* Prowlarr has NewsOwl (3, at the native one's host) and a tracker. */
	indexers = g_strdup_printf("[{\"id\":3,\"name\":\"NewsOwl\",\"protocol\":\"usenet\",\"enable\":true,"
		"\"indexerUrls\":[\"%s/\"],\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}},"
		"{\"id\":5,\"name\":\"Tracker One\",\"protocol\":\"torrent\",\"enable\":true,"
		"\"fields\":[{\"name\":\"baseUrl\",\"value\":\"https://tracker-one.test/\"},{\"name\":\"apiKey\","
		"\"value\":\"********\"}],\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}}]", nzb_base);
	axis_test_prowlarr_set_indexers(fx->prowlarr, indexers);
	prowlarr = add_native(fx, AXIS_INDEXER_PROWLARR_NAME, "Prowlarr", prowlarr_base, "", PROWLARR_KEY, 0);
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, prowlarr, NULL, &error));
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	assert_q(fx, "127.0.0.1|tracker-one.test", "SELECT string_agg(coalesce(host, '-'), '|' ORDER BY indexer_id)"
		" FROM indexer_source_indexer WHERE source_id = (SELECT id FROM indexer_source WHERE kind = 'prowlarr')");

	film_request(&request);
	result = search_ok(fx, 0, &request);
	/* Prowlarr was asked for the tracker only. */
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, 1);
	query = axis_test_prowlarr_search_query(fx->prowlarr, 0);
	g_assert_nonnull(strstr(query, "indexerIds=5"));
	g_assert_null(strstr(query, "indexerIds=3"));
	/* And nothing of NewsOwl's came through it (the stand-in answers for
	 * every indexer; Prowlarr would not). */
	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (release->source_id == prowlarr)
			g_assert_cmpint(release->indexer_id, !=, 3);
	}
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 2);

	/* Every indexer of Prowlarr is also native: it is not asked at all. */
	{
		g_autofree gchar *only = g_strdup_printf("[{\"id\":3,\"name\":\"NewsOwl\",\"protocol\":\"usenet\","
			"\"enable\":true,\"indexerUrls\":[\"%s/\"]}]", nzb_base);
		g_autoptr(AxisIndexerSearchResult) again = NULL;

		axis_test_prowlarr_set_indexers(fx->prowlarr, only);
		g_assert_true(axis_indexers_test_source(fx->indexers, prowlarr, NULL, &error));
		request.year = 1996;
		again = search_ok(fx, 0, &request);
		g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), ==, 1);
		g_assert_false(part_named(again, "Prowlarr")->asked);
		g_assert_nonnull(strstr(part_named(again, "Prowlarr")->problem, "asked directly"));
	}
}

/* fix-first-contact 3: against the answer of a real Prowlarr 3.0.1 (recorded),
 * which gives no address for a private indexer (no "indexerUrls", no "fields"),
 * NewsOwl and DozyNewt added natively as well are asked once, directly, and
 * Prowlarr is asked without them */
static void
test_recorded_prowlarr_does_not_ask_a_native_indexer_twice(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *prowlarr_base = NULL;
	g_autofree gchar *query = NULL;
	g_auto(GStrv) pieces = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 prowlarr;
	gint64 other;
	gboolean has_27 = FALSE;
	gboolean has_29 = FALSE;
	gboolean has_32 = FALSE;
	guint i;

	fx->prowlarr = axis_test_prowlarr_new_recorded(PROWLARR_KEY, "");
	fx->other = axis_test_newznab_new(KEY, FALSE, "");
	prowlarr_base = axis_test_prowlarr_base(fx->prowlarr);
	prowlarr = add_native(fx, AXIS_INDEXER_PROWLARR_NAME, "Prowlarr", prowlarr_base, "", PROWLARR_KEY, 0);
	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 10);
	other = add_nzb(fx, fx->other, "DozyNewt", 20);
	g_assert_true(axis_indexers_test_source(fx->indexers, prowlarr, NULL, &error));
	/* What the real answer carries: no address for either. */
	assert_q(fx, "27||true|29||true", "SELECT string_agg(indexer_id || '|' || coalesce(host, '') || '|' || enabled, '|' ORDER BY "
		"indexer_id) FROM indexer_source_indexer WHERE source_id = (SELECT id FROM indexer_source WHERE kind = "
		"'prowlarr') AND indexer_id IN (27, 29)");

	film_request(&request);
	result = search_ok(fx, 0, &request);
	/* Each native one was asked. */
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), >=, 1);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->other), >=, 1);
	/* Prowlarr was asked, and for the others only. */
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->prowlarr), >=, 1);
	query = axis_test_prowlarr_search_query(fx->prowlarr, 0);
	pieces = g_strsplit(query, "&", -1);
	for (i = 0; pieces[i] != NULL; i++) {
		has_27 = has_27 || strcmp(pieces[i], "indexerIds=27") == 0;
		has_29 = has_29 || strcmp(pieces[i], "indexerIds=29") == 0;
		has_32 = has_32 || strcmp(pieces[i], "indexerIds=32") == 0;
	}
	g_assert_false(has_27);
	g_assert_false(has_29);
	g_assert_true(has_32);
	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (release->source_id == prowlarr)
			g_assert_true(release->indexer_id != 27 && release->indexer_id != 29);
	}
	/* And the result says so. */
	g_assert_nonnull(result->asked_directly);
	g_assert_cmpuint(result->asked_directly->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(result->asked_directly, 0), ==, "DozyNewt is asked directly, not through Prowlarr");
	g_assert_cmpstr(g_ptr_array_index(result->asked_directly, 1), ==, "NewsOwl is asked directly, not through Prowlarr");
	g_assert_true(part_named(result, "Prowlarr")->asked);
	g_assert_cmpint(other, !=, 0);
}

/* The same, by the site's name: a native indexer called anything else, at an
 * address the answer does not give, is not told to be Prowlarr's */
static void
test_a_native_indexer_of_another_name_is_not_taken_for_prowlarrs(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *prowlarr_base = NULL;
	g_autofree gchar *query = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 prowlarr;

	fx->prowlarr = axis_test_prowlarr_new_recorded(PROWLARR_KEY, "");
	prowlarr_base = axis_test_prowlarr_base(fx->prowlarr);
	prowlarr = add_native(fx, AXIS_INDEXER_PROWLARR_NAME, "Prowlarr", prowlarr_base, "", PROWLARR_KEY, 0);
	fx->nzb_id = add_nzb(fx, fx->nzb, "My Usenet", 10);
	g_assert_true(axis_indexers_test_source(fx->indexers, prowlarr, NULL, &error));
	film_request(&request);
	result = search_ok(fx, 0, &request);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), >=, 1);
	/* Nothing to tell it by: Prowlarr is asked for all, as before. */
	query = axis_test_prowlarr_search_query(fx->prowlarr, 0);
	g_assert_null(strstr(query, "indexerIds=27"));
	g_assert_null(result->asked_directly);
}

/* Task 4.3: the key and every fetch address appear in no log, page (the web
 * test's), job row or message */
static void
test_the_key_is_nowhere(Fixture *fx, gconstpointer data)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	static const gchar *const needles[] = {
		"*" KEY "*", "*apikey*", "*t=get*", "*getnzb*", "*jackett_apikey*", "*" FEED_KEY "*", "*Hearth*",
		"*tt0600001*", "*0600001*", NULL
	};
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *none = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	guint i;
	guint j;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	film_request(&request);
	result = search_ok(fx, 0, &request);
	g_assert_true(axis_indexers_fetch(fx->indexers, release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP")->id, NULL,
		&fetched, &error));
	axis_indexer_fetched_clear(&fetched);
	/* Failures of each kind. */
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_BROKEN);
	request.year = 1996;
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &none, &error));
	assert_clean(error->message);
	g_clear_error(&error);
	g_assert_false(axis_indexers_fetch(fx->indexers, release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP")->id, NULL,
		&fetched, &error));
	assert_clean(error->message);
	g_clear_error(&error);
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_NORMAL);
	axis_test_newznab_set_key(fx->nzb, OTHER_KEY);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->nzb_id, NULL, &error));
	axis_test_newznab_set_mode(fx->nzb, AXIS_TEST_NEWZNAB_LIMIT);
	axis_test_newznab_set_key(fx->nzb, KEY);
	g_assert_true(axis_indexers_refresh_caps(fx->indexers, NULL, NULL, &error));

	/* This layer makes no job row; whatever the queue holds holds no key. */
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE payload::text LIKE '%" KEY "%' OR payload::text LIKE '%apikey%'"
		" OR coalesce(last_error, '') LIKE '%" KEY "%' OR coalesce(progress_message, '') LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE coalesce(state_message, '') LIKE '%" KEY "%'"
		" OR coalesce(hold_message, '') LIKE '%" KEY "%' OR coalesce(caps_problem, '') LIKE '%" KEY "%'"
		" OR coalesce(state_message, '') LIKE '%apikey%'");
	for (i = 0; needles[i] != NULL; i++) {
		for (j = 0; j < G_N_ELEMENTS(levels); j++) {
			if (axis_test_log_count(levels[j], needles[i]) > 0) {
				g_autofree gchar *line = axis_test_log_find(levels[j], needles[i]);

				g_error("a log line at level %d matches %s: %s", (gint)levels[j], needles[i], line);
			}
		}
	}
}

/* A native indexer's caps job reads only those that are old. */
static void
test_the_caps_job(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint n = 0;

	fx->nzb_id = add_nzb(fx, fx->nzb, "NewsOwl", 0);
	g_assert_true(axis_indexers_refresh_caps(fx->indexers, NULL, &n, &error));
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_caps(fx->nzb), ==, 1);
	g_assert_true(axis_indexers_refresh_caps(fx->indexers, NULL, &n, &error));
	g_assert_cmpuint(n, ==, 0);
	g_assert_cmpuint(axis_test_newznab_n_caps(fx->nzb), ==, 1);
	axis_test_db_exec(fx->db, "UPDATE indexer_source SET caps_at = now() - interval '2 days'");
	g_assert_true(axis_indexers_refresh_caps(fx->indexers, NULL, &n, &error));
	g_assert_cmpuint(n, ==, 1);
	g_assert_cmpuint(axis_test_newznab_n_caps(fx->nzb), ==, 2);
	g_assert_cmpuint(axis_test_newznab_n_searches(fx->nzb), ==, 0);
}

/* Saving: the native fields, and their checks. */
static void
test_saving_an_indexer(Fixture *fx, gconstpointer data)
{
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_newznab_base(fx->nzb);
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = "NewsOwl";
	spec.kind = AXIS_INDEXER_NEWZNAB_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.secret = KEY;
	spec.movie_categories = "2040, 2045 2040";
	spec.tv_categories = "";
	spec.priority = 7;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	assert_q(fx, "{2040,2045}|{5000}|7|usenet|false", "SELECT movie_categories::text || '|' || tv_categories::text || '|'"
		" || priority || '|' || protocol || '|' || follow_redirects FROM indexer_source");
	spec.movie_categories = "movies";
	g_assert_false(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "categories"));
	g_clear_error(&error);
	spec.movie_categories = NULL;
	spec.priority = 51;
	g_assert_false(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_nonnull(strstr(error->message, "priority"));
	g_clear_error(&error);
	/* A Torznab feed is torrents. */
	spec.priority = 0;
	spec.name = "Feed";
	spec.kind = AXIS_INDEXER_TORZNAB_NAME;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	assert_q(fx, "torrent|25", "SELECT protocol || '|' || priority FROM indexer_source WHERE name = 'Feed'");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/newznab/xml/reads", test_xml_reads);
	g_test_add_func("/newznab/xml/refuses", test_xml_refuses);
	g_test_add_func("/newznab/xml/nothing-of-a-refused-answer", test_xml_nothing_of_a_refused_answer);
	g_test_add_func("/newznab/parse/caps", test_parse_caps);
	g_test_add_func("/newznab/parse/search", test_parse_search);
	g_test_add_func("/newznab/parse/torznab", test_parse_torznab);
	g_test_add_func("/newznab/parse/dates", test_parse_dates);
	g_test_add_func("/newznab/paths", test_search_paths);
	g_test_add_func("/newznab/keys", test_group_and_host_keys);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/newznab/" path, Fixture, NULL, func)
	ADD("test/capabilities", test_a_test_reads_the_capabilities);
	ADD("test/no-capabilities", test_without_capabilities_words_only);
	ADD("test/wrong-key", test_a_wrong_key_is_refused_without_naming_it);
	ADD("search/identifier-or-words", test_searches_by_identifier_or_words);
	ADD("fetch/from-the-indexer", test_fetch_from_the_indexer);
	ADD("fetch/files-elsewhere", test_files_elsewhere_only_once_accepted);
	ADD("torznab", test_a_torznab_feed);
	ADD("paging/total", test_paging_follows_the_total);
	ADD("paging/short-page", test_paging_stops_at_a_short_page);
	ADD("paging/cap-of-pages", test_paging_stops_at_the_cap_of_pages);
	ADD("paging/later-page-fails", test_a_later_page_that_fails_keeps_what_was_read);
	ADD("paging/nothing-new", test_paging_stops_when_a_page_is_nothing_new);
	ADD("paging/torznab-other-path-hash", test_a_torznab_feed_at_another_path_pages_by_hash);
	ADD("limits/limit-reached", test_a_limit_reached_holds_the_indexer);
	ADD("limits/allowances", test_allowances_are_kept_to);
	ADD("all/one-down", test_one_down_does_not_hold_the_others);
	ADD("all/at-once", test_they_are_asked_at_once);
	ADD("all/limits-per-indexer", test_limits_hold_per_indexer);
	ADD("all/alternatives", test_the_same_release_at_two_indexers);
	ADD("all/same-host", test_the_same_indexer_natively_and_through_prowlarr);
	ADD("all/same-site-recorded", test_recorded_prowlarr_does_not_ask_a_native_indexer_twice);
	ADD("all/other-name", test_a_native_indexer_of_another_name_is_not_taken_for_prowlarrs);
	ADD("secrets", test_the_key_is_nowhere);
	ADD("caps-job", test_the_caps_job);
	ADD("save", test_saving_an_indexer);
#undef ADD
	return g_test_run();
}
