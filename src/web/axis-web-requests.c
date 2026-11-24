/*
 * axis-web-requests.c - the requests' part of the web shell: the Requests
 * page, a person's own requests, Request on a title's card and page, the
 * administrator's decisions, and the notices
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET  /requests                    an administrator: every request, the
 *                                     waiting ones first; anyone else: "My
 *                                     requests" and what is left of their quota
 *   POST /requests                    ask for a title (title=, kind=, seasons=,
 *                                     season-N=yes); any signed-in user
 *   GET  /requests/:id                one request: the person who asked, a
 *                                     follower or an administrator; 404 to
 *                                     anyone else, as for one never issued
 *   POST /requests/:id/withdraw       the person who asked, while it waits
 *   POST /requests/:id/unfollow       a follower
 *   POST /requests/:id/approve        an administrator (root=, profile=)
 *   POST /requests/:id/decline        an administrator (reason=, close=yes)
 *   POST /requests/:id/retry          an administrator: place the title again
 *   POST /requests/titles/:id/close   an administrator: close a title
 *   POST /requests/titles/:id/open    an administrator: open it again
 *   POST /requests/defaults           an administrator: the placement defaults
 *   GET  /requests/people             an administrator: the people's switches
 *   POST /requests/people/:id         and limits
 *
 * A request is named by its public identifier (22 random characters), never
 * by its row.  Every route that changes something is behind the shell's CSRF
 * check; the administrator's are AXIS_WEB_ROUTE_ADMIN besides.  Who asks is
 * the session's account, and nothing posted names a person.  A Request post
 * carries a title, a kind and seasons: any other field refuses it whole, so
 * no post can choose a folder, a profile, what is monitored, an episode or a
 * note.
 *
 * What a person who is not an administrator is sent: titles, seasons, states
 * in fixed words, names of accounts (as requests.show_who_asked allows) and
 * an administrator's reason for a decline.  Never a file, a path, a release,
 * an indexer, a download client, a quality profile or a library folder.
 */

#include "requests/axis-requests-private.h"

#include <string.h>

#include "web/axis-web-attention.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_auth_add

/* How many requests a page lists. */
#define PAGE_ROWS 100

/* ── Work off the main loop ─────────────────────────────────────── */

typedef void (*WorkFunc)(AxisRequests *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisRequests *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisRequests   *owner;
	WorkFunc        work;
	DoneFunc        done;
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
	if (response != NULL)
		g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	axis_requests_unref(task->owner);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisRequests   *self,
	HtmxRequest    *request,
	WorkFunc        work,
	DoneFunc        done,
	gpointer        data,
	GDestroyNotify  free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_REQUESTS_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = axis_requests_ref(self);
	task->work = work;
	task->done = done;
	task->data = data;
	task->free_data = free_data;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
text(
	const gchar *css,
	const gchar *words
){
	return axis_web_storage_text(css, words);
}

static HtmxNode *
cell(const gchar *words)
{
	return HTMX_NODE(htmx_td_new_with_text(words != NULL ? words : ""));
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no such request.");
}

/* The status and the page of a refusal.  The message is the layer's own
 * words: it holds nothing that was posted. */
static HtmxResponse *
refusal(
	HtmxRequest  *request,
	const gchar  *heading,
	const GError *error
){
	g_autofree gchar *message = NULL;
	guint status;

	if (error == NULL || error->domain != AXIS_REQUESTS_ERROR)
		return axis_web_internal_error(request, error);
	switch (error->code) {
	case AXIS_REQUESTS_ERROR_NOT_FOUND:
		return not_found(request);
	case AXIS_REQUESTS_ERROR_INVALID:
	case AXIS_REQUESTS_ERROR_PLACEMENT:
		status = 400;
		break;
	default:
		status = 409;
		break;
	}
	message = g_strdup_printf("%c%s.", g_ascii_toupper(error->message[0]), error->message + 1);
	return axis_web_error_respond(request, status, heading, message);
}

static gchar *
title_label(
	const gchar *title,
	gint         year
){
	return year > 0 ? g_strdup_printf("%s (%d)", title != NULL ? title : "", year) : g_strdup(title != NULL ? title : "");
}

/* A whole number from the form (1 to 18 digits); 0 when missing or not one. */
static gint64
form_id(
	HtmxRequest *request,
	const gchar *name
){
	guint64 value = 0;

	if (!axis_web_storage_form_u64(request, name, &value) || value > (guint64)G_MAXINT64)
		return 0;
	return (gint64)value;
}

static const gchar *
done_message(const gchar *key)
{
	static const struct { const gchar *key; const gchar *message; } messages[] = {
		{ "requested", "Requested. It waits for an administrator." },
		{ "request_approved", "Requested and approved. Axis is looking for it." },
		{ "request_followed", "It was requested already: you now follow that request." },
		{ "request_available", "It is in the library already. Nothing was requested." },
		{ "request_unchanged", "You asked for it already. Nothing changed." },
		{ "withdrawn", "The request was withdrawn." },
		{ "handed_over", "You withdrew. A person who follows the request has taken it over." },
		{ "unfollowed", "You no longer follow the request." },
		{ "approved", "Approved: the title was added and its search started." },
		{ "approved_not_added", "Approved, but the title could not be added. The request says why." },
		{ "declined", "Declined." },
		{ "retried", "The title was added and its search started." },
		{ "retry_failed", "The title could still not be added. The request says why." },
		{ "closed", "The title is closed to requests." },
		{ "opened", "The title can be requested again." },
		{ "defaults_saved", "The defaults were saved." },
		{ "person_saved", "The person's switches and limits were saved." }
	};
	guint i;

	for (i = 0; key != NULL && i < G_N_ELEMENTS(messages); i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].message;
	}
	return NULL;
}

/* ── Request on a title's card and page ─────────────────────────── */

typedef struct
{
	GHashTable *states;	/* title id -> AxisRequestTitleState */
	GHashTable *no_movies;	/* accounts whose limit for movies is 0 */
	GHashTable *no_series;
	gboolean    show_who;
	gboolean    movies_on;
	gboolean    series_on;
} Figures;

static void
figures_free(gpointer data)
{
	Figures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->states, g_hash_table_unref);
	g_clear_pointer(&figures->no_movies, g_hash_table_unref);
	g_clear_pointer(&figures->no_series, g_hash_table_unref);
	g_free(figures);
}

static gpointer
actions_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	AxisRequests *self = user_data;
	Figures *figures = g_new0(Figures, 1);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) people = NULL;
	g_autoptr(GError) error = NULL;
	gint64 movies_limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES, 5);
	gint64 seasons_limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SEASONS, 5);
	guint i;

	figures->show_who = axis_requests_show_who(self);
	figures->movies_on = axis_requests_kind_on(self, AXIS_REQUEST_MOVIE);
	figures->series_on = axis_requests_kind_on(self, AXIS_REQUEST_SERIES);
	figures->no_movies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	figures->no_series = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		if (title->id > 0 && (title->kind == AXIS_META_KIND_FILM || title->kind == AXIS_META_KIND_SERIES))
			g_array_append_val(ids, title->id);
	}
	if (!axis_requests_title_states(self, ids, &figures->states, &error)) {
		g_message("requests: the titles' states could not be read: %s", error->message);
		figures_free(figures);
		return NULL;
	}
	/* Who is offered no request of a kind: the few whose limit is none (an
	 * administrator made every account; they are a household's). */
	if (!axis_requests_list_people(self, &people, &error)) {
		g_message("requests: the people's limits could not be read: %s", error->message);
		return figures;
	}
	for (i = 0; i < people->len; i++) {
		const AxisRequestPerson *person = g_ptr_array_index(people, i);
		gint64 *key;

		if (person->admin)
			continue;
		if ((person->movies_limit >= 0 ? person->movies_limit : movies_limit) == 0) {
			key = g_new(gint64, 1);
			*key = person->id;
			g_hash_table_add(figures->no_movies, key);
		}
		if ((person->seasons_limit >= 0 ? person->seasons_limit : seasons_limit) == 0) {
			key = g_new(gint64, 1);
			*key = person->id;
			g_hash_table_add(figures->no_series, key);
		}
	}
	return figures;
}

