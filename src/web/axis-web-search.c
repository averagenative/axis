/*
 * axis-web-search.c - the title search: the box in the top bar, the results
 * page, the posters that arrive after the text
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /search?q=&kind=&page=   films and series that match, as cards
 *   GET /search/poster?...       one poster, for the card that is waiting
 *                                for it (htmx only; see below)
 *
 * The box is a plain GET form, so the search works without script and a
 * result can be bookmarked; htmx only makes it quicker: after a pause of
 * 300 ms in typing the results replace the main content, and the address
 * bar follows.  The tabs and "show more" are links that replace only the
 * results region.  Beside the box a select narrows the search to films or
 * series (kind=): part of the form, sent with what is typed, searching
 * again when it changes, and following the tab chosen on the results.
 *
 * Posters arrive after the text.  A result renders from the store at once;
 * a poster that is not kept yet was queued to be fetched (by the metadata
 * layer's own fetch threads, within the provider's limit), and its card
 * holds a placeholder with the title set in it.  When the card is on
 * screen htmx asks /search/poster: if the image has been kept it answers
 * the image, if not the placeholder again, to ask once more in two
 * seconds, up to five times (a slow image host delays no list and no card
 * but its own).  The first ask also queues the fetch, for the pages whose
 * cards were drawn without one (the Identify pages' candidates), when a
 * stored title shows the image; the fetch queued with the list is the same
 * question, asked once.  An image the provider refused ends the asking: the
 * placeholder stays, with no further request.  Without htmx the
 * placeholder stays.
 *
 * Who may search: anyone logged in, a limited number of times (below).
 * What was typed goes to the provider and nowhere else; it is not logged.
 */

#include "web/axis-web-meta-private.h"

#include <string.h>

#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_meta_add

/* Per user: a burst of this many searches, then one every two seconds.
 * Searches the store answers cost the provider nothing, but a person
 * typing is one request each time they pause, and a script is the same. */
#define SEARCH_BURST        12.0
#define SEARCH_PER_SECOND   0.5

/* The table of users is pruned beyond this many entries... */
#define SEARCHERS_PRUNE_AT  1000

/* ...of those that have been idle this long (their bucket is full again). */
#define SEARCHER_IDLE_US    (10 * G_USEC_PER_SEC * 60)

/* What is shown of each kind at first, and how many more each "more" adds. */
#define PAGE_SIZE           20
#define MAX_PAGES           3

/* The least a search is worth sending to a provider. */
#define MIN_QUERY_CHARS     2

/* The longest query string taken at all; beyond it the search is refused. */
#define MAX_QUERY_BYTES     400

#define RESULTS_ID          "search-results"

/* ── The rate limit ─────────────────────────────────────────────── */

typedef struct
{
	gdouble tokens;
	gint64  stamp_us;
} Bucket;

static gboolean
prune_idle(
	gpointer key,
	gpointer value,
	gpointer data
){
	const Bucket *bucket = value;
	gint64 now = *(gint64 *)data;

	return now - bucket->stamp_us > SEARCHER_IDLE_US;
}

/*
 * Takes one search from @user's bucket.  Memory only: a restart forgets it,
 * which a person cannot use to search faster and a script gains nothing by.
 * Main loop only.
 */
static gboolean
take_search(
	AxisWebMeta *self,
	gint64       user,
	guint       *retry_seconds
){
	gint64 now = g_get_monotonic_time();
	Bucket *bucket;

	if (self->searchers == NULL)
		self->searchers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	if (g_hash_table_size(self->searchers) > SEARCHERS_PRUNE_AT)
		g_hash_table_foreach_remove(self->searchers, prune_idle, &now);
	bucket = g_hash_table_lookup(self->searchers, &user);
	if (bucket == NULL) {
		gint64 *key = g_new(gint64, 1);

		*key = user;
		bucket = g_new0(Bucket, 1);
		bucket->tokens = SEARCH_BURST;
		bucket->stamp_us = now;
		g_hash_table_insert(self->searchers, key, bucket);
	} else {
		bucket->tokens = MIN(SEARCH_BURST, bucket->tokens +
			(gdouble)(now - bucket->stamp_us) / G_USEC_PER_SEC * SEARCH_PER_SECOND);
		bucket->stamp_us = now;
	}
	if (bucket->tokens < 1.0) {
		*retry_seconds = (guint)((1.0 - bucket->tokens) / SEARCH_PER_SECOND) + 1;
		return FALSE;
	}
	bucket->tokens -= 1.0;
	return TRUE;
}

