/*
 * axis-web-arr-preview.c - the preview of what a run would bring from
 * Radarr or Sonarr: the counts, the two mapping tables, the options, the
 * titles that would be skipped and the files that would not be linked
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One page for both apps, in the app's own words (movies, or series).  A
 * Sonarr's adds the list of the series that would be migrated (how each is
 * monitored, season by season, its episodes, where it would go and how many
 * of its files Axis holds), says why nothing is brought while Axis has no
 * TheTVDB key, and has no confirmation: series are not brought yet.
 *
 * Opening the page writes nothing.  Posting it writes the mapping tables and
 * the options to the connection (arr_source.mappings) and nothing else: no
 * movie, link, blocklist entry, search or setting.  Radarr is not asked
 * again: everything is computed from the plan the reading stored.
 *
 * Radarr's own text (a title, a path, a profile's name) is shown as text,
 * never as markup.
 */

#include "web/axis-web-arr-private.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* Folder rows a form may post: those shown, and the one to add. */
#define MAX_POSTED_ROWS 600

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *child)
{
	HtmxTd *cell = htmx_td_new();

	if (child != NULL)
		ADD(cell, child);
	return HTMX_NODE(cell);
}

/* A path or a name of Radarr's, as code that may wrap. */
static HtmxNode *
code_node(const gchar *text)
{
	HtmxCode *code = htmx_code_new();

	ADD(code, axis_web_breakable(text != NULL ? text : ""));
	return HTMX_NODE(code);
}

static gchar *
count_text(guint value)
{
	return axis_web_format_count(value);
}

/* "1,021 movies", "1 movie". */
static gchar *
movies_text(guint n)
{
	g_autofree gchar *count = count_text(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "movie" : "movies");
}

/* "214 series", "1,021 movies": what the app holds. */
static gchar *
titles_text(
	AxisArrApp app,
	guint      n
){
	g_autofree gchar *count = count_text(n);

	return g_strdup_printf("%s %s", count, axis_arr_app_noun(app, n != 1));
}

/* "Movie", "Movies", "Series": the app's noun where a sentence or a heading
 * begins. */
static const gchar *
noun_capital(
	AxisArrApp app,
	gboolean   many
){
	if (app == AXIS_ARR_SONARR)
		return "Series";
	return many ? "Movies" : "Movie";
}

static gchar *
episodes_text(guint n)
{
	g_autofree gchar *count = count_text(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "episode" : "episodes");
}

static gchar *
files_text(guint n)
{
	g_autofree gchar *count = count_text(n);

	return g_strdup_printf("%s %s", count, n == 1 ? "file" : "files");
}

/* A bare <select> for a table cell, with a name for a screen reader. */
static HtmxNode *
cell_select(
	const gchar *name,
	const gchar *aria,
	GPtrArray   *values,	/* gchar *, ended by NULL */
	GPtrArray   *labels,
	const gchar *current
){
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(select), "aria-label", aria);
	for (i = 0; i < values->len && g_ptr_array_index(values, i) != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(g_ptr_array_index(values, i), g_ptr_array_index(labels, i));

		if (g_strcmp0(g_ptr_array_index(values, i), current) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	return HTMX_NODE(select);
}

static HtmxNode *
cell_input(
	const gchar *name,
	const gchar *aria,
	const gchar *value,
	const gchar *placeholder
){
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);

	htmx_input_set_name(input, name);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "aria-label", aria);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "spellcheck", "false");
	if (value != NULL && value[0] != '\0')
		htmx_input_set_value(input, value);
	if (placeholder != NULL)
		htmx_input_set_placeholder(input, placeholder);
	return HTMX_NODE(input);
}

/* ── The counts ─────────────────────────────────────────────────── */

/* What the page is for, said first: how many titles would be migrated as
 * the app monitors them and how many unmonitored; for series, the seasons
 * and episodes that come monitored; and the unmonitored ones left out while
 * the option below is not ticked. */
static HtmxNode *
monitoring_line(const AxisArrPreview *preview)
{
	AxisArrApp app_id = preview->source->app;
	const gchar *app = axis_arr_app_label(app_id);
	guint n = preview->counts[AXIS_ARR_SKIP_NONE];
	guint left_out = preview->counts[AXIS_ARR_SKIP_UNMONITORED];
	g_autofree gchar *all = titles_text(app_id, n);
	g_autofree gchar *on = count_text(preview->bring_monitored);
	g_autofree gchar *off = count_text(n - preview->bring_monitored);
	g_autoptr(GString) text = g_string_new(NULL);
	HtmxNode *line;

	g_string_append_printf(text, "Would be migrated: %s, %s monitored and %s unmonitored, as in %s.", all, on, off,
		app);
	if (app_id == AXIS_ARR_SONARR) {
		g_autofree gchar *seasons = count_text(preview->bring_seasons_on);
		g_autofree gchar *episodes = episodes_text(preview->bring_episodes_on);

		g_string_append_printf(text, " %s %s and %s would be migrated monitored.", seasons,
			preview->bring_seasons_on == 1 ? "season" : "seasons", episodes);
	}
	if (left_out > 0) {
		g_autofree gchar *left = titles_text(app_id, left_out);

		g_string_append_printf(text, " %s that %s does not monitor %s left out: tick the box under Options to have "
			"%s migrated unmonitored.", left, app, left_out == 1 ? "is" : "are", left_out == 1 ? "it" : "them");
	}
	line = axis_web_storage_text(NULL, text->str);
	htmx_element_set_id(HTMX_ELEMENT(line), "arr-monitoring");
	return line;
}

static HtmxNode *
counts_card(const AxisArrPreview *preview)
{
	const gchar *app = axis_arr_app_label(preview->source->app);
	g_autofree gchar *title = g_strdup_printf("What %s has, and what would be migrated", app);
	g_autofree gchar *all = movies_text(preview->n_titles);
	g_autofree gchar *monitored = count_text(preview->n_monitored);
	g_autofree gchar *not = count_text(preview->n_titles - preview->n_monitored);
	g_autofree gchar *with_file = count_text(preview->n_with_file);
	g_autofree gchar *already = count_text(preview->counts[AXIS_ARR_SKIP_ALREADY]);
	g_autofree gchar *bring = movies_text(preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *link = files_text(preview->files[AXIS_ARR_FILE_FOUND]);
	g_autofree gchar *skipped = count_text(preview->n_titles - preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *when = axis_web_format_time(preview->plan->started_us, "");
	g_autofree gchar *has = g_strdup_printf("%s has %s: %s monitored, %s not. %s %s a file. %s %s already in Axis.",
		app, all, monitored, not, with_file, preview->n_with_file == 1 ? "has" : "have", already,
		preview->counts[AXIS_ARR_SKIP_ALREADY] == 1 ? "is" : "are");
	g_autofree gchar *would = g_strdup_printf("Would migrate %s and link %s. %s skipped (see below). Nothing would be "
		"searched for.", bring, link, skipped);
	g_autofree gchar *read = g_strdup_printf("Read from %s on %s. Changing the choices below counts again without "
		"asking %s.", app, when, app);
	HtmxNode *card = axis_web_storage_card(title);

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-counts");
	ADD(card, monitoring_line(preview));
	ADD(card, axis_web_storage_text(NULL, has));
	ADD(card, axis_web_storage_text(NULL, would));
	if (preview->n_unusable > 0) {
		g_autofree gchar *files = files_text(preview->n_unusable);
		g_autofree gchar *line = g_strdup_printf("%s of these movies would not be linked (see below): the movie "
			"would still be migrated, without its file.", files);

		ADD(card, axis_web_storage_text(NULL, line));
	}
	if (preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP] > 0) {
		g_autofree gchar *waiting = movies_text(preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP]);
		g_autofree gchar *line = g_strdup_printf("%s %s not looked up yet: one reading asks TMDb about at most %d "
			"titles Axis does not have, and TMDb may not have answered for some. Read again to continue; what was "
			"looked up is kept.", waiting, preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP] == 1 ? "was" : "were",
			AXIS_ARR_LOOKUP_MAX);

		ADD(card, axis_web_storage_text(NULL, line));
	}
	if (preview->n_in_queue > 0) {
		g_autofree gchar *queued = movies_text(preview->n_in_queue);
		g_autofree gchar *line = g_strdup_printf("%s was downloading %s when it was read.", app, queued);

		ADD(card, axis_web_storage_text("help", line));
	}
	if (preview->n_relink > 0) {
		g_autofree gchar *files = files_text(preview->n_relink);
		g_autofree gchar *line = g_strdup_printf("%s of movies migrated before %s in Axis's index now, and would be "
			"linked.", files, preview->n_relink == 1 ? "is" : "are");

		ADD(card, axis_web_storage_text(NULL, line));
	}
	ADD(card, axis_web_storage_text("help", read));
	ADD(card, axis_web_storage_text("help", "Nothing on this page adds a movie, links a file or starts a search, "
		"until you confirm under Migrate at the end of the page."));
	return card;
}