/* The mark of a state: fixed words, and who asked when the viewer is shown. */
static HtmxNode *
state_mark(
	AxisRequestState            state,
	const AxisRequestTitleView *view
){
	HtmxSpan *mark = htmx_span_new();
	g_autofree gchar *css = g_strdup_printf("request-mark-%s", axis_request_state_name(state));
	g_autofree gchar *words = NULL;

	htmx_element_add_class(HTMX_ELEMENT(mark), "request-mark");
	htmx_element_add_class(HTMX_ELEMENT(mark), css);
	if (view != NULL && view->asked_by != NULL && (state == AXIS_REQUEST_STATE_REQUESTED ||
	    state == AXIS_REQUEST_STATE_APPROVED || state == AXIS_REQUEST_STATE_DECLINED))
		words = g_strdup_printf("%s: %s asked for it", axis_request_state_label(state), view->asked_by);
	htmx_node_set_text_content(HTMX_NODE(mark), words != NULL ? words : axis_request_state_label(state));
	return HTMX_NODE(mark);
}

/* Whether anything of the title can still be asked for. */
static gboolean
can_ask(const AxisRequestTitleState *state)
{
	guint i;

	if (state->state == AXIS_REQUEST_STATE_CLOSED)
		return FALSE;
	if (state->kind == AXIS_REQUEST_MOVIE)
		return state->state == AXIS_REQUEST_STATE_CAN || state->state == AXIS_REQUEST_STATE_DECLINED;
	for (i = 0; i < state->seasons->len; i++) {
		AxisRequestState season = g_array_index(state->seasons, AxisRequestSeasonState, i).state;

		if (season == AXIS_REQUEST_STATE_CAN || season == AXIS_REQUEST_STATE_DECLINED ||
		    season == AXIS_REQUEST_STATE_PARTLY)
			return TRUE;
	}
	return FALSE;
}

static HtmxNode *
option(
	const gchar *value,
	const gchar *label,
	gboolean     selected
){
	HtmxOption *one = htmx_option_new_with_value(value, label);

	if (selected)
		htmx_option_set_selected(one, TRUE);
	return HTMX_NODE(one);
}

/* The Request form: a title, a kind and seasons, and where to come back to. */
static HtmxNode *
request_form(
	HtmxRequest                 *request,
	const AxisRequestTitleState *state,
	gboolean                     with_picked
){
	HtmxNode *form = axis_web_auth_post_form(request, "/requests");
	g_autofree gchar *title = axis_requests_int_text(state->title_id);
	g_autofree gchar *card = axis_web_back_add_place(request, state->title_id);
	HtmxNode *back = axis_web_back_add_field(request, state->title_id);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(form), "request-form");
	ADD(form, axis_web_auth_hidden("title", title));
	ADD(form, axis_web_auth_hidden("kind", axis_request_kind_name(state->kind)));
	if (back != NULL) {
		ADD(form, back);
		ADD(form, axis_web_auth_hidden(AXIS_WEB_PLACE_AT, with_picked ? "requests" : card));
	}
	if (state->kind == AXIS_REQUEST_SERIES) {
		g_autofree gchar *select_id = g_strdup_printf("request-seasons-%" G_GINT64_FORMAT, state->title_id);
		HtmxLabel *label = htmx_label_new_for(select_id);
		HtmxSelect *select = htmx_select_new_with_name("seasons");

		htmx_element_add_class(HTMX_ELEMENT(label), "visually-hidden");
		htmx_node_set_text_content(HTMX_NODE(label), "Seasons to ask for");
		htmx_element_set_id(HTMX_ELEMENT(select), select_id);
		ADD(select, option("all", "The whole series", TRUE));
		ADD(select, option("first", "The first season", FALSE));
		ADD(select, option("latest", "The latest season", FALSE));
		if (with_picked)
			ADD(select, option("picked", "The seasons ticked below", FALSE));
		ADD(form, label);
		ADD(form, select);
		for (i = 0; with_picked && i < state->seasons->len; i++) {
			const AxisRequestSeasonState *season = &g_array_index(state->seasons, AxisRequestSeasonState, i);
			g_autofree gchar *name = g_strdup_printf("season-%d", season->season);
			g_autofree gchar *box_id = g_strdup_printf("request-%" G_GINT64_FORMAT "-%s", state->title_id, name);
			g_autofree gchar *words = season->season == 0 ? g_strdup("Specials") :
				g_strdup_printf("Season %d", season->season);

			if (season->state != AXIS_REQUEST_STATE_CAN && season->state != AXIS_REQUEST_STATE_DECLINED &&
			    season->state != AXIS_REQUEST_STATE_PARTLY)
				continue;
			ADD(form, axis_web_storage_checkbox(box_id, words, name, FALSE));
		}
	}
	ADD(form, axis_web_auth_button("Request", "button-primary button-small"));
	return form;
}

/* What a post that came back to this title says. */
static HtmxNode *
done_line(
	HtmxRequest *request,
	gint64       title_id,
	gboolean     page
){
	const gchar *at = axis_web_place_of(request);
	const gchar *message = done_message(htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE));
	g_autofree gchar *card = g_strdup_printf("title-%" G_GINT64_FORMAT, title_id);

	if (message == NULL || at == NULL || g_strcmp0(at, page ? "requests" : card) != 0)
		return NULL;
	return axis_web_auth_notice(FALSE, message);
}

static HtmxNode *
actions_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             data,
	gpointer             user_data
){
	Figures *figures = data;
	const AxisRequestTitleState *state;
	AxisRequestTitleView view;
	gint64 viewer = axis_web_request_get_user_id(request);
	gboolean admin = axis_web_request_is_admin(request);
	gboolean offered;
	HtmxDiv *piece;
	HtmxNode *done;

	if (figures == NULL || figures->states == NULL || title->id <= 0)
		return NULL;
	if (place != AXIS_WEB_ACTION_CARD && place != AXIS_WEB_ACTION_DISCOVER && place != AXIS_WEB_ACTION_PAGE)
		return NULL;
	state = g_hash_table_lookup(figures->states, &title->id);
	if (state == NULL || !(state->kind == AXIS_REQUEST_SERIES ? figures->series_on : figures->movies_on))
		return NULL;
	axis_request_title_view(state, viewer, admin, figures->show_who, &view);
	/* An administrator has Add; a person whose limit for the kind is none is
	 * offered none. */
	offered = !admin && can_ask(state) && !g_hash_table_contains(state->kind == AXIS_REQUEST_SERIES ?
		figures->no_series : figures->no_movies, &viewer);

	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "request-piece");
	done = done_line(request, title->id, place == AXIS_WEB_ACTION_PAGE);
	if (place != AXIS_WEB_ACTION_PAGE) {
		ADD(piece, state_mark(state->state, &view));
		if (done != NULL)
			ADD(piece, done);
		if (offered)
			ADD(piece, request_form(request, state, FALSE));
		return HTMX_NODE(piece);
	}

	htmx_element_set_id(HTMX_ELEMENT(piece), "requests");
	if (done != NULL)
		ADD(piece, done);
	ADD(piece, state_mark(state->state, &view));
	if (view.mine)
		ADD(piece, text("help", "You asked for it. It is among your requests."));
	else if (view.follows)
		ADD(piece, text("help", "You follow a request for it. It is among your requests."));
	if (view.decline_reason != NULL) {
		g_autofree gchar *reason = g_strdup_printf("Declined: %s", view.decline_reason);

		ADD(piece, text("request-reason", reason));
	}
	if (state->kind == AXIS_REQUEST_SERIES && state->seasons->len > 0) {
		static const AxisWebColumn columns[] = {
			{ "Season", AXIS_WEB_COL_NAME },
			{ "Requests", AXIS_WEB_COL_STATE },
			{ NULL, 0 }
		};
		HtmxNode *table = axis_web_table_new("request-seasons", columns);
		HtmxTbody *body = htmx_tbody_new();
		guint i;

		for (i = 0; i < state->seasons->len; i++) {
			const AxisRequestSeasonState *season = &g_array_index(state->seasons, AxisRequestSeasonState, i);
			g_autofree gchar *name = season->season == 0 ? g_strdup("Specials") :
				g_strdup_printf("Season %d", season->season);
			g_autofree gchar *words = season->state == AXIS_REQUEST_STATE_PARTLY ?
				g_strdup_printf("%s, %u of %u episodes", axis_request_state_label(season->state), season->have,
					season->aired) : g_strdup(axis_request_state_label(season->state));
			HtmxTr *row = htmx_tr_new();

			ADD(row, cell(name));
			ADD(row, cell(words));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(piece, table);
	}
	if (offered)
		ADD(piece, request_form(request, state, TRUE));
	else if (!admin && state->state == AXIS_REQUEST_STATE_CLOSED)
		ADD(piece, text("help", "An administrator closed this title to requests."));
	return HTMX_NODE(piece);
}

