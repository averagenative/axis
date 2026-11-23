/*
 * axis-web-meta.c - the metadata module and the Metadata page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-meta-private.h"

#include <string.h>

#include "jobs/axis-job-registry.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define MAX_PENDING AXIS_WEB_META_MAX_PENDING

#define ADD axis_web_storage_add

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebMeta        *owner;
	AxisWebMetaWorkFunc work;
	AxisWebMetaDoneFunc done;
	HtmxRequest    *request;
	HtmxResponse   *deferred;
	gpointer        data;
	GDestroyNotify  free_data;
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

gboolean
axis_web_meta_is_busy(AxisWebMeta *self)
{
	return self->stopping || self->pending >= MAX_PENDING;
}

HtmxResponse *
axis_web_meta_dispatch(
	AxisWebMeta         *self,
	HtmxRequest         *request,
	AxisWebMetaWorkFunc  work,
	AxisWebMetaDoneFunc  done,
	gpointer             data,
	GDestroyNotify       free_data
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

/* ── Pieces ─────────────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *node = level == 1 ? htmx_h1_new() : level == 2 ? htmx_h2_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(node), text);
	return HTMX_NODE(node);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *child)
{
	HtmxTd *cell = htmx_td_new();

	ADD(cell, child);
	return HTMX_NODE(cell);
}

static gchar *
format_time(gint64 us)
{
	return axis_web_format_time(us, "never");
}

static const gchar *
kinds_text(guint kinds)
{
	gboolean films = (kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_FILM)) != 0;
	gboolean series = (kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES)) != 0;

	return films && series ? "movies, series" : films ? "movies" : "series";
}

/* A labelled drop-down whose option is the current value. */
static HtmxNode *
select_field(
	const gchar        *id,
	const gchar        *label,
	const gchar        *name,
	const gchar *const *values,
	const gchar *const *labels,
	const gchar        *current
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);

		if (g_strcmp0(values[i], current) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	ADD(field, text);
	ADD(field, select);
	return HTMX_NODE(field);
}

/* ── The page's work ────────────────────────────────────────────── */

typedef struct
{
	gchar                 *done;
	/* The lookup asked for, if any. */
	gchar                 *provider;
	gchar                 *kind;
	gchar                 *query;
	gchar                 *year;
	gchar                 *problem;	/* the lookup could not be asked */
	gboolean               searched;
	AxisMetaSearchAnswer   answer;
	GPtrArray             *posters;	/* gchar * (nullable), one per result */
	/* The rest of the page. */
	GArray                *statuses;	/* AxisMetaProviderStatus */
	AxisMetaFigures        figures;
	AxisMetaFreshness      freshness;
	GError                *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;
	guint i;

	g_free(work->done);
	g_free(work->provider);
	g_free(work->kind);
	g_free(work->query);
	g_free(work->year);
	g_free(work->problem);
	axis_meta_search_answer_clear(&work->answer);
	g_clear_pointer(&work->posters, g_ptr_array_unref);
	for (i = 0; work->statuses != NULL && i < work->statuses->len; i++)
		axis_meta_provider_status_clear(&g_array_index(work->statuses, AxisMetaProviderStatus, i));
	g_clear_pointer(&work->statuses, g_array_unref);
	axis_meta_figures_clear(&work->figures);
	g_clear_error(&work->error);
	g_free(work);
}

static gboolean
parse_year(
	const gchar *text,
	gint        *year
){
	gint64 value;
	gchar *end = NULL;

	*year = 0;
	if (text == NULL || text[0] == '\0')
		return TRUE;
	if (strlen(text) != 4 || !g_ascii_isdigit(text[0]))
		return FALSE;
	value = g_ascii_strtoll(text, &end, 10);
	if (end == NULL || *end != '\0' || !axis_meta_year_valid(value))
		return FALSE;
	*year = (gint)value;
	return TRUE;
}