void
axis_web_search_free(AxisWebMeta *self)
{
	g_clear_pointer(&self->searchers, g_hash_table_unref);
}

/* ── The box ────────────────────────────────────────────────────── */

/* The kinds a search can be narrowed to, as the box's select and the
 * results' tabs name them (kind=). */
static const gchar *const kind_values[] = { "all", "film", "series", NULL };
static const gchar *const kind_labels[] = { "All", "Movies", "TV", NULL };

#define KIND_ID "search-kind"

/* @current as one of kind_values, "all" for anything else. */
static const gchar *
kind_value_of(const gchar *current)
{
	guint i;

	for (i = 0; kind_values[i] != NULL; i++) {
		if (g_strcmp0(current, kind_values[i]) == 0)
			return kind_values[i];
	}
	return kind_values[0];
}

/*
 * The kind searched for, beside the box: part of the plain form, so a search
 * sent without script keeps it.  With htmx, a change searches again at once
 * for what is in the box (nothing, when the box is empty: see
 * handle_search()), and typing sends it with the box.
 */
static HtmxNode *
kind_select_new(const gchar *current)
{
	HtmxSelect *select = htmx_select_new_with_name("kind");
	const gchar *chosen = kind_value_of(current);
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(select), KIND_ID);
	htmx_element_add_class(HTMX_ELEMENT(select), "search-kind");
	for (i = 0; kind_values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(kind_values[i], kind_labels[i]);

		if (strcmp(chosen, kind_values[i]) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-get", "/search");
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-include", "#search-q");
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-trigger", "change");
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-target", "#content");
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-push-url", "true");
	htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-sync", "#search-q:replace");
	return HTMX_NODE(select);
}

static HtmxNode *
search_box(
	HtmxRequest *request,
	gpointer     user_data
){
	HtmxForm *form = htmx_form_new();
	HtmxLabel *label = htmx_label_new_for("search-q");
	HtmxLabel *kind_label = htmx_label_new_for(KIND_ID);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_SEARCH);
	const gchar *path = htmx_request_get_path(request);
	gboolean on_search = path != NULL && strcmp(path, "/search") == 0;
	g_autofree gchar *typed = NULL;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/search");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "role", "search");
	htmx_element_add_class(HTMX_ELEMENT(form), "search-box");

	/* What to search, then the box: one control of two parts. */
	htmx_element_add_class(HTMX_ELEMENT(kind_label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(kind_label), "Search in");
	ADD(form, kind_label);
	/* The kind asked for, on the search page itself. */
	ADD(form, kind_select_new(on_search ? htmx_request_get_query_param(request, "kind") : NULL));

	htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
	htmx_node_set_text_content(HTMX_NODE(label), "Search movies and series");
	ADD(form, label);

	htmx_element_set_id(HTMX_ELEMENT(input), "search-q");
	htmx_input_set_name(input, "q");
	/* Short, for the room the row leaves it; the label above says the rest. */
	htmx_input_set_placeholder(input, "Search titles");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "100");
	/* What is typed is the query, on the search page itself. */
	if (on_search) {
		typed = axis_web_meta_param(request, "q", MAX_QUERY_BYTES);
		if (typed != NULL)
			htmx_input_set_value(input, typed);
	}
	/* The enhancement: after a pause in typing the results replace the main
	 * content, and the address bar follows; the kind goes with it.  The plain
	 * form above works without any of it. */
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-get", "/search");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-include", "#" KIND_ID);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-trigger", "input changed delay:300ms, search");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-target", "#content");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-push-url", "true");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "hx-sync", "this:replace");
	ADD(form, input);
	return HTMX_NODE(form);
}

/* ── What a search asks and finds ───────────────────────────────── */

typedef enum
{
	TAB_ALL,
	TAB_FILMS,
	TAB_SERIES
} Tab;

#define N_KINDS 2

typedef struct
{
	AxisMetaKind                kind;
	const AxisMetaProviderInfo *provider;	/* who was asked; %NULL: nobody can be */
	AxisMetaSearchAnswer        answer;
	gchar                      *problem;	/* the question could not be asked */
	guint                       shown;	/* how many of the answer are shown */
	GPtrArray                  *posters;	/* gchar * (nullable), one per shown title */
} Found;

