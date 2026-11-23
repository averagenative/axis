/*
 * axis-tv-web-links.c - a series' links page: its files linked to episodes
 * by hand (add-force-grab-and-assign, slice C1)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /series/:id/links[?file=N&page=P]
 *                                     every video file linked to the series
 *                                     with how its link was made, one page of
 *                                     the video files of its folders with no
 *                                     link (and the file asked for, when it
 *                                     is in that reach); writes nothing
 *   POST /series/:id/links            step=review: the review of the plan,
 *                                     writes nothing; step=apply: the change
 *                                     (digest=, other_<file>=yes, the ticks
 *                                     the review shown)
 *   POST /series/:id/links/manage     file=, step=review: what "Let Axis
 *                                     manage this file again" would do;
 *                                     step=apply (digest=): it does it
 *
 * All an administrator's (AXIS_WEB_ROUTE_ADMIN, and the shell's CSRF check
 * for the posts).  A plan is posted as one field for each file:
 * to_<library file id>=<episodes, as S01E03 S01E04> or unlink_<id>=yes.  The
 * review holds the plan again as hidden fields with its digest; a
 * confirmation made after what it showed changed, or while an import of the
 * series is open or a grab claims an episode it touches, changes nothing and
 * shows the review again with status 409.  File names are set as text.
 */

#include "modules/tv/axis-tv-web.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-links.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add

/* Shared with the movies' links page (web/axis-web-links.h). */
#define LINKS_PLAN_MAX AXIS_WEB_LINKS_PLAN_MAX

#define TO_PREFIX     AXIS_WEB_LINKS_TO
#define UNLINK_PREFIX AXIS_WEB_LINKS_UNLINK
#define OTHER_PREFIX  AXIS_WEB_LINKS_OTHER

#define field_file   axis_web_links_field_file
#define digest_valid axis_web_links_digest_valid
#define tick_box     axis_web_links_tick_box

typedef struct
{
	gint64        series_id;
	gint64        file_id;		/* the page: a file to list wherever it is; manage: the file */
	gboolean      review;
	gboolean      apply;
	gboolean      manage;		/* manage: step=review, or (manage_apply) step=apply */
	gboolean      manage_apply;
	guint         page;		/* the page: which page of the files with no link */
	GPtrArray    *asks;		/* AxisTvLinkAsk */
	GArray       *others;		/* gint64 */
	gchar        *digest;
	gchar        *user;
	gchar        *done;
	AxisTvLinks  *links;
	AxisTvLinkManage *handed;
	gboolean      changed;
	gboolean      refused;
	gboolean      stale;		/* a file went to another series meanwhile: the page again */
	gchar        *stale_text;
	GError       *error;
} LinksWork;

static void
links_work_free(gpointer data)
{
	LinksWork *work = data;

	g_clear_pointer(&work->asks, g_ptr_array_unref);
	g_clear_pointer(&work->others, g_array_unref);
	g_free(work->digest);
	g_free(work->user);
	g_free(work->done);
	g_free(work->stale_text);
	axis_tv_links_free(work->links);
	axis_tv_link_manage_free(work->handed);
	g_clear_error(&work->error);
	g_free(work);
}

