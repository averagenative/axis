/*
 * axis-web-title.c - a title's own page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /title/:id                 a film or a series, by the store's own
 *                                  number (?season=N opens season N, and
 *                                  fetches it if it is not kept)
 *   GET /title/:id/season/:number  one season's episodes (htmx only: what a
 *                                  season that was not ready opens with)
 *
 * The header (backdrop as a dimmed band, poster, title, facts, overview and,
 * when a module has registered them, its actions), the facts, the
 * identifiers as links to the services that know them, and for a series its
 * seasons: each a <details> element holding its episodes with number, name
 * and air date, future ones marked.  No script is needed to read any of it.
 *
 * Seasons are fetched through the metadata layer when the page is asked
 * for, latest first, within a budget of a few seconds in all; a season that
 * did not make it opens with its episodes through htmx, or on a page of its
 * own through the link in it.
 *
 * Whatever came from a provider is text.  The number in the address is
 * checked strictly (digits, no leading zero, at most eighteen) before it is
 * used for anything.
 */

#include "web/axis-web-meta-private.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_meta_add

/* How long one title page spends on fetching its seasons' episodes. */
#define SEASONS_BUDGET_US   (6 * G_USEC_PER_SEC)

/* The most one season's fetch may wait; the page's budget may allow less. */
#define SEASON_WAIT_MS      4000

#define MAX_SEASON_NUMBER   10000

#define MAX_ALT_TITLES      8

/* ── The address ────────────────────────────────────────────────── */

/* The store's id: 1 to 18 digits, the first not zero.  Anything else is no
 * title, whatever the number would be. */
static gboolean
parse_title_id(
	const gchar *text,
	gint64      *id
){
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length < 1 || length > 18 || text[0] == '0')
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	*id = (gint64)g_ascii_strtoull(text, NULL, 10);
	return TRUE;
}

/* A season number: 1 to 5 digits, within what the layer accepts; no sign,
 * no space, no leading zero except "0" itself. */
static gboolean
parse_season(
	const gchar *text,
	gint        *number
){
	gsize length;
	gsize i;

	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length < 1 || length > 5 || (length > 1 && text[0] == '0'))
		return FALSE;
	for (i = 0; i < length; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	*number = (gint)g_ascii_strtoull(text, NULL, 10);
	return *number <= MAX_SEASON_NUMBER;
}

static gchar *
today_text(void)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();

	return g_date_time_format(now, "%Y-%m-%d");
}

/* ── Episodes ───────────────────────────────────────────────────── */

static HtmxNode *
episodes_new(
	const AxisMetaSeason *season,
	const gchar          *today
){
	HtmxUl *list = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(list), "episodes");
	for (i = 0; season->episodes != NULL && i < season->episodes->len; i++) {
		const AxisMetaEpisode *episode = g_ptr_array_index(season->episodes, i);
		HtmxLi *item = htmx_li_new();
		HtmxSpan *number = htmx_span_new();
		HtmxSpan *name = htmx_span_new();
		HtmxSpan *when = htmx_span_new();
		g_autofree gchar *figure = g_strdup_printf("%d", episode->number);
		g_autofree gchar *fallback = g_strdup_printf("Episode %d", episode->number);
		gboolean future = episode->air_date != NULL && strcmp(episode->air_date, today) > 0;

		htmx_element_add_class(HTMX_ELEMENT(item), "episode");
		if (future)
			htmx_element_add_class(HTMX_ELEMENT(item), "episode-future");
		htmx_element_add_class(HTMX_ELEMENT(number), "episode-number");
		htmx_node_set_text_content(HTMX_NODE(number), figure);
		htmx_element_add_class(HTMX_ELEMENT(name), "episode-name");
		htmx_node_set_text_content(HTMX_NODE(name), episode->name != NULL ? episode->name : fallback);
		htmx_element_add_class(HTMX_ELEMENT(when), "episode-date");
		htmx_node_set_text_content(HTMX_NODE(when), episode->air_date != NULL ? episode->air_date : "Not announced");
		ADD(item, number);
		ADD(item, name);
		ADD(item, when);
		if (future)
			ADD(item, axis_web_browser_badge("Not aired", "badge-note"));
		ADD(list, item);
	}
	return HTMX_NODE(list);
}

/* What replaces a season's body, or fills it: its episodes, or why not. */
static HtmxNode *
season_body_new(
	const AxisMetaSeason *season,
	const gchar          *provider_label,
	gboolean              saved,
	const gchar          *problem,
	const gchar          *today
){
	HtmxDiv *body = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(body), "season-body");
	if (season != NULL && season->episodes != NULL && season->episodes->len > 0) {
		ADD(body, episodes_new(season, today));
		return HTMX_NODE(body);
	}
	if (saved) {
		g_autofree gchar *why = axis_web_meta_why_text(provider_label, problem);
		g_autofree gchar *text = g_strdup_printf("%s Try again in a while.", why);

		ADD(body, axis_web_storage_text("help", text));
	} else {
		ADD(body, axis_web_storage_text("help", "No episodes are listed for this season yet."));
	}
	return HTMX_NODE(body);
}