/* ── A Sonarr's counts, and what stands in for its confirmation ─── */

/* Where the TheTVDB key is entered. */
#define METADATA_PAGE "/metadata"

/* Why no series is brought while Axis has no TheTVDB key, and where the key
 * goes: on the preview of a plan read before the key went. */
static HtmxNode *
no_key_notice(const AxisArrPreview *preview)
{
	const gchar *app = axis_arr_app_label(preview->source->app);
	HtmxDiv *box = htmx_div_new();
	HtmxP *where = htmx_p_new();
	g_autofree gchar *why = g_strdup_printf("Axis has no TheTVDB key, so no series would be migrated. %s numbers its "
		"series, its seasons' and episodes' monitoring and its files as TheTVDB does, and a series migrated from %s"
		"follows TheTVDB's aired order, which Axis cannot fetch without a key. Set a TheTVDB key on the Movie and TV "
		"details page, then read %s again.", app, app, app);

	htmx_element_set_id(HTMX_ELEMENT(box), "arr-needs-tvdb");
	ADD(box, axis_web_browser_warning(why));
	ADD(where, axis_web_browser_link(METADATA_PAGE, "Open Movie and TV details to set the TheTVDB key"));
	ADD(box, where);
	return HTMX_NODE(box);
}

static HtmxNode *
series_counts_card(const AxisArrPreview *preview)
{
	const gchar *app = axis_arr_app_label(preview->source->app);
	g_autofree gchar *title = g_strdup_printf("What %s has, and what would be migrated", app);
	g_autofree gchar *all = titles_text(AXIS_ARR_SONARR, preview->n_titles);
	g_autofree gchar *monitored = count_text(preview->n_monitored);
	g_autofree gchar *not = count_text(preview->n_titles - preview->n_monitored);
	g_autofree gchar *already = count_text(preview->counts[AXIS_ARR_SKIP_ALREADY]);
	g_autofree gchar *read_series = titles_text(AXIS_ARR_SONARR, preview->n_series_read);
	g_autofree gchar *episodes = episodes_text(preview->n_episodes);
	g_autofree gchar *episodes_on = count_text(preview->n_episodes_monitored);
	g_autofree gchar *episodes_file = count_text(preview->n_episodes_with_file);
	g_autofree gchar *bring = titles_text(AXIS_ARR_SONARR, preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *bring_episodes = episodes_text(preview->bring_episodes);
	g_autofree gchar *bring_on = count_text(preview->bring_episodes_monitored);
	g_autofree gchar *link = files_text(preview->files[AXIS_ARR_FILE_FOUND]);
	g_autofree gchar *of_files = count_text(preview->bring_files);
	g_autofree gchar *skipped = count_text(preview->n_titles - preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *when = axis_web_format_time(preview->plan->started_us, "");
	g_autofree gchar *has = g_strdup_printf("%s has %s: %s monitored, %s not. %s %s already in Axis.", app, all,
		monitored, not, already, preview->counts[AXIS_ARR_SKIP_ALREADY] == 1 ? "is" : "are");
	g_autofree gchar *lists = g_strdup_printf("The episodes of %s were read: %s, %s monitored in %s, %s with a file.",
		read_series, episodes, episodes_on, app, episodes_file);
	g_autofree gchar *would = g_strdup_printf("Would migrate %s with %s (%s monitored), each season and episode monitored "
		"as in %s, and link %s of their %s episode files. %s skipped (see below). Nothing would be searched for.",
		bring, bring_episodes, bring_on, app, link, of_files, skipped);
	g_autofree gchar *read = g_strdup_printf("Read from %s on %s. Changing the choices below counts again without "
		"asking %s.", app, when, app);
	HtmxNode *card = axis_web_storage_card(title);

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-counts");
	ADD(card, monitoring_line(preview));
	ADD(card, axis_web_storage_text(NULL, has));
	ADD(card, axis_web_storage_text(NULL, lists));
	ADD(card, axis_web_storage_text(NULL, would));
	ADD(card, axis_web_storage_text(NULL, "A series migrated from Sonarr would follow TheTVDB's aired order, the "
		"numbering Sonarr's seasons, episodes and files are in, whatever the numbering of new series in Axis is."));
	if (preview->n_unusable > 0) {
		g_autofree gchar *files = files_text(preview->n_unusable);
		g_autofree gchar *line = g_strdup_printf("%s of these series would not be linked (see below): the series "
			"would still be migrated, without those files.", files);

		ADD(card, axis_web_storage_text(NULL, line));
	}
	if (preview->n_odd > 0) {
		g_autofree gchar *odd = count_text(preview->n_odd);
		g_autofree gchar *line = g_strdup_printf("%s %s of %s's lists of episodes and files could not be read and "
			"%s left out.", odd, preview->n_odd == 1 ? "entry" : "entries", app, preview->n_odd == 1 ? "was" : "were");

		ADD(card, axis_web_storage_text("help", line));
	}
	if (preview->n_relink_renumbered > 0) {
		/* A series brought before, whose numbering a person changed since:
		 * Sonarr's places are TheTVDB's, so nothing more of it is linked. */
		guint n = preview->n_relink_renumbered;
		g_autofree gchar *line = g_strdup_printf("Not linked: %u episode %s of series migrated before that no longer "
			"follow TheTVDB's numbering in Axis. Sonarr's places are TheTVDB's, and the same season and number may be "
			"another episode in the numbering such a series follows now. Link %s by hand on the series' links page.",
			n, n == 1 ? "file" : "files", n == 1 ? "it" : "them");

		ADD(card, axis_web_storage_text(NULL, line));
	}
	if (preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP] > 0) {
		g_autofree gchar *waiting = titles_text(AXIS_ARR_SONARR, preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP]);
		g_autofree gchar *line = g_strdup_printf("%s %s not looked up yet: one reading asks TMDb and TheTVDB at most %d "
			"questions about titles Axis does not have, and one of them may not have answered. Read again to continue; "
			"what was looked up is kept.", waiting, preview->counts[AXIS_ARR_SKIP_NOT_LOOKED_UP] == 1 ? "was" : "were",
			AXIS_ARR_LOOKUP_MAX);

		ADD(card, axis_web_storage_text(NULL, line));
	}
	if (preview->n_in_queue > 0) {
		g_autofree gchar *queued = titles_text(AXIS_ARR_SONARR, preview->n_in_queue);
		g_autofree gchar *line = g_strdup_printf("%s was downloading episodes of %s when it was read.", app, queued);

		ADD(card, axis_web_storage_text("help", line));
	}
	ADD(card, axis_web_storage_text("help", read));
	ADD(card, axis_web_storage_text("help", "Nothing on this page adds a series, links a file or starts a search."));
	return card;
}