typedef struct
{
	AxisWeb          *web;		/* the shell: borrowed, it outlives requests */
	gchar            *query;	/* cleaned; %NULL when there is nothing to ask */
	gboolean          too_long;
	gboolean          too_short;
	gboolean          region_only;	/* only the results region is wanted */
	Tab               tab;
	guint             page;
	Found             found[N_KINDS];
	gboolean          any_provider;
	GPtrArray        *shown;	/* AxisMetaTitle *, borrowed from the answers */
	AxisWebActionRun *actions;
	GError           *error;
} SearchWork;

static void
search_work_free(gpointer data)
{
	SearchWork *work = data;
	guint i;

	for (i = 0; i < N_KINDS; i++) {
		axis_meta_search_answer_clear(&work->found[i].answer);
		g_free(work->found[i].problem);
		g_clear_pointer(&work->found[i].posters, g_ptr_array_unref);
	}
	g_clear_pointer(&work->shown, g_ptr_array_unref);
	axis_web_actions_run_free(work->actions);
	g_free(work->query);
	g_clear_error(&work->error);
	g_free(work);
}

/* The first provider that serves @kind and is able to answer (has its key). */
static const AxisMetaProviderInfo *
ready_provider(
	AxisMeta     *meta,
	AxisMetaKind  kind
){
	const AxisMetaProviderInfo *found = NULL;
	guint i;

	for (i = 0; found == NULL && i < axis_meta_n_providers(meta); i++) {
		AxisMetaProviderStatus status;

		axis_meta_provider_status(meta, i, &status);
		if ((status.info->kinds & AXIS_META_KIND_BIT(kind)) != 0 && !status.info->secondary &&
		    (!status.needs_key || status.key_set))
			found = status.info;
		axis_meta_provider_status_clear(&status);
	}
	return found;
}

static guint
shown_of(
	const SearchWork *work,
	AxisMetaKind      kind,
	guint             total
){
	guint limit = 0;

	if (work->tab == TAB_ALL)
		limit = PAGE_SIZE;
	else if ((work->tab == TAB_FILMS) == (kind == AXIS_META_KIND_FILM))
		limit = PAGE_SIZE * work->page;
	return MIN(limit, total);
}

static void
search_run(
	AxisWebMeta *self,
	gpointer     data
){
	SearchWork *work = data;
	guint k;
	guint i;

	if (work->query == NULL)
		return;
	for (k = 0; k < N_KINDS; k++) {
		Found *found = &work->found[k];
		g_autoptr(GError) local = NULL;

		found->kind = k == 0 ? AXIS_META_KIND_FILM : AXIS_META_KIND_SERIES;
		found->provider = ready_provider(self->meta, found->kind);
		if (found->provider == NULL)
			continue;
		work->any_provider = TRUE;
		if (!axis_meta_search(self->meta, found->provider->name, found->kind, work->query, 0,
		        AXIS_META_DEFAULT_WAIT_MS, &found->answer, &local)) {
			if (!g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_INVALID)) {
				work->error = g_steal_pointer(&local);
				return;
			}
			found->problem = g_strdup(local->message);
			memset(&found->answer, 0, sizeof(found->answer));
		}
	}

	/* Posters: those that are kept are shown, the others are fetched now,
	 * in the background, and arrive on their own. */
	work->shown = g_ptr_array_new();
	for (k = 0; k < N_KINDS; k++) {
		Found *found = &work->found[k];

		found->posters = g_ptr_array_new_with_free_func(g_free);
		found->shown = found->answer.titles != NULL ? shown_of(work, found->kind, found->answer.titles->len) : 0;
		for (i = 0; i < found->shown; i++) {
			const AxisMetaTitle *title = g_ptr_array_index(found->answer.titles, i);

			g_ptr_array_add(found->posters, axis_meta_artwork_find(self->meta, title->provider,
				title->poster_ref, AXIS_META_ART_POSTER_SMALL, TRUE));
			g_ptr_array_add(work->shown, (gpointer)title);
		}
	}
	work->actions = axis_web_actions_collect(axis_web_get_actions(work->web), work->shown, AXIS_WEB_ACTION_CARD);
}

/* ── Rendering ──────────────────────────────────────────────────── */

static const gchar *
kind_word(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? "movies" : "series";
}

