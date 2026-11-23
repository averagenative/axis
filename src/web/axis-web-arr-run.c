/*
 * axis-web-arr-run.c - "Migrate from Radarr": what a connection migrated
 * (the card on its page and the list), and starting the searches of the
 * movies migrated
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only; a post carries the CSRF token (the routes').  The
 * confirmation of "Start searching" is a GET that reads Radarr's download
 * queue (Axis only ever sends Radarr GET requests) and changes nothing; its
 * post reads the queue again and then ends the holds.
 */

#include "web/axis-web-arr-private.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

static gchar *
movies_text(guint n)
{
	g_autofree gchar *count = axis_web_format_count(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "movie" : "movies");
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
}

static gchar *
series_text(guint n)
{
	g_autofree gchar *count = axis_web_format_count(n);

	return g_strdup_printf("%s series", count);
}

static gchar *
episodes_text(guint n)
{
	g_autofree gchar *count = axis_web_format_count(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "episode" : "episodes");
}

static gchar *
files_text(guint n)
{
	g_autofree gchar *count = axis_web_format_count(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "file" : "files");
}

/* ── The card on the connection's page ──────────────────────────── */

/* What the last run said about starting the searches at its end (the tick of
 * its confirmation), under the card's first line. */
static void
add_run_note(
	HtmxNode         *card,
	const AxisArrRun *run
){
	if (run->run_note != NULL && run->run_note[0] != '\0')
		ADD(card, axis_web_storage_text("arr-run-note", run->run_note));
}

/* The files migrated are kept; the way to let Axis upgrade them. */
static void
add_kept(
	HtmxNode            *card,
	const AxisArrSource *source,
	const AxisArrRun    *run
){
	g_autofree gchar *release = axis_web_arr_path(source->id, "/release");
	g_autofree gchar *files = files_text(run->n_kept);
	g_autofree gchar *text = g_strdup_printf("%s migrated from %s %s kept: Axis does not upgrade or replace %s, "
		"whatever the quality profile says.", files, axis_arr_app_label(source->app), run->n_kept == 1 ? "is" : "are",
		run->n_kept == 1 ? "it" : "them");
	HtmxP *go;
	HtmxA *link;

	if (run->n_unkept > 0) {
		g_autofree gchar *unkept = axis_web_format_count(run->n_unkept);
		g_autofree gchar *preview = axis_web_arr_path(source->id, "/preview");
		g_autofree gchar *warn = g_strdup_printf("%s migrated %s %s not kept yet. Run the migration again to keep %s.",
			unkept, run->n_unkept == 1 ? "file" : "files", run->n_unkept == 1 ? "is" : "are",
			run->n_unkept == 1 ? "it" : "them");
		HtmxP *again = htmx_p_new();

		ADD(card, axis_web_storage_text("arr-unkept", warn));
		ADD(again, axis_web_browser_link(preview, "Open the preview"));
		ADD(card, again);
	}
	if (run->n_kept == 0)
		return;
	go = htmx_p_new();
	link = htmx_a_new_with_href(release);
	ADD(card, axis_web_storage_text("arr-kept", text));
	htmx_element_add_class(HTMX_ELEMENT(link), "button");
	htmx_node_set_text_content(HTMX_NODE(link), "Let Axis upgrade these files");
	ADD(go, link);
	ADD(card, go);
	ADD(card, axis_web_storage_text("help", "This opens a page that says how many files it lets go and what happens to "
		"them then. Nothing changes until you confirm there. One file can also be let go alone, on its title's page."));
}

/* A Sonarr's: what it brought in its own words (series, and the episodes
 * held apart, which outlast "Start searching"). */