static const AxisWebColumn bringing_columns[] = {
	{ "Series", AXIS_WEB_COL_NAME },
	{ "In Sonarr", AXIS_WEB_COL_WIDE },
	{ "Episodes", AXIS_WEB_COL_WIDE },
	{ "Becomes, in Axis", AXIS_WEB_COL_WIDE },
	{ "Episode files", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* Where the app's name stands in a series' card is always Sonarr: only a
 * Sonarr has series. */
static HtmxNode *
bringing_card(
	const AxisArrPreview *preview,
	const gchar          *first,
	const gchar          *next
){
	HtmxNode *card = axis_web_storage_card("Series that would be migrated");
	guint total = preview->counts[AXIS_ARR_SKIP_NONE];
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-bringing");
	if (total == 0) {
		ADD(card, axis_web_storage_text("help", "No series would be migrated now: each is in Axis already or skipped for "
			"the reason listed below."));
		return card;
	}
	ADD(card, axis_web_storage_text(NULL, "Each series with how Sonarr monitors it and each of its seasons, how many "
		"of its episodes Sonarr monitors and has a file for, the quality profile and the library folder it would get, "
		"and how many of its episode files Axis's index holds at their mapped paths."));
	if (preview->bring_anime > 0) {
		g_autofree gchar *n = count_text(preview->bring_anime);
		g_autofree gchar *line = g_strdup_printf("%s of these %s anime in Sonarr. An anime series is migrated as any "
			"series: matched by its TheTVDB identifier, numbered by season as TheTVDB's aired order numbers it, its "
			"files linked on the episodes Sonarr has them on and kept, and held until you start searching. It is marked "
			"as anime in Axis. Its row says whether Sonarr's absolute episode numbers agree with TheTVDB's; a "
			"difference changes nothing of what is migrated. Once you start searching, Axis recognises a release "
			"named by an absolute episode number for it, and searches by that number too, when TheTVDB's absolute "
			"numbers for it can be used.", n, preview->bring_anime == 1 ? "is" : "are");

		ADD(card, axis_web_storage_text("help arr-bringing-anime", line));
	}
	table = axis_web_table_new("arr-bringing", bringing_columns);
	body = htmx_tbody_new();
	for (i = 0; preview->bringing != NULL && i < preview->bringing->len; i++) {
		const AxisArrBringRow *row = g_ptr_array_index(preview->bringing, i);
		g_autofree gchar *title = row->title == NULL ? g_strdup_printf("(entry %u of Sonarr's list)", row->seq) :
			row->year > 0 ? g_strdup_printf("%s (%d)", row->title, row->year) : g_strdup(row->title);
		g_autofree gchar *episodes = episodes_text(row->n_episodes);
		g_autofree gchar *on = count_text(row->n_episodes_monitored);
		g_autofree gchar *with_file = count_text(row->n_episodes_with_file);
		g_autofree gchar *files = files_text(row->n_files);
		g_autofree gchar *found = count_text(row->n_found);
		g_autofree gchar *missing = count_text(row->n_files - MIN(row->n_found, row->n_files));
		g_autofree gchar *monitoring = g_strdup_printf("%s.%s%s%s%s%s%s", row->monitored ? "Monitored" :
			"Not monitored", row->seasons_on != NULL ? " Seasons monitored: " : "",
			row->seasons_on != NULL ? row->seasons_on : "", row->seasons_on != NULL ? "." : "",
			row->seasons_off != NULL ? " Not monitored: " : "", row->seasons_off != NULL ? row->seasons_off : "",
			row->seasons_off != NULL ? "." : "");
		g_autofree gchar *counts = g_strdup_printf("%s: %s monitored, %s with a file.%s", episodes, on, with_file,
			row->n_odd > 0 ? " Some entries could not be read." : "");
		/* One Sonarr has as anime: migrated as any series, and what comparing
		 * Sonarr's absolute numbers with TheTVDB's gives, with every episode
		 * that differs.  Said, and changing nothing of the row. */
		g_autofree gchar *anime = !row->anime ? NULL : g_strdup_printf(" Anime: migrated as any series, numbered by "
			"season, and marked as anime in Axis. %s%s%s%s", row->absolute != NULL ? row->absolute : "",
			row->absolute_list != NULL && row->absolute_differ > 1 ? " They differ for: " : "",
			row->absolute_list != NULL && row->absolute_differ > 1 ? row->absolute_list : "",
			row->absolute_list != NULL && row->absolute_differ > 1 ? "." : "");
		g_autofree gchar *becomes = g_strdup_printf("%s, in %s. TheTVDB's aired order.%s",
			row->profile != NULL ? row->profile : "(no profile)", row->root != NULL ? row->root : "(no folder)",
			anime != NULL ? anime : "");
		g_autofree gchar *linked = row->n_files == 0 ? g_strdup("No files.") :
			g_strdup_printf("%s: %s found in Axis, %s not.", files, found, missing);
		HtmxTr *line = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxBdi *text = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(text), title);
		ADD(name, text);
		ADD(line, name);
		ADD(line, cell_text(monitoring));
		ADD(line, cell_text(counts));
		ADD(line, cell_text(becomes));
		ADD(line, cell_text(linked));
		ADD(body, line);
	}
	ADD(table, body);
	ADD(card, table);
	{
		g_autofree gchar *from = count_text(preview->bring_page * AXIS_ARR_PAGE_ROWS + 1);
		g_autofree gchar *to = count_text(MIN((preview->bring_page + 1) * AXIS_ARR_PAGE_ROWS, total));
		g_autofree gchar *all = count_text(total);
		g_autofree gchar *line = g_strdup_printf("%s to %s of %s.", from, to, all);

		ADD(card, axis_web_storage_text("help", line));
		if (first != NULL || next != NULL)
			ADD(card, axis_web_browser_pager(first, next));
	}
	return card;
}

/*
 * The tick of a run's confirmation, never pre-ticked: "Start searching for
 * these as soon as they are migrated".  Its value is the digest of this
 * very run with the tick (the tick is part of what is confirmed), so a page
 * made before anything changed, or a post that makes a value up, is refused.
 */
static HtmxNode *
start_tick(const AxisArrPreview *preview)
{
	const gchar *app = axis_arr_app_label(preview->source->app);
	gboolean series = preview->source->app == AXIS_ARR_SONARR;
	g_autofree gchar *help = g_strdup_printf("Not ticked: everything migrated is held, and nothing is searched for "
		"until you start searching on this connection's page. Ticked: when the run ends it does what Start searching "
		"does. It reads what %s is downloading and leaves those %s held; if %s does not answer then, it starts nothing "
		"and says so. %s %s has a file for that Axis does not see stays held either way. Axis then searches at the "
		"scheduler's ordinary pace for what is missing and monitored, while %s may still be doing the same: unmonitor "
		"them in %s, or stop it, first. The migrated files are kept: Axis does not replace them.", app,
		series ? "series" : "movies", app, series ? "An episode" : "A movie", app, app, app);
	HtmxDiv *wrap = htmx_div_new();
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for("arr-run-start");

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), "arr-run-start");
	htmx_input_set_name(box, "start");
	htmx_input_set_value(box, preview->digest_start != NULL ? preview->digest_start : "");
	htmx_node_set_text_content(HTMX_NODE(text), "Start searching for these as soon as they are migrated");
	ADD(field, box);
	ADD(field, text);
	ADD(wrap, field);
	ADD(wrap, axis_web_storage_text("help", help));
	return HTMX_NODE(wrap);
}

/* What a run does not touch, in plain words, with the count: a title that is
 * in Axis already is left as it is, so its files are not migrated, and
 * not kept by the run. */
static void
add_already(
	HtmxNode             *card,
	const AxisArrPreview *preview
){
	gboolean series = preview->source->app == AXIS_ARR_SONARR;
	guint n = preview->counts[AXIS_ARR_SKIP_ALREADY];
	g_autofree gchar *count = axis_web_format_count(n);
	g_autofree gchar *text = NULL;

	if (n == 0)
		return;
	text = series ?
		g_strdup_printf("%s series %s in Axis already. This run leaves %s exactly as %s: Axis's ordinary rules go on "
			"applying to %s files, which this run does not migrate and so does not keep. What an earlier run of "
			"this connection migrated and kept stays kept.", count, n == 1 ? "is" : "are", n == 1 ? "it" : "them",
			n == 1 ? "it is" : "they are", n == 1 ? "its" : "their") :
		g_strdup_printf("%s %s in Axis already. This run leaves %s exactly as %s: Axis's ordinary rules go on "
			"applying to %s, which this run does not migrate and so does not keep. What an earlier run of this "
			"connection migrated and kept stays kept.", count, n == 1 ? "movie is" : "movies are",
			n == 1 ? "it" : "them", n == 1 ? "it is" : "they are", n == 1 ? "its file" : "their files");
	ADD(card, axis_web_storage_text("arr-run-already", text));
}

/* The confirmation of a Sonarr's run: what it will do with the series, said
 * once more, and the one button that starts it.  The form carries the digest
 * of everything this page shows, as a Radarr's does. */
static HtmxNode *
series_run_card(
	HtmxRequest          *request,
	const AxisArrPreview *preview
){
	const gchar *app = axis_arr_app_label(preview->source->app);
	g_autofree gchar *action = axis_web_arr_path(preview->source->id, "/run");
	g_autofree gchar *bring = titles_text(AXIS_ARR_SONARR, preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *link = files_text(preview->files[AXIS_ARR_FILE_FOUND] + preview->n_relink);
	g_autofree gchar *does = g_strdup_printf("Migrates %s into Axis, each with the quality profile and the library folder "
		"chosen above, numbered as TheTVDB's aired order numbers it from the start, with each season and each episode "
		"monitored as in %s, and links %s that Axis's index holds at the mapped paths, each to the episode or episodes "
		"%s has it on.", bring, app, link, app);
	g_autofree gchar *label = g_strdup_printf("Migrate %s", bring);
	HtmxNode *card = axis_web_storage_card("Migrate");
	HtmxNode *form;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-run");
	if (!preview->series_ready) {
		ADD(card, axis_web_storage_text(NULL, "No series can be migrated now: Axis has no TheTVDB key, and a series "
			"from Sonarr follows TheTVDB's aired order."));
		return card;
	}
	if (preview->counts[AXIS_ARR_SKIP_NONE] + preview->n_relink == 0) {
		ADD(card, axis_web_storage_text(NULL, "There is nothing to migrate now: every series is in Axis already or "
			"skipped for the reason listed above."));
		add_already(card, preview);
		return card;
	}
	ADD(card, axis_web_storage_text(NULL, does));
	/* The folders once more: a wrong one is the mistake that matters. */
	for (i = 0; i < preview->folder_rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);
		const gchar *name = NULL;
		guint r;

		for (r = 0; row->root_id > 0 && r < preview->roots->len; r++) {
			const AxisArrRootChoice *root = g_ptr_array_index(preview->roots, r);

			if (root->id == row->root_id)
				name = root->name;
		}
		if (name != NULL) {
			g_autofree gchar *line = g_strdup_printf("%s's %s is the library folder %s%s%s.", app, row->prefix, name,
				row->below != NULL && row->below[0] != '\0' ? ", below " : "",
				row->below != NULL && row->below[0] != '\0' ? row->below : "");

			ADD(card, axis_web_storage_text("help", line));
		}
	}
	ADD(card, axis_web_storage_text(NULL, "An episode Sonarr has that TheTVDB's aired order, as Axis holds it, does not "
		"(or whose TheTVDB identifier disagrees) is not guessed: its monitoring is not copied, its file is not linked, "
		"and the list of what was migrated says how many there were for each series."));
	ADD(card, axis_web_storage_text(NULL, "Nothing is searched for unless you tick the box below: every series migrated "
		"is held until you start searching, on this connection's page or on a series' own page. An episode Sonarr "
		"has a file for that Axis does not see stays held after that, so that no second copy is fetched."));
	ADD(card, axis_web_storage_text(NULL, "The migrated episode files are kept: Axis does not upgrade or replace "
		"them by itself, until you let it, for one file on the series' page or for all of them on this connection's "
		"page."));
	add_already(card, preview);
	ADD(card, axis_web_storage_text(NULL, "No file is moved, renamed or deleted, and nothing in Sonarr is changed. A "
		"series migrated by mistake is removed as any series, which touches no file."));
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_auth_hidden("digest", preview->digest != NULL ? preview->digest : ""));
	ADD(form, start_tick(preview));
	ADD(form, axis_web_auth_button(label, "button-primary"));
	ADD(card, form);
	return card;
}

