/*
 * axis-web-indexers-definitions.c - the site definitions on the Indexers
 * pages: the installed ones and why each is usable or not, installing one
 * from a file, one definition's page with the form that adds an indexer
 * running it, removing one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only.  A definition is a built-in one (Axis's own, marked
 * so, with its version and whether an override of the administrator's is in
 * use or ignored) or the administrator's data, written by the Prowlarr and
 * Jackett communities: the pages say so, and show what it says of itself
 * only as text.  Installing is the one upload of these pages: a multipart
 * form (the CSRF field is read from it as from any form), its file at most
 * AXIS_INDEXER_DEFINITION_MAX_BYTES, read and judged by the indexer layer
 * before anything is stored; a built-in one's page also takes its override
 * pasted, and removes it.  Nothing here fetches anything; the fetch of
 * Jackett's definitions an administrator asks for is in axis-web-indexers-
 * jackett.c, and this file shows where a fetched one came from.
 */

#include "web/axis-web-indexers-private.h"

#include <string.h>

#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-shipped.h"
#include "indexer/def/axis-def.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* The field the file is uploaded in. */
#define FILE_FIELD "definition"

static const AxisWebColumn definition_columns[] = {
	{ "Site", AXIS_WEB_COL_NAME },
	{ "Searched for", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Why not usable", AXIS_WEB_COL_WIDE },
	{ "Indexers using it", AXIS_WEB_COL_COUNT },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* ── Pieces ─────────────────────────────────────────────────────── */

/* "Movies and TV", "Movies", "TV", "words", or that it has nothing for
 * Axis. */
static const gchar *
covers_text(const AxisIndexerDefinitionInfo *info)
{
	if (info->movies && info->tv)
		return "Movies and TV";
	if (info->movies)
		return "Movies";
	if (info->tv)
		return "TV";
	if (info->general)
		return "Words (a general site)";
	return "Nothing Axis searches for";
}

static HtmxNode *
state_badge(const AxisIndexerDefinitionInfo *info)
{
	return info->usable ? axis_web_browser_badge("Usable", "badge-succeeded") :
		axis_web_browser_badge("Not usable yet", "badge-warn");
}

static HtmxNode *
text_cell(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
node_cell(HtmxNode *child)
{
	HtmxTd *cell = htmx_td_new();

	if (child != NULL)
		ADD(cell, child);
	return HTMX_NODE(cell);
}

/* The note on whose the definitions are. */
static HtmxNode *
licence_note(void)
{
	return axis_web_storage_text("help", "Axis comes with a few definitions of its own, marked Built in: written, "
		"tested and kept up to date with Axis, under its licence. Any other is not part of Axis: the community's are "
		"written and kept by the Prowlarr and Jackett communities (the Prowlarr/Indexers repository on GitHub), and "
		"their own terms apply to them. That repository names no licence in its files; most of its definitions come "
		"from Jackett, which is published under the GNU GPL version 2. Install only definitions you have the right "
		"to use. Axis comes with none of them and fetches none by itself: install a file by hand, or fetch the ones "
		"you choose from Jackett's repository under Fetch definitions from Jackett.");
}

/* "Built in, version 1", "Your override, version 2 (built in: 1)",
 * "Fetched from Jackett (commit 0123456789)", ... */
static gchar *
origin_text(const AxisIndexerDefinitionInfo *info)
{
	if (!info->built_in && info->origin != NULL)
		return g_strdup_printf("Fetched from Jackett (commit %.10s)%s", info->origin_commit != NULL ?
			info->origin_commit : "", info->edited ? ", changed by hand since" : "");
	if (!info->built_in)
		return info->version > 0 ? g_strdup_printf("Installed by hand, version %d", info->version) :
			g_strdup("Installed by hand");
	if (info->overridden)
		return g_strdup_printf("Built in, overridden by your version %d (built in: %d)", info->override_version,
			info->built_in_version);
	if (info->override_ignored)
		return g_strdup_printf("Built in, version %d; your override is not used", info->built_in_version);
	return g_strdup_printf("Built in, version %d", info->built_in_version);
}

static HtmxNode *
definition_row(
	HtmxRequest                     *request,
	const AxisIndexerDefinitionInfo *info
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxTd *actions = htmx_td_new();
	HtmxSpan *detail = htmx_span_new();
	g_autofree gchar *href = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT, info->id);
	g_autofree gchar *row_id = g_strdup_printf("definition-%" G_GINT64_FORMAT, info->id);
	g_autofree gchar *count = axis_web_format_count(info->n_indexers);
	g_autofree gchar *origin = origin_text(info);
	g_autofree gchar *where = g_strdup_printf("%s, %s; %s", info->def_id, info->links[0] != NULL ? info->links[0] : "",
		origin);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_element_set_id(HTMX_ELEMENT(row), row_id);
	ADD(bdi, axis_web_breakable(info->name));
	ADD(link, bdi);
	ADD(name, link);
	htmx_element_add_class(HTMX_ELEMENT(detail), "range-detail");
	ADD(detail, axis_web_breakable(where));
	ADD(name, detail);
	if (info->override_ignored)
		ADD(name, axis_web_browser_badge("Override not used", "badge-warn"));
	/* One fetched from Jackett: what the last check found, and its tick for
	 * the update form of the section below (axis-web-indexers-jackett.c). */
	if (info->origin != NULL && info->origin_file != NULL) {
		g_autofree gchar *field = g_strconcat(AXIS_WEB_INDEXERS_JACKETT_PICK, info->origin_file, NULL);
		g_autofree gchar *tick_id = g_strdup_printf("jackett-pick-%" G_GINT64_FORMAT, info->id);
		g_autofree gchar *tick_text = g_strdup_printf("Tick to update %s from Jackett", info->name);
		HtmxDiv *pick = htmx_div_new();
		HtmxInput *tick = htmx_input_new(HTMX_INPUT_CHECKBOX);
		HtmxLabel *tick_label = htmx_label_new_for(tick_id);

		if (info->newer_upstream)
			ADD(name, axis_web_browser_badge("Newer at Jackett", "badge-warn"));
		if (info->gone_upstream)
			ADD(name, axis_web_browser_badge("No longer at Jackett", "badge-warn"));
		if (info->edited)
			ADD(name, axis_web_browser_badge("Changed by hand", "badge-warn"));
		htmx_element_add_class(HTMX_ELEMENT(pick), "field-check");
		htmx_element_set_id(HTMX_ELEMENT(tick), tick_id);
		htmx_input_set_name(tick, field);
		htmx_input_set_value(tick, "yes");
		htmx_element_set_attribute(HTMX_ELEMENT(tick), "form", AXIS_WEB_INDEXERS_JACKETT_UPDATE_FORM);
		htmx_node_set_text_content(HTMX_NODE(tick_label), tick_text);
		ADD(pick, tick);
		ADD(pick, tick_label);
		ADD(name, pick);
	}
	ADD(row, name);
	ADD(row, text_cell(covers_text(info)));
	ADD(row, node_cell(state_badge(info)));
	ADD(row, text_cell(info->usable ? "" : info->problem));
	ADD(row, text_cell(count));
	if (info->usable) {
		HtmxA *add = htmx_a_new_with_href(href);
		g_autofree gchar *aria = g_strdup_printf("Add an indexer for %s", info->name);

		htmx_element_add_class(HTMX_ELEMENT(add), "button-small");
		htmx_node_set_text_content(HTMX_NODE(add), "Add an indexer");
		htmx_element_set_attribute(HTMX_ELEMENT(add), "aria-label", aria);
		ADD(actions, add);
	}
	/* Its file changed at Jackett: the update form's button for this one. */
	if (info->origin != NULL && info->origin_file != NULL && info->newer_upstream) {
		g_autofree gchar *aria = g_strdup_printf("Update %s from Jackett", info->name);
		HtmxNode *update = axis_web_auth_button("Update", "button-small");

		htmx_element_set_attribute(HTMX_ELEMENT(update), "form", AXIS_WEB_INDEXERS_JACKETT_UPDATE_FORM);
		htmx_element_set_attribute(HTMX_ELEMENT(update), "name", "only");
		htmx_element_set_attribute(HTMX_ELEMENT(update), "value", info->origin_file);
		htmx_element_set_attribute(HTMX_ELEMENT(update), "aria-label", aria);
		ADD(actions, update);
	}
	/* A built-in one is not removed (its override is, on its page). */
	if (info->n_indexers == 0 && !info->built_in) {
		g_autofree gchar *action = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "/remove", info->id);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		g_autofree gchar *aria = g_strdup_printf("Remove the definition of %s", info->name);
		HtmxNode *button = axis_web_auth_button("Remove definition", "button-small");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_hidden("confirm", "yes"));
		htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
		ADD(form, button);
		ADD(actions, form);
	}
	ADD(row, actions);
	return HTMX_NODE(row);
}