static HtmxNode *
series_brought_card(
	const AxisArrSource *source,
	const AxisArrRun    *run
){
	const gchar *app = axis_arr_app_label(source->app);
	g_autofree gchar *list = axis_web_arr_path(source->id, "/brought");
	g_autofree gchar *unlinked = axis_web_arr_path(source->id, "/brought?unlinked=yes");
	g_autofree gchar *start = axis_web_arr_path(source->id, "/start");
	g_autofree gchar *brought = series_text(run->n_brought);
	g_autofree gchar *files = axis_web_format_count(run->n_linked);
	g_autofree gchar *line = g_strdup_printf("%s migrated from %s, %s episode %s linked.", brought, app, files,
		run->n_linked == 1 ? "file" : "files");
	HtmxNode *card = axis_web_storage_card("Migrated");
	HtmxP *links = htmx_p_new();

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-brought");
	if (g_strcmp0(run->last_state, "failed") == 0 || g_strcmp0(run->last_state, "cancelled") == 0)
		ADD(card, axis_web_browser_warning(g_strcmp0(run->last_state, "cancelled") == 0 ?
			"The last run was stopped before it finished. What it migrated stays, each series whole. Open the preview "
			"and confirm again to continue: nothing is migrated twice." :
			"The last run did not finish. What it migrated stays, each series whole. Open the preview and confirm again "
			"to continue: nothing is migrated twice. The Jobs page says why it stopped."));
	ADD(card, axis_web_storage_text(NULL, line));
	add_run_note(card, run);
	if (run->n_gone > 0) {
		g_autofree gchar *gone = series_text(run->n_gone);
		g_autofree gchar *text = g_strdup_printf("%s of them %s removed in Axis since. A later run does not migrate %s "
			"again.", gone, run->n_gone == 1 ? "was" : "were", run->n_gone == 1 ? "it" : "them");

		ADD(card, axis_web_storage_text("help", text));
	}
	ADD(links, axis_web_browser_link(list, "See what was migrated"));
	ADD(card, links);
	/* The held, in the numbers the notice of "Needs attention" states: a
	 * series with a file for every monitored episode needs nothing now; only
	 * those with a monitored episode without one wait. */
	if (run->n_with_file > 0) {
		g_autofree gchar *have = series_text(run->n_with_file);
		g_autofree gchar *text = g_strdup_printf("%s %s no monitored episode without a file. Episode files that were "
			"migrated are kept, and Axis does not replace them: %s nothing from you now. Axis does not search for %s "
			"new episodes until you start searching.", have, run->n_with_file == 1 ? "has" : "have",
			run->n_with_file == 1 ? "this series needs" : "these series need", run->n_with_file == 1 ? "its" : "their");

		ADD(card, axis_web_storage_text("arr-settled", text));
	}
	if (run->n_waiting > 0) {
		g_autofree gchar *waiting = series_text(run->n_waiting);
		g_autofree gchar *text = g_strdup_printf("%s %s monitored episodes without a file. Axis does not search for "
			"them until you start searching.", waiting, run->n_waiting == 1 ? "has" : "have");

		ADD(card, axis_web_storage_text("arr-waiting", text));
	} else if (run->n_held > run->n_downloading) {
		ADD(card, axis_web_storage_text("arr-waiting", "No migrated series waits for you: none that is still held has "
			"a monitored episode without a file, so starting searching would search for nothing now."));
	} else if (run->n_held == 0 && run->n_brought > 0) {
		ADD(card, axis_web_storage_text(NULL, "None of them is held: Axis searches for them by itself."));
	}
	if (run->n_unseen_episodes > 0) {
		g_autofree gchar *unseen = episodes_text(run->n_unseen_episodes);
		g_autofree gchar *text = g_strdup_printf("%s %s held apart: %s has a file for %s that Axis does not see. %s "
			"held when you start the series, until a later run finds the file or you search for the episode, or let "
			"Axis search for its series, on the series' own page.", unseen, run->n_unseen_episodes == 1 ? "is" : "are",
			app, run->n_unseen_episodes == 1 ? "it" : "each", run->n_unseen_episodes == 1 ? "It stays" : "They stay");
		HtmxP *see = htmx_p_new();

		ADD(card, axis_web_storage_text(NULL, text));
		ADD(see, axis_web_browser_link(unlinked, "See the series with files that were not linked"));
		ADD(card, see);
	}
	if (run->n_downloading > 0) {
		g_autofree gchar *waiting = series_text(run->n_downloading);
		g_autofree gchar *text = g_strdup_printf("%s %s left out when searching was started: %s was downloading for "
			"%s. Start searching again once %s has finished.", waiting, run->n_downloading == 1 ? "was" : "were", app,
			run->n_downloading == 1 ? "it" : "them", app);

		ADD(card, axis_web_storage_text(NULL, text));
	}
	if (run->started_us > 0) {
		g_autofree gchar *when = axis_web_format_time(run->started_us, "");
		g_autofree gchar *text = g_strdup_printf("Searching was last started on %s.", when);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (run->n_held > 0) {
		HtmxP *go = htmx_p_new();
		HtmxA *link = htmx_a_new_with_href(start);

		htmx_element_add_class(HTMX_ELEMENT(link), "button");
		if (run->n_waiting > 0)
			htmx_element_add_class(HTMX_ELEMENT(link), "button-primary");
		htmx_node_set_text_content(HTMX_NODE(link), "Start searching for the episodes without a file");
		ADD(go, link);
		ADD(card, go);
		ADD(card, axis_web_storage_text("help", "Starting searching affects only the episodes without a file: the "
			"episode files that were migrated are kept and are not replaced. This opens a page that says what would be "
			"started and checks what Sonarr is downloading. Nothing starts until you confirm there."));
	}
	add_kept(card, source, run);
	return card;
}

static HtmxNode *
movies_brought_card(
	const AxisArrSource *source,
	const AxisArrRun    *run
);

HtmxNode *
axis_web_arr_brought_card(
	const AxisArrSource *source,
	const AxisArrRun    *run
){
	return source->app == AXIS_ARR_SONARR ? series_brought_card(source, run) : movies_brought_card(source, run);
}

static HtmxNode *
movies_brought_card(
	const AxisArrSource *source,
	const AxisArrRun    *run
){
	const gchar *app = axis_arr_app_label(source->app);
	g_autofree gchar *list = axis_web_arr_path(source->id, "/brought");
	g_autofree gchar *unlinked = axis_web_arr_path(source->id, "/brought?unlinked=yes");
	g_autofree gchar *start = axis_web_arr_path(source->id, "/start");
	g_autofree gchar *brought = movies_text(run->n_brought);
	g_autofree gchar *files = axis_web_format_count(run->n_linked);
	g_autofree gchar *line = g_strdup_printf("%s migrated from %s, %s %s linked.", brought, app, files,
		run->n_linked == 1 ? "file" : "files");
	guint to_start = run->n_held - run->n_unseen;
	HtmxNode *card = axis_web_storage_card("Migrated");
	HtmxP *links = htmx_p_new();

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-brought");
	if (g_strcmp0(run->last_state, "failed") == 0 || g_strcmp0(run->last_state, "cancelled") == 0)
		ADD(card, axis_web_browser_warning(g_strcmp0(run->last_state, "cancelled") == 0 ?
			"The last run was stopped before it finished. What it migrated stays. Open the preview and confirm again to "
			"continue: nothing is migrated twice." :
			"The last run did not finish. What it migrated stays. Open the preview and confirm again to continue: "
			"nothing is migrated twice. The Jobs page says why it stopped."));
	ADD(card, axis_web_storage_text(NULL, line));
	add_run_note(card, run);
	if (run->n_gone > 0) {
		g_autofree gchar *gone = movies_text(run->n_gone);
		g_autofree gchar *text = g_strdup_printf("%s of them %s removed in Axis since. A later run does not migrate %s "
			"again.", gone, run->n_gone == 1 ? "was" : "were", run->n_gone == 1 ? "it" : "them");

		ADD(card, axis_web_storage_text("help", text));
	}
	ADD(links, axis_web_browser_link(list, "See what was migrated"));
	ADD(card, links);
	/* The held, in the numbers the notice of "Needs attention" states: those
	 * that have their file need nothing; only those without one wait. */
	if (run->n_with_file > 0) {
		g_autofree gchar *have = movies_text(run->n_with_file);
		g_autofree gchar *text = g_strdup_printf("%s %s. %s kept, and Axis does not replace %s: %s nothing from you.",
			have, run->n_with_file == 1 ? "has its file" : "have their files",
			run->n_with_file == 1 ? "The file is" : "The files are", run->n_with_file == 1 ? "it" : "them",
			run->n_with_file == 1 ? "this movie needs" : "these movies need");

		ADD(card, axis_web_storage_text("arr-settled", text));
	}
	if (run->n_waiting > 0) {
		g_autofree gchar *waiting = movies_text(run->n_waiting);
		g_autofree gchar *text = g_strdup_printf("%s %s. Axis does not search for %s until you start searching.", waiting,
			run->n_waiting == 1 ? "is monitored and has no file" : "are monitored and have no file",
			run->n_waiting == 1 ? "it" : "them");

		ADD(card, axis_web_storage_text("arr-waiting", text));
	} else if (to_start > run->n_downloading) {
		ADD(card, axis_web_storage_text("arr-waiting", "No migrated movie waits for you: none that is still held is "
			"monitored and without a file, so starting searching would search for nothing now."));
	} else if (run->n_held == 0 && run->n_brought > 0) {
		ADD(card, axis_web_storage_text(NULL, "None of them is held: Axis searches for them by itself."));
	}
	if (to_start > run->n_downloading + run->n_with_file + run->n_waiting) {
		g_autofree gchar *rest = movies_text(to_start - run->n_downloading - run->n_with_file - run->n_waiting);
		gboolean one = to_start - run->n_downloading - run->n_with_file - run->n_waiting == 1;
		g_autofree gchar *text = g_strdup_printf("%s %s no file and %s not monitored: Axis does not search for %s, "
			"before or after you start.", rest, one ? "has" : "have", one ? "is" : "are", one ? "it" : "them");

		ADD(card, axis_web_storage_text(NULL, text));
	}
	if (run->n_unseen > 0) {
		g_autofree gchar *unseen = movies_text(run->n_unseen);
		g_autofree gchar *text = g_strdup_printf("%s %s held apart: %s has a file for %s that Axis does not see. %s "
			"held when you start the others, until a later run finds the file or you start the movie on its own page.",
			unseen, run->n_unseen == 1 ? "is" : "are", app, run->n_unseen == 1 ? "it" : "them",
			run->n_unseen == 1 ? "It stays" : "They stay");
		HtmxP *see = htmx_p_new();

		ADD(card, axis_web_storage_text(NULL, text));
		ADD(see, axis_web_browser_link(unlinked, "See the files that were not linked"));
		ADD(card, see);
	}
	if (run->n_downloading > 0) {
		g_autofree gchar *waiting = movies_text(run->n_downloading);
		g_autofree gchar *text = g_strdup_printf("%s %s left out when searching was started: %s was downloading %s. "
			"Start searching again once %s has finished.", waiting, run->n_downloading == 1 ? "was" : "were", app,
			run->n_downloading == 1 ? "it" : "them", app);

		ADD(card, axis_web_storage_text(NULL, text));
	}
	if (run->started_us > 0) {
		g_autofree gchar *when = axis_web_format_time(run->started_us, "");
		g_autofree gchar *text = g_strdup_printf("Searching was last started on %s.", when);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (to_start > 0) {
		HtmxP *go = htmx_p_new();
		HtmxA *link = htmx_a_new_with_href(start);

		htmx_element_add_class(HTMX_ELEMENT(link), "button");
		/* The one thing to do here only while something waits for it. */
		if (run->n_waiting > 0)
			htmx_element_add_class(HTMX_ELEMENT(link), "button-primary");
		htmx_node_set_text_content(HTMX_NODE(link), "Start searching for the movies without a file");
		ADD(go, link);
		ADD(card, go);
		ADD(card, axis_web_storage_text("help", "Starting searching affects only the movies without a file: the files "
			"that were migrated are kept and are not replaced. This opens a page that says what would be started and "
			"checks what Radarr is downloading. Nothing starts until you confirm there."));
	}
	add_kept(card, source, run);
	return card;
}

/* ── The list of what was brought ───────────────────────────────── */

static const AxisWebColumn brought_columns[] = {
	{ "Movie", AXIS_WEB_COL_NAME },
	{ "Its file in Radarr", AXIS_WEB_COL_WIDE },
	{ "Searches", AXIS_WEB_COL_STATE },
	{ "Migrated", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static const AxisWebColumn series_brought_columns[] = {
	{ "Series", AXIS_WEB_COL_NAME },
	{ "Its episode files in Sonarr", AXIS_WEB_COL_WIDE },
	{ "Searches", AXIS_WEB_COL_STATE },
	{ "Migrated", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

typedef struct
{
	gint64         id;
	guint          page;
	gboolean       unlinked;
	AxisArrSource *source;
	GPtrArray     *rows;
	guint          total;
	GError        *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_clear_pointer(&work->source, axis_arr_source_free);
	g_clear_pointer(&work->rows, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebArr *self,
	gpointer    data
){
	ListWork *work = data;

	if (axis_arr_get_source(self->arr, work->id, &work->source, &work->error))
		axis_arr_list_brought(self->arr, work->id, work->unlinked, work->page, &work->rows, &work->total, &work->error);
}

static HtmxNode *
td_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxResponse *
list_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *title = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *back_text = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *only = NULL;
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card;
	HtmxP *filter = htmx_p_new();
	gboolean series;
	guint i;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	title = g_strdup_printf("Migrated: %s", work->source->name);
	back = axis_web_arr_path(work->id, NULL);
	back_text = g_strdup_printf("Back to %s", work->source->name);
	all = axis_web_arr_path(work->id, "/brought");
	only = axis_web_arr_path(work->id, "/brought?unlinked=yes");
	series = work->source->app == AXIS_ARR_SONARR;
	htmx_node_set_text_content(HTMX_NODE(heading), title);
	ADD(content, heading);
	ADD(content, axis_web_page_intro(series ? "Every series this connection migrated into Axis, with what became of "
		"the episode files Sonarr has for it, and how many episodes Sonarr has that Axis's numbering does not. No "
		"file was moved, renamed or deleted. To undo one, remove the series on its page: that touches no file either." :
		"Every movie this connection migrated into Axis, with what became of the file "
		"Radarr has for it. No file was moved, renamed or deleted. To undo one, remove the movie on its page: that "
		"touches no file either."));
	ADD(filter, axis_web_browser_link(work->unlinked ? all : only, work->unlinked ? (series ?
		"Show every migrated series" : "Show every migrated movie") : series ?
		"Show only those with a file that was not linked" : "Show only those whose file was not linked"));
	ADD(content, filter);
	card = axis_web_storage_card(work->unlinked ? "Files that were not linked" : series ? "Migrated series" :
		"Migrated movies");
	if (work->rows->len == 0) {
		ADD(card, axis_web_storage_text("help", work->unlinked ? (series ?
			"Every file Sonarr has for these series is linked." : "Every file Radarr has for these movies is linked.") :
			"Nothing has been migrated yet."));
	} else {
		HtmxNode *table = axis_web_table_new("arr-brought-list", series ? series_brought_columns : brought_columns);
		HtmxTbody *body = htmx_tbody_new();
		guint first = work->page * AXIS_ARR_PAGE_ROWS + 1;
		g_autofree gchar *from = axis_web_format_count(first);
		g_autofree gchar *to = axis_web_format_count(first + work->rows->len - 1);
		g_autofree gchar *of = axis_web_format_count(work->total);
		g_autofree gchar *range = g_strdup_printf("%s to %s of %s.", from, to, of);

		for (i = 0; i < work->rows->len; i++) {
			const AxisArrBroughtRow *row = g_ptr_array_index(work->rows, i);
			g_autofree gchar *file = series ? axis_arr_brought_counts_label(row->counts, work->source->app) :
				axis_arr_brought_file_label(row->file, row->file_detail, work->source->app);
			g_autofree gchar *when = axis_web_format_time(row->brought_us, "");
			g_autofree gchar *held = series && row->movie_id > 0 ? g_strdup_printf("%s%s%u %s held apart",
				row->hold == NULL ? "Started" : strcmp(row->hold, AXIS_TV_HOLD_DOWNLOADING) == 0 ?
				"Held: Sonarr was downloading for it" : "Held", row->held_episodes > 0 ? "; " : "",
				row->held_episodes, row->held_episodes == 1 ? "episode" : "episodes") : NULL;
			HtmxTr *tr = htmx_tr_new();
			HtmxTd *name = htmx_td_new();

			if (row->movie_id > 0) {
				g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, row->title_id);
				HtmxA *link = htmx_a_new_with_href(href);
				HtmxBdi *text = htmx_bdi_new();

				htmx_node_set_text_content(HTMX_NODE(text), row->label);
				ADD(link, text);
				ADD(name, link);
			} else {
				HtmxBdi *text = htmx_bdi_new();

				htmx_node_set_text_content(HTMX_NODE(text), row->label);
				ADD(name, text);
			}
			ADD(tr, name);
			ADD(tr, td_text(file));
			ADD(tr, td_text(row->movie_id <= 0 ? "Removed in Axis since" :
				series ? (row->held_episodes > 0 ? held : row->hold == NULL ? "Started" :
				strcmp(row->hold, AXIS_TV_HOLD_DOWNLOADING) == 0 ? "Held: Sonarr was downloading for it" : "Held") :
				row->hold == NULL ? "Started" :
				strcmp(row->hold, AXIS_MOVIE_HOLD_FILE_UNSEEN) == 0 ? "Held: its file is not seen" :
				strcmp(row->hold, AXIS_MOVIE_HOLD_DOWNLOADING) == 0 ? "Held: Radarr was downloading it" : "Held"));
			ADD(tr, td_text(when));
			ADD(body, tr);
		}
		ADD(table, body);
		ADD(card, table);
		ADD(card, axis_web_storage_text("help", range));
		if (work->page > 0 || first + work->rows->len - 1 < work->total) {
			HtmxP *pages = htmx_p_new();

			if (work->page > 0) {
				g_autofree gchar *rest = g_strdup_printf("/brought?page=%u%s", work->page - 1,
					work->unlinked ? "&unlinked=yes" : "");
				g_autofree gchar *href = axis_web_arr_path(work->id, rest);

				ADD(pages, axis_web_browser_link(href, "Previous page"));
			}
			if (first + work->rows->len - 1 < work->total) {
				g_autofree gchar *rest = g_strdup_printf("/brought?page=%u%s", work->page + 1,
					work->unlinked ? "&unlinked=yes" : "");
				g_autofree gchar *href = axis_web_arr_path(work->id, rest);

				ADD(pages, axis_web_browser_link(href, "Next page"));
			}
			ADD(card, pages);
		}
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, back_text));
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
handle_brought(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *page = htmx_request_get_query_param(request, "page");
	ListWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(ListWork, 1);
	work->id = id;
	work->unlinked = g_strcmp0(htmx_request_get_query_param(request, "unlinked"), "yes") == 0;
	if (page != NULL && g_ascii_isdigit(page[0]) && strlen(page) <= 5)
		work->page = (guint)g_ascii_strtoull(page, NULL, 10);
	return axis_web_arr_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

/* ── Start searching ────────────────────────────────────────────── */

typedef struct
{
	gint64        id;
	gboolean      posting;
	gboolean      stopped;
	gchar        *user;
	AxisArrStart *start;
	GError       *error;
} StartWork;

static void
start_work_free(gpointer data)
{
	StartWork *work = data;

	g_free(work->user);
	g_clear_pointer(&work->start, axis_arr_start_free);
	g_clear_error(&work->error);
	g_free(work);
}

static void
start_run(
	AxisWebArr *self,
	gpointer    data
){
	StartWork *work = data;

	if (work->posting)
		axis_arr_start_searching(self->arr, work->id, work->stopped, work->user, &work->start, &work->error);
	else
		axis_arr_start_review(self->arr, work->id, &work->start, &work->error);
}

/* "about 13 hours", "about 45 minutes". */
static gchar *
duration_text(guint64 seconds)
{
	if (seconds >= 2 * 86400)
		return g_strdup_printf("about %" G_GUINT64_FORMAT " days", (seconds + 43200) / 86400);
	if (seconds >= 2 * 3600)
		return g_strdup_printf("about %" G_GUINT64_FORMAT " hours", (seconds + 1800) / 3600);
	return g_strdup_printf("about %" G_GUINT64_FORMAT " minutes", MAX((seconds + 30) / 60, 1));
}

/* "Start searching for the migrated series": a Sonarr's confirmation, in
 * its words.  The same checks as a Radarr's: Sonarr's queue now, what Sonarr
 * still monitors, and the tick when Sonarr does not answer. */
static HtmxResponse *
series_start_page(
	HtmxRequest        *request,
	const AxisArrStart *start,
	const gchar        *notice,
	guint               status
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const AxisArrSource *source = start->source;
	const gchar *app = axis_arr_app_label(source->app);
	g_autofree gchar *action = axis_web_arr_path(source->id, "/start");
	g_autofree gchar *back = axis_web_arr_path(source->id, NULL);
	g_autofree gchar *back_text = g_strdup_printf("Back to %s", source->name);
	g_autofree gchar *held = series_text(start->n_held);
	g_autofree gchar *missing = series_text(start->n_missing);
	g_autofree gchar *episodes = episodes_text(start->n_missing_episodes);
	/* The same numbers as the connection's card and the notice, less what
	 * Sonarr is downloading for now: only an episode without a file is
	 * searched for. */
	g_autofree gchar *would = g_strdup_printf("Starting searching affects only the episodes without a file. Of %s "
		"migrated from %s that %s still held, %s %s monitored episodes without a file (%s in all): Axis would search "
		"for them once they have aired. An episode whose file was migrated is not searched for: that file is kept "
		"and is not replaced.", held, app, start->n_held == 1 ? "is" : "are", missing,
		start->n_missing == 1 ? "has" : "have", episodes);
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What would be started");
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), "Start searching for the migrated series");
	ADD(content, heading);
	ADD(content, axis_web_page_intro("Until now Axis has searched for none of the series it migrated. This lets it "
		"search for their episodes without a file; an episode that has its file is left as it is. Nothing is searched "
		"for at once: the scheduler takes them at its ordinary pace."));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(TRUE, notice));
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-start");
	if (start->n_held + start->n_in_queue == 0) {
		ADD(card, axis_web_storage_text(NULL, "No series this connection migrated is held: there is nothing to start."));
		ADD(content, card);
		ADD(content, axis_web_browser_link(back, back_text));
		return axis_web_page_respond(request, "Start searching", HTMX_NODE(content));
	}
	ADD(card, axis_web_storage_text(NULL, would));
	ADD(card, axis_web_storage_text(NULL, "The episode files that were migrated from Sonarr are kept: Axis does not upgrade "
		"or replace them, before or after you start, until you release them with Let Axis upgrade these files on this "
		"connection's page, or one file at a time on its series' page."));
	if (start->per_run > 0 && start->interval_s > 0) {
		g_autofree gchar *pace = g_strdup_printf("The pace: at most %u %s every %u minutes, and at most %d of one series "
			"a run. A season with several wanted episodes is sought as one search first. The download client fills as "
			"it would on any day.", start->per_run, start->per_run == 1 ? "search" : "searches",
			start->interval_s / 60, AXIS_TV_PER_SERIES_RUN);

		ADD(card, axis_web_storage_text(NULL, pace));
	}
	if (start->n_unseen_episodes > 0) {
		g_autofree gchar *unseen = episodes_text(start->n_unseen_episodes);
		g_autofree gchar *text = g_strdup_printf("%s %s held and %s not started: %s has a file for %s that Axis does "
			"not see, and searching would fetch a second copy.", unseen,
			start->n_unseen_episodes == 1 ? "stays" : "stay", start->n_unseen_episodes == 1 ? "is" : "are", app,
			start->n_unseen_episodes == 1 ? "it" : "each");

		ADD(card, axis_web_storage_text(NULL, text));
	}
	ADD(content, card);

	card = axis_web_storage_card("Sonarr beside Axis");
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-beside");
	if (start->queue == AXIS_ARR_QUEUE_READ && start->n_in_queue > 0) {
		g_autofree gchar *n = axis_web_format_count(start->n_in_queue);
		g_autoptr(GString) names = g_string_new(NULL);
		g_autofree gchar *text = NULL;
		guint i;

		for (i = 0; i < start->in_queue->len; i++)
			g_string_append_printf(names, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(start->in_queue, i));
		if (start->n_in_queue > start->in_queue->len)
			g_string_append(names, "; and more");
		text = g_strdup_printf("%s is downloading for %s %s: left out, and held until you start searching again. %s.",
			app, start->n_in_queue == 1 ? "this" : "these", n, names->str);
		ADD(card, axis_web_storage_text(NULL, text));
	} else if (start->queue == AXIS_ARR_QUEUE_READ) {
		g_autofree gchar *text = g_strdup_printf("%s is downloading for none of these series now.", app);

		ADD(card, axis_web_storage_text(NULL, text));
	} else if (start->queue == AXIS_ARR_QUEUE_UNCLEAR) {
		g_autofree gchar *text = start->n_queue > 0 ?
			g_strdup_printf("%s answered and lists %u %s, but Axis could not tell which series they are for. Look at "
				"%s's own queue before you start: Axis would search for an episode %s is downloading.", app,
				start->n_queue, start->n_queue == 1 ? "download" : "downloads", app, app) :
			g_strdup_printf("%s answered, but Axis could not read its download queue. Look at %s's own queue before "
				"you start: Axis would search for an episode %s is downloading.", app, app, app);

		ADD(card, axis_web_browser_warning(text));
	} else {
		g_autofree gchar *text = g_strdup_printf("%s did not answer, so Axis could not check what it is downloading. "
			"If it is running, both would download the same episodes.", app);

		ADD(card, axis_web_browser_warning(text));
	}
	if (start->n_monitored > 0) {
		g_autofree gchar *n = axis_web_format_count((guint)start->n_monitored);
		g_autofree gchar *text = g_strdup_printf("%s still monitors %s of these series. Both would download their "
			"episodes. Unmonitor them in %s, or stop %s, before you start. Axis cannot do that for you: it only reads "
			"from %s.", app, n, app, app, app);

		ADD(card, axis_web_browser_warning(text));
	} else if (start->n_monitored < 0) {
		g_autofree gchar *text = g_strdup_printf("Axis does not know whether %s still monitors these series: its last "
			"reading is more than a day old. If it does, both would download their episodes. Read %s again to see.",
			app, app);

		ADD(card, axis_web_storage_text(NULL, text));
	} else {
		g_autofree gchar *text = g_strdup_printf("At the last reading %s monitored none of these series.", app);

		ADD(card, axis_web_storage_text(NULL, text));
	}
	ADD(content, card);

	card = axis_web_storage_card("Start");
	if (start->n_held == 0) {
		ADD(card, axis_web_storage_text(NULL, "Sonarr is downloading for every series that could be started: there is "
			"nothing to start. Come back once Sonarr has finished."));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, action);
		/* What it does for a person: the episodes without a file, of the
		 * series that have one.  With none, what is left of it: the end of the
		 * hold, which lets Axis fetch the episodes still to come. */
		g_autofree gchar *label = start->n_missing > 0 ?
			g_strdup_printf("Start searching for the episodes without a file of %s", missing) :
			g_strdup_printf("End the hold of %s", held);

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		if (start->queue == AXIS_ARR_QUEUE_NO_ANSWER)
			ADD(form, axis_web_storage_checkbox("arr-start-stopped", "Sonarr is stopped, or no longer monitors these "
				"series", "stopped", TRUE));
		ADD(form, axis_web_auth_button(label, start->n_missing > 0 ? "button-primary" : NULL));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help", start->n_missing > 0 ? "The episode files that were migrated are kept "
			"and are not replaced. The hold of every series counted above ends, so Axis also searches for their "
			"monitored episodes that are still to come." : "No episode would be searched for now, and the episode "
			"files that were migrated are kept and are not replaced. Ending the hold means that Axis searches for "
			"these series' monitored episodes that are still to come."));
		ADD(card, axis_web_storage_text("help", "A series can also be started alone, on its own page, with Let Axis "
			"search for it."));
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, back_text));
	response = axis_web_page_respond(request, "Start searching", HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
movies_start_page(
	HtmxRequest        *request,
	const AxisArrStart *start,
	const gchar        *notice,
	guint               status
);

static HtmxResponse *
start_page(
	HtmxRequest        *request,
	const AxisArrStart *start,
	const gchar        *notice,
	guint               status
){
	return start->source->app == AXIS_ARR_SONARR ? series_start_page(request, start, notice, status) :
		movies_start_page(request, start, notice, status);
}

static HtmxResponse *
movies_start_page(
	HtmxRequest        *request,
	const AxisArrStart *start,
	const gchar        *notice,
	guint               status
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const AxisArrSource *source = start->source;
	const gchar *app = axis_arr_app_label(source->app);
	g_autofree gchar *action = axis_web_arr_path(source->id, "/start");
	g_autofree gchar *back = axis_web_arr_path(source->id, NULL);
	g_autofree gchar *back_text = g_strdup_printf("Back to %s", source->name);
	g_autofree gchar *held = movies_text(start->n_held);
	g_autofree gchar *missing = movies_text(start->n_missing);
	g_autofree gchar *with_file = movies_text(start->n_with_file);
	/* The same numbers as the connection's card and the notice, less what
	 * Radarr is downloading now: only a movie without a file is searched for. */
	g_autofree gchar *would = g_strdup_printf("Starting searching affects only the movies without a file. Of %s "
		"migrated from %s that %s still held, %s %s monitored and %s no file: Axis would search for %s. %s %s a file: "
		"nothing changes for %s, because a file that was migrated is kept and is not replaced.", held, app,
		start->n_held == 1 ? "is" : "are", missing, start->n_missing == 1 ? "is" : "are",
		start->n_missing == 1 ? "has" : "have", start->n_missing == 1 ? "it" : "them", with_file,
		start->n_with_file == 1 ? "has" : "have", start->n_with_file == 1 ? "it" : "them");
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What would be started");
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), "Start searching for the migrated movies");
	ADD(content, heading);
	ADD(content, axis_web_page_intro("Until now Axis has searched for none of the movies it migrated. This lets it "
		"search for the ones without a file; a movie that has its file is left as it is. Nothing is searched for at "
		"once: the scheduler takes them at its ordinary pace."));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(TRUE, notice));
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-start");
	if (start->n_held + start->n_in_queue == 0) {
		ADD(card, axis_web_storage_text(NULL, "No movie this connection migrated is held: there is nothing to start."));
		ADD(content, card);
		ADD(content, axis_web_browser_link(back, back_text));
		return axis_web_page_respond(request, "Start searching", HTMX_NODE(content));
	}
	ADD(card, axis_web_storage_text(NULL, would));
	if (start->n_with_file > 0)
		ADD(card, axis_web_storage_text(NULL, "The files that were migrated from Radarr are kept: Axis does not upgrade or "
			"replace them, before or after you start, until you release them with Let Axis upgrade these files on this "
			"connection's page, or one file at a time on its movie's page."));
	if (start->per_run > 0 && start->interval_s > 0 && start->n_missing > 0) {
		guint64 runs = ((guint64)start->n_missing + start->per_run - 1) / start->per_run;
		g_autofree gchar *takes = duration_text(runs * start->interval_s);
		g_autofree gchar *pace = g_strdup_printf("The pace: %u %s every %u minutes, so %s for the %s that %s missing. "
			"The download client fills as it would on any day.", start->per_run,
			start->per_run == 1 ? "search" : "searches", start->interval_s / 60, takes, missing,
			start->n_missing == 1 ? "is" : "are");

		ADD(card, axis_web_storage_text(NULL, pace));
	}
	if (start->n_unseen > 0) {
		g_autofree gchar *unseen = movies_text(start->n_unseen);
		g_autofree gchar *text = g_strdup_printf("%s %s held and %s not started: %s has a file for %s that Axis does "
			"not see, and searching would fetch a second copy.", unseen, start->n_unseen == 1 ? "stays" : "stay",
			start->n_unseen == 1 ? "is" : "are", app, start->n_unseen == 1 ? "it" : "them");

		ADD(card, axis_web_storage_text(NULL, text));
	}
	ADD(content, card);

	/* Running beside Radarr: its queue now, and what it still monitors. */
	card = axis_web_storage_card("Radarr beside Axis");
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-beside");
	if (start->queue == AXIS_ARR_QUEUE_READ && start->n_in_queue > 0) {
		g_autofree gchar *n = axis_web_format_count(start->n_in_queue);
		g_autoptr(GString) names = g_string_new(NULL);
		g_autofree gchar *text = NULL;
		guint i;

		for (i = 0; i < start->in_queue->len; i++)
			g_string_append_printf(names, "%s%s", i > 0 ? "; " : "", (const gchar *)g_ptr_array_index(start->in_queue, i));
		if (start->n_in_queue > start->in_queue->len)
			g_string_append(names, "; and more");
		text = g_strdup_printf("%s is downloading %s %s: left out, and held until you start searching again. %s.", app,
			start->n_in_queue == 1 ? "this" : "these", n, names->str);
		ADD(card, axis_web_storage_text(NULL, text));
	} else if (start->queue == AXIS_ARR_QUEUE_READ) {
		g_autofree gchar *text = g_strdup_printf("%s is downloading none of these movies now.", app);

		ADD(card, axis_web_storage_text(NULL, text));
	} else if (start->queue == AXIS_ARR_QUEUE_UNCLEAR) {
		g_autofree gchar *text = start->n_queue > 0 ?
			g_strdup_printf("%s answered and lists %u %s, but Axis could not tell which movies they are for. Look at "
				"%s's own queue before you start: Axis would search for a movie %s is downloading.", app, start->n_queue,
				start->n_queue == 1 ? "download" : "downloads", app, app) :
			g_strdup_printf("%s answered, but Axis could not read its download queue. Look at %s's own queue before "
				"you start: Axis would search for a movie %s is downloading.", app, app, app);

		ADD(card, axis_web_browser_warning(text));
	} else {
		g_autofree gchar *text = g_strdup_printf("%s did not answer, so Axis could not check what it is downloading. "
			"If it is running, both would download the same movies.", app);

		ADD(card, axis_web_browser_warning(text));
	}
	if (start->n_monitored > 0) {
		g_autofree gchar *n = axis_web_format_count((guint)start->n_monitored);
		g_autofree gchar *text = g_strdup_printf("%s still monitors %s of these movies. Both would download them. "
			"Unmonitor them in %s, or stop %s, before you start. Axis cannot do that for you: it only reads from %s.",
			app, n, app, app, app);

		ADD(card, axis_web_browser_warning(text));
	} else if (start->n_monitored < 0) {
		g_autofree gchar *text = g_strdup_printf("Axis does not know whether %s still monitors these movies: its last "
			"reading is more than a day old. If it does, both would download them. Read %s again to see.", app, app);

		ADD(card, axis_web_storage_text(NULL, text));
	} else {
		g_autofree gchar *text = g_strdup_printf("At the last reading %s monitored none of these movies.", app);

		ADD(card, axis_web_storage_text(NULL, text));
	}
	ADD(content, card);

	card = axis_web_storage_card("Start");
	if (start->n_held == 0) {
		ADD(card, axis_web_storage_text(NULL, "Every movie that could be started is being downloaded by Radarr now: "
			"there is nothing to start. Come back once Radarr has finished."));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, action);
		/* What it does for a person: the movies without a file.  With none,
		 * the label says what is left of it, the end of the hold. */
		g_autofree gchar *label = start->n_missing > 0 ? g_strdup_printf("Start searching for %s without a file", missing) :
			g_strdup_printf("End the hold of %s", held);

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		if (start->queue == AXIS_ARR_QUEUE_NO_ANSWER)
			ADD(form, axis_web_storage_checkbox("arr-start-stopped", "Radarr is stopped, or no longer monitors these "
				"movies", "stopped", TRUE));
		ADD(form, axis_web_auth_button(label, start->n_missing > 0 ? "button-primary" : NULL));
		ADD(card, form);
		ADD(card, axis_web_storage_text("help", start->n_missing > 0 ? "The files that were migrated are kept and are not "
			"replaced. The hold of every movie counted above ends, so a movie whose file is lost or let go later is "
			"searched for then." : "No movie would be searched for now, and the files that were migrated are kept and "
			"are not replaced. Ending the hold only means that a movie whose file is lost or let go later is searched "
			"for then."));
		ADD(card, axis_web_storage_text("help", "A movie can also be started alone, on its own page, with Let Axis "
			"search for it."));
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, back_text));
	response = axis_web_page_respond(request, "Start searching", HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
start_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	StartWork *work = data;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return not_found(request);
	/* Radarr did not answer and the box was not ticked: the page again. */
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NEEDS_TICK) && work->start != NULL)
		return start_page(request, work->start, work->error->message, 409);
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
		g_autofree gchar *location = axis_web_arr_path(work->id, "?done=nothing-held");

		return axis_web_auth_redirect(request, location);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->posting) {
		g_autofree gchar *location = axis_web_arr_path(work->id, "?done=started");

		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT ": searching started for %u movies"
			" (%u left out)", work->id, work->start->started, work->start->n_in_queue);
		return axis_web_auth_redirect(request, location);
	}
	return start_page(request, work->start, NULL, 200);
}

