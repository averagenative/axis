/*
 * axis-movies-web.c - the movies module's part of the web shell: its
 * routes, the work it hands to threads, its piece of every title page and
 * result card, its part of the dashboard, and the administrators' actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   POST /films                       add a film (title, profile, root, ...)
 *   POST /films/:id/search            search now
 *   POST /films/:id/grab              grab a listed release (candidate=)
 *   POST /films/:id/give-up           give up on the open grab (confirm=yes)
 *   POST /films/:id/blocklist/clear   one entry (entry=) or all
 *   GET, POST /films/block-release    Block and search again
 *                                     (web/axis-web-block.h)
 *   POST /films/:id/edit              profile, root, monitoring, availability
 *   POST /films/:id/remove            forget the film (confirm=yes)
 *   GET  /films/:id/state             a film's state block (htmx: polled while
 *                                     it searches or downloads)
 *   /films/:id/manual...              a manual search of the film
 *                                     (web/axis-web-manual.c)
 *
 * The pages (axis-movies-web-view.c) are GET /films, /films/wanted and
 * /films/history.  Every action is an administrator's, checked by the
 * route (AXIS_WEB_ROUTE_ADMIN) and the shell's CSRF check, and recorded in
 * the film's history with the user.
 */

#include "modules/movies/axis-movies-web.h"

#include <string.h>

#include "acquire/axis-acquire-ready.h"
#include "web/axis-user-store.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-back.h"
#include "web/axis-web-block.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-manual.h"
#include "web/axis-web-place.h"
#include "web/axis-web-ready.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* ── Work off the main loop ─────────────────────────────────────── */

typedef struct
{
	AxisMovies         *owner;
	AxisMoviesWorkFunc  work;
	AxisMoviesDoneFunc  done;
	HtmxRequest        *request;
	HtmxResponse       *deferred;
	gpointer            data;
	GDestroyNotify      free_data;
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
	if (task->deferred != NULL)
		htmx_response_complete(task->deferred, response);
	if (response != NULL)
		g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	axis_movies_unref(task->owner);
	g_free(task);
}

HtmxResponse *
axis_movies_web_dispatch(
	AxisMovies         *self,
	HtmxRequest        *request,
	AxisMoviesWorkFunc  work,
	AxisMoviesDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= AXIS_MOVIES_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = axis_movies_ref(self);
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

/* ── Shown films and choices ────────────────────────────────────── */

void
axis_movies_shown_free(AxisMoviesShown *shown)
{
	if (shown == NULL)
		return;
	axis_movie_facts_free(shown->facts);
	axis_movies_work_free(shown->work);
	g_free(shown->reason);
	g_free(shown->poster);
	g_free(shown);
}

AxisMoviesShown *
axis_movies_shown_new(
	AxisMovieFacts       *facts,
	const AxisMovieRules *rules
){
	AxisMoviesShown *shown = g_new0(AxisMoviesShown, 1);

	shown->facts = facts;
	shown->state = axis_movie_derive_attention(facts, rules, &shown->reason, &shown->kind);
	return shown;
}

void
axis_movies_choice_free(AxisMoviesChoice *choice)
{
	if (choice == NULL)
		return;
	g_free(choice->label);
	g_free(choice->slug);
	g_free(choice);
}

void
axis_movies_choices_clear(AxisMoviesChoices *choices)
{
	g_clear_pointer(&choices->profiles, g_ptr_array_unref);
	g_clear_pointer(&choices->roots, g_ptr_array_unref);
	g_clear_pointer(&choices->default_slug, g_free);
}

/* Online first, then the most free space. */
static gint
compare_roots(
	gconstpointer a,
	gconstpointer b
){
	const AxisMoviesChoice *x = *(const AxisMoviesChoice *const *)a;
	const AxisMoviesChoice *y = *(const AxisMoviesChoice *const *)b;

	if (x->usable != y->usable)
		return x->usable ? -1 : 1;
	if (x->free_bytes != y->free_bytes)
		return x->free_bytes > y->free_bytes ? -1 : 1;
	return g_strcmp0(x->label, y->label);
}

gboolean
axis_movies_web_read_choices(
	AxisMovies        *self,
	AxisMoviesChoices *choices,
	GError           **error
){
	g_autoptr(GPtrArray) profiles = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	g_autofree gchar *availability = NULL;
	guint i;

	memset(choices, 0, sizeof(*choices));
	if (!axis_quality_store_list(self->quality, &profiles, error) ||
	    !axis_storage_list_roots(self->storage, 0, &roots, error))
		return FALSE;
	choices->default_slug = axis_quality_store_default_slug(self->quality);
	availability = axis_movies_setting_string(self, AXIS_MOVIES_SETTING_AVAILABILITY, "released");
	choices->availability = AXIS_MOVIE_RELEASED;
	axis_movie_availability_from_name(availability, &choices->availability);
	choices->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movies_choice_free);
	for (i = 0; i < profiles->len; i++) {
		const AxisQualityStored *stored = g_ptr_array_index(profiles, i);
		AxisMoviesChoice *choice = g_new0(AxisMoviesChoice, 1);

		choice->id = stored->id;
		choice->label = g_strdup(stored->name);
		choice->slug = g_strdup(stored->slug);
		choice->usable = stored->usable;
		g_ptr_array_add(choices->profiles, choice);
	}
	choices->roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movies_choice_free);
	for (i = 0; i < roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(roots, i);
		g_autoptr(AxisVolumeInfo) volume = NULL;
		AxisMoviesChoice *choice;

		if (root->media_type != AXIS_MEDIA_MOVIES)
			continue;
		volume = axis_storage_get_volume(self->storage, root->volume_id);
		choice = g_new0(AxisMoviesChoice, 1);
		choice->id = root->id;
		choice->usable = volume != NULL && axis_volume_state_is_usable(volume->state);
		/* A new movie goes only where it can be imported (the server checks
		 * again on Add). */
		choice->why_not = volume != NULL ? axis_volume_state_unwritable_why(volume->state) : "its disk is not known";
		choice->free_bytes = volume != NULL && volume->has_space ? volume->available_bytes : 0;
		if (volume != NULL && volume->has_space && choice->usable) {
			g_autofree gchar *free_text = axis_size_format(volume->available_bytes);

			choice->label = g_strdup_printf("%s on %s (%s free)", root->name, volume->name, free_text);
		} else {
			choice->label = g_strdup_printf("%s on %s (%s)", root->name, volume != NULL ? volume->name : "a disk",
				choice->usable ? "online" : "offline");
		}
		g_ptr_array_add(choices->roots, choice);
	}
	g_ptr_array_sort(choices->roots, compare_roots);
	return TRUE;
}

/* ── The title page and the result cards ────────────────────────── */