static void
page_run(
	AxisWebMeta *self,
	gpointer     data
){
	PageWork *work = data;
	guint i;

	work->statuses = g_array_new(FALSE, TRUE, sizeof(AxisMetaProviderStatus));
	axis_meta_get_freshness(self->meta, &work->freshness);
	if (work->query != NULL) {
		AxisMetaKind kind = AXIS_META_KIND_FILM;
		g_autoptr(GError) local = NULL;
		gint year = 0;

		work->searched = TRUE;
		if (!axis_meta_kind_from_string(work->kind, &kind) || !parse_year(work->year, &year)) {
			work->problem = g_strdup("Choose movie or series, and give the year as four digits (or leave it empty).");
		} else if (axis_meta_provider_find(self->meta, work->provider) != NULL &&
		           axis_meta_provider_find(self->meta, work->provider)->secondary) {
			work->problem = g_strdup_printf("%s is not searched by name: it adds its own episode numbering to a series "
				"TMDb knows. Use its key test under Providers.", axis_meta_provider_find(self->meta, work->provider)->label);
		} else if (!axis_meta_search(self->meta, work->provider, kind, work->query, year,
		               AXIS_META_DEFAULT_WAIT_MS, &work->answer, &local)) {
			if (!g_error_matches(local, AXIS_META_ERROR, AXIS_META_ERROR_INVALID)) {
				work->error = g_steal_pointer(&local);
				return;
			}
			work->problem = g_strdup(local->message);
		} else {
			/* Posters that are kept are shown; the others are fetched now,
			 * in the background, for the next time. */
			work->posters = g_ptr_array_new_with_free_func(g_free);
			for (i = 0; i < work->answer.titles->len; i++) {
				const AxisMetaTitle *title = g_ptr_array_index(work->answer.titles, i);

				g_ptr_array_add(work->posters, axis_meta_artwork_find(self->meta, title->provider,
					title->poster_ref, AXIS_META_ART_POSTER_SMALL, TRUE));
			}
		}
	}
	/* After the lookup, so that the providers' state includes its answer. */
	for (i = 0; i < axis_meta_n_providers(self->meta); i++) {
		AxisMetaProviderStatus status;

		axis_meta_provider_status(self->meta, i, &status);
		g_array_append_val(work->statuses, status);
	}
	axis_meta_load_figures(self->meta, &work->figures, &work->error);
}

/* ── Rendering ──────────────────────────────────────────────────── */

static HtmxNode *
state_badge(const AxisMetaProviderStatus *status)
{
	if (status->needs_key && !status->key_set)
		return axis_web_browser_badge("No key", "badge-note");
	if (status->key_refused)
		return axis_web_browser_badge("Key refused", "badge-failed");
	if (status->limit.paused_until_us != 0)
		return axis_web_browser_badge("Asked to wait", "badge-warn");
	if (status->limit.backoff_until_us != 0)
		return axis_web_browser_badge("Not answering", "badge-failed");
	if (status->limit.failures > 0)
		return axis_web_browser_badge("Failing", "badge-warn");
	return axis_web_browser_badge("Ready", "badge-succeeded");
}

/* "Test TheTVDB key", for a provider that offers a test and has a key, with
 * what its latest test said. */
static HtmxNode *
key_test(
	HtmxRequest                  *request,
	const AxisMetaProviderStatus *status
){
	g_autofree gchar *button = g_strdup_printf("Test %s key", status->info->label);
	HtmxDiv *block = htmx_div_new();
	HtmxNode *form;

	htmx_element_add_class(HTMX_ELEMENT(block), "key-test");
	if (status->test_us > 0 && status->test_text != NULL) {
		g_autofree gchar *when = format_time(status->test_us);
		g_autofree gchar *line = g_strdup_printf("Latest test of the %s key, %s: %s", status->info->label, when,
			status->test_text);

		ADD(block, axis_web_storage_text(status->test_ok ? "notice notice-success" : "notice notice-warning", line));
	}
	if (status->key_refused) {
		g_autofree gchar *line = g_strdup_printf("%s refused this key, so nothing is sent to it. Change the key or "
			"the PIN in Settings to try again.", status->info->label);

		ADD(block, axis_web_storage_text("notice notice-warning", line));
		return HTMX_NODE(block);
	}
	form = axis_web_auth_post_form(request, "/metadata/test-key");
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_hidden("provider", status->info->name));
	ADD(form, axis_web_auth_button(button, NULL));
	ADD(block, form);
	return HTMX_NODE(block);
}

