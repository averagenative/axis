/*
 * axis-web-indexers-check.c - "Check this indexer": one search of an
 * indexer that runs a site definition, shown step by step
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /indexers/:id/check   the form (admin)
 *   POST /indexers/:id/check   one search by the words typed, and its account
 *
 * For an administrator repairing a definition (or deciding whom to tell).
 * The search is the indexer's own (its address, limits and back-off;
 * axis_indexers_check_source()), made only on a POST, which carries the CSRF
 * token.  The account (indexer/def/axis-def-trace.h) is shown as text: every
 * value is set as an element's text content, so the page builder escapes
 * it, nothing in it is a link, and nothing is fetched because of it.  The
 * pages' raw markup is not shown unless excerpts are asked for, and then
 * only the bounded excerpts the trace keeps.  What was typed is not logged
 * or kept.
 */

#include "web/axis-web-indexers-private.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

static const AxisWebColumn field_columns[] = {
	{ "Field", AXIS_WEB_COL_NAME },
	{ "Read by", AXIS_WEB_COL_WIDE },
	{ "What it read", AXIS_WEB_COL_WIDE },
	{ "Its value", AXIS_WEB_COL_WIDE },
	{ "Note", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

typedef struct
{
	gint64                 id;
	gchar                 *words;
	gboolean               excerpts;
	gboolean               run;
	AxisIndexerSourceInfo *source;
	AxisDefTrace          *trace;
	GError                *error;
} CheckWork;

static void
check_work_free(gpointer data)
{
	CheckWork *work = data;

	g_free(work->words);
	g_clear_pointer(&work->source, axis_indexer_source_info_free);
	g_clear_pointer(&work->trace, axis_def_trace_free);
	g_clear_error(&work->error);
	g_free(work);
}

static void
check_run(
	AxisWebIndexers *self,
	gpointer         data
){
	CheckWork *work = data;

	if (!axis_indexers_get_source(self->indexers, work->id, &work->source, &work->error))
		return;
	if (work->run && work->source->definition_id != 0)
		axis_indexers_check_source(self->indexers, work->id, work->words, work->excerpts, NULL, &work->trace,
			&work->error);
}

static HtmxNode *
text_block(
	const gchar *css,
	const gchar *text
){
	HtmxPre *block = htmx_pre_new();

	htmx_element_add_class(HTMX_ELEMENT(block), css);
	htmx_node_set_text_content(HTMX_NODE(block), text);
	return HTMX_NODE(block);
}

static HtmxNode *
cell(const gchar *text)
{
	HtmxTd *td = htmx_td_new();

	if (text != NULL)
		ADD(td, axis_web_breakable(text));
	return HTMX_NODE(td);
}

static HtmxNode *
check_form(
	HtmxRequest     *request,
	const CheckWork *work
){
	g_autofree gchar *action = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/check", work->id);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for("check-excerpts");

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_storage_text_field("check-words", "Words to search for", "words", work->words,
		"a title the site has", "What the indexer is asked for, as a search of yours by words would ask it. It is "
		"not kept or written to a log."));
	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), "check-excerpts");
	htmx_input_set_name(box, "excerpts");
	htmx_input_set_value(box, "yes");
	if (work->excerpts)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), "Show an excerpt of each row read, and of a page no row was read "
		"from");
	ADD(field, box);
	ADD(field, label);
	ADD(form, field);
	ADD(form, axis_web_auth_button("Check it", "button-primary"));
	return form;
}

/* One answer of the account. */
static HtmxNode *
answer_card(
	guint                     number,
	const AxisDefTraceAnswer *answer
){
	g_autofree gchar *title = g_strdup_printf("Answer %u", number);
	g_autofree gchar *size = g_strdup_printf("%" G_GSIZE_FORMAT " bytes%s%s", answer->length,
		answer->kind[0] != '\0' ? ", read as " : "", answer->kind);
	g_autofree gchar *status = answer->status != 0 ? g_strdup_printf("%u", answer->status) : g_strdup("none");
	HtmxNode *card = axis_web_storage_card(title);
	HtmxNode *facts = axis_web_browser_facts_new();
	guint i;
	guint k;

	axis_web_browser_fact(facts, "Asked", answer->path);
	axis_web_browser_fact(facts, "Status", status);
	axis_web_browser_fact(facts, "Size", size);
	if (answer->challenge)
		axis_web_browser_fact(facts, "Browser challenge", "the answer is a challenge's page, not the site's");
	if (answer->expect != NULL) {
		g_autofree gchar *expect = g_strdup_printf("%s: %s", answer->expect, answer->expect_found ? "found" :
			"NOT FOUND");

		axis_web_browser_fact(facts, "What every answer has", expect);
	}
	if (answer->rows_selector != NULL) {
		g_autofree gchar *rows = g_strdup_printf("%s: matched %u", answer->rows_selector, answer->rows_matched);
		g_autofree gchar *counts = g_strdup_printf("%u kept, %u lost for a field every result must have, %u left out "
			"on purpose (%u of them not video)", answer->kept, answer->lost, answer->left_out, answer->not_video);

		axis_web_browser_fact(facts, "Rows", rows);
		axis_web_browser_fact(facts, "Of them", counts);
	}
	axis_web_browser_fact(facts, "Verdict", answer->verdict != NULL ? answer->verdict : "");
	ADD(card, facts);
	if (answer->excerpt != NULL) {
		ADD(card, axis_web_storage_text("help", "An excerpt of the page (no row was read from it), as text:"));
		ADD(card, text_block("check-excerpt", answer->excerpt));
	}
	for (i = 0; i < answer->rows->len; i++) {
		const AxisDefTraceRow *row = g_ptr_array_index(answer->rows, i);
		g_autofree gchar *head = g_strdup_printf("Row %u: %s", row->number, row->fate != NULL ? row->fate : "");
		HtmxNode *table = axis_web_table_new("check-fields", field_columns);
		HtmxTbody *body = htmx_tbody_new();
		HtmxP *line = htmx_p_new();
		HtmxStrong *strong = htmx_strong_new();

		htmx_node_set_text_content(HTMX_NODE(strong), head);
		ADD(line, strong);
		ADD(card, line);
		for (k = 0; k < row->fields->len; k++) {
			const AxisDefTraceField *field = g_ptr_array_index(row->fields, k);
			HtmxTr *tr = htmx_tr_new();

			ADD(tr, cell(field->name));
			ADD(tr, cell(field->how));
			ADD(tr, cell(field->read != NULL ? field->read : ""));
			ADD(tr, cell(field->value != NULL ? field->value : ""));
			ADD(tr, cell(field->note != NULL ? field->note : ""));
			ADD(body, tr);
		}
		ADD(table, body);
		ADD(card, table);
		if (row->excerpt != NULL)
			ADD(card, text_block("check-excerpt", row->excerpt));
	}
	if (answer->rows_matched > answer->rows->len && answer->rows->len > 0) {
		g_autofree gchar *more = g_strdup_printf("The first %u rows are shown, of %u.", answer->rows->len,
			answer->rows_matched);

		ADD(card, axis_web_storage_text("help", more));
	}
	return card;
}