/* ── The confirmation ───────────────────────────────────────────── */

/* What a run will do, said once more, and the one button that starts it.
 * The form carries the digest of everything this page shows: a page made
 * before a choice, the reading or the library changed is refused. */
static HtmxNode *
run_card(
	HtmxRequest          *request,
	const AxisArrPreview *preview
){
	const gchar *app = axis_arr_app_label(preview->source->app);
	g_autofree gchar *action = axis_web_arr_path(preview->source->id, "/run");
	g_autofree gchar *bring = movies_text(preview->counts[AXIS_ARR_SKIP_NONE]);
	g_autofree gchar *link = files_text(preview->files[AXIS_ARR_FILE_FOUND] + preview->n_relink);
	g_autofree gchar *does = g_strdup_printf("Migrates %s into Axis, each with the quality profile and the library folder "
		"chosen above and monitored as in %s, and links %s that Axis's index holds at the mapped paths.", bring, app,
		link);
	g_autofree gchar *label = g_strdup_printf("Migrate %s", bring);
	HtmxNode *card = axis_web_storage_card("Migrate");
	HtmxNode *form;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-run");
	if (preview->counts[AXIS_ARR_SKIP_NONE] + preview->n_relink == 0) {
		ADD(card, axis_web_storage_text(NULL, "There is nothing to migrate now: every movie is in Axis already or "
			"skipped for the reason listed above."));
		add_already(card, preview);
		return card;
	}
	ADD(card, axis_web_storage_text(NULL, does));
	/* The folders once more: a wrong one is the mistake that matters. */
	for (i = 0; i < preview->folder_rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);
		const gchar *name = NULL;
		guint r;

		for (r = 0; row->root_id > 0 && r < preview->roots->len; r++) {
			const AxisArrRootChoice *root = g_ptr_array_index(preview->roots, r);

			if (root->id == row->root_id)
				name = root->name;
		}
		if (name != NULL) {
			g_autofree gchar *line = g_strdup_printf("%s's %s is the library folder %s%s%s.", app, row->prefix, name,
				row->below != NULL && row->below[0] != '\0' ? ", below " : "",
				row->below != NULL && row->below[0] != '\0' ? row->below : "");

			ADD(card, axis_web_storage_text("help", line));
		}
	}
	ADD(card, axis_web_storage_text(NULL, "Nothing is searched for unless you tick the box below: every movie migrated "
		"is held until you start searching, on this connection's page or on a movie's own page."));
	ADD(card, axis_web_storage_text(NULL, "The migrated files are kept: Axis does not upgrade or replace them by "
		"itself, until you let it, for one file on the movie's page or for all of them on this connection's page."));
	add_already(card, preview);
	ADD(card, axis_web_storage_text(NULL, "No file is moved, renamed or deleted, and nothing in Radarr is changed. A "
		"movie migrated by mistake is removed as any movie, which touches no file."));
	form = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, axis_web_auth_hidden("digest", preview->digest != NULL ? preview->digest : ""));
	ADD(form, start_tick(preview));
	ADD(form, axis_web_auth_button(label, "button-primary"));
	ADD(card, form);
	return card;
}

/* ── The mapping tables ─────────────────────────────────────────── */

static const AxisWebColumn profile_columns[] = {
	{ "Quality profile in Radarr", AXIS_WEB_COL_NAME },
	{ "Movies", AXIS_WEB_COL_COUNT },
	{ "Becomes, in Axis", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

static const AxisWebColumn folder_columns[] = {
	{ "Folder in Radarr", AXIS_WEB_COL_NAME },
	{ "Movies", AXIS_WEB_COL_COUNT },
	{ "Library folder in Axis", AXIS_WEB_COL_CONTROLS },
	{ "Found there", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* The same two tables for a Sonarr, in its words. */
static const AxisWebColumn series_profile_columns[] = {
	{ "Quality profile in Sonarr", AXIS_WEB_COL_NAME },
	{ "Series", AXIS_WEB_COL_COUNT },
	{ "Becomes, in Axis", AXIS_WEB_COL_CONTROLS },
	{ NULL, 0 }
};

static const AxisWebColumn series_folder_columns[] = {
	{ "Folder in Sonarr", AXIS_WEB_COL_NAME },
	{ "Series", AXIS_WEB_COL_COUNT },
	{ "Library folder in Axis", AXIS_WEB_COL_CONTROLS },
	{ "Found there", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* Beside a choice no person made: Axis filled it in. */
static HtmxNode *
suggested_badge(void)
{
	return HTMX_NODE(axis_web_browser_badge("Suggested", "badge-note"));
}

static HtmxNode *
note_span(const gchar *text)
{
	HtmxSpan *span = htmx_span_new();

	htmx_node_set_text_content(HTMX_NODE(span), text);
	return HTMX_NODE(span);
}

/* Axis's quality profiles as a choice: "" for none first. */
static void
profile_choices(
	const AxisArrPreview *preview,
	GPtrArray            *values,
	GPtrArray            *labels
){
	guint i;

	g_ptr_array_add(values, g_strdup("0"));
	g_ptr_array_add(labels, g_strdup_printf("(none: skip these %s)", axis_arr_app_noun(preview->source->app, TRUE)));
	for (i = 0; i < preview->quality_profiles->len; i++) {
		const AxisArrChoice *choice = g_ptr_array_index(preview->quality_profiles, i);

		g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT, choice->id));
		g_ptr_array_add(labels, g_strdup(choice->name));
	}
	g_ptr_array_add(values, NULL);
	g_ptr_array_add(labels, NULL);
}

static void
root_choices(
	const AxisArrPreview *preview,
	GPtrArray            *values,
	GPtrArray            *labels
){
	guint i;

	g_ptr_array_add(values, g_strdup("0"));
	g_ptr_array_add(labels, g_strdup("(not mapped)"));
	for (i = 0; i < preview->roots->len; i++) {
		const AxisArrRootChoice *root = g_ptr_array_index(preview->roots, i);

		g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT, root->id));
		g_ptr_array_add(labels, g_strdup_printf("%s (on %s%s)", root->name, root->volume_name,
			root->online ? "" : ", offline"));
	}
	g_ptr_array_add(values, NULL);
	g_ptr_array_add(labels, NULL);
}

static HtmxNode *
profiles_card(const AxisArrPreview *preview)
{
	HtmxNode *card = axis_web_storage_card("Quality profiles");
	AxisArrApp app_id = preview->source->app;
	const gchar *app = axis_arr_app_label(app_id);
	const gchar *nouns = axis_arr_app_noun(app_id, TRUE);
	g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) ids = g_string_new(NULL);
	g_autofree gchar *intro = g_strdup_printf("The quality profile of Axis's that each of %s's becomes. Choose "
		"\"(none: skip these %s)\" to leave a profile's %s out.", app, nouns, nouns);
	g_autofree gchar *person = g_strdup_printf("%s's default for new %s", preview->plan->started_by != NULL ?
		preview->plan->started_by : "the reader", nouns);
	g_autofree gchar *none = g_strdup_printf("No %s that could be migrated uses a quality profile yet.",
		axis_arr_app_noun(app_id, FALSE));
	HtmxNode *table = axis_web_table_new("arr-profiles", app_id == AXIS_ARR_SONARR ? series_profile_columns :
		profile_columns);
	HtmxTbody *body = htmx_tbody_new();
	guint shown = 0;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-profiles");
	ADD(card, axis_web_storage_text(NULL, intro));
	profile_choices(preview, values, labels);
	for (i = 0; i < preview->profile_rows->len; i++) {
		const AxisArrProfileRow *row = g_ptr_array_index(preview->profile_rows, i);
		g_autofree gchar *name = g_strdup_printf("profile_%" G_GINT64_FORMAT, row->arr_id);
		g_autofree gchar *aria = g_strdup_printf("Axis's quality profile for %s", row->name);
		g_autofree gchar *count = count_text(row->n_titles);
		g_autofree gchar *current = g_strdup_printf("%" G_GINT64_FORMAT, row->chosen);
		HtmxTr *line;
		HtmxTd *first;
		HtmxBdi *text;

		/* Only the profiles a movie that could be migrated uses. */
		if (row->n_titles == 0)
			continue;
		line = htmx_tr_new();
		first = htmx_td_new();
		text = htmx_bdi_new();
		htmx_node_set_text_content(HTMX_NODE(text), row->name);
		ADD(first, text);
		ADD(line, first);
		ADD(line, cell_text(count));
		if (row->from == AXIS_ARR_FROM_NAME || row->from == AXIS_ARR_FROM_PERSON ||
		    row->from == AXIS_ARR_FROM_DEFAULT) {
			/* Not a person's choice: what Axis filled in, and from what. */
			HtmxDiv *pair = htmx_div_new();

			htmx_element_add_class(HTMX_ELEMENT(pair), "inline-form");
			ADD(pair, cell_select(name, aria, values, labels, current));
			ADD(pair, suggested_badge());
			ADD(pair, note_span(row->from == AXIS_ARR_FROM_NAME ? "Axis has a profile of the same name" :
				row->from == AXIS_ARR_FROM_PERSON ? person : "Axis's default profile"));
			ADD(line, cell_node(HTMX_NODE(pair)));
		} else {
			ADD(line, cell_node(cell_select(name, aria, values, labels, current)));
		}
		ADD(body, line);
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, ids->len > 0 ? "," : "", row->arr_id);
		shown++;
	}
	ADD(card, axis_web_auth_hidden("profile_ids", ids->str));
	if (shown == 0) {
		g_object_unref(table);
		g_object_unref(body);
		ADD(card, axis_web_storage_text("help", none));
		return card;
	}
	ADD(table, body);
	ADD(card, table);
	if (preview->quality_profiles->len > 0) {
		HtmxDiv *fill = htmx_div_new();
		HtmxNode *button = axis_web_auth_button("Use it for every row", "button-small");

		htmx_element_add_class(HTMX_ELEMENT(fill), "inline-form");
		ADD(fill, cell_select("fill_profile", "A quality profile of Axis's for every row", values, labels, "0"));
		htmx_element_set_attribute(HTMX_ELEMENT(button), "name", "do");
		htmx_element_set_attribute(HTMX_ELEMENT(button), "value", "fill");
		ADD(fill, button);
		ADD(card, fill);
	}
	return card;
}