typedef struct
{
	AxisWebActionPlace  place;
	/* A title's page. */
	gboolean            is_film;
	gint64              title_id;
	AxisMoviesShown    *shown;	/* (nullable): not managed */
	AxisMovieDetail    *detail;
	AxisMoviesChoices   choices;
	gboolean            choices_read;
	AxisReadiness       ready;	/* what is needed to download anything */
	gboolean            ready_read;
	AxisMoviesTried     tried;	/* the profiles tried on what was rejected */
	/* The page's notice: begun with the panel, drawn in the card the post
	 * came back to (the panel's or one of the detail's). */
	AxisWebNotice       notice;
	gboolean            notice_open;
	/* Result cards: title id -> AxisMoviesShown. */
	GHashTable         *by_title;
} ActionFigures;

static void
action_figures_free(gpointer data)
{
	ActionFigures *figures = data;

	if (figures == NULL)
		return;
	/* (A notice never drawn: its slot went with the page.) */
	if (figures->notice_open)
		g_free(figures->notice.text);
	/* The candidates tried are the detail's. */
	axis_movies_tried_clear(&figures->tried);
	axis_movies_shown_free(figures->shown);
	axis_movie_detail_free(figures->detail);
	axis_movies_choices_clear(&figures->choices);
	axis_readiness_clear(&figures->ready);
	g_clear_pointer(&figures->by_title, g_hash_table_unref);
	g_free(figures);
}

static gpointer
actions_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	AxisMovies *self = user_data;
	ActionFigures *figures = g_new0(ActionFigures, 1);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;
	guint i;

	figures->place = place;
	axis_movies_get_rules(self, &rules);
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		if (title->kind == AXIS_META_KIND_FILM && title->id > 0)
			g_array_append_val(ids, title->id);
	}
	if (ids->len == 0)
		return figures;
	if (!axis_movies_get_by_titles(self, ids, &films, &error)) {
		g_message("films: the title page's films could not be read: %s", error->message);
		action_figures_free(figures);
		return NULL;
	}
	if (place != AXIS_WEB_ACTION_PAGE) {
		g_autoptr(GArray) others = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(GPtrArray) on_disk = NULL;

		figures->by_title = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
			(GDestroyNotify)axis_movies_shown_free);
		while (films->len > 0) {
			AxisMovieFacts *facts = g_ptr_array_steal_index(films, 0);
			gint64 *key = g_new(gint64, 1);

			*key = facts->title_id;
			g_hash_table_insert(figures->by_title, key, axis_movies_shown_new(facts, &rules));
		}
		/* The others that are on disk carry their mark too: one statement. */
		for (i = 0; i < ids->len; i++) {
			if (!g_hash_table_contains(figures->by_title, &g_array_index(ids, gint64, i)))
				g_array_append_val(others, g_array_index(ids, gint64, i));
		}
		if (others->len > 0 && !axis_movies_get_library_by_titles(self, others, &on_disk, &error)) {
			g_message("films: the movies on disk among the titles could not be read: %s", error->message);
			g_clear_error(&error);
		}
		while (on_disk != NULL && on_disk->len > 0) {
			AxisMovieFacts *facts = g_ptr_array_steal_index(on_disk, 0);
			gint64 *key = g_new(gint64, 1);

			*key = facts->title_id;
			g_hash_table_insert(figures->by_title, key, axis_movies_shown_new(facts, &rules));
		}
		/* A Discover card, or a search result, adds in one step: with the add
		 * form's defaults. */
		if (place == AXIS_WEB_ACTION_DISCOVER || place == AXIS_WEB_ACTION_CARD) {
			figures->choices_read = axis_movies_web_read_choices(self, &figures->choices, &error);
			if (!figures->choices_read) {
				g_message("films: the profiles and roots could not be read: %s", error->message);
				g_clear_error(&error);
			}
		}
		return figures;
	}
	figures->is_film = TRUE;
	figures->title_id = g_array_index(ids, gint64, 0);
	if (films->len > 0) {
		figures->shown = axis_movies_shown_new(g_ptr_array_steal_index(films, 0), &rules);
		if (!axis_movies_get_detail(self, figures->shown->facts->id, AXIS_MOVIES_WEB_EVENTS_SHOWN, &figures->detail,
		        &error)) {
			g_message("films: a film's page could not be read: %s", error->message);
			g_clear_error(&error);
		}
		/* Its searches in hand and lately ended, for its line and buttons. */
		if (!axis_movies_get_work(self, figures->shown->facts->id, AXIS_WEB_ACTIVITY_OUTCOME_S,
		        &figures->shown->work, &error)) {
			g_message("films: a film's searches in hand could not be read: %s", error->message);
			g_clear_error(&error);
		}
	} else if (axis_movies_get_library_detail(self, figures->title_id, AXIS_MOVIES_WEB_EVENTS_SHOWN,
	               &figures->detail, &error)) {
		/* On disk and not managed: shown from what is there. */
		figures->shown = axis_movies_shown_new(g_steal_pointer(&figures->detail->facts), &rules);
	} else {
		if (!g_error_matches(error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND))
			g_message("films: a movie on disk could not be read: %s", error->message);
		g_clear_error(&error);
	}
	figures->choices_read = axis_movies_web_read_choices(self, &figures->choices, &error);
	if (!figures->choices_read) {
		g_message("films: the profiles and roots could not be read: %s", error->message);
		g_clear_error(&error);
	}
	/* Every release rejected: which profile would take them, by the decision
	 * a search makes. */
	if (figures->choices_read)
		axis_movies_web_try(self, figures->shown, figures->detail, &figures->choices, &figures->tried);
	figures->ready_read = axis_acquire_check_ready(self->meta, self->indexers, self->downloads, self->storage,
		AXIS_READY_FILMS, &figures->ready, &error);
	if (!figures->ready_read)
		g_message("films: what is needed to download could not be read: %s", error->message);
	return figures;
}

/* A film that would be searched for, or whose download would be imported:
 * what stands in the way of it is said at the top of its page.  (A movie on
 * disk Axis does not manage is in the library.) */
static gboolean
film_is_wanted(AxisMovieState state)
{
	return state != AXIS_MOVIE_IN_LIBRARY;
}

