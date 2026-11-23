/*
 * axis-web-identify-pages.c - Media management -> Identify: every films
 * root's figures, one root's review lists, its links and the bulk add, a
 * unit's page with its evidence, candidates and search, and the actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A path or a name from disk is shown through its stored form
 * (axis_web_browser_name_node(): escaped bytes where it is not UTF-8, set as
 * text), a provider's title as text.  Every form posts with the session's
 * CSRF field and is drawn for administrators only; the routes that take them
 * are AXIS_WEB_ROUTE_ADMIN whatever a page offered.
 *
 * An action comes back to where the person was working (web/axis-web-place.h):
 * every card of a list has the id "<list>-<unit id>" and every list's card
 * "list-<list>"; a form says which page and list it was on (Back), and the
 * redirect lands at the card that followed the one acted on in that list
 * (the one before it when it was the last; the list's heading when the list
 * is empty), with the notice drawn there.
 */

#include "web/axis-web-identify-private.h"

#include <string.h>

#include "db/axis-db-error.h"
#include "library/axis-library-name.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Where a form goes back to ──────────────────────────────────── */

/* The lists by AxisIdentifyList, as addresses and ids name them. */
static const gchar *const list_names[] = { "likely", "unknown", "linked", "not_film", "waiting", "doubt", NULL };

static gboolean
list_from_name(
	const gchar      *name,
	AxisIdentifyList *list
){
	guint i;

	for (i = 0; name != NULL && list_names[i] != NULL; i++) {
		if (strcmp(name, list_names[i]) == 0) {
			*list = (AxisIdentifyList)i;
			return TRUE;
		}
	}
	return FALSE;
}

/* @name as the list's own (static) name, or %NULL when it names none. */
static const gchar *
list_known(const gchar *name)
{
	AxisIdentifyList list;

	return list_from_name(name, &list) ? list_names[list] : NULL;
}

/*
 * Where a form's action came from: the root's page ("root"), one list of
 * it on a page of its own ("list", from @from), or the unit's page
 * ("unit").  @list is the list the card is in; a unit's page reached from a
 * list carries it on, so that its actions come back to that list too.
 */
typedef struct
{
	const gchar *back;	/* "root", "list" or "unit" (static) */
	const gchar *list;	/* (nullable) a list's name (static) */
	guint        from;
} Back;

static const Back back_unit = { "unit", NULL, 0 };

/* Whether @back returns to a root's page or one of its lists. */
static gboolean
back_to_root(const Back *back)
{
	return strcmp(back->back, "unit") != 0;
}

/* Whether @back names the list a card is in, so that a page reached from
 * the card carries it on. */
static gboolean
back_carried(const Back *back)
{
	return back_to_root(back) && back->list != NULL;
}

/* The back target that request fields or parameters @back, @list and
 * @from name: "list" needs a list (else it is the root's page), and
 * anything else is the unit's page. */
static Back
back_read(
	const gchar *back,
	const gchar *list,
	const gchar *from
){
	Back read = back_unit;
	guint64 value = 0;

	if (g_strcmp0(back, "root") != 0 && g_strcmp0(back, "list") != 0)
		return read;
	read.back = "root";
	read.list = list_known(list);
	if (g_strcmp0(back, "list") == 0 && read.list != NULL) {
		read.back = "list";
		if (from != NULL && g_ascii_string_to_unsigned(from, 10, 0, 1000000, &value, NULL))
			read.from = (guint)value;
	}
	return read;
}

/* The hidden fields that say where a form came from. */
static void
back_fields(
	HtmxNode   *form,
	const Back *back
){
	ADD(form, axis_web_auth_hidden("back", back->back));
	if (back->list != NULL)
		ADD(form, axis_web_auth_hidden("list", back->list));
	if (back->from > 0) {
		g_autofree gchar *from = g_strdup_printf("%u", back->from);

		ADD(form, axis_web_auth_hidden("from", from));
	}
}

/* The id of a unit's card in @list: "unknown-12". */
static gchar *
card_id(
	const gchar *list,
	gint64       unit_id
){
	return g_strdup_printf("%s-%" G_GINT64_FORMAT, list, unit_id);
}

/* A unit's page; from a card of a root's list, it remembers that list. */
static gchar *
unit_href(
	gint64      unit_id,
	const Back *back
){
	if (!back_carried(back))
		return g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, unit_id);
	if (back->from > 0)
		return g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "?back=%s&list=%s&from=%u", unit_id, back->back,
			back->list, back->from);
	return g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "?back=%s&list=%s", unit_id, back->back, back->list);
}

/*
 * Where the notice of an action goes on a root's page: directly before the
 * card of @unit in @list, or under @list's heading when @unit is 0.  @node
 * is owned until a list takes it.
 */
typedef struct
{
	const gchar *list;
	gint64       unit;
	HtmxNode    *node;
} NoticePlace;

/* The notice, when it goes in @list before @unit_id (0: under its heading). */
static HtmxNode *
notice_take(
	NoticePlace *place,
	const gchar *list,
	gint64       unit_id
){
	if (place == NULL || place->node == NULL || g_strcmp0(place->list, list) != 0 || place->unit != unit_id)
		return NULL;
	return g_steal_pointer(&place->node);
}

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : level == 2 ? htmx_h2_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	if (node != NULL)
		ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
text_link(
	const gchar *href,
	const gchar *text
){
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	ADD(link, bdi);
	return HTMX_NODE(link);
}

static gchar *
minutes_text(gint64 ms)
{
	gint64 minutes = (ms + 30000) / 60000;

	if (ms <= 0)
		return g_strdup("");
	if (minutes >= 60)
		return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", minutes / 60, minutes % 60);
	return g_strdup_printf("%" G_GINT64_FORMAT " min", minutes);
}

static HtmxNode *
figure(
	guint        value,
	const gchar *label,
	const gchar *href
){
	HtmxElement *box = href != NULL ? HTMX_ELEMENT(htmx_a_new_with_href(href)) : HTMX_ELEMENT(htmx_div_new());
	HtmxSpan *number = htmx_span_new();
	HtmxSpan *text = htmx_span_new();
	g_autofree gchar *shown = axis_web_browser_format_count(value);

	htmx_element_add_class(box, "figure");
	htmx_element_add_class(HTMX_ELEMENT(number), "figure-value");
	htmx_node_set_text_content(HTMX_NODE(number), shown);
	htmx_element_add_class(HTMX_ELEMENT(text), "figure-label");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	ADD(box, number);
	ADD(box, text);
	return HTMX_NODE(box);
}

/* The last segment of a stored path. */
static gchar *
base_of(const gchar *stored)
{
	const gchar *slash = stored != NULL ? strrchr(stored, '/') : NULL;

	return g_strdup(slash != NULL ? slash + 1 : stored != NULL ? stored : "");
}

HtmxNode *
axis_web_identify_badge(const AxisIdentifyUnitInfo *unit)
{
	if (g_strcmp0(unit->state, "linked") == 0 && unit->doubt != NULL)
		return axis_web_browser_badge("Linked, now uncertain", "badge-warn");
	if (g_strcmp0(unit->state, "linked") == 0) {
		if (g_strcmp0(unit->linked_how, "imported") == 0)
			return axis_web_browser_badge("Imported", "badge-succeeded");
		if (g_strcmp0(unit->linked_how, "auto") == 0)
			return axis_web_browser_badge("Linked by Axis", "badge-succeeded");
		return axis_web_browser_badge("Linked by a person", "badge-succeeded");
	}
	if (g_strcmp0(unit->state, "not_film") == 0)
		return axis_web_browser_badge(unit->kind == AXIS_META_KIND_SERIES ? "Marked as not a series" : "Marked as not a movie",
			"badge-note");
	if (g_strcmp0(unit->state, "waiting") == 0)
		return axis_web_browser_badge("Waiting for the details provider", "badge-note");
	if (g_strcmp0(unit->confidence, "unknown") == 0 || unit->confidence == NULL)
		return axis_web_browser_badge("Unknown", "badge-note");
	return axis_web_browser_badge("Likely", "badge-warn");
}

static const gchar *
quality_words(const AxisIdentifyFileInfo *file)
{
	AxisQuality quality = AXIS_QUALITY_UNKNOWN;

	if (file->quality != NULL && axis_quality_from_name(file->quality, &quality))
		return axis_quality_label(quality);
	switch (axis_identify_resolution_from_name(file->resolution)) {
	case AXIS_RELEASE_RESOLUTION_480P:  return "Unknown source, 480p";
	case AXIS_RELEASE_RESOLUTION_576P:  return "Unknown source, 576p";
	case AXIS_RELEASE_RESOLUTION_720P:  return "Unknown source, 720p";
	case AXIS_RELEASE_RESOLUTION_1080P: return "Unknown source, 1080p";
	case AXIS_RELEASE_RESOLUTION_2160P: return "Unknown source, 2160p";
	default:                            return "Unknown";
	}
}

