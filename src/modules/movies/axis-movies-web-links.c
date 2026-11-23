/*
 * axis-movies-web-links.c - a movie's links page: its files linked to it by
 * hand (add-force-grab-and-assign, slice C2; a series' is
 * src/modules/tv/axis-tv-web-links.c)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /films/:id/links[?file=N&page=P]
 *                                     every video file linked to the movie
 *                                     with how its link was made, one page of
 *                                     the video files in its reach with no
 *                                     link, and the file asked for wherever it
 *                                     lies in a library folder for movies;
 *                                     writes nothing
 *   POST /films/:id/links             step=review: the review of the plan,
 *                                     writes nothing; step=apply: the change
 *                                     (digest=, other_<file>=yes, the ticks
 *                                     the review shown)
 *   POST /films/:id/links/manage      file=, step=review: what "Let Axis
 *                                     manage this file again" would do;
 *                                     step=apply (digest=): it does it
 *
 * All an administrator's (AXIS_WEB_ROUTE_ADMIN, and the shell's CSRF check
 * for the posts).  A plan is posted as one field for each file:
 * to_<library file id>=yes (link it to this movie) or unlink_<id>=yes.  The
 * review holds the plan again as hidden fields with its digest; a
 * confirmation made after what it showed changed, or while an import of the
 * movie is open or a grab of it is in flight, changes nothing and shows the
 * review again with status 409.  File names are set as text.
 */

#include "modules/movies/axis-movies-web.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-links.h"
#include "web/axis-web-storage-private.h"
#include "web/axis-web-table.h"
#include "web/axis-web-text.h"

#define ADD axis_web_storage_add

typedef struct
{
	gint64          movie_id;
	gint64          file_id;	/* the page: a file to list wherever it is; manage: the file */
	gboolean        review;
	gboolean        apply;
	gboolean        manage;		/* manage: step=review, or (manage_apply) step=apply */
	gboolean        manage_apply;
	guint           page;		/* the page: which page of the files with no link */
	GPtrArray      *asks;		/* AxisMovieLinkAsk */
	GArray         *others;		/* gint64 */
	gchar          *digest;
	gchar          *user;
	gchar          *done;
	AxisMovieLinks *links;
	AxisMovieLinkManage *handed;
	gboolean        changed;
	gboolean        refused;
	gboolean        stale;		/* a file went to another movie meanwhile: the page again */
	gchar          *stale_text;
	GError         *error;
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
	axis_movie_links_free(work->links);
	axis_movie_link_manage_free(work->handed);
	g_clear_error(&work->error);
	g_free(work);
}