static HtmxResponse *
handle_start(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	StartWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(StartWork, 1);
	work->id = id;
	return axis_web_arr_dispatch(user_data, request, start_run, start_done, work, start_work_free);
}

static HtmxResponse *
handle_start_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	StartWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(StartWork, 1);
	work->id = id;
	work->posting = TRUE;
	work->stopped = axis_web_storage_form_yes(request, "stopped");
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_arr_dispatch(user_data, request, start_run, start_done, work, start_work_free);
}

/* ── Let Axis upgrade these files ───────────────────────────────── */

typedef struct
{
	gint64          id;
	gboolean        posting;
	gchar          *digest;
	gchar          *user;
	AxisArrRelease *release;
	GError         *error;
} ReleaseWork;

static void
release_work_free(gpointer data)
{
	ReleaseWork *work = data;

	g_free(work->digest);
	g_free(work->user);
	g_clear_pointer(&work->release, axis_arr_release_free);
	g_clear_error(&work->error);
	g_free(work);
}

static void
release_run(
	AxisWebArr *self,
	gpointer    data
){
	ReleaseWork *work = data;

	if (work->posting)
		axis_arr_release_kept(self->arr, work->id, work->digest, work->user, &work->release, &work->error);
	else
		axis_arr_release_review(self->arr, work->id, &work->release, &work->error);
}