static HtmxNode *
actions_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             data,
	gpointer             user_data
){
	ActionFigures *figures = data;
	gboolean admin = axis_web_request_is_admin(request);

	if (figures == NULL || title->kind != AXIS_META_KIND_FILM)
		return NULL;
	/* Above the list: where the person's Adds go (administrators). */
	if (place == AXIS_WEB_ACTION_BAR)
		return admin && figures->choices_read ? axis_movies_web_defaults_bar(request, &figures->choices, NULL) : NULL;
	if (place == AXIS_WEB_ACTION_CARD || place == AXIS_WEB_ACTION_DISCOVER) {
		AxisMoviesShown *shown = figures->by_title != NULL ? g_hash_table_lookup(figures->by_title, &title->id) : NULL;

		if (shown != NULL)
			return axis_movies_web_mark(shown->state);
		if (!admin || !figures->choices_read || title->id <= 0)
			return NULL;
		return axis_movies_web_quick_add(request, title->id, &figures->choices);
	}
	if (!figures->is_film)
		return NULL;
	if (place == AXIS_WEB_ACTION_NOTICE) {
		if (figures->shown == NULL || !figures->ready_read || !film_is_wanted(figures->shown->state))
			return NULL;
		/* Downloading or arrived: only the import can still be in the way. */
		if (figures->shown->state == AXIS_MOVIE_DOWNLOADING || figures->shown->state == AXIS_MOVIE_TO_IMPORT) {
			AxisReadiness import_only = figures->ready;

			import_only.checks[AXIS_READY_INDEXER].done = TRUE;
			import_only.checks[AXIS_READY_CLIENT].done = TRUE;
			import_only.checks[AXIS_READY_FILMS_ROOT].done = TRUE;
			return axis_web_ready_notice_new(&import_only, AXIS_READY_FILMS, "movie", admin);
		}
		return axis_web_ready_notice_new(&figures->ready, AXIS_READY_FILMS, "movie", admin);
	}
	if (place == AXIS_WEB_ACTION_DETAIL) {
		HtmxNode *detail;

		if (figures->shown == NULL || figures->detail == NULL)
			return NULL;
		/* The notice of a post that came back to one of these cards. */
		detail = axis_movies_web_detail(request, figures->shown, figures->detail,
			figures->choices_read ? &figures->choices : NULL, figures->notice_open ? &figures->notice : NULL);
		if (figures->notice_open)
			axis_web_notice_end(&figures->notice);
		figures->notice_open = FALSE;
		return detail;
	}
	{
		HtmxDiv *piece = htmx_div_new();
		const gchar *done_word = htmx_request_get_query_param(request, "done");
		const gchar *at = axis_web_place_of(request);
		gchar *done = NULL;

		/* What was done at another title's card on this page (More like
		 * this) is said there, by the part that draws the card. */
		if (at != NULL && g_str_has_prefix(at, "title-"))
			done_word = NULL;
		done = axis_movies_web_answer(done_word, figures->shown);
		if (done == NULL)
			done = axis_movies_web_done_text(done_word, figures->shown != NULL ? figures->shown->facts : NULL);
		/* The film's panel: where its own actions come back to. */
		htmx_element_set_id(HTMX_ELEMENT(piece), "film");
		htmx_element_add_class(HTMX_ELEMENT(piece), "film-panel");
		axis_web_notice_begin(&figures->notice, request, FALSE, done, HTMX_NODE(piece));
		figures->notice_open = TRUE;
		axis_web_notice_put(&figures->notice, HTMX_NODE(piece), "film");
		/* Unless it came back to a card of the detail, it is drawn here. */
		if ((g_strcmp0(at, "settings") != 0 && g_strcmp0(at, "releases") != 0 && g_strcmp0(at, "blocklist") != 0) ||
		    figures->detail == NULL || figures->shown == NULL) {
			axis_web_notice_end(&figures->notice);
			figures->notice_open = FALSE;
		}
		/* Its line and buttons follow its searches and downloads. */
		if (figures->shown != NULL && !figures->shown->facts->library_only) {
			gchar *topics[3];
			guint t;

			axis_movies_web_topics(figures->shown->facts, topics);
			for (t = 0; topics[t] != NULL; t++) {
				axis_web_page_add_topic(request, topics[t]);
				g_free(topics[t]);
			}
		}
		if (figures->shown != NULL && figures->shown->facts->library_only) {
			/* On disk, not managed: what it is, and how to have Axis manage it. */
			ADD(piece, axis_movies_web_state_block(figures->shown));
			if (admin && figures->choices_read)
				ADD(piece, axis_movies_web_manage_form(request, figures->shown->facts, &figures->choices));
			else
				ADD(piece, axis_web_storage_text("help", admin ? "Axis does not manage it: the quality profiles and library folders "
					"could not be read, so it cannot be managed now." : "Axis does not manage it, so nothing is "
					"searched for or changed. An administrator can have Axis manage it."));
		} else if (figures->shown != NULL) {
			HtmxNode *resolve;

			ADD(piece, axis_movies_web_state_block(figures->shown));
			if (admin)
				ADD(piece, axis_movies_web_actions(request, figures->shown));
			/* Next to what is wrong, what puts it right. */
			resolve = axis_movies_web_resolve(request, figures->shown, figures->detail,
				figures->choices_read ? &figures->choices : NULL, &figures->tried);
			if (resolve != NULL)
				ADD(piece, resolve);
		} else if (admin && figures->choices_read) {
			HtmxNode *note = figures->ready_read ? axis_web_ready_note_new(&figures->ready, AXIS_READY_FILMS, "movie") :
				NULL;

			ADD(piece, axis_movies_web_add_form(request, title->id, &figures->choices));
			/* Adding is still possible, but not surprising. */
			if (note != NULL)
				ADD(piece, note);
		} else {
			ADD(piece, axis_web_storage_text("help", admin ? "Not among your movies: the quality profiles and library folders could not "
				"be read, so it cannot be added now." : "Not among your movies. An administrator can add it."));
		}
		return HTMX_NODE(piece);
	}
}

/* ── The dashboard ──────────────────────────────────────────────── */

typedef struct
{
	guint      counts[AXIS_MOVIE_N_STATES];
	guint      kinds[AXIS_ACQUIRE_ATTENTION_FAILED + 1];	/* of the films on the first page */
	guint      total;
	GPtrArray *recent;	/* AxisMoviesShown, the latest added first */
	gint64     taken_us;
} DashFigures;

static void
dash_free(gpointer data)
{
	DashFigures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->recent, g_ptr_array_unref);
	g_free(figures);
}

/* The counts by state (of the whole library, the movies on disk Axis does
 * not manage among those in it) and the five films added last (added by a
 * person: managed ones alone): the list's first page of five, so the
 * dashboard reads no more films' facts than it shows. */
static gpointer
dash_collect(gpointer user_data)
{
	AxisMovies *self = user_data;
	AxisMoviesListPage page;
	AxisMoviesListQuery query;
	g_autoptr(GError) error = NULL;
	DashFigures *figures;
	AxisMovieRules rules;
	guint i;

	memset(&query, 0, sizeof(query));
	query.sort = AXIS_MOVIES_SORT_ADDED;
	query.limit = 5;
	query.managed_only = TRUE;
	if (!axis_movies_list_page(self, &query, &page, &error)) {
		g_debug("dashboard: cannot read the films: %s", error->message);
		return NULL;
	}
	axis_movies_get_rules(self, &rules);
	figures = g_new0(DashFigures, 1);
	figures->taken_us = rules.now_us;
	figures->recent = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movies_shown_free);
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++) {
		figures->counts[i] = page.counts[i];
		figures->total += page.counts[i];
	}
	while (page.films->len > 0)
		g_ptr_array_add(figures->recent, axis_movies_shown_new(g_ptr_array_steal_index(page.films, 0), &rules));
	axis_movies_list_page_clear(&page);
	/* Why the films that need attention do, from a page of them: the most
	 * common reason is said beside the count. */
	if (figures->counts[AXIS_MOVIE_ATTENTION] > 0) {
		AxisMoviesListPage attention;

		memset(&query, 0, sizeof(query));
		query.states = 1u << AXIS_MOVIE_ATTENTION;
		query.sort = AXIS_MOVIES_SORT_ADDED;
		query.limit = 50;
		query.managed_only = TRUE;
		if (axis_movies_list_page(self, &query, &attention, &error)) {
			for (i = 0; i < attention.films->len; i++) {
				AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;

				axis_movie_derive_attention(g_ptr_array_index(attention.films, i), &rules, NULL, &kind);
				if ((guint)kind < G_N_ELEMENTS(figures->kinds))
					figures->kinds[kind]++;
			}
			axis_movies_list_page_clear(&attention);
		} else {
			g_debug("dashboard: cannot read the films that need attention: %s", error->message);
		}
	}
	return figures;
}