HtmxNode *
axis_web_identify_files_table(
	const AxisIdentifyUnitInfo *unit,
	gboolean                    offer_link
){
	static const AxisWebColumn columns[] = {
		{ "Video", AXIS_WEB_COL_NAME },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Runtime", AXIS_WEB_COL_NUMBER },
		{ "Quality", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("identify-files", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; unit->files != NULL && i < unit->files->len; i++) {
		const AxisIdentifyFileInfo *file = g_ptr_array_index(unit->files, i);
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *size = axis_size_format((guint64)MAX(file->size, 0));
		g_autofree gchar *runs = minutes_text(file->duration_ms);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxTd *quality = htmx_td_new();
		HtmxA *link = htmx_a_new_with_href(href);

		ADD(link, axis_web_browser_name_node(file->rel_path));
		ADD(name, link);
		if (file->best && unit->files->len > 1)
			ADD(name, axis_web_storage_text("range-detail", "The best copy: a movie's upgrades are judged by it."));
		/* A series' episode file: what its name holds, or why it holds no
		 * episode. */
		if (unit->kind == AXIS_META_KIND_SERIES && file->episode_text != NULL) {
			g_autofree gchar *holds = file->episode_code != NULL ? g_strdup_printf("No episode: %s.",
				file->episode_text) : g_strdup_printf("Holds %s.", file->episode_text);

			ADD(name, axis_web_storage_text("range-detail", holds));
		}
		/* A file Axis could not match: it can be linked by hand. */
		if (offer_link) {
			g_autofree gchar *by_hand = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/link", file->file_id);
			HtmxDiv *offer = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(offer), "range-detail");
			ADD(offer, axis_web_browser_link(by_hand, "Link to a movie or episode by hand"));
			ADD(name, offer);
		}
		ADD(row, name);
		ADD(row, cell_text(size));
		ADD(row, cell_text(file->duration_ms > 0 ? runs : "not read yet"));
		htmx_node_set_text_content(HTMX_NODE(quality), quality_words(file));
		if (file->quality_text != NULL)
			ADD(quality, axis_web_storage_text("range-detail", file->quality_text));
		ADD(row, quality);
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
reasons_list(const AxisIdentifyUnitInfo *unit)
{
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
	for (i = 0; unit->reasons != NULL && i < unit->reasons->len; i++) {
		const AxisIdentifyReason *reason = g_ptr_array_index(unit->reasons, i);
		g_autofree gchar *text = g_strdup_printf("%c%s.", g_ascii_toupper(reason->text[0]),
			reason->text[0] != '\0' ? reason->text + 1 : "");
		HtmxLi *item = htmx_li_new();

		htmx_node_set_text_content(HTMX_NODE(item), reason->text[0] != '\0' ? text : "");
		ADD(list, item);
	}
	return HTMX_NODE(list);
}

/* A link a later look did not confirm: what it is linked to, and why it is
 * listed. */
static HtmxNode *
doubt_line(const AxisIdentifyUnitInfo *unit)
{
	g_autofree gchar *text = g_strdup_printf("Linked to %s. %c%s.", unit->title_label != NULL ? unit->title_label :
		"a title", g_ascii_toupper(unit->doubt[0]), unit->doubt[0] != '\0' ? unit->doubt + 1 : "");
	HtmxNode *line = axis_web_storage_text("help", text);

	htmx_element_add_class(HTMX_ELEMENT(line), "identify-settle");
	return line;
}

/* A unit held for a person: what would settle it, in a sentence. */
static HtmxNode *
settle_line(const AxisIdentifyUnitInfo *unit)
{
	g_autofree gchar *text = g_strdup_printf("To settle it: %s", axis_identify_settle_text(unit->kind,
		unit->reasons));
	HtmxNode *line = axis_web_storage_text("help", text);

	htmx_element_add_class(HTMX_ELEMENT(line), "identify-settle");
	return line;
}

/* A button with its classes: "button-small" when @small, then @css. */
static HtmxNode *
button(
	const gchar *label,
	const gchar *css,
	gboolean     small
){
	HtmxNode *node = axis_web_auth_button(label, css);

	if (small)
		htmx_element_add_class(HTMX_ELEMENT(node), "button-small");
	return node;
}

/* A one-button form for a unit: @action, with @title (0: none), coming back
 * to @back. */
static HtmxNode *
act_form(
	HtmxRequest *request,
	gint64       unit_id,
	const gchar *action,
	gint64       title_id,
	const Back  *back,
	const gchar *label,
	const gchar *css,
	gboolean     small
){
	g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "/act", unit_id);
	HtmxNode *form = axis_web_auth_post_form(request, href);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("action", action));
	if (title_id > 0) {
		g_autofree gchar *title = g_strdup_printf("%" G_GINT64_FORMAT, title_id);

		ADD(form, axis_web_auth_hidden("title", title));
	}
	back_fields(form, back);
	ADD(form, button(label, css, small));
	return form;
}

/* "Search for the film": the unit's page, searching for what its names read
 * as (a GET: it changes nothing), remembering the list it came from.  The
 * page reads the names itself, so that no text from disk travels in this
 * form. */
static HtmxNode *
search_button(
	const AxisIdentifyUnitInfo *unit,
	const Back                 *back
){
	g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "#search", unit->id);
	HtmxForm *form = htmx_form_new();

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", href);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	ADD(form, axis_web_auth_hidden("find", "1"));
	if (back_carried(back))
		back_fields(HTMX_NODE(form), back);
	ADD(form, button(unit->kind == AXIS_META_KIND_SERIES ? "Search for the series" : "Search for the movie", NULL,
		TRUE));
	return HTMX_NODE(form);
}

/* What tells candidates apart at a glance, where the stored record has it:
 * a movie's running time; a series' seasons and the length of an episode.
 * %NULL when it has none of them. */
static gchar *
candidate_facts(const AxisIdentifyCandidateInfo *c)
{
	g_autoptr(GString) text = g_string_new(NULL);
	g_autofree gchar *runs = c->title->runtime > 0 ? minutes_text((gint64)c->title->runtime * 60000) : NULL;

	if (c->title->kind == AXIS_META_KIND_SERIES) {
		if (c->seasons > 0)
			g_string_append_printf(text, "%u %s", c->seasons, c->seasons == 1 ? "season" : "seasons");
		if (runs != NULL)
			g_string_append_printf(text, "%s%s an episode", text->len > 0 ? ", " : "", runs);
	} else if (runs != NULL) {
		g_string_append_printf(text, "Runs %s", runs);
	}
	return text->len > 0 ? g_strdup(text->str) : NULL;
}

/*
 * The candidates, each a small card: a poster thumbnail, the title (to its
 * page), its year and what else tells it apart beside it, and an
 * administrator's small button under them ("Confirm" on the best of one
 * under review, "This film" on the others).
 */
static HtmxNode *
pick_list(
	HtmxRequest                *request,
	const AxisIdentifyUnitInfo *unit,
	GPtrArray                  *candidates,
	gboolean                    from_search,
	const Back                 *back
){
	HtmxUl *list = htmx_ul_new();
	gboolean admin = axis_web_request_is_admin(request);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "identify-picks");
	for (i = 0; i < candidates->len; i++) {
		const AxisIdentifyCandidateInfo *c = g_ptr_array_index(candidates, i);
		g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, c->title->id);
		g_autofree gchar *year = c->title->year > 0 ? g_strdup_printf("%d", c->title->year) : g_strdup("No year");
		g_autofree gchar *facts = candidate_facts(c);
		HtmxLi *item = htmx_li_new();
		HtmxDiv *thumb = htmx_div_new();
		HtmxDiv *text = htmx_div_new();
		HtmxA *name = htmx_a_new_with_href(href);
		HtmxBdi *bdi = htmx_bdi_new();

		htmx_element_add_class(HTMX_ELEMENT(item), "identify-pick");
		htmx_element_add_class(HTMX_ELEMENT(thumb), "identify-thumb");
		ADD(thumb, axis_web_meta_poster_new(c->title, c->poster, FALSE, 0));
		ADD(item, thumb);
		htmx_element_add_class(HTMX_ELEMENT(text), "identify-pick-text");
		htmx_element_add_class(HTMX_ELEMENT(name), "identify-pick-name");
		htmx_node_set_text_content(HTMX_NODE(bdi), c->title->title != NULL ? c->title->title : "");
		ADD(name, bdi);
		ADD(text, name);
		ADD(text, axis_web_storage_text("identify-pick-year", year));
		if (facts != NULL)
			ADD(text, axis_web_storage_text("identify-pick-facts", facts));
		if (admin && !(g_strcmp0(unit->state, "linked") == 0 && unit->title_id == c->title->id)) {
			gboolean confirm = !from_search && i == 0 && g_strcmp0(unit->state, "review") == 0 &&
				g_strcmp0(unit->confidence, "unknown") != 0;

			ADD(text, act_form(request, unit->id, confirm ? "confirm" : "choose", confirm ? 0 : c->title->id, back,
				unit->kind == AXIS_META_KIND_SERIES ? (confirm ? "Confirm this series" : "Choose this series") :
				(confirm ? "Confirm this movie" : "Choose this movie"),
				confirm ? "button-primary" : NULL, TRUE));
		}
		ADD(item, text);
		ADD(list, item);
	}
	return HTMX_NODE(list);
}

/* One unit to review as a bordered row, its card in @back's list: on the
 * left its name, mark, runtime and reasons; on the right its candidates,
 * then what else can be done. */
static HtmxNode *
unit_block(
	HtmxRequest                *request,
	const AxisIdentifyUnitInfo *unit,
	const Back                 *back
){
	HtmxArticle *row = htmx_article_new();
	HtmxDiv *facts = htmx_div_new();
	HtmxDiv *side = htmx_div_new();
	HtmxDiv *head = htmx_div_new();
	g_autofree gchar *href = unit_href(unit->id, back);
	g_autofree gchar *id = card_id(back->list, unit->id);
	g_autofree gchar *runs = minutes_text(unit->duration_ms);
	g_autofree gchar *runs_text = unit->duration_ms > 0 ? g_strdup_printf("Runs %s", runs) :
		g_strdup("Not read yet");
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_element_set_id(HTMX_ELEMENT(row), id);
	htmx_element_add_class(HTMX_ELEMENT(row), "identify-row");
	htmx_element_add_class(HTMX_ELEMENT(facts), "identify-facts");
	htmx_element_add_class(HTMX_ELEMENT(head), "identify-head");
	ADD(link, axis_web_browser_name_node(unit->rel_path));
	ADD(head, link);
	ADD(head, axis_web_identify_badge(unit));
	ADD(facts, head);
	ADD(facts, axis_web_storage_text("identify-runs", runs_text));
	if (unit->doubt != NULL)
		ADD(facts, doubt_line(unit));
	ADD(facts, reasons_list(unit));
	if (g_strcmp0(unit->state, "review") == 0 || g_strcmp0(unit->state, "waiting") == 0)
		ADD(facts, settle_line(unit));
	ADD(row, facts);

	htmx_element_add_class(HTMX_ELEMENT(side), "identify-side");
	if (unit->candidates != NULL && unit->candidates->len > 0)
		ADD(side, pick_list(request, unit, unit->candidates, FALSE, back));
	if (axis_web_request_is_admin(request)) {
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "identify-actions");
		if (unit->doubt != NULL && unit->title_id > 0) {
			ADD(actions, act_form(request, unit->id, "choose", unit->title_id, back, "Keep this link",
				"button-primary", TRUE));
			ADD(actions, act_form(request, unit->id, "undo", 0, back, "Undo the link", NULL, TRUE));
		}
		ADD(actions, search_button(unit, back));
		ADD(actions, act_form(request, unit->id, "not_film", 0, back, unit->kind == AXIS_META_KIND_SERIES ?
			"Mark as not a series" : "Mark as not a movie", NULL, TRUE));
		ADD(side, actions);
	}
	ADD(row, side);
	return HTMX_NODE(row);
}