static const gchar *
tab_value(Tab tab)
{
	return tab == TAB_FILMS ? "film" : tab == TAB_SERIES ? "series" : "all";
}

static gchar *
search_url(
	const SearchWork *work,
	Tab               tab,
	guint             page
){
	g_autofree gchar *q = g_uri_escape_string(work->query, NULL, FALSE);

	if (page > 1)
		return g_strdup_printf("/search?q=%s&kind=%s&page=%u", q, tab_value(tab), page);
	return g_strdup_printf("/search?q=%s&kind=%s", q, tab_value(tab));
}

/* A link that, with htmx, replaces only the results region. */
static HtmxA *
region_link_new(const gchar *href)
{
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_element_set_attribute(HTMX_ELEMENT(link), "hx-get", href);
	htmx_element_set_attribute(HTMX_ELEMENT(link), "hx-target", "#" RESULTS_ID);
	htmx_element_set_attribute(HTMX_ELEMENT(link), "hx-swap", "outerHTML");
	htmx_element_set_attribute(HTMX_ELEMENT(link), "hx-push-url", "true");
	return link;
}

static HtmxNode *
region_link(
	const gchar *text,
	const gchar *href
){
	HtmxA *link = region_link_new(href);

	htmx_node_set_text_content(HTMX_NODE(link), text);
	return HTMX_NODE(link);
}

/* A label and, set apart, a count: "Films 23". */
static void
add_label_and_count(
	gpointer     parent,
	const gchar *label,
	guint        count
){
	HtmxSpan *name = htmx_span_new();
	HtmxSpan *figure = htmx_span_new();
	g_autofree gchar *text = g_strdup_printf("%u", count);

	htmx_node_set_text_content(HTMX_NODE(name), label);
	htmx_element_add_class(HTMX_ELEMENT(figure), "tab-count");
	htmx_node_set_text_content(HTMX_NODE(figure), text);
	ADD(parent, name);
	ADD(parent, figure);
}

static HtmxNode *
notice(
	const gchar *css,
	const gchar *text
){
	return axis_web_storage_text(css, text);
}

/* No provider is able to answer: say what is needed, and where, for those who
 * may do it. */
static HtmxNode *
no_provider_notice(HtmxRequest *request)
{
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "notice");
	htmx_element_add_class(HTMX_ELEMENT(box), "notice-warning");
	ADD(box, axis_web_storage_text(NULL, "There is nothing to search yet: Axis has no TMDb token. TMDb, the "
		"details provider, tells Axis which movies and series exist. Its token is free from the TMDb website."));
	if (axis_web_request_is_admin(request))
		ADD(box, axis_web_browser_link("/admin/settings#module-metadata", "Add a TMDb token in Settings"));
	else
		ADD(box, axis_web_storage_text("help", "An administrator can add one in Settings."));
	return HTMX_NODE(box);
}

/*
 * What was shown instead of an answer, for every kind whose provider did not
 * answer in time: why (in the provider's words), and when the results shown
 * were saved.  Kinds that failed the same way are one notice.
 */
static void
saved_notices(
	const SearchWork *work,
	HtmxNode         *region
){
	gboolean done[N_KINDS] = { FALSE, FALSE };
	guint i;
	guint j;

	for (i = 0; i < N_KINDS; i++) {
		const Found *found = &work->found[i];
		g_autoptr(GString) text = NULL;
		g_autofree gchar *when_first = NULL;
		g_autofree gchar *why = NULL;

		if (done[i] || found->provider == NULL || !found->answer.saved)
			continue;
		why = axis_web_meta_why_text(found->provider->label, found->answer.problem);
		text = g_string_new(why);
		for (j = i; j < N_KINDS; j++) {
			const Found *other = &work->found[j];
			g_autofree gchar *when = NULL;
			gboolean same_time;

			if (done[j] || other->provider != found->provider || !other->answer.saved ||
			    g_strcmp0(other->answer.problem, found->answer.problem) != 0)
				continue;
			done[j] = TRUE;
			when = other->answer.fetched_us > 0 ? axis_web_meta_when_text(other->answer.fetched_us) : NULL;
			same_time = j > i && when != NULL && g_strcmp0(when, when_first) == 0;
			if (j == i)
				when_first = g_strdup(when);
			if (same_time) {
				/* The earlier sentence said when; say that this kind is in it. */
				g_string_append_printf(text, " The %s are from that time too.", kind_word(other->kind));
			} else if (when != NULL) {
				g_string_append_printf(text, " The %s shown are the results saved on %s.", kind_word(other->kind),
					when);
			} else {
				g_string_append_printf(text, " Nothing was saved for this search, so no %s are shown.",
					kind_word(other->kind));
			}
		}
		ADD(region, notice("notice notice-warning", text->str));
	}
}