/* ── One title ──────────────────────────────────────────────────── */

typedef struct
{
	gint            number;
	gchar          *name;
	gchar          *air_date;
	gint            episode_count;
	AxisMetaSeason *loaded;		/* with its episodes, or %NULL: not fetched */
} SeasonView;

static void
season_view_free(SeasonView *view)
{
	g_free(view->name);
	g_free(view->air_date);
	if (view->loaded != NULL)
		axis_meta_season_free(view->loaded);
	g_free(view);
}

typedef struct
{
	AxisWeb             *web;
	gint64               id;
	gint                 open_season;	/* -1: none asked for */
	AxisMetaTitleAnswer  answer;
	gboolean             missing;
	gchar               *poster;
	gchar               *backdrop;
	GPtrArray           *seasons;	/* of SeasonView *, in the order shown */
	gboolean             seasons_saved;
	gchar               *seasons_problem;
	/* Whose numbering the seasons are (add-metadata-tvdb): the one asked
	 * for (?numbering=), the one shown, and the other one offered. */
	gchar               *asked;	/* (nullable) an administrator's only */
	gchar               *shown;	/* (nullable) */
	gchar               *other;	/* (nullable) offered to an administrator only */
	gchar               *followed;	/* (nullable) the numbering the title is followed in */
	gint64               series_id;	/* its managed series, 0 for none */
	gboolean             admin;
	AxisWebActionRun    *actions;
	GError              *error;
} TitleWork;

static void
title_work_free(gpointer data)
{
	TitleWork *work = data;

	g_free(work->asked);
	g_free(work->shown);
	g_free(work->other);
	g_free(work->followed);
	axis_meta_title_answer_clear(&work->answer);
	g_free(work->poster);
	g_free(work->backdrop);
	g_clear_pointer(&work->seasons, g_ptr_array_unref);
	g_free(work->seasons_problem);
	axis_web_actions_run_free(work->actions);
	g_clear_error(&work->error);
	g_free(work);
}

/* Season 0 (specials) after the others. */
static gint
compare_display(
	gconstpointer a,
	gconstpointer b
){
	gint x = (*(SeasonView *const *)a)->number;
	gint y = (*(SeasonView *const *)b)->number;

	if ((x == 0) != (y == 0))
		return x == 0 ? 1 : -1;
	return x < y ? -1 : x > y;
}

/* The latest season first, specials last. */
static gint
compare_fetch(
	gconstpointer a,
	gconstpointer b
){
	gint x = (*(SeasonView *const *)a)->number;
	gint y = (*(SeasonView *const *)b)->number;

	if ((x == 0) != (y == 0))
		return x == 0 ? 1 : -1;
	return x > y ? -1 : x < y;
}

static const gchar *
provider_label(const TitleWork *work)
{
	return work->answer.provider != NULL ? work->answer.provider->label : "The details provider";
}

/* "TMDb", "TheTVDB's aired order": whose numbering @source is. */
static const gchar *
source_label(
	const TitleWork *work,
	const gchar     *source
){
	if (source == NULL || g_strcmp0(source, work->answer.title->provider) == 0)
		return provider_label(work);
	if (strcmp(source, "tvdb") == 0)
		return "TheTVDB's aired order";
	return source;
}

/* The seasons of the source shown (add-metadata-tvdb): the one asked for,
 * else the one the series is followed in; the other one offered while
 * TheTVDB has a key and knows the series.  Looking changes nothing of a
 * series. */
