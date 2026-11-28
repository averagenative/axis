/*
 * axis-web-discover.c - the Discover module: Discover movies, Discover TV,
 * Not interested, and the administrators' actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A page is drawn from what the Discover layer keeps; nothing here asks the
 * provider.  Each card is the shell's result card (axis-web-meta-view.c)
 * with the year and rating in its first line, why it is recommended under
 * its overview, and a row with the movies or TV module's piece (its state,
 * or the one-step add) and, for an administrator, Not interested.
 */

#include "web/axis-web-discover.h"

#include <json-glib/json-glib.h>
#include <string.h>

#include "jobs/axis-job-registry.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-meta-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* Titles a page: one page of TMDb's lists. */
#define PAGE_SIZE   20

/* The furthest page asked for (the layer keeps at most 5 x 20 a list). */
#define MAX_PAGE    500

/* Tasks waiting for a thread beyond this are answered 503. */
#define MAX_PENDING 16

#define MOVIES_PATH        "/discover/movies"
#define TV_PATH            "/discover/tv"
#define NOT_INTERESTED_PATH "/discover/not-interested"

struct _AxisWebDiscover
{
	AxisDiscover *discover;	/* owned reference */
	AxisWeb      *web;	/* the shell its routes are registered with (borrowed) */
	guint         pending;
	gboolean      stopping;
};

/* ── Work off the main loop ─────────────────────────────────────── */

typedef void (*WorkFunc)(AxisWebDiscover *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebDiscover *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebDiscover *owner;
	WorkFunc         work;
	DoneFunc         done;
	HtmxRequest     *request;
	HtmxResponse    *deferred;
	gpointer         data;
	GDestroyNotify   free_data;
} Task;

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;

	task->work(task->owner, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = task->done(task->owner, task->request, task->data);
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_object_unref(task->request);
	g_object_unref(task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebDiscover *self,
	HtmxRequest     *request,
	WorkFunc         work,
	DoneFunc         done,
	gpointer         data,
	GDestroyNotify   free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		if (free_data != NULL)
			free_data(data);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}
	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	task->data = data;
	task->free_data = free_data;
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── Words ──────────────────────────────────────────────────────── */

typedef struct
{
	AxisMetaList  list;
	const gchar  *film_label;
	const gchar  *series_label;
	const gchar  *film_help;
	const gchar  *series_help;
} ListWords;

static const ListWords list_words[] = {
	{ AXIS_META_LIST_POPULAR, "Popular", "Popular",
	  "The movies many people are looking at on TMDb now.",
	  "The series many people are looking at on TMDb now." },
	{ AXIS_META_LIST_TRENDING, "Trending this week", "Trending this week",
	  "The movies rising fastest on TMDb this week.",
	  "The series rising fastest on TMDb this week." },
	{ AXIS_META_LIST_NOW, "In cinemas", "On the air",
	  "Movies showing in cinemas now, in the TMDb region set in Settings, under Movie and TV details.",
	  "Series with a new episode in the next seven days." },
	{ AXIS_META_LIST_UPCOMING, "Coming soon", NULL,
	  "Movies coming to cinemas soon, in the TMDb region set in Settings, under Movie and TV details.", NULL },
	{ AXIS_META_LIST_TOP_RATED, "Top rated", "Top rated",
	  "The best rated movies on TMDb, by its members' votes.",
	  "The best rated series on TMDb, by its members' votes." },
	{ AXIS_META_LIST_RECOMMENDED, "Recommended for you", "Recommended for you",
	  "What TMDb recommends for the movies you have and want, the most recommended first. A movie you have is "
	  "never listed here.",
	  "What TMDb recommends for the series you have and want, the most recommended first. A series you have is "
	  "never listed here." }
};

static const ListWords *
words_of(AxisMetaList list)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(list_words); i++) {
		if (list_words[i].list == list)
			return &list_words[i];
	}
	return &list_words[0];
}

static const gchar *
list_label(
	AxisMetaKind kind,
	AxisMetaList list
){
	const ListWords *words = words_of(list);
	const gchar *label = kind == AXIS_META_KIND_FILM ? words->film_label : words->series_label;

	return label != NULL ? label : words->film_label;
}

static const gchar *
list_help(
	AxisMetaKind kind,
	AxisMetaList list
){
	const ListWords *words = words_of(list);
	const gchar *help = kind == AXIS_META_KIND_FILM ? words->film_help : words->series_help;

	return help != NULL ? help : words->film_help;
}

static const gchar *
kind_plural(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? "movies" : "series";
}

static const gchar *
kind_path(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? MOVIES_PATH : TV_PATH;
}

/* What a ?done= says. */
static const gchar *
done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "added", "Added. Its card below shows its new state, and its page has the rest." },
		{ "managed", "Axis manages it now: its files on disk count as in the library." },
		{ "add_defaults", "Saved: your Adds from a list go there now. A title's own Options still choose for it "
		  "alone." },
		{ "hidden", "Hidden: the title that stood here before this one is gone from Discover. The Not interested page "
		  "lists it and can show it again." },
		{ "shown", "It is shown in Discover again." },
		{ "refreshing", "Axis is fetching the lists from TMDb now. They appear here in a minute or two." },
		{ "already_refreshing", "Axis is fetching the lists already." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(messages[i].key, key) == 0)
			return messages[i].text;
	}
	return NULL;
}

/* A rating in tenths, "7.4". */
static gchar *
rating_text(gint tenths)
{
	return g_strdup_printf("%d.%d", tenths / 10, tenths % 10);
}

/* "2024, rated 7.4 of 10", "2024, no rating yet", "Rated 7.4 of 10". */
static gchar *
card_line(const AxisDiscoverItem *item)
{
	g_autofree gchar *rating = item->rating >= 0 ? rating_text(item->rating) : NULL;

	if (item->title->year > 0)
		return rating != NULL ? g_strdup_printf("%d, rated %s of 10", item->title->year, rating) :
			g_strdup_printf("%d, no rating yet", item->title->year);
	return rating != NULL ? g_strdup_printf("Rated %s of 10", rating) : g_strdup("No rating yet");
}

/* "Because you have A", "Because you have A and B", "Because you have A, B
 * and 3 more"; %NULL when it is not a recommendation. */
static gchar *
because_text(const AxisDiscoverItem *item)
{
	guint named = item->because != NULL ? item->because->len : 0;
	guint more = item->because_n > named ? item->because_n - named : 0;

	if (named == 0)
		return NULL;
	if (named == 1 && more == 0)
		return g_strdup_printf("Because you have %s", (const gchar *)g_ptr_array_index(item->because, 0));
	if (named == 1)
		return g_strdup_printf("Because you have %s and %u more", (const gchar *)g_ptr_array_index(item->because, 0),
			more);
	if (more == 0)
		return g_strdup_printf("Because you have %s and %s", (const gchar *)g_ptr_array_index(item->because, 0),
			(const gchar *)g_ptr_array_index(item->because, 1));
	return g_strdup_printf("Because you have %s, %s and %u more", (const gchar *)g_ptr_array_index(item->because, 0),
		(const gchar *)g_ptr_array_index(item->because, 1), more);
}

/* "12 hours", "2 days". */
static gchar *
period_text(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 86400, seconds == 86400 ? "day" : "days");
	return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 3600, seconds < 7200 ? "hour" : "hours");
}

/* ── What a page asks for ───────────────────────────────────────── */

typedef struct
{
	AxisMetaKind  kind;
	AxisMetaList  list;
	gint          genre;
	gint          year_from;
	gint          year_to;
	gint          rating;	/* whole points: 5 to 8, 0 for any */
	gboolean      hide;
	gboolean      unreleased;	/* hide what has not come out yet */
	guint         page;
	gchar        *done;
	gint64        added;	/* the title an Add added, its card gone from a "hide" list; 0 for none */
} Wanted;

/* A whole number parameter within [@min, @max], else 0. */
static gint
param_int(
	HtmxRequest *request,
	const gchar *name,
	gint         min,
	gint         max
){
	const gchar *text = htmx_request_get_query_param(request, name);
	guint64 value = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, (guint64)MAX(min, 0), (guint64)max, &value, NULL))
		return 0;
	return (gint)value;
}