static const gchar *
done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "confirm", "Confirmed: it is linked to the title." },
		{ "choose", "Linked to the title you chose." },
		{ "not_film", "Marked as not a movie (or not a series): Axis leaves it alone." },
		{ "undo", "Undone: it is back in review, and its title no longer counts its files." },
		{ "run", "Identification of this library folder is queued: it looks at what is new or changed." },
		{ "added", "Axis manages the identified movies now: each is in the library with its file, and nothing was "
		  "searched for." },
		{ "added_series", "Axis manages the identified series now: their episodes on disk are in the library, and "
		  "nothing was searched for." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].text;
	}
	return NULL;
}

static void
notice_words(
	HtmxNode    *line,
	const gchar *words
){
	HtmxSpan *span = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(span), words);
	ADD(line, span);
}

/*
 * The notice of what @done did to @acted, in its names ("Linked <folder> to
 * <title>."), with Undo for an administrator where there is one: that Undo
 * comes back to the unit's own card (land=self).  %NULL when @acted is not
 * as @done left it (someone else acted since): the page says the fixed
 * sentence instead.
 */
static HtmxNode *
acted_notice(
	HtmxRequest                *request,
	const gchar                *done,
	const AxisIdentifyUnitInfo *acted,
	const Back                 *back
){
	gboolean linked = g_strcmp0(acted->state, "linked") == 0;
	gboolean marked = g_strcmp0(acted->state, "not_film") == 0;
	gboolean undone = g_strcmp0(acted->state, "review") == 0 || g_strcmp0(acted->state, "waiting") == 0;
	gboolean series = acted->kind == AXIS_META_KIND_SERIES;
	HtmxDiv *notice;
	HtmxP *line;

	if (!((g_strcmp0(done, "confirm") == 0 || g_strcmp0(done, "choose") == 0) && linked) &&
	    !(g_strcmp0(done, "not_film") == 0 && marked) && !(g_strcmp0(done, "undo") == 0 && undone))
		return NULL;
	notice = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
	htmx_element_add_class(HTMX_ELEMENT(notice), "notice-ok");
	htmx_element_add_class(HTMX_ELEMENT(notice), "identify-notice");
	htmx_element_set_attribute(HTMX_ELEMENT(notice), "role", "status");
	line = htmx_p_new();
	if (linked) {
		HtmxBdi *title = htmx_bdi_new();

		notice_words(HTMX_NODE(line), "Linked ");
		ADD(line, axis_web_browser_name_node(acted->rel_path));
		notice_words(HTMX_NODE(line), " to ");
		htmx_node_set_text_content(HTMX_NODE(title), acted->title_label != NULL ? acted->title_label :
			series ? "the series" : "the movie");
		ADD(line, title);
		notice_words(HTMX_NODE(line), ".");
	} else if (marked) {
		notice_words(HTMX_NODE(line), "Marked ");
		ADD(line, axis_web_browser_name_node(acted->rel_path));
		notice_words(HTMX_NODE(line), series ? " as not a series: it is left alone." :
			" as not a movie: it is left alone.");
	} else {
		notice_words(HTMX_NODE(line), "Undone: ");
		ADD(line, axis_web_browser_name_node(acted->rel_path));
		notice_words(HTMX_NODE(line), " is back in review.");
	}
	ADD(notice, line);
	if (axis_web_request_is_admin(request) && !undone && g_strcmp0(acted->linked_how, "imported") != 0) {
		HtmxNode *undo = act_form(request, acted->id, "undo", 0, back, linked ? "Undo the link" : series ?
			"Undo \"not a series\"" : "Undo \"not a movie\"", NULL, TRUE);

		ADD(undo, axis_web_auth_hidden("land", "self"));
		ADD(notice, undo);
	}
	return HTMX_NODE(notice);
}

/* Where a root is: how far identification got, and why it waits. */
static HtmxNode *
progress_block(const AxisIdentifyRootInfo *root)
{
	HtmxDiv *block = htmx_div_new();
	g_autoptr(GString) text = g_string_new(NULL);

	htmx_element_add_class(HTMX_ELEMENT(block), "identify-progress");
	if (!root->scanned) {
		g_string_append(text, "Waiting for the library folder's first complete scan.");
	} else if (root->scopes == 0 && root->last_run_us == 0) {
		g_string_append(text, "Not looked at yet: the first run comes within ten minutes.");
	} else {
		g_autofree gchar *done = axis_web_browser_format_count(root->scopes - MIN(root->pending, root->scopes));
		g_autofree gchar *all = axis_web_browser_format_count(root->scopes);

		g_string_append_printf(text, "%s of %s %s folders and files looked at", done, all,
			root->kind == AXIS_META_KIND_SERIES ? "series" : "movie");
		if (root->pending > 0)
			g_string_append_printf(text, "; %u left, %s", root->pending, root->running ? "being looked at now" :
				"a few at each run");
		g_string_append_c(text, '.');
		if (root->seasons_pending > 0)
			g_string_append_printf(text, " The episode lists of %u %s of its series are still to fetch, a few at each "
				"run.", root->seasons_pending, root->seasons_pending == 1 ? "season" : "seasons");
	}
	if (root->retry_after_us > g_get_real_time()) {
		g_autofree gchar *when = axis_web_format_time(root->retry_after_us, "");

		g_string_append_printf(text, " The details provider did not answer: the next try is after %s.", when);
	}
	ADD(block, axis_web_storage_text(NULL, text->str));
	if (root->last_run_us > 0) {
		g_autofree gchar *when = axis_web_format_time(root->last_run_us, "");
		g_autofree gchar *last = g_strdup_printf("Last run %s: %s", when, root->last_run_text != NULL ?
			root->last_run_text : "");

		ADD(block, axis_web_storage_text("as-of", last));
	}
	return HTMX_NODE(block);
}

static HtmxNode *
counts_row(
	const AxisIdentifyRootInfo *root,
	gboolean                    links
){
	HtmxDiv *row = htmx_div_new();
	g_autofree gchar *base = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT "?list=", root->root_id);
	g_autofree gchar *likely = g_strconcat(base, "likely", NULL);
	g_autofree gchar *unknown = g_strconcat(base, "unknown", NULL);
	g_autofree gchar *linked = g_strconcat(base, "linked", NULL);
	g_autofree gchar *not_film = g_strconcat(base, "not_film", NULL);

	htmx_element_add_class(HTMX_ELEMENT(row), "figure-row");
	htmx_element_add_class(HTMX_ELEMENT(row), "identify-counts");
	ADD(row, figure(root->sure, "linked by Axis (sure)",links && root->sure > 0 ? linked : NULL));
	ADD(row, figure(root->confirmed, "linked by a person", links && root->confirmed > 0 ? linked : NULL));
	ADD(row, figure(root->likely, "likely, to review", links && root->likely > 0 ? likely : NULL));
	ADD(row, figure(root->unknown, "unknown, to review", links && root->unknown > 0 ? unknown : NULL));
	ADD(row, figure(root->not_film, root->kind == AXIS_META_KIND_SERIES ? "not a series" : "not a movie",
		links && root->not_film > 0 ? not_film : NULL));
	if (root->waiting > 0)
		ADD(row, figure(root->waiting, "waiting for the details provider", NULL));
	if (root->doubted > 0)
		ADD(row, figure(root->doubted, "linked, now uncertain", NULL));
	return HTMX_NODE(row);
}

static gboolean
provider_known(AxisWebIdentify *self)
{
	AxisMeta *meta = axis_identify_get_meta(self->identify);
	guint i;

	for (i = 0; i < axis_meta_n_providers(meta); i++) {
		if ((axis_meta_provider_at(meta, i)->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_FILM)) != 0)
			return TRUE;
	}
	return FALSE;
}

static const gchar intro[] = "The movies and series already on your disks, matched to the titles they are (by an "
	"identifier in a name or an .nfo file, or by the title, year and running time), so Axis knows what you have and "
	"does not download it again; sure matches are linked by themselves, the rest wait here for you. Each video in a "
	"series' folder is read for the episodes its name holds. Nothing on disk is renamed, moved or changed.";

/* ── Media management -> Identify ───────────────────────────────── */

typedef struct
{
	GPtrArray *roots;
	GError    *error;
} RootsWork;