static HtmxResponse *
check_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	CheckWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *back = NULL;
	HtmxHeading *heading;
	HtmxNode *card;
	HtmxA *link;
	guint i;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	if (work->error != NULL && work->error->domain != AXIS_INDEXER_ERROR)
		return axis_web_internal_error(request, work->error);
	if (work->source->definition_id == 0)
		return axis_web_error_respond(request, 404, "Not found", "Only an indexer that runs a site definition can be "
			"checked step by step.");
	content = htmx_div_new();
	title = g_strdup_printf("Check %s", work->source->name);
	heading = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(heading), title);
	ADD(content, heading);
	ADD(content, axis_web_page_intro("One search of this indexer, shown step by step: what was asked, what came back, "
		"what each field of the first rows read, and why a row was left out. Use it when the indexer finds nothing "
		"or fails: it shows whether the site answered, and which part of its definition no longer fits the site's "
		"pages. It searches as the indexer does (its address and its limits) and keeps nothing it finds."));
	if (work->error != NULL)
		ADD(content, axis_web_auth_notice(TRUE, work->error->message));
	card = axis_web_storage_card("Search");
	ADD(card, check_form(request, work));
	ADD(content, card);
	if (work->trace != NULL) {
		HtmxNode *facts = axis_web_browser_facts_new();

		card = axis_web_storage_card("What was asked");
		axis_web_browser_fact(facts, "Definition", work->trace->definition != NULL ? work->trace->definition : "");
		axis_web_browser_fact(facts, "Words", work->trace->words != NULL ? work->trace->words : "");
		for (i = 0; i < work->trace->requests->len; i++)
			axis_web_browser_fact(facts, "Request", g_ptr_array_index(work->trace->requests, i));
		axis_web_browser_fact(facts, "Outcome", work->trace->outcome != NULL ? work->trace->outcome : "");
		ADD(card, facts);
		ADD(content, card);
		for (i = 0; i < work->trace->answers->len; i++)
			ADD(content, answer_card(i + 1, g_ptr_array_index(work->trace->answers, i)));
		card = axis_web_storage_card("Repairing it");
		ADD(card, axis_web_storage_text(NULL, "When the site has changed its pages, its definition needs a repair: a "
			"newer version of it, pasted on the definition's page, overrides a built-in one at once. The steps are in "
			"the contributor guide, \"How to repair an indexer definition\" (docs/CONTRIBUTING-definitions.md), and "
			"axisd --check-definition shows the same account from the command line, against a saved page."));
		ADD(content, card);
	}
	back = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, work->id);
	link = htmx_a_new_with_href(back);
	htmx_node_set_text_content(HTMX_NODE(link), "Back to the indexer");
	ADD(content, link);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
handle_check(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	CheckWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *words;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such indexer.");
	work = g_new0(CheckWork, 1);
	work->id = id;
	work->run = htmx_request_get_method(request) == HTMX_METHOD_POST;
	if (work->run) {
		words = htmx_request_get_form_value(request, "words");
		work->words = words != NULL && strlen(words) <= 1024 && g_utf8_validate(words, -1, NULL) ? g_strdup(words) :
			NULL;
		work->excerpts = axis_web_storage_form_yes(request, "excerpts");
	}
	return axis_web_indexers_dispatch(user_data, request, check_run, check_done, work, check_work_free);
}

void
axis_web_indexers_register_check(
	AxisWebIndexers *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/indexers/:id/check", AXIS_WEB_ROUTE_ADMIN, handle_check, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/:id/check", AXIS_WEB_ROUTE_ADMIN, handle_check, self);
}
