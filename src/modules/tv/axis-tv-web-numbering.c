/*
 * axis-tv-web-numbering.c - changing a series' episode numbering: the
 * confirmation page and the change (add-metadata-tvdb, V3)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /series/:id/numbering?to=SOURCE   what following SOURCE would make
 *                                          of each episode; writes nothing
 *   POST /series/:id/numbering             to=, digest=, unmonitored=yes
 *
 * Both are an administrator's (AXIS_WEB_ROUTE_ADMIN, and the shell's CSRF
 * check for the post).  The page holds the plan's digest; a confirmation
 * made from a page of a series that changed since is refused and the page
 * drawn again, as is one while a grab, an import or a search of the series
 * is open (naming them).  Opening the page asks the other source's provider
 * for its episodes when they are not fresh (kept in the metadata store,
 * nothing of the series).  Everything a provider gave is set as text.
 */

#include "modules/tv/axis-tv-web.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add

typedef struct
{
	gint64              series_id;
	gchar              *to;
	gchar              *digest;	/* the post's */
	gboolean            confirm;
	gboolean            unmonitored;
	gboolean            names;	/* place files by their names after the change */
	gchar              *user;
	AxisTvSeries       *series;
	AxisTvRenumberPlan *plan;
	gboolean            changed;	/* the series changed since the page */
	gchar              *refused;	/* (nullable) why the post changed nothing */
	GError             *error;
} NumberingWork;

static void
numbering_work_free(gpointer data)
{
	NumberingWork *work = data;

	g_free(work->to);
	g_free(work->digest);
	g_free(work->user);
	axis_tv_series_free(work->series);
	axis_tv_renumber_plan_free(work->plan);
	g_free(work->refused);
	g_clear_error(&work->error);
	g_free(work);
}

static void
numbering_run(
	AxisTv   *self,
	gpointer  data
){
	NumberingWork *work = data;
	g_autoptr(GError) local = NULL;

	if (!axis_tv_get(self, work->series_id, &work->series, NULL, &work->error))
		return;
	if (!work->confirm) {
		axis_tv_plan_numbering(self, work->series_id, work->to, TRUE, &work->plan, &work->error);
		return;
	}
	if (axis_tv_change_numbering_full(self, work->series_id, work->to, work->digest, work->unmonitored, work->names,
	        work->user, &work->plan, &work->changed, &local)) {
		if (work->changed)
			work->refused = g_strdup("The series changed after this page was made (a file arrived, an episode was "
				"renumbered, or the other numbering was updated), so nothing was changed. Check the table again, "
				"then confirm.");
		return;
	}
	/* Refused: the page again, with why. */
	if (local->domain == AXIS_TV_ERROR && (local->code == AXIS_TV_ERROR_IN_FLIGHT ||
	    local->code == AXIS_TV_ERROR_INVALID) &&
	    axis_tv_plan_numbering(self, work->series_id, work->to, FALSE, &work->plan, NULL)) {
		work->refused = g_strdup_printf("%c%s.", g_ascii_toupper(local->message[0]), local->message + 1);
		return;
	}
	g_propagate_error(&work->error, g_steal_pointer(&local));
}

/* ── The page ───────────────────────────────────────────────────── */

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
cell_bdi(const gchar *text)
{
	HtmxBdi *bdi = htmx_bdi_new();

	htmx_node_set_text_content(HTMX_NODE(bdi), text != NULL ? text : "");
	return cell_node(HTMX_NODE(bdi));
}

/* "S02E03 "Harbour Masters", 2025-02-24". */
static gchar *
numbered(
	gint         season,
	gint         number,
	const gchar *title,
	const gchar *air_date
){
	g_autofree gchar *label = axis_tv_episode_label(season, number);
	GString *text = g_string_new(label);

	if (title != NULL && title[0] != '\0')
		g_string_append_printf(text, " \"%s\"", title);
	if (air_date != NULL)
		g_string_append_printf(text, ", %s", air_date);
	return g_string_free(text, FALSE);
}

static gchar *
now_text(const AxisTvRenumbered *row)
{
	if (row->how == AXIS_TV_MAPPED_RETURNS)
		return g_strdup("Kept from before");
	return axis_tv_episode_label(row->season, row->number);
}

static gchar *
after_text(
	const AxisTvRenumberPlan *plan,
	const AxisTvRenumbered   *row
){
	if (row->to_season < 0)
		return g_strdup_printf("None: kept, marked as lacking in %s, never searched for", axis_tv_source_label(plan->to));
	return numbered(row->to_season, row->to_number, row->to_title, row->to_air_date);
}