static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	DashFigures *figures = data;
	HtmxDiv *piece;
	HtmxP *head;
	HtmxUl *list;
	g_autofree gchar *summary = NULL;
	guint i;

	if (figures == NULL || figures->total == 0)
		return;
	piece = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	/* The films added by a person; a library of movies found on disk alone
	 * has its counts and no list. */
	if (figures->recent->len > 0) {
		head = htmx_p_new();
		htmx_element_add_class(HTMX_ELEMENT(head), "dash-subhead");
		htmx_node_set_text_content(HTMX_NODE(head), "Movies added lately");
		ADD(piece, head);
	}
	list = htmx_ul_new();
	htmx_element_add_class(HTMX_ELEMENT(list), "space-list");
	for (i = 0; i < figures->recent->len; i++) {
		const AxisMoviesShown *shown = g_ptr_array_index(figures->recent, i);
		g_autofree gchar *href = g_strdup_printf("/title/%" G_GINT64_FORMAT, shown->facts->title_id);
		g_autofree gchar *label = axis_movies_title_label(shown->facts->title, shown->facts->year);
		HtmxLi *item = htmx_li_new();
		HtmxSpan *name = htmx_span_new();
		HtmxA *link = htmx_a_new_with_href(href);
		HtmxBdi *text = htmx_bdi_new();
		HtmxSpan *state = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(item), "space-item");
		htmx_element_add_class(HTMX_ELEMENT(name), "space-name");
		htmx_node_set_text_content(HTMX_NODE(text), label);
		ADD(link, text);
		ADD(name, link);
		ADD(item, name);
		htmx_element_add_class(HTMX_ELEMENT(state), "space-figures");
		htmx_node_set_text_content(HTMX_NODE(state), axis_movie_state_label(shown->state));
		ADD(item, state);
		ADD(list, item);
	}
	if (figures->recent->len > 0)
		ADD(piece, list);
	else
		g_object_unref(list);
	{
		g_autofree gchar *total = axis_web_count(figures->total, "movie", "movies");
		g_autofree gchar *in_library = axis_web_format_count(figures->counts[AXIS_MOVIE_IN_LIBRARY] +
			figures->counts[AXIS_MOVIE_UPGRADE_WANTED]);

		/* The whole library, the movies on disk Axis does not manage too. */
		summary = g_strdup_printf("%s: %s in the library, %u wanted, %u downloading, %u to import.", total, in_library,
			figures->counts[AXIS_MOVIE_WANTED] + figures->counts[AXIS_MOVIE_SEARCHING],
			figures->counts[AXIS_MOVIE_DOWNLOADING], figures->counts[AXIS_MOVIE_TO_IMPORT]);
	}
	ADD(piece, axis_web_storage_text("card-foot", summary));
	ADD(piece, axis_web_dashboard_as_of("As of", figures->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_COMING_IN, 20, "/films", HTMX_NODE(piece));
	/* The movies that need attention are the attention source's to say
	 * (attention_source() below): the dashboard's card is drawn from it. */
}

/* ── The administrators' actions ────────────────────────────────── */

typedef enum
{
	ACT_ADD,
	ACT_SEARCH,
	ACT_GRAB,
	ACT_GIVE_UP,
	ACT_CLEAR,
	ACT_EDIT,
	ACT_REMOVE,
	ACT_START	/* "Let Axis search for it": ends a brought movie's hold */
} Act;

typedef struct
{
	Act            act;
	gint64         movie_id;
	gint64         title_id;
	gint64         arg;
	AxisMovieSpec  spec;
	gchar         *user;
	gchar         *back;	/* (nullable) the page the add came from */
	gchar         *at;	/* (nullable) the place it came from (axis-web-place.h) */
	gboolean       created;
	AxisMoviesAsked asked;	/* a search: what came of it */
	guint          count;
	AxisMovieEdited edited;
	GError        *error;
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
	AxisMovies *self,
	gpointer    data
){
	ActWork *work = data;
	g_autoptr(AxisMovieFacts) facts = NULL;

	work->spec.user = work->user;
	if (work->act == ACT_ADD) {
		work->title_id = work->spec.title_id;
		/* A movie that was on disk is managed with its file: it says so. */
		if (axis_movies_add(self, &work->spec, &work->movie_id, &work->error) &&
		    axis_movies_get(self, work->movie_id, &facts, NULL))
			work->count = facts->n_files;
		return;
	}
	if (!axis_movies_get(self, work->movie_id, &facts, &work->error))
		return;
	work->title_id = facts->title_id;
	switch (work->act) {
	case ACT_SEARCH:
		/* A person's: nothing new while one is in hand or just ended. */
		axis_movies_ask_search(self, work->movie_id, FALSE, work->user, &work->asked, &work->error);
		break;
	case ACT_GRAB:
		axis_movies_grab(self, work->movie_id, work->arg, work->user, NULL, &work->error);
		break;
	case ACT_GIVE_UP:
		axis_movies_give_up(self, work->movie_id, work->user, &work->error);
		break;
	case ACT_CLEAR:
		axis_movies_clear_blocklist(self, work->movie_id, work->arg, work->user, &work->count, &work->error);
		break;
	case ACT_EDIT:
		axis_movies_edit_full(self, work->movie_id, &work->spec, &work->edited, &work->error);
		break;
	case ACT_START:
		axis_movies_start_searching(self, work->movie_id, work->user, &work->created, &work->error);
		break;
	default:
		/* Removed with its file on disk and recognised, it stays listed, not
		 * managed: its page says so. */
		if (axis_movies_remove(self, work->movie_id, work->user, &work->error)) {
			g_autoptr(GArray) titles = g_array_new(FALSE, FALSE, sizeof(gint64));
			g_autoptr(GPtrArray) on_disk = NULL;

			g_array_append_val(titles, work->title_id);
			if (axis_movies_get_library_by_titles(self, titles, &on_disk, NULL))
				work->count = on_disk->len;
		}
		break;
	}
}