/* The upload form. */
static HtmxNode *
install_form(HtmxRequest *request)
{
	HtmxNode *form = axis_web_auth_post_form(request, "/indexers/definitions/install");
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for("definition-file");
	HtmxInput *input = htmx_input_new(HTMX_INPUT_FILE);

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "enctype", "multipart/form-data");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Definition file");
	htmx_element_set_id(HTMX_ELEMENT(input), "definition-file");
	htmx_input_set_name(input, FILE_FIELD);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "accept", ".yml,.yaml");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "required", "required");
	ADD(field, label);
	ADD(field, input);
	ADD(field, axis_web_storage_text("help", "One YAML file of the community's format, at most 256 KiB. A file with "
		"the id of a definition already installed replaces it, and the indexers that run it run the new one; one with "
		"a built-in definition's id is taken as its override when its version is later."));
	ADD(form, field);
	ADD(form, axis_web_auth_button("Install the definition", "button-primary"));
	return form;
}

HtmxNode *
axis_web_indexers_definitions_card(
	HtmxRequest   *request,
	GPtrArray     *definitions,
	AxisWebNotice *notice
){
	HtmxNode *card = axis_web_storage_card("Other torrent sites, through site definitions");
	gboolean any_fetched = FALSE;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "definitions");
	axis_web_notice_put(notice, card, "definitions");

	ADD(card, axis_web_storage_text(NULL, "A site definition describes how to search a torrent site that has no API "
		"of its own: which address to ask, how to read its pages, and where each release is. They are the YAML files "
		"Prowlarr and Jackett use. The built-in ones are offered above; for another, install one, then add an "
		"indexer that runs it. Axis asks only the site's own address, and a release only when it can be fetched from "
		"there. Public sites without a login can be used in this version; a definition that needs something Axis "
		"does not have yet says what."));
	ADD(card, licence_note());
	if (definitions == NULL || definitions->len == 0) {
		ADD(card, axis_web_storage_text("help", "No site definition is installed yet. Install one with the form below."));
	} else {
		HtmxNode *table = axis_web_table_new("indexer-definitions", definition_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < definitions->len; i++) {
			const AxisIndexerDefinitionInfo *info = g_ptr_array_index(definitions, i);

			any_fetched = any_fetched || info->origin != NULL;
			ADD(body, definition_row(request, info));
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(card, install_form(request));
	ADD(card, axis_web_indexers_jackett_section(request, any_fetched));
	return card;
}

/* ── One definition's page ──────────────────────────────────────── */

typedef struct
{
	gint64                     id;
	AxisIndexerDefinitionInfo *info;
	gchar                     *text;	/* the file in use */
	gchar                     *source;	/* nullable: where the version in use came from */
	gchar                     *done;
	gchar                     *refusal;	/* nullable: why an override was refused */
	GError                    *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_clear_pointer(&work->info, axis_indexer_definition_info_free);
	g_free(work->text);
	g_free(work->source);
	g_free(work->done);
	g_free(work->refusal);
	g_clear_error(&work->error);
	g_free(work);
}

/* Which version of a built-in definition is in use and where it came from:
 * the data directory's file (the same as the one in use), or this page;
 * a file there that is not in use is named, with its version. */
static gchar *
source_text(
	const gchar                     *dir,
	const AxisIndexerDefinitionInfo *info,
	const gchar                     *in_use
){
	g_autofree gchar *name = g_strconcat(info->def_id, ".yml", NULL);
	g_autofree gchar *path = dir != NULL ? g_build_filename(dir, name, NULL) : NULL;
	g_autofree gchar *file = NULL;
	g_autoptr(AxisDef) def = NULL;
	gsize length = 0;
	gint file_version = 0;

	if (path != NULL && g_file_test(path, G_FILE_TEST_EXISTS)) {
		file = axis_indexer_read_file_at_most(path, AXIS_INDEXER_DEFINITION_MAX_BYTES, &length, NULL);
		def = file != NULL ? axis_def_load(file, length, NULL) : NULL;
		file_version = def != NULL ? def->version : 0;
	}
	if (info->overridden && file != NULL && g_strcmp0(file, in_use) == 0)
		return g_strdup_printf("In use: your version %d, from the file %s in the data directory.", info->version,
			path);
	if (info->overridden && file != NULL)
		return g_strdup_printf("In use: your version %d, pasted or uploaded on this page. The file %s in the data "
			"directory (%s) is not used: the version in use is as new or newer, or the file was not taken (the "
			"log says why).", info->version, path, file_version > 0 ? "a version it says" : "it says no version");
	if (info->overridden)
		return g_strdup_printf("In use: your version %d, pasted or uploaded on this page.", info->version);
	if (file != NULL)
		return g_strdup_printf("In use: the built-in version %d. The file %s in the data directory is not used: it "
			"was not taken at start (the log says why), or a newer Axis has a version as new.", info->version, path);
	return g_strdup_printf("In use: the built-in version %d.", info->version);
}

static void
page_run(
	AxisWebIndexers *self,
	gpointer         data
){
	PageWork *work = data;

	if (axis_indexers_get_definition(self->indexers, work->id, &work->info, &work->error) && work->info->built_in) {
		axis_indexers_get_definition_text(self->indexers, work->id, &work->text, NULL);
		work->source = source_text(self->definitions_dir, work->info, work->text);
	}
}

/* The forms that override a built-in definition and remove its override. */
static HtmxNode *
override_card(
	HtmxRequest                     *request,
	const AxisIndexerDefinitionInfo *info,
	const gchar                     *text
){
	g_autofree gchar *action = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "/override", info->id);
	g_autofree gchar *remove = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "/override/remove", info->id);
	g_autofree gchar *intro = g_strdup_printf("When the site changes its pages and this definition no longer reads "
		"them, a newer version of it can be used at once, without a new Axis: paste it here (or install it as a file "
		"above, or put it in the data directory as definitions/%s.yml, read at start). It must keep the id %s and "
		"say a later version than %d (\"version: %d\" at its top), and it is checked before it is taken. Once an "
		"Axis that has a newer version of its own is installed, yours is no longer used, and this page says so.",
		info->def_id, info->def_id, info->built_in_version, info->built_in_version + 1);
	HtmxNode *card = axis_web_storage_card("Repairing it: your own version");
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for("override-text");
	HtmxTextarea *area = htmx_textarea_new();

	ADD(card, axis_web_storage_text(NULL, intro));
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "The definition, pasted");
	htmx_element_set_id(HTMX_ELEMENT(area), "override-text");
	htmx_element_set_attribute(HTMX_ELEMENT(area), "name", "text");
	htmx_element_set_attribute(HTMX_ELEMENT(area), "rows", "12");
	htmx_element_set_attribute(HTMX_ELEMENT(area), "spellcheck", "false");
	htmx_element_set_attribute(HTMX_ELEMENT(area), "required", "required");
	ADD(field, label);
	ADD(field, area);
	ADD(form, field);
	ADD(form, axis_web_auth_button("Use this version", "button-primary"));
	ADD(card, form);
	if (info->overridden || info->override_ignored) {
		HtmxNode *removal = axis_web_auth_post_form(request, remove);

		htmx_element_add_class(HTMX_ELEMENT(removal), "inline-form");
		ADD(removal, axis_web_auth_hidden("confirm", "yes"));
		ADD(removal, axis_web_auth_button("Remove your version: use the built-in one", "button-small"));
		ADD(card, removal);
	}
	if (text != NULL) {
		HtmxDetails *details = htmx_details_new();
		HtmxSummary *summary = htmx_summary_new();
		HtmxPre *pre = htmx_pre_new();

		htmx_element_add_class(HTMX_ELEMENT(details), "guide-more");
		htmx_node_set_text_content(HTMX_NODE(summary), "The file in use, to start a repair from");
		htmx_node_set_text_content(HTMX_NODE(pre), text);
		ADD(details, summary);
		ADD(details, pre);
		ADD(card, details);
	}
	return card;
}