static GPtrArray *
source_seasons(
	AxisWebMeta *self,
	TitleWork   *work
){
	const AxisMetaTitle *title = work->answer.title;
	const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");
	/* Another numbering, and asking TheTVDB for it, are an administrator's. */
	gboolean tvdb_offered = work->admin && tvdb != NULL && axis_meta_provider_offered(self->meta, tvdb) &&
		axis_meta_title_get_id(title, "tvdb") != NULL;
	g_autoptr(GPtrArray) seasons = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *asked = work->asked;

	/* Another order of TheTVDB's is shown only while it can be asked. */
	if (asked != NULL && g_str_has_prefix(asked, "tvdb") && !tvdb_offered)
		asked = NULL;
	if (!axis_meta_load_source_seasons(self->meta, work->id, asked, &work->shown, &work->followed, &work->series_id,
	        &seasons, &local)) {
		g_debug("the seasons of title %" G_GINT64_FORMAT " could not be read: %s", work->id, local->message);
		return NULL;
	}
	/* TheTVDB's order not kept yet: asked for whole, then read again. */
	if (seasons->len == 0 && tvdb_offered && g_strcmp0(work->shown, "tvdb") == 0) {
		AxisMetaSeasonAnswer answer;

		memset(&answer, 0, sizeof(answer));
		if (axis_meta_get_source_season(self->meta, work->id, "tvdb", 1, SEASON_WAIT_MS, &answer, NULL)) {
			g_clear_pointer(&seasons, g_ptr_array_unref);
			g_clear_pointer(&work->shown, g_free);
			if (!axis_meta_load_source_seasons(self->meta, work->id, "tvdb", &work->shown, NULL, NULL, &seasons, NULL))
				seasons = NULL;
		}
		axis_meta_season_answer_clear(&answer);
	}
	/* (A series whose record is TheTVDB's own has no other numbering.) */
	if (!work->admin)
		work->other = NULL;
	else if (g_strcmp0(work->shown, title->provider) == 0)
		work->other = tvdb_offered && g_strcmp0(title->provider, "tvdb") != 0 ? g_strdup("tvdb") : NULL;
	else
		work->other = g_strdup(title->provider);
	return g_steal_pointer(&seasons);
}

static void
load_seasons(
	AxisWebMeta *self,
	TitleWork   *work
){
	gint64 deadline = g_get_monotonic_time() + SEASONS_BUDGET_US;
	g_autoptr(GPtrArray) order = g_ptr_array_new();
	g_autoptr(GPtrArray) seasons = source_seasons(self, work);
	guint i;

	work->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)season_view_free);
	if (seasons == NULL)
		return;
	for (i = 0; i < seasons->len; i++) {
		const AxisMetaSeason *season = g_ptr_array_index(seasons, i);
		SeasonView *view = g_new0(SeasonView, 1);

		view->number = season->number;
		view->name = g_strdup(season->name);
		view->air_date = g_strdup(season->air_date);
		view->episode_count = season->episode_count;
		g_ptr_array_add(work->seasons, view);
		g_ptr_array_add(order, view);
	}
	/* The latest first (and the one asked for always): the ones that are
	 * looked at are the ones that arrive in time. */
	g_ptr_array_sort(order, compare_fetch);
	for (i = 0; i < order->len; i++) {
		SeasonView *view = g_ptr_array_index(order, i);
		AxisMetaSeasonAnswer season;
		g_autoptr(GError) local = NULL;
		gint64 left = (deadline - g_get_monotonic_time()) / 1000;
		guint wait = (guint)CLAMP(left, 0, SEASON_WAIT_MS);

		if (view->number != work->open_season && wait == 0)
			continue;
		if (view->number == work->open_season)
			wait = SEASON_WAIT_MS;
		if (!axis_meta_get_source_season(self->meta, work->id, work->shown, view->number, wait, &season, &local)) {
			if (!g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
				g_debug("a season of title %" G_GINT64_FORMAT " could not be read: %s", work->id, local->message);
			continue;
		}
		if (season.saved && work->seasons_problem == NULL) {
			work->seasons_saved = TRUE;
			work->seasons_problem = g_steal_pointer(&season.problem);
		}
		/* A season whose episodes were never fetched is not one to show. */
		if (season.season != NULL && season.season->episodes_fetched_us > 0)
			view->loaded = g_steal_pointer(&season.season);
		axis_meta_season_answer_clear(&season);
	}
	g_ptr_array_sort(work->seasons, compare_display);
}