static HtmxNode *
tabs_new(const SearchWork *work)
{
	HtmxNav *nav = htmx_nav_new();
	HtmxUl *list = htmx_ul_new();
	guint films = work->found[0].answer.titles != NULL ? work->found[0].answer.titles->len : 0;
	guint series = work->found[1].answer.titles != NULL ? work->found[1].answer.titles->len : 0;
	const struct
	{
		Tab          tab;
		const gchar *label;
		guint        count;
	} tabs[] = {
		{ TAB_ALL, "All", films + series },
		{ TAB_FILMS, "Movies", films },
		{ TAB_SERIES, "TV", series }
	};
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(nav), "kind-tabs");
	htmx_element_set_attribute(HTMX_ELEMENT(nav), "aria-label", "Movies or TV");
	for (i = 0; i < G_N_ELEMENTS(tabs); i++) {
		HtmxLi *item = htmx_li_new();
		g_autofree gchar *href = search_url(work, tabs[i].tab, 1);
		HtmxA *link = region_link_new(href);

		add_label_and_count(link, tabs[i].label, tabs[i].count);
		if (tabs[i].tab == work->tab) {
			htmx_element_add_class(HTMX_ELEMENT(link), "current");
			htmx_element_set_attribute(HTMX_ELEMENT(link), "aria-current", "page");
		}
		ADD(item, link);
		ADD(list, item);
	}
	ADD(nav, list);
	return HTMX_NODE(nav);
}

/* What an Add that came back to a result's card says there, naming it
 * (add-activity-feedback); %NULL for none. */
static gchar *
done_text(
	const SearchWork *work,
	HtmxRequest      *request
){
	const gchar *done = htmx_request_get_query_param(request, "done");
	const gchar *place = axis_web_place_of(request);
	guint i;
	guint k;

	if (g_strcmp0(done, "add_defaults") == 0)
		return g_strdup("Saved: your Adds from a list go there now. A title's own Options still choose for it "
			"alone.");
	if (g_strcmp0(done, "added") != 0 && g_strcmp0(done, "managed") != 0)
		return NULL;
	for (k = 0; place != NULL && k < N_KINDS; k++) {
		const Found *found = &work->found[k];

		for (i = 0; found->answer.titles != NULL && i < found->answer.titles->len; i++) {
			const AxisMetaTitle *title = g_ptr_array_index(found->answer.titles, i);
			g_autofree gchar *card = g_strdup_printf("title-%" G_GINT64_FORMAT, title->id);

			if (title->id > 0 && strcmp(card, place) == 0)
				return axis_web_meta_added_text(done, title);
		}
	}
	return g_strdup(g_strcmp0(done, "managed") == 0 ? "Axis manages it now: its files on disk count as in the "
		"library." : "Added. Its card shows where it stands now; its page has the details.");
}