static HtmxNode *
providers_card(
	HtmxRequest   *request,
	PageWork      *work,
	AxisWebNotice *notice
){
	static const AxisWebColumn columns[] = {
		{ "Provider", AXIS_WEB_COL_NAME },
		{ "Serves", AXIS_WEB_COL_TEXT },
		{ "Key", AXIS_WEB_COL_STATE },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Limit", AXIS_WEB_COL_RANGE },
		{ "Requests since Axis started", AXIS_WEB_COL_COUNT },
		{ "Last problem", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Providers");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "providers");
	axis_web_notice_put(notice, card, "providers");
	ADD(card, axis_web_storage_text(NULL, "Axis asks these services what movies and series exist, with your own keys, "
		"and keeps every answer it uses: pages and jobs read what is kept, so a provider that is down never stops a "
		"page. Only the search text or a title's identifier, the key and Axis's name and version are sent. Tokens are "
		"set in Settings, under Movie and TV details."));
	if (work->statuses->len == 0) {
		ADD(card, axis_web_storage_text("help", "This version of Axis has no details provider yet."));
		return card;
	}
	table = axis_web_table_new("meta-providers", columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->statuses->len; i++) {
		const AxisMetaProviderStatus *status = &g_array_index(work->statuses, AxisMetaProviderStatus, i);
		g_autofree gchar *limit = g_strdup_printf("%.2g requests a second", status->limit.rate);
		g_autofree gchar *requests = g_strdup_printf("%" G_GUINT64_FORMAT, status->limit.requests);
		g_autofree gchar *problem = NULL;
		HtmxTr *row = htmx_tr_new();

		if (status->limit.last_error != NULL) {
			g_autofree gchar *when = format_time(status->limit.last_error_us);

			problem = g_strdup_printf("%s (%s)", status->limit.last_error, when);
		}
		ADD(row, cell_text(status->info->label));
		ADD(row, cell_text(kinds_text(status->info->kinds)));
		if (status->needs_key && status->key_set && status->key_refused)
			ADD(row, cell_node(axis_web_browser_badge("Refused", "badge-failed")));
		else if (status->needs_key)
			ADD(row, cell_node(axis_web_browser_badge(status->key_set ? "Set" : "Not set",
				status->key_set ? "badge-succeeded" : "badge-note")));
		else
			ADD(row, cell_text("Not needed"));
		ADD(row, cell_node(state_badge(status)));
		ADD(row, cell_text(limit));
		ADD(row, cell_text(requests));
		ADD(row, cell_text(problem != NULL ? problem : ""));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	for (i = 0; i < work->statuses->len; i++) {
		const AxisMetaProviderStatus *status = &g_array_index(work->statuses, AxisMetaProviderStatus, i);

		if (status->info->test != NULL && status->key_set)
			ADD(card, key_test(request, status));
	}
	/* Each provider's notice; a secondary one's (TheTVDB) while its key is
	 * set, which is when its data can be shown. */
	for (i = 0; i < work->statuses->len; i++) {
		const AxisMetaProviderStatus *status = &g_array_index(work->statuses, AxisMetaProviderStatus, i);

		if (!status->info->secondary || status->key_set)
			ADD(card, axis_web_meta_attribution(status->info));
	}
	ADD(card, axis_web_browser_link("/admin/settings#module-metadata", "Keys and limits in Settings"));
	return card;
}

static gchar *
count_text(
	gint64       value,
	const gchar *one,
	const gchar *many
){
	return g_strdup_printf("%" G_GINT64_FORMAT " %s", value, value == 1 ? one : many);
}

static HtmxNode *
store_card(
	AxisWebMeta *self,
	PageWork    *work
){
	const AxisMetaFigures *f = &work->figures;
	HtmxNode *card = axis_web_storage_card("Details and artwork kept by Axis");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *titles = g_strdup_printf("%" G_GINT64_FORMAT " %s, %" G_GINT64_FORMAT " %s", f->films,
		f->films == 1 ? "movie" : "movies", f->series, f->series == 1 ? "series" : "series");
	g_autofree gchar *ids = count_text(f->ids, "identifier", "identifiers");
	g_autofree gchar *alts = count_text(f->alt_titles, "other name", "other names");
	g_autofree gchar *episodes = g_strdup_printf("%" G_GINT64_FORMAT " %s, %" G_GINT64_FORMAT " %s", f->seasons,
		f->seasons == 1 ? "season" : "seasons", f->episodes, f->episodes == 1 ? "episode" : "episodes");
	g_autofree gchar *searches = count_text(f->searches, "lookup", "lookups");
	g_autofree gchar *store = axis_web_browser_format_size((guint64)MAX(f->store_bytes, 0));
	g_autofree gchar *art_size = axis_web_browser_format_size((guint64)MAX(f->artwork_bytes, 0));
	g_autofree gchar *cap = axis_web_browser_format_size((guint64)MAX(f->artwork_cap, 0));
	g_autofree gchar *artwork = g_strdup_printf("%" G_GINT64_FORMAT " %s, %s of at most %s", f->artwork,
		f->artwork == 1 ? "image" : "images", art_size, cap);
	g_autofree gchar *refused = f->artwork_refused > 0 ? g_strdup_printf("%" G_GINT64_FORMAT " %s: TMDb had none, "
		"or the file was not a valid image", f->artwork_refused, f->artwork_refused == 1 ? "image" : "images") :
		count_text(f->artwork_refused, "image", "images");
	const gchar *dir = axis_meta_get_data_dir_path(self->meta);

	axis_web_browser_fact(facts, "Titles", titles);
	axis_web_browser_fact(facts, "Identifiers", ids);
	axis_web_browser_fact(facts, "Other names", alts);
	axis_web_browser_fact(facts, "Seasons and episodes", episodes);
	axis_web_browser_fact(facts, "Saved lookups", searches);
	axis_web_browser_fact(facts, "In the database", store);
	axis_web_browser_fact(facts, "Artwork", artwork);
	axis_web_browser_fact(facts, "Images not kept", refused);
	if (dir != NULL)
		axis_web_browser_fact_node(facts, "Data directory", HTMX_NODE(htmx_code_new_with_text(dir)));
	else
		axis_web_browser_fact(facts, "Data directory", "none: artwork is not kept");
	ADD(card, facts);
	return card;
}

static gchar *
days_text(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 86400, seconds == 86400 ? "day" : "days");
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", seconds / 3600, seconds == 3600 ? "hour" : "hours");
	return g_strdup_printf("%" G_GINT64_FORMAT " s", seconds);
}

static HtmxNode *
refresh_card(
	HtmxRequest   *request,
	PageWork      *work,
	AxisWebNotice *notice
){
	const AxisMetaFreshness *f = &work->freshness;
	HtmxNode *card = axis_web_storage_card("Keeping details current");
	HtmxNode *facts = axis_web_browser_facts_new();
	HtmxNode *form;
	g_autofree gchar *search = days_text(f->search);
	g_autofree gchar *unreleased = days_text(f->unreleased);
	g_autofree gchar *released = days_text(f->released);
	g_autofree gchar *ended = days_text(f->ended);
	g_autofree gchar *season = days_text(f->current_season);
	g_autofree gchar *negative = days_text(f->negative);
	g_autofree gchar *viewed = days_text(f->viewed);
	g_autofree gchar *intro = g_strdup_printf("Every 15 minutes, what has grown older than its limit is fetched "
		"again, oldest first, within each provider's limit, for the titles something refers to: those viewed in the "
		"last %s, and those other parts of Axis keep (a wanted list, the library). Nothing else is fetched until it "
		"is asked for.", viewed);

	htmx_element_set_id(HTMX_ELEMENT(card), "refresh");
	axis_web_notice_put(notice, card, "refresh");
	ADD(card, axis_web_storage_text(NULL, intro));
	axis_web_browser_fact(facts, "Searches", search);
	axis_web_browser_fact(facts, "Unreleased movies and continuing series", unreleased);
	axis_web_browser_fact(facts, "Released movies", released);
	axis_web_browser_fact(facts, "Ended series", ended);
	axis_web_browser_fact(facts, "Episodes of a current season", season);
	axis_web_browser_fact(facts, "Lookups that found nothing", negative);
	if (work->figures.refresh_state != NULL) {
		g_autofree gchar *when = format_time(work->figures.refresh_finished_us);
		g_autofree gchar *state = g_strdup(work->figures.refresh_state);
		g_autofree gchar *line = NULL;

		state[0] = g_ascii_toupper(state[0]);
		line = work->figures.refresh_finished_us > 0 ? g_strdup_printf("%s, %s", state, when) : g_strdup(state);
		axis_web_browser_fact(facts, "Latest update of details", line);
	} else {
		axis_web_browser_fact(facts, "Latest update of details", "none yet");
	}
	ADD(card, facts);
	form = axis_web_auth_post_form(request, "/metadata/refresh");
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_button("Update out-of-date details now", NULL));
	ADD(card, form);
	return card;
}

static HtmxNode *
lookup_form(
	AxisWebMeta *self,
	PageWork    *work
){
	static const gchar *const kind_values[] = { "film", "series", NULL };
	static const gchar *const kind_labels[] = { "Movie", "TV series", NULL };
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	g_autoptr(GPtrArray) labels = g_ptr_array_new();
	HtmxForm *form = htmx_form_new();
	HtmxDiv *buttons = htmx_div_new();
	HtmxButton *button = htmx_button_new_submit();
	HtmxNode *query;
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/metadata");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "role", "search");
	htmx_element_add_class(HTMX_ELEMENT(form), "filters");
	for (i = 0; i < axis_meta_n_providers(self->meta); i++) {
		const AxisMetaProviderInfo *info = axis_meta_provider_at(self->meta, i);

		/* A secondary provider (TheTVDB) is not searched by name: it adds to
		 * a series TMDb knows (its key is checked by its own test). */
		if (info->secondary)
			continue;
		g_ptr_array_add(names, (gpointer)info->name);
		g_ptr_array_add(labels, (gpointer)info->label);
	}
	g_ptr_array_add(names, NULL);
	g_ptr_array_add(labels, NULL);
	ADD(form, select_field("meta-provider", "Provider", "provider", (const gchar *const *)names->pdata,
		(const gchar *const *)labels->pdata, work->provider));
	ADD(form, select_field("meta-kind", "Movie or series", "kind", kind_values, kind_labels,
		work->kind != NULL ? work->kind : "film"));
	query = axis_web_storage_text_field("meta-q", "Name", "q", work->query, "The Mazework", NULL);
	htmx_element_add_class(HTMX_ELEMENT(query), "field-wide");
	ADD(form, query);
	ADD(form, axis_web_storage_text_field("meta-year", "Year", "year", work->year, "1999", NULL));
	htmx_node_set_text_content(HTMX_NODE(button), "Look up");
	ADD(buttons, button);
	ADD(form, buttons);
	return HTMX_NODE(form);
}

static HtmxNode *
lookup_card(
	AxisWebMeta *self,
	PageWork    *work
){
	HtmxNode *card = axis_web_storage_card("Check a key with a lookup");
	const AxisMetaSearchAnswer *answer = &work->answer;
	guint i;

	ADD(card, axis_web_storage_text(NULL, "Ask a provider what it knows, to check that its key works. The results "
		"are kept like any other answer. To find and add a movie or series, use the search box at the top of the page."));
	if (axis_meta_n_providers(self->meta) == 0)
		return card;
	ADD(card, lookup_form(self, work));
	if (!work->searched)
		return card;
	if (work->problem != NULL) {
		ADD(card, axis_web_auth_notice(TRUE, work->problem));
		return card;
	}
	if (answer->saved) {
		g_autofree gchar *when = format_time(answer->fetched_us);
		g_autofree gchar *fallback = answer->provider != NULL ?
			g_strdup_printf("%s did not answer", answer->provider->label) :
			g_strdup("The details provider did not answer");
		g_autofree gchar *text = answer->fetched_us > 0 ?
			g_strdup_printf("%s. Showing the saved results from %s.", answer->problem != NULL ? answer->problem :
				fallback, when) :
			g_strdup_printf("%s. Nothing was saved for this lookup.", answer->problem != NULL ? answer->problem :
				fallback);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
	} else if (answer->stale) {
		g_autofree gchar *when = format_time(answer->fetched_us);
		g_autofree gchar *text = g_strdup_printf("These results are from %s, older than their freshness limit.", when);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
	} else {
		g_autofree gchar *when = format_time(answer->fetched_us);
		g_autofree gchar *text = g_strdup_printf("Answered %s.", when);

		ADD(card, axis_web_storage_text("as-of", text));
	}
	if (answer->titles != NULL && answer->titles->len > 0) {
		HtmxDiv *list = htmx_div_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "title-grid");
		for (i = 0; i < answer->titles->len; i++)
			ADD(list, axis_web_meta_card_new(g_ptr_array_index(answer->titles, i),
				work->posters != NULL && i < work->posters->len ? g_ptr_array_index(work->posters, i) : NULL,
				NULL, TRUE));
		ADD(card, list);
	} else if (answer->fetched_us > 0) {
		ADD(card, axis_web_storage_text("help", "Nothing was found."));
	}
	if (answer->provider != NULL)
		ADD(card, axis_web_meta_attribution(answer->provider));
	return card;
}