static void
title_run(
	AxisWebMeta *self,
	gpointer     data
){
	TitleWork *work = data;
	g_autoptr(GError) local = NULL;
	const AxisMetaTitle *title;
	g_autoptr(GPtrArray) shown = NULL;

	if (!axis_meta_get_title(self->meta, work->id, AXIS_META_DEFAULT_WAIT_MS, &work->answer, &local)) {
		if (g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
			work->missing = TRUE;
		else
			work->error = g_steal_pointer(&local);
		return;
	}
	title = work->answer.title;
	if (title == NULL) {
		work->missing = TRUE;
		return;
	}
	/* What is kept is shown; what is not is fetched in the background, for
	 * the next time (the poster, which is too late for this page, comes
	 * through the placeholder). */
	work->poster = axis_meta_artwork_find(self->meta, title->provider, title->poster_ref, AXIS_META_ART_POSTER, TRUE);
	work->backdrop = axis_meta_artwork_find(self->meta, title->provider, title->backdrop_ref, AXIS_META_ART_BACKDROP,
		TRUE);
	if (title->kind == AXIS_META_KIND_SERIES)
		load_seasons(self, work);
	shown = g_ptr_array_new();
	g_ptr_array_add(shown, (gpointer)title);
	work->actions = axis_web_actions_collect(axis_web_get_actions(work->web), shown, AXIS_WEB_ACTION_PAGE);
}

/* ── Rendering ──────────────────────────────────────────────────── */

static const gchar *
status_label(AxisMetaStatus status)
{
	switch (status) {
	case AXIS_META_STATUS_RELEASED:
		return "Released";
	case AXIS_META_STATUS_CONTINUING:
		return "Continuing";
	case AXIS_META_STATUS_ENDED:
		return "Ended";
	case AXIS_META_STATUS_UPCOMING:
		return "Upcoming";
	default:
		return NULL;
	}
}

static void
line_item(
	HtmxNode    *list,
	const gchar *text
){
	HtmxLi *item = htmx_li_new();

	htmx_node_set_text_content(HTMX_NODE(item), text);
	ADD(list, item);
}

/* "Film", the year, the runtime and the status, side by side. */
static HtmxNode *
title_line(const AxisMetaTitle *title)
{
	HtmxUl *list = htmx_ul_new();
	g_autofree gchar *year = title->year > 0 ? g_strdup_printf("%d", title->year) : NULL;
	g_autofree gchar *runtime = title->runtime > 0 ?
		g_strdup_printf(title->kind == AXIS_META_KIND_SERIES ? "%d min an episode" : "%d min", title->runtime) : NULL;
	const gchar *status = status_label(title->status);

	htmx_element_add_class(HTMX_ELEMENT(list), "title-line");
	line_item(HTMX_NODE(list), title->kind == AXIS_META_KIND_SERIES ? "TV series" : "Movie");
	if (year != NULL)
		line_item(HTMX_NODE(list), year);
	if (runtime != NULL)
		line_item(HTMX_NODE(list), runtime);
	if (status != NULL)
		line_item(HTMX_NODE(list), status);
	return HTMX_NODE(list);
}

static HtmxNode *
hero_new(
	const TitleWork *work,
	HtmxRequest     *request
){
	const AxisMetaTitle *title = work->answer.title;
	HtmxDiv *hero = htmx_div_new();
	HtmxDiv *head = htmx_div_new();
	HtmxDiv *info = htmx_div_new();
	HtmxHeading *name = htmx_h1_new();
	HtmxNode *actions;

	htmx_element_add_class(HTMX_ELEMENT(hero), "title-hero");
	if (work->backdrop != NULL) {
		g_autofree gchar *src = g_strdup_printf("/art/%s", work->backdrop);
		HtmxImg *backdrop = htmx_img_new_with_src(src, "");

		htmx_element_add_class(HTMX_ELEMENT(backdrop), "title-backdrop");
		ADD(hero, backdrop);
	}
	htmx_element_add_class(HTMX_ELEMENT(head), "title-head");
	ADD(head, axis_web_meta_poster_new(title, work->poster, TRUE, 0));

	htmx_element_add_class(HTMX_ELEMENT(info), "title-info");
	htmx_node_set_text_content(HTMX_NODE(name), title->title);
	ADD(info, name);
	if (title->original_title != NULL && g_strcmp0(title->original_title, title->title) != 0) {
		g_autofree gchar *text = g_strdup_printf("Original title: %s", title->original_title);

		ADD(info, axis_web_storage_text("title-original", text));
	}
	ADD(info, title_line(title));
	if (title->overview != NULL)
		ADD(info, axis_web_storage_text("title-overview", title->overview));
	actions = axis_web_actions_render(work->actions, request, AXIS_WEB_ACTION_PAGE, title);
	if (actions != NULL)
		ADD(info, actions);
	ADD(head, info);
	ADD(hero, head);
	return HTMX_NODE(hero);
}

/* The English name of a language code TMDb gives, or NULL. */
static const gchar *
language_name(const gchar *code)
{
	static const gchar *const names[][2] = {
		{ "en", "English" }, { "fr", "French" }, { "de", "German" }, { "it", "Italian" }, { "es", "Spanish" },
		{ "pt", "Portuguese" }, { "ru", "Russian" }, { "ja", "Japanese" }, { "ko", "Korean" }, { "zh", "Chinese" },
		{ "hi", "Hindi" }, { "ar", "Arabic" }, { "tr", "Turkish" }, { "pl", "Polish" }, { "nl", "Dutch" },
		{ "sv", "Swedish" }, { "da", "Danish" }, { "no", "Norwegian" }, { "fi", "Finnish" }, { "th", "Thai" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		if (g_strcmp0(names[i][0], code) == 0)
			return names[i][1];
	}
	return NULL;
}

static HtmxNode *
facts_card(const AxisMetaTitle *title)
{
	gboolean series = title->kind == AXIS_META_KIND_SERIES;
	HtmxNode *card = axis_web_storage_card(series ? "Details of this series" : "Details of this movie");
	HtmxNode *facts = axis_web_browser_facts_new();
	HtmxNode *links = axis_web_meta_id_links(title);
	guint i;

	if (title->original_title != NULL && g_strcmp0(title->original_title, title->title) != 0)
		axis_web_browser_fact(facts, "Original title", title->original_title);
	if (status_label(title->status) != NULL)
		axis_web_browser_fact(facts, "Status", status_label(title->status));
	if (series) {
		if (title->first_air_date != NULL)
			axis_web_browser_fact(facts, "First aired", title->first_air_date);
		if (title->last_air_date != NULL)
			axis_web_browser_fact(facts, "Last aired", title->last_air_date);
	} else if (title->release_date != NULL) {
		axis_web_browser_fact(facts, "Released", title->release_date);
	}
	if (title->runtime > 0) {
		g_autofree gchar *text = g_strdup_printf("%d minutes%s", title->runtime, series ? " an episode" : "");

		axis_web_browser_fact(facts, "Runtime", text);
	}
	if (title->genres != NULL && title->genres->len > 0) {
		g_autoptr(GString) text = g_string_new(NULL);

		for (i = 0; i < title->genres->len; i++)
			g_string_append_printf(text, "%s%s", i > 0 ? ", " : "", (const gchar *)g_ptr_array_index(title->genres, i));
		axis_web_browser_fact(facts, "Genres", text->str);
	}
	if (title->original_language != NULL)
	{
		const gchar *name = language_name(title->original_language);
		g_autofree gchar *text = name != NULL ? g_strdup_printf("%s (%s)", name, title->original_language) :
			g_strdup(title->original_language);

		axis_web_browser_fact(facts, "Original language", text);
	}
	if (title->alt_titles != NULL && title->alt_titles->len > 0) {
		g_autoptr(GString) text = g_string_new(NULL);

		for (i = 0; i < title->alt_titles->len && i < MAX_ALT_TITLES; i++) {
			const AxisMetaAltTitle *alt = g_ptr_array_index(title->alt_titles, i);

			g_string_append_printf(text, "%s%s", i > 0 ? ", " : "", alt->title);
			if (alt->country != NULL)
				g_string_append_printf(text, " (%s)", alt->country);
		}
		if (title->alt_titles->len > MAX_ALT_TITLES)
			g_string_append_printf(text, " and %u more", title->alt_titles->len - MAX_ALT_TITLES);
		axis_web_browser_fact(facts, "Also known as", text->str);
	}
	if (links != NULL)
		axis_web_browser_fact_node(facts, "On other sites", links);
	ADD(card, facts);
	return card;
}

static gchar *
season_meta_text(const SeasonView *view)
{
	g_autoptr(GString) text = g_string_new(NULL);
	gint count = view->loaded != NULL && view->loaded->episodes != NULL ? (gint)view->loaded->episodes->len :
		view->episode_count;

	if (count >= 0)
		g_string_append_printf(text, "%d %s", count, count == 1 ? "episode" : "episodes");
	if (view->air_date != NULL && strlen(view->air_date) >= 4)
		g_string_append_printf(text, "%s%.4s", text->len > 0 ? ", " : "", view->air_date);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static HtmxNode *
season_new(
	const TitleWork *work,
	const SeasonView *view,
	gboolean          open,
	const gchar      *today
){
	const AxisMetaTitle *title = work->answer.title;
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxSpan *name = htmx_span_new();
	HtmxSpan *meta = htmx_span_new();
	g_autofree gchar *id = g_strdup_printf("season-%d", view->number);
	g_autofree gchar *fallback = g_strdup_printf(view->number == 0 ? "Specials" : "Season %d", view->number);
	g_autofree gchar *facts = season_meta_text(view);

	htmx_element_set_id(HTMX_ELEMENT(details), id);
	htmx_element_add_class(HTMX_ELEMENT(details), "season");
	htmx_details_set_open(details, open);
	htmx_element_add_class(HTMX_ELEMENT(name), "season-name");
	htmx_node_set_text_content(HTMX_NODE(name), view->name != NULL ? view->name : fallback);
	htmx_element_add_class(HTMX_ELEMENT(meta), "season-meta");
	htmx_node_set_text_content(HTMX_NODE(meta), facts);
	ADD(summary, name);
	ADD(summary, meta);
	ADD(details, summary);

	if (view->loaded != NULL) {
		ADD(details, season_body_new(view->loaded, provider_label(work), FALSE, NULL, today));
	} else {
		/* Not fetched in time: its episodes arrive when it is opened (htmx),
		 * or on a page of its own; in the numbering shown. */
		gboolean own = work->shown == NULL || g_strcmp0(work->shown, title->provider) == 0;
		g_autofree gchar *path = own ? g_strdup_printf("/title/%" G_GINT64_FORMAT "/season/%d", title->id,
			view->number) : g_strdup_printf("/title/%" G_GINT64_FORMAT "/season/%d?numbering=%s", title->id,
			view->number, work->shown);
		g_autofree gchar *page = own ? g_strdup_printf("/title/%" G_GINT64_FORMAT "?season=%d#season-%d", title->id,
			view->number, view->number) : g_strdup_printf("/title/%" G_GINT64_FORMAT "?season=%d&numbering=%s#season-%d",
			title->id, view->number, work->shown, view->number);
		HtmxDiv *body = htmx_div_new();
		HtmxP *line = htmx_p_new();
		g_autofree gchar *link_text = view->number == 0 ? g_strdup("Show the specials") :
			g_strdup_printf("Show the episodes of season %d", view->number);

		htmx_element_set_attribute(HTMX_ELEMENT(details), "hx-get", path);
		htmx_element_set_attribute(HTMX_ELEMENT(details), "hx-trigger", "toggle once");
		htmx_element_set_attribute(HTMX_ELEMENT(details), "hx-target", "find .season-body");
		htmx_element_set_attribute(HTMX_ELEMENT(details), "hx-swap", "outerHTML");
		htmx_element_add_class(HTMX_ELEMENT(body), "season-body");
		htmx_element_add_class(HTMX_ELEMENT(line), "help");
		ADD(line, axis_web_browser_link(page, link_text));
		ADD(body, line);
		ADD(details, body);
	}
	return HTMX_NODE(details);
}

/* "TMDb", "TheTVDB": a numbering's name in the choice of the two. */
static const gchar *
source_short(
	const TitleWork *work,
	const gchar     *source
){
	if (source != NULL && g_str_has_prefix(source, "tvdb") && g_strcmp0(source, work->answer.title->provider) != 0)
		return "TheTVDB";
	return source_label(work, source);
}

/* The choice of the two numberings (the site's strip of views, a plain link
 * each, the one shown marked), and under it whether the one shown is the
 * one the series follows. */
static HtmxNode *
numbering_choice(const TitleWork *work)
{
	HtmxDiv *block = htmx_div_new();
	HtmxDiv *strip = htmx_div_new();
	HtmxSpan *label = htmx_span_new();
	const gchar *both[2];
	guint i;

	/* The record provider's first, then the other. */
	both[0] = work->answer.title->provider;
	both[1] = g_strcmp0(work->other, work->answer.title->provider) == 0 ? work->shown : work->other;
	htmx_element_add_class(HTMX_ELEMENT(block), "seasons-numbering");
	htmx_element_add_class(HTMX_ELEMENT(strip), "list-links");
	htmx_element_set_attribute(HTMX_ELEMENT(strip), "role", "group");
	htmx_element_set_attribute(HTMX_ELEMENT(strip), "aria-label", "Numbering");
	htmx_element_add_class(HTMX_ELEMENT(label), "list-links-label");
	htmx_node_set_text_content(HTMX_NODE(label), "Numbering");
	ADD(strip, label);
	for (i = 0; i < G_N_ELEMENTS(both); i++) {
		g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT "?numbering=%s#seasons",
			work->answer.title->id, both[i]);
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_node_set_text_content(HTMX_NODE(link), source_short(work, both[i]));
		if (g_strcmp0(both[i], work->shown) == 0) {
			htmx_element_add_class(HTMX_ELEMENT(link), "current");
			htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "true");
		}
		ADD(strip, link);
	}
	ADD(block, strip);
	if (g_strcmp0(work->shown, work->followed) == 0) {
		ADD(block, axis_web_storage_text("help seasons-follows", "This series follows this numbering."));
	} else {
		g_autofree gchar *follows = g_strcmp0(work->followed, work->answer.title->provider) == 0 ?
			g_strdup_printf("%s's numbering", source_label(work, work->followed)) :
			g_strdup(source_label(work, work->followed));
		g_autofree gchar *text = g_strdup_printf("A preview: this series follows %s. Nothing is changed by looking. ",
			follows);
		HtmxP *notice = htmx_p_new();
		HtmxSpan *words = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
		htmx_element_add_class(HTMX_ELEMENT(notice), "seasons-preview");
		htmx_node_set_text_content(HTMX_NODE(words), text);
		ADD(notice, words);
		/* (Changing it is the series' own page, for a series Axis manages.) */
		if (work->admin && work->series_id > 0) {
			g_autofree gchar *change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=%s",
				work->series_id, work->shown);

			ADD(notice, axis_web_browser_link(change, "Change the episode numbering"));
		}
		ADD(block, notice);
	}
	return HTMX_NODE(block);
}

