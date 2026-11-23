/*
 * axis-indexer-def-check.c - axisd --check-definition
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-def-check.h"

#include <string.h>

#include "core/axis-http-origin.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-kind.h"
#include "indexer/axis-indexer-shipped.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"
#include "indexer/def/axis-def-trace.h"

/* The largest answer read, as the kind reads one. */
#define ANSWER_BYTES (4 * 1024 * 1024)

/* The least time between two requests to the site. */
#define LEAST_DELAY_S 2.0

/* The definition's text: a file, else a built-in one by its id. */
static gchar *
definition_text(
	const gchar  *what,
	gsize        *length,
	GString      *out
){
	const AxisIndexerShipped *shipped;
	g_autoptr(GError) error = NULL;
	gchar *data = NULL;

	if (g_file_test(what, G_FILE_TEST_IS_REGULAR)) {
		data = axis_indexer_read_file_at_most(what, AXIS_INDEXER_DEFINITION_MAX_BYTES, length, &error);
		if (data == NULL) {
			g_string_append_printf(out, "The definition cannot be read: %s\n", error->message);
			return NULL;
		}
		g_string_append_printf(out, "File: %s\n", what);
		return data;
	}
	shipped = axis_indexer_shipped_find(what);
	if (shipped == NULL) {
		g_string_append_printf(out, "There is no file %s, and no built-in definition of that id.\n", what);
		return NULL;
	}
	g_string_append_printf(out, "Built in: %s, version %d\n", shipped->def_id, shipped->version);
	*length = shipped->length;
	return g_strdup(shipped->body);
}

/* @address (or the definition's first link) as an origin and a base path. */
static gboolean
site_address(
	const AxisDef   *def,
	const gchar     *address,
	AxisHttpOrigin  *origin,
	gchar          **base_path,
	GString         *out
){
	const gchar *link = address != NULL ? address : g_ptr_array_index(def->links, 0);
	g_autoptr(GUri) uri = g_uri_parse(link, G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *text = NULL;
	g_autofree gchar *problem = NULL;

	if (uri == NULL || g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL ||
	    g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL) {
		g_string_append(out, "The address is not an http or https address of a host.\n");
		return FALSE;
	}
	text = g_uri_get_port(uri) > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri),
		g_uri_get_port(uri)) : g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	if (!axis_http_origin_parse(text, origin, &problem)) {
		g_string_append_printf(out, "The address %s.\n", problem != NULL ? problem : "cannot be used");
		return FALSE;
	}
	*base_path = g_strdup(g_uri_get_path(uri));
	while ((*base_path)[0] != '\0' && (*base_path)[strlen(*base_path) - 1] == '/')
		(*base_path)[strlen(*base_path) - 1] = '\0';
	g_string_append_printf(out, "Address: %s%s\n", origin->base, *base_path);
	return TRUE;
}

/* Writes answer @index to the folder to save to. */
static void
save_answer(
	const gchar  *dir,
	guint         index,
	const gchar  *kind,
	GBytes       *body,
	GString      *out
){
	g_autofree gchar *name = g_strdup_printf("answer-%u.%s", index + 1, g_ascii_strcasecmp(kind, "JSON") == 0 ?
		"json" : g_ascii_strcasecmp(kind, "XML") == 0 ? "xml" : "html");
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	g_autoptr(GError) error = NULL;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(body, &length);

	if (g_mkdir_with_parents(dir, 0700) != 0 || !g_file_set_contents(path, data, (gssize)length, &error))
		g_string_append_printf(out, "Not saved: %s\n", error != NULL ? error->message : "the folder cannot be made");
	else
		g_string_append_printf(out, "Saved: %s (scrub it before it is a fixture: tests/tools/scrub-fixture.py)\n",
			path);
}

/* One live request: its answer in @body, or why not in the trace. */
static gboolean
ask_live(
	const AxisHttpOrigin  *origin,
	const AxisDefRequest  *request,
	AxisDefTrace          *trace,
	GBytes               **body,
	guint                 *status
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *refusal = NULL;
	const gchar *accept = request->response == AXIS_DEF_JSON ? "application/json" : request->response == AXIS_DEF_XML ?
		"application/rss+xml, application/xml, text/xml" : "text/html, application/xhtml+xml";
	gboolean challenged = FALSE;
	gboolean ok;

	ok = axis_indexer_conn_get_once(origin, request->path, (const gchar *const *)request->headers, accept, ANSWER_BYTES,
		body, status, &refusal, &challenged, &error);
	if (!ok) {
		g_autofree gchar *why = g_strdup_printf("no answer to read: %s%s", error != NULL ? error->message : "it failed",
			challenged || (*status == 403 && axis_def_is_challenge(refusal)) ? " (a browser challenge, which Axis "
			"cannot pass)" : "");

		axis_def_trace_add_failure(trace, request->path, *status, why);
	}
	return ok;
}