static HtmxNode *
fact(
	const gchar *label,
	const gchar *value
){
	HtmxP *line = htmx_p_new();
	HtmxStrong *name = htmx_strong_new();
	HtmxSpan *text = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(name), label);
	ADD(line, name);
	ADD(text, axis_web_breakable(value != NULL && value[0] != '\0' ? value : "not stated"));
	ADD(line, text);
	return HTMX_NODE(line);
}

/* The form that adds an indexer running @info, posted to /indexers. */
HtmxNode *
axis_web_indexers_definition_add_form(
	HtmxRequest                     *request,
	const AxisIndexerDefinitionInfo *info,
	const gchar                     *prefix
){
	g_autofree gchar *name_id = g_strdup_printf("%s-name", prefix);
	g_autofree gchar *address_id = g_strdup_printf("%s-address", prefix);
	g_autofree gchar *other_id = g_strdup_printf("%s-address-other", prefix);
	g_autofree gchar *priority_id = g_strdup_printf("%s-priority", prefix);
	g_autofree gchar *enabled_id = g_strdup_printf("%s-enabled", prefix);
	HtmxNode *form = axis_web_auth_post_form(request, "/indexers");
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(address_id);
	HtmxSelect *select = htmx_select_new_with_name("address");
	HtmxDiv *enabled = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *box_label = htmx_label_new_for(enabled_id);
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, info->id);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_auth_hidden("kind", AXIS_INDEXER_CARDIGANN_NAME));
	ADD(form, axis_web_auth_hidden("definition", id_text));
	ADD(form, axis_web_storage_text_field(name_id, "Name", "name", NULL, info->name,
		"The name Axis shows on its pages; empty is the definition's name."));
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Address");
	htmx_element_set_id(HTMX_ELEMENT(select), address_id);
	for (i = 0; info->links[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(info->links[i], info->links[i]);

		if (i == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	ADD(field, label);
	ADD(field, select);
	ADD(field, axis_web_storage_text("help", "One of the addresses the definition lists for its site, the first its "
		"current one. Axis asks this address and nothing else."));
	ADD(form, field);
	ADD(form, axis_web_storage_text_field(other_id, "Or another address (optional)",
		"address_other", NULL, "https://", "Only when the site has moved to an address its definition does not list "
		"yet, or for a copy of the site you run yourself: http or https, its host and port, and a path if the site "
		"is under one. Typing it is choosing it."));
	ADD(form, axis_web_storage_text_field(priority_id, "Priority", "priority", NULL, "25",
		"1 to 50, lower first: when two indexers found the same release, it is fetched from the one first here. "
		"Empty is 25."));
	htmx_element_add_class(HTMX_ELEMENT(enabled), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), enabled_id);
	htmx_input_set_name(box, "enabled");
	htmx_input_set_value(box, "yes");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(box_label), "Switched on");
	ADD(enabled, box);
	ADD(enabled, box_label);
	ADD(form, enabled);
	ADD(form, axis_web_auth_button("Add the indexer", "button-primary"));
	return form;
}