static const gchar *
done_message(
	const gchar *key,
	gboolean    *error
){
	static const struct
	{
		const gchar *key;
		gboolean     error;
		const gchar *text;
	} messages[] = {
		{ "refresh-queued", FALSE, "An update of out-of-date details is queued. Its progress is on the Jobs page." },
		{ "refresh-exists", FALSE, "An update of details is queued or running already." },
		{ "key-tested", FALSE, "The key was tested. What came back is under Providers." },
		{ "key-not-tested", TRUE, "That provider has no test of its key, or it has no key." },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0) {
			*error = messages[i].error;
			return messages[i].text;
		}
	}
	return NULL;
}

static HtmxResponse *
page_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	PageWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *message;
	AxisWebNotice notice;
	gboolean is_error = FALSE;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, heading(1, "Movie and TV details"));
	ADD(content, axis_web_page_intro("Where Axis learns which movies and series exist, with their details and "
		"artwork: a details provider (TMDb) and its key. Searching, adding a title and identifying what is on disk "
		"all depend on it."));
	message = done_message(work->done, &is_error);
	axis_web_notice_begin(&notice, request, is_error, g_strdup(message), HTMX_NODE(content));
	ADD(content, providers_card(request, work, &notice));
	ADD(content, lookup_card(self, work));
	ADD(content, store_card(self, work));
	ADD(content, refresh_card(request, work, &notice));
	axis_web_notice_end(&notice);
	return axis_web_page_respond(request, "Movie and TV details", HTMX_NODE(content));
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	PageWork *work = g_new0(PageWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;

	/* Only a known key is kept: nothing from the address is shown. */
	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	work->query = axis_web_meta_param(request, "q", 400);
	work->provider = axis_web_meta_param(request, "provider", 16);
	work->kind = axis_web_meta_param(request, "kind", 8);
	work->year = axis_web_meta_param(request, "year", 8);
	if (work->provider == NULL && axis_meta_n_providers(self->meta) > 0)
		work->provider = g_strdup(axis_meta_provider_at(self->meta, 0)->name);
	return axis_web_meta_dispatch(self, request, page_run, page_done, work, page_work_free);
}