/* ── Asking ─────────────────────────────────────────────────────── */

typedef struct
{
	AxisRequestAsk   ask;
	gint             picked[AXIS_REQUESTS_MAX_SEASONS];
	gchar           *back;
	gchar           *at;
	AxisRequestMade  made;
	gboolean         done;
	GError          *error;
} AskWork;

static void
ask_work_free(gpointer data)
{
	AskWork *work = data;

	g_free(work->back);
	g_free(work->at);
	if (work->done)
		axis_request_made_clear(&work->made);
	g_clear_error(&work->error);
	g_free(work);
}

static void
ask_run(
	AxisRequests *self,
	gpointer      data
){
	AskWork *work = data;

	work->done = TRUE;
	axis_requests_ask(self, &work->ask, &work->made, &work->error);
}

static HtmxResponse *
ask_done(
	AxisRequests *self,
	HtmxRequest  *request,
	gpointer      data
){
	AskWork *work = data;
	const gchar *key;

	if (work->error != NULL)
		return refusal(request, "Not requested", work->error);
	switch (work->made.outcome) {
	case AXIS_REQUEST_MADE:
		axis_web_storage_audit(request, "request %s made for title %" G_GINT64_FORMAT, work->made.public_id,
			work->ask.title_id);
		key = work->made.decision == AXIS_REQUEST_APPROVED ? "request_approved" : "requested";
		break;
	case AXIS_REQUEST_FOLLOWED:
		key = "request_followed";
		break;
	case AXIS_REQUEST_ALREADY_AVAILABLE:
		key = "request_available";
		break;
	default:
		key = "request_unchanged";
		break;
	}
	if (work->back != NULL)
		return axis_web_place_redirect(request, work->back, key, work->at);
	return axis_web_place_redirect(request, "/requests", key, NULL);
}

/* "season-N": the number, or -1. */
static gint
season_field(const gchar *name)
{
	const gchar *digits = name + strlen("season-");
	guint i;

	if (!g_str_has_prefix(name, "season-") || digits[0] == '\0' || strlen(digits) > 5)
		return -1;
	for (i = 0; digits[i] != '\0'; i++) {
		if (!g_ascii_isdigit(digits[i]))
			return -1;
	}
	return (gint)g_ascii_strtoll(digits, NULL, 10);
}

static HtmxResponse *
handle_ask(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisRequests *self = user_data;
	GHashTable *form = htmx_request_get_form_data(request);
	const gchar *at = htmx_request_get_form_value(request, AXIS_WEB_PLACE_AT);
	const gchar *seasons = htmx_request_get_form_value(request, "seasons");
	AskWork *work = g_new0(AskWork, 1);
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	/* A request is a title, a kind and seasons.  Any other field (a folder, a
	 * profile, what to monitor, an episode, a note) refuses it whole. */
	if (form != NULL) {
		g_hash_table_iter_init(&iter, form);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			const gchar *name = key;
			gint season;

			if (strcmp(name, AXIS_CSRF_FIELD) == 0 || strcmp(name, "title") == 0 || strcmp(name, "kind") == 0 ||
			    strcmp(name, "seasons") == 0 || strcmp(name, AXIS_WEB_BACK_FIELD) == 0 ||
			    strcmp(name, AXIS_WEB_PLACE_AT) == 0)
				continue;
			season = season_field(name);
			if (season < 0 || work->ask.n_picked >= AXIS_REQUESTS_MAX_SEASONS) {
				ask_work_free(work);
				return axis_web_error_respond(request, 400, "Not requested",
					"A request names a title, a kind and seasons, and nothing else.");
			}
			if (g_strcmp0(value, "yes") == 0)
				work->picked[work->ask.n_picked++] = season;
		}
	}
	work->ask.user_id = axis_web_request_get_user_id(request);
	work->ask.title_id = form_id(request, "title");
	work->ask.picked = work->picked;
	if (work->ask.user_id <= 0 || work->ask.title_id <= 0 ||
	    !axis_request_kind_from_name(htmx_request_get_form_value(request, "kind"), &work->ask.kind) ||
	    (work->ask.kind == AXIS_REQUEST_SERIES && !axis_request_seasons_from_name(seasons, &work->ask.seasons)) ||
	    (work->ask.kind == AXIS_REQUEST_MOVIE && (seasons != NULL || work->ask.n_picked > 0))) {
		ask_work_free(work);
		return axis_web_error_respond(request, 400, "Not requested", "The form is not complete.");
	}
	/* Seasons ticked count only when the person chose "picked". */
	if (work->ask.kind == AXIS_REQUEST_SERIES && work->ask.seasons != AXIS_REQUEST_SEASONS_PICKED)
		work->ask.n_picked = 0;
	work->back = axis_web_back_posted(request);
	work->at = axis_web_place_id_valid(at) ? g_strdup(at) : NULL;
	return dispatch(self, request, ask_run, ask_done, work, ask_work_free);
}

/* ── Acting on one request ──────────────────────────────────────── */

typedef enum
{
	ACT_WITHDRAW,
	ACT_UNFOLLOW,
	ACT_APPROVE,
	ACT_DECLINE,
	ACT_RETRY,
	ACT_CLOSE,
	ACT_OPEN
} Act;

typedef struct
{
	Act       act;
	gchar    *public_id;
	gchar    *user;
	gint64    user_id;
	gint64    title_id;
	gint64    root_id;
	gint64    profile_id;
	gchar    *reason;
	gboolean  close_title;
	gboolean  flag;		/* handed over; added */
	GError   *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->public_id);
	g_free(work->user);
	g_free(work->reason);
	g_clear_error(&work->error);
	g_free(work);
}

static void
act_run(
	AxisRequests *self,
	gpointer      data
){
	ActWork *work = data;

	switch (work->act) {
	case ACT_WITHDRAW:
		axis_requests_withdraw(self, work->public_id, work->user_id, &work->flag, &work->error);
		break;
	case ACT_UNFOLLOW:
		axis_requests_unfollow(self, work->public_id, work->user_id, &work->error);
		break;
	case ACT_APPROVE:
		axis_requests_approve(self, work->public_id, work->user, work->root_id, work->profile_id, &work->flag,
			&work->error);
		break;
	case ACT_DECLINE:
		axis_requests_decline(self, work->public_id, work->user, work->reason, work->close_title, &work->error);
		break;
	case ACT_RETRY:
		axis_requests_try_again(self, work->public_id, work->user, work->root_id, work->profile_id, &work->flag,
			&work->error);
		break;
	default:
		axis_requests_set_title_closed(self, work->title_id, work->act == ACT_CLOSE, work->user, &work->error);
		break;
	}
}

static HtmxResponse *
act_done(
	AxisRequests *self,
	HtmxRequest  *request,
	gpointer      data
){
	static const gchar *const headings[] = {
		"Not withdrawn", "Not changed", "Not approved", "Not declined", "Not added", "Not closed", "Not opened"
	};
	ActWork *work = data;
	g_autofree gchar *place = work->public_id != NULL ? g_strdup_printf("request-%s", work->public_id) : NULL;
	const gchar *key;

	if (work->error != NULL)
		return refusal(request, headings[work->act], work->error);
	switch (work->act) {
	case ACT_WITHDRAW:
		key = work->flag ? "handed_over" : "withdrawn";
		break;
	case ACT_UNFOLLOW:
		key = "unfollowed";
		break;
	case ACT_APPROVE:
		axis_web_storage_audit(request, "request %s approved", work->public_id);
		key = work->flag ? "approved" : "approved_not_added";
		break;
	case ACT_DECLINE:
		axis_web_storage_audit(request, "request %s declined%s", work->public_id,
			work->close_title ? ", its title closed to requests" : "");
		key = "declined";
		break;
	case ACT_RETRY:
		axis_web_storage_audit(request, "request %s placed again", work->public_id);
		key = work->flag ? "retried" : "retry_failed";
		break;
	case ACT_CLOSE:
		axis_web_storage_audit(request, "title %" G_GINT64_FORMAT " closed to requests", work->title_id);
		return axis_web_place_redirect(request, "/requests", "closed", "closed");
	default:
		axis_web_storage_audit(request, "title %" G_GINT64_FORMAT " opened to requests", work->title_id);
		return axis_web_place_redirect(request, "/requests", "opened", "closed");
	}
	return axis_web_place_redirect(request, "/requests", key, place);
}