static const gchar *
act_heading(Act act)
{
	static const gchar *const headings[] = {
		"Not added", "Not searched", "Not grabbed", "Not given up", "Not cleared", "Not saved", "Not removed",
		"Not started"
	};

	return (guint)act < G_N_ELEMENTS(headings) ? headings[act] : "Not done";
}

static const gchar *
act_name(Act act)
{
	static const gchar *const names[] = {
		"added", "search asked for", "grab asked for", "given up", "blocklist cleared", "changed", "removed",
		"searching started"
	};

	return (guint)act < G_N_ELEMENTS(names) ? names[act] : "acted on";
}

static HtmxResponse *
act_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	ActWork *work = data;
	g_autofree gchar *message = NULL;
	g_autofree gchar *location = NULL;
	const gchar *place = "film";
	const gchar *key;

	if (work->error != NULL) {
		guint status;

		if (work->error->domain != AXIS_MOVIES_ERROR)
			return axis_web_internal_error(request, work->error);
		switch (work->error->code) {
		case AXIS_MOVIES_ERROR_NOT_FOUND:
			status = 404;
			break;
		case AXIS_MOVIES_ERROR_INVALID:
			status = 400;
			break;
		case AXIS_MOVIES_ERROR_FAILED:
			status = 502;
			break;
		default:
			status = 409;
			break;
		}
		message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]), work->error->message + 1);
		return axis_web_error_respond(request, status, act_heading(work->act), message);
	}
	axis_web_storage_audit(request, "film %" G_GINT64_FORMAT " %s", work->movie_id, act_name(work->act));
	switch (work->act) {
	case ACT_ADD:
		key = work->count > 0 ? "managed" : "added";
		/* Added from another page (a Discover card): back to it, at the card. */
		if (work->back != NULL)
			return axis_web_place_redirect(request, work->back, key, work->at);
		break;
	case ACT_SEARCH:
		key = work->asked == AXIS_MOVIES_ASKED_QUEUED ? "searching" : work->asked == AXIS_MOVIES_ASKED_RECENT ?
			"searched_recently" : "already_searching";
		break;
	case ACT_GRAB:
		/* Back at the card its button was on (the releases, what resolves it). */
		key = "grabbed";
		place = work->at != NULL ? work->at : "releases";
		break;
	case ACT_GIVE_UP:
		key = "given_up";
		break;
	case ACT_CLEAR:
		key = "cleared";
		place = work->at != NULL ? work->at : "blocklist";
		break;
	case ACT_EDIT:
		/* A new profile searches again: the notice says so, with the name. */
		key = !work->edited.profile_changed ? "saved" : work->edited.searched ? "profile" : !work->edited.monitored ?
			"profile_unmonitored" : "profile_none";
		place = work->at != NULL ? work->at : "settings";
		break;
	case ACT_START:
		key = work->created ? "started" : "not_held";
		break;
	default:
		/* Still on disk: its page, where it is listed as not managed now. */
		if (work->count > 0) {
			key = "unmanaged";
			break;
		}
		return axis_web_auth_redirect(request, "/films?done=removed");
	}
	location = g_strdup_printf("/title/%" G_GINT64_FORMAT, work->title_id);
	return axis_web_place_redirect(request, location, key, place);
}

/* A whole number from the form (1 to 18 digits); 0 when missing or not one. */
static gint64
form_id(
	HtmxRequest *request,
	const gchar *name
){
	guint64 value = 0;

	if (!axis_web_storage_form_u64(request, name, &value) || value == 0 || value > (guint64)G_MAXINT64)
		return 0;
	return (gint64)value;
}

/* The spec's profile, root, monitoring and availability from the form. */
static gboolean
spec_from_form(
	HtmxRequest   *request,
	AxisMovieSpec *spec
){
	const gchar *availability = htmx_request_get_form_value(request, "availability");

	spec->profile_id = form_id(request, "profile");
	spec->root_id = form_id(request, "root");
	spec->monitored = axis_web_storage_form_yes(request, "monitored");
	spec->search_now = axis_web_storage_form_yes(request, "search");
	return availability != NULL && axis_movie_availability_from_name(availability, &spec->availability);
}

static HtmxResponse *
start_act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	Act          act
){
	AxisMovies *self = user_data;
	const gchar *user = axis_web_request_get_username(request);
	ActWork *work = g_new0(ActWork, 1);

	work->act = act;
	work->user = g_strdup(user);
	{
		const gchar *at = htmx_request_get_form_value(request, AXIS_WEB_PLACE_AT);

		work->at = axis_web_place_id_valid(at) ? g_strdup(at) : NULL;
	}
	if (act != ACT_ADD) {
		work->movie_id = axis_web_storage_param_id(params);
		if (work->movie_id <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
		}
	}
	switch (act) {
	case ACT_ADD:
		work->spec.title_id = form_id(request, "title");
		if (work->spec.title_id <= 0 || !spec_from_form(request, &work->spec)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not added", "The form is not complete.");
		}
		work->back = axis_web_back_posted(request);
		break;
	case ACT_EDIT:
		if (!spec_from_form(request, &work->spec)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not saved", "The form is not complete.");
		}
		break;
	case ACT_GRAB:
		work->arg = form_id(request, "candidate");
		if (work->arg <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not grabbed", "Choose a release.");
		}
		break;
	case ACT_CLEAR:
		work->arg = form_id(request, "entry");
		break;
	case ACT_GIVE_UP:
	case ACT_REMOVE:
		if (!axis_web_storage_form_yes(request, "confirm")) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, act_heading(act), "Tick the box to confirm.");
		}
		break;
	default:
		break;
	}
	return axis_movies_web_dispatch(self, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_ADD);
}

static HtmxResponse *
handle_search(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_SEARCH);
}

static HtmxResponse *
handle_grab(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_GRAB);
}

static HtmxResponse *
handle_give_up(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_GIVE_UP);
}

static HtmxResponse *
handle_clear(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_CLEAR);
}

static HtmxResponse *
handle_edit(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_EDIT);
}

static HtmxResponse *
handle_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_REMOVE);
}

static HtmxResponse *
handle_start(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return start_act(request, params, user_data, ACT_START);
}

/* ── A film's state, for htmx ───────────────────────────────────── */

typedef struct
{
	gint64           movie_id;
	AxisMoviesShown *shown;
	GError          *error;
} StateWork;

static void
state_work_free(gpointer data)
{
	StateWork *work = data;

	axis_movies_shown_free(work->shown);
	g_clear_error(&work->error);
	g_free(work);
}

static void
state_run(
	AxisMovies *self,
	gpointer    data
){
	StateWork *work = data;
	AxisMovieFacts *facts = NULL;
	AxisMovieRules rules;

	axis_movies_get_rules(self, &rules);
	if (axis_movies_get(self, work->movie_id, &facts, &work->error)) {
		work->shown = axis_movies_shown_new(facts, &rules);
		axis_movies_get_work(self, work->movie_id, AXIS_WEB_ACTIVITY_OUTCOME_S, &work->shown->work, &work->error);
	}
}