/* What the index says of a folder row's files. */
static gchar *
found_text(
	const AxisArrPreview   *preview,
	const AxisArrFolderRow *row
){
	const gchar *app = axis_arr_app_label(preview->source->app);
	const AxisArrRootChoice *root = NULL;
	g_autofree gchar *files = count_text(row->n_files);
	g_autofree gchar *found = count_text(row->n_found);
	g_autofree gchar *scanned = NULL;
	guint i;

	for (i = 0; row->root_id > 0 && i < preview->roots->len; i++) {
		if (((const AxisArrRootChoice *)g_ptr_array_index(preview->roots, i))->id == row->root_id)
			root = g_ptr_array_index(preview->roots, i);
	}
	if (root == NULL) {
		g_autofree gchar *sampled = count_text(row->n_sampled);
		g_autofree gchar *best = count_text(row->n_sample_found);
		g_autofree gchar *why = NULL;

		/* No saved choice, and nothing Axis could fill in: why. */
		if (row->from == AXIS_ARR_FROM_NOTHING && row->unsure == AXIS_ARR_UNSURE_NO_FILES)
			why = g_strdup_printf(" Nothing suggested: %s has no files under it.", app);
		else if (row->from == AXIS_ARR_FROM_NOTHING && row->unsure == AXIS_ARR_UNSURE_NOT_FOUND)
			why = g_strdup_printf(" Nothing suggested: of %s files looked for, no library folder holds more than %s "
				"at the same paths.", sampled, best);
		else if (row->from == AXIS_ARR_FROM_NOTHING && row->unsure == AXIS_ARR_UNSURE_TWO)
			why = g_strdup(" Nothing suggested: its files are in Axis's index in more than one place. Choose one.");
		return row->n_files > 0 ? g_strdup_printf("%s %s of %s's. Not mapped.%s", files, row->n_files == 1 ? "file" :
			"files", app, why != NULL ? why : "") : g_strdup_printf("Not mapped.%s", why != NULL ? why : "");
	}
	if (row->from == AXIS_ARR_FROM_FILES && root->online && root->scanned_us > 0) {
		/* What the suggestion stands on: every file of the app's under the
		 * folder, counted at the suggested place. */
		scanned = axis_web_format_time(root->scanned_us, "");
		return g_strdup_printf("%s of %s files found here. Last scanned %s.", found, files, scanned);
	}
	if (!root->online)
		return g_strdup_printf("The library folder is on %s, which Axis cannot see now.", root->volume_name);
	if (root->scanned_us <= 0)
		return g_strdup_printf("%s of %s files found at these paths in Axis. This library folder has never been "
			"scanned: scan it on the Library page, then look again.", found, files);
	scanned = axis_web_format_time(root->scanned_us, "");
	if (row->n_files > 0 && row->n_found == 0)
		return g_strdup_printf("0 of %s files found at these paths in Axis. None of %s's files are at these paths: "
			"this is probably not the same folder. Last scanned %s.", files, app, scanned);
	return g_strdup_printf("%s of %s files found at these paths in Axis. Last scanned %s.", found, files, scanned);
}

static HtmxNode *
folders_card(const AxisArrPreview *preview)
{
	const gchar *app = axis_arr_app_label(preview->source->app);
	HtmxNode *card = axis_web_storage_card("Folders");
	g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *rows = g_strdup_printf("%u", preview->folder_rows->len + 1);
	g_autofree gchar *intro = g_strdup_printf("The library folder of Axis's that each of %s's folders is, and the "
		"path below it when the folder is not its top. A file of %s's is linked only when Axis's index holds it at "
		"the same path below that place, with the same size. No file is touched.", app, app);
	HtmxNode *table = axis_web_table_new("arr-folders", preview->source->app == AXIS_ARR_SONARR ?
		series_folder_columns : folder_columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxTr *extra = htmx_tr_new();
	HtmxDiv *pair;
	g_autofree gchar *extra_prefix = NULL;
	g_autofree gchar *extra_root = NULL;
	g_autofree gchar *extra_below = NULL;
	g_autofree gchar *no_roots = g_strdup_printf("Axis has no library folder for %s yet. Add one on the Disks page, "
		"scan it, then come back.", axis_arr_app_noun(preview->source->app, TRUE));
	g_autofree gchar *unreachable = g_strdup_printf("%s cannot reach it", app);
	g_autofree gchar *another = g_strdup_printf("Another folder of %s's", app);
	g_autofree gchar *another_hint = g_strdup_printf("/another/folder of %s's", app);
	g_autofree gchar *another_help = g_strdup_printf("Add a folder that is not one of %s's root folders.", app);
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-folders");
	ADD(card, axis_web_storage_text(NULL, intro));
	if (preview->roots->len == 0)
		ADD(card, axis_web_browser_warning(no_roots));
	root_choices(preview, values, labels);
	for (i = 0; i < preview->folder_rows->len; i++) {
		const AxisArrFolderRow *row = g_ptr_array_index(preview->folder_rows, i);
		g_autofree gchar *prefix_name = g_strdup_printf("folder_prefix_%u", i);
		g_autofree gchar *root_name = g_strdup_printf("folder_root_%u", i);
		g_autofree gchar *below_name = g_strdup_printf("folder_below_%u", i);
		g_autofree gchar *root_aria = g_strdup_printf("Axis's library folder for %s", row->prefix);
		g_autofree gchar *below_aria = g_strdup_printf("Path below the library folder for %s", row->prefix);
		g_autofree gchar *count = count_text(row->n_titles);
		g_autofree gchar *current = g_strdup_printf("%" G_GINT64_FORMAT, row->root_id);
		g_autofree gchar *found = found_text(preview, row);
		HtmxTr *line = htmx_tr_new();
		HtmxTd *first = htmx_td_new();

		ADD(first, code_node(row->prefix));
		ADD(first, axis_web_auth_hidden(prefix_name, row->prefix));
		if (!row->is_root)
			ADD(first, axis_web_browser_badge("Not a root folder", "badge-note"));
		else if (!row->accessible)
			ADD(first, axis_web_browser_badge(unreachable, "badge-warn"));
		ADD(line, first);
		ADD(line, cell_text(count));
		pair = htmx_div_new();
		htmx_element_add_class(HTMX_ELEMENT(pair), "inline-form");
		ADD(pair, cell_select(root_name, root_aria, values, labels, current));
		ADD(pair, cell_input(below_name, below_aria, row->below, "path below it (optional)"));
		if (row->from == AXIS_ARR_FROM_FILES)
			ADD(pair, suggested_badge());
		ADD(line, cell_node(HTMX_NODE(pair)));
		ADD(line, cell_text(found));
		ADD(body, line);
	}
	/* A prefix that is no root folder (Radarr lets a movie live anywhere). */
	extra_prefix = g_strdup_printf("folder_prefix_%u", preview->folder_rows->len);
	extra_root = g_strdup_printf("folder_root_%u", preview->folder_rows->len);
	extra_below = g_strdup_printf("folder_below_%u", preview->folder_rows->len);
	ADD(extra, cell_node(cell_input(extra_prefix, another, NULL, another_hint)));
	ADD(extra, cell_text(""));
	pair = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(pair), "inline-form");
	ADD(pair, cell_select(extra_root, "Axis's library folder for the other folder", values, labels, "0"));
	ADD(pair, cell_input(extra_below, "Path below the library folder for the other folder", NULL,
		"path below it (optional)"));
	ADD(extra, cell_node(HTMX_NODE(pair)));
	ADD(extra, cell_text(another_help));
	ADD(body, extra);
	ADD(table, body);
	ADD(card, table);
	ADD(card, axis_web_auth_hidden("folder_rows", rows));
	return card;
}