static HtmxResponse *
start_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	Act          act
){
	AxisRequests *self = user_data;
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	ActWork *work = g_new0(ActWork, 1);

	work->act = act;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->user_id = axis_web_request_get_user_id(request);
	if (act == ACT_CLOSE || act == ACT_OPEN) {
		work->title_id = axis_web_storage_param_id(params);
		if (work->title_id <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 404, "Not found", "There is no such title.");
		}
		return dispatch(self, request, act_run, act_done, work, act_work_free);
	}
	if (id == NULL || strlen(id) != 22) {
		act_work_free(work);
		return not_found(request);
	}
	work->public_id = g_strdup(id);
	work->root_id = form_id(request, "root");
	work->profile_id = form_id(request, "profile");
	work->reason = g_strdup(htmx_request_get_form_value(request, "reason"));
	work->close_title = axis_web_storage_form_yes(request, "close");
	return dispatch(self, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_withdraw(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_WITHDRAW);
}

static HtmxResponse *
handle_unfollow(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_UNFOLLOW);
}

static HtmxResponse *
handle_approve(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_APPROVE);
}

static HtmxResponse *
handle_decline(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_DECLINE);
}

static HtmxResponse *
handle_retry(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_RETRY);
}

static HtmxResponse *
handle_close(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_CLOSE);
}

static HtmxResponse *
handle_open(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_OPEN);
}

/* ── The pages ──────────────────────────────────────────────────── */

typedef struct
{
	gint64  id;
	gchar  *label;
} Choice;

static void
choice_free(gpointer data)
{
	Choice *choice = data;

	g_free(choice->label);
	g_free(choice);
}

typedef struct
{
	gboolean             admin;
	gint64               viewer;
	gchar               *public_id;		/* one request's page */
	GPtrArray           *requests;
	guint                n_waiting;
	AxisRequestQuota     quota;
	gboolean             quota_read;
	AxisRequestDefaults  defaults;
	GPtrArray           *movie_roots;	/* Choice */
	GPtrArray           *series_roots;
	GPtrArray           *profiles;
	GPtrArray           *closed;
	GPtrArray           *people;
	AxisRequestInfo     *one;
	gint64               movies_limit;
	gint64               seasons_limit;
	GError              *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->public_id);
	g_clear_pointer(&work->requests, g_ptr_array_unref);
	g_clear_pointer(&work->movie_roots, g_ptr_array_unref);
	g_clear_pointer(&work->series_roots, g_ptr_array_unref);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	g_clear_pointer(&work->closed, g_ptr_array_unref);
	g_clear_pointer(&work->people, g_ptr_array_unref);
	axis_request_info_free(work->one);
	g_clear_error(&work->error);
	g_free(work);
}

/* The folders of each kind and the profiles, for an administrator's forms. */
static gboolean
read_choices(
	AxisRequests  *self,
	PageWork      *work,
	GError       **error
){
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GPtrArray) profiles = NULL;
	guint i;

	work->movie_roots = g_ptr_array_new_with_free_func(choice_free);
	work->series_roots = g_ptr_array_new_with_free_func(choice_free);
	work->profiles = g_ptr_array_new_with_free_func(choice_free);
	if (self->storage != NULL) {
		if (!axis_storage_list_roots(self->storage, 0, &roots, error))
			return FALSE;
		for (i = 0; i < roots->len; i++) {
			const AxisRootInfo *root = g_ptr_array_index(roots, i);
			Choice *choice;

			if (root->media_type != AXIS_MEDIA_MOVIES && root->media_type != AXIS_MEDIA_TV)
				continue;
			choice = g_new0(Choice, 1);
			choice->id = root->id;
			choice->label = g_strdup(root->name);
			g_ptr_array_add(root->media_type == AXIS_MEDIA_TV ? work->series_roots : work->movie_roots, choice);
		}
	}
	if (self->quality != NULL) {
		if (!axis_quality_store_list(self->quality, &profiles, error))
			return FALSE;
		for (i = 0; i < profiles->len; i++) {
			const AxisQualityStored *stored = g_ptr_array_index(profiles, i);
			Choice *choice;

			if (!stored->usable)
				continue;
			choice = g_new0(Choice, 1);
			choice->id = stored->id;
			choice->label = g_strdup(stored->name);
			g_ptr_array_add(work->profiles, choice);
		}
	}
	return TRUE;
}

static void
page_run(
	AxisRequests *self,
	gpointer      data
){
	PageWork *work = data;

	if (work->public_id != NULL) {
		if (!axis_requests_get(self, work->public_id, work->viewer, work->admin, &work->one, &work->error))
			return;
		if (work->admin && axis_requests_get_defaults(self, &work->defaults, &work->error))
			read_choices(self, work, &work->error);
		return;
	}
	if (!work->admin) {
		g_autoptr(GError) local = NULL;

		if (!axis_requests_list_mine(self, work->viewer, PAGE_ROWS, &work->requests, &work->error))
			return;
		work->quota_read = axis_requests_get_quota(self, work->viewer, &work->quota, &local);
		return;
	}
	if (!axis_requests_list_all(self, PAGE_ROWS, &work->requests, &work->n_waiting, &work->error) ||
	    !axis_requests_get_defaults(self, &work->defaults, &work->error) ||
	    !read_choices(self, work, &work->error))
		return;
	axis_requests_list_closed(self, &work->closed, &work->error);
}

static HtmxNode *
choice_select(
	const gchar     *id,
	const gchar     *name,
	const gchar     *label_text,
	const GPtrArray *choices,
	gint64           chosen,
	const gchar     *none
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	gboolean found = FALSE;
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; choices != NULL && i < choices->len; i++)
		found = found || ((const Choice *)g_ptr_array_index(choices, i))->id == chosen;
	ADD(select, option("", none, !found));
	for (i = 0; choices != NULL && i < choices->len; i++) {
		const Choice *choice = g_ptr_array_index(choices, i);
		g_autofree gchar *value = axis_requests_int_text(choice->id);

		ADD(select, option(value, choice->label, choice->id == chosen));
	}
	ADD(field, label);
	ADD(field, select);
	return HTMX_NODE(field);
}

/* The folder and the profile of an approval, prefilled with the defaults. */
static void
add_placement(
	HtmxNode              *form,
	const PageWork        *work,
	const AxisRequestInfo *info
){
	gboolean series = info->kind == AXIS_REQUEST_SERIES;
	gint64 root = info->root_id != 0 ? info->root_id : series ? work->defaults.series_root : work->defaults.movies_root;
	gint64 profile = info->profile_id != 0 ? info->profile_id : series ? work->defaults.series_profile :
		work->defaults.movies_profile;
	g_autofree gchar *root_id = g_strdup_printf("root-%s", info->public_id);
	g_autofree gchar *profile_id = g_strdup_printf("profile-%s", info->public_id);

	ADD(form, choice_select(root_id, "root", "Library folder", series ? work->series_roots : work->movie_roots, root,
		"Choose a folder"));
	ADD(form, choice_select(profile_id, "profile", "Quality profile", work->profiles, profile, "Choose a profile"));
}

static const gchar *
wait_words(const gchar *reason)
{
	if (g_strcmp0(reason, "no_defaults") == 0)
		return "Its requests are approved automatically, but no default folder and profile are set for the kind.";
	if (g_strcmp0(reason, "defaults_gone") == 0)
		return "Its requests are approved automatically, but the default folder or profile is missing.";
	if (g_strcmp0(reason, "too_many_seasons") == 0)
		return "It lists more seasons than are approved automatically.";
	return NULL;
}

static HtmxNode *
title_cell(const AxisRequestInfo *info)
{
	HtmxTd *td = htmx_td_new();
	g_autofree gchar *label = title_label(info->title, info->year);
	g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, info->title_id);
	HtmxA *link = htmx_a_new_with_href(href);

	htmx_node_set_text_content(HTMX_NODE(link), label);
	ADD(td, link);
	return HTMX_NODE(td);
}

