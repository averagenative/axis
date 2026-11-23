/*
 * axis-web-indexers-jackett.c - "Fetch definitions from Jackett": the list
 * of Jackett's definition files as an administrator last asked for it, the
 * fetch of the ones they tick, the check for newer versions and the update
 * of installed ones, putting an earlier version back
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only; every change is a posted form with the CSRF field.
 * Nothing on these pages asks GitHub by being opened: only "See what Jackett
 * has" (and "Check Jackett for newer versions", the same asking) and a fetch
 * of ticked files do, each once for one press (add-definition-fetch).  What
 * a fetched file says of itself is shown as text only, as for a file
 * installed by hand.
 */

#include "web/axis-web-indexers-private.h"

#include <string.h>

#include "indexer/axis-indexer-jackett-parse.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add

#define PAGE_PATH   "/indexers/definitions/jackett"
#define LIST_PATH   PAGE_PATH "/list"
#define FETCH_PATH  PAGE_PATH "/fetch"

/* A ticked file is the field "pick-<name>". */
#define PICK_PREFIX AXIS_WEB_INDEXERS_JACKETT_PICK

#define LICENCE_URL "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html"
#define FOLDER_URL  "https://" AXIS_INDEXER_JACKETT_REPOSITORY "/tree/" AXIS_INDEXER_JACKETT_BRANCH "/" \
	AXIS_INDEXER_JACKETT_FOLDER