static HtmxNode *
seasons_card(
	AxisWebMeta     *self,
	const TitleWork *work,
	const gchar     *today
){
	HtmxNode *card = axis_web_storage_card("Seasons and air dates");
	gint latest = 0;
	guint i;

	/* Whose numbering they are; for an administrator who can look at the
	 * other one, a choice of the two, the one shown marked, and whether it is
	 * the one the series follows (looking changes nothing).  TheTVDB's
	 * notice beside its seasons. */
	{
		const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");

		if (work->other != NULL)
			ADD(card, numbering_choice(work));
		else {
			g_autofree gchar *whose = g_strdup_printf("Numbered as %s numbers them.", source_label(work, work->shown));

			ADD(card, axis_web_storage_text("help seasons-source", whose));
		}
		if (tvdb != NULL && work->shown != NULL && g_str_has_prefix(work->shown, "tvdb"))
			ADD(card, axis_web_meta_attribution(tvdb));
	}
	htmx_element_set_id(HTMX_ELEMENT(card), "seasons");
	if (work->seasons == NULL || work->seasons->len == 0) {
		ADD(card, axis_web_storage_text("help", "No seasons are listed for this series yet."));
		return card;
	}
	for (i = 0; i < work->seasons->len; i++) {
		const SeasonView *view = g_ptr_array_index(work->seasons, i);

		latest = MAX(latest, view->number);
	}
	for (i = 0; i < work->seasons->len; i++) {
		const SeasonView *view = g_ptr_array_index(work->seasons, i);
		gboolean open = work->open_season >= 0 ? view->number == work->open_season : view->number == latest;

		ADD(card, season_new(work, view, open, today));
	}
	return card;
}