static const gchar *
page_done_message(const gchar *key)
{
	if (g_strcmp0(key, "installed") == 0)
		return "The definition has been installed.";
	if (g_strcmp0(key, "replaced") == 0)
		return "The definition has been replaced by the file's: the indexers that run it run the new one, and their "
			"kept searches were forgotten.";
	if (g_strcmp0(key, "unchanged") == 0)
		return "That file is the definition already installed: nothing has changed.";
	if (g_strcmp0(key, "overridden") == 0)
		return "Your version is in use: the indexers that run this definition run it from their next search, their "
			"kept searches were forgotten, and a hold for pages that no longer looked as expected was lifted.";
	if (g_strcmp0(key, "override-unchanged") == 0)
		return "That is the version already in use: nothing has changed.";
	if (g_strcmp0(key, "override-removed") == 0)
		return "Your version has been removed: the built-in one is in use again.";
	if (g_strcmp0(key, "put-back") == 0)
		return "The earlier version is in use again: the indexers that run this definition run it from their next "
			"search, and their kept searches were forgotten.";
	return NULL;
}

static HtmxResponse *
page_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	PageWork *work = data;
	const AxisIndexerDefinitionInfo *info = work->info;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxHeading *title;
	g_autofree gchar *installed = NULL;
	g_autofree gchar *updated = NULL;
	g_autofree gchar *size = NULL;
	g_autofree gchar *links = NULL;
	const gchar *message = page_done_message(work->done);
	HtmxNode *card;
	HtmxA *back;

	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	title = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(title), info->name);
	ADD(content, title);
	ADD(content, axis_web_page_intro("One of the site definitions installed on this Axis: what it says of its site, "
		"whether this version of Axis can run it, and the form that adds an indexer running it."));
	if (message != NULL)
		ADD(content, axis_web_auth_notice(FALSE, message));
	if (work->refusal != NULL)
		ADD(content, axis_web_auth_notice(TRUE, work->refusal));
	card = axis_web_storage_card("About this site definition");
	installed = axis_web_format_time(info->installed_us, "never");
	updated = axis_web_format_time(info->updated_us, "never");
	/* The hash's start is enough to tell two files apart on a page (all of
	 * it would not fit a phone's width). */
	size = g_strdup_printf("%" G_GINT64_FORMAT " bytes, SHA-256 %.12s...", info->size_bytes, info->sha256);
	links = g_strjoinv(", ", info->links);
	ADD(card, state_badge(info));
	if (!info->usable)
		ADD(card, axis_web_storage_text("notice notice-warning", info->problem));
	ADD(card, fact("Definition id: ", info->def_id));
	{
		g_autofree gchar *origin = origin_text(info);

		ADD(card, fact("Where it comes from: ", origin));
	}
	if (info->override_ignored && info->override_note != NULL) {
		g_autofree gchar *note = g_strdup_printf("Your version (%d) is not used: %s Remove it, or paste a later one.",
			info->override_version, info->override_note);

		ADD(card, axis_web_storage_text("notice notice-warning", note));
	}
	if (work->source != NULL)
		ADD(card, axis_web_storage_text(NULL, work->source));
	if (info->origin != NULL) {
		g_autofree gchar *fetched = axis_web_format_time(info->origin_us, "never");
		g_autofree gchar *from = g_strdup_printf("%s at commit %s, on %s", info->origin_file != NULL ?
			info->origin_file : "", info->origin_commit != NULL ? info->origin_commit : "", fetched);

		ADD(card, fact("Fetched from Jackett: ", from));
		if (info->newer_upstream)
			ADD(card, axis_web_storage_text("notice notice-warning", "Jackett's file for this definition has "
				"changed since it was fetched. Update it from the list of installed definitions on the Indexers "
				"page."));
		if (info->gone_upstream)
			ADD(card, axis_web_storage_text("notice notice-warning", "Jackett's repository no longer has a file of "
				"this name, as of the list Axis last read. The definition keeps working here as it is."));
		if (info->edited)
			ADD(card, axis_web_storage_text("notice notice-warning", "The file in use is not the one fetched: a "
				"file was installed over it by hand. An update from Jackett leaves it as it is unless you tick the "
				"box that says to replace it."));
	}
	if (info->has_previous) {
		g_autofree gchar *action = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "/put-back", info->id);
		HtmxNode *back_form = axis_web_auth_post_form(request, action);

		g_autofree gchar *kept_when = axis_web_format_time(info->previous_us, "an unknown day");
		g_autofree gchar *kept = info->previous_from_jackett ?
			g_strdup_printf("The version fetched from Jackett on %s is kept.", kept_when) :
			g_strdup("The version installed or changed by hand before is kept.");
		g_autofree gchar *help = g_strconcat(kept, " Putting it back makes it the one in use again, for the indexers "
			"that run this definition too; the version in use now is then not kept.", NULL);

		ADD(card, axis_web_storage_text("help", help));
		htmx_element_add_class(HTMX_ELEMENT(back_form), "inline-form");
		ADD(back_form, axis_web_auth_hidden("confirm", "yes"));
		ADD(back_form, axis_web_auth_button("Put the earlier version back", "button-small"));
		ADD(card, back_form);
	}
	ADD(card, fact("Description: ", info->description));
	ADD(card, fact("Site addresses: ", links));
	ADD(card, fact("Searched for: ", covers_text(info)));
	ADD(card, fact("Language: ", info->language));
	ADD(card, fact("File: ", size));
	ADD(card, fact("Installed: ", installed));
	ADD(card, fact("Last replaced: ", updated));
	ADD(card, licence_note());
	ADD(content, card);
	if (info->usable) {
		card = axis_web_storage_card("Add an indexer that runs this definition");
		ADD(card, axis_web_storage_text(NULL, "It takes no account and no key. It is asked no faster than the rate "
			"set for public sites, and its searches show on the Indexers page with the others."));
		ADD(card, axis_web_indexers_definition_add_form(request, info, "definition"));
		ADD(content, card);
	}
	if (info->built_in)
		ADD(content, override_card(request, info, work->text));
	back = htmx_a_new_with_href("/indexers");
	htmx_node_set_text_content(HTMX_NODE(back), "Back to Indexers");
	ADD(content, back);
	return axis_web_page_respond(request, info->name, HTMX_NODE(content));
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	PageWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	work = g_new0(PageWork, 1);
	work->id = id;
	if (page_done_message(htmx_request_get_query_param(request, "done")) != NULL)
		work->done = g_strdup(htmx_request_get_query_param(request, "done"));
	return axis_web_indexers_dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