/* The block, and for an administrator its buttons out of band: what is in
 * hand changes them both. */
static HtmxResponse *
state_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	StateWork *work = data;
	g_autoptr(HtmxFragment) fragment = htmx_fragment_new();

	if (g_error_matches(work->error, AXIS_MOVIES_ERROR, AXIS_MOVIES_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(fragment, axis_movies_web_state_block(work->shown));
	if (axis_web_request_is_admin(request) && !work->shown->facts->library_only)
		axis_web_activity_add_oob(HTMX_NODE(fragment), axis_movies_web_actions(request, work->shown));
	return axis_web_activity_respond(HTMX_NODE(fragment));
}

static HtmxResponse *
handle_state(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	StateWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	work = g_new0(StateWork, 1);
	work->movie_id = id;
	return axis_movies_web_dispatch(user_data, request, state_run, state_done, work, state_work_free);
}

/* ── Where the person's Adds from a list go ─────────────────────── */

typedef struct
{
	gint64             user_id;
	gint64             root_id;
	gint64             profile_id;
	AxisMoviesChoices  choices;
	gchar             *all;
	const gchar       *refused;
	GError            *error;
} DefaultsWork;

static void
defaults_work_free(gpointer data)
{
	DefaultsWork *work = data;

	axis_movies_choices_clear(&work->choices);
	g_free(work->all);
	g_clear_error(&work->error);
	g_free(work);
}

/* Whether @id is a choice of @list that can be used. */
static gboolean
can_choose(
	const GPtrArray *list,
	gint64           id
){
	guint i;

	for (i = 0; list != NULL && i < list->len; i++) {
		const AxisMoviesChoice *c = g_ptr_array_index(list, i);

		/* (A root: on a disk a new movie can be imported into now.) */
		if (c->id == id)
			return c->usable && c->why_not == NULL;
	}
	return FALSE;
}

static void
defaults_run(
	AxisMovies *self,
	gpointer    data
){
	DefaultsWork *work = data;
	g_autofree gchar *values = NULL;

	if (!axis_movies_web_read_choices(self, &work->choices, &work->error))
		return;
	/* Only a movie folder on a disk Axis can write to now, and a profile
	 * that can be used: as the cards' own Options offer them. */
	if (!can_choose(work->choices.roots, work->root_id)) {
		work->refused = "That folder cannot be chosen: it is not a movie folder, or its disk is away or mounted "
			"read-only now.";
		return;
	}
	if (!can_choose(work->choices.profiles, work->profile_id)) {
		work->refused = "That quality profile cannot be chosen.";
		return;
	}
	values = g_strdup_printf("{\"root\": %" G_GINT64_FORMAT ", \"profile\": %" G_GINT64_FORMAT "}", work->root_id,
		work->profile_id);
	axis_user_store_set_add_defaults(self->pool, work->user_id, AXIS_MOVIES_WEB_DEFAULTS_KIND, values, &work->all,
		&work->error);
}

static HtmxResponse *
defaults_done(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
){
	DefaultsWork *work = data;
	g_autofree gchar *back = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->refused != NULL)
		return axis_web_error_respond(request, 409, "Not chosen", work->refused);
	axis_web_request_note_add_defaults(request, work->all);
	axis_web_storage_audit(request, "movies added from a list go to root %" G_GINT64_FORMAT " with profile %"
		G_GINT64_FORMAT, work->root_id, work->profile_id);
	/* With htmx: the bar and every card's Add, out of band; the page stays
	 * where it is.  Without: back to the list, at the bar. */
	if (htmx_request_is_htmx(request))
		return axis_movies_web_defaults_answer(request, &work->choices);
	back = axis_web_back_posted(request);
	return axis_web_place_redirect(request, back != NULL ? back : "/discover/movies", "add_defaults",
		AXIS_MOVIES_WEB_DEFAULTS_BAR);
}

static HtmxResponse *
handle_defaults(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	DefaultsWork *work = g_new0(DefaultsWork, 1);
	guint64 value = 0;

	work->user_id = axis_web_request_get_user_id(request);
	if (axis_web_storage_form_u64(request, "root", &value) && value <= (guint64)G_MAXINT64)
		work->root_id = (gint64)value;
	value = 0;
	if (axis_web_storage_form_u64(request, "profile", &value) && value <= (guint64)G_MAXINT64)
		work->profile_id = (gint64)value;
	return axis_movies_web_dispatch(user_data, request, defaults_run, defaults_done, work, defaults_work_free);
}

/* ── The Activity page's words ──────────────────────────────────── */