static HtmxResponse *
title_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	TitleWork *work = data;
	const AxisMetaTitle *title;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *today = today_text();
	g_autofree gchar *when = NULL;
	g_autofree gchar *as_of = NULL;
	HtmxResponse *response;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->missing)
		return axis_web_error_respond(request, 404, "Not found", "There is no such title.");
	title = work->answer.title;

	htmx_element_add_class(HTMX_ELEMENT(content), "title-page");
	/* Opened from a list (Discover, the search results): back to it, at this
	 * title's card. */
	{
		g_autofree gchar *from = axis_web_back_from(request);

		if (from != NULL) {
			g_autofree gchar *href = g_strdup_printf("%s#title-%" G_GINT64_FORMAT, from, title->id);
			HtmxP *line = htmx_p_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "series-back");
			ADD(line, axis_web_browser_link(href, g_str_has_prefix(from, "/search") ? "Back to the search results" :
				g_str_has_prefix(from, "/discover") ? "Back to Discover" : "Back to the list"));
			ADD(content, line);
		}
	}
	{
		/* What stands in the way of a wanted title, said first. */
		HtmxNode *notices = axis_web_actions_render(work->actions, request, AXIS_WEB_ACTION_NOTICE, title);

		if (notices != NULL)
			ADD(content, notices);
	}
	if (work->answer.saved) {
		g_autofree gchar *saved = axis_web_meta_when_text(work->answer.fetched_us);
		g_autofree gchar *why = axis_web_meta_why_text(provider_label(work), work->answer.problem);
		g_autofree gchar *text = g_strdup_printf("%s This page shows the details saved on %s.%s", why, saved,
			title->detail ? "" : " Only a search result's short summary was saved, so some details are missing.");

		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
	ADD(content, hero_new(work, request));
	{
		/* A module's own cards about the title (a film's releases, its
		 * history), after its action area and before the facts. */
		HtmxNode *detail = axis_web_actions_render(work->actions, request, AXIS_WEB_ACTION_DETAIL, title);

		if (detail != NULL)
			ADD(content, detail);
	}
	ADD(content, facts_card(title));
	if (title->kind == AXIS_META_KIND_SERIES) {
		if (work->seasons_saved) {
			g_autofree gchar *why = axis_web_meta_why_text(provider_label(work), work->seasons_problem);
			g_autofree gchar *text = g_strdup_printf("%s Some seasons' episodes are not shown: open one to try "
				"again.", why);

			ADD(content, axis_web_storage_text("notice notice-warning", text));
		}
		ADD(content, seasons_card(self, work, today));
	}
	when = axis_web_meta_when_text(work->answer.fetched_us);
	as_of = g_strdup_printf("Details last updated %s.", when);
	ADD(content, axis_web_storage_text("as-of", as_of));
	if (work->answer.provider != NULL)
		ADD(content, axis_web_meta_attribution(work->answer.provider));
	response = axis_web_page_respond(request, title->title, HTMX_NODE(content));
	return response;
}