static HtmxNode *
group_new(
	const SearchWork *work,
	HtmxRequest      *request,
	const Found      *found,
	AxisWebNotice    *placed
){
	HtmxSection *group = htmx_section_new();
	HtmxDiv *grid = htmx_div_new();
	guint total = found->answer.titles != NULL ? found->answer.titles->len : 0;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(group), "title-group");
	if (work->tab == TAB_ALL) {
		HtmxHeading *heading = htmx_h2_new();

		add_label_and_count(heading, found->kind == AXIS_META_KIND_FILM ? "Movies" : "TV", total);
		ADD(group, heading);
	}
	htmx_element_add_class(HTMX_ELEMENT(grid), "title-grid");
	for (i = 0; i < found->shown; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(found->answer.titles, i);
		const gchar *poster = i < found->posters->len ? g_ptr_array_index(found->posters, i) : NULL;
		/* Its page and its Add come back to this card, with the notice in it. */
		g_autofree gchar *href = title->id > 0 ? axis_web_back_title_href(request, title->id) : NULL;
		g_autofree gchar *place = g_strdup_printf("title-%" G_GINT64_FORMAT, title->id);
		HtmxNode *extra = NULL;
		HtmxNode *card;

		if (title->id > 0 && placed != NULL && axis_web_notice_is_at(placed, place)) {
			extra = HTMX_NODE(htmx_div_new());
			axis_web_notice_put(placed, extra, place);
		}
		card = axis_web_meta_card_new_full(title, poster, axis_web_actions_render(work->actions, request,
			AXIS_WEB_ACTION_CARD, title), FALSE, href, NULL, extra);
		if (title->id > 0)
			htmx_element_set_id(HTMX_ELEMENT(card), place);
		ADD(grid, card);
	}
	ADD(group, grid);
	if (found->shown < total) {
		g_autofree gchar *text = NULL;
		g_autofree gchar *href = NULL;
		HtmxP *more = htmx_p_new();

		if (work->tab == TAB_ALL) {
			text = g_strdup_printf("Show all %u %s", total, kind_word(found->kind));
			href = search_url(work, found->kind == AXIS_META_KIND_FILM ? TAB_FILMS : TAB_SERIES, 1);
		} else {
			text = g_strdup_printf("Show more %s (%u of %u shown)", kind_word(found->kind), found->shown, total);
			href = search_url(work, work->tab, MIN(work->page + 1, MAX_PAGES));
		}
		htmx_element_add_class(HTMX_ELEMENT(more), "more");
		ADD(more, region_link(text, href));
		ADD(group, more);
	}
	return HTMX_NODE(group);
}

static HtmxNode *
results_new(
	SearchWork    *work,
	HtmxRequest   *request,
	AxisWebNotice *placed
){
	HtmxDiv *region = htmx_div_new();
	gboolean any_saved = FALSE;
	guint total = 0;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(region), RESULTS_ID);
	htmx_element_add_class(HTMX_ELEMENT(region), "search-results");
	/* The region is what changes while someone types: tell readers of the
	 * page that it did. */
	htmx_element_set_attribute(HTMX_ELEMENT(region), "aria-live", "polite");

	if (work->too_long) {
		ADD(region, notice("notice notice-warning", "That search is too long. Names of movies and series are shorter "
			"than 100 characters."));
		return HTMX_NODE(region);
	}
	if (work->too_short) {
		ADD(region, axis_web_storage_text("help", "Keep typing: a search needs at least two characters."));
		return HTMX_NODE(region);
	}
	if (work->query == NULL) {
		ADD(region, axis_web_storage_text("help", "Type the name of a movie or a series in the box at the top of "
			"the page."));
		return HTMX_NODE(region);
	}
	if (!work->any_provider) {
		ADD(region, no_provider_notice(request));
		return HTMX_NODE(region);
	}

	for (i = 0; i < N_KINDS; i++) {
		const Found *found = &work->found[i];

		if (found->provider != NULL && found->problem != NULL)
			ADD(region, notice("notice notice-warning", found->problem));
		if (found->provider != NULL && found->answer.saved)
			any_saved = TRUE;
		total += found->answer.titles != NULL ? found->answer.titles->len : 0;
	}
	saved_notices(work, HTMX_NODE(region));
	ADD(region, tabs_new(work));
	/* Where this person's Adds go, for each kind shown (administrators). */
	for (i = 0; i < N_KINDS; i++) {
		HtmxNode *bar = work->found[i].shown > 0 ? axis_web_actions_render_bar(work->actions, request,
			work->found[i].kind) : NULL;

		if (bar == NULL)
			continue;
		/* (Each kind's bar its own notice: the movies' bar never shows the
		 * series' one.) */
		if (placed != NULL)
			axis_web_notice_put(placed, bar, axis_web_actions_bar_place(work->found[i].kind));
		ADD(region, bar);
	}
	/* Nothing shown because nothing could be asked is not "nothing matches". */
	if (total == 0 && !any_saved) {
		g_autofree gchar *text = g_strdup_printf("Nothing matches \"%s\". Check the spelling, or try a shorter "
			"name.", work->query);

		ADD(region, axis_web_storage_text("no-results", text));
	}
	for (i = 0; i < N_KINDS; i++) {
		const Found *found = &work->found[i];

		if (found->shown > 0)
			ADD(region, group_new(work, request, found, placed));
	}
	/* Whoever supplied what is shown is named, as its terms require. */
	for (i = 0; i < N_KINDS; i++) {
		const Found *found = &work->found[i];

		if (found->provider == NULL || found->shown == 0)
			continue;
		/* Once per provider, not once per kind. */
		if (i == 1 && work->found[0].shown > 0 && work->found[0].provider == found->provider)
			continue;
		ADD(region, axis_web_meta_attribution(found->provider));
	}
	return HTMX_NODE(region);
}