static void
wanted_from(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	Wanted       *want
){
	const gchar *list = htmx_request_get_query_param(request, "list");
	const gchar *done = htmx_request_get_query_param(request, "done");

	memset(want, 0, sizeof(*want));
	want->kind = kind;
	want->list = AXIS_META_LIST_POPULAR;
	if (list != NULL && !axis_meta_list_from_string(list, &want->list))
		want->list = AXIS_META_LIST_POPULAR;
	want->genre = param_int(request, "genre", 1, G_MAXINT32);
	want->year_from = param_int(request, "from", AXIS_META_YEAR_MIN, AXIS_META_YEAR_MAX);
	want->year_to = param_int(request, "to", AXIS_META_YEAR_MIN, AXIS_META_YEAR_MAX);
	want->rating = param_int(request, "rating", 1, 9);
	want->hide = g_strcmp0(htmx_request_get_query_param(request, "hide"), "yes") == 0;
	want->unreleased = g_strcmp0(htmx_request_get_query_param(request, "unreleased"), "yes") == 0;
	want->page = MAX(1, param_int(request, "page", 1, MAX_PAGE));
	if (done != NULL && done_message(done) != NULL)
		want->done = g_strdup(done);
	want->added = param_int(request, AXIS_WEB_BACK_ADDED, 1, G_MAXINT32);
}

static gboolean
filtered(const Wanted *want)
{
	return want->genre != 0 || want->year_from != 0 || want->year_to != 0 || want->rating != 0 || want->hide ||
		want->unreleased;
}

/* The address of @list (its page @page) with @want's filters, or without. */
static gchar *
list_url(
	const Wanted *want,
	AxisMetaList  list,
	guint         page,
	gboolean      with_filters
){
	GString *url = g_string_new(kind_path(want->kind));

	g_string_append_printf(url, "?list=%s", axis_meta_list_to_string(list));
	if (with_filters) {
		if (want->genre != 0)
			g_string_append_printf(url, "&genre=%d", want->genre);
		if (want->year_from != 0)
			g_string_append_printf(url, "&from=%d", want->year_from);
		if (want->year_to != 0)
			g_string_append_printf(url, "&to=%d", want->year_to);
		if (want->rating != 0)
			g_string_append_printf(url, "&rating=%d", want->rating);
		if (want->hide)
			g_string_append(url, "&hide=yes");
		if (want->unreleased)
			g_string_append(url, "&unreleased=yes");
	}
	if (page > 1)
		g_string_append_printf(url, "&page=%u", page);
	return g_string_free(url, FALSE);
}

/* ── A list's page ──────────────────────────────────────────────── */

typedef struct
{
	AxisWeb           *web;
	Wanted             want;
	AxisMetaList       lists[AXIS_META_N_LISTS];
	guint              n_lists;
	guint              recommend;
	gint64             refresh_after;
	AxisDiscoverPage   page;
	GPtrArray         *genres;	/* AxisMetaGenre */
	GPtrArray         *titles;	/* AxisMetaTitle *, borrowed from the page's items */
	gchar            **posters;	/* one per item, nullable each */
	AxisWebActionRun  *actions;
	gboolean           queued;	/* the first refresh was queued by this view */
	gchar             *added_text;	/* what the notice says of a title added whose card is gone, or %NULL */
	GError            *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;
	guint i;

	if (work->posters != NULL) {
		for (i = 0; work->page.items != NULL && i < work->page.items->len; i++)
			g_free(work->posters[i]);
		g_free(work->posters);
	}
	axis_web_actions_run_free(work->actions);
	g_clear_pointer(&work->titles, g_ptr_array_unref);
	g_clear_pointer(&work->genres, g_ptr_array_unref);
	axis_discover_page_clear(&work->page);
	g_free(work->want.done);
	g_free(work->added_text);
	g_clear_error(&work->error);
	g_free(work);
}

/* The notice for an Add whose card is gone (hide is on): it names the title
 * of "added=<id>" and does not point at a card.  Needs the stored title; the
 * notice is left to the plain one when it is not there. */
static void
added_text_read(
	AxisWebDiscover *self,
	ListWork        *work
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) local = NULL;

	if (work->want.done == NULL || work->want.added <= 0)
		return;
	/* A title hidden from Discover is named whether the list hides what is
	 * had or not: its card is gone either way, and the notice stands at the
	 * card that took its place. */
	if (!work->want.hide && g_strcmp0(work->want.done, "hidden") != 0)
		return;
	memset(&answer, 0, sizeof(answer));
	if (axis_meta_get_title(axis_discover_get_meta(self->discover), work->want.added, 0, &answer, &local) &&
	    answer.title != NULL && answer.title->kind == work->want.kind) {
		g_autofree gchar *label = answer.title->year > 0 ?
			g_strdup_printf("%s (%d)", answer.title->title, answer.title->year) : g_strdup(answer.title->title);

		if (g_strcmp0(work->want.done, "hidden") == 0)
			work->added_text = g_strdup_printf("Hidden from Discover: %s. The Not interested page lists it and can "
				"show it again.", label);
		else if (g_strcmp0(work->want.done, "added") == 0)
			work->added_text = g_strdup_printf("Added %s to %s. It is not listed here now, as the list hides what you "
				"have or want; its page has the details.", label, work->want.kind == AXIS_META_KIND_FILM ?
				"Movies" : "TV");
		else
			work->added_text = axis_web_meta_added_text(work->want.done, answer.title);
	}
	axis_meta_title_answer_clear(&answer);
}

static gboolean
list_shown(
	const ListWork *work,
	AxisMetaList    list
){
	guint i;

	for (i = 0; i < work->n_lists; i++) {
		if (work->lists[i] == list)
			return TRUE;
	}
	return FALSE;
}

/* The posters of @items (AxisDiscoverItem), one each, nullable each: those
 * kept are shown, the others are fetched now, in the background, and arrive
 * on their own.  @titles gets the items' titles (borrowed).  Transfer full:
 * each, and the vector (of at least one). */
static gchar **
item_posters(
	AxisWebDiscover *self,
	const GPtrArray *items,
	GPtrArray       *titles
){
	gchar **posters = g_new0(gchar *, MAX(items->len, 1));
	guint i;

	if (items->len > 0) {
		g_autofree const gchar **providers = g_new0(const gchar *, items->len);
		g_autofree const gchar **refs = g_new0(const gchar *, items->len);

		for (i = 0; i < items->len; i++) {
			const AxisDiscoverItem *item = g_ptr_array_index(items, i);

			providers[i] = item->title->provider;
			refs[i] = item->title->poster_ref;
			g_ptr_array_add(titles, item->title);
		}
		axis_meta_artwork_find_many(axis_discover_get_meta(self->discover), providers, refs, items->len,
			AXIS_META_ART_POSTER_SMALL, TRUE, posters);
	}
	return posters;
}

static void
list_run(
	AxisWebDiscover *self,
	gpointer         data
){
	ListWork *work = data;
	AxisMetaKind kind = work->want.kind;
	const AxisMetaProviderInfo *provider = axis_discover_provider(self->discover, kind, NULL);
	AxisMetaList offered[AXIS_META_N_LISTS];
	AxisDiscoverQuery query;
	guint n;
	guint i;

	work->recommend = axis_discover_recommend_per_run(self->discover);
	work->refresh_after = axis_discover_refresh_after(self->discover);
	n = provider != NULL ? axis_discover_lists(provider, kind, offered) : 0;
	for (i = 0; i < n; i++) {
		/* Recommended for you is off with its setting at 0. */
		if (offered[i] == AXIS_META_LIST_RECOMMENDED && work->recommend == 0)
			continue;
		work->lists[work->n_lists++] = offered[i];
	}
	if (work->n_lists > 0 && !list_shown(work, work->want.list))
		work->want.list = work->lists[0];

	memset(&query, 0, sizeof(query));
	query.kind = kind;
	query.list = work->want.list;
	query.genre = work->want.genre;
	query.year_from = work->want.year_from;
	query.year_to = work->want.year_to;
	query.min_rating = work->want.rating * 10;
	query.hide_have = work->want.hide;
	query.hide_unreleased = work->want.unreleased;
	query.offset = (work->want.page - 1) * PAGE_SIZE;
	query.limit = PAGE_SIZE;
	if (work->n_lists == 0) {
		/* No provider offers lists: the page says so, from an empty page. */
		memset(&work->page, 0, sizeof(work->page));
		work->page.items = g_ptr_array_new();
	} else if (!axis_discover_read(self->discover, &query, &work->page, &work->error)) {
		return;
	}
	if (!axis_discover_genres(self->discover, kind, &work->genres, &work->error))
		return;
	added_text_read(self, work);

	work->titles = g_ptr_array_new();
	work->posters = item_posters(self, work->page.items, work->titles);
	work->actions = axis_web_actions_collect(axis_web_get_actions(work->web), work->titles, AXIS_WEB_ACTION_DISCOVER);

	/* Never fetched at all: the first fetch is queued (once; the job's
	 * own de-duplication keeps it to one). */
	if (work->page.provider != NULL && work->page.ready && !work->page.tried_ever) {
		g_autoptr(GError) local = NULL;

		if (axis_discover_request_refresh(self->discover, FALSE, NULL, NULL, &local))
			work->queued = TRUE;
		else
			g_message("discover: the first refresh could not be queued: %s", local->message);
	}
}