static HtmxNode *
options_card(const AxisArrPreview *preview)
{
	HtmxNode *card = axis_web_storage_card("Options");
	AxisArrApp app = preview->source->app;
	g_autofree gchar *count = count_text(preview->n_titles - preview->n_monitored);
	g_autofree gchar *label = g_strdup_printf("Also migrate unmonitored %s (%s)", axis_arr_app_noun(app, TRUE), count);
	g_autofree gchar *help = app == AXIS_ARR_SONARR ? g_strdup_printf("They would be migrated with no season and no "
		"episode monitored, as a series %s does not monitor has none searched for.", axis_arr_app_label(app)) :
		g_strdup_printf("They would be migrated unmonitored, as they are in %s.", axis_arr_app_label(app));

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-options");
	ADD(card, axis_web_arr_checkbox("arr-unmonitored", label, "unmonitored", preview->unmonitored));
	ADD(card, axis_web_storage_text("help", help));
	return card;
}

/* ── The two lists ──────────────────────────────────────────────── */

static const AxisWebColumn skipped_columns[] = {
	{ "Movie", AXIS_WEB_COL_NAME },
	{ "Why it would be skipped", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static const AxisWebColumn unusable_columns[] = {
	{ "Movie", AXIS_WEB_COL_NAME },
	{ "Path in Radarr", AXIS_WEB_COL_WIDE },
	{ "Place in Axis", AXIS_WEB_COL_WIDE },
	{ "Why it would not be linked", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static const AxisWebColumn series_skipped_columns[] = {
	{ "Series", AXIS_WEB_COL_NAME },
	{ "Why it would be skipped", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

static const AxisWebColumn series_unusable_columns[] = {
	{ "Series", AXIS_WEB_COL_NAME },
	{ "Path in Sonarr", AXIS_WEB_COL_WIDE },
	{ "Place in Axis", AXIS_WEB_COL_WIDE },
	{ "Why it would not be linked", AXIS_WEB_COL_WIDE },
	{ NULL, 0 }
};

/* The preview's own address, with a filter and the pages (the page of the
 * series that would be migrated is kept as the preview has it). */
static gchar *
preview_href_at(
	const AxisArrPreview *preview,
	AxisArrSkip           filter,
	guint                 skip_page,
	guint                 file_page,
	guint                 bring_page,
	const gchar          *anchor
){
	g_autofree gchar *base = axis_web_arr_path(preview->source->id, "/preview");
	GString *href = g_string_new(base);
	gchar separator = '?';

	if (filter != AXIS_ARR_SKIP_NONE) {
		g_string_append_printf(href, "%cskip=%s", separator, axis_arr_skip_key(filter));
		separator = '&';
	}
	if (skip_page > 0) {
		g_string_append_printf(href, "%csp=%u", separator, skip_page);
		separator = '&';
	}
	if (file_page > 0) {
		g_string_append_printf(href, "%cfp=%u", separator, file_page);
		separator = '&';
	}
	if (bring_page > 0)
		g_string_append_printf(href, "%cbp=%u", separator, bring_page);
	if (anchor != NULL)
		g_string_append_printf(href, "#%s", anchor);
	return g_string_free(href, FALSE);
}

static gchar *
preview_href(
	const AxisArrPreview *preview,
	AxisArrSkip           filter,
	guint                 skip_page,
	guint                 file_page,
	const gchar          *anchor
){
	return preview_href_at(preview, filter, skip_page, file_page, preview->bring_page, anchor);
}

static gchar *
title_text(
	const gchar *title,
	gint         year,
	guint        seq,
	AxisArrApp   app
){
	if (title == NULL)
		return g_strdup_printf("(entry %u of %s's list)", seq, axis_arr_app_label(app));
	if (year > 0)
		return g_strdup_printf("%s (%d)", title, year);
	return g_strdup(title);
}

static HtmxNode *
pager(
	const gchar *first,
	const gchar *next,
	guint        page,
	guint        total
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *from = count_text(page * AXIS_ARR_PAGE_ROWS + 1);
	g_autofree gchar *to = count_text(MIN((page + 1) * AXIS_ARR_PAGE_ROWS, total));
	g_autofree gchar *all = count_text(total);
	g_autofree gchar *line = g_strdup_printf("%s to %s of %s.", from, to, all);

	ADD(box, axis_web_storage_text("help", line));
	if (first != NULL || next != NULL)
		ADD(box, axis_web_browser_pager(first, next));
	return HTMX_NODE(box);
}

static HtmxNode *
skipped_card(const AxisArrPreview *preview)
{
	AxisArrApp app = preview->source->app;
	g_autofree gchar *heading = g_strdup_printf("%s that would be skipped", noun_capital(app, TRUE));
	g_autofree gchar *none = g_strdup_printf("No %s would be skipped.", axis_arr_app_noun(app, FALSE));
	HtmxNode *card = axis_web_storage_card(heading);
	guint total = preview->n_titles - preview->counts[AXIS_ARR_SKIP_NONE];
	HtmxUl *filters = htmx_ul_new();
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-skipped");
	if (total == 0) {
		g_object_unref(filters);
		ADD(card, axis_web_storage_text("help", none));
		return card;
	}
	/* The reasons as a filter, each with its count. */
	htmx_element_add_class(HTMX_ELEMENT(filters), "tabs");
	for (i = 0; i < AXIS_ARR_SKIP_N; i++) {
		AxisArrSkip skip = (AxisArrSkip)i;
		guint n = skip == AXIS_ARR_SKIP_NONE ? total : preview->counts[i];
		g_autofree gchar *reason = skip == AXIS_ARR_SKIP_NONE ? g_strdup("All") : axis_arr_skip_label(skip, app);
		g_autofree gchar *count = count_text(n);
		g_autofree gchar *text = g_strdup_printf("%s (%s)", reason, count);
		g_autofree gchar *href = preview_href(preview, skip, 0, preview->file_page, "arr-skipped");
		HtmxLi *item;

		if (n == 0)
			continue;
		item = htmx_li_new();
		if (skip == preview->skip_filter) {
			HtmxSpan *here = htmx_span_new();

			htmx_element_set_attribute(HTMX_ELEMENT(here), "aria-current", "page");
			htmx_node_set_text_content(HTMX_NODE(here), text);
			ADD(item, here);
		} else {
			ADD(item, axis_web_browser_link(href, text));
		}
		ADD(filters, item);
	}
	ADD(card, filters);
	table = axis_web_table_new("arr-skipped", app == AXIS_ARR_SONARR ? series_skipped_columns : skipped_columns);
	body = htmx_tbody_new();
	for (i = 0; i < preview->skipped->len; i++) {
		const AxisArrSkipRow *row = g_ptr_array_index(preview->skipped, i);
		g_autofree gchar *title = title_text(row->title, row->year, row->seq, app);
		g_autofree gchar *reason = axis_arr_skip_label(row->skip, app);
		g_autofree gchar *why = row->skip == AXIS_ARR_SKIP_OFFLINE && row->detail != NULL ?
			g_strdup_printf("%s: the library folder is on %s, which Axis cannot see now", reason, row->detail) :
			row->skip == AXIS_ARR_SKIP_NOT_LOOKED_UP && g_strcmp0(row->detail, "anime") == 0 ?
			g_strdup("Read Sonarr again to include this anime series") : g_strdup(reason);
		HtmxTr *line = htmx_tr_new();
		HtmxTd *first = htmx_td_new();
		HtmxBdi *text = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(text), title);
		if (row->title_id > 0) {
			g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, row->title_id);
			HtmxA *link = htmx_a_new_with_href(href);

			ADD(link, text);
			ADD(first, link);
		} else {
			ADD(first, text);
		}
		ADD(line, first);
		ADD(line, cell_text(why));
		ADD(body, line);
	}
	ADD(table, body);
	ADD(card, table);
	{
		g_autofree gchar *first = preview->skip_page > 0 ?
			preview_href(preview, preview->skip_filter, 0, preview->file_page, "arr-skipped") : NULL;
		g_autofree gchar *next = (preview->skip_page + 1) * AXIS_ARR_PAGE_ROWS < preview->n_skipped_listed ?
			preview_href(preview, preview->skip_filter, preview->skip_page + 1, preview->file_page, "arr-skipped") :
			NULL;

		if (preview->n_skipped_listed > 0)
			ADD(card, pager(first, next, preview->skip_page, preview->n_skipped_listed));
	}
	return card;
}

static HtmxNode *
unusable_card(const AxisArrPreview *preview)
{
	HtmxNode *card = axis_web_storage_card("Files Axis cannot use");
	AxisArrApp app = preview->source->app;
	const gchar *noun = axis_arr_app_noun(app, FALSE);
	const gchar *nouns = axis_arr_app_noun(app, TRUE);
	g_autofree gchar *intro = g_strdup_printf("Files of %s that would be migrated, which would not be linked. The "
		"%s is still migrated; nothing is guessed about its file.", nouns, noun);
	g_autofree gchar *none = g_strdup_printf("Every file of the %s that would be migrated is at its path in "
		"Axis, or they have none.", nouns);
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-unusable");
	ADD(card, axis_web_storage_text(NULL, intro));
	if (preview->n_unusable == 0) {
		ADD(card, axis_web_storage_text("help", none));
		return card;
	}
	table = axis_web_table_new("arr-unusable", app == AXIS_ARR_SONARR ? series_unusable_columns : unusable_columns);
	body = htmx_tbody_new();
	for (i = 0; i < preview->unusable->len; i++) {
		const AxisArrFileRow *row = g_ptr_array_index(preview->unusable, i);
		g_autofree gchar *title = title_text(row->title, row->year, 0, app);
		g_autofree gchar *reason = axis_arr_file_label(row->state, app);
		g_autofree gchar *why = row->other != NULL ? g_strdup_printf("%s: %s", reason, row->other) : g_strdup(reason);
		HtmxTr *line = htmx_tr_new();
		HtmxTd *first = htmx_td_new();
		HtmxBdi *text = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(text), title);
		ADD(first, text);
		ADD(line, first);
		ADD(line, cell_node(row->arr_path != NULL ? code_node(row->arr_path) : NULL));
		ADD(line, cell_node(row->mapped != NULL ? code_node(row->mapped) : NULL));
		ADD(line, cell_text(why));
		ADD(body, line);
	}
	ADD(table, body);
	ADD(card, table);
	{
		g_autofree gchar *first = preview->file_page > 0 ?
			preview_href(preview, preview->skip_filter, preview->skip_page, 0, "arr-unusable") : NULL;
		g_autofree gchar *next = (preview->file_page + 1) * AXIS_ARR_PAGE_ROWS < preview->n_unusable ?
			preview_href(preview, preview->skip_filter, preview->skip_page, preview->file_page + 1, "arr-unusable") :
			NULL;

		ADD(card, pager(first, next, preview->file_page, preview->n_unusable));
	}
	return card;
}

/* ── The page ───────────────────────────────────────────────────── */

static HtmxResponse *
preview_page(
	HtmxRequest          *request,
	const AxisArrPreview *preview,
	const gchar          *notice,
	gboolean              notice_error,
	guint                 status
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *app = axis_arr_app_label(preview->source->app);
	g_autofree gchar *back = axis_web_arr_path(preview->source->id, NULL);
	g_autofree gchar *action = axis_web_arr_path(preview->source->id, "/preview");
	g_autofree gchar *title = g_strdup_printf("Preview: %s", preview->source->name);
	g_autofree gchar *intro = g_strdup_printf("What Axis would migrate from %s, before anything changes. Nothing "
		"has been added to Axis. Changing the choices on this page saves the choices and nothing else.", app);
	g_autofree gchar *back_text = g_strdup_printf("Back to %s", preview->source->name);
	HtmxHeading *heading = htmx_h1_new();
	HtmxResponse *response;

	htmx_node_set_text_content(HTMX_NODE(heading), title);
	ADD(content, heading);
	ADD(content, axis_web_page_intro(intro));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(notice_error, notice));
	if (preview->plan == NULL) {
		gboolean reading = preview->latest != NULL && g_strcmp0(preview->latest->state, "reading") == 0 &&
			preview->latest->job_live;
		g_autofree gchar *none = reading ? g_strdup_printf("%s is being read now. The preview is here when the "
			"reading is done.", app) : g_strdup_printf("There is nothing to preview: read what %s has first (a "
			"reading is kept for a day).", app);

		ADD(content, axis_web_storage_text("notice", none));
		if (!preview->series_ready)
			ADD(content, no_key_notice(preview));
		ADD(content, axis_web_browser_link(back, back_text));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, action);
		gboolean series = preview->source->app == AXIS_ARR_SONARR;

		/* Without a TheTVDB key no series is brought: said first, with where
		 * the key goes. */
		if (!preview->series_ready)
			ADD(content, no_key_notice(preview));
		ADD(content, series ? series_counts_card(preview) : counts_card(preview));
		htmx_element_set_id(HTMX_ELEMENT(form), "arr-mappings");
		if (preview->n_suggested > 0) {
			HtmxNode *filled = axis_web_storage_text(NULL, "Axis filled these in from what it already knows. Change "
				"any that are wrong.");

			htmx_element_set_id(HTMX_ELEMENT(filled), "arr-suggested");
			ADD(form, filled);
		}
		ADD(form, profiles_card(preview));
		ADD(form, folders_card(preview));
		ADD(form, options_card(preview));
		ADD(form, axis_web_auth_button("Save the choices and count again", "button-primary"));
		ADD(content, form);
		if (series) {
			g_autofree gchar *first = preview->bring_page > 0 ? preview_href_at(preview, preview->skip_filter,
				preview->skip_page, preview->file_page, 0, "arr-bringing") : NULL;
			g_autofree gchar *next = (preview->bring_page + 1) * AXIS_ARR_PAGE_ROWS <
				preview->counts[AXIS_ARR_SKIP_NONE] ? preview_href_at(preview, preview->skip_filter, preview->skip_page,
				preview->file_page, preview->bring_page + 1, "arr-bringing") : NULL;

			ADD(content, bringing_card(preview, first, next));
		}
		ADD(content, skipped_card(preview));
		ADD(content, unusable_card(preview));
		ADD(content, series ? series_run_card(request, preview) : run_card(request, preview));
		ADD(content, axis_web_browser_link(back, back_text));
	}
	response = axis_web_page_respond(request, title, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

typedef struct
{
	gint64               id;
	AxisArrSkip          filter;
	guint                skip_page;
	guint                file_page;
	guint                bring_page;
	gboolean             saved;
	/* A post. */
	gboolean             posting;
	AxisArrProfileInput *profiles;
	guint                n_profiles;
	AxisArrFolderInput  *folders;
	guint                n_folders;
	GPtrArray           *texts;	/* the strings @folders points at */
	gboolean             unmonitored;
	GError              *refused;	/* why the post was refused */
	/* The confirmation of a run. */
	gboolean             running;
	gchar               *digest;
	gchar               *start;	/* the tick's value; NULL: not ticked */
	gchar               *user;
	AxisArrPreview      *preview;
	GError              *error;
} PreviewWork;

static void
preview_work_free(gpointer data)
{
	PreviewWork *work = data;

	g_free(work->digest);
	g_free(work->start);
	g_free(work->user);
	g_free(work->profiles);
	g_free(work->folders);
	g_clear_pointer(&work->texts, g_ptr_array_unref);
	g_clear_error(&work->refused);
	g_clear_pointer(&work->preview, axis_arr_preview_free);
	g_clear_error(&work->error);
	g_free(work);
}

static void
preview_run(
	AxisWebArr *self,
	gpointer    data
){
	PreviewWork *work = data;
	AxisArrPages pages;

	memset(&pages, 0, sizeof(pages));
	if (work->running) {
		/* One confirmed run.  Refused (the preview changed, nothing to
		 * bring, one under way): the page again, as it stands now. */
		if (axis_arr_request_apply(self->arr, work->id, work->digest, work->start, work->user, &work->refused))
			return;
		if (!g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_STALE) &&
		    !g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID) &&
		    !g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED) &&
		    !g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY) &&
		    !g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_BUSY)) {
			g_propagate_error(&work->error, g_steal_pointer(&work->refused));
			return;
		}
	}
	if (work->posting) {
		g_autoptr(AxisArrPlan) latest = NULL;
		g_autoptr(AxisArrPlan) usable = NULL;

		/* The page the choices came from had a plan; a form posted without
		 * one (it expired, or was never read) would blank what is stored. */
		if (!axis_arr_plans(self->arr, work->id, &latest, &usable, &work->error))
			return;
		if (usable == NULL) {
			g_set_error_literal(&work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID,
				"The choices were not saved: there is no reading to preview now. Read what the app has first.");
		} else if (axis_arr_save_mappings(self->arr, work->id, work->profiles, work->n_profiles, work->folders,
		        work->n_folders, work->unmonitored, &work->refused)) {
			return;
		}
		/* Refused: the page again, as it stands, with the reason. */
		if (!g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
			g_propagate_error(&work->error, g_steal_pointer(&work->refused));
			return;
		}
	}
	pages.skip_filter = work->filter;
	pages.skip_page = work->skip_page;
	pages.file_page = work->file_page;
	pages.bring_page = work->bring_page;
	axis_arr_preview_pages(self->arr, work->id, &pages, &work->preview, &work->error);
}