static void
roots_work_free(gpointer data)
{
	RootsWork *work = data;

	g_clear_pointer(&work->roots, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
roots_run(
	AxisWebIdentify *self,
	gpointer         data
){
	RootsWork *work = data;

	axis_identify_list_roots(self->identify, &work->roots, &work->error);
}

static HtmxResponse *
roots_done(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer         data
){
	RootsWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	ADD(content, heading(1, "Identify movies and series on disk"));
	ADD(content, axis_web_page_intro(intro));
	if (!provider_known(self))
		ADD(content, axis_web_storage_text("notice notice-warning", "No details provider serves movies and series yet: "
			"identification waits until TMDb has its token (System, Movie and TV details)."));
	if (!provider_known(self))
		ADD(content, axis_web_browser_link("/metadata", "Movie and TV details"));
	if (work->roots->len == 0) {
		HtmxNode *card = axis_web_storage_card("Library folders of movies and TV");

		ADD(card, axis_web_storage_text("help", "No library folder holds movies or TV. Add a library folder of type "
			"Movies or TV under Storage; its first complete scan starts identification."));
		ADD(card, axis_web_browser_link("/storage", "Add a library folder under Disks"));
		ADD(content, card);
	}
	for (i = 0; i < work->roots->len; i++) {
		const AxisIdentifyRootInfo *root = g_ptr_array_index(work->roots, i);
		g_autofree gchar *href = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, root->root_id);
		g_autofree gchar *state = g_ascii_strdown(axis_volume_state_label(root->volume_state), -1);
		g_autofree gchar *where = g_strdup_printf("%s, on %s, %s.", root->kind == AXIS_META_KIND_SERIES ?
			"TV" : "Movies", root->volume_name != NULL ? root->volume_name : "a disk", state);
		HtmxSection *card = htmx_section_new();
		HtmxHeading *h2 = htmx_h2_new();

		htmx_element_add_class(HTMX_ELEMENT(card), "card");
		htmx_element_add_class(HTMX_ELEMENT(card), "identify-root");
		ADD(h2, text_link(href, root->name));
		ADD(card, h2);
		ADD(card, axis_web_storage_text("help", where));
		ADD(card, counts_row(root, TRUE));
		ADD(card, progress_block(root));
		ADD(content, card);
	}
	return axis_web_page_respond(request, "Identify movies and series on disk", HTMX_NODE(content));
}

static HtmxResponse *
handle_roots(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_identify_dispatch(user_data, request, roots_run, roots_done, g_new0(RootsWork, 1), roots_work_free);
}

/* ── One root ───────────────────────────────────────────────────── */

typedef struct
{
	gint64                root_id;
	gchar                *list;	/* (nullable) one whole list */
	guint                 from;
	gchar                *done;
	AxisIdentifyRootInfo *root;
	GPtrArray            *likely;
	GPtrArray            *unknown;
	GPtrArray            *linked;
	GPtrArray            *not_film;
	GPtrArray            *waiting;
	GPtrArray            *doubted;
	guint                 n_likely;
	guint                 n_unknown;
	guint                 n_linked;
	guint                 n_not_film;
	guint                 n_waiting;
	guint                 n_doubted;
	GPtrArray            *profiles;	/* AxisQualityStored */
	gchar                *default_slug;
	gchar                *at;	/* (nullable) where the notice goes (axis_web_place_of()) */
	gint64                acted_id;	/* the unit an action was taken on, or 0 */
	AxisIdentifyUnitInfo *acted;	/* (nullable) that unit, when it is of this root */
	GError               *error;
} RootWork;

static void
root_work_free(gpointer data)
{
	RootWork *work = data;

	g_free(work->list);
	g_free(work->done);
	g_free(work->at);
	axis_identify_unit_info_free(work->acted);
	axis_identify_root_info_free(work->root);
	g_clear_pointer(&work->likely, g_ptr_array_unref);
	g_clear_pointer(&work->unknown, g_ptr_array_unref);
	g_clear_pointer(&work->linked, g_ptr_array_unref);
	g_clear_pointer(&work->not_film, g_ptr_array_unref);
	g_clear_pointer(&work->waiting, g_ptr_array_unref);
	g_clear_pointer(&work->doubted, g_ptr_array_unref);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	g_free(work->default_slug);
	g_clear_error(&work->error);
	g_free(work);
}

/* Where list @one of @work goes, and its total. */
static void
list_slot(
	RootWork          *work,
	AxisIdentifyList   one,
	GPtrArray       ***into,
	guint            **total
){
	switch (one) {
	case AXIS_IDENTIFY_LIST_LIKELY:
		*into = &work->likely;
		*total = &work->n_likely;
		break;
	case AXIS_IDENTIFY_LIST_UNKNOWN:
		*into = &work->unknown;
		*total = &work->n_unknown;
		break;
	case AXIS_IDENTIFY_LIST_LINKED:
		*into = &work->linked;
		*total = &work->n_linked;
		break;
	case AXIS_IDENTIFY_LIST_WAITING:
		*into = &work->waiting;
		*total = &work->n_waiting;
		break;
	case AXIS_IDENTIFY_LIST_DOUBT:
		*into = &work->doubted;
		*total = &work->n_doubted;
		break;
	default:
		*into = &work->not_film;
		*total = &work->n_not_film;
		break;
	}
}

static void
root_run(
	AxisWebIdentify *self,
	gpointer         data
){
	RootWork *work = data;
	AxisIdentifyList one;
	guint shown = AXIS_WEB_IDENTIFY_SHOWN;
	gboolean whole = work->list != NULL && list_from_name(work->list, &one);

	if (!axis_identify_get_root(self->identify, work->root_id, &work->root, &work->error))
		return;
	/* The unit an action was just taken on, for the notice that names it. */
	if (work->acted_id > 0 && axis_identify_get_unit(self->identify, work->acted_id, &work->acted, NULL) &&
	    work->acted->root_id != work->root_id)
		g_clear_pointer(&work->acted, axis_identify_unit_info_free);
	if (whole) {
		GPtrArray **into = NULL;
		guint *total = NULL;

		list_slot(work, one, &into, &total);
		axis_identify_list_units(self->identify, work->root_id, one, work->from, AXIS_WEB_IDENTIFY_PAGE,
			one == AXIS_IDENTIFY_LIST_LIKELY || one == AXIS_IDENTIFY_LIST_UNKNOWN || one == AXIS_IDENTIFY_LIST_WAITING ||
			one == AXIS_IDENTIFY_LIST_DOUBT, into, total, &work->error);
		return;
	}
	if (!axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_LIKELY, 0, shown, TRUE,
	        &work->likely, &work->n_likely, &work->error) ||
	    !axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_UNKNOWN, 0, shown, TRUE,
	        &work->unknown, &work->n_unknown, &work->error) ||
	    !axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_LINKED, 0, shown, FALSE,
	        &work->linked, &work->n_linked, &work->error) ||
	    !axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_NOT_FILM, 0, shown, FALSE,
	        &work->not_film, &work->n_not_film, &work->error) ||
	    !axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_WAITING, 0, shown, TRUE,
	        &work->waiting, &work->n_waiting, &work->error) ||
	    !axis_identify_list_units(self->identify, work->root_id, AXIS_IDENTIFY_LIST_DOUBT, 0, shown, TRUE,
	        &work->doubted, &work->n_doubted, &work->error))
		return;
	if (!axis_quality_store_list(self->quality, &work->profiles, &work->error))
		return;
	work->default_slug = axis_quality_store_default_slug(self->quality);
}

/* A table's row for the notice that goes before the row of @unit_id, across
 * its @columns. */
static void
notice_row(
	HtmxTbody   *body,
	NoticePlace *place,
	const gchar *list,
	gint64       unit_id,
	guint        columns
){
	HtmxNode *notice = notice_take(place, list, unit_id);
	g_autofree gchar *span = g_strdup_printf("%u", columns);
	HtmxTr *row;
	HtmxTd *cell;

	if (notice == NULL)
		return;
	row = htmx_tr_new();
	cell = htmx_td_new();
	htmx_element_add_class(HTMX_ELEMENT(row), "identify-notice-row");
	htmx_element_set_attribute(HTMX_ELEMENT(cell), "colspan", span);
	ADD(cell, notice);
	ADD(row, cell);
	ADD(body, row);
}