/* ── Refresh now ────────────────────────────────────────────────── */

typedef struct
{
	gint64    job_id;
	gboolean  created;
	GError   *error;
} RefreshWork;

static void
refresh_work_free(gpointer data)
{
	RefreshWork *work = data;

	g_clear_error(&work->error);
	g_free(work);
}

static void
refresh_run(
	AxisWebMeta *self,
	gpointer     data
){
	RefreshWork *work = data;

	axis_meta_request_refresh(self->meta, &work->job_id, &work->created, &work->error);
}

static HtmxResponse *
refresh_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	RefreshWork *work = data;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	axis_web_storage_audit(request, "metadata refresh requested: job=%" G_GINT64_FORMAT, work->job_id);
	return axis_web_place_redirect(request, "/metadata", work->created ? "refresh-queued" : "refresh-exists",
		"refresh");
}

static HtmxResponse *
handle_refresh(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return axis_web_meta_dispatch(user_data, request, refresh_run, refresh_done, g_new0(RefreshWork, 1),
		refresh_work_free);
}

/* ── Test a key ─────────────────────────────────────────────────── */

typedef struct
{
	gchar    *provider;
	gboolean  tested;
	gboolean  ok;
} TestWork;

static void
test_work_free(gpointer data)
{
	TestWork *work = data;

	g_free(work->provider);
	g_free(work);
}