static void
links_run(
	AxisTv   *self,
	gpointer  data
){
	LinksWork *work = data;
	g_autoptr(GError) local = NULL;

	if (work->manage && work->manage_apply) {
		if (axis_tv_links_manage(self, work->series_id, work->file_id, work->digest, work->user, &work->handed,
		        &work->changed, &local))
			return;
		/* Refused for now: the confirmation again, which says why. */
		if (work->handed != NULL && g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT)) {
			work->refused = TRUE;
			return;
		}
		g_propagate_error(&work->error, g_steal_pointer(&local));
		return;
	}
	if (work->manage) {
		axis_tv_links_manage_review(self, work->series_id, work->file_id, &work->handed, &work->error);
		return;
	}
	if (work->apply) {
		if (axis_tv_links_apply(self, work->series_id, work->asks, work->others, work->digest, work->user, &work->links,
		        &work->changed, &local))
			return;
		/* Refused for now: the review again, which says why. */
		if (work->links != NULL && g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT)) {
			work->refused = TRUE;
			return;
		}
		/* A file went to another series meanwhile, so the plan no longer
		 * holds: the links page again, as it is now. */
		if (g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_IN_FLIGHT) &&
		    axis_tv_links_read(self, work->series_id, NULL, 0, &work->links, NULL)) {
			work->stale = TRUE;
			work->stale_text = g_strdup(local->message);
			return;
		}
		g_propagate_error(&work->error, g_steal_pointer(&local));
		return;
	}
	if (work->review) {
		axis_tv_links_review(self, work->series_id, work->asks, work->others, &work->links, &work->error);
		return;
	}
	{
		g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));

		if (work->file_id > 0)
			g_array_append_val(also, work->file_id);
		axis_tv_links_read(self, work->series_id, also, work->page, &work->links, &work->error);
	}
}

/* ── Pieces ─────────────────────────────────────────────────────── */

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