static HtmxNode *
linked_table(
	HtmxRequest *request,
	GPtrArray   *units,
	const Back  *back,
	NoticePlace *place
){
	static const AxisWebColumn movie_columns[] = {
		{ "Folder or file", AXIS_WEB_COL_NAME },
		{ "Movie", AXIS_WEB_COL_WIDE },
		{ "How", AXIS_WEB_COL_STATE },
		{ "When", AXIS_WEB_COL_DATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	static const AxisWebColumn series_columns[] = {
		{ "Folder or file", AXIS_WEB_COL_NAME },
		{ "Series", AXIS_WEB_COL_WIDE },
		{ "How", AXIS_WEB_COL_STATE },
		{ "When", AXIS_WEB_COL_DATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	const AxisWebColumn *columns = units->len > 0 && ((const AxisIdentifyUnitInfo *)g_ptr_array_index(units, 0))->kind ==
		AXIS_META_KIND_SERIES ? series_columns : movie_columns;
	HtmxNode *table = axis_web_table_new("identify-linked", columns);
	HtmxTbody *body = htmx_tbody_new();
	gboolean admin = axis_web_request_is_admin(request);
	guint i;

	for (i = 0; i < units->len; i++) {
		const AxisIdentifyUnitInfo *unit = g_ptr_array_index(units, i);
		g_autofree gchar *href = unit_href(unit->id, back);
		g_autofree gchar *id = card_id(back->list, unit->id);
		g_autofree gchar *title = unit->title_id > 0 ? g_strdup_printf("/title/%" G_GINT64_FORMAT, unit->title_id) : NULL;
		g_autofree gchar *when = axis_web_format_time(unit->decided_us > 0 ? unit->decided_us : unit->examined_us, "");
		HtmxTr *row;
		HtmxTd *name;
		HtmxA *link;

		notice_row(body, place, back->list, unit->id, G_N_ELEMENTS(movie_columns) - 1);
		row = htmx_tr_new();
		name = htmx_td_new();
		link = htmx_a_new_with_href(href);
		htmx_element_set_id(HTMX_ELEMENT(row), id);
		ADD(link, axis_web_browser_name_node(unit->rel_path));
		ADD(name, link);
		ADD(row, name);
		ADD(row, title != NULL ? cell_node(text_link(title, unit->title_label)) : cell_text(""));
		ADD(row, cell_node(axis_web_identify_badge(unit)));
		if (unit->decided_by != NULL) {
			HtmxTd *cell = htmx_td_new();
			g_autofree gchar *by = g_strdup_printf("by %s", unit->decided_by);

			htmx_node_set_text_content(HTMX_NODE(cell), when);
			ADD(cell, axis_web_storage_text("range-detail", by));
			ADD(row, cell);
		} else {
			ADD(row, cell_text(when));
		}
		ADD(row, admin && g_strcmp0(unit->linked_how, "imported") != 0 ?
			cell_node(act_form(request, unit->id, "undo", 0, back, "Undo the link", NULL, TRUE)) : cell_text(""));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
not_film_table(
	HtmxRequest *request,
	GPtrArray   *units,
	const Back  *back,
	NoticePlace *place
){
	static const AxisWebColumn columns[] = {
		{ "Folder or file", AXIS_WEB_COL_NAME },
		{ "Marked", AXIS_WEB_COL_DATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("identify-not-film", columns);
	HtmxTbody *body = htmx_tbody_new();
	gboolean admin = axis_web_request_is_admin(request);
	guint i;

	for (i = 0; i < units->len; i++) {
		const AxisIdentifyUnitInfo *unit = g_ptr_array_index(units, i);
		g_autofree gchar *href = unit_href(unit->id, back);
		g_autofree gchar *id = card_id(back->list, unit->id);
		g_autofree gchar *when = axis_web_format_time(unit->decided_us, "");
		HtmxTr *row;
		HtmxTd *name;
		HtmxA *link;

		notice_row(body, place, back->list, unit->id, G_N_ELEMENTS(columns) - 1);
		row = htmx_tr_new();
		name = htmx_td_new();
		link = htmx_a_new_with_href(href);
		htmx_element_set_id(HTMX_ELEMENT(row), id);
		ADD(link, axis_web_browser_name_node(unit->rel_path));
		ADD(name, link);
		ADD(row, name);
		ADD(row, cell_text(when));
		ADD(row, admin ? cell_node(act_form(request, unit->id, "undo", 0, back, unit->kind == AXIS_META_KIND_SERIES ?
			"Undo \"not a series\"" : "Undo \"not a movie\"", NULL, TRUE)) : cell_text(""));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

/* An administrator's bulk add: a profile and the monitoring. */
static HtmxNode *
add_card(
	HtmxRequest                *request,
	const AxisIdentifyRootInfo *root,
	GPtrArray                  *profiles,
	const gchar                *default_slug
){
	gboolean series = root->kind == AXIS_META_KIND_SERIES;
	HtmxNode *card = axis_web_storage_card(series ? "Manage every identified series" : "Manage every identified movie");
	g_autofree gchar *href = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT "/add", root->root_id);
	guint waiting = root->titles - MIN(root->managed, root->titles);
	g_autofree gchar *films = series ? axis_web_count(root->titles, "series is", "series are") :
		axis_web_count(root->titles, "movie is", "movies are");
	g_autofree gchar *managed = axis_web_count(root->managed, "is", "are");
	g_autofree gchar *text = series ? g_strdup_printf("%s identified in this library folder, and each is listed as On disk, "
		"not managed by Axis; %s managed by Axis already. Managed here, each keeps its files where they are, its "
		"episodes on disk count as in the library, and nothing is searched for.", films, managed) :
		g_strdup_printf("%s identified in this library folder, and each is listed as On disk, not managed by Axis; %s managed by Axis "
		"already. Managed here, each keeps its file where it is, and nothing is searched for.", films, managed);
	HtmxNode *form;
	HtmxDiv *profile_field;
	HtmxLabel *profile_label;
	HtmxSelect *profile;
	HtmxDiv *watch_field;
	HtmxLabel *watch_label;
	HtmxSelect *watch;
	g_autofree gchar *button = NULL;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(card), "identify-add");
	ADD(card, axis_web_storage_text("help", text));
	if (waiting == 0)
		return card;
	form = axis_web_auth_post_form(request, href);
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	profile_field = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(profile_field), "field");
	profile_label = htmx_label_new_for("identify-profile");
	htmx_node_set_text_content(HTMX_NODE(profile_label), "Quality profile");
	profile = htmx_select_new_with_name("profile");
	htmx_element_set_id(HTMX_ELEMENT(profile), "identify-profile");
	for (i = 0; profiles != NULL && i < profiles->len; i++) {
		const AxisQualityStored *stored = g_ptr_array_index(profiles, i);
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, stored->id);
		HtmxOption *option;

		if (!stored->usable)
			continue;
		option = htmx_option_new_with_value(value, stored->name);
		htmx_option_set_selected(option, g_strcmp0(stored->slug, default_slug) == 0);
		ADD(profile, option);
	}
	ADD(profile_field, profile_label);
	ADD(profile_field, profile);
	ADD(form, profile_field);
	watch_field = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(watch_field), "field");
	htmx_element_add_class(HTMX_ELEMENT(watch_field), "field-wide");
	watch_label = htmx_label_new_for("identify-monitored");
	htmx_node_set_text_content(HTMX_NODE(watch_label), "Monitoring");
	watch = htmx_select_new_with_name("monitored");
	htmx_element_set_id(HTMX_ELEMENT(watch), "identify-monitored");
	if (series) {
		HtmxOption *future = htmx_option_new_with_value("yes", "Future episodes only");

		htmx_option_set_selected(future, TRUE);
		ADD(watch, future);
		ADD(watch, htmx_option_new_with_value("no", "Nothing yet"));
	} else {
		ADD(watch, htmx_option_new_with_value("no", "Not monitored"));
		ADD(watch, htmx_option_new_with_value("yes", "Monitored for upgrades"));
	}
	ADD(watch_field, watch_label);
	ADD(watch_field, watch);
	ADD(form, watch_field);
	if (series)
		ADD(card, axis_web_storage_text("help", "Future episodes only: each new episode is searched for once it has "
			"aired; the episodes already aired are not monitored, so nothing on disk is upgraded and nothing missing "
			"is searched for. Nothing yet: no episode is monitored and nothing is searched for. Either way, the episodes "
			"on disk are in the library. Each series can be changed on its own page afterwards."));
	else
		ADD(card, axis_web_storage_text("help", "Not monitored: they are shown as in the library and nothing is ever "
			"searched for them. Monitored: a movie below its profile's cutoff is searched for an upgrade later, by the "
			"scheduler; a file whose name states no source is upgraded only if the profile says so."));
	button = series ? g_strdup_printf("Manage %u series", waiting) :
		g_strdup_printf("Manage %u %s", waiting, waiting == 1 ? "movie" : "movies");
	ADD(form, axis_web_auth_button(button, "button-primary"));
	ADD(card, form);
	return card;
}

/* A list's card, "list-<list>": the place to land when the list is empty. */
static HtmxNode *
list_card(
	const gchar *title,
	const Back  *back,
	NoticePlace *place
){
	HtmxNode *card = axis_web_storage_card(title);
	g_autofree gchar *id = g_strdup_printf("list-%s", back->list);
	HtmxNode *notice = notice_take(place, back->list, 0);

	htmx_element_set_id(HTMX_ELEMENT(card), id);
	if (notice != NULL)
		ADD(card, notice);
	return card;
}

static HtmxNode *
review_card(
	HtmxRequest *request,
	const gchar *title,
	const gchar *empty,
	GPtrArray   *units,
	guint        total,
	const gchar *all_href,
	const gchar *all_label,
	const Back  *back,
	NoticePlace *place
){
	HtmxNode *card = list_card(title, back, place);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(card), "identify-review");
	if (units == NULL || units->len == 0) {
		ADD(card, axis_web_storage_text("help", empty));
		return card;
	}
	for (i = 0; i < units->len; i++) {
		const AxisIdentifyUnitInfo *unit = g_ptr_array_index(units, i);
		HtmxNode *notice = notice_take(place, back->list, unit->id);

		if (notice != NULL)
			ADD(card, notice);
		ADD(card, unit_block(request, unit, back));
	}
	if (total > units->len && all_href != NULL)
		ADD(card, axis_web_browser_link(all_href, all_label));
	return card;
}

/* Whether @units holds @unit_id. */
static gboolean
units_have(
	GPtrArray *units,
	gint64     unit_id
){
	guint i;

	for (i = 0; units != NULL && i < units->len; i++) {
		if (((const AxisIdentifyUnitInfo *)g_ptr_array_index(units, i))->id == unit_id)
			return TRUE;
	}
	return FALSE;
}

/* @at ("unknown-12", "list-unknown") as a list's name and a unit (0: the
 * list's heading); FALSE when it names no list. */
static gboolean
place_parse(
	const gchar  *at,
	const gchar **list,
	gint64       *unit_id
){
	const gchar *dash;
	g_autofree gchar *name = NULL;
	guint64 value = 0;

	*list = NULL;
	*unit_id = 0;
	if (at == NULL)
		return FALSE;
	if (g_str_has_prefix(at, "list-")) {
		*list = list_known(at + strlen("list-"));
		return *list != NULL;
	}
	dash = strrchr(at, '-');
	if (dash == NULL || !g_ascii_string_to_unsigned(dash + 1, 10, 1, G_MAXINT64, &value, NULL))
		return FALSE;
	name = g_strndup(at, (gsize)(dash - at));
	*list = list_known(name);
	*unit_id = (gint64)value;
	return *list != NULL;
}

static HtmxResponse *
root_done(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer        data
){
	RootWork *work = data;
	const AxisIdentifyRootInfo *root = work->root;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *base = NULL;
	gboolean admin = axis_web_request_is_admin(request);
	NoticePlace place = { NULL, 0, NULL };
	Back page = { "root", NULL, 0 };
	HtmxNode *card;
	AxisIdentifyList one = AXIS_IDENTIFY_LIST_LIKELY;
	gboolean whole;

	if (g_error_matches(work->error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such library folder of movies or TV.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	base = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, root->root_id);
	whole = work->list != NULL && list_from_name(work->list, &one);
	content = htmx_div_new();
	{
		HtmxHeading *h1 = htmx_h1_new();
		HtmxBdi *bdi = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(bdi), root->name);
		ADD(h1, bdi);
		ADD(content, h1);
	}

	/* The notice of what was just done goes where the action sent the
	 * person: before the card that followed the one acted on, or under its
	 * list's heading; at the top when the page names no place in a list it
	 * shows. */
	if (place_parse(work->at, &place.list, &place.unit) && whole && strcmp(place.list, list_names[one]) != 0)
		place.list = NULL;
	if (place.list != NULL) {
		GPtrArray **into = NULL;
		guint *count = NULL;
		AxisIdentifyList which;

		list_from_name(place.list, &which);
		list_slot(work, which, &into, &count);
		if (place.unit > 0 && !units_have(*into, place.unit))
			place.unit = 0;
	}
	if (whole) {
		page.back = "list";
		page.list = list_names[one];
		page.from = work->from;
	} else {
		page.list = place.list;
	}
	if (work->acted != NULL)
		place.node = acted_notice(request, work->done, work->acted, &page);
	if (place.node == NULL && done_message(work->done) != NULL)
		place.node = axis_web_auth_notice(FALSE, done_message(work->done));
	if (place.node != NULL && place.list == NULL)
		ADD(content, g_steal_pointer(&place.node));

	/* One whole list, paged. */
	if (whole) {
		GPtrArray **into = NULL;
		guint *count = NULL;
		GPtrArray *units;
		guint total;
		g_autofree gchar *next = NULL;
		g_autofree gchar *first = NULL;
		const gchar *label = one == AXIS_IDENTIFY_LIST_LIKELY ? "Likely: confirm or choose" :
			one == AXIS_IDENTIFY_LIST_UNKNOWN ? "Unknown: search or mark" : one == AXIS_IDENTIFY_LIST_LINKED ? "Linked" :
			one == AXIS_IDENTIFY_LIST_WAITING ? "Waiting for the details provider" :
			one == AXIS_IDENTIFY_LIST_DOUBT ? "Linked, now uncertain" :
			root->kind == AXIS_META_KIND_SERIES ? "Marked as not a series" : "Marked as not a movie";
		Back back = { "list", NULL, 0 };

		back.list = list_names[one];
		back.from = work->from;
		list_slot(work, one, &into, &count);
		units = *into;
		total = *count;
		if (work->from + AXIS_WEB_IDENTIFY_PAGE < total)
			next = g_strdup_printf("%s?list=%s&from=%u", base, work->list, work->from + AXIS_WEB_IDENTIFY_PAGE);
		if (work->from > 0)
			first = g_strdup_printf("%s?list=%s", base, work->list);

		if (one == AXIS_IDENTIFY_LIST_LINKED || one == AXIS_IDENTIFY_LIST_NOT_FILM) {
			card = list_card(label, &back, &place);
			if (units == NULL || units->len == 0)
				ADD(card, axis_web_storage_text("help", "None."));
			else
				ADD(card, one == AXIS_IDENTIFY_LIST_LINKED ? linked_table(request, units, &back, &place) :
					not_film_table(request, units, &back, &place));
		} else {
			card = review_card(request, label, "Nothing waits here.", units, 0, NULL, NULL, &back, &place);
		}
		if (next != NULL || first != NULL)
			ADD(card, axis_web_browser_pager(first, next));
		ADD(content, card);
		{
			g_autofree gchar *back_label = g_strdup_printf("Back to the library folder %s", root->name);

			ADD(content, axis_web_browser_link(base, back_label));
		}
		g_clear_object(&place.node);
		return axis_web_page_respond(request, root->name, HTMX_NODE(content));
	}

	card = axis_web_storage_card("Progress in this library folder");
	ADD(card, counts_row(root, TRUE));
	ADD(card, progress_block(root));
	if (admin)
		ADD(card, axis_web_identify_run_form(request, root->root_id, NULL));
	ADD(content, card);
	if (admin && root->titles > 0)
		ADD(content, add_card(request, root, work->profiles, work->default_slug));
	{
		g_autofree gchar *all_likely = g_strdup_printf("%s?list=likely", base);
		g_autofree gchar *all_unknown = g_strdup_printf("%s?list=unknown", base);
		g_autofree gchar *likely_label = g_strdup_printf("All %u likely matches", work->n_likely);
		g_autofree gchar *unknown_label = g_strdup_printf("All %u unknown folders and files", work->n_unknown);
		Back likely = { "root", "likely", 0 };
		Back unknown = { "root", "unknown", 0 };

		ADD(content, review_card(request, "Likely: confirm or choose", "Nothing likely waits for a person.",
			work->likely, work->n_likely, all_likely, likely_label, &likely, &place));
		ADD(content, review_card(request, "Unknown: search or mark", "Nothing unknown waits for a person.",
			work->unknown, work->n_unknown, all_unknown, unknown_label, &unknown, &place));
	}
	/* Links a later look did not confirm: they stand until a person keeps,
	 * changes or undoes them.  Shown empty only to hold the notice of the
	 * last one's decision. */
	if ((work->doubted != NULL && work->doubted->len > 0) || (place.node != NULL && g_strcmp0(place.list, "doubt") == 0)) {
		g_autofree gchar *all = g_strdup_printf("%s?list=doubt", base);
		g_autofree gchar *label = g_strdup_printf("All %u uncertain links",work->n_doubted);
		Back doubt = { "root", "doubt", 0 };

		card = review_card(request, "Linked, now uncertain", "Nothing is uncertain now.", work->doubted,
			work->n_doubted, all, label, &doubt, &place);
		ADD(card, axis_web_storage_text("help", "These are linked, and when Axis looked at them again the evidence no "
			"longer made the link sure, or pointed to another title. Axis never changes a link by itself: keep it, choose "
			"another title, or undo it."));
		ADD(content, card);
	}
	/* What could not be told yet: the provider did not answer for a full
	 * record a folder's decision needs.  Axis looks at each again by itself. */
	if ((work->waiting != NULL && work->waiting->len > 0) ||
	    (place.node != NULL && g_strcmp0(place.list, "waiting") == 0)) {
		g_autofree gchar *all = g_strdup_printf("%s?list=waiting", base);
		g_autofree gchar *label = g_strdup_printf("All %u waiting for the details provider", work->n_waiting);
		Back waiting = { "root", "waiting", 0 };

		card = review_card(request, "Waiting for the details provider", "Nothing waits for the details provider now.",
			work->waiting, work->n_waiting, all, label, &waiting, &place);
		ADD(card, axis_web_storage_text("help", "The details provider did not answer for a title these may be, so "
			"nothing is decided for them yet. Axis looks at each again by itself, after a wait that grows each time "
			"(up to a day), or at once when you select Identify new and changed files now."));
		ADD(content, card);
	}
	{
		Back linked = { "root", "linked", 0 };

		card = list_card("Linked lately", &linked, &place);
		if (work->linked == NULL || work->linked->len == 0) {
			ADD(card, axis_web_storage_text("help", "Nothing is linked yet."));
		} else {
			ADD(card, linked_table(request, work->linked, &linked, &place));
			if (work->n_linked > work->linked->len) {
				g_autofree gchar *all = g_strdup_printf("%s?list=linked", base);
				g_autofree gchar *label = g_strdup_printf("All %u linked folders and files", work->n_linked);

				ADD(card, axis_web_browser_link(all, label));
			}
		}
		ADD(content, card);
	}
	if ((work->not_film != NULL && work->not_film->len > 0) ||
	    (place.node != NULL && g_strcmp0(place.list, "not_film") == 0)) {
		Back not_film = { "root", "not_film", 0 };

		card = list_card(root->kind == AXIS_META_KIND_SERIES ? "Marked as not a series" : "Marked as not a movie",
			&not_film, &place);
		if (work->not_film == NULL || work->not_film->len == 0)
			ADD(card, axis_web_storage_text("help", "None."));
		else
			ADD(card, not_film_table(request, work->not_film, &not_film, &place));
		ADD(content, card);
	}
	ADD(content, axis_web_browser_link("/identify", "Back to Identify"));
	g_clear_object(&place.node);
	return axis_web_page_respond(request, root->name, HTMX_NODE(content));
}

static HtmxResponse *
handle_root(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	RootWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *list = htmx_request_get_query_param(request, "list");
	const gchar *from = htmx_request_get_query_param(request, "from");
	const gchar *acted = htmx_request_get_query_param(request, "unit");
	AxisIdentifyList one;
	guint64 value = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such library folder of movies or TV.");
	work = g_new0(RootWork, 1);
	work->root_id = id;
	if (list != NULL && list_from_name(list, &one))
		work->list = g_strdup(list);
	if (from != NULL && g_ascii_string_to_unsigned(from, 10, 0, 1000000, &value, NULL))
		work->from = (guint)value;
	work->done = g_strdup(htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE));
	work->at = g_strdup(axis_web_place_of(request));
	if (acted != NULL && g_ascii_string_to_unsigned(acted, 10, 1, G_MAXINT64, &value, NULL))
		work->acted_id = (gint64)value;
	return axis_web_identify_dispatch(user_data, request, root_run, root_done, work, root_work_free);
}

/* ── One unit ───────────────────────────────────────────────────── */

typedef struct
{
	gint64                id;
	gchar                *done;
	gchar                *query;
	gint                  year;
	gboolean              find;	/* no query: search for what the names read as */
	Back                  back;	/* where its actions come back to: the list it was opened from, or itself */
	AxisIdentifyUnitInfo *unit;
	GPtrArray            *found;	/* AxisIdentifyCandidateInfo */
	gchar                *problem;
	GError               *error;
} UnitWork;

static void
unit_work_free(gpointer data)
{
	UnitWork *work = data;

	g_free(work->done);
	g_free(work->query);
	axis_identify_unit_info_free(work->unit);
	g_clear_pointer(&work->found, g_ptr_array_unref);
	g_free(work->problem);
	g_clear_error(&work->error);
	g_free(work);
}

static void
found_free(AxisIdentifyCandidateInfo *c)
{
	axis_meta_title_free(c->title);
	g_free(c->poster);
	g_free(c->why);
	g_free(c);
}

static void
unit_run(
	AxisWebIdentify *self,
	gpointer         data
){
	UnitWork *work = data;
	AxisMeta *meta = axis_identify_get_meta(self->identify);
	AxisMetaSearchAnswer answer;
	const AxisMetaProviderInfo *provider = NULL;
	g_autoptr(GError) local = NULL;
	AxisMetaKind kind;
	guint i;

	if (!axis_identify_get_unit(self->identify, work->id, &work->unit, &work->error))
		return;
	if (work->query == NULL && work->find && work->unit->read_title != NULL) {
		g_autofree gchar *words = axis_identify_search_words(work->unit->kind, work->unit->read_title);

		work->query = axis_meta_query_clean(words);
		work->year = work->unit->read_year;
	}
	if (work->query == NULL)
		return;
	/* A series' folder is looked for among series, a film's among films. */
	kind = work->unit->kind == AXIS_META_KIND_SERIES ? AXIS_META_KIND_SERIES : AXIS_META_KIND_FILM;
	for (i = 0; i < axis_meta_n_providers(meta) && provider == NULL; i++) {
		if ((axis_meta_provider_at(meta, i)->kinds & AXIS_META_KIND_BIT(kind)) != 0 &&
		    !axis_meta_provider_at(meta, i)->secondary)
			provider = axis_meta_provider_at(meta, i);
	}
	if (provider == NULL) {
		work->problem = g_strdup(kind == AXIS_META_KIND_SERIES ? "No details provider serves series: add the TMDb token under Settings." :
			"No details provider serves movies: add the TMDb token under Settings.");
		return;
	}
	memset(&answer, 0, sizeof(answer));
	if (!axis_meta_search(meta, provider->name, kind, work->query, work->year, AXIS_META_DEFAULT_WAIT_MS,
	        &answer, &local)) {
		if (local->domain != AXIS_META_ERROR)
			work->error = g_steal_pointer(&local);
		else
			work->problem = g_strdup("Nothing to look for.");
		return;
	}
	work->found = g_ptr_array_new_with_free_func((GDestroyNotify)found_free);
	if (answer.saved && answer.fetched_us == 0)
		work->problem = axis_web_meta_why_text(provider->label, answer.problem);
	for (i = 0; answer.titles != NULL && i < answer.titles->len && work->found->len < 12; i++) {
		AxisMetaTitle *title = g_ptr_array_index(answer.titles, i);
		AxisIdentifyCandidateInfo *c;

		if (title->kind != kind || title->id <= 0)
			continue;
		c = g_new0(AxisIdentifyCandidateInfo, 1);
		c->title = g_ptr_array_steal_index(answer.titles, i);
		i--;
		if (c->title->provider != NULL && c->title->poster_ref != NULL)
			c->poster = axis_meta_artwork_find(meta, c->title->provider, c->title->poster_ref, AXIS_META_ART_POSTER_SMALL,
				FALSE);
		g_ptr_array_add(work->found, c);
	}
	axis_meta_search_answer_clear(&answer);
}

static HtmxNode *
search_form(
	const AxisIdentifyUnitInfo *unit,
	const gchar                *query,
	gint                        year,
	const Back                 *back
){
	g_autofree gchar *href = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT, unit->id);
	g_autofree gchar *year_text = year > 0 ? g_strdup_printf("%d", year) : unit->read_year > 0 ?
		g_strdup_printf("%d", unit->read_year) : NULL;
	g_autofree gchar *words = axis_identify_search_words(unit->kind, unit->read_title);
	HtmxForm *form = htmx_form_new();
	HtmxButton *button = htmx_button_new_submit();

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", href);
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "role", "search");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	/* Searching again keeps the list it came from. */
	if (back_carried(back))
		back_fields(HTMX_NODE(form), back);
	ADD(form, axis_web_storage_text_field("identify-q", "Title", "q", query != NULL ? query : words, NULL, NULL));
	ADD(form, axis_web_storage_text_field("identify-year", "Year", "year", year_text, NULL, NULL));
	htmx_node_set_text_content(HTMX_NODE(button), "Search");
	ADD(form, button);
	return HTMX_NODE(form);
}

static HtmxResponse *
unit_done(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer         data
){
	UnitWork *work = data;
	const AxisIdentifyUnitInfo *unit = work->unit;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *root_href = NULL;
	const gchar *notice = done_message(work->done);
	gboolean admin = axis_web_request_is_admin(request);
	gboolean series;
	HtmxNode *facts;
	HtmxNode *card;
	HtmxDiv *line;
	guint i;

	if (g_error_matches(work->error, AXIS_IDENTIFY_ERROR, AXIS_IDENTIFY_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such folder or file to identify.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	series = unit->kind == AXIS_META_KIND_SERIES;
	base = base_of(unit->rel_path);
	root_href = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT, unit->root_id);
	content = htmx_div_new();
	{
		HtmxHeading *h1 = htmx_h1_new();

		ADD(h1, axis_web_browser_name_node(base));
		ADD(content, h1);
	}
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(FALSE, notice));
	line = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(line), "film-state-line");
	htmx_element_add_class(HTMX_ELEMENT(line), "import-state");
	ADD(line, axis_web_identify_badge(unit));
	if (g_strcmp0(unit->state, "linked") == 0 && unit->title_label != NULL) {
		g_autofree gchar *title = g_strdup_printf("/title/%" G_GINT64_FORMAT, unit->title_id);

		ADD(line, text_link(title, unit->title_label));
	}
	ADD(content, line);

	card = axis_web_storage_card("About this folder or file");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "Library folder", text_link(root_href, unit->root_name));
	axis_web_browser_fact_node(facts, "Path", axis_web_browser_name_node(unit->rel_path));
	axis_web_browser_fact(facts, "Kind", g_strcmp0(unit->unit_kind, "folder") == 0 ? (series ? "A series' folder" :
		"A movie folder") : series ? "An episode file at the top of the library folder" : "A file standing alone");
	if (unit->read_text != NULL)
		axis_web_browser_fact(facts, "Read from the names", unit->read_text);
	{
		g_autofree gchar *runs = minutes_text(unit->duration_ms);

		axis_web_browser_fact(facts, "Runtime", unit->duration_ms > 0 ? runs : "not read yet");
	}
	if (unit->confidence != NULL)
		axis_web_browser_fact(facts, "The evidence says", g_strcmp0(unit->confidence, "sure") == 0 ? "sure" :
			g_strcmp0(unit->confidence, "likely") == 0 ? "likely" : "unknown");
	if (unit->decided_by != NULL) {
		g_autofree gchar *when = axis_web_format_time(unit->decided_us, "");
		g_autofree gchar *by = g_strdup_printf("%s, %s", unit->decided_by, when);

		axis_web_browser_fact(facts, "Decided by", by);
	}
	if (g_strcmp0(unit->state, "linked") == 0 && unit->title_id > 0 && series)
		axis_web_browser_fact(facts, "Managed by Axis", unit->managed ? "yes: its episodes on disk are in the library" :
			"no: it is listed as On disk, not managed by Axis; manage it from its page, or every identified "
			"series of the library folder at once");
	else if (g_strcmp0(unit->state, "linked") == 0 && unit->title_id > 0)
		axis_web_browser_fact(facts, "Managed by Axis", unit->managed ? "yes: it is in the library" :
			"no: it is listed as On disk, not managed by Axis; manage it from its page, or every identified movie of "
			"the library folder at once");
	{
		g_autofree gchar *when = axis_web_format_time(unit->examined_us, "");

		axis_web_browser_fact(facts, "Looked at", when);
	}
	ADD(card, facts);
	ADD(content, card);

	card = axis_web_storage_card("Reasons for this decision");
	if (unit->doubt != NULL)
		ADD(card, doubt_line(unit));
	ADD(card, reasons_list(unit));
	if (g_strcmp0(unit->state, "review") == 0 || g_strcmp0(unit->state, "waiting") == 0)
		ADD(card, settle_line(unit));
	ADD(content, card);

	card = axis_web_storage_card("Videos");
	ADD(card, axis_web_identify_files_table(unit, admin && g_strcmp0(unit->state, "linked") != 0));
	ADD(content, card);

	if (unit->candidates != NULL && unit->candidates->len > 0) {
		card = axis_web_storage_card(g_strcmp0(unit->state, "linked") == 0 ? "The possible matches it had" : "Possible matches");
		ADD(card, pick_list(request, unit, unit->candidates, FALSE, &work->back));
		ADD(content, card);
	}
	if (admin && g_strcmp0(unit->linked_how, "imported") != 0) {
		card = axis_web_storage_card(series ? "Search for the series" : "Search for the movie");
		htmx_element_set_id(HTMX_ELEMENT(card), "search");
		ADD(card, search_form(unit, work->query, work->year, &work->back));
		if (work->problem != NULL)
			ADD(card, axis_web_storage_text("notice notice-warning", work->problem));
		if (work->found != NULL && work->found->len > 0)
			ADD(card, pick_list(request, unit, work->found, TRUE, &work->back));
		else if (work->found != NULL && work->problem == NULL)
			ADD(card, axis_web_storage_text("help", series ? "TMDb knows no series of that title: try another spelling, or clear the year." :
				"TMDb knows no movie of that title: try another spelling, or clear the year."));
		ADD(content, card);

		card = axis_web_storage_card("Change what Axis decided");
		{
			HtmxDiv *row = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(row), "identify-actions");
			if (unit->doubt != NULL && unit->title_id > 0)
				ADD(row, act_form(request, unit->id, "choose", unit->title_id, &work->back, "Keep this link",
					"button-primary", TRUE));
			if (g_strcmp0(unit->state, "linked") == 0 || g_strcmp0(unit->state, "not_film") == 0)
				ADD(row, act_form(request, unit->id, "undo", 0, &work->back, g_strcmp0(unit->state, "linked") == 0 ?
					"Undo the link" : series ? "Undo \"not a series\"" : "Undo \"not a movie\"", NULL, TRUE));
			if (g_strcmp0(unit->state, "not_film") != 0)
				ADD(row, act_form(request, unit->id, "not_film", 0, &work->back, series ? "Mark as not a series" :
					"Mark as not a movie", NULL, TRUE));
			ADD(card, row);
		}
		ADD(card, axis_web_storage_text("help", "Every action is recorded with your name. Nothing on disk is changed."));
		ADD(content, card);
	}
	if (unit->actions != NULL && unit->actions->len > 0) {
		HtmxUl *list = htmx_ul_new();

		card = axis_web_storage_card("What people did");
		htmx_element_add_class(HTMX_ELEMENT(list), "release-reasons");
		for (i = 0; i < unit->actions->len; i++) {
			const AxisIdentifyActionInfo *action = g_ptr_array_index(unit->actions, i);
			g_autofree gchar *when = axis_web_format_time(action->created_us, "");
			g_autofree gchar *text = g_strdup_printf("%s, %s: %s.", when, action->by_user, action->text != NULL ?
				action->text : action->action);
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), text);
			ADD(list, item);
		}
		ADD(card, list);
		ADD(content, card);
	}
	/* Opened from a list: back to its card there. */
	if (back_carried(&work->back)) {
		g_autofree gchar *id = card_id(work->back.list, unit->id);
		g_autofree gchar *href = strcmp(work->back.back, "list") != 0 ? g_strdup_printf("%s#%s", root_href, id) :
			work->back.from > 0 ? g_strdup_printf("%s?list=%s&from=%u#%s", root_href, work->back.list, work->back.from,
			id) : g_strdup_printf("%s?list=%s#%s", root_href, work->back.list, id);

		ADD(content, axis_web_browser_link(href, "Back to the list"));
	} else {
		g_autofree gchar *back_label = g_strdup_printf("Back to the library folder %s", unit->root_name);

		ADD(content, axis_web_browser_link(root_href, back_label));
	}
	return axis_web_page_respond(request, base, HTMX_NODE(content));
}