static gint64
payload_movie(JsonObject *payload)
{
	JsonNode *node = payload != NULL ? json_object_get_member(payload, "movie") : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

/* "Searching for Paper Lantern (2019)": the films read at once. */
static void
activity_words(
	GPtrArray *items,
	gpointer   user_data
){
	AxisMovies *self = user_data;
	g_autoptr(GString) ids = g_string_new("{");
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[1];
	guint i;
	guint r;

	for (i = 0; i < items->len; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			payload_movie(((AxisWebActivityItem *)g_ptr_array_index(items, i))->payload));
	g_string_append_c(ids, '}');
	params[0] = ids->str;
	if (!axis_db_pool_exec(self->pool, "SELECT m.id, t.title, t.year, t.id FROM movie m JOIN meta_title t ON t.id ="
	        " m.title_id WHERE m.id = ANY ($1::bigint[])", 1, params, &rows, NULL))
		return;
	for (i = 0; i < items->len; i++) {
		AxisWebActivityItem *item = g_ptr_array_index(items, i);
		gint64 movie = payload_movie(item->payload);
		JsonNode *cause = item->payload != NULL ? json_object_get_member(item->payload, "cause") : NULL;
		gboolean list = cause != NULL && JSON_NODE_HOLDS_VALUE(cause) &&
			json_node_get_value_type(cause) == G_TYPE_STRING && g_strcmp0(json_node_get_string(cause), "list") == 0;

		for (r = 0; r < axis_db_result_get_n_rows(rows); r++) {
			gint64 id = 0;
			gint64 year = 0;
			gint64 title = 0;
			g_autofree gchar *label = NULL;

			axis_db_result_get_int64(rows, r, 0, &id);
			if (id != movie)
				continue;
			axis_db_result_get_int64(rows, r, 2, &year);
			axis_db_result_get_int64(rows, r, 3, &title);
			label = axis_movies_title_label(axis_db_result_get_text(rows, r, 1), (gint)year);
			item->words = g_strdup_printf("%s %s", list ? "Manual search of" : "Searching for", label);
			item->href = g_strdup_printf("/title/%" G_GINT64_FORMAT "#film", title);
		}
	}
}

/* ── A manual search (web/axis-web-manual.c) ────────────────────── */

static const AxisWebManualLevel manual_levels[] = {
	{ "movie", FALSE },
	{ NULL, FALSE }
};

static gboolean
manual_hold(gpointer data)
{
	AxisMovies *self = data;

	if (self->stopping || self->pending >= AXIS_MOVIES_MAX_PENDING)
		return FALSE;
	axis_movies_ref(self);
	self->pending++;
	return TRUE;
}

static void
manual_release(gpointer data)
{
	AxisMovies *self = data;

	self->pending--;
	axis_movies_unref(self);
}

static guint
manual_status(const GError *error)
{
	if (error->domain != AXIS_MOVIES_ERROR)
		return 0;
	switch (error->code) {
	case AXIS_MOVIES_ERROR_NOT_FOUND:
		return 404;
	case AXIS_MOVIES_ERROR_INVALID:
		return 400;
	case AXIS_MOVIES_ERROR_FAILED:
		return 502;
	default:
		return 409;
	}
}

static gboolean
manual_read(
	gpointer                   data,
	const AxisWebManualTarget *target,
	AxisWebManualRead         *read,
	GError                   **error
){
	AxisMovies *self = data;
	g_autoptr(AxisMovieFacts) facts = NULL;

	if (!axis_movies_get(self, target->owner_id, &facts, error))
		return FALSE;
	read->title = axis_movies_title_label(facts->title, facts->year);
	read->what = g_strdup("the movie");
	read->back = g_strdup_printf("/title/%" G_GINT64_FORMAT, facts->title_id);
	read->automatic = g_strdup_printf("/title/%" G_GINT64_FORMAT "#releases", facts->title_id);
	read->blocklist = g_strdup_printf("/title/%" G_GINT64_FORMAT "#blocklist", facts->title_id);
	if (!axis_movies_get_manual(self, target->owner_id, &read->search, &read->candidates, &read->running, error))
		return FALSE;
	/* Where the one in hand stands. */
	{
		g_autoptr(AxisMoviesWork) work = NULL;

		if (!axis_movies_get_work(self, target->owner_id, 0, &work, error))
			return FALSE;
		if (work->list != NULL)
			read->state = axis_web_activity_state(work->list, work->now_us);
	}
	return TRUE;
}

static gboolean
manual_search(
	gpointer                   data,
	const AxisWebManualTarget *target,
	const gchar               *user,
	gboolean                  *created,
	GError                   **error
){
	AxisMoviesAsked asked = AXIS_MOVIES_ASKED_IN_HAND;

	/* Nothing new while one is in hand or ended a moment ago. */
	*created = FALSE;
	if (!axis_movies_ask_search(data, target->owner_id, TRUE, user, &asked, error))
		return FALSE;
	*created = asked == AXIS_MOVIES_ASKED_QUEUED;
	return TRUE;
}

static gboolean
manual_preview(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	AxisAcquireHandPreview   **preview,
	GError                   **error
){
	return axis_movies_hand_preview(data, target->owner_id, candidate_id, preview, error);
}

static gboolean
manual_grab(
	gpointer                    data,
	const AxisWebManualTarget  *target,
	gint64                      candidate_id,
	const AxisAcquireHandTicks *ticks,
	const gchar                *user,
	AxisAcquireHandGrabbed     *grabbed,
	AxisAcquireGrabResult      *result,
	GError                    **error
){
	return axis_movies_grab_by_hand(data, target->owner_id, candidate_id, ticks, user, grabbed, result, error);
}

static gboolean
manual_force_preview(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	AxisAcquireHandPreview   **preview,
	GError                   **error
){
	return axis_movies_force_preview(data, target->owner_id, candidate_id, preview, error);
}

static gboolean
manual_force_grab(
	gpointer                     data,
	const AxisWebManualTarget   *target,
	gint64                       candidate_id,
	const AxisAcquireForceTicks *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	GError                     **error
){
	return axis_movies_grab_by_force(data, target->owner_id, candidate_id, ticks, user, grabbed, result, error);
}

/* Block this release (add-block-release). */
static gboolean
manual_block(
	gpointer                   data,
	const AxisWebManualTarget *target,
	gint64                     candidate_id,
	const gchar               *user,
	GError                   **error
){
	return axis_movies_block_candidate(data, target->owner_id, candidate_id, user, error);
}

static const AxisWebManualInfo manual_info = {
	"/films",
	"movie",
	manual_levels,
	manual_hold,
	manual_release,
	manual_status,
	manual_read,
	manual_search,
	manual_preview,
	manual_grab,
	manual_force_preview,
	manual_force_grab,
	manual_block
};

/* ── Block and search again (web/axis-web-block.c) ──────────────── */

static gboolean
block_find(
	gpointer           data,
	gint64             grab_id,
	gint64             download_id,
	gint64             import_id,
	AxisWebBlockRead  *read,
	GError           **error
){
	g_autoptr(AxisMovieFacts) facts = NULL;

	if (!axis_movies_block_find(data, grab_id, download_id, import_id, &read->grab, &facts, error))
		return FALSE;
	read->title = axis_movies_title_label(facts->title, facts->year);
	read->back = g_strdup_printf("/title/%" G_GINT64_FORMAT, facts->title_id);
	read->blocklist = g_strdup_printf("/title/%" G_GINT64_FORMAT "#blocklist", facts->title_id);
	return TRUE;
}

static gboolean
block_grab(
	gpointer      data,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
){
	return axis_movies_block_grab(data, owner_id, grab_id, user, cancelled, error);
}

static const AxisWebBlockInfo block_info = {
	"/films",
	"movie",
	manual_hold,
	manual_release,
	manual_status,
	block_find,
	block_grab
};

/* ── Kept by hand ───────────────────────────────────────────────── */

typedef struct
{
	AxisMovies   *self;
	HtmxRequest  *request;
	HtmxResponse *deferred;
	gint64        movie_id;
	gint64        file_id;
	gboolean      kept;
	gchar        *user;
	gint64        title_id;
	GError       *error;
} KeepWork;

static void
keep_thread(
	GTask        *task,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	KeepWork *work = task_data;
	g_autoptr(AxisMovieFacts) facts = NULL;

	if (axis_movies_set_kept(work->self, work->movie_id, work->file_id, work->kept, work->user, &work->error) &&
	    axis_movies_get(work->self, work->movie_id, &facts, &work->error))
		work->title_id = facts->title_id;
	g_task_return_boolean(task, TRUE);
}

static void
keep_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	KeepWork *work = user_data;
	HtmxResponse *response;

	if (work->error != NULL) {
		guint status = manual_status(work->error);
		g_autofree gchar *message = g_strdup_printf("%c%s.", g_ascii_toupper(work->error->message[0]),
			work->error->message + 1);

		response = status == 0 ? axis_web_internal_error(work->request, work->error) :
			axis_web_error_respond(work->request, status, "Not changed", message);
	} else {
		g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, work->title_id);

		axis_web_storage_audit(work->request, "movie %" G_GINT64_FORMAT " file %" G_GINT64_FORMAT " %s",
			work->movie_id, work->file_id, work->kept ? "kept by hand" : "no longer kept by hand");
		response = axis_web_place_redirect(work->request, page, work->kept ? "kept" : "let_go", "files");
	}
	htmx_response_complete(work->deferred, response);
	g_object_unref(response);
	manual_release(work->self);
	g_object_unref(work->request);
	g_object_unref(work->deferred);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