static gchar *
file_text(const AxisTvRenumbered *row)
{
	if (row->n_files == 0)
		return g_strdup(row->grab ? "Downloading" : "No file");
	if (row->n_files == 1)
		return g_strdup(row->file_path);
	return g_strdup_printf("%s and %u more", row->file_path, row->n_files - 1);
}

static const AxisWebColumn rows_columns[] = {
	{ "Numbering now", AXIS_WEB_COL_TEXT },
	{ "Numbering after", AXIS_WEB_COL_NAME },
	{ "File", AXIS_WEB_COL_WIDE },
	{ "How matched", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxNode *
rows_table(
	const AxisTvRenumberPlan *plan,
	gboolean                  with_files
){
	HtmxNode *table = axis_web_table_new(with_files ? "numbering-files" : "numbering-others", rows_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);
		g_autofree gchar *now = NULL;
		g_autofree gchar *after = NULL;
		g_autofree gchar *file = NULL;
		HtmxTr *tr;

		if ((row->n_files > 0 || row->grab) != with_files)
			continue;
		now = now_text(row);
		after = after_text(plan, row);
		file = file_text(row);
		tr = htmx_tr_new();
		if (axis_tv_mapped_flagged(row->how) || row->how == AXIS_TV_MAPPED_NONE)
			htmx_element_add_class(HTMX_ELEMENT(tr), "numbering-check");
		ADD(tr, cell_text(now));
		ADD(tr, cell_bdi(after));
		ADD(tr, cell_node(axis_web_breakable(file)));
		ADD(tr, cell_text(axis_tv_mapped_label(row->how)));
		ADD(body, tr);
	}
	ADD(table, body);
	return table;
}

static void
fact(
	HtmxDl      *list,
	const gchar *term,
	const gchar *value
){
	HtmxDt *dt = htmx_dt_new();
	HtmxDd *dd = htmx_dd_new();

	htmx_node_set_text_content(HTMX_NODE(dt), term);
	htmx_node_set_text_content(HTMX_NODE(dd), value);
	ADD(list, dt);
	ADD(list, dd);
}

/* What changes, in counts. */
static HtmxNode *
summary_card(const AxisTvRenumberPlan *plan)
{
	HtmxNode *card = axis_web_storage_card("What would change");
	HtmxDl *list = htmx_dl_new();
	g_autoptr(GString) how = g_string_new(NULL);
	g_autofree gchar *matched = NULL;
	g_autofree gchar *lacking = NULL;
	g_autofree gchar *added = NULL;
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *monitored = NULL;
	guint n_matched = 0;
	guint i;

	for (i = AXIS_TV_MAPPED_HELD; i <= AXIS_TV_MAPPED_FILE_NAME; i++) {
		static const gchar *const words[] = {
			NULL, "as before", "by air date and title", "by air date only, to check", "by title only, to check",
			"by a file's name, to check"
		};

		if (plan->n_by[i] == 0)
			continue;
		n_matched += plan->n_by[i];
		g_string_append_printf(how, "%s%u %s", how->len > 0 ? "; " : "", plan->n_by[i], words[i]);
	}
	matched = how->len > 0 ? g_strdup_printf("%u (%s)", n_matched, how->str) : g_strdup_printf("%u", n_matched);
	lacking = plan->n_by[AXIS_TV_MAPPED_NONE] == 0 ? g_strdup("None") :
		g_strdup_printf("%u, %u of them with files: kept with their files and history, never searched for",
			plan->n_by[AXIS_TV_MAPPED_NONE], plan->n_unmapped_files);
	added = plan->added->len == 0 ? g_strdup("None") :
		g_strdup_printf("%u, %u of them aired and wanted once added", plan->added->len, plan->n_new_wanted);
	wanted = g_strdup_printf("%u now, %u after", plan->wanted_before, plan->wanted_after);
	monitored = g_strdup_printf("%u now, %u after", plan->monitored_before, plan->monitored_after);
	htmx_element_add_class(HTMX_ELEMENT(list), "facts");
	fact(list, "Episodes matched", matched);
	if (plan->n_by[AXIS_TV_MAPPED_RETURNS] > 0) {
		g_autofree gchar *back = g_strdup_printf("%u, with their files and history", plan->n_by[AXIS_TV_MAPPED_RETURNS]);

		fact(list, "Episodes kept from before that come back", back);
	}
	{
		g_autofree gchar *term = g_strdup_printf("Episodes %s lacks", axis_tv_source_label(plan->to));

		fact(list, term, lacking);
	}
	{
		g_autofree gchar *term = g_strdup_printf("Episodes new in %s", axis_tv_source_label(plan->to));

		fact(list, term, added);
	}
	fact(list, "Wanted episodes", wanted);
	fact(list, "Monitored episodes", monitored);
	ADD(card, list);
	ADD(card, axis_web_storage_text("help", "\"As before\" is the episode a row was the last time the series followed "
		"that numbering, so changing back is exact. A match by air date only, by title only or by a file's name is "
		"one to check in the table."));
	return card;
}

static HtmxNode *
files_card(const AxisTvRenumberPlan *plan)
{
	HtmxNode *card = axis_web_storage_card("Episodes with files or downloads");
	guint n = 0;
	guint i;

	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		if (row->n_files > 0 || row->grab)
			n++;
	}
	if (n == 0) {
		ADD(card, axis_web_storage_text("help", "No episode of the series has a file or a download."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "Each file stays where it is on disk, with its name, linked to the episode "
		"it holds: only that episode is numbered anew. Files are named by the new numbering only when they are "
		"imported from now on."));
	ADD(card, rows_table(plan, TRUE));
	return card;
}

static const AxisWebColumn lacking_columns[] = {
	{ "Numbering now", AXIS_WEB_COL_TEXT },
	{ "File", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* Files whose episode has no counterpart. */
static HtmxNode *
lacking_card(const AxisTvRenumberPlan *plan)
{
	g_autofree gchar *heading = g_strdup_printf("Files on episodes %s lacks", axis_tv_source_label(plan->to));
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (plan->n_unmapped_files == 0)
		return NULL;
	card = axis_web_storage_card(heading);
	ADD(card, axis_web_storage_text("help", "Each of these files stays on disk where it is and stays linked to its "
		"episode, which is kept, marked as lacking, and never searched for. The series' page lists them."));
	table = axis_web_table_new("numbering-lacking", lacking_columns);
	body = htmx_tbody_new();
	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);
		g_autofree gchar *now = NULL;
		g_autofree gchar *file = NULL;
		HtmxTr *tr;

		if (row->to_season >= 0 || row->n_files == 0 || row->how == AXIS_TV_MAPPED_RETURNS)
			continue;
		now = now_text(row);
		file = file_text(row);
		tr = htmx_tr_new();
		ADD(tr, cell_text(now));
		ADD(tr, cell_node(axis_web_breakable(file)));
		ADD(body, tr);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const AxisWebColumn disagree_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Its name says", AXIS_WEB_COL_TEXT },
	{ "Episode after", AXIS_WEB_COL_TEXT },
	{ "If placed by its name", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

/* Files whose own name and new episode disagree (the names a later rename
 * would change). */
static HtmxNode *
disagree_card(const AxisTvRenumberPlan *plan)
{
	HtmxNode *card = NULL;
	HtmxNode *table = NULL;
	HtmxTbody *body = NULL;
	guint i;

	for (i = 0; i < plan->names->len; i++) {
		const AxisTvRenumberedFile *file = g_ptr_array_index(plan->names, i);
		g_autofree gchar *says = NULL;
		g_autofree gchar *after = NULL;
		HtmxTr *tr;

		if (card == NULL) {
			card = axis_web_storage_card("Files whose names and new episodes disagree");
			ADD(card, axis_web_storage_text("help", "Nothing is renamed: these files keep their names, and each stays on "
				"its episode unless you tick \"After the change, place these files by their names\" below; the last "
				"column says where that puts each. Only files identification linked, whose names were read under the "
				"numbering the series follows after the change, are placed: a name Axis wrote at import is probably "
				"just the old number. Only links move, never a file on disk."));
			table = axis_web_table_new("numbering-disagree", disagree_columns);
			body = htmx_tbody_new();
		}
		says = axis_tv_episode_label(file->name_season, file->name_number);
		after = axis_tv_episode_label(file->to_season, file->to_number);
		tr = htmx_tr_new();
		ADD(tr, cell_node(axis_web_breakable(file->path)));
		ADD(tr, cell_text(says));
		ADD(tr, cell_text(after));
		ADD(tr, cell_text(file->by_name != NULL ? file->by_name : "Stays where it is"));
		ADD(body, tr);
	}
	if (card == NULL)
		return NULL;
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const AxisWebColumn hand_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Episode now", AXIS_WEB_COL_TEXT },
	{ "Episode after", AXIS_WEB_COL_TEXT },
	{ "Link", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

/* The links a person made, in a group of their own (add-force-grab-and-
 * assign): each stays on its episode, and the tick box never moves one. */
static HtmxNode *
hand_card(const AxisTvRenumberPlan *plan)
{
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (plan->hand == NULL || plan->hand->len == 0)
		return NULL;
	card = axis_web_storage_card("Files linked by hand");
	ADD(card, axis_web_storage_text("help", "A person made each of these links. Each stays on the episode it is on, "
		"which is numbered anew like every other, and is never placed by its name, with or without the box below."));
	table = axis_web_table_new("numbering-hand", hand_columns);
	body = htmx_tbody_new();
	for (i = 0; i < plan->hand->len; i++) {
		const AxisTvRenumberedFile *file = g_ptr_array_index(plan->hand, i);
		g_autofree gchar *now = file->name_season >= 0 ? axis_tv_episode_label(file->name_season, file->name_number) :
			g_strdup("Kept from before");
		g_autofree gchar *after = file->to_season >= 0 ? axis_tv_episode_label(file->to_season, file->to_number) :
			g_strdup_printf("None: kept, marked as lacking in %s", axis_tv_source_label(plan->to));
		HtmxTr *tr = htmx_tr_new();

		ADD(tr, cell_node(axis_web_breakable(file->path)));
		ADD(tr, cell_text(now));
		ADD(tr, cell_text(after));
		ADD(tr, cell_text(file->by_name));
		ADD(body, tr);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const AxisWebColumn added_columns[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Title", AXIS_WEB_COL_NAME },
	{ "Air date", AXIS_WEB_COL_TEXT },
	{ "Once added", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static HtmxNode *
added_card(const AxisTvRenumberPlan *plan)
{
	g_autofree gchar *heading = g_strdup_printf("Episodes new in %s", axis_tv_source_label(plan->to));
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (plan->added->len == 0)
		return NULL;
	card = axis_web_storage_card(heading);
	ADD(card, axis_web_storage_text("help", "Each is added monitored as its season is. Those that have aired are "
		"wanted and searched for, unless you leave them unmonitored below."));
	table = axis_web_table_new("numbering-added", added_columns);
	body = htmx_tbody_new();
	for (i = 0; i < plan->added->len; i++) {
		const AxisTvRenumberNew *a = g_ptr_array_index(plan->added, i);
		g_autofree gchar *label = axis_tv_episode_label(a->season, a->number);
		HtmxTr *tr = htmx_tr_new();

		ADD(tr, cell_text(label));
		ADD(tr, cell_bdi(a->title != NULL ? a->title : "Untitled"));
		ADD(tr, cell_text(a->air_date != NULL ? a->air_date : "Not known"));
		ADD(tr, cell_text(a->gets_file ? "In the library: a file's name places it here" : !a->monitored ?
			"Not monitored" : a->aired ? "Wanted" : "Monitored, not aired yet"));
		ADD(body, tr);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
others_card(const AxisTvRenumberPlan *plan)
{
	HtmxNode *card;
	HtmxDetails *details;
	HtmxSummary *summary;
	guint n = 0;
	guint i;

	for (i = 0; i < plan->rows->len; i++) {
		const AxisTvRenumbered *row = g_ptr_array_index(plan->rows, i);

		if (row->n_files == 0 && !row->grab)
			n++;
	}
	if (n == 0)
		return NULL;
	card = axis_web_storage_card("Every other episode");
	details = htmx_details_new();
	summary = htmx_summary_new();
	{
		g_autofree gchar *text = g_strdup_printf("Show the %u %s with no file", n, n == 1 ? "episode" : "episodes");

		htmx_node_set_text_content(HTMX_NODE(summary), text);
	}
	ADD(details, summary);
	ADD(details, rows_table(plan, FALSE));
	ADD(card, details);
	return card;
}

static HtmxNode *
confirm_card(
	HtmxRequest              *request,
	const AxisTvRenumberPlan *plan
){
	HtmxNode *card = axis_web_storage_card("Confirm the change");
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", plan->series_id);
	g_autofree gchar *back = g_strdup_printf("/series/%" G_GINT64_FORMAT "#settings", plan->series_id);
	g_autofree gchar *help = g_strdup_printf("The series follows %s from then on: its searches, release matching, "
		"imports and the scheduler use that numbering. The releases its earlier searches found are cleared, and none "
		"found under the old numbering can be grabbed any more: search again afterwards. No file is moved, renamed or "
		"deleted. You can change back the same way.", axis_tv_source_label(plan->to));
	HtmxNode *form;
	HtmxDiv *actions;

	if (plan->waits->len > 0 || plan->problem != NULL) {
		ADD(card, axis_web_storage_text("help", "The change cannot be made now: the notice at the top says why."));
		ADD(card, axis_web_browser_link(back, "Back to the series"));
		return card;
	}
	ADD(card, axis_web_storage_text("help", help));
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(form, axis_web_auth_hidden("to", plan->to));
	ADD(form, axis_web_auth_hidden("digest", plan->digest));
	if (plan->n_new_in_new_seasons > 0) {
		HtmxDiv *check = htmx_div_new();
		HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
		HtmxLabel *label = htmx_label_new_for("numbering-unmonitored");

		htmx_element_add_class(HTMX_ELEMENT(check), "field-check");
		htmx_input_set_name(input, "unmonitored");
		htmx_input_set_value(input, "yes");
		htmx_element_set_id(HTMX_ELEMENT(input), "numbering-unmonitored");
		/* Ticked when they fall in seasons the series did not have: those
		 * the administrator never chose to monitor. */
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
		htmx_node_set_text_content(HTMX_NODE(label), "Leave the episodes of new seasons unmonitored");
		ADD(check, input);
		ADD(check, label);
		ADD(form, check);
	}
	/* Files whose names say another episode after the change: placed by
	 * them only when asked (off by default). */
	{
		if (plan->n_names_placed > 0) {
			HtmxDiv *check = htmx_div_new();
			HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
			HtmxLabel *label = htmx_label_new_for("numbering-names");

			htmx_element_add_class(HTMX_ELEMENT(check), "field-check");
			htmx_input_set_name(input, "names");
			htmx_input_set_value(input, "yes");
			htmx_element_set_id(HTMX_ELEMENT(input), "numbering-names");
			htmx_node_set_text_content(HTMX_NODE(label), "After the change, place these files by their names");
			ADD(check, input);
			ADD(check, label);
			ADD(form, check);
			if (plan->names_left != NULL)
				ADD(form, axis_web_storage_text("help", plan->names_left));
		}
	}
	actions = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
	ADD(actions, axis_web_auth_button("Change the numbering", "button-primary"));
	ADD(actions, axis_web_browser_link(back, "Keep it as it is"));
	ADD(form, actions);
	ADD(card, form);
	return card;
}

static HtmxResponse *
numbering_page(
	AxisTv        *self,
	HtmxRequest   *request,
	NumberingWork *work,
	guint          status
){
	const AxisTvRenumberPlan *plan = work->plan;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = axis_tv_series_label(work->series->title, work->series->year);
	g_autofree gchar *title = g_strdup_printf("Change the episode numbering of %s", label);
	g_autofree gchar *intro = g_strdup_printf("%s follows %s now. Below is what following %s would make of each of "
		"its episodes. Nothing changes until you confirm at the bottom.", label, axis_tv_source_label(plan->from),
		axis_tv_source_label(plan->to));
	const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");
	HtmxResponse *response;
	HtmxNode *card;
	HtmxHeading *h1 = htmx_h1_new();

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro(intro));
	if (work->refused != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", work->refused));
	if (plan->waits->len > 0 && work->refused == NULL) {
		g_autoptr(GString) names = g_string_new(NULL);
		g_autofree gchar *text = NULL;
		guint i;

		for (i = 0; i < plan->waits->len; i++)
			g_string_append_printf(names, "%s%s", i == 0 ? "" : i + 1 == plan->waits->len ? " and " : ", ",
				(const gchar *)g_ptr_array_index(plan->waits, i));
		text = g_strdup_printf("The change waits for %s: a release fetched for one numbering would be read against the "
			"other. It can be made once each is imported or dismissed.", names->str);
		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	if (plan->problem != NULL && work->refused == NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", plan->problem));
	if (plan->notes != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", plan->notes));
	/* A series marked as anime (add-anime-support): what the change does to
	 * its absolute numbers, which come with the rows of the numbering
	 * followed. */
	if (work->series->anime) {
		g_autofree gchar *text = g_strcmp0(plan->to, "tvdb") == 0 ?
			g_strdup("This series is marked as anime. Following TheTVDB's aired order, its episodes carry the absolute "
				"numbers TheTVDB gives them, and its page says whether they can be used.") :
			g_strdup_printf("This series is marked as anime. Absolute numbers come from TheTVDB's aired order alone: "
				"following %s, its episodes have none.", axis_tv_source_label(plan->to));

		ADD(content, axis_web_storage_text("help numbering-anime", text));
	}
	/* What becomes monitored and searched for, said first. */
	if (plan->added->len > 0) {
		g_autofree gchar *text = NULL;

		guint in_old = plan->added->len - plan->n_new_in_new_seasons;
		guint new_monitored = plan->n_new_monitored - plan->n_old_monitored;
		guint new_wanted = plan->n_new_wanted - plan->n_old_wanted;

		if (plan->n_new_in_new_seasons > 0)
			text = g_strdup_printf("%u new %s: %u in a season this series did not have and %u in a season it has. "
				"The %u in a season it has follows that season's monitoring: %u monitored and %u searched for at once. "
				"The %u in a new season is left unmonitored with the box at the bottom ticked, as it is now; unticked, "
				"%u %s monitored and %u %s searched for at once.", plan->added->len,
				plan->added->len == 1 ? "episode" : "episodes", plan->n_new_in_new_seasons, in_old, in_old,
				plan->n_old_monitored, plan->n_old_wanted, plan->n_new_in_new_seasons, new_monitored,
				new_monitored == 1 ? "becomes" : "become", new_wanted, new_wanted == 1 ? "is" : "are");
		else
			text = g_strdup_printf("%u new %s, all in seasons this series has: they follow those seasons' "
				"monitoring, so %u %s monitored and %u %s searched for at once.", plan->added->len,
				plan->added->len == 1 ? "episode" : "episodes", plan->n_new_monitored,
				plan->n_new_monitored == 1 ? "is" : "are", plan->n_new_wanted, plan->n_new_wanted == 1 ? "is" : "are");
		ADD(content, axis_web_storage_text("notice notice-warning numbering-new", text));
	}
	/* The page shows TheTVDB's data: its notice beside it. */
	if (tvdb != NULL && (axis_tv_source_is_tvdb(plan->to) || axis_tv_source_is_tvdb(plan->from)))
		ADD(content, axis_web_meta_attribution(tvdb));
	ADD(content, summary_card(plan));
	ADD(content, files_card(plan));
	card = lacking_card(plan);
	if (card != NULL)
		ADD(content, card);
	card = disagree_card(plan);
	if (card != NULL)
		ADD(content, card);
	card = hand_card(plan);
	if (card != NULL)
		ADD(content, card);
	card = added_card(plan);
	if (card != NULL)
		ADD(content, card);
	card = others_card(plan);
	if (card != NULL)
		ADD(content, card);
	ADD(content, confirm_card(request, plan));
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
numbering_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	NumberingWork *work = data;

	if (work->error != NULL) {
		if (work->error->domain != AXIS_TV_ERROR)
			return axis_web_internal_error(request, work->error);
		if (work->error->code == AXIS_TV_ERROR_NOT_FOUND)
			return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
		{
			g_autofree gchar *message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]),
				work->error->message + 1);

			return axis_web_error_respond(request, 400, "Not changed", message);
		}
	}
	if (work->confirm && work->refused == NULL) {
		g_autofree gchar *location = g_strdup_printf("/series/%" G_GINT64_FORMAT "?done=numbering#settings",
			work->series_id);

		axis_web_storage_audit(request, "series %" G_GINT64_FORMAT " now follows %s", work->series_id, work->to);
		return axis_web_auth_redirect(request, location);
	}
	return numbering_page(self, request, work, work->confirm ? 409 : 200);
}

/* A source's name as the form gives it: what axis_meta_source_valid() can
 * take, and nothing longer. */
static gchar *
form_source(const gchar *text)
{
	if (text == NULL || strlen(text) > 40 || !axis_meta_source_valid(text))
		return NULL;
	return g_strdup(text);
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	NumberingWork *work;
	gint64 id = axis_web_storage_param_id(params);
	g_autofree gchar *to = form_source(htmx_request_get_query_param(request, "to"));

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (to == NULL)
		return axis_web_error_respond(request, 400, "Not shown", "Say which numbering to show.");
	work = g_new0(NumberingWork, 1);
	work->series_id = id;
	work->to = g_steal_pointer(&to);
	return axis_tv_web_dispatch(user_data, request, numbering_run, numbering_done, work, numbering_work_free);
}

static gboolean
digest_valid(const gchar *digest)
{
	gsize i;

	if (digest == NULL || strlen(digest) != 32)
		return FALSE;
	for (i = 0; i < 32; i++) {
		if (!g_ascii_isxdigit(digest[i]) || g_ascii_isupper(digest[i]))
			return FALSE;
	}
	return TRUE;
}

static HtmxResponse *
handle_change(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	NumberingWork *work;
	gint64 id = axis_web_storage_param_id(params);
	g_autofree gchar *to = form_source(htmx_request_get_form_value(request, "to"));
	const gchar *digest = htmx_request_get_form_value(request, "digest");

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (to == NULL || !digest_valid(digest))
		return axis_web_error_respond(request, 400, "Not changed", "The form is not complete: open the page again.");
	work = g_new0(NumberingWork, 1);
	work->series_id = id;
	work->to = g_steal_pointer(&to);
	work->digest = g_strdup(digest);
	work->confirm = TRUE;
	work->unmonitored = axis_web_storage_form_yes(request, "unmonitored");
	work->names = axis_web_storage_form_yes(request, "names");
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_tv_web_dispatch(user_data, request, numbering_run, numbering_done, work, numbering_work_free);
}

/* ── Placing files by their names ───────────────────────────────── */

typedef struct
{
	gint64        series_id;
	gint64        file_id;	/* 0: every one that can be */
	gboolean      confirm;
	gchar        *user;
	gchar        *digest;	/* the list the person saw ("all" only) */
	gchar        *now;	/* the list as it is: its digest */
	gchar        *left;	/* what "Place all" would leave without a file */
	gboolean      changed;	/* it is not the list the person saw */
	AxisTvSeries *series;
	GPtrArray    *list;	/* AxisTvMisplaced */
	guint         placed;
	GError       *error;
} PlaceWork;

static void
place_work_free(gpointer data)
{
	PlaceWork *work = data;

	g_free(work->user);
	g_free(work->digest);
	g_free(work->now);
	g_free(work->left);
	axis_tv_series_free(work->series);
	g_clear_pointer(&work->list, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
place_run(
	AxisTv   *self,
	gpointer  data
){
	PlaceWork *work = data;

	if (!axis_tv_get(self, work->series_id, &work->series, NULL, &work->error))
		return;
	if (work->confirm) {
		if (!axis_tv_place_by_name(self, work->series_id, work->file_id, work->digest, work->user, &work->placed,
		        &work->changed, &work->error) || !work->changed)
			return;
		/* The list is not the one the person saw: it is shown again. */
	}
	axis_tv_list_misplaced(self, work->series_id, work->file_id, &work->list, &work->left, &work->now, &work->error);
}

static const AxisWebColumn place_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "On now", AXIS_WEB_COL_TEXT },
	{ "On after", AXIS_WEB_COL_TEXT },
	{ "What changes", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static HtmxResponse *
place_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	PlaceWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *back = NULL;
	HtmxHeading *h1;
	HtmxNode *table;
	HtmxTbody *body;
	guint shown = 0;
	guint held_back = 0;
	gboolean earlier = FALSE;
	guint i;

	if (work->error != NULL) {
		g_autofree gchar *message = NULL;

		if (work->error->domain != AXIS_TV_ERROR)
			return axis_web_internal_error(request, work->error);
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, work->error->code == AXIS_TV_ERROR_NOT_FOUND ? 404 : 409, "Not placed",
			message);
	}
	if (work->confirm && !work->changed) {
		g_autofree gchar *location = g_strdup_printf("/series/%" G_GINT64_FORMAT "?done=placed#misplaced",
			work->series_id);

		axis_web_storage_audit(request, "series %" G_GINT64_FORMAT ": %u files placed by their names", work->series_id,
			work->placed);
		return axis_web_auth_redirect(request, location);
	}
	label = axis_tv_series_label(work->series->title, work->series->year);
	title = g_strdup_printf("Place files by their names: %s", label);
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", work->series_id);
	back = g_strdup_printf("/series/%" G_GINT64_FORMAT "#misplaced", work->series_id);
	content = htmx_div_new();
	h1 = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Only each file's link moves, to the episode its name says; the file on disk is "
		"not touched. Nothing changes until you confirm below."));
	if (work->changed)
		ADD(content, axis_web_storage_text("notice notice-warning", "What can be placed changed since you opened the "
			"page, so nothing was placed. The list below is what can be placed now: confirm it again if it is right."));
	table = axis_web_table_new("series-place", place_columns);
	body = htmx_tbody_new();
	for (i = 0; work->list != NULL && i < work->list->len; i++) {
		const AxisTvMisplaced *m = g_ptr_array_index(work->list, i);
		g_autofree gchar *on = axis_tv_episode_label(m->season, m->number);
		g_autofree gchar *after = axis_tv_episode_label(m->name_season, m->name_number);
		g_autofree gchar *what = NULL;
		/* All of them: what the run of all makes of each; one file: what
		 * placing it alone does. */
		const gchar *refused = work->file_id > 0 ? m->refused : m->all_refused;
		gboolean leaves_wanted = work->file_id > 0 ? m->leaves_wanted : m->all_leaves_wanted;
		HtmxTr *tr;

		if (work->file_id > 0 ? m->file_id != work->file_id : m->earlier) {
			if (work->file_id == 0)
				held_back++;
			continue;
		}
		if (m->earlier)
			earlier = TRUE;
		what = refused != NULL ? g_strdup_printf("Not placed: %s", refused) : leaves_wanted ?
			g_strdup_printf("%s holds the file; %s is left without one and, monitored and aired, is wanted and searched "
				"for.", after, on) :
			g_strdup_printf("%s holds the file; %s is left without one, not wanted (not monitored, or not aired).", after,
				on);
		tr = htmx_tr_new();
		ADD(tr, cell_node(axis_web_breakable(m->path)));
		ADD(tr, cell_text(on));
		ADD(tr, cell_text(refused != NULL ? on : after));
		ADD(tr, cell_text(what));
		ADD(body, tr);
		if (refused == NULL)
			shown++;
	}
	ADD(table, body);
	{
		HtmxNode *card = axis_web_storage_card(work->file_id > 0 ? "The file and its episodes" :
			"The files and their episodes");

		if (earlier)
			ADD(card, axis_web_storage_text("notice notice-warning", "An import by Axis linked this file, or its name "
				"was read under the earlier numbering, so its name is probably just the old number and the episode it "
				"is on may be the right one. Place it by its name only if you know the name is right: the file may "
				"then be on the wrong episode, and the right one wanted and downloaded again."));
		if (work->file_id == 0 && work->left != NULL)
			ADD(card, axis_web_storage_text("help", work->left));
		if (work->file_id == 0 && held_back > 0) {
			g_autofree gchar *text = g_strdup_printf("%u %s named under the earlier numbering %s not part of this: place "
				"%s one by one from the series page.", held_back, held_back == 1 ? "file" : "files",
				held_back == 1 ? "is" : "are", held_back == 1 ? "it" : "them");

			ADD(card, axis_web_storage_text("help", text));
		}
		ADD(card, table);
		ADD(content, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Confirm");
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		if (shown > 0) {
			HtmxNode *form = axis_web_auth_post_form(request, action);
			g_autofree gchar *file = work->file_id > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, work->file_id) :
				g_strdup("all");

			ADD(form, axis_web_auth_hidden("file", file));
			if (work->now != NULL)
				ADD(form, axis_web_auth_hidden("digest", work->now));
			ADD(form, axis_web_auth_button(work->file_id > 0 ? "Place by its name" : "Place all by their names",
				"button-primary"));
			ADD(actions, form);
		} else {
			ADD(card, axis_web_storage_text("help", "Nothing here can be placed now: each row says why."));
		}
		ADD(actions, axis_web_browser_link(back, "Keep them as they are"));
		ADD(card, actions);
		ADD(content, card);
	}
	{
		HtmxResponse *response = axis_web_page_respond(request, title, HTMX_NODE(content));

		if (work->changed)
			htmx_response_set_status(response, 409);
		return response;
	}
}

static gboolean
place_file(
	HtmxRequest *request,
	const gchar *text,
	gint64      *file_id
){
	guint64 value = 0;

	*file_id = 0;
	if (g_strcmp0(text, "all") == 0)
		return TRUE;
	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, G_MAXINT64, &value, NULL))
		return FALSE;
	*file_id = (gint64)value;
	return TRUE;
}

static HtmxResponse *
start_place(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     confirm
){
	PlaceWork *work;
	gint64 id = axis_web_storage_param_id(params);
	gint64 file_id = 0;
	const gchar *file = confirm ? htmx_request_get_form_value(request, "file") :
		htmx_request_get_query_param(request, "file");

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (!place_file(request, file, &file_id))
		return axis_web_error_respond(request, 400, "Not placed", "Say which file, or all of them.");
	if (confirm && !digest_valid(htmx_request_get_form_value(request, "digest")))
		return axis_web_error_respond(request, 400, "Not placed", "The form is not complete: open the page again.");
	work = g_new0(PlaceWork, 1);
	work->series_id = id;
	work->file_id = file_id;
	work->confirm = confirm;
	if (confirm)
		work->digest = g_strdup(htmx_request_get_form_value(request, "digest"));
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_tv_web_dispatch(user_data, request, place_run, place_done, work, place_work_free);
}

static HtmxResponse *
handle_place_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_place(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_place(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_place(request, params, user_data, TRUE);
}

void
axis_tv_web_register_numbering(
	AxisTv  *self,
	AxisWeb *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/place", AXIS_WEB_ROUTE_ADMIN, handle_place_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/place", AXIS_WEB_ROUTE_ADMIN, handle_place, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/numbering", AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/numbering", AXIS_WEB_ROUTE_ADMIN, handle_change, self);
}