static void
links_run(
	AxisMovies *self,
	gpointer    data
){
	LinksWork *work = data;
	g_autoptr(GError) local = NULL;

	if (work->manage && work->manage_apply) {
		if (axis_movies_links_manage(self, work->movie_id, work->file_id, work->digest, work->user, &work->handed,
		        &work->changed, &local))
			return;
		/* Refused for now: the confirmation again, which says why. */
		if (work->handed != NULL && g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT)) {
			work->refused = TRUE;
			return;
		}
		g_propagate_error(&work->error, g_steal_pointer(&local));
		return;
	}
	if (work->manage) {
		axis_movies_links_manage_review(self, work->movie_id, work->file_id, &work->handed, &work->error);
		return;
	}
	if (work->apply) {
		if (axis_movies_links_apply(self, work->movie_id, work->asks, work->others, work->digest, work->user,
		        &work->links, &work->changed, &local))
			return;
		/* Refused for now: the review again, which says why. */
		if (work->links != NULL && g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT)) {
			work->refused = TRUE;
			return;
		}
		/* A file went to another movie meanwhile, so the plan no longer
		 * holds: the links page again, as it is now. */
		if (g_error_matches(local, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_IN_FLIGHT) &&
		    axis_movies_links_read(self, work->movie_id, NULL, 0, &work->links, NULL)) {
			work->stale = TRUE;
			work->stale_text = g_strdup(local->message);
			return;
		}
		g_propagate_error(&work->error, g_steal_pointer(&local));
		return;
	}
	if (work->review) {
		axis_movies_links_review(self, work->movie_id, work->asks, work->others, &work->links, &work->error);
		return;
	}
	{
		g_autoptr(GArray) also = g_array_new(FALSE, FALSE, sizeof(gint64));

		if (work->file_id > 0)
			g_array_append_val(also, work->file_id);
		axis_movies_links_read(self, work->movie_id, also, work->page, &work->links, &work->error);
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

static gchar *
movie_label(const AxisMovieFacts *movie)
{
	return axis_movies_title_label(movie->title, movie->year);
}

static const AxisWebColumn file_columns[] = {
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

/* The choice for one file: Unlink (a linked one), or Link to this movie. */
static HtmxNode *
choice(const AxisMovieLinkFile *file)
{
	HtmxDiv *controls = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(controls), "film-actions");
	if (file->linked) {
		g_autofree gchar *id = g_strdup_printf("unlink-%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *name = g_strdup_printf(AXIS_WEB_LINKS_UNLINK "%" G_GINT64_FORMAT, file->file_id);

		ADD(controls, axis_web_storage_checkbox(id, "Unlink", name, FALSE));
	} else {
		g_autofree gchar *id = g_strdup_printf("to-%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *name = g_strdup_printf(AXIS_WEB_LINKS_TO "%" G_GINT64_FORMAT, file->file_id);
		g_autofree gchar *label = file->other != NULL ?
			g_strdup_printf("Link to this movie (it leaves %s)", file->other) : g_strdup("Link to this movie");

		ADD(controls, axis_web_storage_checkbox(id, label, name, FALSE));
	}
	return HTMX_NODE(controls);
}

static HtmxResponse *
links_page(
	HtmxRequest     *request,
	const LinksWork *work
){
	const AxisMovieLinks *links = work->links;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = movie_label(links->movie);
	g_autofree gchar *title = g_strdup_printf("Files and links: %s", label);
	g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", links->movie->id);
	g_autofree gchar *manage = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/manage", links->movie->id);
	g_autofree gchar *back = g_strdup_printf("/title/%" G_GINT64_FORMAT, links->movie->title_id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxNode *form;
	HtmxNode *linked_table = axis_web_table_new("film-links", file_columns);
	HtmxNode *free_table = axis_web_table_new("film-links-free", file_columns);
	HtmxNode *hand_table = axis_web_table_new("film-links-hand", hand_columns);
	HtmxTbody *linked_body = htmx_tbody_new();
	HtmxTbody *free_body = htmx_tbody_new();
	HtmxTbody *hand_body = htmx_tbody_new();
	guint n_linked = 0;
	guint n_free = 0;
	guint n_hand = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Say which video files are this movie's. Only links change: no file on disk is "
		"moved, renamed or deleted. A media server reads a file's name, so it still shows a file as what its name says. "
		"Nothing changes until you confirm on the next page."));
	if (g_strcmp0(work->done, "linked") == 0)
		ADD(content, axis_web_auth_notice(FALSE, "The links were changed. No file on disk was touched. Each link "
			"made is marked as made by hand and kept by hand: Axis does not move, replace or upgrade it by itself. The "
			"movie's history says what changed."));
	else if (g_strcmp0(work->done, "managed") == 0)
		ADD(content, axis_web_auth_notice(FALSE, "Axis manages that file again: identification, upgrades and imports "
			"treat it by the ordinary rules."));
	if (work->stale) {
		g_autofree gchar *text = g_strdup_printf("A file changed while you were reviewing, so nothing was changed: %s. Below "
			"are the links as they are now.", work->stale_text != NULL ? work->stale_text : "a file moved to another movie");

		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	if (links->busy != NULL) {
		g_autofree gchar *text = g_strdup_printf("No link can be changed now. %s", links->busy);

		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	if (work->file_id > 0) {
		/* The file asked for, when it cannot be listed: said, not passed over. */
		gboolean listed = FALSE;

		for (i = 0; i < links->files->len && !listed; i++)
			listed = ((const AxisMovieLinkFile *)g_ptr_array_index(links->files, i))->file_id == work->file_id;
		if (!listed) {
			g_autofree gchar *text = g_strdup_printf("File %" G_GINT64_FORMAT " is not a video file that is present in a "
				"library folder for movies, so it is not listed.", work->file_id);

			ADD(content, axis_web_storage_text("notice notice-warning", text));
		}
	}
	if (links->doubles != NULL) {
		HtmxNode *card = axis_web_storage_card("More than one file");

		htmx_element_set_id(HTMX_ELEMENT(card), "doubles");
		ADD(card, axis_web_storage_text("notice notice-warning", links->doubles));
		ADD(card, axis_web_storage_text("help", "All stay linked until you unlink one below. A movie with more than one "
			"file is judged for upgrades by its best."));
		ADD(content, card);
	}
	for (i = 0; i < links->files->len; i++) {
		const AxisMovieLinkFile *file = g_ptr_array_index(links->files, i);
		g_autofree gchar *anchor = g_strdup_printf("file-%" G_GINT64_FORMAT, file->file_id);
		HtmxTr *row = htmx_tr_new();

		htmx_element_set_id(HTMX_ELEMENT(row), anchor);
		ADD(row, cell_node(axis_web_breakable(file->path)));
		ADD(row, cell_text(file->how));
		ADD(row, cell_node(choice(file)));
		if (file->linked) {
			ADD(linked_body, row);
			n_linked++;
		} else {
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
			ADD(card, axis_web_storage_text("help", "No video file is linked to this movie."));
			g_object_unref(linked_table);
		} else {
			ADD(card, axis_web_storage_text("help", "Tick Unlink to take a file off this movie. The file stays on disk, "
				"and identification does not link it again."));
			ADD(card, linked_table);
		}
		ADD(form, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Files with no link to this movie");

		htmx_element_set_id(HTMX_ELEMENT(card), "unlinked");
		if (n_free == 0) {
			ADD(card, axis_web_storage_text("help", "No other video file is listed: none in this movie's folder is "
				"without a link. To link a file from elsewhere in the library, show it with the form below."));
			g_object_unref(free_table);
		} else {
			ADD(card, axis_web_storage_text("help", "Video files in this movie's folder that have no link, files you "
				"unlinked from it, and the file you asked to see. A file unlinked by hand is left alone by identification "
				"until you link it or let Axis manage it again."));
			ADD(card, free_table);
		}
		if (links->free_total > AXIS_MOVIES_LINK_PAGE) {
			g_autofree gchar *shown = g_strdup_printf("Files %u to %u of %u. Each page lists %d files.",
				links->page * AXIS_MOVIES_LINK_PAGE + 1, MIN((links->page + 1) * AXIS_MOVIES_LINK_PAGE,
				links->free_total), links->free_total, AXIS_MOVIES_LINK_PAGE);
			HtmxDiv *pages = htmx_div_new();

			htmx_element_set_id(HTMX_ELEMENT(pages), "unlinked-pages");
			htmx_element_add_class(HTMX_ELEMENT(pages), "film-actions");
			ADD(pages, axis_web_storage_text("help", shown));
			if (links->page > 0) {
				g_autofree gchar *href = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links?page=%u", links->movie->id,
					links->page - 1);

				ADD(pages, axis_web_browser_link(href, "Earlier files"));
			}
			if ((links->page + 1) * AXIS_MOVIES_LINK_PAGE < links->free_total) {
				g_autofree gchar *href = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links?page=%u", links->movie->id,
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
		ADD(actions, axis_web_browser_link(back, "Back to the movie"));
		ADD(form, actions);
	}
	ADD(content, form);
	{
		/* A file from elsewhere: one identification does not match, or one
		 * linked to another movie.  A plain GET of this page. */
		HtmxNode *card = axis_web_storage_card("A file from elsewhere in the library");
		HtmxForm *show = htmx_form_new();

		htmx_element_set_id(HTMX_ELEMENT(card), "elsewhere");
		htmx_element_set_attribute(HTMX_ELEMENT(show), "action", action);
		htmx_element_set_attribute(HTMX_ELEMENT(show), "method", "get");
		ADD(show, axis_web_storage_text_field("links-file", "The file's number", "file", NULL, "123",
			"To link a video file that is not listed above (one Axis could not match, or one linked to another movie), "
			"open the file in the Library and take the number at the end of its address (/library/file/123). The file "
			"is then listed above, where you can tick it."));
		ADD(show, axis_web_auth_button("Show this file", "button-small"));
		ADD(card, show);
		ADD(content, card);
	}
	if (n_hand > 0) {
		HtmxNode *card = axis_web_storage_card("Linked, kept or unlinked by hand");

		htmx_element_set_id(HTMX_ELEMENT(card), "by-hand");
		ADD(card, axis_web_storage_text("help", "Axis leaves these files as a person left them: identification and "
			"imports do not move, replace or link them, and no upgrade is searched for. Letting Axis manage a file again "
			"takes the mark of a person off that one file; the next page says what follows before anything changes."));
		ADD(card, hand_table);
		ADD(content, card);
	} else {
		g_object_unref(hand_table);
	}
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static const AxisWebColumn change_columns[] = {
	{ "File", AXIS_WEB_COL_WIDE },
	{ "Linked to now", AXIS_WEB_COL_TEXT },
	{ "Linked to after", AXIS_WEB_COL_TEXT },
	{ NULL, 0 }
};

static const AxisWebColumn touched_columns[] = {
	{ "Movie", AXIS_WEB_COL_TEXT },
	{ "Files now", AXIS_WEB_COL_WIDE },
	{ "Files after", AXIS_WEB_COL_WIDE },
	{ "What changes", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* The plan again, as hidden fields of @form. */
static void
plan_hidden(
	const LinksWork *work,
	HtmxNode        *form
){
	guint i;

	for (i = 0; work->asks != NULL && i < work->asks->len; i++) {
		const AxisMovieLinkAsk *ask = g_ptr_array_index(work->asks, i);
		g_autofree gchar *name = g_strdup_printf("%s%" G_GINT64_FORMAT, ask->unlink ? AXIS_WEB_LINKS_UNLINK :
			AXIS_WEB_LINKS_TO, ask->file_id);

		ADD(form, axis_web_auth_hidden(name, "yes"));
	}
}

static HtmxResponse *
review_page(
	HtmxRequest     *request,
	const LinksWork *work
){
	const AxisMovieLinks *links = work->links;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = movie_label(links->movie);
	g_autofree gchar *title = g_strdup_printf("Review the change of links: %s", label);
	g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", links->movie->id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxResponse *response;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("Only links change: no file on disk is moved, renamed or deleted. Nothing changes "
		"until you confirm below."));
	if (work->changed)
		ADD(content, axis_web_storage_text("notice notice-warning", "The movie changed after this review was made (a "
			"file arrived, a link moved, or a download started), so nothing was changed. Below is what the change would "
			"do now: confirm it again if it is right."));
	if (links->refused != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", links->refused));
	{
		HtmxNode *card = axis_web_storage_card("Files");
		HtmxNode *table = axis_web_table_new("links-review-files", change_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < links->changes->len; i++) {
			const AxisMovieLinkChange *change = g_ptr_array_index(links->changes, i);
			HtmxTr *row = htmx_tr_new();
			HtmxTd *file = htmx_td_new();

			ADD(file, axis_web_breakable(change->path));
			if (change->note != NULL)
				ADD(file, axis_web_storage_text("notice notice-warning", change->note));
			ADD(row, file);
			ADD(row, cell_text(change->before));
			ADD(row, cell_text(g_strcmp0(change->after, "No movie") == 0 ? "No movie: unlinked, and left alone by "
				"identification until a person links it or lets Axis manage it again" : change->after));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, axis_web_storage_text("help", "Each link made is marked as made by hand and kept by hand."));
		ADD(card, table);
		ADD(content, card);
	}
	{
		HtmxNode *card = axis_web_storage_card("Movies");
		HtmxNode *table = axis_web_table_new("links-review-movies", touched_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < links->touched->len; i++) {
			const AxisMovieLinkTouched *touched = g_ptr_array_index(links->touched, i);
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
			plan_hidden(work, ticks);
			for (i = 0; i < links->offers->len; i++) {
				const AxisMovieLinkOffer *offer = g_ptr_array_index(links->offers, i);
				g_autofree gchar *id = g_strdup_printf("other-%" G_GINT64_FORMAT, offer->file_id);
				g_autofree gchar *name = g_strdup_printf(AXIS_WEB_LINKS_OTHER "%" G_GINT64_FORMAT, offer->file_id);
				g_autofree gchar *text = g_strdup_printf("Unlink %s from %s instead. It stays on disk.", offer->path,
					label);

				ADD(ticks, axis_web_links_tick_box(id, text, name, offer->ticked));
			}
			ADD(ticks, axis_web_storage_text("help", "Without a tick, the movie's present file stays linked beside the new "
				"one. After you tick, show the review again: it then lists every file the ticks touch, before and after."));
			ADD(ticks, axis_web_auth_button("Show the review with these ticks", "button-small"));
			ADD(card, ticks);
		}
		if (links->refused == NULL) {
			HtmxNode *form = axis_web_auth_post_form(request, action);

			htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
			ADD(form, axis_web_auth_hidden("step", "apply"));
			ADD(form, axis_web_auth_hidden("digest", links->digest));
			plan_hidden(work, form);
			/* The ticks the review above shows, and only those. */
			for (i = 0; i < links->offers->len; i++) {
				const AxisMovieLinkOffer *offer = g_ptr_array_index(links->offers, i);
				g_autofree gchar *name = g_strdup_printf(AXIS_WEB_LINKS_OTHER "%" G_GINT64_FORMAT, offer->file_id);

				if (offer->ticked)
					ADD(form, axis_web_auth_hidden(name, "yes"));
			}
			ADD(actions, axis_web_auth_button("Change the links", "button-primary"));
			ADD(actions, axis_web_browser_link(action, "Keep them as they are"));
			ADD(form, actions);
			ADD(card, form);
		} else {
			ADD(card, axis_web_storage_text("help", "The change cannot be made now: the notice at the top says why."));
			ADD(actions, axis_web_browser_link(action, "Back to the links"));
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
	const AxisMovieLinkManage *handed = work->handed;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = movie_label(handed->movie);
	g_autofree gchar *title = g_strdup_printf("Let Axis manage this file again: %s", label);
	g_autofree gchar *action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/manage", handed->movie->id);
	g_autofree gchar *back = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", handed->movie->id);
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, handed->file_id);
	HtmxHeading *h1 = htmx_h1_new();
	HtmxResponse *response;
	HtmxNode *card = axis_web_storage_card("What happens");

	htmx_node_set_text_content(HTMX_NODE(h1), title);
	ADD(content, h1);
	ADD(content, axis_web_page_intro("No file on disk is moved, renamed or deleted. Nothing changes until you confirm "
		"below."));
	if (work->changed)
		ADD(content, axis_web_storage_text("notice notice-warning", "The movie changed after this was shown, so nothing was "
			"changed. Below is what handing the file back would do now: confirm it again if it is right."));
	if (handed->refused != NULL)
		ADD(content, axis_web_storage_text("notice notice-warning", handed->refused));
	htmx_element_set_id(HTMX_ELEMENT(card), "manage");
	ADD(card, axis_web_breakable(handed->path));
	ADD(card, axis_web_storage_text("help", handed->outcome));
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
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	LinksWork *work = data;

	if (work->error != NULL) {
		g_autofree gchar *message = NULL;

		if (work->error->domain != AXIS_MOVIES_ERROR)
			return axis_web_internal_error(request, work->error);
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, work->error->code == AXIS_MOVIES_ERROR_NOT_FOUND ? 404 :
			work->error->code == AXIS_MOVIES_ERROR_INVALID ? 400 : 409, "Not changed", message);
	}
	if (work->manage && work->manage_apply && !work->changed && !work->refused) {
		g_autofree gchar *location = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links?done=managed", work->movie_id);

		axis_web_storage_audit(request, "movie %" G_GINT64_FORMAT " file %" G_GINT64_FORMAT " managed by Axis again",
			work->movie_id, work->file_id);
		return axis_web_auth_redirect(request, location);
	}
	if (work->manage)
		return manage_page(request, work);
	if (work->apply && !work->changed && !work->refused) {
		g_autofree gchar *location = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links?done=linked", work->movie_id);

		axis_web_storage_audit(request, "movie %" G_GINT64_FORMAT ": links of %u files changed by hand", work->movie_id,
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

	work->asks = g_ptr_array_new_with_free_func(g_free);
	work->others = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (fields == NULL)
		return TRUE;
	g_hash_table_iter_init(&iter, fields);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		gint64 file_id = 0;
		gboolean unlink = axis_web_links_field_file(key, AXIS_WEB_LINKS_UNLINK, &file_id);
		g_autofree gchar *text = value != NULL ? g_strstrip(g_strdup(value)) : NULL;
		AxisMovieLinkAsk *ask;

		if (g_strcmp0(text, "yes") != 0)
			continue;
		if (axis_web_links_field_file(key, AXIS_WEB_LINKS_OTHER, &file_id)) {
			if (work->others->len < AXIS_WEB_LINKS_PLAN_MAX)
				g_array_append_val(work->others, file_id);
			continue;
		}
		if (!unlink && !axis_web_links_field_file(key, AXIS_WEB_LINKS_TO, &file_id))
			continue;
		if (work->asks->len >= AXIS_WEB_LINKS_PLAN_MAX)
			return FALSE;
		ask = g_new0(AxisMovieLinkAsk, 1);
		ask->file_id = file_id;
		ask->unlink = unlink;
		g_ptr_array_add(work->asks, ask);
	}
	/* (Unlink and Link ticked for one file are two asks: the store refuses
	 * the file named twice.) */
	return TRUE;
}

static gint
compare_asks(
	gconstpointer a,
	gconstpointer b
){
	const AxisMovieLinkAsk *x = *(const AxisMovieLinkAsk *const *)a;
	const AxisMovieLinkAsk *y = *(const AxisMovieLinkAsk *const *)b;

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
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	if (file != NULL && file[0] != '\0' && !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL))
		return axis_web_error_respond(request, 400, "Not shown", "That is not a file's number.");
	if (page != NULL && !g_ascii_string_to_unsigned(page, 10, 0, 1000000, &page_no, NULL))
		return axis_web_error_respond(request, 400, "Not shown", "That is not a page.");
	work = g_new0(LinksWork, 1);
	work->movie_id = id;
	work->file_id = (gint64)file_id;
	work->page = (guint)page_no;
	work->done = g_strdup(done);
	return axis_movies_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
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
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	if ((!apply && g_strcmp0(step, "review") != 0) || (apply && !axis_web_links_digest_valid(digest)))
		return axis_web_error_respond(request, 400, "Not changed", "The form is not complete: open the page again.");
	work = g_new0(LinksWork, 1);
	work->movie_id = id;
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
	return axis_movies_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
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
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	if (file == NULL || !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL))
		return axis_web_error_respond(request, 400, "Not changed", "Say which file.");
	if ((!apply && g_strcmp0(step, "review") != 0) || (apply && !axis_web_links_digest_valid(digest)))
		return axis_web_error_respond(request, 400, "Not changed", "The form is not complete: open the page again.");
	work = g_new0(LinksWork, 1);
	work->movie_id = id;
	work->file_id = (gint64)file_id;
	work->manage = TRUE;
	work->manage_apply = apply;
	work->digest = g_strdup(digest);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_movies_web_dispatch(user_data, request, links_run, links_done, work, links_work_free);
}

void
axis_movies_web_register_links(
	AxisMovies *self,
	AxisWeb    *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/films/:id/links", AXIS_WEB_ROUTE_ADMIN, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/links", AXIS_WEB_ROUTE_ADMIN, handle_change, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/links/manage", AXIS_WEB_ROUTE_ADMIN, handle_manage, self);
}