static HtmxNode *
list_tabs(const ListWork *work)
{
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *items = htmx_ul_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(nav), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Lists");
	for (i = 0; i < work->n_lists; i++) {
		g_autofree gchar *href = list_url(&work->want, work->lists[i], 1, TRUE);
		HtmxLi *item = htmx_li_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_node_set_text_content(HTMX_NODE(link), list_label(work->want.kind, work->lists[i]));
		if (work->lists[i] == work->want.list) {
			htmx_element_add_class(HTMX_ELEMENT(link), "current");
			htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "page");
		}
		ADD(item, link);
		ADD(items, item);
	}
	ADD(nav, items);
	return HTMX_NODE(nav);
}

static HtmxNode *
labelled(
	const gchar *id,
	const gchar *text,
	HtmxNode    *control
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), text);
	htmx_element_set_id(HTMX_ELEMENT(control), id);
	ADD(field, label);
	ADD(field, control);
	return HTMX_NODE(field);
}

static HtmxNode *
year_input(
	const gchar *name,
	gint         value
){
	HtmxInput *input = htmx_input_new(HTMX_INPUT_NUMBER);
	g_autofree gchar *text = value > 0 ? g_strdup_printf("%d", value) : NULL;
	g_autofree gchar *min = g_strdup_printf("%d", AXIS_META_YEAR_MIN);
	g_autofree gchar *max = g_strdup_printf("%d", AXIS_META_YEAR_MAX);

	htmx_input_set_name(input, name);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "min", min);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "max", max);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "inputmode", "numeric");
	if (text != NULL)
		htmx_input_set_value(input, text);
	return HTMX_NODE(input);
}

/* The filters: a plain GET form of this page. */
static HtmxNode *
filters_form(const ListWork *work)
{
	HtmxForm *form = htmx_form_new();
	HtmxSelect *genre = htmx_select_new_with_name("genre");
	HtmxSelect *rating = htmx_select_new_with_name("rating");
	HtmxOption *any;
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", kind_path(work->want.kind));
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(form), "discover-filters");
	ADD(form, axis_web_auth_hidden("list", axis_meta_list_to_string(work->want.list)));

	any = htmx_option_new_with_value("", "Any genre");
	ADD(genre, any);
	for (i = 0; work->genres != NULL && i < work->genres->len; i++) {
		const AxisMetaGenre *g = g_ptr_array_index(work->genres, i);
		g_autofree gchar *value = g_strdup_printf("%d", g->id);
		HtmxOption *option = htmx_option_new_with_value(value, g->name);

		htmx_option_set_selected(option, g->id == work->want.genre);
		ADD(genre, option);
	}
	ADD(form, labelled("discover-genre", "Genre", HTMX_NODE(genre)));
	ADD(form, labelled("discover-from", "From year", year_input("from", work->want.year_from)));
	ADD(form, labelled("discover-to", "To year", year_input("to", work->want.year_to)));
	{
		static const gchar *const values[] = { "", "5", "6", "7", "8", NULL };
		static const gchar *const labels[] = { "Any rating", "5 and up", "6 and up", "7 and up", "8 and up", NULL };

		for (i = 0; values[i] != NULL; i++) {
			HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);
			gint value = values[i][0] != '\0' ? values[i][0] - '0' : 0;

			htmx_option_set_selected(option, value == work->want.rating);
			ADD(rating, option);
		}
	}
	ADD(form, labelled("discover-rating", "Rating", HTMX_NODE(rating)));
	if (work->want.list != AXIS_META_LIST_RECOMMENDED) {
		HtmxDiv *check = htmx_div_new();
		HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
		HtmxLabel *label = htmx_label_new_for("discover-hide");

		htmx_element_add_class(HTMX_ELEMENT(check), "field-check");
		htmx_input_set_name(box, "hide");
		htmx_input_set_value(box, "yes");
		htmx_element_set_id(HTMX_ELEMENT(box), "discover-hide");
		if (work->want.hide)
			htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
		htmx_node_set_text_content(HTMX_NODE(label), "Hide what I have or want");
		htmx_element_set_attribute(HTMX_ELEMENT(label), "title",
			"Leave out what is in the library or on a wanted list");
		ADD(check, box);
		ADD(check, label);
		ADD(form, check);
	}
	{
		HtmxDiv *check = htmx_div_new();
		HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
		HtmxLabel *label = htmx_label_new_for("discover-unreleased");

		htmx_element_add_class(HTMX_ELEMENT(check), "field-check");
		htmx_input_set_name(box, "unreleased");
		htmx_input_set_value(box, "yes");
		htmx_element_set_id(HTMX_ELEMENT(box), "discover-unreleased");
		if (work->want.unreleased)
			htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
		htmx_node_set_text_content(HTMX_NODE(label), "Hide unreleased");
		htmx_element_set_attribute(HTMX_ELEMENT(label), "title",
			"Leave out what has not come out yet: a movie that is not available yet, a series that has not begun");
		ADD(check, box);
		ADD(check, label);
		ADD(form, check);
	}
	ADD(form, axis_web_auth_button("Apply the filters", "button-small"));
	if (filtered(&work->want)) {
		g_autofree gchar *clear = list_url(&work->want, work->want.list, 1, FALSE);

		ADD(form, axis_web_browser_link(clear, "Clear the filters"));
	}
	return HTMX_NODE(form);
}

/* The block of the cards: where the form of a card that is gone (hidden, or
 * shown again) comes back to. */
#define CARDS_PLACE "discover-cards"

/* The place the refresh form comes back to. */
#define REFRESH_PLACE "discover-refresh"

/* The id of a title's card, the place its forms come back to. */
static gchar *
title_place(gint64 title_id)
{
	return g_strdup_printf("title-%" G_GINT64_FORMAT, title_id);
}

/* What a post that came back to a card says there, naming the title when it
 * is the card's: "Added Paper Lantern (2019) to Movies. ..."  Transfer
 * full; %NULL for none. */
static gchar *
done_at_card(
	const gchar     *done,
	const gchar     *place,
	const GPtrArray *items
){
	guint i;

	if (done == NULL || done_message(done) == NULL)
		return NULL;
	for (i = 0; place != NULL && items != NULL && i < items->len; i++) {
		const AxisMetaTitle *title = ((const AxisDiscoverItem *)g_ptr_array_index(items, i))->title;
		g_autofree gchar *card = title_place(title->id);
		gchar *text;

		if (strcmp(card, place) != 0)
			continue;
		text = axis_web_meta_added_text(done, title);
		if (text != NULL)
			return text;
	}
	return g_strdup(done_message(done));
}

/* Not interested, under a card (administrators); @next: the card it comes
 * back to, the card itself being gone (%NULL: the block of the cards). */
static HtmxNode *
dismiss_form(
	HtmxRequest *request,
	gint64       title_id,
	gint64       next
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, title_id);
	g_autofree gchar *place = next > 0 ? title_place(next) : g_strdup(CARDS_PLACE);
	HtmxNode *back = axis_web_back_field(request);
	HtmxNode *form = axis_web_auth_post_form(request, "/discover/dismiss");
	HtmxNode *button = axis_web_auth_button("Hide from Discover", "button-quiet button-small");

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(form), "discover-dismiss");
	ADD(form, axis_web_auth_hidden("title", id));
	ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, place));
	if (back != NULL)
		ADD(form, back);
	htmx_element_set_attribute(HTMX_ELEMENT(button), "title", "Hide this title from Discover for everyone. The Not "
		"interested page lists it and can show it again.");
	ADD(form, button);
	return form;
}