static HtmxResponse *
handle_title(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	const gchar *season = htmx_request_get_query_param(request, "season");
	TitleWork *work;
	gint64 number = 0;

	if (!parse_title_id(id, &number))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	work = g_new0(TitleWork, 1);
	work->web = axis_web_from_request(request);
	work->id = number;
	work->open_season = -1;
	if (season != NULL && !parse_season(season, &work->open_season))
		work->open_season = -1;
	work->admin = axis_web_request_is_admin(request);
	{
		const gchar *numbering = htmx_request_get_query_param(request, "numbering");

		/* Another numbering is an administrator's to look at. */
		if (work->admin && numbering != NULL && strlen(numbering) <= 40 && axis_meta_source_valid(numbering))
			work->asked = g_strdup(numbering);
	}
	return axis_web_meta_dispatch(self, request, title_run, title_done, work, title_work_free);
}

/* ── One season, for htmx ───────────────────────────────────────── */

typedef struct
{
	gint64                id;
	gint                  number;
	gchar                *numbering;	/* (nullable) the source asked for */
	AxisMetaSeasonAnswer  answer;
	GError               *error;
} SeasonWork;

static void
season_work_free(gpointer data)
{
	SeasonWork *work = data;

	axis_meta_season_answer_clear(&work->answer);
	g_clear_error(&work->error);
	g_free(work->numbering);
	g_free(work);
}