gint
axis_indexer_def_check_run(
	const AxisDefCheckOptions *options,
	GString                   *out
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *base_path = NULL;
	g_autofree gchar *account = NULL;
	g_autoptr(AxisDef) def = NULL;
	g_autoptr(AxisDefSearch) search = NULL;
	g_autoptr(AxisDefTrace) trace = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	AxisIndexerRequest request;
	AxisIndexerOutcome outcome;
	AxisHttpOrigin origin;
	AxisDefSite site;
	gsize length = 0;
	gint status = 0;
	guint i;

	text = definition_text(options->definition, &length, out);
	if (text == NULL)
		return 2;
	def = axis_def_load(text, length, &problem);
	if (def == NULL) {
		g_string_append_printf(out, "Not a definition: %s\n", problem);
		return 1;
	}
	if (!axis_def_usable(def)) {
		g_autofree gchar *needs = axis_def_problem(def);

		g_string_append_printf(out, "Not usable by this version of Axis: %s\n", needs);
		return 1;
	}
	memset(&origin, 0, sizeof(origin));
	if (!site_address(def, options->address, &origin, &base_path, out))
		return 2;
	memset(&site, 0, sizeof(site));
	site.def = def;
	site.origin = &origin;
	site.base_path = base_path;
	site.now_us = g_get_real_time();

	/* The download from a saved release page, read as at grab time. */
	if (options->release_page != NULL) {
		g_autofree gchar *page = NULL;
		g_autofree gchar *magnet = NULL;
		g_autofree gchar *file_path = NULL;
		g_autoptr(GError) error = NULL;
		gsize page_length = 0;

		page = axis_indexer_read_file_at_most(options->release_page, ANSWER_BYTES, &page_length, &error);
		if (page == NULL) {
			g_string_append_printf(out, "The release page cannot be read: %s\n", error->message);
			axis_http_origin_clear(&origin);
			return 2;
		}
		g_clear_pointer(&problem, g_free);
		if (axis_def_read_download(&site, "/", page, page_length, &magnet, &file_path, &problem)) {
			g_autofree gchar *hash = NULL;

			if (magnet != NULL && axis_download_magnet_parse(magnet, &hash, NULL))
				g_string_append_printf(out, "Download: a magnet link of the info-hash %s\n", hash);
			else
				g_string_append_printf(out, "Download: the file at %s on the site\n", file_path);
		} else {
			g_string_append_printf(out, "Download: none: %s\n", problem);
			status = 1;
		}
		if (options->page == NULL && options->words == NULL) {
			axis_http_origin_clear(&origin);
			return status;
		}
	}

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = options->words;
	g_clear_pointer(&problem, g_free);
	search = axis_def_search_new(&site, &request, &problem);
	if (search == NULL) {
		g_string_append_printf(out, "No search can be made: %s\n", problem);
		axis_http_origin_clear(&origin);
		return 1;
	}
	trace = axis_def_trace_new(options->excerpts);
	axis_def_search_set_trace(search, trace);
	axis_indexer_outcome_init(&outcome);
	for (i = 0; i < axis_def_search_n_requests(search); i++) {
		const AxisDefRequest *each = axis_def_search_request(search, i);
		g_autoptr(GBytes) body = NULL;
		g_autofree gchar *why = NULL;
		AxisDefAnswerInfo info;
		const gchar *data;
		guint answer_status = 0;
		gsize body_length = 0;

		if (options->page != NULL) {
			g_autoptr(GError) error = NULL;
			gchar *saved = NULL;

			/* A saved page is the first request's answer, and only that. */
			if (i > 0)
				break;
			saved = axis_indexer_read_file_at_most(options->page, ANSWER_BYTES, &body_length, &error);
			if (saved == NULL) {
				g_string_append_printf(out, "The page cannot be read: %s\n", error->message);
				status = 2;
				break;
			}
			body = g_bytes_new_take(saved, body_length);
		} else {
			if (i > 0)
				g_usleep((gulong)(MAX(def->request_delay, LEAST_DELAY_S) * G_USEC_PER_SEC));
			if (!ask_live(&origin, each, trace, &body, &answer_status)) {
				status = 1;
				break;
			}
			if (options->save_dir != NULL)
				save_answer(options->save_dir, i, each->response == AXIS_DEF_JSON ? "JSON" :
					each->response == AXIS_DEF_XML ? "XML" : "HTML", body, out);
		}
		data = g_bytes_get_data(body, &body_length);
		if (!axis_def_search_read_full(search, i, data, body_length, seen, &outcome, &info, &why))
			status = 1;
		if (info.answer != NULL)
			info.answer->status = answer_status;
		if (status != 0)
			break;
	}
	if (status == 0) {
		g_autofree gchar *found = g_strdup_printf("%u release%s found", outcome.items != NULL ? outcome.items->len : 0,
			outcome.items != NULL && outcome.items->len == 1 ? "" : "s");

		axis_def_trace_set_outcome(trace, found);
	} else if (trace->outcome == NULL) {
		axis_def_trace_set_outcome(trace, "the search did not read its answers: see the verdict above");
	}
	account = axis_def_trace_to_text(trace);
	g_string_append(out, account);
	axis_indexer_outcome_clear(&outcome);
	axis_http_origin_clear(&origin);
	return status;
}