/*
 * A title's card.  @hide: the list hides what is had or wanted, so an Add
 * takes the card away and comes back to @after's (the block of the cards
 * without one).  @dismiss: administrators get "Hide from Discover", which
 * comes back to @next's card.
 */
static HtmxNode *
card_new(
	AxisWebActionRun       *actions,
	gboolean                hide,
	gboolean                dismiss,
	HtmxRequest            *request,
	const AxisDiscoverItem *item,
	const AxisDiscoverItem *next,
	const AxisDiscoverItem *after,
	const gchar            *poster,
	AxisWebNotice          *notice
){
	g_autofree gchar *line = card_line(item);
	g_autofree gchar *because = because_text(item);
	g_autofree gchar *place = title_place(item->title->id);
	/* Its page knows this list, and comes back to this card. */
	g_autofree gchar *href = axis_web_back_title_href(request, item->title->id);
	HtmxDiv *extra = htmx_div_new();
	HtmxDiv *row = htmx_div_new();
	HtmxNode *piece;
	HtmxNode *card;

	htmx_element_add_class(HTMX_ELEMENT(extra), "discover-extra");
	axis_web_notice_put(notice, HTMX_NODE(extra), place);
	if (because != NULL)
		ADD(extra, axis_web_storage_text("title-card-because", because));
	htmx_element_add_class(HTMX_ELEMENT(row), "discover-actions");
	/* Hiding what is had or wanted: an Add takes its card away, and comes
	 * back to the next one (the block of the cards for the last). */
	if (hide) {
		g_autofree gchar *back_to = after != NULL ? title_place(after->title->id) : g_strdup(CARDS_PLACE);

		axis_web_back_set_add_return(request, back_to);
	}
	piece = axis_web_actions_render(actions, request, AXIS_WEB_ACTION_DISCOVER, item->title);
	axis_web_back_set_add_return(request, NULL);
	if (piece != NULL)
		ADD(row, piece);
	if (dismiss && axis_web_request_is_admin(request))
		ADD(row, dismiss_form(request, item->title->id, next != NULL ? next->title->id : 0));
	ADD(extra, row);
	card = axis_web_meta_card_new_full(item->title, poster, NULL, FALSE, href, line, HTMX_NODE(extra));
	htmx_element_set_id(HTMX_ELEMENT(card), place);
	return card;
}

static HtmxNode *
pager_new(
	const Wanted *want,
	guint         total
){
	HtmxDiv *foot = htmx_div_new();
	guint first = (want->page - 1) * PAGE_SIZE + 1;
	guint last = MIN(total, want->page * PAGE_SIZE);
	const gchar *noun = want->kind == AXIS_META_KIND_FILM ? (total == 1 ? "movie" : "movies") : "series";
	g_autofree gchar *count = g_strdup_printf("Showing %u to %u of %u %s.", first, last, total, noun);

	htmx_element_add_class(HTMX_ELEMENT(foot), "list-foot");
	ADD(foot, axis_web_storage_text("list-count", count));
	if (want->page > 1 || last < total) {
		HtmxNav *pager = htmx_nav_new();

		htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
		htmx_element_set_attribute(HTMX_ELEMENT(pager), "aria-label", "Pages");
		if (want->page > 1) {
			g_autofree gchar *href = list_url(want, want->list, want->page - 1, TRUE);

			ADD(pager, axis_web_browser_link(href, "Previous page"));
		}
		if (last < total) {
			g_autofree gchar *href = list_url(want, want->list, want->page + 1, TRUE);

			ADD(pager, axis_web_browser_link(href, "Next page"));
		}
		ADD(foot, pager);
	}
	return HTMX_NODE(foot);
}

/* No provider can be asked: what is needed and where. */
static HtmxNode *
no_key_notice(
	HtmxRequest            *request,
	const AxisDiscoverPage *page
){
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "notice");
	htmx_element_add_class(HTMX_ELEMENT(box), "notice-warning");
	ADD(box, axis_web_storage_text(NULL, "Discover needs a TMDb token: its lists come from TMDb, which Axis asks with "
		"your own token, free of charge. Without one Axis asks nothing."));
	if (page->stored) {
		g_autofree gchar *when = axis_web_meta_when_text(page->fetched_us);
		g_autofree gchar *text = g_strdup_printf("The list below is the one fetched on %s.", when);

		ADD(box, axis_web_storage_text(NULL, text));
	}
	if (axis_web_request_is_admin(request))
		ADD(box, axis_web_browser_link("/admin/settings#module-metadata", "Add a TMDb token in Settings"));
	else
		ADD(box, axis_web_storage_text("help", "An administrator can add one in Settings."));
	return HTMX_NODE(box);
}

/* "Fetch the lists now" (administrators). */
static HtmxNode *
refresh_form(
	HtmxRequest   *request,
	AxisWebNotice *notice
){
	HtmxDiv *box = htmx_div_new();
	HtmxNode *form = axis_web_auth_post_form(request, "/discover/refresh");
	HtmxNode *back = axis_web_back_field(request);

	htmx_element_set_id(HTMX_ELEMENT(box), REFRESH_PLACE);
	axis_web_notice_put(notice, HTMX_NODE(box), REFRESH_PLACE);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, REFRESH_PLACE));
	if (back != NULL)
		ADD(form, back);
	ADD(form, axis_web_auth_button("Fetch the lists now", "button-small"));
	ADD(box, form);
	return HTMX_NODE(box);
}

static void
add_page_state(
	ListWork    *work,
	HtmxRequest *request,
	HtmxNode    *content
){
	const AxisDiscoverPage *page = &work->page;
	AxisMetaKind kind = work->want.kind;
	gboolean recommended = work->want.list == AXIS_META_LIST_RECOMMENDED;

	if (page->provider != NULL && !page->ready) {
		ADD(content, no_key_notice(request, page));
		return;
	}
	if (work->queued || (!page->tried_ever && page->ready)) {
		ADD(content, axis_web_storage_text("notice", "Axis is fetching the lists from TMDb for the first time. "
			"Open this page again in a minute."));
		return;
	}
	if (recommended) {
		g_autofree gchar *text = NULL;

		if (page->have == 0)
			text = g_strdup_printf("Recommended for you comes from the %s you have and want. Find one to add with "
				"the search box at the top of the page, and its recommendations appear here within the hour.", kind_plural(kind));
		else if (page->seeds < page->have)
			text = g_strdup_printf("From the recommendations for %u of your %u %s. Axis fetches those of %u more each "
				"hour.", page->seeds, page->have, kind_plural(kind), work->recommend);
		if (text != NULL)
			ADD(content, axis_web_storage_text("help", text));
		return;
	}
	if (page->problem != NULL) {
		g_autofree gchar *tried = axis_web_meta_when_text(page->tried_us);
		g_autofree gchar *why = axis_web_meta_why_text("TMDb", page->problem);
		g_autofree gchar *text = NULL;

		if (page->stored) {
			g_autofree gchar *when = axis_web_meta_when_text(page->fetched_us);

			text = g_strdup_printf("Axis could not fetch this list again on %s: %s The list shown is the one fetched "
				"on %s.", tried, why, when);
		} else {
			text = g_strdup_printf("Axis could not fetch this list on %s: %s It tries again within the hour.", tried,
				why);
		}
		ADD(content, axis_web_storage_text("notice notice-warning", text));
	}
}