static HtmxResponse *
handle_unit(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	UnitWork *work;
	gint64 id = axis_web_storage_param_id(params);
	g_autofree gchar *query = axis_web_meta_param(request, "q", 300);
	g_autofree gchar *year = axis_web_meta_param(request, "year", 8);
	guint64 value = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such folder or file to identify.");
	work = g_new0(UnitWork, 1);
	work->id = id;
	work->done = g_strdup(htmx_request_get_query_param(request, "done"));
	work->back = back_read(htmx_request_get_query_param(request, "back"), htmx_request_get_query_param(request, "list"),
		htmx_request_get_query_param(request, "from"));
	if (axis_web_request_is_admin(request) && query != NULL) {
		work->query = axis_meta_query_clean(query);
		if (year != NULL && g_ascii_string_to_unsigned(year, 10, AXIS_META_YEAR_MIN, AXIS_META_YEAR_MAX, &value, NULL))
			work->year = (gint)value;
	} else if (axis_web_request_is_admin(request)) {
		work->find = g_strcmp0(htmx_request_get_query_param(request, "find"), "1") == 0;
	}
	return axis_web_identify_dispatch(user_data, request, unit_run, unit_done, work, unit_work_free);
}

/* ── Acting ─────────────────────────────────────────────────────── */

typedef struct
{
	gint64              id;
	AxisIdentifyAction  action;
	gint64              title_id;
	Back                back;
	gboolean            land_self;	/* back to the unit's own card (the notice's Undo) */
	gchar              *user;
	gint64              root_id;
	gint64              linked_title;
	gint64              next_id;	/* the card after it in back.list, before the action */
	gint64              previous_id;
	guint               position;	/* how many of back.list came before it */
	const gchar        *now_list;	/* (static) the list it is in after the action */
	GError             *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

/* The list of a root's page @unit is in now. */
static const gchar *
list_of(const AxisIdentifyUnitInfo *unit)
{
	if (g_strcmp0(unit->state, "review") == 0)
		return g_strcmp0(unit->confidence, "sure") == 0 || g_strcmp0(unit->confidence, "likely") == 0 ?
			list_names[AXIS_IDENTIFY_LIST_LIKELY] : list_names[AXIS_IDENTIFY_LIST_UNKNOWN];
	if (g_strcmp0(unit->state, "linked") == 0)
		return list_names[unit->doubt != NULL ? AXIS_IDENTIFY_LIST_DOUBT : AXIS_IDENTIFY_LIST_LINKED];
	if (g_strcmp0(unit->state, "not_film") == 0)
		return list_names[AXIS_IDENTIFY_LIST_NOT_FILM];
	return list_names[AXIS_IDENTIFY_LIST_WAITING];
}

static void
act_run(
	AxisWebIdentify *self,
	gpointer         data
){
	ActWork *work = data;
	g_autoptr(AxisIdentifyUnitInfo) unit = NULL;
	AxisIdentifyList list;

	/* Its neighbours in the list it was acted on from, before it leaves it. */
	if (work->back.list != NULL && list_from_name(work->back.list, &list) &&
	    !axis_identify_list_neighbours(self->identify, work->id, list, &work->next_id, &work->previous_id,
	        &work->position, &work->error))
		return;
	if (!axis_identify_act(self->identify, work->id, work->action, work->title_id, work->user, &work->error))
		return;
	if (axis_identify_get_unit(self->identify, work->id, &unit, NULL)) {
		work->root_id = unit->root_id;
		work->linked_title = unit->title_id;
		work->now_list = list_of(unit);
	}
}

/*
 * Where an action on a card of a list lands, in that list: the card that
 * followed it (it has left the list, so that card is where it was); the one
 * before it when it was the last; the list's heading when the list is
 * empty.  The notice's Undo lands at the unit's own card, back in the list,
 * or at the heading when the unit went back to another list.  On a page of
 * its own the list may have to show the page before: when the card was the
 * first of its page and the last of the list.
 */
static gchar *
act_place(
	ActWork *work,
	guint   *from
){
	const gchar *list = work->back.list;

	*from = work->back.from;
	if (work->land_self)
		return g_strcmp0(work->now_list, list) == 0 ? card_id(list, work->id) : g_strdup_printf("list-%s", list);
	if (work->next_id > 0)
		return card_id(list, work->next_id);
	if (work->previous_id > 0 && work->position > *from)
		return card_id(list, work->previous_id);
	if (work->previous_id > 0) {
		*from = *from >= AXIS_WEB_IDENTIFY_PAGE ? *from - AXIS_WEB_IDENTIFY_PAGE : 0;
		return card_id(list, work->previous_id);
	}
	*from = 0;
	return g_strdup_printf("list-%s", list);
}

static HtmxResponse *
act_done(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer         data
){
	ActWork *work = data;
	g_autofree gchar *location = NULL;
	const gchar *key = axis_identify_action_name(work->action);

	if (work->error != NULL) {
		g_autofree gchar *message = NULL;
		guint status;

		if (work->error->domain != AXIS_IDENTIFY_ERROR)
			return axis_web_internal_error(request, work->error);
		status = work->error->code == AXIS_IDENTIFY_ERROR_NOT_FOUND ? 404 :
			work->error->code == AXIS_IDENTIFY_ERROR_INVALID ? 409 : 503;
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, status, "Not done", message);
	}
	axis_web_storage_audit(request, "identify unit %" G_GINT64_FORMAT " %s", work->id, key);
	/* Back to the root's page or list it came from, at the place (with the
	 * unit, which the notice names); or to the unit's own page. */
	if (back_to_root(&work->back) && work->root_id > 0) {
		g_autoptr(GString) path = g_string_new(NULL);
		g_autofree gchar *place = NULL;
		guint from = work->back.from;

		if (work->back.list != NULL)
			place = act_place(work, &from);
		g_string_printf(path, "/identify/roots/%" G_GINT64_FORMAT, work->root_id);
		if (strcmp(work->back.back, "list") == 0) {
			g_string_append_printf(path, "?list=%s", work->back.list);
			if (from > 0)
				g_string_append_printf(path, "&from=%u", from);
		}
		g_string_append_printf(path, "%cunit=%" G_GINT64_FORMAT, strchr(path->str, '?') != NULL ? '&' : '?', work->id);
		return axis_web_place_redirect(request, path->str, key, place);
	}
	location = g_strdup_printf("/identify/units/%" G_GINT64_FORMAT "?done=%s", work->id, key);
	return axis_web_auth_redirect(request, location);
}

