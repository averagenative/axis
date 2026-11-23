/*
 * fuzz-def-trace.c - libFuzzer target for what add-builtin-torrent-indexers
 * added to the engine of indexer definitions: the verdict on an answer
 * (drift told apart from no results), results that are not video left out,
 * the trace of a search (indexer/def/axis-def-trace.h) and the page reader's
 * markup written back for excerpts (axis_html_markup())
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a mode byte, a definition, a NUL byte and an answer.  The mode
 * byte's low two bits choose the search (a movie, an episode, words), its
 * next bit whether excerpts are recorded.  The seeds are the definitions Axis
 * ships (data/definitions) with their made-up fixture pages, so most inputs
 * run a real definition against a page shaped like its site's.  Whatever
 * they are, the code returns without crashing or allocating without bound,
 * and:
 *
 *  - reading the answer returns TRUE exactly when the verdict is "read"; a
 *    refusal adds nothing and says why, drift's reason begins with
 *    AXIS_DEF_DRIFT_PROBLEM and a challenge's is AXIS_DEF_CHALLENGE_PROBLEM;
 *  - the figures agree: what was kept is what was added, kept and lost are
 *    no more than the rows matched, at most AXIS_INDEXER_MAX_RESULTS kept;
 *  - every release has a clean name, the torrent protocol, a magnet link the
 *    downloads layer accepts or a fetch path on the site, and is not of a
 *    category that is not video;
 *  - the trace holds one answer for the one read, at most AXIS_DEF_TRACE_ROWS
 *    rows, every text in it valid UTF-8 and within its bounds, excerpts only
 *    when asked for, and an account that is valid UTF-8;
 *  - the page written back is valid UTF-8 within the bytes asked for.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/axis-http-origin.h"
#include "download/axis-download-model.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-sites-parse.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"
#include "indexer/def/axis-def-html.h"
#include "indexer/def/axis-def-trace.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define NOW_US (G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC)

static void
fail(int line)
{
	fprintf(stderr, "fuzz-def-trace: the check at line %d failed\n", line);
	abort();
}

/* Valid UTF-8 of at most @max_chars characters (and the "..." of a cut). */
static void
check_text(
	const gchar *text,
	guint        max_chars,
	int          line
){
	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > (glong)max_chars + 3)
		fail(line);
}

static void
check_trace(
	const AxisDefTrace *trace,
	gboolean            excerpts
){
	g_autofree gchar *account = NULL;
	guint i;
	guint k;
	guint f;

	if (trace->answers->len != 1)
		fail(__LINE__);
	for (i = 0; i < trace->answers->len; i++) {
		const AxisDefTraceAnswer *answer = g_ptr_array_index(trace->answers, i);

		check_text(answer->path, 600, __LINE__);
		check_text(answer->verdict, 600, __LINE__);
		check_text(answer->expect, 200, __LINE__);
		check_text(answer->rows_selector, 200, __LINE__);
		check_text(answer->excerpt, AXIS_DEF_TRACE_EXCERPT_BYTES, __LINE__);
		if (answer->excerpt != NULL && !excerpts)
			fail(__LINE__);
		if (answer->rows->len > AXIS_DEF_TRACE_ROWS || answer->kept + answer->lost > answer->rows_matched ||
		    answer->not_video > answer->left_out)
			fail(__LINE__);
		for (k = 0; k < answer->rows->len; k++) {
			const AxisDefTraceRow *row = g_ptr_array_index(answer->rows, k);
			const AxisDefTraceRow *before = k > 0 ? g_ptr_array_index(answer->rows, k - 1) : NULL;

			/* In the answer's order (an item that is no row is counted, not
			 * traced). */
			if (row->number < k + 1 || (before != NULL && row->number <= before->number) ||
			    (row->excerpt != NULL && !excerpts))
				fail(__LINE__);
			check_text(row->fate, 600, __LINE__);
			check_text(row->excerpt, AXIS_DEF_TRACE_EXCERPT_BYTES, __LINE__);
			for (f = 0; f < row->fields->len; f++) {
				const AxisDefTraceField *field = g_ptr_array_index(row->fields, f);

				check_text(field->name, 64, __LINE__);
				check_text(field->how, AXIS_DEF_TRACE_VALUE_CHARS, __LINE__);
				check_text(field->read, AXIS_DEF_TRACE_VALUE_CHARS, __LINE__);
				check_text(field->value, AXIS_DEF_TRACE_VALUE_CHARS, __LINE__);
				check_text(field->note, AXIS_DEF_TRACE_VALUE_CHARS, __LINE__);
			}
		}
	}
	account = axis_def_trace_to_text(trace);
	if (account == NULL || !g_utf8_validate(account, -1, NULL))
		fail(__LINE__);
}

static void
check_release(const AxisIndexerFound *found)
{
	const AxisIndexerRelease *r = found->release;
	guint i;
	gboolean video = r->n_categories == 0;

	if (r == NULL || r->name == NULL || r->name[0] == '\0' || !g_utf8_validate(r->name, -1, NULL) ||
	    g_utf8_strlen(r->name, -1) > AXIS_INDEXER_NAME_MAX_CHARS)
		fail(__LINE__);
	if (r->protocol != AXIS_DOWNLOAD_TORRENT || (found->magnet == NULL && found->fetch_path == NULL))
		fail(__LINE__);
	if (found->magnet != NULL && !axis_download_magnet_parse(found->magnet, NULL, NULL))
		fail(__LINE__);
	if (found->fetch_path != NULL && !axis_indexer_path_ok(found->fetch_path, ""))
		fail(__LINE__);
	if (r->info_hash != NULL && !axis_download_hash_valid(r->info_hash))
		fail(__LINE__);
	for (i = 0; i < r->n_categories; i++)
		video = video || !axis_sites_category_not_video(r->categories[i]);
	if (!video || r->seeders < -1 || r->leechers < -1 || r->size_bytes < -1)
		fail(__LINE__);
}

