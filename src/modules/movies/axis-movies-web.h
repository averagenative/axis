/*
 * axis-movies-web.h - shared by the files of the movies module's pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-movies-web.c       routes, the work off the main loop, the title
 *                           actions, the dashboard, the actions (POST)
 *   axis-movies-web-view.c  the pieces and the pages: the Films grid,
 *                           Wanted, History, a film's state, its releases,
 *                           blocklist, history and options
 *   axis-movies-web-links.c a movie's links page, "Files and links"
 *
 * Any logged-in user sees every page; every action is an administrator's
 * POST (AXIS_WEB_ROUTE_ADMIN, with the CSRF token), and the pages offer no
 * form to anyone else.
 */

#ifndef AXIS_MOVIES_WEB_H
#define AXIS_MOVIES_WEB_H

#include <htmx-glib.h>

#include "modules/movies/axis-movies-private.h"
#include "web/axis-web.h"
#include "web/axis-web-notice.h"

G_BEGIN_DECLS

/* At most this many releases of a search are listed on a film's page. */
#define AXIS_MOVIES_WEB_RELEASES_SHOWN  150

/* Events on a film's page, and per page of History. */
#define AXIS_MOVIES_WEB_EVENTS_SHOWN    30
#define AXIS_MOVIES_WEB_HISTORY_PAGE    50

typedef void (*AxisMoviesWorkFunc)(AxisMovies *self, gpointer data);

typedef HtmxResponse *(*AxisMoviesDoneFunc)(
	AxisMovies  *self,
	HtmxRequest *request,
	gpointer     data
);

/* Runs @work on a worker thread, then answers @request with @done on the
 * main loop (or at once a 503 when too much is waiting or the pages are
 * stopping).  @free_data is called on the main loop in every case. */
HtmxResponse *
axis_movies_web_dispatch(
	AxisMovies         *self,
	HtmxRequest        *request,
	AxisMoviesWorkFunc  work,
	AxisMoviesDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
);

/* One film as a page shows it: its facts, its state and why, and its
 * poster's stored image. */
typedef struct
{
	AxisMovieFacts       *facts;
	AxisMovieState        state;
	gchar                *reason;
	gchar                *poster;	/* content hash, or %NULL */
	AxisAcquireAttention  kind;	/* why it needs attention, NONE else */
	AxisMoviesWork       *work;	/* (nullable) its searches in hand and lately
					 * ended: on its own page only */
} AxisMoviesShown;

void
axis_movies_shown_free(AxisMoviesShown *shown);

/* @facts is taken. */
AxisMoviesShown *
axis_movies_shown_new(
	AxisMovieFacts       *facts,
	const AxisMovieRules *rules
);

/* A choice for the add and edit forms: a profile, a root. */
typedef struct
{
	gint64   id;
	gchar   *label;
	gchar   *slug;		/* profiles */
	gboolean usable;	/* a root's volume is online; a profile validates */
	guint64  free_bytes;	/* roots */
	const gchar *why_not;	/* roots: why a new movie cannot go there now
				 * (axis_volume_state_unwritable_why()), or NULL */
} AxisMoviesChoice;

void
axis_movies_choice_free(AxisMoviesChoice *choice);

/* The profiles and the film roots (online with most free space first). */
typedef struct
{
	GPtrArray *profiles;	/* AxisMoviesChoice */
	GPtrArray *roots;	/* AxisMoviesChoice */
	gchar     *default_slug;
	AxisMovieAvailability availability;
} AxisMoviesChoices;

void
axis_movies_choices_clear(AxisMoviesChoices *choices);

gboolean
axis_movies_web_read_choices(
	AxisMovies        *self,
	AxisMoviesChoices *choices,
	GError           **error
);

/* ── Pieces ─────────────────────────────────────────────────────── */

/* The small mark a poster carries: one size and one place on every card. */
HtmxNode *
axis_movies_web_mark(AxisMovieState state);

/* A film's state as its page shows it: the mark, the reason, the progress. */
HtmxNode *
axis_movies_web_state_block(const AxisMoviesShown *shown);

/* The add form of a film not managed yet (administrators). */
HtmxNode *
axis_movies_web_add_form(
	HtmxRequest             *request,
	gint64                   title_id,
	const AxisMoviesChoices *choices
);

/* A Discover card's one-step add (administrators): a button that adds the
 * film with what the add form selects at first (the default profile, the
 * first usable root, the default availability, monitored, search now) and
 * comes back to the page, and a link to its title page for the choices.
 * Without a usable profile or root, only the link. */
HtmxNode *
axis_movies_web_quick_add(
	HtmxRequest             *request,
	gint64                   title_id,
	const AxisMoviesChoices *choices
);

/* The person's kind in their add defaults (axis_web_request_add_defaults()). */
#define AXIS_MOVIES_WEB_DEFAULTS_KIND "movies"

/* The id of the bar above a list (also its place for the notice). */
#define AXIS_MOVIES_WEB_DEFAULTS_BAR "add-defaults-movies"

/* The profile and root the person's Adds from a list use: their own choice
 * where it can still be used, else the installation's first. */