/* The controls of a request for an administrator. */
static HtmxNode *
admin_controls(
	HtmxRequest           *request,
	const PageWork        *work,
	const AxisRequestInfo *info
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *base = g_strdup_printf("/requests/%s", info->public_id);

	htmx_element_add_class(HTMX_ELEMENT(box), "actions");
	if (info->decision == AXIS_REQUEST_WAITING) {
		g_autofree gchar *approve = g_strconcat(base, "/approve", NULL);
		g_autofree gchar *decline = g_strconcat(base, "/decline", NULL);
		g_autofree gchar *reason_id = g_strdup_printf("reason-%s", info->public_id);
		g_autofree gchar *close_id = g_strdup_printf("close-%s", info->public_id);
		HtmxNode *approve_form = axis_web_auth_post_form(request, approve);
		HtmxNode *decline_form = axis_web_auth_post_form(request, decline);
		HtmxNode *reason = axis_web_storage_text_field(reason_id, "Reason for declining", "reason", NULL, NULL,
			"Shown to the person who asked. At most 300 characters.");

		htmx_element_add_class(HTMX_ELEMENT(approve_form), "form");
		add_placement(approve_form, work, info);
		ADD(approve_form, axis_web_auth_button("Approve", "button-primary button-small"));
		ADD(box, approve_form);
		htmx_element_add_class(HTMX_ELEMENT(decline_form), "form");
		ADD(decline_form, reason);
		ADD(decline_form, axis_web_storage_checkbox(close_id, "Do not ask again: close the title to requests", "close",
			FALSE));
		ADD(decline_form, axis_web_auth_button("Decline", "button-small"));
		ADD(box, decline_form);
	} else if (info->decision == AXIS_REQUEST_APPROVED && info->state == AXIS_REQUEST_STATE_FAILED &&
	           (info->cause == AXIS_REQUEST_CAUSE_NOT_ADDED || info->cause == AXIS_REQUEST_CAUSE_REMOVED)) {
		g_autofree gchar *retry = g_strconcat(base, "/retry", NULL);
		HtmxNode *retry_form = axis_web_auth_post_form(request, retry);

		htmx_element_add_class(HTMX_ELEMENT(retry_form), "form");
		add_placement(retry_form, work, info);
		ADD(retry_form, axis_web_auth_button("Try again", "button-primary button-small"));
		ADD(box, retry_form);
	}
	if (info->decision == AXIS_REQUEST_DECLINED || info->decision == AXIS_REQUEST_WAITING) {
		g_autofree gchar *close = g_strdup_printf("/requests/titles/%" G_GINT64_FORMAT "/close", info->title_id);
		HtmxNode *close_form = axis_web_auth_post_form(request, close);

		htmx_element_add_class(HTMX_ELEMENT(close_form), "inline-form");
		ADD(close_form, axis_web_auth_button("Close the title to requests", "button-small"));
		ADD(box, close_form);
	}
	return HTMX_NODE(box);
}

/* What an administrator is told beside a request, added to @td (a table
 * cell on the list, a block on the request's own page). */
static void
add_admin_notes(
	gpointer               td,
	const AxisRequestInfo *info
){
	g_autofree gchar *state = axis_request_info_words(info);

	ADD(td, text(NULL, state));
	if (info->decision == AXIS_REQUEST_WAITING) {
		const gchar *why = wait_words(info->wait_reason);
		g_autofree gchar *look = NULL;

		if (!info->look_known)
			look = g_strdup("An approval looks for the aired episodes of its seasons; how many is not known until "
				"the series' episodes have been read.");
		else if (info->kind == AXIS_REQUEST_MOVIE)
			look = g_strdup(info->would_look_for == 0 ? "An approval looks for nothing: it is in the library." :
				"An approval looks for 1 movie.");
		else
			look = g_strdup_printf("An approval looks for %u aired %s.", info->would_look_for,
				info->would_look_for == 1 ? "episode" : "episodes");
		ADD(td, text("help", look));
		if (why != NULL)
			ADD(td, text("help", why));
	}
	if (info->n_followers > 0) {
		g_autofree gchar *followers = g_strdup_printf("%u more %s for it.", info->n_followers,
			info->n_followers == 1 ? "person asked" : "people asked");

		ADD(td, text("help", followers));
	}
	if (info->declined_before > 0) {
		g_autofree gchar *before = info->declined_before == 1 ? g_strdup("It was declined once before.") :
			g_strdup_printf("It was declined %u times before.", info->declined_before);

		ADD(td, text("help", before));
	}
	if (info->decision == AXIS_REQUEST_DECLINED && info->decline_reason != NULL) {
		g_autofree gchar *reason = g_strdup_printf("Reason: %s", info->decline_reason);

		ADD(td, text("help", reason));
	}
	/* The engine's own reason: an administrator's alone. */
	if (info->decision == AXIS_REQUEST_APPROVED && info->add_error != NULL && info->added_us == 0)
		ADD(td, text("request-add-error", info->add_error));
}

static HtmxNode *
admin_notes(const AxisRequestInfo *info)
{
	HtmxTd *td = htmx_td_new();

	add_admin_notes(td, info);
	return HTMX_NODE(td);
}