static void
test_run(
	AxisWebMeta *self,
	gpointer     data
){
	TestWork *work = data;
	const AxisMetaProviderInfo *info = axis_meta_provider_find(self->meta, work->provider);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;

	/* Only a provider that offers a test, and only while it has its key. */
	if (info == NULL || info->test == NULL || !axis_meta_provider_offered(self->meta, info))
		return;
	work->tested = TRUE;
	work->ok = axis_meta_test_key(self->meta, work->provider, NULL, &summary, &error);
}

static HtmxResponse *
test_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	TestWork *work = data;

	if (!work->tested)
		return axis_web_place_redirect(request, "/metadata", "key-not-tested", "providers");
	/* The outcome, never the key: the provider's name and whether it worked. */
	axis_web_storage_audit(request, "metadata key tested: provider=%s answered=%s", work->provider,
		work->ok ? "yes" : "no");
	return axis_web_place_redirect(request, "/metadata", "key-tested", "providers");
}

static HtmxResponse *
handle_test_key(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	const gchar *provider = htmx_request_get_form_value(request, "provider");
	TestWork *work = g_new0(TestWork, 1);

	if (provider != NULL && axis_meta_provider_name_valid(provider))
		work->provider = g_strdup(provider);
	return axis_web_meta_dispatch(self, request, test_run, test_done, work, test_work_free);
}