static HtmxNode *
count_line(const ListWork *work)
{
	const AxisDiscoverPage *page = &work->page;
	g_autofree gchar *text = NULL;
	const gchar *film = work->want.kind == AXIS_META_KIND_FILM ? "movie" : "series";
	const gchar *films = work->want.kind == AXIS_META_KIND_FILM ? "movies" : "series";

	if (!page->stored)
		text = g_strdup("Axis has not fetched this list yet. It tries again within the hour.");
	else if (page->total > 0)
		text = g_strdup_printf("There is nothing on this page: the list holds %u %s that match. ", page->total,
			page->total == 1 ? film : films);
	else if (work->want.unreleased && work->want.list == AXIS_META_LIST_UPCOMING)
		text = g_strdup_printf("No %s is left: this list is of titles that are not out yet, and Hide unreleased hides "
			"them. Clear the filters to see the list.", film);
	else if (filtered(&work->want))
		text = g_strdup_printf("No %s matches these filters among the %u %s of this list that Axis keeps. Change the "
			"filters, or clear them.", film, page->size, page->size == 1 ? film : films);
	else
		text = g_strdup("This list is empty.");
	if (page->stored && page->total > 0) {
		HtmxP *empty = htmx_p_new();
		HtmxSpan *words = htmx_span_new();
		g_autofree gchar *first = list_url(&work->want, work->want.list, 1, TRUE);

		htmx_element_add_class(HTMX_ELEMENT(empty), "no-results");
		htmx_node_set_text_content(HTMX_NODE(words), text);
		ADD(empty, words);
		ADD(empty, axis_web_browser_link(first, "Go to the first page"));
		return HTMX_NODE(empty);
	}
	return axis_web_storage_text("no-results", text);
}

static HtmxResponse *
list_done(
	AxisWebDiscover *self,
	HtmxRequest     *request,
	gpointer         data
){
	ListWork *work = data;
	AxisMetaKind kind = work->want.kind;
	const AxisDiscoverPage *page = &work->page;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *heading_text = g_strdup_printf("Discover %s", kind == AXIS_META_KIND_FILM ? "movies" : "TV");
	HtmxHeading *heading = htmx_h1_new();
	AxisWebNotice notice;
	HtmxDiv *cards;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(content), "discover-page");
	htmx_node_set_text_content(HTMX_NODE(heading), heading_text);
	ADD(content, heading);
	ADD(content, axis_web_page_intro(kind == AXIS_META_KIND_FILM ?
		"Find a movie to add when you do not know its name: what is popular, trending, in cinemas and coming soon "
		"on TMDb, the best rated, and recommendations drawn from the movies you have. Axis keeps these lists and "
		"fetches them again by itself, so this page asks TMDb for nothing." :
		"Find a series to add when you do not know its name: what is popular, trending and on the air on TMDb, the "
		"best rated, and recommendations drawn from the series you have. Axis keeps these lists and fetches them "
		"again by itself, so this page asks TMDb for nothing."));
	axis_web_notice_begin(&notice, request, FALSE, work->added_text != NULL ? g_strdup(work->added_text) :
		done_at_card(work->want.done, axis_web_place_of(request), page->items), HTMX_NODE(content));
	if (work->n_lists == 0) {
		ADD(content, axis_web_storage_text("notice notice-warning", "Discover needs a details provider that offers "
			"lists, such as TMDb, and none here does."));
		axis_web_notice_end(&notice);
		return axis_web_page_respond(request, heading_text, HTMX_NODE(content));
	}
	ADD(content, list_tabs(work));
	ADD(content, axis_web_storage_text("help discover-list-help", list_help(kind, work->want.list)));
	ADD(content, filters_form(work));
	add_page_state(work, request, HTMX_NODE(content));

	/* Where this person's Adds go (administrators), above the cards. */
	{
		HtmxNode *bar = axis_web_actions_render_bar(work->actions, request, kind);

		if (bar != NULL) {
			axis_web_notice_put(&notice, bar, axis_web_actions_bar_place(kind));
			ADD(content, bar);
		}
	}

	/* The cards, in a block of their own: where a card that was hidden was. */
	cards = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(cards), CARDS_PLACE);
	axis_web_notice_put(&notice, HTMX_NODE(cards), CARDS_PLACE);
	if (page->items->len == 0) {
		ADD(cards, count_line(work));
		ADD(content, cards);
	} else {
		HtmxDiv *grid = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(grid), "title-grid");
		for (i = 0; i < page->items->len; i++) {
			/* Not interested takes the card away: it comes back to the next
			 * one (the one before for the last). */
			const AxisDiscoverItem *next = i + 1 < page->items->len ? g_ptr_array_index(page->items, i + 1) :
				i > 0 ? g_ptr_array_index(page->items, i - 1) : NULL;

			const AxisDiscoverItem *after = i + 1 < page->items->len ? g_ptr_array_index(page->items, i + 1) : NULL;

			ADD(grid, card_new(work->actions, work->want.hide, TRUE, request, g_ptr_array_index(page->items, i), next,
				after, work->posters[i], &notice));
		}
		ADD(cards, grid);
		ADD(content, cards);
		ADD(content, pager_new(&work->want, page->total));
	}

	if (page->stored) {
		g_autofree gchar *when = axis_web_meta_when_text(page->fetched_us);
		g_autofree gchar *period = period_text(work->refresh_after);
		g_autofree gchar *text = work->want.list == AXIS_META_LIST_RECOMMENDED ?
			g_strdup_printf("The latest recommendations were fetched from TMDb on %s. Each title's are fetched again "
				"after a week.", when) :
			g_strdup_printf("Fetched from TMDb on %s. Axis fetches each list again once it is %s old.", when, period);

		ADD(content, axis_web_storage_text("as-of", text));
	}
	if (axis_web_request_is_admin(request) && page->ready)
		ADD(content, refresh_form(request, &notice));
	if (page->provider != NULL)
		ADD(content, axis_web_meta_attribution(page->provider));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, heading_text, HTMX_NODE(content));
}