static HtmxNode *
admin_table(
	HtmxRequest    *request,
	const PageWork *work,
	gboolean        waiting,
	AxisWebNotice  *notice
){
	static const AxisWebColumn columns[] = {
		{ "Title", AXIS_WEB_COL_NAME },
		{ "Asked by", AXIS_WEB_COL_TEXT },
		{ "Seasons", AXIS_WEB_COL_TEXT },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Decide", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("requests", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint shown = 0;
	guint i;

	for (i = 0; i < work->requests->len; i++) {
		const AxisRequestInfo *info = g_ptr_array_index(work->requests, i);
		g_autofree gchar *row_id = g_strdup_printf("request-%s", info->public_id);
		g_autofree gchar *seasons = axis_request_seasons_words(info->seasons);
		g_autofree gchar *when = axis_web_storage_format_time(info->created_us, "");
		g_autofree gchar *who = g_strdup_printf("%s%s, %s", info->requested_name, info->requested_by == 0 ?
			" (account removed)" : "", when);
		HtmxNode *first;
		HtmxTr *row;

		if ((info->decision == AXIS_REQUEST_WAITING) != waiting)
			continue;
		row = htmx_tr_new();
		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		first = title_cell(info);
		axis_web_notice_put(notice, first, row_id);
		ADD(row, first);
		ADD(row, cell(who));
		ADD(row, cell(seasons != NULL ? seasons : "Movie"));
		ADD(row, admin_notes(info));
		{
			HtmxTd *controls = htmx_td_new();

			ADD(controls, admin_controls(request, work, info));
			ADD(row, controls);
		}
		ADD(body, row);
		shown++;
	}
	if (shown == 0) {
		g_object_unref(body);
		g_object_unref(table);
		return NULL;
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
defaults_card(
	HtmxRequest    *request,
	const PageWork *work
){
	HtmxNode *card = axis_web_storage_card("Defaults");
	HtmxNode *form = axis_web_auth_post_form(request, "/requests/defaults");
	HtmxA *people = htmx_a_new_with_href("/requests/people");

	htmx_element_set_id(HTMX_ELEMENT(card), "defaults");
	ADD(card, text("help", "Where an approved request puts its title. A person who asks never chooses either. With "
		"none set, every approval asks you, and no request is approved automatically."));
	if ((work->defaults.movies_root != 0 || work->defaults.movies_profile != 0) && !work->defaults.movies_ok)
		ADD(card, axis_web_auth_notice(TRUE, "The default folder or profile for movie requests is missing. Choose "
			"them again."));
	if ((work->defaults.series_root != 0 || work->defaults.series_profile != 0) && !work->defaults.series_ok)
		ADD(card, axis_web_auth_notice(TRUE, "The default folder or profile for series requests is missing. Choose "
			"them again."));
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	ADD(form, choice_select("default-movies-root", "movies_root", "Library folder for requested movies",
		work->movie_roots, work->defaults.movies_root, "None"));
	ADD(form, choice_select("default-movies-profile", "movies_profile", "Quality profile for requested movies",
		work->profiles, work->defaults.movies_profile, "None"));
	ADD(form, choice_select("default-series-root", "series_root", "Library folder for requested series",
		work->series_roots, work->defaults.series_root, "None"));
	ADD(form, choice_select("default-series-profile", "series_profile", "Quality profile for requested series",
		work->profiles, work->defaults.series_profile, "None"));
	ADD(form, axis_web_auth_button("Save the defaults", "button-primary"));
	ADD(card, form);
	htmx_node_set_text_content(HTMX_NODE(people), "People: automatic approval and limits");
	ADD(card, people);
	return card;
}

static HtmxNode *
closed_card(
	HtmxRequest    *request,
	const PageWork *work
){
	static const AxisWebColumn columns[] = {
		{ "Title", AXIS_WEB_COL_NAME },
		{ "Closed", AXIS_WEB_COL_TEXT },
		{ "Open", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Closed to requests");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "closed");
	if (work->closed == NULL || work->closed->len == 0) {
		ADD(card, text("help", "No title is closed to requests. Close one when you decline a request for it, with "
			"\"Do not ask again\"."));
		return card;
	}
	table = axis_web_table_new("requests-closed", columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->closed->len; i++) {
		const AxisRequestClosed *closed = g_ptr_array_index(work->closed, i);
		g_autofree gchar *label = title_label(closed->title, closed->year);
		g_autofree gchar *when = axis_web_storage_format_time(closed->closed_us, "");
		g_autofree gchar *who = g_strdup_printf("%s%s%s", closed->closed_by != NULL ? closed->closed_by : "", closed->closed_by !=
			NULL ? ", " : "", when);
		g_autofree gchar *action = g_strdup_printf("/requests/titles/%" G_GINT64_FORMAT "/open", closed->title_id);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *controls = htmx_td_new();

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_button("Open to requests again", "button-small"));
		ADD(controls, form);
		ADD(row, cell(label));
		ADD(row, cell(who));
		ADD(row, controls);
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxResponse *
admin_page(
	HtmxRequest    *request,
	const PageWork *work
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) heading = htmx_h1_new();
	HtmxNode *waiting = axis_web_storage_card("Waiting for you");
	HtmxNode *decided = axis_web_storage_card("Decided");
	HtmxNode *table;
	AxisWebNotice notice;

	htmx_node_set_text_content(HTMX_NODE(heading), "Requests");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(heading));
	ADD(content, axis_web_page_intro("What people asked for. A request waits here until you approve or decline "
		"it; an approval adds the title with the folder and the quality profile you choose, and starts its search."));
	axis_web_notice_begin(&notice, request, FALSE,
		g_strdup(done_message(htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE))), HTMX_NODE(content));
	htmx_element_set_id(HTMX_ELEMENT(waiting), "waiting");
	table = admin_table(request, work, TRUE, &notice);
	if (table != NULL) {
		if (work->n_waiting > PAGE_ROWS) {
			g_autofree gchar *more = g_strdup_printf("%u requests wait; the oldest %u are listed.", work->n_waiting,
				(guint)PAGE_ROWS);

			ADD(waiting, text("help", more));
		}
		ADD(waiting, table);
	} else {
		ADD(waiting, text("help", "No request waits for you."));
	}
	ADD(content, waiting);
	htmx_element_set_id(HTMX_ELEMENT(decided), "decided");
	table = admin_table(request, work, FALSE, &notice);
	if (table != NULL)
		ADD(decided, table);
	else
		ADD(decided, text("help", "Nothing was decided yet."));
	ADD(content, decided);
	{
		HtmxNode *defaults = defaults_card(request, work);
		HtmxNode *closed = closed_card(request, work);

		axis_web_notice_put(&notice, defaults, "defaults");
		axis_web_notice_put(&notice, closed, "closed");
		ADD(content, defaults);
		ADD(content, closed);
	}
	axis_web_notice_end(&notice);
	axis_web_table_apply_roles(HTMX_NODE(content));
	return axis_web_page_respond(request, "Requests", HTMX_NODE(content));
}

/* A request's state for the person who asked or follows: fixed words, and
 * the administrator's reason for a decline. */
static HtmxNode *
own_state_cell(const AxisRequestInfo *info)
{
	HtmxTd *td = htmx_td_new();
	g_autofree gchar *state = axis_request_info_words(info);

	ADD(td, text(NULL, state));
	if ((info->decision == AXIS_REQUEST_DECLINED || info->decision == AXIS_REQUEST_REPLACED) &&
	    info->decline_reason != NULL) {
		g_autofree gchar *reason = g_strdup_printf("Reason: %s", info->decline_reason);

		ADD(td, text("help", reason));
	}
	return HTMX_NODE(td);
}

static HtmxNode *
own_controls(
	HtmxRequest           *request,
	const PageWork        *work,
	const AxisRequestInfo *info
){
	HtmxTd *td = htmx_td_new();
	g_autofree gchar *action = NULL;
	HtmxNode *form;

	if (info->requested_by == work->viewer && info->decision == AXIS_REQUEST_WAITING) {
		action = g_strdup_printf("/requests/%s/withdraw", info->public_id);
		form = axis_web_auth_post_form(request, action);
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_button("Withdraw", "button-small"));
		ADD(td, form);
	} else if (info->follows && info->requested_by != work->viewer) {
		action = g_strdup_printf("/requests/%s/unfollow", info->public_id);
		form = axis_web_auth_post_form(request, action);
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		ADD(form, axis_web_auth_button("Stop following", "button-small"));
		ADD(td, form);
	}
	return HTMX_NODE(td);
}

static HtmxResponse *
own_page(
	AxisRequests   *self,
	HtmxRequest    *request,
	const PageWork *work
){
	static const AxisWebColumn columns[] = {
		{ "Title", AXIS_WEB_COL_NAME },
		{ "Seasons", AXIS_WEB_COL_TEXT },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Asked", AXIS_WEB_COL_TEXT },
		{ "Change", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) heading = htmx_h1_new();
	HtmxNode *card = axis_web_storage_card("What you asked for");
	gboolean show_who = axis_requests_show_who(self);
	AxisWebNotice notice;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(heading), "My requests");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(heading));
	ADD(content, axis_web_page_intro("What you asked for and what became of it. Ask for a movie or a series from "
		"its page, from a search or from Discover; an administrator approves or declines."));
	axis_web_notice_begin(&notice, request, FALSE,
		g_strdup(done_message(htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE))), HTMX_NODE(content));
	if (work->quota_read) {
		HtmxNode *left = axis_web_storage_card("What you can still ask for");
		g_autofree gchar *movies = axis_request_quota_words(&work->quota, AXIS_REQUEST_MOVIE);
		g_autofree gchar *seasons = axis_request_quota_words(&work->quota, AXIS_REQUEST_SERIES);
		g_autofree gchar *movies_line = g_strdup_printf("Movies: %s.", movies);
		g_autofree gchar *seasons_line = g_strdup_printf("Seasons of series: %s.", seasons);

		htmx_element_set_id(HTMX_ELEMENT(left), "quota");
		ADD(left, text(NULL, movies_line));
		ADD(left, text(NULL, seasons_line));
		ADD(left, text("help", "A request you withdraw gives its count back. One that was declined keeps it."));
		ADD(content, left);
	}
	htmx_element_set_id(HTMX_ELEMENT(card), "mine");
	if (work->requests->len == 0) {
		ADD(card, text("help", "You have not asked for anything yet."));
	} else {
		HtmxNode *table = axis_web_table_new("requests-mine", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->requests->len; i++) {
			const AxisRequestInfo *info = g_ptr_array_index(work->requests, i);
			g_autofree gchar *row_id = g_strdup_printf("request-%s", info->public_id);
			g_autofree gchar *seasons = axis_request_seasons_words(info->seasons);
			g_autofree gchar *when = axis_web_storage_format_time(info->created_us, "");
			g_autofree gchar *asked = NULL;
			HtmxTr *row = htmx_tr_new();
			HtmxNode *first = title_cell(info);

			/* Another person's name only as the setting allows. */
			if (info->requested_by == work->viewer)
				asked = g_strdup_printf("You, %s", when);
			else if (show_who)
				asked = g_strdup_printf("%s, %s. You follow it.", info->requested_name, when);
			else
				asked = g_strdup_printf("Someone else, %s. You follow it.", when);
			htmx_element_set_id(HTMX_ELEMENT(row), row_id);
			axis_web_notice_put(&notice, first, row_id);
			ADD(row, first);
			ADD(row, cell(seasons != NULL ? seasons : "Movie"));
			ADD(row, own_state_cell(info));
			ADD(row, cell(asked));
			ADD(row, own_controls(request, work, info));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);
	axis_web_notice_end(&notice);
	axis_web_table_apply_roles(HTMX_NODE(content));
	return axis_web_page_respond(request, "My requests", HTMX_NODE(content));
}

/* One request: to an administrator with its controls, to the person who
 * asked or follows as their list shows it. */
static HtmxResponse *
one_page(
	AxisRequests   *self,
	HtmxRequest    *request,
	const PageWork *work
){
	const AxisRequestInfo *info = work->one;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) heading = htmx_h1_new();
	g_autofree gchar *label = title_label(info->title, info->year);
	g_autofree gchar *seasons = axis_request_seasons_words(info->seasons);
	g_autofree gchar *state = axis_request_info_words(info);
	g_autofree gchar *when = axis_web_storage_format_time(info->created_us, "");
	g_autofree gchar *asked = NULL;
	HtmxNode *card = axis_web_storage_card(label);
	HtmxA *back = htmx_a_new_with_href("/requests");

	htmx_node_set_text_content(HTMX_NODE(heading), "A request");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(heading));
	htmx_node_set_text_content(HTMX_NODE(back), work->admin ? "All requests" : "My requests");
	ADD(content, back);
	if (work->admin || info->requested_by == work->viewer || axis_requests_show_who(self))
		asked = g_strdup_printf("Asked by %s, %s.", info->requested_by == work->viewer ? "you" : info->requested_name,
			when);
	else
		asked = g_strdup_printf("Asked %s.", when);
	ADD(card, text(NULL, seasons != NULL ? seasons : "Movie"));
	ADD(card, text(NULL, asked));
	if (work->admin) {
		HtmxDiv *holder = htmx_div_new();

		add_admin_notes(holder, info);
		ADD(card, holder);
		ADD(card, admin_controls(request, work, info));
	} else {
		ADD(card, text(NULL, state));
		if ((info->decision == AXIS_REQUEST_DECLINED || info->decision == AXIS_REQUEST_REPLACED) &&
		    info->decline_reason != NULL) {
			g_autofree gchar *reason = g_strdup_printf("Reason: %s", info->decline_reason);

			ADD(card, text("help", reason));
		}
	}
	ADD(content, card);
	return axis_web_page_respond(request, "A request", HTMX_NODE(content));
}