/* ── Artwork ────────────────────────────────────────────────────── */

typedef struct
{
	gchar  *hash;
	GBytes *data;
	gchar  *content_type;
	GError *error;
} ArtWork;

static void
art_work_free(gpointer data)
{
	ArtWork *work = data;

	g_free(work->hash);
	if (work->data != NULL)
		g_bytes_unref(work->data);
	g_free(work->content_type);
	g_clear_error(&work->error);
	g_free(work);
}

static void
art_run(
	AxisWebMeta *self,
	gpointer     data
){
	ArtWork *work = data;

	axis_meta_artwork_read(self->meta, work->hash, &work->data, &work->content_type, &work->error);
}

static HtmxResponse *
art_done(
	AxisWebMeta *self,
	HtmxRequest *request,
	gpointer     data
){
	ArtWork *work = data;
	HtmxResponse *response;
	g_autofree gchar *etag = NULL;

	if (g_error_matches(work->error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	response = htmx_response_new();
	etag = g_strdup_printf("\"%s\"", work->hash);
	htmx_response_set_bytes(response, work->data);
	htmx_response_set_content_type(response, work->content_type);
	htmx_response_add_header(response, "ETag", etag);
	/* The name is the content: it never changes.  Private: it needs a
	 * session, so no shared cache keeps it. */
	htmx_response_add_header(response, "Cache-Control", "private, max-age=31536000, immutable");
	axis_web_keep_cache_policy(response);
	return response;
}

static HtmxResponse *
handle_art(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *hash = params != NULL ? g_hash_table_lookup(params, "hash") : NULL;
	ArtWork *work;

	/* The hash is the whole of the name: anything else is not looked at. */
	if (!axis_meta_artwork_hash_valid(hash))
		return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
	work = g_new0(ArtWork, 1);
	work->hash = g_strdup(hash);
	return axis_web_meta_dispatch(user_data, request, art_run, art_done, work, art_work_free);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	AxisWebMeta *self = user_data;

	axis_meta_set_enabled(self->meta, TRUE);
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	AxisWebMeta *self = user_data;

	axis_meta_set_enabled(self->meta, FALSE);
}

static gboolean
refresh_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebMeta *self = user_data;

	return axis_meta_run_refresh(self->meta, context, NULL, error);
}

static gboolean
cleanup_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebMeta *self = user_data;

	return axis_meta_run_cleanup(self->meta, context, NULL, error);
}

/*
 * Every page's footer: the notice of each provider whose data the pages may
 * show (add-metadata-tvdb, design section 7).  A provider Axis relies on
 * (TMDb) always; a secondary one (TheTVDB) while its key is set, or while a
 * managed series follows its numbering, key or none.  From memory only.
 */