static HtmxResponse *
handle_list(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	gpointer      user_data
){
	ListWork *work = g_new0(ListWork, 1);

	work->web = axis_web_from_request(request);
	wanted_from(request, kind, &work->want);
	return dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

static HtmxResponse *
handle_movies(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return handle_list(request, AXIS_META_KIND_FILM, user_data);
}

static HtmxResponse *
handle_tv(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return handle_list(request, AXIS_META_KIND_SERIES, user_data);
}

/* ── Not interested ─────────────────────────────────────────────── */

typedef struct
{
	guint      page;
	gchar     *done;
	GPtrArray *items;	/* AxisDiscoverDismissed */
	guint      total;
	gchar    **posters;
	GError    *error;
} DismissedWork;

static void
dismissed_work_free(gpointer data)
{
	DismissedWork *work = data;
	guint i;

	if (work->posters != NULL) {
		for (i = 0; work->items != NULL && i < work->items->len; i++)
			g_free(work->posters[i]);
		g_free(work->posters);
	}
	g_clear_pointer(&work->items, g_ptr_array_unref);
	g_free(work->done);
	g_clear_error(&work->error);
	g_free(work);
}

static void
dismissed_run(
	AxisWebDiscover *self,
	gpointer         data
){
	DismissedWork *work = data;
	guint i;

	if (!axis_discover_list_dismissed(self->discover, (work->page - 1) * PAGE_SIZE, PAGE_SIZE, &work->items,
	        &work->total, &work->error))
		return;
	work->posters = g_new0(gchar *, MAX(work->items->len, 1));
	if (work->items->len > 0) {
		g_autofree const gchar **providers = g_new0(const gchar *, work->items->len);
		g_autofree const gchar **refs = g_new0(const gchar *, work->items->len);

		for (i = 0; i < work->items->len; i++) {
			const AxisDiscoverDismissed *item = g_ptr_array_index(work->items, i);

			providers[i] = item->title->provider;
			refs[i] = item->title->poster_ref;
		}
		axis_meta_artwork_find_many(axis_discover_get_meta(self->discover), providers, refs, work->items->len,
			AXIS_META_ART_POSTER_SMALL, TRUE, work->posters);
	}
}

static HtmxResponse *
dismissed_done(
	AxisWebDiscover *self,
	HtmxRequest     *request,
	gpointer         data
){
	DismissedWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	HtmxHeading *heading = htmx_h1_new();
	gboolean admin = axis_web_request_is_admin(request);
	AxisWebNotice notice;
	HtmxDiv *cards;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	content = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(content), "discover-page");
	htmx_node_set_text_content(HTMX_NODE(heading), "Hidden from Discover");
	ADD(content, heading);
	ADD(content, axis_web_page_intro("The movies and series hidden from Discover. Discover leaves them out of every "
		"list, for everyone, until they are shown again here."));
	axis_web_notice_begin(&notice, request, FALSE, work->done != NULL ? g_strdup(done_message(work->done)) : NULL,
		HTMX_NODE(content));
	cards = htmx_div_new();
	htmx_element_set_id(HTMX_ELEMENT(cards), CARDS_PLACE);
	axis_web_notice_put(&notice, HTMX_NODE(cards), CARDS_PLACE);
	if (work->items->len == 0) {
		HtmxP *empty = htmx_p_new();
		HtmxSpan *words = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(empty), "no-results");
		if (work->total == 0) {
			htmx_node_set_text_content(HTMX_NODE(words), "Nothing is hidden from Discover. To hide a title, select "
				"Hide from Discover under its card on ");
			ADD(empty, words);
			ADD(empty, axis_web_browser_link(MOVIES_PATH, "Discover movies"));
			{
				HtmxSpan *joiner = htmx_span_new();

				htmx_node_set_text_content(HTMX_NODE(joiner), " or ");
				ADD(empty, joiner);
			}
			ADD(empty, axis_web_browser_link(TV_PATH, "Discover TV"));
		} else {
			htmx_node_set_text_content(HTMX_NODE(words), "There is nothing on this page. ");
			ADD(empty, words);
			ADD(empty, axis_web_browser_link(NOT_INTERESTED_PATH, "Go to the first page"));
		}
		ADD(cards, empty);
		ADD(content, cards);
	} else {
		HtmxDiv *grid = htmx_div_new();
		guint first = (work->page - 1) * PAGE_SIZE + 1;
		guint last = first + work->items->len - 1;
		g_autofree gchar *count = g_strdup_printf("Showing %u to %u of %u titles.", first, last, work->total);
		HtmxDiv *foot = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(grid), "title-grid");
		for (i = 0; i < work->items->len; i++) {
			const AxisDiscoverDismissed *item = g_ptr_array_index(work->items, i);
			g_autofree gchar *when = axis_web_format_time(item->when_us, "some time ago");
			g_autofree gchar *line = item->by != NULL ? g_strdup_printf("Hidden by %s on %s", item->by, when) :
				g_strdup_printf("Hidden on %s", when);
			g_autofree gchar *place = title_place(item->title->id);
			HtmxDiv *extra = htmx_div_new();
			HtmxDiv *row = htmx_div_new();
			HtmxNode *card;

			htmx_element_add_class(HTMX_ELEMENT(extra), "discover-extra");
			axis_web_notice_put(&notice, HTMX_NODE(extra), place);
			ADD(extra, axis_web_storage_text("title-card-because", line));
			htmx_element_add_class(HTMX_ELEMENT(row), "discover-actions");
			if (admin) {
				g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, item->title->id);
				HtmxNode *form = axis_web_auth_post_form(request, "/discover/undismiss");
				HtmxNode *back = axis_web_back_field(request);

				htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
				ADD(form, axis_web_auth_hidden("title", id));
				/* Its card is gone once it is shown again: the block of the cards. */
				ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, CARDS_PLACE));
				if (back != NULL)
					ADD(form, back);
				ADD(form, axis_web_auth_button("Show in Discover again", "button-small"));
				ADD(row, form);
			}
			ADD(extra, row);
			card = axis_web_meta_card_new_full(item->title, work->posters[i], NULL, FALSE, NULL, NULL,
				HTMX_NODE(extra));
			htmx_element_set_id(HTMX_ELEMENT(card), place);
			ADD(grid, card);
		}
		ADD(cards, grid);
		ADD(content, cards);
		htmx_element_add_class(HTMX_ELEMENT(foot), "list-foot");
		ADD(foot, axis_web_storage_text("list-count", count));
		if (work->page > 1 || last < work->total) {
			HtmxNav *pager = htmx_nav_new();

			htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
			htmx_element_set_attribute(HTMX_ELEMENT(pager), "aria-label", "Pages");
			if (work->page > 1) {
				g_autofree gchar *href = g_strdup_printf(NOT_INTERESTED_PATH "?page=%u", work->page - 1);

				ADD(pager, axis_web_browser_link(href, "Previous page"));
			}
			if (last < work->total) {
				g_autofree gchar *href = g_strdup_printf(NOT_INTERESTED_PATH "?page=%u", work->page + 1);

				ADD(pager, axis_web_browser_link(href, "Next page"));
			}
			ADD(foot, pager);
		}
		ADD(content, foot);
	}
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Hidden from Discover", HTMX_NODE(content));
}

static HtmxResponse *
handle_dismissed(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DismissedWork *work = g_new0(DismissedWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");

	work->page = MAX(1, param_int(request, "page", 1, MAX_PAGE));
	if (done != NULL && done_message(done) != NULL)
		work->done = g_strdup(done);
	return dispatch(user_data, request, dismissed_run, dismissed_done, work, dismissed_work_free);
}

/* ── The administrators' actions ────────────────────────────────── */

typedef enum
{
	ACT_DISMISS,
	ACT_UNDISMISS,
	ACT_REFRESH
} Act;

typedef struct
{
	Act       act;
	gint64    title_id;
	gchar    *user;
	gchar    *back;
	gchar    *at;	/* the card the form was posted from, or NULL */
	gboolean  changed;
	GError   *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->user);
	g_free(work->back);
	g_free(work->at);
	g_clear_error(&work->error);
	g_free(work);
}

static void
act_run(
	AxisWebDiscover *self,
	gpointer         data
){
	ActWork *work = data;

	switch (work->act) {
	case ACT_DISMISS:
		work->changed = axis_discover_dismiss(self->discover, work->title_id, work->user, &work->error);
		break;
	case ACT_UNDISMISS:
		axis_discover_undismiss(self->discover, work->title_id, &work->changed, &work->error);
		break;
	default:
		axis_discover_request_refresh(self->discover, TRUE, NULL, &work->changed, &work->error);
		break;
	}
}

static HtmxResponse *
act_done(
	AxisWebDiscover *self,
	HtmxRequest     *request,
	gpointer         data
){
	ActWork *work = data;
	const gchar *key;

	if (g_error_matches(work->error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such title.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->act) {
	case ACT_DISMISS:
		axis_web_storage_audit(request, "title %" G_GINT64_FORMAT " marked not interested in Discover", work->title_id);
		key = "hidden";
		break;
	case ACT_UNDISMISS:
		axis_web_storage_audit(request, "title %" G_GINT64_FORMAT " shown in Discover again", work->title_id);
		key = "shown";
		break;
	default:
		key = work->changed ? "refreshing" : "already_refreshing";
		break;
	}
	/* The list is told which title went, so that its notice can name it. */
	if (work->act == ACT_DISMISS) {
		const gchar *back = work->back != NULL ? work->back : MOVIES_PATH;
		g_autofree gchar *named = g_strdup_printf("%s%c" AXIS_WEB_BACK_ADDED "=%" G_GINT64_FORMAT, back,
			strchr(back, '?') != NULL ? '&' : '?', work->title_id);

		return axis_web_place_redirect(request, named, key, work->at);
	}
	return axis_web_place_redirect(request, work->back != NULL ? work->back :
		work->act == ACT_UNDISMISS ? NOT_INTERESTED_PATH : MOVIES_PATH, key, work->at);
}

static HtmxResponse *
start_act(
	HtmxRequest *request,
	gpointer     user_data,
	Act          act
){
	ActWork *work = g_new0(ActWork, 1);
	guint64 id = 0;

	work->act = act;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->back = axis_web_back_posted(request);
	if (axis_web_place_id_valid(htmx_request_get_form_value(request, AXIS_WEB_PLACE_AT)))
		work->at = g_strdup(htmx_request_get_form_value(request, AXIS_WEB_PLACE_AT));
	if (act != ACT_REFRESH) {
		if (!axis_web_storage_form_u64(request, "title", &id) || id == 0 || id > (guint64)G_MAXINT64) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not changed", "Say which title.");
		}
		work->title_id = (gint64)id;
	}
	return dispatch(user_data, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, user_data, ACT_DISMISS);
}

static HtmxResponse *
handle_undismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, user_data, ACT_UNDISMISS);
}

static HtmxResponse *
handle_refresh(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, user_data, ACT_REFRESH);
}