void
axis_movies_web_defaults(
	HtmxRequest              *request,
	const AxisMoviesChoices  *choices,
	const AxisMoviesChoice  **profile,
	const AxisMoviesChoice  **root
);

/* The bar above a list of titles (administrators): "New movies go to
 * [folder] with [profile]", a form that stores the choice for this person
 * (POST /films/defaults); with htmx a change posts by itself and the answer
 * (axis_movies_web_defaults_answer()) swaps the bar and the cards' Adds.
 * @page: (nullable): the list's address it comes back to; %NULL for the
 * request's own. */
HtmxNode *
axis_movies_web_defaults_bar(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices,
	const gchar             *page
);

/* The answer to an htmx post of the bar: the bar again, every card's
 * profile and root inputs and the words of its Options, out of band. */
HtmxResponse *
axis_movies_web_defaults_answer(
	HtmxRequest             *request,
	const AxisMoviesChoices *choices
);

/* The form that has Axis manage a movie on disk it does not manage yet
 * (administrators): a profile, a destination, monitoring; it searches
 * nothing. */
HtmxNode *
axis_movies_web_manage_form(
	HtmxRequest             *request,
	const AxisMovieFacts    *facts,
	const AxisMoviesChoices *choices
);

/* The administrator's buttons for a managed film (<div id="film-actions-
 * <id>">): Search now and Manual search say so, and cannot be pressed, while
 * one of theirs is in hand or ended a moment ago. */
HtmxNode *
axis_movies_web_actions(
	HtmxRequest           *request,
	const AxisMoviesShown *shown
);

/* A film's line ("activity-film-<id>"): its search in hand, its download,
 * else what its last search found a while ago; empty and hidden when
 * nothing (or when its work was not read). */
HtmxNode *
axis_movies_web_film_line(const AxisMoviesShown *shown);

/* The notice of a click the work in hand answered ("already_searching",
 * "searched_recently"), in words of where that work stands now; %NULL for
 * another word or without the work. */
gchar *
axis_movies_web_answer(
	const gchar           *done,
	const AxisMoviesShown *shown
);

/* The topics a managed film's page follows (its searches, its title's
 * downloads), into @topics (three: the last %NULL); free the two. */
void
axis_movies_web_topics(
	const AxisMovieFacts *facts,
	gchar               **topics
);

/**
 * AxisMoviesTried:
 * @candidates: (element-type AxisMovieCandidate): the releases the profile
 *   rejected that a person may take (borrowed from the detail)
 * @profile_ids: (element-type gint64): the profiles tried
 * @tries: (nullable) (element-type AxisAcquireProfileTry): one per profile
 *
 * What each usable profile would make of what the last search found, for a
 * film whose releases the profile rejected.
 */
typedef struct
{
	GPtrArray *candidates;
	GArray    *profile_ids;
	GPtrArray *tries;
} AxisMoviesTried;

void
axis_movies_tried_clear(AxisMoviesTried *tried);

/* Reads @tried for a film whose last search's releases the profile rejected
 * (nothing for any other). */
void
axis_movies_web_try(
	AxisMovies              *self,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	AxisMoviesTried         *tried
);

/* Next to a film's state, when it needs attention or its last search found
 * nothing: what puts it right, by why (administrators get the actions,
 * anyone else the words); %NULL when there is nothing to do. */
HtmxNode *
axis_movies_web_resolve(
	HtmxRequest             *request,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	const AxisMoviesTried   *tried
);

/* The notice for ?done=<key> with what only the film's page knows (a new
 * profile's name), or %NULL (transfer full). */
gchar *
axis_movies_web_done_text(
	const gchar          *key,
	const AxisMovieFacts *facts
);

/* A film's cards: its files, the download, the releases, the blocklist, the
 * options and the history; of a movie on disk Axis does not manage, its
 * files and what its title kept of a film removed before.  @notice:
 * (nullable) the page's, drawn in the card it came back to. */
HtmxNode *
axis_movies_web_detail(
	HtmxRequest             *request,
	const AxisMoviesShown   *shown,
	const AxisMovieDetail   *detail,
	const AxisMoviesChoices *choices,
	AxisWebNotice           *notice
);

/* The notice for ?done=<key> on a film's page, or %NULL. */
const gchar *
axis_movies_web_done_message(const gchar *key);

/* "3 days", "5 hours" (an age, for a release). */
gchar *
axis_movies_web_age(gint64 us);

/* ── Pages ──────────────────────────────────────────────────────── */

void
axis_movies_web_register_pages(
	AxisMovies *self,
	AxisWeb    *web
);

/* A movie's links page, "Files and links" (axis-movies-web-links.c). */
void
axis_movies_web_register_links(
	AxisMovies *self,
	AxisWeb    *web
);

/* GET /library/file/:id/link: from a file to the links page of the movie or
 * series it belongs to (axis-movies-web-file-link.c). */
void
axis_movies_web_register_file_link(
	AxisMovies *self,
	AxisWeb    *web
);

G_END_DECLS

#endif /* AXIS_MOVIES_WEB_H */