static HtmxResponse *
search_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	SearchWork *work = data;
	g_autoptr(HtmxNode) region = NULL;
	g_autoptr(HtmxDiv) content = NULL;
	g_autofree gchar *page_title = work->query != NULL ? g_strdup_printf("Search for \"%s\"", work->query) :
		g_strdup("Search movies and TV");
	HtmxResponse *response;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->region_only) {
		/* The region alone, as the page's content: htmx puts it in place of
		 * the old one.  A tab chose another kind: the box's select follows,
		 * so what is typed next keeps it. */
		g_autoptr(HtmxFragment) both = htmx_fragment_new();
		HtmxNode *select = kind_select_new(tab_value(work->tab));

		region = results_new(work, request, NULL);
		htmx_element_set_attribute(HTMX_ELEMENT(select), "hx-swap-oob", "true");
		htmx_node_add_child(HTMX_NODE(both), region);
		ADD(both, select);
		response = axis_web_page_respond(request, page_title, HTMX_NODE(both));
	} else {
		HtmxHeading *title = htmx_h1_new();
		AxisWebNotice placed;

		content = htmx_div_new();
		htmx_element_add_class(HTMX_ELEMENT(content), "search-page");
		htmx_node_set_text_content(HTMX_NODE(title), "Search movies and TV");
		ADD(content, title);
		/* An Add came back to a result's card: its notice there. */
		axis_web_notice_begin(&placed, request, FALSE, done_text(work, request), HTMX_NODE(content));
		region = results_new(work, request, &placed);
		htmx_node_add_child(HTMX_NODE(content), region);
		axis_web_notice_end(&placed);
		response = axis_web_page_respond(request, page_title, HTMX_NODE(content));
	}
	/* The same address answers a whole page, the content, or the region: the
	 * page's Vary names what tells them apart (HX-Target among them). */
	return response;
}

static HtmxResponse *
handle_search(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *raw = htmx_request_get_query_param(request, "q");
	const gchar *kind = htmx_request_get_query_param(request, "kind");
	const gchar *page = htmx_request_get_query_param(request, "page");
	HtmxRequestHeaders *headers = htmx_request_get_headers(request);
	SearchWork *work = g_new0(SearchWork, 1);
	guint retry = 0;

	work->web = axis_web_from_request(request);
	work->tab = TAB_ALL;
	if (g_strcmp0(kind, "film") == 0)
		work->tab = TAB_FILMS;
	else if (g_strcmp0(kind, "series") == 0)
		work->tab = TAB_SERIES;
	work->page = 1;
	if (page != NULL && page[0] >= '1' && page[0] < '1' + MAX_PAGES && page[1] == '\0')
		work->page = (guint)(page[0] - '0');
	work->region_only = headers != NULL && htmx_request_is_htmx(request) &&
		g_strcmp0(htmx_request_headers_get_target_id(headers), RESULTS_ID) == 0;

	if (raw != NULL && (strlen(raw) > MAX_QUERY_BYTES || !g_utf8_validate(raw, -1, NULL))) {
		work->too_long = TRUE;
	} else {
		work->query = axis_meta_query_clean(raw);
		if (work->query != NULL && g_utf8_strlen(work->query, -1) < MIN_QUERY_CHARS)
			work->too_short = TRUE;
	}
	/* Said on the page, with nothing asked. */
	if (work->too_short)
		g_clear_pointer(&work->query, g_free);
	/* The kind changed with nothing in the box: nothing to search, and the
	 * page stays as it is (htmx swaps nothing for a 204). */
	if (work->query == NULL && !work->too_long && !work->too_short && headers != NULL &&
	    htmx_request_is_htmx(request) && g_strcmp0(htmx_request_headers_get_trigger_id(headers), KIND_ID) == 0) {
		HtmxResponse *none = htmx_response_new();

		search_work_free(work);
		htmx_response_set_status(none, 204);
		return none;
	}

	/* Only a search that would be asked counts against the limit. */
	if (work->query != NULL && state != NULL && !take_search(self, state->user_id, &retry)) {
		HtmxResponse *refused;
		g_autofree gchar *after = g_strdup_printf("%u", retry);

		search_work_free(work);
		refused = axis_web_error_respond(request, 429, "Too many searches",
			"You are searching faster than Axis allows. Wait a few seconds and try again.");
		htmx_response_add_header(refused, "Retry-After", after);
		return refused;
	}
	return axis_web_meta_dispatch(self, request, search_run, search_done, work, search_work_free);
}