/* The confirmation: how many files, and what happens to them then.  A GET
 * that writes nothing and asks the app nothing. */
static HtmxResponse *
release_page(
	HtmxRequest          *request,
	const AxisArrRelease *release,
	const gchar          *notice,
	guint                 status
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const AxisArrSource *source = release->source;
	const gchar *app = axis_arr_app_label(source->app);
	gboolean series = source->app == AXIS_ARR_SONARR;
	g_autofree gchar *action = axis_web_arr_path(source->id, "/release");
	g_autofree gchar *back = axis_web_arr_path(source->id, NULL);
	g_autofree gchar *back_text = g_strdup_printf("Back to %s", source->name);
	g_autofree gchar *files = files_text(release->n_files);
	g_autofree gchar *titles = series ? series_text(release->n_titles) : movies_text(release->n_titles);
	g_autofree gchar *would = g_strdup_printf("%s migrated from %s, of %s, %s kept now: Axis does not upgrade or "
		"replace %s. This lets Axis upgrade %s.", files, app, titles, release->n_files == 1 ? "is" : "are",
		release->n_files == 1 ? "it" : "them", release->n_files == 1 ? "it" : "them");
	g_autofree gchar *label = g_strdup_printf("Let Axis upgrade %s", files);
	HtmxHeading *heading = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What would change");
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), "Let Axis upgrade these files");
	ADD(content, heading);
	ADD(content, axis_web_page_intro(series ? "The episode files Axis migrated from Sonarr are kept: Axis never "
		"upgrades or replaces them by itself. This lets them go, for this connection." :
		"The files Axis migrated from Radarr are kept: Axis never upgrades or replaces them by itself. This lets "
		"them go, for this connection."));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(TRUE, notice));
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-release");
	if (release->n_files == 0) {
		ADD(card, axis_web_storage_text(NULL, "No file migrated by this connection is kept: "
			"there is nothing to let go."));
		ADD(content, card);
		ADD(content, axis_web_browser_link(back, back_text));
		response = axis_web_page_respond(request, "Let Axis upgrade these files", HTMX_NODE(content));
		if (status != 200)
			htmx_response_set_status(response, status);
		return response;
	}
	ADD(card, axis_web_storage_text(NULL, would));
	ADD(card, axis_web_storage_text(NULL, "After that, a file that is below the cutoff of its quality profile is "
		"replaced at the scheduler's ordinary pace, and the old file goes to the recycle folder. A file at or above "
		"its cutoff stays."));
	ADD(card, axis_web_storage_text(NULL, series ? "A series that is still held is not searched for until you start "
		"searching: letting the files go starts nothing by itself." : "A movie that is still held is not searched for "
		"until you start searching: letting the files go starts nothing by itself."));
	ADD(card, axis_web_storage_text(NULL, "A file you kept by hand, let go or linked by hand yourself since it was "
		"migrated is not counted and not touched. To keep one of these files after this, use Keep it by hand on "
		"its title's page."));
	{
		HtmxNode *form = axis_web_auth_post_form(request, action);

		htmx_element_add_class(HTMX_ELEMENT(form), "form");
		ADD(form, axis_web_auth_hidden("digest", release->digest != NULL ? release->digest : ""));
		ADD(form, axis_web_auth_button(label, "button-primary"));
		ADD(card, form);
	}
	ADD(content, card);
	ADD(content, axis_web_browser_link(back, back_text));
	response = axis_web_page_respond(request, "Let Axis upgrade these files", HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
release_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	ReleaseWork *work = data;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return not_found(request);
	/* The page that was confirmed is not the one drawn now: the page again. */
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_STALE) && work->release != NULL)
		return release_page(request, work->release, work->error->message, 409);
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
		g_autofree gchar *location = axis_web_arr_path(work->id, "?done=nothing-kept");

		return axis_web_auth_redirect(request, location);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->posting) {
		g_autofree gchar *location = axis_web_arr_path(work->id, "?done=released");

		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT ": %u migrated files let go for "
			"upgrades", work->id, work->release->released);
		return axis_web_auth_redirect(request, location);
	}
	return release_page(request, work->release, NULL, 200);
}

static HtmxResponse *
handle_release(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	ReleaseWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(ReleaseWork, 1);
	work->id = id;
	return axis_web_arr_dispatch(user_data, request, release_run, release_done, work, release_work_free);
}

/* The one post that lets them go: an administrator's, with the CSRF token
 * (the route's) and the digest of the confirmation it was made on. */
static HtmxResponse *
handle_release_post(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	ReleaseWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(ReleaseWork, 1);
	work->id = id;
	work->posting = TRUE;
	work->digest = axis_web_arr_form_text(request, "digest", 128);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_arr_dispatch(user_data, request, release_run, release_done, work, release_work_free);
}

void
axis_web_arr_register_run(
	AxisWebArr *self,
	AxisWeb    *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id/release", AXIS_WEB_ROUTE_ADMIN, handle_release, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/release", AXIS_WEB_ROUTE_ADMIN, handle_release_post,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id/brought", AXIS_WEB_ROUTE_ADMIN, handle_brought, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id/start", AXIS_WEB_ROUTE_ADMIN, handle_start, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/start", AXIS_WEB_ROUTE_ADMIN, handle_start_post, self);
}