/* POST /films/:id/links/keep (file, keep=yes|no): an administrator sets or
 * clears "kept by hand" on one file of the movie. */
static HtmxResponse *
handle_keep(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisMovies *self = user_data;
	const gchar *file = htmx_request_get_form_value(request, "file");
	const gchar *keep = htmx_request_get_form_value(request, "keep");
	guint64 file_id = 0;
	HtmxResponse *returned;
	KeepWork *work;
	GTask *task;

	if (axis_web_storage_param_id(params) <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie.");
	if (file == NULL || !g_ascii_string_to_unsigned(file, 10, 1, G_MAXINT64, &file_id, NULL) ||
	    (g_strcmp0(keep, "yes") != 0 && g_strcmp0(keep, "no") != 0))
		return axis_web_error_respond(request, 400, "Not changed", "Say which file, and whether it is kept by hand.");
	if (!manual_hold(self)) {
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	work = g_new0(KeepWork, 1);
	work->self = self;
	work->request = g_object_ref(request);
	work->movie_id = axis_web_storage_param_id(params);
	work->file_id = (gint64)file_id;
	work->kept = g_strcmp0(keep, "yes") == 0;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->deferred = htmx_response_new_deferred();
	returned = g_object_ref(work->deferred);
	task = g_task_new(NULL, NULL, keep_finished, work);
	g_task_set_task_data(task, work, NULL);
	g_task_run_in_thread(task, keep_thread);
	g_object_unref(task);
	return returned;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The jobs whose end can change which movies need attention: a search (its
 * outcome, or the grab it made) and an import (which ends "its import needs a
 * person", and what follows it).  Not movies.follow or movies.schedule, which
 * run every minute or so whatever happened: what they find becomes a search,
 * and that search's end is told.
 */
static const gchar *const attention_kinds[] = { AXIS_MOVIES_SEARCH_KIND, AXIS_IMPORT_KIND, NULL };

/*
 * The movies' notice (add-attention-notices): how many movies Axis manages
 * need attention for a reason of their own (axis_acquire_attention_is_own()),
 * with the most common reason, as one line that names no title.  One
 * statement counts them by reason and loads no film
 * (axis_movies_count_attention(): the state as the Wanted page's filter works
 * it out, in the database), under the reading's deadline.  It is still the
 * heaviest statement of the board (it derives every managed movie's state),
 * so it is made only when axis_attention_titles_due() says so: after a
 * search or an import ended, never sooner than five minutes after the count
 * before, and every ten minutes when nothing ended; what was last counted is
 * given in between.  Only a person ends these, so there is no wait.
 * "newest" is the highest id among the movies counted.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisMovies *self = user_data;
	AxisAttentionTitles *titles = &self->attention;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;

	if (axis_attention_titles_due(titles, reading, attention_kinds)) {
		guint64 reasons[AXIS_MOVIES_ATTENTION_KINDS];
		gint64 highest[AXIS_MOVIES_ATTENTION_KINDS];
		guint64 count = 0;
		gint64 newest = 0;
		guint top = AXIS_ACQUIRE_ATTENTION_NONE;
		guint n_reasons = 0;
		guint i;

		if (!axis_movies_count_attention(self, reading->deadline_ms, reasons, highest, error))
			return FALSE;
		reasons[AXIS_ACQUIRE_ATTENTION_NONE] = 0;
		for (i = AXIS_ACQUIRE_ATTENTION_NONE + 1; i < G_N_ELEMENTS(reasons); i++) {
			/* A consequence of what has its own notice is not counted. */
			if (!axis_acquire_attention_is_own((AxisAcquireAttention)i) || reasons[i] == 0)
				continue;
			count += reasons[i];
			newest = MAX(newest, highest[i]);
			n_reasons++;
			if (reasons[i] > reasons[top])
				top = i;
		}
		titles->count = count;
		titles->newest = newest;
		titles->top = top;
		titles->n_reasons = n_reasons;
		titles->read_us = reading->now_us;
		titles->known = TRUE;
		titles->dirty = FALSE;
	}

	if (titles->count == 0)
		return TRUE;
	if (titles->count == 1)
		text = g_strdup_printf("1 movie needs attention because %s.",
			axis_acquire_attention_because((AxisAcquireAttention)titles->top));
	else
		text = g_strdup_printf("%" G_GUINT64_FORMAT " movies need attention, %s because %s.", titles->count,
			titles->n_reasons > 1 ? "most often" : "each",
			axis_acquire_attention_because((AxisAcquireAttention)titles->top));
	item = axis_attention_item_new("movies.attention", "", AXIS_ATTENTION_WAITING, titles->count, titles->newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/films/wanted?state=attention", "Open Movies, Wanted");
	g_ptr_array_add(items, item);
	return TRUE;
}

/* ── The routes ─────────────────────────────────────────────────── */

void
axis_movies_web_register(
	AxisMovies *self,
	AxisWeb    *web
){
	axis_web_add_title_actions(web, actions_collect, actions_render, action_figures_free, self);
	axis_web_add_dashboard_part(web, dash_collect, dash_render, dash_free, self);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_COMING_IN);
	axis_web_add_attention_source(web, "the movies", attention_source, self);
	/* Before any route with a parameter in its place: the first route that
	 * matches answers. */
	axis_web_block_register(web, &block_info, self);
	axis_movies_web_register_pages(self, web);
	axis_movies_web_register_links(self, web);
	axis_movies_web_register_file_link(self, web);
	axis_web_manual_register(web, &manual_info, self);
	axis_web_add_activity_words(web, AXIS_MOVIES_SEARCH_KIND, activity_words, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films", AXIS_WEB_ROUTE_ADMIN, handle_add, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/defaults", AXIS_WEB_ROUTE_ADMIN, handle_defaults, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/search", AXIS_WEB_ROUTE_ADMIN, handle_search, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/grab", AXIS_WEB_ROUTE_ADMIN, handle_grab, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/give-up", AXIS_WEB_ROUTE_ADMIN, handle_give_up, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/blocklist/clear", AXIS_WEB_ROUTE_ADMIN, handle_clear,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/edit", AXIS_WEB_ROUTE_ADMIN, handle_edit, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/remove", AXIS_WEB_ROUTE_ADMIN, handle_remove, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/start-searching", AXIS_WEB_ROUTE_ADMIN, handle_start, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/films/:id/links/keep", AXIS_WEB_ROUTE_ADMIN, handle_keep, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/films/:id/state", AXIS_WEB_ROUTE_DEFAULT, handle_state, self);
}