static HtmxResponse *
preview_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	PreviewWork *work = data;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->running && work->refused == NULL) {
		gboolean start = work->start != NULL && work->start[0] != '\0';
		g_autofree gchar *location = axis_web_arr_path(work->id, start ? "?done=running-start" : "?done=running");

		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT ": a run was confirmed%s", work->id,
			start ? ", with searching to start at its end" : "");
		return axis_web_auth_redirect(request, location);
	}
	if (work->running)
		/* Nothing was brought: the preview again, as it is now. */
		return preview_page(request, work->preview, work->refused->message, TRUE,
			g_error_matches(work->refused, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID) ? 400 : 409);
	if (work->posting && work->refused == NULL) {
		g_autofree gchar *location = axis_web_arr_path(work->id, "/preview?done=saved");

		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT ": the preview's choices changed",
			work->id);
		return axis_web_auth_redirect(request, location);
	}
	if (work->refused != NULL)
		return preview_page(request, work->preview, work->refused->message, TRUE, 400);
	return preview_page(request, work->preview, work->saved ? "The choices have been saved. The counts below are "
		"under them." : NULL, FALSE, 200);
}

/* A page number of an address: 0 unless it is a whole number up to @limit (the
 * last page its list can have). */
static guint
page_param(
	HtmxRequest *request,
	const gchar *name,
	guint64      limit
){
	const gchar *text = htmx_request_get_query_param(request, name);
	gchar *end = NULL;
	guint64 value;

	if (text == NULL || !g_ascii_isdigit(text[0]) || strlen(text) > 6)
		return 0;
	value = g_ascii_strtoull(text, &end, 10);
	if (end == NULL || *end != '\0' || value > limit)
		return 0;
	return (guint)value;
}