/* ── Installing and removing ────────────────────────────────────── */

typedef struct
{
	GBytes                         *file;	/* nullable: none was sent */
	gint64                          id;
	AxisIndexerDefinitionInstalled  installed;
	GPtrArray                      *sources;
	GPtrArray                      *definitions;
	GError                         *error;
} ChangeWork;

static void
change_work_free(gpointer data)
{
	ChangeWork *work = data;

	g_clear_pointer(&work->file, g_bytes_unref);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_pointer(&work->definitions, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

/* After a refusal, what the Indexers page shows. */
static void
reread(
	AxisWebIndexers *self,
	ChangeWork      *work
){
	g_autoptr(GError) local = NULL;

	if (work->error == NULL || work->error->domain != AXIS_INDEXER_ERROR ||
	    g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return;
	if (!axis_indexers_list_sources(self->indexers, &work->sources, &local))
		return;
	axis_indexers_list_definitions(self->indexers, &work->definitions, NULL);
}

static void
install_run(
	AxisWebIndexers *self,
	gpointer         data
){
	ChangeWork *work = data;
	gsize length = 0;
	const gchar *bytes;

	if (work->file == NULL) {
		g_set_error_literal(&work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Choose a definition file to install.");
	} else {
		bytes = g_bytes_get_data(work->file, &length);
		axis_indexers_install_definition(self->indexers, bytes, length, &work->installed, &work->error);
	}
	reread(self, work);
}

static HtmxResponse *
refused(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	ChangeWork      *work
){
	if (g_error_matches(work->error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (work->error->domain == AXIS_INDEXER_ERROR && work->sources != NULL)
		return axis_web_indexers_list_response(self, request, work->sources, work->definitions, work->error->message,
			TRUE, 400);
	return axis_web_internal_error(request, work->error);
}

static HtmxResponse *
install_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	ChangeWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return refused(self, request, work);
	axis_web_storage_audit(request, "indexer definition %" G_GINT64_FORMAT " %s", work->installed.id,
		work->installed.override ? (work->installed.unchanged ? "override installed again, unchanged" : "overridden") :
		work->installed.unchanged ? "installed again, unchanged" : work->installed.replaced ? "replaced" : "installed");
	location = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "?done=%s", work->installed.id,
		work->installed.override ? (work->installed.unchanged ? "override-unchanged" : "overridden") :
		work->installed.unchanged ? "unchanged" : work->installed.replaced ? "replaced" : "installed");
	return axis_web_auth_redirect(request, location);
}

/* The uploaded file of the multipart form, when it is one of the size a
 * definition may be (a larger one is refused by the layer, saying so). */
static GBytes *
uploaded_file(HtmxRequest *request)
{
	const gchar *content_type = htmx_request_get_content_type(request);
	GBytes *body = htmx_request_get_body_bytes(request);
	g_autoptr(GPtrArray) files = NULL;
	GHashTable *fields = NULL;
	GBytes *found = NULL;
	guint i;

	if (content_type == NULL || body == NULL || g_ascii_strncasecmp(content_type, "multipart/form-data", 19) != 0)
		return NULL;
	files = htmx_uploaded_file_parse_multipart(content_type, body, &fields, NULL);
	g_clear_pointer(&fields, g_hash_table_unref);
	for (i = 0; files != NULL && i < files->len && found == NULL; i++) {
		const HtmxUploadedFile *file = g_ptr_array_index(files, i);

		if (g_strcmp0(htmx_uploaded_file_get_field_name(file), FILE_FIELD) == 0 &&
		    htmx_uploaded_file_get_data(file) != NULL && htmx_uploaded_file_get_size(file) > 0)
			found = g_bytes_ref(htmx_uploaded_file_get_data(file));
	}
	return found;
}

static HtmxResponse *
handle_install(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work = g_new0(ChangeWork, 1);

	work->file = uploaded_file(request);
	return axis_web_indexers_dispatch(user_data, request, install_run, install_done, work, change_work_free);
}

static void
remove_run(
	AxisWebIndexers *self,
	gpointer         data
){
	ChangeWork *work = data;

	axis_indexers_remove_definition(self->indexers, work->id, &work->error);
	reread(self, work);
}

static HtmxResponse *
remove_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	ChangeWork *work = data;

	if (work->error != NULL)
		return refused(self, request, work);
	axis_web_storage_audit(request, "indexer definition %" G_GINT64_FORMAT " removed", work->id);
	return axis_web_place_redirect(request, "/indexers", "definition-removed", "definitions");
}

static HtmxResponse *
handle_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ChangeWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "The form to remove it was not complete.");
	work = g_new0(ChangeWork, 1);
	work->id = id;
	return axis_web_indexers_dispatch(user_data, request, remove_run, remove_done, work, change_work_free);
}

/* ── A built-in one's override ──────────────────────────────────── */

typedef struct
{
	PageWork                        page;
	GBytes                         *data;	/* nullable: nothing was sent */
	gboolean                        removing;
	AxisIndexerDefinitionInstalled  installed;
	GError                         *failure;
} OverrideWork;

static void
override_work_free(gpointer data)
{
	OverrideWork *work = data;

	g_clear_pointer(&work->page.info, axis_indexer_definition_info_free);
	g_free(work->page.text);
	g_free(work->page.source);
	g_free(work->page.done);
	g_free(work->page.refusal);
	g_clear_error(&work->page.error);
	g_clear_pointer(&work->data, g_bytes_unref);
	g_clear_error(&work->failure);
	g_free(work);
}

static void
override_run(
	AxisWebIndexers *self,
	gpointer         data
){
	OverrideWork *work = data;
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;

	if (!axis_indexers_get_definition(self->indexers, work->page.id, &info, &work->failure))
		return;
	if (!info->built_in) {
		g_set_error_literal(&work->failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Only a built-in definition has an override; install another as a file.");
	} else if (work->removing) {
		axis_indexers_remove_definition_override(self->indexers, work->page.id, &work->failure);
	} else if (work->data == NULL || g_bytes_get_size(work->data) == 0) {
		g_set_error_literal(&work->failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
			"Paste the definition to use.");
	} else {
		gsize length = 0;
		const gchar *bytes = g_bytes_get_data(work->data, &length);
		g_autoptr(AxisDef) def = axis_def_load(bytes, length, NULL);

		/* What is pasted here is this definition's next version, nothing
		 * else. */
		if (def != NULL && strcmp(def->id, info->def_id) != 0)
			g_set_error(&work->failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID,
				"That definition's id is %.64s: a version of this one keeps the id %s.", def->id, info->def_id);
		else
			axis_indexers_install_definition(self->indexers, bytes, length, &work->installed, &work->failure);
	}
	/* Refused: the page again, saying why. */
	if (work->failure != NULL && work->failure->domain == AXIS_INDEXER_ERROR &&
	    !g_error_matches(work->failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND)) {
		work->page.refusal = g_strdup(work->failure->message);
		page_run(self, &work->page);
	}
}

static HtmxResponse *
override_done(
	AxisWebIndexers *self,
	HtmxRequest     *request,
	gpointer         data
){
	OverrideWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (work->failure != NULL && work->page.refusal == NULL)
		return axis_web_internal_error(request, work->failure);
	if (work->failure != NULL) {
		HtmxResponse *response = page_done(self, request, &work->page);

		htmx_response_set_status(response, 400);
		return response;
	}
	axis_web_storage_audit(request, "indexer definition %" G_GINT64_FORMAT " %s", work->page.id, work->removing ?
		"override removed" : work->installed.unchanged ? "override pasted again, unchanged" : "overridden");
	location = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "?done=%s", work->page.id, work->removing ?
		"override-removed" : work->installed.unchanged ? "override-unchanged" : "overridden");
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_override(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	OverrideWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *text;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	work = g_new0(OverrideWork, 1);
	work->page.id = id;
	text = htmx_request_get_form_value(request, "text");
	if (text != NULL && strlen(text) <= AXIS_INDEXER_DEFINITION_MAX_BYTES)
		work->data = g_bytes_new(text, strlen(text));
	else if (text != NULL)
		work->data = g_bytes_new(text, AXIS_INDEXER_DEFINITION_MAX_BYTES + 1);
	return axis_web_indexers_dispatch(user_data, request, override_run, override_done, work, override_work_free);
}

static HtmxResponse *
handle_override_remove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	OverrideWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such definition installed.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "The form to remove it was not complete.");
	work = g_new0(OverrideWork, 1);
	work->page.id = id;
	work->removing = TRUE;
	return axis_web_indexers_dispatch(user_data, request, override_run, override_done, work, override_work_free);
}

void
axis_web_indexers_register_definitions(
	AxisWebIndexers *self,
	AxisWeb         *web
){
	/* First: "/indexers/definitions/jackett" is no definition's page. */
	axis_web_indexers_register_jackett(self, web);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/definitions/:id/override", AXIS_WEB_ROUTE_ADMIN,
		handle_override, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/definitions/:id/override/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_override_remove, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/definitions/install", AXIS_WEB_ROUTE_ADMIN, handle_install,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/indexers/definitions/:id", AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/indexers/definitions/:id/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove, self);
}