static const AxisWebColumn linked_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "On", AXIS_WEB_COL_TEXT },
	{ "Link", AXIS_WEB_COL_TEXT },
	{ "Change", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

static const AxisWebColumn free_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Link", AXIS_WEB_COL_TEXT },
	{ "Change", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

static const AxisWebColumn hand_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Link", AXIS_WEB_COL_TEXT },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

/* The choice for one file: its episodes, and (a linked one) Unlink. */
static HtmxNode *
choice(const AxisTvLinkFile *file)
{
	HtmxDiv *controls = htmx_div_new();
	g_autofree gchar *to_id = g_strdup_printf("to-%" G_GINT64_FORMAT, file->file_id);
	g_autofree gchar *to_name = g_strdup_printf(TO_PREFIX "%" G_GINT64_FORMAT, file->file_id);

	htmx_element_add_class(HTMX_ELEMENT(controls), "film-actions");
	ADD(controls, axis_web_storage_text_field(to_id, file->linked ? "Move to" : "Link to", to_name, NULL,
		file->name_says != NULL ? file->name_says : "S01E01", NULL));
	if (file->linked) {
		g_autofree gchar *un_id = g_strdup_printf("unlink-%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *un_name = g_strdup_printf(UNLINK_PREFIX "%" G_GINT64_FORMAT, file->file_id);

		ADD(controls, axis_web_storage_checkbox(un_id, "Unlink", un_name, FALSE));
	}
	return HTMX_NODE(controls);
}

static HtmxNode *
file_cell(const AxisTvLinkFile *file)
{
	HtmxTd *cell = htmx_td_new();

	ADD(cell, axis_web_breakable(file->path));
	if (file->name_says != NULL) {
		g_autofree gchar *says = file->name_absolute > 0 ?
			g_strdup_printf("Its name says %s, by its absolute number %d as TheTVDB numbers the series now.",
				file->name_says, file->name_absolute) :
			g_strdup_printf("Its name says %s.", file->name_says);

		ADD(cell, axis_web_storage_text("range-detail", says));
	}
	return HTMX_NODE(cell);
}

static HtmxResponse *
links_page(
	HtmxRequest     *request,
	const LinksWork *work
){
	const AxisTvLinks *links = work->links;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = axis_tv_series_label(links->series->title, links->series->year);
	g_autofree gchar *title = g_strdup_printf("Files and links: %s", label);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", links->series->id);
	g_autofree gchar *manage = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links/manage", links->series->id);
	g_autofree gchar *back = g_strdup_printf("/series/%" G_GINT64_FORMAT, links->series->id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxNode *form;
	HtmxNode *linked_table = axis_web_table_new("series-links", linked_columns);
	HtmxNode *free_table = axis_web_table_new("series-links-free", free_columns);
	HtmxNode *hand_table = axis_web_table_new("series-links-hand", hand_columns);
	HtmxTbody *linked_body = htmx_tbody_new();
	HtmxTbody *free_body = htmx_tbody_new();
	HtmxTbody *hand_body = htmx_tbody_new();
	guint n_linked = 0;
	guint n_free = 0;
	guint n_hand = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Say which episodes each video file holds. Only links change: no file on disk is "
		"moved, renamed or deleted. A media server reads a file's name, so a file named for another episode still shows "
		"there as that episode. Nothing changes until you confirm on the next page."));
	if (g_strcmp0(work->done, "linked") == 0)
		ADD(content, axis_web_auth_notice(FALSE, "The links were changed. No file on disk was touched. Each link "
			"made is marked as made by hand and kept by hand: Axis does not move, replace or upgrade it by itself. Each "
			"episode's history says what changed."));
	else if (g_strcmp0(work->done, "managed") == 0)
		ADD(content, axis_web_auth_notice(FALSE, "Axis manages that file again: identification, placing by names, "
			"upgrades and imports treat it by the ordinary rules."));
	if (work->stale) {
		g_autofree gchar *text = g_strdup_printf("A file changed while you were reviewing, so nothing was changed: %s. Below "
			"are the links as they are now.", work->stale_text != NULL ? work->stale_text : "a file moved to another series");

		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	if (links->busy != NULL) {
		g_autofree gchar *text = g_strdup_printf("No link can be changed now. %s", links->busy);

		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	if (links->doubles->len > 0) {
		HtmxNode *card = axis_web_storage_card("Episodes with more than one file");

		htmx_element_set_id(HTMX_ELEMENT(card), "doubles");
		for (i = 0; i < links->doubles->len; i++)
			ADD(card, axis_web_storage_text("notice notice-warning", g_ptr_array_index(links->doubles, i)));
		ADD(card, axis_web_storage_text("help", "Both stay linked until you unlink one below. An episode with two files "
			"is judged for upgrades by its worse file."));
		ADD(content, card);
	}
	for (i = 0; i < links->files->len; i++) {
		const AxisTvLinkFile *file = g_ptr_array_index(links->files, i);
		g_autofree gchar *anchor = g_strdup_printf("file-%" G_GINT64_FORMAT, file->file_id);
		HtmxTr *row = htmx_tr_new();

		htmx_element_set_id(HTMX_ELEMENT(row), anchor);
		ADD(row, file_cell(file));
		if (file->linked) {
			ADD(row, cell_text(file->on_gone ? "On an episode the numbering this series follows does not have" :
				file->on));
			/* Its release is known (the import's download): an administrator may
			 * block it for the future.  The page it leads to writes nothing, and
			 * blocking never touches this file, but while the download's grab is
				 * still open it also gives up on the rest of the download
				 * (add-block-release). */
			if (file->import_id > 0) {
				g_autofree gchar *block = g_strdup_printf("/series/block-release?import=%" G_GINT64_FORMAT,
					file->import_id);
				HtmxTd *how = htmx_td_new();
				HtmxSpan *words = htmx_span_new();
				HtmxSpan *line = htmx_span_new();

				htmx_node_set_text_content(HTMX_NODE(words), file->how != NULL ? file->how : "");
				htmx_element_add_class(HTMX_ELEMENT(line), "range-detail");
				ADD(line, axis_web_browser_link(block, file->grab_open ? "Block and stop the rest of this download" :
					"Block this release"));
				ADD(how, words);
				ADD(how, line);
				ADD(row, how);
			} else {
				ADD(row, cell_text(file->how));
			}
			ADD(row, cell_node(choice(file)));
			ADD(linked_body, row);
			n_linked++;
		} else {
			ADD(row, cell_text(file->how));
			if (file->other != NULL)
				ADD(row, cell_text("Unlink it on that series' links page first."));
			else
				ADD(row, cell_node(choice(file)));
			ADD(free_body, row);
			n_free++;
		}
		if ((file->linked && file->hand) || file->unlinked_by_hand) {
			HtmxTr *hand = htmx_tr_new();
			HtmxNode *let = axis_web_auth_post_form(request, manage);
			g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file->file_id);

			ADD(hand, cell_node(axis_web_breakable(file->path)));
			ADD(hand, cell_text(file->how));
			ADD(let, axis_web_auth_hidden("step", "review"));
			ADD(let, axis_web_auth_hidden("file", id));
			ADD(let, axis_web_auth_button("Let Axis manage this file again", "button-small"));
			ADD(hand, cell_node(let));
			ADD(hand_body, hand);
			n_hand++;
		}
	}
	ADD(linked_table, linked_body);
	ADD(free_table, free_body);
	ADD(hand_table, hand_body);
	form = axis_web_auth_post_form(request, action);
	ADD(form, axis_web_auth_hidden("step", "review"));
	{
		HtmxNode *card = axis_web_storage_card("Linked files");

		htmx_element_set_id(HTMX_ELEMENT(card), "linked");
		if (n_linked == 0) {
			ADD(card, axis_web_storage_text("help", "No video file is linked to this series."));
			g_object_unref(linked_table);
		} else {
			ADD(card, axis_web_storage_text("help", "To move a file's link, write its episodes as S01E03; for a file "
				"that holds two episodes, both, with a space between (S01E03 S01E04). To swap two files, give each the "
				"other's episode. Leave a row empty to leave it as it is."));
			ADD(card, linked_table);
		}
		ADD(form, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Files with no link");

		htmx_element_set_id(HTMX_ELEMENT(card), "unlinked");
		if (n_free == 0) {
			ADD(card, axis_web_storage_text("help", "Every video file in this series' folders is linked."));
			g_object_unref(free_table);
		} else {
			ADD(card, axis_web_storage_text("help", "Video files in this series' folders that no episode holds. A file "
				"unlinked by hand is left alone by identification until you link it or let Axis manage it again."));
			ADD(card, free_table);
		}
		if (links->free_total > AXIS_TV_LINK_PAGE) {
			g_autofree gchar *shown = g_strdup_printf("Files %u to %u of %u. Each page lists %d files.",
				links->page * AXIS_TV_LINK_PAGE + 1, MIN((links->page + 1) * AXIS_TV_LINK_PAGE, links->free_total),
				links->free_total, AXIS_TV_LINK_PAGE);
			HtmxDiv *pages = htmx_div_new();

			htmx_element_set_id(HTMX_ELEMENT(pages), "unlinked-pages");
			htmx_element_add_class(HTMX_ELEMENT(pages), "film-actions");
			ADD(pages, axis_web_storage_text("help", shown));
			if (links->page > 0) {
				g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links?page=%u", links->series->id,
					links->page - 1);

				ADD(pages, axis_web_browser_link(href, "Earlier files"));
			}
			if ((links->page + 1) * AXIS_TV_LINK_PAGE < links->free_total) {
				g_autofree gchar *href = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links?page=%u", links->series->id,
					links->page + 1);

				ADD(pages, axis_web_browser_link(href, "Later files"));
			}
			ADD(card, pages);
		}
		ADD(form, card);
	}
	{
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		if (links->busy == NULL && n_linked + n_free > 0)
			ADD(actions, axis_web_auth_button("Review the change", "button-primary"));
		ADD(actions, axis_web_browser_link(back, "Back to the series"));
		ADD(form, actions);
	}
	ADD(content, form);
	if (n_hand > 0) {
		HtmxNode *card = axis_web_storage_card("Linked, kept or unlinked by hand");

		htmx_element_set_id(HTMX_ELEMENT(card), "by-hand");
		ADD(card, axis_web_storage_text("help", "Axis leaves these files as a person left them: identification, placing "
			"by names, a change of numbering and imports do not move, replace or link them, and no upgrade is searched "
			"for. Letting Axis manage a file again hands that one file back to identification, which places it by its "
			"name: its links are replaced by what its name gives, and the next page shows which episodes that is before "
			"anything changes."));
		ADD(card, hand_table);
		ADD(content, card);
	} else {
		g_object_unref(hand_table);
	}
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static const AxisWebColumn change_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "On now", AXIS_WEB_COL_TEXT },
	{ "On after", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static const AxisWebColumn touched_columns[] = {
	{ "Episode", AXIS_WEB_COL_TEXT },
	{ "Files now", AXIS_WEB_COL_WIDE },
	{ "Files after", AXIS_WEB_COL_WIDE },
	{ "What changes", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static HtmxResponse *
review_page(
	HtmxRequest     *request,
	const LinksWork *work
){
	const AxisTvLinks *links = work->links;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = axis_tv_series_label(links->series->title, links->series->year);
	g_autofree gchar *title = g_strdup_printf("Review the change of links: %s", label);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", links->series->id);
	g_autofree gchar *back = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", links->series->id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxResponse *response;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Only links change: no file on disk is moved, renamed or deleted. Nothing changes "
		"until you confirm below."));
	if (work->changed)
		ADD(content, axis_web_storage_text("notice notice-warning", "The series changed after this review was made (a "
			"file arrived, a link moved, a download started, or its numbering changed), so nothing was changed. Below is "
			"what the change would do now: confirm it again if it is right."));
	if (links->refused != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", links->refused));
	{
		HtmxNode *card = axis_web_storage_card("Files");
		HtmxNode *table = axis_web_table_new("links-review-files", change_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < links->changes->len; i++) {
			const AxisTvLinkChange *change = g_ptr_array_index(links->changes, i);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *file = htmx_td_new();

			ADD(file, axis_web_breakable(change->path));
			if (change->note != NULL)
				ADD(file, axis_web_storage_text("notice notice-warning", change->note));
			ADD(row, file);
			ADD(row, cell_text(change->before));
			ADD(row, cell_text(g_strcmp0(change->after, "No episode") == 0 ? "No episode: unlinked, and left alone by "
				"identification until a person links it or lets Axis manage it again" : change->after));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, axis_web_storage_text("help", "Each link made is marked as made by hand and kept by hand."));
		ADD(card, table);
		ADD(content, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Episodes");
		HtmxNode *table = axis_web_table_new("links-review-episodes", touched_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < links->touched->len; i++) {
			const AxisTvLinkTouched *touched = g_ptr_array_index(links->touched, i);
			HtmxTr *row = htmx_tr_new();

			ADD(row, cell_text(touched->label));
			ADD(row, cell_node(axis_web_breakable(touched->before)));
			ADD(row, cell_node(axis_web_breakable(touched->after)));
			ADD(row, cell_text(touched->what != NULL ? touched->what : "One file, as before."));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
		ADD(content, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Confirm");
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		if (links->offers->len > 0) {
			/* The ticks are part of what is confirmed: ticking is answered with
			 * the review again, showing every file the ticks touch. */
			HtmxNode *ticks = axis_web_auth_post_form(request, action);

			ADD(ticks, axis_web_auth_hidden("step", "review"));
			for (i = 0; work->asks != NULL && i < work->asks->len; i++) {
				const AxisTvLinkAsk *ask = g_ptr_array_index(work->asks, i);
				g_autofree gchar *name = g_strdup_printf("%s%" G_GINT64_FORMAT, ask->unlink ? UNLINK_PREFIX : TO_PREFIX,
					ask->file_id);

				ADD(ticks, axis_web_auth_hidden(name, ask->unlink ? "yes" : ask->to != NULL ? ask->to : ""));
			}
			for (i = 0; i < links->offers->len; i++) {
				const AxisTvLinkOffer *offer = g_ptr_array_index(links->offers, i);
				g_autofree gchar *id = g_strdup_printf("other-%" G_GINT64_FORMAT, offer->file_id);
				g_autofree gchar *name = g_strdup_printf(OTHER_PREFIX "%" G_GINT64_FORMAT, offer->file_id);
				g_autofree gchar *text = g_strdup_printf("Unlink %s from %s instead. It is then on: %s.", offer->path,
					offer->episodes, offer->left);

				ADD(ticks, tick_box(id, text, name, offer->ticked));
			}
			ADD(ticks, axis_web_storage_text("help", "Without a tick, both files stay linked to the episode. After you tick, "
				"show the review again: it then lists every file the ticks touch, before and after."));
			ADD(ticks, axis_web_auth_button("Show the review with these ticks", "button-small"));
			ADD(card, ticks);
		}
		if (links->refused == NULL) {
			HtmxNode *form = axis_web_auth_post_form(request, action);

			htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
			ADD(form, axis_web_auth_hidden("step", "apply"));
			ADD(form, axis_web_auth_hidden("digest", links->digest));
			for (i = 0; work->asks != NULL && i < work->asks->len; i++) {
				const AxisTvLinkAsk *ask = g_ptr_array_index(work->asks, i);
				g_autofree gchar *name = g_strdup_printf("%s%" G_GINT64_FORMAT, ask->unlink ? UNLINK_PREFIX : TO_PREFIX,
					ask->file_id);

				ADD(form, axis_web_auth_hidden(name, ask->unlink ? "yes" : ask->to != NULL ? ask->to : ""));
			}
			/* The ticks the review above shows, and only those. */
			for (i = 0; i < links->offers->len; i++) {
				const AxisTvLinkOffer *offer = g_ptr_array_index(links->offers, i);
				g_autofree gchar *name = g_strdup_printf(OTHER_PREFIX "%" G_GINT64_FORMAT, offer->file_id);

				if (offer->ticked)
					ADD(form, axis_web_auth_hidden(name, "yes"));
			}
			ADD(actions, axis_web_auth_button("Change the links", "button-primary"));
			ADD(actions, axis_web_browser_link(back, "Keep them as they are"));
			ADD(form, actions);
			ADD(card, form);
		} else {
			ADD(card, axis_web_storage_text("help", "The change cannot be made now: the notice at the top says why."));
			ADD(actions, axis_web_browser_link(back, "Back to the links"));
			ADD(card, actions);
		}
		ADD(content, card);
	}
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (work->changed || work->refused)
		htmx_response_set_status(response, 409);
	return response;
}

/* "Let Axis manage this file again": what it does, before it does it. */
static HtmxResponse *
manage_page(
	HtmxRequest     *request,
	const LinksWork *work
){
	const AxisTvLinkManage *handed = work->handed;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = axis_tv_series_label(handed->series->title, handed->series->year);
	g_autofree gchar *title = g_strdup_printf("Let Axis manage this file again: %s", label);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links/manage", handed->series->id);
	g_autofree gchar *back = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", handed->series->id);
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, handed->file_id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxResponse *response;
	HtmxNode *card = axis_web_storage_card("What happens");
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Only links change: no file on disk is moved, renamed or deleted. Nothing changes until "
		"you confirm below."));
	if (work->changed)
		ADD(content, axis_web_storage_text("notice notice-warning", "The series changed after this was shown, so nothing was "
			"changed. Below is what handing the file back would do now: confirm it again if it is right."));
	if (handed->refused != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", handed->refused));
	htmx_element_set_id(HTMX_ELEMENT(card), "manage");
	ADD(card, axis_web_breakable(handed->path));
	ADD(card, axis_web_storage_text("help", handed->outcome));
	if (handed->touched->len > 0) {
		HtmxNode *table = axis_web_table_new("links-manage-episodes", touched_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < handed->touched->len; i++) {
			const AxisTvLinkTouched *touched = g_ptr_array_index(handed->touched, i);
			HtmxTr *row = htmx_tr_new();

			ADD(row, cell_text(touched->label));
			ADD(row, cell_node(axis_web_breakable(touched->before)));
			ADD(row, cell_node(axis_web_breakable(touched->after)));
			ADD(row, cell_text(touched->what != NULL ? touched->what : "One file, as before."));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);
	{
		HtmxNode *confirm = axis_web_storage_card("Confirm");
		HtmxDiv *actions = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(actions), "film-actions");
		if (handed->refused == NULL) {
			HtmxNode *form = axis_web_auth_post_form(request, action);

			ADD(form, axis_web_auth_hidden("step", "apply"));
			ADD(form, axis_web_auth_hidden("file", id));
			ADD(form, axis_web_auth_hidden("digest", handed->digest));
			ADD(actions, axis_web_auth_button("Let Axis manage this file", "button-primary"));
			ADD(actions, axis_web_browser_link(back, "Keep it as it is"));
			ADD(form, actions);
			ADD(confirm, form);
		} else {
			ADD(confirm, axis_web_storage_text("help", "It cannot be done now: the notice at the top says why."));
			ADD(actions, axis_web_browser_link(back, "Back to the links"));
			ADD(confirm, actions);
		}
		ADD(content, confirm);
	}
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (work->changed || work->refused)
		htmx_response_set_status(response, 409);
	return response;
}

static HtmxResponse *
links_done(
	AxisTv      *self,
	HtmxRequest *request,
	gpointer     data
){
	LinksWork *work = data;

	if (work->error != NULL) {
		g_autofree gchar *message = NULL;

		if (work->error->domain != AXIS_TV_ERROR)
			return axis_web_internal_error(request, work->error);
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, work->error->code == AXIS_TV_ERROR_NOT_FOUND ? 404 :
			work->error->code == AXIS_TV_ERROR_INVALID ? 400 : 409, "Not changed", message);
	}
	if (work->manage && work->manage_apply && !work->changed && !work->refused) {
		g_autofree gchar *location = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links?done=managed", work->series_id);

		axis_web_storage_audit(request, "series %" G_GINT64_FORMAT " file %" G_GINT64_FORMAT " managed by Axis again",
			work->series_id, work->file_id);
		return axis_web_auth_redirect(request, location);
	}
	if (work->manage)
		return manage_page(request, work);
	if (work->apply && !work->changed && !work->refused) {
		g_autofree gchar *location = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links?done=linked", work->series_id);

		axis_web_storage_audit(request, "series %" G_GINT64_FORMAT ": links of %u files changed by hand", work->series_id,
			work->links != NULL ? work->links->changes->len : 0);
		return axis_web_auth_redirect(request, location);
	}
	if (work->stale) {
		HtmxResponse *response = links_page(request, work);

		htmx_response_set_status(response, 409);
		return response;
	}
	if (work->review || work->apply)
		return review_page(request, work);
	return links_page(request, work);
}

/* ── The handlers ───────────────────────────────────────────────── */

/* The plan a form posts: one ask for each file with a choice, by file id. */
static gboolean
read_plan(
	HtmxRequest *request,
	LinksWork   *work
){
	GHashTable *fields = htmx_request_get_form_data(request);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	work->asks = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_link_ask_free);
	work->others = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (fields == NULL)
		return TRUE;
	g_hash_table_iter_init(&iter, fields);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 file_id = 0;
		gboolean unlink = field_file(key, UNLINK_PREFIX, &file_id);
		g_autofree gchar *text = value != NULL ? g_strstrip(g_strdup(value)) : NULL;

		if (field_file(key, OTHER_PREFIX, &file_id)) {
			if (g_strcmp0(text, "yes") == 0 && work->others->len < LINKS_PLAN_MAX)
				g_array_append_val(work->others, file_id);
			continue;
		}
		if (!unlink && !field_file(key, TO_PREFIX, &file_id))
			continue;
		if (text == NULL || text[0] == '\0' || (unlink && strcmp(text, "yes") != 0))
			continue;
		if (work->asks->len >= LINKS_PLAN_MAX)
			return FALSE;
		g_ptr_array_add(work->asks, axis_tv_link_ask_new(file_id, unlink, unlink ? NULL : text));
	}
	/* (Unlink ticked and episodes written for one file are two asks: the
	 * store refuses the file named twice.) */
	return TRUE;
}

static gint
compare_asks(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvLinkAsk *x = *(const AxisTvLinkAsk *const *)a;
	const AxisTvLinkAsk *y = *(const AxisTvLinkAsk *const *)b;

	return x->file_id < y->file_id ? -1 : x->file_id > y->file_id ? 1 : 0;
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	LinksWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *file = htmx_request_get_query_param(request, "file");
	const gchar *done = htmx_request_get_query_param(request, "done");
	const gchar *page = htmx_request_get_query_param(request, "page");
	guint64 file_id = 0;
	guint64 page_no = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (file != NULL && !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL))
		return axis_web_error_respond(request, 400, "Not shown", "That is not a file.");
	if (page != NULL && !g_ascii_string_to_unsigned(page, 10, 0, 1000000, &page_no, NULL))
		return axis_web_error_respond(request, 400, "Not shown", "That is not a page.");
	work = g_new0(LinksWork, 1);
	work->series_id = id;
	work->file_id = (gint64)file_id;
	work->page = (guint)page_no;
	work->done = g_strdup(done);
	return axis_tv_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
}

static HtmxResponse *
handle_change(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	LinksWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *step = htmx_request_get_form_value(request, "step");
	const gchar *digest = htmx_request_get_form_value(request, "digest");
	gboolean apply = g_strcmp0(step, "apply") == 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if ((!apply && g_strcmp0(step, "review") != 0) || (apply && !digest_valid(digest)))
		return axis_web_error_respond(request, 400, "Not changed", "The form is not complete: open the page again.");
	work = g_new0(LinksWork, 1);
	work->series_id = id;
	work->review = !apply;
	work->apply = apply;
	work->digest = g_strdup(digest);
	work->user = g_strdup(axis_web_request_get_username(request));
	if (!read_plan(request, work)) {
		links_work_free(work);
		return axis_web_error_respond(request, 400, "Not changed", "That is too many files for one change: change "
			"fewer at a time.");
	}
	g_ptr_array_sort(work->asks, compare_asks);
	return axis_tv_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
}

static HtmxResponse *
handle_manage(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	LinksWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *file = htmx_request_get_form_value(request, "file");
	const gchar *step = htmx_request_get_form_value(request, "step");
	const gchar *digest = htmx_request_get_form_value(request, "digest");
	gboolean apply = g_strcmp0(step, "apply") == 0;
	guint64 file_id = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such series.");
	if (file == NULL || !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL))
		return axis_web_error_respond(request, 400, "Not changed", "Say which file.");
	if ((!apply && g_strcmp0(step, "review") != 0) || (apply && !digest_valid(digest)))
		return axis_web_error_respond(request, 400, "Not changed", "The form is not complete: open the page again.");
	work = g_new0(LinksWork, 1);
	work->series_id = id;
	work->file_id = (gint64)file_id;
	work->manage = TRUE;
	work->manage_apply = apply;
	work->digest = g_strdup(digest);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_tv_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
}

void
axis_tv_web_register_links(
	AxisTv  *self,
	AxisWeb *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/series/:id/links", AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/links", AXIS_WEB_ROUTE_ADMIN, handle_change, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/series/:id/links/manage", AXIS_WEB_ROUTE_ADMIN, handle_manage, self);
}