static HtmxResponse *
handle_preview(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	PreviewWork *work;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
	work = g_new0(PreviewWork, 1);
	work->id = id;
	if (!axis_arr_skip_from_key(htmx_request_get_query_param(request, "skip"), &work->filter))
		work->filter = AXIS_ARR_SKIP_NONE;
	work->skip_page = page_param(request, "sp", AXIS_ARR_MAX_TITLES / AXIS_ARR_PAGE_ROWS);
	/* A Sonarr's plan may hold this many episode files. */
	work->file_page = page_param(request, "fp", AXIS_ARR_PLAN_MAX_FILES / AXIS_ARR_PAGE_ROWS);
	work->bring_page = page_param(request, "bp", AXIS_ARR_MAX_TITLES / AXIS_ARR_PAGE_ROWS);
	work->saved = g_strcmp0(htmx_request_get_query_param(request, "done"), "saved") == 0;
	return axis_web_arr_dispatch_limited(user_data, request, AXIS_WEB_ARR_PREVIEW_PENDING, preview_run, preview_done,
		work, preview_work_free);
}

/* A whole number of a form, 0 when it is none. */
static gint64
form_id(
	HtmxRequest *request,
	const gchar *name
){
	guint64 value = 0;

	if (!axis_web_storage_form_u64(request, name, &value) || value > G_MAXINT64)
		return 0;
	return (gint64)value;
}

static HtmxResponse *
handle_preview_save(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	g_autofree gchar *ids = NULL;
	g_auto(GStrv) each = NULL;
	PreviewWork *work;
	gboolean fill;
	gint64 fill_with;
	gint64 rows;
	guint i;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
	work = g_new0(PreviewWork, 1);
	work->id = id;
	work->posting = TRUE;
	work->texts = g_ptr_array_new_with_free_func(g_free);
	work->unmonitored = axis_web_storage_form_yes(request, "unmonitored");

	/* The profile table: the rows the page named, each with its choice; or
	 * one choice for every row. */
	fill = g_strcmp0(htmx_request_get_form_value(request, "do"), "fill") == 0;
	fill_with = form_id(request, "fill_profile");
	ids = axis_web_arr_form_text(request, "profile_ids", 16384);
	each = g_strsplit(ids != NULL ? ids : "", ",", AXIS_ARR_MAX_PROFILES + 1);
	work->profiles = g_new0(AxisArrProfileInput, AXIS_ARR_MAX_PROFILES);
	for (i = 0; each[i] != NULL && work->n_profiles < AXIS_ARR_MAX_PROFILES; i++) {
		gchar *end = NULL;
		gint64 arr_id = g_ascii_strtoll(each[i], &end, 10);
		g_autofree gchar *name = NULL;

		if (end == each[i] || *end != '\0' || arr_id <= 0)
			continue;
		name = g_strdup_printf("profile_%" G_GINT64_FORMAT, arr_id);
		work->profiles[work->n_profiles].arr_id = arr_id;
		work->profiles[work->n_profiles].chosen = fill && fill_with > 0 ? fill_with : form_id(request, name);
		work->n_profiles++;
	}

	/* The folder table. */
	rows = MIN(form_id(request, "folder_rows"), MAX_POSTED_ROWS);
	work->folders = g_new0(AxisArrFolderInput, MAX_POSTED_ROWS);
	for (i = 0; i < (guint)rows; i++) {
		g_autofree gchar *prefix_name = g_strdup_printf("folder_prefix_%u", i);
		g_autofree gchar *root_name = g_strdup_printf("folder_root_%u", i);
		g_autofree gchar *below_name = g_strdup_printf("folder_below_%u", i);
		gchar *prefix = axis_web_arr_form_text(request, prefix_name, 4096);
		gchar *below = axis_web_arr_form_text(request, below_name, 4096);

		if (prefix == NULL) {
			g_free(below);
			continue;
		}
		g_ptr_array_add(work->texts, prefix);
		if (below != NULL)
			g_ptr_array_add(work->texts, below);
		work->folders[work->n_folders].prefix = prefix;
		work->folders[work->n_folders].root_id = form_id(request, root_name);
		work->folders[work->n_folders].below = below;
		work->n_folders++;
	}
	return axis_web_arr_dispatch(user_data, request, preview_run, preview_done, work, preview_work_free);
}

/* The one post that starts a run: an administrator's, with the CSRF token
 * (the route's), and the digest of the preview it was confirmed on. */
static HtmxResponse *
handle_run(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	PreviewWork *work;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
	work = g_new0(PreviewWork, 1);
	work->id = id;
	work->running = TRUE;
	work->digest = axis_web_arr_form_text(request, "digest", 128);
	/* The tick "Start searching for these as soon as they are migrated":
	 * absent when not ticked, else the digest of the run with the tick. */
	work->start = axis_web_arr_form_text(request, "start", 128);
	work->user = g_strdup(axis_web_request_get_username(request));
	return axis_web_arr_dispatch_limited(user_data, request, AXIS_WEB_ARR_PREVIEW_PENDING, preview_run, preview_done,
		work, preview_work_free);
}

void
axis_web_arr_register_preview(
	AxisWebArr *self,
	AxisWeb    *web
){
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/run", AXIS_WEB_ROUTE_ADMIN, handle_run, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id/preview", AXIS_WEB_ROUTE_ADMIN, handle_preview, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/preview", AXIS_WEB_ROUTE_ADMIN, handle_preview_save,
		self);
}