static HtmxResponse *
page_done(
	AxisRequests *self,
	HtmxRequest  *request,
	gpointer      data
){
	PageWork *work = data;

	if (work->error != NULL)
		return refusal(request, "Requests", work->error);
	if (work->public_id != NULL)
		return one_page(self, request, work);
	return work->admin ? admin_page(request, work) : own_page(self, request, work);
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisRequests *self = user_data;
	PageWork *work = g_new0(PageWork, 1);

	work->admin = axis_web_request_is_admin(request);
	work->viewer = axis_web_request_get_user_id(request);
	return dispatch(self, request, page_run, page_done, work, page_work_free);
}

static HtmxResponse *
handle_one(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisRequests *self = user_data;
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	PageWork *work;

	if (id == NULL || strlen(id) != 22)
		return not_found(request);
	work = g_new0(PageWork, 1);
	work->admin = axis_web_request_is_admin(request);
	work->viewer = axis_web_request_get_user_id(request);
	work->public_id = g_strdup(id);
	return dispatch(self, request, page_run, page_done, work, page_work_free);
}

/* ── The defaults and the people (administrators) ───────────────── */

typedef struct
{
	AxisRequestDefaults  defaults;
	AxisRequestPerson    person;
	gboolean             is_person;
	GError              *error;
} SaveWork;

static void
save_work_free(gpointer data)
{
	SaveWork *work = data;

	g_clear_error(&work->error);
	g_free(work);
}

static void
save_run(
	AxisRequests *self,
	gpointer      data
){
	SaveWork *work = data;

	if (work->is_person)
		axis_requests_set_person(self, &work->person, &work->error);
	else
		axis_requests_set_defaults(self, &work->defaults, &work->error);
}

static HtmxResponse *
save_done(
	AxisRequests *self,
	HtmxRequest  *request,
	gpointer      data
){
	SaveWork *work = data;
	g_autofree gchar *row = NULL;

	if (work->error != NULL)
		return refusal(request, "Not saved", work->error);
	if (!work->is_person) {
		axis_web_storage_audit(request, "the defaults for requests changed");
		return axis_web_place_redirect(request, "/requests", "defaults_saved", "defaults");
	}
	axis_web_storage_audit(request, "the request switches and limits of account %" G_GINT64_FORMAT " changed",
		work->person.id);
	row = g_strdup_printf("person-%" G_GINT64_FORMAT, work->person.id);
	return axis_web_place_redirect(request, "/requests/people", "person_saved", row);
}

static HtmxResponse *
handle_defaults(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SaveWork *work = g_new0(SaveWork, 1);

	work->defaults.movies_root = form_id(request, "movies_root");
	work->defaults.movies_profile = form_id(request, "movies_profile");
	work->defaults.series_root = form_id(request, "series_root");
	work->defaults.series_profile = form_id(request, "series_profile");
	return dispatch(user_data, request, save_run, save_done, work, save_work_free);
}

/* A limit from the form: empty for the installation's (-1), else 0 to 1000;
 * FALSE for anything else. */
static gboolean
form_limit(
	HtmxRequest *request,
	const gchar *name,
	gint        *limit
){
	const gchar *value = htmx_request_get_form_value(request, name);
	guint64 number = 0;

	*limit = -1;
	if (value == NULL || value[0] == '\0')
		return TRUE;
	if (!axis_web_storage_form_u64(request, name, &number) || number > AXIS_REQUESTS_MAX_LIMIT)
		return FALSE;
	*limit = (gint)number;
	return TRUE;
}

static HtmxResponse *
handle_person(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	SaveWork *work = g_new0(SaveWork, 1);

	work->is_person = TRUE;
	work->person.id = axis_web_storage_param_id(params);
	work->person.auto_movies = axis_web_storage_form_yes(request, "auto_movies");
	work->person.auto_series = axis_web_storage_form_yes(request, "auto_series");
	if (work->person.id <= 0) {
		save_work_free(work);
		return axis_web_error_respond(request, 404, "Not found", "There is no such account.");
	}
	if (!form_limit(request, "movies_limit", &work->person.movies_limit) ||
	    !form_limit(request, "seasons_limit", &work->person.seasons_limit)) {
		save_work_free(work);
		return axis_web_error_respond(request, 400, "Not saved", "A limit is a number from 0 to 1000, or empty for "
			"the installation's.");
	}
	return dispatch(user_data, request, save_run, save_done, work, save_work_free);
}

static void
people_run(
	AxisRequests *self,
	gpointer      data
){
	PageWork *work = data;

	work->movies_limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES, 5);
	work->seasons_limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SEASONS, 5);
	axis_requests_list_people(self, &work->people, &work->error);
}

static HtmxNode *
tick(
	const gchar *id,
	const gchar *name,
	const gchar *label_text,
	gboolean     checked
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_element_add_class(HTMX_ELEMENT(field), "field-checkbox");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	ADD(field, input);
	ADD(field, label);
	return HTMX_NODE(field);
}