/* The first link of @def as an origin and a base path. */
static gboolean
site_of(
	const AxisDef  *def,
	AxisHttpOrigin *origin,
	gchar         **base_path
){
	g_autoptr(GUri) uri = g_uri_parse(g_ptr_array_index(def->links, 0), G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *text = NULL;

	if (uri == NULL || g_uri_get_host(uri) == NULL)
		return FALSE;
	text = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri),
		g_uri_get_port(uri)) : g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	if (!axis_http_origin_parse(text, origin, NULL))
		return FALSE;
	*base_path = g_strdup(g_uri_get_path(uri));
	while ((*base_path)[0] != '\0' && (*base_path)[strlen(*base_path) - 1] == '/')
		(*base_path)[strlen(*base_path) - 1] = '\0';
	return TRUE;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text = (const gchar *)data;
	const gchar *nul;
	AxisDef *def;
	AxisHttpOrigin origin;
	gchar *base_path = NULL;
	AxisDefSite site;
	AxisIndexerRequest request;
	AxisDefSearch *search;
	AxisDefTrace *trace;
	AxisIndexerOutcome outcome;
	AxisDefAnswerInfo info;
	gchar *problem = NULL;
	const gchar *page;
	gsize page_length;
	gboolean excerpts;
	gboolean read;
	guint mode;
	guint i;

	if (size < 2 || size > 1 << 20)
		return 0;
	mode = data[0];
	nul = memchr(text + 1, '\0', size - 1);
	if (nul == NULL)
		return 0;
	page = nul + 1;
	page_length = size - (gsize)(page - text);
	excerpts = (mode & 4) != 0;

	def = axis_def_load(text + 1, (gsize)(nul - text - 1), NULL);
	if (def == NULL)
		return 0;
	if (!axis_def_usable(def) || !site_of(def, &origin, &base_path)) {
		axis_def_free(def);
		return 0;
	}
	memset(&site, 0, sizeof(site));
	site.def = def;
	site.origin = &origin;
	site.base_path = base_path;
	site.now_us = NOW_US;
	switch (mode & 3) {
	case 0:
		axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
		request.title = "Made Up Film";
		request.year = 2001;
		request.imdb_id = "tt0000123";
		break;
	case 1:
		axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
		request.title = "Made Up Show";
		request.season = 1;
		request.episode = 2;
		break;
	default:
		axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
		request.title = "made up words";
		break;
	}
	search = axis_def_search_new(&site, &request, NULL);
	if (search == NULL) {
		axis_http_origin_clear(&origin);
		g_free(base_path);
		axis_def_free(def);
		return 0;
	}
	trace = axis_def_trace_new(excerpts);
	axis_def_search_set_trace(search, trace);
	if (trace->requests->len != axis_def_search_n_requests(search) || trace->definition == NULL)
		fail(__LINE__);
	axis_indexer_outcome_init(&outcome);
	read = axis_def_search_read_full(search, 0, page, page_length, NULL, &outcome, &info, &problem);
	if (read != (info.verdict == AXIS_DEF_ANSWER_READ))
		fail(__LINE__);
	if (read) {
		if (problem != NULL || outcome.items == NULL || outcome.items->len != info.kept ||
		    outcome.items->len > AXIS_INDEXER_MAX_RESULTS || info.kept + info.lost > info.rows_matched)
			fail(__LINE__);
		for (i = 0; i < outcome.items->len; i++)
			check_release(g_ptr_array_index(outcome.items, i));
	} else {
		if (problem == NULL || outcome.items != NULL)
			fail(__LINE__);
		if (info.verdict == AXIS_DEF_ANSWER_DRIFT && !g_str_has_prefix(problem, AXIS_DEF_DRIFT_PROBLEM))
			fail(__LINE__);
		if (info.verdict == AXIS_DEF_ANSWER_CHALLENGE && strcmp(problem, AXIS_DEF_CHALLENGE_PROBLEM) != 0)
			fail(__LINE__);
	}
	check_trace(trace, excerpts);
	/* The page written back, as an excerpt is. */
	if (page_length <= AXIS_HTML_MAX_BYTES) {
		AxisHtmlDoc *doc = axis_html_parse(page, page_length, (mode & 8) != 0, NULL);

		if (doc != NULL) {
			gsize max = 1 + (mode >> 4) * 97;
			gchar *markup = axis_html_markup(axis_html_doc_root(doc), max);

			if (!g_utf8_validate(markup, -1, NULL) || strlen(markup) > max)
				fail(__LINE__);
			g_free(markup);
			axis_html_doc_free(doc);
		}
	}
	g_free(problem);
	axis_indexer_outcome_clear(&outcome);
	axis_def_search_free(search);
	axis_def_trace_free(trace);
	axis_http_origin_clear(&origin);
	g_free(base_path);
	axis_def_free(def);
	return 0;
}