static HtmxNode *
footer_notices(
	HtmxRequest *request,
	gpointer     user_data
){
	AxisWebMeta *self = user_data;
	HtmxDiv *notices = NULL;
	guint i;

	for (i = 0; i < axis_meta_n_providers(self->meta); i++) {
		const AxisMetaProviderInfo *info = axis_meta_provider_at(self->meta, i);

		if (info == NULL || info->attribution == NULL)
			continue;
		if (info->secondary && !axis_meta_provider_offered(self->meta, info) &&
		    !axis_meta_provider_followed(self->meta, info))
			continue;
		if (notices == NULL) {
			notices = htmx_div_new();
			htmx_element_add_class(HTMX_ELEMENT(notices), "footer-notices");
		}
		ADD(notices, axis_web_meta_attribution(info));
	}
	return notices != NULL ? HTMX_NODE(notices) : NULL;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * A details provider that refused the key Axis has (add-attention-notices):
 * nothing is sent to it until the key changes, so only a person ends it and
 * it is a notice at once.  The refusal is known in memory only; no statement
 * is run.  "newest" is when this daemon first saw it, so a refusal dismissed
 * before a restart is shown once more after it.  A key that is missing is
 * not a notice here: the dashboard's "Still needed" card says that.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebMeta *self = user_data;
	guint i;

	if (self->refused_since == NULL)
		self->refused_since = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (i = 0; i < axis_meta_n_providers(self->meta); i++) {
		AxisMetaProviderStatus status;
		g_autofree gchar *text = NULL;
		AxisAttentionItem *item;
		gint64 *since;

		axis_meta_provider_status(self->meta, i, &status);
		if (status.info == NULL || !(status.needs_key && status.key_set && status.key_refused)) {
			if (status.info != NULL)
				g_hash_table_remove(self->refused_since, status.info->name);
			axis_meta_provider_status_clear(&status);
			continue;
		}
		since = g_hash_table_lookup(self->refused_since, status.info->name);
		if (since == NULL) {
			since = g_new(gint64, 1);
			*since = reading->now_us / G_USEC_PER_SEC;
			g_hash_table_insert(self->refused_since, g_strdup(status.info->name), since);
		}
		text = g_strdup_printf(status.info->secondary ?
			"%s refused the key Axis has: its episode numbering cannot be read until the key is changed." :
			"%s refused the key Axis has: titles cannot be looked up until the key is changed.", status.info->label);
		item = axis_attention_item_new("meta.refused", status.info->name, AXIS_ATTENTION_BROKEN, 1, *since);
		axis_attention_item_say(item, text, NULL, NULL);
		axis_attention_item_link(item, "/metadata", "Open Movie and TV details");
		item->since_us = *since * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
		axis_meta_provider_status_clear(&status);
	}
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_set_footer_notices(web, footer_notices, user_data);
	axis_web_add_attention_source(web, "the details providers", attention_source, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, "/metadata", AXIS_WEB_ROUTE_ADMIN, handle_page, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/metadata/refresh", AXIS_WEB_ROUTE_ADMIN, handle_refresh, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/metadata/test-key", AXIS_WEB_ROUTE_ADMIN, handle_test_key,
		user_data);
	/* Every page may show artwork, so any logged-in user may fetch it. */
	axis_web_route_full(web, HTMX_METHOD_GET, "/art/:hash", AXIS_WEB_ROUTE_DEFAULT, handle_art, user_data);
	/* Finding titles is for everyone who is logged in. */
	axis_web_search_register(user_data, web);
	axis_web_title_register(user_data, web);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_META_REFRESH_KIND, AXIS_META_QUEUE, 1, refresh_handler,
	  "Keeping the titles' details current", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_META_CLEANUP_KIND, AXIS_META_QUEUE, 3, cleanup_handler,
	  "Removing artwork nothing uses", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_META_REFRESH_SCHEDULE, AXIS_META_REFRESH_KIND, NULL, AXIS_META_REFRESH_INTERVAL_S },
	{ AXIS_META_CLEANUP_SCHEDULE, AXIS_META_CLEANUP_KIND, NULL, AXIS_META_CLEANUP_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Movie and TV details", "/metadata", 40, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_meta_get_module(AxisWebMeta *self)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_META_MODULE_ID;
		info.name = "Movie and TV details";
		info.start = module_start;
		info.stop = module_stop;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Looks up movies and series at the details providers, with your own token; keeps every "
			"answer and the artwork, serves the artwork itself, and keeps what is referred to current within each "
			"provider's limits. Offers the title search and the title pages to everyone who is logged in. Sends "
			"only the search text or a title's identifier, never anything about you or your library.";
	}
	/* The providers' keys and rates are settings of the module. */
	info.settings = axis_meta_get_setting_table(self->meta);
	return &info;
}

AxisWebMeta *
axis_web_meta_new(AxisMeta *meta)
{
	AxisWebMeta *self;

	g_return_val_if_fail(meta != NULL, NULL);

	self = g_new0(AxisWebMeta, 1);
	self->meta = axis_meta_ref(meta);
	return self;
}

void
axis_web_meta_stop(AxisWebMeta *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_meta_free(AxisWebMeta *self)
{
	if (self == NULL)
		return;
	axis_web_meta_stop(self);
	axis_web_search_free(self);
	g_clear_pointer(&self->refused_since, g_hash_table_unref);
	axis_meta_unref(self->meta);
	g_free(self);
}