/* ── The posters that come later ────────────────────────────────── */

typedef struct
{
	gchar    *provider;
	gchar    *ref;
	gchar    *name;
	gboolean  large;
	guint     attempt;
	gchar    *hash;
	gboolean  refused;	/* the provider does not have it: ask no more */
} PosterWork;

static void
poster_work_free(gpointer data)
{
	PosterWork *work = data;

	g_free(work->provider);
	g_free(work->ref);
	g_free(work->name);
	g_free(work->hash);
	g_free(work);
}

static void
poster_run(
	AxisWebMeta *self,
	gpointer     data
){
	PosterWork *work = data;

	/* The first ask fetches what is not kept yet (one fetch, whoever else
	 * asked for it); the retries only look. */
	work->hash = axis_meta_artwork_find_shown(self->meta, work->provider, work->ref,
		work->large ? AXIS_META_ART_POSTER : AXIS_META_ART_POSTER_SMALL, work->attempt == 0, &work->refused);
}

static HtmxResponse *
poster_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	PosterWork *work = data;
	AxisMetaTitle title;
	g_autoptr(HtmxNode) poster = NULL;
	g_autofree gchar *markup = NULL;
	HtmxResponse *response = htmx_response_new();
	guint next;

	/* Only the three fields the poster's place reads: nothing here is owned. */
	memset(&title, 0, sizeof(title));
	title.provider = work->provider;
	title.poster_ref = work->ref;
	title.title = work->name;
	/* Refused: past the last try, a placeholder that asks nothing more. */
	next = work->hash != NULL ? 0 : work->refused ? AXIS_WEB_META_POSTER_RETRIES + 1 : work->attempt + 1;
	poster = axis_web_meta_poster_new(&title, work->hash, work->large, next);
	markup = htmx_element_render(HTMX_ELEMENT(poster));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	return response;
}

static HtmxResponse *
handle_poster(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	PosterWork *work;
	g_autofree gchar *size = axis_web_meta_param(request, "size", 8);
	g_autofree gchar *tries = axis_web_meta_param(request, "try", 2);
	g_autofree gchar *provider = axis_web_meta_param(request, "provider", 16);
	g_autofree gchar *ref = axis_web_meta_param(request, "ref", 200);
	g_autofree gchar *name = axis_web_meta_param(request, "t", 200);
	HtmxResponse *none;

	/* An error page would be swapped into the card: nothing is the answer
	 * to whatever is not as it should be (the card keeps its placeholder). */
	if (provider != NULL && ref != NULL && name != NULL && size != NULL && tries != NULL &&
	    axis_meta_provider_name_valid(provider) && axis_meta_provider_find(self->meta, provider) != NULL &&
	    axis_meta_image_ref_valid(ref) && (strcmp(size, "card") == 0 || strcmp(size, "page") == 0) &&
	    g_ascii_isdigit(tries[0]) && (tries[1] == '\0' || g_ascii_isdigit(tries[1])) &&
	    !axis_web_meta_is_busy(self)) {
		work = g_new0(PosterWork, 1);
		work->provider = g_steal_pointer(&provider);
		work->ref = g_steal_pointer(&ref);
		work->name = g_steal_pointer(&name);
		work->large = strcmp(size, "page") == 0;
		work->attempt = (guint)g_ascii_strtoull(tries, NULL, 10);
		return axis_web_meta_dispatch(self, request, poster_run, poster_done, work, poster_work_free);
	}
	none = htmx_response_new();
	htmx_response_set_status(none, 204);
	return none;
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_web_search_register(
	AxisWebMeta *self,
	AxisWeb     *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/search", AXIS_WEB_ROUTE_DEFAULT, handle_search, self);
	axis_web_route_full(web, HTMX_METHOD_GET, AXIS_WEB_META_POSTER_PATH, AXIS_WEB_ROUTE_DEFAULT, handle_poster, self);
	axis_web_set_search_box(web, search_box, self);
}