/* ── More like this, on a title's own page ──────────────────────── */

/*
 * What the provider recommends for one title, as a card of its page
 * (add-more-like-this).  Discover is a part of the title page as the movies
 * and TV modules are: stored recommendations that are fresh are drawn with
 * the page; otherwise the card drawn loads LIKE_PATH by itself, and that
 * route asks the provider for the one title, stores the answer as the job
 * does and answers the finished card.
 */

/* The card, the place its link without script comes back to. */
#define LIKE_PLACE "more-like-this"

#define LIKE_PATH  "/discover/like/%" G_GINT64_FORMAT

/* The route's parameter that names the page its card is shown on, when that
 * is not the title's own page. */
#define LIKE_ON    "on"

typedef struct
{
	AxisWeb          *web;
	gint64            title_id;	/* the route's: the title asked about */
	AxisMetaTitle    *seed;		/* the route's: its stored record */
	gboolean          missing;
	AxisDiscoverLike  like;
	GPtrArray        *titles;	/* AxisMetaTitle *, borrowed from the items */
	gchar           **posters;	/* one per item, nullable each */
	guint             n_posters;
	AxisWebActionRun *actions;
	GError           *error;
} LikeWork;

/* Main loop (the actions' run is freed there). */
static void
like_work_free(gpointer data)
{
	LikeWork *work = data;
	guint i;

	if (work == NULL)
		return;
	for (i = 0; work->posters != NULL && i < work->n_posters; i++)
		g_free(work->posters[i]);
	g_free(work->posters);
	axis_web_actions_run_free(work->actions);
	g_clear_pointer(&work->titles, g_ptr_array_unref);
	axis_discover_like_clear(&work->like);
	if (work->seed != NULL)
		axis_meta_title_free(work->seed);
	g_clear_error(&work->error);
	g_free(work);
}

/* What is stored of @seed's recommendations, with the posters and the
 * modules' pieces of its cards.  @fetch: ask the provider first when what is
 * stored is not fresh (the route; never the page).  Worker thread. */
static void
like_read(
	AxisWebDiscover     *self,
	LikeWork            *work,
	const AxisMetaTitle *seed,
	gboolean             fetch
){
	if (!axis_discover_read_like(self->discover, seed, AXIS_DISCOVER_LIKE_LIMIT, &work->like, &work->error))
		return;
	if (work->like.provider == NULL)
		return;
	if (fetch && !work->like.fresh && work->like.ready) {
		if (!axis_discover_fetch_like(self->discover, seed, NULL, NULL, &work->error))
			return;
		axis_discover_like_clear(&work->like);
		if (!axis_discover_read_like(self->discover, seed, AXIS_DISCOVER_LIKE_LIMIT, &work->like, &work->error))
			return;
	}
	work->titles = g_ptr_array_new();
	work->n_posters = work->like.items->len;
	work->posters = item_posters(self, work->like.items, work->titles);
	work->actions = axis_web_actions_collect(axis_web_get_actions(work->web), work->titles, AXIS_WEB_ACTION_DISCOVER);
}

/* What an Add that came back to one of the cards says there, naming the
 * title; %NULL when the page was not sent back to one.  Transfer full. */
static gchar *
like_done_text(
	HtmxRequest     *request,
	const GPtrArray *items
){
	const gchar *done = htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE);
	const gchar *place = axis_web_place_of(request);
	guint i;

	for (i = 0; done != NULL && place != NULL && i < items->len; i++) {
		const AxisMetaTitle *title = ((const AxisDiscoverItem *)g_ptr_array_index(items, i))->title;
		g_autofree gchar *card = title_place(title->id);

		if (strcmp(card, place) == 0)
			return axis_web_meta_added_text(done, title);
	}
	return NULL;
}

/* The card, empty: its cards are Discover's, at a phone's width too. */
static HtmxNode *
like_frame(void)
{
	HtmxNode *card = axis_web_storage_card("More like this");

	htmx_element_set_id(HTMX_ELEMENT(card), LIKE_PLACE);
	htmx_element_add_class(HTMX_ELEMENT(card), "more-like-this");
	htmx_element_add_class(HTMX_ELEMENT(card), "discover-page");
	return card;
}

/* The card that loads @path by itself and is replaced by the answer; @text
 * says so.  Without script its link opens @plain, which comes back to the
 * card on the title's own page. */
static HtmxNode *
like_loading(
	const gchar *path,
	const gchar *plain,
	const gchar *text
){
	HtmxNode *card = like_frame();
	HtmxP *line = htmx_p_new();

	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-get", path);
	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-trigger", "load");
	htmx_element_set_attribute(HTMX_ELEMENT(card), "hx-swap", "outerHTML");
	ADD(card, axis_web_storage_text("help", text));
	htmx_element_add_class(HTMX_ELEMENT(line), "help");
	ADD(line, axis_web_browser_link(plain, "Look them up now"));
	ADD(card, line);
	return card;
}

/*
 * The card on a module's own page of a title (a series' episodes page),
 * which has no part of Discover's read with it: it always loads itself.  The
 * route draws what is stored, and asks the provider only when that is not
 * fresh; it is told the page (the way back of its cards' forms) and what was
 * done at which card (an Add that came back here).
 */
static HtmxNode *
like_more(
	HtmxRequest  *request,
	AxisMetaKind  kind,
	gint64        title_id,
	gpointer      user_data
){
	g_autofree gchar *plain = g_strdup_printf(LIKE_PATH, title_id);
	g_autofree gchar *here = axis_web_back_here(request);
	g_autofree gchar *on = here != NULL ? g_uri_escape_string(here, NULL, FALSE) : NULL;
	g_autofree gchar *text = g_strdup_printf("Axis is looking up what is like this %s. The titles appear here in a "
		"moment.", kind == AXIS_META_KIND_FILM ? "movie" : "series");
	const gchar *done = htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE);
	const gchar *place = axis_web_place_of(request);
	g_autoptr(GString) path = g_string_new(plain);

	if (on != NULL)
		g_string_append_printf(path, "?" LIKE_ON "=%s", on);
	if (on != NULL && done != NULL && place != NULL && g_str_has_prefix(place, "title-") &&
	    axis_web_place_id_valid(done))
		g_string_append_printf(path, "&" AXIS_WEB_PLACE_DONE "=%s&" AXIS_WEB_PLACE_AT "=%s", done, place);
	return like_loading(path->str, plain, text);
}

/*
 * The card of @seed's page, or %NULL when the provider cannot be asked about
 * @seed at all.  @lazy: recommendations that are not fresh are fetched by the
 * card itself (the page); without it what is stored is drawn whatever its age
 * (the route's answer, which must not load itself again).
 */