static void
season_run(
	AxisWebMeta *self,
	gpointer     data
){
	SeasonWork *work = data;
	g_autoptr(GError) local = NULL;
	const AxisMetaProviderInfo *tvdb = axis_meta_provider_find(self->meta, "tvdb");

	/* TheTVDB's numbering only while it can be asked. */
	if (work->numbering != NULL && g_str_has_prefix(work->numbering, "tvdb") &&
	    (tvdb == NULL || !axis_meta_provider_offered(self->meta, tvdb)))
		g_clear_pointer(&work->numbering, g_free);
	/* None asked for (or not an administrator's to ask): the numbering the
	 * series is followed in, as the title page lists its seasons. */
	if (work->numbering == NULL) {
		g_autoptr(GPtrArray) seasons = NULL;

		if (!axis_meta_load_source_seasons(self->meta, work->id, NULL, &work->numbering, NULL, NULL, &seasons, &local)) {
			work->error = g_steal_pointer(&local);
			return;
		}
	}
	if (!axis_meta_get_source_season(self->meta, work->id, work->numbering, work->number, SEASON_WAIT_MS,
	        &work->answer, &local))
		work->error = g_steal_pointer(&local);
}

static HtmxResponse *
season_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	SeasonWork *work = data;
	g_autofree gchar *today = today_text();
	g_autoptr(HtmxNode) body = NULL;
	g_autofree gchar *markup = NULL;
	HtmxResponse *response;

	if (g_error_matches(work->error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such season.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	body = season_body_new(work->answer.season, "The details provider", work->answer.saved, work->answer.problem,
		today);
	markup = htmx_element_render(HTMX_ELEMENT(body));
	response = htmx_response_new();
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return response;
}

static HtmxResponse *
handle_season(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	const gchar *number = params != NULL ? g_hash_table_lookup(params, "number") : NULL;
	SeasonWork *work;
	gint64 title_id = 0;
	gint season = 0;

	if (!parse_title_id(id, &title_id) || !parse_season(number, &season))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	if (!htmx_request_is_htmx(request)) {
		/* Without script the same episodes are on a page of their own. */
		const gchar *numbering = htmx_request_get_query_param(request, "numbering");
		gboolean other = numbering != NULL && strlen(numbering) <= 40 && axis_meta_source_valid(numbering);
		g_autofree gchar *page = other ? g_strdup_printf("/title/%" G_GINT64_FORMAT "?season=%d&numbering=%s#season-%d",
			title_id, season, numbering, season) : g_strdup_printf("/title/%" G_GINT64_FORMAT "?season=%d#season-%d",
			title_id, season, season);

		return axis_web_auth_redirect(request, page);
	}
	work = g_new0(SeasonWork, 1);
	work->id = title_id;
	work->number = season;
	{
		const gchar *numbering = htmx_request_get_query_param(request, "numbering");

		if (axis_web_request_is_admin(request) && numbering != NULL && strlen(numbering) <= 40 &&
		    axis_meta_source_valid(numbering))
			work->numbering = g_strdup(numbering);
	}
	return axis_web_meta_dispatch(self, request, season_run, season_done, work, season_work_free);
}

void
axis_web_title_register(
	AxisWebMeta *self,
	AxisWeb     *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/title/:id", AXIS_WEB_ROUTE_DEFAULT, handle_title, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/title/:id/season/:number", AXIS_WEB_ROUTE_DEFAULT, handle_season,
		self);
}