static HtmxResponse *
people_done(
	AxisRequests *self,
	HtmxRequest  *request,
	gpointer      data
){
	static const AxisWebColumn columns[] = {
		{ "Person", AXIS_WEB_COL_NAME },
		{ "Role", AXIS_WEB_COL_TEXT },
		{ "Requests", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	PageWork *work = data;
	g_autoptr(HtmxDiv) content = NULL;
	g_autoptr(HtmxHeading) heading = NULL;
	g_autofree gchar *usual = NULL;
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	HtmxA *back;
	AxisWebNotice notice;
	guint i;

	if (work->error != NULL)
		return refusal(request, "Requests", work->error);
	content = htmx_div_new();
	heading = htmx_h1_new();
	htmx_node_set_text_content(HTMX_NODE(heading), "People who ask");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(heading));
	ADD(content, axis_web_page_intro("For each person: whether their requests are approved without waiting for "
		"you, and how many they may make. Both switches are off for a new account."));
	back = htmx_a_new_with_href("/requests");
	htmx_node_set_text_content(HTMX_NODE(back), "All requests");
	ADD(content, back);
	axis_web_notice_begin(&notice, request, FALSE,
		g_strdup(done_message(htmx_request_get_query_param(request, AXIS_WEB_PLACE_DONE))), HTMX_NODE(content));
	card = axis_web_storage_card("Automatic approval and limits");
	usual = g_strdup_printf("An empty limit is the installation's: %" G_GINT64_FORMAT " movies and %" G_GINT64_FORMAT
		" seasons in each window (Settings, Requests). 0 means the person cannot ask for that kind. An automatic "
		"approval uses the default folder and profile, and never more than the person's limit.", work->movies_limit,
		work->seasons_limit);
	ADD(card, text("help", usual));
	table = axis_web_table_new("request-people", columns);
	body = htmx_tbody_new();
	for (i = 0; i < work->people->len; i++) {
		const AxisRequestPerson *person = g_ptr_array_index(work->people, i);
		g_autofree gchar *row_id = g_strdup_printf("person-%" G_GINT64_FORMAT, person->id);
		g_autofree gchar *action = g_strdup_printf("/requests/people/%" G_GINT64_FORMAT, person->id);
		g_autofree gchar *movies_id = g_strdup_printf("%s-auto-movies", row_id);
		g_autofree gchar *series_id = g_strdup_printf("%s-auto-series", row_id);
		g_autofree gchar *movies_limit_id = g_strdup_printf("%s-movies-limit", row_id);
		g_autofree gchar *seasons_limit_id = g_strdup_printf("%s-seasons-limit", row_id);
		g_autofree gchar *movies_limit = person->movies_limit >= 0 ? g_strdup_printf("%d", person->movies_limit) : NULL;
		g_autofree gchar *seasons_limit = person->seasons_limit >= 0 ? g_strdup_printf("%d", person->seasons_limit) :
			NULL;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *first = htmx_td_new();
		HtmxTd *controls = htmx_td_new();

		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		htmx_node_set_text_content(HTMX_NODE(first), person->username);
		axis_web_notice_put(&notice, HTMX_NODE(first), row_id);
		ADD(row, first);
		ADD(row, cell(person->admin ? "Administrator" : person->disabled ? "User, disabled" : "User"));
		if (person->admin) {
			ADD(controls, text("help", "An administrator adds titles directly and is not limited."));
		} else {
			HtmxNode *form = axis_web_auth_post_form(request, action);

			htmx_element_add_class(HTMX_ELEMENT(form), "form");
			ADD(form, tick(movies_id, "auto_movies", "Approve their movie requests automatically",
				person->auto_movies));
			ADD(form, tick(series_id, "auto_series", "Approve their series requests automatically",
				person->auto_series));
			ADD(form, axis_web_storage_text_field(movies_limit_id, "Movie requests in each window", "movies_limit",
				movies_limit, "The installation's", NULL));
			ADD(form, axis_web_storage_text_field(seasons_limit_id, "Season requests in each window", "seasons_limit",
				seasons_limit, "The installation's", NULL));
			ADD(form, axis_web_auth_button("Save", "button-small"));
			ADD(controls, form);
		}
		ADD(row, controls);
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	ADD(content, card);
	axis_web_notice_end(&notice);
	axis_web_table_apply_roles(HTMX_NODE(content));
	return axis_web_page_respond(request, "People who ask", HTMX_NODE(content));
}

static HtmxResponse *
handle_people(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	PageWork *work = g_new0(PageWork, 1);

	work->admin = TRUE;
	return dispatch(user_data, request, people_run, people_done, work, page_work_free);
}

/* ── The notices ────────────────────────────────────────────────── */

/*
 * Requests that wait for an administrator: one notice with their number and
 * since when the oldest waits, at once (only a person ends it), gone when
 * none waits.  A request approved automatically never waited, so it was
 * never part of it.  One small statement over the partial index of the
 * waiting ones.
 */
static gboolean
waiting_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisRequests *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *since = NULL;
	g_autofree gchar *words = NULL;
	AxisAttentionItem *item;
	gint64 count = 0;
	gint64 oldest_us = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(self->pool, "SELECT count(*), " AXIS_REQUESTS_US("min(created_at)")
	        ", coalesce(max(id), 0) FROM request WHERE decision = 'waiting'", reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &count);
	axis_db_result_get_int64(result, 0, 1, &oldest_us);
	axis_db_result_get_int64(result, 0, 2, &newest);
	if (count <= 0)
		return TRUE;
	since = axis_attention_duration(MAX(reading->now_us - oldest_us, 0));
	if (count == 1)
		words = g_strdup_printf("1 request waits for approval, for %s.", since);
	else
		words = g_strdup_printf("%" G_GINT64_FORMAT " requests wait for approval, the oldest for %s.", count, since);
	item = axis_attention_item_new("requests.waiting", "", AXIS_ATTENTION_WAITING, (guint64)count, newest);
	axis_attention_item_say(item, words, NULL, NULL);
	axis_attention_item_link(item, "/requests", "Open Requests");
	item->since_us = oldest_us;
	g_ptr_array_add(items, item);
	return TRUE;
}

/* Approved requests whose title could not be added: an administrator tries
 * again on the Requests page. */
static gboolean
not_added_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisRequests *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *words = NULL;
	AxisAttentionItem *item;
	gint64 count = 0;
	gint64 newest = 0;

	if (!axis_db_pool_query_bounded(self->pool, "SELECT count(*), coalesce(max(id), 0) FROM request"
	        " WHERE decision = 'approved' AND add_error IS NOT NULL AND added_at IS NULL", reading->deadline_ms, &result,
	        error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &count);
	axis_db_result_get_int64(result, 0, 1, &newest);
	if (count <= 0)
		return TRUE;
	words = count == 1 ? g_strdup("1 approved request could not be added.") :
		g_strdup_printf("%" G_GINT64_FORMAT " approved requests could not be added.", count);
	item = axis_attention_item_new("requests.not_added", "", AXIS_ATTENTION_WAITING, (guint64)count, newest);
	axis_attention_item_say(item, words, NULL, NULL);
	axis_attention_item_link(item, "/requests", "Open Requests");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* ── An account that goes ───────────────────────────────────────── */

static void
account_gone(
	gint64   user_id,
	gpointer user_data
){
	AxisRequests *self = user_data;
	g_autoptr(GError) error = NULL;

	if (!axis_requests_account_gone(self, user_id, &error))
		g_message("requests: the waiting requests of an account that went could not be withdrawn: %s",
			error->message);
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_web_requests_register(
	AxisRequests *self,
	AxisWeb      *web
){
	AxisWebAuth *auth = axis_web_get_auth(web);

	/* A kind whose module is switched off cannot be asked for. */
	axis_requests_set_registry(self, axis_web_get_registry(web));
	if (auth != NULL)
		axis_web_auth_set_account_gone(auth, account_gone, self);
	axis_web_add_title_actions(web, actions_collect, actions_render, figures_free, self);
	axis_web_add_attention_source(web, "the requests", waiting_source, self);
	axis_web_add_attention_source(web, "the approved requests", not_added_source, self);

	axis_web_route_full(web, HTMX_METHOD_GET, "/requests", AXIS_WEB_ROUTE_DEFAULT, handle_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests", AXIS_WEB_ROUTE_DEFAULT, handle_ask, self);
	/* Before the routes with a request's identifier in that place: the first
	 * route that matches answers. */
	axis_web_route_full(web, HTMX_METHOD_GET, "/requests/people", AXIS_WEB_ROUTE_ADMIN, handle_people, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/people/:id", AXIS_WEB_ROUTE_ADMIN, handle_person, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/defaults", AXIS_WEB_ROUTE_ADMIN, handle_defaults, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/titles/:id/close", AXIS_WEB_ROUTE_ADMIN, handle_close, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/titles/:id/open", AXIS_WEB_ROUTE_ADMIN, handle_open, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/requests/:id", AXIS_WEB_ROUTE_DEFAULT, handle_one, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/:id/withdraw", AXIS_WEB_ROUTE_DEFAULT, handle_withdraw, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/:id/unfollow", AXIS_WEB_ROUTE_DEFAULT, handle_unfollow, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/:id/approve", AXIS_WEB_ROUTE_ADMIN, handle_approve, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/:id/decline", AXIS_WEB_ROUTE_ADMIN, handle_decline, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/requests/:id/retry", AXIS_WEB_ROUTE_ADMIN, handle_retry, self);
}