static gint64
form_id(
	HtmxRequest *request,
	const gchar *name
){
	guint64 value = 0;

	if (!axis_web_storage_form_u64(request, name, &value) || value == 0 || value > (guint64)G_MAXINT64)
		return 0;
	return (gint64)value;
}

static HtmxResponse *
handle_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ActWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *action = htmx_request_get_form_value(request, "action");
	const gchar *back = htmx_request_get_form_value(request, "back");
	AxisIdentifyAction which;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such folder or file to identify.");
	if (!axis_identify_action_from_name(action, &which))
		return axis_web_error_respond(request, 400, "Not done", "Choose what to do.");
	work = g_new0(ActWork, 1);
	work->id = id;
	work->action = which;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->back = back_read(back, htmx_request_get_form_value(request, "list"),
		htmx_request_get_form_value(request, "from"));
	work->land_self = g_strcmp0(htmx_request_get_form_value(request, "land"), "self") == 0;
	if (which == AXIS_IDENTIFY_CHOOSE) {
		work->title_id = form_id(request, "title");
		if (work->title_id <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Choose the title.");
		}
	}
	if (work->user == NULL) {
		act_work_free(work);
		return axis_web_error_respond(request, 403, "Not done", "Log in as an administrator.");
	}
	return axis_web_identify_dispatch(user_data, request, act_run, act_done, work, act_work_free);
}