static const AxisWebColumn listing_columns[] = {
	{ "Definition file", AXIS_WEB_COL_NAME },
	{ "Size", AXIS_WEB_COL_SIZE },
	{ "On this Axis", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static const AxisWebColumn outcome_columns[] = {
	{ "Definition file", AXIS_WEB_COL_NAME },
	{ "What happened", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* ── Pieces shared with the Indexers page ───────────────────────── */

static HtmxNode *
link_to(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	htmx_element_set_attribute(HTMX_ELEMENT(link), "rel", "noopener noreferrer");
	return HTMX_NODE(link);
}

static HtmxNode *
span_of(const gchar *text)
{
	HtmxSpan *span = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

HtmxNode *
axis_web_indexers_jackett_licence(void)
{
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	ADD(line, span_of("Jackett's definitions are in its repository at "));
	ADD(line, link_to(FOLDER_URL, AXIS_INDEXER_JACKETT_REPOSITORY ", folder " AXIS_INDEXER_JACKETT_FOLDER));
	ADD(line, span_of(". Jackett is published under the "));
	ADD(line, link_to(LICENCE_URL, "GNU General Public License, version 2 (GPL-2.0)"));
	ADD(line, span_of(". Axis comes with none of those files: you fetch the ones you choose to your own server, and "
		"Axis reads them as data."));
	return HTMX_NODE(line);
}

/* The button that asks GitHub for the list; @from_installed comes back to
 * the Indexers page's list of installed definitions. */
static HtmxNode *
ask_form(
	HtmxRequest *request,
	const gchar *label,
	gboolean     from_installed
){
	HtmxNode *form = axis_web_auth_post_form(request, LIST_PATH);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (from_installed)
		ADD(form, axis_web_auth_hidden("from", "installed"));
	ADD(form, axis_web_auth_button(label, from_installed ? "button-small" : "button-primary"));
	return form;
}

static HtmxNode *
check_field(
	const gchar *id,
	const gchar *name,
	const gchar *label_text,
	const gchar *form_id
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (form_id != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "form", form_id);
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	ADD(field, box);
	ADD(field, label);
	return HTMX_NODE(field);
}

HtmxNode *
axis_web_indexers_jackett_section(
	HtmxRequest *request,
	gboolean     any_fetched
){
	HtmxDiv *section = htmx_div_new();
	HtmxHeading *heading = htmx_h3_new();

	htmx_element_set_id(HTMX_ELEMENT(section), "jackett");
	htmx_node_set_text_content(HTMX_NODE(heading), "Fetch definitions from Jackett");
	ADD(section, heading);
	ADD(section, axis_web_storage_text(NULL, "Axis can list the definition files in Jackett's repository and fetch "
		"the ones you tick. It asks only when you press a button here: nothing is fetched at start or on a schedule, "
		"and nothing updates by itself. A fetched definition is checked like a file you install by hand, and "
		"searches nothing until you add an indexer from it."));
	ADD(section, axis_web_indexers_jackett_licence());
	ADD(section, ask_form(request, "See what Jackett has", FALSE));
	ADD(section, link_to(PAGE_PATH, "The list Axis read last"));
	if (any_fetched) {
		HtmxNode *update = axis_web_auth_post_form(request, FETCH_PATH);

		ADD(section, axis_web_storage_text("help", "For the definitions above that came from Jackett: check whether "
			"their files changed there, then update the ones you tick. The version replaced is kept, and its page "
			"can put it back."));
		ADD(section, ask_form(request, "Check Jackett for newer versions", TRUE));
		htmx_element_set_id(HTMX_ELEMENT(update), AXIS_WEB_INDEXERS_JACKETT_UPDATE_FORM);
		htmx_element_add_class(HTMX_ELEMENT(update), "form");
		ADD(update, axis_web_auth_hidden("from", "installed"));
		ADD(update, check_field("jackett-update-replace", "replace",
			"Also replace definitions installed or changed by hand", NULL));
		ADD(update, axis_web_auth_button("Update the ticked definitions", "button-small"));
		ADD(section, update);
	}
	return HTMX_NODE(section);
}

/* ── The page ───────────────────────────────────────────────────── */

typedef struct
{
	gchar                      *words;		/* nullable: the search */
	gchar                      *done;		/* nullable */
	gboolean                    asking;		/* the list is asked for */
	gboolean                    from_installed;
	gchar                     **names;		/* nullable: the files to fetch */
	gboolean                    replace;
	AxisIndexerJackettListing  *listing;
	GPtrArray                  *outcomes;
	guint                       n_files;
	GError                     *refusal;	/* said on the page */
	GError                     *error;
	GPtrArray                  *sources;	/* for the Indexers page, after a refusal there */
	GPtrArray                  *definitions;
} Work;

static void
work_free(gpointer data)
{
	Work *work = data;

	g_free(work->words);
	g_free(work->done);
	g_strfreev(work->names);
	g_clear_pointer(&work->listing, axis_indexer_jackett_listing_free);
	g_clear_pointer(&work->outcomes, g_ptr_array_unref);
	g_clear_error(&work->refusal);
	g_clear_error(&work->error);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_pointer(&work->definitions, g_ptr_array_unref);
	g_free(work);
}

static void
run(
	AxisWebIndexers *self,
	gpointer         data
){
	Work *work = data;

	if (work->asking)
		axis_indexers_jackett_refresh(self->indexers, NULL, &work->n_files, &work->refusal);
	else if (work->names != NULL)
		axis_indexers_jackett_fetch(self->indexers, (const gchar *const *)work->names, work->replace, NULL,
			&work->outcomes, &work->refusal);
	if (work->refusal != NULL && work->refusal->domain != AXIS_INDEXER_ERROR) {
		work->error = g_steal_pointer(&work->refusal);
		return;
	}
	/* Asked from the Indexers page: back there, with what it shows. */
	if (work->asking && work->from_installed) {
		if (work->refusal != NULL && axis_indexers_list_sources(self->indexers, &work->sources, &work->error))
			axis_indexers_list_definitions(self->indexers, &work->definitions, &work->error);
		return;
	}
	if (work->asking && work->refusal == NULL)
		return;
	axis_indexers_jackett_get_listing(self->indexers, work->words, &work->listing, &work->error);
}

static guint
refusal_status(const GError *refusal)
{
	if (g_error_matches(refusal, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID))
		return 400;
	if (g_error_matches(refusal, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY))
		return 503;
	/* GitHub's answer, or none. */
	return 502;
}

static HtmxNode *
text_cell(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static gchar *
size_text(gint64 bytes)
{
	if (bytes < 1024)
		return g_strdup_printf("%" G_GINT64_FORMAT " bytes", bytes);
	return g_strdup_printf("%.1f KiB", (gdouble)bytes / 1024.0);
}

/* "2 indexers use it." */
static gchar *
used_text(guint n_indexers)
{
	return g_strdup_printf("%u indexer%s use%s it.", n_indexers, n_indexers == 1 ? "" : "s", n_indexers == 1 ? "s" : "");
}

/* What became of each file of a fetch. */
static HtmxNode *
outcomes_card(GPtrArray *outcomes)
{
	HtmxNode *card = axis_web_storage_card("What the fetch did");
	HtmxNode *table = axis_web_table_new("jackett-outcomes", outcome_columns);
	HtmxTbody *body = htmx_tbody_new();
	g_autofree gchar *summary = NULL;
	guint n_taken = 0;
	guint n_refused = 0;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "fetched");
	for (i = 0; i < outcomes->len; i++) {
		const AxisIndexerJackettOutcome *outcome = g_ptr_array_index(outcomes, i);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *what = htmx_td_new();
		g_autofree gchar *said = NULL;

		if (outcome->definition_id != 0) {
			g_autofree gchar *href = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT,
				outcome->definition_id);
			HtmxA *link = htmx_a_new_with_href(href);

			ADD(link, axis_web_breakable(outcome->name));
			ADD(name, link);
		} else {
			ADD(name, axis_web_breakable(outcome->name));
		}
		switch (outcome->result) {
		case AXIS_INDEXER_JACKETT_INSTALLED:
			n_taken++;
			said = g_strdup("Installed. Nothing searches it yet: add an indexer from its page.");
			break;
		case AXIS_INDEXER_JACKETT_UPDATED:
			n_taken++;
			said = g_strdup("Updated. The version it replaced is kept: its page can put it back.");
			if (outcome->n_indexers > 0) {
				g_autofree gchar *used = used_text(outcome->n_indexers);
				g_autofree gchar *more = outcome->cannot_run ? g_strdup_printf("%s Axis cannot run this version, so "
					"%s will not search until the earlier version is put back.", used, outcome->n_indexers == 1 ?
					"that indexer" : "those indexers") : g_strdup_printf("%s It runs this version from its next "
					"search.", used);
				gchar *joined = g_strconcat(said, " ", more, NULL);

				g_free(said);
				said = joined;
			}
			break;
		case AXIS_INDEXER_JACKETT_UNCHANGED:
			said = outcome->message != NULL ? g_strdup(outcome->message) :
				g_strdup("Already installed: the same file. Nothing changed.");
			break;
		default:
			n_refused++;
			said = g_strdup_printf("Not installed. %s", outcome->message != NULL ? outcome->message : "");
			break;
		}
		ADD(what, axis_web_breakable(said));
		ADD(row, name);
		ADD(row, what);
		ADD(body, row);
	}
	summary = g_strdup_printf("%u installed or updated, %u not installed, of %u asked for.", n_taken, n_refused,
		outcomes->len);
	ADD(card, axis_web_auth_notice(n_refused > 0, summary));
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const gchar *
entry_state(const AxisIndexerJackettEntry *entry)
{
	if (entry->definition_id == 0)
		return "Not installed";
	if (!entry->differs)
		return "Installed: the same file";
	if (entry->by_hand)
		return "Installed, and differs: yours was installed or changed by hand";
	return "Installed, and differs: Jackett's file has changed since yours was fetched";
}

static HtmxNode *
listing_card(
	HtmxRequest                     *request,
	const AxisIndexerJackettListing *listing,
	const gchar                     *words
){
	HtmxNode *card = axis_web_storage_card("What Jackett has");
	HtmxForm *search;
	HtmxInput *box;
	HtmxLabel *label;
	HtmxNode *form;
	HtmxNode *table;
	HtmxTbody *body;
	g_autofree gchar *when = NULL;
	g_autofree gchar *n_files = NULL;
	g_autofree gchar *read = NULL;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "list");
	if (listing == NULL) {
		ADD(card, axis_web_storage_text("help", "Axis has not asked Jackett's repository yet. Select See what "
			"Jackett has: Axis asks GitHub twice, for the newest commit and for the names of the definition files "
			"in it. No file is fetched by that."));
		ADD(card, ask_form(request, "See what Jackett has", FALSE));
		return card;
	}
	when = axis_web_format_time(listing->listed_us, "never");
	n_files = axis_web_format_count(listing->n_files);
	read = g_strdup_printf("%s definition files, as Jackett's repository had them at commit %.10s, read on %s. "
		"A file you fetch is that commit's file, whatever has changed there since.", n_files, listing->commit, when);
	ADD(card, axis_web_storage_text(NULL, read));
	if (listing->cut)
		ADD(card, axis_web_storage_text("notice notice-warning", "GitHub gives at most 1,000 files of a folder this "
			"way, and this list is that long: some definitions may be missing from it."));
	if (listing->n_left_out > 0) {
		g_autofree gchar *left = g_strdup_printf("%u entries of the folder are not listed: they are not files, or "
			"their names are not lower-case letters, digits and hyphens with \".yml\", the only names Axis takes.",
			listing->n_left_out);

		ADD(card, axis_web_storage_text("help", left));
	}
	ADD(card, ask_form(request, "See what Jackett has now", FALSE));

	search = htmx_form_new();
	htmx_element_set_attribute(HTMX_ELEMENT(search), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(search), "action", PAGE_PATH);
	htmx_element_add_class(HTMX_ELEMENT(search), "form");
	label = htmx_label_new_for("jackett-q");
	htmx_node_set_text_content(HTMX_NODE(label), "Find a definition by name");
	box = htmx_input_new(HTMX_INPUT_SEARCH);
	htmx_element_set_id(HTMX_ELEMENT(box), "jackett-q");
	htmx_input_set_name(box, "q");
	if (words != NULL)
		htmx_input_set_value(box, words);
	ADD(search, label);
	ADD(search, box);
	ADD(search, axis_web_auth_button("Find", "button-small"));
	ADD(card, search);
	if (words != NULL && words[0] != '\0') {
		g_autofree gchar *shown = g_strdup_printf("%u of %s files have that in their name. Ticks are not kept "
			"from one search to the next: fetch what you ticked, then search again.", listing->entries->len, n_files);

		ADD(card, axis_web_storage_text("help", shown));
		ADD(card, link_to(PAGE_PATH, "Show every file"));
	}
	if (listing->entries->len == 0) {
		ADD(card, axis_web_storage_text("help", "No definition file has that in its name."));
		return card;
	}

	form = axis_web_auth_post_form(request, FETCH_PATH);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	table = axis_web_table_new("jackett-listing", listing_columns);
	body = htmx_tbody_new();
	for (i = 0; i < listing->entries->len; i++) {
		const AxisIndexerJackettEntry *entry = g_ptr_array_index(listing->entries, i);
		g_autofree gchar *field = g_strconcat(PICK_PREFIX, entry->name, NULL);
		g_autofree gchar *id = g_strdup_printf("pick-%u", i);
		g_autofree gchar *size = size_text(entry->size_bytes);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *state = htmx_td_new();
		HtmxInput *tick = htmx_input_new(HTMX_INPUT_CHECKBOX);
		HtmxLabel *tick_label = htmx_label_new_for(id);

		htmx_element_set_id(HTMX_ELEMENT(tick), id);
		htmx_input_set_name(tick, field);
		htmx_input_set_value(tick, "yes");
		ADD(tick_label, axis_web_breakable(entry->name));
		ADD(name, tick);
		ADD(name, tick_label);
		ADD(row, name);
		ADD(row, text_cell(size));
		if (entry->definition_id != 0) {
			g_autofree gchar *href = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT, entry->definition_id);
			HtmxA *link = htmx_a_new_with_href(href);

			htmx_node_set_text_content(HTMX_NODE(link), entry->definition_name);
			ADD(state, span_of(entry_state(entry)));
			ADD(state, span_of(" ("));
			ADD(state, link);
			ADD(state, span_of(")"));
			if (entry->n_indexers > 0) {
				g_autofree gchar *used = used_text(entry->n_indexers);
				g_autofree gchar *spaced = g_strconcat(" ", used, NULL);

				ADD(state, span_of(spaced));
			}
		} else {
			ADD(state, span_of(entry_state(entry)));
		}
		ADD(row, state);
		ADD(body, row);
	}
	ADD(table, body);
	ADD(form, table);
	{
		g_autofree gchar *most = g_strdup_printf("At most %d files at once, each at most %d KiB. Each is checked as "
			"a definition before it is installed; one that is not is left out, and the page says why. Installing "
			"switches nothing on: an indexer is added from a definition's own page.",
			AXIS_INDEXER_JACKETT_MAX_PER_FETCH, AXIS_INDEXER_DEFINITION_MAX_BYTES / 1024);

		ADD(form, axis_web_storage_text("help", most));
	}
	ADD(form, check_field("jackett-replace", "replace", "Also replace definitions installed or changed by hand",
		NULL));
	ADD(form, axis_web_auth_button("Fetch the ticked definitions", "button-primary"));
	ADD(card, form);
	return card;
}

static const gchar *
done_message(const gchar *key)
{
	if (g_strcmp0(key, "listed") == 0)
		return "Axis has read the list of Jackett's definition files. No file was fetched.";
	return NULL;
}

static HtmxResponse *
page(
	HtmxRequest *request,
	Work        *work
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxHeading *title = htmx_h1_new();
	const gchar *message = done_message(work->done);
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(title), "Fetch definitions from Jackett");
	ADD(content, title);
	ADD(content, axis_web_page_intro("The site definitions in Jackett's repository, as Axis last read their names: "
		"tick the ones you want and fetch them to this server. Axis asks GitHub only when you press a button here."));
	if (work->refusal != NULL)
		ADD(content, axis_web_auth_notice(TRUE, work->refusal->message));
	else if (message != NULL)
		ADD(content, axis_web_auth_notice(FALSE, message));
	ADD(content, axis_web_indexers_jackett_licence());
	ADD(content, axis_web_storage_text("help", "Install only definitions you have the right to use. Many describe "
		"private sites that need an account: Axis installs those too, and says what they need before they can be "
		"used."));
	if (work->outcomes != NULL)
		ADD(content, outcomes_card(work->outcomes));
	ADD(content, listing_card(request, work->listing, work->words));
	ADD(content, link_to("/indexers#definitions", "Back to Indexers"));
	response = axis_web_page_respond(request, "Fetch definitions from Jackett", HTMX_NODE(content));
	if (work->refusal != NULL)
		htmx_response_set_status(response, refusal_status(work->refusal));
	return response;
}

static HtmxResponse *
done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	Work *work = data;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->asking) {
		if (work->refusal == NULL) {
			axis_web_storage_audit(request, "indexer definitions: the list of Jackett's files read (%u files)",
				work->n_files);
			if (work->from_installed)
				return axis_web_place_redirect(request, "/indexers", "jackett-checked", "definitions");
			return axis_web_auth_redirect(request, PAGE_PATH "?done=listed");
		}
		if (work->from_installed)
			return axis_web_indexers_list_response(self, request, work->sources, work->definitions,
				work->refusal->message, TRUE, refusal_status(work->refusal));
	}
	for (i = 0; work->outcomes != NULL && i < work->outcomes->len; i++) {
		const AxisIndexerJackettOutcome *outcome = g_ptr_array_index(work->outcomes, i);

		if (outcome->result == AXIS_INDEXER_JACKETT_INSTALLED || outcome->result == AXIS_INDEXER_JACKETT_UPDATED)
			axis_web_storage_audit(request, "indexer definition %" G_GINT64_FORMAT " %s from Jackett (%s)",
				outcome->definition_id, outcome->result == AXIS_INDEXER_JACKETT_UPDATED ? "updated" : "fetched",
				outcome->name);
	}
	return page(request, work);
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);
	const gchar *words = htmx_request_get_query_param(request, "q");

	/* A search is a part of a file's name: nothing longer is one. */
	if (words != NULL && strlen(words) <= AXIS_JACKETT_MAX_STEM_BYTES + 4 && g_utf8_validate(words, -1, NULL))
		work->words = g_strdup(words);
	if (done_message(htmx_request_get_query_param(request, "done")) != NULL)
		work->done = g_strdup(htmx_request_get_query_param(request, "done"));
	return axis_web_indexers_dispatch(user_data, request, run, done, work, work_free);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);

	work->asking = TRUE;
	work->from_installed = g_strcmp0(htmx_request_get_form_value(request, "from"), "installed") == 0;
	return axis_web_indexers_dispatch(user_data, request, run, done, work, work_free);
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The files a form names: the one of an "Update" button, else every ticked
 * one.  Names are outside input: each is checked again where it is used;
 * here only their number is held to what one form can honestly send. */