static HtmxNode *
like_card(
	LikeWork            *work,
	HtmxRequest         *request,
	const AxisMetaTitle *seed,
	gboolean             lazy
){
	const AxisDiscoverLike *like = &work->like;
	const gchar *noun = seed->kind == AXIS_META_KIND_FILM ? "movie" : "series";
	const gchar *label;
	AxisWebNotice notice;
	HtmxNode *card;
	guint i;

	if (like->provider == NULL || like->items == NULL)
		return NULL;
	label = like->provider->label;
	if (lazy && !like->fresh && like->ready) {
		g_autofree gchar *path = g_strdup_printf(LIKE_PATH, seed->id);
		g_autofree gchar *text = g_strdup_printf("Axis is asking %s what is like this %s. The titles appear here in "
			"a moment.", label, noun);

		return like_loading(path, path, text);
	}
	card = like_frame();

	axis_web_notice_begin(&notice, request, FALSE, like_done_text(request, like->items), card);
	if (like->problem != NULL) {
		g_autofree gchar *tried = axis_web_meta_when_text(like->tried_us);
		g_autofree gchar *why = axis_web_meta_why_text(label, like->problem);
		g_autofree gchar *text = NULL;

		if (like->stored) {
			g_autofree gchar *when = axis_web_meta_when_text(like->fetched_us);

			text = g_strdup_printf("Axis could not ask %s again on %s: %s The titles shown are those fetched on %s.",
				label, tried, why, when);
		} else {
			text = g_strdup_printf("Axis could not ask %s what is like this %s on %s: %s It asks again when this "
				"page is opened an hour or more after that.", label, noun, tried, why);
		}
		ADD(card, axis_web_storage_text("notice notice-warning", text));
	} else if (!like->stored && !like->ready) {
		g_autofree gchar *text = g_strdup_printf("Axis needs a %s token to ask what is like this %s.", label, noun);

		ADD(card, axis_web_storage_text("help", text));
		if (axis_web_request_is_admin(request))
			ADD(card, axis_web_browser_link("/admin/settings#module-metadata", "Add a TMDb token in Settings"));
		else
			ADD(card, axis_web_storage_text("help", "An administrator can add one in Settings."));
	} else if (!like->stored) {
		g_autofree gchar *text = g_strdup_printf("Axis has not asked %s what is like this %s yet. Open this page "
			"again in a moment.", label, noun);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (like->stored && like->items->len == 0) {
		g_autofree gchar *text = like->size > 0 ?
			g_strdup_printf("Every title %s recommends for this %s is hidden from Discover.", label, noun) :
			g_strdup_printf("%s recommends nothing for this %s yet.", label, noun);

		ADD(card, axis_web_storage_text("no-results", text));
	}
	if (like->items->len > 0) {
		HtmxDiv *grid = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(grid), "title-grid");
		for (i = 0; i < like->items->len; i++)
			ADD(grid, card_new(work->actions, FALSE, FALSE, request, g_ptr_array_index(like->items, i), NULL, NULL,
				work->posters[i], &notice));
		ADD(card, grid);
	}
	if (like->stored) {
		g_autofree gchar *when = axis_web_meta_when_text(like->fetched_us);
		g_autofree gchar *text = g_strdup_printf("What %s recommends for this %s, fetched on %s and kept a week.", label,
			noun, when);

		ADD(card, axis_web_storage_text("as-of", text));
	}
	axis_web_notice_end(&notice);
	return card;
}

/* Discover's part of a title's page: read with the page, on its worker
 * thread.  The provider is never asked here. */
static gpointer
like_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	AxisWebDiscover *self = user_data;
	LikeWork *work;

	if (place != AXIS_WEB_ACTION_PAGE || titles->len != 1 || self->web == NULL)
		return NULL;
	work = g_new0(LikeWork, 1);
	work->web = self->web;
	like_read(self, work, g_ptr_array_index(titles, 0), FALSE);
	if (work->error != NULL)
		g_message("discover: a title's recommendations could not be read: %s", work->error->message);
	return work;
}

static HtmxNode *
like_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             figures,
	gpointer             user_data
){
	LikeWork *work = figures;

	/* After the modules' own cards of the title, before its facts. */
	if (place != AXIS_WEB_ACTION_DETAIL || work == NULL || work->error != NULL)
		return NULL;
	return like_card(work, request, title, TRUE);
}

static void
like_run(
	AxisWebDiscover *self,
	gpointer         data
){
	LikeWork *work = data;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) titles = NULL;

	g_array_append_val(ids, work->title_id);
	if (!axis_meta_load_titles(axis_discover_get_meta(self->discover), ids, &titles, &work->error))
		return;
	if (titles->len == 0) {
		work->missing = TRUE;
		return;
	}
	work->seed = g_ptr_array_steal_index(titles, 0);
	like_read(self, work, work->seed, TRUE);
}

static HtmxResponse *
like_done(
	AxisWebDiscover *self,
	HtmxRequest     *request,
	gpointer         data
){
	LikeWork *work = data;
	g_autofree gchar *page = NULL;
	g_autofree gchar *markup = NULL;
	g_autoptr(HtmxNode) card = NULL;
	HtmxResponse *response;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->missing)
		return axis_web_error_respond(request, 404, "Not found", "There is no such title.");
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, work->title_id);
	/* Without script the card is on the title's page, now with its titles. */
	if (!htmx_request_is_htmx(request))
		return axis_web_place_redirect(request, page, NULL, LIKE_PLACE);
	/* The cards' forms come back to the page the card is on (the title's own
	 * unless the route was told another of this site), not to this route. */
	{
		const gchar *on = htmx_request_get_query_param(request, LIKE_ON);

		axis_web_back_set_here(request, on != NULL && strchr(on, '#') == NULL && axis_auth_next_is_safe(on) ? on :
			page);
	}
	card = like_card(work, request, work->seed, FALSE);
	/* (No card: nothing takes the place of the one that asked.) */
	markup = card != NULL ? htmx_element_render(HTMX_ELEMENT(card)) : g_strdup("");
	response = htmx_response_new();
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return response;
}

/* GET /discover/like/:id */
static HtmxResponse *
handle_like(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	guint64 id = 0;
	LikeWork *work;

	/* The store's id: digits only, the first not zero, at most eighteen. */
	if (text == NULL || text[0] == '0' || strlen(text) > 18 ||
	    !g_ascii_string_to_unsigned(text, 10, 1, (guint64)G_MAXINT64, &id, NULL))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	work = g_new0(LikeWork, 1);
	work->web = axis_web_from_request(request);
	work->title_id = (gint64)id;
	return dispatch(user_data, request, like_run, like_done, work, like_work_free);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
refresh_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebDiscover *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	gboolean all = payload != NULL && json_object_has_member(payload, AXIS_DISCOVER_PAYLOAD_ALL) &&
		json_object_get_boolean_member(payload, AXIS_DISCOVER_PAYLOAD_ALL);

	return axis_discover_run_refresh(self->discover, context, all, NULL, error);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_route_full(web, HTMX_METHOD_GET, MOVIES_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_movies, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, TV_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_tv, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, NOT_INTERESTED_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_dismissed,
		user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/discover/dismiss", AXIS_WEB_ROUTE_ADMIN, handle_dismiss, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/discover/undismiss", AXIS_WEB_ROUTE_ADMIN, handle_undismiss,
		user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/discover/refresh", AXIS_WEB_ROUTE_ADMIN, handle_refresh, user_data);
	/* More like this: Discover's card of a title's own page, and the route
	 * the card loads when the title's recommendations are to be fetched. */
	{
		AxisWebDiscover *self = user_data;

		self->web = web;
		axis_web_add_title_actions(web, like_collect, like_render, like_work_free, self);
		axis_web_set_title_more(web, like_more, self);
	}
	axis_web_route_full(web, HTMX_METHOD_GET, "/discover/like/:id", AXIS_WEB_ROUTE_DEFAULT, handle_like, user_data);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_DISCOVER_REFRESH_KIND, AXIS_META_QUEUE, 1, refresh_handler,
	  "Fetching the Discover lists", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_DISCOVER_REFRESH_SCHEDULE, AXIS_DISCOVER_REFRESH_KIND, NULL, AXIS_DISCOVER_REFRESH_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

/* A section of Media of its own, beside Movies and TV. */
static const AxisNavEntry nav_entries[] = {
	{ "Discover movies", MOVIES_PATH, 210, AXIS_NAV_GROUP_MEDIA, FALSE, "Discover", 0 },
	{ "Discover TV", TV_PATH, 220, AXIS_NAV_GROUP_MEDIA, FALSE, "Discover", 0 },
	{ "Not interested", NOT_INTERESTED_PATH, 230, AXIS_NAV_GROUP_MEDIA, FALSE, "Discover", 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { AXIS_META_MODULE_ID, NULL };

const AxisModuleInfo *
axis_web_discover_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_DISCOVER_MODULE_ID;
		info.name = "Discover";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Lists what is popular, trending, in cinemas, coming soon and top rated on TMDb, and "
			"recommendations drawn from the movies and series you have, so that you can find something to add "
			"without knowing its name. Keeps the lists and fetches them again with a job; the pages never ask TMDb.";
		info.settings = axis_discover_get_setting_table();
		info.depends_on = depends_on;
	}
	return &info;
}

AxisWebDiscover *
axis_web_discover_new(AxisDiscover *discover)
{
	AxisWebDiscover *self;

	g_return_val_if_fail(discover != NULL, NULL);

	self = g_new0(AxisWebDiscover, 1);
	self->discover = axis_discover_ref(discover);
	return self;
}

void
axis_web_discover_stop(AxisWebDiscover *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_discover_free(AxisWebDiscover *self)
{
	if (self == NULL)
		return;
	axis_web_discover_stop(self);
	axis_discover_unref(self->discover);
	g_free(self);
}