/* ── A root's run, and adding every identified film ─────────────── */

HtmxNode *
axis_web_identify_run_form(
	HtmxRequest *request,
	gint64       root_id,
	const gchar *return_path
){
	g_autofree gchar *action = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT "/run", root_id);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *back = axis_web_setup_return_field(return_path);

	htmx_element_add_class(HTMX_ELEMENT(form), "film-button");
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_button("Identify new and changed files now", NULL));
	return form;
}

typedef struct
{
	gint64       root_id;
	gboolean     add;
	gint64       profile_id;
	gboolean     monitored;
	gchar       *user;
	guint        added;
	guint        skipped;
	gboolean     series;	/* the root is one for TV */
	const gchar *back;	/* the setup guide's address (static), or NULL */
	GError      *error;
} RootActWork;

static void
root_act_free(gpointer data)
{
	RootActWork *work = data;

	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
root_act_run(
	AxisWebIdentify *self,
	gpointer         data
){
	RootActWork *work = data;
	AxisIdentifyRootInfo *root = NULL;

	if (work->add && axis_identify_get_root(self->identify, work->root_id, &root, NULL)) {
		work->series = root->kind == AXIS_META_KIND_SERIES;
		axis_identify_root_info_free(root);
	}
	if (work->add)
		axis_identify_add_all(self->identify, work->root_id, work->profile_id, work->monitored, work->user, &work->added,
			&work->skipped, &work->error);
	else
		axis_identify_request(self->identify, work->root_id, NULL, &work->error);
}

static HtmxResponse *
root_act_done(
	AxisWebIdentify *self,
	HtmxRequest     *request,
	gpointer         data
){
	RootActWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL) {
		g_autofree gchar *message = NULL;
		guint status = 409;

		if (work->error->domain == AXIS_IDENTIFY_ERROR)
			status = work->error->code == AXIS_IDENTIFY_ERROR_NOT_FOUND ? 404 : 503;
		else if (work->error->domain == AXIS_DB_ERROR || work->error->domain == G_IO_ERROR)
			return axis_web_internal_error(request, work->error);
		else
			status = 400;	/* the owner's: a profile or root it refused */
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, status, work->add ? "Not added" : "Not queued", message);
	}
	if (work->add)
		axis_web_storage_audit(request, "identify root %" G_GINT64_FORMAT " add all: %u added, %u managed already",
			work->root_id, work->added, work->skipped);
	else
		axis_web_storage_audit(request, "identify root %" G_GINT64_FORMAT " look again", work->root_id);
	if (work->back != NULL)
		location = axis_web_setup_return_location(work->back, work->add ? "added" : "run");
	else
		location = g_strdup_printf("/identify/roots/%" G_GINT64_FORMAT "?done=%s", work->root_id, work->add ?
			(work->series ? "added_series" : "added") : "run");
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
start_root_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     add
){
	RootActWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *monitored = htmx_request_get_form_value(request, "monitored");

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such library folder of movies or TV.");
	work = g_new0(RootActWork, 1);
	work->root_id = id;
	work->add = add;
	work->back = axis_web_setup_request_return(request);
	work->user = g_strdup(axis_web_request_get_username(request));
	if (add) {
		work->profile_id = form_id(request, "profile");
		if (g_strcmp0(monitored, "yes") != 0 && g_strcmp0(monitored, "no") != 0) {
			root_act_free(work);
			return axis_web_error_respond(request, 400, "Not added", "Choose what is monitored.");
		}
		work->monitored = g_strcmp0(monitored, "yes") == 0;
	}
	if (work->user == NULL) {
		root_act_free(work);
		return axis_web_error_respond(request, 403, "Not done", "Log in as an administrator.");
	}
	return axis_web_identify_dispatch(user_data, request, root_act_run, root_act_done, work, root_act_free);
}

static HtmxResponse *
handle_run(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_root_act(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_root_act(request, params, user_data, TRUE);
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_web_identify_register_pages(
	AxisWebIdentify *self,
	AxisWeb         *web
){
	axis_web_get(web, "/identify", handle_roots, self);
	axis_web_get(web, "/identify/roots/:id", handle_root, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/identify/roots/:id/run", AXIS_WEB_ROUTE_ADMIN, handle_run, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/identify/roots/:id/add", AXIS_WEB_ROUTE_ADMIN, handle_add, self);
	axis_web_get(web, "/identify/units/:id", handle_unit, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/identify/units/:id/act", AXIS_WEB_ROUTE_ADMIN, handle_act, self);
}