static gchar **
picked(HtmxRequest *request)
{
	GPtrArray *names = g_ptr_array_new();
	const gchar *only = htmx_request_get_form_value(request, "only");
	GHashTable *fields = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (only != NULL && only[0] != '\0') {
		g_ptr_array_add(names, g_strdup(only));
	} else if (fields != NULL) {
		g_hash_table_iter_init(&iter, fields);
		while (g_hash_table_iter_next(&iter, &key, &value) && names->len <= AXIS_INDEXER_JACKETT_MAX_PER_FETCH) {
			if (g_str_has_prefix(key, PICK_PREFIX) && g_strcmp0(value, "yes") == 0)
				g_ptr_array_add(names, g_strdup((const gchar *)key + strlen(PICK_PREFIX)));
		}
	}
	g_ptr_array_sort(names, compare_names);
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

static HtmxResponse *
handle_fetch(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);

	work->names = picked(request);
	work->replace = axis_web_storage_form_yes(request, "replace");
	work->from_installed = g_strcmp0(htmx_request_get_form_value(request, "from"), "installed") == 0;
	return axis_web_indexers_dispatch(user_data, request, run, done, work, work_free);
}

/* ── Putting the earlier version back ───────────────────────────── */

typedef struct
{
	gint64  id;
	GError *error;
} BackWork;

static void
back_free(gpointer data)
{
	BackWork *work = data;

	g_clear_error(&work->error);
	g_free(work);
}

static void
back_run(
	AxisWebIndexers *self,
	gpointer         data
){
	BackWork *work = data;

	axis_indexers_jackett_put_back(self->indexers, work->id, &work->error);
}

static HtmxResponse *
back_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	BackWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID))
		return axis_web_error_respond(request, 400, "Nothing to put back", work->error->message);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_storage_audit(request, "indexer definition %" G_GINT64_FORMAT ": the earlier version put back", work->id);
	location = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "?done=put-back", work->id);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_put_back(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	BackWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "The form to put it back was not complete.");
	work = g_new0(BackWork, 1);
	work->id = id;
	return axis_web_indexers_dispatch(user_data, request, back_run, back_done, work, back_free);
}

void
axis_web_indexers_register_jackett(
	AxisWebIndexers *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, PAGE_PATH, AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, LIST_PATH, AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, FETCH_PATH, AXIS_WEB_ROUTE_ADMIN, handle_fetch, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/definitions/:id/put-back", AXIS_WEB_ROUTE_ADMIN,
		handle_put_back, self);
}
